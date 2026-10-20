import math
import unittest

from cs2_mvp.camera import PathCurve, SubjectTrack, compile_camera, easing, look_angles, validate_floor


class CameraTests(unittest.TestCase):
    def test_path_endpoints_and_uniform_distance(self):
        curve = PathCurve([[0, 0, 0], [100, 0, 50], [140, 150, 40], [300, 180, 100]])
        self.assertEqual(curve.sample(0), (0, 0, 0))
        self.assertEqual(curve.sample(1), (300, 180, 100))
        samples = [curve.sample(i / 120) for i in range(121)]
        distances = [math.dist(a, b) for a, b in zip(samples, samples[1:])]
        self.assertLess(max(distances) / min(distances), 1.02)

    def test_constant_camera_and_end_exclusive_sampling(self):
        spec = {"version": 1, "type": "static", "position": [10, 20, 30], "angles": [5, 80]}
        first = compile_camera(spec, None, 1000, 1640, 60)
        second = compile_camera(spec, None, 1000, 1640, 60)
        self.assertEqual(first, second)
        self.assertEqual(len(first["frames"]), 600)
        self.assertTrue(all(frame == first["frames"][0] for frame in first["frames"]))

    def test_yaw_uses_short_arc_across_180(self):
        spec = {"version": 1, "type": "path", "easing": "linear", "points": [
            {"position": [0, 0, 0], "angles": [0, 179]},
            {"position": [10, 0, 0], "angles": [0, -179]}]}
        frames = compile_camera(spec, None, 1000, 1064, 60)["frames"]
        self.assertTrue(all(179 <= f["angles"][1] <= 181 for f in frames))
        self.assertLess(max(abs(b["angles"][1] - a["angles"][1]) for a, b in zip(frames, frames[1:])), .04)

    def test_linear_timing_keeps_speed_smoothstep_starts_slowly(self):
        self.assertEqual(easing(0, 'smootherstep'), 0)
        self.assertEqual(easing(1, 'smootherstep'), 1)
        self.assertLess(easing(.01, 'smootherstep'), .0001)

    def test_follow_uses_interpolated_demo_time(self):
        track = SubjectTrack(list(range(900, 1200)), [(float(i), 0, 0) for i in range(900, 1200)], 'actor', '1')
        spec = {"version": 1, "type": "follow", "offset": [-100, 25, 30],
                "look_at": {"player": "actor", "offset": [0, 0, 48], "smooth_seconds": 0}}
        result = compile_camera(spec, None, 1000, 1064, 120, subject=track)
        self.assertEqual(result['frames'][0]['position'], [900, 25, 78])
        self.assertAlmostEqual(result['frames'][1]['position'][0] - result['frames'][0]['position'][0], 64 / 120)

    def test_invalid_paths_and_missing_motion_capabilities_rejected(self):
        for points in [[[0, 0, 0]], [[0, 0, 0], [0, 0, 0]], [[0, 0, 0], [math.nan, 1, 1]]]:
            with self.assertRaises(ValueError):
                PathCurve(points)
        with self.assertRaises(ValueError):
            look_angles([0, 0, 0], [0, 0, 0])
        with self.assertRaises(ValueError):
            compile_camera({"version": 1, "type": "static", "position": [0, 0, 0], "angles": [0, 0], "fov": 29}, None, 1, 65, 60)

    def test_constant_fov_is_optional_and_does_not_change_the_camera_route(self):
        spec = {"version": 1, "type": "static", "position": [10, 20, 200], "angles": [20, 30]}
        default = compile_camera(spec, None, 1000, 1064, 120)
        wide = compile_camera({**spec, "fov": 105}, None, 1000, 1064, 120)
        for original, changed in zip(default['frames'], wide['frames']):
            self.assertNotIn('fov', original)
            self.assertEqual(changed['fov'], 105)
            self.assertEqual(original['position'], changed['position'])
            self.assertEqual(original['angles'], changed['angles'])
        for invalid in [True, None, math.nan, math.inf, 29.9, 140.1]:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                compile_camera({**spec, 'fov': invalid}, None, 1000, 1064, 120)

    def test_floor_boundary_and_unknown_floor(self):
        frames = [{"position": [0, 0, 32], "angles": [0, 0]}]
        self.assertEqual(validate_floor(frames, {"floor_z": 0})["status"], "passed")
        self.assertEqual(validate_floor(frames, {})["status"], "unchecked")
        for spec in [{"floor_z": True}, {"floor_z": math.nan}, {"clearance": 0}, {"clearance": -1}, {"clearance": math.inf}]:
            with self.assertRaises(ValueError):
                validate_floor(frames, spec)

    def test_floor_guard_rejects_curve_overshoot_between_safe_anchors(self):
        points = [{"position": [i * 100, 0, z], "angles": [0, 0]} for i, z in enumerate([64, 32, 32, 64])]
        spec = {"version": 1, "type": "path", "easing": "linear", "points": points, "floor_z": 0}
        with self.assertRaisesRegex(ValueError, "below floor clearance"):
            compile_camera(spec, None, 1000, 1192, 120)

    def test_world_camera_is_independent_of_actor_motion(self):
        spec = {"version": 1, "type": "path", "points": [{"position": [10, 0, 40]}, {"position": [50, 10, 80]}],
                "look_at": {"point": [0, 100, 40]}, "floor_z": 0}
        actor = SubjectTrack([1000, 1064], [(0, 0, 0), (100, 100, 500)], 'actor', '1')
        self.assertEqual(compile_camera(spec, None, 1000, 1064, 120), compile_camera(spec, None, 1000, 1064, 120, subject=actor))



if __name__ == '__main__':
    unittest.main()
