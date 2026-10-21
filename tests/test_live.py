import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch

from cs2_mvp import live


class NativeSocketTests(unittest.TestCase):
    def exchange(self, responder):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "control.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(path))
        server.listen(1)
        self.addCleanup(server.close)
        failures = []
        def handle():
            try:
                connection, _ = server.accept()
                with connection:
                    request = json.loads(connection.makefile("rb").readline())
                    responder(connection, request)
            except Exception as error:
                failures.append(error)
        thread = threading.Thread(target=handle)
        thread.start()
        self.addCleanup(thread.join, 2)
        return live.NativeClient(path, timeout=1), failures

    def test_split_json_roundtrip(self):
        def respond(connection, request):
            self.assertEqual(request["method"], "set_camera")
            self.assertEqual(request["params"], {"fov": 75})
            response = json.dumps({"id": request["id"], "result": {"camera": {"fov": 75}}}).encode() + b"\n"
            connection.sendall(response[:8])
            connection.sendall(response[8:])
        client, _ = self.exchange(respond)
        self.assertEqual(client.request("set_camera", {"fov": 75}), {"camera": {"fov": 75}})

    def test_native_error_is_actionable(self):
        def respond(connection, request):
            connection.sendall(json.dumps({"id": request["id"], "error": {"message": "Add a key before preview"}}).encode() + b"\n")
        client, _ = self.exchange(respond)
        with self.assertRaisesRegex(live.LiveError, "Add a key"):
            client.request("preview", {"playing": True})

    def test_wrong_response_id_rejected(self):
        client, _ = self.exchange(lambda connection, request: connection.sendall(b'{"id":"other","result":{}}\n'))
        with self.assertRaisesRegex(live.LiveError, "does not match"):
            client.request("get_state")

    def test_engine_exec_is_not_exposed(self):
        with self.assertRaisesRegex(live.LiveError, "Unknown"):
            live.NativeClient("/nonexistent").request("exec", {"command": "quit"})
        with self.assertRaisesRegex(live.LiveError, "JSON object"):
            live.NativeClient("/nonexistent").request("get_state", [])

    def test_session_missing_explains_how_to_start(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"XDG_RUNTIME_DIR": directory}, clear=False):
            with self.assertRaisesRegex(live.LiveError, "Start CS2 Camera Studio"):
                live.session_descriptor()
            self.assertEqual(live.runtime_directory().stat().st_mode & 0o777, 0o700)

    def test_session_directory_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"XDG_RUNTIME_DIR": directory}, clear=False):
            (Path(directory) / f"cs2-camera-{os.getuid()}").symlink_to(Path(directory), target_is_directory=True)
            with self.assertRaisesRegex(live.LiveError, "owned directory"):
                live.runtime_directory()


class LauncherTests(unittest.TestCase):
    def test_persistent_local_demo_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            demo = Path(directory) / "input.dem"
            demo.write_bytes(b"PBDEMS2\0")
            arguments = live.launch_arguments(Path("/runtime"), Path("/game"), demo, Path(directory), screen_size=(2560, 1440))
            from cs2_mvp.csdm import assert_demo_only_launch
            self.assertEqual(assert_demo_only_launch(arguments), demo)
            self.assertIn("-insecure", arguments)
            self.assertIn("-noborder", arguments)
            self.assertEqual(arguments[arguments.index("-w") + 1], "2560")
            self.assertNotIn("+quit", arguments)
            self.assertFalse(demo.with_name("input.dem.json").exists())

    def test_competing_render_lock_is_respected(self):
        import fcntl
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".runs").mkdir()
            with (root / ".runs/render.lock").open("w") as held:
                fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with patch.object(live, "runtime_directory", return_value=root), patch.object(live, "resource_root", return_value=root):
                    with self.assertRaisesRegex(live.LiveError, "Another live session or render"):
                        with live.session_locks():
                            self.fail("Competing job acquired the renderer lock")

    def test_existing_game_is_never_stopped(self):
        with tempfile.TemporaryDirectory() as directory:
            demo = Path(directory) / "test.dem"
            demo.write_bytes(b"PBDEMS2\0")
            from contextlib import nullcontext
            with patch.object(live, "native_libraries", return_value=(Path("/plugin"), None)), \
                 patch.object(live, "session_locks", return_value=nullcontext()), \
                 patch("cs2_mvp.discovery.process_ids", return_value=[1234]), \
                 patch.object(live, "_stop_owned") as stop:
                with self.assertRaisesRegex(live.LiveError, "left untouched"):
                    live.run_session(demo, headless=True)
                stop.assert_not_called()


class LiveConfigurationTests(unittest.TestCase):
    def test_catalog_failure_disables_cosmetics_without_blocking_camera_startup(self):
        notices=[]
        with patch("cs2_mvp.cosmetics_catalog.stage_catalog",side_effect=ValueError("unsupported installed schema")):
            environment=live.staged_cosmetics_environment(Path("/game"),Path("/private/run"),notices.append)
        self.assertNotIn("CS2_CAMERA_COSMETICS_CATALOG",environment)
        self.assertIn("unsupported installed schema",environment["CS2_CAMERA_COSMETICS_CATALOG_ERROR"])
        self.assertIn("unavailable",notices[-1])
        with patch("cs2_mvp.cosmetics_catalog.stage_catalog",return_value=Path("/private/run/catalog.json")):
            self.assertEqual(live.staged_cosmetics_environment(Path("/game"),Path("/private/run"),notices.append),
                             {"CS2_CAMERA_COSMETICS_CATALOG":"/private/run/catalog.json"})

    def test_hotkey_defaults_and_user_bindings_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"XDG_CONFIG_HOME": directory}, clear=False):
            with patch.dict(os.environ, {}, clear=False):
                os.environ.pop("CS2_CAMERA_HOTKEYS", None)
                path = live.hotkey_configuration()
                self.assertEqual(path, Path(directory) / "cs2-camera/hotkeys.json")
                self.assertEqual(json.loads(path.read_text()), {"capture": "F6", "camera": "F7", "editor": "F8", "preview": "F9"})
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                custom = '{"capture":"Ctrl+F6", "camera":"Shift+F7", "editor":"Alt+F8", "preview":"F10"}\n'
                path.write_text(custom)
                self.assertEqual(live.hotkey_configuration(), path)
                self.assertEqual(path.read_text(), custom)

    def test_explicit_hotkey_path_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "custom.json"
            path.write_text('{"editor":"Ctrl+F11"}')
            with patch.dict(os.environ, {"CS2_CAMERA_HOTKEYS": str(path)}, clear=False):
                self.assertEqual(live.hotkey_configuration(), path)
                self.assertEqual(path.read_text(), '{"editor":"Ctrl+F11"}')

    def test_font_only_resolves_a_real_bundled_asset(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(live, "resource_root", return_value=Path(directory)):
            self.assertIsNone(live.bundled_font())
            font = Path(directory) / "native/live/NotoSans-Regular.ttf"
            font.parent.mkdir(parents=True)
            font.write_bytes(b"font")
            self.assertEqual(live.bundled_font(), font)

    def test_installed_game_fonts_are_optional_read_only_references(self):
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory) / "game"
            self.assertEqual(live.installed_game_fonts(game), {})
            fonts = game / "csgo/panorama/fonts"
            fonts.mkdir(parents=True)
            sans = fonts / "notosans-regular.ttf"
            mono = fonts / "stratum2regular_monodigit.ttf"
            sans.write_bytes(b"installed-font")
            mono.mkdir()  # A directory is not a usable font asset.
            before = set(game.rglob("*"))
            environment = live.installed_game_fonts(game)
            self.assertEqual(environment, {"CS2_CAMERA_GAME_FONT": str(sans)})
            self.assertEqual(set(game.rglob("*")), before)
            self.assertEqual(sans.read_bytes(), b"installed-font")
            mono.rmdir()
            mono.write_bytes(b"installed-mono-font")
            self.assertEqual(live.installed_game_fonts(game)["CS2_CAMERA_MONO_FONT"], str(mono))

    def test_owned_game_stops_when_descriptor_write_fails_after_launch(self):
        from contextlib import nullcontext
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            demo = root / "test.dem"
            demo.write_bytes(b"PBDEMS2\0")
            runtime = root / "runtime"
            runtime.mkdir()
            process = Mock(pid=12345)
            original_write = live._write_json
            def write(path, value):
                if path == runtime / "session.json":
                    raise OSError("simulated descriptor failure")
                original_write(path, value)
            with patch.object(live, "native_libraries", return_value=(Path("/plugin"), None)), \
                 patch.object(live, "session_locks", return_value=nullcontext()), \
                 patch.object(live, "runtime_directory", return_value=runtime), \
                 patch.object(live, "state_root", return_value=root / "state"), \
                 patch.object(live, "hotkey_configuration", return_value=root / "hotkeys.json"), \
                 patch.dict(os.environ, {"DISPLAY": ":99", "XDG_DATA_HOME": str(root / "data")}, clear=False), \
                 patch("cs2_mvp.discovery.process_ids", side_effect=lambda name: [] if name == "cs2" else [1]), \
                 patch("cs2_mvp.discovery.find_game", return_value=root), \
                 patch("cs2_mvp.discovery.find_runtime", return_value=Path("/runtime")), \
                 patch("cs2_mvp.discovery.sha256", return_value="fake"), \
                 patch("cs2_mvp.csdm.temporary_plugin", return_value=nullcontext()), \
                 patch("cs2_mvp.render.config_backup", return_value=nullcontext()), \
                 patch.object(live.subprocess, "Popen", return_value=process), \
                 patch.object(live, "_write_json", side_effect=write), \
                 patch.object(live.NativeClient, "request", return_value={}), \
                 patch.object(live, "_stop_owned") as stop:
                with self.assertRaisesRegex(OSError, "descriptor failure"):
                    live.run_session(demo, headless=True, notify=lambda message: None, windowed=True)
                stop.assert_called_once()
                self.assertIs(stop.call_args.args[0], process)

    def test_definite_native_hook_failure_reports_guard_and_restores_owned_session(self):
        from contextlib import nullcontext
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);runtime=root/"runtime";runtime.mkdir()
            demo=root/"test.dem";demo.write_bytes(b"PBDEMS2\0")
            endpoint=root/"control.sock";process=Mock(pid=12345)
            process.poll.return_value=None
            tracker=Mock();tracker.observe.return_value=[{"name":"cs2"}]
            reason="Linux main-view hook requires the validated libclient GNU build-ID"
            states=[{"camera_valid":False,"view_hook_installed":False,"error":reason},
                    {"camera_valid":True,"view_hook_installed":False,"error":reason}]
            queries=[];notices=[]
            def request(method,params=None):
                queries.append(method)
                return states.pop(0) if method=="get_state" else {"keys":[]}
            def launch(*args,**kwargs):
                endpoint.touch()  # The native socket becomes visible after launch.
                return process
            with patch.object(live,"native_libraries",return_value=(Path("/plugin"),None)), \
                 patch.object(live,"session_locks",return_value=nullcontext()), \
                 patch.object(live,"runtime_directory",return_value=runtime), \
                 patch.object(live,"control_socket_path",return_value=endpoint), \
                 patch.object(live,"state_root",return_value=root/"state"), \
                 patch.object(live,"hotkey_configuration",return_value=root/"hotkeys.json"), \
                 patch.object(live,"staged_cosmetics_environment",return_value={}), \
                 patch.object(live,"OwnedProcesses",return_value=tracker), \
                 patch.dict(os.environ,{"DISPLAY":":99","XDG_DATA_HOME":str(root/"data")},clear=False), \
                 patch("cs2_mvp.discovery.process_ids",side_effect=lambda name:[] if name=="cs2" else [1]), \
                 patch("cs2_mvp.discovery.find_game",return_value=root), \
                 patch("cs2_mvp.discovery.find_runtime",return_value=Path("/runtime")), \
                 patch("cs2_mvp.discovery.sha256",return_value="fake"), \
                 patch("cs2_mvp.csdm.temporary_plugin",return_value=nullcontext()), \
                 patch("cs2_mvp.render.config_backup",return_value=nullcontext()), \
                 patch.object(live.subprocess,"Popen",side_effect=launch), \
                 patch.object(live.NativeClient,"request",side_effect=request), \
                 patch.object(live,"_stop_owned") as stop:
                with self.assertRaisesRegex(live.LiveError,"Native camera hook unavailable.*GNU build-ID"):
                    live.run_session(demo,headless=True,notify=notices.append,windowed=True)
            self.assertEqual(queries,["get_state","get_state","export_project"])
            stop.assert_called_once()
            self.assertIs(stop.call_args.args[0],process)
            self.assertFalse(endpoint.exists())
            self.assertFalse((runtime/"session.json").exists())
            saved=json.loads(next((root/"state/runs").glob("*/session.json")).read_text())
            self.assertEqual(saved["status"],"failed")
            self.assertIn(reason,saved["error"])
            self.assertNotIn("90 seconds",saved["error"])
            self.assertTrue(any("restoring" in notice for notice in notices))


class OwnedProcessTests(unittest.TestCase):
    @staticmethod
    def snapshot(pid, start, session, *, state="S", name="cs2", args=None):
        return {"pid": pid, "start": start, "session": session, "parent": 1,
                "state": state, "name": name, "args": args or []}

    def test_identity_survives_zombie_empty_cmdline_and_rejects_pid_reuse(self):
        launcher = self.snapshot(10, 100, 10, name="runtime")
        game = self.snapshot(20, 200, 10)
        zombie = {**game, "state": "Z", "args": []}
        reused = {**game, "start": 999, "session": 99}
        with patch.object(live, "_process_snapshot", return_value=launcher):
            tracker = live.OwnedProcesses(10, Path("/private/input.dem"))
        with patch.object(live, "_process_snapshots", return_value=[launcher, game]):
            self.assertEqual({item["pid"] for item in tracker.observe()}, {10, 20})
        with patch.object(live, "_process_snapshots", return_value=[zombie]):
            self.assertEqual(tracker.observe(), [zombie])
        with patch.object(live, "_process_snapshots", return_value=[reused]):
            self.assertEqual(tracker.observe(), [])

    def test_game_moved_to_runtime_session_is_owned_only_by_exact_demo_link(self):
        launcher = self.snapshot(10, 100, 10, name="runtime")
        private = self.snapshot(20, 200, 90, args=[b"cs2", b"-insecure", b"+playdemo", b"/private/input.dem"])
        foreign = self.snapshot(21, 210, 91, args=[b"cs2", b"-insecure", b"+playdemo", b"/other/input.dem"])
        with patch.object(live, "_process_snapshot", return_value=launcher):
            tracker = live.OwnedProcesses(10, Path("/private/input.dem"))
        with patch.object(live, "_process_snapshots", return_value=[private, foreign]):
            self.assertEqual(tracker.observe(), [private])

    def test_stop_reaps_launcher_and_waits_for_plugin_zombie_check(self):
        from unittest.mock import Mock
        from cs2_mvp.csdm import CsdmError
        class Clock:
            value = 0
            def now(self):
                return self.value
            def sleep(self, duration):
                self.value += duration
        clock, process, tracker = Clock(), Mock(pid=10), Mock()
        zombie = self.snapshot(20, 200, 10, state="Z")
        remaining = [zombie]
        tracker.observe.side_effect = lambda: list(remaining)
        process.poll.side_effect = lambda: remaining.clear()
        checks = [CsdmError("CS2 is running (PID20)"), None, None, None, None, None, None]
        def stopped():
            value = checks.pop(0) if checks else None
            if value:
                raise value
        with patch.object(live, "_process_snapshot", return_value=zombie), \
             patch.object(live.os, "kill") as kill, \
             patch.object(live.time, "monotonic", side_effect=clock.now), \
             patch.object(live.time, "sleep", side_effect=clock.sleep), \
             patch("cs2_mvp.csdm._assert_game_stopped", side_effect=stopped) as check:
            live._stop_owned(process, Path("/private/input.dem"), tracker)
            kill.assert_not_called()
            process.wait.assert_called_once_with(timeout=5)
            self.assertGreaterEqual(check.call_count, 3)
            self.assertGreaterEqual(clock.value, .2)

    def test_foreign_game_blocks_restoration_without_being_signalled(self):
        from unittest.mock import Mock
        from cs2_mvp.csdm import CsdmError
        now = [0.0]
        def sleep(duration):
            now[0] += duration
        process, tracker = Mock(pid=10), Mock()
        tracker.observe.return_value = []
        with patch.object(live.os, "kill") as kill, \
             patch.object(live.time, "monotonic", side_effect=lambda: now[0]), \
             patch.object(live.time, "sleep", side_effect=sleep), \
             patch("cs2_mvp.csdm._assert_game_stopped", side_effect=CsdmError("CS2 is running (PID999)")):
            with self.assertRaisesRegex(live.LiveError, "recovery files retained"):
                live._stop_owned(process, Path("/private/input.dem"), tracker)
            kill.assert_not_called()

    def test_real_snapshot_has_pid_identity(self):
        process = live._process_snapshot(os.getpid())
        self.assertEqual(process["pid"], os.getpid())
        self.assertGreater(process["start"], 0)


class RecoveryTests(unittest.TestCase):
    def create_interrupted(self, root):
        from cs2_mvp.csdm import temporary_plugin
        game = root / "game"
        (game / "csgo").mkdir(parents=True)
        original = b'"GameInfo" { FileSystem { SearchPaths { Game csgo } } }'
        (game / "csgo/gameinfo.gi").write_bytes(original)
        plugin = root / "libserver.so"
        plugin.write_bytes(b"\x7fELF\x02\x01" + bytes(58))
        demo = root / "input.dem"
        demo.write_bytes(b"PBDEMS2\0")
        with patch("cs2_mvp.csdm._assert_game_stopped"), \
             patch("cs2_mvp.csdm.recover_plugin"):
            with temporary_plugin(game, plugin, launch_args=["-insecure", "+playdemo", str(demo)], expected_sha256=None):
                pass  # Deliberately leave only this fixture's transaction pending.
        return game, original

    def test_recover_restores_verified_plugin_and_retains_config_backup(self):
        from contextlib import nullcontext
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game, original = self.create_interrupted(root)
            configs = root / "state/runs/interrupted/original-config/0/fixture.cfg"
            configs.parent.mkdir(parents=True)
            configs.write_text("user configuration backup")
            with patch.object(live, "session_locks", return_value=nullcontext()), \
                 patch("cs2_mvp.csdm._assert_game_stopped"), \
                 patch("cs2_mvp.discovery.find_game", return_value=game), \
                 patch.object(live, "state_root", return_value=root / "state"):
                result = live.recover_session(str(game))
            self.assertEqual(result["status"], "recovered")
            self.assertEqual((game / "csgo/gameinfo.gi").read_bytes(), original)
            self.assertFalse((game / "csgo/gameinfo.gi.cs2-mvp.json").exists())
            self.assertFalse((game / "csgo/csdm/bin/linuxsteamrt64/libserver.so").exists())
            self.assertEqual(configs.read_text(), "user configuration backup")
            self.assertEqual(result["preserved_runs_directory"], str(root / "state/runs"))

    def test_recover_reports_conflict_without_overwriting_gameinfo(self):
        from contextlib import nullcontext
        from cs2_mvp.csdm import RestoreConflict
        with tempfile.TemporaryDirectory() as directory:
            game, _ = self.create_interrupted(Path(directory))
            edited = (game / "csgo/gameinfo.gi").read_bytes() + b"\n// later user edit\n"
            (game / "csgo/gameinfo.gi").write_bytes(edited)
            with patch.object(live, "session_locks", return_value=nullcontext()), \
                 patch("cs2_mvp.csdm._assert_game_stopped"), \
                 patch("cs2_mvp.discovery.find_game", return_value=game):
                with self.assertRaises(RestoreConflict):
                    live.recover_session(str(game))
            self.assertEqual((game / "csgo/gameinfo.gi").read_bytes(), edited)
            self.assertTrue((game / "csgo/gameinfo.gi.cs2-mvp.json").is_file())
            self.assertTrue((game / "csgo/gameinfo.gi.cs2-mvp.backup").is_file())

    def test_running_game_prevents_recovery(self):
        from contextlib import nullcontext
        from cs2_mvp.csdm import CsdmError
        with patch.object(live, "session_locks", return_value=nullcontext()), \
             patch("cs2_mvp.csdm._assert_game_stopped", side_effect=CsdmError("CS2 is running")), \
             patch("cs2_mvp.csdm.recover_plugin") as recover:
            with self.assertRaisesRegex(CsdmError, "CS2 is running"):
                live.recover_session("/game")
            recover.assert_not_called()

    def test_primary_monitor_geometry_wins_over_bounding_desktop(self):
        from unittest.mock import Mock
        display = "Screen 0: minimum 8 x 8, current 5120 x 1440, maximum 32767 x 32767\nDP-1 connected 2560x1440+0+0\nDP-2 connected primary 2560x1440-2560+0\n"
        with patch.object(live.subprocess, "run", return_value=Mock(stdout=display)):
            self.assertEqual(live.desktop_size(), (2560, 1440))


class SharedSocketPathTests(unittest.TestCase):
    def test_socket_is_in_home_backed_state_and_private(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(live, "state_root", return_value=Path(directory) / "state"):
            path = live.control_socket_path()
            self.assertEqual(path, Path(directory) / "state/ipc/control.sock")
            self.assertTrue(path.is_absolute())
            self.assertLessEqual(len(os.fsencode(path)), 107)
            self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)

    def test_socket_parent_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "ipc").symlink_to(root, target_is_directory=True)
            with patch.object(live, "state_root", return_value=root):
                with self.assertRaisesRegex(live.LiveError, "owned directory"):
                    live.control_socket_path()

    def test_long_state_socket_uses_shorter_private_home_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            long = root / ("state" * 20) / ("long" * 20)
            with patch.object(live, "state_root", return_value=long), \
                 patch.object(live.Path, "home", return_value=root):
                path = live.control_socket_path()
                self.assertEqual(path, root / ".cs2-camera-ipc/control.sock")
                self.assertLessEqual(len(os.fsencode(path)), 107)
                self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)


if __name__ == "__main__":
    unittest.main()
