import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from cs2_mvp import map_geometry as mg


def tetrahedron(subdivide_edge=False):
    points = [[0, 0, 0], [8, 0, 0], [0, 8, 0], [0, 0, 8]]
    rings = [(0, 2, 1), (0, 1, 3), (1, 2, 3), (2, 0, 3)]
    if subdivide_edge:
        middle = list(range(4, 126))
        points += [[i * 8 / 123, 0, 0] for i in range(1, 123)]
        rings = [(0, 2, 1, *reversed(middle)), (0, *middle, 1, 3), (1, 2, 3), (2, 0, 3)]
    points = np.asarray(points, dtype="<f4")
    edges, lookup, starts = [], {}, []
    for face, ring in enumerate(rings):
        start = len(edges)
        starts.append(start)
        for i, origin in enumerate(ring):
            lookup[(origin, ring[(i + 1) % len(ring)])] = len(edges)
            edges.append([(start + (i + 1) % len(ring)), 0, origin, face])
    for (origin, end), index in lookup.items():
        edges[index][1] = lookup[end, origin]
    return {"m_VertexPositions": points.tobytes(), "m_Vertices": bytes([0, 1, 2, 3]),
            "m_Edges": np.asarray(edges, dtype="u1").tobytes(), "m_Faces": bytes(starts)}


def aggregate():
    return {"m_bindPose": [], "m_collisionAttributes": [{"m_InteractAsStrings": ["solid"]}],
            "m_parts": [{"m_rnShape": {"m_spheres": [], "m_capsules": [], "m_compounds": [],
            "m_hulls": [{"m_Hull": tetrahedron(), "m_nCollisionAttributeIndex": 0}],
            "m_meshes": [{"m_Mesh": {"m_Vertices": [[20, 0, 0], [20, 8, 0], [20, 0, 8]],
                                      "m_Triangles": [{"m_nIndex": [0, 1, 2]}]}}]}}]}


class PhysicsReaderTests(unittest.TestCase):
    def test_unused_svm_compaction_preserves_sibling_geometry_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, compact = Path(temporary) / "raw.kv3", Path(temporary) / "compact.kv3"
            source.write_text('{\n\tm_parts=[]\n\tm_pRegionSVM = \n\t{\n\t\tm_Planes=#[ 11 22 ]\n\t\tm_nested =\n\t\t{\n\t\t\tx=4\n\t\t}\n\t}\n\tm_Nodes =\n\t#[\n\t\tAA BB\n\t]\n\tm_Planes=#[ 33 44 ]\n\tm_Vertices=[1,2,3]\n}\n')
            mg._compact_physics_dump(source, compact)
            value = mg.parse_physics_dump(compact.read_text())
            self.assertNotIn("m_pRegionSVM", value)
            self.assertNotIn("m_Nodes", value)
            self.assertEqual(value["m_Planes"], b"\x33\x44")
            self.assertEqual(value["m_Vertices"], [1, 2, 3])

    def test_byte_indexed_hull_can_have_256_half_edges(self):
        hull = tetrahedron(subdivide_edge=True)
        self.assertEqual(len(hull["m_Edges"]) // 4, 256)
        result = mg._hull_triangles(hull, len(hull["m_VertexPositions"]) // 12)
        self.assertEqual(result.shape, (248, 3))

    def test_multiline_strings_in_real_entity_keyvalues(self):
        value = mg._KV3('{pathNodes="""\n[1,2,3]\n""" model=resource_name:"models/door.vmdl"}').value()
        self.assertEqual(value["pathNodes"], '\n[1,2,3]\n')
        self.assertEqual(value["model"], "models/door.vmdl")

    def test_official_dump_prefix_binary_and_scalar_forms(self):
        text = 'Resource Type: Model\n<!-- kv3 encoding:text:version{x} -->\n{ m_parts=[] data=#[ 00 FF 10 ] model=resource_name:"maps/test.vmdl" v=[1,-2.5,3e2,true,false,null,"x\\ny"] }\n'
        result = mg.parse_physics_dump(text)
        self.assertEqual(result["data"], b"\0\xff\x10")
        self.assertEqual(result["model"], "maps/test.vmdl")
        self.assertEqual(result["v"], [1, -2.5, 300, True, False, None, "x\ny"])

    def test_rejects_truncation_duplicate_keys_and_unrecognized_scalars(self):
        for text in ('{m_parts=[', '{m_parts=[] x=#[ GG ]}', '{m_parts=[] m_parts=[]}',
                     '{m_parts=[] x=nan}', '{m_parts=[]} trailing'):
            with self.subTest(text=text), self.assertRaises(mg.GeometryExtractionError):
                mg.parse_physics_dump(text)

    def test_convex_hulls_retain_identity_beside_surface_only_mesh(self):
        arrays, components, unsupported = mg.physics_arrays(aggregate())
        self.assertEqual(arrays["vertices"].shape, (7, 3))
        self.assertEqual(arrays["triangles"].shape, (5, 3))
        self.assertEqual(arrays["triangle_component"].tolist(), [0, 0, 0, 0, 1])
        self.assertEqual(arrays["triangles"][-1].tolist(), [4, 5, 6])
        self.assertTrue(components[0]["closed"] and components[0]["convex"])
        self.assertFalse(components[1]["closed"] or components[1]["convex"])
        self.assertEqual(components[0]["bounds"], [[0, 0, 0], [8, 8, 8]])
        self.assertEqual(unsupported, [])

    def test_invalid_topology_cannot_be_marked_as_a_closed_solid(self):
        for mutation in ("twin", "origin", "face", "unused"):
            data = aggregate()
            hull = data["m_parts"][0]["m_rnShape"]["m_hulls"][0]["m_Hull"]
            edges = bytearray(hull["m_Edges"])
            if mutation == "twin": edges[1] = 255
            elif mutation == "origin": edges[2] = 250
            elif mutation == "face": edges[3] = 3
            elif mutation == "unused": hull["m_Faces"] = hull["m_Faces"][:-1]
            hull["m_Edges"] = bytes(edges)
            with self.subTest(mutation=mutation), self.assertRaises(mg.GeometryExtractionError):
                mg.physics_arrays(data)

    def test_mesh_out_of_range_and_nan_vertices_are_rejected(self):
        for mutation in ("index", "nan"):
            data = aggregate()
            mesh = data["m_parts"][0]["m_rnShape"]["m_meshes"][0]["m_Mesh"]
            if mutation == "index": mesh["m_Triangles"][0]["m_nIndex"][2] = 3
            else: mesh["m_Vertices"][0][0] = float("nan")
            with self.subTest(mutation=mutation), self.assertRaises(mg.GeometryExtractionError):
                mg.physics_arrays(data)

    def test_analytic_and_unknown_shapes_are_explicitly_uncovered(self):
        data = aggregate()
        shape = data["m_parts"][0]["m_rnShape"]
        shape.update(m_capsules=[{}], m_compounds=[{}], m_CollisionAttributeIndices=[0, 0])
        _, _, unsupported = mg.physics_arrays(data)
        self.assertTrue(any("sphere/capsule" in value for value in unsupported))
        self.assertTrue(any("m_compounds" in value for value in unsupported))
        self.assertFalse(any("m_CollisionAttributeIndices" in value for value in unsupported))

    def test_bind_pose_transforms_are_applied_to_collision_vertices(self):
        data = aggregate()
        data["m_bindPose"] = [[1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30]]
        arrays, components, _ = mg.physics_arrays(data)
        np.testing.assert_array_equal(arrays["vertices"][0], [10, 20, 30])
        self.assertEqual(components[0]["bounds"], [[10, 20, 30], [18, 28, 38]])


class MapArchiveTests(unittest.TestCase):
    def test_explicit_unreadable_game_build_cannot_fall_back_to_any_build_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch("cs2_mvp.cosmetics_catalog._game_provenance", return_value={"client_build_id": None}), \
                 self.assertRaisesRegex(mg.GeometryExtractionError, "Cannot verify.*client build ID"):
                mg.cache_for_map("de_nuke", root, game_directory=root / "missing-game")
            with patch("cs2_mvp.cosmetics_catalog._game_provenance", return_value={"client_build_id": "abc123"}), \
                 self.assertRaisesRegex(mg.GeometryExtractionError, "found 0"):
                mg.cache_for_map("de_nuke", root, game_directory=root / "selected-game")

    def test_vphys_takes_precedence_over_alternate_vmdl_for_the_same_world(self):
        entries = [{"resource": "maps/cs_italy/world_physics.vmdl_c"},
                   {"resource": "maps/cs_italy/world_physics.vphys_c"}]
        self.assertEqual(mg._select_physics_entry(entries), entries[1])
        self.assertEqual(mg._select_physics_entry(entries[:1]), entries[0])
        with self.assertRaisesRegex(mg.GeometryExtractionError, "world physics group"):
            mg._select_physics_entry(entries + [{"resource": "maps/other/world_physics.vmdl_c"}])

    def test_inline_and_chunked_world_physics_in_vpk_v1_and_v2(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "de_test.vpk"
            for version in (1, 2):
                for chunk in (0x7fff, 3):
                    tree = b"vmdl_c\0maps/de_test\0world_physics\0" + struct.pack("<IHHIIH", 123, 0, chunk, 0, 0, 65535) + b"\0\0\0"
                    header = struct.pack("<III", 0x55aa1234, version, len(tree))
                    if version == 2: header += bytes(16)
                    path.write_bytes(header + tree)
                    self.assertEqual(mg._physics_entries(path), [{"resource": "maps/de_test/world_physics.vmdl_c", "archive": chunk, "crc32": 123}])
            path.write_bytes(struct.pack("<III", 0x55aa1234, 2, 2**30))
            with self.assertRaises(mg.GeometryExtractionError): mg._physics_entries(path)

    def test_discovery_includes_workshop_and_skips_data_chunks_and_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / "steamapps/common/CS2/game"
            maps = game / "csgo/maps"
            workshop = root / "steamapps/workshop/content/730/12345"
            maps.mkdir(parents=True); workshop.mkdir(parents=True)
            (maps / "de_test.vpk").touch()
            (maps / "pak_000.vpk").touch()
            (maps / "linked.vpk").symlink_to(maps / "de_test.vpk")
            (workshop / "custom.vpk").touch()
            result = mg.discover_maps([game])
            self.assertEqual({Path(r["path"]).name for r in result}, {"de_test.vpk", "custom.vpk"})
            self.assertEqual(sum(r["workshop"] for r in result), 1)

    def test_cache_resolution_rejects_changed_sources_and_ambiguous_maps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "test.vpk"
            source.write_bytes(b"old")
            stat = source.stat()
            manifest = {"format": "cs2-camera-geometry", "version": 1, "map_name": "de_test",
                        "identity": {"extractor_revision": mg.EXTRACTOR_REVISION},
                        "source_files": [{"path": str(source), "size": stat.st_size, "mtime_ns": stat.st_mtime_ns}]}
            directory = root / "de_test/hash1"
            directory.mkdir(parents=True)
            (directory / "manifest.json").write_text(json.dumps(manifest))
            self.assertEqual(mg.cache_for_map("de_test", root), directory)
            duplicate = root / "de_test/hash2"
            duplicate.mkdir()
            (duplicate / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(mg.GeometryExtractionError, "found 2"):
                mg.cache_for_map("de_test", root)
            source.write_bytes(b"changed")
            with self.assertRaisesRegex(mg.GeometryExtractionError, "found 0"):
                mg.cache_for_map("de_test", root)


if __name__ == "__main__":
    unittest.main()
