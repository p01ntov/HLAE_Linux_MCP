"""Native Linux CS2 recording through the CS Demo Manager server proxy.

This module only prepares files. It never starts CS2, Steam, or a network server.
The action-file format and installation mechanism follow the pinned upstream
sources linked in docs/csdm-backend.md.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile
from typing import Iterator, Sequence


CSDM_COMMIT = "7270bc95c8cab87b7f9ddd490fea13c6eda3ace7"
PLUGIN_URL = (
    "https://raw.githubusercontent.com/akiver/cs-demo-manager/"
    f"{CSDM_COMMIT}/static/cs2/libserver.so"
)
PLUGIN_SHA256 = "c6bab67a072f83d7ec8e57c1fe243179e267bd9631cb4ee2d9cbf192cea86780"
START_MARKER = "CS2_MVP_RECORD_START"
END_MARKER = "CS2_MVP_RECORD_END"
MIN_START_TICK = 193


class CsdmError(RuntimeError):
    """The native recording environment could not be prepared safely."""


class RestoreConflict(CsdmError):
    """A file changed outside this transaction; its recovery files are retained."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _integer(name: str, value: int, low: int, high: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise ValueError(f"{name} must be an integer from {low} to {high}")


def build_actions(
    start_tick: int,
    end_tick: int,
    steam_id: str,
    movie_name: str,
    fps: int,
    record_audio: bool = True,
    *,
    player_slot: int | None = None,
    camera_only: bool = False,
) -> dict:
    """Return one sequence; write ``[build_actions(...)]`` to ``demo.dem.json``.

    CS2's ``spec_player`` accepts a one-based player slot. Resolve SteamID64 to
    that slot from the demo first; passing a SteamID64 to the command is unsafe.
    The first 96 demo ticks may be skipped by CS2. We require another second of
    lead-in plus 32 ticks of startup slack so seeking and selecting the player
    happen on different frames even when playback skips some early ticks.

    ``record_audio`` is the consumer's encoding preference: native startmovie
    may still write a WAV when it is false. No undocumented sound cvar is used.
    """
    _integer("start_tick", start_tick, MIN_START_TICK, 2**31 - 66)
    _integer("end_tick", end_tick, start_tick + 1, 2**31 - 65)
    _integer("fps", fps, 1, 1000)
    if not isinstance(record_audio, bool):
        raise ValueError("record_audio must be a boolean")
    if not isinstance(camera_only, bool):
        raise ValueError("camera_only must be a boolean")
    if not camera_only and (not isinstance(steam_id, str) or not re.fullmatch(r"[0-9]{17}", steam_id)):
        raise ValueError("steam_id must be a decimal SteamID64 string")
    if not isinstance(movie_name, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", movie_name):
        raise ValueError("movie_name must contain 1-80 ASCII letters, digits, underscores, or hyphens")
    if player_slot is None and not camera_only:
        raise ValueError("Resolve steam_id to a one-based player_slot before building CS2 actions")
    if not camera_only:
        _integer("player_slot", player_slot, 1, 64)

    setup_tick = start_tick - 64
    actions: list[dict] = []

    def command(tick: int, cmd: str) -> None:
        actions.append({"tick": tick, "cmd": cmd})

    for cmd in (
        "sv_cheats 1",
        "demo_ui_mode 0",
        "hideconsole",
        "volume 1",
        "cl_demo_predict 0",
        "spec_autodirector 0",
        "cl_hud_telemetry_frametime_show 0",
        "cl_hud_telemetry_net_misdelivery_show 0",
        "cl_hud_telemetry_ping_show 0",
        "cl_hud_telemetry_serverrecvmargin_graph_show 0",
        "cl_trueview_show_status 0",
        "r_show_build_info 0",
    ):
        command(96, cmd)
    command(96, f"demo_gototick {setup_tick - 1}")
    # Configure the clock/interpolation only after the requested demo has
    # loaded and seeked; keep one second of demo preroll before recording.
    # Source2 HLAE applies cl_interp_ratio=1 after loading to avoid the known
    # zero-ratio recording stutter. This is not the obsolete CS:GO anim fix.
    command(setup_tick, "cl_interp_ratio 1")
    command(setup_tick, "demo_timescale 1")
    command(setup_tick, "host_timescale 1")
    command(setup_tick, "host_force_frametime_to_equal_tick_interval 0")
    command(setup_tick, "host_force_max_frametime_to_tick_interval 0")
    command(setup_tick, f"host_framerate {fps}")
    if not camera_only:
        command(setup_tick, "spec_mode 1")
        command(setup_tick, f"spec_player {player_slot}")
    command(setup_tick, "csdm_info")
    command(start_tick - 4, "pause_playback")
    command(start_tick, f"echo {START_MARKER} {movie_name}")
    command(start_tick, f"startmovie {movie_name}")
    if not camera_only:
        command(start_tick + 4, "getpos_exact")
    command(end_tick, "endmovie")
    command(end_tick, f"echo {END_MARKER} {movie_name}")
    command(end_tick + 64, "quit")
    return {"actions": actions}


def assert_demo_only_launch(launch_args: Sequence[str]) -> Path:
    """Reject an online/server launch before loading the native proxy."""
    args = list(launch_args)
    if "-insecure" not in args or any(x in args for x in ("-secure", "-dedicated")):
        raise CsdmError("The CS:DM proxy requires a local demo launch with -insecure")
    if args.count("+playdemo") != 1:
        raise CsdmError("Exactly one +playdemo argument is required")
    if any(x.startswith("+") and x not in {"+playdemo", "+demo_ui_mode"} for x in args):
        raise CsdmError("Only +playdemo and a fixed demo UI setting are allowed")
    if "+demo_ui_mode" in args:
        ui = args.index("+demo_ui_mode")
        if (args.count("+demo_ui_mode") != 1 or ui + 1 >= len(args)
                or args[ui + 1] != "0" or ui > args.index("+playdemo")):
            raise CsdmError("The optional +demo_ui_mode must be exactly 0 before +playdemo")
    index = args.index("+playdemo")
    if index + 1 >= len(args):
        raise CsdmError("+playdemo requires a local .dem path")
    value = args[index + 1]
    if any(c in value for c in ('\n', '\r', '"', ';')) or "://" in value:
        raise CsdmError("The demo path contains unsupported console syntax")
    demo = Path(value)
    if not demo.is_absolute() or demo.suffix.lower() != ".dem" or not demo.is_file():
        raise CsdmError("+playdemo requires an existing absolute local .dem path")
    return demo.resolve()


@dataclass(frozen=True)
class _Token:
    value: str
    start: int


@dataclass(frozen=True)
class _Entry:
    key: _Token
    value: _Token | list["_Entry"]


_TOKENS = re.compile(r'//[^\r\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]|[^\s{}"]+', re.DOTALL)


def _entries(text: str) -> list[_Entry]:
    tokens = []
    for match in _TOKENS.finditer(text):
        raw = match.group()
        if raw.startswith(("//", "/*")):
            continue
        value = raw[1:-1].replace('\\"', '"') if raw.startswith('"') else raw
        tokens.append(_Token(value, match.start()))

    def parse(index: int, nested: bool = False) -> tuple[list[_Entry], int]:
        entries = []
        while index < len(tokens):
            key = tokens[index]
            if key.value == "}":
                if not nested:
                    raise CsdmError("Unexpected closing brace in gameinfo.gi")
                return entries, index + 1
            if key.value == "{" or index + 1 >= len(tokens):
                raise CsdmError("Malformed gameinfo.gi")
            value = tokens[index + 1]
            if value.value == "{":
                children, index = parse(index + 2, True)
                entries.append(_Entry(key, children))
            else:
                if value.value == "}":
                    raise CsdmError("Missing value in gameinfo.gi")
                entries.append(_Entry(key, value))
                index += 2
        if nested:
            raise CsdmError("Unclosed block in gameinfo.gi")
        return entries, index

    return parse(0)[0]


def patch_gameinfo(original: bytes) -> bytes:
    """Add only the proxy search path, preserving every existing byte otherwise.

    The proxy must be the first Game path: it loads the real Valve server
    directly, bypassing any subsequent Metamod server proxy for this session.
    Existing addon files and search-path entries remain untouched.
    """
    try:
        text = original.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise CsdmError("gameinfo.gi must be UTF-8") from exc
    entries = _entries(text.lstrip("\ufeff"))
    # Token offsets must refer to the original text, including an optional BOM.
    offset = 1 if text.startswith("\ufeff") else 0
    for key in ("gameinfo", "filesystem", "searchpaths"):
        matches = [e for e in entries if e.key.value.casefold() == key and isinstance(e.value, list)]
        if len(matches) != 1:
            raise CsdmError(f"Expected exactly one {key} block in gameinfo.gi")
        entries = matches[0].value
    games = [e for e in entries if e.key.value.casefold() == "game" and isinstance(e.value, _Token)]
    if not any(e.value.value.replace("\\", "/").rstrip("/").casefold() == "csgo" for e in games):
        raise CsdmError("No stock Game csgo search path found")
    existing = [e for e in games if e.value.value.replace("\\", "/").rstrip("/").casefold() == "csgo/csdm"]
    if existing:
        if len(existing) != 1 or existing[0] is not games[0]:
            raise CsdmError("An existing CS:DM search path must be the first Game path")
        return original
    position = games[0].key.start + offset
    line_start = text.rfind("\n", 0, position) + 1
    indent = text[line_start:position]
    if indent.strip():
        # Compact KeyValues is valid too; insert at the token itself.
        indent = ""
    newline = "\r\n" if "\r\n" in text else "\n"
    insertion = f"Game\tcsgo/csdm{newline}{indent}"
    return (text[:position] + insertion + text[position:]).encode("utf-8")


def _regular(path: Path) -> bytes:
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError as exc:
        raise CsdmError(f"Cannot read required regular file {path}: {exc}") from exc
    with os.fdopen(fd, "rb") as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise CsdmError(f"Refusing a symlink or non-regular file: {path}")
        return stream.read()


def _safe_components(path: Path, root: Path) -> None:
    current = root
    for part in path.relative_to(root).parts:
        current /= part
        if current.is_symlink():
            raise CsdmError(f"Refusing symlink in managed game path: {current}")


def _exclusive_file(path: Path, data: bytes, mode: int = 0o600) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode)
    created = os.fstat(fd)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        _sync_directory(path.parent)
    except BaseException:
        # A failed write must not leave an incomplete owned backup/manifest.
        # Do not remove a file another process substituted in the meantime.
        current = path.lstat() if path.exists() else None
        if current is not None and (current.st_dev, current.st_ino) == (created.st_dev, created.st_ino):
            path.unlink()
        raise


def _sync_directory(directory: Path) -> None:
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _replace_file(path: Path, data: bytes, mode: int) -> None:
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "wb") as stream:
            os.fchmod(stream.fileno(), mode)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        _sync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def _assert_game_stopped() -> None:
    proc = Path("/proc")
    if not proc.is_dir():
        raise CsdmError("Linux /proc is required to verify that CS2 is stopped")
    for directory in proc.iterdir():
        if not directory.name.isdigit():
            continue
        try:
            name = (directory / "comm").read_text().strip()
        except (OSError, UnicodeError):
            continue
        if name.casefold() in {"cs2", "cs2.exe"}:
            raise CsdmError(f"CS2 is running (PID {directory.name}); stop it before changing plugin files")


@dataclass(frozen=True)
class PluginInstallation:
    game_directory: Path
    plugin_path: Path
    gameinfo_path: Path
    backup_path: Path
    manifest_path: Path
    recording_directory: Path
    audio_recording_directory: Path
    original_gameinfo_sha256: str
    patched_gameinfo_sha256: str
    plugin_sha256: str
    plugin_created: bool


def _paths(game_directory: str | Path) -> tuple[Path, Path, Path, Path, Path]:
    game = Path(game_directory).resolve(strict=True)
    csgo = game / "csgo"
    gameinfo = csgo / "gameinfo.gi"
    plugin = csgo / "csdm/bin/linuxsteamrt64/libserver.so"
    manifest = csgo / "gameinfo.gi.cs2-mvp.json"
    backup = csgo / "gameinfo.gi.cs2-mvp.backup"
    for path in (gameinfo, plugin, manifest, backup):
        _safe_components(path, game)
    return game, gameinfo, plugin, manifest, backup


def recover_plugin(game_directory: str | Path) -> PluginInstallation:
    """Restore an interrupted transaction, leaving any conflicting files intact.

    Recovery is explicit and must run after CS2 exits. The durable manifest and
    original gameinfo backup remain available on every conflict.
    """
    _assert_game_stopped()
    game, gameinfo, plugin, manifest, backup = _paths(game_directory)
    try:
        raw_manifest = _regular(manifest)
        data = json.loads(raw_manifest)
        if data["version"] != 1 or data["game_directory"] != str(game):
            raise ValueError("Unexpected transaction version or game directory")
        original_hash = data["original_gameinfo_sha256"]
        patched_hash = data["patched_gameinfo_sha256"]
        plugin_hash = data["plugin_sha256"]
        if not all(isinstance(h, str) and re.fullmatch(r"[0-9a-f]{64}", h) for h in (original_hash, patched_hash, plugin_hash)):
            raise ValueError("Invalid transaction hashes")
        if not isinstance(data["plugin_created"], bool):
            raise ValueError("Invalid plugin ownership")
        mode = data["gameinfo_mode"]
        if not isinstance(mode, int) or mode & ~0o777:
            raise ValueError("Invalid file mode")
        # A crash between exclusive manifest creation and backup creation is
        # recoverable when gameinfo is still byte-for-byte original.
        original = _regular(backup) if backup.exists() else _regular(gameinfo)
        if sha256(original) != original_hash:
            raise ValueError("The original backup hash does not match")
    except (KeyError, TypeError, ValueError, CsdmError) as exc:
        raise RestoreConflict(f"Cannot trust recovery state; preserve {manifest} and {backup}: {exc}") from exc

    try:
        if sha256(_regular(gameinfo)) not in {original_hash, patched_hash}:
            raise RestoreConflict(f"gameinfo.gi changed externally; preserve edits and recover from {backup}; manifest: {manifest}")
        if plugin.exists() and sha256(_regular(plugin)) != plugin_hash:
            raise RestoreConflict(f"The CS:DM plugin changed externally; preserve {manifest} and {backup}")
        if sha256(_regular(manifest)) != sha256(raw_manifest):
            raise RestoreConflict(f"The recovery manifest changed externally: {manifest}")
    except CsdmError as exc:
        if isinstance(exc, RestoreConflict):
            raise
        raise RestoreConflict(f"A managed file disappeared or is no longer regular; preserve {manifest} and {backup}: {exc}") from exc

    if sha256(_regular(gameinfo)) == patched_hash and patched_hash != original_hash:
        _replace_file(gameinfo, original, mode)
    if data["plugin_created"] and plugin.exists():
        plugin.unlink()
        _sync_directory(plugin.parent)
    # Never recursively delete csdm: its movie directory contains the capture.
    for relative in ("csgo/csdm/bin/linuxsteamrt64", "csgo/csdm/bin", "csgo/csdm"):
        if relative in data.get("created_directories", []):
            try:
                (game / relative).rmdir()
            except OSError:
                pass
    backup.unlink(missing_ok=True)
    manifest.unlink()
    _sync_directory(gameinfo.parent)
    return PluginInstallation(
        game, plugin, gameinfo, backup, manifest, game / "csgo/csdm/movie",
        game / "csgo/movie", original_hash, patched_hash, plugin_hash, data["plugin_created"],
    )


@contextmanager
def temporary_plugin(
    game_directory: str | Path,
    plugin_source: str | Path,
    *,
    launch_args: Sequence[str],
    expected_sha256: str | None = PLUGIN_SHA256,
) -> Iterator[PluginInstallation]:
    """Install the proxy for one local recording and restore it after CS2 exits.

    ``game_directory`` is the install's ``game`` directory, containing ``csgo``.
    An identical existing binary is reused. A different existing binary, an
    unfinished transaction, a CSDM ``.backup``, or alternate ``gameinfo.vpk`` is
    rejected. Existing Metamod/CounterStrikeSharp files are never modified.
    Call ``recover_plugin`` after a crash; conflict recovery never overwrites
    a gameinfo edit made by Steam, another recorder, or the user.
    """
    assert_demo_only_launch(launch_args)
    _assert_game_stopped()
    game, gameinfo, plugin, manifest, backup = _paths(game_directory)
    for path in (manifest, backup, gameinfo.with_suffix(".gi.backup"), gameinfo.with_name("gameinfo.vpk")):
        if path.exists() or path.is_symlink():
            raise CsdmError(f"Existing recovery state or unsupported alternate gameinfo; inspect it first: {path}")
    binary = _regular(Path(plugin_source))
    binary_hash = sha256(binary)
    if expected_sha256 is not None and binary_hash != expected_sha256:
        raise CsdmError(f"CS:DM plugin SHA-256 mismatch: expected {expected_sha256}, got {binary_hash}")
    if not binary.startswith(b"\x7fELF\x02\x01"):
        raise CsdmError("The CS:DM plugin must be a little-endian ELF64 Linux library")
    plugin_created = not plugin.exists()
    if not plugin_created and sha256(_regular(plugin)) != binary_hash:
        raise CsdmError(f"A different CS:DM plugin already exists; it will not be overwritten: {plugin}")

    original = _regular(gameinfo)
    patched = patch_gameinfo(original)
    mode = stat.S_IMODE(gameinfo.stat().st_mode) & 0o777
    missing = [p for p in (game / "csgo/csdm", game / "csgo/csdm/bin", plugin.parent) if not p.exists()]
    state = {
        "version": 1,
        "game_directory": str(game),
        "original_gameinfo_sha256": sha256(original),
        "patched_gameinfo_sha256": sha256(patched),
        "plugin_sha256": binary_hash,
        "plugin_created": plugin_created,
        "gameinfo_mode": mode,
        "created_directories": [str(p.relative_to(game)) for p in missing],
        "owner_pid": os.getpid(),
    }
    # This fixed, exclusive manifest also serializes recording transactions for
    # this game install, including requests using different output directories.
    _exclusive_file(manifest, (json.dumps(state, indent=2) + "\n").encode())
    backup_created = False
    installed = False
    try:
        _exclusive_file(backup, original)
        backup_created = True
        for directory in missing:
            directory.mkdir()
        if plugin_created:
            _exclusive_file(plugin, binary, 0o755)
        if sha256(_regular(gameinfo)) != state["original_gameinfo_sha256"]:
            raise RestoreConflict(f"gameinfo.gi changed during preparation; recovery files: {manifest}, {backup}")
        if patched != original:
            _replace_file(gameinfo, patched, mode)
        installation = PluginInstallation(
            game, plugin, gameinfo, backup, manifest, game / "csgo/csdm/movie",
            game / "csgo/movie", state["original_gameinfo_sha256"],
            state["patched_gameinfo_sha256"], binary_hash, plugin_created,
        )
        installed = True
        yield installation
    finally:
        if backup_created and manifest.exists():
            recover_plugin(game)
        elif not backup_created:
            # Nothing in the game was modified before creating our backup.
            # Preserve a conflicting manifest rather than removing somebody's.
            if _regular(manifest) == (json.dumps(state, indent=2) + "\n").encode():
                manifest.unlink()
                _sync_directory(manifest.parent)
        elif installed:
            # Explicit recover_plugin() inside the context may already have
            # finished cleanup. Otherwise, disappearing state is a conflict.
            restored = sha256(_regular(gameinfo)) == state["original_gameinfo_sha256"]
            removed = not plugin_created or not plugin.exists()
            if backup.exists() or not restored or not removed:
                raise RestoreConflict(f"Recovery state disappeared during recording: {manifest}; backup: {backup}")
