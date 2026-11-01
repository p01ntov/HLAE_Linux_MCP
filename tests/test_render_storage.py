"""Filesystem allocation coverage for MP4 capture, intermediates and publication."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.render import _validate_capture_storage, estimate_capture_storage, render_clip


class CaptureStorageTests(unittest.TestCase):
    def test_split_disks_fit_raw_and_lens_without_summing_unrelated_free_space(self):
        game, work, destination = Path("/game"), Path("/work"), Path("/output/clip.mp4")
        spaces = {game: (1, 25 * 1024**3), work: (2, 15 * 1024**3), destination.parent: (3, 1024**3)}
        with patch("cs2_mvp.render._filesystem_space", side_effect=spaces.__getitem__):
            budgets = _validate_capture_storage(game, work, destination, 1200, 2560, 1440, 10, False, lens=True)
        self.assertEqual(len(budgets), 3)
        for budget in budgets:
            self.assertLess(budget["required_bytes"], budget["free_bytes"])
        expected = estimate_capture_storage(1200, 2560, 1440, lens=True)
        self.assertEqual(budgets[0]["required_bytes"], expected["raw"])
        self.assertEqual(budgets[1]["required_bytes"], expected["lens_intermediate"] + 512 * 1024**2)

    def test_shared_disk_accumulates_raw_and_lens_once_with_one_reserve(self):
        game, work, destination = Path("/game"), Path("/work"), Path("/work/output/clip.mp4")
        with patch("cs2_mvp.render._filesystem_space", return_value=(1, 40 * 1024**3)):
            budgets = _validate_capture_storage(game, work, destination, 1200, 2560, 1440, 10, False, lens=True)
        self.assertEqual(len(budgets), 1)
        self.assertEqual(budgets[0]["required_bytes"], estimate_capture_storage(1200, 2560, 1440, lens=True)["working"])
        with patch("cs2_mvp.render._filesystem_space", return_value=(1, 25 * 1024**3)), \
                self.assertRaisesRegex(RuntimeError, "/game"):
            _validate_capture_storage(game, work, destination, 1200, 2560, 1440, 10, False, lens=True)

    def test_each_split_disk_reports_its_own_shortage(self):
        game, work, destination = Path("/game"), Path("/work"), Path("/output/clip.mp4")
        for insufficient in (game, work, destination.parent):
            spaces = {path: (index, 1024 if path == insufficient else 100 * 1024**3)
                      for index, path in enumerate((game, work, destination.parent))}
            with self.subTest(insufficient=insufficient), \
                    patch("cs2_mvp.render._filesystem_space", side_effect=spaces.__getitem__), \
                    self.assertRaisesRegex(RuntimeError, str(insufficient)):
                _validate_capture_storage(game, work, destination, 1200, 2560, 1440, 10, False, lens=True)

    def test_mp4_dry_run_records_actual_filesystem_budgets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game, demo, camera = root / "game", root / "demo.dem", root / "camera.json"
            game.mkdir()
            demo.touch()
            camera.write_text(json.dumps({"version": 1, "type": "static", "position": [0, 0, 64], "angles": [0, 0],
                                          "lens": {"enabled": True, "vignette": .5}}))
            environment = {"game_directory": str(game), "steam_running": True, "display": ":test", "cs2_pids": [],
                           "ffmpeg": "ffmpeg", "ffprobe": "ffprobe", "game_free_bytes": 25 * 1024**3}
            destination = root / "output/clip.mp4"
            spaces = {game: (1, 25 * 1024**3), root: (2, 15 * 1024**3), destination.parent: (3, 1024**3)}
            with patch("cs2_mvp.render.ROOT", root), patch("cs2_mvp.render.doctor", return_value=environment), \
                    patch("cs2_mvp.render._filesystem_space", side_effect=spaces.__getitem__), \
                    patch("cs2_mvp.render.parser") as parser:
                parser.return_value.parse_header.return_value = {}
                plan = render_clip(demo, None, 1000, 1640, camera=camera, fps=60, capture_fps=120,
                                   width=2560, height=1440, output=destination, dry_run=True)
            self.assertEqual(len(plan["storage_requirements"]), 3)
            self.assertFalse((root / ".runs").exists())


if __name__ == "__main__":
    unittest.main()
