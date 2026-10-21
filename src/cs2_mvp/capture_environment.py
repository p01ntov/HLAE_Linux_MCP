"""Internal, whitelisted native capture actions and actual readback validation."""
from __future__ import annotations

import copy
import json
import math
from pathlib import Path
import re
import struct

from .appearance import model_resource


def validate_environment(spec, *, allow_experimental_appearance=False, allow_glove_capture=False):
    # Reuse the connector's explicit authoring schema. Engine availability and
    # tighter runtime limits are checked by the native capture adapter.
    from .live_mcp import SCENE, APPEARANCE, COSMETICS, PRESENTATION, ProtocolError, validate
    cosmetics=spec.get("cosmetics")
    presentation=spec.get("presentation")
    try:
        if cosmetics is not None:
            validate(cosmetics,COSMETICS,"cosmetics")
        if presentation is not None:
            validate(presentation,PRESENTATION,"presentation")
    except ProtocolError as error:
        raise ValueError(error.message) from error
    if cosmetics and cosmetics.get("gloves") and not allow_glove_capture:
        raise ValueError("Glove capture is unavailable until the current native capture adapter has verified glove application and restoration. Reset gloves before export; the saved project retains the requested overrides.")
    active_cosmetics = bool(cosmetics and (cosmetics.get("players") or cosmetics.get("weapon_paints") or cosmetics.get("gloves") or cosmetics.get("team_palette",{}).get("enabled")))
    if presentation and (presentation.get("view")=="player" or presentation.get("hud")=="gameplay"):
        raise ValueError("Player view and gameplay HUD capture are unavailable: the offline presentation adapter is not verified. Choose free view with demo or hidden HUD before export; the saved project retains the requested settings.")
    if presentation and "hud" in presentation:
        scene=spec.get("scene")
        if scene is not None and not isinstance(scene,dict):
            raise ValueError("scene must be an object")
        spec={**spec,"scene":{**(scene or {}),"hud_visible":presentation["hud"]!="hidden"}}
    result = {}
    if active_cosmetics:
        if spec.get("appearance"):
            raise ValueError("Reset the global appearance pack before capturing per-player cosmetics")
        result["cosmetics"] = cosmetics
    for field, schema in (("scene", SCENE), ("appearance", APPEARANCE)):
        value = spec.get(field)
        if value is None or value == {}:
            continue
        try:
            validate(value, schema, field)
        except ProtocolError as error:
            raise ValueError(error.message) from error
        if field == "appearance":
            path = Path(value["path"]).expanduser()
            if not path.is_absolute() or not path.is_dir():
                raise ValueError("Appearance pack must be an existing absolute directory")
            models = value["models"]
            for model in models:
                model_resource(model + "_c")
                if model.split("/", 1)[0] not in {"models", "characters", "agents", "weapons"}:
                    raise ValueError("Appearance model uses an unsupported resource root")
            if len(set(models)) != len(models):
                raise ValueError("Appearance model list contains duplicates")
            value = {"path": str(path.resolve()), "models": list(models)}
            if not allow_experimental_appearance:
                raise ValueError("Appearance capture is unavailable: this CS2 build has no verified safe model refresh adapter. Reset appearance and export the scene without a model pack.")
        result[field] = value
    return copy.deepcopy(result)


def build_environment_sequence(sequence, environment):
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
    for name in ("scene", "appearance", "cosmetics"):
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
    if not environment:
        return {"required": False}
    if "CS2_MVP_ENVIRONMENT_ABI 1" not in log or "CS2_MVP_ENVIRONMENT_ERROR" in log:
        raise RuntimeError("Native environment ABI 1 was not confirmed, or the adapter reported an error")
    result = {"abi": 1, "required": True}
    for field, suffix in (("scene", "settings"), ("appearance", "state")):
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
        elif (not isinstance(actual, dict) or actual.get("mounted_verified") is not True
              or actual.get("reload_requested") is not True or actual.get("active") is not True
              or actual.get("path") != requested["path"] or actual.get("models") != requested["models"]):
            raise RuntimeError("Native appearance mount/reload readback differs from the requested pack")
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
    if requested.get("gloves"):
        for phase in ("POSE", "RENDER"):
            abi = list(re.finditer(rf"^CS2_MVP_GLOVES_{phase}_ABI ([^\r\n]*)$", log, re.MULTILINE))
            if len(abi) != 1 or abi[0][1] != "1" or abi[0].start() >= records[0].start():
                raise RuntimeError(f"Native glove {phase.lower()} ABI 1 was not confirmed before cosmetic readback")
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
        for target in requested.get("weapon_paints", []):
            weapons = proof["weapon_cosmetics"]
            if weapons.get("stale") is not False or weapons.get("restore_pending") or weapons.get("error"):
                raise ValueError("weapon state is stale")
            matches = [row for row in weapons["weapons"] if row.get("steam_id") == target["steam_id"] and row.get("item_def") == target["item_def"]]
            if not matches:
                raise ValueError("selected owned weapon is absent")
            for row in matches:
                if row.get("ready") is not True or row.get("applied") is not True or row.get("materials_pending") is not False or row.get("error"):
                    raise ValueError("weapon paint or material refresh was not confirmed")
                jobs = row.get("material_jobs", {})
                if type(jobs.get("world")) is not int or type(jobs.get("hud")) is not int or jobs["world"] != 0 or jobs["hud"] != 0:
                    raise ValueError("weapon material jobs are incomplete")
                if row.get("paint_kit") != target["paint_kit"] or ("seed" in target and row.get("seed") != target["seed"]) or ("wear" in target and not _same(row.get("wear"), target["wear"])):
                    raise ValueError("weapon finish readback differs")
        if requested.get("gloves"):
            gloves = proof["glove_cosmetics"]
            if gloves.get("stale") is not False or gloves.get("restoration_unresolved") is not False or gloves.get("restoration_pending", False) or gloves.get("error"):
                raise ValueError("glove state is stale or has unresolved restoration")
            glove_rows = gloves["gloves"]
            if not isinstance(glove_rows, list):
                raise ValueError("invalid glove rows")
            for target in requested["gloves"]:
                matches = [row for row in glove_rows if row.get("steam_id") == target["steam_id"]]
                if len(matches) != 1:
                    raise ValueError("selected glove pawn is absent or ambiguous")
                row = matches[0]
                if (any(row.get(key) is not True for key in ("ready", "item_readable", "applied", "world_ready", "hud_ready", "world_pose_ready", "hud_pose_ready", "world_render_ready"))
                        or any(row.get(key) is not False for key in ("materials_pending", "needs_refresh", "restoration_unresolved")) or row.get("error")):
                    raise ValueError("glove item and material refresh were not confirmed")
                required = row.get("hud_render_required")
                enabled = row.get("hud_parent_render_enabled")
                hud_render_ready = row.get("hud_render_ready")
                if (type(required) is not bool or type(enabled) is not bool or required != enabled
                        or type(hud_render_ready) is not bool or (required and not hud_render_ready)):
                    raise ValueError("glove HUD render scope and publication were not confirmed")
                bones = row.get("pose_bones", {})
                if any(type(bones.get(key)) is not int or not 2 <= bones[key] <= 512 for key in ("world", "hud")):
                    raise ValueError("glove bone poses were not confirmed")
                jobs = row.get("material_jobs", {})
                if any(type(jobs.get(key)) is not int or jobs[key] != 0 for key in ("world", "hud")):
                    raise ValueError("glove material jobs are incomplete")
                if row.get("item_def") != target["item_def"] or row.get("paint_kit") != target["paint_kit"] or not _same(row.get("seed"), target.get("seed", 0)) or ("wear" in target and not _same(row.get("wear"), target["wear"])):
                    raise ValueError("glove finish readback differs")
                item = row["actual_item"]
                if item["m_iItemDefinitionIndex"] != target["item_def"] or type(item["m_bInitialized"]) is not int or item["m_bInitialized"] != 1:
                    raise ValueError("observed glove item definition differs")
                attributes = {}
                for attribute in item["attributes"]:
                    definition = attribute["definition"]
                    if type(definition) is not int:
                        raise ValueError("invalid observed glove attribute definition")
                    if definition not in (6, 7, 8):
                        continue
                    bits = attribute["value_bits"]
                    if definition in attributes or type(bits) is not int or not 0 <= bits <= 0xffffffff:
                        raise ValueError("invalid observed glove texture attribute")
                    attributes[definition] = struct.unpack("<f", struct.pack("<I", bits))[0]
                if (set(attributes) != {6, 7, 8} or not _same(attributes[6], target["paint_kit"])
                        or not _same(attributes[7], target.get("seed", 0)) or not _same(attributes[8], row.get("wear"))):
                    raise ValueError("observed glove texture attributes differ")
    except (AttributeError, KeyError, TypeError, ValueError) as error:
        raise RuntimeError(f"Invalid native cosmetic readback: {error}") from error
    result = {"setup_tick": int(records[0][1]), "readback": proof, "render_verified": False}
    if capture_end is not None:
        restored = list(re.finditer(r"^CS2_MVP_COSMETICS_RESTORED tick=(\d+) players_owned=(\d+) weapons_owned=(\d+)(?: gloves_owned=(\d+))?[ \t]*$", log, re.MULTILINE))
        restored = [match for match in restored if int(match[1]) >= capture_end and int(match[2]) == 0 and int(match[3]) == 0
                    and (match[4] is None and not requested.get("gloves") or match[4] == "0")]
        final = list(re.finditer(r"CS2_MVP_ENVIRONMENT_RESTORED tick=(\d+) reason=quit_after_flush\b", log))
        if not restored or not final or restored[-1].start() >= final[-1].start():
            raise RuntimeError("Native cosmetic restoration was not confirmed after the final movie flush")
        result["restored"] = {"tick": int(restored[-1][1]), "players_owned": 0, "weapons_owned": 0}
        if restored[-1][4] is not None:
            result["restored"]["gloves_owned"] = 0
    return result
