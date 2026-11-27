// docs/design/networking.md stage 6: replicated objects. Server bodies with a NetId (terrain pieces of
// the replicated size, crates, object pieces), their events (EntitySpawn, EntityDespawn, EntityRest,
// ObjectModel, a Detach with a NetId), velocity snapshots with a priority, kinematic copies on every
// client, the piece caps and the body budget - over the seeded ImpairedTransport, one manual clock,
// player bots that predict. The acceptance: the scripted fight with islands and crates; a collapse
// with 64 awake pieces at 5 % loss (the awake cap reached with nothing asleep, the live cap's
// eviction); a split below the threshold; a player under falling pieces at 150 ms; players blocked by
// resting crates on both sides. The rest rule (a late snapshot after EntityRest) is in ClientTests and
// ClockTests; the simulation half in tests/sim/ReplicationTests.cpp.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <tuple>
#include <string>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <utility>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    constexpr int kGround = 96;              // flat ground: voxels y < 96 (12 m at 0.125 m)
    constexpr float kVoxel = 0.125f;
    constexpr uint16_t kStrong = 1600;       // 100 hardness: everything but bedrock

    net::Token ArenaToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x6a + i * 5);
        return t;
    }

    voxel::WorldMaterials ArenaIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    /// 7 x 7 chunks of play (28 m at 0.125 m) on flat ground at 12 m.
    sim::RegionDesc ArenaRegion() {
        sim::RegionDesc r;
        r.templateId = sim::RegionTemplateId::Empty;
        r.seed = 606;
        r.playMinChunk = {-4, -4};
        r.playMaxChunk = {3, 3};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = kGround;
        r.terrain.voxelSize = kVoxel;
        r.terrain.materials = ArenaIds();
        return r;
    }

    net::ServerSettings ArenaSettings() {
        net::ServerSettings s;
        s.session.token = ArenaToken();
        s.session.region = ArenaRegion();
        s.session.packHidden = false;
        s.simulation.jobThreads = 2;
        s.simulation.physics.threads = 1;
        s.simulation.physics.maxBodies = 2048;
        s.simulation.physics.maxBodyPairs = 4096;
        s.simulation.physics.maxContactConstraints = 4096;
        return s;
    }

    net::Impairment LossyLink(double delay, double jitter) {
        net::Impairment im;
        im.loss = 0.05;
        im.delaySeconds = delay;
        im.jitterSeconds = jitter;
        im.duplicate = 0.01;
        im.reorder = 0.02;
        return im;
    }

    net::Impairment DelayLink(double delay) {
        net::Impairment im;
        im.delaySeconds = delay;
        return im;
    }

    /// A server and player bots on impaired loopbacks, one manual clock (as MovementTests' Match):
    /// a pass is one tick of server time; each client runs the ticks its own clock gives it, each a
    /// whole client tick (RunTick) - which moves its copies of the server's bodies too.
    struct Arena {
        struct Bot {
            net::LoopbackTransport loop;
            std::unique_ptr<net::ImpairedTransport> link;
            std::unique_ptr<net::Client> client;
            std::unique_ptr<sim::Simulation> simulation;
            std::function<sim::PlayerInput(Bot&)> controls;
            bool presentation = false;
            net::PeerId peer = 0;             ///< this bot on the server's side
            bool gone = false;                ///< disconnected (Leave): no longer run or waited for
        };

        explicit Arena(net::ServerSettings settings = ArenaSettings(), net::Impairment impairment = {})
            : impairment(std::move(impairment)), serverLink(serverLoop, this->impairment, 17, [this] { return clock; }),
              server(std::move(settings), serverLink) {
            started = server.Start();
            dt = started ? static_cast<double>(server.Simulation().FixedTimeStep()) : 1.0 / 60.0;
        }

        Bot& AddBot(std::function<sim::PlayerInput(Bot&)> controls, bool presentation = false) {
            auto bot = std::make_unique<Bot>();
            bot->link = std::make_unique<net::ImpairedTransport>(bot->loop, impairment, 900 + bots.size(), [this] { return clock; });
            net::ClientSettings cs;
            cs.token = ArenaToken();
            cs.presentation = presentation;
            bot->presentation = presentation;
            bot->client = std::make_unique<net::Client>(cs, *bot->link);
            bot->controls = std::move(controls);
            bot->peer = net::LoopbackTransport::Connect(serverLoop, bot->loop);
            bots.push_back(std::move(bot));
            return *bots.back();
        }

        void ClientPass(Bot& bot) {
            if (bot.gone) return;
            if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation) {
                sim::SimulationConfig base;
                base.jobThreads = 2;
                base.chunkCapacity = 4096;
                base.maxParticles = bot.presentation ? 4096 : 0;
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
                const sim::PlayerInput in = bot.client->Predicting() && bot.controls ? bot.controls(bot) : sim::PlayerInput{};
                bot.client->RunTick(in);
                if (bot.simulation) {
                    for (const sim::BlastResult& blast : bot.client->TakePresentations()) sim::PresentBlast(*bot.simulation, blast);
                    if (bot.simulation->Terrain().Stats().pending != 0) bot.simulation->Jobs().WaitIdle();
                }
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

        bool Join(int maxPasses = 900) {
            for (int i = 0; i < maxPasses; ++i) {
                bool all = true;
                for (const std::unique_ptr<Bot>& bot : bots) all = all && (bot->gone || bot->client->Predicting());
                if (all) return true;
                Pass();
            }
            return false;
        }

        /// Stops the controls, then passes until the cast queue is empty, no replicated body moves,
        /// every client has applied the server's last event, and that held for `quiet` passes.
        bool Settle(int maxPasses = 3600, int quiet = 60) {
            for (const std::unique_ptr<Bot>& bot : bots) bot->controls = nullptr;
            int held = 0;
            for (int i = 0; i < maxPasses; ++i) {
                bool done = server.QueuedCasts() == 0 && server.AwakeBodies() == 0;
                for (const std::unique_ptr<Bot>& bot : bots)
                    done = done && (bot->gone || (bot->simulation && bot->client->AppliedOpSeq() == server.LastOpSeq() &&
                                                  bot->client->Synced()));
                held = done ? held + 1 : 0;
                if (held >= quiet) return true;
                Pass();
            }
            return false;
        }

        net::EntityComparison Compare(const Bot& bot) const { return net::CompareEntities(server.Entities(), bot.client->Entities()); }

        /// The server drops the bot's connection (as when its client quits): the server sees it leave.
        void Leave(Bot& bot) {
            serverLink.Disconnect(bot.peer);
            bot.gone = true;
        }

        float Disagreement(const Bot& bot) const {
            const std::optional<sim::PlayerState> state = server.GetPlayer(bot.client->Slot());
            return state ? glm::length(state->position - bot.client->Player().position) : 1.0e9f;
        }

        static void Print(const char* name, const net::Client& c) {
            const net::PredictionStats& p = c.Prediction();
            const net::ClientStats& s = c.Stats();
            std::printf("%s: %llu compared, %llu corrected (%llu snaps), max error %.4f m, timeouts %llu, resyncs %u; copies %u made / "
                        "%u failed / %u lost, %u despawns (%u kept as rubble), %u rests, %u models, %u fragments; %llu samples (%llu "
                        "ignored), largest sample gap %u ticks\n",
                        name, static_cast<unsigned long long>(p.compared), static_cast<unsigned long long>(p.corrections),
                        static_cast<unsigned long long>(p.snaps), p.maxError, static_cast<unsigned long long>(p.eventTimeouts), p.hardResyncs,
                        s.copiesMade, s.copiesFailed, s.copiesLost, s.despawns, s.toRubble, s.rests, s.models, s.fragments,
                        static_cast<unsigned long long>(s.bodySamples), static_cast<unsigned long long>(s.bodySamplesIgnored),
                        c.Bodies().MaxGap());
        }

        void PrintServer() const {
            const net::ServerStats& s = server.Stats();
            const sim::ReplicationStats& r = const_cast<net::Server&>(server).Simulation().Replication().stats;
            std::printf("Server: %u Detach bodies, %u object spawns, %u fragments, %u models, %u rests, despawns %u destroyed / %u to "
                        "rubble / %u broken; pieces %u replicated, %u over the tick budget, %u over the awake cap, evicted %u resting / %u "
                        "awake; at most %u awake (%u live / %u awake asked); crates %u; snapshots up to %zu B with %u bodies, %llu sent, "
                        "%llu left out; entity events %llu B of %llu\n",
                        s.detachBodies, s.objectSpawns, s.fragmentSpawns, s.objectModels, s.rests, s.despawns[1], s.despawns[2], s.despawns[3],
                        r.pieces, r.overTickBudget, r.overAwakeCap, r.evictedResting, r.evictedAwake, s.maxAwakeBodies, r.maxLive,
                        r.maxAwake, s.crates, s.maxSnapshotBytes, s.maxSnapshotBodies, static_cast<unsigned long long>(s.bodySamples),
                        static_cast<unsigned long long>(s.bodiesLeftOut), static_cast<unsigned long long>(s.entityEventBytes),
                        static_cast<unsigned long long>(s.eventBytes));
        }

        double clock = 0.0;
        double dt = 1.0 / 60.0;
        net::Impairment impairment;
        net::LoopbackTransport serverLoop;
        net::ImpairedTransport serverLink;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Bot>> bots;
    };

    /// A mushroom on the flat ground: a 3 x `stem` x 3 stone stem with its minimum corner at
    /// (x, ground, z) and a `cap` x 4 x `cap` planks cap on top (9: 324 voxels, over the replicated
    /// 262 at 0.125 m).
    void Mushroom(net::Server& server, const glm::ivec2& xz, int stem = 16, int cap = 9) {
        const voxel::WorldMaterials ids = ArenaIds();
        const int g = kGround;
        server.ApplyOp(sim::WorldOp::Box({xz.x, g, xz.y}, {xz.x + 3, g + stem, xz.y + 3}, ids.stone));
        const glm::ivec2 lo = xz + glm::ivec2(1 - cap / 2);
        server.ApplyOp(sim::WorldOp::Box({lo.x, g + stem, lo.y}, {lo.x + cap, g + stem + 4, lo.y + cap}, ids.planks));
    }

    /// The cast that cuts the stem of the mushroom at `xz` just below its cap: the cap and the
    /// stem's top fall as one piece.
    voxel::SphereQ StemCut(const glm::ivec2& xz, int stem = 16) {
        return voxel::SphereQ{glm::ivec3((xz.x + 1) * 16 + 8, (kGround + stem - 5) * 16 + 8, (xz.y + 1) * 16 + 8), 3 * 16};
    }

    std::function<sim::PlayerInput(Arena::Bot&)> PilotControls(uint64_t seed, net::PilotSettings settings) {
        auto pilot = std::make_shared<net::BotPilot>(ArenaRegion(), seed, settings);
        return [pilot](Arena::Bot& bot) { return pilot->Next(bot.client->Player()); };
    }

    glm::ivec3 SpawnVoxel(const net::Server& server) {
        return glm::ivec3(glm::floor(server.Template().spawn / static_cast<double>(kVoxel)));
    }
}

// The M1 scripted fight (networking.md "Testing"): two player bots walk, run, jump and drop crates
// (the E key) every 2 s; the server runs a drill blast every 6 ticks (10 casts/s) - each rebuilds a
// structure whose blast frees a piece (a mushroom cap, a bar, a neck tower: server bodies with a
// NetId) and catches a crate (ObjectModel / broken into client rubble) - for 600 ticks, over 5 %
// loss, 80 + 0-30 ms, duplicates, reordering and one 200 ms outage. After the drain: equal world
// hashes, the same NetIds and model hashes on every side, every resting copy at the server's pose,
// and each player's prediction within 5 cm of the server's.
TEST(Entities, FightWithIslandsAndCrates) {
    net::Impairment im = LossyLink(0.08, 0.03);
    im.outages = {{5.0, 5.2}};
    Arena a(ArenaSettings(), im);
    ASSERT_TRUE(a.started);
    net::PilotSettings pilot;
    pilot.crateEveryTicks = 120;
    Arena::Bot& one = a.AddBot(PilotControls(1, pilot), true);
    Arena::Bot& two = a.AddBot(PilotControls(2, pilot));
    ASSERT_TRUE(a.Join());

    const net::BlastDrill drill(a.server.Region(), ArenaIds(), kGround);
    uint32_t step = 0;
    for (int t = 0; t < 600; ++t) {
        if (t % 6 == 0) ASSERT_TRUE(net::RunDrillStep(a.server, drill.Step(step++)));
        a.Pass();
        ASSERT_FALSE(a.server.Failed());
    }
    ASSERT_TRUE(a.Settle());
    // A few more passes standing: the last snapshots compare the players at rest.
    a.Passes(60);
    Arena::Print("one", *one.client);
    Arena::Print("two", *two.client);
    a.PrintServer();

    const net::ServerStats& st = a.server.Stats();
    const sim::ReplicationStats& rs = a.server.Simulation().Replication().stats;
    EXPECT_EQ(step, 100u);
    EXPECT_GE(st.detachBodies, 60u);              // the drill's pieces became server bodies...
    EXPECT_GT(st.crates, 100u);                   // ... its crates (one per step) and the players' (E)
    EXPECT_GT(st.despawns[3], 50u);               // crates the blasts broke apart
    EXPECT_GT(st.fragmentSpawns, 50u);            // ... into client rubble
    EXPECT_EQ(rs.bodyFailures, 0u);
    EXPECT_LE(st.maxSnapshotBytes, net::kSnapshotBudgetBytes);
    EXPECT_GT(a.serverLink.Stats().lost, 0u);
    const voxel::WorldHashResult server = a.server.Hash();
    ASSERT_TRUE(server.Complete());
    for (const Arena::Bot* bot : {&one, &two}) {
        const net::Client& c = *bot->client;
        EXPECT_EQ(c.State(), net::ClientState::Playing);
        EXPECT_EQ(c.AppliedOpSeq(), a.server.LastOpSeq());
        EXPECT_EQ(c.Hash().hash, server.hash);
        EXPECT_EQ(c.Stats().copiesFailed, 0u);
        EXPECT_EQ(c.Stats().skippedChunks, 0u);
        const net::EntityComparison e = a.Compare(*bot);
        std::printf("entities: %s\n", e.Describe().c_str());
        EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
        EXPECT_GT(e.server, 2u);
        EXPECT_EQ(e.movingCompared, 0u);
        EXPECT_LT(a.Disagreement(*bot), 0.05f);
        EXPECT_EQ(c.Prediction().snaps, 0u);
    }
    EXPECT_GT(one.client->Stats().fragments, 50u);
    EXPECT_GT(a.server.Stats().objectModels + a.server.Stats().despawns[3], 0u);
}

namespace {
    /// The collapse arena: a grid of mushrooms 2 m apart (none within 3 m of the spawn).
    std::vector<glm::ivec2> MushroomGrid(const net::Server& server, size_t count) {
        const glm::ivec3 spawn = SpawnVoxel(server);
        std::vector<glm::ivec2> out;
        for (int z = -104; z <= 24 && out.size() < count; z += 16)
            for (int x = -112; x <= 72 && out.size() < count; x += 16) {
                if (std::abs(x + 1 - spawn.x) < 24 && std::abs(z + 1 - spawn.z) < 24) continue;
                out.push_back({x, z});
            }
        return out;
    }
}

// A collapse (networking.md "Testing": "collapse with 64 awake pieces, 5 % loss; cap reached with
// nothing asleep"): 80 stems cut, two a tick, so that pieces fall faster than they come to rest.
// The awake cap holds at 64 (the rest of the wave is client rubble, netId 0); snapshots stay under
// 1200 B and every awake body still reaches each client every few snapshots (the priority); after
// the drain the ids and models agree and every copy rests exactly at the server's pose. A second wave
// over the live cap (72 here) evicts the oldest resting pieces to client rubble (EntityDespawn
// {ToRubble}), and the clients follow.
TEST(Entities, CollapseWith64AwakePiecesAt5PercentLoss) {
    net::ServerSettings settings = ArenaSettings();
    settings.simulation.replication.maxLivePieces = 72;
    Arena a(settings, LossyLink(0.04, 0.03));
    ASSERT_TRUE(a.started);
    Arena::Bot& one = a.AddBot(nullptr, true);
    Arena::Bot& two = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    const std::vector<glm::ivec2> grid = MushroomGrid(a.server, 96);
    ASSERT_EQ(grid.size(), 96u);
    for (const glm::ivec2& m : grid) Mushroom(a.server, m);
    ASSERT_TRUE(a.Settle());

    size_t next = 0;
    // Mid-flight (review of stage 6): every few passes the copies of the moving bodies must follow
    // the server's - drawn in the past by the interpolation delay (at most 200 ms) plus the link, so
    // within a bound, not exactly.
    float movingError = 0.0f;
    uint32_t movingCompared = 0;
    auto watch = [&] {
        for (const Arena::Bot* bot : {&one, &two}) {
            const net::EntityComparison e = a.Compare(*bot);
            EXPECT_EQ(e.modelMismatches, 0u) << e.Describe();      // ids still in flight may be missing
            movingError = std::max(movingError, e.maxMovingError);
            movingCompared += e.movingCompared;
        }
    };
    auto wave = [&](size_t casts) {
        for (size_t i = 0; i < casts; i += 2) {
            for (size_t k = 0; k < 2 && next < grid.size(); ++k) ASSERT_TRUE(a.server.QueueCast(StemCut(grid[next++]), kStrong));
            a.Pass();
            if (i % 8 == 0) watch();
        }
    };
    wave(80);
    for (int i = 0; i < 20; ++i) {
        a.Pass();
        if (i % 4 == 0) watch();
    }
    const sim::ReplicationStats first = a.server.Simulation().Replication().stats;
    const uint32_t gapAtLoad = std::max(one.client->Bodies().MaxGap(), two.client->Bodies().MaxGap());
    ASSERT_TRUE(a.Settle());
    for (const Arena::Bot* bot : {&one, &two}) {
        const net::EntityComparison e = a.Compare(*bot);
        EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
    }
    wave(16);
    ASSERT_TRUE(a.Settle());
    Arena::Print("one", *one.client);
    Arena::Print("two", *two.client);
    a.PrintServer();
    std::printf("first wave: %u replicated, %u over the awake cap; largest sample gap %u ticks; moving copies within %.2f m of the "
                "server's (%u compared)\n",
                first.pieces, first.overAwakeCap, gapAtLoad, movingError, movingCompared);

    const net::ServerStats& st = a.server.Stats();
    const sim::ReplicationStats& rs = a.server.Simulation().Replication().stats;
    EXPECT_EQ(st.maxAwakeBodies, 64u);             // the cap reached ...
    EXPECT_GT(first.overAwakeCap, 0u);             // ... with nothing asleep: the rest of the wave is rubble
    EXPECT_GE(first.pieces, 64u);
    EXPECT_GT(rs.evictedResting, 0u);              // the second wave over the live cap
    EXPECT_EQ(rs.evictedAwake, 0u);
    EXPECT_GT(st.despawns[2], 0u);                 // ToRubble
    EXPECT_LE(st.maxSnapshotBytes, net::kSnapshotBudgetBytes);
    EXPECT_GT(st.bodiesLeftOut, 0u);               // not every awake body fits a snapshot: the priority decides
    // 64 awake, about 36 per snapshot: at most 2 snapshots apart without loss (networking.md); with
    // 5 % loss and far bodies (the distance term), a few more.
    EXPECT_LE(gapAtLoad, 12u);
    // The motion reached the clients (review of stage 6): samples were sent and taken, every copy
    // followed its server body while it fell, and every body that is left was sampled.
    EXPECT_GT(movingCompared, 100u);
    EXPECT_LT(movingError, 2.5f);
    EXPECT_GT(st.bodySamples, 0u);
    const voxel::WorldHashResult server = a.server.Hash();
    for (const Arena::Bot* bot : {&one, &two}) {
        const net::Client& c = *bot->client;
        EXPECT_EQ(c.Hash().hash, server.hash);
        EXPECT_EQ(c.Stats().copiesFailed, 0u);
        EXPECT_EQ(c.Stats().copiesLost, 0u);
        EXPECT_GT(c.Stats().bodySamples, 0u);
        for (const net::EntityInfo& e : a.server.Entities()) EXPECT_GT(c.Bodies().Received(e.netId), 0u) << "body " << e.netId;
        const net::EntityComparison e = a.Compare(*bot);
        std::printf("entities: %s\n", e.Describe().c_str());
        EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
        EXPECT_EQ(e.server, 72u);
    }
    EXPECT_GT(one.client->Stats().toRubble, 0u);   // the presenting client keeps evicted pieces as its rubble
    EXPECT_EQ(two.client->Stats().toRubble, 0u);
}

// Stage 7a (networking.md "Testing": a late join during a collapse): a client that joins while 64
// replicated bodies are awake gets them from its entity baseline - awake ones with their state at
// dequeue, resting ones at their exact EntityRest pose - and from the events after its join point;
// after the settle it holds the same bodies as the server and the one that played from the start.
TEST(Entities, ALateJoinerDuringACollapseGetsTheSameBodies) {
    net::ServerSettings settings = ArenaSettings();
    settings.simulation.replication.maxLivePieces = 72;
    Arena a(settings, LossyLink(0.04, 0.03));
    ASSERT_TRUE(a.started);
    Arena::Bot& one = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    const std::vector<glm::ivec2> grid = MushroomGrid(a.server, 96);
    for (const glm::ivec2& m : grid) Mushroom(a.server, m);
    ASSERT_TRUE(a.Settle());
    size_t next = 0;
    for (int i = 0; i < 40; ++i) {
        for (size_t k = 0; k < 2 && next < 80; ++k) ASSERT_TRUE(a.server.QueueCast(StemCut(grid[next++]), kStrong));
        a.Pass();
    }
    const size_t awakeAtJoin = a.server.AwakeBodies();
    Arena::Bot& late = a.AddBot(nullptr);
    // The server's body poses after each stepped tick (what its snapshots label that tick): an awake
    // baseline copy's first sample is the pose of the tick it names (review of stage 7a: the baseline
    // named the tick after it, one tick of motion off).
    std::map<uint32_t, std::map<uint32_t, glm::vec3>> posesAt;
    std::set<uint32_t> seen;
    uint32_t firstSamples = 0;
    float firstSampleError = 0.0f;
    for (int i = 0; i < 30 || (late.client->Stats().baselineEntities == 0 && i < 600); ++i) {      // the collapse goes on while it loads and syncs
        a.Pass();
        std::map<uint32_t, glm::vec3>& now = posesAt[a.server.TickNumber() - 1];
        for (const net::EntityInfo& e : a.server.Entities()) now[e.netId] = e.position;
        for (const net::EntityInfo& e : late.client->Entities()) {
            if (!seen.insert(e.netId).second || e.resting) continue;
            uint32_t tick = 0;
            net::BodyPose pose;
            if (!late.client->Bodies().Oldest(e.netId, tick, pose)) continue;
            const auto at = posesAt.find(tick);
            if (at == posesAt.end() || !at->second.contains(e.netId)) continue;
            firstSampleError = std::max(firstSampleError, glm::length(pose.position - at->second[e.netId]));
            ++firstSamples;
        }
    }
    ASSERT_TRUE(a.Settle());
    std::printf("late joiner: %u awake baseline copies checked, first sample at most %.5f m from the server's pose of its tick\n",
                firstSamples, firstSampleError);
    EXPECT_GT(firstSamples, 0u);
    EXPECT_LT(firstSampleError, 1.0e-4f);
    std::printf("late joiner: %zu bodies awake at its join; %u baseline entities, %u ChunkData, %u held / %u dropped entity events, %u "
                "stale\n",
                awakeAtJoin, late.client->Stats().baselineEntities, late.client->Stats().chunkData, late.client->Stats().heldEntityEvents,
                late.client->Stats().droppedHeldEvents, late.client->Stats().staleEntityEvents);
    EXPECT_GE(awakeAtJoin, 32u);
    EXPECT_GT(late.client->Stats().baselineEntities, 0u);
    for (const Arena::Bot* bot : {&one, &late}) {
        const net::EntityComparison e = a.Compare(*bot);
        EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
        EXPECT_EQ(e.restStateMismatches, 0u);
        EXPECT_EQ(bot->client->Stats().copiesFailed, 0u);
        EXPECT_EQ(bot->client->Stats().copiesLost, 0u);
        EXPECT_EQ(bot->client->Hash().hash, a.server.Hash().hash);
    }
}

// A split (networking.md "Testing": "split below threshold"): a replicated dumbbell of two 8^3
// cubes (512 voxels each) is cut at its neck: the survivor is an ObjectModel and the other cube a
// new server body (EntitySpawn with a NetId); then one cube is blasted below the replicated size: it
// breaks (EntityDespawn{Broken}) into netId-0 fragments, client rubble. The clients' copies follow
// with equal model hashes.
TEST(Entities, ASplitBelowTheThresholdBreaksTheObjectIntoClientRubble) {
    Arena a(ArenaSettings(), LossyLink(0.04, 0.03));
    ASSERT_TRUE(a.started);
    Arena::Bot& one = a.AddBot(nullptr, true);
    Arena::Bot& two = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    const voxel::WorldMaterials ids = ArenaIds();
    voxel::VoxelVolume volume(glm::ivec3(18, 8, 8));
    volume.FillBox(glm::ivec3(0), glm::ivec3(8), ids.planks);
    volume.FillBox(glm::ivec3(10, 0, 0), glm::ivec3(18, 8, 8), ids.planks);
    volume.FillBox(glm::ivec3(8, 3, 3), glm::ivec3(10, 5, 5), ids.planks);
    auto model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, "dumbbell");
    const glm::dvec3 origin = a.server.Template().spawn + glm::dvec3(4.0, 0.05, 0.0);
    const uint32_t id = a.server.SpawnObject(model, origin);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(a.Settle());

    auto entity = [&](uint32_t netId) -> std::optional<net::EntityInfo> {
        for (const net::EntityInfo& e : a.server.Entities())
            if (e.netId == netId) return e;
        return std::nullopt;
    };
    const std::optional<net::EntityInfo> rested = entity(id);
    ASSERT_TRUE(rested.has_value());
    const glm::dvec3 neck = glm::dvec3(rested->position + rested->rotation * (glm::vec3(9.0f, 4.0f, 4.0f) * kVoxel));
    ASSERT_TRUE(a.server.QueueCast(sim::BlastDesc{neck, 0.2f, 6.0f}));
    ASSERT_TRUE(a.Settle());
    EXPECT_EQ(a.server.Stats().objectModels, 1u);
    const uint32_t spawnsBefore = a.server.Stats().objectSpawns;
    EXPECT_EQ(spawnsBefore, 2u);                    // the dumbbell, then the cube the cut freed
    for (const Arena::Bot* bot : {&one, &two}) {
        const net::EntityComparison e = a.Compare(*bot);
        EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
        EXPECT_EQ(e.server, 2u);
        EXPECT_EQ(bot->client->Stats().models, 1u);
    }

    // The freed cube (the newest NetId), blasted from its middle: below 262 voxels it breaks.
    const std::vector<net::EntityInfo> now = a.server.Entities();
    ASSERT_EQ(now.size(), 2u);
    const net::EntityInfo cube = now.back();
    EXPECT_GT(cube.netId, id);
    const glm::dvec3 middle = glm::dvec3(cube.position + cube.rotation * (glm::vec3(4.0f) * kVoxel));
    ASSERT_TRUE(a.server.QueueCast(sim::BlastDesc{middle, 0.55f, 6.0f}));
    ASSERT_TRUE(a.Settle());
    Arena::Print("one", *one.client);
    Arena::Print("two", *two.client);
    a.PrintServer();
    EXPECT_GE(a.server.Stats().despawns[3], 1u);     // Broken
    EXPECT_GE(a.server.Stats().fragmentSpawns, 1u);
    EXPECT_GE(one.client->Stats().fragments, 1u);
    EXPECT_GE(one.simulation->Scene().Rubble().bodies + one.simulation->Scene().Rubble().small, 1u);   // its own rubble
    const voxel::WorldHashResult server = a.server.Hash();
    for (const Arena::Bot* bot : {&one, &two}) {
        const net::EntityComparison e = a.Compare(*bot);
        EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
        EXPECT_EQ(bot->client->Hash().hash, server.hash);
    }
    EXPECT_EQ(a.server.Entities().size(), 1u);
}

// A player under falling replicated pieces at 150 ms (networking.md "Testing"): the bot walks up and
// down a row of mushrooms, under their caps, while the server cuts the stems one after another. A
// falling cap passes through the player (a moving body is in Layer::Rubble on both sides: no
// character meets it) and comes to rest on its stub, where the walking player runs into it - a
// Dynamic body on the server at once, on the client one EntityRest later (half a round trip plus the
// queue): what the client predicted through it there is corrected. Bound set here: no snap, at most
// two corrections per piece, and both sides agree once the player stands.
TEST(Entities, PlayerUnderFallingPiecesAt150ms) {
    Arena a(ArenaSettings(), DelayLink(0.075));
    ASSERT_TRUE(a.started);
    Arena::Bot& bot = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    a.Passes(30);
    const sim::PlayerState start = bot.client->Player();
    const glm::ivec3 feet = glm::ivec3(glm::floor(start.position / kVoxel));
    // Stems 0.56 m in -x of the player's line, 2 m apart along z; the caps (13 voxels, 1.6 m) over
    // the line, their underside 2 m up (the player is 1.8 m tall).
    constexpr int kPieces = 6;
    auto stem = [&](int i) { return glm::ivec2(feet.x - 6, feet.z - 1 + (i - kPieces / 2) * 16); };
    for (int i = 0; i < kPieces; ++i) Mushroom(a.server, stem(i), 16, 13);
    ASSERT_TRUE(a.Settle());
    // Up and down the row: 90 ticks towards -z, 90 back (yaw 0 faces -z).
    bot.controls = [](Arena::Bot& b) {
        sim::PlayerInput in;
        in.moveZ = (b.client->PredictTick() / 90) % 2 == 0 ? 127 : -127;
        return in;
    };
    const uint64_t corrections = bot.client->Prediction().corrections;
    for (int i = 0; i < kPieces; ++i) {
        ASSERT_TRUE(a.server.QueueCast(StemCut(stem(i), 16), kStrong));
        a.Passes(120);
    }
    ASSERT_TRUE(a.Settle());
    a.Passes(30);
    const net::PredictionStats& p = bot.client->Prediction();
    Arena::Print("bot", *bot.client);
    a.PrintServer();
    std::printf("%llu corrections over %d pieces\n", static_cast<unsigned long long>(p.corrections - corrections), kPieces);
    for (const net::EntityInfo& e : a.server.Entities()) {
        const glm::vec3 c = e.position + e.rotation * glm::vec3(6.5f * kVoxel, 3.0f * kVoxel, 6.5f * kVoxel);
        std::printf("  piece %u rests with its centre near (%.2f, %.2f, %.2f)\n", e.netId, c.x, c.y, c.z);
    }
    EXPECT_EQ(a.server.Stats().detachBodies, static_cast<uint32_t>(kPieces));
    EXPECT_EQ(p.snaps, 0u);
    EXPECT_LE(p.corrections - corrections, static_cast<uint64_t>(2 * kPieces));
    EXPECT_LT(a.Disagreement(bot), 0.02f);
    const net::EntityComparison e = a.Compare(bot);
    EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
    EXPECT_EQ(e.server, static_cast<size_t>(kPieces));
    EXPECT_EQ(bot.client->Hash().hash, a.server.Hash().hash);
}

// Players collide with replicated bodies at rest on both sides (sim::kSessionCharacterLayers: the
// server's crate is Dynamic, the client's copy is Dynamic once its EntityRest came), so walking into
// a crate is predicted, not corrected; a crate the player drops (the E key, PlayerButton_Crate) is the
// server's, and the client's copy of it agrees.
TEST(Entities, RestingCratesBlockPlayersOnBothSides) {
    Arena a(ArenaSettings(), DelayLink(0.04));
    ASSERT_TRUE(a.started);
    Arena::Bot& bot = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    a.Passes(30);
    const glm::vec3 feet = bot.client->Player().position;
    // A crate 2 m ahead in +x (its -x face; 0.5 m wide in z around the player's line).
    ASSERT_NE(a.server.SpawnCrate(glm::dvec3(feet) + glm::dvec3(2.0, 0.05, -0.5)), 0u);
    ASSERT_TRUE(a.Settle());
    const uint64_t before = bot.client->Prediction().corrections;
    bot.controls = [](Arena::Bot&) {
        sim::PlayerInput in;
        in.yaw = sim::QuantizeYaw(-1.5707963f);      // facing +x
        in.moveZ = 127;
        return in;
    };
    a.Passes(150);
    bot.controls = nullptr;
    a.Passes(60);
    const sim::PlayerState client = bot.client->Player();
    const std::optional<sim::PlayerState> server = a.server.GetPlayer(bot.client->Slot());
    ASSERT_TRUE(server.has_value());
    std::printf("walked from x %.2f to client %.3f / server %.3f; %llu corrections\n", feet.x, client.position.x, server->position.x,
                static_cast<unsigned long long>(bot.client->Prediction().corrections - before));
    EXPECT_LT(server->position.x, feet.x + 2.0f);     // blocked by the crate
    EXPECT_GT(server->position.x, feet.x + 1.2f);
    EXPECT_LT(glm::length(server->position - client.position), 0.02f);
    EXPECT_EQ(bot.client->Prediction().corrections - before, 0u);

    // The player's own crate (E): held for a while, one edge, one crate; the same on both sides.
    const uint32_t crates = a.server.Stats().crates;
    bot.controls = [](Arena::Bot&) {
        sim::PlayerInput in;
        in.yaw = sim::QuantizeYaw(1.5707963f);       // facing -x, away from the first crate
        in.buttons = sim::PlayerButton_Crate;
        return in;
    };
    a.Passes(10);
    ASSERT_TRUE(a.Settle());
    EXPECT_EQ(a.server.Stats().crates, crates + 1);
    const net::EntityComparison e = a.Compare(bot);
    EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
    EXPECT_EQ(e.server, 2u);
}

// Review of stage 6: the replicated piece size is the metre volume in voxels of the session's size,
// not of the engine's default (Server::Start took it before the region's world: 262 voxels in a 0.1 m
// session instead of 512, 262 instead of 152 at 0.15 m).
TEST(Entities, TheReplicatedSizeFollowsTheSessionVoxelSize) {
    for (const auto& [voxel, expected] : {std::pair{0.1f, 512u}, std::pair{0.125f, 262u}, std::pair{0.15f, 152u}}) {
        net::ServerSettings settings = ArenaSettings();
        settings.session.region.terrain.voxelSize = voxel;
        Arena a(settings);
        ASSERT_TRUE(a.started) << voxel;
        EXPECT_EQ(a.server.Simulation().Scene().Config().replicatedPieceVoxels, expected) << voxel << " m";
        EXPECT_EQ(expected, scene::VoxelsForVolume(sim::kServerReplicatedPieceVolume, static_cast<double>(voxel)));
    }
}

namespace {
    /// The crate's resting pose on the server (its model origin).
    std::optional<net::EntityInfo> ServerEntity(const net::Server& server, uint32_t netId) {
        for (const net::EntityInfo& e : server.Entities())
            if (e.netId == netId) return e;
        return std::nullopt;
    }
}

// Review of stage 6: a player standing on a resting crate that wakes (a blast nearby, a body that
// touches it) stays on it. A moving replicated body is in Layer::Rubble, which no character meets -
// except the body a player stands on: the server keeps it Dynamic while it carries a player, and a
// client keeps its copy Dynamic while it carries its own player. Waking is an event (EntityWake), so
// both sides switch at the same point of the stream. Before, the server's player dropped into the
// crate at once while the client's stood on its copy until a sample came: corrections, then the
// player inside the crate when it slept again.
TEST(Entities, APlayerOnACrateThatWakesStaysOnIt) {
    Arena a(ArenaSettings(), DelayLink(0.04));
    ASSERT_TRUE(a.started);
    Arena::Bot& bot = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    a.Passes(30);
    const glm::vec3 feet = bot.client->Player().position;
    const uint32_t crate = a.server.SpawnCrate(glm::dvec3(feet) + glm::dvec3(2.0, 0.05, -0.375));
    ASSERT_NE(crate, 0u);
    ASSERT_TRUE(a.Settle());
    const std::optional<net::EntityInfo> rested = ServerEntity(a.server, crate);
    ASSERT_TRUE(rested.has_value());
    const float top = rested->position.y + 6.0f * kVoxel;
    const glm::vec3 onTop = rested->position + glm::vec3(3.0f * kVoxel, 6.0f * kVoxel + 0.02f, 3.0f * kVoxel);
    ASSERT_TRUE(a.server.TeleportPlayer(bot.client->Slot(), onTop));
    a.Passes(90);
    ASSERT_TRUE(a.Settle());
    ASSERT_NEAR(a.server.GetPlayer(bot.client->Slot())->position.y, top, 0.05f);
    const uint64_t corrections = bot.client->Prediction().corrections;
    const uint64_t snaps = bot.client->Prediction().snaps;            // the teleport's
    const uint32_t wakes = a.server.Stats().wakes;

    // Woken where it rests (what a blast within two radii, or a touching body, does to it).
    const glm::vec3 centre = rested->position + glm::vec3(3.0f * kVoxel);
    a.server.Simulation().Physics().ActivateInBox(math::AABB(centre - glm::vec3(0.5f), centre + glm::vec3(0.5f)));
    float lowest = 1.0e9f;
    for (int i = 0; i < 120; ++i) {
        a.Pass();
        lowest = std::min(lowest, a.server.GetPlayer(bot.client->Slot())->position.y);
    }
    ASSERT_TRUE(a.Settle());
    a.Passes(30);
    std::printf("crate top %.3f, the player's lowest feet %.3f; %u wake(s), %llu corrections, %u ticks a copy carried the player\n", top,
                lowest, a.server.Stats().wakes - wakes, static_cast<unsigned long long>(bot.client->Prediction().corrections - corrections),
                bot.client->Stats().heldUnderPlayer);
    EXPECT_GE(a.server.Stats().wakes, wakes + 1);
    EXPECT_GE(bot.client->Stats().wakes, 1u);
    EXPECT_GT(lowest, top - 0.05f);                      // never fell into the crate
    EXPECT_EQ(bot.client->Prediction().corrections - corrections, 0u);
    EXPECT_EQ(bot.client->Prediction().snaps, snaps);
    EXPECT_LT(a.Disagreement(bot), 0.02f);
    const net::EntityComparison e = a.Compare(bot);
    EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
}

// Review of stage 6: a player's cast hits a replicated body in front of the terrain (a crate, a
// falling piece), as offline. Before, the server's ray met terrain only: aimed at a crate 10 m away
// it hit the ground about 3 m behind it, out of reach of the crate.
TEST(Entities, ACastAimedAtACrateBreaksIt) {
    Arena a(ArenaSettings(), DelayLink(0.04));
    ASSERT_TRUE(a.started);
    Arena::Bot& bot = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    a.Passes(30);
    const glm::vec3 feet = bot.client->Player().position;
    const uint32_t crate = a.server.SpawnCrate(glm::dvec3(feet) + glm::dvec3(10.0, 0.05, -0.375));
    ASSERT_NE(crate, 0u);
    ASSERT_TRUE(a.Settle());
    const std::optional<net::EntityInfo> rested = ServerEntity(a.server, crate);
    ASSERT_TRUE(rested.has_value());
    const glm::vec3 centre = rested->position + glm::vec3(3.0f * kVoxel);
    const glm::vec3 eye = a.server.GetPlayer(bot.client->Slot())->position + glm::vec3(0.0f, bot.client->Tuning().eyeHeight, 0.0f);
    const glm::vec3 to = centre - eye;
    const float pitch = std::atan2(to.y, glm::length(glm::vec2(to.x, to.z)));
    // The ground the ray would meet without the crate is out of the blast's reach of it.
    const float groundBehind = (eye.y - (feet.y)) / std::tan(-pitch) - glm::length(glm::vec2(to.x, to.z));
    ASSERT_GT(groundBehind, bot.client->Tuning().castRadius + 1.0f);
    int presses = 0;
    bot.controls = [&presses, pitch](Arena::Bot&) {
        sim::PlayerInput in;
        in.yaw = sim::QuantizeYaw(-1.5707963f);          // facing +x
        in.pitch = sim::QuantizePitch(pitch);
        in.buttons = presses++ == 10 ? sim::PlayerButton_Cast : 0;
        return in;
    };
    a.Passes(60);
    ASSERT_TRUE(a.Settle());
    a.PrintServer();
    EXPECT_EQ(a.server.Stats().castsAtBodies, 1u);
    EXPECT_FALSE(ServerEntity(a.server, crate).has_value());   // broken (152 voxels: under the replicated size)
    EXPECT_EQ(a.server.Stats().despawns[3], 1u);
    const net::EntityComparison e = a.Compare(bot);
    EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
    EXPECT_EQ(e.server, 0u);
}

// The drain rule needs the client's ticks: only they move a copy to its samples. A drain that only polls (the
// headless game client's did) held the copy of a falling crate still for the hold time and settled while the
// server's crate still fell; its awake list then named the crate metres from the server's.
TEST(Entities, ADrainSettlesOnlyOnStepsThatFollowTheServersBodies) {
    Arena a(ArenaSettings(), DelayLink(0.04));
    ASSERT_TRUE(a.started);
    Arena::Bot& bot = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    ASSERT_TRUE(a.Settle());
    const glm::vec3 feet = bot.client->Player().position;
    const uint32_t crate = a.server.SpawnCrate(glm::dvec3(feet) + glm::dvec3(4.0, 10.0, 0.0));   // about 1.4 s of fall
    ASSERT_NE(crate, 0u);
    // Polls only: the events and the samples arrive, no tick moves the copy.
    net::DrainWatch polled(1.0);
    int polledSettledAt = -1, passes = 0;
    for (; passes < 400 && a.server.AwakeBodies() > 0 && polledSettledAt < 0; ++passes) {
        a.clock += a.dt;
        a.server.Advance(a.dt);
        if (a.server.Simulation().Terrain().Stats().pending != 0) a.server.Simulation().Jobs().WaitIdle();
        bot.client->Poll();
        if (polled.Update(*bot.client, a.clock)) polledSettledAt = passes;
    }
    EXPECT_LT(polledSettledAt, 0) << "settled after " << polledSettledAt << " passes while the server's crate moved";
    // Ticks: the copy follows the crate; the drain settles once it rests, where the server's rests.
    net::DrainWatch stepped(1.0);
    int steppedSettledAt = -1;
    for (int i = 0; i < 600 && steppedSettledAt < 0; ++i) {
        a.Pass();
        if (stepped.Update(*bot.client, a.clock)) steppedSettledAt = i;
    }
    ASSERT_GE(steppedSettledAt, 0) << stepped.Describe(a.clock);
    const std::optional<net::EntityInfo> server = ServerEntity(a.server, crate);
    ASSERT_TRUE(server.has_value());
    for (const net::EntityInfo& e : bot.client->Entities())
        if (e.netId == crate) EXPECT_LT(glm::length(e.position - server->position), 0.05f);
    const net::EntityComparison e = a.Compare(bot);
    EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
}

// Review of stage 6: players' crates (the E key) are bounded - one per crateCooldownTicks, at most
// maxCratesPerPlayer alive (the oldest goes) - and go when their player leaves (a reconnect loop left
// 8 bodies per connection for the rest of the session). The clients' copies follow, a late joiner's
// too.
TEST(Entities, PlayersCratesAreLimitedAndLeaveWithThePlayer) {
    Arena a(ArenaSettings(), DelayLink(0.04));
    ASSERT_TRUE(a.started);
    Arena::Bot& dropper = a.AddBot(nullptr);
    Arena::Bot& watcher = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    a.Passes(30);
    // E toggled every other tick for 600 ticks, turning 0.4 rad per crate cooldown (the crates go round).
    dropper.controls = [](Arena::Bot& b) {
        const uint32_t t = b.client->PredictTick();
        sim::PlayerInput in;
        in.yaw = sim::QuantizeYaw(0.4f * static_cast<float>(t / 30));
        in.buttons = t % 2 == 0 ? sim::PlayerButton_Crate : 0;
        return in;
    };
    uint32_t mostAlive = 0;
    for (int i = 0; i < 600; ++i) {
        a.Pass();
        mostAlive = std::max(mostAlive, static_cast<uint32_t>(a.server.Entities().size()));
    }
    ASSERT_TRUE(a.Settle());
    const net::ServerStats& st = a.server.Stats();
    std::printf("%u crates made, %u refused, at most %u alive, %u alive now\n", st.crates, st.cratesRefused, mostAlive,
                static_cast<uint32_t>(a.server.Entities().size()));
    EXPECT_LE(st.crates, 21u);                         // one per 30 ticks
    EXPECT_GE(st.crates, 15u);
    EXPECT_GT(st.cratesRefused, 0u);                   // edges in the cooldown
    EXPECT_LE(mostAlive, a.server.Simulation().Config().replication.maxAwakePieces);
    EXPECT_LE(mostAlive, 8u);
    EXPECT_EQ(a.server.Entities().size(), 8u);
    EXPECT_EQ(st.despawns[1], st.crates - 8u);         // the oldest went
    for (const Arena::Bot* bot : {&dropper, &watcher}) {
        const net::EntityComparison e = a.Compare(*bot);
        EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
    }

    // The dropper leaves: its crates go; a late joiner and the watcher agree.
    a.Leave(dropper);
    a.Passes(10);
    EXPECT_EQ(a.server.PlayerCount(), 1u);
    EXPECT_TRUE(a.server.Entities().empty());
    Arena::Bot& late = a.AddBot(nullptr);
    ASSERT_TRUE(a.Join());
    ASSERT_TRUE(a.Settle());
    for (const Arena::Bot* bot : {&watcher, &late}) {
        EXPECT_EQ(bot->client->Hash().hash, a.server.Hash().hash);
        const net::EntityComparison e = a.Compare(*bot);
        EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
        EXPECT_EQ(e.client, 0u);
    }
}

namespace {
    /// A 12^3 checkerboard of planks and wood (1728 voxels): its model is about 3.5 KB on the wire, and
    /// a small blast at a corner leaves a survivor over the replicated size (an ObjectModel).
    std::shared_ptr<voxel::VoxelModel> CheckerCube() {
        const voxel::WorldMaterials ids = ArenaIds();
        voxel::VoxelVolume v(glm::ivec3(12));
        for (int z = 0; z < 12; ++z)
            for (int y = 0; y < 12; ++y)
                for (int x = 0; x < 12; ++x) v.Set(x, y, z, (x + y + z) % 2 == 0 ? ids.planks : ids.wood);
        return std::make_shared<voxel::VoxelModel>(std::move(v), voxel::kWorldPalette, "checker");
    }
}

// Review of stage 6: entity events have a byte budget of their own (ServerSettings::entityEvent*),
// apart from the cast queue's (casts never wait for it, shred.md R5). A damaged replicated object
// resends its whole model (an ObjectModel); blasting large objects again and again used to send that
// without a bound. While the budget is spent a damaged object is let go as client rubble
// (EntityDespawn{ToRubble}) and new pieces are client rubble; the clients still agree.
TEST(Entities, EntityEventsKeepTheirByteBudget) {
    net::ServerSettings settings = ArenaSettings();
    settings.entityEventBytesPerTick = 100;            // 6 kB/s
    settings.entityEventBurstBytes = 4096;
    Arena a(settings, DelayLink(0.04));
    ASSERT_TRUE(a.started);
    Arena::Bot& one = a.AddBot(nullptr, true);
    ASSERT_TRUE(a.Join());
    const std::shared_ptr<voxel::VoxelModel> cube = CheckerCube();
    std::vector<uint32_t> ids;
    for (int i = 0; i < 6; ++i) {
        const uint32_t id = a.server.SpawnObject(cube, a.server.Template().spawn + glm::dvec3(-6.0 + 2.5 * i, 0.05, 4.0));
        ASSERT_NE(id, 0u);
        ids.push_back(id);
    }
    ASSERT_TRUE(a.Settle());
    a.Passes(300);                                     // the spawns' 20 KB of models paid back
    const uint64_t before = a.server.Stats().entityEventBytes;
    const uint32_t startTick = a.server.TickNumber();
    // A small blast at a corner of each live object in turn, one every 5 ticks, for 600 ticks.
    for (int t = 0; t < 600; ++t) {
        if (t % 5 == 0) {
            const std::vector<net::EntityInfo> live = a.server.Entities();
            if (!live.empty()) {
                const net::EntityInfo& e = live[static_cast<size_t>(t / 5) % live.size()];
                const int corner = (t / 5) % 8;
                const glm::vec3 local(corner & 1 ? 12.0f : 0.0f, corner & 2 ? 12.0f : 0.0f, corner & 4 ? 12.0f : 0.0f);
                const glm::vec3 at = e.position + e.rotation * (local * kVoxel);
                ASSERT_TRUE(a.server.QueueCast(sim::BlastDesc{glm::dvec3(at), 0.3f, 6.0f}));
            }
        }
        a.Pass();
    }
    ASSERT_TRUE(a.Settle());
    a.PrintServer();
    const net::ServerStats& st = a.server.Stats();
    const uint64_t spent = st.entityEventBytes - before;
    const uint64_t ticks = a.server.TickNumber() - startTick;
    std::printf("entity events %llu B over %llu ticks (budget %llu + burst %u); %u models, %u let go as rubble\n",
                static_cast<unsigned long long>(spent), static_cast<unsigned long long>(ticks),
                static_cast<unsigned long long>(ticks * settings.entityEventBytesPerTick), settings.entityEventBurstBytes, st.objectModels,
                st.modelsToRubble);
    EXPECT_GT(st.objectModels, 0u);
    EXPECT_GT(st.modelsToRubble, 0u);
    // The budget and its burst, plus what one cast may overshoot (a model of this size and small events).
    EXPECT_LE(spent, ticks * settings.entityEventBytesPerTick + settings.entityEventBurstBytes + 8192u);
    EXPECT_EQ(one.client->Hash().hash, a.server.Hash().hash);
    const net::EntityComparison e = a.Compare(one);
    EXPECT_TRUE(e.Agree(0.001f)) << e.Describe();
}

// Review of stage 6: the session tests' bodies must not follow worker timing. Terrain colliders
// were installed in the order their cook jobs finished, which handed out Jolt body ids (and so
// Jolt's contact order) differently from run to run; now a round of results is installed in chunk
// order. The same script twice - drill blasts with pieces and crates, two walking bots over a lossy
// link - gives the same server bodies, bit for bit.
TEST(Entities, OneScriptGivesTheSameBodiesTwice) {
    auto run = [] {
        Arena a(ArenaSettings(), LossyLink(0.04, 0.03));
        EXPECT_TRUE(a.started);
        net::PilotSettings pilot;
        pilot.crateEveryTicks = 60;
        a.AddBot(PilotControls(7, pilot));
        a.AddBot(PilotControls(8, pilot));
        EXPECT_TRUE(a.Join());
        const net::BlastDrill drill(a.server.Region(), ArenaIds(), kGround);
        uint32_t step = 0;
        std::vector<std::string> trace;     // per pass: the server's opSeq, players and bodies (where two runs part)
        auto record = [&] {
            std::string line = std::to_string(a.server.LastOpSeq());
            for (uint8_t slot = 0; slot < 2; ++slot)
                if (const std::optional<sim::PlayerState> p = a.server.GetPlayer(slot))
                    line += std::format(" p{} {:a} {:a} {:a}", slot, p->position.x, p->position.y, p->position.z);
            for (const net::EntityInfo& e : a.server.Entities())
                line += std::format(" b{} {:a} {:a} {:a}", e.netId, e.position.x, e.position.y, e.position.z);
            trace.push_back(std::move(line));
        };
        for (int t = 0; t < 180; ++t) {
            if (t % 6 == 0) EXPECT_TRUE(net::RunDrillStep(a.server, drill.Step(step++)));
            a.Pass();
            record();
        }
        EXPECT_TRUE(a.Settle());
        std::printf("%zu bodies, %u rests, %u crates, world %016llx\n", a.server.Entities().size(), a.server.Stats().rests,
                    a.server.Stats().crates, static_cast<unsigned long long>(a.server.Hash().hash));
        return std::make_tuple(a.server.Entities(), a.server.Hash().hash, trace);
    };
    const auto [first, firstHash, firstTrace] = run();
    const auto [second, secondHash, secondTrace] = run();
    for (size_t i = 0; i < std::min(firstTrace.size(), secondTrace.size()); ++i)
        if (firstTrace[i] != secondTrace[i]) {
            ADD_FAILURE() << "the runs part at pass " << i << ":\n  " << firstTrace[i] << "\n  " << secondTrace[i];
            break;
        }
    EXPECT_EQ(firstHash, secondHash);
    ASSERT_EQ(first.size(), second.size());
    ASSERT_GT(first.size(), 2u);
    for (size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].netId, second[i].netId);
        EXPECT_EQ(first[i].modelHash, second[i].modelHash);
        EXPECT_EQ(first[i].position, second[i].position) << "body " << first[i].netId;
        EXPECT_EQ(first[i].rotation, second[i].rotation) << "body " << first[i].netId;
    }
}
