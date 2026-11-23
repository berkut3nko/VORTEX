module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

// docs/design/fight.md "Rules", "Damage" and "Round": the fight's rules, GPU-free and network-free. The session
// server owns one FighterState per slot and one Round; the offline sandbox owns none. [wp-7]
namespace vortex::sim {

    const char* ToString(DamageCause cause) {
        switch (cause) {
            case DamageCause::None: return "None";
            case DamageCause::Blast: return "Blast";
            case DamageCause::Fall: return "Fall";
            case DamageCause::Lava: return "Lava";
            case DamageCause::Void: return "Void";
            case DamageCause::Crush: return "Crush";
        }
        return "?";
    }

    const char* ToString(GameMode mode) {
        switch (mode) {
            case GameMode::Sandbox: return "sandbox";
            case GameMode::Deathmatch: return "deathmatch";
            case GameMode::Felling: return "felling";
        }
        return "?";
    }

    const char* ToString(RoundPhase phase) {
        switch (phase) {
            case RoundPhase::Warmup: return "Warmup";
            case RoundPhase::Countdown: return "Countdown";
            case RoundPhase::Live: return "Live";
            case RoundPhase::Ended: return "Ended";
        }
        return "?";
    }

    const char* ToString(RoundEnd end) {
        switch (end) {
            case RoundEnd::None: return "none";
            case RoundEnd::Score: return "score";
            case RoundEnd::Time: return "time";
            case RoundEnd::Felled: return "felled";
            case RoundEnd::Forfeit: return "forfeit";
        }
        return "?";
    }

    FightRules SandboxRules() {
        FightRules r;
        r.mode = GameMode::Sandbox;
        r.maxCharges = 0;
        r.blast = BlastDamageRule{};
        return r;
    }

    int64_t ISqrt64(int64_t v) {
        if (v <= 0) return 0;
        int64_t r = static_cast<int64_t>(std::sqrt(static_cast<double>(v)));
        while (r > 0 && r > v / r) --r;                            // r * r > v without overflow
        while ((r + 1) <= v / (r + 1)) ++r;                        // (r + 1)^2 <= v
        return r;
    }

    int16_t BlastDamageAt(const BlastDamageRule& rule, const glm::ivec3& centreQ, float voxelSize, const glm::vec3& feet,
                          const PlayerTuning& tuning) {
        if (rule.centre <= 0 || rule.zeroMm == 0) return 0;
        const double toMm = static_cast<double>(voxelSize) * 1000.0 / 16.0;
        int64_t c[3], f[3];
        for (int a = 0; a < 3; ++a) {
            c[a] = std::llround(static_cast<double>(centreQ[a]) * toMm);
            f[a] = std::llround(static_cast<double>(feet[a]) * 1000.0);
        }
        const int64_t rMm = std::llround(static_cast<double>(tuning.radius) * 1000.0);
        const int64_t hMm = std::llround(static_cast<double>(tuning.height) * 1000.0);
        const int64_t lo = f[1] + rMm, hi = std::max(lo, f[1] + hMm - rMm);
        const int64_t dy = c[1] < lo ? lo - c[1] : c[1] > hi ? c[1] - hi : 0;
        const int64_t dx = c[0] - f[0], dz = c[2] - f[2];
        const int64_t d = ISqrt64(dx * dx + dz * dz + dy * dy);
        const int64_t s = std::max<int64_t>(0, d - rMm);
        const int64_t zero = rule.zeroMm;
        if (s >= zero) return 0;
        return static_cast<int16_t>(static_cast<int64_t>(rule.centre) * (zero - s) / zero);
    }

    int16_t FallDamage(const FightRules& rules, float velocityBeforeY) {
        const int32_t s = static_cast<int32_t>(std::floor(-velocityBeforeY * 100.0f));
        if (s <= static_cast<int32_t>(rules.fallSafeCmS)) return 0;
        const int64_t d = static_cast<int64_t>(s - rules.fallSafeCmS) * rules.fallPerMetrePerSecond / 100;
        return static_cast<int16_t>(std::min<int64_t>(32767, d));
    }

    DamageResult ApplyDamage(FighterState& target, uint8_t targetPlayer, FighterState* source, const DamageEvent& event,
                             const FightRules& rules, uint32_t tick) {
        DamageResult r;
        if (target.dead || !target.spawned || event.amount <= 0) return r;
        if (tick < target.protectedUntil) return r;
        if (source != nullptr) {
            const bool sameTeam = source->team != kNoTeam && target.team != kNoTeam && source->team == target.team;
            if (sameTeam || event.source == targetPlayer) return r;   // friendly fire and self-damage are off
            target.lastAttacker = event.source;
            target.lastAttackTick = tick;
            source->hits = static_cast<uint8_t>(source->hits + 1);
        }
        r.dealt = std::min<int16_t>(event.amount, target.health);
        target.health = static_cast<int16_t>(target.health - r.dealt);
        if (target.health == 0) {
            r.killed = true;
            target.dead = true;
            target.respawnTick = tick + rules.respawnTicks;
            target.deaths = static_cast<uint16_t>(target.deaths + 1);
            if (event.cause == static_cast<uint8_t>(DamageCause::Blast) && source != nullptr) r.credit = event.source;
            else if (target.lastAttacker != 0 && tick - target.lastAttackTick <= rules.creditTicks) r.credit = target.lastAttacker;
        }
        return r;
    }

    void TickCharges(FighterState& fighter, const FightRules& rules, uint32_t tick) {
        if (rules.maxCharges == 0 || fighter.charges >= rules.maxCharges) {
            fighter.nextChargeTick = 0;
            return;
        }
        if (fighter.nextChargeTick == 0) fighter.nextChargeTick = tick + rules.chargeTicks;
        if (tick >= fighter.nextChargeTick) {
            fighter.charges = static_cast<uint8_t>(fighter.charges + 1);
            fighter.nextChargeTick = fighter.charges < rules.maxCharges ? tick + rules.chargeTicks : 0;
        }
    }

    bool SpendCharge(FighterState& fighter, const FightRules& rules, uint32_t tick) {
        if (rules.maxCharges == 0) {
            fighter.protectedUntil = 0;
            return true;
        }
        if (fighter.charges == 0) return false;
        fighter.charges = static_cast<uint8_t>(fighter.charges - 1);
        if (fighter.nextChargeTick == 0) fighter.nextChargeTick = tick + rules.chargeTicks;
        fighter.protectedUntil = 0;
        return true;
    }

    void StartLife(FighterState& fighter, const FightRules& rules, uint32_t tick) {
        fighter.health = rules.maxHealth;
        fighter.charges = rules.maxCharges;
        fighter.nextChargeTick = 0;
        fighter.dead = false;
        fighter.spawned = true;
        fighter.respawnTick = 0;
        fighter.protectedUntil = tick + rules.protectTicks;
        fighter.lifeSeq = static_cast<uint8_t>(fighter.lifeSeq + 1);
        fighter.lastAttacker = 0;
        fighter.lastAttackTick = 0;
        fighter.nextLavaTick = 0;
    }

    glm::dvec3 TeamSpawnFeet(Simulation& simulation, const FightRules& rules, const RegionDesc& region, TeamId team, uint32_t k,
                             const glm::dvec3& fallback) {
        if (team < 1 || team > kTeamCount) return fallback;
        // The 8 unit offsets of a ring, 45 degrees apart (constants: no std::cos on the way to a position).
        static constexpr double kRing[8][2] = {{1.0, 0.0},  {0.70710678118654752, 0.70710678118654752},   {0.0, 1.0},
                                               {-0.70710678118654752, 0.70710678118654752},  {-1.0, 0.0},
                                               {-0.70710678118654752, -0.70710678118654752}, {0.0, -1.0},
                                               {0.70710678118654752, -0.70710678118654752}};
        const glm::vec2 at = rules.teamSpawns[team - 1];
        const double top = static_cast<double>(region.heightRange.y) - 0.5;
        const float reach = 256.0f;
        const voxel::World& world = simulation.World();
        const double ox = 2.0 * kRing[k % 8][0], oz = 2.0 * kRing[k % 8][1];
        for (const glm::dvec2 p : {glm::dvec2(at.x + ox, at.y + oz), glm::dvec2(at.x, at.y)}) {
            const voxel::WorldRayHit hit = world.Raycast(glm::dvec3(p.x, top, p.y), glm::vec3(0.0f, -1.0f, 0.0f), reach);
            if (hit.hit) return glm::dvec3(p.x, hit.position.y + 0.05, p.y);
        }
        return fallback;
    }

    TeamId PickTeam(std::span<const FighterState> fighters, std::span<const bool> occupied) {
        std::array<uint32_t, 2> count{};
        for (size_t i = 0; i < fighters.size() && i < occupied.size(); ++i)
            if (occupied[i] && fighters[i].team >= 1 && fighters[i].team <= kTeamCount) ++count[fighters[i].team - 1];
        return count[1] < count[0] ? TeamId{2} : TeamId{1};
    }

    void AimAngles(const glm::vec3& dir, uint16_t& yaw, int16_t& pitch) {
        const glm::vec3 d = glm::length(dir) > 0.0f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, -1.0f);
        yaw = QuantizeYaw(std::atan2(-d.x, -d.z));
        pitch = QuantizePitch(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
    }

    namespace {
        void Win(Round& round, const FightRules& rules, uint8_t winner, RoundEnd end, uint32_t tick) {
            round.winner = winner;
            round.end = end;
            round.phase = RoundPhase::Ended;
            round.phaseEndTick = tick + rules.endTicks;
        }
    }

    RoundStep StepRound(Round& round, const FightRules& rules, uint32_t tick, std::array<uint32_t, 2> teamPlayers,
                        std::array<bool, 2> felled) {
        RoundStep step;
        if (!rules.Active()) return step;
        const Round before = round;
        switch (round.phase) {
            case RoundPhase::Warmup:
                if (teamPlayers[0] + teamPlayers[1] >= rules.minPlayers && teamPlayers[0] > 0 && teamPlayers[1] > 0) {
                    round.phase = RoundPhase::Countdown;
                    round.phaseEndTick = tick + rules.countdownTicks;
                    round.score = {0, 0};
                    step.startLives = true;
                }
                break;
            case RoundPhase::Countdown:
                if (tick >= round.phaseEndTick) {
                    round.phase = RoundPhase::Live;
                    round.phaseEndTick = tick + rules.roundTicks;
                    round.emptySince[0] = round.emptySince[1] = 0;
                }
                break;
            case RoundPhase::Live: {
                if (felled[0] != felled[1]) {
                    Win(round, rules, felled[0] ? 2 : 1, RoundEnd::Felled, tick);
                    break;
                }
                if (felled[0] && felled[1]) {
                    const uint8_t w = round.score[0] > round.score[1] ? 1 : round.score[1] > round.score[0] ? 2 : kDraw;
                    Win(round, rules, w, RoundEnd::Felled, tick);
                    break;
                }
                bool forfeit = false;
                for (int t = 0; t < 2 && !forfeit; ++t) {
                    if (teamPlayers[t] > 0) {
                        round.emptySince[t] = 0;
                        continue;
                    }
                    if (round.emptySince[t] == 0) round.emptySince[t] = std::max(1u, tick);
                    if (tick - round.emptySince[t] >= rules.forfeitTicks) {
                        Win(round, rules, static_cast<uint8_t>(t == 0 ? 2 : 1), RoundEnd::Forfeit, tick);
                        forfeit = true;
                    }
                }
                if (forfeit) break;
                if (tick >= round.phaseEndTick) {
                    if (!round.overtime) {
                        if (round.score[0] != round.score[1]) {
                            Win(round, rules, round.score[0] > round.score[1] ? 1 : 2, RoundEnd::Time, tick);
                        } else {
                            round.overtime = true;
                            round.phaseEndTick = tick + rules.overtimeTicks;
                        }
                    } else {
                        Win(round, rules, kDraw, RoundEnd::Time, tick);
                    }
                }
                break;
            }
            case RoundPhase::Ended:
                if (tick >= round.phaseEndTick) step.sessionOver = true;
                break;
        }
        step.changed = round.phase != before.phase || round.phaseEndTick != before.phaseEndTick || round.score != before.score ||
                       round.overtime != before.overtime || round.winner != before.winner || round.end != before.end;
        return step;
    }

    bool ScoreKill(Round& round, const FightRules& rules, TeamId team, uint32_t tick) {
        if (round.phase != RoundPhase::Live || team < 1 || team > kTeamCount) return false;
        round.score[team - 1] = static_cast<uint16_t>(round.score[team - 1] + 1);
        if (round.overtime || round.score[team - 1] >= rules.scoreLimit) {
            Win(round, rules, team, RoundEnd::Score, tick);
            return true;
        }
        return false;
    }

    FellingTower BuildFellingTower(const voxel::World& world, const voxel::WorldMaterials& m, const glm::vec2& at, float voxelSize,
                                   int32_t topVoxelY) {
        auto n = [voxelSize](double metres) { return static_cast<int32_t>(std::llround(metres / static_cast<double>(voxelSize))); };
        const glm::ivec3 c = world.WorldToVoxel(glm::dvec3(at.x, 0.0, at.y));
        const int32_t cx = c.x, cz = c.z, g = topVoxelY;
        FellingTower t;
        t.ops.push_back(WorldOp::Box({cx - n(2), g - n(1), cz - n(2)}, {cx + n(2), g + n(0.5), cz + n(2)}, m.stone));
        t.ops.push_back(WorldOp::Box({cx - n(1), g + n(0.5), cz - n(1)}, {cx + n(1), g + n(10.5), cz + n(1)}, m.brick));
        t.crownMin = {cx - n(1.75), g + n(10.5), cz - n(1.75)};
        t.crownMax = {cx + n(1.75), g + n(13), cz + n(1.75)};
        t.ops.push_back(WorldOp::Box(t.crownMin, t.crownMax, m.brick));
        const glm::ivec3 size = t.crownMax - t.crownMin;
        t.crownVoxels = static_cast<uint32_t>(size.x) * static_cast<uint32_t>(size.y) * static_cast<uint32_t>(size.z);
        return t;
    }

    uint32_t CountSolid(const voxel::World& world, const glm::ivec3& min, const glm::ivec3& max) {
        uint32_t n = 0;
        for (int32_t z = min.z; z < max.z; ++z)
            for (int32_t y = min.y; y < max.y; ++y)
                for (int32_t x = min.x; x < max.x; ++x) n += world.GetVoxel({x, y, z}) != 0 ? 1u : 0u;
        return n;
    }
}
