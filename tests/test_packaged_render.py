import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

from cs2_mvp import camera_backend, discovery, live, render


class BundledCaptureTests(unittest.TestCase):
    def bundle(self, directory):
        root = Path(directory)
        native = root / "native/capture"
        native.mkdir(parents=True)
        plugin = native / "libcamera_capture.so"
        plugin.write_bytes(b"\x7fELF\x02\x01" + b"CS2_MVP_MOTION_ABI 3 CS2_MVP_ENVIRONMENT_ABI 1")
        manifest = {"version": 1, "motion_abi": 3, "environment_abi": 1, "upstream_commit": discovery.CSDM_COMMIT,
                    "capture_sha256": discovery.sha256(plugin),
                    "dependencies": {"easywsclient": "afc1d8cfc584e0f1f4a77e8c0ce3e979d9fe7ce2",
                                     "hl2sdk": "dc8e7324b1e7f3cb115b1a861280a30ba5ce72a2",
                                     "json": "a03e65420ca9a14c97080b67036859aaac06700d"}}
        (native / "plugins.json").write_text(json.dumps(manifest))
        return root, plugin, manifest

    def test_frozen_capture_uses_verified_bundle_without_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            root, plugin, _ = self.bundle(directory)
            with patch.object(sys, "frozen", True, create=True), patch.object(sys, "_MEIPASS", str(root), create=True), \
                 patch.object(camera_backend.subprocess, "run", side_effect=AssertionError("Cannot build in an AppImage")):
                self.assertEqual(camera_backend.ensure_motion_plugin(), plugin)
                plugin.write_bytes(plugin.read_bytes() + b"tampered")
                with self.assertRaisesRegex(RuntimeError, "ELF/ABI/hash"):
                    camera_backend.ensure_motion_plugin()

    def test_frozen_capture_rejects_missing_abi_even_with_matching_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root, plugin, manifest = self.bundle(directory)
            plugin.write_bytes(b"\x7fELF\x02\x01" + bytes(64))
            manifest["capture_sha256"] = discovery.sha256(plugin)
            (plugin.parent / "plugins.json").write_text(json.dumps(manifest))
            with patch.object(sys, "frozen", True, create=True), patch.object(sys, "_MEIPASS", str(root), create=True):
                with self.assertRaisesRegex(RuntimeError, "ELF/ABI/hash"):
                    camera_backend.ensure_motion_plugin()

    def test_frozen_paths_separate_resources_from_durable_state(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(sys, "frozen", True, create=True), patch.object(sys, "_MEIPASS", "/readonly/AppImage", create=True), \
             patch.dict(os.environ, {"XDG_STATE_HOME": directory}):
            self.assertEqual(discovery.resource_root(), Path("/readonly/AppImage"))
            self.assertEqual(discovery.capture_root(), Path(directory) / "cs2-camera/capture")

    def test_capture_environment_restores_host_paths_and_removes_live_injection(self):
        poisoned = {"LD_LIBRARY_PATH": "/app/PySide6:/app/lib", "LD_LIBRARY_PATH_ORIG": "/usr/local/lib",
                    "LD_PRELOAD": "/home/user/session/native/libcs2_camera_overlay.so:/user/other.so",
                    "VK_INSTANCE_LAYERS": "VK_LAYER_CS2_camera:VK_LAYER_USER_overlay",
                    "PYTHONHOME": "/app", "QT_PLUGIN_PATH": "/app/Qt/plugins", "CS2_CAMERA_LIVE": "1",
                    "CS2_CAMERA_SOCKET": "/private/control.sock", "_PYI_APPLICATION_HOME_DIR": "/app"}
        with patch.object(sys, "frozen", True, create=True), patch.dict(os.environ, poisoned, clear=True):
            environment = discovery.external_environment()
        self.assertEqual(environment["LD_LIBRARY_PATH"], "/usr/local/lib")
        self.assertEqual(environment["LD_PRELOAD"], "/user/other.so")
        self.assertEqual(environment["VK_INSTANCE_LAYERS"], "VK_LAYER_USER_overlay")
        for name in ("PYTHONHOME", "QT_PLUGIN_PATH", "CS2_CAMERA_LIVE", "CS2_CAMERA_SOCKET", "_PYI_APPLICATION_HOME_DIR"):
            self.assertNotIn(name, environment)

    def test_source_capture_also_removes_only_this_apps_inherited_injection(self):
        inherited={"LD_LIBRARY_PATH":"/host/lib", "LD_PRELOAD":"/session/libcs2_camera_overlay.so:/user/other.so",
                   "VK_INSTANCE_LAYERS":"VK_LAYER_CS2_camera:VK_LAYER_USER_overlay", "CS2_CAMERA_LIVE":"1",
                   "CS2_CAMERA_SOCKET":"/session/control.sock", "CS2_CAMERA_OVERLAY":"1", "CS2_CAMERA_PROJECT":"/session/project.json",
                   "CS2_CAMERA_COSMETICS_CATALOG":"/session/catalog.json", "CS2_CAMERA_COSMETICS_CATALOG_ERROR":"old error",
                   "CS2_CAMERA_CAPTURE_COSMETICS":"1", "CS2_CAMERA_FULLSCREEN_WINDOWED":"1"}
        with patch.object(sys,"frozen",False,create=True),patch.dict(os.environ,inherited,clear=True):
            environment=discovery.external_environment()
        self.assertEqual(environment["LD_LIBRARY_PATH"],"/host/lib")
        self.assertEqual(environment["LD_PRELOAD"],"/user/other.so")
        self.assertEqual(environment["VK_INSTANCE_LAYERS"],"VK_LAYER_USER_overlay")
        self.assertFalse(any(name.startswith("CS2_CAMERA_") for name in environment))

    def test_packaged_render_obeys_live_session_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root / "runtime"
            runtime.mkdir()
            with patch.object(sys, "frozen", True, create=True), patch.object(render, "ROOT", root / "capture"), \
                 patch.object(live, "runtime_directory", return_value=runtime):
                with live.session_locks():
                    with self.assertRaisesRegex(RuntimeError, "Another live session or render"):
                        with render.capture_lock():
                            self.fail("Packaged render overlapped an editor")

    def test_capture_lock_releases_after_preparation_error(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(render, "ROOT", Path(directory)), \
             patch.object(live, "runtime_directory", return_value=Path(directory)):
            with self.assertRaisesRegex(RuntimeError, "build failed"):
                with render.capture_lock():
                    raise RuntimeError("build failed")
            with render.capture_lock():
                pass

    def test_source_render_also_obeys_shared_editor_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(sys, "frozen", False, create=True), patch.object(render, "ROOT", root), \
                 patch.object(live, "runtime_directory", return_value=root), \
                 patch.object(live, "resource_root", return_value=root):
                with live.session_locks():
                    with self.assertRaisesRegex(RuntimeError, "Another live session or render"):
                        with render.capture_lock():
                            self.fail("Source render overlapped an editor")

    def test_active_capture_holds_finalization_lock_until_owner_finishes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / ".runs/job"
            run.mkdir(parents=True)
            with patch.object(render, "ROOT", root):
                with render.finalization_lock(run):
                    with self.assertRaisesRegex(RuntimeError, "active or already being finalized"):
                        render.finish_capture(run)

    def test_frozen_config_restore_retains_backup_while_zombie_exists(self):
        from cs2_mvp.csdm import CsdmError
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            manifest = run / "config-backup.json"
            manifest.write_text("[]")
            with patch.object(sys, "frozen", True, create=True), \
                 patch("cs2_mvp.csdm._assert_game_stopped", side_effect=CsdmError("CS2 zombie is still present")):
                with self.assertRaisesRegex(RuntimeError, "config backups retained"):
                    render.restore_configs(run)
            self.assertEqual(manifest.read_text(), "[]")

    def test_frozen_dry_run_compiles_1000_samples_into_writable_state_without_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            demo, camera = root / "input.dem", root / "camera.json"
            demo.write_bytes(b"fixture")
            camera.write_text(json.dumps({"version": 1, "type": "static", "position": [0, 0, 64], "angles": [0, 0]}))
            capture = root / "state/cs2-camera/capture"
            environment = {"game_directory": str(root), "steam_running": True, "display": ":test", "cs2_pids": [],
                           "ffmpeg": "ffmpeg", "ffprobe": "ffprobe", "game_free_bytes": 10**15}
            with patch.object(sys, "frozen", True, create=True), patch.object(render, "ROOT", capture), \
                 patch.object(render, "doctor", return_value=environment), patch.object(render, "parser") as parser, \
                 patch.object(render, "validate_media_tools"), \
                 patch.object(render.subprocess, "Popen", side_effect=AssertionError("Dry run must never launch CS2")):
                parser.return_value.parse_header.return_value = {}
                result = render.render_clip(demo, None, 1000, 1008, camera=camera, fps=60,
                                            capture_fps=1000, width=320, height=240, dry_run=True)
            self.assertEqual(result["camera_summary"]["frames"], 125)
            self.assertEqual(result["capture_fps"], 1000)
            self.assertEqual(result["fps"], 60)
            self.assertTrue(str(result["output"]).startswith(str(capture / "outputs")))
            self.assertFalse((capture / ".runs").exists())

    def test_portable_entry_dispatch_preserves_render_arguments(self):
        path = Path(__file__).resolve().parents[1] / "packaging/live/entry.py"
        spec = importlib.util.spec_from_file_location("portable_entry_test", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        arguments = ["Camera.AppImage", "render", "demo.dem", "--capture-fps", "1000"]
        with patch.object(sys, "argv", arguments), patch("cs2_mvp.cli.main", return_value=0) as cli:
            self.assertEqual(module.main(), 0)
            cli.assert_called_once_with()
            self.assertEqual(sys.argv, arguments)

    def test_finalization_is_serialized_without_game_discovery(self):
        import fcntl
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / ".runs/job"
            run.mkdir(parents=True)
            with patch.object(render, "ROOT", root), (run / "finalize.lock").open("a") as held, \
                 patch.object(render, "doctor", side_effect=AssertionError("Offline finish must not discover CS2")):
                fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with self.assertRaisesRegex(RuntimeError, "already being finalized"):
                    render.finish_capture(run)


if __name__ == "__main__":
    unittest.main()
