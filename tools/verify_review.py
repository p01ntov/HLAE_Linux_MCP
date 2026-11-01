#!/usr/bin/env python3
"""Run the offline review checks with all generated files under a chosen root."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--group", choices=["python", "native", "ui", "gpu", "all"], default="all")
    args = parser.parse_args()
    out = args.artifacts.resolve(); out.mkdir(parents=True, exist_ok=True)
    # Linux Unix socket names are capped at 107 bytes: keep fixture roots short.
    temporary = ROOT.parent / "t"; temporary.mkdir(exist_ok=True); temporary.chmod(0o700)
    env = dict(os.environ, TMPDIR=str(temporary), PYTHONDONTWRITEBYTECODE="1", PYTHONPATH=str(ROOT/"src"),
               QT_QPA_PLATFORM="offscreen", CS2_CAMERA_TEST_ARTIFACTS=str(temporary),
               CS2_CAMERA_EXPORT_PRESETS=str(out/"config/export-profiles.json"))
    for name in ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"):
        path = out / name.lower(); path.mkdir(exist_ok=True, mode=0o700); env[name] = str(path)
    commands = {
        "python": [("python", [sys.executable, "-m", "unittest", "discover", "-s", "tests"], 180)],
        "native": [("native", [sys.executable, "native/live/tests/run_tests.py"], 600)],
        "ui": [(name, [sys.executable, "native/live/tests/"+runner], 180) for name,runner in (
            ("editor-ui", "run_editor_ui_test.py"), ("timeline-ui", "run_camera_timeline_ui_test.py"),
            ("tool-surfaces", "run_camera_tool_surfaces_test.py"))],
        "gpu": [(name, [sys.executable, "native/live/tests/"+runner], 90) for name,runner in (
            ("capture-gpu", "run_overlay_capture_test.py"), ("dispatch-gpu", "run_overlay_vulkan_dispatch_test.py"))],
    }
    selected = sum(commands.values(), []) if args.group == "all" else commands[args.group]
    report = {"group":args.group,"source":str(ROOT),"checks":[],"passed":False}
    for name, command, timeout in selected:
        began = time.monotonic()
        with (out/(name+".log")).open("w") as log:
            try:
                result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = 124
        report["checks"].append({"name":name,"exit_code":status,"seconds":round(time.monotonic()-began,3),"log":name+".log"})
        print(name, "PASS" if status==0 else "FAIL", flush=True)
    report["passed"] = all(item["exit_code"]==0 for item in report["checks"])
    (out/("checks-"+args.group+".json")).write_text(json.dumps(report,indent=2)+"\n")
    return 0 if report["passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
