// docs/design/magic.md ([mag-2]): spells in a session. The server captures a Spell edge at its input's tick (dead,
// the capture checks, a charge, the form cooldown), runs it in the cast queue (sim::CastSpell: CastFx, the operations
// through EmitOp, ZoneStart with the lead, ZoneCancel, damage through DealDamage), runs its follow-ups every tick, and
// takes Attune while a player is dead or before its first spawn. A Dash starts at its Spell edge inside StepPlayer on
// the server and on the predicting client alike. One process, loopback links, one manual clock (FightTests.cpp's
// FightField as `SpellYard`).

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    constexpr int kGround = 96;                    // flat ground: voxels y < 96 (12 m at 0.125 m)
    constexpr float kGroundMetres = kGround * 0.125f;

    net::Token TestToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x29 + i * 5);
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
        r.seed = 41;
        r.playMinChunk = {-4, -4};
        r.playMaxChunk = {3, 3};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = kGround;
        r.terrain.voxelSize = 0.125f;
        r.terrain.materials = DefaultIds();
        return r;
    }

    /// A deathmatch of two: spawns at (-6, -2) and (6, -2), a 1-tick countdown, no charge back during the test, a quick
    /// respawn and a blast that kills at its centre.
    sim::FightRules Duel() {
        sim::FightRules f;
        f.mode = sim::GameMode::Deathmatch;
        f.teamSpawns = {glm::vec2(-6.0f, -2.0f), glm::vec2(6.0f, -2.0f)};
        f.minPlayers = 2;
        f.countdownTicks = 1;
        f.endTicks = 60;
        f.endSession = false;
        f.chargeTicks = 6000;
        f.respawnTicks = 30;
        f.blast = {200, 1200};
        return f;
    }

    net::ServerSettings Settings(sim::FightRules fight = sim::SandboxRules()) {
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

    /// A peer that speaks the protocol by hand: it joins with a Hello and keeps every message it gets.
    struct StreamWatcher {
        net::LoopbackTransport loop;
        net::PeerId server = 0;
        std::vector<net::TransportEvent> received;
        void Poll() {
            net::TransportEvent e;
            while (loop.Poll(e)) {
                if (e.type == net::TransportEvent::Type::Connected) server = e.peer;
                if (e.type == net::TransportEvent::Type::Received) received.push_back(std::move(e));
            }
        }
        /// Every event of the opSeq stream received so far, with its opSeq.
        std::vector<std::pair<uint32_t, net::Event>> Events() const {
            std::vector<std::pair<uint32_t, net::Event>> out;
            for (const net::TransportEvent& e : received) {
                if (net::PeekType(e.data) != net::MessageType::EventBatch) continue;
                net::EventBatch batch;
                EXPECT_TRUE(net::Decode(e.data, batch));
                for (size_t i = 0; i < batch.events.size(); ++i) out.emplace_back(batch.firstOpSeq + static_cast<uint32_t>(i), batch.events[i]);
            }
            return out;
        }
    };

    /// A server and player clients on loopback links, one manual clock.
    struct SpellYard {
        struct Bot {
            net::LoopbackTransport loop;
            std::unique_ptr<net::Client> client;
            std::unique_ptr<sim::Simulation> simulation;
            std::function<sim::PlayerInput(Bot&)> controls;
            std::map<uint32_t, net::CastRejection> rejections;   ///< the owner block's word on each cast, by input tick
        };

        explicit SpellYard(net::ServerSettings settings = Settings()) : server(std::move(settings), serverLoop) {
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

        void Watch(StreamWatcher& w) {
            net::LoopbackTransport::Connect(serverLoop, w.loop);
            w.Poll();
            w.loop.Send(w.server, net::Channel::Events, net::Encode(net::Hello{net::kProtocolVersion, server.BuildHashValue(), TestToken()}));
            watchers.push_back(&w);
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
                bot.client->RunTick(in);
                if (bot.simulation && bot.simulation->Terrain().Stats().pending != 0) bot.simulation->Jobs().WaitIdle();
            }
            const net::Client::CastStatus cast = bot.client->LastCast();
            if (cast.tick != 0) bot.rejections[cast.tick] = cast.rejected;
        }

        void Pass() {
            clock += dt;
            server.Advance(dt);
            if (server.Simulation().Terrain().Stats().pending != 0) server.Simulation().Jobs().WaitIdle();
            for (const std::unique_ptr<Bot>& bot : bots) ClientPass(*bot);
            for (StreamWatcher* w : watchers) w->Poll();
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
        /// Every bot has every event and a synced window: its hash equals the server's over its box.
        bool HashesEqual() {
            if (!Until([this] {
                    for (const std::unique_ptr<Bot>& bot : bots)
                        if (bot->client->AppliedOpSeq() != server.LastOpSeq() || !bot->client->Synced()) return false;
                    return true;
                }, 600))
                return false;
            for (const std::unique_ptr<Bot>& bot : bots)
                if (bot->client->Hash().hash != server.Hash(bot->client->SyncWindow()).hash) return false;
            return true;
        }
        glm::vec3 Feet(uint8_t slot) const { return server.GetPlayer(slot).value_or(sim::PlayerState{}).position; }

        double clock = 0.0;
        double dt = 1.0 / 60.0;
        net::LoopbackTransport serverLoop;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Bot>> bots;
        std::vector<StreamWatcher*> watchers;
    };

    /// The bot's look at `target` (metres) from its predicted eye.
    sim::PlayerInput LookAt(const net::Client& client, const glm::vec3& target) {
        sim::PlayerInput in;
        const glm::vec3 eye = client.Player().position + glm::vec3(0.0f, client.Tuning().eyeHeight, 0.0f);
        sim::AimAngles(target - eye, in.yaw, in.pitch);
        return in;
    }

    /// Spells cast by input tick: the spell and where the bot looks, as an offset from its feet (metres).
    struct SpellPlan {
        std::map<uint32_t, std::pair<sim::SpellId, glm::vec3>> casts;
        std::function<sim::PlayerInput(SpellYard::Bot&)> Controls() {
            return [this](SpellYard::Bot& bot) {
                const uint32_t tick = bot.client->PredictTick();
                const auto it = casts.find(tick);
                if (it == casts.end()) return sim::PlayerInput{};
                sim::PlayerInput in = LookAt(*bot.client, bot.client->Player().position + it->second.second);
                in.buttons = sim::PlayerButton_Spell;
                in.spell = it->second.first;
                return in;
            };
        }
    };
}

// A Barricade Wave cast by a client: the server runs it in the cast queue and the stream carries its CastFx first, then
// its knit bands (MapMask with the Knit map) on their ticks; every client ends with the server's voxels.
TEST(Magic, ServerRunsASpellAndEmitsItsEvents) {
    SpellYard y;
    ASSERT_TRUE(y.started);
    SpellPlan plan;
    SpellYard::Bot& caster = y.AddBot(plan.Controls());
    SpellYard::Bot& other = y.AddBot();
    StreamWatcher watcher;
    y.Watch(watcher);
    ASSERT_TRUE(y.Join());
    y.Passes(30);
    const uint32_t castTick = caster.client->PredictTick() + 5;
    plan.casts[castTick] = {sim::kBarricadeWave, glm::vec3(3.0f, 0.0f, 0.0f)};
    y.Passes(240);

    EXPECT_EQ(y.server.Stats().spellCasts, 1u);
    EXPECT_EQ(y.server.Stats().spellsRefused, 0u);
    EXPECT_GT(y.server.Stats().spellOps, 0u);
    ASSERT_TRUE(caster.rejections.contains(castTick));
    EXPECT_EQ(caster.rejections[castTick], net::CastRejection::None);

    std::optional<uint32_t> fxSeq;
    uint32_t bandsAfter = 0, magicOpsBefore = 0;
    for (const auto& [seq, event] : watcher.Events()) {
        if (const net::CastFx* fx = std::get_if<net::CastFx>(&event)) {
            EXPECT_FALSE(fxSeq.has_value()) << "one CastFx";
            fxSeq = seq;
            EXPECT_EQ(fx->spell, sim::kBarricadeWave);
            EXPECT_EQ(fx->slot, caster.client->Slot() + 1u);
            EXPECT_GT(fx->tick, 0u);
            continue;
        }
        const sim::WorldOp* op = std::get_if<sim::WorldOp>(&event);
        if (op == nullptr || op->kind != sim::WorldOpKind::MapMask) continue;
        if (!fxSeq) {
            ++magicOpsBefore;
            continue;
        }
        EXPECT_EQ(op->map, voxel::Map_Knit);
        ++bandsAfter;
    }
    ASSERT_TRUE(fxSeq.has_value());
    EXPECT_EQ(magicOpsBefore, 0u);
    EXPECT_GE(bandsAfter, 2u) << "the bands follow the CastFx on their ticks";
    EXPECT_EQ(caster.client->Stats().castFx, 1u);
    EXPECT_EQ(other.client->Stats().castFx, 1u);
    EXPECT_TRUE(y.HashesEqual());
}

// magic.md spell 11 and "PlayerState, StepPlayer and the physics call" step 1: the Dash starts at its Spell edge inside
// StepPlayer on the server and on the predicting client, at the input's tick: the client predicts it with no correction,
// and the server's run of the spell only traces the path and announces the Impulse (it never starts the dash again).
TEST(Magic, ADashStartsAtTheSpellEdgeOnBothSides) {
    SpellYard y;
    ASSERT_TRUE(y.started);
    SpellPlan plan;
    SpellYard::Bot& dasher = y.AddBot(plan.Controls());
    ASSERT_TRUE(y.Join());
    y.Passes(30);
    const uint8_t slot = dasher.client->Slot();
    const glm::vec3 start = y.Feet(slot);
    const uint64_t corrections = dasher.client->Prediction().corrections;
    const uint32_t castTick = dasher.client->PredictTick() + 5;
    plan.casts[castTick] = {sim::kDash, glm::vec3(4.0f, 1.62f, 0.0f)};   // a level look along +x
    y.Passes(90);

    EXPECT_EQ(y.server.Stats().spellCasts, 1u);
    EXPECT_EQ(y.server.Stats().zonesEmitted, 1u) << "the Impulse along the path";
    const glm::vec3 end = y.Feet(slot);
    EXPECT_NEAR(end.x - start.x, 8.0f, 0.8f) << "one dash of 18 ticks, never two";
    EXPECT_NEAR(end.z, start.z, 0.05f);
    EXPECT_EQ(dasher.client->Prediction().corrections, corrections) << "the client predicted the dash from its own edge";
    EXPECT_LT(glm::length(dasher.client->Player().position - end), 0.05f);
    EXPECT_EQ(dasher.rejections[castTick], net::CastRejection::None);
}

// The Lift of an Updraft under its own caster reaches the client in the opSeq stream before its start tick (the lead):
// the client predicts the climb with no correction.
TEST(Magic, AnUpdraftUnderItsCasterIsPredictedWithoutACorrection) {
    SpellYard y;
    ASSERT_TRUE(y.started);
    SpellPlan plan;
    SpellYard::Bot& caster = y.AddBot(plan.Controls());
    ASSERT_TRUE(y.Join());
    y.Passes(30);
    const uint8_t slot = caster.client->Slot();
    const float ground = y.Feet(slot).y;
    const uint64_t corrections = caster.client->Prediction().corrections;
    const uint32_t castTick = caster.client->PredictTick() + 5;
    plan.casts[castTick] = {sim::kUpdraft, glm::vec3(0.02f, -1.0f, 0.0f)};   // straight down at the own feet
    float top = ground;
    for (int i = 0; i < 360; ++i) {   // the climb, the hover and the fall back to the ground
        y.Pass();
        top = std::max(top, y.Feet(slot).y);
    }
    EXPECT_EQ(y.server.Stats().zonesEmitted, 1u);
    EXPECT_EQ(caster.client->Stats().zonesApplied, 1u);
    EXPECT_GT(top - ground, 3.0f) << "the Lift carried the caster up";
    EXPECT_EQ(caster.client->Prediction().corrections, corrections);
    EXPECT_NEAR(caster.client->Player().position.y, y.Feet(slot).y, 0.05f) << "both on the ground again";
}

// magic.md "Caps" and "Casting: mana, gestures and projectiles": a Spell edge inside the gap is refused at capture
// (TooSoon); a zone over the caster's cap is refused when the spell runs (TooManyZones: no mana is charged); a spell
// whose zone meets an enemy spawn area is refused (NearSpawn). Spells cost no charge.
TEST(Magic, CastRejectedOverTheCaps) {
    net::ServerSettings settings = Settings(Duel());
    settings.maxZonesPerPlayer = 1;
    SpellYard y(settings);
    ASSERT_TRUE(y.started);
    SpellPlan planA, planB;
    SpellYard::Bot& a = y.AddBot(planA.Controls());
    SpellYard::Bot& b = y.AddBot(planB.Controls());
    ASSERT_TRUE(y.Join());
    ASSERT_TRUE(y.Until([&] { return y.server.RoundState().phase == sim::RoundPhase::Live; }, 600));
    y.Passes(40);
    const uint8_t slotA = a.client->Slot(), slotB = b.client->Slot();
    ASSERT_EQ(y.server.Fighter(slotA).team, 1u);
    ASSERT_EQ(y.server.Fighter(slotB).team, 2u);
    ASSERT_EQ(y.server.Fighter(slotA).charges, 3u);
    const uint32_t t0 = a.client->PredictTick() + 5;
    planA.casts[t0] = {sim::kUpdraft, glm::vec3(2.0f, 0.0f, 0.0f)};        // its own Lift, 2 m ahead
    planA.casts[t0 + 6] = {sim::kUpdraft, glm::vec3(2.0f, 0.0f, 0.0f)};    // inside the 12-tick gap
    planA.casts[t0 + 40] = {sim::kPunch, glm::vec3(2.0f, 0.0f, 0.0f)};     // a second zone while its Lift lives
    const glm::vec3 enemySpawn(-5.0f, kGroundMetres, 1.0f);   // inside team 1's spawn area, beside its player
    const uint32_t tB = b.client->PredictTick() + 5;
    planB.casts[tB] = {sim::kUpdraft, enemySpawn - y.Feet(slotB)};
    y.Passes(120);

    EXPECT_EQ(a.rejections[t0], net::CastRejection::None);
    EXPECT_EQ(a.rejections[t0 + 6], net::CastRejection::TooSoon);
    EXPECT_EQ(a.rejections[t0 + 40], net::CastRejection::TooManyZones);
    EXPECT_EQ(b.rejections[tB], net::CastRejection::NearSpawn);
    EXPECT_EQ(y.server.Stats().spellCasts, 1u);
    EXPECT_EQ(y.server.Stats().spellsRefused, 3u);
    EXPECT_EQ(y.server.Fighter(slotA).charges, 3u) << "spells cost no charge";
    EXPECT_EQ(y.server.Fighter(slotB).charges, 3u);
    const sim::SpellTable table = sim::BuildSpellTable(0.125f);
    const sim::CasterState casterA = y.server.Caster(slotA), casterB = y.server.Caster(slotB);
    EXPECT_GE(casterA.spellReadyTick, t0 + 40 + table.spellGapTicks) << "the Punch's capture started the gap";
    EXPECT_EQ(casterA.mana, table.manaPool - table.spells[sim::kUpdraft].manaCost) << "the Updraft alone paid, at its T_run";
    EXPECT_EQ(sim::ManaAt(table, casterB, casterB.manaTick + 1000), table.manaPool) << "B paid nothing it did not get back";
    EXPECT_TRUE(y.HashesEqual());
}

// Decision 12: Attune is taken before the first spawn and while dead (two different, built elements) and applied at the
// next spawn; while alive, or with an element no spell of this build has, it is ignored.
TEST(Magic, AttuneOnlyWhileDeadOrBeforeSpawn) {
    SpellYard y(Settings(Duel()));
    ASSERT_TRUE(y.started);
    SpellYard::Bot& a = y.AddBot();
    ASSERT_TRUE(y.Until([&] { return a.client->State() == net::ClientState::Playing; }, 600));
    y.Passes(10);
    const uint8_t slotA = a.client->Slot();
    ASSERT_FALSE(y.server.Fighter(slotA).spawned) << "alone in the warmup: not spawned yet";
    constexpr uint8_t kMetal = static_cast<uint8_t>(sim::Element::Metal), kPressure = static_cast<uint8_t>(sim::Element::Pressure),
                      kThermal = static_cast<uint8_t>(sim::Element::Thermal);
    a.client->SendAttune(kPressure, kMetal);
    a.client->SendAttune(kThermal, kMetal);                  // Thermal has no built spell: ignored, the first stays
    y.Passes(10);
    EXPECT_EQ(y.server.Stats().attunes, 1u);
    EXPECT_EQ(y.server.Stats().attunesRefused, 1u);
    EXPECT_EQ(y.server.Caster(slotA).elementA, kMetal) << "applied at the spawn, not before";

    // The second player starts the round: the spawn applies the pair.
    bool fire = false;
    SpellYard::Bot& b = y.AddBot([&](SpellYard::Bot& bot) {
        sim::PlayerInput in = LookAt(*bot.client, y.Feet(slotA) + glm::vec3(0.0f, 0.1f, 0.0f));
        if (fire && bot.client->PredictTick() % 20 == 0) in.buttons = sim::PlayerButton_Cast;
        return in;
    });
    ASSERT_TRUE(y.Join());
    ASSERT_TRUE(y.Until([&] { return y.server.RoundState().phase == sim::RoundPhase::Live; }, 600));
    y.Passes(10);
    EXPECT_EQ(y.server.Caster(slotA).elementA, kPressure);
    EXPECT_EQ(y.server.Caster(slotA).elementB, kMetal);
    ASSERT_TRUE(y.Until([&] {
        for (const net::RosterEntry& e : b.client->RosterOf().players)
            if (e.slot == slotA) return e.attune == (kPressure | kMetal << 2);
        return false;
    }, 120)) << "the roster carries the pair";
    EXPECT_EQ(a.client->Magic().attune & 0x0F, kPressure | kMetal << 2);

    // Alive: ignored.
    a.client->SendAttune(kMetal, kPressure);
    y.Passes(10);
    EXPECT_EQ(y.server.Stats().attunesRefused, 2u);
    EXPECT_EQ(y.server.Caster(slotA).elementA, kPressure);

    // Dead: taken, and the respawn applies it.
    y.Passes(130);                                           // past the spawn protection
    fire = true;
    ASSERT_TRUE(y.Until([&] { return y.server.Fighter(slotA).dead; }, 600));
    fire = false;
    a.client->SendAttune(kMetal, kPressure);
    y.Passes(5);
    EXPECT_EQ(y.server.Stats().attunes, 2u);
    EXPECT_EQ(y.server.Caster(slotA).elementA, kPressure) << "not before the respawn";
    ASSERT_TRUE(y.Until([&] { return !y.server.Fighter(slotA).dead; }, 600));
    EXPECT_EQ(y.server.Caster(slotA).elementA, kMetal);
    EXPECT_EQ(y.server.Caster(slotA).elementB, kPressure);
    // [mag-8] A new life starts with a full pool, refilled at the respawn.
    EXPECT_EQ(y.server.Caster(slotA).mana, 6000);
    EXPECT_EQ(y.server.Caster(slotA).manaTick, y.server.Fighter(slotA).lifeStartTick);
}

// --- casting by gesture ([mag-8], magic.md "Casting: mana, gestures and projectiles") ------------------------------

namespace {
    /// A Spell edge at `tick` with a gesture: the bot looks at the release point and sends the gesture with the edge.
    struct GesturePlan {
        uint32_t tick = 0;
        sim::SpellId spell = 0;
        sim::SpellGesture gesture;
        std::function<sim::PlayerInput(SpellYard::Bot&)> Controls() {
            return [this](SpellYard::Bot& bot) {
                if (tick == 0 || bot.client->PredictTick() != tick) return sim::PlayerInput{};
                bot.client->SetSpellGesture(gesture);
                const glm::vec3 release = glm::vec3(glm::dvec3(gesture.pointsQ.back()) / 128.0);
                sim::PlayerInput in = LookAt(*bot.client, release);
                in.buttons = sim::PlayerButton_Spell;
                in.spell = spell;
                return in;
            };
        }
    };

    glm::ivec3 GroundPointQ(const glm::vec3& feet, float dx, float dz) {
        return {static_cast<int32_t>(std::lround((feet.x + dx) * 128.0f)), kGround * 16, static_cast<int32_t>(std::lround((feet.z + dz) * 128.0f))};
    }

    sim::SpellGesture LineGesture(std::vector<glm::ivec3> points) {
        sim::SpellGesture g;
        g.kind = sim::GestureKind::Line;
        g.pointsQ = std::move(points);
        return g;
    }

    using VoxelSet = std::vector<glm::ivec3>;
    VoxelSet VoxelsOf(const voxel::World& world, uint8_t id, const glm::ivec3& min, const glm::ivec3& max) {
        VoxelSet out;
        for (int z = min.z; z < max.z; ++z)
            for (int y = min.y; y < max.y; ++y)
                for (int x = min.x; x < max.x; ++x)
                    if (world.GetVoxel({x, y, z}) == id) out.push_back({x, y, z});
        return out;
    }
}

// A wall drawn by a client: the server pairs the SpellGesture with the Spell edge by its input tick, builds the wall
// along the line, and its CastFx carries the flight and the points. The same cast run offline (sim::CastSpell and
// sim::RunScheduled on a flat world of its own) gives the same knit voxels.
TEST(Gesture, ADrawnWallIsTheSameOfflineAndOnTheServer) {
    SpellYard y;
    ASSERT_TRUE(y.started);
    GesturePlan plan;
    SpellYard::Bot& caster = y.AddBot(plan.Controls());
    StreamWatcher watcher;
    y.Watch(watcher);
    ASSERT_TRUE(y.Join());
    y.Passes(30);
    const uint8_t slot = caster.client->Slot();
    const glm::vec3 feet = y.Feet(slot);
    plan.spell = sim::kBarricadeWave;
    plan.gesture = LineGesture({GroundPointQ(feet, 3.0f, -2.0f), GroundPointQ(feet, 3.0f, 0.0f), GroundPointQ(feet, 4.0f, 1.5f)});
    plan.tick = caster.client->PredictTick() + 5;
    y.Passes(120);

    EXPECT_EQ(y.server.Stats().gestures, 1u);
    EXPECT_EQ(y.server.Stats().spellCasts, 1u);
    ASSERT_TRUE(caster.rejections.contains(plan.tick));
    EXPECT_EQ(caster.rejections[plan.tick], net::CastRejection::None);
    std::optional<net::CastFx> fx;
    for (const auto& [seq, event] : watcher.Events())
        if (const net::CastFx* f = std::get_if<net::CastFx>(&event)) fx = *f;
    ASSERT_TRUE(fx.has_value());
    EXPECT_EQ(fx->pointsQ, plan.gesture.pointsQ);
    EXPECT_GT(fx->flightTicks, 0u);
    EXPECT_EQ(fx->hitQ, plan.gesture.pointsQ.front());
    const sim::SpellTable table = sim::BuildSpellTable(0.125f);
    const int cost = static_cast<int>(sim::SpellManaCost(table, sim::kBarricadeWave, plan.gesture));
    const int since = static_cast<int>(caster.client->Magic().tick - fx->tick);
    EXPECT_EQ(caster.client->Magic().mana, std::min<int>(table.manaPool, 6000 - cost + 10 * since))
        << "the drawn wall costs 10 mana and 5 a metre, then 10 units a tick come back";

    const uint8_t knit = DefaultIds().knitIron;
    const glm::ivec3 centre = plan.gesture.pointsQ[1] >> 4;
    const glm::ivec3 lo = centre - glm::ivec3(40, 2, 40), hi = centre + glm::ivec3(40, 16, 40);
    const VoxelSet server = VoxelsOf(y.server.Simulation().World(), knit, lo, hi);
    EXPECT_GT(server.size(), 500u);

    sim::SimulationConfig config;
    config.chunkCapacity = 1024;
    config.maxParticles = 0;
    config.jobThreads = 2;
    config.world.voxelSize = 0.125f;
    config.world.windowSize = {8, 6, 8};
    config.physics.threads = 1;
    sim::Simulation offline(config);
    const voxel::WorldMaterials ids = DefaultIds();
    ASSERT_TRUE(offline.Initialize(voxel::CreateFlatGenerator(kGround, ids.grass, ids.stone)));
    offline.World().LoadBlocking(glm::dvec3(feet));
    sim::CastContext ctx;
    ctx.walls = sim::WindowWalls(offline.World());
    sim::SpellCast c;
    c.player = 1;
    c.team = 1;
    c.spell = sim::kBarricadeWave;
    c.eye = glm::dvec3(feet) + glm::dvec3(0.0, 1.62, 0.0);
    c.gesture = plan.gesture;
    const uint32_t t0 = offline.Tick();
    const sim::CastSpellResult r = sim::CastSpell(offline, table, c, ctx, {});
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    while (offline.Tick() < t0 + 90) {
        offline.StepPhysics();
        sim::RunScheduled(offline, table, ctx, offline.Tick(), {});
    }
    EXPECT_EQ(VoxelsOf(offline.World(), knit, lo, hi), server) << "the same wall offline and on the server";
    EXPECT_TRUE(y.HashesEqual());
}

// A drawn ice strip and a Punch along a stroke, cast by gesture in a session: every client ends with the server's
// voxels and zones, with no correction of the caster.
TEST(Magic, AGestureCastReplicatesWithEqualHashes) {
    SpellYard y;
    ASSERT_TRUE(y.started);
    GesturePlan plan;
    SpellYard::Bot& caster = y.AddBot(plan.Controls());
    SpellYard::Bot& other = y.AddBot();
    ASSERT_TRUE(y.Join());
    y.Passes(30);
    const uint8_t slot = caster.client->Slot();
    const glm::vec3 feet = y.Feet(slot);
    plan.spell = sim::kFlashFreeze;
    plan.gesture = LineGesture({GroundPointQ(feet, 2.0f, -3.0f), GroundPointQ(feet, 3.0f, -1.5f), GroundPointQ(feet, 4.0f, 0.0f),
                                GroundPointQ(feet, 4.0f, 2.0f)});
    // Frost is not of the default pair: the sandbox caster takes Metal and Frost before its first spell.
    caster.client->SendAttune(static_cast<uint8_t>(sim::Element::Metal), static_cast<uint8_t>(sim::Element::Frost));
    plan.tick = caster.client->PredictTick() + 5;
    y.Passes(120);
    const net::CastRejection first = caster.rejections.contains(plan.tick) ? caster.rejections[plan.tick] : net::CastRejection::None;

    plan.spell = sim::kPunch;
    plan.gesture = LineGesture({GroundPointQ(feet, 1.5f, -1.0f), GroundPointQ(feet, 1.5f, 1.0f)});
    plan.tick = caster.client->PredictTick() + 5;
    y.Passes(120);

    EXPECT_EQ(y.server.Stats().gestures, 2u);
    EXPECT_GE(y.server.Stats().spellCasts, 1u);
    EXPECT_EQ(caster.rejections[plan.tick], net::CastRejection::None) << "the Punch";
    if (first == net::CastRejection::None) {
        const glm::ivec3 centre = plan.gesture.pointsQ[0] >> 4;
        EXPECT_GT(VoxelsOf(y.server.Simulation().World(), DefaultIds().ice, centre - glm::ivec3(48, 4, 48), centre + glm::ivec3(48, 4, 48)).size(), 0u);
    }
    EXPECT_EQ(other.client->Stats().castFx, caster.client->Stats().castFx);
    EXPECT_TRUE(y.HashesEqual());
}
