"""Persistent, local-only CS2 cinematic sessions and the native control client.

The editor runs in the game. This process owns the temporary plugin transaction
and stays alive until the game exits or the user ends the session.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack, contextmanager
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
import uuid

MAX_MESSAGE = 1024 * 1024
# State combines a bounded project, demo metadata and cached project library.
# Keep authoring requests small while allowing the complete read-only snapshot.
MAX_RESPONSE = 4 * 1024 * 1024
LIVE_METHODS = frozenset({"get_state", "set_camera", "release_camera", "add_key", "update_key",
                          "remove_key", "set_project", "scrub", "preview", "demo_control",
                          "apply_preset", "undo", "redo", "export_project", "import_project", "set_scene", "reset_scene",
                          "request_export", "inspect_entities",
                          "set_player_cosmetics", "reset_player_cosmetics", "set_team_palette", "reset_team_palette",
                          "set_presentation", "select_player", "select_timeline_player", "set_editor_preferences", "set_demo_playback_range"})
# Host-produced metadata is not an MCP authoring tool. The native adapter
# validates its recording identity and never changes the camera-key project.
INTERNAL_LIVE_METHODS = frozenset({"set_demo_timeline", "refresh_project_library", "load_saved_project", "save_project", "save_project_copy", "set_safe_exit_status", "guarded_mutation", "set_map_preview", "request_geometry_validation", "set_geometry_validation"})


class LiveError(RuntimeError):
    """An actionable error from the launcher or native editor."""


def resource_root() -> Path:
    """PyInstaller resources are read-only; session state never goes here."""
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parents[2]))


def state_root() -> Path:
    return Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "cs2-camera"


DEFAULT_HOTKEYS = {"capture": "F6", "camera": "F7", "editor": "F8", "preview": "F9"}


def hotkey_configuration() -> Path:
    """Install first-run defaults without modifying a user's existing bindings."""
    configured = os.environ.get("CS2_CAMERA_HOTKEYS")
    path = (Path(configured).expanduser() if configured else
            Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "cs2-camera/hotkeys.json")
    path = path.absolute()
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    try:
        with path.open("x") as handle:
            os.chmod(path, 0o600)
            json.dump(DEFAULT_HOTKEYS, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    except FileExistsError:
        if not path.is_file():
            raise LiveError(f"Hotkey configuration must be a file: {path}")
    return path


def bundled_font() -> Path | None:
    root = resource_root()
    paths = [root / "native/live/NotoSans-Regular.ttf"]
    if not getattr(sys, "frozen", False):
        paths.append(root / "packaging/live/fonts/NotoSans-Regular.ttf")
    return next((path for path in paths if path.is_file()), None)


def installed_game_fonts(game: Path) -> dict[str, str]:
    """Reference installed Panorama fonts in place; never copy Valve assets."""
    directory = game / "csgo/panorama/fonts"
    fonts = {"CS2_CAMERA_GAME_FONT": directory / "notosans-regular.ttf",
             "CS2_CAMERA_MONO_FONT": directory / "stratum2regular_monodigit.ttf"}
    return {variable: str(path.absolute()) for variable, path in fonts.items() if path.is_file()}


def staged_cosmetics_environment(game: Path, run: Path, notify) -> dict[str, str]:
    """Keep camera sessions usable when an installed cosmetic schema changes."""
    from .cosmetics_catalog import stage_catalog
    notify("Reading the installed game's builtin cosmetic catalog…")
    try:
        path = stage_catalog(game, run / "cosmetics-catalog.json")
    except Exception as error:
        reason = f"{type(error).__name__}: {error}"[:2048]
        notify("Builtin cosmetics are unavailable: " + reason)
        return {"CS2_CAMERA_COSMETICS_CATALOG_ERROR": reason}
    return {"CS2_CAMERA_COSMETICS_CATALOG": str(path)}


def staged_demo_timeline_environment(demo: Path, run: Path, notify) -> dict[str, str]:
    """Index the complete recording once on the host before CS2 starts."""
    from .demo_timeline import stage_timeline
    notify("Reading full demo duration, rounds and player kill/death timeline…")
    try:
        path = stage_timeline(demo, run / "demo-timeline.json")
    except Exception as error:
        reason = f"{type(error).__name__}: {error}"[:512]
        notify("Full demo timeline is unavailable: " + reason)
        return {"CS2_CAMERA_DEMO_TIMELINE_ERROR": reason}
    return {"CS2_CAMERA_DEMO_TIMELINE": str(path)}


def refresh_demo_timeline(client=None, demo: Path | None = None):
    """Refresh an existing game's read-only recording metadata without restart."""
    from .demo_timeline import build_timeline
    if demo is None:
        recorded = session_descriptor().get("demo")
        if not recorded:
            raise LiveError("The current session has no recording path for its demo timeline")
        demo = Path(recorded)
    timeline = build_timeline(demo)
    return (client or NativeClient()).request("set_demo_timeline", {"timeline": timeline})


def runtime_directory() -> Path:
    base = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"))
    directory = base / f"cs2-camera-{os.getuid()}"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise LiveError(f"Session directory is not an owned directory: {directory}")
    directory.chmod(0o700)
    return directory


def control_socket_path() -> Path:
    """Use a private home-backed path visible inside Steam pressure-vessel.

    Runtime directory subfolders are not automatically mounted into the game
    container. Keep descriptor/locks there, but put the Unix socket in shared
    user storage and stay within sockaddr_un's 107-byte pathname limit.
    """
    candidates = [state_root() / "ipc", Path.home() / ".cs2-camera-ipc"]
    for directory in candidates:
        directory = directory.expanduser().absolute()
        socket_path = directory / "control.sock"
        if len(os.fsencode(socket_path)) > 107:
            continue
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = directory.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            raise LiveError(f"IPC directory is not an owned directory: {directory}")
        directory.chmod(0o700)
        return socket_path
    raise LiveError("The private Unix socket path exceeds 107 bytes. Set XDG_STATE_HOME to a shorter home-backed directory.")


def _write_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("x", encoding="utf-8") as handle:
            os.chmod(temporary, 0o600)
            json.dump(value, handle, ensure_ascii=False, indent=2, allow_nan=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        temporary.replace(path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def _safe_exit_checkpoint(client, run: Path, projects: Path) -> dict:
    """Confirm the current document exists durably in both recovery locations.

    The native saving phase freezes authoring before this function runs. Never
    treat a successful socket exchange, old checkpoint or saved flag alone as
    proof that the latest camera document reached its destination.
    """
    before = client.request("get_state")
    if not isinstance(before, dict):
        raise LiveError("The current editor state is unavailable")
    project = client.request("export_project")
    # Also reject non-JSON/non-finite values before attempting any shutdown.
    project = json.loads(json.dumps(project, allow_nan=False))
    if not isinstance(project, dict):
        raise LiveError("The current camera project is unavailable")
    try:
        saved = client.request("save_project")
    except LiveError as original:
        # An existing save destination can be damaged/read-only while the
        # project directory still accepts a fresh copy. Keep both errors if it
        # cannot; an old checkpoint must never authorize closing the game.
        try:
            saved = client.request("save_project_copy")
        except LiveError as fallback:
            raise LiveError(f"Project save failed: {original}; saving a copy also failed: {fallback}") from fallback
    if not isinstance(saved, dict) or saved.get("saved") is not True or not isinstance(saved.get("path"), str):
        raise LiveError("The project library did not confirm a saved project")
    path = Path(saved["path"])
    if not path.is_absolute() or path.parent.resolve() != projects.resolve() or path.suffix != ".json":
        raise LiveError("The saved project is outside this session's project library")
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or not 0 < info.st_size <= MAX_RESPONSE:
            raise LiveError("The saved project must be an owned, bounded regular file")
        document = stream.read(MAX_RESPONSE + 1)
    if len(document) > MAX_RESPONSE or json.loads(document) != project:
        raise LiveError("The saved project differs from the current camera project; retry Safe exit")
    _write_json(run / "project.json", project)
    after = client.request("get_state")
    if (not isinstance(after, dict) or before.get("revision") != after.get("revision")
            or before.get("project_document_id") != after.get("project_document_id")
            or after.get("project") != project):
        raise LiveError("The camera project changed while saving; retry Safe exit")
    return saved


class SafeExitGate:
    """Host-only, retryable save barrier shared by Qt and native UI requests."""
    def __init__(self, request: threading.Event | None = None):
        self.request = request if request is not None else threading.Event()
        self.seen_native_id = 0

    def observe(self, state: dict) -> None:
        requested = state.get("safe_exit_request_id", 0)
        if type(requested) is int and requested > self.seen_native_id:
            self.seen_native_id = requested
            self.request.set()

    def attempt(self, client, run: Path, projects: Path, report=None) -> bool:
        if not self.request.is_set():
            return False
        self.request.clear()
        report = report or (lambda phase, error: None)
        try:
            report("saving", "")
            client.request("set_safe_exit_status", {"phase": "saving"})
            _safe_exit_checkpoint(client, run, projects)
            client.request("set_safe_exit_status", {"phase": "closing"})
            report("closing", "")
            return True
        except (LiveError, OSError, ValueError, TypeError) as error:
            message = str(error).replace("\0", "").encode("utf-8", errors="replace")[:2048].decode("utf-8", errors="ignore")
            try:
                client.request("set_safe_exit_status", {"phase": "error", "error": message})
            except (LiveError, OSError, ValueError):
                pass
            report("error", message)
            return False


def session_descriptor() -> dict:
    path = runtime_directory() / "session.json"
    try:
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
            raise LiveError("Untrusted live-session descriptor")
        result = json.loads(path.read_text())
        if not isinstance(result, dict) or not isinstance(result.get("socket"), str):
            raise ValueError("invalid descriptor")
        return result
    except (OSError, ValueError) as error:
        raise LiveError("No live session. Start CS2 Camera Studio with a local demo first.") from error


class NativeClient:
    """One bounded JSON exchange per connection, safe across GUI/MCP threads."""
    def __init__(self, socket_path: str | Path | None = None, timeout: float = 5):
        self.socket_path = str(socket_path) if socket_path else None
        self.timeout = timeout

    def request(self, method: str, params: dict | None = None):
        if method not in LIVE_METHODS and method not in INTERNAL_LIVE_METHODS:
            raise LiveError(f"Unknown native camera method: {method}")
        if params is not None and not isinstance(params, dict):
            raise LiveError("Camera parameters must be a JSON object")
        path = self.socket_path or os.environ.get("CS2_CAMERA_SOCKET") or session_descriptor()["socket"]
        request_id = uuid.uuid4().hex
        payload = json.dumps({"id": request_id, "method": method, "params": params or {}},
                             ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode() + b"\n"
        if len(payload) > MAX_MESSAGE:
            raise LiveError("Camera request exceeds 1 MiB")
        submitted = acknowledged = False
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(self.timeout)
                connection.connect(path)
                if hasattr(socket, "SO_PEERCRED"):
                    _, peer_uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                    if peer_uid != os.getuid():
                        raise LiveError("Camera socket belongs to another user")
                submitted = True
                connection.sendall(payload)
                response = bytearray()
                while b"\n" not in response:
                    chunk = connection.recv(min(65536, MAX_RESPONSE + 1 - len(response)))
                    if not chunk:
                        raise LiveError("Native editor closed the connection without a response")
                    response.extend(chunk)
                    if len(response) > MAX_RESPONSE:
                        raise LiveError("Native editor response exceeds 4 MiB")
            result = json.loads(response.partition(b"\n")[0])
            if not isinstance(result, dict) or result.get("id") != request_id:
                raise LiveError("Native editor response does not match the request")
            if "error" in result:
                acknowledged = True
                error = result["error"]
                raise LiveError(error.get("message", str(error)) if isinstance(error, dict) else str(error))
            if "result" not in result:
                raise LiveError("Native editor returned an invalid response")
            acknowledged = True
            return result["result"]
        except (OSError, ValueError, LiveError) as error:
            if submitted and not acknowledged and method not in {"get_state", "ping", "export_project"}:
                raise LiveError(f"{error}; command outcome is unknown until get_state confirms it; do not retry blindly") from error
            if isinstance(error, LiveError):
                raise
            raise LiveError(f"Cannot contact the live editor at {path}: {error}") from error


def native_libraries(plugin: Path | None = None, overlay: Path | None = None,
                     headless: bool = False) -> tuple[Path, Path | None]:
    root = resource_root()
    plugin = plugin or root / "native/live/libserver.so"
    overlay = overlay or root / "native/live/libcs2_camera_overlay.so"
    if not getattr(sys, "frozen", False):
        if not plugin.is_file() and plugin == root / "native/live/libserver.so":
            plugin = root / ".cache/csdm-live/libserver.so"
        if not overlay.is_file() and overlay == root / "native/live/libcs2_camera_overlay.so":
            overlay = root / ".cache/cs2-overlay/libcs2_camera_overlay.so"
    paths = [plugin] + ([] if headless else [overlay])
    for path in paths:
        if not path.is_file():
            raise LiveError(f"Native library is missing: {path}. Build the live module or use the packaged AppImage.")
        if path.read_bytes()[:6] != b"\x7fELF\x02\x01":
            raise LiveError(f"Expected an ELF64 Linux library: {path}")
    return plugin.resolve(), None if headless else overlay.resolve()


def desktop_size() -> tuple[int, int] | None:
    try:
        result = subprocess.run(["xrandr", "--current"], text=True, capture_output=True, timeout=3)
        # Pick one monitor; the root desktop can span several displays.
        match = re.search(r"^\S+ connected primary (\d+)x(\d+)[+-]", result.stdout, re.MULTILINE)
        match = match or re.search(r"^\S+ connected (\d+)x(\d+)[+-]", result.stdout, re.MULTILINE)
        match = match or re.search(r"current (\d+) x (\d+)", result.stdout)
        if match:
            return int(match[1]), int(match[2])
    except (OSError, subprocess.TimeoutExpired):
        pass
    return None


def game_display(app, window) -> tuple[str | None, tuple[int, int] | None]:
    """Use the desktop's primary screen even when the launcher is elsewhere.

    Resolve this on the GUI thread before starting a session or reopening it
    after export. SDL consumes the connector priority before initialization.
    """
    screen = app.primaryScreen() or window.screen()
    if screen is None:
        return None, desktop_size()
    geometry, scale = screen.geometry(), screen.devicePixelRatio()
    return screen.name(), (round(geometry.width() * scale), round(geometry.height() * scale))


def launch_arguments(runtime: Path, game: Path, demo_link: Path, run: Path,
                     *, windowed: bool = False, screen_size: tuple[int, int] | None = None) -> list[str]:
    # Start windowed; the overlay requests SDL's borderless desktop fullscreen
    # on the window's owner thread. Desktop-sized -noborder alone leaves KDE
    # panels above the game because it does not set the fullscreen state.
    size = (1280, 720) if windowed else screen_size or desktop_size()
    video = ["-sw"] if windowed else ["-sw", "-noborder"]
    if size:
        video += ["-w", str(size[0]), "-h", str(size[1])]
    # Keep the command list deliberately fixed: the native proxy is demo-only.
    return [str(runtime), "--verb=waitforexitandrun", "--", str(game / "cs2.sh"),
            "-insecure", "-novid", *video,
            "-con_logfile", str(run / "console.log"), "+demo_ui_mode", "0",
            "+playdemo", str(demo_link)]


@contextmanager
def session_locks():
    """Serialize packaged sessions and, in a checkout, the existing renderer."""
    with ExitStack() as stack:
        paths = [runtime_directory() / "session.lock"]
        if not getattr(sys, "frozen", False):
            paths.append(resource_root() / ".runs/render.lock")
        for path in paths:
            path.parent.mkdir(parents=True, exist_ok=True)
            lock = stack.enter_context(path.open("a"))
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise LiveError("Another live session or render is active. Let it finish first.") from error
        yield


def recover_session(game_dir: str | None = None) -> dict:
    """Recover only the verified plugin transaction after an interrupted app.

    Config backups remain available for explicit inspection. Picking a config
    snapshot automatically could overwrite edits made after the interrupted run.
    """
    from .csdm import _assert_game_stopped, recover_plugin
    from .discovery import find_game
    with session_locks():
        _assert_game_stopped()
        game = find_game(game_dir)
        manifest = game / "csgo/gameinfo.gi.cs2-mvp.json"
        backup = game / "csgo/gameinfo.gi.cs2-mvp.backup"
        pending = manifest.exists() or manifest.is_symlink()
        if not pending and (backup.exists() or backup.is_symlink()):
            raise LiveError(f"Recovery backup exists without its transaction manifest; preserve it for inspection: {backup}")
        if pending:
            recover_plugin(game)  # Validates all hashes and preserves conflicts.
        return {"status": "recovered" if pending else "nothing_to_recover",
                "game_directory": str(game), "plugin_transaction_restored": pending,
                "preserved_runs_directory": str(state_root() / "runs"),
                "config_backups": "Retained under each run's original-config directory; no configuration snapshot was selected or overwritten."}


def _process_snapshot(pid: int) -> dict | None:
    """Read identity even during a crash, when cmdline is empty or state is Z."""
    directory = Path("/proc") / str(pid)
    try:
        fields = (directory / "stat").read_text().rsplit(")", 1)[1].split()
        result = {"pid": pid, "state": fields[0], "parent": int(fields[1]),
                  "session": int(fields[3]), "start": int(fields[19]),
                  "name": (directory / "comm").read_text().strip()}
        try:
            result["args"] = (directory / "cmdline").read_bytes().split(b"\0")
        except OSError:
            result["args"] = []
        return result
    except (OSError, UnicodeError, ValueError, IndexError):
        return None


def _process_snapshots() -> list[dict]:
    return [item for directory in Path("/proc").iterdir() if directory.name.isdigit()
            if (item := _process_snapshot(int(directory.name))) is not None]


class OwnedProcesses:
    """Remember ownership independently of a process's changing name/state.

    A PID alone is insufficient after a long live session: start time prevents
    signalling an unrelated process that later reused the same PID.
    """
    def __init__(self, launcher_pid: int, demo_link: Path):
        self.launcher_pid, self.demo_link = launcher_pid, demo_link
        self.identities: dict[int, int] = {}
        launcher = _process_snapshot(launcher_pid)
        if launcher is not None:
            self.identities[launcher_pid] = launcher["start"]

    def observe(self) -> list[dict]:
        snapshots = _process_snapshots()
        current = {item["pid"]: item for item in snapshots}
        # A living member with a remembered identity proves that this process
        # session still belongs to us; an old numerical session id does not.
        own_session_exists = any(pid in current and current[pid]["start"] == started
                                 and current[pid]["session"] == self.launcher_pid
                                 for pid, started in self.identities.items())
        for item in snapshots:
            args = item["args"]
            private_demo = (item["name"].casefold() in {"cs2", "cs2.exe"}
                            and b"-insecure" in args
                            and any(args[index:index + 2] == [b"+playdemo", os.fsencode(self.demo_link)]
                                    for index in range(len(args) - 1)))
            if private_demo or (own_session_exists and item["session"] == self.launcher_pid):
                self.identities[item["pid"]] = item["start"]
        return [item for item in snapshots if self.identities.get(item["pid"]) == item["start"]]


def _wait_game_stopped(timeout: float = 5) -> None:
    """Use the plugin transaction's exact definition of stopped before exiting."""
    from .csdm import CsdmError, _assert_game_stopped
    deadline, quiet_since, last_error = time.monotonic() + timeout, None, None
    while time.monotonic() < deadline:
        try:
            _assert_game_stopped()  # Includes unreaped zombies and cs2.exe.
        except CsdmError as error:
            quiet_since, last_error = None, error
        else:
            quiet_since = quiet_since if quiet_since is not None else time.monotonic()
            if time.monotonic() - quiet_since >= .2:
                return
        time.sleep(.05)
    raise LiveError(f"CS2 has not fully exited; recovery files retained. {last_error or ''}".strip())


def _stop_owned(process: subprocess.Popen, demo_link: Path, tracker: OwnedProcesses | None = None):
    tracker = tracker or OwnedProcesses(process.pid, demo_link)
    for signum, limit in ((signal.SIGTERM, 12), (signal.SIGKILL, 5)):
        survivors = tracker.observe()
        if not survivors:
            break
        for item in survivors:
            # Re-check identity immediately before sending a signal. Zombies
            # cannot receive it; wait for their parent/coredump cleanup instead.
            current = _process_snapshot(item["pid"])
            if current is None or current["start"] != item["start"] or current["state"] == "Z":
                continue
            try:
                os.kill(item["pid"], signum)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + limit
        while tracker.observe() and time.monotonic() < deadline:
            process.poll()  # Reap our own launcher if it exited.
            time.sleep(.05)
    process.poll()
    if tracker.observe():
        raise LiveError("Owned CS2 processes are still running or being reaped; recovery files retained")
    process.wait(timeout=5)
    # A foreign game appearing during shutdown is never ours to stop. It will
    # fail this barrier and leave the plugin/config recovery transaction intact.
    _wait_game_stopped()


def _steam_startup_failure(run: Path) -> Path | None:
    """Recognize a definite Steamworks fatal in this session's bounded output."""
    for name in ("game.log", "console.log"):
        path = run / name
        try:
            with path.open("rb") as handle:
                end = handle.seek(0, os.SEEK_END)
                handle.seek(max(0, end - 16384))
                tail = handle.read(16384)
        except OSError:
            continue
        # Early S_API interface warnings may precede successful initialization.
        # Require the known fatal itself, not an unrelated fatal plus warnings.
        if re.search(rb"FATAL ERROR:\s*(?:Failed to initialize the Steamworks SDK|"
                     rb"(?:SteamAPI_Init failed\.\s*)?ConnectToGlobalUser failed)", tail, re.IGNORECASE):
            return path
    return None


def run_session(demo: Path, *, game_dir: str | None = None, plugin: Path | None = None,
                overlay: Path | None = None, headless: bool = False,
                stop: threading.Event | None = None, notify=None, windowed: bool = False,
                screen_size: tuple[int, int] | None = None, screen_name: str | None = None,
                on_ready=None, shot_library: Path | None = None,
                projects_dir: Path | None = None, safe_exit: threading.Event | None = None,
                on_safe_exit=None) -> dict:
    from .csdm import temporary_plugin
    from .discovery import find_game, find_runtime, process_ids, sha256
    from .render import config_backup
    stop = stop or threading.Event()
    safe_exit_gate = SafeExitGate(safe_exit)
    notify = notify or (lambda value: print(value, file=sys.stderr, flush=True))
    demo = demo.expanduser().resolve(strict=True)
    if demo.suffix.lower() != ".dem" or not demo.is_file():
        raise LiveError("Choose an existing local .dem file")
    # Optional authoring input is checked before locks, staging or game changes.
    # A stale/malformed collection never prevents normal camera authoring.
    library = None
    configured_library = shot_library or os.environ.get("CS2_CAMERA_SHOT_LIBRARY")
    if configured_library:
        from .shot_library import read_library
        try:
            library = read_library(Path(configured_library), demo)
        except Exception as error:
            notify("Saved shots are unavailable: " + str(error)[:512])
    plugin, overlay = native_libraries(plugin, overlay, headless)
    with session_locks():
        if process_ids("cs2"):
            raise LiveError("CS2 is already running. The existing game or render was left untouched; close it before starting this session.")
        if not process_ids("steam"):
            raise LiveError("Start and sign in to Steam first")
        if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
            raise LiveError("A graphical desktop session is required")
        game, runtime = find_game(game_dir), find_runtime()
        run = state_root() / "runs" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
        run.mkdir(parents=True, mode=0o700)
        demo_link = run / "input.dem"
        demo_link.symlink_to(demo)  # No CSDM actions sidecar: the game must stay open.
        socket_path = control_socket_path()
        if socket_path.exists() or socket_path.is_symlink():
            socket_info = socket_path.lstat()
            if not stat.S_ISSOCK(socket_info.st_mode) or socket_info.st_uid != os.getuid():
                raise LiveError(f"Refusing to replace a non-socket file: {socket_path}")
            socket_path.unlink()
        launch = launch_arguments(runtime, game, demo_link, run, windowed=windowed, screen_size=screen_size)
        environment = os.environ.copy()
        environment.pop("CS2_CAMERA_SHOT_LIBRARY", None)
        environment.pop("CS2_CAMERA_SHOT_LIBRARY_ERROR", None)
        if library is not None:
            from .shot_library import stage_library
            environment["CS2_CAMERA_SHOT_LIBRARY"] = str(stage_library(library, demo_link, run))
        if getattr(sys, "frozen", False):
            # Restore the user's library lookup for Steam, not PyInstaller's Qt/Python directory.
            original = environment.get("LD_LIBRARY_PATH_ORIG")
            if original is None:
                environment.pop("LD_LIBRARY_PATH", None)
            else:
                environment["LD_LIBRARY_PATH"] = original
            for variable in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "PYTHONHOME"):
                environment.pop(variable, None)
        profile = run / "config"
        profile.mkdir()
        from .project_storage import projects_directory, geometry_directory
        from .editor_preferences import configuration_path as editor_configuration_path
        projects = projects_directory(projects_dir)
        geometry_cache = geometry_directory()
        editor_preferences_path = editor_configuration_path()
        if geometry_cache:
            environment["CS2_CAMERA_GEOMETRY_CACHE"] = str(geometry_cache)
        for variable in ("CS2_CAMERA_COSMETICS_CATALOG", "CS2_CAMERA_COSMETICS_CATALOG_ERROR"):
            environment.pop(variable, None)
        environment.update(staged_cosmetics_environment(game, run, notify))
        for variable in ("CS2_CAMERA_DEMO_TIMELINE", "CS2_CAMERA_DEMO_TIMELINE_ERROR"):
            environment.pop(variable, None)
        environment.update(staged_demo_timeline_environment(demo, run, notify))
        environment.update({"SteamAppId": "730", "SteamGameId": "730", "USRLOCALCSGO": str(profile),
                            "CS2_CAMERA_LIVE": "1", "CS2_CAMERA_SOCKET": str(socket_path),
                            "CS2_CAMERA_FULLSCREEN_WINDOWED": "0" if windowed else "1",
                            "CS2_CAMERA_DEMO_PATH": str(demo_link),
                            "CS2_CAMERA_PROJECT": str(projects / "last-project.json"),
                            "CS2_CAMERA_PROJECTS_DIR": str(projects),
                            "CS2_CAMERA_EDITOR_PREFS": str(editor_preferences_path),
                            "CS2_CAMERA_HOTKEYS": str(hotkey_configuration())})
        if screen_name:
            environment["SDL_VIDEO_DISPLAY_PRIORITY"] = screen_name
        if overlay:
            for variable in ("CS2_CAMERA_GAME_FONT", "CS2_CAMERA_MONO_FONT"):
                environment.pop(variable, None)
            environment.update(installed_game_fonts(game))
            manifest = overlay.parent / "VkLayer_cs2_camera.json"
            if not manifest.is_file():
                raise LiveError(f"Vulkan editor layer manifest is missing: {manifest}")
            native_copy = run / "native"
            native_copy.mkdir()
            overlay_copy = native_copy / "libcs2_camera_overlay.so"
            shutil.copy2(overlay, overlay_copy)
            layer = json.loads(manifest.read_text())
            layer["layer"]["library_path"] = "./libcs2_camera_overlay.so"
            (native_copy / manifest.name).write_text(json.dumps(layer, indent=2) + "\n")
            overlay = overlay_copy
            font = bundled_font()
            if font:
                font_copy = native_copy / "NotoSans-Regular.ttf"
                shutil.copy2(font, font_copy)
                environment["CS2_CAMERA_FONT"] = str(font_copy)
            environment["CS2_CAMERA_OVERLAY"] = str(overlay)
            environment["LD_PRELOAD"] = ":".join(filter(None, (str(overlay), environment.get("LD_PRELOAD"))))
            environment["VK_ADD_LAYER_PATH"] = ":".join(filter(None, (str(overlay.parent), environment.get("VK_ADD_LAYER_PATH"))))
            environment["VK_INSTANCE_LAYERS"] = ":".join(filter(None, ("VK_LAYER_CS2_camera", environment.get("VK_INSTANCE_LAYERS"))))
        descriptor = {"version": 1, "pid": os.getpid(), "socket": str(socket_path), "demo": str(demo), "game_directory": str(game),
                      "run_directory": str(run), "status": "starting", "insecure": True, "overlay": bool(overlay)}
        if geometry_cache:
            descriptor["geometry_cache"] = str(geometry_cache)
        descriptor["editor_preferences_path"] = str(editor_preferences_path)
        descriptor_path = runtime_directory() / "session.json"
        process = None
        owned_stopped = True
        try:
            with config_backup(run), temporary_plugin(game, plugin, launch_args=launch, expected_sha256=sha256(plugin)):
                _write_json(run / "launch.json", {"argv": launch, "socket": str(socket_path)})
                with (run / "game.log").open("w") as log:
                    notify("Starting local demo with -insecure. Waiting for the in-game editor…")
                    process = subprocess.Popen(launch, cwd=game, env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                    owned_stopped = False
                    tracker = None
                    client = NativeClient(socket_path, timeout=.75)
                    handled_preferences_revision = 0
                    safe_exit_confirmed = False
                    ready = False
                    native_connected = False
                    saw_native_endpoint = False
                    try:
                        tracker = OwnedProcesses(process.pid, demo_link)
                        descriptor["game_launcher_pid"] = process.pid
                        _write_json(descriptor_path, descriptor)
                        _write_json(run / "session.json", descriptor)
                        ready, began, absent_since = False, time.monotonic(), None
                        last_checkpoint = 0.0
                        last_geometry_poll, handled_geometry_request = 0.0, 0
                        last_preferences_poll, last_preferences_error = 0.0, ""
                        last_safe_exit_poll = 0.0
                        while not stop.wait(.25):
                            owned = tracker.observe()
                            if not ready:
                                steam_failure = _steam_startup_failure(run)
                                if steam_failure:
                                    raise LiveError("Steam is still connecting or is not ready for this game session. "
                                                    "Wait until the Steam client is ready, then try again. "
                                                    f"See {steam_failure}")
                            if not ready and socket_path.exists():
                                saw_native_endpoint = True
                                try:
                                    state = client.request("get_state")
                                except LiveError:
                                    state = None
                                if isinstance(state, dict) and isinstance(state.get("project"), dict):
                                    native_connected = True
                                    safe_exit_gate.observe(state)
                                if (state and state.get("camera_valid") and not state.get("view_hook_installed")
                                        and isinstance(state.get("error"), str) and state["error"].strip()):
                                    raise LiveError(f"Native camera hook unavailable: {state['error']}. See {run / 'game.log'}")
                                if state and state.get("camera_valid") and state.get("view_hook_installed"):
                                    if state.get("editor_preferences_error"):
                                        notify("Editor preferences were not loaded; original file retained: " + str(state["editor_preferences_error"])[:256])
                                    try:
                                        from .camera_guard import preview_for_demo
                                        client.request("set_map_preview", {"preview": preview_for_demo(demo, game)})
                                    except Exception as error:
                                        notify("Map preview unavailable: " + str(error)[:256])
                                    if on_ready:
                                        on_ready(client)
                                    ready = True
                                    descriptor["status"] = "ready"
                                    _write_json(descriptor_path, descriptor)
                                    _write_json(run / "session.json", descriptor)
                                    notify("Editor connected. Use the in-game camera panel; the session stays open for repeated previews.")
                            if ready and time.monotonic() - last_safe_exit_poll > .5:
                                last_safe_exit_poll = time.monotonic()
                                try:
                                    safe_exit_gate.observe(client.request("get_state"))
                                except (LiveError, OSError, ValueError):
                                    pass
                            if safe_exit_gate.request.is_set():
                                def report_exit(phase, error):
                                    if on_safe_exit:
                                        on_safe_exit(phase, error)
                                    notify({"saving": "Saving the current project before Safe exit…",
                                            "closing": "Project saved. Closing the owned session and restoring temporary settings…",
                                            "error": "Safe exit could not save; the session is still open. Retry Safe exit: " + error}[phase])
                                # A visible endpoint may already serve an editable
                                # native UI even if our first read timed out. Once
                                # seen, losing IPC must never authorize discarding it.
                                saw_native_endpoint = saw_native_endpoint or socket_path.exists()
                                if not ready and not native_connected and not saw_native_endpoint and not (run / "project.json").exists():
                                    # No authorable editor has ever connected. Cancelling
                                    # bootstrap cannot discard an authored document.
                                    safe_exit_gate.request.clear()
                                    descriptor["safe_exit"] = True
                                    notify("Cancelling startup and restoring temporary settings…")
                                    if on_safe_exit:
                                        on_safe_exit("closing", "")
                                    break
                                if safe_exit_gate.attempt(client, run, projects, report_exit):
                                    safe_exit_confirmed = True
                                    descriptor["safe_exit"] = True
                                    break
                            if ready and time.monotonic() - last_geometry_poll > .5:
                                last_geometry_poll = time.monotonic()
                                try:
                                    from .geometry_ui import service_validation
                                    handled_geometry_request = service_validation(client, demo, game, handled_geometry_request)
                                except (LiveError, OSError, ValueError) as error:
                                    notify("Map path check unavailable: " + str(error)[:256])
                            if ready and time.monotonic() - last_preferences_poll > .5:
                                last_preferences_poll = time.monotonic()
                                try:
                                    from .editor_preferences import service_preferences
                                    handled_preferences_revision = service_preferences(client, handled_preferences_revision, editor_preferences_path)
                                    last_preferences_error = ""
                                except (LiveError, OSError, ValueError) as error:
                                    message = str(error)[:256]
                                    if message != last_preferences_error:
                                        notify("Editor preferences could not be saved: " + message)
                                    last_preferences_error = message
                            if ready and time.monotonic() - last_checkpoint > 30:
                                last_checkpoint = time.monotonic()
                                try:
                                    _write_json(run / "project.json", client.request("export_project"))
                                except (LiveError, OSError):
                                    pass
                            if process.poll() is not None:
                                if not any(item["name"].casefold() in {"cs2", "cs2.exe"} for item in owned):
                                    absent_since = absent_since or time.monotonic()
                                    if time.monotonic() - absent_since > 2:
                                        if not ready:
                                            raise LiveError(f"CS2 exited before the editor connected. See {run / 'game.log'}")
                                        break
                                else:
                                    absent_since = None
                            if not ready and time.monotonic() - began > 90:
                                raise LiveError(f"The editor did not connect within 90 seconds. See {run / 'game.log'}")
                    finally:
                        try:
                            try:
                                from .editor_preferences import service_preferences
                                service_preferences(client, handled_preferences_revision, editor_preferences_path)
                            except (LiveError, OSError, ValueError):
                                pass
                            if not safe_exit_confirmed:
                                try:
                                    if ready:
                                        _safe_exit_checkpoint(client, run, projects)
                                    else:
                                        _write_json(run / "project.json", client.request("export_project"))
                                except (LiveError, OSError, ValueError, TypeError):
                                    # Export already owns its separately durable snapshot;
                                    # interrupts/startup failures must still clean only the
                                    # owned process. Explicit Safe exit never reaches here
                                    # after a failed save barrier.
                                    try:
                                        _write_json(run / "project.json", client.request("export_project"))
                                    except (LiveError, OSError, ValueError, TypeError):
                                        pass
                        finally:
                            try:
                                notify("Closing the owned session and restoring the game configuration…")
                            finally:
                                _stop_owned(process, demo_link, tracker)
                                owned_stopped = True
            descriptor["status"] = "closed"
            _write_json(run / "session.json", descriptor)
            return descriptor
        except BaseException as error:
            descriptor.update(status="failed", error=str(error))
            _write_json(run / "session.json", descriptor)
            raise
        finally:
            # If stopping failed, preserve the control endpoint and recovery
            # descriptor so a still-running owned game remains reachable.
            if owned_stopped:
                if descriptor_path.exists():
                    try:
                        if json.loads(descriptor_path.read_text()).get("pid") == os.getpid():
                            descriptor_path.unlink()
                    except (OSError, ValueError):
                        pass
                socket_path.unlink(missing_ok=True)


def graphical_launcher(args) -> int:
    """Optional Qt shell; the shipped AppImage includes this runtime."""
    try:
        from PySide6 import QtCore, QtGui, QtWidgets
    except ImportError as error:
        raise LiveError("The desktop launcher requires PySide6-Essentials. From source, pass --demo FILE --no-launcher, or use the AppImage.") from error
    app = QtWidgets.QApplication([sys.argv[0]])
    app.setApplicationName("CS2 Camera Studio")
    app.setOrganizationName("CS2 Camera Studio")
    font = bundled_font()
    if font:
        font_id = QtGui.QFontDatabase.addApplicationFont(str(font))
        families = QtGui.QFontDatabase.applicationFontFamilies(font_id)
        if families:
            app.setFont(QtGui.QFont(families[0], 10))
    window = QtWidgets.QMainWindow()
    window.setWindowTitle("CS2 Camera Studio · Live")
    window.resize(760, 620)
    body, layout = QtWidgets.QWidget(), QtWidgets.QVBoxLayout()
    body.setLayout(layout)
    window.setCentralWidget(body)
    layout.setContentsMargins(28, 24, 28, 24)
    title = QtWidgets.QLabel("CAMERA STUDIO  /  LIVE SESSION")
    title.setObjectName("title")
    layout.addWidget(title)
    description = QtWidgets.QLabel("Open a local demo once. Build and preview camera paths inside CS2.\nLocal -insecure session · edit and repeat without restarting.")
    description.setWordWrap(True)
    layout.addWidget(description)
    library_row = QtWidgets.QHBoxLayout()
    library_label = QtWidgets.QLabel("Downloaded demos")
    refresh_library_button = QtWidgets.QPushButton("Refresh")
    refresh_library_button.setObjectName("refresh_demo_library")
    library_row.addWidget(library_label, 1)
    library_row.addWidget(refresh_library_button)
    layout.addLayout(library_row)
    demo_library = QtWidgets.QComboBox()
    demo_library.setObjectName("demo_library")
    demo_library.setMinimumContentsLength(30)
    demo_library.setSizeAdjustPolicy(QtWidgets.QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
    demo_library.addItem("Searching installed Steam libraries…", None)
    layout.addWidget(demo_library)
    library_status = QtWidgets.QLabel("Checking Steam libraries and mounted disks")
    library_status.setWordWrap(True)
    layout.addWidget(library_status)
    row = QtWidgets.QHBoxLayout()
    demo_edit = QtWidgets.QLineEdit(str(args.demo or ""))
    demo_edit.setObjectName("demo_path")
    demo_edit.setPlaceholderText("Choose a .dem recording")
    choose = QtWidgets.QPushButton("Browse…")
    row.addWidget(demo_edit, 1)
    row.addWidget(choose)
    layout.addLayout(row)
    game_edit = QtWidgets.QLineEdit(args.game_dir or "")
    game_edit.setObjectName("game_directory")
    game_edit.setPlaceholderText("CS2 game directory · auto-detect")
    layout.addWidget(game_edit)
    fullscreen = QtWidgets.QCheckBox("Fullscreen Windowed · match desktop size")
    fullscreen.setChecked(not args.windowed)
    layout.addWidget(fullscreen)
    session_status = QtWidgets.QLabel("Ready to launch · Fullscreen Windowed")
    session_status.setWordWrap(True)
    layout.addWidget(session_status)
    log_box = QtWidgets.QPlainTextEdit()
    log_box.setReadOnly(True)
    layout.addWidget(log_box, 1)
    buttons = QtWidgets.QHBoxLayout()
    start_button, stop_button = QtWidgets.QPushButton("OPEN DEMO + EDITOR"), QtWidgets.QPushButton("SAFE EXIT")
    stop_button.setObjectName("safe_exit")
    stop_button.setToolTip("Save the current camera project, close this owned session and restore temporary game settings")
    export_button = QtWidgets.QPushButton("EXPORT VIDEO…")
    export_button.setEnabled(False)
    start_button.setObjectName("primary")
    stop_button.setEnabled(False)
    buttons.addWidget(start_button, 1)
    buttons.addWidget(export_button)
    buttons.addWidget(stop_button)
    layout.addLayout(buttons)
    window.setStyleSheet("""
        QMainWindow,QWidget { background: #292929; color: #cccccc; font-size: 13px; }
        QLabel#title { font-size: 19px; font-weight: 600; color: #eeeeee; margin-bottom: 10px; }
        QLineEdit,QPlainTextEdit,QComboBox,QSpinBox { background: #202020; border: 1px solid #555555; padding: 7px; }
        QPushButton { background: #383838; border: 1px solid #666666; min-height: 36px; padding: 0 12px; }
        QPushButton:hover { background: #484848; border-color: #aaaaaa; }
        QPushButton#primary { background: #454545; color: #eeeeee; font-weight: 600; }
        QPushButton:disabled { color: #777777; background: #303030; }
        QProgressBar { border: 1px solid #666666; background: #202020; min-height: 7px; }
        QProgressBar::chunk { background: #bcbcbc; }
    """)
    events, stop_event, safe_exit_event = queue.Queue(), threading.Event(), threading.Event()
    worker = None
    closing = False
    generation = 0
    session_result = {}
    latest_state = None
    seen_export = 0
    exporting = False
    export_dialog = None
    export_job = None
    monitor_stop = threading.Event()
    library_loading = False
    game_directory_edited = bool(args.game_dir)
    selection_controls = (choose, demo_edit, game_edit, fullscreen, demo_library, refresh_library_button)

    from .export_ui import create_export_dialog, perform_export, restore_export_snapshot

    class ExportWorker(QtCore.QThread):
        progress = QtCore.Signal(object)

        def __init__(self, options, dialog, thread, result, demo, game):
            super().__init__(window)
            self.options, self.dialog, self.thread, self.result, self.demo, self.game = options, dialog, thread, result, demo, game

        def run(self):
            def wait():
                self.thread.join(timeout=120)
                if self.thread.is_alive():
                    raise LiveError("The live session has not fully stopped; capture was not launched")
                if self.result.get("error"):
                    raise LiveError("Live session restoration failed: " + self.result["error"])
            self.outcome = perform_export(self.options, demo=self.demo, game_dir=self.game, client=NativeClient(),
                                    stop_session=stop_event.set, wait_session=wait,
                                    cancel_event=self.dialog.cancel_event, notify=self.progress.emit)

    def select_library_demo(index):
        item = demo_library.itemData(index)
        if item and item.get("ready"):
            demo_edit.setText(item["path"])
            if not game_directory_edited:
                game_edit.setText(item["game_directory"])

    def mark_game_directory_edited():
        nonlocal game_directory_edited
        game_directory_edited = bool(game_edit.text().strip())

    def refresh_demo_library():
        nonlocal library_loading
        if library_loading:
            return
        library_loading = True
        refresh_library_button.setEnabled(False)
        library_status.setText("Checking Steam libraries and mounted disks…")
        explicit = game_edit.text().strip() if game_directory_edited else None
        def scan():
            from .discovery import discover_demos, find_games
            try:
                games = find_games(explicit)
                value = {"games": [str(path) for path in games], "demos": discover_demos(games)}
            except Exception as error:
                value = {"games": [], "demos": [], "error": str(error)}
            events.put(("library", (None, value)))
        threading.Thread(target=scan, name="cs2-demo-library", daemon=True).start()

    def populate_demo_library(value):
        nonlocal library_loading
        library_loading = False
        active = bool(worker and worker.is_alive()) or exporting
        refresh_library_button.setEnabled(not active)
        demo_library.blockSignals(True)
        demo_library.clear()
        selected = -1
        for item in value["demos"]:
            stamp = datetime.fromtimestamp(item["modified"]).strftime("%d %b %Y · %H:%M")
            label = (item.get("map_name") + " · " if item.get("map_name") else "")
            label += f"{stamp} · {item['size']/1024**2:.0f} MiB · {item['filename']}"
            if not item["ready"]:
                label += " · incomplete / unsupported"
            demo_library.addItem(label, item)
            index = demo_library.count()-1
            demo_library.setItemData(index, item["path"], QtCore.Qt.ItemDataRole.ToolTipRole)
            if not item["ready"]:
                demo_library.model().item(index).setEnabled(False)
            if demo_edit.text().strip() == item["path"]:
                selected = index
        if not value["demos"]:
            demo_library.addItem("No downloaded .dem files found · use Browse…", None)
        if selected<0 and demo_edit.text().strip():
            demo_library.insertItem(0,"Selected file outside library · "+Path(demo_edit.text()).name,None)
            demo_library.setItemData(0,demo_edit.text(),QtCore.Qt.ItemDataRole.ToolTipRole)
            selected=0
        if selected < 0:
            selected = next((index for index in range(demo_library.count()) if demo_library.itemData(index) and demo_library.itemData(index)["ready"]), 0)
        demo_library.setCurrentIndex(selected)
        demo_library.blockSignals(False)
        if not active and not demo_edit.text().strip():
            select_library_demo(selected)
        if not active and value["games"] and not game_directory_edited and not game_edit.text().strip():
            game_edit.setText(value["games"][0])
        ready = sum(item["ready"] for item in value["demos"])
        library_status.setText(value.get("error") or (f"{ready} ready demos · CS2: " + value["games"][0] if value["games"] else "CS2 was not found in Steam libraries. Choose its game directory or use Browse…"))

    def browse():
        initial = Path(game_edit.text())/"csgo/replays" if game_edit.text().strip() else Path.home()
        path, _ = QtWidgets.QFileDialog.getOpenFileName(window, "Open CS2 demo", str(initial), "CS2 demos (*.dem)")
        if path:
            demo_edit.setText(path)
            demo_library.blockSignals(True)
            match=next((index for index in range(demo_library.count()) if demo_library.itemData(index) and demo_library.itemData(index).get("path")==path),None)
            if match is None:
                demo_library.insertItem(0,"Selected file outside library · "+Path(path).name,None)
                demo_library.setItemData(0,path,QtCore.Qt.ItemDataRole.ToolTipRole)
                match=0
            demo_library.setCurrentIndex(match)
            demo_library.blockSignals(False)

    def launch(checked=False, resume=None):
        nonlocal worker, generation, session_result, latest_state, seen_export
        if worker and worker.is_alive():
            return
        if not demo_edit.text().strip():
            browse()
            if not demo_edit.text().strip():
                return
        stop_event.clear()
        safe_exit_event.clear()
        generation += 1
        current_generation = generation
        latest_state, seen_export = None, 0
        result_holder = {}
        session_result = result_holder
        start_button.setEnabled(False)
        stop_button.setEnabled(True)
        for control in selection_controls:
            control.setEnabled(False)
        demo_path, game_path = Path(demo_edit.text()), game_edit.text().strip() or None
        windowed = not fullscreen.isChecked()
        screen_name, screen_size = game_display(app, window)
        def run():
            try:
                def ready(client):
                    if resume:
                        events.put(("status", (current_generation, "Restoring the saved camera project…")))
                        restore_export_snapshot(client, resume)
                result = run_session(demo_path, game_dir=game_path, plugin=args.plugin, overlay=args.overlay,
                                     headless=args.headless, stop=stop_event, windowed=windowed, screen_size=screen_size,
                                     screen_name=screen_name, shot_library=args.shot_library, projects_dir=args.projects_dir,
                                     safe_exit=safe_exit_event,
                                     on_safe_exit=lambda phase, error: events.put(("safe_exit", (current_generation, (phase, error)))),
                                     on_ready=ready, notify=lambda message: events.put(("status", (current_generation, message))))
                result_holder["result"] = result
                events.put(("done", (current_generation, result)))
            except BaseException as error:
                result_holder["error"] = str(error)
                events.put(("error", (current_generation, str(error))))
        worker = threading.Thread(target=run, name="cs2-live-session", daemon=False)
        worker.start()

    def stop_session():
        nonlocal closing
        closing = True
        safe_exit_event.set()
        stop_button.setEnabled(False)
        export_button.setEnabled(False)
        session_status.setText("Saving the project before Safe exit…")

    def open_export():
        nonlocal export_dialog
        if exporting or not worker or not worker.is_alive() or not latest_state:
            return
        if export_dialog and export_dialog.isVisible():
            export_dialog.raise_()
            export_dialog.activateWindow()
            return
        export_dialog = create_export_dialog(window, latest_state.get("project", {}), start_export=start_export)
        export_dialog.show()
        export_dialog.raise_()
        export_dialog.activateWindow()

    def start_export(dialog, options):
        nonlocal export_job, exporting
        if not worker or not worker.is_alive():
            dialog.complete({"error": "The live session is no longer running"})
            return
        exporting = True
        export_button.setEnabled(False)
        stop_button.setEnabled(False)
        export_job = ExportWorker(options, dialog, worker, session_result, Path(demo_edit.text()), game_edit.text().strip() or None)
        export_job.progress.connect(dialog.report)
        export_job.progress.connect(lambda value: session_status.setText(value.get("message", "Exporting…")))
        job = export_job
        export_job.finished.connect(lambda: export_done(dialog, options, job.outcome))
        export_job.finished.connect(export_job.deleteLater)
        export_job.start()

    def export_done(dialog, options, outcome):
        nonlocal exporting
        exporting = False
        dialog.complete(outcome)
        plan = outcome.get("plan") or {}
        completed = plan.get("status") == "done" and not plan.get("error") and not outcome.get("error") and not outcome.get("cancelled")
        message = "Export complete" if completed else outcome.get("error") or plan.get("error") or "Export stopped"
        log_box.appendPlainText(message)
        session_status.setText(message)
        if options.reopen and outcome.get("stopped") and outcome.get("snapshot") and not closing:
            launch(resume=outcome["snapshot"])
        else:
            active = bool(worker and worker.is_alive())
            start_button.setEnabled(not active)
            stop_button.setEnabled(active)
            export_button.setEnabled(active and bool(latest_state))
            for control in selection_controls:
                control.setEnabled(not active)
            refresh_library_button.setEnabled(not active and not library_loading)

    def monitor():
        client = NativeClient(timeout=.4)
        while not monitor_stop.wait(.5):
            if worker and worker.is_alive() and not exporting:
                snapshot_generation = generation
                try:
                    state = client.request("get_state")
                    events.put(("state", (snapshot_generation, state)))
                except (LiveError, OSError):
                    pass
    monitor_thread = threading.Thread(target=monitor, name="cs2-editor-status", daemon=True)
    monitor_thread.start()

    def tick():
        nonlocal closing, latest_state, seen_export
        while True:
            try:
                kind, value = events.get_nowait()
            except queue.Empty:
                break
            event_generation, value = value
            if kind == "library":
                populate_demo_library(value)
                continue
            if event_generation != generation:
                continue
            if kind == "state":
                latest_state = value
                if export_dialog and export_dialog.isVisible():
                    export_dialog.refresh_project(value.get("project", {}))
                ready = value.get("camera_valid") and value.get("view_hook_installed")
                export_button.setEnabled(bool(ready) and not exporting and not closing)
                requested = value.get("export_request_id", 0)
                if requested > seen_export:
                    seen_export = requested
                    open_export()
            elif kind == "safe_exit":
                phase, error = value
                closing = phase != "error"
                active = bool(worker and worker.is_alive())
                stop_button.setEnabled(phase == "error" and active and not exporting)
                export_button.setEnabled(phase == "error" and active and bool(latest_state) and not exporting)
                if phase == "error":
                    message = "Safe exit could not save. The session is still open; retry Safe exit: " + error
                    session_status.setText(message)
                    log_box.appendPlainText(message)
            elif kind == "status":
                session_status.setText(value)
                log_box.appendPlainText(value)
            else:
                if kind == "error":
                    # A restoration error must remain visible; never report a
                    # completed safe exit while recovery files are retained.
                    closing = False
                elif isinstance(value, dict) and value.get("safe_exit"):
                    closing = True
                start_button.setEnabled(not exporting)
                stop_button.setEnabled(False)
                export_button.setEnabled(False)
                for control in selection_controls:
                    control.setEnabled(not exporting)
                refresh_library_button.setEnabled(not exporting and not library_loading)
                message = "Session closed. Ready for another demo." if kind == "done" else f"Error: {value}"
                session_status.setText(message)
                log_box.appendPlainText(message)
        if closing and not exporting and (worker is None or not worker.is_alive()):
            app.quit()

    def close_event(event):
        nonlocal closing
        if exporting or (worker and worker.is_alive()):
            closing = True
            if export_dialog and export_dialog.busy:
                export_dialog.cancel_event.set()
            stop_session()
            event.ignore()
        else:
            event.accept()
    window.closeEvent = close_event
    timer = QtCore.QTimer()
    timer.timeout.connect(tick)
    timer.start(100)
    choose.clicked.connect(browse)
    demo_library.activated.connect(select_library_demo)
    refresh_library_button.clicked.connect(refresh_demo_library)
    game_edit.textEdited.connect(mark_game_directory_edited)
    start_button.clicked.connect(launch)
    stop_button.clicked.connect(stop_session)
    export_button.clicked.connect(open_export)
    def signal_stop(signum, frame):
        nonlocal closing
        closing = True
        stop_event.set()
        if export_dialog and export_dialog.busy:
            export_dialog.cancel_event.set()
    old_handlers = {signum: signal.getsignal(signum) for signum in (signal.SIGINT, signal.SIGTERM)}
    for signum in old_handlers:
        signal.signal(signum, signal_stop)
    window.show()
    QtCore.QTimer.singleShot(0, refresh_demo_library)
    if args.start_session:
        QtCore.QTimer.singleShot(0, launch)
    try:
        return app.exec()
    finally:
        monitor_stop.set()
        monitor_thread.join(timeout=1)
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="CS2 Camera Studio: persistent -insecure demo session, in-game editor and MCP")
    parser.add_argument("--demo", type=Path, help="Local .dem recording")
    parser.add_argument("--shot-library", type=Path, help="Optional saved camera shots for this demo, shown in the in-game editor")
    parser.add_argument("--projects-dir", type=Path, help="Durable project folder shown in the in-game recent-project browser")
    parser.add_argument("--game-dir", help="CS2 installation's game directory")
    parser.add_argument("--plugin", type=Path, help="Built live libserver.so")
    parser.add_argument("--overlay", type=Path, help="Built in-game overlay library")
    parser.add_argument("--windowed", action="store_true", help="Use a normal 1280x720 window for debugging instead of Fullscreen Windowed")
    parser.add_argument("--headless", action="store_true", help="Run native control without the in-game overlay")
    parser.add_argument("--no-launcher", action="store_true", help="Run the session in this terminal")
    parser.add_argument("--start-session", action="store_true", help="Open the desktop client and immediately start --demo, retaining the export window")
    parser.add_argument("--socket", type=Path, help="Connect to an explicit native Unix socket")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--recover", action="store_true", help="Restore a verified interrupted plugin transaction after CS2 exits; retain config backups")
    mode.add_argument("--status", action="store_true", help="Read current native editor state")
    mode.add_argument("--request", choices=sorted(LIVE_METHODS), help="Call one camera method")
    mode.add_argument("--mcp", action="store_true", help="Serve the MCP stdio connector; does not launch CS2")
    parser.add_argument("--params", default="{}", help="JSON object for --request")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = argument_parser().parse_args(argv)
    try:
        if args.recover:
            print(json.dumps(recover_session(args.game_dir), ensure_ascii=False, indent=2))
            return 0
        if args.mcp:
            from .live_mcp import serve
            return serve(NativeClient(args.socket))
        if args.status or args.request:
            result = NativeClient(args.socket).request(args.request or "get_state", json.loads(args.params))
            print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
            return 0
        if args.start_session and (not args.demo or args.no_launcher):
            raise LiveError("--start-session requires --demo and the desktop launcher")
        if args.no_launcher:
            if not args.demo:
                raise LiveError("--demo is required with --no-launcher")
            def interrupt(signum, frame):
                raise KeyboardInterrupt
            signal.signal(signal.SIGTERM, interrupt)
            result = run_session(args.demo, game_dir=args.game_dir, plugin=args.plugin, overlay=args.overlay, headless=args.headless, windowed=args.windowed, shot_library=args.shot_library, projects_dir=args.projects_dir)
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0
        return graphical_launcher(args)
    except KeyboardInterrupt:
        print("Live session closed; owned game stopped and configuration restoration attempted.", file=sys.stderr)
        return 130
    except Exception as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
