"""Bounded, opt-in camera shot collections for one local recording.

Files are read once before launching the game. The immutable private copy is
input to the overlay, not part of portable project files or frame telemetry.
"""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import stat

MAX_BYTES = 1024 * 1024


def validate_library(value, demo: Path):
    from .live_mcp import PROJECT, validate
    from .live_track import normalize_render_range

    if not isinstance(value, dict) or set(value) != {"version", "demo", "shots"}:
        raise ValueError("Shot library requires only version, demo and shots")
    if type(value["version"]) is not int or value["version"] != 1:
        raise ValueError("Unsupported shot library version")
    recording = value["demo"]
    if (not isinstance(recording, str) or "\0" in recording
            or not Path(recording).is_absolute()
            or Path(recording).resolve(strict=True) != demo.resolve(strict=True)):
        raise ValueError("Shot library does not match the selected demo")
    shots = value["shots"]
    if not isinstance(shots, list) or not 1 <= len(shots) <= 64:
        raise ValueError("Shot library requires 1..64 shots")
    identities = set()
    for shot in shots:
        if not isinstance(shot, dict) or set(shot) != {"id", "label", "project"}:
            raise ValueError("Each saved shot requires only id, label and project")
        for key, maximum in (("id", 128), ("label", 256)):
            text = shot[key]
            if not isinstance(text, str) or "\0" in text or not 1 <= len(text.encode("utf-8")) <= maximum:
                raise ValueError(f"Saved shot {key} requires 1..{maximum} UTF-8 bytes")
        if shot["id"] in identities:
            raise ValueError("Duplicate saved shot id")
        identities.add(shot["id"])
        project = shot["project"]
        validate(project, PROJECT, "shot.project")
        if len(project.get("name", "").encode("utf-8")) > 256:
            raise ValueError("Saved project name exceeds 256 UTF-8 bytes")
        duration = project.get("duration", 5)
        normalize_render_range(project.get("render_range", {}), duration)
        keys, ids, times = project["keys"], set(), []
        if not keys:
            raise ValueError("A saved shot needs at least one camera key")
        for key in keys:
            if len(key["id"].encode("utf-8")) > 128 or key["id"] in ids:
                raise ValueError("Saved camera keys need unique ids up to 128 UTF-8 bytes")
            ids.add(key["id"])
            if key["time"] > duration or abs(key["angles"][0]) > 90:
                raise ValueError("Saved camera key exceeds project duration or pitch bounds")
            times.append(key["time"])
        times.sort()
        if any(b - a < .000001 for a, b in zip(times, times[1:])):
            raise ValueError("Saved camera keys need distinct times")
    if len(json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")) > MAX_BYTES:
        raise ValueError("Shot library exceeds 1 MiB")
    return copy.deepcopy(value)


def read_library(path: Path, demo: Path):
    descriptor = os.open(path.expanduser().absolute(), os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= MAX_BYTES:
            raise ValueError("Shot library must be a regular JSON file up to 1 MiB")
        contents = handle.read(MAX_BYTES + 1)
    if len(contents) > MAX_BYTES:
        raise ValueError("Shot library exceeds 1 MiB")
    return validate_library(json.loads(contents), demo)


def stage_library(value, demo_link: Path, run: Path) -> Path:
    staged = copy.deepcopy(value)
    staged["demo"] = str(demo_link)
    path = run / "shot-library.json"
    with path.open("x", encoding="utf-8") as handle:
        os.chmod(path, 0o600)
        json.dump(staged, handle, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
        handle.write("\n")
    return path
