# View distance: full height, at least 2x across, voxels no more than 1.5x larger

Status: **stage 1 built** (2026-11-11, see "Stage 1 as built" at the end for what differs from this text and every
number measured); stages 2-7 not started. Owner's requests (2026-11-11): render distance at least
2x, voxels may grow up to 1.5x, furniture stays buildable; then "when i lifting uphill all chunks unload util it
reaches chunk where player is stands" and "vertical render distance might be almost unlimited, or add deep fog".
Numbers are **(m)** measured (quoted from `ROADMAP.md` / `CLAUDE.md`), **(c)** computed from sizes, **(e)** estimate.
Code claims were checked against the working tree on 2026-11-11 (M1 stage 2 in progress). This document rebases
[lod.md](lod.md) to a new base voxel and a full-height window; lod.md's other mechanisms are kept.

## Decision in brief

1. **No vertical limit inside the view radius.** The window covers a fixed height range (the generator's clamped
   terrain bounds plus a 32 m build/flight headroom: -48..128 m for the default terrain, or the region's
   `heightRange` in a session) and follows the focus in x and z only. The top of the range is a declared world-space
   build ceiling: an edit or model above it is refused whole with a reason, never cut silently. Climbing, flying and digging never move it vertically, so nothing inside the view radius unloads. This is
   **stage 1**, at today's 0.1 m voxel, before anything else in this plan.
2. **Air and solid rock stay free.** Empty and Uniform cells keep no slot, no GPU bytes and one tracer step. New: a
   per-column pass classifies sky and deep rock without a generation job per chunk, and a ray above the terrain
   steps once per column (column-top table) instead of once per chunk.
3. **Only visible or near chunks are dense on the GPU.** New cell state `Packed`: dense content no ray can reach,
   farther than 16 m from every player. It keeps its canonical RLE bytes on the CPU (the region codec), no slot, and
   is written to the grid as uniform (opaque). An edit decodes it in the same `Update`; nothing waits on generation.
4. **Fog only at the horizontal edge**: a fade by horizontal distance over the last 15 % of the view radius. No
   vertical fog limit; the existing atmospheric fog stays a look setting, not a range limit.
5. **Voxel size 0.125 m** (1.25x today); **2x comes from lod.md cascades**: level L is a 48-chunk-wide window at
   `0.125 * 2^L` m, and every level spans the same height range, so level boundaries are horizontal only.
6. **View distance is a preset, voxel size is not** (client and server must generate the same world).

## What limited the height today

| Suspect | Finding in the code | Verdict |
|---|---|---|
| Window placement | `World::Update` sets `newMin = focusChunk - windowSize / 2` on all three axes (`World.cpp:106-107`) and the focus is the camera (`Engine.cpp:209`). The game window is 48 x 8 x 48 = 25.6 m tall (`main.cpp:606`). Climbing 3.2 m retargets a whole 48 x 48 slab: the bottom row unloads (`Recenter` -> `UnloadCell`), the new top row is sky. | **The hill bug.** Fixed by a fixed vertical range, not by more rows. |
| Unseen cave chunks | Caves reach 30 m below the surface (`caveMaxDepth`), dark stone starts 12 m down; such chunks are Dense. At full height: about 2304 columns x 3-5 = 7000-11 500 extra dense chunks (e) on top of about 4000 surface chunks (m: "about half" of 8100) -> over the 12 288 slots. | **The real reason for 8 rows.** Fixed by `Packed` (no slot for what no ray reaches). |
| Generation and integration | Every cell gets a job (`SubmitJobs`), every result counts against `maxIntegrationsPerUpdate` = 64, Empty ones included; load order is 3D distance. A taller box multiplies jobs for sky and rock. | Fixed by the column pass and a dense-only integration budget. |
| GPU grid | Grows with `bit_ceil` (`Renderer.cpp:979`); the whole grid is re-staged on every revision (`:992`), about once per frame while streaming. | Not a limit; 495 KiB per copy at 55 rows (c). Fixed by dirty 4 KiB pages. |
| Toroidal indexing | Positive modulo, any size (`World.WindowSizeNeedNotBePowerOfTwo`). | Not a limit; with a fixed y range the vertical modulo is unused. |
| Tracer | `VX_MAX_GRID_STEPS` 512 vs at most 48 + 55 + 48 = 151 cells per ray (c). But every air cell is one step: a sun ray climbing to the ceiling crosses up to 40 more cells (c). | Steps are fine; the per-cell air cost is fixed by the column-top skip. |

## Vertical design

**Fixed range.** `IWorldGenerator::VerticalBounds()` (metres, conservative; default: unknown -> the old behaviour).
For `TerrainGenerator`: low = base - continent - hill - detail - `caveMaxDepth` - 1 voxel = -46.5 m, high = base +
continent + hill + mountain + detail + `kMaxTreeHeight` = 84.9 m (c, from `HeightMeters`). `Fbm` and `Ridged` divide
by the amplitude sum (`Noise.cppm:92-129`), but the 2D simplex `Sample` (`70 * n`) has no proven bound, so sampling
cannot pin it: `HeightMeters` **clamps** to the declared terrain bounds (goldens change once, in the stage-1 commit;
`GoldenNoiseBits` must not), and a debug assert checks every height tile's min/max. Then a **build headroom** of 32 m
(`WorldConfig::buildHeadroom`; a region may declare its own ceiling) is added on top, and the range is rounded
outwards to the coarsest level's chunk (16 m at level 2): **-48..128 m = 55 rows at 0.1 m, 44 rows at 0.125 m** (c).
The headroom rows are sky and cost a cell (28 B) and a grid word each, nothing else. Above the ceiling is outside the
world: `ApplyEdit` / model placement / island spawning that would write there are refused whole
(`EditReport::outOfBounds`), and debris thrown above it is clamped by a kill plane, with a test (model at ceiling + 1 m
refused, world unchanged). A sparse overflow above the top row was considered and rejected: the tracer would need a
second structure for a case the arena ceiling forbids anyway. A session uses
`RegionDesc.heightRange` (same rounding) on server and client. Rows are capped at 128 (`Verify`).

**Column pass.** `IWorldGenerator::ClassifyColumn(cx, cz)` returns the chunk rows above which the column is Empty and
below which it is Uniform (with the id), computed from the height tile the generator builds anyway. `Generate`'s own
early-outs (`Generator.cpp:68-74`) are rewritten to call it, so the two cannot disagree; a test checks it on the
golden probes. A new column costs one job (the tile); sky and rock cells are installed on the main thread without a
job unless the `RegionStore` holds a saved chunk there. As soon as `ClassifyColumn` returns, the column's
`COLUMN_TOP` is set to the band's top row (a conservative bound, never below the eventual content) and every band
cell is written as `VX_GRID_PENDING` (moved from stage 6 to stage 1): the tracer stops on a pending cell and returns
the fog/horizon colour for it, so a half-loaded column is never skipped down to its rock top and never shows sky
through itself. Only the band between gets chunk jobs: about 10-12 per column
at 0.125 m (e; 30 m caves + relief + trees), against 8 jobs per column today.

**Load order and Recenter.** Horizontal distance first, then the surface band before the cave layer, with the 16 m
sphere around every focus point first of all. Integration budget counts Dense results only. `Recenter` becomes
incremental: today it walks every cell and `std::sort`s every Unloaded cell by 3D distance (`World.cpp:144-176`),
which at 48 x 55 x 48 is 127 000 cells per shift. Stage 1 touches only the entering and leaving column slabs
(48 columns x rows per row shifted) and keeps the queue as rings of columns by horizontal distance bucket (each
column holds its band rows surface-first), so a shift re-buckets 2304 columns at most, with no full sort; a teleport
rebuilds the buckets (2304 entries). Shift time is logged and gated.

**Throughput.** Jobs in flight are capped at `max(2, 2 x threads)` (`World.cpp:61`, 8 on 4 cores) and generation
chunks/s was never measured; stage 1 measures it first. Flight at 25 m/s needs about 6 column rows/s, 48 x (1 + 12)
= 600 jobs per row, about 3700 jobs/s (c from (e)). When the backlog of columns inside 0.85 R exceeds a threshold,
edge columns get only their surface band (top two band rows); cave rows are deferred and stay `PENDING` (rendered
as rock-coloured fog only if a ray reaches them, which from the air it does not). The cap becomes a config value
measured against frame time. Gate: headless `--fly` (25 m/s at 150 m) logs `max unfilled columns inside 0.85 R`.

**Packed cells.** A Dense result is installed as `Packed` when it is not visible and farther than `nearRadius`
(16 m = collider radius 12 m + one blast box) from every focus point. Its grid word is `0x80000000 | most frequent
solid id`; its bytes are the `EncodeChunk` RLE (maximal runs, `Region.cpp:39-46`), so saving it is a copy and
`ChunkHash` is computed from the runs without decoding (same run definition). Visible means:
- stage 1 (geometric, per chunk): a chunk is visible if it contains air or is a face neighbour of a chunk that does,
  AND that air is connected to the sky through the column test: some column of its 3 x 3 neighbourhood has its
  surface inside or below this chunk's y span; or it was modified (a dug tunnel stays visible). A ridge flank chunk
  far below a lower neighbour's surface is therefore not made dense just because a neighbour is low somewhere: only
  chunks whose y span reaches the neighbour surface count, plus one chunk under. A cave mouth seen from beyond 16 m
  looks plugged one chunk in. The old estimate (5000-7000 slots) assumed gentle relief; stage 1 gates it on the
  highest ridge within the view radius as well as at spawn.
- stage 5 (exact): flood fill over per-chunk face air masks (6 x 128 B) from sky and edge cells; a chunk is visible
  if it is reached or shares a face with a reached chunk that has air on that face. A Packed chunk is then never
  seen by any ray, and its uniform grid word is exact for geometry and material.

**Reveal and edits.** `Materialize` decodes a Packed chunk into a slot (est 0.05-0.15 ms per chunk). After an edit,
the six face neighbours of every changed chunk are promoted in the same `Update` (a ray leaving a chunk enters a face
neighbour first). Stage 5 continues through connected caves **on a budget**: a reveal queue ordered by air
connectivity distance from the blast and the camera, N decodes per `Update` (N set so decodes stay <= 1 ms), and the
not-yet-promoted chunks stay Packed, which renders as rock until they are decoded. Reveal slots count against the
terrain budget; store misses are logged. Acceptance is "the connected cave is complete within K frames", K measured.
A full store leaves the chunk Packed and reports it `unsynced` (existing contract). Moving players promote chunks entering their 16 m sphere (budget 16 decodes per `Update`), one chunk ahead
of the 12 m colliders; `GuardBodies` still freezes anything over a missing collider.

**Fly mode, high camera, digging.** The window ignores the focus height: a camera at 300 m keeps the same cells,
rays enter the box from above (`VxTraceWorld` already slab-tests the box) and fall to the terrain through the column
skip. Digging down reaches every chunk down to the window floor (-48 m), all already generated (Packed or Dense), so the
blasted chunk and its face neighbours show in the same frame and a deep cave behind it completes within the reveal
budget's K frames. Edits below the floor are `skipped` (existing `EditReport`): the floor is solid by
construction. Load order and the edge fog use horizontal distance, so altitude changes neither.

**Tracer.** Grid encoding unchanged. Added: `VX_GRID_COLUMN_TOP(i)`, one byte per column (48 x 48 = 2.3 KiB) = highest
non-Empty row, maintained by `World::SetGrid`; and `VX_GRID_CONTENT_TOP` = the maximum. In `VxTraceWorld`, a cell
above its column top is skipped to the exit of that column's air prism in one step (exit through x / z, or down
onto the column top), and a rising ray above `CONTENT_TOP` ends. A sky or sun ray then costs about one step per
column crossed, as today. Shared GLSL/C++ code, pinned by the reference-DDA tests on a 48 x 55 x 48 window.

**Fog.** Edge fade = smoothstep(0.85 R, R, horizontal distance of the hit from the camera) to the horizon colour, R =
outermost level's half extent; sky pixels unchanged. Peaks and valleys are clear inside R at any altitude.

## Memory and per-frame cost

| Item | Today, 0.1 m, 48 x 8 x 48 | Stage 1, 0.1 m, 48 x 55 x 48 | 0.125 m, 48 x 44 x 48 |
|---|---|---|---|
| Cells (28 B) + grid (4 B) | 18 432: 504 + 72 KiB (c) | 126 720: 3.4 MiB + 495 KiB (c) | 101 376: 2.7 MiB + 396 KiB (c) |
| Dense chunks (slots, GPU) | about 8100 (m) | 5000-7000 (e): surface + one chunk under + 16 m spheres | 4000-6000 (e); 3500-5000 after stage 5 |
| Packed (CPU RLE, 1-10 KB) | 0 | 9000-14 000, 10-140 MiB (e) | 7000-11 500, 10-120 MiB (e) |
| CPU bytes of dense chunks | 285 MiB (c from 8100) | 175-245 MiB (c from the estimate) | 140-210 MiB |
| GPU store | 433 MiB of 473 MiB total (m) | unchanged | unchanged |
| Jobs per new column | 8 (c) | 1 + 12-15 (e) | 1 + 10-12 (e) |

Per frame on the UHD 620 (all (e), gated in stage 1): incremental `Recenter` touches one column slab per shift
(48 x 55 cells plus a re-bucket of 2304 columns), about 1.5 shifts/s walking: under 0.1 ms per shift (e), gated. Grid upload: dirty pages only, a full copy (405 KiB) only on a shift. Packed decodes:
none at rest; at most 1 ms per blast (gate). Tracer: gbuffer +0-0.3 ms (more pixels hit real peaks instead of sky);
sun shadows +0-0.5 ms (rays now climb to real peaks, and peaks now cast shadows, which is new content). Without the
column skip the same rays would cost an estimated +0.7-2.3 ms, which is why it is in stage 1. Generation throughput
(chunks/s on 4 cores) was never measured: full fill rises about 2.7x in jobs that do real work (e), surface first.

## Voxel size choice

| Object | 0.1 m | 0.125 m | 0.15 m |
|---|---|---|---|
| Chair seat 45 cm / back 90 cm; seat 45 x 45 cm | 4.5 / 9; 4-5 square | 3.6 / 7.2; 4 square, 2 gap | 3 / 6; 3 square, 1 gap |
| Table 75 cm, top thickness | 7-8, 10 cm | 6, 12.5 cm | 5, 15 cm |
| Bookshelf 180 x 80 x 30 cm, shelf pitch 30 cm | 18 x 8 x 3, pitch 3 | 14 x 6 x 2.4, pitch 2-3 | 12 x 5 x 2, pitch 2 |
| Door 200 x 90 cm; player 1.8 m / radius 0.3 m | 20 x 9; 18 / 3 | 16 x 7; 14.4 / 2.4 | 13 x 6; 12 / 2 |

0.125 m keeps every object recognisable, is exact in binary (chunk 4 m, edit quantum 1/16 voxel = 7.8 mm) and puts
the level swap below a pixel: a 0.125 m voxel is 1 px at 47 m, 0.25 m at 94 m (c), level 0 ends at 96 m. 0.15 m
gives only 1.5x and a single voxel of air between chair legs. No size can show a 4 cm leg (open question 2).

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

**World, grid and tracer contracts.**
- `WorldConfig`: `windowSize.y` is replaced by `verticalRange` (metres, from the generator or the region);
  `nearRadius` (16 m); `packHidden` (default false, so existing tests keep their meaning). `Update(focus)` uses x/z.
- `CellState::Packed`; `WorldStats` gains `packedChunks`, `packedBytes`, `unloadsInsideRadius`. `GetChunk` returns
  nullptr for Packed; new `EnsureResident(minChunk, maxChunk)` decodes a box (edits, `ExtractFloatingIslands`, the
  collider streamer). `GetVoxel` reads a Packed chunk from its runs. Packing and unpacking are not content changes:
  `TakeChangedChunks` does not list them, `ChunkHash` / `WorldHash` / saves are byte-identical.
- `IWorldGenerator::VerticalBounds`, `ClassifyColumn` (defaults keep custom generators working, one job per cell).
- Grid encoding and `CellIndex` unchanged; `World` exposes dirty 4 KiB grid pages; the renderer copies those
  (keeping slot -> grid index from its last scan, so an `uploaded` flip dirties one page, not the grid).
- `trace.glsl` + `Trace.cpp`: column-top table and content top (stage 1), level-indexed trace (stage 6).
  `FrameUniforms` + C++ mirror: content top and fog edge in the spare `jitter.zw` first (graphics notes), the lod
  fields later (+144 B, `ContractTests`). New descriptor `u_GridColumns` (names resolved by reflection).

**Voxel size (stage 3).** The game's `config.world.voxelSize = 0.1f` (`main.cpp:605`) is removed; defaults in
`World.cppm:25`, `Generator.cppm:50`, `Scene.cppm:27` become 0.125 (`Simulation.cpp:93-94` already copies one into the
others). Debris thresholds keep their metre volume: `minPieceVoxels` 16 -> 8, `debrisBodyVoxels` 48 -> 25,
`thinBodyVoxels` 6 -> 5, server `replicatedPieceVoxels` 512 -> 262 (c), with a test. Content written in voxels keeps
its metre size (beam, figure, glTF `voxelsPerUnit` = `1 / voxelSize`); the tree-trunk rule (`Generator.cpp:259`) is
rewritten in metres (at 0.125 every trunk would be 1 voxel). Step height 0.35 m climbs 25 cm ledges, not 30 cm.
Region headers record the voxel size and refuse a mismatch. `Terrain.GoldenChunkHashes` is regenerated once in its
own commit; `GoldenNoiseBits` / `GoldenHeightBits` must not change.

**Networking.** (Server numbers below are estimates; M1 stage 3 gates them: preload <= 10 s, RLE pool <= 150 MiB.)
The server window (networking.md, stages 0-8) is already fixed: `playBounds` + 2 chunks, vertically
`heightRange`. Now `heightRange` is rounded to 16 m, and a client's window uses the same range, so the y part of
`WindowUpdate` never changes and a climbing client requests nothing. The server uses `Packed` as plain memory
saving with its own rule (resident within 16 m of any player or edited in the last 10 s (e)): a 250 m region at
0.125 m is (63 + 4)^2 x 44 = 197 516 cells and about 20 000-29 000 dense chunks, 0.70-1.0 GiB if all were resident;
with `Packed` about 3000 resident slots (105 MiB) plus RLE (range too wide to size yet; measured). Preload
generates every dense chunk once at session start; its time is the gate. Collapse connectivity
(`ExtractFloatingIslands`) works on Packed runs directly (`GetVoxel` already reads runs) and only the voxels it
removes are decoded; `EnsureResident` is capped per tick, so a large collapse spreads over ticks instead of
decoding thousands of chunks at once. `Hello` carries the voxel size as exact float bits and a mismatch is
refused. LOD levels are client-only render data; nothing new on the wire.

## Stages

| # | Built | Accepted when | Effort (e) |
|---|---|---|---|
| 1 | **Full-height window at 0.1 m** (the hill fix): measure generation chunks/s first; clamped vertical bounds + 32 m headroom and build ceiling, column pass, `VX_GRID_PENDING` + conservative column top, incremental `Recenter`, backlog-aware surface-first loading, fixed y range, horizontal load order, dense-only integration budget, `Packed` with the geometric rule, decode on edit + face neighbours, column-top skip, dirty grid pages, horizontal edge fog, unload counter. | Unit tests (GPU-free): focus moved valley -> 100 m peak -> valley with a test generator: `WindowMinChunk().y` constant and zero unloads; `ClassifyColumn` equals `Generate` on the golden probes; 10^6 heights inside `VerticalBounds`; two worlds with `packHidden` on / off take the same blasts and give equal `WorldHash` and `GetVoxel`; reference DDA with the column skip on 48 x 55 x 48. Headless **climb**: `--climb` finds the lowest and highest `TerrainHeight` within 70 m of spawn, walks the player valley -> peak -> valley, and logs `Climb: ... 0 unloads inside view radius, window y fixed`; `--camera` screenshots from the peak towards the valley and from 150 m above it show the valley floor and the far side to the fog edge with no sky holes; both standard headless runs exit 0 with `Beam cut: ... 1 new object(s)` and `on the ground`; `store a/b` <= 7000 at spawn **and** with `--camera` on the highest ridge within the view radius (from `ClassifyColumn`); headless `--fly` 25 m/s at 150 m logs max unfilled columns inside 0.85 R (gate set from the measured chunks/s); `Recenter` ms per shift <= 0.2; model at ceiling + 1 m refused; reference DDA on half-loaded columns; gbuffer + lighting within +0.8 ms of the pre-stage run on the same cameras; Packed MiB, decode ms per blast (<= 1) and fill frames logged. Windowed climbing is confirmed by the owner. | 5-7 days |
| 2 | **Voxel preview**: `--voxel-size` flag (game only), furniture probe near spawn (chair, table, bookshelf, door on the 1/8 m grid; shelf pitch 25 or 37.5 cm); log generation chunks/s. | Probe close-up and peak screenshots at 0.1 / 0.125 / 0.15; `store a/b`, Packed MiB and timers per size; probe legs blasted do not turn straight into particles. The owner picks the size. | 0.5-1 day |
| 3 | **Switch to 0.125** (after M1 stage 2): defaults, thresholds, content, tree rule, import, region header, goldens, lod.md rebase, ROADMAP. | ctest green, only `GoldenChunkHashes` changed; both headless runs and the climb pass; timers within 5 % of the stage-2 0.125 run. | 2-3 days |
| 4 | **Distance budgets** (lod.md stage 1). | Lighting -2 ms facing a hillside, no change on a near view. | 2 days |
| 5 | **Exact visibility flood** (face air masks), replaces the geometric rule; reveals continue through connected caves. | Brute-force voxel flood test: no Packed chunk is reachable through air; a blast into a cave 60 m away completes the connected cave within K frames (K measured and logged, decodes <= 1 ms per `Update`); dense slots on the headless walk drop >= 20 % vs stage 4 (log); cave-mouth and surface screenshots identical to stage 4. | 3-4 days |
| 6 | **Level 1 at 0.25 m** (lod.md stages 2-3): second `World`, x/z `windowSnap`, level-indexed trace, uniforms, far lighting, edit downsampling, `--lod`. | Reference DDA across the boundary; terrain slots <= 9000; respawn fill <= 300 frames; peak screenshot to 192 m with no seam at 96 m; low-sun screenshot with a level-1 hill shadowing level-0 ground; UHD 620 gates met, otherwise 40-wide level 1. | 1.5-2 weeks |
| 7 | **Presets and level 2**: `EngineConfig` view preset, `--view`, pause-menu entry, level 2, `chunkCapacity` per preset. | Headless run per preset: slots, RSS, timers, climb; validation exit 0. GTX 1650 frame time and the menu are confirmed by the owner. | 3-5 days |

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

## Open questions for the owner

1. **Voxel size** after the stage-2 screenshots: 0.1 m, **0.125 m (recommended)**, or 0.15 m.
2. **Furniture finer than a voxel?** Recommended: accept blocky furniture now, keep it in world voxels so it collapses
   with buildings, revisit finer-scale props after M1.
3. **Other players beyond 96 m**: dynamic objects are drawn inside level 0 only. Recommended: draw replicated players
   at any distance (a few instances, est. small cost).
4. **UHD 620 default preset**: Low (192 m) if stage 6 meets its gate, otherwise Minimal with Low opt-in.
5. **Step height**: keep 0.35 m (25 cm risers) or raise to 0.4 m. Recommended: keep.
6. **Stage 1 cave mouths**: beyond 16 m a cave mouth looks plugged one chunk in until stage 5. Acceptable for the
   stages in between? Recommended: yes; stage 5 follows the distance budgets.
7. **Build ceiling**: terrain bounds + 32 m (about 43 m above the highest tree-topped peak) as the hard limit for
   building and thrown debris, refused above it. Enough, or does the arena need a higher ceiling (for example 64 m;
   costs only sky cells)?

## Stage 1 as built

Built at 0.1 m voxels in the game (`src/main.cpp`: `fullHeight`, `packHidden`); `WorldConfig` defaults keep the old
camera-centred window, so tests, tools and the session server (`sim::ServerConfig` does not touch these fields)
behave as before. Owner decisions taken: cave mouths beyond 16 m may look plugged; the build ceiling is the terrain
bounds + 32 m.

**Measurement caveat.** The `build/` directory (`dev` preset) compiles with no `-O` flag at all (its cache has an empty
`CMAKE_CXX_FLAGS_RELWITHDEBINFO`; a fresh configure gives `-O2 -g`). CPU numbers marked *dev* are unoptimised; CPU
numbers marked *release* come from `cmake --preset release` (`build-release/`, configured offline from the same
dependency sources, no tests). GPU timings do not depend on it, except that the generation threads share the UHD
620's power budget (below).

### What differs from the design, and why

| Design | As built | Why |
|---|---|---|
| Visible: row reaches a 3 x 3 neighbour's surface, **plus one chunk under** | Row `>=` the row of the lowest surface voxel of the 3 x 3 columns (`ColumnClass::surfaceFrom + 1`); no extra chunk | No ray reaches the chunk under the surface except through a cave. Measured at the spawn camera after a full fill: 7574 -> 5509 dense slots (dev). Sky rays from every angle hit 0 Packed chunks (`ViewDistance.SkyRaysRarelyMeetPackedChunks`, about 2000 hits per seed). |
| Horizontal distance first, caves of near columns before far surfaces; surface-only edge columns when the backlog is high | Near sphere first, then classification and the surface band of every column by distance, then caves (`surfaceFirstBacklog`, default 0 = always surface first) | Distance-first put about 10 000 cave chunks ahead of the last edge surfaces: the game start took 25-27 s (dev) instead of 6 s (dev) / 1.3 s (release). |
| Grid `x + z*sx + y*sx*sz` | **Column-major** `(x + z*sx) * sy + y` (World, `trace.glsl`, tests) | A window step re-targets 48 columns of 55 cells; with the old order every cell was 64 KiB from the next (recentre 0.4-0.9 ms in tests). |
| Column tops in a new descriptor `u_GridColumns` | Appended to `u_Grid` (`World::GridWords()`): cells, column tops, content top, **region tops** | No new binding or reflection change; the dirty-page upload covers them. |
| Column-top skip only | Plus a **4 x 4 column region top** (window-relative, rebuilt per window move) | Sun rays climb to the real peaks one column per step; at the spawn, before regions, lighting cost +0.9 ms over the same camera without full height. Steps per ray in the 48 x 55 x 48 test scene: 15.7 (none) -> 8.0 (columns) -> 4.1 (regions), 0 mismatches against the traversal without tables and a voxel DDA. |
| Pending cell returns the fog colour | Grid word `0x80000000` (uniform flag, id 0): the tracer needs no change; `gbuffer_world.frag` writes it as sky with its depth, `World::Raycast` reports no hit | Simplest exact form; lighting already skips sky pixels. |
| Jobs in flight `max(2, 2 x threads)` | `max(2, 8 x threads)` (56 on this laptop) | Jobs are submitted once per `Update`; many results (sky rows, classifications) finish within a frame, so the cap is a per-frame cap. At 14 the climb at 10 m/s left 419 columns unfilled; at 28 the fly left 101; at 56, 0. The queue is shared with collider cooking (up to about 8 ms of delay in release). |
| Debug assert on every height tile | `HeightMeters` clamps to `TerrainSurfaceBounds`; `ViewDistance.TerrainHeightsStayInsideTheDeclaredBounds` samples 10^6 heights | With the clamp the assert cannot fire. Sampled -8.0..59.2 m against -16.35..77.35 m: no golden changed (`GoldenChunkHashes`, `GoldenNoiseBits`, `GoldenHeightBits` all unchanged). |
| Build ceiling = terrain bounds + 32 m | The top of the rounded range: 84.85 + 32 = 116.85 m, rounded to **128 m** | Everything inside the window is buildable; the range is what client and server must agree on. |
| Debris above the ceiling "clamped by a kill plane" | `Scene::DestroyAbove(ceiling)` removes awake dynamic bodies entirely above it; the terrain guard treats the sky above a full-height window as ready | Without the guard change a body thrown upwards froze under the ceiling for good (found by the test). |
| `EnsureResident` for the collider streamer | Physics reads Packed chunks through `World::ReadChunk` (a decoded copy, no slot); `EnsureResident` exists for callers that need resident chunks | Colliders far from the player (thrown rubble) would otherwise cost slots. |
| Focus = every player | One focus (the camera) per `World::Update` | The client has one; the server's own rule is M1 stage 3. |

Also built as designed: `IWorldGenerator::VerticalBounds` / `ClassifyColumn` (terrain and flat generators; `Generate`
uses the same classification), column jobs that check the `RegionStore` for saved chunks in sky / rock rows,
incremental `Recenter` (only re-targeted columns, counting-sort rebucket), Dense-only integration budget, `Packed`
with the RLE of `EncodeChunk` (encoded on the worker), decode at an edit's first changed voxel plus the six face
neighbours of every changed chunk, near-focus promotion (16 decodes per `Update`), `WorldHash` from the runs,
`EditReport::outOfBounds`, dirty 4 KiB grid pages (`GridPageRevisions`, renderer `hiddenAt`), horizontal edge fog
(`jitter.zw`, `RenderSettings::edgeFog`), `WorldStats::unloadsInsideRadius`, headless `--climb` and `--fly`. Memory of
unloaded chunks is released on a worker (it was half of the recentre time).

### Acceptance, measured (UHD 620 laptop, 7 workers)

| Gate | Result |
|---|---|
| Generation chunks/s, measured first (pre-stage, dev) | 6450-6710 chunks/s, 1530-1670 dense/s; full height has 13.7 dense chunks per column near the spawn (`ViewDistance.DISABLED_GenerationThroughput`). |
| Unit tests | 404 in all at the time (412 after the review fixes below), one of them the disabled throughput measurement (`ViewDistance.*`: climb valley -> 100 m peak -> valley with constant `WindowMinChunk().y` and 0 unloads, while the camera-centred window unloads; `ClassifyColumn` = `Generate` on 64 columns x 3 seeds; 10^6 heights; Packed on / off: equal `WorldHash`, edit lists, `GetVoxel`, `ReadChunk`; column skip on 48 x 55 x 48 with half-loaded columns; ceiling; column tops; recentre slab; `Simulation.ModelsAboveTheBuildCeiling...`: model at ceiling + 1 m refused, world unchanged). |
| `--climb` | 329 m spawn -> valley (-2.5 m) -> peak (56.8 m) -> valley, `0 unloads inside view radius, window y fixed`, max 0 unfilled columns inside 0.85 R at 10 m/s in release, 0-1 in the unoptimised dev build. |
| `--fly` 25 m/s at 150 m | Max 0 unfilled columns inside 0.85 R (release, two runs; frames held to real time). |
| Store at the spawn / highest ridge | 5620-5684 at 200 frames (release), 5509 after a full fill (dev, spawn camera); 6567 on the highest point within 70 m after a full fill (release). Gate 7000 met. |
| Packed | 16 958-18 082 chunks in 24.3-24.7 MiB after a full fill (estimate was 10-140 MiB). |
| Fill | Surface around the start: 1.2-1.5 s release (5.8-6.6 s dev); whole window with caves: 680-691 frames, 8-16 s release (23-26 s dev). |
| Recentre per one-chunk step (<= 0.2 ms) | Release, thread CPU time over a climb: median 0.16 ms, p90 0.35 ms; logged maxima 0.49 ms (spawn), 0.62 ms (climb), 1.3-2.3 ms (fly). **Met at the median only**; the rest is the slab's cell work under full worker load. The median and p90 came from separate, temporary per-step instrumentation (thread CPU time); the logged `maxRecenterMs` is wall time of the slowest step and does not reproduce them. |
| Decode ms per blast (<= 1) | 3-5 decodes, 0.28-0.57 ms per carve into a Packed chunk (dev); a decode is 0.016-0.03 ms in release. |
| gbuffer + lighting within +0.8 ms on the same cameras | Fixed spawn camera, window filled, same code with full height off / on: gbuffer +0.07 / +0.08 ms, lighting +0.10 / +0.17 ms. While the caves stream (first ~25 s in dev) the difference is +1.3 to +2.4 ms: the generation threads take the shared power budget. Pre-stage default run (200 frames): gbuffer 8.30, lighting 11.41 ms; stage 1: 8.13-8.93 and 12.69-13.15 ms (still streaming). The pre-stage peak and 150 m cameras showed sky (hill bug), so their timings (7.91 / 1.29, 2.93 / 0.28 ms) have nothing to compare with. |
| Screenshots | From the peak towards the valley: the summit, the slopes and the lowland down to the fog edge, no holes (pre-stage: the summit floating in the sky). From 150 m above the valley: the valley floor, the coast and the plaza to the round fog edge, no holes (pre-stage: nothing). The valley point itself is 130 m from the peak, beyond the 73.6 m view radius: the radius grows only with stage 6. |
| Standard headless runs | Exit 0 with validation, `Beam cut: 37 voxels removed, 1 new object(s)`, `Player feet at ..., on the ground`; the `--editor` run exits 0 (its last-frame player state flips between "on the ground" and "in the air" from run to run, 1 of 4 in the air). |
| Windowed climbing | Not confirmed: needs the owner. |

### Review fixes (after stage 1)

A review of the uncommitted stage found these; each was reproduced by a test that fails without the fix (checked
by disabling the fix), except the shader and renderer ones (GPU, checked by screenshot and timing).

| Finding | Fix | Test |
|---|---|---|
| A chunk at the window's edge was packed without its neighbour column outside the window and never judged again: cliff faces turned into rock cubes after travel. | `ApplyColumnClass` re-judges the Packed cells of its 8 neighbours (`RecheckNeighbours`, unknown columns ignored: they re-check when they arrive); visible ones are queued (`m_Reveal`) and decoded under `maxDecodesPerUpdate`. Climb 424, fly 436-442 reveals (release). | `PackedChunksAreJudgedAgainWhenTheWindowMoves` (rule recomputed from the generator after 9 steps) |
| A chunk loaded after an edited or saved face neighbour could be packed (crater walls after a reload, or a neighbour still loading during a blast). | `Visible` is also true next to a `modified` / `fromSave` face neighbour; a `fromSave` result promotes its Packed face neighbours. | `EditedChunksNeverBorderPackedOnesAfterAReload` (3 visits) |
| Decoded chunks were never packed again: the store grew over a session in one area. A full store installed a saved chunk as Empty and the next edit overwrote the save. | A decoded (or near-installed) buried chunk keeps its runs (`m_Resident`); `Repack` packs it again once the focus is `repackMargin` (8 m) beyond `nearRadius` and it is still hidden, unedited, with no edited face neighbour (at most `maxDecodesPerUpdate` per `Update`, 128 examined). A load that finds the store full is installed Packed (encoded on the main thread if needed), never Empty. | `ResidentBuriedChunksArePackedAgainBehindTheFocus`, `AFullStoreNeverDropsASavedChunk` |
| The island search of a blast silently gave up while cave rows waited behind the surface backlog. | `World::RequestLoad` (boxes loaded right after the near sphere); `sim::Blast` requests the island box and remembers the search; `sim::RetryIslandSearches` (called by `Engine::Run`) runs it once the box is loaded and returns a `deferred` result (no carve, no explosion). | `RequestedBoxesLoadBeforeTheRestOfTheWindow`, `Blast.IslandSearchRunsLaterOnceItsBoxHasLoaded` |
| `PromoteNear` ran only on a focus-chunk change: chunks inside 16 m stayed packed while the focus moved within its chunk. | Scan `nearRadius + h` whenever the focus moved `h` = a quarter chunk; pack only beyond `nearRadius + 2h`. | `EverythingNearTheFocusIsResidentWhileItMovesInsideAChunk` |
| The spatial filter (and the lighting history) took the zero lighting of pending pixels: dark halos while streaming. | Pending pixels write normal alpha 1 (`VX_NORMAL_UNLIT`); both skip such taps from the normal they already read. Composite at the spawn camera: 1.86 ms median with, 1.81 ms without (3 runs each, noise 0.15 ms); a `u_LightCount` fetch per tap had cost +0.3 ms. | screenshot / timing only |
| A dense word whose slot was not uploaded was written as air: loaded cells flashed see-through. | The renderer writes `World::GridFallback(cell)`: what the cell showed before (pending, packed / uniform stand-in, or air). | `GridFallbackIsWhatTheCellShowedBefore` (World side) |
| The dirty-page upload compared the World's address: a new World at the same address kept the old grid. | `World::InstanceId()` (process-unique) is compared instead. | idem (ids differ) |
| `unloadsInsideRadius` was 0 by construction in full height. | Counted in `RetargetCell` by what is lost: a cell with content whose column is within `ViewRadius()` of the focus. The camera-centred case still counts (climb test); a test hook to force a wrong horizontal move was not added. | `ClimbingKeepsTheWindowRowsAndUnloadsNothing` |
| Store-full drops counted as filled. | `storeFullLoads` / `storeFullDecodes` in `WorldStats`, the exit log, `--climb` / `--fly`; 0 in the climb and fill tests. | climb, surface-load tests |
| Height test vacuous after the clamp; `ClassifyColumn = Generate` circular for sky / rock rows. | Sampled extremes must stay 4 voxels inside the bounds (the clamp never engaged); `TerrainSettings::skipClassifiedRows = false` runs the voxel loop for the skipped rows, which must be all air / all dark stone (4 rows x 64 columns x 3 seeds). | `TerrainHeightsStayInsideTheDeclaredBounds`, `ClassifyColumnAgreesWithGenerate` |
| Packed on / off equality: no edit across a chunk seam, 4 of 6 faces, `skipped` not compared. | Corner carves / fills / swaps over up to 8 mixed-state chunks, element-wise edit lists (`newId` too), equal `skipped` / `unsynced`, all six faces. | `PackedAndUnpackedWorldsTakeTheSameEdits` |
| The recentre test did not test its name; column / region tops only checked after a fresh load. | `retargetedColumns` must equal the slab (48, diagonal 95) and the other columns keep their chunk pointers; tops recomputed after a fill above the top, a carve, while streaming and after a 2000 m move; a ray onto a pending column top stops with material 0 (`World::Raycast`: no hit). | `RecenterTouchesOnlyTheEnteringColumns`, `ColumnTopsAndPendingCellsDescribeTheGrid` |

Measured after the fixes (UHD 620 laptop, release unless marked; "before" is the release binary of the stage
without these fixes, same commands):

| Gate | Result |
|---|---|
| Unit tests | 412 (one disabled measurement), all pass. |
| `--climb` (3 runs) | 0 unloads inside the view radius, window y fixed, max 0 unfilled columns (dev build: 2), 0 store-full loads / decodes; recentre max 0.60 / 2.75 / 3.18 ms (before: 0.54 / 3.09 ms; wall time, noisy). Store at the end 4684-4703 (before 5050-5085). 3777-3805 decodes in 104-116 ms over 1977 frames (before 1767-1792 in 41-50 ms): reveals and re-decodes on the way back down the same path; 3544-3572 packed again. |
| Blast 30 m ahead, 6 m deep | 24 decodes in 0.63-0.65 ms (gate 1 ms), whole blast 48-57 ms (before: 0 decodes, the chunks stayed resident since the walk; 47 ms). |
| `--fly` (6 runs) | Max 0 unfilled columns, 0 unloads, 0 store-full; 487-493 decodes (436-442 revealed), 292 packed again. Recentre max (wall time, one step in 120): 1.3, 1.6, 1.6, 3.9, 7.4, 10.5 ms; not compared with the binary before the fixes. |
| Standard headless runs | Exit 0 with validation, `Beam cut: 37 voxels removed, 1 new object(s)`, `on the ground`; store 5600 / 12 288 (dev, 200 frames). |
| Screenshots | From the peak towards the valley (pitch -0.22 and -0.55) and from 150 m above the valley: no holes, terrain to the fog edge. |

### Still open after stage 1

- Recentre p90 / maxima above 0.2 ms; next steps would be skipping the physics change list for rock cells or
  spreading the slab over two frames.
- A blast into cave rows that are not loaded yet (surface first) is `skipped` for its carve (its island search is
  deferred now, the carve is not replayed); the near sphere covers the player's own surroundings, not another
  player's blast 60 m away (a session needs the server's rule, M1 stage 3).
- `unloadsInsideRadius` has no negative test for a horizontal window move (no test hook to misplace the window).
- `RegionDesc.heightRange` / `Hello` voxel size for sessions (networking stage 3) are not wired; `WorldConfig::heightRange`
  is the hook.
- The `dev` build directory has no optimisation flags (above); a reconfigure with
  `-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG"` would fix it (all CPU numbers in earlier docs are unoptimised).
