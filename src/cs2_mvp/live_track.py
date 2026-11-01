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
            or not low <= value <= high or not math.isfinite(value)):
        raise ValueError(f"Live project {label} must be finite and within {low}..{high}")
    return float(value)


def normalize_render_range(value, duration):
    """Validate saved temporal In/Out even when full composition is selected."""
    if not isinstance(value, dict):
        raise ValueError("Live project render_range must be an object")
    unknown = set(value) - {"mode", "in", "out"}
    if unknown:
        raise ValueError(f"Unknown live project render_range field: {sorted(unknown)[0]}")
    mode = value.get("mode", "composition")
    if not isinstance(mode, str) or mode not in {"composition", "region"}:
        raise ValueError("Live project render_range mode must be composition or region")
    begin = _number(value.get("in", 0), "render In", 0, duration)
    end = _number(value.get("out", duration), "render Out", 0, duration)
    if end <= begin:
        raise ValueError("Live project render Out must be greater than render In")
    return {"mode": mode, "in": begin, "out": end}


def select_render_range(project, start=None, end=None, scope=None):
    """Resolve a native project into absolute demo ticks without trimming keys."""
    track = LiveTrack(project)
    track.validate_capture_fov()
    if (start is None) != (end is None):
        raise ValueError("Supply both --start and --end, or omit both for a live project")
    if scope is not None and scope not in {"composition", "region"}:
        raise ValueError("--render-scope must be composition or region")
    if start is not None:
        if scope is not None:
            raise ValueError("--render-scope cannot be combined with explicit --start/--end ticks")
        if any(isinstance(value, bool) or not isinstance(value, int) for value in (start, end)):
            raise ValueError("Explicit demo bounds must be integer ticks")
        requested_in, requested_out = (start-track.start_tick)/64, (end-track.start_tick)/64
        chosen, source, quantization = "explicit", "cli_ticks", "explicit_ticks"
    else:
        chosen = scope or track.render_range["mode"]
        requested_in = 0.0 if chosen == "composition" else track.render_range["in"]
        requested_out = track.duration if chosen == "composition" else track.render_range["out"]
        start = track.start_tick + math.floor(requested_in*64)
        end = track.start_tick + math.ceil(requested_out*64)
        source = "cli_scope" if scope else "saved_project" if "render_range" in project else "project_default"
        quantization = "floor_in_ceil_out"
    metadata = {"scope": chosen, "source": source, "project_start_tick": track.start_tick,
                "tickrate": 64, "quantization": quantization,
                "requested": {"in": requested_in, "out": requested_out},
                "effective": {"in": (start-track.start_tick)/64, "out": (end-track.start_tick)/64,
                              "start_tick": start, "end_tick": end}}
    return start, end, metadata


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


def _cubic_value(a, b, c, d, u):
    v = 1-u
    return v*v*v*a + 3*v*v*u*b + 3*v*u*u*c + u*u*u*d


def _cubic_extrema(controls):
    a, b, c, d = controls
    quadratic, linear, constant = -a + 3*b - 3*c + d, 2*(a - 2*b + c), b - a
    roots = []
    if quadratic == 0:
        if linear != 0:
            roots.append(-constant / linear)
    else:
        discriminant = linear*linear - 4*quadratic*constant
        if discriminant >= 0:
            q = -.5*(linear + math.copysign(math.sqrt(discriminant), linear))
            if q == 0:
                roots.append(-linear / (2*quadratic))
            else:
                roots.extend((q / quadratic, constant / q))
    values = [a, d] + [_cubic_value(a, b, c, d, u) for u in roots if 0 < u < 1]
    return min(values), max(values)


@dataclass(frozen=True)
class _Key:
    identity: str
    time: float
    position: tuple[float, ...]
    angles: tuple[float, ...]
    fov: float
    easing: tuple[float, ...]
    position_in: tuple[float, ...] | None = None
    position_out: tuple[float, ...] | None = None
    rotation_in: tuple[float, ...] | None = None
    rotation_out: tuple[float, ...] | None = None
    fov_in: float | None = None
    fov_out: float | None = None
    position_easing: tuple[float, ...] | None = None
    rotation_easing: tuple[float, ...] | None = None
    fov_easing: tuple[float, ...] | None = None

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
        self.render_range = normalize_render_range(project.get("render_range", {}), self.duration)
        from .lens_effects import validate_lens
        self.lens = validate_lens(project.get("lens", {}))
        self.interpolation = project.get("interpolation", "smooth")
        if not isinstance(self.interpolation, str) or self.interpolation not in {"linear", "smooth", "bezier"}:
            raise ValueError("Live project interpolation must be linear, smooth or bezier")
        self.rotation_interpolation = project.get("rotation_interpolation", "slerp")
        self.fov_interpolation = project.get("fov_interpolation", "linear")
        if not isinstance(self.rotation_interpolation, str) or self.rotation_interpolation not in {"slerp", "bezier"}:
            raise ValueError("Live project rotation_interpolation must be slerp or bezier")
        if not isinstance(self.fov_interpolation, str) or self.fov_interpolation not in {"linear", "bezier"}:
            raise ValueError("Live project fov_interpolation must be linear or bezier")
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
            curve = vector(value.get("easing", [0, 0, 1, 1]), "Live camera key easing", 4)
            if any(not 0 <= v <= 1 for v in curve):
                raise ValueError("Live camera key easing coordinates must be within 0..1")
            options = {}
            for name in ("position_in", "position_out", "rotation_in", "rotation_out"):
                item = value.get(name)
                if item is not None:
                    item = vector(item, "Live camera key " + name)
                    if any(abs(x) > 1_000_000 for x in item):
                        raise ValueError("Live camera relative handle exceeds +/-1000000")
                options[name] = item
            for name in ("fov_in", "fov_out"):
                item = value.get(name)
                options[name] = None if item is None else _number(item, name, -1_000_000, 1_000_000)
            for name in ("position_easing", "rotation_easing", "fov_easing"):
                item = value.get(name)
                if item is not None:
                    item = vector(item, "Live camera key " + name, 4)
                    if any(not 0 <= x <= 1 for x in item):
                        raise ValueError("Live camera channel easing coordinates must be within 0..1")
                options[name] = item
            keys.append(_Key(identity, time, position, angles, fov, curve, **options))
        self.keys = sorted(keys, key=lambda key: key.time)
        if any(b.time-a.time < .000001 for a, b in zip(self.keys, self.keys[1:])):
            raise ValueError("Live camera keys need distinct times at least 0.000001 seconds apart")
        self.times = [key.time for key in self.keys]
        self.position_values = tuple(key.position for key in self.keys)
        self.fov_values = tuple((key.fov,) for key in self.keys)
        self.rotation_values = []
        for key in self.keys:
            value = list(key.angles)
            if self.rotation_values:
                value = [a + math.remainder(b - a, 360) for a, b in zip(self.rotation_values[-1], value)]
            self.rotation_values.append(tuple(value))

    def _automatic_controls(self, left, values, linear=False):
        if isinstance(left, bool) or not isinstance(left, int) or not 0 <= left < len(self.keys) - 1:
            raise ValueError("Curve controls require an existing outgoing segment")
        right = left + 1
        previous, after = max(0, left - 1), min(right + 1, len(self.keys) - 1)
        a, b, p, n = values[left], values[right], values[previous], values[after]
        span = self.times[right] - self.times[left]
        controls = [tuple(a), [], [], tuple(b)]
        for axis in range(len(a)):
            ta = b[axis] - a[axis] if linear else (b[axis] - p[axis]) / (self.times[right] - self.times[previous]) * span
            tb = b[axis] - a[axis] if linear else (n[axis] - a[axis]) / (self.times[after] - self.times[left]) * span
            controls[1].append(a[axis] + ta / 3)
            controls[2].append(b[axis] - tb / 3)
        return tuple(tuple(x) for x in controls)

    def position_controls(self, left):
        """Absolute controls of the exact native spatial locus for one segment."""
        linear = self.interpolation == "linear" or self.interpolation == "smooth" and len(self.keys) < 3
        controls = list(self._automatic_controls(left, self.position_values, linear))
        if self.interpolation == "bezier":
            for control, key, name in ((1, self.keys[left], "position_out"), (2, self.keys[left + 1], "position_in")):
                offset = getattr(key, name)
                if offset is not None:
                    anchor = controls[0 if control == 1 else 3]
                    controls[control] = tuple(a + b for a, b in zip(anchor, offset))
        return tuple(controls)

    def rotation_controls(self, left):
        """Unwrapped Euler controls; legacy SLERP itself is not an Euler cubic."""
        controls = list(self._automatic_controls(left, self.rotation_values, self.rotation_interpolation != "bezier"))
        if self.rotation_interpolation == "bezier":
            for control, key, name in ((1, self.keys[left], "rotation_out"), (2, self.keys[left + 1], "rotation_in")):
                offset = getattr(key, name)
                if offset is not None:
                    anchor = controls[0 if control == 1 else 3]
                    controls[control] = tuple(a + b for a, b in zip(anchor, offset))
        return tuple(controls)

    def fov_controls(self, left):
        controls = [x[0] for x in self._automatic_controls(left, self.fov_values, self.fov_interpolation != "bezier")]
        if self.fov_interpolation == "bezier":
            if self.keys[left].fov_out is not None:
                controls[1] = controls[0] + self.keys[left].fov_out
            if self.keys[left + 1].fov_in is not None:
                controls[2] = controls[3] + self.keys[left + 1].fov_in
        return tuple(controls)

    def validate_capture_fov(self):
        """Validate the entire project before Steam discovery or session stop.

        Bounded easing covers the same full value-cubic parameter interval.
        Exact derivative roots catch overshoot between otherwise valid keys.
        """
        if any(not 30 <= key.fov <= 140 for key in self.keys):
            raise ValueError("Live project capture requires key FOV within 30..140 degrees; the in-game editor's wider 1..179 range is not supported by capture ABI 3")
        low, high = min(key.fov for key in self.keys), max(key.fov for key in self.keys)
        if self.fov_interpolation == "bezier":
            for left in range(len(self.keys) - 1):
                minimum, maximum = _cubic_extrema(self.fov_controls(left))
                minimum, maximum = max(1., minimum), min(179., maximum)
                if minimum < 30 or maximum > 140:
                    raise ValueError(f"Live project FOV curve {self.keys[left].identity} -> {self.keys[left+1].identity} exceeds capture ABI 3's 30..140 degrees ({minimum:g}..{maximum:g}); adjust FOV handles")
                low, high = min(low, minimum), max(high, maximum)
        return {"minimum": low, "maximum": high}

    def sample(self, time):
        if isinstance(time, bool) or not isinstance(time, (int, float)) or not math.isfinite(time):
            raise ValueError("Live camera sample time must be finite seconds")
        keys = self.keys
        if time <= keys[0].time:
            return keys[0].pose()
        if time >= keys[-1].time:
            pose = keys[-1].pose()
            if self.rotation_interpolation == "bezier":
                pose["angles"] = list(self.rotation_values[-1])
            return pose
        right = bisect_right(self.times, time)
        left = right-1
        a, b = keys[left], keys[right]
        span = b.time-a.time
        progress = (time-a.time)/span
        u = bezier_ease(progress, a.position_easing or a.easing)
        if self.interpolation == "bezier":
            controls = self.position_controls(left)
            position = [_cubic_value(*(point[axis] for point in controls), u) for axis in range(3)]
        elif self.interpolation == "linear" or len(keys) < 3:
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
        rotation_u = bezier_ease(progress, a.rotation_easing or a.easing)
        fov_u = bezier_ease(progress, a.fov_easing or a.easing)
        if self.rotation_interpolation == "bezier":
            controls = self.rotation_controls(left)
            angles = [_cubic_value(*(point[axis] for point in controls), rotation_u) for axis in range(3)]
            angles[0] = min(90., max(-90., angles[0]))
        else:
            angles = list(_euler(_slerp(_rotation(a.angles), _rotation(b.angles), rotation_u)))
            for axis in (1, 2):
                reference = a.angles[axis]+rotation_u*math.remainder(b.angles[axis]-a.angles[axis], 360)
                angles[axis] = reference+math.remainder(angles[axis]-reference, 360)
        fov = min(179., max(1., _cubic_value(*self.fov_controls(left), fov_u))) if self.fov_interpolation == "bezier" else a.fov+fov_u*(b.fov-a.fov)
        return {"position": position, "angles": angles, "fov": fov}


def compile_live_project(spec, start, end, fps):
    from .camera import distance, validate_floor

    track = LiveTrack(spec)
    track.validate_capture_fov()
    duration = (end-start)/64
    offset = (start-track.start_tick)/64
    frames = [track.sample(offset+index/fps) for index in range(math.ceil(duration*fps))]
    for index, frame in enumerate(frames):
        if any(abs(v) > 1_000_000 for v in (*frame["position"], *frame["angles"])):
            raise ValueError(f"Live camera sample {index} exceeds native coordinate bounds; reduce curve overshoot")
        if not 30 <= frame["fov"] <= 140:
            raise ValueError(f"Live camera sample {index} FOV is outside capture ABI 3's 30..140 degrees; reduce the FOV curve handles")
    steps = [distance(a["position"], b["position"]) for a, b in zip(frames, frames[1:])]
    return {"frames": frames, "summary": {
        "type": "live_project", "frames": len(frames), "fps": fps, "duration_seconds": duration,
        "live_project_interpolation": track.interpolation, "project_name": track.name,
        "project_start_tick": track.start_tick, "project_duration_seconds": track.duration,
        "project_sample_start_seconds": offset,
        "curve": None, "easing": "outgoing_cubic_bezier",
        "position_interpolation": "bezier" if track.interpolation == "bezier" else "hermite" if track.interpolation == "smooth" and len(track.keys) >= 3 else "linear",
        "position_easing": "outgoing_cubic_bezier", "rotation_easing": "outgoing_cubic_bezier",
        "path_length_units": sum(steps), "max_speed_units_per_second": max(steps, default=0)*fps,
        "tracked_player": None, "orientation_mode": "unwrapped_euler_bezier" if track.rotation_interpolation == "bezier" else "quaternion_slerp",
        "rotation_interpolation": track.rotation_interpolation, "fov_interpolation": track.fov_interpolation,
        "floor_check": validate_floor(frames, spec), "collision_checked": False,
        "fov_supported": True, "fov_override": None,
        "fov_animation": {"start": frames[0]["fov"], "end": frames[-1]["fov"],
                          "minimum": min(frame["fov"] for frame in frames),
                          "maximum": max(frame["fov"] for frame in frames)},
        "point_of_interest": None, "roll_supported": True,
    }}
