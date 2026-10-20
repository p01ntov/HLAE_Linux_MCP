import io
import json
from pathlib import Path
import tempfile
import unittest

from cs2_mvp.live import LIVE_METHODS, LiveError
from cs2_mvp.live_mcp import Server, TOOLS, call_tool, serve


class FakeClient:
    def __init__(self):
        self.calls = []
        self.project = {"name": "Test", "duration": 5, "start_tick": 300, "keys": []}
        self.failure = None

    def request(self, method, params=None):
        self.calls.append((method, params))
        if self.failure:
            raise LiveError(self.failure)
        return self.project if method == "export_project" else {"method": method, "params": params}


class McpTests(unittest.TestCase):
    def setUp(self):
        self.client = FakeClient()
        self.server = Server(self.client)
        self.server.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25"}})

    def call(self, name, arguments):
        return self.server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "cs2_camera_" + name, "arguments": arguments}})

    def test_all_native_methods_have_explicit_schemas(self):
        methods = {tool["name"].removeprefix("cs2_camera_") for tool in TOOLS}
        self.assertTrue(LIVE_METHODS.issubset(methods))
        for tool in TOOLS:
            self.assertEqual(tool["inputSchema"]["type"], "object")
            self.assertFalse(tool["inputSchema"]["additionalProperties"])
        self.assertNotIn("cs2_camera_exec", methods)

    def test_camera_edit_and_bezier_key_are_forwarded(self):
        camera = {"position": [1, 2, 3], "angles": [-10, 45, 0], "fov": 75}
        result = self.call("set_camera", camera)
        self.assertFalse(result["result"]["isError"])
        self.assertEqual(self.client.calls[-1], ("set_camera", camera))
        key = {"id": "key-1", "time": 2, "easing": [.42, 0, .58, 1]}
        self.call("update_key", key)
        self.assertEqual(self.client.calls[-1], ("update_key", key))

    def test_preset_matches_native_name_parameter(self):
        self.call("apply_preset", {"name": "dolly", "distance": 200, "duration": 5})
        self.assertEqual(self.client.calls[-1][1]["name"], "dolly")

    def test_schema_rejects_invalid_and_unexpected_arguments(self):
        for method, arguments in [("set_camera", {"position": [1, 2]}),
                                  ("set_camera", {"fov": True}),
                                  ("set_camera", {"fov": float("nan")}),
                                  ("set_camera", {"exec": "quit"}),
                                  ("preview", {}), ("demo_control", {"action": "seek"})]:
            with self.subTest(method=method, arguments=arguments):
                result = self.call(method, arguments)
                self.assertEqual(result["error"]["code"], -32602)
        self.assertEqual(self.client.calls, [])

    def test_native_failure_is_tool_error(self):
        self.client.failure = "No live session"
        response = self.call("get_state", {})
        self.assertTrue(response["result"]["isError"])
        self.assertEqual(response["result"]["content"][0]["text"], "No live session")

    def test_initialize_and_notifications_use_stdio_framing(self):
        requests = ["not-json", json.dumps({"jsonrpc": "2.0", "id": "init", "method": "initialize", "params": {"protocolVersion": "2025-03-26"}}),
                    json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
                    json.dumps({"jsonrpc": "2.0", "id": "tools", "method": "tools/list"})]
        output = io.StringIO()
        self.assertEqual(serve(self.client, io.StringIO("\n".join(requests) + "\n"), output), 0)
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(len(responses), 3)
        self.assertEqual(responses[0]["error"]["code"], -32700)
        self.assertEqual(responses[1]["result"]["protocolVersion"], "2025-03-26")
        self.assertEqual(len(responses[2]["result"]["tools"]), len(TOOLS))
        self.assertEqual(self.client.calls, [])

    def test_initialize_is_required_and_unknown_methods_fail(self):
        request = {"jsonrpc": "2.0", "id": 3, "method": "tools/list"}
        self.assertEqual(Server(self.client).handle(request)["error"]["code"], -32002)
        self.assertEqual(self.server.handle({**request, "method": "execute"})["error"]["code"], -32601)

    def test_project_file_roundtrip_and_no_implicit_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "camera.json")
            self.assertTrue(call_tool(self.client, "cs2_camera_save_project", {"path": path})["saved"])
            with self.assertRaises(FileExistsError):
                call_tool(self.client, "cs2_camera_save_project", {"path": path})
            call_tool(self.client, "cs2_camera_load_project", {"path": path})
            self.assertEqual(self.client.calls[-1], ("import_project", {"project": self.client.project}))
            self.assertEqual(json.loads(Path(path).read_text()), self.client.project)


if __name__ == "__main__":
    unittest.main()
