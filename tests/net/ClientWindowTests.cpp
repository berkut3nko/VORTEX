// docs/design/networking.md "Stage 7a plan", milestone M-b: a session client whose window is smaller
// than the region follows its player and is kept in sync chunk by chunk (WindowUpdate on every move,
// the modified chunks of the entering slab as ChunkData, pristine ones based on the ack; chunks that
// leave are dropped and come back from the server). A flat region 40 chunks wide (160 m at 0.125 m)
// and a client window of 12 chunks (48 m): test-sized, the game's is 48.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
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
    constexpr int kGround = 96;                    // flat ground: voxels y < 96 (12 m at 0.125 m)
    constexpr float kGroundMetres = kGround * 0.125f;
    constexpr int32_t kWindow = 12;                // the client's window, chunks (48 m)

    net::Token TestToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x63 + i * 5);
        return t;
    }

    voxel::WorldMaterials DefaultIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    /// 40 x 40 chunks of play (160 m), 46 x 8 x 46 chunks of server window, flat ground at 12 m.
    sim::RegionDesc WideRegion() {
        sim::RegionDesc r;
        r.templateId = sim::RegionTemplateId::Empty;
        r.seed = 707;
        r.playMinChunk = {-20, -20};
        r.playMaxChunk = {20, 20};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = kGround;
        r.terrain.voxelSize = 0.125f;
        r.terrain.materials = DefaultIds();
        return r;
    }

    /// 7 x 7 chunks of play: fits any window of 13 chunks or more.
    sim::RegionDesc SmallRegion() {
        sim::RegionDesc r = WideRegion();
        r.playMinChunk = {-4, -4};
        r.playMaxChunk = {3, 3};
        return r;
    }

    net::ServerSettings Settings(const sim::RegionDesc& region = WideRegion()) {
        net::ServerSettings s;
        s.session.token = TestToken();
        s.session.region = region;
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

    /// A server and walking bots on one manual clock (the Match fixture of the movement tests, with
    /// the client window): one pass = one server tick, then each bot's due ticks (RunTick: streaming
    /// around its player, Poll, the predicted step).
    struct Field {
        struct Bot {
            net::LoopbackTransport loop;
            std::unique_ptr<net::ImpairedTransport> link;
            std::unique_ptr<net::Client> client;
            std::unique_ptr<sim::Simulation> simulation;
            std::function<sim::PlayerInput(Bot&)> controls;
            bool manual = false;          ///< the test drives it (Poll, its world) instead of RunTick
        };

        explicit Field(net::ServerSettings settings = Settings(), net::Impairment impairment = {})
            : impairment(std::move(impairment)), serverLink(serverLoop, this->impairment, 11, [this] { return clock; }),
              server(std::move(settings), serverLink) {
            started = server.Start();
        }

        Bot& AddBot(std::function<sim::PlayerInput(Bot&)> controls, net::ClientSettings cs = {}) {
            auto bot = std::make_unique<Bot>();
            bot->link = std::make_unique<net::ImpairedTransport>(bot->loop, impairment, 900 + bots.size(), [this] { return clock; });
            cs.token = TestToken();
            if (cs.windowChunks == 0) cs.windowChunks = kWindow;
            bot->client = std::make_unique<net::Client>(cs, *bot->link);
            bot->controls = std::move(controls);
            net::LoopbackTransport::Connect(serverLoop, bot->loop);
            bots.push_back(std::move(bot));
            return *bots.back();
        }

        void ClientPass(Bot& bot) {
            if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation) {
                sim::SimulationConfig base;
                base.jobThreads = 2;
                base.maxParticles = 0;
                base.physics.threads = 1;
                base.physics.maxBodies = 2048;
                base.physics.maxBodyPairs = 4096;
                base.physics.maxContactConstraints = 4096;
                bot.simulation = std::make_unique<sim::Simulation>(bot.client->MakeSimulationConfig(base, storeLimit));
                ASSERT_TRUE(bot.simulation->Initialize(bot.client->MakeGenerator()));
                ASSERT_TRUE(bot.client->Attach(*bot.simulation));
            }
            if (bot.manual) {
                bot.client->Poll();
                return;
            }
            const uint32_t n = bot.client->TicksDue(dt);
            for (uint32_t i = 0; i < n; ++i) {
                const sim::PlayerInput in = bot.client->Predicting() ? bot.controls(bot) : sim::PlayerInput{};
                bot.client->RunTick(in);
                // Reproducible runs (stage 6 rule): collider cooks and loads finish inside the tick.
                if (bot.simulation) bot.simulation->Jobs().WaitIdle();
            }
        }

        void Pass() {
            clock += dt;
            server.Advance(dt);
            server.Simulation().Jobs().WaitIdle();
            for (const std::unique_ptr<Bot>& bot : bots) ClientPass(*bot);
        }

        void Passes(int count) {
            for (int i = 0; i < count; ++i) Pass();
        }

        /// Passes until every bot predicts its player.
        bool Join(int maxPasses = 900) {
            for (int i = 0; i < maxPasses; ++i) {
                bool all = true;
                for (const std::unique_ptr<Bot>& bot : bots) all = all && (bot->manual || bot->client->Predicting());
                if (all) return true;
                Pass();
            }
            return false;
        }

        /// Passes until the cast queue is empty and every bot has applied the server's last event and
        /// is synced (its window current, no request out, its baseline in).
        bool Drain(int maxPasses = 1800) {
            for (int i = 0; i < maxPasses; ++i) {
                bool done = server.QueuedCasts() == 0;
                for (const std::unique_ptr<Bot>& bot : bots)
                    done = done && bot->simulation && bot->client->AppliedOpSeq() == server.LastOpSeq() && bot->client->Synced();
                if (done) return true;
                Pass();
            }
            return false;
        }

        /// The bot's hash over its sync box and the server's over the same box, at the same opSeq.
        void ExpectSameWorld(const Bot& bot) const {
            const net::SyncBox box = bot.client->SyncWindow();
            EXPECT_FALSE(box.Empty());
            EXPECT_EQ(bot.client->AppliedOpSeq(), server.LastOpSeq());
            const voxel::WorldHashResult c = bot.client->Hash(), s = server.Hash(box);
            EXPECT_TRUE(c.Complete()) << c.notLoaded << " chunks of the sync box not current";
            EXPECT_TRUE(s.Complete());
            EXPECT_EQ(c.hash, s.hash);
            EXPECT_EQ(c.solidChunks, s.solidChunks);
            const net::EntityComparison e = net::CompareEntities(server.Entities(), bot.client->Entities());
            EXPECT_TRUE(e.Agree(0.05f)) << e.Describe();
        }

        /// A crater on the flat ground at (x, z) metres.
        bool Crater(double x, double z, float radius = 1.5f) {
            return server.QueueCast(sim::BlastDesc{glm::dvec3(x, kGroundMetres, z), radius, 6.0f});
        }

        double clock = 0.0;
        double dt = 1.0 / 60.0;
        uint32_t storeLimit = 0;
        net::Impairment impairment;
        net::LoopbackTransport serverLoop;
        net::ImpairedTransport serverLink;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Bot>> bots;
    };

    /// Controls that run the player to `points` (metres, x / z) in order, then stand.
    std::function<sim::PlayerInput(Field::Bot&)> Walk(const sim::RegionDesc& region, std::vector<glm::vec2> points,
                                                       std::shared_ptr<net::BotPilot>* out = nullptr) {
        net::PilotSettings ps;
        ps.waypoints = std::move(points);
        ps.jumps = false;
        ps.stuckTicks = 600;
        auto pilot = std::make_shared<net::BotPilot>(region, 77, ps);
        if (out) *out = pilot;
        return [pilot](Field::Bot& bot) { return pilot->Next(bot.client->Player()); };
    }

    sim::PlayerInput Stand(Field::Bot&) { return {}; }

    glm::ivec3 ChunkAt(double x, double y, double z) {
        return voxel::VoxelToChunk(glm::ivec3(glm::floor(glm::dvec3(x, y, z) / 0.125)));
    }
}

// --- the window ------------------------------------------------------------------------------------

TEST(ClientWindow, ARegionThatFitsKeepsTheFixedWindow) {
    // The pure functions: a region that fits keeps its window; a wide one gets the client's, which
    // World places where ClientWindowMinChunk says.
    const sim::RegionDesc small = SmallRegion(), wide = WideRegion();
    EXPECT_FALSE(sim::ClientWindowMoves(small, 0));
    EXPECT_FALSE(sim::ClientWindowMoves(small, 48));
    EXPECT_FALSE(sim::ClientWindowMoves(wide, 0));
    EXPECT_FALSE(sim::ClientWindowMoves(wide, 46));
    EXPECT_TRUE(sim::ClientWindowMoves(wide, 45));
    EXPECT_EQ(sim::ClientWorldConfig(small, 48).windowSize, sim::RegionWorldConfig(small).windowSize);
    EXPECT_EQ(sim::ClientChunkCapacity(small, 48), sim::RegionChunkCapacity(small));
    EXPECT_EQ(sim::ClientWorldConfig(wide, kWindow).windowSize, glm::ivec3(kWindow, 1, kWindow));
    EXPECT_TRUE(sim::ClientWorldConfig(wide, kWindow).reportLoads);
    EXPECT_FALSE(sim::ClientWorldConfig(wide, kWindow).keepUnloadedEdits);
    {
        voxel::ChunkStore store(2048);
        JobSystem jobs(1);
        voxel::World world(sim::ClientWorldConfig(wide, kWindow), store, jobs, sim::MakeRegionGenerator(wide, DefaultIds()));
        for (const glm::dvec3 focus : {glm::dvec3(0.3, 13.0, -0.2), glm::dvec3(-61.9, 3.0, 47.0), glm::dvec3(4.0, 1.0, -4.0)}) {
            world.Update(focus);
            EXPECT_EQ(world.WindowMinChunk(), sim::ClientWindowMinChunk(wide, kWindow, focus));
            EXPECT_EQ(world.WindowSize(), sim::ClientWindowSize(wide, kWindow));
        }
    }

    // A session on a region that fits: the stage 6 window (the region's, loaded whole), one
    // WindowUpdate only, though the player walks.
    Field f(Settings(small), Lossy());
    ASSERT_TRUE(f.started);
    net::ClientSettings cs;
    cs.windowChunks = 48;
    Field::Bot& bot = f.AddBot(Walk(small, {{8.0f, 8.0f}, {-8.0f, 6.0f}}), cs);
    ASSERT_TRUE(f.Join());
    EXPECT_FALSE(bot.client->WindowMoves());
    EXPECT_EQ(bot.simulation->World().WindowSize(), sim::ClientWindowSize(small, 0));
    EXPECT_EQ(bot.client->Window(), net::RegionSyncBox(small));
    EXPECT_EQ(bot.client->SyncWindow(), net::RegionSyncBox(small));
    f.Crater(2.0, 2.0);
    f.Passes(400);
    ASSERT_TRUE(f.Drain());
    EXPECT_EQ(bot.client->Stats().windowUpdates, 1u);
    EXPECT_EQ(f.server.Stats().windowUpdates, 1u);
    f.ExpectSameWorld(bot);
}

TEST(ClientWindow, APlayerWalksInFromOutsideIntoChunksModifiedBefore) {
    Field f(Settings(), Lossy());
    ASSERT_TRUE(f.started);
    // 30 craters on the far side (x 40..76 m) before anyone joins: outside the window (+-24 m) of a
    // player at the spawn.
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(f.Crater(40.0 + 1.2 * i, (i % 3 - 1) * 2.5));
        for (int t = 0; t < 6; ++t) f.server.Tick();
    }
    while (f.server.QueuedCasts() > 0) f.server.Tick();
    const size_t modifiedBefore = f.server.ModifiedChunks();
    EXPECT_GT(modifiedBefore, 10u);
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& bot = f.AddBot(Walk(f.server.Region(), {{74.0f, 0.0f}, {20.0f, 0.0f}}, &pilot));
    ASSERT_TRUE(f.Join());
    EXPECT_TRUE(bot.client->WindowMoves());
    // At the join none of those chunks is in the window: nothing to send.
    EXPECT_EQ(bot.client->Stats().chunkData, 0u);
    const glm::ivec3 far = ChunkAt(60.0, kGroundMetres - 0.5, 0.0);
    EXPECT_FALSE(bot.client->Window().Contains(far));
    // Walk there and back to x = 20 m; casts land ahead of the walker on the way (outside its window
    // when made) and beside it (inside).
    int pass = 0;
    while (!pilot->Done() && pass < 3600) {
        f.Pass();
        if (++pass % 60 == 0 && pass < 900) {
            const double x = bot.client->Player().position.x;
            f.Crater(std::min(x + 32.0, 76.0), 3.0);
            f.Crater(std::min(x + 4.0, 76.0), -4.0);
        }
    }
    EXPECT_TRUE(pilot->Done());
    EXPECT_EQ(pilot->WaypointsReached(), 2u);
    ASSERT_TRUE(f.Drain());
    const net::ClientStats& cs = bot.client->Stats();
    const net::PredictionStats& p = bot.client->Prediction();
    const net::ChunkSyncStats& ss = bot.client->Sync().Stats();
    std::printf("walk-in: %u window updates (%u resets), %u ChunkData (%u installed, %u stale), %.1f KiB bulk, %u chunk / %u model "
                "requests, store at most %u of %u; held %llu ticks, %llu corrections, %llu snaps; server: %llu ChunkData, %u dropped\n",
                cs.windowUpdates, cs.windowResets, cs.chunkData, ss.chunksInstalled, ss.staleChunkData, static_cast<double>(cs.bulkBytes) / 1024.0,
                cs.chunkRequests, cs.modelRequests, cs.maxStoreUsed, bot.simulation->Chunks().Capacity(),
                static_cast<unsigned long long>(p.syncHeldTicks), static_cast<unsigned long long>(p.corrections),
                static_cast<unsigned long long>(p.snaps), static_cast<unsigned long long>(f.server.Stats().chunkData),
                f.server.Stats().chunksDropped);
    // The window moved with the player; the far chunks came as ChunkData when they entered.
    EXPECT_GT(cs.windowUpdates, 15u);
    EXPECT_EQ(cs.windowResets, 0u);
    EXPECT_GT(ss.chunksInstalled, 20u);
    // Walking at run speed never waited for a chunk's data; no chunk was requested (each came once
    // per entry); the store never filled.
    EXPECT_EQ(p.syncHeldTicks, 0u);
    EXPECT_EQ(cs.chunkRequests, 0u);
    EXPECT_EQ(f.server.Stats().chunkRequests, 0u);
    EXPECT_EQ(cs.unsyncedChunks, 0u);
    EXPECT_EQ(bot.simulation->World().Stats().storeFullLoads, 0u);
    EXPECT_LT(cs.maxStoreUsed, bot.simulation->Chunks().Capacity());
    EXPECT_EQ(cs.skippedChunks, 0u);
    EXPECT_EQ(p.snaps, 0u);
    // A chunk modified before the join, now inside the window, came from the server.
    const glm::ivec3 back = ChunkAt(40.0, kGroundMetres - 0.5, -2.5);     // the first crater's
    ASSERT_TRUE(bot.client->Window().Contains(back));
    EXPECT_TRUE(bot.client->Sync().FromData(back));
    f.ExpectSameWorld(bot);
}

TEST(ClientWindow, AChunkThatLeavesAndComesBackAfterEditsIsSentAgain) {
    Field f(Settings(), Lossy());
    ASSERT_TRUE(f.started);
    std::shared_ptr<net::BotPilot> pilot;
    // Out to x = -40 m (the spawn's chunks leave the 48 m window) and back to the spawn.
    Field::Bot& bot = f.AddBot(Walk(f.server.Region(), {{-40.0f, 0.0f}, {0.5f, 0.5f}}, &pilot));
    ASSERT_TRUE(f.Join());
    // C: a crater 10 m east of the spawn (in the window), P: a pristine chunk beside it.
    const glm::ivec3 c = ChunkAt(10.0, kGroundMetres - 0.5, 0.0), p = ChunkAt(10.0, kGroundMetres - 0.5, 8.0);
    ASSERT_TRUE(f.Crater(10.0, 1.5));
    const uint32_t before = f.server.LastOpSeq();
    uint32_t firstSeq = 0, secondSeq = 0;
    bool left = false, edited = false;
    for (int pass = 0; pass < 3600 && !pilot->Done(); ++pass) {
        f.Pass();
        if (firstSeq == 0 && f.server.LastOpSeq() > before) firstSeq = f.server.LastOpSeq();
        if (!left && !bot.client->Window().Contains(c)) left = true;
        if (left && !edited) {
            // C is out of the client's window: edited again, which the client cannot see.
            ASSERT_TRUE(f.Crater(10.5, 1.0, 1.0f));
            edited = true;
        }
        if (edited && secondSeq == 0 && f.server.QueuedCasts() == 0) secondSeq = f.server.LastOpSeq();
    }
    ASSERT_TRUE(left);
    ASSERT_TRUE(pilot->Done());
    ASSERT_TRUE(f.Drain());
    ASSERT_GT(secondSeq, firstSeq);
    ASSERT_TRUE(bot.client->Window().Contains(c));
    ASSERT_TRUE(bot.client->Window().Contains(p));
    // C came back from the server as of its second edit or later; P was generated again (pristine:
    // nothing was sent for it).
    EXPECT_TRUE(bot.client->Sync().FromData(c));
    EXPECT_GE(bot.client->Sync().BaseOf(c), secondSeq);
    EXPECT_FALSE(bot.client->Sync().FromData(p));
    EXPECT_EQ(bot.client->Sync().StateOf(p), net::ChunkSyncState::Based);
    // 80 m of walking: the window moved about every 2 chunks (one chunk of follow slack).
    EXPECT_GE(bot.client->Stats().windowUpdates, 8u);
    f.ExpectSameWorld(bot);
}

TEST(ClientWindow, QueuedChunksThatLeaveTheWindowAreDropped) {
    Field f(Settings(), Lossy());
    ASSERT_TRUE(f.started);
    // Many modified chunks around the spawn, before the join: a long bulk queue.
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(f.Crater(-20.0 + (i % 8) * 5.0, -20.0 + (i / 8) * 9.0, 2.0f));
        for (int t = 0; t < 6; ++t) f.server.Tick();
    }
    while (f.server.QueuedCasts() > 0) f.server.Tick();
    f.server.SetMaxBulkBytesPerSecond(16384.0);
    Field::Bot& bot = f.AddBot(Stand);
    bot.manual = true;                     // the test moves its window (a teleport, no player)
    for (int i = 0; i < 40 && !bot.simulation; ++i) f.Pass();
    ASSERT_TRUE(bot.simulation);
    f.Passes(20);
    const uint64_t sentBefore = f.server.Stats().chunkData;
    // The window jumps 80 m east while most of its queue is still waiting.
    const glm::dvec3 east(80.0, kGroundMetres, 0.0);
    const std::array<glm::dvec3, 1> focus = {east};
    for (int pass = 0; pass < 60; ++pass) {
        bot.simulation->UpdateStreaming(east, focus);
        f.Pass();
    }
    f.server.SetMaxBulkBytesPerSecond(0.0);
    bool synced = false;
    for (int pass = 0; pass < 1800 && !synced; ++pass) {
        bot.simulation->UpdateStreaming(east, focus);
        f.Pass();
        synced = bot.client->Synced() && bot.client->AppliedOpSeq() == f.server.LastOpSeq();
    }
    ASSERT_TRUE(synced);
    std::printf("teleport: %llu ChunkData sent before, %llu in all, %u dropped at dequeue; client: %u stale, %u installed\n",
                static_cast<unsigned long long>(sentBefore), static_cast<unsigned long long>(f.server.Stats().chunkData),
                f.server.Stats().chunksDropped, bot.client->Sync().Stats().staleChunkData, bot.client->Sync().Stats().chunksInstalled);
    EXPECT_GT(f.server.Stats().chunksDropped, 20u);
    EXPECT_EQ(bot.client->Stats().windowResets, 0u);
    EXPECT_EQ(bot.client->Window().min, sim::ClientWindowMinChunk(f.server.Region(), kWindow, east));
    f.ExpectSameWorld(bot);
}

// Two window steps between two Polls (the game streams every frame and polls on ticks; review of
// stage 7a): the client names both steps in order (no reset), so a chunk that left at the first and
// came back at the second - regenerated pristine - enters again with the second update and the server
// sends it again.
TEST(ClientWindow, TwoWindowStepsBetweenPollsAreTwoUpdates) {
    Field f(Settings(), Lossy());
    ASSERT_TRUE(f.started);
    Field::Bot& bot = f.AddBot(Stand);
    bot.manual = true;
    for (int i = 0; i < 40 && !bot.simulation; ++i) f.Pass();
    ASSERT_TRUE(bot.simulation);
    // C: a crater at the window's -x edge, made and synced while the window is at the spawn.
    const glm::dvec3 home = f.bots.front()->client->Spawn();
    const std::array<glm::dvec3, 1> focusHome = {home};
    ASSERT_TRUE(f.Crater(home.x - 20.0, home.z));
    for (int pass = 0; pass < 900 && !(bot.client->Synced() && f.server.QueuedCasts() == 0 &&
                                         bot.client->AppliedOpSeq() == f.server.LastOpSeq()); ++pass) {
        bot.simulation->UpdateStreaming(home, focusHome);
        f.Pass();
    }
    const glm::ivec3 c = ChunkAt(home.x - 20.0, kGroundMetres - 0.5, home.z);
    ASSERT_TRUE(bot.client->Sync().IsCurrent(c));
    const uint32_t updatesBefore = bot.client->Stats().windowUpdates;
    // Two moves between two Polls: 16 m east (C leaves, its cell is regenerated pristine for another
    // chunk), then home again (C is generated pristine).
    const glm::dvec3 east = home + glm::dvec3(16.0, 0.0, 0.0);
    bot.simulation->UpdateStreaming(east, std::array<glm::dvec3, 1>{east});
    bot.simulation->Jobs().WaitIdle();
    bot.simulation->UpdateStreaming(home, focusHome);
    const uint32_t ackedBefore = static_cast<uint32_t>(f.server.Stats().ackedModified);
    bool synced = false;
    for (int pass = 0; pass < 1800 && !synced; ++pass) {
        bot.simulation->UpdateStreaming(home, focusHome);
        f.Pass();
        synced = bot.client->Synced() && bot.client->AppliedOpSeq() == f.server.LastOpSeq();
    }
    ASSERT_TRUE(synced);
    // Two updates, no reset; the second marked (and the server sent) C again.
    EXPECT_EQ(bot.client->Stats().windowResets, 0u);
    EXPECT_EQ(bot.client->Stats().windowUpdates, updatesBefore + 2u);
    EXPECT_GT(f.server.Stats().ackedModified, ackedBefore);
    EXPECT_TRUE(bot.client->Sync().FromData(c));
    f.ExpectSameWorld(bot);
}

// The game's pattern (review of stage 7a): the window follows the camera every frame, the client
// polls on ticks. A walker whose window is also pulled to a point 10 m off its feet between its ticks
// (a third-person camera swinging round) steps twice between Polls again and again: every step is
// named, nothing resets, the player is never held, and the window ends the server's.
TEST(ClientWindow, AWindowThatStepsBetweenPollsWhileWalkingNeverResets) {
    Field f(Settings(), Lossy());
    ASSERT_TRUE(f.started);
    // Craters along the walk (diagonal, north-east) before the join: modified chunks enter and leave.
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(f.Crater(8.0 + 4.0 * i, 6.0 + 4.0 * i, 2.0f));
        for (int t = 0; t < 6; ++t) f.server.Tick();
    }
    while (f.server.QueuedCasts() > 0) f.server.Tick();
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& bot = f.AddBot(Walk(f.server.Region(), {{40.0f, 38.0f}}, &pilot));
    ASSERT_TRUE(f.Join());
    uint32_t extraSteps = 0;
    for (int pass = 0; pass < 2400 && !pilot->Done(); ++pass) {
        if (pass % 3 == 0) {
            // The "camera" 10 m off the feet, swinging round: a step out and (in RunTick) a step back.
            const double a = pass * 0.21;
            const glm::dvec3 feet(bot.client->Player().position);
            const glm::dvec3 camera = feet + glm::dvec3(10.0 * std::cos(a), 1.5, 10.0 * std::sin(a));
            const uint64_t revision = bot.simulation->World().WindowRevision();
            bot.simulation->UpdateStreaming(camera, std::array<glm::dvec3, 1>{camera});
            extraSteps += bot.simulation->World().WindowRevision() != revision ? 1u : 0u;
        }
        f.Pass();
    }
    ASSERT_TRUE(pilot->Done());
    ASSERT_TRUE(f.Drain());
    const net::ClientStats& cs = bot.client->Stats();
    std::printf("steps between polls: %u extra steps, %u window updates, %u resets, %llu ticks held, %u ChunkData\n", extraSteps,
                cs.windowUpdates, cs.windowResets, static_cast<unsigned long long>(bot.client->Prediction().syncHeldTicks), cs.chunkData);
    EXPECT_GT(extraSteps, 10u);
    EXPECT_EQ(cs.windowResets, 0u);
    EXPECT_EQ(bot.client->Prediction().syncHeldTicks, 0u);
    f.ExpectSameWorld(bot);
}

TEST(ClientWindow, HashesCompareOverTheSameBoxAtTheSameOpSeq) {
    Field f(Settings(), Lossy());
    ASSERT_TRUE(f.started);
    // A crater in the spawn's window before the join; its chunks wait while the bulk is held back.
    ASSERT_TRUE(f.Crater(6.0, 6.0));
    while (f.server.QueuedCasts() > 0) f.server.Tick();
    f.server.SetMaxBulkBytesPerSecond(1.0);
    Field::Bot& bot = f.AddBot(Stand);
    for (int i = 0; i < 300; ++i) f.Pass();
    ASSERT_TRUE(bot.simulation);
    const voxel::WorldHashResult held = bot.client->Hash();
    EXPECT_FALSE(held.Complete());          // chunks of its sync box wait for their data
    EXPECT_FALSE(bot.client->Synced());
    f.server.SetMaxBulkBytesPerSecond(0.0);
    ASSERT_TRUE(f.Drain());
    // The sync box: the window inside the server's (here the whole window).
    EXPECT_EQ(bot.client->SyncWindow(), net::Intersect(bot.client->Window(), net::RegionSyncBox(f.server.Region())));
    f.ExpectSameWorld(bot);
    // The comparison can fail: one voxel of the client's sync box changed gives another hash, and a
    // box one chunk wider is another sum.
    const voxel::WorldHashResult good = bot.client->Hash();
    net::SyncBox wider = bot.client->SyncWindow();
    wider.min.x -= 1;
    wider.size.x += 1;
    EXPECT_NE(f.server.Hash(wider).hash, good.hash);
    const glm::ivec3 v = glm::ivec3(glm::floor(glm::dvec3(1.0, kGroundMetres - 0.5, 1.0) / 0.125));
    ASSERT_TRUE(bot.simulation->World().SetVoxel(v, 0));
    EXPECT_NE(bot.client->Hash().hash, f.server.Hash(bot.client->SyncWindow()).hash);
}

// networking.md "Chunk sync": a full store marks a chunk Unsynced (logged, counted, not walkable, not
// hashed), it is not requested again until a slot is free, and then once. Here with a moving window:
// the chunks that need slots are the modified ones that enter as the player walks.
TEST(ClientWindow, AFullStoreWhileWalkingMarksChunksUnsyncedAndRequestsEachOnceWhenSlotsFree) {
    net::ServerSettings settings = Settings();
    settings.countChunkRequests = true;
    Field f(settings, Lossy());
    ASSERT_TRUE(f.started);
    // 18 small gold boxes in the air (y = 20 m) at x = 30..50 m before the join, one per chunk: outside
    // the window at the spawn, on the walker's way. Their chunks are Empty when pristine; as data they
    // need a slot each.
    for (int i = 0; i < 18; ++i) {
        const glm::ivec3 lo(240 + 32 * (i % 6) + 8, 160, 32 * (i / 6 - 1) + 8);
        ASSERT_NE(f.server.ApplyOp(sim::WorldOp::Box(lo, lo + glm::ivec3(2), DefaultIds().gold)), 0u);
    }
    f.server.Tick();
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& bot = f.AddBot(Walk(f.server.Region(), {{44.0f, 0.0f}}, &pilot));
    ASSERT_TRUE(f.Join());
    // The store is taken (as if a larger window held it) but for 4 slots. The window's dense surface
    // chunks keep their number as it moves (those that leave free what those that enter take); the
    // boxes' ChunkData need slots of their own.
    voxel::ChunkStore& store = bot.simulation->Chunks();
    std::vector<uint32_t> taken;
    for (uint32_t slot; (slot = store.Alloc()) != voxel::ChunkStore::kInvalidSlot;) taken.push_back(slot);
    ASSERT_GT(taken.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        (void)store.Release(taken.back());
        taken.pop_back();
    }
    for (int pass = 0; pass < 2400 && !pilot->Done(); ++pass) f.Pass();
    ASSERT_TRUE(pilot->Done());
    f.Passes(120);
    const net::ChunkSyncStats& ss = bot.client->Sync().Stats();
    const uint32_t unsynced = ss.unsynced;
    std::printf("full store: %u chunk(s) unsynced, %u installed, %u requested while full; held %llu ticks\n", unsynced, ss.chunksInstalled,
                ss.requested, static_cast<unsigned long long>(bot.client->Prediction().syncHeldTicks));
    EXPECT_GT(unsynced, 0u);
    EXPECT_EQ(ss.requested, 0u) << "requested while the store is full";
    EXPECT_EQ(f.server.Stats().chunkRequests, 0u);
    EXPECT_EQ(bot.client->State(), net::ClientState::Playing);
    EXPECT_FALSE(bot.client->Synced());
    EXPECT_FALSE(bot.client->Hash().Complete()) << "an unsynced chunk is not hashed as the server's";
    // The chunks Unsynced in the window now: each must be requested exactly once once slots free.
    std::vector<glm::ivec3> waiting;
    const net::SyncBox window = bot.client->Window();
    for (int z = window.min.z; z < window.Max().z; ++z)
        for (int y = window.min.y; y < window.Max().y; ++y)
            for (int x = window.min.x; x < window.Max().x; ++x)
                if (bot.client->Sync().StateOf({x, y, z}) == net::ChunkSyncState::Unsynced) waiting.emplace_back(x, y, z);
    EXPECT_FALSE(waiting.empty());
    // Slots free: each unsynced chunk still in the window is requested once, installed, and the
    // window is the server's.
    for (const uint32_t slot : taken) (void)store.Release(slot);
    ASSERT_TRUE(f.Drain());
    // Per coordinate, as the server received them (review of stage 7a: a total could hide one chunk
    // asked twice and another never).
    const std::vector<std::pair<glm::ivec3, uint32_t>> counts = f.server.ChunkRequestCounts();
    for (const auto& [chunk, n] : counts) EXPECT_EQ(n, 1u) << "chunk (" << chunk.x << ", " << chunk.y << ", " << chunk.z << ") requested " << n << " times";
    for (const glm::ivec3& c : waiting) {
        const auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& e) { return e.first == c; });
        EXPECT_TRUE(it != counts.end()) << "unsynced chunk (" << c.x << ", " << c.y << ", " << c.z << ") never requested";
    }
    std::printf("full store: %u requested after the slots freed, %u installed in all\n", ss.requested, ss.chunksInstalled);
    EXPECT_GT(ss.requested, 0u);
    EXPECT_LE(ss.requested, unsynced) << "a chunk requested twice";
    EXPECT_EQ(ss.unsynced, unsynced) << "a chunk found no slot after the slots freed";
    EXPECT_EQ(f.server.Stats().chunkRequests, ss.requested);
    EXPECT_EQ(bot.client->Stats().chunkRequests, ss.requested);
    f.ExpectSameWorld(bot);
}

// networking.md "The moving client window": entities are not windowed. A replicated piece that breaks
// off outside the window (its Detach touches no chunk this client holds) cannot be built from the
// voxels it removed: the client asks for its model (ModelRequest) and copies the server's body.
TEST(ClientWindow, APieceThatBreaksOffOutsideTheWindowIsAskedFor) {
    Field f(Settings(), Lossy());
    ASSERT_TRUE(f.started);
    Field::Bot& bot = f.AddBot(Stand);
    ASSERT_TRUE(f.Join());
    ASSERT_TRUE(f.Drain());
    // A mushroom (a cap on a stem, cut: an island of the replicated size) on a drill site 37.5 m east
    // of the middle: every chunk of it outside the client's 48 m window.
    const net::BlastDrill drill(f.server.Region(), DefaultIds(), kGround);
    uint32_t index = 0;
    for (; index < 3 * drill.Sites(); ++index) {
        const net::DrillStep s = drill.Step(index);
        const glm::dvec3 centre = glm::dvec3(s.cast.center) / static_cast<double>(voxel::kQuantaPerVoxel) * 0.125;
        if (s.kind == net::DrillKind::Mushroom && centre.x > 36.0 && centre.x < 52.0 && std::abs(centre.z) < 14.0) break;
    }
    ASSERT_LT(index, 3 * drill.Sites());
    const net::DrillStep step = drill.Step(index);
    const glm::ivec3 site = voxel::VoxelToChunk(step.cast.center / voxel::kQuantaPerVoxel);
    ASSERT_FALSE(bot.client->Window().Contains(site));
    const size_t objectsBefore = f.server.Entities().size();
    const uint32_t baselineBefore = bot.client->Stats().baselineEntities;
    ASSERT_TRUE(net::RunDrillStep(f.server, step));
    f.Passes(240);
    ASSERT_TRUE(f.Drain());
    const net::ClientStats& cs = bot.client->Stats();
    std::printf("piece outside the window: %zu server objects (%zu before), %u model request(s), %u answer(s), %u copies made\n",
                f.server.Entities().size(), objectsBefore, cs.modelRequests, cs.baselineEntities - baselineBefore, cs.copiesMade);
    // The cap is a server body (with the step's crates); the client asked for it and holds the server's.
    EXPECT_GT(f.server.Stats().detachBodies, 0u);
    EXPECT_GE(cs.modelRequests, 1u);
    EXPECT_GE(cs.baselineEntities - baselineBefore, 1u);
    EXPECT_EQ(cs.copiesFailed, 0u);
    // Nothing of the site entered: the window's terrain is untouched, its hash the server's.
    EXPECT_EQ(bot.client->Stats().chunkData, 0u);
    f.ExpectSameWorld(bot);
}

TEST(ClientWindow, ThePlayerIsHeldOverChunksWithoutABase) {
    Field f(Settings(), Lossy());
    ASSERT_TRUE(f.started);
    // Craters at x = 30..36 m: outside the window of a player at the spawn, on its way east.
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(f.Crater(30.0 + 2.0 * i, (i % 2) * 3.0 - 1.5));
        for (int t = 0; t < 6; ++t) f.server.Tick();
    }
    while (f.server.QueuedCasts() > 0) f.server.Tick();
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& bot = f.AddBot(Walk(f.server.Region(), {{40.0f, 0.0f}}, &pilot));
    ASSERT_TRUE(f.Join());
    // Their data is held back: the player walks onto chunks that have no base.
    f.server.SetMaxBulkBytesPerSecond(1.0);
    uint64_t held = 0;
    for (int pass = 0; pass < 1200 && held < 30; ++pass) {
        f.Pass();
        held = bot.client->Prediction().syncHeldTicks;
    }
    EXPECT_GE(held, 30u);
    EXPECT_GT(bot.client->Prediction().syncTaken, 0u);
    const uint64_t correctionsBefore = bot.client->Prediction().corrections;
    f.server.SetMaxBulkBytesPerSecond(0.0);
    for (int pass = 0; pass < 1800 && !pilot->Done(); ++pass) f.Pass();
    EXPECT_TRUE(pilot->Done());
    ASSERT_TRUE(f.Drain());
    const net::PredictionStats& p = bot.client->Prediction();
    std::printf("held: %llu ticks held, %llu snapshots taken as they were, %llu resumes, %llu corrections (%llu before the data), %llu snaps\n",
                static_cast<unsigned long long>(p.syncHeldTicks), static_cast<unsigned long long>(p.syncTaken),
                static_cast<unsigned long long>(p.syncResumes), static_cast<unsigned long long>(p.corrections),
                static_cast<unsigned long long>(correctionsBefore), static_cast<unsigned long long>(p.snaps));
    EXPECT_GE(p.syncResumes, 1u);
    EXPECT_LE(p.corrections, correctionsBefore + 2u);
    EXPECT_EQ(p.snaps, 0u);
    f.ExpectSameWorld(bot);
}

// Review of stage 7a: the start gate waited for the whole entity baseline, which grows with every live
// body of the session (2-3 KiB each with its model). The server now marks the end of the entities near
// the spawn (BaselineNear) and the joiner starts on it; the far ones follow. Here 225 crates 40-68 m
// east of the spawn and 4 beside it, a 64 kB/s downlink: the joiner predicts before its baseline ends.
TEST(ClientWindow, AJoinerStartsBeforeAFarBaselineEnds) {
    net::Impairment im;
    im.delaySeconds = 0.04;
    im.peerBytesPerSecond = 64.0 * 1024.0;
    Field f(Settings(), im);
    ASSERT_TRUE(f.started);
    const std::shared_ptr<voxel::VoxelModel> crate = sim::CrateModel(DefaultIds(), 1.0);
    uint32_t spawned = 0;
    for (int i = 0; i < 4; ++i) spawned += f.server.SpawnObject(crate, glm::dvec3(4.0 + 2.0 * i, kGroundMetres + 0.1, 3.0)) != 0 ? 1u : 0u;
    for (int x = 0; x < 15; ++x)
        for (int z = 0; z < 15; ++z)
            spawned += f.server.SpawnObject(crate, glm::dvec3(40.0 + 2.0 * x, kGroundMetres + 0.1, -14.0 + 2.0 * z)) != 0 ? 1u : 0u;
    ASSERT_EQ(spawned, 229u);
    for (int i = 0; i < 600 && f.server.AwakeBodies() > 0; ++i) f.server.Tick();
    Field::Bot& bot = f.AddBot(Stand);
    ASSERT_TRUE(f.Join(1800));
    ASSERT_TRUE(f.Drain(3600));
    const net::ClientStats& cs = bot.client->Stats();
    std::printf("far baseline: %u baseline entities (%.1f KiB of bulk in all); started after %.2f s, near set in after %.2f s, baseline "
                "ended after %.2f s (server ticks)\n",
                cs.baselineEntities, static_cast<double>(cs.bulkBytes) / 1024.0, cs.joinStartTicks / 60.0, cs.baselineNearTicks / 60.0,
                cs.baselineEndTicks / 60.0);
    EXPECT_GE(cs.baselineEntities, 229u);
    EXPECT_EQ(f.server.Stats().baselineNear, 1u);
    EXPECT_GT(cs.baselineNearTicks, 0u);
    EXPECT_LT(cs.baselineNearTicks, cs.baselineEndTicks);
    EXPECT_LT(cs.joinStartTicks, cs.baselineEndTicks) << "the player waited for the far baseline";
    f.ExpectSameWorld(bot);
}
