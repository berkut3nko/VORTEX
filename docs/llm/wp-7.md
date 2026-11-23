# [wp-7] M2a "first fight": task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/networking|networking]]

| | |
|---|---|
| Stage | `[wp-7]` M2a "first fight" (ROADMAP "Next" item 3): the sandbox becomes a PvP game. You can hurt, die, respawn at your team's spawn and win a round |
| Design | The project review (game `docs/history/reviews/project-review-2026-11-17.md`) "WP7" and section 3.5 rows "No stakes or goal", "Cast rate vs budgets"; ROADMAP "M2 - Fight feel under authority"; [networking.md](../design/networking.md) "Player movement", "Entity replication"; [magic.md](../design/magic.md) "Resources" and "Time to kill, against M2a" (what later stages assume). No design document exists yet: card 1 writes `engine/docs/design/fight.md` from this pack's "Decisions" |
| Code read at | engine `85bf73b`, game `270eb27` (`[wp-4]` committed; `[wp-5]` and `[wp-6]` land before this stage: find code with `grep -n`, never by line number) |
| Depends on | `[wp-4]` (settings page, pause menu), `[wp-5]` (play boundary, moving window), `[wp-6]` (`vortex.sim:rules`, the `Server` / `Client` split into `Server*.cpp` / `Client*.cpp`) |
| Cards | 11: design (0.5 d), skeleton (0.5 d), combat rules (1 d), wire (1 d), server fight (1.5 d), client fight state (1 d), fighting bots + G22 (1 d), HUD (1 d), Felling (1 d), practice in-process (1.5 d), close (0.5 d) |
| Effort | 10.5 d (e) (review: 8-12 d) |
| Changes | wire: **yes**, `net::kProtocolVersion` 10 -> **11** (card 4 only). Movement rule: **no** (`sim::kPlayerRuleRevision` stays 1: a dead player is not stepped; the caller skips it). `buildHash`: no new input. Gates: **G22** (a 2v2 bot match ends with a winner, tier `full`) |

What the player gets: health, blast damage, falls and lava that hurt, death with a 3 s respawn at the team spawn,
two teams, three cast charges, rounds won by score (team deathmatch) or by felling the enemy tower (Felling duel),
a HUD with health, charges, kill feed, round banner and scoreboard, bots that fight, and offline practice against
bots through an in-process server.

## As built (2026-11-23)

Built in one stage (engine and game `[wp-7]` commits); the code is the reference where it differs from the cards below:

- Card 2's stub bodies and `DISABLED_` tests were skipped: the implementation went in directly (one commit).
- **Warmup**: players are stepped and cast freely (no charges, no damage); `spawned == false` only shows as the owner
  flag `kSpectator`. Decision 7's "not stepped" applies to the dead only (a joiner waiting frozen is no fun, and its
  client predicts it like any player). A joiner's `Welcome::spawn` is its team spawn.
- `KillNotice` refuses `killer == victim + 1` (a self credit; `killer` is slot + 1), not `victim == killer`.
- `Combat.BlastDamageFallsLinearlyToZeroAt1200mm` uses centres that are whole 1/16 voxels: 0.5 m aside 50, 0.75 m 37,
  1.5 m 0, above the head at 12.5 m 25 (0.7 m and 12.4 m are not representable at 0.125 m).
- The session tests' flat field uses a blast of 80 at the centre (the victim falls into the first crater, and a
  second blast at its floor takes less), casts 2.5 m away with a third allowed, edges 16 ticks apart.
- `vortex_server --until-round-end` turns `end_session` off for its in-process bots (the drain compares the hashes
  after the round) and also logs `Fight: bot N (slot S, team T): ...`.
- G22's fixture is `tools/tests/fixtures/verify/G22.log` with the planted faults as `sed` edits in
  `tools/selftest-verify.sh` (the repository's pattern), not three fixture files.
- Practice: `end_session` is off (the result stays until Enter; Enter rebuilds the server, the bots and the engine),
  the bots load before the engine starts, and the loopback needs no keep-alive thread. The session's engine and game
  are `PlaySession` (`src/Session.h`), shared by `--connect` / `--join` and `--practice`.
- HUD: a dead remote figure is hidden (not destroyed); a red edge flash on damage was added; the warmup banner shows
  the roster's count only (the client does not know `min_players`).
- Added beside the pack's interfaces: `sim::SandboxRules`, `ToString` of causes, modes, phases and ends,
  `net::Client::TeamOf`, `net::Server::RoundState` / `Fighter` / `SessionOver` / `FightLine`,
  `BotPilot::TargetSlot`, `RemotePlayerView::fight`.

## Before you start

- `git log --oneline | grep -E '\[(wp-4|wp-5|wp-6)\]'` shows all three in the game root and in `engine/`. Missing:
  stop, `Status: blocked`.
- The code this stage builds on (check each with `grep -n`):
  - `engine/src/sim/Rules.cppm` (`[wp-6]`): `sim::TeamId`, `kNoTeam`, `ActionSlot`, `ActionRequest`, `ActionAim`,
    `DamageEvent{target, source, amount, cause}`, `PlayerView{player, team, feet, alive}`,
    `BlastDamageRule{centre, zeroMm}`, `RulesContext{tuning, players, tick, blastDamage}`, `ActionOutcome::damage`,
    `ResolveAction`. Read the built file; this pack uses those names exactly.
  - `engine/src/sim/Player.cppm`: `PlayerState` (`position`, `velocity`, `flags`, `cooldownTicks`, `lastButtons`),
    `PlayerTuning` (`height` 1.8, `radius` 0.3, `eyeHeight` 1.62, `castCooldownTicks` 15), `PlayerButton_Cast` (4),
    `kPlayerButtonMask` (15), `LookDirection(uint16_t yaw, int16_t pitch)`, `StepPlayer`, `PlayerStepResult`.
  - `engine/src/sim/WorldOp.cppm`: `WorldOp::Box(min, max, id)` (`FillBox`, voxel box `[min, max)`).
  - `engine/src/net/Messages.cppm`: `kProtocolVersion` 10, `MessageType` 1-20, `CastRejection` 0-3 and
    `kMaxCastRejection`, `Welcome`, `RemotePlayer`, `Snapshot` (owner block `lastCastTick`, `castRejected`),
    `kSnapshotOwnerBytes` 32, `kSnapshotPlayerBytes` 16, `RejectionOf(sim::ActionRefusal)` (`[wp-6]`).
  - `engine/src/net/Session.cppm`: `SessionConfig`, `ParseSession`, `FormatSession`.
  - `engine/src/net/internal/ServerImpl.cppm` and `ServerPeers.cpp` (`OnHello`, `StepPlayers`, `SendSnapshots`),
    `ServerCasts.cpp` (`CaptureCast`, `RunCast`, `RunCasts`), `Server.cpp` (`Server::Start`, the tick loop),
    `Server::TeleportPlayer`. Client: `ClientPrediction.cpp` (`RunTick`, `Reconcile`, the history), `Client.cpp`
    (message dispatch).
  - `engine/src/net/Drill.cppm`: `PilotSettings`, `BotPilot::Next(const sim::PlayerState&)`; `Bot.cppm`: `net::Bot`,
    `BotSettings::pilot`. `engine/tools/server/main.cpp`: `--clients`, `--ticks`, the in-process bots.
  - Game: `src/Session.cpp` `SessionGame` (`DrawHud`, `m_Remotes`, the figure model, `m_Transport` typed
    `net::EnetTransport&`), `RunSession`; `src/main.cpp` flag dispatch (`--connect` / `--join` -> `RunSession`).
  - `engine/src/core/Hud.cppm` `HudCanvas` (`Rect`, `Text`, `Measure`, `Crosshair`, `TextScale`, `Size`,
    `DimBackground`).

## Decisions

Settled by us (2026-11-23; the owner may veto). Values first fixed by the review and ROADMAP M2 are kept, except the
respawn time (decision 5).

1. **Server-only rules.** Health, damage, death, respawn, charges, teams, score and the round are decided by the
   session server only; clients show what the owner block, the remote record and three new messages say. Nothing of
   it is predicted except "a dead player does not move" (decision 7). Floats may decide server-only results; this
   pack still uses integers wherever a test pins a value.
2. **`vortex.sim:combat`** (`engine/src/sim/Combat.cppm`, implementation `internal/Combat.cpp`): GPU-free and
   network-free. It holds the rules (`FightRules`), the per-player state (`FighterState`), the damage functions and
   the round machine (`sim::Round`). `net::Server` owns one `FighterState` per slot and one `Round`; the offline
   sandbox owns none.
3. **Modes**: session key `mode = sandbox | deathmatch | felling`, default `sandbox`. `sandbox` is today's game: no
   teams (team 0), no damage, unlimited charges, no round. Every existing session file (`drill`, `arena250`,
   `playground`) stays `sandbox`, so G11-G21 keep their behaviour. `deathmatch`: the first team to `score_limit`
   kills wins. `felling`: as deathmatch, and releasing the enemy tower's crown wins at once (card 9).
4. **Health 100.** A blast does 60 at its centre and falls linearly to 0 at 1200 mm, measured from the blast centre
   to the surface of the victim's capsule (decision 9). Friendly fire off, self-damage off.
5. **Respawn 3 s** (180 ticks) at the team spawn (review / ROADMAP had 5 s; 3 s keeps a 2v2 moving, fun first; card
   1 updates ROADMAP M2 and magic.md's two "5 s" mentions). After a respawn: 2 s (120 ticks) of spawn protection (no
   damage taken), cleared by the player's first cast.
6. **Charges**: 3 at most, one back every 72 ticks (1.2 s), the 15-tick cooldown kept as the burst limit. A cast edge
   at capture: dead -> `CastRejection::Dead` (5); no charge -> `NoCharge` (4); else one charge is spent and the cast
   queued. No refunds (a cast refused later, Missed / Outside, keeps its charge spent). Session key `charge_ticks`
   (wp-8 sets 60 in its fight10 session), `max_charges`.
7. **Death and the movement rule.** A dead player is not stepped (the server skips `StepPlayer` for it; its state is
   frozen where it died, velocity zero). The client stops predicting while its owner block says dead. A respawn is a
   teleport by the server: `PlayerState{spawn feet, velocity 0, flags 0, cooldownTicks 0, lastButtons =
   kPlayerButtonMask}` (no button edge until released: a held click never fires at spawn). The owner block's
   `lifeSeq` (u8, +1 per spawn) tells the client: a new `lifeSeq` adopts the snapshot's owner state and replays the
   inputs after it **without** counting a correction or a snap (session-check's 0 snaps stays true). No change to
   `StepPlayer`: `kPlayerRuleRevision` stays 1.
8. **Environment damage** (server, after each player's step, slot order): a **fall**: on the tick a player becomes
   `OnGround` after a step that began in the air, with `s = (int)floor(-velocityBefore.y * 100)` cm/s, damage
   `(s - 886) * 11 / 100` (integer, floor) when `s > 886` (886 cm/s = a 4 m fall). **Lava**: when the voxel under the
   feet (`feet - 0.05 m` in y) or at the shins (`feet + 0.2 m`) is the material `lava`, 20 every 15 ticks (first hit
   at once). **Void**: feet below `region height min + 2 m`: 999. Causes `Fall`, `Lava`, `Void`.
9. **Blast damage geometry** (integers, millimetres): centre `c = llround(double(centreQ) * voxelSize * 1000 / 16)`
   per axis; feet `f = llround(double(feet) * 1000)` per axis; the capsule axis runs from `f.y + rMm` to
   `f.y + hMm - rMm` (`rMm = llround(radius * 1000)`, `hMm = llround(height * 1000)`); `dy` = distance of `c.y`
   outside that range (0 inside); `d2 = dx^2 + dz^2 + dy^2` (int64); `d = ISqrt64(d2)` (floor); `s = max(0, d - rMm)`;
   damage `= centre * (zeroMm - s) / zeroMm` (int64, floor) when `s < zeroMm`, else 0. No occlusion: walls within
   1.2 m are carved by the same blast.
10. **Kill credit.** A death by `Blast` credits its source if the source is on the other team. A death by `Fall`,
    `Lava`, `Void` or `Crush` credits the last enemy who damaged the victim within 300 ticks (5 s); else nobody.
    A credited kill: killer `kills + 1`, its team's score + 1. Every death: victim `deaths + 1`.
11. **The round** (`sim::Round`, decision 12 of card 3): phases `Warmup` (until `min_players` players are on teams)
    -> `Countdown` (`countdown_ticks`, 300; every player respawned at its team spawn at its start, health full,
    charges full, scores 0) -> `Live` (`round_seconds` 480 s; damage counts only here) -> `Ended` (`end_ticks`, 600;
    nobody takes damage, casts still carve). Win: a team reaches `score_limit` (default 15), or Felling's crown
    (card 9), or a team has no players for 1800 ticks in `Live` (forfeit). At time out: the higher score wins; a tie
    goes to **overtime**: the next credited kill wins, at most 3600 more ticks, then a draw.
12. **One round per session** (review WP7): after `Ended` the server logs `Round: session over`, sends a last
    `RoundUpdate`, and with `end_session = 1` (default for non-sandbox) disconnects every peer and stops
    (`vortex_server` exits 0; a live install's wrapper restarts it, clients rejoin). No region reset.
13. **Teams**: 1 and 2 (`TeamId`). A joiner goes to the team with fewer players, ties to team 1. A peer that leaves
    frees its place; teams are never rebalanced mid-round. Team spawns are session keys `team_spawn_1 = X Z`,
    `team_spawn_2 = X Z` (metres, at least 4 m inside the play bounds); the feet go to the ground found by a
    downward `World::Raycast` from `region height max` at `(X + ox, Z + oz)`, `(ox, oz)` = 2 m on a ring of 8
    (`k = respawns of that team mod 8`, angle `k * 45` degrees), plus 0.05 m; a miss uses `(X, Z)` itself, then
    `Welcome::spawn`.
14. **Wire (protocol 11)**, the only bump in WP4-WP8, values from llm/README "Values planned across the packs":
    `CastRejection` 4 `NoCharge`, 5 `Dead` (`kMaxCastRejection` 5); `MessageType` 21 `Roster`, 22 `RoundUpdate`,
    23 `KillNotice`; owner block +7 B (39); remote record +1 B (17); `Welcome` +7 B. No new `PlayerButton`, no new
    `EventKind`: the blast stays on `PlayerButton_Cast`.
15. **`DamageCause`** (a wire value in `KillNotice`): 1 `Blast`, 2 `Fall`, 3 `Lava`, 4 `Void`, 5 `Crush` (reserved
    for `[si-2]`; nothing emits it here). `kMaxDamageCause` 5. `[mag-1]` / `[mag-2]` spell damage uses `Blast`.
16. **The damage entry** on the server is `Server::Impl::DealDamage(const sim::DamageEvent&)` in the new
    `engine/src/net/internal/ServerFight.cpp`; every damage (blasts, environment, later spells and crush) goes
    through it, so kill credit, the feed and the score have one path.
17. **Bots fight** with `PilotSettings::fight` (wp-8 relies on the name): `vortex_server --fight`,
    `vortex_bot --fight`. Deterministic: no randomness besides the pilot's existing seed.
18. **Offline practice** is an in-process `net::Server` on `net::LoopbackTransport`, driven on the game's main thread
    (transports are main-thread only), with in-process `net::Bot`s: `Shredborne --practice [--bots N]
    [--mode deathmatch|felling]`. The offline sandbox (no flag; G3-G10, the editor) is unchanged. mag-2 and si-2
    find it with `grep -n 'InProcessServer' src/`.
19. **HUD** (game, `HudCanvas`, no ImGui; CLAUDE.md "HUD"): health bar, charge pips, hit marker, kill feed, round
    banner, scoreboard (Tab, or `--scoreboard` for screenshots), death screen with the respawn count-down. Team
    colours: team 1 blue `Rgba(70, 130, 255)`, team 2 red `Rgba(230, 70, 60)`; figures take their team's torso
    colour; a dead remote figure is hidden. Names are `P<slot + 1>`, the own one `YOU`.
20. **fight.md headings** (card 1 writes them; code comments may cite only these): "Decision", "Rules", "Damage",
    "Round", "Wire", "Bots", "Practice", "HUD", "Stages", "Open questions".

## Skeleton

Card 2 lands it with stub bodies (`// [wp-7] card N fills this`) and every test of this stage as `DISABLED_`.

`engine/src/sim/Combat.cppm` (`Sim.cppm` gets `export import :combat;` after `export import :rules;`):

```cpp
module;
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <vector>
export module vortex.sim:combat;
import vortex.voxel;
import :simulation;
import :region;
import :player;
import :rules;

// docs/design/fight.md "Rules": health, damage, charges, teams and the round; server-only results. [wp-7]
export namespace vortex::sim {
    enum class DamageCause : uint8_t { None = 0, Blast = 1, Fall = 2, Lava = 3, Void = 4, Crush = 5 };
    inline constexpr uint8_t kMaxDamageCause = 5;
    enum class GameMode : uint8_t { Sandbox = 0, Deathmatch = 1, Felling = 2 };
    inline constexpr uint8_t kMaxGameMode = 2;
    inline constexpr TeamId kTeamCount = 2;            ///< teams 1 and 2

    /// @brief A session's fight rules (session keys in brackets). Sandbox: every rule off.
    struct FightRules {
        GameMode mode = GameMode::Sandbox;             ///< [mode]
        int16_t maxHealth = 100;
        BlastDamageRule blast{60, 1200};               ///< centre damage, zero at mm
        uint16_t respawnTicks = 180;                   ///< [respawn_ticks]
        uint16_t protectTicks = 120;
        uint8_t maxCharges = 3;                        ///< [max_charges]; 0 = unlimited (sandbox)
        uint16_t chargeTicks = 72;                     ///< [charge_ticks]
        uint16_t scoreLimit = 15;                      ///< [score_limit]
        uint32_t roundTicks = 28800;                   ///< [round_seconds] x 60
        uint16_t countdownTicks = 300;                 ///< [countdown_ticks]
        uint16_t endTicks = 600;                       ///< [end_ticks]
        uint32_t overtimeTicks = 3600;
        uint32_t forfeitTicks = 1800;
        uint16_t creditTicks = 300;                    ///< environment deaths credit the last enemy attacker this recent
        uint8_t minPlayers = 2;                        ///< [min_players]
        bool endSession = true;                        ///< [end_session]
        uint16_t fallSafeCmS = 886;
        uint16_t fallPerMetrePerSecond = 11;
        int16_t lavaDamage = 20;
        uint16_t lavaEveryTicks = 15;
        std::array<glm::vec2, 2> teamSpawns{};         ///< [team_spawn_1], [team_spawn_2] metres x / z
        std::array<glm::vec2, 2> towers{};             ///< [tower_1], [tower_2] (felling)
        bool Active() const { return mode != GameMode::Sandbox; }
        friend bool operator==(const FightRules&, const FightRules&) = default;
    };

    /// @brief One player's fight state on the server (slot-indexed).
    struct FighterState {
        TeamId team = kNoTeam;
        int16_t health = 100;
        uint8_t charges = 3;
        uint32_t nextChargeTick = 0;                   ///< 0 = full
        bool dead = false;
        bool spawned = false;                          ///< false until its first spawn on a team
        uint32_t respawnTick = 0;                      ///< dead: the tick it respawns
        uint32_t protectedUntil = 0;                   ///< no damage before this tick
        uint8_t lifeSeq = 0;                           ///< +1 per spawn (owner block)
        uint8_t hits = 0;                              ///< damage events dealt to enemies, wrapping (hit marker)
        uint8_t lastAttacker = 0;                      ///< slot + 1 of the last enemy that damaged it
        uint32_t lastAttackTick = 0;
        uint32_t nextLavaTick = 0;
        uint16_t kills = 0, deaths = 0;
    };

    /// @brief What ApplyDamage did.
    struct DamageResult {
        int16_t dealt = 0;                             ///< health actually taken
        bool killed = false;                           ///< health reached 0 now
        uint8_t credit = 0;                            ///< slot + 1 credited with the kill; 0 = none
    };

    /// @brief Floor of the square root of `v` (v >= 0), exact for every int64.
    int64_t ISqrt64(int64_t v);
    /// @brief Decision 9: a blast at `centreQ` (1/16 voxel) against a player with feet at `feet`.
    int16_t BlastDamageAt(const BlastDamageRule& rule, const glm::ivec3& centreQ, float voxelSize, const glm::vec3& feet,
                          const PlayerTuning& tuning);
    /// @brief Fall damage for the vertical speed (m/s, negative down) before a landing; 0 under fallSafeCmS.
    int16_t FallDamage(const FightRules& rules, float velocityBeforeY);
    /// @brief Takes `event.amount` from `target` (team rules, protection, credit; decision 10). `source` may be null.
    DamageResult ApplyDamage(FighterState& target, uint8_t targetPlayer, FighterState* source, const DamageEvent& event,
                             const FightRules& rules, uint32_t tick);
    /// @brief One back every chargeTicks up to maxCharges; call once per tick per living fighter.
    void TickCharges(FighterState& fighter, const FightRules& rules, uint32_t tick);
    /// @brief Spends one charge (true) or refuses (false). Unlimited when maxCharges is 0.
    bool SpendCharge(FighterState& fighter, const FightRules& rules, uint32_t tick);
    /// @brief Health, charges, protection, lifeSeq for a new life; dead and respawn cleared.
    void StartLife(FighterState& fighter, const FightRules& rules, uint32_t tick);
    /// @brief Decision 13: the feet of the k-th spawn of `team` (server world ray).
    glm::dvec3 TeamSpawnFeet(Simulation& simulation, const FightRules& rules, const RegionDesc& region, TeamId team,
                             uint32_t k, const glm::dvec3& fallback);
    /// @brief The team a joiner gets (decision 13): fewer players, ties to 1.
    TeamId PickTeam(std::span<const FighterState> fighters, std::span<const bool> occupied);
    /// @brief Yaw / pitch (PlayerInput units) that LookDirection turns back into `dir` (within 0.1 degree).
    void AimAngles(const glm::vec3& dir, uint16_t& yaw, int16_t& pitch);

    enum class RoundPhase : uint8_t { Warmup = 0, Countdown = 1, Live = 2, Ended = 3 };
    enum class RoundEnd : uint8_t { None = 0, Score = 1, Time = 2, Felled = 3, Forfeit = 4 };
    inline constexpr uint8_t kDraw = 255;              ///< Round::winner of a draw
    /// @brief The round machine (decision 11). Server-only; `RoundUpdate` carries it.
    struct Round {
        RoundPhase phase = RoundPhase::Warmup;
        uint32_t phaseEndTick = 0;                     ///< 0 = open-ended (Warmup)
        std::array<uint16_t, 2> score{};
        bool overtime = false;
        uint8_t winner = 0;                            ///< 0 none, 1, 2, kDraw
        RoundEnd end = RoundEnd::None;
        uint32_t emptySince[2]{};                      ///< Live: tick a team became empty, 0 = it has players
    };
    /// @brief What StepRound changed this tick (the server sends a RoundUpdate on any).
    struct RoundStep {
        bool changed = false;
        bool startLives = false;                       ///< Countdown began: respawn everyone, reset scores
        bool sessionOver = false;                      ///< Ended ran out
    };
    /// @brief One tick of the machine. `teamPlayers` = players per team; `felled[t]` = team t+1's crown released.
    RoundStep StepRound(Round& round, const FightRules& rules, uint32_t tick, std::array<uint32_t, 2> teamPlayers,
                        std::array<bool, 2> felled);
    /// @brief A credited kill for `team` (score, overtime end). Returns true when it ended the round.
    bool ScoreKill(Round& round, const FightRules& rules, TeamId team, uint32_t tick);

    /// @brief Felling (card 9): one team's tower as FillBox operations and its crown box.
    struct FellingTower {
        std::vector<WorldOp> ops;
        glm::ivec3 crownMin{0}, crownMax{0};           ///< voxel box [min, max)
        uint32_t crownVoxels = 0;                      ///< solid voxels in the crown once built
    };
    FellingTower BuildFellingTower(const voxel::World& world, const voxel::WorldMaterials& m, const glm::vec2& at,
                                   float voxelSize, int32_t topVoxelY);
    /// @brief Solid voxels in [min, max), x then y then z ascending (World::GetVoxel).
    uint32_t CountSolid(const voxel::World& world, const glm::ivec3& min, const glm::ivec3& max);
}
```

`engine/src/net/Messages.cppm` (card 4 lands the values; card 2 only the types with the old protocol number):

```cpp
// MessageType: Roster = 21, RoundUpdate = 22, KillNotice = 23 (server -> client, events channel)
// CastRejection: NoCharge = 4, Dead = 5; kMaxCastRejection = 5
/// @brief The fight fields of Welcome (7 B): mode, maxHealth, maxCharges, chargeTicks, respawnTicks.
struct WelcomeFight { sim::GameMode mode = sim::GameMode::Sandbox; uint8_t maxHealth = 100; uint8_t maxCharges = 0;
                      uint16_t chargeTicks = 0; uint16_t respawnTicks = 0; };   // Welcome::fight
/// @brief Owner block fields (+7 B, after castRejected).
struct OwnerFight { uint8_t health = 100; uint8_t charges = 0; uint8_t chargeTicks = 0; uint8_t flags = 0;
                    uint8_t respawnTicks = 0; uint8_t lifeSeq = 0; uint8_t hits = 0;
                    static constexpr uint8_t kDead = 1, kProtected = 2, kSpectator = 4; };     // Snapshot::fight
// RemotePlayer gains `uint8_t fight = 0;` bits 0-6 health (min 127), bit 7 dead.
struct RosterEntry { uint8_t slot = 0; sim::TeamId team = 0; uint16_t kills = 0, deaths = 0; };
struct Roster { std::vector<RosterEntry> players; };              // count u8 (max 64), 6 B each
struct RoundUpdate { sim::GameMode mode{}; sim::RoundPhase phase{}; uint32_t phaseEndTick = 0;
                     std::array<uint16_t, 2> score{}; uint16_t scoreLimit = 0; uint8_t winner = 0;
                     sim::RoundEnd end{}; bool overtime = false; };                         // 16 B with type
struct KillNotice { uint32_t tick = 0; uint8_t victim = 0; uint8_t killer = 0; sim::DamageCause cause{}; uint8_t flags = 0;
                    static constexpr uint8_t kCredited = 1; };                               // 9 B with type
```

`engine/src/net/Session.cppm`: `SessionConfig` gains `sim::FightRules fight;` (keys in the comments above).
`engine/src/net/Client.cppm`: `struct FightView` (below, card 6) and `Client::Fight()`, `Client::RosterOf()`,
`Client::RoundOf()`, `Client::TakeKillNotices()`. `engine/src/net/Drill.cppm`: `PilotSettings::fight`,
`PilotTarget`, `BotPilot::SetTargets`.

Tests: new `engine/tests/sim/CombatTests.cpp` (suites `Combat`, `Felling`), `Rules.*` appended to
`engine/tests/sim/BlastTests.cpp`, new `engine/tests/net/FightTests.cpp` (suites `Fight`, `FightWire`), the pilot
test in `engine/tests/net/BotTests.cpp` (or the file holding `Bot.*` tests: `grep -ln 'TEST(Bot,' engine/tests`).
Files are globbed: no CMake line.

## Card 1: fight.md and the plan

| | |
|---|---|
| Goal | The fight has its design document; ROADMAP and magic.md agree with it |
| Depends on | - |
| Effort | 0.5 d (e) |
| Touches | documents only. Wire: no. Movement rule: no. buildHash: no |

**Read first** (about 30 KiB): this pack "Decisions", "Skeleton"; `engine/docs/design/rules.md` (`[wp-6]`, as the
model of a header table); `engine/docs/README.md` "Documents"; ROADMAP "M2"; `magic.md` lines with `5 s`
(`grep -n '5 s' engine/docs/design/magic.md`).

**Steps**
1. `engine/docs/design/fight.md`: title `# Fight rules (M2a)`; nav line
   `[[../../../00 Docs Map|Docs map]] · [[rules|rules seam]] · [[networking|networking]] · [[magic|magic]] · [[../notes/net|net notes]]`;
   header table: Status "Designed; building in [wp-7]"; Decided "Us, 2026-11-23 (the owner may veto): decisions 1-20
   of the [wp-7] task pack"; Next "[wp-7] cards 2-11"; Open "the owner's taste check"; History "-"; Marks "code read
   at engine `85bf73b`". Then the ten headings of decision 20: "Decision" (decisions 1-3, 12), "Rules" (4-8, 10, 13),
   "Damage" (9 with the worked numbers of card 3's tests), "Round" (11), "Wire" (14-15 and card 4's byte tables),
   "Bots" (17, card 7's behaviour), "Practice" (18), "HUD" (19, card 8's layout), "Stages" (`| [wp-7] | M2a first
   fight | designed |`), "Open questions" ("The owner's taste check after the first playtest: too fast, too slow,
   too lethal?").
2. `engine/docs/README.md` "Documents": a row after `design/rules.md`; `00 Docs Map.md` (game): a line after the
   rules line: `- [[engine/docs/design/fight|Fight rules]] — health, damage, teams, charges and rounds (M2a).`
3. ROADMAP "M2": "5 s respawn" -> "3 s respawn (fight.md)". magic.md: the two "5 s" respawn mentions -> "3 s"
   (wording only; nothing else).
4. `engine/docs/design/game-ui.md` "Next": "M2a's HUD ... is WP7" -> "M2a's HUD: a link to fight.md \"HUD\"".

**Checks** `tools/doc-check.sh --build build` - PASS.
**Done when** fight.md has the ten headings, the nav line, the header table; doc-check PASS; engine commit, then
game commit (Docs Map + submodule).

## Card 2: skeleton

| | |
|---|---|
| Goal | The stage's interfaces compile with stubs; every new test exists disabled |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | Sim (new partition), Net (types only; **protocol stays 10 here**), tests |

**Files**: new `engine/src/sim/Combat.cppm`, `engine/src/sim/internal/Combat.cpp`; change `Sim.cppm`,
`Messages.cppm` (the structs of the Skeleton, not yet encoded; enum values not yet added), `Session.cppm`
(`SessionConfig::fight`, not parsed yet), `Drill.cppm` (card 7's types); new test files.

**Steps**
1. Copy the Skeleton. `Combat.cpp` preamble as `Rules.cpp` (`module;` includes, `module vortex.sim;`, imports
   `vortex.core`, `vortex.math`, `vortex.voxel`, `vortex.physics`, `vortex.scene`). Stubs return 0 / `{}` / `false`
   / `kNoTeam`; `StepRound` returns `{}`.
2. Write every test named in cards 3-10 in full, each `DISABLED_<Name>`.
3. ROADMAP's test count line (doc-check names the numbers).

**Checks** build warning-free; `tools/ctest.sh` all pass; `tools/doc-check.sh --build build` PASS;
`grep -n 'kProtocolVersion = 10' engine/src/net/Messages.cppm` one line.

**Traps**: `vortex.sim:combat` imports no net module (`vortex_assert_no_network`). `RulesContext` has a reference
member: build it as `sim::RulesContext{tuning, players, tick, rule}`. `FightRules`'s defaulted `operator==` needs
one on `[wp-6]`'s `BlastDamageRule`: add `friend bool operator==(const BlastDamageRule&, const BlastDamageRule&) =
default;` there (no behaviour change).

## Card 3: combat rules in sim

| | |
|---|---|
| Goal | Damage, charges, lives, spawns and the round machine work and are pinned by tests; a blast's outcome lists its damage |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | Sim (`Combat.cpp`, `Rules.cpp`). Wire: no. Movement rule: no |

**Read first**: `engine/src/sim/Rules.cppm` and `internal/Rules.cpp` (`ResolveAction`), this pack decisions 4-13.

**Steps**
1. `ISqrt64`: `r = (int64)std::sqrt((double)v)`; then `while (r * r > v) --r; while ((r + 1) * (r + 1) <= v) ++r;`.
2. `BlastDamageAt`: decision 9 exactly. `rule.centre <= 0` -> 0.
3. `FallDamage`: `s = (int32)std::floor(-velocityBeforeY * 100.0f)`; `s <= fallSafeCmS` -> 0; else
   `min(32767, (s - fallSafeCmS) * fallPerMetrePerSecond / 100)`.
4. `ApplyDamage(target, targetPlayer, source, e, rules, tick)`, in order: (a) `target.dead` or `!target.spawned` or
   `e.amount <= 0` -> `{}`; (b) `tick < target.protectedUntil` -> `{}`; (c) `source != nullptr`: same team (both not
   `kNoTeam`) or `e.source == targetPlayer` -> `{}` (friendly fire, self); else `target.lastAttacker = e.source`,
   `lastAttackTick = tick`, `source->hits += 1` (wraps); (d) `dealt = min(e.amount, target.health)`,
   `health -= dealt`; (e) `health == 0`: `dead = true`, `respawnTick = tick + respawnTicks`, `deaths += 1`; credit =
   `e.source` when cause is `Blast` and source not null; else `target.lastAttacker` if `lastAttacker != 0` and
   `tick - lastAttackTick <= creditTicks`; the caller adds the killer's `kills` (it holds every fighter).
5. `TickCharges`: `maxCharges == 0` or `charges >= maxCharges` -> `nextChargeTick = 0`, return; `nextChargeTick ==
   0` -> `nextChargeTick = tick + chargeTicks`; `tick >= nextChargeTick` -> `charges += 1`, `nextChargeTick =
   charges < maxCharges ? tick + chargeTicks : 0`.
   `SpendCharge`: unlimited -> true; `charges == 0` -> false; `charges -= 1`; if `nextChargeTick == 0`,
   `nextChargeTick = tick + chargeTicks`; `protectedUntil = 0` (a cast ends protection); true.
6. `StartLife`: health `maxHealth`, charges `maxCharges`, `nextChargeTick 0`, `dead false`, `spawned true`,
   `protectedUntil = tick + protectTicks`, `lifeSeq += 1`, `lastAttacker 0`, `nextLavaTick 0`.
7. `TeamSpawnFeet`: decision 13; `team` 1 or 2 indexes `teamSpawns[team - 1]`; angle by an integer table of the 8
   unit offsets (`{1,0},{0.7071,0.7071},{0,1},...`, written as constants, no `std::cos`); ray from
   `(x, region height max - 0.5, z)` down, 256 m; hit -> `hit.y + 0.05`.
8. `PickTeam`: count `spawned`-or-joined fighters per team among `occupied` slots; fewer wins, ties 1.
9. `AimAngles`: invert `LookDirection` (read its body: yaw u16 over a full turn, pitch i16 scale); round to
   nearest. Test it against `LookDirection` (step Tests).
10. `StepRound(round, rules, tick, teamPlayers, felled)`, `!rules.Active()` -> `{}`:
    - Warmup: `teamPlayers[0] + teamPlayers[1] >= minPlayers` and both > 0 -> Countdown,
      `phaseEndTick = tick + countdownTicks`, `score = {0, 0}`, `startLives = true`.
    - Countdown: `tick >= phaseEndTick` -> Live, `phaseEndTick = tick + roundTicks`.
    - Live: `felled[i]` for one team only -> winner the other team, end `Felled`; both -> higher score, else draw.
      A team with 0 players: `emptySince` set the first such tick; `tick - emptySince >= forfeitTicks` -> the other
      team wins, `Forfeit`. `tick >= phaseEndTick`: not overtime -> scores differ: higher wins, `Time`; equal:
      `overtime = true`, `phaseEndTick = tick + overtimeTicks`; overtime ran out -> `winner = kDraw`, `Time`.
      Any win -> Ended, `phaseEndTick = tick + endTicks`.
    - Ended: `tick >= phaseEndTick` -> `sessionOver = true` (phase stays Ended).
    `changed` is true whenever a field changed.
    `ScoreKill`: only in Live; `score[team - 1] += 1`; `overtime` or `score >= scoreLimit` -> that team wins (`Score`),
    Ended.
11. `ResolveAction` (`Rules.cpp`), after the blast is applied and `context.blastDamage.centre > 0`: the source team is
    the `team` of the `PlayerView` with `player == request.player` (none found or `request.player == 0`: no damage);
    for each view in `context.players` in span order: skip `!alive`, `player == request.player`, same team when the
    team is not `kNoTeam`; `amount = BlastDamageAt(context.blastDamage, aim.sphere.center, voxelSize, view.feet,
    context.tuning)` (voxel size from the simulation's world); `amount > 0` -> push
    `DamageEvent{view.player, request.player, amount, uint8_t(DamageCause::Blast)}`.
12. `BuildFellingTower` and `CountSolid` stay stubs (card 9).

**Tests** (`engine/tests/sim/CombatTests.cpp`, enable them now)
- `Combat.BlastDamageFallsLinearlyToZeroAt1200mm`: voxel 0.125 m, tuning default, feet (0, 10, 0). Centre at the feet
  (`centreQ = (0, 1280, 0)`): 60. Centre 0.7 m to the side at y 10.9 (axis distance 700 mm, surface 400): 40. 1.5 m to
  the side (surface 1200): 0. Above the head, centre y 10 + 1.8 + 0.6 m (dy = 0.6 + 0.3 = 900, surface 600): 30.
- `Combat.ISqrt64IsTheFloorRoot`: 0, 1, 2, 3, 4, 2^62 - 1, (2^31 - 1)^2 and that value - 1.
- `Combat.FallDamageStartsAtFourMetres`: -8.85 m/s -> 0; -12.53 -> 40; -17.72 -> 97; -30 -> 232.
- `Combat.FriendlyFireAndSelfDamageAreOff`: same team source -> dealt 0; `e.source == targetPlayer` -> 0.
- `Combat.ADeathCountsOnceAndCreditsTheBlastSource`: 60 + 60 at ticks 10, 11: second kills (`dealt 40`, `killed`,
  `credit` = source); a third event: `{}`; `deaths == 1`; `source.hits == 2`.
- `Combat.EnvironmentDeathCreditsTheLastEnemyWithinFiveSeconds`: blast 30 at tick 100, fall 100 at 399 -> credit the
  attacker; the same at tick 401 -> credit 0.
- `Combat.ChargesComeBackOnePer72Ticks`: spend 3 at tick 0; `TickCharges` each tick: charges 1 at 72, 2 at 144, 3 at
  216, then `nextChargeTick == 0`; `SpendCharge` on 0 charges is false; maxCharges 0: always true.
- `Combat.SpawnProtectionEndsAtTheFirstCast`: `StartLife` at 0; damage at 50 -> 0; `SpendCharge` at 60; damage at 61
  -> taken.
- `Combat.TheRoundRunsWarmupCountdownLiveEnded`: minPlayers 2, teams {1, 1}: Countdown at once with `startLives`;
  Live at +300; `ScoreKill` x scoreLimit for team 2 -> Ended, winner 2, `Score`; `sessionOver` at +600.
- `Combat.ATiedRoundGoesToOvertimeThenTheNextKillWins` and `...ThenADrawWhenOvertimeRunsOut`.
- `Combat.AnEmptyTeamForfeitsAfter1800Ticks`.
- `Combat.AimAnglesInvertLookDirection`: 64 directions (yaw 0..315 by 45 degrees x 8 pitches -80..80):
  `dot(LookDirection(yaw, pitch), dir) > cos(0.1 degree)`.
- `Rules.ABlastDamagesEnemiesInRangeOnly` (`BlastTests.cpp`, its `Arena`): players 1 (team 1, the caster), 2 (team 2,
  feet at the sphere's centre), 3 (team 1, same place), 4 (team 2, 5 m away), 5 (team 2, `alive = false`, at the
  centre); `RulesContext{tuning, views, 0, {60, 1200}}`: `damage` = exactly `{2, 1, 60, Blast}`; with centre 0: empty.

**Checks** build; `tools/ctest.sh -R 'Combat\.|Rules\.' --output-on-failure`; `tools/ctest.sh`; doc-check.
**Must not break**: `Rules.TheBlastSlotGivesTheSameOperationsAsTheOldPath` (no damage when centre is 0).

## Card 4: wire, protocol 11

| | |
|---|---|
| Goal | The fight travels: Welcome fields, owner block, remote record, Roster, RoundUpdate, KillNotice, two rejections |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | Net (`Messages`, `Session`). Wire: **yes**, `kProtocolVersion` 11 |

**Read first**: `Messages.cppm` / `internal/Messages.cpp`: the `Welcome`, `Snapshot` and `EntityList` encoders and
decoders; `engine/docs/notes/net.md` section 2 (the registry) and section 4 "Recipes" (new message).

**Steps** (little endian, as every message)
1. `MessageType`: `Roster = 21`, `RoundUpdate = 22`, `KillNotice = 23`. `CastRejection`: `NoCharge = 4`, `Dead = 5`,
   `kMaxCastRejection = 5`; `ToString`: `"no charge"`, `"dead"`.
2. `Welcome`, after `probes`: `fight.mode u8` (<= `kMaxGameMode`), `maxHealth u8` (>= 1), `maxCharges u8`,
   `chargeTicks u16`, `respawnTicks u16` = 7 B.
3. Owner block, after `castRejected`: `health, charges, chargeTicks, flags, respawnTicks, lifeSeq, hits`, u8 each
   (`kSnapshotOwnerBytes` 39). The server clamps `chargeTicks` and `respawnTicks` (ticks left) to 255. Decoder
   refuses `flags > 7`. A snapshot without an owner (`hasOwner` false) carries no fight bytes, as today.
4. `RemotePlayer` after `flags`: `fight u8` (`kSnapshotPlayerBytes` 17).
5. `Roster` (22 + ...): `type u8, count u8`, then per entry `slot u8, team u8, kills u16, deaths u16` (6 B). Decoder
   refuses count > 64, team > 2, duplicate slots, trailing or missing bytes.
6. `RoundUpdate`: `type, mode, phase, phaseEndTick u32, score1 u16, score2 u16, scoreLimit u16, winner u8, end u8,
   overtime u8` = 16 B. Refuses mode > 2, phase > 3, winner not in {0, 1, 2, 255}, end > 4, overtime > 1.
7. `KillNotice`: `type, tick u32, victim u8, killer u8, cause u8, flags u8` = 9 B. Refuses cause 0 or > 5, flags > 1,
   victim == killer when killer != 0 (killer is slot + 1, victim is slot).
8. `kProtocolVersion = 11`, comment line `11: [wp-7] (fight: Welcome fight fields, owner block +7, remote record +1,
   Roster 21, RoundUpdate 22, KillNotice 23, CastRejection NoCharge 4 / Dead 5).`
9. Session keys (`ParseSession` / `FormatSession`): `mode`, `team_spawn_1`, `team_spawn_2`, `score_limit`,
   `round_seconds`, `respawn_ticks`, `charge_ticks`, `max_charges`, `min_players`, `countdown_ticks`, `end_ticks`,
   `tower_1`, `tower_2`, `end_session`. Errors: mode not sandbox without both team spawns; a spawn or tower less than
   4 m inside the play bounds; felling without both towers; `max_charges` > 9; `charge_ticks` 0 while
   `max_charges` > 0; `round_seconds` 0 or > 3600. Sandbox: `maxCharges = 0`, blast centre 0 (applied by the parser
   whatever other keys say). `FormatSession` writes the fight keys only for a non-sandbox mode.
10. Registry, notes/net.md section 2: the values of steps 1-7, protocol 11, `DamageCause` 1-5 (5 reserved for
    `[si-2]`), `GameMode` 0-2, `RoundPhase` 0-3, `RoundEnd` 0-4, winner 255 = draw.

**Tests**
- `FightWire.MessagesRoundTripAndRefuseBadValues` (`FightTests.cpp`): each new message round-trips equal; each
  refusal of steps 5-7 refused; one trailing and one missing byte refused per message.
- `FightWire.SizesAreAsDesigned`: owner block 39 B, remote record 17 B, Welcome grows by 7 B, `Roster` with 4 players
  = 26 B, `RoundUpdate` 16 B, `KillNotice` 9 B.
- `SessionFile.FightKeysRoundTrip` (the file holding `SessionFile.*`): a deathmatch config with every key formats and
  parses back equal; the errors of step 9 each refused with the key named.
- Extend `Wire.MessagesRoundTripAndRefuseTrailingOrMissingBytes` with one of each new message.

**Checks** build; `tools/ctest.sh -R 'Wire\.|FightWire\.|SessionFile\.|Session\.' --output-on-failure`;
`tools/ctest.sh`; doc-check (it checks the protocol number in the docs); `VERIFY_SLOT=1 tools/verify.sh quick --only
G11,G15,G16` (sandbox sessions unchanged in behaviour).

**Must not break**: CLAUDE.md "Sessions" (any byte change bumps the protocol once: here). The `Welcome` keeps
`RegionDesc` first (`Session.ClientBuildsItsWorldFromWelcomeAndRefusesAnyOther`).

## Card 5: the server fights

| | |
|---|---|
| Goal | A session server with `mode = deathmatch` runs health, damage, death, respawn, charges, teams and the round |
| Depends on | cards 3, 4 |
| Effort | 1.5 d (e) |
| Touches | Net (server). Wire: as card 4. Movement rule: no |

**Read first**: `ServerImpl.cppm` (`Impl` members, `Player`, `Peer`), `ServerPeers.cpp` `OnHello`, `StepPlayers`,
`SendSnapshots`, `DropPlayer`; `ServerCasts.cpp` `CaptureCast`, `RunCast`; `Server.cpp` `Server::Start` and the tick
function; `Server::TeleportPlayer`.

**Files**: new `engine/src/net/internal/ServerFight.cpp`; change `ServerImpl.cppm`, `ServerPeers.cpp`,
`ServerCasts.cpp`, `Server.cpp`, `Server.cppm` (`ServerStats` fight counters).

**Interfaces** (in `struct Server::Impl`)
```cpp
std::array<sim::FighterState, 256> fighters{};      // by slot
sim::Round round;
std::array<uint32_t, 2> teamSpawnCount{};
void FightJoin(uint8_t slot);                        // team, Roster to all, RoundUpdate + Roster to the joiner
void FightLeave(uint8_t slot);
void DealDamage(const sim::DamageEvent& event);      // decision 16: the one damage entry
void EnvironmentDamage(uint8_t slot, const sim::PlayerState& before, const sim::PlayerState& after);
void Respawn(uint8_t slot);
void StepFight();                                    // charges, respawns, round; after RunCasts
std::vector<sim::PlayerView> PlayerViews() const;    // slot ascending, joined players
void SendRoster();  void SendRound();  void SendKill(const net::KillNotice& kill);
```
`ServerStats`: `uint32_t kills, deaths, damageEvents, damageTotal, castsRefusedDead, castsRefusedNoCharge, respawns;`

**Steps**
1. Sandbox (`!settings.session.fight.Active()`): `FightJoin` sets `spawned = true`, team 0; nothing else of this
   card runs; casts are never refused for charges; the owner block sends health 100, charges 0, flags 0. Today's
   behaviour exactly.
2. `FightJoin` (called from `OnHello` after the Welcome is queued): team = `PickTeam`; `spawned = false` (a
   spectator until its first spawn); send this peer a `RoundUpdate` and the `Roster`, then the `Roster` to every
   other peer. Log `Fight: slot S joins team T`. If the round is `Live` or `Countdown`, `Respawn(slot)` at once.
3. Per tick, in this order (`Server.cpp` tick function; write it down in a comment "Fight order per tick"):
   (a) `StepPlayers`: a fighter with `dead` or `!spawned` is **not** stepped; its input is consumed and acknowledged;
   a cast edge in it (`(buttons & Cast) && !(lastButtons & Cast)`) records `lastCastTick = input.tick`,
   `castRejected = Dead`, `++castsRefusedDead`; `state.lastButtons = input.buttons`. A stepped player: keep `before`,
   step as today, then `EnvironmentDamage(slot, before, after)` when the round is Live.
   (b) `CaptureCast` (inside the step's cast edge): Live or Countdown or Ended in a fight mode:
   `!sim::SpendCharge(f, rules, tick)` -> reject `NoCharge` (`++castsRefusedNoCharge`), not queued.
   (c) `RunCasts` -> `RunCast`: `RulesContext{tuning, PlayerViews(), tick, round.phase == Live ? rules.blast :
   BlastDamageRule{}}`; after the cast's ops are emitted, `DealDamage` for each `outcome.damage` in order.
   (d) `StepFight()`: per slot ascending: living fighters `TickCharges`; dead with `tick >= respawnTick` -> `Respawn`;
   then `StepRound(round, rules, tick, players per team, felled)` (felled from card 9, `{false, false}` until then);
   `startLives` -> every joined fighter `Respawn`, kills / deaths 0, Roster; `changed` -> `SendRound()`;
   `sessionOver` -> step 7.
   (e) `SendSnapshots` fills the owner block from `fighters[slot]` and `RemotePlayer::fight`.
4. `DealDamage(e)`: target = `fighters[e.target - 1]`, source = `e.source ? &fighters[e.source - 1] : nullptr`;
   only in Live; `ApplyDamage`; `++damageEvents`, `damageTotal += dealt`. `killed`: `++deaths`; credit != 0 ->
   `fighters[credit - 1].kills += 1`, `++kills`, `ScoreKill(round, rules, team of credit, tick)`; `SendKill`;
   `SendRoster`; `SendRound` when the score changed. The victim's `PlayerState.velocity = 0`.
5. `EnvironmentDamage`: decision 8. Fall: `!(before.flags & OnGround) && (after.flags & OnGround)` ->
   `FallDamage(rules, before.velocity.y)`. Lava: `tick >= nextLavaTick` and lava at either probe voxel
   (`World::GetVoxel` of `World::WorldToVoxel`) -> damage, `nextLavaTick = tick + lavaEveryTicks`. Void: feet y <
   `region.heightMin + 2` m. Each through `DealDamage` with source 0 and its cause.
6. `Respawn(slot)`: `k = teamSpawnCount[team - 1]++`; feet = `TeamSpawnFeet(...)`; `StartLife`; the player's state as
   decision 7 (the same path as `Server::TeleportPlayer`: `SetCharacterState` then `FlushBox(PlayerReach(feet))`);
   `++respawns`. Log `Fight: slot S spawns at (x, y, z) life L`.
7. Session over (`end_session`): log `Round: session over at tick T`; `SendRound()`, flush, disconnect every peer;
   `Server::SessionOver()` (new, `bool`) becomes true; `vortex_server` then stops its loop (card 7).
8. `FightLeave` (from `DropPlayer`): fighter reset to `{}`; Roster to the others.

**Log lines** (contract once G22 reads them; docs/verify.md lists them in card 7)
- `Round: phase <Warmup|Countdown|Live|Ended> at tick T (players A + B)`
- `Kill: tick T slot V (team X) by <Blast|Fall|Lava|Void|Crush>, credit slot K` (`credit none` when 0)
- `Round: team N wins A-B by <score|time|felled|forfeit> at tick T (<deathmatch|felling>)` or
  `Round: draw A-B at tick T (<mode>)`
- At exit / session over: `Fight: kills K, deaths D, damage events E, damage H hp, refused dead X, no charge Y,
  respawns R`

**Tests** (`FightTests.cpp`, a loopback server with a deathmatch config on the `Arena`-style flat session the other
net tests use (copy the helper of `MovementTests.cpp`, rename it `FightField`), clients stepped with explicit inputs)
- `Fight.TeamsAreBalancedAtJoin`: 3 joins -> teams 1, 2, 1; the Roster each client holds lists all three.
- `Fight.ABlastKillsAndTheVictimRespawnsAtItsTeamSpawnAfter180Ticks`: minPlayers 2, countdown 1; teleport the victim
  1 m in front of the attacker's aim point; two casts 20 ticks apart aimed at the victim's feet: the second kills
  (`Kill:` counted; killer kills 1; score 1); the victim's owner block says dead with `respawnTicks` counting down; at
  `+180` it is alive, health 100, `lifeSeq` 2 (1 at the first spawn), feet within 2.1 m (x / z) of team 2's spawn.
- `Fight.ADeadPlayerIsNotSteppedAndItsCastsAreRefusedDead`: while dead, inputs with move and jump leave its position
  equal bit for bit; a cast edge gives `castRejected == Dead`.
- `Fight.CastsWithoutAChargeAreRefusedNoCharge`: 4 cast edges 15 ticks apart: 3 run, the 4th `NoCharge`; one more
  at +72 ticks after the first spend runs.
- `Fight.ARoundEndsAtTheScoreLimit`: score_limit 2: two kills -> `Round: team 1 wins 2-0 by score`; RoundUpdate on
  every client says Ended, winner 1; with `end_session` the server reports `SessionOver()` after `end_ticks`.
- `Fight.SandboxSessionsKeepTodaysCasts`: playground-like sandbox field, 10 cast edges 15 ticks apart: 10 run, no
  damage, owner block charges 0, flags 0.
- `Fight.FallsAndLavaHurtOnlyInALiveRound`: drop a player from 10 m (teleport) in Warmup: health 100; in Live: health
  `100 - FallDamage(-sqrt(2 * 9.81 * 10))` within 1 (Jolt's landing speed). Lava: a `FillBox` of lava under the feet:
  20 at once, 40 after 15 ticks.

**Checks** build; `tools/ctest.sh -R 'Fight\.|Movement\.|Session\.|Entities\.' --output-on-failure`;
`tools/ctest.sh`; doc-check; `VERIFY_SLOT=1 tools/verify.sh quick --only G11,G15,G16`.

**Must not break**: "One movement rule" (the dead are skipped, never moved another way; respawn is a teleport like
`TeleportPlayer`); `Movement.NoCorrectionAbove2cmOnFlatGround` at 0; "Replicated bodies" (damage reads only player
positions); `CastBudget.CastWorkIsEqualColdAndWarm` (damage is outside `CastWorkOf`). L9: nothing here spawns bodies.

**Traps**: `PeerOfSlot` / `lastCastTick` must stay the same comparison for `NoCharge` and `Dead` as for the old
rejections. `fighters` is by slot, `DamageEvent::target` is slot + 1. Damage only in Live: Countdown and Ended carve
but never hurt.

## Card 6: the client's fight state

| | |
|---|---|
| Goal | A client shows the fight correctly: death stops prediction, respawn is not a correction, roster / round / kills are readable |
| Depends on | card 5 |
| Effort | 1 d (e) |
| Touches | Net (client). Wire: no (decoded in card 4) |

**Interfaces** (`Client.cppm`)
```cpp
struct FightView {
    sim::GameMode mode = sim::GameMode::Sandbox;
    uint8_t health = 100, maxHealth = 100, charges = 0, maxCharges = 0, chargeTicks = 0;
    uint16_t chargeTicksFull = 0;           ///< Welcome chargeTicks (for the pip's fill)
    bool dead = false, protectedNow = false, spectator = false;
    uint8_t respawnTicks = 0, lifeSeq = 0, hits = 0;
};
const FightView& Fight() const;
const Roster& RosterOf() const;
const RoundUpdate& RoundOf() const;
std::vector<KillNotice> TakeKillNotices();   ///< the notices since the last call, in arrival order (at most 32 kept)
```
`RemotePlayerView` gains `uint8_t fight` (copied from the snapshot record). `PredictionStats` gains
`uint64_t respawns = 0, deathsSeen = 0;`.

**Steps**
1. Dispatch `Roster`, `RoundUpdate`, `KillNotice` on receipt (not by opSeq); keep the newest; kill notices into a
   deque capped at 32 (oldest dropped).
2. Each accepted snapshot with an owner: copy the fight bytes into `FightView` (the tick of the newest snapshot
   wins; an older snapshot never overwrites).
3. Death: the first snapshot with `kDead` while `m_Fight.dead` was false: `++deathsSeen`; the predicted state becomes
   the snapshot's owner state; the history is cleared; log `Client: died at tick T (life L)`. While dead, `RunTick`
   records and sends the input as today but does not call `StepPlayer`; the predicted state stays the owner state
   of the newest snapshot. Reconcile compares nothing while dead.
4. Respawn: a snapshot whose `lifeSeq` differs from the last seen: `++respawns`; state = owner state of that
   snapshot at its tick S; replay the history's inputs after S with `StepPlayer` exactly as `Reconcile` replays (the
   same `FlushBox` calls), but count no correction, no snap and blend nothing (the camera cuts); log
   `Client: respawned at tick S (life L)`. Then normal prediction.
5. `castsRejected` counts `NoCharge` and `Dead` like the others.

**Tests**
- `Fight.ARespawnIsNotACorrection`: the card 5 kill scenario with prediction on both clients: the victim's
  `Prediction().corrections` and snaps are unchanged across death and respawn; `respawns == 1`, `deathsSeen == 1`;
  after the respawn its predicted feet equal the server's within 2 cm at the next compared snapshot.
- `Fight.TheClientSeesRosterRoundAndKills`: after the kill: `RosterOf()` lists the killer with kills 1;
  `RoundOf().score` = {1, 0} (or {0, 1}); `TakeKillNotices()` returns one notice with cause `Blast` and the killer's
  slot + 1, and an empty vector on the next call.
- `Fight.RemoteDeadPlayersCarryTheDeadBit`: the attacker's view of the victim has bit 7 set while dead.

**Checks** build; `tools/ctest.sh -R 'Fight\.|Client\.|Movement\.' --output-on-failure`; `tools/ctest.sh`;
doc-check; `VERIFY_SLOT=1 tools/verify.sh quick --only G15,G16` (session-check: 0 snaps, corrections within bound).

**Must not break**: "Movement" (client predicts only through `StepPlayer`, `FlushBox` before replays); "Chunk sync"
(events still by opSeq; the three messages are not stream events).

## Card 7: bots that fight, and G22

| | |
|---|---|
| Goal | Bots fight in a match; a 2v2 bot match ends with a winner in `verify.sh full` |
| Depends on | card 6 |
| Effort | 1 d (e) |
| Touches | Net (`Drill`, `Bot`), `vortex_server`, `vortex_bot`, `tools/verify.sh`, `tools/lib/verify-gates.sh`, fixtures, docs |

**Interfaces** (`Drill.cppm`)
```cpp
// PilotSettings:
bool fight = false;               ///< aim and cast at enemies (PilotTarget) instead of the ground ahead
float fightRange = 30.0f;         ///< metres: casts only at targets this near
float keepDistance = 12.0f;       ///< metres: closes in beyond +4, backs off under -4, strafes between
uint32_t strafeTicks = 60;        ///< ticks per strafe direction
uint32_t reactionTicks = 20;      ///< ticks between seeing a new target and the first cast at it
struct PilotTarget { uint8_t slot = 0; glm::vec3 feet{0.0f}; };
// BotPilot:
void SetTargets(std::span<const PilotTarget> enemies);   ///< living enemies, slot ascending; before each Next
void SetCanCast(bool canCast);                           ///< owner block: alive, a charge, cooldown 0
uint32_t FightCasts() const;
```

**Steps**
1. `net::Bot::Pass`: before each `Next`, when `settings.pilot.fight`: targets = `RemotePlayers()` whose roster team
   differs from its own and is not 0, and whose `fight` bit 7 is clear; `SetCanCast(!Fight().dead && (charges > 0 ||
   maxCharges == 0) && Player().cooldownTicks == 0)`. A dead bot sends neutral inputs.
2. `BotPilot::Next` with `fight` and at least one target: target = the nearest (x / z distance, ties: lower slot);
   kept until it leaves the list, then the next nearest (`reactionTicks` restart). Movement: distance `d`; `d >
   keepDistance + 4` -> run toward it; `d < keepDistance - 4` -> walk away; else strafe at right angles, direction
   flipped every `strafeTicks` (first direction: seed & 1). Jumps as today. Inside the play bounds as today
   (`edgeMargin`). Aim: `dir = normalize(target.feet + (0, 0.1, 0) - eye)`, `AimAngles(dir, yaw, pitch)`. Cast:
   when `canCast`, `d <= fightRange`, the target is held `>= reactionTicks`, and the Cast bit was up the tick before:
   set Cast for one tick (an edge). No target: today's behaviour.
3. Line of sight is not checked (the blast carves cover; fun first, and the bot stays cheap).
4. `vortex_server`: `--fight` (every in-process bot gets `pilot.fight = true`), `--until-round-end` (run until
   `Server::SessionOver()`, then drain and compare as after `--ticks`; `--ticks N` stays the cap: not over by then ->
   `Round: not over after N ticks`, exit 1), `--require-winner` (a draw -> exit 1). `vortex_bot --fight`.
5. `engine/tools/server/sessions/duel.session`: `playground.session` with `mode = deathmatch`, `team_spawn_1 = -30 0`,
   `team_spawn_2 = 30 0`, `min_players = 4`, `score_limit = 5`, `round_seconds = 300`, `countdown_ticks = 120`,
   `end_ticks = 120`, a comment block like the playground's.
6. Gate **G22** (`tools/verify.sh`, tier `full`, no UDP port): `build/engine/tools/server/vortex_server
   --session=engine/tools/server/sessions/duel.session --clients 4 --fight --until-round-end --require-winner
   --ticks 30000 --loss 0.05 --delay-ms 40 --jitter-ms 30`. Judge (`gate_G22` in `tools/lib/verify-gates.sh`): exit 0;
   `^Round: team [12] wins`; `hashes equal`; four `Entities: client N: ... agree`; no `Checks:` mismatch line;
   `Fight: kills K` with K >= 5. A planted fault per check in the judge's self-test with fixtures
   `tools/tests/fixtures/G22-pass.log`, `G22-draw.log`, `G22-nokills.log` (`Tools.VerifyAssertions`).
7. CLAUDE.md section 2: the G22 row (`Gate | Command | Must` as above). docs/verify.md: the `Round:`, `Kill:`,
   `Fight:` lines and G22. `tools/verify.sh --list` names G22 (doc-check compares).

**Tests**
- `Bot.FightPilotAimsAtTheNearestEnemyAndCastsOnEdges` (`BotTests.cpp`): a pilot with two targets at 10 m and 20 m:
  its yaw / pitch point at the nearer within 1 degree; no cast before `reactionTicks`; then Cast set on one tick,
  clear on the next; `SetCanCast(false)`: no cast.
- `Fight.ATwoVersusTwoBotMatchEndsWithAWinner` (loopback, no impairment, score_limit 2, round 120 s; label it `slow`
  if `engine/tests/CMakeLists.txt` has the label): a winner, `hashes equal`. Record its run time in the report.

**Checks** build; `tools/ctest.sh -R 'Bot\.|Fight\.|Tools\.' --output-on-failure`; `tools/ctest.sh`; doc-check;
the G22 command by hand (record ticks to the win and wall time); `VERIFY_SLOT=1 tools/verify.sh quick --only G11`.

**Numbers**: ticks to the winner, kills, `Fight:` line, wall seconds of G22.
**Traps**: the bots must be deterministic: no `std::random_device`, iterate remote players by slot. G22 is `full`
tier only (CLAUDE.md gate table and `verify.sh` tier lists).

## Card 8: the HUD

| | |
|---|---|
| Goal | Players see health, charges, hits, kills, the round and the scoreboard; teams have colours |
| Depends on | card 6 |
| Effort | 1 d (e) |
| Touches | game (`src/Session.cpp`, a new `src/FightHud.cpp` / `src/FightHud.h`). Engine: none. Wire: no |

**Read first**: `src/Session.cpp` `DrawHud`, the figure model and `m_Remotes`; `engine/src/core/Hud.cppm`
(`HudCanvas`, `PlaceRect`, `Anchor`, `BitmapFont::kLineHeight`, `DimBackground`); magic.md "Input and HUD" (the pips).

**Interfaces** (`src/FightHud.h`, plain game code)
```cpp
struct FightHudInput {
    const net::FightView& fight; const net::Roster& roster; const net::RoundUpdate& round;
    uint8_t ownSlot; uint32_t serverTick; float tickSeconds; bool scoreboard; uint64_t frame;
};
class FightHud {
public:
    void AddKills(std::vector<net::KillNotice> notices, uint64_t frame);   // kept 6 s, at most 5 shown
    void NoteHits(uint8_t hits, uint64_t frame);                            // hit marker for 12 frames on a change
    void Draw(HudCanvas& hud, const FightHudInput& in);
};
Rgba TeamColour(sim::TeamId team);                                          // decision 19; team 0 gold as today
```

**Steps** (sizes at 720p times `TextScale`; nothing drawn in sandbox mode except as today)
1. Health: bottom centre, a 200 x 14 px bar (backdrop `Rgba(10, 12, 16, 150)`), fill green above 60, yellow above
   30, red below; the number at its right. Protected: the bar outlined white.
2. Charges: `maxCharges` pips of 6 x 6 px, 4 px apart, 18 px under the crosshair; full white, empty 30 % white, the
   next one filled from the bottom by `1 - chargeTicks / chargeTicksFull`.
3. Hit marker: four 6 px diagonal strokes around the crosshair (as `Rect`s) while active.
4. Kill feed: top right under the fps line; `KILLER > VICTIM  CAUSE` (cause words `BLAST`, `FALL`, `LAVA`, `VOID`,
   `CRUSHED`), names in team colours, `>` grey; without a killer: `VICTIM  CAUSE`.
5. Round banner: top centre: `BLUE 3 : 2 RED   7:42` (time = `(phaseEndTick - serverTick) * tickSeconds`, m:ss,
   floor); Warmup: `WAITING FOR PLAYERS A/B`; Countdown: `STARTS IN N` (ceil seconds) at scale 2; overtime:
   `OVERTIME` under the score; Ended: centre, scale 3, `BLUE WINS` / `RED WINS` / `DRAW`, the reason word under it.
6. Scoreboard (Tab held, or `--scoreboard`): centre table, two columns by team, rows `P3   K 4   D 2`, own row `YOU`
   highlighted; backdrop through `DimBackground` (L6: no full-screen blended quad).
7. Death screen: `DimBackground`; centre `KILLED BY P2 (BLAST)` (from the last notice naming the own slot as victim)
   and `RESPAWN IN 2.4`; the crosshair and pips hidden.
8. `Cast not run:` keeps its line; `NoCharge` / `Dead` show `NO CHARGE` / `DEAD` (mag-2 adds words here).
9. Figures: one figure model per team colour (built once, like today's); a remote's entity uses its roster team's
   model; bit 7 set -> the entity is destroyed, recreated when clear. The own figure (third person) too.
10. Exit line (session and practice clients): `Fight: slot S team T health H kills K deaths D, round <phase> A-B`.

**Checks** build; `VERIFY_SLOT=1 tools/verify.sh quick --only G3,G15` (G3 sandbox HUD unchanged: look at the
screenshot); after card 10 the practice screenshot shows the fight HUD. Owner checks (add to
`docs/owner-checks.md`): the HUD read in a windowed practice round; Tab scoreboard; team colours.
**Traps**: no ImGui; HUD colours are display values; text width through `Measure`.

## Card 9: the Felling duel

| | |
|---|---|
| Goal | `mode = felling`: each team has a tower; releasing the enemy's crown wins at once |
| Depends on | card 5 |
| Effort | 1 d (e) |
| Touches | Sim (`Combat.cpp`), Net (server start, `StepFight`). Wire: no (towers are `FillBox` operations of the stream) |

**Steps**
1. `BuildFellingTower(world, m, at, voxelSize, topVoxelY)`, all in voxels, `n(metres) = llround(metres / voxelSize)`,
   `(cx, cz) = WorldToVoxel(at)`, ground `g = topVoxelY` (the server finds it with a downward ray at `at`):
   base `Box([cx - n(2), g - n(1), cz - n(2)], [cx + n(2), g + n(0.5), cz + n(2)], m.stone)`; shaft
   `Box([cx - n(1), g + n(0.5), cz - n(1)], [cx + n(1), g + n(10.5), cz + n(1)], m.brick)`; crown
   `Box([cx - n(1.75), g + n(10.5), cz - n(1.75)], [cx + n(1.75), g + n(13), cz + n(1.75)], m.brick)`; crown box =
   the crown's box; `crownVoxels` = its voxel count.
2. Server start (felling): after the template's ops, each tower's ops are applied and emitted like template ops
   (`EmitOp`), team 1's then team 2's. Log `Felling: tower T at (x, z), crown V voxels`.
3. `StepFight`, Live, every 30 ticks (`tick % 30 == 0`): `CountSolid` of each crown on the server world; team i's
   crown is felled when `count * 5 < crownVoxels` (under 20 %). Felled feeds `StepRound`'s `felled`.
4. `engine/tools/server/sessions/felling.session`: as duel.session with `mode = felling`, `tower_1 = -38 0`,
   `tower_2 = 38 0`, `min_players = 2`, `score_limit = 15`, `round_seconds = 480`.

**Tests** (`CombatTests.cpp`, the sim `Arena` with flat ground, voxel 0.125 m)
- `Felling.TheTowerStandsWhenABlastHits6MetresAway`: apply the ops; one `sim::Blast` 6 m from the shaft: no
  `Detach` touches the tower box; `CountSolid(crown) == crownVoxels`.
- `Felling.CuttingTheShaftFellsTheCrown`: carve spheres of 1.2 m around the shaft at 3 m height on all four sides
  (`sim::Blast` each, load check on): after them `CountSolid(crown) * 5 < crownVoxels`.
- `Felling.TheRoundEndsWhenACrownFalls`: `StepRound` with `felled = {true, false}` in Live: winner 2, `Felled`.

**Checks** build; `tools/ctest.sh -R 'Felling\.|Combat\.|Neck\.|Structure\.' --output-on-failure`; `tools/ctest.sh`;
doc-check; a run `vortex_server --session=engine/tools/server/sessions/felling.session --clients 2 --ticks 600`
exits 0 with two `Felling: tower` lines and `hashes equal`.
**Must not break**: the load check never runs after `FillBox` ("World edits and blasts"); `Neck.*`; the towers are
stream operations, so joiners get them through chunk sync.

## Card 10: offline practice through an in-process server

| | |
|---|---|
| Goal | `Shredborne --practice [--bots N] [--mode deathmatch|felling]` plays a round against bots, offline |
| Depends on | cards 7, 8 (9 for `--mode felling`) |
| Effort | 1.5 d (e) |
| Touches | game (`src/Session.cpp`, new `src/Practice.cpp`, `src/main.cpp`). Engine: none |

**Read first**: `src/Session.cpp` `RunSession` and `SessionGame`'s use of `m_Transport`; `engine/src/net/Transport.cppm`
`LoopbackTransport` (`Connect`; main thread only); `engine/tools/server/main.cpp` where in-process bots are made
(`net::Bot`, `Pass`).

**Steps**
1. `SessionGame` takes `net::ITransport&` plus `std::function<double()> roundTripSeconds` (ENet: the old
   `SmoothedRoundTripSeconds(1)`; practice: `0`) and an optional `std::function<void(double)> host` called first in
   every update with the frame's seconds.
2. `src/Practice.cpp`: `struct InProcessServer { net::LoopbackTransport serverEnd, clientEnd; std::vector<std::unique_ptr<net::LoopbackTransport>> botEnds; std::unique_ptr<net::Server> server; std::vector<std::unique_ptr<net::Bot>> bots; }`.
   `PracticeSession(mode, bots)`: the playground region of `playground.session` written in code (template
   playground, seed 1337, play -12..12, height -48 128, voxel 0.125, generator terrain, terrain seed 1337), a random
   token from `std::random_device` (not a rule: only the loopback sees it), the duel spawns, `min_players = 1 + bots`,
   `score_limit = 10`, `round_seconds = 480`, `end_session = 1`; felling adds the towers of felling.session.
3. `RunPractice(args, ...)`: build the server (`Start()`; log `Practice: server ready in X s, N bot(s)`), connect
   the client end first (slot 0, team 1), then the bots (pilot `fight = true`, seeds `0xb07 + i`); host function =
   `server.Advance(seconds)`, then each `bot->Pass(seconds, true)`. `--bots` 0-3 (default 1; more refused with exit 2).
4. Round over (the server's `SessionOver()`): the HUD shows the result; `Enter` (or, headless, nothing) starts a new
   practice (tear down, build again; log `Practice: new round`); Esc -> the pause menu as today.
5. `main.cpp`: `--practice` dispatches to `RunPractice` before the sandbox; excludes `--connect` / `--join` (exit 2).
6. README "Controls" and the game's docs: `--practice`, `--bots`, `--mode`, Tab.

**Checks** build; `./build/Shredborne --headless --practice --bots 1 --frames 1500 --screenshot build/practice.png
--validation` exits 0, logs `Practice: server ready`, `Fight: slot 0 team 1 ...` and `HUD: N quads` with N > 0
(look at the screenshot: banner, health, pips); the same with `--scoreboard` (the table); `VERIFY_SLOT=1
tools/verify.sh quick --only G3,G15` (sandbox and `--connect` unchanged).
**Numbers**: server ready seconds, RSS (`/proc/self/status` VmRSS at exit, logged by the practice), CPU frame p50 /
p99 from the `Perf:` line with 1 and with 3 bots.
**Owner checks** (docs/owner-checks.md): a windowed practice round with 3 bots to the end; the taste check question
(review WP7): "Does the fight feel right? Too fast, too slow, too lethal?".
**Traps**: transports are main-thread only: no server thread. Every `Bot` has its own world: 3 bots are 4 client
worlds + the server's; measure before raising the default.

## Card 11: close [wp-7]

| | |
|---|---|
| Goal | The stage is verified and its documents tell the truth |
| Depends on | cards 1-10 |
| Effort | 0.5 d (e) |

**Steps**
1. `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage.
2. `VERIFY_SLOT=1 tools/verify.sh full --ci` (G22 included) and `VERIFY_SLOT=1 tools/verify.sh soak` in the
   background: both PASS (the batch may run these once at its end instead; say which).
3. fight.md "Stages": `[wp-7]` **Built** with the commits; ROADMAP "Now" / "Next" (M2a built, [wp-8] next), "Playable
   today" (practice, fights), "Not a game yet" (drop health, damage, death, respawn, teams, rounds, score), the test
   count; networking.md header "Next" (the next protocol change is [mech-1]'s, 12); game-ui.md; CLAUDE.md section 5
   (a "Fight" paragraph: server-only rules, the damage entry `DealDamage`, `lifeSeq` respawn is not a correction,
   `kSnapshotOwnerBytes` 39, with its tests) and section 2 (G22).
4. Report: the numbers of cards 7 and 10; owner checks added; the Windows client must be rebuilt (protocol 11).

## Values this stage lands

notes/net.md section 2 and llm/README "Values planned across the packs":
- `net::kProtocolVersion` 11.
- `MessageType` 21 `Roster`, 22 `RoundUpdate`, 23 `KillNotice` (server -> client, events channel). `[mag-2]`'s
  `Attune` = 24.
- `CastRejection` 4 `NoCharge`, 5 `Dead`; `kMaxCastRejection` 5.
- `DamageCause` 1 `Blast`, 2 `Fall`, 3 `Lava`, 4 `Void`, 5 `Crush` (reserved for `[si-2]`).
- `GameMode` 0 `Sandbox`, 1 `Deathmatch`, 2 `Felling`; `RoundPhase` 0-3; `RoundEnd` 0-4; winner 255 = draw.
- `kSnapshotOwnerBytes` 39 (`[mech-1]` adds 5 -> 44), `kSnapshotPlayerBytes` 17 (`[mag-2]` adds 1), `Welcome` +7 B.
- No `PlayerButton`, `EventKind`, `WorldOpKind` or `EntityKind`; `kPlayerRuleRevision` stays 1; `buildHash`
  unchanged.
- Gate G22 (tier `full`).
