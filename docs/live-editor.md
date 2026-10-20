# CS2 Camera Studio — Linux live prototype

Native Linux CS2 runs once with `-insecure`. A private client-side module owns
the demo camera; a Vulkan layer draws the editor in the game's swapchain. The
same project is edited by the in-game controls and the local MCP connector.
The original `.dem` is never rewritten. The existing batch-render CLI remains
separate.

## Start the application

Run `outputs/CS2-Camera-Studio-x86_64.AppImage`, select a local `.dem`, and click
**Open Demo + Editor**. Steam must already be running. Leave **Fullscreen
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
Use the project’s `start_tick` and `start_tick + duration*64` as the capture range.

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

The connector exposes 17 typed tools: actual camera/demo state, set/release
camera, add/update/remove keys, project settings, scrub, preview, demo control,
presets, undo/redo, export/import and save/load. The camera engine samples the
path locally; the AI does not have to send every frame.

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

This prototype targets Linux x86-64 and the validated `libclient.so` GNU
build-ID `d5bacd2f5df4a3a68fa8bee8692337993548e14a` (installed CS2 build
25966348). A different client build refuses camera writes. The build currently
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

There is no video export control in the live editor yet: this prototype covers
session, camera authoring and preview. The separate batch `cs2-mvp render` now
supports native capture up to 1000 samples per demo second. See
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
