from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.export_ui import ExportOptions, describe_export
from cs2_mvp.render import finish_capture, resolve_video_encoding, validate_resolution


PROJECT = {"name": "Custom export", "start_tick": 1000, "duration": .25,
           "keys": [{"id": "1", "time": 0, "position": [0, 0, 64], "angles": [0, 0, 0], "fov": 90}]}


class ExportSettingsTests(unittest.TestCase):
    def test_custom_resolution_accepts_portrait_and_rejects_bad_dimensions_or_pixel_budget(self):
        for size in ((2560, 1440), (1440, 2560), (2160, 3840), (3840, 2160), (8192, 1012), (320, 240)):
            self.assertEqual(validate_resolution(*size), size)
        for size in ((True, 1080), (1920.0, 1080), (319, 240), (320, 239), (1441, 2560),
                     (1440, 2561), (8194, 240), (320, 8194), (8192, 1024), (7680, 4320)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                validate_resolution(*size)
        info = describe_export(PROJECT, ExportOptions(Path("portrait.mp4"), width=1440, height=2560, video_bitrate_mbps=12.5))
        self.assertEqual(info["video_encoding"]["target_bitrate_bps"], 12_500_000)

    def test_video_rate_controls_are_mutually_exclusive_typed_and_not_applied_to_png(self):
        self.assertEqual(resolve_video_encoding("mp4")["crf"], 18)
        self.assertEqual(resolve_video_encoding("mp4", crf=0)["crf"], 0)
        self.assertFalse(resolve_video_encoding("mp4", crf=0)["lossless_rgb"])
        for crf in (True, 18.0, -1, 52):
            with self.subTest(crf=crf), self.assertRaises(ValueError):
                resolve_video_encoding("mp4", crf=crf)
        for rate in (True, 0, .09, 1000.01, float("nan"), float("inf"), "20"):
            with self.subTest(rate=rate), self.assertRaises(ValueError):
                resolve_video_encoding("mp4", video_bitrate_mbps=rate)
        with self.assertRaisesRegex(ValueError, "not both"):
            resolve_video_encoding("mp4", crf=18, video_bitrate_mbps=20)
        for parameters in ({"crf": 18}, {"video_bitrate_mbps": 20}):
            with self.subTest(parameters=parameters), self.assertRaisesRegex(ValueError, "does not use"):
                resolve_video_encoding("png", **parameters)
        png = describe_export(PROJECT, ExportOptions(Path("frames"), export_format="png"))
        self.assertTrue(png["video_encoding"]["lossless_rgb"])
        self.assertNotIn("rate_control", png["video_encoding"])

    def test_cli_forwards_custom_resolution_and_one_rate_control(self):
        from cs2_mvp.cli import main
        args = ["cs2-mvp", "render", "match.dem", "--camera", "camera.json", "--width", "1440", "--height", "2560",
                "--video-bitrate-mbps", "12.5", "--dry-run"]
        with patch.object(sys, "argv", args), patch("cs2_mvp.render.render_clip", return_value={}) as render, redirect_stdout(io.StringIO()):
            self.assertEqual(main(), 0)
        self.assertEqual((render.call_args.kwargs["width"], render.call_args.kwargs["height"]), (1440, 2560))
        self.assertEqual(render.call_args.kwargs["video_bitrate_mbps"], 12.5)
        self.assertIsNone(render.call_args.kwargs["crf"])
        with patch.object(sys, "argv", args + ["--crf", "18"]), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            main()

    @unittest.skipUnless(importlib.util.find_spec("PySide6"), "Qt runtime required")
    def test_offscreen_custom_resolution_rate_controls_and_png_lossless_mode(self):
        code = """
from PySide6 import QtWidgets
from cs2_mvp.export_ui import create_export_dialog
app = QtWidgets.QApplication([])
project = {"name":"Test", "start_tick":1000, "duration":.25,
           "keys":[{"id":"1", "time":0, "position":[0,0,64], "angles":[0,0,0], "fov":90}]}
dialog = create_export_dialog(None, project, start_export=lambda *args: None)
dialog.show()
app.processEvents()
assert dialog.options().width == 2560 and dialog.options().height == 1440 and dialog.options().crf == 18
dialog.resolution.setCurrentIndex(dialog.resolution.findData(None))
dialog.custom_width.setValue(1440)
dialog.custom_height.setValue(2560)
assert dialog.custom_dimensions.isVisible() and dialog.render_button.isEnabled()
assert (dialog.options().width, dialog.options().height) == (1440,2560)
dialog.quality_mode.setCurrentIndex(1)
dialog.bitrate.setValue(12.5)
assert dialog.options().crf is None and dialog.options().video_bitrate_mbps == 12.5
assert dialog.bitrate.isVisible() and not dialog.crf.isVisible()
dialog.export_format.setCurrentIndex(1)
app.processEvents()
dialog.adjustSize()
app.processEvents()
assert dialog.options().crf is None and dialog.options().video_bitrate_mbps is None
assert not dialog.quality_mode.isVisible() and not dialog.bitrate.isEnabled() and dialog.png_encoding_info.isVisible()
assert "no bitrate" in dialog.png_encoding_info.text()
assert dialog.summary.height() >= dialog.summary.heightForWidth(dialog.summary.width())
assert (dialog.options().width,dialog.options().height) == (1440,2560)
dialog.custom_width.setValue(8192)
dialog.custom_height.setValue(8192)
assert not dialog.render_button.isEnabled() and "total-pixel budget" in dialog.summary.text()
dialog.custom_width.setValue(1440)
dialog.custom_height.setValue(2560)
dialog.export_format.setCurrentIndex(0)
assert dialog.options().video_bitrate_mbps == 12.5 and dialog.render_button.isEnabled()
dialog.quality_mode.setCurrentIndex(0)
dialog.crf.setValue(23)
assert dialog.options().crf == 23 and dialog.options().video_bitrate_mbps is None
dialog.close()
print("CUSTOM_EXPORT_GUI_PASS")
"""
        result = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True,
                                env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CUSTOM_EXPORT_GUI_PASS", result.stdout)

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "ffmpeg/ffprobe required")
    def test_real_portrait_mp4_uses_requested_abr_or_crf_and_reports_measured_bitrate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images = root / ".runs/custom", root / "game/csgo/csdm/movie"
            run.mkdir(parents=True)
            images.mkdir(parents=True)
            header = bytearray(18)
            header[2] = 2
            header[12:16] = struct.pack("<HH", 320, 480)
            header[16:18] = bytes([24, 32])
            for index in range(30):
                row = bytes(channel for x in range(320) for channel in ((x + index * 7) % 256, x % 256, index * 5))
                (images / f"custom{index:08d}.tga").write_bytes(header + row * 480)
            setup = "".join(f"[936] Executing: {command}\n" for command in (
                "cl_interp_ratio 1", "demo_timescale 1", "host_timescale 1", "host_force_frametime_to_equal_tick_interval 0",
                "host_force_max_frametime_to_tick_interval 0", "host_framerate 120"))
            (run / "plugin.log").write_text(setup + "[1000] Executing: startmovie custom\n[1016] Executing: endmovie\n")
            plan = {"environment": {"game_directory": str(root / "game")}, "movie_prefix": "custom", "fps": 60,
                    "capture_fps": 120, "expected_duration": .25, "start_tick": 1000, "end_tick": 1016,
                    "width": 320, "height": 480, "audio": False, "output": str(root / "bitrate.mp4"), "video_bitrate_mbps": 1.25}
            (run / "manifest.json").write_text(json.dumps(plan))
            with patch("cs2_mvp.render.ROOT", root):
                bitrate = finish_capture(run, keep_raw=True, timeout=30)
            self.assertEqual(bitrate["video_encoding"]["target_bitrate_bps"], 1_250_000)
            self.assertEqual(bitrate["video_encoding"]["rate_control"], "bitrate")
            self.assertGreater(bitrate["media"]["bitrate_bps"], 0)
            self.assertEqual((bitrate["media"]["width"], bitrate["media"]["height"], bitrate["media"]["frames"]), (320, 480, 15))
            self.assertEqual((bitrate["media"]["codec"], bitrate["media"]["pixel_format"]), ("h264", "yuv420p"))
            self.assertIn(b"rc=abr", (root / "bitrate.mp4").read_bytes())
            self.assertIn(b"bitrate=1250", (root / "bitrate.mp4").read_bytes())
            plan.update(output=str(root / "crf.mp4"), video_bitrate_mbps=None, crf=23)
            (run / "manifest.json").write_text(json.dumps(plan))
            with patch("cs2_mvp.render.ROOT", root):
                crf = finish_capture(run, keep_raw=True, timeout=30)
            self.assertEqual(crf["video_encoding"]["crf"], 23)
            self.assertIn(b"rc=crf", (root / "crf.mp4").read_bytes())
            self.assertIn(b"crf=23.0", (root / "crf.mp4").read_bytes())
            self.assertEqual(len(list(images.glob("*.tga"))), 30)


if __name__ == "__main__":
    unittest.main()
