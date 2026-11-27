// docs/design/fight.md ([wp-7]): the fight's rules in sim - blast damage by distance, falls, kill credit, charges,
// spawn protection, the round machine, aim angles - and the Felling duel's tower (suite Felling). GPU-free and
// network-free: what the session server decides.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;

using namespace vortex;

namespace {
    constexpr float kVoxel = 0.125f;

    /// centreQ of a point in metres (exact for multiples of 1/128 m at 0.125 m voxels).
    glm::ivec3 Q(double x, double y, double z) {
        const double perMetre = 16.0 / static_cast<double>(kVoxel);
        return {static_cast<int>(std::lround(x * perMetre)), static_cast<int>(std::lround(y * perMetre)), static_cast<int>(std::lround(z * perMetre))};
    }

    sim::FightRules Deathmatch() {
        sim::FightRules r;
        r.mode = sim::GameMode::Deathmatch;
        return r;
    }

    sim::FighterState Alive(sim::TeamId team) {
        sim::FighterState f;
        f.team = team;
        f.spawned = true;
        return f;
    }

    sim::DamageEvent Hit(uint8_t target, uint8_t source, int16_t amount, sim::DamageCause cause = sim::DamageCause::Blast) {
        return {target, source, amount, static_cast<uint8_t>(cause)};
    }
}

// Decision 9: 60 at the capsule's surface, linear to 0 at 1.2 m from it (worked numbers in fight.md "Damage").
TEST(Combat, BlastDamageFallsLinearlyToZeroAt1200mm) {
    const sim::BlastDamageRule rule{60, 1200};
    const sim::PlayerTuning tuning;
    const glm::vec3 feet(0.0f, 10.0f, 0.0f);
    EXPECT_EQ(sim::BlastDamageAt(rule, glm::ivec3(0, 1280, 0), kVoxel, feet, tuning), 60);          // at the feet: 300 mm below the axis
    EXPECT_EQ(sim::BlastDamageAt(rule, Q(0.5, 10.75, 0.0), kVoxel, feet, tuning), 50);              // 0.5 m aside: surface 200 mm
    EXPECT_EQ(sim::BlastDamageAt(rule, Q(0.75, 10.75, 0.0), kVoxel, feet, tuning), 37);             // surface 450 mm: 60 * 750 / 1200
    EXPECT_EQ(sim::BlastDamageAt(rule, Q(1.5, 10.75, 0.0), kVoxel, feet, tuning), 0);               // surface 1200 mm
    EXPECT_EQ(sim::BlastDamageAt(rule, Q(0.0, 12.5, 0.0), kVoxel, feet, tuning), 25);               // above the head: dy 1000, surface 700
    EXPECT_EQ(sim::BlastDamageAt(sim::BlastDamageRule{}, glm::ivec3(0, 1280, 0), kVoxel, feet, tuning), 0);   // centre 0: none
}

TEST(Combat, ISqrt64IsTheFloorRoot) {
    EXPECT_EQ(sim::ISqrt64(0), 0);
    EXPECT_EQ(sim::ISqrt64(1), 1);
    EXPECT_EQ(sim::ISqrt64(2), 1);
    EXPECT_EQ(sim::ISqrt64(3), 1);
    EXPECT_EQ(sim::ISqrt64(4), 2);
    const int64_t big = (int64_t{1} << 62) - 1;
    EXPECT_EQ(sim::ISqrt64(big), (int64_t{1} << 31) - 1);
    const int64_t m = (int64_t{1} << 31) - 1;
    EXPECT_EQ(sim::ISqrt64(m * m), m);
    EXPECT_EQ(sim::ISqrt64(m * m - 1), m - 1);
}

// Decision 8: (s - 886) * 11 / 100 hp above 8.86 m/s (a 4 m fall).
TEST(Combat, FallDamageStartsAtFourMetres) {
    const sim::FightRules rules = Deathmatch();
    EXPECT_EQ(sim::FallDamage(rules, -8.85f), 0);
    EXPECT_EQ(sim::FallDamage(rules, -12.53f), 40);
    EXPECT_EQ(sim::FallDamage(rules, -17.72f), 97);
    EXPECT_EQ(sim::FallDamage(rules, -30.0f), 232);
    EXPECT_EQ(sim::FallDamage(rules, 5.0f), 0);
}

TEST(Combat, FriendlyFireAndSelfDamageAreOff) {
    const sim::FightRules rules = Deathmatch();
    sim::FighterState target = Alive(1), mate = Alive(1);
    EXPECT_EQ(sim::ApplyDamage(target, 1, &mate, Hit(1, 2, 60), rules, 10).dealt, 0);
    EXPECT_EQ(sim::ApplyDamage(target, 1, &target, Hit(1, 1, 60), rules, 10).dealt, 0);
    EXPECT_EQ(target.health, 100);
    EXPECT_EQ(mate.hits, 0);
}

TEST(Combat, ADeathCountsOnceAndCreditsTheBlastSource) {
    const sim::FightRules rules = Deathmatch();
    sim::FighterState target = Alive(1), source = Alive(2);
    EXPECT_EQ(sim::ApplyDamage(target, 1, &source, Hit(1, 2, 60), rules, 10).dealt, 60);
    const sim::DamageResult second = sim::ApplyDamage(target, 1, &source, Hit(1, 2, 60), rules, 11);
    EXPECT_EQ(second.dealt, 40);
    EXPECT_TRUE(second.killed);
    EXPECT_EQ(second.credit, 2);
    EXPECT_TRUE(target.dead);
    EXPECT_EQ(target.respawnTick, 11u + rules.respawnTicks);
    const sim::DamageResult third = sim::ApplyDamage(target, 1, &source, Hit(1, 2, 60), rules, 12);
    EXPECT_EQ(third.dealt, 0);
    EXPECT_FALSE(third.killed);
    EXPECT_EQ(target.deaths, 1);
    EXPECT_EQ(source.hits, 2);
}

// Decision 10: a fall, lava or void death credits the last enemy who hurt the victim within 300 ticks.
TEST(Combat, EnvironmentDeathCreditsTheLastEnemyWithinFiveSeconds) {
    const sim::FightRules rules = Deathmatch();
    for (const uint32_t fallTick : {399u, 401u}) {
        sim::FighterState target = Alive(1), source = Alive(2);
        EXPECT_EQ(sim::ApplyDamage(target, 1, &source, Hit(1, 2, 30), rules, 100).dealt, 30);
        const sim::DamageResult fall = sim::ApplyDamage(target, 1, nullptr, Hit(1, 0, 100, sim::DamageCause::Fall), rules, fallTick);
        EXPECT_TRUE(fall.killed);
        EXPECT_EQ(fall.credit, fallTick == 399u ? 2 : 0) << "fall at tick " << fallTick;
    }
}

TEST(Combat, ChargesComeBackOnePer72Ticks) {
    const sim::FightRules rules = Deathmatch();
    sim::FighterState f = Alive(1);
    sim::StartLife(f, rules, 0);
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(sim::SpendCharge(f, rules, 0));
    EXPECT_FALSE(sim::SpendCharge(f, rules, 0));
    EXPECT_EQ(f.charges, 0);
    std::vector<uint32_t> back;
    for (uint32_t t = 1; t <= 300; ++t) {
        const uint8_t before = f.charges;
        sim::TickCharges(f, rules, t);
        if (f.charges != before) back.push_back(t);
    }
    EXPECT_EQ(back, (std::vector<uint32_t>{72, 144, 216}));
    EXPECT_EQ(f.charges, 3);
    EXPECT_EQ(f.nextChargeTick, 0u);
    sim::FightRules unlimited = rules;
    unlimited.maxCharges = 0;
    sim::FighterState u = Alive(1);
    u.charges = 0;
    for (int i = 0; i < 10; ++i) EXPECT_TRUE(sim::SpendCharge(u, unlimited, 5));
}

TEST(Combat, SpawnProtectionEndsAtTheFirstCast) {
    const sim::FightRules rules = Deathmatch();
    sim::FighterState target = Alive(1), source = Alive(2);
    sim::StartLife(target, rules, 0);
    EXPECT_EQ(target.lifeSeq, 1);
    EXPECT_EQ(sim::ApplyDamage(target, 1, &source, Hit(1, 2, 30), rules, 50).dealt, 0);
    EXPECT_TRUE(sim::SpendCharge(target, rules, 60));
    EXPECT_EQ(sim::ApplyDamage(target, 1, &source, Hit(1, 2, 30), rules, 61).dealt, 30);
}

TEST(Combat, TheRoundRunsWarmupCountdownLiveEnded) {
    sim::FightRules rules = Deathmatch();
    rules.minPlayers = 2;
    sim::Round round;
    EXPECT_FALSE(sim::StepRound(round, rules, 10, {1, 0}, {false, false}).changed);   // one team only: waits
    const sim::RoundStep start = sim::StepRound(round, rules, 11, {1, 1}, {false, false});
    EXPECT_TRUE(start.changed);
    EXPECT_TRUE(start.startLives);
    EXPECT_EQ(round.phase, sim::RoundPhase::Countdown);
    EXPECT_FALSE(sim::StepRound(round, rules, 310, {1, 1}, {false, false}).changed);
    EXPECT_TRUE(sim::StepRound(round, rules, 311, {1, 1}, {false, false}).changed);
    EXPECT_EQ(round.phase, sim::RoundPhase::Live);
    EXPECT_EQ(round.phaseEndTick, 311u + rules.roundTicks);
    bool ended = false;
    for (uint16_t k = 0; k < rules.scoreLimit; ++k) ended = sim::ScoreKill(round, rules, 2, 400 + k);
    EXPECT_TRUE(ended);
    EXPECT_EQ(round.phase, sim::RoundPhase::Ended);
    EXPECT_EQ(round.winner, 2);
    EXPECT_EQ(round.end, sim::RoundEnd::Score);
    EXPECT_FALSE(sim::ScoreKill(round, rules, 1, 500));   // no score after the end
    const uint32_t over = 400u + rules.scoreLimit - 1 + rules.endTicks;
    EXPECT_FALSE(sim::StepRound(round, rules, over - 1, {1, 1}, {false, false}).sessionOver);
    EXPECT_TRUE(sim::StepRound(round, rules, over, {1, 1}, {false, false}).sessionOver);
    // A sandbox has no round.
    sim::Round none;
    EXPECT_FALSE(sim::StepRound(none, sim::SandboxRules(), 10, {4, 4}, {false, false}).changed);
    EXPECT_EQ(none.phase, sim::RoundPhase::Warmup);
}

namespace {
    sim::Round LiveRound(const sim::FightRules& rules, uint32_t& tick) {
        sim::Round round;
        sim::StepRound(round, rules, tick, {1, 1}, {false, false});
        tick += rules.countdownTicks;
        sim::StepRound(round, rules, tick, {1, 1}, {false, false});
        return round;
    }
}

TEST(Combat, ATiedRoundGoesToOvertimeThenTheNextKillWins) {
    const sim::FightRules rules = Deathmatch();
    uint32_t tick = 1;
    sim::Round round = LiveRound(rules, tick);
    sim::ScoreKill(round, rules, 1, tick + 5);
    sim::ScoreKill(round, rules, 2, tick + 6);
    sim::StepRound(round, rules, round.phaseEndTick, {1, 1}, {false, false});
    EXPECT_EQ(round.phase, sim::RoundPhase::Live);
    EXPECT_TRUE(round.overtime);
    EXPECT_TRUE(sim::ScoreKill(round, rules, 1, round.phaseEndTick - 10));
    EXPECT_EQ(round.winner, 1);
    EXPECT_EQ(round.end, sim::RoundEnd::Score);
}

TEST(Combat, ATiedRoundGoesToOvertimeThenADrawWhenOvertimeRunsOut) {
    const sim::FightRules rules = Deathmatch();
    uint32_t tick = 1;
    sim::Round round = LiveRound(rules, tick);
    const uint32_t end = round.phaseEndTick;
    sim::StepRound(round, rules, end, {1, 1}, {false, false});
    EXPECT_TRUE(round.overtime);
    EXPECT_EQ(round.phaseEndTick, end + rules.overtimeTicks);
    sim::StepRound(round, rules, end + rules.overtimeTicks, {1, 1}, {false, false});
    EXPECT_EQ(round.phase, sim::RoundPhase::Ended);
    EXPECT_EQ(round.winner, sim::kDraw);
    EXPECT_EQ(round.end, sim::RoundEnd::Time);
}

TEST(Combat, AnEmptyTeamForfeitsAfter1800Ticks) {
    const sim::FightRules rules = Deathmatch();
    uint32_t tick = 1;
    sim::Round round = LiveRound(rules, tick);
    const uint32_t empty = tick + 10;
    for (uint32_t t = empty; t < empty + rules.forfeitTicks; ++t) {
        sim::StepRound(round, rules, t, {2, 0}, {false, false});
        ASSERT_EQ(round.phase, sim::RoundPhase::Live) << "tick " << t;
    }
    sim::StepRound(round, rules, empty + rules.forfeitTicks, {2, 0}, {false, false});
    EXPECT_EQ(round.phase, sim::RoundPhase::Ended);
    EXPECT_EQ(round.winner, 1);
    EXPECT_EQ(round.end, sim::RoundEnd::Forfeit);
}

// --- siege (fight.md "Siege") ----------------------------------------------------------------------------------------

namespace {
    sim::FightRules Siege() {
        sim::FightRules r;
        r.mode = sim::GameMode::Siege;
        r.holdTicks = 600;
        return r;
    }
}

TEST(Siege, AnUnbrokenHoldOfTheSanctumWinsForTheAttackers) {
    const sim::FightRules rules = Siege();
    uint32_t tick = 1;
    sim::Round round = LiveRound(rules, tick);
    const uint32_t from = tick + 100;
    // Every tick an attacker and no defender is inside holds it; the hold's first tick counts.
    for (uint32_t t = from; t < from + rules.holdTicks - 1; ++t) {
        const sim::RoundStep step = sim::StepRound(round, rules, t, {2, 2}, {false, false}, {0, 1});
        ASSERT_EQ(round.phase, sim::RoundPhase::Live) << "tick " << t;
        ASSERT_EQ(round.holdSince, from);
        EXPECT_EQ(step.changed, t == from) << "a RoundUpdate when the hold starts, none while it runs";
    }
    const sim::RoundStep win = sim::StepRound(round, rules, from + rules.holdTicks - 1, {2, 2}, {false, false}, {0, 2});
    EXPECT_TRUE(win.changed);
    EXPECT_EQ(round.phase, sim::RoundPhase::Ended);
    EXPECT_EQ(round.winner, sim::kSiegeAttackers);
    EXPECT_EQ(round.end, sim::RoundEnd::Held);
}

TEST(Siege, ADefenderInsideOrNoAttackerBreaksTheHold) {
    const sim::FightRules rules = Siege();
    uint32_t tick = 1;
    sim::Round round = LiveRound(rules, tick);
    uint32_t t = tick + 10;
    for (uint32_t k = 0; k < rules.holdTicks - 10; ++k) sim::StepRound(round, rules, t++, {2, 2}, {false, false}, {0, 1});
    EXPECT_NE(round.holdSince, 0u);
    // A defender comes in: the hold breaks at once and starts again from 0 when the room is the attackers' again.
    EXPECT_TRUE(sim::StepRound(round, rules, t++, {2, 2}, {false, false}, {1, 2}).changed);
    EXPECT_EQ(round.holdSince, 0u);
    const uint32_t again = t;
    sim::StepRound(round, rules, t++, {2, 2}, {false, false}, {0, 1});
    EXPECT_EQ(round.holdSince, again);
    // The last attacker leaves (or dies): the hold breaks too.
    sim::StepRound(round, rules, t++, {2, 2}, {false, false}, {0, 0});
    EXPECT_EQ(round.holdSince, 0u);
    for (uint32_t k = 0; k < rules.holdTicks - 1; ++k) sim::StepRound(round, rules, t++, {2, 2}, {false, false}, {0, 1});
    EXPECT_EQ(round.phase, sim::RoundPhase::Live) << "a hold one tick short does not win";
}

TEST(Siege, TheDefendersWinWhenTheTimeRunsOutAndKillsNeverEndIt) {
    const sim::FightRules rules = Siege();
    uint32_t tick = 1;
    sim::Round round = LiveRound(rules, tick);
    for (uint16_t k = 0; k < rules.scoreLimit + 5; ++k) EXPECT_FALSE(sim::ScoreKill(round, rules, sim::kSiegeAttackers, tick + k));
    EXPECT_EQ(round.score[1], rules.scoreLimit + 5);
    EXPECT_EQ(round.phase, sim::RoundPhase::Live);
    sim::StepRound(round, rules, round.phaseEndTick - 1, {2, 2}, {false, false}, {0, 1});
    EXPECT_EQ(round.phase, sim::RoundPhase::Live);
    sim::StepRound(round, rules, round.phaseEndTick, {2, 2}, {false, false}, {0, 1});
    EXPECT_EQ(round.phase, sim::RoundPhase::Ended);
    EXPECT_EQ(round.winner, sim::kSiegeDefenders) << "no overtime: the keep held out";
    EXPECT_EQ(round.end, sim::RoundEnd::Time);
    EXPECT_FALSE(round.overtime);
}

TEST(Attune, APickKeepsTwoElementsAndDropsTheFirstPicked) {
    sim::AttunePick pick;   // Metal + Pressure: the first life's
    EXPECT_TRUE(pick.Complete());
    pick.Toggle(static_cast<uint8_t>(sim::Element::Frost));
    EXPECT_EQ(pick.picked, (std::array<uint8_t, 2>{2, 3})) << "Metal (picked first) goes";
    pick.Toggle(static_cast<uint8_t>(sim::Element::Frost));
    EXPECT_FALSE(pick.Complete());
    EXPECT_TRUE(pick.Has(2));
    pick.Toggle(static_cast<uint8_t>(sim::Element::Thermal));
    EXPECT_TRUE(pick.Complete());
    EXPECT_EQ(pick.picked, (std::array<uint8_t, 2>{2, 1}));
    pick.Toggle(2);
    pick.Toggle(1);
    EXPECT_EQ(pick.count, 0);
    pick.Toggle(4);   // no fifth element
    EXPECT_EQ(pick.count, 0);
}

TEST(Combat, AimAnglesInvertLookDirection) {
    const float limit = std::cos(glm::radians(0.1f));
    for (int y = 0; y < 8; ++y)
        for (int p = 0; p < 8; ++p) {
            const float yaw = glm::radians(45.0f * static_cast<float>(y));
            const float pitch = glm::radians(-80.0f + 160.0f * static_cast<float>(p) / 7.0f);
            const glm::vec3 dir(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
            uint16_t qy = 0;
            int16_t qp = 0;
            sim::AimAngles(dir, qy, qp);
            EXPECT_GT(glm::dot(sim::LookDirection(qy, qp), dir), limit) << "yaw " << y * 45 << " pitch " << p;
        }
}

TEST(Combat, TeamsAreBalancedByPickTeam) {
    std::array<sim::FighterState, 4> f{};
    std::array<bool, 4> occupied{};
    EXPECT_EQ(sim::PickTeam(f, occupied), 1);
    f[0].team = 1;
    occupied[0] = true;
    EXPECT_EQ(sim::PickTeam(f, occupied), 2);
    f[1].team = 2;
    occupied[1] = true;
    EXPECT_EQ(sim::PickTeam(f, occupied), 1);
}

// --- Felling (card 9) ----------------------------------------------------------------------------------------

namespace {
    constexpr int kGround = 64;                           // flat ground: voxels y < 64, its top at 8 m

    struct FellingArena {
        FellingArena() : simulation(Config()) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(kGround, ids.grass, ids.stone));
            if (initialized) simulation.World().LoadBlocking(glm::dvec3(0.0, 12.0, 0.0));
        }
        static sim::SimulationConfig Config() {
            sim::SimulationConfig c;
            c.chunkCapacity = 1024;
            c.maxParticles = 0;
            c.jobThreads = 2;
            c.world.voxelSize = kVoxel;
            c.world.windowSize = {6, 8, 6};
            c.physics.maxBodies = 2048;
            c.physics.maxBodyPairs = 4096;
            c.physics.maxContactConstraints = 4096;
            c.physics.threads = 1;
            return c;
        }
        sim::FellingTower Build() {
            const sim::FellingTower t = sim::BuildFellingTower(simulation.World(), simulation.WorldMaterialIds(), glm::vec2(0.0f), kVoxel, kGround);
            for (const sim::WorldOp& op : t.ops) EXPECT_TRUE(sim::ApplyOp(simulation.World(), op).Complete());
            return t;
        }
        uint32_t TowerVoxels(const sim::FellingTower& t) const {
            uint32_t n = 0;
            for (const sim::WorldOp& op : t.ops) n += sim::CountSolid(simulation.World(), op.min, op.max);
            return n;
        }
        sim::Simulation simulation;
        bool initialized = false;
    };
}

TEST(Felling, TheTowerStandsWhenABlastHits6MetresAway) {
    FellingArena a;
    ASSERT_TRUE(a.initialized);
    const sim::FellingTower t = a.Build();
    EXPECT_EQ(sim::CountSolid(a.simulation.World(), t.crownMin, t.crownMax), t.crownVoxels);
    EXPECT_EQ(t.crownVoxels, 28u * 20u * 28u);
    const uint32_t before = a.TowerVoxels(t);
    const double vs = kVoxel;
    sim::Blast(a.simulation, voxel::QuantizeSphere(glm::dvec3(6.0, 8.0, 0.0) / vs, 1.2 / vs), 1600);
    EXPECT_EQ(a.TowerVoxels(t), before);
    EXPECT_EQ(sim::CountSolid(a.simulation.World(), t.crownMin, t.crownMax), t.crownVoxels);
}

TEST(Felling, CuttingTheShaftFellsTheCrown) {
    FellingArena a;
    ASSERT_TRUE(a.initialized);
    const sim::FellingTower t = a.Build();
    const double vs = kVoxel, h = 8.0 + 3.0;
    for (const glm::dvec2 side : {glm::dvec2(1.0, 0.0), glm::dvec2(-1.0, 0.0), glm::dvec2(0.0, 1.0), glm::dvec2(0.0, -1.0)})
        sim::Blast(a.simulation, voxel::QuantizeSphere(glm::dvec3(side.x, h, side.y) / vs, 1.2 / vs), 1600);
    EXPECT_LT(sim::CountSolid(a.simulation.World(), t.crownMin, t.crownMax) * 5, t.crownVoxels);
}

TEST(Felling, TheRoundEndsWhenACrownFalls) {
    sim::FightRules rules = Deathmatch();
    rules.mode = sim::GameMode::Felling;
    uint32_t tick = 1;
    sim::Round round = LiveRound(rules, tick);
    sim::StepRound(round, rules, tick + 30, {1, 1}, {true, false});
    EXPECT_EQ(round.phase, sim::RoundPhase::Ended);
    EXPECT_EQ(round.winner, 2);
    EXPECT_EQ(round.end, sim::RoundEnd::Felled);
}
