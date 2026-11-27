// Hollowkeep in a session and the siege (docs/design/fight.md "Siege", fortress.md): the server builds the hollowkeep
// template on the land generator's map, replicates the portcullis and its lever as mechanisms and the props as server
// bodies, a client agrees with it, and an attacker who holds the sanctum long enough wins the round. One process,
// loopback links, one manual clock (FightTests.cpp's FightField).

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    net::Token SiegeToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x5a + i * 3);
        return t;
    }

    /// HollowkeepTests.cpp's map: a plain (the starter region) with a mountain knoll on it, a ridge to the west, the sea
    /// to the south. The bake puts the castle on the knoll.
    voxel::LandSource KnollMap() {
        voxel::LandSource s;
        s.width = 200;
        s.height = 160;
        s.metresPerPixel = 5.0f;
        s.heights.resize(size_t(200) * 160);
        s.biomes.resize(s.heights.size());
        s.regions.resize(s.heights.size());
        for (int z = 0; z < 160; ++z)
            for (int x = 0; x < 200; ++x) {
                const size_t i = size_t(z) * 200 + size_t(x);
                if (z >= 140) {
                    s.heights[i] = -6.0f;
                    s.biomes[i] = uint8_t(voxel::LandBiome::Sea);
                    continue;
                }
                if (x < 70) {
                    s.heights[i] = 20.0f + float(std::min(70 - x, 30)) * 3.0f;
                    s.biomes[i] = uint8_t(voxel::LandBiome::Mountain);
                    s.regions[i] = 6;
                    continue;
                }
                s.heights[i] = 3.0f + float(x - 70) * 0.02f + float(140 - z) * 0.03f;
                s.biomes[i] = uint8_t(voxel::LandBiome::Grassland);
                s.regions[i] = 2;
                const float dx = float(x - 110), dz = float(z - 60);
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d < 18.0f) {
                    s.heights[i] += 32.0f * (1.0f - d / 18.0f) - (dz > 0.0f ? dz * 0.2f : 0.0f);
                    s.biomes[i] = uint8_t(voxel::LandBiome::Mountain);
                    s.regions[i] = 6;
                }
            }
        s.starterRegion = 2;
        return s;
    }

    struct Castle {
        std::shared_ptr<const voxel::LandMap> map;
        voxel::FortPlan plan;
        sim::RegionDesc region;
    };

    /// The baked knoll and a hollowkeep region 96 m square round the keep (24 x 24 chunks of 4 m), -16..96 m.
    const Castle& KnollCastle() {
        static const Castle c = [] {
            Castle out;
            out.map = std::make_shared<const voxel::LandMap>(voxel::BakeLand(KnollMap()));
            voxel::MaterialLibrary library;
            voxel::LandGeneratorSettings land;
            land.materials = voxel::RegisterDefaultWorldMaterials(library);
            land.origin = voxel::LandWorldOrigin(*out.map);
            out.plan = voxel::PlanFort(*out.map, land);
            sim::RegionDesc& r = out.region;
            r.templateId = sim::RegionTemplateId::Hollowkeep;
            r.seed = 7;
            const glm::ivec2 keep(int(std::floor(out.plan.keep.x / 4.0f)), int(std::floor(out.plan.keep.y / 4.0f)));
            r.playMinChunk = keep - glm::ivec2(12);
            r.playMaxChunk = keep + glm::ivec2(12);
            r.heightRange = {-16.0f, 96.0f};
            r.terrain.voxelSize = 0.125f;
            r.terrain.materials = land.materials;
            sim::SetRegionLand(r, out.map);
            return out;
        }();
        return c;
    }

    /// Defenders 14 m beside the keep (inside the bailey), attackers 6 m out past the bridge; a 2 s hold wins.
    sim::FightRules SiegeRules(const Castle& c) {
        sim::FightRules f;
        f.mode = sim::GameMode::Siege;
        const glm::vec2 out = c.plan.gatePoint + c.plan.axis * (c.plan.bridgeSpan + 8.0f);
        f.teamSpawns = {c.plan.keep + c.plan.across * 14.0f, out};
        f.minPlayers = 2;
        f.countdownTicks = 1;
        f.endTicks = 60;
        f.endSession = false;
        f.holdTicks = 120;
        f.roundTicks = 60 * 60;
        return f;
    }

    net::ServerSettings SiegeSettings(const Castle& c) {
        net::ServerSettings s;
        s.session.token = SiegeToken();
        s.session.region = c.region;
        s.session.packHidden = false;
        s.session.fight = SiegeRules(c);
        s.simulation.jobThreads = 2;
        s.simulation.physics.threads = 1;
        s.simulation.physics.maxBodies = 2048;
        s.simulation.physics.maxBodyPairs = 4096;
        s.simulation.physics.maxContactConstraints = 4096;
        return s;
    }

    /// A server and standing clients on loopback links, one manual clock.
    struct SiegeField {
        struct Bot {
            net::LoopbackTransport loop;
            std::unique_ptr<net::Client> client;
            std::unique_ptr<sim::Simulation> simulation;
        };

        explicit SiegeField(net::ServerSettings settings) : server(std::move(settings), serverLoop) {
            started = server.Start();
            dt = started ? static_cast<double>(server.Simulation().FixedTimeStep()) : 1.0 / 60.0;
        }

        Bot& AddBot() {
            auto bot = std::make_unique<Bot>();
            net::ClientSettings cs;
            cs.token = SiegeToken();
            const std::shared_ptr<const voxel::LandMap> map = KnollCastle().map;
            cs.landMap = [map] { return map; };
            bot->client = std::make_unique<net::Client>(cs, bot->loop);
            net::LoopbackTransport::Connect(serverLoop, bot->loop);
            bots.push_back(std::move(bot));
            return *bots.back();
        }

        void ClientPass(Bot& bot) {
            if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation) {
                sim::SimulationConfig base;
                base.jobThreads = 2;
                base.chunkCapacity = 8192;
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
                bot.client->RunTick(sim::PlayerInput{});
                if (bot.simulation && bot.simulation->Terrain().Stats().pending != 0) bot.simulation->Jobs().WaitIdle();
            }
        }

        void Pass() {
            server.Advance(dt);
            if (server.Simulation().Terrain().Stats().pending != 0) server.Simulation().Jobs().WaitIdle();
            for (const std::unique_ptr<Bot>& bot : bots) ClientPass(*bot);
        }
        bool Until(const std::function<bool()>& done, int maxPasses) {
            for (int i = 0; i < maxPasses; ++i) {
                if (done()) return true;
                Pass();
            }
            return done();
        }
        bool Settled() {
            if (server.QueuedCasts() > 0 || server.AwakeBodies() > 0) return false;
            for (const auto& bot : bots)
                if (!bot->simulation || bot->client->AppliedOpSeq() != server.LastOpSeq() || !bot->client->Synced()) return false;
            return true;
        }

        double dt = 1.0 / 60.0;
        net::LoopbackTransport serverLoop;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Bot>> bots;
    };
}

TEST(Siege, ASiegeOnAPlainRegionIsRefused) {
    // The sanctum is Hollowkeep's: a siege on another template does not start.
    net::ServerSettings s = SiegeSettings(KnollCastle());
    s.session.region.templateId = sim::RegionTemplateId::Empty;
    net::LoopbackTransport loop;
    net::Server server(s, loop);
    EXPECT_FALSE(server.Start());
    // A defenders' spawn whose 6 m spawn area covers the sanctum would refuse the attackers' spells there.
    net::ServerSettings near = SiegeSettings(KnollCastle());
    near.session.fight.teamSpawns[0] = KnollCastle().plan.keep + KnollCastle().plan.across * 9.0f;
    net::LoopbackTransport loop2;
    net::Server server2(near, loop2);
    EXPECT_FALSE(server2.Start());
}

TEST(Siege, HollowkeepReplicatesItsPortcullisLeverAndPropsAndTheHoldWins) {
    const Castle& c = KnollCastle();
    ASSERT_TRUE(c.plan.placed);
    SiegeField f(SiegeSettings(c));
    ASSERT_TRUE(f.started);
    const sim::RegionTemplate& t = f.server.Template();
    ASSERT_TRUE(t.hasSanctum);
    ASSERT_EQ(t.mechanisms.size(), 2u) << "the portcullis and the guardroom's lever";
    EXPECT_GE(t.objects.size(), 4u) << "the props inside the play bounds";
    EXPECT_GE(t.lights.size(), 4u);
    // The sanctum is the keep's ground floor round the keep's middle.
    const float vs = 0.125f;
    const glm::vec3 lo = glm::vec3(t.sanctumMin) * vs, hi = glm::vec3(t.sanctumMax) * vs;
    EXPECT_LT(lo.x, c.plan.keep.x);
    EXPECT_GT(hi.x, c.plan.keep.x);
    EXPECT_NEAR(hi.x - lo.x, 9.5f, 0.25f);

    SiegeField::Bot& defender = f.AddBot();
    SiegeField::Bot& attacker = f.AddBot();
    ASSERT_TRUE(f.Until([&] { return f.server.RoundState().phase == sim::RoundPhase::Live; }, 1200));
    ASSERT_EQ(f.server.Fighter(defender.client->Slot()).team, sim::kSiegeDefenders);
    ASSERT_EQ(f.server.Fighter(attacker.client->Slot()).team, sim::kSiegeAttackers);

    // Every client holds the server's bodies: both mechanisms and every prop, the same models, resting where the
    // server's rest.
    ASSERT_TRUE(f.Until([&] { return f.Settled(); }, 1800));
    for (const auto& bot : f.bots) {
        const net::EntityComparison e = net::CompareEntities(f.server.Entities(), bot->client->Entities());
        EXPECT_EQ(e.server, t.objects.size() + t.mechanisms.size());
        EXPECT_EQ(e.missing, 0u);
        EXPECT_EQ(e.extra, 0u);
        EXPECT_EQ(e.modelMismatches, 0u);
        EXPECT_LT(e.maxRestError, 0.05f);
        EXPECT_EQ(bot->client->Hash().hash, f.server.Hash(bot->client->SyncWindow()).hash);
    }

    // The attacker stands in the sanctum, the defender outside: the hold starts, every client hears it, and 2 s win.
    const glm::vec3 room(c.plan.keep.x, c.plan.keepGround + 0.05f, c.plan.keep.y);
    ASSERT_TRUE(f.server.TeleportPlayer(attacker.client->Slot(), room));
    ASSERT_TRUE(f.Until([&] { return f.server.RoundState().holdSince != 0; }, 30));
    const uint32_t since = f.server.RoundState().holdSince;
    ASSERT_TRUE(f.Until([&] { return attacker.client->RoundOf().holdSince == since; }, 30));
    EXPECT_EQ(defender.client->RoundOf().holdTicks, 120u);
    ASSERT_TRUE(f.Until([&] { return f.server.RoundState().phase == sim::RoundPhase::Ended; }, 200));
    EXPECT_EQ(f.server.RoundState().winner, sim::kSiegeAttackers);
    EXPECT_EQ(f.server.RoundState().end, sim::RoundEnd::Held);
    EXPECT_GE(f.server.TickNumber() - 1 - since + 1, 120u);
    ASSERT_TRUE(f.Until([&] { return defender.client->RoundOf().phase == sim::RoundPhase::Ended; }, 30));
    EXPECT_EQ(defender.client->RoundOf().end, sim::RoundEnd::Held);
}
