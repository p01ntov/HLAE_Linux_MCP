import json
import copy
import unittest

from cs2_mvp.capture_environment import build_environment_sequence, validate_environment, validate_environment_log
from cs2_mvp.csdm import build_actions


class CaptureEnvironmentTests(unittest.TestCase):
    def test_combat_switches_preserve_exact_values_in_actions_and_require_native_readback(self):
        settings = {"xray_enabled": True, "blood_enabled": False, "metal_sparks_enabled": True}
        environment = {"scene": settings}
        self.assertEqual(validate_environment(environment), environment)
        sequence = build_environment_sequence(build_actions(1000, 1008, "", "combat", 1000, camera_only=True), environment)
        action = next(item for item in sequence["actions"] if item["cmd"].startswith("cs2_mvp_scene "))
        self.assertEqual(json.loads(bytes.fromhex(action["cmd"].split()[1])), settings)
        def evidence(values):
            return ("CS2_MVP_ENVIRONMENT_ABI 1\nCS2_MVP_SCENE_APPLIED tick=936 settings=" + json.dumps(values) +
                    "\nCS2_MVP_ENVIRONMENT_RESTORED tick=1072 reason=quit_after_flush\n")
        result = validate_environment_log(evidence(settings), environment, 1000, 1008)
        self.assertEqual(result["scene"]["readback"], settings)
        self.assertFalse(result["scene"]["render_verified"])
        for name in settings:
            for actual in ({key: value for key, value in settings.items() if key != name},
                           {**settings, name: not settings[name]}, {**settings, name: int(settings[name])}):
                with self.subTest(name=name, actual=actual), self.assertRaisesRegex(RuntimeError, "readback differs"):
                    validate_environment_log(evidence(actual), environment, 1000, 1008)
            with self.subTest(invalid=name), self.assertRaises(ValueError):
                validate_environment({"scene": {name: 1}})
        self.assertEqual(environment, {"scene": settings})

    def test_internal_actions_are_hex_at_preroll_after_standard_setup(self):
        expected = {"scene": {"dof_override": True, "dof_near_crisp": 100, "hud_visible": False}}
        base = build_actions(1000, 1008, "", "test", 1000, camera_only=True)
        sequence = build_environment_sequence(base, expected)
        item = next(a for a in sequence["actions"] if a["cmd"].startswith("cs2_mvp_scene "))
        self.assertEqual(item["tick"], 936)
        self.assertEqual(json.loads(bytes.fromhex(item["cmd"].split()[1])), expected["scene"])
        self.assertNotIn("cs2_mvp_scene", str(base))
        self.assertLess(sequence["actions"].index(item), next(i for i,a in enumerate(sequence["actions"]) if a["cmd"].startswith("startmovie ")))

    def test_actual_scene_readback_not_planned_json_is_required(self):
        request = {"scene": {"dof_override": True, "exposure": 1.25, "hud_visible": False}}
        applied = 'CS2_MVP_ENVIRONMENT_ABI 1\nCS2_MVP_SCENE_APPLIED tick=936 settings={"dof_override":true,"exposure":1.25,"hud_visible":false}\n'
        evidence = validate_environment_log(applied, request, 1000)
        self.assertEqual(evidence["scene"]["setup_tick"], 936)
        self.assertFalse(evidence["scene"]["render_verified"])
        for bad in (applied.replace("true", "false"), applied.replace("1.25", "1.5"), applied.replace("tick=936", "tick=1000"),
                    applied + "CS2_MVP_ENVIRONMENT_ERROR restore failed", "CS2_MVP_ENVIRONMENT_ABI 1\n" + json.dumps(request)):
            with self.subTest(log=bad), self.assertRaises(RuntimeError):
                validate_environment_log(bad, request, 1000)
        with self.assertRaisesRegex(RuntimeError,"restoration"):
            validate_environment_log(applied,request,1000,1008)
        completed=validate_environment_log(applied+'CS2_MVP_ENVIRONMENT_RESTORED tick=1072 reason=quit_after_flush',request,1000,1008)
        self.assertEqual(completed["restored"],{"tick":1072,"reason":"quit_after_flush"})

    def test_removed_skin_fields_fail_before_native_actions_or_log_acceptance(self):
        base=build_actions(1000,1008,"","test",1000,camera_only=True)
        for spec in ({"appearance":{}},{"appearance":{"path":"/private/pack","models":["agents/models/a.vmdl"]}},
                     {"cosmetics":{"weapon_paints":[]}},{"cosmetics":{"gloves":[]}},
                     {"cosmetics":{"weapon_paints":[{"steam_id":"76561198000000000","item_def":7,"paint_kit":282}]}},
                     {"cosmetics":{"gloves":[{"steam_id":"76561198000000000","item_def":5030,"paint_kit":10006}]}}):
            before=copy.deepcopy(spec)
            with self.subTest(spec=spec):
                for check in (lambda:validate_environment(spec),lambda:build_environment_sequence(base,spec),
                              lambda:validate_environment_log("CS2_MVP_ENVIRONMENT_ABI 1",spec,1000)):
                    with self.assertRaisesRegex(ValueError,"not supported"):
                        check()
                self.assertEqual(spec,before)
                self.assertFalse(any(action.get("cmd","").startswith("cs2_mvp_") for action in base["actions"]))

    def test_supported_agent_and_palette_settings_are_preserved(self):
        for cosmetics in ({"players":[{"steam_id":"76561198000000000","agent_item_def":5036}]},
                          {"team_palette":{"enabled":True,"t":[255,0,0],"ct":[0,0,255]}}):
            with self.subTest(cosmetics=cosmetics):
                self.assertEqual(validate_environment({"cosmetics":cosmetics}),{"cosmetics":cosmetics})
        inactive={"players":[],"team_palette":{"enabled":False,"t":[255,0,0],"ct":[0,0,255]}}
        self.assertEqual(validate_environment({"cosmetics":inactive}),{})
        self.assertEqual(validate_environment({"cosmetics":{}}),{})
        for spec in ({"scene":{"quit":True}},{"scene":{"hud_visible":1}},{"scene":{"shading_mode":.5}},
                     {"cosmetics":{"players":False}},{"cosmetics":{"team_palette":{"enabled":False}}}):
            with self.subTest(spec=spec),self.assertRaises(ValueError):
                validate_environment(spec)

    def test_agent_capture_requires_real_bindings_before_movie_and_zero_restoration(self):
        settings={"players":[{"steam_id":"76561198000000000","agent_item_def":4726}]}
        proof={"settings":settings,"players":{"stale":False,"needs_demo_refresh":False,
            "players":[{"steam_id":"76561198000000000","agent_item_def":4726,"agent_applied":True,"model_ready":True}]}}
        def log(value=proof,restore="players_owned=0 weapons_owned=0"):
            return ("CS2_MVP_ENVIRONMENT_ABI 1\nCS2_MVP_COSMETICS_APPLIED tick=936 proof="+json.dumps(value)+
                    "\nExecuting: startmovie test tga\nCS2_MVP_COSMETICS_RESTORED tick=1072 "+restore+
                    "\nCS2_MVP_ENVIRONMENT_RESTORED tick=1072 reason=quit_after_flush\n")
        for suffix in (""," gloves_owned=0"):
            good=validate_environment_log(log(restore="players_owned=0 weapons_owned=0"+suffix),{"cosmetics":settings},1000,1008)
            self.assertEqual(good["cosmetics"]["restored"]["players_owned"],0)
            self.assertFalse(good["cosmetics"]["render_verified"])
        for key,value in (("stale",True),("needs_demo_refresh",True),("restoration_pending",True),("error","failed")):
            bad=copy.deepcopy(proof);bad["players"][key]=value
            with self.subTest(key=key),self.assertRaises(RuntimeError):
                validate_environment_log(log(bad),{"cosmetics":settings},1000,1008)
        for key,value in (("agent_applied",False),("model_ready",False),("agent_item_def",5036),("error","failed")):
            bad=copy.deepcopy(proof);bad["players"]["players"][0][key]=value
            with self.subTest(key=key),self.assertRaises(RuntimeError):
                validate_environment_log(log(bad),{"cosmetics":settings},1000,1008)
        for restore in ("players_owned=1 weapons_owned=0","players_owned=0 weapons_owned=1",
                        "players_owned=0 weapons_owned=0 gloves_owned=1"):
            with self.subTest(restore=restore),self.assertRaisesRegex(RuntimeError,"restoration"):
                validate_environment_log(log(restore=restore),{"cosmetics":settings},1000,1008)
        for broken in (log().replace("tick=936","tick=1000"),log().replace("Executing: startmovie","Executing: echo"),
                       log()+"CS2_MVP_ENVIRONMENT_ERROR failed\n"):
            with self.subTest(log=broken),self.assertRaises(RuntimeError):
                validate_environment_log(broken,{"cosmetics":settings},1000,1008)

    def test_palette_capture_checks_each_current_actor_color_readback(self):
        settings={"team_palette":{"enabled":True,"t":[255,0,0],"ct":[0,0,255]}}
        proof={"settings":settings,"players":{"stale":False,"needs_demo_refresh":False,"team_palette":settings["team_palette"],
            "players":[{"steam_id":"76561198000000000","palette_applied":True,"color_ready":True}]}}
        def log(value):
            return "CS2_MVP_ENVIRONMENT_ABI 1\nCS2_MVP_COSMETICS_APPLIED tick=936 proof="+json.dumps(value)+"\nExecuting: startmovie test tga\n"
        self.assertIn("cosmetics",validate_environment_log(log(proof),{"cosmetics":settings},1000))
        for rows in ([],[{"palette_applied":False,"color_ready":True}],[{"palette_applied":True,"color_ready":False}]):
            bad=copy.deepcopy(proof);bad["players"]["players"]=rows
            with self.subTest(rows=rows),self.assertRaises(RuntimeError):
                validate_environment_log(log(bad),{"cosmetics":settings},1000)

    def test_capture_translates_demo_hidden_hud_and_rejects_unwired_player_gameplay(self):
        for hud,visible in (("hidden",False),("demo",True)):
            spec={"presentation":{"view":"free","hud":hud},"scene":{"hud_visible":not visible,"exposure":1.1}}
            self.assertEqual(validate_environment(spec),{"scene":{"hud_visible":visible,"exposure":1.1}})
            self.assertEqual(spec["scene"]["hud_visible"],not visible)
        self.assertEqual(validate_environment({"presentation":{"view":"free"}}),{})
        for presentation in ({"view":"player"},{"hud":"gameplay"},{"view":"player","hud":"hidden"}):
            with self.subTest(presentation=presentation),self.assertRaisesRegex(ValueError,"offline presentation adapter"):
                validate_environment({"presentation":presentation})
        for presentation in ({"view":"free","hud":"gameplay"},{"hud":"bogus"},{"camera":"player"}):
            with self.subTest(presentation=presentation),self.assertRaises(ValueError):
                validate_environment({"presentation":presentation})


if __name__ == "__main__":
    unittest.main()
