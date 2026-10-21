from __future__ import annotations

import argparse
import json
from pathlib import Path
import signal
import sys

from .demo import inspect_demo
from .discovery import doctor, ensure_plugin


def main() -> int:
    cli = argparse.ArgumentParser(description="Native Linux CS2 demo → ticks → POV → MP4")
    commands = cli.add_subparsers(dest="command", required=True)
    check = commands.add_parser("doctor", help="Inspect CS2, Steam, runtime and available demos")
    check.add_argument("--game-dir")
    commands.add_parser("bootstrap", help="Download and checksum the pinned CSDM Linux plugin")
    finalize = commands.add_parser("finalize", aliases=["finish"], help="Retry encoding an existing captured job without launching CS2")
    finalize.add_argument("run", type=Path)
    finalize.add_argument("--keep-raw", action="store_true")
    inspect = commands.add_parser("inspect", help="List demo header and players; optionally kill events")
    inspect.add_argument("demo", type=Path)
    inspect.add_argument("--kills", action="store_true")
    compose = commands.add_parser("compose", help="Fit a fixed elevated camera to two world-space model bounds")
    compose.add_argument("spec", type=Path)
    compose.add_argument("--output", type=Path, help="Save a new static camera JSON; never overwrite an existing file")
    render = commands.add_parser("render", help="Record the requested tick range and first-person player POV")
    render.add_argument("demo", type=Path)
    render.add_argument("--player", help="Exact nickname or SteamID64 for first-person POV")
    render.add_argument("--camera", type=Path, help="JSON cinematic camera plan; uses the native motion extension")
    render.add_argument("--start", type=int, help="Absolute start tick; omit with --end to use a live project saved range")
    render.add_argument("--end", type=int, help="Absolute exclusive end tick; must be paired with --start")
    render.add_argument("--render-scope", choices=("composition", "region"), help="Override a live project saved range mode; cannot be combined with explicit ticks")
    render.add_argument("--output", type=Path)
    render.add_argument("--game-dir")
    render.add_argument("--fps", type=int, default=30, help="Encoded output FPS, 15..1000 (default: 30)")
    render.add_argument("--capture-fps", type=int, help="Native fixed-step samples per demo second, up to 1000; use --keep-raw to retain them for slow motion")
    render.add_argument("--slow-motion", action="store_true", help="Keep every native sample and play it at --fps; audio is muted")
    render.add_argument("--mute", dest="audio", action="store_false", help="Encode video without audio")
    render.add_argument("--width", type=int, default=1280)
    render.add_argument("--height", type=int, default=720)
    render.add_argument("--timeout", type=float, default=600)
    render.add_argument("--keep-raw", action="store_true")
    render.add_argument("--dry-run", action="store_true")
    args = vars(cli.parse_args())
    command = args.pop("command")

    def interrupt(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    try:
        if command == "doctor":
            result = doctor(args["game_dir"])
        elif command == "bootstrap":
            result = {"plugin": str(ensure_plugin())}
        elif command == "inspect":
            result = inspect_demo(args["demo"], args["kills"])
        elif command == "compose":
            from .composition import compose_pair
            result = compose_pair(json.loads(args["spec"].expanduser().read_text()))
            if args["output"]:
                with args["output"].expanduser().open("x") as handle:
                    json.dump(result, handle, ensure_ascii=False, indent=2)
                    handle.write("\n")
        elif command in {"finalize", "finish"}:
            from .render import finish_capture
            result = finish_capture(args["run"], keep_raw=args["keep_raw"])
        else:
            from .render import render_clip
            result = render_clip(**args)
        print(json.dumps(result, ensure_ascii=False, indent=2, default=str))
        return 0
    except KeyboardInterrupt:
        print("Cancelled; managed game stopped and restoration attempted. Check the job manifest.", file=sys.stderr)
        return 130
    except Exception as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
