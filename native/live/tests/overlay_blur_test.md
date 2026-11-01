# Offscreen Gaussian backdrop verification

This test creates a small private Vulkan device and renders synthetic images.
It does not open a window, launch CS2, load the live plugin, or change game files.
Build prerequisites are `g++`, Vulkan loader/development libraries, and the
Vulkan headers downloaded by `native/live/build_overlay.py` (system headers
also work).

Run once with the system's selected Vulkan driver:

```sh
python3 native/live/tests/run_overlay_blur_test.py
```

For a repeatable software-driver run, select an installed lavapipe ICD:

```sh
python3 native/live/tests/run_overlay_blur_test.py --icd /usr/share/vulkan/icd.d/lvp_icd.json
```

The runner compiles into a temporary directory and removes the executable when
finished. It preserves the caller's Vulkan configuration unless `--icd` is
specified; that option sets both `VK_DRIVER_FILES` and `VK_ICD_FILENAMES`.

The GPU result is copied to host memory and compared with an independent CPU
reference: sRGB decoding followed by a normalized Gaussian with sigma 2 pixels
and radius 6 in each axis. Eleven small cases exercise a uniform mid-gray,
a white impulse, asymmetric RGB gradients/checkerboards, alternating private
image indices, repeated rendering, exact encoded-space black dimming at 160/255
(including dark gradients), unsupported-format rejection, balanced
texture registration/removal, and destroy/reinitialize. The allowed absolute
linear RGB error is 0.0015 to account for hardware bilinear precision and the
two RGBA16F intermediates. The test checks actual pixels, not just successful
Vulkan calls.

Validated on llvmpipe and NVIDIA RTX 4060. This offscreen test does not exercise
WSI swapchain negotiation, presentation semaphore ownership, ImGui composition,
or resize handling; those remain integration checks in the overlay. Khronos GPU
validation layers are optional and are not bundled by this runner.
