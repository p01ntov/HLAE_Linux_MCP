"""Bounded, read-only demo evidence for MCP camera planning.

Ticks are always read from the demo. ``seconds`` is simulation tick / 64,
not the on-screen round timer, recording wall time, or output video time.
"""
from __future__ import annotations

from functools import lru_cache
from bisect import bisect_right
import math
from pathlib import Path

from .demo import parser

TICKRATE = 64
TIMEBASE = {"tickrate": TICKRATE, "seconds": "absolute demo simulation tick / 64; not the round clock",
            "source": "CS2 64 Hz simulation; subtick events do not increase demo tickrate"}
MAX_EVENTS = 200000


def _clean(value):
    if hasattr(value, "item"):
        value = value.item()
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {str(key): _clean(child) for key, child in value.items()}
    if isinstance(value, (list, tuple)):
        return [_clean(child) for child in value]
    return value


def _rows(frame):
    if len(frame) > MAX_EVENTS:
        raise ValueError(f"Demo event table exceeds the {MAX_EVENTS}-row analysis limit")
    return _clean(frame.to_dict(orient="records"))


def _parsed(function, *args, **kwargs):
    try:
        return function(*args, **kwargs)
    except (OSError, ValueError):
        raise
    except Exception as error:
        raise ValueError(f"Demo parser could not read this recording: {error}") from error


def _steam(value):
    return str(value) if value not in (None, "", 0, "0") else None


def _position(row, prefix=""):
    values = [row.get(prefix + axis) for axis in ("X", "Y", "Z")]
    return values if all(isinstance(v, (int, float)) and math.isfinite(v) for v in values) else None


def _person(row, prefix):
    return {"name": row.get(prefix + "name"), "steamid": _steam(row.get(prefix + "steamid")),
            "position": _position(row, prefix), "team": row.get(prefix + "team_name")}


def _round(row):
    completed = row.get("total_rounds_played")
    return int(completed) + 1 if isinstance(completed, (int, float)) and completed >= 0 else None


def _tick(row):
    value = row.get("tick")
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or int(value) != value or value < 0:
        raise ValueError("Demo parser returned an invalid event tick; refusing to guess")
    return int(value)


@lru_cache(maxsize=2)
def _index(path, size, modified):
    # Fingerprint keys invalidate the cache after a demo is replaced or edited.
    demo = _parsed(parser, Path(path))
    players = _rows(_parsed(demo.parse_player_info))
    for player in players:
        player["steamid"] = _steam(player.get("steamid"))
    deaths = _rows(_parsed(demo.parse_event, "player_death", player=["X", "Y", "Z", "team_name"],
                          other=["total_rounds_played", "is_warmup_period"]))
    starts = _rows(_parsed(demo.parse_event, "round_start", other=["total_rounds_played", "is_warmup_period"]))
    kills = []
    for row in deaths:
        tick = _tick(row)
        attacker, victim = _person(row, "attacker_"), _person(row, "user_")
        valid_teams = {attacker["team"], victim["team"]} == {"CT", "TERRORIST"}
        kills.append({"tick": tick, "seconds": tick / TICKRATE, "round": _round(row),
                      "warmup": row.get("is_warmup_period"), "attacker": attacker, "victim": victim,
                      "weapon": row.get("weapon"), "headshot": row.get("headshot"),
                      "noscope": row.get("noscope"), "through_smoke": row.get("thrusmoke"),
                      "penetrated": row.get("penetrated"),
                      "enemy_kill": bool(attacker["steamid"] and attacker["steamid"] != victim["steamid"] and valid_teams)})
    rounds = [{"round": _round(row), "start_tick": _tick(row), "start_seconds": _tick(row) / TICKRATE,
               "warmup": row.get("is_warmup_period")} for row in starts]
    rounds.sort(key=lambda row: row["start_tick"])
    round_ticks = [row["start_tick"] for row in rounds]
    for kill in kills:
        location = bisect_right(round_ticks, kill["tick"]) - 1
        start = rounds[location] if location >= 0 else None
        kill["round_start_tick"] = start["start_tick"] if start and start["round"] == kill["round"] else None
    return {"path": path, "header": _clean(_parsed(demo.parse_header)), "players": players,
            "rounds": rounds,
            "kills": sorted(kills, key=lambda row: row["tick"])}


def _load(path):
    path = Path(path).expanduser().resolve()
    info = path.stat()
    return _index(str(path), info.st_size, info.st_mtime_ns)


def _identity(index, identity):
    if identity is None:
        return None
    matches = [p for p in index["players"] if p["steamid"] == identity or str(p.get("name", "")).casefold() == identity.casefold()]
    if len(matches) != 1:
        raise ValueError("Player must match exactly one full nickname or SteamID64; inspect the demo and use SteamID64 for duplicate names")
    if matches[0]["steamid"] is None:
        raise ValueError("This player has no recorded SteamID64; exact per-player queries are unavailable for that identity")
    return matches[0]


def inspect(path):
    index = _load(path)
    kills = index["kills"]
    weapons = sorted({k["weapon"] for k in kills if isinstance(k["weapon"], str)})
    return {"path": index["path"], "header": index["header"], "timebase": TIMEBASE,
            "players": index["players"][:128], "players_total": len(index["players"]),
            "rounds": index["rounds"][:256], "rounds_total": len(index["rounds"]),
            "weapons": weapons[:256], "weapons_total": len(weapons),
            "death_events": len(kills), "enemy_kills": sum(k["enemy_kill"] for k in kills),
            "observed_event_ticks": {"first": min([r["start_tick"] for r in index["rounds"]] + [k["tick"] for k in kills], default=None),
                                     "last": max([r["start_tick"] for r in index["rounds"]] + [k["tick"] for k in kills], default=None)},
            "notes": ["Observed event bounds are not the demo end tick.",
                      "round = event total_rounds_played + 1; warmup/restarts may repeat round numbers.",
                      "Player-info team_number is roster metadata; use per-event or per-tick team for the moment being reviewed.",
                      "Positions are player origins, not eye/bone positions or collision-tested camera locations."]}


def _interval(query):
    ticks = (query.get("start_tick"), query.get("end_tick"))
    seconds = (query.get("start_seconds"), query.get("end_seconds"))
    if any(value is not None for value in ticks) and any(value is not None for value in seconds):
        raise ValueError("Choose tick bounds or seconds bounds, not both")
    bounds = seconds if any(value is not None for value in seconds) else ticks
    if any(value is not None for value in bounds) and not all(value is not None for value in bounds):
        raise ValueError("Search start and end must be provided together")
    if bounds[0] is not None and bounds[1] <= bounds[0]:
        raise ValueError("Search end must be greater than start (the end bound is exclusive)")
    return (seconds[0] * TICKRATE, seconds[1] * TICKRATE) if seconds[0] is not None else ticks


def find_moments(path, **query):
    lower, upper = _interval(query)  # Reject contradictory queries before parsing.
    if query.get("min_kills", 1) > 1 and query.get("role", "attacker") != "attacker":
        raise ValueError("Multikill searches require role=attacker")
    index = _load(path)
    player = _identity(index, query.get("player"))
    role = query.get("role", "attacker")
    weapons = {weapon.casefold().removeprefix("weapon_") for weapon in query.get("weapons", [])}
    selected = []
    for kill in index["kills"]:
        if not query.get("include_warmup", False) and kill["warmup"] is not False:
            continue  # Unknown warmup status must not masquerade as match evidence.
        if not query.get("include_non_enemy", False) and not kill["enemy_kill"]:
            continue
        if lower is not None and not lower <= kill["tick"] < upper:
            continue
        if "round" in query and kill["round"] != query["round"]:
            continue
        if weapons and str(kill["weapon"]).casefold().removeprefix("weapon_") not in weapons:
            continue
        if "headshot" in query and kill["headshot"] != query["headshot"]:
            continue
        roles = ("attacker", "victim") if role == "either" else (role,)
        if player and not any(kill[side]["steamid"] == player["steamid"] for side in roles):
            continue
        selected.append(kill)
    minimum = query.get("min_kills", 1)
    groups = [[kill] for kill in selected]
    if minimum > 1:
        by_player = {}
        for kill in selected:
            if kill["round"] is None:
                continue
            by_player.setdefault((kill["attacker"]["steamid"], kill["round"], kill["round_start_tick"]), []).append(kill)
        groups = []
        width = query.get("window_seconds", 10) * TICKRATE
        for kills in by_player.values():
            start = 0
            while start < len(kills):
                end = start + 1
                while end < len(kills) and kills[end]["tick"] - kills[start]["tick"] <= width:
                    end += 1
                if end - start >= minimum:
                    groups.append(kills[start:end])
                    start = end
                else:
                    start += 1
    groups.sort(key=lambda group: group[0]["tick"])
    offset, limit = query.get("offset", 0), query.get("limit", 10)
    moments = [{"start_tick": group[0]["tick"], "end_tick": group[-1]["tick"],
                "start_seconds": group[0]["seconds"], "end_seconds": group[-1]["seconds"],
                "round": group[0]["round"], "kill_count": len(group), "kills": group[:32],
                "kills_truncated": len(group) > 32} for group in groups[offset:offset + limit]]
    return {"path": index["path"], "timebase": TIMEBASE, "query": query, "player": player,
            "matched_kills": len(selected), "matched_moments": len(groups), "offset": offset,
            "moments": moments, "next_offset": offset + len(moments) if offset + len(moments) < len(groups) else None,
            "grouping": "Non-overlapping same-attacker, same-round groups; last-first <= window_seconds. Filters apply before counting.",
            "notes": ["Event evidence only: no automatic claim of a clutch, jump shot, visibility or camera clearance.",
                      "A moment end_tick is the last event tick, not an exclusive render boundary; add pre/post-roll before capture."]}


def player_positions(path, player, ticks):
    index = _load(path)
    identity = _identity(index, player)
    wanted = sorted(set(ticks))
    demo = _parsed(parser, Path(index["path"]))
    rows = _rows(_parsed(demo.parse_ticks, ["X", "Y", "Z", "pitch", "yaw", "is_alive", "team_name", "health"],
                         players=[int(identity["steamid"])], ticks=wanted))
    samples = []
    for row in rows:
        tick = _tick(row)
        if tick not in wanted or _steam(row.get("steamid")) != identity["steamid"]:
            raise ValueError("Demo parser returned a sample outside the requested player/ticks")
        samples.append({"tick": tick, "seconds": tick / TICKRATE, "position": _position(row),
                        "angles": [row.get("pitch"), row.get("yaw")], "alive": row.get("is_alive"),
                        "health": row.get("health"), "team": row.get("team_name")})
    return {"path": index["path"], "timebase": TIMEBASE, "player": identity,
            "samples": sorted(samples, key=lambda sample: sample["tick"]),
            "missing_ticks": sorted(set(wanted) - {sample["tick"] for sample in samples}),
            "notes": ["Player origin XYZ and view pitch/yaw; not eye height, bones, a collision trace, or a solved camera pose. Missing samples are never interpolated."]}
