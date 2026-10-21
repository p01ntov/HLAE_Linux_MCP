#!/usr/bin/env python3
"""Build the isolated CS:DM motion fork. Never touch Steam or start the game."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
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
RELATIVE = Path("cs2-server-plugin/cs2-server-plugin")
BUILD_FLAGS = (
    "-std=c++17", "-Wall", "-Wno-switch", "-shared", "-fPIC",
    "-fvisibility=hidden", "-fvisibility-inlines-hidden", "-fno-gnu-unique",
    "-Wl,-Bsymbolic-functions", "-Wl,--version-script=capture_exports.map",
    "-DPOSIX=1", "-DNO_MALLOC_OVERRIDE=1", "-DCOMPILER_GCC=1",
    "-DPLATFORM_POSIX=1", "-DX64BITS=1", "-DPLATFORM_64BITS=1",
    "-DLINUX=1", "-Dstricmp=strcasecmp",
)
SOURCES = (
    HERE / "motion_track.h", HERE / "linux_view_patch.h",
    HERE / "capture_environment.h", HERE / "capture_cosmetics.h", HERE / "capture_exports.map",
    HERE / "live/scene_environment.h", HERE / "live/appearance_pack.h",
    HERE / "live/appearance_runtime.h", HERE / "live/compat_math.cpp",
    HERE / "live/team_palette.h", HERE / "live/cosmetics_catalog.h",
    HERE / "live/player_cosmetics.h", HERE / "live/cosmetics_runtime.h", HERE / "live/weapon_cosmetics.h",
    HERE / "live/glove_cosmetics.h", HERE / "live/glove_pose.h", HERE / "live/glove_render_phase.h",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_fingerprint() -> str:
    value = hashlib.sha256(COMMIT.encode())
    for path in (HERE / "csdm-motion.patch", *SOURCES, HERE / "build_motion.py"):
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
                and data["environment_abi"] == 1
                and data["sha256"] == digest(plugin)
                and b"CS2_MVP_MOTION_ABI 3" in plugin.read_bytes()
                and b"CS2_MVP_ENVIRONMENT_ABI 1" in plugin.read_bytes()):
            return plugin
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def apply_patch(source: Path, cache: Path) -> None:
    """Upgrade only a main.cpp verified as the managed prior build."""
    relative = str(RELATIVE / "main.cpp")
    main = source / relative
    marker = cache / "main-source.sha256"
    patch = HERE / "csdm-motion.patch"
    original = subprocess.check_output(["git", "show", f"{COMMIT}:{relative}"], cwd=source)
    if main.read_bytes() != original:
        verified = marker.is_file() and marker.read_text().strip() == digest(main)
        if not verified:
            reverse = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                                     cwd=source, capture_output=True)
            if reverse.returncode:
                raise RuntimeError(f"Managed native source has unrelated edits: {source}")
    changed = subprocess.check_output(["git", "diff", "--name-only"], cwd=source, text=True).splitlines()
    allowed = {relative, "static/cs2/libserver.so", *(str(RELATIVE / "deps" / name) for name in DEPENDENCIES)}
    if any(path not in allowed for path in changed):
        raise RuntimeError(f"Managed native checkout has unrelated tracked changes: {source}")
    main.write_bytes(original)
    run(["git", "apply", "--check", str(patch)], source)
    run(["git", "apply", str(patch)], source)
    marker.write_text(digest(main) + "\n")


def build(cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / "build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        existing = cached_plugin(cache)
        if existing:
            return existing
        fingerprint = source_fingerprint()
        for command in ("git", "g++", "make", "nm"):
            if shutil.which(command) is None:
                raise RuntimeError(f"Native motion build requires {command}")
        source = cache / "upstream"
        if source.is_symlink():
            raise RuntimeError("Managed native source must not be a symlink")
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
        relative = str(RELATIVE)
        dependencies = [str(RELATIVE / "deps" / name) for name in DEPENDENCIES]
        verified_dependencies = True
        for name, expected in DEPENDENCIES.items():
            dependency = source / relative / "deps" / name
            if not dependency.is_dir():
                verified_dependencies = False
                continue
            revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=dependency,
                                      text=True, capture_output=True)
            if revision.returncode or revision.stdout.strip() != expected:
                verified_dependencies = False
        if not verified_dependencies:
            run(["git", "submodule", "update", "--init", "--depth=1", "--jobs=3", "--", *dependencies], source)
        for name, expected in DEPENDENCIES.items():
            actual = run(["git", "rev-parse", "HEAD"], source / relative / "deps" / name)
            if actual != expected:
                raise RuntimeError(f"Unexpected {name} dependency commit: {actual}")

        apply_patch(source, cache)
        project = source / relative
        for path in SOURCES:
            shutil.copyfile(path, project / path.name)
        run(["make", "build", "CXXFLAGS=" + " ".join(BUILD_FLAGS),
             "SRC_FILES=main.cpp compat_math.cpp ./deps/easywsclient/easywsclient.cpp ./deps/hl2sdk/tier1/convar.cpp"], project)
        built = source / "static/cs2/libserver.so"
        binary = built.read_bytes()
        if (not binary.startswith(b"\x7fELF\x02\x01") or b"CS2_MVP_MOTION_ABI 3" not in binary
                or b"CS2_MVP_ENVIRONMENT_ABI 1" not in binary):
            raise RuntimeError("Compiled library is not the expected ELF64 motion plugin")
        versions = {(int(major), int(minor)) for major, minor in re.findall(rb"GLIBC_(\d+)\.(\d+)", binary)}
        if versions and max(versions) > (2, 38):
            raise RuntimeError(f"Capture plugin requires glibc {max(versions)}; supported baseline is 2.38")
        exports = {line.split()[-1] for line in run(["nm", "-D", "--defined-only", str(built)]).splitlines() if line.split()}
        if exports != {"CreateInterface"}:
            raise RuntimeError(f"Unexpected native capture exports: {sorted(exports)}")
        if source_fingerprint() != fingerprint:
            raise RuntimeError("Capture sources changed during compilation; refusing to install a stale library")
        destination = cache / "libserver.so"
        temporary = cache / "libserver.so.tmp"
        shutil.copy2(built, temporary)
        temporary.chmod(0o755)
        os.replace(temporary, destination)
        manifest = {
            "motion_abi": 3,
            "environment_abi": 1,
            "upstream_commit": COMMIT,
            "dependencies": DEPENDENCIES,
            "source_fingerprint": fingerprint,
            "sha256": digest(destination),
            "main_source_sha256": digest(project / "main.cpp"),
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
