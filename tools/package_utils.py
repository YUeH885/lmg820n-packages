"""Common utility functions for LM-G820N package builds."""

import hashlib
from pathlib import Path
import subprocess


def digest(path: Path, algorithm: str = "sha256") -> str:
    h = hashlib.new(algorithm)
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run_cmd(args, **kwargs):
    cmd_str = [str(x) for x in args]
    return subprocess.run(cmd_str, check=True, **kwargs)
