#!/usr/bin/env python3
"""Compile actual overlay helpers against pinned ImGui; never launch CS2."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[2]
vendor = root / ".cache/cs2-overlay/vendor"
imgui = vendor / "imgui"
with tempfile.TemporaryDirectory(prefix="cs2-overlay-dispatch-test-") as temporary:
    binary = Path(temporary) / "test"
    subprocess.run([
        "g++", "-std=c++17", "-O2", "-fvisibility=hidden", "-fvisibility-inlines-hidden",
        "-fno-gnu-unique", "-Wl,-Bsymbolic-functions", "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
        f"-I{imgui}", f"-I{imgui / 'backends'}", f"-I{vendor / 'vulkan/include'}",
        f"-I{vendor / 'json'}", "-I/usr/include/SDL3",
        str(here / "overlay_vulkan_dispatch_test.cpp"), str(here.parent / "compat_math.cpp"),
        str(here.parent / "resolver.S"),
        *(str(imgui / name) for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp")),
        str(imgui / "backends/imgui_impl_vulkan.cpp"), "-ldl", "-pthread", "-o", str(binary),
    ], check=True, timeout=120)
    environment = dict(os.environ)
    environment.pop("CS2_CAMERA_DEPTH_PROBE", None)
    subprocess.run([str(binary)], env=environment, check=True, timeout=15)
    subprocess.run([str(binary)], env=dict(environment, CS2_CAMERA_DEPTH_PROBE=str(Path(temporary) / "depth.jsonl")),
                   check=True, timeout=15)
