import os
from pathlib import Path
import tempfile
import struct
import unittest
from unittest.mock import patch

from cs2_mvp import discovery


class DiscoveryTests(unittest.TestCase):
    def demo_bytes(self):
        # Structurally complete framing with a final DEM_FileInfo command.
        return b"PBDEMS2\0"+struct.pack("<II",19,0)+bytes([1,0,0,2,0,0])

    def game(self, library):
        game=library/"steamapps/common/Counter-Strike Global Offensive/game"
        (game/"csgo/replays").mkdir(parents=True)
        (game/"cs2.sh").write_text("#!/bin/sh\n")
        (game/"csgo/gameinfo.gi").write_text('"GameInfo" {}')
        return game

    def test_registered_and_mounted_libraries_include_external_cs2_without_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);home=root/"home";steam=home/".local/share/Steam"
            (steam/"steamapps").mkdir(parents=True)
            external=root/"disk/SteamLibrary";game=self.game(external)
            alias=root/"alias";alias.symlink_to(external,target_is_directory=True)
            (steam/"steamapps/libraryfolders.vdf").write_text(
                f'"libraryfolders" {{ "0" {{ "path" "{steam}" }} "1" {{ "path" "{external}" }} "2" "{alias}" "3" {{ "path" "{root}/missing" }} }}')
            with patch.object(Path,"home",return_value=home),patch.object(discovery,"_mounted_library_roots",return_value=[external,alias]):
                libraries=discovery.steam_roots()
                self.assertEqual(libraries,[steam,external])
                self.assertEqual(discovery.find_games(),[game])
                self.assertEqual(discovery.find_game(),game)
                self.assertEqual(discovery.find_game(str(game.parent)),game)

    def test_unreadable_registration_does_not_hide_other_mounted_libraries(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);home=root/"home";steam=home/".local/share/Steam"
            (steam/"steamapps").mkdir(parents=True)
            blocked=steam/"steamapps/libraryfolders.vdf";blocked.write_text("unreadable")
            external=root/"external";game=self.game(external)
            original=Path.read_text
            def read(path,*args,**kwargs):
                if path==blocked:
                    raise PermissionError("library unavailable")
                return original(path,*args,**kwargs)
            with patch.object(Path,"home",return_value=home),patch.object(discovery,"_mounted_library_roots",return_value=[external]),patch.object(Path,"read_text",read):
                self.assertEqual(discovery.find_games(),[game])

    def test_downloaded_demos_are_sorted_deduplicated_and_invalid_headers_are_marked(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);game=self.game(root/"library");replays=game/"csgo/replays"
            old=replays/"old.dem";old.write_bytes(self.demo_bytes())
            new=replays/"latest.dem";new.write_bytes(self.demo_bytes())
            (game/"csgo/alias.dem").symlink_to(new)
            invalid=replays/"incomplete.dem";invalid.write_bytes(b"PBDE")
            (replays/"compressed.dem.bz2").write_bytes(b"BZh")
            os.utime(old,(100,100));os.utime(new,(300,300));os.utime(invalid,(200,200))
            demos=discovery.discover_demos([game,game])
            self.assertEqual([item["filename"] for item in demos],["latest.dem","incomplete.dem","old.dem"])
            self.assertEqual([item["ready"] for item in demos],[True,False,True])
            self.assertTrue(demos[0]["downloaded"])
            self.assertEqual(demos[0]["game_directory"],str(game))
            self.assertIn("header",demos[1]["reason"])
            self.assertEqual(discovery.discover_demos([game],limit=1)[0]["filename"],"latest.dem")
            for limit in (True,0,2001):
                with self.subTest(limit=limit),self.assertRaises(ValueError):
                    discovery.discover_demos([game],limit=limit)

    def test_magic_only_and_incomplete_final_metadata_are_not_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            game=self.game(Path(directory)/"library");replays=game/"csgo/replays"
            samples=(b"PBDEMS2\0",b"PBDEMS2\0"+struct.pack("<II",10000,0)+bytes(10),
                     b"PBDEMS2\0"+struct.pack("<II",19,0)+bytes([1,0,0,2,0,99]))
            for index,data in enumerate(samples):
                (replays/f"partial{index}.dem").write_bytes(data)
            demos=discovery.discover_demos([game])
            self.assertEqual(len(demos),3)
            self.assertFalse(any(item["ready"] for item in demos))

    def test_missing_demo_directory_and_unreadable_demo_are_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);game=self.game(root/"library");blocked=game/"csgo/replays/private.dem"
            blocked.write_bytes(b"PBDEMS2\0payload")
            original=Path.open
            def opened(path,*args,**kwargs):
                if path==blocked:
                    raise PermissionError("not readable")
                return original(path,*args,**kwargs)
            with patch.object(Path,"open",opened):
                self.assertEqual(discovery.discover_demos([game,root/"missing"]),[])


if __name__=="__main__":
    unittest.main()
