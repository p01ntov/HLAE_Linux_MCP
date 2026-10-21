import copy
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.camera import compile_camera
from cs2_mvp.live_mcp import TOOLS, call_tool
from cs2_mvp.live_track import LiveTrack, select_render_range
from cs2_mvp.render import render_clip


def project():
    return {"name": "Region camera", "start_tick": 1000, "duration": 4,
            "interpolation": "smooth", "render_range": {"mode": "region", "in": 1.1, "out": 2.2},
            "keys": [{"id": str(index), "time": time, "position": [x, 0, 64],
                      "angles": [0, angle, 0], "fov": fov, "easing": [.42, 0, .58, 1]}
                     for index, (time, x, angle, fov) in enumerate([(0, 0, 0, 40), (1, 30, 20, 50),
                                                                  (3, 10, 50, 80), (4, 90, 80, 90)])]}


class RenderRangeTests(unittest.TestCase):
    def test_legacy_live_project_defaults_to_full_composition(self):
        spec = project()
        del spec["render_range"]
        start, end, metadata = select_render_range(spec)
        self.assertEqual((start, end), (1000, 1256))
        self.assertEqual(metadata["scope"], "composition")
        self.assertEqual(metadata["source"], "project_default")
        self.assertEqual(LiveTrack(spec).render_range, {"mode": "composition", "in": 0, "out": 4})

    def test_region_quantizes_outward_and_reports_requested_and_effective_bounds(self):
        start, end, metadata = select_render_range(project())
        self.assertEqual((start, end), (1070, 1141))
        self.assertEqual(metadata["requested"], {"in": 1.1, "out": 2.2})
        self.assertEqual(metadata["effective"], {"in": 70/64, "out": 141/64, "start_tick": 1070, "end_tick": 1141})
        self.assertEqual(metadata["quantization"], "floor_in_ceil_out")
        self.assertEqual(metadata["scope"], "region")

    def test_composition_override_preserves_saved_region_and_original_project(self):
        spec = project()
        original = copy.deepcopy(spec)
        start, end, metadata = select_render_range(spec, scope="composition")
        self.assertEqual((start, end), (1000, 1256))
        self.assertEqual(metadata["requested"], {"in": 0, "out": 4})
        self.assertEqual(metadata["source"], "cli_scope")
        self.assertEqual(spec, original)
        spec["render_range"]["mode"] = "composition"
        self.assertEqual(select_render_range(spec, scope="region")[:2], (1070, 1141))

    def test_region_sampling_keeps_neighbor_keys_and_project_time_origin(self):
        spec = project()
        start, end, _ = select_render_range(spec)
        rendered = compile_camera(spec, None, start, end, 1000)
        track = LiveTrack(spec)
        for index in (0, 50, len(rendered["frames"])-1):
            expected = track.sample((start-spec["start_tick"])/64 + index/1000)
            self.assertEqual(rendered["frames"][index], expected)
        self.assertEqual(len(spec["keys"]), 4)
        self.assertNotEqual(rendered["frames"][0]["position"], spec["keys"][1]["position"])

    def test_invalid_saved_region_is_rejected_even_in_composition_mode(self):
        for value in [None, [], {"mode": "crop"}, {"in": True}, {"out": math.inf},
                      {"in": -1}, {"out": 5}, {"in": 2, "out": 2}, {"in": 3, "out": 2},
                      {"in": 0, "out": 10**500}, {"other": 1}]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                LiveTrack({**project(), "render_range": value})

    def test_absolute_ticks_require_complete_pair_and_reject_scope_override(self):
        for start, end, scope in [(1000, None, None), (None, 1100, None), (1000, 1100, "region")]:
            with self.subTest(start=start, end=end), self.assertRaises(ValueError):
                select_render_range(project(), start, end, scope)
        start, end, metadata = select_render_range(project(), 1032, 1064)
        self.assertEqual((start, end), (1032, 1064))
        self.assertEqual(metadata["scope"], "explicit")
        self.assertEqual(metadata["requested"], {"in": .5, "out": 1})

    def test_auto_range_and_early_errors_do_not_require_a_running_steam_or_game(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            camera = root / "project.json"
            camera.write_text(json.dumps(project()))
            demo = root / "input.dem"
            demo.write_bytes(b"fixture")
            environment = {"game_directory": str(root), "steam_running": True, "display": ":test", "cs2_pids": [],
                           "ffmpeg": "ffmpeg", "ffprobe": "ffprobe", "game_free_bytes": 10**15}
            with patch("cs2_mvp.render.doctor", return_value=environment), patch("cs2_mvp.render.parser") as parser:
                parser.return_value.parse_header.return_value = {}
                result = render_clip(demo, None, camera=camera, fps=60, capture_fps=1000, width=320, height=240, dry_run=True)
            self.assertEqual((result["start_tick"], result["end_tick"]), (1070, 1141))
            self.assertEqual(result["render_range"]["scope"], "region")
            self.assertEqual(result["camera_summary"]["project_sample_start_seconds"], 70/64)
            invalid = project()
            invalid["keys"][0]["fov"] = 179
            camera.write_text(json.dumps(invalid))
            with patch("cs2_mvp.render.doctor", side_effect=AssertionError("Fail before environment discovery")):
                with self.assertRaisesRegex(ValueError, "30..140"):
                    render_clip(demo, None, camera=camera, dry_run=True)
                with self.assertRaisesRegex(ValueError, "both --start and --end"):
                    render_clip(demo, "player", start=1000, dry_run=True)

    def test_auto_bounds_enforce_preroll_and_camera_duration_before_environment_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            camera = Path(directory) / "project.json"
            with patch("cs2_mvp.render.doctor", side_effect=AssertionError("Validate bounds first")):
                too_early = project()
                too_early["start_tick"] = 0
                camera.write_text(json.dumps(too_early))
                with self.assertRaisesRegex(ValueError, "start >= 193"):
                    render_clip(Path("missing.dem"), None, camera=camera, dry_run=True)
                long = project()
                long["duration"] = 301
                long["render_range"]["mode"] = "composition"
                camera.write_text(json.dumps(long))
                with self.assertRaisesRegex(ValueError, "300 seconds"):
                    render_clip(Path("missing.dem"), None, camera=camera, dry_run=True)

    def test_tiny_region_is_rejected_before_capture_if_output_would_have_fewer_than_two_frames(self):
        with tempfile.TemporaryDirectory() as directory:
            camera = Path(directory) / "project.json"
            spec = project()
            spec["render_range"].update({"in": 0, "out": .001})
            camera.write_text(json.dumps(spec))
            with patch("cs2_mvp.render.doctor", side_effect=AssertionError("Reject before launching a pointless capture")):
                for fps in (15, 60):
                    with self.subTest(fps=fps), self.assertRaisesRegex(ValueError, "fewer than two encoded frames"):
                        render_clip(Path("missing.dem"), None, camera=camera, fps=fps, dry_run=True)

    def test_legacy_or_pov_reject_automatic_scope_and_missing_ticks_early(self):
        with tempfile.TemporaryDirectory() as directory:
            camera = Path(directory) / "camera.json"
            camera.write_text(json.dumps({"version": 1, "type": "static", "position": [0, 0, 64], "angles": [0, 0]}))
            with patch("cs2_mvp.render.doctor", side_effect=AssertionError("Fail before environment discovery")):
                with self.assertRaisesRegex(ValueError, "require both --start and --end"):
                    render_clip(Path("missing.dem"), None, camera=camera, dry_run=True)
                with self.assertRaisesRegex(ValueError, "requires a project"):
                    render_clip(Path("missing.dem"), None, 1000, 1100, camera=camera, render_scope="region", dry_run=True)

    def test_mcp_supports_partial_typed_range_and_full_live_fov(self):
        class Client:
            def request(self, method, arguments):
                return {"method": method, "arguments": arguments}
        self.assertIn("cs2_camera_set_project", {tool["name"] for tool in TOOLS})
        change = {"render_range": {"mode": "composition"}}
        result = call_tool(Client(), "cs2_camera_set_project", change)
        self.assertEqual(result["arguments"], change)
        bounds = {"render_range": {"in": .5, "out": 1.5}}
        self.assertEqual(call_tool(Client(), "cs2_camera_set_project", bounds)["arguments"], bounds)
        for name, arguments in [("set_camera", {"fov": 1}), ("update_key", {"id": "1", "fov": 179})]:
            self.assertEqual(call_tool(Client(), "cs2_camera_"+name, arguments)["arguments"], arguments)

    def test_offline_capture_validates_saved_scene_before_environment_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            camera = Path(directory) / "scene.json"
            camera.write_text(json.dumps({"start_tick": 300, "duration": 2, "scene": {"unregistered_arbitrary_command": True},
                                          "keys": [{"id": "1", "time": 0, "position": [0, 0, 64], "angles": [0, 0, 0]}]}))
            with patch("cs2_mvp.render.doctor", side_effect=AssertionError("Reject lost effects before environment discovery")):
                with self.assertRaisesRegex(ValueError, "Unknown field"):
                    render_clip(Path("missing.dem"), None, camera=camera, dry_run=True)

    def test_cli_live_project_does_not_require_tick_flags(self):
        from cs2_mvp.cli import main
        args = ["cs2-mvp", "render", "match.dem", "--camera", "project.json", "--render-scope", "region", "--dry-run"]
        with patch.object(sys, "argv", args), patch("cs2_mvp.render.render_clip", return_value={}) as capture, \
             patch("builtins.print"):
            self.assertEqual(main(), 0)
        self.assertIsNone(capture.call_args.kwargs["start"])
        self.assertIsNone(capture.call_args.kwargs["end"])
        self.assertEqual(capture.call_args.kwargs["render_scope"], "region")


if __name__ == "__main__":
    unittest.main()
