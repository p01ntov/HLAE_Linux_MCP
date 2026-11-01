from pathlib import Path
import json
import stat
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.csdm import (
    CsdmError,
    RestoreConflict,
    assert_demo_only_launch,
    build_actions,
    patch_gameinfo,
    recover_plugin,
    temporary_plugin,
)


GAMEINFO = b'''"GameInfo"\r\n{\r\n\tFileSystem\r\n\t{\r\n\t\tSearchPaths\r\n\t\t{\r\n\t\t\t// Game csgo/csdm is only a comment\r\n\t\t\tGame_LowViolence csgo_lv\r\n\t\t\t"Game" "csgo/addons/metamod" // keep this\r\n\t\t\tGame csgo\r\n\t\t\tGame core\r\n\t\t}\r\n\t}\r\n}\r\n'''
FAKE_ELF = b"\x7fELF\x02\x01" + bytes(58) + b"non-executable fixture"
STEAM_ID = "76561198000000001"


class ActionsTests(unittest.TestCase):
    def test_native_sequence_keeps_seek_selection_and_capture_separate(self):
        sequence = build_actions(12000, 12640, STEAM_ID, "cs2mvp_abcd", 60, player_slot=4)
        actions = sequence["actions"]
        by_command = {item["cmd"]: item["tick"] for item in actions}
        self.assertEqual(by_command["demo_gototick 11935"], 96)
        self.assertEqual(by_command["spec_player 4"], 11936)
        self.assertEqual(by_command["spec_mode 1"], 11936)
        self.assertLess([a["cmd"] for a in actions].index("spec_mode 1"),
                        [a["cmd"] for a in actions].index("spec_player 4"))
        self.assertEqual(by_command["pause_playback"], 11996)
        self.assertEqual(by_command["startmovie cs2mvp_abcd"], 12000)
        self.assertEqual(by_command["endmovie"], 12640)
        self.assertEqual(by_command["quit"], 12704)
        self.assertFalse(any("mirv_" in action["cmd"] for action in actions))
        self.assertEqual(json.loads(json.dumps([sequence]))[0], sequence)

    def test_unresolved_steam_id_early_ticks_and_console_injection_fail(self):
        invalid = [
            ((12000, 12640, STEAM_ID, "safe", 60), {}),
            ((160, 200, STEAM_ID, "safe", 60), {"player_slot": 4}),
            ((192, 250, STEAM_ID, "safe", 60), {"player_slot": 4}),
            ((12000, 12000, STEAM_ID, "safe", 60), {"player_slot": 4}),
            ((12000, 12640, STEAM_ID, "safe;connect_server", 60), {"player_slot": 4}),
            ((12000, 12640, STEAM_ID, "../safe", 60), {"player_slot": 4}),
            ((12000, 12640, STEAM_ID, "safe", 60), {"player_slot": 0}),
        ]
        for args, kwargs in invalid:
            with self.subTest(args=args, kwargs=kwargs), self.assertRaises(ValueError):
                build_actions(*args, **kwargs)

    def test_patch_preserves_custom_paths_comments_and_crlf(self):
        patched = patch_gameinfo(GAMEINFO)
        insertion = b"Game\tcsgo/csdm\r\n\t\t\t"
        self.assertEqual(patched.replace(insertion, b"", 1), GAMEINFO)
        self.assertLess(patched.index(insertion), patched.index(b'"Game" "csgo/addons/metamod"'))
        self.assertEqual(patch_gameinfo(patched), patched)

    def test_duplicate_or_lower_priority_csdm_path_is_rejected(self):
        wrong = GAMEINFO.replace(b"Game csgo\r\n", b"Game csgo/csdm\r\n\t\t\tGame csgo\r\n")
        with self.assertRaises(CsdmError):
            patch_gameinfo(wrong)

    def test_compact_and_bom_keyvalues_are_supported(self):
        for original in [b'"GameInfo" { "FileSystem" { "SearchPaths" { Game csgo } } }',
                         b"\xef\xbb\xbf" + GAMEINFO]:
            patched = patch_gameinfo(original)
            self.assertEqual(patch_gameinfo(patched), patched)


class InstallationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.game = self.root / "game"
        self.csgo = self.game / "csgo"
        self.csgo.mkdir(parents=True)
        self.gameinfo = self.csgo / "gameinfo.gi"
        self.gameinfo.write_bytes(GAMEINFO)
        self.gameinfo.chmod(0o640)
        self.source = self.root / "libserver.so"
        self.source.write_bytes(FAKE_ELF)
        self.demo = self.root / "fixture.dem"
        self.demo.write_bytes(b"fixture")
        self.args = ["-insecure", "-novid", "+playdemo", str(self.demo)]
        stopped = patch("cs2_mvp.csdm._assert_game_stopped")
        stopped.start()
        self.addCleanup(stopped.stop)

    def context(self):
        return temporary_plugin(self.game, self.source, launch_args=self.args, expected_sha256=None)

    def test_restore_exact_bytes_mode_and_preserve_raw_and_addons(self):
        addon = self.csgo / "addons/counterstrikesharp/fixture"
        addon.parent.mkdir(parents=True)
        addon.write_bytes(b"custom addon")
        with self.context() as installation:
            self.assertEqual(installation.plugin_path.read_bytes(), FAKE_ELF)
            self.assertEqual(installation.backup_path.read_bytes(), GAMEINFO)
            manifest = json.loads(installation.manifest_path.read_text())
            self.assertTrue(manifest["plugin_created"])
            raw = installation.recording_directory / "fixture/frame.tga"
            raw.parent.mkdir(parents=True)
            raw.write_bytes(b"frame")
        self.assertEqual(self.gameinfo.read_bytes(), GAMEINFO)
        self.assertEqual(stat.S_IMODE(self.gameinfo.stat().st_mode), 0o640)
        self.assertFalse(installation.plugin_path.exists())
        self.assertFalse(installation.manifest_path.exists())
        self.assertFalse(installation.backup_path.exists())
        self.assertEqual(addon.read_bytes(), b"custom addon")
        self.assertEqual(raw.read_bytes(), b"frame")

    def test_body_exception_rolls_back(self):
        with self.assertRaisesRegex(RuntimeError, "recording failed"):
            with self.context():
                raise RuntimeError("recording failed")
        self.assertEqual(self.gameinfo.read_bytes(), GAMEINFO)
        self.assertFalse((self.csgo / "csdm").exists())

    def test_identical_existing_plugin_is_reused(self):
        plugin = self.csgo / "csdm/bin/linuxsteamrt64/libserver.so"
        plugin.parent.mkdir(parents=True)
        plugin.write_bytes(FAKE_ELF)
        inode = plugin.stat().st_ino
        with self.context() as installation:
            self.assertFalse(installation.plugin_created)
            self.assertEqual(plugin.stat().st_ino, inode)
        self.assertEqual(plugin.read_bytes(), FAKE_ELF)
        self.assertEqual(plugin.stat().st_ino, inode)
        self.assertEqual(self.gameinfo.read_bytes(), GAMEINFO)

    def test_different_existing_plugin_fails_before_mutation(self):
        plugin = self.csgo / "csdm/bin/linuxsteamrt64/libserver.so"
        plugin.parent.mkdir(parents=True)
        plugin.write_bytes(b"custom plugin")
        with self.assertRaisesRegex(CsdmError, "different CS:DM plugin"):
            with self.context():
                self.fail("must not enter")
        self.assertEqual(plugin.read_bytes(), b"custom plugin")
        self.assertEqual(self.gameinfo.read_bytes(), GAMEINFO)

    def test_external_gameinfo_change_is_preserved_for_manual_recovery(self):
        with self.assertRaises(RestoreConflict):
            with self.context() as installation:
                patched = self.gameinfo.read_bytes()
                self.gameinfo.write_bytes(patched + b"// new user edit\n")
        self.assertTrue(self.gameinfo.read_bytes().endswith(b"// new user edit\n"))
        self.assertEqual(installation.backup_path.read_bytes(), GAMEINFO)
        self.assertTrue(installation.manifest_path.exists())
        self.assertTrue(installation.plugin_path.exists())
        # A deliberate review/merge can put the recognized patch back, then
        # recovery completes without deleting any movie data.
        self.gameinfo.write_bytes(patched)
        recover_plugin(self.game)
        self.assertEqual(self.gameinfo.read_bytes(), GAMEINFO)

    def test_external_plugin_change_blocks_cleanup(self):
        with self.assertRaises(RestoreConflict):
            with self.context() as installation:
                installation.plugin_path.write_bytes(b"updated externally")
        self.assertEqual(installation.plugin_path.read_bytes(), b"updated externally")
        self.assertTrue(installation.backup_path.exists())

    def test_incomplete_transaction_and_standard_csdm_backup_are_rejected(self):
        with self.context():
            with self.assertRaises(CsdmError):
                with self.context():
                    self.fail("overlapping recording must fail")
        existing = self.csgo / "gameinfo.gi.backup"
        existing.write_bytes(b"unrelated original")
        with self.assertRaises(CsdmError):
            with self.context():
                self.fail("existing CSDM backup must be reviewed first")
        self.assertEqual(existing.read_bytes(), b"unrelated original")

    def test_alternate_gameinfo_and_symlinked_plugin_tree_are_rejected(self):
        alternate = self.csgo / "gameinfo.vpk"
        alternate.write_bytes(b"unknown format")
        with self.assertRaises(CsdmError):
            with self.context():
                self.fail("unsupported gameinfo precedence")
        alternate.unlink()
        elsewhere = self.root / "elsewhere"
        elsewhere.mkdir()
        (self.csgo / "csdm").symlink_to(elsewhere, target_is_directory=True)
        with self.assertRaises(CsdmError):
            with self.context():
                self.fail("symlinked managed tree")
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_recovery_can_finish_before_context_exit(self):
        with self.context() as installation:
            recover_plugin(self.game)
            self.assertEqual(self.gameinfo.read_bytes(), GAMEINFO)
            self.assertFalse(installation.plugin_path.exists())

    def test_disappearing_backup_is_a_conflict_while_gameinfo_is_patched(self):
        with self.assertRaises(RestoreConflict):
            with self.context() as installation:
                installation.backup_path.unlink()
        self.assertTrue(installation.manifest_path.exists())
        self.assertTrue(installation.plugin_path.exists())

    def test_crash_before_backup_creation_can_recover_unchanged_gameinfo(self):
        # Prepare the transaction, then model its initial on-disk state: the
        # manifest was durable but no backup, binary or gameinfo write happened.
        with self.context() as installation:
            self.gameinfo.write_bytes(GAMEINFO)
            installation.backup_path.unlink()
            installation.plugin_path.unlink()
            recover_plugin(self.game)
        self.assertEqual(self.gameinfo.read_bytes(), GAMEINFO)
        self.assertFalse(installation.manifest_path.exists())

    def test_pin_mismatch_fails_before_manifest_creation(self):
        with self.assertRaisesRegex(CsdmError, "SHA-256 mismatch"):
            with temporary_plugin(self.game, self.source, launch_args=self.args):
                self.fail("wrong binary")
        self.assertEqual(self.gameinfo.read_bytes(), GAMEINFO)
        self.assertFalse((self.csgo / "gameinfo.gi.cs2-mvp.json").exists())

    def test_launch_guard_rejects_online_and_arbitrary_console_commands(self):
        self.assertEqual(assert_demo_only_launch(self.args), self.demo.resolve())
        for args in [self.args + ["+connect", "127.0.0.1"], self.args + ["+exec", "autoexec"],
                     ["+playdemo", str(self.demo)], self.args + ["-secure"],
                     ["-insecure", "+playdemo", "https://example.com/demo.dem"]]:
            with self.subTest(args=args), self.assertRaises(CsdmError):
                assert_demo_only_launch(args)

    def test_launch_ui_setting_is_fixed_and_precedes_demo_playback(self):
        valid = ["-insecure", "+demo_ui_mode", "0", "+playdemo", str(self.demo)]
        self.assertEqual(assert_demo_only_launch(valid), self.demo.resolve())
        for args in [self.args + ["+demo_ui_mode", "0"],
                     ["-insecure", "+demo_ui_mode", "1", "+playdemo", str(self.demo)],
                     ["-insecure", "+demo_ui_mode", "0;connect 127.0.0.1", "+playdemo", str(self.demo)],
                     valid + ["+demo_ui_mode", "0"]]:
            with self.subTest(args=args), self.assertRaises(CsdmError):
                assert_demo_only_launch(args)


if __name__ == "__main__":
    unittest.main()
