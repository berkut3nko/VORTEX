// The "second shot" bug and its fix (docs/design/shred.md, stages 1, 1b and 1c): a piece that a
// blast frees but that reaches past the island box face is proven floating by an outward flood
// inside the blast's fixed flood box; a carve that skipped loading chunks is replayed before a
// late search; deferred searches and oversize islands are counted, never lost silently.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <functional>
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
    const glm::dvec3 kFocus(0.0, 8.0, 0.0);            // window chunks (-3..2, -1..4, -3..2): voxels x, z -96..95, y -32..159
    constexpr uint16_t kStrong = 1600;

    sim::SimulationConfig FloodConfig(unsigned jobThreads = 3, glm::ivec3 window = {6, 6, 6}) {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.maxParticles = 4096;
        config.jobThreads = jobThreads;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = window;
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        // docs/design/shred.md stage 4: every island search of these tests (the graph, the
        // default) also runs the stage-1 voxel search on the same voxels; FloodArena fails the
        // test if they ever found different islands. Voxel-only counters are read from
        // BlastResult::reference.
        config.checkIslandSearch = true;
        return config;
    }

    voxel::WorldMaterials DefaultIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    std::shared_ptr<voxel::IWorldGenerator> Ground() {
        const voxel::WorldMaterials ids = DefaultIds();
        return voxel::CreateFlatGenerator(g, ids.grass, ids.stone);
    }

    /// The flat ground without its column pass: every chunk of the window is a job, so a chunk
    /// that has just entered the window stays Loading until a later World::Update.
    class UnclassifiedGround final : public voxel::IWorldGenerator {
    public:
        UnclassifiedGround() : m_Inner(Ground()) {}
        voxel::ChunkContent Generate(const glm::ivec3& coord, voxel::Chunk& chunk) const override { return m_Inner->Generate(coord, chunk); }

    private:
        std::shared_ptr<voxel::IWorldGenerator> m_Inner;
    };

    /// The flat ground with a generated stone "mushroom": a 3 x 16 x 3 stem at x, z 0..2 on the
    /// ground and an 11 x 3 x 11 cap on it (x, z -4..6, y g+16..g+18). Generated, so a carve that
    /// skips the stem's chunk while it loads leaves the stem whole when it arrives.
    class MushroomGround final : public voxel::IWorldGenerator {
    public:
        MushroomGround() : m_Inner(UnclassifiedGround()), m_Stone(DefaultIds().stone) {}
        static bool InStem(const glm::ivec3& v) { return v.x >= 0 && v.x < 3 && v.z >= 0 && v.z < 3 && v.y >= g && v.y < g + 16; }
        static bool InCap(const glm::ivec3& v) { return v.x >= -4 && v.x < 7 && v.z >= -4 && v.z < 7 && v.y >= g + 16 && v.y < g + 19; }
        voxel::ChunkContent Generate(const glm::ivec3& coord, voxel::Chunk& chunk) const override {
            voxel::ChunkContent content = m_Inner.Generate(coord, chunk);
            const glm::ivec3 base = voxel::ChunkToVoxel(coord);
            bool touched = false;
            for (int z = 0; z < voxel::kChunkSize; ++z)
                for (int y = 0; y < voxel::kChunkSize; ++y)
                    for (int x = 0; x < voxel::kChunkSize; ++x) {
                        const glm::ivec3 v = base + glm::ivec3(x, y, z);
                        if (!InStem(v) && !InCap(v)) continue;
                        if (!touched && content.kind != voxel::ChunkKind::Dense) {
                            const uint8_t fill = content.kind == voxel::ChunkKind::Uniform ? content.uniformId : uint8_t{0};
                            for (uint8_t& id : chunk.ids) id = fill;
                        }
                        touched = true;
                        chunk.ids[voxel::Chunk::VoxelIndex(x, y, z)] = m_Stone;
                    }
            if (touched) content = {voxel::ChunkKind::Dense, 0};
            return content;
        }

    private:
        UnclassifiedGround m_Inner;
        uint8_t m_Stone;
    };

    struct FloodArena {
        explicit FloodArena(const sim::SimulationConfig& config = FloodConfig(), std::shared_ptr<voxel::IWorldGenerator> generator = Ground(),
                            bool load = true)
            : simulation(config) {
            initialized = simulation.Initialize(std::move(generator));
            if (initialized && load) simulation.World().LoadBlocking(kFocus);
        }
        ~FloodArena() {
            const sim::Simulation::IslandSearchChecks& checks = simulation.IslandSearchCheck();
            EXPECT_EQ(checks.mismatches, 0u) << "the graph and the voxel island search differed in " << checks.mismatches << " of "
                                             << checks.searches << " search(es)";
            if (expectChecked)
                EXPECT_EQ(checks.searches, checks.run) << "island searches run without the comparison (checkIslandSearch lost on the way?)";
            // shred.md Part 4b: these arenas are pillars, ledges and slabs far inside the strength
            // table's margins; the load check releases nothing in them.
            if (!allowNeckReleases) EXPECT_EQ(checks.neckReleases, 0u) << "the load check released a part in a stage-1 arena";
        }
        /// The cost measurement's load-check runs (the tower falls by the check there).
        bool allowNeckReleases = false;
        /// false for the arenas that run one search on purpose (checkIslandSearch off).
        bool expectChecked = true;
        FloodArena(const FloodArena&) = delete;
        FloodArena& operator=(const FloodArena&) = delete;
        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        voxel::WorldHashResult Hash() const {
            const voxel::World& world = simulation.World();
            return voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize());
        }
        sim::Simulation simulation;
        bool initialized = false;
    };

    void Apply(voxel::World& world, std::span<const sim::WorldOp> ops, bool mustComplete = true) {
        for (const sim::WorldOp& op : ops) {
            const sim::OpResult r = sim::ApplyOp(world, op);
            if (mustComplete) ASSERT_TRUE(r.Complete());
        }
    }

    std::vector<sim::WorldOp> OpsOf(const sim::BlastResult& blast) {
        std::vector<sim::WorldOp> out;
        for (const sim::AppliedOp& applied : blast.ops) out.push_back(applied.op);
        return out;
    }

    /// shred.md "Tests for stage 1": a 3 x height x 3 stone pillar on the ground at x, z 0..2.
    sim::WorldOp Pillar(const voxel::WorldMaterials& ids, int height) { return sim::WorldOp::Box({0, g, 0}, {3, g + height, 3}, ids.stone); }
    /// The base cut: centre (1.5, g + 4.5, 1.5) voxels, radius 4 voxels; with kStrong it removes the
    /// pillar's rows g+1..g+7 (and its centre voxel in rows g and g+8). The island box is
    /// (1, g+4, 1) +- 28 voxels: its top face is row g+31.
    constexpr voxel::SphereQ kBaseCut{glm::ivec3(24, (g + 4) * 16 + 8, 24), 64};
}

// --- stage 1: the face flood ----------------------------------------------------------------------

TEST(Blast, IslandReachingTheSearchBoxFaceIsDetachedByTheBlastThatFreedIt) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const std::vector<sim::WorldOp> setup{Pillar(a.Ids(), 40)};
    Apply(a.World(), setup);

    const sim::BlastResult blast = sim::Blast(a.simulation, kBaseCut, kStrong);
    EXPECT_TRUE(blast.islandsSearched);
    ASSERT_EQ(blast.IslandCount(), 1u);
    ASSERT_EQ(blast.ops.size(), 2u);
    const sim::AppliedOp& detach = blast.ops[1];
    EXPECT_EQ(detach.op.kind, sim::WorldOpKind::Detach);
    // Rows g+8..g+39 minus the centre voxel of row g+8.
    EXPECT_EQ(detach.op.mask.Count(), 32u * 9u - 1u);
    EXPECT_EQ(detach.result.changed, detach.op.mask.Count());
    EXPECT_TRUE(blast.search.graph);
    EXPECT_EQ(blast.reference.faceCandidates, 1u);
    EXPECT_EQ(blast.search.facePieces, 1u);
    EXPECT_EQ(blast.search.attachedByUnloaded, 0u);
    EXPECT_EQ(blast.search.referenceMismatch, 0u);
    EXPECT_EQ(a.World().GetVoxel({1, g + 30, 1}), 0);
    EXPECT_EQ(a.World().GetVoxel({1, g + 39, 1}), 0);
    EXPECT_EQ(a.World().GetVoxel({0, g, 0}), a.Ids().stone);     // the stump stays
}

// The symptom as the owner saw it: the second shot freed what the first had cut. Now the first
// shot frees it and the second finds nothing.
TEST(Blast, SecondShotStillFreesTheTowerItWouldHaveFreedToday) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const std::vector<sim::WorldOp> setup{Pillar(a.Ids(), 40)};
    Apply(a.World(), setup);
    const sim::BlastResult first = sim::Blast(a.simulation, kBaseCut, kStrong);
    EXPECT_EQ(first.IslandCount(), 1u);
    // A power-0 blast high on the pillar carves nothing; its island box holds the whole piece.
    const sim::BlastResult second = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(24, (g + 24) * 16 + 8, 24), 64}, 0);
    EXPECT_TRUE(second.islandsSearched);
    EXPECT_EQ(second.ops[0].result.changed, 0u);
    EXPECT_EQ(second.IslandCount(), 0u);
}

TEST(Blast, ShortPillarControl) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const std::vector<sim::WorldOp> setup{Pillar(a.Ids(), 25)};
    Apply(a.World(), setup);
    const sim::BlastResult blast = sim::Blast(a.simulation, kBaseCut, kStrong);
    ASSERT_EQ(blast.IslandCount(), 1u);
    EXPECT_EQ(blast.ops[1].op.mask.Count(), 17u * 9u - 1u);
    EXPECT_EQ(blast.reference.faceCandidates, 0u);  // inside the island box: no flood
    EXPECT_EQ(blast.search.graphFloods, 0u);
    EXPECT_EQ(blast.search.facePieces, 0u);
    EXPECT_EQ(a.World().GetVoxel({1, g + 20, 1}), 0);
}

TEST(Blast, LedgeCutAtItsRootFalls) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    // A wall (3 x 30 x 3 on the ground) and a 60-voxel ledge from its side along +x.
    const std::vector<sim::WorldOp> setup{sim::WorldOp::Box({0, g, 0}, {3, g + 30, 3}, ids.stone),
                                          sim::WorldOp::Box({3, g + 20, 0}, {63, g + 23, 3}, ids.stone)};
    Apply(a.World(), setup);
    // Cut at the root: x 3..9 of the ledge go, the wall keeps its place.
    const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(6 * 16 + 8, (g + 21) * 16 + 8, 24), 64}, kStrong);
    EXPECT_TRUE(blast.islandsSearched);
    ASSERT_EQ(blast.IslandCount(), 1u);
    EXPECT_EQ(blast.search.facePieces, 1u);
    EXPECT_EQ(a.World().GetVoxel({62, g + 21, 1}), 0);         // the ledge's end fell
    EXPECT_EQ(a.World().GetVoxel({1, g + 25, 1}), ids.stone);  // the wall stands
}

TEST(Blast, FaceFloodGivesUpOnTheGround) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const std::vector<sim::WorldOp> setup{Pillar(a.Ids(), 70)};
    Apply(a.World(), setup);
    // A power-0 blast high on the pillar: its island box (g+22..g+77) cuts the pillar at both
    // faces, and the pillar stands on the ground, which leaves the flood box.
    const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(24, (g + 50) * 16 + 8, 24), 64}, 0);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_EQ(blast.IslandCount(), 0u);
    EXPECT_GE(blast.reference.faceCandidates, 1u);
    EXPECT_EQ(blast.search.facePieces, 0u);
    EXPECT_GE(blast.reference.attachedByLimit, 1u);
    EXPECT_GE(blast.search.attachedByLimit + blast.search.attachedByFace, 1u);
    EXPECT_EQ(a.World().GetVoxel({1, g + 60, 1}), a.Ids().stone);
}

// The flood box is a function of the blast's integers: whether the chunks just outside it are
// loaded changes nothing (the server can guarantee it is loaded, offline never waits for more).
TEST(Blast, FaceFloodIndependentOfLoadingOutsideTheFloodBox) {
    // Window 6 chunks: voxels -96..95 = exactly the flood box of a blast centred in voxel 0.
    // Window 8 chunks: voxels -128..127, the chunks just outside the flood box are loaded.
    FloodArena small(FloodConfig(3, {6, 6, 6})), large(FloodConfig(3, {8, 6, 8}));
    ASSERT_TRUE(small.initialized && large.initialized);
    const voxel::WorldMaterials& ids = small.Ids();
    const std::vector<sim::WorldOp> setup{
        // P1: a pillar with an arm along -x that leaves the flood box (it ends at x = -110).
        sim::WorldOp::Box({0, g, 0}, {3, g + 30, 3}, ids.stone),
        sim::WorldOp::Box({-110, g + 20, 0}, {0, g + 23, 3}, ids.planks),
        // P2: a 40-voxel pillar 5 voxels away, free once its base is cut.
        sim::WorldOp::Box({5, g, 0}, {8, g + 40, 3}, ids.stone),
    };
    Apply(small.World(), setup, false);       // the small window does not hold x < -96
    Apply(large.World(), setup);
    ASSERT_FALSE(small.World().IsChunkLoaded({-4, 3, 0}));
    ASSERT_TRUE(large.World().IsChunkLoaded({-4, 3, 0}));

    const voxel::SphereQ cut{glm::ivec3(8, (g + 4) * 16 + 8, 8), 128};
    const sim::BlastResult a = sim::Blast(small.simulation, cut, kStrong);
    const sim::BlastResult b = sim::Blast(large.simulation, cut, kStrong);
    EXPECT_TRUE(a.islandsSearched);           // the flood never needed the missing chunk
    EXPECT_TRUE(b.islandsSearched);
    EXPECT_EQ(OpsOf(a), OpsOf(b));
    EXPECT_EQ(a.IslandCount(), 1u);           // P2
    EXPECT_EQ(a.reference.faceCandidates, b.reference.faceCandidates);
    EXPECT_EQ(a.reference.attachedByLimit, b.reference.attachedByLimit);
    EXPECT_EQ(a.reference.floodVisits, b.reference.floodVisits);
    EXPECT_GE(a.reference.attachedByLimit, 1u);  // P1 and its arm
    // The graph search alike: it never reads a chunk outside the flood box either.
    EXPECT_EQ(a.search.attachedByLimit, b.search.attachedByLimit);
    EXPECT_EQ(a.search.attachedByFace, b.search.attachedByFace);
    EXPECT_EQ(a.search.nodeVisits, b.search.nodeVisits);
    EXPECT_EQ(a.search.chunkGraphs, b.search.chunkGraphs);
    EXPECT_GE(a.search.attachedByLimit + a.search.attachedByFace, 1u);
    EXPECT_EQ(small.World().GetVoxel({-50, g + 21, 1}), ids.planks);
}

TEST(Blast, FaceFloodDefersOfflineOnAnUnloadedChunk) {
    FloodArena a(FloodConfig(), std::make_shared<UnclassifiedGround>());
    ASSERT_TRUE(a.initialized);
    // A pillar to the top of the window (row 4 ends at y = 159).
    const std::vector<sim::WorldOp> setup{Pillar(a.Ids(), 80)};
    Apply(a.World(), setup);
    // The window moves up one chunk row: row 5 (y 160..191), right above the pillar, is loading.
    const glm::dvec3 up = kFocus + glm::dvec3(0.0, 3.2, 0.0);
    a.World().Update(up);
    ASSERT_FALSE(a.World().IsChunkLoaded({0, 5, 0}));
    ASSERT_TRUE(a.World().IsChunkLoaded({0, 4, 0}));

    const sim::BlastResult blast = sim::Blast(a.simulation, kBaseCut, kStrong);
    EXPECT_GT(blast.ops[0].result.changed, 0u);
    EXPECT_FALSE(blast.islandsSearched);
    EXPECT_TRUE(blast.search.floodDeferred);
    EXPECT_EQ(blast.IslandCount(), 0u);
    EXPECT_EQ(a.World().GetVoxel({1, g + 30, 1}), a.Ids().stone);       // still up: nothing was proven
    ASSERT_EQ(a.simulation.DeferredIslandSearches().size(), 1u);
    EXPECT_TRUE(sim::RetryIslandSearches(a.simulation).empty());        // the chunk is not there yet

    a.World().LoadBlocking(up);
    const std::vector<sim::BlastResult> late = sim::RetryIslandSearches(a.simulation);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_TRUE(late[0].deferred);
    EXPECT_TRUE(late[0].islandsSearched);
    EXPECT_FALSE(late[0].carveReplayed);
    EXPECT_EQ(late[0].IslandCount(), 1u);
    EXPECT_EQ(late[0].search.facePieces, 1u);
    EXPECT_EQ(a.World().GetVoxel({1, g + 30, 1}), 0);
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
}

// The server's answer to the same flood (shred.md rule 6): no deferral, the candidate stays
// attached, the error counter says so.
TEST(Blast, FaceFloodOnAServerCountsAnUnloadedChunk) {
    FloodArena a(sim::ServerConfig(FloodConfig()), std::make_shared<UnclassifiedGround>());
    ASSERT_TRUE(a.initialized);
    const std::vector<sim::WorldOp> setup{Pillar(a.Ids(), 80)};
    Apply(a.World(), setup);
    a.World().Update(kFocus + glm::dvec3(0.0, 3.2, 0.0));
    ASSERT_FALSE(a.World().IsChunkLoaded({0, 5, 0}));

    const sim::BlastResult blast = sim::Blast(a.simulation, kBaseCut, kStrong);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_FALSE(blast.search.floodDeferred);
    EXPECT_EQ(blast.search.attachedByUnloaded, 1u);
    EXPECT_EQ(blast.IslandCount(), 0u);
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
    EXPECT_EQ(a.World().GetVoxel({1, g + 30, 1}), a.Ids().stone);
}

// A server never waits for an island box either: it is an error (counted, logged), not a deferral.
TEST(Blast, ServerNeverDefersAnIslandSearch) {
    FloodArena a(sim::ServerConfig(FloodConfig()));
    ASSERT_TRUE(a.initialized);
    const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(89 * 16 + 8, (g + 8) * 16 + 8, 24), 40}, 0);
    EXPECT_FALSE(blast.islandsSearched);
    EXPECT_EQ(blast.search.islandBoxUnloaded, 1u);
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
}

// Below the bottom row of a full-height window nothing is ever loaded: those rows are the ground
// of the world (the flood already treats them so), never "not loaded". Before, a blast within
// r/16 + 24 voxels of the bottom deferred its search for good.
TEST(Blast, IslandBoxBelowTheBottomOfAFullHeightWindowIsSearchedAtOnce) {
    sim::SimulationConfig config = FloodConfig();
    config.world.fullHeight = true;
    config.world.heightRange = {6.4f, 19.2f};       // rows 2..5: voxels y 64..191
    config.world.heightSnap = 3.2f;
    FloodArena a(config);
    ASSERT_TRUE(a.initialized);
    ASSERT_TRUE(a.World().FullHeight());
    ASSERT_EQ(a.World().WindowMinChunk().y, 2);
    const std::vector<sim::WorldOp> setup{Pillar(a.Ids(), 40)};
    Apply(a.World(), setup);

    // The island box is y g-24..g+31 = 56..111: its bottom 8 rows lie below the window.
    const sim::BlastResult blast = sim::Blast(a.simulation, kBaseCut, kStrong);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_EQ(blast.IslandCount(), 1u);
    EXPECT_EQ(blast.search.facePieces, 1u);
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
    EXPECT_EQ(a.World().GetVoxel({1, g + 30, 1}), 0);
    EXPECT_EQ(a.World().GetVoxel({1, g - 10, 1}), a.Ids().stone);    // the ground stays
}

// A session server's window is its play bounds plus sim::kServerWindowMarginChunks chunks: every
// flood box of a cast centred inside the play bounds is loaded, so no flood ever meets a chunk
// that is not loaded, even with structures that reach past the play bounds.
TEST(Blast, ServerFloodBoxAlwaysLoaded) {
    // Window 8 x 8 x 8 chunks around chunk (0, 3, 0): chunks (-4..3, -1..6, -4..3). Play bounds =
    // the window minus 3 chunks on every side: chunks (-1..0, 2..3, -1..0) = voxels x, z -32..31,
    // y 64..127.
    FloodArena a(sim::ServerConfig(FloodConfig(3, {8, 8, 8})), Ground(), false);
    ASSERT_TRUE(a.initialized);
    const glm::dvec3 focus(1.6, 11.2, 1.6);
    a.World().LoadBlocking(focus);
    ASSERT_EQ(a.World().WindowMinChunk(), glm::ivec3(-4, -1, -4));
    const glm::ivec3 playMin = voxel::ChunkToVoxel(a.World().WindowMinChunk() + glm::ivec3(sim::kServerWindowMarginChunks));
    const glm::ivec3 playMax = voxel::ChunkToVoxel(a.World().WindowMinChunk() + a.World().WindowSize() - glm::ivec3(sim::kServerWindowMarginChunks));
    ASSERT_EQ(playMin, glm::ivec3(-32, 64, -32));
    ASSERT_EQ(playMax, glm::ivec3(32, 128, 32));
    const glm::ivec3 windowMin = voxel::ChunkToVoxel(a.World().WindowMinChunk());
    const glm::ivec3 windowMax = windowMin + a.World().WindowSize() * voxel::kChunkSize;

    // The geometry: the flood box of a cast at any corner of the play bounds lies in the window.
    for (int corner = 0; corner < 8; ++corner) {
        const glm::ivec3 v((corner & 1) ? playMax.x - 1 : playMin.x, (corner & 2) ? playMax.y - 1 : playMin.y,
                           (corner & 4) ? playMax.z - 1 : playMin.z);
        glm::ivec3 mn, mx;
        sim::FloodBox(voxel::SphereQ{v * 16 + glm::ivec3(15), 64}, mn, mx);
        EXPECT_TRUE(glm::all(glm::greaterThanEqual(mn, windowMin)) && glm::all(glm::lessThanEqual(mx, windowMax))) << corner;
    }

    // And in use: at each ground corner of the play bounds a pillar with arms that run outwards
    // past the play bounds to the end of the window; the cast cuts the pillar, the floods follow
    // the arms to the flood box face and stop there, never at a chunk that is not loaded.
    const voxel::WorldMaterials& ids = a.Ids();
    uint32_t limit = 0;
    for (int corner = 0; corner < 4; ++corner) {
        const int x = (corner & 1) ? playMax.x - 3 : playMin.x;
        const int z = (corner & 2) ? playMax.z - 3 : playMin.z;
        const int sx = (corner & 1) ? 1 : -1, sz = (corner & 2) ? 1 : -1;
        std::vector<sim::WorldOp> ops{sim::WorldOp::Box({x, g, z}, {x + 3, g + 30, z + 3}, ids.stone)};
        ops.push_back(sx > 0 ? sim::WorldOp::Box({x + 3, g + 20, z}, {windowMax.x, g + 23, z + 3}, ids.planks)
                             : sim::WorldOp::Box({windowMin.x, g + 20, z}, {x, g + 23, z + 3}, ids.planks));
        ops.push_back(sz > 0 ? sim::WorldOp::Box({x, g + 26, z + 3}, {x + 3, g + 29, windowMax.z}, ids.planks)
                             : sim::WorldOp::Box({x, g + 26, windowMin.z}, {x + 3, g + 29, z}, ids.planks));
        Apply(a.World(), ops);
        const sim::BlastResult blast =
            sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(x * 16 + 24, (g + 4) * 16 + 8, z * 16 + 24), 64}, kStrong);
        EXPECT_TRUE(blast.islandsSearched) << corner;
        EXPECT_FALSE(blast.search.floodDeferred) << corner;
        EXPECT_EQ(blast.search.attachedByUnloaded, 0u) << corner;
        EXPECT_EQ(blast.IslandCount(), 0u) << corner;
        EXPECT_EQ(blast.reference.attachedByUnloaded, 0u) << corner;
        limit += blast.search.attachedByLimit + blast.search.attachedByFace;
    }
    EXPECT_GE(limit, 4u);
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
}

namespace {
    /// Four floating slabs of 50 x 25 x 50 = 62 500 voxels (over kMaxIslandVoxels) around a blast
    /// at (0, 112, 0), each with a 1-voxel rod reaching 18 voxels into its island box (x, z
    /// -28..27, y 84..139), and one small real island (5 + 27 voxels) crossing the box's top face.
    std::vector<sim::WorldOp> SlabsAndASmallIsland(const voxel::WorldMaterials& ids) {
        return {
            sim::WorldOp::Box({30, 100, -25}, {80, 125, 25}, ids.stone),   sim::WorldOp::Box({10, 112, 0}, {30, 113, 1}, ids.stone),
            sim::WorldOp::Box({-80, 100, -25}, {-30, 125, 25}, ids.stone), sim::WorldOp::Box({-30, 112, 0}, {-10, 113, 1}, ids.stone),
            sim::WorldOp::Box({-25, 100, 30}, {25, 125, 80}, ids.stone),   sim::WorldOp::Box({0, 112, 10}, {1, 113, 30}, ids.stone),
            sim::WorldOp::Box({-25, 100, -80}, {25, 125, -30}, ids.stone), sim::WorldOp::Box({0, 112, -30}, {1, 113, -10}, ids.stone),
            sim::WorldOp::Box({20, 136, 20}, {21, 141, 21}, ids.wood),     sim::WorldOp::Box({19, 141, 19}, {22, 144, 22}, ids.wood),
        };
    }
}

TEST(Blast, SmallIslandProvenBeforeTheBudgetIsSpent) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    // Four over-limit slabs whose rods reach into the island box, and a small real island.
    const std::vector<sim::WorldOp> setup = SlabsAndASmallIsland(ids);
    Apply(a.World(), setup);
    static_assert(sim::kFloodVisitBudget == 4 * sim::kMaxIslandVoxels);
    const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(8, 112 * 16 + 8, 8), 64}, 0);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_EQ(blast.reference.faceCandidates, 5u);
    ASSERT_EQ(blast.IslandCount(), 1u);
    EXPECT_EQ(blast.search.facePieces, 1u);
    EXPECT_EQ(blast.ops[1].op.mask.Count(), 5u + 27u);
    // Voxel search: the small island costs 32 visits; three slabs give up by limit at 60 001
    // visits each (180 035 in all); the fourth runs out of budget at visit 240 001.
    EXPECT_EQ(blast.reference.attachedByLimit, 3u);
    EXPECT_EQ(blast.reference.attachedByBudget, 1u);
    EXPECT_EQ(blast.reference.floodVisits, sim::kFloodVisitBudget + 1u);
    // Graph search: four slab floods of about 120 nodes each give up by the size limit, far
    // inside its node budget.
    EXPECT_EQ(blast.search.attachedByLimit, 4u);
    EXPECT_EQ(blast.search.attachedByBudget, 0u);
    EXPECT_LT(blast.search.nodeVisits, sim::kGraphNodeVisitBudget / 8);
    EXPECT_EQ(a.World().GetVoxel({20, 142, 20}), 0);
    EXPECT_EQ(a.World().GetVoxel({50, 110, 0}), ids.stone);
}

// Server A blasts, client B only applies: equal worlds, and B's piece has A's voxels.
TEST(Blast, FaceFloodTwoSimulationsEqualHashes) {
    FloodArena server(FloodConfig(3)), client(FloodConfig(1));
    ASSERT_TRUE(server.initialized && client.initialized);
    const voxel::WorldMaterials& ids = server.Ids();
    const std::vector<sim::WorldOp> setup{Pillar(ids, 40), sim::WorldOp::Box({3, g + 30, 0}, {40, g + 33, 3}, ids.planks)};
    Apply(server.World(), setup);
    Apply(client.World(), setup);
    ASSERT_EQ(server.Hash().hash, client.Hash().hash);

    const sim::BlastResult blast = sim::Blast(server.simulation, kBaseCut, kStrong);
    ASSERT_EQ(blast.search.facePieces, 1u);
    for (const sim::WorldOp& op : OpsOf(blast)) {
        std::vector<voxel::VoxelEdit> edits;
        const sim::OpResult r = sim::ApplyOp(client.World(), op, &edits);
        EXPECT_TRUE(r.Complete());
        if (op.kind == sim::WorldOpKind::Detach) EXPECT_EQ(sim::BuildDetachedPiece(op, edits).CountSolid(), op.mask.Count());
    }
    const voxel::WorldHashResult s = server.Hash(), c = client.Hash();
    ASSERT_TRUE(s.Complete() && c.Complete());
    EXPECT_EQ(s.hash, c.hash);
}

// The budget counts voxel visits, never time: the same counters and operations with one and with
// three workers (load and collider timing differ), in a forest of trunks that stop at each other
// and in four over-limit slabs that spend the whole budget.
TEST(Blast, FaceFloodBudgetIsCountedInVisits) {
    {
        FloodArena one(FloodConfig(1)), three(FloodConfig(3));
        ASSERT_TRUE(one.initialized && three.initialized);
        const voxel::WorldMaterials& ids = one.Ids();
        std::vector<sim::WorldOp> setup;
        for (int i = 0; i < 20; ++i) {
            const int x = -20 + (i % 5) * 9, z = -16 + (i / 5) * 9;
            setup.push_back(sim::WorldOp::Box({x, g, z}, {x + 2, g + 70, z + 2}, ids.wood));
        }
        Apply(one.World(), setup);
        Apply(three.World(), setup);
        const voxel::SphereQ cut{glm::ivec3(8, (g + 40) * 16 + 8, 8), 64};
        const sim::BlastResult a = sim::Blast(one.simulation, cut, 0);
        const sim::BlastResult b = sim::Blast(three.simulation, cut, 0);
        EXPECT_EQ(OpsOf(a), OpsOf(b));
        EXPECT_EQ(a.reference.faceCandidates, 20u);
        EXPECT_EQ(a.reference.faceCandidates, b.reference.faceCandidates);
        EXPECT_EQ(a.reference.attachedByLimit, b.reference.attachedByLimit);
        EXPECT_EQ(a.reference.attachedByKnown, b.reference.attachedByKnown);
        EXPECT_EQ(a.reference.floodVisits, b.reference.floodVisits);
        // The first trunk floods into the ground and gives up; the others stop at its voxels.
        EXPECT_EQ(a.reference.attachedByLimit, 1u);
        EXPECT_EQ(a.reference.attachedByKnown, 19u);
        // The graph search counts nodes, the same with one and three workers.
        EXPECT_EQ(a.search.boxComponents, b.search.boxComponents);
        EXPECT_EQ(a.search.graphFloods, b.search.graphFloods);
        EXPECT_EQ(a.search.attachedByLimit, b.search.attachedByLimit);
        EXPECT_EQ(a.search.attachedByFace, b.search.attachedByFace);
        EXPECT_EQ(a.search.attachedByKnown, b.search.attachedByKnown);
        EXPECT_EQ(a.search.nodeVisits, b.search.nodeVisits);
        EXPECT_GT(a.search.nodeVisits, 0u);
        EXPECT_EQ(a.IslandCount(), 0u);
    }
    // The budget itself is reached: the same visit at which it runs out, whatever the workers.
    FloodArena one(FloodConfig(1)), three(FloodConfig(3));
    ASSERT_TRUE(one.initialized && three.initialized);
    Apply(one.World(), SlabsAndASmallIsland(one.Ids()));
    Apply(three.World(), SlabsAndASmallIsland(three.Ids()));
    const voxel::SphereQ cut{glm::ivec3(8, 112 * 16 + 8, 8), 64};
    const sim::BlastResult a = sim::Blast(one.simulation, cut, 0);
    const sim::BlastResult b = sim::Blast(three.simulation, cut, 0);
    EXPECT_EQ(OpsOf(a), OpsOf(b));
    EXPECT_GE(a.reference.attachedByBudget, 1u);
    EXPECT_EQ(a.reference.attachedByBudget, b.reference.attachedByBudget);
    EXPECT_EQ(a.reference.attachedByLimit, b.reference.attachedByLimit);
    EXPECT_EQ(a.reference.floodVisits, sim::kFloodVisitBudget + 1u);
    EXPECT_EQ(b.reference.floodVisits, sim::kFloodVisitBudget + 1u);
    EXPECT_EQ(a.search.nodeVisits, b.search.nodeVisits);
    EXPECT_EQ(a.search.attachedByLimit, b.search.attachedByLimit);
}

// --- stage 4: the graph search gives the stage-1 results ------------------------------------------

namespace {
    /// One stage-1 test case: a world, its setup and its casts.
    struct Stage1Case {
        const char* name;
        std::vector<sim::WorldOp> setup;
        std::vector<std::pair<voxel::SphereQ, uint16_t>> casts;
        sim::SimulationConfig config = FloodConfig();
        bool unclassified = false;           // UnclassifiedGround, window moved up before the casts
    };

    std::vector<Stage1Case> Stage1Cases(const voxel::WorldMaterials& ids) {
        std::vector<Stage1Case> cases;
        const auto Add = [&cases](const char* name, std::vector<sim::WorldOp> setup, std::vector<std::pair<voxel::SphereQ, uint16_t>> casts) {
            cases.push_back({name, std::move(setup), std::move(casts)});
            return &cases.back();
        };
        Add("pillar 40, base cut", {Pillar(ids, 40)}, {{kBaseCut, kStrong}});
        Add("second shot", {Pillar(ids, 40)}, {{kBaseCut, kStrong}, {voxel::SphereQ{glm::ivec3(24, (g + 24) * 16 + 8, 24), 64}, 0}});
        Add("short pillar", {Pillar(ids, 25)}, {{kBaseCut, kStrong}});
        Add("ledge", {sim::WorldOp::Box({0, g, 0}, {3, g + 30, 3}, ids.stone), sim::WorldOp::Box({3, g + 20, 0}, {63, g + 23, 3}, ids.stone)},
            {{voxel::SphereQ{glm::ivec3(6 * 16 + 8, (g + 21) * 16 + 8, 24), 64}, kStrong}});
        Add("ground", {Pillar(ids, 70)}, {{voxel::SphereQ{glm::ivec3(24, (g + 50) * 16 + 8, 24), 64}, 0}});
        Add("arm out of the flood box",
            {sim::WorldOp::Box({0, g, 0}, {3, g + 30, 3}, ids.stone), sim::WorldOp::Box({-95, g + 20, 0}, {0, g + 23, 3}, ids.planks),
             sim::WorldOp::Box({5, g, 0}, {8, g + 40, 3}, ids.stone)},
            {{voxel::SphereQ{glm::ivec3(8, (g + 4) * 16 + 8, 8), 128}, kStrong}});
        Add("slabs and a small island", SlabsAndASmallIsland(ids), {{voxel::SphereQ{glm::ivec3(8, 112 * 16 + 8, 8), 64}, 0}});
        Add("pillar with a plank arm", {Pillar(ids, 40), sim::WorldOp::Box({3, g + 30, 0}, {40, g + 33, 3}, ids.planks)}, {{kBaseCut, kStrong}});
        {
            std::vector<sim::WorldOp> forest;
            for (int i = 0; i < 20; ++i) {
                const int x = -20 + (i % 5) * 9, z = -16 + (i / 5) * 9;
                forest.push_back(sim::WorldOp::Box({x, g, z}, {x + 2, g + 70, z + 2}, ids.wood));
            }
            Add("forest", forest, {{voxel::SphereQ{glm::ivec3(8, (g + 40) * 16 + 8, 8), 64}, 0}});
        }
        Add("two legs and a bridge",
            {sim::WorldOp::Box({0, g, 0}, {3, g + 40, 3}, ids.stone), sim::WorldOp::Box({6, g, 0}, {9, g + 40, 3}, ids.stone),
             sim::WorldOp::Box({0, g + 40, 0}, {9, g + 43, 3}, ids.stone)},
            {{voxel::SphereQ{glm::ivec3(72, (g + 4) * 16 + 8, 24), 96}, kStrong}});
        for (const bool oneMore : {false, true}) {
            std::vector<sim::WorldOp> setup{Pillar(ids, 40), sim::WorldOp::Box({-19, g + 40, -19}, {21, g + 77, 21}, ids.stone),
                                            sim::WorldOp::Box({-19, g + 77, -19}, {8, g + 78, 0}, ids.stone)};
            if (oneMore) setup.push_back(sim::WorldOp::Box({20, g + 77, 20}, {21, g + 78, 21}, ids.stone));
            Add(oneMore ? "60 001 voxels" : "60 000 voxels", setup, {{kBaseCut, kStrong}});
        }
        Add("water channel",
            {Pillar(ids, 40), sim::WorldOp::Box({3, g + 20, 0}, {60, g + 22, 3}, ids.water), sim::WorldOp::Box({60, g, 0}, {63, g + 22, 3}, ids.stone)},
            {{kBaseCut, kStrong}});
        Add("oversize island", {sim::WorldOp::Box({-20, 96, -20}, {20, 134, 20}, ids.stone)}, {{voxel::SphereQ{glm::ivec3(8, 115 * 16 + 8, 8), 0}, 0}});
        {
            Stage1Case* c = Add("full-height bottom", {Pillar(ids, 40)}, {{kBaseCut, kStrong}});
            c->config.world.fullHeight = true;
            c->config.world.heightRange = {6.4f, 19.2f};
            c->config.world.heightSnap = 3.2f;
        }
        {
            Stage1Case* c = Add("server, unloaded chunk", {Pillar(ids, 80)}, {{kBaseCut, kStrong}});
            c->config = sim::ServerConfig(FloodConfig());
            c->unclassified = true;
        }
        Add("offline, unloaded chunk", {Pillar(ids, 80)}, {{kBaseCut, kStrong}})->unclassified = true;
        return cases;
    }
}

// shred.md stage 4's acceptance: every stage-1 case gives the same operations, in the same order,
// and the same world with the graph search as with the voxel search - two independent
// simulations, late searches included. (Every stage-1 test above also compares both searches on
// the same voxels: FloodConfig sets checkIslandSearch.)
TEST(Structure, GraphFloatingEqualsFaceFlood) {
    const voxel::WorldMaterials ids = DefaultIds();
    uint32_t detaches = 0, late = 0;
    for (const Stage1Case& c : Stage1Cases(ids)) {
        std::vector<std::vector<sim::WorldOp>> ops[2];
        uint64_t hash[2] = {};
        for (int k = 0; k < 2; ++k) {
            sim::SimulationConfig config = c.config;
            config.checkIslandSearch = false;
            config.neckRule = false;          // two island searches compared op for op (the reference has no load check)
            config.islandSearch = k == 0 ? sim::IslandSearchKind::Graph : sim::IslandSearchKind::Voxel;
            FloodArena a(config, c.unclassified ? std::shared_ptr<voxel::IWorldGenerator>(std::make_shared<UnclassifiedGround>()) : Ground());
            a.expectChecked = false;
            ASSERT_TRUE(a.initialized) << c.name;
            Apply(a.World(), c.setup, false);
            const glm::dvec3 up = kFocus + glm::dvec3(0.0, 3.2, 0.0);
            if (c.unclassified) a.World().Update(up);
            for (const auto& [sphere, power] : c.casts) {
                const sim::BlastResult blast = sim::Blast(a.simulation, sphere, power);
                EXPECT_EQ(blast.search.graph, k == 0) << c.name;
                ops[k].push_back(OpsOf(blast));
            }
            if (c.unclassified) {
                a.World().LoadBlocking(up);
                for (const sim::BlastResult& r : sim::RetryIslandSearches(a.simulation)) ops[k].push_back(OpsOf(r));
            }
            hash[k] = a.Hash().hash;
        }
        ASSERT_EQ(ops[0].size(), ops[1].size()) << c.name;
        for (size_t i = 0; i < ops[0].size(); ++i) {
            EXPECT_EQ(ops[0][i], ops[1][i]) << c.name << ", result " << i;
            for (const sim::WorldOp& op : ops[0][i]) detaches += op.kind == sim::WorldOpKind::Detach ? 1u : 0u;
        }
        late += static_cast<uint32_t>(ops[0].size() - c.casts.size());
        EXPECT_EQ(hash[0], hash[1]) << c.name;
    }
    EXPECT_GE(detaches, 12u);
    EXPECT_GE(late, 1u);
}

// --- stage 1b: the carve of a blast into chunks that were still loading --------------------------

TEST(Blast, CarveIntoLoadingChunkIsReplayedBeforeTheLateSearch) {
    FloodArena a(FloodConfig(), std::make_shared<MushroomGround>(), false);
    ASSERT_TRUE(a.initialized);
    a.World().Update(kFocus);                          // the window is placed; nothing has loaded
    ASSERT_FALSE(a.World().IsChunkLoaded({0, 2, 0}));
    const voxel::SphereQ stemCut{glm::ivec3(24, (g + 8) * 16 + 8, 24), 64};
    const sim::BlastResult blast = sim::Blast(a.simulation, stemCut, kStrong);
    EXPECT_EQ(blast.ops[0].result.changed, 0u);
    EXPECT_FALSE(blast.ops[0].result.report.skipped.empty());
    EXPECT_FALSE(blast.islandsSearched);

    a.World().LoadBlocking(kFocus);
    EXPECT_EQ(a.World().GetVoxel({1, g + 8, 1}), a.Ids().stone);       // generated, never carved
    const std::vector<sim::BlastResult> late = sim::RetryIslandSearches(a.simulation);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_TRUE(late[0].deferred);
    EXPECT_TRUE(late[0].carveReplayed);
    ASSERT_GE(late[0].ops.size(), 2u);
    EXPECT_EQ(late[0].ops[0].op, sim::WorldOp::Carve(stemCut, kStrong));
    EXPECT_GT(late[0].ops[0].result.changed, 0u);
    EXPECT_TRUE(late[0].Report().Complete());
    EXPECT_EQ(late[0].IslandCount(), 1u);
    EXPECT_EQ(a.World().GetVoxel({1, g + 8, 1}), 0);
    EXPECT_EQ(a.World().GetVoxel({0, g + 17, 0}), 0);                   // the cap fell
    EXPECT_EQ(a.World().GetVoxel({1, g + 1, 1}), a.Ids().stone);       // the stump stays
}

// The replay covers only the chunks the blast's carve skipped, as soon as they load: an edit made
// inside the crater after the blast, in a chunk the carve did reach, survives it. Before, the
// whole sphere was carved again when the search ran.
TEST(Blast, CarveReplayKeepsEditsMadeAfterTheBlast) {
    FloodArena a(FloodConfig(), std::make_shared<UnclassifiedGround>());
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    const std::vector<sim::WorldOp> setup{Pillar(ids, 80)};      // y g..159, the top of row 4
    Apply(a.World(), setup);
    const glm::dvec3 up = kFocus + glm::dvec3(0.0, 3.2, 0.0);
    a.World().Update(up);                                        // row 5 (y 160..191) is loading
    ASSERT_FALSE(a.World().IsChunkLoaded({0, 5, 0}));

    const voxel::SphereQ top{glm::ivec3(24, 160 * 16, 24), 64};
    const sim::BlastResult blast = sim::Blast(a.simulation, top, kStrong);
    ASSERT_GT(blast.ops[0].result.changed, 0u);
    ASSERT_FALSE(blast.ops[0].result.report.skipped.empty());
    ASSERT_FALSE(blast.islandsSearched);
    ASSERT_EQ(a.World().GetVoxel({1, 158, 1}), 0);
    // Someone fills part of the crater in the loaded chunk, on what is left of the pillar (row 155).
    ASSERT_EQ(a.World().GetVoxel({1, 155, 1}), ids.stone);
    ASSERT_EQ(a.World().FillBox({1, 156, 1}, {2, 159, 2}, ids.planks), 3u);

    a.World().LoadBlocking(up);
    const std::vector<sim::BlastResult> late = sim::RetryIslandSearches(a.simulation);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_TRUE(late[0].carveReplayed);
    EXPECT_TRUE(late[0].islandsSearched);
    EXPECT_EQ(a.World().GetVoxel({1, 158, 1}), ids.planks) << "the replayed carve removed an edit made after the blast";
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
}

// --- flood edge cases -----------------------------------------------------------------------------

// One piece that crosses the island box face twice: two candidates, one flood, one Detach.
TEST(Blast, FaceFloodMergesAPieceThatCrossesTheFaceTwice) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    // Two 3 x 40 x 3 legs at x 0..2 and 6..8 joined by a bridge above the island box (top row g+33).
    const std::vector<sim::WorldOp> setup{sim::WorldOp::Box({0, g, 0}, {3, g + 40, 3}, ids.stone),
                                          sim::WorldOp::Box({6, g, 0}, {9, g + 40, 3}, ids.stone),
                                          sim::WorldOp::Box({0, g + 40, 0}, {9, g + 43, 3}, ids.stone)};
    Apply(a.World(), setup);
    // Radius 6 voxels around (4.5, g+4.5, 1.5): rows g..g+8 of both legs go.
    const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(72, (g + 4) * 16 + 8, 24), 96}, kStrong);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_EQ(blast.reference.faceCandidates, 2u);
    EXPECT_EQ(blast.reference.merged, 1u);
    EXPECT_EQ(blast.search.graphFloods, 1u);       // the legs' two box components: one flood
    EXPECT_EQ(blast.search.merged, 1u);
    EXPECT_EQ(blast.search.facePieces, 1u);
    ASSERT_EQ(blast.IslandCount(), 1u);
    EXPECT_EQ(blast.ops[1].result.changed, blast.ops[1].op.mask.Count());
    EXPECT_GT(blast.ops[1].op.mask.Count(), 2u * 25u * 9u + 81u);
    // Both legs above the crater and the bridge went with the one piece.
    uint32_t left = 0;
    for (int y = g + 12; y < g + 43; ++y)
        for (int z = 0; z < 3; ++z)
            for (int x = 0; x < 9; ++x) left += a.World().GetVoxel({x, y, z}) != 0 ? 1u : 0u;
    EXPECT_EQ(left, 0u);
}

// The flood's size limit is the island box's: a piece of exactly kMaxIslandVoxels is detached,
// one voxel more and it stays (attached by limit).
TEST(Blast, FaceFloodLimitIsTheInBoxLimit) {
    for (const bool oneMore : {false, true}) {
        FloodArena a;
        ASSERT_TRUE(a.initialized);
        const voxel::WorldMaterials& ids = a.Ids();
        // Above the cut: 287 voxels of pillar, 40 x 37 x 40 = 59 200 of slab and 27 x 1 x 19 = 513
        // on it: 60 000.
        std::vector<sim::WorldOp> setup{Pillar(ids, 40), sim::WorldOp::Box({-19, g + 40, -19}, {21, g + 77, 21}, ids.stone),
                                        sim::WorldOp::Box({-19, g + 77, -19}, {8, g + 78, 0}, ids.stone)};
        if (oneMore) setup.push_back(sim::WorldOp::Box({20, g + 77, 20}, {21, g + 78, 21}, ids.stone));
        Apply(a.World(), setup);
        static_assert(sim::kMaxIslandVoxels == 60000);
        const sim::BlastResult blast = sim::Blast(a.simulation, kBaseCut, kStrong);
        EXPECT_TRUE(blast.islandsSearched) << oneMore;
        EXPECT_EQ(blast.reference.faceCandidates, 1u) << oneMore;
        if (!oneMore) {
            ASSERT_EQ(blast.IslandCount(), 1u);
            EXPECT_EQ(blast.ops[1].op.mask.Count(), 60000u);
            EXPECT_EQ(blast.ops[1].result.changed, 60000u);
            EXPECT_EQ(a.World().GetVoxel({1, g + 30, 1}), 0);
        } else {
            EXPECT_EQ(blast.IslandCount(), 0u);
            EXPECT_EQ(blast.reference.attachedByLimit, 1u);
            EXPECT_EQ(blast.reference.floodVisits, 60001u);
            EXPECT_EQ(blast.search.attachedByLimit, 1u);
            EXPECT_EQ(a.World().GetVoxel({1, g + 30, 1}), ids.stone);
        }
    }
}

// Liquid neither holds a piece nor goes with it, inside the island box or outside it.
TEST(Blast, FaceFloodIgnoresWater) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    // The pillar's piece touches a water channel that runs out of the island box (x max 29) to a
    // stone post on the ground at x 60..62.
    const std::vector<sim::WorldOp> setup{Pillar(ids, 40), sim::WorldOp::Box({3, g + 20, 0}, {60, g + 22, 3}, ids.water),
                                          sim::WorldOp::Box({60, g, 0}, {63, g + 22, 3}, ids.stone)};
    Apply(a.World(), setup);
    const sim::BlastResult blast = sim::Blast(a.simulation, kBaseCut, kStrong);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_EQ(blast.search.facePieces, 1u);
    ASSERT_EQ(blast.IslandCount(), 1u);
    EXPECT_EQ(blast.ops[1].op.mask.Count(), 32u * 9u - 1u);     // the pillar's voxels only
    EXPECT_EQ(a.World().GetVoxel({1, g + 30, 1}), 0);
    EXPECT_EQ(a.World().GetVoxel({10, g + 20, 1}), ids.water);
    EXPECT_EQ(a.World().GetVoxel({61, g + 10, 1}), ids.stone);
}

// --- stage 1c: nothing silent ----------------------------------------------------------------------

TEST(Blast, DeferredSearchDropIsCounted) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    // Blasts at the window's +x edge: each island box reaches outside the window and waits. The
    // window's middle is voxel 0, so a search's distance grows with |z|. The oldest (z = 0) is the
    // nearest; z = -80 and z = +80 tie for the farthest.
    const auto BlastAt = [&a](int z) {
        return sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(89 * 16 + 8, (g + 8) * 16 + 8, z * 16 + 8), 40}, 0);
    };
    const auto Waiting = [&a](int z) {
        for (const sim::DeferredIslandSearch& s : a.simulation.DeferredIslandSearches())
            if (s.sphere.center.z == z * 16 + 8) return true;
        return false;
    };
    static constexpr int kZ[16] = {0, -80, -70, -60, -50, 80, -40, -30, -20, -10, 10, 20, 30, 40, 50, 60};
    for (const int z : kZ) {
        const sim::BlastResult blast = BlastAt(z);
        EXPECT_FALSE(blast.islandsSearched);
        EXPECT_EQ(blast.deferredDropped, 0u) << z;
    }
    ASSERT_EQ(a.simulation.DeferredIslandSearches().size(), sim::kMaxDeferredIslandSearches);

    // Full: the farthest goes, not the oldest; of two equally far, the older (-80 before +80).
    EXPECT_EQ(BlastAt(70).deferredDropped, 1u);
    EXPECT_EQ(a.simulation.DeferredIslandSearches().size(), sim::kMaxDeferredIslandSearches);
    EXPECT_TRUE(Waiting(0));
    EXPECT_FALSE(Waiting(-80));
    EXPECT_TRUE(Waiting(80));
    EXPECT_TRUE(Waiting(70));                 // the new search is kept, whatever its distance
    EXPECT_EQ(BlastAt(65).deferredDropped, 1u);
    EXPECT_FALSE(Waiting(80));
    EXPECT_TRUE(Waiting(0) && Waiting(70) && Waiting(65));
}

TEST(Blast, IslandOverMaxIslandVoxelsIsCounted) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    // 40 x 38 x 40 = 60 800 voxels floating in the air, inside the island box of a radius-0 blast.
    const std::vector<sim::WorldOp> setup{sim::WorldOp::Box({-20, 96, -20}, {20, 134, 20}, a.Ids().stone)};
    Apply(a.World(), setup);
    const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(8, 115 * 16 + 8, 8), 0}, 0);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_EQ(blast.IslandCount(), 0u);
    EXPECT_EQ(blast.search.oversizeIslands, 1u);
    EXPECT_EQ(a.World().GetVoxel({0, 115, 0}), a.Ids().stone);
}

TEST(Blast, DeferredSearchIsGivenUpOnlyOnceItsFloodBoxLeftTheWindow) {
    FloodArena a;
    ASSERT_TRUE(a.initialized);
    const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(89 * 16 + 8, (g + 8) * 16 + 8, 24), 40}, 0);
    ASSERT_FALSE(blast.islandsSearched);
    // Far longer than the old 600-call limit: still waiting while the window holds its flood box.
    for (int i = 0; i < 700; ++i) ASSERT_TRUE(sim::RetryIslandSearches(a.simulation).empty());
    EXPECT_EQ(a.simulation.DeferredIslandSearches().size(), 1u);
    // The window moves far away: given up, and said so.
    a.World().Update(glm::dvec3(-400.0, 8.0, 0.0));
    const std::vector<sim::BlastResult> late = sim::RetryIslandSearches(a.simulation);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_EQ(late[0].deferredGivenUp, 1u);
    EXPECT_TRUE(late[0].ops.empty());
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
}

// --- stage 0 / 4: the measurement cases (not a gate) -----------------------------------------------
// The playground's watch tower and a castle of FillBox walls at the game's 0.125 m, and a 1.5 m
// blast 6 m under the surface (the `--climb` case), on two grounds: flat (mostly Uniform chunks and
// blocks of one id: the fast paths of the graph build) and the built-in terrain generator with
// Packed chunks (caves, ores, mixed blocks: what the game builds graphs from). What one 1.2 m,
// power-6 shot costs in the island search, in a quiet process (no streaming, no renderer): the
// graph search (stage 4; its chunk graph builds counted separately) in one simulation, and the
// stage-1 voxel search in a second simulation with the same voxels and shots, so neither warms the
// CPU cache for the other (checkIslandSearch off). Three fresh worlds each. Run with
//   VortexSimTests --gtest_also_run_disabled_tests --gtest_filter='*CostOfTheTowerAndCastleCases*'

namespace {
    voxel::SphereQ ShotAt(const glm::dvec3& metres, double voxelSize) {
        return voxel::QuantizeSphere(metres / voxelSize, 1.2 / voxelSize);
    }

    struct CostShot {
        const char* name;
        voxel::SphereQ sphere;
        uint16_t power;
    };

    /// One measurement scene: a world of `window` chunks around `focus` (metres), built by `build`
    /// (FillBox ops from the site's corner), then `shots` in order.
    struct CostScene {
        const char* name;
        glm::ivec3 window;
        glm::dvec3 focus;
        std::function<void(voxel::World&, const voxel::WorldMaterials&)> build;
        std::vector<CostShot> shots;
    };

    struct CostRow {
        double searchMs = 0.0, buildMs = 0.0, floodMs = 0.0, relabelMs = 0.0, neckMs = 0.0, neckBuildMs = 0.0;
        uint32_t built = 0, used = 0, visits = 0, overfull = 0, islands = 0;
        uint32_t neckNodes = 0, neckLoads = 0, neckArcs = 0, neckExterior = 0, neckWork = 0, neckPieces = 0, neckSolid = 0;
        // The graph run only: the load check run on its world right after the shot (voxel::
        // SearchIslandsOnGraph, nothing applied), so its cost is measured on the same world state
        // as the search without it (review: the check's own run diverged once it released the tower).
        double probeMs = 0.0, probeBuildMs = 0.0;
        uint32_t probeNodes = 0, probeLoads = 0, probeVisits = 0, probeWork = 0, probeParts = 0, probeVoxels = 0, probeSolid = 0;
        uint64_t probeLoad = 0, probeCut = 0;
    };

    /// `neck`: the graph search with its load check (shred.md Part 4b); off for the hash comparison
    /// with the voxel search (the reference has no load check).
    std::vector<CostRow> RunCostScene(const CostScene& scene, bool terrain, sim::IslandSearchKind kind, uint64_t& hash, bool neck = false) {
        constexpr float vs = 0.125f;
        sim::SimulationConfig config = FloodConfig(3, scene.window);
        config.world.voxelSize = vs;
        config.world.packHidden = terrain;
        config.chunkCapacity = 8192;
        config.checkIslandSearch = false;
        config.islandSearch = kind;
        config.neckRule = neck;
        FloodArena a(config, terrain ? nullptr : Ground(), false);
        a.expectChecked = false;
        a.allowNeckReleases = neck;
        std::vector<CostRow> rows;
        if (!a.initialized) return rows;
        a.World().LoadBlocking(scene.focus);
        scene.build(a.World(), a.Ids());
        for (const CostShot& shot : scene.shots) {
            const sim::BlastResult blast = sim::Blast(a.simulation, shot.sphere, shot.power);
            CostRow row;
            row.searchMs = blast.times.searchMs + blast.times.floodMs;
            row.buildMs = blast.times.graphBuildMs;
            row.floodMs = blast.times.graphFloodMs;
            row.relabelMs = blast.times.relabelMs;
            row.built = blast.search.graphsBuilt;
            row.used = blast.search.chunkGraphs;
            row.visits = kind == sim::IslandSearchKind::Graph ? blast.search.nodeVisits : blast.search.floodVisits;
            row.overfull = blast.search.overfullCells;
            row.islands = blast.IslandCount();
            row.neckMs = blast.times.neckMs;
            row.neckNodes = blast.search.neckNodes;
            row.neckLoads = blast.search.neckLoads;
            row.neckArcs = blast.search.neckArcs;
            row.neckExterior = blast.search.neckExteriorVisits;
            row.neckWork = blast.search.neckFlowWork;
            row.neckPieces = blast.search.neckPieces + blast.search.neckCrumbs;
            row.neckBuildMs = blast.times.neckBuildMs;
            row.neckSolid = blast.search.neckSolidChunks;
            if (kind == sim::IslandSearchKind::Graph && !neck) {
                voxel::GraphSearchLimits limits;
                sim::IslandBox(shot.sphere, limits.boxMin, limits.boxMax);
                sim::FloodBox(shot.sphere, limits.floodMin, limits.floodMax);
                limits.maxIslandVoxels = sim::kMaxIslandVoxels;
                limits.maxNodeVisits = sim::kGraphNodeVisitBudget;
                const voxel::GraphMaterials materials = voxel::MakeGraphMaterials(&a.simulation.Materials(), voxel::MaterialFlag_Liquid);
                voxel::NeckLimits neckLimits = voxel::MakeNeckLimits(materials, a.World().VoxelSize());
                limits.neck = &neckLimits;
                const voxel::GraphSearchResult probe = voxel::SearchIslandsOnGraph(a.World(), a.simulation.TerrainGraphs(), materials, limits);
                row.probeMs = probe.neckMs;
                row.probeBuildMs = probe.neckBuildMs;
                row.probeNodes = probe.neckNodes;
                row.probeLoads = probe.neckLoads;
                row.probeVisits = probe.neckExteriorVisits;
                row.probeWork = probe.neckFlowWork;
                row.probeSolid = probe.neckSolidChunks;
                row.probeParts = static_cast<uint32_t>(probe.neckPieces.size());
                for (const voxel::NeckCut& c : probe.neckCuts) {
                    row.probeVoxels += c.voxels;
                    if (c.load > row.probeLoad) {
                        row.probeLoad = c.load;
                        row.probeCut = c.cut;
                    }
                }
            }
            rows.push_back(row);
        }
        hash = a.Hash().hash;
        return rows;
    }
}

TEST(Blast, DISABLED_CostOfTheTowerAndCastleCases) {
    constexpr float vs = 0.125f;
    const auto S = [](int units) { return static_cast<int>(std::lround(units * 0.8)); };      // 0.1 m units -> 0.125 m voxels
    const auto SV = [&S](glm::ivec3 v) { return glm::ivec3(S(v.x), S(v.y), S(v.z)); };
    voxel::TerrainSettings settings;
    settings.voxelSize = vs;
    for (const bool terrain : {false, true}) {
        // The ground under a site: y = g on flat ground; the generator's surface on the terrain.
        const auto GroundAt = [&](double x, double z) {
            return terrain ? static_cast<int>(std::floor(voxel::TerrainHeight(settings, static_cast<float>(x), static_cast<float>(z)) / vs)) + 1 : g;
        };
        std::vector<CostScene> scenes;
        {
            // The watch tower of src/main.cpp (BuildPlayground), its floor on the ground at the origin.
            const glm::ivec3 c(0, GroundAt(4.4, -5.8), 0);
            CostScene t{"tower", {8, 8, 8}, glm::dvec3(0.0, c.y * vs, 0.0), nullptr, {}};
            t.build = [c, SV](voxel::World& w, const voxel::WorldMaterials& m) {
                const auto box = [&](glm::ivec3 lo, glm::ivec3 hi, uint8_t id) { w.FillBox(c + SV(lo), c + glm::max(SV(hi), SV(lo) + 1), id); };
                box({30, 0, -60}, {58, 70, -32}, m.stone);
                box({33, 0, -57}, {55, 66, -35}, 0);
                box({41, 0, -35}, {47, 20, -32}, 0);
                box({28, 66, -62}, {60, 70, -30}, m.planks);
                for (int i = 0; i < 4; ++i) {
                    const glm::ivec3 p(i & 1 ? 58 : 28, 70, i & 2 ? -32 : -62);
                    box(p, p + glm::ivec3(2, 8, 2), m.wood);
                    box(p + glm::ivec3(0, 8, 0), p + glm::ivec3(2, 10, 2), m.torch);
                }
            };
            // The --climb case first: a 1.5 m blast 6 m under the surface.
            t.shots.push_back({"underground 1.5 m (cold)", voxel::QuantizeSphere(glm::dvec3(-40.0, c.y - 48.0, 40.0), 1.5 / vs),
                               voxel::QuantizePower(6.0f)});
            static constexpr glm::dvec2 kShots[] = {{31.5, -58.5}, {44.0, -58.5}, {56.5, -58.5}, {56.5, -46.0},
                                                    {56.5, -33.5}, {31.5, -33.5}, {31.5, -46.0}};
            static constexpr const char* kNames[] = {"tower ring shot 1", "tower ring shot 2", "tower ring shot 3", "tower ring shot 4",
                                                     "tower ring shot 5", "tower ring shot 6", "tower ring shot 7 (frees it)"};
            // Review: a dud where shot 1 lands, before it - does the tower stand on this ground before any shot?
            t.shots.push_back({"tower dud before shot 1 (cold)", ShotAt(glm::dvec3(c) * static_cast<double>(vs) + glm::dvec3(kShots[0].x, 5.0, kShots[0].y) * 0.1, vs),
                               0});
            for (size_t k = 0; k < 7; ++k) {
                const glm::dvec3 at = glm::dvec3(c) * static_cast<double>(vs) + glm::dvec3(kShots[k].x, 5.0, kShots[k].y) * 0.1;
                t.shots.push_back({kNames[k], ShotAt(at, vs), voxel::QuantizePower(6.0f)});
            }
            scenes.push_back(std::move(t));
        }
        {
            // The castle of src/main.cpp (BuildCastle), centred on the origin.
            const glm::ivec3 c(0, GroundAt(0.0, 0.0), -S(260));
            CostScene k{"castle", {10, 8, 10}, glm::dvec3(0.0, c.y * vs + 4.0, 0.0), nullptr, {}};
            k.build = [c, SV](voxel::World& w, const voxel::WorldMaterials& m) {
                const auto box = [&](glm::ivec3 lo, glm::ivec3 hi, uint8_t id) { w.FillBox(c + SV(lo), c + glm::max(SV(hi), SV(lo) + 1), id); };
                box({-100, 0, 160}, {100, 100, 165}, m.stone);
                box({-100, 0, 355}, {100, 100, 360}, m.stone);
                box({-100, 0, 160}, {-95, 100, 360}, m.stone);
                box({95, 0, 160}, {100, 100, 360}, m.stone);
                for (int i = 0; i < 4; ++i) {
                    const glm::ivec3 lo(i & 1 ? 78 : -118, 0, i & 2 ? 338 : 142);
                    box(lo, lo + glm::ivec3(40, 120, 40), m.stone);
                    box(lo + glm::ivec3(5, 0, 5), lo + glm::ivec3(35, 115, 35), 0);
                }
                box({-40, 0, 220}, {40, 120, 300}, m.brick);
                box({-35, 0, 225}, {35, 115, 295}, 0);
                for (int y : {30, 60, 90}) box({-35, y, 225}, {35, y + 2, 295}, m.planks);
            };
            static constexpr glm::dvec3 kAt[] = {{0.0, 50.0, 162.5}, {-37.5, 45.0, 260.0}, {115.5, 100.0, 357.5}};
            static constexpr const char* kName[] = {"castle curtain wall at 5 m (cold)", "castle keep wall at 4.5 m (cold)",
                                                    "castle corner tower at 10 m (cold)"};
            for (size_t i = 0; i < 3; ++i) {
                const glm::dvec3 at = glm::dvec3(c) * static_cast<double>(vs) + kAt[i] * 0.1;
                k.shots.push_back({kName[i], ShotAt(at, vs), voxel::QuantizePower(6.0f)});
            }
            scenes.push_back(std::move(k));
        }
        for (const CostScene& scene : scenes)
            for (int run = 0; run < 3; ++run) {
                uint64_t hash[3] = {};
                const std::vector<CostRow> graph = RunCostScene(scene, terrain, sim::IslandSearchKind::Graph, hash[0]);
                const std::vector<CostRow> voxel = RunCostScene(scene, terrain, sim::IslandSearchKind::Voxel, hash[1]);
                const std::vector<CostRow> neck = RunCostScene(scene, terrain, sim::IslandSearchKind::Graph, hash[2], true);
                ASSERT_EQ(graph.size(), scene.shots.size());
                ASSERT_EQ(voxel.size(), scene.shots.size());
                EXPECT_EQ(hash[0], hash[1]) << scene.name;
                for (size_t k = 0; k < graph.size(); ++k) {
                    const CostRow& r = graph[k];
                    std::printf("%s run %d %s: graph search %.2f ms (builds %.2f ms: %u of %u graphs, %.1f us each; floods %.2f ms, %u node "
                                "visits; re-label %.2f ms; %u over-full cells); voxel search %.2f ms (%u visits); %u / %u island(s)\n",
                                terrain ? "terrain" : "flat", run, scene.shots[k].name, r.searchMs, r.buildMs, r.built, r.used,
                                r.built ? 1000.0 * r.buildMs / r.built : 0.0, r.floodMs, r.visits, r.relabelMs, r.overfull,
                                voxel[k].searchMs, voxel[k].visits, r.islands, voxel[k].islands);
                    std::printf("%s run %d %s: the check on this world (nothing applied): %.2f ms + %.2f ms of graph builds (%u nodes, %u loads, "
                                "%u exterior visits, %u flow work, %u solid chunk(s)); would release %u part(s), %u voxels%s\n",
                                terrain ? "terrain" : "flat", run, scene.shots[k].name, r.probeMs, r.probeBuildMs, r.probeNodes, r.probeLoads,
                                r.probeVisits, r.probeWork, r.probeSolid, r.probeParts, r.probeVoxels,
                                r.probeParts ? std::format(" (heaviest: load {} > cut {} MU, {:.2f}x)", r.probeLoad, r.probeCut,
                                                           static_cast<double>(r.probeLoad) / static_cast<double>(std::max<uint64_t>(1, r.probeCut)))
                                                   .c_str()
                                             : "");
                    if (k < neck.size()) {
                        const CostRow& n = neck[k];
                        std::printf("%s run %d %s: with the load check%s: search %.2f ms of which check %.2f ms + builds %.2f ms (%u nodes, %u "
                                    "loads, %u arcs, %u exterior visits, %u flow work, %u released); %u Detach(es)\n",
                                    terrain ? "terrain" : "flat", run, scene.shots[k].name,
                                    std::any_of(neck.begin(), neck.begin() + static_cast<std::ptrdiff_t>(k), [](const CostRow& x) { return x.neckPieces > 0; })
                                        ? " (AFTER A RELEASE: another world)"
                                        : "",
                                    n.searchMs, n.neckMs, n.neckBuildMs, n.neckNodes, n.neckLoads, n.neckArcs, n.neckExterior, n.neckWork,
                                    n.neckPieces, n.islands);
                    }
                }
            }
    }
}
