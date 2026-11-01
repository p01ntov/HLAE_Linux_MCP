import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.editor_preferences import DEFAULTS, configuration_path, load_preferences, save_preferences, service_preferences, validate_preferences


class PreferencesTests(unittest.TestCase):
    def test_flat_partial_types_ranges_and_defaults(self):
        changed = validate_preferences({"time_unit": "frames", "frame_rate": 1000, "panel_opacity": .3})
        self.assertEqual(changed["time_unit"], "frames")
        self.assertEqual(changed["inspector_width"], 430)
        self.assertEqual(validate_preferences({"ui_scale": 1.2}, {"ui_scale": 1})["ui_scale"], 1.2)
        for name, value in (("frame_rate", True), ("frame_rate", 59.5), ("ui_scale", float("nan")),
                            ("ui_scale", .74), ("timeline_height", 421), ("panel_opacity", 1.1),
                            ("compact_transport", 1), ("snap_mode", "ticks"), ("resize_mode", "stretch"),
                            ("workspace_source", "shot"), ("workspace_tool", "scene"),
                            ("inspector_section", "map"), ("workspace_open", 1),
                            ("demo_navigation_pinned", None),
                            ("show_tooltips", None), ("version", 1)):
            with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                validate_preferences({name: value})

    def test_configuration_path_defaults_and_explicit_override(self):
        with patch.dict(os.environ, {"XDG_CONFIG_HOME": "/absolute/config"}, clear=True):
            self.assertEqual(configuration_path(), Path("/absolute/config/cs2-camera/editor.json"))
        with patch.dict(os.environ, {"CS2_CAMERA_EDITOR_PREFS": "/absolute/custom/editor.json"}, clear=True):
            self.assertEqual(configuration_path(), Path("/absolute/custom/editor.json"))
        with patch.dict(os.environ, {"CS2_CAMERA_EDITOR_PREFS": "relative.json"}, clear=True):
            with self.assertRaisesRegex(ValueError, "absolute"):
                configuration_path()

    def test_atomic_roundtrip_and_private_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "config/editor.json"
            self.assertEqual(load_preferences(path), DEFAULTS)
            save_preferences({"time_unit": "ticks", "timeline_fov": False}, path)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            document = json.loads(path.read_text())
            self.assertEqual(document["version"], 1)
            self.assertEqual(document["preferences"]["time_unit"], "ticks")
            self.assertFalse(load_preferences(path)["timeline_fov"])
            self.assertEqual(list(path.parent.glob(".editor-*.tmp")), [])

    def test_previous_version_one_preferences_gain_navigation_defaults(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "editor.json"
            path.write_text(json.dumps({"version": 1, "preferences": {
                "time_unit": "ticks", "frame_rate": 1000, "compact_transport": False}}))
            loaded = load_preferences(path)
            self.assertEqual(loaded["workspace_source"], "camera")
            self.assertEqual(loaded["workspace_tool"], "timeline")
            self.assertEqual(loaded["inspector_section"], "camera")
            self.assertTrue(loaded["workspace_open"])
            self.assertFalse(loaded["demo_navigation_pinned"])
            saved = save_preferences({**loaded, "workspace_source": "demo", "workspace_tool": "curves",
                                      "inspector_section": "scene", "workspace_open": False,
                                      "demo_navigation_pinned": True}, path)
            self.assertEqual(load_preferences(path), saved)
            self.assertEqual(saved["time_unit"], "ticks")
            self.assertEqual(saved["frame_rate"], 1000)

    def test_failed_write_or_replace_preserves_existing_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "editor.json"
            save_preferences(DEFAULTS, path); before = path.read_bytes()
            with patch("cs2_mvp.editor_preferences.os.replace", side_effect=OSError("fixture disk error")):
                with self.assertRaisesRegex(OSError, "fixture disk error"):
                    save_preferences({"frame_rate": 24}, path)
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(list(path.parent.glob(".editor-*.tmp")), [])
            with patch("cs2_mvp.editor_preferences.os.fsync", side_effect=OSError("fixture flush error")):
                with self.assertRaisesRegex(OSError, "fixture flush error"):
                    save_preferences({"frame_rate": 24}, path)
            self.assertEqual(path.read_bytes(), before)

    def test_invalid_version_unknown_field_oversized_and_symlink_reject(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "editor.json"
            for document in ({"version": 2, "preferences": {}}, {"version": 1.0, "preferences": {}},
                             {"version": 1, "preferences": {"unknown": True}}):
                path.write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    load_preferences(path)
            path.write_bytes(b"x" * 16385)
            with self.assertRaisesRegex(ValueError, "16 KiB"):
                load_preferences(path)
            target = Path(temporary) / "target.json"; target.write_text("preserve")
            path.unlink(); path.symlink_to(target)
            for operation in (lambda: load_preferences(path), lambda: save_preferences(DEFAULTS, path)):
                with self.assertRaises(ValueError):
                    operation()
            self.assertEqual(target.read_text(), "preserve")

    def test_service_only_persists_new_preferences_and_never_writes_game_state(self):
        class Client:
            def __init__(self):
                self.calls = []
                self.state = {"editor_preferences_revision": 0, "editor_preferences": dict(DEFAULTS), "revision": 17}
            def request(self, method):
                self.calls.append(method)
                return copy.deepcopy(self.state)
        client = Client()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "editor.json"
            self.assertEqual(service_preferences(client, 0, path), 0)
            self.assertFalse(path.exists())
            client.state["editor_preferences_revision"] = 1
            client.state["editor_preferences"]["time_unit"] = "ticks"
            self.assertEqual(service_preferences(client, 0, path), 1)
            self.assertEqual(load_preferences(path)["time_unit"], "ticks")
            with patch("cs2_mvp.editor_preferences.save_preferences", side_effect=AssertionError("No repeat write")):
                self.assertEqual(service_preferences(client, 1, path), 1)
            self.assertEqual(client.calls, ["get_state"] * 3)
            self.assertEqual(client.state["revision"], 17)

    def test_initial_native_load_error_preserves_malformed_file_until_explicit_edit(self):
        class Client:
            state = {"editor_preferences_revision": 1, "editor_preferences": dict(DEFAULTS),
                     "editor_preferences_error": "unsupported editor preference document"}
            def request(self, method):
                return copy.deepcopy(self.state)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "editor.json"; path.write_text('{"version":99,"future":"keep me"}')
            original = path.read_bytes(); client = Client()
            self.assertEqual(service_preferences(client, 0, path), 0)
            self.assertEqual(path.read_bytes(), original)
            client.state = {"editor_preferences_revision": 2, "editor_preferences": {**DEFAULTS, "frame_rate": 24}, "editor_preferences_error": ""}
            self.assertEqual(service_preferences(client, 0, path), 2)
            self.assertEqual(load_preferences(path)["frame_rate"], 24)


if __name__ == "__main__":
    unittest.main()
