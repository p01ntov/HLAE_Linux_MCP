import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.project_storage import configuration_path, projects_directory, geometry_directory


class ProjectStorageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.environment = patch.dict(os.environ, {
            "XDG_CONFIG_HOME": str(self.root / "config"),
            "XDG_DATA_HOME": str(self.root / "data"),
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.override = os.environ.pop("CS2_CAMERA_PROJECTS_DIR", None)
        self.addCleanup(lambda: os.environ.update({"CS2_CAMERA_PROJECTS_DIR": self.override}) if self.override is not None else None)
        self.geometry_override = os.environ.pop("CS2_CAMERA_GEOMETRY_CACHE", None)
        self.addCleanup(lambda: os.environ.update({"CS2_CAMERA_GEOMETRY_CACHE": self.geometry_override}) if self.geometry_override is not None else None)

    def write_config(self, value):
        path = configuration_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    def test_default_folder_and_existing_camera_json_are_preserved(self):
        path = projects_directory()
        expected = self.root / "data/cs2-camera/projects"
        self.assertEqual(path, expected)
        project = path / "old-project.json"
        project.write_text('{"keys":[]}')
        self.assertEqual(projects_directory(), path)
        self.assertEqual(project.read_text(), '{"keys":[]}')

    def test_explicit_env_and_persisted_selection_precedence(self):
        saved, environment, explicit = (self.root / name for name in ("saved", "environment", "explicit"))
        self.write_config({"version": 1, "projects_directory": str(saved)})
        self.assertEqual(projects_directory(), saved)
        with patch.dict(os.environ, {"CS2_CAMERA_PROJECTS_DIR": str(environment)}):
            self.assertEqual(projects_directory(), environment)
            self.assertEqual(projects_directory(explicit), explicit)
        self.assertFalse((self.root / "data").exists())

    def test_invalid_configuration_never_silently_writes_elsewhere(self):
        for value in ({"version": 2}, {"version": True}, {"projects_directory": 3}, {"projects_directory": "relative"}, []):
            self.write_config(value)
            with self.subTest(value=value), self.assertRaises(ValueError):
                projects_directory()
        self.assertFalse((self.root / "data").exists())
        path = configuration_path()
        path.write_text(" " * 16385)
        with self.assertRaisesRegex(ValueError, "16 KiB"):
            projects_directory()

    def test_file_instead_of_folder_is_rejected_without_overwrite(self):
        path = self.root / "not-a-directory"
        path.write_text("keep")
        with self.assertRaises(FileExistsError):
            projects_directory(path)
        self.assertEqual(path.read_text(), "keep")

    def test_geometry_cache_is_opt_in_and_never_created_implicitly(self):
        self.assertIsNone(geometry_directory())
        saved = self.root / "geometry-on-ssd"
        self.write_config({"version": 1, "geometry_directory": str(saved)})
        self.assertEqual(geometry_directory(), saved)
        override = self.root / "other-cache"
        with patch.dict(os.environ, {"CS2_CAMERA_GEOMETRY_CACHE": str(override)}):
            self.assertEqual(geometry_directory(), override)
        self.assertFalse(saved.exists())
        self.assertFalse(override.exists())

    def test_invalid_geometry_selection_never_falls_back(self):
        for selected in ("relative", 3, "", "bad\0path"):
            self.write_config({"version": 1, "geometry_directory": selected})
            with self.subTest(selected=selected), self.assertRaises(ValueError):
                geometry_directory()
        self.write_config({"version": True})
        with self.assertRaises(ValueError):
            geometry_directory()
