// The `Field` harness of the session tests with a moving client window (networking.md "Stage 7a plan"
// M-b, "Stage 7b plan"): a server and walking bots on one manual clock, the regions they share, the
// pilot helpers. Moved out of ClientWindowTests.cpp for Region250Tests.cpp (stage 7b).
//
// Not a module and not self-contained: a test file includes it AFTER its own #includes (gtest, glm,
// <algorithm>, <array>, <cmath>, <cstdint>, <cstdio>, <functional>, <memory>, <span>, <utility>,
// <vector>) and its imports (vortex.core, vortex.voxel, vortex.sim, vortex.net) and its
// `using namespace vortex;` - a standard header included after an import can clash with the module's
// own copy, so this file includes nothing.

#pragma once

namespace {
    [[maybe_unused]] constexpr int kGround = 96;                    // flat ground: voxels y < 96 (12 m at 0.125 m)
    [[maybe_unused]] constexpr float kGroundMetres = kGround * 0.125f;
    [[maybe_unused]] constexpr int32_t kWindow = 12;                // the client's window, chunks (48 m)

    [[maybe_unused]] net::Token TestToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x63 + i * 5);
        return t;
    }

    [[maybe_unused]] voxel::WorldMaterials DefaultIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    /// 40 x 40 chunks of play (160 m), 46 x 8 x 46 chunks of server window, flat ground at 12 m.
    [[maybe_unused]] sim::RegionDesc WideRegion() {
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
    [[maybe_unused]] sim::RegionDesc SmallRegion() {
        sim::RegionDesc r = WideRegion();
        r.playMinChunk = {-4, -4};
        r.playMaxChunk = {3, 3};
        return r;
    }

    [[maybe_unused]] net::ServerSettings Settings(const sim::RegionDesc& region = WideRegion()) {
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

    [[maybe_unused]] net::Impairment Lossy() {
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
            bool packHidden = false;      ///< its world packs buried chunks (WorldConfig::packHidden, as the game's)
            /// Waits for its jobs after every tick (the stage 6 rule). Off only while a test holds one of
            /// its generation jobs (a chunk still loading): waiting would never end.
            bool waitIdle = true;
            /// Called after each of its ticks (stage 7b tests watch a chunk or the reach box per tick).
            std::function<void(Bot&)> afterTick;
        };

        /// `seed` varies the links' random draws (loss, jitter) from the default (0) runs.
        explicit Field(net::ServerSettings settings = Settings(), net::Impairment impairment = {}, uint64_t seed = 0)
            : impairment(std::move(impairment)), seed(seed), serverLink(serverLoop, this->impairment, 11 + 7919 * seed, [this] { return clock; }),
              server(std::move(settings), serverLink) {
            started = server.Start();
        }

        Bot& AddBot(std::function<sim::PlayerInput(Bot&)> controls, net::ClientSettings cs = {}) {
            auto bot = std::make_unique<Bot>();
            bot->link = std::make_unique<net::ImpairedTransport>(bot->loop, impairment, 900 + 7919 * seed + bots.size(), [this] { return clock; });
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
                base.world.packHidden = bot.packHidden;
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
                const sim::PlayerInput in = bot.client->Predicting() && !draining ? bot.controls(bot) : sim::PlayerInput{};
                bot.client->RunTick(in);
                // Reproducible runs (stage 6 rule): collider cooks and loads finish inside the tick.
                if (bot.simulation && bot.waitIdle) bot.simulation->Jobs().WaitIdle();
                // A presenting bot shows what it applied (particles, its own rubble), as a game does.
                if (bot.simulation)
                    for (const sim::BlastResult& blast : bot.client->TakePresentations()) sim::PresentBlast(*bot.simulation, blast);
                if (bot.afterTick) bot.afterTick(bot);
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

        /// Drain (the players stand, stage 7b) until also every replicated body rests (the entity check
        /// compares rest poses).
        bool Settle(int maxPasses = 3600) {
            draining = true;
            for (int i = 0; i < maxPasses; ++i) {
                bool done = server.QueuedCasts() == 0 && server.AwakeBodies() == 0;
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
        bool draining = false;            ///< the players stand (Settle)
        net::Impairment impairment;
        uint64_t seed = 0;
        net::LoopbackTransport serverLoop;
        net::ImpairedTransport serverLink;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Bot>> bots;
    };

    /// Controls that run the player to `points` (metres, x / z) in order, then stand.
    [[maybe_unused]] std::function<sim::PlayerInput(Field::Bot&)> Walk(const sim::RegionDesc& region, std::vector<glm::vec2> points,
                                                       std::shared_ptr<net::BotPilot>* out = nullptr, bool run = true) {
        net::PilotSettings ps;
        ps.waypoints = std::move(points);
        ps.jumps = false;
        ps.run = run;
        ps.stuckTicks = 600;
        auto pilot = std::make_shared<net::BotPilot>(region, 77, ps);
        if (out) *out = pilot;
        return [pilot](Field::Bot& bot) { return pilot->Next(bot.client->Player()); };
    }

    [[maybe_unused]] sim::PlayerInput Stand(Field::Bot&) { return {}; }

    [[maybe_unused]] glm::ivec3 ChunkAt(double x, double y, double z) {
        return voxel::VoxelToChunk(glm::ivec3(glm::floor(glm::dvec3(x, y, z) / 0.125)));
    }
}


namespace {
    /// Stage 7b's 250 m test region (networking.md "Stage 7b plan", Tests; choice 13): 64 x 64 chunks
    /// of play at 0.125 m (256 m), a server window of 70 x 8 x 70 chunks with rows 0-32 m, flat ground
    /// at 12 m (preload under a second). Its bots get the game's 48-chunk window and store.
    [[maybe_unused]] sim::RegionDesc Plain250() {
        sim::RegionDesc r = WideRegion();
        r.seed = 250;
        r.playMinChunk = {-32, -32};
        r.playMaxChunk = {32, 32};
        return r;
    }
    [[maybe_unused]] constexpr int32_t kGameWindow = 48;
    [[maybe_unused]] constexpr uint32_t kGameStore = 12288, kGameHeadroom = 2048;

    /// A bot of the 250 m tests: the game's window and its store headroom.
    [[maybe_unused]] net::ClientSettings GameClient(bool presenting = false) {
        net::ClientSettings cs;
        cs.windowChunks = kGameWindow;
        cs.minFreeStoreSlots = kGameHeadroom;
        cs.presentation = presenting;
        return cs;
    }
}


namespace {
    /// A client build whose generator makes other terrain everywhere (stage 7b, server terrain mode):
    /// the region's flat generator with its ground at `groundVoxels` instead of the region's.
    [[maybe_unused]] std::function<std::shared_ptr<voxel::IWorldGenerator>(std::shared_ptr<voxel::IWorldGenerator>)> OtherGround(
        const sim::RegionDesc& region, int groundVoxels) {
        return [region, groundVoxels](std::shared_ptr<voxel::IWorldGenerator>) {
            sim::RegionDesc other = region;
            other.flatGroundVoxels = groundVoxels;
            return sim::MakeRegionGenerator(other, DefaultIds());
        };
    }

    /// What server terrain mode cost a bot (the server's view) and what its client saw, one line.
    [[maybe_unused]] void PrintTerrain(const char* name, const Field& f, const Field::Bot& bot) {
        const net::TerrainPeerCost t = f.server.TerrainCost(bot.client->Slot());
        const double steps = t.steps > 0 ? static_cast<double>(t.stepBytes) / static_cast<double>(t.steps) / 1024.0 : 0.0;
        std::printf("[ terrain ] %s: join ack %.1f KiB + %u chunks / %.1f KiB of data (%u uniform in the ack); %u steps, ack + data %.1f KiB per "
                    "step on average, %.1f KiB at most; client: %s\n",
                    name, static_cast<double>(t.joinAckBytes) / 1024.0, t.joinChunks, static_cast<double>(t.joinDataBytes) / 1024.0,
                    t.joinUniform, t.steps, steps, static_cast<double>(t.maxStepBytes) / 1024.0, net::DescribeTerrainMode(*bot.client).c_str());
    }
}

