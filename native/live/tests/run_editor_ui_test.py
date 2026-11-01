#!/usr/bin/env python3
"""Run the editor interaction regression without SDL, a display server, or a GPU.

Dependencies are the pinned ImGui/JSON cache populated by build_overlay.py.
This runner does not download files, launch CS2, or modify the game.
"""
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

    spec = importlib.util.spec_from_file_location("editor_overlay_build", HERE.parent / "build_overlay.py")
    assert spec and spec.loader
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    vendor = options.cache_dir.resolve() / "vendor"
    for dependency in ("imgui", "json"):
        marker = vendor / dependency / ".source-sha256"
        if not marker.is_file() or marker.read_text().strip() != build.SOURCES[dependency][1]:
            parser.error(
                f"Pinned {dependency} dependency missing or stale. "
                "Run native/live/build_overlay.py with this cache directory first."
            )

    imgui = vendor / "imgui"
    with tempfile.TemporaryDirectory(prefix="cs2-editor-ui-test-") as temporary:
        binary = Path(temporary) / "editor_ui_test"
        subprocess.run(
            [compiler, "-std=c++17", "-O0", "-Wall", "-Wextra", "-Wpedantic",
             "-DIMGUI_ENABLE_TEST_ENGINE", f"-I{imgui}", f"-I{vendor / 'json'}",
             str(HERE / "editor_ui_test.cpp"),
             *(str(imgui / name) for name in
               ("imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp")),
             "-o", str(binary)],
            check=True, timeout=120,
        )
        subprocess.run([str(binary)], check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
