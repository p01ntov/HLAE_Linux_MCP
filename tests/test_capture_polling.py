from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from cs2_mvp.render import _CaptureLogWatcher, _wait_for_capture


class CapturePollingTests(unittest.TestCase):
    def test_only_new_bytes_from_the_plugin_log_are_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, archive = root / "csdm.log", root / "plugin.log"
            path.write_text("CS2_MVP_ENVIRONMENT_ERROR stale previous job\n")
            watcher = _CaptureLogWatcher(path, archive)
            self.assertIsNone(watcher.poll())
            (root / "game.log").write_text("CS2_MVP_CAMERA_ERROR unrelated stdout\n")
            with path.open("a") as log:
                log.write("current capture is ready\n")
            self.assertIsNone(watcher.poll())
            self.assertEqual(archive.read_text(), "current capture is ready\n")
            with path.open("a") as log:
                log.write("CS2_MVP_ENVIRON")
            self.assertIsNone(watcher.poll())
            with path.open("a") as log:
                log.write("MENT_ERROR selected pawn vanished\n")
            self.assertEqual(watcher.poll(), "CS2_MVP_ENVIRONMENT_ERROR selected pawn vanished")

    def test_truncated_and_replaced_log_starts_a_new_segment(self):
        for operation in ("truncate", "rewrite_larger", "replace", "remove_then_create"):
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path, archive = root / "csdm.log", root / "plugin.log"
                path.write_text("previous capture\n" * 8)
                watcher = _CaptureLogWatcher(path, archive)
                error = "CS2_MVP_CAMERA_ERROR current hook failed\n"
                if operation == "truncate":
                    path.write_text("")
                    self.assertIsNone(watcher.poll())
                    path.write_text(error)
                elif operation == "rewrite_larger":
                    path.write_text("new capture\n" * 30 + error)
                elif operation == "replace":
                    replacement = root / "replacement"
                    replacement.write_text(error)
                    replacement.replace(path)
                else:
                    path.unlink()
                    self.assertIsNone(watcher.poll())
                    path.write_text(error)
                self.assertEqual(watcher.poll(), error.strip())
                self.assertIn(error, archive.read_text())
                self.assertNotIn("previous capture", archive.read_text())

    def test_missing_log_and_partial_line_do_not_borrow_old_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, archive = root / "csdm.log", root / "plugin.log"
            watcher = _CaptureLogWatcher(path, archive)
            self.assertIsNone(watcher.poll())
            path.write_text("CS2_MVP_CAMERA_ERROR created after launch\n")
            self.assertIn("created after launch", watcher.poll())
            path.write_text("CS2_MVP_ENVIRON")
            watcher = _CaptureLogWatcher(path, root / "next.log")
            with path.open("a") as log:
                log.write("MENT_ERROR belongs to an old partial line\n")
            self.assertIsNone(watcher.poll())

    def test_real_failed_process_reports_error_and_keeps_restore_grace_bounded(self):
        for marker in ("CS2_MVP_ENVIRONMENT_ERROR", "CS2_MVP_CAMERA_ERROR"):
            with self.subTest(marker=marker), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path, archive = root / "csdm.log", root / "plugin.log"
                watcher = _CaptureLogWatcher(path, archive)
                script = (
                    "from pathlib import Path; import sys,time\n"
                    "p=Path(sys.argv[1])\n"
                    "with p.open('a') as f: f.write(sys.argv[2]+' actual failure reason\\n')\n"
                    "time.sleep(.1)\n"
                    "with p.open('a') as f: f.write('CS2_MVP_ENVIRONMENT_RESTORED players_owned=0 weapons_owned=0\\n')\n"
                    "time.sleep(30)\n"
                )
                process = subprocess.Popen([sys.executable, "-c", script, str(path), marker])
                events = []
                began = time.monotonic()
                try:
                    deadline = began + 3
                    while not path.exists() or not path.stat().st_size:
                        self.assertLess(time.monotonic(), deadline, "Owned test process never wrote its error")
                        time.sleep(.005)
                    failed_at = []
                    def notify(event):
                        events.append(event)
                        if event["stage"] == "failed":
                            failed_at.append(time.monotonic())
                            self.assertNotIn("CS2_MVP_ENVIRONMENT_RESTORED", archive.read_text())
                    with self.assertRaisesRegex(RuntimeError, marker + " actual failure reason"):
                        _wait_for_capture(process, None, root, watcher, timeout=20,
                                          notify=notify, failure_grace=.3)
                    self.assertLess(time.monotonic() - began, 2)
                    self.assertGreaterEqual(time.monotonic() - failed_at[0], .29)
                    self.assertIsNone(process.poll(), "Caller must retain ownership of final process cleanup")
                    failures = [event for event in events if event["stage"] == "failed"]
                    self.assertEqual(len(failures), 1)
                    self.assertEqual(failures[0]["message"], marker + " actual failure reason")
                    self.assertEqual(events[-1]["stage"], "failed")
                    self.assertIn("CS2_MVP_ENVIRONMENT_RESTORED", archive.read_text())
                finally:
                    process.terminate()
                    process.wait(timeout=5)

    def test_real_successful_process_ignores_stale_error_and_exits_normally(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, archive = root / "csdm.log", root / "plugin.log"
            path.write_text("CS2_MVP_CAMERA_ERROR stale job\n")
            watcher = _CaptureLogWatcher(path, archive)
            process = subprocess.Popen([sys.executable, "-c",
                "from pathlib import Path; import sys; "
                "p=Path(sys.argv[1]); f=p.open('a'); f.write('current success\\n'); f.close()", str(path)])
            events = []
            try:
                _wait_for_capture(process, None, root, watcher, timeout=5, notify=events.append)
                self.assertEqual(process.returncode, 0)
                self.assertFalse(any(event["stage"] == "failed" for event in events))
                self.assertEqual(archive.read_text(), "current success\n")
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
