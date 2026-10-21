import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import importlib.util
from types import SimpleNamespace
from unittest.mock import patch
import wave

from cs2_mvp.export_ui import ExportOptions, describe_export, perform_export, restore_export_snapshot, save_export_snapshot
from cs2_mvp.render import RenderCancelled, _run_process, finish_capture


PROJECT = {"name": "Test", "start_tick": 1000, "duration": .125,
           "keys": [{"id": "1", "time": 0, "position": [0,0,64], "angles": [0,0,0], "fov": 90}]}


class Client:
    def __init__(self):
        self.calls = []
        self.appearance = {"active": False}
        self.project = PROJECT
        self.tick = 1003
        self.paused = True
        self.cursor = .05

    def request(self, method, params=None):
        self.calls.append((method, params))
        if method == "get_state":
            return {"tick": self.tick, "paused": self.paused, "cursor": self.cursor, "camera": {"position": [0,0,64], "angles": [0,0,0], "fov": 90}, "appearance": self.appearance}
        if method == "demo_control":
            if params["action"] == "seek":
                self.tick = params["tick"]
            elif params["action"] == "pause":
                self.paused = True
        return json.loads(json.dumps(self.project)) if method == "export_project" else {}


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.stable_patch = patch("cs2_mvp.export_ui._RESTORE_STABLE_SECONDS", 0)
        self.stable_patch.start()
        self.addCleanup(self.stable_patch.stop)

    @unittest.skipUnless(importlib.util.find_spec("PySide6"), "Qt runtime required")
    def test_offscreen_launcher_automatically_lists_selects_and_refreshes_downloaded_demos(self):
        result=subprocess.run([sys.executable,str(Path(__file__).with_name("gui_demo_library_probe.py"))],
            capture_output=True,text=True,env=dict(os.environ,QT_QPA_PLATFORM="offscreen"),timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("DEMO_LIBRARY_GUI_PASS",result.stdout)

    @unittest.skipUnless(importlib.util.find_spec("PySide6"), "Qt runtime required")
    def test_offscreen_launcher_native_export_request_capture_and_reopen(self):
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("gui_export_probe.py"))],
                                capture_output=True, text=True, env=environment, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("EXPORT_GUI_LIFECYCLE_PASS", result.stdout)

    def test_slow_conform_duration_counts_every_native_sample(self):
        options = ExportOptions(Path("new.mp4"), capture_fps=1000, fps=60, slow_motion=True)
        info = describe_export(PROJECT, options)
        self.assertEqual((info["samples"], info["frames"]), (125,125))
        self.assertAlmostEqual(info["output_duration"], 125/60)
        self.assertEqual(info["speed"], .06)

    def test_job_saves_before_stop_waits_for_restoration_then_renders(self):
        with tempfile.TemporaryDirectory() as directory:
            root, client, order = Path(directory), Client(), []
            def stop():
                self.assertTrue((root / "saved/camera-project.json").exists())
                order.append("stop")
            def wait():
                self.assertEqual(order, ["stop"])
                order.append("restored")
            def render(*args, **kwargs):
                self.assertEqual(order, ["stop", "restored"])
                self.assertEqual(kwargs["camera"], root / "saved/camera-project.json")
                self.assertTrue(kwargs["slow_motion"])
                self.assertFalse(kwargs["audio"])
                order.append("render")
                return {"output": str(kwargs["output"])}
            options = ExportOptions(root / "output.mp4", capture_fps=1000, slow_motion=True)
            outcome = perform_export(options, demo="match.dem", game_dir=None, client=client,
                                     stop_session=stop, wait_session=wait, render=render, directory=root / "saved")
            self.assertIsNone(outcome["error"])
            self.assertEqual(order, ["stop", "restored", "render"])
            self.assertEqual(oct((root / "saved/camera-project.json").stat().st_mode & 0o777), "0o600")

    def test_invalid_output_or_unverified_appearance_keeps_live_session_open(self):
        with tempfile.TemporaryDirectory() as directory:
            root, client, stops = Path(directory), Client(), []
            existing = root / "existing.mp4"
            existing.touch()
            outcome = perform_export(ExportOptions(existing), demo="match.dem", game_dir=None, client=client,
                                     stop_session=lambda: stops.append(True), wait_session=lambda: None,
                                     render=lambda *a,**kw: self.fail("No capture"), directory=root / "first")
            self.assertIn("new .mp4", outcome["error"])
            client.appearance = {"active":True,"mounted_verified":True,"reload_requested":True,"render_verified":False,
                                 "path":"/private/pack","models":["agents/models/a.vmdl"]}
            outcome = perform_export(ExportOptions(root / "new.mp4"), demo="match.dem", game_dir=None, client=client,
                                     stop_session=lambda: stops.append(True), wait_session=lambda: None,
                                     render=lambda *a,**kw: self.fail("No capture"), directory=root / "second")
            self.assertIn("experimental", outcome["error"])
            self.assertEqual(stops, [])
            self.assertEqual(outcome["snapshot"]["appearance"]["path"], "/private/pack")

    def test_restoration_failure_prevents_render_and_preserves_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def wait():
                raise RuntimeError("Owned game still running")
            outcome = perform_export(ExportOptions(root / "out.mp4"), demo="match.dem", game_dir=None, client=Client(),
                                     stop_session=lambda: None, wait_session=wait,
                                     render=lambda *a,**kw: self.fail("No capture"), directory=root / "saved")
            self.assertTrue(outcome["stopped"])
            self.assertIn("still running", outcome["error"])
            self.assertTrue(Path(outcome["snapshot"]["path"]).exists())

    def test_cosmetics_require_capture_adapter_before_stopping_live_session(self):
        with tempfile.TemporaryDirectory() as directory:
            root,client,stops=Path(directory),Client(),[]
            cosmetics={"players":[{"steam_id":"76561198000000000","nickname":"Player","agent_item_def":5036}]}
            client.project={**PROJECT,"cosmetics":cosmetics}
            with patch("cs2_mvp.camera_backend.verify_cosmetics_capture_support",side_effect=RuntimeError("Missing cosmetic readback and restoration adapter")):
                outcome=perform_export(ExportOptions(root/"new.mp4"),demo="match.dem",game_dir=None,client=client,
                    stop_session=lambda:stops.append(True),wait_session=lambda:None,
                    render=lambda *a,**kw:self.fail("No capture"),directory=root/"saved")
            self.assertIn("readback and restoration",outcome["error"])
            self.assertEqual(stops,[])
            self.assertEqual(outcome["snapshot"]["project"]["cosmetics"],cosmetics)
            self.assertEqual(json.loads(Path(outcome["snapshot"]["path"]).read_text())["cosmetics"],cosmetics)

    def test_unwired_player_hud_capture_retains_request_before_closing_session(self):
        with tempfile.TemporaryDirectory() as directory:
            root,client,stops=Path(directory),Client(),[]
            presentation={"view":"player","hud":"gameplay"}
            client.project={**PROJECT,"presentation":presentation}
            outcome=perform_export(ExportOptions(root/"new.mp4"),demo="match.dem",game_dir=None,client=client,
                stop_session=lambda:stops.append(True),wait_session=lambda:None,
                render=lambda *a,**kw:self.fail("No capture"),directory=root/"saved")
            self.assertIn("offline presentation adapter",outcome["error"])
            self.assertEqual(stops,[])
            self.assertEqual(outcome["snapshot"]["project"]["presentation"],presentation)

    def test_unverified_glove_capture_preserves_saved_request_and_live_session(self):
        with tempfile.TemporaryDirectory() as directory:
            root,client,stops=Path(directory),Client(),[]
            cosmetics={"gloves":[{"steam_id":"76561198000000000","item_def":5030,"paint_kit":10006}]}
            client.project={**PROJECT,"cosmetics":cosmetics}
            with patch("cs2_mvp.camera_backend.verify_cosmetics_capture_support",side_effect=RuntimeError("Missing glove pose and render publication adapter")) as support:
                outcome=perform_export(ExportOptions(root/"new.mp4"),demo="match.dem",game_dir=None,client=client,
                    stop_session=lambda:stops.append(True),wait_session=lambda:None,
                    render=lambda *a,**kw:self.fail("Gloves must fail before capture"),directory=root/"saved")
            support.assert_called_once_with(require_gloves=True)
            self.assertIn("glove pose and render publication",outcome["error"])
            self.assertEqual(stops,[])
            self.assertEqual(outcome["snapshot"]["project"]["cosmetics"],cosmetics)
            self.assertEqual(json.loads(Path(outcome["snapshot"]["path"]).read_text())["cosmetics"],cosmetics)

    def test_reopen_restores_project_scene_and_appearance_as_explicit_session_adapter(self):
        client = Client()
        project = {**PROJECT, "scene": {"hud_visible":False}, "appearance": {"path":"/private/pack","models":["agents/models/a.vmdl"]}}
        snapshot = {"project":project,"appearance":project["appearance"],"cursor":.05,"camera":{"fov":80}}
        restore_export_snapshot(client,snapshot)
        self.assertEqual([method for method,_ in client.calls], ["import_project","apply_appearance_pack","scrub","set_camera"])
        self.assertNotIn("appearance",client.calls[0][1]["project"])
        self.assertEqual(client.calls[0][1]["project"]["scene"],{"hud_visible":False})
        self.assertEqual(client.calls[2], ("scrub", {"time":.05,"seek_demo":True}))

    def test_snapshot_reopens_actual_demo_tick_independently_of_camera_cursor(self):
        with tempfile.TemporaryDirectory() as directory:
            client = Client()
            client.project = {**PROJECT, "start_tick": 0}
            client.tick, client.cursor = 11771, 0
            snapshot = save_export_snapshot(client, Path(directory) / "saved")
        self.assertEqual(snapshot["demo_tick"], 11771)
        reopened = Client()
        restore_export_snapshot(reopened, snapshot)
        self.assertEqual(reopened.calls, [
            ("import_project", {"project": snapshot["project"]}),
            ("scrub", {"time": 0, "seek_demo": False}),
            ("demo_control", {"action": "pause"}),
            ("demo_control", {"action": "seek", "tick": 11771}),
            ("get_state", None),
            ("set_camera", snapshot["camera"]),
        ])

    def test_demo_tick_bounds_and_types_are_checked_before_snapshot_or_restore(self):
        for tick in (True, False, 11771.0, "11771", None, -1, 2000000001):
            with self.subTest(tick=tick), tempfile.TemporaryDirectory() as directory:
                client = Client()
                client.tick = tick
                destination = Path(directory) / "saved"
                with self.assertRaisesRegex(ValueError, "Demo tick"):
                    save_export_snapshot(client, destination)
                self.assertFalse(destination.exists())
                client.calls.clear()
                with self.assertRaisesRegex(ValueError, "Demo tick"):
                    restore_export_snapshot(client, {"project": PROJECT, "demo_tick": tick})
                self.assertEqual(client.calls, [])

    def test_reopen_empty_project_restores_paused_demo_and_live_camera(self):
        client = Client()
        snapshot = {"project": {**PROJECT, "keys": []}, "demo_tick": 11779,
                    "cursor": 0, "camera": {"position": [547, -1719, -42], "fov": 50}}
        original_request = client.request
        def request(method, params=None):
            if method == "scrub":
                raise RuntimeError("add a camera key before preview")
            return original_request(method, params)
        client.request = request
        restore_export_snapshot(client, snapshot)
        self.assertEqual(client.calls, [
            ("import_project", {"project": snapshot["project"]}),
            ("demo_control", {"action": "pause"}),
            ("demo_control", {"action": "seek", "tick": 11779}),
            ("get_state", None),
            ("set_camera", snapshot["camera"]),
        ])

    def test_reopen_waits_through_late_observer_rewind_before_restoring_camera(self):
        clock = [0.0]
        client = Client()
        original_request = client.request
        seeks = []
        def request(method, params=None):
            if method == "demo_control" and params["action"] == "seek":
                seeks.append(clock[0])
            result = original_request(method, params)
            if method == "get_state":
                # Mode readback becomes ready before the engine's deferred
                # startup resync rewinds one tick and resumes playback.
                result["presentation"] = {"view": "free", "pending": clock[0] < .2}
                if 2 <= clock[0] < 2.2:
                    result.update(tick=11778, paused=False)
            return result
        client.request = request
        fake_time = SimpleNamespace(monotonic=lambda: clock[0], sleep=lambda seconds: clock.__setitem__(0, clock[0]+seconds))
        snapshot = {"project": {**PROJECT, "presentation": {"view": "free", "hud": "hidden"}},
                    "demo_tick": 11779, "camera": {"fov": 50}}
        with patch("cs2_mvp.export_ui.time", fake_time), patch("cs2_mvp.export_ui._RESTORE_STABLE_SECONDS", 2.5):
            restore_export_snapshot(client, snapshot)
        self.assertEqual(len(seeks), 2)
        self.assertGreaterEqual(clock[0], 4.7)
        self.assertEqual(client.calls[-1], ("set_camera", {"fov": 50}))

    def test_reopen_reports_unreachable_tick_with_a_bounded_wait(self):
        clock = [0.0]
        client = Client()
        original_request = client.request
        def request(method, params=None):
            result = original_request(method, params)
            if method == "get_state":
                result.update(tick=1, paused=False)
            return result
        client.request = request
        fake_time = SimpleNamespace(monotonic=lambda: clock[0], sleep=lambda seconds: clock.__setitem__(0, clock[0]+seconds))
        with patch("cs2_mvp.export_ui.time", fake_time), self.assertRaisesRegex(Exception, "saved tick 11779"):
            restore_export_snapshot(client, {"project": PROJECT, "demo_tick": 11779})
        self.assertLessEqual(clock[0], 15.1)
        self.assertEqual(sum(method == "demo_control" and params["action"] == "seek" for method, params in client.calls), 1)

    def test_demo_tick_bounds_are_inclusive(self):
        for tick in (0, 2000000000):
            with self.subTest(tick=tick), tempfile.TemporaryDirectory() as directory:
                client = Client()
                client.tick = tick
                snapshot = save_export_snapshot(client, Path(directory) / "saved")
                client.calls.clear()
                restore_export_snapshot(client, snapshot)
                self.assertIn(("demo_control", {"action": "seek", "tick": tick}), client.calls)

    def test_cancel_stops_only_its_owned_media_process_and_reaps_it(self):
        event = threading.Event()
        timer = threading.Timer(.15, event.set)
        timer.start()
        began = time.monotonic()
        try:
            with self.assertRaises(RenderCancelled):
                _run_process([sys.executable,"-c","import time; time.sleep(30)"], cancel_event=event)
        finally:
            timer.join()
        self.assertLess(time.monotonic()-began,3)

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "Media tools required")
    def test_actual_encoder_conforms_125_samples_to_60fps_without_dropping_or_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            run=root/".runs/probe"
            run.mkdir(parents=True)
            images=root/"game/csgo/csdm/movie"
            images.mkdir(parents=True)
            header=bytearray(18);header[2]=2;header[12:16]=struct.pack("<HH",16,16);header[16:18]=bytes([24,32])
            for i in range(125):
                (images/f"slow{i:08d}.tga").write_bytes(header+bytes([i,100,200])*256)
            with wave.open(str(images/"slow.wav"),"wb") as wav:
                wav.setparams((1,2,48000,0,"NONE","uncompressed"));wav.writeframes(bytes(12000))
            (run/"plugin.log").write_text("[936] Executing: host_framerate 1000\n[1000] Executing: startmovie slow\n[1008] Executing: endmovie\n")
            plan={"environment":{"game_directory":str(root/"game")},"movie_prefix":"slow","fps":60,"capture_fps":1000,
                  "expected_duration":.125,"start_tick":1000,"end_tick":1008,"width":16,"height":16,"audio":False,
                  "retiming":{"mode":"slow_motion"},"output":str(root/"out.mp4")}
            (run/"manifest.json").write_text(json.dumps(plan))
            with patch("cs2_mvp.render.ROOT",root):
                result=finish_capture(run,keep_raw=True,timeout=20)
            self.assertEqual(result["media"]["frames"],125)
            self.assertEqual(result["media"]["fps"],"60/1")
            self.assertFalse(result["media"]["audio"])
            self.assertAlmostEqual(result["media"]["duration"],125/60,delta=.001)
            self.assertEqual(result["retiming"]["encoded_playback_speed"],.06)


if __name__ == "__main__":
    unittest.main()
