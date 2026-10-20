#!/usr/bin/env python3
"""Compile and run the offscreen Gaussian backdrop test; never launch CS2."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--icd", type=Path, help="Explicit Vulkan ICD JSON, e.g. /usr/share/vulkan/icd.d/lvp_icd.json")
    options = parser.parse_args()
    if not shutil.which("g++"):
        parser.error("g++ is required to compile the isolated Vulkan test")
    include = ROOT / ".cache/cs2-overlay/vendor/vulkan/include"
    if not (include / "vulkan/vulkan.h").is_file():
        include = Path("/usr/include")
    if not (include / "vulkan/vulkan.h").is_file():
        parser.error("Vulkan headers are missing; build native/live/build_overlay.py first, or install Vulkan development headers")
    environment = os.environ.copy()
    if options.icd:
        icd = options.icd.expanduser().resolve(strict=True)
        environment["VK_ICD_FILENAMES"] = str(icd)
        # A parent VK_DRIVER_FILES otherwise takes precedence in newer loaders.
        environment["VK_DRIVER_FILES"] = str(icd)
    with tempfile.TemporaryDirectory(prefix="cs2-overlay-blur-test-") as temporary:
        binary = Path(temporary) / "overlay_blur_test"
        subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra",
                        "-Wno-missing-field-initializers", f"-I{include}",
                        str(HERE / "overlay_blur_test.cpp"), "-lvulkan", "-o", str(binary)],
                       check=True, timeout=60)
        subprocess.run([str(binary)], env=environment, check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
