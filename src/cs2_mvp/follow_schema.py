"""Dependency-free input schema for recorded-player camera drafts."""

_VECTOR = {"type": "array", "items": {"type": "number", "minimum": -1000000, "maximum": 1000000},
           "minItems": 3, "maxItems": 3}
FOLLOW_OPTIONS_SCHEMA = {
    "type": "object", "additionalProperties": False, "properties": {
        "mode": {"type": "string", "enum": ["look_at", "attach", "lock_on"],
                 "description": "look_at follows the player origin and aims at it plus look_at_offset; attach follows recorded view rotation; lock_on rotates a fixed camera_position."},
        "offset": {**_VECTOR, "description": "Camera offset in Source units. Local axes are forward/left/up; relative to player origin, never a measured bone or eye."},
        "offset_space": {"type": "string", "enum": ["yaw", "view", "world"],
                         "description": "Rotate camera offset with player yaw, full pitch/yaw view, or keep world axes."},
        "look_at_offset": {**_VECTOR, "description": "World XYZ offset from recorded player origin; not a measured eye/head position."},
        "camera_position": {**_VECTOR, "description": "Fixed world XYZ, required only for lock_on."},
        "angle_offset": {**_VECTOR, "description": "Local rotation trim [pitch,yaw,roll] in degrees, attach mode only."},
        "fov": {"type": "number", "minimum": 30, "maximum": 140},
        "position_half_life": {"type": "number", "minimum": 0, "maximum": 1,
                               "description": "Position filter half-life in simulation seconds; zero is exact. Smoothing can cross geometry and still requires collision validation."},
        "rotation_half_life": {"type": "number", "minimum": 0, "maximum": 1,
                               "description": "Quaternion rotation filter half-life in simulation seconds; zero is exact."},
        "on_loss": {"type": "string", "enum": ["error", "truncate"],
                    "description": "Reject by default; truncate ends before the first dead/missing/invalid/teleported sample. No reconnect or interpolation through the gap."},
        "name": {"type": "string", "minLength": 1, "maxLength": 256},
    },
}

