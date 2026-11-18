# Networking history: stage 2 as built, crumbs, and the changes by shred.md stages 0-1c, 4 and 4b

Moved verbatim from `docs/design/networking.md` lines 361-489 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

### Stage 2 as built

Built: `voxel::SphereQ`, `QuantizeSphere`, `QuantizePower`, `InSphereQ`, `SphereVoxelBounds`, `voxel::EditReport`,
`voxel::VoxelMask`; `World::CarveSphereQ / FillSphereQ / SwapSphereQ / ClearMask` and a report argument on them and
on `FillBox` (the float `CarveSphere` / `FillSphere` quantise and forward); `ExtractFloatingIslands(..., bool*
searched)`; in `vortex.sim` the partitions `:worldop` (`WorldOpKind`, `WorldOp`, `OpResult`, `ApplyOp`,
`BuildDetachedPiece`) and `:blast` (`BlastDesc`, `AppliedOp`, `BlastResult`, `BlastPush`, `Blast`);
`SceneConfig::replicatedPieceVoxels`, `DamageFragment`, `DamageResult::fragments / toRubble`;
`sim::kServerReplicatedPieceVoxels` = 512 set by `ServerConfig`; `Engine::PresentBlast`. `Engine::Explode` is now
`PresentBlast(sim::Blast(...))`, and the game's scripted crate blast calls the two halves itself. Where the design was
open or disagreed with the code, this was chosen:

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| Rounding of the float -> q conversion. | `std::llround(x * 16)` in double (halves away from zero), centre clamped to the int32 range (+-2^27 voxels), radius to [0, 65535] q (4095 voxels), non-finite input = radius 0. Metres are divided by the voxel size in double first (`sim::Blast(sim, BlastDesc)`). | Quantisation runs only where an operation is made (server, offline); clients receive the integers. Every later decision uses only the integers: the island box is `floor(centre / 16) +- (radius / 16 + 24)` from the q values, plus the face flood inside the flood box `floor(centre / 16) +- 96` (shred.md stage 1); since stage 4 both are searched on the cell graph (same boxes). |
| Sphere bounds. | `SphereVoxelBounds` is the per-axis box `abs(16 v + 8 - c) <= r`, which can be one voxel larger than the tight bounds per side; the voxel test decides. Edit boxes are clipped to +-2^30 voxels (`kMaxEditVoxel`), `FillBox` too, so no `ChunkToVoxel(chunk) + 32` overflows. | `InSphereQ` returns early when one axis exceeds the radius, so squares stay below 2^34 for any `int` voxel. |
| How is `power` compared with a float hardness without rounding? | A voxel survives when `hardness * 16 > power` in float: multiplying by 16 is exact and so is converting a 16-bit integer, so the compare has no rounding. A per-carve table of 256 flags is built from it. | The material table must be equal on both sides (`buildHash` in `Hello`, stage 3). A hardness that is not a multiple of 1/16 (0.6 = 9.6 units) compares exactly: power 9 leaves it, 10 removes it (tested). |
| "Skipped / unsynced chunk report": which chunks? | Only chunks the operation would really touch (exact for sphere, box and mask), in visit order (y, z, x). `skipped` = not loaded: outside the window, `Unloaded` or `Loading`; `unsynced` = loaded but Empty / Uniform and the store had no slot. A chunk is all-or-nothing: a non-dense chunk is materialised at its first change, so a failed slot leaves it untouched. | Stage 7 turns a skipped chunk into replay from the ring or a `ChunkRequest`, an unsynced one into the "unsynced" client state. Today the offline game ignores the report, as it ignored the silent skip before. |
| `WorldOp` shape. | One struct with a kind (`CarveSphere = 1, FillSphere = 2, SwapSphere = 3, FillBox = 4, Detach = 5`; values are wire format) and the union of the fields; unused fields keep defaults so `==` compares exactly. `Detach` holds `min` = origin, a `VoxelMask` (bits packed 8 per byte in `VoxelVolume::Index` order, the bytes are the wire form before RLE), `velocity`, `netId`. `FillSphere` was added (templates, the editor brush); the swap writes `id` in place of `from`; `from == to` is a no-op, `from = 0` fills air. | Stage 3 encodes it as in the "World operations" table. No `opSeq` yet: `BlastResult::ops` is an ordered list and the server assigns sequence numbers when it emits them. |
| How does the blasting side clear an island? | `ExtractFloatingIslands` runs with `removeFromWorld = false`; each island becomes a `Detach` that is applied with `ApplyOp` (= `World::ClearMask`), the call a client makes. The piece is built from the ids that call removed (`BuildDetachedPiece`), on the server as on a client. | The old path removed island voxels with `SetVoxel` one by one; the new one is one edit per island, with the report. Islands are found by the single-threaded scan of `LabelComponents` in `Index` order and emitted in the order of their first voxel, so equal voxels in the box give equal operations in equal order (tested: two islands, scan order). |
| An island box that is not fully loaded. | The search is refused as before, but now visible: `BlastResult::islandsSearched = false` (new `searched` out-parameter). No `Detach` is emitted, so nothing diverges. Offline, `sim::Blast` asks the world to load the box (`World::RequestLoad`) and `sim::RetryIslandSearches` runs the search once it is loaded (view distance stage 1 review): its `Detach` / `ClearMask` are later operations of their own, not part of the blast's. | A server window always has the flood box loaded (3-chunk margin; shred.md stage 1), so a server never defers: an island box that is still not loaded there is an error (`islandBoxUnloaded`, logged), not a deferral. A client must not defer either: it applies the server's operations. |
| Who creates rubble for a `Detach` with `netId = 0`? | Nobody in the simulation: `sim::Blast` makes no entity for an island. `Engine::PresentBlast` creates the local rubble (`VoxelObjectDesc::debris`) from the voxels the Detach removed and starts it with `WorldOp::velocity` = `BlastPush` at the island's corner, the velocity change the old impulse gave (impulse = mass x push, so mass cancels). The corner is where the old code sampled it too: the rubble body sat at `VoxelToWorld(island.origin)` and `BodyState::position` is the shape origin, not the centre of mass (test `BodyPositionIsTheShapeOrigin`). Sampling every push at the centre instead (bodies, fragments, islands together) would be a gameplay change, not part of stage 2. | **Behaviour change offline:** a fresh island is no longer hit by the same blast's `Damage` (the old snapshot was taken after islands were created). Islands are pushed exactly as before. |
| What does `PresentBlast` get, and what stays in `Scene::Damage`? | `BlastResult` keeps per operation the `VoxelEdit`s it made (the carved voxels for particles, the island voxels for the piece), the quantised centre and radius, and the object fragments. Terrain debris particles, island rubble, fragment rubble, the explosion burst and the flash are in `PresentBlast`. `Scene::Damage` still spawns its own carve / loose-voxel particles into the scene's pool (empty on a server). | Moving object particles out of `Damage` belongs to stage 6, where a client makes them from the old and new `ObjectModel`. |
| `Scene::Damage` threshold: default, survivor, motion type. | `SceneConfig::replicatedPieceVoxels`, default 0 = off (offline: every piece of `minPieceVoxels` or more is an entity, unchanged). `ServerConfig` sets 512 (view-distance stage 3: 0.512 m^3 in voxels of the world, 262 at 0.125 m). A piece below it gets no entity, no body and no slot; it is returned as `DamageFragment{model, pose, linear, angular velocity}`. The survivor goes too (`toRubble`, `destroyed`) when it is below the threshold **and Dynamic** after the cut; a Static / Kinematic object that is only dented keeps its entity whatever its size. `sim::Blast` adds `BlastPush` to fragment velocities. | Stage 6 sends these as `EntitySpawn` with `netId = 0` / `EntityDespawn{toRubble}`. On a server today a crate (512 solid voxels in the sim test, 296 in the game) disappears at its first hit; offline nothing changes. |
| Where `Blast` runs in the offline game. | Where `Explode` ran: `OnUpdate` (mouse click; the headless script at frame 90), not in `OnFixedUpdate`. | Unchanged timing keeps the before / after comparison exact. The server runs `Blast` inside the tick, after streaming and before physics (stage 3 / 5 cast queue). |

Verified: 22 new tests (12 `QuantizedEdits.*` in `VortexCoreTests`; `WorldOps.*` 3, `Blast.*` 5 and `ServerScene.*` 2 new
in `VortexSimTests`), 385 in all, green; the sim tests repeated 10 times. The acceptance test
`Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` builds two simulations with different worker counts (1
and 3), applies the same set-up operations (boxes and a swap), runs two `sim::Blast`s on one (two islands, then a
plain crater) and only `ApplyOp`s the resulting four operations on the other: `WorldHash` over the whole window is
equal and complete, and each client piece has the server mask's voxel count. The same stream with the first carve
1/16 voxel larger, or with one `Detach` left out, gives a different hash. Headless game, editor and pause-menu runs
exit 0 under validation; the crate blast logs `7 -> 7 objects (0 terrain voxels, 0 island(s), 558 object voxels in 5
object(s))` - before the change the same 5 objects lost 118 + 100 + 44 + 252 + 44 = 558 voxels and no terrain - and
`Beam cut: 37 voxels removed, 1 new object(s)`, as before. Screenshots of the crate area (`--camera
14.3,7.0,-4.5,3.1416,-0.2`, frames 91, 95, 120, 200) before and after look the same; the resting places of the
pieces differ between runs of one build as much as between the builds. A temporary run (not kept) blasting the base
of a crystal column showed the island path offline: 1 island of 144 voxels detached, became rubble and fell.
**Not done:** no opSeq, encoding or transport (stage 3); `BlastResult::Report()` is not acted on anywhere; object
particles are still spawned inside `Damage`; the 512 threshold is untested in play (stage 6 tunes it).

**Crumbs (after stage 2, owner report).** A blast into rough terrain made one `Detach` per floating island, however
small: one blast produced ~180 records of 1-5 voxels each, and offline every one of them became a `VoxelModel` that
`CreateVoxelObject` then crumbled. The owner's rule: 1-5 voxel objects must not live in memory; under 16 voxels (0.1 m then; 0.016 m^3 since view-distance stage 3) is
particles.

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| Which islands are crumbs? | `voxelCount < scene::kMinPieceVoxels` (16), a constant exported by `vortex.scene` and the default of `SceneConfig::minPieceVoxels`. Since view-distance stage 3: `voxelCount < scene::MinPieceVoxels(world.VoxelSize())`, the piece volume 0.016 m^3 in voxels of the session's size (16 at 0.1 m, 8 at 0.125 m). | What the server sends must not depend on a scene's tuning, so `sim::Blast` uses the world's voxel size (a session parameter both sides hold bit for bit), not `Scene().Config()`. A scene with a larger `minPieceVoxels` still crumbles a larger `Detach` piece when it presents it. |
| How are they sent? | One new `WorldOpKind::ClearMask = 6` (`WorldOp::Clear(origin, mask)`: `min` and `mask` only, other fields default) per blast, after the carve and the `Detach`es, only if there are crumbs. `ApplyOp` = `World::ClearMask`, the call a `Detach` makes. | The mask's bits are set from each crumb's own voxels, so it does not depend on the order islands come in; the islands themselves come in scan order anyway. The world after it is the same as after one `Detach` per crumb (crumbs are disjoint). `BlastResult::crumbs` counts them, `IslandCount()` now counts `Detach` operations only, `CrumbVoxels()` the cleared voxels. |
| How is the union mask bounded? | **One mask over the smallest box holding every crumb**, no split per chunk. Since shred.md stage 1 a crumb proven by the face flood touches the island box face and reaches past it by less than `MinPieceVoxels`; since stage 4b a crumb the load check releases touches a judged node (whose voxels lie at most 7 voxels outside the island box) or is a load alone hanging on one, so it reaches past the box by less than `MinPieceVoxels + 7` (corrected in the stage-4b review). The raw mask is therefore at most `(2 * (radius / 16 + 24) + 2 * (MinPieceVoxels + 7))^3` bits (before: `(2 * (radius / 16 + 24))^3`, 45.6 KiB for the game's 1.2 m blast at 0.1 m); 864 KiB plus that margin at the hard limit (ExtractFloatingIslands refuses boxes over 192 voxels per axis). It lives only while the blast is applied and sent; the wire form is RLE. | One mask per chunk would turn the same crumbs into up to 27-64 records (a 72-voxel box spans 3-4 chunks per axis) with the same run-length bytes plus a header each, which is what this change removes. Applying a sparse mask is cheap: `World::ClearMask` skips every row of the box whose mask bits are all zero (`VoxelMask::AnyInRange`) before it reads a chunk id. Re-applying the largest measured mask (45 x 60 x 70 voxels) took 1-2 ms in the dev build, 18.7 ms with the old per-voxel test. |
| Bytes per blast. | Measured on the default terrain (44 blasts of 1.2 m, power 6, shot straight down on a 7 x 7 grid of points +-22 m around the origin, trees included). Wire bytes are estimates: header 17 B per carve, 30 B per `Detach`, 19 B per `ClearMask`, the mask as alternating zero / one run lengths in LEB128. | **Before:** 2051 operations (44 carves + 2007 `Detach`, every one of them under 16 voxels), worst blast 299 operations (370 voxels in 298 islands), est. 65 KB in all, 9.6 KB for the worst. **After:** 75 operations (44 carves + 31 `ClearMask`; no island reached 16 voxels), at most 2 per blast, est. 7.2 KB in all, 908 B for the worst (19 B + 889 B of runs; isolated crumb voxels cost two runs each, about 2.4 B per voxel). Largest raw mask held: 23.6 KiB. |
| Who shows them? | `sim::PresentBlast` (new, GPU-free; `Engine::PresentBlast` forwards to it): for a `ClearMask` it spawns `fx::SpawnVoxelBreak` for every `debrisParticleStride`-th (3) removed voxel, at most 400 bursts, in the voxel's material colour, each pushed with `BlastPush` at that voxel. No model, entity or body. | Before, a crumb got at least one burst and the push of its corner; now a 1-2 voxel crumb can get none when its voxels are not on the stride. The presentation moved out of `Engine` so the "no entity, no body" rule is unit tested; a stage-3 client calls the same function. |

Verified: 7 new tests (`QuantizedEdits.MaskRangeTestSeesExactlyTheBitsInTheRange`,
`QuantizedEdits.SparseClearMaskReportsOnlyChunksWithMaskedVoxels`; `Blast.CrumbsBecomeOneClearMaskAndLargerIslandsKeepTheirDetach`
(22 crumbs + one cap -> carve, one `Detach`, one `ClearMask` of exactly the 83 crumb voxels over their tight box),
`Blast.IslandOfSixteenVoxelsIsAPieceAndOfFifteenACrumb`, `Blast.OfflineCrumbsBecomeParticlesWithoutAnyEntityOrBody`,
`Blast.OfflineLargeIslandStillBecomesRubble`, `Blast.TwoSimulationsFedOperationsWithAClearMaskHaveEqualHashes` (equal
hashes; a second server makes the same operations; the stream without its `ClearMask` gives a different hash)) and
`WorldOps.ApplyOpIsTheWorldCallOfEachKind` extended; 392 in all, green. The measurement above was a temporary test,
not kept. Headless game, editor and pause-menu runs exit 0; the crate blast logs `0 island(s), 0 crumb(s) of 0
voxels` (unchanged: it carves no terrain). **Found, not addressed:** the island search itself took 70-200 ms per
1.2 m blast in that measurement, about all of `sim::Blast`'s time - and the `dev` build compiles without any `-O`
flag (`CMAKE_CXX_FLAGS_RELWITHDEBINFO` is empty in the cache, CMake 4.4), so every CPU time in this paragraph is
unoptimised.

**Changed by shred.md stages 0-1c (2026-11-12).** The island search proves face components by a bounded flood
inside the flood box (`voxel::ProveFaceComponents`), so the **result** of a search changes for the same voxels: a
piece that reaches past the island box is now one more `Detach` (or crumbs in the `ClearMask`) of the same blast. No
new `WorldOpKind`, no change of `Detach` or `ClearMask`: clients still only apply. The decision is integer-only, in a
fixed candidate and visit order, with a work budget counted in visits (240 000 per blast), and does not depend on what
is loaded outside the flood box (`Blast.FaceFloodIndependentOfLoadingOutsideTheFloodBox`,
`Blast.FaceFloodTwoSimulationsEqualHashes`, `Blast.FaceFloodBudgetIsCountedInVisits`, which reaches the budget with
1 and 3 workers at the same visit, 240 001). A proven piece can span up to
192 voxels per axis (raw mask up to 864 KiB, RLE on the wire); the watch tower is 8516 voxels at 0.125 m. A `Detach`
whose RLE mask exceeds the 16 kB event burst is still sent (correctness first) and must be logged by stage 3.
Offline only: a flood that meets a chunk that is not loaded defers the search; a carve that skipped loading chunks is
replayed on exactly those chunks as each one loads, before the late search (`World::CarveSphereQInChunk`; such a
replay is never sent; a client replays skipped ops in stage 7 instead).

**Changed by shred.md stage 4 (2026-11-12, before stage 3 by the owner's decision).** The server searches islands on
the cell graph cached per terrain chunk (`voxel::SearchIslandsOnGraph`, `sim::IslandSearchKind::Graph`, the default)
instead of the voxel search, in the same island and flood boxes, with the same emission order. The graph is exact
(every component of an 8^3 cell is a node; collapse.md's "more than 8 components merge" was not built), so the
**results** equal the stage-1 search except where a work budget runs out (the graph counts node visits,
`sim::kGraphNodeVisitBudget` = 32 768 per blast, the voxel search voxel visits, 240 000) and, when a chunk of the flood
box is not loaded, in the offline deferral and the reason a flood counts as attached (they flood in another order; on a
server both attach, so the operations are equal). That is a server decision;
clients still only apply, and no `WorldOpKind`, `Detach`, `ClearMask` or wire format changed. Tested: every stage-1
case gives equal operations and hashes with either search (`Structure.GraphFloatingEqualsFaceFlood`), every stage-1
test compares the two on its own voxels (`SimulationConfig::checkIslandSearch`; face islands only where no budget ran
out and every flood box chunk was loaded), and the graph never detaches what the
voxel search keeps on random terrain with liquids (`Structure.GraphNeverDetachesWhatTheVoxelSearchKeeps`). A cast now
costs 0.06-1.0 ms of island search on a quiet CPU, cold graph builds included (game terrain and flat ground; the voxel
search 3.3-19 ms). In game a cold chunk graph costs 70-120 us (15-27 us quiet; shred.md "Stage 4 as built"), and the
node budget does not bound builds: the first cast at an untouched site builds up to the island box's chunks (up to 64
for a 1.2 m blast) plus chunks its floods reach, so the cold worst case is about 64 x 120 us = 7.7 ms. **Stage 3's cast
budget must charge `IslandSearchStats::chunkGraphs` (graphs used, a function of the flood box voxels) at that cold
price**, not `graphsBuilt`: which graphs are cached depends on earlier casts, eviction and streaming, so a budget
charged by builds would put a replayed cast on another tick. Prebuilding graphs on a worker (shred.md owner question)
would lower the price. The graph cache is per `sim::Simulation` and invalidated by
`World::ChunkRevision`, so a server needs nothing new at stage 9 beyond the flood box it already keeps loaded.

**Changed by shred.md stage 4b (2026-11-13, before stage 3 by the owner's decision).** After the island floods the
server runs a load check on the same graph (shred.md Part 4b): the nodes of the island box that are not islands are
judged, what hangs on them inside the flood box is a load, what reaches the flood box face is support, and the largest
set whose weight no distribution fits the strength of what holds it (integer max-flow, unique whatever the visit order)
is released. The **meaning of the operation stream changes**: a blast may now emit more `Detach` operations (netId 0)
after its islands - a tower on a thin neck falls on the cast that judges it - and their crumbs join the one `ClearMask`.
**No new `WorldOpKind`, no change of `Detach`, `ClearMask` or of the wire format; clients still only apply.** Inputs: the
flood box voxels, the integer strength table (`Material::strength`, compiled-in until stage 3 hashes it into
`buildHash`, R6) and `VoxelSizeMicro(World::VoxelSize())`; a replayed server releases the same parts
(`Blast.NeckReplayGivesIdenticalOps`, `Blast.NeckReleaseTwoSimulationsEqualHashes`,
`Blast.NeckNothingOutsideTheFloodBoxIsRead`; `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` unchanged). A
released piece can be large: the watch tower is one `Detach` of 8558 voxels at 0.125 m (1-2 KB RLE, inside the 16 kB
event burst). On a server an exterior flood that meets a chunk that is not loaded is support and an error
(`neckUnloaded`); offline it is support for that cast too (counted, `neckLoading` / `neckBeyondWindow`) and a late
search runs the check again once the chunk has loaded - it never holds back the islands (stage-4b review). One check
releases at most 64 parts and 60 000 voxels (`kNeckMaxReleasedParts` / `kNeckMaxReleasedVoxels`; the rest stays for a
later cast), so one cast adds at most 64 `Detach` operations and one tower-sized mask per cast. The
check costs 0.13-0.73 ms quiet at a cached site, plus up to 1.8 ms of graph builds at a cold one on game terrain
(shred.md "Review of stage 4b"); its work is counted (above), so stage 3's budget charges it. **At its own budgets it
is far over the cast phase:** the preflow alone takes 5-6 ms at about 207 000 arc scans of its 262 144, est. 8-12 ms
with every budget full; stage 3 either charges it so that such a cast moves to a later tick, or the flow budget comes
down (shred.md open question 7). Strength-table drift between a server and its
replays changes decisions: the table must reach `buildHash` in stage 3.
