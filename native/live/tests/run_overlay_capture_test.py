#!/usr/bin/env python3
"""Verify one-shot post-render readback on an offscreen image; never launch CS2."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--icd", type=Path, help="Optional explicit Vulkan ICD JSON")
    options = parser.parse_args()
    here = Path(__file__).resolve().parent
    include = here.parents[2] / ".cache/cs2-overlay/vendor/vulkan/include"
    environment = os.environ.copy()
    if options.icd:
        environment["VK_DRIVER_FILES"] = environment["VK_ICD_FILENAMES"] = str(options.icd.resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="cs2-overlay-capture-test-") as temporary:
        binary = Path(temporary) / "test"
        subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", f"-I{include}",
                        str(here / "overlay_capture_test.cpp"), "-lvulkan", "-o", str(binary)],
                       check=True, timeout=60)
        subprocess.run([str(binary)], check=True, env=environment, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
