import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.live import LIVE_METHODS, LiveError
from cs2_mvp.live_mcp import GUIDE_URI, Server, TOOLS, call_tool, serve


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

    def test_agent_guide_is_discoverable_and_frozen_resources_are_self_contained(self):
        initialization = Server(self.client).handle({"jsonrpc": "2.0", "id": 1, "method": "initialize"})["result"]
        self.assertIn("resources", initialization["capabilities"])
        self.assertIn(GUIDE_URI, initialization["instructions"])
        listing = self.server.handle({"jsonrpc": "2.0", "id": 2, "method": "resources/list"})
        self.assertEqual(listing["result"]["resources"][0]["uri"], GUIDE_URI)
        source = Path(__file__).resolve().parents[1] / "docs/mcp-agent-guide.md"
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "docs").mkdir()
            shutil.copy2(source, Path(directory) / "docs/mcp-agent-guide.md")
            with patch.object(sys, "_MEIPASS", directory, create=True):
                response = self.server.handle({"jsonrpc": "2.0", "id": 3, "method": "resources/read", "params": {"uri": GUIDE_URI}})
        text = response["result"]["contents"][0]["text"]
        for name in ("cs2_demo_inspect", "cs2_demo_find_moments", "cs2_demo_player_positions", "render_range", "scene_capabilities"):
            self.assertIn(name, text)
        self.assertEqual(self.client.calls, [])
        unknown = self.server.handle({"jsonrpc": "2.0", "id": 4, "method": "resources/read", "params": {"uri": "file:///etc/passwd"}})
        self.assertEqual(unknown["error"]["code"], -32002)

    def test_scene_controls_are_typed_whitelisted_and_native_validated(self):
        settings = {"depth_of_field": True, "dof_override": True, "dof_radius": 2.5, "exposure": 1.2,
                    "hud_visible": False, "viewmodel_visible": False}
        response = self.call("set_scene", {"settings": settings})
        self.assertFalse(response["result"]["isError"])
        self.assertEqual(self.client.calls[-1], ("set_scene", {"settings": settings}))
        self.call("reset_scene", {})
        self.assertEqual(self.client.calls[-1], ("reset_scene", {}))
        for settings in ({"exec": "sv_cheats 1"}, {"fog_gradient": 1}, {"dof_radius": 11}, {"exposure": float("inf")}):
            with self.subTest(settings=settings):
                self.assertEqual(self.call("set_scene", {"settings": settings})["error"]["code"], -32602)
        self.client.failure = "Scene control unavailable on this client"
        self.assertTrue(self.call("set_scene", {"settings": {"depth_of_field": True}})["result"]["isError"])

    def test_cosmetics_methods_forward_only_native_fields_and_preserve_partial_palette_edits(self):
        player = {"steam_id": "76561198000000001", "nickname": "Игрок", "agent_item_def": 11}
        operations = [("set_player_cosmetics", player),
                      ("reset_player_cosmetics", {"steam_id": player["steam_id"]}),
                      ("reset_player_cosmetics", {}),
                      ("set_team_palette", {"enabled": True, "t": [255, 64, 0], "ct": [0, 128, 255]}),
                      ("set_team_palette", {"ct": [64, 128, 255]}),
                      ("set_team_palette", {"enabled": False}),
                      ("reset_team_palette", {})]
        for method, arguments in operations:
            with self.subTest(method=method, arguments=arguments):
                response = self.call(method, arguments)
                self.assertFalse(response["result"]["isError"])
                self.assertEqual(self.client.calls[-1], (method, arguments))
        definitions = {item["name"]: item for item in TOOLS}
        for method in ("set_player_cosmetics", "set_team_palette"):
            description = definitions["cs2_camera_" + method]["description"]
            self.assertIn("get_state.cosmetics.capabilities", description)
            self.assertIn("render_verified_1p/render_verified_3p=false", description)
        for unsupported in ("set_weapon_cosmetics", "set_glove_cosmetics", "set_paint_kit"):
            self.assertNotIn("cs2_camera_" + unsupported, definitions)
        self.client.failure = "verified client model adapter is unavailable"
        self.assertTrue(self.call("set_player_cosmetics", player)["result"]["isError"])

    def test_cosmetics_schema_rejects_ambiguous_ids_invalid_rgb_and_unsupported_fields(self):
        player = {"steam_id": "76561198000000001", "agent_item_def": 11}
        invalid = [("set_player_cosmetics", {"steam_id": player["steam_id"]}),
                   ("set_player_cosmetics", {"agent_item_def": 11}),
                   ("set_player_cosmetics", {**player, "paint_kit": 282}),
                   ("reset_team_palette", {"enabled": False}),
                   ("set_team_palette", {"enabled": 1})]
        invalid += [("set_player_cosmetics", {**player, "steam_id": value}) for value in
                    (0, True, 76561198000000001, "0", "01", "-1", "+1", "1.0", "1\n", "１２", "18446744073709551616")]
        invalid += [("set_player_cosmetics", {**player, "agent_item_def": value}) for value in (True, 11.0, 0, 65536)]
        invalid += [("set_player_cosmetics", {**player, "nickname": value}) for value in ("a\0b", "я" * 65)]
        invalid += [("set_team_palette", {"t": value}) for value in ([1, 2], [0, 0, 256], [True, 0, 0], [1.0, 2, 3])]
        for method, arguments in invalid:
            with self.subTest(method=method, arguments=arguments):
                self.assertEqual(self.call(method, arguments)["error"]["code"], -32602)
        self.assertEqual(self.client.calls, [])
        self.assertFalse(self.call("set_player_cosmetics", {**player, "steam_id": "18446744073709551615", "nickname": "я" * 64})["result"]["isError"])

    def test_project_cosmetics_schema_and_file_roundtrip(self):
        cosmetics = {"players": [{"steam_id": "76561198000000001", "agent_item_def": 11}],
                     "team_palette": {"enabled": True, "t": [255, 64, 64], "ct": [64, 128, 255]}}
        self.assertFalse(self.call("set_project", {"cosmetics": cosmetics})["result"]["isError"])
        self.assertEqual(self.client.calls[-1], ("set_project", {"cosmetics": cosmetics}))
        self.client.project["cosmetics"] = cosmetics
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "cosmetics.json")
            call_tool(self.client, "cs2_camera_save_project", {"path": path})
            call_tool(self.client, "cs2_camera_load_project", {"path": path})
            self.assertEqual(self.client.calls[-1], ("import_project", {"project": self.client.project}))
            self.assertEqual(json.loads(Path(path).read_text())["cosmetics"], cosmetics)
        for invalid in ({"team_palette": {"enabled": True}}, {"players": [{}]},
                        {"players": cosmetics["players"] * 129}, {"weapons": []}):
            with self.subTest(cosmetics=invalid):
                self.assertEqual(self.call("set_project", {"cosmetics": invalid})["error"]["code"], -32602)

    def test_presentation_requests_and_project_persistence_are_typed(self):
        for value in ({"view":"player","hud":"gameplay"},{"view":"free","hud":"hidden"},{"hud":"demo"}):
            with self.subTest(value=value):
                self.assertFalse(self.call("set_presentation",value)["result"]["isError"])
                self.assertEqual(self.client.calls[-1],("set_presentation",value))
                self.assertFalse(self.call("set_project",{"presentation":value})["result"]["isError"])
        before=list(self.client.calls)
        for value in ({"view":"free","hud":"gameplay"},{"view":"roaming"},{"hud":False},{"exec":"quit"}):
            with self.subTest(value=value):
                self.assertEqual(self.call("set_presentation",value)["error"]["code"],-32602)
                self.assertEqual(self.call("set_project",{"presentation":value})["error"]["code"],-32602)
        self.assertEqual(self.client.calls,before)

    def test_weapon_paint_uses_session_catalog_bounds_and_preserves_native_validation(self):
        catalog={"weapons":[{"item_def":7,"kind":"weapon","paint_kits":[282]}],
                 "gloves":[{"item_def":5027,"kind":"glove","paint_kits":[10006]}],
                 "paint_kits":[{"id":282,"wear_min":.1,"wear_max":.7},{"id":10006,"wear_min":0,"wear_max":1}]}
        paint={"steam_id":"76561198000000001","nickname":"Player","item_def":7,"paint_kit":282,"wear":.4,"seed":42}
        with patch("cs2_mvp.live_mcp.session_descriptor",return_value={"game_directory":"/session/game"}), \
             patch("cs2_mvp.cosmetics_catalog.installed_catalog",return_value=catalog) as installed:
            self.assertFalse(self.call("set_player_weapon_paint",paint)["result"]["isError"])
            installed.assert_called_with("/session/game")
            self.assertEqual(self.client.calls[-1],("set_player_weapon_paint",paint))
            omitted={key:value for key,value in paint.items() if key!="wear"}
            self.assertFalse(self.call("set_player_weapon_paint",omitted)["result"]["isError"])
            self.assertEqual(self.client.calls[-1],("set_player_weapon_paint",omitted))
            for method,value in (("reset_player_weapon_paint",{"steam_id":paint["steam_id"]}),
                                 ("reset_player_weapon_paint",{"steam_id":paint["steam_id"],"item_def":7}),
                                 ("set_project",{"cosmetics":{"weapon_paints":[paint]}}),
                                 ("import_project",{"project":{"keys":[],"cosmetics":{"weapon_paints":[paint]}}})):
                self.assertFalse(self.call(method,value)["result"]["isError"])
                self.assertEqual(self.client.calls[-1],(method,value))
            before=list(self.client.calls)
            for value in ({**paint,"paint_kit":999},{**paint,"wear":.01},{**paint,"wear":.8},
                          {**paint,"item_def":5027,"paint_kit":10006}):
                with self.subTest(value=value):
                    self.assertTrue(self.call("set_player_weapon_paint",value)["result"]["isError"])
            self.assertEqual(self.client.calls,before)
            for value in ({**paint,"item_def":True},{**paint,"paint_kit":282.0},{**paint,"seed":1001},
                          {**paint,"seed":True},{**paint,"wear":float("nan")},{**paint,"steam_id":"0"}):
                with self.subTest(value=value):
                    self.assertEqual(self.call("set_player_weapon_paint",value)["error"]["code"],-32602)
            self.assertEqual(self.client.calls,before)
            self.client.failure="weapon adapter unavailable for this build"
            self.assertTrue(self.call("set_player_weapon_paint",paint)["result"]["isError"])

    def test_gloves_are_typed_catalog_scoped_and_persisted_without_bypassing_native(self):
        catalog={"weapons":[{"item_def":7,"kind":"weapon","paint_kits":[282]}],
                 "gloves":[{"item_def":5030,"kind":"glove","paint_kits":[10006]}],
                 "paint_kits":[{"id":282,"wear_min":.1,"wear_max":.7},{"id":10006,"wear_min":.06,"wear_max":.8}]}
        glove={"steam_id":"76561198000000001","nickname":"Player","item_def":5030,"paint_kit":10006,"wear":.4,"seed":42}
        with patch("cs2_mvp.live_mcp.session_descriptor",return_value={"game_directory":"/session/game"}), \
             patch("cs2_mvp.cosmetics_catalog.installed_catalog",return_value=catalog) as installed:
            for method,value in (("set_player_gloves",glove),("set_player_gloves",{k:v for k,v in glove.items() if k!="wear"}),
                                 ("reset_player_gloves",{"steam_id":glove["steam_id"]}),
                                 ("set_project",{"cosmetics":{"gloves":[glove]}}),
                                 ("import_project",{"project":{"keys":[],"cosmetics":{"gloves":[glove]}}})):
                with self.subTest(method=method):
                    self.assertFalse(self.call(method,value)["result"]["isError"])
                    self.assertEqual(self.client.calls[-1],(method,value))
            installed.assert_called_with("/session/game")
            self.client.project["cosmetics"]={"gloves":[glove]}
            with tempfile.TemporaryDirectory() as directory:
                path=Path(directory)/"gloves.json"
                call_tool(self.client,"cs2_camera_save_project",{"path":str(path)})
                call_tool(self.client,"cs2_camera_load_project",{"path":str(path)})
                self.assertEqual(self.client.calls[-1],("import_project",{"project":self.client.project}))
                self.assertEqual(json.loads(path.read_text())["cosmetics"]["gloves"],[glove])
                before=list(self.client.calls)
                saved=json.loads(path.read_text());saved["cosmetics"]["gloves"][0]["paint_kit"]=282;path.write_text(json.dumps(saved))
                with self.assertRaises(ValueError):
                    call_tool(self.client,"cs2_camera_load_project",{"path":str(path)})
                self.assertEqual(self.client.calls,before)
            before=list(self.client.calls)
            for value in ({**glove,"item_def":7,"paint_kit":282},{**glove,"paint_kit":282},{**glove,"wear":.01},{**glove,"wear":.9}):
                with self.subTest(value=value):
                    self.assertTrue(self.call("set_player_gloves",value)["result"]["isError"])
            self.assertTrue(self.call("set_project",{"cosmetics":{"gloves":[glove,glove]}})["result"]["isError"])
            for value in ({**glove,"item_def":True},{**glove,"paint_kit":10006.0},{**glove,"seed":True},{**glove,"wear":float("nan")},
                          {**glove,"raw_address":1}):
                with self.subTest(value=value):
                    self.assertEqual(self.call("set_player_gloves",value)["error"]["code"],-32602)
            self.assertEqual(self.call("reset_player_gloves",{})["error"]["code"],-32602)
            self.assertEqual(self.call("reset_player_gloves",{"steam_id":glove["steam_id"],"item_def":5030})["error"]["code"],-32602)
            self.assertEqual(self.call("set_project",{"cosmetics":{"gloves":[glove]*129}})["error"]["code"],-32602)
            self.assertEqual(self.client.calls,before)
            self.client.failure="current glove item cannot be safely copied"
            self.assertTrue(self.call("set_player_gloves",glove)["result"]["isError"])


if __name__ == "__main__":
    unittest.main()
