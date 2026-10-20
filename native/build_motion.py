#!/usr/bin/env python3
"""Build the isolated CS:DM motion fork. Never touch Steam or start the game."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

COMMIT = "7270bc95c8cab87b7f9ddd490fea13c6eda3ace7"
REPOSITORY = "https://github.com/akiver/cs-demo-manager.git"
DEPENDENCIES = {
    "easywsclient": "afc1d8cfc584e0f1f4a77e8c0ce3e979d9fe7ce2",
    "hl2sdk": "dc8e7324b1e7f3cb115b1a861280a30ba5ce72a2",
    "json": "a03e65420ca9a14c97080b67036859aaac06700d",
}
HERE = Path(__file__).resolve().parent


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_fingerprint() -> str:
    value = hashlib.sha256(COMMIT.encode())
    for path in (HERE / "csdm-motion.patch", HERE / "motion_track.h", HERE / "linux_view_patch.h", HERE / "build_motion.py"):
        value.update(path.name.encode())
        value.update(path.read_bytes())
    return value.hexdigest()


def run(args: list[str], cwd: Path | None = None) -> str:
    print("+ " + " ".join(args), flush=True)
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n", flush=True)
    if result.returncode:
        raise RuntimeError(f"Build command failed with status {result.returncode}: {args[0]}")
    return result.stdout.strip()


def cached_plugin(cache: Path) -> Path | None:
    manifest, plugin = cache / "build.json", cache / "libserver.so"
    if not manifest.is_file() or not plugin.is_file() or plugin.is_symlink():
        return None
    try:
        data = json.loads(manifest.read_text())
        if (data["source_fingerprint"] == source_fingerprint()
                and data["upstream_commit"] == COMMIT and data["motion_abi"] == 3
                and data["sha256"] == digest(plugin)
                and b"CS2_MVP_MOTION_ABI 3" in plugin.read_bytes()):
            return plugin
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def build(cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / "build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        existing = cached_plugin(cache)
        if existing:
            return existing
        for command in ("git", "g++", "make"):
            if shutil.which(command) is None:
                raise RuntimeError(f"Native motion build requires {command}")
        source = cache / "upstream"
        if not (source / ".git").is_dir():
            source.mkdir(exist_ok=True)
            run(["git", "init", "-q"], source)
            run(["git", "remote", "add", "origin", REPOSITORY], source)
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=source, text=True,
                              capture_output=True)
        if head.returncode:
            run(["git", "fetch", "--depth=1", "origin", COMMIT], source)
            run(["git", "checkout", "--detach", COMMIT], source)
        elif head.stdout.strip() != COMMIT:
            raise RuntimeError(f"Unexpected upstream checkout in managed build cache: {source}")
        relative = "cs2-server-plugin/cs2-server-plugin"
        dependencies = [f"{relative}/deps/{name}" for name in DEPENDENCIES]
        run(["git", "submodule", "update", "--init", "--depth=1", "--jobs=3", "--", *dependencies], source)
        for name, expected in DEPENDENCIES.items():
            actual = run(["git", "rev-parse", "HEAD"], source / relative / "deps" / name)
            if actual != expected:
                raise RuntimeError(f"Unexpected {name} dependency commit: {actual}")

        patch = HERE / "csdm-motion.patch"
        check = subprocess.run(["git", "apply", "--check", str(patch)], cwd=source,
                               capture_output=True)
        if check.returncode == 0:
            run(["git", "apply", str(patch)], source)
        else:
            # A prior interrupted build may already have applied this exact
            # patch. Refuse unrelated edits instead of resetting the checkout.
            reverse = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                                     cwd=source, capture_output=True)
            if reverse.returncode:
                raise RuntimeError(f"Managed native source has unrelated changes: {source}")
        project = source / relative
        for name in ["motion_track.h", "linux_view_patch.h"]:
            shutil.copyfile(HERE / name, project / name)
        run(["make", "build"], project)
        built = source / "static/cs2/libserver.so"
        binary = built.read_bytes()
        if not binary.startswith(b"\x7fELF\x02\x01") or b"CS2_MVP_MOTION_ABI 3" not in binary:
            raise RuntimeError("Compiled library is not the expected ELF64 motion plugin")
        destination = cache / "libserver.so"
        temporary = cache / "libserver.so.tmp"
        shutil.copy2(built, temporary)
        temporary.chmod(0o755)
        os.replace(temporary, destination)
        manifest = {
            "motion_abi": 3,
            "upstream_commit": COMMIT,
            "dependencies": DEPENDENCIES,
            "source_fingerprint": source_fingerprint(),
            "sha256": digest(destination),
            "compiler": run(["g++", "--version"]).splitlines()[0],
        }
        (cache / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
        shutil.copy2(source / "LICENSE", cache / "UPSTREAM-LICENSE")
        return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, required=True)
    options = parser.parse_args()
    print(build(options.cache_dir.resolve()))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Native motion build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
