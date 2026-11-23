# [wg-W1] A stamped village, offline: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/worldgen|worldgen]]

| | |
|---|---|
| Stage | `[wg-W1]`: the first real map, offline: `Shredborne --region village --seed N` plans a timber village with streets, 1-3 storey frame houses and a segmented stage tower, stamps it into the terrain at generation time, and the tower's top stage falls when its designed neck is cut |
| Design | [worldgen.md](../design/worldgen.md) "W1: a stamped village, offline", "Architecture", "The planner, step by step" (P0, P1, P3-P9, P11-P13 in their W1 form), "Data formats", "Load-check guarantee" (C1-C8, "Designed necks", "Three layers of proof"), "Determinism and replication", "Budgets"; the project review (game repository, `docs/history/reviews/project-review-2026-11-17.md`) "WP11" and its row "W1 design assumes CI and a nightly sweep" |
| Code read at | engine `ea41341`, game `58d7b87`. Twelve packs land before this one: find code with `grep -n`, never by line number |
| Depends on | nothing in sim rules; card 1 (the nightly tier) is its own first step. Expected to land after `[spread-S1]`, so the palette ends at 56 (W1 uses existing materials only: wood, planks, stone, stone_dark, iron, torch, dirt) |
| Cards | 12, about 8-9 d (e) |
| Wire / rules / hashes | wire: **no** (no `RegionGenerator::Structured`, no `RegionPlanPart`: W2). Movement rule: no. `buildHash`: no (`KitLibraryHash`, `kWorldGenGoldenHash` computed and logged; W2 adds them). New library `Vortex::WorldGen`. No gate (the nightly tier is not a gate) |

## Before you start

- `git status` clean in both repositories; `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` exits 0.
- What the code looks like at the start:
  - `engine/src/voxel/Generator.cppm`: `IWorldGenerator` (`Generate(coord, chunk)`, `VerticalBounds()`,
    `ClassifyColumn(cx, cz)` returning `ColumnClass{known, emptyFrom, solidBelow, solidId, surfaceFrom}`),
    `TerrainSettings`, `CreateTerrainGenerator`, `TerrainHeight`, `kGeneratorGoldenHash`.
  - `engine/src/voxel/internal/Generator.cpp`: `HeightTile` (34 x 34 floats with a 1-voxel border, `minHeight`,
    `maxHeight`), `TerrainGenerator::Tile(cx, cz)` (cached), `Classify(tile)`, `Generate` (surface = `floor(h)`,
    trees on a jittered grid, caves).
  - `engine/src/sim/Region.cppm`: `RegionTemplate {ops, lights, objects, spawn, anchor, sited}`, `TemplateLight`,
    `TemplateObject`, `CreateTemplateLights`, `CreateTemplateObjects`, `CrateModel`.
  - `engine/src/voxel/Structure.cppm`: `NeckLimits` (`capFace[3][256]`), `MakeNeckLimits`; the load check is called
    from `sim/internal/Blast.cpp` (`grep -n "MakeNeckLimits\|neckRule" engine/src/sim/internal/Blast.cpp`).
  - `engine/src/core/Random.cppm`: `Rng` (PCG32), `engine/src/voxel/Hash.cppm`: `HashMix64`.
  - `engine/CMakeLists.txt`: `vortex_add_module_library(name dirs...)`, `vortex_assert_gpu_free`,
    `vortex_assert_no_network`, `vortex_assert_no_https`; `engine/tests/CMakeLists.txt`: `vortex_add_tests(name dir libs)`.
  - Game `src/main.cpp`: `engine.Initialize()` (built-in terrain), the plaza (`sim::PlaygroundTemplate`,
    `ApplyTemplateOps`), `--camera` (`ParseCameraPose`, `SetProbeCamera`), `--neck-blast` (`SetNeckBlast`, the
    scripted-blast pattern), `--voxel-size`.
  - `tools/verify.sh` (tiers quick / full / soak), `tools/soak.sh`.

## Decisions (us, 2026-11-23; the owner may veto)

1. **Nightly tier = a script plus a user timer the owner enables.** `tools/nightly.sh` runs the soak tier and the
   long WorldGen sweeps; `tools/systemd/vortex-nightly.{service,timer}` are shipped, never enabled by an assistant.
   Kept to half a day (owner: features over test tooling).
2. **Voxel size 0.125 m only.** `--region` with `--voxel-size` other than 0.125 exits 2 (`--region needs 0.125 m
   voxels`). Kit cells are 16 x 24 x 16 voxels (2 x 3 x 2 m).
3. **Style and recipe built in code** (`HighlandTimberStyle()`, `VillageRecipe()`); the `.style` / `.recipe` files
   come with W2 / W4. Theme `frame.oak`: frame = wood, infill = planks, plinth = stone, roof = planks, trim = wood,
   metal = iron, emissive = torch, deco = stone_dark, opening = air.
4. **Plan area**: a 160 m square (1280 voxels) centred on world (0, 0); village disc radius 40 m around the site.
5. **Gaps between buildings: 2 m** (one kit cell); party-edge 1-voxel variants (C7) wait for W3. Openings are air
   (open question 9).
6. **Planner randomness**: `Rng(HashMix64(HashMix64(HashMix64(seed, stepTag), entityId), attempt))`; step tags are
   fixed constants (`kTagSite = 1, kTagRoads = 2, kTagPlots = 3, kTagMassing = 4, kTagModules = 5, kTagMarkers = 6`).
7. **Stamper decay heights** (`Decay` records) are not in W1: no record kind `Decay` is emitted (W4).
8. **`--region` replaces the plaza**: no playground template in that run; the default run is unchanged.
9. **Fast CI uses 1 seed** for `WorldGen.RegionNeverReleases`; 200 seeds are `WorldGenSweep.*` disabled
   measurement tests that only the nightly tier runs (they stay `DISABLED_` by design).

## Skeleton

New library (card 2). `engine/CMakeLists.txt`, after Core and before Sim:

```cmake
# --- WorldGen: plans, kits, the stamper and the planner (worldgen.md); no GPU, no network -----
vortex_add_module_library(VortexWorldGen worldgen)
target_link_libraries(VortexWorldGen PUBLIC VortexCore)
add_library(Vortex::WorldGen ALIAS VortexWorldGen)
# Sim: target_link_libraries(VortexSim PUBLIC VortexCore VortexPhysics VortexWorldGen EnTT::EnTT)
# plus: vortex_assert_gpu_free(VortexWorldGen) vortex_assert_no_network(VortexWorldGen) vortex_assert_no_https(VortexWorldGen)
```

`engine/tests/CMakeLists.txt`: `vortex_add_tests(VortexWorldGenTests worldgen VortexWorldGen)` (new dir
`engine/tests/worldgen/`), and `vortex_assert_gpu_free(VortexWorldGenTests)`.

`engine/src/worldgen/WorldGen.cppm` (`export module vortex.worldgen;` `export import :plan; :kit; :stamp; :planner;`).

`engine/src/worldgen/Plan.cppm` (`vortex.worldgen:plan`), `internal/Plan.cpp`:

```cpp
export namespace vortex::worldgen {
    inline constexpr uint16_t kPlanVersion = 1;
    enum class ShapeKind : uint8_t { Pad = 0, Terrace = 1, Foundation = 2, Cut = 3, Rampart = 4, Batter = 5, Mound = 6,
                                     SpineCore = 7, CaveKeepOut = 8, TreeKeepOut = 9 };
    enum class PrimKind : uint8_t { Box = 0, Shell = 1, Cylinder = 2, Octagon = 3, Dome = 4, Stair = 5, Taper = 6 };
    enum class RoofForm : uint8_t { Gable = 0, Hip = 1, HalfHip = 2, Pent = 3, Flat = 4, Tiered = 5, Concave = 6, Tent = 7, Dome = 8 };
    enum class MarkerKind : uint8_t { Spawn = 0, Objective = 1, Light = 2, Object = 3, Landmark = 4, Neck = 5, Contest = 6, Seam = 7, Yard = 8 };
    enum Slot : uint8_t { Slot_Keep = 0, Slot_Frame = 1, Slot_Infill = 2, Slot_Plinth = 3, Slot_Roof = 4, Slot_Trim = 5,
                          Slot_Opening = 6, Slot_Metal = 7, Slot_Emissive = 8, Slot_Deco = 15, Slot_Air = 255 };
    struct PlanHeader { uint16_t flags = 0; uint32_t recipeId = 0; uint16_t styleId = 0; uint8_t attempt = 0;
                        uint32_t seed = 0; uint64_t kitLibraryHash = 0; glm::ivec3 origin{0}; };
    struct ShapeRec  { ShapeKind kind; uint8_t fillSlot, surfaceSlot, skirt; int16_t x0, z0, x1, z1, y, param; uint16_t owner; };
    struct RoadRec   { glm::i16vec3 a, b; uint8_t width, tier, surfaceSlot, flags; };          ///< width in 2-voxel units
    struct RoofRec   { RoofForm form; uint8_t rise, run, overhang; int16_t x0, z0, x1, z1, baseY;
                       uint8_t shellSlot, trimSlot, thickness, axis; uint16_t building; };
    struct ModuleRec { uint16_t moduleId; int16_t x, y, z; uint8_t rot, mirror, theme; uint16_t building; };
    struct PrimRec   { PrimKind shape; uint8_t slot, thickness, rot; int16_t box[6]; uint16_t building, param; };
    struct BuildingRec { uint16_t id; uint8_t archetype, state; uint16_t flags, settlement; int16_t box[6]; };
    struct MarkerRec { MarkerKind kind; uint8_t team; uint16_t id; int16_t x, y, z; uint16_t facing, param, building; };
    /// All coordinates: int16 voxels relative to header.origin. Records sorted; the global index is the stamp order
    /// (shapes, roads, prims, modules, roofs in that order; buildings and markers are never stamped).
    struct RegionPlan {
        PlanHeader header;
        std::vector<ShapeRec> shapes; std::vector<RoadRec> roads; std::vector<PrimRec> prims;
        std::vector<ModuleRec> modules; std::vector<RoofRec> roofs;
        std::vector<BuildingRec> buildings; std::vector<MarkerRec> markers;
    };
    std::vector<uint8_t> EncodePlan(const RegionPlan& plan);                 ///< 'VXPL', little-endian, design sizes
    bool DecodePlan(std::span<const uint8_t> bytes, RegionPlan& out);         ///< false on any malformed byte, unknown version or kind
    uint64_t PlanHash(std::span<const uint8_t> bytes);                        ///< HashMix64 chain over the bytes and the size
    /// Records inside the plan area and the height range, known module ids, slots and kinds.
    bool ValidatePlan(const RegionPlan& plan, const struct KitLibrary& kits, std::string* why = nullptr);
}
```

`engine/src/worldgen/Kit.cppm` (`:kit`), `internal/Kit.cpp`, `internal/FrameKit.cpp`:

```cpp
export namespace vortex::worldgen {
    inline constexpr int kKitCellX = 16, kKitCellY = 24, kKitCellZ = 16;
    enum class ModuleClass : uint8_t { Interior, Edge, OuterCorner, InnerCorner, RoofTrim, Prop, Landmark };
    enum class Storey : uint8_t { Ground, Upper, Top, Any };
    struct Module {
        std::string name; uint16_t id = 0; ModuleClass cls = ModuleClass::Edge; Storey storey = Storey::Any;
        glm::ivec3 sizeCells{1, 1, 1}; uint16_t weight = 1; uint32_t tags = 0;   ///< tags: kTagDoor, kTagWindow, ...
        std::vector<uint8_t> slots;   ///< size x*y*z voxels, index x + sx * (y + sy * z); out face = -z
        // derived at load (all hashed):
        uint32_t voxels = 0, graphNodes = 0;
        std::vector<uint32_t> massPerBottomColumn;    ///< kg per 1-voxel column (densities of theme frame.oak)
        std::vector<uint64_t> ratedTopLoad;           ///< kg per top cell column (8 x 8), halved (C4)
    };
    struct Theme { std::array<uint8_t, 256> material{}; };    ///< slot -> world id (Keep 0 and Air 255 never mapped)
    struct Style { std::string name; uint16_t id = 0; std::vector<Theme> themes; uint8_t pitchRise = 1, pitchRun = 1; };
    struct KitLibrary {
        std::vector<Module> modules; std::vector<Style> styles;
        const Module* Find(uint16_t id) const;
    };
    /// Code-built Frame kit (14 modules) and the highland_timber style for `ids`; refuses modules breaking C2, C3, C6.
    bool BuildDefaultKits(const voxel::WorldMaterials& ids, const voxel::MaterialLibrary& materials,
                          KitLibrary& out, std::string* why = nullptr);
    uint64_t KitLibraryHash(const KitLibrary& kits);
    /// Module voxel slot at local p after rotation (0-3 quarter turns about y) and mirror (x): index permutation only.
    uint8_t ModuleSlotAt(const Module& m, uint8_t rot, bool mirror, const glm::ivec3& p);
}
```

`engine/src/worldgen/Stamp.cppm` (`:stamp`), `internal/Stamp.cpp`, `internal/StructuredGenerator.cpp`:

```cpp
export namespace vortex::worldgen {
    /// Per chunk column: the records touching it (indices into the plan, global stamp order) and the
    /// [lowest, highest] voxel row any of them writes. Built once; immutable; thread safe.
    class PlanIndex {
    public:
        PlanIndex(const RegionPlan& plan, const KitLibrary& kits);
        std::span<const uint32_t> RecordsIn(int cx, int cz) const;     ///< sorted global indices
        bool RowsOf(int cx, int cz, int32_t& lowRow, int32_t& highRow) const;  ///< false: untouched column
        int32_t SurfaceOverride(int x, int z, int32_t baseSurface) const;      ///< Pad / Cut heights (voxels)
        bool CavesAllowed(int x, int z) const;
        bool TreeAllowed(int x, int z) const;
    };
    /// The terrain generator with the plan's surface hook and its records stamped on top.
    std::shared_ptr<voxel::IWorldGenerator> CreateStructuredGenerator(const voxel::TerrainSettings& settings,
        std::shared_ptr<const RegionPlan> plan, std::shared_ptr<const KitLibrary> kits);
    inline constexpr uint64_t kWorldGenGoldenHash = 0;   ///< card 6 sets it from WorldGen.ChunkGoldens
}
```

`engine/src/worldgen/Planner.cppm` (`:planner`), `internal/Planner*.cpp`:

```cpp
export namespace vortex::worldgen {
    struct Recipe { uint32_t id = 1; std::string name = "village"; int32_t halfSizeVoxels = 640;
                    int32_t villageRadiusVoxels = 320; uint8_t minLanes = 2, maxLanes = 4; uint8_t crates = 4;
                    uint8_t maxLights = 24; uint16_t maxExtraDenseChunks = 1200; uint32_t maxGraphNodesPer24m = 6000; };
    Recipe VillageRecipe();
    struct PlanReport { bool ok = false; uint32_t buildings = 0, modules = 0, bytes = 0, structureChunks = 0;
                        uint32_t rerolls = 0, fallbacks = 0, maxGraphNodesPer24m = 0; double ms = 0.0; std::string why; };
    /// P0-P13 (W1 form). Integer-only after sampling TerrainHeight; equal inputs give equal bytes.
    RegionPlan PlanRegion(const voxel::TerrainSettings& terrain, const Recipe& recipe, const KitLibrary& kits,
                          uint32_t seed, PlanReport& report);
}
```

`engine/src/voxel/Generator.cppm` (card 3):

```cpp
/// Optional per-column hook of the built-in terrain (worldgen.md "Changes to existing code").
/// Thread safe and pure. With no hook the output is bit-identical to today.
class ISurfaceModifier {
public:
    virtual ~ISurfaceModifier() = default;
    /// Integer surface (voxels; y < surface is ground) of world voxel column (x, z), given the generator's own floor(h).
    virtual int32_t Surface(int x, int z, int32_t baseSurface) const = 0;
    virtual bool CavesAllowed(int x, int z) const = 0;
    virtual bool TreeAllowed(int x, int z) const = 0;   ///< (x, z) = the tree's trunk column
};
std::shared_ptr<IWorldGenerator> CreateTerrainGenerator(const TerrainSettings& settings,
                                                        std::shared_ptr<const ISurfaceModifier> modifier);
```

Sim (card 9): `SimulationConfig` gains `uint32_t loadCheckMarginNum = 1, loadCheckMarginDen = 1;  ///< tests only`;
`sim/Region.cppm` gains `RegionTemplate PlanTemplate(const worldgen::RegionPlan& plan, const voxel::WorldMaterials&
m, float voxelSize);`.

Tests (card 2 writes them all as `DISABLED_`):
- `engine/tests/worldgen/PlanTests.cpp`: `WorldGen.PlanRoundTrip`, `WorldGen.DecodeRefusesMalformed`.
- `engine/tests/worldgen/KitTests.cpp`: `WorldGen.KitRefusesBadModules`, `WorldGen.RotationIsAPermutation`.
- `engine/tests/worldgen/StampTests.cpp`: `WorldGen.EmptyPlanIsTerrain`, `WorldGen.ChunkGoldens`,
  `WorldGen.ChunkOrderIndependent`, `WorldGen.StructuredClassifyColumnAgreesWithGenerate`.
- `engine/tests/worldgen/PlannerTests.cpp`: `WorldGen.PlanGolden`, `WorldGen.PlanIsReproducible`.
- `engine/tests/sim/WorldGenStandTests.cpp` (VortexSimTests): `WorldGen.ArchetypesStand`,
  `WorldGen.DesignedNecksRelease`, `WorldGen.RegionNeverReleases`; `WorldGenSweep.DISABLED_RegionNeverReleases200Seeds`.

## Card 1: The nightly tier

| | |
|---|---|
| Goal | The long sweeps and the soak run every night on the owner's laptop, on AC power, without anyone starting them |
| Effort | 0.5 d |
| Touches | `tools/` only (game repository). No engine code |

**Files**: new `tools/nightly.sh`, `tools/systemd/vortex-nightly.service`, `tools/systemd/vortex-nightly.timer`;
change `docs/verify.md` (section "Nightly").

**Steps**
1. `tools/nightly.sh`: `set -euo pipefail`; exit 0 with `Nightly: skipped (on battery)` when no
   `/sys/class/power_supply/A*/online` reads 1. Run folder `${VERIFY_OUT:-build/verify}/nightly-$(date +%F-%H%M)`.
2. Run, each to its own log: `VERIFY_SLOT=9 tools/verify.sh soak`; then, if `build/engine/tests/VortexSimTests` has
   `WorldGenSweep` tests (`--gtest_list_tests`), `--gtest_also_run_disabled_tests --gtest_filter='WorldGenSweep.*'`.
3. Last line `Nightly: PASS` or `Nightly: FAIL (<first failing step>: <log>)`; exit 0 / 1. Keep the 14 newest
   nightly folders.
4. The service: `Type=oneshot`, `WorkingDirectory=%h/Projects/Shredborne`, `ExecStart=%h/Projects/Shredborne/tools/nightly.sh`,
   `Nice=10`. The timer: `OnCalendar=*-*-* 03:00`, `Persistent=true`.
5. docs/verify.md: what it runs, ports (slot 9: 27140-27149), and the owner's line
   `systemctl --user link ~/Projects/Shredborne/tools/systemd/vortex-nightly.{service,timer} && systemctl --user enable --now vortex-nightly.timer`.
   An assistant never runs it.

**Checks**: `bash -n tools/nightly.sh`; `tools/nightly.sh` once by hand only if the owner says so (35 min);
`tools/doc-check.sh --build build`.

## Card 2: Skeleton: Vortex::WorldGen, partitions, stubs, tests DISABLED_

| | |
|---|---|
| Goal | The library, its layering checks and every declaration compile; every test of the stage exists, disabled |
| Effort | 0.5 d |
| Touches | CMake (engine, tests), new `engine/src/worldgen/`, Sim links WorldGen |

**Steps**
1. CMake lines of "Skeleton". Every new library check: WorldGen GPU-free, no network, no https.
2. All partitions of "Skeleton" with stub bodies (`return {};`, `false`, `0`; `// [wg-W1] card N fills this`).
   `CreateStructuredGenerator` stub returns `voxel::CreateTerrainGenerator(settings)`.
3. Every test of "Skeleton" written in full, `DISABLED_`.
4. CLAUDE.md section 4: the layering list gains `Vortex::WorldGen` (worldgen; plans, kits, stamper, planner; no GPU,
   no network) -> Core; Sim -> WorldGen; the three asserts. engine/docs/ARCHITECTURE.md library list likewise.

**Checks**: build; `tools/ctest.sh`; doc-check (test count line).

**Must not break**: the layering DAG (Graphics -> Core only; WorldGen never imports physics, sim, net).

## Card 3: The surface hook in the terrain generator

| | |
|---|---|
| Goal | Pads, cuts and keep-outs change the terrain itself; with no hook nothing changes, bit for bit |
| Effort | 0.5-1 d |
| Touches | voxel Generator. `kGeneratorGoldenHash` and every terrain golden: **unchanged** |

**Steps**
1. `TerrainGenerator` keeps a `std::shared_ptr<const ISurfaceModifier> m_Mod` (null by default; the old
   `CreateTerrainGenerator(settings)` passes null).
2. Where the `HeightTile` is filled: if `m_Mod`, for each of the 34 x 34 entries `s = m_Mod->Surface(x, z,
   (int32_t)floor(h))`; if `s != floor(h)` store `h = (float)s` (exact under 2^24). Recompute `minHeight` /
   `maxHeight` from the inner 32 x 32 after the overrides. `Classify` then agrees by construction.
3. Caves: skip carving a voxel whose column `!CavesAllowed(x, z)`. Trees: skip a tree whose trunk column
   `!TreeAllowed(x, z)` (decided at the trunk, so every chunk the canopy reaches agrees).
4. No float may be introduced on the null path: wrap every new line in `if (m_Mod)`.

**Tests**
- `WorldGen.EmptyPlanIsTerrain` (worldgen tests): a generator with a modifier returning `baseSurface` and `true`,
  and `CreateStructuredGenerator` with an empty plan: for 64 chunks of 3 seeds, `ChunkHash` equal to the plain
  terrain generator's.
- `ViewDistance.ClassifyColumnAgreesWithGenerate` and every `Terrain.*` golden unchanged (run them).

**Checks**: build; `tools/ctest.sh -R 'Terrain\.|ViewDistance\.|WorldGen\.'`; `tools/ctest.sh`.

## Card 4: Plan records: encode, decode, validate, hash

| | |
|---|---|
| Goal | A plan is a byte string with one hash; anything malformed is refused before it builds a voxel |
| Effort | 0.5 d |
| Touches | worldgen `:plan` |

**Steps**
1. `EncodePlan`: header (`'V','X','P','L'`, u16 version, u16 flags, u32 recipeId, u16 styleId, u8 attempt, u8 0,
   u32 seed, u64 kitLibraryHash, i32 origin x3), then **seven u32 counts**, one per record vector in `RegionPlan`
   order (the design's `u16 counts[4]` cannot count seven kinds; planVersion 1 is ours to define, nothing reads it
   yet: write the change into worldgen.md "Plan" in this card). Then each vector in the order of
   `RegionPlan`, fields little-endian at the design's sizes (Shape 20, Road 16, Roof 20, Module 12, Prim 20,
   Building 20, Marker 16; pad with zero bytes to the size).
2. `DecodePlan`: exact sizes; refuse trailing bytes, unknown version, enum values beyond the last named one.
3. `ValidatePlan`: every record box inside `[-halfSize, halfSize)` x, z around the origin and y in `[-384, 1024)`
   voxels (-48..128 m), module ids known, slots in the named set, `x0 < x1`, `z0 < z1`.
4. `PlanHash`: `h = HashMix64(0x5658504c, bytes.size())`, then 8 bytes at a time (little-endian u64, last word zero
   padded).

**Tests**: `WorldGen.PlanRoundTrip` (one of each record kind: encode -> decode -> equal; encode again -> same bytes);
`WorldGen.DecodeRefusesMalformed` (truncated by 1, one trailing byte, version 2, ShapeKind 10: each false).

## Card 5: The Frame kit and the highland_timber style

| | |
|---|---|
| Goal | 14 code-built modules with ratings the planner can trust |
| Effort | 1 d |
| Touches | worldgen `:kit` |

**Read first**: worldgen.md "Kits", "Construction rules" C1-C8; `voxel::Material::strength` and density.

**Steps**
1. A tiny DSL in `internal/FrameKit.cpp`: `Box(m, lo, hi, slot)`, `Frame(m)` (2 x 2 posts at x 0-1 and 14-15 on
   z 0-1, full height; sill beam y 0-1 and head beam y 22-23 on z 0-1), `Infill(m, lo, hi)`. Out face = -z.
2. Modules (ids 1..14, names exact): `wall_ground` (Frame + plinth rows y 0-3 stone + infill), `wall_upper`,
   `wall_top` (head beam 3 thick for the roof plate), `corner_outer` (posts on both exposed faces -x and -z),
   `corner_inner`, `door_ground` (opening x 4..11, y 4..19 air, lintel frame y 20-21), `window_upper` (opening x
   4..11, y 8..15 air), `braced_upper` (two 2-voxel diagonal frame braces), `jetty_upper` (wall pushed 4 voxels out
   on two 2 x 2 frame brackets: the only cantilever), `stair_interior` (planks steps 2 voxels per step over the cell),
   `floor_interior` (planks y 0-1 over the cell, frame joists every 8 voxels), `lantern_prop` (torch 2^3 in its own
   8^3 cell, iron bracket), `trim_ridge_end`, `trim_eave_corner`.
3. Derived fields at load: voxels; graph nodes (connected components per 8^3 cell, 6-connected); mass per bottom
   column (density of the theme's material x vs^3); rated top load per top cell column = min over bearing cells of
   that column of (shared horizontal faces x the weakest bearing material's compression strength x vs^2) / 2.
4. Refuse (`BuildDefaultKits` false, `why` names the module): a bearing slot (frame, plinth) thinner than 2 voxels in
   any cell it occupies (C6); emissive or opening voxels in a cell holding bearing slots when the opening is glass
   (W1 openings are air: only emissive is checked) (C2, C8); deco in a bearing cell where the theme's deco material
   is weaker in any of C, S, T (C2).
5. `KitLibraryHash`: every module field in id order (slots byte by byte, derived fields), every style and theme.

**Tests**: `WorldGen.KitRefusesBadModules` (a 1-voxel-thick post; a torch in a post's cell: each refused, named);
`WorldGen.RotationIsAPermutation` (for every module and rot / mirror, the multiset of slots is unchanged and rot 4x is
the identity).

## Card 6: PlanIndex, the stamper and StructuredGenerator

| | |
|---|---|
| Goal | Chunks generate with the village in them, the same in any order, on any thread |
| Effort | 1 d |
| Touches | worldgen `:stamp` |

**Steps**
1. `PlanIndex`: for each stamped record its voxel box (including a Pad's skirt, a roof's overhang, a road's width),
   per chunk column the sorted list of global indices and [low, high] rows. Surface overrides: Pad sets `y` inside
   `[x0,x1) x [z0,z1)` and blends over `skirt` voxels outside: at Chebyshev distance `d` in `1..skirt`,
   `s = base + (y - base) * (skirt + 1 - d) / (skirt + 1)` (int, truncation toward zero); Cut sets `min(base, y)`
   inside; later records win. `CaveKeepOut` / `TreeKeepOut` boxes: `false` inside.
2. `Generate(coord)`: base `Generate` from the hooked terrain generator. If no record touches the column's rows in
   this chunk, return the base result. Otherwise materialise Uniform / Empty into Dense ids, then stamp in global
   index order, each voxel of the chunk inside the record:
   - `Foundation`: fillSlot material in the box (y from `param` depth below `y` to `y`).
   - `Road`: for columns whose xz distance to segment a-b is within `width` (int64: `cross^2 <= (width)^2 * len^2`
     and the projection in [0, len^2]), the voxel at `surface - 1` becomes surfaceSlot; air above to `surface + 2`.
   - Prims: Box solid; Shell = hollow box with walls `thickness`; Cylinder vertical, int64 `dx^2 + dz^2 <= r^2` with
     centres in half voxels; Stair = `param` steps rising along the box's axis.
   - Modules: `ModuleSlotAt(rot, mirror, local)`; Keep leaves the voxel; Air writes 0; else the theme's material.
   - Roofs: for each column in the footprint plus overhang, `h = baseY + rise * dEave / run` (int), Gable `dEave`
     = distance to the nearer long edge, Hip = to the nearest edge; voxels `[h - thickness, h)` = shellSlot; gable
     end triangles below `h` = trimSlot infill one voxel thick. Shell thickness >= `ceil(rise / run) + 1` (C5;
     checked when the record is emitted).
   Then decide Empty / Uniform / Dense from the ids.
3. `ClassifyColumn`: base class merged: `emptyFrom = max(base.emptyFrom, highRow / 32 + 1)`, `solidBelow =
   min(base.solidBelow, lowRow / 32)` (floor division), `surfaceFrom = min(base.surfaceFrom, lowRow / 32 - 1)` for
   columns with a Cut or an Air slot. Too high is allowed, never too low.
4. Run `ChunkGoldens` once, print its hash, set `kWorldGenGoldenHash`.

**Tests**: `WorldGen.ChunkGoldens` (a fixed hand-written plan: one Pad, one Foundation, one Road, one Shell, 6
modules, one Gable, one Hip; 64 chunks on built columns: folded hash == `kWorldGenGoldenHash`; prints it);
`WorldGen.ChunkOrderIndependent` (the same 64 chunks in 3 orders on 1 and 7 threads of `JobSystem`: equal);
`WorldGen.StructuredClassifyColumnAgreesWithGenerate` (as `ViewDistance.ClassifyColumnAgreesWithGenerate`, with
`skipClassifiedRows = false` on the base, over every column the plan touches).

**Must not break** "No float decides a stamped voxel" (only the base terrain's floats, untouched).

## Card 7: Planner v0, part 1: sample, frame, site, roads, plots

| | |
|---|---|
| Goal | A seed gives a street plan on the terrain |
| Effort | 1 d |
| Touches | worldgen `:planner` |

**Steps** (all integers after step 1; ties to the lower index, z then x)
1. P0: `H[i][j] = floor(TerrainHeight(settings, x, z) / vs)` on a 2 m grid (16 voxels) over the plan area (80 x 80);
   water where `H < floor(seaLevel / vs) + 8`.
2. P1/P3 site: 4 m lattice cells within 40 m of the origin; score = max - min of H within 16 m of the cell;
   refuse water; lowest score wins.
3. P4 roads, A* on the 2 m grid, 4-neighbour: step cost `10 + 3 * |dH|`, forbidden where `3 * |dH| > 16` (1:3) or
   water; heuristic `10 * manhattan`; ties by (f, g, index). Main street from site - 40 m to site + 40 m along x
   (width 4 m: `width` = 16), then `rng(2..4)` lanes perpendicular from street points at x offsets drawn without
   repeat from {-24, -8, 8, 24} m, length `rng(20..32)` m, to +z or -z by rng (width 2.5 m). Store each path as
   straight `RoadRec` segments (merge collinear steps). Roads add `TreeKeepOut` over their box + 3 m.
4. P5 plots: along each road side, walk in 2 m steps: frontage `rng(3..4)` cells, depth `rng(3..4)` cells; the
   front edge on the first 2 m grid line at least 1 m beyond the road's edge. Reject: overlap with a road box + 2 m,
   another plot + 2 m (decision 5), the tower's reserve, outside the village disc, water, or max - min H over the
   footprint > 24 voxels (3 m). Plot ids in creation order.

**Tests**: `WorldGen.PlanIsReproducible` (seed 1 twice: equal bytes; seed 2: different); roads never cross water
(sampled), every plot touches a road within 3 m.

## Card 8: Planner v0, part 2: foundations, massing, modules, roofs, tower, markers, checks, emit

| | |
|---|---|
| Goal | A complete village plan that passes its own checks |
| Effort | 1 d |
| Touches | worldgen `:planner` |

**Steps**
1. P6 foundations per plot: `y = ceil8(max H over footprint)`; `Pad` (footprint + 1 m apron, skirt 8, surfaceSlot
   plinth) and `Foundation` stone from `floor8(min H) - 8` to `y` under the footprint (C1, C2); `CaveKeepOut` and
   `TreeKeepOut` over footprint + 3 m.
2. P7 massing: storeys by rng weights {1: 2, 2: 3, 3: 1}; full rectangle.
3. P8 modules: per storey, per perimeter cell: class by exposed faces (one: Edge, two: OuterCorner), rot so out
   faces the exposed side; variant by storey (Ground: `wall_ground`, the middle street-side cell `door_ground`;
   Upper: weighted `wall_upper` 5, `window_upper` 3, `braced_upper` 2, street side may take `jetty_upper` 1; Top:
   `wall_top`). Interior cells of storeys above ground: `floor_interior`; one `stair_interior` per building of 2+
   storeys. One `lantern_prop` on the street face of the ground storey (Light marker).
4. P9 roofs: `Gable` along the longer axis (Hip when square), rise / run from the style (1:1), overhang 4, thickness 2.
5. Stage tower (landmark) at the site's centre, on its own reserve 10 x 10 m: Foundation; stage 1 stone `Shell`
   6 x 6 m, walls 4, 72 voxels high (9 m: C4 <= 19 m); the **neck**: a planks `Shell` course, walls 2, 4 voxels high;
   stage 2 wood `Shell` walls 3, 48 voxels high; planks Hip roof. Markers `Landmark` at its top centre and `Neck`
   at the course's centre (param = course height). Estimated stage-2 release about 33 000 voxels (<= 45 000).
6. P11 markers: `Spawn` team 0 at the street's west end, team 1 at its east end (1 m above the road); 4 `Object`
   (crates) on the street at 1/5, 2/5, 3/5, 4/5; lights from lanterns, at most 24 (nearest the tower first).
7. P12 checks: per building, per bearing cell column, the mass above <= the module's rated top load (C4); top <= 126 m;
   extra dense chunks (chunk count touched by records) <= 1200; graph nodes per 24 m box <= 6000 (sum of module
   nodes). A failing building rerolls with `attempt + 1` up to 4, then falls back to 1 storey of `wall_ground`; a
   plan failing 8 whole attempts returns `report.ok = false`.
8. P13: sort each vector (shapes by kind then building then position; modules by building, y, z, x), encode, hash.

**Tests**: `WorldGen.PlanGolden` (village x seeds 1, 2, 3: `PlanHash` equals three pinned constants; prints them);
the plan validates; report fields > 0.

## Card 9: Sim: the plan's template, the load margin, stand and neck tests

| | |
|---|---|
| Goal | Proof that pristine houses stand at twice their load and the tower falls where designed |
| Effort | 1 d |
| Touches | sim Region, Blast (margin), VortexSimTests |

**Steps**
1. `loadCheckMarginNum / Den`: where `MakeNeckLimits` is called for a search, scale every `capFace` entry by
   `Den / Num` (uint64, floor). Default 1/1: no change. Never set by a server or the game.
2. `PlanTemplate`: lights from Light markers (`TemplateLight`, warm, the existing lantern intensity), objects from
   Object markers (`CrateModel`), `spawn` = team 0's spawn, `anchor` = the landmark marker.
3. Dud sweep helper (test file): grid spacing 6 m over every point within 4 m of a structure voxel, plus one dud on
   every storey base and neck layer; each dud `sim::Blast` r 1.2 m, power 0 carve (a dud: nothing carved, the island
   search and load check run).

**Tests** (VortexSimTests; a Simulation with `CreateStructuredGenerator`, window loaded around the subject)
- `WorldGen.ArchetypesStand`: town house 1, 2 and 3 storeys and the stage tower, x 8 seeds x 3 slope classes, margin
  2/1, full sweep: 0 voxels released.
- `WorldGen.DesignedNecksRelease`: for the tower, `FillBox` air over 3 of the 4 sides of the neck course, then a cast
  at each extreme corner of the remaining side in a fresh world: each releases stage 2, <= 45 000 voxels per part,
  <= 60 000 per cast, every released voxel within 84 voxels Chebyshev of every neck voxel.
- `WorldGen.RegionNeverReleases`: village seed 1, every building swept at margin 2/1: 0 releases.
- `WorldGenSweep.DISABLED_RegionNeverReleases200Seeds`: the same over seeds 1..200 (nightly only).

**Must not break** the load check contract (no runtime check after generation: these tests only prove), `Neck.*`,
`GraphNode` 72 B. If the tower does not release, change the neck course (thickness, material), never the check.

## Card 10: The game: --region village, cameras, --region-blast

| | |
|---|---|
| Goal | The owner can walk the village offline and fell its tower in a scripted run |
| Effort | 1 d |
| Touches | game `src/main.cpp`, README "Controls" / flags, docs/verify.md |

**Steps**
1. `--region village --seed N`: refuse `--connect` / `--join` (exit 2) and other voxel sizes (decision 2). Before
   `engine.Initialize`: default `TerrainSettings` (the ones the engine uses for its built-in terrain, materials from a
   scratch `RegisterDefaultWorldMaterials`), `BuildDefaultKits`, `PlanRegion`; on `!ok` log `Region: plan failed:
   <why>` and exit 1. Then `engine.Initialize(CreateStructuredGenerator(...))`; skip the plaza; `PlanTemplate` ->
   lights, objects, player at the spawn.
2. Log `Region: N buildings, M modules, plan K bytes, planned in X ms, structure chunks C` and `Region: kits <hash
   hex>, plan <hash hex>, worldgen golden <hex>`.
3. `--camera region-landmark` (30 m south of the tower, 1.7 m above ground, looking at 8 m up the tower) and
   `--camera region-street` (the street's west end, looking east).
4. `--region-blast`: after `World: window filled` + 30 frames, casts r 1.2 m power 6 at the neck course, around the
   ring every 10 frames until a release or 24 casts; logs `Region blast: tower released: N voxels in P part(s) after
   C cast(s)`; exit 3 if none or N > 45 000.

**Checks**: `./build/Shredborne --headless --validation --region village --seed 1 --frames 600 --camera region-landmark
--screenshot build/region-landmark.png` exit 0; the same with `region-street`; `--region-blast --frames 1200`;
look at both screenshots. `VERIFY_SLOT=1 tools/verify.sh quick` unchanged (default run untouched).

**Numbers** (write into ROADMAP "Performance and measured values"): planner ms; chunks/s with and without the
region (`World:` lines); `store a/b` inside the village; GPU passes at `--camera region-street` after fill; graph nodes
per 24 m box (report max).

## Card 11: tools/region_check with --plan-png

| | |
|---|---|
| Goal | A seed a player reports is reproduced and checked in one command |
| Effort | 0.5 d |
| Touches | new `engine/tools/region_check/` (links Sim and WorldGen, `vortex_cpu_check`, `vortex_options`) |

**Steps**: `region_check --recipe village --seed N [--plan-png out.png] [--sweep]`: plans, prints the report line of
card 10, validates; `--sweep` runs card 9's sweep in a headless Simulation and prints `Sweep: D duds, R released`;
`--plan-png`: top view 1 px per 2 voxels (roads grey, plots by building id, tower red, markers white) through
`stbi_write_png` (its implementation is already in `ThirdPartyImpl.cpp`; declare the function in a plain `.cpp`
of the tool, never in a module fragment). Exit 1 on a failed plan, 3 on releases.

## Card 12: Close [wg-W1]

1. `grep -rn 'DISABLED_' engine/tests` shows only `WorldGenSweep.*` of this stage (decision 9).
2. `VERIFY_SLOT=1 tools/verify.sh full --ci`; soak in the background.
3. Docs: worldgen.md W1 row "Built", plan format note of card 4; CLAUDE.md section 5 new "World generation" block
   (the four contract lines of worldgen.md "Contract lines for CLAUDE.md", with `WorldGen.ChunkGoldens`,
   `WorldGen.EmptyPlanIsTerrain`, `WorldGen.ArchetypesStand`); ROADMAP Now / Next, test count, numbers; README flags;
   docs/owner-checks.md: "walk the village, look at the tower falling (`--region-blast` windowed)".

## Values this stage lands

- Library `Vortex::WorldGen`; `planVersion` 1 (seven u32 counts, card 4); module ids 1-14; slots of the "Kits" table.
- `KitLibraryHash`, `kWorldGenGoldenHash`, three `PlanGolden` constants: computed, logged, not in `buildHash` (W2).
- No wire value, no `kProtocolVersion`, `kPlayerRuleRevision`, gate or palette id change.

**Traps**
- The hook path must not touch the null path: one stray float rounding changes `kGeneratorGoldenHash`.
- `ClassifyColumn` may be too high, never too low: a stamped roof above the base `emptyFrom` must raise it, or the
  chunk is skipped as Empty.
- Trees: decide by the trunk column, never by the voxel's column, or chunk borders disagree.
- Planner containers: `std::map` / sorted vectors only; never iterate an `unordered_*` to decide a record.
- Bearing members 2 voxels or more (C6); torch in its own cell (C8); no solid 32^3 chunk-aligned cube in a part meant
  to fall (it becomes an anchor).
