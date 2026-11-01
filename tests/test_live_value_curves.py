import copy
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.camera import compile_camera
from cs2_mvp.live_track import LiveTrack
from cs2_mvp.render import render_clip


def automatic():
    return {"start_tick": 1000, "duration": 4.7, "interpolation": "bezier",
            "rotation_interpolation": "bezier", "fov_interpolation": "bezier", "keys": [
                {"id": "a", "time": 0, "position": [0, 0, 0], "angles": [0, 170, 5], "fov": 60},
                {"id": "b", "time": 1.3, "position": [13, 6, 3], "angles": [10, -170, 15], "fov": 80},
                {"id": "c", "time": 4.7, "position": [90, 25, 12], "angles": [-5, -135, 30], "fov": 110}]}


class LiveValueCurveTests(unittest.TestCase):
    def test_production_native_samples_and_all_controls_match_python(self):
        fixture = json.loads((Path(__file__).parent / "fixtures/live_bezier_native_samples.json").read_text())
        self.assertEqual(len(fixture["cases"]), 7)
        for case in fixture["cases"]:
            track = LiveTrack(case["project"])
            for expected in case["samples"]:
                with self.subTest(case=case["name"], time=expected["time"]):
                    actual = track.sample(expected["time"])
                    for channel in ("position", "angles"):
                        for x, y in zip(actual[channel], expected[channel]):
                            self.assertAlmostEqual(x, y, places=8)
                    self.assertAlmostEqual(actual["fov"], expected["fov"], places=8)
            for left, expected in enumerate(case["controls"]):
                for method, channel in ((track.position_controls, "position"), (track.rotation_controls, "rotation")):
                    for actual, wanted in zip(method(left), expected[channel]):
                        for x, y in zip(actual, wanted):
                            self.assertAlmostEqual(x, y, places=8)
                for x, y in zip(track.fov_controls(left), expected["fov"]):
                    self.assertAlmostEqual(x, y, places=8)

    def test_automatic_handles_have_C1_time_derivatives_at_nonuniform_key(self):
        track = LiveTrack(automatic())
        for controls in (track.position_controls, track.rotation_controls):
            a, b = controls(0), controls(1)
            for axis in range(3):
                self.assertAlmostEqual(3 * (a[3][axis] - a[2][axis]) / 1.3,
                                       3 * (b[1][axis] - b[0][axis]) / 3.4, places=10)
        a, b = track.fov_controls(0), track.fov_controls(1)
        self.assertAlmostEqual(3 * (a[3] - a[2]) / 1.3, 3 * (b[1] - b[0]) / 3.4, places=10)
        h = .0001
        a, b, c = (track.sample(t) for t in (1.3 - h, 1.3, 1.3 + h))
        for channel in ("position", "angles"):
            for x, y, z in zip(a[channel], b[channel], c[channel]):
                self.assertAlmostEqual((y - x) / h, (z - y) / h, delta=.02)
        self.assertAlmostEqual((b["fov"] - a["fov"]) / h, (c["fov"] - b["fov"]) / h, delta=.02)

    def test_bezier_auto_position_matches_legacy_smooth_locus(self):
        spec = automatic(); legacy = copy.deepcopy(spec); legacy["interpolation"] = "smooth"
        before, after = LiveTrack(legacy), LiveTrack(spec)
        for t in (0, .25, .8, 1.3, 2, 3, 4.7):
            for x, y in zip(before.sample(t)["position"], after.sample(t)["position"]):
                self.assertAlmostEqual(x, y, places=10)

    def test_two_key_explicit_value_controls_are_not_spatial_easing(self):
        spec = automatic(); spec["keys"] = spec["keys"][:2]; spec["duration"] = 2
        a, b = spec["keys"]; a.update(position=[0, 0, 0], angles=[0, 0, 0], fov=60,
                                      position_out=[0, 40, 0], rotation_out=[0, 120, 0], fov_out=40)
        b.update(time=2, position=[40, 0, 0], angles=[0, 90, 0], fov=100,
                 position_in=[0, 40, 0], rotation_in=[0, 0, 0], fov_in=40)
        sample = LiveTrack(spec).sample(1)
        self.assertAlmostEqual(sample["position"][0], 20, places=8)
        self.assertAlmostEqual(sample["position"][1], 30, places=8)
        self.assertAlmostEqual(sample["angles"][1], 90, places=8)
        self.assertAlmostEqual(sample["fov"], 110, places=8)

    def test_independent_easing_and_nullable_fallback_leave_other_channels_alone(self):
        spec = automatic(); spec.update(duration=2, interpolation="linear", rotation_interpolation="slerp", fov_interpolation="linear")
        spec["keys"] = spec["keys"][:2]
        a, b = spec["keys"]; a.update(position=[0, 0, 0], angles=[0, 0, 0], fov=60,
            easing=[.9, 0, 1, 1], position_easing=[0, 0, 1, 1], rotation_easing=[0, 0, 1, 1], fov_easing=None)
        b.update(time=2, position=[100, 0, 0], angles=[0, 90, 0], fov=100)
        before = LiveTrack(spec).sample(1)
        self.assertAlmostEqual(before["position"][0], 50, places=8)
        self.assertAlmostEqual(before["angles"][1], 45, places=8)
        self.assertLess(before["fov"], 68)
        a["fov_easing"] = [0, 0, 1, 1]
        after = LiveTrack(spec).sample(1)
        self.assertEqual(before["position"], after["position"])
        self.assertEqual(before["angles"], after["angles"])
        self.assertAlmostEqual(after["fov"], 80, places=8)

    def test_global_yaw_unwrap_stays_continuous_at_keys_and_final_hold(self):
        track = LiveTrack(automatic())
        self.assertEqual(track.rotation_values[1][1], 190)
        self.assertEqual(track.rotation_values[2][1], 225)
        for t in (1.3, 4.7):
            left, middle, right = (track.sample(v)["angles"][1] for v in (t - .00001, t, t + .00001))
            self.assertLess(abs(left - middle), .001)
            self.assertLess(abs(right - middle), .001)
        self.assertEqual(track.sample(6)["angles"][1], 225)

    def test_null_resets_to_auto_and_handle_units_are_relative(self):
        spec = automatic(); spec["keys"][0].update(position_out=[10, 20, 30], rotation_out=[1, 2, 3], fov_out=5)
        track = LiveTrack(spec)
        self.assertEqual(track.position_controls(0)[1], (10, 20, 30))
        self.assertEqual(track.rotation_controls(0)[1], (1, 172, 8))
        self.assertEqual(track.fov_controls(0)[1], 65)
        for name in ("position_out", "rotation_out", "fov_out"):
            spec["keys"][0][name] = None
        reset, original = LiveTrack(spec), LiveTrack(automatic())
        self.assertEqual(reset.position_controls(0), original.position_controls(0))
        self.assertEqual(reset.rotation_controls(0), original.rotation_controls(0))
        self.assertEqual(reset.fov_controls(0), original.fov_controls(0))

    def test_pitch_fov_clamp_live_but_capture_checks_every_sample(self):
        spec = automatic(); spec["keys"][0].update(rotation_out=[1000, 0, 0], fov_out=1000)
        spec["keys"][1].update(rotation_in=[1000, 0, 0], fov_in=1000)
        sample = LiveTrack(spec).sample(.65)
        self.assertEqual(sample["angles"][0], 90)
        self.assertEqual(sample["fov"], 179)
        with self.assertRaisesRegex(ValueError, "FOV curve .*30..140"):
            compile_camera(spec, None, 1000, 1064, 1000)

    def test_fov_extrema_fail_before_game_discovery_even_with_valid_keys(self):
        spec = automatic(); spec["keys"] = spec["keys"][:2]; spec["duration"] = 1.3
        for key in spec["keys"]:
            key["fov"] = 90
        spec["keys"][0]["fov_out"] = 80
        spec["keys"][1]["fov_in"] = 80
        self.assertEqual(LiveTrack(spec).sample(.65)["fov"], 150)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "project.json"; path.write_text(json.dumps(spec))
            with patch("cs2_mvp.render.doctor", side_effect=AssertionError("Must reject before game discovery")):
                with self.assertRaisesRegex(ValueError, "FOV curve .*30..140"):
                    render_clip(Path("missing.dem"), None, camera=path, dry_run=True)
        spec["keys"][0]["fov_out"] = 60
        spec["keys"][1]["fov_in"] = -60
        bounds = LiveTrack(spec).validate_capture_fov()
        self.assertAlmostEqual(bounds["minimum"], 90-10*math.sqrt(3), places=8)
        self.assertAlmostEqual(bounds["maximum"], 90+10*math.sqrt(3), places=8)
        spec = automatic(); spec["keys"][0]["fov"] = 20
        self.assertEqual(LiveTrack(spec).sample(0)["fov"], 20)
        with self.assertRaisesRegex(ValueError, "capture requires key FOV"):
            compile_camera(spec, None, 1000, 1064, 1000)

    def test_invalid_modes_and_nonfinite_optional_controls_reject(self):
        for mode, invalid in (("rotation_interpolation", "squad"), ("fov_interpolation", None)):
            spec = automatic(); spec[mode] = invalid
            with self.assertRaises(ValueError):
                LiveTrack(spec)
        for name, invalid in (("position_out", [0, 0, math.nan]), ("rotation_in", [0, 0]),
                              ("fov_in", math.inf), ("fov_out", True), ("position_in", [0, 0, 1000001]),
                              ("position_easing", [0, -1, 1, 1]), ("rotation_easing", [0, 0, 1, math.nan]),
                              ("fov_easing", True)):
            spec = automatic(); spec["keys"][0][name] = invalid
            with self.subTest(field=name), self.assertRaises(ValueError):
                LiveTrack(spec)


if __name__ == "__main__":
    unittest.main()
