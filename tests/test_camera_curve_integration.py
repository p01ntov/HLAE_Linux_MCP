"""Real key-2 regression and portable export boundaries; no game/capture I/O."""
import copy
import json
import math
from pathlib import Path
import tempfile
import unittest

from cs2_mvp.camera import compile_camera
from cs2_mvp.camera_backend import build_camera_sequence
from cs2_mvp.csdm import build_actions
from cs2_mvp.export_ui import save_export_snapshot
from cs2_mvp.live_track import LiveTrack
from cs2_mvp.shot_library import validate_library, stage_library


def observed_project():
    path = Path(__file__).parent / "fixtures/camera_key2_discontinuity.json"
    return json.loads(path.read_text())["project"]


def custom_project():
    return {"name": "Independent curves", "start_tick": 1000, "duration": 2,
            "interpolation": "bezier", "rotation_interpolation": "bezier", "fov_interpolation": "bezier",
            "keys": [
                {"id": "a", "time": 0, "position": [0, 0, 64], "angles": [0, 10, 0], "fov": 60,
                 "position_out": [20, 60, 5], "rotation_out": [3, 20, 7], "fov_out": 20,
                 "position_easing": [0, 0, 1, 1], "rotation_easing": [.7, 0, 1, 1],
                 "fov_easing": [0, 0, .5, 1]},
                {"id": "b", "time": 2, "position": [100, 0, 64], "angles": [10, 90, 5], "fov": 100,
                 "position_in": [-40, 30, 10], "rotation_in": [-2, -10, -3], "fov_in": -10,
                 "position_out": None, "rotation_out": None, "fov_out": None,
                 "position_easing": None, "rotation_easing": None, "fov_easing": None}]}


class CameraCurveIntegrationTests(unittest.TestCase):
    def test_actual_key2_timing_discontinuity_is_fixed_without_a_forced_stop(self):
        old = observed_project()
        time = old["keys"][1]["time"]
        h = .001
        def sides(track):
            center = track.sample(time)["position"]
            return (math.dist(track.sample(time-h)["position"], center)/h,
                    math.dist(track.sample(time+h)["position"], center)/h)
        before, after = sides(LiveTrack(old))
        self.assertGreater(after/before, 5)
        smooth = copy.deepcopy(old)
        smooth.update(interpolation="bezier", rotation_interpolation="bezier", fov_interpolation="bezier")
        for key in smooth["keys"]:
            for channel in ("position", "rotation", "fov"):
                key[channel+"_easing"] = [0, 0, 1, 1]
                key[channel+"_in"] = key[channel+"_out"] = None
        left, right = sides(LiveTrack(smooth))
        self.assertGreater(min(left, right), 400)  # Continuous pass, not two ease-to-zero segments.
        self.assertLess(abs(left-right), 1)
        self.assertEqual(smooth["keys"][1]["position"], old["keys"][1]["position"])
        self.assertEqual(smooth["keys"][1]["time"], time)

    def test_custom_curve_samples_reach_native_capture_sidecar_unchanged(self):
        spec = custom_project()
        compiled = compile_camera(spec, None, 1000, 1128, 1000)
        self.assertEqual(len(compiled["frames"]), 2000)
        sequence = build_camera_sequence(build_actions(1000, 1128, "", "curves", 1000, camera_only=True), compiled["frames"])
        self.assertEqual(sequence["camera_frames"], compiled["frames"])
        for index in (0, 317, 1000, 1507, 1999):
            self.assertEqual(sequence["camera_frames"][index], LiveTrack(spec).sample(index/1000))
        self.assertGreater(compiled["frames"][1000]["position"][1], 20)
        self.assertTrue(all(30 <= f["fov"] <= 140 for f in compiled["frames"]))

    def test_snapshot_and_shot_library_keep_handles_and_independent_curves(self):
        spec = custom_project()
        class Client:
            def request(self, method, params=None):
                if method == "get_state":
                    return {"tick": 1042, "cursor": .65625, "camera": LiveTrack(spec).sample(.65625)}
                if method == "export_project":
                    return copy.deepcopy(spec)
                if method != "preview":
                    raise AssertionError("Unexpected mutation: " + method)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshot = save_export_snapshot(Client(), root / "snapshot")
            self.assertEqual(json.loads(Path(snapshot["path"]).read_text()), spec)
            demo = root / "demo.dem"
            demo.write_bytes(b"local fixture")
            library = validate_library({"version": 1, "demo": str(demo), "shots": [
                {"id": "curves", "label": "Custom curves", "project": spec}]}, demo)
            staged = stage_library(library, demo, root)
            self.assertEqual(json.loads(staged.read_text())["shots"][0]["project"], spec)


if __name__ == "__main__":
    unittest.main()
