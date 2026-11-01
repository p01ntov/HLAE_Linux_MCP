"""Read-only installed Source 2 physics indexing for camera collision checks.

ValveResourceFormat decodes PHYS; this module retains each convex hull instead
of flattening unrelated solids into one glTF mesh. It never changes game files.
Non-baked props, moving doors and demo entity state are explicitly not covered.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import urllib.request
import zipfile
import zlib

import numpy as np

FORMAT_VERSION = 1
EXTRACTOR_REVISION = 1
VRF_VERSION = "20.1"
VRF_ARCHIVE_SHA256 = "29d25a6d6b62377fc5386c3e5145b0a2180a401a50041afe9494b3f503deaef5"
VRF_EXECUTABLE_SHA256 = "3aaa77568beaf6b130d974993a42088d8ce58b2e001fdda42a2fd8aae58ffb4d"
VRF_URL = f"https://github.com/ValveResourceFormat/ValveResourceFormat/releases/download/{VRF_VERSION}/cli-linux-x64.zip"
MAX_TEXT = 512 * 1024**2
MAX_RAW_TEXT = 2 * 1024**3
MAX_TRIANGLES = 8_000_000
MAX_VERTICES = 8_000_000
MAX_COMPONENTS = 200_000


class GeometryExtractionError(ValueError):
    pass


def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda: f.read(1024**2), b""):
            h.update(block)
    return h.hexdigest()


def bootstrap_tool(directory):
    """Fetch the pinned official Linux decoder into an explicit cache folder."""
    directory = Path(directory)
    executable = directory / f"vrf-{VRF_VERSION}" / "Source2Viewer-CLI"
    archive = directory / f"cli-linux-x64-{VRF_VERSION}.zip"
    directory.mkdir(parents=True, exist_ok=True)
    if not archive.is_file() or file_hash(archive) != VRF_ARCHIVE_SHA256:
        temporary = archive.with_suffix(".download")
        try:
            with urllib.request.urlopen(VRF_URL, timeout=45) as response, temporary.open("wb") as output:
                total = 0
                while chunk := response.read(1024**2):
                    total += len(chunk)
                    if total > 80 * 1024**2:
                        raise GeometryExtractionError("VRF download exceeds 80 MiB")
                    output.write(chunk)
            if file_hash(temporary) != VRF_ARCHIVE_SHA256:
                raise GeometryExtractionError("Official VRF archive checksum mismatch")
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    with zipfile.ZipFile(archive) as z:
        for name in ("Source2Viewer-CLI", "libSkiaSharp.so", "libspirv-cross.so"):
            info = z.getinfo(name)
            if info.file_size > 160 * 1024**2:
                raise GeometryExtractionError("VRF executable exceeds extraction bound")
            target = executable.parent / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst)
            target.chmod(0o755)
    return executable


class _KV3:
    """Bounded text KV3 reader for official decoder dumps, including hex blobs."""
    atom = re.compile(r'[^\s{}\[\],=]+')
    number = re.compile(r'[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?\Z')

    def __init__(self, text):
        if len(text.encode("utf-8")) > MAX_TEXT:
            raise GeometryExtractionError("Decoded PHYS text exceeds 512 MiB")
        marker = text.find("<!-- kv3 ")
        if marker >= 0:
            end = text.find("-->", marker)
            if end < 0:
                raise GeometryExtractionError("Truncated KV3 header")
            text = text[end + 3:]
        self.text, self.p, self.count = text, 0, 0

    def skip(self):
        t = self.text
        while self.p < len(t):
            if t[self.p].isspace() or t[self.p] == ',':
                self.p += 1
            elif t.startswith("//", self.p):
                end = t.find("\n", self.p)
                self.p = len(t) if end < 0 else end + 1
            else:
                break

    def token(self):
        self.skip()
        if self.p >= len(self.text):
            raise GeometryExtractionError("Truncated PHYS KV3")
        if self.text.startswith('"""', self.p):
            end = self.text.find('"""', self.p + 3)
            if end < 0:
                raise GeometryExtractionError("Truncated KV3 multiline string")
            value = self.text[self.p + 3:end]
            self.p = end + 3
            return value
        if self.text[self.p] == '"':
            try:
                value, end = json.decoder.scanstring(self.text, self.p + 1, True)
            except ValueError as error:
                raise GeometryExtractionError("Invalid KV3 string") from error
            self.p = end
            return value
        match = self.atom.match(self.text, self.p)
        if not match:
            raise GeometryExtractionError(f"Unexpected KV3 token at {self.p}")
        self.p = match.end()
        return match.group()

    def value(self, depth=0):
        self.skip()
        self.count += 1
        if depth > 64 or self.count > 12_000_000:
            raise GeometryExtractionError("PHYS KV3 nesting/value limit exceeded")
        if self.p >= len(self.text):
            raise GeometryExtractionError("Truncated PHYS KV3 value")
        text, ch = self.text, self.text[self.p]
        flag = re.match(r'(?:resource_name|resource|panorama|soundevent|subclass):', text[self.p:self.p + 32])
        if flag:
            self.p += flag.end()
            return self.value(depth + 1)
        if text.startswith("#[", self.p):
            end = text.find("]", self.p + 2)
            if end < 0:
                raise GeometryExtractionError("Truncated KV3 binary blob")
            try:
                result = bytes.fromhex(text[self.p + 2:end])
            except ValueError as error:
                raise GeometryExtractionError("Invalid KV3 binary blob") from error
            self.p = end + 1
            return result
        if ch == '{':
            self.p += 1
            result = {}
            while True:
                self.skip()
                if self.p >= len(text):
                    raise GeometryExtractionError("Truncated KV3 object")
                if text[self.p] == '}':
                    self.p += 1
                    return result
                key = self.token()
                self.skip()
                if self.p >= len(text) or text[self.p] != '=' or key in result:
                    raise GeometryExtractionError("Invalid or duplicate KV3 field")
                self.p += 1
                result[key] = self.value(depth + 1)
        if ch == '[':
            self.p += 1
            result = []
            while True:
                self.skip()
                if self.p >= len(text):
                    raise GeometryExtractionError("Truncated KV3 array")
                if text[self.p] == ']':
                    self.p += 1
                    return result
                result.append(self.value(depth + 1))
        quoted = ch == '"'
        token = self.token()
        if quoted:
            return token
        if token in ("true", "false", "null"):
            return {"true": True, "false": False, "null": None}[token]
        if self.number.fullmatch(token):
            return float(token) if any(c in token for c in '.eE') else int(token)
        raise GeometryExtractionError(f"Unsupported PHYS KV3 scalar: {token[:80]}")


def parse_physics_dump(text):
    reader = _KV3(text)
    value = reader.value()
    reader.skip()
    if reader.p != len(reader.text) or not isinstance(value, dict) or not isinstance(value.get("m_parts"), list):
        raise GeometryExtractionError("PHYS dump is not a complete physics aggregate")
    return value


def _array(value, dtype, width, *, fields=None):
    if isinstance(value, bytes):
        if len(value) % (np.dtype(dtype).itemsize * width):
            raise GeometryExtractionError("Misaligned physics binary array")
        result = np.frombuffer(value, dtype=dtype).reshape(-1, width)
    else:
        if fields:
            value = [[row[field] for field in fields] for row in value]
        elif value and isinstance(value[0], dict):
            value = [[row[key] for key in ("x", "y", "z")] for row in value]
        result = np.asarray(value, dtype=dtype).reshape(-1, width)
    if len(result) > MAX_VERTICES or not np.isfinite(result).all():
        raise GeometryExtractionError("Physics array exceeds bounds or contains non-finite values")
    return result


def _hull_triangles(hull, vertex_count):
    edges = _array(hull["m_Edges"], "u1", 4,
                   fields=("m_nNext", "m_nTwin", "m_nOrigin", "m_nFace"))
    faces = hull["m_Faces"]
    starts = list(faces) if isinstance(faces, bytes) else [f["m_nEdge"] for f in faces]
    if not 1 <= len(edges) <= 256:
        raise GeometryExtractionError("Invalid convex hull half-edge count")
    for index, edge in enumerate(edges):
        twin, following = int(edge[1]), int(edge[0])
        if twin >= len(edges) or following >= len(edges) or int(edges[twin, 1]) != index:
            raise GeometryExtractionError("Convex hull has an invalid twin/next edge")
        if int(edges[following, 2]) != int(edges[twin, 2]):
            raise GeometryExtractionError("Convex hull twin endpoint does not match the face loop")
    triangles, visited = [], set()
    for face, start in enumerate(starts):
        ring, current = [], int(start)
        while True:
            if current >= len(edges) or current in ring or len(ring) >= 255:
                raise GeometryExtractionError("Invalid convex hull half-edge cycle")
            ring.append(current)
            edge = edges[current]
            if int(edge[2]) >= vertex_count or int(edge[3]) != face:
                raise GeometryExtractionError("Convex hull half-edge references invalid vertex/face")
            current = int(edge[0])
            if current == start:
                break
        if len(ring) < 3:
            raise GeometryExtractionError("Convex hull has fewer than three face vertices")
        indices = [int(edges[index, 2]) for index in ring]
        visited.update(ring)
        triangles.extend((indices[0], indices[i], indices[i + 1]) for i in range(1, len(indices) - 1))
    if len(visited) != len(edges):
        raise GeometryExtractionError("Convex hull contains unreferenced half-edges")
    return np.asarray(triangles, dtype="<u4").reshape(-1, 3)


def physics_arrays(physics):
    """Convert decoded source-local hull/mesh physics to the shared NPZ schema."""
    vertices, triangles, membership, components, unsupported = [], [], [], [], []
    nv = nt = 0
    poses = physics.get("m_bindPose", [])
    attributes = physics.get("m_collisionAttributes", [])
    for part_index, part in enumerate(physics["m_parts"]):
        shape = part["m_rnShape"]
        for name, value in shape.items():
            if name not in {"m_spheres", "m_capsules", "m_hulls", "m_meshes", "m_CollisionAttributeIndices"} and value:
                unsupported.append(f"part {part_index}: unsupported shape field {name}")
        if shape.get("m_spheres") or shape.get("m_capsules"):
            unsupported.append(f"part {part_index}: analytic sphere/capsule shapes not indexed")
        pose = None
        if poses:
            pose = np.asarray(poses[part_index], dtype="<f8").reshape(3, 4)
            if not np.isfinite(pose).all():
                raise GeometryExtractionError("Invalid physics bind pose")
        for key, inner, kind, convex in (("m_hulls", "m_Hull", "world_hull", True),
                                         ("m_meshes", "m_Mesh", "world_mesh", False)):
            for descriptor in shape.get(key, []):
                data = descriptor[inner]
                raw_vertices = data.get("m_VertexPositions", data.get("m_Vertices"))
                verts = _array(raw_vertices, "<f4", 3)
                if pose is not None:
                    verts = np.asarray(verts @ pose[:, :3].T + pose[:, 3], dtype="<f4")
                if convex:
                    faces = _hull_triangles(data, len(verts))
                else:
                    raw_triangles = data["m_Triangles"]
                    if not isinstance(raw_triangles, bytes):
                        raw_triangles = [v["m_nIndex"] for v in raw_triangles]
                    faces = _array(raw_triangles, "<u4", 3)
                if not len(verts) or not len(faces) or int(faces.max()) >= len(verts):
                    raise GeometryExtractionError("Physics component has empty or out-of-range triangle indices")
                if nv + len(verts) > MAX_VERTICES or nt + len(faces) > MAX_TRIANGLES or len(components) >= MAX_COMPONENTS:
                    raise GeometryExtractionError("Map physics geometry exceeds supported bounds")
                attr_index = int(descriptor.get("m_nCollisionAttributeIndex", 0))
                if not 0 <= attr_index < len(attributes):
                    raise GeometryExtractionError("Physics component references an invalid collision attribute")
                tags = attributes[attr_index].get("m_InteractAsStrings", [])
                components.append({"kind": kind, "triangle_start": nt, "triangle_count": len(faces),
                                   "closed": convex, "convex": convex,
                                   "bounds": [verts.min(axis=0).tolist(), verts.max(axis=0).tolist()],
                                   "part": part_index, "collision_attribute": attr_index,
                                   "collision_tags": tags, "surface_property": int(descriptor.get("m_nSurfacePropertyIndex", 0))})
                vertices.append(verts)
                triangles.append(faces + nv)
                membership.append(np.full(len(faces), len(components) - 1, dtype="<u4"))
                nv += len(verts)
                nt += len(faces)
    if not nt:
        raise GeometryExtractionError("World physics contains no supported triangles")
    return {"vertices": np.concatenate(vertices), "triangles": np.concatenate(triangles),
            "triangle_component": np.concatenate(membership)}, components, unsupported


def discover_maps(game_dirs, *, workshop_roots=()):
    result = {}
    for game in game_dirs:
        game = Path(game).resolve()
        roots = (game / "csgo/maps", game / "csgo/addons", game.parents[2] / "workshop/content/730",
                 *(Path(p) for p in workshop_roots))
        for root in roots:
            if not root.is_dir():
                continue
            for path in sorted(root.rglob("*.vpk")):
                if path.is_symlink() or not path.is_file() or re.search(r'_\d{3}\.vpk$', path.name):
                    continue
                result[str(path.resolve())] = {"path": str(path.resolve()), "game_directory": str(game),
                                               "workshop": "workshop" in path.parts}
                if len(result) > 4096:
                    raise GeometryExtractionError("Installed map archive count exceeds 4096")
    return list(result.values())


def _run(tool, arguments, output=None, *, timeout=120):
    from .discovery import external_environment
    if output is not None:
        with Path(output).open("w") as f:
            result = subprocess.run([str(tool), *map(str, arguments)], stdout=f, stderr=subprocess.PIPE,
                                    env=external_environment(), timeout=timeout, text=True)
    else:
        result = subprocess.run([str(tool), *map(str, arguments)], capture_output=True,
                                env=external_environment(), timeout=timeout, text=True)
    if result.returncode:
        raise GeometryExtractionError("Official Source 2 decoder failed: " + result.stderr[-2048:])
    return result.stdout if output is None else None


def _physics_entries(archive):
    """Read bounded VPK v1/v2 trees without treating map.vpk as *_dir.vpk."""
    with Path(archive).open("rb") as f:
        header = f.read(12)
        if len(header) != 12:
            raise GeometryExtractionError("Truncated map VPK header")
        magic, version, tree_size = struct.unpack("<III", header)
        if magic != 0x55AA1234 or version not in (1, 2) or not 1 <= tree_size <= 32 * 1024**2:
            raise GeometryExtractionError("Unsupported or oversized map VPK directory")
        f.seek(28 if version == 2 else 12)
        tree = f.read(tree_size)
        if len(tree) != tree_size:
            raise GeometryExtractionError("Truncated map VPK directory tree")
    cursor, entries, total = 0, [], 0
    def string():
        nonlocal cursor
        end = tree.find(b"\0", cursor)
        if end < 0 or end - cursor > 4096:
            raise GeometryExtractionError("Invalid map VPK tree string")
        value = tree[cursor:end].decode("utf-8")
        cursor = end + 1
        return value
    while extension := string():
        while directory := string():
            while name := string():
                if cursor + 18 > len(tree):
                    raise GeometryExtractionError("Truncated map VPK entry")
                crc, preload, chunk, offset, size, terminator = struct.unpack_from("<IHHIIH", tree, cursor)
                cursor += 18
                total += 1
                if total > 1_000_000 or terminator != 65535 or cursor + preload > len(tree):
                    raise GeometryExtractionError("Invalid or oversized map VPK entry table")
                cursor += preload
                path = ("" if directory == " " else directory + "/") + name + "." + extension
                if name == "world_physics" and extension in ("vphys_c", "vmdl_c"):
                    if path.startswith("/") or ".." in path.split("/") or "\\" in path:
                        raise GeometryExtractionError("Unsafe physics resource path")
                    entries.append({"resource": path, "archive": chunk, "crc32": crc})
    if cursor != len(tree):
        raise GeometryExtractionError("Unexpected map VPK directory suffix")
    return entries


def _select_physics_entry(entries):
    # VRF WorldLoader uses world_physics.vphys_c first, with .vmdl_c/PHYS as
    # fallback. Both may exist in the same map (installed cs_italy does this).
    # They describe alternate representations, not two placed world groups.
    parents = {entry["resource"].rsplit("/", 1)[0] for entry in entries}
    if len(parents) != 1:
        raise GeometryExtractionError(f"Expected one world physics group; found {len(parents)}")
    preferred = [entry for entry in entries if entry["resource"].endswith(".vphys_c")]
    selected = preferred or entries
    if len(selected) != 1:
        raise GeometryExtractionError("World physics has duplicate compiled resources")
    return selected[0]


def _compact_physics_dump(source, destination):
    """Discard unused SVM/mesh-BVH acceleration before loading KV3.

    The pinned decoder prints fields on indented lines. RegionSVM is a nested
    acceleration structure; mesh m_Nodes is its BVH. Positions, triangle indices,
    hull edges/faces/planes are separate
    sibling fields and remain intact. This avoids gigabytes of redundant text
    for large maps without dropping any indexed collision shape or triangle.
    """
    if Path(source).stat().st_size > MAX_RAW_TEXT:
        raise GeometryExtractionError("Raw decoded physics text exceeds 2 GiB")
    skipped, written = None, 0
    with Path(source).open() as src, Path(destination).open("w") as dst:
        for line in src:
            if skipped is not None:
                if line.rstrip("\r\n") == skipped[0] + skipped[1]:
                    skipped = None
                continue
            match = re.fullmatch(r'([ \t]*)(m_pRegionSVM|m_Nodes)[ \t]*=[ \t]*\r?\n', line)
            if match:
                skipped = (match[1], "}" if match[2] == "m_pRegionSVM" else "]")
                continue
            written += len(line.encode("utf-8"))
            if written > MAX_TEXT:
                raise GeometryExtractionError("Decoded collision geometry text exceeds 512 MiB after SVM compaction")
            dst.write(line)
    if skipped is not None:
        raise GeometryExtractionError("Truncated decoded physics RegionSVM field")


def index_map(archive, cache_root, tool, *, game_directory=None):
    """Index one installed archive with content-addressed provenance and limits."""
    archive, cache_root, tool = Path(archive).resolve(strict=True), Path(cache_root), Path(tool).resolve(strict=True)
    archive_hash = file_hash(archive)
    entry, source_paths = _select_physics_entry(_physics_entries(archive)), [archive]
    resource = entry["resource"]
    if entry["archive"] != 0x7fff:
        stem = archive.name.removesuffix("_dir.vpk") if archive.name.endswith("_dir.vpk") else archive.stem
        source_paths.append(archive.with_name(stem + f"_{entry['archive']:03d}.vpk"))
    source_files = []
    for path in source_paths:
        info = path.stat()
        source_files.append({"path": str(path), "sha256": archive_hash if path == archive else file_hash(path),
                             "size": info.st_size, "mtime_ns": info.st_mtime_ns})
    map_name = resource.rsplit("/", 2)[-2]
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,128}', map_name):
        raise GeometryExtractionError("Invalid map name in physics resource")
    provenance = {}
    if game_directory:
        from .cosmetics_catalog import _game_provenance
        provenance = _game_provenance(Path(game_directory))
    tool_hash = file_hash(tool)
    if tool_hash != VRF_EXECUTABLE_SHA256:
        raise GeometryExtractionError("Source 2 decoder is not the pinned official VRF 20.1 Linux executable")
    identity = {"version": FORMAT_VERSION, "source_sha256": archive_hash, "source_files": source_files,
                "resource": resource, "client_build_id": provenance.get("client_build_id"),
                "tool_sha256": tool_hash, "extractor_revision": EXTRACTOR_REVISION}
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    destination = cache_root / map_name / key
    if (destination / "manifest.json").is_file():
        manifest = json.loads((destination / "manifest.json").read_text())
        if manifest.get("identity") == identity and file_hash(destination / "geometry.npz") == manifest.get("geometry_sha256"):
            return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".index-", dir=destination.parent) as temporary:
        work = Path(temporary)
        raw, dump = work / Path(resource).name, work / "physics-raw.kv3"
        _run(tool, ["-i", archive, "--vpk_filepath", resource, "-o", raw])
        if not raw.is_file() or raw.stat().st_size > 128 * 1024**2:
            raise GeometryExtractionError("Extracted world physics resource is absent or exceeds 128 MiB")
        if zlib.crc32(raw.read_bytes()) & 0xffffffff != entry["crc32"]:
            raise GeometryExtractionError("Decoded physics resource does not match the VPK directory CRC32")
        block = "DATA" if resource.endswith(".vphys_c") else "PHYS"
        _run(tool, ["-i", raw, "--block", block], dump)
        compact = work / "physics.kv3"
        _compact_physics_dump(dump, compact)
        text = compact.read_text()
        if "<!-- kv3 " not in text:
            raise GeometryExtractionError(f"Compiled world resource has no decoded {block} physics block")
        arrays, components, unsupported = physics_arrays(parse_physics_dump(text))
        geometry = work / "geometry.npz"
        np.savez_compressed(geometry, **arrays)
        bounds = [arrays["vertices"].min(axis=0).tolist(), arrays["vertices"].max(axis=0).tolist()]
        coverage = {"world_collision": not unsupported, "static_props": False, "glass": False,
                    "inside_solid": all(c["convex"] for c in components), "dynamic_entities": False}
        unsupported += ["non-baked static prop/model collision coverage is unverified",
                        "breakable glass and doors require current demo entity state",
                        "dynamic props, players, moving entities and render-only geometry are not indexed"]
        manifest = {"format": "cs2-camera-geometry", "version": FORMAT_VERSION, "map_name": map_name,
                    "coordinates": "source_z_up", "identity": identity, "ready": coverage["world_collision"],
                    "source_files": source_files,
                    "physics_resource": resource, "physics_resource_sha256": file_hash(raw),
                    "tool": {"name": "ValveResourceFormat", "version": VRF_VERSION, "sha256": tool_hash,
                             "archive_sha256": VRF_ARCHIVE_SHA256}, "game": provenance,
                    "bounds": bounds, "vertices": len(arrays["vertices"]), "triangles": len(arrays["triangles"]),
                    "components": components, "coverage": coverage, "unsupported": unsupported,
                    "unresolved_entities_bounds": [], "unresolved_entities_bounds_complete": False,
                    "geometry_sha256": file_hash(geometry), "created_utc": datetime.now(timezone.utc).isoformat()}
        (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # Raw game resources stay private temporary files; only derived collision
        # arrays and provenance survive. Publish complete files after validation.
        destination.mkdir(exist_ok=True)
        for name in ("geometry.npz", "manifest.json"):
            os.replace(work / name, destination / name)
    return destination


def cache_for_map(map_name, cache_root, game_directory=None):
    """Find one current cache; ambiguity, changed sources or builds fail closed.

    The collision loader still verifies cryptographic geometry/source hashes.
    This resolver only avoids choosing stale map/build identities by accident.
    """
    map_name = str(map_name).removesuffix(".vpk").removesuffix(".vmap").rsplit("/", 1)[-1]
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,128}', map_name):
        raise GeometryExtractionError("Invalid map name for geometry cache")
    build_id = None
    if game_directory:
        from .cosmetics_catalog import _game_provenance
        provenance = _game_provenance(Path(game_directory))
        build_id = provenance.get("client_build_id")
        if not isinstance(build_id, str) or not re.fullmatch(r'[0-9a-f]{2,256}', build_id):
            raise GeometryExtractionError("Cannot verify the explicitly selected game's client build ID; geometry cache matching is unavailable")
    matches = []
    for path in sorted((Path(cache_root) / map_name).glob("*/manifest.json")):
        try:
            if path.stat().st_size > 32 * 1024**2:
                continue
            manifest = json.loads(path.read_text())
            identity = manifest.get("identity", {})
            if (manifest.get("format") != "cs2-camera-geometry" or manifest.get("version") != FORMAT_VERSION
                    or manifest.get("map_name") != map_name or identity.get("extractor_revision") != EXTRACTOR_REVISION
                    or build_id is not None and identity.get("client_build_id") != build_id):
                continue
            sources = manifest.get("source_files", [])
            if not sources:
                continue
            if any(Path(item["path"]).stat().st_size != item["size"] or
                   Path(item["path"]).stat().st_mtime_ns != item["mtime_ns"] for item in sources):
                continue
            matches.append(path.parent)
        except (OSError, ValueError, TypeError, KeyError):
            continue
    if len(matches) != 1:
        raise GeometryExtractionError(f"Expected one current geometry cache for {map_name}; found {len(matches)}. Run the map indexer.")
    return matches[0]


def load_for_demo(demo, game_dir=None, cache_root=None):
    """Resolve a demo's indexed geometry directory without launching the game."""
    root = cache_root or os.environ.get("CS2_CAMERA_GEOMETRY_CACHE")
    if root is None:
        raise GeometryExtractionError("Set CS2_CAMERA_GEOMETRY_CACHE to the installed map geometry cache")
    from .demo import parser
    header = parser(Path(demo)).parse_header()
    map_name = header.get("map_name") or header.get("mapname")
    if not map_name:
        raise GeometryExtractionError("Demo header does not identify its map")
    return cache_for_map(map_name, root, game_directory=game_dir)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", action="append", help="Installed game directory; repeat for multiple libraries")
    parser.add_argument("--cache-root", type=Path, required=True)
    parser.add_argument("--tool", type=Path)
    parser.add_argument("--bootstrap-tool", action="store_true", help="Download the pinned official decoder into cache-root/tools")
    parser.add_argument("--map", action="append", help="Archive basename to index, e.g. de_nuke; omitted scans every archive")
    args = parser.parse_args(argv)
    args.cache_root = args.cache_root.expanduser()
    if not args.cache_root.is_absolute():
        parser.error("--cache-root must be an explicit absolute directory path, for example /mnt/ssd/cs2-map-geometry")
    from .discovery import find_games, steam_roots
    games = find_games(args.game_dir[0] if args.game_dir and len(args.game_dir) == 1 else None) if not args.game_dir or len(args.game_dir) == 1 else [Path(p) for p in args.game_dir]
    args.cache_root.mkdir(parents=True, exist_ok=True)
    tool = bootstrap_tool(args.cache_root / "tools") if args.bootstrap_tool else args.tool
    if tool is None:
        parser.error("Pass --tool or --bootstrap-tool for the official physics decoder")
    records = []
    workshop_roots = [root / "steamapps/workshop/content/730" for root in steam_roots()]
    for item in discover_maps(games, workshop_roots=workshop_roots):
        if args.map and Path(item["path"]).stem not in args.map:
            continue
        try:
            directory = index_map(item["path"], args.cache_root, tool, game_directory=item["game_directory"])
            record = {**item, "status": "indexed", "cache_directory": str(directory)}
        except Exception as error:
            record = {**item, "status": "unavailable", "error": str(error)[:2048]}
        records.append(record)
        print(json.dumps(record), flush=True)
        (args.cache_root / "index.json").write_text(json.dumps({"format": "cs2-camera-map-index", "version": 1, "maps": records}, indent=2) + "\n")
    return 0 if records and all(r["status"] == "indexed" for r in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
