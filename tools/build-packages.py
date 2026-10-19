#!/usr/bin/env python3
"""Unified package build system for LM-G820N.

Dynamically tracks Debian sid rolling updates without hardcoded version numbers,
applies device-specific patches, and produces standard arm64 .deb packages.
Supports direct native cross-building in debian:sid containers and fallback builder.
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from package_utils import digest, run_cmd, fetch, extract

ROOT = Path(__file__).resolve().parent.parent
PACKAGES_DIR = ROOT / "packages"
BUILD_DIR = ROOT / "build"
DIST_DIR = ROOT / "build/dist"


def is_native_sid_environment() -> bool:
    """Check if we are directly running inside a Debian environment as root."""
    return os.geteuid() == 0 and Path("/etc/debian_version").exists()


def apply_patches(source_dir: Path, patches_dir: Path):
    if not patches_dir.exists():
        return
    patches = sorted(patches_dir.glob("*.patch"))
    for patch in patches:
        print(f"  -> Applying patch: {patch.name}")
        run_cmd(["patch", "--batch", "-p1", "-i", str(patch)], cwd=source_dir)


def local_package_version(source_version: str, patches_dir: Path):
    revision_file = patches_dir.parent / "revision"
    revision = int(revision_file.read_text()) if revision_file.exists() else 1
    return f"{source_version}+lmg820n{revision}"


def build_debian_derived_native(package_name: str, source_pkg_name: str,
                                recipe_dir: Path, output_dir: Path, jobs: int,
                                patch_subdir: str = "patches", custom_debian: Path = None):
    """Directly build a Debian-derived package in the current Debian environment."""
    work_dir = BUILD_DIR / package_name
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)

    print(f"Fetching latest '{source_pkg_name}' source from Debian sid...")
    run_cmd(["apt-get", "update"])
    run_cmd(["apt-get", "source", source_pkg_name], cwd=work_dir)

    subdirs = [p for p in work_dir.iterdir() if p.is_dir() and (p / "debian/control").exists()]
    if not subdirs:
        raise RuntimeError(f"Could not find unpacked source directory for {source_pkg_name} in {work_dir}")
    src_dir = subdirs[0]

    version_out = subprocess.check_output(
        ["dpkg-parsechangelog", "-S", "Version"], cwd=src_dir, text=True
    ).strip()
    print(f"Upstream sid version detected: {version_out}")

    patches_dir = recipe_dir / patch_subdir
    if patches_dir.exists():
        apply_patches(src_dir, patches_dir)

    if custom_debian and custom_debian.exists():
        print(f"  -> Applying custom debian files from {custom_debian}...")
        for item in custom_debian.iterdir():
            target = src_dir / "debian" / item.name
            if item.is_dir():
                shutil.copytree(item, target, dirs_exist_ok=True)
            else:
                shutil.copy2(item, target)

    local_version = local_package_version(version_out, patches_dir)
    print(f"Setting local package version: {local_version}")
    env = {
        **os.environ,
        "DEBEMAIL": "builder@lmg820n.invalid",
        "DEBFULLNAME": "LM-G820N Package Builder",
    }
    run_cmd([
        "dch", "-b", "-v", local_version, "Automated build with LM-G820N patches for Debian sid"
    ], cwd=src_dir, env=env)

    current_arch = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
    is_arm64_host = (current_arch == "arm64")

    print(f"Installing build dependencies and compiling {package_name} ({current_arch} -> arm64)...")
    if is_arm64_host:
        run_cmd([
            "mk-build-deps", "--build-dep", "--install", "--remove",
            "--tool=apt-get --yes --no-install-recommends -oDpkg::Use-Pty=false", "debian/control",
        ], cwd=src_dir)
        build_env = {
            **env,
            "DEB_BUILD_OPTIONS": f"parallel={jobs} nocheck",
        }
        run_cmd([
            "dpkg-buildpackage", "-B", "-us", "-uc",
        ], cwd=src_dir, env=build_env)
    else:
        run_cmd([
            "mk-build-deps", "--host-arch=arm64", "--build-arch=amd64",
            "--build-profiles=cross", "--build-dep", "--install", "--remove",
            "--tool=apt-get --yes --no-install-recommends -oDpkg::Use-Pty=false", "debian/control",
        ], cwd=src_dir)
        build_env = {
            **env,
            "DEB_BUILD_PROFILES": "cross",
            "DEB_BUILD_OPTIONS": f"parallel={jobs} nocheck",
        }
        run_cmd([
            "dpkg-buildpackage", "--host-arch=arm64", "-B", "-us", "-uc",
        ], cwd=src_dir, env=build_env)

    output_dir.mkdir(parents=True, exist_ok=True)
    for deb in work_dir.glob("*.deb"):
        target = output_dir / deb.name
        shutil.copy2(deb, target)
        print(f"  Output: {target}")


def build_debian_derived_chroot(builder, package_name: str, source_pkg_name: str,
                                recipe_dir: Path, output_dir: Path, log, jobs: int,
                                patch_subdir: str = "patches", custom_debian: Path = None):
    """Fallback build inside mmdebstrap chroot when not running in native sid."""
    from debian_cross import in_builder

    dest_in_builder = f"/build/{package_name}"
    builder_fs_dest = builder / "build" / package_name

    run_cmd(["sudo", "rm", "-rf", "--", str(builder_fs_dest)])
    run_cmd(["sudo", "mkdir", "-p", str(builder_fs_dest)])

    print(f"Fetching latest '{source_pkg_name}' source from Debian sid (chroot)...")
    in_builder(builder, dest_in_builder, ["apt-get", "update"], log)
    in_builder(builder, dest_in_builder, ["apt-get", "source", source_pkg_name], log)

    subdirs = [p for p in builder_fs_dest.iterdir() if p.is_dir() and (p / "debian/control").exists()]
    if not subdirs:
        raise RuntimeError(f"Could not find unpacked source directory for {source_pkg_name} in {builder_fs_dest}")
    src_dir = subdirs[0]
    src_dir_in_builder = f"{dest_in_builder}/{src_dir.name}"
    run_cmd(["sudo", "chown", "-R", f"{os.getuid()}:{os.getgid()}", str(src_dir)])

    version_out = subprocess.check_output(
        ["dpkg-parsechangelog", "-S", "Version"], cwd=src_dir, text=True
    ).strip()
    print(f"Upstream sid version detected: {version_out}")

    patches_dir = recipe_dir / patch_subdir
    if patches_dir.exists():
        apply_patches(src_dir, patches_dir)

    if custom_debian and custom_debian.exists():
        print(f"  -> Applying custom debian files from {custom_debian}...")
        for item in custom_debian.iterdir():
            target = src_dir / "debian" / item.name
            if item.is_dir():
                shutil.copytree(item, target, dirs_exist_ok=True)
            else:
                shutil.copy2(item, target)

    local_version = local_package_version(version_out, patches_dir)
    print(f"Setting local package version: {local_version}")
    in_builder(builder, src_dir_in_builder, [
        "dch", "-b", "-v", local_version, "Automated build with LM-G820N patches for Debian sid"
    ], log)

    print(f"Installing build dependencies and cross-compiling {package_name} (arm64)...")
    in_builder(builder, src_dir_in_builder, [
        "mk-build-deps", "--host-arch=arm64", "--build-arch=amd64",
        "--build-profiles=cross", "--build-dep", "--install", "--remove",
        "--tool=apt-get --yes --no-install-recommends -oDpkg::Use-Pty=false", "debian/control",
    ], log)

    in_builder(builder, src_dir_in_builder, [
        "DEB_BUILD_PROFILES=cross", f"DEB_BUILD_OPTIONS=parallel={jobs} nocheck",
        "dpkg-buildpackage", "--host-arch=arm64", "-B", "-us", "-uc",
    ], log)

    output_dir.mkdir(parents=True, exist_ok=True)
    for deb in builder_fs_dest.glob("*.deb"):
        target = output_dir / deb.name
        shutil.copy2(deb, target)
        print(f"  Output: {target}")


def build_local_package(builder_ctx, output_dir: Path, log, jobs: int, component: str,
                        source_package: str):
    recipe = PACKAGES_DIR / component
    is_native = builder_ctx is None

    if is_native:
        work_dir = BUILD_DIR / f"{component}-build"
        if work_dir.exists():
            shutil.rmtree(work_dir)
        work_dir.mkdir(parents=True, exist_ok=True)
        src_copy = work_dir / "source"
        shutil.copytree(recipe, src_copy)

        current_arch = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
        is_arm64_host = (current_arch == "arm64")

        print(f"Installing build dependencies and compiling {source_package} ({current_arch} -> arm64)...")
        if is_arm64_host:
            run_cmd([
                "mk-build-deps", "--build-dep", "--install", "--remove",
                "--tool=apt-get --yes --no-install-recommends -oDpkg::Use-Pty=false", "debian/control",
            ], cwd=src_copy)
            run_cmd([
                "dpkg-buildpackage", "-B", "-us", "-uc",
            ], cwd=src_copy, env={**os.environ, "DEB_BUILD_OPTIONS": f"parallel={jobs} nocheck"})
        else:
            run_cmd([
                "mk-build-deps", "--host-arch=arm64", "--build-arch=amd64",
                "--build-profiles=cross", "--build-dep", "--install", "--remove",
                "--tool=apt-get --yes --no-install-recommends -oDpkg::Use-Pty=false", "debian/control",
            ], cwd=src_copy)
            run_cmd([
                "dpkg-buildpackage", "--host-arch=arm64", "-B", "-us", "-uc",
            ], cwd=src_copy, env={**os.environ, "DEB_BUILD_PROFILES": "cross", "DEB_BUILD_OPTIONS": f"parallel={jobs} nocheck"})

        output_dir.mkdir(parents=True, exist_ok=True)
        for deb in work_dir.glob("*.deb"):
            target = output_dir / deb.name
            shutil.copy2(deb, target)
            print(f"  Output: {target}")
    else:
        from debian_cross import in_builder
        dest_in_builder = f"/build/{source_package}"
        builder_fs_dest = builder_ctx / "build" / source_package

        run_cmd(["sudo", "rm", "-rf", "--", str(builder_fs_dest)])
        run_cmd(["sudo", "mkdir", "-p", str(builder_fs_dest)])
        run_cmd(["sudo", "cp", "-a", str(recipe), str(builder_fs_dest / "source")])

        cwd = f"{dest_in_builder}/source"
        print(f"Installing build dependencies and building {source_package} (chroot)...")
        in_builder(builder_ctx, cwd, ["apt-get", "update"], log)
        in_builder(builder_ctx, cwd, [
            "mk-build-deps", "--host-arch=arm64", "--build-arch=amd64",
            "--build-profiles=cross", "--build-dep", "--install", "--remove",
            "--tool=apt-get --yes --no-install-recommends -oDpkg::Use-Pty=false", "debian/control",
        ], log)
        in_builder(builder_ctx, cwd, [
            "DEB_BUILD_PROFILES=cross", f"DEB_BUILD_OPTIONS=parallel={jobs}",
            "dpkg-buildpackage", "--host-arch=arm64", "-B", "-us", "-uc",
        ], log)

        output_dir.mkdir(parents=True, exist_ok=True)
        for deb in builder_fs_dest.glob("*.deb"):
            target = output_dir / deb.name
            shutil.copy2(deb, target)
            print(f"  Output: {target}")


def make_builder_runner(builder_ctx, log, jobs):
    is_native = builder_ctx is None

    def runner(package_name, source_pkg_name, recipe_dir, output_dir, patch_subdir="patches", custom_debian=None):
        if is_native:
            build_debian_derived_native(package_name, source_pkg_name, recipe_dir, output_dir, jobs,
                                        patch_subdir, custom_debian)
        else:
            build_debian_derived_chroot(builder_ctx, package_name, source_pkg_name, recipe_dir, output_dir, log, jobs,
                                        patch_subdir, custom_debian)
    return runner


def build_geoclue(runner, output_dir: Path):
    recipe = PACKAGES_DIR / "geoclue"
    runner("geoclue-2.0", "geoclue-2.0", recipe, output_dir)


def build_pipewire_bluetooth(runner, output_dir: Path):
    recipe = PACKAGES_DIR / "pipewire-bluetooth"
    runner("pipewire-bluetooth", "pipewire", recipe, output_dir, custom_debian=recipe / "debian")


def build_sensors(runner, output_dir: Path):
    recipe = PACKAGES_DIR / "sensors"
    for comp in ("hexagonrpc", "libssc", "iio-sensor-proxy"):
        print(f"\n--- Building sensor component: {comp} ---")
        if comp == "iio-sensor-proxy":
            libssc_debs = sorted(output_dir.glob("libssc*.deb")) + sorted(output_dir.glob("gir1.2-ssc*.deb"))
            if libssc_debs:
                print(f"Installing local libssc dependencies for iio-sensor-proxy: {[p.name for p in libssc_debs]}")
                if is_native_sid_environment():
                    run_cmd(["apt-get", "install", "-y", "--allow-downgrades", *[str(p) for p in libssc_debs]])
        runner(comp, comp, recipe, output_dir, patch_subdir=f"{comp}/patches")


def build_vpu_playback(runner, output_dir: Path):
    recipe = PACKAGES_DIR / "vpu-playback"
    runner("ffmpeg", "ffmpeg", recipe, output_dir)


def build_libcamera(runner, output_dir: Path):
    recipe = PACKAGES_DIR / "libcamera"
    runner("libcamera", "libcamera", recipe, output_dir)


def build_fcitx5_osk(builder_ctx, output_dir: Path, log, jobs: int):
    recipe = PACKAGES_DIR / "fcitx5-osk"
    manifest = json.loads((recipe / "sources.json").read_text())

    work_dir = BUILD_DIR / "fcitx5-osk"
    work_dir.mkdir(parents=True, exist_ok=True)
    downloads_dir = BUILD_DIR / "downloads"
    downloads_dir.mkdir(parents=True, exist_ok=True)

    print("Preparing fcitx5-osk sources, crates and fonts...")
    source_dir = work_dir / "source"
    extract(fetch(manifest["source"], downloads_dir), source_dir)
    for patch_name in manifest["source"]["patches"]:
        patch_file = recipe / "patches" / patch_name
        print(f"  -> Applying patch: {patch_name}")
        run_cmd(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch_file)], cwd=source_dir)

    crates_dir = work_dir / "crates"
    if crates_dir.exists():
        shutil.rmtree(crates_dir)
    crates_dir.mkdir(parents=True, exist_ok=True)
    for crate in manifest["crates"]:
        dest = crates_dir / f"{crate['name']}-{crate['version']}"
        extract(fetch(crate, downloads_dir), dest)
        for patch_name in crate["patches"]:
            patch_file = recipe / "patches" / patch_name
            print(f"  -> Applying patch: {patch_name}")
            run_cmd(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch_file)], cwd=dest)

    for font_resource in manifest["font"]:
        fetch(font_resource, downloads_dir)

    print(f"Building fcitx5-osk binaries (Cargo, jobs={jobs})...")
    target_arch = "aarch64-unknown-linux-gnu"
    target_dir = work_dir / "target"
    linker = shutil.which("aarch64-linux-gnu-gcc") or "gcc"
    strip_bin = shutil.which("aarch64-linux-gnu-strip") or "strip"

    env = {
        **os.environ,
        "CARGO_TARGET_DIR": str(target_dir),
        "CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER": linker,
        "CC_aarch64_unknown_linux_gnu": linker,
        "CARGO_PROFILE_RELEASE_LTO": "false",
        "CARGO_PROFILE_RELEASE_CODEGEN_UNITS": "16",
        "SOURCE_DATE_EPOCH": str(manifest["source_date_epoch"]),
    }

    run_cmd(["cargo", "build", "--manifest-path", str(source_dir / "Cargo.toml"), "--workspace",
             "--release", "--locked", "--no-default-features", "--target", target_arch,
             "--jobs", str(jobs)], env=env)

    print("Staging files with CMake...")
    cmake_dir = work_dir / "cmake"
    stage_dir = work_dir / "package"
    for d in (cmake_dir, stage_dir):
        if d.exists():
            shutil.rmtree(d)

    run_cmd(["cmake", "-S", str(source_dir), "-B", str(cmake_dir), "-DCMAKE_INSTALL_PREFIX=/usr",
             "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_KWIN_PLUGIN=OFF"])

    (cmake_dir / "cargo_target").mkdir(parents=True, exist_ok=True)
    release_symlink = cmake_dir / "cargo_target/release"
    if not release_symlink.exists():
        release_symlink.symlink_to(target_dir / target_arch / "release", target_is_directory=True)
    run_cmd(["cmake", "--install", str(cmake_dir)], env={**env, "DESTDIR": str(stage_dir)})

    for name in ("otp", "pw"):
        custom_action = stage_dir / "usr/share/fcitx5-osk/custom_actions" / f"{name}.toml"
        if custom_action.exists():
            custom_action.unlink()

    binaries = ("fcitx5-osk", "fcitx5-osk-key-helper", "fcitx5-osk-kwin-launcher")
    for name in binaries:
        binary = stage_dir / "usr/bin" / name
        run_cmd([strip_bin, "--strip-unneeded", str(binary)])

    font_dir = stage_dir / "usr/share/fonts/truetype/fcitx5-osk"
    license_dir = stage_dir / "usr/share/licenses/fcitx5-osk/NotoSerifNerdFont"
    font_dir.mkdir(parents=True, exist_ok=True)
    license_dir.mkdir(parents=True, exist_ok=True)
    for resource in manifest["font"]:
        fname = resource["filename"]
        d = font_dir if fname.endswith(".ttf") else license_dir
        shutil.copyfile(downloads_dir / fname, d / fname)

    doc_dir = stage_dir / "usr/share/doc/fcitx5-osk"
    doc_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(recipe / "debian/copyright", doc_dir / "copyright")

    files = sorted(path for path in stage_dir.rglob("*") if path.is_file() and not path.is_symlink())
    installed_size = sum((path.stat().st_size + 1023) // 1024 for path in files)
    control_dir = stage_dir / "DEBIAN"
    control_dir.mkdir(parents=True, exist_ok=True)
    template = (recipe / "debian/control.in").read_text()
    (control_dir / "control").write_text(template.replace("@VERSION@", manifest["version"])
                                         .replace("@INSTALLED_SIZE@", str(installed_size)))

    if (recipe / "debian/conffiles").exists():
        shutil.copyfile(recipe / "debian/conffiles", control_dir / "conffiles")
    if (recipe / "debian/postinst").exists():
        shutil.copyfile(recipe / "debian/postinst", control_dir / "postinst")
        (control_dir / "postinst").chmod(0o755)

    conffiles = set()
    if (control_dir / "conffiles").exists():
        conffiles = set((control_dir / "conffiles").read_text().splitlines())

    (control_dir / "md5sums").write_text("".join(
        f"{digest(path, 'md5')}  {path.relative_to(stage_dir)}\n"
        for path in files if "/" + str(path.relative_to(stage_dir)) not in conffiles))

    epoch = manifest["source_date_epoch"]
    for path in [*stage_dir.rglob("*"), stage_dir]:
        os.utime(path, (epoch, epoch), follow_symlinks=False)

    output_dir.mkdir(parents=True, exist_ok=True)
    deb_filename = f"fcitx5-osk_{manifest['version']}_arm64.deb"
    final_deb = output_dir / deb_filename
    with tempfile.TemporaryDirectory(dir=output_dir, prefix=".fcitx5-osk-") as temporary:
        built = Path(temporary) / deb_filename
        run_cmd(["dpkg-deb", "--root-owner-group", "--build", str(stage_dir), str(built)], env=env)
        built.replace(final_deb)
    print(f"  Output: {final_deb}")


PACKAGE_BUILDERS = {
    "geoclue": build_geoclue,
    "pipewire-bluetooth": build_pipewire_bluetooth,
    "sensors": build_sensors,
    "vpu-playback": build_vpu_playback,
    "libcamera": build_libcamera,
    "gnss": None,  # handled separately
    "fcitx5-osk": None,  # handled separately
}


def verify_artifacts(dist_dir: Path):
    debs = list(dist_dir.rglob("*.deb"))
    print(f"\nVerifying {len(debs)} package artifacts in {dist_dir}...")
    errors = 0
    for deb in debs:
        try:
            out = subprocess.check_output(
                ["dpkg-deb", "-f", str(deb), "Package", "Version", "Architecture"], text=True
            ).strip()
            info = {}
            for line in out.splitlines():
                if ": " in line:
                    k, v = line.split(": ", 1)
                    info[k.strip()] = v.strip()
            pkg = info.get("Package", "unknown")
            ver = info.get("Version", "unknown")
            arch = info.get("Architecture", "unknown")
            if arch != "arm64":
                print(f"  [ERROR] {deb.name}: Expected arm64 architecture, found {arch}")
                errors += 1
            else:
                print(f"  [OK] {pkg} ({ver}) [{arch}] - {deb.name}")
        except Exception as e:
            print(f"  [FAIL] {deb.name}: {e}")
            errors += 1
    if errors:
        raise SystemExit(f"Verification failed with {errors} error(s).")
    print("All package artifacts verified successfully.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", nargs="*", help="Packages to build (default: all)")
    parser.add_argument("--all", action="store_true", help="Build all defined packages")
    parser.add_argument("--verify", action="store_true", help="Verify built package artifacts")
    parser.add_argument("--create-repo", action="store_true", help="Generate APT repository after build")
    parser.add_argument("--output-dir", "-o", type=Path, default=DIST_DIR, help="Output directory for deb packages")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1, help="Parallel jobs")
    args = parser.parse_args()

    output_dir = args.output_dir.resolve()

    if args.verify:
        verify_artifacts(output_dir)
        return

    targets = args.packages
    if args.all or not targets:
        targets = list(PACKAGE_BUILDERS.keys())

    unknown = set(targets) - set(PACKAGE_BUILDERS.keys())
    if unknown:
        sys.exit(f"Unknown package(s): {', '.join(unknown)}. Available: {', '.join(PACKAGE_BUILDERS.keys())}")

    log_path = BUILD_DIR / "package-build.log"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    with (BUILD_DIR / "build.lock").open("w") as lock, log_path.open("w") as log:
        fcntl.flock(lock, fcntl.LOCK_EX)

        builder_ctx = None
        needs_builder = any(t != "fcitx5-osk" for t in targets)
        if is_native_sid_environment():
            print("Running in native Debian sid environment. Using direct cross-build pipeline.")
        elif needs_builder:
            print("Not in native Debian sid root. Preparing multiarch chroot builder...")
            from debian_cross import prepare_builder
            builder_ctx = prepare_builder(log)

        runner = make_builder_runner(builder_ctx, log, args.jobs) if needs_builder else None

        for target in targets:
            print(f"\n==========================================")
            print(f"Building package: {target}")
            print(f"==========================================")
            pkg_output = output_dir / target
            if target == "gnss":
                build_local_package(builder_ctx, pkg_output, log, args.jobs, "gnss", "lmg820n-gnssd")
            elif target == "fcitx5-osk":
                build_fcitx5_osk(builder_ctx, pkg_output, log, args.jobs)
            else:
                builder_fn = PACKAGE_BUILDERS[target]
                builder_fn(runner, pkg_output)

    if args.create_repo:
        print("\n==========================================")
        print("Generating APT repository...")
        print("==========================================")
        from make_repo import create_apt_repository
        create_apt_repository(output_dir, ROOT / "repo", "sid")

    print("\nPackage build complete.")


if __name__ == "__main__":
    main()
