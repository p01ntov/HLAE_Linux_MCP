# CS2 Camera Studio 0.1.14: local review and implementation

## Scope and provenance

This is a local review of the recovered 0.1.13 source, saved as Git commit
`3947490` before edits. The user has no associated GitHub pull request. Work,
review evidence, build state and test fixtures are confined to
`/mnt/ssd/CodexWorks/CS2CameraStudio`; the editable source is `worktree`.

SourceMVM was researched at commit
`51d2e12826f157948568fdada8aa1bdb09e188d4` (main, 0.4.0). Its open PR #113 is a
draft Windows CS:S port, not the application's PR or a Linux adapter. The
implementation here is original code inspired by its workflows; no SourceMVM
code, shaders or game assets were copied. Its MIT license requires retaining
the authors' notices if substantial source is copied in a future change.

## Implemented behavior

- Export profiles: reusable capture/encoding settings, last used values, named
  profiles, and a bounded history of completed/failed/cancelled exports. Applying
  a profile does not overwrite the destination or current project range and
  does not start a job. Failed history persistence cannot turn a finished render
  into failure or interrupt session restoration. Concurrent preset changes use
  a compare-before-replace check under the write lock; an older window cannot
  silently overwrite or delete a newer saved profile. Extreme history dates
  fall back to UTC if local-time conversion overflows.
- Export lifecycle: Escape cancels a running job while keeping its progress
  window accessible; completion requires a successful renderer result.
- Native protocol: queued mutations expire before execution after two seconds;
  disconnected callers cannot leave queued changes to execute later. A lost
  response after execution starts is reported as an unknown outcome. UTF-8
  project names/key IDs no longer expand through ASCII escaping on restore.
- Project validation: duplicate IDs are checked in linear time rather than with
  an all-pairs scan, preserving the existing bounds and failure behavior.

- Timeline: stable-ID multiselection, marquee, one guarded group retime with
  one undo, nearest-unselected-key and duration bounds, conflict/Escape
  cancellation, fitting and exact navigation. Validated project parsing is
  cached until the project changes.
- Project browser: cached locale-independent search and stable sorting,
  compatible-only filtering, preserved exact selection and explicit empty
  results. IDs containing ImGui label markers are hashed as literal data.
- Final cross-feature acceptance: literal key ID `@camera` is distinct from
  live-camera selection across timeline/map/inspector, and a collapsed/minimized
  viewport cancels unfinished pointer gestures instead of committing them on
  reappearance.

Final test counts and package hashes are recorded in the final verification
artifact after independent review.

## Deferred SourceMVM capabilities

Lens/depth/normal passes, relighting, follow cameras and particle/cosmetic
systems need separate Linux Vulkan/entity/resource adapters and live/capture
parity evidence. The existing project schema, command allowlists, geometry
guards and unsupported-feature checks remain the integration contract. A
Windows Direct3D implementation is not a working Linux renderer.

## Verification boundaries

The quality pass exercises real Qt events, headless Dear ImGui interactions,
actual local file/socket concurrency, deterministic controller tests, and
small real FFmpeg/PNG outputs. The AppImage is verified from its extracted
contents and a fresh runtime directory. These checks do not establish live
CS2 compatibility, visual camera clearance, game audio capture or GPU frame
timing; those require a game session and map-specific checks.

## Completed quality pass before package verification

- Existing Python baseline: 377 tests. Final source run: 403 tests, all passed.
- The native controller/socket/helper test runner and ELF isolation check passed.
- Final complete ImGui Editor/Timeline/Tool Surfaces suites passed at supported
  test sizes; the tests exercise real widgets and controller callbacks.
- NVIDIA RTX 4060 offscreen Vulkan readback and dispatch tests passed.
- Independent ASan/UBSan/leak checks passed; grouped retime was exercised with
  30,000 author trials and 4,996 additional independent valid random cases.
- An independent eight-process profile-store stress pass completed 160 writes
  without lost presets or partial JSON; history retained its configured bound.
- Real Python→C++ IPC round-trip of 3,000 Unicode IDs, cancellation and descriptor
  reuse tests passed. No test drove the running game or changed user projects.

The isolated 4,096-key ImGui timeline CPU benchmark improved from 11.58 to
1.07 ms/frame through validated-project caching. This is not live game FPS.
Timeline multiselection currently operates on **timing**; inspector edits and
Delete still address the primary key. Position/rotation/FOV share each key's
time. No independent channel times or cut semantics were added.

The final package verifier checks archived Python code against source, native
hashes/ABI, runtime relocatability, CLI/MCP and included notices. Its exact result
is stored beside the candidate, not inferred from passing source tests.

Dependency packaging now includes release-pinned upstream license/attribution
texts for Qt for Python, Shiboken, QtBase/SVG/Wayland and demoparser2 even when
the installed wheels omit those files. The build checks their provenance hashes
and dependency versions. Bundled shared-library replacement and source links
are documented in the package license notice.
