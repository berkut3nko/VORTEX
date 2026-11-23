# [spread-S1] Fire offline: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/spread|spread]]

| | |
|---|---|
| Stage | `[spread-S1]`: offline only, a kindled timber house burns upward and sideways (never down), its beams give out, the stone room on them falls by the load check, and a burning piece lights the floor where it lands. No wire change |
| Design | [spread.md](../design/spread.md) "Stages" S1, "Cell model and data", "The rules" (Fire only), "Destruction and the load check", "Planning one logical step" (steps 1-5, 7, 8, 10, 11 of the real pass only), "Presentation" (flicker, partial id uploads, `PresentSpread` bursts), "Headless flags" (`--fire-demo`); [magic.md](../design/magic.md) "Amendments to other designs" 1 (silent `s` advance) and 3 (Ignite is `MapShape` map 1) |
| Code read at | engine `ea41341`, game `58d7b87`. Thirteen packs land before this one and move lines: find code with `grep -n`, never by line number |
| Depends on | `[mag-1]` (`voxel::TurningTable` with `Map_Ignite = 1`, `WorldOpKind::MapShape`, `voxel::ShapeQ`, `knit_iron` = id 21, `MaterialFlag_Conjured` = bit 4, `sim::ChunkSite`, `sim::BlastOptions{.noCarve}`, `ISqrt64`); `[si-2]` (impact queue; this stage only reads it); `[mag-2]` (TurningTableHash already in `buildHash`) |
| Cards | 11, about 6-7 d (e) |
| Wire / rules / hashes | wire: **no** (`kProtocolVersion` unchanged; fire never runs in a session). Movement rule: no. `buildHash`: changes only through `MaterialTableHash` (35 new ids) and `TurningTableHash` (map 1 filled). `SpreadTableHash` is computed and logged, not in `buildHash` (S2). No gate added |

## Before you start

- `git log --oneline | grep '\[mag-1\]'` and `'\[si-2\]'` show their close cards in both repositories.
- Find what they built (names may differ slightly; adapt and say so under "Deviations"):
  - `grep -n "Map_Ignite\|BuildTurningTable\|WorldTurningTable" engine/src/voxel/Turning.cppm`
  - `grep -n "MapShape\|static WorldOp Map" engine/src/sim/WorldOp.cppm`
  - `grep -n "ChunkSite\|struct BlastOptions\|noCarve" engine/src/sim/Blast.cppm`
  - `grep -n "ISqrt64" engine/src/math/Util.cppm`; `grep -n "knitIron" engine/src/voxel/Material.cppm`
- Code this stage builds on (engine `ea41341`):
  - `voxel::Material` (`flags`, `strength[3]`, `hardness`, `density`, `emissive`), `MaterialFlags` (0-3 used, 4 =
    `Conjured` from mag-1), `WorldMaterials`, `RegisterDefaultWorldMaterials` (the `add(...)` lambda in
    `voxel/internal/Material.cpp`).
  - `voxel::Chunk` (`ids[kChunkVolume]` block-ordered, `VoxelIndex`, `BlockIndex`, `kBlocksPerChunk` = 512 blocks of
    4^3), `voxel::ChunkStore` (`MarkDirty`, `TakeDirty`), `voxel::World` (`ReadChunk`, `EditRegion`,
    `TakeChunkChanges`, `ChunkChange{coord, editMin, editMax}`), `physics/internal/Terrain.cpp` (consumes
    `TakeChunkChanges`), `graphics/internal/Renderer.cpp` (`store.TakeDirty()` then `EnqueueSlot`).
  - `sim::Simulation` (`Tick()`, `Jobs()` with `ParallelFor(count, grain, fn(begin, end))`, `World()`,
    `Materials()`, `WorldMaterialIds()`, `Config().fixedTimeStep` = 1/60), `sim::ApplyOp`, `sim::Blast`,
    `sim::PresentBlast`; `engine/internal/Engine.cpp` fixed step: `app.OnFixedUpdate(*this, fixedDt);
    sim.StepPhysics();`.
  - Game `src/main.cpp`: `--probe` scene-building pattern (`SetProbe`, a scene east of the plaza), `--camera probe`,
    `m_ScriptFrame`. Free keys today: not `A D E F S V W Space F3 F5 F12 Escape`; mech-1 moved the crate to Q and
    took E; mag-2 took the spell keys: `grep -on "Key::[A-Za-z0-9]*" src/main.cpp | sort -u` before choosing.

## Decisions (us, 2026-11-23; the owner may veto)

1. **Ignite is `MapShape(Map_Ignite, sphere)`**, no `IgniteSphere` kind (magic.md amendment 3). Kindle radius 0.5 m
   at the hit point, quantised with the existing `QuantizeSphere` rule.
2. **Ids** 22-56 in this order (knit_iron is 21): `planks_k1..k6`, `planks_b1..b6`, `wood_k1..k8`, `wood_b1..b8`,
   `foliage_k1..k3`, `foliage_b1..b2`, `charred_wood`, `scorched`. `knitwater_*` is S4, not here. If id 21 is not
   `knit_iron` or 22 is taken, take the next free run and say so.
3. **`MaterialFlag_Burning = 1u << 5`** (presentation: flicker, bursts). No rule reads it.
4. **The real pass only.** S1 has no isolated pass and no overrides: the offline driver reads true neighbours
   (frozen when not stepped) and writes the result. `StepBox` is still a pure function of a padded 40^3 box, so S2
   adds the isolated pass by filling the pad with bedrock.
5. **The driver runs only when `SimulationConfig::spread` is true** (default false). The offline game sets it;
   `vortex_server`, session clients and every existing test leave it false, so G1-G21 do not change.
6. **Kindle key offline: `G`** (first free of G, H, K, L; record the one used in README "Controls"). LMB keeps the
   blast. Cast ray = the blast's ray; refused (HUD `Kindle not run: too much fire`) at the caps of decision 8.
7. **Sites offline**: at most 8 site chunks per logical step, run as `sim::Blast(sim, sim::ChunkSite(world, c),
   {.noCarve = true})`; the rest wait in a FIFO of at most 256 (dedup; over: counted `sitesDropped`).
8. **Caps** (spread.md "Admission caps", fire rows): Kindle refused while 16+ chunks within 32 m (chunk centre to
   hit point) hold burning voxels, or 128 chunks anywhere do. Landing ignitions follow the same caps.
9. **Work credit** `kSpreadWorkPerStep = 2'000'000`, clamped to [-8, +1] steps' worth; hard cap at 2x (receivers
   not admitted). Byte credit: none offline.
10. **Chunks that unload while burning leave the index** (accepted: a reloaded saved chunk with burning ids sits
    still until a Kindle or a neighbour's step lists it). Noted in Traps; no test.

## Skeleton

New files are picked up by the globs (`vortex_add_module_library` over `src/<dir>/*.cppm|*.cpp`); add
`export import :spread;` to `engine/src/voxel/Voxel.cppm` and to `engine/src/sim/Sim.cppm`.

`engine/src/voxel/Spread.cppm` (partition `vortex.voxel:spread`), `internal/Spread.cpp`:

```cpp
module;
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
export module vortex.voxel:spread;
import :material;
import :chunk;
export namespace vortex::voxel {
    /// 512 bits, one per 4^3 block, bit b = Chunk::BlockIndex order (word b >> 6, bit b & 63).
    using BlockMask = std::array<uint64_t, 8>;
    enum class SpreadKind : uint8_t { None = 0, Flammable = 1, Burning = 2, GroundCover = 3, WetSource = 4 };
    enum SpreadClass : uint8_t { Class_None = 0, Class_Planks = 1, Class_Wood = 2, Class_Foliage = 3 };
    inline constexpr int kSpreadPad = 4, kSpreadBoxEdge = 40;          ///< 32 + 2 x 4
    inline constexpr uint32_t kSpreadBoxVolume = 40u * 40u * 40u;      ///< 64 000 B
    struct SpreadClassDef {
        uint8_t K = 0, B = 0, pace = 1;                                ///< kindle stages, burn stages, burn pace (steps)
        uint8_t dBelow = 0, dReach = 0, dSlope = 0, dSide = 0, dDiag = 0, dJump = 0;  ///< 0 = never
        uint8_t baseId = 0, firstKindle = 0, firstBurn = 0;            ///< ids
        uint8_t residue = 0, quenchEarly = 0, quenchLate = 0;          ///< ids
    };
    struct SpreadTable {
        std::array<SpreadKind, 256> kind{};
        std::array<uint8_t, 256> cls{};          ///< SpreadClass of a flammable or burning id
        std::array<uint8_t, 256> stage{};        ///< burning: k_i -> i, b_j -> K + j; else 0
        std::array<uint8_t, 256> next{};         ///< burning: id after advancing (residue after bB); else the id
        std::array<SpreadClassDef, 4> classes{};
        std::array<std::array<std::array<uint8_t, 2>, 4>, 2> kPhase{};   ///< [a][y & 3][c], 16 distinct 0..15
        uint8_t bedrock = 0, air = 0;
    };
    SpreadTable BuildSpreadTable(const WorldMaterials& ids);
    const SpreadTable& WorldSpreadTable();                 ///< built once from a scratch library (thread-safe static)
    uint64_t SpreadTableHash(const SpreadTable& table);    ///< every field, fixed order (S2 puts it in buildHash)
    /// Burn phase of world voxel v (absolute coordinates, two's complement & 3).
    uint8_t SpreadPhase(const SpreadTable& table, const glm::ivec3& v);
    /// Padded box: box[(x + 40 * (y + 40 * z))], chunk-local voxel (0,0,0) at (4,4,4).
    struct SpreadBox { std::array<uint8_t, kSpreadBoxVolume> ids; };
    struct SpreadCounts {
        uint32_t evaluated = 0, ignited = 0, advanced = 0, burnedOut = 0, quenched = 0;
        bool shapeChanged = false;   ///< some voxel changed collider-ness (0 <-> solid)
        bool site = false;           ///< spread.md "Sites": a collider became air, or a voxel entered bB or charred_wood
    };
    /// One synchronous fire step of `chunk` from `in` (old ids only). `out` starts as a copy of the
    /// chunk's old ids (Chunk::ids order); only voxels in `evalBlocks` are evaluated.
    void StepBox(const SpreadTable& table, const SpreadBox& in, const glm::ivec3& chunk, uint32_t s,
                 const BlockMask& evalBlocks, uint8_t* out /* kChunkVolume */, SpreadCounts& counts);
    /// Blocks of `chunk` holding a burning id; returns the burning voxel count.
    uint32_t LiveBlocks(const SpreadTable& table, const Chunk& chunk, BlockMask& out);
    /// `mask` grown by one block inside the chunk (26-neighbourhood).
    BlockMask DilateBlocks(const BlockMask& mask);
}
```

Changes in `vortex.voxel`:

```cpp
// Material.cppm, MaterialFlags:
MaterialFlag_Burning = 1u << 5,   ///< presentation only ([spread-S1]): flicker, bursts; no rule reads it
// Material.cppm, WorldMaterials (after knitIron): first id of each consecutive run
uint8_t planksK1 = 0, planksB1 = 0, woodK1 = 0, woodB1 = 0, foliageK1 = 0, foliageB1 = 0, charredWood = 0, scorched = 0;
// World.cppm, ChunkChange:
bool shapeChanged = true;   ///< false only when every edit since the last take kept each voxel's collider-ness
// World.cppm, class World (public, beside FillBox):
/// Writes one loaded chunk: id = newIds[Chunk::VoxelIndex(local)] where it differs, through EditRegion
/// with ChunkClip::One(chunk). Returns voxels changed; 0 when the chunk is not loaded (report.skipped).
uint32_t EditVoxelsInChunk(const glm::ivec3& chunk, const uint8_t* newIds /* kChunkVolume */,
                           std::vector<VoxelEdit>* edits = nullptr, EditReport* report = nullptr);
// Store.cppm, ChunkStore:
/// Marks only these id blocks of `slot` for upload (occupancy unchanged). A whole MarkDirty wins.
void MarkDirtyBlocks(uint32_t slot, const BlockMask& blocks);
struct DirtyBlocks { uint32_t slot; BlockMask blocks; };
/// Slots dirty by blocks only (not in TakeDirty's list), cleared. Call after TakeDirty.
std::vector<DirtyBlocks> TakeDirtyBlocks();
```

`engine/src/sim/Spread.cppm` (partition `vortex.sim:spread`), `internal/Spread.cpp`:

```cpp
export namespace vortex::sim {
    inline constexpr uint32_t kSpreadSlotTicks = 6;             ///< 10 Hz at 60 Hz
    inline constexpr int64_t kSpreadWorkPerStep = 2'000'000;
    inline constexpr uint32_t kMaxSitesPerStep = 8, kMaxQueuedSites = 256;
    inline constexpr uint32_t kKindleLocalChunks = 16, kKindleGlobalChunks = 128;
    inline constexpr double kKindleLocalMetres = 32.0, kKindleRadiusMetres = 0.5, kLandingRadiusMetres = 0.375;
    struct SpreadStats {
        uint32_t steps = 0, slotsWaited = 0, hardCapSteps = 0, silentSteps = 0;
        uint32_t awakeMax = 0, steppedMax = 0;
        uint64_t evaluated = 0, ignited = 0, burnedOut = 0, quenched = 0, recooks = 0;
        uint32_t sites = 0, sitesDropped = 0, releases = 0, landingsEmitted = 0, landingsDropped = 0;
        double msSum = 0.0, msMax = 0.0;
        uint32_t peakBurningVoxels = 0;
    };
    enum class KindleResult : uint8_t { Done, TooMuchFire, Nothing };
    class SpreadDriver {
    public:
        /// Once per tick (offline, SimulationConfig::spread): a slot every kSpreadSlotTicks ticks.
        /// Returns the ops it applied this tick (landing ignitions, site Detach / ClearMask) for PresentSpread.
        std::vector<BlastResult> Tick(Simulation& simulation, std::vector<voxel::VoxelEdit>* stepEdits = nullptr);
        /// Applies MapShape(Map_Ignite, sphere) at `center` unless a cap refuses; indexes what it lit.
        KindleResult Kindle(Simulation& simulation, const glm::dvec3& center, double radiusMetres = kKindleRadiusMetres);
        uint32_t StepIndex() const;                 ///< s of the last logical step
        uint32_t BurningVoxels() const;
        uint32_t BurningChunks() const;
        const SpreadStats& Stats() const;
    private: /* card 6 */
    };
    /// `Spread: ...` (exact format in card 10).
    std::string FormatSpreadLine(const SpreadStats& stats, uint32_t s);
}
```

`SimulationConfig` gains `bool spread = false;` and `Simulation` gains `SpreadDriver& Spread();`. The engine's
fixed step calls `if (sim.Config().spread) sim.Spread().Tick(sim, &edits)` after `OnFixedUpdate`, before
`StepPhysics`, and hands the edits and results to `PresentSpread` (`vortex.engine`, beside `PresentBlast`):

```cpp
void PresentSpread(Simulation& simulation, std::span<const voxel::VoxelEdit> edits, std::span<const BlastResult> released);
```

Tests (card 1 writes them all as `DISABLED_`; each card renames the ones it makes pass):
- `engine/tests/core/SpreadTableTests.cpp` (VortexCoreTests): `SpreadTable.*`, `SpreadRule.*`.
- `engine/tests/sim/SpreadTests.cpp` (VortexSimTests): `Spread.*`.
- `engine/tests/core/WorldTests.cpp`: `World.EditVoxelsInChunk*`; `engine/tests/core/ChunkTests.cpp`: `ChunkStore.*`.

## Card 1: Skeleton: partitions, declarations, stubs, tests DISABLED_

| | |
|---|---|
| Goal | Everything above compiles with neutral stub bodies; every test of the stage exists and is disabled |
| Depends on | `[mag-1]`, `[si-2]` committed |
| Effort | 0.5 d |
| Touches | voxel, sim, engine (declarations). Wire: no |

**Read first**: this pack's "Skeleton"; `engine/src/voxel/Voxel.cppm`, `engine/src/sim/Sim.cppm` (the
`export import` lines); one existing partition as a model (`engine/src/voxel/Turning.cppm` from mag-1).

**Steps**
1. Add the files and declarations of "Skeleton" exactly. Stubs return `{}`, `0`, `false`, `KindleResult::Nothing`,
   comment `// [spread-S1] card N fills this`. `EditVoxelsInChunk` stub returns 0. `MarkDirtyBlocks` calls
   `MarkDirty(slot)` (safe fallback). `ChunkChange::shapeChanged` defaults true (nothing changes yet).
2. Write every test of cards 2-10 in full, named `DISABLED_<Name>`.
3. ROADMAP test count line (README rule 21).

**Checks** build; `tools/ctest.sh`; `tools/doc-check.sh --build build`.

**Done when** build warning-free, all tests pass, `grep -c DISABLED_ engine/tests/*/Spread*Tests.cpp` > 0.

## Card 2: Fire ids 22-56, the Burning flag, map 1 Ignite

| | |
|---|---|
| Goal | The palette holds every fire stage and residue; Ignite turns timber and leaves into stage k1 |
| Effort | 0.5 d |
| Touches | voxel. `MaterialTableHash` and `TurningTableHash` change (a new build; sessions of old builds are refused by `buildHash`, as intended) |

**Read first**: spread.md "New world-palette ids" and "Material table values"; `RegisterDefaultWorldMaterials` and
its `add` lambda; how strength is set (`grep -n strength engine/src/voxel/internal/Material.cpp`).

**Steps** (after `knit_iron`, in `RegisterDefaultWorldMaterials`, in this order; names exactly `planks_k1` ...)
1. For class (base, K, B) in (planks, 6, 6), (wood, 8, 8): kindle `k_i`, i = 1..K: base colour, emissive
   `1 + 3 (i-1)/(K-1)`, base density / hardness / strength, flags = base flags without `Flammable`, plus `Burning`.
   Burn `b_j`, j = 1..B: colour `base x (1 - 0.6 j / B)`, emissive `5 - 3.5 (j-1)/(B-1)`, density base, hardness
   `0.6 x base`, `strength[k] = base.strength[k] x (B + 1 - j) / B` (uint64 multiply, floor), flags `Burning`.
2. Foliage (from `leaves`): `foliage_k1..k3`, `foliage_b1..b2` the same way with K 3, B 2; keep `Foliage` flag.
3. `charred_wood`: Srgb(28, 24, 22), emissive 0, density 280, hardness 0.6, strength = wood's / 4 (floor), no flags.
   `scorched`: Srgb(58, 44, 34), dirt's other values, no flags.
4. Fill `WorldMaterials` run starts. Map 1 Ignite in `BuildTurningTable`: `planks -> planksK1`, `wood -> woodK1`,
   `leaves, leavesAutumn -> foliageK1`, `grass, grassDry -> scorched`; identity elsewhere. If mag-2 already filled
   map 1, check it equals this and leave it.

**Tests**
- `Materials.FireIdsAreAppendedInOrder` (core): ids 22..56 by name, in order; `bedrock` and `knit_iron` unchanged.
- `Materials.BurnStagesWeaken` (core): `planks_b6.strength[0] == planks.strength[0] / 6`; hardness ratio 0.6.
- `TurningTable.IgniteMap` (core): the five rows; `map1[bedrock] == bedrock`; `map1[stone] == stone`.

**Checks** build; `tools/ctest.sh -R 'Materials\.|TurningTable\.|SessionFile\.'`; `tools/ctest.sh`; doc-check;
`VERIFY_SLOT=1 tools/verify.sh quick` (G3-G10 must stay equal: no rule reads the new ids yet).

**Must not break** `Chunk.LayoutMatchesGpuContract` (the GPU material table only grows), `Neck.NaturalTerrainNeverFails`,
`GraphNode` stays 72 B.

## Card 3: SpreadTable, kPhase, hash

| | |
|---|---|
| Goal | One integer table every fire decision reads |
| Effort | 0.5 d |
| Touches | voxel `:spread` |

**Steps**
1. `BuildSpreadTable(ids)`: start `kind = None`, `next[i] = i`. Classes (index 1 planks, 2 wood, 3 foliage):

   | class | K | B | pace | dBelow | dReach | dSlope | dSide | dDiag | dJump | residue | quenchEarly | quenchLate |
   |---|---|---|---|---|---|---|---|---|---|---|---|---|
   | planks | 6 | 6 | 10 | 2 | 4 | 3 | 3 | 4 | 10 | air | planks | charred_wood |
   | wood | 8 | 8 | 10 | 3 | 5 | 5 | 6 | 8 | 12 | charred_wood | wood | charred_wood |
   | foliage | 3 | 2 | 3 | 1 | 2 | 2 | 0 | 0 | 0 | air | air | air |

2. `kind[planks] = kind[wood] = kind[leaves] = kind[leavesAutumn] = Flammable` with their class. `kind[grass] =
   kind[grassDry] = GroundCover`. `kind[water] = WetSource`. For each class: `k_i` ids `Burning`, `stage = i`,
   `next = k_{i+1}` (k_K -> b_1); `b_j`: `stage = K + j`, `next = b_{j+1}`, b_B -> residue.
3. `kPhase[a][y][c] = kBayer[a * 2 + c][y]` with `kBayer = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}}`.
   `SpreadPhase(v) = kPhase[min(v.x & 3, 3 - (v.x & 3))][v.y & 3][min(v.z & 3, 3 - (v.z & 3))]`.
4. `SpreadTableHash`: `h = HashMix64(0x53505244 /* "SPRD" */, 1)`; then every array in declaration order, element by
   element (`HashMix64(h, value)`), classes field by field, kPhase, bedrock, air.

**Tests**: `SpreadTable.BedrockIsInert` (kind None, never a residue/next of anything but itself);
`SpreadTable.PhaseIsSixteenDistinctAndMirrored` (16 distinct values; `SpreadPhase(x) == SpreadPhase(3 - x)` per 4-cell);
`SpreadTable.HashCoversEveryField` (flip one entry of each array and one class field: hash changes);
`SpreadTable.NeverDerivedFromFloats` is a code review item, not a test.

## Card 4: StepBox, the fire rule

| | |
|---|---|
| Goal | The exact fire rule as a pure function, pinned by tests |
| Effort | 1 d |
| Touches | voxel `:spread`. No randomness: a configure-time grep (card 10) fails on `Hash3|rand|random|mt19937` in `internal/Spread*.cpp` |

**Read first**: spread.md "The rules" (notation) and "Fire (up and sideways, never down)".

**Steps** (`StepBox`; `B(dx,dy,dz)` = `in.ids` at the target's box position + offset; world voxel `w = chunk * 32 + local`)
1. For each block b set in `evalBlocks`, for each of its 64 voxels (Chunk order), `++counts.evaluated`, `o = B(0,0,0)`:
2. If `kind[o] == Flammable`, class c = `classes[cls[o]]`:
   1. `wet` = any face neighbour id with `kind == WetSource`. Wet: unchanged.
   2. Lit if any of (sources are ids with `kind == Burning`, `st = stage[src]`):
      - below `(0,-1,0)`: `c.dBelow && st >= c.dBelow`;
      - reach `(0,-2,0)` with `B(0,-1,0) == air`: `c.dReach && st >= c.dReach`;
      - slope `(±1,-1,0)`, `(0,-1,±1)`: `c.dSlope && st >= c.dSlope`;
      - sides `(±1,0,0)`, `(0,0,±1)`: `c.dSide && ...`; side-diagonals `(±1,0,±1)`: `c.dDiag && ...`;
      - jumps `(±2,0,0)`, `(0,0,±2)` with the middle cell `== air`: `c.dJump && ...`.
      Never a source with dy > 0. Lit: `out = c.firstKindle`, `++ignited`.
3. If `kind[o] == Burning`, c = its class, t = `stage[o]`:
   1. wet: `out = t <= K ? quenchEarly : quenchLate` (foliage: air); `++quenched`.
   2. else if `t <= K` or `(s + SpreadPhase(w)) % pace == 0`: `out = next[o]`; `++advanced`; if `out` is a residue
      or air, `++burnedOut`.
4. Everything else unchanged (ground cover never catches from a neighbour; only Ignite scorches it).
5. Flags: `shapeChanged |= (o != 0) != (out != 0)`; `site |= (o != 0 && out == 0) || out == charred_wood ||
   (kind[out] == Burning && stage[out] == K + B of its class)`.
6. Integer arithmetic only (`uint32_t` s; `(s + phase) % pace` in uint32). Write to `out` only; never read `out`.

**Tests** (`SpreadRule.*`, core; helper `FireYard` builds a `SpreadBox` from a lambda and steps it N times,
re-padding with the previous output each step, bedrock outside)
- `SpreadRule.FireNeverIgnitesBelow`: a 12^3 planks block lit at one voxel on layer y = 6; 300 steps; no voxel with
  y < 6 ever left `planks`.
- `SpreadRule.FrontSpeedsMatchTheTable`: a 30-long planks row lit at x = 0: after 10, 20, 40 steps the farthest lit
  voxel is at x = `floor(steps / dSide)` ± 1 (planks dSide 3 -> 3, 6, 13); up column with dBelow 2 -> 5, 10, 20.
- `SpreadRule.JumpsOneGapNotTwo`: two planks walls 1 air apart catch within 40 steps; 2 apart never (100 steps);
  foliage never jumps.
- `SpreadRule.FoliageOnlyClimbs`: a 1-voxel-thick horizontal leaf sheet lit at one voxel: no other voxel of the
  sheet ever burns (foliage dSide / dDiag / dJump are 0); a leaf column above the lit voxel burns upward.
- `SpreadRule.SymmetricSetupStaysSymmetric`: planks block mirrored about x = 15.5 (4n - 1/2 with n = 4), lit
  symmetrically; every step, ids mirror.
- `SpreadRule.EveryFireBurnsOutWithinBound`: any single voxel of each class lit: gone (residue) within
  `K + B * pace` steps (planks 66, wood 88, foliage 9).
- `SpreadRule.QuenchEarlySavesQuenchLateChars`, `SpreadRule.WetBlocksIgnition` (a `water` voxel beside the target).
- `SpreadRule.BurnoutsSpreadEvenly`: 60 steps of a steady 24^3 planks fire: burnouts per step within 2x of the mean
  over steps 30-60.
- `SpreadRule.PadIsInert`: bedrock pad vs air pad around a block that burns only inside: equal outputs.

## Card 5: EditVoxelsInChunk, ChunkChange::shapeChanged, no re-cook on stage changes

| | |
|---|---|
| Goal | A step writes a chunk in one edit; stage changes do not re-cook colliders |
| Effort | 0.5 d |
| Touches | voxel World, physics Terrain. Contract: "every chunk edit goes through World::EditRegion" kept |

**Read first**: `World::EditRegion` (template in `World.cppm`, body in `World.cpp`), where it queues the
`ChunkChange` (`grep -n "editMin" engine/src/voxel/internal/World.cpp`); `Terrain.cpp` around `TakeChunkChanges`.

**Steps**
1. `EditVoxelsInChunk`: box = the chunk's voxel box; `ChunkClip c = ChunkClip::One(chunk)`; `decide(v, old)` returns
   `newIds[Chunk::VoxelIndex(v - chunk * 32)]`; `touches` = the box test. Not loaded: `report.skipped`, return 0.
2. Where EditRegion records a pending change for a chunk: keep a per-chunk `shapeChanged` that ORs
   `(old != 0) != (new != 0)` per changed voxel. Load, unload, `SetChunkEncoded` and every other path set it true.
   Merge rule in `TakeChunkChanges`: OR.
3. Terrain: for a change with `Edited() && !shapeChanged`, skip the re-cook (the collider's shape is equal). Count
   skipped re-cooks (`recooksSkipped`) for the `Spread:` line.

**Tests**: `World.EditVoxelsInChunkEqualsSetVoxel` (core: a random id array over a loaded chunk = the same through
`SetVoxel` per voxel; equal `ChunkHash`, one revision bump); `World.ShapeChangedOnlyWhenOccupancyChanges` (planks ->
planks_k1 gives `shapeChanged == false`; planks -> air true; load true); `Spread.ShapeUnchangedSkipsRecook`
(sim: a terrain collider chunk, a stage change: no re-cook counted; a burnout: one).

**Must not break** `Structure.ChunkRevisionChangesWithEveryContentChange`, `World.SetChunkEncodedInstallsAnyChunkAndIsAContentChange`,
L8 (bodies over a chunk whose collider is pending stay frozen).

## Card 6: SpreadDriver: index, awake set, logical steps, the work credit

| | |
|---|---|
| Goal | Offline fire advances 10 times a second, all-or-nothing, on worker threads |
| Effort | 1 d |
| Touches | sim `:spread`, `Simulation` (config flag, `Spread()`), engine fixed step |

**Steps** (private state: `std::map<std::tuple<int,int,int>, Entry>` keyed (y, z, x), `Entry { uint32_t burning;
voxel::BlockMask live; }`; `uint32_t m_S = 0`; `int64_t m_Credit = kSpreadWorkPerStep`; site FIFO)
1. `Tick`: return if `Tick() % kSpreadSlotTicks != 0`. `m_Credit = min(m_Credit + K, K)`, floor `-8K`. If
   `m_Credit < 0`: `++slotsWaited`, return (s holds).
2. Awake A = entries with `burning > 0`, map order. A empty: `++m_S; ++silentSteps`; return (amendment 1).
3. Stepped S: A, plus each of the 26 neighbours N of a chunk in A that is loaded and inside the window, whose
   eval mask (below) is not empty. Eval mask of chunk X = `DilateBlocks(live(X))` OR, for each face/edge/corner
   neighbour Y in A, the blocks of X within one block of Y's live blocks across the shared boundary (blocks of X
   adjacent to the boundary whose mirrored block in Y is live). Sort S in (y, z, x).
4. Work = Σ over S of `1000 + 64 * popcount(eval)`. If work > 2K: drop from S every chunk not in A
   (`++hardCapSteps`), recompute work. `m_Credit -= work`.
5. Gather on the main thread: read every chunk of S and its 26 neighbours once (`World::ReadChunk` into a
   `std::map` of `std::unique_ptr<voxel::Chunk>`; not loaded or outside the window: absent). Build one `SpreadBox`
   per chunk of S from that cache (absent -> bedrock).
6. Compute with `simulation.Jobs().ParallelFor(S.size(), 1, ...)`: per chunk `out = copy of old ids`, `StepBox`.
   One output buffer per chunk (vector sized |S| x 32 768); no shared writes.
7. `++m_S` before computing (the step uses the new s). Write on the main thread, in S order:
   `EditVoxelsInChunk(chunk, out, stepEdits)`; recount each written chunk with `LiveBlocks` (erase entries at 0).
   A chunk with `site` set: `QueueSite`.
8. Stats: max awake / stepped, evaluated, ignitions, burnouts, ms (steady clock around steps 3-7; logging only).
9. `Kindle(center, r)`: caps of decision 8 (count entries with burning > 0: global; within 32 m: chunk centre
   `(c + 0.5) * 32 * vs` distance). Then `ApplyOp(world, WorldOp::Map(Map_Ignite, ShapeQ{kShapeSphere,
   QuantizeSphere...}), &edits)`; recount every chunk in `report.changed`. No edits: `Nothing`.

**Tests**: `Spread.StepClockCountsLogicalSteps` (a credit forced negative skips slots; `StepIndex` advances only on
steps; a wood block still burns out within 88 logical steps); `Spread.PhaseGatedContentAdvancesWithNothingElseAwake`
(nothing burning: `s` still advances each slot); `Spread.ResultIndependentOfChunkOrderAndThreads` (a 3x2x3-chunk
timber yard, 1 and 4 workers and a shuffled S: equal `WorldHash` after 100 steps); `Spread.FireCrossesChunkFaces`
(a planks slab spanning 2 chunks lit in one: the other catches); `Spread.KindleCapsRefuse` (17 burning chunks within
32 m: `TooMuchFire`).

**Must not break** R5-like rule: no budget reads `graphsBuilt` / `chunksDecoded` (`CastBudget.*`).

## Card 7: Sites offline and the landing ignition

| | |
|---|---|
| Goal | A burning support gives out and what it held falls; a burning piece lights the floor below it |
| Effort | 1 d |
| Touches | sim `:spread`, uses `sim::Blast`, `sim::ChunkSite`, `BlastOptions{.noCarve}` (mag-1) |

**Steps**
1. Each `Tick` after the write: pop up to `kMaxSitesPerStep` chunks from the FIFO (dedup on push; over 256:
   `++sitesDropped`); `r = Blast(sim, ChunkSite(world, c), {.noCarve = true})`; push into the returned vector;
   `releases += r.ops count of Detach`.
2. For each `Detach` op of a result: count its burning voxels (edits of that op with `kind[oldId] == Burning`). If
   >= 8: lowest burning voxel `p` (lowest y, then z, then x). Column walk from `p.y - 1` down at most 512 voxels to
   the first voxel `q` with `IsCollider` (id != 0, not `Liquid`) via `World::GetVoxel`. None: drop
   (`++landingsDropped`).
3. `drop = p.y - q.y` (voxels). `fallSq1024` once per driver: `llround(2.0 * vs / 9.81 / dt^2 * 1024)` (93 942 at
   0.125 m, 60 Hz). `fallTicks = ISqrt64(drop * fallSq1024 / 1024) + 1` (80 voxels -> 86 ticks).
4. Schedule `{dueTick = Tick() + fallTicks, voxel q}` in a `std::multimap<uint32_t, glm::ivec3>`; at the due tick
   re-walk the column from the same `p` (world may have changed) and `Kindle(top centre of q, kLandingRadiusMetres)`;
   a refusal counts `landingsDropped`, success `landingsEmitted`.

**Tests**: `Spread.BurnoutReleasesAStructure` (planks posts 2x2 voxels, 3 m, under a stone slab 3x3 m: kindled at
the foot; within 300 steps a release of the slab; `IslandSearchCheck().neckReleases` > 0);
`Spread.FallingBurningPieceLightsFloor` (a burning planks beam released over a planks floor: the floor holds a
burning id within `fallTicks + 12` ticks of the release); `Neck.NaturalTerrainNeverFails` unchanged; the stage-1
arenas release nothing.

**Must not break** the load check contract (released set from max-flow, only `Detach` / `ClearMask`; never after
FillBox / editor / generation: fire runs only from `Kindle`), L9 (released pieces are rubble with lifetimes).

## Card 8: Partial id uploads

| | |
|---|---|
| Goal | A fire's stage changes upload 64 B blocks, not 36 936 B chunks |
| Effort | 0.5-1 d |
| Touches | voxel Store, World (EditRegion's dirty marking), graphics Renderer |

**Steps**
1. `ChunkStore`: per slot a `BlockMask` and a flag "in block list". `MarkDirtyBlocks` ORs blocks unless the slot is
   whole-dirty; `MarkDirty` clears its block entry. `TakeDirtyBlocks` returns and clears.
2. EditRegion: per changed chunk, if no voxel changed occupancy (0 <-> non-0, the same test as `shapeChanged`):
   `MarkDirtyBlocks(slot, blocks touched)`; else `MarkDirty(slot)` as today.
3. Renderer, after `for (slot : store.TakeDirty()) EnqueueSlot(slot)`: for each `DirtyBlocks`, copy runs of
   consecutive set blocks: source `&chunk.ids[b * 64]`, destination offset `slot * kChunkBytes + offsetof(Chunk, ids)
   + b * 64` (assert `offsetof` against the GPU layout used by `Chunk.LayoutMatchesGpuContract`). Count bytes in the
   existing upload stat.

**Tests**: `ChunkStore.BlockDirtyIsSupersededByWhole` (core); `ChunkStore.BlockDirtyListsOnlyChangedBlocks`.
GPU behaviour is checked by card 10's screenshot (no GPU in ctest).

**Must not break** `Chunk.LayoutMatchesGpuContract`; the per-frame upload cap (128 chunks) still bounds whole uploads.

## Card 9: Kindle key, flicker, PresentSpread bursts

| | |
|---|---|
| Goal | The player lights things offline and sees fire flicker without bands |
| Effort | 0.5-1 d |
| Touches | game `src/main.cpp`, engine `PresentSpread`, shaders (lighting / emissive pass, **not** `trace.glsl`) |

**Steps**
1. Game, offline only: `config.spread = true` for the offline game; `Key::G` (decision 6) casts the blast's ray
   and calls `Spread().Kindle(hit)`; HUD line `Kindle not run: too much fire` for 2 s on `TooMuchFire`; key hint
   `Kindle [G]` in the mode panel. Session: the key does nothing.
2. Flicker: where emissive is applied in the composite / lighting shader, for materials flagged burning (add
   `VX_MATERIAL_BURNING` to the GPU material `params` the same way as `kGpuMaterialLiquid`): `emissive *= 0.7 + 0.3 *
   noise(voxel, time)` with an integer hash of the voxel and `floor(time * 8)`. Presentation only.
3. `PresentSpread`: per edit `Flammable -> Burning`: at most 32 `Flame()` bursts per step (first 32 in edit
   order); `Burning -> air`: at most 32 ember particles; released pieces through `PresentBlast` as today.

**Checks**: build; `tools/ctest.sh -R 'ShaderContract\.|LayoutContract\.'`; owner check (add to
`docs/owner-checks.md`: "press G on a timber wall: it catches, climbs, flickers, burns out").

## Card 10: --fire-demo, the Spread: line, no-randomness grep

| | |
|---|---|
| Goal | A headless run proves the burning house that falls |
| Effort | 1 d |
| Touches | game, `engine/CMakeLists.txt` (configure grep) |

**Steps**
1. `--fire-demo` (implies offline; refused in a session; exit 2 with `--connect`), built east of the plaza like
   `--probe` at 0.125 m: house A 5 x 5 m planks floor and 3 m walls (2 voxels thick), wood corner posts 2x2 voxels
   carrying wood beams at 3 m, a stone upper room (4 x 4 x 2 m, walls 2 voxels) on the beams; house B the same
   planks box 1 voxel east of A's east wall; a tree (wood trunk 2x2, leaf crown 3 m wide) 2 voxels west of A whose
   crown starts 1 voxel above A's wall top. Frame 30: `Kindle` at A's south-west corner foot.
2. Log every 60 frames and at exit `Spread: s S, steps N (waited W, silent Q, hard cap H), awake A max, stepped T
   max, evaluated E, ignited I, burned out B, quenched Q, recooks R (skipped K), sites X (dropped Y), releases Z,
   landings L (dropped M), ms avg U max V, table T` (T = SpreadTableHash hex).
3. Expectations (exit 3 when unmet): `Fire: 0 voxels ignited below the ignition layer` (track min y of every
   ignition against the kindle layer, excluding landing ignitions); `Fire: tree caught`; `Fire: neighbour house
   caught`; `Fire: peak burning fraction P %` (P >= 35, of A's timber voxels); `Fire: collapse: N piece(s) released`
   (N >= 1); with `--frames 1200`: `Fire: out after N steps`.
4. `--camera fire-demo` frames house A from 6 m south-west; `--screenshot` at frame 200.
5. Configure-time grep: `file(STRINGS)` over `src/voxel/internal/Spread.cpp` and `src/sim/internal/Spread.cpp`; fail
   on `Hash3`, `rand(`, `random`, `mt19937`.
6. docs/verify.md: the `--fire-demo` lines and flags (not a gate).

**Checks**: `./build/Shredborne --headless --validation --fire-demo --frames 1200 --camera fire-demo --screenshot
build/fire.png` exits 0 with every line above; look at the screenshot (glow, no equal-brightness bands); the same
with `--after-fill` for `ms`. `VERIFY_SLOT=1 tools/verify.sh quick`: G3-G10 lines unchanged.

**Numbers**: `Spread:` ms avg / max after fill; GPU pass times at the fixed camera with and without fire; upload
bytes per step frame.

## Card 11: Close [spread-S1]

**Steps**
1. `grep -rn 'DISABLED_' engine/tests` shows no `Spread`, `SpreadRule`, `SpreadTable`, `ChunkStore` test of this stage.
2. `VERIFY_SLOT=1 tools/verify.sh full --ci`; `VERIFY_SLOT=1 tools/verify.sh soak` in the background (expected
   unchanged: sessions never run fire).
3. Docs: spread.md stage row S1 "Built", "Status"; ROADMAP Now / Next and test count; CLAUDE.md section 5 under
   "World edits and blasts": **Spread (offline)**: "spread state lives only in voxel ids; `StepBox` decides from the
   chunk's padded old ids, `s` and world coordinates only; no randomness (configure grep); `ChunkChange::shapeChanged`
   decides re-cooks" with the tests `SpreadRule.SymmetricSetupStaysSymmetric`, `Spread.ShapeUnchangedSkipsRecook`;
   README "Controls" (the Kindle key); docs/owner-checks.md entry.
4. Report with the numbers of card 10.

## Values this stage lands

- World palette: ids 22-56 (35 fire ids, decision 2). `MaterialFlag_Burning` = bit 5.
- `TurningTable` map 1 Ignite rows. `SpreadTableHash` computed, logged, **not** in `buildHash` (S2).
- No `WorldOpKind`, `EventKind`, `MessageType`, `PlayerButton` or `kProtocolVersion` change; no gate.

**Traps**
- Reading `out` instead of `in` anywhere makes the rule order-dependent: `SymmetricSetupStaysSymmetric` catches it.
- `& 3` on negative coordinates must be two's complement on the int (not `% 4`).
- Do not decide anything by `graphsBuilt` / `chunksDecoded` / time; `ms` is logging only.
- A burning chunk that leaves the window drops out of the index (decision 10); never "rescan everything" per step.
- `EditVoxelsInChunk` must go through `EditRegion` (revision bump, `changed`, ceiling rule); a direct `ids[]` write
  leaves island graphs stale (`Structure.StaleGraphNeverDetachesAttachedVoxels`).
