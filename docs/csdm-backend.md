# Native Linux CS2 recording backend

Checked on 2026-10-18 against CS Demo Manager main commit
`7270bc95c8cab87b7f9ddd490fea13c6eda3ace7` (2026-10-15). Its latest released
application is [v3.20.1](https://github.com/akiver/cs-demo-manager/releases/tag/v3.20.1),
published 2026-07-30. No CS:DM executable was found in PATH or the usual `/opt`
installation location on this machine. Research did not start the game or change
Steam/game files.

## Why the native proxy

[Upstream plugin documentation](https://cs-demo-manager.com/docs/development/cs-server-plugin)
explains that CS2's `-netconport` needs Windows-only `-tools`; VDM playback scripts
are unavailable in CS2. CS:DM therefore uses its own server library. Its custom
library also unhides `startmovie` and other development console commands.

[Video documentation](https://cs-demo-manager.com/docs/guides/video) supports Linux
CS2 with native `startmovie`: it writes TGA frames and PCM WAV, then FFmpeg encodes
them. HLAE and separate Vulkan/screen capture are unnecessary. This costs roughly
`width * height * 3 * fps * seconds` bytes for uncompressed frames, potentially
more depending on pixel format. Encoding can use system FFmpeg.

The full [CS:DM CLI](https://cs-demo-manager.com/docs/cli) supports:

```sh
csdm analyze /absolute/demo.dem
csdm video /absolute/demo.dem 12000 12640 \
  --recording-system CS --encoder-software FFmpeg \
  --focus-player 76561198000000001 --framerate 60 \
  --width 1280 --height 720 --ffmpeg-video-container mp4
```

It needs CS:DM's analyzed database, daemon, embedded PostgreSQL and packaged
Electron runtime. The 207.6 MB AppImage is portable, but unnecessary for this MVP:
we reuse only the 1.7 MB ready-built Linux server proxy and its action format.

## Pinned binary and compatibility

`cs2_mvp.csdm.PLUGIN_URL` points at the immutable Git commit above. Its SHA-256 is
`c6bab67a072f83d7ec8e57c1fe243179e267bd9631cb4ee2d9cbf192cea86780`.
The binary last changed in upstream commit
`b7f9dc77487f543c5ff2b5251dba00a92b3b70d8` (2026-07-24). It links only libc,
libgcc and libstdc++; the Steam Linux Runtime supplies the game's environment.

The current binary targets the post-1.41.6.8 engine layout. A subsequent local
pilot on installed CS2 PatchVersion `1.41.9.1`, build `25966348`, loaded the Nuke
demo, selected slot 9 (LocalAI), executed capture at ticks `79864..80504` for the
requested `79863..80503`, produced 300 TGA frames plus WAV and exited. That proves
the recording path for this installation; final encoding/media validation is a
separate pipeline stage. This proxy hooks engine virtual tables and can break
after Valve updates. See [upstream version notes](https://github.com/akiver/cs-demo-manager/blob/7270bc95c8cab87b7f9ddd490fea13c6eda3ace7/src/common/types/cs2-plugin-version.ts).

A complete follow-up `render` run at 1920x1080, 60 FPS succeeded automatically:
requested and logged command ticks were both `79863..80503`, game exit code 0,
600 contiguous raw frames, 599 encoded frames, H.264 video duration 9.983333 s and AAC stereo audio in a
10.00 s MP4. Full decode passed, sound was non-silent (peak -2.0 dB), and visual
inspection confirmed the player's airborne AWP noscope at demo tick 80283.
The output is `outputs/nuke-silo-jump-noscope-1080p60.mp4`. Per-game configs
matched their original backups afterward and no plugin/recovery files remained.
These are observed command boundaries; they do not establish sub-frame temporal
accuracy or universal compatibility with every historical demo.

## Actions and launch

The 2026-10-20 SourceTV run exposed a startup path where
`ClientFullyConnect` never arrived after the Init-time client lookup failed.
The motion proxy now also attempts its frame-clock installation when
`Source2GameClients001` is requested. The successful diagnostic run
`.runs/20261020T152136Z-35c702a6` logs `installed phase=GameClientsInterface`,
120 planned/applied poses and 120 final-view writes without a fault. This does
not change the pinned Linux view-hook offsets or weaken the build-ID checks.

The launcher sets `+demo_ui_mode 0` before `+playdemo`. Setting it only in the
action sequence can be too late to prevent the initial Panorama demo controls.
The launch guard allows only that exact value in that position, alongside the
existing local-demo command; arbitrary console launch commands stay rejected.

[Upstream action generator](https://github.com/akiver/cs-demo-manager/blob/7270bc95c8cab87b7f9ddd490fea13c6eda3ace7/src/node/counter-strike/json-actions-file/json-actions-file-generator.ts)
writes a JSON array of sequences beside the demo: `demo.dem.json`. Each sequence
has an `actions` array of `{tick, cmd}` objects. `build_actions` returns one such
sequence; the caller writes `[sequence]`.

Resolve SteamID64 to the demo's one-based player slot before calling:

```python
sequence = build_actions(12000, 12640, "76561198000000001", "cs2mvp_abcd", 60,
                         player_slot=4)
# json.dump([sequence], ...)
```

The [video sequence generator](https://github.com/akiver/cs-demo-manager/blob/7270bc95c8cab87b7f9ddd490fea13c6eda3ace7/src/node/video/generation/create-cs2-video-json-file.ts)
seeks before running `spec_mode 1`, then `spec_player SLOT`. Seeking and selecting
on the same tick can lose the camera selection. The backend sets up the POV a
second before recording, pauses two wall-clock seconds at start minus four ticks
to clear the loading tint, executes `startmovie NAME` at start, `endmovie` at end,
and `quit` 64 ticks later. It rejects starts below tick 193 because the engine can
skip the first 96 ticks; setup must follow the initial seek on another frame,
with 32 ticks of additional startup slack.

The console logs actual execution as `CSDM: [TICK] Executing: COMMAND`; its native
`csdm.log` omits the `CSDM:` prefix. In the tested installation that file is at
`game/bin/linuxsteamrt64/csdm.log`, despite the older upstream documentation
placing it in `game/bin`. The caller must collect the native log as well. Compare
these actual start/end ticks against the request and also verify frame count and
encoded duration. `echo CS2_MVP_RECORD_START NAME`, `echo CS2_MVP_RECORD_END NAME`
and `csdm_info` provide additional capture diagnostics. Merely finding an MP4 or
requested ticks inside the action-file echo is not evidence of correct playback.

[Upstream Linux launcher](https://github.com/akiver/cs-demo-manager/blob/7270bc95c8cab87b7f9ddd490fea13c6eda3ace7/src/node/counter-strike/launcher/start-counter-strike.ts)
uses the Steam Linux Runtime sniper entry point, not the game ELF directly:

```sh
"/path/to/SteamLinuxRuntime_sniper/_v2-entry-point" \
  --verb=waitforexitandrun -- \
  "/path/to/Counter-Strike Global Offensive/game/cs2.sh" \
  -insecure -novid -sw -width 1280 -height 720 +playdemo /absolute/demo.dem
```

No WebSocket server is needed: `+playdemo` causes the
[native proxy](https://github.com/akiver/cs-demo-manager/blob/7270bc95c8cab87b7f9ddd490fea13c6eda3ace7/cs2-server-plugin/cs2-server-plugin/main.cpp)
to load the sibling action file directly. Its optional connection is
`ws://localhost:4574?process=game` (`CSDM_WS_PORT` can override it). Incoming JSON
supports `{"name":"playdemo","payload":"/absolute/demo.dem"}` and
`capture-player-view`; it does **not** expose arbitrary command execution. Local
actions continue when that connection fails.

## Reversible installation

`temporary_plugin(game_directory, plugin_source, launch_args=...)` expects
`game_directory` to be the installation's `game` directory. Its yielded
`PluginInstallation` records all paths and content hashes.

The [upstream installer](https://github.com/akiver/cs-demo-manager/blob/7270bc95c8cab87b7f9ddd490fea13c6eda3ace7/src/node/counter-strike/launcher/cs2-server-plugin.ts)
places the proxy at `csgo/csdm/bin/linuxsteamrt64/libserver.so` and adds
`Game csgo/csdm` to `csgo/gameinfo.gi`. Our patch inserts it before the first
active `Game` entry, preserving comments, newline style and all existing paths.
Identical existing binaries are reused; a different binary is rejected. Symlinks
within the managed tree are rejected. Only local demo launches with `-insecure`
are allowed; other `+` launch commands and dedicated/secure launches are rejected.
This guard constrains this tool's launch; it is not a network sandbox.

The native proxy directly forwards to Valve's stock server library. Consequently
an existing Metamod server proxy is bypassed during this recording even though
its path and CounterStrikeSharp/addon files remain intact. After cleanup the
original search paths are restored byte-for-byte.

The backend stores `gameinfo.gi.cs2-mvp.backup` and the exclusive, durable
`gameinfo.gi.cs2-mvp.json` manifest before patching. Both are fsynced. It avoids
CS:DM's normal `gameinfo.gi.backup` name because the native plugin restores that
file automatically without checking whether the user changed gameinfo. An
existing normal backup, recovery state, or `gameinfo.vpk` blocks installation.
`gameinfo.vpk` is absent in the checked installation and unsupported by the
upstream installer; its precedence is not assumed or altered. Referenced ordinary
VPK search paths remain unchanged.

Cleanup/recovery requires CS2 to be stopped. Restoration is allowed only when
current gameinfo matches the original or the precise patch. A changed gameinfo,
backup, manifest or owned binary is retained with a `RestoreConflict`; Steam or
user edits are never overwritten. After a crash, run `recover_plugin(game_directory)`
to restore a recognized transaction. A conflicting file needs manual review of
the manifest and backup first. The same exclusive manifest prevents overlapping
recording transactions against the same installation.

## Raw outputs

[Upstream raw-file handling](https://github.com/akiver/cs-demo-manager/blob/7270bc95c8cab87b7f9ddd490fea13c6eda3ace7/src/node/video/generation/get-sequence-raw-files.ts)
finds CS2 frames under `game/csgo/csdm/movie/**/NAME%08d.tga`. Since Armory, WAV
is under the matching `game/csgo/movie/**/NAME.wav`, outside the plugin directory.
Native `startmovie` can create audio even when `record_audio=False`; that flag
controls whether the caller includes the WAV during encoding.

The context manager never recursively removes `csdm` or raw movie files. The
caller should use a unique safe movie name, encode and validate its own files,
then remove only files belonging to that name when cleanup is requested.
