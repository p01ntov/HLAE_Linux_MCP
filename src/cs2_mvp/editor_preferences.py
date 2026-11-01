"""Typed editor-only preferences, persisted outside the game's frame thread."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import stat
import tempfile

DEFAULTS = {"time_unit": "seconds", "frame_rate": 60, "snap_mode": "tick", "resize_mode": "keep",
            "compact_transport": True, "ui_scale": 1.0, "inspector_width": 430, "panel_opacity": .94,
            "blur_enabled": True, "show_key_labels": True, "show_tooltips": True,
            "timeline_position": True, "timeline_rotation": True, "timeline_fov": True, "timeline_height": 240,
            "workspace_source": "camera", "workspace_tool": "timeline", "workspace_open": True,
            "inspector_section": "camera", "demo_navigation_pinned": False}
ENUMS = {"time_unit": {"seconds", "ticks", "frames"}, "snap_mode": {"tick", "frame", "off"},
         "resize_mode": {"keep", "scale"}, "workspace_source": {"demo", "camera"},
         "workspace_tool": {"timeline", "map", "curves"}, "inspector_section": {"camera", "shot", "scene"}}
RANGES = {"frame_rate": (1, 1000), "ui_scale": (.75, 1.5), "inspector_width": (320, 640),
          "panel_opacity": (.3, 1), "timeline_height": (160, 420)}


def configuration_path():
    configured = os.environ.get("CS2_CAMERA_EDITOR_PREFS")
    path = Path(configured).expanduser() if configured else Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "cs2-camera/editor.json"
    if not path.is_absolute():
        raise ValueError("Editor preferences require an absolute configuration path")
    return path


def validate_preferences(patch, base=None):
    result = dict(DEFAULTS)
    for fields in (base if base is not None else {}, patch):
        if not isinstance(fields, dict):
            raise ValueError("Editor preferences must be a flat object")
        for name, value in fields.items():
            if name not in DEFAULTS:
                raise ValueError("Unknown editor preference: " + str(name))
            default = DEFAULTS[name]
            if type(default) is bool:
                if type(value) is not bool:
                    raise ValueError(name + " must be boolean")
            elif name in ENUMS:
                if not isinstance(value, str) or value not in ENUMS[name]:
                    raise ValueError("Invalid editor preference enum: " + name)
            else:
                low, high = RANGES[name]
                if (isinstance(value, bool) or not isinstance(value, (int, float))
                        or type(default) is int and type(value) is not int
                        or not math.isfinite(value) or not low <= value <= high):
                    raise ValueError(name + " has an invalid numeric type or range")
            result[name] = value
    return result


def load_preferences(path=None):
    path = Path(path) if path is not None else configuration_path()
    if not path.is_absolute():
        raise ValueError("Editor preferences require an absolute configuration path")
    try:
        info = path.lstat()
    except FileNotFoundError:
        return dict(DEFAULTS)
    if not stat.S_ISREG(info.st_mode) or info.st_size > 16384:
        raise ValueError("Editor preference file must be a regular file no larger than 16 KiB")
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("Editor preference file must be regular")
        data = stream.read(16385)
    if len(data) > 16384:
        raise ValueError("Editor preference file exceeds 16 KiB")
    document = json.loads(data.decode("utf-8"))
    if (not isinstance(document, dict) or set(document) != {"version", "preferences"}
            or type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported editor preference document")
    return validate_preferences(document["preferences"])


def save_preferences(preferences, path=None):
    values = validate_preferences(preferences)
    path = Path(path) if path is not None else configuration_path()
    if not path.is_absolute():
        raise ValueError("Editor preferences require an absolute configuration path")
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.is_symlink():
        raise ValueError("Editor preference destination must not be a symlink")
    if path.exists() and not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("Editor preference destination must be a regular file")
    data = json.dumps({"version": 1, "preferences": values}, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    descriptor, temporary = tempfile.mkstemp(prefix=".editor-", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)
    return values


def service_preferences(client, handled_revision=0, path=None):
    state = client.request("get_state")
    revision = state.get("editor_preferences_revision")
    if type(revision) is not int or revision < 0:
        raise ValueError("Native editor preference revision is unavailable")
    if state.get("editor_preferences_error"):
        # Loading a malformed/newer file falls back for display only. Retain
        # that file until an explicit user preference edit clears the warning.
        return handled_revision
    if revision <= handled_revision:
        return handled_revision
    save_preferences(state.get("editor_preferences"), path)
    return revision
