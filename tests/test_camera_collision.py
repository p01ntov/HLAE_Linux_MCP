import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from cs2_mvp.camera_collision import Geometry, GeometryError, load_geometry, project_cubics


def box(low, high, kind="world_hull", coverage=None, **metadata):
    vertices = [[x, y, z] for x in (low[0], high[0]) for y in (low[1], high[1]) for z in (low[2], high[2])]
    triangles = [[0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5],
                 [0, 4, 5], [0, 5, 1], [2, 3, 7], [2, 7, 6],
                 [0, 2, 6], [0, 6, 4], [1, 5, 7], [1, 7, 3]]
    complete = {name: True for name in ("world_collision", "static_props", "glass", "inside_solid", "dynamic_entities")}
    manifest = {"format": "cs2-camera-geometry", "version": 1, "coordinates": "source_z_up",
                "map_name": "unit_map", "ready": True, "coverage": coverage or complete,
                "components": [{"kind": kind, "closed": True, "convex": True, "triangle_start": 0,
                                "triangle_count": len(triangles), "bounds": [low, high]}], **metadata}
    return Geometry(manifest, vertices, triangles, np.zeros(len(triangles), dtype=np.uint32))


def project(positions, interpolation="linear"):
    return {"duration": max(1, len(positions) - 1), "interpolation": interpolation,
            "keys": [{"id": str(i), "time": i, "position": p, "angles": [0, 0, 0], "fov": 74}
                     for i, p in enumerate(positions)]}


class CollisionTests(unittest.TestCase):
    def test_wall_between_clear_keys_is_rejected(self):
        geometry = box([0, -100, -100], [1, 100, 100])
        self.assertTrue(geometry.validate_position([-20, 0, 0])["passed"])
        self.assertTrue(geometry.validate_position([20, 0, 0])["passed"])
        result = geometry.validate_project(project([[-20, 0, 0], [20, 0, 0]]))
        self.assertFalse(result["passed"])
        self.assertEqual(result["collision"]["kind"], "surface_clearance")
        self.assertEqual(result["collision"]["keys"], ["0", "1"])

    def test_floor_crossing_and_glass_are_solid_obstacles(self):
        floor = box([-100, -100, 0], [100, 100, 1])
        self.assertFalse(floor.validate_segment([0, 0, 20], [0, 0, -20])["passed"])
        glass = box([0, -100, -100], [.1, 100, 100], kind="glass")
        result = glass.validate_segment([-20, 0, 0], [20, 0, 0])
        self.assertFalse(result["passed"])
        self.assertEqual(result["collision"]["component_kind"], "glass")

    def test_corner_clearance_is_euclidean_not_expanded_box_only(self):
        geometry = box([0, 0, -100], [10, 10, 100])
        self.assertFalse(geometry.validate_position([-5, -5, 0])["passed"])
        self.assertTrue(geometry.validate_position([-7, -7, 0])["passed"])

    def test_inside_convex_solid_rejected_even_far_from_surface(self):
        geometry = box([-100, -100, -100], [100, 100, 100])
        result = geometry.validate_position([0, 0, 0])
        self.assertEqual(result["collision"]["kind"], "inside_solid")
        self.assertFalse(result["passed"])

    def test_hermite_bulge_hits_wall_that_linear_path_clears(self):
        geometry = box([-5, -4, -50], [5, -3, 50])
        positions = [[-40, 30, 0], [-20, 0, 0], [20, 0, 0], [40, 30, 0]]
        self.assertTrue(geometry.validate_project(project(positions), clearance=1)["passed"])
        result = geometry.validate_project(project(positions, "smooth"), clearance=1)
        self.assertFalse(result["passed"])
        self.assertEqual(result["collision"]["keys"], ["1", "2"])
        self.assertIn("curve_parameter_interval", result["collision"])

    def test_explicit_bezier_handles_hit_wall_despite_clear_keys(self):
        geometry = box([0, -100, -100], [1, 100, 100])
        value = project([[-30, -30, 0], [-30, 30, 0]], "bezier")
        self.assertTrue(geometry.validate_project(value)["passed"])
        value["keys"][0]["position_out"] = [80, 0, 0]
        value["keys"][1]["position_in"] = [80, 0, 0]
        positions, cubics = project_cubics(value)
        self.assertEqual(cubics[0]["controls"], ((-30., -30., 0.), (50., -30., 0.),
                                               (50., 30., 0.), (-30., 30., 0.)))
        self.assertTrue(all(geometry.validate_position(p)["passed"] for p in positions))
        result = geometry.validate_project(value)
        self.assertFalse(result["passed"])
        self.assertEqual(result["collision"]["keys"], ["0", "1"])
        self.assertIn("curve_parameter_interval", result["collision"])

    def test_null_spatial_handles_restore_auto_and_legacy_modes_ignore_handles(self):
        geometry = box([0, -100, -100], [1, 100, 100])
        value = project([[-30, -30, 0], [-30, 30, 0]], "bezier")
        automatic = project_cubics(value)
        self.assertEqual(automatic[1][0]["controls"], ((-30., -30., 0.), (-30., -10., 0.),
                                                      (-30., 10., 0.), (-30., 30., 0.)))
        value["keys"][0]["position_out"] = [200, 0, 0]
        self.assertFalse(geometry.validate_project(value)["passed"])
        for mode in ("linear", "smooth"):
            value["interpolation"] = mode
            self.assertTrue(geometry.validate_project(value)["passed"])
        value["interpolation"] = "bezier"
        value["keys"][0]["position_out"] = None
        value["keys"][1]["position_in"] = None
        self.assertEqual(project_cubics(value), automatic)
        self.assertTrue(geometry.validate_project(value)["passed"])

    def test_bezier_auto_tangents_use_neighbor_key_times(self):
        value = project([[-40, 30, 0], [-20, 0, 0], [20, 0, 0], [40, 30, 0]], "bezier")
        value["duration"] = 6
        for key, time in zip(value["keys"], (0, 1, 3, 6)):
            key["time"] = time
        controls = project_cubics(value)[1][1]["controls"]
        self.assertTrue(np.allclose(controls, ((-20, 0, 0), (-20 + 40/3, -20/3, 0),
                                               (12, -4, 0), (20, 0, 0))))
        value["keys"][1]["position_out"] = [10, -5, 2]
        changed = project_cubics(value)[1][1]["controls"]
        self.assertEqual(changed[1], (-10., -5., 2.))
        self.assertEqual(changed[2], controls[2])

    def test_channel_easings_do_not_change_complete_spatial_curve(self):
        value = project([[-30, -30, 0], [-30, 30, 0]], "bezier")
        value["keys"][0]["position_out"] = [5, 2, 1]
        value["keys"][1]["position_in"] = [3, -2, 1]
        original = project_cubics(value)
        value.update(rotation_interpolation="bezier", fov_interpolation="bezier")
        value["keys"][0].update(position_easing=[1, 0, 0, 1], rotation_easing=[0, 1, 1, 0],
                                fov_easing=None, rotation_out=[30, 45, 180], fov_out=15)
        value["keys"][1].update(rotation_in=[-20, 90, -90], fov_in=-12)
        self.assertEqual(project_cubics(value), original)

    def test_unknown_modes_and_nonfinite_controls_cannot_be_diagnosed_as_clear(self):
        for field in ("interpolation", "rotation_interpolation", "fov_interpolation"):
            for keys in ([], project([[-30, 0, 0]])["keys"]):
                with self.subTest(field=field, keys=bool(keys)), self.assertRaises(ValueError):
                    project_cubics({"keys": keys, field: "unknown"})
        for field, invalid in (("position_out", [float("nan"), 0, 0]),
                               ("rotation_in", [0, float("inf"), 0]),
                               ("fov_out", float("inf")),
                               ("position_easing", [0, 0, 1, float("nan")])):
            value = project([[-30, -30, 0], [-30, 30, 0]], "bezier")
            value["keys"][0][field] = invalid
            with self.subTest(field=field), self.assertRaises(ValueError):
                project_cubics(value)

    def test_partial_coverage_returns_static_diagnosis_but_never_pass(self):
        geometry = box([0, -100, -100], [1, 100, 100])
        geometry.manifest["coverage"]["glass"] = False
        clear = geometry.validate_position([-20, 0, 0])
        self.assertTrue(clear["static_clear"])
        self.assertFalse(clear["complete"] or clear["passed"])
        self.assertEqual(clear["status"], "unknown")
        hit = geometry.validate_segment([-20, 0, 0], [20, 0, 0])
        self.assertFalse(hit["static_clear"])
        self.assertEqual(hit["status"], "blocked")

    def test_fully_bounded_unknowns_reject_region_and_allow_distant_path(self):
        geometry = box([0, -100, -100], [1, 100, 100],
                       unresolved_entities_bounds_complete=True,
                       unresolved_entities_bounds=[{"bounds": [[-50, -5, -5], [-40, 5, 5]], "reason": "door state unknown"}])
        geometry.manifest["coverage"]["dynamic_entities"] = False
        self.assertTrue(geometry.validate_position([-20, 20, 0])["passed"])
        result = geometry.validate_segment([-70, 0, 0], [-20, 0, 0])
        self.assertEqual(result["collision"]["kind"], "unresolved_entity_bounds")
        self.assertFalse(result["passed"])
        geometry.unknown_bounds.clear()
        self.assertFalse(geometry.status()["spatial_mutations_allowed"])

    def test_unknown_mesh_interiors_cannot_be_authorized_by_manifest_flag(self):
        geometry = box([0, -100, -100], [1, 100, 100])
        geometry.components[0]["convex"] = False
        self.assertIn("inside_solid", geometry.status()["missing_coverage"])

    def test_project_live_fov_and_easing_preserve_geometry(self):
        value = project([[-30, 20, 0], [-30, 30, 0]], "smooth")
        value["keys"][0].update(fov=170, easing=[1, 1, 0, 0])
        positions, cubics = project_cubics(value)
        self.assertEqual(len(cubics), 1)
        self.assertEqual(positions[0], (-30., 20., 0.))
        value["keys"][0]["fov"] = float("nan")
        with self.assertRaises(GeometryError):
            project_cubics(value)

    def test_plane_preview_is_actual_bounded_geometry(self):
        geometry = box([0, -100, -100], [1, 100, 100])
        result = geometry.preview()
        self.assertLessEqual(len(result["layers"]), 8)
        self.assertTrue(all(len(x["lines"]) <= 1000 for x in result["layers"]))
        for layer in result["layers"]:
            for line in layer["lines"]:
                self.assertTrue(all(np.isfinite(line)))
                self.assertTrue(0 <= line[0] <= 1 and 0 <= line[2] <= 1)

    def test_preview_limits_utf8_labels_and_partial_cache_never_authorizes(self):
        geometry = box([0, -100, -100], [1, 100, 100], unsupported=["неполное покрытие" * 200])
        geometry.manifest["ready"] = False
        preview = geometry.preview()
        self.assertLessEqual(len(preview["reason"].encode("utf-8")), 1024)
        self.assertEqual(preview["status"], "partial")
        self.assertFalse(geometry.validate_position([-30, 30, 0])["passed"])

    def test_cache_hashes_and_installed_source_changes_fail_closed(self):
        geometry = box([0, -100, -100], [1, 100, 100])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "map.vpk"; source.write_bytes(b"actual map identity")
            arrays = root / "geometry.npz"
            np.savez_compressed(arrays, vertices=geometry.vertices, triangles=geometry.triangles,
                                triangle_component=geometry.triangle_component)
            manifest = copy.deepcopy(geometry.manifest)
            manifest["source_files"] = [{"path": str(source), "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}]
            manifest["geometry_sha256"] = hashlib.sha256(arrays.read_bytes()).hexdigest()
            (root / "manifest.json").write_text(json.dumps(manifest))
            self.assertTrue(load_geometry(root).status()["spatial_mutations_allowed"])
            source.write_bytes(b"changed map identity")
            with self.assertRaisesRegex(GeometryError, "map changed"):
                load_geometry(root)
            source.write_bytes(b"actual map identity")
            arrays.write_bytes(b"tampered")
            with self.assertRaisesRegex(GeometryError, "NPZ hash mismatch"):
                load_geometry(root)


if __name__ == "__main__":
    unittest.main()
