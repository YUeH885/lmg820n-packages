#!/usr/bin/env python3
"""Generate a standard Debian APT repository from built .deb packages."""

import argparse
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import gzip

from package_utils import digest, run_cmd

ROOT = Path(__file__).resolve().parent.parent


def get_deb_info(deb_path: Path):
    """Extract metadata from a .deb package using dpkg-deb."""
    out = subprocess.check_output(
        ["dpkg-deb", "-f", str(deb_path)], text=True, errors="replace"
    )
    fields = {}
    current_key = None
    for line in out.splitlines():
        if line.startswith(" ") and current_key:
            fields[current_key] += "\n" + line
        elif ":" in line:
            key, val = line.split(":", 1)
            current_key = key.strip()
            fields[current_key] = val.strip()
    return fields


def create_apt_repository(debs_dir: Path, repo_dir: Path, codename: str = "sid"):
    """Create a standard APT repository pool and dists tree."""
    pool_dir = repo_dir / "pool/main"
    dists_dir = repo_dir / f"dists/{codename}/main/binary-arm64"
    pool_dir.mkdir(parents=True, exist_ok=True)
    dists_dir.mkdir(parents=True, exist_ok=True)

    deb_files = sorted(debs_dir.rglob("*.deb"))
    if not deb_files:
        print(f"Warning: No .deb files found in {debs_dir}")
        return

    print(f"Processing {len(deb_files)} package(s)...")

    # Copy / update deb files in pool
    packages_entries = []

    for deb in deb_files:
        info = get_deb_info(deb)
        pkg_name = info.get("Package", deb.name.split("_")[0])
        first_letter = pkg_name[:4] if pkg_name.startswith("lib") else pkg_name[0]
        
        target_dir = pool_dir / first_letter / pkg_name
        target_dir.mkdir(parents=True, exist_ok=True)
        target_deb = target_dir / deb.name
        
        shutil.copy2(deb, target_deb)
        
        # Calculate relative path from repo root
        rel_path = target_deb.relative_to(repo_dir)
        size = target_deb.stat().st_size
        sha256 = digest(target_deb, "sha256")
        md5 = digest(target_deb, "md5")
        sha1 = digest(target_deb, "sha1")

        # Format package stanza for Packages file
        raw_control = subprocess.check_output(
            ["dpkg-deb", "-f", str(target_deb)], text=True, errors="replace"
        ).strip()
        
        entry = (
            f"{raw_control}\n"
            f"Filename: {rel_path}\n"
            f"Size: {size}\n"
            f"MD5sum: {md5}\n"
            f"SHA1: {sha1}\n"
            f"SHA256: {sha256}\n\n"
        )
        packages_entries.append(entry)

    packages_content = "".join(packages_entries).encode("utf-8")

    # Write Packages & Packages.gz
    packages_file = dists_dir / "Packages"
    packages_gz = dists_dir / "Packages.gz"

    packages_file.write_bytes(packages_content)
    with gzip.open(packages_gz, "wb") as gz:
        gz.write(packages_content)

    # Generate Release file
    release_dir = repo_dir / f"dists/{codename}"
    release_file = release_dir / "Release"
    date_str = datetime.now(timezone.utc).strftime("%a, %d %b %Y %H:%M:%S UTC")

    p_size = packages_file.stat().st_size
    p_sha256 = digest(packages_file, "sha256")
    gz_size = packages_gz.stat().st_size
    gz_sha256 = digest(packages_gz, "sha256")

    release_content = (
        f"Origin: LM-G820N Linux Project\n"
        f"Label: LM-G820N Packages\n"
        f"Suite: {codename}\n"
        f"Codename: {codename}\n"
        f"Date: {date_str}\n"
        f"Architectures: arm64\n"
        f"Components: main\n"
        f"Description: Debian sid binary packages for LG G8 ThinQ (sm8150-lg-alphaplus)\n"
        f"SHA256:\n"
        f" {p_sha256} {p_size} main/binary-arm64/Packages\n"
        f" {gz_sha256} {gz_size} main/binary-arm64/Packages.gz\n"
    )
    release_file.write_text(release_content)

    print(f"APT repository successfully generated in: {repo_dir}")
    print(f"  Distribution: {codename}")
    print(f"  Components: main")
    print(f"  Architectures: arm64")
    print(f"  Packages count: {len(packages_entries)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", "-i", type=Path, default=ROOT / "build/dist",
                        help="Input directory containing .deb files (default: build/dist)")
    parser.add_argument("--output", "-o", type=Path, default=ROOT / "repo",
                        help="Output directory for APT repository (default: repo)")
    parser.add_argument("--codename", "-c", type=str, default="sid",
                        help="Debian codename (default: sid)")
    args = parser.parse_args()

    create_apt_repository(args.input.resolve(), args.output.resolve(), args.codename)


if __name__ == "__main__":
    main()
