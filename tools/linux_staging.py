#!/usr/bin/env python3
"""Run a build with Linux symlink semantics at a workspace path on NTFS.

Requires unprivileged user/mount namespaces and util-linux. The tmpfs mount
exists only in the child namespace, never in the desktop's mount namespace.
Write final artifacts outside --mount: staging disappears when the child exits.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mount", type=Path, required=True)
    parser.add_argument("--size", default="3G", help="tmpfs upper bound, allocated on demand")
    parser.add_argument("--inside-namespace", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("pass a command after --")
    mount = args.mount.expanduser().resolve()
    workspace = Path(__file__).resolve().parents[1]
    if workspace.is_relative_to(mount) or not mount.is_relative_to(workspace.parent):
        parser.error("--mount must be a staging subdirectory inside the project workspace")
    if not args.inside_namespace:
        mount.mkdir(parents=True, exist_ok=True)
        return subprocess.call(["unshare", "--user", "--map-root-user", "--mount", "--propagation", "private",
                                sys.executable, str(Path(__file__).resolve()), "--inside-namespace",
                                "--mount", str(mount), "--size", args.size, "--", *command])
    subprocess.run(["mount", "-t", "tmpfs", "-o", f"size={args.size},mode=700", "tmpfs", str(mount)], check=True)
    return subprocess.call(command, env=os.environ.copy())


if __name__ == "__main__":
    raise SystemExit(main())
