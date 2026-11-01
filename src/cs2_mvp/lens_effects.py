"""Color-only lens finishing shared with the native Vulkan preview.

The pixel-center mapping and linear-light interpolation match
``native/live/lens_effects.frag``. There is deliberately no inferred depth or
depth-of-field control. Source frames are never modified in place.
"""
from __future__ import annotations

import math
from pathlib import Path
import struct


LENS_DEFAULTS = {"enabled": False, "distortion": 0.0, "vignette": 0.0, "chromatic": 0.0}
LENS_ALGORITHM = "color-lens-srgb-v1"
_LIMITS = {"distortion": (-.75, .75), "vignette": (0.0, 1.0), "chromatic": (0.0, 8.0)}
_MAX_PIXELS = 3840 * 2160  # Same total-pixel budget as capture resolution validation.


def validate_lens(settings: object) -> dict:
    """Validate a JSON lens object before any capture/session side effects."""
    if not isinstance(settings, dict):
        raise ValueError("lens must be an object")
    unknown = settings.keys() - LENS_DEFAULTS.keys()
    if unknown:
        raise ValueError("Unknown lens field(s): " + ", ".join(sorted(map(str, unknown))))
    result = dict(LENS_DEFAULTS)
    if "enabled" in settings:
        if not isinstance(settings["enabled"], bool):
            raise ValueError("lens.enabled must be a boolean")
        result["enabled"] = settings["enabled"]
    for name, (lower, upper) in _LIMITS.items():
        if name not in settings:
            continue
        value = settings[name]
        if (isinstance(value, bool) or not isinstance(value, (int, float)) or
                not lower <= value <= upper or not math.isfinite(value)):
            raise ValueError(f"lens.{name} must be a finite number between {lower:g} and {upper:g}")
        result[name] = float(value)
    return result


def lens_active(settings: object) -> bool:
    value = validate_lens(settings)
    return value["enabled"] and any(value[key] != 0 for key in _LIMITS)


def apply_lens(rgb, settings: object, *, check_cancel=None, tile_rows: int = 64):
    """Return processed uint8 H×W×3 RGB; disabled/neutral returns the same array.

    Only the input/output frame and bounded scratch tiles are held in memory.
    Sampling decodes sRGB *before* bilinear interpolation, clamps texels at the
    image edge, and encodes sRGB after the vignette. GPU fixture parity permits
    two 8-bit steps for shader float/UNORM rounding against this reference.
    """
    import numpy as np

    value = validate_lens(settings)
    if (not isinstance(rgb, np.ndarray) or rgb.dtype != np.uint8 or rgb.ndim != 3 or
            rgb.shape[2] != 3 or not all(rgb.shape[:2])):
        raise ValueError("Lens input must be a nonempty uint8 RGB image")
    if isinstance(tile_rows, bool) or not isinstance(tile_rows, int) or not 1 <= tile_rows <= 1024:
        raise ValueError("Lens tile_rows must be an integer between 1 and 1024")
    check_cancel = check_cancel or (lambda: None)
    check_cancel()
    if not lens_active(value):
        return rgb
    height, width = rgb.shape[:2]
    result = np.empty_like(rgb)
    steps = np.arange(256, dtype=np.float64) / 255
    decoded = np.where(steps <= .04045, steps / 12.92, ((steps + .055) / 1.055) ** 2.4)
    x_centered = 2 * (np.arange(width, dtype=np.float64) + .5) / width - 1
    aspect = width / height
    radial_x = x_centered[None, :] * aspect
    coefficient = .4 * value["distortion"]
    # Keep temporary coordinate/sampling arrays below 65536 pixels even at 8K.
    rows = min(tile_rows, max(1, 65536 // width))

    def sample(u, v, channel):
        px, py = u * width - .5, v * height - .5
        bx, by = np.floor(px), np.floor(py)
        fx, fy = px - bx, py - by
        x0 = np.clip(bx, 0, width - 1).astype(np.intp)
        x1 = np.clip(bx + 1, 0, width - 1).astype(np.intp)
        y0 = np.clip(by, 0, height - 1).astype(np.intp)
        y1 = np.clip(by + 1, 0, height - 1).astype(np.intp)
        a, b = decoded[rgb[y0, x0, channel]], decoded[rgb[y0, x1, channel]]
        c, d = decoded[rgb[y1, x0, channel]], decoded[rgb[y1, x1, channel]]
        return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy

    for first in range(0, height, rows):
        check_cancel()
        last = min(first + rows, height)
        y_centered = 2 * (np.arange(first, last, dtype=np.float64)[:, None] + .5) / height - 1
        length_squared = radial_x * radial_x + y_centered * y_centered
        radius_squared = length_squared / (aspect * aspect + 1)
        scale = (1 + coefficient * radius_squared) / (1 + max(coefficient, 0))
        u = x_centered[None, :] * scale * .5 + .5
        v = y_centered * scale * .5 + .5
        separation = value["chromatic"] * radius_squared / np.maximum(np.sqrt(length_squared), .000001)
        shift_x, shift_y = radial_x * separation / width, y_centered * separation / height
        smooth = np.clip((radius_squared - .2) / .8, 0, 1)
        dim = 1 - value["vignette"] * smooth * smooth * (3 - 2 * smooth)
        for channel, sign in enumerate((1, 0, -1)):
            linear = sample(u + sign * shift_x, v + sign * shift_y, channel) * dim
            linear = np.clip(linear, 0, 1)
            encoded = np.where(linear <= .0031308, 12.92 * linear, 1.055 * linear ** (1 / 2.4) - .055)
            result[first:last, :, channel] = np.clip(np.floor(encoded * 255 + .5), 0, 255).astype(np.uint8)
    check_cancel()
    return result


def read_tga(path: Path, size: tuple[int, int], *, check_cancel=None):
    """Decode engine true-color TGA (raw/RLE, 24/32-bit, every image origin).

    The requested render size is checked before allocating a frame. Color maps,
    interleaved/grayscale/16-bit images are rejected rather than reinterpreted.
    The optional TGA footer is not image data and may follow the decoded pixels.
    """
    import numpy as np

    check_cancel = check_cancel or (lambda: None)
    check_cancel()
    with Path(path).open("rb") as source:
        def read_exact(count):
            data = source.read(count)
            if len(data) != count:
                raise ValueError(f"Truncated TGA frame: {path.name}")
            return data

        header = read_exact(18)
        width, height = struct.unpack_from("<HH", header, 12)
        depth, descriptor = header[16:18]
        if (width, height) != size or not width or not height:
            raise ValueError(f"TGA frame resolution {width}x{height} differs from requested {size[0]}x{size[1]}")
        if width > 8192 or height > 8192 or width * height > _MAX_PIXELS:
            raise ValueError("TGA frame exceeds the capture total-pixel budget")
        if header[1] != 0 or header[2] not in (2, 10) or depth not in (24, 32) or descriptor & 0xC0:
            raise ValueError("Lens export requires non-interleaved true-color 24/32-bit TGA frames (raw or RLE)")
        read_exact(header[0])
        channels, count = depth // 8, width * height
        pixels = np.empty((count, 3), dtype=np.uint8)
        if header[2] == 2:
            for row in range(height):
                check_cancel()
                bgr = np.frombuffer(read_exact(width * channels), dtype=np.uint8).reshape(width, channels)
                pixels[row * width:(row + 1) * width] = bgr[:, 2::-1]
        else:
            offset = 0
            while offset < count:
                check_cancel()
                packet = read_exact(1)[0]
                length = (packet & 0x7F) + 1
                if offset + length > count:
                    raise ValueError(f"TGA RLE packet exceeds frame size: {path.name}")
                if packet & 0x80:
                    bgr = np.frombuffer(read_exact(channels), dtype=np.uint8)
                    pixels[offset:offset + length] = bgr[2::-1]
                else:
                    bgr = np.frombuffer(read_exact(length * channels), dtype=np.uint8).reshape(length, channels)
                    pixels[offset:offset + length] = bgr[:, 2::-1]
                offset += length
        pixels = pixels.reshape(height, width, 3)
        if not descriptor & 0x20:
            pixels = pixels[::-1]
        if descriptor & 0x10:
            pixels = pixels[:, ::-1]
        return pixels


def write_tga(path: Path, rgb, *, check_cancel=None):
    """Write a new, top-left 24-bit TGA without modifying its source pixels."""
    import numpy as np

    if (not isinstance(rgb, np.ndarray) or rgb.dtype != np.uint8 or rgb.ndim != 3 or rgb.shape[2] != 3 or
            not 0 < rgb.shape[0] <= 65535 or not 0 < rgb.shape[1] <= 65535):
        raise ValueError("TGA output must be a nonempty uint8 RGB image within 65535x65535")
    check_cancel = check_cancel or (lambda: None)
    height, width = rgb.shape[:2]
    header = bytearray(18)
    header[2] = 2
    header[12:16] = struct.pack("<HH", width, height)
    header[16:18] = bytes((24, 32))
    with Path(path).open("xb") as target:
        target.write(header)
        for row in rgb:
            check_cancel()
            target.write(row[:, ::-1].tobytes())

