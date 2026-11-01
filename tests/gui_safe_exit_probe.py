"""Virtual Qt-only safe-exit lifecycle; all desktop/game adapters are fake."""
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from unittest.mock import patch

from PySide6 import QtCore, QtWidgets
from cs2_mvp.live import _write_json, argument_parser, graphical_launcher

MODE = sys.argv[1]
assert os.environ.get("QT_QPA_PLATFORM") == "offscreen"
PROJECT = {"name": "Manual shot", "duration": 6.2, "keys": [{"time": 1.25, "fov": 80}]}
events, failures = [], []
phase = {"requested": False, "retried": False}
real_exec = QtWidgets.QApplication.exec


class Client:
    def __init__(self, *args, **kwargs):
        pass

    def request(self, method, params=None):
        assert method == "get_state", method
        return {"project": PROJECT, "camera_valid": True, "view_hook_installed": True, "export_request_id": 0}


with tempfile.TemporaryDirectory() as temporary:
    saved = Path(temporary) / "project.json"
    os.environ["XDG_STATE_HOME"] = temporary

    def session(demo, **kwargs):
        kwargs["on_ready"](Client())
        events.append("ready")
        attempts = 0
        while not kwargs["stop"].wait(.01):
            if kwargs["safe_exit"].is_set():
                kwargs["safe_exit"].clear()
                attempts += 1
                events.append("saving")
                kwargs["on_safe_exit"]("saving", "")
                if MODE == "retry" and attempts == 1:
                    events.append("error")
                    kwargs["on_safe_exit"]("error", "simulated disk failure")
                    continue
                _write_json(saved, PROJECT)
                events.append("saved")
                kwargs["on_safe_exit"]("closing", "")
                events.append("restored")
                return {"status": "closed", "safe_exit": True}
        failures.append("unsafe stop_event used for an explicit UI exit")
        return {"status": "closed"}

    def loop(app):
        window = next(item for item in app.topLevelWidgets() if isinstance(item, QtWidgets.QMainWindow))
        button = window.findChild(QtWidgets.QPushButton, "safe_exit")
        assert button is not None
        began = time.monotonic()
        timer = QtCore.QTimer()

        def step():
            if time.monotonic() - began > 5:
                failures.append("timeout")
                # Stop only this fake worker, never an actual owned process.
                button.click()
                return
            if "ready" in events and not phase["requested"]:
                phase["requested"] = True
                if MODE == "close":
                    window.close()
                else:
                    button.click()
            if MODE == "retry" and "error" in events and button.isEnabled() and not phase["retried"]:
                assert window.isVisible()
                assert not saved.exists()
                assert "restored" not in events
                phase["retried"] = True
                button.click()

        timer.timeout.connect(step)
        timer.start(20)
        return real_exec()

    with patch.object(QtWidgets.QApplication, "exec", loop), \
         patch("cs2_mvp.live.run_session", session), \
         patch("cs2_mvp.live.NativeClient", Client), \
         patch("cs2_mvp.discovery.find_games", return_value=[]), \
         patch("cs2_mvp.discovery.discover_demos", return_value=[]), \
         patch("cs2_mvp.live.game_display", return_value=("Fake primary", (2560, 1440))):
        result = graphical_launcher(argument_parser().parse_args(["--demo", "/fake/input.dem", "--start-session"]))
    assert result == 0 and not failures, (result, failures, events)
    assert json.loads(saved.read_text()) == PROJECT
    assert events[-2:] == ["saved", "restored"], events
    if MODE == "retry":
        assert phase["retried"] and events.count("saving") == 2
print("SAFE_EXIT_GUI_PASS", MODE, json.dumps(events))
