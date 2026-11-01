"""Offscreen end-to-end desktop lifecycle with a fake owned game/backend."""
import json
import os
from pathlib import Path
import tempfile
import time
from unittest.mock import patch

from PySide6 import QtCore, QtWidgets
from cs2_mvp.live import argument_parser, graphical_launcher

PROJECT = {"name":"Test","start_tick":1000,"duration":1,
           "keys":[{"id":"1","time":0,"position":[0,0,64],"angles":[0,0,0],"fov":90}]}
calls, sessions, failures = [], [], []
phase = {"launched":False,"exported":False,"closing":False}


class Client:
    def __init__(self, *args, **kwargs):
        pass

    def request(self, method, params=None):
        calls.append(method)
        if method == "get_state":
            return {"project":PROJECT,"camera_valid":True,"view_hook_installed":True,"export_request_id":1,
                    "cursor":.5,"camera":{"position":[0,0,64],"angles":[0,0,0],"fov":90},"appearance":{"active":False}}
        return json.loads(json.dumps(PROJECT)) if method == "export_project" else {}


def session(demo, **kwargs):
    sessions.append(str(demo))
    kwargs["on_ready"](Client())
    while not kwargs["stop"].wait(.02):
        if kwargs["safe_exit"].is_set():
            kwargs["on_safe_exit"]("saving", "")
            calls.append("safe_exit_saved")
            kwargs["on_safe_exit"]("closing", "")
            return {"status":"closed", "safe_exit":True}
    return {"status":"closed"}


def render(*args, **kwargs):
    calls.append("render")
    Path(kwargs["output"]).write_bytes(b"mocked backend output")
    return {"output":str(kwargs["output"])}


real_exec = QtWidgets.QApplication.exec
with tempfile.TemporaryDirectory() as temporary:
    os.environ["XDG_STATE_HOME"] = temporary
    def loop(app):
        window = next(w for w in app.topLevelWidgets() if isinstance(w, QtWidgets.QMainWindow))
        began = time.monotonic()
        timer = QtCore.QTimer()
        def step():
            if time.monotonic()-began > 8:
                failures.append("timeout")
                window.close()
                return
            if not phase["launched"]:
                # The dedicated demo shortcut starts through the full GUI so
                # its in-game Export request has a desktop owner.
                phase["launched"] = bool(sessions)
            dialogs = [w for w in app.topLevelWidgets() if hasattr(w,"destination") and w.isVisible()]
            if dialogs and not phase["exported"]:
                dialog = dialogs[0]
                dialog.destination.setText(str(Path(temporary)/"out.mp4"))
                dialog.render_button.click()
                phase["exported"] = True
            if len(sessions) >= 2 and "import_project" in calls and not phase["closing"]:
                phase["closing"] = True
                for dialog in dialogs:
                    dialog.close()
                window.close()
        timer.timeout.connect(step)
        timer.start(25)
        return real_exec()
    with patch.object(QtWidgets.QApplication,"exec",loop), patch("cs2_mvp.live.run_session",session), \
         patch("cs2_mvp.live.NativeClient",Client), patch("cs2_mvp.export_ui.render_clip",render), \
         patch("cs2_mvp.discovery.find_games",return_value=[]), \
         patch("cs2_mvp.discovery.discover_demos",return_value=[]), \
         patch("cs2_mvp.live.game_display",return_value=("Fake primary",(2560,1440))), \
         patch("cs2_mvp.discovery.validate_media_tools",return_value={}):
        status = graphical_launcher(argument_parser().parse_args(["--demo","/fake/match.dem","--start-session"]))
assert status == 0 and not failures and len(sessions) == 2 and calls.count("render") == 1 and "import_project" in calls and calls.count("safe_exit_saved") == 1, (status, failures, sessions, calls)
print("EXPORT_GUI_LIFECYCLE_PASS", json.dumps({"sessions":len(sessions),"render_jobs":calls.count("render"),"restored":True}))
