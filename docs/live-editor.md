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
Windowed** checked for a borderless window at the current monitor's resolution.
The application owns its session and stays open until the game exits or **End
Session** is selected.

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

## Editing a shot

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

Save/Load in the editor uses the persistent quick-save file
`$XDG_DATA_HOME/cs2-camera/projects/last-project.json` (normally
`~/.local/share/cs2-camera/projects/last-project.json`). MCP can save/load any
explicit project `.json` path. Undo/redo applies to project edits made by both
clients. The active project is also checkpointed in the session run directory.

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

## Hotkeys

| Key | Action |
| --- | --- |
| F6 | Capture/update a camera key, including with the UI hidden |
| F7 | Switch between free and held camera |
| F8 | Show/hide the editor |
| F9 | Play/pause camera preview using the current Sync demo setting |
| F10 | Toggle the separate, movable shortcuts panel while the editor is open |
| K | Capture/update a key with the editor open |
| Space | Play/pause preview with the editor open |
| I / O | Set temporal render In / Out at the playhead with the editor open |
| Delete | Delete the selected key |
| Ctrl+Z | Undo |
| Ctrl+Y / Ctrl+Shift+Z | Redo |
| Ctrl+S / Ctrl+O | Quick-save / quick-load |

Commands are suppressed during text/numeric entry. The editor's **Hotkeys**
button opens a separate movable panel with active bindings and mouse controls. Customize them in
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

The connector exposes 37 typed tools: actual camera/demo state, bounded actor
inspection, set/release
camera, add/update/remove keys, project settings, scrub, preview, demo control,
presets, undo/redo, export/import, save/load, scene effects, and offline demo
inspection, moment search and player positions, builtin asset catalog/private
pack preparation, presentation controls, capability-gated player cosmetics,
weapon paints/team palette and appearance
diagnostics. The camera engine samples the
path locally; the AI does not have to send every frame.

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
Only controls marked available can be applied; runtime types, limits and engine
readback are authoritative. `reset_scene` restores this session's baseline.
Scene overrides are saved in `project.scene`. Offline capture applies them
through an internal whitelisted native adapter during preroll and requires an
actual typed readback record before recording; unsupported controls fail the
job. Model/skin replacement uses separate adapters. A gray-map material override
is unavailable.

`cs2_assets_cosmetics_catalog` lists builtin agents, weapons, knives, gloves and
evidenced paint kits from the installed item schema, localization and inventory
icons. Agent selection requires the target player's current SteamID and team;
T agents are limited to T players and CT agents to CT players. Read
`get_state.cosmetics.capabilities` before using `set_player_cosmetics` or
`set_team_palette`; unavailable categories fail explicitly. Map-based default
agents restore the current map's original model. The project preserves player
overrides and RGB team colors, including disabled palette colors. Native
readback does not prove the image: `render_verified_1p` and `render_verified_3p`
are conservative capability flags. Check the actual image for each view and
case; one successful visual check does not validate the full catalogue.

`set_player_weapon_paint` takes `steam_id`, an exact currently owned `item_def`,
an evidenced `paint_kit`, optional `wear` and integer `seed` (0–1000). MCP checks
the current session installation's catalog and explicit wear range; native code
also verifies capability, ownership and entity lifetime. This changes the paint
on an existing matching item; it does not swap weapon/knife definitions. Omitted
wear uses 0.01 clamped to the kit's declared range. `reset_player_weapon_paint`
requires SteamID and optionally one item definition. Overrides persist in
`project.cosmetics.weapon_paints`. Weapon/knife item swaps and per-player custom
model imports are unavailable.

`set_player_gloves` takes the same finish fields, with an exact builtin glove
`item_def` and its evidenced paint kit. It targets one SteamID and saves one
entry per player in `project.cosmetics.gloves` (up to 128 players).
`reset_player_gloves` requires that SteamID. Read
`get_state.cosmetics.capabilities.gloves` and `get_state.glove_cosmetics` first.
The native adapter requires two verified engine scratch copies before touching
the live item and retains the original item for restoration. `item_readable`,
`ready`, `applied` and `materials_pending` describe different checks;
`restoration_unresolved=true` retains the original journal and blocks a guessed
reset. After resetting one SteamID, confirm its saved override is removed and its
observed original item is restored without a target error or unresolved
restoration. `owned_overrides` counts all retained glove journals in the session;
it reaches zero after all glove overrides are reset. Glove rendering still
requires inspection in each view; the `render_verified_1p`/`render_verified_3p`
capability flags are not automatic image tests. Live apply/reset was visually
checked for AURA, item 5030/paint 10018, wear 0.06 and seed 1: blue Sport Gloves
applied and the original 5035 Hydra gloves returned while the demo stayed paused
at tick 11779. This verifies that case, not the full catalogue.

`set_presentation` chooses `view=player|free` and `hud=gameplay|demo|hidden`.
Gameplay HUD requires player view and its separate native adapter. Its live
implementation is partial: spectator money and weapon panels can remain visible.
Successful requests and readback do not confirm a complete player HUD. Read
`get_state.presentation`: requestable capabilities allow a request, while actual
view/HUD and `pending` show whether it has taken effect. The choices persist in
`project.presentation`. Offline capture supports the authored free camera and
demo/hidden HUD, with presentation HUD taking precedence over `scene.hud_visible`.
Player view/gameplay HUD capture fails before closing the live session and
remains in the saved snapshot. Agent, team-color, owned weapon-paint and glove
capture require a capture plugin with the matching cosmetic proof and restoration
markers. Before `startmovie`, gloves require exact item/finish readback, populated
world/HUD poses, completed material jobs and published world scene objects. HUD
visibility is also required when its validated parent is enabled; a validated
disabled parent exempts only HUD visibility, with HUD pose/material checks still
required. Missing or pending proof blocks recording. All retained capture
overrides must be restored after the final flush. Empty cosmetic lists and a
disabled palette retain their settings without blocking capture.

`cs2_assets_appearance_catalog` reads the installed VPK index, while
`cs2_assets_prepare_appearance_pack` creates a new private pack under
`~/.local/share/cs2-camera/appearance-packs/`. It checks selected file CRC32 and
never modifies the original VPK. This is global resource replacement, not
per-player inventory/paint-kit editing. Preparation does not apply a pack.
Read `appearance_capabilities` before a native appearance call. The temporary
filesystem probe verifies GAME lookup bytes and unmounts without a model
refresh; it is not proof of a visible replacement. Runtime refresh is still
being validated. Offline appearance capture is blocked until a safe adapter
has passed its game test.

Example requests to an AI: “Read the current camera, add a key here, then make a
five-second dolly ending 120 units forward”; “Set the first key's outgoing
Bezier to `[0.42,0,0.58,1]` and preview with the scene paused.” Camera placements
still need visual inspection: automatic scene understanding, visibility traces
and obstacle avoidance are not implemented in this prototype.

## Build, compatibility and recovery

Build the AppImage after both native libraries:

```sh
.venv/bin/python -m pip install -e '.[appimage]'
.venv/bin/python tools/build_live_appimage.py
```

Python, Qt, Noto Sans and the two native modules are bundled. CS2/Steam and the
graphics driver are supplied by the machine. Native dependencies are pinned
and checksummed. No source compiler is needed by the packaged user.

This prototype targets Linux x86-64. The current verified `libclient.so` GNU
build-ID is `f1e04e3616921a3bb25940eea9b4b6bfd7ccada5` (Steam build 26087496).
Separate camera-only guards remain for the previously validated build
`d5bacd2f5df4a3a68fa8bee8692337993548e14a` (25966348); cosmetics require the current
F1 build. Unsupported client builds refuse camera writes. The build currently
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
Video…** in the desktop launcher. The desktop export window chooses a new MP4
file, resolution, whole composition/region (initially the saved project mode), native
capture FPS, video FPS and audio. No FFmpeg command needs to be typed.

**Normal duration** resamples native captures to the chosen video FPS.
**Slow motion** keeps every captured sample and plays it at the video FPS;
audio is muted. For example, 125 samples from 0.125 demo seconds captured at
1000 FPS become 125 frames at 60 FPS, approximately 2.083 video seconds. The
window shows predicted frames, duration and raw disk space before export.

The application saves a fresh camera project before closing its owned live
session, waits for game/config restoration and lock release, then records and
encodes in a background job. Cancel stops only owned capture/media processes;
the saved project and recovery files remain available. **Reopen this demo** is
enabled by default and restores the project, scene settings, playhead and
camera after the job. An active unverified appearance pack blocks export
before the live session is stopped; its reference remains in the snapshot.
Per-player agent, weapon-paint, team-color and glove requests are preserved for
the capture adapter and checked against its actual readback. The real Qt glove
export in `.runs/20261021T115557Z-3fdf2a60/manifest.json` produced 125 blue-glove
frames sampled at 1000 FPS and encoded at 60 FPS (2.083333 seconds), with native
pose/render/attribute checks and zero retained overrides restored at tick 11851.
See [the capture evidence](high-fps-capture.md) for the tested item and limits.

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
python3 native/live/tests/run_editor_ui_test.py
```
