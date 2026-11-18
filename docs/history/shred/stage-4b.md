# Shred history: stage 4b (weak necks, the load check): judgement, plan, as built, review

Moved verbatim from `docs/design/shred.md` lines 788-810, 1082-1149 and 1258-1397 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

Current text: `docs/design/shred.md` Part 4b; stage row [shred-4b]. Code cites "Stage 4b review": that is
"Review of stage 4b" below. Commits: engine `ce6a6da`, game `305cfc1`.

---

<!-- source lines 788-810 -->

### Judgement of the three designs

| | Cross-section sweep | Load-flow DAG | **Min-cut (winner)** |
|---|---|---|---|
| Tower on a 2x2 neck | falls; layer-exact by per-layer plates | falls; a neck straddling a cell face needs a second round | falls; inside-cell necks by the exact node split (grafted, below) |
| Full base, beams, overhangs | stand | stand | stand |
| Natural rock | never judged ("open" by the flood box face): safest | judged inside its examined set: DAG concentration and weakest-voxel strength can fail rock near a crater (its own F6, F7) | judged only inside the island box, and only where **no** load path fits: connected rock has many paths, so it does not fail |
| Parallel supports | summed at one horizontal layer; mass hanging below the cut counts as support | split by bond along one DAG, a heuristic | exact (max-flow) |
| Bending, sideways load | none (vertical only) | arching by support distance | shear and tension per link; no bending moment |
| Cascades | follow-up checks queued over ticks (new simulation state) | up to 4 rounds per cast | none needed: the remainder is proven to hold |
| Determinism | fixed orders | fixed orders; the answer depends on its DAG rules and remainder split | released set unique whatever the visit order (tested) |
| Cost per cast (e) | 0.03-1.1 ms | 0.02-7 ms | 0.05-0.3 ms typical, about 3-4 ms at the budgets |
| Implementation risk | plates, layer band, epoch union-find, follow-up queue | DAG, plane validity, pseudo-nodes, rounds, brick split | textbook push-relabel, exterior floods, an exact node split; every part brute-force testable |

**Grafted** from the cross-section sweep: the strict rule (load equal to capacity holds), the voxel-size-invariant
integer formula, a bond as strong as the weaker of its two materials, the neck test at every height inside a cell,
"never runs without a cast". From the load-flow DAG: the narrowest plane computed per node when the graph is built (the
bond between two layers, not a slice's area), the pristine-terrain gate test, the `--neck-blast` variants (inside a
cell, on a cell face, the threshold width), and castle bricks as the next step (4c). From the min-cut design itself:
the network, the directional strengths, weightless foliage, the contracted exterior, the budgets. Rejected inside the
min-cut design: its "throat" capacity from slice areas (inexact, and a cap on all flow through a node could fail deep
dirt) - replaced by the exact split; "ties release" - replaced by the strict rule.

<!-- source lines 1082-1149 -->

### Code changes (file:line)

| Where | Change |
|---|---|
| `src/voxel/Material.cppm:37-43`, `internal/Material.cpp:136-155` | `uint32_t strength[3]` (C, S, T; kg/m^2), default stone's; the table above |
| `src/voxel/Structure.cppm:43-55` | `GraphNode`: `cutFaces`, `cutPlane` (bits 0-2 plane, bit 7 anchor), `weak[3]`; fields reordered; `static_assert` 72 bytes |
| `src/voxel/Structure.cppm:91-98`, `internal/Structure.cpp:275-288` | `GraphMaterials`: `strength`, `anchor`; foliage `densityQ` 0 |
| `src/voxel/internal/Structure.cpp:102-248, 290-368` | build: `weak`, anchor bit, plane cuts and areas, halves' connectivity for constricted nodes |
| `src/voxel/Structure.cppm:182-247` | `GraphSearchLimits::neck` (`const NeckLimits*`: `capFace[3][256]` for the session voxel size, the four budgets); `GraphSearchResult::neckPieces`, `neckCuts`, the counters |
| `src/voxel/internal/Structure.cpp:718-1100` | phase 3b; step 4 takes the third list; `GraphTable` gets the exterior label array |
| `src/sim/Blast.cppm:58-108, 169-187` | `IslandSearchStats` neck counters; the four constants |
| `src/sim/internal/Blast.cpp:156-199, 230-357` | `GraphSearch` fills `NeckLimits` from `Materials()` and `World().VoxelSize()`; emission of neck pieces before the `ClearMask` |
| `src/sim/Simulation.cppm:31-69` | `bool neckRule = true` |
| `src/engine` `PresentBlast` | the `Blast:` line gets `neck N nodes (s split), L loads, A arcs, V ext visits, W flow work, P piece(s) [load > cut], X ms` |
| `src/main.cpp` | `--neck-blast [W]`; `--tower-blast` says "by an island" or "by the load check"; `--castle-blast` logs the neck counters |

### Tests (GPU-free; written first, seen failing against a stub that releases nothing)

VortexCoreTests (`tests/core/StructureTests.cpp`, new `tests/core/NeckTests.cpp`):

- `Structure.NodePlaneCutWeakestMaterialAndAnchorBit`: random cells against brute force - every plane's cut and
  area, the chosen plane, the constriction rule, the halves' connectivity, `weak[k]` per class, the anchor bit; the
  shared Uniform graph. `Structure.FoliageCarriesNoLoad`. The graph comparison (`StructureTests.cpp:311-330`) covers
  the new fields, so `RefreshEqualsRebuild` and `EvictionNeverChangesAGraph` keep their meaning;
  `NodesCarryMassAndLinksTheirBond` is unchanged (iron and stone).
- `Neck.MaxFlowReleasesTheLargestOverloadedSet`: random networks of up to 16 nodes, all 2^16 sets enumerated: the
  released set is the largest maximiser of `mass(A) - cap(A -> out)` among strictly overloaded sets; load equal to
  capacity holds.
- `Neck.ReleasedSetIndependentOfArcOrder`: a test hook shuffles arc and queue order; same set, every time.
- `Neck.NetworkEqualsVoxelReference`: random volumes over 2 x 2 x 2 chunks with water: the same problem built per
  voxel (infinite capacity inside a node or half, per-face capacity across) and solved by a plain reference max-flow
  releases the same voxels.
- `Neck.TowerOnATwoByTwoNeckFalls`: the tower's FillBoxes at 0.125 m with the neck at each of the 8 heights mod 8,
  across an x cell face and across a z cell face, after a dud in the hollow: one piece holding all 8526 voxels above
  the neck and at most its 48 neck voxels, ring row 0 and the paving untouched, roof air; at the aligned offset
  exactly 8574 voxels, load 17 986 800, cut 3 200 000.
- `Neck.ThresholdIsExact` (2 x 11 falls, 2 x 12 stands; a block of exactly n x capFace stands, one MU more falls),
  `Neck.FullBaseStands`, `Neck.ParallelNecksAddUp`, `Neck.BondIsTheWeakerMaterial` (stone tower on a planks layer),
  `Neck.VoxelSizeInvariant` (0.1 / 0.125 / 0.15: 2 x 2 falls, full base stands).
- `Neck.StoneOverhang3mStands`, `Neck.OverhangRootThresholdIsExact` (24 faces fall, 25 stand),
  `Neck.SandOverhangSlumps` (2 m falls, 1.5 m stands), `Neck.WoodBeamBridgeStands` (also with one end cut: it stands,
  no bending, failure mode 3), `Neck.TreeWithOneVoxelTrunkStands`.
- `Neck.NaturalTerrainNeverFails` (the rock gate): built-in terrain at 0.125 m and 0.1 m, caves, water, Packed
  chunks: 300 duds at random surface, cliff, cave, overhang and tree points release nothing; 200 power-6 blasts
  release only pieces that touch the crater's ring (each logged with its ratio).
- `Neck.ChecksOnlyInsideTheIslandBox` (a thin neck high on a pillar, a dud at the base: nothing),
  `Neck.ExteriorLoadIsPartOfThePiece`, `Neck.OversizePieceStaysCounted`, `Blast.NeckNothingOutsideTheFloodBoxIsRead`
  (VortexSimTests; a chunk just outside loaded and unloaded: equal operations), `Neck.StaleGraphNeverReleasesAttachedVoxels` (the
  `InjectForTesting` hook, Structure.cppm:153-156), `Neck.ExactAtLargeLoads` (a 54 872-voxel gold block on 1067 /
  1068 iron faces at 0.1 m, about 1.07e9 MU; named `NoOverflowAtTheLimits` before the review) and
  `Neck.NetworkAtTheLimits` (review: 8192 nodes of the largest supply, arcs just under 2^45 MU, checked in 128-bit
  integers).
- `Neck.BudgetsCountWorkNotTime`: small budgets reached; every counter equal cold vs warm cache and 1 vs 3 workers;
  only `graphsBuilt` may differ.

VortexSimTests:

- `Blast.NeckReleaseIsAnOrdinaryDetach`: operations carve, islands, the neck `Detach`, `ClearMask`; a second
  simulation fed the operations has an equal `WorldHash` (`Blast.NeckReleaseTwoSimulationsEqualHashes` with the
  server config on A).
- `Blast.NeckReplayGivesIdenticalOps` (casts at fixed ticks), `Blast.NeckOnAServerCountsAnUnloadedChunk`,
  `Blast.NeckDefersOfflineAndTheLateSearchReleases`, `Rubble.TowerNeckReleasesStayUnderCaps`.
- `Blast.NeckRuleOffEqualsStage4`: with `neckRule = false` every arena gives today's operations.
- Unchanged and green: `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes`;
  `Structure.GraphFloatingEqualsFaceFlood` runs with `neckRule` off (it compares two island searches op for op). Every
  stage-1 and stage-4 arena asserts `neckPieces == 0` (they are pillars, ledges and slabs far inside the table's
  margins), or says in the test why not.

<!-- source lines 1258-1397 -->

### Stage 4b as built (2026-11-13)

Built as designed above unless this table says otherwise; reviewed by three readers ("Review of stage 4b" below:
several rows here are superseded there).

| Item | As built | Why / what later stages must know |
|---|---|---|
| Materials | `Material::strength[3]` (C, S, T, kg/m^2, `uint32_t`, default stone's), the table above in `RegisterDefaultWorldMaterials` (bedrock keeps the default: it is an anchor). `GraphMaterials` gained `anchor` (Indestructible and not ignored) and `strength`; foliage gets `densityQ` 0 there. | Stage 3 moves the integers into `MaterialPhysicsQ` and `buildHash` (R6). `MaterialFlag_Foliage` now has one reader (`MakeGraphMaterials`). |
| Graph fields | `GraphNode` reordered, still 72 bytes (`static_assert`): `cutFaces`, `cutPlane` (bits 0-2 plane, bit 7 anchor), `weak[3]`. Single-id cells cost nothing; a mixed 4^3 block collects its solid-id set in the loop that already sums its mass, so a **single-node mixed cell takes its weakest ids from the cell's id set** (no per-voxel pass); only a cell with several nodes and several ids reads ids per voxel. The plane cut is the bytewise-popcount sweep of the design plus `FloodCell` on both halves for constricted nodes. | **Measured** (`Structure.DISABLED_GraphBuildCost`, quiet, -O2, game terrain 0.125 m, 3 passes): Dense 29-31 us per chunk (stage 4: 25-28), Packed 22-24 us (20-23), Dense with the data in the CPU cache 22.7 us (16.6). A first draft that read ids per voxel for every mixed node cost 44-46 us; the id set fixed it. Memory unchanged (5.2 / 6.5 KB per Dense / Packed graph). |
| Phase 3b | `NeckCheck` in `internal/Structure.cpp`, called by `SearchOnce` after the floods when `limits.neck` is set and the search is complete; the `VoxelSource` moved before it (split nodes are re-labelled for their halves' masses, and re-checked against the graph: a stale node counts `relabelMismatch` and the search runs again). `GraphTable` gained `neck` (labels, grown with `state` in `Resolve`) and `ForEachBond` (face and bond per neighbour; goes on after an unloaded chunk when told to). | **Trap found by the tests:** `ForEachBond` takes its node by value; a reference into the flood queue dangled when the flood grew the queue (segfault in `Neck.ReleasedSetIndependentOfArcOrder`). |
| Rule details | (1) Every finite arc capacity is perturbed after summing (`c x 2^17 + 1` per arc pair direction), supplies `x 2^17`, infinite arcs `2^62`. (2) **Nothing is released when all supply reaches the sink**, even a weightless part with no support at all (the largest-maximiser set would hold it at value 0; such a part is the island search's business, e.g. a component it gave up on by budget). (3) A judged node with the anchor bit, a voxel on the flood box face layer or the window's bottom row is a sink with **both** halves (the design said the node; the anchor voxel may be in either half). (4) An exterior flood that meets a load label (impossible: a closed load holds its whole component) is taken as an anchor. | `Neck.MaxFlowReleasesTheLargestOverloadedSet` pins (2) against brute force (240 random networks of 2-16 nodes; all 2^n sets). |
| **Offline, a chunk outside the window** (superseded by the review: every unloaded chunk is support now, and a late search re-checks once it loaded) | An exterior flood (or a judged node) that meets a chunk **outside the world window** is support, counted in `neckBeyondWindow`; a chunk **of** the window that is still loading defers the search as designed (`complete = false`). Server unchanged: support plus `neckUnloaded` (an error). | Deviation, found by two stage-1 tests that failed with the design's rule (`Blast.IslandSearchRunsLaterOnceItsBoxHasLoaded`, `Rubble.TowerCutsStayUnderCaps`): the ground's exterior flood reaches the flood box face, and near the window's edge the flood box sticks out of the window, so **every** search near the edge waited for chunks that cannot load until the player moves - face islands included (stage 1 accepted that for small face components only). Offline only; the server's window holds every flood box. Taken as support, a tower near the window edge stands until a later cast (failure mode 4's direction: late, never wrong). |
| `sim::Blast` | `SimulationConfig::neckRule` (default true; only with the graph search); `GraphSearch` fills `NeckLimits` from the material tables and `World::VoxelSize()` and the four `kNeck*` budgets (`Blast.cppm`). Emission: carve, in-box islands, face islands, neck pieces (piece order), one `ClearMask`. `IslandSearchStats` gained the `neck*` counters, `neckBeyondWindow` and `neckCuts` (load, cut, faces, voxels, inside a cell); `BlastTimes::neckMs`; `Simulation::IslandSearchChecks::neckReleases` (the stage-1 arenas fail if the check releases anything there). The engine's `Blast:` line prints the network, the work and each released part. | No `WorldOpKind`, no `Detach` / `ClearMask` or wire change; `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` keeps its four operations. `relabelledNodes` now also counts released neck nodes. |
| Game | `--neck-blast [W]` as designed; the frame-30 line gives the neck's corner in metres; the pristine line counts released parts separately from islands / crumbs that already floated as built (99 at 0.125 m, 80 of them at the tower-base dud next to a generated tree, 93 at 0.15 m: crumbs that already floated, which the island search takes on any dud, as in stage 4; cause not traced further). `--tower-blast` says "by an island" or "by the load check: load L > cut C over F faces"; `--castle-blast` logs the check's counters. | |

**Tests** (33 new, 510 in all, 3 of them disabled measurements). VortexCoreTests (`tests/core/NeckTests.cpp`): the
design's list - `Neck.MaxFlowReleasesTheLargestOverloadedSet`, `ReleasedSetIndependentOfArcOrder` (150 networks x 6
shuffles; the tower with 4 shuffled networks), `LimitsFollowTheVoxelSize`, `TowerOnATwoByTwoNeckFalls` (8 floor layers
x neck across an x and a z cell face: everything above the neck plus at most its 48 voxels, the cut 4 faces = 3 200 000
MU; at the aligned floor exactly 8574 voxels and 17 986 800 MU), `NeckInsideACellIsSplit` (added: a neck that only a
split node can see), `ThresholdIsExact` (2 x 10 / 11 fall with the exact loads, 2 x 12 stands; a block of exactly n x
capFace stands, one MU less capacity falls), `FullBaseStands`, `ParallelNecksAddUp`, `BondIsTheWeakerMaterial`,
`VoxelSizeInvariant`, `StoneOverhang3mStands` (and the mushroom rock), `OverhangRootThresholdIsExact` (24 root faces
fall with 3072 voxels, 25 stand), `SandOverhangSlumps`, `WoodBeamBridgeStands` (both ends and one), `TreeWithOneVoxelTrunkStands`
(and with 200 kg/m^3 leaves it falls), `ChecksOnlyInsideTheIslandBox`, `ExteriorLoadIsPartOfThePiece`,
`OversizePieceStaysCounted`, `ExactAtLargeLoads` (named `NoOverflowAtTheLimits` until the review; a 38^3 gold block on 1067 / 1068 iron faces at 0.1 m:
1 067 352 200 MU against 1 067 000 000 falls, exact), `StaleGraphNeverReleasesAttachedVoxels`,
`BudgetsCountWorkNotTime` (each budget reached; every counter equal 1 worker cold, warm, 3 workers cold; only
`graphsBuilt` differs), `NetworkEqualsVoxelReference` (14 random volumes on bedrock with random face capacities: the
released voxels equal those of the same problem built per voxel face and solved by a plain Dinic - 14 of 14 release,
17 690 voxels, splits and loads present; the reference takes the nodes' weakest materials and planes from the graph, so
it checks the network, not the build), `NaturalTerrainNeverFails` (game terrain at 0.125 and 0.1 m, full height, Packed
chunks, 3 sites each: 420 duds at column edges - surfaces, cave ceilings and floors, overhangs - release nothing; 144
power-6 1.2 m blasts release nothing either; the design asked 300 + 200), `Structure.NodePlaneCutWeakestMaterialAndAnchorBit`
(400 random cells: 846 nodes, 190 constricted, against brute force), `Structure.FoliageCarriesNoLoad`. VortexSimTests:
`Blast.NeckReleaseIsAnOrdinaryDetach`, `NeckReleaseTwoSimulationsEqualHashes` (server on A, 3 / 1 workers),
`NeckReplayGivesIdenticalOps` (casts at ticks 3 and 7, 1 and 3 workers: equal operations and hashes; the first shot
leaves a 44-face neck standing, the second cuts it under its threshold), `NeckOnAServerCountsAnUnloadedChunk`,
`NeckDefersOfflineAndTheLateSearchReleases`, `NeckRuleOffEqualsStage4`, `Blast.NeckNothingOutsideTheFloodBoxIsRead`,
`Rubble.TowerNeckReleasesStayUnderCaps`. Order of work, plainly: the API and a stub that released nothing came first,
then the tests; against the stub 22 of the 25 core tests and all 7 sim tests then written failed (the three that passed
are "must stand" cases: `StoneOverhang3mStands`, `WoodBeamBridgeStands`, `LimitsFollowTheVoxelSize`;
`NaturalTerrainNeverFails` is a gate that a stub passes by construction). `Blast.NeckNothingOutsideTheFloodBoxIsRead` was
written after the implementation (it fails against the stub: it requires the release). Two existing tests failed
against the first implementation and led to the beyond-window rule (above).

**Measured** (-O2, the UHD 620 laptop). Quiet, `Blast.DISABLED_CostOfTheTowerAndCastleCases` (now a third simulation
per scene with the check on; the graph-vs-voxel hash comparison runs with it off), three fresh worlds per row:

| Case | Flat ground: check per cast (network) | Game terrain: check per cast (network) |
|---|---|---|
| Underground 1.5 m, 6 m deep (cold) | 0.54-0.60 ms (1000 nodes, 3176 arc pairs, 929 exterior visits, 10 725 flow work) | 1.57-1.85 ms (1115 nodes, 3658 arcs, 974 visits, 11 981 work), of a 2.5-2.8 ms search |
| Tower ring shots (cached site) | 0.14-0.32 ms (324-368 nodes, ~1000-1090 arcs, 405-433 visits, 3000-8200 work) | 0.28-0.72 ms (432-502 nodes, ~1300-1390 arcs, 487-607 visits, ~4000-4260 work) |
| Tower ring shot 1 (cold) | 0.24-0.27 ms | 1.08-1.22 ms: **it releases the tower** (on the terrain its floor partly overhangs lower ground; stage 4 freed it at shot 3) |
| Castle curtain / keep / corner tower (cold) | 0.06-0.21 ms (100-192 nodes) | 0.11-0.90 ms (105-273 nodes, 0-8 loads) |

Phase split (instrumented once, then removed; quiet terrain, 564 checks of `Neck.NaturalTerrainNeverFails`): median
0.17 ms, p90 0.39 ms, max 1.14 ms; the judged nodes take 0.03-0.07 ms, the **exterior floods most of the rest, and
when the site is cold they build the graphs of the chunks around the island box** (counted in `chunkGraphs`), the
arcs and the preflow 0.03-0.07 ms each. Game, headless, validation on (`--after-fill` where noted):

| Run | Load check per blast | Outcome |
|---|---|---|
| `--neck-blast` (W = 2, 0.125 m), after fill | 6 pristine duds 1.0-5.0 ms (7-45 graphs built per dud, cold sites); the neck shot 1.32 ms (0.42-0.49 without `--after-fill`) | released 8558 voxels (the cut is the cell face 4 rows up the neck at this floor height), load 17 945 200 MU (35.0 t) > cut 3 200 000 MU, 4 faces, 5.61x; 0 pieces from the duds; piece 0.22 m/s at +5 frames, store 5170 / 12288 |
| `--neck-blast 11` / `12` | 0.42 / 0.31 ms | 11: 8702 voxels, 18 319 600 > 17 600 000 MU (22 faces), 1.04x; 12: stands, roof solid |
| `--neck-blast --voxel-size 0.1` / `0.15` | 1.40 / 0.97-1.00 ms | 19 228 voxels, 41 490 400 > 4 000 000 MU, 10.37x / 5847 voxels, 12 509 800 > 2 666 664 MU, 4.69x |
| `--tower-blast --after-fill` | 0.92-3.80 ms per shot (3.80 the cold first shot) | **freed by shot 6 of 7 by the load check** (2 faces left: 18 033 600 > 1 600 000 MU, 11.27x; 8592 voxels; ring "cut" by its row test at the same shot) instead of shot 7 by an island. Judged right: what is left after shot 6 holds 35 t on 2 voxel faces |
| `--castle-blast --after-fill` | 0.35-1.69 ms | 0 pieces: every wall reaches the flood box face or is over 60 000 voxels (open question 2) |
| `--probe-blast` | 1.59 ms | unchanged: 1 island of 32 voxels, `0 floating island(s)` at the build |
| `--climb` | 2.79 ms of a 7.85 ms blast while streaming (69 graphs built in 4.21 ms) | releases nothing; stage 4's blast was 4.75 ms |
| Standard run (crate blast) | 1.12-3.84 ms (cold, streaming) | unchanged: `Beam cut: 13 voxels removed, 1 new object(s)`, HUD 149 quads, on the ground |

Screenshots (the watch tower seen from the plaza, `--neck-blast --camera 11,6.5,-2,-1.18,0.12`): at frame 47 the ring
is gone but the 2 x 2 neck at the north-west corner and the row-0 stub, the tower still upright (it has just been
released); at frame 100 it has dropped onto its row-0 ring and tips over. (The render trails the FillBox by several
frames: a frame-39 capture still shows the full base.)

**Against the targets.** The check costs **2-4x the design's estimate** (0.05-0.3 ms typical): quiet 0.06-0.72 ms per
cast at a cached site, 1.1-1.9 ms cold on game terrain, in game 0.3-1.7 ms cached and up to 5 ms at a cold site,
most of the cold cost being graph builds of the chunks around the island box that only the exterior floods read. So a
cast at an untouched site now costs about 1.5-2x stage 4's (`--climb`: 7.85 against 4.75 ms; the 3 ms offline frame
rule is broken more often), and a cached one 0.2-0.7 ms more. On a server the work is counted (`neckExteriorVisits`,
`neckArcs`, `neckFlowWork`, `neckSplitRelabels`, `neckVoxels`, and `chunkGraphs` at the cold price), so stage 3's
budget charges it; at 10 casts/s a 5v5 server spends about 2-19 ms of CPU per second on it (quiet numbers), inside the
cast phase. Session clients pay nothing new. Prebuilding graphs (the stage-4 owner question) would remove the cold
part here too. Not measured: the worst case at the budgets (est. 3-4 ms), the UHD 620 under a real fight.

**Not done / not confirmed:** the review by fresh readers (done: "Review of stage 4b" below); windowed play (the tower falling on its neck, its rubble)
is for the owner; bending / tipping (failure mode 3) and stage 4c (castle bricks) are not built; the stage-3 budget
does not exist yet, so the counters are only logged.

### Review of stage 4b (2026-11-13)

Three reviewers read the uncommitted stage 4b. Each finding was checked against the code; bugs got a failing test
first (seen failing against the reviewed code, then passing). What was changed:

| Finding | Verdict | Change |
|---|---|---|
| No cap on what one check releases: dozens of 60 000-voxel `Detach` ops possible; the design's worst case and the 16 kB burst did not hold | Real | `NeckLimits::maxReleasedVoxels` / `maxReleasedParts`, `sim::kNeckMaxReleasedVoxels` = 60 000, `kNeckMaxReleasedParts` = 64: parts in piece order while both fit, the rest stays (`neckDeferredParts`, logged). Exact and order-free (step 5 above). `Neck.ReleaseIsCappedPerCheck` failed first (three blocks released past a two-block cap), passes; a later check releases what stayed. |
| A released part can be a load alone; the crumb mask bound and "a neck piece holds an R node" were wrong | Real (docs, comment) | Bound corrected to `192 + 2 x (MinPieceVoxels + 7)` per axis (Blast.cpp, step 7, networking.md "Crumbs"); `Neck.HangingLoadIsReleasedAloneItsNodeStays` (224 sand voxels hanging under a ceiling in tension: released alone, cut 16 x 4 000 MU, the ceiling stays). The code was right; the test passes against it. |
| Offline, a chunk outside the window was support and nothing judged the structure again | Real | Every unloaded chunk the check meets offline is support for that cast (it can only release less) and `sim::Blast` queues a late search (`IslandSearchStats::neckDeferred`, `SearchOutcome::NeckWaits`) that waits for it - outside the window until the window moved over it - and runs search and check again. `Blast.NeckBeyondTheWindowIsCheckedAgainOnceLoaded` failed first. Cost: a blast whose flood box reaches past the window leaves one waiting entry (the game's window is 192 m wide; flood boxes reach past it only for casts over about 84 m away); `Blast.IslandSearchRunsLaterOnceItsBoxHasLoaded` now expects it. |
| The stale-graph guard did not compare what the check reads | Real | `SameAsGraph` re-derives mass, weakest materials, anchor bit and narrowest plane from the voxels it re-labels (one table lookup per voxel) for split and released nodes. `Neck.StaleGraphNeverReleasesAttachedVoxels` gained graphs with the same voxels but sand as the weakest material, and 10x the mass: both released the standing tower before, both are caught now. Kept nodes still rely on `World::ChunkRevision` (failure mode 8, notes/voxel.md). |
| Weakest material per node: leaves or snow beside a wall set its strength | Real (high) | Foliage is left out of a node's weakest material unless the node holds nothing else (`GraphMaterials::foliage`); snow's compression 10 000 -> 50 000 (packed snow; shear and tension unchanged). `Neck.StoneWallWithLeavesStands` and `Neck.StoneWallOnSnowStands` failed first (both 6 m walls released), pass. Failure mode 2 corrected. Rejected the reviewer's alternative of rating C / T from the top and bottom layers only: snow lying on the same layer as the bond would still rate it, and the inner plane of a split node needs other layers; exact per-face bonds stay rejected (cost). |
| The rock gate did not sample beaches, snow, deep caves, 0.15 m, repeated craters | Real (test) | `Neck.NaturalTerrainNeverFails` rewritten: 0.125 / 0.1 / 0.15 m; per size the three sites near the origin plus a sand beach, a snow field and a rock slope, chosen by `TerrainHeight` and checked against the generator's top voxel (the cover rule has a noise term: the first "snow" pick was stone); edges scanned 30 m deep with at least 8 points more than 6 m under the surface per site; three overlapping power-6 shots deepening one crater and two craters 2.5 m apart, each followed by a dud. Result: 18 sites, 648 duds (144 deep), 396 blasts, **0 parts released**. |
| Deferral of the check threw away proven face islands | Real | Same change as the beyond-window finding: the check never makes the search incomplete any more. `Blast.NeckWaitKeepsTheFaceIslands` (a floating bar across the island box face falls in the blast itself while the check waits for a loading chunk) failed first; `Blast.NeckDefersOfflineAndTheLateSearchReleases` updated (the tower stands for that cast, the late search releases it). |
| Cold-site cost spikes; worst case at the budgets never measured | Real (cost) | Measured (`Neck.DISABLED_CostAtTheBudgets`): far over the design's 3-4 ms (Budgets, open question 7). The suggested early anchor was built: an exterior flood that enters a chunk solid through (a pure function of its voxels, asked before the chunk is resolved: Uniform id, Dense occupancy, Packed RLE runs) ends there as ground (`neckSolidChunks`, `Neck.SolidChunkIsGround`). It ends 173 floods per tower shot on flat ground and 1-34 per castle shot in game, but **0 on generated terrain**: there the cold cost is the graphs of the chunks next to the island box, which the judged nodes' bonds need, so only prebuilding graphs (the stage-4 owner question) removes it. |
| The tower falls at ring shot 1 on game terrain, unexplained | Real (explained) | The cost scene now fires a dud before shot 1 and runs the check on the graph run's world: the tower (no foundation, on a slope) is overloaded before any shot - it rests on about 25 faces of turf, 22 932 000 MU against 10 000 000 (2.29x) - failure mode 6, open question 6. The game's playground on its foundation releases nothing (`--neck-blast`: 6 duds, 0 parts). |
| The voxel reference barely exercised splits and loads, compared released sets only | Real (test) | `Neck.NetworkEqualsVoxelReference` rewritten: hourglass columns with the narrow layer inside a cell, in touching pairs and across the chunk faces x / z = 0, +-32 and y = 96 / 128, structures sticking out of the island box; four capacity tables per trial (random per material, one uniform capacity, and the uniform capacity at which the heaviest released part holds exactly and one MU under it); every part compared voxel for voxel **and by load, cut (MU) and cut faces**. 14 trials, 118 split nodes, 31 loads, 56 solves, 231 parts compared. It failed first - on a bug of the reference (an exterior component split by an earlier anchored flood became a load there; the graph's rule makes it an anchor) - and found no fault in the network. |
| No exact shear or tension cut on a chunk face | Real (test) | `Neck.OverhangRootThresholdIsExactOnAChunkFace` (root at x = 31 / 32: 24 faces fall, 25 stand, cut 24 x 320 000 MU), `Neck.HangingBlockTensionThresholdOnAChunkFace` (neck across y = 95 / 96, tension set to load / 4 holds, one MU less falls), `Neck.TowerNeckAcrossTwoChunkFacesFalls` (neck at x = -1 / 0 and z = -1 / 0: 8574 voxels, 4 faces, 3 200 000 MU). All pass. |
| The rock gate did not assert that the check ran in full; "blasts release nothing" not pinned; counts below the design | Real (test) | Every dud and blast asserts `neckRan` and no spent budget or skip, every dud no unloaded chunk taken as support; blasts on hard ground must release nothing, on sand and snow nothing reaching 2 m past the crater; at least 300 duds (40 deep) and 200 blasts. |
| The cost table's "cached site" rows compared a fallen tower | Real | The cost scene runs the check on the graph run's world after each shot (`voxel::SearchIslandsOnGraph`, nothing applied) and marks the check run's rows after a release. Corrected numbers in "Measured after the review". |
| `neckMs` and `graphBuildMs` overlapped | Real | `neckMs` now excludes the graphs the check built, logged as `neckBuildMs` (`BlastTimes::neckBuildMs`, the `Blast:` line "+ X ms of graph builds"). |
| Test names and the large-load test's description | Real (docs) | `Blast.NeckNothingOutsideTheFloodBoxIsRead` named right; the gold test is `Neck.ExactAtLargeLoads` (54 872 voxels, about 1.07e9 MU); `Neck.NetworkAtTheLimits` added (8192 nodes of 512 x 65 535 MU, arcs at 2^45 - 1 MU, exact in 128-bit integers). |

**Measured after the review** (-O2, the UHD 620 laptop). Quiet, `Blast.DISABLED_CostOfTheTowerAndCastleCases`, the
check run on the graph run's own world after each shot (same world state as the search without it), three runs:

| Case | Graph search alone | The check (+ its graph builds) |
|---|---|---|
| Tower ring shots 2-7, flat ground (cached) | 0.06-0.27 ms | 0.13-0.48 ms (+0.00-0.05) |
| Tower ring shots 2-7, game terrain (cached) | 0.08-0.26 ms | 0.30-0.70 ms (+0.00-0.32) |
| Tower dud before shot 1, game terrain (cold) | 0.69-0.81 ms | 0.61-0.73 ms (+0.68-0.80); it would release the tower (above) |
| Castle shots (cold), flat / terrain | 0.41-0.58 / 0.46-1.43 ms | 0.04-0.16 (+0.00-0.09) / 0.10-0.21 ms (+0.00-0.74) |
| Underground 1.5 m (cold), flat / terrain | 0.31-0.54 / 0.98-1.60 ms | 0.41-0.85 (+0.09-0.12) / 0.93-1.46 ms (+1.00-1.76) |
| At the budgets (`Neck.DISABLED_CostAtTheBudgets`) | - | preflow alone 5.0-5.9 ms (8192 nodes, 65 535 arc pairs, 207 k scans); 529 pillars 2.0-2.4 ms (+0.8 cold); 55 936-voxel release 0.8-0.9 ms |

So at a cached site the check costs 2-5x the search it follows (0.13-0.73 ms against 0.06-0.27); a cold site pays
about as much again in graph builds. Game, headless, validation on: the crate blast of the standard run 0.30-0.40 ms
+ 0.5-0.7 ms of builds; `--tower-blast --after-fill` 0.84-1.86 ms + 0.06-2.23 ms per shot (freed by shot 6 of 7 by the
check, 2 faces, 11.27x, as before); `--neck-blast --after-fill` duds 0.33-2.27 ms + 0.31-2.71 ms, the neck shot 2.86 ms
(the release: preflow to the cut and 8558 voxels re-labelled; 0.54-1.05 ms in runs without `--after-fill`);
`--castle-blast --after-fill` 0.48-1.19 ms + 0.12-2.76 ms; `--climb` 1.60 ms + 1.42 ms of a 7.44 ms blast while
streaming. A 5v5 server at 10 casts/s: est. 3-15 ms of CPU per second on cached sites, about twice that at cold
ones, inside the cast phase per cast except at the budgets (open question 7).

Not done / not confirmed after the review: windowed play (the tower falling, its rubble, walls on snow) is for the
owner; the flow budget (open question 7) and stage 3's charge; graphs are still built on demand (prebuilding is the
stage-4 owner question and the only fix found for the cold cost); kept judged nodes are not re-checked against stale
graphs; bending / tipping (failure mode 3) and stage 4c are not built.
