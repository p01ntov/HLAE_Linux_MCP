import concurrent.futures
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

from cs2_mvp import export_presets as presets
from cs2_mvp.export_ui import ExportOptions


class ExportPresetsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="export-presets-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.path = self.root / "config/exports.json"

    def options(self, **changes):
        return ExportOptions(output=self.root / "video.mp4", **changes)

    def record(self, **changes):
        values = dict(project_name="Mirage · Выход", demo=self.root / "Match.dem",
                      finished_at="2026-11-01T12:00:00Z")
        values.update(changes)
        return presets.make_history_record(self.options(), {"error": "No capture"}, **values)

    def test_reusable_settings_use_renderer_limits_and_never_carry_output_or_range(self):
        options = self.options(width=1440, height=2560, capture_fps=1000, fps=60,
                               scope="region", video_bitrate_mbps=12.5, reopen=False)
        settings = presets.settings_from_options(options)
        self.assertNotIn("scope", settings)
        self.assertNotIn("output", settings)
        self.assertEqual(ExportOptions(output=self.root / "next.mp4", **settings).video_bitrate_mbps, 12.5)
        self.assertEqual(presets.settings_from_options({**settings, "output": "ignore", "scope": "region"}), settings)
        self.assertEqual(presets.validate_settings({}), presets.DEFAULT_SETTINGS)
        with self.assertRaises(ValueError):
            presets.validate_settings({"output": str(self.root / "keep.mp4")})
        with self.assertRaises(ValueError):
            presets.validate_settings({"scope": "region"})
        invalid = ({"width": True}, {"width": 1441}, {"height": 239}, {"width": 8192, "height": 8192},
                   {"fps": 59.5}, {"fps": 1001}, {"capture_fps": 119}, {"capture_fps": True},
                   {"fps": 240, "capture_fps": 120}, {"export_format": "webm"},
                   {"audio": 1}, {"reopen": "true"}, {"keep_raw": None}, {"slow_motion": []},
                   {"crf": 52}, {"crf": 18.0}, {"crf": True}, {"video_bitrate_mbps": float("nan")},
                   {"video_bitrate_mbps": float("inf")}, {"video_bitrate_mbps": .09},
                   {"video_bitrate_mbps": 1001}, {"video_bitrate_mbps": 10 ** 400},
                   {"crf": 18, "video_bitrate_mbps": 20},
                   {"export_format": "png", "crf": 18}, {"export_format": "png", "video_bitrate_mbps": 20},
                   {"export_format": "png", "capture_fps": 1000, "fps": 1000})
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                presets.validate_settings(value)
        self.assertEqual(presets.validate_settings({"export_format": "png", "fps": 999, "capture_fps": 1000})["fps"], 999)

    def test_default_and_override_paths_are_absolute_and_reads_have_no_side_effect(self):
        with patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root)}, clear=True):
            self.assertEqual(presets.configuration_path(), self.root / "cs2-camera/exports.json")
            self.assertEqual(presets.load_export_store()["last_used"], None)
        self.assertEqual(list(self.root.iterdir()), [])
        with patch.dict(os.environ, {"CS2_CAMERA_EXPORT_PRESETS": str(self.path)}, clear=True):
            self.assertEqual(presets.configuration_path(), self.path)
        with patch.dict(os.environ, {"CS2_CAMERA_EXPORT_PRESETS": "relative.json"}, clear=True):
            with self.assertRaisesRegex(ValueError, "absolute"):
                presets.configuration_path()
        for operation in (presets.load_export_store, lambda path: presets.save_last_used({}, path)):
            with self.assertRaisesRegex(ValueError, "absolute"):
                operation("relative.json")

    def test_private_atomic_roundtrip_and_restart(self):
        settings = presets.settings_from_options(self.options(width=1440, height=2560, crf=23))
        presets.save_preset("Portrait", settings, self.path)
        presets.save_last_used({"export_format": "png", "capture_fps": 1000}, self.path)
        presets.append_history(self.record(), self.path)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.path.parent.stat().st_mode & 0o777, 0o700)
        loaded = presets.load_export_store(self.path)
        self.assertEqual(loaded["presets"], [{"name": "Portrait", "settings": settings}])
        self.assertEqual(loaded["last_used"]["export_format"], "png")
        self.assertEqual(loaded["history"][0]["status"], "failed")
        result = subprocess.run([sys.executable, "-c",
            "import json,sys; from cs2_mvp.export_presets import load_export_store; print(json.dumps(load_export_store(sys.argv[1])))",
            str(self.path)], text=True, capture_output=True, timeout=10, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), loaded)
        self.assertEqual(list(self.path.parent.glob(".exports-*.tmp")), [])
        loaded["presets"][0]["settings"]["width"] = 320
        self.assertEqual(presets.load_export_store(self.path)["presets"][0]["settings"]["width"], 1440)
        self.assertFalse((self.root / "video.mp4").exists())

    def test_presets_are_bounded_and_named_replacement_preserves_other_data(self):
        for index in range(presets.MAX_PRESETS):
            presets.save_preset(f"Preset {index}", {}, self.path)
        original = self.path.read_bytes()
        with self.assertRaisesRegex(ValueError, "delete"):
            presets.save_preset("Another", {}, self.path)
        self.assertEqual(self.path.read_bytes(), original)
        changed = presets.save_preset("  PRESET 0  ", {"crf": 27}, self.path)
        self.assertEqual(len(changed["presets"]), 16)
        self.assertEqual(changed["presets"][0]["name"], "PRESET 0")
        self.assertEqual(changed["presets"][0]["settings"]["crf"], 27)
        self.assertEqual(len(presets.delete_preset("preset 0", self.path)["presets"]), 15)
        self.assertEqual(len(presets.save_preset("Шот ①", {}, self.path)["presets"]), 16)
        for name in (None, "", "   ", "bad\nname", "a" * 257):
            with self.subTest(name=name), self.assertRaises(ValueError):
                presets.save_preset(name, {}, self.path)

    def test_history_success_requires_done_and_errors_or_cancellation_take_priority(self):
        output = self.root / "video.mp4"
        output.write_bytes(b"existing published file")
        done = {"status": "done", "output": str(output), "start_tick": 1000, "end_tick": 1064,
                "expected_duration": 1, "expected_output_duration": 1,
                "media": {"frames": 60, "duration": 1}}
        outcome = {"plan": done, "error": None, "cancelled": False,
                   "snapshot": {"path": str(self.root / "snapshot.json")}}
        record = presets.make_history_record(self.options(), outcome, project_name="Take 1",
                    started_at="2026-11-01T14:59:55+03:00", finished_at="2026-11-01T12:00:00Z", elapsed_seconds=5)
        self.assertEqual(record["status"], "success")
        self.assertEqual(record["started_at"], "2026-11-01T11:59:55.000000Z")
        self.assertEqual(record["frames"], 60)
        self.assertEqual(record["snapshot"], str(self.root / "snapshot.json"))
        for changes, expected in (({"error": "Publication failed"}, "failed"),
                                  ({"cancelled": True}, "cancelled"),
                                  ({"plan": {**done, "error": "Missing samples"}}, "failed"),
                                  ({"plan": {**done, "status": "encoding"}}, "failed"),
                                  ({"plan": {"output": str(output)}}, "failed"),
                                  ({"plan": None}, "failed")):
            with self.subTest(changes=changes):
                failed = presets.make_history_record(self.options(), {**outcome, **changes})
                self.assertEqual(failed["status"], expected)
                self.assertIsNone(failed["frames"])
                self.assertIsNone(failed["output_duration"])
                self.assertTrue(failed["error"])
                presets.append_history(failed, self.path)
        self.assertEqual(output.read_bytes(), b"existing published file")

    def test_confirmed_preset_version_is_checked_inside_the_write_lock(self):
        document = presets.save_preset("Master", {"crf": 15}, self.path, expected=None)
        prior = document["presets"][0]
        before = self.path.read_bytes()
        with self.assertRaises(presets.PresetConflictError):
            presets.save_preset("MASTER", {"crf": 25}, self.path, expected=None)
        self.assertEqual(self.path.read_bytes(), before)
        current = presets.save_preset("Master", {"crf": 25}, self.path, expected=prior)
        before = self.path.read_bytes()
        for operation in (lambda: presets.save_preset("Master", {"crf": 35}, self.path, expected=prior),
                          lambda: presets.delete_preset("Master", self.path, expected=prior)):
            with self.assertRaises(presets.PresetConflictError):
                operation()
            self.assertEqual(self.path.read_bytes(), before)
        presets.delete_preset("Master", self.path, expected=current["presets"][0])
        self.assertEqual(presets.load_export_store(self.path)["presets"], [])

    def test_history_is_bounded_ordered_and_repeated_completion_is_idempotent(self):
        presets.save_last_used({"crf": 21}, self.path)
        presets.save_preset("Keep", {}, self.path)
        start = datetime(2026, 11, 1, tzinfo=timezone.utc)
        for index in range(35):
            record = self.record(finished_at=(start + timedelta(minutes=index)).isoformat(), project_name=f"Take {index}")
            presets.append_history(record, self.path)
        store = presets.append_history(record, self.path)
        self.assertEqual(len(store["history"]), 32)
        self.assertEqual(store["history"][0]["project_name"], "Take 34")
        self.assertEqual(store["history"][-1]["project_name"], "Take 3")
        self.assertEqual(store["last_used"]["crf"], 21)
        self.assertEqual(store["presets"][0]["name"], "Keep")
        with self.assertRaisesRegex(ValueError, "rewritten"):
            presets.append_history({**record, "project_name": "Altered"}, self.path)
        late = self.record(finished_at=(start + timedelta(minutes=33, seconds=30)).isoformat(), project_name="Late callback")
        store = presets.append_history(late, self.path)
        self.assertEqual(store["history"][1]["project_name"], "Late callback")

    def test_invalid_history_is_rejected_without_saving(self):
        record = self.record()
        for field, value in (("status", "done"), ("status", True), ("id", "bad"),
                             ("finished_at", "2026-11-01"), ("finished_at", "nonsense"),
                             ("started_at", "2026-11-02T12:00:00Z"),
                             ("elapsed_seconds", -1), ("elapsed_seconds", float("nan")),
                             ("elapsed_seconds", 10 ** 400),
                             ("elapsed_seconds", True), ("output", "relative.mp4"),
                             ("demo", "relative.dem"), ("snapshot", "relative.json"),
                             ("project_name", "bad\0name"), ("start_tick", 1000),
                             ("frames", 60), ("output_duration", 1), ("settings", {"scope": "region"})):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                presets.append_history({**record, field: value}, self.path)
        with self.assertRaisesRegex(ValueError, "failed"):
            presets.append_history({**record, "status": "success"}, self.path)
        self.assertFalse(self.path.exists())

    def test_malformed_or_newer_store_is_preserved_by_every_mutation(self):
        empty = presets.load_export_store(self.path)
        invalid = (b"{broken", b"\xff", json.dumps({**empty, "version": 2}).encode(),
                   json.dumps({**empty, "version": True}).encode(),
                   json.dumps({**empty, "unknown": 1}).encode(),
                   json.dumps({**empty, "last_used": {"reopen": 1}}).encode(),
                   json.dumps({**empty, "presets": [{"name": "A", "settings": {}}, {"name": "a", "settings": {}}]}).encode(),
                   b'{"version":1,"version":1,"last_used":null,"presets":[],"history":[]}',
                   b'{"version":1,"last_used":{"crf":NaN},"presets":[],"history":[]}',
                   b"[" * 1200 + b"]" * 1200,
                   b"x" * (presets.MAX_STORE_BYTES + 1))
        self.path.parent.mkdir()
        for raw in invalid:
            self.path.write_bytes(raw)
            for operation in (lambda: presets.load_export_store(self.path),
                              lambda: presets.save_last_used({}, self.path),
                              lambda: presets.save_preset("Take", {}, self.path),
                              lambda: presets.delete_preset("Take", self.path),
                              lambda: presets.append_history(self.record(), self.path)):
                with self.subTest(raw=raw[:80]), self.assertRaises(ValueError):
                    operation()
                self.assertEqual(self.path.read_bytes(), raw)

    def test_symlink_directory_fifo_and_lock_symlink_are_not_followed(self):
        self.path.parent.mkdir()
        target = self.root / "untouched.json"
        target.write_text("keep me")
        self.path.symlink_to(target)
        for operation in (lambda: presets.load_export_store(self.path), lambda: presets.save_last_used({}, self.path)):
            with self.assertRaises(ValueError):
                operation()
        self.assertEqual(target.read_text(), "keep me")
        self.path.unlink()
        self.path.mkdir()
        with self.assertRaises((ValueError, OSError)):
            presets.load_export_store(self.path)
        self.path.rmdir()
        os.mkfifo(self.path)
        with self.assertRaises(ValueError):
            presets.load_export_store(self.path)
        self.path.unlink()
        lock = self.path.with_name("." + self.path.name + ".lock")
        lock.unlink(missing_ok=True)
        lock.symlink_to(target)
        with self.assertRaises(ValueError):
            presets.save_last_used({}, self.path)
        self.assertEqual(target.read_text(), "keep me")
        self.assertFalse(self.path.exists())

    def test_write_failures_preserve_previous_store_and_remove_temporary_files(self):
        presets.save_last_used({"crf": 19}, self.path)
        original = self.path.read_bytes()
        for name in ("os.replace", "os.fsync"):
            with patch("cs2_mvp.export_presets." + name, side_effect=OSError("fixture disk failure")):
                with self.assertRaisesRegex(OSError, "fixture disk failure"):
                    presets.save_last_used({"crf": 28}, self.path)
            self.assertEqual(self.path.read_bytes(), original)
            self.assertEqual(list(self.path.parent.glob(".exports-*.tmp")), [])

    def test_external_edit_or_symlink_swap_during_write_is_preserved(self):
        presets.save_last_used({}, self.path)
        native_fsync = os.fsync
        changed = False

        def concurrent_edit(descriptor):
            nonlocal changed
            if not changed:
                changed = True
                self.path.write_text("External edit: keep this")
            native_fsync(descriptor)

        with patch("cs2_mvp.export_presets.os.fsync", side_effect=concurrent_edit):
            with self.assertRaisesRegex(ValueError, "changed"):
                presets.save_last_used({"crf": 28}, self.path)
        self.assertEqual(self.path.read_text(), "External edit: keep this")
        self.path.unlink()
        presets.save_last_used({}, self.path)
        target = self.root / "target.json"
        target.write_text("Untouched target")
        changed = False

        def swap(descriptor):
            nonlocal changed
            if not changed:
                changed = True
                self.path.unlink()
                self.path.symlink_to(target)
            native_fsync(descriptor)

        with patch("cs2_mvp.export_presets.os.fsync", side_effect=swap):
            with self.assertRaisesRegex(ValueError, "changed"):
                presets.save_last_used({}, self.path)
        self.assertTrue(self.path.is_symlink())
        self.assertEqual(target.read_text(), "Untouched target")
        self.assertEqual(list(self.path.parent.glob(".exports-*.tmp")), [])

    def test_concurrent_launchers_do_not_lose_each_others_presets_or_history(self):
        barrier = threading.Barrier(8)

        def save(index):
            barrier.wait(timeout=5)
            presets.save_preset(f"Take {index}", {"crf": index}, self.path)
            presets.append_history(self.record(project_name=f"Take {index}"), self.path)

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(save, range(8)))
        store = presets.load_export_store(self.path)
        self.assertEqual({item["name"] for item in store["presets"]}, {f"Take {index}" for index in range(8)})
        self.assertEqual({item["project_name"] for item in store["history"]}, {f"Take {index}" for index in range(8)})


if __name__ == "__main__":
    unittest.main()
