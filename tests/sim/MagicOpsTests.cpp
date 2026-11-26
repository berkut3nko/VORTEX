// The map operations of magic (docs/design/magic.md "Apply", "Determinism traps"): MapShape and
// MapMask decide per voxel from the old id and the op's integers only, so a clipped application
// chunk by chunk, in any order, equals the whole one, and two simulations fed the same ops agree.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;

using namespace vortex;

namespace {
    constexpr float kVoxel = 0.1f;
    constexpr int kGroundVoxels = 80;                 // flat ground: its top is at y = 8 m
    const glm::dvec3 kFocus(0.0, 8.0, 0.0);           // window chunks (-3..2, -1..4, -3..2)

    sim::SimulationConfig YardConfig(unsigned jobThreads) {
        sim::SimulationConfig config;
        config.chunkCapacity = 512;
        config.maxParticles = 1024;
        config.jobThreads = jobThreads;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = {6, 6, 6};
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        return config;
    }

    std::shared_ptr<voxel::IWorldGenerator> FlatGround() {
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
        return voxel::CreateFlatGenerator(kGroundVoxels, ids.grass, ids.stone);
    }

    /// One simulation on flat ground with its window loaded.
    struct MagicYard {
        explicit MagicYard(unsigned jobThreads = 2) : simulation(YardConfig(jobThreads)) {
            initialized = simulation.Initialize(FlatGround());
            if (initialized) simulation.World().LoadBlocking(kFocus);
        }
        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        uint64_t Hash() const {
            const voxel::World& world = simulation.World();
            const voxel::WorldHashResult r = voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize());
            EXPECT_TRUE(r.Complete());
            return r.hash;
        }
        sim::Simulation simulation;
        bool initialized = false;
    };

    voxel::ShapeQ BoxShape(const glm::ivec3& min, const glm::ivec3& max) {
        voxel::ShapeQ s;
        s.kind = voxel::kShapeBox;
        s.a = min;
        s.b = max;
        return s;
    }

    voxel::ShapeQ SphereShape(const glm::ivec3& centreVoxel, int radiusVoxels) {
        voxel::ShapeQ s;
        s.kind = voxel::kShapeSphere;
        s.a = centreVoxel * 16;
        s.radiusQ = static_cast<uint16_t>(radiusVoxels * 16);
        return s;
    }

    voxel::ShapeQ CapsuleShape(const glm::ivec3& aVoxel, const glm::ivec3& bVoxel, int radiusVoxels) {
        voxel::ShapeQ s;
        s.kind = voxel::kShapeCapsule;
        s.a = aVoxel * 16 + glm::ivec3(3, 5, 7);   // off the voxel grid on purpose
        s.b = bVoxel * 16 + glm::ivec3(11, 2, 9);
        s.radiusQ = static_cast<uint16_t>(radiusVoxels * 16);
        return s;
    }

    voxel::ShapeQ CylinderShape(const glm::ivec3& centreVoxel, int radiusVoxels, int halfHeightVoxels) {
        voxel::ShapeQ s;
        s.kind = voxel::kShapeCylinder;
        s.a = centreVoxel * 16;
        s.radiusQ = static_cast<uint16_t>(radiusVoxels * 16);
        s.halfHeightQ = static_cast<uint16_t>(halfHeightVoxels * 16);
        return s;
    }

    voxel::VoxelMask PatternMask(const glm::ivec3& size, int every) {
        voxel::VoxelMask mask(size);
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x)
                    if ((x * 7 + y * 3 + z * 5) % every != 0) mask.Set(x, y, z, true);
        return mask;
    }

    /// The spell ops of the clip and determinism tests: each spans 2 x 2 x 2 chunks around the chunk
    /// corner (0, 96, 0) (chunks -1..0, 2..3, -1..0), above the ground at y = 80 and through it.
    std::vector<sim::WorldOp> SpellOps() {
        return {
            sim::WorldOp::Map(voxel::Map_Knit, BoxShape({-8, 88, -8}, {8, 104, 8})),
            sim::WorldOp::MapMasked(voxel::Map_Unknit, {-6, 90, -6}, PatternMask({12, 12, 12}, 3)),
            sim::WorldOp::Map(voxel::Map_Knit, CapsuleShape({-12, 70, 4}, {12, 100, -4}, 3)),
            sim::WorldOp::Map(voxel::Map_Ablate, SphereShape({0, 96, 0}, 6)),
            sim::WorldOp::Map(voxel::Map_Unknit, CylinderShape({2, 96, -2}, 5, 7)),
            sim::WorldOp::MapMasked(voxel::Map_Knit, {-10, 92, -10}, PatternMask({20, 8, 20}, 4)),
        };
    }

    uint8_t VoxelAt(voxel::World& world, const glm::ivec3& v) { return world.GetVoxel(v); }
}

TEST(MapMask, DecidesFromTheOldIdOnly) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    voxel::World& world = yard.World();
    const uint8_t stone = yard.Ids().stone, knit = yard.Ids().knitIron;
    // Three blocks above the ground in a row along x: stone, knit, air.
    sim::ApplyOp(world, sim::WorldOp::Box({0, 90, 0}, {8, 98, 8}, stone));
    sim::ApplyOp(world, sim::WorldOp::Map(voxel::Map_Knit, BoxShape({8, 90, 0}, {16, 98, 8})));
    const glm::ivec3 origin(0, 90, 0);
    const voxel::VoxelMask mask = PatternMask({24, 8, 8}, 3);

    std::vector<voxel::VoxelEdit> edits;
    const sim::OpResult r = sim::ApplyOp(world, sim::WorldOp::MapMasked(voxel::Map_Unknit, origin, mask), &edits);
    EXPECT_TRUE(r.Complete());
    uint32_t maskedKnit = 0;
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 24; ++x) {
                const glm::ivec3 v = origin + glm::ivec3(x, y, z);
                const bool masked = mask.Get(x, y, z);
                const uint8_t before = x < 8 ? stone : (x < 16 ? knit : uint8_t{0});
                const uint8_t expected = masked && before == knit ? uint8_t{0} : before;
                maskedKnit += masked && before == knit ? 1u : 0u;
                EXPECT_EQ(VoxelAt(world, v), expected) << x << " " << y << " " << z;
            }
    ASSERT_GT(maskedKnit, 0u);
    EXPECT_EQ(r.changed, maskedKnit);
    ASSERT_EQ(edits.size(), maskedKnit);
    for (const voxel::VoxelEdit& e : edits) {
        EXPECT_EQ(e.oldId, knit);
        EXPECT_EQ(e.newId, 0);
        EXPECT_TRUE(mask.Get(e.voxel - origin));
    }

    // Knit through the same mask fills only the masked air: stone and the knit left stay as they are.
    edits.clear();
    const sim::OpResult k = sim::ApplyOp(world, sim::WorldOp::MapMasked(voxel::Map_Knit, origin, mask), &edits);
    EXPECT_EQ(k.changed, static_cast<uint32_t>(edits.size()));
    for (const voxel::VoxelEdit& e : edits) {
        EXPECT_EQ(e.oldId, 0);
        EXPECT_EQ(e.newId, knit);
    }
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) EXPECT_EQ(VoxelAt(world, origin + glm::ivec3(x, y, z)), stone);
}

TEST(Magic, MapZeroAndUnknownMapsChangeNothing) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint64_t before = yard.Hash();
    for (const uint8_t map : {uint8_t{0}, uint8_t{16}, uint8_t{255}}) {
        std::vector<voxel::VoxelEdit> edits;
        const sim::OpResult a = sim::ApplyOp(yard.World(), sim::WorldOp::Map(map, BoxShape({-8, 70, -8}, {8, 90, 8})), &edits);
        const sim::OpResult b = sim::ApplyOp(yard.World(), sim::WorldOp::MapMasked(map, {-8, 70, -8}, PatternMask({16, 20, 16}, 2)), &edits);
        EXPECT_EQ(a.changed + b.changed, 0u) << static_cast<int>(map);
        EXPECT_TRUE(edits.empty());
        EXPECT_TRUE(a.report.changed.empty() && a.report.skipped.empty() && b.report.changed.empty());
    }
    EXPECT_EQ(yard.Hash(), before);
}

TEST(Magic, ClipEqualsWhole) {
    MagicYard whole, forward, reverse;
    ASSERT_TRUE(whole.initialized && forward.initialized && reverse.initialized);
    ASSERT_EQ(whole.Hash(), forward.Hash());
    ASSERT_EQ(whole.Hash(), reverse.Hash());
    for (const sim::WorldOp& op : SpellOps()) {
        glm::ivec3 lo, hi;
        ASSERT_TRUE(sim::OpChunkRange(op, lo, hi));
        EXPECT_EQ(lo, glm::ivec3(-1, 2, -1)) << static_cast<int>(op.kind);
        EXPECT_EQ(hi, glm::ivec3(0, 3, 0)) << static_cast<int>(op.kind);
        std::vector<glm::ivec3> chunks;
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) chunks.emplace_back(x, y, z);

        std::vector<voxel::VoxelEdit> a, b, c;
        const sim::OpResult ra = sim::ApplyOp(whole.World(), op, &a);
        EXPECT_TRUE(ra.Complete());
        EXPECT_GT(ra.changed, 0u) << static_cast<int>(op.kind);
        uint32_t forwardChanged = 0, reverseChanged = 0;
        std::vector<glm::ivec3> changedChunks;
        for (const glm::ivec3& chunk : chunks) {
            const voxel::ChunkClip clip = voxel::ChunkClip::One(chunk);
            const size_t before = b.size();
            const sim::OpResult r = sim::ApplyOp(forward.World(), op, &b, &clip);
            forwardChanged += r.changed;
            changedChunks.insert(changedChunks.end(), r.report.changed.begin(), r.report.changed.end());
            if (b.size() > before) EXPECT_TRUE(sim::OpTouchesChunk(op, chunk));
            if (!sim::OpTouchesChunk(op, chunk)) EXPECT_EQ(b.size(), before);
        }
        for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
            const voxel::ChunkClip clip = voxel::ChunkClip::One(*it);
            reverseChanged += sim::ApplyOp(reverse.World(), op, &c, &clip).changed;
        }
        EXPECT_EQ(forwardChanged, ra.changed);
        EXPECT_EQ(reverseChanged, ra.changed);
        EXPECT_EQ(changedChunks, ra.report.changed);
        ASSERT_EQ(a.size(), b.size());
        for (size_t i = 0; i < a.size(); ++i) {
            EXPECT_EQ(a[i].voxel, b[i].voxel);
            EXPECT_EQ(a[i].oldId, b[i].oldId);
            EXPECT_EQ(a[i].newId, b[i].newId);
        }
        const uint64_t h = whole.Hash();
        EXPECT_EQ(forward.Hash(), h) << static_cast<int>(op.kind);
        EXPECT_EQ(reverse.Hash(), h) << static_cast<int>(op.kind);
    }
}

TEST(Magic, TwoSimulationsFedSpellOpsHaveEqualHashes) {
    // Different worker counts: load and collider timing differ, the voxels must not.
    MagicYard server(3), client(1);
    ASSERT_TRUE(server.initialized && client.initialized);
    ASSERT_EQ(server.Hash(), client.Hash());
    const uint8_t knit = server.Ids().knitIron;
    std::vector<sim::WorldOp> stream = {
        sim::WorldOp::Map(voxel::Map_Knit, BoxShape({-20, 80, -6}, {20, 96, 6})),                   // a wall on the ground
        sim::WorldOp::MapMasked(voxel::Map_Unknit, {-10, 84, -6}, PatternMask({16, 8, 12}, 2)),      // part of it expires
        sim::WorldOp::Map(voxel::Map_Knit, CapsuleShape({-24, 70, 10}, {24, 100, -10}, 2)),         // through stone, knit and air
        sim::WorldOp::Map(voxel::Map_Ablate, SphereShape({4, 90, 0}, 5)),                            // an absorbed blast
        sim::WorldOp::Clear({-16, 76, -4}, PatternMask({12, 10, 8}, 3)),                             // crumbs: stone and knit
        sim::WorldOp::Carve(voxel::SphereQ{glm::ivec3(-8, 84, 2) * 16 + glm::ivec3(5), 6 * 16}, voxel::QuantizePower(8.0f)),
    };
    uint32_t knitMade = 0;
    for (const sim::WorldOp& op : stream) {
        std::vector<voxel::VoxelEdit> se, ce;
        const sim::OpResult s = sim::ApplyOp(server.World(), op, &se);
        const sim::OpResult c = sim::ApplyOp(client.World(), op, &ce);
        EXPECT_TRUE(s.Complete());
        EXPECT_TRUE(c.Complete());
        EXPECT_GT(s.changed, 0u) << static_cast<int>(op.kind);
        EXPECT_EQ(s.changed, c.changed);
        for (const voxel::VoxelEdit& e : se) knitMade += e.newId == knit ? 1u : 0u;
    }
    EXPECT_GT(knitMade, 0u);
    EXPECT_EQ(server.Hash(), client.Hash());

    // One op 1/16 voxel different gives a different world.
    MagicYard other(2);
    ASSERT_TRUE(other.initialized);
    stream[2].shape.radiusQ = static_cast<uint16_t>(stream[2].shape.radiusQ + 3);
    for (const sim::WorldOp& op : stream) sim::ApplyOp(other.World(), op);
    EXPECT_NE(other.Hash(), server.Hash());
}
