"""Reusable export settings and a bounded, durable record of completed jobs.

The store never opens an output, restores a project, or starts a render. Applying
``settings`` to ExportOptions leaves destination and the current project range
to the caller. Every mutation reloads under a lock. GUI mutations additionally
compare the preset the user saw, protecting edits made in another window.
"""
from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import errno
import fcntl
import json
import math
import os
from pathlib import Path
import stat
import time
import uuid

from .render import resolve_capture_fps, resolve_video_encoding, validate_resolution


MAX_PRESETS = 16
MAX_HISTORY = 32
MAX_STORE_BYTES = 512 * 1024
_ANY_PRESET = object()


class PresetConflictError(ValueError):
    """The preset changed after the caller last saw or confirmed it."""


DEFAULT_SETTINGS = {
    "width": 2560, "height": 1440, "capture_fps": 120, "fps": 60,
    "slow_motion": False, "audio": True, "keep_raw": False, "reopen": True,
    "export_format": "mp4", "crf": None, "video_bitrate_mbps": None,
}
_HISTORY_FIELDS = {
    "id", "status", "started_at", "finished_at", "elapsed_seconds",
    "project_name", "demo", "snapshot", "output", "settings", "start_tick",
    "end_tick", "source_duration", "output_duration", "frames", "error",
}


def configuration_path() -> Path:
    configured = os.environ.get("CS2_CAMERA_EXPORT_PRESETS")
    path = (Path(configured).expanduser() if configured else
            Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "cs2-camera/exports.json")
    return _absolute_path(path)


def _absolute_path(path) -> Path:
    path = Path(path)
    if not path.is_absolute() or not path.name:
        raise ValueError("Export settings require an absolute configuration file path")
    return path


def validate_settings(settings) -> dict:
    """Validate reusable fields using the same limits as the renderer."""
    if not isinstance(settings, dict):
        raise ValueError("Export settings must be an object")
    unknown = set(settings) - set(DEFAULT_SETTINGS)
    if unknown:
        raise ValueError("Unknown export setting: " + str(next(iter(unknown))))
    values = {**DEFAULT_SETTINGS, **settings}
    for name in ("slow_motion", "audio", "keep_raw", "reopen"):
        if type(values[name]) is not bool:
            raise ValueError(name + " must be boolean")
    try:
        validate_resolution(values["width"], values["height"])
        values["capture_fps"] = resolve_capture_fps(values["fps"], values["capture_fps"], True)
        resolve_video_encoding(values["export_format"], values["crf"], values["video_bitrate_mbps"])
    except OverflowError as error:
        raise ValueError("Export settings contain a number outside the renderer limits") from error
    if values["export_format"] == "png" and values["fps"] > 999:
        raise ValueError("PNG interpretation FPS must be at most 999")
    return values


def settings_from_options(options) -> dict:
    """Extract settings, deliberately omitting output and project range/scope."""
    if isinstance(options, dict):
        return validate_settings({name: options[name] for name in DEFAULT_SETTINGS if name in options})
    try:
        return validate_settings({name: getattr(options, name) for name in DEFAULT_SETTINGS})
    except AttributeError as error:
        raise ValueError("Export options are missing reusable settings") from error


def _text(value, name, maximum, *, allow_empty=False, multiline=False):
    if not isinstance(value, str) or (not allow_empty and not value.strip()):
        raise ValueError(name + " must be text")
    if len(value.encode("utf-8")) > maximum or any(
            ord(char) < 32 and not (multiline and char in "\n\t") for char in value):
        raise ValueError(name + " contains invalid text or is too long")
    return value


def _preset_name(name):
    return _text(name, "Preset name", 256).strip()


def _path_reference(value, name, *, optional=False):
    if value is None and optional:
        return None
    value = _text(value, name, 4096)
    if not Path(value).is_absolute():
        raise ValueError(name + " must be an absolute path")
    return value


def _timestamp(value, name, *, optional=False):
    if value is None and optional:
        return None
    value = _text(value, name, 64)
    try:
        parsed = datetime.fromisoformat(value)
        if parsed.tzinfo is None or parsed.utcoffset() is None:
            raise ValueError("timezone missing")
        return parsed.astimezone(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")
    except (ValueError, OverflowError) as error:
        raise ValueError(name + " must be an ISO timestamp with timezone") from error


def _number(value, name, maximum, *, integer=False):
    if value is None:
        return None
    if (type(value) not in (int, float) or integer and type(value) is not int
            or not 0 <= value <= maximum or not math.isfinite(value)):
        raise ValueError(name + " has an invalid numeric type or range")
    return value


def validate_history_record(record) -> dict:
    if not isinstance(record, dict) or set(record) != _HISTORY_FIELDS:
        raise ValueError("Invalid export history record")
    values = dict(record)
    identifier = _text(values["id"], "History id", 32)
    if len(identifier) != 32 or any(char not in "0123456789abcdef" for char in identifier):
        raise ValueError("Invalid export history id")
    if values["status"] not in ("success", "failed", "cancelled"):
        raise ValueError("Invalid export history status")
    values["started_at"] = _timestamp(values["started_at"], "Start time", optional=True)
    values["finished_at"] = _timestamp(values["finished_at"], "Finish time")
    if values["started_at"] and values["started_at"] > values["finished_at"]:
        raise ValueError("Export finish time precedes its start time")
    values["elapsed_seconds"] = _number(values["elapsed_seconds"], "Elapsed seconds", 7 * 86400)
    values["project_name"] = _text(values["project_name"], "Project name", 1024, allow_empty=True)
    for name in ("demo", "snapshot", "output"):
        values[name] = _path_reference(values[name], name, optional=name != "output")
    values["settings"] = validate_settings(values["settings"])
    for name in ("start_tick", "end_tick"):
        values[name] = _number(values[name], name, 2_000_000_000, integer=True)
    if ((values["start_tick"] is None) != (values["end_tick"] is None)
            or values["start_tick"] is not None and values["end_tick"] <= values["start_tick"]):
        raise ValueError("Export history needs a complete, increasing tick range")
    values["source_duration"] = _number(values["source_duration"], "Source duration", 300)
    # Capture validation permits two boundary samples beyond the requested
    # duration. Do not reject an otherwise valid maximum-length result here.
    values["output_duration"] = _number(values["output_duration"], "Output duration", 7 * 86400)
    values["frames"] = _number(values["frames"], "Frame count", 2_000_000_000, integer=True)
    if values["error"] is not None:
        values["error"] = _text(values["error"], "Export error", 8192, multiline=True)
    if values["status"] == "success" and values["error"] is not None:
        raise ValueError("A failed export cannot be marked successful")
    if values["status"] != "success" and any(values[name] is not None for name in ("output_duration", "frames")):
        raise ValueError("An unfinished export cannot report completed media")
    return values


def _now():
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def _reference(value):
    return str(Path(value).expanduser().absolute()) if value is not None else None


def make_history_record(options, outcome, *, project_name="", demo=None,
                        started_at=None, elapsed_seconds=None, finished_at=None) -> dict:
    """Summarize the renderer's outcome; a plan alone is not a completed export."""
    if not isinstance(outcome, dict):
        raise ValueError("Export outcome must be an object")
    plan = outcome.get("plan") if isinstance(outcome.get("plan"), dict) else {}
    snapshot = outcome.get("snapshot") if isinstance(outcome.get("snapshot"), dict) else {}
    error = outcome.get("error") or plan.get("error")
    cancelled = bool(outcome.get("cancelled")) or plan.get("status") == "cancelled"
    success = not cancelled and not error and plan.get("status") == "done"
    status = "cancelled" if cancelled else "success" if success else "failed"
    if not success and not error:
        error = "Export cancelled" if cancelled else "Export did not finish"
    media = plan.get("media") if success and isinstance(plan.get("media"), dict) else {}
    output = options.get("output") if isinstance(options, dict) else options.output
    record = {
        "id": uuid.uuid4().hex, "status": status, "started_at": started_at,
        "finished_at": finished_at if finished_at is not None else _now(),
        "elapsed_seconds": elapsed_seconds, "project_name": project_name,
        "demo": _reference(demo), "snapshot": _reference(snapshot.get("path")),
        "output": _reference(plan.get("output", output) if success else output),
        "settings": settings_from_options(options), "start_tick": plan.get("start_tick"),
        "end_tick": plan.get("end_tick"), "source_duration": plan.get("expected_duration"),
        "output_duration": media.get("duration", plan.get("expected_output_duration")) if success else None,
        "frames": media.get("frames"), "error": str(error) if error else None,
    }
    # Expected frames/duration in a failed plan are never shown as actual output.
    return validate_history_record(record)


def _empty_store():
    return {"version": 1, "last_used": None, "presets": [], "history": []}


def _validate_store(document):
    if (not isinstance(document, dict) or set(document) != {"version", "last_used", "presets", "history"}
            or type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported export settings document")
    result = _empty_store()
    if document["last_used"] is not None:
        result["last_used"] = validate_settings(document["last_used"])
    presets, history = document["presets"], document["history"]
    if not isinstance(presets, list) or len(presets) > MAX_PRESETS:
        raise ValueError("Export settings support at most 16 named presets")
    names = set()
    for preset in presets:
        if not isinstance(preset, dict) or set(preset) != {"name", "settings"}:
            raise ValueError("Invalid named export preset")
        name = _preset_name(preset["name"])
        if name.casefold() in names:
            raise ValueError("Duplicate export preset name")
        names.add(name.casefold())
        result["presets"].append({"name": name, "settings": validate_settings(preset["settings"])})
    if not isinstance(history, list) or len(history) > MAX_HISTORY:
        raise ValueError("Export history supports at most 32 records")
    identities = set()
    for record in history:
        record = validate_history_record(record)
        if record["id"] in identities:
            raise ValueError("Duplicate export history id")
        identities.add(record["id"])
        result["history"].append(record)
    return result


def _object(pairs):
    value = {}
    for name, item in pairs:
        if name in value:
            raise ValueError("Duplicate key in export settings: " + name)
        value[name] = item
    return value


def _invalid_constant(value):
    raise ValueError("Non-finite value in export settings: " + value)


def _identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _read(directory, name):
    try:
        descriptor = os.open(name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    except FileNotFoundError:
        return _empty_store(), None
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise ValueError("Export settings file must not be a symlink") from error
        raise
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_STORE_BYTES:
            raise ValueError("Export settings must be a regular file no larger than 512 KiB")
        data = stream.read(MAX_STORE_BYTES + 1)
        after = os.fstat(stream.fileno())
    if len(data) > MAX_STORE_BYTES:
        raise ValueError("Export settings file exceeds 512 KiB")
    if _identity(info) != _identity(after):
        raise ValueError("Export settings changed while reading; try again")
    try:
        document = json.loads(data.decode("utf-8"), object_pairs_hook=_object, parse_constant=_invalid_constant)
    except (UnicodeError, RecursionError) as error:
        raise ValueError("Invalid export settings document") from error
    return _validate_store(document), _identity(after)


def load_export_store(path=None) -> dict:
    """Return fresh data, or defaults when absent. Invalid files stay untouched."""
    path = _absolute_path(path) if path is not None else configuration_path()
    try:
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    except FileNotFoundError:
        return _empty_store()
    try:
        return _read(directory, path.name)[0]
    finally:
        os.close(directory)


@contextmanager
def _locked(directory, name):
    try:
        descriptor = os.open("." + name + ".lock", os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
                             0o600, dir_fd=directory)
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise ValueError("Export settings lock must not be a symlink") from error
        raise
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("Export settings lock must be a regular file")
        deadline = time.monotonic() + 2
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise OSError("Export settings are busy; try again")
                time.sleep(.01)
        yield
    finally:
        os.close(descriptor)


def _write(directory, name, document, previous):
    data = (json.dumps(document, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    if len(data) > MAX_STORE_BYTES:
        raise ValueError("Export settings file exceeds 512 KiB")
    temporary = ".exports-" + uuid.uuid4().hex + ".tmp"
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600, dir_fd=directory)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            current = _identity(os.stat(name, dir_fd=directory, follow_symlinks=False))
        except FileNotFoundError:
            current = None
        if current != previous:
            raise ValueError("Export settings changed while saving; try again")
        os.replace(temporary, name, src_dir_fd=directory, dst_dir_fd=directory)
        os.fsync(directory)
    finally:
        try:
            os.unlink(temporary, dir_fd=directory)
        except FileNotFoundError:
            pass


def _update(change, path):
    path = _absolute_path(path) if path is not None else configuration_path()
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        with _locked(directory, path.name):
            document, previous = _read(directory, path.name)
            changed = change(document)
            result = _validate_store(changed)
            _write(directory, path.name, result, previous)
            return result
    finally:
        os.close(directory)


def save_last_used(settings, path=None) -> dict:
    values = validate_settings(settings)
    return _update(lambda document: {**document, "last_used": values}, path)


def _current_preset(document, name, expected):
    current = next((preset for preset in document["presets"] if preset["name"].casefold() == name.casefold()), None)
    if expected is not _ANY_PRESET and current != expected:
        raise PresetConflictError("Preset changed in another window; review the refreshed presets and try again")
    return current


def save_preset(name, settings, path=None, *, expected=_ANY_PRESET) -> dict:
    """Save by name; expected=None requires a new name, a dict confirms its version."""
    name, values = _preset_name(name), validate_settings(settings)

    def change(document):
        preset = _current_preset(document, name, expected)
        if preset is not None:
            preset.update(name=name, settings=values)
        else:
            if len(document["presets"]) >= MAX_PRESETS:
                raise ValueError("All 16 export preset slots are used; delete a preset first")
            document["presets"].append({"name": name, "settings": values})
        return document

    return _update(change, path)


def delete_preset(name, path=None, *, expected=_ANY_PRESET) -> dict:
    """Delete by name, optionally requiring the version confirmed by the caller."""
    name = _preset_name(name).casefold()

    def change(document):
        _current_preset(document, name, expected)
        return {**document, "presets": [preset for preset in document["presets"] if preset["name"].casefold() != name]}

    return _update(change, path)


def append_history(record, path=None) -> dict:
    record = validate_history_record(record)

    def change(document):
        for existing in document["history"]:
            if existing["id"] == record["id"]:
                if existing != record:
                    raise ValueError("An export history record cannot be rewritten")
                return document
        document["history"] = sorted([record, *document["history"]],
                                     key=lambda item: item["finished_at"], reverse=True)[:MAX_HISTORY]
        return document

    return _update(change, path)
