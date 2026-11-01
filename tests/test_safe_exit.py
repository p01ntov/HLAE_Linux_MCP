"""Safe-exit checks use temporary files and fake IPC/processes only."""
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
import importlib.util
import subprocess
import sys
from unittest.mock import Mock, patch

from cs2_mvp import live


class SafeExitTests(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec("PySide6"), "Qt runtime required")
    def test_offscreen_button_window_close_and_retry_preserve_save_barrier(self):
        for mode in ("button", "close", "retry"):
            with self.subTest(mode=mode):
                result = subprocess.run([sys.executable, str(Path(__file__).with_name("gui_safe_exit_probe.py")), mode],
                                        capture_output=True, text=True,
                                        env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), timeout=8)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("SAFE_EXIT_GUI_PASS " + mode, result.stdout)

    def fixture(self, root):
        run, projects = root / "run", root / "projects"
        run.mkdir(); projects.mkdir()
        project = {"name": "Manual camera", "duration": 6.2, "keys": [{"time": 1.25, "fov": 80}]}
        state = {"project": project, "revision": 17, "project_document_id": 3}
        path = projects / "project-current.json"
        calls = []

        def request(method, params=None):
            calls.append((method, params))
            if method == "get_state":
                return json.loads(json.dumps(state))
            if method == "export_project":
                return json.loads(json.dumps(project))
            if method in {"save_project", "save_project_copy"}:
                live._write_json(path, project)
                return {"saved": True, "path": str(path), "id": "project-current"}
            if method == "set_safe_exit_status":
                return {}
            raise AssertionError(method)

        client = Mock()
        client.request.side_effect = request
        return run, projects, project, state, path, calls, client, request

    def test_save_barrier_confirms_library_and_fsynced_checkpoint_before_closing(self):
        with tempfile.TemporaryDirectory() as temporary:
            run, projects, project, _, path, calls, client, _ = self.fixture(Path(temporary))
            gate = live.SafeExitGate(); gate.request.set(); phases = []
            original_fsync = os.fsync
            durable = []
            def fsync(fd):
                info = os.fstat(fd)
                durable.append((info.st_dev, info.st_ino))
                return original_fsync(fd)
            with patch.object(live.os, "fsync", side_effect=fsync):
                self.assertTrue(gate.attempt(client, run, projects, lambda phase, error: phases.append((phase, error))))
            self.assertEqual(json.loads(path.read_text()), project)
            self.assertEqual(json.loads((run / "project.json").read_text()), project)
            self.assertEqual((run / "project.json").stat().st_mode & 0o777, 0o600)
            self.assertEqual(phases, [("saving", ""), ("closing", "")])
            self.assertEqual(calls[-1], ("set_safe_exit_status", {"phase": "closing"}))
            # This fake client cannot prove the native library writer's fsync.
            # Check the real host checkpoint file and parent directory only.
            for destination in (run / "project.json", run):
                info = destination.stat()
                self.assertIn((info.st_dev, info.st_ino), durable)
            self.assertFalse(gate.request.is_set())

    def test_failed_save_does_not_close_and_fresh_native_request_retries(self):
        with tempfile.TemporaryDirectory() as temporary:
            run, projects, _, _, _, calls, client, request = self.fixture(Path(temporary))
            failed = [True]
            def save(method, params=None):
                if method in {"save_project", "save_project_copy"} and failed[0]:
                    raise live.LiveError("disk is full")
                return request(method, params)
            client.request.side_effect = save
            gate = live.SafeExitGate(); phases = []
            gate.observe({"safe_exit_request_id": 10})
            self.assertFalse(gate.attempt(client, run, projects, lambda *value: phases.append(value)))
            self.assertFalse((run / "project.json").exists())
            self.assertIn("disk is full", phases[-1][1])
            self.assertEqual(phases[-1][0], "error")
            self.assertNotIn(("set_safe_exit_status", {"phase": "closing"}), calls)
            gate.observe({"safe_exit_request_id": 10})
            self.assertFalse(gate.request.is_set())  # old monitor snapshots never retry/loop
            failed[0] = False
            gate.observe({"safe_exit_request_id": 11})
            self.assertTrue(gate.attempt(client, run, projects))

    def test_fresh_library_copy_can_replace_unwritable_existing_document(self):
        with tempfile.TemporaryDirectory() as temporary:
            run, projects, project, _, path, calls, client, request = self.fixture(Path(temporary))
            original = b'{"name":"Previously saved camera"}\n'
            path.write_bytes(original)
            copied = projects / "project-recovered-copy.json"
            def save(method, params=None):
                if method == "save_project":
                    raise live.LiveError("existing destination is read-only")
                if method == "save_project_copy":
                    calls.append((method, params))
                    live._write_json(copied, project)
                    return {"saved": True, "path": str(copied), "id": "project-recovered-copy"}
                return request(method, params)
            client.request.side_effect = save
            gate = live.SafeExitGate(); gate.request.set()
            self.assertTrue(gate.attempt(client, run, projects))
            self.assertIn(("save_project_copy", None), calls)
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(json.loads(copied.read_text()), project)
            self.assertEqual(json.loads((run / "project.json").read_text()), project)

    def test_stale_library_file_or_mid_save_edits_keep_session_open(self):
        for failure in ("stale_file", "revision", "document", "checkpoint"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                run, projects, project, state, path, calls, client, request = self.fixture(Path(temporary))
                def save(method, params=None):
                    result = request(method, params)
                    if method == "save_project":
                        if failure == "stale_file":
                            path.write_text(json.dumps({**project, "duration": 5}))
                        elif failure == "revision":
                            state["revision"] += 1
                        elif failure == "document":
                            state["project_document_id"] += 1
                        else:
                            (run / "project.json").mkdir()
                    return result
                client.request.side_effect = save
                gate = live.SafeExitGate(); gate.request.set()
                self.assertFalse(gate.attempt(client, run, projects))
                self.assertNotIn(("set_safe_exit_status", {"phase": "closing"}), calls)
                self.assertEqual(calls[-1][0], "set_safe_exit_status")
                self.assertEqual(calls[-1][1]["phase"], "error")

    def test_untrusted_or_unconfirmed_save_destinations_cannot_authorize_close(self):
        for failure in ("not_saved", "outside", "symlink", "non_finite"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                run, projects, project, _, path, calls, client, request = self.fixture(root)
                def save(method, params=None):
                    if method == "export_project" and failure == "non_finite":
                        return {**project, "duration": float("nan")}
                    result = request(method, params)
                    if method == "save_project":
                        if failure == "not_saved":
                            result["saved"] = False
                        elif failure == "outside":
                            result["path"] = str(root / "outside.json")
                        elif failure == "symlink":
                            outside = root / "outside.json"
                            path.rename(outside); path.symlink_to(outside)
                    return result
                client.request.side_effect = save
                gate = live.SafeExitGate(); gate.request.set()
                self.assertFalse(gate.attempt(client, run, projects))
                self.assertNotIn(("set_safe_exit_status", {"phase": "closing"}), calls)
                self.assertEqual(calls[-1][0], "set_safe_exit_status")
                self.assertEqual(calls[-1][1]["phase"], "error")
                self.assertFalse((run / "project.json").exists())
                if failure == "non_finite":
                    self.assertFalse(any(method in {"save_project", "save_project_copy"} for method, _ in calls))
                    self.assertIn("Out of range float", calls[-1][1]["error"])

    def test_counter_validation_and_deduplication(self):
        gate = live.SafeExitGate()
        for value in (True, -1, 0, "1", 1.5, None):
            gate.observe({"safe_exit_request_id": value})
            self.assertFalse(gate.request.is_set())
        gate.observe({"safe_exit_request_id": 2})
        self.assertTrue(gate.request.is_set())
        gate.request.clear()
        for value in (0, 1, 2):
            gate.observe({"safe_exit_request_id": value})
            self.assertFalse(gate.request.is_set())


class SafeExitSessionTests(unittest.TestCase):
    def run_mock_session(self, root, *, native_request=False, startup_cancel=False, fail_once=False, restore_error=False, no_demo=False,
                         initial_state_failures=0):
        from contextlib import ExitStack, contextmanager, nullcontext
        runtime, projects = root / "runtime", root / "projects"
        runtime.mkdir(); projects.mkdir()
        demo = root / "input.dem"; demo.write_bytes(b"PBDEMS2\0")
        endpoint = root / "control.sock"
        project = {"name": "User five keys", "keys": [{"time": number} for number in range(5)]}
        state = {"camera_valid": True, "view_hook_installed": True, "revision": 12,
                 "project_document_id": 4, "project": project, "safe_exit_request_id": int(native_request)}
        if no_demo:
            state.update(camera_valid=False, view_hook_installed=False)
        process = Mock(pid=1234); process.poll.return_value = None
        tracker = Mock(); tracker.observe.return_value = [{"name": "cs2"}]
        safe_exit = threading.Event(); stop = threading.Event(); order = []; notices = []; attempts = [0]
        phases = []; failed_state_reads = [0]
        saved_path = projects / "project-current.json"

        def request(method, params=None):
            if method == "get_state":
                if failed_state_reads[0] < initial_state_failures:
                    failed_state_reads[0] += 1
                    raise live.LiveError("simulated transient native state timeout")
                return dict(state)
            if method == "export_project":
                return project
            if method in {"save_project", "save_project_copy"}:
                if fail_once and attempts[0] < 2:
                    attempts[0] += 1
                    raise live.LiveError("simulated disk failure")
                live._write_json(saved_path, project); order.append("saved")
                return {"saved": True, "path": str(saved_path)}
            if method == "set_safe_exit_status":
                order.append(params["phase"])
            return {}

        def phase(value, error):
            phases.append(value)
            if value == "error":
                self.assertFalse(any(item == "stopped" for item in order))
                self.assertFalse(stop.is_set())
                if native_request:
                    state["safe_exit_request_id"] += 1
                else:
                    safe_exit.set()

        def launch(*args, **kwargs):
            if not startup_cancel:
                endpoint.touch()
                if initial_state_failures:
                    safe_exit.set()
            else:
                safe_exit.set()
            return process

        @contextmanager
        def transaction(name):
            try:
                yield
            finally:
                self.assertIn("stopped", order)
                order.append(name + "-restored")
                if restore_error and name == "config":
                    raise RuntimeError("config writer still active; backups retained")

        def stopped(*args):
            if not startup_cancel:
                self.assertEqual(json.loads(saved_path.read_text()), project)
                checkpoints = list((root / "state/runs").glob("*/project.json"))
                self.assertEqual(json.loads(checkpoints[0].read_text()), project)
                self.assertEqual(order[-1], "closing")
            order.append("stopped")

        with ExitStack() as stack:
            patches = [patch.object(live, "native_libraries", return_value=(Path("/plugin"), None)),
                       patch.object(live, "session_locks", return_value=nullcontext()),
                       patch.object(live, "runtime_directory", return_value=runtime),
                       patch.object(live, "control_socket_path", return_value=endpoint),
                       patch.object(live, "state_root", return_value=root / "state"),
                       patch.object(live, "hotkey_configuration", return_value=root / "hotkeys.json"),
                       patch.object(live, "staged_cosmetics_environment", return_value={}),
                       patch.object(live, "staged_demo_timeline_environment", return_value={}),
                       patch.object(live, "OwnedProcesses", return_value=tracker),
                       patch.dict(os.environ, {"DISPLAY": ":fake", "XDG_DATA_HOME": str(root / "data")}, clear=False),
                       patch("cs2_mvp.discovery.process_ids", side_effect=lambda name: [] if name == "cs2" else [1]),
                       patch("cs2_mvp.discovery.find_game", return_value=root),
                       patch("cs2_mvp.discovery.find_runtime", return_value=Path("/runtime")),
                       patch("cs2_mvp.discovery.sha256", return_value="fake"),
                       patch("cs2_mvp.csdm.temporary_plugin", side_effect=lambda *a, **k: transaction("plugin")),
                       patch("cs2_mvp.render.config_backup", side_effect=lambda *a, **k: transaction("config")),
                       patch("cs2_mvp.editor_preferences.configuration_path", return_value=root / "editor.json"),
                       patch("cs2_mvp.project_storage.geometry_directory", return_value=None),
                       patch.object(live, "desktop_size", side_effect=AssertionError("No desktop queries in internal tests")),
                       patch("cs2_mvp.editor_preferences.service_preferences", return_value=0),
                       patch("cs2_mvp.geometry_ui.service_validation", return_value=0),
                       patch("cs2_mvp.camera_guard.preview_for_demo", return_value={}),
                       patch.object(live.subprocess, "Popen", side_effect=launch),
                       patch.object(live.NativeClient, "request", side_effect=request),
                       patch.object(live, "_stop_owned", side_effect=stopped)]
            for replacement in patches:
                stack.enter_context(replacement)
            def ready(client):
                if not native_request:
                    safe_exit.set()
            kwargs = dict(headless=True, notify=notices.append, projects_dir=projects,
                          windowed=True,
                          stop=stop, safe_exit=safe_exit, on_ready=ready, on_safe_exit=phase)
            if restore_error:
                with self.assertRaisesRegex(RuntimeError, "backups retained"):
                    live.run_session(demo, **kwargs)
                result = json.loads(next((root / "state/runs").glob("*/session.json")).read_text())
            else:
                result = live.run_session(demo, **kwargs)
        return result, phases, order, notices

    def test_launcher_and_native_exit_save_before_owned_stop_and_restore(self):
        for native in (False, True):
            with self.subTest(native=native), tempfile.TemporaryDirectory() as temporary:
                result, phases, order, _ = self.run_mock_session(Path(temporary), native_request=native)
                self.assertEqual(result["status"], "closed")
                self.assertTrue(result["safe_exit"])
                self.assertEqual(phases, ["saving", "closing"])
                self.assertLess(order.index("saved"), order.index("stopped"))
                self.assertEqual(order[-2:], ["plugin-restored", "config-restored"])

    def test_save_failure_is_retryable_without_stopping_or_restoring_first(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, phases, order, _ = self.run_mock_session(Path(temporary), fail_once=True)
            self.assertEqual(result["status"], "closed")
            self.assertEqual(phases, ["saving", "error", "saving", "closing"])
            self.assertEqual(order.count("stopped"), 1)

    def test_startup_cancel_has_no_authored_project_to_discard(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, phases, order, notices = self.run_mock_session(Path(temporary), startup_cancel=True)
            self.assertEqual(result["status"], "closed")
            self.assertEqual(phases, ["closing"])
            self.assertTrue(any("Cancelling startup" in value for value in notices))
            self.assertIn("stopped", order)

    def test_connected_native_exit_can_save_without_active_demo_or_camera_hook(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, phases, order, _ = self.run_mock_session(Path(temporary), native_request=True, no_demo=True)
            self.assertEqual(result["status"], "closed")
            self.assertEqual(phases, ["saving", "closing"])
            self.assertIn("saved", order)

    def test_visible_endpoint_with_initial_timeouts_cannot_bypass_save_barrier(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, phases, order, notices = self.run_mock_session(Path(temporary), initial_state_failures=2)
            self.assertEqual(result["status"], "closed")
            self.assertEqual(phases, ["saving", "error", "saving", "closing"])
            self.assertEqual(order.count("stopped"), 1)
            self.assertLess(order.index("saved"), order.index("stopped"))
            self.assertFalse(any("Cancelling startup" in value for value in notices))

    def test_restoration_failure_never_returns_successful_safe_exit(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, _, order, _ = self.run_mock_session(Path(temporary), restore_error=True)
            self.assertEqual(result["status"], "failed")
            self.assertIn("backups retained", result["error"])
            self.assertIn("stopped", order)


if __name__ == "__main__":
    unittest.main()
