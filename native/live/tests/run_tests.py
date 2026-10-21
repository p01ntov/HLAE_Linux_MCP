#!/usr/bin/env python3
"""Compile and run the live controller's isolated C++ tests."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
includes = ROOT / ".cache/csdm-live/upstream/cs2-server-plugin/cs2-server-plugin/deps/json/include"
if not includes.is_dir():
    includes = ROOT / ".cache/csdm-motion/upstream/cs2-server-plugin/cs2-server-plugin/deps/json/include"
if not includes.is_dir():
    raise SystemExit("Build native/live/build_live.py first to fetch the pinned JSON headers")
with tempfile.TemporaryDirectory(prefix="cs2-live-tests-") as temporary:
    for source in (HERE / "live_controller_test.cpp", HERE / "live_socket_test.cpp", HERE / "compat_math_test.cpp", HERE / "overlay_platform_test.cpp", HERE / "live_telemetry_test.cpp", HERE / "unsupported_cosmetics_test.cpp", HERE / "appearance_pack_test.cpp", HERE / "team_palette_test.cpp", HERE / "player_cosmetics_test.cpp", HERE / "weapon_cosmetics_test.cpp", HERE / "glove_cosmetics_test.cpp", HERE / "glove_pose_test.cpp", HERE / "glove_render_phase_test.cpp", HERE / "presentation_test.cpp", HERE / "observer_player_test.cpp", HERE / "player_selection_test.cpp", HERE / "hud_presentation_test.cpp", HERE / "demo_refresh_test.cpp", ROOT / "native/tests/capture_environment_test.cpp", ROOT / "native/tests/linux_view_patch_test.cpp"):
        binary = Path(temporary) / source.stem
        subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-pthread",
                        "-fvisibility=hidden", "-fvisibility-inlines-hidden", "-fno-gnu-unique",
                        "-Wl,-Bsymbolic-functions", f"-I{includes}", f"-I{HERE.parent}",
                        str(source), str(HERE.parent / "compat_math.cpp"), "-o", str(binary)], check=True)
        versions = {(int(major), int(minor)) for major, minor in re.findall(rb"GLIBC_(\d+)\.(\d+)", binary.read_bytes())}
        if versions and max(versions) > (2, 38):
            raise RuntimeError(f"{source.name} requires glibc {max(versions)} above the 2.38 baseline")
        subprocess.run([str(binary)], check=True, timeout=15)
subprocess.run([sys.executable, str(HERE / "interposition_test.py")], check=True, timeout=60)
