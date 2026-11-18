# Shred history: sections changed by the 2026-11-18 rewrite, as they were

Moved verbatim from `docs/design/shred.md` lines 2-32, 33-63, 64-76, 126-203, 218-233, 458-493, 494-543, 544-574, 694-733, 745-759, 811-835, 858-889, 890-918, 919-956, 957-1031, 1032-1044, 1055-1081, 1150-1169, 1170-1209, 1441-1486, 1487-1508, 1509-1522, 1562-1574, 2057-2076 and 2077-2104 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

Each range is a whole section (or the old header) that stays in `docs/design/shred.md` in an edited form: line
references replaced by symbols and section names, status words updated, as-built facts merged. Lines 2-32 are
the old header (status, revision 2, relation to other designs).

---

<!-- source lines 2-32 -->


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

<!-- source lines 33-63 -->

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

<!-- source lines 64-76 -->

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

<!-- source lines 126-203 -->

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

<!-- source lines 218-233 -->

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

<!-- source lines 458-493 -->

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

<!-- source lines 494-543 -->

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

<!-- source lines 544-574 -->

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

<!-- source lines 694-733 -->

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

<!-- source lines 745-759 -->

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

<!-- source lines 811-835 -->

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

<!-- source lines 858-889 -->

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

<!-- source lines 890-918 -->

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

<!-- source lines 919-956 -->

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

<!-- source lines 957-1031 -->

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

<!-- source lines 1032-1044 -->

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

<!-- source lines 1055-1081 -->

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

<!-- source lines 1150-1169 -->

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

<!-- source lines 1170-1209 -->

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

<!-- source lines 1441-1486 -->

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

<!-- source lines 1487-1508 -->

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

<!-- source lines 1509-1522 -->

## Structural integrity plan (2026-11-17): stages 5, 5b, 6 against the code after networking 7b

Status: **plan, nothing built; amended the same day after a critic's review** (15 findings, all checked against the
code: 15 accepted, 5 of them with a changed or extended fix - see "Review of the plan"). The owner asked (2026-11-17): "Implement a
Structural Integrity System to check if object connections can break under their own weight and under inertial
contact." This section re-plans stages 5, 5b and 6 against the code as it is after networking stages 6-7b (sessions
with server bodies, NetIds, the entity byte budget, chunk sync and checks) and replaces rows 5, 5b and 6 of "Stages"
and the unbuilt parts of Part 4 ("Host and scope": the registry and overhang items; "Propagation over ticks") and of
Part 5 wherever it says so. Row 7 (bending, `ShredSphere`, `DetachFractured`) stays optional; collapse.md stage 1
(static objects) stays at ROADMAP M3. The fog half of the same message is not part of this plan. Written read-only;
line numbers are quoted only where the review checked them (networking stage 8 is being built in the same tree: they
may move). Every time or size marked **(e)** is an estimate. Binding as for every stage: Part 2 (R1-R7), fun and few
bugs over realism, the UHD 620 frame, tests first, GPU-free logic.

<!-- source lines 1562-1574 -->

### What changed since Parts 4 and 5 were written

| Then | Now (code after networking 7b) | Consequence for this plan |
|---|---|---|
| "Server bodies need networking stage 6"; every release a `netId` 0 `Detach` | A `Detach` of at least `replicatedPieceVoxels` (0.512 m^3: 262 voxels at 0.125 m) that gets `TakePieceSlot` carries a NetId; the server makes its body after the push (`sim::Blast` step 5) | A neck-released tower is a server body in a session and can strike the ground (SI-2). No new path for terrain releases |
| `CollapseBudget{releasesPerTick 6, serverBodiesPerTick 2, ...}` | `TakePieceSlot`: 2 bodies per tick, 64 awake replicated bodies (crates included), 256 live pieces, none while the entity byte budget is spent; largest pieces of a cast ask first | `CollapseBudget` is dropped: every new piece of SI-1/2/3 asks `TakePieceSlot`, over budget it is client rubble (a fragment / `netId` 0) |
| Entity events counted against the event budget | Entity events have their own credit (200 B/tick = 12 kB/s, burst 16 KiB) that never moves a cast; `ObjectModel` resends a whole model (`WriteVolumeRuns`); `EmitObjectEvents` lets a damaged object go `ToRubble` (6 B) once the credit is at or below 0 | Client rubble is `Layer::Rubble`, which session characters never meet (`kSessionCharacterLayers` = Static \| Dynamic): a `ToRubble` makes a landed body a walk-through ghost. **SI-2 never causes one**: an impact `Damage` / shatter needs `entityCredit` >= the model's encoded size (its runs counted, no allocation), else it is skipped and counted. SI-1's survivor is an ordinary `ObjectModel` and inherits the `ToRubble` degradation, as any cast's damage does |
| Client copies placeholder; "dust until the body exists" | Copies are kinematic; moving replicated bodies are `Layer::Rubble` on both sides (they pass through players); `characterMovesBodies` false; the client builds every copy when its event applies | Crush stays out (a moving body never touches a player; there is no health yet: M2). Contacts on a client are drained and dropped |
| Join by event log | Join by chunk sync (7a); every server terrain op must go through `Server::Impl::EmitOp` (modified map) | Impact craters and re-checks are emitted through `EmitOp` like casts; joiners get them as modified chunks |
| No checks | `HashCheck` / `EntityCheck` / `EntityList` (7b) | Nothing new to check: craters are `ClearMask` ops, shatter pieces are entities. Both are in the existing comparisons |
| `TakeContactEvents` nobody calls; queue fills | Still true (notes/physics.md); `ContactCollector::Publish` keeps one event per body pair after an unstable `std::sort` of events filled in Jolt's thread arrival order (PhysicsWorld.cpp:279-306) | SI-2 drains every tick on every side (also a fix on its own) and makes `Publish`'s order a function of the events' content |
| A client presents a received `CarveSphere` as a cast (non-deferred group: `fx::SpawnExplosion` and a flash, Blast.cpp:868-891); a `Detach` / `ClearMask` with no open group as `deferred` (no burst, no flash) | Built (Client.cpp `Present`) | Craters are a `ClearMask`, re-checks emit no carve: both are shown as dust. Offline, impact and re-check results set `deferred` too, so one rule shows them everywhere (`BlastResult::cause` is for logs and stats only) |
| Part 5: `ColliderMaterials`, fatigue, a 30-tick impact queue, `OpBatch` / `PresentOps` | Not built | Simplified away (next table) |

<!-- source lines 2057-2076 -->

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

<!-- source lines 2077-2104 -->

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
