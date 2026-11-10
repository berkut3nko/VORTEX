# Light structural collapse ("SHRED light")

Status: design decision, nothing built. All run-time costs for new code are estimates from
operation counts (about 5 ns per voxel flood-filled, 1-2 ns per mask operation); stage 2 contains
the benchmark that replaces them. Claims about today's code were checked against the sources.

## Decision

Collapse is solved on a **pre-computed support graph over 8^3-voxel cells**, not on voxels. One
graph format (fixed 32-byte cells, exact connectivity) serves both hosts: static voxel objects
first, terrain-stamped structures later. Only the part that lost its support leaves the structure;
the anchored remainder stays static and untouched. What falls is cut along **pre-computed
fracture bricks** of 4x4x4 cells (32^3 voxels = one chunk slot, one body) and passes a
**budget** that turns the largest, nearest bricks into bodies and everything else into particles.
Every collapse body gets a `Lifetime`.

Buildings are **static voxel objects (entities) up to 128^3** until stage 5. Larger structures and
anything that must stream go into the terrain grid in stage 5, using the same cells.

Rejected, and why:

- **Terrain-first structures (proposal B's core).** Right for scale (streaming, zero instances for
  an intact building, future clip-map LOD), but needs a placement registry with persistence,
  palette remap to the 255 world materials, an 0.8 m placement grid, `World::StampVolume` /
  `ExtractBox`, and a fix for bodies spawned inside stale terrain colliders, before the first
  wall falls. The generator has no structures yet (ROADMAP "Planned, not built"), so nothing needs
  it today. Kept as stage 5.
- **Variable-size CSR node/edge graph with bond areas, masses and a load check (B).** A load path
  that breaks bonds needs per-material strength tuning ("too weak and buildings fall on spawn")
  and is not required for the wanted result. The overhang rule below gives floors that drop when
  a wall goes at the cost of one traversal. Fixed-size cells are updated in place; CSR is not.
- **Dominator-tree weak-point table (B).** No consumer (AI, targeting) exists.
- **Whole-object graph on unlimited object sizes (A as written).** A 256^3 object is 512 of the
  12 288 chunk slots and a 16.7 MB volume per private copy; that size belongs to the terrain.
- **Keeping today's behaviour for large objects.** Per hit `Scene::Damage` copies the whole volume,
  allocates a `uint32_t` label per voxel, re-bakes every chunk and rebuilds the whole compound.

## How it works

### What is wrong today (verified)

- `Scene::Damage` (`scene/internal/Scene.cpp`, `splitApart`): once a static object is in more than
  one piece, every piece becomes Dynamic. A building that loses a balcony falls as a whole.
- Per-hit cost scales with the object: 64^3 = 256 KiB copy + 1 MiB labels + up to 8 chunks
  re-baked (fine, estimated 1-3 ms); 256^3 = 16 MiB + 64 MiB + up to 512 chunks, which at
  `maxChunkUploadsPerFrame = 128` is at least 4 frames of upload alone.
- Terrain: `Engine::Explode` searches radius + 24 voxels with a 60 000-voxel cap;
  `ExtractFloatingIslands` treats a component touching the search box as attached and refuses
  regions over 192 voxels per axis. The playground (`src/main.cpp:282`, `world.FillBox`) is
  terrain, so a tower with its base blasted out stays in the air.
- No piece ever gets a `Lifetime`; `minPieceVoxels = 6` keeps tiny bodies alive.

### Data (`vortex.voxel:structure`, CPU only)

```cpp
constexpr int kCellSize = 8;          // voxels; 4^3 cells per chunk = ChunkOccupancy::cellMask
constexpr int kMaxCellNodes = 8;
constexpr int kBrickCells = 4;        // fracture brick = 4x4x4 cells = 32^3 voxels

struct SupportCell {                  // 32 bytes
    uint64_t link[3];                 // +X,+Y,+Z face: bit (i*8+j) = my node i touches neighbour node j
    uint8_t  nodeCount;               // face-connected solid components inside the cell, 0..8
    uint8_t  anchorMask;              // bit i = node i is an anchor
    uint16_t solid;                   // solid voxels, 0..512 (budget, mass estimate)
    uint8_t  material;                // dominant material id -> overhang limit
    uint8_t  reserved[3];
};
struct SupportGraph { glm::ivec3 cells; uint64_t revision; std::vector<SupportCell> cell; };
```

- A node is one face-connected component inside one cell, numbered by its first voxel in scan
  order, so re-labelling an unchanged cell reproduces the same indices. No per-voxel labels are
  stored. Links exist exactly where two voxels share a face across a cell border, so graph
  connectivity **equals** voxel 6-connectivity. Two thin walls crossing one cell are two nodes.
- More than 8 components in a cell (gratings): extras merge into node 7. This over-connects (a
  fragment may hang) but never drops attached matter. The bake counts and logs such cells.
- Anchors: a node holding a voxel at model y == 0 or a `MaterialFlag_Indestructible` voxel. Used
  only when the body is Static; dynamic objects keep "largest piece keeps the entity".
- Fracture bricks are implicit: `brick = cell / 4`, with a per-model hash jitter of the brick
  origin so neighbouring buildings do not break on the same lines. Nothing is stored.
- Size: 64^3 = 512 cells = 16 KB; 128^3 = 128 KB; 256^3 = 1.0 MB (6.25 % of the dense volume).
  Held as `shared_ptr<const SupportGraph>` by the `VoxelModel`, shared by identical buildings,
  copied on the first private damage.
- Bake: one 512-voxel fill per cell plus three face scans; estimated 1.5 ms (64^3), 12 ms
  (128^3). Run on a `JobSystem` job at spawn / editor placement; built lazily in `Damage` with a
  warning above 96^3. `revision` is compared with `VoxelModel::Revision()`; mismatch = rebuild.

### Run time (`Scene::Damage` on an object with a graph)

1. **Carve** the sphere (loop unchanged).
2. **Refresh** the cells overlapping the carve box plus their face masks. Radius 0.6 m: at most
   27 cells; 1.2 m: at most 125. Estimated 0.3-0.8 ms, independent of model size.
3. **Early out**: no `nodeCount` or `link` changed = a dent. Skip the rest.
4. **Support solve**: bucketed 0-1-2 search from all anchor nodes. Stepping up costs 0, sideways
   1, hanging below 2; the sum is the node's overhang. A node falls if unreached or if its
   overhang exceeds the limit of its material (start values in cells: stone 3, wood 6, iron 10;
   the limit is off until stage 3, i.e. pure connectivity). One byte of visited bits per cell.
   Estimated 5 us (64^3), 50-80 us (hollow 256^3), 0.4 ms worst case (solid 256^3).
5. **Group** falling nodes by connectivity, then split groups by brick id.
6. **Budget** each brick, scored by `solid / distance to the camera`:

   | Condition | Becomes |
   |---|---|
   | Top `maxCollapseBodiesPerStep`, live count below `maxLiveCollapseBodies`, inside `collapseBodyRadius` | Dynamic entity: voxels moved into a cropped volume (at most 32^3), `SplitComponents` on it for exact pieces, then model + `Bake` + `CreateBody` as today. Estimated 0.3-0.5 ms each |
   | Over budget or under `minPieceVoxels` | Voxels removed, sampled `fx::SpawnVoxelBreak` particles (cap 400 per event, as `Explode` uses) |
   | Outside the radius | Voxels removed, no bodies, no particles |

7. **Queue**: bricks not released this step wait in a scene queue, lowest first, and stay drawn
   and solid in the static model until their turn. A big collapse therefore spreads over several
   fixed steps (6 bodies = about 2-3 ms per step) instead of one spike.
8. **Lifetime**: bodies under `debrisBodyVoxels` live `debrisLifetime`; larger ones
   `rubbleLifetime`. Above `maxLiveCollapseBodies` the oldest sleeping one is removed first.

`SplitComponents` / `LabelComponents` stay as the reference implementation, the path for objects
without a graph, and the exact splitter for extracted bricks. `DamageResult` and the
`Engine::Explode` loop are unchanged.

### Failure modes

- Chunk store full when a piece bakes: `Bake` is all-or-nothing; the brick is demoted to
  particles instead of being simulated invisibly.
- Out of bodies: existing "piece dropped" path; the budget keeps the count far below 16 384.
- Player standing on a queued brick rides static ground for a few steps; on a body whose
  `Lifetime` ends, it disappears. Rule: expire only bodies that are asleep.
- A building bridged to a cliff or set into a hill is anchored only at y == 0 until anchors can
  be painted (stage 3).
- Overhang limits too low make a pristine building unsupported: the bake runs the solve once and
  reports unsupported nodes instead of dropping them.

## What changes in the engine

| File | Change |
|---|---|
| `src/voxel/Structure.cppm`, `src/voxel/internal/Structure.cpp` (new) | `SupportGraph`, `BuildSupportGraph(volume, materials)`, `RefreshSupportGraph(graph, volume, min, max) -> bool changed`, `FindUnsupported(graph, limits, out)`; re-exported by `Voxel.cppm` |
| `src/voxel/Model.cppm` | `Structure()` / `SetStructure()` holding the shared graph; stage 4: `RebakeRegion(min, max)` rewriting only touched chunk slots |
| `src/scene/Scene.cppm` | `SceneConfig`: `minPieceVoxels` 6 -> 24, `maxCollapseBodiesPerStep = 6`, `maxLiveCollapseBodies = 64`, `collapseBodyRadius = 40`, `debrisLifetime = 8`, `rubbleLifetime = 30`. Additions only; no exported signature changes |
| `src/scene/internal/Scene.cpp` | `Damage`: anchored pieces stay Static (stage 1), graph path, budget, queue drained in `Update`, `Lifetime` on pieces |
| `src/engine/internal/Engine.cpp` | `Explode`: `Lifetime` on terrain islands; stage 5 hook for terrain structures |
| `src/physics` | Stage 4: per-`ModelPart` collider box cache + existing `PhysicsWorld::SetShape` (`PhysicsWorld.cpp:718`). Stage 5: `TerrainColliders` "is this chunk's collider current" query (today only `IsReady(worldPos)`) |
| `docs/ARCHITECTURE.md` 7.2, `docs/ROADMAP.md` | Updated with each stage |
| `tests/core`, `tests/engine`, `src/main.cpp` | Tests and the headless tower check below |

## Stages

1. **Anchored remainder + debris clean-up (about a day, no new subsystem).** In `Scene::Damage`,
   when the source was Static, every piece with `piece.offset.y == 0` (or holding an
   indestructible voxel) stays Static and the largest of them keeps the entity; the rest become
   Dynamic. Add `Lifetime` to pieces and terrain islands, raise `minPieceVoxels` to 24, enforce
   `maxLiveCollapseBodies`. This alone gives partial collapse for objects up to about 64^3 and
   answers the "tiny debris lives forever" feedback.
   *Verify*: `VortexEngineTests`: cut a static pillar-and-slab object; base is Static, top is one
   new Dynamic entity; a 5-voxel fragment yields no entity; a piece is gone after its lifetime.
   Headless run still logs "Beam cut: ... 1 new object(s)" (the beam is dynamic).
2. **Graph core, headless.** `voxel:structure` with build, refresh and connectivity-only solve.
   *Verify*: `VortexCoreTests`: on random volumes graph components equal `LabelComponents`;
   refresh after a carve equals a full rebuild; removing the ground-floor cells of a test
   building releases exactly the upper cells; a benchmark on a solid 256^3 prints bake, refresh
   and solve times (replaces the estimates above).
3. **Graph in `Scene::Damage`.** Dent early-out, brick extraction, budget table, release queue,
   overhang limits per material, editor overlay for anchors / unsupported nodes / overflow cells.
   *Verify*: a static 96^3 test tower in `src/main.cpp`; the headless run blasts its base and
   logs "Tower: N bricks fell, base static", validation-clean; `ProfileScope` around `Damage`
   stays under 3 ms per fixed step during the collapse on the UHD 620.
4. **No whole-object work per hit.** Edit in place when `model.use_count() == 1`
   (`RebakeRegion`: 1-8 chunks instead of all), per-chunk collider boxes with `SetShape`, cook on
   a job and swap a frame later.
   *Verify*: test that a dent in a 128^3 model dirties at most 8 chunk slots; profile of a dent
   on a 128^3 object under 1 ms.
5. **Terrain structures.** The same 32-byte cells cached per loaded chunk (64 cells = 2 KB per
   chunk, filled straight from `ChunkOccupancy::voxelMask`), a `StructureField` with placement
   records, `World::StampVolume` / `ExtractBox`, body creation deferred until the chunk collider
   is current, events queued while chunks are unloaded, far collapses resolved as a stamped
   rubble heap. Removes the 192-voxel limit for stamped structures.
   *Verify*: the playground tower (terrain) collapses when its base is blasted; reload restores
   the damaged state from chunk deltas plus the placement record.

## Open questions for the game

- **How big is the largest destructible building?** Up to 128^3 (12.8 m) the object path of
  stages 1-4 is enough. Keeps and towers of 25 m and more need stage 5 and with it the world
  palette (255 shared materials) and 0.8 m placement steps for those buildings. Acceptable?
- **What happens to rubble?** Removed after `rubbleLifetime` (cheapest), stamped into the terrain
  as a permanent heap (persistent, costs chunk edits), or kept while a player is near.
- **Should material strength matter?** The overhang limits (stone 3 cells, wood 6, iron 10) make
  floors drop when a wall goes. Is that the wanted feel, or should buildings only fall when
  fully cut (limits off)?
- **Budget on the target machine**: 6 new bodies per step, 64 live, bodies only within 40 m are
  guesses; they should be set from a measured fight scene, and the owner decides how much of a
  collapse may be particles instead of tumbling blocks.
- **Brick size**: 32^3 voxels (3.2 m) blocks are cheap (one chunk slot each). Smaller rubble (16^3)
  looks better and costs up to 8 times the bodies.
- **Do falling pieces hurt?** Damage to players and enemies from collapse bodies is game logic
  the engine can expose (contact events) but must be asked for.
