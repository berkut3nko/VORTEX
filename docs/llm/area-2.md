# [area-2] The siege event: keep, free for all, parties, objective: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/event-areas|event areas]]

| | |
|---|---|
| Stage | `[area-2]`: Hollowkeep gets its keep and runs events. Players gather inside (the lobby is the place), a countdown starts once 4 are in, everyone fights everyone (parties of friends fight together, friendly fire on) to capture the keep, the result shows, everyone goes back to their local world and the area resets for the next event. Leaving mid-fight costs a 60 s re-entry delay, and leaving under fire counts as a death |
| Design | [event-areas.md](../design/event-areas.md) "Decision" 7-15, 21-24, "Wire" (protocol 15 rows), "Tests"; [fight.md](../design/fight.md) "Rules", "Damage", "Round" (what stays as built) |
| Code read at | engine `9474e9c`, game `9729e09`, plus `[area-1]` (land it first; find code with `grep -n`) |
| Depends on | `[area-1]` (`vortex.sim:area`, `Hello::entry`, the shell, `RunSessionOn`, G25), `[wp-7]` (`vortex.sim:combat`, `ServerFight.cpp`, `FightHud.cpp`, G22) |
| Cards | 9: wire + skeleton (0.5 d), the stamped keep (1 d), the objective rule (1 d), sides, parties and friendly fire (1 d), the siege server (1.5 d), the reset loop (1 d), bots and G26 (1 d), the game: identity, party, siege HUD (1.5 d), close (0.5 d) |
| Effort | 9 d (e) |
| Changes | wire: **yes**, `net::kProtocolVersion` -> **15** (card 1 only; the number after `[area-1]`'s 14). New values: `GameMode::Siege` 3, `RoundEnd::Objective` 5, `DamageCause::Leave` 6, `RefuseReason` 7 `Reentry` / 8 `EventOver` / 9 `NoIdentity`, `RegionTemplateId::Keep` 2, `MessageType::ObjectiveUpdate` 25 (24 is `[mag-2]`'s `Attune` in the plan), `sim::ObjectiveKind` 0-4. Movement rule: **no**. `buildHash`: no new input. Gate: **G26** (tier `full`, no UDP port; the next free after G25) |

## Before you start

- `git log --oneline | grep -E '\[(wp-7|area-1)\]'` shows both in the game root and in `engine/`. Missing: stop.
- The code this stage builds on (check each with `grep -n`):
  - `engine/src/sim/Combat.cppm`: `DamageCause`, `GameMode`, `FightRules` (all fields), `SandboxRules`,
    `FighterState` (`team`, `health`, `dead`, `spawned`, `lastAttacker`, `lastAttackTick`, `kills`, `deaths`),
    `ApplyDamage`, `StartLife`, `TeamSpawnFeet` (its `kRing` table), `PickTeam`, `RoundPhase`, `RoundEnd`, `Round`,
    `RoundStep`, `StepRound`, `ScoreKill`, `kDraw`, `CountSolid`.
  - `engine/src/sim/Rules.cppm`: `TeamId`, `kNoTeam`, `PlayerView{player, team, feet, alive}`, `RulesContext`,
    `DamageEvent{target, source, amount, cause}`, `ResolveAction` ("every living enemy of the actor").
  - `engine/src/sim/Area.cppm` (`[area-1]`): `AreaDesc`, `BuiltInAreas`, `ClampEntry`, `GroundFeetAt`.
  - `engine/src/sim/Region.cppm`: `RegionTemplateId`, `RegionDesc`, `MakeRegionGenerator`, `BuildRegionTemplate`,
    `IsValid`; `engine/src/voxel/Generator.cppm`: `IWorldGenerator` (`Generate`, `VerticalBounds`,
    `ClassifyColumn`), `ChunkKind`, `ChunkContent{kind, uniformId}`, `ColumnClass{known, ...}`,
    `TerrainSettings`, `TerrainHeight`; `engine/src/voxel/Chunk.cppm`: `kChunkSize` 32, `Chunk::VoxelIndex`,
    `Chunk::ids`; `engine/src/voxel/Material.cppm`: `WorldMaterials` (`stone`, `brick`, `planks`).
  - `engine/src/net/Messages.cppm`: `Hello` (with `[area-1]`'s entry), `Refuse`, `RefuseReason` (1-5, 6 retired),
    `Roster` (`team <= 2`), `RoundUpdate` (`winner` 0/1/2/255), `KillNotice` (`cause` 1..5), `MessageType` up to 23.
  - `engine/src/net/internal/ServerFight.cpp`: `JoinTeam`, `FightJoin`, `FightLeave`, `PlayerViews`, `TeamPlayers`,
    `MakeRoster`, `MakeRoundUpdate`, `DealDamage`, `EnvironmentDamage`, `Respawn`, `StepFight`,
    `StepRoundAndLives`, `BuildFellingTowers`, `CheckCrowns`; `ServerImpl.cppm` (`fighters`, `round`,
    `sessionOver`, `Rules()`, `Fighting()`); `ServerPeers.cpp` `OnHello`, `DropPlayer`.
  - `engine/src/net/Drill.cppm`: `PilotSettings` (`fight`, `keepDistance`, `fightRange`, `reactionTicks`),
    `BotPilot` (`SetTargets`, `SetCanCast`, `TargetSlot`); `engine/src/net/Bot.cppm` `BotSettings`.
  - `engine/tools/server/main.cpp`: `--fight`, `--until-round-end`, `--require-winner`, `--clients`, the listening
    loop (`server.SessionOver() && ... endSession`), the `Fight:` lines.
  - Game: `src/FightHud.cpp` / `.h` (the banner, roster, kill feed, team colours), `src/Areas.cpp` (`[area-1]`),
    `src/Session.cpp` `SessionGame`.

## Decisions

Settled by us (2026-11-24; the owner may veto).

1. **Hollowkeep becomes a siege**: `hollowkeep.session` and `sim::BuiltInAreas()` both switch to `template = keep`,
   `site_y = <S>`, `mode = siege`, `objective = capture`, `min_players = 4`, `countdown_ticks = 600`,
   `round_seconds = 600`, `end_ticks = 600`, `max_party = 4`, `spawn_radius = 36`, `capture_ticks = 1800`,
   `reentry_seconds = 60`, `max_clients = 32`. `S` is chosen in card 2 from the terrain under the keep.
2. **The keep's boxes** (voxels at 0.125 m; `c` = the play bounds' centre voxel x / z = (3584, 0), `g` =
   `siteVoxelY`), applied in this order (later wins):
   1. air `[c-160, g, c-160] .. [c+160, g+96, c+160]` (the courtyard cleared, 40 m square, 12 m high);
   2. stone `[c-160, g-16, c-160] .. [c+160, g, c+160]` (a 2 m foundation);
   3. stone curtain, 1 m thick, 6 m high: west `[c-160, g, c-160]..[c-152, g+48, c-16]` and
      `[c-160, g, c+16]..[c-152, g+48, c+160]` (a 4 m gate gap at z in [-16, 16)), east `[c+152, g, c-160]..[c+160,
      g+48, c+160]`, south `[c-160, g, c-160]..[c+160, g+48, c-152]`, north `[c-160, g, c+152]..[c+160, g+48, c+160]`;
   4. brick keep `[c-48, g, c-48]..[c+48, g+80, c+48]` (12 m, 10 m high);
   5. air hall `[c-40, g, c-40]..[c+40, g+72, c+40]` (1 m walls, 1 m roof);
   6. air door `[c-48, g, c-8]..[c-40, g+24, c+8]` (2 m wide, 3 m high, facing west: the gate and the local plaza).
   The capture zone is `[c-40, g, c-40]..[c+40, g+32, c+40]` (the hall, 4 m high). Ids: `WorldMaterials::stone`,
   `brick`, 0 for air.
3. **Stamping is integer and order-fixed**: per chunk, the boxes in list order, each clipped to the chunk; a voxel's
   id is the last box's that contains it. The result does not depend on chunk order or thread.
4. **Sides** (event-areas decision 9-10): a solo joiner takes the smallest side id in 1..64 that no present player
   holds; a party joiner (non-zero `partyKey`) takes the side of the lowest-slot present player with the same key if
   that side has fewer than `max_party` members, else a new side as a solo joiner. A side id is freed when its last
   member leaves. `max_clients` of a siege session is at most 64 (the parser refuses more).
5. **Damage** in a siege: everyone but the caster (`FightRules::friendlyFire` true). Kill credit: only across sides.
   `Fall` / `Lava` / `Void` / `Crush` / `Leave` credit the last attacker of another side within `creditTicks`.
6. **The leave death**: a player who leaves (asked or lost) during `Live` while its last damage from another side is
   within `creditTicks` dies by `Leave` (a `KillNotice`, the attacker credited) before it is removed.
7. **Re-entry ledger**: `Server::Impl::reentry` (`std::map<PlayerId, uint32_t untilTick>`; ordered: iteration never
   decides anything but the map is ordered anyway). A leave during `Countdown` or `Live` writes `tick +
   reentryTicks`. A siege Hello whose id has `untilTick > tick` gets `Refuse{Reentry, seconds = ceil((untilTick -
   tick) / 60)}`. Cleared at the reset (a new `Server`). A siege Hello with a zero id gets `Refuse{NoIdentity}`.
8. **Spawn ring**: 8 points at `spawn_radius` from the play bounds' centre, the `kRing` table of `TeamSpawnFeet`
   (moved to `sim::kRingOffsets`, exported), the ground under each by `GroundFeetAt`. A respawn takes the point
   whose smallest squared distance (integer centimetres, x / z) to every living player of another side is largest;
   ties: the lowest index; with no such player, index `(lifeSeq + slot) % 8`. `Countdown`'s respawns take points in
   slot order with the same rule, counting players already placed this tick as living.
9. **Join spawn in a siege**: the entry (`ClampEntry` + `GroundFeetAt`, as `[area-1]`), in every phase. A Hello
   without an entry (bots without one): the ring rule.
10. **The round**: `Waiting` (`RoundPhase::Warmup`) until at least `min_players` players on at least 2 sides ->
    `Countdown` -> `Live` -> `Ended` -> session over. Win in `Live`: the objective completes (`RoundEnd::Objective`,
    winner = the holder's side); fewer than 2 sides with players for `forfeitTicks` (the remaining side wins,
    `Forfeit`; nobody left: a draw); time out: `MostKills` (event-areas decision 12). No overtime in a siege.
11. **The end**: at `Ended`'s end the server sends `RoundUpdate` (end flag), then `Refuse{EventOver, 0}` to every
    joined peer, disconnects them and sets `sessionOver`; `vortex_server --area` builds the next `Server` (card 6).
12. **Charges and the rest** stay [wp-7]'s defaults (3 charges, 72 ticks; health 100; respawn 180; protection 120).
13. **Party key**: FNV-1a 64 of the party word's bytes as typed, lower-cased ASCII (`--party WORD`, settings key
    `party`); empty word = 0. **Player id** (this stage): 16 random bytes made once and kept in
    `<settings folder>/player.id` (32 hex digits); `[area-3]` replaces it with the key-derived id. Bots: id =
    `HashMix64(seed)` twice into 16 bytes (`grep -rn "HashMix64" engine/src/voxel/Hash.cppm`).
14. **`ObjectiveUpdate`** is sent to every joined peer when the holder or the contested flag changes, at most every
    15 ticks while the progress changes, and once at each phase change; taken on receipt (like `RoundUpdate`).
15. **HUD** (game): own side blue `Rgba(70, 130, 255)`, the party's other members green `Rgba(90, 210, 110)`, every
    other side red `Rgba(230, 70, 60)`. Banner: `WAITING FOR PLAYERS (N)`, `STARTS IN 9`, then `HOLD THE KEEP` with
    `<HOLDER> 42%` (or `CONTESTED`, or `KEEP FREE`) and the clock; at the end `<WINNER> TAKES THE KEEP` /
    `<WINNER> WINS ON KILLS` / `DRAW`, then `EVENT OVER - BACK TO THE WORLD`. A side's name is its lowest-slot
    member's (`P3`, `YOU`, `YOUR PARTY`). The scoreboard groups by side.

## Skeleton

Wire (`engine/src/net/Messages.cppm`, card 1):

```cpp
    inline constexpr uint16_t kProtocolVersion = 15;   // [area-2] siege (event-areas.md "Wire")

    using PlayerId = std::array<uint8_t, 16>;
    struct Hello {
        // ... [area-1]'s fields ...
        PlayerId playerId{};      ///< [area-2] the player's id (all zero: none; a siege refuses it NoIdentity)
        uint64_t partyKey = 0;    ///< [area-2] FNV-1a 64 of the party word; 0 = solo
    };
    enum class RefuseReason : uint8_t {
        // 1..5 as before; 6 retired
        Reentry = 7,      ///< [area-2] left this event recently; Refuse::seconds says how long
        EventOver = 8,    ///< [area-2] the event ended: back to the local world
        NoIdentity = 9,   ///< [area-2] a siege needs a player id
    };
    struct Refuse {
        RefuseReason reason = RefuseReason::Malformed;
        uint16_t seconds = 0;     ///< [area-2] Reentry: seconds left
    };
    /// @brief [area-2] The objective's state (MessageType::ObjectiveUpdate = 25). 8 B.
    struct ObjectiveUpdate {
        sim::ObjectiveKind kind = sim::ObjectiveKind::None;   ///< 1..4
        uint8_t holder = 0;       ///< side id 0..64 (0 = nobody)
        uint16_t progress = 0;
        uint16_t target = 0;      ///< capture_ticks
        bool contested = false;   ///< flags bit 0; other bits refused
    };
```

New partition `vortex.sim:objective` (`engine/src/sim/Objective.cppm`, `internal/Objective.cpp`; imported by
`:combat`'s users, exported from `Sim.cppm`, listed in `engine/CMakeLists.txt`):

```cpp
export module vortex.sim:objective;

import :rules;

export namespace vortex::sim {
    /// @brief What wins a siege (event-areas.md decision 11). Wire format (ObjectiveUpdate).
    enum class ObjectiveKind : uint8_t { None = 0, CaptureKeep = 1, FellTower = 2, HoldTimer = 3, MostKills = 4 };
    inline constexpr uint8_t kMaxObjectiveKind = 4;
    const char* ToString(ObjectiveKind kind);

    /// @brief The objective's fixed data (from the session and the template).
    struct ObjectiveDesc {
        ObjectiveKind kind = ObjectiveKind::None;
        glm::ivec3 zoneMin{0}, zoneMax{0};      ///< CaptureKeep: the zone, voxels [min, max)
        uint16_t captureTicks = 1800;
        float voxelSize = 0.125f;
    };
    /// @brief The objective's state (server; ObjectiveUpdate carries it).
    struct ObjectiveState {
        uint8_t holder = 0;                     ///< side 1..64, 0 = nobody
        uint16_t progress = 0;
        bool contested = false;
        friend bool operator==(const ObjectiveState&, const ObjectiveState&) = default;
    };
    struct ObjectiveStep {
        bool changed = false;                   ///< holder or contested changed
        bool completed = false;
        uint8_t winner = 0;                     ///< side, with completed
    };
    /// @brief One Live tick of the objective. `players`: PlayerView with team = side; dead ones are skipped.
    ObjectiveStep StepObjective(ObjectiveState& state, const ObjectiveDesc& desc, std::span<const PlayerView> players);
    /// @brief The time-out winner: most credited kills per side (`sideKills[side]`), then the holder with progress
    /// above 0, else kDraw.
    uint8_t TimeoutWinner(const ObjectiveState& state, std::span<const uint16_t, 65> sideKills);
}
```

`vortex.sim:combat` additions (card 4):

```cpp
    enum class DamageCause : uint8_t { None = 0, Blast = 1, Fall = 2, Lava = 3, Void = 4, Crush = 5, Leave = 6 };
    inline constexpr uint8_t kMaxDamageCause = 6;
    enum class GameMode : uint8_t { Sandbox = 0, Deathmatch = 1, Felling = 2, Siege = 3 };
    inline constexpr uint8_t kMaxGameMode = 3;
    enum class RoundEnd : uint8_t { None = 0, Score = 1, Time = 2, Felled = 3, Forfeit = 4, Objective = 5 };
    inline constexpr uint8_t kMaxRoundEnd = 5;
    inline constexpr TeamId kMaxSides = 64;

    struct FightRules {
        // ... as built, plus:
        bool friendlyFire = false;              ///< [area-2] siege: true
        uint8_t maxParty = 4;                   ///< [max_party]
        float spawnRadius = 36.0f;              ///< [spawn_radius] metres
        ObjectiveKind objective = ObjectiveKind::None;  ///< [objective] capture | kills
        uint16_t captureTicks = 1800;           ///< [capture_ticks]
        uint32_t reentryTicks = 3600;           ///< [reentry_seconds] x 60
    };
    /// @brief The ring's unit offsets, 45 degrees apart (moved out of TeamSpawnFeet).
    inline constexpr double kRingOffsets[8][2] = { /* TeamSpawnFeet's kRing, unchanged */ };
    /// @brief Decision 4: the side of a joiner. `present[s]` = slot of a present fighter or empty.
    TeamId PickSide(std::span<const FighterState> fighters, std::span<const bool> occupied, std::span<const uint64_t> partyKeys,
                    uint64_t partyKey, uint8_t maxParty);
    /// @brief Decision 8: the ring index for a respawn of `slot`.
    uint32_t PickRingPoint(std::span<const glm::dvec3> ring, std::span<const PlayerView> players, TeamId side, uint32_t fallback);
    /// @brief One tick of a siege's round (decision 10). `sides` = sides with at least one player.
    RoundStep StepSiegeRound(Round& round, const FightRules& rules, uint32_t tick, uint32_t players, uint32_t sides,
                             const ObjectiveStep& objective, uint8_t onlySide, uint8_t timeoutWinner);
```

Region (`engine/src/sim/Region.cppm`):

```cpp
    enum class RegionTemplateId : uint32_t { Empty = 0, Playground = 1, Keep = 2 /* [area-2] */ };
    struct RegionDesc {
        // ... as built, plus:
        int32_t siteVoxelY = 0;   ///< [area-2] the template's floor level (Keep), voxels; session key `site_y`
    };
    /// @brief [area-2] The keep's boxes (decision 2) and its capture zone.
    struct KeepLayout {
        std::vector<voxel::StampBox> boxes;
        glm::ivec3 zoneMin{0}, zoneMax{0};
    };
    KeepLayout KeepLayoutOf(const RegionDesc& region, const voxel::WorldMaterials& ids);
    /// @brief [area-2] The local world's generator: the built-in terrain with every area's stamps.
    std::shared_ptr<voxel::IWorldGenerator> MakeLocalGenerator(std::span<const AreaDesc> areas, const voxel::WorldMaterials& ids);
```

`AreaDesc` lives in `:area`, which imports `:region`; `MakeLocalGenerator` therefore goes in `:area` instead
(same signature). New partition `vortex.voxel:stamp` (`engine/src/voxel/Stamp.cppm`, `internal/Stamp.cpp`):

```cpp
export namespace vortex::voxel {
    /// @brief A box of one id, voxels [min, max) (id 0 = air).
    struct StampBox {
        glm::ivec3 min{0}, max{0};
        uint8_t id = 0;
        friend bool operator==(const StampBox&, const StampBox&) = default;
    };
    /// @brief `inner` with `boxes` written over it, in list order, per chunk (event-areas.md decision 13).
    class StampedGenerator final : public IWorldGenerator {
    public:
        StampedGenerator(std::shared_ptr<IWorldGenerator> inner, std::vector<StampBox> boxes);
        ChunkContent Generate(const glm::ivec3& coord, Chunk& chunk) const override;
        HeightBounds VerticalBounds() const override;
        ColumnClass ClassifyColumn(int cx, int cz) const override;
    private:
        std::shared_ptr<IWorldGenerator> m_Inner;
        std::vector<StampBox> m_Boxes;
    };
}
```

Tests: `engine/tests/core/StampTests.cpp` (`Keep.*`; VortexCoreTests links Core), `engine/tests/sim/ObjectiveTests.cpp`
(`Objective.*`), `engine/tests/sim/SiegeRulesTests.cpp` (`SiegeRules.*`), `engine/tests/net/SiegeTests.cpp`
(`SiegeWire.*`, `Siege.*`, `AreaReset.*`).

## Card 1: Wire 15 and the skeleton

| | |
|---|---|
| Goal | Every new value and message exists, round-trips and is refused out of range; stubs compile; tests written disabled |
| Effort | 0.5 d (e) |
| Touches | net (Messages, Session), sim (Combat, Region, new Objective), voxel (new Stamp). **Wire: protocol 15 here** |

**Steps**
1. The enums and structs of the skeleton; `ToString` for the new values (`"siege"`, `"objective"`, `"leave"`,
   `"capture"`).
2. `Hello`: after the entry, `Bytes(playerId)` (16), `U64(partyKey)`: +24 B. `Refuse`: `U16(seconds)` after the
   reason (+2 B); decoding refuses `seconds != 0` unless the reason is `Reentry`. `RefuseReason` decoding accepts 1-5
   and 7-9 (6 stays refused).
3. `ObjectiveUpdate` (25): `type, kind, holder, progress u16, target u16, flags`: 8 B; refuse `kind` 0 or > 4,
   `holder > 64`, `flags > 1`.
4. `Roster`: `team <= 2` stays for every mode but `Siege`; a siege roster allows `team <= 64`. The decoder does not
   know the mode: change the rule to `team <= kMaxSides` for all modes and let the client check `<= 2` for the
   [wp-7] modes (log and ignore an entry over 2 there). `RoundUpdate`: `mode <= 3`; `winner` 0..64 or 255; `end <= 5`.
   `KillNotice`: `cause` 1..6.
5. `RegionDesc::siteVoxelY` in the `Welcome`'s region encoding (`grep -n "terrain_seed\|EncodeRegion\|Region(" engine/src/net/internal/Messages.cpp`):
   `I32` after the last field; `RegionDesc::operator==` compares it.
6. `ParseSession` keys: `mode = siege`, `site_y` (voxels), `objective = capture | kills` (`fell`, `hold`: error
   "objective not built yet"), `max_party` (1..8), `spawn_radius` (metres, 8..200), `capture_ticks` (60..36000),
   `reentry_seconds` (0..600); `template = keep`. Refused: `mode = siege` with `max_clients > 64`; `template = keep`
   with the keep (40 m) not inside the play bounds; siege with `friendlyFire` forced true by the parser (no key).
   `FormatSession` writes them for a siege.
7. `kProtocolVersion` = 15. Stubs for every new function (`// [area-2] card N fills this`). Tests of cards 2-7 in
   full as `DISABLED_`.

**Tests** (enabled here; `engine/tests/net/SiegeTests.cpp`)
- `SiegeWire.SizesAreAsDesigned`: Hello 60 B (36 + 24), Refuse 4 B (`type, reason, seconds`), ObjectiveUpdate 8 B.
  Count the bytes of the built encoders and write the numbers the code gives; if they differ from these, stop and
  report (a field is missing or extra).
- `SiegeWire.ValuesRoundTripAndRefuseOutOfRange`: each new message with `RefusesTrailingAndMissing`; ObjectiveUpdate
  kind 5, holder 65, flags 2 refused; Refuse reason 6 refused; KillNotice cause 7 refused; RoundUpdate end 6 refused.
- `SiegeWire.SessionKeysParseAndFormat`: a siege session text round-trips through `ParseSession` /
  `FormatSession`; `max_clients = 65` refused; `objective = hold` refused with the message.

**Checks**: build; `tools/ctest.sh -R 'Wire|Session' --output-on-failure`; `tools/ctest.sh`; doc-check.

**Docs**: notes/net.md section 2 (every value above, protocol 15); CLAUDE.md section 5 "Sessions" and "Fight" lists
(`sim::ObjectiveKind`, the new values).

**Must not break**: `FightWire.*` (expected sizes change only where this card says), G22 (deathmatch rosters with
teams 1-2 decode as before).

## Card 2: The stamped keep

| | |
|---|---|
| Goal | The keep exists in the generator: the server, every client and the local world make the same voxels |
| Depends on | card 1 |
| Effort | 1 d (e) |
| Touches | voxel (Stamp), sim (Region, Area), game (`main.cpp`: the offline generator). Wire: no. buildHash: no |

**Read first**: `engine/src/voxel/Generator.cppm` (all of `IWorldGenerator`'s contract: "the caller rebuilds the
occupancy pyramid", Empty / Uniform may leave the chunk untouched); `engine/src/voxel/internal/Generator.cpp`
`TerrainGenerator::Generate`, `ClassifyColumn`; `engine/src/sim/internal/Region.cpp` `MakeRegionGenerator`;
`src/main.cpp` `engine.Initialize()` (offline: built-in terrain).

**Steps**
1. `StampedGenerator::Generate(coord, chunk)`:
   1. `c = m_Inner->Generate(coord, chunk)`.
   2. Chunk voxel box: `lo = coord * 32`, `hi = lo + 32`. Collect the boxes that overlap it (`b.min < hi && b.max >
      lo` on all three axes), in list order. None: return `c`.
   3. If `c.kind != Dense`: fill all `kChunkVolume` ids with `c.kind == Uniform ? c.uniformId : 0` (by
      `Chunk::VoxelIndex(x, y, z)` for x, y, z in 0..31).
   4. For each overlapping box in order: for z, y, x over the clipped range (z outer, x inner):
      `chunk.ids[Chunk::VoxelIndex(x - lo.x, y - lo.y, z - lo.z)] = b.id`.
   5. Return `{Dense, 0}` (the caller rebuilds the pyramid; a chunk that ends all air or all one id is still
      correct as Dense; do not try to re-classify).
2. `ClassifyColumn(cx, cz)`: if any box overlaps the column's x / z range: `ColumnClass{}` (`known` false: every row
   is generated); else the inner's.
3. `VerticalBounds()`: the inner's, with `max` raised to the highest box top in metres (`maxY * voxelSize`; the
   generator does not know the voxel size: pass it to the constructor if `HeightBounds` is in metres, check).
4. `KeepLayoutOf(region, ids)`: decision 2 with `c = ((playMin + playMax) * 32 / 2)` per axis (x, z in voxels: for
   Hollowkeep (100 + 124) * 32 / 2 = 3584 and (-12 + 12) * 32 / 2 = 0), `g = region.siteVoxelY`. The sizes in the
   decision are for 0.125 m voxels: scale each length with `n(m) = llround(m / voxelSize)` as `BuildFellingTower`
   does (`n(20) = 160`, `n(1) = 8`, `n(6) = 48`, `n(2) = 16`, `n(12) = 96`, `n(6)` half keep = 48, `n(5) = 40`,
   `n(10) = 80`, `n(9) = 72`, `n(3) = 24`, `n(4) = 32`). The zone as decided.
5. `MakeRegionGenerator`: `Keep` -> `std::make_shared<voxel::StampedGenerator>(terrainGenerator,
   KeepLayoutOf(region, ids).boxes)`. `BuildRegionTemplate` for `Keep`: no ops, no lights, no objects, `spawn` =
   the play bounds' centre on the ground (it is not used by sieges: card 5), `sited` true.
6. `MakeLocalGenerator(areas, ids)`: the built-in terrain generator (as `Engine::Initialize(nullptr)` makes it:
   find how) wrapped in one `StampedGenerator` with every `Keep` area's boxes, areas in list order. The game
   (`src/main.cpp`) passes it to `engine.Initialize(...)` in the default offline run only (not with `--voxel-size`
   other than 0.125: the areas are authored for 0.125 m; with another size no areas, no stamps, log
   `Areas: off at voxel size X`).
7. Choose `site_y`: write a disabled measuring test `DISABLED_Keep.PrintTheGroundUnderTheKeep` that prints
   `voxel::TerrainHeight(TerrainSettings{}, x, z)` min / max over the curtain's square (x 428..468, z -20..20, every
   1 m). Run it once by hand (`--gtest_also_run_disabled_tests`). `site_y = floor(median height / 0.125)`. If max -
   min > 6 m, stop (`Status: blocked`) and report the numbers (the area should move). Write `site_y` into
   `hollowkeep.session` and `BuiltInAreas` together with decision 1's keys.

**Tests** (`engine/tests/core/StampTests.cpp`)
- `Keep.StampsAreTheSameInAnyChunkOrder`: a flat inner generator (a test class `FlatFake`, not named like an engine
  type), two boxes overlapping; generate the 8 chunks of a 2x2x2 block in order A and in reverse; per chunk ids equal.
- `Keep.LaterBoxesWin`: box 1 id 5, box 2 id 0 inside it: the voxel in both is 0; only in box 1 is 5.
- `Keep.AnEmptyChunkTouchedByABoxBecomesDense`: inner returns Empty; the result kind Dense, ids 0 except the box.
- `Keep.UntouchedColumnsKeepTheInnersClass` and `Keep.TouchedColumnsAreUnknown`.
- `Keep.TheLayoutFitsTheBuiltInArea` (sim suite, `engine/tests/sim/SiegeRulesTests.cpp`): every box inside the play
  bounds' voxel box; the zone inside the keep's hall; `siteVoxelY` within 2 m of `TerrainHeight` at the centre.
- `AreaSession.TheBuiltInAreaMatchesItsSessionFile` ([area-1]) still passes with the new keys.

**Checks**: build; `tools/ctest.sh -R 'Keep\.|Area|Session' --output-on-failure`; `tools/ctest.sh`; doc-check;
`VERIFY_SLOT=1 tools/verify.sh quick` (G3-G10 unchanged: the keep is 400 m away); the G25 pair by hand (the client's
hash still equals the server's: both stamp).

**Must not break**: `Terrain.GeneratorGoldenHashNamesTheGoldenTables` (the inner generator is unchanged; the golden
hash is of the inner); probes (`ProbeChunks` hashes generator output: the server's and the client's both stamp).

**Traps**
- The courtyard's air box removes terrain above `g`: a hill there leaves a cut; that is intended.
- A `Packed` / hidden-chunk shortcut that skips `Generate` for a column the inner classified as solid would skip
  the stamp: that is why touched columns return `known = false`.

## Card 3: The objective rule

| | |
|---|---|
| Goal | `sim::StepObjective` and `TimeoutWinner`, pure and tested |
| Effort | 1 d (e) |
| Touches | sim (Objective). Wire: no |

**Steps** (`StepObjective` for `CaptureKeep`; `MostKills` returns `{}` every tick; `FellTower` / `HoldTimer`
`Log::Error` once and return `{}`)
1. `count[1..64] = 0`. For each view with `alive` and `team` in 1..64: `v = floor(feet / voxelSize)` per axis
   (`std::floor` of the double, then `int32_t`); inside when `zoneMin <= v < zoneMax` on x, y, z: `count[team] += 1`.
2. `sides` = the side ids with `count > 0`, ascending. `before = state`.
3. 0 sides: `contested = false`; `progress = progress > 0 ? progress - 1 : 0`.
4. 2 or more: `contested = true` (progress unchanged).
5. 1 side `S`, `k = min(count[S], 3)`: `contested = false`; if `holder == 0 || holder == S`: `holder = S`,
   `progress = min(captureTicks, progress + k)`; else `progress = progress > 2k ? progress - 2k : 0`, and when it is
   0, `holder = S`.
6. `changed = holder != before.holder || contested != before.contested`.
7. `progress >= captureTicks`: `completed = true`, `winner = holder`.
8. `TimeoutWinner`: the side with the highest `sideKills[s]` (s 1..64); a unique maximum above 0 wins; a tie (or all
   0): `holder` if `progress > 0` and the holder is among the tied (or all are 0); else `kDraw`.

**Tests** (`engine/tests/sim/ObjectiveTests.cpp`; zone `[0,0,0]..[80,32,80]` at 0.125 m, `captureTicks` 1800)
- `Objective.OnePlayerCapturesIn1800Ticks`: one view inside; after 1799 steps not completed, step 1800 completed,
  winner its side.
- `Objective.ThreeOrMoreCaptureThreeTimesFaster`: 3 views of side 2: 600 steps; 4 views: also 600.
- `Objective.ContestedHoldsTheProgress`: side 1 for 100 ticks (progress 100), then sides 1 and 2 for 50: 100 and
  contested; then nobody for 30: 70.
- `Objective.AnotherSideDrainsThenTakesOver`: side 1 to 100; side 2 alone, 1 player: 50 ticks to 0 (2 per tick), at
  that tick holder 2; one more tick progress 1.
- `Objective.TheDeadAndTheOutsideDoNotCount`: a dead view inside, a view 1 voxel above the zone: nothing changes.
- `Objective.TimeoutGoesToMostKillsThenTheHolder`: kills {s1: 3, s2: 5}: 2; {2, 2} with holder 1 progress 10: 1;
  {0, 0}, progress 0: `kDraw`.

**Checks**: build; `tools/ctest.sh -R 'Objective\.' --output-on-failure`; `tools/ctest.sh`; doc-check.

## Card 4: Sides, parties and friendly fire

| | |
|---|---|
| Goal | The fight rules know sides, parties and friendly fire; spawns on the ring |
| Depends on | cards 1, 3 |
| Effort | 1 d (e) |
| Touches | sim (Combat, Rules). Wire: no. Movement rule: no |

**Steps**
1. `ApplyDamage`: today it refuses `source.team == target.team`. New: if `rules.friendlyFire`, refuse only when
   the source is the target (`event.source == targetPlayer + 1`: check how `DamageEvent::source` encodes the slot);
   kill credit only when `source->team != target.team`. Environment causes (`Fall`, `Lava`, `Void`, `Crush`,
   `Leave`) credit `lastAttacker` within `creditTicks` only if that attacker's side differs (`lastAttacker` is set
   only by damage from another side: change the place it is set accordingly).
2. `RulesContext` gains `bool friendlyFire = false;` and `ResolveAction`'s damage loop takes "living players other
   than the actor" when it is set (else "living enemies" as now). The server sets it from the rules.
3. `PickSide` (decision 4): `occupied[s]` per slot, `partyKeys[s]` per slot. If `partyKey != 0`: the lowest slot
   with `occupied`, the same key and `fighters[slot].team` in 1..64; count members of that side; `< maxParty` ->
   that side. Else: the smallest id in 1..64 that no occupied slot's fighter holds. All 64 held: `kNoTeam` (the server
   refuses `Full`).
4. `kRingOffsets`: move the table out of `TeamSpawnFeet` (same values; `TeamSpawnFeet` uses it; G22 unchanged).
5. `PickRingPoint(ring, players, side, fallback)`: for each index i in 0..7: `best_i = min over views with alive and
   team != side and team != 0 of (dx^2 + dz^2)` with `dx = llround(ring[i].x * 100) - llround(feet.x * 100)` (int64)
   etc.; no such view: return `fallback % 8`. Return the i with the largest `best_i`, ties the lowest i.
6. `StepSiegeRound(round, rules, tick, players, sides, objective, onlySide, timeoutWinner)` (decision 10):
   - `Warmup`: `players >= minPlayers && sides >= 2` -> `Countdown` (`startLives`, `phaseEndTick = tick +
     countdownTicks`).
   - `Countdown`: at `phaseEndTick` -> `Live` (`phaseEndTick = tick + roundTicks`, `emptySince[0] = 0`).
   - `Live`: `objective.completed` -> win `objective.winner`, `RoundEnd::Objective`. `sides < 2`: start / continue
     `emptySince[0]`; after `forfeitTicks`: `onlySide` wins (`Forfeit`), or a draw when `players == 0`. Time out ->
     `timeoutWinner` (`Time`; `kDraw` = draw). No overtime.
   - `Ended`: at `phaseEndTick` -> `sessionOver`.
   Reuse `StepRound`'s private `Win` helper for the phase change (it sets `Ended`, `phaseEndTick = tick + endTicks`).

**Tests** (`engine/tests/sim/SiegeRulesTests.cpp`)
- `SiegeRules.FriendlyFireHurtsThePartyButNeverTheCaster`: siege rules; A and B on side 1, C on side 2; a 40-point
  blast event by A on B: B loses 40; on A itself: 0.
- `SiegeRules.APartyKillIsNotCredited`: A kills B (same side): B deaths 1, A kills 0, `credit` 0.
- `SiegeRules.AFallAfterAPartyHitCreditsNobody`: B hit by A, then falls to death within 300 ticks: credit 0; hit by C
  then falls: credit C.
- `SiegeRules.SidesFillTheSmallestFreeId`: four solo joins -> 1, 2, 3, 4; slot 1 leaves; a fifth -> 2.
- `SiegeRules.PartiesShareASideUpToMaxParty`: key K for slots 0-4 with `maxParty` 4: sides 1, 1, 1, 1, 2.
- `SiegeRules.TheRingPicksTheFarthestPoint`: ring of radius 36 round (0, 0); one enemy at (36, 0): index 4 (-x).
  Two enemies at (36, 0) and (-36, 0): indices 2 and 6 tie -> 2.
- `SiegeRules.TheRoundWaitsForTwoSides`: 4 players on 1 side: stays Warmup; 4 on 2 sides: Countdown.
- `SiegeRules.TheObjectiveEndsTheRound`, `SiegeRules.ALoneSideWinsByForfeit`, `SiegeRules.TimeOutTakesTheTimeoutWinner`.
- `Combat.*`, `Fight.*` unchanged (the [wp-7] modes keep friendly fire off).

**Checks**: build; `tools/ctest.sh -R 'SiegeRules\.|Combat\.|Fight\.|Rules\.' --output-on-failure`; `tools/ctest.sh`;
doc-check; the G22 command by hand (must pass unchanged).

## Card 5: The siege server

| | |
|---|---|
| Goal | A `mode = siege` session runs the whole event on the server: sides, entry spawns, the objective, the leave death, re-entry |
| Depends on | cards 1-4 |
| Effort | 1.5 d (e) |
| Touches | net (ServerFight, ServerPeers, ServerImpl, Client). Wire: as card 1 |

**Steps**
1. `ServerImpl.cppm`: `std::array<uint64_t, 256> partyKeys{}`, `std::array<PlayerId, 256> playerIds{}`,
   `std::map<PlayerId, uint32_t> reentry`, `sim::ObjectiveDesc objective`, `sim::ObjectiveState objectiveState`,
   `std::array<uint16_t, 65> sideKills{}`, `uint32_t lastObjectiveSend = 0`, `bool objectiveDirty = false`.
   `bool Siege() const { return Rules().mode == sim::GameMode::Siege; }`.
2. `Server::Start`: for a siege, `objective = {Rules().objective, KeepLayoutOf(region, ids).zone..., captureTicks,
   voxelSize}`; log `Siege: objective capture, zone [min] .. [max], target T ticks`.
3. `OnHello` for a siege (after the [wp-7] checks): zero `playerId` -> `Refuse{NoIdentity}`; `reentry[id] > Tick()`
   -> `Refuse{Reentry, seconds}` (log `Siege: refused P<id8> re-entry, N s left`, the first 8 hex digits); a
   `playerId` already joined -> `Refuse{Full}` (one connection per id). Then `JoinTeam` -> `PickSide`
   (`kNoTeam`: `Refuse{Full}`); store `partyKeys[slot]`, `playerIds[slot]`; spawn = the entry (`[area-1]`) or the ring.
4. `DealDamage` / `ResolveAction` context: `friendlyFire` from the rules. A credited kill in a siege:
   `sideKills[killer side] += 1` (no `ScoreKill`: no team score in a siege).
5. `FightLeave(slot)` (called for asked leaves and lost peers alike): in a siege, before the removal: if
   `round.phase == Live` and the fighter is alive and `lastAttacker != 0` and `Tick() - lastAttackTick <=
   creditTicks`: a `DamageEvent{target slot, source lastAttacker, 999, Leave}` through `DealDamage` (kill credit, the
   feed, one path). Then if the phase is `Countdown` or `Live`: `reentry[playerIds[slot]] = Tick() + reentryTicks`.
   Log `Siege: slot S left in <phase>, re-entry in N s`.
6. `StepRoundAndLives` for a siege: `players` = joined fighters with a side, `sides` = distinct sides; in `Live`,
   `step = StepObjective(objectiveState, objective, PlayerViews())` (once per tick, after the players' steps and
   casts: the [wp-7] order of `StepFight`); `onlySide` = the side when `sides == 1`; `timeout =
   TimeoutWinner(objectiveState, sideKills)`; `StepSiegeRound(...)`. `startLives`: each fighter in slot order
   respawns at `PickRingPoint` (decision 8), `sideKills`, `objectiveState` reset. A respawn (`Respawn(slot)`) in a
   siege uses the ring instead of the team spawn.
7. `ObjectiveUpdate`: `objectiveDirty` on `step.changed` or a phase change; also when the progress changed and
   `Tick() - lastObjectiveSend >= 15`. Sent to every joined peer at the end of `StepFight`, beside the roster.
8. The end: when `StepSiegeRound` returns `sessionOver`: send `RoundUpdate`, then `Refuse{EventOver}` to every
   joined peer, disconnect them (the [wp-7] `end_session` path: read it), `sessionOver = true`. A siege always ends
   its session (`endSession` forced true by the parser).
9. Log lines (G26 reads them; exact): `Siege: slot S joins side D (party K8|solo) at (x, z)` (K8 = the key's first
   8 hex digits); `Objective: side D holds the keep at tick T` (on a holder change); `Objective: capture by side D
   complete at tick T`; `Round: side D wins by <objective|time|forfeit> at tick T (siege)` or `Round: draw at tick T
   (siege)`; `Fight:` line gains `, party hits P, leave deaths L` (count `DealDamage` events with source and target on
   one side, and `Leave` deaths).
10. Client: decode `ObjectiveUpdate` (`Client::Objective()` accessor returning the last), the new Refuse reasons
    (`Client::RefusedBy()` with the seconds), sides in the roster (`TeamOf(slot)` returns the side).

**Tests** (`engine/tests/net/SiegeTests.cpp`, the `FightField` pattern with a flat siege region whose zone is the
middle 10 x 4 x 10 m; a helper `SiegeField`)
- `Siege.FourSoloJoinersGetFourSidesAndTheRoundStarts`: 4 bots: sides 1-4; after `countdownTicks` the phase is Live.
- `Siege.APartySharesASide`: bots 0 and 1 with key K, 2 and 3 solo: sides 1, 1, 2, 3.
- `Siege.FriendlyFireInAPartyCountsNoKill`: bot 0 kills bot 1 (same party) in Live: bot 1 deaths 1, bot 0 kills 0,
  the feed's `credit none`.
- `Siege.StandingInTheKeepWins`: one bot walks into the zone and stays (the others stand away): the round ends
  `Objective` with its side after `captureTicks` (+- 2 ticks); the clients' last `ObjectiveUpdate` progress equals
  the target.
- `Siege.LeavingUnderFireIsACreditedDeath`: bot 0 damages bot 1, bot 1 disconnects 60 ticks later: a `KillNotice`
  cause `Leave` credited to bot 0; bot 0's kills 1.
- `Siege.ALeaverIsRefusedReentryFor60Seconds`: bot 1 leaves in Live, reconnects with the same id after 600 ticks:
  `Refuse{Reentry}` with seconds 50; after 3600 ticks: accepted.
- `Siege.AZeroIdIsRefused`, `Siege.TheSameIdTwiceIsRefusedFull`.
- `Siege.TheEventEndsWithEventOver`: a short round (`round_seconds` 5, `end_ticks` 60): every client gets
  `Refuse{EventOver}` and is disconnected; `server.SessionOver()`.
- `Fight.*` all unchanged.

**Checks**: build; `tools/ctest.sh -R 'Siege|Fight\.|FightWire' --output-on-failure`; `tools/ctest.sh`; doc-check;
G22 by hand.

**Must not break**: CLAUDE.md section 5 "Fight": every damage through `DealDamage`; damage only in `Live`; a dead
player not stepped; respawn = teleport with every button held and a new `lifeSeq`. "Checks": hashes equal (the
keep is generator content, so a check never sees it as a difference).

## Card 6: The reset loop

| | |
|---|---|
| Goal | `vortex_server --area` runs event after event on one port; each starts pristine |
| Depends on | card 5 |
| Effort | 1 d (e) |
| Touches | tools/server (main.cpp), net (Server destruction with peers). Wire: no |

**Steps**
1. `--area` (with `--listen`, or with in-process `--clients`): a loop `for (event = 1; ; ++event)`: build
   `net::Server(settings, link)`, `Start()`, run the existing pass loop until `server.SessionOver()`, log `Area:
   event N over (<result>)` (`Round:`'s result words), destroy the server, log `Area: resetting`, then the next;
   `Area: event N+1 open in X.XX s` (time from destruction to `Start()` returning). `--events N` stops after N
   events (exit 0). Ctrl-C stops between ticks as now.
2. The transport outlives the servers: check that `net::Server`'s destructor does not close a listening
   `EnetTransport` (read `Server::~Server` / `Impl::~Impl`); if it does, add a `ServerSettings::keepTransport`
   (default false) and set it in the loop. Peers were all disconnected by the event's end (card 5 step 8).
3. In-process bots across events: after an event the bots are disconnected (`EventOver`); destroy them and make new
   ones for the next event (new loopbacks), same seeds and ids, party keys per `--party-bots`.
4. A peer that connects during the rebuild: its Hello waits in the transport until the new server polls (check that
   ENet keeps it; the test below proves the loopback case).

**Tests**
- `AreaReset.TheNextEventStartsPristine` (`engine/tests/net/SiegeTests.cpp`): a server on a loopback, a bot carves
  3 craters in event 1; the event ends; a new `Server` on the same `LoopbackTransport`: its `RegionHash` equals the
  first server's hash at start, and a new bot joins and plays 60 ticks.

**Checks**: build; `tools/ctest.sh -R 'AreaReset|Siege' --output-on-failure`; `tools/ctest.sh`; doc-check; by hand
`vortex_server --session=engine/tools/server/sessions/hollowkeep.session --area --events 2 --clients 4 --fight
--ticks 80000` (card 7 makes the bots finish an event).

## Card 7: Bots that seek the keep, and gate G26

| | |
|---|---|
| Goal | Four fighting bots, two of them a party, play two full events on the keep; G26 checks them |
| Depends on | cards 5, 6 |
| Effort | 1 d (e) |
| Touches | net (Drill, Bot), tools/server, tools (verify), docs |

**Steps**
1. `PilotSettings::objective` (`bool`) and `objectiveCentre` (`glm::vec3`, set from the Welcome's region:
   `KeepLayoutOf(region).zone` centre): with no target within `fightRange` the pilot walks to the zone's centre
   (the door first: waypoint `(c.x - 8 m, c.z)` until within 2 m of it, then the centre), else it fights as [wp-7].
   Deterministic: no randomness besides the seed.
2. `BotSettings::partyKey`, `playerId` (decision 13), `vortex_server --party-bots K`: bots 0..K-1 share key
   `FNV-1a("bots")`.
3. G26 (tier `full`, no UDP port):

```
build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/hollowkeep.session --area --events 2 \
    --clients 4 --party-bots 2 --fight --ticks 80000 --loss 0.05 --delay-ms 40 --jitter-ms 30
```

   Must: exit 0; `Area: event 1 over`, `Area: event 2 over`, one `Area: event 2 open in X s` with X < 5; per event
   `hashes equal` and four `Entities: client N: ... agree`; no `Checks:` mismatch; at least one of the two events
   ends `Round: side D wins` (not a draw); `Siege: slot ... joins side 1 (party ...)` twice in each event (the party
   shares a side).
4. Judge `gate_G26` in `tools/lib/verify-gates.sh`; fixture `tools/tests/fixtures/verify/G26.log`; planted faults in
   `tools/selftest-verify.sh` (`sed`): drop `Area: event 2 over`; both rounds a draw; a `Checks:` mismatch line.
   CLAUDE.md section 2 gate row; docs/verify.md the flags and lines.

**Numbers**: the event length (ticks), the reset time, the `Fight:` line per event, the server's RSS.

**Checks**: build; `tools/ctest.sh -R 'Tools\.' --output-on-failure`; the G26 command by hand; `tools/doc-check.sh
--build build`; `VERIFY_SLOT=1 tools/verify.sh full` (G26 included).

## Card 8: The game: identity, party, siege HUD

| | |
|---|---|
| Goal | The player joins with an id and a party word, sees sides, the keep's capture and the result, and returns to the world at the event's end |
| Depends on | cards 5, 6 |
| Effort | 1.5 d (e) |
| Touches | game (`src/Areas.cpp`, `src/FightHud.cpp`, `src/Session.cpp`), core (Settings). Wire: no |

**Steps**
1. `GameSettings::party` (string, key `party`); `--party WORD` wins. `player.id` beside the settings file
   (`DefaultSettingsPath().parent_path() / "player.id"`): read 32 hex digits, or make 16 bytes with
   `std::random_device` and write them (not a rule: the server never derives anything from the bytes). The area
   client's `Hello` carries both.
2. Refusals in the prompt (`[area-1]` card 6): `Reentry` -> the server's seconds replace the local timer
   (`HOLLOWKEEP: REENTRY IN 42 S`); `EventOver` while connecting -> `HOLLOWKEEP: EVENT ENDING`; `NoIdentity` ->
   `NO PLAYER ID` (a bug: log it).
3. In the session: `Refuse{EventOver}` (or the disconnect after `Ended`) ends the run with `asked` false; the shell
   returns the player to the local world (no re-entry timer: the event is over).
4. `FightHud` in a siege (decision 15): colours by side and party (the client knows its own side from the roster,
   party members = the roster entries with its side); the banner (`HOLD THE KEEP`, `<HOLDER> 42%` from
   `Client::Objective()`: `progress * 100 / target`, floor; `CONTESTED`); the scoreboard grouped by side, sides by
   summed kills; the end texts. The kill feed shows `LEAVE` for the new cause.
5. A siege run's exit line: `Fight: slot S side D health H kills K deaths L, round <phase>, objective <holder>
   <progress>/<target>`.

**Checks**: build; `tools/ctest.sh`; doc-check; `VERIFY_SLOT=1 tools/verify.sh quick`; G25 by hand (the area is a
siege now: the headless client joins in `Waiting`, walks, leaves: no re-entry, no damage); a headless screenshot in
the session (`--area-test --screenshot` at a frame inside): look at the banner.

**Docs**: `docs/owner-checks.md` **check 24**: "Siege at Hollowkeep with a friend: both set the same party word,
join, you share a colour (green), the banner counts players, a countdown, stand in the keep's hall and watch the
percentage, a third player contests it, friendly fire hurts your friend, the event ends and you are back in the
world; leave mid-fight and see REENTRY IN 60 S". README (game) "Controls": the party word.

## Card 9: Close

1. `VERIFY_SLOT=1 tools/verify.sh full --ci`; `VERIFY_SLOT=1 tools/verify.sh soak` in the background (unrelated soak
   failures go to ROADMAP as known flakes); `tools/build.sh ci` clean.
2. `grep -rn 'DISABLED_' engine/tests tests`: only `Keep.PrintTheGroundUnderTheKeep` (a measuring test, disabled by
   design: say so in its comment).
3. event-areas.md: stage row [area-2] -> **Built**, the measured reset and event numbers into "Memory and cost";
   fight.md: a line under "Stages" that [area-2] added `Siege` and that `deathmatch` / `felling` are test and dev
   modes; ROADMAP "Now" / "Next", the test count; CLAUDE.md section 5 "Fight": the siege rules (friendly fire on in
   `Siege` only, `Leave` deaths, the re-entry refusal) with their pinning tests by name
   (`SiegeRules.FriendlyFireHurtsThePartyButNeverTheCaster`, `Siege.LeavingUnderFireIsACreditedDeath`,
   `Siege.ALeaverIsRefusedReentryFor60Seconds`, `Keep.StampsAreTheSameInAnyChunkOrder`).
4. The report with the numbers of card 7.

## Values this stage lands

- `net::kProtocolVersion` 15. `MessageType` 25 `ObjectiveUpdate` (8 B). `RefuseReason` 7 `Reentry`, 8 `EventOver`,
  9 `NoIdentity`; `Refuse` + `seconds u16`. `Hello` + `playerId[16]`, `partyKey u64` (60 B in all).
  `GameMode` 3 `Siege`; `RoundEnd` 5 `Objective`; `DamageCause` 6 `Leave`; `RegionTemplateId` 2 `Keep`;
  `RegionDesc::siteVoxelY` (+4 B in Welcome); `ObjectiveKind` 0-4; `Roster` team and `RoundUpdate` winner up to 64.
- llm/README "Values planned across the packs": protocol 15 `[area-2]`, `MessageType` 25, gate G26.
- Owner check 24. No `PlayerButton`, `EventKind`, `WorldOpKind`, `EntityKind`; `kPlayerRuleRevision` and
  `buildHash` inputs unchanged.
