"""Exercise automatic demo selection, refresh, browse and launch offscreen."""
from pathlib import Path
import time
from unittest.mock import patch

from PySide6 import QtCore,QtWidgets
from cs2_mvp.live import argument_parser,graphical_launcher

games=[Path("/installed/SteamLibrary/steamapps/common/Counter-Strike Global Offensive/game")]
demos=[{"path":"/installed/replays/new.dem","filename":"new.dem","map_name":"de_nuke","modified":300,
        "size":1024**2,"game_directory":str(games[0]),"ready":True},
       {"path":"/installed/replays/old.dem","filename":"old.dem","map_name":"de_dust2","modified":100,
        "size":1024**2,"game_directory":str(games[0]),"ready":True}]
scans=[];sessions=[];failures=[];phase=0
real_exec=QtWidgets.QApplication.exec

def discover(paths):
    scans.append(paths)
    return demos

def session(demo,**kwargs):
    sessions.append((str(demo),kwargs["game_dir"]))
    return {"status":"closed"}

def loop(app):
    window=next(w for w in app.topLevelWidgets() if isinstance(w,QtWidgets.QMainWindow))
    combo=window.findChild(QtWidgets.QComboBox,"demo_library")
    demo=window.findChild(QtWidgets.QLineEdit,"demo_path")
    game=window.findChild(QtWidgets.QLineEdit,"game_directory")
    refresh=window.findChild(QtWidgets.QPushButton,"refresh_demo_library")
    began=time.monotonic();timer=QtCore.QTimer()
    def step():
        global phase
        try:
            if time.monotonic()-began>5:
                raise AssertionError("demo library timed out")
            if phase==0 and combo.count()==2:
                assert demo.text()==demos[0]["path"] and game.text()==str(games[0])
                assert "de_nuke" in combo.itemText(0)
                combo.setCurrentIndex(1);combo.activated.emit(1)
                assert demo.text()==demos[1]["path"]
                refresh.click();phase=1
            elif phase==1 and len(scans)>=2 and refresh.isEnabled():
                assert demo.text()==demos[1]["path"]
                next(b for b in window.findChildren(QtWidgets.QPushButton) if b.text()=="Browse…").click()
                assert demo.text()=="/manual/custom.dem"
                assert "outside library" in combo.currentText()
                refresh.click();phase=2
            elif phase==2 and len(scans)>=3 and refresh.isEnabled():
                assert demo.text()=="/manual/custom.dem"
                assert "outside library" in combo.currentText()
                next(b for b in window.findChildren(QtWidgets.QPushButton) if b.text()=="OPEN DEMO + EDITOR").click()
                phase=3
            elif phase==3 and sessions:
                assert sessions==[("/manual/custom.dem",str(games[0]))]
                phase=4;window.close()
        except BaseException as error:
            failures.append(str(error));window.close();app.quit()
    timer.timeout.connect(step);timer.start(20)
    return real_exec()

with patch.object(QtWidgets.QApplication,"exec",loop),patch("cs2_mvp.discovery.find_games",return_value=games), \
     patch("cs2_mvp.discovery.discover_demos",discover),patch("cs2_mvp.live.run_session",session), \
     patch.object(QtWidgets.QFileDialog,"getOpenFileName",return_value=("/manual/custom.dem","")):
    status=graphical_launcher(argument_parser().parse_args([]))
assert status==0 and phase==4 and not failures,(status,phase,failures)
print("DEMO_LIBRARY_GUI_PASS")
