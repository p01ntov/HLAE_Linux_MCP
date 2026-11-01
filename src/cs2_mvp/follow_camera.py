"""Bake measured demo player motion into ordinary editable camera keys.

This is a recorded-player adapter, not an entity hook or a collision solver.
Every pose is authored on the demo's 64 Hz clock, then sampled by LiveTrack;
scrubbing backwards cannot retain a filter from a later frame. The returned
project is a draft. Applying it must use the normal camera geometry guard.
"""
from __future__ import annotations

import math
from pathlib import Path
import re

from .camera import blend, distance, look_angles
from .demo import parser
from .follow_schema import FOLLOW_OPTIONS_SCHEMA
from .demo_analysis import TICKRATE, _clean, _parsed
from .live_track import LiveTrack
from .pose_track import from_angles, slerp, to_angles

MAX_DURATION_SECONDS = 30
# A player cannot move this far in one recorded tick by normal movement. Never
# smooth a teleport/round reset into a flight through the intervening geometry.
MAX_PLAYER_STEP = 64.0


def _number(value, name, lower, upper):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not lower <= value <= upper or not math.isfinite(value)):
        raise ValueError(f"Follow {name} must be finite and within {lower}..{upper}")
    return float(value)


def _bounded_vector(value, name, limit=1_000_000):
    if not isinstance(value, (list, tuple)) or len(value) != 3:
        raise ValueError(f"Follow {name} must contain 3 numbers")
    return tuple(_number(component, name, -limit, limit) for component in value)


def _steam_id(value):
    # Floating point cannot identify uint64 players faithfully. Never repair an
    # imprecise ID, infer a slot, or select a similarly named player.
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise ValueError("Follow requires an exact nonzero decimal SteamID64")
    identity = str(value)
    if not re.fullmatch(r"[1-9][0-9]{0,19}", identity) or int(identity) > 2**64 - 1:
        raise ValueError("Follow requires an exact nonzero decimal uint64 SteamID64")
    return identity


def _interval(start_tick, end_tick):
    if any(isinstance(tick, bool) or not isinstance(tick, int) or not 0 <= tick <= 2_000_000_000
           for tick in (start_tick, end_tick)):
        raise ValueError("Follow bounds must be absolute integer demo ticks within 0..2000000000")
    if not 1 <= end_tick - start_tick <= TICKRATE * MAX_DURATION_SECONDS:
        raise ValueError(f"Follow requires a nonempty [start_tick,end_tick) interval of at most {MAX_DURATION_SECONDS} seconds")


def _options(options):
    if options is None:
        options = {}
    if not isinstance(options, dict):
        raise ValueError("Follow options must be an object")
    unknown = set(options) - set(FOLLOW_OPTIONS_SCHEMA["properties"])
    if unknown:
        raise ValueError("Unknown follow option: " + str(next(iter(unknown))))
    result = {"mode": "look_at", "offset": [-160., 0., 72.], "offset_space": "yaw",
              "look_at_offset": [0., 0., 48.], "angle_offset": [0., 0., 0.], "fov": 90.,
              "position_half_life": 0., "rotation_half_life": 0., "on_loss": "error", **options}
    for field in ("mode", "offset_space", "on_loss"):
        if result[field] not in FOLLOW_OPTIONS_SCHEMA["properties"][field]["enum"]:
            raise ValueError("Unknown follow " + field)
    for field in ("offset", "look_at_offset", "angle_offset"):
        result[field] = list(_bounded_vector(result[field], field))
    if result["mode"] == "lock_on":
        result["camera_position"] = list(_bounded_vector(result.get("camera_position"), "camera_position"))
        if any(field in options for field in ("offset", "offset_space", "position_half_life")):
            raise ValueError("lock_on uses a fixed camera_position; offset, offset_space and position_half_life do not apply")
    elif "camera_position" in options:
        raise ValueError("camera_position is only valid for lock_on")
    if result["mode"] != "attach" and "angle_offset" in options:
        raise ValueError("angle_offset is only valid for attach")
    if result["mode"] == "attach" and "look_at_offset" in options:
        raise ValueError("look_at_offset is only valid for look_at or lock_on")
    result["fov"] = _number(result["fov"], "fov", 30, 140)
    for field in ("position_half_life", "rotation_half_life"):
        result[field] = _number(result[field], field, 0, 1)
    if "name" in result and (not isinstance(result["name"], str) or not result["name"]
                             or len(result["name"].encode("utf-8")) > 256):
        raise ValueError("Follow name must contain 1..256 UTF-8 bytes")
    return result


def _rows(frame, limit):
    if len(frame) > limit:
        raise ValueError("Demo parser returned too many follow samples")
    result = _clean(frame.to_dict(orient="records"))
    if not isinstance(result, list) or any(not isinstance(row, dict) for row in result):
        raise ValueError("Demo parser returned malformed follow rows")
    return result


def _rotate(value, quaternion):
    # Quaternion XYZW; local X forward, Y left, Z up. Source positive pitch
    # points downward. This basis agrees with the portable live camera track.
    x, y, z, w = quaternion
    vx, vy, vz = value
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + y * tz - z * ty,
            vy + w * ty + z * tx - x * tz,
            vz + w * tz + x * ty - y * tx)


def _compose(first, second):
    x, y, z, w = first
    a, b, c, d = second
    return (w*a + x*d + y*c - z*b, w*b - x*c + y*d + z*a,
            w*c + x*b - y*a + z*d, w*d - x*a - y*b - z*c)


def _filter_alpha(half_life):
    return 1. if half_life == 0 else -math.expm1(-math.log(2) / (TICKRATE * half_life))


def _sample(row):
    if row.get("is_alive") is not True:
        return None, "dead" if row.get("is_alive") is False else "unknown_life_state"
    try:
        health = _number(row.get("health"), "recorded health", 0, 1_000_000)
        if health <= 0:
            return None, "dead"
        position = _bounded_vector([row.get(axis) for axis in ("X", "Y", "Z")], "recorded origin")
        pitch = _number(row.get("pitch"), "recorded pitch", -90, 90)
        yaw = _number(row.get("yaw"), "recorded yaw", -1_000_000, 1_000_000)
    except ValueError:
        return None, "invalid_pose"
    return {"position": position, "angles": (pitch, yaw, 0.)}, None


def _fingerprint(path):
    return _stat_fingerprint(path.stat())


def _stat_fingerprint(value):
    return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns


def _fingerprint_evidence(value):
    return dict(zip(("device", "inode", "size", "mtime_ns", "ctime_ns"), value))


def bake_demo_follow(demo, player, start_tick, end_tick, options=None):
    """Return ``{project, evidence}`` without writing files or calling the game.

    ``player`` is an exact SteamID64, bounds are absolute demo ticks, and the
    end is exclusive. Each recorded tick contributes one editable key. A last
    duplicate key holds that last measured pose only until the exclusive end,
    one tick later; it does not invent a target sample at the end boundary.

    Missing/dead targets or a >64-unit origin step fail the entire request by
    default. ``on_loss='truncate'`` ends just before the first bad tick, never
    resumes after a respawn or gap and never silently substitutes another actor.
    """
    _interval(start_tick, end_tick)
    identity = _steam_id(player)
    config = _options(options)
    path = Path(demo).expanduser().resolve()
    before = _fingerprint(path)
    source = _parsed(parser, path)
    roster = _rows(_parsed(source.parse_player_info), 4096)
    # Unknown/bot roster IDs are irrelevant; malformed IDs can never match.
    matches = []
    for entry in roster:
        try:
            candidate = _steam_id(entry.get("steamid"))
        except ValueError:
            continue
        if candidate == identity:
            matches.append(entry)
    if len(matches) != 1:
        raise ValueError("Follow SteamID64 must identify exactly one recorded player; inspect the demo first")
    nickname = matches[0].get("name")
    if not isinstance(nickname, str):
        nickname = identity
    name = config.get("name", f"{config['mode'].replace('_', ' ').title()}: {nickname}")
    # Roster text is untrusted metadata, not an engine command or file path.
    name = name.encode("utf-8")[:256].decode("utf-8", "ignore")
    wanted = list(range(start_tick, end_tick))
    records = _rows(_parsed(source.parse_ticks, ["X", "Y", "Z", "pitch", "yaw", "is_alive", "health"],
                            players=[int(identity)], ticks=wanted), len(wanted))
    if _fingerprint(path) != before:
        raise ValueError("Demo changed while baking follow; original project was not modified")
    by_tick = {}
    for row in records:
        tick = row.get("tick")
        if (isinstance(tick, bool) or not isinstance(tick, int) or not start_tick <= tick < end_tick
                or _steam_id(row.get("steamid")) != identity):
            raise ValueError("Demo parser returned a follow sample outside the exact player/tick request")
        if tick in by_tick:
            raise ValueError("Demo parser returned duplicate follow ticks; refusing an ambiguous target pose")
        by_tick[tick] = row
    keys = []
    loss = None
    previous_origin = previous_position = previous_rotation = None
    position_alpha = _filter_alpha(config["position_half_life"])
    rotation_alpha = _filter_alpha(config["rotation_half_life"])
    trim = from_angles(config["angle_offset"])
    for tick in wanted:
        row = by_tick.get(tick)
        sample, reason = _sample(row) if row is not None else (None, "missing")
        if sample is not None and previous_origin is not None and distance(sample["position"], previous_origin) > MAX_PLAYER_STEP:
            sample, reason = None, "discontinuity"
        if sample is None:
            loss = {"tick": tick, "reason": reason}
            if config["on_loss"] == "error" or not keys:
                raise ValueError(f"Follow target {reason} at absolute tick {tick}; no camera project generated")
            break
        origin, angles = sample["position"], sample["angles"]
        view_rotation = from_angles(angles)
        if config["mode"] == "lock_on":
            position = tuple(config["camera_position"])
        else:
            offset = config["offset"]
            if config["offset_space"] != "world":
                basis = view_rotation if config["offset_space"] == "view" else from_angles((0., angles[1], 0.))
                offset = _rotate(offset, basis)
            position = tuple(a + b for a, b in zip(origin, offset))
            if previous_position is not None:
                position = blend(previous_position, position, position_alpha)
        if config["mode"] == "attach":
            rotation = _compose(view_rotation, trim)
        else:
            target = tuple(a + b for a, b in zip(origin, config["look_at_offset"]))
            # Coincident/vertical targets have no stable yaw. Refuse rather than
            # retaining an angle that depends on the order of later seek calls.
            try:
                pitch, yaw = look_angles(position, target)
            except ValueError as error:
                raise ValueError(f"Follow aim is undefined at tick {tick}: {error}") from error
            rotation = from_angles((pitch, yaw, 0.))
        if previous_rotation is not None:
            rotation = slerp(previous_rotation, rotation, rotation_alpha)
        solved_angles = to_angles(rotation)
        _bounded_vector(position, "solved position")
        keys.append({"id": f"follow-{identity}-{tick}", "time": (tick-start_tick)/TICKRATE,
                     "position": list(position), "angles": list(solved_angles), "fov": config["fov"],
                     "easing": [0., 0., 1., 1.]})
        previous_origin, previous_position, previous_rotation = origin, position, rotation
    effective_end = loss["tick"] if loss else end_tick
    duration = (effective_end-start_tick)/TICKRATE
    # End is exclusive for demo sampling, inclusive for portable key coverage.
    last = keys[-1]
    keys.append({**last, "id": f"follow-{identity}-end-{effective_end}", "time": duration,
                 "position": list(last["position"]), "angles": list(last["angles"]),
                 "easing": list(last["easing"])})
    project = {"name": name, "start_tick": start_tick, "duration": duration,
               "interpolation": "linear", "rotation_interpolation": "slerp", "fov_interpolation": "linear",
               "render_range": {"mode": "composition", "in": 0., "out": duration}, "keys": keys}
    LiveTrack(project).validate_capture_fov()
    evidence = {
        "adapter": "recorded_demo_follow", "dynamic_entity_tracking": False,
        "status": "draft_unverified", "geometry_checked": False, "visual_verified": False,
        "requires_geometry_guard_before_apply": True,
        "source": {"demo": str(path), "size": before[2], "mtime_ns": before[3],
                   "file_identity": _fingerprint_evidence(before), "steamid": identity, "name": nickname},
        "requested": {"start_tick": start_tick, "end_tick": end_tick},
        "effective": {"start_tick": start_tick, "end_tick": effective_end},
        "tickrate": TICKRATE, "recorded_samples": len(keys)-1, "keys": len(keys), "loss": loss,
        "last_recorded_tick": effective_end-1, "terminal_hold_ticks": 1,
        "options": config,
        "limitations": [
            "Offsets reference recorded player origins and pitch/yaw; no measured eyes, bones or render-time entity interpolation.",
            "Linear key segments and optional filtering do not establish a free route, rendered visibility or frustum clearance.",
            "One key per recorded 64 Hz tick; higher output FPS interpolates these poses and does not add demo evidence.",
            "Filters start at the first requested tick; the bake is seek-order independent but changing the start tick changes filter history.",
        ],
    }
    return {"project": project, "evidence": evidence}
