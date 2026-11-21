# Destruction that finishes the job ("SHRED")

[[../../../00 Docs Map|Docs map]] · [[../../ROADMAP|roadmap]] · [[../../ARCHITECTURE|architecture]] · [[collapse|collapse (superseded parts)]] · [[networking|networking]] · [[fx-budget|fx budget]] · [[lod|LOD]] · [[../notes/voxel|voxel notes]] · [[../notes/scene-engine|scene/engine notes]]

## How others solve this optimally (web survey, 2026-11-21)

- Teardown is the shipped reference for arbitrary voxel destruction: connectivity search over voxel data plus rigid bodies for released chunks, no per-voxel stress analysis, deterministic voxel physics as the engine's core asset ([engine autopsy](https://devops-geek.net/nerd-space/dissecting-the-teardown-engine-a-technical-autopsy-of-voxel-destruction-pipelines/), [Tuxedo Labs developer on destruction + multiplayer](https://80.lv/articles/teardown-developer-breaks-down-multiplayer-and-voxel-destruction-tech)).
- A per-voxel "structural integrity" value recalculated on damage is the naive community variant ([Unity discussions thread](https://discussions.unity.com/t/want-to-recreate-teardown-destructable-voxel-physics/828648)); the cell-graph + max-flow load check here is more principled and cheaper per shot, and its integer-exactness is what a session server needs on top.
- Where server-authoritative collapse is done at all, the consensus cheap variant is: bake support data per prefab at build time, the server releases only cells / a few large bodies, clients add local debris — the same route as collapse.md M3's baked support graph and this design's piece budgets (2 per tick, 64 awake).

Status: **stages 0, 1, 1b, 1c, 1d and 1e built and reviewed** (2026-11-12, see "Stage 0-1c as built", "Stages 1d-1e
as built" and "Review of stages 0-1e" at the end of Part 1); the owner took the recommended answers to open questions 1, 2, 3 and 7 ("Yes, build
1-1e"). **Stage 4 built** (2026-11-12, owner's answer to question 4: yes, before networking stage 3; see "Stage 4 as
built" in Part 4; not yet reviewed by a second reader). **Stage 4b, weak necks (load check), designed and judged**
(2026-11-12, Part 4b; the owner: "Yes, weak necks next"). Networking stage 3 (2026-11-13) built the cast work budget
(R5), hashed the material table into `buildHash` (R6) and lowered the flow budget (open question 7). The rest is a draft. Written read-only against the working tree of 2026-11-12, while the
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
   `netId 0` `Detach`. **Owner decision (2026-11-12): thin necks must break under the weight above them, now** -
   stage 4b (Part 4b), an integer max-flow capacity check on the stage-4 graph, run only after a cast.
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
| Cell format "more than 8 components merge into node 7" (collapse.md:74-75) | **Replaced in stage 4** (as built): every component of a cell is its own node in a per-chunk node list, so the graph equals voxel connectivity everywhere, over-full cells included (counted, `Structure.GraphEqualsLabelComponentsInOverfullCells`). |
| Rejected "CSR node/edge graph with bond areas, masses and a load check (B)" (collapse.md:28-31) | **Its vertical part is stage 4b** (Part 4b): masses and bonds on the stage-4 graph, a max-flow capacity check that runs only after a cast, so pristine buildings never fall on spawn. A stress / bending model stays optional (stage 7). |

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
  *Built in networking stage 3 (2026-11-13): `sim::CastWorkOf` charges node visits, `chunkGraphs` at the cold price,
  the load check's formula above and the operations' boxes (never `graphsBuilt`, `chunksDecoded` or the object half);
  `kCastWorkPerTick` = 60 000 units, calibrated to 1.09 ms per 10 000 units on the reference machine; the event byte
  budget is a credit of its own. `CastBudget.ScheduleFollowsCountsNotTimeOrCache` replays a server with 1 and 3
  workers and an emptied graph cache: same ticks, same operations.*
- **R6 One integer material table** (`MaterialPhysicsQ`, stage 3) hashed into `Hello::buildHash` (networking.md:88).
  This also makes explicit what `CarvableIds` already relies on (float hardness read on both sides,
  `World.cpp:1547-1560`).
  *Networking stage 3 (2026-11-13) hashes the inputs instead: `voxel::MaterialTableHash` (raw bits of `hardness` and
  `density`, `flags`, `strength[3]` per world-palette id) is part of `buildHash`, so a strength-table drift refuses
  the session. The integer `MaterialPhysicsQ` table itself is not built.*
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
  `Material.cpp:143, 152`). Test `MaterialQ.SaturatesAndKeepsIndestructible`. Bond strengths (`Material::strength`,
  integer kg/m^2 for compression, shear, tension) come with stage 4b (Part 4b) and join this table.
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
  voxels; no new server margin). *(Design; superseded by "Stage 4 as built": no merge, every component is a node, so
  the rest of this item and the next describe what was not built.)* Graph connectivity equals voxel 6-connectivity **except in cells with more than 8
  components**, which merge into node 7 and over-connect (collapse.md:74-75). Cells over 8 components are resolved
  by the exact per-voxel search for those cells (decided in stage 4 from a measured count; fallback: keep the merge
  and accept "the graph never detaches what the voxel search keeps"). A per-search counter `overfullCells` is
  logged. Only released nodes are re-labelled per voxel to build `Detach` masks.
- *(Design, superseded.)* Switching from the voxel search to the graph **can change server results** where cells are
  over-full. As built it cannot: over-full cells are exact, and results equal stage 1 except where a work budget runs
  out (networking.md "Changed by shred.md stage 4"; it is a server decision, clients only apply).
- **Overhang rule inside registered structures** (stage 5). `sim::StructureRegistry` holds integer voxel boxes:
  the playground buildings now, prefab placements in M3/M4. Inside a box: collapse.md's 0-1-2 bucketed search
  (up 0, side 1, hang 2; collapse.md:94-97) from anchors = solid nodes in the ring of cells outside the box (the
  natural ground) and indestructible nodes; a node falls if unreached or past its material's limit (stone 3, wood 6,
  iron 10 cells). **Outside structures: connectivity only**, so cliffs never fail by themselves.
- **No-fall-on-spawn guard**: registering a structure runs the solve once; nodes that would fall are reported and
  pinned (a per-cell flag), as collapse.md:127-128 asks.

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

### Optional later: load flow (stage 7, owner decision) - resolved by stage 4b

The owner decided (2026-11-12) that thin necks must break now. The per-node mass, per-face bond area and integer
compression / shear / tension capacities per material are built by **stage 4b** (Part 4b) - not as a load flow over a
support-distance DAG (judged there: its load split is a heuristic that can fail rock near a crater) but as a
max-flow check whose released set is exact and unique. What stays optional for stage 7, only if play shows the need:
a bending moment and a tip check of the centre of mass per layer (cantilevers, half-cut beams, leaning towers), necks
that narrow sideways inside one cell, and the spawn guard's "reinforce and report" of overloaded parts (stage 4b's
open question 6: an editor neck report). Re-checking structures away from a cast is stage 5's dirty boxes calling the
stage-4b check.

## Part 4b - Weak necks (load check)

Status: **built and reviewed 2026-11-13 ("Stage 4b as built" and "Review of stage 4b" at the end of this part); design judged 2026-11-12.** The owner asked, with a screenshot, that a whole watch tower
standing on a 4-voxel stone neck must not hold: a thin neck must break under the weight above it. Owner decisions:
build it now, before networking stage 3 ("Yes, weak necks next"); rock stays strong enough that natural cliffs and
overhangs do not fall by themselves; whole castles may collapse. Three designs were written independently (a
horizontal cross-section sweep, a load-flow DAG, a bounded min-cut); this part is the judged result. Line numbers are
from the working tree of 2026-11-12; every time marked **(e)** is an estimate.

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

### Units and the integer formula

- **Mass** is `GraphNode::mass` (Structure.cppm:47): the sum of `densityQ` (kg/m^3, integer) over the node's
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
  faces. `bond` is `GraphLink::bond` inside a chunk, `BondVoxels` across (Structure.cppm:59-64, 85-87). A node's
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

`GraphNode` (Structure.cppm:43-55) uses 67 of its 72 bytes. Five more, fields reordered so it stays **72 bytes**
(`static_assert(sizeof(GraphNode) == 72)`): `cutFaces` (u8), `cutPlane` (u8), `weak[3]` (u8 each). Layout: `face[6]`,
`mass`, `voxels`, `firstVoxel`, `firstLink`, `linkCount`, `cutFaces`, `cutPlane`, `cell`, `boxMin[3]`, `boxMax[3]`,
`weak[3]`.

- **`weak[k]`**: the id with the lowest `strength[k]` among the node's voxels, ties to the smaller id. A 4^3 block of
  one id costs nothing (the `BlockSolid` fast path, Structure.cpp:116-120); mixed blocks fold it into the per-voxel
  loop that already sums the mass (`NodeMass`, Structure.cpp:239-248). The shared full Uniform graph gets
  `weak = uniformId` (Structure.cpp:304-321).
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
- **`GraphMaterials`** (Structure.cppm:91-98) gains `strength[3][256]` (uint32, kg/m^2) and `anchor[256]`;
  `densityQ` is 0 for `MaterialFlag_Foliage`. They are part of `operator==`, so a table change drops the cache
  (Structure.cpp:410-415), as today. Without a library every id gets stone's strengths and no anchor.
- **Build cost** (e): +2-6 us per Dense chunk on today's 25-33 us; measured by `Structure.DISABLED_GraphBuildCost`.
  Memory unchanged.

### Material strengths (`Material::strength[3]`, kg/m^2, integers; `RegisterDefaultWorldMaterials`, Material.cpp:136-155)

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

Stone's C is the owner's knob (open question 1): it sets where the tower falls. Natural rock keeps a margin of 3x or
more at it (next table); soil less, by design (open question 5). Stage 3's `MaterialPhysicsQ` takes these integers
over and hashes them into `buildHash` (R6); until then they are compiled-in defaults, the same on both sides of a
session.

### The owner's cases, with the arithmetic (0.125 m unless noted)

The watch tower (`src/main.cpp:727-737`, `S(x) = lround(0.8 x)`), recomputed voxel for voxel: a stone shell 22 x 56
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
| Castle curtain wall 10 m, 0.5 m thick (stone, per face) | 80 x 2600 = 208 000 | 800 000 | stands 0.26; the castle is over 60 000 voxels anyway, so it is an anchor in 4b (open question 2) |
| Test arena mushrooms (`tests/sim/BlastTests.cpp:81-92`, 0.1 m): stone cap and stem / planks cap and stone stem, 9 stem faces | 1 318 200 / 592 200 | 9 000 000 (4 500 000 if the floor were soil) | stand: `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` keeps its operations |

`--tower-blast` (7 shots round the ring, `src/main.cpp:907-982`) may now free the tower one or two shots early, once
the ring's remaining section is under about 23 faces: its acceptance becomes "freed by shot K <= 7, by an island or
by the load check (the log says which)".

### The check, step by step

Phase **3b** of `voxel::SearchOnce` (Structure.cpp:718): after the island floods (step 3, ends at :969) and before
the re-labelling (step 4, :971). It runs only when `limits.neck != nullptr`, the island box was searched and the
face floods are complete (`result.searched && result.complete`). It uses the same `GraphTable` (Structure.cpp:543-659:
chunks resolved once, `chunkGraphs` counted once, `ForEachLink` order -x +x -y +y -z +z then link order) and reads
`table.state` only for `kIsland`. Island nodes (in-box and face, crumbs included) are not in the network.

1. **R (judged nodes).** The seeds of step 1 (nodes whose bounds meet the island box, chunks z, y, x, then graph
   order; Structure.cpp:770-794) that are not `kIsland`, in that order. A node with `cutFaces != 255` becomes two
   network nodes, lower then upper. Network index = this order. If R and its loads (step 2) exceed
   `kNeckMaxNetworkNodes` (8192) network nodes, or the arcs (step 3) 65 535 pairs, the check is skipped and nothing
   is released (`neckSkipped`, a warning).
2. **Exterior floods.** For each R node in order and each of its links, in `ForEachLink` order, to a node that is
   neither in R nor `kIsland` nor labelled yet: a FIFO flood over such nodes, inside the flood box. It ends as an
   **anchor** on a node with a voxel on the flood box face layer or the window's bottom row (the face flood's test,
   Structure.cpp:919-927), on an anchor-material node, on more than `kMaxIslandVoxels` voxels, on a node an earlier
   flood labelled anchor, or once `kNeckExteriorVisitBudget` (16 384 node visits per check) is spent (that flood and
   every later one: `neckExteriorBudget`, conservative). **As built (review):** also on entering a chunk solid
   through (32^3 solid voxels, whatever its storage; asked before the chunk is resolved, `neckSolidChunks`).
   Otherwise it closes: a **load** (members, mass, voxels). All its visited nodes get its label. A chunk that is not
   loaded is **support** for this check, which can only make it release less than the full answer (adding support
   only shrinks the largest overloaded set): on a server `neckUnloaded` counts an error (the flood box is loaded by
   construction, `kServerWindowMarginChunks`); offline it is counted (`neckLoading` inside the window,
   `neckBeyondWindow` outside) and `sim::Blast` queues a late search that runs island search and check again once that
   chunk has loaded (`neckDeferred`; a chunk outside the window loads once the window moved over it). **The islands
   are not held back** (the design held the face islands with the check; the review found that slowed them while
   chunks stream). The
   per-slot label array lives in `GraphTable`, resized with `state` / `seed` whenever a flood resolves a new chunk
   (trap: floods resolve chunks, Structure.cpp:589-591). Loads get network indices after R, in the order they close.
3. **Network.** For each network node in index order, its links in `ForEachLink` order, each pair once (the twin
   link is skipped by index):
   - R-R: one arc pair, capacities by direction class and `weak` (above). For a split node a -y link belongs to the
     lower half, a +y link to the upper half, and an x or z link is split by its overlap bits
     `a.face[f] & b.face[f ^ 1]` (x face: y = bit & 7; z face: y = bit >> 3): bits at y <= k* go to the lower half,
     the rest to the upper half, each part its own arc pair with its own popcount. Two split nodes side by side are
     split on both ends the same way. Inside a split node: upper -> lower `cutFaces x capFace[C][weak[C]]`, lower ->
     upper `cutFaces x capFace[T][weak[T]]`.
   - R-load: capacities summed per (R node, load), in first-encounter order. R-anchor: summed into one arc R -> t.
   - R nodes with the anchor bit or a voxel on the window's bottom row: an infinite arc to t.
   - Supplies: node mass; a split node's halves get the exact mass of their voxels (`NodeVoxels`,
     Structure.cpp:374-386, through the search's `VoxelSource`, one table lookup per voxel; counted in
     `neckSplitRelabels`); a load its total.
4. **Maximum preflow** (push-relabel, all integer, fixed order): exact initial labels by a reverse BFS from t over
   arcs with residual capacity; nodes that cannot reach t get label n. Active nodes first in label order descending,
   ties by index (the "gravity sweep": in a structure that holds, all mass flows down in this one pass), then FIFO,
   current-arc pointers in arc order, a global relabel every `max(64, n / 4)` relabels (a count, not a time). Stop
   when no active node has a label below n. Work = arc scans (discharges and BFS) against `kNeckFlowWorkBudget`
   (262 144 as built in stage 4b; **65 536** since networking stage 3, open question 7); if it is spent, nothing is
   released (`neckFlowBudget`, a warning).
5. **Released set and pieces.** If all supply reached t: done. Otherwise a reverse BFS from t over residual arcs; A =
   the nodes it does not reach. **Pieces** = connected parts of A over network links (loads expand to their member
   nodes), ordered by their smallest network index. Per piece: voxels, load (MU), cut (sum of the real capacities of
   its outgoing arcs) and cut faces, for the log and the tests. A piece over `kMaxIslandVoxels` stays (counted
   `neckOversize`; castle scale, open question 2). **As built (review):** at most `kNeckMaxReleasedParts` (64) parts
   and `kNeckMaxReleasedVoxels` (60 000) voxels are released per check, parts in piece order while both fit; the
   first that does not and every later one stay (`neckDeferredParts`). Exact and order-free: parts of A have no
   network arc between them, so each one left is still overloaded on its own (released by a later cast near it), and
   the kept side holds with them in place (the preflow routed their cut capacity through it).
6. **Voxels.** Released nodes are re-labelled with `NodeVoxels` (an upper half keeps its layers > k*) and the stage-4
   stale-graph check (count, face masks, bounds; Structure.cpp:1007-1022): a mismatch counts `relabelMismatch` and
   the whole search runs again on fresh graphs (Structure.cpp:1103-1125). Pieces go into a new
   `GraphSearchResult::neckPieces` (`std::vector<FloatingIsland>`, plus `std::vector<NeckCut>` with load, cut, faces);
   they bypass the island step's "no voxel in the island box" drop (Structure.cpp:1049-1052) and the face ordering
   (:1065-1093).
7. **Operations.** `sim::SearchIslands` (Blast.cpp:230-357) emits, in this order: carve; in-box islands (scan
   order); face islands (candidate order); **neck pieces (piece order)**; one `ClearMask` for every crumb of the three
   lists. A neck piece of at least `MinPieceVoxels(vs)` is `WorldOp::Detach(origin, mask, BlastPush(centre, radius,
   corner))`, `netId 0`, applied at once like an island (Blast.cpp:326-331); a smaller one is a crumb. A neck piece
   touches an R node (or is one) - **a released part may be a load alone** (review: sand hanging in tension under a
   ceiling that holds, `Neck.HangingLoadIsReleasedAloneItsNodeStays`) - and an R node's voxels lie at most 7 voxels
   outside the island box, so a crumb reaches past the island box by less than `MinPieceVoxels + 7` and the
   `ClearMask` box stays under `192 + 2 x (MinPieceVoxels + 7)` voxels per axis (networking.md "Crumbs", corrected).
   `PresentBlast` makes rubble from it as from any island (stage 1e lifetimes and caps).

### When it runs, and when it does not

- After every `sim::Blast` with the graph search, duds included (a power-0 blast is how the tests and the pristine
  check probe a structure), and in every late search (`RetryIslandSearches`, Blast.cpp:566).
- Never after editor or game `FillBox`, brush edits, generation, loading, or a client's `ApplyOp` (R1). So a
  structure is never judged without a cast near it: the answer to collapse.md's "buildings fall on spawn"
  (collapse.md:28-31), and why the owner's screenshot (a neck cut by `FillBox`) stood until a shot landed near it.
- Not after its own release: the remainder is proven to hold.
- `SimulationConfig::neckRule` (default true; Simulation.cppm:31-69); off with `IslandSearchKind::Voxel` (the
  reference has no load check). `checkIslandSearch` / `SameResult` (Blast.cpp:203-222) keep comparing islands only.
- Later, stage 5's `StructuralSolver::MarkDirty` calls the same phase on dirty boxes under its tick budget, and stage
  6's impact carves call it like a blast.

### Determinism and replication

- Inputs: the voxels of the flood box (through the graphs, pure functions of the chunks), the material table, the
  session's `vsMicro`. Nothing outside the flood box is read (anchors stop at its face layer).
- Every order is fixed: R by seed order, loads by closing order, arcs by `ForEachLink` order, the preflow by labels
  and indices, pieces by smallest index. The released set does not depend on any of these orders (step 5), which
  `Neck.ReleasedSetIndependentOfArcOrder` pins; the work counters do depend on them, and they are fixed.
- Clients only apply the `Detach` / `ClearMask` the server sends; no `WorldOpKind`, `Detach`, `ClearMask` or wire
  change. The search result for the same input changes (more pieces fall): a server decision, as stage 1 and 4.

### Work bound per cast (R5) and cost

New constants next to `kGraphNodeVisitBudget` (`sim/Blast.cppm:177`): `kNeckMaxNetworkNodes` = 8192,
`kNeckMaxArcPairs` = 65 535, `kNeckExteriorVisitBudget` = 16 384 node visits, `kNeckFlowWorkBudget` = 262 144 arc
scans (65 536 since networking stage 3, open question 7); since the review also `kNeckMaxReleasedVoxels` = 60 000 and `kNeckMaxReleasedParts` = 64 per check (what the
re-label, the `Detach` ops and the wire may carry from one cast). Counters (`IslandSearchStats`, the `Blast:` line): `neckNodes`, `neckSplit`, `neckLoads`, `neckAnchors`,
`neckArcs`, `neckExteriorVisits`, `neckFlowWork`, `neckSplitRelabels`, `neckPieces`, `neckCrumbs`, `neckVoxels`,
`neckOversize`, `neckSkipped`, `neckExteriorBudget`, `neckFlowBudget`, `neckUnloaded`; `neckMs` logged only. All of
them follow from the flood box voxels: equal cold or warm, with 1 or 3 workers. The cast work budget (networking.md:94)
adds `neckExteriorVisits + neckArcs + neckFlowWork / 8 + 64 x neckSplitRelabels + neckVoxels / 64` (weights
calibrated once, e); graphs the exterior floods resolve are already in `chunkGraphs` at the cold price.

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
- **Offline (today's game)**: +0.05-0.6 ms on a blast frame; a typical cast stays inside the 3 ms rule, the worst
  does not (as cold graph builds today).
- Memory: `thread_local` network arrays, about 1-2 MB at the node cap; nothing new is cached.

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

### Headless `--neck-blast [W]` (game, next to `UpdateTowerBlast`, `src/main.cpp:907-982`)

1. **Frame 20, pristine:** six duds (`BlastDesc{p, 1.2f, 0.0f}`) at the tower base, both brick walls, the timber
   frame, the crystals and the metal blocks. Must log `Neck: pristine playground: 6 duds, 0 pieces released`.
2. **Frame 30, the owner's screenshot:** `World::FillBox` turns the tower ring's rows 1 .. lround(1.5 m / vs) to air
   except a stone segment 2 x W (default W = 2) at the north-west corner inside the wall line. No check runs for
   `FillBox`: must log `Neck: tower on a 2 x W stone neck (2W faces), roof solid`.
3. **Frame 40, the shot:** 0.6 m, power 6, at the centre of the tower's hollow 0.5 m above the floor (it carves only
   paving; the neck is about 1.9 m away, inside the island box). Must log
   `Neck: tower released: 1 piece of N voxels, load L MU (x t) > cut C MU (F faces, inside a cell | on a cell face);
   network n nodes (s split), l loads, a arcs, v exterior visits, w flow work, X ms; roof AIR`, the piece's speed at
   +1 and +5 frames (as `--tower-blast`) and `store a/b`.

Acceptance (validation on, exit 0): W = 2 at 0.125 m releases (8574 voxels at an aligned floor, at least 8526 at any,
ratio 5.62); `--neck-blast 11` releases (1.05x); `--neck-blast 12` logs `Neck: tower stands, roof solid`;
`--voxel-size 0.1` and `0.15` release with W = 2 (10.4x, 4.70x). Unchanged: the standard and editor runs, `--climb`
(its blast releases nothing), `--probe-blast` (`0 floating island(s)`), `--castle-blast` (neck counters logged, 0
pieces: the castle is an anchor), `--tower-blast` (freed by shot K <= 7, says how). Not confirmable headless, for the
owner: the fall seen in a window, its rubble, and the cost on a busy UHD 620.

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
   fell at 10 000). Sand and snow still slump in shear and tension near a crater (intended, open question 5); the rock
   gate samples a beach, a snow field, a rock slope and deep caves and releases nothing there. Exact per-face
   material bonds stay rejected (voxel reads per link across chunks on every search).
3. **No bending moment, no tipping.** Long cantilevers, a beam cut at one end and slender towers do not fail by
   moment; a tower leaning over its neck holds if the neck has the faces. Stage 7 (moment / tip check) or stage 5's
   overhang rule inside registered structures.
4. **Locality.** Only the island box is judged. A neck formed by earlier shots is judged when a cast lands within the
   island box of it (about 4 m for a 1.2 m blast at 0.125 m); a shot at the roof does not re-check a neck 5 m below
   (the tower above the roof shot is part of an anchor through the neck). Stage 5's dirty boxes call the same check.
5. **Exterior anchors are trusted**: anything that reaches the flood box face is ground, even if it hangs by a thread
   beyond it, and parts over 60 000 voxels are anchors - castles do not fall by this check in 4b (open question 2).
   Since the review a chunk solid through (32^3 solid voxels) is ground too: a chunk-aligned 4 m solid cube hanging
   on a neck outside the island box is held (`Neck.SolidChunkIsGround`; one voxel off the grid it falls).
6. **Pre-existing overloads are shaken loose** by any cast nearby, duds included (an editor-built structure that was
   too weak from the start falls at the first shot near its weak point). Counted in `neckPieces`; the pristine
   playground and terrain tests pin zero.
7. **Spent budgets release nothing** that cast (counted, warned); until stage 5 can re-queue the box, the structure
   stays until a later cast nearby. Not lost, but late (R7 in its weakest form). The same for parts over the release
   caps (64 parts / 60 000 voxels per cast, `neckDeferredParts`).
8. **Stale graphs**: released and split nodes are re-checked and the search retried, as for islands - since the review
   on everything the check reads (voxel count, face masks, bounds, mass, weakest materials, anchor bit, plane);
   kept judged nodes, loads and anchors are not re-read: for them `World::ChunkRevision` is the only guard.
9. **Foliage is weightless**: a canopy never overloads a trunk; leaves still carry (and can fail under) what rests
   on them.
10. **Strength-table drift** between a server and its replays changes decisions: the table must be in `buildHash`
    (R6, stage 3). Clients are not affected (they only apply).

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

### Open questions for the owner (each with a recommendation)

1. **Where does the tower fall?** With stone C = 100 000 kg/m^2 the watch tower (35 t above its neck) falls on any
   stone neck under 24 faces (0.375 m^2, a sixth of its base) and stands on 24 or more; natural rock keeps a 3x
   margin or more. **Recommendation: keep it and tune by playing** (one integer per material; at 150 000 the tower
   holds from 16 faces, at 70 000 only from 34).
2. **Whole castles may collapse - when?** In 4b anything over 60 000 voxels is an anchor, so a castle never falls by
   this check (a corner tower, about 45 000 voxels, could once the walls beside it are cut away).
   **Recommendation: a stage 4c right after 4b is measured**: raise the exterior load limit to 4 x 60 000, split any
   release over 60 000 voxels (and oversize islands) into chunk-aligned 32^3 bricks, each its own `Detach`, at most
   240 000 released voxels per cast (the rest a later cast), wire bursts measured; stage 5 then spreads big collapses
   over ticks.
3. **A shot at the roof does not re-check a neck far below** (failure mode 4). **Recommendation: accept** until stage
   5's dirty boxes; players cut necks with shots near them.
4. **Foliage weightless?** **Recommendation: yes** - generated trees have 1-voxel trunks under canopies of up to
   26 800 leaves and would otherwise all fail.
5. **Soft ground slumps**: sand overhangs over 1.9 m and dirt over 5.3 m fall when a cast lands near them.
   **Recommendation: yes**, it reads as "shredding" soft ground; rock is unaffected.
6. **Weak player-built structures** stand until a shot lands near them, then fall. **Recommendation: yes, plus an
   editor "neck report"** later (run the check on a box and list overloaded parts with their ratio; collapse.md's
   spawn guard as a report, never a release). The review measured the case: the watch tower FillBoxed onto a game
   slope **without its foundation** rests on about 25 faces of turf (10 000 000 MU against 22 932 000) and falls on the
   first dud near it; the game's playground, built on its foundation, releases nothing under the six `--neck-blast`
   duds.
7. **(Review) The flow budget allows a cast far over the cast phase.** The preflow takes 5-6 ms at about 207 000 of
   its 262 144 arc scans; every typical cast needs 2 600-13 000 (the tower's release 7 000-10 700). **Recommendation:**
   lower `kNeckFlowWorkBudget` to 65 536 (about 1.5 ms, 5x the largest measured need; spent means "released next
   cast", R7) when stage 3's cast budget is built, and charge `neckFlowWork` there; not changed now because it changes
   which casts release (only in networks far larger than any measured).
   *Done with networking stage 3 (2026-11-13): `kNeckFlowWorkBudget` = 65 536, and the cast work budget charges
   `neckFlowWork` (2 x `neckFlowWork` / 8 units of about 0.1 us, networking.md "Stage 3 as built"). Every test and the
   `--neck-blast` / `--tower-blast` runs are unchanged (the game's neck release needs 7 055).*

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
| Graph connectivity + face flood on nodes (stage 4) | **Measured** (quiet, -O2): 0.13-1.00 ms per cast on game terrain, 0.06-0.72 ms on flat ground, cold graph builds included; in game after the window filled 0.24-0.75 ms at a cached site, 0.5-3.7 ms at the first blast of an untouched site; while streaming 4.0-4.75 ms (`--climb`). Cold worst case unbounded by the node budget: island-box chunks (up to 64) x 70-120 us in game = 4.5-7.7 ms, so stage 3 charges `chunkGraphs` x a cold price |
| Weak-neck check (stage 4b, per cast) | **Measured** (quiet, -O2, on the same world as the search without it, "Review of stage 4b"): 0.13-0.46 ms at a cached site on flat ground, 0.30-0.73 ms on game terrain, plus 0.0-1.8 ms of graph builds at a cold site (chunks next to the island box, which the judged nodes' bonds need). **At its budgets far over the design's 3-4 ms:** the preflow alone takes 5-6 ms at about 207 000 of its 262 144 arc scans (8192 nodes, 65 535 arc pairs), a forest of 529 pillars (3574 nodes, 3963 exterior visits) 2.0-2.4 ms; est. 8-12 ms with every budget full. Releases at most 64 parts / 60 000 voxels per cast. Counted work ("Stage 4b as built"); stage 3 must charge it or the flow budget come down (open question 7). Done in networking stage 3: both (the cast work budget charges it, the flow budget is 65 536 arc scans, est. 1.5 ms at the budget) |
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
| 4 | **Built 2026-11-12** ("Stage 4 as built"). Cell graph cached per terrain chunk (liquid-masked); graph connectivity + node face flood replace the voxel search in the same flood box | `Structure.GraphNeverDetachesWhatTheVoxelSearchKeeps` (random volumes, liquids included), `Structure.GraphEqualsLabelComponentsWithoutOverfullCells`, `RefreshEqualsRebuild`, `GraphFloatingEqualsFaceFlood` on every stage-1 test; `overfullCells` logged; island search <= 0.5 ms optimised at the probe blast | same op kinds; results equal stage 1 except where a work budget runs out, and offline deferral / the attach reason when a flood box chunk is not loaded (over-full cells are exact: "Stage 4 as built"; networking.md notes it) | 4-6 days | M3, **or before networking stage 3 if stage 0 shows search + flood over 3 ms** |
| 4b | **Built 2026-11-13** ("Stage 4b as built"; review pending). **Weak necks (load check), Part 4b**: integer strengths per material (C / S / T), node plane cut + weakest materials + anchor bit in the cell graph, max-flow check in the island box after the island floods, largest strictly overloaded set released; `--neck-blast` | Part 4b's tests (brute-force max-flow, voxel reference, tower at every height mod 8, thresholds exact, `Neck.NaturalTerrainNeverFails`, budgets equal cold / warm and 1 / 3 workers); `--neck-blast` W = 2 and 11 release, W = 12 stands (0.125 m), W = 2 releases at 0.1 and 0.15 m; all headless runs exit 0; `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` unchanged | `Detach` (`netId 0`) and `ClearMask` only; search result changes (more pieces); strengths into `buildHash` with stage 3 | 4-6 days | now (owner), before networking stage 3 |
| 5 | `StructureRegistry`, overhang rule inside structures, `StructuralSolver` with tick budgets and release queue, spawn guard, `OpBatch` / `PresentOps`, client queued-piece handling, headless `--collapse` | `Structure.StoneOverhang3HoldsAnd4Falls`, `PristineStructureStable`, `SolveIndependentOfDirtyOrder`; `Collapse.TowerReleasedOverTicksWithinBudget`, `Collapse.SameOpsTwoSimulationsEqualHash`, `Collapse.ReplayGivesIdenticalOpList` (casts fed at fixed ticks); `--collapse` logs `Collapse: N releases over T ticks, max B bodies/tick, X ms max`, physics step p99, `store a/b` under 7000, validation exit 0 | `Detach` with `netId 0` and `ClearMask` only. **Server bodies (`Detach` with `netId != 0`, `EntitySpawn`, `EntityRest`) need networking stage 6** (networking.md:295); before it every release is a `netId 0` Detach | 1.5-2 weeks | M3 |
| 5b | collapse.md stages 3-4 on objects (same graph code) | collapse.md's own criteria | `ObjectModel`, `EntitySpawn` / `EntityDespawn` (networking stage 6) | 1 week | M3 |
| 6 | Contacts drained and extended, `ColliderMaterials`, ownership bit, `sim::Impact` with total order, cooldowns and budgets; crush check | `ContactQueueNeverFillsWhenSimulationDrains` (10k ticks), `Impact.HeavyDropCarvesLightDoesNot`, `CooldownAndFatigue`, `BudgetCapsBodiesPerTick`, `ShuffledEventsSameOps` (shuffles include several events with an equal netId pair across two terrain chunks), `ClientRubbleNeverCausesImpacts` on the offline **and** the server config, `Crush.FallingServerBodyDamagesPlayer`, `Crush.RubbleNeverDamages` | terrain side: `CarveSphere` (kind 1, later 7); body side: `ObjectModel`, `EntitySpawn` / `EntityDespawn` (**networking stage 6**); crush: a damage event (M2). Before networking stage 6 impacts act on terrain only | 1-1.5 weeks | M3 / M5 (thrown matter) |
| 7 | Optional: `ShredSphere = 7`, `DetachFractured = 8` with golden tests; bending / tip check on top of stage 4b; settle-to-heap | `WorldOps.ApplyOpIsTheWorldCallOfEachKind` extended; `ShredSphere.*` and `ShredNoise.GoldenValues`; two-simulation hash tests | new kinds 7, 8 (after networking stage 3; `DetachFractured` after stage 6) | 1-2 weeks | after networking stage 3 / 6 |

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
| Load-flow stress as the default rule | collapse.md's judgement stands: tuning risk, "buildings fall on spawn"; kept optional (stage 7). Weak necks are stage 4b's max-flow check instead, run only after a cast (Part 4b, "Rejected within this part"). |
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
   **Answered yes (2026-11-12); stage 4 is built.** New for the owner: build chunk graphs ahead of the first blast
   (on a worker when a chunk near the player loads or changes) so the first blast at a site also stays under 0.5 ms?
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
11. **Stage 4b (weak necks)**: its six questions (where the tower falls, castle bricks as stage 4c, locality,
    weightless foliage, soft ground slumping, weak player-built structures) are at the end of Part 4b.
