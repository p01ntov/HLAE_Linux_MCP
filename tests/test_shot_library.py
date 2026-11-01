import copy
import json
import os
from pathlib import Path
import tempfile
import unittest

from cs2_mvp.shot_library import MAX_BYTES, read_library, stage_library, validate_library


class ShotLibraryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.demo = self.root / "match.dem"
        self.demo.write_bytes(b"PBDEMS2\0")
        self.value = {"version": 1, "demo": str(self.demo), "shots": [
            {"id": "round-2", "label": "R2 - two headshots", "project": {
                "name": "R2", "start_tick": 1000, "duration": 3,
                "scene": {"hud_visible": False, "viewmodel_visible": False},
                "keys": [{"id": "a", "time": 0, "position": [1, 2, 3], "angles": [0, 90, 0], "fov": 160},
                         {"id": "b", "time": 3, "position": [4, 5, 6], "angles": [0, 95, 0]}]}}]}

    def test_stage_preserves_projects_and_uses_actual_launched_demo(self):
        path = self.root / "shots.json"
        path.write_text(json.dumps(self.value))
        loaded = read_library(path, self.demo)
        run = self.root / "run"
        run.mkdir()
        link = run / "input.dem"
        link.symlink_to(self.demo)
        output = stage_library(loaded, link, run)
        staged = json.loads(output.read_text())
        self.assertEqual(staged["demo"], str(link))
        self.assertEqual(staged["shots"], self.value["shots"])
        self.assertEqual(loaded, self.value)
        self.assertEqual(output.stat().st_mode & 0o777, 0o600)
        validate_library(staged, self.demo)

    def test_wrong_demo_unknown_fields_and_duplicate_ids_fail(self):
        wrong = self.root / "other.dem"
        wrong.write_bytes(b"PBDEMS2\0")
        changes = [lambda v: v.update(demo=str(wrong)), lambda v: v.update(extra=True),
                   lambda v: v.update(version=True), lambda v: v["shots"].append(copy.deepcopy(v["shots"][0])),
                   lambda v: v["shots"][0].update(label="bad\0label"),
                   lambda v: v.update(shots=v["shots"] * 65)]
        for change in changes:
            value = copy.deepcopy(self.value)
            change(value)
            with self.subTest(value=repr(value)[:120]), self.assertRaises(Exception):
                validate_library(value, self.demo)

    def test_native_project_cross_field_bounds_and_removed_cosmetics_fail(self):
        changes = [lambda p: p.update(duration=1), lambda p: p["keys"][1].update(time=0),
                   lambda p: p["keys"][1].update(id="a"), lambda p: p["keys"][0].update(angles=[91, 0, 0]),
                   lambda p: p.update(render_range={"mode": "region", "in": 2, "out": 1}),
                   lambda p: p.update(cosmetics={"gloves": []}), lambda p: p.update(keys=[])]
        for change in changes:
            value = copy.deepcopy(self.value)
            change(value["shots"][0]["project"])
            with self.subTest(change=change), self.assertRaises(Exception):
                validate_library(value, self.demo)

    def test_file_bounds_symlinks_and_nonregular_inputs_fail(self):
        path = self.root / "library.json"
        path.write_bytes(b" " * (MAX_BYTES + 1))
        with self.assertRaisesRegex(ValueError, "1 MiB"):
            read_library(path, self.demo)
        link = self.root / "link.json"
        link.symlink_to(path)
        with self.assertRaises(OSError):
            read_library(link, self.demo)
        fifo = self.root / "pipe.json"
        os.mkfifo(fifo)
        with self.assertRaisesRegex(ValueError, "regular"):
            read_library(fifo, self.demo)


if __name__ == "__main__":
    unittest.main()
