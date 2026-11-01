# Linux color lens adapter

`lens_effects.h` owns an original Vulkan full-frame pass for radial barrel / pincushion distortion, vignette and chromatic separation. It is independent of `overlay_blur.h`; that existing pass only softens the backdrop of editor panels. The lens pass runs before both backdrop and editor rendering. F8 hides the editor while leaving the lens active.

The adapter does not infer depth from RGB. True depth of field, scene normals, geometry-aware relighting and depth masks require the actual scene depth attachment, its projection, viewport depth range and ownership/layout provenance. A present-only swapchain interception does not supply those. The adapter reports `depth_available=false`.

## State and capability bridge

The controller publishes root state `lens`:

```json
{"enabled":true,"distortion":-0.35,"vignette":0.2,"chromatic":1.5}
```

`Settings::Valid()` requires finite values: distortion `[-0.75,0.75]` (positive barrel, negative pincushion), vignette `[0,1]`, chromatic `[0,8]` output pixels. Missing, invalid, disabled and all-zero state performs no lens GPU work. The layer also requires `connected=true` and `demo=true`, so stored project effects do not alter the main menu after a demo ends. Zero effects preserve source pixels exactly. Parameters are snapshotted per present, with no animated randomness or dependence on wall time.

The optional controller export `void cs2_live_lens_status_json(const char*)` receives changed capability state only. The callback must be thread-safe and must not read game memory or perform blocking work. Payload: `available`, `backend="vulkan-color"`, `depth_available=false`, `reason`. Availability is revoked during swapchain cleanup or overlay failure; the next valid swapchain republishes it. The local editor also receives `lens_runtime` in its state.

SDR R8/B8 SRGB and UNORM swapchains with SRGB nonlinear color space are supported. The layer negotiates `TRANSFER_SRC`, requires existing `COLOR_ATTACHMENT`, and excludes protected and multilayer targets. HDR/unsupported formats fail closed. Each swap image has an independent sampled copy; swapchain views remain caller-owned. The pass never waits for GPU idle or submits work itself.

## Capture contract

The pass modifies the swap image before the diagnostic GPU capture, so that capture sees the same effects as the preview. Engine `startmovie` captures TGA frames before presentation; it does **not** capture this pass. Batch video/PNG export must apply the following matching transform to its captured frame sequence once, before encoding. The UI should not imply export parity without that batch processing step.

For pixel `(x,y)` in an image of width `W`, height `H`:

1. `uv=((x+0.5)/W,(y+0.5)/H)`, `c=2*uv-1`, `a=W/H`.
2. `r=c*(a,1)`, `q=dot(r,r)/(a*a+1)`.
3. `k=0.4*distortion`, `s=(1+k*q)/(1+max(k,0))`, `p=c*s*0.5+0.5`.
4. `d=r/max(length(r),0.000001)*(1/W,1/H)*chromatic*q`.
5. Sample red at `p+d`, green at `p`, blue at `p-d`.
6. Multiply linear RGB by `1-vignette*smoothstep(0.2,1,q)`.
7. Encode to SRGB and round to output bytes. Alpha is opaque for an active pass.

Sampling uses clamped texel centers `p*(W,H)-0.5` and explicit bilinear interpolation of SRGB-decoded texels, not interpolation of encoded bytes. The shader uses four `texelFetch` calls per sample to avoid device-specific fixed-point sampler weights. SRGB framebuffer attachments encode automatically; UNORM attachments use explicit SRGB encoding. This preserves brightness and keeps Linux live and offline render implementations comparable within one byte on the tested devices.

The bounded radial mapping remains monotonic across the frame and fits the corners to avoid black borders. This is photographic radial distortion, not a calibrated equidistant or equisolid fisheye camera projection.

## Build and verification

`build_lens_shaders.py` compiles the original GLSL with `glslangValidator`, validates SPIR-V with `spirv-val`, and writes `lens_effects_spv.h`. `--check` verifies that the committed header matches the source. The production build fingerprints all local headers, including the generated SPIR-V, and requires no shader compiler at runtime.

`tests/run_lens_effects_test.py` exercises actual Vulkan dispatch on the selected ICD. It compares 120 rendered cases to an independent CPU reference, covering four SDR formats, landscape/portrait/square resize, two target images, disabled and neutral bit-exact output, independent effects, combined effects, invalid inputs, canceled command buffers, format rejection and teardown. Optional `--validation` enables `VK_LAYER_KHRONOS_validation`; `--artifacts PATH` saves visual PPM examples. This does not substitute for a live CS2 scene or full encoder verification.
