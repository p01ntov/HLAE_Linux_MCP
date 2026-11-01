import json
import math
from pathlib import Path
import unittest

from cs2_mvp.camera import compile_camera
from cs2_mvp.live_track import LiveTrack, bezier_ease


def project():
    return {"name": "Editor project", "start_tick": 1000, "duration": 2,
            "interpolation": "linear", "keys": [
                {"id": "first", "time": 0, "position": [0, 20, 40], "angles": [0, 179, 15], "fov": 60},
                {"id": "last", "time": 2, "position": [128, 20, 40], "angles": [0, -179, 35], "fov": 100}]}


class LiveTrackTests(unittest.TestCase):
    def test_sampler_matches_native_editor_fixtures(self):
        fixture = json.loads((Path(__file__).parent / "fixtures/live_camera_native_samples.json").read_text())
        for case in fixture["cases"]:
            track = LiveTrack(case["project"])
            for expected in case["samples"]:
                with self.subTest(case=case["name"], time=expected["time"]):
                    actual = track.sample(expected["time"])
                    for name in ["position", "angles"]:
                        for a, b in zip(actual[name], expected[name]):
                            self.assertAlmostEqual(a, b, places=8)
                    self.assertAlmostEqual(actual["fov"], expected["fov"], places=8)

    def test_hermite_uses_native_endpoint_and_neighbor_tangents(self):
        spec = project()
        spec.update(duration=3, interpolation="smooth")
        spec["keys"] = [{"id": str(i), "time": t, "position": [x, 0, 40], "angles": [0, 0, 0]}
                        for i, (t, x) in enumerate([(0, 0), (1, 10), (3, 0)])]
        self.assertAlmostEqual(LiveTrack(spec).sample(.5)["position"][0], 6.25, places=8)
        spec["keys"] = spec["keys"][:2]
        self.assertAlmostEqual(LiveTrack(spec).sample(.5)["position"][0], 5, places=8)

    def test_outgoing_bezier_inverts_x_and_controls_lens_too(self):
        spec = project()
        spec["keys"][0]["easing"] = [.9, 0, 1, 1]
        spec["keys"][1]["easing"] = [0, 1, 0, 1]
        u = bezier_ease(.5, [.9, 0, 1, 1])
        self.assertLess(u, .2)
        sample = LiveTrack(spec).sample(1)
        self.assertAlmostEqual(sample["position"][0], 128*u)
        self.assertAlmostEqual(sample["fov"], 60+40*u)

    def test_absolute_start_tick_offset_and_1000_end_exclusive_samples(self):
        spec = {**project(), "format": "cs2-camera-project", "version": 1, "ignored_metadata": {"x": 1}}
        short = compile_camera(spec, None, 1032, 1064, 4)
        self.assertEqual(len(short["frames"]), 2)
        self.assertAlmostEqual(short["frames"][0]["position"][0], 32, places=8)
        self.assertAlmostEqual(short["frames"][1]["position"][0], 48, places=8)
        sampled = compile_camera(spec, None, 1000, 1064, 1000)
        self.assertEqual(len(sampled["frames"]), 1000)
        self.assertEqual(len({frame["position"][0] for frame in sampled["frames"]}), 1000)
        self.assertAlmostEqual(sampled["frames"][1]["position"][0], .064, places=8)
        self.assertAlmostEqual(sampled["frames"][-1]["position"][0], 63.936, places=8)
        self.assertTrue(all(len(frame["angles"]) == 3 for frame in sampled["frames"]))
        self.assertEqual(sampled["summary"]["live_project_interpolation"], "linear")
        self.assertEqual(sampled["summary"]["project_start_tick"], 1000)
        self.assertEqual(sampled["summary"]["orientation_mode"], "quaternion_slerp")
        self.assertTrue(sampled["summary"]["roll_supported"])
        self.assertAlmostEqual(sampled["summary"]["fov_animation"]["end"], 79.98, places=8)

    def test_native_endpoint_holds_and_unsorted_key_normalization(self):
        spec = project()
        spec["keys"][0]["time"] = .5
        spec["keys"][1]["time"] = 1.5
        spec["keys"].reverse()
        track = LiveTrack(spec)
        self.assertEqual(track.sample(-3)["position"], [0, 20, 40])
        self.assertEqual(track.sample(.25)["position"], [0, 20, 40])
        self.assertEqual(track.sample(20)["position"], [128, 20, 40])
        self.assertEqual(compile_camera(spec, None, 900, 916, 4)["frames"][0]["position"], [0, 20, 40])
        self.assertEqual(compile_camera(spec, None, 1200, 1216, 4)["frames"][0]["position"], [128, 20, 40])

    def test_rejects_nonfinite_and_invalid_project_values(self):
        changes = [
            ("duration", math.nan), ("duration", math.inf), ("duration", True),
            ("duration", .009), ("start_tick", True), ("start_tick", 1.5),
            ("start_tick", -1), ("interpolation", "catmull_rom"), ("keys", []),
        ]
        for key, value in changes:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                LiveTrack({**project(), key: value})
        for key, values in {"position": [[math.inf, 0, 0], [True, 0, 0], [1_000_001, 0, 0]],
                            "angles": [[91, 0, 0], [0, math.nan, 0], [0, 0]],
                            "time": [math.nan, True, -1, 3],
                            "easing": [[0, 0, 1, math.inf], [0, -1, 1, 1], [0, 1]],
                            "fov": [math.nan, True, 0, 180], "id": [None, "", "x"*129]}.items():
            for value in values:
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    spec = project()
                    spec["keys"][0][key] = value
                    LiveTrack(spec)

    def test_duplicate_time_or_id_and_capture_fov_limit_are_explicit(self):
        for key, value in [("time", 0), ("time", .0000005), ("id", "first")]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                spec = project()
                spec["keys"][1][key] = value
                LiveTrack(spec)
        for fov in [1, 29.9, 140.1, 179]:
            spec = project()
            spec["keys"][0]["fov"] = fov
            with self.assertRaisesRegex(ValueError, "capture requires key FOV within 30..140"):
                compile_camera(spec, None, 1000, 1064, 1000)

    def test_roll_pole_and_floor_validation_are_preserved(self):
        spec = project()
        spec["keys"][0]["angles"] = [89, 0, 35]
        spec["keys"][1]["angles"] = [89, 180, -145]
        frames = compile_camera(spec, None, 1000, 1128, 64)["frames"]
        self.assertTrue(all(all(math.isfinite(v) for v in frame["angles"]) for frame in frames))
        self.assertGreater(max(frame["angles"][0] for frame in frames), 89.99)
        spec["floor_z"] = 10
        with self.assertRaisesRegex(ValueError, "below floor clearance"):
            compile_camera(spec, None, 1000, 1064, 64)

    def test_project_and_clip_durations_have_separate_bounds(self):
        spec = project()
        spec["duration"] = 1000
        self.assertEqual(len(compile_camera(spec, None, 1000, 1064, 1000)["frames"]), 1000)
        with self.assertRaisesRegex(ValueError, "at most 300 seconds"):
            compile_camera(spec, None, 1000, 1000+301*64, 1000)
        for fps in [True, 0, 1001]:
            with self.subTest(fps=fps), self.assertRaises(ValueError):
                compile_camera(spec, None, 1000, 1064, fps)


if __name__ == "__main__":
    unittest.main()
