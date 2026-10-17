#!/usr/bin/python3
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "gnss_diag", Path(__file__).parent.parent / "src" / "gnss-diag.py")
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)


def frame(raw):
    return b"\x7e\x01" + struct.pack("<H", len(raw)) + raw + b"\x7e"


FEATURES = struct.pack("<III", 8, 7, 3) + bytes.fromhex("f7fe03")
ACK = bytes.fromhex("4b0d6400dead0200000005000000010000000000000000000000")
GPS_COMMANDS = bytes.fromhex("01000000ff000d0003000000640064000000000008000800000000000c000c0000000000")


def registration(payload=GPS_COMMANDS):
    return struct.pack("<II", 1, len(payload)) + payload


class FakeEndpoint:
    def __init__(self):
        self.sent = []

    def send(self, raw, peer):
        self.sent.append((raw, peer))


class DiagTests(unittest.TestCase):
    def setUp(self):
        self.endpoints = {name: FakeEndpoint() for name in ("control", "command")}
        self.session = diag.Session(self.endpoints)
        self.peer = (0, 112)
        self.control_peer = (0, 111)

    def discover(self, command=4, port=112):
        self.session.discovery(struct.pack("<5I", command, 4097, 1, 0, port))

    def handshake(self):
        self.discover()
        self.session.control(FEATURES, self.control_peer)

    def test_handshake_before_commands(self):
        self.discover()
        self.assertFalse(self.endpoints["command"].sent)
        self.session.control(FEATURES, self.control_peer)
        self.assertEqual(self.endpoints["control"].sent[0],
                         (bytes.fromhex("080000000700000003000000512000"), self.control_peer))
        self.assertEqual(self.endpoints["command"].sent, [(b"\x1c", self.peer)])

    def test_features_before_discovery(self):
        self.session.control(FEATURES, self.control_peer)
        self.assertFalse(self.endpoints["command"].sent)
        self.discover()
        self.assertEqual(self.session.stage, "version")

    def test_enable_after_init(self):
        self.handshake()
        self.session.control(registration(), self.control_peer)
        self.session.response(frame(b"\x1c\x08\x00"), self.peer)
        self.assertEqual(self.session.stage, "gps_init")
        self.session.response(frame(b"\x4b\x0d\x08\x00"), self.peer)
        self.assertEqual(self.session.stage, "drsync")
        self.assertEqual(self.endpoints["command"].sent[-1][0].hex(),
                         "4b0d6400cb00000000000000010000000100000000000000010000000100000000000000")
        self.session.response(frame(ACK), self.peer)
        self.assertEqual(self.session.stage, "configured")
        self.assertIsNone(self.session.deadline)

    def test_rejected_init_stops_enable(self):
        self.handshake()
        self.session.control(registration(), self.control_peer)
        self.session.response(frame(b"\x1c\x08\x00"), self.peer)
        with self.assertRaises(ValueError):
            self.session.response(frame(b"\x13\x4b\x0d\x08\x00"), self.peer)
        self.assertEqual(len(self.endpoints["command"].sent), 2)

    def test_gps_init_waits_for_command_registration(self):
        self.handshake()
        self.session.response(frame(b"\x1c\x08\x00"), self.peer)
        self.assertEqual(self.session.stage, "gps_wait")
        self.assertEqual(self.endpoints["command"].sent, [(b"\x1c", self.peer)])
        self.session.control(registration(), self.control_peer)
        self.assertEqual(self.session.stage, "gps_init")
        self.assertEqual(self.endpoints["command"].sent[-1][0], b"\x4b\x0d\x08\x00")

    def test_both_gps_commands_are_required(self):
        self.handshake()
        self.session.response(frame(b"\x1c\x08\x00"), self.peer)
        for command in (8, 100):
            payload = struct.pack("<IHHIHHI", 1, 0xff, 13, 1, command, command, 0)
            self.session.control(registration(payload), self.control_peer)
            self.assertEqual(self.session.stage, "gps_wait" if command == 8 else "gps_init")

    def test_registration_bounds_and_subsystem(self):
        self.assertEqual(diag.gps_commands(GPS_COMMANDS), {8, 100})
        unrelated = bytearray(GPS_COMMANDS)
        unrelated[6] = 12
        self.assertEqual(diag.gps_commands(unrelated), set())
        for payload in (b"", GPS_COMMANDS[:-1], GPS_COMMANDS + b"a"):
            with self.assertRaises(ValueError):
                diag.gps_commands(payload)

    def test_response_requires_queue_success(self):
        self.assertTrue(diag.validate_response("drsync", ACK))
        for offset in (6, 10, 14):
            rejected = bytearray(ACK)
            rejected[offset] = 0
            self.assertFalse(diag.validate_response("drsync", rejected))
        self.assertFalse(diag.validate_response("drsync", ACK[:14]))

    def test_modem_restart_requires_new_handshake(self):
        self.handshake()
        self.session.control(registration(), self.control_peer)
        self.discover(command=5)
        self.discover(port=200)
        self.assertEqual(self.session.stage, "waiting")
        self.assertFalse(self.session.gps_commands)
        self.session.control(FEATURES, (0, 199))
        self.assertEqual(self.endpoints["command"].sent[-1], (b"\x1c", (0, 200)))

    def test_duplicate_features_are_acknowledged(self):
        self.handshake()
        self.session.control(FEATURES, self.control_peer)
        self.assertEqual(len(self.endpoints["command"].sent), 1)
        self.assertEqual(self.endpoints["control"].sent[-1][0][:4], struct.pack("<I", 8))

    def test_timeout_is_bounded(self):
        self.handshake()
        with patch.object(diag.time, "monotonic", return_value=self.session.deadline):
            with self.assertRaises(TimeoutError):
                self.session.check_timeout()

    def test_unrelated_peer_is_ignored(self):
        self.handshake()
        self.session.response(frame(b"\x13"), (0, 300))
        self.assertEqual(self.session.stage, "version")

    def test_frames_and_control_bounds(self):
        self.assertEqual(list(diag.diag_frames(frame(b"a") + frame(b"bc"))), [b"a", b"bc"])
        for raw in (b"", FEATURES[:-1], FEATURES + b"a"):
            if raw:
                with self.assertRaises(ValueError):
                    list(diag.control_messages(raw))
        for raw in (b"\x7e", frame(b"a")[:-1], b"\x7e\x01\xff\xff\x7e"):
            with self.assertRaises(ValueError):
                list(diag.diag_frames(raw))
        with self.assertRaises(ValueError):
            diag.feature_response(struct.pack("<I", 3) + b"\x01\x00\x00")


if __name__ == "__main__":
    unittest.main()
