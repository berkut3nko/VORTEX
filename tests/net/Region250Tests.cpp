// Networking stage 7b (docs/design/networking.md "Stage 7b plan" with its amendments, "Testing"): the
// test table on a 250 m region. `Plain250` (FieldHarness.h): 64 x 64 chunks of flat play at 0.125 m,
// bots with the game's 48-chunk moving window and store. Milestone M-a's rows: HashCheck and the wire
// EntityCheck in a fight (no false mismatch), a broken chunk on a walker's way repaired before it gets
// there, a corrupted chunk and a lost copy repaired, an entity mismatch re-sent by the list. Milestone
// M-b's: server terrain mode (a generator broken everywhere, three broken pristine chunks), and 7a's
// rows re-run on 250 m (late join during casts with a terrain-mode joiner, walk in from outside, chunks
// that load late while the window moves, a small store, the border blast, the collapse, the region's
// edge and ceiling), plus terrain mode with packing on a small noise region.
//
// Every test ends with each bot synced at the server's last opSeq, its hash over its sync box equal to
// the server's, its objects agreeing (Field::ExpectSameWorld), and - where no fault was injected - no
// hash or entity mismatch and no check out of its stream position.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.scene;
import vortex.physics;
import vortex.sim;
import vortex.net;

using namespace vortex;

#include "FieldHarness.h"

namespace {
    /// The checks a bot compared, and what they found (one line per bot).
    void PrintChecks(const char* name, const Field::Bot& bot) {
        const net::ClientStats& c = bot.client->Stats();
        const net::ChunkSyncStats& s = bot.client->Sync().Stats();
        std::printf("[ checks ] %s: %u hash checks (%llu equal, %llu ahead, %llu not current, %llu outside), %u mismatches (%u pristine, %u "
                    "modified), %u repairs installed; %u entity checks + %u partial (%u skipped, %u ahead), %u mismatches, %u lists, %u "
                    "restored / %u replaced / %u dropped; %u out of order; checks %.3f ms max\n",
                    name, c.hashChecks, static_cast<unsigned long long>(s.checkEqual), static_cast<unsigned long long>(s.checkAhead),
                    static_cast<unsigned long long>(s.checkNotCurrent), static_cast<unsigned long long>(s.checkOutside), s.mismatches,
                    c.mismatchesPristine, c.mismatchesModified, s.repairsInstalled, c.entityChecks, c.entityChecksPartial, c.entityChecksSkipped,
                    c.entityChecksAhead, c.entityMismatches, c.entityLists, c.copiesRestored, c.copiesReplaced, c.copiesDropped,
                    c.checksOutOfOrder, c.maxCheckSeconds * 1000.0);
    }

    /// No fault was injected: the checks must find nothing (their false positives are 7b's main risk).
    void ExpectNoFalseChecks(const Field::Bot& bot, uint32_t minHashChecks, uint32_t minEntityChecks) {
        const net::ClientStats& c = bot.client->Stats();
        EXPECT_EQ(bot.client->Sync().Stats().mismatches, 0u);
        EXPECT_EQ(c.entityMismatches, 0u);
        EXPECT_EQ(c.checksOutOfOrder, 0u);
        EXPECT_EQ(bot.client->Sync().Stats().checksOutOfOrder, 0u);
        EXPECT_GE(c.hashChecks, minHashChecks);
        EXPECT_GE(c.entityChecks + c.entityChecksPartial, minEntityChecks);
    }

    /// The server's player and the bot's prediction of it, once both stand.
    void ExpectOwnPosition(const Field& f, const Field::Bot& bot) {
        const std::optional<sim::PlayerState> server = f.server.GetPlayer(bot.client->Slot());
        ASSERT_TRUE(server.has_value());
        EXPECT_LT(glm::distance(server->position, bot.client->Player().position), 0.05f);
    }

    /// The drill's site at grid cell (x, z) of the 250 m region (20 x 20 sites of 12.5 m).
    uint32_t SiteAt(int x, int z) { return static_cast<uint32_t>(z * 20 + x); }
}

// The M1 fight on 250 m (networking.md "Testing": the scripted fight, re-run with the checks): two
// bots, one presenting and wandering with crates, one walking 90 m east so the west sites leave its
// window; drill blasts every 6 ticks (10 casts/s) on four sites about 87 m apart (islands, face-flood
// pieces, neck releases, crates); 5 % loss, 80 + 0-30 ms, a 200 ms outage. HashChecks and EntityChecks
// run all along: none may report a mismatch or come out of its stream position, and each bot compares
// at least 4 of each kind; then the usual end state, own positions within 5 cm.
TEST(Region250, FightWithIslandsAndCratesChecksFindNothing) {
    net::Impairment im = Lossy();
    im.delaySeconds = 0.08;
    im.outages.push_back({6.0, 6.2});
    Field f(Settings(Plain250()), im);
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    const sim::RegionDesc& region = f.server.Region();
    net::PilotSettings wander;
    wander.crateEveryTicks = 120;
    auto pilot = std::make_shared<net::BotPilot>(region, 11, wander);
    Field::Bot& a = f.AddBot([pilot](Field::Bot& bot) { return pilot->Next(bot.client->Player()); }, GameClient(true));
    std::shared_ptr<net::BotPilot> walker;
    Field::Bot& b = f.AddBot(Walk(region, {{90.0f, 2.0f}}, &walker), GameClient());
    ASSERT_TRUE(f.Join());
    const net::BlastDrill drill(region, DefaultIds(), kGround);
    ASSERT_EQ(drill.Sites(), 400u);
    const std::array<uint32_t, 4> sites = {SiteAt(6, 6), SiteAt(13, 6), SiteAt(6, 13), SiteAt(13, 13)};
    for (uint32_t step = 0; step < 100; ++step) {
        // Site s, step k there: index s + 400 k cycles the three kinds per site.
        ASSERT_TRUE(net::RunDrillStep(f.server, drill.Step(sites[step % 4] + 400u * (step / 4))));
        for (int t = 0; t < 6; ++t) f.Pass();
        ASSERT_FALSE(f.server.Failed());
    }
    // Some more checks while the bodies settle.
    f.Passes(240);
    ASSERT_TRUE(f.Settle());
    PrintChecks("fight A", a);
    PrintChecks("fight B", b);
    const net::ServerStats& st = f.server.Stats();
    std::printf("[ checks ] server: %u hash checks (%llu recent, %llu modified, %llu pristine entries), %u entity checks, %u lists; %u repairs "
                "requested; checks %.3f ms max per peer and tick; %u pieces as server bodies, %u crates\n",
                st.hashChecksSent, static_cast<unsigned long long>(st.hashCheckEntries[0]), static_cast<unsigned long long>(st.hashCheckEntries[1]),
                static_cast<unsigned long long>(st.hashCheckEntries[2]), st.entityChecksSent, st.entityListsSent, st.repairsRequested,
                st.maxCheckSeconds * 1000.0, st.detachBodies, st.crates);
    EXPECT_GT(st.detachBodies, 0u);
    EXPECT_GT(st.hashCheckEntries[1], 0u);
    EXPECT_GT(st.hashCheckEntries[2], 0u);
    EXPECT_GT(b.client->Stats().modelRequests, 0u) << "no piece broke off outside B's window";
    for (const Field::Bot* bot : {&a, &b}) {
        f.ExpectSameWorld(*bot);
        ExpectNoFalseChecks(*bot, 4, 4);
        ExpectOwnPosition(f, *bot);
    }
    EXPECT_EQ(st.repairsRequested, 0u);
    EXPECT_EQ(st.entityListRequests, 0u);
}

// networking.md "Testing": generator broken in one non-probe chunk (stage 7b, amendment 5). The
// walker's generator changes 64 voxels of the surface chunk C 30 m east on its line; C is pristine on
// the server. A HashCheck must pick C on the way ahead, the client asks for it (Repair) and installs
// the server's chunk BEFORE the walker's reach box first touches it - at walk and at run speed, and
// whatever the phase of the 2 s checks against the walk (start offsets 0, 40, 80 ticks). The player is
// never held; one pristine failure, no terrain mode.
TEST(Region250, AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt) {
    for (const bool run : {false, true})
        for (const int offset : {0, 40, 80}) {
            SCOPED_TRACE(std::string(run ? "run" : "walk") + ", offset " + std::to_string(offset));
            Field f(Settings(Plain250()), Lossy());
            ASSERT_TRUE(f.started);
            f.storeLimit = kGameStore;
            const sim::RegionDesc& region = f.server.Region();
            const glm::dvec3 spawn = f.server.Template().spawn;
            const glm::ivec3 spawnChunk = ChunkAt(spawn.x, spawn.y - 0.5, spawn.z);
            const float lineZ = (static_cast<float>(spawnChunk.z) + 0.5f) * 4.0f;    // the middle of the chunk row
            const glm::ivec3 c = ChunkAt(spawn.x + 30.0, kGroundMetres - 0.5, lineZ);
            ASSERT_EQ(f.server.Simulation().World().StateOf(c), voxel::CellState::Dense);
            for (const net::ProbeHash& p : f.server.Probes()) ASSERT_NE(p.chunk, c) << "the broken chunk is a probe";
            net::ClientSettings cs = GameClient();
            cs.wrapGenerator = [c](std::shared_ptr<voxel::IWorldGenerator> g) { return net::BreakChunks(std::move(g), {c}, DefaultIds().stone); };
            net::PilotSettings ps;
            ps.waypoints = {{static_cast<float>(spawn.x), lineZ}, {static_cast<float>(spawn.x) + 45.0f, lineZ}};
            ps.jumps = false;
            ps.run = run;
            ps.stuckTicks = 600;
            auto pilot = std::make_shared<net::BotPilot>(region, 5, ps);
            int ticks = 0;
            Field::Bot& bot = f.AddBot(
                [pilot, &ticks, offset](Field::Bot& b) { return ticks < offset ? sim::PlayerInput{} : pilot->Next(b.client->Player()); }, cs);
            ASSERT_TRUE(f.Join());
            ASSERT_FALSE(bot.client->Sync().FromData(c));
            // Per tick: when C is the server's again, when the reach box first touches C.
            int repairedAt = -1, reachedAt = -1;
            const math::AABB cBox(glm::vec3(voxel::ChunkToVoxel(c)) * 0.125f, glm::vec3(voxel::ChunkToVoxel(c + glm::ivec3(1))) * 0.125f);
            bot.afterTick = [&](Field::Bot& b) {
                ++ticks;
                if (repairedAt < 0 && b.client->Sync().FromData(c) && b.client->Sync().IsCurrent(c)) repairedAt = ticks;
                if (reachedAt < 0 && b.client->Predicting()) {
                    const math::AABB reach = sim::PlayerReach(b.client->Player().position, b.client->Tuning());
                    if (glm::all(glm::lessThan(reach.min, cBox.max)) && glm::all(glm::greaterThan(reach.max, cBox.min))) reachedAt = ticks;
                }
            };
            for (int pass = 0; pass < 3000 && !pilot->Done(); ++pass) f.Pass();
            EXPECT_TRUE(pilot->Done());
            ASSERT_TRUE(f.Drain());
            PrintChecks(run ? "broken chunk, run" : "broken chunk, walk", bot);
            std::printf("[ checks ] offset %d: C repaired at tick %d, reached at tick %d (%.2f s before)\n", offset, repairedAt, reachedAt,
                        static_cast<double>(reachedAt - repairedAt) / 60.0);
            EXPECT_GE(repairedAt, 0) << "C was never repaired";
            EXPECT_GE(reachedAt, 0) << "the walker never reached C";
            EXPECT_LT(repairedAt, reachedAt) << "repaired after the walker reached it";
            const net::ClientStats& s = bot.client->Stats();
            EXPECT_EQ(s.mismatchesPristine, 1u);
            EXPECT_EQ(s.pristineFailures, 1u);
            EXPECT_EQ(s.mismatchesModified, 0u);
            EXPECT_EQ(bot.client->Prediction().syncHeldTicks, 0u);
            EXPECT_EQ(s.checksOutOfOrder, 0u);
            EXPECT_EQ(f.server.Stats().repairsSent, 1u);
            f.ExpectSameWorld(bot);
        }
}

// networking.md "Testing" (stage 7b): a corrupted chunk and a lost copy on a client are repaired. After
// drill blasts beside two standing bots, the test writes gold into the chunk the last operation changed
// on bot B's world (a local edit the server never made) and destroys one of B's resting copies. Within
// two checks (4 s) the chunk is asked for (Repair) and installed, and the copy is asked for and made
// again; then the usual end state.
TEST(Region250, ACorruptedChunkAndALostCopyAreRepaired) {
    Field f(Settings(Plain250()), Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    const sim::RegionDesc& region = f.server.Region();
    Field::Bot& a = f.AddBot(Stand, GameClient(true));
    Field::Bot& b = f.AddBot(Stand, GameClient());
    ASSERT_TRUE(f.Join());
    const net::BlastDrill drill(region, DefaultIds(), kGround);
    for (uint32_t k = 0; k < 3; ++k) {
        ASSERT_TRUE(net::RunDrillStep(f.server, drill.Step(SiteAt(8, 8) + 400u * k)));
        for (int t = 0; t < 6; ++t) f.Pass();
    }
    ASSERT_TRUE(f.Settle());
    ExpectNoFalseChecks(b, 1, 1);
    const std::vector<std::pair<uint32_t, glm::ivec3>> recent = f.server.RecentChanged();
    ASSERT_FALSE(recent.empty());
    const glm::ivec3 chunk = recent.back().second;
    ASSERT_TRUE(b.client->Sync().IsCurrent(chunk));
    ASSERT_FALSE(b.client->Sync().FromData(chunk));
    const glm::ivec3 v = voxel::ChunkToVoxel(chunk);
    ASSERT_GT(b.simulation->World().FillBox(v + glm::ivec3(2), v + glm::ivec3(30), DefaultIds().gold), 0u);
    uint32_t lost = 0;
    for (const net::EntityInfo& e : b.client->Entities())
        if (e.resting) {
            lost = e.netId;
            break;
        }
    ASSERT_NE(lost, 0u);
    ASSERT_TRUE(b.client->TestDestroyCopy(lost));
    int passes = 0;
    while (passes < 300 && !(b.client->Sync().FromData(chunk) && b.client->Sync().IsCurrent(chunk) && b.client->Stats().copiesRestored > 0 &&
                             b.client->Synced())) {
        f.Pass();
        ++passes;
    }
    EXPECT_LT(passes, 300) << "not repaired within two checks";
    ASSERT_TRUE(f.Settle());
    PrintChecks("corrupted A", a);
    PrintChecks("corrupted B", b);
    std::printf("[ checks ] the chunk and the copy repaired after %d passes\n", passes);
    const net::ClientStats& s = b.client->Stats();
    EXPECT_EQ(b.client->Sync().Stats().mismatches, 1u);
    EXPECT_EQ(s.mismatchesModified, 1u);
    EXPECT_EQ(b.client->Sync().Stats().repairsInstalled, 1u);
    EXPECT_GE(s.entityMismatches, 1u);
    EXPECT_EQ(s.copiesRestored, 1u);
    EXPECT_EQ(s.copiesDropped, 0u);
    EXPECT_EQ(s.checksOutOfOrder, 0u);
    ExpectNoFalseChecks(a, 1, 1);
    f.ExpectSameWorld(a);
    f.ExpectSameWorld(b);
}

// networking.md "Testing" (stage 7b): an EntityCheck mismatch re-sends the list and it agrees. Bot B
// drops one EntityDespawn (an extra copy), one ObjectModel (a copy with the wrong model) and loses one
// copy; the list drops, replaces (asks for the body) and restores (asks for it): agreement. With the
// entity checks off the same faults stay (asserted, so the test can fail).
TEST(Region250, AnEntityCheckMismatchResendsTheListAndItAgrees) {
    for (const bool checks : {true, false}) {
        SCOPED_TRACE(checks ? "checks on" : "checks off");
        net::ServerSettings settings = Settings(Plain250());
        if (!checks) settings.entityCheckTicks = 0;
        Field f(settings, Lossy());
        ASSERT_TRUE(f.started);
        f.storeLimit = kGameStore;
        struct Faults {
            bool armed = false, model = false, despawn = false;
        };
        auto faults = std::make_shared<Faults>();
        net::ClientSettings cs = GameClient();
        cs.testDropEntityEvent = [faults](uint32_t, const net::Event& e) {
            if (!faults->armed) return false;
            if (std::holds_alternative<net::ObjectModel>(e) && !faults->model) return faults->model = true;
            if (std::holds_alternative<net::EntityDespawn>(e) && !faults->despawn) return faults->despawn = true;
            return false;
        };
        Field::Bot& a = f.AddBot(Stand, GameClient(true));
        Field::Bot& b = f.AddBot(Stand, cs);
        ASSERT_TRUE(f.Join());
        // Three 10^3 stone cubes (1000 voxels: a hit leaves more than the replicated size) 6 m east.
        voxel::VoxelVolume volume{glm::ivec3(10)};
        volume.FillBox(glm::ivec3(0), glm::ivec3(10), DefaultIds().stone);
        auto model = std::make_shared<voxel::VoxelModel>(volume, voxel::kWorldPalette, "cube");
        const glm::dvec3 spawn = f.server.Template().spawn;
        std::array<uint32_t, 3> ids{};
        for (int i = 0; i < 3; ++i) {
            ids[i] = f.server.SpawnObject(model, glm::dvec3(spawn.x + 6.0, kGroundMetres + 0.25, spawn.z + 4.0 * (i - 1)));
            ASSERT_NE(ids[i], 0u);
        }
        ASSERT_TRUE(f.Settle());
        faults->armed = true;
        ASSERT_TRUE(b.client->TestDestroyCopy(ids[0]));
        // A small blast at the top corner of the second cube: it survives with a new model (ObjectModel).
        const glm::dvec3 corner(spawn.x + 6.0, kGroundMetres + 0.25 + 1.25, spawn.z);
        ASSERT_TRUE(f.server.QueueCast(sim::BlastDesc{corner, 0.4f, 6.0f}));
        f.Passes(30);
        // The third goes (EntityDespawn).
        ASSERT_EQ(f.server.ClearBodies(glm::dvec3(spawn.x + 5.5, kGroundMetres - 1.0, spawn.z + 3.5),
                                       glm::dvec3(spawn.x + 7.75, kGroundMetres + 2.0, spawn.z + 5.75)),
                  1u);
        f.Passes(600);
        ASSERT_TRUE(f.Settle());
        EXPECT_TRUE(faults->model) << "no ObjectModel to drop";
        EXPECT_TRUE(faults->despawn) << "no EntityDespawn to drop";
        PrintChecks(checks ? "entity faults, checks on" : "entity faults, checks off", b);
        const net::ClientStats& s = b.client->Stats();
        const net::EntityComparison c = net::CompareEntities(f.server.Entities(), b.client->Entities());
        if (checks) {
            EXPECT_GE(s.entityMismatches, 1u);
            EXPECT_GE(s.entityLists, 1u);
            EXPECT_LE(s.entityLists, 3u);
            EXPECT_GE(s.copiesDropped, 1u);
            EXPECT_GE(s.copiesReplaced, 1u);
            EXPECT_GE(s.copiesRestored, 1u);
            EXPECT_TRUE(c.Agree(0.05f)) << c.Describe();
            f.ExpectSameWorld(b);
        } else {
            EXPECT_EQ(s.entityChecks + s.entityChecksPartial, 0u);
            EXPECT_FALSE(c.Agree(0.05f)) << "the faults left no trace without the checks: the test could not fail";
        }
        ExpectNoFalseChecks(a, 1, checks ? 1u : 0u);
        f.ExpectSameWorld(a);
        EXPECT_EQ(s.checksOutOfOrder, 0u);
    }
}

// --- milestone M-b: server terrain mode and the rest of the table on 250 m ---------------------------

// networking.md "Testing": generator broken on a client everywhere -> server terrain mode, equal
// hashes (stage 7b M-b). Bot B's build makes its ground 1.5 m higher everywhere: its probe chunks differ
// at the Welcome, so its first WindowUpdate already asks for server terrain mode and it never
// generates a chunk of the server's window; it runs 120 m east past craters made before and during
// its walk, its window stepping all the way. Bot A (the region's generator) stands. Both end with the
// server's world over their sync boxes, their objects agreeing and no check finding anything; B's
// store stays under the game's headroom gate and no chunk ever found no slot. Bytes per join and per
// window step are printed.
TEST(Region250, AGeneratorBrokenEverywhereJoinsInServerTerrainMode) {
    Field f(Settings(Plain250()), Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    const sim::RegionDesc& region = f.server.Region();
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(f.Crater(25.0 + 10.0 * i, 1.0));
    f.Passes(30);
    Field::Bot& a = f.AddBot(Stand, GameClient(true));
    net::ClientSettings cs = GameClient();
    cs.wrapGenerator = OtherGround(region, kGround + 12);
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& b = f.AddBot(Walk(region, {{120.0f, 0.0f}}, &pilot), cs);
    ASSERT_TRUE(f.Join());
    EXPECT_TRUE(b.client->TerrainMode());
    EXPECT_EQ(b.client->TerrainModeWhy(), net::TerrainModeReason::Probes);
    EXPECT_GT(b.client->Stats().probeMismatches, 0u);
    EXPECT_FALSE(a.client->TerrainMode());
    for (int pass = 0; pass < 3000 && !pilot->Done(); ++pass) {
        f.Pass();
        if (const double x = static_cast<double>(b.client->Player().position.x) + 25.0; pass % 300 == 150 && x < 120.0)
            ASSERT_TRUE(f.Crater(x, 1.0));
    }
    EXPECT_TRUE(pilot->Done());
    ASSERT_TRUE(f.Settle());
    PrintChecks("broken everywhere A", a);
    PrintChecks("broken everywhere B", b);
    PrintTerrain("broken everywhere B", f, b);
    const std::shared_ptr<const net::SessionGenerator> g = b.client->ClientGenerator();
    ASSERT_TRUE(g);
    EXPECT_GT(g->Skipped(), 0u);
    EXPECT_EQ(g->DenseInsideInTerrainMode(), 0u) << "a chunk of the server's window was generated here";
    EXPECT_EQ(f.server.Stats().terrainModePeers, 1u);
    EXPECT_GT(b.client->Stats().terrainAcks, 10u) << "the window stepped in terrain mode";
    EXPECT_EQ(b.client->Stats().windowResets, 0u);
    EXPECT_EQ(b.client->Sync().Stats().unsynced, 0u);
    EXPECT_LT(b.client->Stats().maxStoreUsed, kGameStore - kGameHeadroom);
    EXPECT_GT(b.client->Stats().terrainChunkData, 0u);
    EXPECT_EQ(b.client->Stats().pristineFailures, 0u);
    std::printf("[ terrain ] B: store at most %u slots, held %llu ticks, x %.1f m\n", b.client->Stats().maxStoreUsed,
                static_cast<unsigned long long>(b.client->Prediction().syncHeldTicks), static_cast<double>(b.client->Player().position.x));
    for (const Field::Bot* bot : {&a, &b}) {
        f.ExpectSameWorld(*bot);
        ExpectNoFalseChecks(*bot, 4, 4);
    }
}

// networking.md "Testing": generator broken in some pristine chunks -> after 3 of them, server terrain
// mode (stage 7b M-b, the second trigger). B's generator makes every surface chunk east of x = 40 m
// near its line differently, but no probe chunk (so the Welcome finds nothing). Walking east, its
// HashChecks find pristine chunks it generated differently; at the third it switches: one reset, the
// whole window asked for again in terrain mode, the player held while its near data comes; it ends
// with the server's world over its sync box.
TEST(Region250, ThreePristineChunksSwitchToServerTerrainMode) {
    Field f(Settings(Plain250()), Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    const sim::RegionDesc& region = f.server.Region();
    std::vector<glm::ivec3> broken;
    for (int x = 10; x < 32; ++x)
        for (int z = -8; z <= 8; ++z) {
            const glm::ivec3 c(x, 2, z);
            if (std::none_of(f.server.Probes().begin(), f.server.Probes().end(), [&](const net::ProbeHash& p) { return p.chunk == c; }))
                broken.push_back(c);
        }
    net::ClientSettings cs = GameClient();
    cs.wrapGenerator = [broken](std::shared_ptr<voxel::IWorldGenerator> g) { return net::BreakChunks(std::move(g), broken, DefaultIds().stone); };
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& b = f.AddBot(Walk(region, {{100.0f, 0.0f}}, &pilot, false), cs);
    ASSERT_TRUE(f.Join());
    EXPECT_FALSE(b.client->TerrainMode()) << "the probes are not broken";
    EXPECT_EQ(b.client->Stats().probeMismatches, 0u);
    for (int pass = 0; pass < 4000 && !pilot->Done(); ++pass) f.Pass();
    EXPECT_TRUE(pilot->Done());
    ASSERT_TRUE(f.Settle());
    PrintChecks("three pristine", b);
    PrintTerrain("three pristine", f, b);
    const net::ClientStats& s = b.client->Stats();
    EXPECT_TRUE(b.client->TerrainMode());
    EXPECT_EQ(b.client->TerrainModeWhy(), net::TerrainModeReason::PristineChecks);
    EXPECT_GE(s.pristineFailures, 3u);
    EXPECT_GT(s.terrainModeTick, 0u);
    EXPECT_EQ(s.windowResets, 1u) << "one reset: the switch";
    EXPECT_EQ(f.server.Stats().terrainModePeers, 1u);
    EXPECT_EQ(s.mismatchesModified, 0u);
    EXPECT_EQ(s.entityMismatches, 0u);
    EXPECT_EQ(s.checksOutOfOrder, 0u);
    EXPECT_EQ(b.client->Sync().Stats().unsynced, 0u);
    std::printf("[ terrain ] switched at tick %u after %u pristine failures; held %llu ticks; x %.1f m\n", s.terrainModeTick, s.pristineFailures,
                static_cast<unsigned long long>(b.client->Prediction().syncHeldTicks), static_cast<double>(b.client->Player().position.x));
    f.ExpectSameWorld(b);
}

namespace {
    struct FireRun {
        uint32_t p99A = 0, p99B = 0, p99C = 0;   ///< event latency p99 (ticks from made to applied) of the 600 ticks after the joins
        bool converged = false;
        bool terrainC = false;
        uint64_t bulkB = 0, bulkC = 0;
        uint32_t syncB = 0, syncC = 0;    ///< ticks from the joins to Synced (0 = not within the run)
        std::string why;
    };

    uint32_t P99(std::vector<uint32_t> v) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(0.99 * static_cast<double>(v.size()))) - 1)];
    }

    /// The drill at 10 casts/s beside bot A (standing at the spawn) on Plain250; after 40 steps bot B
    /// (the region's generator) and / or bot C (another build's ground: server terrain mode) join;
    /// latency of the events made in the 600 ticks after that, then the settle and the comparison.
    FireRun Fire(const net::Impairment& im, double bulkCap, bool joinB, bool joinC, uint64_t seed = 0) {
        net::ServerSettings settings = Settings(Plain250());
        settings.maxBulkBytesPerSecond = bulkCap;
        Field f(settings, im, seed);
        FireRun run;
        if (!f.started) return run;
        f.storeLimit = kGameStore;
        Field::Bot& a = f.AddBot(Stand, GameClient());
        if (!f.Join()) return run;
        const net::BlastDrill drill(f.server.Region(), DefaultIds(), kGround);
        std::vector<uint32_t> madeAt;     // [opSeq - 1] -> server tick
        std::vector<uint32_t> lat[3];
        std::array<Field::Bot*, 3> bots = {&a, nullptr, nullptr};
        std::array<uint32_t, 3> seen = {a.client->AppliedOpSeq(), 0, 0};
        uint32_t joinTick = 0;
        const auto note = [&] {
            while (madeAt.size() < f.server.LastOpSeq()) madeAt.push_back(f.server.TickNumber());
        };
        note();
        for (uint32_t step = 0; step < 140; ++step) {
            if (step == 40) {
                joinTick = f.server.TickNumber();
                if (joinB) bots[1] = &f.AddBot(Stand, GameClient());
                if (joinC) {
                    net::ClientSettings cs = GameClient();
                    cs.wrapGenerator = OtherGround(f.server.Region(), kGround + 12);
                    bots[2] = &f.AddBot(Stand, cs);
                }
            }
            if (!net::RunDrillStep(f.server, drill.Step(SiteAt(10, 10) + 400u * step))) return run;
            note();
            for (int t = 0; t < 6; ++t) {
                f.Pass();
                note();
                const uint32_t tick = f.server.TickNumber();
                for (size_t i = 0; i < 3; ++i) {
                    Field::Bot* bot = bots[i];
                    if (!bot || !bot->simulation) continue;
                    if (seen[i] == 0) seen[i] = bot->client->JoinOpSeq();
                    for (; seen[i] < bot->client->AppliedOpSeq(); ++seen[i])
                        if (joinTick != 0 && madeAt[seen[i]] >= joinTick && madeAt[seen[i]] < joinTick + 600) lat[i].push_back(tick - madeAt[seen[i]]);
                    if (i == 1 && run.syncB == 0 && bot->client->Synced()) run.syncB = tick - joinTick;
                    if (i == 2 && run.syncC == 0 && bot->client->Synced()) run.syncC = tick - joinTick;
                }
            }
        }
        if (!f.Settle(7200)) {
            run.why = "not settled";
            return run;
        }
        run.converged = true;
        for (Field::Bot* bot : bots) {
            if (!bot) continue;
            const voxel::WorldHashResult c = bot->client->Hash(), s = f.server.Hash(bot->client->SyncWindow());
            const net::EntityComparison e = net::CompareEntities(f.server.Entities(), bot->client->Entities());
            const bool ok = c.Complete() && c.hash == s.hash && e.Agree(0.05f) && bot->client->Sync().Stats().mismatches == 0 &&
                            bot->client->Stats().entityMismatches == 0 && bot->client->Stats().checksOutOfOrder == 0;
            if (!ok) run.why += "slot " + std::to_string(bot->client->Slot()) + ": " + e.Describe() + "; ";
            run.converged = run.converged && ok;
        }
        run.p99A = P99(lat[0]);
        run.p99B = P99(lat[1]);
        run.p99C = P99(lat[2]);
        if (bots[1]) run.bulkB = bots[1]->client->Stats().bulkBytes;
        if (bots[2]) {
            run.bulkC = bots[2]->client->Stats().bulkBytes;
            run.terrainC = bots[2]->client->TerrainMode();
            PrintTerrain("late joiner C", f, *bots[2]);
        }
        return run;
    }
}

// networking.md "Testing": late join during 10 casts/s and object splits (stage 7a's test on 250 m,
// with a terrain-mode joiner). Per-peer links of 128 kB/s, 1 % loss, bulk capped at 32 kB/s: the
// event latency p99 of the bot playing and of the joiners stays under 15 ticks (250 ms) while B (its
// own terrain) and C (server terrain mode: the whole window on bulk) join, and all three converge.
// A second pair of runs with one shared server uplink: a joiner that generates its own terrain, and the
// same join in terrain mode (megabytes of bulk instead of kilobytes): the pacer keeps the playing bot's
// p99 within 6 ticks (100 ms) of the first run's - compared as medians over three seeds of the links'
// draws (review of stage 7b: one seed's p99 moves by a few ticks when any message changes the draws),
// and never above 30 ticks in any run (an unpaced terrain-mode join was measured at 75).
TEST(Region250, LateJoinDuring10CastsPerSecondConverges) {
    constexpr double kSharedUplink = 96.0 * 1024.0;
    net::Impairment perPeer = Lossy();
    perPeer.loss = 0.01;
    perPeer.peerBytesPerSecond = 128.0 * 1024.0;
    perPeer.rngPerPeer = true;
    const FireRun wide = Fire(perPeer, 32768.0, true, true);
    std::printf("[ late join ] 128 kB/s per peer, 1 %% loss: p99 A %u / B %u / C %u ticks; B synced after %u ticks (%.1f KiB), C (terrain "
                "mode) after %u ticks (%.1f KiB)\n",
                wide.p99A, wide.p99B, wide.p99C, wide.syncB, static_cast<double>(wide.bulkB) / 1024.0, wide.syncC,
                static_cast<double>(wide.bulkC) / 1024.0);
    EXPECT_TRUE(wide.converged) << wide.why;
    EXPECT_TRUE(wide.terrainC);
    EXPECT_LT(wide.p99A, 15u);
    EXPECT_LT(wide.p99B, 15u);
    EXPECT_LT(wide.p99C, 15u);

    net::Impairment shared = Lossy();
    shared.loss = 0.01;
    shared.bytesPerSecond = kSharedUplink;
    shared.rngPerPeer = true;
    std::vector<uint32_t> ownP99, crowdedP99;
    for (const uint64_t seed : {0u, 1u, 2u}) {
        const FireRun own = Fire(shared, 0.0, true, false, seed);
        const FireRun crowded = Fire(shared, 0.0, false, true, seed);
        std::printf("[ late join ] one shared %.0f kB/s uplink, seed %llu: p99 A %u ticks while B joins with its own terrain (B %u, %.1f KiB), "
                    "%u while C joins in terrain mode (C %u ticks, synced after %u ticks, %.1f KiB)\n",
                    kSharedUplink / 1024.0, static_cast<unsigned long long>(seed), own.p99A, own.p99B, static_cast<double>(own.bulkB) / 1024.0,
                    crowded.p99A, crowded.p99C, crowded.syncC, static_cast<double>(crowded.bulkC) / 1024.0);
        EXPECT_TRUE(own.converged) << own.why;
        EXPECT_TRUE(crowded.converged) << crowded.why;
        EXPECT_TRUE(crowded.terrainC);
        EXPECT_LT(crowded.p99A, 30u) << "seed " << seed;
        ownP99.push_back(own.p99A);
        crowdedP99.push_back(crowded.p99A);
    }
    std::sort(ownP99.begin(), ownP99.end());
    std::sort(crowdedP99.begin(), crowdedP99.end());
    std::printf("[ late join ] medians over 3 seeds: p99 A %u ticks (own terrain joiner), %u (terrain-mode joiner)\n", ownP99[1], crowdedP99[1]);
    EXPECT_LE(crowdedP99[1], ownP99[1] + 6);
}

// networking.md "Testing": walk in from outside (stage 7a's test on 250 m). The walker goes to the
// west edge first; then 30 craters are made near the east edge (outside its window); it runs 230 m
// east into them: every window step names its entering chunks, the modified ones come as data, no
// reset, the player is never held at run speed, the store stays under the game's gate.
TEST(Region250, WalkInFromOutsideIntoChunksModifiedBefore) {
    net::ServerSettings settings = Settings(Plain250());
    Field f(settings, Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    const sim::RegionDesc& region = f.server.Region();
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& bot = f.AddBot(Walk(region, {{-110.0f, 0.0f}, {115.0f, 0.0f}}, &pilot), GameClient());
    ASSERT_TRUE(f.Join());
    for (int pass = 0; pass < 2000 && bot.client->Player().position.x > -100.0f; ++pass) f.Pass();
    ASSERT_LT(bot.client->Player().position.x, -100.0f);
    const uint32_t dataBefore = bot.client->Stats().chunkData;
    for (int i = 0; i < 30; ++i) ASSERT_TRUE(f.Crater(85.0 + 1.2 * i, -8.0 + 0.55 * i));
    for (int pass = 0; pass < 4000 && !pilot->Done(); ++pass) f.Pass();
    EXPECT_TRUE(pilot->Done());
    ASSERT_TRUE(f.Settle());
    PrintChecks("walk in", bot);
    const net::ClientStats& s = bot.client->Stats();
    std::printf("[ walk in ] %u window updates, %u resets, %u ChunkData after the craters, held %llu ticks, store at most %u slots\n", s.windowUpdates,
                s.windowResets, s.chunkData - dataBefore, static_cast<unsigned long long>(bot.client->Prediction().syncHeldTicks), s.maxStoreUsed);
    EXPECT_GT(s.windowUpdates, 40u);
    EXPECT_EQ(s.windowResets, 0u);
    EXPECT_GT(s.chunkData, dataBefore) << "the craters' chunks came as data";
    EXPECT_EQ(bot.client->Prediction().syncHeldTicks, 0u);
    EXPECT_LT(s.maxStoreUsed, kGameStore - kGameHeadroom);
    f.ExpectSameWorld(bot);
    ExpectNoFalseChecks(bot, 8, 8);
}

namespace {
    /// A generator that holds the chunks of some columns (generation blocks) until released: chunks
    /// that load late while the window moves (stage 7b M-b).
    class HeldColumns final : public voxel::IWorldGenerator {
    public:
        explicit HeldColumns(std::shared_ptr<voxel::IWorldGenerator> inner) : m_Inner(std::move(inner)) {}
        voxel::ChunkContent Generate(const glm::ivec3& c, voxel::Chunk& chunk) const override {
            std::unique_lock lock(m_Mutex);
            m_Cv.wait(lock, [&] { return !m_Holding || c.x < m_MinX || c.x > m_MaxX; });
            lock.unlock();
            return m_Inner->Generate(c, chunk);
        }
        voxel::HeightBounds VerticalBounds() const override { return m_Inner->VerticalBounds(); }
        voxel::ColumnClass ClassifyColumn(int cx, int cz) const override { return m_Inner->ClassifyColumn(cx, cz); }
        void Hold(int minX, int maxX) {
            std::scoped_lock lock(m_Mutex);
            m_Holding = true;
            m_MinX = minX;
            m_MaxX = maxX;
        }
        void Release() {
            {
                std::scoped_lock lock(m_Mutex);
                m_Holding = false;
            }
            m_Cv.notify_all();
        }

    private:
        std::shared_ptr<voxel::IWorldGenerator> m_Inner;
        mutable std::mutex m_Mutex;
        mutable std::condition_variable m_Cv;
        bool m_Holding = false;
        int m_MinX = 0, m_MaxX = -1;
    };
    struct ReleaseHeld {
        HeldColumns* generator = nullptr;
        ~ReleaseHeld() {
            if (generator) generator->Release();
        }
    };
}

namespace {
    /// A generator that holds one chunk's generation until released (review of stage 7b: a chunk
    /// still loading when the server first picks it for a HashCheck). Only one job thread waits.
    class HeldChunk final : public voxel::IWorldGenerator {
    public:
        HeldChunk(std::shared_ptr<voxel::IWorldGenerator> inner, const glm::ivec3& chunk) : m_Inner(std::move(inner)), m_Chunk(chunk) {}
        voxel::ChunkContent Generate(const glm::ivec3& c, voxel::Chunk& chunk) const override {
            std::unique_lock lock(m_Mutex);
            m_Cv.wait(lock, [&] { return !m_Holding || c != m_Chunk; });
            lock.unlock();
            return m_Inner->Generate(c, chunk);
        }
        voxel::HeightBounds VerticalBounds() const override { return m_Inner->VerticalBounds(); }
        voxel::ColumnClass ClassifyColumn(int cx, int cz) const override { return m_Inner->ClassifyColumn(cx, cz); }
        void Hold() {
            std::scoped_lock lock(m_Mutex);
            m_Holding = true;
        }
        void Release() {
            {
                std::scoped_lock lock(m_Mutex);
                m_Holding = false;
            }
            m_Cv.notify_all();
        }

    private:
        std::shared_ptr<voxel::IWorldGenerator> m_Inner;
        glm::ivec3 m_Chunk;
        mutable std::mutex m_Mutex;
        mutable std::condition_variable m_Cv;
        bool m_Holding = false;
    };
    struct ReleaseHeldChunk {
        HeldChunk* generator = nullptr;
        ~ReleaseHeldChunk() {
            if (generator) generator->Release();
        }
    };
}

// Review of stage 7b (finding 7): the server's planner counts a pick as checked whether or not the
// client could compare it, and ranks never-checked chunks first. A broken surface chunk C 110 m east
// on the walker's line (outside its window at the spawn) is held loading on the client from when it
// enters the window until the client has processed the HashCheck that first picked it (skipped there
// as not current). C must still be found and repaired before the walker's reach box touches it - at
// walk and at run speed. (Without the client's deferred compare C is never picked again on the way:
// the never-checked chunks entering the tube fill the check every 2 s.)
TEST(Region250, AChunkPickedWhileItWasStillLoadingIsRepairedBeforeThePlayerReachesIt) {
    for (const bool run : {false, true}) {
        SCOPED_TRACE(run ? "run" : "walk");
        Field f(Settings(Plain250()), Lossy());
        ASSERT_TRUE(f.started);
        f.storeLimit = kGameStore;
        const sim::RegionDesc& region = f.server.Region();
        const glm::dvec3 spawn = f.server.Template().spawn;
        const glm::ivec3 spawnChunk = ChunkAt(spawn.x, spawn.y - 0.5, spawn.z);
        const float lineZ = (static_cast<float>(spawnChunk.z) + 0.5f) * 4.0f;
        const glm::ivec3 c = ChunkAt(spawn.x + 110.0, kGroundMetres - 0.5, lineZ);
        ASSERT_EQ(f.server.Simulation().World().StateOf(c), voxel::CellState::Dense);
        for (const net::ProbeHash& p : f.server.Probes()) ASSERT_NE(p.chunk, c) << "the broken chunk is a probe";
        std::shared_ptr<HeldChunk> held;
        net::ClientSettings cs = GameClient();
        cs.wrapGenerator = [c, &held](std::shared_ptr<voxel::IWorldGenerator> g) {
            held = std::make_shared<HeldChunk>(net::BreakChunks(std::move(g), {c}, DefaultIds().stone), c);
            return held;
        };
        net::PilotSettings ps;
        ps.waypoints = {{static_cast<float>(spawn.x), lineZ}, {static_cast<float>(spawn.x) + 122.0f, lineZ}};
        ps.jumps = false;
        ps.run = run;
        ps.stuckTicks = 600;
        auto pilot = std::make_shared<net::BotPilot>(region, 5, ps);
        Field::Bot& bot = f.AddBot([pilot](Field::Bot& b) { return pilot->Next(b.client->Player()); }, cs);
        ASSERT_TRUE(f.Join());
        ASSERT_TRUE(held);
        ASSERT_FALSE(bot.client->Window().Contains(c)) << "C must enter the window while the walker walks";
        held->Hold();
        bot.waitIdle = false;              // a job waits for C: the tick's jobs never all finish
        ReleaseHeldChunk guard{held.get()};
        int ticks = 0, pickedAt = -1, releasedAt = -1, repairedAt = -1, reachedAt = -1;
        uint32_t checksAtPick = 0;
        bool skipped = false;
        const math::AABB cBox(glm::vec3(voxel::ChunkToVoxel(c)) * 0.125f, glm::vec3(voxel::ChunkToVoxel(c + glm::ivec3(1))) * 0.125f);
        bot.afterTick = [&](Field::Bot& b) {
            ++ticks;
            uint32_t tick = 0;
            if (pickedAt < 0 && f.server.LastHashChecked(b.client->Slot(), c, tick)) {
                pickedAt = ticks;
                checksAtPick = b.client->Stats().hashChecks;
            }
            // The check that picked C was processed while C was loading: release it now.
            if (pickedAt >= 0 && releasedAt < 0 && b.client->Stats().hashChecks > checksAtPick) {
                skipped = !b.client->Sync().IsCurrent(c);
                held->Release();
                b.waitIdle = true;
                releasedAt = ticks;
            }
            if (repairedAt < 0 && b.client->Sync().FromData(c) && b.client->Sync().IsCurrent(c)) repairedAt = ticks;
            if (reachedAt < 0 && b.client->Predicting()) {
                const math::AABB reach = sim::PlayerReach(b.client->Player().position, b.client->Tuning());
                if (glm::all(glm::lessThan(reach.min, cBox.max)) && glm::all(glm::greaterThan(reach.max, cBox.min))) reachedAt = ticks;
            }
        };
        for (int pass = 0; pass < 4000 && !pilot->Done(); ++pass) f.Pass();
        EXPECT_TRUE(pilot->Done());
        held->Release();
        bot.waitIdle = true;
        ASSERT_TRUE(f.Drain());
        PrintChecks(run ? "picked while loading, run" : "picked while loading, walk", bot);
        std::printf("[ checks ] C first picked at tick %d, released at %d, repaired at %d, reached at %d (%.2f s before)\n", pickedAt, releasedAt,
                    repairedAt, reachedAt, static_cast<double>(reachedAt - repairedAt) / 60.0);
        ASSERT_GE(pickedAt, 0) << "C was never picked";
        EXPECT_TRUE(skipped) << "C was current when its first check came: the test did not hold it";
        EXPECT_GE(repairedAt, 0) << "C was never repaired";
        EXPECT_GE(reachedAt, 0) << "the walker never reached C";
        EXPECT_LT(repairedAt, reachedAt) << "repaired after the walker reached it";
        EXPECT_EQ(bot.client->Stats().mismatchesPristine, 1u);
        EXPECT_GE(bot.client->Sync().Stats().deferredMismatches, 1u);
        EXPECT_EQ(bot.client->Stats().checksOutOfOrder, 0u);
        f.ExpectSameWorld(bot);
    }
}

// Review of stage 7b (finding 9): a terrain-mode walker on a slow link (bulk capped at 64 kB/s: its
// window's data falls behind the walk) runs 70 m east and back. Every window update prunes the queued
// chunks that left the window - none is ever queued outside it, the queue never holds more than the
// window's non-uniform chunks (one dense row of 48 x 48 on flat ground) - and the walker converges.
TEST(Region250, ATerrainModeWalkerOnASlowLinkQueuesOnlyItsWindow) {
    net::ServerSettings settings = Settings(Plain250());
    settings.maxBulkBytesPerSecond = 64.0 * 1024.0;
    Field f(settings, Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    net::ClientSettings cs = GameClient();
    cs.wrapGenerator = OtherGround(f.server.Region(), kGround + 12);
    const glm::dvec3 spawn = f.server.Template().spawn;
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& bot = f.AddBot(Walk(f.server.Region(), {{static_cast<float>(spawn.x) + 70.0f, 0.0f}, {static_cast<float>(spawn.x), 0.0f}}, &pilot),
                               cs);
    ASSERT_TRUE(f.Join(3600));
    ASSERT_TRUE(bot.client->TerrainMode());
    uint32_t outside = 0, steps = 0;
    for (int pass = 0; pass < 4000 && !pilot->Done(); ++pass) {
        f.Pass();
        const net::TerrainPeerCost t = f.server.TerrainCost(bot.client->Slot());
        outside = std::max(outside, t.queuedOutsideWindow);
        steps = t.steps;
    }
    EXPECT_TRUE(pilot->Done());
    const net::TerrainPeerCost t = f.server.TerrainCost(bot.client->Slot());
    std::printf("[ slow link ] terrain mode at 64 kB/s: %u window steps, at most %u chunks queued (%u outside the window at most), %llu "
                "pruned; queue sorts %.3f ms at most\n",
                steps, t.maxQueuedChunks, outside, static_cast<unsigned long long>(f.server.Stats().queuePruned),
                f.server.Stats().maxQueueSortSeconds * 1000.0);
    EXPECT_GT(steps, 10u);
    EXPECT_EQ(outside, 0u) << "chunks that left the window stayed queued";
    EXPECT_LE(t.maxQueuedChunks, static_cast<uint32_t>(kGameWindow * kGameWindow)) << "the queue outgrew the window";
    EXPECT_GT(f.server.Stats().queuePruned, 0u) << "the link kept up: nothing was left to prune";
    ASSERT_TRUE(f.Settle(7200));
    f.ExpectSameWorld(bot);
    ExpectNoFalseChecks(bot, 4, 2);
}

// networking.md "Testing": chunk still loading (on a moving window, stage 7b M-b). The test moves a
// client's window 8 m east while the generation of the entering slab's chunks is held: the ack bases
// them (pristine) while they are still loading, three craters hit them, the operations skip them;
// released, they load and catch up from the ring (replays > 0, never a partial replay), and the window
// is the server's.
TEST(Region250, ChunksThatLoadLateCatchUpWhileTheWindowMoves) {
    Field f(Settings(Plain250()), Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    std::shared_ptr<HeldColumns> held;
    net::ClientSettings cs = GameClient();
    cs.wrapGenerator = [&held](std::shared_ptr<voxel::IWorldGenerator> g) {
        held = std::make_shared<HeldColumns>(std::move(g));
        return held;
    };
    Field::Bot& bot = f.AddBot(Stand, cs);
    bot.manual = true;
    for (int i = 0; i < 60 && !bot.simulation; ++i) f.Pass();
    ASSERT_TRUE(bot.simulation);
    ASSERT_TRUE(held);
    ReleaseHeld guard{held.get()};
    const glm::dvec3 home = bot.client->Spawn();
    const std::array<glm::dvec3, 1> focusHome = {home};
    for (int pass = 0; pass < 900 && !bot.client->Synced(); ++pass) {
        bot.simulation->UpdateStreaming(home, focusHome);
        f.Pass();
    }
    ASSERT_TRUE(bot.client->Synced());
    // The window steps east: hold its entering columns' generation.
    const net::SyncBox before = bot.client->Window();
    held->Hold(before.Max().x, before.Max().x + 3);
    const glm::dvec3 east = home + glm::dvec3(10.0, 0.0, 0.0);
    const std::array<glm::dvec3, 1> focusEast = {east};
    for (int pass = 0; pass < 60 && bot.client->Window() == before; ++pass) {
        bot.simulation->UpdateStreaming(east, focusEast);
        f.Pass();
    }
    ASSERT_NE(bot.client->Window(), before);
    const net::SyncBox after = bot.client->Window();
    // Wait for the ack (the entering chunks Based, not current: still loading).
    const glm::ivec3 slab(after.Max().x - 1, 2, 0);
    for (int pass = 0; pass < 120 && bot.client->Sync().StateOf(slab) != net::ChunkSyncState::Based; ++pass) {
        bot.simulation->UpdateStreaming(east, focusEast);
        f.Pass();
    }
    ASSERT_EQ(bot.client->Sync().StateOf(slab), net::ChunkSyncState::Based);
    ASSERT_FALSE(bot.client->Sync().IsCurrent(slab)) << "the held chunk loaded";
    const uint64_t replaysBefore = bot.client->Sync().Stats().replays;
    const double x = (static_cast<double>(after.Max().x) - 1.5) * 4.0;
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(f.Crater(x, -6.0 + 6.0 * i));
    for (int pass = 0; pass < 600 && !(f.server.QueuedCasts() == 0 && bot.client->AppliedOpSeq() == f.server.LastOpSeq()); ++pass) {
        bot.simulation->UpdateStreaming(east, focusEast);
        f.Pass();
    }
    ASSERT_EQ(bot.client->AppliedOpSeq(), f.server.LastOpSeq());
    EXPECT_GT(bot.client->Sync().Stats().chunkSkips, 0u);
    EXPECT_FALSE(bot.client->Synced());
    held->Release();
    bool synced = false;
    for (int pass = 0; pass < 1800 && !synced; ++pass) {
        bot.simulation->UpdateStreaming(east, focusEast);
        f.Pass();
        synced = bot.client->Synced() && bot.client->AppliedOpSeq() == f.server.LastOpSeq();
    }
    ASSERT_TRUE(synced);
    const net::ChunkSyncStats& s = bot.client->Sync().Stats();
    std::printf("[ load late ] %llu replays after the release, %llu chunk skips, %u not covered\n",
                static_cast<unsigned long long>(s.replays - replaysBefore), static_cast<unsigned long long>(s.chunkSkips), s.notCovered);
    EXPECT_GT(s.replays, replaysBefore) << "nothing was replayed from the ring";
    EXPECT_EQ(s.notCovered, 0u);
    EXPECT_EQ(bot.client->Stats().windowResets, 0u);
    f.ExpectSameWorld(bot);
}

// networking.md "Testing": a client with a deliberately small store (stage 7a's test, in server
// terrain mode). 18 gold boxes in the air on the walker's way (one per chunk: air when pristine);
// the terrain-mode walker's store is taken but for 4 slots: their data finds no slot (Unsynced). A
// window step frees slots before the entering slab's data comes: those are not free for requests
// (no request while full, no request / StoreFull loop); slots freed, each is requested exactly once
// and the window is the server's.
TEST(Region250, ASmallStoreInTerrainModeRequestsEachChunkOnce) {
    net::ServerSettings settings = Settings(Plain250());
    settings.countChunkRequests = true;
    Field f(settings, Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    for (int i = 0; i < 18; ++i) {
        const glm::ivec3 lo(800 + 32 * (i % 6) + 8, 160, 32 * (i / 6 - 1) + 8);    // x 100-125 m: beyond the window at the spawn
        ASSERT_NE(f.server.ApplyOp(sim::WorldOp::Box(lo, lo + glm::ivec3(2), DefaultIds().gold)), 0u);
    }
    f.server.Tick();
    net::ClientSettings cs = GameClient();
    cs.wrapGenerator = OtherGround(f.server.Region(), kGround + 12);
    std::shared_ptr<net::BotPilot> pilot;
    auto walk = Walk(f.server.Region(), {{45.0f, 0.0f}}, &pilot);
    auto go = std::make_shared<bool>(false);
    Field::Bot& bot = f.AddBot([walk, go](Field::Bot& b) { return *go ? walk(b) : sim::PlayerInput{}; }, cs);
    ASSERT_TRUE(f.Join());
    ASSERT_TRUE(bot.client->TerrainMode());
    // Let the join's data come first (its chunks are the window's surface).
    for (int pass = 0; pass < 3000 && bot.client->Stats().joinDataTicks == 0; ++pass) f.Pass();
    ASSERT_GT(bot.client->Stats().joinDataTicks, 0u);
    // The store is taken (as if a larger window held it) but for 4 slots. The window's dense surface
    // chunks keep their number as it moves (a step frees the leaving slab's slots before the entering
    // slab's data takes them); the boxes' chunks need slots of their own.
    voxel::ChunkStore& store = bot.simulation->Chunks();
    std::vector<uint32_t> taken;
    while (store.Capacity() - store.UsedCount() > 4) taken.push_back(store.Alloc());
    const uint32_t requestsBefore = bot.client->Sync().Stats().requested;
    *go = true;
    for (int pass = 0; pass < 2400 && !pilot->Done(); ++pass) f.Pass();
    ASSERT_TRUE(pilot->Done());
    f.Passes(120);
    const net::ChunkSyncStats& ss = bot.client->Sync().Stats();
    const uint32_t unsynced = ss.unsynced;
    std::printf("[ small store ] terrain mode: %u chunk(s) unsynced, %u requested while full\n", unsynced, ss.requested - requestsBefore);
    EXPECT_GT(unsynced, 0u);
    EXPECT_EQ(ss.requested, requestsBefore) << "requested while the store is full";
    EXPECT_FALSE(bot.client->Synced());
    std::vector<glm::ivec3> waiting;
    const net::SyncBox window = bot.client->Window();
    for (int z = window.min.z; z < window.Max().z; ++z)
        for (int y = window.min.y; y < window.Max().y; ++y)
            for (int x = window.min.x; x < window.Max().x; ++x)
                if (bot.client->Sync().StateOf({x, y, z}) == net::ChunkSyncState::Unsynced) waiting.emplace_back(x, y, z);
    EXPECT_FALSE(waiting.empty());
    for (const uint32_t slot : taken) (void)store.Release(slot);
    ASSERT_TRUE(f.Drain());
    const std::vector<std::pair<glm::ivec3, uint32_t>> counts = f.server.ChunkRequestCounts();
    for (const auto& [chunk, n] : counts) EXPECT_EQ(n, 1u) << "chunk (" << chunk.x << ", " << chunk.y << ", " << chunk.z << ") requested " << n << " times";
    for (const glm::ivec3& c : waiting)
        EXPECT_TRUE(std::any_of(counts.begin(), counts.end(), [&](const auto& e) { return e.first == c; }))
            << "unsynced chunk (" << c.x << ", " << c.y << ", " << c.z << ") never requested";
    EXPECT_EQ(ss.unsynced, unsynced) << "a chunk found no slot after the slots freed";
    f.ExpectSameWorld(bot);
}

// networking.md "Testing": blast at a chunk border beside a walking player (stage 4's test while the
// game's window steps under the player). The bot strafes to x = 1.9 m and walks 20 m along z and back,
// the window stepping as it goes; every 20 ticks the server blasts the x = 0 border ahead of it, then
// 0.6 m closer so the rims reach under the capsule. At most 4 corrections in the 40 rim blasts, the
// largest error under 2 cm, no snap, never held; the checks find nothing.
TEST(Region250, BlastAtAChunkBorderBesideAWalkingPlayer) {
    net::Impairment im;
    im.delaySeconds = 0.04;
    im.jitterSeconds = 0.03;
    Field f(Settings(Plain250()), im);
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    uint32_t start = 0;
    Field::Bot& bot = f.AddBot(
        [&start](Field::Bot& b) {
            sim::PlayerInput in;
            const uint32_t t = b.client->PredictTick() - start;
            if (t < 30) {
                in.moveX = 127;
            } else {
                in.moveZ = 127;
                in.yaw = ((t - 30) / 300) % 2 == 0 ? 0 : 32768;
            }
            return in;
        },
        GameClient());
    ASSERT_TRUE(f.Join());
    start = bot.client->PredictTick();
    f.Passes(60);
    const net::PredictionStats before = bot.client->Prediction();
    const uint32_t updatesBefore = bot.client->Stats().windowUpdates;
    auto blasts = [&](int count, double x) {
        for (int i = 0; i < count; ++i) {
            const std::optional<sim::PlayerState> p = f.server.GetPlayer(bot.client->Slot());
            ASSERT_TRUE(p.has_value());
            const double ahead = p->velocity.z < 0.0f ? -1.5 : 1.5;
            ASSERT_TRUE(f.server.QueueCast(sim::BlastDesc{glm::dvec3(x, kGroundMetres, static_cast<double>(p->position.z) + ahead), 1.2f, 6.0f}));
            f.Passes(20);
        }
    };
    blasts(40, 0.0);
    const net::PredictionStats beside = bot.client->Prediction();
    blasts(40, 0.6);
    const net::PredictionStats rim = bot.client->Prediction();
    const uint64_t rimCorrections = rim.corrections - beside.corrections;
    std::printf("[ border ] beside: %llu corrections; rim: %llu corrections in 40 blasts, largest error %.4f m, %u window updates during\n",
                static_cast<unsigned long long>(beside.corrections - before.corrections), static_cast<unsigned long long>(rimCorrections),
                static_cast<double>(rim.maxError), bot.client->Stats().windowUpdates - updatesBefore);
    EXPECT_GT(bot.client->Stats().windowUpdates, updatesBefore + 2) << "the window did not step under the player";
    EXPECT_EQ(beside.corrections, before.corrections);
    EXPECT_LE(rimCorrections, 4u);
    EXPECT_LT(rim.maxError, 0.02f);
    EXPECT_EQ(rim.snaps, 0u);
    EXPECT_EQ(rim.heldTicks, 0u);
    EXPECT_EQ(bot.client->Prediction().syncHeldTicks, 0u);
    ASSERT_TRUE(f.Settle());
    f.ExpectSameWorld(bot);
    ExpectNoFalseChecks(bot, 4, 4);
}

// networking.md "Testing": collapse with 64 awake pieces (stage 6's test, with the site outside bot B's
// window). 80 mushrooms 60 m east of the spawn have their stems cut two a tick: the awake cap holds at
// 64, the rest is client rubble; bot A stands beside the site, bot B 60 m west of the spawn (the site
// is outside its window): its copies come by model requests. The entity checks find nothing during
// the wave (they are skipped or partial while models are asked for), snapshots stay under 1200 B, and
// both end with the server's bodies and world.
TEST(Region250, CollapseWith64AwakePiecesOutsideOneWindow) {
    Field f(Settings(Plain250()), Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    const sim::RegionDesc& region = f.server.Region();
    std::shared_ptr<net::BotPilot> pa, pb;
    Field::Bot& a = f.AddBot(Walk(region, {{55.0f, -20.0f}}, &pa), GameClient(true));
    Field::Bot& b = f.AddBot(Walk(region, {{-60.0f, 0.0f}}, &pb), GameClient());
    ASSERT_TRUE(f.Join());
    for (int pass = 0; pass < 1500 && !(pa->Done() && pb->Done()); ++pass) f.Pass();
    ASSERT_TRUE(pa->Done() && pb->Done());
    // 80 mushrooms (a 3 x 3 stem 2 m tall, a 9 x 9 cap) on a 2 m grid around (60, 0) m.
    const voxel::WorldMaterials ids = DefaultIds();
    std::vector<glm::ivec2> grid;
    for (int z = -40; z < 40 && grid.size() < 80; z += 16)
        for (int x = 400; x < 400 + 16 * 16 && grid.size() < 80; x += 16) grid.push_back({x, z});
    ASSERT_EQ(grid.size(), 80u);
    for (const glm::ivec2& m : grid) {
        f.server.ApplyOp(sim::WorldOp::Box({m.x, kGround, m.y}, {m.x + 3, kGround + 16, m.y + 3}, ids.stone));
        const glm::ivec2 lo = m + glm::ivec2(1 - 9 / 2);
        f.server.ApplyOp(sim::WorldOp::Box({lo.x, kGround + 16, lo.y}, {lo.x + 9, kGround + 20, lo.y + 9}, ids.planks));
    }
    ASSERT_TRUE(f.Settle());
    f.draining = false;
    ASSERT_FALSE(b.client->Window().Contains(glm::ivec3(400 / 32, 3, 0))) << "the site is inside B's window";
    const uint32_t modelRequestsBefore = b.client->Stats().modelRequests;
    for (size_t i = 0; i < grid.size(); i += 2) {
        for (size_t k = i; k < i + 2; ++k) {
            const glm::ivec2& m = grid[k];
            ASSERT_TRUE(f.server.QueueCast(voxel::SphereQ{glm::ivec3((m.x + 1) * 16 + 8, (kGround + 11) * 16 + 8, (m.y + 1) * 16 + 8), 3 * 16}, 1600));
        }
        f.Pass();
    }
    f.Passes(20);
    ASSERT_TRUE(f.Settle());
    PrintChecks("collapse A", a);
    PrintChecks("collapse B", b);
    const net::ServerStats& st = f.server.Stats();
    const sim::ReplicationStats& rs = f.server.Simulation().Replication().stats;
    std::printf("[ collapse ] most awake %u, %u pieces replicated, %u over the awake cap; largest snapshot %zu B; B: %u model requests\n",
                st.maxAwakeBodies, rs.pieces, rs.overAwakeCap, st.maxSnapshotBytes, b.client->Stats().modelRequests - modelRequestsBefore);
    EXPECT_EQ(st.maxAwakeBodies, 64u);
    EXPECT_GT(rs.overAwakeCap, 0u);
    EXPECT_LE(st.maxSnapshotBytes, net::kSnapshotBudgetBytes);
    EXPECT_GT(b.client->Stats().modelRequests, modelRequestsBefore) << "B's copies did not come by model requests";
    for (const Field::Bot* bot : {&a, &b}) {
        f.ExpectSameWorld(*bot);
        ExpectNoFalseChecks(*bot, 4, 4);
    }
}

// networking.md "Testing": a blast at the region edge and at the maximum height beside a window that
// straddles the server's (stage 7b M-b). Two bots stand at the east play edge (one generates its own
// terrain, one is in server terrain mode): their windows reach 90 m past the server's window. Blasts
// at the edge, at the bottom of the build headroom and near the ceiling. Chunks beyond the server's
// window are never requested, never checked, never in a TerrainAck (code 0) and still based; both end
// with the server's world over their sync boxes.
TEST(Region250, ABlastAtTheRegionEdgeAndCeilingReachesAStraddlingWindow) {
    net::ServerSettings settings = Settings(Plain250());
    settings.countChunkRequests = true;
    Field f(settings, Lossy());
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    const sim::RegionDesc& region = f.server.Region();
    std::shared_ptr<net::BotPilot> pa, pb;
    Field::Bot& a = f.AddBot(Walk(region, {{125.0f, 0.0f}}, &pa), GameClient(true));
    net::ClientSettings cs = GameClient();
    cs.wrapGenerator = OtherGround(region, kGround + 12);
    Field::Bot& b = f.AddBot(Walk(region, {{125.0f, 6.0f}}, &pb), cs);
    ASSERT_TRUE(f.Join());
    for (int pass = 0; pass < 2400 && !(pa->Done() && pb->Done()); ++pass) f.Pass();
    ASSERT_TRUE(pa->Done() && pb->Done());
    const net::SyncBox server = net::RegionSyncBox(region);
    ASSERT_GT(a.client->Window().Max().x, server.Max().x) << "the window does not reach past the server's";
    // At the edge, on the ground; a tower to the ceiling cut near its top.
    const double edge = 127.0;
    for (int i = 0; i < 6; ++i) ASSERT_TRUE(f.Crater(edge, -10.0 + 4.0 * i, 1.8f));
    const glm::ivec3 top = f.server.Simulation().World().WorldToVoxel(glm::dvec3(edge - 2.0, 0.0, 8.0));
    const int ceiling = static_cast<int>(std::lround(32.0 / 0.125));
    ASSERT_NE(f.server.ApplyOp(sim::WorldOp::Box({top.x, kGround, top.z}, {top.x + 4, ceiling, top.z + 4}, DefaultIds().stone)), 0u);
    ASSERT_TRUE(f.server.QueueCast(sim::BlastDesc{glm::dvec3(edge - 1.75, 30.0, 8.25), 1.5f, 6.0f}));
    ASSERT_TRUE(f.server.QueueCast(sim::BlastDesc{glm::dvec3(edge - 1.75, 20.0, 8.25), 1.0f, 6.0f}));
    f.Passes(240);
    ASSERT_TRUE(f.Settle());
    PrintChecks("edge A", a);
    PrintChecks("edge B", b);
    PrintTerrain("edge B", f, b);
    for (const auto& [chunk, n] : f.server.ChunkRequestCounts())
        EXPECT_TRUE(server.Contains(chunk)) << "chunk (" << chunk.x << ", " << chunk.y << ", " << chunk.z << ") beyond the server's window requested";
    for (const Field::Bot* bot : {&a, &b}) {
        EXPECT_EQ(bot->client->Sync().Stats().checkOutside, 0u) << "the server checked a chunk outside its window";
        const net::SyncBox w = bot->client->Window();
        const glm::ivec3 beyond(server.Max().x + 2, 2, w.min.z + w.size.z / 2);
        ASSERT_TRUE(w.Contains(beyond));
        EXPECT_TRUE(bot->client->Sync().IsCurrent(beyond)) << "a chunk beyond the server's window is not based";
        EXPECT_FALSE(bot->client->Sync().FromData(beyond));
        f.ExpectSameWorld(*bot);
        ExpectNoFalseChecks(*bot, 2, 2);
    }
    EXPECT_TRUE(b.client->TerrainMode());
}

// The amendment of the critic (6): server terrain mode on real (noise) terrain with packing, GPU-free.
// A 64 m noise-terrain region whose server packs its buried chunks; a client whose build makes other
// terrain (another seed: every probe differs) walks across it in server terrain mode. Every chunk the
// server holds Dense is resident on the client (never packed: it is visible there), chunks the server
// packs are installed packed (no slot), the store stays small, and the world is the server's.
TEST(TerrainMode, ANoiseRegionPacksAsTheServerDoes) {
    sim::RegionDesc r;
    r.templateId = sim::RegionTemplateId::Empty;
    r.seed = 4242;
    r.playMinChunk = {-8, -8};
    r.playMaxChunk = {8, 8};
    r.heightRange = {-48.0f, 128.0f};
    r.generator = sim::RegionGenerator::Terrain;
    r.terrain.voxelSize = 0.125f;
    r.terrain.seed = 4242;
    r.terrain.materials = DefaultIds();
    net::ServerSettings settings = Settings(r);
    settings.session.packHidden = true;
    Field f(settings, Lossy());
    ASSERT_TRUE(f.started);
    const sim::RegionDesc& region = f.server.Region();
    net::ClientSettings cs = GameClient();
    cs.windowChunks = 16;              // a moving window over the 70 m server window
    cs.wrapGenerator = [region](std::shared_ptr<voxel::IWorldGenerator>) {
        sim::RegionDesc other = region;
        other.terrain.seed = region.terrain.seed + 1;
        return sim::MakeRegionGenerator(other, DefaultIds());
    };
    std::shared_ptr<net::BotPilot> pilot;
    Field::Bot& b = f.AddBot(Walk(region, {{25.0f, 20.0f}, {-25.0f, -20.0f}}, &pilot), cs);
    b.packHidden = true;               // its world packs as the game's does
    ASSERT_TRUE(f.Join(1800));
    ASSERT_TRUE(b.client->TerrainMode());
    for (int pass = 0; pass < 2400 && !pilot->Done(); ++pass) f.Pass();
    ASSERT_TRUE(f.Settle());
    PrintTerrain("noise region", f, b);
    const voxel::World& sw = f.server.Simulation().World();
    const voxel::World& cw = b.simulation->World();
    const net::SyncBox box = b.client->SyncWindow();
    uint32_t serverDense = 0, serverPacked = 0, packedHere = 0, denseHere = 0;
    for (int z = box.min.z; z < box.Max().z; ++z)
        for (int y = box.min.y; y < box.Max().y; ++y)
            for (int x = box.min.x; x < box.Max().x; ++x) {
                const glm::ivec3 c(x, y, z);
                const voxel::CellState s = sw.StateOf(c), here = cw.StateOf(c);
                if (s == voxel::CellState::Dense) {
                    ++serverDense;
                    EXPECT_NE(here, voxel::CellState::Packed) << "a chunk the server shows is packed here";
                }
                if (s == voxel::CellState::Packed) ++serverPacked;
                packedHere += here == voxel::CellState::Packed ? 1u : 0u;
                denseHere += here == voxel::CellState::Dense ? 1u : 0u;
            }
    std::printf("[ noise ] server: %u dense / %u packed in the sync box; client: %u dense / %u packed; store at most %u slots; installed packed %u "
                "/ shown %u\n",
                serverDense, serverPacked, denseHere, packedHere, b.client->Stats().maxStoreUsed, b.client->Sync().Stats().installedHidden,
                b.client->Sync().Stats().installedShown);
    EXPECT_GT(serverPacked, 0u);
    EXPECT_GT(b.client->Sync().Stats().installedHidden, 0u);
    EXPECT_GT(packedHere, 0u);
    // Review of stage 7b (finding 10): a bound a regression that keeps buried chunks resident here
    // breaks. A chunk the server packs may be resident on the client only near the player (PromoteNear;
    // Repack packs it again beyond nearRadius + its margin) or next to an edit: none here far away.
    const voxel::WorldConfig& wc = cw.Config();
    const double far = static_cast<double>(wc.nearRadius) + std::max(static_cast<double>(wc.repackMargin), 8.0) + 8.0;
    const glm::dvec3 feet = glm::dvec3(b.client->Player().position);
    uint32_t residentFar = 0;
    for (int z = box.min.z; z < box.Max().z; ++z)
        for (int y = box.min.y; y < box.Max().y; ++y)
            for (int x = box.min.x; x < box.Max().x; ++x) {
                const glm::ivec3 c(x, y, z);
                if (sw.StateOf(c) != voxel::CellState::Packed || cw.StateOf(c) != voxel::CellState::Dense) continue;
                const glm::dvec3 lo = glm::dvec3(c) * 4.0;
                residentFar += glm::length(feet - glm::clamp(feet, lo, lo + glm::dvec3(4.0))) > far ? 1u : 0u;
            }
    std::printf("[ noise ] chunks the server packs resident here farther than %.0f m: %u\n", far, residentFar);
    EXPECT_EQ(residentFar, 0u) << "buried chunks the server packs are resident here";
    f.ExpectSameWorld(b);
    ExpectNoFalseChecks(b, 2, 2);
}

// Stage 8 (the 10-minute arena soak): every bot's drain failed and the headless game client exited 1 -
// one server body never rested (it was awake on the server, so no EntityRest came), though it did not
// move. The drain rule (net::DrainWatch: the game's headless exit and vortex_bot's) must settle beside a
// copy that is not resting but stands within 1 mm for the hold time, and must not settle while a copy
// moves. Here the server keeps a crate on flat ground awake by activating its body every tick.
TEST(Region250, ADrainSettlesBesideAStillAwakeServerBody) {
    Field f(Settings(Plain250()));
    ASSERT_TRUE(f.started);
    f.storeLimit = kGameStore;
    Field::Bot& a = f.AddBot([](Field::Bot&) { return sim::PlayerInput{}; }, GameClient());
    ASSERT_TRUE(f.Join());
    const glm::dvec3 spawn = f.server.Template().spawn;
    const uint32_t netId = f.server.SpawnCrate(spawn + glm::dvec3(4.0, 1.0, 0.0));
    ASSERT_NE(netId, 0u);
    ASSERT_TRUE(f.Settle());
    physics::BodyId crate{};
    for (const auto [entity, id, body] : f.server.Simulation().Scene().Registry().view<const scene::NetId, const scene::RigidBody>().each())
        if (id.value == netId) crate = body.body;
    ASSERT_TRUE(f.server.Simulation().Physics().IsValid(crate));
    // Awake on the server from now on, still on the ground.
    const auto pass = [&] {
        f.server.Simulation().Physics().Activate(crate);
        f.Pass();
    };
    for (int i = 0; i < 60; ++i) pass();
    bool copyResting = true;
    for (const net::EntityInfo& e : a.client->Entities())
        if (e.netId == netId) copyResting = e.resting;
    ASSERT_FALSE(copyResting) << "the copy never woke: the test shows nothing";
    net::DrainWatch watch(1.0);
    int settledAt = -1;
    for (int i = 0; i < 300 && settledAt < 0; ++i) {
        pass();
        if (watch.Update(*a.client, f.clock)) settledAt = i;
    }
    std::printf("[ drain ] beside an awake crate standing still (NetId %u): settled after %d passes, %u copy not resting\n", netId, settledAt,
                watch.NotResting());
    EXPECT_GE(settledAt, 59) << "at least the hold time";
    EXPECT_LT(settledAt, 150);
    EXPECT_EQ(watch.NotResting(), 1u);
    // A copy that moves keeps the hold from settling: the crate is thrown up and falls back.
    f.server.Simulation().Physics().AddImpulse(crate, glm::vec3(0.0f, 400.0f, 0.0f));
    net::DrainWatch moving(1.0);
    int movingSettledAt = -1;
    for (int i = 0; i < 40 && movingSettledAt < 0; ++i) {
        pass();
        if (moving.Update(*a.client, f.clock)) movingSettledAt = i;
    }
    EXPECT_LT(movingSettledAt, 0) << "settled while the copy flew";
}
