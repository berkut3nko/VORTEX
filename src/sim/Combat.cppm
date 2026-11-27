module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

export module vortex.sim:combat;

import vortex.voxel;
import :simulation;
import :worldop;
import :region;
import :player;
import :rules;

// docs/design/fight.md "Rules": health, damage, charges, teams and the round; server-only results. [wp-7]
export namespace vortex::sim {
    /// @brief Why a player lost health (fight.md "Damage"). Wire format (KillNotice).
    enum class DamageCause : uint8_t { None = 0, Blast = 1, Fall = 2, Lava = 3, Void = 4, Crush = 5 };
    inline constexpr uint8_t kMaxDamageCause = 5;
    /// @brief A session's game (session key `mode`). Wire format (Welcome, RoundUpdate).
    /// Siege (fight.md "Siege"): team 1 defends Hollowkeep's sanctum, team 2 attacks it.
    enum class GameMode : uint8_t { Sandbox = 0, Deathmatch = 1, Felling = 2, Siege = 3 };
    inline constexpr uint8_t kMaxGameMode = 3;
    inline constexpr TeamId kTeamCount = 2;            ///< teams 1 and 2
    inline constexpr TeamId kSiegeDefenders = 1;
    inline constexpr TeamId kSiegeAttackers = 2;

    const char* ToString(DamageCause cause);
    const char* ToString(GameMode mode);

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
        uint32_t holdTicks = 1800;                     ///< [hold_seconds] x 60: siege, the unbroken hold that wins
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
    /// @brief The rules of a sandbox session: no damage, unlimited charges, no round (what the parser sets
    /// for `mode = sandbox` whatever the other keys say).
    FightRules SandboxRules();

    /// @brief One player's fight state on the server (slot-indexed).
    struct FighterState {
        TeamId team = kNoTeam;
        int16_t health = 100;
        uint8_t charges = 3;
        uint32_t nextChargeTick = 0;                   ///< 0 = full
        bool dead = false;
        bool spawned = false;                          ///< false until its first spawn on a team
        uint32_t respawnTick = 0;                      ///< dead: the tick it respawns
        uint32_t lifeStartTick = 0;                    ///< the tick of its last spawn: an input of this tick or older is not of this life
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
    /// @brief Spends one charge (true) or refuses (false). Unlimited when maxCharges is 0. A cast ends protection.
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
    /// Held: a siege's attackers held the sanctum for FightRules::holdTicks.
    enum class RoundEnd : uint8_t { None = 0, Score = 1, Time = 2, Felled = 3, Forfeit = 4, Held = 5 };
    inline constexpr uint8_t kMaxRoundPhase = 3;
    inline constexpr uint8_t kMaxRoundEnd = 5;
    inline constexpr uint8_t kDraw = 255;              ///< Round::winner of a draw
    const char* ToString(RoundPhase phase);
    const char* ToString(RoundEnd end);
    /// @brief The round machine (decision 11). Server-only; `RoundUpdate` carries it.
    struct Round {
        RoundPhase phase = RoundPhase::Warmup;
        uint32_t phaseEndTick = 0;                     ///< 0 = open-ended (Warmup)
        std::array<uint16_t, 2> score{};
        bool overtime = false;
        uint8_t winner = 0;                            ///< 0 none, 1, 2, kDraw
        RoundEnd end = RoundEnd::None;
        uint32_t emptySince[2]{};                      ///< Live: tick a team became empty, 0 = it has players
        /// Siege, Live: the tick the attackers' unbroken hold of the sanctum began; 0 = no hold.
        uint32_t holdSince = 0;
    };
    /// @brief What StepRound changed this tick (the server sends a RoundUpdate on any).
    struct RoundStep {
        bool changed = false;
        bool startLives = false;                       ///< Countdown began: respawn everyone, reset scores
        bool sessionOver = false;                      ///< Ended ran out
    };
    /**
     * @brief One tick of the machine. `teamPlayers` = players per team; `felled[t]` = team t+1's crown released;
     * `inSanctum[t]` = living players of team t+1 whose feet are in the sanctum (siege). Siege, Live: a tick with an
     * attacker and no defender in the sanctum holds it (the hold starts at that tick); any other tick breaks the hold.
     * A hold of `holdTicks` wins for the attackers (Held); the time running out wins for the defenders (Time, no
     * overtime).
     */
    RoundStep StepRound(Round& round, const FightRules& rules, uint32_t tick, std::array<uint32_t, 2> teamPlayers,
                        std::array<bool, 2> felled, std::array<uint32_t, 2> inSanctum = {});
    /// @brief A credited kill for `team` (score, overtime end; a siege's kills only count). Returns true when it ended
    /// the round.
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
