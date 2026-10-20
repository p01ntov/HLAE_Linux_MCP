"""Fit two world-space model bounds in an elevated, fixed perspective camera.

This solves the frustum inequalities directly with a fixed viewing direction.
It neither follows actors nor infers map collision or the engine's actual lens.
"""
from __future__ import annotations

from itertools import product
import math

from .camera import add, blend, look_angles, scale, subtract, vector


def number(value, name, low, high):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(value) or not low <= value <= high):
        raise ValueError(f"{name} must be finite and between {low} and {high}")
    return float(value)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def unit(value):
    length = math.sqrt(dot(value, value))
    if length < 1e-8:
        raise ValueError("Camera basis is degenerate")
    return scale(value, 1 / length)


def model_bounds(value, name):
    if not isinstance(value, dict):
        raise ValueError(f"{name} needs world-space min/max bounds")
    low = vector(value.get("min"), f"{name}.min")
    high = vector(value.get("max"), f"{name}.max")
    if any(a >= b for a, b in zip(low, high)) or any(abs(v) > 1e6 for v in (*low, *high)):
        raise ValueError(f"{name} bounds must have positive volume within native coordinate bounds")
    return {"center": blend(low, high, .5), "top": high[2],
            "corners": list(product(*zip(low, high)))}


def compose_pair(spec: dict) -> dict:
    """Return a standard static camera plus an independently checkable fit report.

    FOV is the actual horizontal projection FOV, not a 4:3 reference FOV. The
    caller must establish the engine lens separately. Margin is a fraction of
    full image width/height on EACH edge (0.1 leaves the central 80 percent).
    """
    if not isinstance(spec, dict) or spec.get("version") != 1:
        raise ValueError("Composition requires version: 1")
    first = model_bounds(spec.get("first"), "first")
    second = model_bounds(spec.get("second"), "second")
    fov = number(spec.get("horizontal_fov", 90), "horizontal_fov", 5, 170)
    aspect = number(spec.get("aspect_ratio", 16 / 9), "aspect_ratio", .25, 4)
    margin = number(spec.get("margin", .1), "margin", 0, .4)
    elevation = number(spec.get("elevation", 25), "elevation", 1, 60)
    side = number(spec.get("side_angle", 35), "side_angle", -80, 80)
    if abs(side) < 1:
        raise ValueError("Use a side_angle of at least one degree to separate the models")
    clearance = number(spec.get("height_above_models", 48), "height_above_models", 1, 10000)
    near = number(spec.get("near_clip", 8), "near_clip", .01, 1000)
    behind = number(spec.get("behind_first", 32), "behind_first", 0, 10000)
    maximum = number(spec.get("max_distance", 20000), "max_distance", 1, 1e6)
    aim_weight = number(spec.get("aim_weight", .5), "aim_weight", 0, 1)
    delta = subtract(second["center"], first["center"])
    planar = math.hypot(delta[0], delta[1])
    if planar < 1e-5:
        raise ValueError("Subjects need distinct XY positions to establish the first-to-second direction")
    direction = (delta[0] / planar, delta[1] / planar, 0.)
    perpendicular = (-direction[1], direction[0], 0.)
    side, elevation = math.radians(side), math.radians(elevation)
    horizontal = add(scale(direction, math.cos(side)), scale(perpendicular, math.sin(side)))
    forward = (*scale(horizontal, math.cos(elevation))[:2], -math.sin(elevation))
    right = unit(cross(forward, (0, 0, 1)))
    up = unit(cross(right, forward))
    if dot(delta, forward) <= 0:
        raise ValueError("This elevation puts the second model nearer; lower elevation or swap subjects")
    target = blend(first["center"], second["center"], aim_weight)
    tan_h = math.tan(math.radians(fov) / 2)
    tan_v = tan_h / aspect
    safe = 1 - 2 * margin
    # With camera = target - forward*d, each corner's depth is d+dot(q,forward).
    # x and y are constant, so all frustum and height constraints are linear in d.
    required = [near, (max(first["top"], second["top"]) + clearance - target[2]) / -forward[2],
                (dot(subtract(target, first["center"]), direction) + behind) / dot(forward, direction)]
    for model in (first, second):
        for corner in model["corners"]:
            relative = subtract(corner, target)
            depth = dot(relative, forward)
            required.extend([near - depth, abs(dot(relative, right)) / (tan_h * safe) - depth,
                             abs(dot(relative, up)) / (tan_v * safe) - depth])
    distance = max(required) + 1e-5
    if distance > maximum:
        raise ValueError(f"Composition needs distance {distance:.3f}, above max_distance {maximum}")
    position = subtract(target, scale(forward, distance))
    if any(abs(v) > 1e6 for v in position):
        raise ValueError("Composed camera exceeds native coordinate bounds")
    projections = []
    for model in (first, second):
        points = []
        depths = []
        for corner in model["corners"]:
            relative = subtract(corner, position)
            depth = dot(relative, forward)
            depths.append(depth)
            x = dot(relative, right) / (depth * tan_h)
            y = dot(relative, up) / (depth * tan_v)
            points.append([(x + 1) / 2, (1 - y) / 2])
        low = [min(p[i] for p in points) for i in range(2)]
        high = [max(p[i] for p in points) for i in range(2)]
        if min(depths) < near - 1e-6 or any(v < margin - 1e-6 for v in low) or any(v > 1 - margin + 1e-6 for v in high):
            raise ValueError("Composed camera failed its frustum validation")
        projections.append({"corners_uv": points, "min_uv": low, "max_uv": high,
                            "height_fraction": high[1] - low[1], "minimum_depth": min(depths)})
    camera = {"version": 1, "type": "static", "position": list(position),
              "angles": list(look_angles(position, target))}
    report = {"type": "two_subjects", "verification": "geometry_only",
              "horizontal_fov": fov, "aspect_ratio": aspect, "margin_per_edge": margin,
              "height_above_highest_model": position[2] - max(first["top"], second["top"]),
              "distance_to_aim": distance, "look_at": list(target), "subjects": projections,
              "first_to_second_screen_height": projections[0]["height_fraction"] / projections[1]["height_fraction"],
              "collision_checked": False, "lens_applied": False}
    camera["composition"] = report
    return camera
