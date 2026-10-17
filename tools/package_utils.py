"""Common utility functions for LM-G820N package builds."""

import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


def digest(path: Path, algorithm: str = "sha256") -> str:
    h = hashlib.new(algorithm)
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run_cmd(args, **kwargs):
    cmd_str = [str(x) for x in args]
    return subprocess.run(cmd_str, check=True, **kwargs)


def fetch(resource: dict, downloads_dir: Path) -> Path:
    path = downloads_dir / resource["filename"]
    if path.exists():
        if digest(path) != resource["sha256"]:
            print(f"Checksum mismatch for {path}, re-downloading...")
            path.unlink()
        else:
            return path
    print(f"Downloading {resource['url']}...", flush=True)
    with tempfile.NamedTemporaryFile(dir=downloads_dir, delete=False) as temporary:
        temporary_path = Path(temporary.name)
        try:
            req = urllib.request.Request(resource["url"], headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=60) as response:
                shutil.copyfileobj(response, temporary)
            temporary.flush()
            if digest(temporary_path) != resource["sha256"]:
                raise RuntimeError(f"Downloaded source checksum mismatch: {resource['url']}")
            temporary_path.replace(path)
        finally:
            temporary_path.unlink(missing_ok=True)
    return path


def extract(archive: Path, destination: Path):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".extract-") as temporary:
        temporary_path = Path(temporary)
        with tarfile.open(archive) as source:
            kwargs = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
            source.extractall(temporary_path, **kwargs)
        roots = list(temporary_path.iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise RuntimeError(f"Expected one source directory in {archive}")
        if destination.exists():
            shutil.rmtree(destination)
        roots[0].rename(destination)
