# CS2 Camera Studio — Linux live prototype

Native Linux CS2 runs once with `-insecure`. A private client-side module owns
the demo camera; a Vulkan layer draws the editor in the game's swapchain. The
same project is edited by the in-game controls and the local MCP connector.
The original `.dem` is never rewritten. The existing batch-render CLI remains
separate.

## Start the application

Run `outputs/CS2-Camera-Studio-x86_64.AppImage`. The launcher detects installed
CS2 across registered Steam libraries and conventional mounted-disk folders,
then lists local recordings from `game/csgo/replays` and `game/csgo`. Select a
demo from **Downloaded demos** and click **Open Demo + Editor**. The selector
shows recorded map metadata, date and size; **Refresh** checks for new downloads,
and **Browse…** opens another local `.dem`. Incomplete or unsupported demo
headers are marked and cannot be selected from the library. Steam must already
be running. Leave **Fullscreen
Windowed** checked for SDL desktop fullscreen on the desktop's primary monitor.
The launcher may remain on another monitor; reopening the editor after export
also selects the current primary monitor. The normal **Windowed** mode remains
a 1280×720 window on that monitor. This
sets the compositor's fullscreen state so KDE panels stay behind the game;
it retains the desktop display mode. A desktop-sized borderless window alone
does not provide that state. See [SDL's desktop fullscreen contract](https://wiki.libsdl.org/SDL3/SDL_SetWindowFullscreen).
The application owns its session and stays open until the game exits or **Safe
exit** is selected.

The native module must be present when CS2 starts. An already open game is left
untouched; close it before starting a Camera Studio session. The application
uses the same render lock as the original source checkout, so it cannot launch
over a render in progress.

If FUSE is unavailable:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./CS2-Camera-Studio-x86_64.AppImage
```

Development launch:

```sh
python3 native/live/build_live.py
python3 native/live/build_overlay.py
.venv/bin/python -m cs2_mvp.live --demo /absolute/path/match.dem
```

Pass `--no-launcher` for a terminal session, `--windowed` for a normal 1280×720
window, or `--headless` to omit the overlay while retaining camera control.

### Safe exit and video settings

**Safe exit** is available in the fixed bottom bar and the launcher. It commits
the active numeric field, saves the current camera document to the project
library and a recovery checkpoint, then closes the owned game session. Closing
the launcher window uses the same save barrier. A failed save leaves the
session open with an error and a retry button; **Keep editing** dismisses the
error in the editor. Editing is locked while saving and closing. **Hide** only
hides the editor.

After the owned game stops and pending configuration writes settle, the
launcher restores the recorded per-game configuration, including the original
fullscreen/windowed mode and resolution in `cs2_video.txt`. Restoration uses
atomic file replacements and checks the resulting bytes before reporting
success. An ordinary CS2 session already running is never closed for recovery.
If another writer conflicts, the launcher reports the failure and retains
`original-config`, `changed-config` and `config-restore.json` in the session's
run directory. Repeating a completed restore cannot roll back later ordinary
game edits. If the exit snapshot could not be completed, a later automatic
retry is refused and the retained backups require manual inspection. New files
in the config directory are preserved because their ownership is unknown. A
concurrent independent writer cannot be excluded by the quiet interval alone.
This does not change global KDE panel or compositor settings.

## Editing a shot

The permanent bottom bar selects **Demo** or **Camera**. Demo shows the full
recording with absolute demo ticks, rounds and player kill/death markers.
Camera shows shot-relative time and selects **Timeline**, **Map** or **Curves**
inside one working area. Switching the source or tool does not seek, move keys
or resize the shot. The working area can be collapsed independently of the
inspector; Project, Settings and Export stay accessible.

The right inspector separates **Camera** (Live/Key pose, FOV and key actions),
**Shot** (start tick, duration, render range, saved shots and presets) and
**Scene** (HUD, appearance and supported engine effects). Its content scrolls
under fixed section tabs. Selecting a key does not jump the playhead or reopen
a deliberately hidden inspector. Use the ruler, previous/next key or Go to key
to move time.

**Camera → Map** opens the in-game XY plan with actual
interpolated camera motion, numbered keys and a blue live-camera handle. Drag a
handle to change XY while keeping its Z, lens, rotation and key time unchanged.
Release creates one undo action; Escape cancels. Use wheel zoom, middle-button
pan, Fit path / Fit map and the height-slice selector for multi-level maps.
Slices come from indexed physics, not the radar texture, and do not certify a
3D route. Static context is cached rather than rebuilt on every game frame.

Selected keys now expose incoming and outgoing **Bezier handles**. Dragging
these handles in the map changes their XY components while retaining Z.
**Auto** derives handles from neighboring key times, **Aligned** links opposing
handles with their time spans, and **Free** edits them independently. Ctrl-drag
temporarily releases the link. Handle edits enable Bezier spatial interpolation;
the existing Linear and Smooth modes retain their earlier behavior.

**Camera → Curves → Values** shows the actual sampled Position X/Y/Z, Rotation pitch/yaw/roll
and FOV graphs. Edit key values, key times and incoming/outgoing shape handles.
Yaw and roll use continuous angle branches in Bezier mode, so crossing 360°
does not force a spin. The **Timing** subview selects Position, Rotation
or FOV and controls progress through each outgoing segment. **Use shared**
restores the key's common timing curve. Shape and timing are separate controls.

**Smooth through key** resets the selected key's automatic shape handles and
sets linear timing on both adjacent segments for position, rotation and FOV.
It enables Bezier interpolation for the three tracks across the project, so
review the rest of the path too. This makes a continuous pass through the key;
it does not promise constant speed along the whole shot. One undo restores the
previous settings. Export uses the same curve sampler as the live editor.

### Composition timeline and editor preferences

**Camera → Timeline** opens the camera-composition workspace. Position, Rotation and
FOV lanes display the same compound keys: moving a key's time moves its whole
camera pose. **Demo** switches to the independent full-demo kill/death timeline. Use wheel
zoom at the pointer, middle-button pan, **Fit composition**, key dragging, the
playhead and In/Out handles. Shift bypasses snapping during a drag.

Change **Duration** or drag the shot's right edge to resize it. **Keep key
times** preserves every key and refuses to shorten past the last one. **Scale
keys and render region** proportionally retimes all keys and In/Out; spatial
handles, rotations, FOV values and timing-curve shapes are preserved. Each
completed resize is one undo action, and Escape cancels a drag.

**Settings** controls seconds/demo ticks/frames, frame-grid rate, tick/frame
snapping, resize behavior, interface scale, inspector width, timeline height,
panel opacity, blur, labels, tooltips and lane visibility. Frame-grid rate is
for editing; choose recording FPS separately in Export. The recorded demo
tick rate is not changed. Project files and MCP still store relative times in
seconds and `start_tick` as an absolute demo tick.

Preferences persist separately from project history in
`$XDG_CONFIG_HOME/cs2-camera/editor.json` (normally
`~/.config/cs2-camera/editor.json`). They do not mark a shot as edited or consume
its undo history. The launcher writes preferences outside the game's render
thread. Source, tool, inspector section and workspace visibility also persist
here. Old version-1 preference files gain these defaults automatically;
`compact_transport` remains readable for compatibility. The fixed camera
toolbar retains capture, preview, key navigation, range and sync controls.
An optional Settings switch can keep demo navigation above the camera tool.

Camera times and ticks are relative to `project.start_tick`; Demo ticks are
absolute. The selected source controls ruler navigation, Space and the local
K/Capture action. Camera capture uses its own playhead, including with Sync
off. Demo capture uses the actual demo tick relative to the shot's start;
the first key anchors a new shot at the current demo tick. Capturing after the
shot extends it explicitly; capturing before its start is rejected. I/O only
operate in Camera and reject an out-of-shot playhead. Changing the source
alone never seeks or retimes. Global F6/F9 retain camera capture/preview when
the editor is hidden.

The time beside Camera Play is editable: **Position** scrubs inside the shot,
and **Duration** extends or shortens it using the adjacent Keep/Scale choice.
The same duration control remains in the Shot inspector and timeline; the
toolbar does not require opening either one.

Demo has editable **Tick**, **In tick**, **Out tick** and **Limit playback**
beside its playback controls. Tick seeks to an absolute recording tick.
The optional playback range is session-only and independent of the camera
project, its render region and undo history. Demo Play from outside the range
seeks to In, then stops when the game reports Out or a later tick. Out is
exclusive; paused navigation outside the limits remains available. Disabling
the limit retains its bounds. Camera preview still uses the camera shot's
own composition or render range.

Pause the demo and click **Check 3D path** for a read-only check of the entire
camera curve. A detected obstruction is highlighted; editing invalidates the
result. Partial or missing geometry remains unverified. This diagnostic does
not snap the camera, guarantee framing or detect every dynamic object.

The MCP guard checks proposed spatial edits before applying them and uses a
native atomic revision/tick comparison to preserve concurrent user edits.
Known collisions and unresolved required coverage reject the request. Manual
in-game edits remain available for drafting. See [map indexing](map-geometry.md)
and the [agent guide](mcp-agent-guide.md).

The player dropdown in the bottom bar and **General → Player** lists current
demo players by nickname, team and SteamID suffix. Selecting one releases the
held camera and requests first-person observation; it keeps camera keys and the
HUD choice. Selection is confirmed from the engine's actual observer target.
The adjacent **...** button opens the project save/load and history menu.

1. Pause/seek the demo to the desired moment. **Shot Settings** sets the start
   tick and camera-track duration; key times are seconds relative to that tick.
2. Use **Free / hold camera** to release the authored camera. With the editor
   hidden, use the game's roaming spectator camera to compose a view.
3. **Capture keyframe** records the current world position, pitch/yaw/roll and
   FOV at the playhead. Capturing an occupied time updates that key.
4. Move the playhead and compose another camera. Select a key diamond to edit
   its pose, time and outgoing speed curve; drag diamonds to change timing. A left
   click selects a numeric value for replacement. Hold the middle mouse button
   and move horizontally to scrub it: four logical pixels per step of 1; Ctrl
   uses 0.1 for decimal fields and Shift uses 10. Release or Enter commits once;
   Escape cancels.
5. **Preview** evaluates the path repeatedly inside the same game process.
   **Sync demo** also seeks/plays the demo; disabling it lets the camera move
   through a frozen scene. Editing a synchronized preview pauses the demo.

The render range selects **time**, with In/Out measured in seconds relative to
the project's `start_tick`. **Composition** captures the entire project duration;
**Region** captures the saved In–Out interval. This selection does not crop the
image or remove keys. The project persists it as, for example:

```json
{"render_range": {"mode": "region", "in": 0.5, "out": 2.5}}
```

Both modes keep the whole camera track and its original time origin. A region
beginning between keys therefore starts at the same interpolated pose as preview.
The toolbar switches Composition/Region. **Shot Settings** edits In/Out or resets
them to the full duration; with the editor open, **I** sets In and **O** sets Out
at the playhead. Switching back to Composition preserves the saved region.

The current-camera FOV control patches only the live lens value, preserving its
position and orientation. It does not rewrite an existing key. A key's FOV is
edited separately after selecting that key; MCP uses `set_camera` for the current
view and `update_key` for a saved key. Both authoring controls accept 1–179;
offline capture ABI 3 still requires key FOV within 30–140.

The **Hold**, **Dolly forward** and **Orbit** presets produce ordinary editable
keys around the current camera. Smooth/linear controls the spatial path.
The Bezier graph is elapsed time → path progress for the outgoing segment;
its slope changes camera speed. It is not a collision solver or a spatial
Bezier handle editor. Quaternion SLERP interpolates orientation.

**Save / Ctrl+S** persists the current camera project to its own portable JSON
file. An imported composition gets a new file on its first save; subsequent
saves update that composition's file. **Save as new project / Ctrl+Shift+S**
creates a separate copy. **Load / Ctrl+O** opens the recent-project browser,
where each entry shows its camera keys, duration, start tick and recording.
Loading restores position, rotation, FOV, easing, interpolation, render range
and scene settings together; Undo restores the previous composition.

The launcher selects the shared folder with `--projects-dir` /
`CS2_CAMERA_PROJECTS_DIR`. A persistent choice can also be stored in
`$XDG_CONFIG_HOME/cs2-camera/storage.json` (default `~/.config/cs2-camera/storage.json`):
`{"version":1,"projects_directory":"/absolute/projects"}`.
The command-line option overrides the environment, which overrides this setting;
otherwise the launcher uses the XDG data projects folder.
Portable `.json` files placed in that folder are discovered when the browser
opens or Refresh is pressed. Separate `.meta` sidecars track recording identity
and saved/opened recency without changing the portable format. Projects tied
to another recording explain the mismatch and cannot load into the current
demo. Metadata-free portable projects show a warning and can be opened.
Scanning is bounded to 256 owned regular JSON files, 4 MiB each and 32 MiB in
total; symlinks and invalid files are skipped. There is no filesystem scan on
normal frames or `get_state`.

The old `last-project.json` quick-save remains a recovery copy, available from
**Load recovery copy** in the browser. MCP continues to save/load any explicit
project `.json` path. The active project is also checkpointed in its run folder.

`examples/live-nuke-flyby.json` is a portable project for the tested Nuke demo
around tick 80091. The older batch camera JSON format has independent position,
rotation and lens tracks. The recorder detects the live project’s keys and
samples the same Hermite, Bezier, quaternion and FOV math; it preserves roll.
For a saved native project, omit `--start`/`--end` to use its saved render mode.
`--render-scope composition` or `--render-scope region` overrides that mode for
one capture. An explicit `--start`/`--end` pair instead selects absolute demo
ticks and cannot be combined with `--render-scope`. Older camera plans and POV
capture still require both ticks. See the
[capture range examples](high-fps-capture.md#render-a-project-saved-by-the-in-game-editor).

The export window defaults to QHD 2560×1440, with Full HD, 4K and HD presets.
**Custom** exposes editable width and height; capture FPS and playback FPS are
separate numeric fields. Even dimensions within 320–8192 × 240–8192 are accepted,
up to 8,294,400 total pixels. Actual captured dimensions must match the request.
MP4 offers either CRF quality (default 18, range 0–51) or a target average video
bitrate (0.1–1000 Mbps). PNG keeps every native sample without RGB loss and hides
video bitrate settings. See [encoding and image-sequence details](high-fps-capture.md#lossless-frames-for-after-effects).

## Saved shot collections

Launch the selected recording with `--shot-library /absolute/shot-library.json`
(or `CS2_CAMERA_SHOT_LIBRARY`). Under **Camera keys → Saved shots**, choose a
shot and press **Load**. The editor pauses playback, imports the editable
project, and waits for its native acknowledgement before seeking the demo and
camera to the beginning. **Ctrl+Z** restores the previous composition.

Switching shots retains their edits in memory for this live session. **Ctrl+S**
persists the current composition as its own camera-project file; **Ctrl+O**
opens the recent-project browser. MCP can also use `save_project` with explicit
`.json` paths. The original shot library is never overwritten.

The version-1 library contains `demo` (the selected recording's absolute path)
and `shots`, each with `id`, `label` and a normal portable `project`. It is
limited to 64 shots and 1 MiB. A wrong recording or malformed collection
disables only the collection; normal camera editing remains available.

## Hotkeys

| Key | Action |
| --- | --- |
| F6 | Capture/update a key at the camera playhead, including with the UI hidden |
| F7 | Switch between free and held camera |
| F8 | Show/hide the editor |
| F9 | Play/pause camera preview using the current Sync demo setting |
| F10 | Toggle the separate, movable shortcuts panel while the editor is open |
| K | Capture/update a key using the selected Demo/Camera time context |
| Space | Play/pause the selected Demo or Camera source |
| I / O | Set shot-relative render In / Out in Camera |
| Delete | Delete the active key in the Camera inspector or key canvas |
| Ctrl+Z | Undo |
| Ctrl+Y / Ctrl+Shift+Z | Redo |
| Ctrl+S / Ctrl+O | Save current project / open recent-project browser |
| Ctrl+Shift+S | Save the current camera project as a separate copy |

Commands are suppressed during text/numeric entry. **Project → Hotkeys** or
F10 opens a separate movable panel with active bindings and mouse controls. Customize them in
`~/.config/cs2-camera/hotkeys.json` before the next session:

```json
{"capture":"F6","camera":"F7","editor":"F8","preview":"F9"}
```

Global bindings accept SDL key names with optional `Ctrl+`, `Shift+` and `Alt+`.
`CS2_CAMERA_HOTKEYS` selects another config path.

## Connect an AI client

Start a live game session, then configure a local MCP stdio server with the
AppImage as command and `--mcp` as its argument. Starting MCP does not start a
second game. No API key or model subscription is embedded in Camera Studio;
the AI runs in the user's chosen MCP client.

For Codex, adapt `packaging/live/mcp-config.example.toml` in the local MCP
configuration. The `command`, `args`, `env` and `[mcp_servers.<name>]` structure
follows the [official MCP configuration documentation](https://learn.chatgpt.com/docs/extend/mcp?surface=cli).
For clients using JSON, see `packaging/live/mcp-config.example.json`.

The connector exposes typed camera/demo tools, bounded actor inspection,
SteamID spectator selection, keys and presets, project settings, scrub and
preview, undo/redo, export/import, scene and HUD controls, offline demo analysis,
and builtin agents/team colors. The camera engine samples the path locally;
the AI does not have to send every frame.

The AI workflow is bundled in [mcp-agent-guide.md](mcp-agent-guide.md) and
published through MCP `resources/list` / `resources/read` as
`cs2-camera://guides/agent`. Initialization points clients to it. Offline tools
accept an explicit `demo` path with CS2 closed; omitting it uses the current
launcher's demo. `cs2_demo_inspect` lists actual players, weapon IDs and round
start ticks. `cs2_demo_find_moments` filters recorded kills by player, weapon,
round, headshot and tick/time bounds, including bounded same-attacker/round
multikill groups. `cs2_demo_player_positions` samples at most 32 exact ticks.
Results include absolute demo ticks and simulation seconds (`tick / 64`),
without guessing missing positions or treating them as solved camera views.

`get_state` reports the current camera, including FOV. `set_camera` changes that
held view; it does not rewrite a key's lens. Use `update_key` with an `id` and
`fov` to edit a key, or `add_key` to capture the current view into a key. Live
camera authoring accepts FOV 1–179 degrees. Native capture ABI 3 accepts 30–140;
an unsupported capture lens fails clearly before the game starts. The saved
render range is project metadata and is editable through the existing
`set_project`/import/export tools.

Read `get_state.scene_capabilities` before changing effects through `set_scene`.
The **Combat** group exposes available player X-ray control. Clean frame also
disables available X-ray. Scene overrides are static project settings with native
readback, Undo/Redo, persistence and capture propagation. The blood adapter is
unavailable: matched real-game captures show that registered `violence_hblood`
does not suppress the current CS2 hit-particle effect. The legacy generic metal
spark cvar is unregistered in this client. These ineffective/unavailable controls
are hidden and rejected through MCP or capture instead of appearing to work.

Blood amount, spark count and velocity spread do not currently have a verified
live adapter. They are emitter/initializer values in compiled particle resources,
not substitutes for global particle budgets, weapon spread or blood CP3 color.
The current build does not expose nonfunctional quantity sliders.
Only controls marked available can be applied; runtime types, limits and engine
readback are authoritative. `reset_scene` restores this session's baseline.
The Lens toggle enables the manual DOF override and, when registered, the
engine's DOF renderer in one edit. Focus-distance fields explain when manual
DOF is off. An absent renderer toggle leaves its rendering status unverified;
it does not by itself disable registered focus controls. Lighting has one
Normal / Fullbright / Diffuse selector. Exposure `0` selects the engine's
automatic mode. Bloom and postprocessing have separate enable controls where
the game exposes them; a disabled gate is shown beside the effect settings.
Bloom strength `-1` restores automatic strength independently of other scene
settings; nonnegative values override the engine strength.
Checkboxes, shading selection and numeric edits wait for typed engine readback
and display rejection next to the control.

These controls drive the game's existing rendering parameters. This prototype
does not yet implement its own depth-buffer DOF, configurable bokeh iris or
map relighting shaders. A successful value readback confirms application of
the parameter; visually inspect the scene before claiming an effect rendered.
Scene overrides are saved in `project.scene`. Offline capture applies them
through an internal whitelisted native adapter during preroll and requires an
actual typed readback record before recording; unsupported controls fail the
job. A gray-map material override is unavailable.

`cs2_assets_cosmetics_catalog` lists builtin agents from the installed item
schema and localization. **General → Player agents** assigns an agent to an
exact SteamID; T agents require T players and CT agents require CT players.
Read `get_state.cosmetics.capabilities` before using `set_player_cosmetics` or
`set_team_palette`. Map defaults restore the current map's original model.
The project retains agent choices and RGB team colors with undo/redo.
Native readback and `render_verified_1p`/`render_verified_3p` are separate from
visual verification.

Weapon paints, glove replacements and global appearance packs have been removed
from this build, including their runtime adapters and UI/MCP controls. Old
projects containing requests for these removed features fail explicitly before
mutation or export. Weapon/knife definition swaps and custom model import are
unavailable. Stock demo weapons and gloves continue to render normally.

`set_presentation` chooses `view=player|free` and `hud=gameplay|demo|hidden`.
Gameplay HUD requires player view and its separate native adapter. Its live
implementation is partial: spectator money and weapon panels can remain visible.
Successful requests and readback do not confirm a complete player HUD. Read
`get_state.presentation`: requestable capabilities allow a request, while actual
view/HUD and `pending` show whether it has taken effect. The choices persist in
`project.presentation`. Offline capture supports the authored free camera and
demo/hidden HUD, with presentation HUD taking precedence over `scene.hud_visible`.
Player view/gameplay HUD capture fails before closing the live session and
remains in the saved snapshot. Agent and team-color capture require matching
native application/readback and restoration proof before recording. Empty
agent lists and a disabled palette retain their settings without blocking capture.

Example requests to an AI: “Read the current camera, add a key here, then make a
five-second dolly ending 120 units forward”; “Set the first key's outgoing
Bezier to `[0.42,0,0.58,1]` and preview with the scene paused.” Camera placements
still need visual inspection: automatic scene understanding, visibility traces
and obstacle avoidance are not implemented in this prototype.

## Separate demo and camera timelines

The full demo strip uses the actual playback tick and full duration from the
recording's `DEM_FileInfo`. Round bands and the selected SteamID's recorded
kills/deaths use the **Choose marker player** roster filter. This recorded
roster remains usable when the current client entity/observer adapter is
unsupported. **Follow viewed player** clears the explicit filter and uses a
confirmed observer identity when available. It does not switch the watched
player; the separate spectator selector remains under **General → Player**.
MCP exposes the same filter as `select_timeline_player` with an exact SteamID
from `get_state.demo_timeline.players`, or `null` to follow the viewed player.
Clicking a marker or
scrubbing this strip seeks an absolute demo tick; 15-second and round buttons
operate on the demo. These controls do not edit camera keys or the shot's range.
The camera strip retains its own keys, project time, Bezier easing and In/Out.
Preview plays the authored shot; Play demo controls the recording itself.

The launcher parses events once outside the game and stages a private bounded
index bound to the demo's path, size, modification time and SHA-256. No event
parser or per-frame file scan runs in CS2. Missing/invalid metadata shows its
reason; the UI never uses the last kill as the demo's end. Only the host can
refresh the recorded event metadata for a compatible running editor.

Scene numeric drafts wait for acknowledgement of their own value so stale
capability snapshots cannot overwrite committed edits. A rejected native edit
shows its error. Enter, click-away and middle-button gestures keep their normal
commit/cancel behavior.

Black/flickering first-person hands and weapons with `game.watereffectsdepth`
render-target allocation errors are also reported in
[Valve's Linux tracker](https://github.com/ValveSoftware/csgo-osx-linux/issues/4177).
A graphics setting/shader-quality toggle has helped affected users. This is a
reported workaround, not proof of the cause in every session; the editor's
scene lighting reset does not rebuild the engine's render targets.

## Build, compatibility and recovery

Build the live controller, overlay and capture plugin before packaging:

```sh
.venv/bin/python -m pip install -e '.[appimage]'
python3 native/live/build_live.py
python3 native/live/build_overlay.py
.venv/bin/python native/build_motion.py --cache-dir .cache/csdm-motion-capture
.venv/bin/python -c 'from cs2_mvp.discovery import ensure_plugin; ensure_plugin()'
.venv/bin/python tools/build_live_appimage.py
```

Python, Qt, Noto Sans, the live controller, overlay, capture plugin and verified
stock fallback are bundled. CS2/Steam and the
graphics driver are supplied by the machine. Native dependencies are pinned
and checksummed. No source compiler is needed by the packaged user.

This prototype targets Linux x86-64. The camera has separate guarded profiles
for `libclient.so` GNU build-IDs
`f00942ca6d0c486d11f988c53381c47a5be27143` (Steam 26144732),
`f1e04e3616921a3bb25940eea9b4b6bfd7ccada5` (26087496) and
`d5bacd2f5df4a3a68fa8bee8692337993548e14a` (25966348). The 26144732 profile
passed ELF/ABI guards and live hook connection. A same-tick Nuke comparison
confirmed visible fullbright, forced exposure, DOF focus override and the post
processing switch. Bloom strength and fog still need separate visual proof;
registered values alone do not confirm their rendered effect. Entity, observer
and cosmetic adapters retain their own older
build guards: camera support alone does not enable them on a newer client.
Unsupported client builds refuse camera writes. The build currently
requires glibc 2.38 or newer; other distributions and GPUs need separate
validation. Windows is a future platform adapter.

Session logs and checkpoints live under
`~/.local/state/cs2-camera/runs/<session>/`. The native control socket is placed
in a private home-backed IPC directory, because Steam Runtime does not expose
arbitrary host `$XDG_RUNTIME_DIR` subdirectories. The host session descriptor
remains under `$XDG_RUNTIME_DIR/cs2-camera-<uid>/`.

Normal exit restores the plugin transaction and game configs. After SIGKILL or
power loss, close CS2 and run the AppImage with `--recover`. Recovery checks the
recorded hashes and retains conflicting user/Steam edits. It reports existing
config backup locations without choosing an ambiguous backup automatically.

The AppImage also accepts `render` and `finish` subcommands;
`CS2-Camera-Studio-x86_64.AppImage render --help` shows capture options.
FFmpeg/ffprobe must be available on the host. Packaged capture jobs and logs
are stored under `~/.local/state/cs2-camera/capture/`.

## Export a video

Click **Export** in the in-game editor, use **Ctrl+E**, or choose **Export
Video…** in the desktop launcher. The desktop export window chooses MP4 or a
lossless PNG sequence in a new folder, resolution, whole composition/region
(initially the saved project mode), native capture FPS, playback FPS and audio.
No FFmpeg command needs to be typed.
New exports default to **2560 × 1440 (QHD)**. Full HD, 4K and HD presets remain
available; 720p diagnostic samples do not limit the export resolution.
Dedicated demo shortcuts can pass `--demo /absolute/match.dem --start-session`
to start immediately while retaining the desktop owner of the export dialog.
`--no-launcher` is terminal-only and does not open that dialog.

**Normal duration** resamples native captures to the chosen video FPS.
**Slow motion** keeps every captured sample and plays it at the video FPS;
audio is muted. For example, 125 samples from 0.125 demo seconds captured at
1000 FPS become 125 frames at 60 FPS, approximately 2.083 video seconds. The
window shows predicted frames, duration and raw disk space before export.
**PNG sequence (lossless)** retains every native sample regardless of the MP4
retiming mode. Each RGB8 frame is verified against its original captured image.
The folder includes frame numbering, capture/interpretation metadata and AE
import instructions. Interpret a 1000-sample sequence at 60 FPS for 16.6667×
slow motion. PNG compression preserves image values; the output can still be
large. See [lossless frames and AE](high-fps-capture.md#lossless-frames-for-after-effects).

The application saves a fresh camera project before closing its owned live
session, waits for game/config restoration and lock release, then records and
encodes in a background job. Cancel stops only owned capture/media processes;
the saved project and recovery files remain available. **Reopen this demo** is
enabled by default and restores the project, scene settings, playhead and
camera after the job. Unsupported legacy skin or appearance requests block
export before the live session stops. Agent and team-color requests are
preserved and checked against native capture readback and restoration.

The batch `cs2-mvp render` remains available and supports native capture up to
1000 samples per demo second. See
[high-FPS capture and slow-motion conforming](high-fps-capture.md) for the actual
125-frame game test and model-interpolation limits. Depth/matte capture, collision checks and automatic directing
are later work.

The editor uses the installed game’s Noto Sans and Stratum2 Monodigit fonts;
Valve assets are not redistributed. Dock dimensions and neutral color tokens
follow the native demo-controller styles (96 logical pixels at 1080p, 36-pixel
controls). A two-pass Gaussian backdrop and black `#000000a0` dim (evaluated in encoded sRGB to preserve contrast) are rendered
inside the Vulkan swapchain. Camera-specific controls and vector icons are
custom additions. Unsupported HDR/UNORM surfaces retain the dim background
without the blur.

Checks:

```sh
.venv/bin/python -m unittest discover -s tests -v
python3 native/live/tests/run_tests.py
python3 native/live/tests/overlay_resolver_test.py
python3 native/live/tests/run_overlay_blur_test.py
python3 native/live/tests/run_overlay_capture_test.py
python3 native/live/tests/run_overlay_vulkan_dispatch_test.py
python3 native/live/tests/run_editor_ui_test.py
```
