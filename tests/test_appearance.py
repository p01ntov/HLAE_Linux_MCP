import json
import os
from pathlib import Path
import stat
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

from cs2_mvp import appearance as assets
from cs2_mvp.live_mcp import ProtocolError, TOOL_MAP, call_tool


def fixture(root, *, corrupt=False):
    """A real-format v2 index with preload-only, inline and external resources."""
    entries = {
        "agents/models/ctm_test/first.vmdl_c": (b"PRE", 0, 0, b"BODY"),
        "characters/models/ctm_test/second.vmdl_c": (b"", 0x7FFF, 0, b"INLINE"),
        "agents/models/shared/glove_test/glove.vmdl_c": (b"GLOVE", 0, 0, b""),
        "weapons/models/test/weapon.vmdl_c": (b"WEAPON", 0, 0, b""),
    }
    groups = {}
    for path, value in entries.items():
        item = Path(path)
        groups.setdefault(str(item.parent), []).append((item.stem, value))
    tree = bytearray(b"vmdl_c\0")
    for directory, values in groups.items():
        tree.extend(directory.encode() + b"\0")
        for name, (preload, archive, offset, body) in values:
            payload = preload + body
            crc = zlib.crc32(payload) & 0xFFFFFFFF
            if corrupt and name == "first":
                crc ^= 1
            tree.extend(name.encode() + b"\0" + struct.pack("<IHHIIH", crc, len(preload), archive, offset, len(body), 0xFFFF) + preload)
        tree.extend(b"\0")
    tree.extend(b"\0\0")
    path = root / "pak01_dir.vpk"
    path.write_bytes(struct.pack("<7I", assets.SIGNATURE, 2, len(tree), 6, 0, 0, 0) + tree + b"INLINE")
    (root / "pak01_000.vpk").write_bytes(b"BODY")
    return path, entries


class AppearanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path, self.entries = fixture(self.root)

    def test_vpk_reads_preload_inline_and_external_data_and_verifies_crc(self):
        index = assets.VpkIndex(self.path)
        self.assertEqual(len(index.entries), 4)
        for resource, (preload, _, _, body) in self.entries.items():
            self.assertEqual(index.read_model(resource), preload + body)
        (self.root / "pak01_000.vpk").write_bytes(b"FAIL")
        with self.assertRaisesRegex(ValueError, "CRC32"):
            index.read_model(next(iter(self.entries)))

    def test_malformed_tree_and_archive_bounds_are_rejected(self):
        original = self.path.read_bytes()
        for malformed in (original[:10], original[:-1], original.replace(b"\xff\xff", b"\xff\x00", 1),
                          struct.pack("<7I", assets.SIGNATURE, 2, assets.MAX_TREE_BYTES + 1, 0, 0, 0, 0)):
            self.path.write_bytes(malformed)
            with self.subTest(length=len(malformed)), self.assertRaises(ValueError):
                assets.VpkIndex(self.path)
        self.path.write_bytes(original)
        index = assets.VpkIndex(self.path)
        (self.root / "pak01_000.vpk").write_bytes(b"B")
        with self.assertRaisesRegex(ValueError, "beyond"):
            index.read_model(next(iter(self.entries)))

    def test_tree_paths_and_external_archive_symlinks_cannot_escape(self):
        for resource in ("../secret.vmdl_c", "/models/first.vmdl_c", "models//first.vmdl_c", "models/./first.vmdl_c",
                         "C:/models/first.vmdl_c", "models\\first.vmdl_c", "models/first.fbx"):
            with self.subTest(resource=resource), self.assertRaises(ValueError):
                assets.model_resource(resource)
        archive = self.root / "pak01_000.vpk"
        archive.unlink()
        archive.symlink_to(self.path)
        with self.assertRaisesRegex(ValueError, "symlink"):
            assets.VpkIndex(self.path).read_model(next(iter(self.entries)))

    def test_catalog_filters_real_paths_and_has_bounded_pagination(self):
        with patch.object(assets, "installed_index", return_value=assets.VpkIndex(self.path)):
            result = assets.catalog(kind="character", search="TEST", limit=1)
            self.assertEqual(result["total"], 2)
            self.assertEqual(result["next_offset"], 1)
            self.assertTrue(result["models"][0]["engine_model"].endswith(".vmdl"))
            self.assertEqual(assets.catalog(kind="glove")["total"], 1)
            for query in ({"limit": 101}, {"limit": True}, {"offset": -1}, {"search": "x" * 257}, {"kind": "skin"}):
                with self.subTest(query=query), self.assertRaises(ValueError):
                    assets.catalog(**query)

    def test_prepare_publishes_only_new_private_pack_without_modifying_source(self):
        before = self.path.read_bytes()
        original, replacement = list(self.entries)[:2]
        environment = {"XDG_DATA_HOME": str(self.root / "private")}
        with patch.dict(os.environ, environment), patch.object(assets, "installed_index", return_value=assets.VpkIndex(self.path)):
            result = assets.prepare_override_pack({original: replacement})
            second = assets.prepare_override_pack({original: replacement})
        pack = Path(result["path"])
        self.assertNotEqual(pack, Path(second["path"]))
        self.assertEqual((pack / original).read_bytes(), b"INLINE")
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(stat.S_IMODE(pack.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((pack / original).stat().st_mode), 0o600)
        self.assertEqual(result["models"], [original.removesuffix("_c")])
        manifest = json.loads((pack / "appearance-pack.json").read_text())
        self.assertEqual(manifest["scope"], "global_resource_override")
        self.assertEqual(manifest["mappings"][0]["replacement"], replacement)

    def test_crc_failure_and_limits_leave_no_partially_published_pack(self):
        fixture(self.root, corrupt=True)
        source = next(iter(self.entries))
        with patch.dict(os.environ, {"XDG_DATA_HOME": str(self.root / "private")}), \
             patch.object(assets, "installed_index", return_value=assets.VpkIndex(self.path)):
            with self.assertRaisesRegex(ValueError, "CRC32"):
                assets.prepare_override_pack({source: source})
            self.assertEqual(list((self.root / "private/cs2-camera/appearance-packs").iterdir()), [])
            with patch.object(assets, "MAX_PACK_BYTES", 1), self.assertRaisesRegex(ValueError, "total limit"):
                assets.prepare_override_pack({source: source})

    def test_pack_destination_symlinks_and_noncompiled_inputs_are_rejected(self):
        data = self.root / "private"
        data.mkdir()
        (data / "cs2-camera").symlink_to(self.root, target_is_directory=True)
        source = next(iter(self.entries))
        with patch.dict(os.environ, {"XDG_DATA_HOME": str(data)}), \
             patch.object(assets, "installed_index", return_value=assets.VpkIndex(self.path)):
            with self.assertRaisesRegex(ValueError, "non-symlink"):
                assets.prepare_override_pack({source: source})
            with self.assertRaisesRegex(ValueError, "vmdl_c"):
                assets.prepare_override_pack({source: "/tmp/custom.fbx"})

    def test_asset_mcp_catalog_and_pack_prep_are_real_offline_operations(self):
        class NoEngine:
            def request(self, *args):
                raise AssertionError("Asset preparation must not send engine commands")
        original, replacement = list(self.entries)[:2]
        with patch.dict(os.environ, {"XDG_DATA_HOME": str(self.root / "private")}), \
             patch.object(assets, "installed_index", return_value=assets.VpkIndex(self.path)):
            listed = call_tool(NoEngine(), "cs2_assets_appearance_catalog", {"kind":"character","limit":1})
            self.assertEqual(listed["total"],2)
            args={"mappings":[{"original":original,"replacement":replacement}]}
            pack=call_tool(NoEngine(),"cs2_assets_prepare_appearance_pack",args)
            self.assertEqual((Path(pack["path"])/original).read_bytes(),b"INLINE")
            self.assertTrue(Path(pack["manifest"]).is_file())
            self.assertEqual(pack["models"],[original.removesuffix("_c")])
            with self.assertRaisesRegex(ValueError,"only once"):
                call_tool(NoEngine(),"cs2_assets_prepare_appearance_pack",{"mappings":args["mappings"]*2})
            with self.assertRaises(ProtocolError):
                call_tool(NoEngine(),"cs2_assets_prepare_appearance_pack",{"mappings":args["mappings"]*33})
        self.assertFalse(TOOL_MAP["cs2_camera_probe_appearance_pack"]["annotations"]["readOnlyHint"])


if __name__ == "__main__":
    unittest.main()
