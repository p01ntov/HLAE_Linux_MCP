#!/usr/bin/env python3
"""Compile the composition timeline's real headless ImGui interactions."""
from __future__ import annotations
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=ROOT / ".cache/cs2-overlay")
    options = parser.parse_args()
    compiler = shutil.which("g++")
    if not compiler:
        parser.error("g++ is required")
    spec = importlib.util.spec_from_file_location("timeline_overlay_build", HERE.parent / "build_overlay.py")
    assert spec and spec.loader
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    vendor = options.cache_dir.resolve() / "vendor"
    for dependency in ("imgui", "json"):
        marker = vendor / dependency / ".source-sha256"
        if not marker.is_file() or marker.read_text().strip() != build.SOURCES[dependency][1]:
            parser.error(f"Pinned {dependency} dependency missing/stale; run build_overlay.py with this cache first")
    imgui = vendor / "imgui"
    with tempfile.TemporaryDirectory(prefix="cs2-camera-timeline-test-") as temporary:
        binary = Path(temporary) / "camera_timeline_ui_test"
        subprocess.run([compiler, "-std=c++17", "-O0", "-Wall", "-Wextra", "-Wpedantic",
                        "-DIMGUI_ENABLE_TEST_ENGINE", f"-I{imgui}", f"-I{vendor / 'json'}",
                        str(HERE / "camera_timeline_ui_test.cpp"),
                        *(str(imgui / name) for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp")),
                        "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=45)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
