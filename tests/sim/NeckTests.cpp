// docs/design/shred.md Part 4b (stage 4b) in sim::Blast: a part the load check releases is an
// ordinary Detach (netId 0) after the islands, crumbs in the blast's one ClearMask; nothing new on
// the wire. Server and offline answers to an unloaded chunk, replay, and the rule switched off.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.scene;
import vortex.sim;

using namespace vortex;

namespace {
    constexpr float kVoxel = 0.1f;
    constexpr int g = 80;                              // flat ground: voxels y < 80, its top at 8 m
    const glm::dvec3 kFocus(0.0, 8.0, 0.0);

    /// Window 6 x 8 x 6 chunks: voxels x, z -96..95, y -64..191 (the tower's flood box, y up to 181).
    sim::SimulationConfig NeckConfig(unsigned jobThreads = 3, glm::ivec3 window = {6, 8, 6}) {
        sim::SimulationConfig config;
        config.chunkCapacity = 1536;
        config.maxParticles = 4096;
        config.jobThreads = jobThreads;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = window;
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        config.checkIslandSearch = true;
        return config;
    }

    voxel::WorldMaterials DefaultIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    std::shared_ptr<voxel::IWorldGenerator> Ground() { return voxel::CreateFlatGenerator(g, DefaultIds().grass, DefaultIds().stone); }

    /// The flat ground without its column pass: a chunk that has just entered the window stays
    /// Loading until a later World::Update (as in FaceFloodTests).
    class UnclassifiedGround final : public voxel::IWorldGenerator {
    public:
        UnclassifiedGround() : m_Inner(Ground()) {}
        voxel::ChunkContent Generate(const glm::ivec3& coord, voxel::Chunk& chunk) const override { return m_Inner->Generate(coord, chunk); }

    private:
        std::shared_ptr<voxel::IWorldGenerator> m_Inner;
    };

    struct NeckArena {
        explicit NeckArena(const sim::SimulationConfig& config = NeckConfig(), std::shared_ptr<voxel::IWorldGenerator> generator = Ground())
            : simulation(config) {
            initialized = simulation.Initialize(std::move(generator));
            if (initialized) simulation.World().LoadBlocking(kFocus);
        }
        ~NeckArena() {
            const sim::Simulation::IslandSearchChecks& checks = simulation.IslandSearchCheck();
            EXPECT_EQ(checks.mismatches, 0u) << "the graph and the voxel island search differed";
            EXPECT_EQ(checks.searches, checks.run);
        }
        NeckArena(const NeckArena&) = delete;
        NeckArena& operator=(const NeckArena&) = delete;
        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        voxel::WorldHashResult Hash() const {
            const voxel::World& world = simulation.World();
            return voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize());
        }
        sim::Simulation simulation;
        bool initialized = false;
    };

    void Apply(voxel::World& world, std::span<const sim::WorldOp> ops) {
        for (const sim::WorldOp& op : ops) ASSERT_TRUE(sim::ApplyOp(world, op).Complete());
    }

    std::vector<sim::WorldOp> OpsOf(const sim::BlastResult& blast) {
        std::vector<sim::WorldOp> out;
        for (const sim::AppliedOp& applied : blast.ops) out.push_back(applied.op);
        return out;
    }

    /// The game's watch tower at 0.1 m (src/main.cpp, 0.1 m units), its floor corner at
    /// (-44, g, 46) so that its hollow is around x, z = 0; on a stone_dark paving row; with a
    /// `width` x 2 stone neck (rows 1..15) at its north-west corner, 0 = the full base.
    const glm::ivec3 kTower(-44, g, 46);
    std::vector<sim::WorldOp> TowerOnANeck(const voxel::WorldMaterials& m, int width) {
        const glm::ivec3 c = kTower;
        const auto B = [&c](glm::ivec3 lo, glm::ivec3 hi, uint8_t id) { return sim::WorldOp::Box(c + lo, c + hi, id); };
        std::vector<sim::WorldOp> ops{
            B({10, -1, -80}, {80, 0, -10}, m.stoneDark),  B({30, 0, -60}, {58, 70, -32}, m.stone), B({33, 0, -57}, {55, 66, -35}, 0),
            B({41, 0, -35}, {47, 20, -32}, 0),            B({28, 66, -62}, {60, 70, -30}, m.planks),
        };
        for (int i = 0; i < 4; ++i) {
            const glm::ivec3 p(i & 1 ? 58 : 28, 70, i & 2 ? -32 : -62);
            ops.push_back(B(p, p + glm::ivec3(2, 8, 2), m.wood));
            ops.push_back(B(p + glm::ivec3(0, 8, 0), p + glm::ivec3(2, 10, 2), m.torch));
        }
        if (width > 0) {
            ops.push_back(B({30, 1, -60}, {58, 16, -32}, 0));
            ops.push_back(B({30, 1, -60}, {32, 16, -60 + width}, m.stone));
        }
        return ops;
    }
    /// The --neck-blast shot: 0.6 m, power 6, in the hollow 0.5 m above the floor.
    voxel::SphereQ NeckShot() { return voxel::SphereQ{(kTower + glm::ivec3(44, 5, -46)) * 16 + glm::ivec3(8), 6 * 16}; }
    const uint16_t kPower6 = voxel::QuantizePower(6.0f);
    const glm::ivec3 kRoof = kTower + glm::ivec3(44, 68, -46);
}

TEST(Blast, NeckReleaseIsAnOrdinaryDetach) {
    NeckArena a;
    ASSERT_TRUE(a.initialized);
    Apply(a.World(), TowerOnANeck(a.Ids(), 2));
    ASSERT_EQ(a.World().GetVoxel(kRoof), a.Ids().planks);
    const sim::BlastResult blast = sim::Blast(a.simulation, NeckShot(), kPower6);
    ASSERT_TRUE(blast.islandsSearched);
    EXPECT_TRUE(blast.search.neckRan);
    // Carve, the neck's Detach, (crumbs).
    ASSERT_GE(blast.ops.size(), 2u);
    EXPECT_EQ(blast.ops[0].op.kind, sim::WorldOpKind::CarveSphere);
    EXPECT_GT(blast.ops[0].result.changed, 0u);
    EXPECT_EQ(blast.ops[1].op.kind, sim::WorldOpKind::Detach);
    EXPECT_EQ(blast.ops[1].op.netId, 0u);
    EXPECT_EQ(blast.IslandCount(), 1u);
    EXPECT_EQ(blast.search.neckPieces, 1u);
    // At 0.1 m: the tower above the 15 neck rows (19 184 voxels) and at most the 60 neck voxels.
    EXPECT_GE(blast.ops[1].op.mask.Count(), 19184u);
    EXPECT_LE(blast.ops[1].op.mask.Count(), 19244u);
    ASSERT_EQ(blast.search.neckCuts.size(), 1u);
    EXPECT_EQ(blast.search.neckCuts[0].faces, 4u);
    EXPECT_EQ(blast.search.neckCuts[0].cut, 4u * 1000000u);
    EXPECT_GT(blast.search.neckCuts[0].load, 10u * blast.search.neckCuts[0].cut);
    for (size_t i = 2; i < blast.ops.size(); ++i) EXPECT_EQ(blast.ops[i].op.kind, sim::WorldOpKind::ClearMask);
    EXPECT_EQ(a.World().GetVoxel(kRoof), 0);                                  // the roof is in the piece
    EXPECT_EQ(a.World().GetVoxel(kTower + glm::ivec3(31, 0, -59)), a.Ids().stone);    // ring row 0 stays
    EXPECT_EQ(a.simulation.Scene().VoxelObjectCount(), 0u);                   // no entity on the simulation side
    EXPECT_EQ(a.simulation.IslandSearchCheck().neckReleases, 1u);
}

// A server releases the tower; a client fed the operations has the same world (different worker
// counts on each side).
TEST(Blast, NeckReleaseTwoSimulationsEqualHashes) {
    NeckArena server(sim::ServerConfig(NeckConfig(3))), client(NeckConfig(1));
    ASSERT_TRUE(server.initialized && client.initialized);
    const std::vector<sim::WorldOp> setup = TowerOnANeck(server.Ids(), 2);
    Apply(server.World(), setup);
    Apply(client.World(), setup);
    ASSERT_EQ(server.Hash().hash, client.Hash().hash);
    const sim::BlastResult blast = sim::Blast(server.simulation, NeckShot(), kPower6);
    ASSERT_EQ(blast.search.neckPieces, 1u);
    for (const sim::WorldOp& op : OpsOf(blast)) {
        std::vector<voxel::VoxelEdit> edits;
        const sim::OpResult r = sim::ApplyOp(client.World(), op, &edits);
        EXPECT_TRUE(r.Complete());
        if (op.kind == sim::WorldOpKind::Detach) EXPECT_EQ(sim::BuildDetachedPiece(op, edits).CountSolid(), op.mask.Count());
    }
    const voxel::WorldHashResult s = server.Hash(), c = client.Hash();
    ASSERT_TRUE(s.Complete() && c.Complete());
    EXPECT_EQ(s.hash, c.hash);
    EXPECT_EQ(client.World().GetVoxel(kRoof), 0);
}

// Casts at fixed ticks in two simulations (1 and 3 workers, both servers): identical operations.
TEST(Blast, NeckReplayGivesIdenticalOps) {
    std::vector<std::vector<sim::WorldOp>> ops[2];
    uint64_t hash[2] = {};
    for (int k = 0; k < 2; ++k) {
        NeckArena a(sim::ServerConfig(NeckConfig(k == 0 ? 1u : 3u)));
        ASSERT_TRUE(a.initialized);
        Apply(a.World(), TowerOnANeck(a.Ids(), 22));      // 44 faces: 44 M MU hold 43.1 M at 0.1 m
        for (int tick = 0; tick < 12; ++tick) {
            if (tick == 3) ops[k].push_back(OpsOf(sim::Blast(a.simulation, NeckShot(), kPower6)));
            // Tick 7: a small shot into the neck takes some of its columns.
            if (tick == 7) {
                const voxel::SphereQ edge{(kTower + glm::ivec3(31, 8, -50)) * 16 + glm::ivec3(8), 3 * 16};
                ops[k].push_back(OpsOf(sim::Blast(a.simulation, edge, kPower6)));
            }
            a.simulation.Step(std::span<const glm::dvec3>(&kFocus, 1));
        }
        hash[k] = a.Hash().hash;
    }
    ASSERT_EQ(ops[0].size(), 2u);
    EXPECT_EQ(ops[0], ops[1]);
    EXPECT_EQ(hash[0], hash[1]);
    // The first shot left the tower standing; the second cut the neck under its threshold.
    uint32_t detaches[2] = {};
    for (int i = 0; i < 2; ++i)
        for (const sim::WorldOp& op : ops[0][static_cast<size_t>(i)]) detaches[i] += op.kind == sim::WorldOpKind::Detach ? 1u : 0u;
    EXPECT_EQ(detaches[0], 0u);
    EXPECT_GE(detaches[1], 1u);
}

// shred.md Part 4b step 2: an exterior flood that meets a chunk that is not loaded. A server takes it
// as support (an error, counted); offline the search waits and the late search releases.
TEST(Blast, NeckOnAServerCountsAnUnloadedChunk) {
    NeckArena a(sim::ServerConfig(NeckConfig(3, {6, 6, 6})), std::make_shared<UnclassifiedGround>());
    ASSERT_TRUE(a.initialized);
    Apply(a.World(), TowerOnANeck(a.Ids(), 2));      // its torches reach row 159, the top of chunk row 4
    a.World().Update(kFocus + glm::dvec3(0.0, 3.2, 0.0));
    ASSERT_FALSE(a.World().IsChunkLoaded({0, 5, 0}));
    const sim::BlastResult blast = sim::Blast(a.simulation, NeckShot(), kPower6);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_FALSE(blast.search.floodDeferred);
    EXPECT_GE(blast.search.neckUnloaded, 1u);
    EXPECT_EQ(blast.search.neckPieces, 0u);
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
    EXPECT_EQ(a.World().GetVoxel(kRoof), a.Ids().planks);
}

// Offline, a chunk of the window that is still loading is support for this cast (it can only make
// the check release less) and a late search runs the check again once it has loaded (review: the
// first version held the whole search, face islands included, until then).
TEST(Blast, NeckDefersOfflineAndTheLateSearchReleases) {
    NeckArena a(NeckConfig(3, {6, 6, 6}), std::make_shared<UnclassifiedGround>());
    ASSERT_TRUE(a.initialized);
    Apply(a.World(), TowerOnANeck(a.Ids(), 2));
    const glm::dvec3 up = kFocus + glm::dvec3(0.0, 3.2, 0.0);
    a.World().Update(up);
    ASSERT_FALSE(a.World().IsChunkLoaded({0, 5, 0}));
    const sim::BlastResult blast = sim::Blast(a.simulation, NeckShot(), kPower6);
    EXPECT_TRUE(blast.search.neckDeferred);
    EXPECT_GE(blast.search.neckLoading + blast.search.neckBeyondWindow, 1u);
    EXPECT_EQ(blast.search.neckPieces, 0u);
    EXPECT_EQ(a.World().GetVoxel(kRoof), a.Ids().planks);
    ASSERT_EQ(a.simulation.DeferredIslandSearches().size(), 1u);

    a.World().LoadBlocking(up);
    const std::vector<sim::BlastResult> late = sim::RetryIslandSearches(a.simulation);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_TRUE(late[0].deferred);
    EXPECT_TRUE(late[0].islandsSearched);
    EXPECT_FALSE(late[0].search.neckDeferred);
    EXPECT_EQ(late[0].search.neckPieces, 1u);
    EXPECT_EQ(late[0].IslandCount(), 1u);
    EXPECT_EQ(a.World().GetVoxel(kRoof), 0);
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
}

// The same, with a floating bar across the island box face: the face flood proves it and it is
// emitted in the blast itself while the check waits for the loading chunk.
TEST(Blast, NeckWaitKeepsTheFaceIslands) {
    NeckArena a(NeckConfig(3, {6, 6, 6}), std::make_shared<UnclassifiedGround>());
    ASSERT_TRUE(a.initialized);
    Apply(a.World(), TowerOnANeck(a.Ids(), 2));
    const std::vector<sim::WorldOp> bar{sim::WorldOp::Box({20, 100, 20}, {40, 102, 22}, a.Ids().stone)};
    Apply(a.World(), bar);
    const glm::dvec3 up = kFocus + glm::dvec3(0.0, 3.2, 0.0);
    a.World().Update(up);
    ASSERT_FALSE(a.World().IsChunkLoaded({0, 5, 0}));
    const sim::BlastResult blast = sim::Blast(a.simulation, NeckShot(), kPower6);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_FALSE(blast.search.floodDeferred);
    EXPECT_TRUE(blast.search.neckDeferred);
    EXPECT_EQ(blast.search.facePieces, 1u);
    EXPECT_EQ(a.World().GetVoxel({30, 100, 20}), 0);                   // the bar fell now
    EXPECT_EQ(a.World().GetVoxel(kRoof), a.Ids().planks);               // the tower waits for the chunk
    a.World().LoadBlocking(up);
    const std::vector<sim::BlastResult> late = sim::RetryIslandSearches(a.simulation);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_EQ(late[0].search.neckPieces, 1u);
    EXPECT_EQ(late[0].search.facePieces, 0u);
    EXPECT_EQ(a.World().GetVoxel(kRoof), 0);
}

// Offline, a chunk outside the window is support for now (review: nothing judged it again): the
// late search waits until the window has moved over it and loaded it, then releases the tower.
TEST(Blast, NeckBeyondTheWindowIsCheckedAgainOnceLoaded) {
    NeckArena a(NeckConfig(3, {6, 6, 6}));               // rows -1..4 around the focus
    ASSERT_TRUE(a.initialized);
    const voxel::World& world = a.World();
    ASSERT_EQ(world.WindowMinChunk().y + world.WindowSize().y, 5) << "chunk row 5 must lie above the window";
    Apply(a.World(), TowerOnANeck(a.Ids(), 2));                        // its torches end at row 159, under chunk row 5
    const sim::BlastResult blast = sim::Blast(a.simulation, NeckShot(), kPower6);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_GE(blast.search.neckBeyondWindow, 1u);
    EXPECT_TRUE(blast.search.neckDeferred);
    EXPECT_EQ(blast.search.neckPieces, 0u);
    EXPECT_EQ(a.World().GetVoxel(kRoof), a.Ids().planks);
    ASSERT_EQ(a.simulation.DeferredIslandSearches().size(), 1u);
    EXPECT_TRUE(sim::RetryIslandSearches(a.simulation).empty());        // the window has not moved
    a.World().LoadBlocking(kFocus + glm::dvec3(0.0, 3.2, 0.0));       // rows 0..5
    ASSERT_TRUE(a.World().IsChunkLoaded({0, 5, 0}));
    const std::vector<sim::BlastResult> late = sim::RetryIslandSearches(a.simulation);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_EQ(late[0].search.neckPieces, 1u);
    EXPECT_EQ(a.World().GetVoxel(kRoof), 0);
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
}

// With SimulationConfig::neckRule off the graph search gives stage 4's operations: the tower on its
// neck stays; with it on, the full base stands too (the same operations either way).
TEST(Blast, NeckRuleOffEqualsStage4) {
    for (const int width : {2, 0}) {
        std::vector<sim::WorldOp> ops[2];
        for (int k = 0; k < 2; ++k) {
            sim::SimulationConfig config = NeckConfig();
            config.neckRule = k == 1;
            NeckArena a(config);
            ASSERT_TRUE(a.initialized);
            Apply(a.World(), TowerOnANeck(a.Ids(), width));
            const sim::BlastResult blast = sim::Blast(a.simulation, NeckShot(), kPower6);
            EXPECT_EQ(blast.search.neckRan, k == 1);
            ops[k] = OpsOf(blast);
        }
        if (width == 0) EXPECT_EQ(ops[0], ops[1]);
        else EXPECT_EQ(ops[1].size(), ops[0].size() + 1);       // the neck's Detach, nothing else
        for (const sim::WorldOp& op : ops[0]) EXPECT_NE(op.kind, sim::WorldOpKind::Detach);
    }
}

// Nothing outside the flood box is read: window 6 chunks (x, z -96..95 = exactly the flood box of
// the shot) and 8 chunks (the chunks just outside loaded) give equal operations and counters.
TEST(Blast, NeckNothingOutsideTheFloodBoxIsRead) {
    NeckArena small(NeckConfig(3, {6, 8, 6})), large(NeckConfig(3, {8, 8, 8}));
    ASSERT_TRUE(small.initialized && large.initialized);
    ASSERT_FALSE(small.World().IsChunkLoaded({-4, 2, 0}));
    ASSERT_TRUE(large.World().IsChunkLoaded({-4, 2, 0}));
    Apply(small.World(), TowerOnANeck(small.Ids(), 2));
    Apply(large.World(), TowerOnANeck(large.Ids(), 2));
    const sim::BlastResult a = sim::Blast(small.simulation, NeckShot(), kPower6);
    const sim::BlastResult b = sim::Blast(large.simulation, NeckShot(), kPower6);
    ASSERT_TRUE(a.islandsSearched && b.islandsSearched);
    EXPECT_EQ(OpsOf(a), OpsOf(b));
    EXPECT_EQ(a.search.neckPieces, 1u);
    EXPECT_EQ(a.search.neckNodes, b.search.neckNodes);
    EXPECT_EQ(a.search.neckLoads, b.search.neckLoads);
    EXPECT_EQ(a.search.neckAnchors, b.search.neckAnchors);
    EXPECT_EQ(a.search.neckArcs, b.search.neckArcs);
    EXPECT_EQ(a.search.neckExteriorVisits, b.search.neckExteriorVisits);
    EXPECT_EQ(a.search.neckFlowWork, b.search.neckFlowWork);
    EXPECT_EQ(a.search.chunkGraphs, b.search.chunkGraphs);
    EXPECT_EQ(a.search.neckBeyondWindow + b.search.neckBeyondWindow, 0u);   // floods stop at the face layer
}
