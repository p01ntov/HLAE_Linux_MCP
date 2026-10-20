"""Deterministic 3D camera paths, sampled at output frame times.

The compiler is independent of CS2's renderer. It produces position/pitch/yaw
samples for the native frame updater, never wall-clock console input.
"""
from __future__ import annotations

from bisect import bisect_right
from dataclasses import dataclass
import math
from pathlib import Path


def vector(value, name="vector", size=3) -> tuple[float, ...]:
    if not isinstance(value, (list, tuple)) or len(value) != size:
        raise ValueError(f"{name} must contain {size} numbers")
    if any(isinstance(x, bool) or not isinstance(x, (float, int)) or not math.isfinite(x) for x in value):
        raise ValueError(f"{name} must contain finite numbers")
    return tuple(float(x) for x in value)


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def subtract(a, b):
    return tuple(x - y for x, y in zip(a, b))


def scale(a, s):
    return tuple(x * s for x in a)


def distance(a, b):
    return math.dist(a, b)


def blend(a, b, t):
    return add(scale(a, 1 - t), scale(b, t))


def easing(t: float, name: str) -> float:
    t = min(1.0, max(0.0, t))
    if name == "linear":
        return t
    if name == "smoothstep":
        return t * t * (3 - 2 * t)
    if name == "smootherstep":
        return t * t * t * (t * (t * 6 - 15) + 10)
    raise ValueError("easing must be linear, smoothstep, or smootherstep")


def nearest_angle(angle: float, previous: float) -> float:
    return previous + ((angle - previous + 180) % 360 - 180)


def look_angles(position, target) -> tuple[float, float]:
    delta = subtract(target, position)
    planar = math.hypot(delta[0], delta[1])
    if math.sqrt(sum(v * v for v in delta)) < 1e-6:
        raise ValueError("Camera and look-at target coincide")
    if planar < 1e-6:
        raise ValueError("A vertical look-at has undefined yaw; move the camera away from that axis")
    pitch = -math.degrees(math.atan2(delta[2], planar))
    yaw = math.degrees(math.atan2(delta[1], delta[0]))
    return pitch, yaw


class PathCurve:
    """Centripetal Catmull-Rom geometry, optionally reparameterized by distance.

    Control points are geometric anchors, not timestamped keyframes. Constant
    speed changes when intermediate anchors are passed; start/end stay fixed.
    Easing is applied once to the complete shot, so the camera does not stop at
    every control point. Straight constant-speed travel may deliberately use
    linear timing.
    """
    def __init__(self, points, curve="catmull_rom", constant_speed=True):
        self.points = [vector(p) for p in points]
        if not 2 <= len(self.points) <= 64:
            raise ValueError("A path requires 2..64 positions")
        if any(distance(a, b) < 1e-5 for a, b in zip(self.points, self.points[1:])):
            raise ValueError("Adjacent path anchors must differ; use a static shot for a hold")
        if curve not in {"linear", "catmull_rom"}:
            raise ValueError("curve must be linear or catmull_rom")
        if not isinstance(constant_speed, bool):
            raise ValueError("constant_speed must be boolean")
        self.curve = curve
        self.constant_speed = constant_speed
        self.parameters = [i / ((len(self.points) - 1) * 256) for i in range((len(self.points) - 1) * 256 + 1)]
        samples = [self.position_at_parameter(u) for u in self.parameters]
        lengths = [0.0]
        for a, b in zip(samples, samples[1:]):
            lengths.append(lengths[-1] + distance(a, b))
        self.length = lengths[-1]
        self.lengths = [v / self.length for v in lengths]

    def position_at_parameter(self, u):
        u = min(1.0, max(0.0, u))
        at = u * (len(self.points) - 1)
        i = min(int(at), len(self.points) - 2)
        local = at - i
        p1, p2 = self.points[i:i + 2]
        if self.curve == "linear" or len(self.points) == 2:
            return blend(p1, p2, local)
        p0 = self.points[i - 1] if i else add(p1, subtract(p1, p2))
        p3 = self.points[i + 2] if i + 2 < len(self.points) else add(p2, subtract(p2, p1))
        t0 = 0.0
        t1 = t0 + math.sqrt(distance(p0, p1))
        t2 = t1 + math.sqrt(distance(p1, p2))
        t3 = t2 + math.sqrt(distance(p2, p3))
        t = t1 + local * (t2 - t1)
        a1 = blend(p0, p1, (t - t0) / (t1 - t0))
        a2 = blend(p1, p2, (t - t1) / (t2 - t1))
        a3 = blend(p2, p3, (t - t2) / (t3 - t2))
        b1 = blend(a1, a2, (t - t0) / (t2 - t0))
        b2 = blend(a2, a3, (t - t1) / (t3 - t1))
        return blend(b1, b2, (t - t1) / (t2 - t1))

    def parameter(self, progress):
        progress = min(1.0, max(0.0, progress))
        if not self.constant_speed or progress in {0.0, 1.0}:
            return progress
        i = min(max(1, bisect_right(self.lengths, progress)), len(self.lengths) - 1)
        a, b = self.lengths[i - 1:i + 1]
        t = (progress - a) / (b - a) if b > a else 0
        return self.parameters[i - 1] + (self.parameters[i] - self.parameters[i - 1]) * t

    def sample(self, progress):
        return self.position_at_parameter(self.parameter(progress))


@dataclass
class SubjectTrack:
    ticks: list[int]
    positions: list[tuple[float, ...]]
    name: str
    steam_id: str

    def sample(self, tick):
        if not self.ticks or tick < self.ticks[0] or tick > self.ticks[-1]:
            raise ValueError(f"Target position is unavailable at tick {tick:.3f}")
        i = bisect_right(self.ticks, tick)
        if i == len(self.ticks):
            return self.positions[-1]
        a, b = self.ticks[i - 1], self.ticks[i]
        if b - a > 4:
            raise ValueError("Target trajectory contains a gap; refusing to fly across it")
        return blend(self.positions[i - 1], self.positions[i], (tick - a) / (b - a))

    def smooth_sample(self, tick, seconds):
        # Centered, normalized filtering has no wall-clock state and no startup
        # lag. At demo boundaries use available samples, never invent positions.
        if seconds <= 0:
            return self.sample(tick)
        sigma = seconds * 64
        left = max(self.ticks[0], math.floor(tick - 2 * sigma))
        right = min(self.ticks[-1], math.ceil(tick + 2 * sigma))
        total = (0.0, 0.0, 0.0)
        weights = 0.0
        for t in range(left, right + 1):
            w = math.exp(-.5 * ((t - tick) / sigma) ** 2)
            total = add(total, scale(self.sample(t), w))
            weights += w
        if not weights:
            raise ValueError("No target samples available for smoothing")
        return scale(total, 1 / weights)


def load_subject(demo: Path, identity: str, start: int, end: int) -> SubjectTrack:
    from .demo import parser
    source = parser(demo)
    matches = [p for p in source.parse_player_info().to_dict(orient="records")
               if str(p["steamid"]) == identity or p["name"].casefold() == identity.casefold()]
    if len(matches) != 1:
        raise ValueError("Camera target must match one exact nickname or SteamID64")
    player = matches[0]
    rows = source.parse_ticks(["X", "Y", "Z", "is_alive"], players=[int(player["steamid"])])
    rows = rows[(rows["tick"] >= max(0, start - 128)) & (rows["tick"] <= end + 128)].sort_values("tick")
    samples = rows.to_dict(orient="records")
    relevant = [r for r in samples if start <= int(r["tick"]) < end]
    if not relevant or any(r.get("is_alive") is not True for r in relevant):
        raise ValueError("Follow/look-at requires the selected actor to remain alive through this shot")
    ticks = [int(r["tick"]) for r in samples]
    points = [vector([r["X"], r["Y"], r["Z"]], "player position") for r in samples]
    if not ticks or ticks[0] > start or ticks[-1] < end - 1:
        raise ValueError("Target data does not cover the shot")
    if any(distance(a, b) > 256 for a, b in zip(points, points[1:])):
        raise ValueError("Target trajectory contains a teleport; split the camera shot")
    return SubjectTrack(ticks, points, player["name"], str(player["steamid"]))


def validate_floor(frames, spec):
    """Validate every sampled pose against a declared world-space floor plane.

    Adapted from the external Sol 6.1 max review. This does not trace map solids
    or certify the space between discrete samples.
    """
    def number(value):
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError("Expected finite numeric floor/camera value, excluding bool")
        return value
    clearance = number(spec.get("clearance", 32))
    if clearance <= 0:
        raise ValueError("clearance must be positive")
    floor = number(spec["floor_z"]) if "floor_z" in spec else None
    limit = number(floor + clearance) if floor is not None else None
    count = 0
    lowest = math.inf
    for index, frame in enumerate(frames):
        position, angles = frame["position"], frame["angles"]
        if len(position) != 3 or len(angles) not in {2, 3}:
            raise ValueError("Expected position[3] and angles[2 or 3]")
        for value in (*position, *angles):
            number(value)
        lowest = min(lowest, position[2])
        if limit is not None and position[2] < limit:
            raise ValueError(f"Camera sample {index} at Z={position[2]:.3f} is below floor clearance Z={limit:.3f}")
        count += 1
    if count == 0:
        raise ValueError("No sampled camera frames")
    return {"status": "unchecked" if limit is None else "passed", "floor_z": floor,
            "clearance": clearance, "minimum_camera_z": lowest, "samples": count}


def compile_camera(spec: dict, demo: Path | None, start: int, end: int, fps: int,
                   *, subject: SubjectTrack | None = None) -> dict:
    if (not isinstance(start, int) or not isinstance(end, int) or end <= start
            or isinstance(fps, bool) or not isinstance(fps, int) or not 1 <= fps <= 1000):
        raise ValueError("Invalid camera timeline")
    duration = (end - start) / 64
    if duration > 300:
        raise ValueError("A camera shot must be at most 300 seconds; split longer scenes")
    if isinstance(spec, dict) and "type" not in spec and "keys" in spec:
        from .live_track import compile_live_project
        return compile_live_project(spec, start, end, fps)
    if not isinstance(spec, dict) or spec.get("version") != 1:
        raise ValueError("Camera plan requires version: 1")
    kind = spec.get("type", "path")
    if kind not in {"static", "path", "orbit", "follow", "keyframes"}:
        raise ValueError("Camera type must be static, path, orbit, follow, or keyframes")
    fov = spec.get("fov")
    if "fov" in spec and (isinstance(fov, bool) or not isinstance(fov, (int, float))
                          or not math.isfinite(fov) or not 30 <= fov <= 140):
        raise ValueError("fov must be a finite camera FOV between 30 and 140 degrees")
    lens = None
    if "fov_keys" in spec:
        from .pose_track import FovKeys
        lens = FovKeys(spec,duration)
    if "roll" in spec:
        raise ValueError("Supply roll in rotation_keys angles or XYZW quaternion")
    ease_name = spec.get("easing", "smootherstep")
    easing(0, ease_name)
    look = spec.get("look_at")
    if look is not None and not isinstance(look, dict):
        raise ValueError("look_at must be {point:[x,y,z]} or {player:identity, offset:[x,y,z]}")
    offset = vector(look.get("offset", [0, 0, 48])) if look else (0, 0, 0)
    smooth = look.get("smooth_seconds", .12) if look else 0
    if isinstance(smooth, bool) or not isinstance(smooth, (int, float)) or not math.isfinite(smooth) or not 0 <= smooth <= 1:
        raise ValueError("smooth_seconds must be between 0 and 1")
    if look:
        if ("point" in look) == ("player" in look):
            raise ValueError("look_at needs exactly one of point or player")
        if "point" in look:
            target_point = vector(look["point"], "look_at point")
        else:
            if subject is None:
                if demo is None:
                    raise ValueError("A demo is required to track a player")
                subject = load_subject(demo, str(look["player"]), start, end)
    if kind in {"follow", "orbit"} and not look:
        raise ValueError(f"{kind} requires a look_at target")
    if kind == "follow" and (not look or "player" not in look):
        raise ValueError("follow requires a player target")
    curve = None
    angles = None
    if kind == "keyframes":
        from .pose_track import PoseKeys
        key_track = PoseKeys(spec, duration)
    elif kind == "path":
        points = spec.get("points")
        if not isinstance(points, list) or not all(isinstance(p, dict) for p in points):
            raise ValueError("A path needs points:[{position:[x,y,z],angles:[pitch,yaw]}]")
        curve = PathCurve([p.get("position") for p in points], spec.get("curve", "catmull_rom"), spec.get("constant_speed", True))
        if not look:
            angles = [vector(p.get("angles"), "angles", 2) for p in points]
            for i in range(1, len(angles)):
                angles[i] = (angles[i][0], nearest_angle(angles[i][1], angles[i - 1][1]))
    elif kind == "static":
        fixed = vector(spec.get("position"), "static position")
        if not look:
            fixed_angles = vector(spec.get("angles"), "angles", 2)
    elif kind == "follow":
        follow_offset = vector(spec.get("offset"), "follow offset")
        if distance(follow_offset, (0, 0, 0)) < 1:
            raise ValueError("Follow offset must leave space between camera and actor")
    elif kind == "orbit":
        radius = spec.get("radius", 180)
        height = spec.get("height", 50)
        start_angle, end_angle = spec.get("start_angle", 0), spec.get("end_angle", 45)
        vector([radius, height, start_angle, end_angle], "orbit parameters", 4)
        if radius <= 0:
            raise ValueError("Orbit radius must be positive")
    count = math.ceil(duration * fps)
    frames = []
    prior_yaw = None
    for index in range(count):
        tick = start + index * 64 / fps
        u = easing(index / (duration * fps), ease_name)
        target = None
        if look:
            target = add(subject.smooth_sample(tick, smooth), offset) if "player" in look else target_point
        if kind == "keyframes":
            position, rotation = key_track.pose(index / fps)
        elif kind == "static":
            position = fixed
        elif kind == "path":
            position = curve.sample(u)
        elif kind == "follow":
            position = add(target, follow_offset)
        else:
            angle = math.radians(start_angle + (end_angle - start_angle) * u)
            position = add(target, (radius * math.cos(angle), radius * math.sin(angle), height))
        if kind == "keyframes":
            pitch, yaw, roll = rotation
        elif target is not None:
            pitch, yaw = look_angles(position, target)
        elif kind == "static":
            pitch, yaw = fixed_angles
        else:
            at = curve.parameter(u) * (len(angles) - 1)
            segment = min(int(at), len(angles) - 2)
            pitch, yaw = blend(angles[segment], angles[segment + 1], at - segment)
        if prior_yaw is not None:
            yaw = nearest_angle(yaw, prior_yaw)
        prior_yaw = yaw
        pitch_limit = 90 if kind == "keyframes" else 89.9
        if not -pitch_limit <= pitch <= pitch_limit:
            raise ValueError(f"Camera pitch is outside +/-{pitch_limit} degrees")
        frame = {"position": list(position), "angles": [pitch, yaw, roll] if kind == "keyframes" else [pitch, yaw]}
        if lens is not None or fov is not None:
            frame["fov"] = lens.sample(index/fps) if lens is not None else float(fov)
        frames.append(frame)
    steps = [distance(a["position"], b["position"]) for a, b in zip(frames, frames[1:])]
    floor_check = validate_floor(frames, spec)
    return {"frames": frames, "summary": {
        "type": kind, "frames": count, "fps": fps, "duration_seconds": duration,
        "curve": spec.get("curve", "catmull_rom") if curve else None, "easing": ease_name,
        "position_interpolation": key_track.position_interpolation if kind == "keyframes" else None,
        "position_easing": key_track.position_easing if kind == "keyframes" else None,
        "rotation_easing": key_track.rotation_easing if kind == "keyframes" else None,
        "path_length_units": curve.length if curve else sum(steps),
        "max_speed_units_per_second": max(steps, default=0) * fps,
        "tracked_player": subject.name if subject and look and "player" in look else None,
        "orientation_mode": "independent_keys" if kind == "keyframes" else "look_at" if look else "authored",
        "floor_check": floor_check,
        "collision_checked": False, "fov_supported": True, "fov_override": fov,
        "fov_animation": {"start": frames[0]["fov"], "end": frames[-1]["fov"],
                          "minimum": min(f["fov"] for f in frames),
                          "maximum": max(f["fov"] for f in frames)} if lens is not None else None,
        "point_of_interest": list(key_track.interest_point) if kind == "keyframes" and key_track.interest_point else None,
        "roll_supported": kind == "keyframes",
    }}
