"""Generate a recorded-player follow draft without launching or editing the game."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import tempfile
from .follow_camera import bake_demo_follow

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("demo",type=Path)
    parser.add_argument("--player",required=True,help="Exact decimal SteamID64")
    parser.add_argument("--start",type=int,required=True)
    parser.add_argument("--end",type=int,required=True,help="Exclusive absolute demo tick")
    parser.add_argument("--mode",choices=["look_at","attach","lock_on"],default="look_at")
    parser.add_argument("--offset",type=float,nargs=3)
    parser.add_argument("--offset-space",choices=["world","yaw","view"])
    parser.add_argument("--camera-position",type=float,nargs=3)
    parser.add_argument("--look-at-offset",type=float,nargs=3)
    parser.add_argument("--angle-offset",type=float,nargs=3)
    parser.add_argument("--fov",type=float,default=90)
    parser.add_argument("--position-half-life",type=float)
    parser.add_argument("--rotation-half-life",type=float)
    parser.add_argument("--on-loss",choices=["error","truncate"],default="error")
    parser.add_argument("--output",type=Path,required=True,help="New project JSON; existing files are never overwritten")
    args=parser.parse_args(argv)
    options={name:getattr(args,name) for name in ("mode","offset","offset_space","camera_position","look_at_offset","angle_offset","fov","position_half_life","rotation_half_life","on_loss") if getattr(args,name) is not None}
    result=bake_demo_follow(args.demo,args.player,args.start,args.end,options)
    output=args.output.expanduser().absolute();output.parent.mkdir(parents=True,exist_ok=True)
    # Portable project includes provenance as metadata; import still uses native validation.
    project={**result["project"],"follow_evidence":result["evidence"]}
    descriptor,temporary=tempfile.mkstemp(prefix=".follow-",suffix=".json",dir=output.parent)
    try:
        with os.fdopen(descriptor,"w") as stream:
            json.dump(project,stream,ensure_ascii=False,allow_nan=False,indent=2);stream.write("\n");stream.flush();os.fsync(stream.fileno())
        os.link(temporary,output)
    finally:
        Path(temporary).unlink(missing_ok=True)
    print(json.dumps({"output":str(output),"draft_unverified":True,"keys":len(project["keys"]),"evidence":result["evidence"]},ensure_ascii=False))
    return 0

if __name__=="__main__":
    raise SystemExit(main())
