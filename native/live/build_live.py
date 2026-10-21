#!/usr/bin/env python3
"""Build the isolated live CS:DM fork; never alter Steam or the capture cache."""
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

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent
COMMIT = "7270bc95c8cab87b7f9ddd490fea13c6eda3ace7"
REPOSITORY = "https://github.com/akiver/cs-demo-manager.git"
DEPENDENCIES = {
    "easywsclient": "afc1d8cfc584e0f1f4a77e8c0ce3e979d9fe7ce2",
    "hl2sdk": "dc8e7324b1e7f3cb115b1a861280a30ba5ce72a2",
    "json": "a03e65420ca9a14c97080b67036859aaac06700d",
}
RELATIVE = Path("cs2-server-plugin/cs2-server-plugin")
BUILD_FLAGS = (
    "-std=c++17", "-O2", "-Wall", "-Wno-switch", "-shared", "-fPIC",
    "-fvisibility=hidden", "-fvisibility-inlines-hidden", "-fno-gnu-unique",
    "-Wl,-Bsymbolic-functions", "-Wl,--version-script=live_exports.map",
    "-DPOSIX=1", "-DNO_MALLOC_OVERRIDE=1",
    "-DCOMPILER_GCC=1", "-DPLATFORM_POSIX=1", "-DX64BITS=1",
    "-DPLATFORM_64BITS=1", "-DLINUX=1", "-Dstricmp=strcasecmp",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_fingerprint() -> str:
    value = hashlib.sha256(COMMIT.encode())
    for path in (NATIVE / "csdm-motion.patch", NATIVE / "motion_track.h",
                 NATIVE / "linux_view_patch.h", NATIVE / "capture_environment.h", NATIVE / "capture_cosmetics.h", HERE / "csdm-live.patch",
                 HERE / "live_controller.h", HERE / "scene_environment.h", HERE / "team_palette.h",
                 HERE / "cosmetics_catalog.h", HERE / "player_cosmetics.h", HERE / "cosmetics_runtime.h",
                 HERE / "presentation.h", HERE / "presentation_runtime.h", HERE / "observer_player.h", HERE / "hud_presentation.h",
                 HERE / "live_telemetry.h",
                 HERE / "demo_refresh.h",
                 HERE / "compat_math.cpp", HERE / "live_exports.map", Path(__file__).resolve()):
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
        raise RuntimeError(f"Live build command failed ({result.returncode}): {args[0]}")
    return result.stdout.strip()


def head(path: Path) -> str | None:
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=path, text=True,
                            capture_output=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def cached_plugin(cache: Path) -> Path | None:
    manifest, plugin = cache / "build.json", cache / "libserver.so"
    if not manifest.is_file() or not plugin.is_file() or plugin.is_symlink():
        return None
    try:
        data = json.loads(manifest.read_text())
        if (data["source_fingerprint"] == source_fingerprint()
                and data["upstream_commit"] == COMMIT and data["live_abi"] == 1
                and data["sha256"] == digest(plugin)
                and b"CS2_CAMERA_LIVE_ABI 1" in plugin.read_bytes()):
            return plugin
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def prepare_source(cache: Path) -> Path:
    source = cache / "upstream"
    if source.is_symlink():
        raise RuntimeError("Live upstream source must not be a symlink")
    donor = NATIVE.parent / ".cache/csdm-motion/upstream"
    if not (source / ".git").is_dir():
        if source.exists() and any(source.iterdir()):
            raise RuntimeError(f"Nonempty unmanaged live source directory: {source}")
        # Clone only immutable git objects. Local donor worktree edits are never
        # copied and all later writes remain under the independent live cache.
        if (donor / ".git").is_dir() and head(donor) == COMMIT:
            run(["git", "clone", "--no-hardlinks", "--no-checkout", str(donor), str(source)])
            run(["git", "checkout", "--detach", COMMIT], source)
        else:
            source.mkdir(parents=True, exist_ok=True)
            run(["git", "init", "-q"], source)
            run(["git", "remote", "add", "origin", REPOSITORY], source)
            run(["git", "fetch", "--depth=1", "origin", COMMIT], source)
            run(["git", "checkout", "--detach", COMMIT], source)
    if head(source) != COMMIT:
        raise RuntimeError(f"Unexpected live upstream commit: {source}")
    # Keep make independent of network when verified dependency clones are
    # already available. The copy includes only immutable tracked files.
    for name, expected in DEPENDENCIES.items():
        dependency = source / RELATIVE / "deps" / name
        local = donor / RELATIVE / "deps" / name
        marker = dependency / ".live-dependency-commit"
        if marker.is_file() and marker.read_text().strip() == expected:
            continue
        if local.is_dir() and head(local) == expected:
            if dependency.exists() and any(dependency.iterdir()):
                raise RuntimeError(f"Unexpected dependency contents: {dependency}")
            dependency.mkdir(parents=True, exist_ok=True)
            archive = subprocess.Popen(["git", "archive", "HEAD"], cwd=local, stdout=subprocess.PIPE)
            try:
                subprocess.run(["tar", "-xf", "-", "-C", str(dependency)], stdin=archive.stdout, check=True)
            finally:
                if archive.stdout:
                    archive.stdout.close()
            if archive.wait():
                raise RuntimeError(f"Cannot archive verified {name} dependency")
            marker.write_text(expected + "\n")
        else:
            run(["git", "submodule", "update", "--init", "--depth=1", "--", str(RELATIVE / "deps" / name)], source)
            if head(dependency) != expected:
                raise RuntimeError(f"Unexpected {name} dependency commit")
    return source


def apply_patches(source: Path) -> None:
    # Materialize main.cpp from pinned git, then apply both known patches. A
    # manifest identifies this directory as managed; unrelated tracked edits
    # elsewhere are rejected instead of reset.
    relative = str(RELATIVE / "main.cpp")
    marker = source / ".live-main-sha256"
    main = source / relative
    if marker.is_file() and digest(main) != marker.read_text().strip():
        raise RuntimeError("Managed live main.cpp has unrelated edits; use a fresh live cache")
    status = subprocess.run(["git", "diff", "--name-only"], cwd=source, text=True,
                            capture_output=True, check=True).stdout.splitlines()
    allowed = {relative, "static/cs2/libserver.so", *(str(RELATIVE / "deps" / name) for name in DEPENDENCIES)}
    if any(path not in allowed for path in status):
        raise RuntimeError("Live checkout has unrelated tracked changes")
    original = subprocess.check_output(["git", "show", f"{COMMIT}:{relative}"], cwd=source)
    (source / relative).write_bytes(original)
    for patch in (NATIVE / "csdm-motion.patch", HERE / "csdm-live.patch"):
        run(["git", "apply", "--check", str(patch)], source)
        run(["git", "apply", str(patch)], source)
    for path in (NATIVE / "motion_track.h", NATIVE / "linux_view_patch.h", NATIVE / "capture_environment.h", NATIVE / "capture_cosmetics.h", HERE / "live_controller.h", HERE / "scene_environment.h", HERE / "team_palette.h", HERE / "cosmetics_catalog.h", HERE / "player_cosmetics.h", HERE / "cosmetics_runtime.h", HERE / "presentation.h", HERE / "presentation_runtime.h", HERE / "observer_player.h", HERE / "hud_presentation.h", HERE / "live_telemetry.h", HERE / "demo_refresh.h", HERE / "compat_math.cpp", HERE / "live_exports.map"):
        shutil.copyfile(path, source / RELATIVE / path.name)
    marker.write_text(digest(main) + "\n")


def ensure_live_plugin(cache_dir: Path | str | None = None) -> Path:
    cache = Path(cache_dir) if cache_dir else NATIVE.parent / ".cache/csdm-live"
    cache = cache.resolve()
    capture = (NATIVE.parent / ".cache/csdm-motion").resolve()
    if cache == capture or capture in cache.parents:
        raise RuntimeError("Live cache must be separate from the batch capture cache")
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / "build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        existing = cached_plugin(cache)
        if existing:
            return existing
        fingerprint = source_fingerprint()
        for command in ("git", "g++", "make", "tar"):
            if shutil.which(command) is None:
                raise RuntimeError(f"Native live build requires {command}")
        source = prepare_source(cache)
        apply_patches(source)
        run(["make", "build", "CXXFLAGS=" + " ".join(BUILD_FLAGS),
             "SRC_FILES=main.cpp compat_math.cpp ./deps/easywsclient/easywsclient.cpp ./deps/hl2sdk/tier1/convar.cpp"], source / RELATIVE)
        built = source / "static/cs2/libserver.so"
        if source_fingerprint() != fingerprint:
            raise RuntimeError("Native live sources changed during compilation; rebuild the current sources")
        binary = built.read_bytes()
        if not binary.startswith(b"\x7fELF\x02\x01") or b"CS2_CAMERA_LIVE_ABI 1" not in binary:
            raise RuntimeError("Compiled library is not the expected ELF64 live plugin")
        # The host compiler can silently raise the runtime minimum when glibc
        # adds a new default version to an existing math function.
        versions = {(int(major), int(minor)) for major, minor in re.findall(rb"GLIBC_(\d+)\.(\d+)", binary)}
        if versions and max(versions) > (2, 38):
            raise RuntimeError(f"Live plugin requires glibc {max(versions)}; supported baseline is 2.38")
        destination = cache / "libserver.so"
        temporary = cache / "libserver.so.tmp"
        shutil.copy2(built, temporary)
        temporary.chmod(0o755)
        os.replace(temporary, destination)
        manifest = {
            "live_abi": 1, "motion_abi": 3, "upstream_commit": COMMIT,
            "dependencies": DEPENDENCIES, "source_fingerprint": fingerprint,
            "sha256": digest(destination),
            "compiler": run(["g++", "--version"]).splitlines()[0],
        }
        (cache / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
        shutil.copy2(source / "LICENSE", cache / "UPSTREAM-LICENSE")
        return destination


build = ensure_live_plugin


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path)
    options = parser.parse_args()
    try:
        print(ensure_live_plugin(options.cache_dir))
    except Exception as error:
        print(f"Native live build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
