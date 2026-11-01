import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.live import LIVE_METHODS, LiveError
from cs2_mvp.live_mcp import GUIDE_URI, ProtocolError, Server, TOOLS, call_tool, serve


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
        # These protocol/schema tests isolate native forwarding. Mandatory map
        # preflight and CAS behavior have dedicated real-geometry guard tests.
        guard = patch("cs2_mvp.live_mcp.guarded_camera_call", side_effect=lambda client, method, args: client.request(method, args))
        guard.start(); self.addCleanup(guard.stop)
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

    def test_spatial_and_channel_bezier_fields_forward_without_stripping_nulls(self):
        controls = {"position_in": None, "position_out": [10, -20, 5],
                    "rotation_in": [-5, 20, 180], "rotation_out": None,
                    "fov_in": -7, "fov_out": None,
                    "position_easing": [.2, 0, .8, 1], "rotation_easing": None,
                    "fov_easing": [0, .3, 1, .7]}
        for method, arguments in (("add_key", {"time": 1, **controls}),
                                  ("update_key", {"id": "camera-key", **controls})):
            with self.subTest(method=method):
                self.assertFalse(self.call(method, arguments)["result"]["isError"])
                self.assertEqual(self.client.calls[-1], (method, arguments))
        value = {"interpolation": "bezier", "rotation_interpolation": "bezier",
                 "fov_interpolation": "bezier", "keys": [
                     {"id": "camera-key", "time": 0, "position": [1, 2, 3], "angles": [0, 90, 0], **controls}]}
        for method, arguments in (("set_project", value), ("import_project", {"project": value}),
                                  ("validate_camera_path", {"project": value})):
            with self.subTest(method=method):
                if method == "validate_camera_path":
                    with patch("cs2_mvp.live_mcp.validate_camera_path", return_value={"passed": False}) as validate_path:
                        self.assertFalse(self.call(method, arguments)["result"]["isError"])
                    validate_path.assert_called_once_with(self.client, project=value)
                else:
                    self.assertFalse(self.call(method, arguments)["result"]["isError"])
                    self.assertEqual(self.client.calls[-1], (method, arguments))

    def test_bezier_schemas_reject_invalid_controls_and_unknown_channel_modes(self):
        for field, invalid in (("position_in", [0, 0]), ("position_out", [0, 0, float("inf")]),
                               ("rotation_in", [0, True, 0]), ("rotation_out", [0, 0, 1000001]),
                               ("fov_in", float("nan")), ("fov_out", True), ("fov_out", -1000001),
                               ("position_easing", [0, 0, 1, 1.01]),
                               ("rotation_easing", [0, 0, 1]), ("fov_easing", [0, 0, 1, float("nan")]),
                               ("easing", None)):
            with self.subTest(field=field, invalid=invalid):
                self.assertEqual(self.call("update_key", {"id": "key", field: invalid})["error"]["code"], -32602)
        for field in ("interpolation", "rotation_interpolation", "fov_interpolation"):
            for invalid in (None, "cubic", True):
                for method, arguments in (("set_project", {field: invalid}),
                                          ("import_project", {"project": {"keys": [], field: invalid}})):
                    with self.subTest(field=field, invalid=invalid, method=method):
                        self.assertEqual(self.call(method, arguments)["error"]["code"], -32602)
        self.assertEqual(self.client.calls, [])

    def test_preset_matches_native_name_parameter(self):
        self.call("apply_preset", {"name": "dolly", "distance": 200, "duration": 5})
        self.assertEqual(self.client.calls[-1][1]["name"], "dolly")

    def test_editor_preferences_are_typed_partial_and_never_project_edits(self):
        patch = {"time_unit": "frames", "frame_rate": 1000, "snap_mode": "frame", "resize_mode": "scale",
                 "timeline_rotation": False, "ui_scale": 1.2, "panel_opacity": .8, "blur_enabled": False,
                 "workspace_source": "demo", "workspace_tool": "map", "workspace_open": False,
                 "inspector_section": "shot", "demo_navigation_pinned": True}
        self.assertFalse(self.call("set_editor_preferences", patch)["result"]["isError"])
        self.assertEqual(self.client.calls[-1], ("set_editor_preferences", patch))
        self.client.calls.clear()
        for value in ({"time_unit": "minutes"}, {"frame_rate": 0}, {"frame_rate": True},
                      {"snap_mode": "second"}, {"resize_mode": None}, {"timeline_height": 421},
                      {"timeline_fov": 0}, {"ui_scale": float("nan")}, {"panel_opacity": .2},
                      {"workspace_source": "shot"}, {"workspace_tool": "scene"},
                      {"inspector_section": "curves"}, {"workspace_open": 1}, {"demo_navigation_pinned": None},
                      {"keys": []}, {"duration": 10}):
            with self.subTest(value=value):
                self.assertEqual(self.call("set_editor_preferences", value)["error"]["code"], -32602)
        self.assertEqual(self.client.calls, [])

    def test_select_player_uses_exact_steamid_and_rejects_console_or_nickname_targets(self):
        result = self.call("select_player", {"steam_id": "76561198000000000"})
        self.assertFalse(result["result"]["isError"])
        self.assertEqual(self.client.calls[-1], ("select_player", {"steam_id": "76561198000000000"}))
        self.client.calls.clear()
        for arguments in ({"nickname": "Player"}, {"steam_id": "Player; quit"},
                          {"steam_id": "76561198000000000", "index": 7}):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.call("select_player", arguments)["error"]["code"], -32602)
        self.assertEqual(self.client.calls, [])

    def test_demo_playback_limits_use_typed_absolute_ticks_without_project_edits(self):
        for patch in ({"enabled": True, "in_tick": 100, "out_tick": 250},
                      {"in_tick": 120}, {"enabled": False}):
            self.assertFalse(self.call("set_demo_playback_range", patch)["result"]["isError"])
            self.assertEqual(self.client.calls[-1], ("set_demo_playback_range", patch))
        self.client.calls.clear()
        for patch in ({"enabled": 1}, {"in_tick": True}, {"in_tick": -1},
                      {"out_tick": 0}, {"out_tick": 1.5}, {"in_tick": 2000000001},
                      {"duration": 5}, {"exec": "quit"}):
            with self.subTest(patch=patch):
                self.assertEqual(self.call("set_demo_playback_range", patch)["error"]["code"], -32602)
        self.assertEqual(self.client.calls, [])

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

    def test_timeline_marker_filter_uses_recorded_steamid_or_null(self):
        for steam_id in ("76561198000000000", "18446744073709551615", None):
            result = self.call("select_timeline_player", {"steam_id": steam_id})
            self.assertFalse(result["result"]["isError"])
            self.assertEqual(self.client.calls[-1], ("select_timeline_player", {"steam_id": steam_id}))
        self.client.calls.clear()
        for arguments in ({}, {"nickname": "L201742"}, {"steam_id": True}, {"steam_id": 7},
                          {"steam_id": "0"}, {"steam_id": "076561198000000000"},
                          {"steam_id": "18446744073709551616"}, {"steam_id": "7; quit"},
                          {"steam_id": None, "index": 7}):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.call("select_timeline_player", arguments)["error"]["code"], -32602)
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

    def test_portable_file_roundtrip_preserves_curve_fields_and_metadata(self):
        self.client.project.update(format="cs2-camera-project", version=1, interpolation="bezier",
                                   rotation_interpolation="bezier", fov_interpolation="bezier", keys=[
                                       {"id": "key", "time": 0, "position": [1, 2, 3], "angles": [0, 0, 0],
                                        "position_in": None, "position_out": [5, 6, 7],
                                        "rotation_in": [-10, 0, 20], "rotation_out": None,
                                        "fov_in": None, "fov_out": 12, "position_easing": None,
                                        "rotation_easing": [0, 0, 1, 1], "fov_easing": [.2, .3, .8, .7]}])
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "curve.json")
            call_tool(self.client, "cs2_camera_save_project", {"path": path})
            call_tool(self.client, "cs2_camera_load_project", {"path": path})
            self.assertEqual(self.client.calls[-1], ("import_project", {"project": self.client.project}))
            self.assertEqual(json.loads(Path(path).read_text()), self.client.project)

    def test_portable_files_validate_curve_fields_before_native_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid-curve.json"
            for value in ({"keys": [], "rotation_interpolation": "unknown"},
                          {"keys": [{"id": "key", "fov_out": float("nan")}]},
                          {"keys": [{"id": "key", "position_easing": [0, 0, 1, 2]}]}):
                path.write_text(json.dumps(value))
                with self.subTest(value=value), self.assertRaises(ProtocolError):
                    call_tool(self.client, "cs2_camera_load_project", {"path": str(path)})
            self.assertEqual(self.client.calls, [])

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

    def test_scene_postprocess_gates_require_booleans_and_preserve_native_failures(self):
        settings_schema = next(tool for tool in TOOLS if tool["name"] == "cs2_camera_set_scene")["inputSchema"]["properties"]["settings"]["properties"]
        self.assertEqual(settings_schema["bloom_strength"]["minimum"], -1)
        self.assertIn("-1 restores automatic strength", settings_schema["bloom_strength"]["description"])
        for method, arguments in (("set_scene", {"settings": {"bloom_strength": -1}}),
                                  ("set_project", {"scene": {"bloom_strength": -1}}),
                                  ("import_project", {"project": {"keys": [], "scene": {"bloom_strength": -1}}})):
            self.assertFalse(self.call(method, arguments)["result"]["isError"])
            self.assertEqual(self.client.calls[-1], (method, arguments))
        for value in (-1.01, 10.01, True, float("nan")):
            self.assertEqual(self.call("set_scene", {"settings": {"bloom_strength": value}})["error"]["code"], -32602)
        gates = {"postprocess_enabled": True, "bloom_enabled": False}
        self.assertFalse(self.call("set_scene", {"settings": gates})["result"]["isError"])
        self.assertEqual(self.client.calls[-1], ("set_scene", {"settings": gates}))
        self.assertFalse(self.call("set_project", {"scene": gates})["result"]["isError"])
        self.assertEqual(self.client.calls[-1], ("set_project", {"scene": gates}))
        self.assertFalse(self.call("import_project", {"project": {"keys": [], "scene": gates}})["result"]["isError"])
        before = list(self.client.calls)
        for gate in gates:
            for value in (0, 1, 1.0, "true", None):
                with self.subTest(gate=gate, value=value):
                    self.assertEqual(self.call("set_scene", {"settings": {gate: value}})["error"]["code"], -32602)
                    self.assertEqual(self.call("set_project", {"scene": {gate: value}})["error"]["code"], -32602)
        self.assertEqual(self.client.calls, before)
        self.client.failure = "bloom_enabled is unavailable on this game build"
        response = self.call("set_scene", {"settings": {"bloom_enabled": True}})["result"]
        self.assertTrue(response["isError"])
        self.assertIn("bloom_enabled is unavailable", response["content"][0]["text"])

    def test_combat_scene_switches_are_strict_booleans_in_all_project_entry_points(self):
        settings = {"xray_enabled": True, "blood_enabled": False, "metal_sparks_enabled": True}
        schema = next(tool for tool in TOOLS if tool["name"] == "cs2_camera_set_scene")["inputSchema"]["properties"]["settings"]["properties"]
        for name in settings:
            self.assertEqual(schema[name]["type"], "boolean")
        self.assertIn("metal impacts only", schema["metal_sparks_enabled"]["description"])
        self.assertIn("count and spread are unavailable", schema["metal_sparks_enabled"]["description"])
        for method, arguments in (("set_scene", {"settings": settings}), ("set_project", {"scene": settings}),
                                  ("import_project", {"project": {"keys": [], "scene": settings}})):
            self.assertFalse(self.call(method, arguments)["result"]["isError"])
            self.assertEqual(self.client.calls[-1], (method, arguments))
        before = list(self.client.calls)
        for name in settings:
            for value in (0, 1, .5, "true", None):
                for method, arguments in (("set_scene", {"settings": {name: value}}),
                                          ("set_project", {"scene": {name: value}}),
                                          ("import_project", {"project": {"keys": [], "scene": {name: value}}})):
                    with self.subTest(name=name, value=value, method=method):
                        self.assertEqual(self.call(method, arguments)["error"]["code"], -32602)
        self.assertEqual(self.client.calls, before)
        for unsupported in ("blood_amount", "spark_count", "spark_spread"):
            self.assertEqual(self.call("set_scene", {"settings": {unsupported: 2}})["error"]["code"], -32602)
        self.client.failure = "metal_sparks_enabled is unavailable on this game build"
        response = self.call("set_scene", {"settings": {"metal_sparks_enabled": False}})["result"]
        self.assertTrue(response["isError"])
        self.assertIn("metal_sparks_enabled is unavailable", response["content"][0]["text"])

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

    def test_removed_skin_and_appearance_tools_never_reach_native(self):
        names = {tool["name"] for tool in TOOLS}
        removed = ("set_player_weapon_paint", "reset_player_weapon_paint", "set_player_gloves",
                   "reset_player_gloves", "apply_appearance_pack", "reset_appearance", "probe_appearance_pack")
        self.assertEqual(len(TOOLS),36)
        for method in removed:
            with self.subTest(method=method):
                self.assertNotIn(method,LIVE_METHODS)
                self.assertNotIn("cs2_camera_"+method,names)
                self.assertEqual(self.call(method,{})["error"]["code"],-32602)
        self.assertEqual(self.client.calls,[])

    def test_legacy_project_skin_fields_fail_without_silent_removal(self):
        legacy = ({"appearance":{}}, {"appearance":{"path":"/private/pack","models":["agents/models/a.vmdl"]}},
                  {"cosmetics":{"weapon_paints":[]}}, {"cosmetics":{"gloves":[]}},
                  {"cosmetics":{"gloves":[{"steam_id":"76561198000000001","item_def":5030,"paint_kit":10006}]}})
        for fields in legacy:
            with self.subTest(fields=fields), tempfile.TemporaryDirectory() as directory:
                project={"keys":[],**fields}
                for method,args in (("set_project",fields),("import_project",{"project":project})):
                    self.assertEqual(self.call(method,args)["error"]["code"],-32602)
                path=Path(directory)/"legacy.json";path.write_text(json.dumps(project))
                with self.assertRaisesRegex(ProtocolError,"not supported"):
                    call_tool(self.client,"cs2_camera_load_project",{"path":str(path)})
                self.assertEqual(json.loads(path.read_text()),project)
        self.assertEqual(self.client.calls,[])

    def test_public_catalog_only_lists_agents(self):
        with patch("cs2_mvp.cosmetics_catalog.query_catalog",return_value={"items":[]}) as catalog:
            self.assertEqual(call_tool(self.client,"cs2_assets_cosmetics_catalog",{"kind":"agents","team":"CT"}),{"items":[]})
            catalog.assert_called_once_with(kind="agents",team="CT")
            for kind in ("weapons","knives","gloves"):
                with self.subTest(kind=kind),self.assertRaises(ProtocolError):
                    call_tool(self.client,"cs2_assets_cosmetics_catalog",{"kind":kind})
            self.assertEqual(catalog.call_count,1)
        self.assertEqual(self.client.calls,[])


if __name__ == "__main__":
    unittest.main()
