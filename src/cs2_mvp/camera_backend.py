"""Adapter for the locally built, pinned CSDM motion extension."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

from .camera import vector
from .discovery import ROOT, sha256, CSDM_COMMIT, bundled_capture_manifest, verify_bundled_library


def ensure_motion_plugin() -> Path:
    if getattr(sys, "frozen", False):
        manifest = bundled_capture_manifest()
        expected_dependencies = {"easywsclient": "afc1d8cfc584e0f1f4a77e8c0ce3e979d9fe7ce2",
                                 "hl2sdk": "dc8e7324b1e7f3cb115b1a861280a30ba5ce72a2",
                                 "json": "a03e65420ca9a14c97080b67036859aaac06700d"}
        if (manifest.get("motion_abi") != 3 or manifest.get("environment_abi") != 1 or manifest.get("upstream_commit") != CSDM_COMMIT
                or manifest.get("dependencies") != expected_dependencies):
            raise RuntimeError("Bundled capture plugin ABI or upstream pins do not match")
        plugin = verify_bundled_library("libcamera_capture.so", manifest.get("capture_sha256"), b"CS2_MVP_MOTION_ABI 3")
        if b"CS2_MVP_ENVIRONMENT_ABI 1" not in plugin.read_bytes():
            raise RuntimeError("Bundled capture plugin failed environment ELF/ABI/hash verification")
        return plugin
    helper = ROOT / "native/build_motion.py"
    # Keep the previous chat's donor checkout read-only. This capture build
    # can evolve independently, including endmovie's held-camera boundary.
    cache = ROOT / ".cache/csdm-motion-capture"
    cache.mkdir(parents=True, exist_ok=True)
    # Run the build helper even for a cache hit: it verifies source fingerprint,
    # upstream/dependency pins, ABI marker and output hash before returning.
    with (cache / "build.log").open("a") as log:
        subprocess.run([sys.executable, str(helper), "--cache-dir", str(cache)],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    module_spec = importlib.util.spec_from_file_location("cs2_mvp_motion_builder", helper)
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    plugin = module.cached_plugin(cache)
    if plugin is None:
        raise RuntimeError(f"Native motion plugin verification failed; see {cache / 'build.log'}")
    return plugin


def verify_cosmetics_capture_support(plugin: Path | None = None) -> Path:
    plugin = plugin or ensure_motion_plugin()
    binary = plugin.read_bytes()
    if any(marker not in binary for marker in (b"CS2_MVP_MOTION_ABI 3", b"CS2_MVP_ENVIRONMENT_ABI 1",
                                                b"CS2_MVP_COSMETICS_APPLIED", b"CS2_MVP_COSMETICS_RESTORED")):
        raise RuntimeError("This capture plugin lacks the verified per-player cosmetic readback and restoration adapter")
    return plugin


def build_camera_sequence(base_sequence: dict, camera_frames: list) -> dict:
    if not isinstance(camera_frames, list) or not 1 <= len(camera_frames) <= 300000:
        raise ValueError("camera_frames must contain 1..300000 poses")
    frames = []
    for frame in camera_frames:
        if not isinstance(frame, dict) or set(frame) not in ({"position", "angles"}, {"position", "angles", "fov"}):
            raise ValueError("Each camera frame requires position, angles and optional fov")
        position = vector(frame["position"], "camera position")
        raw_angles = frame["angles"]
        if not isinstance(raw_angles, (list, tuple)) or len(raw_angles) not in {2, 3}:
            raise ValueError("Camera angles require pitch/yaw[/roll]")
        angles = vector(raw_angles, "camera angles", len(raw_angles))
        if any(abs(v) > 1_000_000 for v in (*position, *angles)) or abs(angles[0]) > (90 if len(angles) == 3 else 89.9):
            raise ValueError("Camera coordinates exceed native bounds")
        pose = {"position": list(position), "angles": list(angles)}
        if "fov" in frame:
            fov = frame["fov"]
            if isinstance(fov, bool) or not isinstance(fov, (float, int)) or not 30 <= fov <= 140:
                raise ValueError("Camera FOV must be finite and within 30..140 degrees")
            pose["fov"] = float(fov)
        frames.append(pose)
    sequence = copy.deepcopy(base_sequence)
    actions = sequence.get("actions")
    if not isinstance(actions, list):
        raise ValueError("Base camera sequence requires actions")
    starts = [a for a in actions if a.get("cmd", "").startswith("startmovie ")]
    ends = [a for a in actions if a.get("cmd") == "endmovie"]
    if len(starts) != 1 or len(ends) != 1 or starts[0]["tick"] >= ends[0]["tick"]:
        raise ValueError("A camera sequence must contain one ordered startmovie/endmovie range")
    if any(a.get("cmd") in {"camera_begin", "camera_end"} for a in actions):
        raise ValueError("Base sequence already contains camera controls")
    start = starts[0]["tick"]
    result = []
    for action in actions:
        command = action.get("cmd", "")
        if command == "cl_demo_predict 0":
            action["cmd"] = command = "cl_demo_predict 1"
        if command.split(" ", 1)[0] in {"spec_mode", "spec_player", "spec_goto"}:
            continue
        if command.startswith("startmovie "):
            result.append({"tick": action["tick"], "cmd": "camera_begin"})
        elif command == "endmovie":
            result.append({"tick": action["tick"], "cmd": "camera_end"})
        result.append(action)
    setup = max(96, start - 64)
    for command in ["spec_autodirector 0", "spec_mode 4", "cl_demoviewoverride 0", "cl_drawhud 0", "r_drawviewmodel 0", "spec_show_xray 0"]:
        result.append({"tick": setup, "cmd": command})
    for tick in [start + 4, (start + ends[0]["tick"]) // 2, ends[0]["tick"] - 4]:
        result.append({"tick": tick, "cmd": "getpos_exact"})
    sequence["actions"] = sorted(result, key=lambda a: a["tick"])
    sequence["camera_frames"] = frames
    return sequence


def validate_motion_log(log: str, expected_frames: int, expected_fov: float | dict | None = None) -> dict:
    if "CS2_MVP_MOTION_ABI 3" not in log or "CS2_MVP_CAMERA_ERROR" in log:
        raise RuntimeError("Motion extension ABI 3 was not confirmed, or reported an error")
    begins = re.findall(r"CS2_MVP_CAMERA_BEGIN expected=(\d+) tick=(\d+)", log)
    ends = re.findall(r"CS2_MVP_CAMERA_END applied=(\d+) expected=(\d+) callbacks=(\d+) overflow=(\d+) tick=(\d+) reason=(\w+)", log)
    if len(begins) != 1 or len(ends) != 1:
        raise RuntimeError("Expected one completed native camera track")
    applied, expected, callbacks, overflow, tick, reason = ends[0]
    applied, expected, callbacks, overflow, tick = map(int, [applied, expected, callbacks, overflow, tick])
    if int(begins[0][0]) != expected_frames or expected != expected_frames or reason != "camera_end":
        raise RuntimeError("Native camera track differs from the compiled camera plan")
    if abs(callbacks - expected) > 2 or abs(applied - expected) > 2 or overflow > 2:
        raise RuntimeError(f"Camera frame clock disagrees with capture: {callbacks} callbacks for {expected} poses")
    view = re.findall(r"CS2_MVP_VIEW_END writes=(\d+) fault=(\d+) tick=(\d+)", log)
    builds = re.findall(r"CS2_MVP_VIEW_HOOK linux_build=([^\s]*)", log)
    if ("CS2_MVP_CAMERA_STAGE FRAME_START_LINUX_MAIN_VIEW" not in log
            or len(builds) != 1 or builds[0] not in {
                "d5bacd2f5df4a3a68fa8bee8692337993548e14a",
                "f1e04e3616921a3bb25940eea9b4b6bfd7ccada5",
                "f00942ca6d0c486d11f988c53381c47a5be27143",
                "c6c5098eab52964d9d088279ca5cc991c76d1a7d"}
            or len(view) != 1 or int(view[0][1]) != 0 or abs(int(view[0][0]) - expected_frames) > 2
            or int(view[0][2]) != tick):
        raise RuntimeError("Late Linux main-view writes were not confirmed for every captured camera pose")
    fov_records = re.findall(r"CS2_MVP_FOV_END writes=(\d+) value=([\d.]+)", log)
    final_fov = expected_fov.get("end") if isinstance(expected_fov,dict) else expected_fov
    if expected_fov is not None and (len(fov_records) != 1
            or abs(int(fov_records[0][0]) - expected_frames) > 2
            or abs(float(fov_records[0][1]) - final_fov) > 1e-3):
        raise RuntimeError("Requested camera FOV was not confirmed by the Linux view hook")
    if isinstance(expected_fov,dict):
        stats = re.findall(r"CS2_MVP_FOV_END writes=\d+ value=[\d.]+ min=([\d.]+) max=([\d.]+)",log)
        if (len(stats)!=1 or abs(float(stats[0][0])-expected_fov["minimum"])>1e-3
                or abs(float(stats[0][1])-expected_fov["maximum"])>1e-3):
            raise RuntimeError("Animated FOV range was not confirmed by the Linux view hook")
    return {"abi": 3, "expected": expected, "applied": applied, "callbacks": callbacks,
            "overflow": overflow, "start_tick": int(begins[0][1]), "end_tick": tick,
            "view_hook": True, "view_writes": int(view[0][0]), "fov_override": expected_fov, "client_build_id": builds[0],
            "backend": "Linux CViewSetup / FRAME_START"}


def build_static_camera_sequence(base_sequence: dict, frame: dict) -> dict:
    """Apply a fixed camera once, before recording, using the stock plugin."""
    sequence = build_camera_sequence(base_sequence, [frame])
    sequence.pop("camera_frames")
    actions = [a for a in sequence["actions"] if a["cmd"] not in {"camera_begin", "camera_end"}]
    start = next(a["tick"] for a in actions if a["cmd"].startswith("startmovie "))
    values = [*frame["position"], *frame["angles"]]
    actions.append({"tick": start - 16, "cmd": "spec_goto " + " ".join(f"{v:.6f}" for v in values)})
    sequence["actions"] = sorted(actions, key=lambda a: a["tick"])
    return sequence
