import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp import cosmetics_catalog as cosmetics
from cs2_mvp.live_mcp import ProtocolError, call_tool


SCHEMA_TEXT = r'''
"items_game" {
 "prefabs" {
  "agent_base" { "item_class" "customplayer" "used_by_classes" { "terrorists" "1" "counter-terrorists" "1" } }
  "gun_base" { "item_class" "weapon_rifle" "model_player" "weapons/models/test/rifle.vmdl" "model_world" "weapons/models/test/rifle_world.vmdl" }
  "gun_derived" { "prefab" "gun_base" "item_name" "#GunName" }
  "hands" { "item_class" "wearable_item" "item_slot" "clothing_hands" "used_by_classes" { "terrorists" "1" "counter-terrorists" "1" } }
 }
 "items" {
  "11" { "name" "customplayer_ct_test" "prefab" "agent_base" "item_name" "#CTName" "model_player" "agents/models/ct/test.vmdl" "used_by_classes" { "counter-terrorists" "1" } }
  "12" { "name" "customplayer_t_test" "prefab" "agent_base" "item_name" "#TName" "model_player" "agents/models/t/test.vmdl" "used_by_classes" { "terrorists" "1" } }
  "7" { "name" "weapon_test" "prefab" "gun_derived" "used_by_classes" { "terrorists" "1" } }
  "500" { "name" "weapon_knife_test" "item_class" "weapon_knife" "item_name" "#KnifeName" "model_player" "weapons/models/knife/test.vmdl" "used_by_classes" { "terrorists" "1" "counter-terrorists" "1" } }
  "5027" { "name" "test_gloves" "prefab" "hands" "item_name" "#GloveName" "model_player" "agents/models/shared/glove/test.vmdl" }
 }
 "paint_kits" {
  "282" { "name" "test_red" "description_tag" "#PaintRed" "wear_remap_min" "0.1" "wear_remap_max" "0.7" }
  "10006" { "name" "glove_unassigned" "description_tag" "#PaintGlove" "vmt_path" "materials/models/weapons/customization/paints_gloves/test.vmat" }
 }
 "item_sets" { "collection" { "items" { "[test_red]weapon_test" "1" } } }
 "client_loot_lists" { "rare" { "[test_red]weapon_test" "1" } }
}
'''
TOKENS = {"CTName":"CT Agent","TName":"T Agent","GunName":"Test Rifle","KnifeName":"Test Knife",
          "GloveName":"Test Gloves","PaintRed":"Red Finish","PaintGlove":"Unassigned Gloves"}


class CosmeticsTests(unittest.TestCase):
    def catalog(self):
        return cosmetics.build_catalog(cosmetics.parse_keyvalues(SCHEMA_TEXT),TOKENS)

    def test_prefab_inheritance_localized_names_and_team_restrictions(self):
        catalog=self.catalog()
        ct=cosmetics.validate_agent_team(11,"CT",catalog)
        self.assertEqual(ct["display_name"],"CT Agent")
        self.assertEqual(ct["allowed_teams"],[3])
        self.assertEqual(ct["model"],"agents/models/ct/test.vmdl")
        self.assertIsNone(ct["arms_model"])
        with self.assertRaisesRegex(ValueError,"not allowed"):
            cosmetics.validate_agent_team(11,"T",catalog)
        with self.assertRaises(ValueError):
            cosmetics.validate_agent_team(11,0,catalog)
        self.assertEqual(cosmetics.validate_agent_team(12,2,catalog)["team"],"T")
        rifle=next(item for item in catalog["weapons"] if item["item_def"]==7)
        self.assertEqual(rifle["model_world"],"weapons/models/test/rifle_world.vmdl")
        self.assertEqual(rifle["display_name"],"Test Rifle")

    def test_only_evidenced_paint_pairing_is_exposed_never_glove_id_guessing(self):
        catalog=self.catalog()
        rifle=cosmetics.validate_paint_association(7,282,catalog)
        self.assertEqual(rifle["paint_kits"],[282])
        paint=next(paint for paint in catalog["paint_kits"] if paint["id"]==282)
        self.assertEqual(paint["association_sources"],["item_sets","client_loot_lists"])
        self.assertEqual(catalog["gloves"][0]["paint_kits"],[])
        with self.assertRaisesRegex(ValueError,"not evidenced"):
            cosmetics.validate_paint_association(5027,10006,catalog)

    def test_exact_generated_inventory_icon_evidences_glove_finish(self):
        resource=cosmetics.ICON_PREFIX+"test_gloves_glove_unassigned_light_png.vtex_c"
        catalog=cosmetics.build_catalog(cosmetics.parse_keyvalues(SCHEMA_TEXT),TOKENS,resource_names=[resource])
        self.assertEqual(cosmetics.validate_paint_association(5027,10006,catalog)["paint_kits"],[10006])
        paint=next(paint for paint in catalog["paint_kits"] if paint["id"]==10006)
        self.assertEqual(paint["association_sources"],["vpk_generated_inventory_icon"])
        self.assertEqual(paint["item_icons"],{"5027":resource})
        catalog=cosmetics.build_catalog(cosmetics.parse_keyvalues(SCHEMA_TEXT),TOKENS,
            resource_names=[resource.replace("test_gloves_","test_gloves_extra_")])
        with self.assertRaisesRegex(ValueError,"not evidenced"):
            cosmetics.validate_paint_association(5027,10006,catalog)

    def test_map_based_defaults_are_identified_without_guessing_map_model(self):
        schema=cosmetics.parse_keyvalues(SCHEMA_TEXT)
        ct=schema["items_game"]["items"]["11"]
        ct["name"]="customplayer_ct_map_based"
        ct["model_player"]="agents/models/ctm_sas/ctm_sas.vmdl"
        catalog=cosmetics.build_catalog(schema,TOKENS)
        default=cosmetics.validate_agent_team(11,"CT",catalog)
        self.assertTrue(default["map_based"])
        self.assertEqual(default["model"],"agents/models/ctm_sas/ctm_sas.vmdl")
        self.assertTrue(any("fallback" in note for note in catalog["notes"]))

    def test_parser_handles_quoted_brackets_comments_unicode_and_fails_malformed_or_includes(self):
        value=cosmetics.parse_keyvalues('''// comment\n"lang" {"Tokens" {"[paint]weapon_test" "Имя \\\"quoted\\\""}}'''.encode())
        self.assertIn("[paint]weapon_test",value["lang"]["Tokens"])
        for data in ('"a" {"b"', '"a" "unterminated', '"a" "b" [$WIN32]', '#include "outside.txt"', '"a" {'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                cosmetics.parse_keyvalues(data)

    def test_schema_duplicates_cannot_silently_replace_item_ids_fields_or_prefabs(self):
        duplicates = [
            '"items_game" {"items" {"11" {"name" "customplayer_a"} "11" {"name" "customplayer_b"}}}',
            '"items_game" {"items" {"11" {"name" "customplayer_a" "name" "customplayer_b"}}}',
            '"items_game" {"prefabs" {"agent" {"item_class" "a"} "agent" {"item_class" "b"}} "items" {}}',
            '"items_game" {"paint_kits" {"282" {"name" "a"} "282" {"name" "b"}} "items" {}}',
            '"items_game" {"items" {"11" {"name" "a"}} "items" {"11" {"model_player" "agents/b.vmdl"}}}',
        ]
        for data in duplicates:
            for options in ({}, {"catalog_schema": True}):
                with self.subTest(data=data, options=options), self.assertRaisesRegex(ValueError, "Duplicate|Conflicting"):
                    cosmetics.parse_keyvalues(data, **options)

    def test_installed_schema_allows_disjoint_sections_and_known_ignored_metadata_repeats(self):
        schema = '''"items_game" {
            "items" {"11" {"name" "customplayer_ct"}} "items" {"12" {"name" "customplayer_t"}}
            "prefabs" {"campaign_prefab" {"image_inventory" "a" "image_inventory" "b"}}
            "client_loot_lists" {"case" {"limit_description_to_number_rnd" "1" "limit_description_to_number_rnd" "2"}}
            "campaigns" {"campaign" {"tag" "a" "tag" "b"}} "campaigns" {"other" "c"}
        }'''
        parsed = cosmetics.parse_keyvalues(schema, catalog_schema=True)["items_game"]
        self.assertEqual(set(parsed["items"]), {"11", "12"})
        self.assertEqual(parsed["prefabs"]["campaign_prefab"]["image_inventory"], "b")
        self.assertEqual(parsed["client_loot_lists"]["case"]["limit_description_to_number_rnd"], "2")
        self.assertEqual(parsed["campaigns"]["campaign"]["tag"], "b")
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            cosmetics.parse_keyvalues(schema)

    def test_localization_duplicate_tokens_are_permitted_only_explicitly(self):
        locale = '"lang" {"Tokens" {"Agent" "Old" "Agent" "Новый"}}'
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            cosmetics.parse_keyvalues(locale)
        tokens = cosmetics.parse_keyvalues(locale, allow_duplicate_keys=True)["lang"]["Tokens"]
        self.assertEqual(tokens, {"Agent": "Новый"})

    def test_validation_rejects_bool_float_and_string_aliases_for_numeric_ids(self):
        catalog = self.catalog()
        for item_def in (True, False, 11.0, "11", 0, 65536):
            with self.subTest(agent=item_def), self.assertRaises(ValueError):
                cosmetics.validate_agent_team(item_def, "CT", catalog)
        for team in (True, False, 3.0, "3", "ct", None):
            with self.subTest(team=team), self.assertRaises(ValueError):
                cosmetics.validate_agent_team(11, team, catalog)
        for item_def in (True, 7.0, "7", 0, 65536):
            with self.subTest(item=item_def), self.assertRaises(ValueError):
                cosmetics.validate_paint_association(item_def, 282, catalog)
        for paint in (True, 282.0, "282", -1, 2147483648):
            with self.subTest(paint=paint), self.assertRaises(ValueError):
                cosmetics.validate_paint_association(7, paint, catalog)

    def test_compiled_model_presence_is_index_evidence_and_not_a_render_claim(self):
        self.assertIsNone(self.catalog()["agents"][0]["compiled_model_present"])
        catalog = cosmetics.build_catalog(cosmetics.parse_keyvalues(SCHEMA_TEXT), TOKENS,
            resource_names=["agents/models/ct/test.vmdl_c"])
        ct = cosmetics.validate_agent_team(11, "CT", catalog)
        t = cosmetics.validate_agent_team(12, "T", catalog)
        self.assertEqual(ct["compiled_model"], "agents/models/ct/test.vmdl_c")
        self.assertTrue(ct["compiled_model_present"])
        self.assertFalse(t["compiled_model_present"])
        self.assertNotIn("render_verified", ct)

    def test_mutating_installed_result_does_not_contaminate_cached_catalog(self):
        cached = self.catalog()
        cached["source"] = {"items_game_sha256": "schema-hash"}
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            (game / "csgo").mkdir()
            (game / "csgo/pak01_dir.vpk").write_bytes(b"fixture")
            with patch.object(cosmetics, "find_game", return_value=game), \
                 patch.object(cosmetics, "_installed", return_value=cached) as read_catalog, \
                 patch.object(cosmetics, "_game_provenance", return_value={"client_build_id": "build-id"}):
                first = cosmetics.installed_catalog()
                first["source"]["items_game_sha256"] = "changed"
                first["agents"][0]["allowed_teams"].append(2)
                first["paint_kits"][0]["items"].clear()
                second = cosmetics.installed_catalog()
        self.assertEqual(read_catalog.call_args_list[0], read_catalog.call_args_list[1])
        self.assertEqual(second["source"], {"items_game_sha256": "schema-hash", "client_build_id": "build-id"})
        self.assertEqual(second["agents"][0]["allowed_teams"], [3])
        self.assertEqual(second["paint_kits"][0]["items"], [7])
        self.assertNotIn("client_build_id", cached["source"])

    def test_prefab_cycles_and_unknown_references_cannot_create_guessed_agents(self):
        schema=cosmetics.parse_keyvalues(SCHEMA_TEXT)
        schema["items_game"]["prefabs"]["agent_base"]["prefab"]="agent_base"
        result=cosmetics.build_catalog(schema,TOKENS)
        self.assertEqual(result["agents"],[])
        self.assertEqual(result["unresolved_total"],2)

    def test_catalog_query_filters_team_weapon_and_paint_names_with_bounded_results(self):
        catalog=self.catalog()
        with patch.object(cosmetics,"installed_catalog",return_value=catalog):
            ct=cosmetics.query_catalog(kind="agents",team="CT")
            self.assertEqual([item["item_def"] for item in ct["items"]],[11])
            rifle=cosmetics.query_catalog(kind="weapons",weapon="weapon_test",search="red finish")
            self.assertEqual(rifle["items"][0]["paint_kits"][0]["id"],282)
            gloves=cosmetics.query_catalog(kind="gloves",search="unassigned gloves")
            self.assertEqual(gloves["items"],[])
            self.assertEqual(gloves["unassigned_paint_kits"][0]["id"],10006)
            self.assertEqual(gloves["unassigned_paint_kits"][0]["items"],[])
            knives=cosmetics.query_catalog(kind="knives")
            self.assertEqual([item["item_def"] for item in knives["items"]],[500])
        with patch.object(cosmetics,"installed_catalog",side_effect=AssertionError("Invalid query must fail first")):
            for args in ({"limit":101},{"limit":True},{"limit":1.0},{"offset":-1},{"offset":False},{"offset":0.0},
                         {"team":"both"},{"kind":"inventory"},{"search":"a"*257},{"weapon":{}},{"weapon":""}):
                with self.subTest(args=args), self.assertRaises(ValueError):
                    cosmetics.query_catalog(**args)

    def test_query_byte_bound_paginates_without_skipping_and_rejects_single_oversized_rows(self):
        with patch.object(cosmetics, "installed_catalog", return_value=self.catalog()):
            complete = cosmetics.query_catalog(kind="agents", limit=100)
            sizes = [len(json.dumps(row, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode())
                     for row in complete["items"]]
            with patch.object(cosmetics, "MAX_QUERY_BYTES", max(sizes)):
                first = cosmetics.query_catalog(kind="agents", limit=100)
                second = cosmetics.query_catalog(kind="agents", offset=first["next_offset"], limit=100)
            self.assertEqual(first["next_offset"], 1)
            self.assertTrue(first["page_limited_by_bytes"])
            self.assertIsNone(second["next_offset"])
            self.assertEqual(first["items"] + second["items"], complete["items"])
            with patch.object(cosmetics, "MAX_QUERY_BYTES", sizes[0] - 1), self.assertRaisesRegex(ValueError, "query bound"):
                cosmetics.query_catalog(kind="agents", search="CT Agent")

    def test_staged_catalog_is_private_exclusive_and_native_schema_compatible(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(cosmetics,"installed_catalog",return_value=self.catalog()):
            path=Path(directory)/"cosmetics.json"
            self.assertEqual(cosmetics.stage_catalog(None,path),path)
            catalog=json.loads(path.read_text())
            self.assertEqual((catalog["format"],catalog["version"]),("cs2-camera-cosmetics-catalog",1))
            self.assertEqual(path.stat().st_mode&0o777,0o600)
            self.assertEqual(catalog["agents"][0]["allowed_teams"],[3])
            with self.assertRaises(FileExistsError):
                cosmetics.stage_catalog(None,path)

    def test_mcp_catalog_is_offline_and_does_not_expose_apply_setters(self):
        class NoEngine:
            def request(self,*args):
                raise AssertionError("Catalog does not change any player")
        with patch.object(cosmetics,"installed_catalog",return_value=self.catalog()):
            result=call_tool(NoEngine(),"cs2_assets_cosmetics_catalog",{"kind":"agents","team":"T"})
        self.assertEqual([item["item_def"] for item in result["items"]],[12])
        with self.assertRaises(ProtocolError):
            call_tool(NoEngine(),"cs2_assets_cosmetics_catalog",{"team":"T","steamid":"11"})


if __name__=="__main__":
    unittest.main()
