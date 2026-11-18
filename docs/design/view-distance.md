# View distance: full height, at least 2x across, voxels no more than 1.5x larger

| | |
|---|---|
| Status | Partly built: stages 1-3 (full-height window, 0.125 m voxels, 92 m offline view radius) built and reviewed; stages 4-7 (distance budgets, exact visibility, LOD levels, presets) not started. |
| Decided | - Owner, 2026-11-11 (the request): render distance at least 2x, voxels may grow up to 1.5x, furniture stays buildable; then "when i lifting uphill all chunks unload util it reaches chunk where player is stands" and "vertical render distance might be almost unlimited, or add deep fog".<br>- Owner, 2026-11-11 (with stage 1): cave mouths beyond 16 m may look plugged one chunk in until stage 5 (question 6); the build ceiling is the terrain bounds + 32 m, rounded to the range's top, 128 m (question 7).<br>- Owner, 2026-11-12: voxel size **0.125 m**, after the stage-2 screenshots (question 1).<br>- Taken with the recommended answers at stage 3, 2026-11-12, recorded in ROADMAP as owner decisions: furniture stays blocky world voxels (question 2); step height stays 0.35 m, it climbs 25 cm risers (question 5).<br>- Owner, 2026-11-12, at a windowed 0.125 m peak view: the 92 m view radius is "the sweet spot" on the UHD 620; frame rate comes before more distance there, so LOD level 1 (192 m) is for stronger presets.<br>- Us (this design): view distance is a preset, voxel size is not (client and server must generate the same world). |
| Next | [vd-4] distance budgets (= [lod.md](lod.md) stage 1 = [perf.md](perf.md) item L1), in ROADMAP "Later" (frame-rate block B). Then [vd-5]..[vd-7], M6. |
| Open | Questions 3 and 4 ("Open questions"); the points in "Open points". |
| History | [stage-1.md](../history/view-distance/stage-1.md): stage 1 as built, acceptance measured, review fixes, still open after stage 1<br>[stage-2.md](../history/view-distance/stage-2.md): stage 2 as built (the voxel preview) and its per-size measurements<br>[stage-3.md](../history/view-distance/stage-3.md): stage 3 as built (the switch to 0.125 m) and its measurements<br>[owner-questions-2026-11-11.md](../history/view-distance/owner-questions-2026-11-11.md): the questions as put to the owner, with the answers noted there<br>[before-rewrite-2026-11-18.md](../history/view-distance/before-rewrite-2026-11-18.md): the original header and the sections this rewrite changed |
| Marks | **(m)** measured (the numbers are in the history files and ROADMAP "Performance and measured values"), **(c)** computed from sizes, **(e)** estimate. |

Related: this document rebases [lod.md](lod.md) to a 0.125 m base voxel and a full-height window (lod.md's other
mechanisms are kept); [perf.md](perf.md) "Ranked list" (L1 = stage 4); [networking.md](networking.md) "Chunk sync"
(the moving session-client window); [fog.md](fog.md) (fog marched to the view radius R).

## Decision in brief

1. **No vertical limit inside the view radius.** The window covers a fixed height range (the generator's clamped
   terrain bounds plus a 32 m build/flight headroom: -48..128 m for the default terrain, or the region's
   `heightRange` in a session) and follows the focus in x and z only. The top of the range is a declared world-space
   build ceiling: an edit or model above it is refused whole with a reason, never cut silently. Climbing, flying and digging never move it vertically, so nothing inside the view radius unloads. This is
   **stage 1** [vd-1], built at the 0.1 m voxel of that time, before anything else in this plan.
2. **Air and solid rock stay free.** Empty and Uniform cells keep no slot, no GPU bytes and one tracer step. New: a
   per-column pass classifies sky and deep rock without a generation job per chunk, and a ray above the terrain
   steps once per column (column-top table) instead of once per chunk.
3. **Only visible or near chunks are dense on the GPU.** New cell state `Packed`: dense content no ray can reach,
   farther than 16 m from every player. It keeps its canonical RLE bytes on the CPU (the region codec), no slot, and
   is written to the grid as uniform (opaque). An edit decodes it in the same `Update`; nothing waits on generation.
4. **Fog only at the horizontal edge**: a fade by horizontal distance over the last 15 % of the view radius. No
   vertical fog limit; the existing atmospheric fog stays a look setting, not a range limit.
5. **Voxel size 0.125 m** (1.25x the former 0.1 m; the engine default since [vd-3]); **2x comes from lod.md
   cascades**: level L is a 48-chunk-wide window at `0.125 * 2^L` m, and every level spans the same height range, so
   level boundaries are horizontal only.
6. **View distance is a preset, voxel size is not** (client and server must generate the same world).

## What limited the height before stage 1

The code before [vd-1], kept as the reason for the design.

| Suspect | Finding in the code | Verdict |
|---|---|---|
| Window placement | `World::Update` set `newMin = focusChunk - windowSize / 2` on all three axes and the focus is the camera (`Engine::Run`). The game window was 48 x 8 x 48 = 25.6 m tall (the game's world config in `src/main.cpp`). Climbing 3.2 m retargeted a whole 48 x 48 slab: the bottom row unloaded (`Recenter` -> `UnloadCell`), the new top row was sky. | **The hill bug.** Fixed by a fixed vertical range, not by more rows. |
| Unseen cave chunks | Caves reach 30 m below the surface (`caveMaxDepth`), dark stone starts 12 m down; such chunks are Dense. At full height: about 2304 columns x 3-5 = 7000-11 500 extra dense chunks (e) on top of about 4000 surface chunks (m: "about half" of 8100) -> over the 12 288 slots. | **The real reason for 8 rows.** Fixed by `Packed` (no slot for what no ray reaches). |
| Generation and integration | Every cell got a job (`SubmitJobs`), every result counted against `maxIntegrationsPerUpdate` = 64, Empty ones included; load order was 3D distance. A taller box multiplies jobs for sky and rock. | Fixed by the column pass and a dense-only integration budget. |
| GPU grid | Grew with `bit_ceil` and the whole grid was re-staged on every revision (the renderer's grid upload in `Renderer.cpp`), about once per frame while streaming. | Not a limit; 495 KiB per copy at 55 rows (c). Fixed by dirty 4 KiB pages. |
| Toroidal indexing | Positive modulo, any size (`World.WindowSizeNeedNotBePowerOfTwo`). | Not a limit; with a fixed y range the vertical modulo is unused. |
| Tracer | `VX_MAX_GRID_STEPS` 512 vs at most 48 + 55 + 48 = 151 cells per ray (c). But every air cell is one step: a sun ray climbing to the ceiling crosses up to 40 more cells (c). | Steps are fine; the per-cell air cost is fixed by the column-top skip. |

## Vertical design

Built in [vd-1]. Where the build differs from the first text, the paragraphs below state the build; the full list
of differences and the reasons is the table "What differs from the design, and why" in
[stage-1.md](../history/view-distance/stage-1.md).

**Fixed range.** `IWorldGenerator::VerticalBounds()` (metres, conservative; default: unknown -> the old behaviour).
For `TerrainGenerator`: low = base - continent - hill - detail - `caveMaxDepth` - 1 voxel = -46.5 m, high = base +
continent + hill + mountain + detail + `kMaxTreeHeight` = 84.9 m (c, from `HeightMeters`). `Fbm` and `Ridged` divide
by the amplitude sum (`SimplexNoise` in `math/Noise.cppm`), but the 2D simplex `Sample` (`70 * n`) has no proven
bound, so sampling cannot pin it: `HeightMeters` **clamps** to the declared terrain bounds (`TerrainSurfaceBounds`),
and `ViewDistance.TerrainHeightsStayInsideTheDeclaredBounds` samples 10^6 heights, which must stay 4 voxels inside
them (so the clamp never engages and no golden changed). Then a **build headroom** of 32 m
(`WorldConfig::buildHeadroom`; a region may declare its own ceiling) is added on top, and the range is rounded
outwards to `WorldConfig::heightSnap` (16 m, the coarsest planned level's chunk): **-48..128 m = 55 rows at 0.1 m, 44
rows at 0.125 m** (c). The headroom rows are sky and cost a cell (28 B) and a grid word each, nothing else. Above the
ceiling is outside the world: an edit, a model placement or an island spawn that would write there is refused whole
(`EditReport::outOfBounds`), and an awake dynamic body entirely above it is removed (`Scene::DestroyAbove`, the kill
plane), with tests (`ViewDistance.EditsAboveTheBuildCeilingAreRefusedWhole`; a model at ceiling + 1 m is refused,
world unchanged). A sparse overflow above the top row was considered and rejected: the tracer would need a second
structure for a case the arena ceiling forbids anyway. A session uses `RegionDesc.heightRange` (rounded to
`sim::kRegionHeightSnap`, 16 m) on server and client, through `sim::RegionWorldConfig`. Rows are capped at 128
(`Verify`).

**Column pass.** `IWorldGenerator::ClassifyColumn(cx, cz)` returns the chunk rows above which the column is Empty and
below which it is Uniform (with the id), computed from the height tile the generator builds anyway. `Generate` uses
the same classification for its early-outs, so the two cannot disagree; `ViewDistance.ClassifyColumnAgreesWithGenerate`
checks it on 64 columns x 3 seeds (with `TerrainSettings::skipClassifiedRows = false` running the voxel loop for the
skipped rows). A new column costs one job (the tile); sky and rock cells are installed on the main thread without a
job unless the `RegionStore` holds a saved chunk there. As soon as `ClassifyColumn` returns, the column's
`COLUMN_TOP` is set to the band's top row (a conservative bound, never below the eventual content) and every band
cell is written as `VX_GRID_PENDING` (grid word `0x80000000`: uniform flag, id 0): the tracer stops on it,
`gbuffer_world.frag` writes it as sky with its depth and `World::Raycast` reports no hit, so a half-loaded column is
never skipped down to its rock top and never shows sky through itself. Only the band between gets chunk jobs: about
10-12 per column at 0.125 m (e; 30 m caves + relief + trees), against 8 jobs per column before; measured 13.7 dense
chunks per column near the spawn at 0.1 m.

**Load order and Recenter.** The 16 m sphere around the focus first, then boxes asked for by `World::RequestLoad`
(a blast's island box), then classification and the surface band of every column by horizontal distance, then the
cave rows (`WorldConfig::surfaceFirstBacklog` = 0: always surface first; distance-first put about 10 000 cave chunks
ahead of the last edge surfaces and made the game start take 25-27 s). The integration budget counts Dense results
only. `Recenter` is incremental: before [vd-1] it walked every cell and `std::sort`ed every Unloaded cell by 3D
distance, which at 48 x 55 x 48 is 127 000 cells per shift. Now it touches only the entering and leaving column slabs
(48 columns x rows per row shifted) and keeps the queue as rings of columns by horizontal distance bucket (each
column holds its band rows surface-first, rebucketed by a counting sort), so a shift re-buckets 2304 columns at most,
with no full sort; a teleport rebuilds the buckets (2304 entries). Memory of unloaded chunks is released on a worker.
Shift time is logged (`recentre max` in the `--climb` / `--fly` lines).

**Throughput.** Jobs in flight are capped by `WorldConfig::maxJobsInFlight`, default `max(2, 8 x workers)` (56 with
the laptop's 7 workers; it was `max(2, 2 x threads)` before [vd-1]). Jobs are submitted once per `Update`, so it is a
per-frame cap: at 2 per worker the climb at 10 m/s left 419 columns unfilled, at 4 the fly left 101, at 8 none. The
queue is shared with collider cooking. Flight at 25 m/s needs about 6 column rows/s, 48 x (1 + 12) = 600 jobs per
row, about 3700 jobs/s (c from (e)). Generation chunks/s is logged at exit (the `World: generation` line: wall rate,
capped by 64 dense integrations per frame, and the generator's own rate per worker-second). Gate: headless `--fly`
(25 m/s at 150 m) logs `max N unfilled columns inside 0.85 R`.

**Packed cells.** A Dense result is installed as `Packed` when it is not visible and farther than `nearRadius`
(16 m = collider radius 12 m + one blast box) from the focus. Its grid word is `0x80000000 | most frequent solid id`;
its bytes are the `voxel::EncodeChunk` RLE (maximal runs, encoded on the worker), so saving it is a copy and
`ChunkHash` is computed from the runs without decoding (same run definition). Visible means:
- stage 1 (geometric, per chunk), as built: a chunk is visible if its row is at or above the row of the lowest
  surface voxel of its 3 x 3 neighbourhood of columns (`ColumnClass::surfaceFrom`), or it was modified, or a face
  neighbour was modified or loaded from a save (crater walls stay visible after a reload). No extra chunk under the
  surface: no ray reaches it except through a cave (7574 -> 5509 dense slots at the spawn camera, 0.1 m;
  `ViewDistance.SkyRaysRarelyMeetPackedChunks`). A cave mouth seen from beyond 16 m looks plugged one chunk in.
  Every column arrival re-judges the Packed cells of its 8 neighbours, so a chunk packed at the window's edge without
  its outer neighbour is judged again once that column loads.
- stage 5 (exact, not built): flood fill over per-chunk face air masks (6 x 128 B) from sky and edge cells; a chunk
  is visible if it is reached or shares a face with a reached chunk that has air on that face. A Packed chunk is then
  never seen by any ray, and its uniform grid word is exact for geometry and material.

**Reveal and edits.** A Packed chunk is decoded into a slot by an edit (at its first changed voxel) and by the
reveal queue (est. 0.05-0.15 ms per chunk in the design; measured 0.016-0.03 ms in release). After an edit, the six
face neighbours of every changed chunk are promoted in the same `Update` (a ray leaving a chunk enters a face
neighbour first). Stage 5 continues through connected caves **on a budget**: a reveal queue ordered by air
connectivity distance from the blast and the camera, N decodes per `Update` (N set so decodes stay <= 1 ms), and the
not-yet-promoted chunks stay Packed, which renders as rock until they are decoded. Reveal slots count against the
terrain budget; store misses are logged (`storeFullLoads` / `storeFullDecodes`). Acceptance is "the connected cave
is complete within K frames", K measured. A load that finds the store full is installed Packed, never Empty (a saved
chunk is never dropped). The focus promotes chunks within `nearRadius` (`WorldConfig::maxDecodesPerUpdate` = 16
decodes per `Update`, scanned whenever the focus has moved a quarter chunk), one chunk ahead of the 12 m colliders; a
decoded buried chunk keeps its runs and is packed again once the focus is `repackMargin` (8 m) beyond `nearRadius`
and it is still hidden, unedited and has no edited face neighbour. `GuardBodies` still freezes anything over a
missing collider; physics reads Packed chunks through `World::ReadChunk` (a decoded copy, no slot).

**Fly mode, high camera, digging.** The window ignores the focus height: a camera at 300 m keeps the same cells,
rays enter the box from above (`VxTraceWorld` already slab-tests the box) and fall to the terrain through the column
skip. Digging down reaches every chunk down to the window floor (-48 m), all already generated (Packed or Dense), so the
blasted chunk and its face neighbours show in the same frame and a deep cave behind it completes within the reveal
budget's K frames. Edits below the floor are `skipped` (existing `EditReport`): the floor is solid by
construction. Load order and the edge fog use horizontal distance, so altitude changes neither.

**Tracer.** Grid cells are column-major, `(x + z*sx) * sy + y` (World, `trace.glsl` and the tests alike: a window
step re-targets whole columns). Appended to the same buffer (`World::GridWords()`, binding `u_Grid`, no new
descriptor): `VX_GRID_COLUMN_TOP(c)` per column = highest non-Empty row, maintained by `World::SetGrid`;
`VX_GRID_CONTENT_TOP` = the maximum; and a **4 x 4 column region top** (window-relative, rebuilt per window move). In
`VxTraceWorld`, a cell above its column top is skipped to the exit of that column's (or region's) air prism in one
step (exit through x / z, or down onto the top), and a rising ray above `CONTENT_TOP` ends. A sky or sun ray then
costs about one step per region crossed (steps per ray in the 48 x 55 x 48 test scene: 15.7 without tables, 8.0 with
column tops, 4.1 with region tops). Shared GLSL/C++ code, pinned by `ViewDistance.ColumnSkipMatchesTheTraversalWithoutIt`
and the reference-DDA tests on a 48 x 55 x 48 window with half-loaded columns. The grid is uploaded by dirty 4 KiB
pages (`World::GridPageRevisions`; the renderer compares `World::InstanceId()` and writes `World::GridFallback` for a
dense cell whose slot is not uploaded yet).

**Fog.** Edge fade = smoothstep(0.85 R, R, horizontal distance of the hit from the camera) to the horizon colour, R =
`World::ViewRadius()` (half the window minus one chunk and the follow slack: 92 m offline at 0.125 m, 88 m for a
moving session client; with LOD levels later, the outermost level's half extent), passed in `jitter.zw`
(`RenderSettings::edgeFog`); sky pixels unchanged. Peaks and valleys are clear inside R at any altitude.

## Memory and per-frame cost

| Item | Before stage 1, 0.1 m, 48 x 8 x 48 | Stage 1, 0.1 m, 48 x 55 x 48 | 0.125 m, 48 x 44 x 48 |
|---|---|---|---|
| Cells (28 B) + grid (4 B) | 18 432: 504 + 72 KiB (c) | 126 720: 3.4 MiB + 495 KiB (c) | 101 376: 2.7 MiB + 396 KiB (c) |
| Dense chunks (slots, GPU) | about 8100 (m) | 5000-7000 (e): surface + one chunk under + 16 m spheres | 4000-6000 (e); 3500-5000 after stage 5 |
| Packed (CPU RLE, 1-10 KB) | 0 | 9000-14 000, 10-140 MiB (e) | 7000-11 500, 10-120 MiB (e) |
| CPU bytes of dense chunks | 285 MiB (c from 8100) | 175-245 MiB (c from the estimate) | 140-210 MiB |
| GPU store | 433 MiB of 473 MiB total (m) | unchanged | unchanged |
| Jobs per new column | 8 (c) | 1 + 12-15 (e) | 1 + 10-12 (e) |

The table and the paragraph below are the design's estimates. Measured after the build (m): at 0.125 m, filled,
5311-5632 store slots in use (`store a/b`) and about 14 800 Packed chunks in 23-24 MiB of RLE; at 0.1 m (release)
recentre median 0.16 ms, p90 0.35 ms, decodes 0.016-0.03 ms each, gbuffer + lighting within +0.25 ms of the same camera
without full height.
Details in the history files' measurement tables; current values in ROADMAP "Performance and measured values".

Per frame on the UHD 620 (all (e), gated in stage 1): incremental `Recenter` touches one column slab per shift
(48 x 55 cells plus a re-bucket of 2304 columns), about 1.5 shifts/s walking: under 0.1 ms per shift (e), gated. Grid upload: dirty pages only, a full copy (405 KiB) only on a shift. Packed decodes:
none at rest; at most 1 ms per blast (gate). Tracer: gbuffer +0-0.3 ms (more pixels hit real peaks instead of sky);
sun shadows +0-0.5 ms (rays now climb to real peaks, and peaks now cast shadows, which is new content). Without the
column skip the same rays would cost an estimated +0.7-2.3 ms, which is why it is in stage 1. Full fill rises about
2.7x in jobs that do real work (e), surface first; the generator does about 1000-1100 chunks per worker-second (m).

## Voxel size choice

| Object | 0.1 m | 0.125 m | 0.15 m |
|---|---|---|---|
| Chair seat 45 cm / back 90 cm; seat 45 x 45 cm | 4.5 / 9; 4-5 square | 3.6 / 7.2; 4 square, 2 gap | 3 / 6; 3 square, 1 gap |
| Table 75 cm, top thickness | 7-8, 10 cm | 6, 12.5 cm | 5, 15 cm |
| Bookshelf 180 x 80 x 30 cm, shelf pitch 30 cm | 18 x 8 x 3, pitch 3 | 14 x 6 x 2.4, pitch 2-3 | 12 x 5 x 2, pitch 2 |
| Door 200 x 90 cm; player 1.8 m / radius 0.3 m | 20 x 9; 18 / 3 | 16 x 7; 14.4 / 2.4 | 13 x 6; 12 / 2 |

0.125 m keeps every object recognisable, is exact in binary (chunk 4 m, edit quantum 1/16 voxel = 7.8 mm) and puts
the level swap below a pixel: a 0.125 m voxel is 1 px at 47 m, 0.25 m at 94 m (c), level 0 ends at 96 m. 0.15 m
gives only 1.5x and a single voxel of air between chair legs. No size can show a 4 cm leg; the owner chose blocky
furniture in world voxels (see "Decided").

## How 2x is reached per preset, and LOD

Every level is a `World` with the same y range (-48..128 m): 44 rows at level 0, 22 at level 1 (8 m chunks), 11 at
level 2 (c). No level needs terrain-following columns: the column pass makes sky and rock free at every level, and
`Packed` covers level 1's buried strata (no caves there). Level-1 cells covered by level 0 (the centre 24 x 24
columns) are installed Empty with no job and never uploaded. `windowSnap` 2 applies to x and z only; a ray leaving
level L through the top or bottom has left every level, so `VxTraceWorldLod` continues into L+1 only on an x / z exit.
Grid steps per level stay at most 48 + 44 + 48 = 140 (c), reset per level (a test pins it).

| Preset | Levels | View radius | Height | x today | Slots (e) | GPU store | GPU cost (e) |
|---|---|---|---|---|---|---|---|
| Minimal | L0 0.125 m | 96 m | -48..128 m | 1.25 | 4000-6000; 3500-5000 after stage 5 | 433 MiB | stage-1 gate |
| **Low (UHD 620)** | L0 + L1 0.25 m | **192 m** | same | **2.5** | + 2800-3200; total 6300-8200 | 433 MiB | gbuffer +0.5-1.5, lighting +0.3-1 |
| Low fallback | L0 + L1 at 40 wide | 160 m | same | 2.08 | L1 2000-2200 | 433 MiB | about 80 % of the above |
| **High (GTX 1650)** | + L2 0.5 m | **384 m** | same | **5** | + 2000-2500; total 8300-10 700 | 433 MiB; 577 MiB (16 384 slots) if stage 5 saves less | unmeasured |

- Terrain slots must stay <= 9000 of 12 288: 1500 are reserved for baked models and debris in a fight
  (`SimulationConfig::chunkCapacity`, terrain and models share the store). CPU: the level-1 `World` adds about 113 MiB (c, 3200 dense chunks).
- **UHD 620 gate** for level 1: gbuffer + lighting at most +2.5 ms over the stage-5 baseline on a far view and +0.3 ms
  on a near view, paid for by lod.md's distance budgets (stage 4: lighting -0.3 ms flat, -2 to -3 ms on a hillside).
  Net target: frame <= 23.5 ms at 768x432. GTX 1650: 16.7 ms target, nothing measured; the owner measures it.
- Shadow and AO rays that leave level 0 horizontally continue in level 1 with a capped step count (+0.3-0.8 ms (e),
  inside the gate). With the full height, a peak 150 m away now shadows the valley at low sun.
- Fill: level 1 has 2304 columns, about 3200 dense chunks; with 128 uploads per frame and level 0 served first, a
  respawn fills in <= 300 frames (gate). "window filled in N frames" and "surface filled in N frames" are logged.

## What changes

Stages 1-3 built the items below except the stage-6 ones (the level-indexed trace, the lod fields of
`FrameUniforms`); "as built" marks where the build differs from the plan.

**World, grid and tracer contracts.**
- `WorldConfig` (as built): `fullHeight` replaces `windowSize.y` by the rows of a fixed range (`heightRange`, or the
  generator's `VerticalBounds` + `buildHeadroom`, rounded outwards to `heightSnap`); `nearRadius` (16 m);
  `packHidden` (default false, so existing tests keep their meaning); `surfaceFirstBacklog`, `maxDecodesPerUpdate`,
  `repackMargin`. `Update(focus)` uses x/z. The game turns `fullHeight` and `packHidden` on; a session takes them from
  `sim::RegionWorldConfig` / `sim::ClientWorldConfig`.
- `CellState::Packed`; `WorldStats` gains `packedChunks`, `packedBytes`, `unloadsInsideRadius`, `storeFullLoads`,
  `storeFullDecodes`. `GetChunk` returns nullptr for Packed; `EnsureResident(minChunk, maxChunk)` decodes a box for
  callers that need resident chunks, and `ReadChunk` gives a decoded copy without a slot (as built, the collider
  streamer uses it). `GetVoxel` reads a Packed chunk from its runs. Packing and unpacking are not content changes:
  `TakeChangedChunks` does not list them, `ChunkHash` / `WorldHash` / saves are byte-identical.
- `IWorldGenerator::VerticalBounds`, `ClassifyColumn` (defaults keep custom generators working, one job per cell).
- Grid (as built): cells column-major, `(x + z*sx) * sy + y`; `World` exposes dirty 4 KiB grid pages
  (`GridPageRevisions`); the renderer copies those (keeping slot -> grid index from its last scan, so an `uploaded`
  flip dirties one page, not the grid).
- `trace.glsl` + `Trace.cpp`: column tops, content top and 4 x 4 region tops appended to `u_Grid` (stage 1, as built:
  no new descriptor), level-indexed trace (stage 6). `FrameUniforms` + C++ mirror: the fog edge in the spare
  `jitter.zw` (stage 1, graphics notes), the lod fields later (+144 B, `ContractTests`).

**Voxel size (stage 3, built).** The game sets no voxel size; `voxel::kDefaultVoxelSize` = 0.125 (`Chunk.cppm`) is
the default of `WorldConfig::voxelSize`, `TerrainSettings::voxelSize` and `scene::SceneConfig::voxelSize`, and
`sim::Simulation` copies the world's size into the generator and the scene. Debris thresholds keep their metre size
(as built: metre constants `scene::kMinPieceVolume` 0.016 m^3, `kDebrisBodyVolume` 0.048 m^3, `kThinBodyLength`
0.6 m and `sim::kServerReplicatedPieceVolume` 0.512 m^3, turned into voxels of each world's size by
`scene::VoxelsForVolume` / `VoxelsForLength`): 8 / 25 / 5 / 262 voxels at 0.125 m (16 / 48 / 6 / 512 at 0.1 m), with
tests. Content written in voxels keeps its metre size (beam, figure, glTF `voxelsPerUnit` = `1 / voxelSize`); the
tree-trunk rule (`Generator.cpp`) is in metres (at 0.125 every trunk would otherwise be 1 voxel). Step height 0.35 m
climbs 25 cm ledges, not 30 cm. Region headers record the voxel size (`"VXR2"`) and refuse a mismatch or an
unreadable file without overwriting it. `Terrain.GoldenChunkHashes` was regenerated once in its own commit;
`GoldenNoiseBits` / `GoldenHeightBits` did not change.

**Networking.** (Measured on `arena250.session`, networking stage 3: preload 9.6-10.4 s against the 10 s gate, 48.5
MiB of RLE against 150 MiB; [stage-3.md](../history/networking/stage-3.md).) The server window is fixed: `playBounds`
+ 3 chunks on every horizontal side (`sim::kServerWindowMarginChunks`; 2 when this section was first written, so the
cell count below is for + 4), vertically `heightRange`, rounded to 16 m; a client's window uses the same range, so
the y part of `WindowUpdate` never changes and a climbing client requests nothing. The server packs with `World`'s
own rule (hidden and farther than 16 m from the window's centre); a server-specific resident rule (resident within
16 m of any player or edited in the last 10 s (e)) is designed, not built. A 250 m region at 0.125 m is (63 + 4)^2 x 44
= 197 516 cells and about 20 000-29 000 dense chunks (e), 0.70-1.0 GiB if all were resident; as measured, 9888 dense
resident + 31 522 Packed chunks. Preload generates every dense chunk once at session start; its time is the gate.
The island search reads Packed chunks through `World::ReadChunk`, a decoded copy without a slot (since [shred-4]
it runs on cached cell graphs: shred.md Part 4), so the search itself takes no slot; only the chunks an operation edits are decoded. The voxel size comes in `Welcome`'s `RegionDesc` (exact float bits, networking.md) and the client
builds its `World` from it; a client that cannot (a forced `--voxel-size`) refuses the session. LOD levels are
client-only render data; nothing new on the wire.

**A session client's window is the game's** (networking stage 7a M-b) when the region is wider than it: 48 x the
region's rows x 48 chunks, full height, following the camera exactly as offline (`sim::ClientWorldConfig`,
`net::ClientSettings::windowChunks`, 48 in the game, `EngineConfig::windowFollowsCamera`), packed as offline, in the
game's 12 288-slot store; a region that fits (playground, drill) keeps the region's fixed window. Unlike offline it
follows with one chunk of slack (`WorldConfig::recenterSlackChunks` = `sim::kClientWindowSlackChunks` = 1, review of
networking 7a: it moves once the camera's chunk is more than one chunk from its centre chunk, so 22-24 whole chunks
lie ahead of the camera's chunk (offline 23), and stepping across a border and back moves nothing). The view radius
leaves the slack out (`World::ViewRadius` = (half the window - 1 - slack) chunks: 88 m instead of 92 m for a session
client; recheck of networking 7a), so the edge fog ends before the window's edge on the side the player walks toward.
Every window step is a `WindowUpdate` (x / z only: the rows never change, as said above; each step is listed by
`World::TakeWindowSteps`, also two in one frame); chunks that leave are dropped (`WorldConfig::keepUnloadedEdits`
false) and come back from the server if they were modified. Measured on `arena250.session`: at most 5563 slots used
while walking across it (5500 after the review, with half the window steps; networking history
[stage-7a.md](../history/networking/stage-7a.md), "Stage 7a as built", "M-b as built").

## Stages

| ID | Stage | State | Commits (engine / game) | History |
|---|---|---|---|---|
| [vd-1] | **Full-height window at 0.1 m** (the hill fix): clamped vertical bounds + 32 m headroom and build ceiling, column pass, `VX_GRID_PENDING` + conservative column top, incremental `Recenter`, surface-first loading, fixed y range, horizontal load order, dense-only integration budget, `Packed` with the geometric rule, decode on edit + face neighbours, column / region-top skip, dirty grid pages, horizontal edge fog, unload counter, headless `--climb` / `--fly`. | Built and reviewed. Windowed climbing: the owner's check. | `5cda1c2` / `7b853bb` | [stage-1.md](../history/view-distance/stage-1.md) |
| [vd-2] | **Voxel preview**: `--voxel-size` (game only, offline, 0.1-0.15 m), the furniture probe (`--probe`, `--camera probe` / `probe-far`, `--probe-blast`), generation chunks/s logged. The owner picked 0.125 m from it. | Built | `809f110` / `d7426f0` | [stage-2.md](../history/view-distance/stage-2.md) |
| [vd-3] | **Switch to 0.125 m**: defaults, thresholds in metres, content, tree rule, import, region header, goldens (own commit), lod.md rebase. | Built and reviewed. Windowed play at 0.125 m: the owner's check. | `5ffa7e6`, `20b5dfc` (goldens) / `e2aae4f` | [stage-3.md](../history/view-distance/stage-3.md) |
| [vd-4] | **Distance budgets** (lod.md stage 1; perf.md L1). Accepted when: lighting -2 ms facing a hillside, no change on a near view. Effort 2 days (e). | Not started | - | - |
| [vd-5] | **Exact visibility flood** (face air masks), replaces the geometric rule; reveals continue through connected caves. Accepted when: brute-force voxel flood test: no Packed chunk is reachable through air; a blast into a cave 60 m away completes the connected cave within K frames (K measured and logged, decodes <= 1 ms per `Update`); dense slots on the headless walk drop >= 20 % vs stage 4 (log); cave-mouth and surface screenshots identical to stage 4. Effort 3-4 days (e). | Not started | - | - |
| [vd-6] | **Level 1 at 0.25 m** (lod.md stages 2-3): second `World`, x/z `windowSnap`, level-indexed trace, uniforms, far lighting, edit downsampling, `--lod`. Accepted when: reference DDA across the boundary; terrain slots <= 9000; respawn fill <= 300 frames; peak screenshot to 192 m with no seam at 96 m; low-sun screenshot with a level-1 hill shadowing level-0 ground; UHD 620 gates met, otherwise 40-wide level 1. Effort 1.5-2 weeks (e). | Not started | - | - |
| [vd-7] | **Presets and level 2**: `EngineConfig` view preset, `--view`, pause-menu entry, level 2, `chunkCapacity` per preset. Accepted when: headless run per preset: slots, RSS, timers, climb; validation exit 0. GTX 1650 frame time and the menu are confirmed by the owner. Effort 3-5 days (e). | Not started | - | - |

The acceptance criteria of stages 1-3 as planned are in [before-rewrite-2026-11-18.md](../history/view-distance/before-rewrite-2026-11-18.md)
("Stages"); what was measured against them is in each stage's history file. Their checks stay in the verify gates
(`--climb`, `--fly`, `--voxel-size 0.1` / `0.15`, `--probe-blast`).

## Rejected alternatives

- **More rows on a camera-centred window** (for example 48 x 16 x 48): still unloads on a 50 m climb and in fly mode,
  and doubles the dense cave chunks: over the store. It raises a number instead of fixing the cause.
- **Terrain-following column bands** (each column its own y window): the tracer would read a per-column base at every
  step and lose the integer DDA in y, edits would have to move bands, and digging below a band has no answer. The
  only gain is cell memory, which is 2-3 MiB (c). Columns are used where they pay: classification and the ray skip.
- **Deep fog as the vertical limit**: hides the valley from the peak, which is the view the owner asked for.
- **Dropping Packed bytes and regenerating on reveal**: a blast would wait for generation (unmeasured, est 0.5-2 ms
  per chunk) or be applied late, which breaks the in-order edit stream. Kept as fallback only for unmodified chunks
  beyond 48 m if the measured RLE pool exceeds 96 MiB.
- **0.15 m voxels**: 1.5x only, furniture breaks. **0.1 m with level 1 at 0.2 m**: exactly 2.0x, no content change;
  close second and the answer if the owner rejects the 0.125 look. **Voxel size per preset**: worlds would differ.
- **Bigger level-0 window** (80-96 wide): 22 000-32 000 slots (e). **Height-map far field, hollow rings, cone LOD**:
  rejected in lod.md, reasons unchanged.

## Open questions

The questions as put to the owner, with the answers, are in
[owner-questions-2026-11-11.md](../history/view-distance/owner-questions-2026-11-11.md); questions 1, 2 and 5-7 are
answered (header, "Decided"). Still open (numbers kept):

3. **Other players beyond 96 m**: dynamic objects are drawn inside level 0 only. Recommended: draw replicated players
   at any distance (a few instances, est. small cost). Matters only with stage 6.
4. **UHD 620 default preset**: Low (192 m) if stage 6 meets its gate, otherwise Minimal with Low opt-in. (The
   owner's "sweet spot" remark points to Minimal.)

## Open points

- **Recentre time.** The gate of 0.2 ms per one-chunk step is met at the median only: 0.16 ms median, p90 0.35 ms of
  thread CPU time (release, a climb); logged wall-time maxima 0.5-3.2 ms climbing and 1.3-10.5 ms in `--fly` under
  full worker load (the slab's cell work). Next steps would be skipping the physics change list for rock cells or
  spreading the slab over two frames (perf.md X2 is the first).
- **No negative test for `unloadsInsideRadius`** on a horizontal window move (there is no test hook to misplace the
  window); the climb test covers the vertical case.
- **Owner-only checks**: windowed climbing and the streaming look (no dark halos, no see-through flashes), windowed
  play at 0.125 m. Results go to the game's `docs/owner-checks.md`.
- **Plugged cave mouths** beyond 16 m until stage 5 (accepted by the owner).
