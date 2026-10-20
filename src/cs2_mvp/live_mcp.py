"""Dependency-free MCP stdio bridge for the local in-game camera controller.

Wire format follows the official MCP stdio transport: one UTF-8 JSON-RPC
message per line. Only protocol messages are written to stdout.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import sys

from .live import LiveError, MAX_MESSAGE, NativeClient

PROTOCOL_VERSION = "2025-11-25"
SUPPORTED_PROTOCOLS = {PROTOCOL_VERSION, "2025-06-18", "2025-03-26", "2024-11-05"}
NUMBER = {"type": "number"}
TIME = {"type": "number", "minimum": 0, "maximum": 86400}
VECTOR = {"type": "array", "items": {"type": "number", "minimum": -1000000, "maximum": 1000000}, "minItems": 3, "maxItems": 3}
EASING = {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 1}, "minItems": 4, "maxItems": 4,
          "description": "Cubic Bezier speed handles [x1,y1,x2,y2] for the outgoing segment; all handles are in [0,1]."}
CAMERA_FIELDS = {"position": VECTOR, "angles": {**VECTOR, "description": "[pitch,yaw,roll] in degrees"},
                 "fov": {"type": "number", "minimum": 30, "maximum": 140}}
KEY_FIELDS = {"id": {"type": "string", "minLength": 1, "maxLength": 128}, "time": TIME,
              **CAMERA_FIELDS, "easing": EASING}


def object_schema(properties=None, required=()):
    return {"type": "object", "properties": properties or {}, "required": list(required), "additionalProperties": False}


PROJECT = object_schema({"name": {"type": "string", "maxLength": 256},
                         "start_tick": {"type": "integer", "minimum": 0, "maximum": 2000000000},
                         "duration": {"type": "number", "minimum": .01, "maximum": 86400},
                         "interpolation": {"type": "string", "enum": ["smooth", "linear"]},
                         "keys": {"type": "array", "items": object_schema(KEY_FIELDS, ("id", "time", "position", "angles")), "maxItems": 4096}}, ("keys",))


def tool(method, description, properties=None, required=(), read_only=False):
    return {"name": "cs2_camera_" + method, "title": method.replace("_", " ").title(),
            "description": description, "inputSchema": object_schema(properties, required),
            "annotations": {"readOnlyHint": read_only, "destructiveHint": False, "openWorldHint": False}}


TOOLS = [
    tool("get_state", "Read the live camera, demo tick, project keys, preview cursor and hook health. The in-game editor and MCP share this state.", read_only=True),
    tool("set_camera", "Set the live cinematic camera without restarting CS2. Coordinates are Source world units; angles are degrees. Changes hold the camera at this pose.", CAMERA_FIELDS),
    tool("release_camera", "Release cinematic control and return to the demo's normal spectator camera."),
    tool("add_key", "Add a camera key to the project. Omit pose to capture the current live view; omit time to use the timeline cursor.", {k: v for k, v in KEY_FIELDS.items() if k != "id"}),
    tool("update_key", "Edit an existing key's pose, time or outgoing Bezier easing; changes also appear in the in-game timeline.", KEY_FIELDS, ("id",)),
    tool("remove_key", "Remove a camera key. Undo can restore it.", {"id": KEY_FIELDS["id"]}, ("id",)),
    tool("set_project", "Edit project metadata or replace keys. Time is seconds relative to start_tick.", PROJECT["properties"]),
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
    elif kind in {"array", "string"}:
        suffix = "Items" if kind == "array" else "Length"
        if len(value) < schema.get("min" + suffix, 0) or len(value) > schema.get("max" + suffix, math.inf):
            raise ProtocolError(-32602, f"{path} has an invalid length")
        if kind == "array":
            for index, child in enumerate(value):
                validate(child, schema["items"], f"{path}[{index}]")


def call_tool(client: NativeClient, name: str, arguments: dict):
    definition = TOOL_MAP.get(name)
    if definition is None:
        raise ProtocolError(-32602, f"Unknown tool: {name}")
    validate(arguments, definition["inputSchema"])
    method = name.removeprefix("cs2_camera_")
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
            return client.request("import_project", {"project": project})
        project = client.request("export_project")
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
                          "capabilities": {"tools": {"listChanged": False}},
                          "serverInfo": {"name": "cs2-camera-studio", "version": "0.1.0"},
                          "instructions": "Connects to an already running local CS2 -insecure demo session. Read get_state and hook health first. Camera/project edits are shared with the in-game editor and support undo. No online play or arbitrary engine command execution."}
            elif method == "ping":
                result = {}
            elif not self.initialized:
                raise ProtocolError(-32002, "Initialize the MCP session first")
            elif method == "tools/list":
                if params.get("cursor"):
                    raise ProtocolError(-32602, "This server has no additional tool pages")
                result = {"tools": TOOLS}
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
