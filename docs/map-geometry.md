# Installed map collision indexing

The read-only indexer extracts real compiled physics from installed CS2 map
archives. It does not use player coordinates, radar artwork or inferred room
names as substitutes for geometry. It does not launch or modify the game.

Run the installed AppImage directly:

```sh
./CS2-Camera-Studio-x86_64.AppImage index-maps \
  --game-dir '/path/to/Counter-Strike Global Offensive/game' \
  --cache-root '/path/on/SSD/cs2-map-geometry' \
  --bootstrap-tool
```

Use `index-maps --help` for its options. `--cache-root` is required and must be
an absolute directory; no geometry cache is created in Home by default. From a
source checkout the equivalent command is
`PYTHONPATH=src .venv/bin/python -m cs2_mvp.map_geometry` with the same arguments.

Omit `--game-dir` to use discovered Steam installations. The scanner includes
`csgo/maps`, `csgo/addons` and installed Workshop content for app 730 in registered
Steam libraries. `--map de_nuke` limits a run to that archive basename;
`--tool /path/Source2Viewer-CLI` reuses the pinned decoder without downloading it.
The official Linux x64 Source2Viewer/ValveResourceFormat 20.1 release archive and
executable are verified against fixed SHA-256 values. No .NET installation is
required for that self-contained tool.

Each cache contains `manifest.json` and `geometry.npz` under
`<cache-root>/<map-name>/<identity-hash>/`. The identity includes complete source
VPK hashes, referenced data-chunk hashes, the installed Linux client build ID,
decoder hash and extractor revision. Source size and modification timestamps
allow stale candidates to be rejected before the collision loader verifies
their hashes. Archives without world physics remain explicitly unavailable in
`index.json`; a vanity or render-only archive is not silently called a scanned
collision world.

The arrays use Source's original Z-up coordinates and units:

- `vertices[N,3]`: little-endian float32 positions.
- `triangles[M,3]`: little-endian uint32 vertex indices.
- `triangle_component[M]`: little-endian uint32 component IDs.

The manifest describes each component's triangle range, XYZ bounds, collision
tags, source part and surface property index. Source convex hulls retain their
individual solid identity. The extractor verifies complete face loops, twin
edges and valid vertex references before marking a hull closed and convex.
Triangle meshes remain surface-only; this extractor does not infer their solid
interior. Bind poses are applied before the arrays are published. The extracted
resource CRC32 is checked against the VPK directory.
Unused RegionSVM and mesh-BVH acceleration dumps are removed as a stream before
loading the text document; source positions, faces, edges, triangle indices and
planes remain intact. Raw decoder text is bounded to 2 GiB on disk and the
remaining collision document to 512 MiB. The published arrays have independent
vertex, triangle and component limits.

`ready` means that the supported baked world collision shapes decoded
successfully. It does **not** mean that every visible object or every state of
the demo has been covered. Separate coverage flags report world collision,
non-baked static props, glass, solid interiors and dynamic entities. Unsupported
analytic shapes or compounds prevent complete world-collision coverage. Moving
doors, broken glass, players, other dynamic objects and render-only geometry
require additional adapters. An empty unresolved-bounds list with
`unresolved_entities_bounds_complete=false` means that unknown geometry is
unbounded, not that no unknown objects exist. The MCP guard must reject spatial
authoring while its required coverage remains unresolved.

The current collision checker accepts at most 5 million vertices and 5 million
triangles per index. The extractor's separate limit is 8 million triangles;
larger successfully extracted maps remain unavailable to checking until the
checker supports them. No successful extraction or 2D preview bypasses that
limit or grants complete coverage.

The decoder format and precedence follow primary upstream implementations:
[WorldLoader](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/20.1/Renderer/Renderer/World/WorldLoader.cs),
[PhysAggregateData](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/20.1/ValveResourceFormat/Resource/ResourceTypes/PhysAggregateData.cs),
[Hull](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/20.1/ValveResourceFormat/Resource/ResourceTypes/RubikonPhysics/Shapes/Hull.cs)
and [Mesh](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/20.1/ValveResourceFormat/Resource/ResourceTypes/RubikonPhysics/Shapes/Mesh.cs).
When both `world_physics.vphys_c` and `world_physics.vmdl_c` exist for the same
world, the `.vphys_c` resource takes precedence, matching WorldLoader. Alternate
compiled forms are not overlaid as duplicate solids. Raw Valve resources are
temporary private inputs; only derived arrays and provenance remain in cache.
