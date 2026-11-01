"""Independent world-space position keys and XYZW quaternion rotation keys."""
from __future__ import annotations

from bisect import bisect_right
import math

from .camera import blend, easing, look_angles, vector


def quaternion(value):
    q = vector(value, "quaternion XYZW", 4)
    norm = math.hypot(*q)
    if norm < 1e-9:
        raise ValueError("Quaternion must have nonzero norm")
    return tuple(v / norm for v in q)


def from_angles(value):
    if not isinstance(value, (list, tuple)) or len(value) not in {2, 3}:
        raise ValueError("Rotation angles require pitch/yaw[/roll]")
    pitch, yaw, roll = vector([*value, 0] if len(value) == 2 else value, "angles")
    p, y, r = (math.radians(v) / 2 for v in (pitch, yaw, roll))
    cp, sp, cy, sy, cr, sr = math.cos(p), math.sin(p), math.cos(y), math.sin(y), math.cos(r), math.sin(r)
    return quaternion((sr*cp*cy-cr*sp*sy, cr*sp*cy+sr*cp*sy,
                       cr*cp*sy-sr*sp*cy, cr*cp*cy+sr*sp*sy))


def to_angles(q):
    x, y, z, w = quaternion(q)
    pitch = math.asin(max(-1., min(1., 2*(w*y-z*x))))
    if abs(math.cos(pitch)) < 1e-7:
        # At a pole yaw and roll are coupled. Choose roll=0 while preserving
        # the rotation matrix; the world orientation remains continuous.
        yaw = math.atan2(-2*(x*y-w*z), 1-2*(x*x+z*z))
        roll = 0.
    else:
        yaw = math.atan2(2*(w*z+x*y), 1-2*(y*y+z*z))
        roll = math.atan2(2*(w*x+y*z), 1-2*(x*x+y*y))
    return tuple(math.degrees(v) for v in (pitch, yaw, roll))


def slerp(first, second, t):
    a, b = quaternion(first), quaternion(second)
    dot = sum(x*y for x,y in zip(a,b))
    if dot < 0:
        b, dot = tuple(-v for v in b), -dot
    dot = max(-1., min(1., dot))
    if dot > .9995:
        return quaternion(blend(a,b,t))
    angle = math.acos(dot)
    left, right = math.sin((1-t)*angle) / math.sin(angle), math.sin(t*angle) / math.sin(angle)
    return quaternion(tuple(x*left+y*right for x,y in zip(a,b)))


class PoseKeys:
    def __init__(self, spec, duration):
        if any(k in spec for k in ["player", "look_at", "follow", "offset"]):
            raise ValueError("World keyframes cannot reference an actor or look_at target")
        if ("position" in spec) == ("position_keys" in spec):
            raise ValueError("Supply exactly one of position or position_keys")
        hold_last = spec.get("hold_last", False)
        if not isinstance(hold_last, bool):
            raise ValueError("hold_last must be boolean")
        if "position" in spec:
            self.positions = [(0., vector(spec["position"], "position"))]
        else:
            self.positions = self.keys(spec.get("position_keys"), duration, "position", hold_last)
        if ("rotation_keys" in spec) == ("point_of_interest" in spec):
            raise ValueError("Supply exactly one of rotation_keys or a fixed world point_of_interest")
        self.interest_point = None
        if "point_of_interest" in spec:
            self.interest_point = vector(spec["point_of_interest"], "point_of_interest")
            # The point authors sparse rotation keys; it is not an actor target
            # or a per-frame tracking controller. Rotation keeps its own easing.
            rotations = [{"time": time, "angles": [*look_angles(position, self.interest_point), 0.]}
                         for time, position in self.positions]
            self.rotations = self.keys(rotations, duration, "rotation", hold_last)
        else:
            self.rotations = self.keys(spec.get("rotation_keys"), duration, "rotation", hold_last)
        self.position_easing = spec.get("position_easing", "linear")
        self.rotation_easing = spec.get("rotation_easing", "smootherstep")
        self.position_interpolation = spec.get("position_interpolation", "linear")
        if self.position_interpolation not in {"linear", "hermite"}:
            raise ValueError("position_interpolation must be linear or hermite")
        if self.position_interpolation == "hermite" and self.position_easing != "linear":
            raise ValueError("Hermite motion uses linear key time; its endpoint tangents provide the easing")
        easing(0, self.position_easing)
        easing(0, self.rotation_easing)

    @staticmethod
    def keys(values, duration, kind, hold_last=False):
        if not isinstance(values, list) or not 1 <= len(values) <= 64:
            raise ValueError(f"{kind}_keys requires 1..64 keys")
        keys = []
        for value in values:
            if not isinstance(value, dict):
                raise ValueError("Each key needs time and a pose value")
            time = value.get("time")
            if isinstance(time, bool) or not isinstance(time, (int,float)) or not math.isfinite(time) or not 0 <= time <= 300:
                raise ValueError("Key time must be finite seconds between 0 and 300")
            if keys and time <= keys[-1][0]:
                raise ValueError("Key times must strictly increase")
            if kind == "position":
                pose = vector(value.get("position"), "position key")
            elif kind == "fov":
                pose = value.get("fov")
                if isinstance(pose, bool) or not isinstance(pose, (int,float)) or not math.isfinite(pose) or not 30 <= pose <= 140:
                    raise ValueError("FOV keys must contain finite values between 30 and 140")
                pose = float(pose)
            else:
                if ("quaternion" in value) == ("angles" in value):
                    raise ValueError("A rotation key requires exactly one of quaternion XYZW or angles")
                pose = quaternion(value["quaternion"]) if "quaternion" in value else from_angles(value["angles"])
            keys.append((float(time), pose))
        if keys[0][0] != 0 or (len(keys) > 1 and keys[-1][0] < duration and not hold_last):
            raise ValueError("Keys must start at time 0 and cover the shot; a single key holds its value")
        return keys

    @staticmethod
    def sample(keys, time, curve, rotation=False):
        if len(keys) == 1:
            return keys[0][1]
        times = [k[0] for k in keys]
        i = min(max(1,bisect_right(times,time)), len(keys)-1)
        first, second = keys[i-1], keys[i]
        t = easing((time-first[0])/(second[0]-first[0]), curve)
        return slerp(first[1],second[1],t) if rotation else blend(first[1],second[1],t)

    def pose(self, time):
        return (self.smooth_position(time) if self.position_interpolation == "hermite"
                else self.sample(self.positions,time,self.position_easing),
                to_angles(self.sample(self.rotations,time,self.rotation_easing,True)))

    def smooth_position(self, time):
        """C1 world-space travel through timed keys, with stationary endpoints.

        Interior velocities use neighboring keys in seconds, so unequal key
        intervals do not introduce a velocity jump. This is authored motion,
        not actor tracking. Curved segments can overshoot their anchors; the
        normal sampled floor check and rendered collision review still apply.
        """
        keys = self.positions
        if len(keys) == 1 or time <= keys[0][0]:
            return keys[0][1]
        if time >= keys[-1][0]:
            return keys[-1][1]
        index = bisect_right([key[0] for key in keys], time) - 1
        t0, p0 = keys[index]
        t1, p1 = keys[index + 1]
        duration = t1 - t0
        u = (time - t0) / duration

        def velocity(i):
            if i == 0 or i == len(keys) - 1:
                return (0., 0., 0.)
            before, after = keys[i - 1], keys[i + 1]
            return tuple((b - a) / (after[0] - before[0])
                         for a, b in zip(before[1], after[1]))

        m0, m1 = velocity(index), velocity(index + 1)
        return tuple((2*u**3 - 3*u**2 + 1)*a + (u**3 - 2*u**2 + u)*duration*va
                     + (-2*u**3 + 3*u**2)*b + (u**3 - u**2)*duration*vb
                     for a, b, va, vb in zip(p0, p1, m0, m1))


class FovKeys:
    """An independent lens track; it does not alter position or orientation."""
    def __init__(self, spec, duration):
        if "fov" in spec:
            raise ValueError("Supply either fov or fov_keys")
        hold = spec.get("hold_last",False)
        if not isinstance(hold,bool):
            raise ValueError("hold_last must be boolean")
        self.keys = PoseKeys.keys(spec.get("fov_keys"),duration,"fov",hold)
        self.curve = spec.get("fov_easing","smootherstep")
        easing(0,self.curve)
        self.times = [key[0] for key in self.keys]

    def sample(self,time):
        if len(self.keys)==1:
            return self.keys[0][1]
        i=min(max(1,bisect_right(self.times,time)),len(self.keys)-1)
        a,b=self.keys[i-1:i+1]
        t=easing((time-a[0])/(b[0]-a[0]),self.curve)
        return a[1]+(b[1]-a[1])*t
