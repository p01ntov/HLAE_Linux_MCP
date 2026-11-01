"""Conservative 3-D camera checks against extracted Source collision geometry.

The BVH contains actual map triangles. A cubic is bounded by recursively split
Bezier control hulls, not by a fixed list of timeline samples. Missing collision
coverage never becomes a successful validation. This is not a render visibility
or full frustum solver.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

DEFAULT_CLEARANCE = 8.0
MAX_CURVE_LEAVES = 131072
REQUIRED_COVERAGE = ("world_collision", "static_props", "glass", "inside_solid", "dynamic_entities")


class GeometryError(ValueError):
    pass


def _sha256(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def _vector(value, name="position"):
    if (not isinstance(value, (tuple, list)) or len(value) != 3
            or any(isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x)
                   or abs(x) > 1_000_000 for x in value)):
        raise GeometryError(f"{name} requires three finite Source coordinates within +/-1000000")
    return tuple(float(x) for x in value)


def _radius(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 1 <= value <= 128:
        raise GeometryError("Camera clearance must be within 1..128 Source units")
    return float(value)


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def _scale(a, s):
    return tuple(x * s for x in a)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _point_segment_sq(p, a, b):
    ab = _sub(b, a)
    d = _dot(ab, ab)
    t = max(0., min(1., _dot(_sub(p, a), ab) / d)) if d > 1e-24 else 0.
    delta = _sub(p, _add(a, _scale(ab, t)))
    return _dot(delta, delta)


def _point_triangle_sq(p, a, b, c):
    # Ericson's Voronoi-region closest point; degenerate triangles fall back to
    # their edges, so thin features cannot silently disappear.
    ab, ac, ap = _sub(b, a), _sub(c, a), _sub(p, a)
    d1, d2 = _dot(ab, ap), _dot(ac, ap)
    if d1 <= 0 and d2 <= 0:
        return _dot(ap, ap)
    bp = _sub(p, b)
    d3, d4 = _dot(ab, bp), _dot(ac, bp)
    if d3 >= 0 and d4 <= d3:
        return _dot(bp, bp)
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0 and d1 != d3:
        q = _sub(p, _add(a, _scale(ab, d1 / (d1 - d3))))
        return _dot(q, q)
    cp = _sub(p, c)
    d5, d6 = _dot(ab, cp), _dot(ac, cp)
    if d6 >= 0 and d5 <= d6:
        return _dot(cp, cp)
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0 and d2 != d6:
        q = _sub(p, _add(a, _scale(ac, d2 / (d2 - d6))))
        return _dot(q, q)
    va = d3 * d6 - d5 * d4
    if va <= 0 and d4 - d3 >= 0 and d5 - d6 >= 0 and d4 - d3 + d5 - d6 > 0:
        q = _sub(p, _add(b, _scale(_sub(c, b), (d4 - d3) / (d4 - d3 + d5 - d6))))
        return _dot(q, q)
    denom = va + vb + vc
    if abs(denom) < 1e-24:
        return min(_point_segment_sq(p, a, b), _point_segment_sq(p, b, c), _point_segment_sq(p, c, a))
    q = _sub(p, _add(a, _add(_scale(ab, vb / denom), _scale(ac, vc / denom))))
    return max(0., _dot(q, q))


def _segments_sq(a, b, c, d):
    u, v, w = _sub(b, a), _sub(d, c), _sub(a, c)
    aa, bb, cc, dd, ee = _dot(u, u), _dot(u, v), _dot(v, v), _dot(u, w), _dot(v, w)
    if aa <= 1e-24:
        return _point_segment_sq(a, c, d)
    if cc <= 1e-24:
        return _point_segment_sq(c, a, b)
    denominator = aa * cc - bb * bb
    s = max(0., min(1., (bb * ee - cc * dd) / denominator)) if denominator > 1e-24 else 0.
    t = (bb * s + ee) / cc
    if t < 0:
        t, s = 0., max(0., min(1., -dd / aa))
    elif t > 1:
        t, s = 1., max(0., min(1., (bb - dd) / aa))
    delta = _sub(_add(w, _scale(u, s)), _scale(v, t))
    return max(0., _dot(delta, delta))


def _segment_triangle_sq(start, end, a, b, c):
    direction, e1, e2 = _sub(end, start), _sub(b, a), _sub(c, a)
    h = _cross(direction, e2)
    determinant = _dot(e1, h)
    if abs(determinant) > 1e-14:
        inverse = 1 / determinant
        s = _sub(start, a)
        u = inverse * _dot(s, h)
        q = _cross(s, e1)
        v = inverse * _dot(direction, q)
        t = inverse * _dot(e2, q)
        if -1e-10 <= u <= 1 + 1e-10 and v >= -1e-10 and u + v <= 1 + 1e-10 and -1e-10 <= t <= 1 + 1e-10:
            return 0.
    return min(_point_triangle_sq(start, a, b, c), _point_triangle_sq(end, a, b, c),
               _segments_sq(start, end, a, b), _segments_sq(start, end, b, c), _segments_sq(start, end, c, a))


def _segment_box(start, end, low, high, radius=0.):
    left, right = 0., 1.
    for axis in range(3):
        direction = end[axis] - start[axis]
        lo, hi = low[axis] - radius, high[axis] + radius
        if abs(direction) < 1e-15:
            if not lo <= start[axis] <= hi:
                return False
        else:
            x, y = (lo - start[axis]) / direction, (hi - start[axis]) / direction
            left, right = max(left, min(x, y)), min(right, max(x, y))
            if left > right:
                return False
    return True


def _split_cubic(control):
    a, b, c, d = control
    ab, bc, cd = (_scale(_add(x, y), .5) for x, y in ((a, b), (b, c), (c, d)))
    abc, bcd = _scale(_add(ab, bc), .5), _scale(_add(bc, cd), .5)
    middle = _scale(_add(abc, bcd), .5)
    return (a, ab, abc, middle), (middle, bcd, cd, d)


def project_cubics(project):
    """Exact native position geometry as Bezier controls, independent of easing.

    LiveTrack validates all channel controls and live authoring bounds. Its
    canonical position_controls keeps this checker in step with native sampling.
    """
    from .live_track import LiveTrack
    if not isinstance(project, dict) or not isinstance(project.get("keys"), list):
        raise GeometryError("Camera project must contain a keys array")
    for field, default, allowed in (("interpolation", "smooth", {"linear", "smooth", "bezier"}),
                                    ("rotation_interpolation", "slerp", {"slerp", "bezier"}),
                                    ("fov_interpolation", "linear", {"linear", "bezier"})):
        mode = project.get(field, default)
        if not isinstance(mode, str) or mode not in allowed:
            raise GeometryError(f"Unknown {field} mode")
    if not project["keys"]:
        return [], []
    for key in project["keys"]:
        if not isinstance(key, dict):
            raise GeometryError("Camera keys must be objects")
        fov = key.get("fov", 90)
        if isinstance(fov, bool) or not isinstance(fov, (int, float)) or not math.isfinite(fov) or not 1 <= fov <= 179:
            raise GeometryError("Live camera FOV must be within 1..179 degrees")
    track = LiveTrack(project)
    keys, cubics = track.keys, []
    for i, (a, b) in enumerate(zip(keys, keys[1:])):
        controls = track.position_controls(i)
        cubics.append({"controls": controls, "keys": [a.identity, b.identity], "time": [a.time, b.time]})
    return [key.position for key in keys], cubics


class Geometry:
    def __init__(self, manifest, vertices, triangles, triangle_component, cache_path=None):
        import numpy as np
        self.np, self.manifest, self.cache_path = np, manifest, str(cache_path) if cache_path else None
        self.vertices = np.asarray(vertices, dtype=np.float64)
        self.triangles = np.asarray(triangles, dtype=np.int64)
        self.triangle_component = np.asarray(triangle_component, dtype=np.int64)
        if (self.vertices.ndim != 2 or self.vertices.shape[1] != 3 or not np.isfinite(self.vertices).all()
                or np.abs(self.vertices).max() > 1_000_000
                or self.triangles.ndim != 2 or self.triangles.shape[1] != 3
                or not len(self.triangles) or len(self.triangles) > 5_000_000 or len(self.vertices) > 5_000_000
                or len(self.triangle_component) != len(self.triangles)
                or self.triangles.min() < 0 or self.triangles.max() >= len(self.vertices)):
            raise GeometryError("Invalid or excessive map triangle arrays")
        self.components = manifest.get("components", [])
        if (not isinstance(self.components, list) or not self.components or any(not isinstance(c, dict) for c in self.components)
                or self.triangle_component.min() < 0
                or self.triangle_component.max() >= len(self.components)):
            raise GeometryError("Map geometry is missing component identity")
        self.faces = self.vertices[self.triangles]
        self.lows, self.highs = self.faces.min(axis=1), self.faces.max(axis=1)
        self.centers = (self.lows + self.highs) * .5
        self.nodes = []
        self._build(np.arange(len(self.triangles), dtype=np.int64))
        self.hulls = []
        self.maximum_hull_plane_expansion = 0.
        self.hull_lows, self.hull_highs = [], []
        for index, component in enumerate(self.components):
            if not component.get("convex") or not component.get("closed"):
                continue
            begin, count = component.get("triangle_start"), component.get("triangle_count")
            if not isinstance(begin, int) or not isinstance(count, int) or begin < 0 or count < 4 or begin + count > len(self.triangles):
                raise GeometryError("Invalid convex hull triangle range")
            indices = np.arange(begin, begin + count)
            if not np.all(self.triangle_component[indices] == index):
                raise GeometryError("Convex hull component range does not match triangle identity")
            faces = self.faces[indices]
            center = faces.reshape(-1, 3).mean(axis=0)
            normals = np.cross(faces[:, 1] - faces[:, 0], faces[:, 2] - faces[:, 0])
            lengths = np.linalg.norm(normals, axis=1)
            nonzero = lengths > 1e-9
            normals = normals[nonzero] / lengths[nonzero, None]
            offsets = -(normals * faces[nonzero, 0]).sum(axis=1)
            orientation = normals @ center + offsets
            normals[orientation > 0] *= -1
            offsets[orientation > 0] *= -1
            hull_vertices = self.vertices[np.unique(self.triangles[indices])]
            if len(normals) < 4:
                raise GeometryError("Claimed convex hull lacks solid boundary planes")
            # Native hull face fans can contain slightly nonplanar/quantized
            # vertices. Enclose every source vertex rather than dropping a bad
            # face or creating a false-negative hole in the interior test.
            expansion = np.maximum(0., (normals @ hull_vertices.T + offsets[:, None]).max(axis=1))
            self.maximum_hull_plane_expansion = max(self.maximum_hull_plane_expansion, float(expansion.max()))
            offsets -= expansion + 1e-5
            self.hulls.append((index, normals, offsets, float(expansion.max())))
            self.hull_lows.append(hull_vertices.min(axis=0))
            self.hull_highs.append(hull_vertices.max(axis=0))
        self.hull_lows, self.hull_highs = np.asarray(self.hull_lows), np.asarray(self.hull_highs)
        self.unknown_bounds = []
        for value in manifest.get("unresolved_entities_bounds", []):
            bounds = value.get("bounds") if isinstance(value, dict) else value
            if not isinstance(bounds, (list, tuple)) or len(bounds) != 2:
                raise GeometryError("Unresolved entity bounds require finite [min,max] XYZ")
            lo, hi = _vector(bounds[0], "unknown bounds"), _vector(bounds[1], "unknown bounds")
            if any(a > b for a, b in zip(lo, hi)):
                raise GeometryError("Unresolved entity bounds are reversed")
            self.unknown_bounds.append((lo, hi, value))

    def _build(self, indices):
        np = self.np
        index = len(self.nodes)
        low, high = self.lows[indices].min(axis=0), self.highs[indices].max(axis=0)
        self.nodes.append(None)
        if len(indices) <= 24:
            self.nodes[index] = (low, high, indices, None, None)
        else:
            axis = int(np.argmax(self.centers[indices].max(axis=0) - self.centers[indices].min(axis=0)))
            middle = len(indices) // 2
            ordered = indices[np.argpartition(self.centers[indices, axis], middle)]
            left, right = self._build(ordered[:middle]), self._build(ordered[middle:])
            self.nodes[index] = (low, high, None, left, right)
        return index

    def _candidates(self, start, end, radius):
        stack = [0]
        while stack:
            low, high, indices, left, right = self.nodes[stack.pop()]
            if not _segment_box(start, end, low, high, radius):
                continue
            if indices is None:
                stack.extend((left, right))
            else:
                for index in indices:
                    if _segment_box(start, end, self.lows[index], self.highs[index], radius):
                        yield int(index)

    def status(self):
        coverage = self.manifest.get("coverage", {})
        missing = [key for key in REQUIRED_COVERAGE if coverage.get(key) is not True]
        if any(not c.get("closed") or not c.get("convex") for c in self.components) and "inside_solid" not in missing:
            # This kernel classifies convex source solids only. A manifest may
            # not turn unsupported arbitrary mesh interiors into a proof.
            missing.append("inside_solid")
        bounded = self.manifest.get("unresolved_entities_bounds_complete") is True
        ready = self.manifest.get("ready") is True and (not missing or bounded)
        # "complete bounds" must be explicit and populated whenever coverage is
        # missing; an empty list must not accidentally authorize an unknown map.
        if missing and not self.unknown_bounds:
            ready = False
        reasons = list(self.manifest.get("unsupported", []))
        if missing and not ready:
            reasons.insert(0, "Unbounded or incomplete collision coverage: " + ", ".join(missing))
        return {"map": self.manifest.get("map_name"), "status": "ready" if ready else "partial",
                "spatial_mutations_allowed": ready, "static_geometry_available": True,
                "coverage": coverage, "missing_coverage": missing,
                "unresolved_bounds_complete": bounded, "unresolved_bounds_count": len(self.unknown_bounds),
                "reason": "; ".join(reasons) if reasons else "Extracted collision geometry ready",
                "source_sha256": self.manifest.get("identity", {}).get("source_sha256"),
                "cache_path": self.cache_path, "triangle_count": len(self.triangles),
                "convex_hull_count": len(self.hulls), "clearance_units": DEFAULT_CLEARANCE,
                "maximum_hull_plane_expansion_units": self.maximum_hull_plane_expansion,
                "dynamic_state_verified": coverage.get("dynamic_entities") is True,
                "frustum_checked": False, "render_visibility_checked": False}

    def _inside(self, point):
        np = self.np
        if not self.hulls:
            return None
        p = np.asarray(point)
        possible = np.nonzero(np.all(self.hull_lows <= p + 1e-5, axis=1) & np.all(self.hull_highs >= p - 1e-5, axis=1))[0]
        for hull in possible:
            component, normals, offsets, expansion = self.hulls[int(hull)]
            if np.all(normals @ p + offsets <= 1e-5):
                return component, expansion
        return None

    def _hit(self, start, end, radius):
        inside = self._inside(start)
        if inside is not None:
            return {"kind": "inside_solid", "component": inside[0], "position": list(start),
                    "conservative_plane_expansion_units": inside[1]}
        for lo, hi, value in self.unknown_bounds:
            if _segment_box(start, end, lo, hi, radius):
                return {"kind": "unresolved_entity_bounds", "bounds": [list(lo), list(hi)],
                        "reason": value.get("reason", "Unverified entity collision") if isinstance(value, dict) else "Unverified entity collision"}
        limit = (radius + 1e-5) ** 2
        for index in self._candidates(start, end, radius + 1e-5):
            a, b, c = (tuple(row) for row in self.faces[index])
            distance = _segment_triangle_sq(start, end, a, b, c)
            if distance <= limit:
                component = int(self.triangle_component[index])
                return {"kind": "surface_clearance", "triangle": index, "component": component,
                        "component_kind": self.components[component].get("kind", "unknown"),
                        "distance_units": math.sqrt(distance)}
        return None

    def _result(self, collision, radius, **details):
        status = self.status()
        passed = collision is None and status["spatial_mutations_allowed"]
        return {"passed": passed, "status": "blocked" if collision else "clear" if passed else "unknown",
                "complete": status["spatial_mutations_allowed"], "static_clear": collision is None,
                "clearance_units": radius, "collision": collision, "geometry": status,
                "frustum_checked": False, "render_visibility_checked": False, **details}

    def validate_position(self, position, clearance=DEFAULT_CLEARANCE):
        point, radius = _vector(position), _radius(clearance)
        return self._result(self._hit(point, point, radius), radius, position=list(point),
                            method="sphere_against_triangles_and_convex_solids")

    def validate_segment(self, start, end, clearance=DEFAULT_CLEARANCE):
        start, end, radius = _vector(start), _vector(end), _radius(clearance)
        return self._result(self._hit(start, end, radius), radius, method="swept_sphere_capsule",
                            start=list(start), end=list(end))

    def validate_project(self, project, clearance=DEFAULT_CLEARANCE):
        radius = _radius(clearance)
        positions, cubics = project_cubics(project)
        leaves, checks = 0, 0
        for key_index, position in enumerate(positions):
            hit = self._hit(position, position, radius)
            checks += 1
            if hit:
                hit.update(key_index=key_index, position=list(position))
                return self._result(hit, radius, method="conservative_cubic_capsules", checked_leaves=leaves, checks=checks)
        for segment in cubics:
            stack = [(segment["controls"], 0., 1., 0)]
            while stack:
                control, begin, end, depth = stack.pop()
                deviation = math.sqrt(max(_point_segment_sq(control[1], control[0], control[3]),
                                          _point_segment_sq(control[2], control[0], control[3])))
                hit = self._hit(control[0], control[3], radius + deviation)
                checks += 1
                if not hit:
                    leaves += 1
                    continue
                if deviation <= .02 or depth >= 20:
                    hit.update(keys=segment["keys"], curve_parameter_interval=[begin, end],
                               key_time_interval=segment["time"],
                               curve_segment=[list(control[0]), list(control[3])],
                               curve_bound_units=deviation)
                    return self._result(hit, radius, method="conservative_cubic_capsules", checked_leaves=leaves, checks=checks)
                if checks >= MAX_CURVE_LEAVES:
                    return self._result({"kind": "validation_budget_exceeded", "reason": "Curve cannot be conservatively resolved within the bounded validation budget"},
                                        radius, method="conservative_cubic_capsules", checked_leaves=leaves, checks=checks)
                left, right = _split_cubic(control)
                middle = (begin + end) * .5
                stack.extend(((right, middle, end, depth + 1), (left, begin, middle, depth + 1)))
        return self._result(None, radius, method="conservative_cubic_capsules", checked_leaves=leaves, checks=checks,
                            key_count=len(positions), curve_count=len(cubics),
                            guarantee="Swept sphere bounded over complete native linear/Hermite/Bezier spatial curves including explicit handles against indexed geometry only")

    def preview(self):
        """Bounded actual triangle/plane intersections for the in-game 2-D map."""
        np, faces = self.np, self.faces
        normals = np.cross(faces[:, 1] - faces[:, 0], faces[:, 2] - faces[:, 0])
        lengths = np.linalg.norm(normals, axis=1)
        horizontal = np.abs(normals[:, 2]) > .95 * lengths
        levels = {}
        for z, area in zip(faces[horizontal, :, 2].mean(axis=1), lengths[horizontal]):
            level = round(float(z) / 32) * 32
            levels[level] = levels.get(level, 0.) + float(area)
        selected = []
        for floor in sorted(levels, key=levels.get, reverse=True):
            z = floor + 48.
            if all(abs(z - previous) >= 64 for previous in selected):
                selected.append(z)
            if len(selected) == 8:
                break
        if not selected:
            selected = [float(np.median(faces[:, :, 2]))]
        layers = []
        for z in sorted(selected):
            candidates = faces[(faces[:, :, 2].min(axis=1) < z) & (faces[:, :, 2].max(axis=1) > z)]
            lines = {}
            for face in candidates:
                hits = []
                for a, b in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                    if (a[2] <= z < b[2]) or (b[2] <= z < a[2]):
                        point = a + (b - a) * ((z - a[2]) / (b[2] - a[2]))
                        hits.append((round(float(point[0]), 2), round(float(point[1]), 2)))
                if len(hits) == 2 and hits[0] != hits[1]:
                    key = tuple(sorted(hits))
                    lines[key] = math.dist(*key)
            # The minimap is decimated visual context, never the collision mesh.
            chosen = sorted(lines, key=lines.get, reverse=True)[:1000]
            layers.append({"z": z, "lines": [[*a, *b] for a, b in chosen]})
        bounds = [float(self.vertices[:, 0].min()), float(self.vertices[:, 1].min()),
                  float(self.vertices[:, 0].max()), float(self.vertices[:, 1].max())]
        status = self.status()
        result = {"map": status["map"], "status": "static" if status["spatial_mutations_allowed"] else "partial",
                  "reason": status["reason"].encode("utf-8")[:1024].decode("utf-8", "ignore"),
                  "bounds": bounds, "layers": layers, "source_sha256": status["source_sha256"] or ""}
        while len(json.dumps(result, separators=(",", ":"), ensure_ascii=False).encode("utf-8")) > 384 * 1024:
            for layer in layers:
                layer["lines"] = layer["lines"][:max(0, len(layer["lines"]) - 100)]
        return result


_CACHE = {}


def load_geometry(cache_path):
    path = Path(cache_path).expanduser().resolve()
    manifest_path, geometry_path = path / "manifest.json", path / "geometry.npz"
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get("format") != "cs2-camera-geometry" or manifest.get("version") != 1
            or manifest.get("coordinates") != "source_z_up" or not isinstance(manifest.get("ready"), bool)):
        raise GeometryError("Map geometry cache is not a recognized Source Z-up extraction")
    sources = manifest.get("source_files")
    if not isinstance(sources, list) or not sources or any(not isinstance(x, dict) or not x.get("sha256") for x in sources):
        raise GeometryError("Map geometry has no verifiable installed source identity")
    fingerprints = []
    for file in [manifest_path, geometry_path] + [Path(x["path"]) for x in sources]:
        info = file.stat()
        fingerprints.append((str(file), info.st_size, info.st_mtime_ns))
    fingerprint = tuple(fingerprints)
    key = str(path)
    if key in _CACHE and _CACHE[key][0] == fingerprint:
        return _CACHE[key][1]
    if _sha256(geometry_path) != manifest.get("geometry_sha256"):
        raise GeometryError("Map geometry NPZ hash mismatch")
    for source in sources:
        if _sha256(source["path"]) != source["sha256"]:
            raise GeometryError("Installed map changed; rebuild geometry cache before camera authoring")
    import numpy as np
    with np.load(geometry_path, allow_pickle=False) as arrays:
        geometry = Geometry(manifest, arrays["vertices"], arrays["triangles"], arrays["triangle_component"], path)
    _CACHE.clear()
    _CACHE[key] = fingerprint, geometry
    return geometry
