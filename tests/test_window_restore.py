"""Offline fixtures for editor video/config restoration; never touch Steam or /proc."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp import render


class WindowRestoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.steam = self.root / "steam"
        self.cfg = self.steam / "userdata/123/730/local/cfg"
        self.cfg.mkdir(parents=True)
        self.run = self.root / "run"
        self.run.mkdir()
        self.video = self.cfg / "cs2_video.txt"
        self.original = (b'"video.cfg"\r\n{\r\n "setting.fullscreen" "1"\r\n'
                         b' "setting.defaultres" "1920"\r\n "setting.defaultresheight" "1080"\r\n}\r\n')
        self.video.write_bytes(self.original)
        self.now = 0.
        self.process_probe = self.enter_context(patch.object(render, "process_ids", return_value=[]))
        self.enter_context(patch.object(render.sys, "frozen", False, create=True))
        self.enter_context(patch.object(render, "steam_roots", return_value=[self.steam]))
        self.enter_context(patch.object(render.time, "monotonic", side_effect=lambda: self.now))
        self.sleep = self.enter_context(patch.object(render.time, "sleep", side_effect=self.advance))

    # unittest.TestCase.enterContext is unavailable on some supported runtimes.
    def enter_context(self, manager):
        value = manager.__enter__()
        self.addCleanup(manager.__exit__, None, None, None)
        return value

    def advance(self, seconds):
        self.now += seconds

    def backup_only(self):
        # Persist the same interrupted-session backup without invoking cleanup.
        with patch.object(render, "restore_configs"):
            with render.config_backup(self.run):
                pass

    def journal(self):
        return json.loads((self.run / "config-restore.json").read_text())

    def test_restores_exact_window_settings_and_archives_session_changes(self):
        with render.config_backup(self.run):
            self.video.write_bytes(b'editor windowed settings')
            (self.cfg / "editor-created.vcfg").write_bytes(b'session')
        self.assertEqual(self.video.read_bytes(), self.original)
        self.assertEqual((self.cfg / "editor-created.vcfg").read_bytes(), b'session')
        self.assertEqual((self.run / "changed-config/0/cs2_video.txt").read_bytes(), b'editor windowed settings')
        self.assertEqual((self.run / "changed-config/0/editor-created.vcfg").read_bytes(), b'session')
        self.assertEqual(self.journal()["status"], "restored")
        self.assertLessEqual(self.now, 1.2)

    def test_completed_restore_cannot_overwrite_later_normal_game_settings(self):
        with render.config_backup(self.run):
            self.video.write_bytes(b'editor windowed settings')
        self.video.write_bytes(b'user changed the resolution later')
        # A repeat cleanup must not even require inspecting a currently running
        # ordinary game, let alone replace its current config with an old one.
        self.process_probe.side_effect = AssertionError("Completed restore must be a no-op")
        result = render.restore_configs(self.run)
        self.assertEqual(result["status"], "restored")
        self.assertEqual(self.video.read_bytes(), b'user changed the resolution later')

    def test_fullscreen_and_original_resolution_restore_after_late_flush(self):
        self.backup_only()
        editor_video = (b'"video.cfg"\n{\n "setting.fullscreen" "0"\n'
                        b' "setting.defaultres" "2560"\n "setting.defaultresheight" "1440"\n}\n')
        self.video.write_bytes(editor_video)
        late_video = editor_video.replace(b'"video.cfg"', b'"video.cfg" // late Steam-style flush')
        flushed = False
        def late_flush(seconds):
            nonlocal flushed
            self.advance(seconds)
            if self.now >= .3 and not flushed:
                self.video.write_bytes(late_video)
                flushed = True
        self.sleep.side_effect = late_flush
        render.restore_configs(self.run)
        self.assertTrue(flushed)
        self.assertGreaterEqual(self.now, .8)
        self.assertEqual(self.video.read_bytes(), self.original)
        self.assertIn(b'"setting.fullscreen" "1"', self.video.read_bytes())
        self.assertIn(b'"setting.defaultres" "1920"', self.video.read_bytes())
        self.assertIn(b'"setting.defaultresheight" "1080"', self.video.read_bytes())
        self.assertEqual((self.run / "changed-config/0/cs2_video.txt").read_bytes(), late_video)

    def test_continuous_writer_times_out_and_keeps_config_backup(self):
        self.backup_only()
        def write_each_poll(seconds):
            self.advance(seconds)
            self.video.write_text(str(self.now))
        self.sleep.side_effect = write_each_poll
        with self.assertRaisesRegex(RuntimeError, "still being written"):
            render.restore_configs(self.run)
        self.assertLess(self.now, 3.2)
        self.assertNotEqual(self.video.read_bytes(), self.original)
        self.assertEqual((self.run / "original-config/0/cs2_video.txt").read_bytes(), self.original)
        self.assertEqual(self.journal()["status"], "snapshot_pending")
        self.video.write_bytes(b'later ordinary-game settings')
        with self.assertRaisesRegex(RuntimeError, "snapshot was not completed"):
            render.restore_configs(self.run)
        self.assertEqual(self.video.read_bytes(), b'later ordinary-game settings')

    def test_delayed_write_after_restore_is_not_reported_as_safe_exit(self):
        self.backup_only()
        self.video.write_bytes(b'editor settings')
        wrote_after_restore = False
        def late_write(seconds):
            nonlocal wrote_after_restore
            self.advance(seconds)
            if self.video.read_bytes() == self.original and not wrote_after_restore:
                self.video.write_bytes(b'late new writer changed the config')
                wrote_after_restore = True
        self.sleep.side_effect = late_write
        with self.assertRaisesRegex(RuntimeError, "changed after restoration"):
            render.restore_configs(self.run)
        self.assertTrue(wrote_after_restore)
        self.assertEqual(self.video.read_bytes(), b'late new writer changed the config')
        self.assertEqual(self.journal()["status"], "restoring")
        self.assertEqual((self.run / "original-config/0/cs2_video.txt").read_bytes(), self.original)

    def test_game_appearing_during_quiet_period_keeps_everything_untouched(self):
        self.backup_only()
        self.video.write_bytes(b'new game settings')
        self.process_probe.side_effect = [[], [999]]
        with self.assertRaisesRegex(RuntimeError, "still running"):
            render.restore_configs(self.run)
        self.assertEqual(self.video.read_bytes(), b'new game settings')
        self.assertEqual(self.journal()["status"], "snapshot_pending")

    def test_pending_retry_rejects_later_user_edit(self):
        self.backup_only()
        self.video.write_bytes(b'editor settings')
        with patch.object(render, "_atomic_config_copy", side_effect=OSError("interrupted before archive")):
            with self.assertRaisesRegex(OSError, "interrupted"):
                render.restore_configs(self.run)
        self.assertEqual(self.journal()["status"], "restoring")
        self.video.write_bytes(b'later ordinary-game edit')
        with self.assertRaisesRegex(RuntimeError, "changed during exit"):
            render.restore_configs(self.run)
        self.assertEqual(self.video.read_bytes(), b'later ordinary-game edit')

    def test_changed_config_after_archive_is_not_overwritten(self):
        self.backup_only()
        self.video.write_bytes(b'editor settings')
        copy = render._atomic_config_copy
        def change_after_archive(source, destination):
            copy(source, destination)
            if destination == self.run / 'changed-config/0/cs2_video.txt':
                self.video.write_bytes(b'concurrent user edit')
        with patch.object(render, '_atomic_config_copy', side_effect=change_after_archive):
            with self.assertRaisesRegex(RuntimeError, 'changed after the exit snapshot'):
                render.restore_configs(self.run)
        self.assertEqual(self.video.read_bytes(), b'concurrent user edit')
        self.assertEqual(self.journal()['status'], 'restoring')

    def test_game_appearing_after_archive_blocks_restore(self):
        self.backup_only()
        self.video.write_bytes(b'editor settings')
        copy = render._atomic_config_copy
        def start_game_after_archive(source, destination):
            copy(source, destination)
            if destination == self.run / 'changed-config/0/cs2_video.txt':
                self.process_probe.return_value = [999]
        with patch.object(render, '_atomic_config_copy', side_effect=start_game_after_archive):
            with self.assertRaisesRegex(RuntimeError, 'still running'):
                render.restore_configs(self.run)
        self.assertEqual(self.video.read_bytes(), b'editor settings')
        self.assertEqual(self.journal()['status'], 'restoring')

    def test_retry_after_partial_restore_finishes_without_losing_archives(self):
        self.backup_only()
        self.video.write_bytes(b'editor settings')
        nested = self.cfg / "nested/other.vcfg"
        nested.parent.mkdir()
        nested.write_bytes(b'new session config')
        copy = render._atomic_config_copy
        failed = False
        def interrupted(source, destination):
            nonlocal failed
            copy(source, destination)
            if destination == self.video and not failed:
                failed = True
                raise OSError("interrupted after replace")
        with patch.object(render, "_atomic_config_copy", side_effect=interrupted):
            with self.assertRaisesRegex(OSError, "interrupted after replace"):
                render.restore_configs(self.run)
        self.assertEqual(self.journal()["status"], "restoring")
        render.restore_configs(self.run)
        self.assertEqual(self.video.read_bytes(), self.original)
        self.assertEqual(nested.read_bytes(), b'new session config')
        self.assertEqual((self.run / "changed-config/0/cs2_video.txt").read_bytes(), b'editor settings')
        self.assertEqual((self.run / "changed-config/0/nested/other.vcfg").read_bytes(), b'new session config')
        self.assertEqual(self.journal()["status"], "restored")

    def test_backup_corruption_is_not_used_as_restoration(self):
        self.backup_only()
        self.video.write_bytes(b'editor settings')
        (self.run / "original-config/0/cs2_video.txt").write_bytes(b'corrupt backup')
        with self.assertRaisesRegex(RuntimeError, "backup changed"):
            render.restore_configs(self.run)
        self.assertEqual(self.video.read_bytes(), b'editor settings')

    def test_substituted_symlink_is_never_followed(self):
        self.backup_only()
        unrelated = self.root / "unrelated.txt"
        unrelated.write_bytes(b'leave alone')
        self.video.unlink()
        self.video.symlink_to(unrelated)
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            render.restore_configs(self.run)
        self.assertEqual(unrelated.read_bytes(), b'leave alone')
        self.assertTrue(self.video.is_symlink())

    def test_no_config_tree_before_session_preserves_new_files(self):
        self.video.unlink()
        self.cfg.rmdir()
        with render.config_backup(self.run):
            self.cfg.mkdir(parents=True)
            (self.cfg / "cs2_video.txt").write_bytes(b'new windowed config')
        self.assertEqual((self.cfg / "cs2_video.txt").read_bytes(), b'new windowed config')
        self.assertEqual((self.run / "changed-config/0/cs2_video.txt").read_bytes(), b'new windowed config')

    def test_atomic_restore_never_exposes_partial_config_on_replace_failure(self):
        self.backup_only()
        self.video.write_bytes(b'editor settings')
        replace = render.os.replace
        def fail_video(source, destination):
            if destination == self.video:
                raise OSError("replace failed")
            return replace(source, destination)
        with patch.object(render.os, "replace", side_effect=fail_video):
            with self.assertRaisesRegex(OSError, "replace failed"):
                render.restore_configs(self.run)
        self.assertEqual(self.video.read_bytes(), b'editor settings')
        self.assertFalse(list(self.cfg.glob("*.cs2-camera-*.tmp")))
        self.assertEqual(self.journal()["status"], "restoring")


if __name__ == "__main__":
    unittest.main()
