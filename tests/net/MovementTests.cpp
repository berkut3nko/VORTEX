// docs/design/networking.md stage 4: players. Inputs up, snapshots down, the client clock ahead of
// the server, prediction with sim::StepPlayer and reconciliation, remote players - all over the
// seeded ImpairedTransport in one process with one manual clock. The acceptance: no correction
// above 2 cm on flat ground; a blast at a chunk border beside a walking player; a +50 ms ping
// step; three lost input packets that carry a cast; a 150 ms server hitch; a forced
// misprediction; forged packets.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    constexpr int kGround = 96;              // flat ground: voxels y < 96 (12 m at 0.125 m)
    constexpr float kGroundMetres = kGround * 0.125f;

    net::Token TestToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x51 + i * 3);
        return t;
    }

    voxel::WorldMaterials DefaultIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    /// 7 x 7 chunks of play (28 m at 0.125 m) on flat ground at 12 m; the spawn is the middle.
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

    net::ServerSettings Settings() {
        net::ServerSettings s;
        s.session.token = TestToken();
        s.session.region = FlatRegion();
        s.session.packHidden = false;
        s.simulation.jobThreads = 2;
        s.simulation.physics.threads = 1;
        s.simulation.physics.maxBodies = 2048;
        s.simulation.physics.maxBodyPairs = 4096;
        s.simulation.physics.maxContactConstraints = 4096;
        return s;
    }

    net::Impairment Lossy() {
        net::Impairment im;
        im.loss = 0.05;
        im.delaySeconds = 0.04;
        im.jitterSeconds = 0.03;
        im.duplicate = 0.01;
        im.reorder = 0.02;
        return im;
    }

    net::Impairment Delay(double seconds) {
        net::Impairment im;
        im.delaySeconds = seconds;
        return im;
    }

    /// A server and player bots, every link impaired the same way, one manual clock. A pass is one
    /// tick of server time: the server runs its due ticks (Advance), each client the ticks its own
    /// clock gives it (TicksDue), each of them a whole client tick (RunTick) with the bot's controls.
    struct Match {
        struct Bot {
            net::LoopbackTransport loop;
            std::unique_ptr<net::ImpairedTransport> link;
            std::unique_ptr<net::Client> client;
            std::unique_ptr<sim::Simulation> simulation;
            std::function<sim::PlayerInput(Bot&)> controls;
            std::vector<int32_t> margins;         // the clock's margin after every pass
            uint32_t ticks = 0;                   // client ticks run
            uint32_t casts = 0;                   // cast results of predicted ticks
            net::PeerId atServer = 0;             // the id the server's loopback has for this bot
        };

        explicit Match(net::ServerSettings settings = Settings(), net::Impairment impairment = {}, uint64_t serverSeed = 7)
            : impairment(std::move(impairment)), serverLink(serverLoop, this->impairment, serverSeed, [this] { return clock; }),
              server(std::move(settings), serverLink) {
            started = server.Start();
            dt = started ? static_cast<double>(server.Simulation().FixedTimeStep()) : 1.0 / 60.0;
        }

        Bot& AddBot(std::function<sim::PlayerInput(Bot&)> controls, uint64_t seed = 0, net::ClientSettings cs = {}) {
            auto bot = std::make_unique<Bot>();
            bot->link = std::make_unique<net::ImpairedTransport>(bot->loop, impairment, 500 + seed + bots.size(), [this] { return clock; });
            cs.token = TestToken();
            bot->client = std::make_unique<net::Client>(cs, *bot->link);
            bot->controls = std::move(controls);
            bot->atServer = net::LoopbackTransport::Connect(serverLoop, bot->loop);
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
                ++bot.ticks;
                // Collider cooking gets the time a real tick would give it (unless the test wants
                // the workers' own timing: `waitIdle` false).
                if (waitIdle && bot.simulation && bot.simulation->Terrain().Stats().pending != 0) bot.simulation->Jobs().WaitIdle();
            }
            bot.margins.push_back(bot.client->Prediction().lastMargin);
        }

        /// One pass of server time; `serverRuns` false is a server hitch (it does not run at all).
        void Pass(bool serverRuns = true) {
            clock += dt;
            if (serverRuns) {
                server.Advance(dt + hitchDebt);
                hitchDebt = 0.0;
                if (waitIdle && server.Simulation().Terrain().Stats().pending != 0) server.Simulation().Jobs().WaitIdle();
            } else {
                hitchDebt += dt;
            }
            for (const std::unique_ptr<Bot>& bot : bots) ClientPass(*bot);
        }

        void Passes(int count) {
            for (int i = 0; i < count; ++i) Pass();
        }

        /// Passes until every bot predicts.
        bool Join(int maxPasses = 600) {
            for (int i = 0; i < maxPasses; ++i) {
                bool all = true;
                for (const std::unique_ptr<Bot>& bot : bots) all = all && bot->client->Predicting();
                if (all) return true;
                Pass();
            }
            return false;
        }

        static void Print(const char* name, const net::Client& c) {
            const net::PredictionStats& p = c.Prediction();
            std::printf("%s: %llu ticks, %llu compared: %llu within 2 cm, %llu corrected (%llu snaps), max error %.4f m; errors <1mm %llu, "
                        "<2cm %llu, <5cm %llu, <20cm %llu, <1.5m %llu, more %llu; replays %llu ticks (max %u, %.3f ms max); waited for "
                        "events %llu, timeouts %llu, too old %llu, behind %llu; clock: %u jumps (%u ticks), %u resyncs, %llu bleed / %llu catch-up ticks, "
                        "margin %d..%d (last %d), jitter %.2f, target %d; %llu input packets / %llu inputs\n",
                        name, static_cast<unsigned long long>(p.ticks), static_cast<unsigned long long>(p.compared),
                        static_cast<unsigned long long>(p.within), static_cast<unsigned long long>(p.corrections),
                        static_cast<unsigned long long>(p.snaps), p.maxError, static_cast<unsigned long long>(p.errorBuckets[0]),
                        static_cast<unsigned long long>(p.errorBuckets[1]), static_cast<unsigned long long>(p.errorBuckets[2]),
                        static_cast<unsigned long long>(p.errorBuckets[3]), static_cast<unsigned long long>(p.errorBuckets[4]),
                        static_cast<unsigned long long>(p.errorBuckets[5]), static_cast<unsigned long long>(p.replayTicks), p.maxReplayTicks,
                        p.maxReplaySeconds * 1000.0, static_cast<unsigned long long>(p.waitedForEvents),
                        static_cast<unsigned long long>(p.eventTimeouts), static_cast<unsigned long long>(p.tooOld),
                        static_cast<unsigned long long>(p.behind), p.clockJumps, p.clockJumpTicks, p.hardResyncs,
                        static_cast<unsigned long long>(p.bleedTicks), static_cast<unsigned long long>(p.catchUpTicks),
                        p.minMargin, p.maxMargin, p.lastMargin, static_cast<double>(c.Clock().Jitter()), c.Clock().Target(),
                        static_cast<unsigned long long>(p.inputPackets), static_cast<unsigned long long>(p.inputsSent));
        }

        void PrintServer() const {
            const net::ServerStats& s = server.Stats();
            std::printf("Server: %llu input packets (%llu malformed): %llu queued, %llu late, %llu duplicate, %llu too early; %llu ticks "
                        "repeated, %llu stood, %llu held; %u casts from inputs (%u late, %u missed); %llu snapshots, %.1f B each\n",
                        static_cast<unsigned long long>(s.inputPackets), static_cast<unsigned long long>(s.malformedInputs),
                        static_cast<unsigned long long>(s.inputsQueued), static_cast<unsigned long long>(s.inputsLate),
                        static_cast<unsigned long long>(s.inputsDuplicate), static_cast<unsigned long long>(s.inputsTooEarly),
                        static_cast<unsigned long long>(s.ticksRepeated), static_cast<unsigned long long>(s.ticksStood),
                        static_cast<unsigned long long>(s.heldTicks), s.castsFromInputs, s.castsFromLateInputs, s.castsMissed,
                        static_cast<unsigned long long>(s.snapshots),
                        s.snapshots ? static_cast<double>(s.snapshotBytes) / static_cast<double>(s.snapshots) : 0.0);
        }

        double clock = 0.0;
        double dt = 1.0 / 60.0;
        double hitchDebt = 0.0;
        bool waitIdle = true;
        net::Impairment impairment;
        net::LoopbackTransport serverLoop;
        net::ImpairedTransport serverLink;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Bot>> bots;
    };

    /// Walks the way the controls say, from the bot's predicted state.
    std::function<sim::PlayerInput(Match::Bot&)> Pilot(const sim::RegionDesc& region, uint64_t seed, net::PilotSettings settings = {}) {
        auto pilot = std::make_shared<net::BotPilot>(region, seed, settings);
        return [pilot](Match::Bot& bot) { return pilot->Next(bot.client->Player()); };
    }
}

// The acceptance of stage 4 on flat ground: two bots walk, run, strafe, turn and jump for 30 s
// over a link with 5 % loss, 40 + 0-30 ms delay, duplicates and reordering. Not one snapshot
// differs from the prediction by 2 cm or more; each sees the other where the server had it.
TEST(Movement, NoCorrectionAbove2cmOnFlatGround) {
    Match m(Settings(), Lossy());
    ASSERT_TRUE(m.started);
    Match::Bot& a = m.AddBot(Pilot(FlatRegion(), 1));
    Match::Bot& b = m.AddBot(Pilot(FlatRegion(), 2));
    ASSERT_TRUE(m.Join());
    float remoteWorst = 0.0f;
    for (int i = 0; i < 1800; ++i) {
        m.Pass();
        if (i > 120 && i % 30 == 0) {
            // A sees B where the server had B a little while ago (drawn about 4-6 ticks in the past).
            const std::vector<net::RemotePlayerView> others = a.client->RemotePlayers();
            ASSERT_EQ(others.size(), 1u);
            const std::optional<sim::PlayerState> server = m.server.GetPlayer(b.client->Slot());
            ASSERT_TRUE(server.has_value());
            remoteWorst = std::max(remoteWorst, glm::length(others[0].position - server->position));
        }
    }
    Match::Print("A", *a.client);
    Match::Print("B", *b.client);
    m.PrintServer();
    for (const Match::Bot* bot : {&a, &b}) {
        const net::PredictionStats& p = bot->client->Prediction();
        EXPECT_GT(p.compared, 800u);
        EXPECT_EQ(p.corrections, 0u);
        EXPECT_EQ(p.snaps, 0u);
        EXPECT_LT(p.maxError, 0.02f);
        EXPECT_EQ(p.hardResyncs, 0u);
        EXPECT_EQ(p.heldTicks, 0u);
        EXPECT_EQ(p.eventTimeouts, 0u);
        EXPECT_GT(p.inputPackets, 800u);
    }
    EXPECT_EQ(m.server.Stats().inputsTooEarly, 0u);
    EXPECT_EQ(m.server.Stats().heldTicks, 0u);
    // The network did interfere: state packets were lost both ways, duplicated and reordered.
    EXPECT_GT(m.serverLink.Stats().lost, 0u);
    EXPECT_GT(m.serverLink.Stats().duplicated, 0u);
    EXPECT_GT(m.serverLink.Stats().reordered, 0u);
    for (const Match::Bot* bot : {&a, &b}) {
        EXPECT_GT(bot->link->Stats().lost, 0u);
        EXPECT_GT(bot->link->Stats().reordered, 0u);
    }
    // Remote players are drawn in the past: the snapshot's way down (3-5 ticks here) plus the
    // interpolation delay (4 ticks, more after a lost snapshot), at up to 7 m/s.
    EXPECT_LT(remoteWorst, 1.5f);
    std::printf("Remote player drawn at most %.2f m from the server's present position\n", remoteWorst);
    // Both moved about: the prediction ends where the server has the player.
    for (const Match::Bot* bot : {&a, &b}) {
        const net::Client& c = *bot->client;
        EXPECT_GT(glm::length(c.Player().position - glm::vec3(m.server.Template().spawn)), 1.0f);
    }
}

namespace {
    /// Controls that depend only on the tick being predicted (and `script`).
    std::function<sim::PlayerInput(Match::Bot&)> Scripted(std::function<sim::PlayerInput(uint32_t tick)> script) {
        return [script = std::move(script)](Match::Bot& bot) { return script(bot.client->PredictTick()); };
    }

    /// The prediction is where the server has the player (after a quiet moment).
    float Disagreement(Match& m, const Match::Bot& bot) {
        const std::optional<sim::PlayerState> server = m.server.GetPlayer(bot.client->Slot());
        return server ? glm::length(server->position - bot.client->Player().position) : 1.0e9f;
    }

    /// Stops the bots' controls and lets everything settle: both sides end in the same place.
    void Settle(Match& m, int passes = 120) {
        for (const std::unique_ptr<Match::Bot>& bot : m.bots) bot->controls = [](Match::Bot&) { return sim::PlayerInput{}; };
        m.Passes(passes);
    }
}

// A blast at a chunk border beside a walking player (networking.md stage 4): the bot walks up and
// down along z, 1.9 m inside chunk column x = 0, and every 20 ticks the server blasts the border
// x = 0 a little ahead of it. Every blast edits the chunk the player stands on, so its collider is
// re-cooked on the server and on the client at their own times; the gate (HasCollider) does not
// wait for re-cooks, so neither side holds the player and nothing differs by 2 cm. A second phase
// blasts 0.6 m closer, so that the crater's rim reaches under the capsule: the client learns of each
// hole one round trip late, which is corrected, never snapped. The link has jitter but no loss, so
// that what is measured is the terrain (loss is NoCorrectionAbove2cmOnFlatGround's).
namespace {
    /// The border-blast run; `waitIdle` false leaves collider cooking to the workers' own timing
    /// (a client whose workers are busy streaming), so only the tick-defined collider updates
    /// (TerrainColliders::FlushBox before every step and replay) make both sides agree.
    void BorderBlast(bool waitIdle) {
        net::Impairment im = Delay(0.04);
        im.jitterSeconds = 0.03;
        Match m(Settings(), im);
        ASSERT_TRUE(m.started);
        m.waitIdle = waitIdle;
        // Strafe right for 30 ticks (to x = 1.9 m), then walk 120 ticks along -z and 120 back.
        uint32_t start = 0;
        Match::Bot& bot = m.AddBot(Scripted([&start](uint32_t tick) {
            sim::PlayerInput in;
            const uint32_t t = tick - start;
            if (t < 30) {
                in.moveX = 127;
            } else {
                in.moveZ = 127;
                in.yaw = ((t - 30) / 120) % 2 == 0 ? 0 : 32768;
            }
            return in;
        }));
        ASSERT_TRUE(m.Join());
        start = bot.client->PredictTick();
        m.Passes(60);
        auto blasts = [&m, &bot](int count, double x) {
            for (int i = 0; i < count; ++i) {
                const std::optional<sim::PlayerState> p = m.server.GetPlayer(bot.client->Slot());
                const double ahead = m.server.GetPlayer(bot.client->Slot())->velocity.z < 0.0f ? -1.5 : 1.5;
                EXPECT_TRUE(
                    m.server.QueueCast(sim::BlastDesc{glm::dvec3(x, kGroundMetres, static_cast<double>(p->position.z) + ahead), 1.2f, 6.0f}));
                m.Passes(20);
            }
        };
        const net::PredictionStats before = bot.client->Prediction();
        std::printf("Before the blasts: %llu compared, largest error %.4f m\n", static_cast<unsigned long long>(before.compared),
                    static_cast<double>(before.maxError));
        EXPECT_LT(before.maxError, 0.001f);      // walking along the border, nothing edited yet: bit-equal
        blasts(40, 0.0);
        const net::PredictionStats beside = bot.client->Prediction();
        Match::Print("Beside", *bot.client);
        EXPECT_NEAR(m.server.GetPlayer(bot.client->Slot())->position.x, 2.0f, 0.15f);   // 30 ticks of strafe from 0.06 m
        EXPECT_EQ(beside.corrections - before.corrections, 0u);
        EXPECT_EQ(beside.heldTicks, 0u);
        EXPECT_GT(beside.compared - before.compared, 300u);

        blasts(40, 0.6);
        const net::PredictionStats rim = bot.client->Prediction();
        Match::Print("Rim", *bot.client);
        m.PrintServer();
        const uint64_t rimCorrections = rim.corrections - beside.corrections;
        std::printf("Rim blasts (%s): %llu corrections in 40 blasts, largest error %.4f m\n", waitIdle ? "cooks waited for" : "worker timing",
                    static_cast<unsigned long long>(rimCorrections), static_cast<double>(rim.maxError));
        EXPECT_LE(rimCorrections, 4u);           // the bound set in this stage (measured: 0)
        EXPECT_LT(rim.maxError, 0.02f);          // a growing hop shows here before it becomes corrections
        EXPECT_EQ(rim.snaps, 0u);
        EXPECT_EQ(rim.heldTicks, 0u);
        EXPECT_EQ(m.server.Stats().heldTicks, 0u);
        EXPECT_EQ(m.server.Stats().castsRun, 80u);
        Settle(m);
        EXPECT_LT(Disagreement(m, bot), 0.02f);
        EXPECT_EQ(bot.client->Hash().hash, m.server.Hash().hash);
    }
}

TEST(Movement, BlastAtAChunkBorderBesideAWalkingPlayer) { BorderBlast(true); }

// The same with collider cooks finishing whenever the workers get to them (review of stage 4: the
// acceptance above waits for every cook after every tick, which made cooks synchronous on both
// sides and hid when a re-cooked collider is installed).
TEST(Movement, BlastAtAChunkBorderWithoutWaitingForCooks) { BorderBlast(false); }

// A +50 ms ping step (networking.md stage 4): inputs start to arrive three ticks later; the clock
// jumps forward within 0.5 s and never needs a hard resync. The step back down is a surplus that
// is bled off, again without a resync.
TEST(Movement, PingStepMarginRecoversWithinHalfASecond) {
    Match m(Settings(), Delay(0.04));
    ASSERT_TRUE(m.started);
    net::PilotSettings calm;
    calm.jumps = false;
    Match::Bot& bot = m.AddBot(Pilot(FlatRegion(), 7, calm));
    ASSERT_TRUE(m.Join());
    m.Passes(600);
    ASSERT_GE(bot.client->Prediction().lastMargin, 0);
    const size_t step = bot.margins.size();
    m.serverLink.Settings().delaySeconds += 0.05;
    bot.link->Settings().delaySeconds += 0.05;
    m.Passes(600);
    int lastNegative = -1;
    for (size_t i = step; i < bot.margins.size(); ++i)
        if (bot.margins[i] < 0) lastNegative = static_cast<int>(i - step);
    int lowestAfterStep = 1000;
    for (size_t i = step; i < bot.margins.size(); ++i) lowestAfterStep = std::min(lowestAfterStep, bot.margins[i]);
    std::printf("Ping step: lowest margin %d, the last late one %d passes after the step; %u clock jumps\n", lowestAfterStep, lastNegative,
                bot.client->Prediction().clockJumps);
    // Felt: the first pass whose margin is more than a tick short of the target (the step's own
    // transit: inputs sent after it arrive later, and the snapshot saying so comes back). Recovered:
    // from this pass on, no margin is more than one tick short of the target.
    const int target = bot.client->Clock().Target();
    int felt = -1, recovered = 0;
    for (size_t i = step; i < bot.margins.size(); ++i)
        if (bot.margins[i] < target - 1) {
            if (felt < 0) felt = static_cast<int>(i - step);
            recovered = static_cast<int>(i - step) + 1;
        }
    std::printf("Ping step: margin short %d passes after the step, back at the target (%d) - 1 after %d passes\n", felt, target,
                recovered);
    EXPECT_LT(lastNegative, 30);             // never late, or not for long
    EXPECT_LT(lowestAfterStep, target);      // the step was felt
    ASSERT_GE(felt, 0);
    EXPECT_LE(recovered - felt, 30);         // made up within 0.5 s of the first short report
    EXPECT_LE(recovered, 36);                // 0.6 s after the step, its transit included (measured 31)
    EXPECT_GE(bot.margins.back(), target - 1);
    EXPECT_EQ(bot.client->Prediction().hardResyncs, 0u);

    // Down again: 3 ticks of surplus, bled off.
    m.serverLink.Settings().delaySeconds -= 0.05;
    bot.link->Settings().delaySeconds -= 0.05;
    const size_t down = bot.margins.size();
    m.Passes(900);
    int lowest = 1000;
    for (size_t i = down; i < bot.margins.size(); ++i) lowest = std::min(lowest, bot.margins[i]);
    Match::Print("Ping step", *bot.client);
    EXPECT_GE(lowest, 0);
    EXPECT_EQ(bot.client->Prediction().hardResyncs, 0u);
    EXPECT_GT(bot.client->Prediction().bleedTicks, 0u);
    EXPECT_LE(bot.client->Prediction().lastMargin, bot.client->Clock().Target() + 1);
    EXPECT_EQ(bot.client->Prediction().snaps, 0u);
    EXPECT_LE(bot.client->Prediction().corrections, 2u);
}

// Three lost input packets that carry a cast (networking.md stage 4): every packet repeats the
// unacknowledged inputs, so the cast reaches the server in the fourth - late for its tick, so it
// only refreshes the last input, and its edge is cast then. Exactly one cast; the client that
// pressed sees one cast too and gets the hole.
TEST(Movement, LostInputPacketsWithACastStillCastOnce) {
    Match m(Settings(), Delay(0.03));
    ASSERT_TRUE(m.started);
    uint32_t pressAt = 0xFFFFFFFFu;
    // A clock that keeps inputs only just early enough (target 1 tick): the inputs held back by the
    // lost packets arrive after their tick.
    net::ClientSettings tight;
    tight.clock.baseMargin = 0;
    tight.clock.jitterFactor = 0.0f;
    Match::Bot& bot = m.AddBot(Scripted([&pressAt](uint32_t tick) {
        sim::PlayerInput in;
        in.pitch = sim::QuantizePitch(-0.5f);          // the ground about 3 m ahead
        if (tick >= pressAt && tick < pressAt + 2) in.buttons = sim::PlayerButton_Cast;
        return in;
    }), 0, tight);
    ASSERT_TRUE(m.Join());
    m.Passes(120);
    const uint64_t lostBefore = bot.link->Stats().lost;
    // The next three input packets are lost (one every 2 ticks), and the press is in them.
    pressAt = bot.client->PredictTick() + 1;
    bot.link->Settings().outages.push_back({m.clock + 0.5 * m.dt, m.clock + 6.5 * m.dt});   // the next six passes
    m.Passes(240);
    const uint64_t lost = bot.link->Stats().lost - lostBefore;
    m.PrintServer();
    EXPECT_EQ(lost, 3u);
    EXPECT_EQ(bot.casts, 1u);                           // the client showed one cast
    EXPECT_EQ(m.server.Stats().castsFromInputs, 1u);
    EXPECT_EQ(m.server.Stats().castsFromLateInputs, 1u);
    EXPECT_EQ(m.server.Stats().castsMissed, 0u);
    EXPECT_EQ(m.server.Stats().castsRun, 1u);
    ASSERT_EQ(m.server.Casts().size(), 1u);
    EXPECT_EQ(m.server.Casts()[0].player, bot.client->Slot() + 1);
    EXPECT_EQ(bot.client->AppliedOpSeq(), m.server.LastOpSeq());
    EXPECT_EQ(bot.client->Hash().hash, m.server.Hash().hash);
    EXPECT_GT(m.server.Stats().inputsLate, 0u);
}

// A 150 ms server hitch (networking.md stage 4): the server does not run for 9 ticks, then catches
// up 2 ticks per pass. Inputs wait in its queue (at most 16 ahead), margins rise above 8 for less
// than a second: no hard resync, no input dropped, and the predictions stay right.
TEST(Movement, ServerHitchNeedsNoHardResync) {
    net::Impairment im = Delay(0.04);
    im.jitterSeconds = 0.01;
    Match m(Settings(), im);
    ASSERT_TRUE(m.started);
    Match::Bot& a = m.AddBot(Pilot(FlatRegion(), 11));
    Match::Bot& b = m.AddBot(Pilot(FlatRegion(), 12));
    ASSERT_TRUE(m.Join());
    m.Passes(300);
    const uint32_t before = m.server.TickNumber();
    for (int i = 0; i < 9; ++i) m.Pass(false);
    EXPECT_EQ(m.server.TickNumber(), before);
    m.Passes(600);
    Match::Print("Hitch A", *a.client);
    Match::Print("Hitch B", *b.client);
    m.PrintServer();
    for (const Match::Bot* bot : {&a, &b}) {
        const net::PredictionStats& p = bot->client->Prediction();
        EXPECT_EQ(p.hardResyncs, 0u);
        EXPECT_GT(p.maxMargin, 8);              // the hitch was felt
        EXPECT_EQ(p.corrections, 0u);
        EXPECT_EQ(p.snaps, 0u);
    }
    EXPECT_EQ(m.server.Stats().inputsTooEarly, 0u);
    Settle(m);
    EXPECT_LT(Disagreement(m, a), 0.02f);
    EXPECT_LT(Disagreement(m, b), 0.02f);
}

// A forced misprediction (networking.md stage 4): a wall the server builds just behind where its
// player is reaches the client after the client's player has walked through it - corrected,
// replayed against the wall, blended; then the server teleports the player 5 m: snapped. Both
// times the prediction converges.
TEST(Movement, ForcedMispredictionsConverge) {
    Match m(Settings(), Delay(0.05));
    ASSERT_TRUE(m.started);
    Match::Bot& bot = m.AddBot(Scripted([](uint32_t) {
        sim::PlayerInput in;
        in.moveZ = 127;                                  // along -z
        return in;
    }));
    ASSERT_TRUE(m.Join());
    m.Passes(30);
    const sim::PlayerState here = *m.server.GetPlayer(bot.client->Slot());
    ASSERT_GT(bot.client->Player().position.z + 0.3f, here.position.z - 2.0f);   // the client is ahead of the server
    // A wall across the path, 0.5 m ahead of the server's player: the client is already past it.
    const glm::ivec3 v = m.server.Simulation().World().WorldToVoxel(glm::dvec3(here.position));
    ASSERT_GT(m.server.ApplyOp(sim::WorldOp::Box(glm::ivec3(v.x - 24, kGround, v.z - 8), glm::ivec3(v.x + 24, kGround + 24, v.z - 4),
                                                m.server.Simulation().WorldMaterialIds().stone)),
              0u);
    m.Passes(120);
    const net::PredictionStats wall = bot.client->Prediction();
    Match::Print("Wall", *bot.client);
    EXPECT_GE(wall.corrections, 1u);
    EXPECT_EQ(wall.snaps, 0u);
    EXPECT_LT(Disagreement(m, bot), 0.02f);              // against the wall on both sides
    EXPECT_NEAR(bot.client->Player().position.z, static_cast<float>(v.z - 4) * 0.125f + 0.3f, 0.05f);
    m.Passes(60);
    EXPECT_EQ(bot.client->Prediction().corrections, wall.corrections);   // converged: nothing more to correct

    ASSERT_TRUE(m.server.TeleportPlayer(bot.client->Slot(), here.position + glm::vec3(5.0f, 0.0f, 0.0f)));
    m.Passes(60);
    Match::Print("Teleport", *bot.client);
    EXPECT_EQ(bot.client->Prediction().snaps, 1u);
    EXPECT_NEAR(bot.client->Player().position.x, here.position.x + 5.0f, 0.05f);
    Settle(m);
    EXPECT_LT(Disagreement(m, bot), 0.02f);
    EXPECT_LT(glm::length(bot.client->PlayerViewPosition() - bot.client->Player().position), 1.0e-4f);   // nothing left to blend
}

// Forged packets (networking.md stage 4): random bytes and absurd inputs on the state channel of a
// joined client, random bytes to the client: nothing crashes, nothing is dropped from the session,
// and the prediction converges once the forgeries stop.
TEST(Movement, ForgedPacketsAreHarmless) {
    Match m(Settings(), Delay(0.03));
    ASSERT_TRUE(m.started);
    Match::Bot& bot = m.AddBot(Pilot(FlatRegion(), 21));
    ASSERT_TRUE(m.Join());
    m.Passes(60);
    Rng rng(4321);
    constexpr net::PeerId kServerAsSeenByBot = 1;        // the bot's loopback names its only peer 1
    for (int i = 0; i < 600; ++i) {
        std::vector<uint8_t> bytes(rng.NextBounded(120));
        for (uint8_t& b : bytes) b = static_cast<uint8_t>(rng.NextU32());
        if (!bytes.empty() && i % 3 == 0) bytes[0] = static_cast<uint8_t>(net::MessageType::Inputs);
        bot.loop.Send(kServerAsSeenByBot, net::Channel::State, bytes);
        if (i % 5 == 0) {
            net::InputBatch forged;
            forged.firstTick = i % 2 == 0 ? 0xFFFFFFFCu : m.server.TickNumber() + rng.NextBounded(40);
            forged.inputs.resize(1 + rng.NextBounded(net::kMaxBatchInputs));
            for (sim::PlayerInput& in : forged.inputs) {
                in.moveX = static_cast<int8_t>(rng.NextU32());
                in.moveZ = static_cast<int8_t>(rng.NextU32());
                in.yaw = static_cast<uint16_t>(rng.NextU32());
                in.buttons = static_cast<uint8_t>(rng.NextU32() & ~sim::PlayerButton_Cast);
            }
            bot.loop.Send(kServerAsSeenByBot, net::Channel::State, net::Encode(forged));
        }
        std::vector<uint8_t> down(rng.NextBounded(80));
        for (uint8_t& b : down) b = static_cast<uint8_t>(rng.NextU32());
        if (!down.empty() && i % 2 == 0) down[0] = static_cast<uint8_t>(net::MessageType::Snapshot);
        m.serverLoop.Send(bot.atServer, net::Channel::State, down);
        m.Pass();
    }
    m.PrintServer();
    EXPECT_FALSE(m.server.Failed());
    EXPECT_EQ(m.server.ClientCount(), 1u);
    EXPECT_EQ(bot.client->State(), net::ClientState::Playing);
    EXPECT_GT(m.server.Stats().malformedInputs, 100u);
    EXPECT_GT(bot.client->Stats().malformedSnapshots, 100u);
    // The forgeries end: the real inputs take over and the prediction agrees again.
    m.Passes(120);
    const uint64_t corrections = bot.client->Prediction().corrections;
    m.Passes(300);
    EXPECT_LE(bot.client->Prediction().corrections - corrections, 1u);
    Settle(m);
    EXPECT_LT(Disagreement(m, bot), 0.02f);
}

// Review of stage 4: a client whose lead is far too large - placed with a 1 s round trip that then
// falls to 40 ms (ENet reports 500 ms before it has samples) - sends inputs 16 or more ticks early,
// which the server drops (its queue holds 16). Its margin reports must still reach the clock (they are about the dropped inputs, not
// about the last one the server kept), so the clock holds after a second above 8 and the server
// takes the inputs again.
TEST(Movement, AClientFarTooEarlyResyncsAndItsInputsAreTakenAgain) {
    Match m(Settings(), Delay(0.5));
    ASSERT_TRUE(m.started);
    net::PilotSettings calm;
    calm.jumps = false;
    Match::Bot& bot = m.AddBot(Pilot(FlatRegion(), 31, calm));
    ASSERT_TRUE(m.Join());
    m.Passes(120);
    EXPECT_EQ(m.server.Stats().inputsTooEarly, 0u);
    m.serverLink.Settings().delaySeconds = 0.02;
    bot.link->Settings().delaySeconds = 0.02;
    m.Passes(300);
    Match::Print("Far ahead", *bot.client);
    m.PrintServer();
    EXPECT_GT(m.server.Stats().inputsTooEarly, 0u);     // the drop was felt
    EXPECT_GE(bot.client->Prediction().hardResyncs, 1u);
    const uint64_t tooEarly = m.server.Stats().inputsTooEarly;
    const uint64_t queued = m.server.Stats().inputsQueued;
    m.Passes(300);
    EXPECT_EQ(m.server.Stats().inputsTooEarly, tooEarly);
    EXPECT_GT(m.server.Stats().inputsQueued - queued, 250u);  // every tick's input is taken again
    EXPECT_GE(bot.client->Prediction().lastMargin, 0);
    EXPECT_LE(bot.client->Prediction().lastMargin, bot.client->Clock().Target() + 1);
    Settle(m);
    EXPECT_LT(Disagreement(m, bot), 0.02f);
}

// Review of stage 4: events that fall behind the snapshots (a throttled events channel) must not
// stop reconciliation silently. A snapshot whose events have not arrived within eventWaitTicks of
// the first one that waited is given up and counted; once the events are in, snapshots are
// compared again and the prediction agrees.
TEST(Movement, SnapshotsWhoseEventsLagAreGivenUpAndCounted) {
    net::ServerSettings settings = Settings();
    settings.maxEventBytesInFlight = 1;                  // one batch on its way at a time
    Match m(settings, Delay(0.04));
    ASSERT_TRUE(m.started);
    net::PilotSettings calm;
    calm.jumps = false;
    Match::Bot& bot = m.AddBot(Pilot(FlatRegion(), 41, calm));
    ASSERT_TRUE(m.Join());
    m.Passes(60);
    // One small operation a tick for a second, far from the player: a batch per tick, delivered
    // one at a time - the events fall further and further behind.
    const uint8_t stone = m.server.Simulation().WorldMaterialIds().stone;
    for (int i = 0; i < 60; ++i) {
        ASSERT_GT(m.server.ApplyOp(sim::WorldOp::Box(glm::ivec3(60, kGround, 60 + i), glm::ivec3(61, kGround + 1, 61 + i), stone)), 0u);
        m.Pass();
    }
    m.Passes(30);
    Match::Print("Events lag", *bot.client);
    EXPECT_GT(bot.client->Prediction().eventTimeouts, 0u);
    for (int i = 0; i < 900 && bot.client->AppliedOpSeq() < m.server.LastOpSeq(); ++i) m.Pass();
    ASSERT_EQ(bot.client->AppliedOpSeq(), m.server.LastOpSeq());
    const uint64_t compared = bot.client->Prediction().compared;
    m.Passes(120);
    EXPECT_GT(bot.client->Prediction().compared - compared, 40u);
    Settle(m);
    EXPECT_LT(Disagreement(m, bot), 0.02f);
    EXPECT_EQ(bot.client->Hash().hash, m.server.Hash().hash);
}

// Review of stage 4: the cast test's twin with a jump. The press reaches the server only in a late
// input; its edge must not be spent without a jump (the client predicted one): the server jumps
// on the next tick it simulates, once.
TEST(Movement, LostInputPacketsWithAJumpStillJumpOnce) {
    Match m(Settings(), Delay(0.03));
    ASSERT_TRUE(m.started);
    uint32_t pressAt = 0xFFFFFFFFu;
    net::ClientSettings tight;
    tight.clock.baseMargin = 0;
    tight.clock.jitterFactor = 0.0f;
    Match::Bot& bot = m.AddBot(Scripted([&pressAt](uint32_t tick) {
        sim::PlayerInput in;
        if (tick >= pressAt && tick < pressAt + 2) in.buttons = sim::PlayerButton_Jump;
        return in;
    }), 0, tight);
    ASSERT_TRUE(m.Join());
    m.Passes(120);
    const uint64_t lostBefore = bot.link->Stats().lost;
    const uint64_t lateBefore = m.server.Stats().inputsLate;
    pressAt = bot.client->PredictTick() + 1;
    bot.link->Settings().outages.push_back({m.clock + 0.5 * m.dt, m.clock + 6.5 * m.dt});
    float highest = 0.0f;
    for (int i = 0; i < 240; ++i) {
        m.Pass();
        highest = std::max(highest, m.server.GetPlayer(bot.client->Slot())->position.y - kGroundMetres);
    }
    m.PrintServer();
    EXPECT_EQ(bot.link->Stats().lost - lostBefore, 3u);
    EXPECT_GT(m.server.Stats().inputsLate, lateBefore);
    EXPECT_GT(highest, 0.5f);                            // the server's player jumped (apex about 1.08 m)
    EXPECT_LT(highest, 1.3f);                            // once
    Settle(m);
    EXPECT_LT(Disagreement(m, bot), 0.02f);
}

// Review of stage 4: another player walking straight at constant speed is drawn moving forward
// only. Snapshots arrive every 2 ticks with 0-30 ms of jitter; the time the client draws them at
// must never step back when a snapshot is late.
TEST(Movement, RemotePlayersAreDrawnMovingForwardOnly) {
    net::Impairment im = Delay(0.04);
    im.jitterSeconds = 0.03;
    Match m(Settings(), im);
    ASSERT_TRUE(m.started);
    uint32_t walkFrom = 0xFFFFFFFFu;
    Match::Bot& a = m.AddBot(Scripted([](uint32_t) { return sim::PlayerInput{}; }));
    Match::Bot& b = m.AddBot(Scripted([&walkFrom](uint32_t tick) {
        sim::PlayerInput in;
        if (tick >= walkFrom) in.moveZ = 127;            // along -z
        return in;
    }));
    ASSERT_TRUE(m.Join());
    m.Passes(60);
    walkFrom = b.client->PredictTick();
    m.Passes(30);                                        // walking, and seen walking
    float previous = 1.0e9f;
    int backwards = 0;
    float worstStep = 0.0f;
    for (int i = 0; i < 150; ++i) {
        m.Pass();
        const std::vector<net::RemotePlayerView> others = a.client->RemotePlayers();
        ASSERT_EQ(others.size(), 1u);
        const float z = others[0].position.z;
        if (z > previous + 1.0e-4f) {
            ++backwards;
            worstStep = std::max(worstStep, z - previous);
        }
        previous = z;
    }
    std::printf("Remote player drawn stepping back %d times (worst %.3f m)\n", backwards, static_cast<double>(worstStep));
    EXPECT_EQ(backwards, 0);
}

// Measurement (disabled; networking.md "Stage 4 as built"): the flat-ground acceptance over 10
// seeds of the network and the pilots. A correction on flat ground needs an input that arrives
// after its tick (two lost packets in a row, or loss plus jitter) at a tick where the controls
// changed: the repeated input then differs from the real one.
TEST(Movement, DISABLED_FlatGroundCorrectionRateOverSeeds) {
    uint64_t corrections = 0, compared = 0, late = 0, ticks = 0;
    for (uint64_t seed = 1; seed <= 10; ++seed) {
        Match m(Settings(), Lossy(), 7 + 100 * seed);
        ASSERT_TRUE(m.started);
        Match::Bot& a = m.AddBot(Pilot(FlatRegion(), 10 * seed + 1), 100 * seed);
        Match::Bot& b = m.AddBot(Pilot(FlatRegion(), 10 * seed + 2), 100 * seed);
        ASSERT_TRUE(m.Join());
        m.Passes(3600);
        for (const Match::Bot* bot : {&a, &b}) {
            corrections += bot->client->Prediction().corrections;
            compared += bot->client->Prediction().compared;
            ticks += bot->client->Prediction().ticks;
        }
        late += m.server.Stats().inputsLate;
        std::printf("seed %llu: corrections %llu / %llu, late inputs %llu\n", static_cast<unsigned long long>(seed),
                    static_cast<unsigned long long>(a.client->Prediction().corrections + b.client->Prediction().corrections),
                    static_cast<unsigned long long>(a.client->Prediction().compared + b.client->Prediction().compared),
                    static_cast<unsigned long long>(m.server.Stats().inputsLate));
    }
    std::printf("10 seeds x 2 bots x 60 s: %llu corrections in %llu comparisons (%.2f per player-minute), %llu late inputs\n",
                static_cast<unsigned long long>(corrections), static_cast<unsigned long long>(compared),
                static_cast<double>(corrections) / (static_cast<double>(ticks) / 3600.0), static_cast<unsigned long long>(late));
}
