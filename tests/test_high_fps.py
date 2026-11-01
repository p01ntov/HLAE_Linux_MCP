import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch
import wave

from cs2_mvp.camera import compile_camera
from cs2_mvp.camera_backend import build_camera_sequence
from cs2_mvp.csdm import build_actions
from cs2_mvp.render import finish_capture, render_clip, resolve_capture_fps, validate_capture_clock


INTERPOLATION_LOG = "".join(f"[936] Executing: {command}\n" for command in (
    "cl_interp_ratio 1", "demo_timescale 1", "host_timescale 1", "host_force_frametime_to_equal_tick_interval 0"))


class HighFpsTests(unittest.TestCase):
    def test_capture_and_output_are_distinct_and_do_not_invent_frames(self):
        self.assertEqual(resolve_capture_fps(30, None, True), 120)
        self.assertEqual(resolve_capture_fps(120, None, True), 120)
        self.assertEqual(resolve_capture_fps(1000, None, True), 1000)
        self.assertEqual(resolve_capture_fps(30, None, False), 30)
        self.assertEqual(resolve_capture_fps(60, 1000, True), 1000)
        for output, capture, camera in [(1001, None, True), (True, None, True), (60, 120.0, True),
                                        (1000, 120, True), (60, 60, True), (30, 1001, False)]:
            with self.subTest(output=output, capture=capture), self.assertRaises(ValueError):
                resolve_capture_fps(output, capture, camera)

    def test_camera_has_1000_unique_native_samples_per_demo_second(self):
        spec = {"version": 1, "type": "path", "easing": "linear", "points": [
            {"position": [0, 0, 32], "angles": [0, 0]},
            {"position": [1000, 0, 32], "angles": [0, 0]}]}
        plan = compile_camera(spec, None, 1000, 1064, 1000)
        self.assertEqual(len(plan["frames"]), 1000)
        xs = [frame["position"][0] for frame in plan["frames"]]
        self.assertEqual(len(set(xs)), 1000)
        self.assertAlmostEqual(xs[1], 1)
        self.assertAlmostEqual(xs[-1], 999)
        with self.assertRaises(ValueError):
            compile_camera(spec, None, 1000, 1064, 1001)

    def test_sidecar_accepts_high_fps_tracks_beyond_old_72000_cap(self):
        pose = {"position": [0, 0, 64], "angles": [0, 0]}
        base = build_actions(1000, 1064, "", "highfps", 1000, camera_only=True)
        sequence = build_camera_sequence(base, [pose] * 72001)
        self.assertEqual(len(sequence["camera_frames"]), 72001)
        with self.assertRaisesRegex(ValueError, "300000"):
            build_camera_sequence(base, [pose] * 300001)

    def test_interpolation_setup_is_after_demo_load_before_capture(self):
        sequence = build_actions(1000, 1008, "", "highfps", 1000, camera_only=True)
        commands = {action["cmd"]: action["tick"] for action in sequence["actions"]}
        self.assertEqual(commands["cl_interp_ratio 1"], 936)
        self.assertEqual(commands["host_framerate 1000"], 936)
        self.assertLess(commands["demo_gototick 935"], commands["cl_interp_ratio 1"])
        self.assertLess(commands["cl_interp_ratio 1"], commands["startmovie highfps"])

    def test_clock_requires_executed_setup_not_a_plan_or_encoder_tag(self):
        start_end = "[1000] Executing: startmovie highfps\n[1008] Executing: endmovie\n"
        clock = validate_capture_clock(INTERPOLATION_LOG + "[936] Executing: host_framerate 1000\n" + start_end, "highfps", 1000)
        self.assertTrue(clock["command_confirmed"])
        self.assertFalse(clock["engine_frame_time_measured"])
        self.assertFalse(clock["model_animation_verified"])
        self.assertEqual(clock["interpolation_setup"]["cl_interp_ratio"]["value"], 1)
        for log in ['{"tick":936,"cmd":"host_framerate 1000"}\n' + start_end,
                    "[936] Executing: host_framerate 120\n" + start_end,
                    "[936] Executing: host_framerate 1000\n" + start_end + "[1004] Executing: host_framerate 120\n"]:
            with self.subTest(log=log), self.assertRaises(RuntimeError):
                validate_capture_clock(log, "highfps", 1000)

    def test_clock_requires_interpolation_commands_and_rejects_mid_capture_changes(self):
        valid = INTERPOLATION_LOG + "[936] Executing: host_framerate 1000\n[1000] Executing: startmovie highfps\n[1008] Executing: endmovie\n"
        for command in ("cl_interp_ratio 1", "demo_timescale 1", "host_timescale 1", "host_force_frametime_to_equal_tick_interval 0"):
            with self.subTest(command=command), self.assertRaisesRegex(RuntimeError, "interpolation setup"):
                validate_capture_clock(valid.replace(f"[936] Executing: {command}\n", ""), "highfps", 1000)
            name, value = command.split()
            with self.subTest(change=command), self.assertRaisesRegex(RuntimeError, "interpolation setup"):
                validate_capture_clock(valid + f"[1004] Executing: {name} {0 if value == '1' else 1}\n", "highfps", 1000)

    def test_dry_run_camera_1000_is_not_silently_capped_at_120(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / "game"
            game.mkdir()
            demo, camera = root / "input.dem", root / "camera.json"
            demo.write_bytes(b"fixture")
            camera.write_text(json.dumps({"version": 1, "type": "static", "position": [0, 0, 64], "angles": [0, 0]}))
            environment = {"game_directory": str(game), "steam_running": True, "display": ":test", "cs2_pids": [],
                           "ffmpeg": "ffmpeg", "ffprobe": "ffprobe", "game_free_bytes": 10**15}
            with patch("cs2_mvp.render.doctor", return_value=environment), patch("cs2_mvp.render.parser") as parser:
                parser.return_value.parse_header.return_value = {}
                result = render_clip(demo, None, 1000, 1008, camera=camera, fps=1000, width=320, height=240, dry_run=True)
                self.assertEqual((result["fps"], result["capture_fps"], result["camera_summary"]["frames"]), (1000, 1000, 125))
                result = render_clip(demo, None, 1000, 1008, camera=camera, fps=60, capture_fps=1000,
                                     width=320, height=240, keep_raw=True, dry_run=True)
                self.assertEqual(result["capture_fps"], 1000)
                self.assertEqual(result["retiming"]["encoded_playback_speed"], 1)

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "ffmpeg/ffprobe required")
    def test_encoder_preserves_125_samples_as_1000fps_not_120fps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run = root / ".runs/probe"
            run.mkdir(parents=True)
            game = root / "game"
            images = game / "csgo/csdm/movie"
            images.mkdir(parents=True)
            header = bytearray(18)
            header[2] = 2
            header[12:16] = struct.pack("<HH", 16, 16)
            header[16:18] = bytes([24, 32])
            for index in range(125):
                (images / f"highfps{index:08d}.tga").write_bytes(header + bytes([index, 100, 200]) * 16 * 16)
            with wave.open(str(images / "highfps.wav"), "wb") as audio:
                audio.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
                audio.writeframes(bytes(6000 * 2))
            (run / "plugin.log").write_text(INTERPOLATION_LOG + "[936] Executing: host_framerate 1000\n[1000] Executing: startmovie highfps\n[1008] Executing: endmovie\n")
            plan = {"environment": {"game_directory": str(game)}, "movie_prefix": "highfps", "fps": 1000, "capture_fps": 1000,
                    "expected_duration": .125, "start_tick": 1000, "end_tick": 1008, "width": 16, "height": 16,
                    "output": str(root / "output.mp4")}
            (run / "manifest.json").write_text(json.dumps(plan))
            with patch("cs2_mvp.render.ROOT", root):
                result = finish_capture(run, keep_raw=True, timeout=20)
            self.assertEqual(result["media"]["frames"], 125)
            self.assertEqual(result["media"]["fps"], "1000/1")
            self.assertEqual(result["capture_clock_validation"]["raw_frames"], 125)
            self.assertEqual(len(list(images.glob("*.tga"))), 125)


if __name__ == "__main__":
    unittest.main()
