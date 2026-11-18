# Shred history: stage 4 (the cell graph) as built and reviewed

Moved verbatim from `docs/design/shred.md` lines 575-693 and 734-744 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

Current text: `docs/design/shred.md` Part 4, "The cell graph as built"; stage row [shred-4]. Commits: engine
`f83c6d6`, game `5a9a4eb`.

---

<!-- source lines 575-693 -->

### Stage 4 as built (2026-11-12)

The owner answered open question 4 with yes: the cell graph replaces the voxel island search **before** networking
stage 3. Built as designed above unless this table says otherwise. Reviewed by three readers ("Review of stage 4" below).

| Item | As built | Why / what later stages must know |
|---|---|---|
| Cell format | `vortex.voxel:structure` (`voxel/Structure.cppm`, `internal/Structure.cpp`). A **node** is one 6-connected component of solid, not ignored voxels inside one 8^3 cell; a `ChunkGraph` holds every node of a chunk's 64 cells (`GraphNode`, 72 bytes: 6 face masks of 8 x 8 bits, `voxels`, `mass`, `firstVoxel`, cell-local bounds, its links) and its links inside the chunk (`GraphLink`: node, face, `bond`). Nodes are ordered by cell, then by first voxel in scan order; links by face, then node. collapse.md's fixed 32-byte `SupportCell` (8 nodes, node 7 over-connects) is **not** used. | The owner's forward requirement (a whole tower on a 4-voxel bridge must not hold, stage 7) needs per node its voxel count and an integer mass, and per link its bond: `GraphNode::voxels`, `GraphNode::mass` (sum of `densityQ` = material density in kg/m^3, rounded; kg = mass x vs^3), `GraphLink::bond` (voxel faces shared, 1..64) inside a chunk, `BondVoxels(a, b, face)` = popcount of the two face masks across a chunk face. A load check can walk the cached graph without rebuilding anything. `Structure.NodesCarryMassAndLinksTheirBond` (a 2 x 2 iron neck under a stone block: bond 4). |
| Over-full cells | **Measured, then decided: exact, no merge.** Count over the game's terrain at 0.125 m (`Structure.DISABLED_GraphBuildCost`, a 12 x 8 x 12 window with packing): 22 cells with more than 8 components in 1301 Dense and Packed chunks; in the headless runs 0 at the probe, the castle and `--climb`, 3 on the first tower shot (crater crumbs); random noise volumes in the tests: hundreds. They are rare, but they sit exactly where blasts leave crumbs, and the merge would leave those crumbs hanging. Since a node list per chunk costs nothing extra for a normal cell, every component is its own node: the design's "exact per-voxel search for those cells" is done once when the chunk graph is built, not per search. `overfullCells` is still counted (graphs a search used) and logged. | The graph equals voxel 6-connectivity **everywhere** (`Structure.GraphEqualsLabelComponentsWithoutOverfullCells`, `...InOverfullCells`), so results do **not** change in over-full cells. They can change only where a work budget runs out (next rows). |
| Liquids, Uniform, Packed | Solid = id != 0 and not ignored (`GraphMaterials`: `MaterialFlag_Liquid`, as the island search). Each 4^3 block is read once: a block of one id costs no per-voxel work; otherwise one table lookup per voxel sums the density and says whether anything was ignored (if not, the occupancy mask is the solid mask). A Uniform chunk copies a prebuilt full-chunk graph (a uniform liquid chunk has no nodes), Empty has none, Packed is decoded once (`ReadChunk`). The cell flood is bitwise: z slices of 64 bits, an occluded (Kogge-Stone) fill per slice, a worklist of slices. | Measured build per chunk (quiet, -O2, game terrain 0.125 m): Dense 26-33 us (16.5 us with the chunk in the CPU cache), Packed 32-38 us (decode included), Uniform 1.1-3.9 us, Empty 0.1 us. The design estimated 20-50 us. |
| Cache and revision | `World::ChunkRevision(chunk)`: a per-World counter given to a chunk whenever `QueueChange` reports it (every edit through `EditRegion`: game, editor, `FillBox`, replicated ops; and loads), when it unloads, is re-targeted or placed by the column pass; 0 = not loaded; never reused; packing / unpacking keep it. `voxel::TerrainGraphCache` (owned by `sim::Simulation`, `TerrainGraphs()`) keeps one graph per chunk with the revision it was built from and rebuilds a chunk on the first lookup after its revision changed - lazily, only chunks a search reaches. Dense and Packed chunks own their graph; every Uniform chunk of one id shares one graph (and every empty or uniform-liquid chunk the empty one), built once per material table (review: each Uniform chunk held a 6.6 KB copy before). Graphs keep no vector growth slack. At most 4096 entries: when a search starts over the cap, entries whose chunk changed or unloaded go first, then the least recently used, down to 3/4 of the cap; cleared when the World or the material tables change. **Measured memory** (`TerrainGraphStats::bytes`, logged in the `Blast:` line; `DISABLED_GraphBuildCost`, game terrain at 0.125 m): 5.2 KB per Dense graph, 6.5 KB per Packed one, 1.3 KB per chunk over a whole full-height window (most chunks are Uniform or empty), so at most about 27 MB at the cap if every entry were a surface chunk, about 5 MB for a typical mix. | Which graphs are cached never changes a result (`Structure.RefreshEqualsRebuild`: random carves, fills, liquids and ClearMasks over terrain with Packed chunks; the kept cache equals a fresh one chunk for chunk and rebuilt exactly the changed chunks; `Structure.EvictionNeverChangesAGraph`: a 64-entry cache over a 704-chunk window, edits between passes, eviction exercised, every graph equal to a fresh build, the byte count equal to what the graphs hold). **Stale graphs (review):** a re-labelled island node whose voxel count, face masks or bounds differ from its graph (an edit path that missed the revision) is counted (`relabelMismatch`), nothing of it is written (before the review it wrote outside the island volume: heap corruption), the cache is dropped and the search runs again on fresh graphs (`staleGraphRetries`, an error line); if that still differs no island is returned. `Structure.StaleGraphNeverDetachesAttachedVoxels` (a test hook keeps a graph from before a 1-voxel bridge: before the fix 68 attached voxels were detached). Only island nodes are checked: a stale attached neighbour is not seen, the revision rule stays the guard. |
| Search | `voxel::SearchIslandsOnGraph` (GPU-free, no world change), same boxes as stage 1: the island box (rows below a full-height window cut off) and the flood box. (1) Seeds = nodes whose bounds meet the island box (chunks of the box must be loaded, as before, or the search is refused: offline deferral / server error unchanged). (2) Box components = seeds joined by links between seeds (union-find; no chunk outside the island box is read). (3) A box component over `kMaxIslandVoxels` (whole-node count) is attached (`faceOversize`; `oversizeIslands` if it lies inside the box interior: exact). One inside the box interior is a whole component: an island without a flood. (4) The others are flooded **smallest first** (whole-node voxel count, ties by first seed), FIFO over nodes, faces -x +x -y +y -z +z, links in their order: a node with a voxel on the flood box face layer (or on the window's bottom row) ends the flood (`attachedByFace`), so does passing `kMaxIslandVoxels` voxels (`attachedByLimit`), reaching a node an earlier flood gave up on (`attachedByKnown`), the budget (`attachedByBudget`) or a chunk that is not loaded (offline: face islands withheld, the search waits for that chunk; server: `attachedByUnloaded`, an error). (5) Only island nodes are re-labelled per voxel (`NodeVoxels`: the cell flooded again from the node's first voxel) to build the island volumes. (6) Islands are placed where the voxel search puts them: inside the box interior by first voxel in box scan order; across the box face by their smallest in-box component (`LabelComponents` on the island's in-box voxels, only when two or more must be ordered), ties by its first voxel. | A node touching the flood box face layer is attached because any voxel path from the interior to outside the box crosses that layer, so the voxel flood would have reached the face too; nodes off the face read no chunk outside the flood box (`Blast.FaceFloodIndependentOfLoadingOutsideTheFloodBox` holds for the graph counters too). Equal voxels in the flood box give equal islands in equal order, whatever is cached or loaded outside it. |
| Budget (R5) | `sim::kGraphNodeVisitBudget` = 32 768 node visits per blast (e: a flood gives up at 60 000 voxels, about 120 nodes of solid cells, about 940 of 1-voxel walls). The stage-1 budget (240 000 voxel visits) applies to the voxel search only. | **The only place results may differ from stage 1:** where either budget runs out. `Blast.SmallIslandProvenBeforeTheBudgetIsSpent`: the voxel search spends its budget on the fourth slab; the graph gives up on all four slabs by limit after about 500 node visits, far inside its budget - the operations are the same. `Structure.GraphBudgetIsCountedInNodeVisits` reaches a small budget (150) at visit 151, the small island still first, with every counter a budget may read equal cold vs warm cache (same world, three workers) and one vs three workers (both cold); only `graphsBuilt` differs. **Also differs (review):** when a chunk of the flood box is not loaded, the two searches flood in another order, so offline one may defer (`complete` false) while the other returns its face islands, and on a server one may count `attachedByUnloaded` where the other counts `attachedByFace` (both attached: the same operations). A flood that closes reads the same chunks in both. `checkIslandSearch` compares only in-box islands then. |
| `sim::Blast` | `SimulationConfig::islandSearch` = `IslandSearchKind::Graph` (default) or `Voxel` (the stage-1 search, kept as the reference); `checkIslandSearch` (tests) runs the other one on the same voxels before anything is applied and counts differences (`Simulation::IslandSearchCheck()`, `IslandSearchStats::referenceMismatch`, `BlastResult::reference` = the other search's counters, `BlastTimes::referenceMs`). Emission is unchanged: carve; in-box islands; face islands; one `ClearMask`. `IslandSearchStats` gained the graph counters (`graph`, `seedNodes`, `boxComponents`, `graphFloods`, `attachedByFace`, `nodeVisits`, `chunkGraphs`, `graphsBuilt`, `overfullCells`, `relabelledNodes`, `outsideBox`), `BlastTimes` `graphBuildMs`, `graphFloodMs`, `relabelMs` (parts of `searchMs`). The `Blast:` line logs them. | No `WorldOpKind`, `Detach` / `ClearMask` or wire change; `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` and `Blast.FaceFloodTwoSimulationsEqualHashes` green. `faceCandidates`, `floodVisits` and `boxVoxels` are voxel-search counters (0 for the graph). |
| Game | `--after-fill` counts the `--probe-blast` / `--tower-blast` / `--castle-blast` script frames from `World: window filled` (costs on a CPU no longer shared with generation; run with `--frames 700` or more). The tower and castle log lines print the graph counters. | |

**Tests** (12 new, 477 in all after the review, 3 of them disabled measurements). VortexCoreTests (`tests/core/StructureTests.cpp`):
`Structure.GraphEqualsLabelComponentsWithoutOverfullCells`, `GraphEqualsLabelComponentsInOverfullCells` (random boxes
and noise over 2 x 2 x 2 chunks with water: the same partition as `LabelComponents`, node voxels / first voxel / mass
re-checked, link bonds = face masks), `UniformAndEmptyChunks`, `NodesCarryMassAndLinksTheirBond`,
`ChunkRevisionChangesWithEveryContentChange`, `RefreshEqualsRebuild`, `EvictionNeverChangesAGraph` and
`StaleGraphNeverDetachesAttachedVoxels` (review), `GraphNeverDetachesWhatTheVoxelSearchKeeps`
(built-in terrain at 0.5 m with and without Packed chunks, full height, 80 searches after blasts at random pillars,
arms, roofs, noise and water: no voxel the voxel search keeps is detached, and where no budget ran out the same
in-box and face islands in the same order - 926 islands, 138 across the box face, 0 of 80 searches with a budget
spent; over-full cells summed per search 320, a cell repeating across searches; the subset check is skipped where the
voxel search ran out of budget),
`GraphBudgetIsCountedInNodeVisits`, the disabled `DISABLED_GraphBuildCost`. VortexSimTests:
`Structure.GraphFloatingEqualsFaceFlood` (every stage-1 case - pillars, ledge, ground, arm, slabs and small island,
plank arm, forest, two legs, 60 000 / 60 001 voxels, water channel, oversize island, full-height bottom, server and
offline unloaded chunk with the late search - in two simulations, graph and voxel: equal operations per result and
equal hashes). Besides, every `Blast.*` and `Rubble.*` arena (`FaceFloodTests.cpp`, `BlastTests.cpp`,
`RubbleTests.cpp`) now runs with `checkIslandSearch`, fails on any difference and checks that every search it ran was
compared (`IslandSearchChecks::run`): every stage-1 test compares the two searches on its own voxels, except face
islands where a work budget ran out on either side or a flood met an unloaded chunk (those cases are compared op for op
in `GraphFloatingEqualsFaceFlood`). Stage-1 asserts on voxel-search counters read `BlastResult::reference`; graph
counterparts were added where they mean something (limit / face, node visits equal with 1 and 3 workers, one flood for
two legs). Order of work, plainly: the tests were written after a first draft of the search, then each was checked
against sabotaged builds - floods disabled together with revisions ignored by the cache (3 core tests failed:
`RefreshEqualsRebuild`, `GraphNeverDetachesWhatTheVoxelSearchKeeps`, `GraphBudgetIsCountedInNodeVisits`; 15 sim tests,
`GraphFloatingEqualsFaceFlood` and the checked stage-1 tests among them), a cell flood that does not cross z slices
(5 core tests failed) - and passes on the real one.

**Measured** (-O2, UHD 620 laptop). Quiet: `Blast.DISABLED_CostOfTheTowerAndCastleCases`, 0.125 m, 1.2 m power-6
shots, three fresh worlds per row, on two grounds: **flat** (`CreateFlatGenerator` with FillBox structures: Uniform
chunks and one-id blocks, the build's fast paths) and **game terrain** (`CreateTerrainGenerator`, Packed chunks on:
caves, ores, mixed blocks). Since the stage-4 review the graph search runs in one simulation and the stage-1 voxel
search in a second one with the same voxels and shots (`checkIslandSearch` off), so neither warms the CPU cache for
the other; both end with equal `WorldHash`. (The first table, before the review, ran both in one simulation on flat
ground only: its cold numbers flattered the game.)

| Case (all cold = first shot at a site) | Flat: graph search per cast (builds) | Game terrain: graph search per cast (builds, graphs built) | Voxel search, flat / terrain |
|---|---|---|---|
| Underground 1.5 m, 6 m under the surface | 0.28-0.34 ms (0.17-0.20) | 0.70-0.83 ms (0.59-0.73, 27 at 22-27 us) | 14.6-19.2 / 8.9-12.4 ms |
| Watch tower ring shot 1 (cold) | 0.18-0.21 ms (0.12-0.14) | 0.58-0.60 ms (0.43-0.46, 21 at 21-22 us) | 5.6-10.5 / 3.7-4.2 ms |
| Watch tower ring shots 2-6 | 0.06-0.16 ms | 0.13-0.40 ms (3-14 built, 6-49 us each) | 3.7-7.1 / 3.3-6.6 ms |
| Watch tower, the freeing shot | 0.22-0.26 ms (8516 voxels, shot 7) | 0.22-0.31 ms (9506 voxels: on the terrain the tower falls at shot 3, the ground there is higher) | 5.3-5.5 / 4.3-6.6 ms |
| Castle curtain / keep / corner tower (cold) | 0.27-0.72 ms (0.22-0.58, 19-27 built) | 0.34-1.00 ms (0.29-0.88, 20-49 built at 15-21 us) | 3.6-5.8 / 4.2-8.5 ms |

Graph builds on game terrain cost 15-27 us per graph in a quiet process (up to 49 us when only a few are built: the
per-call overhead of a cold cache line), against 8-14 us on flat ground. `Structure.DISABLED_GraphBuildCost` (a whole
12 x 44 x 12 full-height window): Dense 25-28 us, Packed 20-23 us (decode included), 16.6 us with the chunk in the
CPU cache; Uniform and empty chunks share a graph per id (no build).

Game, headless, validation on, after `World: window filled` (`--after-fill`), two runs each (before and after the
review fixes, which do not change the work; the spread is run to run): `--probe-blast` 1.19-1.82 ms per cast (18
graphs built at 58-90 us each); tower ring shots 0.24-2.91 ms (cold shots 0.77-2.91 ms: 13-21 graphs at 26-105 us
each; cached 0.24-0.75 ms), the freeing shot 0.33-0.36 ms (freed by shot 7 of 7); castle first shots 0.50-3.66 ms
(27-45 graphs at 16-70 us each). **The ramp-up check:** `--probe-blast --castle-blast` blasts the untouched probe site,
then the castle sites 10, 20 and 30 frames later: 90 us per graph at the probe, then 24-32 us at the castle; the castle
alone gave 27-70 us per graph for its first site in two runs. So the in-game per-graph cost varies 3-4x from run to
run for the same chunks and is highest for the first blast after the window filled, but it is not simply cold memory
(the quiet game-terrain build is 15-27 us) and not proven to be clock ramp-up: est. power-state / frequency effects of
the laptop with the GPU working, unmeasured. While streaming: the `--climb` blast 4.75 ms, of which 4.32 ms build 36
graphs (120 us each; 4.0 ms in an earlier run).

**Against the targets (corrected in the review):** the 3 ms rule holds on a quiet CPU (worst 1.00 ms on game terrain,
cold graph builds included) and in game after the window filled in these runs (worst 3.66 ms once - over 3 ms: the
first castle shot of one run, 45 cold graphs at 70 us; otherwise at most 2.91 ms), **but not while the world streams**
(`--climb`: 4.0-4.75 ms, all of it cold graph builds) **and not as a bound**: graph builds have no work budget (the
budget is node visits, `kGraphNodeVisitBudget`), and a cast can build up to the island box's chunks (up to 4^3 = 64 for
a 1.2 m blast) plus any chunk of the flood box a flood reaches (up to 7^3 = 343): 64 cold chunks at the in-game 70-120
us is 4.5-7.7 ms. Stage 3's work budget must therefore charge `chunkGraphs` (chunk graphs used: a function of the
voxels, unlike `graphsBuilt`, which depends on the cache's history and must never decide a tick) at the measured cold
in-game price per chunk (networking.md "Changed by shred.md stage 4"). **"Island search <= 0.5 ms optimised at the
probe blast" holds for every blast at a site whose graphs are cached (refresh of the edited chunks included), not for
the first blast at an untouched site** (1.2-1.8 ms in game, mostly cold graph builds). Building graphs ahead of the
first blast - on a worker when a chunk near the player loads or changes, the graph being a pure function of the chunk -
would remove that and the budget charge; not built, for the owner to decide. Its memory cost: the game window is 48 x
44 x 48 chunks, about 9 Dense / Packed chunks per column on this terrain, so about 21 000 graphs at 5-7 KB = about
125 MB, far over today's 4096-entry cap (about 27 MB at most); prebuilding only near the player (say 16 x 16 columns:
about 2300 graphs, 15 MB) fits under the cap.

### Review of stage 4 (2026-11-12)

Three reviewers read the uncommitted stage 4. What was changed (failing test first where it was a bug):

| Finding | Verdict | Change |
|---|---|---|
| A stale graph only logged; its islands were still emitted | Real, and worse: the re-label wrote voxels outside the island volume (heap corruption) | Island nodes are checked (count, face masks, bounds) before anything is written; on a mismatch the cache is dropped and the search runs again (`staleGraphRetries`), no island if it still differs. `Structure.StaleGraphNeverDetachesAttachedVoxels` failed first (68 attached voxels detached, then a double free). |
| `graphsBuilt` among the "same on every machine" counters; networking.md wanted the cast budget to count it | Real (two reviewers) | `graphsBuilt` and `chunksDecoded` documented as logged only, never a budget input (`Blast.cppm`, `Structure.cppm`); the stage-3 budget charges `chunkGraphs` at a cold price instead; `GraphBudgetIsCountedInNodeVisits` shows `chunkGraphs` equal cold and warm. |
| Cold graph builds have no budget; the 3 ms rule text contradicted its own numbers | Real (docs) | Targets above rewritten; networking.md gives the cold worst case. The budget charge is stage 3's work. |
| The quiet benchmark ran on flat ground with the reference search warming the cache | Real | Benchmark on game terrain too, reference in a second simulation; tables above. |
| Cache memory unmeasured; Uniform chunks held full copies; vectors kept growth slack; stale entries stayed | Real | Shared Uniform / empty graphs, no slack, `TerrainGraphStats::bytes` (logged), eviction drops changed / unloaded entries first; measured above. |
| Ordering two face islands scanned each island's whole bounding box | Real (cost) | The scan is clamped to the island's in-box bounds. |
| shred.md still said results change in over-full cells | Real (docs) | Design text marked superseded; row 4 corrected. |
| Offline deferral and the attach reason may differ between the searches when a flood box chunk is not loaded | Real (docs, check) | Stated in `Structure.cppm`, `Simulation.cppm`, `Blast.cppm`, networking.md; `checkIslandSearch` compares only in-box islands then. No test pins the difference (both are correct). |
| `GraphNeverDetachesWhatTheVoxelSearchKeeps` asserted the subset when the voxel budget had run out | Real (test) | Guarded; trials with a budget spent counted and printed (0 of 80 today). |
| LRU eviction never exercised | Real (test) | `Structure.EvictionNeverChangesAGraph` (64-entry cache, 704 chunks, edits; byte count checked). |
| Link symmetry claimed, not checked | Real (test) | The partition tests check every link's reverse and the (face, node) order. The library's cross-chunk lookup (`AcrossChunkFace`) is still covered only by the search-equality tests. |
| Arena checks could pass without comparing | Real (test) | Arenas check `IslandSearchChecks::searches == run`; doc wording narrowed. |
| Budget test mixed cache state and worker count | Real (test) | Cold vs warm in one world, 1 vs 3 workers both cold, every budget counter compared. |
| Over-full counts summed per search read as distinct cells | Real (docs) | Reworded. |

**Not done / not confirmed:** windowed play (the probe chair and tower falling) is for the owner; the load check the
stored masses and bonds are for (stage 7) is not built; graph builds are not yet charged to a work budget (stage 3,
`chunkGraphs` at a cold price); graphs are not prebuilt (owner question above); a stale attached neighbour of an
island node is not detected (only island nodes are re-checked).

<!-- source lines 734-744 -->

### Optional later: load flow (stage 7, owner decision) - resolved by stage 4b

The owner decided (2026-11-12) that thin necks must break now. The per-node mass, per-face bond area and integer
compression / shear / tension capacities per material are built by **stage 4b** (Part 4b) - not as a load flow over a
support-distance DAG (judged there: its load split is a heuristic that can fail rock near a crater) but as a
max-flow check whose released set is exact and unique. What stays optional for stage 7, only if play shows the need:
a bending moment and a tip check of the centre of mass per layer (cantilevers, half-cut beams, leaning towers), necks
that narrow sideways inside one cell, and the spawn guard's "reinforce and report" of overloaded parts (stage 4b's
open question 6: an editor neck report). Re-checking structures away from a cast is stage 5's dirty boxes calling the
stage-4b check.
