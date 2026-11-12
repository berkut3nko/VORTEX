# Destruction that finishes the job ("SHRED")

Status: **stages 0, 1, 1b, 1c, 1d and 1e built and reviewed** (2026-11-12, see "Stage 0-1c as built", "Stages 1d-1e
as built" and "Review of stages 0-1e" at the end of Part 1); the owner took the recommended answers to open questions 1, 2, 3 and 7 ("Yes, build
1-1e"). The rest is a draft. Written read-only against the working tree of 2026-11-12, while the
voxel size moves from 0.1 to 0.125 m (the game already defaults to 0.125 m, `src/main.cpp:23-24`). Line numbers are
from the working tree. Every millisecond figure that does not cite a measurement is an estimate and is marked **(e)**.
Every CPU time in the docs before 2026-11-12 comes from a build without `-O` (CLAUDE.md:120-121); `build/` is now
`-O2 -g` (`build/CMakeCache.txt:60`, enforced by `engine/CMakeLists.txt:18-20`), so CLAUDE.md:36 ("the dev build is
unoptimised") is stale and the measurements asked for below can be taken in `build/` as well as `build-release/`.

Revision 2 (after review): the flood got a fixed integer reach that the server is guaranteed to have loaded; offline
defers instead of deciding "attached"; rubble of every size gets a lifetime and a cap in the same review as stage 1;
the two visible physics defects got a stage (1d); the cast budget becomes a work budget; impact order, impact
ownership, crush damage, the `ShredSphere` rule and the graph-equality claim were corrected; budgets gained the
per-cast client spike, server physics and chunk-store slots.

Relation to other designs:

- **Extends [collapse.md](collapse.md)** and supersedes parts of it (table "What happens to collapse.md" below). The
  32-byte `SupportCell`, the 8^3-voxel cells, the overhang rule, the release queue and the "pristine building
  reports, never drops" rule are kept. What changes is the **order of hosts** (terrain first) and the replication
  rules, which collapse.md predates.
- **Fits [networking.md](networking.md)**: everything that changes the world is a `sim::WorldOp` decided on the
  server (or offline) from integers; clients only `ApplyOp`. The M1 stages 3-9 (ROADMAP.md:149) are not reordered,
  but stage 1 here changes four lines of networking.md (94, 251, 262, 343; listed under stage 1).
- **Uses [fx-budget.md](fx-budget.md)** caps for everything a client shows, and **fixes [lod.md](lod.md)**'s debris
  threshold to be presentation only (Part 3).

## Decision in brief

1. **Stage 1 fixes the "second shot" bug now**, before anything else in this document. A piece freed by a blast stays
   in the air because the island search treats every component that reaches the face of its search box as attached
   (`voxel/internal/Connectivity.cpp:225-227`) and nothing searches again. The fix keeps `ExtractFloatingIslands`
   unchanged and adds a **bounded outward flood inside a fixed flood box** (blast centre +-96 voxels) for components
   that touch the face: if the flood closes inside the flood box, the component is an island and becomes an ordinary
   `Detach`. No new operation, no wire change, integer-exact. The server keeps the whole flood box loaded; offline,
   a flood that meets an unloaded chunk **defers the whole search** (as an unloaded island box does today).
   It fixes towers, walls and ledges up to 60 000 voxels (117 m^3 at 0.125 m) inside the flood box; **castle-scale
   release waits for stages 4-5.**
2. **More pieces fall, so rubble needs a budget at the same time** (stage 1e): every piece of client rubble gets a
   lifetime by size, and bodies and chunk-store slots get a global cap. Today only rubble under 25 voxels has one.
3. **Measure before optimising** (stage 0): the only island-search time on record, 70-200 ms per blast, is from an
   unoptimised build (networking.md:386-387). Stage 0 also lands the one safe speed-up (gather the box by chunk) and
   logs a per-blast total (CPU ms, edited chunks, upload bytes), because a blast is today a main-thread spike.
4. **The two physics defects the owner sees after a blast are fixed now** (stage 1d): the lost blast push and bodies
   that resume asleep in the air.
5. **Shredding improves on the client first** (stages 2-3): material-dependent chips / dust / splinters and local
   fracture of client rubble (`netId 0`) need no determinism. A material-dependent carve on the wire
   (`ShredSphere = 7`) comes later, when the encoding exists (networking stage 3).
6. **Structural updates run on terrain first** (stages 4-5), on collapse.md's cell graph cached per chunk. The
   rule is collapse.md's overhang rule (judged, one traversal); a load / stress model is an **optional later stage
   that the owner decides** (stage 7), because collapse.md rejected it for "buildings fall on spawn" and that
   argument still holds. Server bodies from a collapse need networking stage 6; before it every release is a
   `netId 0` `Detach`.
7. **Impact destruction is server-only and quantised once** (stage 6): Jolt contact energy becomes an integer
   `ImpactQ` the moment it is read; everything after that is integer, totally ordered and replays. Only authoritative
   bodies cause impacts, offline as on a server. Crush damage to players is a separate server check, not a contact.

## What happens to collapse.md

| collapse.md | Status under this draft |
|---|---|
| Stage 1: anchored remainder of a static object stays static (collapse.md:144-152) | **Kept unchanged**, still ROADMAP M3 (ROADMAP.md:169-170). Independent of this draft; can be done any time. The game creates only dynamic objects today (`src/main.cpp:304, 961`), so it has no visible effect before M3 content. |
| Stage 2: graph core, headless | **Kept and extended**: same cell format; built per terrain chunk as well as per model (here stage 4). |
| Stage 3: graph in `Scene::Damage` | **Moved after the terrain host** (here stage 5b). |
| Stage 4: no whole-object work per hit | Kept unchanged. |
| Stage 5: terrain structures | **Moved first** (here stages 4-5). The game's buildings are terrain `FillBox` boxes (`src/main.cpp:686-712`), castles must exceed 12.8 m (ROADMAP.md:174), terrain ops already replicate, object pieces need networking stage 6. |
| "What is wrong today": `minPieceVoxels = 6`, no `Lifetime`, `Engine::Explode` does islands (collapse.md:41-50) | **Stale.** Pieces under `MinPieceVoxels` (8 at 0.125 m, `scene/Scene.cppm:53-55`) are particles; rubble under `debrisBodyVoxels` (25 at 0.125 m, `Scene.cppm:41,73-75`) has `Debris`, 15 s life and a cap of 192 (`Scene.cppm:114,119`, `Scene.cpp:222`); larger rubble had neither (built: stage 1e); islands run in `sim::Blast` (`sim/internal/Blast.cpp:68-114`). The "tower stays in the air" line (collapse.md:47-48) is the bug of stage 1 here. |
| Cell format "more than 8 components merge into node 7" (collapse.md:74-75) | Kept, but it over-connects, so the graph is **not** equal to voxel connectivity in such cells (stage 4 tests say so). |
| Rejected "CSR node/edge graph with bond areas, masses and a load check (B)" (collapse.md:28-31) | **Kept rejected for the default.** Revisited only as optional stage 7, with a spawn guard that answers the rejection. |

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

### Fix: prove face-touching components by an outward flood (stage 1)

Where: `sim::SearchIslands` (`Blast.cpp:68-114`), with a new GPU-free function in `vortex.voxel`
(`voxel/Connectivity.cppm`; implementation in `internal/Connectivity.cpp`):

```cpp
inline constexpr int32_t kFloodHalfExtent = 96;      // voxels, NOT rescaled with the voxel size (like the margin)
// Flood box of a blast: floor(c / 16) +- kFloodHalfExtent, a pure function of the integer sphere.
// 192 voxels per axis = the ExtractFloatingIslands region limit; 24 m at 0.125 m, 19.2 m at 0.1 m.
struct FaceComponent { uint32_t label; glm::ivec3 firstVoxel; uint32_t inBoxVoxels; };
struct FloodLimits   { glm::ivec3 floodMin, floodMax;  // the flood box
                       uint32_t maxVoxels;             // kMaxIslandVoxels
                       uint32_t maxVisitsPerSearch; }; // work budget, R5 below (4 x kMaxIslandVoxels)
struct FloodResult   { std::vector<FloatingIsland> islands;   // closed floods, in candidate order
                       bool complete;                          // false: a chunk of the flood box was not loaded
                       uint32_t candidates, proven, attachedByLimit, attachedByKnown, attachedByBudget,
                                attachedByUnloaded, chunksDecoded, visits; };
// ExtractFloatingIslands gains an optional out-parameter listing face-touching components
// whose in-box count is <= maxIslandVoxels (label order).
FloodResult ProveFaceComponents(const World&, const MaterialLibrary*, std::span<const FaceComponent>,
                                const glm::ivec3& boxMin, const glm::ivec3& boxMax,
                                const ComponentLabels& boxLabels, uint8_t ignoreFlags, const FloodLimits&);
```

Rules:

1. **Candidates**: components that touch the island box face **and** whose in-box voxel count is at most
   `kMaxIslandVoxels`. Anything larger can never be detached anyway, so the ground under a surface hit (33 layers of
   66 x 66 voxels = 144k in the box at 0.125 m) is never flooded. **Candidates are processed by in-box count
   ascending, ties by label**, so small real islands are proven before large floods spend the budget. This order is
   part of the contract.
2. **Flood**: FIFO breadth-first from the candidate's first voxel in box scan order, through world voxels,
   6-connected, neighbours in the order `LabelComponents` uses (`Connectivity.cpp:92-97`), solid = id != 0 and not
   `Liquid` (the ignore mask `Blast.cpp:74` passes today), **never leaving the flood box**.
3. **Reads** (never a whole-chunk copy: `World::ReadChunk` copies 36 936 B for a Dense chunk, fills one for a
   Uniform chunk and RLE-decodes a Packed one, `World.cpp:1325-1338`): keep the last chunk coordinate and its
   `World::GetChunk` pointer / uniform id (`World.cpp:1316-1322`, `World.cppm:296`); read Dense voxels through the
   pointer, Uniform through the id. A Packed chunk is decoded **once per search** into a map keyed by chunk
   coordinate, at most 16 entries (16 x 37 KB = 0.6 MB), all dropped at the end of the search; an evicted entry that
   is needed again is decoded again and counted (`chunksDecoded`). Packing is never a content change.
4. **State**: a 2-bit array over the flood box (192^3 x 2 bits = 1.7 MiB per worker, allocated once): unvisited,
   visiting, island, attached. Only words touched by a search are cleared (from the visit lists), never a full
   memset.
5. **Gives up as attached** (and stays in the world) when any of these holds:
   - the visited count exceeds `kMaxIslandVoxels` (`attachedByLimit`);
   - the flood would leave the flood box, or go below the window's bottom row (`attachedByLimit`). The flood box is a
     function of the integer sphere only, so this never depends on what is loaded;
   - the flood reaches a voxel marked attached, inside the box (a non-candidate, or a candidate given up on) or
     outside it (a voxel visited by a flood that gave up by limit; joining it means the same over-limit component)
     (`attachedByKnown`). This keeps forests and castles cheaper: the first trunk or wall flood that gives up marks
     its voxels; later floods that touch them stop at once;
   - the per-search work budget `maxVisitsPerSearch` is spent (`attachedByBudget`; counted in voxel visits, never in
     time). Every later candidate is attached by budget as well.
6. **A chunk inside the flood box that is not loaded** (outside the window, `Unloaded` or `Loading`):
   - **Server**: cannot happen by construction (see "Server loading" below). It is counted in `attachedByUnloaded`,
     the candidate is treated as attached so nothing diverges, and it is an error counter **asserted zero in tests**
     and logged in the session.
   - **Offline**: the whole search is refused (`complete == false`, the same path as `searched == false` today,
     `Blast.cpp:146-152`): no `Detach` from the face flood, `World::RequestLoad` on the flood box, and
     `RetryIslandSearches` repeats island search and flood once it is loaded. The in-box islands of the first search
     are still emitted at once (they do not depend on the flood). A flood never gives up for good because the world
     was still streaming.
7. **Above the build ceiling** nothing exists (`EditReport::outOfBounds`, `Scene::DestroyAbove`), so the flood treats
   those rows as air, never as "not loaded". This also fixes the never-falls case of stage 1c at the ceiling.
8. **Closed flood** = an island. Candidates whose first voxel was already visited by an earlier closed flood are
   skipped (two face components can be one island joined outside the box).
9. **Emission order** (part of the contract): carve; in-box islands as today (scan order); proven islands in
   candidate order (rule 1); one `ClearMask` for all crumbs, in-box and proven. Proven islands of at least
   `MinPieceVoxels(vs)` are a `Detach`, smaller ones are crumbs.

Why not simply a bigger island box: the 192-voxel region limit and the label array are cubic (a 192^3 box is 7 MB of
ids and 28 MB of labels, e); the flood costs in proportion to the piece.

**Scope, plainly:** stage 1 frees towers, walls, ledges and floors whose whole remaining piece is at most 60 000
voxels (117 m^3 at 0.125 m, 60 m^3 at 0.1 m) **and** lies inside the flood box (+-12 m at 0.125 m). In a castle,
every wall or floor that leaves the island box usually connects to more than 60k voxels of castle and stays up
(`attachedByLimit`), so castle-scale collapse is stages 4-5, not stage 1.

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

### Network and determinism rules for stage 1

- No new `WorldOpKind`, no change of `Detach` (`sim/WorldOp.cppm:16-26`): clients still apply what the server
  sends, so `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` (`tests/sim/BlastTests.cpp:350`) is unaffected.
- The search **result** changes for the same input: which voxels a blast detaches is a server decision.
- Integer only: voxel coordinates, counts, the flood box and a fixed visit and candidate order. The budget counts
  visits, so the decision does not depend on thread count, machine speed or what streamed in; with the work-budget
  cast queue of R5 the tick a cast runs on does not either.
- Masks grow: a proven piece can span up to 192 voxels per axis, raw mask 864 KiB at the limit, RLE on the wire.
  The tower: about 26 x 64 x 26 voxels = 5.4 KiB raw, est. 1-2 KB RLE; inside the event burst of 16 kB
  (networking.md:88). A piece whose RLE mask exceeds the burst is still sent (correctness first) and logged; the
  structure stages bound this properly.
- The crumb mask box can now extend past the island box by less than `MinPieceVoxels` per side; the bound in the
  comment at `Blast.cpp:94` and in networking.md "Crumbs" changes from `(2(r/16 + 24))^3` to
  `(2(r/16 + 24) + 2 * MinPieceVoxels)^3` bits.

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
| Per-blast log (stage 0) | `Engine::PresentBlast` logs one `Blast:` line per blast (`Blast (late search):` for a retry): total ms = carve + islands + flood + ops + objects (from `BlastResult::times`, wall clock in `sim::Blast`) + present; island / crumb counts; face candidates, proven, merged, attached by limit / known / budget / unloaded; face components over the limit; oversize islands; decodes; deferral, skipped chunks, dropped / given-up searches; edited chunks + rubble slots and the bytes they upload (36 936 B each). | Times are logged only. The counters (`BlastResult::search`, `IslandSearchStats`) are integer work and the same on every machine: what R5's work budget will count. |
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
so by this document's rule **stage 4 (cell graph) must move before networking stage 3**. That is open question 4,
which the owner has not answered yet (recommendation: yes).

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

## Part 2 - Rules every later stage follows

- **R1 Server decides, clients apply.** Island proofs, structural releases and impacts run in `Vortex::Sim` on the
  server or offline; the output is `WorldOp`s in `opSeq` order. Clients never re-run a solver or an island search
  (networking.md, rejected alternatives). How a client cuts its **own** rubble (`netId 0`) is presentation and free.
- **R2 Integer operations only.** New op kinds take values 7 and up, integer parameters, and decide per voxel from
  `(voxel, old id, op integers, integer material table)`. Noise in an op comes from `vortex::Hash3`
  (`core/Random.cppm:22-27`), pinned by a golden-value test.
- **R3 Quantise floats once, at emission.** Jolt values (contact point, speed, masses) become integers on the server
  the moment they are read; Jolt is not cross-platform deterministic here (networking.md:41 row "Jolt").
- **R4 Fixed visit order, total orders only.** Graph cells in index order, neighbours in a fixed order, FIFO buckets,
  ties by index; candidates by (in-box count, label); impacts sorted by their **full integer key after
  quantising** (Part 5), never by arrival or by Jolt `BodyID` (process-local, `PhysicsWorld.cpp:228-229`,
  networking.md:45).
- **R5 Budgets count work, never wall-clock**, on the server: visits, voxels, ops, bodies, bytes per tick. This
  includes the cast queue: networking.md:94's "queued casts within a CPU budget (est. 6 ms)" becomes **a work budget**
  (island-box voxels scanned + flood visits + graph node visits + ops + event bytes per tick), calibrated once from
  stage 0's optimised measurement to about 6 ms on the reference machine (e); wall-clock is only logged. Then the
  tick a cast runs on depends only on the queue, and a replayed server makes the same decisions. Replay tests feed
  casts at fixed ticks. Only client presentation uses millisecond budgets (fx-budget `FxDirector`).
- **R6 One integer material table** (`MaterialPhysicsQ`, stage 3) hashed into `Hello::buildHash` (networking.md:88).
  This also makes explicit what `CarvableIds` already relies on (float hardness read on both sides,
  `World.cpp:1547-1560`).
- **R7 Over budget means later or cosmetic, never lost.** A release over the tick budget waits, static and solid; a
  piece over the body cap becomes `netId 0` rubble or crumbs (networking.md:28: "never dropped silently"); rubble
  over its cap becomes particles. Impacts are the one exception: they are derived and may be dropped after 30 ticks,
  with a counter.

## Part 3 - Shredding (how material breaks)

Today every material breaks into the same 4 + 2 debris and 1 + 1 dust per burst, only the colour differs
(`fx/internal/Effects.cpp:286-310`); hardness is only a carve threshold; one island is one blob body
(`Blast.cpp:87-91`); `Scene::Damage` ignores hardness (`Scene.cpp:355-358`, notes/scene-engine.md:143) so iron cuts
like dirt.

- **Integer material table** (`vortex.voxel`, `Material.cppm`, Core): `MaterialPhysicsQ{hardnessQ u16, densityQ,
  fractureCell, grainAxis, roughness, crumble, carvable}` (16 B), built once from `Material`, hashed (R6).
  `hardnessQ = min(hardness * 16, 65535)` (saturated: bedrock's 1.0e9 does not fit, `Material.cpp:155`), and
  `carvable = !(flags & Indestructible)` (water and lava have hardness 0 but are `Indestructible`,
  `Material.cpp:143, 152`). Test `MaterialQ.SaturatesAndKeepsIndestructible`. Bond strengths are added only if
  stage 7 is taken.
- **Crumble profiles by material** (client, `fx`): powder (sand, dirt, snow: little debris, more dust), chips
  (stone, brick), splinters (wood, planks: stretched particles, no dust), shards (glass, crystal), chunks (iron). They
  replace the fixed counts in `SpawnVoxelBreak` (`Effects.cpp:303,309`) and stay inside fx-budget caps: 1500 spawns
  per frame, 8192 alive, particles GPU <= 2.5 ms (fx-budget.md:60, 77). Dust is the fill problem on the UHD 620
  (fx-budget.md:44); hard materials get **less** dust than today.
- **Fracture-surface dust**: for each `Detach` / released piece, voxels of the piece that faced the remaining world
  get dust bursts (at most 64 per op, 200 per frame), so a falling tower sheds dust along the cut.
- **Local fracture of client rubble** (`voxel:fracture`, Core, pure function): seeds on a `fractureCell^3` grid
  jittered by `Hash3`, nearest seed by integer squared distance (wood: grain axis weighted 1/4 -> splinters), then
  `LabelComponents` per region; pieces under `MinPieceVoxels` become crumbs; over `maxPieces` the smallest merge.
  `PresentBlast` fractures the `BuildDetachedPiece` volume with LOD (6 pieces near, 2 at 15-60 px, dust only far,
  fx-budget.md:92-102). Pieces count against stage 1e's caps; the excess becomes particles. No wire change.
- **Distance LOD is presentation only, in one place.** lod.md stage 1 scales the piece threshold in `Scene::Damage`
  with distance (lod.md:156-158; its test "Damage at 60 m creates no entity below 197 voxels", lod.md:183-188). The
  piece / crumb rule that decides `Detach` against `ClearMask` (and, on a server, which object pieces get entities)
  must come from the session voxel size only, never from a scene's config (`Scene.cppm:45-53`, `Blast.cpp:79`), and
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

  int64 throughout; `>=` like `CarvableIds` (power equal to `hardness * 16` carves, `World.cpp:1556-1557`). Soft
  ground loses the whole sphere, stone a smaller jagged crater, iron survives; water, lava and bedrock never go.
  Golden tests: `ShredSphere.LiquidsAndBedrockSurviveAnyPower`, `ShredSphere.FalloffAndRoughnessZeroEqualsCarve`
  (voxel for voxel against `CarveSphereQ`), `ShredNoise.GoldenValues`. `Carve = 1` stays for tools and the editor.
  The same integer rule replaces the float sphere in `Scene::Damage`.
- **Server-side fracture** (with networking stage 6): the server fractures released pieces and emits one `Detach`
  per piece. A `DetachFractured = 8` op (mask + seed + params, clients recompute the partition) is an optimisation
  only after the golden test of `Fracture` exists.

## Part 4 - Structural updates after destruction

### Host and scope

- **Terrain first.** Cells of collapse.md (8^3 voxels = `ChunkOccupancy::cellMask` granularity,
  `voxel/Chunk.cppm:31-40`; 32-byte `SupportCell`, collapse.md:60-68) are **cached per chunk**, built lazily on the
  first edit near them: Uniform chunk = one constant cell (none for a uniform **liquid** chunk), Empty = nothing,
  Packed = decode first (0.016-0.03 ms, view-distance.md:315), Dense = 64 cells from the voxel masks **minus a
  per-chunk liquid mask**: the occupancy masks count every non-zero id (`Chunk.cppm:82-95`), liquids included,
  while the island search ignores `Liquid` (`Blast.cpp:74`). A chunk that holds any liquid builds its cells from the
  ids (e: 20-50 us per chunk either way). Invalidated by a new `World::ChunkRevision(coord)` bumped where
  `EditRegion` queues a change, so editor and game `FillBox` edits are seen too. Memory: a 32 m castle = 512 chunks
  = 1 MB of cells (e), only near edits or structures.
- **Connectivity on the graph replaces the voxel search** (stage 4), inside the same flood box as stage 1 (+-96
  voxels; no new server margin). Graph connectivity equals voxel 6-connectivity **except in cells with more than 8
  components**, which merge into node 7 and over-connect (collapse.md:74-75). Cells over 8 components are resolved
  by the exact per-voxel search for those cells (decided in stage 4 from a measured count; fallback: keep the merge
  and accept "the graph never detaches what the voxel search keeps"). A per-search counter `overfullCells` is
  logged. Only released nodes are re-labelled per voxel to build `Detach` masks.
- Switching from the voxel search to the graph **can change server results** where cells are over-full; networking.md
  says so in stage 4's commit (it is a server decision, clients only apply).
- **Overhang rule inside registered structures** (stage 5). `sim::StructureRegistry` holds integer voxel boxes:
  the playground buildings now, prefab placements in M3/M4. Inside a box: collapse.md's 0-1-2 bucketed search
  (up 0, side 1, hang 2; collapse.md:94-97) from anchors = solid nodes in the ring of cells outside the box (the
  natural ground) and indestructible nodes; a node falls if unreached or past its material's limit (stone 3, wood 6,
  iron 10 cells). **Outside structures: connectivity only**, so cliffs never fail by themselves.
- **No-fall-on-spawn guard**: registering a structure runs the solve once; nodes that would fall are reported and
  pinned (a per-cell flag), as collapse.md:127-128 asks.

### Propagation over ticks (server or offline)

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
  budget (24 kB/s, burst 16 kB, networking.md:28,88). Queued parts stay static and solid until their turn
  (collapse.md:108-110). A release dirties its neighbours, so cascades unfold over ticks: a castle crumbles over
  0.5-2 s (e) instead of one spike.
- **New server bodies are limited by what a client can build**, not by the server: a client builds about 4 bodies
  per frame (0.3-0.5 ms each, collapse.md:104, e) inside fx-budget's 4 ms CPU (fx-budget.md:61), which at 30 fps is
  about 120 per second. So `serverBodiesPerTick = 2` (120/s); further releases in the same tick are `netId 0`
  rubble (subject to stage 1e's caps, then particles).
- **Client side of a queued piece**: a client applies every op at once in `opSeq` order (it must: later ops may touch
  the same chunks). For a `Detach` whose body cannot be built this frame, the voxels are cleared, the piece volume is
  kept (`BuildDetachedPiece`), and dust is shown along the cut until the body exists; a `netId != 0` piece then
  appears at its latest snapshot pose (a kinematic copy, networking.md:232-233), a `netId 0` piece at its start pose
  if it is built within 4 frames, otherwise as particles. Holding back the `Detach` itself was rejected: it would
  hold back every later op.
- Released groups are cut by collapse.md's bricks or by `Fracture`; pieces of at least the replicated threshold
  (512 voxels, networking stage 2) become server bodies with a netId up to the 64 awake / 256 live caps
  (networking.md:235-237) **once networking stage 6 exists**; before it, and for smaller or over-cap pieces, they
  are `netId 0` client rubble; under `MinPieceVoxels`, crumbs.
- After a release: `ActivateInBox(release bounds + 1 m)`. Bodies never count as support.
- `BlastResult` generalises to `OpBatch{cause: Blast | Collapse | Impact}` and `PresentBlast` to `PresentOps`, so
  collapse and impact releases get rubble and dust too; `Engine::Run` calls it next to `RetryIslandSearches`
  (`Engine.cpp:163-165`).
- Sleeping server bodies are evicted as `EntityDespawn{toRubble}` (networking.md:235-236). Rubble lifetimes and caps
  are stage 1e, not here.

### Optional later: load flow (stage 7, owner decision)

A stress model (per-node mass, per-face bond area, integer load flow over a support-distance DAG, compression / shear
/ tension capacities per material, plus a tip check of the centre of mass per layer) gives "the floor drops when a
wall goes" from physics instead of a distance limit. It needs per-material tuning and is what collapse.md rejected;
it comes only if stage 5 feels wrong in play, with the spawn guard extended to "reinforce and report" overloaded
edges. Est. 0.1-0.3 ms per structure solve (e).

## Part 5 - Impact destruction by estimated weight (stage 6)

- **Contacts become usable**: `ContactEvent` (`physics/PhysicsWorld.cppm:101-107`) gains `invMassA/B` (0 for static
  / terrain, read in the listener while bodies are locked, `PhysicsWorld.cpp:207-208`) and the compound child index
  of each side. **Drain every tick** inside `Simulation::StepPhysics`: today nobody calls `TakeContactEvents`, so the
  16 384-event queue fills and stays full (notes/physics.md:160-161).
- **Child -> material**: a `ColliderMaterials` component filled in `CreateBody` from `BuildColliderBoxes(.., false)`
  (`Scene.cpp:42-47`).
- **Ownership, not existence, decides who may cause an impact.** Offline, client rubble lives in the same
  `Simulation` and `PhysicsWorld` (`sim::PresentBlast` creates it in `simulation.Scene()`, `Blast.cpp:225-234`) and
  every piece is an entity (`replicatedPieceVoxels = 0`, `Scene.cppm:100-109`); a 24-511-voxel piece (up to about
  1 m^3, 2.6 t of stone at 0.125 m) falling 5 m carries about 127 kJ, far over `Emin`. So bodies created as rubble
  carry a `clientRubble` bit in their Jolt user data (`VoxelObjectDesc::clientRubble`, planned in networking.md:227;
  `ContactEvent::userDataA/B`, `PhysicsWorld.cppm:103`). `sim::Impact` ignores every contact whose dynamic side is
  client rubble or below the replicated threshold of the session's voxel size, **offline included**. Offline
  allocates `NetId`s as a server does (the offline simulation is the server), so the sort key below exists there too.
- **`sim::Impact`** (server / offline, in the cast phase of the next tick): per event, energy `E = 0.5 mu v_n^2`
  (mu = reduced mass; `1 / invA` against static), early reject under `Emin` (e: 500 J). Then quantise once (R3):
  `ImpactQ{pointQ, normal int8[3], energyQ (J/4), netA, netB, matA, matB, terrainChunk, tick}`. **Then** deduplicate
  per `(netA, netB, terrainChunk)` (keep the highest `energyQ`, ties by the rest of the key) and sort by the full
  integer key `(netA, netB, pointQ.x, pointQ.y, pointQ.z, energyQ desc, matA, matB)`. `PhysicsWorld` today keeps one
  event per Jolt `BodyID` pair (`PhysicsWorld.cpp:251-258`), and terrain is many per-chunk static colliders with no
  netId, so one body landing across two terrain chunks gives several events with the same `(netA, 0)`; without the
  full key, ties fall back to Jolt `BodyID` order, which depends on when collider cooks finished.
  - Terrain side: crushed volume `V = eta * E / breakEnergy[ground]` (eta 0.3, e); radius by integer cube root;
    power = the struck material's `hardnessQ`; emit a `Carve` (later `ShredSphere`) half a radius into the surface
    and mark it dirty for the structural solver, so a falling tower cracks the wall it lands on.
  - Body side (server bodies only, needs networking stage 6): dent with a small `Damage` below a shatter threshold,
    `Fracture` above it; emits `ObjectModel` for the survivor and `EntitySpawn` / `EntityDespawn` for pieces.
  - Calibration (e): 1 m^3 of stone (2600 kg) dropped 5 m = 127 kJ -> a 0.3 m crater and the block shatters; a
    347 kg crate at 5 m/s = 4.3 kJ -> a dent in dirt, no shatter.
- **Cooldowns and budgets** (tick-counted state, R5): one fracture per entity per 15 ticks, sub-threshold hits add
  decaying fatigue, new fragments are immune for 30 ticks; per tick at most 4 impact ops and 2 shatters, taken in
  the sorted order; new bodies share the collapse budget (`serverBodiesPerTick`, 64 awake / 256 live). Over budget
  an impact may only dent; queued impacts older than 30 ticks are dropped and counted (R7).
- Before networking stage 6 there are no server bodies: impacts act on terrain only, from authoritative offline
  bodies (crates, `Damage` pieces of at least the replicated threshold).
- **Crush damage to players is not a contact.** Players are `JPH::CharacterVirtual` (`PhysicsWorld.cpp:21, 311,
  1038`), which never reaches the body `ContactListener` (`PhysicsWorld.cpp:213`), and on a client moving pieces and
  all local rubble never block the predicted player (networking.md:213-214). So crush is a separate server check
  each tick: a sweep of every awake server body (`netId != 0`) against the player capsules, damage from the
  quantised relative speed and mass, sent as a damage event (M2 game logic). `netId 0` rubble never damages.

## Budgets

### Server (60 Hz tick, 16.7 ms; cast phase est. 6 ms, networking.md:94, to become a work budget, R5)

| Item | Cost (e unless cited) |
|---|---|
| Island search today, one 1.2 m blast | 70-200 ms **unoptimised** (networking.md:386-387); est. 4-12 ms optimised (fx-budget.md:41). Stage 0 measures it |
| Island search after stage 0's chunk gather | est. 2-8 ms (gather is about half the per-voxel work, e); stage 0 measures it |
| Face flood (stage 1) | 0-6 ms per blast, bounded by `maxVisitsPerSearch` |
| **Search + flood per cast (stages 0-1)** | **est. 2-14 ms: one castle cast can use the whole 6 ms cast phase**; the work budget then moves later casts to later ticks (cast delay, logged) |
| Graph connectivity + face flood on nodes (stage 4) | 0.1-0.5 ms per blast including cell refresh |
| Overhang solve, one structure (<= 5k nodes) | 0.05-0.3 ms |
| Per release: mask + `ClearMask` | 0.05-0.2 ms |
| Per server body: colliders + body, no bake | 0.1-0.3 ms |
| `physics.Step` with 64 awake compound bodies + terrain | 1-3 ms, **unmeasured** (`--collapse` measures it, threads = 1 and hw-1) |
| Terrain collider re-cooks per edited chunk (Carve, Detach, ClearMask dirty the chunk and its 6 neighbours) | 0.1-0.4 ms each on workers; the main-thread scans are est. 2-4 ms while debris flies (fx-budget.md:46) |
| Worst tick under `CollapseBudget`, excluding casts | about 3-6 ms (releases + bodies + physics) |
| Average at 10 casts/s | under 1 ms of cast work **only after stage 4**; with stages 0-1 only, est. 1-6 ms average (1 cast per 6 ticks x 2-14 ms, spread by the work budget) |

**Stage 0 decides the schedule:** if the optimised island search plus the castle-case flood is over about 3 ms per
cast, the chunk gather (stage 0) is not enough and **stage 4 moves before networking stage 3**, so that the encoded
operation stream is tested against the search the server will really run.

### Client, UHD 620 (frame about 21.5 ms on the far view: gbuffer 12.20 / lighting 7.28-7.38 / composite 1.24-1.40, view-distance.md:425)

| Item | Cost |
|---|---|
| Destruction + effects, CPU main thread | <= 4 ms per frame (fx-budget.md:61) |
| Effects, GPU | <= 4 ms (particles 2.5, effect lights 0.5, uploads 1.0; fx-budget.md:60) |
| Applying a `ClearMask` | 1-2 ms for the largest measured mask, unoptimised (networking.md, "Crumbs"); est. 0.2-0.6 ms optimised |
| Each local rubble body | 0.3-0.5 ms CPU (collapse.md:104, an estimate there) + 1-3 chunk-store slots of 36 936 B; cap 4 new per frame, the rest wait (dust) or become particles |
| Edited chunk uploads | whole chunks of 36 936 B; a 1.2 m blast plus a tower piece edits est. 10-20 chunks = 0.4-0.75 MB; at most 16 per frame target (fx-budget.md); today 128 (`Renderer.cppm:42`) |
| **Per cast, offline (today's game: the client is the authority)** | carve est. 0.2-1 + search est. 4-12 (2-8 after stage 0) + flood 0-6 + `ClearMask` 0.2-0.6 + bodies 0.3-0.5 each + uploads: **est. 5-20 ms in one frame**, over the 4 ms cap by itself |
| **Per cast, session client** (no search, no flood) | apply ops est. 0.2-1 + bodies (capped 4 per frame) + uploads: est. 0.5-3 ms |
| **Client frame during 10 casts/s** (30 fps: 1 cast per 3 frames on average, bursts of 2-3 in one frame, e) | session client: est. 1-6 ms on burst frames, inside 4 ms only with the body cap and U1's upload budget; offline: not inside 4 ms until stage 4 (or a server process) takes the search off the frame |
| ChunkStore slots | filled window 5311 / 5629 at 0.125 m (view-distance.md:422) + rubble <= `maxRubbleSlots` (600, e) + server bodies <= 256 x 1-3 = 768 worst case: **about 7000 worst case**, at the game guideline; `--collapse` gates on `store a/b` |
| Extra instances in the ray-traced passes | **unmeasured**: `--collapse` / `--stress-fx` measure it |

Stage 0 logs, per blast: CPU ms of carve / search / flood / ops / bodies, edited chunk count and upload bytes; the
row "client frame during 10 casts/s" is filled from that log and a `--stress-fx 10` run (fx-budget stage 1), and
perf.md's U1 (upload byte budget) is sized from it.

Wire: a castle collapse is est. 2-12 KB of masks spread over about 0.5 s by the event budget; a `ShredSphere` is about
20 B.

## Stages

Each stage: design note first where a contract changes, build, ctest, both headless runs (plus the stage's own),
screenshot, review, ROADMAP update in the same commit.

| # | Built | Accepted when | Wire / contract (needs) | Effort (e) | Milestone |
|---|---|---|---|---|---|
| 0 | **Measure + one safe speed-up**: a `Blast:` log line per blast (carve / search / flood / ops / bodies ms, voxel counts, edited chunks, upload bytes); the island box gathered by chunk (`GetChunk` once per chunk, ids read through the pointer in row order instead of `World::GetVoxel` per voxel, `Connectivity.cpp:205-211`); write this design into `engine/docs/design/shred.md`, update collapse.md's stale lines | Labels and emitted ops equal before and after the gather (existing `Blast.*` tests + `FloatingIslands.GatherByChunkEqualsPerVoxel` on random volumes with Dense / Uniform / Packed chunks); island-search ms for the probe blast, the `--climb` blast, a tower shot and a castle-like `FillBox` case recorded, 3 runs each, before and after; the stage-4 schedule decided from them | none | 1 day | now |
| 1 | **Face flood** (`ProveFaceComponents`) in the flood box, offline defer, server error counter, counters, ceiling-as-air, smallest-first candidates | Tests of stage 1 green; `--tower-blast` logs the tower freed by the cutting shot; flood ms within the table above; networking.md:94 (work budget), :251 (3-chunk margin), :262 (EnsureLoaded flood box), :337, :343 and voxel.md:316-317 updated | search result, no format change | 2 days | now, before networking stage 3 |
| 1b | Carve replay before a late search | `Blast.CarveIntoLoadingChunkIsReplayedBeforeTheLateSearch`; `--climb` blast logs 0 skipped after the retry | none (offline only) | 0.5 day | now |
| 1c | Nothing silent: drop / give-up / oversize / flood counters, retry until the flood box leaves the window | `Blast.DeferredSearchDropIsCounted`, `Blast.IslandOverMaxIslandVoxelsIsCounted` | none | 0.5 day | now |
| 1d | Physics after destruction: stale collider not ready for new pieces; wake on first build of a modified chunk | the two VortexPhysicsTests; `--tower-blast` speed at +1 and +5 frames non-zero | none (physics only) | 1 day | now |
| 1e | Rubble budget: lifetime by size for all rubble, `maxRubbleBodies`, `maxRubbleSlots`, failed bake crumbles | `Rubble.TowerCutsStayUnderCaps`; `--tower-blast` `store a/b` under 7000 | none (presentation) | 0.5-1 day | now, same review as 1 |
| 2 | Crumble profiles, fracture-surface dust, carve lumps (client) | fx caps hold with `--stress-fx 10` (fx-budget stage 1); particles GPU <= 2.5 ms; screenshots per material | none | 2-3 days | M2, with fx-budget stages 1-2 |
| 3 | `MaterialPhysicsQ` + `voxel:fracture`; client rubble fractured with LOD (presentation-only distance rule; lod.md stage 1 changed to match) | `Fracture.*` tests (cover exactly, disjoint, same seed same result, cell size controls count, wood splinters along grain, small pieces are crumbs); `MaterialQ.SaturatesAndKeepsIndestructible`; fracture stays under 1e's caps; table hash stable | material table hash in `buildHash` | 3-4 days | M2 |
| 4 | Cell graph cached per terrain chunk (liquid-masked); graph connectivity + node face flood replace the voxel search in the same flood box | `Structure.GraphNeverDetachesWhatTheVoxelSearchKeeps` (random volumes, liquids included), `Structure.GraphEqualsLabelComponentsWithoutOverfullCells`, `RefreshEqualsRebuild`, `GraphFloatingEqualsFaceFlood` on every stage-1 test; `overfullCells` logged; island search <= 0.5 ms optimised at the probe blast | same op kinds; **results may change in over-full cells** (networking.md notes it) | 4-6 days | M3, **or before networking stage 3 if stage 0 shows search + flood over 3 ms** |
| 5 | `StructureRegistry`, overhang rule inside structures, `StructuralSolver` with tick budgets and release queue, spawn guard, `OpBatch` / `PresentOps`, client queued-piece handling, headless `--collapse` | `Structure.StoneOverhang3HoldsAnd4Falls`, `PristineStructureStable`, `SolveIndependentOfDirtyOrder`; `Collapse.TowerReleasedOverTicksWithinBudget`, `Collapse.SameOpsTwoSimulationsEqualHash`, `Collapse.ReplayGivesIdenticalOpList` (casts fed at fixed ticks); `--collapse` logs `Collapse: N releases over T ticks, max B bodies/tick, X ms max`, physics step p99, `store a/b` under 7000, validation exit 0 | `Detach` with `netId 0` and `ClearMask` only. **Server bodies (`Detach` with `netId != 0`, `EntitySpawn`, `EntityRest`) need networking stage 6** (networking.md:295); before it every release is a `netId 0` Detach | 1.5-2 weeks | M3 |
| 5b | collapse.md stages 3-4 on objects (same graph code) | collapse.md's own criteria | `ObjectModel`, `EntitySpawn` / `EntityDespawn` (networking stage 6) | 1 week | M3 |
| 6 | Contacts drained and extended, `ColliderMaterials`, ownership bit, `sim::Impact` with total order, cooldowns and budgets; crush check | `ContactQueueNeverFillsWhenSimulationDrains` (10k ticks), `Impact.HeavyDropCarvesLightDoesNot`, `CooldownAndFatigue`, `BudgetCapsBodiesPerTick`, `ShuffledEventsSameOps` (shuffles include several events with an equal netId pair across two terrain chunks), `ClientRubbleNeverCausesImpacts` on the offline **and** the server config, `Crush.FallingServerBodyDamagesPlayer`, `Crush.RubbleNeverDamages` | terrain side: `CarveSphere` (kind 1, later 7); body side: `ObjectModel`, `EntitySpawn` / `EntityDespawn` (**networking stage 6**); crush: a damage event (M2). Before networking stage 6 impacts act on terrain only | 1-1.5 weeks | M3 / M5 (thrown matter) |
| 7 | Optional: `ShredSphere = 7`, `DetachFractured = 8` with golden tests; load-flow stress; settle-to-heap | `WorldOps.ApplyOpIsTheWorldCallOfEachKind` extended; `ShredSphere.*` and `ShredNoise.GoldenValues`; two-simulation hash tests | new kinds 7, 8 (after networking stage 3; `DetachFractured` after stage 6) | 1-2 weeks | after networking stage 3 / 6 |

## Rejected alternatives

| Rejected | Why |
|---|---|
| Relax the face rule (a face component counts as an island) | Rips out hillsides and trees; voxel.md:316-317 forbids it; the server would send huge wrong masks. |
| Grow the search box until no component touches the face | Cubic cost: a 192^3 box is est. 7 MB of ids + 28 MB of labels per blast; refused today above 192 per axis. |
| Raise `kMaxIslandVoxels` | It bounds per-cast cost and body size; big natural pieces are split by the structure stages instead. |
| Re-run the island search on clients | Needs identical, fully loaded voxels in the whole box (networking.md, rejected alternatives). |
| A flood that meets an unloaded chunk counts as attached (draft 1) | Makes the decision depend on load state (streaming offline, player positions in stage 9), and offline it brings the hanging piece back while the world streams. Replaced by the fixed flood box: loaded by construction on a server, deferred offline. |
| Flood reach without a fixed box (draft 1: only an extent limit) | The reach must be a function of the blast's integers so the server can guarantee it is loaded. |
| Cast queue on a wall-clock budget | Tick placement would follow machine speed; replaced by a work budget (R5). |
| Hold back a `Detach` until the client has built its body | Every later op that touches the same chunks would wait too; dust covers the gap instead. |
| Distance-scaled piece threshold in `Scene::Damage` (lod.md stage 1 as written) | It would make a server decision depend on a viewer; moved to presentation. |
| Object host first (collapse.md order) | The game's buildings are terrain; castles exceed 12.8 m; object pieces need networking stage 6. |
| Load-flow stress as the default rule | collapse.md's judgement stands: tuning risk, "buildings fall on spawn"; kept optional (stage 7). |
| Float impact energy deciding voxels | Jolt is not cross-platform deterministic; quantised once on the server instead (R3). |
| Impact filter by "bodies under the threshold do not exist on the server" | False offline (rubble shares the simulation); replaced by the ownership bit. |
| Crush damage from contact events | Characters are `CharacterVirtual` and never produce body contacts; a separate server sweep instead. |
| Every material same particles, fewer of them | Loses the "shred" read of a hit; profiles cost nothing extra on the wire and stay in fx caps. |

## Open questions for the owner (each with a recommendation)

1. **Stage 1 (with 1b-1e) before networking stage 3?** Recommendation: yes. About 5 days together, no format
   change, and the box rule is cheaper to change before the operation stream is encoded and tested over loopback.
2. **Server memory for the flood box**: a 3-chunk margin instead of 2 around `playBounds` (est. +10 % server
   memory). Recommendation: yes; the alternative (a flood box of only 2 chunks past the island box) frees fewer
   towers near the edge of the arena and in stage 9.
3. **Should big natural pieces and castle walls (over 60 000 voxels = 117 m^3 at 0.125 m) ever fall?**
   Recommendation: not in M1; counted (stage 1c). In M3 the structure stages split them into pieces under the cap.
4. **If stage 0 shows search + flood over 3 ms per cast, move stage 4 (cell graph) before networking stage 3?**
   Recommendation: yes; a 5v5 at 10 casts/s cannot afford a 4-14 ms search on the server tick or the offline frame.
5. **Do floors drop when a wall goes (overhang limits on) or only when fully cut (connectivity only)?**
   Recommendation: overhang limits on, inside registered structures only; natural terrain connectivity only.
6. **How long may a collapse take to unfold?** Recommendation: up to 2 s, released largest and lowest first;
   queued parts stay solid; at most 2 new server bodies per tick.
7. **How long does rubble stay?** Recommendation: 15 s small, 20 s medium, 30 s large, caps of 48 large bodies
   and 600 store slots; no stamping into heaps until a game rule needs it.
8. **Do falling pieces hurt players?** Recommendation: yes for awake server bodies over the replicated threshold
   (the crush the direction asks for, ROADMAP.md:61), by a server sweep against player capsules in M2 game logic;
   never for client rubble.
9. **Shatter vs dent thresholds and body caps**: Recommendation: start with the estimates above and set them from a
   measured `--collapse` fight scene on the UHD 620 (2 new server bodies per tick, 64 awake, 256 live).
10. **May derived impacts be dropped over budget** (unlike casts, which never are, fx-budget.md)? Recommendation:
    yes, after 30 ticks, counted and logged.
