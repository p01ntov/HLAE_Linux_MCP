# Native capture up to 1000 samples per second

`render --fps` accepts 15–1000 encoded frames per second. A camera job now captures
at `max(120, output_fps)` by default, preserving the existing 120-sample camera
clock for lower output rates. `--capture-fps` can request a higher native capture
rate explicitly. It cannot be lower than the output FPS; the renderer does not
duplicate frames to advertise a higher rate. Camera capture requires at least 120.

Each capture uses the game's offline `host_framerate` clock. After seeking and one
demo second before recording, setup requests normal demo/host timescale, disables
tick-interval frame-time overrides, and sets the native frame rate. Finalization
requires an executed `host_framerate` setup in the plugin log, contiguous raw image
numbers, the expected raw image count, and matching native camera/view counts.
This verifies command execution and samples, not the engine's internal fractional
clock or model animation accuracy. The manifest records that distinction.

## Short verification on the current Nuke demo

The fixed camera is intentionally motionless so repeated model poses are visible.
The eight ticks around the Zeus hit cover 0.125 demo seconds and should produce
125 raw images. Run only after the existing live session/render has stopped:

```sh
.venv/bin/python -m cs2_mvp.cli render \
  '/mnt/ssd/SteamLibrary/steamapps/common/Counter-Strike Global Offensive/game/csgo/replays/match730_003849381187264970947_1107093056_187.dem' \
  --camera examples/high-fps-probe-camera.json --start 89688 --end 89696 \
  --fps 1000 --width 640 --height 360 --keep-raw --timeout 300 \
  --output outputs/high-fps-probe-1000.mp4
```

Output paths must be new. Inspect `capture_clock_validation`, `motion_validation`,
`raw_sequence`, and `media` in the returned manifest.

The first real Linux game test on 2026-10-20 completed in
`.runs/20261020T170824Z-123640ff/manifest.json`: 125 contiguous raw images,
125 applied camera samples and 125 camera/FOV view writes, with no overflow.
The executed `host_framerate 1000` command was confirmed. The resulting MP4 has
125 frames at 1000/1 FPS and lasts 0.125 seconds. This is offline sampling; it is
not a claim that the GPU renders 1000 frames per wall-clock second.

Visual inspection also caught a boundary defect that the matching counters alone
did not detect: image 124 reverted to the spectator camera while the movie flushed.
The native capture backend now retains its last applied view through that flush,
without advancing the track or adding to its sample counters.

The corrected real-game test,
`.runs/20261020T172459Z-bf376fff/manifest.json`, passed the same 125-frame camera
and clock checks. All 125 images, including final image 124, retain the intended
camera. Its stationary-background difference between images 123 and 124 is
3.430/255, within the ordinary grain/TAA range (median 3.434, maximum 3.456),
compared with 27.16 at the leaked boundary in the original run. A visual comparison
is saved as `outputs/high-fps-probe-fixed-frame-check.png`. The original test and
its derived artifacts remain as diagnostic evidence.

## Slow-motion conforming

The desktop export window provides both modes without terminal commands:
**Normal duration** resamples to video FPS; **Slow motion** retains every native
sample and interprets the sequence at video FPS, with muted audio. Its frame and
duration preview uses the same range/FPS calculations as the recorder.

Capture FPS and playback FPS are different. `--fps 60 --capture-fps 1000` encodes
normal-duration 60 FPS video; it resamples the 1000 native samples. `--keep-raw`
retains every original image, and the manifest includes its pattern and rate.

Add `--slow-motion` to keep all native samples in the encoded MP4 at video FPS.
For example, `--capture-fps 1000 --fps 60 --slow-motion` turns one demo second
into 16.6667 video seconds at 0.06× speed. This mode automatically mutes audio;
the original WAV has normal game timing. `--mute` also removes audio from a
normal-duration export. You can still import retained frames into an editor as
60 FPS for another conform.
High-FPS MP4 support varies between playback/editing applications, so keep the
image sequence for conforming.

`outputs/high-fps-probe-fixed-slow60.mp4` demonstrates this conform using all 125
raw images from the corrected test: 60 FPS, 2.083333 seconds, no audio, with the
intended camera retained through the final image.

## Render a project saved by the in-game editor

The `--camera` input also accepts the live editor's native JSON project directly:
`name`, `start_tick`, `duration`, `interpolation`, and timed `keys`. No conversion
to the older camera-plan format is needed. Optional export format/version metadata
is ignored. Native project sampling preserves outgoing Bezier curves, linear or
Hermite travel, shortest-arc quaternion rotation including roll, and animated FOV.
Its math is checked against 71 poses computed by the native editor itself.

For `examples/live-nuke-flyby.json`, the origin is tick 80091 and duration is
3.140625 seconds, or 201 ticks. With no explicit ticks, native projects use their
saved render mode; projects without `render_range` use the full composition:

```sh
./outputs/CS2-Camera-Studio-x86_64.AppImage render /absolute/path/match.dem \
  --camera examples/live-nuke-flyby.json --render-scope composition \
  --fps 1000 --keep-raw --output outputs/editor-flyby-1000.mp4
```

Use the same demo and a new output filename. The in-game Save button normally
writes `$XDG_DATA_HOME/cs2-camera/projects/last-project.json` (default
`~/.local/share/cs2-camera/projects/last-project.json`); MCP can save another
explicit path. Replace the example with that file. Omitting `--render-scope`
uses the mode saved in the project:

```json
{"render_range": {"mode": "region", "in": 0.5, "out": 2.5}}
```

`composition` renders from project time zero through `duration`; `region` uses
the saved In/Out seconds relative to `start_tick`. In/Out is a temporal selection,
not an image crop. `--render-scope composition|region` overrides the saved mode
without modifying the file. For example, render the region with:

```sh
./outputs/CS2-Camera-Studio-x86_64.AppImage render /absolute/path/match.dem \
  --camera /absolute/path/saved-project.json --render-scope region \
  --fps 60 --capture-fps 1000 --keep-raw --output outputs/editor-region.mp4
```

The recorder quantizes the selected In down and Out up to 64 Hz demo ticks:
`start_tick + floor(in * 64)` and `start_tick + ceil(out * 64)`. Full-composition
bounds use zero and `duration`. The returned plan reports the effective capture
boundaries so sub-tick rounding is visible. A range that would produce fewer than
two encoded frames is rejected before capture; lengthen it or increase `--fps`.

To select absolute demo ticks directly, supply both `--start` and `--end`;
this pair cannot be combined with `--render-scope`. The example composition can
also be rendered with `--start 80091 --end 80292`. Older camera-plan JSON and POV
capture still require the explicit pair. Each captured interval remains limited
to 300 seconds.

A selected region or explicit excerpt retains the whole original track and time
origin: a later interval samples its existing curves at the corresponding project
time. Keys are not trimmed or retimed, and interpolation still uses neighboring
keys outside the region. Outside the first/last key time, the camera holds that
endpoint as it does in the editor.

The live editor and MCP camera authoring permit FOV 1–179; native capture ABI 3
currently supports 30–140. `set_camera` changes the current held view; editing
an existing key's FOV requires `update_key` for that key.
A project containing a wider lens value fails with an explicit range error before
launching the game, so adjust its FOV keys for capture. Project coordinates and
sampled curve overshoot must also stay within the capture backend's bounds.

Native `project.scene` effects are passed through internal capture ABI 1 actions
after preroll setup. Finalization requires actual engine readback for every
requested value before `startmovie`; a JSON plan alone cannot satisfy this check.
Unavailable controls fail the capture. Model appearance uses a separate adapter
and remains blocked in offline capture while safe resource refresh is under
validation.

Saved agents, team colors, paints on currently owned matching weapons and gloves
are sent through private per-job cosmetic actions. The launcher checks the
capture plugin's proof/restoration support, stages its builtin catalog in the
private job directory, and enables cosmetics only for that capture process.
Before `startmovie`, gloves require exact item/finish readback, populated
world/HUD poses, completed material jobs and published world scene objects. HUD
visibility is required when its validated parent is enabled; a validated disabled
parent exempts only HUD visibility, with HUD pose/material checks still required.
Missing or pending evidence blocks recording. Finalization requires zero retained
overrides after the final flush; missing or mismatched evidence fails the job
before encoding.

The real Qt glove export on 2026-10-21 completed in
`.runs/20261021T115557Z-3fdf2a60/manifest.json`: AURA's builtin glove 5030 with
paint 10018, wear 0.06 and seed 1, ticks 11779–11787, 125 native samples at
1000 FPS, and 125 encoded frames at 60/1 FPS lasting 2.083333 seconds. The blue
gloves were visible in `outputs/gloves-ui-export-1000-slow60.mp4`. Native
item/attribute, pose and render-publication checks passed; restoration reported
zero retained overrides at tick 11851 after the final flush. This proves the
tested case, not every catalogue entry, and does not change the global
`render_verified_1p`/`render_verified_3p` capability flags.

The real-game scene/range/slow-motion check on 2026-10-20 completed in
`.runs/20261020T201816Z-859d44bb/manifest.json`: selected ticks 89692–89700,
125 native frames, 125 encoded frames at 60/1 FPS and 2.083333 video seconds.
The plugin confirmed requested scene values before recording and restored its
environment on exit. The diagnostic output is `outputs/region-dof-slow60.mp4`.

## Model interpolation evidence and limits

The earlier interpolation investigation found the `cl_interp_ratio` cvar name at
ELF file offset `0xb0694e` in the previously validated Linux client, GNU build ID
`d5bacd2f5df4a3a68fa8bee8692337993548e14a` (Steam build 25966348). This is a
historical offset, not an offset for the current F1 build
`f1e04e3616921a3bb25940eea9b4b6bfd7ccada5` (26087496). Setup requests `cl_interp_ratio 1`
after the demo loads, leaving one second of preroll. The setting follows
[HLAE's Source 2 fix](https://github.com/advancedfx/advancedfx/wiki/Source2%3Amirv_fix),
whose current implementation changes a zero ratio to one during demo playback.

HLAE removed its separate animation patch in
[2.187.5](https://github.com/advancedfx/advancedfx/releases/tag/v2.187.5) because
Valve had fixed third-person demo animation smoothness. The older CS:GO
`mirv_fix playerAnimState` recipe must not be copied into CS2.

[HLAE's Source 2 recorder](https://github.com/advancedfx/advancedfx/blob/main/AfxHookSource2/RenderSystemDX11Hooks.cpp)
also uses `host_framerate` for offline capture.
[Its camera clock](https://github.com/advancedfx/advancedfx/blob/main/AfxHookSource2/MirvTime.cpp)
uses demo tick plus an engine interpolation fraction. The live prototype currently
uses integer demo ticks for synchronized preview; reading a validated Linux
fractional clock is a separate improvement. Windows offsets are not portable.

Rendering between recorded ticks can sample the game's interpolation and skeletal
animation more densely. It cannot recover an unrecorded authoritative action or
turn a 64-tick demo into a 1000-tick simulation. Smooth model motion must be checked
on a fixed-camera clip; moving the camera alone can hide stepped model poses.

The corrected test's body crops are in
`outputs/high-fps-probe-fixed-model-contact-sheet.png`; numeric observations are in
`outputs/high-fps-probe-fixed-model-metrics.json`. The analysis uses only event-free
images 0–44 for local optical flow, avoiding the later muzzle flash/electrical
effect. It applies spatial noise reduction, with no temporal smoothing, and
compares body features with the stationary background.

At 6 ms separation, median arms/weapon and distant-player displacement was
0.160/0.184 pixels, above background residual scatter of 0.095 pixels. At 16 ms,
those displacements increased to 0.439/0.507 pixels. That supports visible motion
between 64 Hz demo ticks. At 1 ms, body displacements of 0.041–0.053 pixels were
below background noise of 0.083 pixels. The nearer legs also had small motion,
so this 640×360 clip cannot reliably count repeated skeletal poses
or establish 1000 distinct bone updates per second. Grain and temporal antialiasing
make different full-frame hashes insufficient proof. The capture clock is tested;
complete elimination of model-animation stutter remains unverified.
