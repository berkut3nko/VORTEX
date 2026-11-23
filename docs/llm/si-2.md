# [si-2] Impacts with crush damage: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/shred|shred]]

| | |
|---|---|
| Stage | `[si-2]`: heavy falling pieces crater soft ground, dent still objects and shatter once; a falling server body crushes players; first the `NeckCheck` split and a chunk-source interface under the graph search |
| Design | [shred.md](../design/shred.md) "SI-2 - impacts (inertial contact)" (Data, Algorithm, the cases, Determinism and replication, Budgets, Tests, Headless, Risks); Part 4b "The check, step by step" (the phases of `NeckCheck`); SI-1 step 5 (`BuildNodeNetwork` over a graph view: why the split); open question 8 (crush, settled below). ROADMAP "Next" item 8; the project review "WP9" |
| Code read at | engine `ea41341`, game `58d7b87` (before `[wp-4]`..`[mag-2]`; where a card builds on them it names the symbol and says "read the built interface") |
| Depends on | `[wp-7]` (`vortex.sim:combat`: health, `ApplyDamage`, `DamageCause`, the in-process offline server), `[mag-1]` (`BlastOptions{noCarve}`, `BlastSite`, `StructureCredit`), `[mag-2]` (committed first on the ROADMAP order; nothing of it is used) |
| Cards | 11 |
| Effort | 7-11 d (e) (split and chunk source 2-4 d, impacts 5-7 d) |
| Changes | wire: **no** (README plan: `[si-2]` changes no byte; a crater is a `ClearMask`, dents and shatters are existing entity events). Movement rule: **no**. `buildHash`: no. Gates: **none new** (the design asks for log lines; `--impact-drop` is run by hand in cards 8 and 11); G3-G17 lines unchanged |

## Before you start

- Committed in both repositories: `[wp-7]`, `[mag-1]`, `[mag-2]`. Check with `git log --oneline | grep -E
  '\[(wp-7|mag-1|mag-2)\]'` in the game root and in `engine/`. Missing: stop, `Status: blocked`.
- What this stage builds on (verify each with `grep -n`):
  - `engine/src/voxel/internal/Structure.cpp` (2 100 lines: read by ranges): `class GraphTable` (holds `const World&`,
    `TerrainGraphCache&`; `Resolve`, `SolidThrough`, `state`, `seed`, `neck`), `class VoxelSource` (Packed copies),
    `NeckCheck(table, seeds, lim, limits, materials, source, innerMin, innerMax, result)` with its six numbered phase
    comments (`// 1. The judged nodes` .. `// 6. Voxels`), `SearchOnce(const World&, ...)`, `SearchIslandsOnGraph`.
    The `World` calls it makes: `StateOf`, `GetChunk(chunk, &uniformId)`, `ReadChunk`, `PackedData`,
    `ChunkRevision`, `IsChunkLoaded`, `FullHeight`, `WindowMinChunk`, `WindowSize`, `InstanceId`.
  - `engine/src/voxel/Structure.cppm`: `TerrainGraphCache` (`BeginPass(const World&, ...)`, `Get(const World&,
    chunk)`, `InjectForTesting`), `NeckLimits`, `NeckNetwork`, `SolveNeckNetwork`, `GraphSearchLimits`,
    `GraphSearchResult` (`neckFlowWork`, `neckRan`, ...). `GraphNode` is 72 bytes (`static_assert`).
  - `engine/src/physics/PhysicsWorld.cppm`: `ContactEvent{bodyA, bodyB, userDataA, userDataB, position, normal,
    approachSpeed}`, `TakeContactEvents()`; `internal/PhysicsWorld.cpp`: `ContactCollector::OnContactAdded`,
    `Publish` (sorts by (bodyA, bodyB, approachSpeed desc) today), `kMaxQueuedContactEvents` 16 384. **Nothing calls
    `TakeContactEvents` today**: the queue fills and warns.
  - `engine/src/sim/Blast.cppm`: `Blast(Simulation&, const BlastSite&, const BlastOptions&)` (`[mag-1]`),
    `BlastResult` (`deferred`, `fragments`, `objectEvents`), `CastWorkOf`; `sim::CastFitsRegion` (`Region.cppm`);
    `sim::TakePieceSlot` (`Replication.cppm`); `kServerReplicatedPieceVolume` (`Simulation.cppm`).
  - `engine/src/scene/Scene.cppm`: `Scene::Damage(entity, center, radius)` -> `DamageResult`, `RigidBody`, `NetId`,
    `Lifetime`; `voxel::SplitComponents` (`Connectivity.cppm`).
  - `engine/src/net/internal/Server.cpp`: `Server::Tick` order, `RunCasts`, `EmitOp`, `EmitObjectEvents`,
    `entityCredit`, `ServerStats`. `engine/src/engine/internal/Engine.cpp`: `Engine::PresentBlast`, the
    `RetryIslandSearches` loop in `Engine::Run`.
  - `[mag-1]`'s `sim::StructureCredit` (`Knit.cppm`: `kWorkPerTick` 15 000, `credit`, `queue`, `Run`) and its owner
    (`CastContext::structure`, on `net::Server::Impl` since `[mag-2]`). `[wp-7]`: `sim::ApplyDamage`, `DamageCause`
    (read the built names).
  - Tests: `engine/tests/sim/NeckTests.cpp`, `FaceFloodTests.cpp`, `BlastTests.cpp`: all must stay green, unchanged.

## Decisions

1. **The split is bit-for-bit.** Card 1 records a golden of the load check's network (a hash over node supplies and
   arcs in order) and the released sets on the tower, neck and castle fixtures *before* any change; cards 1-2 must
   keep both equal, plus `--neck-blast` flow work 7 055 (`Neck: ... flow work 7055`).
2. **The chunk source is a pure virtual interface in voxel** (`voxel::ChunkSource`), `World` implements it (no
   wrapper: `class World : public ChunkSource`, `final` overrides). The cache keys entries by
   `ChunkSource::InstanceId()`. SI-1's model tiles implement it later; here only `World` and a test source do.
3. **Contacts are drained every tick in every configuration** (`Simulation::StepPhysics` and `Step`); with `impacts`
   off they are dropped. Session clients: always off.
4. **`SimulationConfig::impacts`** default false; the game's offline config and `vortex_server` set it true;
   `--no-impacts` (game and server) clears it.
5. **Constants** (shred.md, all (e)), in integers after quantising: `kImpactMinEnergyQ` = 20 kJ / 16 = 1250,
   `kDentMinSpeedQ` 400 cm/s, `kCraterEnergyQ` 3125 (50 kJ), `kCraterMinSpeedQ` 600, crater radius `min(1.0 m, 0.35 m x
   cbrt(E / 50 kJ))`, power `min(2.5, 1 + E / 500 kJ)` hardness, `kShatterSpecificEnergy` 30 J/kg,
   `kImpactReadyTicks` 30, `kImpactMaxAgeTicks` 10, `kMaxPendingImpacts` 64, per tick 1 crater, 4 dents, 1 shatter.
   Radius and power are integers from `energyQ` with `math::ICbrt64` (new, integer cube root, floor): `rQ =
   min(Q(1.0), (Q(0.35) * ICbrt64(energyQ * 2^24 / 3125)) >> 8)` (cbrt(E / 50 kJ) in 1/256); dent `rQ = min(Q(0.5),
   (Q(0.2) * ICbrt64(energyQ * 2^24 / 1250)) >> 8)`; `powerQ = min(40, 16 + energyQ * 16 / 31250)` (1/16 hardness).
   `Q(m) = llround(m / voxelSize * 16)`, computed once per simulation (45 q for 0.35 m at 0.125 m).
6. **Structure credit.** `[mag-1]`'s `StructureCredit` gains `bytes` (200 B per tick, same clamp: `[-6 x perTick,
   perTick]`) and `bool Spend(int64_t work, int64_t bytes)`. Impacts and site checks share it; `RunCasts` never
   touches it. Offline: no credits, only the per-tick counts.
7. **Crush damage (open question 8, settled: with `[si-2]`).** A server sweep, not contacts (characters are
   `CharacterVirtual`): every tick after `StepPhysics`, each awake replicated body (`NetId`, at least
   `kServerReplicatedPieceVolume`) with speed >= 4 m/s and moving down or toward the player (`dot(v, toPlayer) > 0`),
   whose box (grown 0.05 m) meets a living player's capsule box, deals `amount = clamp(E_kJ, 20, 100)` with `E = 0.5 m
   v^2` (float on the server: damage is a server-only result), at most once per (body, player) per 30 ticks. Offline
   through `[wp-7]`'s in-process server, the same code. Cause: `DamageCause::Crush` if `[wp-7]` built it; else the
   value it uses for world damage (fall, environment) and say so under Deviations. **Never add a wire value here.**
   Moving replicated bodies are `Layer::Rubble` and never meet characters: the sweep is the only way they hurt.
8. **Keys** (`strikerKey`, `receiverKey`): the NetId on a server, `entt::to_integral(entity) + 1` offline; terrain 0.
9. **Shatter planes** shift by `shift = voxel::HashMix64(strikerKey) & 1` voxel (0 or 1: "within one voxel"); the
   `seed` argument of `Scene::Shatter` is that value. No `rand`, no time.

## Skeleton

Card 3 lands the impact skeleton (stubs `// [si-2] card N fills this`, tests `DISABLED_`); cards 1-2 are refactors with
their own tests and land no stubs.

`engine/src/voxel/World.cppm`, before `class World` (card 2):

```cpp
/// What the graph search reads of a chunk grid (World; SI-1's model tiles later). Main thread.
class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    virtual uint64_t InstanceId() const = 0;                                  ///< changes with the grid (cache key)
    virtual CellState StateOf(const glm::ivec3& chunk) const = 0;
    virtual bool IsChunkLoaded(const glm::ivec3& chunk) const = 0;
    virtual uint64_t ChunkRevision(const glm::ivec3& chunk) const = 0;
    virtual const Chunk* GetChunk(const glm::ivec3& chunk, uint8_t* uniformId = nullptr) const = 0;
    virtual bool ReadChunk(const glm::ivec3& chunk, Chunk& out) const = 0;
    virtual std::span<const uint8_t> PackedData(const glm::ivec3& chunk) const = 0;
    virtual bool FullHeight() const = 0;                                      ///< fixed rows: bottom row anchors
    virtual glm::ivec3 WindowMinChunk() const = 0;
    virtual glm::ivec3 WindowSize() const = 0;
};
// class World final : public ChunkSource { ... the existing members become `override`; WindowSize / WindowMinChunk
// return by value in the interface (World keeps its const& accessors under other names only if a caller needs them).
```

`engine/src/voxel/Structure.cppm`: every `const World&` parameter of `TerrainGraphCache`, `SearchIslandsOnGraph` and
`BuildChunkGraph` callers becomes `const ChunkSource&` (World converts implicitly: no caller changes). Exported for
tests and SI-1 (card 1):

```cpp
/// Phase 3 of the load check: arc pairs of the judged network, in the terrain's order (shred.md Part 4b step 3).
/// The network view hides whether nodes come from a GraphTable or a tile list.
struct NeckNetworkHash { uint64_t nodes = 0, arcs = 0; };
NeckNetworkHash HashNeckNetwork(const NeckNetwork& network);      ///< tests: the card 1 golden
```

`engine/src/math/Util.cppm`: `uint64_t ICbrt64(uint64_t v);` (floor cube root, exact for every `uint64_t`).

`engine/src/physics/PhysicsWorld.cppm` (`ContactEvent` gains):

```cpp
float invMassA = 0.0f, invMassB = 0.0f;   ///< Jolt inverse masses; 0 static, kinematic, frozen
```

`engine/src/sim/Impact.cppm` (partition `vortex.sim:impact`, `export import :impact;` in `Sim.cppm`; imports
`:simulation`, `:blast`), `internal/Impact.cpp`:

```cpp
struct ImpactQ {
    glm::ivec3 centreQ{0};            ///< 1/16 voxel, floor(p / vs * 16)
    std::array<int8_t, 3> normal{};   ///< x 127, from the striker into the receiver
    uint16_t speedQ = 0;              ///< v_n, cm/s
    uint32_t energyQ = 0;             ///< J / 16
    uint32_t strikerKey = 0, receiverKey = 0;   ///< decision 8; receiver 0 = terrain
    uint8_t terrainId = 0;            ///< voxel half a voxel along +normal from the contact
    uint32_t tick = 0;
    friend bool operator==(const ImpactQ&, const ImpactQ&) = default;
};
/// Total order: energyQ desc, strikerKey, receiverKey, centreQ x, y, z.
bool ImpactBefore(const ImpactQ& a, const ImpactQ& b);
struct ImpactState { uint32_t readyTick = 0; bool shattered = false; };    ///< component on every RigidBody entity
struct ImpactStats { uint32_t drained = 0, eligible = 0, deduplicated = 0, resolved = 0, craters = 0, belowCrater = 0,
                     hardSurface = 0, dents = 0, shatters = 0, droppedAge = 0, droppedBudget = 0, droppedBounds = 0,
                     droppedCredit = 0, droppedSlot = 0, droppedPending = 0; };
struct ImpactLimits {
    uint32_t craters = 1, dents = 4, shatters = 1;
    StructureCredit* credit = nullptr;            ///< server; null offline
    const RegionDesc* region = nullptr;           ///< server: CastFitsRegion; null offline (window rows test)
    std::function<bool(uint32_t modelBytes)> entityCredit;   ///< server: entityCredit >= bytes; empty offline: true
};
/// Step 1-3 of the design: drain, quantise, dedupe, sort, push into PendingImpacts. Called inside StepPhysics.
void CollectImpacts(Simulation& simulation, std::vector<physics::ContactEvent> events);
/// Step 4: resolve pending impacts in key order under the limits. Results have cause Impact and `deferred` set.
std::vector<BlastResult> ResolveImpacts(Simulation& simulation, const ImpactLimits& limits);
struct CrushHit { uint8_t slot = 0; uint16_t amount = 0; uint32_t netId = 0; };
struct CrushTarget { uint8_t slot = 0; glm::vec3 feet{0.0f}; float radius = 0.3f, height = 1.8f; };
/// Decision 7: the crush sweep over awake replicated bodies; `lastHit` keys (netId << 8 | slot) -> tick.
std::vector<CrushHit> SweepCrush(Simulation& simulation, std::span<const CrushTarget> players, uint32_t tick,
                                 std::map<uint64_t, uint32_t>& lastHit);
```

`engine/src/sim/Simulation.cppm`: `SimulationConfig::impacts = false`; `std::span<const ImpactQ> PendingImpacts()
const`; `ImpactStats& Impacts()`. `engine/src/sim/Blast.cppm`: `BlastOptions` gains `bool carveAsClear = false; bool
objects = true; bool push = true; bool deferred = false;`; `BlastResult` gains `uint8_t cause = 0` (`kCauseCast` 0,
`kCauseImpact` 1, `kCauseSite` 2). `engine/src/scene/Scene.cppm`: `std::vector<Entity> Shatter(Entity entity, uint32_t
blocks, uint32_t seed, std::vector<DamageFragment>* fragments);`.

Tests: `engine/tests/physics/ContactTests.cpp` (`PhysicsWorld.*`), `engine/tests/sim/ImpactTests.cpp` (`Impact.*`,
`Crush.*`), `engine/tests/core/StructureSourceTests.cpp` (`Structure.*` chunk source), additions to `NeckTests.cpp`.
Helpers: `ImpactYard`, `DropBlock`, `FakeChunkGrid` (never `Chunk`, `World`, `Impact`).

## Card 1: NeckCheck split into its phases, bit for bit

| | |
|---|---|
| Goal | The load check is six named functions with one network builder SI-1 can reuse; nothing changes |
| Depends on | - |
| Effort | 1-1.5 d (e) |
| Touches | voxel (Structure). Wire: no. Every `Neck.*` result: unchanged |

**Read first**: shred.md Part 4b "The check, step by step"; `Structure.cpp` `NeckCheck` phase by phase (`sed -n` from
`// 1. The judged nodes` to `// 6. Voxels` and the function end: about 430 lines).

**Steps**
1. **Golden first.** Add a test-only hook: `GraphSearchLimits::onNeckNetwork` (`std::function<void(const NeckNetwork&)>`,
   empty by default) called in phase 4 with the network as solved. Add `HashNeckNetwork` (`HashMix64` over supplies, then
   each arc's from, to, cap, capBack, in order). Write `Neck.NetworkGoldenOnTheFixtures` (below), run it on the
   unchanged code, put the printed values into the test, commit (`[si-2] card 1: neck network goldens`).
2. Split into functions in `Structure.cpp`'s anonymous namespace, one per phase, in a `NeckWork` struct holding the
   locals they share (`units`, `judged`, `masks`, labels, the `NeckNetwork`, counters):
   `NeckJudge` (1), `NeckExteriorFloods` (2), `BuildNodeNetwork` (3: over a `NeckGraphView` with `NodeCount`, `Node`,
   `ForEachBond(i, fn)` - the `GraphTable` view only in this stage), `NeckSolve` (4), `NeckReleasedParts` (5),
   `NeckRelabel` (6). `NeckCheck` becomes their calls in order. Move code; do not rewrite loops.
3. No new allocation per call beyond today's; `GraphNode` stays 72 bytes.

**Tests**
- `Neck.NetworkGoldenOnTheFixtures`: the tower (`--neck-blast` setup as `Neck.*` builds it, W = 2 and 11), the castle
  case, `Neck.MaxFlowReleasesTheLargestOverloadedSet`'s arena: network hash and released voxel count equal the
  recorded goldens.
- Existing, unchanged: `Neck.*`, `FaceFlood.*`, `Structure.*`, `Blast.Neck*`.

**Checks** build; `tools/ctest.sh -R 'Neck\.|FaceFlood\.|Structure\.|Blast\.'`; `tools/ctest.sh`; G10 by hand:
`./build/Shredborne --headless --validation --frames 200 --neck-blast` and `--neck-blast 11`, `--neck-blast 12` (lines
as CLAUDE.md G10; flow work 7055).

**Must not break** "World edits and blasts" (load check: unique released set; `GraphNode` 72 B; never after FillBox).

## Card 2: A chunk-source interface under the graph search

| | |
|---|---|
| Goal | The graph cache and search read any chunk grid, not only a `World` |
| Depends on | card 1 |
| Effort | 1 d (e) |
| Touches | voxel (World, Structure). Results: unchanged |

**Steps**
1. Declare `ChunkSource` (Skeleton); `World` derives from it; mark the ten members `override` (`WindowSize` /
   `WindowMinChunk` return `glm::ivec3` by value in both).
2. Replace `const World&` by `const ChunkSource&` in `GraphTable`, `VoxelSource`, `TerrainGraphCache` (`BeginPass`,
   `Get`, `Evict`, `InjectForTesting`), `SearchOnce`, `SearchIslandsOnGraph`. `BeginPass` keys on `InstanceId()` as today.
3. `World` methods the search uses that are not in the interface: none may remain (`grep -n 'World' Structure.cpp`
   after the change lists only comments and the `World` overloads that forward).

**Tests** (`StructureSourceTests.cpp`)
- `Structure.SearchOverAChunkSourceEqualsTheWorld`: a `FakeChunkGrid` (a `std::map` of chunks copied from a World with a
  tower fixture, Dense and Uniform, no Packed) gives the same `GraphSearchResult` islands, neck counters and released
  set as the World.
- `Structure.CacheKeysOnTheSourceInstance`: two sources with equal chunks but other instance ids rebuild.

**Checks** as card 1 (the goldens of card 1 unchanged).

## Card 3: Impact skeleton; contacts carry inverse masses, published in content order, drained every tick

| | |
|---|---|
| Goal | The contact queue never fills; the kept contact per pair never depends on Jolt's threads |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | physics, sim (Skeleton). Wire: no |

**Steps**
1. Land the Skeleton's impact declarations with stubs; tests of cards 3-9 written in full, `DISABLED_`.
2. `OnContactAdded`: `invMassA = body1.IsDynamic() && !frozen ? body1.GetMotionProperties()->GetInverseMass() : 0`
   (same for B). Frozen: the body's frozen flag (`PhysicsWorld::SetFrozen`).
3. `Publish` sort key: (bodyA, bodyB, approachSpeed desc, position x, y, z, normal x, y, z), floats compared as their bit
   patterns (`std::bit_cast<uint32_t>`) where the speed ties; keep the first per pair.
4. `Simulation::StepPhysics` (and `Step`): after the physics step, `auto events = physics.TakeContactEvents();` then
   `impacts ? CollectImpacts(*this, std::move(events)) : (void)0` (dropped, counted in `drained`).
5. `math::ICbrt64` (binary search on `[0, 2^22)`, `uint64_t` products checked against overflow by `v / m / m`).

**Tests**
- `PhysicsWorld.ContactEventsCarryInverseMasses` (static side 0, a frozen body 0, a 2 kg box 0.5).
- `PhysicsWorld.PublishOrderIsByContent` (a box landing flat on 4 triangles: the kept event has the lowest position
  bits whatever the arrival order; arrival shuffled through a test hook on the collector).
- `Impact.ContactQueueIsDrainedEveryTick` (10 000 ticks of 20 bouncing bodies, impacts off and on: no warning, queue
  under its cap).
- `Math.ICbrt64IsExact` (every cube k^3 and k^3 - 1 for k up to 2^21, plus `UINT64_MAX`).

**Checks** build; `tools/ctest.sh -R 'PhysicsWorld\.|Impact\.|Math\.'`; `tools/ctest.sh`; doc-check.

## Card 4: ImpactQ: eligibility, quantising once, dedupe, order, immunity

| | |
|---|---|
| Goal | Each landing becomes one integer `ImpactQ`, the same whatever the thread count |
| Depends on | card 3 |
| Effort | 1 d (e) |
| Touches | sim (Impact, Simulation) |

**Steps** (`CollectImpacts`)
1. `ImpactState` stamped by an `on_construct<scene::RigidBody>` observer in `Simulation`: `readyTick = Tick() + 30`.
2. Per event, for each side: eligible striker = Dynamic, voxels >= `scene::VoxelsForVolume(kServerReplicatedPieceVolume,
   vs)` (262 at 0.125 m, offline too), `Tick() >= readyTick`, on a server a `NetId`. Receiver: terrain (`userData` 0),
   a NetId body (server) or any body (offline). Neither side eligible: skip.
3. Quantise at once: `centreQ = floor(p / vs * 16)`; normal from striker into receiver (negate when the striker is B),
   `round(n * 127)`; `speedQ = round(v_n * 100)`; `E = 0.5 * v_n^2 / (invA + invB)` (both 0: skip),
   `energyQ = min(UINT32_MAX, floor(E / 16))`; `terrainId = world.GetVoxel(floor((p + n * vs / 2) / vs))`.
4. Floors: `energyQ < kImpactMinEnergyQ` or `speedQ < kDentMinSpeedQ`: dropped (not counted as eligible).
5. Dedupe per unordered pair (striker = the eligible side with the larger key), keep the `ImpactBefore`-first; sort by
   `ImpactBefore`; append to pending; over 64: drop the last by key (`droppedPending`).

**Tests**: `Impact.QuantisedOnceAndTotallyOrdered`, `Impact.SameImpactsWithOneOrThreeThreads`
(`PhysicsConfig::threads` 1 and 3: equal `ImpactQ` lists), `Impact.NormalPointsIntoTheReceiver`,
`Impact.OnePairOneImpact`, `Impact.NewBodiesAreImmune`, `Impact.SmallBodiesCratesAndRubbleNeverStrike` (pending only).

**Traps** Never decide anything from a float after step 3. Jolt makes the higher motion type body 1: do not assume
the striker is A between two dynamic bodies.

## Card 5: BlastOptions: carveAsClear, objects, push, deferred

| | |
|---|---|
| Goal | A crater is the carve rule as one `ClearMask`, and can run without pushing or damaging bodies |
| Depends on | card 3; `[mag-1]` `BlastOptions{noCarve}` |
| Effort | 0.5 d (e) |
| Touches | sim (Blast). Results with default options: unchanged |

**Steps**
1. `carveAsClear`: compute the carve's voxels with the integer carve rule on the world (read-only, the same visit as
   `CarveSphereQ`), emit `WorldOp::Clear(min, mask)` over their box instead of the `Carve`.
2. `objects = false`: skip `Scene::Damage`. `push = false`: every `Detach` velocity 0, no `AddImpulse`; bodies in the
   box still woken (`ActivateInBox`). `deferred`: copied into the result.
3. `PresentBlast` / `Engine::PresentBlast`: move the fragment loop above the `deferred` early return (shred.md step 5).

**Tests**: `Impact.CraterIsTheCarveRuleAsAMask` (equal voxels and `WorldHash` with a `Carve` on a copy),
`Impact.PushFalseMeansStill`. All `Blast.*` unchanged.

## Card 6: ResolveImpacts: craters and dents

| | |
|---|---|
| Goal | Heavy landings crater soft ground; a falling block dents a still object |
| Depends on | cards 4, 5 |
| Effort | 1 d (e) |
| Touches | sim (Impact) |

**Steps** (`ResolveImpacts`, pending in key order, until the per-call limits or, server, `credit->credit <= 0`)
1. Age: `Tick() - tick > kImpactMaxAgeTicks`: dropped (`droppedAge`).
2. Terrain receiver: crater needs `energyQ >= kCraterEnergyQ`, `speedQ >= kCraterMinSpeedQ` (else `belowCrater`) and
   `hardnessQ(terrainId) <= powerQ` (else `hardSurface`). Sphere at `centreQ`, radius `rQ` (decision 5). Server:
   `CastFitsRegion(*region, world, sphere)` false -> `droppedBounds` (never a fail); offline the window rows test of a
   cast. Then `Blast(SphereSite(world, sphere, powerQ), {.carveAsClear = true, .objects = false, .push = false,
   .deferred = true})`, `cause = kCauseImpact`. Server: `credit->Spend(CastWorkOf(result).Total(), bytes of its ops)`.
3. Body receiver: dent only if the receiver is still (linear speed < 0.05 m/s) and the entity credit allows its model
   bytes: `Scene::Damage(receiver, contact, r)`, `r = min(0.5 m, 0.2 m x cbrt(E / 20 kJ))` (the dent
   `rQ` of decision 5). Moving receiver: nothing. The striker is never dented.
4. Counters into `ImpactStats`; one log line per resolved impact (the `--impact-drop` lines below).

**Tests**: `Impact.HeavyDropCarvesLightDoesNot` (512-voxel block on `FillBox` ledges, 5 m: crater voxels > 0; 1 m:
`belowCrater`, 0 voxels), `Impact.SpeedFloorStopsTrenches`, `Impact.HardSurfaceIsNeverCratered`,
`Impact.StrikerIsNeverDented`, `Impact.DentsOnlyAStillReceiver`, `Impact.CraterRunsTheLoadCheck`,
`Impact.OutsidePlayBoundsCarvesNothing`, `Impact.BudgetPerTickAndAge` (20 blocks in one tick: 1 crater, 4 dents, rest
dropped after 10 ticks), `Impact.TwoSimulationsFedTheOpsHaveEqualHashes`.

**Must not break** "World edits and blasts" (integer ops; the load check never after FillBox: a crater is a blast, not a
fill); L8, L9.

## Card 7: Shatter

| | |
|---|---|
| Goal | A felled tower breaks into 2-3 blocks once, never again |
| Depends on | card 6 |
| Effort | 1 d (e) |
| Touches | scene (Shatter), sim (Impact) |

**Steps**
1. After the terrain or dent step, the striker if it still exists and is eligible: `E / mass >= 30 J/kg`, as integers
   `uint64_t(energyQ) * 16 >= 30 * massKg` (`massKg` = the body's mass rounded down, taken when the `ImpactQ` was made
   and kept beside it in the pending list, never in `ImpactQ`'s key), longest model axis >= 32 voxels, not
   `shattered`; server: entity credit and `TakePieceSlot` grants >= 1 (asked before cutting; none: `droppedSlot`).
2. `Scene::Shatter(entity, blocks = min(granted + 1, 3) (offline 3), seed)`: cut along the longest model axis into equal
   slabs, plane shift by decision 9; `SplitComponents` per slab; pieces through `Damage`'s piece path (the largest keeps
   entity and NetId; under the replicated size: fragments). Every piece `shattered = true`; `readyTick` fresh.
3. `Damage` copies the parent's `shattered` to its pieces (explicitly, after making them).

**Tests**: `Impact.FelledTowerShattersOnce` (the 4b neck tower released by a cast lands on turf: crater, 2-3 blocks,
all `shattered`, a block later split by `Damage` keeps it, none shatters again).

## Card 8: Offline: the game resolves impacts; --impact-drop

| | |
|---|---|
| Goal | Offline, a dropped block craters and a felled tower breaks, shown as dust |
| Depends on | card 7 |
| Effort | 0.5-1 d (e) |
| Touches | engine (`Engine::Run`), game (`src/main.cpp`). Gates: G3-G10 lines unchanged |

**Steps**
1. `Engine::Run`: after the `RetryIslandSearches` loop, `for (auto& r : sim::ResolveImpacts(sim, {})) PresentBlast(r);`
   (deferred: dust and debris, no burst, no flash). The game's offline config sets `impacts = true`; `--no-impacts`.
2. `--impact-drop` (beside the plaza, not in the default world; shred.md "Headless"): frame 20 three 512-voxel blocks on
   `FillBox` ledges (5 m and 1 m over dirt, 5 m over the paving); frame 50 remove the ledges; frame 60 a 16 x 56 x 16
   stone column from 4 m onto turf. Lines (exact):
   `Impact: block (512 voxels, 2.6 t) at V m/s, E kJ: crater r R m (power P), N voxels`;
   `Impact: block (512 voxels, 2.6 t) at V m/s, E kJ: below the crater`;
   `Impact: block (512 voxels, 2.6 t) at V m/s, E kJ: hard surface (stone_dark)`;
   `Impact: column (14336 voxels, M t) at V m/s, E kJ: shattered into K pieces`;
   exit line `Impacts: drained D, resolved R, craters C, dents T, shatters S, dropped X`. Checks (exit 3): the four lines
   present, 2 <= K <= 3, `store a/b` under 7000. `--neck-blast` also logs `Impact: tower ...` (not checked by G10).

**Checks** build; `./build/Shredborne --headless --validation --frames 300 --impact-drop` at default, `--voxel-size 0.1`
and `0.15` (exit 0); `VERIFY_SLOT=1 tools/verify.sh quick` (G3-G10 PASS; look at the screenshots).

**Owner** windowed: a tower landing looks right (docs/owner-checks.md, card 11).

## Card 9: The server: RunImpacts on the structure credit, entity credit, no ToRubble

| | |
|---|---|
| Goal | In a session, landings break the same for every client, inside the budgets |
| Depends on | card 7 |
| Effort | 1 d (e) |
| Touches | net (Server, ServerStats). Wire: no (`ClearMask`, `Detach`, `ObjectModel`, `EntitySpawn`, despawns) |

**Steps**
1. `ServerConfig::impacts = true` (`--no-impacts`); the server simulation's `SimulationConfig::impacts`.
2. Tick order (shred.md step 5): `Receive`, streaming, players, `RunCasts`, **`RunImpacts`**, site checks
   (`StructureCredit::Run`), `StepPhysics` (collects), lifetimes, despawns, `TrackBodies`, flush, snapshots.
   `RunImpacts`: `ResolveImpacts(sim, {.credit = &ctx.structure, .region = &region, .entityCredit = ...})`; each result's
   ops through `EmitOp`, object events through `EmitObjectEvents` gated so they never evict (`ToRubble`); a skipped one
   counts `impactsSkippedByCredit`.
3. Client: its simulation has `impacts` false (drains to nothing). A `deferred` group shows no burst and no flash.
4. Exit line: `Impacts: server: drained D, resolved R, craters C, dents T, shatters S, dropped X, skipped by credit K,
   ToRubble by impacts 0`. `vortex_server` fails (exit 1) when the last field is not 0.

**Tests** (net): `Entities.FelledTowerBreaksTheSameOnBothClients` (5 % loss: hashes equal, ids, models, rest poses
agree), `Presentation.ImpactsAndRechecksAreDeferred`, `Entities.ImpactsNeverCauseToRubble`,
`CastBudget.ScheduleFollowsCountsNotTimeOrCache` with impacts on (unchanged expectations; if Jolt differs across thread
counts, run it with impacts off and say so: design risk 5).

**Checks** build; `tools/ctest.sh -R 'Entities\.|Presentation\.|CastBudget\.|Replication\.'`; `tools/ctest.sh`; G11 and
G13 by hand (`hashes equal`, every `Entities: ... agree`, the `Impacts: server:` line with ToRubble 0).

**Must not break** "Replicated bodies" (2 new server bodies per tick, caps, rest and wake events), "Chunk sync" (every
op through `EmitOp`), R5 (impacts never touch the cast credit).

## Card 10: Crush damage

| | |
|---|---|
| Goal | A tower falling on a player kills; a resting block or small rubble never hurts |
| Depends on | card 9; `[wp-7]` damage |
| Effort | 0.5-1 d (e) |
| Touches | sim (Impact: `SweepCrush`), net (Server), the offline in-process server. Wire: no |

**Steps**
1. `SweepCrush` (decision 7): bodies by NetId ascending; players by slot ascending; box test in floats (server-only).
   `lastHit[(netId << 8) | slot]` blocks a repeat for 30 ticks; entries older than 30 ticks pruned each call.
2. Server: after `StepPhysics`, targets = living players; each hit -> `[wp-7]`'s damage entry with the crush cause,
   source 0 (no player). Kill feed word `CRUSHED` (game; HUD text only).
3. Log at exit: `Crush: hits H, kills K`.

**Tests**: `Crush.FallingBlockKillsAPlayerUnderIt` (a 2 x 2 x 2 m stone block from 8 m onto a player with 100 health:
dead), `Crush.RestingBodyNeverHurts` (a player walking into a resting block: 0), `Crush.SmallRubbleNeverHurts` (200
voxels: 0), `Crush.OncePerBodyPerHalfSecond` (a block sliding over a player: hits 30 ticks apart).

**Checks** build; `tools/ctest.sh -R 'Crush\.|Impact\.'`; `tools/ctest.sh`.

## Card 11: Close [si-2]

| | |
|---|---|
| Goal | Verified, measured and documented |
| Depends on | cards 1-10 |
| Effort | 0.5-1 d (e) |

**Steps**
1. `tools/session-check.sh`: fail on `ToRubble by impacts` not 0 (verdict, fixture and `Tools.SessionCheckVerdict`
   together, rule 22).
2. Measure and report: `--impact-drop` landing frame ms offline (after fill, `--after-fill`); the drill's worst tick with
   impacts (`ServerStats::maxTickSeconds`); bytes per tower landing.
3. Docs: shred.md stage row `[si-2]` -> Built and the "Now" line; ROADMAP Now / Next, the test count, "Destruction" gaps
   row; CLAUDE.md section 5 "World edits and blasts" (impacts: integer after `ImpactQ`, structure credit, no ToRubble)
   and "Replicated bodies" (crush sweep); notes/physics.md (contacts drained); notes/voxel.md (`ChunkSource`, the phase
   functions); docs/verify.md (`--impact-drop`, `--no-impacts`, the `Impacts:` and `Crush:` lines);
   docs/owner-checks.md (a tower landing, a crushing).
4. `grep -rn 'DISABLED_' engine/tests tests`: no `[si-2]` test left.
5. `VERIFY_SLOT=1 tools/verify.sh full --ci`; `VERIFY_SLOT=1 tools/verify.sh soak` in the background.

**Done when** - [ ] full --ci PASS; - [ ] `--neck-blast` flow work 7055 and every `Neck.*` unchanged; - [ ] the drill
logs `ToRubble by impacts 0`; - [ ] numbers reported.

## Values this stage lands

No wire value, no protocol bump, no rule revision, no `buildHash` input, no gate. New log lines (docs/verify.md):
`Impact: ...`, `Impacts: ...`, `Impacts: server: ...`, `Crush: hits H, kills K`. New flags: `--impact-drop`,
`--no-impacts` (game and `vortex_server`). `[mag-7]` builds `extraLoad` on the phase functions of card 1; SI-1 builds
its tile view on `ChunkSource` and `BuildNodeNetwork`.
