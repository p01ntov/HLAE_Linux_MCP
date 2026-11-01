import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import sys
import threading
import unittest
import wave
from unittest.mock import patch

from cs2_mvp.export_ui import ExportOptions, describe_export, perform_export
from cs2_mvp.render import RenderCancelled, _publish_directory, _read_frame_hashes, _validate_png_storage, finish_capture, render_clip


def captured_job(root, *, audio=False):
    run, images = root / ".runs/png", root / "game/csgo/csdm/movie"
    run.mkdir(parents=True)
    images.mkdir(parents=True)
    header = bytearray(18)
    header[2] = 2
    header[12:16] = struct.pack("<HH", 16, 16)
    header[16:18] = bytes([24, 32])
    pixels = bytearray()
    for index in range(125):
        rgb = bytes(channel for y in range(16) for x in range(16)
                    for channel in ((255 if x % 2 else 0), (index + y) % 256, (255 if y % 2 else 0)))
        bgr = bytearray(rgb)
        bgr[0::3], bgr[2::3] = rgb[2::3], rgb[0::3]
        (images / f"png{index + 7:08d}.tga").write_bytes(header + bgr)
        pixels.extend(rgb)
    if audio:
        with wave.open(str(images / "png.wav"), "wb") as sound:
            sound.setparams((1, 2, 48000, 0, "NONE", "uncompressed"))
            sound.writeframes(bytes(range(256)) * 8)
    interpolation = "".join(f"[936] Executing: {command}\n" for command in (
        "cl_interp_ratio 1", "demo_timescale 1", "host_timescale 1", "host_force_frametime_to_equal_tick_interval 0"))
    (run / "plugin.log").write_text(interpolation + "[936] Executing: host_framerate 1000\n[1000] Executing: startmovie png\n[1008] Executing: endmovie\n")
    plan = {"environment": {"game_directory": str(root / "game")}, "movie_prefix": "png", "fps": 60,
            "capture_fps": 1000, "export_format": "png", "audio": audio, "expected_duration": .125,
            "start_tick": 1000, "end_tick": 1008, "width": 16, "height": 16, "output": str(root / "png-frames")}
    (run / "manifest.json").write_text(json.dumps(plan))
    return run, images, bytes(pixels)


@unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "ffmpeg/ffprobe required")
class PngExportTests(unittest.TestCase):
    def test_real_qhd_png_sequence_preserves_sixteen_full_resolution_rgb_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, _ = captured_job(root)
            for path in images.iterdir():
                path.unlink()
            width, height, count = 2560, 1440, 16
            header = bytearray(18)
            header[2] = 2
            header[12:16] = struct.pack("<HH", width, height)
            header[16:18] = bytes([24, 32])
            expected = []
            for index in range(count):
                rgb_row = bytes(channel for x in range(width) for channel in (
                    255 if x % 2 else 0, index * 13, x % 256))
                bgr_row = bytearray(rgb_row)
                bgr_row[0::3], bgr_row[2::3] = rgb_row[2::3], rgb_row[0::3]
                digest = hashlib.sha256()
                with (images / f"png{index:08d}.tga").open("wb") as frame:
                    frame.write(header)
                    for _ in range(height):
                        frame.write(bgr_row)
                        digest.update(rgb_row)
                expected.append(digest.hexdigest())
            plan = json.loads((run / "manifest.json").read_text())
            plan.update(width=width, height=height, end_tick=1001, expected_duration=1/64)
            (run / "manifest.json").write_text(json.dumps(plan))
            (run / "plugin.log").write_text((run / "plugin.log").read_text().replace("[1008]", "[1001]"))
            with patch("cs2_mvp.render.ROOT", root):
                result = finish_capture(run, timeout=60)
            self.assertEqual((result["media"]["width"], result["media"]["height"], result["media"]["frames"]), (width, height, count))
            self.assertEqual([digest for _, digest in _read_frame_hashes(run / "png-rgb.sha256")], expected)
            self.assertTrue(result["lossless_validation"]["verified"])

    def test_all_1000fps_samples_survive_lossless_rgb_and_ae_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, expected = captured_job(root)
            with patch("cs2_mvp.render.ROOT", root):
                result = finish_capture(run, timeout=20)
            output = Path(result["output"])
            frames = sorted(output.glob("frame_*.png"))
            self.assertEqual(len(frames), 125)
            self.assertEqual((frames[0].name, frames[-1].name), ("frame_00000000.png", "frame_00000124.png"))
            decoded = subprocess.run(["ffmpeg", "-v", "error", "-framerate", "1000", "-i", str(output / "frame_%08d.png"),
                "-f", "rawvideo", "-pix_fmt", "rgb24", "-threads", "1", "-"], check=True, capture_output=True).stdout
            self.assertEqual(hashlib.sha256(decoded).hexdigest(), hashlib.sha256(expected).hexdigest())
            self.assertEqual(decoded, expected)
            self.assertLess(sum(path.stat().st_size for path in frames), len(expected))
            metadata = json.loads((output / "sequence.json").read_text())
            self.assertEqual((metadata["capture_fps"], metadata["suggested_interpret_fps"], metadata["frames"]), (1000, 60, 125))
            self.assertAlmostEqual(metadata["interpreted_duration"], 125 / 60)
            self.assertEqual(metadata["lossless_validation"]["frames"], 125)
            self.assertTrue(metadata["lossless_validation"]["verified"])
            self.assertFalse(metadata["lossless_validation"]["temporal_resampling"])
            self.assertIn("Assume this frame rate: 60", (output / "AE_IMPORT.txt").read_text())
            self.assertFalse((output / "audio.wav").exists())
            checksums = (output / "checksums.sha256").read_text().splitlines()
            self.assertEqual(len(checksums), 125)
            self.assertEqual(checksums[0], hashlib.sha256(frames[0].read_bytes()).hexdigest() + "  frame_00000000.png")
            self.assertEqual(list(images.glob("*.tga")), [])
            self.assertEqual(result["retiming"]["mode"], "image_sequence")
            self.assertTrue(result["retiming"]["retained_for_conform"])

    def test_cancel_during_validation_keeps_raw_and_publishes_no_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, _ = captured_job(root)
            cancelled = threading.Event()
            def notify(value):
                if value["stage"] == "validation":
                    cancelled.set()
            with patch("cs2_mvp.render.ROOT", root), self.assertRaises(RenderCancelled):
                finish_capture(run, cancel_event=cancelled, notify=notify, timeout=20)
            self.assertFalse((root / "png-frames").exists())
            self.assertEqual(len(list(images.glob("*.tga"))), 125)
            self.assertEqual(list(root.glob(".cs2-camera-png-*")), [])

    def test_finalize_rejects_unsupported_ae_interpretation_without_losing_raw(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, _ = captured_job(root)
            plan = json.loads((run / "manifest.json").read_text())
            plan["fps"] = 1000
            (run / "manifest.json").write_text(json.dumps(plan))
            with patch("cs2_mvp.render.ROOT", root), self.assertRaisesRegex(ValueError, "at most 999"):
                finish_capture(run, timeout=20)
            self.assertFalse((root / "png-frames").exists())
            self.assertEqual(len(list(images.glob("*.tga"))), 125)

    def test_original_wav_is_separate_and_unchanged_when_requested(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, _ = captured_job(root, audio=True)
            audio = (images / "png.wav").read_bytes()
            with patch("cs2_mvp.render.ROOT", root):
                result = finish_capture(run, keep_raw=True, timeout=20)
            output = Path(result["output"])
            self.assertEqual((output / "audio.wav").read_bytes(), audio)
            self.assertEqual(json.loads((output / "sequence.json").read_text())["audio"]["timing"], "original_demo_time")
            self.assertEqual(len(list(images.glob("*.tga"))), 125)

    def test_publish_refuses_destination_created_during_compression(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging, destination = root / "staging", root / "output"
            staging.mkdir()
            destination.mkdir()
            (destination / "user.txt").write_text("keep")
            with self.assertRaises(FileExistsError):
                _publish_directory(staging, destination)
            self.assertEqual((destination / "user.txt").read_text(), "keep")
            self.assertTrue(staging.is_dir())

    def test_png_dry_run_never_resamples_native_samples_to_playback_fps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game, demo, camera = root / "game", root / "match.dem", root / "camera.json"
            game.mkdir()
            demo.touch()
            camera.write_text(json.dumps({"version": 1, "type": "static", "position": [0, 0, 64], "angles": [0, 0]}))
            environment = {"game_directory": str(game), "steam_running": True, "display": ":test", "cs2_pids": [],
                           "ffmpeg": "ffmpeg", "ffprobe": "ffprobe", "game_free_bytes": 10**15}
            with patch("cs2_mvp.render.doctor", return_value=environment), patch("cs2_mvp.render.parser") as parser:
                parser.return_value.parse_header.return_value = {}
                result = render_clip(demo, None, 1000, 1008, camera=camera, fps=60, capture_fps=1000,
                    export_format="png", width=320, height=240, output=root / "frames", dry_run=True)
            self.assertEqual(result["retiming"]["mode"], "image_sequence")
            self.assertAlmostEqual(result["expected_output_duration"], 125 / 60)
            self.assertEqual(result["capture_fps"], 1000)
            self.assertTrue(result["retiming"]["retained_for_conform"])


class PngExportUiTests(unittest.TestCase):
    def test_storage_is_grouped_per_filesystem_and_checks_destination_before_capture(self):
        gib = 1024**3
        game, work, destination = Path("/game"), Path("/work"), Path("/output/frames")
        free = {game: (1, 10*gib), work: (2, gib), destination.parent: (3, 100*gib)}
        with patch("cs2_mvp.render._filesystem_space", side_effect=lambda path: free[path]):
            budgets = _validate_png_storage(game, work, destination, 1000, 1920, 1080, 1, False)
        self.assertEqual(len(budgets), 3)
        self.assertGreater(sum(budget["required_bytes"] for budget in budgets), 10*gib)
        free[destination.parent] = (3, gib)
        with patch("cs2_mvp.render._filesystem_space", side_effect=lambda path: free[path]), self.assertRaisesRegex(RuntimeError, "/output"):
            _validate_png_storage(game, work, destination, 1000, 1920, 1080, 1, False)
        free[destination.parent] = (1, 10*gib)
        with patch("cs2_mvp.render._filesystem_space", side_effect=lambda path: free[path]), self.assertRaisesRegex(RuntimeError, "/game"):
            _validate_png_storage(game, work, destination, 1000, 1920, 1080, 1, False)

    @unittest.skipUnless(importlib.util.find_spec("PySide6"), "Qt runtime required")
    def test_offscreen_png_selector_selects_all_samples_and_ae_interpretation(self):
        code = """
from pathlib import Path
from PySide6 import QtWidgets
from cs2_mvp.export_ui import create_export_dialog
app = QtWidgets.QApplication([])
project = {"name":"Test", "start_tick":1000, "duration":.125,
           "keys":[{"id":"1", "time":0, "position":[0,0,64], "angles":[0,0,0], "fov":90}]}
dialog = create_export_dialog(None, project, start_export=lambda *args: None)
assert dialog.options().width == 2560 and dialog.options().height == 1440
assert dialog.resolution.currentText() == "2560 × 1440 · QHD"
assert [dialog.resolution.itemData(i) for i in range(dialog.resolution.count())] == [(2560,1440), (1920,1080), (3840,2160), (1280,720), None]
dialog.destination.setText("/tmp/test.mp4")
dialog.export_format.setCurrentIndex(1)
options = dialog.options()
assert options.export_format == "png" and options.output.name == "test-frames"
assert options.capture_fps == 1000 and options.fps == 60 and not options.audio and not options.slow_motion
assert not dialog.mode.isEnabled() and "125 lossless RGB PNG frames" in dialog.summary.text()
assert dialog.output_fps_label.text() == "AE interpretation FPS"
dialog.export_format.setCurrentIndex(0)
assert dialog.options().output.name == "test.mp4" and dialog.mode.isEnabled()
dialog.close()
print("PNG_EXPORT_GUI_PASS")
"""
        result = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True,
                                env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PNG_EXPORT_GUI_PASS", result.stdout)

    def test_summary_counts_all_samples_even_when_normal_video_mode_was_selected(self):
        project = {"name": "Test", "start_tick": 1000, "duration": .125, "keys": [
            {"id": "1", "time": 0, "position": [0, 0, 64], "angles": [0, 0, 0], "fov": 90}]}
        info = describe_export(project, ExportOptions(Path("frames"), capture_fps=1000, fps=60, export_format="png"))
        self.assertEqual((info["frames"], info["samples"], info["speed"]), (125, 125, .06))
        self.assertAlmostEqual(info["output_duration"], 125 / 60)
        with self.assertRaisesRegex(ValueError, "limited to 999"):
            describe_export(project, ExportOptions(Path("frames"), capture_fps=1000, fps=1000, export_format="png"))
        info = describe_export(project, ExportOptions(Path("frames"), capture_fps=1000, fps=999, export_format="png"))
        self.assertEqual(info["samples"], 125)


if __name__ == "__main__":
    unittest.main()
