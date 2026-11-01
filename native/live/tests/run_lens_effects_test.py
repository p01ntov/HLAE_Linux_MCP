#!/usr/bin/env python3
"""Compile and exercise real offscreen Vulkan lens passes; never launch CS2."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--icd", type=Path)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--benchmark", action="store_true", help="Measure a warmed 1080p combined pass with GPU timestamps")
    options = parser.parse_args()
    include = ROOT / ".cache/cs2-overlay/vendor/vulkan/include"
    if not (include / "vulkan/vulkan.h").is_file():
        include = Path("/usr/include")
    environment = os.environ.copy()
    if options.icd:
        environment["VK_ICD_FILENAMES"] = environment["VK_DRIVER_FILES"] = str(options.icd.resolve(strict=True))
    if options.validation:
        environment["CS2_LENS_TEST_VALIDATION"] = "1"
    if options.benchmark:
        environment["CS2_LENS_TEST_BENCHMARK"] = "1"
    if options.artifacts:
        options.artifacts.mkdir(parents=True, exist_ok=True)
        environment["CS2_LENS_TEST_ARTIFACTS"] = str(options.artifacts.resolve())
    with tempfile.TemporaryDirectory(prefix="cs2-lens-test-") as temporary:
        binary = Path(temporary) / "lens_effects_test"
        subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", f"-I{include}",
                        str(HERE / "lens_effects_test.cpp"), "-lvulkan", "-o", str(binary)],
                       check=True, timeout=60)
        subprocess.run([str(binary)], env=environment, check=True, timeout=60)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
