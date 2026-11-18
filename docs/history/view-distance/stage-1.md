# View-distance history: stage 1 as built, its acceptance and review fixes

Moved verbatim from `docs/design/view-distance.md` lines 278-383 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

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
- `RegionDesc.heightRange` / the `Welcome` voxel size for sessions (networking stage 3) are not wired; `WorldConfig::heightRange`
  is the hook. *Wired in networking stage 3 (2026-11-13): `sim::RegionWorldConfig` gives server and client the same
  voxel size (raw bits from `Welcome`), window and full-height range (`kRegionHeightSnap` 16 m); the client refuses a
  world built otherwise. Measured on `arena250.session` (250 m at 0.125 m, packed): preload 9.6-10.4 s against the
  10 s gate, 48.5 MiB of RLE against 150 MiB.*
- The `dev` build directory has no optimisation flags (above); a reconfigure with
  `-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG"` would fix it (all CPU numbers in earlier docs are unoptimised).
