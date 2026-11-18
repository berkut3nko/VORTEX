# vortex.voxel - maintenance notes

Checked against engine `430fde0`.

Scope: `src/voxel`, `shaders/shared`, `tests/core`. Read `CLAUDE.md` and `docs/ARCHITECTURE.md` (sections 4, 5)
first; nothing from them is repeated here. Paths are relative to `engine/`. Code is cited by file and symbol; search
for the symbol. The text before this rewrite, with its line numbers and measured costs, is
`docs/history/notes/voxel-2026-11-18.md`.

## 1. Map

| File | Owns |
|---|---|
| `src/voxel/Voxel.cppm` | Primary interface; re-exports every partition. A new partition must be added here. |
| `Chunk.cppm` (header-only) | `Chunk`, `ChunkOccupancy`, index maths, `VoxelToChunk/Local`, `ChunkToVoxel`, `kDefaultVoxelSize`. |
| `Store.cppm`, `internal/Store.cpp` | `ChunkStore`: slot pool, LIFO free list, dirty list, high-water mark. |
| `World.cppm`, `internal/World.cpp` | Window, cells, grid encoding, load jobs, edits, raycast, change list, revisions. Quantised edits [net-2]: `SphereQ`, `QuantizeSphere`, `QuantizePower`, `InSphereQ`, `SphereVoxelBounds`, `EditReport`, `CarveSphereQ` / `FillSphereQ` / `SwapSphereQ` / `ClearMask`. Chunk sync calls [net-7a]: `ChunkClip`, `EncodeChunkAt`, `SetChunkEncoded`, `TakeLoadedChunks`, `TakeWindowSteps`; install modes [net-7b]. |
| `Hash.cppm`, `internal/Hash.cpp` | `ChunkHash`, `UniformChunkHash`, `kAirChunkHash`, `WorldHashTerm`, `WorldHash`, `ChunkHashAt` (one chunk of a world in any cell state, the per-cell part of `WorldHash`; what a HashCheck compares [net-7b]), `HashMix64`, `MaterialTableHash` (what replicated decisions read of the world palette) and `kGeneratorGoldenHash` (a hash of the golden tables in `tests/core/HashTests.cpp`; both in `Hello::buildHash`) [net-3]. |
| `Region.cppm`, `internal/Region.cpp` | Chunk RLE codec (`EncodeChunk` / `DecodeChunk`), `RegionStore` (`r.x.y.z.vxr`). |
| `Generator.cppm`, `internal/Generator.cpp` | `IWorldGenerator`, `TerrainGenerator` (height tiles, caves, trees), `FlatGenerator`. |
| `Volume.cppm`, `internal/Volume.cpp` | `VoxelVolume`: dense x/y/z array, the authoring form of objects. `VoxelMask`: one bit per voxel of a box, packed 8 per byte in `VoxelVolume::Index` order (the `Detach` / `ClearMask` mask); `AnyInRange` tests a run of bits, `SetRange` sets one (the wire decoder). |
| `Model.cppm`, `internal/Model.cpp` | `VoxelModel`: volume + baked `ModelPart`s (one slot per non-empty 32^3 piece), `Revision()`. |
| `Instances.cppm`, `internal/Instances.cpp` | `GpuInstance` (224 B), `GpuBvhNode` (32 B), `BuildBvh`. |
| `Trace.cppm`, `internal/Trace.cpp` | `TraceScene`, `Trace` / `TraceWorld` / `TraceChunk`; `TraceKernel` includes `shared/trace.glsl`. |
| `Collider.cppm`, `internal/Collider.cpp` | `BuildColliderBoxes`, `ComputeMassProperties`, `CollisionFilter`, `BuildChunkCollisionMesh`. |
| `Connectivity.cppm`, `internal/Connectivity.cpp` | `LabelComponents`, `SplitComponents`, `ExtractFloatingIslands` (+ `IslandBoxScan`), `ProveFaceComponents` (the face flood [shred-1]; `FaceComponent`, `FloodLimits`, `FloodResult`, `kFloodHalfExtent`). |
| `Structure.cppm`, `internal/Structure.cpp` | The cell graph [shred-4]: `GraphNode`, `GraphLink`, `ChunkGraph`, `BondVoxels`, `GraphMaterials` / `MakeGraphMaterials`, `BuildChunkGraph`, `NodeVoxels`, `TerrainGraphCache`, `SearchIslandsOnGraph` (+ `GraphSearchLimits`, `GraphSearchResult`), `kCellSize`, `kMaxCellNodes`. The load check [shred-4b] (shred.md Part 4b): `StrengthClass` / `StrengthClassOf`, `kNoCut`, `kCutAnchorBit`, `NeckLimits` / `MakeNeckLimits` / `VoxelSizeMicro`, `NeckCut`, `NeckNetwork` / `SolveNeckNetwork` (the rule on an abstract network, for tests), the `NeckCheck` phase inside the search. |
| `Material.cppm`, `internal/Material.cpp` | `Material`, `GpuMaterial`, `MaterialLibrary` (palettes), `RegisterDefaultWorldMaterials`. |
| `VoxFormat.cppm`, `internal/VoxFormat.cpp` | MagicaVoxel `.vox` read/write, `RegisterVoxPalette`. |
| `MeshImport.cppm`, `internal/MeshImport.cpp` | `VoxelizeTriangles` (SAT), interior fill, glTF import through tinygltf. |
| `internal/ThirdPartyImpl.cpp` | The one non-module TU with the tinygltf / stb implementations. |
| `shaders/shared/trace.glsl` | Traversal compiled as GLSL and C++: `VxTraceChunk/World/Instances/Scene`. |
| `shaders/shared/shading.glsl`, `cube.glsl`, `vx_cpp.h`, `vx_glsl.glsl` | Shared shading helpers, proxy-cube corners, and the two language shims (the rules are the comment at the top of `vx_cpp.h`). |
| `tests/core/*.cpp` | `VortexCoreTests`: Chunk (+ChunkStore), World (+Region, Terrain), WorldOps (`QuantizedEdits.*`), Hash (+golden terrain), Trace (+Bvh), Collider, Connectivity, VoxFormat, MeshImport, ViewDistance (with `FloatingIslands.GatherByChunkEqualsPerVoxel` and `.RowsAboveTheBuildCeilingAreAir`, which need its terrain fixtures), Structure (`Structure.*`: graph = `LabelComponents`, cache refresh = rebuild, graph search vs the voxel search, `DISABLED_GraphBuildCost`), Neck (`NeckTests.cpp`, `Neck.*` and `Structure.NodePlaneCutWeakestMaterialAndAnchorBit` / `.FoliageCarriesNoLoad`: brute-force max-flow, a per-voxel reference network, the watch tower on a neck at every height mod 8, thresholds, natural cases, the terrain gate, budgets), ChunkSyncWorld (`ChunkSyncWorldTests.cpp`: the world side of chunk sync and install modes). `ParticleTests.cpp` and `HudTests.cpp` in the same binary belong to `fx` and `core`, not to this module. |

Who compiles the shared files as C++ today: `trace.glsl` in `Trace.cpp`, `cube.glsl` in
`tests/graphics/ContractTests.cpp`. `shading.glsl` says it is C++-compatible but no C++ file includes it, so nothing
enforces that; only glslang checks it.

The mirrors of this module's GPU structs and the GLSL side of the `VX_*` accessors are in
`shaders/include/common.glsl` (`VxMaterial`, `VxInstance`, `VxBvhNode`, `VX_GRID_ENTRY`, `VX_OCC_WORD` ...).

## 2. Invariants

**Three voxel orders - never mix them.** `Chunk::ids` is block-ordered, `VoxelIndex = block*64 + local`
(`Chunk::VoxelIndex`). `VoxelVolume` is `x + sx*(y + sy*z)` (`VoxelVolume::Index`). The world grid and cells are
column-major `(x + z*sx) * sy + y` [vd-1] (`World::CellIndex`, the index arithmetic in `World.cpp` - `ci * sy + wy` in
`Recenter`, `RetargetColumn`, `ApplyColumnClass`, `SubmitColumnRows`, `WriteGrid`, `UpdateColumnTop`,
`IntegrateResults` - and `VxTraceWorld` in `trace.glsl`): those sites change together. Column indices (`x + z*sx`,
toroidal) and region indices (window-relative 4 x 4 columns) are separate orders.

**Chunk.** `occ` must equal what `RebuildOccupancy()` produces from `ids`. `Set()` and `Fill()` keep it; any other
write through `ids[]` must be followed by `RebuildOccupancy()`. Today's raw writers: generators (`LoadJob` rebuilds
for them, only for `Dense` results), `VoxelModel::Bake` and `DecodeChunk` (both rebuild themselves). `Get/Set/IsSolid`
do not bounds-check (0..31 only); `GetSafe` does. A `Chunk` is 36 KB: keep it on the heap (`LoadJob` uses a
`thread_local unique_ptr`), never as a local in recursive code. `OccupancyWord` reads the struct as little-endian
uint32 pairs; the `trace.glsl` offsets (`VX_OCC_BLOCK_OFFSET = 2`, `VX_OCC_VOXEL_OFFSET = 18`) and the `0x00330033u`
2^3-group mask in `trace.glsl` depend on the bit order `lx + ly*4 + lz*16`.

**ChunkStore** (`Store.cpp`). Main thread only; workers build a `Chunk` and the main thread `Adopt()`s it.
- `Alloc/Adopt` return `kInvalidSlot` when full (`Adopt` also for a null chunk): every caller must handle it. Both
  `MarkDirty` the new slot. Chunks are allocated per slot on demand; an unused slot costs one null pointer.
- The free list is LIFO: a freed slot is handed out by the very next `Alloc` (asserted in `ChunkTests`). Any stale
  reference to a freed slot (grid entry, `ModelPart`, `GpuInstance`) then shows another chunk's voxels.
- `Get(slot)` dereferences unchecked; a free slot is a null pointer. Use `TryGet` / `IsAllocated` for slots from
  outside. `TraceKernel::OccWord` / `VoxelId` (`Trace.cpp`) use `Get`: a trace through a stale slot crashes. Only
  `TraceChunk` checks `IsAllocated` first.
- `Free` neither removes the slot from the dirty list nor lowers `HighWaterMark()`: consumers of `TakeDirty()` must
  tolerate unallocated slots.
- Exactly one consumer drains `TakeDirty()` (the renderer, `SyncVoxelData` in `Renderer.cpp`); a second drain steals
  uploads. Whoever edits a chunk in place must call `MarkDirty(slot)`.

**World** (`World.cpp`).
- The public interface is main-thread only. Workers run only `World::LoadJob`, which touches only `World::Shared`
  (generator, `RegionStore`, `results` under `mutex`, atomic `inFlight`) and a `thread_local` scratch chunk. It must
  never touch `m_Cells`, `m_Grid` or the `ChunkStore`.
- Jobs hold `shared_ptr<Shared>`, so a `World` may die with jobs queued; generator and `RegionStore` live until the
  last job ends. `JobSystem` and `ChunkStore` are held by reference and must outlive the `World` (member order of
  `sim::Simulation::Impl`: jobs, store, world).
- Order inside `Update`: `Recenter` (only when the window minimum changed; touches only re-targeted columns, then
  `Rebucket` + `RebuildRegionTops`) -> `IntegrateResults` (column results first, then chunk results; the budget
  `maxIntegrationsPerUpdate` counts Dense results only) -> `PromoteNear` (Packed cells within `nearRadius` + a quarter
  chunk, scanned whenever the focus moved a quarter chunk since the last scan) -> `ProcessReveals` (Packed cells found
  visible; the two share `maxDecodesPerUpdate`) -> `Repack` (a round-robin share of `m_Resident`) -> `SubmitJobs`
  (while `inFlight < m_MaxJobs`: near sphere, then `RequestLoad` boxes, then classification + surface band by
  horizontal distance, then caves) -> `RefreshContentTop` (lazy lowered tops). `LoadBlocking` runs the same steps
  without budgets and also waits for `m_Reveal` to drain.
- **The job cap is per frame.** Jobs are submitted once per `Update`, so `WorldConfig::maxJobsInFlight` (0 = the
  default, 8 per worker thread, at least 2) is what a frame can start. 2 per worker starved streaming while climbing
  at 10 m/s; 8 per worker keeps up with 25 m/s flight (CLAUDE.md lessons). Lowering it shows in gate G5 (`--climb`)
  and in `--fly`'s unfilled columns.
- Load surfaces before caves (the `SubmitJobs` order): distance-first loading made the game start far slower
  (CLAUDE.md lessons).
- Column lifecycle: `None -> Classifying` (`SubmitColumnJob`) `-> Known` (`ApplyColumnClass`: rows outside the band
  become Empty / Uniform without a job unless saved; band rows go to the column's `surfaceRows` / `caveRows` bit masks,
  window-relative, and the grid gets `kGridPending` in full height). With `packHidden` a column's band jobs wait until
  its in-window 3 x 3 neighbours are Known (the pack decision reads them, `NeighboursKnown`), and `ApplyColumnClass`
  re-judges the Packed cells of its 8 neighbours (`RecheckNeighbours`, `Visible(cell, false)`): a chunk at the
  window's edge was packed without its neighbour outside the window.
- Cell lifecycle: `Unloaded -> Loading` (`SubmitCellJob`) `-> Empty | Uniform | Dense | Packed` (`IntegrateResults`;
  or `Unloaded -> Empty | Uniform` in `PlaceCell`); `Empty/Uniform/Packed -> Dense` only through `Materialize` /
  `Unpack` (edits, `PromoteNear`, `ProcessReveals`, `PromoteFaceNeighbours`, `EnsureResident`); `Dense -> Packed` only
  through `Repack` (and a full store at load, below); anything `-> Unloaded` through `UnloadCell` (or the sky fast
  path of `RetargetCell`). A `Dense` cell owns its slot; it is freed in `UnloadCell`, `Repack` and `~World`, nowhere
  else. `m_Packed[index]` holds the runs of a `Packed` cell, or of a resident buried chunk that may be packed again
  (generated, unmodified, listed in `m_Resident`); `DropKeptBytes` frees them (unload, first edit, a saved chunk). A
  `Packed` cell is never `modified`; it is `fromSave` only when the store was full at its load. Window min =
  `focusChunk - size/2`, except y in full height (fixed at construction).
- A result is accepted only if `assigned && coord == r.coord && generation == r.generation && state == Loading`
  (`IntegrateResults`). `generation` is bumped in `UnloadCell` and whenever `Recenter` re-targets a cell. Keep all
  four checks.
- `m_Grid[i]`: `0` = air, unloaded or loading (the same to a ray), `0x80000000` = pending band cell (full height only;
  a ray stops there), `slot + 1` = dense; `0x80000000 | id` = uniform or packed. Cells are written only through
  `WriteGrid` (via `SetGrid`, `UnloadCell`, `PlaceCell`), which bumps `m_GridRevision`, the page revision and the
  column top. Column tops: raised at once (column, content and region top), lowered by a search down the column and
  lazily for content / region tops (`m_ContentTopStale`, `m_RegionTopsStale`). A top too high only costs steps; one
  too low would skip content - never write a cell without `WriteGrid`. An in-place edit of a dense chunk does not bump
  anything (only `MarkDirty`). `WriteGrid` also records `m_Fallback[i]`, the word a cell had before it became dense
  (`GridFallback`, what the renderer shows until the slot is uploaded).
- Counters (`WorldStats`): `unloadsInsideRadius` counts re-targeted cells with content whose column is within
  `ViewRadius()` of the focus horizontally (`RetargetCell`, by distance, whatever moved the window);
  `retargetedColumns`, `reveals`, `repacks`; `storeFullLoads` (a Dense result installed Packed for want of a slot)
  and `storeFullDecodes` (a decode refused) must stay 0. `generatedChunks` / `generatedDense` / `generateMs` are
  counted by the worker in `LoadJob` (atomics in `Shared`, saved chunks not counted, stale results counted: it is
  generation work) and read by `Stats()`.
- `modified` = "differs from generator output": set by edits, cleared by `PersistCell` and by integrating a load.
  Only modified cells reach the `RegionStore`.
- `TakeChunkChanges()` lists chunks that loaded as Dense / Uniform / Packed, were edited, or were unloaded with
  content (not a pack / unpack), each once (`Cell::changeQueued` / `changeIndex` point at its entry). It may hold
  coordinates no longer in the window; a chunk that loads `Empty` is not listed. Each entry carries the box (world
  voxels) of the voxels `EditRegion` changed in that chunk since the last take - empty for a load / unload; an entry
  queued before its cell is unloaded keeps the old coordinate and box [shred-1d] (physics needs to know where an old
  collider is stale; `World.ChunkChangesCarryTheBoxOfTheEditedVoxels`). `TakeChangedChunks()` is the same list as
  coordinates and empties it too. Single consumer: `TerrainColliders::Impl::ConsumeChanges`.
- Units: world positions `double` metres; voxel coordinates `int` world voxels (floored, negatives work);
  `FillSphere` / `CarveSphere` take centre and radius in **voxels** and test voxel centres (`+0.5`) with `<= r^2`;
  `FillBox` is `[min, max)`.
- `Raycast`: origin `double` metres, `distance` metres, `voxel` in world voxels, world only (no instances); it
  normalises `direction` itself and misses before the first `Update`. `Trace()` needs a normalised direction and a
  `float` origin in render space (relative to `WindowOrigin()`); it returns world voxels for a world hit but
  **chunk-local** voxels of the hit part for an instance hit (`Trace` in `Trace.cpp`); add `ModelPart::offset` for
  model voxels.
- `MakeTraceScene()` is a snapshot: it copies `gridMinChunk` and points into `m_Grid`, and it has no instances or BVH
  (the caller fills those in). Build a new one after every `Update`; never keep one across frames.
- No code outside `src/voxel` and the tests calls `Trace`, `TraceChunk` or `MakeTraceScene` (grep). The only CPU ray
  query the engine uses is `World::Raycast`. The instance path of the CPU tracer is covered by `TraceTests` only.

**Full height and Packed** [vd-1] (`docs/design/view-distance.md`).
- `ClassifyColumn` and `Generate` must agree for every row outside the band
  (`ViewDistance.ClassifyColumnAgreesWithGenerate`); the terrain generator's `Generate` calls `Classify` itself.
  `HeightMeters` clamps to `TerrainSurfaceBounds` (no sampled height of the goldens reaches the bounds, so the goldens
  did not change).
- Packing is exact: `ViewDistance.PackedAndUnpackedWorldsTakeTheSameEdits` (WorldHash, edit lists, GetVoxel,
  ReadChunk). An edit reads a Packed chunk from `m_Scratch` and makes it resident only at its first change, so the
  `unsynced` rule is the same as for Empty / Uniform. Keep the face-neighbour promotion after edits
  (`PromoteFaceNeighbours`): a ray leaving a changed chunk enters a neighbour, and a Packed neighbour would show as a
  solid box.
- The visibility rule (`Visible`) reads `surfaceFrom + 1` (the row of the lowest surface voxel) of the 3 x 3 columns;
  a column outside the window is ignored (re-checked when it is classified), an unclassified one makes the chunk
  visible (or is ignored, in the re-check). A chunk with a face neighbour that is `modified` or `fromSave` is visible
  too (a crater wall, `FaceNeighbourEdited`); a `fromSave` result promotes its Packed face neighbours when it loads, so
  the order in which a crater and its neighbours load does not matter.
- Near sphere: everything within `nearRadius` of the focus is resident. `PromoteNear` scans `nearRadius + h` whenever
  the focus moved more than `h` (a quarter chunk) since its last scan; a load result is packed only beyond
  `nearRadius + 2h` (`NearSlack()`). Between scans the focus stays within `h` of the last one and within `2h` of where
  anything was packed since. `Repack` packs again beyond `nearRadius + repackMargin` (8 m): hysteresis.
- Store full: a Dense result that finds no slot is installed Packed (its runs, encoded on the main thread when the job
  did not; drawn as a rock stand-in; `storeFullLoads`, one function-static warning `ChunkStore is full` per process),
  never Empty: a saved chunk would vanish and the next edit would overwrite the save
  (`ViewDistance.AFullStoreNeverDropsASavedChunk`). `ChunkStore::Adopt` destroys the chunk when it fails: test for a
  free slot first.
- `RequestLoad(min, max)`: up to 16 boxes loaded right after the near sphere (columns classified, neighbours too when
  packing); a box goes once every chunk of it in the window is loaded. `sim::Blast` uses it for an island search it
  could not run (below).
- Build ceiling: `EditRegion` evaluates `decide(v, 0)` for the part of the box above the top row; any solid result
  refuses the whole edit (`outOfBounds`). The test is in integers and needs no loaded chunk.
- `World::Raycast` returns no hit at a pending cell (material 0); `Trace` / `TraceWorld` return it as a hit with
  material 0. Callers of the latter must check the material.
- `GetVoxel` on a Packed chunk walks its runs (O(runs)); bulk readers use `ReadChunk`.

**Persistence** (`Region.cpp`).
- Chunk bytes: `[kind]`; uniform `+[id]`; dense `+` runs `[id u8][len u16 LE]` over the block-ordered ids. The format
  depends on `Chunk::VoxelIndex` order and on raw material ids.
- `DecodeChunk` rejects: unknown kind, a uniform chunk with id 0, a zero-length run, runs that do not add up to
  exactly 32768. `PersistCell` therefore writes an all-air chunk as `Empty`, never as uniform 0.
- Region file: `"VXR2"`, `u32` voxel size (float bits), `u32 count` (<= 4096), then `[u16 local][u32 size][bytes]`;
  host endianness, no checksum. A region is 16^3 chunks, `local = x + y*16 + z*256`. `"VXR1"` files (no size) are read
  as 0.1 m (`RegionStore::kLegacyVoxelSize`). A file whose size is not bit-equal to the world's is **refused** [vd-3]:
  nothing loaded from it, `Contains` false, `Flush` never writes that region (edits there stay in memory for the
  session), `WorldStats::refusedRegions` counts it. Voxel ids of one size are a different world at another.
- `RegionStore` is thread safe (one mutex). `Save()` is memory only; `Flush()` writes temp file + rename. With an
  empty directory, edits still survive leaving and re-entering the window (kept in memory).
- Saved ids are world-palette indices handed out by `MaterialLibrary::Add` in call order (`RegisterDefaultWorldMaterials`:
  grass = 1 ... bedrock = 20). Reordering or inserting inside `RegisterDefaultWorldMaterials` silently re-materials
  every existing save. Append only.

**Determinism and hashes** (`Hash.cpp`, `CMakeLists.txt` `vortex_options`) [net-0].
- Every engine TU and everything importing engine modules is compiled with `-ffp-contract=off` through
  `vortex_options`: no fused multiply-add is formed from `a * b + c`, although `-mfma` is on. Do not add
  `-ffast-math`, `-ffp-contract=fast` or `#pragma STDC FP_CONTRACT ON` to a target or a file, and do not call
  `std::fma` or a libm function whose result is not exactly specified (`sin`, `pow`, `exp` ...) on the path from seed
  to voxel ids. Do not count on a BMI mismatch to catch a target built without the flag (not verified); the golden
  tests are the check. clang-cl ignores the bare flag with a warning, so `CMakeLists.txt` passes
  `/clang:-ffp-contract=off` there and `-Werror=unknown-argument` on every Clang; compilers other than Clang and GCC
  only get a configure warning.
- `ChunkHash` is defined over the 32768 ids in `Chunk::VoxelIndex` order as maximal runs `(id, length)`, each absorbed
  with a splitmix64 step. It reads `ids` bytes only: not the pyramid, not slots, pointers, padding or host byte order.
  Empty cell == Uniform 0 == cleared Dense == `kAirChunkHash`; Uniform `id` == Dense filled with `id` ==
  `UniformChunkHash(id)`. The storage kind is therefore never part of what two machines compare.
- `ChunkHash` skips eight voxels at a time while a run goes on [net-7b] (a HashCheck hashes up to 24 chunks): the
  runs, and so the hash, are those of the voxel-by-voxel loop (`ChunkHash.EqualsTheVoxelByVoxelRunHash`, the goldens,
  the pinned hash). Any other speed-up must keep the maximal-run definition: the value is wire-relevant.
- `ChunkHashAt` hashes a Packed cell from its runs without decoding (runs of one id are merged, so any split of the
  runs gives the maximal-run hash): a server holding a buried chunk Packed and a client holding the same voxels Dense
  agree (`World.ChunkHashAtEqualsChunkHashInEveryCellState`). It returns false for a chunk that is not loaded; never
  count such a chunk as equal.
- `WorldHash(world, min, max)` is the wrapping sum of `WorldHashTerm(coord, hash)` over the box `[min, max)` in chunk
  coordinates. An air chunk's term is 0, so the box may hold any amount of sky; the sum is order independent and
  additive over disjoint boxes. Chunks that are `Unloaded` / `Loading` add nothing and are counted in `notLoaded`:
  compare two results only when both are `Complete()`. Main thread only (it reads cells and the store).
- The hash function, `kAirChunkHash`, the run order and the constants in `Hash.cpp` are a wire contract: values are
  compared between machines and checked in (`ChunkHash.DependsOnlyOnMaterialIds` pins two chunk hashes,
  `WorldHash.TermDependsOnChunkPosition` two terms, `Terrain.GoldenChunkHashes` 192). Changing `Chunk::VoxelIndex`
  order changes every hash.
- `kGeneratorGoldenHash` names the goldens: regenerate `Terrain.GoldenChunkHashes` (or the noise / height bits) and
  `Terrain.GeneratorGoldenHashNamesTheGoldenTables` fails with the new value to paste into `Hash.cppm`. Builds with
  different values refuse each other's sessions. `MaterialTableHash` reads raw float bits of `hardness` and `density`,
  `flags` and `strength`: a new material property that a carve, the island search or the load check reads must be
  added to it (and `Materials.TableHashSeesWhatDecisionsRead`).
- `Terrain.GoldenChunkHashes` pins the built-in generator with **default** `TerrainSettings` and the default world
  materials for seeds 1337, 77 and 0x9e3779b9. Any change to the generator, to a `TerrainSettings` default or to the
  order of `RegisterDefaultWorldMaterials` fails it. If the change was meant, paste the table the failure prints over
  `kGoldenHashes` (`tests/core/HashTests.cpp`); if it was not, two builds no longer generate the same terrain. The test
  calls `Generate` directly, so it does not depend on job threads. The probe coordinates (`kGoldenCoords`, 64 per
  seed) are fixed and were picked from the generated heights of 3.2 m chunks; at 0.125 m each probe is the 4 m chunk
  holding the middle of the listed one (`ProbeChunk`, integer `floor((4c + 2) / 5)`; the test asserts the 0.125 m
  default). Picked: surface, underground, tree tops, snow columns, and 16 chunks 1 km out. The test also requires
  every seed's probes to contain grass, dirt, stone, dark stone, sand, snow, water, wood, leaves and a cave; after a
  generator change that fails this, pick new coordinates, do not drop the check.
- **The chunk hashes alone do not prove equal floats**: they see them after `floor` and compares (a build without
  `-ffp-contract=off` was measured to change only one of the 192 hashes). `Terrain.GoldenNoiseBits` (7 noise
  expressions x 3 seeds) and `Terrain.GoldenHeightBits` (`voxel::TerrainHeight`, 3 seeds) hash raw float bit patterns
  at 4096 points out to 1 km and all fail in such a build. A failure there with green chunk hashes is still a real
  disagreement.
- `voxel::IsValid(TerrainSettings)` gives the supported range (finite, `voxelSize` 0.01-10 m, `horizontalScale`
  0.01-1000, `treeSpacing` >= 0.5 m ...). Outside it `static_cast<int>` of an out-of-range float in `Noise.cppm`
  `FastFloor` and in the generator is undefined behaviour, and `coord * kChunkSize` overflows beyond +-2^25 chunks.
  `CreateTerrainGenerator` does not check; settings from a file or the network must be checked by the caller.

**Model / Instances.**
- `VoxelModel::Volume()` is the source of truth; parts are derived. Change voxels only through `SetVolume`
  (re-bakes). A replicated object's model is never changed in place: the session server caches model hashes by model
  pointer (CLAUDE.md, checks [net-7b]); make a new model.
- `Bake` first releases every part, so even a successful re-bake may get different slots and marks them all dirty.
  `Revision()` changes on `Bake`, `Unbake` and `SetVolume`. Cache derived data by revision, never by slot.
- `~VoxelModel` frees its slots into the store it was baked in: the `ChunkStore` must outlive every
  `shared_ptr<VoxelModel>`.
- `ModelPart`: `offset` in model voxels (multiple of 32), `boundsMin/Max` chunk-local `[min, max)`. All-empty pieces
  get no part. There is one `GpuInstance` per part, not per model.
- `GpuInstance` / `GpuBvhNode` sizes are `static_assert`ed and mirrored in GLSL (`VxInstance`, `VxBvhNode`);
  `boundsMinSlot.w` = slot, `boundsMaxPalette.w` = `palette * 256`.
- BVH: leaf `b != 0`, `a` = primitive; inner `b == 0`, children `a` and `a + 1`; one primitive per leaf; no primitives
  = no nodes. Node 0 is the root.
- `VoxelVolume`: `Get` / `Set` are bounds-checked (out-of-range reads give 0, writes are dropped silently); `Index` and
  `Data()` are not. `IsEmpty()` and `CountSolid()` scan the whole array. A negative size is clamped to 0.

**Materials.** Id 0 is air in every palette and cannot be set (`MaterialLibrary::Set`). GPU index =
`palette * 256 + id`. `MaterialLibrary` is not thread safe and has no removal API. `Revision()` bumps on every `Set` /
`CreatePalette`. `CarveSphere` and `ExtractFloatingIslands` always look up `kWorldPalette`.

**Shared GLSL.** `trace.glsl` is included *inside* `struct TraceKernel` (`Trace.cpp`); data is reached only through
the `VX_*` accessor macros defined there and, with the same names, in the shaders.

**Chunk sync calls** [net-7a]. Edits take a `ChunkClip`: the build ceiling is decided on the whole edit first, then
only the chunks inside the clip are visited, in the same order with the same per-voxel decisions, so one clipped call
per chunk equals the whole call (`World.EditClipEqualsTheWholeEditChunkByChunk`). `EditReport::changed` lists the
chunks an edit changed (a server's modified map). `EncodeChunkAt` gives any loaded chunk in `EncodeChunk` form (a
Packed cell's own bytes); `SetChunkEncoded` installs one into a cell in any state as a content change (`modified`,
`QueueChange`, `MarkDirty`; never reported by `TakeLoadedChunks`). With `WorldConfig::reportLoads` every cell
streaming loads is listed once (`TakeLoadedChunks`: job results and the column pass's `PlaceCell` cells) and every
window corner since the last take (`TakeWindowSteps`, in order: a session client sends each step); `WindowRevision`
counts window moves; `keepUnloadedEdits = false` drops a modified chunk that leaves the window instead of keeping it in
the `RegionStore`. `recenterSlackChunks` (session clients 1, offline 0): the window moves only once the focus chunk is
more than that many chunks from its centre chunk (`FollowMinFor`, used by `Update` and `LoadBlocking`), so
`WindowMinFor(focus)` is where a move puts it, not where every focus does. `ViewRadius` subtracts the slack (the focus
may be that many chunks off the centre): (half - 1 - slack) chunks.

**Install modes** [net-7b]. `SetChunkEncoded(chunk, rle, mode)`: `Edit` (the default, 7a) marks the cell modified -
an edit, visible, revealing its face neighbours, never packed; `Content` keeps the cell's modified bit and marks it
server-shown (never packed, but not an edit: buried neighbours stay packed); `HiddenContent` installs a Dense chunk
Packed with the given bytes and marks it server-hidden (no slot; decided before any `Alloc`; downgraded to `Content`
when the world packs nothing, the cell is an edit, or a face neighbour is an edit: the server shows such a chunk, and
an edit that ran before the install had nothing to unpack). `PacksHidden()` says whether this world packs (a
`HiddenContent` install then takes no slot). `ShowServerHiddenNeighbours(chunk)` (`ChunkSync` calls it in terrain mode
for a chunk an operation skipped) makes the server-hidden face neighbours server-shown for good and queues a packed
one for its decode (`m_Reveal`, processed by `Update`). `Cell::serverVisibility` answers `Visible()` before the column
rule (hidden: only an edited face neighbour reveals it), so a terrain-mode client never packs by its own generator's
classes. It is reset whenever the cell is re-targeted or unloaded and by job results; an `Edit` clears it. A packed
install sets `m_NearPromotePending` (near the focus it is made resident like a loaded chunk, and `Repack` packs it
again). Hashes never depend on the mode (`World.ContentInstallsPackHiddenChunksLikeLoads`,
`World.AHiddenInstallNextToAnEditIsShown`).

## 3. Traps

- **A cell that streaming owes is in its column's bookkeeping** (row bits, `Column::surfaceLeft`) and may have a job in
  flight. Anything that fills such a cell other than `IntegrateResults` / `PlaceCell` must take it out of both
  (`World::SettleLoading`: clear the row bits, decrement `surfaceLeft` if the row counted, bump the generation), or the
  job's result overwrites it later or `LoadBlocking` / `IsFilled` wait for ever
  (`World.SetChunkEncodedInstallsAnyChunkAndIsAContentChange` runs `LoadBlocking` under a timeout).
- **A Dense install must `MarkDirty` its slot**, even in place: the renderer re-uploads only `ChunkStore::TakeDirty`.
- **An install over a loaded chunk is an edit of the whole chunk** for `TakeChunkChanges` (`editMin` / `editMax` = the
  chunk): physics reads an empty box as "only loaded" and would keep simulating bodies against the old collider
  (`BodyReady` / `StaleWhere`). An install into an Unloaded / Loading cell stays a load (empty box).
- **Edits never free a slot.** A chunk carved completely empty stays `Dense` until it leaves the window; `PersistCell`
  demotes it to Empty / Uniform only in the saved bytes. Demoting inside `EditRegion` would also have to handle the
  pending GPU upload, the grid entry and the terrain collider of that chunk.
- **A full store keeps a loading chunk Packed** (above): drawn as a rock stand-in, not retried until a slot frees;
  the `ChunkStore is full` warning is function-static, once per process. An edit of such a cell needs a slot to
  materialise; if none is free the chunk is listed `unsynced` and keeps its voxels.
- `SetVoxel` returns `true` when the voxel already has that id, `false` for an unloaded chunk or a full store.
  `EditRegion` skips `Unloaded` / `Loading` chunks and chunks outside the window; it says so only through an
  `EditReport` (`skipped`), and lists a chunk whose `Materialize` failed under `unsynced`. Without a report both are
  silent; compare the returned count. Only chunks the edit really touches are listed (`touches` predicate of
  `EditRegion`: exact sphere / box / mask test), so a skipped chunk always means "this chunk now differs".
- **Replicated edits must stay integer.** `CarveSphereQ` / `FillSphereQ` / `SwapSphereQ` / `ClearMask` / `FillBox`
  decide which voxels change from integers only (`InSphereQ`: int64; carvability from `hardness * 16 > power`, exact
  in float). Do not add a float test to these paths, and do not make `EditRegion` visit chunks or voxels in another
  order: the `VoxelEdit` order is what `sim::BuildDetachedPiece` and the particle sampling read, and two machines must
  agree on it. The float `CarveSphere` / `FillSphere` quantise first (`llround(x * 16)`, halves away from zero), so a
  sphere 1/32 voxel off a voxel centre can differ from a plain float test; whole and half voxel values are exact.
- `SphereQ::center` is an int32 in 1/16 voxel: +-2^27 voxels (13 400 km at 0.1 m). `QuantizeSphere` clamps there;
  every edit box is clipped to +-`kMaxEditVoxel` (2^30) voxels, `FillBox` included. `SphereVoxelBounds` is the
  per-axis box, up to one voxel larger than the tight bounds per side; the voxel test decides.
- A failed `Materialize` happens at the first changed voxel of an Empty / Uniform cell, before anything was written,
  so an edit is all-or-nothing per chunk. Keep it that way (the `unsynced` state relies on it).
- `CarveSphere`: a voxel survives when `hardness > power` (equal is removed) or it is `Indestructible`; in the integer
  form `hardness * 16 > power` with `power` in 1/16 units (`QuantizePower(6.0f)` = 96; a hardness of 0.6 is 9.6 units:
  power 9 leaves it, 10 removes it). Water and lava have hardness 0 and are protected only by the flag. With
  `materials == nullptr` everything is removed.
- `World::Save()` has exactly two callers: the editor's "Save world" button (`Editor.cpp`) and `~World` when
  `saveDirectory` is set. `sim::Simulation` passes `saveDirectory` through (the engine's world, and with it the
  editor's button, persists when `EngineConfig::world.saveDirectory` is set); `sim::ServerConfig` clears it, and
  `net::Client::Attach` refuses a simulation that has one: a session world is memory only. There is no autosave: a
  crash loses all edits since the last save. `Save()` writes every dirty region file on the calling (main) thread.
- A region file this build cannot read (another magic, a header cut short, an impossible count) is refused with an
  error, never loaded and never overwritten (`RegionStore` `ReadFile`), like a file of another voxel size. A file
  truncated inside its chunk list keeps the chunks read before the break (`Region file ... is truncated`; a break
  inside a chunk body gives no warning), and the next `Flush` of that region overwrites the file with only what is in
  memory. A corrupt single chunk is regenerated (`LoadJob`: `Saved chunk ... is corrupt; regenerating`).
- `RegionStore::Load` reads the region file from disk while holding the mutex and `Flush` writes under it: the main
  thread's `PersistCell -> Save` can stall behind a worker's disk read. Regions are never evicted from memory.
- `TerrainSettings::voxelSize` and `WorldConfig::voxelSize` are separate fields and nothing checks that they agree
  (the simulation copies one into the other when it builds the default generator, `sim::Simulation::Initialize`).
  Both default to `voxel::kDefaultVoxelSize` (0.125 m [vd-3], `Chunk.cppm`), as does `scene::SceneConfig`.
- Generator rules are in metres; the tree trunk is 0.19-0.34 m wide, drawn as the nearest odd voxel count (1 or 3 at
  0.1 / 0.125 m). The roots (2 voxels into the ground), the cave lattice (every 4 voxels) and the slope test (voxels
  per voxel) are per voxel by design.
- `IWorldGenerator::Generate` runs on workers, concurrently, repeatedly for the same chunk. The terrain tile cache is
  mutex-guarded and cleared above 4096 tiles (`m_Cache` in `Generator.cpp`). Generators write `ids` only; `LoadJob`
  rebuilds the pyramid and demotes trivial Dense results; a `Uniform` result with id 0 becomes `Empty`.
- Trees cross chunk borders only because every chunk re-derives the trees of nearby grid cells within
  `kMaxCanopyRadius` (2.6 m), and the Empty early-out adds `kMaxTreeHeight` (7.5 m). Larger trunks or canopies than
  these constants get cut at chunk borders.
- Ray limits fail silently as a miss: `VX_MAX_GRID_STEPS = 512`, `VX_MAX_CHUNK_STEPS = 192`, BVH stack
  `VX_BVH_STACK_SIZE` 32 (children are dropped when it is full, `trace.glsl`). A ray that leaves the window misses.
- Deliberate: a ray starting inside solid reports `t = tMin` and a normal opposing its dominant axis (`VxFaceNormal`,
  axis -1); a uniform chunk is hit as a box with `slot = VX_INVALID`; `VxSafeDirection` nudges zero components to 1e-6
  instead of branching. Do not "fix" any of the three. Consequence for callers: `RayHit::slot` is `kInvalidSlot` on a
  valid world hit in a uniform chunk, so check it before `store.Get(hit.slot)`.
- `VoxelModel::SetVolume` on a baked model ignores `Bake`'s result: with a full store the model silently becomes
  unbaked (`IsBaked() == false`, only a `Log::Error`). Callers must re-check.
- `MaterialLibrary::Add` returns 0 (= air) when the palette is full or invalid. `Get` returns a reference into a vector
  that `CreatePalette` reallocates: do not hold it across palette creation. `Get` with a bad palette returns a default
  `Material` (density 1000), not an error. Unused entries are magenta on purpose (`Material.cpp`).
- `.vox`: ids are the file's colour indices; RGBA entry `i` colours id `i + 1`. Import maps (x, y, z) ->
  (x, z, sizeY-1-y) and translations to (x, z, -y). Only the first frame's `_t` is read; rotations (`_r`) are ignored.
  `WriteVox` writes no scene graph (names and translations are lost) and crops models to 256 per axis.
- glTF: voxel id = material index + 1, clamped to 255; primitives without a material also get id 1. Only the colour is
  converted (base colour factor x average of the whole texture). Only TRIANGLES with float VEC3 positions. The first
  triangle to claim a voxel wins; interior voxels take the id of the nearest solid voxel towards -X. `FillInterior`
  allocates `(size + 2)^3` bytes on top of the volume (about 270 MB together at 512^3).
- `ExtractFloatingIslands` refuses regions over 192 voxels per axis and returns nothing if any chunk of the region is
  not loaded; the optional `searched` out-parameter tells a refusal from "no island" (`sim::Blast` reports it as
  `islandsSearched`, asks the world for the box with `RequestLoad` and remembers the search;
  `sim::RetryIslandSearches` runs it once the box is loaded, from `Engine::Run`). In a fixed-height window, chunk rows
  at or above the build ceiling count as air, not as "not loaded" (nothing can exist there). `sim::Blast` calls it with
  `removeFromWorld = false` and clears each island of at least `scene::MinPieceVoxels(world voxel size)` (8 at
  0.125 m) with its own `ClearMask` (a `Detach`), all smaller ones with one `ClearMask` over their tight box. A
  component touching the region boundary counts as attached - the safety that stops it ripping out hillsides; do not
  relax it. **The face rule stands; face components are then proven by a bounded flood inside the flood box**
  (`ProveFaceComponents`, below). Ignored (liquid) voxels neither support nor belong to an island and stay in the
  world.
- The box is gathered chunk by chunk [shred-0]: `StateOf` + `GetChunk` once per chunk, empty 4^3 blocks skipped by
  `blockMask`, a Uniform chunk filled by rows, a Packed chunk decoded once into a copy (`DecodeChunk` rebuilds its
  occupancy, which the block skip reads). `FloatingIslands.GatherByChunkEqualsPerVoxel` pins it against a per-voxel
  `GetVoxel` gather. Measured (2026-11-12, -O2, quiet process, 0.125 m): 1.0-4.0 ms for a 66^3 box above ground,
  9-12 ms for a solid underground 72^3 box, against 7-14 / 20-25 ms per voxel; the labelling (`LabelComponents`,
  about 20-25 ns per solid voxel) is most of it.
- `IslandBoxScan` (optional out-parameter): the labels (`ComponentLabels::firstIndex` = each component's first voxel
  in scan order, its seed), the face components with at most `maxIslandVoxels` voxels in the box (the flood
  candidates, label order), `faceOversize`, `oversizeIslands` (floating but over the limit: they stay, counted),
  `boxVoxels` and `boxMin` / `boxMax`, the box actually labelled: in a full-height window the region without its rows
  below the bottom row (the world's ground, never loaded; a component touching that row touches the box face and is
  attached, as the flood's `bottomY` rule; a region entirely below is searched and empty). Pass `boxMin` / `boxMax`,
  not the requested region, to `ProveFaceComponents` (it checks the label size).
- `World::CarveSphereQInChunk` is `CarveSphereQ` restricted to one chunk (same decision and (z, y, x) order; all
  chunks in (y, z, x) order equal the whole carve, `World.CarveSphereQInChunkEqualsTheWholeCarveChunkByChunk`). Only
  the offline carve replay uses it; it is not a `WorldOp`.
- `ProveFaceComponents` [shred-1] (shred.md flood rules 1-9): candidates by in-box count ascending, ties by label; FIFO
  flood through world voxels, neighbours -x +x -y +y -z +z; gives up (attached) over `maxVoxels`, on a voxel of the
  flood box face (whatever lies outside), below the bottom row of a fixed-height window, at a voxel an earlier flood
  gave up on or a non-candidate face component of the box, or when `maxVisitsPerSearch` is spent (every later
  candidate too). "Visits" are voxels enqueued, so a flood that gives up at the limit costs `maxVoxels + 1`. Inside
  the island box the box labels decide, outside the world is read through a table of the flood box's chunks (one
  `StateOf` / `GetChunk` per chunk) with at most 16 decoded Packed chunks (FIFO; a re-decode is counted). A chunk that
  is not loaded stops the whole call (`deferOnUnloaded`, offline: `complete == false`, `unloadedChunk`) or attaches
  the candidate (`attachedByUnloaded`, a session server). The 2-bit state over 192^3 voxels (1.7 MiB) is a
  `thread_local`, allocated on a thread's first call (that call costs about 1 ms more), cleared through the list of
  touched voxels; take a reference to it outside hot loops (a `thread_local` with a constructor pays a guard per use).
  Measured (2026-11-12, -O2) about 50 ns per visit: 0.5 ms for the 8516-voxel tower, 2.2-3.5 ms for a 60 000-visit
  castle wall.
- `BuildColliderBoxes`: with `mergeMaterials = true`, `box.material` is only the first voxel's id; mass code uses
  `false` (`scene::Scene` `Impl::CreateBody`). `ComputeMassProperties` returns the centre of mass in voxel units from
  the volume origin; negative densities count as 0, and a volume of weightless materials gives `mass == 0` with the
  geometric centre, so callers must handle zero mass.
- `BuildChunkCollisionMesh`: vertices are chunk-local voxel units, triangles counter-clockwise seen from the empty
  side. With a null `outside` every border face is emitted (the neighbour counts as open).
  `CollisionFilter::FromMaterials` with an unknown palette returns "everything collides" plus a warning.
- **Cell graph** [shred-4]. A node is a 6-connected component of solid, not ignored voxels inside one 8^3 cell;
  **every** component is a node (collapse.md's 8-node limit with a merge into node 7 was not built), so graph
  connectivity equals voxel connectivity everywhere. Over-full cells (more than `kMaxCellNodes` = 8 nodes) are only
  counted. Node order: by cell (cx + 4 cy + 16 cz), then first voxel in (z, y, x) scan order; link order: face
  (-x +x -y +y -z +z), then node. Links inside a chunk are stored with their `bond`; across a chunk face they are
  computed from the face masks (`BondVoxels`, a face bit is y + 8z / x + 8z / x + 8y for x / y / z faces) - a graph
  never depends on a neighbour chunk, so a chunk is rebuilt only when it changes. `mass` = sum of `densityQ` (kg/m^3,
  integer); keep the masses, voxel counts and bonds: the load check uses them.
- **Load check fields** [shred-4b] (shred.md Part 4b). `GraphNode` stays 72 bytes (`static_assert`): `weak[3]` = per
  bond class (compression, shear, tension) the weakest id of the node (lowest `GraphMaterials::strength`, ties to the
  smaller id); `cutPlane` bit 7 (`kCutAnchorBit`) = the node holds an anchor id (`Indestructible` and not ignored:
  bedrock); `cutFaces` / `cutPlane` bits 0-2 = its narrowest horizontal plane (lowest k with the fewest faces across,
  narrower than the widest layer below **and** above, both halves 6-connected; shred.md "The narrowest plane"), else
  `cutFaces = kNoCut`. All are pure functions of the chunk and the tables, built with the graph (single-id cells cost
  nothing; a single-node mixed cell takes the id set of its blocks; only multi-node mixed cells read ids per voxel).
  `GraphMaterials` has `anchor` and `strength` (part of `operator==`: a table change drops the cache) and gives
  **foliage density 0** (weightless: `GraphNode::mass` of leaves is 0). `Material::strength[3]` (kg/m^2, integers)
  defaults to stone's; the table is in `RegisterDefaultWorldMaterials`.
- **The load check (`GraphSearchLimits::neck`).** Runs after the island floods when the search is complete: judged
  nodes = seeds that are not islands (a split node is two network nodes); exterior floods from their neighbours end as
  an anchor (flood box face layer, window bottom row, anchor bit, over `maxIslandVoxels`, an earlier anchor, the visit
  budget) or close as a load; arcs per (node, half) pair, loads and the sink summed per half in first-encounter order;
  capacities `bond x min(capFace[k][weak])` by the face's class (-y C, +y T, x/z S). The rule is a maximum preflow on
  capacities x 2^17 + 1 (supplies x 2^17): released = the nodes that cannot reach the sink, the same for every maximum
  preflow (**order-independent; do not "optimise" by stopping early or by a heuristic split**), and nothing at all when
  every supply reaches the sink. Each piece re-labels its nodes and re-checks them against the graph (stale:
  `relabelMismatch`, the search runs again). **Traps:** `ForEachBond` takes its node by value because floods grow the
  vector the node came from (a reference dangled: segfault); `GraphTable::neck` grows with `state` whenever a flood
  resolves a chunk. **A chunk that is not loaded is support** for the check (it can only release less): server
  `neckUnloaded` (an error); offline `neckLoading` / `neckBeyondWindow` and `neckWaits` + `neckWaitChunk`, on which
  `sim::Blast` queues a late search - the check never makes the search incomplete, so face islands are never held back
  by it (stage-4b review). An exterior flood that enters a chunk **solid through** (32^3 solid voxels) ends there as
  ground (`GraphTable::SolidThrough`, asked through `ForEachBond`'s `enter` before the chunk is resolved: Uniform id,
  Dense occupancy then ids, Packed RLE runs - it must stay equal to `graph->solidVoxels == kChunkVolume`). One check
  releases at most `maxReleasedParts` parts / `maxReleasedVoxels` voxels, in piece order (`neckDeferredParts`).
  **Foliage is left out of `weak[]`** when the node holds any other solid id (`GraphMaterials::foliage`, `WeakestOf`).
  **Stale graphs:** split and released nodes are re-derived from their voxels on everything the check reads (count,
  face masks, bounds, mass, `weak`, anchor bit, plane: `SameAsGraph`); kept judged nodes, loads and anchors are not -
  for them `World::ChunkRevision` is the only guard. `neckMs` excludes the graphs the check built (`neckBuildMs`).
- `World::ChunkRevision(chunk)` changes on every content change and load / unload of a chunk (0 = not loaded, never
  reused). **An edit path that changes chunk ids without `QueueChange` would leave a stale graph** (and a stale hash
  in the session server's cache, `notes/net.md`); all edits go through `EditRegion`. A stale *island* node is caught
  when it is re-labelled (count, face masks, bounds: `relabelMismatch`, an error line), nothing of it is written, the
  cache is dropped and the search runs again (`staleGraphRetries`; `Structure.StaleGraphNeverDetachesAttachedVoxels`);
  a stale attached neighbour is not caught. Packing / unpacking keep the revision.
- `TerrainGraphCache` is main-thread only, like the `World`. `Get` returns a pointer valid for the current pass;
  `BeginPass` (one per search) evicts above the cap (4096 graphs, least recently used) and clears everything when the
  `World` (`InstanceId`) or the material tables change. Over the cap, changed / unloaded entries go first, then the
  least recently used. Dense / Packed chunks own their graph (no growth slack); every Uniform chunk of one id shares
  one graph (empty and uniform-liquid chunks the empty one), so a cached graph's `chunk` is (0, 0, 0) for those.
  `TerrainGraphStats::bytes` counts the memory. Stats (`built`, `graphsBuilt`, `chunksDecoded`) depend on the cache's
  history: never use them to decide anything.
- `SearchIslandsOnGraph` returns the islands `ExtractFloatingIslands` + `ProveFaceComponents` would, in their order,
  except where a budget runs out (it counts node visits). A node with a voxel on the flood box face layer (or on the
  bottom row of a full-height window) is attached; such nodes are never expanded, so no chunk outside the flood box is
  read. The box island order is rebuilt from per-voxel keys of released nodes only (`LabelComponents` on an island's
  in-box voxels, and only when two face islands must be ordered). Uniform / empty chunks share one graph and cost no
  build; `Structure.DISABLED_GraphBuildCost` measures Dense and Packed builds.
- Connectivity is 6-connected: pieces touching only by an edge or a corner are separate components, and different
  materials still connect (`Connectivity.CubesTouchingOnlyByAnEdgeOrCornerAreSeparate`,
  `.DifferentMaterialsStillConnect`). Changing this changes what every destruction produces.
- `MaterialFlag_Flammable` is set on the default materials but read by no code. `MaterialFlag_Foliage` is read only by
  `MakeGraphMaterials` (weightless for the load check, and left out of a node's weakest material). The other flags with
  behaviour are `Liquid` (`Material.cpp` `Pack`, the terrain collision filter in `physics/internal/Terrain.cpp`
  `RefreshFilter`, default `ignoreFlags` of `ExtractFloatingIslands`) and `Indestructible` (`CarveSphere`,
  `scene::Scene::Damage`, the load check's anchors). Do not assume fire behaviour exists (spread.md is a design).

## 4. Recipes

**A. Add a world material.** Append to `WorldMaterials` (`Material.cppm`) and to the *end* of
`RegisterDefaultWorldMaterials` (`Material.cpp`); use it in `Generator.cpp` through `m_S.materials`. Flags decide
behaviour: `Liquid` (no terrain collision, liquid shading via `GpuMaterial::params.w`) and `Indestructible`;
`Foliage` only for the load check; `Flammable` does nothing (see traps). Give it a `strength` (load check). A new
`GpuMaterial` field or `params.w` value also needs the shader side (`VxMaterial`, `VX_MATERIAL_*` in
`shaders/include/common.glsl`). A material property that a replicated decision reads goes into `MaterialTableHash`.
A new default material changes the generator's goldens only if the generator uses it. Tests:
`CollisionFilter.FromMaterialsExcludesFlaggedMaterials`, `World.CarveRespectsHardness`,
`Materials.TableHashSeesWhatDecisionsRead`.

**B. Add a world edit operation** (capsule, replace-material ...). Write it as two lambdas over `EditRegion`
(`World.cpp`): `decide(voxel, oldId)` returns the new id, or `-1` to leave the voxel; `touches(lo, hi)` says whether
the operation reaches any voxel of that box (used only to report skipped chunks); optionally `rows(first, length)`,
false to skip a whole row along +x before any id is read (`ClearMask` uses it with `VoxelMask::AnyInRange`, so a
sparse mask over a big box costs byte tests, not voxel reads). `EditRegion` already materialises, calls `MarkDirty`,
sets `modified`, queues the change (and so bumps `ChunkRevision`), fills the `VoxelEdit` list and the `EditReport`
(`changed`, `skipped`, `unsynced`, `outOfBounds`); do not duplicate that, and never edit chunk ids outside it. Declare
it in `World.cppm`, with a `ChunkClip` parameter if a session client applies it. If it is to be replicated: integer
parameters only (q units, see traps), a new `sim::WorldOpKind` value at the end (values are wire format), a factory
and a case in `sim::ApplyOp` (`src/sim/internal/WorldOp.cpp`), and the wire side (`notes/net.md` recipe "New event
kind"). Test next to `QuantizedEdits.*` (`tests/core/WorldOpsTests.cpp`): count, old / new ids, a reference set, an
edit across a chunk border and at negative coordinates, the skipped report on an unloaded chunk; an `ApplyOp` case in
`WorldOps.ApplyOpIsTheWorldCallOfEachKind` (`tests/sim/BlastTests.cpp`); and a clip case like
`World.EditClipEqualsTheWholeEditChunkByChunk`.

**C. Change or add a generator.** Implement `IWorldGenerator` in `Generator.cpp` (anonymous namespace, factory
declared in `Generator.cppm`): a pure function of (seed, coord), thread safe, writing only `chunk.ids` via
`Chunk::VoxelIndex`. Return `Empty` / `Uniform` early where possible (no slot), and keep `ClassifyColumn` in agreement.
Anything crossing chunk borders must be derivable by each chunk on its own. Extend
`Terrain.GenerationIsDeterministicAndSeamless` and `Terrain.StreamsThroughTheWorld`. A change to the built-in terrain
also needs new constants in `Terrain.GoldenChunkHashes` (the failure prints them) and a new `kGeneratorGoldenHash`
(builds then refuse each other's sessions until both are rebuilt). Changing an existing generator does not touch
saved (modified) chunks, so old saves show seams.

**D. Change the chunk layout or grid encoding.** Touch together: `Chunk.cppm`, `trace.glsl` (offsets, index maths),
`World.cpp` (`CellIndex`, `Recenter`, `SetGrid`, `GridWords`), `Region.cpp` (change the `VXR2` magic so old files are
rejected - the codec is order-dependent), `Hash.cpp` (every hash changes: wire format), the renderer's upload sizes,
`ARCHITECTURE.md` 4.1 / 4.3. Contract tests: `Chunk.LayoutMatchesGpuContract`, `Chunk.OccupancyWordsMatchBits`,
`World.GridEncodingMatchesShaderContract`, `Trace.SingleChunkMatchesReferenceDDA`, `Region.ChunkCodecRoundTrip`.

**E. Change the traversal** (`trace.glsl`). Stay in the shared subset; it must compile as C++ and as GLSL. Run
`Trace.*`, `Bvh.*`, `World.RaycastHitsGroundAndMatchesReference`, `World.WindowSizeNeedNotBePowerOfTwo`,
`ViewDistance.ColumnSkipMatchesTheTraversalWithoutIt`, then gates G3 and G4, and compare screenshots from a fixed
`--camera` before and after. `RayHit::steps` and `Trace.EmptySpaceSkippingIsEffective` guard the cost.

## 5. Verifying a change here

Gates G1 and G2 first. Binary: `build/engine/tests/VortexCoreTests` (gtest; ctest discovers the tests as
`Suite.Name`).

```
ctest --test-dir build -R '^(Chunk|ChunkStore|ChunkHash|World|WorldHash|QuantizedEdits|Region|Terrain|Trace|Bvh)\.'
ctest --test-dir build -R '^(ColliderBoxes|MassProperties|CollisionFilter|ChunkCollisionMesh)\.'
ctest --test-dir build -R '^(Connectivity|SplitComponents|FloatingIslands|VoxFormat|Voxelize|MeshImport)\.'
ctest --test-dir build -R '^(ViewDistance|Structure|Neck|Materials)\.'
build/engine/tests/VortexCoreTests --gtest_filter='World.*'      # one suite, full output
```

| Area | Tests that pin it |
|---|---|
| Store / lifetime | `ChunkStore.AllocFreeAndDirtyTracking`, `World.WindowMovementKeepsEditsAndFreesMemory`, `World.StoreExhaustionDegradesGracefully`, `ViewDistance.AFullStoreNeverDropsASavedChunk`, `ViewDistance.ResidentBuriedChunksArePackedAgainBehindTheFocus` |
| Window / streaming | `World.LoadsWindowAndClassifiesChunks`, `World.SmallStepsOnlyReloadTheNewSlab`, `World.WindowSizeNeedNotBePowerOfTwo`, `ViewDistance.*` (climb, column pass, Packed, ceiling, column tops after edits and moves, recentre slab, `RequestLoad`) |
| Packed rule | `ViewDistance.SkyRaysRarelyMeetPackedChunks`, `...PackedChunksAreJudgedAgainWhenTheWindowMoves`, `...EditedChunksNeverBorderPackedOnesAfterAReload`, `...EverythingNearTheFocusIsResidentWhileItMovesInsideAChunk`, `...GridFallbackIsWhatTheCellShowedBefore` |
| Column skip | `ViewDistance.ColumnSkipMatchesTheTraversalWithoutIt` |
| Persistence | `World.PersistsEditsToDisk`, `Region.ChunkCodecRoundTrip` |
| Hashes / determinism | `ChunkHash.*`, `WorldHash.*`, `Terrain.GoldenChunkHashes`, `Terrain.GoldenNoiseBits`, `Terrain.GoldenHeightBits`; `QuantizedEdits.*`; in `VortexSimTests` `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` |
| Graph and load check | `Structure.*`, `Neck.*`; in `VortexSimTests` `Structure.GraphFloatingEqualsFaceFlood`, `Blast.Neck*` |
| Chunk sync and install modes | `World.EditClipEqualsTheWholeEditChunkByChunk`, `World.SetChunkEncodedInstallsAnyChunkAndIsAContentChange`, `World.ChunkHashAtEqualsChunkHashInEveryCellState`, `World.ContentInstallsPackHiddenChunksLikeLoads`, `World.AHiddenInstallNextToAnEditIsShown` |

Then the whole suite (scene, physics terrain, the session tests and the renderer contract tests consume these types)
and the gates for what you touched: G3 and G4 always; G5 for streaming or the window; G6-G10 for edits, the island
search or the load check; G11, G13, G14 for anything a session hashes, syncs or installs. In the headless game log:
- `World: N dense, N uniform, N empty chunks; store a/b, N store-full loads, N store-full decodes` (`Engine::Run`,
  printed only when `exitAfterFrames != 0`): `a` must stay below `b` (12 288 in the game; the `EngineConfig` default is
  smaller) and both store-full counts must be 0. `a == b` or `ChunkStore is full` means chunks drawn as a rock
  stand-in (the overflow is kept Packed, never dropped).
- `World: window W x H x D chunks (rows ..., fixed), N packed chunks in X MiB, N pending; 0 unloads inside the view
  radius`, `World: N window moves, recentre ... ms; N packed decodes in ... ms (N revealed), N packed again`,
  `World: surface filled in N frames`, `World: window filled in N frames` (once each; with a moving camera the window
  may never be complete).
- None of: `Saved chunk ... is corrupt`, `Region file ... refused`, `Region file ... is truncated`,
  `VoxelModel ...: chunk store is full`, `Renderer: chunk slot ... is beyond the GPU chunk capacity`.
- `Beam cut: ... 1 new object(s)` (split + re-bake path) and `Player feet at ..., on the ground` (terrain collision fed
  by `TakeChangedChunks`) still appear (G3).
- Screenshot: magenta voxels = bad material id or palette; chunk-sized (4 m at 0.125 m) holes in terrain = slots
  exhausted or a chunk never uploaded; an object showing the wrong shape or terrain voxels = stale slot after a
  re-bake.
