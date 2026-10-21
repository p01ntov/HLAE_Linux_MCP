from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys
import urllib.request
from itertools import islice
from functools import lru_cache

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
    frozen = getattr(sys, "frozen", False)
    if frozen:
        original = environment.get("LD_LIBRARY_PATH_ORIG")
        if original is None:
            environment.pop("LD_LIBRARY_PATH", None)
        else:
            environment["LD_LIBRARY_PATH"] = original
        for name in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "PYTHONHOME"):
            environment.pop(name, None)
    for name in ("CS2_CAMERA_LIVE", "CS2_CAMERA_FULLSCREEN_WINDOWED",
                 "CS2_CAMERA_SOCKET", "CS2_CAMERA_OVERLAY", "CS2_CAMERA_PROJECT", "CS2_CAMERA_HOTKEYS", "CS2_CAMERA_DEMO_PATH", "CS2_CAMERA_COSMETICS_CATALOG", "CS2_CAMERA_COSMETICS_CATALOG_ERROR", "CS2_CAMERA_CAPTURE_COSMETICS"):
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
        if frozen and name.startswith("_PYI_"):
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


def _bounded_children(directory: Path, maximum=128) -> list[Path]:
    try:
        return [path for path in islice(directory.iterdir(),maximum) if path.is_dir()]
    except (OSError,RuntimeError):
        return []


def _mounted_library_roots() -> list[Path]:
    """Inspect mount points and conventional folders, never recurse through disks."""
    candidates=[]
    try:
        with Path("/proc/self/mountinfo").open() as handle:
            for line in islice(handle,4096):
                fields=line.split()
                if len(fields)>5:
                    path=Path(re.sub(r"\\([0-7]{3})",lambda m:chr(int(m[1],8)),fields[4]))
                    if str(path).startswith(("/mnt/","/media/","/run/media/")):
                        candidates.append(path)
    except OSError:
        pass
    for base in (Path("/mnt"),Path("/media"),Path("/run/media")):
        children=_bounded_children(base)
        candidates.extend(children)
        if base!=Path("/mnt"):
            for child in children:
                candidates.extend(_bounded_children(child,32))
    roots=[]
    seen=set()
    for disk in candidates[:256]:
        for candidate in (disk,disk/"SteamLibrary",disk/"Steam",disk/"steam",disk/"Games/SteamLibrary",disk/"Games/Steam",*_bounded_children(disk,64)):
            try:
                if (candidate/"steamapps").is_dir():
                    resolved=candidate.resolve()
                    if resolved not in seen:
                        roots.append(resolved);seen.add(resolved)
            except (OSError,RuntimeError):
                continue
    return roots


def steam_roots() -> list[Path]:
    roots: list[Path] = []
    candidates=[Path.home()/".local/share/Steam",Path.home()/".steam/steam",Path.home()/".steam/root",
                Path.home()/".var/app/com.valvesoftware.Steam/.local/share/Steam",*_mounted_library_roots()]
    for candidate in candidates:
        try:
            resolved=candidate.resolve()
            if candidate.is_dir() and resolved not in roots:
                roots.append(resolved)
        except (OSError,RuntimeError):
            continue
    # Library files also exist in library roots. Follow a bounded queue so a
    # registered external disk can point to another existing Steam library.
    for steam in roots:
        if len(roots)>=128:
            break
        vdf=steam/"steamapps/libraryfolders.vdf"
        try:
            if not vdf.is_file() or vdf.stat().st_size>1024*1024:
                continue
            text=vdf.read_text(encoding="utf-8-sig")
        except (OSError,UnicodeError):
            continue
        for value in re.findall(r'"(?:path|\d+)"\s*"((?:\\.|[^"\\])*)"',text):
            candidate=Path(re.sub(r'\\([\\"])',r'\1',value)).expanduser()
            try:
                if candidate.is_absolute() and candidate.is_dir() and candidate.resolve() not in roots:
                    roots.append(candidate.resolve())
                    if len(roots)>=128:
                        break
            except (OSError,RuntimeError):
                continue
    return roots


def find_games(explicit: str | None = None) -> list[Path]:
    candidates = [Path(explicit).expanduser()] if explicit else [
        root / "steamapps/common/Counter-Strike Global Offensive/game" for root in steam_roots()
    ]
    games=[]
    for candidate in candidates:
        try:
            if (candidate / "game/cs2.sh").is_file():
                candidate /= "game"
            if (candidate / "cs2.sh").is_file() and (candidate / "csgo/gameinfo.gi").is_file() and candidate.resolve() not in games:
                games.append(candidate.resolve())
        except (OSError,RuntimeError):
            continue
    return games


def find_game(explicit: str | None = None) -> Path:
    games=find_games(explicit)
    if games:
        return games[0]
    raise RuntimeError("Native CS2 not found; pass --game-dir /path/to/install/game")


@lru_cache(maxsize=512)
def _demo_map_name(path, size, modified):
    if size<1024*1024:
        return None
    try:
        from .demo import parser
        value=parser(Path(path)).parse_header().get("map_name")
        return value if isinstance(value,str) and 1<=len(value)<=128 else None
    except Exception:
        return None


def _demo_readiness(path, size):
    # PBDEMS2 fixed-header offsets are absolute command positions:
    # https://docs.rs/pbdems2/latest/pbdems2/guide/file_structure/index.html
    with path.open("rb") as handle:
        header=handle.read(16)
        if len(header)!=16 or header[:8]!=b"PBDEMS2\0":
            return False,"Unsupported or incomplete CS2 demo header"
        file_info,spawn_groups=struct.unpack_from("<II",header,8)
        if not 16<=file_info<size or spawn_groups and not 16<=spawn_groups<size:
            return False,"Recording is incomplete: final demo metadata is absent or beyond the downloaded file"
        handle.seek(file_info)
        def varint():
            value=0
            for shift in range(0,35,7):
                byte=handle.read(1)
                if not byte:
                    raise ValueError("Truncated final demo metadata")
                value|=(byte[0]&127)<<shift
                if not byte[0]&128:
                    return value
            raise ValueError("Invalid final demo metadata frame")
        try:
            command=varint();varint();payload_size=varint()
            if command&~64!=2 or handle.tell()+payload_size>size:
                return False,"Recording is incomplete: final demo metadata frame is invalid"
        except ValueError as error:
            return False,str(error)
    return True,""


def discover_demos(game_dirs=None, *, limit=500) -> list[dict]:
    """List local CS2 recordings in game/csgo and downloaded game/csgo/replays."""
    if type(limit) is not int or not 1<=limit<=2000:
        raise ValueError("Demo library limit must be an integer in 1..2000")
    games=find_games() if game_dirs is None else game_dirs
    recordings=[]
    seen=set()
    for game in games[:128]:
        game=Path(game)
        for directory in (game/"csgo/replays",game/"csgo"):
            try:
                entries=list(islice(directory.iterdir(),8192))
            except (OSError,RuntimeError):
                continue
            for candidate in entries:
                if candidate.suffix.lower()!=".dem":
                    continue
                try:
                    path=candidate.resolve(strict=True)
                    if path in seen or not path.is_file():
                        continue
                    info=path.stat()
                    ready,reason=_demo_readiness(path,info.st_size)
                    seen.add(path)
                    recordings.append({"path":str(path),"name":path.stem,"filename":path.name,
                        "game_directory":str(game.resolve()),"modified":info.st_mtime,"modified_ns":info.st_mtime_ns,"size":info.st_size,
                        "downloaded":directory.name=="replays","ready":ready,
                        "reason":reason})
                except (OSError,RuntimeError):
                    continue
    recordings=sorted(recordings,key=lambda item:(-item["modified"],item["path"]))[:limit]
    # Parsing only headers of the latest recordings is cheap and cached by the
    # exact file fingerprint. Event/player analysis belongs to the MCP tools.
    for item in recordings[:64]:
        if item["ready"]:
            item["map_name"]=_demo_map_name(item["path"],item["size"],item["modified_ns"])
    return recordings


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
