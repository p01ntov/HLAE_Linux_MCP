# CS2 Camera Studio 0.1.15

This candidate continues the 0.1.14 workflow work with Linux adapters. It retains
the export profiles, timeline group editing, project browser and previous fixes.

## Implemented

- Exact client build profiles for Steam build 26334290, game version 1.41.9.4.
  Unsupported binaries fail the build/profile checks instead of using guessed
  offsets. Capture verification recognizes the newly validated client build.
- A Vulkan color lens pass: radial distortion, vignette and chromatic separation.
  It runs only in a connected local demo, before the editor UI. Project settings
  support undo/redo and are applied to exported frames with the matching CPU
  algorithm. No depth reconstruction is implied.
- Follow drafts baked from recorded per-tick player motion, with exact SteamID64
  selection. Look-at, attach and lock-on modes produce ordinary editable keys.
  Missing, dead or teleported targets cause an explicit error or truncation;
  the solver never switches to another player. Desktop, CLI and MCP workflows
  are available. Desktop library saving verifies the recording identity and
  SHA256 again.
- Passive depth attachment provenance behind `CS2_CAMERA_DEPTH_PROBE`. Tracking
  does not add image usage flags, barriers, copies, waits or rendering work.
  The feature stays off by default and reports depth unavailable.
- Runtime writable state in the user's data directory, shared by editor and
  capture launchers. Explicit pressure-vessel overrides are preserved. This
  addresses the observed default-runtime NTFS symlink failure without changing
  Steam files or global launch settings.

## Review fixes

- Reject malformed lens project settings before changing native project state.
- Recognize the validated 1.41.9.4 client in final motion-log verification.
- Scale lens processing time allowance with frame count and pixel count, retain
  an inactivity limit and cancellation, and preserve raw capture on failure.
- Accumulate storage requirements per filesystem rather than comparing their
  combined allocation to the smaller of two disks.
- Include lens intermediates in the export dialog estimate.
- Reject recording replacement even when file size and mtime are preserved;
  saving runs on a worker and supports cooperative cancellation.

## Verification scope

The verification covers Python behavior, real offscreen Qt interactions, native
controller/ImGui suites, synthetic Vulkan dispatch/capture/lens tests, sanitizer
checks for passive depth tracking, a real recorded-demo Follow bake/save and
the packaged AppImage. Release assets include machine-readable test results.
No test in this continuation takes over a running ordinary CS2 session.

The new effects have not completed a fresh end-to-end live-game capture with
audio. A running stock CS2 process confirms ordinary launch restoration, not
the behavior of the new editor/capture libraries inside a demo. Builds for other
Linux distributions and graphics drivers remain unverified.

## Explicitly unavailable

Depth pixels, main-camera depth identification, depth of field based on captured
depth, normals, depth masks, geometry-aware relighting, particle editing and
runtime bone tracking are not implemented. Passive attachment records alone
cannot establish those capabilities. See [depth implementation gates](../native/live/depth_adapter.md)
and [lens implementation](../native/live/lens_adapter.md).

Follow output is an unverified camera draft. It does not establish free space,
camera collision clearance, visible targets or a safe interpolated path. The
existing geometry guard and visual review requirements still apply.

## Provenance

SourceMVM informed the feature comparison and workflow choices. These Linux
implementations do not bundle SourceMVM or Valve game binaries. Native plugin
builds pin CS Demo Manager and preserve its MIT notice; the AppImage also carries
the Qt, Python and native dependency notices described in
[packaging notices](../packaging/licenses/NOTICE.md).
