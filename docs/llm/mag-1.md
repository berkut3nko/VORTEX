# [mag-1] Siege core in sim, headless only: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/magic|magic]]

| | |
|---|---|
| Stage | `[mag-1]`: four spells in sim, offline and headless: Siege Cannon (breach), Barricade Wave (cover), Shockwave (knock off), Updraft (climb) |
| Design | [magic.md](../design/magic.md) "Decision in brief", "The two axes as data", "The four operators (exact)" (Pierce, Burst, Plume's column), "The element verbs", "Conjured matter", "Players: zones, statuses and the movement rule", "Determinism and replication" ("Apply", "Determinism traps"), "Tests", "Headless flags and gates", "Stages" row `[mag-1]` |
| Code read at | engine `bafca16`, game `0e27ea7` (before `[wp-4]`..`[mech-1]`: where a card builds on what those stages add, it names the symbol and says "read the built interface") |
| Depends on | `[wp-6]` (rules seam, `vortex.sim:rules`), `[wp-7]` (`vortex.sim:combat`: health, teams, charges, spawns), `[wp-8]` (fight baseline), `[mech-1]` (committed before this stage on the ROADMAP order) |
| Cards | 12 |
| Effort | 6-9 d (e) |
| Changes | wire: **no** (`kProtocolVersion` stays; the three op kinds are sim values only). Movement rule: **yes**, `sim::kPlayerRuleRevision` 1 -> 2 in card 9 only. `buildHash`: through `MaterialTableHash` only (card 3 adds `knit_iron`); `SpellTableHash` and `TurningTableHash` are computed here and join `buildHash` in `[mag-2]`. Gates: **G22** (`--siege-matrix`, card 11) |

## Before you start

- Committed in both repositories: `[wp-6]`, `[wp-7]`, `[wp-8]`, `[mech-1]`. Check with
  `git log --oneline | grep -E '\[(wp-6|wp-7|wp-8|mech-1)\]'` in the game root and in `engine/`. If one is missing,
  stop (`Status: blocked`).
- What exists today and what this stage builds on (verify each with `grep -n`):
  - `engine/src/sim/WorldOp.cppm`: `sim::WorldOpKind` (1-6), `sim::WorldOp` (one struct for every kind, factories
    `Carve`, `Fill`, `Swap`, `Box`, `Detach`, `Clear`), `sim::ApplyOp`, `sim::OpChunkRange`, `sim::OpTouchesChunk`.
  - `engine/src/voxel/World.cppm`: `voxel::SphereQ`, `InSphereQ`, `SphereVoxelBounds`, `SphereTouchesBox`,
    `EditReport` (`changed`, `skipped`, `unsynced`, `outOfBounds`), `ChunkClip`, `World::CarveSphereQ`,
    `World::ClearMask`, private template `World::EditRegion(min, max, decide, touches, edits, report, clip, rows)`
    (chunks in (y, z, x) order, voxels in (z, y, x) order inside a chunk), `World::Raycast`, `World::ReadChunk`,
    `World::IsChunkLoaded`, `World::GetVoxel`. In `internal/World.cpp`: `CarvableIds`, `FloorDiv64`, `CeilDiv64`
    (anonymous namespace).
  - `engine/src/voxel/Volume.cppm`: `voxel::VoxelMask` (bits in x-fastest order, `Index`, `Get`, `Set`,
    `AnyInRange`, `Count`).
  - `engine/src/voxel/Material.cppm`: `MaterialFlags` bits 0-3 used (`Indestructible`, `Flammable`, `Liquid`,
    `Foliage`), `voxel::WorldMaterials` (ids 1-20, `bedrock` = 20), `RegisterDefaultWorldMaterials`.
    `voxel::MaterialTableHash` (`Hash.cppm`) hashes hardness, flags, density and strength of all 255 ids.
    `voxel::HashMix64` is exported from `Hash.cppm`.
  - `engine/src/sim/Blast.cppm`: `sim::Blast(Simulation&, const voxel::SphereQ&, uint16_t power)`, `BlastResult`,
    `AppliedOp`, `CastWork`, `CastWorkOf`, `OpWork`, `kOpWork` (100), `kColdChunkGraphWork` (200),
    `kIslandSearchMargin` (24), `kCastWorkPerTick` (60 000), `FloodBox`, `IslandBox`, `RetryIslandSearches`,
    `PresentBlast`. `voxel::kFloodHalfExtent` (96) is in `Connectivity.cppm`.
  - `engine/src/sim/Simulation.cppm`: `sim::Simulation` (pimpl), `DeferredIslandSearch { SphereQ sphere; uint16_t
    power; ... }`, `Simulation::Tick()`.
  - `engine/src/sim/Player.cppm`: `PlayerState`, `PlayerTuning`, `PlayerInput`, `PlayerButton`, `StepPlayer`,
    `TakeCast`, `kPlayerRuleRevision = 1`, `LookDirection`, `PlayerBounds`, `PlayerReach`.
  - `engine/src/physics/PhysicsWorld.cppm`: `PhysicsWorld::UpdateCharacter(character, dt, desiredVelocity,
    jumpSpeed = 0.0f)` (the horizontal wish only; y ignored).
  - `engine/src/sim/Rules.cppm` (`[wp-6]`) and `engine/src/sim/Combat.cppm` (`[wp-7]`): read their exported names
    now (`grep -n 'export\|struct\|enum\|^    [a-zA-Z].*(' engine/src/sim/Rules.cppm engine/src/sim/Combat.cppm`).
    This pack uses `sim::TeamId`, the team spawn points and the damage entry point by those names; if a built name
    differs, use the built one and list it under "Deviations".
  - Source files are globbed (`vortex_add_module_library`, `vortex_add_tests` with `CONFIGURE_DEPENDS`): a new
    `.cppm`, `.cpp` or test file needs **no** CMake line. A new partition must be added to its module's primary
    interface (`export import :x;` in `engine/src/sim/Sim.cppm` or `engine/src/voxel/Voxel.cppm`).
  - Game: `src/main.cpp` (92 KiB: never read it whole). Headless scripts run in `OnUpdate` from `m_ScriptFrame`
    (grep `UpdateNeckBlast`, `m_ScriptFrame`, `SetNeckBlast`, `Expect(`); they call `sim::Blast(engine.Simulation(),
    ...)` and `engine.PresentBlast(...)` directly. Failed checks go through `m_Checks.Expect(...)` (exit 3).

## Decisions

Settled here (us; the owner may veto). Each is binding for every card.

1. **Numbers.** `WorldOpKind::MapShape = 7`, `MapMask = 8`, `CarveCapsule = 9` (sim values in this stage; they go on
   the wire in `[mag-2]`, which then records them in the registry). Palette id 21 `knit_iron`. `MaterialFlag_Conjured
   = 1u << 4`. `kPlayerRuleRevision = 2` (card 9). Gate G22 = `--siege-matrix` (card 11). If a value is taken when
   your card lands, take the next free one and say so.
2. **Shapes live in voxel.** `voxel::ShapeQ` sits in `World.cppm` beside `SphereQ`; `sim::WorldOp` gains `map`,
   `shape` and `axis` fields. `CarveCapsule` uses `shape` (kind capsule) and `power`; `MapMask` uses `min` (origin),
   `mask`, `map`, `axis`; `MapShape` uses `map`, `shape`.
3. **Chunk ranges by box.** `OpChunkRange` uses the op's voxel box for all three kinds; `OpTouchesChunk` is by box for
   `MapShape` and `CarveCapsule` (conservative, which the design allows: "Box-based") and exact for `MapMask` (the
   same row test as `ClearMask`). The `touches` callback inside `EditRegion` follows the same rule.
4. **The mask's run axis is an encoding choice only.** `ApplyOp` ignores `axis`. This stage adds the run encoding in
   voxel (`MaskRuns`, `MaskFromRuns`, `MaskRunBytes`, `ShortestRunAxis`) so `[mag-2]`'s wire and this stage's byte
   counts share it. Axis order: the run axis is fastest, the other two follow in x, y, z order (axis 0: x, y, z;
   axis 1: y, x, z; axis 2: z, x, y). Runs alternate clear / set and start with a (possibly empty) clear run, as
   `WriteMaskRuns` does today. Ties of `ShortestRunAxis` go to the lower axis.
5. **Map 0 and unknown maps.** `ApplyOp` with `map == 0` or `map >= 16` changes nothing and returns 0 changed
   (no voxel decision, no report entries).
6. **Bore cost table.** `SpellTable::boreCost[id]` is authored in code by id (card 4 lists every value);
   `Materials.BoreCostMatchesHardness` checks it equals `voxel::QuantizePower(hardness)` for every id that is neither
   liquid nor indestructible. Liquids cost 0; indestructible non-liquids stop a bore (`kBoreStop = 0xFFFF`).
7. **Where operators live.** One partition `vortex.sim:propagate` (`Propagate.cppm`, `internal/Propagate.cpp`) holds
   the voxel cache, the walls and the three operators this stage needs (Pierce, the Burst strip sweep, Plume's single
   column). Flow and the full Plume / Burst disc are `[mag-3]`/`[mag-4]`.
8. **Blasts become site-based.** `sim::Blast` gets an overload `Blast(Simulation&, const BlastSite&, const
   BlastOptions&)`; the sphere overload forwards to it with `SphereSite(...)` and must give bit-identical results
   (every existing test and G3-G10 unchanged). `DeferredIslandSearch` stores the `BlastSite`.
9. **Site checks** (expiries and anything else that turns a collider into a non-collider) run one chunk at a time
   on the minimal structure credit: island box = the chunk grown by `kIslandSearchMargin`, flood box = the chunk's
   centre +- `kFloodHalfExtent`, `BlastOptions{.noCarve = true}`, no objects, no push. Credit: `15 000` per tick,
   `credit = clamp(credit + 15000, -90000, 15000)`, a site runs while `credit > 0`, charged `CastWorkOf(result).Total()`
   (shred.md SI-2's numbers). Queue: FIFO, at most 256 chunks, a chunk already queued is not queued twice, a full
   queue drops the new chunk (counted).
10. **An absorbed blast runs its island search inline** (on the cast's own work, as a blast does), not as a site
    check: ops = `[MapShape(Ablate, sphere hitQ, 0.25 m)]`, then `Blast(site, {.noCarve = true})` for islands,
    objects and push. The design's "runs a site check like an expiry" is met by that search (same chunks, earlier).
11. **Server-only spell state is the caller's.** `sim::CastContext` (registry, schedule, structure credit, spawn
    areas, casters, zone owners) is owned by whoever casts (the offline game now, `net::Server` in `[mag-2]`), passed
    by reference. Only the `ZoneSet` lives in the `Simulation` (every peer evaluates zones).
12. **Teams in zones.** A zone's `teamMask` has bit `t` set when it acts on team `t` (`0xFF` = everyone). A player's
    team bit is `1u << team`; a player with no team (an offline player before `[wp-7]` gives one) is team 0. "Enemies"
    of caster team `c` = `0xFF & ~(1u << c)`. The caster's team and dispellability of each zone are kept server-side
    in `CastContext::zoneOwners`, never in the zone.
13. **Impulse acts on its start tick only** and is stored with `duration = 1`. Lift acts on `[start, start +
    duration)`. Pin, Chill, Root, Release and Block are reserved values with no behaviour in this stage.
14. **Push.** `PlayerState::push[3]` in cm/s (int16, clamped to +-1500). The horizontal part is added to the wish
    (`push / 100` m/s); the vertical part is a one-shot kick: it becomes `UpdateCharacter`'s `vertical` on the tick
    after it is set and is then zeroed (Jolt carries the flight). Horizontal decay per tick, after the physics call:
    each axis moves toward 0 by `pushDecayGround` (45) on the ground, `pushDecayAir` (1) in the air.
15. **The capsule Impulse** (Siege Cannon's follow-through) pushes along its axis `(B - A)`, scaled by the falloff
    from the axis (`f = strength * max(0, R - len) / R`, `len` = distance from the axis): a fixed direction, so a
    millimetre never reverses it. Sphere and cylinder Impulses push radially as the design gives.
16. **Lift** sets `velocity.y = max(velocity.y, target)` after gravity inside `UpdateCharacter`
    (`std::optional<float> vertical`), and turns off the floor snap (`mStickToFloorStepDown` zero) for that update when
    `*vertical > 0`. Status `Lifted` is set while in a Lift and cleared on the first tick on the ground.
17. **Damage.** `CastSpell` and `RunScheduled` return `SpellDamage{slot, amount}` entries; the caller applies them
    through `[wp-7]`'s damage function with the cause it uses for casts. Player hit tests for damage are server-only
    and may use floats.
18. **Charges.** `CastSpell` does not check or spend `[wp-7]` charges: the caller does (the offline scripts skip
    them; `[mag-2]`'s server checks and spends one before queueing). Form cooldowns and attunement are checked by
    `sim::CheckCapture` from `CastContext::casters`.
19. **No refunds.** A cast refused at run time (missed, outside, caps, near spawn) keeps its charge and form cooldown
    spent, as a missed blast does.
20. **The demo player.** The headless spell scripts cast as team 1 with player slot 0 ("no player") so that the
    offline player (team 0) is an enemy and Shockwave reaches it; Updraft is "everyone".

## Skeleton

Card 1 lands every declaration below with stub bodies (`return {};` / `return false;` / `return 0;`, comment
`// [mag-1] card N fills this`) and every test of this stage written in full and named `DISABLED_<Name>`. Each later
card renames the tests it makes pass.

### voxel (Core)

`engine/src/voxel/Turning.cppm` (partition `vortex.voxel:turning`, add `export import :turning;` to `Voxel.cppm`),
implementation `engine/src/voxel/internal/Turning.cpp`:

```cpp
module;
#include <array>
#include <cstdint>
export module vortex.voxel:turning;
import :material;
export namespace vortex::voxel {
    /// A voxel's new id is map[old]; every map is the identity on ids it does not name. Wire values (map numbers).
    using TurningMap = std::array<uint8_t, 256>;
    enum TurningMapId : uint8_t {
        Map_Ignite = 1, Map_Douse = 2, Map_Knit = 3, Map_Slag = 4, Map_Set = 5, Map_Pitch = 6, Map_Heat = 7,
        Map_Cool = 8, Map_Rime = 9, Map_Thaw = 10, Map_Freeze = 11, Map_FreezeWater = 12, Map_Shatter = 13,
        Map_Unknit = 14, Map_Ablate = 15,
    };
    inline constexpr uint8_t kTurningMapCount = 16;   ///< maps[0] is the identity and never sent
    struct TurningTable { std::array<TurningMap, kTurningMapCount> maps; };
    /// The table for the world palette of RegisterDefaultWorldMaterials (ids by name, authored in code).
    TurningTable BuildTurningTable(const WorldMaterials& ids);
    /// The process-wide table built from a scratch library's default ids (built once, thread-safe static).
    const TurningTable& WorldTurningTable();
    /// Hash of every entry of every map (joins net::BuildHash in [mag-2]).
    uint64_t TurningTableHash(const TurningTable& table);
}
```

`engine/src/voxel/Material.cppm`: `MaterialFlag_Conjured = 1u << 4` (read by the absorb rule, the caps, the piece
rule and the editor); `WorldMaterials` gains `uint8_t knitIron = 0;` (after `bedrock`).

`engine/src/voxel/World.cppm` (beside `SphereQ`):

```cpp
/// A shape in q (1/16 voxel), world voxel space. Wire values: kind.
struct ShapeQ {
    uint8_t kind = 0;            ///< 0 sphere (a, radiusQ), 1 capsule (a, b, radiusQ), 2 box (a = min voxel, b = max voxel, exclusive), 3 y-cylinder (a = centre, radiusQ, halfHeightQ)
    glm::ivec3 a{0}, b{0};
    uint16_t radiusQ = 0, halfHeightQ = 0;
    friend bool operator==(const ShapeQ&, const ShapeQ&) = default;
};
inline constexpr uint8_t kShapeSphere = 0, kShapeCapsule = 1, kShapeBox = 2, kShapeCylinder = 3;
/// Voxel v is in the shape (int64, voxel centre c = 16 v + 8; see card 2 for each test).
bool InShapeQ(const ShapeQ& shape, const glm::ivec3& voxel);
/// Voxel box [min, max) holding every voxel of the shape (empty when none), clipped to +-kMaxEditVoxel.
void ShapeVoxelBounds(const ShapeQ& shape, glm::ivec3& min, glm::ivec3& max);
/// "may be carved" per id for a carve of `power` (the table CarveSphereQ uses), exported for the Pierce walk.
std::array<bool, 256> CarvableIdsFor(const MaterialLibrary* materials, uint16_t power);
```

`World` members (public, beside `ClearMask`):

```cpp
/// id = map[old] for every voxel of the shape, EditRegion order. Returns voxels changed.
uint32_t MapShapeQ(const ShapeQ& shape, const TurningMap& map, std::vector<VoxelEdit>* edits = nullptr,
                   EditReport* report = nullptr, const ChunkClip* clip = nullptr);
/// id = map[old] for every voxel origin + p whose mask bit p is set.
uint32_t MapMaskQ(const glm::ivec3& origin, const VoxelMask& mask, const TurningMap& map, std::vector<VoxelEdit>* edits = nullptr,
                  EditReport* report = nullptr, const ChunkClip* clip = nullptr);
/// Air for every voxel of the capsule (shape.kind must be kShapeCapsule) that is carvable with hardness * 16 <= power.
uint32_t CarveCapsuleQ(const ShapeQ& capsule, uint16_t power, std::vector<VoxelEdit>* removed = nullptr,
                       EditReport* report = nullptr, const ChunkClip* clip = nullptr);
```

`engine/src/voxel/Volume.cppm` (after `VoxelMask`):

```cpp
/// The mask as alternating clear / set run lengths (first run clear, may be 0) in the order of `axis` (Decision 4).
std::vector<uint32_t> MaskRuns(const VoxelMask& mask, uint8_t axis);
/// The inverse: false (mask left empty) when axis > 2 or the runs do not add up to size exactly.
bool MaskFromRuns(const glm::ivec3& size, uint8_t axis, std::span<const uint32_t> runs, VoxelMask& out);
/// Bytes of MaskRuns(mask, axis) as LEB128 numbers.
size_t MaskRunBytes(const VoxelMask& mask, uint8_t axis);
/// The axis whose runs take the fewest bytes (ties: the lower axis).
uint8_t ShortestRunAxis(const VoxelMask& mask);
```

`engine/src/math/Util.cppm`: `uint64_t ISqrt64(uint64_t v);` (floor of the square root, exact for every `uint64_t`).

### physics

`engine/src/physics/PhysicsWorld.cppm`:

```cpp
void UpdateCharacter(CharacterId character, float dt, const glm::vec3& desiredVelocity, float jumpSpeed = 0.0f,
                     std::optional<float> vertical = std::nullopt);
```

### sim

`engine/src/sim/WorldOp.cppm`:

```cpp
enum class WorldOpKind : uint8_t { CarveSphere = 1, FillSphere = 2, SwapSphere = 3, FillBox = 4, Detach = 5, ClearMask = 6,
    MapShape = 7,      ///< `map`, `shape`
    MapMask = 8,       ///< `map`, `min` = origin, `mask`, `axis` (run order on the wire only)
    CarveCapsule = 9,  ///< `shape` (capsule), `power`
};
// in struct WorldOp:
uint8_t map = 0;            ///< MapShape, MapMask: a voxel::TurningMapId (1..15)
voxel::ShapeQ shape;        ///< MapShape: the shape; CarveCapsule: the capsule
uint8_t axis = 0;           ///< MapMask: the run axis of its mask's encoding (0 x, 1 y, 2 z)
static WorldOp Map(uint8_t map, const voxel::ShapeQ& shape);
static WorldOp MapMasked(uint8_t map, const glm::ivec3& origin, voxel::VoxelMask mask);   ///< axis = ShortestRunAxis(mask)
static WorldOp Bore(const voxel::ShapeQ& capsule, uint16_t power);
/// Bytes the op takes on the wire as [mag-2] encodes it (magic.md "Wire values"; kinds 1-6 as WriteEvent today).
size_t OpWireBytes(const WorldOp& op);
```

`engine/src/sim/Blast.cppm` (add):

```cpp
/// What one island search covers and what it carves first (a blast's sphere, a bore's capsule, a site check's chunk).
struct BlastSite {
    WorldOp carve;                        ///< CarveSphere or CarveCapsule; ignored with BlastOptions::noCarve
    glm::ivec3 islandMin{0}, islandMax{0};///< island box [min, max), voxels
    glm::ivec3 floodMin{0}, floodMax{0};  ///< flood box [min, max), voxels; holds the island box
    glm::dvec3 center{0.0};               ///< metres: the push, the objects and the log
    float radius = 0.0f;                  ///< metres: objects and push reach (0: none)
};
struct BlastOptions { bool noCarve = false; };   ///< [si-2] adds carveAsClear, objects, push, deferred
BlastSite SphereSite(const voxel::World& world, const voxel::SphereQ& sphere, uint16_t power);
BlastSite CapsuleSite(const voxel::World& world, const voxel::ShapeQ& capsule, uint16_t power, const glm::ivec3& hitQ);
BlastSite ChunkSite(const voxel::World& world, const glm::ivec3& chunk);
BlastResult Blast(Simulation& simulation, const BlastSite& site, const BlastOptions& options = {});
/// The M2a blast with the absorb rule (card 7): `hitMaterial` is the world id the ray hit (0: a body or unknown).
BlastResult BlastAt(Simulation& simulation, const voxel::SphereQ& sphere, uint16_t power, uint8_t hitMaterial);
bool AbsorbsBlast(const voxel::MaterialLibrary& materials, uint8_t hitMaterial, uint16_t power);
// BlastResult gains: bool absorbed = false;   ///< ops[0] is the Ablate MapShape, no carve
```

`engine/src/sim/Simulation.cppm`: `DeferredIslandSearch { BlastSite site; std::vector<glm::ivec3> carvePending; bool
waitsForChunk; glm::ivec3 waitChunk; uint32_t attempts; }` (replaces `sphere`, `power`); `ZoneSet& Zones(); const
ZoneSet& Zones() const;` (the partition `:simulation` imports `:zones`).

`engine/src/sim/Zones.cppm` (partition `vortex.sim:zones`), `internal/Zones.cpp`:

```cpp
enum class ZoneKind : uint8_t { Impulse = 1, Lift = 2, Pin = 3, Chill = 4, Root = 5, Release = 6, Block = 7 };   ///< wire values ([mag-2])
struct Zone {
    uint16_t zoneId = 0;
    ZoneKind kind = ZoneKind::Impulse;
    uint8_t teamMask = 0xFF;      ///< bit t: acts on team t
    voxel::ShapeQ shape;          ///< kinds 0, 1, 3 (box unused by zones in this stage)
    uint32_t startTick = 0;
    uint16_t duration = 1;        ///< acts on [startTick, startTick + duration)
    int16_t strength[2] = {0, 0}; ///< cm/s: Impulse {outward or along, up}; Lift {target up, 0}
    uint32_t EndTick() const { return startTick + duration; }
    friend bool operator==(const Zone&, const Zone&) = default;
};
inline constexpr size_t kMaxZoneRecords = 64;   ///< a client's cap; a server allows 32 live or pending (CastContext)
class ZoneSet {
public:
    bool Insert(const Zone& zone);                        ///< false when full or the id is present
    bool Cancel(uint16_t zoneId, uint32_t endTick);       ///< truncation; false when the id is unknown
    const Zone* Find(uint16_t zoneId) const;
    /// Zones live at `tick` whose shape box meets [reachMinQ, reachMaxQ] (q), in (startTick, zoneId) order.
    template <class F> void ForEachLive(uint32_t tick, const glm::ivec3& reachMinQ, const glm::ivec3& reachMaxQ, F&& fn) const;
    void PruneEndedBefore(uint32_t tick);                 ///< removes zones with EndTick() <= tick
    size_t Size() const;
    size_t LiveOrPendingAt(uint32_t tick) const;          ///< EndTick() > tick
    std::span<const Zone> All() const;                    ///< sorted by (startTick, zoneId)
private:
    std::vector<Zone> m_Zones;
};
/// The zone shape's box in q [min, max] (inclusive).
void ZoneBoundsQ(const Zone& zone, glm::ivec3& minQ, glm::ivec3& maxQ);
```

`engine/src/sim/Player.cppm` (add; card 9 fills):

```cpp
enum PlayerStatus : uint8_t { PlayerStatus_Rooted = 1, PlayerStatus_Pinned = 2, PlayerStatus_Chilled = 4,
                              PlayerStatus_Lifted = 8, PlayerStatus_Free = 16, PlayerStatus_Fresh = 32 };
// PlayerState gains (all zero by default, part of operator==):
int16_t push[3] = {0, 0, 0};  ///< cm/s
uint8_t status = 0;           ///< PlayerStatus bits
uint8_t slowQ = 0, slowLeft = 0, rootLeft = 0, pinLeft = 0, ccGuardLeft = 0;   ///< [mag-3] fills them
// PlayerTuning gains:
uint16_t pushDecayGround = 45;   ///< cm/s per tick
uint16_t pushDecayAir = 1;       ///< cm/s per tick
/// What a step reads besides the player: the zones live at the stepped tick (nullptr: none).
struct PlayerZones {
    const ZoneSet* zones = nullptr;
    const voxel::World* world = nullptr;   ///< voxel size for q; the ground id from [mag-4]
    uint32_t tick = 0;                     ///< the tick being stepped (a replay: its HistoryEntry's tick)
    uint8_t teamBit = 1;                   ///< 1u << the player's team
};
PlayerStepResult StepPlayer(physics::PhysicsWorld& physics, const physics::TerrainColliders& terrain, physics::CharacterId character,
                            PlayerState& state, const PlayerInput& input, const PlayerTuning& tuning, const PlayArea& area,
                            const PlayerZones& zones, float dt);
// the existing overload stays and forwards with PlayerZones{}
uint8_t CastCooldownFor(const PlayerState& state, const PlayerTuning& tuning);   ///< castCooldownTicks * (16 + slowQ / 2) / 16
inline constexpr uint32_t kPlayerRuleRevision = 2;                                 ///< card 9 only
```

`engine/src/sim/Spell.cppm` (partition `vortex.sim:spell`), `internal/Spell.cpp`: card 4 gives the full structs.

```cpp
enum class Form : uint8_t { Flow = 0, Plume = 1, Pierce = 2, Burst = 3 };          ///< wire order
enum class Element : uint8_t { Metal = 0, Thermal = 1, Pressure = 2, Frost = 3 };  ///< wire order
using SpellId = uint8_t;
inline constexpr SpellId kSpellCount = 18;   ///< 0..15 the matrix; 16 Kindle, 17 Pour (spread drills, not built here)
constexpr SpellId MakeSpell(Form f, Element e) { return static_cast<SpellId>(static_cast<uint8_t>(f) * 4 + static_cast<uint8_t>(e)); }
constexpr Form FormOf(SpellId s) { return static_cast<Form>(s / 4); }
constexpr Element ElementOf(SpellId s) { return static_cast<Element>(s % 4); }
inline constexpr SpellId kUpdraft = MakeSpell(Form::Plume, Element::Pressure);       // 6
inline constexpr SpellId kSiegeCannon = MakeSpell(Form::Pierce, Element::Pressure);  // 10
inline constexpr SpellId kBarricadeWave = MakeSpell(Form::Burst, Element::Metal);    // 12
inline constexpr SpellId kShockwave = MakeSpell(Form::Burst, Element::Pressure);     // 14
struct FormDef;  struct ZoneDef;  struct SpellDef;  struct SpellTable;   // card 4
SpellTable BuildSpellTable(float voxelSize, const voxel::WorldMaterials& ids);
uint64_t SpellTableHash(const SpellTable& table);
```

`engine/src/sim/Propagate.cppm` (partition `vortex.sim:propagate`), `internal/Propagate.cpp`: card 5 and 6 give it.
`engine/src/sim/Knit.cppm` (partition `vortex.sim:knit`), `internal/Knit.cpp`: `KnitRegistry`, `StructureCredit`
(card 7). `engine/src/sim/Cast.cppm` (partition `vortex.sim:cast`), `internal/Cast.cpp`: `SpellCast`,
`CastContext`, `CheckCapture`, `CastSpell`, `RunScheduled` (card 10). Partition import order (no cycles):
`:zones` <- `:simulation` <- `:worldop` <- `:blast` <- `:spell` <- `:propagate` <- `:knit` <- `:cast`; `:player`
imports `:zones`. Add each `export import` to `Sim.cppm`.

### Tests (files and suites)

- `engine/tests/core/TurningTests.cpp` (VortexCoreTests): `TurningTable.*`, `MapMask.*` (encoding), `ShapeQ.*`.
- `engine/tests/sim/MagicOpsTests.cpp` (VortexSimTests): `CarveCapsule.*`, `MapMask.DecidesFromTheOldIdOnly`,
  `Magic.ClipEqualsWhole`, `Magic.TwoSimulationsFedSpellOpsHaveEqualHashes`.
- `engine/tests/sim/SpellTableTests.cpp`: `SpellTable.*`, `Materials.BoreCostMatchesHardness`.
- `engine/tests/sim/PropagateTests.cpp`: `Propagate.*`, `Pierce.*`.
- `engine/tests/sim/KnitTests.cpp`: `KnitRegistry.*`, `Magic.ABlastOnFreshKnitIsAbsorbed`,
  `Magic.AnAbsorbedBlastAblatesThePlate`, `Magic.ConjuredPiecesNeverTakeASlot`, `StructureCredit.*`.
- `engine/tests/sim/ZoneTests.cpp`: `ZoneSet.*`.
- `engine/tests/sim/PlayerTests.cpp` (existing file): the new `Player.*` tests.
- `engine/tests/sim/MagicTests.cpp`: `Magic.*` spell tests, `CastBudget.SpellWorkIsEqualColdAndWarm`.
- Helpers in test files live in an anonymous namespace and never take an engine type's name (no `Zone`, `Spell`,
  `Chunk`, `Arena` clash: call them `MagicYard`, `TestZone`).

## Card 1: Skeleton: SpellTable, TurningTable, the operators, KnitRegistry, StructureCredit, ZoneSet, CastSpell and WorldOpKinds 7-9, stubbed; tests DISABLED_

| | |
|---|---|
| Goal | Every interface of the stage exists and compiles; every test of the stage is written and disabled, so later cards only fill bodies and rename tests |
| Depends on | `[wp-6]`, `[wp-7]`, `[wp-8]`, `[mech-1]` committed |
| Effort | 1 d (e) |
| Touches | voxel, math, physics, sim, net (switch cases only). Wire: no. Movement rule: no (the revision stays 1 here). buildHash: no |

**Read first** (about 60 KiB)
- This pack: "Decisions", "Skeleton", and the "Tests" list of every card (you write them all now).
- `engine/src/sim/WorldOp.cppm` and `internal/WorldOp.cpp` (whole, 11 KiB).
- `engine/src/voxel/World.cppm` lines of `SphereQ` .. `SphereTouchesBox` and the editing block (`grep -n 'SphereQ\|ClearMask' `).
- `engine/src/net/internal/Wire.cpp`: `WriteOp`, `ReadOp`, `ReadEvent` (the `kind >= 1 && kind <= 6` test).
- `engine/src/sim/Sim.cppm`, `engine/src/voxel/Voxel.cppm`.

**Files**
- new: every file of "Skeleton", with stub bodies.
- change `engine/src/sim/internal/WorldOp.cpp`: add the factories `Map`, `MapMasked`, `Bore` (filled now: they only set
  fields; `MapMasked` sets `axis = voxel::ShortestRunAxis(mask)` - stub returns 0 until card 2); in `Bounds`,
  `ApplyOp` and `OpTouchesChunk` add cases for the three kinds that do nothing yet (`outMin = outMax = 0`, 0 changed,
  `false`) with `// [mag-1] card 2 fills this`.
- change `engine/src/net/internal/Wire.cpp`: `WriteOp` gets `case MapShape: case MapMask: case CarveCapsule:
  break; // [mag-2] card 1 puts these on the wire; a [mag-1] server never emits them`. `ReadOp` returns `false`
  for them. `ReadEvent` keeps accepting kinds 1-6 only. No other net change.
- `PhysicsWorld::UpdateCharacter` gets the `vertical` parameter (declaration and definition), unused until card 9.

**Steps**
1. Write the voxel partition `:turning` with `BuildTurningTable` returning all-identity maps (stub), `WorldTurningTable`
   as a function-local static built from `RegisterDefaultWorldMaterials` on a scratch `MaterialLibrary`, and
   `TurningTableHash` returning 0.
2. Add `ShapeQ`, `InShapeQ`, `ShapeVoxelBounds`, `CarvableIdsFor`, `MapShapeQ`, `MapMaskQ`, `CarveCapsuleQ` stubs.
   `CarvableIdsFor` may be filled now: move the body of the anonymous `CarvableIds` into it and make `CarvableIds`
   call it (no behaviour change).
3. Add the mask run functions (stubs) and `math::ISqrt64` (fill it now: `r = (uint64_t)std::sqrt((double)v)`; then
   `while (r * r > v) --r; while ((r + 1) * (r + 1) <= v) ++r;` with the products in `unsigned __int128` or guarded
   by `r < 4294967296`).
4. Add the sim partitions with every declaration of the Skeleton and of cards 4-10 ("Interfaces" blocks), bodies
   stubbed. `Simulation` gets a `ZoneSet` member in its `Impl` and the two accessors (filled now).
5. Write every test named in cards 2-11 in its file, each as `TEST(Suite, DISABLED_Name)`. A test may reference
   only declared names. Write the setup and the assertions as the cards say, with the expected values they give.
6. Build. Run all tests.

**Tests**
- No new enabled test. Every new test exists, disabled: `tools/ctest.sh -N | grep -c DISABLED_` grows by the number
  of tests this pack names (count them and put the number in the report).

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning (every `switch` over
   `WorldOpKind` handles the new values; `-Wswitch` is on).
2. `tools/ctest.sh` - all pass (nothing changed behaviour).
3. `tools/doc-check.sh --build build` - PASS (update ROADMAP's test count line if the count moved: disabled tests
   are still `TEST` macros).

**Docs**
- `engine/docs/ROADMAP.md` "State": the `Tests:` line (N and M).

**Must not break**
- "World edits and blasts": `WorldOpKind` values 1-6 unchanged; the edit visit order.
- "Sessions": no byte on the wire changes (`Wire.*` unchanged and green).

**Done when**
- [ ] Build clean; all existing tests pass; the new tests exist, all `DISABLED_`.
- [ ] `grep -rn 'mag-1\] card' engine/src` lists only stubs that later cards name.

**Traps**
- A partition that imports another partition of the same module writes `import :zones;`, never `import vortex.sim;`.
- Do not include `<glm/...>` or `<span>` after an `import` line in a test file: includes first, then imports
  (FieldHarness.h explains the clash).
- `PlayerState` grows: every aggregate initialiser of it in tests and code still compiles (new fields have defaults).

## Card 2: The new world ops in ApplyOp: MapShape, MapMask, CarveCapsule; OpChunkRange and OpTouchesChunk by box; determinism tests

| | |
|---|---|
| Goal | Three integer-exact world operations every peer can apply with the same result, clip by clip |
| Depends on | card 1 |
| Effort | 1 d (e) |
| Touches | voxel (World), sim (WorldOp). Wire: no. Movement: no. buildHash: no |

**Read first** (about 45 KiB)
- `engine/src/voxel/internal/World.cpp`: `EditRegion`, `SphereTouchesBox`, `AxisDistance2`, `InSphereQ`,
  `SphereVoxelBounds`, `World::CarveSphereQ`, `World::ClearMask` (`grep -n`, then `sed -n`).
- `engine/src/sim/internal/WorldOp.cpp` (whole). `engine/src/voxel/Volume.cppm` `VoxelMask`.
- magic.md "Apply (`sim::ApplyOp`, ...)" and "Pierce" (the capsule test).
- `engine/tests/sim/WorldOpClipTests.cpp` (how today's clip tests compare whole and clipped calls).

**Files**
- change `engine/src/voxel/internal/World.cpp`: fill `InShapeQ`, `ShapeVoxelBounds`, `MapShapeQ`, `MapMaskQ`,
  `CarveCapsuleQ`.
- change `engine/src/voxel/internal/Volume.cpp` (or the file holding `VoxelMask`): fill the four mask-run functions.
- change `engine/src/sim/internal/WorldOp.cpp`: `Bounds`, `ApplyOp`, `OpTouchesChunk`, `OpWireBytes` for the new kinds.

**Interfaces**: as in "Skeleton" (voxel and `WorldOp`).

**Steps**
1. `InShapeQ(shape, v)`, with `c = 16 * v + 8` per axis in int64:
   - sphere: `InSphereQ({shape.a, shape.radiusQ}, v)`.
   - capsule: `d = c - a`, `u = b - a` (int64 x 3), `r = radiusQ`, `L2 = dot(u, u)`, `t = dot(d, u)`. If `L2 == 0` or
     `t <= 0`: in when `dot(d, d) <= r*r`. Else if `t >= L2`: `e = c - b`, in when `dot(e, e) <= r*r`. Else in when
     `dot(d, d) * L2 - t * t <= r*r * L2`. Before the products: if any `|d_i| > |u_i| + r` (outside the box) return
     false, so every product stays below 2^62 for capsules up to 8 192 q long and radius 1 024 q.
   - box: `a.x <= v.x < b.x` and the same for y, z (voxels, not q).
   - y-cylinder: `dx = c.x - a.x`, `dz = c.z - a.z`, in when `dx*dx + dz*dz <= r*r` and `|c.y - a.y| <= halfHeightQ`.
   - unknown kind: false.
2. `ShapeVoxelBounds`: per axis the q interval `[lo_q, hi_q]` the shape covers (sphere: `a +- r`; capsule:
   `min(a, b) - r .. max(a, b) + r`; cylinder: x, z `a +- r`, y `a +- halfHeightQ`), then voxels
   `lo = CeilDiv64(lo_q - 8, 16)`, `hi = FloorDiv64(hi_q - 8, 16) + 1` (as `SphereVoxelBounds`), clamped to
   `+-kMaxEditVoxel`, `hi >= lo`. Box: `[a, b)` clamped, `hi = max(hi, lo)`.
3. `MapShapeQ`: `ShapeVoxelBounds`, then `EditRegion(min, max, decide, touches, edits, report, clip)` with
   `decide(v, old) = InShapeQ(shape, v) && map[old] != old ? map[old] : -1` and `touches(lo, hi) = true` (box-based,
   Decision 3; EditRegion already restricts to the box).
4. `MapMaskQ`: the same box check as `ClearMask` (origin + size inside `+-kMaxEditVoxel`, empty size -> 0), the same
   `anyInRow` row filter and `touches` as `ClearMask`, `decide(v, old) = mask.Get(v - origin) && map[old] != old ?
   map[old] : -1`. Note the difference from `ClearMask`: air is not skipped (Knit maps air).
5. `CarveCapsuleQ`: `kind != kShapeCapsule` -> 0. `carvable = CarvableIdsFor(m_Materials, power)`;
   `decide(v, old) = old != 0 && carvable[old] && InShapeQ(capsule, v) ? 0 : -1`; `touches = true`.
6. `WorldOp::Bounds`: MapShape and CarveCapsule -> `voxel::ShapeVoxelBounds(shape, ...)`; MapMask -> as ClearMask
   (`min`, `min + mask.Size()`).
7. `ApplyOp`: `map == 0 || map >= voxel::kTurningMapCount` -> return a default `OpResult` (Decision 5). Else
   `world.MapShapeQ(op.shape, voxel::WorldTurningTable().maps[op.map], edits, &result.report, clip)`; MapMask ->
   `world.MapMaskQ(op.min, op.mask, table.maps[op.map], ...)`; CarveCapsule -> `world.CarveCapsuleQ(op.shape,
   op.power, ...)`.
8. `OpTouchesChunk`: MapShape, CarveCapsule -> `true` once the box test at the top passed; MapMask -> the
   ClearMask row loop.
9. Mask runs (Decision 4). `MaskRuns(mask, axis)`: iterate positions in axis order (axis 0: `for z, for y, for x`;
   axis 1: `for z, for x, for y`; axis 2: `for y, for x, for z` - the run axis innermost, the other two with x before
   y before z from inner to outer), count alternating runs starting with clear. `MaskFromRuns`: walk the same order,
   set bits of set runs with `VoxelMask::Set`; the sum of runs must equal the voxel count exactly. `MaskRunBytes`:
   sum of LEB128 lengths (`1 + floor(log128(n))`, 0 -> 1 byte). `ShortestRunAxis`: the smallest `MaskRunBytes`, ties
   to the lower axis.
10. `OpWireBytes`: CarveSphere 17, FillSphere 16, SwapSphere 17, FillBox 26, Detach `29 + MaskRunBytes(mask, 0)`,
    ClearMask `19 + MaskRunBytes(mask, 0)`, MapShape `{17, 29, 27, 19}[shape.kind]`, MapMask `21 +
    MaskRunBytes(mask, axis)`, CarveCapsule 29. (Today's sizes are in the comment of `net::WriteEvent`; check them.)

**Tests**
- `ShapeQ.CapsuleOfZeroLengthIsASphere` (core, TurningTests.cpp): for 200 voxels around a point, `InShapeQ(capsule
  a == b, r)` equals `InSphereQ({a, r})`.
- `CarveCapsule.MatchesAChainOfSpheres` (sim, MagicOpsTests.cpp): (a) `b - a = (160, 0, 0)`, r 51: the capsule's
  voxels equal the union of spheres of radius 51 centred at `a + (k, 0, 0)` for k = 0..160 (every q step: a voxel's
  nearest axis point has an integer x), in a 40^3 box; both non-empty. (b) `b - a = (96, 128, 0)`, r 51: the union of
  spheres at `a + k * (3, 4, 0)` for k = 0..32 is a subset of the capsule's voxels, and the capsule has at most 2 %
  more voxels (the gaps between samples).
- `CarveCapsule.FarCoordinatesStayExact`: the same capsule (a) shifted by `1 << 26` voxels (`1 << 30` q) in x and z
  contains the same voxels, shifted.
- `MapMask.AxisOrderGivesTheSameVoxels` (core): a random mask 37 x 13 x 21 (seeded `std::mt19937(7)` in the test
  only); for each axis, `MaskFromRuns(MaskRuns(m, axis))` equals `m`; `ShortestRunAxis` of a single y-column mask is 1
  and of a single x-row mask is 0.
- `MapMask.DecidesFromTheOldIdOnly` (sim): a world box of stone, air and knit; `MapMasked(Map_Unknit, ...)` turns
  exactly the masked knit voxels to air and leaves masked stone and air unchanged; the edit list holds only those.
- `Magic.ClipEqualsWhole` (sim): for one op of each new kind spanning 2 x 2 x 2 chunks, applying it whole equals
  applying it with `ChunkClip::One(c)` for every chunk of `OpChunkRange` in (y, z, x) order and also in reverse
  order: same `WorldHash`, same summed `changed`.
- `Magic.TwoSimulationsFedSpellOpsHaveEqualHashes` (sim): two simulations with 3 and 1 job threads (copy `Arena`
  from BlastTests.cpp as `MagicYard`), feed both a stream of 6 ops (a MapShape Knit box, a MapMask Unknit of part of
  it, a CarveCapsule through stone and knit, a MapShape Ablate sphere, a ClearMask, a CarveSphere); equal
  `WorldHash`, every `OpResult::Complete()`.

**Checks**
1. Build, no warning. 2. `tools/ctest.sh -R 'ShapeQ\.|CarveCapsule\.|MapMask\.|Magic\.' --output-on-failure`.
3. `tools/ctest.sh`. 4. `tools/doc-check.sh --build build`.

**Docs**
- `engine/docs/notes/voxel.md`: one paragraph under the edit functions: the three new edits, their decide rules, the
  box-based touches of MapShape and CarveCapsule.

**Must not break**
- "World edits and blasts": integer-exact edits, visit order, `EditReport::outOfBounds` refuses an op whole (a Knit
  map over air above the ceiling is refused whole by `EditRegion`'s ceiling test, unchanged).
- `ViewDistance.PackedAndUnpackedWorldsTakeTheSameEdits` (packed chunks are read through `EditRegion`'s decode).

**Done when**
- [ ] The seven tests above pass (renamed from `DISABLED_`); all tests pass.

**Traps**
- `EditRegion` skips `newId == oldId`: still return `-1` for identity entries, it is cheaper.
- `InShapeQ` must use int64 everywhere; `glm::ivec3` arithmetic is int32 and overflows for far voxels.
- The C++ `/` truncates toward zero: use `FloorDiv64` / `CeilDiv64` for voxel bounds of negative coordinates.

## Card 3: knit_iron (id 21) and the Conjured flag through MaterialTableHash; TurningTable with maps 3, 14, 15

| | |
|---|---|
| Goal | Knit iron exists as a terrain material, and the three maps this stage uses turn the right ids |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | voxel. Wire: no. `MaterialTableHash` changes (so `buildHash` changes: sessions of this build refuse older builds, as intended). Movement: no |

**Read first** (about 15 KiB)
- `engine/src/voxel/internal/Material.cpp` `RegisterDefaultWorldMaterials` (whole function).
- magic.md "New world-palette ids" (the `knit_iron` row) and "The Turning table".
- The editor's palette code: `grep -rn 'Indestructible\|WorldMaterials' engine/src/editor` (conjured ids are hidden).

**Files**
- change `engine/src/voxel/Material.cppm`, `internal/Material.cpp`: the flag, `knitIron`.
- change `engine/src/voxel/internal/Turning.cpp`: fill `BuildTurningTable`, `TurningTableHash`.
- change the editor palette (where it lists world materials): skip ids with `MaterialFlag_Conjured`.

**Steps**
1. After `bedrock` in `RegisterDefaultWorldMaterials`: `ids.knitIron = add("knit_iron", Srgb(92, 84, 112), 0.55f,
   2000.0f, 7.0f, 0.10f, MaterialFlag_Conjured, 0.0f, 0.6f);` and `strength(ids.knitIron, 100000, 40000, 15000);`.
   Check `ids.knitIron == 21` with a `Verify` / assert in the test, not in code.
2. `BuildTurningTable(ids)`: every map starts as the identity (`m[i] = i`). Map 3 Knit: `m[0] = knitIron`. Map 14
   Unknit: `m[knitIron] = 0` (slag, ice, floe, rimed and hot knit join with their stages). Map 15 Ablate:
   `m[knitIron] = 0`. Maps 1, 2 and 4-13 stay the identity (their stages fill them). Never name `bedrock`.
3. `TurningTableHash`: `h = HashMix64(0x5455524eull /* "TURN" */, kTurningMapCount)`; for map 0..15, for id 0..255:
   `h = HashMix64(h, m[id])`; return `h`.
4. Editor: hide `Conjured` ids from the material palette (one `continue`).

**Tests**
- `TurningTable.NeverNameBedrockAndIdentityElsewhere` (core): for every map, `m[bedrock] == bedrock`; for maps other
  than 3, 14, 15 every entry is the identity; map 3 differs from the identity only at 0; maps 14 and 15 only at 21.
- `TurningTable.HashCoversEveryMap` (core): changing any one entry of any map (try map 0 entry 255, map 3 entry 0,
  map 15 entry 200) changes `TurningTableHash`.
- `Materials.KnitIronIsConjuredAndStoneStrong` (core): id 21 is named `knit_iron`, hardness 7.0, density 2000,
  flags exactly `Conjured`, strength `{100000, 40000, 15000}`.
- Existing `SessionFile.BuildHashSeesTheMaterialTable` stays green.

**Checks**
1. Build. 2. `tools/ctest.sh -R 'TurningTable\.|Materials\.|SessionFile\.' --output-on-failure`. 3. `tools/ctest.sh`
   (no golden changes: generators never write id 21). 4. doc-check.

**Docs**
- `engine/docs/notes/voxel.md`: id 21 and the flag in the palette list.

**Must not break**
- `Terrain.GeneratorGoldenHashNamesTheGoldenTables` (the generator goldens do not include the material table).
- `GraphNode` stays 72 B: the new id is only a `GraphMaterials` entry.

**Done when**
- [ ] Three new tests pass; all tests pass.

**Traps**
- Do not regenerate goldens. If a golden test fails, the id was added somewhere a generator reads: stop and report.

## Card 4: sim::SpellTable: forms, elements, SpellId, FormDef and SpellDef for the four spells, metres converted once, SpellTableHash

| | |
|---|---|
| Goal | One integer table holds every number of the four spells at the session's voxel size |
| Depends on | card 3 |
| Effort | 0.5 d (e) |
| Touches | sim. Wire: no. buildHash: no (it joins in `[mag-2]`) |

**Read first** (about 25 KiB)
- magic.md "`sim::SpellTable`", "Operator parameters", "Resources" (cooldowns), the entries of Barricade Wave,
  Updraft, Siege Cannon and Shockwave in "The sixteen spells".

**Files**
- change `engine/src/sim/Spell.cppm`, fill `internal/Spell.cpp`.

**Interfaces**

```cpp
struct FormDef { uint16_t cooldownTicks = 0; };
/// How a spell's zone is placed and what it does (all integers; q = 1/16 voxel).
struct ZoneDef {
    uint8_t kind = 0;          ///< ZoneKind value; 0: no zone
    uint8_t place = 0;         ///< 0 none, 1 Updraft column, 2 Siege follow-through capsule, 3 Shockwave radial
    uint8_t target = 0;        ///< 0 enemies, 1 everyone, 2 allies
    uint16_t durationTicks = 0;
    int16_t strength[2] = {0, 0};   ///< cm/s
    uint16_t radiusQ = 0;      ///< zone radius
    uint16_t halfHeightQ = 0;  ///< Shockwave cylinder
    uint16_t lengthQ = 0;      ///< Siege follow-through length
    uint16_t rampQ = 0;        ///< Lift: rim ramp width
    uint16_t hoverQ = 0;       ///< Lift: ease below the top
};
struct SpellDef {
    bool built = false;
    // Pierce
    uint16_t pierceRadiusQ = 0; uint16_t piercePower = 0; uint32_t pierceBudget = 0; uint16_t pierceMaxSlices = 0;
    // Burst (strip)
    uint16_t burstHalfWidth = 0; uint16_t burstHalfThickness = 0; uint16_t burstPops = 0; uint16_t burstVisitCap = 0;
    uint8_t stepMax = 0; uint16_t burstPlane = 0; int16_t burstNormalMin = 0; int16_t burstNormalMax = 0;
    // Plume (single column)
    uint16_t riseMax = 0;
    // verbs
    uint8_t fillMap = 0; uint8_t rimHeight = 0;
    uint8_t bandCount = 0; uint8_t bandFirstTick = 0; uint8_t bandSpacingTicks = 0;
    uint16_t lifetimeTicks = 0; uint8_t expiryMap = 0;
    ZoneDef zone;
    // server only
    uint16_t directDamage = 0; uint16_t zoneDamage = 0; uint8_t maxBodies = 0; uint16_t maxBodyKickCmS = 0;
    uint16_t dispelRadiusQ = 0;
};
inline constexpr uint16_t kBoreStop = 0xFFFF;
struct SpellTable {
    float voxelSize = 0.125f;
    std::array<FormDef, 4> forms{};
    std::array<SpellDef, kSpellCount> spells{};
    std::array<uint16_t, 256> boreCost{};   ///< 1/16 hardness per voxel; 0 air and liquids; kBoreStop indestructible
    uint16_t ablateRadiusQ = 0;             ///< the absorb rule's patch (0.25 m)
    uint32_t zoneLeadTicks = 24;            ///< [mag-2]: a session setting
};
```

**Steps**
1. Conversions, once, in double from the float `voxelSize`: `V(m) = std::lround(m / vs)` (voxels), `Q(m) =
   std::lround(m / vs * 16.0)` (q). Halves round away from zero.
2. Forms: Flow 540, Plume 540, Pierce 300, Burst 420 ticks.
3. `boreCost` by id (Decision 6): 0 air; grass 10, grass_dry 10, dirt 13, stone 48, stone_dark 64, sand 6, snow 3,
   water 0, wood 24, leaves 3, leaves_autumn 3, planks 19, brick 40, iron 128, gold 40, crystal 48, lava 0, torch 5,
   glass 8, bedrock `kBoreStop`, knit_iron 112; every other id 0. Use `ids.<name>`, never a literal index.
4. Siege Cannon (10): `built = true; pierceRadiusQ = min(64, Q(0.4))` (51 at 0.125 m); `piercePower = 128`;
   `pierceBudget = lround(24576 * pow(0.125 / vs, 3))` (24 576; 48 000 at 0.1; 14 222 at 0.15); `pierceMaxSlices =
   64`; zone `{kind Impulse, place 2, target 0, durationTicks 1, strength {900, 200}, radiusQ Q(1.0), lengthQ
   Q(3.0)}`; `directDamage 30`; `maxBodies 8`; `maxBodyKickCmS 1200`.
5. Barricade Wave (12): `burstHalfWidth = V(2.25)` (18); `burstHalfThickness = max(1, V(0.125))` (1); `stepMax =
   V(0.375)` (3); `burstPops = 4000`; `burstVisitCap = 6000`; `burstPlane = V(4.25)` (34); `burstNormalMin =
   -V(0.5)` (-4); `burstNormalMax = V(1.75)` (14); `fillMap = Map_Knit`; `rimHeight = V(1.5)` (12); `bandCount 4,
   bandFirstTick 18, bandSpacingTicks 4`; `lifetimeTicks 1200`; `expiryMap = Map_Unknit`.
6. Updraft (6): `riseMax = V(10.0)` (80); zone `{Lift, place 1, target 1, durationTicks 90, strength {900, 0},
   radiusQ Q(1.0) (128), rampQ Q(0.25) (32), hoverQ Q(0.5) (64)}`; `maxBodies 8, maxBodyKickCmS 1200`.
7. Shockwave (14): zone `{Impulse, place 3, target 0, durationTicks 1, strength {900, 300}, radiusQ Q(4.0) (512),
   halfHeightQ Q(1.5) (192)}`; `zoneDamage 15`; `dispelRadiusQ Q(5.0)` (640); `maxBodies 8, maxBodyKickCmS 1200`.
8. `ablateRadiusQ = Q(0.25)` (32). Every other spell stays `built = false`.
9. `SpellTableHash`: start `HashMix64(0x5350454cull /* "SPEL" */, kSpellCount)`; mix `bit_cast<uint32_t>(voxelSize)`,
   `zoneLeadTicks`, `ablateRadiusQ`, each form's cooldown, each `boreCost`, then every field of every `SpellDef` in
   declaration order (bools as 0/1, signed values cast to `uint64_t` through `uint16_t`). Write one helper
   `MixSpell(h, const SpellDef&)` that lists every field; a new field added later must be added there.

**Tests**
- `SpellTable.IdsAreFormTimesFourPlusElement` (sim): `kSiegeCannon == 10`, `kBarricadeWave == 12`, `kShockwave ==
  14`, `kUpdraft == 6`; `FormOf`/`ElementOf` invert `MakeSpell` for all 16.
- `SpellTable.SizesHoldWithinOneVoxelAcrossVoxelSizes`: at 0.1, 0.125, 0.15: `pierceRadiusQ * vs / 16` within one
  voxel of 0.4 m; Barricade's `rimHeight * vs` within one voxel of 1.5 m; Updraft `riseMax * vs` within one voxel of
  10 m; at 0.125 the exact values of steps 4-8.
- `SpellTable.HashCoversEveryField`: for a copy of the table, changing each field of Siege Cannon's `SpellDef` and
  its `ZoneDef` (one at a time, list them all) changes the hash; so does `boreCost[4]` and `forms[2]`.
- `Materials.BoreCostMatchesHardness`: for every id 1..21 that is neither liquid nor indestructible,
  `boreCost[id] == voxel::QuantizePower(hardness)`; liquids 0; bedrock `kBoreStop`.

**Checks**
1. Build. 2. `tools/ctest.sh -R 'SpellTable\.|Materials\.'`. 3. `tools/ctest.sh`. 4. doc-check.

**Must not break**
- Rule 6 of the guide: no float decides a voxel. The table is built from the session's float voxel size once; both
  sides of a session hold the same bits.

**Done when**
- [ ] Four tests pass.

**Traps**
- `std::lround(2.5) == 3` (away from zero): that is the rule; do not use `std::round` on floats.

## Card 5: The Pierce operator (bore boxes, kMaxBoreVoxels, a budget paying per voxel) emitting one CarveCapsule: Siege Cannon's bore

| | |
|---|---|
| Goal | A bore that goes through 2 m of stone, 0.86 m of knit, stops at bedrock, and fells a tower's neck from range |
| Depends on | cards 2, 4 |
| Effort | 1 d (e) |
| Touches | sim (Propagate, Blast). Wire: no. Movement: no. buildHash: no |

**Read first** (about 55 KiB)
- magic.md "Inputs every operator shares" (the hit, the aim, the flood box, "A bore's boxes"), "Rules shared by every
  operator" (walls, order, work), "Pierce (Solid)".
- `engine/src/sim/internal/Blast.cpp`: `IslandBox`, `FloodBox`, `VoxelSearch`, `GraphSearch` (their first 15 lines),
  `SearchIslands` (signature), `Blast(Simulation&, const SphereQ&, uint16_t)` (whole), `RetryIslandSearches` (whole),
  `Defer`, `DistanceToWindow`, `RequestFor`, `SetCenter`.
- `engine/src/sim/Simulation.cppm` `DeferredIslandSearch`.

**Files**
- fill `engine/src/sim/Propagate.cppm` / `internal/Propagate.cpp` (cache, walls, Pierce).
- change `engine/src/sim/Blast.cppm`, `internal/Blast.cpp`: `BlastSite`, `SphereSite`, `CapsuleSite`, `ChunkSite`,
  `Blast(site, options)` (options read in card 7; here `noCarve` is still ignored), the sphere overload forwards.
- change `engine/src/sim/Simulation.cppm`: `DeferredIslandSearch::site`.

**Interfaces**

```cpp
/// The operator's world: walls (Decision: magic.md "Walls") and a per-cast chunk cache.
struct OperatorWalls { glm::ivec3 min{0}, max{0}; };   ///< voxel box [min, max); outside reads as support
/// The offline walls: the world window's voxel box, rows below the build ceiling. A session's in [mag-2].
OperatorWalls WindowWalls(const voxel::World& world);
class VoxelCache {
public:
    VoxelCache(const voxel::World& world, const OperatorWalls& walls);
    uint8_t Get(const glm::ivec3& v);           ///< the id; a wall voxel or an unloaded chunk reads kWallId
    bool IsWall(const glm::ivec3& v);
    uint32_t ChunksRead() const;                ///< distinct chunks Get touched (loaded or not)
    static constexpr uint8_t kWallId = 0xFF;    ///< never a real id here (ids end at 21)
private:
    const voxel::World& m_World; OperatorWalls m_Walls;
    std::map<std::tuple<int, int, int>, std::unique_ptr<voxel::Chunk>> m_Chunks;   ///< nullptr: not loaded
};
inline constexpr int32_t kMaxBoreVoxels = 64;
inline constexpr int32_t kMaxPierceRadiusVoxels = 4;
inline constexpr int32_t kBoreProofMargin = 24;
static_assert(kMaxBoreVoxels / 2 + 3 * kMaxPierceRadiusVoxels + kIslandSearchMargin + kBoreProofMargin <= voxel::kFloodHalfExtent);
inline constexpr uint32_t kPierceVisitCap = 3000;
inline constexpr uint64_t kPierceSliceWork = 100;
inline constexpr uint64_t kPropagateVisitWork = 1;
struct PierceParams {
    glm::ivec3 hitQ{0};            ///< the ray's hit point, q
    glm::ivec3 aim{0};             ///< D = round(direction * 16384), each in [-16384, 16384]
    uint16_t radiusQ = 0; uint16_t power = 0; uint32_t budget = 0; uint16_t maxSlices = 0;
    const std::array<uint16_t, 256>* boreCost = nullptr;
    const voxel::MaterialLibrary* materials = nullptr;
    std::vector<glm::ivec3>* paidOut = nullptr;   ///< tests: the paid solid voxels in walk order
};
struct PierceResult {
    bool bored = false;            ///< false: slice 0 already stopped (nothing to carve)
    voxel::ShapeQ capsule;         ///< kind capsule: a = A, b = B_last, radiusQ
    glm::ivec3 step{0};            ///< S, q per slice (about 16 long)
    uint32_t slices = 0;           ///< k of B_last
    uint32_t visits = 0;           ///< sum |N_k|, slice 0 included
    uint32_t paidVoxels = 0;       ///< solid voxels paid (Path)
    uint32_t frontVoxels = 0;      ///< |N_0| (the stub)
    uint32_t budgetLeft = 0;
    uint32_t chunksRead = 0;
    uint8_t stopReason = 0;        ///< 0 slices, 1 too hard / indestructible, 2 budget, 3 length, 4 wall, 5 visit cap
};
PierceResult Pierce(const voxel::World& world, const OperatorWalls& walls, const PierceParams& params);
glm::ivec3 AimOf(const glm::vec3& direction);   ///< round(direction * 16384) per axis (llround, clamped)
```

**Steps**
1. `VoxelCache::Get(v)`: if `v` outside `walls` -> `kWallId`. Chunk `c = FloorDiv(v, 32)`; first time: if
   `world.IsChunkLoaded(c)`, `ReadChunk` into a new `Chunk`, else store nullptr; count it in `ChunksRead`. nullptr ->
   `kWallId`. Else `chunk->Get(local)`. `WindowWalls`: `min = WindowMinChunk() * 32`, `max = (WindowMinChunk() +
   WindowSize()) * 32` (a full-height window's top row ends at the ceiling already).
2. `Pierce` (all int64; `r = radiusQ`, at most `16 * kMaxPierceRadiusVoxels`):
   1. `len = ISqrt64(dot(aim, aim))`; if 0 -> `bored = false`. `S_i = round(16 * aim_i / len)` with rounding half away
      from zero in integers: `n = 16 * aim_i`; `S_i = (n >= 0 ? (2n + len) : (2n - len)) / (2 * len)`.
   2. `A = hitQ - (2 * r * S) / 16` (per axis, C++ `/` truncation toward zero; Decision of the design: "integer
      division"). `B_0 = A`.
   3. Slice 0: `N_0` = voxels of `Capsule(A, A, r)` (a sphere) in (z, y, x) order over its voxel bounds. Then slices
      `k = 1, 2, ...`: `B_k = A + k * S`; `N_k` = voxels of `Capsule(B_{k-1}, B_k, r)`'s bounds, in (z, y, x) order,
      that are in `Capsule(A, B_k, r)` and not in `Capsule(A, B_{k-1}, r)` (use `voxel::InShapeQ`).
   4. For each slice `k >= 0`, before paying: stop **before** `k` (keep `B_{k-1}`; for `k = 0` nothing is bored) when
      `k > maxSlices` (reason 0); `k >= 1` and `dot(B_k - hitQ, B_k - hitQ) > (16 * kMaxBoreVoxels)^2` (reason 3);
      `visits + |N_k| > kPierceVisitCap` (reason 5); any voxel of `N_k` reads `kWallId` (reason 4); any voxel is
      indestructible and not liquid, or solid and `!carvable[id]` for `power` (`CarvableIdsFor`) (reason 1);
      `cost(N_k) = sum boreCost[id]` over non-air voxels (liquids cost 0) is above the budget left (reason 2).
   5. Else pay: `budget -= cost`, `visits += |N_k|`, `paidVoxels += solid voxels of N_k that are carvable`, `frontVoxels
      = |N_0|` for k = 0, `slices = k`, continue.
   6. Result: `capsule = {kShapeCapsule, A, B_slices, r}`, `bored = true` once slice 0 passed. `chunksRead` from the
      cache.
3. `CapsuleSite(world, capsule, power, hitQ)`: `carve = WorldOp::Bore(capsule, power)`; island box = the capsule's
   `ShapeVoxelBounds` grown by `kIslandSearchMargin`; flood box centre `f = FloorDiv(hitQ + capsule.b, 32)` (per
   axis, int64 then int32), `floodMin = f - kFloodHalfExtent`, `floodMax = f + kFloodHalfExtent`; `center` = metres of
   `(a + b) / 2` (q * vs / 16), `radius = 0` (no object damage, no push: the follow-through zone does that).
4. `SphereSite(world, sphere, power)`: `carve = WorldOp::Carve(sphere, power)`, boxes from `IslandBox` / `FloodBox`,
   `center`/`radius` as `SetCenter` computes them. `ChunkSite(world, chunk)`: no carve (a default `WorldOp` with
   `power 0`), island box `[32c - 24, 32c + 56)`, flood box `32c + 16 +- 96`, `center` = metres of the chunk centre,
   `radius 0`.
5. Refactor `Blast.cpp`: `VoxelSearch`, `GraphSearch`, `SearchIslands`, `Defer`, `DistanceToWindow`, `RequestFor`
   take the island and flood boxes (from the site) instead of the sphere; `SetCenter` takes the site. `Blast(sim,
   site, options)` is today's sphere `Blast` body with `site.carve` applied through `ApplyOp` in step 1, the boxes
   from the site, `center`/`radius` from the site (objects and push run only when `radius > 0`, as today).
   `Blast(sim, sphere, power)` becomes `return Blast(sim, SphereSite(sim.World(), sphere, power));`.
6. `DeferredIslandSearch`: replace `sphere`, `power` with `BlastSite site`. In `RetryIslandSearches`, the carve
   replay becomes `ApplyOp(world, search.site.carve, &carve.edits, &report, &clip)` with `clip =
   ChunkClip::One(chunk)` (equal to `CarveSphereQInChunk` for a sphere: same decisions, same order). Accumulate
   `changed` and the report as today.

**Tests** (PropagateTests.cpp, sim; build worlds with `FillBox` on a flat arena at 0.125 m unless said)
- `Pierce.WalkAndCapsuleAgree`: a wall of mixed stone, brick, planks and one iron voxel row; for 8 aims (axis and
  diagonal), apply `Bore(result.capsule, power)` and compare the removed voxels with the walk's paid voxels: equal
  sets (record paid voxels in a debug vector the test can read: add `std::vector<glm::ivec3>* paidOut` to
  `PierceParams`, default nullptr).
- `Propagate.PierceStopsAtBedrockAndWhenTheBudgetIsSpent`: Siege Cannon's numbers, horizontal aim into a 6 m thick
  stone block: bored depth (from the face to the deepest carved voxel along the aim) between 14 and 18 voxels
  (about 2.0 m); into iron: 5-7 voxels (0.75 m); into knit: 6-8 voxels (0.86 m); into a bedrock face: nothing
  carved, `stopReason == 1`. Print the measured depths (`std::printf`) for the report.
- `Pierce.BoreStaysInsideItsFloodBox`: at 0.1, 0.125 and 0.15 m, a 64-voxel bore through air ending at the play
  bound: the capsule's island box lies inside the flood box `CapsuleSite` gives, and the flood box lies inside the
  server window (play bounds + 3 chunks).
- `Propagate.WorkIsEqualColdAndWarm`: the same Pierce twice on one world (cold cache, then after an unrelated search
  warmed the graph cache): equal `visits`, `slices`, `chunksRead`.
- `Blast.SiteOverloadEqualsTheSphereBlast`: on two equal arenas, `Blast(sim, sphere, power)` and `Blast(sim,
  SphereSite(world, sphere, power))` give equal op lists and equal hashes (`StemCut` of BlastTests.cpp style).

**Checks**
1. Build. 2. `tools/ctest.sh -R 'Pierce\.|Propagate\.|Blast\.' --output-on-failure` (every existing `Blast.*` and
   `Neck.*` must still pass: the refactor is behaviour-free). 3. `tools/ctest.sh`. 4. doc-check.
5. `VERIFY_SLOT=1 tools/verify.sh quick` (G3-G10 unchanged: the refactor touched every blast).

**Docs**
- `engine/docs/notes/scene-engine.md` or the notes file that describes `sim::Blast` (grep `RetryIslandSearches` in
  `engine/docs/notes`): `BlastSite` and the capsule site in one paragraph.

**Must not break**
- `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes`, `CastBudget.CastWorkIsEqualColdAndWarm`,
  `Structure.GraphNeverDetachesWhatTheVoxelSearchKeeps`, every `Neck.*`; G7-G10 lines.
- "World edits and blasts": `sim::Blast` stays GPU-free and picture-free.

**Done when**
- [ ] Five tests pass; `verify.sh quick` PASS.

**Traps**
- `B_k` and `S` are in q; `kMaxBoreVoxels` is in voxels: compare `16 * 64` q.
- `N_k`'s scan box must be the new cap's box (`Capsule(B_{k-1}, B_k, r)`), not the whole capsule's: the whole box
  is 64x larger and the walk becomes slow.
- Do not decide anything from `chunksRead` other than the work count.

## Card 6: The Burst strip and seed plane, Plume's single-seed column, Barricade Wave's knit fill through MapMask, the ablative absorb rule

| | |
|---|---|
| Goal | A strip of open cells that follows the ground across the aim, extruded into a 1.5 m wall in four masks; a column up to the ceiling; blasts that hit fresh knit chip it instead of carving |
| Depends on | cards 2, 4, 5 (cache, walls) |
| Effort | 1 d (e) |
| Touches | sim. Wire: no. Movement: no |

**Read first** (about 35 KiB)
- magic.md "Rules shared by every operator" (cell classes, seeds, order), "Burst (Torrent)", "Plume (Flame)" first
  bullet, Barricade Wave's entry, "The absorb rule, ablative".
- This pack's card 5 interfaces (`VoxelCache`, `OperatorWalls`).

**Files**
- `engine/src/sim/Propagate.cppm` / `internal/Propagate.cpp`: `FaceAxes`, `BurstStrip`, `PlumeColumn`,
  `BarricadeFill`.
- `engine/src/sim/internal/Blast.cpp`: `AbsorbsBlast`, `BlastAt` (the absorb), `BlastResult::absorbed`.

**Interfaces**

```cpp
/// The hit face: n = normal (one axis, sign), u, v = the other two axes in x, y, z order (unit vectors).
struct FaceAxes { glm::ivec3 n{0, 1, 0}, u{1, 0, 0}, v{0, 0, 1}; };
FaceAxes FaceAxesOf(const glm::ivec3& normal);
bool IsOpenId(uint8_t id);                                        ///< air (films from [mag-5]); kWallId is not open
struct BurstParams { glm::ivec3 seed{0}; FaceAxes axes; glm::ivec3 aim{0}; uint16_t halfWidth = 0, halfThickness = 0;
                     uint8_t stepMax = 0; uint16_t pops = 0, visitCap = 0, plane = 0; int16_t normalMin = 0, normalMax = 0; };
struct BurstResult {
    std::vector<glm::ivec3> cells;     ///< the strip, in pop order (all Front)
    std::vector<int32_t> lateral;      ///< per cell: |du*Dv - dv*Du| / |Dp| (0..halfWidth)
    uint32_t visits = 0, chunksRead = 0;
};
BurstResult BurstStrip(const voxel::World& world, const OperatorWalls& walls, const BurstParams& params);
struct PlumeColumn { glm::ivec3 bottom{0}, top{0}; uint32_t visits = 0, chunksRead = 0; };   ///< inclusive cells
PlumeColumn PlumeSingleColumn(const voxel::World& world, const OperatorWalls& walls, const glm::ivec3& seed, uint16_t riseMax);
struct FillBands { std::array<std::vector<glm::ivec3>, 6> bands; uint32_t cells = 0; };
/// Barricade Wave's fill: strip cells plus their extrusion along +n (rimHeight - 1 more while open), banded by lateral.
FillBands BarricadeFill(const voxel::World& world, const OperatorWalls& walls, const BurstResult& strip, const FaceAxes& axes,
                        uint8_t rimHeight, uint8_t bandCount, uint16_t halfWidth);
```

**Steps**
1. `FaceAxesOf(normal)`: `n = normal` (exactly one non-zero component, +-1); `u`, `v` = unit vectors of the two other
   axes, lower axis first (n on y: u = x, v = z; n on x: u = y, v = z; n on z: u = x, v = y).
2. A **Burst cell** `c`: `IsOpenId(cache.Get(c))` and `!IsOpenId(cache.Get(c - n))` (a wall or a solid or a liquid
   counts as support: `kWallId` is not open).
3. `BurstStrip` (the sweep; offsets `du = dot(c - seed, u)`, `dv = dot(c - seed, v)`, `dn = dot(c - seed, n)`):
   1. `Du = dot(aim, u)`, `Dv = dot(aim, v)`; `Dp = ISqrt64(Du*Du + Dv*Dv)`; if `Dp == 0`: `Du = 1, Dv = 0, Dp = 1`.
   2. In the footprint when `|du*Dv - dv*Du| <= halfWidth * Dp` and `|du*Du + dv*Dv| <= halfThickness * Dp` (int64).
   3. In the box when `|du| <= plane`, `|dv| <= plane`, `normalMin <= dn <= normalMax`; outside the box reads as a
      wall. `visited` is a dense bitset over the box, indexed `((dn - normalMin) * (2p + 1) + (dv + p)) * (2p + 1) +
      (du + p)` with `p = plane`; it is never iterated.
   4. If `seed` is not a Burst cell: empty result. Else push `seed` into a min-heap keyed by the tuple `(du*du +
      dv*dv, du, dv, dn, z, y, x)` (all int64; use `std::priority_queue` with `std::greater` on a `std::tuple`), mark
      visited.
   5. Pop while the heap is non-empty, `pops` not reached and `visits < visitCap`: append the cell and its lateral
      (`|du*Dv - dv*Du| / Dp`, integer division). For `e` in `+u, -u, +v, -v` (that order), `p = c + e`; for `k` in `0,
      +1, -1, +2, -2, ..., +stepMax, -stepMax`: `q = p + k * n`; `++visits`; take the first `q` that is a Burst cell
      with a clear way (k > 0: `c + j*n` open for `1 <= j <= k`; k < 0: `p + j*n` open for `k < j <= 0`); if found, in
      the box, in the footprint and not visited: mark, push.
4. `PlumeSingleColumn(seed)`: if `seed` is not open: `bottom = top = seed`, nothing. Else `top = seed`; while `top.y
   - seed.y < riseMax` and the cell above `top` is open (not a wall): `++top.y`, `++visits`. `bottom = seed`.
5. `BarricadeFill`: for each strip cell in pop order: band `b = min(bandCount - 1, lateral * bandCount /
   (halfWidth + 1))`; add the cell, then `c + j*n` for `j = 1 .. rimHeight - 1` while open; each added cell goes into
   `bands[b]`. A cell added twice (two strip cells extruding into the same cell cannot happen on one normal, but
   check with a set) is added once.
6. `AbsorbsBlast(materials, id, power)`: `id != 0 && (flags & Conjured) && hardness * 16 > power` (the float compare
   of `CarvableIdsFor`, exact).
7. `BlastAt(sim, sphere, power, hitMaterial)`: if `!AbsorbsBlast(...)` return `Blast(sim, sphere, power)`. Else: op
   `Map(Map_Ablate, ShapeQ{kShapeSphere, sphere.center, {}, radius})` with `radius = lround(0.25 / vs * 16)` from
   the world's voxel size (the same number as `SpellTable::ablateRadiusQ`; `BlastAt` takes no table), applied with
   `ApplyOp`, as `ops[0]`
   of the result; then `Blast(sim, SphereSite(world, sphere, power), {.noCarve = true})` (card 7 makes `noCarve`
   work; until then this card's test applies only the ablation), appending its ops after it and copying its objects,
   fragments, events and stats; `absorbed = true`.

**Tests**
- `Propagate.BurstCellsRestOnTheirSurface` (PropagateTests.cpp): on stairs (0.375 m steps) and a slope, every strip
  cell is open with support at `c - n`.
- `Propagate.BurstNeverStepsMoreThanStepMax`: with a 0.5 m (4 voxel) ledge across the strip, no cell lies past the
  ledge on the low side; with a 3-voxel step it continues.
- `Propagate.BarricadeIsAWallAcrossTheAim`: flat floor, aim along +x tilted down: the fill's cells span 37 voxels
  in z (2 * 18 + 1), 3 in x, 12 in y; about 1 300 cells (between 1 250 and 1 340); 4 bands, band 0 holds the middle.
- `Propagate.PlumeColumnStopsUnderACeiling`: a 3 m ceiling over the seed: `top.y - bottom.y == 23` (24 cells of
  0.125 m); open sky: `riseMax` cells.
- `Magic.ABlastOnFreshKnitIsAbsorbed` (KnitTests.cpp): a knit plate 3 voxels thick in front of stone; `BlastAt` at
  the plate's face (power 96 = 6.0): `absorbed`, no `CarveSphere` in the ops, the stone behind unchanged.
- `Magic.AnAbsorbedBlastAblatesThePlate`: the same: the knit voxels within 0.25 m of the hit point are air, the rest of
  the plate stays; a second blast at the same spot after the first is carving stone (not absorbed if it hits stone;
  absorbed again if it hits remaining knit).

**Checks**
1. Build. 2. `tools/ctest.sh -R 'Propagate\.|Magic\.' --output-on-failure`. 3. `tools/ctest.sh`. 4. doc-check.

**Must not break**
- `Trace.SingleChunkMatchesReferenceDDA` (nothing in `trace.glsl` changes).
- Rule 6: the heap key ends in (z, y, x); nothing iterates an unordered container.

**Done when**
- [ ] Six tests pass.

**Traps**
- A strip on a wall hit grows out of the wall (n horizontal): that is the design ("a ceiling hit hangs a wall").
- `Dp` can be 0 when aiming straight into the face: the `(1, 0)` fallback is required, or the strip is empty.

## Card 7: KnitRegistry: mask entries, cheap expiries, site checks by what changed, BlastOptions::noCarve and the minimal StructureCredit, the conjured-piece rule

| | |
|---|---|
| Goal | Conjured matter crumbles on schedule, exactly and only its own voxels, and whatever it held up falls by the load check |
| Depends on | cards 2, 3, 5 |
| Effort | 1 d (e) |
| Touches | sim (Knit, Blast). Wire: no. Movement: no |

**Read first** (about 35 KiB)
- magic.md "`sim::KnitRegistry`", "Expiry and structure", "Caps" (the conjured rows).
- shred.md lines with "Structure credit" (`grep -n 'Structure credit' engine/docs/design/shred.md`, 10 lines).
- `engine/src/sim/internal/Blast.cpp` around `TakePieceSlot` in `SearchIslands` (the candidate list).

**Files**
- fill `engine/src/sim/Knit.cppm`, `internal/Knit.cpp`.
- change `engine/src/sim/internal/Blast.cpp`: `noCarve`; the conjured-piece rule.

**Interfaces**

```cpp
inline constexpr uint32_t kMaxConjuredPerPlayer = 6000;
inline constexpr uint32_t kMaxConjuredPerRegion = 60000;
inline constexpr uint32_t kMaxTurnedPerRegion = 40000;
struct KnitOwnerInfo {
    uint8_t layer = 0;        ///< 0 conjure (knit), 1 turn (rime, heat; [mag-3])
    uint8_t expiryMap = 0;    ///< Map_Unknit for conjure
    uint8_t caster = 0;       ///< player slot + 1; 0 none
    uint32_t dueTick = 0;
    bool hasShape = false;    ///< its matter came from one MapShape: expire with that shape
    voxel::ShapeQ shape;
};
class KnitRegistry {
public:
    uint32_t NewOwner(const KnitOwnerInfo& info);                                   ///< ids count up from 1
    /// Records the voxels an op of `owner` changed into an id of its layer (edits from ApplyOp).
    void Record(uint32_t owner, std::span<const voxel::VoxelEdit> edits, const voxel::MaterialLibrary& materials);
    /// Due owners in (dueTick, owner) order: one expiry op each, applied; returns the applied ops in order.
    std::vector<AppliedOp> RunExpiries(Simulation& simulation, uint32_t tick);
    /// Expire `caster`'s oldest conjure owners now until `incoming` more voxels fit kMaxConjuredPerPlayer.
    std::vector<AppliedOp> MakeRoom(Simulation& simulation, uint8_t caster, uint32_t incoming);
    uint32_t LiveVoxels(uint8_t layer) const;
    uint32_t LiveVoxelsOf(uint8_t caster) const;
    size_t Owners() const;
private: /* see Steps */
};
struct StructureCredit {
    static constexpr int64_t kWorkPerTick = 15000;
    static constexpr size_t kMaxQueued = 256;
    int64_t credit = 0;
    uint32_t dropped = 0;                 ///< queue full
    std::deque<glm::ivec3> queue;
    void QueueSite(const glm::ivec3& chunk);
    /// Every chunk where an edit turned a collider into a non-collider.
    void QueueFromEdits(std::span<const voxel::VoxelEdit> edits, const voxel::MaterialLibrary& materials);
    /// Refill once (call once per tick), then run sites while credit > 0; results in run order.
    uint32_t Run(Simulation& simulation, std::vector<BlastResult>& out);
};
bool IsCollider(const voxel::MaterialLibrary& materials, uint8_t id);   ///< id != 0 and not Liquid (not Film from [mag-5])
```

**Steps**
1. Registry data: `std::map<std::tuple<int,int,int>, std::array<std::vector<std::pair<uint16_t, uint32_t>>, 2>>
   m_Chunks` (per chunk, per layer, sorted by voxel index `x + 32 * (y + 32 * z)` of the chunk-local voxel);
   `std::map<uint32_t, Owner> m_Owners` with `Owner { KnitOwnerInfo info; uint32_t live = 0; glm::ivec3 boxMin,
   boxMax; bool overlapped = false; bool any = false; }`.
2. `Record(owner, edits)`: for each edit whose `newId` has `Conjured` (layer 0; layer 1 in `[mag-3]`): find the entry
   for that voxel in its chunk's layer vector (binary search); if present with another owner, that owner's `live -=
   1`, set the entry's owner; else insert keeping the order. `live += 1`; grow the owner's box (`any = true`). Then
   for every other live owner of the same layer whose box meets this owner's box (both `[min, max)`): set
   `overlapped = true` on both.
3. `RunExpiries(sim, tick)`: collect owners with `dueTick <= tick` in `(dueTick, id)` order. For each:
   1. If `live == 0`: erase it, no op.
   2. Cheap form: if `!overlapped` and `hasShape`: op `Map(expiryMap, shape)`. Else if `!overlapped`: op
      `Map(expiryMap, ShapeQ{kShapeBox, boxMin, boxMax})`.
   3. Mask form (overlapped): the voxels this owner still owns (scan its box's chunks' vectors) that still hold an id
      the map changes (`map[world.GetVoxel(v)] != id`): a `VoxelMask` over their bounding box, op `MapMasked(expiryMap,
      origin, mask)`. No voxel: erase, no op.
   4. Apply with `ApplyOp(world, op, &edits)`, push the `AppliedOp`; remove every entry of this owner from the chunk
      vectors; erase the owner.
   The caller queues site checks from each op's edits (`QueueFromEdits`).
4. `MakeRoom(sim, caster, incoming)`: while `LiveVoxelsOf(caster) + incoming > kMaxConjuredPerPlayer` and the caster
   has a conjure owner: expire its oldest owner (smallest id) now as in step 3; return the ops. Log `Magic: knit
   cap: player <caster>: oldest conjuring crumbled (<n> voxels)` once per call that expired something.
5. `IsCollider`. `QueueFromEdits`: for each edit with `IsCollider(old) && !IsCollider(new)`: `QueueSite(chunk of
   voxel)`. `QueueSite`: skip if the chunk is already in `queue`; if `queue.size() == kMaxQueued`: `++dropped`, skip;
   else push back.
6. `Run(sim, out)`: `credit = clamp(credit + kWorkPerTick, -6 * kWorkPerTick, kWorkPerTick)`; while `credit > 0` and
   the queue is non-empty: pop front `c`; `r = Blast(sim, ChunkSite(world, c), {.noCarve = true})`; `credit -=
   CastWorkOf(r).Total()`; `out.push_back(r)`; count it. Return the count.
7. `Blast(site, options)` with `options.noCarve`: skip step 1 entirely (no carve op in `ops`, `carvePending` empty);
   everything else as before (the site's `radius == 0` already skips objects and push).
8. The conjured-piece rule in `SearchIslands`: when building replication candidates, an island whose volume holds
   any id with `Conjured` is not a candidate (it never asks `TakePieceSlot`): its `Detach` keeps netId 0.
   Offline this changes nothing (no replication).

**Tests** (KnitTests.cpp)
- `KnitRegistry.ExpiryRemovesOnlyItsOwnMatter`: owner 1 knits a 10x3x6 wall (MapShape Knit box, recorded);
  carve a hole through it; owner 2 knits a plug in the hole (MapMasked); at owner 1's due tick its expiry leaves the
  plug (owner 2's voxels) and removes the rest; at owner 2's tick the plug goes. Expected: after both, no knit voxel.
- `KnitRegistry.ShapeExpiryOnlyWithoutOverlap`: one owner alone expires with a `MapShape` box op (17-29 B by
  `OpWireBytes`); two owners with meeting boxes both expire with `MapMask` ops.
- `KnitRegistry.ReplayGivesTheSameOps`: the same script of records and expiries on two registries and two equal
  worlds gives equal op lists (`operator==` on `WorldOp`) and equal hashes.
- `KnitRegistry.PlayerCapExpiresTheOldestFirst`: caster 3 records 4 walls of 1 500 voxels; `MakeRoom(3, 1500)`
  expires exactly owner 1; `LiveVoxelsOf(3) == 4500`.
- `KnitRegistry.FillRunsNoLoadCheckAndExpiryRunsASiteCheck`: a knit beam under a stone slab on a thin stone neck:
  the fill queues no site; its expiry queues the beam's chunks; `StructureCredit::Run` then releases the slab
  (`Detach` in `out`) within 3 ticks of runs.
- `StructureCredit.QueueIsBoundedAndDeduplicated`: 300 distinct chunks queued: 256 kept, `dropped == 44`; queuing a
  queued chunk again changes nothing.
- `StructureCredit.CreditRefillsAndCarriesDebt`: three expensive sites in one tick run while credit > 0 (the first
  runs at credit 15 000; after a site of work 40 000 the credit is -25 000 and no more run that tick; the next tick
  refills to -10 000 and runs none; the tick after to 5 000 and runs one).
- `Magic.ConjuredPiecesNeverTakeASlot`: on a replicating simulation (`ServerConfig`), a knit block falling as an
  island of the replicated size gets a `Detach` with netId 0; a stone block of the same size gets a NetId.

**Checks**
1. Build. 2. `tools/ctest.sh -R 'KnitRegistry\.|StructureCredit\.|Magic\.' --output-on-failure`.
3. `tools/ctest.sh`. 4. doc-check.

**Must not break**
- "World edits and blasts": the load check never runs after a fill (`FillBox`, editor, generation, a knit fill).
- "Replicated bodies": `TakePieceSlot` budgets and order for non-conjured pieces unchanged
  (`Replication.AtMostTwoNewServerBodiesPerTick`).
- `Neck.BudgetsCountWorkNotTime`: the credit charges counters only, never time.

**Done when**
- [ ] Eight tests pass; `Magic.ABlastOnFreshKnitIsAbsorbed` / `AnAbsorbedBlastAblatesThePlate` still pass with
  the island search after the ablation.

**Traps**
- Never iterate `std::unordered_map` to decide an order. `std::map` keyed by owner id gives `(id)` order; sort due
  owners by `(dueTick, id)` explicitly.
- The box expiry must not run when `overlapped` is set, even if the other owner has expired since (the bit stays: a
  mask is always correct).

## Card 8: sim::ZoneSet with Impulse and Lift: insert, the ZoneCancel truncation, replay retention; every lookup takes the tick being stepped

| | |
|---|---|
| Goal | A small sorted set of zones that every peer can ask "what acts on this player at tick T" |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | sim (Zones, Simulation). Wire: no. Movement: no (nothing reads it yet) |

**Read first** (about 12 KiB)
- magic.md "`sim::ZoneSet`".

**Files**
- fill `engine/src/sim/Zones.cppm`, `internal/Zones.cpp`.

**Steps**
1. `Insert(z)`: refuse (false) when `m_Zones.size() >= kMaxZoneRecords` or a zone with `z.zoneId` exists. Insert
   keeping `(startTick, zoneId)` order (`std::upper_bound`).
2. `Cancel(id, endTick)`: find by id; `if (endTick <= startTick) duration = 0; else duration = min(duration,
   endTick - startTick)`. Never lengthens. Return true when found.
3. `ZoneBoundsQ(zone)`: sphere `a +- r`; capsule `min(a, b) - r .. max(a, b) + r`; cylinder x, z `a +- r`, y `a +-
   halfHeight`; box `[16a, 16b - 1]`.
4. `ForEachLive(tick, reachMinQ, reachMaxQ, fn)`: for zones in order with `startTick <= tick < EndTick()` whose
   bounds meet the reach box (inclusive), call `fn(const Zone&)`.
5. `PruneEndedBefore(tick)`: erase zones with `EndTick() <= tick` (`std::erase_if`, order kept).
6. `LiveOrPendingAt(tick)`: count with `EndTick() > tick`.

**Tests** (ZoneTests.cpp)
- `ZoneSet.ZonesAreKeptInStartThenIdOrder`: insert ids 5 (start 10), 3 (start 10), 9 (start 4): `All()` is 9, 3, 5.
- `ZoneSet.CancelOnlyTruncates`: a Lift start 100 duration 90; cancel at 130 -> duration 30; cancel at 200 ->
  still 30; cancel at 90 -> 0. Unknown id -> false.
- `ZoneSet.LookupsUseTheGivenTick`: a zone live on [100, 190): `ForEachLive(99)` none, `(100)` one, `(189)` one,
  `(190)` none; after `PruneEndedBefore(150)` it is kept, after `PruneEndedBefore(190)` it is gone.
- `ZoneSet.FullSetRefuses`: 64 inserts succeed, the 65th fails.

**Checks**
1. Build. 2. `tools/ctest.sh -R 'ZoneSet\.'`. 3. `tools/ctest.sh`. 4. doc-check.

**Done when**
- [ ] Four tests pass.

**Traps**
- `startTick + duration` is `uint32_t`: use `EndTick()` everywhere, never `int`.

## Card 9: PlayerState push and status, StepPlayer revision 2 (zones, push decay, Lift), the UpdateCharacter vertical parameter, kPlayerRuleRevision = 2

| | |
|---|---|
| Goal | A Shockwave throws a player 3-4 m and an Updraft carries one 6-10 m up, the same way on every peer |
| Depends on | cards 1, 8 |
| Effort | 1 d (e) |
| Touches | sim (Player), physics (UpdateCharacter), net (callers pass zones), game (`src/main.cpp` caller). **Movement rule: yes, `kPlayerRuleRevision` 2** (this card only). Wire: no |

**Read first** (about 40 KiB)
- magic.md "`PlayerState`, `StepPlayer` and the physics call" (steps 3 Impulse and Lift, 4, 6, 7, 8 only).
- `engine/src/sim/internal/Player.cpp` `StepPlayer`, `LimitToArea`, `AreaBound` (whole function block).
- `engine/src/physics/internal/PhysicsWorld.cpp` `PhysicsWorld::UpdateCharacter` (whole).
- The callers: `grep -rn 'sim::StepPlayer(' engine/src src engine/tests` (13 sites; the server's in
  `StepPlayers`, the client's in `PredictTick`, `Reconcile` and the resume replay, the game's `OnFixedUpdate`).

**Files**
- change `engine/src/physics/...PhysicsWorld.cppm/.cpp`, `engine/src/sim/Player.cppm`, `internal/Player.cpp`.
- change the callers in `engine/src/net/internal/` (server and client) and `src/main.cpp` to pass `PlayerZones`.

**Steps**
1. `UpdateCharacter(..., vertical)`: after `velocity += gravity * dt;` and before adding the wish:
   `if (vertical && IsFinite(*vertical)) velocity.SetY(std::max(velocity.GetY(), *vertical));`. When `vertical &&
   *vertical > 0`, set `update.mStickToFloorStepDown = JPH::Vec3::sZero()` for this update.
2. `StepPlayer(..., zones, dt)` keeps today's order and adds (`vs = zones.world ? world->VoxelSize() : 0.125f`):
   1. Edges and cast as today (`TakeCast` unchanged; `CastCooldownFor` returns `castCooldownTicks` while `slowQ ==
      0`, so nothing changes now).
   2. The gate as today.
   3. Zones: if `zones.zones != nullptr`: `feetQ = llround(position / vs * 16)` per axis (int64); reach box =
      `PlayerReach(position)` in q (`floor(min / vs * 16)`, `ceil(max / vs * 16)`). `vertical` starts empty. For each
      zone of `ForEachLive(zones.tick, ...)` with `(teamMask & teamBit) != 0`:
      - Impulse, only when `zones.tick == startTick`: membership and push as Decisions 15 and the design:
        sphere: `d = feetQ - a` (3D); cylinder: `d = (feetQ.x - a.x, 0, feetQ.z - a.z)`, member when `|feetQ.y - a.y| <=
        halfHeight`; capsule: `d` = feet minus the nearest point of segment `ab` (int64: `t = clamp(dot(feet - a, u), 0,
        dot(u, u))`, nearest `= a + u * t / dot(u, u)` truncated). `len = ISqrt64(dot(d, d))`, `R = radiusQ`; member
        when `len < R`. `f = strength0 * (R - len) / R`. Sphere / cylinder: `push.x += d.x * f / max(len, R / 8)`,
        `push.z += d.z * f / max(len, R / 8)`. Capsule: `ul = ISqrt64(u.x*u.x + u.z*u.z)` (if 0 skip horizontal),
        `push.x += u.x * f / ul`, `push.z += u.z * f / ul`. Up: `push.y += strength1 * (R - len) / R`. All int64, then
        clamp each axis to +-1500.
      - Lift (every live tick; cylinder): horizontal `dist = ISqrt64(dx*dx + dz*dz)`, member when `dist <= R` and
        `bottom <= feetQ.y <= top` (`bottom = a.y - halfHeight`, `top = a.y + halfHeight`). `rampQ =
        lround(0.25 / vs * 16)`, `hoverQ = lround(0.5 / vs * 16)` (computed here from the world's voxel size, the same
        formula as card 4: a zone carries no ramp on the wire). `ramp = min(R - dist, rampQ)`, `ease = min(max(top -
        feetQ.y, 0), hoverQ)`, `target = strength0 * ramp * ease / (rampQ * hoverQ)` cm/s (int64). `vertical =
        max(vertical, target / 100.0f)`; set `Lifted`.
   4. Wish as today, then `wish.x += push.x / 100.0f`, `wish.z += push.z / 100.0f` **before** the play-area cut;
      after the cut, if an axis was reduced, zero `push` on that axis (Decision 14; `Player.KnockbackStopsAtThePlayArea`).
   5. Vertical kick: if `push.y > 0`: `vertical = max(vertical.value_or(-inf), push.y / 100.0f)`, `push.y = 0`.
   6. `UpdateCharacter(character, dt, wish, jumpSpeed, vertical)`.
   7. After the physics call: if on the ground: clear `Lifted`; decay `push.x`, `push.z` toward 0 by
      `pushDecayGround` (on ground) or `pushDecayAir` (in air), never past 0.
3. The old overload forwards with `PlayerZones{}` (no zones: push still decays, so a restored state with push
   behaves the same).
4. Callers: the server passes `{&simulation->Zones(), &simulation->World(), Tick(), teamBit}` (team from `[wp-7]`'s
   fighter state; `1` if it has none); the client passes `{&simulation->Zones(), &simulation->World(), tick, 1u <<
   team}` with `tick = nextTick` when predicting and **the history entry's own tick** when replaying
   (`HistoryEntry::tick`, never `nextTick`); the game passes `engine.Simulation().Zones()` and `Simulation().Tick()`.
5. `kPlayerRuleRevision = 2` with the comment line `2: push, Impulse and Lift zones, the vertical parameter
   ([mag-1], magic.md)`.

**Tests** (PlayerTests.cpp, sim)
- `Player.PushDecaysToZeroInBoundedTicks`: a standing player with `push = {1500, 0, 0}`: on the ground it is 0 after
  at most 34 ticks (1500 / 45 rounded up); position moved between 1.5 and 4 m.
- `Player.ZonesAreEvaluatedInZoneOrderFromQuantisedFeet`: two Impulse zones at the same start tick on the same
  player, inserted in reverse id order: the resulting push equals the sum computed by hand in id order; two players
  whose feet differ by 1e-4 m get equal pushes (quantised feet).
- `Player.ZoneEdgeAndCentreAreContinuous`: feet moved across the Impulse's dead radius (`R / 8`) in 1 q steps: no step
  changes `|push|` by more than 30 cm/s; at the rim the push is within 10 cm/s of 0.
- `Player.LiftCarriesAPlayerToTheTopAndHovers`: an Updraft-sized cylinder 6 m tall: after 90 ticks the feet are
  between 5 and 6.2 m above the start; `Lifted` set during the lift, cleared after landing.
- `Player.KnockbackStopsAtThePlayArea`: a player 0.3 m inside a bounded play area, an outward Impulse of 900 cm/s:
  the feet never pass the area's edge and `push` on that axis is 0 after the step that met it.
- `Player.ImpulseActsOnlyOnItsStartTickAndOnlyOnItsTeams`: a zone with `teamMask = 0b10`: team 0 untouched, team 1
  pushed; stepping the tick after `startTick` adds nothing.
- Existing `SessionFile.BuildHashSeesThePlayerRuleRevision` passes with the new value; `Movement.*` (net) all pass,
  `Movement.NoCorrectionAbove2cmOnFlatGround` still 0 corrections.

**Checks**
1. Build. 2. `tools/ctest.sh -R 'Player\.|Movement\.|SessionFile\.' --output-on-failure`. 3. `tools/ctest.sh`.
4. doc-check. 5. `VERIFY_SLOT=1 tools/verify.sh quick` (G3's "on the ground", G11's corrections).

**Docs**
- CLAUDE.md section 5 "Movement": add "Zones (`sim::ZoneSet` in the `Simulation`) are evaluated inside `StepPlayer`
  from quantised feet at the stepped tick; push decays; revision 2" with the new tests' names (in card 12 if you
  prefer one docs commit: say which).

**Must not break**
- "Movement": one movement rule; `TerrainColliders::FlushBox` before every step; the play area cut (a move is never
  undone); `Movement.NoCorrectionAbove2cmOnFlatGround` 0.
- Every caller passes the tick it steps; a replay never passes `nextTick`.

**Done when**
- [ ] Six new tests pass; all `Movement.*` pass; verify quick PASS.

**Traps**
- `push` is integer; convert to m/s only at the physics call. Keep the integer decay in the state, not a float.
- Lift's target is a lower bound on `velocity.y`, not an addition: a player already rising faster keeps its speed.

## Card 10: sim::CastSpell, the whole pipeline in Sim; Shockwave and Updraft; the spawn guard; server damage hooks

| | |
|---|---|
| Goal | One call casts any of the four spells on a `Simulation` and returns its ops, zones, damage and costs; follow-ups and expiries run on their ticks |
| Depends on | cards 4-9; `[wp-7]` (`sim::TeamId`, spawn points, damage) |
| Effort | 1 d (e) |
| Touches | sim (Cast). Wire: no. Movement: no (zones exist; their rule is card 9's) |

**Read first** (about 55 KiB)
- magic.md "The cast pipeline", "The element verbs" (the Metal and Pressure columns), "Follow-ups", "Caps", the four
  spells' entries, "Budgets and costs" ("Work").
- `engine/src/sim/Combat.cppm` (`[wp-7]`): team type, spawn points, the damage function.
- `engine/src/net/internal/Server.cpp` `ResolveRay` (the cast ray with the body test).

**Files**
- fill `engine/src/sim/Cast.cppm`, `internal/Cast.cpp`.

**Interfaces**

```cpp
enum class SpellRefusal : uint8_t { None, NotBuilt, FormCooling, NotAttuned, Missed, Outside, TooMuchConjured, TooManyZones, NearSpawn };
const char* ToString(SpellRefusal refusal);
struct SpellCast {
    uint8_t player = 0;          ///< slot + 1; 0 = a script
    TeamId team{};               ///< [wp-7]'s team type
    SpellId spell = 0;
    uint32_t inputTick = 0;
    glm::dvec3 eye{0.0};         ///< metres: the eye at the input's tick (StateAt on a server)
    glm::vec3 dir{0.0f, 0.0f, -1.0f};
};
struct SpellTarget { uint8_t slot = 0; uint8_t team = 0; glm::vec3 feet{0.0f}; };   ///< living players at this tick
struct SpellDamage { uint8_t slot = 0; uint16_t amount = 0; };
struct SpawnArea { uint8_t team = 0; glm::ivec3 min{0}, max{0}; };   ///< voxel box [min, max), full height
struct CasterState { std::array<uint32_t, 4> formReadyTick{}; uint8_t elementA = 0, elementB = 2; };   ///< Metal + Pressure
struct ZoneOwner { uint8_t casterTeam = 0; uint8_t caster = 0; bool dispellable = false; };
struct ScheduledAction {
    uint32_t tick = 0; uint32_t seq = 0;   ///< run order (tick, seq)
    uint8_t kind = 0;                      ///< 0 band (op), 1 zone tick (damage, bodies)
    uint32_t owner = 0;                    ///< band: the registry owner
    WorldOp op;                            ///< band: its MapMask
    uint16_t zoneId = 0; SpellId spell = 0; uint8_t casterTeam = 0;
};
struct CastContext {
    KnitRegistry registry;
    StructureCredit structure;
    std::vector<ScheduledAction> schedule;         ///< kept sorted by (tick, seq)
    uint32_t nextSeq = 1;
    uint16_t nextZoneId = 1;                       ///< skips 0 on wrap
    std::map<uint16_t, ZoneOwner> zoneOwners;
    std::map<uint8_t, CasterState> casters;
    std::vector<SpawnArea> spawns;
    OperatorWalls walls;                           ///< set by the owner (WindowWalls offline)
    uint32_t maxZones = 32, maxZonesPerPlayer = 4;
};
struct SpellStats { uint32_t path = 0, front = 0, contact = 0, visits = 0, chunksRead = 0, slices = 0, ops = 0, bytes = 0,
                    playersHit = 0, released = 0; };
struct CastSpellResult {
    SpellRefusal refusal = SpellRefusal::None;
    glm::ivec3 hitQ{0}; uint8_t face = 0;          ///< face: axis * 2 + (sign < 0); for CastFx in [mag-2]
    std::vector<BlastResult> groups;               ///< every op applied now, in order (a bore: its blast; others: one op each)
    std::vector<Zone> zonesStarted;
    std::vector<std::pair<uint16_t, uint32_t>> zonesCancelled;   ///< (zoneId, endTick)
    std::vector<SpellDamage> damage;
    SpellStats stats; CastWork work; double ms = 0.0;   ///< ms is logged only
};
struct ScheduledResult { std::vector<BlastResult> groups; std::vector<SpellDamage> damage; uint32_t sitesRun = 0; };
SpellRefusal CheckCapture(const CastContext& ctx, const SpellTable& table, const SpellCast& cast, uint32_t tick);
CastSpellResult CastSpell(Simulation& simulation, const SpellTable& table, const SpellCast& cast, CastContext& ctx,
                          std::span<const SpellTarget> targets);
ScheduledResult RunScheduled(Simulation& simulation, const SpellTable& table, CastContext& ctx, uint32_t tick,
                             std::span<const SpellTarget> targets);
CastWork CastWorkOf(const CastSpellResult& spell);
/// Spawn areas from team spawn points (team, feet in metres): a box 6 m around each (V(6) voxels per side in x and
/// z), full height (the window's rows). The caller converts [wp-7]'s spawn list into the pairs.
std::vector<SpawnArea> SpawnAreasOf(std::span<const std::pair<uint8_t, glm::dvec3>> spawns, const voxel::World& world);
```
`TeamId` is `[wp-7]`'s type; wherever this pack needs a team bit it uses `static_cast<uint8_t>(team)`.

**Steps**
1. `CheckCapture`: spell >= 16 or `!table.spells[spell].built` -> NotBuilt; element not `elementA`/`elementB` of
   `casters[player]` (a missing caster: the default) -> NotAttuned; `tick < formReadyTick[form]` -> FormCooling.
   `CheckCapture` changes nothing. On `None` the **caller** sets `casters[player].formReadyTick[form] = tick +
   forms[form].cooldownTicks` at capture (Decision 19: never given back). `CastSpell` never reads or writes it.
2. `CastSpell` at `T_run = simulation.Tick()`:
   1. Ray: `World::Raycast(eye, dir, castRange)` (castRange from the caller's `PlayerTuning`; take 60 m) and
      `Physics().Raycast` for `Dynamic | Rubble` up to the terrain distance (as `ResolveRay`). None -> Missed. A body
      in front: Siege Cannon damages it (`Scene::Damage(entity, hit point, pierceRadius metres)`) and stops (no bore,
      no zone); the other spells take the air cell in front of the body's hit point as `s0`. Terrain: `h` =
      `hit.voxel`, `n` = `lround` of `hit.normal` per axis, `s0 = h + n`, `hitQ = QuantizeSphere(hit.position / vs,
      0).center`. `face = axis * 2 + (sign < 0)`. `aim = AimOf(dir)`.
   2. Outside: `hitQ / 16` outside `ctx.walls` -> Outside.
   3. Operator and verbs per spell:
      - **Siege Cannon**: `Pierce(world, walls, {hitQ, aim, r, power, budget, maxSlices, &boreCost, &materials})`.
        Not bored: no op. Else group = `Blast(sim, CapsuleSite(world, capsule, power, hitQ))` (carve + islands + load
        check). Zone: capsule `a = capsule.b`, `b = capsule.b + (step * lengthQ) / 16`, radius `zone.radiusQ`,
        Impulse `strength {900, 200}`, enemies. Direct damage 30 to each enemy target whose capsule
        (`PlayerBounds(feet)` grown by the bore radius in metres) contains a sample of the segment eye -> B (samples
        every 0.25 m, float, server only).
      - **Barricade Wave**: `BurstStrip` with `seed = s0`, `FaceAxesOf(n)`, `aim`, the table's strip numbers;
        `BarricadeFill`. Exclude cells inside any target's `PlayerBounds` grown by one voxel at `T_run`, inside any
        rigid body's AABB (`Scene::QueryBodies` over the fill's box), inside any spawn area. Planned voxels = the
        sum of band sizes. Caps: `registry.LiveVoxels(0) + planned > kMaxConjuredPerRegion` -> TooMuchConjured;
        `MakeRoom(caster, planned)` (its ops are groups of this cast). Owner = `NewOwner({0, Map_Unknit, player,
        T_run + lifetime})`. Band k: `ScheduledAction{tick = T_run + bandFirstTick + k * bandSpacingTicks, kind 0,
        owner, op = MapMasked(Map_Knit, origin, mask)}` (empty bands are skipped).
      - **Updraft**: `PlumeSingleColumn(world, walls, s0, riseMax)`. Zone cylinder: `a = (16 s0.x + 8, (bottom + top)
        / 2, 16 s0.z + 8)` with `bottom = 16 * column.bottom.y`, `top = 16 * (column.top.y + 1)`, `halfHeight = (top -
        bottom) / 2`, radius 128; Lift `{900, 0}`, everyone, 90 ticks. A zone-tick action for bodies.
      - **Shockwave**: zone shape: `n` on y -> cylinder `a = (16 s0.x + 8, 16 s0.y + halfHeight, 16 s0.z + 8)` for a
        floor hit (`n.y > 0`), `a.y = 16 (s0.y + 1) - halfHeight` for a ceiling hit; else a sphere at `16 s0 + 8`;
        radius 512; Impulse `{900, 300}`, enemies. Dispel: every zone of `simulation.Zones()` with `zoneOwners[id]`
        dispellable, `casterTeam != cast.team`, its centre (`a`; capsule: `(a + b) / 2`) within `dispelRadiusQ` of
        `hitQ`, `EndTick() > T_z`: `Zones().Cancel(id, T_z)` and record `(id, T_z)`. A zone-tick action for damage 15
        and bodies.
   4. Spawn guard (before anything is applied: do the operator and the planning first, apply after the checks): any
      enemy spawn area that meets an op box (`OpChunkRange` box in voxels of every planned op: the bore's capsule box,
      each band mask box) or a zone's box (`ZoneBoundsQ / 16`) -> NearSpawn (nothing applied, no zone).
   5. Zone caps: `Zones().LiveOrPendingAt(T_run) + new zones > maxZones` or this caster's live or pending zones (count
      `zoneOwners` entries whose zone is still in the set) `+ new > maxZonesPerPlayer` -> TooManyZones.
   6. Apply: the bore's blast (step 3) runs here; zones: `start = T_run + table.zoneLeadTicks` (`T_z`), `zoneId =
      nextZoneId++` (skip 0), `Zones().Insert(z)`, `zoneOwners[id] = {team, player, kind is Lift or later Pin /
      Chill}`, `zonesStarted.push_back(z)`; zone-tick actions with `tick = T_z`.
   7. Work: `search += visits * kPropagateVisitWork + chunksRead * kColdChunkGraphWork + slices * kPierceSliceWork`;
      the bore blast's `CastWorkOf` added; `ops += OpWork` of every op applied now **and** every band scheduled (charged
      whole at `T_run`). `stats.bytes` = `OpWireBytes` of every op now and scheduled + 32 per zone start (cylinder) /
      42 (capsule) / 30 (sphere) + 7 per cancel + 32 (`CastFx`).
3. `RunScheduled(sim, table, ctx, tick, targets)` in this order:
   1. Due actions (`action.tick <= tick`) in `(tick, seq)` order. Band: drop mask voxels inside any target's grown
      capsule or any rigid body's AABB at this tick (`VoxelMask::Set(p, false)`); apply; `registry.Record(owner,
      edits)`; group. Zone tick: the zone (`Zones().Find(id)`; skip if gone); damage `zoneDamage` to each enemy target
      inside it (the membership test of card 9 on the target's quantised feet); bodies: rigid bodies whose position is
      inside the zone shape, nearest first by `(len in q, entity id)`, at most `maxBodies`, each `AddImpulse(mass *
      kick)` with the kick from the zone formula in m/s, length capped at `maxBodyKickCmS / 100`; wake them.
   2. `registry.RunExpiries(sim, tick)`: groups; `structure.QueueFromEdits(edits)` for each.
   3. `structure.QueueFromEdits` for every group of step 1 too; `sitesRun = structure.Run(sim, groups)`.
   4. `Zones().PruneEndedBefore(tick)` (server and offline: a zone is gone once its end tick passed).
4. `CastWorkOf(const CastSpellResult&)` returns `work`.

**Tests** (MagicTests.cpp; a `MagicYard` on flat ground at 0.125 m with a stone tower on a 2 x 4 voxel neck, a stone
wall, `ctx.walls = WindowWalls(world)`, team spawn areas at chosen places)
- `Magic.SiegeCannonFellsTheNeckFromRange`: cast from 8 m at the neck's middle height: the tower's top voxel is air
  after the cast (a Detach of more than 1 000 voxels in the groups); one zone started at `T_run + 24`.
- `Magic.BarricadeRisesInFourBandsAndCrumblesAt20s`: knit voxels after `T_run + 18`, `+22`, `+26`, `+30` grow in 4
  steps to between 1 250 and 1 340; at `T_run + 1200` all are air; registry empty.
- `Magic.FillNeverEntombsAPlayer`: a target standing on the barricade line: no knit voxel inside its bounds grown
  by one voxel, at the cast and at every band tick (move the target between bands).
- `Magic.UpdraftLiftsEveryoneAndShockwaveOnlyEnemies`: with the player rule (card 9) on two characters of teams 0 and
  1: an Updraft lifts both; a team-0 Shockwave pushes only the team-1 player.
- `Magic.ShockwaveTruncatesLiveEnemyZones`: an enemy Updraft live; a Shockwave within 5 m: the Updraft's end tick
  becomes the Shockwave's `T_z`; an own-team Updraft is untouched.
- `Magic.NoSpellReachesAnEnemySpawn`: a Barricade aimed into an enemy spawn area -> NearSpawn, no op; aimed at the
  own spawn's edge -> cast, but no fill cell inside the own area.
- `Magic.CastRejectedOverTheCaps`: 33 zones -> the 33rd TooManyZones (`maxZones` 32 with 4 per player over 9
  casters); region knit over 60 000 -> TooMuchConjured.
- `CastBudget.SpellWorkIsEqualColdAndWarm`: the same Siege Cannon on two equal yards, one with a warm graph cache:
  equal `work.Total()`.
- `Magic.OfflineAndServerEmitTheSameOps`: two simulations (one `ServerConfig`), the same four casts: equal op lists
  and equal hashes.

**Checks**
1. Build. 2. `tools/ctest.sh -R 'Magic\.|CastBudget\.' --output-on-failure`. 3. `tools/ctest.sh`. 4. doc-check.

**Must not break**
- "Replicated bodies": body impulses are server results; nothing in them decides a voxel.
- "Sessions": the cast work budget charges counters only (`CastBudget.CastWorkIsEqualColdAndWarm`).
- L9: no spell spawns a body; conjured pieces are rubble with lifetimes.

**Done when**
- [ ] Nine tests pass.

**Traps**
- Plan everything, check the spawn guard and caps, then apply: a refused cast must change nothing (no `MakeRoom`
  expiry, no zone, no band).
- `nextZoneId` wraps at 65535 -> 1, never 0; skip an id still present in `Zones()`.

## Card 11: Headless: --spell-demo NAME, --siege-matrix (4 rows) as gate G22, Magic: cost lines, the screenshot

| | |
|---|---|
| Goal | The owner (and verify.sh) can see each spell work in the real game, headless, with its cost |
| Depends on | card 10 |
| Effort | 1 d (e) |
| Touches | game (`src/main.cpp`), engine (`PresentBlast`: the bore's particles), tools (verify gate G22, judge, fixture, self-test), docs. Wire: no |

**Read first** (about 45 KiB)
- `src/main.cpp`: `UpdateNeckBlast` (whole), `SetNeckBlast`, the flag parsing block (`grep -n 'neck-blast'`),
  `FinalExpectations`, `OnFixedUpdate` (the player step), `PlazaPoint`, `m_Plaza`, `S(`.
- `engine/src/sim/internal/Blast.cpp` `PresentBlast` (the `CarveSphere` branch).
- `tools/verify.sh` (`QUICK_GATES`, `gate_tiers`, `gate_cmd`, `gate_G10`), `tools/lib/verify-gates.sh`
  (`judge_G10`), `tools/selftest-verify.sh` (the G10 cases), `tools/tests/fixtures/verify/G10-2.log`.
- magic.md "Headless flags and gates".

**Files**
- change `src/main.cpp`: a `CastContext`, the table, the two flags, the scripts, the log lines.
- change `engine/src/sim/internal/Blast.cpp` `PresentBlast`: treat `CarveCapsule` like `CarveSphere` (debris from
  `edits`); ignore `MapShape`/`MapMask` (no picture yet; `[mag-2]` adds `PresentSpell`).
- change `tools/verify.sh`, `tools/lib/verify-gates.sh`, `tools/selftest-verify.sh`; new
  `tools/tests/fixtures/verify/G22.log`.
- change `CLAUDE.md` section 2 (the G22 row), `docs/verify.md` (the flags, lines and the quick tier list).

**Steps**
1. The game owns `sim::SpellTable m_Spells = sim::BuildSpellTable(voxelSize, ids)` and `sim::CastContext m_Magic`
   (`walls = WindowWalls(world)`, updated every frame since the offline window moves; spawns empty offline). Every
   fixed tick (`OnFixedUpdate`, after the player step): `RunScheduled(sim, m_Spells, m_Magic, sim.Tick(), targets)`
   with `targets = {SpellTarget{1, 0, m_Feet}}`; present its groups with `engine.PresentBlast`.
2. The left-click blast (grep the offline cast in `main.cpp`, or the `[wp-6]` seam call it goes through) uses
   `sim::BlastAt(..., hit.material)` (the absorb rule offline).
3. `--spell-demo NAME` (`siege-cannon`, `barricade-wave`, `shockwave`, `updraft`) and `--siege-matrix` (all four).
   Scripts count `m_ScriptFrame` (`--after-fill` respected). Frame 20: set up (siege-cannon: the watch tower's ring
   as `--neck-blast 4` does at its frame 30; others: teleport the player with `RestorePlayer` to a fixed plaza point
   `PlazaPoint(engine, {10, 5, 10})`). Frame 30 (matrix: 30, 90, 150, 210 in the order siege-cannon,
   barricade-wave, updraft, shockwave): `CastSpell` with `SpellCast{0, team 1, spell, tick, eye, dir}` (siege-cannon:
   eye 8 m east of the neck at its middle height, dir toward it; barricade-wave: eye 1.62 m above the player looking
   4 m ahead and down; updraft: eye above the player looking straight down; shockwave: eye 3 m from the player looking
   at the ground 1 m beside its feet).
4. Log at the cast tick: `Magic: <name>: path P, front F, contact C, ops N, bytes B, work W, ms M, players hit H,
   released R` (from `stats`, `work.Total()`, `ms`; released = Detach count). Check lines:
   - siege-cannon: `Cannon: bore X.XX m <material>, tower released: N piece(s), roof AIR` (roof check as
     `UpdateNeckBlast`; Expect the roof is air).
   - barricade-wave: at `T_run + 31`: `Barricade: N knit voxels in 4 bands` (Expect 1 000 <= N <= 1 600 at 0.125 m;
     scale the bounds by `(0.125 / vs)^2` at other sizes); at its expiry tick + 1: `Barricade: crumbled, 0 knit
     voxels left`.
   - updraft: the highest feet over the 90 ticks after `T_z`: `Updraft: lifted +X.XX m` (Expect X >= 4.0).
   - shockwave: the player's horizontal speed 1 tick after `T_z`: `Shockwave: pushed X.XX m/s` (Expect X >= 3.0).
   - matrix end: `Magic: 4 spells, F failed` (Expect F == 0) and, once the registry is empty after the last expiry,
     `Magic: all conjured matter gone at tick T`.
   - `Magic: refused <name>: <refusal>` for any refusal (an Expect failure).
5. `--camera siege-matrix`: a fixed pose framing the tower and the barricade spot (pick it, print it in the report).
   `--siege-matrix` in a session (`--connect`) is refused with a warning (exit 2 as other script flags).
6. Gate G22 (tier quick): `G --frames 1500 --siege-matrix --camera siege-matrix --screenshot g22.png`, then `G
   --frames 200 --spell-demo siege-cannon --voxel-size 0.1` and `0.15`. Judge: exit 0; `Magic: 4 spells, 0
   failed`; the four check lines; `Magic: all conjured matter gone`; at 0.1 and 0.15 `Cannon: .* roof AIR` and
   `World: generation .* voxel 0.100 m` / `0.150 m` (as `judge_G10`). Fixture `G22.log` from a real run (cut to the
   judged lines plus 20 lines of context). Self-test: `case_ok "G22 passes"`, `case_bad` with `0 failed` changed to
   `1 failed`.

**Tests**
- No new GoogleTest. `Tools.VerifyAssertions` covers the judge (its self-test cases).

**Log lines and flags**
- `--spell-demo NAME`, `--siege-matrix`, `--camera siege-matrix`; the `Magic:`, `Cannon:`, `Barricade:`, `Updraft:`,
  `Shockwave:` lines above (read by `judge_G22`).

**Checks**
1. Build. 2. `./build/Shredborne --headless --validation --frames 1500 --siege-matrix --camera siege-matrix
   --screenshot /tmp/<scratch>/g22.png` (use your scratch dir) - exit 0, the lines; look at the screenshot.
3. `tools/ctest.sh`. 4. doc-check (CLAUDE.md's G22 row against `verify.sh --list`).
5. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS with G22; look at `g22.png`.

**Docs**
- `CLAUDE.md` section 2: the G22 row. `docs/verify.md`: the flags, the lines, G22 in the quick tier.

**Must not break**
- "Verification": a log line a judge reads changes with its judge and fixture; each judge has a planted fault.
- G3-G10 runs (the new code runs only under the new flags, except `RunScheduled` every tick, which does nothing with
  an empty context, and `BlastAt`, which equals `Blast` without knit).

**Done when**
- [ ] The matrix and both voxel sizes pass by hand and in `verify.sh quick`; the screenshot shows the barricade or
  the felled tower (say which frame you captured).

**Traps**
- Headless frames are one tick each (fixed frame time): `T_run + 1200` is frame 90 + 1 200 = 1 290 < 1 500.
- Teleporting the character: `RestorePlayer` and also `m_Player.position`, `m_Feet`.

## Card 12: Close [mag-1]: the magic.md row, ROADMAP, CLAUDE.md section 5, notes/net.md, verify full --ci

| | |
|---|---|
| Goal | The stage is recorded as built, its contracts are in CLAUDE.md, every check passes |
| Depends on | cards 1-11 |
| Effort | 0.5 d (e) |
| Touches | docs only (plus fixes the checks find) |

**Read first** (about 30 KiB)
- magic.md "Contract changes" and "Stages"; ROADMAP "Now", "Next" item 8, "State"; CLAUDE.md section 5.

**Steps**
1. `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage (the 6 disabled measurements of before
   stay). `grep -rn '\[mag-1\] card' engine/src src` shows nothing.
2. CLAUDE.md section 5: under "World edits and blasts" add **Spell ops** (`MapShape`, `MapMask`, `CarveCapsule`
   decide every voxel from (voxel, old id, op integers, an integer table); `Magic.ClipEqualsWhole`,
   `Pierce.WalkAndCapsuleAgree`), **The registry principle** (`KnitRegistry.ReplayGivesTheSameOps`,
   `KnitRegistry.ExpiryRemovesOnlyItsOwnMatter`), **No load check after a knit fill; site checks by what changed**
   (`KnitRegistry.FillRunsNoLoadCheckAndExpiryRunsASiteCheck`, `Pierce.BoreStaysInsideItsFloodBox`), **Conjured
   pieces take no slot** (`Magic.ConjuredPiecesNeverTakeASlot`), **The absorb rule** (`Magic.ABlastOnFreshKnitIsAbsorbed`,
   `Magic.AnAbsorbedBlastAblatesThePlate`), **The spawn guard** (`Magic.NoSpellReachesAnEnemySpawn`). Under
   "Movement": zones at the stepped tick, revision 2 (`Player.ZonesAreEvaluatedInZoneOrderFromQuantisedFeet`,
   `Player.KnockbackStopsAtThePlayArea`). Every test name must exist (doc-check checks CLAUDE.md).
3. `engine/docs/notes/net.md` section 2: `WorldOpKind` 7 `MapShape`, 8 `MapMask`, 9 `CarveCapsule` as **sim values,
   on the wire from `[mag-2]`**; the next free op kind is 10.
4. magic.md "Stages" row `[mag-1]`: State **Built** with the engine and game commits of the stage's cards; the
   design's "Status" line: `[mag-1]` built. Gate numbers in magic.md "Headless flags and gates": G22 is
   `--siege-matrix` (the design said G21; the plan moved it), G23 the spell drill.
5. ROADMAP: "Next" item 8 -> state Built (move it per ROADMAP's own convention), "State" test count, measured values
   from the `Magic:` lines (work and ms of each spell after `--after-fill`, labelled (m)) under "Performance and
   measured values".
6. `VERIFY_SLOT=1 tools/verify.sh full --ci` - PASS. `VERIFY_SLOT=1 tools/verify.sh soak` in the background - PASS
   (nothing in a session changed but the revision; report its line).

**Done when**
- [ ] full --ci PASS, soak PASS, doc-check PASS; the report lists every measured `Magic:` line.

## Values this stage lands

- notes/net.md section 2: `WorldOpKind` 7 `MapShape`, 8 `MapMask`, 9 `CarveCapsule` (sim values; wire from
  `[mag-2]`).
- `sim::kPlayerRuleRevision` 2. World-palette id 21 `knit_iron`. `MaterialFlags` bit 4 `Conjured`.
- Map numbers 1-15 (`voxel::TurningMapId`) and `SpellId` 0-17, `Form` and `Element` orders, `ZoneKind` 1-7, shape
  kinds 0-3: fixed here, wire values from `[mag-2]`.
- Gate G22 (`--siege-matrix`, tier quick).
- Planned for `[mag-2]`: `buildHash` += `SpellTableHash`, `TurningTableHash`.
