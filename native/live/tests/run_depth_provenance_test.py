#!/usr/bin/env python3
"""Exercise the passive depth provenance model without loading a game or Vulkan."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    options = parser.parse_args()
    include = ROOT / ".cache/cs2-overlay/vendor/vulkan/include"
    if not (include / "vulkan/vulkan.h").is_file():
        include = Path("/usr/include")
    temporary_root = ROOT / "adapters-20261101/depth/tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="provenance-", dir=temporary_root) as directory:
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if options.sanitize else []
        environment = dict(os.environ, TMPDIR=str(temporary_root))
        for name in ("depth_provenance_test", "depth_probe_layer_test", "depth_probe_path_test"):
            binary = Path(directory) / name
            subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-Wno-missing-field-initializers", *flags, f"-I{include}",
                            f"-I{ROOT / '.cache/cs2-overlay/vendor/json'}",
                            str(HERE / (name + ".cpp")), "-o", str(binary)],
                           env=environment, check=True, timeout=60)
            subprocess.run([str(binary)], env=environment, check=True, timeout=15)
            if name == "depth_probe_layer_test":
                subprocess.run([str(binary)], env=dict(environment, CS2_CAMERA_DEPTH_PROBE=str(Path(directory) / "probe.jsonl")),
                               check=True, timeout=15)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
