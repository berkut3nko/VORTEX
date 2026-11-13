// docs/design/networking.md stage 3: a session in one process. Session files, the build hash and
// probe chunks, the handshake, the server window, the template split, the cast budget, and the
// acceptance: an in-process server and two clients over a lossy loopback agree on the world after
// 200 server-side blasts that free terrain (in-box islands with crumbs, face-flood islands and
// weak-neck releases), one client joining late from the full event log.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.scene;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    constexpr int kGround = 96;              // flat ground: voxels y < 96 (12 m at 0.125 m)
    constexpr uint32_t kStepTicks = 6;

    net::Token TestToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x30 + i * 7);
        return t;
    }

    voxel::WorldMaterials DefaultIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    /// 7 x 7 chunks of play (28 m at 0.125 m), 13 x 8 x 13 chunks of window, flat ground at 12 m.
    sim::RegionDesc DrillRegion(sim::RegionTemplateId templateId = sim::RegionTemplateId::Empty) {
        sim::RegionDesc r;
        r.templateId = templateId;
        r.seed = 4242;
        r.playMinChunk = {-4, -4};
        r.playMaxChunk = {3, 3};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = kGround;
        r.terrain.voxelSize = 0.125f;
        r.terrain.materials = DefaultIds();
        return r;
    }

    net::ServerSettings Settings(const sim::RegionDesc& region, unsigned jobThreads = 2) {
        net::ServerSettings s;
        s.session.token = TestToken();
        s.session.region = region;
        s.session.packHidden = false;
        s.simulation.jobThreads = jobThreads;
        s.simulation.physics.threads = 1;
        s.simulation.physics.maxBodies = 2048;
        s.simulation.physics.maxBodyPairs = 4096;
        s.simulation.physics.maxContactConstraints = 4096;
        return s;
    }

    /// A server and its clients on loopback transports, every link impaired the same way, one
    /// manual clock: Tick() is one server tick and one tick of every client.
    struct Session {
        struct Bot {
            net::LoopbackTransport loop;
            std::unique_ptr<net::ImpairedTransport> link;
            std::unique_ptr<net::Client> client;
            std::unique_ptr<sim::Simulation> simulation;
            bool connected = false;
            sim::SimulationConfig base;
        };

        explicit Session(const net::ServerSettings& settings, net::Impairment impairment = {})
            : impairment(std::move(impairment)), serverLink(serverLoop, this->impairment, 77, [this] { return clock; }),
              server(settings, serverLink) {
            started = server.Start();
        }

        Bot& AddClient(net::ClientSettings settings = {}, bool connect = true) {
            if (settings.token == net::Token{}) settings.token = TestToken();
            auto bot = std::make_unique<Bot>();
            bot->link = std::make_unique<net::ImpairedTransport>(bot->loop, impairment, 1000 + bots.size(), [this] { return clock; });
            bot->client = std::make_unique<net::Client>(settings, *bot->link);
            bot->base.jobThreads = 2;
            bot->base.chunkCapacity = 4096;
            bot->base.physics.threads = 1;
            bot->base.physics.maxBodies = 2048;
            bot->base.physics.maxBodyPairs = 4096;
            bot->base.physics.maxContactConstraints = 4096;
            bots.push_back(std::move(bot));
            if (connect) Connect(*bots.back());
            return *bots.back();
        }

        void Connect(Bot& bot) {
            net::LoopbackTransport::Connect(serverLoop, bot.loop);
            bot.connected = true;
        }

        void Tick() {
            server.Tick();
            clock += 1.0 / 60.0;
            for (const std::unique_ptr<Bot>& b : bots) {
                Bot& bot = *b;
                if (!bot.connected) continue;
                bot.client->Poll();
                if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation && autoAttach) {
                    bot.simulation = std::make_unique<sim::Simulation>(bot.client->MakeSimulationConfig(bot.base));
                    ASSERT_TRUE(bot.simulation->Initialize(bot.client->MakeGenerator()));
                    ASSERT_TRUE(bot.client->Attach(*bot.simulation));
                }
                if (bot.simulation) {
                    for (const sim::BlastResult& blast : bot.client->TakePresentations()) sim::PresentBlast(*bot.simulation, blast);
                    bot.simulation->Step({});
                }
            }
        }

        /// Ticks until every attached client has applied the server's last event (and the cast queue is empty).
        bool Drain(uint32_t maxTicks = 900) {
            for (uint32_t i = 0; i < maxTicks; ++i) {
                bool done = server.QueuedCasts() == 0;
                for (const std::unique_ptr<Bot>& bot : bots)
                    done = done && (!bot->connected || (bot->simulation && bot->client->AppliedOpSeq() == server.LastOpSeq()));
                if (done) return true;
                Tick();
            }
            return false;
        }

        double clock = 0.0;
        net::Impairment impairment;
        net::LoopbackTransport serverLoop;
        net::ImpairedTransport serverLink;
        net::Server server;
        bool started = false;
        bool autoAttach = true;
        std::vector<std::unique_ptr<Bot>> bots;
    };

    net::Impairment Lossy() {
        net::Impairment im;
        im.loss = 0.05;
        im.delaySeconds = 0.04;
        im.jitterSeconds = 0.03;
        im.duplicate = 0.01;
        im.reorder = 0.02;
        return im;
    }
}

// --- session file, build hash, probes ----------------------------------------------------------

TEST(SessionFile, RoundTripsAndNamesItsErrors) {
    net::SessionConfig config;
    config.token = TestToken();
    config.region = DrillRegion();
    config.region.terrain.voxelSize = 0.1f;          // written and read back bit for bit
    config.region.heightRange = {-48.0f, 128.0f};
    config.maxClients = 10;
    config.packHidden = false;
    const std::string text = net::FormatSession(config);
    net::SessionConfig back;
    std::string error;
    ASSERT_TRUE(net::ParseSession(text, back, error)) << error << "\n" << text;
    EXPECT_EQ(back.token, config.token);
    sim::RegionDesc a = back.region, b = config.region;
    a.terrain.materials = b.terrain.materials = {};  // not part of the file (the server's own ids)
    EXPECT_TRUE(a == b);
    EXPECT_EQ(back.maxClients, 10u);
    EXPECT_FALSE(back.packHidden);

    EXPECT_FALSE(net::ParseSession("seed = 1\n", back, error));
    EXPECT_NE(error.find("token"), std::string::npos);
    EXPECT_FALSE(net::ParseSession(text + "colour = blue\n", back, error));
    EXPECT_NE(error.find("unknown key 'colour'"), std::string::npos);
    EXPECT_FALSE(net::ParseSession(text + "play_max = 3\n", back, error));
    EXPECT_NE(error.find("play_max"), std::string::npos);
    EXPECT_FALSE(net::ParseSession(text + "voxel_size = nan\n", back, error));
    EXPECT_NE(error.find("invalid region"), std::string::npos);
    EXPECT_FALSE(net::ParseSession(text + "height = 0 900\n", back, error));     // 225 rows at 4 m
    EXPECT_FALSE(net::ParseSession("token = 00112233\n", back, error));

    // The example sessions of vortex_server parse.
    for (const char* name : {"drill", "arena250", "playground"}) {
        const std::filesystem::path path = std::filesystem::path(VORTEX_TEST_SOURCE_DIR) / "tools/server/sessions" / (std::string(name) + ".session");
        EXPECT_TRUE(net::LoadSessionFile(path, back, error)) << error;
    }
}

TEST(SessionFile, RegionValidationRefusesWhatCannotBeBuilt) {
    sim::RegionDesc r = DrillRegion();
    std::string why;
    EXPECT_TRUE(sim::IsValid(r, &why)) << why;
    sim::RegionDesc bad = r;
    bad.playMaxChunk = bad.playMinChunk;
    EXPECT_FALSE(sim::IsValid(bad));
    bad = r;
    bad.playMaxChunk.x = bad.playMinChunk.x + sim::kMaxRegionChunks + 1;
    EXPECT_FALSE(sim::IsValid(bad));
    bad = r;
    bad.playMinChunk.x = -(1 << 26);
    EXPECT_FALSE(sim::IsValid(bad));
    bad = r;
    bad.heightRange = {10.0f, 10.0f};
    EXPECT_FALSE(sim::IsValid(bad));
    bad = r;
    bad.terrain.treeDensity = 2.0f;                    // voxel::IsValid
    EXPECT_FALSE(sim::IsValid(bad));
    bad = r;
    bad.flatGroundVoxels = 100000;
    EXPECT_FALSE(sim::IsValid(bad));
    bad = r;                                           // a playground must hold the plaza's search area
    bad.templateId = sim::RegionTemplateId::Playground;
    bad.playMinChunk = {40, 40};
    bad.playMaxChunk = {60, 60};
    EXPECT_FALSE(sim::IsValid(bad, &why));
    EXPECT_EQ(why, "a playground's play bounds must contain the plaza site around the origin");
    bad = r;
    bad.templateId = sim::RegionTemplateId::Playground;
    EXPECT_TRUE(sim::IsValid(bad, &why)) << why;
    bad = r;
    bad.templateId = static_cast<sim::RegionTemplateId>(99);
    EXPECT_FALSE(sim::IsValid(bad, &why));
    EXPECT_EQ(why, "unknown template");
}

TEST(SessionFile, BuildHashSeesTheMaterialTable) {
    voxel::MaterialLibrary a, b;
    const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(a);
    voxel::RegisterDefaultWorldMaterials(b);
    EXPECT_EQ(net::BuildHash(a), net::BuildHash(b));
    voxel::Material stone = b.Get(voxel::kWorldPalette, ids.stone);
    stone.strength[0] += 1;                            // shred.md R6: a strength reaches the hash
    const std::string name(b.Name(voxel::kWorldPalette, ids.stone));
    b.Set(voxel::kWorldPalette, ids.stone, stone, name);
    EXPECT_NE(net::BuildHash(a), net::BuildHash(b));
}

TEST(SessionFile, ProbeChunksFindAGeneratorThatDiffers) {
    sim::RegionDesc region = DrillRegion();
    region.generator = sim::RegionGenerator::Terrain;
    region.heightRange = {-48.0f, 128.0f};
    region.playMinChunk = {-10, -10};
    region.playMaxChunk = {10, 10};
    const auto generator = sim::MakeRegionGenerator(region, DefaultIds());
    const std::vector<net::ProbeHash> probes = net::ProbeChunks(region, *generator);
    ASSERT_EQ(probes.size(), net::kProbeChunks);
    EXPECT_EQ(probes, net::ProbeChunks(region, *generator));     // a function of the seed
    EXPECT_EQ(net::CheckProbes(probes, *generator), 0u);
    uint32_t air = 0;
    for (const net::ProbeHash& p : probes) air += p.hash == voxel::kAirChunkHash ? 1u : 0u;
    EXPECT_LT(air, net::kProbeChunks / 4) << "probes should hold terrain";
    // Another terrain seed: most probes differ.
    sim::RegionDesc other = region;
    other.terrain.seed += 1;
    EXPECT_GT(net::CheckProbes(probes, *sim::MakeRegionGenerator(other, DefaultIds())), net::kProbeChunks / 2);
}

// --- handshake, window, template ------------------------------------------------------------------

TEST(Session, HandshakeRefusesAWrongTokenBuildOrSlot) {
    net::ServerSettings settings = Settings(DrillRegion());
    settings.session.maxClients = 1;
    Session s(settings);
    ASSERT_TRUE(s.started);
    Session::Bot& good = s.AddClient();
    net::ClientSettings wrongToken;
    wrongToken.token = TestToken();
    wrongToken.token[3] ^= 1;
    Session::Bot& badToken = s.AddClient(wrongToken);
    net::ClientSettings otherBuild;
    otherBuild.buildHash = 0x1234;
    Session::Bot& badBuild = s.AddClient(otherBuild);
    for (int i = 0; i < 10; ++i) s.Tick();
    EXPECT_EQ(good.client->State(), net::ClientState::Playing);
    EXPECT_EQ(badToken.client->State(), net::ClientState::Refused);
    EXPECT_EQ(badToken.client->RefusalReason(), net::RefuseReason::Token);
    EXPECT_EQ(badBuild.client->State(), net::ClientState::Refused);
    EXPECT_EQ(badBuild.client->RefusalReason(), net::RefuseReason::Build);
    Session::Bot& full = s.AddClient();
    for (int i = 0; i < 10; ++i) s.Tick();
    EXPECT_EQ(full.client->RefusalReason(), net::RefuseReason::Full);
    EXPECT_EQ(s.server.ClientCount(), 1u);
    EXPECT_EQ(s.server.Stats().clientsRefused, 3u);
}

TEST(Session, ClientBuildsItsWorldFromWelcomeAndRefusesAnyOther) {
    sim::RegionDesc region = DrillRegion();
    region.terrain.voxelSize = 0.1f;                  // not the engine's default: the client must take it
    region.flatGroundVoxels = 120;
    Session s(Settings(region));
    ASSERT_TRUE(s.started);
    s.autoAttach = false;
    Session::Bot& bot = s.AddClient();
    for (int i = 0; i < 5 && bot.client->State() != net::ClientState::Welcomed; ++i) s.Tick();
    ASSERT_EQ(bot.client->State(), net::ClientState::Welcomed);
    EXPECT_EQ(bot.client->Region().terrain.voxelSize, 0.1f);
    EXPECT_EQ(bot.client->Stats().probeMismatches, 0u);

    // Its own defaults (0.125 m, a camera window): refused.
    sim::SimulationConfig own = bot.base;
    own.world.windowSize = {8, 8, 8};
    sim::Simulation wrong(own);
    ASSERT_TRUE(wrong.Initialize(bot.client->MakeGenerator()));
    EXPECT_FALSE(bot.client->Attach(wrong));
    // The session's world with a save directory: refused (a session world is memory only).
    sim::SimulationConfig saving = bot.client->MakeSimulationConfig(bot.base);
    saving.world.saveDirectory = std::filesystem::temp_directory_path() / "vortex_net_test_never_written";
    sim::Simulation withSaves(saving);
    ASSERT_TRUE(withSaves.Initialize(bot.client->MakeGenerator()));
    EXPECT_FALSE(bot.client->Attach(withSaves));
    // The session's world: taken, and it is the server's.
    sim::Simulation right(bot.client->MakeSimulationConfig(bot.base));
    ASSERT_TRUE(right.Initialize(bot.client->MakeGenerator()));
    ASSERT_TRUE(bot.client->Attach(right));
    EXPECT_EQ(right.World().VoxelSize(), 0.1f);
    EXPECT_EQ(right.World().WindowMinChunk(), s.server.Simulation().World().WindowMinChunk());
    EXPECT_EQ(right.World().WindowSize(), s.server.Simulation().World().WindowSize());
    EXPECT_EQ(right.World().BuildCeiling(), s.server.Simulation().World().BuildCeiling());
    EXPECT_EQ(bot.client->Hash().hash, s.server.Hash().hash);
}

TEST(Session, ServerWindowIsThePlayBoundsPlusThreeChunks) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    const voxel::World& world = s.server.Simulation().World();
    EXPECT_EQ(world.WindowMinChunk(), glm::ivec3(-4 - 3, 0, -4 - 3));
    EXPECT_EQ(world.WindowSize(), glm::ivec3(7 + 6, 8, 7 + 6));
    EXPECT_TRUE(world.FullHeight());
    EXPECT_TRUE(s.server.Hash().Complete());
    // Casts centred outside the play bounds are rejected; inside, they run.
    const glm::ivec3 inside((3 * 32 - 1) * 16 + 8, kGround * 16, 8), outside(3 * 32 * 16 + 8, kGround * 16, 8);
    EXPECT_TRUE(s.server.QueueCast(voxel::SphereQ{inside, 64}, 1600));
    EXPECT_FALSE(s.server.QueueCast(voxel::SphereQ{outside, 64}, 1600));
    EXPECT_FALSE(s.server.QueueCast(voxel::SphereQ{glm::ivec3(8, 40 * 32 * 16, 8), 64}, 1600));    // above the rows
    s.server.Tick();
    EXPECT_EQ(s.server.Stats().castsRun, 1u);
    EXPECT_EQ(s.server.Stats().castsRejected, 2u);
    EXPECT_FALSE(s.server.Failed());
}

// A cast centred inside the play bounds must keep its carve inside the window's rows and its island
// box inside the 3-chunk margin; otherwise it is rejected (counted), never run into a broken server.
TEST(Session, CastsWhoseBoxesLeaveTheWindowAreRejected) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    const int edge = (3 * 32 - 1) * 16 + 8;                 // the last voxel column of the play bounds (x)
    static_assert(sim::kMaxCastRadiusQ / voxel::kQuantaPerVoxel + sim::kIslandSearchMargin <=
                  sim::kServerWindowMarginChunks * voxel::kChunkSize);
    EXPECT_FALSE(s.server.QueueCast(voxel::SphereQ{glm::ivec3(edge, kGround * 16, 8), sim::kMaxCastRadiusQ + 16}, 1600));
    // Through the floor or the ceiling of the rows (0..32 m: voxels 0..255).
    EXPECT_FALSE(s.server.QueueCast(voxel::SphereQ{glm::ivec3(8, 2 * 16 + 8, 8), 4 * 16}, 1600));
    EXPECT_FALSE(s.server.QueueCast(voxel::SphereQ{glm::ivec3(8, 253 * 16 + 8, 8), 4 * 16}, 1600));
    EXPECT_EQ(s.server.Stats().castsRejected, 3u);
    // The largest cast at the edge, and casts that touch the floor and the ceiling rows from inside, run.
    EXPECT_TRUE(s.server.QueueCast(voxel::SphereQ{glm::ivec3(edge, kGround * 16, 8), sim::kMaxCastRadiusQ}, 1600));
    EXPECT_TRUE(s.server.QueueCast(voxel::SphereQ{glm::ivec3(8, 4 * 16 + 8, 8), 4 * 16}, 1600));
    EXPECT_TRUE(s.server.QueueCast(voxel::SphereQ{glm::ivec3(8, 251 * 16 + 8, 8), 4 * 16}, 1600));
    for (int i = 0; i < 8; ++i) s.server.Tick();
    EXPECT_EQ(s.server.Stats().castsRun, 3u);
    EXPECT_EQ(s.server.Stats().searchErrors, 0u);
    EXPECT_EQ(s.server.Stats().incompleteOps, 0u);
    EXPECT_FALSE(s.server.Failed());
}

// networking.md "Server world without a camera": a failed server ends the session. Nothing more is
// sent, joined clients are disconnected, new ones are not welcomed, nothing more is queued.
TEST(Session, AFailedServerEndsTheSession) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    Session::Bot& bot = s.AddClient();
    s.server.ApplyOp(sim::WorldOp::Box({0, kGround, 0}, {4, kGround + 4, 4}, DefaultIds().gold));
    ASSERT_TRUE(s.Drain());
    const uint32_t before = bot.client->AppliedOpSeq();
    // An admin operation outside the server's window cannot be applied in full: the session is broken.
    EXPECT_EQ(s.server.ApplyOp(sim::WorldOp::Box({4000, kGround, 0}, {4004, kGround + 4, 4}, DefaultIds().gold)), 0u);
    EXPECT_TRUE(s.server.Failed());
    EXPECT_EQ(s.server.LastOpSeq(), before);
    EXPECT_FALSE(s.server.QueueCast(voxel::SphereQ{glm::ivec3(8, kGround * 16, 8), 64}, 1600));
    EXPECT_EQ(s.server.ApplyOp(sim::WorldOp::Box({0, kGround, 0}, {1, kGround + 1, 1}, DefaultIds().iron)), 0u);
    Session::Bot& late = s.AddClient();
    for (int i = 0; i < 10; ++i) s.Tick();
    EXPECT_EQ(s.server.ClientCount(), 0u);
    EXPECT_EQ(bot.client->State(), net::ClientState::Closed);
    EXPECT_EQ(bot.client->AppliedOpSeq(), before);
    EXPECT_NE(late.client->State(), net::ClientState::Playing);
    EXPECT_NE(late.client->State(), net::ClientState::Welcomed);
}

// A client that fails (here: a corrupted batch) leaves; its slot is free for the next one.
TEST(Session, AFailedClientLeavesAndFreesItsSlot) {
    // Truncates the next event batch it receives once armed.
    struct Corrupting final : net::ITransport {
        explicit Corrupting(net::ITransport& inner) : inner(inner) {}
        void Send(net::PeerId peer, net::Channel channel, std::span<const uint8_t> data) override { inner.Send(peer, channel, data); }
        bool Poll(net::TransportEvent& out) override {
            if (!inner.Poll(out)) return false;
            if (armed && out.type == net::TransportEvent::Type::Received && net::PeekType(out.data) == net::MessageType::EventBatch) {
                out.data.pop_back();
                armed = false;
            }
            return true;
        }
        double RoundTripSeconds(net::PeerId peer) const override { return inner.RoundTripSeconds(peer); }
        size_t ReliableBytesInFlight(net::PeerId peer) const override { return inner.ReliableBytesInFlight(peer); }
        void Disconnect(net::PeerId peer) override { inner.Disconnect(peer); }
        net::ITransport& inner;
        bool armed = false;
    };
    net::ServerSettings settings = Settings(DrillRegion());
    settings.session.maxClients = 1;
    Session s(settings);
    ASSERT_TRUE(s.started);
    net::LoopbackTransport loop;
    Corrupting link(loop);
    net::ClientSettings cs;
    cs.token = TestToken();
    net::Client client(cs, link);
    net::LoopbackTransport::Connect(s.serverLoop, loop);
    sim::SimulationConfig base;
    base.jobThreads = 1;
    base.physics.threads = 1;
    base.physics.maxBodies = 256;
    base.physics.maxBodyPairs = 512;
    base.physics.maxContactConstraints = 512;
    std::unique_ptr<sim::Simulation> simulation;
    for (int i = 0; i < 5; ++i) {
        s.Tick();
        client.Poll();
        if (client.State() == net::ClientState::Welcomed && !simulation) {
            simulation = std::make_unique<sim::Simulation>(client.MakeSimulationConfig(base));
            ASSERT_TRUE(simulation->Initialize(client.MakeGenerator()));
            ASSERT_TRUE(client.Attach(*simulation));
        }
    }
    ASSERT_EQ(client.State(), net::ClientState::Playing);
    ASSERT_EQ(s.server.ClientCount(), 1u);
    link.armed = true;
    s.server.ApplyOp(sim::WorldOp::Box({0, kGround, 0}, {1, kGround + 1, 1}, DefaultIds().gold));
    for (int i = 0; i < 3; ++i) {
        s.Tick();
        client.Poll();
    }
    EXPECT_EQ(client.State(), net::ClientState::Failed);
    EXPECT_EQ(s.server.ClientCount(), 0u);
    Session::Bot& next = s.AddClient();
    for (int i = 0; i < 5; ++i) s.Tick();
    EXPECT_EQ(next.client->State(), net::ClientState::Playing);
}

// A peer that connects and never says Hello, and a refused one that never reads its refusal, are
// disconnected after a while: the server holds no peer forever.
TEST(Session, PeersThatNeverJoinAreDisconnected) {
    net::ServerSettings settings = Settings(DrillRegion());
    Session s(settings);
    ASSERT_TRUE(s.started);
    net::LoopbackTransport silent, deaf;
    const net::PeerId silentServer = net::LoopbackTransport::Connect(silent, s.serverLoop);
    (void)silentServer;
    const net::PeerId deafServer = net::LoopbackTransport::Connect(deaf, s.serverLoop);
    net::Hello wrong;
    wrong.token = TestToken();
    wrong.token[0] ^= 0xff;
    wrong.buildHash = s.server.BuildHashValue();
    deaf.Send(deafServer, net::Channel::Events, net::Encode(wrong));
    auto disconnected = [](net::LoopbackTransport& end) {
        net::TransportEvent e;
        bool seen = false;
        while (end.Poll(e)) seen = seen || e.type == net::TransportEvent::Type::Disconnected;
        return seen;
    };
    const uint32_t start = s.server.TickNumber();
    while (s.server.TickNumber() - start < settings.closeGraceTicks + 2) s.server.Tick();
    EXPECT_TRUE(disconnected(deaf));            // refused, its refusal unread: closed after the grace period
    while (s.server.TickNumber() - start < settings.helloTimeoutTicks + 2) s.server.Tick();
    EXPECT_TRUE(disconnected(silent));
    EXPECT_EQ(s.server.Stats().peersTimedOut, 2u);
}

// A playground needs ground near the origin inside its play bounds: without a site the server does
// not start (rather than build the plaza in the air or outside the play bounds).
TEST(Session, APlaygroundWithoutASiteDoesNotStart) {
    sim::RegionDesc region = DrillRegion(sim::RegionTemplateId::Playground);
    region.heightRange = {-16.0f, 32.0f};
    region.flatGroundVoxels = 4;                             // 0.5 m: every candidate is "under water"
    Session s(Settings(region));
    EXPECT_FALSE(s.started);
}

TEST(Session, TemplateRunsOnTheServerAndReachesClientsAsEvents) {
    Session s(Settings(DrillRegion(sim::RegionTemplateId::Playground)));
    ASSERT_TRUE(s.started);
    const sim::RegionTemplate& t = s.server.Template();
    ASSERT_GT(t.ops.size(), 40u);
    EXPECT_EQ(t.lights.size(), 6u);
    EXPECT_EQ(t.objects.size(), 6u);
    // Operations 1..k are the template's, then one EntitySpawn per light.
    EXPECT_EQ(s.server.LastOpSeq(), t.ops.size() + t.lights.size());
    EXPECT_EQ(s.server.Stats().templateOps, t.ops.size());
    EXPECT_EQ(s.server.Simulation().Scene().VoxelObjectCount(), 6u);    // crates: server bodies (replicated from stage 6)

    Session::Bot& bot = s.AddClient();
    ASSERT_TRUE(s.Drain());
    EXPECT_EQ(bot.client->Hash().hash, s.server.Hash().hash);
    EXPECT_EQ(bot.client->Stats().lightsSpawned, 6u);
    EXPECT_EQ(bot.client->Spawn(), t.spawn);
    EXPECT_EQ(bot.simulation->Scene().VoxelObjectCount(), 0u);
    // The same template applied offline (the game's path) gives the same voxels.
    sim::SimulationConfig offline = bot.client->MakeSimulationConfig(bot.base);
    sim::Simulation game(offline);
    ASSERT_TRUE(game.Initialize(bot.client->MakeGenerator()));
    game.World().LoadBlocking(sim::RegionWindowFocus(s.server.Region()));
    const sim::RegionTemplate local = sim::BuildRegionTemplate(game, s.server.Region());
    EXPECT_EQ(local.ops, t.ops);
    sim::ApplyTemplateOps(game, local);
    EXPECT_EQ(net::RegionHash(game.World(), s.server.Region()).hash, s.server.Hash().hash);
}

// networking.md "Time and ticks": the tick is never dropped; after a hitch the server catches up at
// most 2 ticks per loop pass.
TEST(Session, AdvanceCatchesUpTwoTicksAPassAndDropsNone) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    const uint32_t start = s.server.TickNumber();
    const double dt = s.server.Simulation().FixedTimeStep();
    EXPECT_EQ(s.server.Advance(dt / 2), 0u);                 // half a tick
    EXPECT_EQ(s.server.Advance(dt / 2), 1u);
    EXPECT_EQ(s.server.Advance(6 * dt), 2u);                 // a 100 ms hitch: 6 ticks due
    uint32_t passes = 0;
    while (s.server.Advance(0.0) > 0) ++passes;
    EXPECT_EQ(passes, 2u);                                   // the other 4, two per pass
    EXPECT_EQ(s.server.TickNumber() - start, 7u);
}

// --- the cast budget -------------------------------------------------------------------------------

namespace {
    struct CastLine {
        uint32_t tick, firstOpSeq, opCount;
        uint64_t work;
        bool operator==(const CastLine&) const = default;
    };

    /// A server fed the drill in bursts (four steps at once every 24 ticks), so the budgets bite.
    std::vector<CastLine> RunBursts(unsigned jobThreads, bool coldGraphs, uint64_t& hash, net::ServerStats& stats) {
        Session s(Settings(DrillRegion(), jobThreads));
        EXPECT_TRUE(s.started);
        // A client follows the interleaved stream (build operations of later steps made while
        // earlier casts still wait in the queue, casts run late or overdue).
        Session::Bot& bot = s.AddClient();
        const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
        uint32_t step = 0;
        for (uint32_t t = 0; t < 24 * 6; ++t) {
            if (t % 24 == 0)
                for (int k = 0; k < 4; ++k, ++step) {
                    const net::DrillStep d = drill.Step(step);
                    for (const sim::WorldOp& op : d.build) s.server.ApplyOp(op);
                    s.server.QueueCast(d.cast, d.power);
                }
            if (coldGraphs) s.server.Simulation().TerrainGraphs().Clear();
            s.Tick();
        }
        EXPECT_TRUE(s.Drain());
        EXPECT_FALSE(s.server.Failed());
        EXPECT_EQ(bot.client->State(), net::ClientState::Playing);
        EXPECT_EQ(bot.client->Hash().hash, s.server.Hash().hash);
        hash = s.server.Hash().hash;
        stats = s.server.Stats();
        std::vector<CastLine> out;
        for (const net::CastRecord& c : s.server.Casts()) out.push_back({c.tick, c.firstOpSeq, c.opCount, c.work.Total()});
        return out;
    }
}

// R5: the tick a cast runs on follows from counts only. Two servers with 1 and 3 workers, one of
// them with its graph cache emptied before every tick (every graph built cold, graphsBuilt
// differs), run the same casts on the same ticks with the same work.
TEST(CastBudget, ScheduleFollowsCountsNotTimeOrCache) {
    uint64_t hashA = 0, hashB = 0;
    net::ServerStats a, b;
    const std::vector<CastLine> warm = RunBursts(1, false, hashA, a);
    const std::vector<CastLine> cold = RunBursts(3, true, hashB, b);
    ASSERT_EQ(warm.size(), 24u);
    EXPECT_EQ(warm, cold);
    EXPECT_EQ(hashA, hashB);
    // The budgets did bite: casts of a burst were spread over ticks, none waited past the limit.
    EXPECT_GT(a.ticksWaitedForWork + a.ticksWaitedForBytes, 0u);
    EXPECT_GT(a.castDelay[0], 0u);
    EXPECT_LT(a.castDelay[0], 24u);
    EXPECT_EQ(a.castDelay[7], 0u);
    EXPECT_LE(a.maxCastDelay, 6u);
}

// The work of a cast is the same whether its graphs were cached or built (networking.md: charge
// chunkGraphs at the cold price, never graphsBuilt).
TEST(CastBudget, CastWorkIsEqualColdAndWarm) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
    const net::DrillStep step = drill.Step(0);      // the mushroom: an island, a crumb, the load check judging the stem
    for (const sim::WorldOp& op : step.build) s.server.ApplyOp(op);
    sim::Simulation& simulation = s.server.Simulation();
    // The same site on a second world whose graph cache a dud next to the mushroom has warmed
    // (nothing there is overloaded, so the dud changes nothing), against the server's emptied cache.
    sim::SimulationConfig config = sim::ServerConfig(Settings(DrillRegion()).simulation);
    config.world = sim::RegionWorldConfig(s.server.Region(), config.world);
    config.chunkCapacity = 4096;
    sim::Simulation other(config);
    ASSERT_TRUE(other.Initialize(sim::MakeRegionGenerator(s.server.Region(), DefaultIds())));
    other.World().LoadBlocking(sim::RegionWindowFocus(s.server.Region()));
    for (const sim::WorldOp& op : step.build) sim::ApplyOp(other.World(), op);
    const sim::BlastResult dud = sim::Blast(other, step.cast, 0);
    ASSERT_EQ(dud.ops.size(), 1u);
    ASSERT_EQ(dud.ops[0].result.changed, 0u);
    const sim::BlastResult warm = sim::Blast(other, step.cast, step.power);
    simulation.TerrainGraphs().Clear();
    const sim::BlastResult cold = sim::Blast(simulation, step.cast, step.power);
    EXPECT_GT(cold.search.graphsBuilt, warm.search.graphsBuilt);
    EXPECT_EQ(cold.search.chunkGraphs, warm.search.chunkGraphs);
    EXPECT_EQ(sim::CastWorkOf(cold).Total(), sim::CastWorkOf(warm).Total());
    EXPECT_GT(cold.IslandCount(), 0u);
    EXPECT_TRUE(cold.search.neckRan);
    EXPECT_GT(sim::CastWorkOf(cold).neck, 0u);
    EXPECT_EQ(net::RegionHash(other.World(), s.server.Region()).hash, s.server.Hash().hash);
}

// --- pacing ---------------------------------------------------------------------------------------------

namespace {
    struct JoinRun {
        uint32_t maxLiveLatency = 0;      ///< Ticks between an event being made and client A applying it, after B connected.
        uint32_t joinTicks = 0;           ///< Ticks until B had applied its whole join log.
        uint64_t logBytes = 0;
        bool equal = false;
        net::ServerStats stats;
    };

    /// Client A plays from the start; B joins after 30 drill blasts. The server's uplink is 48 kB/s.
    JoinRun RunLateJoin(uint32_t maxEventBytesInFlight) {
        net::Impairment im;
        im.delaySeconds = 0.02;
        im.bytesPerSecond = 48.0 * 1024.0;
        net::ServerSettings settings = Settings(DrillRegion());
        settings.maxEventBytesInFlight = maxEventBytesInFlight;
        Session s(settings, im);
        EXPECT_TRUE(s.started);
        Session::Bot& a = s.AddClient();
        Session::Bot& b = s.AddClient({}, false);
        const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
        JoinRun run;
        std::vector<uint32_t> madeAt;    // [opSeq - 1] -> tick
        uint32_t seenA = 0, joinTick = 0;
        bool joined = false;
        for (uint32_t step = 0; step < 48; ++step) {
            if (step == 30) {
                s.Connect(b);
                joined = true;
                joinTick = s.server.TickNumber();
                run.logBytes = s.server.LogBytes();
            }
            const net::DrillStep d = drill.Step(step);
            for (const sim::WorldOp& op : d.build) s.server.ApplyOp(op);
            EXPECT_TRUE(s.server.QueueCast(d.cast, d.power));
            for (uint32_t t = 0; t < 12; ++t) {
                s.Tick();
                const uint32_t tick = s.server.TickNumber();
                while (madeAt.size() < s.server.LastOpSeq()) madeAt.push_back(tick);
                for (; seenA < a.client->AppliedOpSeq(); ++seenA)
                    if (joined) run.maxLiveLatency = std::max(run.maxLiveLatency, tick - madeAt[seenA]);
                if (joined && run.joinTicks == 0 && b.simulation && b.client->AppliedOpSeq() >= b.client->LogEnd())
                    run.joinTicks = tick - joinTick;
            }
        }
        EXPECT_TRUE(s.Drain());
        run.equal = b.client->State() == net::ClientState::Playing && a.client->Hash().hash == s.server.Hash().hash &&
                    b.client->Hash().hash == s.server.Hash().hash;
        run.stats = s.server.Stats();
        return run;
    }
}

// networking.md "Transport": everything that is sent has a byte budget. A joiner's catch-up is
// handed to the transport only while less than maxEventBytesInFlight is on its way to that peer,
// so on a shared uplink the live stream of the others is not stuck behind the whole log.
TEST(Session, ALateJoinIsPacedAndLiveEventsKeepFlowing) {
    const JoinRun paced = RunLateJoin(16384);
    const JoinRun unpaced = RunLateJoin(0);
    std::printf("[ pacing ] log at join %.1f KiB; live latency max %u ticks paced / %u unpaced; join %u / %u ticks; %u sends held\n",
                static_cast<double>(paced.logBytes) / 1024.0, paced.maxLiveLatency, unpaced.maxLiveLatency, paced.joinTicks,
                unpaced.joinTicks, paced.stats.sendsHeld);
    EXPECT_TRUE(paced.equal);
    EXPECT_TRUE(unpaced.equal);
    EXPECT_GT(paced.logBytes, 48u * 1024u);                  // more than a second of the uplink
    EXPECT_GT(paced.stats.sendsHeld, 0u);
    EXPECT_EQ(unpaced.stats.sendsHeld, 0u);
    EXPECT_LT(paced.maxLiveLatency * 2, unpaced.maxLiveLatency);
    EXPECT_GT(paced.joinTicks, 0u);
}

// --- the acceptance ----------------------------------------------------------------------------------

// networking.md stage 3: "In-process server + 2 clients, 200 server-side blasts that free islands,
// 5 % loss: hashes equal". Every link drops 5 % of its packets (reliable ones are resent: latency),
// with 40 + 0-30 ms of delay, duplicates and reordering on the state channel, and a 200 ms outage
// on every link. Client A joins at once and shows what it applies (particles, rubble); client B
// joins after 100 blasts and receives the whole event log first.
TEST(SessionAcceptance, TwoClientsAgreeAfter200BlastsAt5PercentLoss) {
    net::Impairment im = Lossy();
    im.outages.push_back({10.0, 10.2});
    Session s(Settings(DrillRegion()), im);
    ASSERT_TRUE(s.started);
    net::ClientSettings presenting;
    presenting.presentation = true;
    Session::Bot& a = s.AddClient(presenting);
    Session::Bot& b = s.AddClient({}, false);
    const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
    ASSERT_EQ(drill.Sites(), 4u);

    constexpr uint32_t kBlasts = 200;
    for (uint32_t step = 0; step < kBlasts; ++step) {
        if (step == 100) s.Connect(b);
        const net::DrillStep d = drill.Step(step);
        for (const sim::WorldOp& op : d.build) s.server.ApplyOp(op);
        ASSERT_TRUE(s.server.QueueCast(d.cast, d.power));
        for (uint32_t t = 0; t < kStepTicks; ++t) s.Tick();
        ASSERT_FALSE(s.server.Failed());
    }
    ASSERT_TRUE(s.Drain());

    // Every blast freed terrain the way its structure was built for.
    const std::deque<net::CastRecord>& casts = s.server.Casts();
    ASSERT_EQ(casts.size(), kBlasts);
    uint32_t inBox = 0, face = 0, neck = 0, crumbs = 0;
    for (uint32_t i = 0; i < kBlasts; ++i) {
        const net::CastRecord& c = casts[i];
        EXPECT_EQ(c.searchErrors, 0u) << "cast " << i;
        switch (drill.Step(i).kind) {
            case net::DrillKind::Mushroom:
                EXPECT_GE(c.inBoxIslands, 1u) << "cast " << i;
                EXPECT_GE(c.crumbs, 1u) << "cast " << i;
                break;
            case net::DrillKind::Bar: EXPECT_GE(c.facePieces, 1u) << "cast " << i; break;
            case net::DrillKind::NeckTower: EXPECT_GE(c.neckPieces, 1u) << "cast " << i; break;
        }
        inBox += c.inBoxIslands;
        face += c.facePieces;
        neck += c.neckPieces;
        crumbs += c.crumbs;
    }
    EXPECT_GE(inBox, 66u);
    EXPECT_GE(face, 66u);
    EXPECT_GE(neck, 66u);
    EXPECT_GE(crumbs, 66u);

    // Equal worlds.
    const voxel::WorldHashResult server = s.server.Hash();
    ASSERT_TRUE(server.Complete());
    for (Session::Bot* bot : {&a, &b}) {
        ASSERT_EQ(bot->client->State(), net::ClientState::Playing);
        EXPECT_EQ(bot->client->AppliedOpSeq(), s.server.LastOpSeq());
        const voxel::WorldHashResult h = bot->client->Hash();
        EXPECT_TRUE(h.Complete());
        EXPECT_EQ(h.hash, server.hash);
        EXPECT_EQ(h.solidChunks, server.solidChunks);
        EXPECT_EQ(bot->client->Stats().skippedChunks, 0u);
        EXPECT_EQ(bot->client->Stats().unsyncedChunks, 0u);
        EXPECT_EQ(bot->client->Stats().probeMismatches, 0u);
    }
    EXPECT_GT(b.client->LogEnd(), s.server.LastOpSeq() / 3);     // B started from the log
    // The network really was bad: resends and lost / duplicated status packets on the way down.
    EXPECT_GT(s.serverLink.Stats().resent, 20u);
    EXPECT_GT(s.serverLink.Stats().lost, 5u);
    EXPECT_GT(a.client->Stats().statusReceived, 0u);
    EXPECT_GT(b.client->Stats().statusReceived, 0u);
    // Reliable events are never duplicated by this transport or this server (Client.* tests
    // feed overlapping batches and gaps by hand).
    EXPECT_EQ(a.client->Stats().duplicateEvents, 0u);
    EXPECT_EQ(b.client->Stats().duplicateEvents, 0u);
    // Client A made rubble for what fell, within the rubble caps; the server made none.
    EXPECT_EQ(s.server.Simulation().Scene().DebrisCount(), 0u);
    const scene::RubbleStats rubble = a.simulation->Scene().Rubble();
    EXPECT_LE(rubble.bodies, a.simulation->Scene().Config().maxRubbleBodies);
    // And the stream stayed inside its budgets but for the largest pieces.
    const net::ServerStats& st = s.server.Stats();
    EXPECT_EQ(st.castDelay[7], 0u);
    EXPECT_EQ(st.searchErrors, 0u);
    EXPECT_EQ(st.incompleteOps, 0u);
    EXPECT_GT(st.castSeconds, 0.0);                       // logged, never read by the schedule
    EXPECT_GE(st.maxCastMsPer10k, st.castSeconds * 1000.0 * 10000.0 / static_cast<double>(st.castWork));
    std::printf("[ acceptance ] %u events, %.1f KiB, %u over the burst; casts delayed 0..6 ticks: %u %u %u %u %u %u %u; %u past the "
                "budgets; work %llu max per cast; %u in-box islands, %u face-flood islands, %u neck releases, %u crumbs; resent %llu, "
                "lost %llu\n",
                static_cast<unsigned>(st.events), static_cast<double>(st.eventBytes) / 1024.0, st.oversizeEvents, st.castDelay[0],
                st.castDelay[1], st.castDelay[2], st.castDelay[3], st.castDelay[4], st.castDelay[5], st.castDelay[6], st.castsOverdue,
                static_cast<unsigned long long>(st.maxCastWork), inBox, face, neck, crumbs,
                static_cast<unsigned long long>(s.serverLink.Stats().resent), static_cast<unsigned long long>(s.serverLink.Stats().lost));
}

// A stream with one event changed is caught: the hashes are not equal by construction.
TEST(SessionAcceptance, AStreamWithOneEventChangedGivesAnotherHash) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    Session::Bot& bot = s.AddClient();
    const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
    for (uint32_t step = 0; step < 6; ++step) {
        const net::DrillStep d = drill.Step(step);
        for (const sim::WorldOp& op : d.build) s.server.ApplyOp(op);
        s.server.QueueCast(d.cast, d.power);
        for (uint32_t t = 0; t < kStepTicks; ++t) s.Tick();
    }
    ASSERT_TRUE(s.Drain());
    ASSERT_EQ(bot.client->Hash().hash, s.server.Hash().hash);
    // The server alone takes one more 1-voxel edit: the client no longer agrees.
    sim::ApplyOp(s.server.Simulation().World(), sim::WorldOp::Box({0, kGround, 0}, {1, kGround + 1, 1}, DefaultIds().gold));
    EXPECT_NE(bot.client->Hash().hash, s.server.Hash().hash);
}

// Calibration of the cast work units (run by hand: --gtest_also_run_disabled_tests). Casts on the
// built-in terrain at 0.125 m (random surface points, 0.6-2 m, power 6; every other one with an
// emptied graph cache) and the drill's three structures on flat ground; prints per cast the work
// by part and the wall time of the simulation half (carve + search + operations) as CSV lines
// "calib,kind,cold,search,neck,ops,ms,searchMs,neckMs,buildMs,chunkGraphs,nodeVisits,opCount".
TEST(CastBudget, DISABLED_CalibrateWorkAgainstTime) {
    auto report = [](const char* kind, bool cold, const sim::BlastResult& r) {
        const sim::CastWork w = sim::CastWorkOf(r);
        const double ms = r.times.carveMs + r.times.searchMs + r.times.opsMs;
        std::printf("calib,%s,%d,%llu,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%u,%u,%u\n", kind, cold ? 1 : 0, static_cast<unsigned long long>(w.search),
                    static_cast<unsigned long long>(w.neck), static_cast<unsigned long long>(w.ops), ms, r.times.searchMs, r.times.neckMs,
                    r.times.graphBuildMs, r.search.chunkGraphs, r.search.nodeVisits, static_cast<unsigned>(r.ops.size()));
    };
    {
        sim::RegionDesc region = DrillRegion();
        region.generator = sim::RegionGenerator::Terrain;
        region.heightRange = {-48.0f, 128.0f};
        region.playMinChunk = {-12, -12};
        region.playMaxChunk = {12, 12};
        net::ServerSettings settings = Settings(region, 3);
        settings.session.packHidden = true;
        Session s(settings);
        ASSERT_TRUE(s.started);
        sim::Simulation& simulation = s.server.Simulation();
        voxel::TerrainSettings terrain = s.server.Region().terrain;
        Rng rng(5);
        for (int i = 0; i < 240; ++i) {
            const float x = rng.Range(-44.0f, 44.0f), z = rng.Range(-44.0f, 44.0f);
            const float y = voxel::TerrainHeight(terrain, x, z) - 0.3f;
            const bool cold = (i & 1) != 0;
            if (cold) simulation.TerrainGraphs().Clear();
            const sim::BlastResult r = sim::Blast(simulation, sim::BlastDesc{glm::dvec3(x, y, z), rng.Range(0.6f, 2.0f), 6.0f});
            report("terrain", cold, r);
        }
    }
    {
        Session s(Settings(DrillRegion(), 3));
        ASSERT_TRUE(s.started);
        const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
        for (uint32_t i = 0; i < 60; ++i) {
            const net::DrillStep d = drill.Step(i);
            for (const sim::WorldOp& op : d.build) s.server.ApplyOp(op);
            const bool cold = (i / 3) % 2 != 0;
            if (cold) s.server.Simulation().TerrainGraphs().Clear();
            report(net::ToString(d.kind), cold, sim::Blast(s.server.Simulation(), d.cast, d.power));
        }
    }
}
