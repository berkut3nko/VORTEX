# Map and minimap

[[../../../00 Docs Map|Docs map]] · [[../README|docs index]] · [[../ROADMAP|roadmap]] · [[game-ui|game UI]] · [[worldgen|worldgen]] · [[fight|fight]] · [[view-distance|view distance]]

| | |
|---|---|
| Status | Designed, nothing built or measured for it. |
| Decided | - Owner (2026-11-23), the map quiz: landmarks (castles, villages, dungeons) always marked; terrain detail only where the player has been (an explored mask, later kept in the server-held profile); "hot zones" = areas where an event will start soon; inside a fight area the minimap shows only the player and the party (finding enemies is part of the fight); minimap = a live top-down picture, full map (M) = parchment style, "but choose more optimized".<br>- Us (this design, 2026-11-24; the owner may veto): no second ray-traced view. One CPU-baked map image (per metre column: top height and top material), patched only where a chunk column changed, drawn by the HUD pipeline in two looks ("Decision" 1-7; the task pack settles the rest). |
| Next | [map-1] (after [wg-W1]; pack [llm/map-1.md](../llm/map-1.md)). [map-2] and [map-3] in "Stages". |
| Open | Questions 1-5 in "Open questions" (each with the recommended answer). |
| History | - |
| Marks | "Code" = read on engine `9474e9c` / game `9729e09` ([wp-7] committed). "(e)" = an estimate, replaced by a measurement in [map-1]'s last card. HUD numbers measured before this design are from [game-ui.md](game-ui.md) "Rendering and cost". |

## Decision

1. **One map image, baked on the CPU.** A grid of 32-bit texels, one per square metre of the map's extent, each
   holding the height of the top solid voxel of that metre column (decimetres), its material id and two flags
   (known, explored). The map never traces rays: it reads the `voxel::World` that the game already streams.
2. **Patched, never rebuilt.** A chunk column is re-baked only when `World::ColumnRevision` (new, the largest
   `ChunkRevision` of the column's cells) differs from the revision it was baked at. Edits, loads and server chunk
   data all bump it. At most 24 columns are baked per frame, nearest first.
3. **One buffer on the GPU** (`u_Map`, binding 11, a storage buffer like `u_Grid`): a 64-word header (extent, hot
   zones), two 256-entry colour tables (minimap and parchment, by world material id), then the texels. Uploaded by
   4 KiB page through the frame's staging buffer, as the grid is ([perf.md](perf.md) M0's `upload` pass counts it).
4. **Drawn by the HUD pipeline.** A HUD quad gets a kind in its last word (`HudQuad::reserved`, today always 0):
   kind 0 is today's 5 x 9 cell quad; kind 1 is the minimap look, kind 2 the parchment look, and the quad's other
   words carry the view (centre, scale, opacity, circle mask). No new pipeline, no texture, no sampler, no image
   layout: one more branch in `hud.frag` that is uniform per quad.
5. **Markers are ordinary HUD quads** drawn over the map: landmark icons, the player's arrow and party dots as
   5 x 9 cell bitmaps (`HudCanvas::Cells`), labels as text. Hot zones are drawn by the map shader itself (rings
   from the header), so a ring costs no quads.
6. **Explored is a separate mask** of 4 x 4 m cells (one bit each), the source of truth; the texels' explored bit is
   its copy for the shader. It is what a server-held profile will store later (8 KiB for 1 km x 1 km).
7. **Local display only.** Nothing of the map decides a voxel, a hit, a movement or a byte on the wire. It is not
   in `buildHash`. A texel is a pure function of its metre column's voxels, so tests are reproducible whatever the
   bake order.

What the player gets: a round minimap in the top-left corner (north up, the player's arrow, teammates' dots,
landmark icons at the rim when they are off it); M opens a parchment map of the whole extent with contour lines,
landmarks with names, hot zones pulsing, and detail only where the player has been.

## What the code gives us today

- `voxel::World` (`engine/src/voxel/World.cppm`): `ChunkRevision(chunk)` (0 when not loaded, never reused),
  `StateOf(chunk)` (`CellState` Unloaded, Loading, Empty, Uniform, Dense, Packed), `GetChunk` (Dense only, or the
  uniform id), `ReadChunk` (any loaded chunk, Packed decoded), `GridWords()` (cells, then one column-top word per
  window column: rows at or above it are air), `WindowMinChunk()`, `WindowSize()`. `TakeChunkChanges()` has one
  consumer (`physics::TerrainColliders`): the map must not take it, hence the column revision.
- `vortex.core:hud` (`engine/src/core/Hud.cppm`): `HudQuad {x, y, w, h, color, bitsLo, bitsHi, reserved}` (32 B,
  `VxHudQuad` in `shaders/include/common.glsl`, binding 10 `u_Hud`), `HudCanvas` (`Rect`, `Frame`, `Text`,
  `Crosshair`, `DimBackground`), `BitmapFont`. `reserved` is written 0 everywhere.
- The renderer (`engine/src/graphics/internal/Renderer.cpp`): the `hud` pipeline draws `vkCmdDraw(6, quadCount)`
  after `present`, each pass timed (`GPU hud`); `FindBuffer` binds storage buffers by name; the voxel upload copies
  changed 4 KiB grid pages through `frame.staging` (`GridPageRevisions`).
- `ShaderContract.BuffersUseTheCommonBindingTable` (`engine/tests/graphics/ContractTests.cpp`, `kBuffers`): bindings
  0-10 are buffers, textures start at 16. Binding 11 is free.
- `voxel::MaterialLibrary` (`Material::color` linear albedo, `MaterialFlag_Liquid`, `MaterialFlag_Foliage`) and
  `voxel::WorldMaterials` (the world palette's ids by name).
- Worldgen ([wg-W1]): `worldgen::RegionPlan` with `MarkerRec` (`MarkerKind::Landmark` at the stage tower's top) and
  `BuildingRec::settlement`. Fights ([wp-7]): `sim::FightRules::teamSpawns`, `towers`, `net::Client::TeamOf`,
  `RemotePlayers()`. The plaza: `sim::RegionTemplate::anchor`.

## The map image

### Extent

`voxel::MapExtent {glm::ivec2 originMetres; glm::ivec2 sizeMetres; float heightBaseMetres;}`: the map covers world x
in `[origin.x, origin.x + size.x)` and z likewise, 1 texel per metre. The origin and the size are multiples of 4 m
(the explored cell). Set by the game before the first frame:

| Run | Extent | Texels | Bytes (texels) |
|---|---|---|---|
| Offline default (plaza, open terrain) | 1024 m square centred on the spawn, snapped to 4 m | 1024 x 1024 | 4 MiB |
| Offline `--region village` ([wg-W1]) | the plan area, 160 m, plus 16 m each side = 192 m | 192 x 192 | 144 KiB |
| Session / practice | the play bounds plus 16 m each side (arena250: 63 chunks = 252 m, plus 32 m = 284 m) | 284 x 284 | 315 KiB |

The cap is 1024 x 1024 texels (4 MiB): a larger extent is refused (`Map: extent too large`) and the map is off for
that run. Outside the extent the minimap shows "unknown". The open world beyond 1 km is [map-3] (map tiles with
the 1 km zones of [net-9] / [wg-W5]).

`heightBaseMetres` is the world's lowest row (-48 m for the full-height window); heights are stored above it.

### Texel

One `uint32_t` per texel, row-major (`tx + tz * width`), `tz` growing with world z:

| Bits | Field |
|---|---|
| 0-15 | `heightDm`: decimetres from `heightBaseMetres` to the top of the top solid voxel (`floor(((y + 1) * voxelSize - base) * 10 + 0.5)`, clamped to 0..65535); 0 when the column holds no solid voxel |
| 16-23 | the top voxel's world-palette id (0 = none) |
| 24 | `Known`: the column was baked at least once |
| 25 | `Explored`: copy of the explored mask's cell |
| 26-31 | reserved (0) |

The sample of texel `(tx, tz)` is one voxel column: `vx = floor((origin.x + tx + 0.5) / voxelSize)`, likewise `vz`.
At 0.125 m that is voxel 4 of the 8 in each metre (an exact float); at 0.1 m and 0.15 m the same formula holds.

### Colour tables

Built once from the `MaterialLibrary` when the map starts (and again if `MaterialLibrary::Revision` changes):

- **Minimap colour** (display RGBA8): the material's linear albedo to sRGB, times 1.15 (clamped) so the small picture
  reads brighter than the lit scene; liquids keep their colour, emissive materials (`lava`, `torch`) are drawn at
  full brightness.
- **Parchment class** of each id (`voxel::MapClass`): `Water` (liquid flag, or `water`), `Lava` (`lava`), `Rock`
  (`stone`, `stone_dark`, `bedrock`, `crystal`), `Soil` (`dirt`, `sand`), `Green` (`grass`, `grass_dry`, foliage
  flag), `Snow` (`snow`), `Built` (`wood`, `planks`, `brick`, `iron`, `gold`, `glass`, `torch`, any id not named
  here). The parchment table holds the class's ink colour (fixed table in "Looks").

### Baking a chunk column

For the chunk column `(cx, cz)` (`MapBaker::BakeColumn`):

1. Texels whose sample voxel column lies in this chunk column (`32 cx <= vx < 32 cx + 32`, likewise z): at 0.125 m
   a 4 x 4 block; at 0.1 m 3-4 per axis; at 0.15 m 4-5. None (outside the extent): done.
2. `top = World::ColumnTopRow(cx, cz)` (window rows `>= top` are air; -1 when the column is not in the window: skip
   and leave the texels as they are).
3. For window row `r` from `top - 1` down to 0, chunk `(cx, minChunk.y + r, cz)`:
   `StateOf` is `Unloaded` / `Loading` -> stop: the column is incomplete, nothing is written, its revision is not
   recorded (a later load bumps it and the column is baked again). `Empty` -> next row. `Uniform` (`GetChunk`'s
   `uniformId`) -> every sample still open gets this chunk's top voxel row and the id. `Dense` -> `GetChunk`;
   `Packed` -> `ReadChunk` into the baker's scratch chunk; then for each open sample, scan `y = 31..0`, the first
   id != 0 closes it. Stop when every sample is closed.
4. Samples still open after row 0: height 0, id 0 (a hole to the void).
5. Write each texel: height, id, `Known`, and `Explored` from the mask; bump the revision of its 4 KiB page; record
   `ColumnRevision(cx, cz)` for the column.

Water and leaves count as the top: lakes are blue, forests green, and a roof is the building's colour.

### Scheduling (`MapBaker::Update`, once per frame, main thread)

1. For each window column `(cx, cz)` inside the extent: `rev = World::ColumnRevision(cx, cz)`; if `rev != 0` and
   `rev != baked[cx, cz]`, it is dirty. (A 48 x 48 window: 2304 reads, about 5 us (e).)
2. Sort the dirty columns by squared chunk distance to the focus column, then by `(cz, cx)` (a total order), and
   bake the first 24 (`kBakeColumnsPerFrame`). The rest stay dirty: their revisions were not recorded.
3. Columns that left the window keep their texels: the map remembers what was seen.

At 0.125 m the first fill of a 48 x 48 chunk window is 2304 columns: about 96 frames (1.6 s at 60 fps) after the
window filled. A blast changes 1-4 columns: re-baked the same frame.

Cost (e): 10-25 us per column (a top-down scan of 16 sample columns through 1-2 chunks; a Packed chunk's decode
about 30 us more); at most 24 per frame: 0.25-0.6 ms worst case, about 0 when nothing changed. Measured by
[map-1] ("Costs").

### Upload

`voxel::MapImage` keeps one revision per 4 KiB page of its word array (`PageRevisions()`, as
`World::GridPageRevisions`). The renderer keeps the GPU copy device-local, and in the voxel upload copies the pages
whose revision is newer than its own, at most 64 pages (256 KiB) per frame. The first upload of a 4 MiB map takes
16 frames. When there is no map (`FrameInput::map == nullptr`) a minimal buffer with `width = height = 0` is bound,
so `hud.frag` always has `u_Map`.

## Explored mask

`voxel::ExploredMask`: one bit per 4 x 4 m cell of the extent (1024 m: 256 x 256 bits = 8 KiB), row-major.

- **Reveal**: when the player's feet enter a new cell (and on the first frame), every cell whose centre is within
  16 cells of the feet cell (`dx^2 + dz^2 <= 256`, integer cell offsets) is set: a 64 m disc. Newly set cells copy
  their bit into their 16 texels (`Explored`), bumping those pages.
- **Sessions and practice**: the whole play area is explored at join (an event area is a place you came to fight
  in; its map is given). The mask is not saved.
- **Offline**: saved at exit to `<DefaultSettingsPath()'s folder>/explored/<key>.bin` and loaded at start, where
  `key` names the world (`seed<S>-<region>-<originX>_<originZ>`, region `plaza` or `village`). Headless runs never
  read or write it (as the settings file).
- **Format** (`ExploredMask::Encode` / `Decode`): `'VXEX'`, u16 version 1, u16 cells x, u16 cells z, i32 origin x,
  i32 origin z (metres), then the bit bytes as runs `(u8 count 1..255, u8 value)`. `Decode` refuses a wrong magic,
  version, size or trailing bytes (and keeps the mask empty).
- **Later** ([map-2]): the server-held profile stores the same bytes per region; the local file goes.

## Landmarks

`voxel::MapLandmark {MapLandmarkKind kind; glm::ivec2 metres; std::string name;}`, kinds `Castle 1`, `Village 2`,
`Dungeon 3`, `Tower 4`, `Site 5`, `Spawn 6`. Always drawn, explored or not. Sources:

| Source | Landmarks |
|---|---|
| A worldgen plan ([wg-W1]: `--region village`) | `worldgen::PlanLandmarks(plan)`: one `Village` per settlement id (the integer mean of its buildings' box centres, named `VILLAGE`), and one `Tower` per `MarkerKind::Landmark` marker (`TOWER`). Castles ([wg-W3]) and dungeons come from the same function as their markers land: `MarkerRec::param` will carry the kind |
| The offline plaza | one `Site` at `RegionTemplate::anchor` (`PLAZA`) |
| A fight session ([wp-7]) | `Spawn` at the own team's spawn (`BASE`, from `Welcome::spawn`: a remote client knows no other fight position, and no wire changes for it). In `--practice` (the session's `SessionConfig` is in the process): `Spawn` at both team spawns (`BLUE BASE`, `RED BASE`) and in felling a `Tower` at each tower (`BLUE TOWER`, `RED TOWER`). [map-2]'s message carries an area's landmarks to remote clients |

The minimap draws a landmark's icon where it is, or clamped to the rim (dimmed) when it is off the minimap, so the
way to the village is always shown. The full map adds the name under the icon.

## Hot zones

`voxel::MapHotZone {glm::ivec2 metres; uint16_t radiusMetres; uint16_t startsInSeconds; uint8_t kind;}`: at most 8 in
the image header. The shader draws each as a ring (2 px at the radius) and a 12 % fill, pulsing once per second
(`u_Frame.temporalParams.w`, the time); the HUD writes `m:ss` under the ring's centre on the full map. On the
minimap a zone off the minimap is an arrow at the rim.

The feed is the event-area server state, which does not exist yet: [map-1] lands the drawing and
`MapImage::SetHotZones`, fed only by the test flag `--hot-zone X,Z,R,S` (offline and practice). [map-2] feeds it from
the server's event-area state (one message, the next free `MessageType` and a protocol bump at that stage).

## Players on the map

- **Self**: always, an arrow (8 directions from the yaw, 5 x 9 cell bitmaps) at the predicted feet (sessions) or the
  player's feet (offline).
- **Party**: in a session, the other players for whom `sim::ShowsOnMap(ownTeam, theirTeam)` is true: today the same
  team (`TeamId` 1 or 2; sandbox team 0 shows nobody). Dots in the team colour (`TeamColour`), from
  `net::Client::RemotePlayers()` (interpolated, so drawn in the past like the figures); dead teammates are a grey
  cross. Enemies are never drawn: in a fight area finding them is the fight. When event areas replace teams by
  parties (free for all, friends in parties), only `ShowsOnMap` changes.
- Offline: self only.

## Looks

Both looks run in `hud.frag` for map quads; `t` = the texel under the pixel, `n` = the neighbours at `-x` and `-z`.

**Minimap** (kind 1): `t` not Known -> `(28, 32, 38)` at 85 % alpha; Known but not Explored -> `(52, 56, 62)` (a fog
of war that still shows the shape of the minimap's circle); else the minimap colour of the id, times a slope shade
`clamp(1 + 0.04 * ((h - h_-x) + (h - h_-z)), 0.65, 1.3)` (decimetres; light from the north-west) and a height tint
(`+8 %` per 32 m above the map's base, clamped). Circle mask: pixels outside the inscribed circle write 0; a 2 px
ring `(230, 225, 210)` at the edge. Hot-zone rings over it.

**Parchment** (kind 2): base `(236, 222, 186)`. Explored texels mix in their class ink at 35 %: `Water (120, 160,
185)`, `Lava (200, 90, 40)`, `Rock (150, 140, 125)`, `Soil (190, 165, 120)`, `Green (150, 170, 110)`, `Snow (245, 245,
240)`, `Built (120, 80, 55)` at 60 %. Contours: `band = heightDm / 40` (4 m); a texel whose band differs from `n`'s
is a line `(110, 85, 60)` at 55 % (every fifth band, 20 m, at 85 %). Unexplored texels are the bare base, 6 % darker,
no contour; unknown the same. Hot zones over it. No texture, no noise: the paper look is the palette and the lines.

Display values, drawn after tone mapping (the HUD contract). Nearest texel; no filtering (4 loads per pixel: the
texel, two neighbours, a colour table word).

## Minimap and full map

- **Minimap**: top-left, diameter `72 px x TextScale` (144 px at 720p, 216 at 1080p), 8 px margin, north up, 96 m
  across (radius 48 m: inside the 64 m reveal disc, so the circle is always explored), centred on the feet. The
  fight HUD's banner is top centre and the kill feed top right: no overlap. Hidden in the pause menu, with
  `--no-hud`, while the full map is open, and with the settings row `Minimap: on / off` (settings page, after
  `Play bounds`).
- **Full map** (M toggles; Escape closes it first): centred, the extent fitted into 80 % of the screen height, zoom
  1x / 2x / 4x with the wheel (2x and 4x centred on the player, clamped to the extent). The scene is dimmed through
  `HudCanvas::DimBackground(0.5)` (L6). The player keeps moving; the crosshair is hidden and the left click casts
  nothing while the map is open. `--map` opens it from the first frame (headless screenshots).
- **Session corner, banner and feed** stay as they are; the map adds no ImGui.

## The HUD contract change

`HudQuad::reserved` becomes `HudQuad::kind` (same offset, same 32 bytes; `VxHudQuad::data.w`):

| `kind & 0xFF` | Meaning | `color` | `bitsLo` / `bitsHi` |
|---|---|---|---|
| 0 `kHudKindCells` | today's 5 x 9 cells | RGBA8 | the cell bits |
| 1 `kHudKindMapMinimap` | map, minimap look | bits 0-15: texels per pixel x 256; bits 24-31: opacity | centre x / z in 1/16 texel (int32) |
| 2 `kHudKindMapParchment` | map, parchment look | as kind 1 | as kind 1 |

`kind` bit 8 (`kHudFlagCircle`): circle mask. The constants are mirrored in `common.glsl` (`VX_HUD_KIND_*`,
`VX_HUD_FLAG_CIRCLE`) and pinned by `LayoutContract.HudQuadKindsMatchTheGlslConstants`. The buffer `u_Map`
(`MapBuffer`, binding 11) joins the binding table (`kBuffers`) and the `hud` pipeline's buffer list; its header
offsets (`VX_MAP_HEADER_WORDS` 64, `VX_MAP_PALETTE_WORDS` 256, texels from word 576) are pinned by
`LayoutContract.MapHeaderMatchesTheGlslConstants`. CLAUDE.md section 5 "HUD" names both tests.

Header words: 0 width, 1 height, 2 origin x (m, int), 3 origin z, 4 `heightBaseDm` (int), 5 contour step (40 dm),
6 hot-zone count (0-8), 7 reserved, 8-39 hot zones (4 words each: centre x and z in 1/16 texel, radius in 1/16
texel, `startsInSeconds | kind << 16`), 40-63 reserved. Words 64-319 the minimap colours, 320-575 the parchment
inks, 576 onward the texels.

## Module placement (respects layering)

| Part | Where | GPU | Tested |
|---|---|---|---|
| `MapImage`, `MapExtent`, `MapLandmark`, `MapHotZone`, `MapClass`, `ExploredMask`, `MapBaker`, colour tables | partition `vortex.voxel:map` (Core) | no | unit (`VortexCoreTests`) |
| `World::ColumnRevision`, `World::ColumnTopRow` | `vortex.voxel:world` | no | unit |
| `HudQuad::kind`, `HudCanvas::Map`, `HudCanvas::Cells`, `MapView`, `MapProjection` | `vortex.core:hud` | no | unit |
| `worldgen::PlanLandmarks` | `vortex.worldgen` ([wg-W1]'s library) | no | unit |
| `sim::ShowsOnMap` | `vortex.sim:combat` | no | unit |
| `u_Map` upload, `hud.vert` / `hud.frag` map path | `vortex.graphics` (`Renderer`) | yes | contract tests, headless screenshot |
| `Engine::Map()` (the baker), `Engine::SetMapExtent`, `FrameInput::map` | `vortex.engine` | - | headless |
| Minimap, full map, markers, M key, flags | game `src/MapHud.cpp`, `src/main.cpp`, `src/Session.cpp` | - | headless + owner checks |

## Costs and their measurement

| Item | Budget | Estimate (e) | Measured by |
|---|---|---|---|
| CPU bake | <= 0.6 ms in a frame that bakes 24 columns; 0 when nothing changed | 10-25 us per column | `Map: baked ...` exit line (max ms per frame, columns) with `--after-fill`, a blast script |
| CPU dirty scan | <= 0.01 ms | 5 us for 2304 columns | the same line |
| Upload | <= 64 pages (256 KiB) per frame; 0 when nothing changed | 0.05-0.1 ms GPU `upload` while filling | `GPU upload` |
| GPU minimap | <= 0.05 ms at 720p on the UHD 620 | 0.01-0.03 ms (16 k pixels, 4 loads each) | `GPU hud` median, with / without `--no-minimap`, fixed `--camera` |
| GPU full map | <= 0.30 ms at 720p | 0.10-0.20 ms (330 k pixels) | `GPU hud` median with `--map` |
| Memory | 4 MiB + 2.3 KiB GPU and CPU at the 1024 m cap; 8 KiB explored | - | `Map: extent ...` start line |

Measure after `World: window filled`, a fixed `--camera`, on AC power (CLAUDE.md "Measuring"). G21's GPU baselines
cover only passes whose baseline is at least 0.20 ms: `hud` (0.01 ms today) stays below that with the minimap;
`upload` is idle once the map is baked.

## Stages

| Stage | What | Wire | Gate |
|---|---|---|---|
| [map-1] | The map image, bake, explored mask (offline file), `u_Map` and the two looks, minimap, M full map, landmarks (plan, plaza, fight sessions), party dots, hot zones from `--hot-zone`, measured costs. After [wg-W1] | no | G25 (proposed: the next free after llm/README's G23 / G24) |
| [map-2] | Hot zones and an area's landmarks from the event-area server state; explored cells in the server-held profile; parties replace teams in `ShowsOnMap`. After the event-area stages | one message (protocol bump at that stage) | - |
| [map-3] | Map tiles for 1 km zones ([net-9], [wg-W5]): the image becomes a toroidal window of 256 m tiles; region names; player pins | no | - |

## Tests

Listed with their files in the pack ([llm/map-1.md](../llm/map-1.md) "Skeleton"): `MapImage.*`, `MapBake.*`,
`ExploredMask.*`, `MapPalette.*`, `MapProjection.*`, `HudCanvas.MapQuadPacksItsView`, `HudCanvas.CellsQuadKeepsItsBits`,
`World.ColumnRevisionFollowsEveryCellRevision` (core); `WorldGen.PlanLandmarksNameTheVillageAndItsTower` (worldgen);
`Combat.MapShowsTeammatesOnly` (sim); `LayoutContract.HudQuadKindsMatchTheGlslConstants`,
`LayoutContract.MapHeaderMatchesTheGlslConstants` and the updated `ShaderContract` table (graphics).

## Rejected alternatives

- **A second ray-traced top-down view** (the owner's "live render" taken literally): a second `gbuffer`-like pass of
  the minimap's pixels each frame costs about what the main view costs per pixel (the UHD 620's trace is the frame
  budget's largest item) and shows nothing a baked height and material do not. Rejected for cost.
- **A sampled image (`VkImage`, R32_UINT or RGBA8) with a sampler**: needs image layouts, a staging copy to an image
  and a second descriptor kind in the HUD pipeline; the HUD was built without any texture on purpose
  ([game-ui.md](game-ui.md) "Rendering and cost"). A storage buffer read with integer texel addresses does the same
  with the grid's upload code. Rejected.
- **Baking from the generator** (`TerrainHeight`, `ClassifyColumn`) for columns never loaded: what is never loaded
  is never explored, so it is never shown in detail; and the generator does not know edits or stamped buildings.
  The world is the only source.
- **Reading `World::TakeChunkChanges`**: it has one consumer (physics); a second would starve it. The column
  revision is a read-only poll.
- **A rotating minimap**: needs rotated icons and labels and makes landmarks at the rim move with every turn. North
  up with a turning arrow (open question 1).
- **Per-pixel CPU rasterisation of the minimap into a small buffer**: 20-40 k pixels a frame on the CPU, uploaded
  every frame; the shader path costs less and keeps the CPU free for streaming.

## Open questions (each with the recommended answer)

1. Minimap rotation: north up (recommended, settled for [map-1]) or turning with the view? The owner may ask for a
   turning minimap; it is a shader rotation plus rotated rim markers, about half a day.
2. Explored in sessions: the whole play area at join (recommended) or only where the player walked? Event areas are
   small and fought over; hiding their terrain adds nothing.
3. Reveal radius 64 m (recommended) or the view radius? 64 m keeps the parchment interesting while exploring.
4. Full-map pan with the mouse: not in [map-1] (zoom centred on the player only); add when the open world grows
   ([map-3]).
5. Landmark names: fixed words now (`VILLAGE`, `TOWER`); generated names come with [wg-W4]'s history hooks.
