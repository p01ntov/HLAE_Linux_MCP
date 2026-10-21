"""Dependency-free MCP stdio bridge for the local in-game camera controller.

Wire format follows the official MCP stdio transport: one UTF-8 JSON-RPC
message per line. Only protocol messages are written to stdout.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import re
import sys

from .live import LiveError, MAX_MESSAGE, NativeClient, resource_root, session_descriptor

PROTOCOL_VERSION = "2025-11-25"
SUPPORTED_PROTOCOLS = {PROTOCOL_VERSION, "2025-06-18", "2025-03-26", "2024-11-05"}
NUMBER = {"type": "number"}
TIME = {"type": "number", "minimum": 0, "maximum": 86400}
VECTOR = {"type": "array", "items": {"type": "number", "minimum": -1000000, "maximum": 1000000}, "minItems": 3, "maxItems": 3}
EASING = {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 1}, "minItems": 4, "maxItems": 4,
          "description": "Cubic Bezier speed handles [x1,y1,x2,y2] for the outgoing segment; all handles are in [0,1]."}
CAMERA_FIELDS = {"position": VECTOR, "angles": {**VECTOR, "description": "[pitch,yaw,roll] in degrees"},
                 "fov": {"type": "number", "minimum": 1, "maximum": 179}}
KEY_FIELDS = {"id": {"type": "string", "minLength": 1, "maxLength": 128}, "time": TIME,
              **CAMERA_FIELDS, "easing": EASING}


def object_schema(properties=None, required=()):
    return {"type": "object", "properties": properties or {}, "required": list(required), "additionalProperties": False}


RENDER_RANGE = object_schema({"mode": {"type": "string", "enum": ["composition", "region"]},
                              "in": TIME, "out": TIME})
SCENE_FIELDS = {name: {"type": "boolean"} for name in (
    "depth_of_field", "dof_override", "color_correction", "tonemapping", "fullbright",
    "fog", "fog_gradient", "fog_cubemap", "fog_volume", "hud_visible", "viewmodel_visible",
    "fps_counter_visible", "trueview_status_visible", "telemetry_frametime_visible", "telemetry_network_visible",
    "telemetry_ping_visible", "telemetry_margin_visible", "build_info_visible")}
SCENE_FIELDS["shading_mode"] = {"type": "integer", "minimum": 0, "maximum": 2,
                                "description": "Engine shading enum: 0 normal, 1 fullbright, 2 diffuse lighting; not a guaranteed gray-material override."}
SCENE_FIELDS.update({name: {"type": "number", "minimum": bounds[0], "maximum": bounds[1]} for name, bounds in {
    "dof_near_blurry": (-10000, 100000), "dof_near_crisp": (0, 100000),
    "dof_far_crisp": (0, 100000), "dof_far_blurry": (0, 100000), "dof_tilt": (0, 1),
    "dof_radius": (0, 10), "dof_max_blur": (0, 64), "bloom_strength": (0, 10), "exposure": (0, 16)}.items()})
SCENE = object_schema(SCENE_FIELDS)
STEAM_ID = {"type": "string", "minLength": 1, "maxLength": 20, "pattern": r"^[1-9][0-9]{0,19}$",
            "format": "steamid64", "description": "Exact nonzero decimal uint64 SteamID64, without leading zeroes; read current players from get_state.cosmetics.players."}
PLAYER_COSMETICS_FIELDS = {"steam_id": STEAM_ID,
                           "nickname": {"type": "string", "maxLength": 128, "format": "cosmetic-nickname",
                                        "description": "Optional display label, up to 128 UTF-8 bytes; targeting uses steam_id."},
                           "agent_item_def": {"type": "integer", "minimum": 1, "maximum": 65535,
                                              "description": "Exact builtin agent item_def from cosmetics_catalog matching the player's current T/CT team."}}
PLAYER_COSMETICS = object_schema(PLAYER_COSMETICS_FIELDS, ("steam_id", "agent_item_def"))
RGB = {"type": "array", "items": {"type": "integer", "minimum": 0, "maximum": 255}, "minItems": 3, "maxItems": 3}
TEAM_PALETTE_FIELDS = {"enabled": {"type": "boolean"}, "t": RGB, "ct": RGB}
TEAM_PALETTE = object_schema(TEAM_PALETTE_FIELDS, ("enabled", "t", "ct"))
COSMETICS = {**object_schema({"players": {"type": "array", "items": PLAYER_COSMETICS, "maxItems": 128},
                             "team_palette": TEAM_PALETTE}), "format": "agent-cosmetics"}
PRESENTATION_FIELDS = {"view": {"type": "string", "enum": ["player", "free"]},
                       "hud": {"type": "string", "enum": ["gameplay", "demo", "hidden"]}}
PRESENTATION = {**object_schema(PRESENTATION_FIELDS), "format": "presentation"}

PROJECT = {**object_schema({"name": {"type": "string", "maxLength": 256},
                         "start_tick": {"type": "integer", "minimum": 0, "maximum": 2000000000},
                         "duration": {"type": "number", "minimum": .01, "maximum": 86400},
                         "interpolation": {"type": "string", "enum": ["smooth", "linear"]},
                         "render_range": RENDER_RANGE,
                         "scene": SCENE,
                         "cosmetics": COSMETICS,
                         "presentation": PRESENTATION,
                         "keys": {"type": "array", "items": object_schema(KEY_FIELDS, ("id", "time", "position", "angles")), "maxItems": 4096}}, ("keys",)), "format": "camera-project"}

GUIDE_URI = "cs2-camera://guides/agent"
GUIDE_RESOURCE = {"uri": GUIDE_URI, "name": "mcp-agent-guide", "title": "Demo evidence and camera authoring workflow",
                  "description": "Read first: grounded moment search, tick/time conventions, live camera edits, preview, save and temporal render ranges.",
                  "mimeType": "text/markdown"}


def agent_guide():
    return (resource_root() / "docs/mcp-agent-guide.md").read_text(encoding="utf-8")


def tool(method, description, properties=None, required=(), read_only=False):
    return {"name": "cs2_camera_" + method, "title": method.replace("_", " ").title(),
            "description": description, "inputSchema": object_schema(properties, required),
            "annotations": {"readOnlyHint": read_only, "destructiveHint": False, "openWorldHint": False}}


TOOLS = [
    tool("get_state", "Read the live camera, demo tick, project keys, preview cursor and hook health. The in-game editor and MCP share this state.", read_only=True),
    tool("set_presentation", "Request player/free spectator view and gameplay/demo/hidden HUD. Read get_state.presentation.capabilities: requestable permits a request, available/actual view and HUD readback confirm current state; pending is not completion. Gameplay HUD requires player view and its separate native adapter. Its live implementation is partial: spectator money and weapon panels can remain visible, so successful requests and readback do not confirm a complete player HUD. Free+gameplay is rejected. Saved in project.presentation with undo/redo. Offline capture supports free authored camera and demo/hidden HUD; player view or gameplay HUD export fails explicitly until wired.", PRESENTATION_FIELDS),
    tool("select_player", "Watch a demo player identified by their exact SteamID64 from get_state.cosmetics.players. Releases the held authored camera without deleting keys or changing the HUD choice. Check get_state.presentation.selected_player, selection_pending and capabilities.select_player; a submitted request is not a confirmed observer target.", {"steam_id": STEAM_ID}, ("steam_id",)),
    tool("set_camera", "Set the live cinematic camera without restarting CS2. Coordinates are Source world units; angles are degrees. Changes hold the camera at this pose. FOV 1..179 changes the current view; editing a saved key requires update_key. Offline capture ABI 3 supports key FOV 30..140.", CAMERA_FIELDS),
    tool("release_camera", "Release cinematic control and return to the demo's normal spectator camera."),
    tool("add_key", "Add a camera key to the project. Omit pose to capture the current live view; omit time to use the timeline cursor.", {k: v for k, v in KEY_FIELDS.items() if k != "id"}),
    tool("update_key", "Edit an existing key's pose, time or outgoing Bezier easing; changes also appear in the in-game timeline.", KEY_FIELDS, ("id",)),
    tool("remove_key", "Remove a camera key. Undo can restore it.", {"id": KEY_FIELDS["id"]}, ("id",)),
    tool("set_project", "Edit project metadata or replace keys. render_range is temporal In/Out in seconds relative to start_tick; partial render_range updates preserve other bounds. Changing mode to composition keeps the saved region.", PROJECT["properties"]),
    tool("scrub", "Sample the camera path at a timeline time; optionally seek the demo to the matching tick.",
         {"time": TIME, "seek_demo": {"type": "boolean", "default": False}}, ("time",)),
    tool("preview", "Start or pause an in-game camera preview. sync_demo=false flies through a frozen scene; true also resumes the demo from the project cursor.",
         {"playing": {"type": "boolean"}, "sync_demo": {"type": "boolean", "default": False}}, ("playing",)),
    tool("demo_control", "Pause/resume the local demo or seek to an absolute demo tick. No arbitrary engine commands are exposed.",
         {"action": {"type": "string", "enum": ["pause", "resume", "seek"]}, "tick": {"type": "integer", "minimum": 0, "maximum": 2000000000}}, ("action",)),
    tool("apply_preset", "Create a hold, dolly or orbit path around the current camera. Review the generated keys and preview before rendering.",
         {"name": {"type": "string", "enum": ["hold", "dolly", "orbit"]},
          "duration": {"type": "number", "minimum": .01, "maximum": 86400},
          "distance": {"type": "number", "minimum": 1, "maximum": 10000}}, ("name",)),
    tool("undo", "Undo the latest project edit from either MCP or the in-game UI."),
    tool("redo", "Redo a project edit."),
    tool("export_project", "Return the portable camera project as JSON.", read_only=True),
    tool("import_project", "Load a portable project into the running editor. Undo can restore the previous project.", {"project": PROJECT}, ("project",)),
    tool("save_project", "Save the current camera project to an explicit host .json path. Existing files require overwrite=true.",
         {"path": {"type": "string", "minLength": 1}, "overwrite": {"type": "boolean", "default": False}}, ("path",)),
    tool("load_project", "Load a camera project from an explicit host .json path into the current game session.",
         {"path": {"type": "string", "minLength": 1}}, ("path",)),
    tool("set_scene", "Apply whitelisted scene effects to the running -insecure demo. First read get_state.scene_capabilities: only available=true controls may be changed, and runtime bounds/types are authoritative. Native code validates and reads back actual engine values, rejecting an unsupported batch. DOF planes must satisfy near_blurry<=near_crisp<=far_crisp<=far_blurry; dof_override enables explicit focus. Only use depth_of_field when that separate control is available; its absence does not disable available focus controls. Saved as project.scene with undo/redo. This is not a skin/model/material API. A gray-map material override is unavailable.",
         {"settings": SCENE}, ("settings",)),
    tool("reset_scene", "Restore this session's original engine values for scene effects and clear project.scene overrides. This does not reset camera keys."),
    tool("request_export", "Ask this session's desktop launcher to open the export window. With a terminal-only launcher, use its Export UI through the desktop app; this request alone does not start a render."),
    tool("inspect_entities", "Read bounded native actor/entity evidence from the local demo: validated build/profile, class/team and available player identities. No raw addresses or mutation are exposed. For cosmetics, also inspect get_state.cosmetics.players and category capabilities before editing.",
         {"limit": {"type": "integer", "minimum": 1, "maximum": 64, "default": 64}}, read_only=True),
    tool("set_player_cosmetics", "Select one builtin agent for an exact SteamID64 in the running -insecure demo. First read get_state.cosmetics.capabilities.agents and current players; available must be true. Native validation rechecks the current pawn, team and installed compiled model before applying. nickname is a display label. Saved as project.cosmetics with undo/redo. render_verified_1p/render_verified_3p=false means rendering still needs visual verification. This setter only changes builtin agents; per-player custom model imports are unavailable.",
         PLAYER_COSMETICS_FIELDS, ("steam_id", "agent_item_def")),
    tool("reset_player_cosmetics", "Restore the selected SteamID64's original agent model and remove its saved override; omit steam_id to reset all player agent overrides. Read get_state.cosmetics capabilities/state and visually verify restoration. Supports undo/redo.",
         {"steam_id": STEAM_ID}),
    tool("set_team_palette", "Edit team colors in the running -insecure demo. First read get_state.cosmetics.capabilities.team_palette; available must be true to enable it. t/ct are integer RGB triples; omitted fields preserve current settings or session defaults. Saved as project.cosmetics.team_palette with undo/redo. render_verified_1p/render_verified_3p=false means rendering still needs visual verification.",
         TEAM_PALETTE_FIELDS),
    tool("reset_team_palette", "Restore original player colors and remove the saved team palette override. Read get_state.cosmetics capabilities/state and visually verify restoration. Supports undo/redo."),

]

DEMO_PATH = {"type": "string", "minLength": 1, "maxLength": 4096,
             "description": "Local .dem path. Omit to use the active launcher's demo; explicit paths also work with CS2 closed."}
TICK = {"type": "integer", "minimum": 0, "maximum": 2000000000}
PLAYER = {"type": "string", "minLength": 1, "maxLength": 256,
          "description": "Exact full nickname (case-insensitive) or SteamID64; duplicate names require SteamID64."}


def demo_tool(method, description, properties, required=()):
    definition = tool(method, description, {"demo": DEMO_PATH, **properties}, required, read_only=True)
    definition["name"] = "cs2_demo_" + method
    return definition


TOOLS += [
    demo_tool("inspect", "Read local CS2 demo header, players, actual round-start ticks and weapon IDs. No game is launched. Event bounds are not demo duration. Read cs2-camera://guides/agent before directing.", {}),
    demo_tool("find_moments", "Find actual player_death evidence by player, weapon, headshot, round and tick/time interval. min_kills>1 groups the same attacker and round within window_seconds. Counts only events matching every filter; does not infer clutches or visibility. Defaults exclude warmup and non-enemy deaths. Seconds are absolute demo tick/64, not round clock. End search bound is exclusive. Results are chronological and paginated.",
              {"player": PLAYER, "role": {"type": "string", "enum": ["attacker", "victim", "either"], "default": "attacker"},
               "weapons": {"type": "array", "items": {"type": "string", "minLength": 1, "maxLength": 64}, "maxItems": 16},
               "headshot": {"type": "boolean"}, "round": {"type": "integer", "minimum": 1, "maximum": 1000},
               "start_tick": TICK, "end_tick": TICK, "start_seconds": TIME, "end_seconds": TIME,
               "min_kills": {"type": "integer", "minimum": 1, "maximum": 10, "default": 1},
               "window_seconds": {"type": "number", "minimum": .01, "maximum": 120, "default": 10},
               "include_warmup": {"type": "boolean", "default": False}, "include_non_enemy": {"type": "boolean", "default": False},
               "limit": {"type": "integer", "minimum": 1, "maximum": 20, "default": 10},
               "offset": {"type": "integer", "minimum": 0, "maximum": 200000, "default": 0}}),
    demo_tool("player_positions", "Read one player's recorded world origin, view pitch/yaw, health and alive state at up to 32 exact demo ticks. Missing samples are reported, never guessed. Positions do not prove camera visibility or collision clearance.",
              {"player": PLAYER, "ticks": {"type": "array", "items": TICK, "minItems": 1, "maxItems": 32}}, ("player", "ticks")),
]
TOOLS += [
    {**tool("cosmetics_catalog", "List builtin agent definitions and localized names from the installed game. Agent team filtering is strict CT/T from used_by_classes. Returns real agent item_def and compiled model paths; it does not apply cosmetics or modify Steam inventory. Use get_state.cosmetics.capabilities.agents before selecting an agent. Weapon skins, knives, gloves and global appearance packs are not supported in this build.",
            {"game_dir": {"type":"string","minLength":1,"maxLength":4096},
             "kind": {"type":"string","enum":["agents"],"default":"agents"},
             "team": {"type":"string","enum":["CT","T"]},
             "search": {"type":"string","maxLength":256}, "limit": {"type":"integer","minimum":1,"maximum":100},
             "offset": {"type":"integer","minimum":0,"maximum":100000},
             "language": {"type":"string","enum":["english","russian"],"default":"english"}}, read_only=True),
     "name":"cs2_assets_cosmetics_catalog"},
]
TOOL_MAP = {item["name"]: item for item in TOOLS}


class ProtocolError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message
        super().__init__(message)


def validate(value, schema, path="arguments"):
    """Validate this connector's finite, fixed schema subset without a dependency."""
    kind = schema.get("type")
    correct = {"object": isinstance(value, dict), "array": isinstance(value, list),
               "string": isinstance(value, str), "boolean": isinstance(value, bool),
               "integer": isinstance(value, int) and not isinstance(value, bool),
               "number": isinstance(value, (int, float)) and not isinstance(value, bool)}
    if kind and not correct.get(kind, False):
        raise ProtocolError(-32602, f"{path} must be {kind}")
    if kind in {"number", "integer"}:
        if (isinstance(value, float) and not math.isfinite(value)) or value < schema.get("minimum", -math.inf) or value > schema.get("maximum", math.inf):
            raise ProtocolError(-32602, f"{path} is outside the allowed range")
    if "enum" in schema and value not in schema["enum"]:
        raise ProtocolError(-32602, f"{path} must be one of {schema['enum']}")
    if kind == "object":
        if schema.get("format") == "camera-project":
            reject_removed_cosmetics(value)
        elif schema.get("format") == "agent-cosmetics":
            reject_removed_cosmetics({"cosmetics":value})
        for key in schema.get("required", []):
            if key not in value:
                raise ProtocolError(-32602, f"{path}.{key} is required")
        properties = schema.get("properties", {})
        for key, child in value.items():
            if key not in properties:
                if schema.get("additionalProperties") is False:
                    raise ProtocolError(-32602, f"Unknown field: {path}.{key}")
            else:
                validate(child, properties[key], f"{path}.{key}")
        if schema.get("format") == "presentation" and value.get("view") == "free" and value.get("hud") == "gameplay":
            raise ProtocolError(-32602, "Gameplay HUD requires player view; free view supports demo or hidden HUD")
    elif kind in {"array", "string"}:
        suffix = "Items" if kind == "array" else "Length"
        if len(value) < schema.get("min" + suffix, 0) or len(value) > schema.get("max" + suffix, math.inf):
            raise ProtocolError(-32602, f"{path} has an invalid length")
        if kind == "array":
            for index, child in enumerate(value):
                validate(child, schema["items"], f"{path}[{index}]")
        elif "pattern" in schema and re.fullmatch(schema["pattern"], value) is None:
            raise ProtocolError(-32602, f"{path} has an invalid format")
        if kind == "string" and schema.get("format") == "steamid64" and int(value) > 18446744073709551615:
            raise ProtocolError(-32602, f"{path} exceeds uint64")
        if kind == "string" and schema.get("format") == "cosmetic-nickname" and ("\0" in value or len(value.encode("utf-8")) > 128):
            raise ProtocolError(-32602, f"{path} must be a display string up to 128 UTF-8 bytes")


def reject_removed_cosmetics(project):
    """Reject legacy skin requests without deleting portable project data."""
    if not isinstance(project, dict):
        return
    if "appearance" in project:
        raise ProtocolError(-32602, "Global appearance packs are not supported in this build; remove project.appearance explicitly before loading or exporting")
    cosmetics = project.get("cosmetics")
    if isinstance(cosmetics, dict):
        for field in ("weapon_paints", "gloves"):
            if field in cosmetics:
                raise ProtocolError(-32602, f"project.cosmetics.{field} is not supported in this build; remove it explicitly before loading or exporting")


def _project_cosmetics_preflight(project):
    reject_removed_cosmetics(project)
    if "cosmetics" not in project:
        return
    cosmetics = project["cosmetics"]
    # Loading portable files may include format/version metadata, but new
    # typed fields still receive schema validation before native import.
    validate(cosmetics, COSMETICS, "project.cosmetics")


def call_tool(client: NativeClient, name: str, arguments: dict):
    definition = TOOL_MAP.get(name)
    if definition is None:
        raise ProtocolError(-32602, f"Unknown tool: {name}")
    if name == "cs2_camera_set_project":
        reject_removed_cosmetics(arguments)
    validate(arguments, definition["inputSchema"])
    if name.startswith("cs2_assets_"):
        if name == "cs2_assets_cosmetics_catalog":
            from .cosmetics_catalog import query_catalog
            return query_catalog(**arguments)
    if name.startswith("cs2_demo_"):
        # Keep parser/Rust/dataframe imports lazy for ordinary live MCP use.
        from . import demo_analysis
        query = dict(arguments)
        path = query.pop("demo", None)
        if path is None:
            path = session_descriptor().get("demo")
            if not path:
                raise LiveError("No active session demo; pass an explicit demo path")
        method = name.removeprefix("cs2_demo_")
        if method == "inspect":
            return demo_analysis.inspect(path)
        if method == "player_positions":
            return demo_analysis.player_positions(path, **query)
        return demo_analysis.find_moments(path, **query)
    method = name.removeprefix("cs2_camera_")
    if method == "set_presentation":
        validate(arguments, PRESENTATION)
    elif method == "set_project":
        _project_cosmetics_preflight(arguments)
    elif method == "import_project":
        _project_cosmetics_preflight(arguments["project"])
    if method == "demo_control" and arguments["action"] == "seek" and "tick" not in arguments:
        raise ProtocolError(-32602, "tick is required for a demo seek")
    if method in {"save_project", "load_project"}:
        path = Path(arguments["path"]).expanduser().absolute()
        if path.suffix.lower() != ".json":
            raise LiveError("Camera project file must have a .json extension")
        if method == "load_project":
            if path.stat().st_size > MAX_MESSAGE:
                raise LiveError("Project file exceeds 1 MiB")
            project = json.loads(path.read_text())
            # Native export may contain format/version metadata; the controller
            # remains the authoritative validator for portable project files.
            if not isinstance(project, dict):
                raise LiveError("Project file must contain a JSON object")
            _project_cosmetics_preflight(project)
            return client.request("import_project", {"project": project})
        project = client.request("export_project")
        _project_cosmetics_preflight(project)
        mode = "w" if arguments.get("overwrite", False) else "x"
        with path.open(mode) as handle:
            json.dump(project, handle, ensure_ascii=False, indent=2, allow_nan=False)
            handle.write("\n")
        return {"path": str(path), "saved": True}
    return client.request(method, arguments)


class Server:
    def __init__(self, client):
        self.client, self.initialized = client, False

    def handle(self, request):
        request_id = request.get("id") if isinstance(request, dict) else None
        try:
            if (not isinstance(request, dict) or request.get("jsonrpc") != "2.0"
                    or not isinstance(request.get("method"), str)
                    or ("id" in request and (isinstance(request_id, bool) or not isinstance(request_id, (str, int))))):
                raise ProtocolError(-32600, "Invalid JSON-RPC request")
            if "id" not in request:
                return None  # Notifications, including initialized/cancelled, have no reply.
            method, params = request["method"], request.get("params", {})
            if not isinstance(params, dict):
                raise ProtocolError(-32602, "params must be an object")
            if method == "initialize":
                version = params.get("protocolVersion", PROTOCOL_VERSION)
                if not isinstance(version, str):
                    raise ProtocolError(-32602, "protocolVersion must be a string")
                self.initialized = True
                result = {"protocolVersion": version if version in SUPPORTED_PROTOCOLS else PROTOCOL_VERSION,
                          "capabilities": {"tools": {"listChanged": False}, "resources": {"subscribe": False, "listChanged": False}},
                          "serverInfo": {"name": "cs2-camera-studio", "version": "0.1.2"},
                          "instructions": "Read resource cs2-camera://guides/agent first for the evidence-to-camera workflow. Offline cs2_demo_* tools inspect local .dem files and find real event ticks without launching CS2; pass demo or use the active launcher demo. Never invent kills, ticks, player positions or supported scene effects. Camera tools connect to an already running local CS2 -insecure demo session; read get_state and hook health before editing. Project edits share the in-game editor and support undo. No online play or arbitrary engine commands."}
            elif method == "ping":
                result = {}
            elif not self.initialized:
                raise ProtocolError(-32002, "Initialize the MCP session first")
            elif method == "tools/list":
                if params.get("cursor"):
                    raise ProtocolError(-32602, "This server has no additional tool pages")
                result = {"tools": TOOLS}
            elif method == "resources/list":
                if params.get("cursor"):
                    raise ProtocolError(-32602, "This server has no additional resource pages")
                result = {"resources": [GUIDE_RESOURCE]}
            elif method == "resources/templates/list":
                result = {"resourceTemplates": []}
            elif method == "resources/read":
                if params.get("uri") != GUIDE_URI:
                    raise ProtocolError(-32002, "Resource not found")
                try:
                    result = {"contents": [{"uri": GUIDE_URI, "mimeType": "text/markdown", "text": agent_guide()}]}
                except OSError as error:
                    raise ProtocolError(-32603, f"Bundled agent guide unavailable: {error}") from error
            elif method == "tools/call":
                name, arguments = params.get("name"), params.get("arguments", {})
                if not isinstance(name, str):
                    raise ProtocolError(-32602, "Tool name must be a string")
                try:
                    value = call_tool(self.client, name, arguments)
                    result = {"content": [{"type": "text", "text": json.dumps(value, ensure_ascii=False, allow_nan=False)}], "isError": False}
                    if isinstance(value, dict):
                        result["structuredContent"] = value
                except (LiveError, OSError, ValueError) as error:
                    result = {"content": [{"type": "text", "text": str(error)}], "isError": True}
            else:
                raise ProtocolError(-32601, f"Unknown method: {method}")
            return {"jsonrpc": "2.0", "id": request_id, "result": result}
        except ProtocolError as error:
            return {"jsonrpc": "2.0", "id": request_id, "error": {"code": error.code, "message": error.message}}


def serve(client: NativeClient | None = None, input_stream=None, output_stream=None) -> int:
    server = Server(client or NativeClient())
    input_stream, output_stream = input_stream or sys.stdin, output_stream or sys.stdout
    while True:
        line = input_stream.readline(MAX_MESSAGE + 1)
        if not line:
            return 0
        if len(line.encode("utf-8")) > MAX_MESSAGE:
            # Drain the rest of this line to preserve framing for the next call.
            while not line.endswith("\n") and line:
                line = input_stream.readline(MAX_MESSAGE + 1)
            response = {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Message exceeds 1 MiB"}}
        else:
            try:
                request = json.loads(line, parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Non-finite JSON number")))
                response = server.handle(request)
            except (ValueError, UnicodeError, RecursionError):
                response = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Invalid JSON"}}
        if response is not None:
            output_stream.write(json.dumps(response, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n")
            output_stream.flush()


if __name__ == "__main__":
    raise SystemExit(serve())
