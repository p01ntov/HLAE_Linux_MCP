from __future__ import annotations

from contextlib import ExitStack, contextmanager
from datetime import datetime, timezone
import fcntl
from fractions import Fraction
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

from .demo import parser, resolve_player
from .discovery import ROOT, doctor, ensure_plugin, process_ids, sha256, steam_roots, external_environment, validate_media_tools


class RenderCancelled(RuntimeError):
    """The user cancelled this owned export; recovery files remain available."""


def _check_cancel(event):
    if event is not None and event.is_set():
        raise RenderCancelled("Export cancelled; owned processes stopped and restoration attempted")


def _run_process(command, *, timeout=600, cancel_event=None, stdout=None, stderr=None, capture_output=False, text=False, on_poll=None):
    """Run one owned media process with prompt cancellation and bounded cleanup."""
    _check_cancel(cancel_event)
    if capture_output:
        stdout, stderr = subprocess.PIPE, subprocess.PIPE
    process = subprocess.Popen(command, stdout=stdout, stderr=stderr, text=text,
                               env=external_environment(), start_new_session=True)
    deadline = time.monotonic() + timeout
    try:
        while True:
            _check_cancel(cancel_event)
            if on_poll:
                on_poll()
            if time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(command, timeout)
            try:
                out, err = process.communicate(timeout=.15)
                if process.returncode:
                    raise subprocess.CalledProcessError(process.returncode, command, out, err)
                return subprocess.CompletedProcess(command, process.returncode, out, err)
            except subprocess.TimeoutExpired:
                continue
    except BaseException:
        for sig, wait in ((signal.SIGTERM, 2), (signal.SIGKILL, 5)):
            if process.poll() is not None:
                break
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
            try:
                process.communicate(timeout=wait)
            except subprocess.TimeoutExpired:
                pass
        raise


def save_json(path: Path, data: dict | list) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2, default=str) + "\n")
    temporary.replace(path)


@contextmanager
def config_backup(run: Path):
    """Keep a durable copy of Steam's per-game configs, even with USRLOCALCSGO."""
    directories = []
    for root in steam_roots():
        userdata = root / "userdata"
        if userdata.is_dir():
            directories.extend(account / "730/local/cfg" for account in userdata.iterdir() if account.is_dir() and account.name.isdigit())
    directories = sorted(set(path.resolve() for path in directories))
    manifest = []
    for index, directory in enumerate(directories):
        backup = run / "original-config" / str(index)
        existed = directory.exists()
        if existed:
            shutil.copytree(directory, backup)
        manifest.append({"path": str(directory), "backup": str(backup), "existed": existed})
    save_json(run / "config-backup.json", manifest)
    try:
        yield
    finally:
        restore_configs(run)


def restore_configs(run: Path):
    """Restore a durable job backup after the managed game has stopped."""
    if getattr(sys, "frozen", False):
        from .csdm import CsdmError, _assert_game_stopped
        try:
            _assert_game_stopped()
        except CsdmError as error:
            raise RuntimeError(f"CS2 has not fully exited; config backups retained at {run}") from error
    elif process_ids("cs2"):
        raise RuntimeError(f"CS2 is still running; config backups retained at {run} and have not been restored")
    manifest = json.loads((run / "config-backup.json").read_text())
    for index, item in enumerate(manifest):
        directory, backup = Path(item["path"]), Path(item["backup"])
        original = {p.relative_to(backup): p for p in backup.rglob("*") if p.is_file()} if item["existed"] else {}
        # Retain post-capture changes before rollback, including Steam Cloud
        # writes, instead of silently discarding them.
        changed = run / "changed-config" / str(index)
        for path in directory.rglob("*"):
            if path.is_file():
                relative = path.relative_to(directory)
                if relative not in original or sha256(path) != sha256(original[relative]):
                    saved = changed / relative
                    saved.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, saved)
        if not item["existed"]:
            if directory.exists():
                shutil.rmtree(directory)
            continue
        # Only the game config directory is in scope. Never touch Steam auth/session files.
        for path in directory.rglob("*"):
            if path.is_file() and path.relative_to(directory) not in original:
                path.unlink()
        for relative, source in original.items():
            destination = directory / relative
            if not destination.exists() or sha256(destination) != sha256(source):
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)

def session_processes(session_id: int) -> list[int]:
    pids = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
                if int(fields[3]) == session_id and fields[0] != "Z":
                    pids.append(int(entry.name))
            except (OSError, ValueError, IndexError):
                pass
    return pids


def stop_process(process: subprocess.Popen) -> None:
    # The SLR wrapper can exit before its descendants. Ownership is the new
    # process session we created, not the continued existence of its leader.
    def survivors():
        return sorted(set(session_processes(process.pid) + owned_game_processes(process.args)))

    for sig, timeout in [(signal.SIGTERM, 15), (signal.SIGKILL, 5)]:
        pids = survivors()
        if not pids:
            break
        for pid in pids:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + timeout
        while survivors() and time.monotonic() < deadline:
            time.sleep(.1)
    if survivors():
        raise RuntimeError("Managed runtime descendants are still alive; recovery state retained")
    process.wait(timeout=5)


def owned_game_processes(launch) -> list[int]:
    """Match only the insecure game opened with this job's private demo link.

    Steam Runtime can put the game in another process session. A process name
    alone never authorizes stopping it; the absolute private job link must match.
    """
    if not isinstance(launch, (list, tuple)) or '-insecure' not in launch or '+playdemo' not in launch:
        return []
    index = launch.index('+playdemo')
    if index + 1 >= len(launch):
        return []
    demo = Path(launch[index + 1])
    if not demo.is_absolute() or demo.name != 'input.dem' or demo.parent.parent != ROOT / '.runs':
        return []
    matches = []
    for pid in process_ids('cs2'):
        try:
            args = (Path('/proc') / str(pid) / 'cmdline').read_bytes().split(b'\0')
            if b'-insecure' in args and any(args[i:i+2] == [b'+playdemo', os.fsencode(demo)] for i in range(len(args)-1)):
                matches.append(pid)
        except (OSError, ValueError):
            continue
    return matches


def owned_raw_files(game: Path, movie: str) -> list[Path]:
    result = []
    for base in [game / "csgo/csdm/movie", game / "csgo/movie"]:
        if base.exists():
            result.extend(path for path in base.rglob(movie + "*") if path.is_file() and path.suffix.lower() in {".tga", ".wav"})
    return sorted(set(result))


def command_ticks(log: str, movie: str) -> dict:
    result = {}
    for key, command in [("start", rf"startmovie {re.escape(movie)}(?:\s|$)"), ("end", r"endmovie(?:\s|$)")]:
        matches = re.findall(rf"(?:CSDM: )?\[(\d+)\] Executing: {command}", log, flags=re.MULTILINE)
        result[key] = int(matches[-1]) if matches else None
    return result


def resolve_capture_fps(output_fps: int, requested: int | None, camera: bool) -> int:
    """Keep native time samples separate from the encoded playback rate."""
    if isinstance(output_fps, bool) or not isinstance(output_fps, int) or not 15 <= output_fps <= 1000:
        raise ValueError("Output FPS must be an integer between 15 and 1000")
    capture_fps = requested if requested is not None else max(120, output_fps) if camera else output_fps
    if isinstance(capture_fps, bool) or not isinstance(capture_fps, int) or not 15 <= capture_fps <= 1000:
        raise ValueError("Capture FPS must be an integer between 15 and 1000")
    if capture_fps < output_fps:
        raise ValueError("Capture FPS must be at least output FPS; frames are not duplicated to raise the rate")
    if camera and capture_fps < 120:
        raise ValueError("Cinematic camera capture requires at least 120 FPS for the native frame clock")
    return capture_fps


def validate_capture_clock(log: str, movie: str, capture_fps: int) -> dict:
    """Require an executed fixed-step setup, not a JSON plan or ffmpeg tag."""
    ticks = command_ticks(log, movie)
    if ticks["start"] is None or ticks["end"] is None:
        raise RuntimeError("Cannot verify capture clock without executed startmovie/endmovie")
    commands = re.findall(r"(?:CSDM: )?\[(\d+)\] Executing: host_framerate ([^\r\n]+)", log)
    before = [(int(tick), value.strip()) for tick, value in commands if int(tick) < ticks["start"]]
    if not before:
        raise RuntimeError("Native fixed-step host_framerate setup was not confirmed before capture")
    setup_tick, value = before[-1]
    try:
        valid = math.isfinite(float(value)) and float(value) == capture_fps
    except ValueError:
        valid = False
    if not valid:
        raise RuntimeError(f"Native capture clock differs from requested {capture_fps} FPS: {value}")
    for tick, value in commands:
        if ticks["start"] <= int(tick) < ticks["end"]:
            try:
                same = float(value) == capture_fps
            except ValueError:
                same = False
            if not same:
                raise RuntimeError("Native fixed-step clock changed during capture")
    return {"mode": "host_framerate", "requested_fps": capture_fps,
            "step_seconds": 1 / capture_fps, "setup_tick": setup_tick,
            "command_confirmed": True, "engine_frame_time_measured": False}


def validate_media(path: Path, expected_duration: float, fps: int, audio: bool, cancel_event=None) -> dict:
    probe = _run_process(["ffprobe", "-v", "error", "-count_frames", "-show_streams", "-show_format", "-of", "json", str(path)], capture_output=True, text=True, timeout=60, cancel_event=cancel_event)
    data = json.loads(probe.stdout)
    videos = [stream for stream in data["streams"] if stream["codec_type"] == "video"]
    audios = [stream for stream in data["streams"] if stream["codec_type"] == "audio"]
    if len(videos) != 1 or (audio and not audios) or (not audio and audios):
        raise RuntimeError("Encoded output is missing the required video/audio stream")
    video = videos[0]
    frames = int(video.get("nb_read_frames", 0))
    duration = float(video.get("duration", data["format"]["duration"]))
    if Fraction(video["r_frame_rate"]) != fps or abs(frames - expected_duration * fps) > 2 or frames < 2 or abs(duration - expected_duration) > max(2 / fps, 0.08):
        raise RuntimeError(f"Unexpected video duration/frame count: {duration}s, {frames} frames; expected about {expected_duration:.3f}s")
    if audio and abs(float(audios[0].get("duration", data["format"]["duration"])) - duration) > max(2 / fps, 0.1):
        raise RuntimeError("Audio and video durations do not match")
    _run_process(["ffmpeg", "-v", "error", "-xerror", "-i", str(path), "-f", "null", "-"], capture_output=True, timeout=600, cancel_event=cancel_event)
    return {"duration": duration, "frames": frames, "width": video["width"], "height": video["height"], "fps": video["r_frame_rate"], "audio": bool(audios)}


def collect_plugin_log(game: Path, run: Path) -> str:
    # Linux's engine changes cwd into bin/linuxsteamrt64; the proxy's log is
    # written there, independently from the launch process's stdout.
    log = game / "bin/linuxsteamrt64/csdm.log"
    if not log.is_file():
        raise RuntimeError(f"The CSDM plugin log was not created: {log}")
    text = log.read_text(errors="replace")
    (run / "plugin.log").write_text(text)
    return text


class _CaptureLogWatcher:
    """Read only bytes written to the plugin log after this launch's baseline."""

    def __init__(self, path: Path, archive: Path):
        self.path, self.archive = path, archive
        self.identity = None
        self.offset = 0
        self.anchor = b""
        self.partial = b""
        try:
            with path.open("rb") as log:
                info = os.fstat(log.fileno())
                self.identity = (info.st_dev, info.st_ino)
                self.offset = info.st_size
                log.seek(max(0, self.offset - 64))
                self.anchor = log.read(self.offset - log.tell())
        except FileNotFoundError:
            pass

    def poll(self) -> str | None:
        try:
            with self.path.open("rb") as log:
                info = os.fstat(log.fileno())
                identity = (info.st_dev, info.st_ino)
                reset = identity != self.identity or info.st_size < self.offset
                if not reset and self.anchor:
                    log.seek(self.offset - len(self.anchor))
                    reset = log.read(len(self.anchor)) != self.anchor
                if reset:
                    self.offset, self.partial = 0, b""
                self.identity = identity
                log.seek(self.offset)
                new = log.read()
                self.offset = log.tell()
                log.seek(max(0, self.offset - 64))
                self.anchor = log.read(self.offset - log.tell())
        except FileNotFoundError:
            return None
        if not new:
            return None
        with self.archive.open("ab") as saved:
            saved.write(new)
        text = (self.partial + new).decode("utf-8", errors="replace")
        # Retain a split marker across writes without retaining an unbounded line.
        self.partial = (self.partial + new).rsplit(b"\n", 1)[-1][-4096:]
        match = re.search(r"\bCS2_MVP_(?:ENVIRONMENT|CAMERA)_ERROR\b[^\r\n]*", text)
        return match.group(0).strip() if match else None


def _wait_for_capture(process, tracker, game: Path, log: _CaptureLogWatcher, *,
                      timeout: float, cancel_event=None, notify=None, failure_grace: float = 3):
    """Give a failed native capture bounded time to restore and queue its quit."""
    notify = notify or (lambda value: None)
    began, reported = time.monotonic(), 0
    failure, failed_at = None, None
    while True:
        _check_cancel(cancel_event)
        owned = tracker.observe() if tracker is not None else []
        exited = process.poll() is not None and (tracker is None or not any(
            item["name"].casefold() in {"cs2", "cs2.exe"} and item["state"] != "Z" for item in owned))
        error = log.poll()
        now = time.monotonic()
        if error and failure is None:
            failure, failed_at = error, now
            notify({"stage": "failed", "message": error, "error": error})
        if failure is not None:
            if exited or now - failed_at >= failure_grace:
                raise RuntimeError(f"{failure}; see {log.archive}")
        elif exited:
            return
        elif now - began > timeout:
            raise TimeoutError(f"CS2 render timed out after {timeout}s; see {log.archive.parent / 'game.log'}")
        elif shutil.disk_usage(game).free < 512 * 1024**2:
            raise RuntimeError("Stopping capture: less than 512 MiB free")
        elif now - reported > 2:
            reported = now
            notify({"stage": "capturing", "message": f"Recording in CS2 · {int(now-began)} seconds elapsed"})
        delay = min(.25, max(0, failure_grace - (now-failed_at))) if failure else .25
        if cancel_event is not None:
            cancel_event.wait(delay)
        else:
            time.sleep(delay)


@contextmanager
def capture_lock():
    """Keep source and packaged captures exclusive with the live editor."""
    with ExitStack() as stack:
        from .live import runtime_directory
        shared = stack.enter_context((runtime_directory() / "session.lock").open("a"))
        try:
            fcntl.flock(shared, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Another live session or render is active") from error
        ROOT.joinpath(".runs").mkdir(parents=True, exist_ok=True)
        lock = stack.enter_context(ROOT.joinpath(".runs/render.lock").open("a"))
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Another render is already active") from error
        yield


@contextmanager
def finalization_lock(run: Path):
    run = run.resolve(strict=True)
    if run.parent != ROOT.joinpath(".runs").resolve():
        raise ValueError("Only this application's job directories can be finalized")
    with (run / "finalize.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("This capture is active or already being finalized") from error
        yield run


def finish_capture(run: Path, *, keep_raw: bool = False, timeout: float = 600, cancel_event=None, notify=None) -> dict:
    with finalization_lock(run) as managed_run:
        if getattr(sys, "frozen", False):
            validate_media_tools()
        return _finish_capture(managed_run, keep_raw=keep_raw, timeout=timeout, cancel_event=cancel_event, notify=notify)


def _finish_capture(run: Path, *, keep_raw: bool = False, timeout: float = 600, cancel_event=None, notify=None) -> dict:
    """Encode an existing job; useful when encoding/validation needs a retry."""
    run = run.resolve(strict=True)
    if run.parent != ROOT.joinpath(".runs").resolve():
        raise ValueError("Only this project's job directories can be finalized")
    _check_cancel(cancel_event)
    notify = notify or (lambda message: None)
    plan = json.loads((run / "manifest.json").read_text())
    game = Path(plan["environment"]["game_directory"])
    movie, fps = plan["movie_prefix"], int(plan["fps"])
    capture_fps = int(plan.get("capture_fps", fps))
    destination = Path(plan["output"])
    if destination.exists():
        raise ValueError("Output already exists; it will not be overwritten")
    log_path = run / "plugin.log"
    if not log_path.exists():
        collect_plugin_log(game, run)
    native_log = log_path.read_text()
    if "CS2_MVP_ENVIRONMENT_ERROR" in native_log:
        raise RuntimeError(f"Native capture environment failed; see {log_path}")
    ticks = command_ticks(native_log, movie)
    plan["actual_command_ticks"] = ticks
    if None in ticks.values():
        raise RuntimeError(f"Capture commands were not confirmed by the plugin; see {log_path}")
    plan["capture_clock_validation"] = validate_capture_clock(log_path.read_text(), movie, capture_fps)
    if plan.get("capture_environment"):
        from .capture_environment import validate_environment_log
        plan["environment_validation"] = validate_environment_log(native_log, plan["capture_environment"], ticks["start"], ticks["end"])
    if plan.get("motion_abi"):
        from .camera_backend import validate_motion_log
        plan["motion_validation"] = validate_motion_log(log_path.read_text(), plan["camera_summary"]["frames"],
                                                       plan["camera_summary"].get("fov_animation") or plan["camera"].get("fov"))
    tolerance_ticks = math.ceil(64 / capture_fps)
    start, end = plan["start_tick"], plan["end_tick"]
    if not (start <= ticks["start"] <= start + tolerance_ticks and end <= ticks["end"] <= end + tolerance_ticks):
        raise RuntimeError(f"Capture command boundaries differ from requested ticks: {ticks}")
    raw = owned_raw_files(game, movie)
    frames = sorted(path for path in raw if path.suffix == ".tga")
    waves = [path for path in raw if path.suffix == ".wav"]
    audio = plan.get("audio", True)
    slow = plan.get("retiming", {}).get("mode") == "slow_motion"
    if slow and audio:
        raise ValueError("Slow-motion conform requires muted audio")
    if len(frames) < 2 or (audio and len(waves) != 1) or len(waves) > 1:
        raise RuntimeError(f"Expected TGA frames + one WAV, found {len(frames)} frames and {len(waves)} WAVs")
    numbers = [int(re.search(r"(\d{8})\.tga$", path.name).group(1)) for path in frames]
    if numbers != list(range(numbers[0], numbers[0] + len(numbers))):
        raise RuntimeError("Capture frame sequence contains gaps")
    if len(frames) != round(plan["expected_duration"] * capture_fps) and abs(len(frames) / capture_fps - plan["expected_duration"]) > 2 / capture_fps:
        raise RuntimeError("Captured frame count differs from the requested duration")
    plan["capture_clock_validation"].update({"raw_frames": len(frames), "frame_count_confirmed": True,
                                            "effective_samples_per_demo_second": len(frames) / plan["expected_duration"]})
    plan.update({"raw_files": [str(path) for path in raw], "status": "encoding"})
    save_json(run / "manifest.json", plan)
    pattern = str(frames[0].parent / f"{movie}%08d.tga")
    plan["raw_sequence"] = {"pattern": pattern, "start_number": numbers[0], "frames": len(frames),
                            "fps": capture_fps, "retained": keep_raw}
    plan.setdefault("retiming", {}).update({"mode": "slow_motion" if slow else "normal_duration", "raw_samples_fps": capture_fps,
                                            "retained_for_conform": keep_raw, "encoded_playback_speed": fps / capture_fps if slow else 1.0,
                                            "raw_conform_speed_if_reinterpreted": fps / capture_fps})
    encoded = run / "encoded.mp4"
    encoded.unlink(missing_ok=True)
    output_frames = len(frames) if slow else round(len(frames) * fps / capture_fps)
    command = ["ffmpeg", "-v", "warning", "-nostdin", "-framerate", str(fps if slow else capture_fps), "-start_number", str(numbers[0]), "-i", pattern]
    if audio:
        command += ["-i", str(waves[0]), "-map", "0:v:0", "-map", "1:a:0", "-c:a", "aac", "-b:a", "192k", "-af", "apad"]
    else:
        command += ["-map", "0:v:0", "-an"]
    command += ["-c:v", "libx264", "-preset", "fast", "-crf", "18", "-pix_fmt", "yuv420p"]
    if not slow:
        command += ["-vf", f"fps=fps={fps}:start_time=0:round=near"]
    command += ["-frames:v", str(output_frames), "-t", str(output_frames / fps), "-movflags", "+faststart", str(encoded)]
    progress_file = run / "ffmpeg-progress.txt"
    progress_file.unlink(missing_ok=True)
    command[1:1] = ["-progress", str(progress_file), "-nostats"]
    previous_progress = -1
    def encoding_progress():
        nonlocal previous_progress
        try:
            with progress_file.open("rb") as source:
                source.seek(0, os.SEEK_END)
                source.seek(max(0, source.tell()-4096))
                values = re.findall(rb"(?:^|\n)frame=(\d+)\s*(?:\n|$)", source.read())
            if values:
                frame = min(int(values[-1]), output_frames)
                if frame != previous_progress:
                    previous_progress = frame
                    notify({"stage": "encoding", "message": f"Encoding {frame:,} / {output_frames:,} frames",
                            "progress": frame / output_frames})
        except OSError:
            pass
    notify({"stage": "encoding", "message": f"Encoding {output_frames} frames at {fps} FPS", "frames": output_frames})
    with (run / "ffmpeg.log").open("w") as log:
        _run_process(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout, cancel_event=cancel_event, on_poll=encoding_progress)
    notify({"stage": "validation", "message": "Checking the exported video"})
    plan["expected_output_duration"] = output_frames / fps
    plan["media"] = validate_media(encoded, output_frames / fps, fps, audio, cancel_event)
    if (plan["media"]["width"], plan["media"]["height"]) != (plan["width"], plan["height"]):
        raise RuntimeError("CS2 did not use the requested video resolution")
    destination.parent.mkdir(parents=True, exist_ok=True)
    _check_cancel(cancel_event)
    fd, name = tempfile.mkstemp(prefix=".cs2-mvp-", suffix=".mp4", dir=destination.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "wb") as output_file, encoded.open("rb") as source:
            while True:
                _check_cancel(cancel_event)
                chunk = source.read(1024**2)
                if not chunk:
                    break
                output_file.write(chunk)
            output_file.flush()
            os.fsync(output_file.fileno())
        # Atomic, exclusive publication on the output filesystem: a partially
        # copied file never appears under the final name.
        _check_cancel(cancel_event)
        os.link(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    plan.pop("error", None)
    plan.update({"status": "done", "output_sha256": sha256(destination)})
    if not keep_raw:
        for path in raw:
            path.unlink()
    save_json(run / "manifest.json", plan)
    return plan


def render_clip(demo: Path, player: str | None, start: int | None = None, end: int | None = None, *, game_dir: str | None = None,
                output: Path | None = None, fps: int = 30, width: int = 1280, height: int = 720,
                timeout: float = 600, keep_raw: bool = False, dry_run: bool = False,
                camera: Path | None = None, capture_fps: int | None = None,
                render_scope: str | None = None, slow_motion: bool = False, audio: bool = True,
                cancel_event=None, notify=None) -> dict:
    from .csdm import build_actions, temporary_plugin

    _check_cancel(cancel_event)
    notify = notify or (lambda message: None)
    if not isinstance(slow_motion, bool) or not isinstance(audio, bool):
        raise ValueError("slow_motion and audio must be booleans")
    if slow_motion:
        audio = False
    if camera is not None and player:
        raise ValueError("Choose only one of --player and --camera")
    camera_spec = json.loads(camera.expanduser().read_text()) if camera is not None else None
    if camera is not None and not isinstance(camera_spec, dict):
        raise ValueError("Camera JSON must contain an object")
    live_project = isinstance(camera_spec, dict) and "type" not in camera_spec and "keys" in camera_spec
    from .capture_environment import validate_environment
    capture_environment = validate_environment(camera_spec or {})
    range_metadata = None
    if live_project:
        from .live_track import select_render_range
        start, end, range_metadata = select_render_range(camera_spec, start, end, render_scope)
    else:
        if render_scope is not None:
            raise ValueError("--render-scope requires a project saved by the live editor")
        if start is None or end is None:
            raise ValueError("Legacy camera plans and POV renders require both --start and --end ticks")
    if (any(isinstance(value, bool) or not isinstance(value, int) for value in (start, end))
            or start < 193 or end <= start or end > 2**31-65):
        raise ValueError("Capture requires integer end > start >= 193 and end <= 2147483583, allowing setup/preroll")
    if camera is not None and (end-start)/64 > 300:
        raise ValueError("A camera shot must be at most 300 seconds; select a shorter render region or split the scene")
    capture_fps = resolve_capture_fps(fps, capture_fps, camera is not None)
    expected_samples = math.ceil((end-start)/64 * capture_fps)
    if (expected_samples if slow_motion else round(expected_samples * fps / capture_fps)) < 2:
        raise ValueError("The selected time range yields fewer than two encoded frames; lengthen the range or increase --fps")
    if not (320 <= width <= 3840 and 240 <= height <= 2160) or width % 2 or height % 2:
        raise ValueError("Use even dimensions between 320x240 and 3840x2160")
    if timeout <= 0:
        raise ValueError("Timeout must be positive")
    if getattr(sys, "frozen", False):
        ROOT.mkdir(parents=True, exist_ok=True)
    notify({"stage": "preparing", "message": "Checking capture settings and installed tools"})
    environment = doctor(game_dir)
    _check_cancel(cancel_event)
    if not environment["steam_running"] or not environment["display"]:
        raise RuntimeError("A logged-in Steam client and DISPLAY are required")
    if environment["cs2_pids"]:
        raise RuntimeError("CS2 is already running; close it before starting an isolated render")
    if not environment["ffmpeg"] or not environment["ffprobe"]:
        raise RuntimeError("ffmpeg and ffprobe are required")
    if getattr(sys, "frozen", False):
        validate_media_tools()
    game = Path(environment["game_directory"])
    demo = demo.expanduser().resolve(strict=True)
    header = parser(demo).parse_header()
    camera_plan = None
    # FRAME_START can run at the 64 Hz simulation cadence below 64 output FPS.
    # Keep the verified 120 Hz cadence for existing low-FPS camera jobs. Higher
    # requested rates get their own native samples rather than 120 Hz images
    # duplicated by the encoder. Raw count/view-write guards validate each run.
    # This is not a claim that FRAME_START is an actual output-frame hook.
    if camera is not None:
        from .camera import compile_camera
        composition = camera_spec.get("composition")
        if composition is not None:
            if not isinstance(composition, dict) or not math.isclose(
                    composition.get("aspect_ratio", 0), width / height, rel_tol=1e-6):
                raise ValueError("Composed camera aspect_ratio must match capture width/height")
        camera_plan = compile_camera(camera_spec, demo, start, end, capture_fps)
        target = None
    else:
        if not player:
            raise ValueError("Choose either --player for POV or --camera for a cinematic shot")
        target = resolve_player(demo, player, start, end)
    duration = (end - start) / 64
    # A static shot also needs the late main-view override: a one-shot
    # spec_goto is overwritten by the demo's observer processing.
    uses_motion = camera_plan is not None
    estimate = math.ceil(duration * capture_fps * width * height * 4 * 1.2 + 512 * 1024**2)
    if estimate > min(environment["game_free_bytes"], shutil.disk_usage(ROOT).free):
        raise RuntimeError(f"Not enough free disk space for ~{estimate / 1024**3:.1f} GiB of raw capture")
    destination = (output or ROOT / "outputs" / f"clip-{start}-{end}-{uuid.uuid4().hex[:8]}.mp4").expanduser().resolve()
    if destination.suffix.lower() != ".mp4" or destination.exists():
        raise ValueError("Output must be a new .mp4 file; existing results are never overwritten")
    plan = {"demo": str(demo), "demo_header": header, "target": target, "start_tick": start,
            "end_tick": end, "tickrate": 64, "expected_duration": duration, "fps": fps,
            "capture_fps": capture_fps,
            "capture_clock": {"mode": "fixed_step", "host_framerate": capture_fps, "step_seconds": 1 / capture_fps},
            "retiming": {"mode": "slow_motion" if slow_motion else "normal_duration", "raw_samples_fps": capture_fps,
                         "retained_for_conform": keep_raw, "encoded_playback_speed": fps / capture_fps if slow_motion else 1.0,
                         "raw_conform_speed_if_reinterpreted": fps / capture_fps},
            "audio": audio, "expected_output_duration": expected_samples / fps if slow_motion else round(expected_samples * fps / capture_fps) / fps,
            "width": width, "height": height, "estimated_raw_bytes": estimate,
            "output": str(destination), "environment": environment}
    if capture_environment:
        plan["capture_environment"] = capture_environment
        plan["environment_abi"] = 1
    if range_metadata is not None:
        plan["render_range"] = range_metadata
    if camera_plan is not None:
        plan.update({"camera": camera_spec, "camera_summary": camera_plan["summary"],
                     "camera_backend": "motion_extension" if uses_motion else "static_command"})
        if uses_motion:
            plan["motion_abi"] = 3
    if dry_run:
        return plan
    destination.parent.mkdir(parents=True, exist_ok=True)
    _check_cancel(cancel_event)
    with capture_lock():
        run = ROOT / ".runs" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
        run.mkdir(mode=0o700)
        with finalization_lock(run):
            movie = "cs2mvp_" + uuid.uuid4().hex[:16]
            # A job-owned symlink makes the JSON sidecar private and leaves the original demo untouched.
            demo_link = run / "input.dem"
            demo_link.symlink_to(demo)
            if camera_plan is not None:
                from .camera_backend import build_camera_sequence, build_static_camera_sequence
                base = build_actions(start, end, "", movie, capture_fps, camera_only=True)
                actions = (build_camera_sequence(base, camera_plan["frames"]) if uses_motion
                           else build_static_camera_sequence(base, camera_plan["frames"][0]))
            else:
                actions = build_actions(start, end, target["steam_id"], movie, capture_fps, player_slot=target["slot"])
            if capture_environment:
                from .capture_environment import build_environment_sequence
                actions = build_environment_sequence(actions, capture_environment)
            save_json(run / "input.dem.json", [actions])
            plan.update({"run_directory": str(run), "movie_prefix": movie, "demo_sha256": sha256(demo), "status": "prepared"})
            save_json(run / "manifest.json", plan)
            if camera_plan is not None or capture_environment:
                from .camera_backend import ensure_motion_plugin
                plugin = ensure_motion_plugin()
            else:
                plugin = ensure_plugin()
            catalog_path = None
            if capture_environment.get("cosmetics"):
                from .camera_backend import verify_cosmetics_capture_support
                from .cosmetics_catalog import stage_catalog
                verify_cosmetics_capture_support(plugin)
                catalog_path = stage_catalog(game, run / "cosmetics-catalog.json")
                plan["cosmetics_catalog_sha256"] = sha256(catalog_path)
            plan["loaded_plugin_sha256"] = sha256(plugin)
            save_json(run / "manifest.json", plan)
            launch = [environment["runtime"], "--verb=waitforexitandrun", "--", str(game / "cs2.sh"),
                      "-insecure", "-novid", "-sw", "-w", str(width), "-h", str(height),
                      "-con_logfile", str(run / "console.log"),
                      "+demo_ui_mode", "0",
                      "+playdemo", str(demo_link)]
            env = external_environment()
            if catalog_path is not None:
                env.update({"CS2_CAMERA_CAPTURE_COSMETICS": "1", "CS2_CAMERA_COSMETICS_CATALOG": str(catalog_path)})
            profile = run / "config"
            profile.mkdir()
            env.update({"USRLOCALCSGO": str(profile), "SteamAppId": "730", "SteamGameId": "730"})
            process = None
            tracker = None
            game_stopped = False
            def stop_game():
                nonlocal game_stopped
                if process is None or game_stopped:
                    return
                if tracker is not None:
                    from .live import _stop_owned
                    _stop_owned(process, demo_link, tracker)
                else:
                    stop_process(process)
                game_stopped = True
            log_path = run / "game.log"
            try:
                _check_cancel(cancel_event)
                with config_backup(run), temporary_plugin(game, plugin, launch_args=launch, expected_sha256=plan["loaded_plugin_sha256"]):
                    save_json(run / "launch.json", {"argv": launch, "USRLOCALCSGO": str(profile)})
                    with log_path.open("w") as log:
                        native_log = _CaptureLogWatcher(game / "bin/linuxsteamrt64/csdm.log", run / "plugin.log")
                        process = subprocess.Popen(launch, cwd=game, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                        try:
                            from .live import OwnedProcesses
                            tracker = OwnedProcesses(process.pid, demo_link)
                            plan.update({"status": "capturing", "pid": process.pid})
                            save_json(run / "manifest.json", plan)
                            _wait_for_capture(process, tracker, game, native_log, timeout=timeout,
                                              cancel_event=cancel_event, notify=notify)
                        finally:
                            stop_game()
                            native_log.poll()
                plan["game_exit_code"] = process.returncode
                save_json(run / "manifest.json", plan)
                collect_plugin_log(game, run)
                return _finish_capture(run, keep_raw=keep_raw, timeout=timeout, cancel_event=cancel_event, notify=notify)
            except BaseException as error:
                plan = json.loads((run / "manifest.json").read_text())
                plan.update({"status": "cancelled" if isinstance(error, (RenderCancelled, KeyboardInterrupt)) else "failed", "error": f"{type(error).__name__}: {error}"})
                save_json(run / "manifest.json", plan)
                raise
            finally:
                stop_game()
