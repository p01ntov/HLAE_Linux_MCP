"""Internal, whitelisted native capture actions and actual readback validation."""
from __future__ import annotations

import copy
import json
import math
import re


def validate_environment(spec):
    # The current native build carries scene, agent and team-palette adapters.
    # Removed skin fields are rejected even when empty; never drop old requests.
    from .live_mcp import SCENE, COSMETICS, PRESENTATION, ProtocolError, reject_removed_cosmetics, validate
    cosmetics=spec.get("cosmetics")
    presentation=spec.get("presentation")
    try:
        reject_removed_cosmetics(spec)
        if cosmetics is not None:
            validate(cosmetics,COSMETICS,"cosmetics")
        if presentation is not None:
            validate(presentation,PRESENTATION,"presentation")
    except ProtocolError as error:
        raise ValueError(error.message) from error
    active_cosmetics = bool(cosmetics and (cosmetics.get("players") or cosmetics.get("team_palette",{}).get("enabled")))
    if presentation and (presentation.get("view")=="player" or presentation.get("hud")=="gameplay"):
        raise ValueError("Player view and gameplay HUD capture are unavailable: the offline presentation adapter is not verified. Choose free view with demo or hidden HUD before export; the saved project retains the requested settings.")
    if presentation and "hud" in presentation:
        scene=spec.get("scene")
        if scene is not None and not isinstance(scene,dict):
            raise ValueError("scene must be an object")
        spec={**spec,"scene":{**(scene or {}),"hud_visible":presentation["hud"]!="hidden"}}
    result = {"cosmetics":cosmetics} if active_cosmetics else {}
    scene=spec.get("scene")
    if scene is not None and scene != {}:
        try:
            validate(scene,SCENE,"scene")
        except ProtocolError as error:
            raise ValueError(error.message) from error
        result["scene"]=scene
    return copy.deepcopy(result)


def build_environment_sequence(sequence, environment):
    environment = validate_environment(environment)
    result = copy.deepcopy(sequence)
    if not environment:
        return result
    actions = result["actions"]
    starts = [action["tick"] for action in actions if action.get("cmd", "").startswith("startmovie ")]
    if len(starts) != 1:
        raise ValueError("Environment capture requires one startmovie action")
    setup = max(96, starts[0] - 64)
    if not any(action.get("cmd") == "sv_cheats 1" and action["tick"] <= setup for action in actions):
        raise ValueError("Environment capture requires sv_cheats setup before its internal actions")
    for name in ("scene", "cosmetics"):
        if environment.get(name):
            encoded = json.dumps(environment[name], ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode().hex()
            actions.append({"tick": setup, "cmd": "cs2_mvp_" + name + " " + encoded})
    result["actions"] = sorted(actions, key=lambda action: action["tick"])
    return result


def _same(actual, expected):
    if isinstance(expected, bool):
        return isinstance(actual, bool) and actual == expected
    if isinstance(expected, (int, float)):
        return not isinstance(actual, bool) and isinstance(actual, (int, float)) and math.isfinite(actual) and math.isclose(actual, expected, abs_tol=1e-5, rel_tol=1e-5)
    return actual == expected


def validate_environment_log(log, environment, capture_start, capture_end=None):
    environment = validate_environment(environment)
    if not environment:
        return {"required": False}
    if "CS2_MVP_ENVIRONMENT_ABI 1" not in log or "CS2_MVP_ENVIRONMENT_ERROR" in log:
        raise RuntimeError("Native environment ABI 1 was not confirmed, or the adapter reported an error")
    result = {"abi": 1, "required": True}
    for field, suffix in (("scene", "settings"),):
        if field not in environment:
            continue
        pattern = rf"CS2_MVP_{field.upper()}_APPLIED tick=(\d+) {suffix}=(\{{[^\r\n]*\}})"
        records = re.findall(pattern, log)
        if len(records) != 1 or int(records[0][0]) >= capture_start:
            raise RuntimeError(f"Native {field} was not applied once before recording")
        try:
            actual = json.loads(records[0][1])
        except ValueError as error:
            raise RuntimeError(f"Invalid native {field} readback record") from error
        requested = environment[field]
        if field == "scene":
            if not isinstance(actual, dict) or set(actual) != set(requested) or any(not _same(actual[key], value) for key, value in requested.items()):
                raise RuntimeError("Native scene readback differs from the requested settings")
        result[field] = {"setup_tick": int(records[0][0]), "readback": actual,
                         "render_verified": False}
    if environment.get("cosmetics"):
        result["cosmetics"] = _validate_cosmetics_log(log, environment["cosmetics"], capture_start, capture_end)
    if capture_end is not None:
        restored = [(int(tick), reason) for tick, reason in re.findall(r"CS2_MVP_ENVIRONMENT_RESTORED tick=(\d+) reason=(\w+)", log)
                    if int(tick) >= capture_end]
        if not restored:
            raise RuntimeError("Native capture environment restoration was not confirmed after recording")
        result["restored"] = {"tick": restored[-1][0], "reason": restored[-1][1]}
    return result


def _validate_cosmetics_log(log, requested, capture_start, capture_end):
    records = list(re.finditer(r"CS2_MVP_COSMETICS_APPLIED tick=(\d+) proof=(\{[^\r\n]*\})", log))
    if len(records) != 1 or int(records[0][1]) >= capture_start:
        raise RuntimeError("Native cosmetics were not confirmed once before recording")
    starts = list(re.finditer(r"Executing:\s*startmovie\b", log))
    if len(starts) != 1 or records[0].start() >= starts[0].start():
        raise RuntimeError("Native cosmetic proof must precede the actual startmovie action")
    try:
        proof = json.loads(records[0][2])
        if proof["settings"] != requested:
            raise ValueError("requested settings differ")
        rows = []
        if requested.get("players") or requested.get("team_palette", {}).get("enabled"):
            players = proof["players"]
            if (players.get("stale") is not False or players.get("needs_demo_refresh") is not False
                    or players.get("restoration_pending", False) or players.get("error")):
                raise ValueError("player state is stale or needs recovery")
            rows = players["players"]
            if not isinstance(rows, list):
                raise ValueError("invalid player rows")
        for target in requested.get("players", []):
            matches = [row for row in rows if row.get("steam_id") == target["steam_id"]]
            if len(matches) != 1 or any(row.get("agent_applied") is not True or row.get("model_ready") is not True or row.get("error") for row in matches):
                raise ValueError("selected agent binding was not confirmed")
            if matches[0].get("agent_item_def") != target["agent_item_def"]:
                raise ValueError("agent item definition differs")
        if requested.get("team_palette", {}).get("enabled"):
            if not rows or players.get("team_palette") != requested["team_palette"] or any(row.get("palette_applied") is not True or row.get("color_ready") is not True or row.get("error") for row in rows):
                raise ValueError("team color bindings were not confirmed")
    except (AttributeError, KeyError, TypeError, ValueError) as error:
        raise RuntimeError(f"Invalid native cosmetic readback: {error}") from error
    result = {"setup_tick": int(records[0][1]), "readback": proof, "render_verified": False}
    if capture_end is not None:
        restored = list(re.finditer(r"^CS2_MVP_COSMETICS_RESTORED tick=(\d+) players_owned=(\d+) weapons_owned=(\d+)(?: gloves_owned=(\d+))?[ \t]*$", log, re.MULTILINE))
        restored = [match for match in restored if int(match[1]) >= capture_end and int(match[2]) == 0 and int(match[3]) == 0
                    and (match[4] is None or match[4] == "0")]
        final = list(re.finditer(r"CS2_MVP_ENVIRONMENT_RESTORED tick=(\d+) reason=quit_after_flush\b", log))
        if not restored or not final or restored[-1].start() >= final[-1].start():
            raise RuntimeError("Native cosmetic restoration was not confirmed after the final movie flush")
        result["restored"] = {"tick": int(restored[-1][1]), "players_owned": 0, "weapons_owned": 0}
        if restored[-1][4] is not None:
            result["restored"]["gloves_owned"] = 0
    return result
