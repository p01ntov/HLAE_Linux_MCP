"""Sample portable in-game camera projects with the native editor's math.

Keep these operations in step with cs2_live::ReadProject/Sample in
native/live/live_controller.h. This track deliberately does not use PoseKeys:
the live editor has different endpoint tangents and per-segment Bezier timing.
"""
from __future__ import annotations

from bisect import bisect_right
from dataclasses import dataclass
import math

from .camera import vector


def _number(value, label, low, high):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(value) or not low <= value <= high):
        raise ValueError(f"Live project {label} must be finite and within {low}..{high}")
    return float(value)


def _rotation(angles):
    pitch, yaw, roll = (v * math.pi / 360 for v in angles)
    cp, sp, cy, sy, cr, sr = (math.cos(pitch), math.sin(pitch), math.cos(yaw),
                              math.sin(yaw), math.cos(roll), math.sin(roll))
    # WXYZ, Source Rz(yaw) * Ry(pitch) * Rx(roll).
    return (cr*cp*cy+sr*sp*sy, sr*cp*cy-cr*sp*sy,
            cr*sp*cy+sr*cp*sy, cr*cp*sy-sr*sp*cy)


def _slerp(a, b, t):
    dot = sum(x*y for x, y in zip(a, b))
    if dot < 0:
        b, dot = tuple(-v for v in b), -dot
    left, right = 1-t, t
    if dot < .9995:
        angle = math.acos(max(-1., min(1., dot)))
        denominator = math.sin(angle)
        left, right = math.sin((1-t)*angle)/denominator, math.sin(t*angle)/denominator
    result = tuple(left*x+right*y for x, y in zip(a, b))
    norm = math.sqrt(sum(v*v for v in result))
    return tuple(v/norm for v in result)


def _euler(q):
    w, x, y, z = q
    sine_pitch = max(-1., min(1., 2*(w*y-z*x)))
    if abs(sine_pitch) > 1-1e-12:
        return (math.copysign(90., sine_pitch),
                math.degrees(math.atan2(2*(w*z-x*y), 1-2*(x*x+z*z))), 0.)
    return (math.degrees(math.asin(sine_pitch)),
            math.degrees(math.atan2(2*(w*z+x*y), 1-2*(y*y+z*z))),
            math.degrees(math.atan2(2*(w*x+y*z), 1-2*(x*x+y*y))))


def _bezier_coordinate(t, a, b):
    v = 1-t
    return 3*v*v*t*a + 3*v*t*t*b + t*t*t


def bezier_ease(progress, curve):
    """Invert x(t) before evaluating y(t), with the native 36-step bisection."""
    if progress <= 0:
        return 0.
    if progress >= 1:
        return 1.
    low, high = 0., 1.
    for _ in range(36):
        middle = (low+high)*.5
        if _bezier_coordinate(middle, curve[0], curve[2]) < progress:
            low = middle
        else:
            high = middle
    return _bezier_coordinate((low+high)*.5, curve[1], curve[3])


@dataclass(frozen=True)
class _Key:
    identity: str
    time: float
    position: tuple[float, ...]
    angles: tuple[float, ...]
    fov: float
    easing: tuple[float, ...]

    def pose(self):
        return {"position": list(self.position), "angles": list(self.angles), "fov": self.fov}


class LiveTrack:
    """Validated live project; times are seconds relative to project.start_tick."""

    def __init__(self, project):
        if not isinstance(project, dict):
            raise ValueError("Live project must be an object")
        self.name = project.get("name", "Untitled cinematic")
        if not isinstance(self.name, str) or len(self.name.encode("utf-8")) > 256:
            raise ValueError("Live project name must contain at most 256 UTF-8 bytes")
        self.start_tick = project.get("start_tick", 0)
        if (isinstance(self.start_tick, bool) or not isinstance(self.start_tick, int)
                or not 0 <= self.start_tick <= 2_000_000_000):
            raise ValueError("Live project start_tick must be an integer within 0..2000000000")
        self.duration = _number(project.get("duration", 5), "duration", .01, 86400)
        self.interpolation = project.get("interpolation", "smooth")
        if not isinstance(self.interpolation, str) or self.interpolation not in {"linear", "smooth"}:
            raise ValueError("Live project interpolation must be linear or smooth")
        values = project.get("keys")
        if not isinstance(values, list) or not 1 <= len(values) <= 4096:
            raise ValueError("Live project requires 1..4096 camera keys before rendering")
        keys = []
        identities = set()
        for value in values:
            if not isinstance(value, dict):
                raise ValueError("Each live camera key must be an object")
            identity = value.get("id")
            if not isinstance(identity, str) or not 1 <= len(identity.encode("utf-8")) <= 128:
                raise ValueError("Live camera key id must contain 1..128 UTF-8 bytes")
            if identity in identities:
                raise ValueError("Live project has duplicate key id")
            identities.add(identity)
            time = _number(value.get("time"), "key time", 0, self.duration)
            position = vector(value.get("position"), "Live camera key position")
            angles = vector(value.get("angles"), "Live camera key angles")
            if any(abs(v) > 1_000_000 for v in (*position, *angles)) or abs(angles[0]) > 90:
                raise ValueError("Live camera key coordinates exceed native bounds")
            fov = _number(value.get("fov", 90), "key FOV", 1, 179)
            if not 30 <= fov <= 140:
                raise ValueError("Live project capture requires key FOV within 30..140 degrees; "
                                 "the in-game editor's wider 1..179 range is not supported by capture ABI 3")
            curve = vector(value.get("easing", [0, 0, 1, 1]), "Live camera key easing", 4)
            if any(not 0 <= v <= 1 for v in curve):
                raise ValueError("Live camera key easing coordinates must be within 0..1")
            keys.append(_Key(identity, time, position, angles, fov, curve))
        self.keys = sorted(keys, key=lambda key: key.time)
        if any(b.time-a.time < .000001 for a, b in zip(self.keys, self.keys[1:])):
            raise ValueError("Live camera keys need distinct times at least 0.000001 seconds apart")
        self.times = [key.time for key in self.keys]

    def sample(self, time):
        if isinstance(time, bool) or not isinstance(time, (int, float)) or not math.isfinite(time):
            raise ValueError("Live camera sample time must be finite seconds")
        keys = self.keys
        if time <= keys[0].time:
            return keys[0].pose()
        if time >= keys[-1].time:
            return keys[-1].pose()
        right = bisect_right(self.times, time)
        left = right-1
        a, b = keys[left], keys[right]
        span = b.time-a.time
        u = bezier_ease((time-a.time)/span, a.easing)
        if self.interpolation == "linear" or len(keys) < 3:
            position = [x+u*(y-x) for x, y in zip(a.position, b.position)]
        else:
            previous, after = keys[left-1 if left else left], keys[min(right+1, len(keys)-1)]
            u2, u3 = u*u, u*u*u
            position = []
            for axis in range(3):
                tangent_a = (b.position[axis]-previous.position[axis])/(b.time-previous.time)*span
                tangent_b = (after.position[axis]-a.position[axis])/(after.time-a.time)*span
                position.append((2*u3-3*u2+1)*a.position[axis] + (u3-2*u2+u)*tangent_a
                                + (-2*u3+3*u2)*b.position[axis] + (u3-u2)*tangent_b)
        angles = list(_euler(_slerp(_rotation(a.angles), _rotation(b.angles), u)))
        for axis in (1, 2):
            reference = a.angles[axis]+u*math.remainder(b.angles[axis]-a.angles[axis], 360)
            angles[axis] = reference+math.remainder(angles[axis]-reference, 360)
        return {"position": position, "angles": angles, "fov": a.fov+u*(b.fov-a.fov)}


def compile_live_project(spec, start, end, fps):
    from .camera import distance, validate_floor

    track = LiveTrack(spec)
    duration = (end-start)/64
    offset = (start-track.start_tick)/64
    frames = [track.sample(offset+index/fps) for index in range(math.ceil(duration*fps))]
    for index, frame in enumerate(frames):
        if any(abs(v) > 1_000_000 for v in (*frame["position"], *frame["angles"])):
            raise ValueError(f"Live camera sample {index} exceeds native coordinate bounds; reduce curve overshoot")
    steps = [distance(a["position"], b["position"]) for a, b in zip(frames, frames[1:])]
    return {"frames": frames, "summary": {
        "type": "live_project", "frames": len(frames), "fps": fps, "duration_seconds": duration,
        "live_project_interpolation": track.interpolation, "project_name": track.name,
        "project_start_tick": track.start_tick, "project_duration_seconds": track.duration,
        "project_sample_start_seconds": offset,
        "curve": None, "easing": "outgoing_cubic_bezier",
        "position_interpolation": "hermite" if track.interpolation == "smooth" and len(track.keys) >= 3 else "linear",
        "position_easing": "outgoing_cubic_bezier", "rotation_easing": "outgoing_cubic_bezier",
        "path_length_units": sum(steps), "max_speed_units_per_second": max(steps, default=0)*fps,
        "tracked_player": None, "orientation_mode": "quaternion_slerp",
        "floor_check": validate_floor(frames, spec), "collision_checked": False,
        "fov_supported": True, "fov_override": None,
        "fov_animation": {"start": frames[0]["fov"], "end": frames[-1]["fov"],
                          "minimum": min(frame["fov"] for frame in frames),
                          "maximum": max(frame["fov"] for frame in frames)},
        "point_of_interest": None, "roll_supported": True,
    }}
