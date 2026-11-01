from __future__ import annotations

from contextlib import ExitStack, contextmanager
import ctypes
from datetime import datetime, timezone
import fcntl
from fractions import Fraction
import json
import math
from itertools import zip_longest
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
        manifest.append({"path": str(directory), "backup": str(backup), "existed": existed,
                         "original_files": _config_files(backup) if existed else {}})
    save_json(run / "config-backup.json", manifest)
    try:
        yield
    finally:
        restore_configs(run)


def _assert_configs_game_stopped(run: Path) -> None:
    if getattr(sys, "frozen", False):
        from .csdm import CsdmError, _assert_game_stopped
        try:
            _assert_game_stopped()
        except CsdmError as error:
            raise RuntimeError(f"CS2 has not fully exited; config backups retained at {run}") from error
    elif process_ids("cs2"):
        raise RuntimeError(f"CS2 is still running; config backups retained at {run} and have not been restored")


def _config_files(directory: Path) -> dict[str, str]:
    """Hash the bounded configuration tree without following substituted symlinks."""
    if directory.is_symlink():
        raise RuntimeError(f"Configuration directory became a symlink; backup retained: {directory}")
    if not directory.exists():
        return {}
    if not directory.is_dir():
        raise RuntimeError(f"Configuration directory is no longer a directory: {directory}")
    files = {}
    for path in directory.rglob("*"):
        if path.is_symlink():
            raise RuntimeError(f"Configuration symlink needs inspection; backup retained: {path}")
        if path.is_file():
            files[path.relative_to(directory).as_posix()] = sha256(path)
    return files


def _quiet_config_files(run: Path, manifest: list, *, quiet: float = .5, timeout: float = 3.) -> list[dict]:
    """Wait for bounded post-exit writes, including a late Steam config flush.

    This is not a lock on Steam Cloud. The subsequent restore compares each
    file again and retains recovery data if another writer is still active.
    """
    _assert_configs_game_stopped(run)
    previous = [_config_files(Path(item["path"])) for item in manifest]
    began = unchanged = time.monotonic()
    while manifest:
        now = time.monotonic()
        if now - unchanged >= quiet:
            return previous
        if now - began >= timeout:
            raise RuntimeError(f"Game configuration is still being written; backups retained at {run}")
        time.sleep(.1)
        _assert_configs_game_stopped(run)
        current = [_config_files(Path(item["path"])) for item in manifest]
        if current != previous:
            previous, unchanged = current, time.monotonic()
    return previous


def _atomic_config_copy(source: Path, destination: Path) -> None:
    """Replace one config completely; never leave half a video/config file."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".cs2-camera-" + uuid.uuid4().hex + ".tmp")
    try:
        with source.open("rb") as original, temporary.open("xb") as output:
            shutil.copyfileobj(original, output)
            output.flush()
            os.fsync(output.fileno())
        shutil.copystat(source, temporary)
        os.replace(temporary, destination)
        descriptor = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    finally:
        temporary.unlink(missing_ok=True)


def _save_config_journal(path: Path, value: dict) -> None:
    save_json(path, value)
    with path.open("rb") as stream:
        os.fsync(stream.fileno())
    descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def restore_configs(run: Path):
    """Restore the stopped session once, retaining concurrent changes for recovery.

    A durable plan prevents retrying an old session from rolling back a later
    ordinary-game edit. No Steam account or desktop/compositor setting is in
    scope; only the recorded per-game cfg trees may be changed.
    """
    manifest_path = run / "config-backup.json"
    manifest = json.loads(manifest_path.read_text())
    manifest_hash = sha256(manifest_path)
    journal_path = run / "config-restore.json"
    journal = json.loads(journal_path.read_text()) if journal_path.exists() else None
    if journal is not None:
        if journal.get("manifest_sha256") != manifest_hash:
            raise RuntimeError(f"Configuration recovery manifest changed; backups retained at {run}")
        if journal.get("status") == "restored":
            return journal
        if journal.get("status") != "restoring":
            raise RuntimeError(f"Configuration exit snapshot was not completed; backups retained for manual inspection at {run}")
        _assert_configs_game_stopped(run)
    else:
        # Record the attempt before waiting for an exit snapshot. If waiting
        # fails or the process dies, a later retry cannot mistake settings from
        # a subsequent ordinary game for the editor session's final settings.
        _save_config_journal(journal_path, {"version": 1, "status": "snapshot_pending",
                                           "manifest_sha256": manifest_hash})
        expected = _quiet_config_files(run, manifest)
        journal = {"version": 1, "status": "restoring", "manifest_sha256": manifest_hash, "directories": []}
        for index, item in enumerate(manifest):
            backup = Path(item["backup"])
            if item["existed"] and not backup.is_dir():
                raise RuntimeError(f"Configuration backup is missing; retained for inspection: {backup}")
            original = _config_files(backup) if item["existed"] else {}
            if "original_files" in item and item["original_files"] != original:
                raise RuntimeError(f"Configuration backup changed; retained for inspection: {backup}")
            journal["directories"].append({"expected": expected[index], "original": original})
        _save_config_journal(journal_path, journal)
    # Archive every changed config before any rollback. A pending journal uses
    # the same exit snapshot after interruption instead of accepting new edits.
    for index, (item, plan) in enumerate(zip(manifest, journal["directories"], strict=True)):
        for relative, checksum in plan["expected"].items():
            if plan["original"].get(relative) == checksum:
                continue
            source, saved = Path(item["path"]) / relative, run / "changed-config" / str(index) / relative
            if saved.exists():
                if saved.is_symlink() or sha256(saved) != checksum:
                    raise RuntimeError(f"Archived configuration changed; retained for inspection: {saved}")
                continue
            if not source.is_file() or source.is_symlink() or sha256(source) != checksum:
                raise RuntimeError(f"Configuration changed during exit; backup retained: {source}")
            _atomic_config_copy(source, saved)
            if sha256(saved) != checksum:
                raise RuntimeError(f"Configuration changed while archiving; backup retained: {source}")
    for item, plan in zip(manifest, journal["directories"], strict=True):
        directory, backup = Path(item["path"]), Path(item["backup"])
        current = _config_files(directory)
        original, expected = plan["original"], plan["expected"]
        if item["existed"] and _config_files(backup) != original:
            raise RuntimeError(f"Configuration backup changed; retained for inspection: {backup}")
        # Validate the entire tree before changing this directory. On a retry,
        # already restored files are accepted, but new user edits are not.
        for relative in set(current) | set(expected) | set(original):
            actual = current.get(relative)
            if actual not in (expected.get(relative), original.get(relative)):
                raise RuntimeError(f"Configuration changed after the exit snapshot; backup retained: {directory / relative}")
        for relative in sorted(set(expected) | set(original)):
            destination = directory / relative
            desired, observed = original.get(relative), current.get(relative)
            # A new path is not proof of ownership: the user or another
            # program may have created it while the session was active.
            if desired is None:
                continue
            if desired == observed:
                continue
            _assert_configs_game_stopped(run)
            # Fail closed if Steam or another writer changed the file since
            # archiving it. os.replace keeps accepted restoration atomic.
            actual = sha256(destination) if destination.is_file() and not destination.is_symlink() else None
            if actual != observed or destination.is_symlink():
                raise RuntimeError(f"Configuration changed during restoration; backup retained: {destination}")
            _atomic_config_copy(backup / relative, destination)
    # Do not report successful exit while a delayed writer has already
    # replaced the restored video/config files again. A conflicting write is
    # retained for inspection, rather than guessed to belong to this session.
    final = _quiet_config_files(run, manifest)
    for item, plan, observed in zip(manifest, journal["directories"], final, strict=True):
        preserved_new = {path: checksum for path, checksum in plan["expected"].items()
                         if path not in plan["original"]}
        if observed != {**preserved_new, **plan["original"]}:
            raise RuntimeError(f"Configuration changed after restoration; backups retained at {run}: {item['path']}")
    journal["status"] = "restored"
    _save_config_journal(journal_path, journal)
    return journal


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


def validate_resolution(width, height):
    if (type(width) is not int or type(height) is not int or width % 2 or height % 2
            or not 320 <= width <= 8192 or not 240 <= height <= 8192):
        raise ValueError("Use even integer dimensions: width 320–8192, height 240–8192")
    if width * height > 3840 * 2160:
        raise ValueError("Custom resolution exceeds the 3840×2160 total-pixel budget (8,294,400 pixels); reduce width or height")
    return width, height


def resolve_video_encoding(export_format, crf=None, video_bitrate_mbps=None):
    if export_format == "png":
        if crf is not None or video_bitrate_mbps is not None:
            raise ValueError("PNG lossless compression does not use CRF or video bitrate; leave MP4 encoding settings unset")
        return {"codec": "png", "pixel_format": "rgb24", "compression_level": 6, "prediction": "mixed", "lossless_rgb": True}
    if export_format != "mp4":
        raise ValueError("Export format must be mp4 or png")
    if crf is not None and video_bitrate_mbps is not None:
        raise ValueError("Choose either CRF quality or target video bitrate, not both")
    settings = {"codec": "libx264", "pixel_format": "yuv420p", "preset": "fast", "lossless_rgb": False}
    if video_bitrate_mbps is not None:
        if (isinstance(video_bitrate_mbps, bool) or not isinstance(video_bitrate_mbps, (int, float))
                or not math.isfinite(video_bitrate_mbps) or not .1 <= video_bitrate_mbps <= 1000):
            raise ValueError("Target video bitrate must be a finite number between 0.1 and 1000 Mbps")
        return {**settings, "rate_control": "bitrate", "target_bitrate_mbps": float(video_bitrate_mbps),
                "target_bitrate_bps": round(video_bitrate_mbps * 1_000_000)}
    crf = 18 if crf is None else crf
    if type(crf) is not int or not 0 <= crf <= 51:
        raise ValueError("MP4 CRF must be an integer between 0 and 51; lower means higher quality")
    return {**settings, "rate_control": "crf", "crf": crf}


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
    interpolation_setup = {}
    for command, required in (("cl_interp_ratio", 1), ("demo_timescale", 1), ("host_timescale", 1),
                              ("host_force_frametime_to_equal_tick_interval", 0)):
        executions = [(int(tick), value.strip()) for tick, value in re.findall(
            rf"(?:CSDM: )?\[(\d+)\] Executing: {command} ([^\r\n]+)", log)]
        before = [(tick, value) for tick, value in executions if tick < ticks["start"]]
        try:
            valid_setup = bool(before) and math.isfinite(float(before[-1][1])) and float(before[-1][1]) == required
            unchanged = all(float(value) == required for tick, value in executions
                            if ticks["start"] <= tick < ticks["end"])
        except ValueError:
            valid_setup, unchanged = False, False
        if not valid_setup or not unchanged:
            raise RuntimeError(f"Executed capture interpolation setup was not confirmed: {command} {required}")
        interpolation_setup[command] = {"value": required, "setup_tick": before[-1][0], "command_confirmed": True}
    return {"mode": "host_framerate", "requested_fps": capture_fps,
            "step_seconds": 1 / capture_fps, "setup_tick": setup_tick,
            "command_confirmed": True, "engine_frame_time_measured": False,
            "interpolation_setup": interpolation_setup, "model_animation_verified": False}


def validate_export_destination(destination: Path, export_format: str) -> None:
    if export_format not in {"mp4", "png"}:
        raise ValueError("Export format must be mp4 or png")
    if destination.exists() or destination.is_symlink():
        noun = ".mp4 output file" if export_format == "mp4" else "PNG sequence directory"
        raise ValueError(f"Choose a new {noun}; existing results are never overwritten")
    if export_format == "mp4" and destination.suffix.casefold() != ".mp4":
        raise ValueError("Choose a new .mp4 output file; existing files are never overwritten")


def estimate_capture_storage(samples, width, height, export_format="mp4"):
    raw = math.ceil(samples * width * height * 4 * 1.2 + 512 * 1024**2)
    # The engine first writes uncompressed TGA. PNGs coexist with those inputs
    # until validation/publication succeeds; compressed size is scene-dependent.
    png = math.ceil(samples * (width * height * 3 + height + 1024) * 1.02) if export_format == "png" else 0
    return {"raw": raw, "output_upper_estimate": png, "working": raw + png}


def _filesystem_space(path):
    path = path.absolute()
    while not path.exists():
        path = path.parent
    return path.stat().st_dev, shutil.disk_usage(path).free


def _validate_png_storage(game, work_root, destination, samples, width, height, duration, audio):
    storage = estimate_capture_storage(samples, width, height, "png")
    reserve = 512 * 1024**2
    # Source TGAs, per-job metadata/hashes and final PNGs can be on separate
    # disks. Add their requirements only when they share the same filesystem.
    allocations = ((game, storage["raw"] - reserve), (work_root, 0),
                   (destination.parent, storage["output_upper_estimate"] + (math.ceil(duration * 384000) if audio else 0)))
    budgets = {}
    for path, size in allocations:
        device, free = _filesystem_space(path)
        budget = budgets.setdefault(device, {"paths": [], "required_bytes": reserve, "free_bytes": free})
        budget["paths"].append(str(path))
        budget["required_bytes"] += size
        budget["free_bytes"] = min(budget["free_bytes"], free)
    for budget in budgets.values():
        if budget["required_bytes"] > budget["free_bytes"]:
            raise RuntimeError(f"Not enough free disk space at {budget['paths'][0]}: "
                               f"reserve approximately {budget['required_bytes']/1024**3:.1f} GiB for capture/export working files")
    return list(budgets.values())


def _encode_frames(command, run, output_frames, *, timeout, cancel_event, notify, description):
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
                    notify({"stage": "encoding", "message": f"{description} {frame:,} / {output_frames:,} frames",
                            "progress": frame / output_frames})
        except OSError:
            pass
    notify({"stage": "encoding", "message": f"{description} {output_frames:,} frames", "frames": output_frames})
    with (run / "ffmpeg.log").open("w") as log:
        _run_process(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout,
                     cancel_event=cancel_event, on_poll=encoding_progress)


def _rgb_frame_hashes(pattern, start_number, fps, output, *, timeout, cancel_event):
    # One fixed-size frame at a time: validation never holds the image sequence
    # in RAM, changes its cadence, or passes through a chroma-subsampled format.
    command = ["ffmpeg", "-v", "error", "-nostdin", "-xerror", "-framerate", str(fps),
               "-start_number", str(start_number), "-i", str(pattern), "-map", "0:v:0", "-an",
               "-c:v", "rawvideo", "-pix_fmt", "rgb24", "-threads", "1", "-f", "framehash", "-hash", "sha256", "-y", str(output)]
    _run_process(command, capture_output=True, timeout=timeout, cancel_event=cancel_event)


def _read_frame_hashes(path):
    with path.open() as source:
        for line in source:
            if line.startswith("#") or not line.strip():
                continue
            fields = [field.strip() for field in line.split(",")]
            if len(fields) != 6 or not re.fullmatch(r"[0-9a-f]{64}", fields[5]):
                raise RuntimeError("Malformed decoded RGB frame checksum")
            yield int(fields[4]), fields[5]


def _frame_hash_dimensions(path):
    with path.open() as source:
        for line in source:
            match = re.fullmatch(r"#dimensions 0: (\d+)x(\d+)\s*", line)
            if match:
                return tuple(int(value) for value in match.groups())
            if not line.startswith("#"):
                break
    raise RuntimeError("Decoded sequence dimensions were not reported")


def _publish_directory(staging: Path, destination: Path):
    # Linux renameat2 publishes the whole sequence atomically and refuses an
    # existing destination, including a directory created during encoding.
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(staging), -100, os.fsencode(destination), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(destination))
    descriptor = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _export_png_sequence(plan, run, destination, pattern, start_number, frames, waves, *,
                         timeout, cancel_event, notify):
    capture_fps, fps = int(plan.get("capture_fps", plan["fps"])), int(plan["fps"])
    count = len(frames)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if estimate_capture_storage(count, plan["width"], plan["height"], "png")["output_upper_estimate"] > shutil.disk_usage(destination.parent).free:
        raise RuntimeError("Not enough free space on the destination for the lossless PNG sequence; raw capture retained")
    staging = Path(tempfile.mkdtemp(prefix=".cs2-camera-png-", dir=destination.parent))
    try:
        output_pattern = staging / "frame_%08d.png"
        command = ["ffmpeg", "-v", "warning", "-nostdin", "-framerate", str(capture_fps),
                   "-start_number", str(start_number), "-i", pattern, "-map", "0:v:0", "-an",
                   "-c:v", "png", "-pix_fmt", "rgb24", "-compression_level", "6", "-pred", "mixed",
                   "-threads", "2", "-fps_mode", "passthrough", "-frames:v", str(count),
                   "-start_number", "0", str(output_pattern)]
        _encode_frames(command, run, count, timeout=timeout, cancel_event=cancel_event,
                       notify=notify, description="Compressing lossless PNG")
        expected = [f"frame_{index:08d}.png" for index in range(count)]
        if sorted(path.name for path in staging.iterdir()) != expected:
            raise RuntimeError("Exported PNG sequence contains missing or unexpected frames")
        notify({"stage": "validation", "message": "Comparing every PNG with its captured RGB frame"})
        original_hashes, png_hashes = run / "raw-rgb.sha256", run / "png-rgb.sha256"
        _rgb_frame_hashes(pattern, start_number, capture_fps, original_hashes, timeout=timeout, cancel_event=cancel_event)
        _rgb_frame_hashes(output_pattern, 0, capture_fps, png_hashes, timeout=timeout, cancel_event=cancel_event)
        if any(_frame_hash_dimensions(path) != (plan["width"], plan["height"]) for path in (original_hashes, png_hashes)):
            raise RuntimeError("CS2 did not use the requested image-sequence resolution")
        compared = 0
        for original, encoded in zip_longest(_read_frame_hashes(original_hashes), _read_frame_hashes(png_hashes)):
            _check_cancel(cancel_event)
            if original != encoded or original is None or original[0] != plan["width"] * plan["height"] * 3:
                raise RuntimeError(f"PNG lossless RGB validation failed at frame {compared}")
            compared += 1
        if compared != count:
            raise RuntimeError("PNG decoded frame count differs from the capture")
        validation = {"verified": True, "method": "decoded_rgb24_sha256", "frames": compared,
                      "temporal_resampling": False, "chroma_subsampling": False}
        audio = bool(plan.get("audio", True))
        if audio:
            with waves[0].open("rb") as source, (staging / "audio.wav").open("xb") as output:
                while chunk := source.read(1024**2):
                    _check_cancel(cancel_event)
                    output.write(chunk)
        total = 0
        with (staging / "checksums.sha256").open("x") as checksums:
            for name in expected + (["audio.wav"] if audio else []):
                _check_cancel(cancel_event)
                path = staging / name
                checksums.write(f"{sha256(path)}  {name}\n")
                total += path.stat().st_size
                with path.open("rb") as output:
                    os.fsync(output.fileno())
        metadata = {"format": "cs2-camera-png-sequence", "version": 1, "pattern": "frame_%08d.png",
                    "start_number": 0, "frames": count, "width": plan["width"], "height": plan["height"],
                    "pixel_format": "rgb24", "bit_depth": 8, "capture_fps": capture_fps,
                    "suggested_interpret_fps": fps, "source_duration": plan["expected_duration"],
                    "interpreted_duration": count / fps, "interpreted_speed": fps / capture_fps,
                    "start_tick": plan["start_tick"], "end_tick": plan["end_tick"], "tickrate": plan.get("tickrate", 64),
                    "audio": {"file": "audio.wav", "timing": "original_demo_time"} if audio else None,
                    "lossless_validation": validation, "checksums_file": "checksums.sha256",
                    "checksums_sha256": sha256(staging / "checksums.sha256")}
        save_json(staging / "sequence.json", metadata)
        (staging / "AE_IMPORT.txt").write_text(
            "CS2 Camera Studio - lossless RGB PNG sequence\n\n"
            "In After Effects choose File > Import > File, select frame_00000000.png, and enable PNG Sequence.\n"
            f"Then choose File > Interpret Footage > Main and Assume this frame rate: {fps}.\n"
            f"All {count} native frames are present. Capture rate: {capture_fps} samples per demo second.\n"
            f"At {fps} fps the sequence lasts {count/fps:.6f} seconds ({fps/capture_fps:.6f}x game speed).\n"
            "After Effects does not automatically read sequence.json; set the interpretation rate yourself.\n"
            "No resize, frame blending, optical flow, YUV conversion or chroma subsampling was applied.\n"
            "The images preserve the captured 8-bit RGB values; no HDR or alpha channel is claimed.\n"
            + ("audio.wav retains original game timing and must be retimed separately for slow motion.\n" if audio else "")
        )
        for name in ("sequence.json", "checksums.sha256", "AE_IMPORT.txt"):
            with (staging / name).open("rb") as output:
                os.fsync(output.fileno())
        descriptor = os.open(staging, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        _check_cancel(cancel_event)
        _publish_directory(staging, destination)
        plan["lossless_validation"] = validation
        plan["image_sequence"] = {**metadata, "directory": str(destination),
                                  "pattern": str(destination / "frame_%08d.png"), "bytes": total}
        plan["media"] = {"format": "png_sequence", "frames": count, "width": plan["width"], "height": plan["height"],
                         "fps": f"{fps}/1", "capture_fps": capture_fps, "duration": count / fps, "audio": audio}
        plan["expected_output_duration"] = count / fps
        plan["output_manifest_sha256"] = sha256(destination / "sequence.json")
    finally:
        if staging.exists():
            shutil.rmtree(staging)


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
    return {"duration": duration, "frames": frames, "width": video["width"], "height": video["height"], "fps": video["r_frame_rate"], "audio": bool(audios),
            "codec": video.get("codec_name"), "pixel_format": video.get("pix_fmt"),
            "bitrate_bps": int(video["bit_rate"]) if video.get("bit_rate", "").isdigit() else None}


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
            validate_media_tools(export_format=json.loads((managed_run / "manifest.json").read_text()).get("export_format", "mp4"))
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
    export_format = plan.get("export_format", "mp4")
    validate_export_destination(destination, export_format)
    encoding = resolve_video_encoding(export_format, plan.get("crf"), plan.get("video_bitrate_mbps"))
    plan["video_encoding"] = encoding
    if export_format == "png" and fps > 999:
        raise ValueError("After Effects interpretation FPS must be at most 999; set playback FPS to 60 while keeping capture_fps 1000")
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
    plan.setdefault("retiming", {}).update({"mode": "image_sequence" if export_format == "png" else "slow_motion" if slow else "normal_duration", "raw_samples_fps": capture_fps,
                                            "retained_for_conform": keep_raw or export_format == "png", "encoded_playback_speed": fps / capture_fps if slow or export_format == "png" else 1.0,
                                            "raw_conform_speed_if_reinterpreted": fps / capture_fps})
    if export_format == "png":
        _export_png_sequence(plan, run, destination, pattern, numbers[0], frames, waves,
                             timeout=timeout, cancel_event=cancel_event, notify=notify)
        plan.pop("error", None)
        plan.update({"status": "done", "export_format": "png"})
        if not keep_raw:
            for path in raw:
                path.unlink()
        save_json(run / "manifest.json", plan)
        return plan
    encoded = run / "encoded.mp4"
    encoded.unlink(missing_ok=True)
    output_frames = len(frames) if slow else round(len(frames) * fps / capture_fps)
    command = ["ffmpeg", "-v", "warning", "-nostdin", "-framerate", str(fps if slow else capture_fps), "-start_number", str(numbers[0]), "-i", pattern]
    if audio:
        command += ["-i", str(waves[0]), "-map", "0:v:0", "-map", "1:a:0", "-c:a", "aac", "-b:a", "192k", "-af", "apad"]
    else:
        command += ["-map", "0:v:0", "-an"]
    command += ["-c:v", "libx264", "-preset", "fast", "-pix_fmt", "yuv420p"]
    if encoding["rate_control"] == "bitrate":
        command += ["-b:v", str(encoding["target_bitrate_bps"])]
    else:
        command += ["-crf", str(encoding["crf"])]
    if not slow:
        command += ["-vf", f"fps=fps={fps}:start_time=0:round=near"]
    command += ["-frames:v", str(output_frames), "-t", str(output_frames / fps), "-movflags", "+faststart", str(encoded)]
    _encode_frames(command, run, output_frames, timeout=timeout, cancel_event=cancel_event,
                   notify=notify, description="Encoding")
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
                export_format: str = "mp4", crf: int | None = None, video_bitrate_mbps: float | None = None,
                cancel_event=None, notify=None) -> dict:
    from .csdm import build_actions, temporary_plugin

    _check_cancel(cancel_event)
    notify = notify or (lambda message: None)
    if not isinstance(slow_motion, bool) or not isinstance(audio, bool):
        raise ValueError("slow_motion and audio must be booleans")
    if export_format not in {"mp4", "png"}:
        raise ValueError("Export format must be mp4 or png")
    encoding = resolve_video_encoding(export_format, crf, video_bitrate_mbps)
    validate_resolution(width, height)
    if export_format == "png" and isinstance(fps, int) and fps > 999:
        raise ValueError("After Effects interpretation FPS must be at most 999; use --capture-fps 1000 and --fps 60 to retain every native sample")
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
    preserve_samples = slow_motion or export_format == "png"
    if (expected_samples if preserve_samples else round(expected_samples * fps / capture_fps)) < 2:
        raise ValueError("The selected time range yields fewer than two encoded frames; lengthen the range or increase --fps")
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
        validate_media_tools(export_format=export_format)
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
    suffix = ".mp4" if export_format == "mp4" else "-frames"
    destination = (output or ROOT / "outputs" / f"clip-{start}-{end}-{uuid.uuid4().hex[:8]}{suffix}").expanduser().absolute()
    validate_export_destination(destination, export_format)
    storage = estimate_capture_storage(expected_samples, width, height, export_format)
    estimate = storage["working"]
    storage_requirements = None
    if export_format == "png":
        storage_requirements = _validate_png_storage(game, ROOT, destination, expected_samples, width, height, duration, audio)
    elif estimate > min(environment["game_free_bytes"], shutil.disk_usage(ROOT).free):
        raise RuntimeError(f"Not enough free disk space for ~{estimate / 1024**3:.1f} GiB of raw capture")
    plan = {"demo": str(demo), "demo_header": header, "target": target, "start_tick": start,
            "end_tick": end, "tickrate": 64, "expected_duration": duration, "fps": fps,
            "capture_fps": capture_fps, "export_format": export_format, "video_encoding": encoding,
            "crf": encoding.get("crf"), "video_bitrate_mbps": encoding.get("target_bitrate_mbps"),
            "capture_clock": {"mode": "fixed_step", "host_framerate": capture_fps, "step_seconds": 1 / capture_fps},
            "retiming": {"mode": "image_sequence" if export_format == "png" else "slow_motion" if slow_motion else "normal_duration", "raw_samples_fps": capture_fps,
                         "retained_for_conform": keep_raw or export_format == "png", "encoded_playback_speed": fps / capture_fps if preserve_samples else 1.0,
                         "raw_conform_speed_if_reinterpreted": fps / capture_fps},
            "audio": audio, "expected_output_duration": expected_samples / fps if preserve_samples else round(expected_samples * fps / capture_fps) / fps,
            "width": width, "height": height, "estimated_raw_bytes": storage["raw"], "estimated_working_bytes": estimate,
            "estimated_output_upper_bytes": storage["output_upper_estimate"],
            "output": str(destination), "environment": environment}
    if storage_requirements is not None:
        plan["storage_requirements"] = storage_requirements
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
