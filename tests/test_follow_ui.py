import copy
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from cs2_mvp import follow_ui


STEAM = "76561198836929612"


def draft_for(path):
    info = path.stat()
    return {"project": {"name": "Follow draft", "start_tick": 640, "duration": 1,
                         "interpolation": "linear", "rotation_interpolation": "slerp", "fov_interpolation": "linear",
                         "keys": [{"id": "key", "time": 0, "position": [0, 0, 64], "angles": [0, 0, 0], "fov": 90}]},
            "evidence": {"adapter": "recorded_demo_follow", "status": "draft_unverified", "geometry_checked": False,
                         "visual_verified": False, "loss": None, "requires_geometry_guard_before_apply": True,
                         "source": {"demo": str(path), "size": info.st_size, "mtime_ns": info.st_mtime_ns,
                                    "file_identity": follow_ui._fingerprint_evidence(follow_ui._fingerprint(path)),
                                    "steamid": STEAM, "name": "L201742", "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}}}


class Frame:
    def __init__(self, rows):
        self.rows = rows

    def __len__(self):
        return len(self.rows)

    def to_dict(self, orient):
        return copy.deepcopy(self.rows)


class FollowStorageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.demo = self.root/"recording.dem"
        self.demo.write_bytes(b"PBDEMS2\0test")
        self.library = self.root/"projects"
        self.library.mkdir()
        self.draft = draft_for(self.demo)

    def test_roster_targets_exact_ids_even_with_duplicate_names_and_skips_bots(self):
        class Parser:
            def parse_player_info(self):
                return Frame([{"steamid": int(STEAM), "name": "same"}, {"steamid": int(STEAM)+1, "name": "same"},
                              {"steamid": 0, "name": "bot"}, {"steamid": float(STEAM), "name": "lossy"}])
        with patch.object(follow_ui, "parser", return_value=Parser()):
            players = follow_ui.load_follow_players(self.demo)
        self.assertEqual([player["steamid"] for player in players], [STEAM, str(int(STEAM)+1)])

    def test_duplicate_roster_ids_reject_selection(self):
        class Parser:
            def parse_player_info(self):
                return Frame([{"steamid": int(STEAM), "name": "one"}, {"steamid": int(STEAM), "name": "two"}])
        with patch.object(follow_ui, "parser", return_value=Parser()), self.assertRaisesRegex(ValueError, "duplicate SteamID"):
            follow_ui.load_follow_players(self.demo)

    def test_preparation_fingerprints_but_never_saves_or_calls_game(self):
        baked = copy.deepcopy(self.draft)
        baked["evidence"]["source"].pop("sha256")
        with patch.object(follow_ui, "bake_demo_follow", return_value=baked) as baker:
            result = follow_ui.prepare_follow_draft(self.demo, STEAM, 640, 704, {"fov": 90})
        baker.assert_called_once_with(self.demo, STEAM, 640, 704, {"fov": 90})
        self.assertEqual(result["evidence"]["source"]["sha256"], hashlib.sha256(self.demo.read_bytes()).hexdigest())
        self.assertFalse(list(self.library.iterdir()))

    def test_save_rejects_replacement_with_identical_size_and_mtime(self):
        before = self.demo.stat()
        replacement = self.root/"replacement.dem"
        replacement.write_bytes(b"X"*before.st_size)
        os.utime(replacement, ns=(before.st_atime_ns, before.st_mtime_ns))
        replacement.replace(self.demo)
        self.assertEqual((self.demo.stat().st_size, self.demo.stat().st_mtime_ns),
                         (before.st_size, before.st_mtime_ns))
        with self.assertRaisesRegex(ValueError, "changed since"):
            follow_ui.save_follow_draft(self.draft, self.library)
        self.assertFalse(list(self.library.iterdir()))

    def test_save_rehashes_content_even_when_current_identity_matches_evidence(self):
        before = self.demo.stat()
        self.demo.write_bytes(b"Y"*before.st_size)
        os.utime(self.demo, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.draft["evidence"]["source"]["file_identity"] = follow_ui._fingerprint_evidence(follow_ui._fingerprint(self.demo))
        with self.assertRaisesRegex(ValueError, "content changed since"):
            follow_ui.save_follow_draft(self.draft, self.library)
        self.assertFalse(list(self.library.iterdir()))

    def test_changes_during_hashing_reject_mixed_recording(self):
        original_hash = hashlib.sha256
        for replace in (False, True):
            with self.subTest(replace=replace):
                draft = draft_for(self.demo)
                before = self.demo.stat()
                unchanged_metadata = follow_ui._fingerprint(self.demo)
                class ChangingDigest:
                    def __init__(inner):
                        inner.digest = original_hash()

                    def update(inner, block):
                        inner.digest.update(block)
                        if replace:
                            target = self.root/"replacement.dem"
                            target.write_bytes(b"Z"*before.st_size)
                            os.utime(target, ns=(before.st_atime_ns, before.st_mtime_ns))
                            target.replace(self.demo)
                        else:
                            self.demo.write_bytes(b"W"*before.st_size)
                            os.utime(self.demo, ns=(before.st_atime_ns, before.st_mtime_ns))

                    def hexdigest(inner):
                        return inner.digest.hexdigest()

                with patch.object(follow_ui.hashlib, "sha256", ChangingDigest), \
                     patch.object(follow_ui, "_fingerprint", return_value=unchanged_metadata), \
                     patch.object(follow_ui, "_stat_fingerprint", return_value=unchanged_metadata), \
                     patch.object(follow_ui, "bake_demo_follow", return_value=draft), \
                     self.assertRaisesRegex(ValueError, "changed while fingerprinting"):
                    follow_ui.prepare_follow_draft(self.demo, STEAM, 640, 704)
                self.assertFalse(list(self.library.iterdir()))

    def test_preparation_detects_content_change_during_bake_even_with_unchanged_metadata(self):
        unchanged_metadata = follow_ui._fingerprint(self.demo)
        before = self.demo.stat()
        def changed(*args, **kwargs):
            self.demo.write_bytes(b"B"*before.st_size)
            os.utime(self.demo, ns=(before.st_atime_ns, before.st_mtime_ns))
            return self.draft
        # Some mounted filesystems expose coarse or cached change timestamps.
        # Content checks must work even when every stat field stays the same.
        with patch.object(follow_ui, "_fingerprint", return_value=unchanged_metadata), \
             patch.object(follow_ui, "_stat_fingerprint", return_value=unchanged_metadata), \
             patch.object(follow_ui, "bake_demo_follow", side_effect=changed), \
             self.assertRaisesRegex(ValueError, "changed while fingerprinting"):
            follow_ui.prepare_follow_draft(self.demo, STEAM, 640, 704)
        self.assertFalse(list(self.library.iterdir()))

    def test_change_during_publication_rolls_back_all_sidecars(self):
        original = os.link
        def replace_after_publish(*args, **kwargs):
            result = original(*args, **kwargs)
            self.demo.write_bytes(b"changed after publication")
            return result
        with patch.object(follow_ui.os, "link", replace_after_publish), self.assertRaisesRegex(ValueError, "changed while saving"):
            follow_ui.save_follow_draft(self.draft, self.library, project_id="follow-changed")
        self.assertFalse(list(self.library.iterdir()))

    def test_preparation_detects_demo_replacement_and_early_cancellation(self):
        original = copy.deepcopy(self.draft)
        self.demo.write_bytes(b"changed")
        with patch.object(follow_ui, "bake_demo_follow", return_value=original), self.assertRaisesRegex(ValueError, "changed after baking"):
            follow_ui.prepare_follow_draft(self.demo, STEAM, 640, 704)
        cancelled = threading.Event()
        cancelled.set()
        with patch.object(follow_ui, "bake_demo_follow") as baker, self.assertRaises(InterruptedError):
            follow_ui.prepare_follow_draft(self.demo, STEAM, 640, 704, cancel_event=cancelled)
        baker.assert_not_called()

    def test_save_is_portable_and_metadata_matches_native_project_store(self):
        result = follow_ui.save_follow_draft(self.draft, self.library, project_id="follow-test")
        self.assertEqual(json.loads(Path(result["project"]).read_text()), self.draft["project"])
        meta = json.loads(Path(result["metadata"]).read_text())
        self.assertEqual((meta["version"], meta["id"], meta["opened_at_ns"]), (1, "follow-test", 0))
        self.assertEqual(meta["demo"]["fingerprint"]["sha256"], hashlib.sha256(self.demo.read_bytes()).hexdigest())
        self.assertEqual(meta["demo"]["fingerprint"]["bytes"], self.demo.stat().st_size)
        self.assertEqual(json.loads(Path(result["evidence"]).read_text())["status"], "draft_unverified")
        self.assertEqual(len(list(self.library.glob("*.json"))), 1)
        for key in ("project", "metadata", "evidence"):
            self.assertEqual(Path(result[key]).stat().st_mode & 0o777, 0o600)
        self.assertEqual(set(p.name for p in self.library.iterdir()), {"follow-test.json", "follow-test.meta", "follow-test.evidence"})

    def test_save_never_overwrites_any_existing_file(self):
        for suffix in ("json", "meta", "evidence"):
            with self.subTest(suffix=suffix):
                path = self.library/("follow-test."+suffix)
                path.write_text("original")
                with self.assertRaises(FileExistsError):
                    follow_ui.save_follow_draft(self.draft, self.library, project_id="follow-test")
                self.assertEqual(path.read_text(), "original")
                self.assertEqual(list(self.library.iterdir()), [path])
                path.unlink()

    def test_save_refuses_symlink_destination_and_rolls_back_sidecars(self):
        target = self.root/"untouched.json"
        target.write_text("untouched")
        destination = self.library/"follow-test.json"
        try:
            destination.symlink_to(target)
        except OSError as error:
            unsupported = {errno.EPERM, errno.EACCES, errno.ENOTSUP, errno.ENOSYS, errno.ENOENT}
            if error.errno in unsupported and self.library.is_dir() and target.is_file():
                self.skipTest(f"Fixture filesystem cannot create symbolic links: {error}")
            raise
        with self.assertRaises(FileExistsError):
            follow_ui.save_follow_draft(self.draft, self.library, project_id="follow-test")
        self.assertEqual(target.read_text(), "untouched")
        self.assertEqual(list(self.library.iterdir()), [destination])

    def test_failed_publish_leaves_no_partial_library_entry(self):
        link = os.link
        count = 0
        def fail(*args, **kwargs):
            nonlocal count
            count += 1
            if count == 3:
                raise OSError("simulated full disk")
            return link(*args, **kwargs)
        with patch.object(follow_ui.os, "link", fail), self.assertRaisesRegex(OSError, "full disk"):
            follow_ui.save_follow_draft(self.draft, self.library, project_id="follow-test")
        self.assertFalse(list(self.library.iterdir()))

    def test_cancellation_during_save_rolls_back_its_own_sidecars(self):
        cancel = threading.Event()
        original = os.link
        def cancel_after_publish(*args, **kwargs):
            result = original(*args, **kwargs)
            cancel.set()
            return result
        with patch.object(follow_ui.os, "link", cancel_after_publish), self.assertRaises(InterruptedError):
            follow_ui.save_follow_draft(self.draft, self.library, project_id="follow-cancel", cancel_event=cancel)
        self.assertFalse(list(self.library.iterdir()))

    def test_cancellation_at_last_publication_rolls_back_complete_entry(self):
        cancel = threading.Event()
        original, published = os.link, []
        def cancel_after_last_publish(*args, **kwargs):
            result = original(*args, **kwargs)
            published.append(True)
            if len(published) == 3:
                cancel.set()
            return result
        with patch.object(follow_ui.os, "link", cancel_after_last_publish), self.assertRaises(InterruptedError):
            follow_ui.save_follow_draft(self.draft, self.library, project_id="follow-cancel", cancel_event=cancel)
        self.assertFalse(list(self.library.iterdir()))

    def test_save_checks_source_changes_and_draft_provenance(self):
        for mutate in (lambda d: d["evidence"].update(geometry_checked=True),
                       lambda d: d["evidence"]["source"].pop("sha256"),
                       lambda d: d["evidence"]["source"].pop("file_identity"),
                       lambda d: d["project"]["keys"][0].update(position=[float("nan"), 0, 0])):
            broken = copy.deepcopy(self.draft)
            mutate(broken)
            with self.assertRaises(ValueError):
                follow_ui.save_follow_draft(broken, self.library)
        self.demo.write_bytes(b"new content")
        with self.assertRaisesRegex(ValueError, "changed since"):
            follow_ui.save_follow_draft(self.draft, self.library)
        self.assertFalse(list(self.library.iterdir()))

    def test_concurrent_same_id_has_one_complete_winner(self):
        barrier = threading.Barrier(2)
        successes, failures = [], []
        def save():
            barrier.wait()
            try:
                successes.append(follow_ui.save_follow_draft(self.draft, self.library, project_id="follow-race"))
            except FileExistsError:
                failures.append(True)
        threads = [threading.Thread(target=save) for _ in range(2)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
        self.assertEqual((len(successes), len(failures)), (1, 1))
        self.assertEqual(json.loads(Path(successes[0]["project"]).read_text()), self.draft["project"])
        self.assertEqual(len(list(self.library.iterdir())), 3)


@unittest.skipUnless(importlib.util.find_spec("PySide6"), "Qt runtime required")
class FollowDialogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6 import QtCore, QtWidgets
        cls.QtCore, cls.QtWidgets = QtCore, QtWidgets
        cls.app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.demo = self.root/"recording.dem"
        self.demo.write_bytes(b"PBDEMS2\0test")
        self.library = self.root/"projects"
        self.library.mkdir()
        self.draft = draft_for(self.demo)
        self.dialogs = []
        self.addCleanup(self.cleanup_dialogs)

    def cleanup_dialogs(self):
        for dialog in self.dialogs:
            dialog.reject()
            dialog.deleteLater()
        self.app.processEvents()

    def wait_for(self, predicate, timeout=3):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            self.app.processEvents()
            if predicate():
                return
            time.sleep(.005)
        self.fail("Qt workflow did not complete")

    def dialog(self, **kwargs):
        with patch.object(follow_ui, "load_follow_players", return_value=[{"steamid": STEAM, "name": "Same nickname"}, {"steamid": str(int(STEAM)+1), "name": "Same nickname"}]):
            dialog = follow_ui.create_follow_dialog(None, self.demo, self.library, initial_tick=640, **kwargs)
            self.dialogs.append(dialog)
            dialog.show()
            self.wait_for(lambda: not dialog.busy)
        return dialog

    def test_generate_requires_explicit_save_and_changes_invalidate_result(self):
        dialog = self.dialog()
        self.assertEqual(dialog.player.currentData(), STEAM)
        self.assertFalse(dialog.save_button.isEnabled())
        with patch.object(follow_ui, "prepare_follow_draft", return_value=self.draft) as bake:
            dialog.player.setCurrentIndex(1)
            dialog.generate_button.click()
            self.wait_for(lambda: not dialog.busy)
        self.assertEqual(bake.call_args.args[1], str(int(STEAM)+1))
        self.assertTrue(dialog.save_button.isEnabled())
        self.assertIn("unverified", dialog.status.text())
        self.assertFalse(list(self.library.iterdir()))
        dialog.fov.setValue(80)
        self.assertIsNone(dialog.draft)
        self.assertFalse(dialog.save_button.isEnabled())

    def test_modes_emit_only_valid_options_and_range_never_exceeds_30s(self):
        dialog = self.dialog()
        dialog.mode.setCurrentIndex(dialog.mode.findData("attach"))
        options = dialog.options()
        self.assertIn("angle_offset", options)
        self.assertNotIn("look_at_offset", options)
        self.assertTrue(dialog.target_widget.isHidden())
        dialog.mode.setCurrentIndex(dialog.mode.findData("lock_on"))
        options = dialog.options()
        self.assertIn("camera_position", options)
        self.assertNotIn("offset", options)
        self.assertNotIn("position_half_life", options)
        self.assertTrue(dialog.offset_widget.isHidden())
        self.assertFalse(dialog.position_widget.isHidden())
        dialog.end_tick.setValue(100000)
        self.assertEqual(dialog.end_tick.value()-dialog.start_tick.value(), 30*64)
        dialog.start_tick.setValue(2_000_000_000-1)
        self.assertEqual(dialog.end_tick.value(), 2_000_000_000)

    def test_ui_stays_responsive_and_escape_discards_background_generation(self):
        dialog = self.dialog()
        started, release, finished = threading.Event(), threading.Event(), threading.Event()
        ui_events = []
        timer = self.QtCore.QTimer()
        timer.setInterval(5)
        timer.timeout.connect(lambda: ui_events.append(True))
        def blocked(*args, **kwargs):
            started.set()
            release.wait(2)
            finished.set()
            return self.draft
        with patch.object(follow_ui, "prepare_follow_draft", blocked):
            timer.start()
            dialog.generate_button.click()
            self.wait_for(lambda: started.is_set() and len(ui_events) >= 5)
            self.assertTrue(dialog.busy)
            self.assertFalse(dialog.generate_button.isEnabled())
            dialog.reject()
            self.assertFalse(dialog.isVisible())
            release.set()
            self.wait_for(finished.is_set)
            timer.stop()
        dialog.poll()
        self.assertIsNone(dialog.draft)
        self.assertFalse(list(self.library.iterdir()))

    def test_cancel_save_chooser_does_not_write_then_explicit_save_creates_library_entry(self):
        dialog = self.dialog()
        with patch.object(follow_ui, "prepare_follow_draft", return_value=self.draft):
            dialog.generate()
            self.wait_for(lambda: not dialog.busy)
        with patch.object(self.QtWidgets.QFileDialog, "getExistingDirectory", return_value=""):
            dialog.save()
        self.assertFalse(list(self.library.iterdir()))
        emitted = []
        dialog.draftSaved.connect(emitted.append)
        with patch.object(self.QtWidgets.QFileDialog, "getExistingDirectory", return_value=str(self.library)):
            dialog.save_button.click()
            self.wait_for(lambda: not dialog.busy)
        self.assertEqual(len(emitted), 1)
        self.assertEqual(Path(emitted[0]).parent, self.library)
        self.assertEqual(json.loads(Path(emitted[0]).read_text()), self.draft["project"])
        self.assertIn("Ctrl+O", dialog.status.text())
        self.assertIn("unverified", dialog.status.text())
        self.assertFalse(dialog.save_button.isEnabled())

    def test_worker_failure_is_visible_and_can_regenerate(self):
        dialog = self.dialog()
        with patch.object(follow_ui, "prepare_follow_draft", side_effect=ValueError("Target dead at tick 641")):
            dialog.generate()
            self.wait_for(lambda: not dialog.busy)
        self.assertIn("Target dead at tick 641", dialog.status.text())
        self.assertTrue(dialog.generate_button.isEnabled())
        self.assertFalse(dialog.save_button.isEnabled())
        self.assertFalse(list(self.library.iterdir()))

    def test_cancel_during_save_is_responsive_and_waits_for_rollback(self):
        dialog = self.dialog()
        with patch.object(follow_ui, "prepare_follow_draft", return_value=self.draft):
            dialog.generate()
            self.wait_for(lambda: not dialog.busy)
        release, started = threading.Event(), threading.Event()
        original = follow_ui.save_follow_draft
        def blocked(*args, **kwargs):
            started.set()
            release.wait(2)
            return original(*args, **kwargs)
        with patch.object(follow_ui, "save_follow_draft", blocked), \
             patch.object(self.QtWidgets.QFileDialog, "getExistingDirectory", return_value=str(self.library)):
            dialog.save()
            self.wait_for(started.is_set)
            self.assertTrue(dialog.close_button.isEnabled())
            self.assertEqual(dialog.close_button.text(), "Cancel")
            dialog.close()
            self.app.processEvents()
            self.assertTrue(dialog.isVisible())
            self.assertTrue(dialog._cancel.is_set())
            self.assertIn("Cancelling", dialog.status.text())
            release.set()
            self.wait_for(lambda: not dialog.busy)
        self.assertFalse(dialog.isVisible())
        self.assertFalse(list(self.library.iterdir()))

    def test_late_cancel_reports_successfully_committed_path(self):
        dialog = self.dialog()
        with patch.object(follow_ui, "prepare_follow_draft", return_value=self.draft):
            dialog.generate()
            self.wait_for(lambda: not dialog.busy)
        release, committed = threading.Event(), threading.Event()
        original = follow_ui.save_follow_draft
        def committed_then_blocked(*args, **kwargs):
            result = original(*args, **kwargs)
            committed.set()
            release.wait(2)
            return result
        emitted = []
        dialog.draftSaved.connect(emitted.append)
        with patch.object(follow_ui, "save_follow_draft", committed_then_blocked), \
             patch.object(self.QtWidgets.QFileDialog, "getExistingDirectory", return_value=str(self.library)):
            dialog.save()
            self.wait_for(committed.is_set)
            dialog.reject()
            self.assertTrue(dialog.isVisible())
            release.set()
            self.wait_for(lambda: not dialog.busy)
        self.assertEqual(len(emitted), 1)
        self.assertTrue(Path(emitted[0]).is_file())
        self.assertIn("Saved draft:", dialog.status.text())
        self.assertTrue(dialog.isVisible())
        self.assertTrue(dialog.close_button.isEnabled())
        dialog.reject()
        self.assertFalse(dialog.isVisible())


if __name__ == "__main__":
    unittest.main()
