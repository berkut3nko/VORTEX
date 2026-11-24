# [area-1] Border and handoff: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/event-areas|event areas]]

| | |
|---|---|
| Stage | `[area-1]`: one castle area (Hollowkeep) is a place in the local world. At its border the HUD asks "Join the siege?"; Enter joins its server session after a short load; walking back to the edge (or the pause menu) leaves it, and the player is back in the local world, which nothing changed meanwhile |
| Design | [event-areas.md](../design/event-areas.md) "Decision" 1-7, 21-24, "The handoff, step by step", "Memory and cost", "Wire" (protocol 14 row) |
| Code read at | engine `9474e9c`, game `9729e09` ([wp-7] committed). Find code with `grep -n`, never by line number |
| Depends on | `[wp-7]` (`PlaySession`, `SessionHost`, `vortex.sim:combat`, `Server::Impl::FightJoin`) |
| Cards | 7: wire + skeleton (0.5 d), `vortex.sim:area` (1 d), the engine's simulation swap (1.5 d), the server's entry spawn and loopback tests (1 d), the game shell and the area client (1.5 d), prompts, leave and the pause menu (1 d), G25 and close (1 d) |
| Effort | 7.5 d (e) |
| Changes | wire: **yes**, `net::kProtocolVersion` -> **14** (card 1 only; the next free number after llm/README's plan: 11 [wp-7], 12 [mech-1], 13 [mag-2]. If `[mech-1]` / `[mag-2]` have not landed, take the next free number at landing and say so under "Deviations"). Movement rule: **no** (`kPlayerRuleRevision` unchanged: the fence is `SetCharacterWalls`, which already exists, and the join spawn is a server teleport). `buildHash`: no new input. Gate: **G25** (tier `full`, UDP P25 = 27045, slot B+5; the next free gate after llm/README's plan, G20-G24) |

What the player gets: walk 400 m east of the plaza to Hollowkeep's curtain (pristine terrain in this stage: the keep
comes with `[area-2]`), read `JOIN THE SIEGE AT HOLLOWKEEP?  ENTER join`, press Enter, load for a few seconds, and
play inside the area's session with whoever is there; at the edge `LEAVE THE SIEGE?  ENTER leave` brings you back to
the local world where you left it.

## Before you start

- `git log --oneline | grep '\[wp-7\]'` shows the stage in the game root and in `engine/`. Missing: stop,
  `Status: blocked`.
- The code this stage builds on (check each with `grep -n`):
  - `engine/src/sim/Region.cppm`: `RegionDesc` (`templateId`, `seed`, `playMinChunk`, `playMaxChunk`, `heightRange`,
    `generator`, `terrain`, `operator==`), `IsValid(const RegionDesc&, std::string*)`, `PlayAreaOf`,
    `PlayBoundsOf`, `MakeRegionGenerator`, `ClientWindowMoves`, `RegionWindowMinChunk`, `RegionWindowSize`.
  - `engine/src/sim/Player.cppm`: `PlayArea{min, max, Bounded()}`, `DistanceToPlayAreaEdge(area, feet)` (negative
    outside), `SetPlayAreaWalls`, `PlayerState`, `kPlayerButtonMask`.
  - `engine/src/physics/PhysicsWorld.cppm`: `SetCharacterWalls(min, max)` - four 2 m thick static boxes in
    `Layer::Bounds` that close the rectangle (corners covered: `half = 0.5 (max - min) + 2 * kHalfThickness`), so a
    character is stopped from outside as from inside. One rectangle at a time (a new call replaces it).
  - `engine/src/sim/Combat.cppm`: `TeamSpawnFeet(simulation, rules, region, team, k, fallback)` (its downward ray is
    the ground search this stage reuses).
  - `engine/src/sim/Rules.cppm`: `ActionRefusal` (`None`, `Cooldown`, `Missed`, `Outside`), `AimAction`,
    `OfflineAimRules`, `ActionAim::sphere` (`voxel::SphereQ`), `ResolveAction`.
  - `engine/src/net/Messages.cppm`: `kProtocolVersion` 11 (or later), `Hello{protocolVersion, buildHash, token}`,
    `Welcome{slot, tick, region, spawn, player, ...}`; `engine/src/net/internal/Messages.cpp`: `Encode(const Hello&)`,
    `Decode(span, Hello&)`.
  - `engine/src/net/Client.cppm`: `ClientSettings` (`token`, `windowChunks`, `minFreeStoreSlots`, `presentation`),
    `Client::MakeSimulationConfig(base, storeLimit)`, `MakeGenerator`, `Region()`, `State()`, `ClientState`.
  - `engine/src/net/internal/ServerPeers.cpp`: `Server::Impl::OnHello` (decode, protocol / build / token / full
    refusals, the slot, the Welcome's spawn); `ServerFight.cpp`: `FightJoin`.
  - `engine/src/engine/Engine.cppm` / `internal/Engine.cpp`: `Engine::Impl` holds `sim::Simulation sim` by value
    (constructed from `MakeSimConfig(config)`), `Initialize(generator)`, `Run(app)` (calls `app.OnInit` each time,
    `exitRequested`, `exitAfterFrames` from `--frames`), `SetTickSource`, `SetPlayBounds`, `~Engine`
    (`device.WaitIdle`, `sim.Shutdown`, ...). `engine/src/voxel/Store.cppm`: `ChunkStore::TakeDirty()`.
  - Game: `src/main.cpp` (the offline `Engine engine(config)`, `ShredborneGame`, `OnFixedUpdate` with
    `sim::StepPlayer(..., sim::PlayArea{}, dt)`, the left click through `sim::AimAction` / `sim::ResolveAction`,
    `engine.Run(game)`, the headless exit lines), `src/Session.cpp` (`RunSession`, `PlaySession`, `SessionGame`,
    its pause menu `"Leave the session"` / `kMenuQuit`, `kGpuStoreSlots` 12288, `kSessionWindowChunks` 48,
    `kSessionStoreHeadroom`), `src/Session.h` (`SessionHost`, `ClientSettingsForSession`).
  - `engine/tools/server/main.cpp`: `--listen`, `--bind`, `--token`, `--exit-when-empty`; the "client left" line
    with the window hash (G15 reads it).

## Decisions

Settled by us (2026-11-24; the owner may veto). The design's decisions are not repeated; these settle what it left
to the stage.

1. **One area, pristine terrain.** `sim::BuiltInAreas()` returns one `AreaDesc`: id 1, name `HOLLOWKEEP`, region:
   template `Empty`, seed 1337, play bounds chunks x [100, 124), z [-12, 12) (metres x 400..496, z -48..48 at
   0.125 m: a chunk is 32 voxels = 4 m), height -48..128, generator `Terrain`, `terrain` = `TerrainSettings{}`
   (seed 1337, 0.125 m) with the default world material ids. The session file
   `engine/tools/server/sessions/hollowkeep.session` says the same (mode `sandbox`); a test pins the equality.
2. **Same coordinates**: no transform anywhere. The offline world and the session world use the same voxel and
   metre coordinates (design decision 2).
3. **The fence** is on while the feet are within 96 m of the area's play bounds (`-DistanceToPlayAreaEdge` <= 96),
   off beyond 128 m. Offline only; it is `engine.Physics().SetCharacterWalls(bounds.min, bounds.max)` with
   `bounds = sim::PlayBoundsOf(area.region)`; off = `SetCharacterWalls(vec2(inf), vec2(-inf))` (non-finite removes).
   The offline game sets no other walls (`sim::PlayArea{}` stays unbounded for `StepPlayer`).
4. **The guard**: an offline cast whose quantised sphere's box (centre +- radius, in voxels; no damage reach: there
   are no other players offline) meets the play bounds' voxel box grown by 1 chunk (4 m) is refused as
   `ActionRefusal::Outside`. The editor is not guarded (a dev tool).
5. **Approach band**: feet outside the play bounds and within 4 m of them: `-4 <= d < 0` with
   `d = sim::DistanceToPlayAreaEdge(sim::PlayBoundsOf(region), feet.xz)`. **Leave band** (inside a session): `d < 1.5`
   with `d` against `PlayBoundsOf` (the feet area stops 0.3 m inside it).
6. **Entry**: the feet x / z in centimetres, `llround(feet * 100)`, sent in `Hello`. The server's spawn: clamp the
   entry into `PlayAreaOf(region, tuning)` shrunk by 2 m per side (metres, doubles), then the ground under it
   (`sim::GroundFeetAt`, the ray of `TeamSpawnFeet`), plus 0.05 m; a miss: the template's spawn. In a fight mode the
   [wp-7] team spawn still wins (fight sessions are not areas in this stage).
7. **Exit**: the session's last feet x / z pushed 3 m outside the nearest edge of the play bounds (the fence boxes
   take [edge - 2 m, edge]; 3 m clears them with the 0.3 m capsule), the ground under it in the local world (a
   downward `World::Raycast` from y = 128 m, plus 0.05 m); a miss: the feet before the join. The local
   `PlayerState` gets velocity 0, flags 0, `lastButtons = kPlayerButtonMask` (no edge from a held key).
8. **Store**: the session's store is capped to the engine's (`engine.Config().chunkCapacity`, 8192 offline) through
   `client.MakeSimulationConfig(base, capacity)`; the renderer's chunk pool is never re-allocated by a swap.
   `ClientSettings::minFreeStoreSlots` stays `kSessionStoreHeadroom` (Attach refuses a world that leaves less).
9. **Swap mechanics**: `Engine::Impl::sim` becomes `std::unique_ptr<sim::Simulation>`; the engine runs whichever it
   holds. The shell (`RunLocalWorld` in `src/main.cpp`) calls `engine.Run(local)`, and on a join request
   `engine.Run(session)` on the same engine, then `engine.Run(local)` again. `Engine::Run` may be called any number
   of times; `--frames N` counts frames across all runs (`Impl::framesTotal`).
10. **Re-entry timer (client side only in this stage)**: a leave while the session's round phase is `Countdown` or
    `Live` starts 60 s; in a sandbox area (this stage's) no timer. The prompt then reads
    `HOLLOWKEEP: REENTRY IN 42 S`. `[area-2]` moves the rule to the server.
11. **Server address**: `--area-server HOST:PORT` and `--area-token HEX` on the command line; the settings file keys
    `area_server` and `area_token` (`GameSettings::areaServer`, `areaToken`; not on the settings page). Without a
    server: the prompt reads `HOLLOWKEEP: NO SERVER SET` and Enter does nothing.
12. **Headless script** `--area-test`: the player starts at the area's test entry (x 397, z 0: 3 m west of the
    bounds), confirms at frame 60, walks +x for 240 frames inside, leaves by the script (the same path as the
    prompt) and walks 60 frames locally; it fails (exit 3) when the local world's hash differs after the return or
    the session's world hash does not show.

## Skeleton

New partition `vortex.sim:area` (`engine/src/sim/Area.cppm`, implementation `engine/src/sim/internal/Area.cpp`),
exported from `engine/src/sim/Sim.cppm` (`export import :area;`) and listed in `engine/CMakeLists.txt` beside
`Combat.cppm` (find the line with `grep -n "Combat.cppm" engine/CMakeLists.txt`).

```cpp
module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

export module vortex.sim:area;

import vortex.voxel;
import :region;
import :player;
import :simulation;

// docs/design/event-areas.md: castle event areas in the local world. [area-1]
export namespace vortex::sim {
    /// @brief One event area: a box of the local world whose session the game can join.
    struct AreaDesc {
        uint8_t id = 0;
        std::string_view name;               ///< upper case, for the HUD ("HOLLOWKEEP")
        RegionDesc region;                   ///< what the area's server must Welcome with (operator==)
        glm::vec2 testEntry{0.0f};           ///< --area-test's start (x, z metres), 3 m outside the bounds
    };
    /// @brief The game's areas (decision 1): fixed data; `ids` = the default world material ids.
    std::span<const AreaDesc> BuiltInAreas(const voxel::WorldMaterials& ids);

    inline constexpr float kAreaApproachMetres = 4.0f;   ///< the join prompt's band outside the bounds
    inline constexpr float kAreaLeaveMetres = 1.5f;      ///< the leave prompt's band inside the bounds
    inline constexpr float kAreaFenceOnMetres = 96.0f;
    inline constexpr float kAreaFenceOffMetres = 128.0f;
    inline constexpr float kAreaExitMetres = 3.0f;       ///< the local feet after a leave, outside the nearest edge
    inline constexpr double kAreaEntryInsetMetres = 2.0; ///< the join spawn, inside the feet area
    inline constexpr uint32_t kReentryTicks = 3600;      ///< 60 s

    /// @brief The area whose play bounds are nearest to `feet` (x, z), and the signed distance to them
    /// (DistanceToPlayAreaEdge: negative outside). nullptr for an empty list.
    const AreaDesc* NearestArea(std::span<const AreaDesc> areas, const glm::vec2& feet, float& distance);
    /// @brief Feet in the join prompt's band: outside the bounds and at most kAreaApproachMetres from them.
    bool InApproachBand(const AreaDesc& area, const glm::vec2& feet);
    /// @brief Feet in the leave prompt's band (inside a session).
    bool InLeaveBand(const RegionDesc& region, const glm::vec2& feet);
    /// @brief The entry sent in Hello: llround(x * 100), llround(z * 100).
    glm::ivec2 EntryCentimetres(const glm::vec2& feet);
    /// @brief Decision 6: the clamped entry (x, z metres) inside PlayAreaOf shrunk by kAreaEntryInsetMetres.
    glm::dvec2 ClampEntry(const RegionDesc& region, const PlayerTuning& tuning, const glm::ivec2& entryCm);
    /// @brief Decision 7: x, z of the local feet after a leave from session feet `feet`.
    glm::dvec2 ExitPoint(const RegionDesc& region, const glm::vec2& feet);
    /// @brief The ground under (x, z) in `simulation`'s world: a downward World::Raycast from the region's height
    /// maximum, feet = hit + 0.05 m; `fallback` on a miss. The ray of TeamSpawnFeet, shared with it.
    glm::dvec3 GroundFeetAt(Simulation& simulation, double topY, const glm::dvec2& xz, const glm::dvec3& fallback);
    /// @brief Decision 4: true when `sphere` may change voxels of an area (the play bounds grown by one chunk).
    bool AreaGuard(std::span<const AreaDesc> areas, const voxel::SphereQ& sphere, float voxelSize);
}
```

Wire (`engine/src/net/Messages.cppm`), card 1:

```cpp
    inline constexpr uint16_t kProtocolVersion = 14;   // [area-1] Hello::entry (event-areas.md "Wire")

    struct Hello {
        uint16_t protocolVersion = kProtocolVersion;
        uint64_t buildHash = 0;
        Token token{};
        /// [area-1] Where the player walked in (feet, centimetres); `hasEntry` false = the template's spawn.
        bool hasEntry = false;
        glm::ivec2 entryCm{0};
    };
```

Engine (`engine/src/engine/Engine.cppm`, class `Engine`), card 3:

```cpp
        /// [area-1] Hands the current simulation to the caller (device idle, world jobs drained, not shut down);
        /// the engine holds none until StartSimulation or AdoptSimulation. Not while Run is running.
        std::unique_ptr<sim::Simulation> ParkSimulation();
        /// [area-1] Shuts the current simulation down (device idle first); the engine holds none.
        void StopSimulation();
        /// [area-1] A new simulation from `config`, initialised with `generator`; every chunk it loads is uploaded
        /// as usual. `windowFollowsCamera` replaces EngineConfig's. false when config.chunkCapacity exceeds
        /// the engine's (the renderer's pool is never re-allocated) or the simulation fails to start (logged).
        bool StartSimulation(const sim::SimulationConfig& config, std::shared_ptr<voxel::IWorldGenerator> generator,
                             bool windowFollowsCamera);
        /// [area-1] Takes back a parked simulation: every used chunk is uploaded again (ChunkStore::MarkAllDirty).
        void AdoptSimulation(std::unique_ptr<sim::Simulation> simulation, bool windowFollowsCamera);
        /// [area-1] Frames run by every Run so far (`--frames N` stops at N across runs).
        uint32_t FramesRun() const;
```

`engine/src/voxel/Store.cppm`, class `ChunkStore`:

```cpp
        /// [area-1] Marks every used slot dirty (a renderer whose pool held another store's chunks uploads them all).
        void MarkAllDirty();
```

Game files: new `src/Areas.cpp` and `src/Areas.h` (the area client and prompts; `Areas.h` follows `Session.h`'s
"not self-contained" rule: included after the imports and `using namespace vortex;`), added to the game's
`CMakeLists.txt` beside `src/Practice.cpp`.

Tests (new files; the CMake globs pick them up): `engine/tests/sim/AreaTests.cpp` (suite `Area`),
`engine/tests/net/AreaTests.cpp` (suites `AreaWire`, `AreaHandoff`, `AreaSession`). A test helper never shares a
name with an engine type (no helper called `Area` or `AreaDesc`).

## Card 1: Wire 14 and the skeleton

| | |
|---|---|
| Goal | `Hello` carries the entry; `vortex.sim:area` exists with stubs; the stage's tests exist, disabled |
| Depends on | - |
| Effort | 0.5 d (e) |
| Touches | net (Messages), sim (new partition). **Wire: `kProtocolVersion` -> 14 here.** Movement rule: no. buildHash: through the protocol only |

**Read first** (about 30 KiB)
- `engine/src/net/Messages.cppm` `Hello`, `kProtocolVersion`; `engine/src/net/internal/Messages.cpp` `Encode(const
  Hello&)`, `Decode(std::span<const uint8_t>, Hello&)`, `ByteWriter` (`U8`, `U16`, `U32`, `U64`, `Bytes`) and
  `ByteReader`.
- `engine/tests/net/WireTests.cpp` (how sizes and refusals are pinned); `engine/tests/net/FightTests.cpp`
  `RefusesTrailingAndMissing`, `DecodesWith`.
- `engine/docs/notes/net.md` section 2 (the registry).

**Steps**
1. `Hello`: add `hasEntry`, `entryCm` (skeleton). Encode after `token`: `U8(hasEntry ? 1 : 0)`,
   `U32(static_cast<uint32_t>(entryCm.x))`, `U32(static_cast<uint32_t>(entryCm.y))` (two's complement, as the other
   signed fields: `grep -n "int32_t" engine/src/net/internal/Messages.cpp` and follow the existing pattern).
   Decode: the flags byte must be 0 or 1 (else false); with 0 the two values must be 0 (else false). Size: 1 (type) + 2 +
   8 + 16 + 9 = **36 B** (check against the current encoder: count, do not trust this sum;
   write the measured size in the test).
2. **Trap**: a Hello of another protocol must still be refused `Protocol`, not `Malformed`. Read `OnHello`: if it
   decodes the whole message before comparing the version, change it to peek the version first (`bytes[1..2]`
   after the type) and refuse `Protocol` when it differs, before the full decode.
3. `kProtocolVersion` = 14 with a comment naming `[area-1]`.
4. Write `Area.cppm` from the skeleton with stub bodies (`// [area-1] card 2 fills this`: return nullptr, false,
   zero vectors, `fallback`, an empty span). Add it to `Sim.cppm` and `engine/CMakeLists.txt`.
5. Write every test of the stage (cards 2, 4) in full, each named `DISABLED_<Name>`; the card that makes one pass
   renames it.

**Tests** (written now, enabled here)
- `AreaWire.HelloCarriesTheEntry` (`engine/tests/net/AreaTests.cpp`): encode `Hello{.hasEntry = true, .entryCm =
  {-40050, 1234}}`, decode, equal; the size is the measured one; `RefusesTrailingAndMissing`; flags 2 refused;
  flags 0 with a non-zero x refused.
- `AreaWire.AnOldProtocolIsRefusedProtocol`: a Hello of protocol 13 (hand-built bytes, 27 B) to a loopback server
  gets `Refuse{Protocol}`.

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning
2. `tools/ctest.sh -R 'AreaWire\.|Wire\.|FightWire\.' --output-on-failure` - all pass
3. `tools/ctest.sh` - all pass
4. `tools/doc-check.sh --build build` - PASS

**Docs**
- `engine/docs/notes/net.md` section 2: protocol 14, `Hello` + 9 B.
- `CLAUDE.md` section 5 "Sessions": add `Hello::entry` to the wire list.

**Must not break**
- G11-G15, G22 (their clients send `hasEntry` false: the template / team spawn as before).

**Done when**
- [ ] `kProtocolVersion` is 14; Hello round-trips with and without an entry
- [ ] every new test exists, the wire ones enabled and passing

**Traps**
- Every binary of a run must be rebuilt together (bots, server, game): a stale `vortex_server` refuses the game.

## Card 2: `vortex.sim:area`

| | |
|---|---|
| Goal | The area geometry, the guard and the built-in area, GPU-free and tested |
| Depends on | card 1 |
| Effort | 1 d (e) |
| Touches | sim. Wire: no. Movement rule: no |

**Read first** (about 25 KiB)
- `engine/src/sim/internal/Region.cpp`: `PlayBoundsOf`, `PlayAreaOf` (doubles then floats), `IsValid`.
- `engine/src/sim/internal/Combat.cpp`: `TeamSpawnFeet` (the ray), `engine/src/sim/internal/Player.cpp`:
  `DistanceToPlayAreaEdge`.
- `engine/src/voxel/Sphere.cppm` or wherever `SphereQ` is (`grep -rn "struct SphereQ" engine/src/voxel`):
  `centreQ` (1/16 voxel), `radiusQ`.

**Steps**
1. `BuiltInAreas(ids)`: a function-local `static std::array<AreaDesc, 1>` filled on first call under a
   `std::once_flag` (the ids are the same in every call of one process; assert it with a comparison of the first
   call's ids, `Log::Error` and keep the first otherwise). Region as decision 1: `templateId = Empty`,
   `seed = 1337`, `playMinChunk = {100, -12}`, `playMaxChunk = {124, 12}`, `heightRange = {-48, 128}`,
   `generator = Terrain`, `terrain = voxel::TerrainSettings{}` with `terrain.materials = ids`. `testEntry =
   {397, 0}`. `sim::IsValid(region)` must be true (a test).
2. `NearestArea`: for each area, `d = DistanceToPlayAreaEdge(PlayBoundsOf(region), feet)`; the nearest is the one
   with the largest `d` (least outside, or inside); ties: the lower index. Write `distance = d`.
3. `InApproachBand`: `d = DistanceToPlayAreaEdge(PlayBoundsOf(area.region), feet)`; `-kAreaApproachMetres <= d && d < 0`.
4. `InLeaveBand`: `0 <= d && d < kAreaLeaveMetres` with the same `d` (a session's feet never go below 0.3).
5. `EntryCentimetres`: `{llround(double(feet.x) * 100), llround(double(feet.y) * 100)}` clamped to +-2^30.
6. `ClampEntry`: `a = PlayAreaOf(region, tuning)`; `lo = dvec2(a.min) + 2.0`, `hi = dvec2(a.max) - 2.0`;
   `p = dvec2(entryCm) / 100.0`; return `clamp(p, lo, hi)` per axis (if `lo > hi` on an axis, the middle).
7. `ExitPoint`: `b = PlayBoundsOf(region)`; distances to the four edges `feet.x - b.min.x`, `b.max.x - feet.x`,
   `feet.y - b.min.y`, `b.max.y - feet.y`; the smallest (ties in that order: -x, +x, -z, +z) names the edge; the
   result is the feet with that coordinate set to `edge -+ kAreaExitMetres` (outside), the other kept and clamped to
   the bounds.
8. `GroundFeetAt`: move `TeamSpawnFeet`'s ray into this function (origin `(x, topY, z)`, direction `(0, -1, 0)`,
   length `topY - heightMin`... copy its exact arguments) and make `TeamSpawnFeet` call it; G22 must not change.
9. `AreaGuard`: for each area, the voxel box of its play bounds: `min = playMinChunk * 32 - 32`, `max =
   playMaxChunk * 32 + 32` (x, z; one chunk = 32 voxels of margin); the sphere's box: `c = centreQ >> 4` (floor, as
   `InPlayBounds` does: read it), `r = (radiusQ + 15) >> 4` (ceil); overlap in x and z (`c.x + r >= min.x && c.x - r
   < max.x`, same for z) -> true. y is not checked.
10. Offline cast hook (game, card 5 wires it): after `AimAction` returns `None`, `if (sim::AreaGuard(areas,
    aim.sphere, voxelSize))` the cast is dropped and `Cast not run: OUTSIDE` shows as for any refusal.

**Tests** (`engine/tests/sim/AreaTests.cpp`)
- `Area.TheBuiltInAreaIsAValidRegion`: `IsValid` true; bounds metres x [400, 496), z [-48, 48) (`PlayBoundsOf`).
- `Area.TheApproachBandIsFourMetresOutside`: feet (396.1, 0) in, (395.9, 0) out, (400.1, 0) out (inside), (397, 47)
  in, corner (398, -49) in (`DistanceToPlayAreaEdge` returns the smallest of the four signed distances: -2 here;
  check its code and assert that value).
- `Area.TheLeaveBandIsOneAndAHalfMetresInside`: (401.4, 0) in, (401.6, 0) out.
- `Area.EntryIsClampedTwoMetresInsideTheFeetArea`: entry (39700, 0) cm -> x = 400 + 0.3 + 2 = 402.3 (PlayAreaOf
  shrinks by the radius 0.3); (45000, -9000) -> (450, -48 + 2.3 = -45.7).
- `Area.TheExitIsThreeMetresOutsideTheNearestEdge`: (401, 5) -> (397, 5); (450, 47) -> (450, 51); (495.9, -47.9):
  +x and -z are both 0.1 away, a tie: +x comes first in the order -x, +x, -z, +z, so (499, -47.9) (x = 496 + 3).
- `Area.TheGuardCoversTheBoundsAndOneChunk`: 0.125 m voxels: a sphere at x = 396.5 m (voxel 3172) radius 1 m is
  guarded (bounds - 4 m = 396 m); at x = 390 m radius 1 m is not.
- `Area.NearestPicksTheLeastOutside`: two hand-made areas; ties pick the lower index.

**Checks**: build; `tools/ctest.sh -R 'Area\.|Combat\.|Fight\.' --output-on-failure`; `tools/ctest.sh`; doc-check.

**Docs**: `event-areas.md` stays the reference; nothing else.

**Must not break**: G22 (`TeamSpawnFeet` through `GroundFeetAt`: the same feet bit for bit -
`Fight.TeamsAreBalancedAtJoin` and the 2v2 match pass unchanged).

**Done when**
- [ ] `Area.*` pass; `TeamSpawnFeet` calls `GroundFeetAt`

**Traps**
- `PlayBoundsOf` is computed in doubles then rounded to floats; compare test values with `EXPECT_NEAR(..., 1e-4)`.

## Card 3: The engine's simulation swap

| | |
|---|---|
| Goal | One `Engine` runs the local world, then a session's world, then the local one again, without a new window |
| Depends on | card 1 |
| Effort | 1.5 d (e) |
| Touches | engine, voxel (Store), graphics (only if the renderer keeps per-world state: step 4). Wire: no |

**Read first** (about 60 KiB)
- `engine/src/engine/internal/Engine.cpp`: `struct Engine::Impl` (members in order: `config`, `input`, `sim`,
  `window`, `device`, `renderer`, ...; the comment "Declared before the renderer, which draws from its chunk
  store"), `MakeSimConfig`, `Engine::~Engine`, `Initialize`, `Run` (the start: `sim::Simulation& sim = s.sim;`, the
  frame loop, `exitAfterFrames`, the stream-fill log lines, the end-of-run `Perf:` lines), `Explode`, `PresentBlast`,
  every `s.sim.` use (`grep -n "s\.sim\.\|m_Impl->sim" engine/src/engine/internal/*.cpp`).
- `engine/src/voxel/Store.cppm` `ChunkStore` (`TakeDirty`, `UsedCount`, `Capacity`), its implementation.
- `engine/src/graphics/Renderer.cppm` `Renderer::Initialize(device, window, w, h, chunkCapacity, settings)`,
  `RenderFrame(const FrameInput&)`; in `engine/src/graphics/internal/Renderer.cpp` look for anything kept between
  frames that depends on the world (`grep -n "history\|prevOrigin\|windowMin\|origin" ...Renderer.cpp`).

**Steps**
1. `Impl::sim` -> `std::unique_ptr<sim::Simulation>`, made in the `Impl` constructor as before
   (`std::make_unique<sim::Simulation>(MakeSimConfig(config))`). Replace every `s.sim.` with `s.sim->` (mechanical).
   Keep the member's place (before `renderer`): destruction order unchanged. Add `Impl::framesTotal` (u32) and
   `Impl::followsCamera` (bool, from `config.windowFollowsCamera`); use `followsCamera` where `Run` reads
   `config.windowFollowsCamera`.
2. `Run`: at its start `if (!s.sim) { Log::Error("Engine: Run without a simulation"); return 1; }`;
   `s.exitRequested = false;` (a second run must not end at once). The frame limit: replace the per-run frame
   count test with `s.framesTotal` (+1 per frame) against `exitAfterFrames`. `FramesRun()` returns it.
3. `ParkSimulation`: `s.device.WaitIdle()` (when initialised); drain the world's jobs: find how
   `Simulation::Shutdown` / `World::Shutdown` waits for its jobs (`grep -n "Wait\|jobsInFlight" engine/src/voxel/internal/World.cpp`)
   and, if no public wait exists, add `void World::WaitForJobs()` that does the same wait without shutting down;
   call it; then `return std::move(s.sim);` (s.sim is null afterwards). Log `Engine: simulation parked (N chunks
   used)`.
4. `StopSimulation`: `WaitIdle`, `s.sim->Shutdown()`, `s.sim.reset()`.
5. `StartSimulation(config, generator, follows)`: refuse (`Log::Error`, false) when `config.chunkCapacity >
   s.config.chunkCapacity`; `s.sim = std::make_unique<sim::Simulation>(config)`; `Initialize(generator)` (false:
   reset and return false); `s.followsCamera = follows`; reset what the renderer keeps per world (step 7); true.
   Thread counts: apply `s.config.jobThreads` / `physicsThreads` as `MakeSimConfig` does for a session config.
6. `AdoptSimulation(sim, follows)`: `if (s.sim) StopSimulation();` `s.sim = std::move(sim)`;
   `s.sim->Chunks().MarkAllDirty()`; `s.followsCamera = follows`; reset the renderer's per-world state (step 7).
7. Renderer: the chunk pool keeps its size (capacity unchanged). If step "Read first" found state that remembers
   the previous frame's world or camera (temporal history, the previous render origin), add
   `void Renderer::ResetHistory()` that drops it the way a camera cut does (if a cut path exists, call it; if
   none exists, set the TAA history weight to 0 for the next frame) and call it from steps 5 and 6. If nothing
   such exists, write that in the report and add nothing.
8. `ChunkStore::MarkAllDirty()`: every slot that holds a chunk goes into the dirty list once (use the store's own
   dirty bookkeeping: read how a written slot becomes dirty).
9. `~Engine`: `if (s.sim) s.sim->Shutdown();` (a parked simulation handed out is the caller's to shut down).

**Tests** (VortexEngineTests need a GPU only for rendering; these do not render)
- `ChunkStore.MarkAllDirtyListsEveryUsedSlotOnce` (`engine/tests/core/` beside the store's tests: `grep -rln
  "ChunkStore" engine/tests/core`): allocate 3 slots, `TakeDirty` (drain), `MarkAllDirty`, `TakeDirty` has the 3,
  sorted, once each; a second `TakeDirty` is empty.

**Checks**
1. build; `tools/ctest.sh -R 'ChunkStore\.|Store\.' --output-on-failure`; `tools/ctest.sh`; doc-check.
2. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS (G3-G10 run on the changed engine; nothing may move). Look at the
   screenshots it lists.

**Docs**
- `engine/docs/ARCHITECTURE.md`: one paragraph where the engine's ownership of the simulation is described
  (`grep -n "one sim::Simulation\|owns one" engine/docs/ARCHITECTURE.md`): "holds one simulation at a time; a
  parked one belongs to the game (event-areas.md)". CLAUDE.md section 4 says "`Vortex::Engine` (engine, editor; owns
  one `sim::Simulation` and draws it)": change to "holds one `sim::Simulation` at a time".

**Must not break**
- G3-G10, G21 (the offline run is the same run with a pointer); L8 (a parked world is not stepped: its bodies stay
  as they are, frozen in time, and nothing simulates over unloaded terrain).

**Done when**
- [ ] `Engine::Run` twice in a row works (card 5 proves it end to end); verify quick PASS

**Traps**
- `Run` caches `sim::Simulation& sim = s.sim;` at its top: after step 1 it is `*s.sim`, valid for that run only;
  never swap inside a run.
- `Engine::Simulation()` and the shortcuts (`World()`, `Physics()`, `Scene()`, `Chunks()`) dereference `s.sim`:
  the game must not call them between `ParkSimulation` and `Start` / `Adopt` (the shell does not).

## Card 4: The server's entry spawn and loopback tests

| | |
|---|---|
| Goal | A joiner with an entry spawns at it; a client refuses a Welcome of another region; a leaver vanishes for the others |
| Depends on | cards 1, 2 |
| Effort | 1 d (e) |
| Touches | net (ServerPeers, Client). Wire: as card 1 |

**Read first** (about 45 KiB)
- `engine/src/net/internal/ServerPeers.cpp` `OnHello` (where `welcome.spawn` is set, the fight path
  `JoinTeam` / `TeamSpawnFeet`), `DropPlayer`.
- `engine/src/net/internal/Client.cpp` `OnWelcome`, `Leave`; `engine/src/net/Client.cppm` `ClientSettings`.
- `engine/tests/net/MovementTests.cpp` (the `Match` harness) or `FightTests.cpp` `FightField` (loopback server +
  clients on one manual clock): copy the pattern, with a new helper name (`AreaField`).

**Steps**
1. `ClientSettings`: add `bool hasEntry = false; glm::ivec2 entryCm{0};` and `std::optional<sim::RegionDesc>
   expectRegion;` (`[area-1]`). The client's Hello copies the entry.
2. `Client::Impl::OnWelcome`: with `expectRegion` set and `!(welcome.region == *expectRegion)`: log
   `Client: the server's region differs from the expected one`, state `Failed`, leave (disconnect). Nothing built.
3. `Server::Impl::OnHello`: after the slot is taken and before the Welcome is sent: if `hello.hasEntry` and the
   session is not a fight (`!Fighting()`), `xz = sim::ClampEntry(region, settings.player, hello.entryCm)` and
   `spawn = sim::GroundFeetAt(*simulation, region.heightRange.y, xz, tmpl.spawn)`; use it as the Welcome's spawn and
   the player's start (exactly where the template's spawn is used now). Log `Server: slot S enters at (x, z) ->
   feet (x, y, z)` (`{:.2f}`).
4. Fight sessions keep the team spawn (`[area-2]` changes this for `Siege`).

**Tests** (`engine/tests/net/AreaTests.cpp`)
- `AreaHandoff.AJoinerSpawnsAtItsEntry`: a flat-region server (`FightTests`' `FlatRegion()` pattern: play bounds
  chunks [-4, 4) at 0.125 m: metres [-16, 16)), sandbox; a client with entry (-1700, 0) cm (x -17 m, outside):
  its Welcome spawn x = -16 + 0.3 + 2 = -13.7 m (+- 0.01), z 0, y = the flat ground top + 0.05.
- `AreaHandoff.WithoutAnEntryTheTemplateSpawnIsUsed`: Welcome spawn equals `server.Template().spawn` as today.
- `AreaHandoff.ALeaverVanishesForTheOthers`: two clients play 60 ticks; client 0 leaves (`Client::Leave` or the
  transport's disconnect, as the game does); within 30 ticks client 1's remote players do not hold slot 0.
- `AreaHandoff.TheClientRefusesAnotherRegion`: `expectRegion` = the flat region with `seed + 1`: state `Failed`, no
  simulation attached.
- `AreaSession.TheBuiltInAreaMatchesItsSessionFile`: `net::LoadSessionFile(VORTEX_TEST_SOURCE_DIR
  "/../tools/server/sessions/hollowkeep.session")` (check the path: `VORTEX_TEST_SOURCE_DIR` is `engine/tests/..`),
  its `region == sim::BuiltInAreas(ids)[0].region` with `ids` the default material ids the parser uses (read how
  `ParseSession` fills `terrain.materials`); the mode is sandbox.
- New file `engine/tools/server/sessions/hollowkeep.session`:

```
# [area-1] Hollowkeep, the first event area (docs/design/event-areas.md): pristine terrain, sandbox.
#   vortex_server --session hollowkeep.session --listen P --bind 127.0.0.1 --token T --exit-when-empty (gate G25)
token = 5b8e2d4c1a9f30762e4d5c6b7a8f9e01
template = empty
seed = 1337
play_min = 100 -12
play_max = 124 12
height = -48 128
voxel_size = 0.125
generator = terrain
terrain_seed = 1337
max_clients = 16
chunk_capacity = 0
pack_hidden = 1
```

  (Check every key against `ParseSession`; `template = empty` must be the parser's word for `Empty`: `grep -n
  "playground\|empty" engine/src/net/internal/Session.cpp`.)

**Checks**: build; `tools/ctest.sh -R 'Area|Session\.|Fight\.' --output-on-failure`; `tools/ctest.sh`; doc-check;
by hand: `build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/hollowkeep.session --ticks=60`
exits 0 and logs `Server: region of ... loaded in X s` (write X in the report: the area's preload).

**Docs**: `docs/verify.md` (game): nothing yet (card 7). `engine/docs/notes/net.md`: the client's region check.

**Must not break**: `Session.ClientBuildsItsWorldFromWelcomeAndRefusesAnyOther`, G11-G15.

**Done when**
- [ ] the five tests pass; the session file loads

**Traps**
- The spawn's ground needs the server's world loaded there: the server's window is loaded at start (it is), so the
  ray hits; a flat region's ground is `flatGroundVoxels * voxelSize`.

## Card 5: The game shell and the area client

| | |
|---|---|
| Goal | `Shredborne` (offline) can join Hollowkeep's server and come back, on one engine |
| Depends on | cards 2-4 |
| Effort | 1.5 d (e) |
| Touches | game (`src/main.cpp`, `src/Session.cpp`, `src/Session.h`, new `src/Areas.cpp/.h`), core (Settings). Wire: no |

**Read first** (about 80 KiB; read sections, never whole files)
- `src/main.cpp`: `ShredborneGame` (`OnInit`, `OnFixedUpdate`, `OnUpdate`, `m_Feet`, `m_Player`, `m_Character`,
  `m_Tuning`, the left-click block, `DrawHud`), the end of `main` (`Engine engine(config)` to `return result;`).
- `src/Session.cpp`: `PlaySession` (all of it), `SessionGame` (constructor, `OnUpdate`'s pause menu handling,
  `kMenuQuit`, `Lost()`, `Attached()`), `RunSession` (how the ENet transport and the client are made:
  `net::EnetTransport::Connect(host, port, settings, &why)`).
- `engine/src/core/Settings.cppm`: `GameSettings`, `ParseGameSettings`, `FormatGameSettings`.

**Interfaces** (`src/Areas.h`)

```cpp
/// [area-1] The local world's side of the event areas (engine/docs/design/event-areas.md "The handoff").
enum class AreaJoin : uint8_t { None, Prompt, Connecting, Welcomed, Failed };

/// [area-1] One join attempt: the ENet link and the client, owned by the shell while the session runs.
struct AreaLink {
    const sim::AreaDesc* area = nullptr;
    std::unique_ptr<net::EnetTransport> transport;
    std::unique_ptr<net::Client> client;
    double startedSeconds = 0.0;
};

/// [area-1] Per frame of the local game: the fence, the prompt state, Enter, the connection while Connecting.
class AreaApproach {
public:
    AreaApproach(std::string server, net::Token token);
    /// Fence on / off, band, prompt; on Enter starts a link. Returns Welcomed once the link's client is Welcomed.
    AreaJoin Update(Engine& engine, const glm::vec2& feet, bool enterPressed, double nowSeconds);
    void Draw(HudCanvas& hud, glm::vec2 screen, int scale) const;
    /// The Welcomed link (moves it out; the approach goes back to None).
    AreaLink TakeLink();
    /// A leave from a Countdown / Live round: the re-entry timer for `area` (decision 10).
    void StartReentry(uint8_t areaId, double nowSeconds);
    std::span<const sim::AreaDesc> Areas() const;
private:
    // ...
};
```

`src/Session.h`: split `PlaySession` so its body after the `Engine engine(config); ... engine.Initialize(...)`
part is reusable:

```cpp
/// [area-1] The session game on an existing engine whose simulation the caller started from the client
/// (StartSimulation). Returns as PlaySession; `leaveAsked` / `feet`: how it ended and where the player stood.
int RunSessionOn(Engine& engine, const CommandLine& args, const GameSettings& settings, const std::filesystem::path& settingsPath,
                 net::Client& client, net::ITransport& transport, const std::string& server, const SessionHost& sh,
                 bool* newRound, bool* leaveAsked, glm::vec3* lastFeet, sim::RoundPhase* lastPhase);
```

`PlaySession` keeps its behaviour by building its engine and calling `RunSessionOn`.

**Steps**
1. `GameSettings`: `std::string areaServer, areaToken;` keys `area_server`, `area_token` (strings, may be empty;
   `FormatGameSettings` writes them after `play_bounds`). A test in the existing settings suite:
   `Settings.AreaKeysRoundTrip` (`grep -rn "ParseGameSettings" engine/tests`).
2. `AreaApproach::Update` (each local frame, after the player step):
   1. `area = sim::NearestArea(areas, feet, d)`; fence: if `-d <= 96` and not on -> `engine.Physics().SetCharacterWalls(b.min, b.max)`
      (`b = PlayBoundsOf(area->region)`), `engine.SetPlayBounds(b)`; if `-d > 128` and on -> off (non-finite
      walls, `SetPlayBounds(std::nullopt)`).
   2. `None`/`Prompt`: `Prompt` while `InApproachBand`, else `None`. On `enterPressed` in `Prompt`: no server ->
      message `NO SERVER SET` for 3 s; re-entry timer running -> nothing; else connect: parse `HOST:PORT`,
      `net::EnetTransport::Connect` (as `RunSession` does), `net::Client` with `ClientSettingsForSession().settings`,
      `token`, `hasEntry = true`, `entryCm = sim::EntryCentimetres(feet)`, `expectRegion = area->region`; log
      `Area: joining HOLLOWKEEP at HOST:PORT, entry (x, z)`; state `Connecting`.
   3. `Connecting`: `client->Poll(); transport->Flush();` each frame (the transport is main-thread only); Welcomed ->
      `Welcomed`; `Failed` or a refusal -> `Failed` with the refusal's text for 3 s (`Area: join failed: <reason>`),
      the link dropped; 10 s without a Welcome -> `Failed` (`NO ANSWER`). Walking out of the band while
      `Connecting` cancels (disconnect, `Area: join cancelled`).
3. `ShredborneGame`: owns an `AreaApproach` (made from settings / flags, `--area-server`, `--area-token` win);
   calls `Update` in `OnUpdate` with `in.KeyPressed(Key::Enter)` while playing (not in a menu); on `Welcomed` it
   records `m_HandoffAsked = true` and calls `engine.RequestExit()`. The left click: after `AimAction`, the guard of
   card 2 step 10.
4. `ShredborneGame::OnInit`: when called a second time (`m_Started` true) it does not reload or move anything: it
   only re-sets the tick source to none (`engine.SetTickSource({})`), the fence and the play bounds as `Update`
   decides, and `engine.SetPlayBounds`. Add `void ResumeAt(const glm::dvec3& feet)` that puts the character there
   (`engine.Physics().SetCharacterState(m_Character, feet, vec3(0))`, `m_Player.position = feet`, velocity 0,
   flags 0, `lastButtons = sim::kPlayerButtonMask`, the camera at the eye).
5. The shell in `main` (replaces `const int result = engine.Run(game);`):

```
loop:
  result = engine.Run(game)
  if !game.HandoffAsked(): break                      // a normal exit
  link = game.Approach().TakeLink(); game.ClearHandoff()
  before = voxel::WorldHash(engine.World(), windowMin(area), windowMax(area))    // the area's server window cells
  feetBefore = game.Feet()
  t0 = now
  parked = engine.ParkSimulation()
  log "Area: parked world N chunks, RSS X MiB"        // N = parked->Chunks().UsedCount(), net::ResidentMiB()
  base = the SimulationConfig PlaySession builds (base.chunkCapacity = engine.Config().chunkCapacity, ...)
  ok = engine.StartSimulation(link.client->MakeSimulationConfig(base, engine.Config().chunkCapacity),
                              link.client->MakeGenerator(),
                              sim::ClientWindowMoves(link.client->Region(), kSessionWindowChunks))
     (service the link from a helper thread meanwhile, exactly as PlaySession's keepAlive)
  if !ok: log "Area: the session world did not start"; engine.AdoptSimulation(parked, true); game.ResumeAt(feetBefore); continue
  log "Area: joined HOLLOWKEEP in X.XX s (slot S, spawn (x, y, z))"
  r = RunSessionOn(engine, ..., *link.client, *link.transport, server, SessionHost{rtt, {}, true, false}, nullptr,
                   &leaveAsked, &lastFeet, &lastPhase)
  link.client->Leave() (or the transport's disconnect, as SessionGame's kMenuQuit does); flush
  t1 = now
  engine.StopSimulation(); engine.AdoptSimulation(std::move(parked), true)
  after = voxel::WorldHash(engine.World(), same box)
  log "Area: left HOLLOWKEEP after N ticks (asked|ended|lost), back at (x, y, z)"
  log "Area: local world back in X.XX s, hash H1 before, H2 after - same|DIFFERENT"
  exit = sim::ExitPoint(area.region, lastFeet.xz); feet = sim::GroundFeetAt(engine.Simulation(), 128, exit, feetBefore)
  game.ResumeAt(feet); if lastPhase is Countdown or Live: game.Approach().StartReentry(area.id, now)
  if headless and frames are used up: break
```

   `windowMin(area)` / `windowMax(area)` = `sim::RegionWindowMinChunk(region)` and `+ RegionWindowSize(region)`,
   with y the offline world's rows (the offline window is full height too: use `engine.World().WindowMinChunk().y`
   and `WindowSize().y`). The parked world is not touched, so a difference means a bug: log it, and in a headless run
   add a failed expectation (exit 3).
6. `RunSessionOn`: the part of `PlaySession` after `engine.Initialize(...)`; it sets the tick source, runs the
   `SessionGame`, and before returning `engine.SetTickSource({})`. `SessionGame` reports `LeaveAsked()` (card 6),
   its last feet and the round phase it last saw (`client.RoundState()` or the [wp-7] accessor: `grep -n
   "RoundPhase" src/Session.cpp`). The session's own exit lines (the world hash line G15 reads: `grep -n "world hash"
   src/Session.cpp`) are logged at every leave, not only at process exit.

**Tests**
- `Settings.AreaKeysRoundTrip` (above). The rest is proved by card 7's headless run.

**Checks**: build; `tools/ctest.sh -R 'Settings\.' --output-on-failure`; `tools/ctest.sh`; doc-check;
`VERIFY_SLOT=1 tools/verify.sh quick` (offline gates unchanged); by hand, the G25 pair of card 7 with ports 27065.

**Docs**: `README.md` (game) "Controls": Enter at an area's border joins, at its edge leaves. `docs/verify.md`
(card 7).

**Must not break**: G3-G10 (no area within their reach: the area is 400 m east), G15-G17 (`--connect` still runs
`PlaySession`), G21 (its camera at (-1, 61, 65): the curtain 400 m away is not drawn: the fence switches on only
within 96 m).

**Done when**
- [ ] a manual run joins and leaves (card 7 makes it a gate)

**Traps**
- ENet's `Connect` blocks? Read `EnetTransport::Connect`: if it waits for the handshake, call it at most once per
  Enter and show `JOINING` before (the HUD shows the frame after). Never call it each frame.
- The session's `Engine::SetPlayBounds` is set by the session game; the shell's `ResumeAt` must set the local one
  back (`Update` does it in the next frame).

## Card 6: Prompts, leave and the pause menu

| | |
|---|---|
| Goal | The HUD asks before joining and before leaving; the pause menu can leave; refusals read clearly |
| Depends on | card 5 |
| Effort | 1 d (e) |
| Touches | game (`src/Areas.cpp`, `src/Session.cpp`). Wire: no |

**Read first**: `engine/src/core/Hud.cppm` `HudCanvas` (`Text`, `Measure`, `TextScale`, `Size`); `src/Session.cpp`
the `EDGE OF THE FIELD` hint ([wp-5]: `grep -n "EDGE OF THE FIELD" src/Session.cpp`), the pause menu items,
`kMenuQuit`.

**Steps**
1. `AreaApproach::Draw` (local HUD, below the crosshair at 60 % of the height, centred, `TextScale` x 2):
   - `Prompt`: `JOIN THE SIEGE AT HOLLOWKEEP?` and under it `ENTER join   walk away to cancel` (white, the key in
     yellow `Rgba(255, 220, 90)`).
   - re-entry running: `HOLLOWKEEP: REENTRY IN 42 S` (whole seconds, ceil).
   - `Connecting`: `JOINING HOLLOWKEEP...`.
   - `Failed`: the reason for 3 s: `NO SERVER SET`, `NO ANSWER`, `FULL`, `WRONG VERSION` (Protocol / Build),
     `WRONG TOKEN`, `REGION MISMATCH`.
2. `SessionGame` in an area (a flag `SetArea(const sim::AreaDesc*)` from the shell): when `sim::InLeaveBand(region,
   feet.xz)`: `LEAVE THE SIEGE?   ENTER leave` in place of [wp-5]'s hint text; Enter there sets `m_LeaveAsked` and
   `engine.RequestExit()`. The pause menu's `"Leave the session"` reads `"Leave the siege"` in an area and does the
   same (`m_LeaveAsked`). A disconnect from the server (`Lost()`) or the round's session end ends the run too
   (`asked` false).
3. Log lines: `Area: HOLLOWKEEP in reach at (x, z), prompt shown` (once per entry into the band),
   `Area: leave asked at (x, y, z)`.

**Checks**: build; `tools/ctest.sh`; doc-check; `VERIFY_SLOT=1 tools/verify.sh quick`; a headless `--area-test` run
of card 7 with `--screenshot` at a frame inside the band (frame 30): look at it (the prompt must be readable).

**Docs**: `docs/owner-checks.md` (game): **check 23** "Event area: walk to Hollowkeep (400 m east of the plaza), the
curtain shows within 96 m, you cannot walk in, the prompt shows at the border, Enter joins in a few seconds (note
the time), the world inside looks the same, walk to the edge, Enter leaves, you stand outside where you left;
pause menu `Leave the siege` does the same". `docs/owner-checks.md` lists it under "Network session".

**Must not break**: [wp-5]'s hint outside areas; [wp-7]'s HUD.

**Done when**
- [ ] the screenshot shows the prompt; owner check 23 written

## Card 7: Gate G25 and close

| | |
|---|---|
| Goal | A headless client joins and leaves the area; its world equals the server's; the local world comes back unchanged |
| Depends on | cards 1-6 |
| Effort | 1 d (e) |
| Touches | game (`--area-test`), tools (`verify.sh`, `lib/verify-gates.sh`, fixtures, self-test), docs |

**Steps**
1. `--area-test` (game, `ShredborneGame`): at start the player is put at `area.testEntry` on the ground (as
   `--camera` scripts place things: `ResumeAt`), the script presses Enter at frame 60 (the same code path as the key),
   in the session walks +x (`PlayerInput` forward with the yaw of +x) for 240 frames, then asks leave (the path of
   Enter in the leave band, without walking there), and after the return walks -x for 60 frames. Excludes
   `--connect`, `--join`, `--practice`, `--editor`, the blast scripts (exit 2 with a message, as `RunPractice` does).
   Headless only; with `--frames` (rule: never a script without `--frames`).
2. Gate G25 in `tools/verify.sh` (tier `full`; port `P25` = 27045 in the main checkout, `B + 5` in a slot: add it to
   both port lines and the busy-port loop, `grep -n "P20S" tools/verify.sh`):

```
T=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/hollowkeep.session --listen P25 \
    --bind 127.0.0.1 --token $T --exit-when-empty
./build/Shredborne --headless --validation --area-test --area-server 127.0.0.1:P25 --area-token $T --frames 900 \
    --screenshot build/verify/G25.png
```

   Judge `gate_G25` (`tools/lib/verify-gates.sh`, beside `gate_G15`): both exit 0; the client log has `Area: joined
   HOLLOWKEEP in X s` with X < 10; `Area: left HOLLOWKEEP after N ticks (asked)`; `Area: local world back in ... -
   same`; the client's `world hash H ... over [box]` equals the server's `window hash H over [box]` of its "client
   left" line (reuse G15's comparison function).
3. Fixture `tools/tests/fixtures/verify/G25.log` (a real passing run's lines, trimmed) and planted faults in
   `tools/selftest-verify.sh` as `sed` edits: `- same` -> `- DIFFERENT`; the joined time 12.5 s; the client's hash
   changed. Each must make the judge fail with its message.
4. CLAUDE.md section 2: the G25 row (command and Must, as above). `docs/verify.md`: the flags `--area-test`,
   `--area-server`, `--area-token`, the lines, the port P25. `tools/doc-check.sh` compares the gate table with
   `tools/verify.sh --list`: keep them equal.
5. Close: `VERIFY_SLOT=1 tools/verify.sh full --ci`; `VERIFY_SLOT=1 tools/verify.sh soak` in the background (a soak
   failure unrelated to this stage is recorded in ROADMAP as a known flake); `DEPS_FROM=build/_deps tools/build.sh
   ci` clean.
6. Documents: `event-areas.md` "Stages" row [area-1] -> **Built** (engine and game `[area-1]` commits, date), the
   measured join, return and parked-RSS numbers into "Memory and cost" (replacing the estimates); ROADMAP "Now" /
   "Next" and the test count (`ctest -N --test-dir build | tail -1`; the `TEST(` count); engine/docs/README.md: the
   document row for event-areas.md and the stage IDs `[area-1]`, `[area-2]`, `[area-3]` (if the integrator has not
   added them); `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage.

**Log lines** (the judge and the report read them; exact)
- `Area: joining HOLLOWKEEP at 127.0.0.1:27045, entry (397.00, 0.00)`
- `Area: parked world N chunks, RSS X MiB`
- `Area: joined HOLLOWKEEP in 3.21 s (slot 0, spawn (402.30, 12.34, 0.00))`
- `Area: left HOLLOWKEEP after 240 ticks (asked), back at (397.00, 11.90, 3.10)`
- `Area: local world back in 0.41 s, hash 0123456789abcdef before, 0123456789abcdef after - same`

**Numbers** (the report): the join and return times and the parked RSS from G25's log (dev build), the area's
server preload (`Server: region of ... loaded in X s`), and the GPU upload after the return (the `upload` pass of the
first frames: `grep upload` in the log if `--gpu-log` exists; else "not measured").

**Checks**: as step 5; G25 by itself first: `VERIFY_SLOT=1 tools/verify.sh G25` (or the tier's single-gate option:
`tools/verify.sh --help`).

**Done when**
- [ ] G25 passes in `full`; its self-test fires on each planted fault
- [ ] ROADMAP, event-areas.md, CLAUDE.md, docs/verify.md, owner check 23 updated in the same commits

## Values this stage lands

- `net::kProtocolVersion` 14 (`Hello` + 9 B: `entryFlags u8`, `entryXcm i32`, `entryZcm i32`). notes/net.md
  section 2 gets the line; llm/README "Values planned across the packs": protocol 14 `[area-1]`, gate G25.
- No new `MessageType`, `EventKind`, `PlayerButton`, `RefuseReason`, `RegionTemplateId`; `kPlayerRuleRevision`
  unchanged; `buildHash` unchanged.
- Owner check 23. Port P25 27045 / B+5.
