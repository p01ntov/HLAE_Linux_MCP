import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.camera_guard import guarded_camera_call, validate_camera_path, preview_for_demo
from cs2_mvp.live import LiveError
from cs2_mvp.live_mcp import call_tool
from test_camera_collision import box, project


class GuardClient:
    def __init__(self, positions=None):
        self.state = {"project": project(positions or [[-30, 30, 0], [-30, 60, 0]]),
                      "camera": {"position": [-30, 30, 0], "angles": [0, 180, 0], "fov": 74},
                      "revision": 7, "tick": 640, "tick_rate": 64, "cursor": 0,
                      "demo": True, "paused": True, "camera_valid": True,
                      "view_hook_installed": True, "demo_seek": {"active": False, "queued": False},
                      "guarded_mutation_abi": 1}
        self.calls, self.mutations = [], []
        self.race = False

    def request(self, method, params=None):
        self.calls.append((method, copy.deepcopy(params)))
        if method == "get_state":
            snapshot = copy.deepcopy(self.state)
            if self.race:
                self.state["revision"] += 1
            return snapshot
        if method == "export_project":
            return copy.deepcopy(self.state["project"])
        if method == "guarded_mutation":
            if params["expected_revision"] != self.state["revision"]:
                raise LiveError("camera changed during geometry validation")
            if params["expected_tick"] != self.state["tick"] or params["expected_paused"] != self.state["paused"]:
                raise LiveError("demo changed during geometry validation")
            self.mutations.append((params["method"], copy.deepcopy(params["params"])))
            return {"applied": True}
        self.mutations.append((method, copy.deepcopy(params)))
        return {"method": method, "params": params}


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.client = GuardClient()
        self.geometry = box([0, -10, -100], [1, 10, 100])
        self.context = patch("cs2_mvp.camera_guard.session_geometry", return_value=self.geometry)
        self.context.start(); self.addCleanup(self.context.stop)

    def test_safe_pose_uses_native_compare_and_set_with_explicit_defaults(self):
        result = guarded_camera_call(self.client, "set_camera", {"position": [-30, 40, 0]})
        self.assertTrue(result["applied"])
        method, params = self.client.calls[-1]
        self.assertEqual(method, "guarded_mutation")
        self.assertEqual(params["expected_revision"], 7)
        self.assertEqual(params["expected_tick"], 640)
        self.assertTrue(params["expected_paused"])
        self.assertEqual(params["params"]["angles"], [0, 180, 0])

    def test_rejected_wall_project_and_load_do_not_change_original(self):
        original = copy.deepcopy(self.client.state)
        unsafe = project([[-30, 0, 0], [30, 0, 0]])
        for method, args in (("set_project", {"keys": unsafe["keys"]}), ("import_project", {"project": unsafe})):
            with self.subTest(method=method), self.assertRaisesRegex(LiveError, "original project preserved"):
                guarded_camera_call(self.client, method, args)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "unsafe.json"; path.write_text(json.dumps(unsafe))
            with self.assertRaisesRegex(LiveError, "original project preserved"):
                call_tool(self.client, "cs2_camera_load_project", {"path": str(path)})
        self.assertEqual(self.client.state, original)
        self.assertEqual(self.client.mutations, [])

    def test_removing_waypoint_can_make_a_wall_crossing(self):
        self.client = GuardClient([[-30, -30, 0], [-30, 30, 0], [30, 30, 0]])
        self.assertTrue(self.geometry.validate_project(self.client.state["project"])["passed"])
        with self.assertRaisesRegex(LiveError, "rejected"):
            guarded_camera_call(self.client, "remove_key", {"id": "1"})
        self.assertEqual(len(self.client.state["project"]["keys"]), 3)
        self.assertEqual(self.client.mutations, [])

    def test_add_key_defaults_capture_current_pose_but_check_new_path(self):
        self.client = GuardClient([[-30, 0, 0]])
        self.client.state["camera"]["position"] = [30, 0, 0]
        self.client.state["cursor"] = .5
        with self.assertRaisesRegex(LiveError, "rejected"):
            guarded_camera_call(self.client, "add_key", {})
        self.assertEqual(self.client.mutations, [])

    def test_editing_either_spatial_handle_checks_the_entire_candidate(self):
        for identity, field in (("0", "position_out"), ("1", "position_in")):
            client = GuardClient([[-30, -5, 0], [-30, 5, 0]])
            client.state["project"]["interpolation"] = "bezier"
            original = copy.deepcopy(client.state)
            with self.subTest(field=field), self.assertRaisesRegex(LiveError, "original project preserved"):
                guarded_camera_call(client, "update_key", {"id": identity, field: [200, 0, 0]})
            self.assertEqual(client.state, original)
            self.assertEqual(client.mutations, [])

    def test_null_handle_update_can_repair_a_saved_unsafe_curve(self):
        self.client = GuardClient([[-30, -5, 0], [-30, 5, 0]])
        self.client.state["project"]["interpolation"] = "bezier"
        self.client.state["project"]["keys"][0]["position_out"] = [200, 0, 0]
        self.assertFalse(self.geometry.validate_project(self.client.state["project"])["passed"])
        guarded_camera_call(self.client, "update_key", {"id": "0", "position_out": None})
        self.assertEqual(self.client.mutations[-1], ("update_key", {"id": "0", "position_out": None}))

    def test_new_key_controls_participate_in_preflight_and_preserve_defaults(self):
        self.client = GuardClient([[-30, -5, 0]])
        self.client.state["project"]["interpolation"] = "bezier"
        self.client.state["camera"]["position"] = [-30, 5, 0]
        self.client.state["cursor"] = 1
        with self.assertRaisesRegex(LiveError, "rejected"):
            guarded_camera_call(self.client, "add_key", {"position_in": [200, 0, 0]})
        self.assertEqual(self.client.mutations, [])
        controls = {"position_in": None, "rotation_in": [0, -20, 0], "fov_in": -5,
                    "position_easing": None, "rotation_easing": [0, 0, 1, 1]}
        guarded_camera_call(self.client, "add_key", controls)
        method, value = self.client.mutations[-1]
        self.assertEqual(method, "add_key")
        self.assertEqual(value, {**controls, **self.client.state["camera"], "time": 1})

    def test_activating_preexisting_handles_cannot_bypass_collision_check(self):
        self.client = GuardClient([[-30, -5, 0], [-30, 5, 0]])
        self.client.state["project"]["keys"][0]["position_out"] = [200, 0, 0]
        self.assertTrue(self.geometry.validate_project(self.client.state["project"])["passed"])
        with self.assertRaisesRegex(LiveError, "rejected"):
            guarded_camera_call(self.client, "set_project", {"interpolation": "bezier"})
        self.assertEqual(self.client.mutations, [])

    def test_candidate_preserves_all_channel_controls_and_modes(self):
        value = project([[-30, -5, 0], [-30, 5, 0]], "bezier")
        value.update(rotation_interpolation="bezier", fov_interpolation="bezier")
        value["keys"][0].update(position_out=[-10, 0, 0], position_in=None,
                                rotation_out=[10, 45, 5], rotation_in=None, fov_out=8, fov_in=None,
                                position_easing=[.1, .2, .8, 1], rotation_easing=None,
                                fov_easing=[0, 0, 1, 1])
        value["keys"][1].update(position_in=[-10, 0, 0], rotation_in=[-5, -20, 0], fov_in=-6)
        original = copy.deepcopy(value)
        for method, args in (("set_project", value), ("import_project", {"project": value})):
            with self.subTest(method=method):
                guarded_camera_call(self.client, method, args)
                self.assertEqual(self.client.mutations[-1], (method, args))
        self.assertEqual(value, original)
        self.client.state["project"] = value
        self.assertEqual(guarded_camera_call(self.client, "export_project", {}), original)

    def test_channel_mode_changes_remain_fail_closed_on_partial_geometry(self):
        self.geometry.manifest["coverage"]["glass"] = False
        for field in ("interpolation", "rotation_interpolation", "fov_interpolation"):
            with self.subTest(field=field), self.assertRaisesRegex(LiveError, "rejected"):
                guarded_camera_call(self.client, "set_project", {field: "bezier"})
        self.assertEqual(self.client.mutations, [])

    def test_partial_geometry_is_diagnostic_only_and_no_bypass_schema(self):
        self.geometry.manifest["coverage"]["glass"] = False
        report = validate_camera_path(self.client, project=project([[-30, 0, 0], [30, 0, 0]]))
        self.assertEqual(report["status"], "blocked")
        self.assertFalse(report["complete"] or report["passed"])
        with self.assertRaisesRegex(LiveError, "rejected"):
            guarded_camera_call(self.client, "set_camera", {"position": [-30, 40, 0]})
        from cs2_mvp.live_mcp import ProtocolError
        with self.assertRaises(ProtocolError):
            call_tool(self.client, "cs2_camera_set_camera", {"position": [-30, 40, 0], "ignore_geometry": True})
        self.assertEqual(self.client.mutations, [])

    def test_nonspatial_edits_and_preview_stop_remain_available(self):
        self.geometry.manifest["coverage"]["glass"] = False
        guarded_camera_call(self.client, "set_project", {"name": "Saved draft"})
        guarded_camera_call(self.client, "preview", {"playing": False})
        self.assertEqual([x[0] for x in self.client.mutations], ["set_project", "preview"])

    def test_fov_only_edit_cannot_hold_a_camera_inside_a_wall(self):
        self.client.state["camera"]["position"] = [.5, 0, 0]
        with self.assertRaisesRegex(LiveError, "inside_solid"):
            guarded_camera_call(self.client, "set_camera", {"fov": 90})
        self.assertEqual(self.client.mutations, [])

    def test_preset_is_materialized_and_unsafe_dolly_is_rejected(self):
        self.client.state["camera"].update(position=[-30, 0, 0], angles=[0, 0, 0])
        with self.assertRaisesRegex(LiveError, "rejected"):
            guarded_camera_call(self.client, "apply_preset", {"name": "dolly", "distance": 80})
        self.client.state["camera"].update(position=[-100, -100, 0], angles=[0, 180, 0])
        guarded_camera_call(self.client, "apply_preset", {"name": "orbit", "distance": 20, "duration": 3})
        method, value = self.client.mutations[-1]
        self.assertEqual(method, "set_project")
        self.assertEqual(len(value["keys"]), 5)
        self.assertEqual(value["keys"][0]["position"], [-100., -100., 0])
        self.assertTrue(self.client.calls[-1][1]["reset_cursor"])

    def test_state_race_does_not_apply_the_prevalidated_project(self):
        self.client.race = True
        with self.assertRaisesRegex(LiveError, "camera changed"):
            guarded_camera_call(self.client, "set_camera", {"position": [-30, 40, 0]})
        self.assertEqual(self.client.mutations, [])

    def test_missing_native_cas_or_unpaused_demo_fails_before_mutation(self):
        for key, value in (("guarded_mutation_abi", None), ("paused", False), ("camera_valid", False)):
            with self.subTest(key=key):
                client = GuardClient(); client.state[key] = value
                with self.assertRaises(LiveError):
                    guarded_camera_call(client, "set_camera", {"position": [-30, 40, 0]})
                self.assertEqual(client.mutations, [])

    def test_undo_and_redo_cannot_restore_uninspected_paths(self):
        for method in ("undo", "redo"):
            with self.assertRaisesRegex(LiveError, "cannot inspect"):
                guarded_camera_call(self.client, method, {})
        self.assertEqual(self.client.mutations, [])

    def test_look_at_is_collision_line_not_render_visibility(self):
        report = validate_camera_path(self.client, position=[-30, 0, 0], look_at=[30, 0, 0])
        self.assertFalse(report["passed"])
        self.assertEqual(report["look_at_check"]["status"], "blocked")
        self.assertFalse(report["render_visibility_checked"] or report["frustum_checked"])

    def test_get_state_exposes_guard_readiness_without_writing_game_state(self):
        self.geometry.manifest["coverage"]["glass"] = False
        state = call_tool(self.client, "cs2_camera_get_state", {})
        self.assertFalse(state["geometry_guard"]["spatial_mutations_allowed"])
        self.assertTrue(state["geometry_guard"]["native_compare_and_set_available"])
        self.assertEqual(self.client.mutations, [])

    def test_missing_cache_is_explicit_unknown_and_preview_label_is_bounded(self):
        with patch("cs2_mvp.camera_guard.session_geometry", side_effect=FileNotFoundError("missing geometry")):
            result = validate_camera_path(self.client, position=[-30, 30, 0])
            self.assertFalse(result["passed"] or result["complete"] or result["static_clear"])
            self.assertEqual(result["status"], "unknown")
        with patch("cs2_mvp.camera_guard.geometry_for_demo", side_effect=FileNotFoundError("нет индекса" * 200)):
            preview = preview_for_demo("recording.dem")
            self.assertEqual(preview["status"], "unavailable")
            self.assertLessEqual(len(preview["reason"].encode("utf-8")), 1024)


if __name__ == "__main__":
    unittest.main()
