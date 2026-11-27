// docs/design/networking.md stage 3: a session in one process. Session files, the build hash and
// probe chunks, the handshake, the server window, the template split, the cast budget, and the
// acceptance: an in-process server and two clients over a lossy loopback agree on the world after
// 200 server-side blasts that free terrain (in-box islands with crumbs, face-flood islands and
// weak-neck releases), one client joining late from the full event log.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
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
import vortex.physics;
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
            bool frozen = false;          ///< connected but never polled (a client that stopped reading)
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
            // Reproducible runs (stage 6 rule): collider cooks finish inside the tick, not on worker timing.
            if (waitIdle) server.Simulation().Jobs().WaitIdle();
            clock += 1.0 / 60.0;
            for (const std::unique_ptr<Bot>& b : bots) {
                Bot& bot = *b;
                if (!bot.connected || bot.frozen) continue;
                bot.client->Poll();
                if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation && autoAttach) {
                    bot.simulation = std::make_unique<sim::Simulation>(bot.client->MakeSimulationConfig(bot.base));
                    ASSERT_TRUE(bot.simulation->Initialize(bot.client->MakeGenerator()));
                    ASSERT_TRUE(bot.client->Attach(*bot.simulation));
                }
                if (bot.simulation) {
                    for (const sim::BlastResult& blast : bot.client->TakePresentations()) sim::PresentBlast(*bot.simulation, blast);
                    bot.simulation->Step({});
                    if (waitIdle) bot.simulation->Jobs().WaitIdle();
                }
            }
        }

        /// Ticks until every attached client has applied the server's last event and is synced (its
        /// chunks current, its baseline in; stage 7a), and the cast queue is empty.
        bool Drain(uint32_t maxTicks = 900) {
            for (uint32_t i = 0; i < maxTicks; ++i) {
                bool done = server.QueuedCasts() == 0;
                for (const std::unique_ptr<Bot>& bot : bots)
                    done = done && (!bot->connected || bot->frozen ||
                                    (bot->simulation && bot->client->AppliedOpSeq() == server.LastOpSeq() && bot->client->Synced()));
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
        bool waitIdle = false;
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
    config.mechanisms = true;
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
    EXPECT_TRUE(back.mechanisms);

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
    for (const char* name : {"drill", "arena250", "playground", "duel", "felling", "fight10"}) {
        const std::filesystem::path path = std::filesystem::path(VORTEX_TEST_SOURCE_DIR) / "tools/server/sessions" / (std::string(name) + ".session");
        EXPECT_TRUE(net::LoadSessionFile(path, back, error)) << error;
    }
}

// [wp-7] fight.md "Rules": the fight keys format and parse back; a sandbox is SandboxRules() whatever the keys say;
// every refusal names its key.
TEST(SessionFile, FightKeysRoundTrip) {
    net::SessionConfig config;
    config.token = TestToken();
    config.region = DrillRegion();
    sim::FightRules& f = config.fight;
    f = sim::FightRules{};
    f.mode = sim::GameMode::Felling;
    f.teamSpawns = {glm::vec2(-10.5f, 2.25f), glm::vec2(7.5f, -2.25f)};
    f.towers = {glm::vec2(-12.0f, 0.0f), glm::vec2(8.0f, 0.5f)};
    f.scoreLimit = 7;
    f.roundTicks = 300 * 60;
    f.holdTicks = 45 * 60;
    f.respawnTicks = 150;
    f.chargeTicks = 60;
    f.maxCharges = 4;
    f.minPlayers = 4;
    f.countdownTicks = 120;
    f.endTicks = 90;
    f.endSession = false;
    const std::string text = net::FormatSession(config);
    net::SessionConfig back;
    std::string error;
    ASSERT_TRUE(net::ParseSession(text, back, error)) << error << "\n" << text;
    EXPECT_TRUE(back.fight == config.fight) << text;
    // A sandbox session: the sandbox's rules whatever the other keys say; no fight key is written.
    net::SessionConfig sandbox = config;
    sandbox.fight = sim::SandboxRules();
    const std::string plain = net::FormatSession(sandbox);
    EXPECT_EQ(plain.find("mode"), std::string::npos);
    ASSERT_TRUE(net::ParseSession(plain + "score_limit = 3\nmax_charges = 5\n", back, error)) << error;
    EXPECT_TRUE(back.fight == sim::SandboxRules());
    // The refusals name their key.
    const std::string base = net::FormatSession(sandbox);
    const auto refused = [&](const std::string& extra, const char* key) {
        net::SessionConfig out;
        std::string why;
        EXPECT_FALSE(net::ParseSession(base + extra, out, why)) << extra;
        EXPECT_NE(why.find(key), std::string::npos) << extra << " -> " << why;
    };
    const std::string dm = "mode = deathmatch\n";
    refused(dm + "team_spawn_1 = 0 0\n", "team_spawn_2");
    refused(dm + "team_spawn_2 = 0 0\n", "team_spawn_1");
    refused(dm + "team_spawn_1 = 0 0\nteam_spawn_2 = 1000 0\n", "team_spawn_2");
    refused("mode = felling\nteam_spawn_1 = -5 0\nteam_spawn_2 = 5 0\ntower_1 = -8 0\n", "tower_2");
    refused(dm + "team_spawn_1 = -5 0\nteam_spawn_2 = 5 0\ntower_1 = 1000 0\n", "tower_1");
    refused(dm + "max_charges = 10\n", "max_charges");
    refused(dm + "team_spawn_1 = -5 0\nteam_spawn_2 = 5 0\ncharge_ticks = 0\n", "charge_ticks");
    refused(dm + "round_seconds = 0\n", "round_seconds");
    refused(dm + "round_seconds = 3601\n", "round_seconds");
    refused("mode = brawl\n", "mode");
    // Siege: the sanctum is Hollowkeep's; the hold is 1..600 s.
    refused("mode = siege\nteam_spawn_1 = -5 0\nteam_spawn_2 = 5 0\n", "template = hollowkeep");
    refused(dm + "hold_seconds = 0\n", "hold_seconds");
    refused(dm + "hold_seconds = 601\n", "hold_seconds");
    // Review of [wp-7]: a fight's players all fit the Roster (64); a sandbox keeps 255.
    const std::string duel = dm + "team_spawn_1 = -5 0\nteam_spawn_2 = 5 0\n";
    refused(duel + "max_clients = 65\n", "max_clients");
    ASSERT_TRUE(net::ParseSession(base + duel + "max_clients = 64\n", back, error)) << error;
    EXPECT_EQ(back.maxClients, net::kMaxRosterPlayers);
    ASSERT_TRUE(net::ParseSession(base + "max_clients = 255\n", back, error)) << error;
    EXPECT_EQ(back.maxClients, 255u);
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
    bad.templateId = sim::RegionTemplateId::Hollowkeep;   // Hollowkeep stands on the world map
    EXPECT_FALSE(sim::IsValid(bad, &why));
    EXPECT_EQ(why, "the hollowkeep template needs the land generator");
    bad.generator = sim::RegionGenerator::Land;
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

// WP1: the movement rule is part of the build hash, so a client and a server that would move a
// player differently (one with the play-area clamp, one without) refuse each other ("another build")
// instead of correcting each other at the edge. Revision 0 (a build before the revision existed)
// mixes nothing.
TEST(SessionFile, BuildHashSeesThePlayerRuleRevision) {
    voxel::MaterialLibrary materials;
    voxel::RegisterDefaultWorldMaterials(materials);
    const uint64_t table = voxel::MaterialTableHash(materials);
    const uint64_t spells = sim::SpellTableHash(sim::BuildSpellTable(voxel::kDefaultVoxelSize));
    const uint64_t turning = voxel::TurningTableHash(voxel::WorldTurningTable());
    EXPECT_EQ(net::BuildHash(materials), net::BuildHashOf(net::kProtocolVersion, voxel::kGeneratorGoldenHash, table, sim::kPlayerRuleRevision,
                                                          voxel::kLandGoldenHash, spells, turning));
    EXPECT_GE(sim::kPlayerRuleRevision, 1u);
    const uint64_t before = net::BuildHashOf(net::kProtocolVersion, voxel::kGeneratorGoldenHash, table, 0, voxel::kLandGoldenHash, spells, turning);
    EXPECT_NE(net::BuildHash(materials), before);
    EXPECT_NE(net::BuildHashOf(net::kProtocolVersion, voxel::kGeneratorGoldenHash, table, 2),
              net::BuildHashOf(net::kProtocolVersion, voxel::kGeneratorGoldenHash, table, 1));
}

// land.md [land-5]: the land code's golden hashes are part of the build hash, so builds that bake or sample the world
// map differently refuse each other at the Hello ("another build") before any map is compared.
TEST(SessionFile, BuildHashSeesTheLandGoldenHash) {
    voxel::MaterialLibrary materials;
    voxel::RegisterDefaultWorldMaterials(materials);
    const uint64_t table = voxel::MaterialTableHash(materials);
    const uint64_t r = sim::kPlayerRuleRevision;
    EXPECT_NE(net::BuildHash(materials), net::BuildHashOf(net::kProtocolVersion, voxel::kGeneratorGoldenHash, table, r));
    EXPECT_NE(net::BuildHashOf(net::kProtocolVersion, voxel::kGeneratorGoldenHash, table, r, voxel::kLandGoldenHash ^ 1),
              net::BuildHash(materials));
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

// Review of stage 4: the tick length, gravity and the characters' collision set decide every
// predicted step, so they are the session's: a client takes the server's (MakeSimulationConfig)
// and refuses a simulation with another tick length or gravity; server and client characters
// collide with the same layers (terrain only until stage 6).
TEST(Session, ClientTakesTheServersTickGravityAndCharacterLayers) {
    net::ServerSettings settings = Settings(DrillRegion());
    settings.simulation.fixedTimeStep = 1.0f / 30.0f;
    settings.simulation.physics.gravity = glm::vec3(0.0f, -12.5f, 0.0f);
    Session s(settings);
    ASSERT_TRUE(s.started);
    s.autoAttach = false;
    Session::Bot& bot = s.AddClient();
    for (int i = 0; i < 5 && bot.client->State() != net::ClientState::Welcomed; ++i) s.Tick();
    ASSERT_EQ(bot.client->State(), net::ClientState::Welcomed);
    const sim::SimulationConfig& server = s.server.Simulation().Config();
    EXPECT_EQ(server.physics.characterLayers, physics::LayerBit(physics::Layer::Static) | physics::LayerBit(physics::Layer::Dynamic));
    const sim::SimulationConfig made = bot.client->MakeSimulationConfig(bot.base);   // its own: 1/60 s, -9.81
    EXPECT_EQ(std::bit_cast<uint32_t>(made.fixedTimeStep), std::bit_cast<uint32_t>(server.fixedTimeStep));
    EXPECT_EQ(made.physics.gravity, server.physics.gravity);
    EXPECT_EQ(made.physics.characterLayers, server.physics.characterLayers);

    sim::SimulationConfig ownTick = made;
    ownTick.fixedTimeStep = 1.0f / 60.0f;
    sim::Simulation wrongTick(ownTick);
    ASSERT_TRUE(wrongTick.Initialize(bot.client->MakeGenerator()));
    EXPECT_FALSE(bot.client->Attach(wrongTick));
    sim::SimulationConfig ownGravity = made;
    ownGravity.physics.gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    sim::Simulation wrongGravity(ownGravity);
    ASSERT_TRUE(wrongGravity.Initialize(bot.client->MakeGenerator()));
    EXPECT_FALSE(bot.client->Attach(wrongGravity));
    sim::Simulation right(made);
    ASSERT_TRUE(right.Initialize(bot.client->MakeGenerator()));
    EXPECT_TRUE(bot.client->Attach(right));
    EXPECT_EQ(right.FixedTimeStep(), 1.0f / 30.0f);
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

// Stage 7a: the template runs on the server; a client that joins afterwards receives none of its
// events (it starts at its join point): the plaza's chunks come as ChunkData, the lights and crates
// as its entity baseline (the crates resting, exactly at their rest poses).
TEST(Session, ATemplateReachesAJoinerAsChunkDataAndBaseline) {
    Session s(Settings(DrillRegion(sim::RegionTemplateId::Playground)));
    ASSERT_TRUE(s.started);
    const sim::RegionTemplate& t = s.server.Template();
    ASSERT_GT(t.ops.size(), 40u);
    EXPECT_EQ(t.lights.size(), 6u);
    EXPECT_EQ(t.objects.size(), 6u);
    // Operations 1..k are the template's, then one EntitySpawn per light and one per crate (stage 6).
    EXPECT_EQ(s.server.LastOpSeq(), t.ops.size() + t.lights.size() + t.objects.size());
    EXPECT_EQ(s.server.Stats().templateOps, t.ops.size());
    EXPECT_EQ(s.server.Simulation().Scene().VoxelObjectCount(), 6u);    // crates: replicated server bodies
    EXPECT_EQ(s.server.Stats().objectSpawns, 6u);

    Session::Bot& bot = s.AddClient();
    ASSERT_TRUE(s.Drain());
    EXPECT_EQ(bot.client->JoinOpSeq(), t.ops.size() + t.lights.size() + t.objects.size());
    EXPECT_EQ(bot.client->Stats().eventsApplied, 0u) << "an event at or before the join point was sent";
    EXPECT_GT(bot.client->Stats().chunkData, 0u);
    EXPECT_EQ(bot.client->Stats().chunkData, s.server.ModifiedChunks());
    EXPECT_EQ(bot.client->Stats().baselineEntities, 12u);
    EXPECT_EQ(bot.client->Hash().hash, s.server.Hash().hash);
    EXPECT_TRUE(bot.client->Hash().Complete());
    EXPECT_EQ(bot.client->Stats().lightsSpawned, 6u);
    EXPECT_EQ(bot.client->Spawn(), t.spawn);
    // The crates are kinematic copies of the server's (stage 6): the same NetIds and models.
    EXPECT_EQ(bot.simulation->Scene().VoxelObjectCount(), 6u);
    const net::EntityComparison entities = net::CompareEntities(s.server.Entities(), bot.client->Entities());
    EXPECT_TRUE(entities.Agree()) << entities.Describe();
    EXPECT_EQ(entities.server, 6u);
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

// Review of [wp-7]: practice calls Advance once per rendered frame. With up to 8 ticks a call and a backlog bound, 40 ms
// frames (25 fps) keep the server on real time, and a long hitch is dropped rather than run fast later.
TEST(Session, AdvanceOncePerSlowFrameKeepsRealTime) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    const uint32_t start = s.server.TickNumber();
    const double dt = s.server.Simulation().FixedTimeStep();
    for (int frame = 0; frame < 50; ++frame) s.server.Advance(0.040, 8, 8);   // 2 s of 25 fps
    const double due = 2.0 / dt;
    EXPECT_NEAR(static_cast<double>(s.server.TickNumber() - start), due, 1.0);
    // A 1 s hitch: 8 ticks now, at most 8 kept for the next frame, the rest dropped.
    const uint32_t before = s.server.TickNumber();
    EXPECT_EQ(s.server.Advance(1.0, 8, 8), 8u);
    while (s.server.Advance(0.0, 8, 8) > 0) {
    }
    EXPECT_GE(s.server.TickNumber() - before, 15u);           // 8 now and the 8 kept (rounding may leave the last)
    EXPECT_LE(s.server.TickNumber() - before, 16u);
    // The default (2 a call, nothing dropped) is unchanged: 40 ms frames fall behind.
    const uint32_t slow = s.server.TickNumber();
    for (int frame = 0; frame < 50; ++frame) s.server.Advance(0.040);
    EXPECT_EQ(s.server.TickNumber() - slow, 100u);
}

// --- the cast budget -------------------------------------------------------------------------------

namespace {
    /// A cast as the schedule placed it: its tick, its terrain operations and its work. Not its
    /// opSeq: entity events (stage 6: where pieces rest, which crate a blast catches) follow from
    /// Jolt and collider timing and are numbered in the same stream.
    struct CastLine {
        uint32_t tick, opCount;
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
                    net::RunDrillStep(s.server, drill.Step(step));
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
        for (const net::CastRecord& c : s.server.Casts()) out.push_back({c.tick, c.opCount, c.work.Total()});
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
        uint32_t maxLiveLatency = 0;      ///< Ticks between an event being made and client A applying it, while B joins.
        uint32_t joinTicks = 0;           ///< Ticks until B was synced.
        size_t modified = 0;              ///< Chunks the server's modified map held at B's join.
        bool equal = false;
        net::ServerStats stats;
        uint64_t bulkBytes = 0;
    };

    enum class JoinMode { None, Paced, Unpaced };

    /// Client A plays from the start; B joins after 30 drill blasts (chunk sync: its modified chunks
    /// on bulk), or never (the baseline of A's latency). The server's uplink is 48 kB/s, shared: B's
    /// bulk and A's live events queue on it. A's latency is measured for the events made in the 300
    /// ticks after B connected.
    JoinRun RunLateJoin(JoinMode mode) {
        const bool paced = mode != JoinMode::Unpaced;
        net::Impairment im;
        im.delaySeconds = 0.02;
        im.bytesPerSecond = 48.0 * 1024.0;
        // Stage 8 (the flaky test of recheck 7b (e)): reproducible bit for bit. Without these the
        // server's collider cooks ran on worker timing, which decided which drill pieces became server
        // bodies (TakePieceSlot reads Jolt's sleep state) and so B's model requests and bulk: under
        // ctest -j8 B asked for 70.9 KiB instead of 44.3 and was not synced in the measured ticks.
        im.rngPerPeer = true;
        net::ServerSettings settings = Settings(DrillRegion());
        if (!paced) settings.bulkPacer.floorBytes = settings.bulkPacer.ceilingBytes = size_t{64} << 20;   // the window unlimited
        // Stage 6: snapshots carry the drill's falling pieces; at the design's 1200 B to each of two
        // clients they alone would fill this 48 kB/s uplink. The test is about events and bulk.
        settings.snapshotBytes = 128;
        Session s(settings, im);
        EXPECT_TRUE(s.started);
        s.waitIdle = true;
        Session::Bot& a = s.AddClient();
        Session::Bot& b = s.AddClient({}, false);
        const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
        JoinRun run;
        std::vector<uint32_t> madeAt;    // [opSeq - 1] -> tick
        uint32_t seenA = 0, joinTick = 0;
        bool joined = false;
        for (uint32_t step = 0; step < 48; ++step) {
            if (step == 30) {
                if (mode != JoinMode::None) s.Connect(b);
                joined = true;
                joinTick = s.server.TickNumber();
                run.modified = s.server.ModifiedChunks();
            }
            EXPECT_TRUE(net::RunDrillStep(s.server, drill.Step(step)));
            for (uint32_t t = 0; t < 12; ++t) {
                s.Tick();
                const uint32_t tick = s.server.TickNumber();
                while (madeAt.size() < s.server.LastOpSeq()) madeAt.push_back(tick);
                for (; seenA < a.client->AppliedOpSeq(); ++seenA)
                    if (joined && madeAt[seenA] < joinTick + 300) run.maxLiveLatency = std::max(run.maxLiveLatency, tick - madeAt[seenA]);
                if (joined && run.joinTicks == 0 && b.simulation && b.client->Synced()) run.joinTicks = tick - joinTick;
            }
        }
        // Stage 8: B's join is measured in ticks to its end, not in a fixed window (at most 900 more).
        for (uint32_t t = 0; mode != JoinMode::None && run.joinTicks == 0 && t < 900; ++t) {
            s.Tick();
            if (b.simulation && b.client->Synced()) run.joinTicks = s.server.TickNumber() - joinTick;
        }
        EXPECT_TRUE(s.Drain());
        run.equal = a.client->Hash().hash == s.server.Hash().hash &&
                    (mode == JoinMode::None || (b.client->State() == net::ClientState::Playing && b.client->Hash().hash == s.server.Hash().hash));
        run.stats = s.server.Stats();
        run.bulkBytes = b.client->Stats().bulkBytes;
        return run;
    }
}

// networking.md "Transport": everything that is sent has a byte budget; stage 7a: a joiner's chunk
// data is bulk, handed over only while the peer's bytes in flight are under the delay-based window,
// so on a shared uplink the live stream of the others is not stuck behind it.
TEST(Session, ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing) {
    const JoinRun none = RunLateJoin(JoinMode::None);
    const JoinRun paced = RunLateJoin(JoinMode::Paced);
    const JoinRun unpaced = RunLateJoin(JoinMode::Unpaced);
    // Stage 8: the run is reproducible (waitIdle, rngPerPeer): a second paced run gives the same numbers.
    const JoinRun again = RunLateJoin(JoinMode::Paced);
    EXPECT_EQ(again.bulkBytes, paced.bulkBytes);
    EXPECT_EQ(again.joinTicks, paced.joinTicks);
    EXPECT_EQ(again.maxLiveLatency, paced.maxLiveLatency);
    EXPECT_EQ(again.stats.bulkHeld, paced.stats.bulkHeld);
    EXPECT_EQ(again.stats.events, paced.stats.events);
    std::printf("[ pacing ] %zu modified chunks at the join, %.1f KiB of bulk; A's live latency in the 300 ticks after B connected: max %u "
                "ticks without a join, %u paced, %u unpaced; B synced after %u / %u ticks; bulk held %llu times, window %.1f..%.1f KiB\n",
                paced.modified, static_cast<double>(paced.bulkBytes) / 1024.0, none.maxLiveLatency, paced.maxLiveLatency, unpaced.maxLiveLatency, paced.joinTicks,
                unpaced.joinTicks, static_cast<unsigned long long>(paced.stats.bulkHeld), static_cast<double>(paced.stats.minBulkWindow) / 1024.0,
                static_cast<double>(paced.stats.maxBulkWindow) / 1024.0);
    EXPECT_TRUE(none.equal);
    EXPECT_TRUE(paced.equal);
    EXPECT_TRUE(unpaced.equal);
    EXPECT_GT(paced.bulkBytes, 32u * 1024u);                 // most of a second of the uplink
    EXPECT_GT(paced.stats.bulkHeld, 0u);
    // The join adds little to the others' latency when paced: at most the window (a few ticks of the
    // link) or one message larger than it, which goes alone (no split in 7a; plan amendment 2), plus
    // a tick each way; more when the whole join is handed to the link at once.
    const uint32_t oneMessage = static_cast<uint32_t>(std::ceil(static_cast<double>(paced.stats.maxBulkMessage) / (48.0 * 1024.0 / 60.0)));
    std::printf("[ pacing ] largest bulk message %zu B = %u ticks of the link\n", paced.stats.maxBulkMessage, oneMessage);
    EXPECT_LE(paced.maxLiveLatency, none.maxLiveLatency + oneMessage + 2);
    EXPECT_GT(unpaced.maxLiveLatency, paced.maxLiveLatency);
    EXPECT_GT(paced.joinTicks, 0u);
}

// --- a late join under fire (stage 7a) ----------------------------------------------------------------

namespace {
    struct FireRun {
        uint32_t p99A = 0, p99B = 0;      ///< event latency p99 (ticks from made to applied) in the measured window
        uint32_t maxA = 0;
        uint32_t joinTick = 0, syncTicks = 0;
        uint64_t bulkBytes = 0;
        bool converged = false;
        net::ServerStats stats;
        std::string entities;
    };

    uint32_t P99(std::vector<uint32_t> v) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(0.99 * static_cast<double>(v.size()))) - 1)];
    }

    /// The drill at 10 casts/s with its crates (pieces, crates broken into fragments) beside client A;
    /// every link 5 % loss and 40 + 0-30 ms, each peer's downlink 64 kB/s (or as given); B joins at step 60 (or
    /// never), bulk capped at `bulkCap` bytes/s. Latency is measured for the events made in the 600
    /// ticks after B connected.
    FireRun RunUnderFire(bool join, double bulkCap, double peerBytesPerSecond = 64.0 * 1024.0, double loss = 0.05) {
        net::Impairment im = Lossy();
        im.loss = loss;
        im.peerBytesPerSecond = peerBytesPerSecond;
        im.rngPerPeer = true;           // A's packets meet the same losses with and without B
        net::ServerSettings settings = Settings(DrillRegion());
        settings.maxBulkBytesPerSecond = bulkCap;
        Session s(settings, im);
        s.waitIdle = true;
        EXPECT_TRUE(s.started);
        Session::Bot& a = s.AddClient();
        Session::Bot& b = s.AddClient({}, false);
        const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
        FireRun run;
        std::vector<uint32_t> madeAt;     // [opSeq - 1] -> server tick
        std::vector<uint32_t> latA, latB;
        uint32_t seenA = 0, seenB = 0;
        bool joined = false;
        auto note = [&] {
            while (madeAt.size() < s.server.LastOpSeq()) madeAt.push_back(s.server.TickNumber());
        };
        for (uint32_t step = 0; step < 200; ++step) {
            if (step == 60 && join) {
                s.Connect(b);
                joined = true;
                run.joinTick = s.server.TickNumber();
            } else if (step == 60) {
                run.joinTick = s.server.TickNumber();
            }
            EXPECT_TRUE(net::RunDrillStep(s.server, drill.Step(step)));
            note();
            for (uint32_t t = 0; t < kStepTicks; ++t) {
                s.Tick();
                note();
                const uint32_t tick = s.server.TickNumber();
                const bool measure = run.joinTick != 0;
                for (; seenA < a.client->AppliedOpSeq(); ++seenA)
                    if (measure && madeAt[seenA] >= run.joinTick && madeAt[seenA] < run.joinTick + 600) latA.push_back(tick - madeAt[seenA]);
                if (joined && b.simulation) {
                    if (seenB == 0) seenB = b.client->JoinOpSeq();
                    for (; seenB < b.client->AppliedOpSeq(); ++seenB)
                        if (madeAt[seenB] < run.joinTick + 600) latB.push_back(tick - madeAt[seenB]);
                    if (run.syncTicks == 0 && b.client->Synced()) run.syncTicks = tick - run.joinTick;
                }
            }
        }
        // Drain: the casts end, every body rests, every client is synced at the last event.
        for (int i = 0; i < 1800 && s.server.AwakeBodies() > 0; ++i) s.Tick();
        EXPECT_TRUE(s.Drain());
        const voxel::WorldHashResult h = s.server.Hash();
        run.converged = h.Complete() && a.client->Hash().hash == h.hash;
        if (join) {
            const voxel::WorldHashResult hb = b.client->Hash();
            const net::EntityComparison e = net::CompareEntities(s.server.Entities(), b.client->Entities());
            run.entities = e.Describe();
            run.converged = run.converged && hb.Complete() && hb.hash == s.server.Hash(b.client->SyncWindow()).hash && b.client->Synced() &&
                            e.Agree(0.001f) && net::CompareEntities(s.server.Entities(), a.client->Entities()).Agree(0.001f);
            run.bulkBytes = b.client->Stats().bulkBytes;
        }
        run.p99A = P99(latA);
        run.p99B = P99(latB);
        for (const uint32_t l : latA) run.maxA = std::max(run.maxA, l);
        run.stats = s.server.Stats();
        return run;
    }
}

// networking.md "Testing": "Late join during 10 casts/s and object splits, bulk limited to 32 kB/s ...
// converges: equal hash, ids and models; event latency p99 under 250 ms during the join". Plan
// amendment 2: the join's cost is measured against the same run without it (on a 64 kB/s link that
// also carries the snapshots a lost reliable packet alone costs 10-12 ticks, so there it is asserted
// relative); the absolute 15 ticks is asserted on a 128 kB/s link (review of stage 7a).
TEST(ChunkSync, LateJoinDuring10CastsPerSecondConverges) {
    const FireRun quiet = RunUnderFire(false, 32768.0);
    const FireRun joined = RunUnderFire(true, 32768.0);
    const double seconds = static_cast<double>(std::max(1u, joined.syncTicks)) / 60.0;
    std::printf("[ late join ] event latency p99: A %u ticks without the join, A %u / B %u with it (A's max %u / %u); B synced after %.2f s; "
                "%.1f KiB of bulk (%.1f KiB/s over the join, cap 32), %llu ChunkData, %u baseline entities, %u model / %u chunk requests; "
                "entities %s\n",
                quiet.p99A, joined.p99A, joined.p99B, quiet.maxA, joined.maxA, seconds, static_cast<double>(joined.bulkBytes) / 1024.0,
                static_cast<double>(joined.bulkBytes) / 1024.0 / seconds, static_cast<unsigned long long>(joined.stats.chunkData),
                joined.stats.baselineEntities, joined.stats.modelRequests, joined.stats.chunkRequests, joined.entities.c_str());
    EXPECT_TRUE(quiet.converged);
    EXPECT_TRUE(joined.converged) << joined.entities;
    EXPECT_GT(joined.syncTicks, 0u);
    EXPECT_GT(joined.stats.chunkData, 0u);
    // The bulk cap holds over the join (its credit allows a 4 KiB burst).
    EXPECT_LE(static_cast<double>(joined.bulkBytes), 32768.0 * seconds * 1.05 + 4096.0 + 16384.0);
    // The join adds at most 3 ticks to the live stream's p99 of the client already playing. The
    // joiner's own events share its downlink with its bulk: at most the same plus one bulk message
    // larger than the window, which goes alone (7a splits none: plan amendment 2; here a felled
    // tower's model of about 10 KB, 9-10 ticks of a 64 kB/s link).
    const uint32_t oneMessage = static_cast<uint32_t>(std::ceil(static_cast<double>(joined.stats.maxBulkMessage) / (64.0 * 1024.0 / 60.0)));
    std::printf("[ late join ] largest bulk message %zu B = %u ticks of the joiner's link; design target p99 < 15 ticks: A %s, B %s\n",
                joined.stats.maxBulkMessage, oneMessage, joined.p99A < 15 ? "met" : "NOT met", joined.p99B < 15 ? "met" : "NOT met");
    EXPECT_LE(joined.p99A, quiet.p99A + 3);
    // The joiner's own events share its downlink with its bulk: a fixed allowance of 10 ticks (one
    // felled tower's model of about 10 KB on this link; review of stage 7a: a bound scaled by the
    // largest message would loosen as messages grow).
    EXPECT_LE(joined.p99B, quiet.p99A + 10);

    // The design's absolute target where the link allows it (review of stage 7a): 128 kB/s per peer,
    // bulk capped at 32 kB/s as the design row says, 1 % loss - p99 under 15 ticks (250 ms) for both
    // clients from B's Welcome until it is synced. (At 5 % loss no pacing can meet it: a lost reliable
    // packet is resent after about 130 ms and holds the 8 ticks of events behind it, so about 40 % of
    // the events wait - the quiet run's p99 is 21 ticks on any downlink.)
    const FireRun wide = RunUnderFire(true, 32768.0, 128.0 * 1024.0, 0.01);
    std::printf("[ late join ] 128 kB/s per peer, 1 %% loss: event latency p99 A %u / B %u ticks; B synced after %.2f s, %.1f KiB of bulk, "
                "%llu ChunkData, %u baseline entities, %u model requests, window %.1f..%.1f KiB\n",
                wide.p99A, wide.p99B, static_cast<double>(wide.syncTicks) / 60.0, static_cast<double>(wide.bulkBytes) / 1024.0,
                static_cast<unsigned long long>(wide.stats.chunkData), wide.stats.baselineEntities, wide.stats.modelRequests,
                static_cast<double>(wide.stats.minBulkWindow) / 1024.0, static_cast<double>(wide.stats.maxBulkWindow) / 1024.0);
    EXPECT_TRUE(wide.converged) << wide.entities;
    EXPECT_LT(wide.p99A, 15u);
    EXPECT_LT(wide.p99B, 15u);
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
        ASSERT_TRUE(net::RunDrillStep(s.server, drill.Step(step)));
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
    EXPECT_GT(b.client->JoinOpSeq(), s.server.LastOpSeq() / 3);  // B joined by chunk sync half-way
    EXPECT_GT(b.client->Stats().chunkData, 0u);
    // The network really was bad: resends and lost / duplicated status packets on the way down.
    EXPECT_GT(s.serverLink.Stats().resent, 20u);
    EXPECT_GT(s.serverLink.Stats().lost, 5u);
    EXPECT_GT(a.client->Stats().snapshotsReceived, 0u);
    EXPECT_GT(b.client->Stats().snapshotsReceived, 0u);
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
        net::RunDrillStep(s.server, drill.Step(step));
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

// Stage 7a: there is no join log any more; what bounds the server's memory per client is the bytes in
// flight to it. A client that stops reading is disconnected once more than maxPeerBacklogBytes wait for
// it; the others play on, and the server's backlog is trimmed to the slowest joined peer.
TEST(Session, ASlowPeerIsDisconnectedWhenItsBacklogOutgrowsTheLimit) {
    net::ServerSettings settings = Settings(DrillRegion());
    settings.maxPeerBacklogBytes = 4096;
    Session s(settings);
    ASSERT_TRUE(s.started);
    Session::Bot& first = s.AddClient();
    Session::Bot& slow = s.AddClient();
    ASSERT_TRUE(s.Drain());
    slow.frozen = true;                                          // it stops reading
    for (int i = 0; i < 400 && s.server.ClientCount() == 2; ++i) {
        ASSERT_NE(s.server.ApplyOp(sim::WorldOp::Box({i % 64, kGround, i / 64}, {i % 64 + 1, kGround + 1, i / 64 + 1}, DefaultIds().gold)), 0u);
        s.Tick();
    }
    EXPECT_EQ(s.server.ClientCount(), 1u);
    EXPECT_EQ(s.server.Stats().peersTooSlow, 1u);
    EXPECT_EQ(s.server.BacklogBytes(), 0u) << "the backlog keeps events every joined peer has";
    EXPECT_EQ(first.client->State(), net::ClientState::Playing);
    slow.connected = false;
    ASSERT_TRUE(s.Drain());
    EXPECT_EQ(first.client->Hash().hash, s.server.Hash().hash);
}

// --- review of stage 7a ---------------------------------------------------------------------------

namespace {
    /// One chunk whose RLE is large: stone in every other x plane of the air chunk (1, 3, 1) above the
    /// drill region's ground. Returns its ChunkData size.
    size_t MakeNoisyChunk(net::Server& server) {
        const voxel::WorldMaterials ids = DefaultIds();
        const glm::ivec3 lo(32, 96, 32);
        for (int x = 0; x < 32; x += 2) EXPECT_NE(server.ApplyOp(sim::WorldOp::Box(lo + glm::ivec3(x, 0, 0), lo + glm::ivec3(x + 1, 32, 32), ids.stone)), 0u);
        std::vector<uint8_t> rle;
        EXPECT_TRUE(server.Simulation().World().EncodeChunkAt(glm::ivec3(1, 3, 1), rle));
        return rle.size();
    }

    /// A peer that speaks the protocol by hand (no net::Client): it can send what a client never would.
    struct RawPeer {
        net::LoopbackTransport loop;
        net::PeerId server = 0;
        std::vector<net::TransportEvent> received;
        bool disconnected = false;
        void Send(const std::vector<uint8_t>& bytes) { loop.Send(server, net::Channel::Events, bytes); }
        void Poll() {
            net::TransportEvent e;
            while (loop.Poll(e)) {
                if (e.type == net::TransportEvent::Type::Connected) server = e.peer;
                if (e.type == net::TransportEvent::Type::Disconnected) disconnected = true;
                if (e.type == net::TransportEvent::Type::Received) received.push_back(std::move(e));
            }
        }
        /// The received messages of one type, decoded.
        template <typename T>
        std::vector<T> Take(net::MessageType type) {
            std::vector<T> out;
            for (auto it = received.begin(); it != received.end();) {
                if (net::PeekType(it->data) == type) {
                    T m;
                    EXPECT_TRUE(net::Decode(it->data, m));
                    out.push_back(std::move(m));
                    it = received.erase(it);
                } else {
                    ++it;
                }
            }
            return out;
        }
    };
}

// Review of stage 7a: a bulk message larger than the pacer's window went only when nothing at all was
// in flight to the peer, so live events every tick (a fight) held it - and the joiner's whole bulk
// queue behind it - for as long as they flowed. Now it waits only for the peer's bulk in flight.
TEST(Session, AnOversizeBulkMessageGoesWhileEventsKeepFlowing) {
    net::Impairment im;
    im.delaySeconds = 0.05;                  // 100 ms round trip: an event every tick is always in flight
    Session s(Settings(DrillRegion()), im);
    ASSERT_TRUE(s.started);
    Session::Bot& a = s.AddClient();
    for (int i = 0; i < 60; ++i) s.Tick();
    const size_t noisy = MakeNoisyChunk(s.server);
    std::printf("[ oversize ] the noisy chunk's data is %zu B\n", noisy);
    ASSERT_GT(noisy, 20000u);
    Session::Bot& b = s.AddClient({}, false);
    s.Connect(b);
    const voxel::WorldMaterials ids = DefaultIds();
    uint32_t syncedAfter = 0;
    for (uint32_t tick = 1; tick <= 900 && syncedAfter == 0; ++tick) {
        // One live event every tick, in a far corner of the region (not the noisy chunk).
        const glm::ivec3 v(-28 + static_cast<int>(tick % 8), kGround, -28);
        s.server.ApplyOp(sim::WorldOp::Box(v, v + glm::ivec3(1), tick % 2 == 0 ? ids.gold : ids.stone));
        s.Tick();
        if (b.simulation && b.client->Synced()) syncedAfter = tick;
    }
    const net::ServerStats& st = s.server.Stats();
    std::printf("[ oversize ] joiner synced after %u ticks with an event every tick; largest bulk message %zu B, %u sent past the window "
                "(no bulk in flight), window %.1f..%.1f KiB\n",
                syncedAfter, st.maxBulkMessage, st.bulkAlone, static_cast<double>(st.minBulkWindow) / 1024.0,
                static_cast<double>(st.maxBulkWindow) / 1024.0);
    EXPECT_GT(syncedAfter, 0u) << "the oversize ChunkData never left: the joiner never synced";
    EXPECT_LE(syncedAfter, 240u);
    EXPECT_GE(st.maxBulkMessage, noisy);
    EXPECT_GT(st.bulkAlone, 0u);
    ASSERT_TRUE(s.Drain());
    EXPECT_EQ(b.client->Hash().hash, s.server.Hash().hash);
    EXPECT_EQ(a.client->Hash().hash, s.server.Hash().hash);
}

// Recheck of stage 7a, 1: the oversize fix let one bulk message of any size go whenever no bulk was in
// flight, even while the peer's live events alone kept its bytes in flight at or over the window (a
// link the events saturate, the pacer backed off to its floor): bulk then ran at one message per bulk
// round trip on top of the events. The window holds: a message goes alone only while the bytes in
// flight are under it; the join finishes once the events ease.
TEST(Session, BulkWaitsWhileLiveEventsAloneFillTheWindow) {
    net::Impairment im;
    im.delaySeconds = 0.02;
    im.peerBytesPerSecond = 16.0 * 1024.0;   // each peer's downlink: the events below take about 1.4 times it
    im.rngPerPeer = true;
    Session s(Settings(DrillRegion()), im);
    ASSERT_TRUE(s.started);
    Session::Bot& a = s.AddClient();
    ASSERT_TRUE(s.Drain());
    const size_t noisy = MakeNoisyChunk(s.server);     // the joiner's bulk: one large ChunkData and the plaza's
    ASSERT_GT(noisy, 20000u);
    for (int i = 0; i < 24; ++i) ASSERT_NE(s.server.ApplyOp(sim::WorldOp::Box({-28 + i, kGround, 20}, {-27 + i, kGround + 2, 21}, DefaultIds().gold)), 0u);
    Session::Bot& b = s.AddClient({}, false);
    s.Connect(b);
    const voxel::WorldMaterials ids = DefaultIds();
    uint64_t heldBefore = s.server.Stats().bulkHeld;
    for (uint32_t tick = 1; tick <= 240; ++tick) {
        // 12 small boxes a tick in a far corner, alternating gold and stone: about 22 kB/s of events.
        for (int k = 0; k < 12; ++k) {
            const glm::ivec3 v(-28 + k * 2, kGround, -28);
            ASSERT_NE(s.server.ApplyOp(sim::WorldOp::Box(v, v + glm::ivec3(1), (tick + k) % 2 == 0 ? ids.gold : ids.stone)), 0u);
        }
        s.Tick();
    }
    const net::ServerStats& st = s.server.Stats();
    std::printf("[ events fill ] window %.1f..%.1f KiB, bulk held %llu ticks, %llu B of bulk sent so far (both joins), %u alone, %u over the window\n",
                static_cast<double>(st.minBulkWindow) / 1024.0, static_cast<double>(st.maxBulkWindow) / 1024.0,
                static_cast<unsigned long long>(st.bulkHeld - heldBefore), static_cast<unsigned long long>(st.bulkBytes), st.bulkAlone,
                st.bulkOverWindow);
    EXPECT_EQ(st.minBulkWindow, net::BulkPacerSettings{}.floorBytes) << "the events never filled the link: the test shows nothing";
    EXPECT_GT(st.bulkHeld, heldBefore);
    EXPECT_EQ(st.bulkOverWindow, 0u) << "bulk handed over while the live events alone filled the window";
    // The events ease: the join finishes and both clients agree.
    ASSERT_TRUE(s.Drain(3600));
    EXPECT_EQ(b.client->Hash().hash, s.server.Hash().hash);
    EXPECT_EQ(a.client->Hash().hash, s.server.Hash().hash);
}

// Review of stage 7a: a ModelRequest for a NetId whose answer is still queued is answered by that one
// answer (it is encoded at its dequeue, after the request came); a peer whose queued answers exceed
// ServerSettings::maxQueuedAnswers is disconnected like one that broke its sync stream.
TEST(Session, RepeatedModelRequestsAreAnsweredOnceAndAnswersAreBounded) {
    net::ServerSettings settings = Settings(DrillRegion());
    settings.maxQueuedAnswers = 64;
    Session s(settings);
    ASSERT_TRUE(s.started);
    const uint32_t crate = s.server.SpawnObject(sim::CrateModel(DefaultIds(), 1.25), glm::dvec3(2.0, 12.5, 2.0));
    ASSERT_NE(crate, 0u);
    Session::Bot& a = s.AddClient();
    ASSERT_TRUE(s.Drain());
    const uint32_t answersBefore = s.server.Stats().baselineEntities;
    for (int i = 0; i < 100; ++i) a.loop.Send(1, net::Channel::Events, net::Encode(net::ModelRequest{{crate}}));
    for (int i = 0; i < 30; ++i) s.Tick();
    EXPECT_EQ(s.server.Stats().baselineEntities - answersBefore, 1u) << "one answer for 100 requests of a queued id";
    EXPECT_EQ(s.server.Stats().modelRequestRepeats, 99u);
    EXPECT_EQ(a.client->State(), net::ClientState::Playing);
    EXPECT_EQ(a.client->Stats().baselineDiscarded, 1u);       // it holds the copy already
    // Asked again once that answer has gone: answered again.
    a.loop.Send(1, net::Channel::Events, net::Encode(net::ModelRequest{{crate}}));
    for (int i = 0; i < 30; ++i) s.Tick();
    EXPECT_EQ(s.server.Stats().baselineEntities - answersBefore, 2u);
    // 65 chunk requests in one message (inside the window, all queued at once): over the bound.
    const net::SyncBox box = a.client->SyncWindow();
    net::ChunkRequest many;
    for (int i = 0; i < 65; ++i) many.chunks.push_back(box.min + glm::ivec3(i % box.size.x, (i / box.size.x) % box.size.y, i / (box.size.x * box.size.y)));
    a.loop.Send(1, net::Channel::Events, net::Encode(many));
    for (int i = 0; i < 5; ++i) s.Tick();
    EXPECT_EQ(s.server.Stats().malformedRequests, 1u);
    EXPECT_EQ(s.server.ClientCount(), 0u);
}

// Review of stage 7a: the server's rules for sync messages a client never sends (networking.md "Stage 7a
// plan", messages): requests outside the windows and for unknown NetIds are ignored and counted; a
// WindowUpdate out of order, or with rows that are not the region's, ends the peer's session; a window
// that jumps by more than its size has every modified chunk of its new box marked in the ack.
TEST(Session, HostileOrOutOfOrderSyncMessagesAreIgnoredOrEndThePeer) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    const voxel::WorldMaterials ids = DefaultIds();
    // Three modified chunks.
    for (const glm::ivec3& v : {glm::ivec3(1, kGround, 1), glm::ivec3(40, kGround, 1), glm::ivec3(1, kGround, 40)})
        ASSERT_NE(s.server.ApplyOp(sim::WorldOp::Box(v, v + glm::ivec3(1), ids.gold)), 0u);
    s.Tick();
    const net::SyncBox region = net::RegionSyncBox(s.server.Region());
    RawPeer raw;
    net::LoopbackTransport::Connect(s.serverLoop, raw.loop);
    raw.Poll();
    raw.Send(net::Encode(net::Hello{net::kProtocolVersion, s.server.BuildHashValue(), TestToken()}));
    auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            s.Tick();
            raw.Poll();
        }
    };
    run(2);
    ASSERT_EQ(raw.Take<net::Welcome>(net::MessageType::Welcome).size(), 1u);
    raw.Send(net::Encode(net::WindowUpdate{1, 0, region.min, region.size}));
    run(2);
    std::vector<net::WindowAck> acks = raw.Take<net::WindowAck>(net::MessageType::WindowAck);
    ASSERT_EQ(acks.size(), 1u);
    EXPECT_EQ(acks[0].modified.Count(), 3u);
    run(30);                                                  // its three ChunkData
    const uint64_t chunkDataBefore = s.server.Stats().chunkData;
    const uint32_t ignoredBefore = s.server.Stats().requestsIgnored;
    // A chunk outside its window and the server's, a NetId that never existed: ignored, nothing sent.
    raw.Send(net::Encode(net::ChunkRequest{{region.Max() + glm::ivec3(5, 0, 5)}}));
    raw.Send(net::Encode(net::ModelRequest{{9999}}));
    run(10);
    EXPECT_EQ(s.server.Stats().requestsIgnored, ignoredBefore + 2u);
    EXPECT_EQ(s.server.Stats().chunkData, chunkDataBefore);
    EXPECT_FALSE(raw.disconnected);
    // Recheck of stage 7a, 4: a server keeps no per-coordinate request counts unless a test asks.
    EXPECT_TRUE(s.server.ChunkRequestCounts().empty()) << "the request counts grow with every coordinate asked for";
    // The window jumps far away (nothing of the server's window), then back by more than its size: no
    // overlap with the previous box, so every modified chunk of the new box is entering.
    raw.Send(net::Encode(net::WindowUpdate{2, 0, region.min + glm::ivec3(500, 0, 0), region.size}));
    raw.Send(net::Encode(net::WindowUpdate{3, 0, region.min, region.size}));
    run(2);
    acks = raw.Take<net::WindowAck>(net::MessageType::WindowAck);
    ASSERT_EQ(acks.size(), 2u);
    EXPECT_EQ(acks[0].modified.Count(), 0u);
    EXPECT_EQ(acks[1].modified.Count(), 3u);
    EXPECT_EQ(s.server.Stats().malformedRequests, 0u);
    // Out of order (4 skipped): the session ends.
    raw.Send(net::Encode(net::WindowUpdate{5, 0, region.min, region.size}));
    run(3);
    EXPECT_EQ(s.server.Stats().malformedRequests, 1u);
    EXPECT_TRUE(raw.disconnected);
    EXPECT_EQ(s.server.ClientCount(), 0u);
    // Rows that are not the region's.
    RawPeer rows;
    net::LoopbackTransport::Connect(s.serverLoop, rows.loop);
    rows.Poll();
    rows.Send(net::Encode(net::Hello{net::kProtocolVersion, s.server.BuildHashValue(), TestToken()}));
    for (int i = 0; i < 2; ++i) {
        s.Tick();
        rows.Poll();
    }
    rows.Send(net::Encode(net::WindowUpdate{1, 0, region.min + glm::ivec3(0, 1, 0), region.size}));
    for (int i = 0; i < 3; ++i) {
        s.Tick();
        rows.Poll();
    }
    EXPECT_EQ(s.server.Stats().malformedRequests, 2u);
    EXPECT_TRUE(rows.disconnected);

    // Stage 7b. A peer that joined: 50 EntityListRequests in one tick give one list (49 merged), sent
    // right after its events; a ChunkRequest with an unknown reason ends it.
    auto join = [&](RawPeer& peer) {
        net::LoopbackTransport::Connect(s.serverLoop, peer.loop);
        peer.Poll();
        peer.Send(net::Encode(net::Hello{net::kProtocolVersion, s.server.BuildHashValue(), TestToken()}));
        for (int i = 0; i < 2; ++i) {
            s.Tick();
            peer.Poll();
        }
        peer.Send(net::Encode(net::WindowUpdate{1, 0, region.min, region.size}));
        for (int i = 0; i < 2; ++i) {
            s.Tick();
            peer.Poll();
        }
    };
    RawPeer lists;
    join(lists);
    const uint32_t listsBefore = s.server.Stats().entityListsSent;
    for (int i = 0; i < 50; ++i) lists.Send(net::Encode(net::EntityListRequest{s.server.LastOpSeq()}));
    for (int i = 0; i < 3; ++i) {
        s.Tick();
        lists.Poll();
    }
    EXPECT_EQ(s.server.Stats().entityListsSent, listsBefore + 1);
    EXPECT_EQ(s.server.Stats().entityListsMerged, 49u);
    const std::vector<net::EntityList> sent = lists.Take<net::EntityList>(net::MessageType::EntityList);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].asOfOpSeq, s.server.LastOpSeq());
    EXPECT_FALSE(lists.disconnected);
    // Review of stage 7b: a client that asks every tick (a broken back-off) gets at most one list per
    // half check period (60 ticks here): none more during 50 ticks of requests, then one for all of them.
    for (int i = 0; i < 50; ++i) {
        lists.Send(net::Encode(net::EntityListRequest{s.server.LastOpSeq()}));
        s.Tick();
        lists.Poll();
    }
    EXPECT_EQ(s.server.Stats().entityListsSent, listsBefore + 1) << "a list per request";
    EXPECT_GT(s.server.Stats().entityListsDelayed, 0u);
    for (int i = 0; i < 20; ++i) {
        s.Tick();
        lists.Poll();
    }
    EXPECT_EQ(s.server.Stats().entityListsSent, listsBefore + 2);
    // Recheck of stage 7b: the delayed list is counted once, not once per tick it waited.
    EXPECT_EQ(s.server.Stats().entityListsDelayed, 1u) << "delayed lists, not ticks of waiting";
    EXPECT_EQ(lists.Take<net::EntityList>(net::MessageType::EntityList).size(), 1u);
    EXPECT_FALSE(lists.disconnected);
    std::vector<uint8_t> unknown = net::Encode(net::ChunkRequest{{region.min}, net::ChunkRequestReason::Repair});
    unknown[1] = 7;
    lists.Send(unknown);
    for (int i = 0; i < 3; ++i) {
        s.Tick();
        lists.Poll();
    }
    EXPECT_EQ(s.server.Stats().malformedRequests, 3u);
    EXPECT_TRUE(lists.disconnected);
    // Server terrain mode (stage 7b M-b): a terrain-mode WindowUpdate is answered with a TerrainAck
    // that describes every chunk of the server's window inside it (none left at code 0; the first
    // update's whole box enters) and queues the non-uniform ones; the flag is sticky - an update
    // without it ends the peer (choice 12).
    RawPeer terrain;
    net::LoopbackTransport::Connect(s.serverLoop, terrain.loop);
    terrain.Poll();
    terrain.Send(net::Encode(net::Hello{net::kProtocolVersion, s.server.BuildHashValue(), TestToken()}));
    for (int i = 0; i < 2; ++i) {
        s.Tick();
        terrain.Poll();
    }
    terrain.Send(net::Encode(net::WindowUpdate{1, net::WindowUpdate::kTerrainMode, region.min, region.size}));
    for (int i = 0; i < 3; ++i) {
        s.Tick();
        terrain.Poll();
    }
    EXPECT_EQ(s.server.Stats().malformedRequests, 3u);
    EXPECT_FALSE(terrain.disconnected);
    EXPECT_TRUE(terrain.Take<net::WindowAck>(net::MessageType::WindowAck).empty());
    const std::vector<net::TerrainAck> tacks = terrain.Take<net::TerrainAck>(net::MessageType::TerrainAck);
    ASSERT_EQ(tacks.size(), 1u);
    EXPECT_EQ(tacks[0].size, region.size);
    uint64_t described = 0, data = 0;
    for (const net::TerrainRun& r : tacks[0].runs) {
        described += r.code != net::TerrainCode::NotInAck ? r.count : 0u;
        data += r.code == net::TerrainCode::Data || r.code == net::TerrainCode::HiddenData ? r.count : 0u;
    }
    EXPECT_EQ(described, static_cast<uint64_t>(region.size.x) * region.size.y * region.size.z) << "the region's window is the server's";
    EXPECT_GT(data, 3u) << "pristine non-uniform chunks are data too";
    EXPECT_EQ(s.server.Stats().terrainModePeers, 1u);
    terrain.Send(net::Encode(net::WindowUpdate{2, 0, region.min, region.size}));
    for (int i = 0; i < 3; ++i) {
        s.Tick();
        terrain.Poll();
    }
    EXPECT_EQ(s.server.Stats().malformedRequests, 4u);
    EXPECT_TRUE(terrain.disconnected);
}

// Stage 7b (networking.md "HashCheck: server", "Wire EntityCheck"): every joined peer gets a HashCheck
// every 120 ticks once its first window ack went out, and an EntityCheck every 120 ticks once its
// baseline ended, each sent right after the peer's events: every check a client processes is at its
// stream position (applied == its opSeq), over the drill's 3000 ticks with a late joiner, 5 % loss and
// reordering. With events held in the server by maxEventBytesInFlight (a test hook) the checks wait
// with them and still come in order. No check finds anything (no fault was injected).
TEST(Session, ChecksAreSentEvery2sAfterTheJoinAndAtTheStreamPosition) {
    for (const uint32_t held : {0u, 1500u}) {
        SCOPED_TRACE(held == 0 ? "events never held" : "events held by maxEventBytesInFlight");
        net::ServerSettings settings = Settings(DrillRegion());
        settings.maxEventBytesInFlight = held;
        Session s(settings, Lossy());
        ASSERT_TRUE(s.started);
        net::ClientSettings presenting;
        presenting.presentation = true;
        Session::Bot& a = s.AddClient(presenting);
        Session::Bot& b = s.AddClient({}, false);
        const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
        const uint32_t ticks = held == 0 ? 3000 : 1200;
        uint32_t joinTick = 0;
        for (uint32_t step = 0; step * kStepTicks < ticks; ++step) {
            if (step == ticks / 3 / kStepTicks) {
                s.Connect(b);
                joinTick = s.server.TickNumber();
            }
            ASSERT_TRUE(net::RunDrillStep(s.server, drill.Step(step)));
            for (uint32_t t = 0; t < kStepTicks; ++t) s.Tick();
            ASSERT_FALSE(s.server.Failed());
        }
        ASSERT_TRUE(s.Drain());
        const net::ServerStats& st = s.server.Stats();
        uint32_t hashChecks = 0, entityChecks = 0;
        for (Session::Bot* bot : {&a, &b}) {
            const net::ClientStats& c = bot->client->Stats();
            EXPECT_EQ(c.checksOutOfOrder, 0u);
            EXPECT_EQ(bot->client->Sync().Stats().checksOutOfOrder, 0u);
            EXPECT_EQ(bot->client->Sync().Stats().mismatches, 0u);
            EXPECT_EQ(c.entityMismatches, 0u);
            hashChecks += c.hashChecks;
            entityChecks += c.entityChecks + c.entityChecksPartial + c.entityChecksSkipped + c.entityChecksAhead;
            std::printf("[ checks ] %s: %u hash checks (%llu entries equal, %llu ahead, %llu not current), %u entity checks (%u partial, %u "
                        "skipped, %u ahead), %u out of order\n",
                        bot == &a ? "A" : "B", c.hashChecks, static_cast<unsigned long long>(bot->client->Sync().Stats().checkEqual),
                        static_cast<unsigned long long>(bot->client->Sync().Stats().checkAhead),
                        static_cast<unsigned long long>(bot->client->Sync().Stats().checkNotCurrent), c.entityChecks, c.entityChecksPartial,
                        c.entityChecksSkipped, c.entityChecksAhead, c.checksOutOfOrder);
        }
        // Every check sent was processed, at its position (the last one of each client may still be in flight).
        EXPECT_LE(hashChecks, st.hashChecksSent);
        EXPECT_GE(hashChecks + 2, st.hashChecksSent);
        EXPECT_LE(entityChecks, st.entityChecksSent);
        EXPECT_GE(entityChecks + 2, st.entityChecksSent);
        std::printf("[ checks ] server: %u hash checks, %u entity checks over %u ticks; %u batches held\n", st.hashChecksSent, st.entityChecksSent,
                    s.server.TickNumber(), st.sendsHeld);
        if (held == 0) {
            // One of each kind per 120 ticks per peer, from its join on (B joined a third of the way in).
            const uint32_t total = s.server.TickNumber();
            EXPECT_GE(a.client->Stats().hashChecks + 2, total / 120);
            EXPECT_LE(a.client->Stats().hashChecks, total / 120 + 1);
            EXPECT_GE(b.client->Stats().hashChecks + 3, (total - joinTick) / 120);
            EXPECT_LE(b.client->Stats().hashChecks, (total - joinTick) / 120 + 1);
            EXPECT_GE(a.client->Stats().entityChecks + a.client->Stats().entityChecksPartial + 3, total / 120);
        } else {
            EXPECT_GT(st.sendsHeld, 0u) << "the hook held nothing";
            EXPECT_GT(st.hashChecksSent, 0u);
        }
        const voxel::WorldHashResult server = s.server.Hash();
        for (Session::Bot* bot : {&a, &b}) EXPECT_EQ(bot->client->Hash().hash, server.hash);
    }
}

// --- join and leave ticks (stage 8) -------------------------------------------------------------------

// networking.md "Stage 8": the owner's server logged a 126 ms pass at a first join: OnHello cooked every
// pending collider (Terrain().Flush()) and, with no player near the spawn, the spawn's ground. The spawn
// is now a permanent collider focus built at Start and the join does no global flush: its tick cooks no
// collider on its own thread, even right after a drill step left re-cooks pending elsewhere.
TEST(Session, TheFirstJoinCooksNoColliderInItsTick) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    s.waitIdle = true;
    EXPECT_GT(s.server.Stats().spawnColliders, 0u);
    const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
    for (uint32_t t = 0; t < 600; ++t) s.Tick();
    // A drill step at a corner site one tick before the join: chunks there wait for re-cooks.
    ASSERT_TRUE(net::RunDrillStep(s.server, drill.Step(0)));
    s.Tick();
    s.AddClient();
    uint64_t joinTickBuilt = 0;
    for (uint32_t t = 0; t < 30 && s.server.Stats().helloTicks == 0; ++t) {
        const uint64_t builtBefore = s.server.Simulation().Terrain().Stats().built;
        s.Tick();                                        // the Hello tick (the player's first step too)
        joinTickBuilt = s.server.Simulation().Terrain().Stats().built - builtBefore;
    }
    const net::ServerStats& st = s.server.Stats();
    std::printf("[ join tick ] %u spawn colliders at Start (%.1f ms); the join tick: %u cooked on its thread, %llu built in all, %.2f ms\n",
                st.spawnColliders, st.spawnColliderSeconds * 1000.0, st.maxHelloTickCooks, static_cast<unsigned long long>(joinTickBuilt),
                st.maxHelloTickSeconds * 1000.0);
    EXPECT_EQ(st.helloTicks, 1u);
    EXPECT_EQ(st.maxHelloTickCooks, 0u);
    // What the background finishes in a tick (one round of the job budget), never the spawn's ~100.
    EXPECT_LT(joinTickBuilt, 16u);
    for (uint32_t t = 0; t < 120; ++t) s.Tick();
    s.AddClient();
    for (uint32_t t = 0; t < 30 && s.server.Stats().helloTicks < 2; ++t) s.Tick();
    EXPECT_EQ(s.server.Stats().helloTicks, 2u);
    EXPECT_EQ(s.server.Stats().maxHelloTickCooks, 0u);
    EXPECT_TRUE(s.Drain());
    EXPECT_EQ(s.bots[0]->client->Hash().hash, s.server.Hash().hash);
    EXPECT_EQ(s.bots[1]->client->Hash().hash, s.server.Hash().hash);
}

// The leave stall (207 ms in the owner's run): vortex_server hashes the window when a client leaves. The
// server's hashes are cached per chunk by World::ChunkRevision: a quiet window is hashed by lookups, an
// edit recomputes the chunks it changed, and the value is WorldHash's.
TEST(Session, ServerHashesAreCachedByChunkRevision) {
    Session s(Settings(DrillRegion()));
    ASSERT_TRUE(s.started);
    s.waitIdle = true;
    const voxel::WorldHashResult first = s.server.Hash();
    const uint64_t computed = s.server.ChunkHashesComputed();
    EXPECT_GT(computed, 0u);
    EXPECT_EQ(first.hash, net::RegionHash(s.server.Simulation().World(), s.server.Region()).hash);
    for (uint32_t t = 0; t < 30; ++t) s.Tick();
    const voxel::WorldHashResult quiet = s.server.Hash();
    EXPECT_EQ(quiet.hash, first.hash);
    EXPECT_EQ(s.server.ChunkHashesComputed(), computed);      // nothing changed: lookups only
    const net::BlastDrill drill(s.server.Region(), DefaultIds(), kGround);
    ASSERT_TRUE(net::RunDrillStep(s.server, drill.Step(0)));
    for (uint32_t t = 0; t < 30; ++t) s.Tick();
    const voxel::WorldHashResult edited = s.server.Hash();
    const uint64_t recomputed = s.server.ChunkHashesComputed() - computed;
    EXPECT_NE(edited.hash, first.hash);
    EXPECT_EQ(edited.hash, net::RegionHash(s.server.Simulation().World(), s.server.Region()).hash);
    EXPECT_GT(recomputed, 0u);
    EXPECT_LT(recomputed, computed / 4);                      // the drill site's chunks, not the window
    const net::SyncBox box = net::RegionSyncBox(s.server.Region());
    EXPECT_EQ(s.server.Hash(box).hash, voxel::WorldHash(s.server.Simulation().World(), box.min, box.Max()).hash);
}

// land.md [land-5] "Decision" 3: no map travels. The server's Welcome carries its baked map's hash; a client that
// baked the same map from the same files joins, one with other files (or none) fails with the reason.
namespace {
    /// A small round island (40 x 40 pixels of 5 m), no starter region: a quick bake.
    voxel::LandSource SmallIsland(float bump = 0.0f) {
        voxel::LandSource s;
        s.width = s.height = 40;
        s.metresPerPixel = 5.0f;
        s.heights.resize(1600);
        s.biomes.resize(1600);
        s.regions.resize(1600);
        for (int z = 0; z < 40; ++z)
            for (int x = 0; x < 40; ++x) {
                const size_t i = size_t(z) * 40 + size_t(x);
                const float d2 = float((x - 20) * (x - 20) + (z - 20) * (z - 20));
                const bool land = d2 < 256.0f;
                s.heights[i] = land ? 1.0f + (256.0f - d2) * 0.05f : -6.0f;
                s.biomes[i] = uint8_t(land ? voxel::LandBiome::Grassland : voxel::LandBiome::Sea);
                s.regions[i] = land ? 1 : 0;
            }
        s.heights[20 * 40 + 20] += bump;   // another file: one pixel moved
        return s;
    }

    std::shared_ptr<const voxel::LandMap> Bake(const voxel::LandSource& s, int streamIterations = 40) {
        voxel::LandSettings st;
        st.streamIterations = streamIterations;
        return std::make_shared<const voxel::LandMap>(voxel::BakeLand(s, st));
    }

    sim::RegionDesc LandRegion(std::shared_ptr<const voxel::LandMap> map) {
        sim::RegionDesc r = DrillRegion();
        r.heightRange = {-16.0f, 48.0f};
        sim::SetRegionLand(r, std::move(map));
        return r;
    }
}

TEST(LandSession, AClientWithTheSameMapJoinsAndBuildsTheServersWorld) {
    const auto map = Bake(SmallIsland());
    Session s(Settings(LandRegion(map)));
    ASSERT_TRUE(s.started);
    s.autoAttach = false;
    net::ClientSettings cs;
    const auto own = Bake(SmallIsland());          // the client's own bake of the same files
    cs.landMap = [own] { return own; };
    Session::Bot& bot = s.AddClient(cs);
    for (int i = 0; i < 5 && bot.client->State() != net::ClientState::Welcomed; ++i) s.Tick();
    ASSERT_EQ(bot.client->State(), net::ClientState::Welcomed);
    EXPECT_EQ(bot.client->Region().generator, sim::RegionGenerator::Land);
    EXPECT_EQ(bot.client->Region().landHash, voxel::LandHash(*map));
    EXPECT_EQ(bot.client->Stats().probeMismatches, 0u);
    sim::Simulation world(bot.client->MakeSimulationConfig(bot.base));
    ASSERT_TRUE(world.Initialize(bot.client->MakeGenerator()));
    ASSERT_TRUE(bot.client->Attach(world));
    EXPECT_EQ(bot.client->Hash().hash, s.server.Hash().hash);
}

TEST(LandSession, AClientWithOtherMapFilesOrNoneFails) {
    Session s(Settings(LandRegion(Bake(SmallIsland()))));
    ASSERT_TRUE(s.started);
    s.autoAttach = false;
    net::ClientSettings other;
    const auto moved = Bake(SmallIsland(3.0f));
    other.landMap = [moved] { return moved; };
    Session::Bot& a = s.AddClient(other);
    Session::Bot& b = s.AddClient();               // no map at all
    for (int i = 0; i < 5; ++i) s.Tick();
    EXPECT_EQ(a.client->State(), net::ClientState::Failed);
    EXPECT_EQ(b.client->State(), net::ClientState::Failed);
}

TEST(LandSession, TheReasonNamesTheFilesOrTheBake) {
    sim::RegionDesc region = LandRegion(Bake(SmallIsland()));
    std::string why;
    sim::RegionDesc a = region;
    EXPECT_FALSE(sim::CheckRegionLand(a, Bake(SmallIsland(3.0f)), why));
    EXPECT_NE(why.find("map files differ"), std::string::npos) << why;
    // The same files baked by other code (here: other bake settings): the files agree, the bake does not.
    sim::RegionDesc b = region;
    EXPECT_FALSE(sim::CheckRegionLand(b, Bake(SmallIsland(), 10), why));
    EXPECT_NE(why.find("another build of the bake"), std::string::npos) << why;
    sim::RegionDesc c = region;
    EXPECT_TRUE(sim::CheckRegionLand(c, Bake(SmallIsland()), why));
    EXPECT_NE(c.land, nullptr);
}

TEST(LandSession, TheLandHashesTravelInTheWelcomeAndTheSessionFile) {
    const sim::RegionDesc region = LandRegion(Bake(SmallIsland()));
    net::SessionConfig config;
    config.token = TestToken();
    config.region = region;
    config.landMap = "assets/world";
    net::SessionConfig back;
    std::string error;
    ASSERT_TRUE(net::ParseSession(net::FormatSession(config), back, error)) << error;
    EXPECT_EQ(back.region.generator, sim::RegionGenerator::Land);
    EXPECT_EQ(back.landMap, "assets/world");
    // The hashes are the bake's, not the file's: a parsed session has none until it is baked.
    EXPECT_EQ(back.region.landHash, 0u);
    EXPECT_NE(region.landHash, 0u);
    EXPECT_NE(region.landSourceHash, 0u);
}
