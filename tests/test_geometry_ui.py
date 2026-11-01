import unittest
from unittest.mock import Mock, patch

from cs2_mvp.geometry_ui import service_validation


class GeometryUiTests(unittest.TestCase):
    def test_idle_worker_does_not_load_or_check_geometry(self):
        client = Mock()
        for request in (0, 8, "9", True):
            client.request.return_value = {"geometry_validation_request_id": request}
            with patch("cs2_mvp.camera_guard.validate_camera_path") as validate:
                self.assertEqual(service_validation(client, "demo", "game", handled=8), 8)
                validate.assert_not_called()
        self.assertTrue(all(call.args == ("get_state",) for call in client.request.call_args_list))

    def test_explicit_check_is_bound_to_the_captured_document_revision(self):
        state = {"geometry_validation_request_id": 9, "revision": 42, "project": {"name": "editing"}}
        client = Mock()
        client.request.side_effect = [state, {"accepted": False}]
        result = {"passed": False, "status": "blocked", "collision": {"position": [1, 2, 3]}}
        with patch("cs2_mvp.camera_guard.validate_camera_path", return_value=result) as validate:
            self.assertEqual(service_validation(client, "demo", "game", handled=8), 9)
            validate.assert_called_once_with(client, project=state["project"], demo="demo", game_dir="game")
        self.assertEqual(client.request.call_args.args, ("set_geometry_validation", {
            "request_id": 9, "expected_revision": 42, "result": result}))
        # A stale reply is never retried against a newer revision or turned into a write.
        self.assertEqual(client.request.call_count, 2)

    def test_unavailable_geometry_reports_unknown_without_camera_mutation(self):
        client = Mock()
        client.request.side_effect = [{"geometry_validation_request_id": 1, "revision": 4, "project": {}}, {}]
        with patch("cs2_mvp.camera_guard.validate_camera_path", side_effect=ValueError("cache unavailable")):
            service_validation(client, "demo", "game")
        result = client.request.call_args.args[1]["result"]
        self.assertFalse(result["passed"])
        self.assertFalse(result["static_clear"])
        self.assertEqual(result["status"], "unknown")


if __name__ == "__main__":
    unittest.main()
