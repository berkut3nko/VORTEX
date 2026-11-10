# Level of detail for distant terrain and objects

Status: design decision, nothing built or measured for it. Millisecond figures not quoted from
`ROADMAP.md` are estimates; every stage names the measurement that confirms or kills them.
Claims about today's code were checked against the sources (uncommitted physics work included).

## Decision

1. **Distance budgets first** (lighting rate, AO cut-off, shadow-BVH membership, debris and
   particle scaling). No data-layout change, and it is the only part that makes fights cheaper.
2. **Far terrain = filled clip-map cascades**: level L is a full 48 x 8 x 48 window with voxel
   size `0.1 * 2^L` m around the same focus, *including* the region the finer levels cover. A ray
   traces the finest level and continues in the next one where the finer box ends. Level 1 is a
   second `voxel::World` on the shared `ChunkStore` in the existing chunk format (proposal A);
   shader, uniforms and CPU twin are level-indexed for up to 4 levels from the start (proposal B).
3. **Dynamic objects get no LOD representation.** They are visible inside the level-0 window
   only; anything that must be seen further away has to be world voxels, which get LOD through
   edit propagation (same rule as `collapse.md` stage 5).
4. **Simulation LOD** is the terrain guard that already exists plus distance-scaled debris
   thresholds; lifetimes and body budgets are owned by `collapse.md` and not repeated here.

Rejected, and why:

- **Height-map far field.** Second traversal that `trace.glsl` / `Trace.cpp` and its tests do
  not share (ARCHITECTURE 4.5), no trees, overhangs, craters with undercut or built structures.
- **Bigger level-0 window.** 96 x 8 x 96 is about 32 000 slots = 1.1 GiB.
- **Hollow rings.** Save 25 % of the columns per level but make every unloaded fine chunk a hole
  and forbid coarse shadow rays. Filled levels also give a fallback for streaming holes.
- **Compact `LodChunk` format + `LodStore` now (B).** 28 MiB per level instead of 113 MiB, but a
  second chunk format through `World`, the region store, the generator interface, the upload path
  and `VX_VOXEL_ID` before the first far pixel is seen. Level 1 fits in slots that are already
  allocated. Revisited in stage 4, where memory actually decides.
- **Cone LOD for primary rays (B).** A 0.2 m voxel is 1 px only at 75 m; using level 1 from 37 m
  would show 2 px voxels inside the level-0 window. Coarser levels are used only where the
  finer one ends, and for secondary rays of far pixels.
- **Model mips (B) and impostors.** Mip 1 is pixel-exact from 75 m; objects end at 77 m. There is
  no distance at which a mip is both valid and visible at 768x432, and each mip costs slots plus
  a re-bake on every `Scene::Damage`. The proxy box already is the impostor.
- **Physics tiers with Kinematic switching / `Deactivate` (both).** Superseded: the working tree
  already has `PhysicsWorld::SetFrozen` and `TerrainColliders::GuardBodies`, which freeze any
  dynamic body whose terrain is not ready, with no 32-body limit.

## How it works

### Numbers the design rests on

- 768x432, `fovY` 60 deg: one pixel = 2 tan(30 deg) / 432 = 0.00267 rad. Voxel = 1 px at:

| Level | voxel | 1 px at | window (48 x 8 x 48) | view | fog transmittance at edge |
|---|---|---|---|---|---|
| 0 | 0.1 m | 37 m | 154 x 25.6 x 154 m | 77 m | 66 % |
| 1 | 0.2 m | 75 m | 307 x 51 x 307 m | 154 m | 42 % |
| 2 | 0.4 m | 150 m | 614 x 102 x 614 m | 307 m | 17 % |
| 3 | 0.8 m | 300 m | 1229 x 205 x 1229 m | 614 m | 2.7 % |

  Each level starts about where its voxel becomes one pixel, so the swap is sub-pixel. Fog
  (`density` 0.006 / m, start 8 m) hides the end only at level 3. The vertical extent matters as
  much as the horizontal: level 0 is 25.6 m tall and generator mountains reach 55 m.
- Today a ray leaving the window is a miss (`gbuffer_world.frag` discards, `lighting.frag` returns
  for sky), so **far terrain is new cost, not a saving**. Far pixels are about 6 % of the screen
  on flat ground and 30-50 % facing a hillside; every "by distance" saving scales with that share.
- GPU store: `chunkCapacity` 12 288 x 36 936 B = 433 MiB, already allocated; about 8100 slots
  used, about half of them unseen cave chunks (ROADMAP known cost 1).
- `TerrainGenerator` works in metres; `TerrainSettings::voxelSize = 0.2`, `caves = false` yields
  the same terrain at half resolution with no new generator code.
- Every `GpuInstance` enters the per-frame BVH (`Renderer.cpp:1146`); frustum culling affects
  only the draw list, so every shadow and AO ray can visit every pebble.

### Terrain cascades

**Data.** One `voxel::World` per level, sharing the `ChunkStore`. Level 1: `voxelSize = 0.2`,
same `windowSize`, `saveDirectory = save/"lod1"`, `maxJobsInFlight = 2`, generator without
caves. Estimated 1.4 dense chunks per column = about 3200 slots = 113 MiB of CPU memory, **no new
GPU memory** (free slots), grid +74 KiB. Slot use becomes about 11 300 of 12 288; the real count is
logged and is the gate for stage 2. Render space stays relative to the level-0 window origin.

**Alignment.** `WorldConfig::windowSnap` (default 1; 2 for every level that has a coarser one):
the window minimum is floored to a multiple of `snap` chunks, so the box of level L always ends on
chunk faces of level L+1 and the exit axis of L is a valid entry axis of L+1 (no epsilon).

**Trace** (`shared/trace.glsl`, compiled as GLSL and by `Trace.cpp`). Today's `VxTraceWorld` body
becomes `VxTraceGrid(level, ro, rd, tMin, tMax, startAxis, hit)`; `VxTraceChunk` and
`VxTraceInstances` are untouched; `VxTraceWorld` stays as `VxTraceGrid(0, ...)` so callers and
tests keep working. New `VxTraceWorldLod(ro, rd, tMin, tMax, minLevel, hit)`:

```
for level = minLevel .. VX_LOD_COUNT-1:
    if VxTraceGrid(level, from the exit of the previous level's box, exit axis) -> hit, hit.level = level
miss
```

A ray never re-enters a convex box, so no level is walked where a finer one was. `minLevel` lets
secondary rays of far pixels skip the fine levels (legal only because levels are filled).
`VxHit` gains `level`; the voxel hash is keyed on (absolute voxel at that level, level).

**Streaming holes (stage 3).** A level-0 cell that is unloaded or not uploaded is written as
`VX_GRID_PENDING` (0x40000000) instead of 0. `VxTraceGrid` then traces the matching octant of the
level-1 chunk: `VxTraceChunk` with `bmin` / `bmax` clipped to that 16^3 sub-box (a fine chunk face
is an integer coarse voxel coordinate, so the existing entry rule stays exact).

**Lighting of pixels beyond level 0.** One any-hit sun ray with `minLevel` = the pixel's level,
no instances, no AO ray, no point-light shadows, refresh period 4 (8 from level 2). Shadow rays of
near pixels still end at the level-0 box; continuing them upwards (mountain shadows, estimated
+0.3-1 ms on the 4.7 ms sun cost) is a flag, off by default.

**Edits.** Edits only happen in loaded level-0 chunks. `World::TakeEditedChunks()` (a second
list; `TakeChangedChunks` stays exclusive to `TerrainColliders`) feeds a pending set in the engine;
up to 4 chunks per frame are downsampled into the parent: coarse voxel solid if >= 4 of 8 children
are, id = most frequent solid child (ties: the upper one). 4096 writes, one 37 KiB upload. A chunk
stays pending until its parent cell is loaded; the parent's own `RegionStore` persists it, and
with more levels the parent is queued for its own parent. Known loss: 1-voxel walls and thin
trunks erode at level 1.

**Cost (estimates, UHD 620).** gbuffer +0.5-1.5 ms in open views (0 looking at near ground),
lighting +0.3-1 ms for level 1; initial fill 3200 chunks at 128 uploads per frame = 25 frames.
The level-1 `World::Update` must have `TakeChangedChunks()` drained every frame.

### Distance budgets (lighting and instances)

All in `lighting.frag` from the `t` already reconstructed per pixel, thresholds in a new
`vec4 lodParams`:

- **Refresh period.** The 8x8 tile checkerboard traces every 2nd frame today
  (`((px.x>>3)+(px.y>>3)+frame) & 1`). Period becomes 4 beyond 30 m: trace only if
  `((tx + 2*ty + frame) & 3) == 0`, and only once `count >= 12`. Disoccluded pixels (count 0)
  still trace every frame.
- **AO.** Fade the occlusion to unoccluded between 0.6 D and D, no ambient / emissive ray beyond
  D (default 50 m). Point-light shadow rays are skipped beyond D as well.
- **Saving** = f x (AO 5.9 ms) + f x (rest / 2) with f = share of lit pixels beyond the
  thresholds: about 0.3 ms on flat ground, 2-3 ms facing a hillside. Risk: far lighting reacts up
  to 4 frames late (explosion flash on a distant wall).
- **Shadow-BVH membership.** An instance casts if it is within 15 m of the camera or
  `boundsRadius / distance >= 0.008` (3 px: a 0.5 m piece stops casting at 62 m, a 3-voxel piece
  at 19 m). Partition `gpuInstances` casters-first **before** the instance upload and build the
  BVH over that prefix; leaf index = instance index stays valid, no shader change. Stateless, so
  a small shadow can pop at the threshold. Gain is unknown (the instance share of lighting was
  never isolated); add a BVH-leaf-visit counter first.
- **Sub-pixel draw culling.** Skip draw-list entries with `radius / distance < 0.0013`.

### Simulation by distance

- **Ground.** Already handled: `GuardBodies` freezes a dynamic body while any chunk it touches is
  unloaded, outside the window or without a collider; sleeping rubble left behind is parked for
  free. Addition: an entity with a `Lifetime` whose body has been frozen outside the level-0
  window for longer than 2 s is destroyed; authored entities stay frozen.
- **Debris threshold.** `Scene::SetViewPoint(pos)`; in `Scene::Damage` the piece threshold is
  `minPieceVoxels * clamp((d / 15 m)^2, 1, 16)`: with the 24 of `collapse.md`, 24 voxels up to
  15 m, 96 at 30 m, 384 from 60 m. Smaller pieces become particles as today.
- **Particles.** Effect counts (fx presets, the 400-voxel sample in `Engine::Explode`) scale by
  `clamp(20 m / d, 0.1, 1)^2`; the `SolidQuery` lambda at `Engine.cpp:234` returns false beyond
  30 m (saves one `World::IsSolidAt` per far particle per frame); 25 % of the pool is reserved
  for effects within 15 m so a far fight cannot starve a near cast.

## What changes in the engine

| File | Change |
|---|---|
| `shaders/lighting.frag` | Distance period, AO fade / cut, far-pixel path (stage 2) |
| `shaders/include/common.glsl`, `src/graphics/Frame.cppm` | `FrameUniforms` += `ivec4 lodMinChunk[4]` (w of [0] = level count), `vec4 lodOrigin[4]` (xyz = window minimum in render space, w = voxel size), `vec4 lodParams`: +144 B, `static_assert` becomes `3*64 + 30*16`; contract test updated. Stage 1 adds `lodParams` only |
| `shaders/shared/trace.glsl`, `src/voxel/internal/Trace.cpp` | `VX_GRID_ENTRY(l,i)`, `VX_GRID_MIN_CHUNK(l)`, `VX_GRID_ORIGIN(l)`, `VX_VOXEL_SIZE(l)`, `VX_LOD_COUNT`, `VX_GRID_PENDING`; `VxTraceGrid`, `VxTraceWorldLod`, `VxHit::level`; `TraceScene` gains per-level grids. `World::Raycast`, physics and gameplay queries stay level 0 |
| `shaders/gbuffer_world.frag` | Calls `VxTraceWorldLod`, level in the voxel hash |
| `src/voxel/World.cppm`, `internal/World.cpp` | `WorldConfig::windowSnap`, `TakeEditedChunks()`. Additions only |
| `src/graphics/Frame.cppm`, `internal/Renderer.cpp` | `FrameInput::lodWorlds` (span of `const World*`); grids concatenated in `u_Grid` (level * cells), one revision per level; caster partition; sub-pixel culling; `PENDING` instead of the "hide it" 0 |
| `src/engine/Engine.cppm`, `internal/Engine.cpp` | `EngineConfig::lodLevels` (0 = off) and `lodGeneratorFactory` (`std::function<std::shared_ptr<IWorldGenerator>(float voxelSize)>`, default: terrain generator without caves); LOD worlds update, edit downsample queue, particle query radius, `--lod N` |
| `src/scene/Scene.cppm`, `internal/Scene.cpp` | `SetViewPoint`, distance-scaled piece threshold, frozen-debris removal |
| `src/fx/internal/Effects.cpp` | Distance scale for preset counts, near reserve in the pool |

No existing exported signature changes; defaults keep every current test valid.

## Stages

1. **Distance budgets** (about 2 days). Lighting period and AO cut, caster prefix + leaf-visit
   counter, sub-pixel culling, distance-scaled piece threshold, particle scaling and query radius.
   *Verify*: pass timers with the camera facing a hillside and on flat ground, before / after
   (target: lighting -2 ms on the hillside, no change in a near-only view); unit tests: a
   3-voxel piece at 60 m is outside the BVH prefix, `Damage` at 60 m creates no entity below 384
   voxels; headless and editor runs stay validation-clean with "Beam cut: ... 1 new object(s)".
2. **One terrain level** (1-1.5 weeks). Second `World`, `windowSnap`, level-indexed trace and
   uniforms, concatenated grid, far-pixel lighting, CPU twin, `--lod 0/1`.
   *Verify*: unit test: a ray leaving level 0 hits level 1 within one coarse voxel of a 0.1 m
   reference; logged dense level-1 chunk count <= 3500; screenshot at a mountain view with
   `--lod 0` and `--lod 1`; gbuffer + lighting <= +2.5 ms with `VX_DEBUG_STEPS` showing no steps in
   level 1 under level 0. If slots run out: 40 x 8 x 40 level-1 window, or ROADMAP memory item 1 first.
3. **Edits, persistence, holes.** `TakeEditedChunks`, downsample queue, `lod1` region store,
   one-off rebuild for old saves, `VX_GRID_PENDING` fallback.
   *Verify*: test: a crater carved in level 0 is present in level 1 after N frames and after a
   save / reload; a teleport (`LoadBlocking` off) shows coarse terrain instead of sky holes.
4. **Levels 2 and 3** (only if stage 2 measures well). Needs about 6400 more slots: do ROADMAP
   memory item 1 (skip unseen underground chunks, about -4000 slots) and then choose by measured
   slot count between the palette-compressed chunks ROADMAP already plans (helps every level) and
   B's 8264 B `LodChunk` (same `ChunkOccupancy`, one id per 2^3 group). Optional flag for sun rays
   continuing into coarser levels.
   *Verify*: total GPU memory <= today's 473 MiB; window edge invisible in fog at 614 m; frame
   time in an open view within +1 ms of stage 2.
5. **Multiplayer** (with networking). Server keeps no LOD grids but runs the same edit downsample
   into LOD region files and sends coarse deltas to far clients.

## Open questions for the game

- **How far must the player see?** 154 m (level 1, cheap) or the horizon at 600 m (levels 2-3,
  needs the memory work of stage 4)? Denser fog would hide the level-1 edge and make stage 4
  unnecessary.
- **Is +1-2.5 ms acceptable for doubled view distance** on the UHD 620, where a frame already
  costs 21 ms, or should `--lod` default to off on integrated GPUs?
- **Do distant caves and thin structures matter?** Coarse levels have no caves and erode 1-voxel
  walls; a player-built thin tower may vanish beyond 77 m.
- **Should far mountains cast shadows on the player** (estimated +0.3-1 ms), or is fog enough?
- **Objects beyond 77 m are not drawn.** Acceptable for creatures and loose props, or must
  something (bosses, flying objects, other players) be visible further out?
- **Late far lighting**: an explosion lights distant walls up to 4 frames late. Acceptable?
- **Debris at range**: pieces under 384 voxels (about 0.7 m) hit beyond 60 m become particles
  only. Is a fight ever watched from that far (ranged spells, sieges)?
