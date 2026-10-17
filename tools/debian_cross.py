"""Cross-build Debian arm64 packages using an amd64 multiarch build root."""

import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from package_utils import digest, run_cmd

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build/debian-cross-arm64"
BUILD_PACKAGES = (
    "crossbuild-essential-arm64", "debhelper", "devscripts", "equivs",
    "meson", "ninja-build", "pkgconf", "qemu-user", "ca-certificates",
    "dpkg-dev", "patch", "quilt",
)


def prepare_builder(log):
    build_arch = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
    if build_arch != "amd64":
        raise RuntimeError("Expected an amd64 build host")
    mirror = os.environ.get("MIRROR", "https://deb.debian.org/debian")
    key = hashlib.sha256(json.dumps(["sid-src", build_arch, "arm64", mirror, BUILD_PACKAGES]).encode()).hexdigest()[:16]
    builder = WORK / f"rootfs-{key}"
    if builder.exists():
        # Ensure deb-src is enabled in existing builder
        sources_list = builder / "etc/apt/sources.list"
        if sources_list.exists() and "deb-src" not in sources_list.read_text():
            run_cmd(["sudo", "bash", "-c", f'echo "deb-src {mirror} sid main contrib non-free non-free-firmware" >> {sources_list}'])
        return builder

    WORK.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=WORK, prefix=".bootstrap-") as temporary:
        target = Path(temporary) / "rootfs"
        try:
            print(f"Preparing local multiarch build root: {builder}", flush=True)
            sources = (
                f"deb {mirror} sid main contrib non-free non-free-firmware\n"
                f"deb-src {mirror} sid main contrib non-free non-free-firmware\n"
            )
            with tempfile.NamedTemporaryFile("w", delete=False) as s_file:
                s_file.write(sources)
                sources_tmp = s_file.name

            run_cmd([
                "sudo", "mmdebstrap", "--mode=root", "--format=directory", "--variant=minbase",
                "--architectures=amd64,arm64", "--include=" + ",".join(BUILD_PACKAGES),
                '--aptopt=APT::Install-Recommends "false"',
                "sid", target, sources_tmp,
            ], stdout=log, stderr=subprocess.STDOUT)
            os.unlink(sources_tmp)
            run_cmd(["sudo", "mv", target, builder])
        finally:
            if target.exists():
                run_cmd(["sudo", "rm", "-rf", "--", target])
    return builder


def in_builder(builder, cwd, arguments, log):
    run_cmd([
        "sudo", "unshare", "--mount", "--propagation", "private", "sh", "-ec",
        'mount -t proc proc "$1/proc"; '
        'mount -t devpts -o newinstance,ptmxmode=0666 devpts "$1/dev/pts"; '
        'exec chroot "$@"',
        "debian-cross", builder, "env", f"--chdir={cwd}",
        "LC_ALL=C.UTF-8", "DEBIAN_FRONTEND=noninteractive",
        "DEBEMAIL=builder@lmg820n.invalid", "DEBFULLNAME=LM-G820N Package Builder",
        *arguments,
    ], stdout=log, stderr=subprocess.STDOUT)


def fetch_sid_source(builder, source_pkg_name, build_work_dir, log):
    """Fetch the latest source package from Debian sid into builder."""
    in_builder(builder, build_work_dir, ["apt-get", "update"], log)
    in_builder(builder, build_work_dir, ["apt-get", "source", source_pkg_name], log)


def build_package(source, package_name, output, log_path, jobs=None):
    if jobs is None:
        jobs = os.cpu_count() or 1
    WORK.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    with (WORK / "build.lock").open("w") as lock, log_path.open("w") as log:
        fcntl.flock(lock, fcntl.LOCK_EX)
        builder = prepare_builder(log)
        destination = builder / "build" / package_name
        run_cmd(["sudo", "rm", "-rf", "--", destination])
        run_cmd(["sudo", "mkdir", "-p", destination])
        run_cmd(["sudo", "cp", "-a", source, destination / "source"])
        cwd = f"/build/{package_name}/source"
        print(f"Cross-building {package_name}; log: {log_path}", flush=True)
        in_builder(builder, cwd, ["apt-get", "update"], log)
        in_builder(builder, cwd, [
            "mk-build-deps", "--host-arch=arm64", "--build-arch=amd64",
            "--build-profiles=cross", "--build-dep", "--install", "--remove",
            "--tool=apt-get --yes --no-install-recommends -oDpkg::Use-Pty=false", "debian/control",
        ], log)
        in_builder(builder, cwd, [
            "DEB_BUILD_PROFILES=cross", f"DEB_BUILD_OPTIONS=parallel={jobs}",
            "dpkg-buildpackage", "--host-arch=arm64", "-B", "-us", "-uc",
        ], log)
        with tempfile.TemporaryDirectory(dir=output, prefix=".cross-build-") as temporary:
            temporary = Path(temporary)
            for pattern in ("*.deb", "*.buildinfo", "*.changes"):
                for path in sorted(destination.glob(pattern)):
                    copied = temporary / path.name
                    shutil.copyfile(path, copied)
                    copied.replace(output / path.name)
