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

## Lossless frames for After Effects

Choose **PNG sequence (lossless)** in the export window, or use
`render --format png --capture-fps 1000 --fps 60 --mute --output /absolute/new-folder`.
The destination is a new folder, containing consecutive `frame_00000000.png`
images, `sequence.json` timing metadata and `AE_IMPORT.txt` instructions.
PNG output retains every captured sample: unlike a normal-duration MP4 export,
it never reduces 1000 captured samples to 60 images. The output FPS supplies the
suggested interpretation rate; the original capture rate remains in metadata.

The export dialog defaults to **2560 × 1440 · QHD** and also offers Full HD, 4K
and HD presets. **Custom** accepts explicit width and height, including portrait
and ultrawide aspect ratios. Both dimensions must be even integers: width
320–8192 and height 240–8192, with at most 8,294,400 pixels in total. These are
capture limits, not a promise that every desktop/game configuration supports
every aspect ratio. Finalization rejects a capture whose actual dimensions do
not match the request; it does not silently upscale a smaller image.

For MP4, **CRF quality** defaults to 18; lower values produce higher quality and
usually larger files. Alternatively, select **Average bitrate** and enter
0.1–1000 Mbps. This targets the average video bitrate, excluding audio; the
measured result can vary. The two modes are mutually exclusive. The CLI accepts
`--crf 18` or `--video-bitrate-mbps 12.5`, along with `--width` and `--height`.
MP4 uses H.264 YUV 4:2:0, so even CRF 0 is not exact source RGB preservation.
Use PNG for that purpose: PNG hides the MP4 quality/bitrate controls and uses
lossless compression instead. Neither bitrate nor CRF changes native capture
FPS or creates new model animation samples.

Frames preserve the captured SDR RGB image at 8 bits per channel, at the requested
resolution. There is no JPEG compression, H.264 generation, YUV 4:2:0 subsampling,
upscaling or optical-flow frame synthesis in this path. Finalization compares
decoded RGB hashes against the original capture before publishing the folder.
This preserves the rendered image; it cannot reverse blur or antialiasing already
applied by the game. A 16-bit container would not add source precision to this
8-bit capture.

In After Effects, import the first PNG with **PNG Sequence** enabled. Under
**File > Interpret Footage > Main**, set **Assume This Frame Rate** to 60 for a
60 FPS edit, or 24 for a 24 FPS edit. One demo second sampled at 1000 FPS then
lasts 16.6667 or 41.6667 seconds respectively. Set the composition to the desired
delivery frame rate. You do not need a 1000 FPS composition to retain the sampled
frames for slow motion. See Adobe's
[image-sequence import and interpretation instructions](https://helpx.adobe.com/my_en/after-effects/desktop/import-files/import-still-images/preparing-importing-still-images.html).
The AE interpretation field accepts at most 999 FPS; native capture still permits
1000 samples per demo second. The PNG exporter keeps these separate and defaults
the interpretation rate to 60 FPS, following Adobe's
[high-frame-rate guidance](https://helpx.adobe.com/dk/after-effects/desktop/work-with-footage-items/import-and-interpret-footage-items/importing-interpreting-footage-items.html#support-for-high-frame-rate-footage).

PNG compression trades encoding time against file size without changing image
quality. It is smaller than uncompressed RGB/TGA for ordinary game images, but
1000 full-resolution images per demo second remain substantial, and grain can
compress poorly. The exporter reports the actual sequence size; it does not
promise a fixed compression ratio. The implementation uses FFmpeg's
[PNG compression and prediction options](https://ffmpeg.org/ffmpeg-codecs.html#png).
Audio, when requested, is a separate WAV at normal game speed; it is not
automatically stretched to match an arbitrary frame-rate interpretation.

The 2026-10-24 real-game validation captured ticks 111385–111417 in the
L201742 Nuke recording on client build `f00942ca6d0c486d11f988c53381c47a5be27143`:
500 images at 1280×720, 500 camera/view/FOV writes, no overflow, 0.5 demo seconds.
PNG finalization verified all 500 decoded RGB hashes. The PNG images totaled
905,760,079 bytes versus 1,843,209,000 captured TGA bytes (49.14%). This scene's
1000 FPS output therefore costs about 1.81 GB per demo second, even at 720p.
This is a measured example, not a promised compression ratio.

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
Unavailable controls fail the capture.

Saved agents and team colors are sent through private per-job cosmetic actions.
The launcher checks native proof/restoration support, stages the builtin agent
catalog in the private job directory, and enables it only for that capture.
Actual application proof must precede recording, and finalization requires zero
retained overrides after the final flush before encoding.

Version 0.1.2 removes weapon paints, glove replacements and global appearance
packs from the live/capture modules. Projects containing retired fields fail
explicitly before launch. Historical glove export evidence from 0.1.1 remains
in local diagnostics, but does not describe a feature available in this build.

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

A second fixed-camera check on 2026-10-24 used the current client and a close
actor in the L201742 demo, with 1 ms frame spacing. In the visible first 100 ms,
all 100 neighboring actor-image pairs changed above twice the static background
noise after spatial low-pass filtering. Mean actor/background differences were
1.650 versus 0.158–0.205 levels out of 255; at 6 ms the actor difference grew to
7.889 and at 16 ms to 14.926. No obvious held images or long freeze appeared in
that visible sample. The actor becomes occluded by roughly 250 ms, so the rest
of the clip does not test its motion. Translation and skeletal animation are
combined in these image measurements: they do not prove 1000 independent bone
poses or guarantee smoothness for every recording. Consequently automatic export
metadata still reports `model_animation_verified: false`.
