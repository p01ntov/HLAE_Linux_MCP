"""Exercise the same dispatcher bundled as the installed AppImage executable."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

from cs2_mvp import map_geometry


def dispatcher():
    path = Path(__file__).resolve().parents[1] / "packaging/live/entry.py"
    spec = importlib.util.spec_from_file_location("camera_appimage_entry", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MapEntryTests(unittest.TestCase):
    def test_index_maps_passes_exact_arguments_and_result_without_opening_launcher(self):
        arguments = ["--cache-root", "/mnt/ssd/maps", "--game-dir", "/games/CS2/game", "--bootstrap-tool", "--map", "de_nuke"]
        with patch.object(sys, "argv", ["AppImage", "index-maps", *arguments]), \
             patch.object(map_geometry, "main", return_value=17) as index, \
             patch("cs2_mvp.live.main") as live:
            self.assertEqual(dispatcher().main(), 17)
            index.assert_called_once_with(arguments)
            live.assert_not_called()

    def test_actual_indexer_help_is_available_from_appimage_dispatch_without_io(self):
        output = io.StringIO()
        with patch.object(sys, "argv", ["AppImage", "index-maps", "--help"]), \
             patch.object(map_geometry, "bootstrap_tool") as download, \
             patch.object(map_geometry.subprocess, "run") as process, \
             contextlib.redirect_stdout(output), self.assertRaises(SystemExit) as stopped:
            dispatcher().main()
        self.assertEqual(stopped.exception.code, 0)
        self.assertTrue(all(flag in output.getvalue() for flag in ("--cache-root", "--bootstrap-tool", "--tool", "--map")))
        download.assert_not_called()
        process.assert_not_called()

    def test_relative_cache_root_is_rejected_before_directory_creation_or_download(self):
        with patch.object(sys, "argv", ["AppImage", "index-maps", "--cache-root", "relative", "--bootstrap-tool"]), \
             patch.object(Path, "mkdir") as mkdir, \
             patch.object(map_geometry, "bootstrap_tool") as download, \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped:
            dispatcher().main()
        self.assertEqual(stopped.exception.code, 2)
        mkdir.assert_not_called()
        download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
