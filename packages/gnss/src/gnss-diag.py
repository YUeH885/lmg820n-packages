#!/usr/bin/python3
"""Keep the MPSS DIAG handshake alive and configure nav_pps at startup."""

import ctypes
import errno
import logging
import os
import select
import signal
import socket
import struct
import time

AF_QRTR = 42
DIAG_SERVICE = 4097
QRTR_CONTROL_PORT = 0xfffffffe
FEATURE_MASK = 0x2051
FEATURE_REQUIRED = (1 << 4) | (1 << 6) | (1 << 13)
LIBC = ctypes.CDLL(None, use_errno=True)
LOG = logging.getLogger("gnss-diag")


def control_messages(raw):
    offset = 0
    while offset < len(raw):
        if len(raw) - offset < 8:
            raise ValueError("Truncated DIAG control header")
        code, length = struct.unpack_from("<II", raw, offset)
        end = offset + 8 + length
        if end > len(raw):
            raise ValueError("Truncated DIAG control payload")
        yield code, raw[offset + 8:end]
        offset = end


def feature_response(payload):
    if len(payload) < 4:
        raise ValueError("Truncated DIAG feature mask")
    length, = struct.unpack_from("<I", payload)
    if len(payload) != 4 + length:
        raise ValueError("Invalid DIAG feature mask length")
    features = int.from_bytes(payload[4:], "little")
    if features & FEATURE_REQUIRED != FEATURE_REQUIRED:
        raise ValueError("MPSS does not support the required DIAG transport features")
    return struct.pack("<III", 8, 7, 3) + FEATURE_MASK.to_bytes(3, "little")


def diag_frames(raw):
    offset = 0
    while offset < len(raw):
        if len(raw) - offset < 5 or raw[offset:offset + 2] != b"\x7e\x01":
            raise ValueError("Invalid DIAG non-HDLC frame")
        length, = struct.unpack_from("<H", raw, offset + 2)
        end = offset + 4 + length
        if end >= len(raw) or raw[end] != 0x7e:
            raise ValueError("Truncated DIAG non-HDLC frame")
        yield raw[offset + 4:end]
        offset = end + 1


def gps_commands(payload):
    if len(payload) < 12:
        raise ValueError("Truncated DIAG command registration")
    version, command, subsystem, count, _ = struct.unpack_from("<IHHHH", payload)
    if version != 1 or len(payload) != 12 + count * 8:
        raise ValueError("Invalid DIAG command registration")
    if command != 0xff or subsystem != 13:
        return set()
    commands = set()
    for offset in range(12, len(payload), 8):
        first, last, _ = struct.unpack_from("<HHI", payload, offset)
        commands.update(value for value in (8, 100) if first <= value <= last)
    return commands


def drsync_packet():
    packet = bytearray(36)
    struct.pack_into("<BBH", packet, 0, 0x4b, 13, 100)
    packet[4] = 203
    struct.pack_into("<II", packet, 8, 0, 1)
    packet[16] = 1
    struct.pack_into("<iii", packet, 24, 1, 1, 0)
    return bytes(packet)


def validate_response(stage, response):
    if stage == "version":
        return response == b"\x1c\x08\x00"
    if stage == "gps_init":
        return response == b"\x4b\x0d\x08\x00"
    if stage == "drsync":
        return (len(response) == 26 and response[:4] == b"\x4b\x0d\x64\x00"
                and struct.unpack_from("<I", response, 6)[0] == 2
                and struct.unpack_from("<I", response, 10)[0] == 5
                and response[14] == 1)
    raise ValueError("Unknown DIAG request stage")


class Address(ctypes.Structure):
    _fields_ = [("family", ctypes.c_ushort), ("node", ctypes.c_uint32),
                ("port", ctypes.c_uint32)]


def send_timeout(signum, frame):
    raise TimeoutError("QRTR send blocked by flow control")


class Endpoint:
    def __init__(self):
        fd = LIBC.socket(AF_QRTR, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC, 0)
        if fd < 0:
            raise OSError(ctypes.get_errno(), "Cannot create QRTR socket")
        self.sock = socket.socket(fileno=fd)
        try:
            address = Address(AF_QRTR, 1, 0)
            if LIBC.bind(fd, ctypes.byref(address), ctypes.sizeof(address)) < 0:
                raise OSError(ctypes.get_errno(), "Cannot bind QRTR socket")
            size = ctypes.c_uint(ctypes.sizeof(address))
            if LIBC.getsockname(fd, ctypes.byref(address), ctypes.byref(size)) < 0:
                raise OSError(ctypes.get_errno(), "Cannot get QRTR address")
            self.node, self.port = address.node, address.port
            self.sock.setblocking(False)
        except BaseException:
            self.sock.close()
            raise

    def send(self, raw, peer):
        address = Address(AF_QRTR, *peer)
        buffer = ctypes.create_string_buffer(raw)
        # QRTR flow control waits even on nonblocking sockets.
        signal.setitimer(signal.ITIMER_REAL, 3)
        try:
            size = LIBC.sendto(self.sock.fileno(), buffer, len(raw), 0,
                               ctypes.byref(address), ctypes.sizeof(address))
            if size < 0:
                raise OSError(ctypes.get_errno(), "QRTR send failed")
            if size != len(raw):
                raise OSError(errno.EIO, "Short QRTR send")
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)

    def receive(self):
        buffer = ctypes.create_string_buffer(65536)
        address = Address()
        size = ctypes.c_uint(ctypes.sizeof(address))
        length = LIBC.recvfrom(self.sock.fileno(), buffer, len(buffer), 0,
                               ctypes.byref(address), ctypes.byref(size))
        if length < 0:
            raise OSError(ctypes.get_errno(), "QRTR receive failed")
        return buffer.raw[:length], (address.node, address.port)

    def advertise(self, instance, command=4):
        self.send(struct.pack("<5I", command, DIAG_SERVICE, instance,
                              self.node, self.port),
                  (self.node, QRTR_CONTROL_PORT))


class Session:
    def __init__(self, endpoints):
        self.endpoints = endpoints
        self.reset()

    def reset(self):
        self.command_peer = None
        self.control_peer = None
        self.features_sent = False
        self.gps_commands = set()
        self.stage = "waiting"
        self.deadline = None

    def discovery(self, raw):
        if len(raw) != 20:
            raise ValueError("Invalid QRTR discovery packet")
        command, service, instance, node, port = struct.unpack("<5I", raw)
        if service != DIAG_SERVICE or instance != 1 or node != 0:
            return
        peer = (node, port)
        if command == 5 and peer == self.command_peer:
            LOG.info("MPSS DIAG command server disappeared")
            self.reset()
            notify_status("DIAG endpoints registered; waiting for MPSS")
        elif command == 4:
            if self.command_peer is not None and self.command_peer != peer:
                self.reset()
            self.command_peer = peer
            self.begin()

    def control(self, raw, peer):
        if peer[0] != 0:
            return
        for code, payload in control_messages(raw):
            if code == 1:
                self.gps_commands.update(gps_commands(payload))
                self.begin_gps()
            elif code == 8:
                reply = feature_response(payload)
                if self.control_peer is not None and self.control_peer != peer:
                    command_peer = self.command_peer
                    self.reset()
                    self.command_peer = command_peer
                self.endpoints["control"].send(reply, peer)
                self.control_peer = peer
                self.features_sent = True
                LOG.info("DIAG features acknowledged at %u:%u", *peer)
            elif code == 33:
                if len(payload) < 9 or payload[-1] != 0:
                    raise ValueError("Invalid DIAG ID request")
                version, = struct.unpack_from("<I", payload)
                if version != 1:
                    raise ValueError("Unsupported DIAG ID request version")
                name = payload[8:].split(b"\0", 1)[0] + b"\0"
                reply = struct.pack("<4I", 33, 8 + len(name), 1, 2) + name
                self.endpoints["control"].send(reply, peer)
        self.begin()

    def begin(self):
        if self.stage != "waiting" or not self.features_sent or not self.command_peer:
            return
        self.endpoints["control"].send(
            struct.pack("<11I", 3, 36, 1, 1, 1, 0, 0, 0, 0, 0, 0), self.control_peer)
        self.request("version", b"\x1c", 60)

    def begin_gps(self):
        if self.stage == "gps_wait" and self.gps_commands == {8, 100}:
            LOG.info("GPS DIAG commands 8 and 100 registered")
            self.request("gps_init", b"\x4b\x0d\x08\x00", 15)

    def request(self, stage, raw, timeout):
        self.endpoints["command"].send(raw, self.command_peer)
        self.stage = stage
        self.deadline = time.monotonic() + timeout

    def response(self, raw, peer):
        if peer != self.command_peer or self.stage in ("waiting", "gps_wait", "configured"):
            return
        for response in diag_frames(raw):
            if not validate_response(self.stage, response):
                raise ValueError(f"Invalid {self.stage} response: {response.hex()}")
            LOG.info("DIAG %s accepted: %s", self.stage, response.hex())
            if self.stage == "version":
                # MPSS publishes its transport before GPS command handlers.
                self.stage = "gps_wait"
                self.deadline = time.monotonic() + 15
                notify_status("DIAG handshake complete; waiting for GPS commands")
                self.begin_gps()
            elif self.stage == "gps_init":
                self.request("drsync", drsync_packet(), 15)
            else:
                self.stage = "configured"
                self.deadline = None
                LOG.info("DRSync configuration queued: GPIO60, period 1s, phase 0s")
                notify_status("DRSync configuration queued: GPIO60, period 1s")

    def check_timeout(self):
        if self.deadline is not None and time.monotonic() >= self.deadline:
            raise TimeoutError(f"DIAG {self.stage} response timed out")


def notify_status(status, ready=False):
    address = os.environ.get("NOTIFY_SOCKET")
    if address:
        if address.startswith("@"):
            address = "\0" + address[1:]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as notify:
            message = ("READY=1\n" if ready else "") + "STATUS=" + status
            notify.sendto(message.encode(), address)


def stop(signum, frame):
    raise KeyboardInterrupt


def main():
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    signal.signal(signal.SIGALRM, send_timeout)
    signal.signal(signal.SIGTERM, stop)
    endpoints = {}
    advertised = []
    session = None
    try:
        for name in ("lookup", "control", "data", "dci", "command"):
            endpoints[name] = Endpoint()
        lookup = endpoints["lookup"]
        lookup.send(struct.pack("<5I", 10, DIAG_SERVICE, 0, 0, 0),
                    (lookup.node, QRTR_CONTROL_PORT))
        for name, instance in (("control", 0), ("data", 2), ("dci", 4)):
            endpoints[name].advertise(instance)
            advertised.append((name, instance))
        session = Session(endpoints)
        notify_status("DIAG endpoints registered; waiting for MPSS", ready=True)
        LOG.info("DIAG endpoints registered; waiting for MPSS")
        by_socket = {endpoint.sock: name for name, endpoint in endpoints.items()}
        while True:
            timeout = (max(0, session.deadline - time.monotonic())
                       if session.deadline is not None else None)
            for sock in select.select(list(by_socket), [], [], timeout)[0]:
                name = by_socket[sock]
                raw, peer = endpoints[name].receive()
                if peer[1] == QRTR_CONTROL_PORT:
                    session.discovery(raw)
                elif name == "control":
                    session.control(raw, peer)
                elif name == "command":
                    session.response(raw, peer)
            session.check_timeout()
    except KeyboardInterrupt:
        LOG.info("Stopping DIAG session")
        return 0
    except (OSError, ValueError, TimeoutError) as error:
        LOG.error("DIAG session failed: %s", error)
        return 1
    finally:
        if session is not None and session.control_peer is not None:
            try:
                endpoints["control"].send(
                    struct.pack("<11I", 3, 36, 1, 0, 0, 0, 0, 0, 0, 0, 0),
                    session.control_peer)
            except (OSError, TimeoutError) as error:
                LOG.error("Cannot release DIAG real-time vote: %s", error)
        for name, instance in advertised:
            try:
                endpoints[name].advertise(instance, 5)
            except (OSError, TimeoutError) as error:
                LOG.error("Cannot unregister DIAG endpoint %s: %s", name, error)
        for endpoint in endpoints.values():
            endpoint.sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
