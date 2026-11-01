"""Host-selected durable project folder, independent from build/session caches."""
from __future__ import annotations

import json
import os
from pathlib import Path


def configuration_path() -> Path:
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "cs2-camera/storage.json"


def geometry_directory() -> Path | None:
    """Resolve a user-selected read-only geometry cache; never create one in Home."""
    selected = os.environ.get("CS2_CAMERA_GEOMETRY_CACHE")
    if not selected:
        config = configuration_path()
        if config.exists():
            if config.stat().st_size > 16384:
                raise ValueError("Project storage configuration exceeds 16 KiB")
            data = json.loads(config.read_text(encoding="utf-8"))
            if not isinstance(data, dict) or type(data.get("version", 1)) is not int or data.get("version", 1) != 1:
                raise ValueError("Unsupported project storage configuration")
            selected = data.get("geometry_directory")
    if selected is None:
        return None
    if not isinstance(selected, str) or not selected or "\0" in selected:
        raise ValueError("geometry_directory must be an absolute directory path")
    path = Path(selected).expanduser()
    if not path.is_absolute():
        raise ValueError("geometry_directory must be an absolute directory path")
    return path.resolve()


def projects_directory(explicit: Path | str | None = None) -> Path:
    selected = explicit or os.environ.get("CS2_CAMERA_PROJECTS_DIR")
    if selected is None:
        config = configuration_path()
        if config.exists():
            if config.stat().st_size > 16384:
                raise ValueError("Project storage configuration exceeds 16 KiB")
            try:
                data = json.loads(config.read_text(encoding="utf-8"))
                if (not isinstance(data, dict) or type(data.get("version", 1)) is not int
                        or data.get("version", 1) != 1):
                    raise ValueError("Unsupported project storage configuration")
                selected = data.get("projects_directory")
                if selected is not None and (not isinstance(selected, str) or not selected or "\0" in selected):
                    raise ValueError("projects_directory must be an absolute directory path")
            except (OSError, json.JSONDecodeError) as error:
                raise ValueError(f"Cannot read project storage configuration: {config}: {error}") from error
    if selected is None:
        path = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "cs2-camera/projects"
    else:
        path = Path(selected).expanduser()
        if not path.is_absolute():
            raise ValueError("Project folder must use an absolute path")
    path.mkdir(parents=True, mode=0o700, exist_ok=True)
    path = path.resolve(strict=True)
    if not path.is_dir() or not os.access(path, os.W_OK | os.X_OK):
        raise ValueError(f"Project folder is not writable: {path}")
    return path
