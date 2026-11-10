# vortex.voxel - maintenance notes

Scope: `src/voxel`, `shaders/shared`, `tests/core`. Read `CLAUDE.md` and `docs/ARCHITECTURE.md` (sections 4, 5) first;
nothing from them is repeated here. Paths are relative to `engine/`. Every statement was checked against the code.
Line numbers are as of engine commit `ec073a8`; if a number no longer matches, search for the symbol named next to it.

## 1. Map

| File | Owns |
|---|---|
| `src/voxel/Voxel.cppm` | Primary interface; re-exports every partition. A new partition must be added here. |
| `Chunk.cppm` (header-only) | `Chunk`, `ChunkOccupancy`, index maths, `VoxelToChunk/Local`, `ChunkToVoxel`. |
| `Store.cppm`, `internal/Store.cpp` | `ChunkStore`: slot pool, LIFO free list, dirty list, high-water mark. |
| `World.cppm`, `internal/World.cpp` | Window, cells, grid encoding, load jobs, edits, raycast, change list. |
| `Region.cppm`, `internal/Region.cpp` | Chunk RLE codec (`EncodeChunk`/`DecodeChunk`), `RegionStore` (`r.x.y.z.vxr`). |
| `Generator.cppm`, `internal/Generator.cpp` | `IWorldGenerator`, `TerrainGenerator` (height tiles, caves, trees), `FlatGenerator`. |
| `Volume.cppm`, `internal/Volume.cpp` | `VoxelVolume`: dense x/y/z array, the authoring form of objects. |
| `Model.cppm`, `internal/Model.cpp` | `VoxelModel`: volume + baked `ModelPart`s (one slot per non-empty 32^3 piece), `Revision()`. |
| `Instances.cppm`, `internal/Instances.cpp` | `GpuInstance` (224 B), `GpuBvhNode` (32 B), `BuildBvh`. |
| `Trace.cppm`, `internal/Trace.cpp` | `TraceScene`, `Trace`/`TraceWorld`/`TraceChunk`; `TraceKernel` includes `shared/trace.glsl`. |
| `Collider.cppm`, `internal/Collider.cpp` | `BuildColliderBoxes`, `ComputeMassProperties`, `CollisionFilter`, `BuildChunkCollisionMesh`. |
| `Connectivity.cppm`, `internal/Connectivity.cpp` | `LabelComponents`, `SplitComponents`, `ExtractFloatingIslands`. |
| `Material.cppm`, `internal/Material.cpp` | `Material`, `GpuMaterial`, `MaterialLibrary` (palettes), `RegisterDefaultWorldMaterials`. |
| `VoxFormat.cppm`, `internal/VoxFormat.cpp` | MagicaVoxel `.vox` read/write, `RegisterVoxPalette`. |
| `MeshImport.cppm`, `internal/MeshImport.cpp` | `VoxelizeTriangles` (SAT), interior fill, glTF import through tinygltf. |
| `internal/ThirdPartyImpl.cpp` | The one non-module TU with the tinygltf / stb implementations. |
| `shaders/shared/trace.glsl` | Traversal compiled as GLSL and C++: `VxTraceChunk/World/Instances/Scene`. |
| `shaders/shared/shading.glsl`, `cube.glsl`, `vx_cpp.h`, `vx_glsl.glsl` | Shared shading helpers, proxy-cube corners, and the two language shims (rules: `vx_cpp.h:22-27`). |
| `tests/core/*.cpp` | `VortexCoreTests`: Chunk (+ChunkStore), World (+Region, Terrain), Trace (+Bvh), Collider, Connectivity, VoxFormat, MeshImport. `ParticleTests.cpp` in the same binary belongs to `fx`, not to this module. |

Who compiles the shared files as C++ today: `trace.glsl` in `Trace.cpp`, `cube.glsl` in
`tests/graphics/ContractTests.cpp`. `shading.glsl` says it is C++-compatible but no C++ file includes it, so nothing
enforces that; only glslang checks it.

The mirrors of this module's GPU structs and the GLSL side of the `VX_*` accessors are in
`shaders/include/common.glsl` (`VxMaterial`, `VxInstance`, `VxBvhNode`, `VX_GRID_ENTRY`, `VX_OCC_WORD` ...).

## 2. Invariants

**Three voxel orders - never mix them.** `Chunk::ids` is block-ordered, `VoxelIndex = block*64 + local`
(`Chunk.cppm:51-57`). `VoxelVolume` is `x + sx*(y + sy*z)` (`Volume.cppm:28`). The world grid and cells are
`x + z*sx + y*sx*sz` (`World.cpp:89`, `World.cpp:143`, `trace.glsl:220`): those three sites change together.

**Chunk.** `occ` must equal what `RebuildOccupancy()` produces from `ids`. `Set()` and `Fill()` keep it; any other
write through `ids[]` must be followed by `RebuildOccupancy()`. Today's raw writers: generators (`LoadJob` rebuilds
for them, only for `Dense` results), `VoxelModel::Bake` and `DecodeChunk` (both rebuild themselves). `Get/Set/IsSolid`
do not bounds-check (0..31 only); `GetSafe` does. A `Chunk` is 36 KB: keep it on the heap (`LoadJob` uses a
`thread_local unique_ptr`), never as a local in recursive code. `OccupancyWord` reads the struct as little-endian uint32 pairs; the
`trace.glsl` offsets (`VX_OCC_BLOCK_OFFSET = 2`, `VX_OCC_VOXEL_OFFSET = 18`) and the `0x00330033u` 2^3-group mask
(`trace.glsl:160-162`) depend on the bit order `lx + ly*4 + lz*16`.

**ChunkStore** (`Store.cpp`). Main thread only; workers build a `Chunk` and the main thread `Adopt()`s it.
- `Alloc/Adopt` return `kInvalidSlot` when full (`Adopt` also for a null chunk): every caller must handle it. Both
  `MarkDirty` the new slot. Chunks are allocated per slot on demand; an unused slot costs one null pointer.
- The free list is LIFO: a freed slot is handed out by the very next `Alloc` (asserted in `ChunkTests`). Any stale
  reference to a freed slot (grid entry, `ModelPart`, `GpuInstance`) then shows another chunk's voxels.
- `Get(slot)` dereferences unchecked; a free slot is a null pointer. Use `TryGet`/`IsAllocated` for slots from outside.
  `TraceKernel::OccWord/VoxelId` (`Trace.cpp:23-24`) use `Get`: a trace through a stale slot crashes. Only
  `TraceChunk` checks `IsAllocated` first.
- `Free` neither removes the slot from the dirty list nor lowers `HighWaterMark()`: consumers of `TakeDirty()` must
  tolerate unallocated slots.
- Exactly one consumer drains `TakeDirty()` (the renderer, `Renderer.cpp:896`); a second drain steals uploads.
  Whoever edits a chunk in place must call `MarkDirty(slot)`.

**World** (`World.cpp`).
- The public interface is main-thread only. Workers run only `LoadJob` (`:236`), which touches only `World::Shared`
  (generator, `RegionStore`, `results` under `mutex`, atomic `inFlight`) and a `thread_local` scratch chunk. It must
  never touch `m_Cells`, `m_Grid` or the `ChunkStore`.
- Jobs hold `shared_ptr<Shared>`, so a `World` may die with jobs queued; generator and `RegionStore` live until the last
  job ends. `JobSystem` and `ChunkStore` are held by reference and must outlive the `World` (member order
  `Engine.cpp:50-55`: jobs, store, world).
- Order inside `Update` (`:104`): `Recenter` (only when the window minimum changed) -> `IntegrateResults` (budget
  `maxIntegrationsPerUpdate`) -> `SubmitJobs` (while `inFlight < m_MaxJobs`), nearest cells first.
- Cell lifecycle: `Unloaded -> Loading` (`SubmitJobs`) `-> Empty | Uniform | Dense` (`IntegrateResults`);
  `Empty/Uniform -> Dense` only through `Materialize` (edits); anything `-> Unloaded` through `UnloadCell`.
  A `Dense` cell owns its slot; it is freed in `UnloadCell` and `~World`, nowhere else. Window min = `focusChunk - size/2`.
- A result is accepted only if `assigned && coord == r.coord && generation == r.generation && state == Loading`
  (`:308`). `generation` is bumped in `UnloadCell` and whenever `Recenter` re-targets a cell. Keep all four checks.
- `m_Grid[i]`: `0` = air, unloaded or loading (the same to a ray); `slot + 1` = dense; `0x80000000 | id` = uniform.
  Only `SetGrid`/`UnloadCell` write it, bumping `m_GridRevision`; `Recenter` bumps it too. An in-place edit of a dense
  chunk does not bump it (only `MarkDirty`).
- `modified` = "differs from generator output": set by edits, cleared by `PersistCell` and by integrating a load. Only
  modified cells reach the `RegionStore`.
- `TakeChangedChunks()` lists chunks that loaded as Dense/Uniform, were edited, or were unloaded with content. It may
  hold coordinates no longer in the window; a chunk that loads `Empty` is not listed. Single consumer:
  `physics/internal/Terrain.cpp:566`.
- Units: world positions `double` metres; voxel coordinates `int` world voxels (floored, negatives work);
  `FillSphere/CarveSphere` take centre and radius in **voxels** and test voxel centres (`+0.5`) with `<= r^2`;
  `FillBox` is `[min, max)`.
- `Raycast`: origin `double` metres, `distance` metres, `voxel` in world voxels, world only (no instances); it
  normalises `direction` itself and misses before the first `Update`. `Trace()` needs a normalised direction and a
  `float` origin in render space (relative to `WindowOrigin()`); it returns world voxels for a world hit but
  **chunk-local** voxels of the hit part for an instance hit (`Trace.cpp:74`); add `ModelPart::offset` for model voxels.
- `MakeTraceScene()` is a snapshot: it copies `gridMinChunk` and points into `m_Grid`, and it has no instances or BVH
  (the caller fills those in). Build a new one after every `Update`; never keep one across frames.
- No code outside `src/voxel` and the tests calls `Trace`, `TraceChunk` or `MakeTraceScene` today (grep). The only
  CPU ray query the engine uses is `World::Raycast`. The instance path of the CPU tracer is covered by `TraceTests`
  only.

**Persistence** (`Region.cpp`).
- Chunk bytes: `[kind]`; uniform `+[id]`; dense `+` runs `[id u8][len u16 LE]` over the block-ordered ids. The format
  depends on `Chunk::VoxelIndex` order and on raw material ids.
- `DecodeChunk` rejects: unknown kind, a uniform chunk with id 0, a zero-length run, runs that do not add up to
  exactly 32768. `PersistCell` therefore writes an all-air chunk as `Empty`, never as uniform 0.
- Region file: `"VXR1"`, `u32 count` (<= 4096), then `[u16 local][u32 size][bytes]`; host endianness, no checksum.
  A region is 16^3 chunks, `local = x + y*16 + z*256`.
- `RegionStore` is thread safe (one mutex). `Save()` is memory only; `Flush()` writes temp file + rename. With an empty
  directory, edits still survive leaving and re-entering the window (kept in memory).
- Saved ids are world-palette indices handed out by `MaterialLibrary::Add` in call order (`Material.cpp:136-155`:
  grass = 1 ... bedrock = 20). Reordering or inserting inside `RegisterDefaultWorldMaterials` silently re-materials
  every existing save. Append only.

**Model / Instances.**
- `VoxelModel::Volume()` is the source of truth; parts are derived. Change voxels only through `SetVolume` (re-bakes).
- `Bake` first releases every part, so even a successful re-bake may get different slots and marks them all dirty.
  `Revision()` changes on `Bake`, `Unbake` and `SetVolume`. Cache derived data by revision, never by slot.
- `~VoxelModel` frees its slots into the store it was baked in: the `ChunkStore` must outlive every
  `shared_ptr<VoxelModel>`.
- `ModelPart`: `offset` in model voxels (multiple of 32), `boundsMin/Max` chunk-local `[min, max)`. All-empty pieces
  get no part. There is one `GpuInstance` per part, not per model.
- `GpuInstance`/`GpuBvhNode` sizes are `static_assert`ed and mirrored in GLSL (`VxInstance`, `VxBvhNode`);
  `boundsMinSlot.w` = slot, `boundsMaxPalette.w` = `palette * 256`.
- BVH: leaf `b != 0`, `a` = primitive; inner `b == 0`, children `a` and `a + 1`; one primitive per leaf; no primitives =
  no nodes. Node 0 is the root.
- `VoxelVolume`: `Get`/`Set` are bounds-checked (out-of-range reads give 0, writes are dropped silently); `Index` and
  `Data()` are not. `IsEmpty()` and `CountSolid()` scan the whole array. A negative size is clamped to 0.

**Materials.** Id 0 is air in every palette and cannot be set (`Material.cpp:71`). GPU index = `palette * 256 + id`.
`MaterialLibrary` is not thread safe and has no removal API. `Revision()` bumps on every `Set`/`CreatePalette`.
`CarveSphere` and `ExtractFloatingIslands` always look up `kWorldPalette`.

**Shared GLSL.** `trace.glsl` is included *inside* `struct TraceKernel` (`Trace.cpp:20-44`); data is reached only through
the `VX_*` accessor macros defined there and, with the same names, in the shaders.

## 3. Traps

- **Edits never free a slot.** A chunk carved completely empty stays `Dense` until it leaves the window; `PersistCell`
  (`:167`) demotes it to Empty/Uniform only in the saved bytes. Demoting inside `EditRegion` would also have to handle
  the pending GPU upload, the grid entry and the terrain collider of that chunk.
- **Store exhaustion turns terrain into `Empty` for that visit** (`:323-331`): not retried, not saved; the warning is
  function-static, once per process. If the player then edits such a cell it is materialised from *empty*, marked
  modified and saved - the generated terrain of that chunk is lost for good (read from the code, no test).
- `SetVoxel` returns `true` when the voxel already has that id, `false` for an unloaded chunk or a full store.
  `EditRegion` silently skips `Unloaded`/`Loading` chunks; compare the returned count.
- `CarveSphere`: a voxel survives when `hardness > power` (equal is removed) or it is `Indestructible`. Water and lava
  have hardness 0 and are protected only by the flag. With `materials == nullptr` everything is removed.
- `World::Save()` has exactly two callers: the editor (`Editor.cpp:833`) and `~World` when `saveDirectory` is set.
  There is no autosave: a crash loses all edits since the last save. `Save()` writes every dirty region file on the
  calling (main) thread.
- A region file with a bad header is ignored with a warning; a truncated one keeps the chunks read before the break
  (`Region.cpp:122-147`; a break inside a chunk body gives no warning). Either way the next `Flush` of that region
  overwrites the file with only what is in memory. A corrupt single chunk is regenerated (`World.cpp:250`).
- `RegionStore::Load` reads the region file from disk while holding the mutex and `Flush` writes under it: the main
  thread's `PersistCell -> Save` can stall behind a worker's disk read. Regions are never evicted from memory.
- `TerrainSettings::voxelSize` and `WorldConfig::voxelSize` are separate fields and nothing checks that they agree (the
  engine copies one into the other, `Engine.cpp:96`).
- `IWorldGenerator::Generate` runs on workers, concurrently, repeatedly for the same chunk. The terrain tile cache is
  mutex-guarded and cleared at 4096 tiles (`Generator.cpp:160-163`). Generators write `ids` only; `LoadJob` rebuilds
  the pyramid and demotes trivial Dense results; a `Uniform` result with id 0 becomes `Empty`.
- Trees cross chunk borders only because every chunk re-derives the trees of nearby grid cells within
  `kMaxCanopyRadius` (2.6 m), and the Empty early-out adds `kMaxTreeHeight` (7.5 m). Larger trunks or canopies than
  these constants get cut at chunk borders.
- Ray limits fail silently as a miss: `VX_MAX_GRID_STEPS = 512`, `VX_MAX_CHUNK_STEPS = 192`, BVH stack 32 (children are
  dropped when it is full, `trace.glsl:285`). A ray that leaves the window misses.
- Deliberate: a ray starting inside solid reports `t = tMin` and a normal opposing its dominant axis (`VxFaceNormal`,
  axis -1); a uniform chunk is hit as a box with `slot = VX_INVALID`; `VxSafeDirection` nudges zero components to 1e-6
  instead of branching. Do not "fix" any of the three. Consequence for callers: `RayHit::slot` is `kInvalidSlot` on a
  valid world hit in a uniform chunk, so check it before `store.Get(hit.slot)`.
- `VoxelModel::SetVolume` on a baked model ignores `Bake`'s result: with a full store the model silently becomes
  unbaked (`IsBaked() == false`, only a `Log::Error`). Callers must re-check.
- `MaterialLibrary::Add` returns 0 (= air) when the palette is full or invalid. `Get` returns a reference into a vector
  that `CreatePalette` reallocates: do not hold it across palette creation. `Get` with a bad palette returns a default
  `Material` (density 1000), not an error. Unused entries are magenta on purpose (`Material.cpp:57-61`).
- `.vox`: ids are the file's colour indices; RGBA entry `i` colours id `i + 1`. Import maps (x, y, z) ->
  (x, z, sizeY-1-y) and translations to (x, z, -y). Only the first frame's `_t` is read; rotations (`_r`) are ignored.
  `WriteVox` writes no scene graph (names and translations are lost) and crops models to 256 per axis.
- glTF: voxel id = material index + 1, clamped to 255; primitives without a material also get id 1. Only the colour is
  converted (base colour factor x average of the whole texture). Only TRIANGLES with float VEC3 positions. The first
  triangle to claim a voxel wins; interior voxels take the id of the nearest solid voxel towards -X.
  `FillInterior` allocates `(size + 2)^3` bytes on top of the volume (about 270 MB together at 512^3).
- `ExtractFloatingIslands` refuses regions over 192 voxels per axis and returns nothing if any chunk of the region is
  not loaded. A component touching the region boundary counts as attached - the safety that stops it ripping out
  hillsides; do not relax it. Ignored (liquid) voxels neither support nor belong to an island and stay in the world.
- `BuildColliderBoxes`: with `mergeMaterials = true`, `box.material` is only the first voxel's id; mass code uses
  `false` (`Scene.cpp:41`). `ComputeMassProperties` returns the centre of mass in voxel units from the volume origin;
  negative densities count as 0, and a volume of weightless materials gives `mass == 0` with the geometric centre, so
  callers must handle zero mass.
- `BuildChunkCollisionMesh`: vertices are chunk-local voxel units, triangles counter-clockwise seen from the empty
  side. With a null `outside` every border face is emitted (the neighbour counts as open).
  `CollisionFilter::FromMaterials` with an unknown palette returns "everything collides" plus a warning.
- Connectivity is 6-connected: pieces touching only by an edge or a corner are separate components, and different
  materials still connect (`Connectivity.CubesTouchingOnlyByAnEdgeOrCornerAreSeparate`, `.DifferentMaterialsStillConnect`).
  Changing this changes what every destruction produces.
- `MaterialFlag_Flammable` and `MaterialFlag_Foliage` are set on the default materials but read by no code
  (grep over `engine/src`, `engine/shaders`, `src/`). The flags with behaviour are `Liquid` (`Material.cpp` `Pack`,
  `physics/internal/Terrain.cpp:204`, default `ignoreFlags` of `ExtractFloatingIslands`) and `Indestructible`
  (`CarveSphere`, `scene/internal/Scene.cpp:341`). Do not assume foliage or fire behaviour exists.

## 4. Recipes

**A. Add a world material.** Append to `WorldMaterials` (`Material.cppm`) and to the *end* of
`RegisterDefaultWorldMaterials` (`Material.cpp`); use it in `Generator.cpp` through `m_S.materials`. Flags decide
behaviour: `Liquid` (no terrain collision, liquid shading via `GpuMaterial::params.w`) and `Indestructible`;
`Flammable` and `Foliage` do nothing yet (see traps). A new `GpuMaterial` field or `params.w` value also needs the
shader side (`VxMaterial`, `VX_MATERIAL_*` in `shaders/include/common.glsl`). Tests:
`CollisionFilter.FromMaterialsExcludesFlaggedMaterials`, `World.CarveRespectsHardness`.

**B. Add a world edit operation** (capsule, replace-material ...). Write it as a lambda over `EditRegion`
(`World.cpp:451`): return the new id, or `-1` to leave the voxel. `EditRegion` already materialises, calls `MarkDirty`,
sets `modified`, queues the change and fills the `VoxelEdit` list; do not duplicate that. Declare it in `World.cppm`.
Test next to `World.EditsMaterializeAndReportChanges`: count, old/new ids, `TakeChangedChunks`, an edit across a chunk
border, an edit on an unloaded chunk.

**C. Change or add a generator.** Implement `IWorldGenerator` in `Generator.cpp` (anonymous namespace, factory declared
in `Generator.cppm`): a pure function of (seed, coord), thread safe, writing only `chunk.ids` via `Chunk::VoxelIndex`.
Return `Empty`/`Uniform` early where possible (no slot). Anything crossing chunk borders must be derivable by each chunk
on its own. Extend `Terrain.GenerationIsDeterministicAndSeamless` and `Terrain.StreamsThroughTheWorld`. Changing an
existing generator does not touch saved (modified) chunks, so old saves show seams.

**D. Change the chunk layout or grid encoding.** Touch together: `Chunk.cppm`, `trace.glsl` (offsets, index maths),
`World.cpp` (`CellIndex`, `Recenter`, `SetGrid`), `Region.cpp` (change the `VXR1` magic so old files are rejected - the
codec is order-dependent), the renderer's upload sizes, `ARCHITECTURE.md` 4.1/4.3. Contract tests:
`Chunk.LayoutMatchesGpuContract`, `Chunk.OccupancyWordsMatchBits`, `World.GridEncodingMatchesShaderContract`,
`Trace.SingleChunkMatchesReferenceDDA`, `Region.ChunkCodecRoundTrip`.

**E. Change the traversal** (`trace.glsl`). Stay in the shared subset; it must compile as C++ and as GLSL. Run `Trace.*`,
`Bvh.*`, `World.RaycastHitsGroundAndMatchesReference`, `World.WindowSizeNeedNotBePowerOfTwo`, then both headless runs
with `--validation`, and compare screenshots from a fixed `--camera` before and after. `RayHit::steps` and
`Trace.EmptySpaceSkippingIsEffective` guard the cost.

## 5. Verifying a change

Binary: `build/engine/tests/VortexCoreTests` (gtest; ctest discovers the tests as `Suite.Name`).

```
ctest --test-dir build -R '^(Chunk|ChunkStore|World|Region|Terrain|Trace|Bvh)\.'
ctest --test-dir build -R '^(ColliderBoxes|MassProperties|CollisionFilter|ChunkCollisionMesh)\.'
ctest --test-dir build -R '^(Connectivity|SplitComponents|FloatingIslands|VoxFormat|Voxelize|MeshImport)\.'
build/engine/tests/VortexCoreTests --gtest_filter='World.*'      # one suite, full output
```

| Area | Tests that pin it |
|---|---|
| Store / lifetime | `ChunkStore.AllocFreeAndDirtyTracking`, `World.WindowMovementKeepsEditsAndFreesMemory`, `World.StoreExhaustionDegradesGracefully` |
| Window / streaming | `World.LoadsWindowAndClassifiesChunks`, `World.SmallStepsOnlyReloadTheNewSlab`, `World.WindowSizeNeedNotBePowerOfTwo` |
| Persistence | `World.PersistsEditsToDisk`, `Region.ChunkCodecRoundTrip` |

Then the whole suite (scene, physics terrain and the renderer contract tests consume these types) and both headless
runs from CLAUDE.md. In the headless game log:
- `World: N dense, N uniform, N empty chunks; store a/b` (`Engine.cpp:288`, printed only when `exitAfterFrames != 0`): `a` must
  stay below `b` (12 288 in the game, `src/main.cpp:472`; the `EngineConfig` default is 8192). `a == b` or
  `ChunkStore is full` means missing terrain.
- None of: `Saved chunk ... is corrupt`, `Region file ... is corrupt`, `Region file ... is truncated`,
  `VoxelModel ...: chunk store is full`,
  `Renderer: chunk slot ... is beyond the GPU chunk capacity`.
- `Beam cut: ... 1 new object(s)` (split + re-bake path) and `Player feet at ..., on the ground` (terrain collision fed
  by `TakeChangedChunks`) still appear.
- Screenshot: magenta voxels = bad material id or palette; chunk-sized (3.2 m) holes in terrain = slots exhausted or a
  chunk never uploaded; an object showing the wrong shape or terrain voxels = stale slot after a re-bake.
