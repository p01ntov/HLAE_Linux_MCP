import json
import copy
import struct
from pathlib import Path
import tempfile
import unittest

from cs2_mvp.capture_environment import build_environment_sequence, validate_environment, validate_environment_log
from cs2_mvp.csdm import build_actions


class CaptureEnvironmentTests(unittest.TestCase):
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

    def test_appearance_requires_actual_mount_reload_and_matching_pack(self):
        expected = {"appearance": {"path": "/private/pack", "models": ["agents/models/a.vmdl"]}}
        state = {"active": True, "path": "/private/pack", "models": ["agents/models/a.vmdl"],
                 "mounted_verified": True, "reload_requested": True, "render_verified": False}
        def log():
            return "CS2_MVP_ENVIRONMENT_ABI 1\nCS2_MVP_APPEARANCE_APPLIED tick=936 state=" + json.dumps(state)
        self.assertTrue(validate_environment_log(log(), expected, 1000)["appearance"]["readback"]["mounted_verified"])
        for key, value in (("mounted_verified", False), ("path", "/wrong"), ("models", [])):
            previous = state[key]
            state[key] = value
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                validate_environment_log(log(), expected, 1000)
            state[key] = previous

    def test_unknown_scene_ids_and_unsafe_model_paths_fail_before_launch(self):
        for spec in ({"scene": {"quit": True}}, {"scene": {"hud_visible": 1}}, {"scene": {"shading_mode": .5}}):
            with self.subTest(spec=spec), self.assertRaises(ValueError):
                validate_environment(spec)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                validate_environment({"appearance": {"path": directory, "models": ["../bad.vmdl"]}})
            with self.assertRaisesRegex(ValueError, "no verified safe model refresh"):
                validate_environment({"appearance": {"path": directory, "models": ["agents/models/a.vmdl"]}})
            good = validate_environment({"appearance": {"path": directory, "models": ["agents/models/a.vmdl"]}}, allow_experimental_appearance=True)
            self.assertEqual(good["appearance"]["path"], str(Path(directory).resolve()))

    def test_player_cosmetics_are_preserved_for_verified_native_actions(self):
        for cosmetics in ({"players":[{"steam_id":"76561198000000000","agent_item_def":5036}]},
                          {"weapon_paints":[{"steam_id":"76561198000000000","item_def":7,"paint_kit":282}]},
                          {"team_palette":{"enabled":True,"t":[255,0,0],"ct":[0,0,255]}}):
            with self.subTest(cosmetics=cosmetics):
                self.assertEqual(validate_environment({"cosmetics":cosmetics}),{"cosmetics":cosmetics})
        self.assertEqual(validate_environment({"cosmetics":{}}),{})

    def test_cosmetics_require_real_before_movie_bindings_and_completed_material_jobs(self):
        settings={"players":[{"steam_id":"76561198000000000","agent_item_def":4726}],
                  "weapon_paints":[{"steam_id":"76561198000000000","item_def":7,"paint_kit":282,"seed":1,"wear":.1}]}
        proof={"settings":settings,
               "players":{"stale":False,"needs_demo_refresh":False,"players":[{"steam_id":"76561198000000000","agent_item_def":4726,"agent_applied":True,"model_ready":True}]},
               "weapon_cosmetics":{"stale":False,"weapons":[{"steam_id":"76561198000000000","item_def":7,"paint_kit":282,"seed":1,"wear":.10000000149,
                    "applied":True,"ready":True,"materials_pending":False,"material_jobs":{"world":0,"hud":0}}]}}
        def log(value=proof):
            return ('CS2_MVP_ENVIRONMENT_ABI 1\nCS2_MVP_COSMETICS_APPLIED tick=936 proof='+json.dumps(value)+'\nExecuting: startmovie test tga\n'
                    'CS2_MVP_COSMETICS_RESTORED tick=1072 players_owned=0 weapons_owned=0\nCS2_MVP_ENVIRONMENT_RESTORED tick=1072 reason=quit_after_flush\n')
        good=validate_environment_log(log(),{"cosmetics":settings},1000,1008)
        self.assertEqual(good["cosmetics"]["restored"]["players_owned"],0)
        self.assertFalse(good["cosmetics"]["render_verified"])
        weapon_only=copy.deepcopy(proof);weapon_only["settings"]={"weapon_paints":settings["weapon_paints"]};weapon_only.pop("players")
        self.assertIn("cosmetics",validate_environment_log(log(weapon_only),{"cosmetics":weapon_only["settings"]},1000,1008))
        for section,field,value in (("players","stale",True),("players","needs_demo_refresh",True)):
            bad=json.loads(json.dumps(proof));bad[section][field]=value
            with self.subTest(field=field),self.assertRaises(RuntimeError):
                validate_environment_log(log(bad),{"cosmetics":settings},1000,1008)
        for field,value in (("applied",False),("materials_pending",True),("material_jobs",{"world":1,"hud":0}),("material_jobs",{"world":False,"hud":0}),("paint_kit",300)):
            bad=json.loads(json.dumps(proof));bad["weapon_cosmetics"]["weapons"][0][field]=value
            with self.subTest(field=field),self.assertRaises(RuntimeError):
                validate_environment_log(log(bad),{"cosmetics":settings},1000,1008)
        for broken in (log().replace('tick=936','tick=1000'),log().replace('players_owned=0','players_owned=1'),log().replace('Executing: startmovie','Executing: echo'),
                       log()+ 'CS2_MVP_ENVIRONMENT_ERROR failed\n'):
            with self.subTest(log=broken),self.assertRaises(RuntimeError):
                validate_environment_log(broken,{"cosmetics":settings},1000,1008)

    def test_inactive_saved_cosmetics_do_not_block_capture_but_malformed_state_does(self):
        inactive={"players":[],"weapon_paints":[],"gloves":[],"team_palette":{"enabled":False,"t":[255,0,0],"ct":[0,0,255]}}
        self.assertEqual(validate_environment({"cosmetics":inactive}),{})
        for cosmetics in ({"players":False},{"team_palette":{"enabled":False}},{"weapon_paints":[{}]},
                          {"weapon_paints":[],"unknown":1}):
            with self.subTest(cosmetics=cosmetics),self.assertRaises(ValueError):
                validate_environment({"cosmetics":cosmetics})

    def test_active_gloves_remain_blocked_without_current_capture_verification(self):
        settings={"gloves":[{"steam_id":"76561198000000000","item_def":5030,"paint_kit":10006,"seed":1,"wear":.1}]}
        with self.assertRaisesRegex(ValueError,"Glove capture is unavailable"):
            validate_environment({"cosmetics":settings})
        self.assertEqual(validate_environment({"cosmetics":settings},allow_glove_capture=True),{"cosmetics":settings})
        mixed={**settings,"players":[{"steam_id":"76561198000000000","agent_item_def":4726}]}
        with self.assertRaisesRegex(ValueError,"Glove capture is unavailable"):
            validate_environment({"cosmetics":mixed})
        self.assertEqual(len(mixed["gloves"]),1)

    def test_future_glove_capture_requires_actual_item_materials_and_zero_restoration(self):
        settings={"gloves":[{"steam_id":"76561198000000000","item_def":5030,"paint_kit":10006,"seed":1,"wear":.1}]}
        def attribute(definition,value):
            return {"definition":definition,"value_bits":struct.unpack("<I",struct.pack("<f",value))[0],"set_bonus":0}
        row={**settings["gloves"][0],"ready":True,"item_readable":True,"applied":True,"world_ready":True,"hud_ready":True,
             "world_pose_ready":True,"hud_pose_ready":True,"pose_bones":{"world":53,"hud":53},
             "world_render_ready":True,"hud_render_ready":True,"hud_render_required":True,"hud_parent_render_enabled":True,
             "needs_refresh":False,"restoration_unresolved":False,"materials_pending":False,"material_jobs":{"world":0,"hud":0},
             "actual_item":{"m_iItemDefinitionIndex":5030,"m_bInitialized":1,"attributes":[attribute(6,10006),attribute(7,1),attribute(8,.1)]}}
        proof={"settings":settings,"players":{"stale":False,"needs_demo_refresh":False,"players":[]},"weapon_cosmetics":{},
               "glove_cosmetics":{"stale":False,"restoration_unresolved":False,"gloves":[row]}}
        def log(value=proof,gloves_owned=" gloves_owned=0"):
            return ('CS2_MVP_ENVIRONMENT_ABI 1\nCS2_MVP_GLOVES_POSE_ABI 1\nCS2_MVP_GLOVES_RENDER_ABI 1\nCS2_MVP_COSMETICS_APPLIED tick=936 proof='+json.dumps(value)+'\nExecuting: startmovie test tga\n'
                    'CS2_MVP_COSMETICS_RESTORED tick=1072 players_owned=0 weapons_owned=0'+gloves_owned+'\nCS2_MVP_ENVIRONMENT_RESTORED tick=1072 reason=quit_after_flush\n')
        good=validate_environment_log(log(),{"cosmetics":settings},1000,1008)
        self.assertEqual(good["cosmetics"]["restored"]["gloves_owned"],0)
        free_view=copy.deepcopy(proof)
        free_view["glove_cosmetics"]["gloves"][0].update(hud_render_required=False,hud_parent_render_enabled=False,hud_render_ready=False)
        self.assertIn("cosmetics",validate_environment_log(log(free_view),{"cosmetics":settings},1000,1008))
        for broken in (log().replace('CS2_MVP_GLOVES_POSE_ABI 1\n',''),
                       log().replace('CS2_MVP_GLOVES_POSE_ABI 1','CS2_MVP_GLOVES_POSE_ABI 2'),
                       log()+'CS2_MVP_GLOVES_POSE_ABI 1\n'):
            with self.assertRaisesRegex(RuntimeError,'pose ABI'):
                validate_environment_log(broken,{"cosmetics":settings},1000,1008)
        for broken in (log().replace('CS2_MVP_GLOVES_RENDER_ABI 1\n',''),
                       log().replace('CS2_MVP_GLOVES_RENDER_ABI 1','CS2_MVP_GLOVES_RENDER_ABI 2'),
                       log()+'CS2_MVP_GLOVES_RENDER_ABI 1\n'):
            with self.assertRaisesRegex(RuntimeError,'render ABI'):
                validate_environment_log(broken,{"cosmetics":settings},1000,1008)
        glove_only=copy.deepcopy(proof);glove_only.pop("players");glove_only.pop("weapon_cosmetics")
        self.assertIn("cosmetics",validate_environment_log(log(glove_only),{"cosmetics":settings},1000,1008))
        for field,value in (("item_readable",False),("ready",False),("world_ready",False),("hud_ready",False),("applied",False),
                            ("world_pose_ready",False),("hud_pose_ready",False),("pose_bones",{"world":0,"hud":53}),
                            ("world_render_ready",False),("hud_render_ready",False),("hud_render_ready",None),
                            ("hud_render_required",False),("hud_render_required",0),("hud_render_required",None),
                            ("hud_parent_render_enabled",False),("hud_parent_render_enabled",0),("hud_parent_render_enabled",None),
                            ("pose_bones",{"world":True,"hud":53}),("pose_bones",{"world":53,"hud":513}),
                            ("materials_pending",True),("restoration_unresolved",True),("needs_refresh",True),("item_def",5035),
                            ("paint_kit",10007),("seed",2),("wear",.2),("material_jobs",{"world":0,"hud":1}),
                            ("material_jobs",{"world":False,"hud":0})):
            bad=copy.deepcopy(proof);bad["glove_cosmetics"]["gloves"][0][field]=value
            with self.subTest(field=field),self.assertRaises(RuntimeError):
                validate_environment_log(log(bad),{"cosmetics":settings},1000,1008)
        for field,value in (("stale",True),("restoration_unresolved",True),("gloves",[]),("gloves",[row,row])):
            bad=copy.deepcopy(proof);bad["glove_cosmetics"][field]=value
            with self.subTest(field=field),self.assertRaises(RuntimeError):
                validate_environment_log(log(bad),{"cosmetics":settings},1000,1008)
        for mutate in (lambda p:p.pop("glove_cosmetics"),
                       lambda p:p["glove_cosmetics"]["gloves"][0]["actual_item"]["attributes"][0].update(value_bits=0),
                       lambda p:p["glove_cosmetics"]["gloves"][0]["actual_item"].update(m_iItemDefinitionIndex=5035)):
            bad=copy.deepcopy(proof);mutate(bad)
            with self.assertRaises(RuntimeError):
                validate_environment_log(log(bad),{"cosmetics":settings},1000,1008)
        for suffix in (""," gloves_owned=1"," gloves_owned=0garbage"):
            with self.subTest(suffix=suffix),self.assertRaises(RuntimeError):
                validate_environment_log(log(gloves_owned=suffix),{"cosmetics":settings},1000,1008)

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
