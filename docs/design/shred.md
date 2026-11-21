# Destruction that finishes the job ("SHRED")

[[../../../00 Docs Map|Docs map]] · [[../ROADMAP|roadmap]] · [[../ARCHITECTURE|architecture]] · [[collapse|collapse (superseded parts)]] · [[networking|networking]] · [[fx-budget|fx budget]] · [[lod|LOD]] · [[../notes/voxel|voxel notes]] · [[../notes/scene-engine|scene/engine notes]]

| | |
|---|---|
| Status | Partly built. Built and reviewed: the face flood and the rubble budget ([shred-0] .. [shred-1e]), the cell-graph island search [shred-4] and the load check [shred-4b]. Designed, nothing built: the structural integrity plan [si-1] .. [si-3]. Not started: [shred-2], [shred-3], [shred-4c], [shred-7]. Superseded by the SI plan: [shred-5], [shred-6]. Detail in "Stages". |
| Decided | - Owner, 2026-11-12: build stages 1-1e now, before networking stage 3 ("Yes, build 1-1e"), with the recommended answers to "Open questions for the owner" 1, 2, 3 and 7 (a 3-chunk server window margin; pieces over 60 000 voxels counted, not split, in M1; rubble lives 15 / 20 / 30 s, caps of 48 bodies and 600 store slots).<br>- Owner, 2026-11-12: question 4, yes: the cell graph [shred-4] before networking stage 3.<br>- Owner, 2026-11-12: thin necks must break under the weight above them, now ("Yes, weak necks next"); rock stays strong enough that natural cliffs and overhangs do not fall by themselves; whole castles may collapse (Part 4b).<br>- Us, 2026-11-13: [shred-4b] built with the recommended answers to Part 4b's questions 1, 3, 4, 5 and 6 (stone C 100 000 kg/m^2, tuned by play; a shot at the roof does not re-check a neck far below; foliage weightless; soft ground slumps; weak player-built structures fall at the first shot near them). No owner answer to them is recorded. Question 2 (castle bricks) is open.<br>- Us, 2026-11-13, in [net-3]: Part 4b question 7: `sim::kNeckFlowWorkBudget` lowered to 65 536 and `neckFlowWork` charged to the cast work budget.<br>- Owner, 2026-11-17: "Implement a Structural Integrity System to check if object connections can break under their own weight and under inertial contact." Us, 2026-11-17: the SI plan - one rule (Part 4b's check) for terrain and objects, no wire change; `StructureRegistry`, the overhang rule, `StructuralSolver` and `CollapseBudget` dropped.<br>- Us, 2026-11-17 (project review, section 4; the owner did not object): [si-2] comes after M2a and the fight measurement; [si-1] and [si-3] after W1. |
| Next | [si-2] impacts, in ROADMAP "Next" after M2a (WP7), the fight measurement (WP8) and mechanisms stage 1. It starts with the review's WP9 groundwork: `NeckCheck` split into its phases and a chunk-source interface under the graph search. [si-1] and [si-3] are in ROADMAP "Later", after W1. |
| Open | "Open questions for the owner": 4 (prebuilding chunk graphs), 8 (crush damage with SI-2 or in M2), 11 (Part 4b question 2: castle bricks, [shred-4c]); the SI plan's questions 1-6 ("Open questions for the owner (SI plan ...)"). Questions 5, 6, 9 and 10 are superseded by the SI plan. |
| History | [stage-0-1e.md](../history/shred/stage-0-1e.md): Part 1's analysis, the stage-1 plan, costs and tests, stages 0-1e as built, their review.<br>[stage-4.md](../history/shred/stage-4.md): stage 4 as built, its review, the load-flow note.<br>[stage-4b.md](../history/shred/stage-4b.md): the judgement of the three designs, the code changes and test plan, stage 4b as built, its review ("Stage 4b review" in code comments).<br>[owner-questions.md](../history/shred/owner-questions.md): Part 4b's questions as put to the owner.<br>[part-5-impacts-2026-11-12.md](../history/shred/part-5-impacts-2026-11-12.md): Part 5, impacts as first designed.<br>[si-plan-review-2026-11-17.md](../history/shred/si-plan-review-2026-11-17.md): the critic's review of the SI plan.<br>[before-rewrite-2026-11-18.md](../history/shred/before-rewrite-2026-11-18.md): the old header and every section this rewrite changed. |
| Marks | (m) measured, (c) computed, (e) estimate (also written **(e)** or "est."). Times are -O2 on the UHD 620 laptop unless a row says otherwise; "quiet" = no streaming. |

Related: [collapse.md](collapse.md) (partly superseded: "What happens to collapse.md" below); [networking.md](networking.md)
"Time and ticks" (the cast work budget, R5), "Entity replication" and "Interaction with shred.md"; [fx-budget.md](fx-budget.md)
(the client caps); [lod.md](lod.md) (its debris threshold is presentation only, Part 3); [notes/voxel.md](../notes/voxel.md)
(the face rule, chunk revisions); [ARCHITECTURE.md](../ARCHITECTURE.md) 7.2.

## Decision in brief

1. **The "second shot" bug is fixed by a bounded outward flood** [shred-1]. A piece freed by a blast stayed in the air
   because the island search treats every component that reaches the face of its search box as attached
   (`voxel::ExtractFloatingIslands`) and nothing searched again. Components that touch the face are proven by a flood
   inside a fixed **flood box** (blast centre +-96 voxels, `voxel::kFloodHalfExtent`): if it closes inside the box, the
   component is an island and becomes an ordinary `Detach`. No new operation, no wire change, integer-exact. A session
   server keeps every flood box loaded; offline, a flood that meets an unloaded chunk **defers the whole search**. It
   frees towers, walls and ledges up to 60 000 voxels (117 m^3 at 0.125 m) inside the flood box; castle-scale release
   is not built ([shred-4c]).
2. **More pieces fall, so rubble has a budget** [shred-1e]: every piece of client rubble has a lifetime by size, and
   rubble bodies and chunk-store slots have global caps.
3. **Measure before optimising** [shred-0]: a `Blast:` log line per blast and the island box gathered by chunk. Its
   measurement (search + flood over 3 ms per cast) moved the cell graph before networking stage 3.
4. **The two physics defects after a blast are fixed** [shred-1d]: a new piece waits frozen with its velocity while
   the collider under it is stale, and a body parked over ground edited out of sight wakes on that chunk's first
   collider build.
5. **Shredding improves on the client first** (stages 2-3, not started): material-dependent chips, dust and splinters
   and local fracture of client rubble (`netId 0`) need no determinism. A material-dependent carve on the wire
   (`ShredSphere = 7`) comes later (stage 7).
6. **Structural updates run on terrain first, with one rule.** The island search runs on a cell graph cached per
   terrain chunk [shred-4]. The structural rule is Part 4b's integer max-flow load check on that graph, run only after a
   cast [shred-4b] (owner decision 2026-11-12: thin necks must break under the weight above them), so pristine
   buildings never fall on spawn. collapse.md's overhang rule is not used (the SI plan dropped it); a stress / bending
   model stays optional (stage 7). In a session a released piece of the replicated size that gets a slot
   (`sim::TakePieceSlot`) is a server body; the rest is `netId 0` client rubble.
7. **Impact destruction is server-only and quantised once** [si-2]: a Jolt contact becomes an integer `ImpactQ` the
   moment it is read; everything after that is integer, totally ordered and replays. Only bodies of at least the
   replicated volume strike, offline as on a server. Crush damage to players is not a contact (SI plan, "Dropped or
   simplified").

## What happens to collapse.md

| collapse.md | Status under this design |
|---|---|
| Stage 1: anchored remainder of a static object stays static (collapse.md "Stages") | **Kept unchanged**, still ROADMAP M3. Not in the SI plan: nothing in the game or a session is static (the game, `sim::TemplateObject` and the server's `SpawnObjectAt` make only dynamic objects; only the offline editor's `spawnMotion` can). |
| Stage 2: graph core, headless | **Replaced** by [shred-4]: a node graph per terrain chunk ("The cell graph as built"). |
| Stage 3: graph in `Scene::Damage` | **Moved after the terrain host**; now [si-1]: Part 4b's check on an object's tile graphs. |
| Stage 4: no whole-object work per hit | Kept unchanged; not needed for SI (server models are not baked). |
| Stage 5: terrain structures | **Moved first** ([shred-4], [shred-4b]). The game's buildings are terrain `FillBox` boxes (`sim::PlaygroundTemplate`), castles must exceed 12.8 m (ROADMAP M3), and terrain ops replicate. |
| "What is wrong today": `minPieceVoxels = 6`, no `Lifetime`, `Engine::Explode` does islands | **Stale.** Pieces under `scene::MinPieceVoxels` (8 at 0.125 m) are particles; every rubble piece has a lifetime by size and caps [shred-1e]; islands run in `sim::Blast`. Its "tower stays in the air" line was the bug of Part 1. |
| Cell format: "more than 8 components merge into node 7" | **Replaced** [shred-4]: every component of a cell is its own node in a per-chunk node list, so the graph equals voxel connectivity everywhere, over-full cells included (`Structure.GraphEqualsLabelComponentsInOverfullCells`). |
| Rejected "CSR node/edge graph with bond areas, masses and a load check (B)" | **Its vertical part is [shred-4b]** (Part 4b): masses and bonds on the stage-4 graph, a max-flow check that runs only after a cast, so pristine buildings never fall on spawn. A stress / bending model stays optional (stage 7). |

## Part 1 - The "second shot" bug (verified)

A shot cut the base of a pillar, tower or wall; the part above hung in the air; one more shot nearby made it fall. The
island box of a blast (`floor(c / 16) +- (r / 16 + sim::kIslandSearchMargin)` voxels: 33 voxels = 4.1 m for a 1.2 m
blast at 0.125 m) counts every component that reaches its face as attached, and nothing searched again. Fixed by stage
1's face flood [shred-1], below: the watch tower falls on the shot that cuts its ring (since [shred-4b] one shot
earlier, by the load check). The analysis, the numbers and the other causes checked (skipped carves, the deferred-search
list, oversize islands, stale colliders, full stores) are in [history/shred/stage-0-1e.md](../history/shred/stage-0-1e.md),
"Cause (confirmed in code)" and "Other causes checked".

### Fix: prove face-touching components by an outward flood (stage 1)

Where: the island search of `sim::Blast`, with GPU-free functions in `vortex.voxel` (`voxel/Connectivity.cppm`):

- `ExtractFloatingIslands` fills an optional `IslandBoxScan`: the box labels, `faceComponents`
  (`FaceComponent{label, firstVoxel, inBoxVoxels}`, the candidates), `faceOversize`, `oversizeIslands`, `boxVoxels` and
  the box actually labelled (`boxMin`, `boxMax`).
- `ProveFaceComponents(world, materials, candidates, boxMin, boxMax, boxLabels, ignoreFlags, FloodLimits)` floods
  them. `FloodLimits{floodMin, floodMax, maxVoxels, maxVisitsPerSearch, deferOnUnloaded}`; `FloodResult{islands,
  complete, unloadedChunk, candidates, proven, merged, attachedByLimit / Known / Budget / Unloaded, chunksDecoded,
  visits}`.
- The flood box is `floor(c / 16) +- voxel::kFloodHalfExtent` (96 voxels; 192 per axis = the `ExtractFloatingIslands`
  region limit; 24 m at 0.125 m, 19.2 m at 0.1 m), a pure function of the integer sphere, **not** rescaled with the
  voxel size (like the margin). `sim::IslandBox` and `sim::FloodBox` give both boxes. `maxVoxels` is
  `sim::kMaxIslandVoxels` (60 000), `maxVisitsPerSearch` is `sim::kFloodVisitBudget` (4 x `kMaxIslandVoxels`).

Since [shred-4] this voxel flood is the reference search (`SimulationConfig::islandSearch = IslandSearchKind::Voxel`).
The default search runs on the cell graph ("The cell graph as built") with the same boxes and rules and gives the same
operations, except where a work budget runs out or a flood box chunk is not loaded.

Rules:

1. **Candidates**: components that touch the island box face **and** whose in-box voxel count is at most
   `kMaxIslandVoxels`. Anything larger can never be detached anyway, so the ground under a surface hit (33 layers of
   66 x 66 voxels = 144k in the box at 0.125 m) is never flooded. **Candidates are processed by in-box count
   ascending, ties by label**, so small real islands are proven before large floods spend the budget. This order is
   part of the contract.
2. **Flood**: FIFO breadth-first from the candidate's first voxel in box scan order, through world voxels,
   6-connected, neighbours in the order `LabelComponents` uses (-x +x -y +y -z +z), solid = id != 0 and not
   `Liquid` (the ignore mask `sim::Blast` passes), **never leaving the flood box**.
3. **Reads** (never a whole-chunk copy: `World::ReadChunk` copies 36 936 B for a Dense chunk, fills one for a
   Uniform chunk and RLE-decodes a Packed one): a table over the flood box's chunks (at most 7^3) resolves each chunk
   once (`World::StateOf`, `World::GetChunk`); Dense voxels are read through the pointer, Uniform through the id. Packed
   chunks are decoded into at most 16 copies (16 x 37 KB = 0.6 MB), FIFO, all dropped at the end of the search; an
   evicted chunk that is needed again is decoded again and counted (`chunksDecoded`: every decode). Packing is never a
   content change.
4. **State**: a 2-bit array over the flood box (192^3 x 2 bits = 1.7 MiB), a `thread_local` allocated once per
   thread (the first flood on a thread pays about 1 ms for it): unvisited, visiting, island, attached. Only voxels
   touched by a search are cleared (from the visit lists), never a full memset.
5. **Gives up as attached** (and stays in the world) when any of these holds:
   - the visited count exceeds `kMaxIslandVoxels` (`attachedByLimit`);
   - the flood **visits a voxel on the flood box face** (whatever lies outside), or a neighbour lies below the bottom
     row of a fixed-height window (`attachedByLimit`). It never reads outside the flood box, and the flood box is a
     function of the integer sphere only, so this never depends on what is loaded
     (`Blast.FaceFloodIndependentOfLoadingOutsideTheFloodBox`); a piece that only touches the face from inside stays,
     as at the island box face;
   - the flood reaches a voxel marked attached, inside the box (a non-candidate, or a candidate given up on) or
     outside it (a voxel visited by a flood that gave up by limit; joining it means the same over-limit component)
     (`attachedByKnown`). This keeps forests and castles cheaper: the first trunk or wall flood that gives up marks
     its voxels; later floods that touch them stop at once;
   - the per-search work budget `maxVisitsPerSearch` is spent (`attachedByBudget`; counted in voxel visits, never in
     time). Every later candidate is attached by budget as well.

   A **visit** is a voxel **enqueued** (marked) by a flood, the seed included: a flood gives up by limit when it has
   marked `maxVoxels + 1`; the budget (`sim::kFloodVisitBudget` = 240 000) is spent when the call has marked more than
   `maxVisitsPerSearch` (three floods that reach the limit spend 180 003 visits, a fourth runs out at visit 240 001:
   `Blast.SmallIslandProvenBeforeTheBudgetIsSpent`). Inside the island box voxels are judged by the island box
   labels: label 0 = air or liquid; a face component that is not a candidate (over the limit), and a candidate given
   up on, are attached (`attachedByKnown`).
6. **A chunk inside the flood box that is not loaded** (outside the window, `Unloaded` or `Loading`):
   - **Server** (`SimulationConfig::sessionServer`, set by `ServerConfig`; `FloodLimits::deferOnUnloaded` false):
     cannot happen by construction: the server window is `playBounds` plus `sim::kServerWindowMarginChunks` = 3
     chunks on all six sides (a `static_assert` against `kFloodHalfExtent`), loaded at start and never moved, so a cast
     inside `playBounds` has its whole flood box loaded. It is counted in `attachedByUnloaded`, the candidate is
     treated as attached so nothing diverges, and it is an error (`Log::Error`) **asserted zero in tests**
     (`Blast.ServerFloodBoxAlwaysLoaded`, `Blast.FaceFloodOnAServerCountsAnUnloadedChunk`). An island box that is not
     loaded is the same kind of error on a server: `islandBoxUnloaded`, nothing searched, never deferred
     (`Blast.ServerNeverDefersAnIslandSearch`).
   - **Offline**: the whole search is refused (`complete == false`): no `Detach` from the face flood, `islandsSearched`
     false with `search.floodDeferred`, `World::RequestLoad` on the whole flood box. The in-box islands of the first
     search are still emitted at once (they do not depend on the flood). The deferred search waits for its island box
     and for the chunk the flood met (`waitsForChunk`); `RetryIslandSearches` repeats island search and flood once they
     are loaded. It is given up (`deferredGivenUp`, a warning) only when its flood box shares no chunk column with the
     window; a full list (`kMaxDeferredIslandSearches` = 16) drops the search whose island box is farthest from the
     window's middle (`deferredDropped`, a warning). A flood never gives up for good because the world was still
     streaming.
7. **Above the build ceiling** nothing exists (`EditReport::outOfBounds`, `Scene::DestroyAbove`), so the flood treats
   those rows as air, never as "not loaded". This also fixes the never-falls case of stage 1c at the ceiling. The
   island box does the same (`ExtractFloatingIslands` does not refuse a box that reaches above the ceiling). Rows
   below the window's bottom row are the world's ground for both: the island box ends at the bottom row
   (`IslandBoxScan::boxMin`), so a component touching that row is attached by the face rule, and a box entirely below
   the window is searched and empty.
8. **Closed flood** = an island. Candidates whose first voxel was already visited by an earlier closed flood are
   skipped (two face components can be one island joined outside the box).
9. **Emission order** (part of the contract): carve; in-box islands (scan order); proven islands in
   candidate order (rule 1); one `ClearMask` for all crumbs, in-box and proven. Proven islands of at least
   `MinPieceVoxels(vs)` are a `Detach`, smaller ones are crumbs.

Why not simply a bigger island box: the 192-voxel region limit and the label array are cubic (a 192^3 box is 7 MB of
ids and 28 MB of labels, e); the flood costs in proportion to the piece.

**Scope, plainly:** stage 1 frees towers, walls, ledges and floors whose whole remaining piece is at most 60 000
voxels (117 m^3 at 0.125 m, 60 m^3 at 0.1 m) **and** lies inside the flood box (+-12 m at 0.125 m). In a castle,
every wall or floor that leaves the island box usually connects to more than 60k voxels of castle and stays up
(`attachedByLimit`), so castle-scale collapse is not stage 1's (it is [shred-4c]).

### Network and determinism rules for stage 1

- No new `WorldOpKind`, no change of `Detach` (`sim::WorldOpKind`): clients still apply what the server
  sends, so `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` is unaffected.
- The search **result** changes for the same input: which voxels a blast detaches is a server decision.
- Integer only: voxel coordinates, counts, the flood box and a fixed visit and candidate order. The budget counts
  visits, so the decision does not depend on thread count, machine speed or what streamed in; with the work-budget
  cast queue of R5 the tick a cast runs on does not either.
- Masks grow: a proven piece can span up to 192 voxels per axis, raw mask 864 KiB at the limit, RLE on the wire.
  The tower: about 26 x 64 x 26 voxels = 5.4 KiB raw, est. 1-2 KB RLE; inside the event burst of 16 kB
  (networking.md "Time and ticks"). A piece whose RLE mask exceeds the burst is still sent (correctness first) and logged; the
  structure stages bound this properly.
- The crumb mask box can extend past the island box by less than `MinPieceVoxels` per side; the bound in the
  comment in `SearchIslands` (`sim/internal/Blast.cpp`) and in networking.md "Crumbs"
  ([history](../history/networking/stage-2.md)) changed from `(2(r/16 + 24))^3` to
  `(2(r/16 + 24) + 2 * MinPieceVoxels)^3` bits; with the load check it is `192 + 2 x (MinPieceVoxels + 7)` voxels per
  axis (Part 4b, step 7).

## Part 2 - Rules every later stage follows

- **R1 Server decides, clients apply.** Island proofs, structural releases and impacts run in `Vortex::Sim` on the
  server or offline; the output is `WorldOp`s in `opSeq` order. Clients never re-run a solver or an island search
  (networking.md, rejected alternatives). How a client cuts its **own** rubble (`netId 0`) is presentation and free.
- **R2 Integer operations only.** New op kinds take values 7 and up, integer parameters, and decide per voxel from
  `(voxel, old id, op integers, integer material table)`. Noise in an op comes from `vortex::Hash3`
  (`core/Random.cppm`), pinned by a golden-value test.
- **R3 Quantise floats once, at emission.** Jolt values (contact point, speed, masses) become integers on the server
  the moment they are read; Jolt is not cross-platform deterministic here (it is built without `CROSS_PLATFORM_DETERMINISTIC`).
- **R4 Fixed visit order, total orders only.** Graph cells in index order, neighbours in a fixed order, FIFO buckets,
  ties by index; candidates by (in-box count, label); impacts sorted by their **full integer key after
  quantising** ([si-2]), never by arrival or by Jolt `BodyID` (process-local).
- **R5 Budgets count work, never wall-clock**, on the server: visits, voxels, ops, bodies, bytes per tick. This
  includes the cast queue: networking.md's "queued casts within a CPU budget (est. 6 ms)" became **a work budget**
  (island-box voxels scanned + flood visits + graph node visits + ops + event bytes per tick), calibrated once from
  stage 0's optimised measurement to about 6 ms on the reference machine (e); wall-clock is only logged. Then the
  tick a cast runs on depends only on the queue, and a replayed server makes the same decisions. Replay tests feed
  casts at fixed ticks. Only client presentation uses millisecond budgets (fx-budget `FxDirector`).
  *Built [net-3] (networking.md "Time and ticks"): `sim::CastWorkOf` charges node visits, `chunkGraphs` at the cold price,
  the load check's formula above and the operations' boxes (never `graphsBuilt`, `chunksDecoded` or the object half);
  `kCastWorkPerTick` = 60 000 units, calibrated to 1.09 ms per 10 000 units on the reference machine; the event byte
  budget is a credit of its own. `CastBudget.ScheduleFollowsCountsNotTimeOrCache` replays a server with 1 and 3
  workers and an emptied graph cache: same ticks, same operations.*
- **R6 One integer material table** (`MaterialPhysicsQ`, stage 3) hashed into `Hello::buildHash`.
  This also makes explicit what `CarvableIds` already relies on (float hardness read on both sides).
  *Built [net-3]: the inputs are hashed instead: `voxel::MaterialTableHash` (raw bits of `hardness` and
  `density`, `flags`, `strength[3]` per world-palette id) is part of `buildHash`, so a strength-table drift refuses
  the session. The integer `MaterialPhysicsQ` table itself is not built.*
- **R7 Over budget means later or cosmetic, never lost.** A release over the tick budget waits, static and solid; a
  piece over the body cap becomes `netId 0` rubble or crumbs (networking.md "Decision in brief": "never dropped silently"); rubble
  over its cap becomes particles. Impacts are the one exception: they are derived and may be dropped after 10 ticks
  (`kImpactMaxAgeTicks`, [si-2]), with a counter.

## Part 3 - Shredding (how material breaks)

Today every material breaks into the same 4 + 2 debris and 1 + 1 dust per burst, only the colour differs
(`fx::SpawnVoxelBreak`); hardness is only a carve threshold; one island is one body (`sim::Blast`);
`Scene::Damage` ignores hardness (notes/scene-engine.md) so iron cuts like dirt.

- **Integer material table** (`vortex.voxel`, `Material.cppm`, Core): `MaterialPhysicsQ{hardnessQ u16, densityQ,
  fractureCell, grainAxis, roughness, crumble, carvable}` (16 B), built once from `Material`, hashed (R6).
  `hardnessQ = min(hardness * 16, 65535)` (saturated: bedrock's 1.0e9 does not fit), and
  `carvable = !(flags & Indestructible)` (water and lava have hardness 0 but are `Indestructible`). Test `MaterialQ.SaturatesAndKeepsIndestructible`. Bond strengths (`Material::strength`,
  integer kg/m^2 for compression, shear, tension) were built with stage 4b (Part 4b) and join this table.
- **Crumble profiles by material** (client, `fx`): powder (sand, dirt, snow: little debris, more dust), chips
  (stone, brick), splinters (wood, planks: stretched particles, no dust), shards (glass, crystal), chunks (iron). They
  replace the fixed counts in `SpawnVoxelBreak` and stay inside fx-budget caps: 1500 spawns per frame, 8192 alive,
  particles GPU <= 2.5 ms (fx-budget.md "Targets", "`FxDirector` and its caps"). Dust is the fill problem on the
  UHD 620 (fx-budget.md "Where one blast costs time today"); hard materials get **less** dust than today.
- **Fracture-surface dust**: for each `Detach` / released piece, voxels of the piece that faced the remaining world
  get dust bursts (at most 64 per op, 200 per frame), so a falling tower sheds dust along the cut.
- **Local fracture of client rubble** (`voxel:fracture`, Core, pure function): seeds on a `fractureCell^3` grid
  jittered by `Hash3`, nearest seed by integer squared distance (wood: grain axis weighted 1/4 -> splinters), then
  `LabelComponents` per region; pieces under `MinPieceVoxels` become crumbs; over `maxPieces` the smallest merge.
  `PresentBlast` fractures the `BuildDetachedPiece` volume with LOD (6 pieces near, 2 at 15-60 px, dust only far,
  fx-budget.md "Effect LOD and load factor"). Pieces count against stage 1e's caps; the excess becomes particles. No wire change.
- **Distance LOD is presentation only, in one place.** lod.md stage 1 scales the piece threshold in `Scene::Damage`
  with distance (lod.md "Simulation by distance"; its test "Damage at 60 m creates no entity below 197 voxels", lod.md "Stages"). The
  piece / crumb rule that decides `Detach` against `ClearMask` (and, on a server, which object pieces get entities)
  must come from the session voxel size only, never from a scene's config (`scene::SceneConfig`, `sim::Blast`), and
  a server has no viewer. So: `sim::Blast` and `Scene::Damage` keep `MinPieceVoxels(vs)` / `replicatedPieceVoxels`;
  the distance threshold moves into `PresentBlast` / `PresentOps` and decides only whether a `netId 0` piece becomes
  a rubble body, fractured pieces or particles. lod.md stage 1 is changed to match in stage 3's commit; the
  particle / body savings are counted in fx-budget.md only, not again in lod.md or here.
- **Material-dependent carve on the wire** (`WorldOpKind::ShredSphere = 7`, after networking stage 3): sphere,
  power, `falloff u8`, `seed u16` (about 20 B); a voxel goes when

  ```
  carvable[id] && score >= hardnessQ[id],
  score = power * (16 R^2 - falloff d^2) / (16 R^2) + ((Hash3(v, seed) & 255) * roughness[id] >> 8)
  ```

  int64 throughout; `>=` like `CarvableIds` (power equal to `hardness * 16` carves, `CarvableIds`). Soft
  ground loses the whole sphere, stone a smaller jagged crater, iron survives; water, lava and bedrock never go.
  Golden tests: `ShredSphere.LiquidsAndBedrockSurviveAnyPower`, `ShredSphere.FalloffAndRoughnessZeroEqualsCarve`
  (voxel for voxel against `CarveSphereQ`), `ShredNoise.GoldenValues`. `Carve = 1` stays for tools and the editor.
  The same integer rule replaces the float sphere in `Scene::Damage`.
- **Server-side fracture** (with networking stage 6): the server fractures released pieces and emits one `Detach`
  per piece. A `DetachFractured = 8` op (mask + seed + params, clients recompute the partition) is an optimisation
  only after the golden test of `Fracture` exists.

## Part 4 - Structural updates after destruction

### Host and scope

- **Terrain first.** collapse.md's 8^3-voxel cells (`ChunkOccupancy::cellMask` granularity) are the unit, cached per
  chunk and built lazily on the first search that reaches them ("The cell graph as built"). Liquids are left out
  (`MaterialFlag_Liquid`, as in the island search). A graph is invalidated by `World::ChunkRevision`, which every edit
  through `World::EditRegion` bumps (game, editor, `FillBox`, replicated ops), and which loads and unloads change.
- **Connectivity on the graph replaces the voxel search** [shred-4], inside the same island box and flood box as stage
  1 (no new server margin). Over-full cells are exact (every component is a node), so the results equal stage 1 except
  where a work budget runs out, and in offline deferral or the attach reason when a flood box chunk is not loaded. It is
  a server decision; clients only apply.
- **The one structural rule is Part 4b's load check** on the same graph, run only after a cast. The overhang rule inside
  registered structures (`StructureRegistry`, collapse.md's 0-1-2 bucketed search with per-material limits) and its
  spawn guard were designed here for stage 5 and dropped by the SI plan ("Dropped or simplified, with the reason").
  Outside a cast nothing is judged, so cliffs never fail by themselves.
- Objects get the same check on their own tile graphs in [si-1].

### The cell graph as built

- `vortex.voxel:structure` (`voxel/Structure.cppm`, GPU-free). A **node** (`GraphNode`) is one 6-connected component of
  solid, not ignored voxels inside one 8^3 cell: 6 face masks of 8 x 8 bits, `voxels`, `mass` (the sum of
  `GraphMaterials::densityQ`, kg/m^3; kg = mass x vs^3; foliage 0), `firstVoxel`, its links, cell-local bounds, and the
  load check's `cutFaces`, `cutPlane` (narrowest plane, anchor bit) and `weak[3]` (Part 4b, "Graph additions").
  **`GraphNode` must stay 72 bytes** (`static_assert`).
- A `ChunkGraph` holds every node of a chunk's 64 cells, ordered by cell, then by first voxel in scan order, and its
  links inside the chunk (`GraphLink`: node, face, `bond` = voxel faces shared, 1..64), ordered by face, then node.
  Links across a chunk face are computed when needed (`BondVoxels(a, b, face)`). Every component is its own node, so
  the graph equals voxel 6-connectivity everywhere, over-full cells (more than 8 components) included; they are counted
  (`overfullCells`). A graph is a pure function of the chunk's ids and the material tables.
- `voxel::TerrainGraphCache`, owned by `sim::Simulation` (`TerrainGraphs()`), keeps one graph per chunk with the
  `World::ChunkRevision` it was built from and rebuilds a chunk on the first lookup after its revision changed. Dense
  and Packed chunks own their graph; every Uniform chunk of one id shares one (an empty or uniform-liquid chunk shares
  the empty one). At most 4096 entries: when a pass starts over the cap, entries whose chunk changed or unloaded go
  first, then the least recently used. Cleared when the World or the material tables change. Which graphs are cached
  never changes a result (`Structure.RefreshEqualsRebuild`, `Structure.EvictionNeverChangesAGraph`).
- **Every chunk edit must go through `World::EditRegion`** (or bump the revision), or searches go stale. A stale island
  or released node (voxel count, face masks, bounds; for the load check also mass, weakest materials, anchor bit and
  plane) is caught when it is re-labelled: `relabelMismatch` is counted, nothing is written, the cache is dropped and
  the search runs again (`staleGraphRetries`); if it still differs, no island is returned. A stale attached neighbour
  is not seen: the revision rule is the only guard there.
- `voxel::SearchIslandsOnGraph`: seeds are the nodes whose bounds meet the island box (its chunks must be loaded, as
  before); box components are joined over links between seeds; a box component over `kMaxIslandVoxels` is attached
  (`faceOversize`; `oversizeIslands` when it lies inside the box interior); one inside the interior is an island
  without a flood. The others are flooded smallest first over nodes (faces -x +x -y +y -z +z, links in their order) and
  end at a node on the flood box face layer or the window's bottom row (`attachedByFace`), past `kMaxIslandVoxels`
  voxels (`attachedByLimit`), at a node an earlier flood gave up on (`attachedByKnown`), on the budget
  (`attachedByBudget`) or at an unloaded chunk (rule 6). Only island nodes are re-labelled per voxel (`NodeVoxels`);
  islands come out in the voxel search's order.
- Budget: `sim::kGraphNodeVisitBudget` = 32 768 node visits per blast. `graphsBuilt` and `chunksDecoded` depend on the
  cache's history: logged only, never a budget input. The cast work budget charges `chunkGraphs` at a cold price (R5).
- `SimulationConfig::islandSearch` = `IslandSearchKind::Graph` (default) or `Voxel` (the stage-1 reference);
  `checkIslandSearch` (tests) runs the other one on the same voxels before anything is applied and counts differences
  (`Simulation::IslandSearchCheck()`).

### Propagation over ticks (server or offline)

*Superseded by the SI plan except where a line says built: `CollapseBudget` and `StructuralSolver` are dropped; new
pieces ask `sim::TakePieceSlot`; what a check's part cap left is re-checked by [si-3]. Kept because code comments and
the SI plan refer to its rules.*

```cpp
struct CollapseBudget { uint32_t nodeVisitsPerTick = 20000; uint8_t solvesPerTick = 2;
                        uint8_t releasesPerTick = 6;        // Detach ops of any netId
                        uint8_t serverBodiesPerTick = 2;    // new netId != 0 pieces: 120/s at 60 Hz
                        uint32_t eventBytesPerTick = 400; };   // (e), tuned in stage 5
class StructuralSolver {                       // owned by sim::Simulation, server / offline only
    void MarkDirty(const glm::ivec3& minVoxel, const glm::ivec3& maxVoxel);  // from WorldOp::Bounds
    void Tick(Simulation&, OpBatch& out);      // solve within budget, emit Detach / ClearMask in order
};
```

- Every applied op marks its integer bounds dirty, so the dirty set replays. Dirty structures are solved round-robin
  by first-dirty tick, then by box index.
- Releases are queued, largest and lowest first; at most `releasesPerTick` per tick and inside the event byte
  budget (24 kB/s, burst 16 kB, networking.md "Time and ticks"). Queued parts stay static and solid until their turn
  (collapse.md "Run time", step 7). A release dirties its neighbours, so cascades unfold over ticks: a castle crumbles over
  0.5-2 s (e) instead of one spike.
- **New server bodies are limited by what a client can build**, not by the server: a client builds about 4 bodies
  per frame (0.3-0.5 ms each, collapse.md "Run time", e) inside fx-budget's 4 ms CPU (fx-budget.md "Targets"), which at 30 fps is
  about 120 per second. So `serverBodiesPerTick = 2` (120/s; built as `sim::TakePieceSlot`'s 2 per tick, [net-6]); further releases in the same tick are `netId 0`
  rubble (subject to stage 1e's caps, then particles).
- **Client side of a queued piece**: a client applies every op at once in `opSeq` order (it must: later ops may touch
  the same chunks). For a `Detach` whose body cannot be built this frame, the voxels are cleared, the piece volume is
  kept (`BuildDetachedPiece`), and dust is shown along the cut until the body exists; a `netId != 0` piece then
  appears at its latest snapshot pose (a kinematic copy, networking.md "Entity replication"), a `netId 0` piece at its start pose
  if it is built within 4 frames, otherwise as particles. Holding back the `Detach` itself was rejected: it would
  hold back every later op.
- Released groups are cut by collapse.md's bricks or by `Fracture`; pieces of at least the replicated threshold
  (0.512 m^3, `sim::kServerReplicatedPieceVolume`: 262 voxels at 0.125 m) become server bodies with a netId up to the
  64 awake / 256 live caps (networking.md "Entity replication"; built [net-6]); smaller or over-cap pieces are
  `netId 0` client rubble; under `MinPieceVoxels`, crumbs.
- After a release: `ActivateInBox(release bounds + 1 m)`. Bodies never count as support.
- `BlastResult` generalises to `OpBatch{cause: Blast | Collapse | Impact}` and `PresentBlast` to `PresentOps`, so
  collapse and impact releases get rubble and dust too; `Engine::Run` calls it next to `RetryIslandSearches`.
- Sleeping server bodies are evicted as `EntityDespawn{toRubble}` (networking.md "Entity replication"). Rubble lifetimes and caps
  are stage 1e, not here.

*Load flow (stage 7, an owner decision): resolved by Part 4b. What stays optional for stage 7, only if play shows the
need: a bending moment and a tip check per layer, necks that narrow sideways inside one cell, an editor neck report
("Stages"; the original note is in [history/shred/stage-4.md](../history/shred/stage-4.md)).*

## Part 4b - Weak necks (load check)

The owner asked, with a screenshot, that a whole watch tower standing on a 4-voxel stone neck must not hold: a thin neck
must break under the weight above it. Owner decisions (2026-11-12): build it now, before networking stage 3 ("Yes, weak
necks next"); rock stays strong enough that natural cliffs and overhangs do not fall by themselves; whole castles may
collapse. Three designs were written independently (a horizontal cross-section sweep, a load-flow DAG, a bounded
min-cut); this part is the judged result ("Judgement of the three designs" in
[history/shred/stage-4b.md](../history/shred/stage-4b.md)). Built and reviewed [shred-4b]; every time marked **(e)** is
an estimate. "Open question N" in this part means Part 4b's own questions: they are in
[history/shred/owner-questions.md](../history/shred/owner-questions.md), their state in the header and in "Open questions
for the owner", item 11.

Binding, as for every structural stage (Part 2): the server (or offline) decides, clients only apply (R1); integers
only, fixed visit order, a result that depends on nothing but the voxels of the flood box, the material table and the
session voxel size - not on cache state or worker count (R2, R4); output is ordinary `Detach` / `ClearMask` operations,
no new `WorldOpKind`, no wire change; `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` stays green; GPU-free
in `Vortex::Core` / `Vortex::Sim`; work per cast bounded by counts that follow from the voxels (R5).

### Decision in brief

1. **Rule: a part falls when no distribution of its weight fits the strength of what holds it.** After a blast's
   island search, inside the same graph pass, one integer **max-flow** problem is solved around the blast: every node
   supplies its mass, every link carries at most `bond faces x per-face strength` (compression, shear or tension,
   by the link's direction), and the anchors (everything that reaches the flood box face, the window's bottom row,
   indestructible voxels) are the sink. If all mass reaches the sink, nothing happens. Otherwise the **largest
   strictly overloaded set** - the nodes that cannot reach the sink in the residual graph of a maximum preflow - is
   released as ordinary `Detach` pieces (`netId 0`), crumbs in the blast's one `ClearMask`.
2. **The released set is unique**: it does not depend on the order the flow was found in (min-cut theory), so the
   decision is exact and replays. Parallel supports add up. Every released piece satisfies `load > capacity`
   strictly (load equal to capacity holds). What stays provably holds without the released part, so **no cascade
   step and no follow-up queue are needed**.
3. **Necks inside a cell are found exactly**: stage 4's graph only sees cross-sections on cell faces (every 8
   voxels). Each node now stores its narrowest horizontal plane, computed when the graph is built; a constricted
   node is split there into a lower and an upper half for the check, and its side links are split bit for bit by
   height. A neck is found at any of the 8 heights inside a cell.
4. **Scope: the island box, judged; the rest of the flood box, contracted.** Nodes of the island box (left after the
   islands) are judged. Outside it, what is closed inside the flood box is a load hanging on them, and what reaches
   the flood box face is an anchor. Natural cliffs and hills always reach the face, so they are never judged; rock
   that is judged keeps a margin of 3x or more in every natural case below.
5. **Materials**: per material three integer strengths in kg/m^2 (compression, shear, tension); foliage weighs
   nothing for the check. Stone: 100 000 / 40 000 / 15 000. The watch tower falls on any neck under 24 voxel faces
   (0.375 m^2 at 0.125 m), stands on its full base with a 5.2x margin.
6. **Cost**: est. 0.05-0.3 ms per typical cast, about 3-4 ms at the per-cast budgets; nothing on a session client.
   *(Measured in the review: 0.13-0.73 ms at a cached site plus graph builds at a cold one; 5-6 ms for the preflow
   alone at its budget, open question 7.)*

### Units and the integer formula

- **Mass** is `GraphNode::mass`: the sum of `densityQ` (kg/m^3, integer) over the node's
  voxels; call the unit MU. kg = MU x vs^3. Foliage gets `densityQ` 0 in the graph tables (weightless for the
  check, below); `GraphNode::mass` has no other consumer (the build and the tests).
- **Strength** per material and class k in {C compression, S shear, T tension}: `strength[k]` in kg/m^2, an
  **integer** field of `Material` (no float to round). One voxel face carries `sigma x vs^2` kg, so per face in MU:

  ```
  capFace[k][id] = strength[k][id] * 1 000 000 / vsMicro       // uint64, floor
  vsMicro        = llround(World::VoxelSize() * 1e6)            // 125 000 at 0.125 m, 100 000 at 0.1, 150 000 at 0.15
  ```

  That is x8 at 0.125 m, x10 at 0.1 m, x6.67 (floor) at 0.15 m. `VoxelSize()` is the session float both sides hold
  bit for bit (the same one `MinPieceVoxels` uses), and only the server computes this. The rule is physical: the same
  structure in kg and m^2 falls or stands at every voxel size, up to how its shape rounds.
- **A link's capacity** from node u to node v across face f of u: `bond x min(capFace[k][weak[k](u)],
  capFace[k][weak[k](v)])` with k = C when f is -y (u rests on v), T when f is +y (u hangs from v), S for x and z
  faces. `bond` is `GraphLink::bond` inside a chunk, `BondVoxels` across. A node's
  `weak[k]` is its weakest material for class k, so a bond is as strong as the weaker side: stone on planks holds
  what the planks hold.
- **Overflow**: capacities and supplies are uint64. Per arc at most 64 faces x 10 000 000 MU (iron at 0.1 m) =
  6.4e8; total supply at most 12.6 M voxels (network and load limits below) x 19 300 (gold) = 2.4e11. With the
  strictness factor `M` = 2^17 (next section) everything stays under 3.2e16 < 1.8e19. Infinite arcs use a sentinel.

### The rule, exactly

The network (next section) has a source s with an arc `s -> v` of capacity `mass(v)` per node, arcs between linked
nodes as above, and arcs to the sink t from anchored nodes. Solve a maximum preflow.

- **Strict rule by perturbation.** Every finite arc capacity c is used as `c x M + 1`, every supply as
  `mass x M`, with `M = 2^17` larger than the number of directed arcs (the network is refused above 65 535 arc
  pairs). For a set A, `supply'(A) - cap'(A -> out) = M x (mass(A) - cap(A -> out)) - outArcs(A)`: it is positive
  exactly when `mass(A) > cap(A -> out)` in real units. So "all supply reaches t" holds exactly when no set is
  strictly overloaded, and a load equal to its capacity holds.
- **Released set** A = the network nodes that cannot reach t in the residual graph of the maximum preflow. It is the
  largest maximiser of `supply' - cap'(out)` and the same for every maximum preflow (arcs from the kept side into A
  carry no flow, so turning the preflow into a flow changes nothing on the kept side). Consequences:
  - each connected part P of A (over network links, saturated or not) has `mass(P) > cap(P -> out)` strictly;
  - the kept side routes all of its own mass to t without A: after the release it holds, so **nothing cascades**
    within the network, and no node of the kept side is cut off from an anchor (each one reaches t through links);
  - of two cuts of the same strength the lower one wins (more mass above it, so more overload): the tower takes its
    neck down to the lowest plane of the same strength instead of leaving a stub (except where that plane lies
    inside an unsplit node, failure mode 1);
  - a weightless part hanging only from a released piece goes with it (no leaves left in the air).
- If all supply reaches t (`F == supply'`), nothing is released and the pass ends after the first sweep.

### Graph additions (built with the chunk graph, cached, pure functions of the chunk)

`GraphNode` holds five bytes for the check, fields reordered so it stays **72 bytes**
(`static_assert(sizeof(GraphNode) == 72)`): `cutFaces` (u8), `cutPlane` (u8), `weak[3]` (u8 each). Layout: `face[6]`,
`mass`, `voxels`, `firstVoxel`, `firstLink`, `linkCount`, `cutFaces`, `cutPlane`, `cell`, `boxMin[3]`, `boxMax[3]`,
`weak[3]`.

- **`weak[k]`**: the id with the lowest `strength[k]` among the node's voxels, ties to the smaller id. Foliage is left out unless
  the node holds nothing else (review of 4b). A cell of one id costs nothing; a mixed 4^3 block collects its solid-id
  set in the loop that already sums its mass, so a single-node mixed cell takes its weakest ids from the cell's id set,
  and only a cell with several nodes and several ids reads ids per voxel. The shared full Uniform graph gets
  `weak = uniformId`.
- **`cutPlane` bit 7, anchor**: the node holds a voxel whose material is `Indestructible` and not ignored (bedrock;
  water and lava are ignored, so never). Such a node is an anchor.
- **The narrowest plane** (`cutFaces`, `cutPlane` bits 0-2). For each horizontal plane k between cell-local layers
  k and k+1 that the node spans, its cut `c[k]` = voxel faces of the node across it, and each layer's area `a[y]`:
  with z slices `s` (bit x + 8y), `t = s & (s >> 8)` holds plane k's faces in byte k, and a bytewise popcount summed
  over the 8 slices gives all 7 cuts at once (and the same on `s` gives the 8 areas), about 20-30 ns per node. Then
  `k*` = the lowest k with `c[k] = min c` and `c[k] < min(max a[y <= k], max a[y > k])` (a real constriction: wider
  on both sides). If such a k* exists **and** the node's voxels at layers <= k* form one 6-connected component and
  those above form one (the cell flood the build already has, run on each half; only for constricted nodes, which
  are rare away from necks), store `cutFaces = c[k*]` (1..63) and `cutPlane = k*`. Otherwise `cutFaces = 255`: the
  node is never split. Slopes, plates, full cells and columns of constant width are not constricted; an hourglass
  left between two craters, or a neck with the wall above and the ring below in one cell, is.
- **Why the halves must be connected**: a released upper half must not leave a lower part hanging that was joined
  to the rest only through it (a U shape inside one cell). Such nodes are not split; the check then sees the cell
  faces only there (lenient).
- **`GraphMaterials`** holds `strength[3][256]` (uint32, kg/m^2), `anchor[256]` and `foliage[256]`; `densityQ` is 0
  for `MaterialFlag_Foliage`. They are part of `operator==`, so a table change drops the cache. Without a library
  every id gets stone's strengths and no anchor.
- **Build cost** (m, `Structure.DISABLED_GraphBuildCost`, quiet, game terrain at 0.125 m): Dense 29-31 us per chunk
  (25-28 without the check's fields), Packed 22-24 us, decode included. Memory unchanged.

### Material strengths (`Material::strength[3]`, kg/m^2, integers; `RegisterDefaultWorldMaterials`)

Closed forms make the table readable at any voxel size: a free column of height h holds while `rho x h <= C`; a
prismatic cantilever of length L holds at its root while `rho x L <= S`; a hanging piece of length h while
`rho x h <= T`; a slab of span w over a hole, held at its rim, while `rho x w / 4 <= S`.

| Material | rho | C | S | T | Per face at 0.125 m, MU (C / S / T) | Column / cantilever / hanging limits |
|---|---|---|---|---|---|---|
| stone, crystal | 2600 | 100 000 | 40 000 | 15 000 | 800 000 / 320 000 / 120 000 | 38 m / 15.4 m / 5.8 m |
| stone_dark | 2800 | 150 000 | 60 000 | 20 000 | 1 200 000 / 480 000 / 160 000 | 54 m / 21 m / 7.1 m |
| brick | 1900 | 80 000 | 30 000 | 8 000 | 640 000 / 240 000 / 64 000 | 42 m / 15.8 m / 4.2 m |
| wood | 700 | 80 000 | 30 000 | 30 000 | 640 000 / 240 000 / 240 000 | 114 m / 43 m / 43 m |
| planks | 600 | 50 000 | 25 000 | 25 000 | 400 000 / 200 000 / 200 000 | 83 m / 42 m / 42 m |
| dirt, grass, grass_dry | 1500 / 1300 | 50 000 | 8 000 | 2 000 | 400 000 / 64 000 / 16 000 | 33 m / 5.3 m / 1.3 m (dirt) |
| sand | 1600 | 40 000 | 3 000 | 500 | 320 000 / 24 000 / 4 000 | 25 m / 1.9 m / 0.3 m |
| snow (packed; C was 10 000 before the review) | 400 | 50 000 | 2 000 | 500 | 400 000 / 16 000 / 4 000 | 125 m / 5 m / 1.25 m |
| glass | 2500 | 60 000 | 10 000 | 4 000 | 480 000 / 80 000 / 32 000 | 24 m / 4 m / 1.6 m |
| iron | 7800 | 1 000 000 | 600 000 | 600 000 | 8 000 000 / 4 800 000 / 4 800 000 | 128 m / 77 m / 77 m |
| gold | 19300 | 150 000 | 80 000 | 80 000 | 1 200 000 / 640 000 / 640 000 | 7.8 m / 4.1 m / 4.1 m |
| torch | 500 | 20 000 | 8 000 | 8 000 | 160 000 / 64 000 / 64 000 | |
| leaves, leaves_autumn | weightless | 10 000 | 5 000 | 5 000 | 80 000 / 40 000 / 40 000 | carry what rests on them, weigh nothing; rate a node only when it holds nothing else (review) |
| bedrock (any `Indestructible`, not ignored) | | anchor | | | | never fails, always supports |
| water, lava | | ignored | | | | not in the graph (as today) |

Stone's C is the owner's knob (Part 4b question 1, built as recommended): it sets where the tower falls. Natural rock keeps a margin of 3x or
more at it (next table); soil less, by design (Part 4b question 5). They are compiled-in defaults, the same on both sides
of a session, hashed into `buildHash` through `voxel::MaterialTableHash` since [net-3] (R6); stage 3's
`MaterialPhysicsQ` would take them over.

### The owner's cases, with the arithmetic (0.125 m unless noted)

The watch tower (`sim::PlaygroundTemplate`, `S(x) = lround(0.8 x)` at 0.125 m), recomputed voxel for voxel: a stone shell 22 x 56
x 22 with walls 2 voxels thick (160 per row, 150 in rows 0-15 at the 5 x 2 doorway), planks 26 x 3 x 26 at rows
53-55, four 2 x 6 x 2 wood posts and 2 x 2 x 2 torches: **10 476 voxels** (stone 8320, planks 2028, wood 96, torch
32), **22 932 000 MU = 44.8 t**. The tower above row 12 (rows 13-55 and the roof) is 8526 voxels, 17 862 000 MU. The
neck test (`--neck-blast`, below) turns the ring's rows 1-12 (1.5 m) to air except a stone segment 2 voxels thick
and W long at the north-west corner inside the wall line, so it meets row 0 below and row 13 above with 2W faces.

| Case | Load, MU (kg) | Capacity, MU | Result |
|---|---|---|---|
| Tower on a 2 x 2 stone neck: released set = rows 1 and up (the tower plus the 48 neck voxels: 8574 voxels) | 17 986 800 (35 130 kg) | 4 x 800 000 = 3 200 000 (6 250 kg) | **falls**, 5.62x over |
| Same, neck 2 x 10 (20 faces) | 17 862 000 + 20 x 12 x 2600 = 18 486 000 | 16 000 000 | falls, 1.16x |
| Same, neck 2 x 11 (22 faces) | 18 548 400 | 17 600 000 | falls, 1.05x |
| Same, neck 2 x 12 (24 faces) | 18 610 800 | 19 200 000 | stands, 0.97. **Threshold: n <= 23 faces fall** (`17 862 000 + 31 200 n > 800 000 n` for n < 23.2), 0.375 m^2 holds |
| Four 2 x 4 necks, one per corner (32 faces) | 18 860 400 | 25 600 000 | stands (a widest-path rule would see 8 faces and drop it); one of them alone (8 faces, 18 111 600 against 6 400 000) falls |
| Tower on its full base: everything on the paving, 150 faces (min of stone and the stone_dark paving = stone) | 22 932 000 | 120 000 000 | **stands**, 0.19 (5.2x margin) |
| Roof on the walls (160 faces, planks C) / a corner post on the roof (4 faces, planks C) / the wall over the doorway (370 voxels, 148 shear faces) | 1 300 000 / 20 800 / 962 000 | 64 000 000 / 1 600 000 / 47 360 000 | stand (0.02 / 0.013 / 0.02) |
| 0.1 m: 2 x 2 neck rows 1-15 (19 244 voxels) / full base (282 faces) | 41 532 000 / 53 107 200 | 4 000 000 / 282 000 000 | falls 10.4x / stands 0.19 |
| 0.15 m: 2 x 2 neck rows 1-10 (5859 voxels) / full base (128 faces) | 12 541 000 / 16 097 800 | 2 666 664 / 85 333 248 | falls 4.70x / stands 0.19 |
| 3 m natural stone overhang, prismatic: per root face (shear) | 24 x 2600 = 62 400 | 320 000 | **stands**, 0.20 (stone_dark 67 200 / 480 000 = 0.14) |
| 3 m slab, 1 m thick, 2 m wide (3072 voxels), root of 128 faces | 7 987 200 | 40 960 000 | stands 0.20; falls when its root is cut to 24 faces or fewer (19 %) |
| Mushroom rock: 3 x 3 x 1 m cap on a 1 x 1 m stem, 2 m tall (5632 voxels), stem 64 faces | 14 643 200 | 51 200 000 | stands, 0.29 |
| Hillside, cliff, cave walls | - | - | **not judged**: they reach the flood box face (anchors). Inside the island box rock is full cells, 64 faces per link, with paths in every direction; a 1.2 m crater cannot leave a set whose every load path is overloaded unless it cuts it nearly free |
| Cave ceiling of stone / dirt / sand, span w, held at its rim | rho w / 4 per face | S | holds to w = 61 / 21 / 7.5 m |
| Underground 1.2 m blast 1.5 m deep (`--climb`-like, e): a 0.3 m dirt roof about 2.4 m across | 1500 x 2.4 / 4 = 900 kg/m^2 | dirt S 8 000 | stands |
| Dirt overhang 3 m / sand overhang 2 m | 4 500 / 3 200 kg/m^2 | 8 000 / 3 000 | dirt stands (0.56), sand slumps (intended) |
| Wooden beam 3 x 3 x 48 (6 m) butted into stone at both ends: 18 shear faces, min(wood, stone) S | 302 400 | 4 320 000 | **stands**, 0.07 |
| Plank deck 8 m span, 8 wide, 2 thick, 4 voxels of bearing at each end (64 faces) | 691 200 | 25 600 000 | stands, 0.03 |
| Playground timber frame: one 2 x 2 wood post with its share of deck and beams, on the paving | about 340 000 (e) | 4 x 640 000 = 2 560 000 | stands, 0.13 |
| Generated tree, 1-voxel trunk 2.2-4.6 m (18-37 wood voxels) rooted in soil, canopy up to about 26 800 leaves | at most 25 900 (leaves weightless) | min(wood, dirt) C 400 000 | stands, 0.06. With leaves weighing 200 kg/m^3 the canopy alone is 5.36 M MU on one face: it would fall, hence weightless foliage |
| Castle curtain wall 10 m, 0.5 m thick (stone, per face) | 80 x 2600 = 208 000 | 800 000 | stands 0.26; the castle is over 60 000 voxels anyway, so it is an anchor in 4b (Part 4b question 2, [shred-4c]) |
| Test arena mushrooms (`tests/sim/BlastTests.cpp`, 0.1 m): stone cap and stem / planks cap and stone stem, 9 stem faces | 1 318 200 / 592 200 | 9 000 000 (4 500 000 if the floor were soil) | stand: `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` keeps its operations |

`--tower-blast` (7 shots round the ring) can free the tower one or two shots early, once the ring's remaining section
is under about 23 faces: its acceptance is "freed by shot K <= 7, by an island or by the load check (the log says
which)". As built it is freed by shot 6 of 7 by the load check (2 faces left).

### The check, step by step

Phase **3b** of the graph search (`NeckCheck`, called by `SearchOnce` in `voxel/internal/Structure.cpp`): after the
island floods (step 3) and before the re-labelling (step 4). It runs only when `limits.neck != nullptr`, the island box was searched and the
face floods are complete (`result.searched && result.complete`). It uses the same `GraphTable` (chunks resolved
once, `chunkGraphs` counted once, `ForEachLink` order -x +x -y +y -z +z then link order) and reads
`table.state` only for `kIsland`. Island nodes (in-box and face, crumbs included) are not in the network.

1. **R (judged nodes).** The seeds of step 1 (nodes whose bounds meet the island box, chunks z, y, x, then graph
   order) that are not `kIsland`, in that order. A node with `cutFaces != 255` becomes two
   network nodes, lower then upper. Network index = this order. If R and its loads (step 2) exceed
   `kNeckMaxNetworkNodes` (8192) network nodes, or the arcs (step 3) 65 535 pairs, the check is skipped and nothing
   is released (`neckSkipped`, a warning).
2. **Exterior floods.** For each R node in order and each of its links, in `ForEachLink` order, to a node that is
   neither in R nor `kIsland` nor labelled yet: a FIFO flood over such nodes, inside the flood box. It ends as an
   **anchor** on a node with a voxel on the flood box face layer or the window's bottom row (the face flood's test), on an anchor-material node, on more than `kMaxIslandVoxels` voxels, on a node an earlier
   flood labelled anchor, or once `kNeckExteriorVisitBudget` (16 384 node visits per check) is spent (that flood and
   every later one: `neckExteriorBudget`, conservative). **As built (review):** also on entering a chunk solid
   through (32^3 solid voxels, whatever its storage; asked before the chunk is resolved, `neckSolidChunks`).
   Otherwise it closes: a **load** (members, mass, voxels). All its visited nodes get its label (as built, a flood that meets a load label - which cannot happen: a closed load
   holds its whole component - is taken as an anchor). A chunk that is not
   loaded is **support** for this check, which can only make it release less than the full answer (adding support
   only shrinks the largest overloaded set): on a server `neckUnloaded` counts an error (the flood box is loaded by
   construction, `kServerWindowMarginChunks`); offline it is counted (`neckLoading` inside the window,
   `neckBeyondWindow` outside) and `sim::Blast` queues a late search that runs island search and check again once that
   chunk has loaded (`neckDeferred`; a chunk outside the window loads once the window moved over it). **The islands
   are not held back** (the design held the face islands with the check; the review found that slowed them while
   chunks stream). The
   per-slot label array lives in `GraphTable`, resized with `state` / `seed` whenever a flood resolves a new chunk
   (trap: floods resolve chunks). Loads get network indices after R, in the order they close.
3. **Network.** For each network node in index order, its links in `ForEachLink` order, each pair once (the twin
   link is skipped by index):
   - R-R: one arc pair, capacities by direction class and `weak` (above). For a split node a -y link belongs to the
     lower half, a +y link to the upper half, and an x or z link is split by its overlap bits
     `a.face[f] & b.face[f ^ 1]` (x face: y = bit & 7; z face: y = bit >> 3): bits at y <= k* go to the lower half,
     the rest to the upper half, each part its own arc pair with its own popcount. Two split nodes side by side are
     split on both ends the same way. Inside a split node: upper -> lower `cutFaces x capFace[C][weak[C]]`, lower ->
     upper `cutFaces x capFace[T][weak[T]]`.
   - R-load: capacities summed per (R node, load), in first-encounter order. R-anchor: summed into one arc R -> t.
   - R nodes with the anchor bit, a voxel on the flood box face layer or a voxel on the window's bottom row: an
     infinite arc to t, from **both** halves of a split node (as built: the anchor voxel may be in either half).
   - Supplies: node mass; a split node's halves get the exact mass of their voxels (`NodeVoxels`,
     through the search's `VoxelSource`, one table lookup per voxel; counted in
     `neckSplitRelabels`); a load its total.
4. **Maximum preflow** (push-relabel, all integer, fixed order): exact initial labels by a reverse BFS from t over
   arcs with residual capacity; nodes that cannot reach t get label n. Active nodes first in label order descending,
   ties by index (the "gravity sweep": in a structure that holds, all mass flows down in this one pass), then FIFO,
   current-arc pointers in arc order, a global relabel every `max(64, n / 4)` relabels (a count, not a time). Stop
   when no active node has a label below n. Work = arc scans (discharges and BFS) against `kNeckFlowWorkBudget`
   (**65 536** since [net-3]; 262 144 in [shred-4b]); if it is spent, nothing is released (`neckFlowBudget`, a
   warning). As built, every finite arc capacity is perturbed after summing (`c x 2^17 + 1` per arc pair direction),
   supplies are `mass x 2^17`, infinite arcs `2^62`.
5. **Released set and pieces.** If all supply reached t: done - nothing is released, even a weightless part with no
   support at all (that is the island search's business, e.g. a component it gave up on by budget). Otherwise a reverse BFS from t over residual arcs; A =
   the nodes it does not reach. **Pieces** = connected parts of A over network links (loads expand to their member
   nodes), ordered by their smallest network index. Per piece: voxels, load (MU), cut (sum of the real capacities of
   its outgoing arcs) and cut faces, for the log and the tests. A piece over `kMaxIslandVoxels` stays (counted
   `neckOversize`; castle scale, Part 4b question 2, [shred-4c]). **As built (review):** at most `kNeckMaxReleasedParts` (64) parts
   and `kNeckMaxReleasedVoxels` (60 000) voxels are released per check, parts in piece order while both fit; the
   first that does not and every later one stay (`neckDeferredParts`). Exact and order-free: parts of A have no
   network arc between them, so each one left is still overloaded on its own (released by a later cast near it), and
   the kept side holds with them in place (the preflow routed their cut capacity through it).
6. **Voxels.** Released nodes are re-labelled with `NodeVoxels` (an upper half keeps its layers > k*) and the stage-4
   stale-graph check (count, face masks, bounds; since the review also mass, weakest materials, anchor bit and plane:
   `SameAsGraph`): a mismatch counts `relabelMismatch` and the whole search runs again on fresh graphs. Pieces go
   into a new
   `GraphSearchResult::neckPieces` (`std::vector<FloatingIsland>`, plus `std::vector<NeckCut>` with load, cut, faces);
   they bypass the island step's "no voxel in the island box" drop and the face ordering.
7. **Operations.** `SearchIslands` (`sim/internal/Blast.cpp`) emits, in this order: carve; in-box islands (scan
   order); face islands (candidate order); **neck pieces (piece order)**; one `ClearMask` for every crumb of the three
   lists. A neck piece of at least `MinPieceVoxels(vs)` is `WorldOp::Detach(origin, mask, BlastPush(centre, radius,
   corner))`, `netId 0`, applied at once like an island; a smaller one is a crumb. A neck piece
   touches an R node (or is one) - **a released part may be a load alone** (review: sand hanging in tension under a
   ceiling that holds, `Neck.HangingLoadIsReleasedAloneItsNodeStays`) - and an R node's voxels lie at most 7 voxels
   outside the island box, so a crumb reaches past the island box by less than `MinPieceVoxels + 7` and the
   `ClearMask` box stays under `192 + 2 x (MinPieceVoxels + 7)` voxels per axis (networking.md "Crumbs", corrected: [history](../history/networking/stage-2.md)).
   `PresentBlast` makes rubble from it as from any island (stage 1e lifetimes and caps).

### When it runs, and when it does not

- After every `sim::Blast` with the graph search, duds included (a power-0 blast is how the tests and the pristine
  check probe a structure), and in every late search (`RetryIslandSearches`).
- Never after editor or game `FillBox`, brush edits, generation, loading, or a client's `ApplyOp` (R1). So a
  structure is never judged without a cast near it: the answer to collapse.md's "buildings fall on spawn"
  (collapse.md "Decision", rejected B), and why the owner's screenshot (a neck cut by `FillBox`) stood until a shot landed near it.
- Not after its own release: the remainder is proven to hold.
- `SimulationConfig::neckRule` (default true); off with `IslandSearchKind::Voxel` (the
  reference has no load check). `checkIslandSearch` / `SameResult` keep comparing islands only.
- Planned: [si-2]'s craters run it like a blast, and [si-3]'s re-checks run it for parts a check's cap left (the SI
  plan dropped stage 5's `StructuralSolver` and its dirty boxes).

### Determinism and replication

- Inputs: the voxels of the flood box (through the graphs, pure functions of the chunks), the material table, the
  session's `vsMicro`. Nothing outside the flood box is read (anchors stop at its face layer).
- Every order is fixed: R by seed order, loads by closing order, arcs by `ForEachLink` order, the preflow by labels
  and indices, pieces by smallest index. The released set does not depend on any of these orders (step 5), which
  `Neck.ReleasedSetIndependentOfArcOrder` pins; the work counters do depend on them, and they are fixed.
- Clients only apply the `Detach` / `ClearMask` the server sends; no `WorldOpKind`, `Detach`, `ClearMask` or wire
  change. The search result for the same input changes (more pieces fall): a server decision, as stage 1 and 4.

### Work bound per cast (R5) and cost

Constants next to `kGraphNodeVisitBudget` (`sim/Blast.cppm`): `kNeckMaxNetworkNodes` = 8192,
`kNeckMaxArcPairs` = 65 535, `kNeckExteriorVisitBudget` = 16 384 node visits, `kNeckFlowWorkBudget` = 65 536 arc
scans (262 144 in [shred-4b], lowered in [net-3]); since the review also `kNeckMaxReleasedVoxels` = 60 000 and `kNeckMaxReleasedParts` = 64 per check (what the
re-label, the `Detach` ops and the wire may carry from one cast). Counters (`IslandSearchStats`, the `Blast:` line): `neckNodes`, `neckSplit`, `neckLoads`, `neckAnchors`,
`neckArcs`, `neckExteriorVisits`, `neckFlowWork`, `neckSplitRelabels`, `neckPieces`, `neckCrumbs`, `neckVoxels`,
`neckOversize`, `neckSkipped`, `neckExteriorBudget`, `neckFlowBudget`, `neckUnloaded`; `neckMs` logged only. All of
them follow from the flood box voxels: equal cold or warm, with 1 or 3 workers. Since the review also
`neckDeferredParts`, `neckSolidChunks`, `neckBuildMs` (logged only) and, offline, `neckLoading` / `neckBeyondWindow` /
`neckDeferred` (these follow what is loaded). The cast work budget (`sim::CastWorkOf`, networking.md "Time and ticks")
adds `neckExteriorVisits + neckArcs + neckFlowWork / 8 + 64 x neckSplitRelabels + neckVoxels / 64` x `sim::kNeckWorkScale` (2; weights
calibrated once); graphs the exterior floods resolve are already in `chunkGraphs` at the cold price.

| Case | Work (e) | Time (e, -O2) |
|---|---|---|
| Surface 1.2 m blast, nothing overloaded | R 400-1500 nodes, 2-6 k arc pairs; exterior 150-3000 visits (a ground flood stops at 60 000 voxels, about 118 full nodes; later floods stop at its labels); one gravity sweep, 5-15 k arc scans | 0.05-0.3 ms |
| The tower's neck shot (the release) | the sweep, about n relabels and one global relabel (20-60 k scans), 1-4 split nodes, re-label of 8574 voxels; ops and presentation as the measured freeing island (ops 1.05 ms, present 1.25 ms, Part 1) | 0.2-0.6 ms of check |
| Underground (`--climb`) | 15-25 ground floods, 2-4 k visits | 0.2-0.5 ms |
| Worst at the budgets | 16 k visits x 50-100 ns + 262 k scans x 4-8 ns + 65 k arcs to build + re-label up to 60 000 voxels | about 3-4 ms. **Measured in the review** (`Neck.DISABLED_CostAtTheBudgets`, quiet, -O2): the preflow alone 5-6 ms at 207 k arc scans on 8192 nodes / 65 535 arc pairs; 529 overloaded pillars (3574 nodes, 3963 visits, 45 k work, 64 parts released by the cap) 2.0-2.4 ms; a 55 936-voxel gold block released 0.8-0.9 ms (3.3 once). Est. 8-12 ms with every budget full: the per-arc cost is 20-30 ns, not 4-8 |

- **Server, 5v5 at 10 casts/s**: about 3 ms of CPU per second on average (0.05 ms per tick); a worst cast stays
  inside the 6 ms cast phase, and its tick is decided by counts, not time. A release adds one `Detach` (the tower's
  mask about 1-2 KB RLE, inside the 16 kB event burst).
- **Session client on the UHD 620**: nothing new runs; a release is one `Detach` and one rubble body, as an island.
- **Offline (the game)**: +0.05-0.6 ms on a blast frame; a typical cast stays inside the 3 ms rule, the worst
  does not (as cold graph builds).
- Memory: `thread_local` network arrays, about 1-2 MB at the node cap; nothing new is cached.

### Tests

GPU-free. `Neck.*` (VortexCoreTests, `tests/core/NeckTests.cpp`): brute-force max-flow, arc-order independence, a
per-voxel reference network, the tower on its neck at every height mod 8 and across chunk faces, exact thresholds,
the rock gate `Neck.NaturalTerrainNeverFails`, budgets equal cold / warm and with 1 / 3 workers, stale graphs.
`Structure.*` (`tests/core/StructureTests.cpp` and `NeckTests.cpp`): the graph against `LabelComponents`, plane cuts,
weakest materials and the anchor bit against brute force, cache refresh and eviction. VortexSimTests: `Blast.Neck*`
(`tests/sim/NeckTests.cpp`: an ordinary `Detach`, equal hashes in two simulations, replay, server and offline unloaded
chunks), `Rubble.TowerNeckReleasesStayUnderCaps`, and the stage-1 face flood in `Blast.*` / `FloatingIslands.*`
(`tests/sim/FaceFloodTests.cpp`, `BlastTests.cpp`). Every stage-1 arena runs with `checkIslandSearch` and fails if the
load check releases anything there (`IslandSearchChecks::neckReleases`). The test plan and the list as built:
[history/shred/stage-4b.md](../history/shred/stage-4b.md).

### Headless `--neck-blast [W]` (game, next to `UpdateTowerBlast`)

1. **Frame 20, pristine:** six duds (`BlastDesc{p, 1.2f, 0.0f}`) at the tower base, both brick walls, the timber
   frame, the crystals and the metal blocks. Logs `Neck: pristine playground: 6 duds (N checked), P pieces released
   (I island(s) / crumb(s) that already floated)`; P must be 0. (Islands and crumbs that already floated as built are
   counted apart: the island search takes them on any dud.)
2. **Frame 30, the owner's screenshot:** `World::FillBox` turns the tower ring's rows 1 .. lround(1.5 m / vs) to air
   except a stone segment 2 x W (default W = 2) at the north-west corner inside the wall line. No check runs for
   `FillBox`: logs `Neck: tower on a 2 x W stone neck (2W faces, rows 1..R) at (x, y, z) m, roof solid`.
3. **Frame 40, the shot:** 0.6 m, power 6, at the centre of the tower's hollow 0.5 m above the floor (it carves only
   paving; the neck is about 1.9 m away, inside the island box). Logs `Neck: tower released: N piece(s), the first of V
   voxels, load L MU (x t) > cut C MU (F faces, inside a cell | on a cell face), R x; network n nodes (s split), l
   loads, a arcs, v exterior visits, w flow work, X ms; roof AIR`, or `Neck: tower stands, roof solid; ...`; then the
   piece's speed at +1 and +5 frames (as `--tower-blast`) and `store a/b`.

The pass values are CLAUDE.md's: W = 2 releases at 0.125, 0.1 and 0.15 m, W = 11 releases, W = 12 stands, the duds
release 0 pieces. Measured [shred-4b] (m): W = 2 at 0.125 m releases 8558 voxels, 5.61x (the cut is the cell face 4 rows
up the neck at this floor height); W = 11 1.04x; W = 2 at 0.1 / 0.15 m 10.37x / 4.69x. The other runs keep their
results: `--climb` (its blast releases nothing), `--probe-blast` (`0 floating island(s)`), `--castle-blast` (neck
counters logged, 0 pieces: the castle is an anchor), `--tower-blast` (freed by shot K <= 7, says how). Not confirmable
headless, for the owner: the fall seen in a window, its rubble, and the cost on a busy UHD 620.

### Failure modes, stated

1. **Cell granularity.** Necks are judged on cell faces and on the one narrowest horizontal plane of a constricted
   node whose halves are each connected. Two narrow planes in one node, U shapes, and necks that narrow sideways
   (along x or z) inside one cell are seen only at cell faces: they hold (lenient). A neck stub of up to 7 voxel rows
   may stay below a release.
2. **Weakest material per node and class.** A node with one glass voxel among stone is rated as glass, whether or not
   that voxel is on the bond. Errs toward failing. **Corrected in the review:** natural terrain does mix weak
   materials into strong ones - sand (S 3 000, T 500) over stone on beaches, snow over dirt above the snow line, leaves
   against anything a canopy touches - so a stone node with a sand voxel is rated as sand. Two changes keep that from
   dropping structures: **foliage is left out of a node's weakest material unless the node holds nothing else**
   (`Neck.StoneWallWithLeavesStands`: a 6 m wall with leaves against it was released, 1.3x over the leaves' 80 000 MU
   per face), and **snow's compression is 50 000** like dirt (`Neck.StoneWallOnSnowStands`: a 6 m wall on 0.4 m of snow
   fell at 10 000). Sand and snow still slump in shear and tension near a crater (intended, Part 4b question 5); the rock
   gate samples a beach, a snow field, a rock slope and deep caves and releases nothing there. Exact per-face
   material bonds stay rejected (voxel reads per link across chunks on every search).
3. **No bending moment, no tipping.** Long cantilevers, a beam cut at one end and slender towers do not fail by
   moment; a tower leaning over its neck holds if the neck has the faces. Stage 7 (moment / tip check) on the same
   check, only if play shows the need (the SI plan dropped the overhang rule).
4. **Locality.** Only the island box is judged. A neck formed by earlier shots is judged when a cast lands within the
   island box of it (about 4 m for a 1.2 m blast at 0.125 m); a shot at the roof does not re-check a neck 5 m below
   (the tower above the roof shot is part of an anchor through the neck). No re-check runs away from a cast (Part 4b question 3, built as
   recommended; [si-3] re-checks only what a part cap left).
5. **Exterior anchors are trusted**: anything that reaches the flood box face is ground, even if it hangs by a thread
   beyond it, and parts over 60 000 voxels are anchors - castles do not fall by this check in 4b (Part 4b question 2, [shred-4c]).
   Since the review a chunk solid through (32^3 solid voxels) is ground too: a chunk-aligned 4 m solid cube hanging
   on a neck outside the island box is held (`Neck.SolidChunkIsGround`; one voxel off the grid it falls).
6. **Pre-existing overloads are shaken loose** by any cast nearby, duds included (an editor-built structure that was
   too weak from the start falls at the first shot near its weak point). Counted in `neckPieces`; the pristine
   playground and terrain tests pin zero.
7. **Spent budgets release nothing** that cast (counted, warned); the structure stays until a later cast nearby (a
   spent budget is never re-queued: the SI plan). Not lost, but late (R7 in its weakest form). The same for parts over
   the release caps (64 parts / 60 000 voxels per cast, `neckDeferredParts`), until [si-3] re-checks them.
8. **Stale graphs**: released and split nodes are re-checked and the search retried, as for islands - since the review
   on everything the check reads (voxel count, face masks, bounds, mass, weakest materials, anchor bit, plane);
   kept judged nodes, loads and anchors are not re-read: for them `World::ChunkRevision` is the only guard.
9. **Foliage is weightless**: a canopy never overloads a trunk; leaves still carry (and can fail under) what rests
   on them.
10. **Strength-table drift** between a server and its replays changes decisions: the table is in `buildHash`
    (`voxel::MaterialTableHash`, R6, since [net-3]). Clients are not affected (they only apply).

### Rejected within this part

| Rejected | Why |
|---|---|
| Horizontal cross-section sweep as the rule | vertical only; a load hung below the cut counts as support; cascades need a queued follow-up over ticks; plates and an epoch sweep are more code than the flow for less physics. Its strict rule and its tests are kept |
| Load-flow DAG as the rule | the split of a node's load among its supporters is a heuristic that concentrates load (false falls near craters in rock), weakest-voxel strengths, plane validity rules and up to 4 rounds; its build-time plane cut is kept |
| Widest or bottleneck path | ignores parallel supports (four corner necks) |
| Smallest overloaded set | leaves weightless or lightly loaded parts in the air: the "second shot" bug in a new form |
| "Ties release" | a structure exactly at its capacity would fall only when something else overloads; replaced by the strict perturbation |
| A throat capacity from slice areas on every node | inexact (area is not the bond between layers) and caps sideways flow too; replaced by the exact split |
| Exact per-face material bonds | voxel reads per link across chunks on every search; the per-node weakest material is cached |
| A flow over the whole flood box | about 10x the network; the contracted exterior keeps it local and lets cliffs be anchors |
| Isotropic strength | either rock overhangs and soil roofs fail, or a neck under the tower holds; compression / shear / tension separate them, and sand slumps |
| Checking after `FillBox` / editor edits | "buildings fall on spawn" (collapse.md); a stage-5 editor report instead (open question 6) |

## Part 5 - Impact destruction by estimated weight (stage 6)

Superseded by the SI plan's [si-2] (impacts, below); crush damage to players moved to M2 (no health yet). The original
Part 5 (2026-11-12, never built) is in [history/shred/part-5-impacts-2026-11-12.md](../history/shred/part-5-impacts-2026-11-12.md).

## Budgets

### Server (60 Hz tick, 16.7 ms; the cast phase is a work budget, R5)

| Item | Cost |
|---|---|
| Cast work budget | `sim::kCastWorkPerTick` = 60 000 units per tick, about 6 ms on the reference machine (calibrated once, R5). A cast waits for a tick with credit; wall clock is only logged. |
| Island search on the graph [shred-4] | (m) quiet: 0.06-0.72 ms per cast on flat ground, 0.13-1.00 ms on game terrain, cold graph builds included. In game after `World: window filled`: 0.24-0.75 ms at a cached site, 0.5-3.7 ms at the first blast of an untouched site; while streaming 4.0-4.75 ms (`--climb`). A cold cast can build up to the island box's chunks (64 for a 1.2 m blast) at 70-120 us in game (4.5-7.7 ms): the budget charges `chunkGraphs` x `sim::kColdChunkGraphWork`. |
| Voxel search + face flood (stage 1, now the reference) | (m) quiet 3.3-5.6 ms per cast in the tower and castle cases, 8.8-12.4 ms underground; the flood costs about 50 ns per visit and is bounded by `kFloodVisitBudget` (240 000 visits, 2-6 ms). |
| Load check [shred-4b] | (m, quiet, on the same world as the search without it): 0.13-0.46 ms at a cached site on flat ground, 0.30-0.73 ms on game terrain, plus 0.0-1.8 ms of graph builds at a cold site. At the old flow budget the preflow alone took 5-6 ms at about 207 000 arc scans (8192 nodes, 65 535 arc pairs; est. 8-12 ms with every budget full), so the flow budget is 65 536 (est. 1.5 ms) and the work is charged. At most 64 parts / 60 000 voxels released per cast. |
| Per release: mask + `ClearMask` | 0.05-0.2 ms (e) |
| Per server body: colliders + body, no bake | 0.1-0.3 ms (e) |
| `physics.Step` with 64 awake compound bodies + terrain | 1-3 ms (e), unmeasured |
| Terrain collider re-cooks per edited chunk (Carve, Detach, ClearMask dirty the chunk and its 6 neighbours) | 0.1-0.4 ms each on workers (e); the main-thread scans est. 2-4 ms while debris flies (fx-budget.md "Where one blast costs time today") |
| SI per tick (designed) | object checks at most about 3.6 ms ([si-1]); impacts about 3-5 ms at worst, at most once per 6 ticks by the structure credit ([si-2]); one re-check 0.1-1 ms cached ([si-3]); all (e) |

### Client, UHD 620

The frame on the far view at 0.125 m was about 21.5 ms (m, [vd-3]: gbuffer 12.20 / lighting 7.28-7.38 / composite
1.24-1.40 ms); ROADMAP "Performance and measured values" holds the current numbers.

| Item | Cost |
|---|---|
| Destruction + effects, CPU main thread | <= 4 ms per frame (fx-budget.md "Targets") |
| Effects, GPU | <= 4 ms (particles 2.5, effect lights 0.5, uploads 1.0; fx-budget.md "Targets") |
| Applying a `ClearMask` | 1-2 ms for the largest measured mask, unoptimised (networking stage 2, "Crumbs" in [history](../history/networking/stage-2.md)); est. 0.2-0.6 ms optimised |
| Each local rubble body | 0.3-0.5 ms CPU (e, collapse.md "Run time") + 1-3 chunk-store slots of 36 936 B; capped by [shred-1e]: 48 rubble bodies of 25 voxels or more, 600 store slots, 192 small pieces; over a cap the oldest resting piece goes, else the new one becomes particles |
| Edited chunk uploads | whole chunks of 36 936 B; a 1.2 m shot edits 2-7 chunks (m, 0.07-0.26 MB), the shot that frees the tower 13 chunks + 2 rubble slots (m, 0.55 MB); target at most 16 per frame (fx-budget.md "`FxDirector` and its caps"); the renderer allows 128 (`RenderSettings::maxChunkUploadsPerFrame`) |
| **Per cast, offline (the game is the authority)** | carve + graph search + load check + `ClearMask` + bodies + uploads in one frame: inside the 3 ms rule ("Stages", stage 0) at a cached site, over it at a cold site and while streaming (m, [shred-4], [shred-4b]) |
| **Per cast, session client** (no search, no check) | apply ops est. 0.2-1 + bodies (capped) + uploads: est. 0.5-3 ms |
| **Client frame during 10 casts/s** (30 fps: 1 cast per 3 frames on average, bursts of 2-3 in one frame, e) | session client: est. 1-6 ms on burst frames, inside 4 ms only with the body cap and perf.md U1's upload budget; unmeasured (fx-budget stage 1's `--stress-fx`) |
| ChunkStore slots | filled window 5311 / 5629 at 0.125 m (m) + rubble <= 600 + server bodies <= 256 x 1-3 = 768 worst case: **about 7000 worst case**, at the game guideline |
| Extra instances in the ray-traced passes | unmeasured |

Wire: a castle collapse is est. 2-12 KB of masks spread over about 0.5 s by the event budget; a `ShredSphere` is about
20 B.

## Stages

Each stage: design note first where a contract changes, build, ctest, both headless runs (plus the stage's own),
screenshot, review, ROADMAP update in the same commit.

| ID | Stage | State | Commits (engine / game) | History |
|---|---|---|---|---|
| [shred-0] | Measure + one safe speed-up: a `Blast:` log line per blast (ms per phase, counters, edited chunks, upload bytes); the island box gathered by chunk | Built and reviewed | `9cd4960` / `597f555` | [stage-0-1e.md](../history/shred/stage-0-1e.md) |
| [shred-1] | Face flood (`voxel::ProveFaceComponents`) in the flood box; offline deferral, server error counter, rows above the ceiling as air, smallest-first candidates; server window margin 3 chunks; `--tower-blast`, `--castle-blast` | Built and reviewed | `9cd4960` / `597f555` | [stage-0-1e.md](../history/shred/stage-0-1e.md) |
| [shred-1b] | Carve replay before a late search (offline), only on the chunks the carve skipped, as they load | Built and reviewed | `9cd4960` / `597f555` | [stage-0-1e.md](../history/shred/stage-0-1e.md) |
| [shred-1c] | Nothing silent: drop / give-up / oversize / flood counters; a deferred search is given up only once its flood box left the window | Built and reviewed | `9cd4960` / `597f555` | [stage-0-1e.md](../history/shred/stage-0-1e.md) |
| [shred-1d] | Physics after destruction: a body is not simulated where the collider under it still holds edited voxels (frozen, velocity kept); wake on the first collider build of a chunk edited out of sight | Built and reviewed | `9cd4960` / `597f555` | [stage-0-1e.md](../history/shred/stage-0-1e.md) |
| [shred-1e] | Rubble budget: lifetime by size (15 / 20 / 30 s), `maxRubbleBodies` 48, `maxRubbleSlots` 600, `maxDebris` 192, a failed bake crumbles | Built and reviewed | `9cd4960` / `597f555` | [stage-0-1e.md](../history/shred/stage-0-1e.md) |
| [shred-2] | Crumble profiles, fracture-surface dust, carve lumps (client). Accepted when fx caps hold with `--stress-fx 10` (fx-budget stage 1), particles GPU <= 2.5 ms, screenshots per material. With fx-budget stages 1-2 | Not started | - | - |
| [shred-3] | `MaterialPhysicsQ` + `voxel:fracture`; client rubble fractured with LOD (presentation-only distance rule; lod.md stage 1 changed to match). Accepted when the `Fracture.*` tests and `MaterialQ.SaturatesAndKeepsIndestructible` pass and fracture stays under [shred-1e]'s caps | Not started (the material table is already hashed into `buildHash`, R6) | - | - |
| [shred-4] | Cell graph cached per terrain chunk; graph connectivity + node face flood replace the voxel search in the same flood box | Built and reviewed | `f83c6d6` / `5a9a4eb` | [stage-4.md](../history/shred/stage-4.md) |
| [shred-4b] | Weak necks (load check), Part 4b: integer strengths per material, node plane cut + weakest materials + anchor bit, max-flow check in the island box after the island floods; `--neck-blast` | Built and reviewed | `ce6a6da` / `305cfc1` | [stage-4b.md](../history/shred/stage-4b.md), [owner-questions.md](../history/shred/owner-questions.md) |
| [shred-4c] | Castle bricks (Part 4b question 2): exterior load limit 4 x 60 000, releases over 60 000 voxels (and oversize islands) split into chunk-aligned 32^3 bricks, each its own `Detach`, at most 240 000 released voxels per cast, wire bursts measured | Not started (owner question open) | - | [owner-questions.md](../history/shred/owner-questions.md) |
| [shred-5] | `StructureRegistry`, overhang rule inside structures, `StructuralSolver` with tick budgets and a release queue, spawn guard, `OpBatch` / `PresentOps`, `--collapse`; 5b: collapse.md stages 3-4 on objects | Superseded by [si-1] and [si-3] | - | [before-rewrite-2026-11-18.md](../history/shred/before-rewrite-2026-11-18.md) ("Stages") |
| [shred-6] | Impacts (Part 5): contacts drained, `ImpactQ`, cooldowns, crush check | Superseded by [si-2]; crush to M2 | - | [part-5-impacts-2026-11-12.md](../history/shred/part-5-impacts-2026-11-12.md) |
| [shred-7] | Optional: `ShredSphere = 7`, `DetachFractured = 8` with golden tests (`ShredSphere.*`, `ShredNoise.GoldenValues`); a bending / tip check on top of 4b; settle-to-heap. New op kinds: a protocol bump | Not started (only if play shows the need) | - | - |
| [si-1] | Objects break under their own weight ("SI-1") | Designed | design `4855f85` | [si-plan-review-2026-11-17.md](../history/shred/si-plan-review-2026-11-17.md) |
| [si-2] | Impacts, inertial contact ("SI-2") | Designed | design `4855f85` | [si-plan-review-2026-11-17.md](../history/shred/si-plan-review-2026-11-17.md) |
| [si-3] | Re-checks away from casts, for parts a check's cap left ("SI-3") | Designed | design `4855f85` | [si-plan-review-2026-11-17.md](../history/shred/si-plan-review-2026-11-17.md) |

The 3 ms rule (stage 0): if the island search plus the face flood of a castle-case cast is over about 3 ms (optimised),
the cell graph moves before networking stage 3. It was (3.3-5.6 ms), and it did ([shred-4]).

## Structural integrity plan (2026-11-17): stages 5, 5b, 6 against the code after networking 7b

Designed, nothing built ([si-1] .. [si-3] in "Stages"). Amended the same day after a critic's review (15 findings, all
checked against the code and accepted, 5 of them with a changed or extended fix:
[history/shred/si-plan-review-2026-11-17.md](../history/shred/si-plan-review-2026-11-17.md)). The owner asked
(2026-11-17): "Implement a Structural Integrity System to check if object connections can break under their own weight
and under inertial contact." This section re-plans stages 5, 5b and 6 against the code as it is after networking stages
6-7b (sessions with server bodies, NetIds, the entity byte budget, chunk sync and checks) and replaces rows 5, 5b and 6
of "Stages" and the unbuilt parts of Part 4 ("Host and scope": the registry and overhang items; "Propagation over
ticks") and of Part 5 wherever it says so. Row 7 (bending, `ShredSphere`, `DetachFractured`) stays optional;
collapse.md stage 1 (static objects) stays at ROADMAP M3. The fog half of the same message is not part of this plan
([fog.md](fog.md)). It cites symbols, not line numbers. Every time or size marked **(e)** is an estimate. Binding as for
every stage: Part 2 (R1-R7), fun and few bugs over realism, the UHD 620 frame, tests first, GPU-free logic.

### Decision in brief

1. **Three milestones**, each ending in green tests, a headless demo, a review and a commit: **SI-1** objects break
   under their own weight, **SI-2** impacts, **SI-3** terrain re-checks for parts a check's cap left. Order SI-1 ->
   SI-2 -> SI-3 (SI-2 dents resting objects and so calls SI-1's check). SI-3 depends on neither and may go before SI-2;
   whichever of the two comes first introduces the structure credit (SI-2, "Data"). They start once networking stage 8
   is committed (one build directory).
2. **One structural rule everywhere: Part 4b's max-flow load check.** Dynamic objects get it on their own graph (tile
   graphs of the model volume, the whole object judged, no island box); Static objects are not judged (today's rule;
   nothing in the game or a session is static: collapse.md stage 1 stays at M3). No second rule:
   **`StructureRegistry`, the overhang rule, the spawn guard, `StructuralSolver` and `CollapseBudget` are dropped**
   (reasons below).
3. **No wire change, no new event kind.** Everything a client sees is what it sees today: `Carve` / `Detach` /
   `ClearMask` (terrain), `ObjectModel`, `EntitySpawn` (with a NetId or `netId` 0), `EntityDespawn`, `EntityRest`,
   `EntityWake`. A crater travels as the `ClearMask` of the voxels the integer carve rule removes and a re-check emits
   no carve at all, so a client presents both as a `deferred` group (dust, no explosion, no flash: `Client::Impl::
   Present`). `kProtocolVersion` is not touched by SI-1..3.
4. **Impacts cross one integer boundary, `ImpactQ`** (R3): a Jolt contact is drained in an order fixed by its content,
   quantised at once, oriented from the striker into the receiver; from there on everything is integer and totally
   ordered. **Impacts and re-checks have a structure credit of their own** (work and bytes), never read or written by
   `RunCasts`: a physics event never delays a player's cast, and R5 stays as written.
5. **A release never needs a re-check** (Part 4b's kept side routes all its mass to the anchors without the released
   set, so removing that set cannot overload anything: SI-3 pins it in a test). What can need one is only what a check's
   part cap left (SI-3, terrain) and an object that was moving or over the per-tick bound when it was hit (SI-1's
   pending checks). A spent work budget is not retried: it waits for a later cast, as today (R7).

### What still holds

| Plan item (Parts 2, 4, 4b, 5) | Still holds because |
|---|---|
| R1 server decides, clients apply; R2 integer terrain operations; R4 fixed orders | Unchanged by networking 6-7b. Objects were always different: `Scene::Damage` decides with a float sphere and the server sends the result as a whole model (`ObjectModel`) or a spawn, so an object's decision may read floats (pose, motion) - it is the server's, replicated by value |
| R5 counted work | Built (networking stage 3): `CastWorkOf`, `kCastWorkPerTick` 60 000, the event byte credit. **Not amended**: impacts and re-checks are charged to their own structure credit, object checks to a per-tick count |
| R7 over budget = later or cosmetic | Built for casts and pieces (`TakePieceSlot` -> client rubble, the entity byte budget -> `ToRubble`). Impacts keep the one exception (derived, may be dropped, counted) and never cause a `ToRubble` (they are skipped instead) |
| Part 4b's rule, strengths, budgets (`kNeckFlowWorkBudget` 65 536, 64 parts / 60 000 voxels per check) | Built, reviewed, in `buildHash`. SI-1 reuses its network solver (`voxel::SolveNeckNetwork`) and its node build; terrain results must not change by one voxel or one work unit. No budget is scaled (the strict rule needs fewer than 2^17 directed arcs: 65 535 pairs) |
| "Never after `FillBox`, editor edits, generation" (no buildings falling on spawn) | Kept and extended to objects: never on spawn, never on rest by itself |
| Rubble lifetimes and caps (stage 1e), `MinPieceVoxels`, `kMaxIslandVoxels` | Unchanged; every new piece of SI-1/2 goes through `CreateVoxelObject(.debris)` or `TakePieceSlot` |
| Part 5's integer energy, total order, cooldowns, server-only impacts, no crush from contacts | Kept, simplified (below) |
| collapse.md's fracture bricks | Kept in a new, simpler place: SI-2's shatter cuts a body into 2-3 blocks |

### What changed since Parts 4 and 5 were written

| Then | Now (code after networking 7b) | Consequence for this plan |
|---|---|---|
| "Server bodies need networking stage 6"; every release a `netId` 0 `Detach` | A `Detach` of at least `replicatedPieceVoxels` (0.512 m^3: 262 voxels at 0.125 m) that gets `TakePieceSlot` carries a NetId; the server makes its body after the push (`sim::Blast` step 5) | A neck-released tower is a server body in a session and can strike the ground (SI-2). No new path for terrain releases |
| `CollapseBudget{releasesPerTick 6, serverBodiesPerTick 2, ...}` | `TakePieceSlot`: 2 bodies per tick, 64 awake replicated bodies (crates included), 256 live pieces, none while the entity byte budget is spent; largest pieces of a cast ask first | `CollapseBudget` is dropped: every new piece of SI-1/2/3 asks `TakePieceSlot`, over budget it is client rubble (a fragment / `netId` 0) |
| Entity events counted against the event budget | Entity events have their own credit (200 B/tick = 12 kB/s, burst 16 KiB) that never moves a cast; `ObjectModel` resends a whole model (`WriteVolumeRuns`); `EmitObjectEvents` lets a damaged object go `ToRubble` (6 B) once the credit is at or below 0 | Client rubble is `Layer::Rubble`, which session characters never meet (`kSessionCharacterLayers` = Static \| Dynamic): a `ToRubble` makes a landed body a walk-through ghost. **SI-2 never causes one**: an impact `Damage` / shatter needs `entityCredit` >= the model's encoded size (its runs counted, no allocation), else it is skipped and counted. SI-1's survivor is an ordinary `ObjectModel` and inherits the `ToRubble` degradation, as any cast's damage does |
| Client copies placeholder; "dust until the body exists" | Copies are kinematic; moving replicated bodies are `Layer::Rubble` on both sides (they pass through players); `characterMovesBodies` false; the client builds every copy when its event applies | Crush stays out (a moving body never touches a player; there is no health yet: M2). Contacts on a client are drained and dropped |
| Join by event log | Join by chunk sync (7a); every server terrain op must go through `Server::Impl::EmitOp` (modified map) | Impact craters and re-checks are emitted through `EmitOp` like casts; joiners get them as modified chunks |
| No checks | `HashCheck` / `EntityCheck` / `EntityList` (7b) | Nothing new to check: craters are `ClearMask` ops, shatter pieces are entities. Both are in the existing comparisons |
| `TakeContactEvents` nobody calls; queue fills | Still true (notes/physics.md); `ContactCollector::Publish` keeps one event per body pair after an unstable `std::sort` of events filled in Jolt's thread arrival order (`PhysicsWorld.cpp`) | SI-2 drains every tick on every side (also a fix on its own) and makes `Publish`'s order a function of the events' content |
| A client presents a received `CarveSphere` as a cast (non-deferred group: `fx::SpawnExplosion` and a flash, `sim::PresentBlast`); a `Detach` / `ClearMask` with no open group as `deferred` (no burst, no flash) | Built (Client.cpp `Present`) | Craters are a `ClearMask`, re-checks emit no carve: both are shown as dust. Offline, impact and re-check results set `deferred` too, so one rule shows them everywhere (`BlastResult::cause` is for logs and stats only) |
| Part 5: `ColliderMaterials`, fatigue, a 30-tick impact queue, `OpBatch` / `PresentOps` | Not built | Simplified away (next table) |

### Dropped or simplified, with the reason

| Item | Decision | Reason |
|---|---|---|
| `StructureRegistry`, the overhang rule, the registry's spawn guard | **Dropped** | Part 4b's check gives the "thin necks fall" the owner asked for with one rule and no registry state that a server, its replays, joins and saves must keep in sync. Its gap (no bending: a floor cantilevered from the remaining walls holds by shear) is row 7's bending / lever term on the same check, built only if play shows hanging floors |
| `StructuralSolver` with dirty boxes after every op, round-robin solves | **Dropped**; SI-3 keeps a re-check queue fed by part-cap leftovers only | Every carve already runs the check at its site (casts and, after SI-2, craters); releases provably cannot overload anything; so the only work left over is what a part cap cut short |
| `CollapseBudget`, release queue over ticks, "queued parts stay solid" | **Dropped** | `TakePieceSlot` + 4b's per-check caps + SI-3's queue already spread a big collapse over ticks (64 parts per check) |
| `OpBatch{cause}` / `PresentOps` | **Simplified**: `BlastResult::cause` (`Cast`, `Late`, `Impact`, `Recheck`) for logs and stats; presentation by the existing `deferred` flag | The server's emit path (`RunCast`: `EmitOp`, `EmitObjectEvents`) and the offline `PresentBlast` already take a `BlastResult`; impacts and re-checks are `sim::Blast` calls with options. `cause` never reaches a client (no wire change), so it must not decide what a client shows |
| Part 5's `ColliderMaterials` (compound child -> material) | **Dropped** | The struck terrain material is read from the world at the quantised point (integer, the same for every side); the striker's material does not enter the rule |
| Part 5's fatigue (sub-threshold hits add up) | **Dropped** | Hidden state per body, hard to read in play; immunity ticks, the speed floors and "a shattered piece never shatters again" are enough against spam |
| Part 5's 30-tick impact queue | **Reduced to 10 ticks** | An impact shown half a second late reads as a bug; older ones are dropped and counted (R7's exception) |
| Part 5's impact record and replay (`ImpactRecord`, `impactSource = Recorded`), and the R5 amendment it carried | **Dropped** (review) | With a content-ordered drain and a credit of their own, impacts never move a cast by credit; Jolt is documented deterministic for the same inputs and call order, which `Impact.SameImpactsWithOneOrThreeThreads` checks. A record would still have read Jolt-made state (the striker) and could not be tested apart from Jolt on one machine |
| Striker dents (Part 5 and the first draft of SI-2) | **Dropped** (review) | Each resent the striker's whole model (a tower about 6.5 KiB **(e)**) and, once the entity credit was spent, turned a landed body into a walk-through ghost; a stone block chipped by landing on grass also reads wrong. A striker either shatters or stays whole |
| Crush damage to players | **Out of SI** (M2) | No health system exists; moving bodies are `Layer::Rubble` and never meet a character, so there is nothing to detect in physics either. The server sweep of Part 5 stays the M2 design |
| collapse.md stage 1 (static objects: `BaseRow` anchors, the anchored remainder stays Static) | **Not in SI** (review) | Nothing in a session or the game is static: `sim::TemplateObject` has no motion field, the server places objects Dynamic (`SpawnObjectAt`), the game spawns only dynamic objects; only the offline editor's `spawnMotion` can. It stays at M3, when static content exists |
| collapse.md stage 4 for objects (`RebakeRegion`, per-chunk collider boxes) | **Not needed for SI** | Server models are not baked; offline objects are small. Stays a later optimisation |
| Offline NetIds "as a server does" (Part 5) | **Dropped** | Eligibility to cause an impact is by size (the replicated volume, the same metre size offline and on a server), never by a NetId |
| Re-checks with 4x budgets (first draft of SI-3) | **Dropped** (review) | A full 4x re-check charges up to about 650 000 work units **(e)** (11 ticks of cast credit), scaling the arc cap breaks the strict rule's 2^17 bound, and no measured cast has spent a budget (largest need 13 000 of 65 536; 0 of 80 stage-4 searches). A spent budget stays "released by a later cast" (R7 as today), logged and counted |

### SI-1 - objects break under their own weight

**Owner-facing behaviour.** A stone-topped table whose leg is shot snaps its other legs and drops the slab; a statue
whose neck is grazed loses its head, which drops onto the pedestal. A burst of quick shots is judged hit by hit (a
neck chipped by three shots falls at the shot that overloads it). Nothing falls when it is placed, dropped or comes to
rest - only after it is damaged (as terrain: never on spawn). A damaged object that is moving is judged once it is
still. In a session both clients see the same: the survivor as an `ObjectModel`, the released parts as server bodies (2
per tick, largest first) or as client rubble.

**Data.**
- `voxel::VolumeLoadCheck` (Core, `voxel/Structure.cppm`, pure function, GPU-free):
  `VolumeLoadInput{const VoxelVolume&, const GraphMaterials&, const NeckLimits&, uint8_t downAxis (one of 6),
  uint32_t maxTiles, uint32_t maxMemberVoxels}` -> `VolumeLoadResult{std::vector<VolumePiece> released; uint32_t
  crumbled; std::vector<NeckCut> cuts; counters: tiles, nodes, arcs, flowWork, skipped, flowBudget}`. Anchors are
  always the bottom layer (step 4); there is no anchor-rule parameter.
- `GraphMaterials` per palette: `MakeGraphMaterials(materials, palette, ignoreFlags)` (today world palette only); the
  scene caches one per palette with its `NeckLimits` for the object's voxel size (`transform.scale`, as `Damage` reads
  it). A `.vox` palette without strengths gets stone's (the `Material` default).
- `SceneConfig::objectLoadCheck` (default **false**: every existing test keeps its numbers; the game's offline config
  and `sim::ServerConfig` set it), `kObjectLoadMaxTiles` = 8 (2^3 tiles: 64^3 voxels, 8 m at 0.125 m; a 22 x 56 x 22
  tower piece is 2 tiles), `kObjectLoadChecksPerTick` = 2, `kObjectLoadTilesPerTick` = 16, `kLoadMemberVoxels` = 64,
  `kRestSpeed` 0.1 m/s, `kRestSpin` 0.1 rad/s (all (e)).
- `scene::LoadCheckPending{uint32 sinceTick}` on an object that was hit while moving or over the per-tick bound;
  `sim::ObjectLoadBudget{checks, tiles}` per tick (server) or per frame (offline), passed to `Damage` by `sim::Blast`.
- `DamageResult` gains `loadChecked`, `loadPending` (moving, or over the per-tick bound), `loadSkipped` (tilted, too
  large, Static), `loadReleased` (voxels), `loadCrumbled`, `cuts`; `BlastResult` sums them (`objectLoad`), the `Blast:`
  line logs them.

**Algorithm** (inside `Scene::Damage`, after the carve and before `SplitComponents`; and `Scene::CheckLoad` for a
pending object, the same steps without a carve):
1. **When:** `objectLoadCheck`, voxels were removed, the body is **Dynamic** (Static: skipped, today's rule - cut apart
   it becomes Dynamic) and **still before this hit**: not frozen-wanting-to-move, |v| < `kRestSpeed` and |w| <
   `kRestSpin`, read from the old body before `Damage` removes it (not Jolt's sleep flag: every `Damage` re-creates the
   body with `EActivation::Activate`, Jolt sleeps a body only after 0.5 s still, a blast wakes everything within two
   radii, and a player casts every 15 ticks, so after the first hit of a burst every hit would land on an "awake"
   object). Moving, or the tick's `ObjectLoadBudget` spent (counted in checks and in tiles from the object's own
   oriented size, never from a time or a cache): `LoadCheckPending` (checked later, step 8). Tilted or too large:
   skipped (lenient, never pending).
2. **Down axis:** world down in model space (`inverse(rotation) * (0, -1, 0)`); the model axis with the largest
   component if that is at least cos 30 deg, else skip (`loadSkipped`, lenient: a tilted object holds). The volume is
   copied with its axes permuted so that down is -y (a cheap copy; the graph code keeps its fixed -y = compression).
3. **Graph:** the oriented volume is cut into 32^3 tiles from its corner (air beyond its size); each tile is filled into
   a scratch `voxel::Chunk` (thread_local, `RebuildOccupancy`) and built with `BuildChunkGraph` - the same build terrain
   uses, so nodes, splits at the narrowest plane, weakest materials and the anchor bit are the terrain's. More than
   `kObjectLoadMaxTiles` tiles: skipped (counted; the object breaks only by connectivity, as today).
4. **Anchors:** every node holding a voxel of the oriented volume's lowest solid layer (what it stands on) and every
   anchor-material node, with an infinite arc to the sink, as the window's bottom-row nodes have in terrain.
5. **Network:** every node of every tile is judged (no island box, no exterior floods: an object is small), split
   halves as in 4b, arcs by bond class and the weaker material, links across tile faces by `BondVoxels`. Built by the
   same function as terrain: the R-R arc builder of 4b's `NeckCheck` is extracted (`BuildNodeNetwork` over a graph view:
   the terrain `GraphTable` or the tile list), **keeping the terrain's arc order bit for bit** (every `Neck.*` and
   `Blast.Neck*` test unchanged, `--neck-blast` flow work 7 055 unchanged). Solved by `SolveNeckNetwork` with the terrain
   budgets; spent or over the caps: nothing released, counted.
6. **Released set** A as in 4b (the largest strictly overloaded set, unique). **The kept-side support under each
   released part crumbles** (new, objects only). Why: a rigid simulation keeps a released part resting on whatever
   still stands under its cut faces (a neck stub, the legs that just failed), and no push reaches it (`Damage` gives
   pieces the parent's velocity, 0 at rest; a blast's push reaches 3 x its radius from the body's corner). Rule, per
   released part: start at the kept voxels directly below its cut faces; walk down column by column, layer by layer,
   while the layer's in-plane 4-connected kept component holding the column voxel has at most as many voxels as the
   part's cut faces; stop at the first wider layer or after `kLoadMemberVoxels` voxels for the part (top first). Those
   voxels are cleared as crumbs. A table's three remaining legs (1 voxel wide down to the floor) go; the statue's neck
   stub (2 columns of 4 voxels above the 8 x 8 pedestal) goes and the head drops 0.5 m onto the pedestal. Terrain keeps
   4b exactly (its released tower takes its neck down to the lowest cut and tips on its own).
7. **Pieces:** released voxels move into their own volumes; `SplitComponents` runs on the remainder and on each released
   volume; all pieces sorted largest first as today; the largest keeps the entity (unchanged). Below
   `replicatedPieceVoxels` a piece is a fragment, as today.
8. **Pending checks** (`sim::CheckPendingObjects`, server after the casts and impacts of a tick, offline once per frame
   after `ResolveImpacts`): objects with `LoadCheckPending` that are now still by step 1's rule, in key order (NetId on a
   server, entity value offline), while the tick's `ObjectLoadBudget` holds: `Scene::CheckLoad` (steps 2-7, no carve),
   emitted like `Damage`. Pending for more than 600 ticks (10 s, a rubble lifetime): dropped, counted. Any later
   `Damage` of the object runs the check itself and clears the mark.

**The owner's cases** (0.125 m; stone C 800 000 MU per face, wood C 640 000; MU = kg / vs^3; demo models
`sim::StatueModel` / `sim::TableModel` next to `sim::CrateModel`, both above the replicated size so they are shared
bodies in a session - a 184-voxel plank table would break into client rubble at its first hit, like a crate):

| Case | Load, MU | Capacity, MU | Result |
|---|---|---|---|
| Table: stone slab 24 x 3 x 12 (864 voxels) on four 1 x 8 x 1 wood legs | 2 246 400 | 4 x 640 000 = 2 560 000 | stands, 0.88 |
| Same, one leg's foot shot (0.25 m): its upper part hangs from the slab | 2 246 400 + 3 500 | 3 x 640 000 = 1 920 000 | **falls, 1.17x**: slab + hanging leg released, the three legs crumble (8 voxels each, 24 for the part), the slab drops 1 m |
| Statue: stone pedestal 8 x 4 x 8, neck 2 x 6 x 2, head 10 x 10 x 10 (1000 voxels), neck across the cell face y = 8 | 2 620 800 | 4 x 800 000 = 3 200 000 | stands, 0.82 |
| Same, the neck grazed to 2 faces | 2 620 800 | 1 600 000 | **falls, 1.64x**: the head is released; the stub under its 2 cut faces (part of the pedestal's 272-voxel node, 2 x 4 voxels) crumbles; the head drops 0.5 m onto the pedestal |
| A dumbbell (two 512-voxel cubes on a 2 x 2 neck) lying on the ground | both cubes anchored by their bottom layer | - | nothing released; the stage-6 dumbbell tests must keep their outcome with the check on |

**Determinism and replication.** Server or offline only (R1); clients never run `Damage`. The decision reads the pose
and the motion before the hit, like `TakePieceSlot` reads Jolt: a server decision, replicated by value. Survivor ->
`ObjectModel` (or `ToRubble` while the entity credit is spent, as for any cast's damage), released and split pieces ->
`TakePieceSlot` (largest first) -> `EntitySpawn` with a NetId or a fragment (`netId` 0) - the existing `sim::Blast` step
3 path, unchanged. Crumbled voxels become particles on a client through the existing "voxels an `ObjectModel` removed"
rule (at most 64 bursts). Objects are not in `WorldHash`; `EntityCheck` / `EntityList` compare ids and model hashes as
today.

**Budgets per tick.** At most `kObjectLoadChecksPerTick` (2) object checks and `kObjectLoadTilesPerTick` (16) tiles per
tick (offline: per frame), shared by checks inside `Damage` and pending checks; per check the terrain network caps (8192
nodes, 65 535 arc pairs, 65 536 flow work, 64 parts / 60 000 voxels) and 8 tiles. The object half is not charged to the
cast work credit (it reads Jolt, R5); the counts bound it, and an object over them waits as `LoadCheckPending`.

**Cost (e).** Orient copy: the model's voxels once (a 22 x 56 x 22 tower piece, 27 k voxels: about 0.03 ms). Tiles:
25-30 us each (the measured Dense chunk build); the statue and table are one tile, a 64^3 object 8. Network: tens of
nodes for the demo models, 150-300 for a tower piece; the preflow is microseconds there. **Per check 0.05-0.3 ms; at the
caps about 1.8 ms** (8 tiles 0.25 ms + 65 536 flow work about 1.5 ms, from 4b's measured 5-6 ms per 207 k scans); **per
tick at most about 3.6 ms** (2 checks at the caps; typical under 0.6 ms). Session clients: nothing new. Memory: one
scratch chunk (37 KB) and the tile graphs (5-7 KB each), freed after the call.

**Tests (GPU-free, written first, seen failing against a stub that releases nothing; every negative test has a positive
control in the same setup).** VortexCoreTests (`tests/core/VolumeLoadTests.cpp`):
- `VolumeLoad.EqualsTheTerrainCheckOnTheSameVoxels`: the statue, the table (both intact and with one leg shot) and 40
  random hourglass / slab volumes placed as terrain with their y = 0 row on the bottom row of a full-height window
  (judged nodes holding a bottom-row voxel are sinks, as `BottomLayer` makes the lowest layer anchors), the whole volume
  inside a dud's island box, and as volumes: the same released voxels, loads and cuts (before the object-only crumble).
- `VolumeLoad.BedrockContactIsAKnownDifference`: the one-leg table placed as terrain on bedrock releases the legs with
  the slab (the feet's finite wood-bedrock arcs are the lower cut of the same strength, 4b's "lower cut wins"); the
  volume releases the slab and the hanging leg only. Pinned so nobody "fixes" one side to match the other.
- `VolumeLoad.TableOnThreeLegsFallsOnFourStands`, `VolumeLoad.StatueNeckThresholdIsExact` (4 faces stand, 2 fall; a head
  of exactly the capacity holds, one MU more falls).
- `VolumeLoad.DownAxisRotatesTheVolume` (the statue in 6 orientations: the same release; tilted 40 deg: skipped; the same
  statue tilted 20 deg: released).
- `VolumeLoad.LyingDumbbellReleasesNothing` (positive control: the same dumbbell standing on one cube releases the
  other), `VolumeLoad.SupportUnderTheCutCrumbles` (table legs 24 voxels, statue stub 8 voxels; a support wider than the
  cut stays; a 100-voxel column stops at 64), `VolumeLoad.NeckAcrossATileFace` (a 40^3 volume, the neck at y = 31 /
  32), `VolumeLoad.TileAndNetworkCapsSkip` (9 tiles: skipped; 8: judged), `VolumeLoad.ReleasedSetIndependentOfArcOrder`
  (the shuffle hook).
- Unchanged and green: every `Neck.*` test (the extracted network builder), with their work counters.
VortexSimTests (`tests/sim/ObjectLoadTests.cpp`):
- `ObjectLoad.ShotLegDropsTheSlab` (offline config: pieces where their voxels were, legs crumbled, the slab's lowest
  voxel at least 0.5 m lower within 60 ticks).
- `ObjectLoad.GrazedNeckDropsTheHead`: the check run at rest with **no blast push** (`Scene::CheckLoad` on a statue whose
  neck was cut by `Damage` with the check off); the head's lowest voxel moves down at least 0.25 m within 60 ticks.
- `ObjectLoad.BurstOfShotsIsJudgedHitByHit` (three casts 15 ticks apart chip a resting statue's neck from 4 to 2 faces:
  the head is released at the hit that overloads it, not left for a later shot).
- `ObjectLoad.MovingObjectIsPendingThenChecked` (hit while sliding at 2 m/s: pending, checked when still, released;
  control: the same hit at rest releases at once), `ObjectLoad.NeverOnSpawnOrRest` (an overloaded statue placed and
  dropped stands for 600 ticks; control: the same statue hit at rest releases), `ObjectLoad.ChecksPerTickBound` (5
  statues hit in one tick: 2 checked, 3 pending, all released within 3 ticks), `ObjectLoad.PendingIsDroppedAfterTenSeconds`,
  `ObjectLoad.StaticObjectIsNotChecked` (config on: a static object cut gives today's `Damage` result; control: the
  same model Dynamic at rest releases), `ObjectLoad.OffKeepsTodaysDamage` (config off: `Damage` results equal before
  and after).
- `Replication.ObjectLoadReleaseIsAModelAndPieces` (server config: one `ObjectModel`, pieces largest first through
  `TakePieceSlot`, over the tick budget fragments; over the entity credit `ToRubble`).
VortexNetTests: `Entities.StatueAndTableBreakTheSameOnBothClients` (5 % loss, 40 + 0-30 ms: ids, models, rest poses
agree; hashes equal).

**Headless.** Game `--object-blast` (implies the demo objects, 8 m east of the probe site so the default world is
unchanged): frame 20 places the table and the statue as dynamic objects; frame 30 a 0.6 m miss beside each removes
nothing (`ObjectLoad: pristine: 2 objects, 0 checked`); frame 40 a 0.25 m shot at one table foot, frame 50 at the
statue's neck. Must log `ObjectLoad: table released: 1 part of N voxels, load L > cut C (3 faces), support crumbled
(24 voxels)` and `ObjectLoad: statue released: head of 1000+ voxels, ... 1.64x, support crumbled (8 voxels)` (the exact
numbers come from the tests), the released part's lowest voxel lower at +20 frames than at release, `store a/b`; frame
60 throws a second statue up with a 1 m push and grazes its neck in flight (`ObjectLoad: statue checked at rest after N
frames: head released`); exit 0 under validation, also at `--voxel-size 0.1` / `0.15` (the models scale with
`PlaygroundScale`). Session: `vortex_server --object-drill N` (`Server::SpawnObject` places both models at a drill site,
a cast hits the table foot and the neck) must exit 0 with `hashes equal` and `Entities: ... agree`. The standard,
editor, `--neck-blast`, `--tower-blast` runs: unchanged numbers.

**Risks.** (1) Bottom-layer anchors are wrong for an object resting on something other than its lowest layer (lying on a
rock with a leg hanging): it may release a part that a real body would not lose - lenient cases are fine, false falls
are bounded by "only after damage, only when still" and logged with the ratio. (2) Imported palettes have no strengths
(stone's apply). (3) The crumble turns up to 64 voxels per released part into particles: a client sees dust, not a
snapped leg body. (4) The extracted network builder could change terrain work counters: pinned by the unchanged 4b
tests. (5) "Still" by `kRestSpeed` reads a float velocity: a server decision replicated by value, never a client's.
(6) Effort (e): 4-6 days.

### SI-2 - impacts (inertial contact)

**Owner-facing behaviour.** A heavy piece that lands hard on soft ground leaves a crater; a felled tower that hits the
ground breaks into 2-3 big blocks and cracks the turf it lands on (a wall it falls onto may then lose a part by the load
check); a resting statue a falling block hits is dented and may lose its head. Hard surfaces (the plaza paving, bare
stone) take no crater. Small rubble, crates and pieces that just broke off never smash anything; a piece that broke from
a shatter never shatters again. Bodies sliding or tumbling slowly never plough trenches. In the arena margin a landing
leaves no crater. Players are not hurt (M2). A landing shows dust, never an explosion or a flash.

**Data.**
- `physics::ContactEvent` gains `invMassA`, `invMassB` (Jolt's inverse mass in the listener, bodies locked; 0 for
  static, kinematic and frozen bodies).
- `ContactCollector::Publish` sorts by content: (bodyA, bodyB, approachSpeed desc, position x / y / z, normal x / y / z),
  the floats compared as bit patterns, so the event kept per body pair never depends on Jolt's thread arrival order (a
  body landing flat ties on `approachSpeed` across its terrain triangles).
- `sim::ImpactQ{int32 centreQ[3] (SphereQ units, 1/16 voxel), int8 normal[3] (x 127, **from the striker into the
  receiver**: Jolt's normal points from body A to body B, negated here when the striker is B; Jolt makes the body of the
  higher motion type A, so on terrain the striker is A, and between two dynamic bodies either may be), uint16 speedQ (v_n in cm/s), uint32 energyQ
  (J / 16), uint32 strikerKey, uint32 receiverKey (0 = terrain), uint8 terrainId (the receiver's voxel half a voxel
  along +normal from the contact point), uint32 tick}` and its total key `(energyQ desc, strikerKey, receiverKey,
  centreQ.x, .y, .z)`. Keys: the NetId on a server, the entity value offline.
- `sim::ImpactState{uint32 readyTick; bool shattered}` on every body, stamped by an `on_construct<scene::RigidBody>`
  observer in `Simulation` (readyTick = tick + 30, shattered false). An observer cannot know a parent, so `Damage` and
  `Shatter` set each piece's `shattered` explicitly after making it (the parent's flag; true for a shatter's blocks); its
  readyTick stays the fresh one (pieces are born touching).
- `SimulationConfig::impacts` (default **false**; the game's offline config and `vortex_server` turn it on,
  `--no-impacts` turns it off; a session client is always off), `Simulation::PendingImpacts()` (at most 64),
  `ImpactStats` (drained, eligible, deduplicated, resolved, craters, belowCrater, hardSurface, dents, shatters, dropped
  by age / budget / bounds / credit / slot).
- `BlastOptions{bool carveAsClear; bool noCarve; bool objects = true; bool push = true; bool deferred}`:
  `carveAsClear` applies the voxels the integer carve rule removes as one `WorldOp::Clear` (`ClearMask`: the same
  voxels and hashes as the `Carve`); `noCarve` emits no carve op (SI-3); `push = false` means every `Detach` carries
  velocity 0 and no body gets an impulse (bodies in the box are still woken); `deferred` marks the result for
  presentation.
- **Structure credit** (server, `ServerStats` logs it): `structureWorkPerTick` 15 000 work units and
  `structureBytesPerTick` 200 B (e), each `credit = clamp(credit + perTick, -perTick x 6, perTick)` as the cast credit,
  charged with `CastWorkOf` and the op bytes of every impact and re-check (SI-3). `RunCasts` never reads or writes it.
- Constants (all **(e)**, tuned by `--impact-drop` and play): dent `kImpactMinEnergy` 20 kJ and `kDentMinSpeed` 4 m/s;
  crater `kCraterEnergy` 50 kJ and `kCraterMinSpeed` 6 m/s (about a 2 m fall); crater radius `min(1.0 m, 0.35 m x
  cbrt(E / 50 kJ))` (integer cube root on `energyQ`); power `min(2.5, 1 + E / 500 kJ)` hardness (2.5 = brick: stone,
  3.0, never craters, so the plaza's stone foundation is never hollowed under its stone_dark paving, 4.0);
  `kShatterSpecificEnergy` 30 J/kg (a 3 m fall); `kImpactReadyTicks` 30, `kImpactMaxAgeTicks` 10. Every threshold is
  compared as an integer after quantising.

**Algorithm.**
1. **Drain every tick** (`Simulation::StepPhysics` and `Step`, every configuration): `TakeContactEvents`. With
   `impacts` off the events are dropped (this alone fixes the queue that fills, notes/physics.md).
2. **Eligible striker:** a body side that is Dynamic, holds at least the replicated volume (`kServerReplicatedPieceVolume`
   in the world's voxels: 262 at 0.125 m, offline too - so a crate, 152 voxels, never strikes, and offline rubble under
   that size never strikes), is past its `readyTick`, and on a server carries a NetId. Receiver: terrain (`userData` 0),
   a body with a NetId (server) or any body (offline). **Quantise at once** (R3): centre = contact point
   (`floor(p / vs x 16)`), normal oriented into the receiver, `speedQ`, `energyQ` from `E = 0.5 v_n^2 / (invA + invB)`
   (both 0: skipped), keys, `terrainId` (integer read of the world). Then the integer floors: `energyQ` under
   `kImpactMinEnergy` or `speedQ` under `kDentMinSpeed`: dropped.
3. **Dedupe** per unordered body pair (the striker is the eligible side with the larger key; one landing across two
   terrain chunk colliders is one impact against terrain), keeping the highest total key; **sort** by the total key,
   push into `PendingImpacts` (over 64: the lowest keys are dropped, counted).
4. **Resolve** (`sim::ResolveImpacts(simulation, limits)` -> `std::vector<BlastResult>` with `cause = Impact` and
   `deferred` set), per impact in key order, while the tick's limits hold (1 crater, 4 dents, 1 shatter) and (server) the
   structure credit is positive:
   - **terrain receiver:** a crater needs `energyQ` >= `kCraterEnergy`, `speedQ` >= `kCraterMinSpeed` and
     `hardnessQ[terrainId] <= powerQ` (else `belowCrater` / `hardSurface`, counted, no change). The sphere is centred
     at the contact point (never moved into the ground: a hard skin is never undercut), quantised, and refused unless
     `sim::CastFitsRegion(region, world, sphere)` holds (server; offline the window-rows test of a cast) - counted, no
     crater, never a `Fail`. Then `sim::Blast(sphere, power, {.carveAsClear = true, .objects = false, .push = false,
     .deferred = true})`: the `ClearMask`, island search and load check (a tower landing on a wall can release part of
     it); bodies in the box are woken, not damaged or pushed. The terrain half reads `ImpactQ` and the world only: the
     striker need not exist any more;
   - **body receiver:** a dent `Damage(receiver, contact, r)` with `r = min(0.5 m, 0.2 m x cbrt(E / 20 kJ))`, only if
     the receiver is still (SI-1 step 1's rule) and (server) `entityCredit` >= its model's encoded size; it runs SI-1's
     check through the tick's `ObjectLoadBudget` (a resting statue hit by a falling block can lose its head). A moving
     receiver: nothing. **The striker is never dented**;
   - **shatter** (the striker, after the terrain or dent step): it still exists and is eligible, `E / mass >=
     kShatterSpecificEnergy`, its longest axis spans at least 32 voxels, it never shattered, and (server) `entityCredit`
     >= its model's encoded size and `TakePieceSlot` grants at least one slot (asked before cutting; no slot: no
     shatter, counted). `Scene::Shatter(entity, blocks, seed)` cuts the body across its longest axis into `blocks =
     min(granted + 1, 3)` slabs of equal length (offline: 3), the planes shifted by `Hash3(strikerKey)` within one
     voxel; `SplitComponents` per slab; pieces through `Damage`'s piece path (the largest keeps the entity and its
     NetId; under the replicated size: fragments). SI-1's check runs on none of them (they move).
5. **Order in a tick.** Server: `Receive`, streaming, players, `RunCasts`, **`RunImpacts`**, `RunRechecks` (SI-3),
   `CheckPendingObjects` (SI-1), `StepPhysics` (drains into `PendingImpacts`), lifetimes, despawns, `TrackBodies`, flush,
   snapshots. Impacts and re-checks are charged to the structure credit only; an impact that waited
   `kImpactMaxAgeTicks` is dropped (counted). Offline: after `RetryIslandSearches` in `Engine::Run`, the same per-call
   limits, no credits; `PresentBlast` shows a `deferred` result as dust and debris, no explosion burst and no flash (its
   fragment loop moves above the `deferred` return, so a deferred result with fragments still makes them).

**The cases (e: the contact speed of a tumbling body is not its centre's; tuned in play).**

| Case | E, v_n | Outcome |
|---|---|---|
| 1 m^3 stone block (512 voxels, 2.6 t) dropped 5 m onto dirt (9.9 m/s), past its 30 ticks | 127 kJ, 9.9 m/s | crater r 0.48 m, power 1.25 (grass, dirt, sand go; wood and stone stay); 49 J/kg but 8 voxels long < 32: no shatter |
| The same from 1 m (4.4 m/s), past its 30 ticks | 26 kJ, 4.4 m/s | eligible, under the crater speed: nothing (`belowCrater`) |
| The 5 m block onto the plaza paving | 127 kJ | stone_dark 4.0 > power 1.25: nothing (`hardSurface`) |
| A crate (152 voxels) from any height | - | not a striker; struck at rest by a striker: dented (in a session that breaks it, `Broken`, as a blast does) |
| The neck-released watch tower (8558 voxels, 35 t) tipping, 5-10 m/s at the contact | 0.44-1.75 MJ | on the plaza paving (stone_dark 4.0, pattern lines stone 3.0): **no crater**; on the turf beside it: crater r 0.73-1.0 m, power 1.9-2.5; shatters at 7.75 m/s and up (30 J/kg) into 2-3 blocks (by the slots granted) |
| A 35 t block sliding down a 30 deg voxel slope at 5 m/s | v_n well under 6 m/s | no crater (the speed floor); dropped 3 m (7.7 m/s, 1.03 MJ) onto dirt: one crater |
| A shatter block landing again 0.4 s later | - | immune (30 ticks); after that it may crater or dent, never shatter |
| Rubble (any size under 262 voxels), offline or in a session | - | never an impact |

**Determinism and replication.** `ImpactQ` is the integer boundary; everything after it (crater sphere, power, the
`ClearMask`, island search, load check, operation order) is integer and fixed, and which contact survives a body pair is
fixed by content. Jolt decides which `ImpactQ`s exist; Jolt documents its simulation as deterministic for the same
inputs and call order, which `Impact.SameImpactsWithOneOrThreeThreads` checks for the thread counts we use. **R5 is not
amended**: impacts never touch the cast credit, and `CastBudget.ScheduleFollowsCountsNotTimeOrCache` runs with impacts
on (craters change voxels, so later casts' counted work follows them). Replication: the crater is a `ClearMask` (and
the search's `Detach` / `ClearMask`) through `EmitOp` (modified map, chunk sync, `HashCheck` as any op); a client shows
it as a `deferred` group (no burst, no flash). An impact's ops emitted right behind a cast's ops in the same tick, with
no entity event between them, join that cast's open group on a client and are shown with the cast's dust (accepted:
the cast's burst is there anyway). Dents and shatters are `ObjectModel`, `EntitySpawn` (NetId or `netId` 0) and
despawns through `EmitObjectEvents`, gated so they never cause a `ToRubble` (`ServerStats::impactsSkippedByCredit`; the
`ToRubble` count caused by impacts is logged separately and must be 0). A client applies; its own simulation has
`impacts` off and drains to nothing. No wire change.

**Budgets per tick.** 64 pending, 1 crater (with its island search and load check: the cast budgets apply inside it),
4 dents, 1 shatter (at most 3 blocks: the survivor keeps its NetId, at most 2 new server bodies by `TakePieceSlot`),
inside the structure credit (15 000 work units, 200 B per tick, 6 ticks of debt at most; e); dents and shatters also
need the entity credit as above; offline the same counts per frame.

**Cost (e).** Drain and quantise: about 1-5 us per event, tens per tick (the content sort adds nothing measurable).
A crater: one blast's search and check, measured 0.3-1.9 ms at a cached site, up to 3.7 ms cold (Part 4 / 4b),
charged to the structure credit. A dent: one `Damage` (0.1-0.5 ms). A shatter: `SplitComponents` over the body (about
25 ns per voxel: 0.2 ms for the tower) plus up to 2 new bodies (server 0.1-0.3 ms each, no bake). **Worst server tick
from impacts about 3-5 ms**, at most once per 6 ticks by the structure credit's debt. Offline frame: a shatter's 3
bodies at 0.3-0.5 ms each = 0.9-1.5 ms on the landing frame plus the crater's search (over the 3 ms rule on that frame
only with a cold crater, like a cold cast). Session client on the UHD 620: one landing = a few ops plus up to 3 copies
to build (est. 0.5-2 ms). Wire per tower landing (e): a crater `ClearMask` 0.3-0.6 KiB (runs of a half sphere up to
17^3 voxels), the survivor's `ObjectModel` plus two `EntitySpawn` with models together about the tower's model, about
6.5 KiB (the review counted 3336 runs for the 8526 voxels above the neck; the stage-6 test cubes were 3.4 KB), inside
the 16 KiB entity burst and refused while the entity credit is below it.

**Tests (written first; every negative test has a positive control in the same setup).** VortexPhysicsTests:
`PhysicsWorld.ContactEventsCarryInverseMasses` (static side 0, a frozen body 0), `PhysicsWorld.PublishOrderIsByContent`
(equal approach speeds on several triangles: the kept event is the one with the lowest position bits, whatever the
arrival order). VortexSimTests (`tests/sim/ImpactTests.cpp`):
- `Impact.ContactQueueIsDrainedEveryTick` (10 000 ticks of bouncing bodies, impacts off and on: the queue never reaches
  its cap, no warning).
- `Impact.QuantisedOnceAndTotallyOrdered` (events shuffled, two terrain chunk colliders under one body, equal energies:
  the same `ImpactQ` list and the same operations) and `Impact.SameImpactsWithOneOrThreeThreads` (the same drops with
  `PhysicsConfig::threads` 1 and 3: equal `ImpactQ` lists and operations).
- `Impact.NormalPointsIntoTheReceiver` (a block landing on terrain; two dynamic blocks with the striker as A and as B:
  the normal points into the receiver, `terrainId` is the ground's material).
- `Impact.HeavyDropCarvesLightDoesNot` (the 512-voxel blocks spawned on `FillBox` ledges; the ledges removed and the box
  woken 30 ticks later; 5 m: crater voxels > 0; 1 m: counted eligible and `belowCrater`, 0 voxels).
- `Impact.SpeedFloorStopsTrenches` (the 35 t block pushed down a 30 deg voxel slope at 5 m/s: no crater; dropped 3 m:
  one).
- `Impact.HardSurfaceIsNeverCratered` (a heavy drop on the plaza's paving and on a stone pattern line: 0 voxels; the
  same onto turf: > 0; no voxel removed under an intact surface voxel).
- `Impact.SmallBodiesCratesAndRubbleNeverStrike` (offline and server configs: a 200-voxel rubble piece and a crate
  dropped 10 m cause nothing; control: a 262-voxel block from the same height craters).
- `Impact.NewBodiesAreImmune` (a piece made touching the ground with a blast push: no impact for 30 ticks; control: the
  same piece dropped again after tick 30 strikes).
- `Impact.StrikerIsNeverDented`, `Impact.DentsOnlyAStillReceiver` (a falling block on a resting crate dents it; on a
  sliding one: nothing).
- `Impact.OnePairOneImpact` (two eligible blocks colliding: one `ImpactQ`, the larger key strikes).
- `Impact.FelledTowerShattersOnce` (the 4b neck tower released by a cast lands on turf: crater, 2-3 blocks by the slots,
  every block `shattered`, a block later split by `Damage` keeps `shattered`, none shatters again).
- `Impact.BudgetPerTickAndAge` (20 blocks landing in one tick: 1 crater, 4 dents, the rest wait and are dropped after 10
  ticks, counted).
- `Impact.OutsidePlayBoundsCarvesNothing` (server: contacts 1-4 voxels inside the play-bounds edge with the normal
  pointing outwards, one in the margin and one at the window's bottom row: counted, no crater where `CastFitsRegion`
  fails, never a `Fail`; control: the same landing 2 chunks inside craters).
- `Impact.CraterIsTheCarveRuleAsAMask` (the `ClearMask`'s voxels equal `Carve(sphere, power)`'s on a copy of the world;
  equal `WorldHash`), `Impact.PushFalseMeansStill` (every `Detach` velocity 0, no body impulse, bodies in the box awake).
- `Impact.CraterRunsTheLoadCheck` (a block lands beside a 2 x 3 stone neck carrying a load: the neck's part is released).
- `Impact.TwoSimulationsFedTheOpsHaveEqualHashes`.
VortexNetTests: `Entities.FelledTowerBreaksTheSameOnBothClients` (5 % loss: hashes equal, ids, models and rest poses
agree), `Presentation.ImpactsAndRechecksAreDeferred` (a client's presentation of a crater and of a re-check has
`deferred` set: no explosion, no flash), `Entities.ImpactsNeverCauseToRubble` (entity credit spent: the dent and the
shatter are skipped and counted, 0 `ToRubble` by impacts), `CastBudget.ScheduleFollowsCountsNotTimeOrCache` with
impacts on.

**Headless.** Game `--impact-drop` (beside the plaza, not in the default world): frame 20 places the 512-voxel stone
block on `FillBox` ledges 5 m and 1 m above dirt and a third 5 m above the paving, frame 50 removes the ledges; frame 60
drops a 2 x 7 x 2 m stone column (16 x 56 x 16 voxels) from 4 m onto turf (8.9 m/s at its lower end). Must log
`Impact: block (512 voxels, 2.6 t) at 9.9 m/s, 127 kJ: crater r 0.48 m (power 1.25), N voxels`, `Impact: block ... 26
kJ, 4.4 m/s: below the crater`, `Impact: block ...: hard surface (stone_dark)`, `Impact: column ... shattered into K
pieces (2 <= K <= 3)`, an `Impacts:` total at exit (drained, resolved, craters, dents, shatters, dropped) and `store a/b`
under 7000; exit 0 under validation, also at 0.1 / 0.15 m. `--neck-blast` additionally logs the tower's landing
(`Impact: tower ...`). `vortex_server` (impacts on by default): the CLAUDE.md drill and the arena walk must still exit 0
with `hashes equal` and every `Entities: ... agree`, and log `Impacts: server: ...` with 0 `ToRubble` by impacts (the
drill's neck towers now land and break); `tools/session-check.sh` PASS, and it fails on a `ToRubble` by an impact.

**Risks.** (1) Contacts at creation (a piece born touching its crater) would read as impacts: the 30-tick immunity.
(2) Cascades (blocks shattering blocks): the `shattered` flag, the immunity and the speed floors. (3) A crater that does
not fit the region would break the server's loaded-flood-box guarantee: refused by `CastFitsRegion` on the final
sphere. (4) Energy of a tumbling body is overestimated with its whole mass: thresholds are estimates for the owner to
tune. (5) If Jolt differs between 1 and 3 threads, `CastBudget.ScheduleFollowsCountsNotTimeOrCache` must run with
impacts off (open question 5). (6) Offline landing frames with a cold crater go over the 3 ms rule. (7) Effort (e):
1-1.5 weeks.

### SI-3 - re-checks away from casts, only where play needs them

**What can still be left standing that should fall** (with SI-1 and SI-2 built): only a load check that hit its part
cap (64 parts / 60 000 voxels: `neckDeferredParts`) - the rest is still overloaded. **Not** on the list: a spent work
budget (`neckFlowBudget`, `neckExteriorBudget`, `attachedByBudget`: logged and counted, released by a later cast, R7 as
today); an object hit while it moved (SI-1's pending checks); after a release (proven: the kept side holds without
it); after a crater (it is a blast with its own check); after `FillBox` / editor / generation (never, by design);
castle-scale parts over 60 000 voxels (stage 4c, owner question 2, unchanged).

**Data.** `sim::RecheckEntry{SphereQ sphere; uint8 attempts}` in `Simulation::Rechecks()` (at most 16; a full list drops
the new entry, counted); `RecheckStats`.

**Algorithm.**
1. **Queue** after every cast, crater or re-check with `neckDeferredParts > 0`: the same sphere, normal budgets
   (progress is guaranteed: released parts are gone). At most 16 attempts per entry (1024 parts). A spent budget or
   `neckSkipped` (network over the node cap) is never queued.
2. **Run** one entry per tick: `sim::Blast(sphere, power 0, {.noCarve = true, .objects = false, .push = false,
   .deferred = true})` - island search and load check only: no carve op is applied or emitted (a power-0 carve changes
   no voxel, and a received `CarveSphere` would be shown as an explosion), no object damage, `Detach` velocity 0. Server:
   after the impacts, only while the structure credit is positive, charged to it; emitted through `EmitOp`. Offline:
   one per frame after `ResolveImpacts`.

**Determinism and replication.** Re-checks are integer and caused by integer counters; their output is ordinary
`Detach` / `ClearMask`, shown on a client as a `deferred` group. They never touch the cast credit, so the cast schedule
does not depend on them. No wire change.

**Budgets per tick.** 1 re-check at normal budgets, inside the structure credit. **Cost (e):** a re-check is a dud's
search and check, 0.1-1 ms cached (up to about 3.7 ms cold).

**Owner-facing behaviour.** A forest of overloaded pillars cut by one shot comes down 64 parts per tick instead of
waiting for the next shot. Nothing ever falls "by itself" without a shot or a landing before it.

**Tests (written first).** VortexSimTests (`tests/sim/RecheckTests.cpp`):
- `Recheck.DeferredPartsAreReleasedOverTicks` (4b's 529-pillar case: 64 parts per re-check, all released within 8
  re-checks after the cast, counters exact).
- `Recheck.SpentBudgetIsCountedNotRetried` (a network needing about 100 000 flow work: the cast releases nothing, no
  entry is queued, the counter says why; a later cast with the same counters releases nothing either).
- `Recheck.AReleaseNeverNeedsARecheck` (property: after every releasing cast of the `Neck.*` arenas, the tower and 50
  random hourglass forests, a dud at the same sphere releases nothing; at least 60 releasing casts are checked, and the
  casts with `neckDeferredParts` or a spent budget are excluded by name) - the proof behind dropping the solver.
- `Recheck.ScheduleFollowsCountsNotTimeOrCache` (server, 1 and 3 workers, cold and warm graph cache: the same ticks and
  operations), `Recheck.UsesTheStructureCreditNeverTheCastCredit` (a tick of casts at full cast credit: the casts run on
  the same ticks with and without a pending re-check), `Recheck.EmitsNoCarve` (the result's ops are `Detach` /
  `ClearMask` only).

**Headless.** Game `--pillar-forest` (beside the plaza): frame 30 `FillBox`es 200 thin stone pillars with heavy caps in
a 6 m square, frame 40 one dud among them; must log `Recheck: 200 parts released over N ticks (64 per check), 0 left`
and exit 0.

**Risks.** (1) A full queue drops new entries (counted): only in fights far beyond the measured cases. (2) A spent
budget leaves an overloaded structure standing until a later cast (as today); counted so play can show whether it
matters. (3) Effort (e): 1-2 days.

### Milestones at a glance

| Milestone | Built | Accepted when | Wire / contract | Effort (e) |
|---|---|---|---|---|
| SI-1 | `VolumeLoadCheck` on tile graphs (bottom-layer anchors), extracted network builder, `objectLoadCheck` in `Damage` for still Dynamic objects, support crumble, `LoadCheckPending` + `CheckPendingObjects`, per-tick check budget, demo models, `--object-blast`, `vortex_server --object-drill` | SI-1 tests green (each negative with its control), every 4b test unchanged; `--object-blast` logs both releases and the in-flight statue at 0.1 / 0.125 / 0.15 m; drill `hashes equal`, entities agree; standard runs unchanged | none (objects by value) | 4-6 days |
| SI-2 | contacts drained, content-ordered `Publish`, `invMass`, `ImpactQ` (oriented normal, `speedQ`), `ImpactState`, `ResolveImpacts`, `BlastOptions` (`carveAsClear`, `push`, `objects`, `deferred`), `Scene::Shatter` (2-3 slabs), the structure credit, server `RunImpacts`, `--impact-drop` | SI-2 tests green; `--impact-drop` and `--neck-blast` impact lines; drill, arena walk and `session-check.sh` pass with impacts on and 0 `ToRubble` by impacts; `CastBudget.ScheduleFollowsCountsNotTimeOrCache` green with impacts on | none; R5 unchanged | 1-1.5 weeks |
| SI-3 | `Rechecks()` for `neckDeferredParts` (normal budgets, `noCarve`), on the structure credit (introduced here if SI-3 goes before SI-2), `--pillar-forest` | SI-3 tests green; `--pillar-forest` releases all 200 parts | none | 1-2 days |

Each commit updates this section ("as built"), notes/physics.md (`TakeContactEvents` now has a caller, `Publish`'s
order), notes/scene-engine.md (`Damage` options, `CheckLoad`, `Shatter`), notes/net.md and networking.md (impacts and
re-checks after the cast phase, the structure credit, the `deferred` presentation of craters and re-checks), ARCHITECTURE.md
7.2, ROADMAP.md (state, test count) and the game's CLAUDE.md (the new flags). Not confirmable headless, for the owner:
the falls and landings in a window, how the thresholds feel, the UHD 620 frame during a tower landing.

### Open questions for the owner (SI plan; each with a recommendation)

1. **Should damaged objects be judged at all (SI-1)?** Recommendation: yes, when still, with the support crumble, only
   after damage; tilted objects (over 30 deg) hold.
2. **Impact thresholds** (dent 20 kJ and 4 m/s, crater 50 kJ and 6 m/s, power at most 2.5, shatter 30 J/kg into 2-3
   blocks). Recommendation: start there and tune from `--impact-drop` and play; the tower should break when it falls
   flat, a block should not crater when dropped 1 m, nothing should plough trenches.
3. **Crush damage**: stays M2 (needs health). Recommendation: yes, keep it out of SI.
4. **Floors that hang after their walls go** (no bending term): Recommendation: wait for play; if they read wrong, add
   row 7's lever term to the same check instead of reviving the overhang rule.
5. **If Jolt gives different contacts with 1 and 3 threads** (`Impact.SameImpactsWithOneOrThreeThreads` fails):
   Recommendation: pin the server's physics thread count in `ServerConfig` and run `CastBudget.ScheduleFollowsCounts
   NotTimeOrCache` with impacts off, recording why; do not bring back the impact record.
6. **Stone never craters** (power capped at 2.5). Recommendation: keep it - landings on rock and the plaza show dust and
   a shatter, never a hidden hollow; revisit only if play wants rock craters, and then with a struck-surface-only rule.

## Rejected alternatives

| Rejected | Why |
|---|---|
| Relax the face rule (a face component counts as an island) | Rips out hillsides and trees; notes/voxel.md forbids it; the server would send huge wrong masks. |
| Grow the search box until no component touches the face | Cubic cost: a 192^3 box is est. 7 MB of ids + 28 MB of labels per blast; refused today above 192 per axis. |
| Raise `kMaxIslandVoxels` | It bounds per-cast cost and body size; big natural pieces are split by the structure stages instead. |
| Re-run the island search on clients | Needs identical, fully loaded voxels in the whole box (networking.md, rejected alternatives). |
| A flood that meets an unloaded chunk counts as attached (draft 1) | Makes the decision depend on load state (streaming offline, player positions in stage 9), and offline it brings the hanging piece back while the world streams. Replaced by the fixed flood box: loaded by construction on a server, deferred offline. |
| Flood reach without a fixed box (draft 1: only an extent limit) | The reach must be a function of the blast's integers so the server can guarantee it is loaded. |
| Cast queue on a wall-clock budget | Tick placement would follow machine speed; replaced by a work budget (R5). |
| Hold back a `Detach` until the client has built its body | Every later op that touches the same chunks would wait too; dust covers the gap instead. |
| Distance-scaled piece threshold in `Scene::Damage` (lod.md stage 1 as written) | It would make a server decision depend on a viewer; moved to presentation. |
| Object host first (collapse.md order) | The game's buildings are terrain; castles exceed 12.8 m; object pieces need networking stage 6. |
| Load-flow stress as the default rule | collapse.md's judgement stands: tuning risk, "buildings fall on spawn"; kept optional (stage 7). Weak necks are stage 4b's max-flow check instead, run only after a cast (Part 4b, "Rejected within this part"). |
| Float impact energy deciding voxels | Jolt is not cross-platform deterministic; quantised once on the server instead (R3). |
| Impact filter by "bodies under the threshold do not exist on the server" | False offline (rubble shares the simulation); replaced by the ownership bit. |
| Crush damage from contact events | Characters are `CharacterVirtual` and never produce body contacts; a separate server sweep instead. |
| Every material same particles, fewer of them | Loses the "shred" read of a hit; profiles cost nothing extra on the wire and stay in fx caps. |

## Open questions for the owner (each with a recommendation)

1. **Stage 1 (with 1b-1e) before networking stage 3?** Recommendation: yes. About 5 days together, no format
   change, and the box rule is cheaper to change before the operation stream is encoded and tested over loopback.
   **Answered yes by the owner (2026-11-12, "Yes, build 1-1e"); built [shred-0] .. [shred-1e].**
2. **Server memory for the flood box**: a 3-chunk margin instead of 2 around `playBounds` (est. +10 % server
   memory). Recommendation: yes; the alternative (a flood box of only 2 chunks past the island box) frees fewer
   towers near the edge of the arena and in stage 9.
   **Answered: the owner took the recommendation (2026-11-12); built (`sim::kServerWindowMarginChunks` = 3).**
3. **Should big natural pieces and castle walls (over 60 000 voxels = 117 m^3 at 0.125 m) ever fall?**
   Recommendation: not in M1; counted (stage 1c). In M3 the structure stages split them into pieces under the cap.
   **Answered: the owner took the recommendation (2026-11-12); counted (`oversizeIslands`, `faceOversize`). Splitting
   is [shred-4c] (item 11).**
4. **If stage 0 shows search + flood over 3 ms per cast, move stage 4 (cell graph) before networking stage 3?**
   Recommendation: yes; a 5v5 at 10 casts/s cannot afford a 4-14 ms search on the server tick or the offline frame.
   **Answered yes (2026-11-12); stage 4 is built.** New for the owner: build chunk graphs ahead of the first blast
   (on a worker when a chunk near the player loads or changes) so the first blast at a site also stays under 0.5 ms?
   **Open.** It is the only fix found for the cold cost of the search and of the load check ([shred-4b]).
5. **Do floors drop when a wall goes (overhang limits on) or only when fully cut (connectivity only)?**
   Recommendation: overhang limits on, inside registered structures only; natural terrain connectivity only.
   **Superseded** by the SI plan (us, 2026-11-17): the overhang rule is dropped; floors that hang are its question 4.
6. **How long may a collapse take to unfold?** Recommendation: up to 2 s, released largest and lowest first;
   queued parts stay solid; at most 2 new server bodies per tick.
   **Superseded** by the SI plan: no release queue; `sim::TakePieceSlot` (2 per tick), 64 parts per check and [si-3]
   spread a big collapse.
7. **How long does rubble stay?** Recommendation: 15 s small, 20 s medium, 30 s large, caps of 48 large bodies
   and 600 store slots; no stamping into heaps until a game rule needs it.
   **Answered: the owner took the recommendation (2026-11-12); built [shred-1e].**
8. **Do falling pieces hurt players?** Recommendation: yes for awake server bodies over the replicated threshold
   (the crush the direction asks for, ROADMAP "Direction"), by a server sweep against player capsules in M2 game logic;
   never for client rubble.
   **Superseded** by the SI plan's question 3 (crush stays M2, no health yet). The queued order (project review,
   2026-11-17) puts crush with [si-2] once M2a's health exists: settled when SI-2 starts.
9. **Shatter vs dent thresholds and body caps**: Recommendation: start with the estimates above and set them from a
   measured `--collapse` fight scene on the UHD 620 (2 new server bodies per tick, 64 awake, 256 live).
   **Superseded** by the SI plan's question 2 (the SI-2 thresholds).
10. **May derived impacts be dropped over budget** (unlike casts, which never are, fx-budget.md)? Recommendation:
    yes, after 30 ticks, counted and logged.
    **Superseded** by the SI plan: dropped after 10 ticks (`kImpactMaxAgeTicks`), counted (R7's exception).
11. **Stage 4b (weak necks)**: its seven questions (where the tower falls, castle bricks as stage 4c, locality,
    weightless foliage, soft ground slumping, weak player-built structures, the flow budget) are in
    [history/shred/owner-questions.md](../history/shred/owner-questions.md). Questions 1, 3, 4, 5 and 6 were built as
    recommended (no owner answer recorded; question 6's editor "neck report" is not built); 7 was done in [net-3]. **Open: question 2, whole castles.** In 4b anything
    over 60 000 voxels is an anchor, so a castle never falls by the check (a corner tower, about 45 000 voxels, could
    once the walls beside it are cut away). Recommendation: [shred-4c] - raise the exterior load limit to 4 x 60 000,
    split any release over 60 000 voxels (and oversize islands) into chunk-aligned 32^3 bricks, each its own `Detach`,
    at most 240 000 released voxels per cast (the rest a later cast), wire bursts measured.
