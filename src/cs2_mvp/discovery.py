from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request

def resource_root() -> Path:
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parents[2]))


def capture_root() -> Path:
    if getattr(sys, "frozen", False):
        return Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")).expanduser().absolute() / "cs2-camera/capture"
    return resource_root()


ROOT = capture_root()
CSDM_COMMIT = "7270bc95c8cab87b7f9ddd490fea13c6eda3ace7"
PLUGIN_SHA256 = "c6bab67a072f83d7ec8e57c1fe243179e267bd9631cb4ee2d9cbf192cea86780"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def external_environment() -> dict[str, str]:
    """Restore host library lookup for tools launched by a frozen app."""
    environment = os.environ.copy()
    if not getattr(sys, "frozen", False):
        return environment
    original = environment.get("LD_LIBRARY_PATH_ORIG")
    if original is None:
        environment.pop("LD_LIBRARY_PATH", None)
    else:
        environment["LD_LIBRARY_PATH"] = original
    for name in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "PYTHONHOME", "CS2_CAMERA_LIVE",
                 "CS2_CAMERA_SOCKET", "CS2_CAMERA_OVERLAY", "CS2_CAMERA_PROJECT", "CS2_CAMERA_HOTKEYS"):
        environment.pop(name, None)
    # A capture is separate from the in-game editing overlay. Preserve other
    # user overlays while removing only this application's injected library/layer.
    if "LD_PRELOAD" in environment:
        entries = re.split(r"[ :]+", environment["LD_PRELOAD"])
        environment["LD_PRELOAD"] = ":".join(value for value in entries if value and "libcs2_camera_overlay.so" not in value)
        if not environment["LD_PRELOAD"]:
            environment.pop("LD_PRELOAD")
    if "VK_INSTANCE_LAYERS" in environment:
        environment["VK_INSTANCE_LAYERS"] = ":".join(value for value in environment["VK_INSTANCE_LAYERS"].split(":") if value != "VK_LAYER_CS2_camera")
    for name in list(environment):
        if name.startswith("_PYI_"):
            environment.pop(name, None)
    return environment


def bundled_capture_manifest() -> dict:
    path = resource_root() / "native/capture/plugins.json"
    try:
        value = json.loads(path.read_text())
        if not isinstance(value, dict) or value.get("version") != 1:
            raise ValueError("Unsupported capture bundle version")
        return value
    except (OSError, ValueError) as error:
        raise RuntimeError(f"Cannot verify bundled capture plugins: {path}: {error}") from error


def verify_bundled_library(filename: str, expected_hash: str, marker: bytes | None = None) -> Path:
    path = resource_root() / "native/capture" / filename
    if path.name != filename or not isinstance(expected_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
        raise RuntimeError("Invalid bundled native plugin manifest")
    try:
        binary = path.read_bytes()
    except OSError as error:
        raise RuntimeError(f"Bundled native plugin is missing: {path}") from error
    if not binary.startswith(b"\x7fELF\x02\x01") or (marker and marker not in binary) or sha256(path) != expected_hash:
        raise RuntimeError(f"Bundled native plugin failed ELF/ABI/hash verification: {path}")
    return path


def validate_media_tools() -> dict[str, str]:
    tools = {name: shutil.which(name) for name in ("ffmpeg", "ffprobe")}
    if not all(tools.values()):
        raise RuntimeError("Install system ffmpeg and ffprobe before capturing or finalizing a video")
    result = subprocess.run([tools["ffmpeg"], "-hide_banner", "-encoders"], env=external_environment(),
                            capture_output=True, text=True, timeout=15, check=True)
    if not re.search(r"\blibx264\b", result.stdout) or not re.search(r"\baac\b", result.stdout):
        raise RuntimeError("System ffmpeg must provide the libx264 and AAC encoders")
    return tools


def steam_roots() -> list[Path]:
    roots: list[Path] = []
    for candidate in [Path.home() / ".local/share/Steam", Path.home() / ".steam/steam"]:
        if candidate.exists() and candidate.resolve() not in roots:
            roots.append(candidate.resolve())
    for steam in roots[:]:
        vdf = steam / "steamapps/libraryfolders.vdf"
        if vdf.exists():
            for path in re.findall(r'"path"\s*"([^"]+)"', vdf.read_text()):
                candidate = Path(path.replace("\\\\", "\\"))
                if candidate.exists() and candidate.resolve() not in roots:
                    roots.append(candidate.resolve())
    return roots


def find_game(explicit: str | None = None) -> Path:
    candidates = [Path(explicit).expanduser()] if explicit else [
        root / "steamapps/common/Counter-Strike Global Offensive/game" for root in steam_roots()
    ]
    for candidate in candidates:
        if (candidate / "game/cs2.sh").exists():
            candidate /= "game"
        if (candidate / "cs2.sh").exists() and (candidate / "csgo/gameinfo.gi").exists():
            return candidate.resolve()
    raise RuntimeError("Native CS2 not found; pass --game-dir /path/to/install/game")


def find_runtime() -> Path:
    for root in steam_roots():
        runtime = root / "steamapps/common/SteamLinuxRuntime_sniper/_v2-entry-point"
        if runtime.is_file():
            return runtime
    raise RuntimeError("Steam Linux Runtime sniper is not installed")


def process_ids(name: str) -> list[int]:
    found = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                state = (entry / "stat").read_text().rsplit(")", 1)[1].split()[0]
                if state != "Z" and (entry / "comm").read_text().strip() == name:
                    found.append(int(entry.name))
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                pass
    return found


def doctor(explicit: str | None = None) -> dict:
    game = find_game(explicit)
    version = dict(line.split("=", 1) for line in (game / "csgo/steam.inf").read_text().splitlines() if "=" in line)
    manifest = game.parents[2] / "appmanifest_730.acf"
    build = re.search(r'"buildid"\s*"(\d+)"', manifest.read_text()) if manifest.exists() else None
    return {
        "game_directory": str(game), "runtime": str(find_runtime()),
        "patch_version": version.get("PatchVersion"), "build_id": build.group(1) if build else None,
        "steam_running": bool(process_ids("steam")), "cs2_pids": process_ids("cs2"),
        "display": os.environ.get("DISPLAY"), "session_type": os.environ.get("XDG_SESSION_TYPE"),
        "ffmpeg": shutil.which("ffmpeg"), "ffprobe": shutil.which("ffprobe"),
        "game_free_bytes": shutil.disk_usage(game).free,
        "demos": [str(p) for p in sorted((game / "csgo").rglob("*.dem"), key=lambda p: p.stat().st_mtime, reverse=True)],
        "plugin_commit": CSDM_COMMIT, "plugin_sha256": PLUGIN_SHA256,
    }


def ensure_plugin() -> Path:
    if getattr(sys, "frozen", False):
        manifest = bundled_capture_manifest()
        if manifest.get("stock_sha256") != PLUGIN_SHA256:
            raise RuntimeError("Bundled stock CSDM plugin pin does not match")
        return verify_bundled_library("libserver_stock.so", PLUGIN_SHA256)
    destination = ROOT / ".cache/csdm" / CSDM_COMMIT
    destination.mkdir(parents=True, exist_ok=True)
    plugin = destination / "libserver.so"
    if plugin.exists() and sha256(plugin) == PLUGIN_SHA256:
        return plugin
    base = f"https://raw.githubusercontent.com/akiver/cs-demo-manager/{CSDM_COMMIT}"
    temporary = plugin.with_suffix(".download")
    try:
        with urllib.request.urlopen(f"{base}/static/cs2/libserver.so", timeout=60) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        if sha256(temporary) != PLUGIN_SHA256:
            raise RuntimeError("Downloaded CSDM plugin checksum mismatch")
        with urllib.request.urlopen(f"{base}/LICENSE", timeout=30) as response:
            (destination / "LICENSE").write_bytes(response.read())
        temporary.replace(plugin)
        plugin.chmod(0o755)
    finally:
        temporary.unlink(missing_ok=True)
    return plugin
