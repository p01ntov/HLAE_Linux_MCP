"""Fail-closed MCP camera authoring with native frame-atomic compare-and-set."""
from __future__ import annotations

import copy
import json
import math
import uuid

from .camera_collision import DEFAULT_CLEARANCE, GeometryError, load_geometry, project_cubics
from .live import LiveError, session_descriptor

SPATIAL_METHODS = frozenset({"set_camera", "add_key", "update_key", "remove_key", "import_project",
                             "apply_preset", "scrub", "preview", "request_export", "undo", "redo"})
SPATIAL_PROJECT_FIELDS = frozenset({"keys", "interpolation", "rotation_interpolation",
                                    "fov_interpolation", "duration", "start_tick"})


def _unavailable(reason):
    return {"status": "unavailable", "spatial_mutations_allowed": False,
            "static_geometry_available": False, "reason": str(reason),
            "frustum_checked": False, "render_visibility_checked": False}


def geometry_for_demo(demo, game_dir=None, cache_root=None):
    from .map_geometry import load_for_demo
    if cache_root is None:
        from .project_storage import geometry_directory
        cache_root = geometry_directory()
    if cache_root is None:
        raise GeometryError("No map geometry cache configured; set geometry_directory or CS2_CAMERA_GEOMETRY_CACHE")
    return load_geometry(load_for_demo(demo, game_dir=game_dir, cache_root=cache_root))


def session_geometry():
    descriptor = session_descriptor()
    demo = descriptor.get("demo")
    if not demo:
        raise GeometryError("Session descriptor does not identify its demo")
    return geometry_for_demo(demo, descriptor.get("game_directory"), descriptor.get("geometry_cache"))


def map_status(demo=None, game_dir=None):
    try:
        geometry = geometry_for_demo(demo, game_dir) if demo is not None else session_geometry()
        return geometry.status()
    except (OSError, ValueError, LiveError, ImportError) as error:
        return _unavailable(error)


def preview_for_demo(demo, game_dir=None):
    """Actual plane-sliced map context; partial context grants no authoring pass."""
    try:
        return geometry_for_demo(demo, game_dir).preview()
    except (OSError, ValueError, LiveError, ImportError) as error:
        return {"map": "", "status": "unavailable", "reason": str(error).encode("utf-8")[:1024].decode("utf-8", "ignore"),
                "bounds": [0, 0, 1, 1], "layers": [], "source_sha256": ""}


def decorate_state(state):
    result = dict(state)
    result["geometry_guard"] = {**map_status(), "native_compare_and_set_available": state.get("guarded_mutation_abi") == 1,
                                "current_project_validated": False,
                                "policy": "Mandatory indexed 3D collision preflight; unknown coverage rejects spatial MCP mutations"}
    return result


def validate_camera_path(client, *, project=None, position=None, clearance=DEFAULT_CLEARANCE, look_at=None,
                         demo=None, game_dir=None):
    if project is not None and position is not None:
        raise GeometryError("Provide project or position, not both")
    if look_at is not None and position is None:
        raise GeometryError("look_at requires an explicit camera position")
    try:
        geometry = geometry_for_demo(demo, game_dir) if demo is not None else session_geometry()
    except (OSError, ValueError, LiveError, ImportError) as error:
        return {"passed": False, "status": "unknown", "complete": False, "static_clear": False,
                "geometry": _unavailable(error),
                "frustum_checked": False, "render_visibility_checked": False}
    if position is not None:
        result = geometry.validate_position(position, clearance)
        if look_at is not None:
            sight = geometry.validate_segment(position, look_at, 1)
            result["look_at_check"] = {**sight, "method": "static_geometry_line_of_sight_with_1_unit_tolerance",
                                       "render_visibility_checked": False,
                                       "limitation": "Collision line only: no rendered occlusion, player identity, animation or frustum verification"}
            result["passed"] = result["passed"] and sight["passed"]
            if not sight["passed"]:
                result["status"] = sight["status"]
    else:
        if project is None:
            project = client.request("get_state")["project"]
        result = geometry.validate_project(project, clearance)
    return result


def _pose(state, value):
    source = state.get("camera")
    if not isinstance(source, dict):
        raise LiveError("Current live camera pose is unavailable")
    pose = {key: copy.deepcopy(value.get(key, source.get(key))) for key in ("position", "angles", "fov")}
    project_cubics({"duration": 1, "keys": [{"id": "pose", "time": 0, **pose}]})
    return pose


def _resize_range(project, previous_duration):
    if project.get("duration", 5) == previous_duration:
        return
    value = project.setdefault("render_range", {"mode": "composition", "in": 0., "out": previous_duration})
    duration = project["duration"]
    if abs(value.get("out", previous_duration) - previous_duration) < 1e-9:
        value["out"] = duration
    span = min(.01, duration)
    value["in"] = min(max(value.get("in", 0), 0.), duration - span)
    value["out"] = min(max(value.get("out", duration), value["in"] + span), duration)


def _candidate(state, method, params):
    """Build the exact candidate without writing state; materialize defaults."""
    project = copy.deepcopy(state.get("project"))
    if not isinstance(project, dict) or not isinstance(project.get("keys"), list):
        raise LiveError("Live project snapshot is unavailable")
    dispatch, outgoing = method, copy.deepcopy(params)
    if method == "set_camera":
        return None, "set_camera", _pose(state, params)
    if method == "add_key":
        pose, time = _pose(state, params), params.get("time", state.get("cursor", 0))
        outgoing = {**params, **pose, "time": time}
        key = {**outgoing, "id": "guard-" + uuid.uuid4().hex}
        previous = project.get("duration", 5)
        project["duration"] = max(previous, time)
        _resize_range(project, previous)
        if not project["keys"]:
            project["start_tick"] = max(0, state["tick"] - math.floor(time * state.get("tick_rate", 64) + .5))
        project["keys"].append(key)
    elif method in {"update_key", "remove_key"}:
        keys = project["keys"]
        found = next((i for i, key in enumerate(keys) if key.get("id") == params["id"]), None)
        if found is None:
            raise LiveError("Unknown camera key id")
        if method == "update_key":
            keys[found].update(params)
        else:
            keys.pop(found)
    elif method == "set_project":
        previous = project.get("duration", 5)
        if "duration" in params:
            project["duration"] = params["duration"]
            _resize_range(project, previous)
        if "render_range" in params:
            outgoing["render_range"] = {**project.get("render_range", {}), **params["render_range"]}
        project.update(outgoing)
    elif method == "import_project":
        project = copy.deepcopy(params["project"])
    elif method == "apply_preset":
        pose = _pose(state, {})
        name, duration, distance = params["name"], params.get("duration", 5), params.get("distance", 200)
        project.update(name=name + " cinematic", start_tick=max(0, state["tick"]), duration=duration)
        _resize_range(project, state["project"].get("duration", 5))
        keys = []
        def add(time, value):
            keys.append({"id": "guard-" + uuid.uuid4().hex, "time": time, **copy.deepcopy(value)})
        if name == "hold":
            add(0, pose); add(duration, pose)
        elif name == "dolly":
            pitch, yaw = (math.radians(x) for x in pose["angles"][:2])
            end = copy.deepcopy(pose)
            end["position"] = [pose["position"][0] + distance * math.cos(pitch) * math.cos(yaw),
                               pose["position"][1] + distance * math.cos(pitch) * math.sin(yaw),
                               pose["position"][2] - distance * math.sin(pitch)]
            add(0, pose); add(duration, end)
        elif name == "orbit":
            yaw = math.radians(pose["angles"][1])
            cx, cy = pose["position"][0] + distance * math.cos(yaw), pose["position"][1] + distance * math.sin(yaw)
            for i in range(5):
                end = copy.deepcopy(pose)
                end["position"][:2] = [cx - distance * math.cos(yaw + i * math.pi / 8), cy - distance * math.sin(yaw + i * math.pi / 8)]
                end["angles"][1] += i * 22.5
                add(duration * i / 4, end)
        else:
            raise LiveError("Unknown camera preset")
        project["keys"] = keys
        # Send these exact validated poses, never ask native code to regenerate
        # a preset around a possibly changed free camera after the check.
        dispatch, outgoing = "set_project", project
    return project, dispatch, outgoing


def guarded_camera_call(client, method, params):
    if method == "get_state":
        return decorate_state(client.request(method, params))
    if method == "set_project" and not SPATIAL_PROJECT_FIELDS.intersection(params):
        return client.request(method, params)
    if method not in SPATIAL_METHODS and method != "set_project":
        return client.request(method, params)
    if method == "preview" and params.get("playing") is False:
        return client.request(method, params)
    if method in {"undo", "redo"}:
        raise LiveError("Geometry guard cannot inspect native undo/redo target projects; use the in-game history or explicitly import a checked project")
    state = client.request("get_state")
    if state.get("guarded_mutation_abi") != 1:
        raise LiveError("Geometry guard requires the native frame-atomic guarded-mutation adapter; update/restart the Camera Studio session")
    if (state.get("demo") is not True or not state.get("camera_valid") or not state.get("view_hook_installed")
            or state.get("paused") is not True or state.get("demo_seek", {}).get("active")
            or state.get("demo_seek", {}).get("queued")):
        raise LiveError("Geometry guard requires a ready, paused local demo camera with no pending seek")
    revision, tick = state.get("revision"), state.get("tick")
    if isinstance(revision, bool) or not isinstance(revision, int) or isinstance(tick, bool) or not isinstance(tick, int):
        raise LiveError("Native camera revision/tick evidence is unavailable")
    project, dispatch, outgoing = _candidate(state, method, params)
    # Removing the final key is safe without a map; it eliminates motion.
    empty = project is not None and not project.get("keys") and method in {"remove_key", "set_project", "import_project"}
    if not empty:
        try:
            geometry = session_geometry()
            if method == "set_camera":
                report = geometry.validate_position(outgoing["position"])
            else:
                report = geometry.validate_project(project)
        except (OSError, ValueError, ImportError) as error:
            raise LiveError("Camera geometry guard unavailable: " + str(error)) from error
        if not report["passed"]:
            raise LiveError("Camera geometry guard rejected the change; original project preserved: " + json.dumps(report, ensure_ascii=False, separators=(",", ":")))
    mutation = {"expected_revision": revision, "expected_tick": tick,
                "expected_paused": True, "method": dispatch, "params": outgoing}
    if method == "apply_preset":
        mutation["reset_cursor"] = True
    return client.request("guarded_mutation", mutation)
