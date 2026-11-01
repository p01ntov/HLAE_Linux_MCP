"""Real image/FFmpeg integration coverage for color lens preview/export parity."""
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import threading
import unittest
import wave
from unittest.mock import patch

import numpy as np

from cs2_mvp.lens_effects import apply_lens, lens_active, read_tga, validate_lens, write_tga
from cs2_mvp.render import (RenderCancelled, _lens_input_sequence, _validate_png_storage,
                            estimate_capture_storage, finish_capture, render_clip)


def _decode(path, *, image=False):
    command = ["ffmpeg", "-v", "error", "-i", str(path)]
    if image:
        command += ["-frames:v", "1"]
    return subprocess.run(command + ["-f", "rawvideo", "-pix_fmt", "rgb24", "-threads", "1", "-"],
                          capture_output=True, check=True, timeout=20).stdout


def _source(width=128, height=72):
    y, x = np.indices((height, width))
    return np.stack((32 + x * 210 // (width - 1), 16 + y * 220 // (height - 1),
                     np.where((x // 7 + y // 5) % 2, 230, 30)), axis=2).astype(np.uint8)


def _tga(rgb, *, origin=0x20, depth=24, rle=False):
    """Independent fixture writer, including RLE packets crossing scanlines."""
    height, width = rgb.shape[:2]
    ordered = rgb
    if not origin & 0x20:
        ordered = ordered[::-1]
    if origin & 0x10:
        ordered = ordered[:, ::-1]
    ordered = ordered[:, :, ::-1]
    if depth == 32:
        ordered = np.concatenate((ordered, np.full((height, width, 1), 93, dtype=np.uint8)), axis=2)
    flat = ordered.reshape(-1, depth // 8)
    header = bytearray(18)
    header[:3] = bytes((3, 0, 10 if rle else 2))
    header[12:16] = struct.pack("<HH", width, height)
    header[16:18] = bytes((depth, origin | (8 if depth == 32 else 0)))
    body = bytearray()
    if rle:
        offset = 0
        while offset < len(flat):
            # Emit both run and literal packets, including repeated scanline edges.
            count = 1
            while count < 128 and offset + count < len(flat) and np.array_equal(flat[offset], flat[offset + count]):
                count += 1
            if count > 1:
                body.extend(bytes((0x80 | (count - 1),)) + flat[offset].tobytes())
            else:
                count = min(3, len(flat) - offset)
                body.extend(bytes((count - 1,)) + flat[offset:offset + count].tobytes())
            offset += count
    else:
        body.extend(flat.tobytes())
    return bytes(header) + b"tag" + body


def _captured_job(root, *, export_format="png", lens=None, audio=False, slow=False):
    run, images = root / ".runs/lens", root / "game/csgo/csdm/movie"
    run.mkdir(parents=True)
    images.mkdir(parents=True)
    width, height, count = 64, 48, 30
    source = _source(width, height)
    raw = []
    for index in range(count):
        frame = source.copy()
        frame[:, :, 1] = (frame[:, :, 1].astype(int) + index * 3).astype(np.uint8)
        raw.append(frame)
        (images / f"lens{index + 7:08d}.tga").write_bytes(_tga(frame, depth=32 if index % 2 else 24,
                                                              origin=(index % 4) << 4, rle=bool(index % 3)))
    if audio:
        with wave.open(str(images / "lens.wav"), "wb") as sound:
            sound.setparams((1, 2, 48000, 0, "NONE", "uncompressed"))
            sound.writeframes(bytes(12000 * 2))
    setup = "".join(f"[936] Executing: {command}\n" for command in (
        "cl_interp_ratio 1", "demo_timescale 1", "host_timescale 1", "host_force_frametime_to_equal_tick_interval 0",
        "host_force_max_frametime_to_tick_interval 0", "host_framerate 120"))
    (run / "plugin.log").write_text(setup + "[1000] Executing: startmovie lens\n[1016] Executing: endmovie\n")
    output = root / ("output.mp4" if export_format == "mp4" else "output-frames")
    plan = {"environment": {"game_directory": str(root / "game")}, "movie_prefix": "lens", "fps": 60,
            "capture_fps": 120, "export_format": export_format, "audio": audio, "expected_duration": .25,
            "start_tick": 1000, "end_tick": 1016, "width": width, "height": height, "output": str(output),
            "retiming": {"mode": "slow_motion" if slow else "normal_duration"}, "crf": 8}
    if export_format == "png":
        plan.pop("crf")
    if lens is not None:
        plan["lens"] = lens
    (run / "manifest.json").write_text(json.dumps(plan))
    return run, images, raw


class LensSettingsTests(unittest.TestCase):
    def test_schema_defaults_ranges_and_strict_json_types(self):
        self.assertEqual(validate_lens({}), {"enabled": False, "distortion": 0., "vignette": 0., "chromatic": 0.})
        self.assertEqual(validate_lens({"enabled": True, "distortion": -.75, "vignette": 1, "chromatic": 8}),
                         {"enabled": True, "distortion": -.75, "vignette": 1., "chromatic": 8.})
        invalid = [None, [], True, {"enabled": 1}, {"enabled": "false"}, {"depth": 1}, {"distortion": -.751},
                   {"distortion": .751}, {"vignette": -1}, {"vignette": 1.001}, {"chromatic": 8.001},
                   {"chromatic": -1}, {"vignette": "1"}, {"vignette": True}, {"chromatic": None},
                   {"distortion": math.nan}, {"vignette": math.inf}, {"chromatic": -math.inf}, {"vignette": 10**999}]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_lens(value)
        self.assertFalse(lens_active({"enabled": False, "vignette": 1}))
        self.assertFalse(lens_active({"enabled": True}))
        self.assertTrue(lens_active({"enabled": True, "vignette": .1}))

    def test_disabled_or_neutral_is_bit_identical_without_allocating_a_new_frame(self):
        rgb = _source()[:, ::-1]
        original = rgb.copy()
        for settings in ({}, {"enabled": True}, {"enabled": False, "chromatic": 8, "vignette": 1, "distortion": .75}):
            self.assertIs(apply_lens(rgb, settings), rgb)
            np.testing.assert_array_equal(rgb, original)

    def test_processing_is_tiled_source_immutable_and_cancellable_within_a_frame(self):
        rgb = _source(96, 129)
        original = rgb.copy()
        settings = {"enabled": True, "distortion": .75, "vignette": .85, "chromatic": 8}
        one_row = apply_lens(rgb, settings, tile_rows=1)
        np.testing.assert_array_equal(one_row, apply_lens(rgb, settings, tile_rows=1024))
        np.testing.assert_array_equal(rgb, original)
        calls = 0
        def cancel():
            nonlocal calls
            calls += 1
            if calls == 3:
                raise RenderCancelled("cancel inside pixel processing")
        with self.assertRaises(RenderCancelled):
            apply_lens(rgb, settings, tile_rows=1, check_cancel=cancel)
        self.assertEqual(calls, 3)

    def test_real_vulkan_readbacks_match_python_reference(self):
        directory = Path(__file__).parent / "fixtures/lens_gpu"
        manifest = json.loads((directory / "fixtures.json").read_text())
        for item in manifest["fixtures"]:
            with self.subTest(item=item["file"]):
                data = (directory / item["file"]).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
                magic, size, depth, pixels = data.split(b"\n", 3)
                self.assertEqual((magic, depth), (b"P6", b"255"))
                width, height = map(int, size.split())
                gpu = np.frombuffer(pixels, dtype=np.uint8).reshape(height, width, 3)
                expected = apply_lens(_source(width, height), item["settings"])
                error = np.abs(expected.astype(np.int16) - gpu.astype(np.int16))
                self.assertLessEqual(error.max(), 2)
                if not lens_active(item["settings"]):
                    self.assertEqual(error.max(), 0)

    def test_vignette_is_in_linear_light_and_preserves_center(self):
        rgb = np.full((9, 9, 3), 255, dtype=np.uint8)
        out = apply_lens(rgb, {"enabled": True, "vignette": 1})
        self.assertEqual(out[4, 4, 0], 255)
        r2 = (8 / 9) ** 2
        smooth = (r2 - .2) / .8
        linear = 1 - smooth * smooth * (3 - 2 * smooth)
        expected = round((1.055 * linear ** (1 / 2.4) - .055) * 255)
        self.assertEqual(out[0, 0, 0], expected)
        self.assertGreater(out[0, 0, 0], round(linear * 255) + 40)


class LensTgaTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg required")
    def test_tga_raw_rle_origins_and_alpha_match_real_ffmpeg(self):
        rgb = np.array([[[91, 72, 250], [91, 72, 250], [91, 72, 250], [0, 255, 120]],
                        [[0, 255, 120], [0, 255, 120], [255, 0, 19], [37, 92, 173]]], dtype=np.uint8)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input.tga"
            for depth in (24, 32):
                for rle in (False, True):
                    for origin in (0, 0x10, 0x20, 0x30):
                        with self.subTest(depth=depth, rle=rle, origin=origin):
                            path.write_bytes(_tga(rgb, origin=origin, depth=depth, rle=rle))
                            actual = read_tga(path, (4, 2))
                            np.testing.assert_array_equal(actual, rgb)
                            self.assertEqual(actual.tobytes(), _decode(path, image=True))
            output = Path(temporary) / "output.tga"
            write_tga(output, rgb[:, ::-1])
            self.assertEqual(_decode(output, image=True), rgb[:, ::-1].tobytes())

    def test_malformed_tga_rejected_before_unbounded_allocation(self):
        rgb = _source(8, 4)
        valid = _tga(rgb)
        invalid = [valid[:17], valid[:-1], valid[:2] + bytes((1,)) + valid[3:],
                   valid[:12] + struct.pack("<HH", 65535, 65535) + valid[16:],
                   valid[:16] + bytes((16,)) + valid[17:], valid[:17] + bytes((0xE0,)) + valid[18:]]
        overrun = bytearray(valid[:21])
        overrun[2] = 10
        overrun += bytes((0xFF, 0, 0, 0))
        invalid.extend((bytes(overrun), _tga(rgb, rle=True)[:-1]))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.tga"
            for data in invalid:
                path.write_bytes(data)
                with self.subTest(data=data[:18]), self.assertRaises(ValueError):
                    read_tga(path, (8, 4))
            path.write_bytes(valid[:12] + struct.pack("<HH", 65535, 65535) + valid[16:])
            with self.assertRaisesRegex(ValueError, "pixel budget"):
                read_tga(path, (65535, 65535))


@unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "ffmpeg/ffprobe required")
class LensExportTests(unittest.TestCase):
    settings = {"enabled": True, "distortion": -.6, "vignette": .4, "chromatic": 4}

    def test_real_png_export_keeps_every_processed_rgb_sample_original_audio_and_raw_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, raw = _captured_job(root, lens=self.settings, audio=True)
            raw_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in images.iterdir()}
            with patch("cs2_mvp.render.ROOT", root):
                result = finish_capture(run, keep_raw=True, timeout=30)
            output = Path(result["output"])
            decoded = _decode(output / "frame_%08d.png")
            self.assertEqual(decoded, b"".join(apply_lens(frame, self.settings).tobytes() for frame in raw))
            self.assertNotEqual(decoded, b"".join(frame.tobytes() for frame in raw))
            self.assertEqual({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in images.iterdir()}, raw_hashes)
            self.assertEqual((output / "audio.wav").read_bytes(), (images / "lens.wav").read_bytes())
            self.assertEqual((result["media"]["frames"], result["media"]["capture_fps"], result["media"]["fps"]), (30, 120, "60/1"))
            metadata = json.loads((output / "sequence.json").read_text())
            self.assertEqual(metadata["lens"], validate_lens(self.settings))
            self.assertTrue(metadata["lens_processing"]["complete"])
            self.assertEqual(metadata["lossless_validation"]["reference"], "lens_processed_rgb24")
            self.assertIn("resulting 8-bit sRGB", (output / "AE_IMPORT.txt").read_text())
            self.assertFalse((run / "raw-rgb.sha256").exists())
            self.assertTrue((run / "lens-rgb.sha256").exists())
            self.assertEqual(list(run.glob("lens-frames-*")), [])

    def test_real_mp4_uses_processed_input_and_preserves_audio_cadence_and_slow_motion(self):
        for slow in (False, True):
            with self.subTest(slow=slow), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                run, images, raw = _captured_job(root, export_format="mp4", lens=self.settings, audio=not slow, slow=slow)
                with patch("cs2_mvp.render.ROOT", root):
                    result = finish_capture(run, keep_raw=True, timeout=30)
                self.assertEqual((result["media"]["frames"], result["media"]["fps"], result["media"]["audio"]),
                                 (30 if slow else 15, "60/1", not slow))
                # A separate ffmpeg encode of the explicit processed sequence must
                # yield the exact same decoded video frames as the full exporter.
                manual = root / "manual"
                manual.mkdir()
                for index, frame in enumerate(raw):
                    write_tga(manual / f"frame_{index:08d}.tga", apply_lens(frame, self.settings))
                expected = root / "manual.mp4"
                command = ["ffmpeg", "-v", "error", "-framerate", "60" if slow else "120", "-i", str(manual / "frame_%08d.tga"),
                           "-an", "-c:v", "libx264", "-preset", "fast", "-pix_fmt", "yuv420p", "-crf", "8"]
                if not slow:
                    command += ["-vf", "fps=fps=60:start_time=0:round=near"]
                command += ["-frames:v", str(30 if slow else 15), "-t", str(.5 if slow else .25), str(expected)]
                subprocess.run(command, capture_output=True, check=True, timeout=20)
                self.assertEqual(_decode(Path(result["output"])), _decode(expected))
                self.assertEqual(len(list(images.glob("*.tga"))), 30)
                self.assertEqual(list(run.glob("lens-frames-*")), [])

    def test_neutral_export_has_no_pixel_postpass_and_is_exact_source_rgb(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, raw = _captured_job(root, lens={"enabled": True})
            with patch("cs2_mvp.render.ROOT", root), patch("cs2_mvp.render.apply_lens", side_effect=AssertionError("neutral postpass")):
                result = finish_capture(run, keep_raw=True, timeout=30)
            self.assertEqual(_decode(Path(result["output"]) / "frame_%08d.png"), b"".join(frame.tobytes() for frame in raw))
            self.assertEqual(result["lossless_validation"]["reference"], "raw_engine_rgb24")
            self.assertFalse(result["lens_processing"]["active"])

    def test_cancel_lens_pass_publishes_nothing_keeps_raw_and_retry_uses_frozen_settings(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, raw = _captured_job(root, lens=self.settings)
            cancelled = threading.Event()
            def notify(value):
                if value["stage"] == "lens" and value["progress"] >= .1:
                    cancelled.set()
            with patch("cs2_mvp.render.ROOT", root), self.assertRaises(RenderCancelled):
                finish_capture(run, timeout=30, cancel_event=cancelled, notify=notify)
            self.assertFalse((root / "output-frames").exists())
            self.assertEqual(len(list(images.glob("*.tga"))), 30)
            self.assertEqual(list(run.glob("lens-frames-*")), [])
            # Simulate editing the project after capture. Its saved plan stays
            # authoritative for resumed finishing, not this later camera object.
            plan = json.loads((run / "manifest.json").read_text())
            plan["camera"] = {"lens": {"enabled": False}}
            (run / "manifest.json").write_text(json.dumps(plan))
            with patch("cs2_mvp.render.ROOT", root):
                result = finish_capture(run, keep_raw=True, timeout=30)
            self.assertEqual(_decode(Path(result["output"]) / "frame_%08d.png"),
                             b"".join(apply_lens(frame, self.settings).tobytes() for frame in raw))

    def test_lens_validation_and_storage_fail_before_capture_or_raw_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            camera = root / "camera.json"
            camera.write_text(json.dumps({"type": "static", "lens": {"enabled": "yes"}}))
            with patch("cs2_mvp.render.doctor", side_effect=AssertionError("capture side effect")), self.assertRaisesRegex(ValueError, "boolean"):
                render_clip(root / "missing.dem", None, 1000, 1016, camera=camera)
            run, images, _ = _captured_job(root, lens=self.settings)
            with patch("cs2_mvp.render.ROOT", root), patch("cs2_mvp.render.shutil.disk_usage") as usage:
                usage.return_value.free = 1024
                with self.assertRaisesRegex(RuntimeError, "intermediate"):
                    finish_capture(run)
            self.assertEqual(len(list(images.glob("*.tga"))), 30)
            self.assertFalse((root / "output-frames").exists())

    def test_truncated_raw_frame_publishes_nothing_and_discards_partial_intermediates(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, images, _ = _captured_job(root, lens=self.settings)
            broken = sorted(images.glob("*.tga"))[6]
            broken.write_bytes(broken.read_bytes()[:-1])
            with patch("cs2_mvp.render.ROOT", root), self.assertRaisesRegex(ValueError, "Truncated"):
                finish_capture(run, timeout=30)
            self.assertEqual(len(list(images.glob("*.tga"))), 30)
            self.assertEqual(list(run.glob("lens-frames-*")), [])
            self.assertFalse((root / "output-frames").exists())

    def test_processing_timeout_and_later_encoding_cancel_keep_raw_and_remove_intermediates(self):
        for phase in ("timeout", "encoding"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                run, images, _ = _captured_job(root, lens=self.settings)
                cancelled = threading.Event()
                def notify(value):
                    if value["stage"] == "encoding":
                        cancelled.set()
                exception = TimeoutError if phase == "timeout" else RenderCancelled
                with patch("cs2_mvp.render.ROOT", root), self.assertRaises(exception):
                    finish_capture(run, timeout=0 if phase == "timeout" else 30,
                                   cancel_event=cancelled, notify=notify)
                self.assertEqual(len(list(images.glob("*.tga"))), 30)
                self.assertEqual(list(run.glob("lens-frames-*")), [])
                self.assertEqual(list(root.glob(".cs2-camera-png-*")), [])
                self.assertFalse((root / "output-frames").exists())

    def test_healthy_1440p_30_second_lens_job_can_exceed_capture_timeout(self):
        # Simulate the measured ~0.3s/frame CPU throughput without allocating a
        # 40-GB fixture or waiting 20 minutes. Existing real-image tests exercise
        # the same context manager's pixel processing and final FFmpeg input.
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            plan = {"lens": self.settings, "width": 2560, "height": 1440}
            clock, pixels = [0.], object()
            def process(rgb, settings, *, check_cancel):
                check_cancel()
                clock[0] += .35
                check_cancel()
                return rgb
            with patch("cs2_mvp.render.time.monotonic", side_effect=lambda: clock[0]), \
                    patch("cs2_mvp.render.shutil.disk_usage") as usage, \
                    patch("cs2_mvp.render.read_tga", return_value=pixels), \
                    patch("cs2_mvp.render.apply_lens", side_effect=process), \
                    patch("cs2_mvp.render.write_tga"):
                usage.return_value.free = 10**15
                with _lens_input_sequence(plan, run, [Path("raw.tga")] * 3600, "raw%08d.tga", 0,
                                          timeout=600, cancel_event=None, notify=lambda value: None) as (pattern, start):
                    self.assertTrue(Path(pattern).parent.is_dir())
                    self.assertEqual(start, 0)
                    self.assertGreater(clock[0], 600)
                    self.assertTrue(plan["lens_processing"]["complete"])
                    self.assertGreater(plan["lens_processing"]["processing_budget"]["total_seconds"], clock[0])
                self.assertEqual(list(run.glob("lens-frames-*")), [])

    def test_processing_inactivity_and_scaled_total_budget_preserve_raw_for_retry(self):
        for phase in ("inactivity", "total"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                run, images, _ = _captured_job(root, lens=self.settings)
                clock = [0.]
                def process(rgb, settings, *, check_cancel):
                    check_cancel()
                    clock[0] += 2 if phase == "inactivity" else .25
                    check_cancel()
                    return rgb
                message = "inactivity" if phase == "inactivity" else "input-size"
                with patch("cs2_mvp.render.ROOT", root), \
                        patch("cs2_mvp.render.time.monotonic", side_effect=lambda: clock[0]), \
                        patch("cs2_mvp.render.apply_lens", side_effect=process), \
                        self.assertRaisesRegex(TimeoutError, message):
                    finish_capture(run, timeout=1)
                self.assertEqual(len(list(images.glob("*.tga"))), 30)
                self.assertEqual(list(run.glob("lens-frames-*")), [])
                self.assertFalse((root / "output-frames").exists())

    def test_mp4_workspace_and_publication_space_failures_preserve_raw(self):
        for phase in ("encoding", "publication"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                run, images, _ = _captured_job(root, lens=self.settings, export_format="mp4")
                sufficient = shutil.disk_usage(root)._replace(free=10**15)
                insufficient = sufficient._replace(free=1024)
                encoding = [False]
                def notify(value):
                    if value["stage"] == "encoding":
                        encoding[0] = True
                def space(path):
                    low = Path(path) == root if phase == "publication" else encoding[0]
                    return insufficient if low else sufficient
                message = "while encoding" if phase == "encoding" else "publish the encoded"
                with patch("cs2_mvp.render.ROOT", root), \
                        patch("cs2_mvp.render.shutil.disk_usage", side_effect=space), \
                        self.assertRaisesRegex(RuntimeError, message):
                    finish_capture(run, timeout=30, notify=notify)
                self.assertEqual(len(list(images.glob("*.tga"))), 30)
                self.assertEqual(list(run.glob("lens-frames-*")), [])
                self.assertFalse((root / "output.mp4").exists())
                self.assertEqual(list(root.glob(".cs2-mvp-*")), [])

    def test_dry_run_freezes_lens_and_budgets_intermediates_on_the_work_filesystem(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            demo, camera, game = root / "demo.dem", root / "camera.json", root / "game"
            demo.touch()
            game.mkdir()
            camera.write_text(json.dumps({"version": 1, "type": "static", "position": [0, 0, 64], "angles": [0, 0], "lens": self.settings}))
            environment = {"game_directory": str(game), "steam_running": True, "display": ":test", "cs2_pids": [],
                           "ffmpeg": "ffmpeg", "ffprobe": "ffprobe", "game_free_bytes": 10**15}
            with patch("cs2_mvp.render.ROOT", root), patch("cs2_mvp.render.doctor", return_value=environment), patch("cs2_mvp.render.parser") as parser:
                parser.return_value.parse_header.return_value = {}
                result = render_clip(demo, None, 1000, 1016, camera=camera, fps=60, capture_fps=120,
                                     width=320, height=240, output=root / "result-frames", export_format="png", dry_run=True)
            self.assertEqual(result["lens"], validate_lens(self.settings))
            extra = 30 * (320 * 240 * 3 + 18)
            self.assertEqual(result["estimated_lens_intermediate_bytes"], extra)
            plain = estimate_capture_storage(30, 320, 240, "png")
            self.assertEqual(result["estimated_working_bytes"], plain["working"] + extra)
        game, work, destination = Path("/game"), Path("/work"), Path("/output/frames")
        free = {game: (1, 100 * 1024**3), work: (2, 1024**3), destination.parent: (3, 100 * 1024**3)}
        with patch("cs2_mvp.render._filesystem_space", side_effect=lambda path: free[path]), self.assertRaisesRegex(RuntimeError, "/work"):
            _validate_png_storage(game, work, destination, 1000, 1920, 1080, 1, False, lens=True)


if __name__ == "__main__":
    unittest.main()
