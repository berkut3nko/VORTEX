# Shred history: stages 0-1e (the bug, the plan, as built, the review)

Moved verbatim from `docs/design/shred.md` lines 77-125, 204-217 and 234-457 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

Current text: `docs/design/shred.md` Part 1 and "Fix: prove face-touching components by an outward flood
(stage 1)"; stage rows [shred-0] .. [shred-1e]. Commits: engine `9cd4960`, game `597f555`.

---

<!-- source lines 77-125 -->

## Part 1 - The "second shot" bug (verified)

### Symptom

A shot cuts the base of a pillar, tower or wall; the part above visibly hangs in the air; one more shot nearby makes
it fall.

### Cause (confirmed in code)

1. A terrain hit calls `engine.Explode(pos, 1.2 m, 6)` (`src/main.cpp:369`) -> `sim::Blast`
   (`engine/src/engine/internal/Engine.cpp:113`).
2. The island box is `floor(c / 16) +- (r / 16 + kIslandSearchMargin)` voxels (`Blast.cpp:56-63`), margin 24 voxels
   (`sim/Blast.cppm:74`), deliberately not rescaled with the voxel size (view-distance.md:492-493).
3. `ExtractFloatingIslands` labels the box; a component with `mn == 0` or `mx == size` on any axis is **attached**
   (`Connectivity.cpp:225-227`). voxel.md:316-317 says not to relax this rule; it stops a blast ripping out hillsides.
4. The search is a "proof or nothing" step: `searched == true` and no island, so no retry is queued (retries exist
   only for `searched == false`, `Blast.cpp:146-152`). Nothing else ever searches: the only other caller is the
   probe in `src/main.cpp`.
5. So **anything that reaches farther than the box from the blast centre never falls from that blast**. A later
   blast falls only if its own box happens to contain the whole piece, which is the "one more shot".

Numbers (exact integer arithmetic, `QuantizeSphere` `World.cpp:1564-1575`):

| Voxel size | 1.2 m radius in q | `r / 16` | Half box | Box | A piece must lie within |
|---|---|---|---|---|---|
| 0.125 m (game) | 154 | 9 | 33 voxels = 4.1 m | [c-33, c+33) = 8.25 m | c-32 .. c+31 on every axis |
| 0.1 m | 192 | 12 | 36 voxels = 3.6 m | [c-36, c+36) = 7.2 m | c-35 .. c+35 |

Playground trigger: the watch tower (`src/main.cpp:694-704`) is 70 units of stone and planks, plus wood posts and
torches to 80 units = 8.0 m, all 6-connected (torch and wood are not `Liquid`, `voxel/internal/Material.cpp:136-155`).
A base shot at about 0.5 m has its box top at c + 33 voxels, about 4.6 m: the tower reaches the face, stays. A second
shot frees it only if its centre is at 4.0 m or higher and no more than 32 voxels above the crater top (e: 4.0-5.7 m
at 0.125 m). The L-shaped brick walls (`src/main.cpp:688-691`, 5 m and 4 m long) behave the same: a cut at one end
leaves them up, a cut in the middle third frees them.

### Other causes checked

| Candidate | Verdict | Where it goes |
|---|---|---|
| Box-face rule, no second search | **Confirmed**, matches the tower and walls exactly | Stage 1 |
| Blast carve skipped in chunks still `Loading`; the deferred search then sees uncarved generator voxels (`World.cpp:1443-1448`, `Blast.cpp:137-142, 181-203`; listed open in ROADMAP.md:254) | Plausible, far shots while streaming only (beyond the 16 m near radius) | Stage 1b |
| Island over `kMaxIslandVoxels` = 60 000 skipped silently (`Connectivity.cpp:227`, `Blast.cppm:72`) | Plausible on natural terrain and **certain for castles** (a 10 m tower section with 0.5 m walls is about 200 m^3 = 100k voxels, e); playground pieces are under it (tower about 12.7k voxels at 0.125 m, e) | Stage 1c (count it), stages 4-5 (split it) |
| Deferred search lost: 16-entry FIFO drops the oldest (`Blast.cpp:150`), retry given up after 600 frames silently (`Blast.cpp:196-200`, `Blast.cppm:76-78`), boxes outside the window never load | Plausible, rare | Stage 1c |
| A real 1-voxel bridge (6-connectivity, `Connectivity.cpp:92-97`) | Behaviour by contract; looks like the bug | Diagnostic test + face-candidate counter; no rule change |
| Stale terrain collider holds the piece | **Refuted** as the cause of a piece that stays up: the piece is created active (`PhysicsWorld.cpp:659`) and its own chunks are re-cooked and woken (`Terrain.cpp:392-416`). It is the most likely cause of the **lost blast push** (view-distance.md:444-451, "from the code, not isolated") | Stage 1d |
| Frozen body resumes asleep after its ground changed (`Terrain.cpp:409-415` wakes only on a rebuild, `PhysicsWorld.cpp:811`) | **Refuted** for islands; a real bug for crates and rubble | Stage 1d |
| `Detach` applied partly on a full store (`World.cpp:1470-1489`) | **Refuted** today: the game stays under 7000 of 12 288 slots. Stage 1 makes more and larger pieces, so stage 1e adds a slot cap | Stage 1e |
| Static object left unsupported | **Refuted**: the game creates only dynamic objects (`src/main.cpp:304, 961`) | - |

<!-- source lines 204-217 -->

### Server loading (networking.md changes in the stage-1 commit)

- **Stages 0-8** (networking.md:251-255): the server window becomes `playBounds` plus **3 chunks** on all six sides
  (was 2), with `playBounds` snapped to whole chunks (asserted). A cast centred inside `playBounds` then has its whole
  flood box (96 voxels past the boundary at most) loaded, because the window is `LoadBlocking` at start and never
  moves. Memory (e): for the 154 m region, 170 -> 178 m across, about +10 % dense chunks: 0.45-0.55 GiB -> about
  0.5-0.6 GiB.
- **Stage 9** (networking.md:262): `World::EnsureLoaded` (blocking) loads the **flood box** of every cast, not only
  its island box. Stage 4's graph search uses the same flood box (not +-16 m), so no further margin is needed.
- networking.md:343 ("A server window always has the box loaded (2-chunk margin)") becomes "the flood box
  (3-chunk margin)"; networking.md:337's island-box sentence becomes "island box + face flood inside the flood box".
- voxel.md:316-317 gets "the face rule stands; face components are then proven by a bounded flood inside the flood
  box".

<!-- source lines 234-457 -->

### Cost of stage 1 (estimates; measured in its acceptance)

| Case | Work | Time (e, optimised) |
|---|---|---|
| Surface hit on flat ground, no face candidate | none extra | 0 |
| Tower base cut, one candidate of ~12.7k voxels, 2 x 3 x 2 chunks | 12.7k visits through a cached chunk pointer, 0-12 Packed decodes (0.016-0.03 ms each, view-distance.md:315) | 0.2-0.6 ms |
| Forest hit, N trunks crossing the face | first trunk floods to 60k, the rest stop at the first attached voxel | 0.5-1.5 ms |
| Castle hit, several wall / floor candidates over 60k each | up to the budget, 240k visits, + up to 16 decodes | 2-6 ms (the worst case is the **usual** case in a castle fight) |
| Worst case per blast (budget 4 x 60k visits) | 240k visits, 1.7 MiB state, re-decodes counted | 2-6 ms |

The worst case is not rare: in a castle every blast near a wall pays it, and it comes **on top of** the island search
(est. 4-12 ms optimised, fx-budget.md:41). See "Budgets" for what that means for the server tick and the client
frame, and stage 0 for when stage 4 must move earlier.

### Stages 1b-1e (small, same review as stage 1)

- **1b, carve replay** (offline only): `DeferredIslandSearch` also keeps `power` and a "carve incomplete" flag set
  when the carve's `EditReport::skipped` is non-empty; `RetryIslandSearches` first re-applies `WorldOp::Carve` (it
  is idempotent on chunks already carved) and appends it to the late result before searching. A server never defers;
  clients already replay skipped ops (stage 7 of networking).
- **1c, nothing silent**: `BlastResult` gets `faceCandidates`, `facePieces`, `oversizeIslands`, the `FloodResult`
  counters, `deferredDropped`, `deferredGivenUp`; a log line per deferred drop or give-up. The 600-frame retry limit
  becomes "give up only once the flood box has left the window"; on FIFO overflow the new search replaces the oldest
  whose box is farthest from the focus. `kIslandSearchMargin`, `kFloodHalfExtent` and `kMaxIslandVoxels` stay
  constant (cost limits).
- **1d, physics after destruction** (physics only, no determinism impact: body poses are never hashed,
  networking.md:41):
  - *Lost blast push*: count a dirty, stale terrain collider as not ready for bodies overlapping the edited area, so
    a new piece is frozen until its chunks are re-cooked and keeps its velocity (view-distance.md:444-451,
    notes/scene-engine.md:419-425). Test (VortexPhysicsTests, TerrainGuard style): a body created with velocity
    over a chunk whose collider is stale keeps that velocity after the re-cook.
  - *Asleep in the air*: wake a parked body on the first build of a chunk modified since it was parked
    (`Terrain.cpp:409-415` wakes only on a rebuild; `Terrain.cpp:596-611` drops chunks). Test: a body parked over a
    chunk that is carved while unloaded falls after the reload.
  - Headless: `--tower-blast` logs the freed piece's speed at +1 and +5 frames; both must be non-zero.
- **1e, rubble budget** (client presentation; offline also the authority today):
  - Every rubble entity gets `Debris` with a **lifetime by size**, not only pieces under `debrisBodyVoxels`
    (`Scene.cpp:222` tests `IsSmall`; `PresentBlast` creates every `Detach` piece with `.debris = true`,
    `Blast.cpp:225-234`): under 25 voxels 15 s as today, under 512 voxels 20 s, larger 30 s (e, tuned by the
    owner; collapse.md proposed 8 s / 30 s). The far-rest rule (`debrisFarDistance`) stays for small rubble only
    (review: a felled tower resting 25 m away would vanish 2 s after it stops).
  - **Global caps**: `maxDebris` (192) stays for small rubble; new `maxRubbleBodies` (e: 48) for rubble of 25 voxels
    or more and `maxRubbleSlots` (e: 600 chunk-store slots) for all rubble models. Over a cap the oldest resting
    rubble is removed with dust (`debrisDespawnBursts`); if a new piece still does not fit, it becomes particles
    (stride, as `Scene::Crumble`), never a silent loss.
  - A failed `Bake` (store full) today returns `kNoEntity` after the `Detach` already removed the voxels
    (`Scene.cpp:202-205`): the piece vanishes. It crumbles to particles instead, counted.
  - Stage 3's client fracture counts its pieces against the same caps; the excess becomes particles.
  - Test `Rubble.TowerCutsStayUnderCaps` (VortexSimTests): 20 tower cuts in a row, live rubble bodies <= caps,
    store slots used by rubble <= `maxRubbleSlots`, no piece lost without particles. Headless: the `--tower-blast`
    run logs `store a/b` after the cuts; it must stay under the 7000-slot game guideline (CLAUDE.md).

### Tests for stage 1 (VortexSimTests unless noted; all GPU-free)

| Test | Today | After |
|---|---|---|
| `Blast.IslandReachingTheSearchBoxFaceIsDetachedByTheBlastThatFreedIt`: Arena (0.1 m, g = 80), 3 x 40 x 3 stone pillar `WorldOp::Box`, blast `SphereQ{(24, (g+4)*16+8, 24), 64}` with `kStrong` removes rows g+1..g+7; the rest touches the box top | 0 islands, `GetVoxel({1, g+30, 1})` stone | 1 Detach, voxel air |
| `Blast.SecondShotStillFreesTheTowerItWouldHaveFreedToday` (symptom pin): a power-0 blast at g+24 frees it today | 1 island | first blast already freed it; the second finds nothing |
| `Blast.ShortPillarControl` (height 25) | freed | freed (unchanged) |
| `Blast.LedgeCutAtItsRootFalls` (horizontal, 60 voxels) | stays | falls |
| `Blast.FaceFloodGivesUpOnTheGround` (pillar standing on ground that exits the box) | attached | attached, `faceCandidates >= 1`, `facePieces == 0` |
| `Blast.FaceFloodIndependentOfLoadingOutsideTheFloodBox`: the same blast with a chunk just outside the flood box loaded and unloaded | - | equal ops |
| `Blast.FaceFloodDefersOfflineOnAnUnloadedChunk` (a chunk inside the flood box `Loading`) | - | no face Detach, search deferred; after the load the retry frees the piece |
| `Blast.ServerFloodBoxAlwaysLoaded` (server config, casts at the `playBounds` corners) | - | `attachedByUnloaded == 0` |
| `Blast.SmallIslandProvenBeforeTheBudgetIsSpent` (4 candidates over 60k + 1 small real island) | - | the small one is a Detach, `attachedByBudget` counted |
| `Blast.FaceFloodTwoSimulationsEqualHashes`: server A blasts, B only applies; equal `WorldHash`, B's piece = A's mask count | - | equal |
| `Blast.FaceFloodBudgetIsCountedInVisits` (forest of 20 trunks): same ops with 1 and 3 workers, budget counters equal | - | equal |
| `Blast.CarveIntoLoadingChunkIsReplayedBeforeTheLateSearch` (1b) | cap floats | 1 island |
| `Blast.DeferredSearchDropIsCounted` (1c, 17 deferred searches) | silent | counter 1, log line |
| `Rubble.TowerCutsStayUnderCaps` (1e) | uncapped | under caps |
| 1d tests in VortexPhysicsTests (above) | fail | pass |
| `FloatingIslands.OneVoxelThreadKeepsThePieceAttached` (VortexCoreTests, diagnostic) | passes | passes |

Headless (game): a `--tower-blast` script shoots the watch tower's base ring at 0.5 m until it is cut and logs
`Tower: freed by shot K of K, 1 island of N voxels, face flood X ms, speed +1 / +5 frames`; the tower top voxel must
be air afterwards. The standard runs still log `Beam cut: ... 1 new object(s)`, `on the ground` and the HUD line,
exit 0 under validation, at 0.125 m and with `--voxel-size 0.1`. Windowed: the owner confirms the tower and walls fall
on the cutting shot and keep their push.

### Stage 0-1c as built (2026-11-12)

What was built, as designed above unless this table says otherwise:

| Item | As built | Why / what later stages must know |
|---|---|---|
| Per-blast log (stage 0) | `Engine::PresentBlast` logs one `Blast:` line per blast (`Blast (late search):` for a retry): total ms = carve + islands + flood + ops + objects (from `BlastResult::times`, wall clock in `sim::Blast`) + present; island / crumb counts; face candidates, proven, merged, attached by limit / known / budget / unloaded; face components over the limit; oversize islands; decodes; deferral, skipped chunks, dropped / given-up searches; edited chunks + rubble slots and the bytes they upload (36 936 B each). | Times are logged only. The counters (`BlastResult::search`, `IslandSearchStats`) are integer work and the same on every machine: what R5's work budget will count - except `chunksDecoded` and stage 4's `graphsBuilt`, which depend on streaming and on what the graph cache held (logged only, never a budget input; stage 4 review). |
| Chunk gather (stage 0) | `ExtractFloatingIslands` reads each chunk once: `StateOf` + `GetChunk`, empty 4^3 blocks skipped by `blockMask`, Uniform chunks filled by rows, a Packed chunk decoded once (`DecodeChunk` rebuilds its occupancy). `FloatingIslands.GatherByChunkEqualsPerVoxel` compares it with the old per-voxel gather on 80 random boxes over packed and unpacked built-in terrain with holes (Dense, Uniform, Packed, Empty chunks and water). | The labelling (`LabelComponents`, about 20-25 ns per solid voxel) is now most of the island search. |
| Flood API | As sketched, plus `IslandBoxScan` (the optional last parameter of `ExtractFloatingIslands`: labels, face components, `faceOversize`, `oversizeIslands`, `boxVoxels`), `ComponentLabels::firstIndex` (each component's seed = first voxel in scan order), `FloodLimits::deferOnUnloaded`, `FloodResult::merged` / `unloadedChunk`; `ignoreFlags` is `uint32_t` like `ExtractFloatingIslands`'. `sim::IslandBox` / `sim::FloodBox` give both boxes. | |
| "Visits" (rules 5, R5) | A visit is a voxel **enqueued** (marked) by a flood, the seed included. A flood gives up by limit when it has marked `maxVoxels + 1`; the budget is spent when the call has marked more than `maxVisitsPerSearch` (`kFloodVisitBudget` = 240 000). | Three floods that each reach the limit spend 180 003 visits; a fourth runs out of budget at visit 240 001 (`Blast.SmallIslandProvenBeforeTheBudgetIsSpent`: the small island's 32 visits, 1 proven, exactly 3 by limit, 1 by budget, `floodVisits` 240 001). |
| Leaving the flood box (rule 5) | A flood gives up when it **visits a voxel on the flood box face**, whatever lies outside, and when a neighbour lies below the bottom row of a fixed-height window. It never reads outside the flood box. | The decision cannot depend on a chunk outside the box (`Blast.FaceFloodIndependentOfLoadingOutsideTheFloodBox`); a piece that only touches the face from inside stays, as at the island box face. |
| In-box voxels | Judged by the island box labels: label 0 = air or liquid; a face component that is not a candidate (over the limit) and a candidate given up on are attached (`attachedByKnown`). | |
| Reads | A table over the flood box's chunks (at most 7^3) resolves each chunk once (`StateOf`, `GetChunk`); Packed chunks are decoded into at most 16 copies, FIFO; an evicted chunk decodes again when needed and `chunksDecoded` counts every decode. | |
| State | 2-bit state over 192^3 voxels as a `thread_local` (1.7 MiB), cleared through the list of touched voxels. | The first flood on a thread pays its allocation, about 1 ms (the 1.1-1.4 ms of the first tower shots). |
| Ceiling (rule 7) | Rows at or above the build ceiling of a fixed-height window are air for the flood **and** for the island box (`ExtractFloatingIslands` no longer refuses a box that reaches above it). Rows below the window's bottom row are the world's ground for both (review): the island box ends at the bottom row (`IslandBoxScan::boxMin`, so a component touching that row is attached by the face rule), `BoxLoaded` skips those rows, and a box entirely below is searched and empty. | `FloatingIslands.RowsAboveTheBuildCeilingAreAir`, `FloatingIslands.RowsBelowTheBottomOfAFullHeightWindowAreGround`, `Blast.IslandBoxBelowTheBottomOfAFullHeightWindowIsSearchedAtOnce` (before the review such a blast deferred for good: the box never loaded and the flood box never left the window). The clamp depends only on the fixed row range; `IslandBox()` stays a pure function of the sphere. |
| Server / offline (rule 6) | `SimulationConfig::sessionServer` (set by `ServerConfig`) picks the server answer: the candidate is attached, `attachedByUnloaded` counts it, `Log::Error` (`Blast.FaceFloodOnAServerCountsAnUnloadedChunk`). An island box that is not loaded is the same kind of error on a server: `islandBoxUnloaded`, `Log::Error`, nothing searched, never deferred (`Blast.ServerNeverDefersAnIslandSearch`). Offline the call stops, the in-box islands are emitted, `islandsSearched` is false with `search.floodDeferred`, `RequestLoad` asks for the whole flood box. | `sim::kServerWindowMarginChunks` = 3 with a `static_assert` against `kFloodHalfExtent`; `Blast.ServerFloodBoxAlwaysLoaded` builds the 3-chunk window and casts at the play-bounds corners with arms out to the window's end. |
| Emission (rule 9) | carve; in-box islands (scan order); proven islands (candidate order); one `ClearMask` for in-box and proven crumbs. | The crumb mask box may reach past the island box by less than `MinPieceVoxels` (networking.md "Crumbs" updated). |
| 1b carve replay | `DeferredIslandSearch` keeps `power` and `carvePending`: the chunks the carve skipped that can still load (in a fixed-height window, rows of its range), in the carve's visit order. In the first `RetryIslandSearches` call after one of them loads (the engine calls it right after streaming, before anything else can edit that chunk) the carve is applied to exactly the loaded ones (`World::CarveSphereQInChunk`: CarveSphereQ's decision and order restricted to one chunk) and is `ops[0]` of that call's late result (`carveReplayed`, `carveReplayChunks`); a result may hold only the replay while the search still waits. `PresentBlast` shows its debris, never an explosion. Review: the first version replayed the whole sphere when the search ran, which erased edits made inside the crater after the blast (`Blast.CarveReplayKeepsEditsMadeAfterTheBlast`). Offline only, never sent. | `Blast.CarveIntoLoadingChunkIsReplayedBeforeTheLateSearch` uses a generator that makes the mushroom itself, so the stem is whole when its chunk arrives. |
| 1c retry and drops | A deferred search waits for its island box and, after a flood met a chunk that is not loaded, for that chunk (`waitsForChunk`, `waitChunk`); the check is cheap, the search reruns only when they are loaded. It is given up (`deferredGivenUp` on an op-less late result, `Log::Warn`) only when its flood box shares no chunk column with the window. A full list (16) drops the search whose island box centre is farthest (squared voxels) from the window's middle, ties the oldest (`deferredDropped` on the new blast's result, `Log::Warn`). | `kIslandRetryLimit` (600 calls) is gone. |
| 1c counters | `IslandSearchStats`: `boxVoxels`, `faceCandidates`, `facePieces`, `faceCrumbs`, `faceOversize`, `oversizeIslands`, `merged`, `attachedByLimit / Known / Budget / Unloaded`, `chunksDecoded`, `floodVisits`, `floodDeferred`. | All in the `Blast:` line. |
| Headless | `--tower-blast` (7 shots around the ring at 0.5 m from frame 20, every 10 frames; logs per shot, `Tower: freed by shot K of K ...` and the piece's speed at +1 / +5 frames) and `--castle-blast` (a FillBox castle 16-36 m south of the plaza: 0.5 m curtain walls 10 m high, four hollow 4 m towers 12 m high, a brick keep with plank floors; blasts at frames 40 / 50 / 60). `--climb` now waits for a deferred search before it ends and logs `Climb: blast's island search done after N frame(s); centre voxel carved`. | |

**Tests** (VortexSimTests unless noted; 19 new, 438 in all, 2 of them disabled measurements): every row of "Tests
for stage 1" except 1d and 1e, plus `Blast.IslandOverMaxIslandVoxelsIsCounted`,
`Blast.DeferredSearchIsGivenUpOnlyOnceItsFloodBoxLeftTheWindow`, `FloatingIslands.GatherByChunkEqualsPerVoxel`,
`FloatingIslands.RowsAboveTheBuildCeilingAreAir` (VortexCoreTests) and the disabled cost case
`Blast.DISABLED_CostOfTheTowerAndCastleCases`. Written first and seen failing (flood stubbed as "all attached", carve
replay off): the pillar test, the symptom pin (today: 0 islands, then 1 on the second shot), the ledge, the
loading-independence, the offline deferral, the budget order, the two-simulation and the visit-count tests, and 1b.
`Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` is unchanged and green; no `WorldOpKind` or wire change.

**Measured** (build `-O2 -g`, UHD 620 laptop). Quiet: `Blast.DISABLED_CostOfTheTowerAndCastleCases`, flat ground,
0.125 m, no streaming, three fresh worlds; 1.2 m power-6 shots (1.5 m underground). Game: headless runs, 3 each, while
the window still streams (generation threads share the CPU: these vary 2-5x).

| Case | Island search, per-voxel gather (before) | Island search, chunk gather | Face flood | Search + flood |
|---|---|---|---|---|
| Watch tower, the shot that frees it (quiet) | 9.5-13.9 ms | 3.0-5.0 ms | 0.5-0.65 ms (8516 visits; 1.4-1.6 ms on the thread's first flood) | **3.5-4.7 ms** (6.6 ms once, with the first allocation) |
| Watch tower, other ring shots (quiet) | 9.5-13.9 ms | 3.0-4.7 ms | 0 | 3.0-4.7 ms |
| Castle curtain wall at 5 m (quiet) | 7.3-8.5 ms | 1.0-1.7 ms | 2.8-3.9 ms (60 001 visits, by limit) | **3.9-5.6 ms** |
| Castle keep wall at 4.5 m (quiet) | 8.0-9.0 ms | 1.5-1.9 ms | 2.5-3.0 ms (60 001) | **4.0-4.9 ms** |
| Castle tower at 10 m (quiet) | 7.8-8.0 ms | 1.1-1.3 ms | 2.2-2.7 ms (39 791, at the flood box face) | 3.3-3.9 ms |
| Underground 1.5 m, the `--climb` case (quiet) | 19.8-25.1 ms | 8.8-12.4 ms | 0 | 8.8-12.4 ms |
| Game: tower ring shots | 12.4-49.3 ms | 4.7-27.6 ms | 1.1-1.3 ms on the freeing shot | |
| Game: castle (3 blasts) | 15.8-24.9 ms | 2.3-10.8 ms | 4.5-12.9 ms (60 001 / 60 077 / 39 791) | |
| Game: `--probe-blast` (52^3 box) | 13.0-19.7 ms | 5.1-10.5 ms | 0 | |
| Game: `--climb` blast (72^3, underground) | 19.8-33.2 ms | 19.0-31.6 ms | 0 | |

The flood costs about 50 ns per visit, 3-10x the 5 ns per voxel collapse.md assumed and above this design's
estimate for the tower (0.2-0.6 ms) only by the first allocation; the castle case is inside its 2-6 ms. Upload per
1.2 m shot: 2-7 edited chunks (0.07-0.26 MB); the freeing tower shot 13 edited chunks + 2 rubble slots = 0.55 MB.
At 0.1 m (`--voxel-size 0.1 --tower-blast`) the tower is freed by shot 7 of 7 too: 19 106 voxels, 12 candidates,
31 027 visits, 2.4 ms of flood.

**The 3 ms rule ("Budgets"): search + flood is over 3 ms per cast** - 3.3-5.6 ms in the tower and castle cases on a
quiet CPU, and the island search alone is 3-5 ms at the tower and 9-12 ms underground. The chunk gather is not enough,
so by this document's rule **stage 4 (cell graph) must move before networking stage 3**. That was open question 4;
the owner answered yes and stage 4 is built (Part 4, "Stage 4 as built": 0.06-1.0 ms per cast in the same quiet
cases on flat ground and game terrain, cold graph builds included; cold builds in game are slower).

**Headless (validation on, all exit 0):** the standard game run (`Beam cut: 13 voxels removed, 1 new object(s)`, HUD
149 quads, `on the ground`), the editor run, `--climb` (`0 unloads inside view radius, window y fixed`; its blast
logs 0 skipped chunks and is searched at once), `--voxel-size 0.1` / `0.15`, `--probe-blast` (`0 floating island(s)`)
and `--tower-blast`: `Tower: freed by shot 7 of 7 (the ring was cut by it), 1 island of 8516 voxels, face flood
1.27 ms`. Before the flood (stubbed) the same script logged `ring cut by shot 7, tower NOT freed by it`.

**Not done / found:** stages 1d and 1e (built next, below). The tower and walls falling on the cutting shot in a
window, with a mouse, are not confirmed by the owner. The game-side times while streaming are noisy; compare with the
quiet case. (The "speed 0.00 m/s, frozen at +1" this build logged was `GetState` of a frozen body, which reads zero;
the piece kept its velocity, see 1d below.)

### Stages 1d-1e as built (2026-11-12)

| Item | As built | Why / what later stages must know |
|---|---|---|
| 1d stale collider | `World::TakeChunkChanges()` gives each changed chunk with the box (world voxels) of the voxels edits changed in it (empty for a load / unload); `TakeChangedChunks()` is the same list as coordinates. `TerrainColliders` keeps per tracked chunk the box of edits since its newest snapshot (`pendingEdit`) and, per snapshot not installed yet, the box it was the first to show (`cookingEdits`, tagged with the generation; installing generation g drops every tag <= g). The guard's test is `BodyReady` = the old chunk test **and** no touched chunk stale where the body is (bounds + one voxel + 0.05 s of travel). The public `GuardBodies()` takes the world's changes first, so a blast in a fixed update is seen by the same tick's guard. | Per-snapshot tags, not one union cleared by the newest result: a chunk edited every frame would never clear and would hold every body near a held brush (`TerrainGuard.BodyBesideAChunkEditedEveryFrameIsNotHeld`). `IsReady` (characters) is unchanged, chunk level: a player in the crater they shot is not held. |
| 1d wake on first build | Chunks edited while untracked (and loaded), or dropped while an edit was not cooked, are remembered (`modifiedUnseen`); a **first** build of one wakes its bounds (`ActivateInBox`). Every guard pass keeps only chunks a body frozen now lies on (bounds + one voxel; review: before, the list was cleared only when no body at all was frozen, so one parked crate let every later edit pile up). `TerrainColliderStats::unseenEdits`, `TerrainGuard.UnseenEditsAreRememberedOnlyUnderFrozenBodies`. | A rebuild already woke them; a first build did not, so a body parked over ground carved just before its collider was dropped (or before the chunk unloaded) resumed asleep in the air. |
| 1d logging | `BodyState::keptLinearVelocity` (frozen bodies: what they resume with). `--tower-blast` and `--probe-blast` log it for a frozen piece ("frozen, velocity kept"); `--probe-blast` logs `Probe piece N keeps its push: horizontal X m/s at +5 frames of Y m/s given` (keeps = at least half). | |
| 1d what reproduced | The rule: before 1d a piece made over its stale collider was stepped at once (`TerrainGuard.PieceOverAStaleColliderWaitsFrozenWithItsVelocity`, `Rubble.DetachedPieceWaitsFrozenWithItsPushOverAStaleCollider` failed on `IsFrozen`; renamed in the review, their velocity checks pass on the old code too), and both parked-body tests ended with the body in the air. **The lost push itself did not reproduce in isolation**: a box and a blast-cut pillar stepped inside the stale collider kept their velocity (Jolt ignores the back faces they sit behind), and in this build's game runs the piece was already frozen in its first step by a neighbour's `exposed` rule and kept its push; the 0.00 m/s at +1 logged by stage 1 was `GetState` of a frozen body. The loss view-distance.md measured came from an earlier engine. | The rule stays: no body is simulated against geometry known to be gone. If the owner still sees a lost push in a window, it is something else (record the probe log). |
| 1e lifetimes | Every Dynamic object created with `VoxelObjectDesc::debris` and a body is rubble (`markDebris` on): `Debris{age, lifetime}`; lifetime by size: under `debrisBodyVoxels` 15 s, under `rubbleLargeVoxels` (`kLargeRubbleVolume` = 1 m^3: 512 voxels at 0.125 m) 20 s, larger 30 s. Pieces `Scene::Damage` splits off rubble stay rubble: new rubble, age 0, their own lifetime (review: with the parent's age a piece of 25 s-old large rubble got a 20 s or 15 s lifetime and vanished on the next `Update`, in flight; `SceneDebris.PiecesCutOffOldRubbleAreNotRemovedAtOnce`). The far-rest rule applies to small rubble only (review: `SceneDebris.LargeRubbleFarFromTheViewerLivesItsLifetime`); for it a frozen piece still counts as resting, deliberately: small rubble frozen 25 m away has no collider and would hang there. | A `Debris` added by hand (`lifetime = -1`) keeps `debrisLifetime`. Pieces of a non-rubble object of 25 voxels or more (offline crate halves) are still ordinary objects. |
| 1e caps | `maxDebris` 192 (small rubble only now), `maxRubbleBodies` 48 (rubble of 25 voxels or more), `maxRubbleSlots` 600 (chunk-store slots of all rubble models). `CreateVoxelObject` makes room: the body cap before the bake, the slot cap after it (the bake is undone if no room is found); room = the oldest **resting** rubble (asleep, or parked; a piece frozen while waiting for colliders is not resting) that helps the full cap, removed with dust. Nothing resting left: the new piece becomes particles (one burst per `debrisParticleStride` voxels, at most 400), counted. `Update` enforces the three caps too, for rubble split by `Damage`: resting rubble first, then moving, oldest first each (review: it took the oldest moving or not; `SceneDebris.CapAfterASplitRemovesRestingRubbleFirst`). Removing any body (expiry, a cap, `Destroy`, `Damage`) wakes its bounds plus one voxel, frozen bodies there included (Jolt wakes nothing on removal; review: a crate on expired rubble stayed asleep in the air, `SceneDebris.RemovedRubbleWakesWhatRestsOnIt`); `Scene::Clear` wakes nothing. | `Scene::Rubble()` (`RubbleStats`): small, bodies, slots now; `removedByCap`, `crumbledByCap`, `crumbledByBake` since the scene was made. Stage 3's fracture must create its pieces through `CreateVoxelObject(.debris)` to be counted. |
| 1e failed bake | A rubble model that cannot be baked (store full) crumbles into particles (`crumbledByBake`, a warning) instead of returning `kNoEntity` after the `Detach` removed its voxels. An ordinary object still just fails. | |
| Logs | The `Blast:` line ends with `rubble S small, B/48 bodies, N/600 slots` (and the pieces of that blast the budget crumbled); `--tower-blast` logs at +5 frames `Tower: store a/b after the cuts; rubble ...`. | |

**Tests** (13 new, 451 in all, 2 of them disabled measurements). VortexPhysicsTests:
`TerrainGuard.PieceOverAStaleColliderWaitsFrozenWithItsVelocity`, `BodiesAwayFromAnEditAreNotHeldByTheStaleCollider`,
`BodyBesideAChunkEditedEveryFrameIsNotHeld`, `ParkedBodyFallsWhenItsGroundWasCarvedJustBeforeTheColliderWasDropped`,
`ParkedBodyOverAChunkCarvedAndUnloadedFallsAfterTheReload`. VortexSimTests: `Rubble.DetachedPieceWaitsFrozenWithItsPushOverAStaleCollider`,
`Rubble.TowerCutsStayUnderCaps` (20 tower cuts, caps lowered to 6 bodies / 8 slots so they are reached: every Detach
is a rubble body or particles, caps hold after every cut and every frame). VortexEngineTests:
`SceneDebris.EveryRubblePieceLivesByItsSize`, `RubbleThatCannotBeBakedBecomesParticles`,
`RubbleBodyCapRemovesTheOldestRestingPieceOrCrumblesTheNewOne`, `RubbleSlotCapKeepsRubbleModelsInTheirShareOfTheStore`,
`PiecesCutOffRubbleStayRubble`. VortexCoreTests: `World.ChunkChangesCarryTheBoxOfTheEditedVoxels`. Seen failing first:
the four 1d reproductions on the old `Terrain.cpp` (the two push tests on `IsFrozen`, the two parked bodies in the
air), and the five scene tests with the old rubble rules in `Scene.cpp`. Three existing tests asserted the old rule
("rubble of 25 voxels or more is not rubble") and now assert the new one:
`SceneDebris.DefaultThresholdsAtTheEngineVoxelSize`, `SceneDebris.RubbleTooSmallToBeAPieceCrumblesInsteadOfBecomingAnEntity`,
`Blast.OfflineLargeIslandStillBecomesRubble`. No `WorldOpKind`, wire or hash change; the GPU-free libraries link
nothing new. `Rubble.TowerCutsStayUnderCaps` uses the new caps and was not run against the old code (the scene tests
above show the old rules fail).

**Headless (validation on, all exit 0, 0.125 m unless noted):** standard run (`Beam cut: 13 voxels removed, 1 new
object(s)`, HUD 149 quads, `on the ground`, `0 frozen`), editor run, `--climb` (`0 unloads inside view radius, window
y fixed`, search done after 0 frames), `--voxel-size 0.1` / `0.15` (Beam cut, on the ground), `--probe-blast` (`0
floating island(s)`; `Probe piece 0 keeps its push: horizontal 3.18 m/s at +5 frames of 3.19 m/s given`; frozen at +1
with its velocity kept), `--tower-blast` (freed by shot 7 of 7; `speed 2.92 m/s (frozen, velocity kept)` at +1, `2.71
m/s (awake)` at +5; `store 5273/12288 after the cuts; rubble 0 small, 1/48 bodies, 2/600 slots`), the same at 0.1 m
(19 106 voxels, 2.96 / 2.73 m/s, store 5772/12288), `--castle-blast`.

**Not confirmed:** windowed play (the tower falling with its push, rubble disappearing after 20-30 s, dust puffs when
a cap removes rubble) is for the owner. Costs of the guard's overlap test and of the rubble listing are not measured
(a few comparisons per chunk per body; O(n log n) for n <= 240 rubble per frame).

### Review of stages 0-1e (2026-11-12)

Three reviewers read the uncommitted change; each finding was checked against the code, bugs were reproduced in a
failing GPU-free test first.

| Finding | Verdict | What changed |
|---|---|---|
| Island box rows below a full-height window's bottom count as "not loaded" forever (a blast within r/16 + 24 voxels of -48 m deferred for good, also on a server); reported twice | **Fixed** | Those rows are ground: `ExtractFloatingIslands` ends the box at the bottom row (`IslandBoxScan::boxMin/boxMax`, used for the flood), `BoxLoaded` skips them, the carve does not keep them pending. A server never defers an island search: `islandBoxUnloaded`, `Log::Error`. Tests: `Blast.IslandBoxBelowTheBottomOfAFullHeightWindowIsSearchedAtOnce`, `FloatingIslands.RowsBelowTheBottomOfAFullHeightWindowAreGround`, `Blast.ServerNeverDefersAnIslandSearch` (all three failed first). |
| Carve replay runs late over the whole sphere and erases later edits | **Fixed** | Replayed only on the skipped chunks, each in the first retry after it loads (`World::CarveSphereQInChunk`, `DeferredIslandSearch::carvePending`, `BlastResult::carveReplayChunks`). `Blast.CarveReplayKeepsEditsMadeAfterTheBlast` failed with the whole-sphere replay; `World.CarveSphereQInChunkEqualsTheWholeCarveChunkByChunk`. |
| Far-rest rule deletes large rubble 2 s after it stops 25 m away; frozen pieces count as resting; distance from the corner | **Fixed in part** | The rule is for small rubble only (`SceneDebris.LargeRubbleFarFromTheViewerLivesItsLifetime`, failed first). Kept: for small rubble a frozen piece counts as resting (25 m away it has no collider and would hang in the air until the viewer returns; a fresh piece has 2 s before the rule applies, longer than a stale-collider freeze), and the distance from the model corner (a small piece is under 25 voxels). |
| Despawning rubble does not wake what rests on it | **Fixed** | `OnRigidBodyDestroyed` wakes the bounds + one voxel + 0.05 m after the removal, for every removal but `Scene::Clear` (`SceneDebris.RemovedRubbleWakesWhatRestsOnIt`, failed first). |
| Pieces split off old rubble inherit its age with a shorter lifetime; cap pass takes moving pieces | **Fixed** | Split pieces are new rubble (age 0); the `Update` cap pass takes resting pieces first (`SceneDebris.PiecesCutOffOldRubbleAreNotRemovedAtOnce`, `CapAfterASplitRemovesRestingRubbleFirst`, both failed first; `PiecesCutOffRubbleStayRubble` now expects age 0). |
| Per-blast log sorts one entry per edited voxel, outside the timed interval | **Fixed** | Runs of one chunk collapse before the sort; the log's work counts as `present` in the `Blast:` line. No test (presentation timing). |
| `modifiedUnseen` grows while any body stays frozen | **Fixed** | Each guard pass keeps only chunks under a body frozen now (`TerrainColliderStats::unseenEdits`, `TerrainGuard.UnseenEditsAreRememberedOnlyUnderFrozenBodies`, failed first: 5 instead of 0). Rejected: dropping entries that left the window - a chunk unloaded under a parked body must stay remembered (`ParkedBodyOverAChunkCarvedAndUnloadedFallsAfterTheReload`). |
| `DeferredSearchDropIsCounted` cannot tell farthest-first from oldest-first | **Fixed (test)** | The oldest search is now the nearest; a tie at z = -80 / +80 drops the older. The new search is never itself dropped (it is the one the player just made): stated in `kMaxDeferredIslandSearches`' comment and here. |
| Server flood "unloaded" branch never exercised | **Fixed (test)** | `Blast.FaceFloodOnAServerCountsAnUnloadedChunk`; it passed on the existing code. |
| Comments state the stale collider "eats the push" | **Fixed** | Comments in `Terrain.cpp`, `TerrainGuardTests.cpp`, `RubbleTests.cpp` and ROADMAP.md state the rule; the two tests are renamed `TerrainGuard.PieceOverAStaleColliderWaitsFrozenWithItsVelocity`, `Rubble.DetachedPieceWaitsFrozenWithItsPushOverAStaleCollider`. |
| `FaceFloodBudgetIsCountedInVisits` never spends the budget | **Fixed (test)** | A second case (the four over-limit slabs) reaches the budget with 1 and 3 workers: equal ops and counters, `floodVisits` 240 001 in both. |
| Flood edge cases untested (merged, exact limit, liquids) | **Fixed (tests)** | `Blast.FaceFloodMergesAPieceThatCrossesTheFaceTwice` (2 candidates, 1 merged, 1 Detach), `FaceFloodLimitIsTheInBoxLimit` (60 000 voxels detached, 60 001 attached by limit at visit 60 001), `FaceFloodIgnoresWater`. All passed on the existing code. |
| "Four floods spend exactly the budget" is wrong | **Fixed** | The "Visits" row above; `SmallIslandProvenBeforeTheBudgetIsSpent` asserts 3 by limit, 1 by budget, 240 001 visits. |

**Tests:** 14 new, 465 in all (2 disabled measurements), all pass. **Headless** (validation on, all exit 0, 0.125 m):
standard run (`Beam cut: 13 voxels removed, 1 new object(s)`, HUD 149 quads, `on the ground`, `0 frozen`; screenshot
checked), editor run, `--climb` (`0 unloads inside view radius, window y fixed`, search done after 0 frames),
`--voxel-size 0.1` / `0.15` (Beam cut 37 / 16 voxels, on the ground), `--probe-blast` (`0 floating island(s)`, piece
keeps 3.18 of 3.19 m/s), `--tower-blast` (freed by shot 7 of 7, 8516 voxels, 8785 visits, face flood 1.70 ms; 2.92 m/s
frozen with its velocity at +1, 2.71 m/s awake at +5; store 5273/12288, rubble 1/48 bodies, 2/600 slots),
`--castle-blast`. **Per-blast cost** in these runs (-O2, world still streaming, so 2-5x noisy): tower ring shots
7.0-20.3 ms total (island search 6.4-18.4, present + log 0.45-1.6), the freeing shot 16.3 ms (islands 12.2, flood
1.7, ops 1.05, present 1.25); castle 10.4-17.1 ms (flood 6.0-9.9 ms for 39 791-60 077 visits); the `--climb` blast
40 ms (island search 37.9 ms while streaming; 8.8-12.4 ms quiet, above). Not re-measured quiet.
