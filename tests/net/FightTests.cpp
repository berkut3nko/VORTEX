// docs/design/fight.md ([wp-7]): the fight on the wire (suite FightWire) and in a session (suite Fight): teams at
// the join, blast damage and kills, death (not stepped, casts refused Dead), respawn at the team spawn (never a
// correction), charges (NoCharge), the round to its score limit, falls and lava, a sandbox unchanged, and a 2v2
// match of fighting bots that ends with a winner. One process, loopback links, one manual clock (the `Match`
// harness of MovementTests.cpp as `FightField`).

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;
import vortex.net;

using namespace vortex;

// --- FightWire ------------------------------------------------------------------------------------------------

namespace {
    template <typename T>
    void RefusesTrailingAndMissing(const std::vector<uint8_t>& bytes) {
        T out;
        std::vector<uint8_t> longer = bytes;
        longer.push_back(0);
        EXPECT_FALSE(net::Decode(longer, out));
        EXPECT_FALSE(net::Decode(std::span(bytes).first(bytes.size() - 1), out));
    }

    template <typename T>
    bool DecodesWith(std::vector<uint8_t> bytes, size_t at, uint8_t value) {
        bytes[at] = value;
        T out;
        return net::Decode(bytes, out);
    }
}

TEST(FightWire, MessagesRoundTripAndRefuseBadValues) {
    net::Roster roster;
    roster.players = {{0, 1, 3, 1}, {2, 2, 0, 4}, {5, 1, 65535, 2}};
    {
        const std::vector<uint8_t> bytes = net::Encode(roster);
        net::Roster r;
        ASSERT_TRUE(net::Decode(bytes, r));
        EXPECT_EQ(r, roster);
        RefusesTrailingAndMissing<net::Roster>(bytes);
        EXPECT_FALSE(DecodesWith<net::Roster>(bytes, 3, 3));        // team 3
        EXPECT_FALSE(DecodesWith<net::Roster>(bytes, 8, 0));        // slot 0 twice
        net::Roster many;
        for (uint8_t i = 0; i < 65; ++i) many.players.push_back({i, 1, 0, 0});
        std::vector<uint8_t> tooMany = net::Encode(many);           // the encoder keeps 64
        EXPECT_EQ(tooMany[1], 64);
        tooMany[1] = 65;
        for (int i = 0; i < 6; ++i) tooMany.push_back(0);
        net::Roster r2;
        EXPECT_FALSE(net::Decode(tooMany, r2));
    }
    net::RoundUpdate round;
    round.mode = sim::GameMode::Felling;
    round.phase = sim::RoundPhase::Ended;
    round.phaseEndTick = 123456;
    round.score = {7, 9};
    round.scoreLimit = 15;
    round.winner = sim::kDraw;
    round.end = sim::RoundEnd::Forfeit;
    round.overtime = true;
    {
        const std::vector<uint8_t> bytes = net::Encode(round);
        net::RoundUpdate r;
        ASSERT_TRUE(net::Decode(bytes, r));
        EXPECT_EQ(r, round);
        RefusesTrailingAndMissing<net::RoundUpdate>(bytes);
        EXPECT_FALSE(DecodesWith<net::RoundUpdate>(bytes, 1, 3));   // mode 3
        EXPECT_FALSE(DecodesWith<net::RoundUpdate>(bytes, 2, 4));   // phase 4
        EXPECT_FALSE(DecodesWith<net::RoundUpdate>(bytes, 13, 3));  // winner 3
        EXPECT_TRUE(DecodesWith<net::RoundUpdate>(bytes, 13, 2));
        EXPECT_FALSE(DecodesWith<net::RoundUpdate>(bytes, 14, 5));  // end 5
        EXPECT_FALSE(DecodesWith<net::RoundUpdate>(bytes, 15, 2));  // overtime 2
    }
    net::KillNotice kill{98765, 3, 2, sim::DamageCause::Lava, net::KillNotice::kCredited};
    {
        const std::vector<uint8_t> bytes = net::Encode(kill);
        net::KillNotice k;
        ASSERT_TRUE(net::Decode(bytes, k));
        EXPECT_EQ(k, kill);
        RefusesTrailingAndMissing<net::KillNotice>(bytes);
        EXPECT_FALSE(DecodesWith<net::KillNotice>(bytes, 7, 0));    // cause 0
        EXPECT_FALSE(DecodesWith<net::KillNotice>(bytes, 7, 6));    // cause 6
        EXPECT_TRUE(DecodesWith<net::KillNotice>(bytes, 7, 5));     // Crush (reserved for [si-2]) decodes
        EXPECT_FALSE(DecodesWith<net::KillNotice>(bytes, 8, 2));    // flags 2
        EXPECT_FALSE(DecodesWith<net::KillNotice>(bytes, 6, 4));    // killer slot 3 + 1 = the victim
        EXPECT_TRUE(DecodesWith<net::KillNotice>(bytes, 6, 0));     // no killer
    }
    // The owner block's flags and the two new rejections.
    net::Snapshot snap;
    snap.tick = 10;
    snap.hasOwner = true;
    snap.castRejected = net::CastRejection::Dead;
    snap.fight = {37, 2, 41, net::Snapshot::Fight::kDead | net::Snapshot::Fight::kProtected, 150, 4, 250};
    snap.others.push_back({3, glm::vec3(1.0f, 2.0f, 3.0f), 77, 1, static_cast<uint8_t>(55 | net::RemotePlayer::kDeadBit)});
    {
        const std::vector<uint8_t> bytes = net::Encode(snap);
        net::Snapshot s;
        ASSERT_TRUE(net::Decode(bytes, s));
        EXPECT_EQ(s.fight, snap.fight);
        EXPECT_EQ(s.castRejected, net::CastRejection::Dead);
        ASSERT_EQ(s.others.size(), 1u);
        EXPECT_EQ(s.others[0], snap.others[0]);
        const size_t flagsAt = net::kSnapshotHeaderBytes - 2 + 32 + 3;   // header without the counts, the old owner block, health/charges/chargeTicks
        EXPECT_FALSE(DecodesWith<net::Snapshot>(bytes, flagsAt, 8));
        EXPECT_TRUE(DecodesWith<net::Snapshot>(bytes, flagsAt, 7));
        const size_t rejectedAt = net::kSnapshotHeaderBytes - 2 + 31;
        EXPECT_TRUE(DecodesWith<net::Snapshot>(bytes, rejectedAt, 5));
        EXPECT_FALSE(DecodesWith<net::Snapshot>(bytes, rejectedAt, 6));
    }
    EXPECT_STREQ(net::ToString(net::CastRejection::NoCharge), "no charge");
    EXPECT_STREQ(net::ToString(net::CastRejection::Dead), "dead");
    EXPECT_EQ(net::kProtocolVersion, 11);
}

TEST(FightWire, SizesAreAsDesigned) {
    net::Snapshot none, owner;
    owner.hasOwner = true;
    EXPECT_EQ(net::Encode(owner).size() - net::Encode(none).size(), 39u);
    EXPECT_EQ(net::kSnapshotOwnerBytes, 39u);
    net::Snapshot one = owner;
    one.others.push_back({});
    EXPECT_EQ(net::Encode(one).size() - net::Encode(owner).size(), 17u);
    EXPECT_EQ(net::kSnapshotPlayerBytes, 17u);
    // The Welcome's last 7 bytes are its fight fields: without them it does not decode.
    net::Welcome w;
    w.region.terrain.voxelSize = 0.125f;
    w.fight = {sim::GameMode::Deathmatch, 100, 3, 72, 180};
    const std::vector<uint8_t> bytes = net::Encode(w);
    net::Welcome back;
    ASSERT_TRUE(net::Decode(bytes, back));
    EXPECT_EQ(back.fight, w.fight);
    EXPECT_FALSE(net::Decode(std::span(bytes).first(bytes.size() - 7), back));
    EXPECT_EQ(bytes[bytes.size() - 7], 1);   // mode
    EXPECT_EQ(bytes[bytes.size() - 5], 3);   // max charges
    net::Roster four;
    for (uint8_t i = 0; i < 4; ++i) four.players.push_back({i, static_cast<sim::TeamId>(1 + i % 2), 0, 0});
    EXPECT_EQ(net::Encode(four).size(), 26u);
    EXPECT_EQ(net::Encode(net::RoundUpdate{}).size(), 16u);
    EXPECT_EQ(net::Encode(net::KillNotice{}).size(), 9u);
}

// --- Fight: sessions ------------------------------------------------------------------------------------------

namespace {
    constexpr int kGround = 96;                    // flat ground: voxels y < 96 (12 m at 0.125 m)

    net::Token TestToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x71 + i * 7);
        return t;
    }

    voxel::WorldMaterials DefaultIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    /// 7 x 7 chunks of play (-16 .. 12 m at 0.125 m) on flat ground at 12 m.
    sim::RegionDesc FlatRegion() {
        sim::RegionDesc r;
        r.templateId = sim::RegionTemplateId::Empty;
        r.seed = 99;
        r.playMinChunk = {-4, -4};
        r.playMaxChunk = {3, 3};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = kGround;
        r.terrain.voxelSize = 0.125f;
        r.terrain.materials = DefaultIds();
        return r;
    }

    /// A deathmatch on the flat region: team spawns at (-6, -2) and (6, -2), two players start it, a 1-tick countdown,
    /// a stronger blast.
    sim::FightRules FightRulesForTests() {
        sim::FightRules f;
        f.mode = sim::GameMode::Deathmatch;
        f.teamSpawns = {glm::vec2(-6.0f, -2.0f), glm::vec2(6.0f, -2.0f)};
        f.minPlayers = 2;
        f.countdownTicks = 1;
        f.endTicks = 60;
        f.endSession = false;
        // 80 at the centre (the game's 60 is sim's pin): the victim of the kill scenario falls into the first crater,
        // and the second blast, at the crater's floor, then still takes the rest of its health.
        f.blast = {80, 1200};
        return f;
    }

    net::ServerSettings Settings(sim::FightRules fight = FightRulesForTests()) {
        net::ServerSettings s;
        s.session.token = TestToken();
        s.session.region = FlatRegion();
        s.session.packHidden = false;
        s.session.fight = fight;
        s.simulation.jobThreads = 2;
        s.simulation.physics.threads = 1;
        s.simulation.physics.maxBodies = 2048;
        s.simulation.physics.maxBodyPairs = 4096;
        s.simulation.physics.maxContactConstraints = 4096;
        return s;
    }

    /// A server and player clients on loopback links, one manual clock (MovementTests.cpp's Match).
    struct FightField {
        struct Bot {
            net::LoopbackTransport loop;
            std::unique_ptr<net::Client> client;
            std::unique_ptr<sim::Simulation> simulation;
            std::function<sim::PlayerInput(Bot&)> controls;
            uint32_t casts = 0;
        };

        explicit FightField(net::ServerSettings settings = Settings()) : server(std::move(settings), serverLoop) {
            started = server.Start();
            dt = started ? static_cast<double>(server.Simulation().FixedTimeStep()) : 1.0 / 60.0;
        }

        Bot& AddBot(std::function<sim::PlayerInput(Bot&)> controls = {}) {
            auto bot = std::make_unique<Bot>();
            net::ClientSettings cs;
            cs.token = TestToken();
            bot->client = std::make_unique<net::Client>(cs, bot->loop);
            bot->controls = controls ? std::move(controls) : [](Bot&) { return sim::PlayerInput{}; };
            net::LoopbackTransport::Connect(serverLoop, bot->loop);
            bots.push_back(std::move(bot));
            return *bots.back();
        }

        void ClientPass(Bot& bot) {
            if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation) {
                sim::SimulationConfig base;
                base.jobThreads = 2;
                base.chunkCapacity = 4096;
                base.maxParticles = 0;
                base.physics.threads = 1;
                base.physics.maxBodies = 2048;
                base.physics.maxBodyPairs = 4096;
                base.physics.maxContactConstraints = 4096;
                bot.simulation = std::make_unique<sim::Simulation>(bot.client->MakeSimulationConfig(base));
                ASSERT_TRUE(bot.simulation->Initialize(bot.client->MakeGenerator()));
                ASSERT_TRUE(bot.client->Attach(*bot.simulation));
            }
            const uint32_t n = bot.client->TicksDue(dt);
            for (uint32_t i = 0; i < n; ++i) {
                const sim::PlayerInput in = bot.client->Predicting() ? bot.controls(bot) : sim::PlayerInput{};
                const sim::PlayerStepResult r = bot.client->RunTick(in);
                bot.casts += r.cast ? 1u : 0u;
                if (bot.simulation && bot.simulation->Terrain().Stats().pending != 0) bot.simulation->Jobs().WaitIdle();
            }
        }

        void Pass() {
            clock += dt;
            server.Advance(dt);
            if (server.Simulation().Terrain().Stats().pending != 0) server.Simulation().Jobs().WaitIdle();
            for (const std::unique_ptr<Bot>& bot : bots) ClientPass(*bot);
        }
        void Passes(int count) {
            for (int i = 0; i < count; ++i) Pass();
        }
        bool Until(const std::function<bool()>& done, int maxPasses) {
            for (int i = 0; i < maxPasses; ++i) {
                if (done()) return true;
                Pass();
            }
            return done();
        }
        bool Join(int maxPasses = 600) {
            return Until([this] {
                for (const std::unique_ptr<Bot>& bot : bots)
                    if (!bot->client->Predicting()) return false;
                return true;
            }, maxPasses);
        }
        glm::vec3 Feet(uint8_t slot) const { return server.GetPlayer(slot).value_or(sim::PlayerState{}).position; }

        double clock = 0.0;
        double dt = 1.0 / 60.0;
        net::LoopbackTransport serverLoop;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Bot>> bots;
    };

    /// Controls that aim the bot's eye at `target()` (feet, 0.1 m up) and press Cast on the ticks `cast` says.
    std::function<sim::PlayerInput(FightField::Bot&)> Aimer(std::function<glm::vec3()> target, std::function<bool(uint32_t tick)> cast) {
        return [target = std::move(target), cast = std::move(cast)](FightField::Bot& bot) {
            sim::PlayerInput in;
            const glm::vec3 eye = bot.client->Player().position + glm::vec3(0.0f, bot.client->Tuning().eyeHeight, 0.0f);
            sim::AimAngles(target() + glm::vec3(0.0f, 0.1f, 0.0f) - eye, in.yaw, in.pitch);
            if (cast(bot.client->PredictTick())) in.buttons = sim::PlayerButton_Cast;
            return in;
        };
    }

    /// What the kill scenario saw (one run feeds the tests that read it).
    struct KillRun {
        bool ok = false;
        uint32_t killsCounted = 0, killerKills = 0;
        std::array<uint16_t, 2> score{};
        bool victimSawDead = false;
        uint8_t respawnTicksSeen = 0, respawnTicksLater = 0;
        bool aliveAfter = false;
        int16_t healthAfter = 0;
        uint8_t lifeSeqAfter = 0;
        float fromSpawn = 1.0e9f;
        bool frozenWhileDead = false;
        net::CastRejection deadCast = net::CastRejection::None;
        uint64_t correctionsBefore = 0, correctionsAfter = 0, snapsBefore = 0, snapsAfter = 0;
        uint64_t respawns = 0, deathsSeen = 0;
        float errorAfterRespawn = 1.0e9f;
        net::Roster killerRoster;
        net::RoundUpdate killerRound;
        std::vector<net::KillNotice> notices, noticesAgain;
        bool deadBitSeen = false;
    };

    /// Slot 0 (team 1) kills slot 1 (team 2), 2.5 m in front of it, with blasts at its feet 20 ticks apart (80 + the rest;
    /// a third if the crater took the second); slot 1, dead,
    /// pushes every button and casts; it respawns at team 2's spawn 180 ticks later.
    KillRun RunKill() {
        KillRun out;
        FightField f;
        if (!f.started) return out;
        bool fire = false;
        uint32_t firstCast = 0;
        auto victimFeet = [&f] { return f.Feet(1); };
        FightField::Bot& attacker = f.AddBot(Aimer(victimFeet, [&](uint32_t tick) {
            return fire && (tick == firstCast || tick == firstCast + 20 || tick == firstCast + 40);
        }));
        bool victimPushes = false;
        FightField::Bot& victim = f.AddBot([&](FightField::Bot& bot) {
            sim::PlayerInput in;
            if (!victimPushes) return in;
            in.moveZ = 127;
            in.moveX = 50;
            const uint32_t t = bot.client->PredictTick();
            in.buttons = static_cast<uint8_t>(sim::PlayerButton_Jump | sim::PlayerButton_Run | ((t / 4) % 2 == 0 ? sim::PlayerButton_Cast : 0));
            return in;
        });
        if (!f.Join()) return out;
        if (!f.Until([&] { return f.server.RoundState().phase == sim::RoundPhase::Live; }, 600)) return out;
        f.Passes(130);                                        // spawn protection (120 ticks) over
        // The victim stands 4 m in front of the attacker.
        f.server.TeleportPlayer(1, f.Feet(0) + glm::vec3(2.5f, 0.05f, 0.0f));
        f.Passes(60);
        firstCast = attacker.client->PredictTick() + 5;
        fire = true;
        if (!f.Until([&] { return f.server.Fighter(1).dead; }, 200)) {
            const net::ServerStats& st = f.server.Stats();
            std::printf("no kill: %u casts from inputs, %u run, %u missed, %u on cooldown, %u damage events (%u hp), victim health %d at (%.2f, %.2f, %.2f), "
                        "attacker at (%.2f, %.2f, %.2f), first cast tick %u\n",
                        st.castsFromInputs, st.castsRun, st.castsMissed, st.castsOnCooldown, st.damageEvents, st.damageTotal, f.server.Fighter(1).health,
                        f.Feet(1).x, f.Feet(1).y, f.Feet(1).z, f.Feet(0).x, f.Feet(0).y, f.Feet(0).z, firstCast);
            return out;
        }
        f.Passes(10);                                         // the death reaches the victim (the craters' corrections come before)
        out.correctionsBefore = victim.client->Prediction().corrections;
        out.snapsBefore = victim.client->Prediction().snaps;
        out.killsCounted = f.server.Stats().kills;
        out.killerKills = f.server.Fighter(0).kills;
        out.score = f.server.RoundState().score;
        fire = false;
        f.Passes(20);
        out.victimSawDead = victim.client->Fight().dead;
        out.respawnTicksSeen = victim.client->Fight().respawnTicks;
        for (const net::RemotePlayerView& o : attacker.client->RemotePlayers()) out.deadBitSeen = out.deadBitSeen || (o.slot == 1 && o.Dead());
        // Dead: every button pushed, nothing moves; its cast edges are refused Dead.
        const glm::vec3 frozen = f.Feet(1);
        victimPushes = true;
        f.Passes(40);
        out.frozenWhileDead = f.Feet(1) == frozen;
        out.deadCast = victim.client->LastCast().rejected;
        out.respawnTicksLater = victim.client->Fight().respawnTicks;
        victimPushes = false;
        out.killerRoster = attacker.client->RosterOf();
        out.killerRound = attacker.client->RoundOf();
        out.notices = attacker.client->TakeKillNotices();
        out.noticesAgain = attacker.client->TakeKillNotices();
        if (!f.Until([&] { return !f.server.Fighter(1).dead; }, 200)) return out;
        f.Passes(40);
        out.aliveAfter = !victim.client->Fight().dead;
        out.healthAfter = f.server.Fighter(1).health;
        out.lifeSeqAfter = victim.client->Fight().lifeSeq;
        const glm::vec3 feet = f.Feet(1);
        out.fromSpawn = glm::length(glm::vec2(feet.x, feet.z) - glm::vec2(6.0f, -2.0f));
        out.correctionsAfter = victim.client->Prediction().corrections;
        out.snapsAfter = victim.client->Prediction().snaps;
        out.respawns = victim.client->Prediction().respawns;
        out.deathsSeen = victim.client->Prediction().deathsSeen;
        out.errorAfterRespawn = victim.client->Prediction().lastError;
        out.ok = true;
        return out;
    }
}

TEST(Fight, TeamsAreBalancedAtJoin) {
    FightField f;
    ASSERT_TRUE(f.started);
    for (int i = 0; i < 3; ++i) f.AddBot();
    ASSERT_TRUE(f.Join());
    f.Passes(10);
    EXPECT_EQ(f.server.Fighter(0).team, 1);
    EXPECT_EQ(f.server.Fighter(1).team, 2);
    EXPECT_EQ(f.server.Fighter(2).team, 1);
    for (const auto& bot : f.bots) {
        const net::Roster& r = bot->client->RosterOf();
        ASSERT_EQ(r.players.size(), 3u);
        for (uint8_t s = 0; s < 3; ++s) EXPECT_EQ(bot->client->TeamOf(s), s == 1 ? 2 : 1);
    }
    // Each starts at its team's spawn (2 m ring), not the template's.
    for (uint8_t s = 0; s < 3; ++s) {
        const glm::vec3 feet = f.Feet(s);
        const glm::vec2 spawn = s == 1 ? glm::vec2(6.0f, -2.0f) : glm::vec2(-6.0f, -2.0f);
        EXPECT_LT(glm::length(glm::vec2(feet.x, feet.z) - spawn), 2.1f) << "slot " << int(s);
    }
}

TEST(Fight, ABlastKillsAndTheVictimRespawnsAtItsTeamSpawnAfter180Ticks) {
    const KillRun r = RunKill();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.killsCounted, 1u);
    EXPECT_EQ(r.killerKills, 1);
    EXPECT_EQ(r.score, (std::array<uint16_t, 2>{1, 0}));
    EXPECT_TRUE(r.victimSawDead);
    EXPECT_GT(r.respawnTicksSeen, r.respawnTicksLater);       // counting down
    EXPECT_GT(r.respawnTicksSeen, 0);
    EXPECT_TRUE(r.aliveAfter);
    EXPECT_EQ(r.healthAfter, 100);
    EXPECT_EQ(r.lifeSeqAfter, 2);
    EXPECT_LT(r.fromSpawn, 2.1f);
}

TEST(Fight, ADeadPlayerIsNotSteppedAndItsCastsAreRefusedDead) {
    const KillRun r = RunKill();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.frozenWhileDead);
    EXPECT_EQ(r.deadCast, net::CastRejection::Dead);
}

TEST(Fight, ARespawnIsNotACorrection) {
    const KillRun r = RunKill();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.correctionsAfter, r.correctionsBefore);
    EXPECT_EQ(r.snapsAfter, r.snapsBefore);
    EXPECT_EQ(r.respawns, 1u);
    EXPECT_EQ(r.deathsSeen, 1u);
    EXPECT_LT(r.errorAfterRespawn, 0.02f);
}

TEST(Fight, TheClientSeesRosterRoundAndKills) {
    const KillRun r = RunKill();
    ASSERT_TRUE(r.ok);
    bool listed = false;
    for (const net::RosterEntry& e : r.killerRoster.players)
        if (e.slot == 0) listed = e.kills == 1;
    EXPECT_TRUE(listed);
    EXPECT_EQ(r.killerRound.score, (std::array<uint16_t, 2>{1, 0}));
    EXPECT_EQ(r.killerRound.phase, sim::RoundPhase::Live);
    ASSERT_EQ(r.notices.size(), 1u);
    EXPECT_EQ(r.notices[0].victim, 1);
    EXPECT_EQ(r.notices[0].killer, 1);                        // slot 0 + 1
    EXPECT_EQ(r.notices[0].cause, sim::DamageCause::Blast);
    EXPECT_EQ(r.notices[0].flags, net::KillNotice::kCredited);
    EXPECT_TRUE(r.noticesAgain.empty());
}

TEST(Fight, RemoteDeadPlayersCarryTheDeadBit) {
    const KillRun r = RunKill();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.deadBitSeen);
}

TEST(Fight, CastsWithoutAChargeAreRefusedNoCharge) {
    FightField f;
    ASSERT_TRUE(f.started);
    uint32_t first = 0;
    std::vector<uint32_t> at;
    auto ground = [&f] { return f.Feet(0) + glm::vec3(0.0f, 0.0f, -5.0f); };
    f.AddBot(Aimer(ground, [&](uint32_t tick) { return std::find(at.begin(), at.end(), tick) != at.end(); }));
    f.AddBot();
    ASSERT_TRUE(f.Join());
    ASSERT_TRUE(f.Until([&] { return f.server.RoundState().phase == sim::RoundPhase::Live; }, 600));
    f.Passes(20);
    first = f.bots[0]->client->PredictTick() + 5;
    at = {first, first + 16, first + 32, first + 48};
    f.Passes(80);
    EXPECT_EQ(f.server.Stats().castsRun, 3u);
    EXPECT_EQ(f.server.Stats().castsRefusedNoCharge, 1u);
    EXPECT_EQ(f.bots[0]->client->LastCast().rejected, net::CastRejection::NoCharge);
    at = {first + 80};                                         // the first charge is back at first + 72
    f.Passes(40);
    EXPECT_EQ(f.server.Stats().castsRun, 4u);
    EXPECT_EQ(f.server.Stats().castsRefusedNoCharge, 1u);
}

TEST(Fight, ARoundEndsAtTheScoreLimit) {
    sim::FightRules rules = FightRulesForTests();
    rules.scoreLimit = 2;
    rules.respawnTicks = 30;
    rules.protectTicks = 10;
    rules.endSession = true;
    FightField f(Settings(rules));
    ASSERT_TRUE(f.started);
    bool fire = false;
    uint32_t firstCast = 0;
    f.AddBot(Aimer([&f] { return f.Feet(1); }, [&](uint32_t tick) { return fire && (tick == firstCast || tick == firstCast + 20 || tick == firstCast + 40); }));
    f.AddBot();
    ASSERT_TRUE(f.Join());
    ASSERT_TRUE(f.Until([&] { return f.server.RoundState().phase == sim::RoundPhase::Live; }, 600));
    for (int kill = 0; kill < 2; ++kill) {
        f.Passes(20);
        f.server.TeleportPlayer(1, f.Feet(0) + glm::vec3(2.5f, 0.05f, 0.0f));
        f.Passes(30);
        firstCast = f.bots[0]->client->PredictTick() + 5;
        fire = true;
        ASSERT_TRUE(f.Until([&] { return f.server.Stats().kills == static_cast<uint32_t>(kill + 1); }, 200)) << "kill " << kill;
        fire = false;
        if (kill == 0) ASSERT_TRUE(f.Until([&] { return !f.server.Fighter(1).dead; }, 200));
    }
    EXPECT_EQ(f.server.RoundState().phase, sim::RoundPhase::Ended);
    EXPECT_EQ(f.server.RoundState().winner, 1);
    EXPECT_EQ(f.server.RoundState().end, sim::RoundEnd::Score);
    f.Passes(10);
    for (const auto& bot : f.bots) {
        EXPECT_EQ(bot->client->RoundOf().phase, sim::RoundPhase::Ended);
        EXPECT_EQ(bot->client->RoundOf().winner, 1);
        EXPECT_EQ(bot->client->RoundOf().score, (std::array<uint16_t, 2>{2, 0}));
    }
    EXPECT_FALSE(f.server.SessionOver());
    ASSERT_TRUE(f.Until([&] { return f.server.SessionOver(); }, 120));
    f.Passes(5);
    EXPECT_EQ(f.server.ClientCount(), 0u);                    // end_session: every peer disconnected
}

TEST(Fight, SandboxSessionsKeepTodaysCasts) {
    FightField f(Settings(sim::SandboxRules()));
    ASSERT_TRUE(f.started);
    uint32_t first = 0;
    f.AddBot(Aimer([&f] { return f.Feet(0) + glm::vec3(0.0f, 0.0f, -5.0f); },
                   [&](uint32_t tick) { return first != 0 && tick >= first && tick < first + 160 && (tick - first) % 16 == 0; }));
    f.AddBot();
    ASSERT_TRUE(f.Join());
    f.Passes(20);
    first = f.bots[0]->client->PredictTick() + 5;
    f.Passes(200);
    EXPECT_EQ(f.server.Stats().castsRun, 10u);
    EXPECT_EQ(f.server.Stats().damageEvents, 0u);
    EXPECT_EQ(f.server.Stats().castsRefusedNoCharge, 0u);
    EXPECT_EQ(f.server.RoundState().phase, sim::RoundPhase::Warmup);
    const net::FightView& v = f.bots[0]->client->Fight();
    EXPECT_FALSE(v.Active());
    EXPECT_EQ(v.health, 100);
    EXPECT_EQ(v.charges, 0);
    EXPECT_FALSE(v.dead || v.protectedNow || v.spectator);
    EXPECT_EQ(f.server.Fighter(0).team, sim::kNoTeam);
}

TEST(Fight, FallsAndLavaHurtOnlyInALiveRound) {
    FightField f;
    ASSERT_TRUE(f.started);
    f.AddBot();
    ASSERT_TRUE(f.Join());
    f.Passes(20);
    // Warmup (one player): a 10 m fall hurts nobody.
    ASSERT_EQ(f.server.RoundState().phase, sim::RoundPhase::Warmup);
    f.server.TeleportPlayer(0, f.Feet(0) + glm::vec3(0.0f, 10.0f, 0.0f));
    f.Passes(120);
    EXPECT_EQ(f.server.Fighter(0).health, 100);
    EXPECT_TRUE(f.server.GetPlayer(0)->OnGround());
    f.AddBot();
    ASSERT_TRUE(f.Join());
    ASSERT_TRUE(f.Until([&] { return f.server.RoundState().phase == sim::RoundPhase::Live; }, 600));
    f.Passes(130);                                             // spawn protection over
    f.server.TeleportPlayer(0, f.Feet(0) + glm::vec3(0.0f, 10.0f, 0.0f));
    f.Passes(120);
    const int expected = 100 - sim::FallDamage(sim::FightRules{}, -std::sqrt(2.0f * 9.81f * 10.0f));
    EXPECT_NEAR(f.server.Fighter(0).health, expected, 2);
    EXPECT_LT(f.server.Fighter(0).health, 100);
    // Lava under slot 1's feet: 20 at once, 40 after 15 ticks.
    const glm::vec3 feet = f.Feet(1);
    const glm::ivec3 v = f.server.Simulation().World().WorldToVoxel(glm::dvec3(feet) - glm::dvec3(0.0, 0.05, 0.0));
    const uint8_t lava = f.server.Simulation().WorldMaterialIds().lava;
    ASSERT_NE(f.server.ApplyOp(sim::WorldOp::Box(v - glm::ivec3(6, 1, 6), v + glm::ivec3(7, 1, 7), lava)), 0u);
    ASSERT_TRUE(f.Until([&] { return f.server.Fighter(1).health < 100; }, 10));
    EXPECT_EQ(f.server.Fighter(1).health, 80);
    f.Passes(15);
    EXPECT_EQ(f.server.Fighter(1).health, 60);
}

// fight.md "Bots": four fighting pilots, two a side, play a round to its score limit (2) on the flat field.
TEST(Fight, ATwoVersusTwoBotMatchEndsWithAWinner) {
    sim::FightRules rules = FightRulesForTests();
    rules.minPlayers = 4;
    rules.scoreLimit = 2;
    rules.roundTicks = 120 * 60;
    FightField f(Settings(rules));
    ASSERT_TRUE(f.started);
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < 4; ++i) {
        net::PilotSettings ps;
        ps.fight = true;
        auto pilot = std::make_shared<net::BotPilot>(FlatRegion(), 0xb07 + i, ps);
        f.AddBot([pilot](FightField::Bot& bot) {
            net::Client& c = *bot.client;
            if (c.Fight().dead) return sim::PlayerInput{};
            const sim::TeamId own = c.TeamOf(c.Slot());
            std::vector<net::PilotTarget> enemies;
            for (const net::RemotePlayerView& o : c.RemotePlayers())
                if (c.TeamOf(o.slot) != sim::kNoTeam && c.TeamOf(o.slot) != own && !o.Dead()) enemies.push_back({o.slot, o.position});
            pilot->SetTargets(enemies);
            pilot->SetCanCast((c.Fight().charges > 0 || c.Fight().maxCharges == 0) && c.Player().cooldownTicks == 0);
            return pilot->Next(c.Player());
        });
    }
    ASSERT_TRUE(f.Join());
    ASSERT_TRUE(f.Until([&] { return f.server.RoundState().phase == sim::RoundPhase::Ended; }, 130 * 60));
    const sim::Round& round = f.server.RoundState();
    EXPECT_TRUE(round.winner == 1 || round.winner == 2) << "winner " << int(round.winner);
    // The session stays replicated: the bots stand, the world settles, every client's hash is the server's.
    for (const auto& bot : f.bots) bot->controls = [](FightField::Bot&) { return sim::PlayerInput{}; };
    ASSERT_TRUE(f.Until([&] {
        if (f.server.QueuedCasts() > 0 || f.server.AwakeBodies() > 0) return false;
        for (const auto& bot : f.bots)
            if (bot->client->AppliedOpSeq() != f.server.LastOpSeq() || !bot->client->Synced()) return false;
        return true;
    }, 1200));
    for (const auto& bot : f.bots) EXPECT_EQ(bot->client->Hash().hash, f.server.Hash(bot->client->SyncWindow()).hash);
    std::printf("2v2 bot match: team %d won %d-%d by %s at tick %u (%u kills, %u casts run) in %.1f s wall\n", round.winner, round.score[0],
                round.score[1], sim::ToString(round.end), f.server.TickNumber(), f.server.Stats().kills, f.server.Stats().castsRun,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}
