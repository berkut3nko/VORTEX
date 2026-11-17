// networking.md stage 7a: a client applies an operation to the chunks that have a base below it
// (sim::ApplyOp with a voxel::ChunkClip). Clipped chunk by chunk it must equal the whole operation,
// for every WorldOpKind, the Detach piece included; OpTouchesChunk names the chunks it can change.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;

using namespace vortex;

namespace {
    struct Flat {
        Flat() : store(1024), jobs(2) {
            voxel::WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = {6, 4, 6};
            config.fullHeight = true;
            config.heightRange = {-16.0f, 48.0f};
            ids = voxel::RegisterDefaultWorldMaterials(materials);
            world = std::make_unique<voxel::World>(config, store, jobs, voxel::CreateFlatGenerator(40, ids.grass, ids.stone), &materials);
            world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
        }
        uint64_t Hash() const { return voxel::WorldHash(*world, world->WindowMinChunk(), world->WindowMinChunk() + world->WindowSize()).hash; }
        voxel::ChunkStore store;
        JobSystem jobs;
        voxel::MaterialLibrary materials;
        voxel::WorldMaterials ids;
        std::unique_ptr<voxel::World> world;
    };
}

TEST(WorldOps, ApplyOpClippedToEachChunkEqualsTheWholeOp) {
    Flat whole, clipped;
    ASSERT_EQ(whole.Hash(), clipped.Hash());
    voxel::VoxelMask mask(glm::ivec3(40, 12, 36));
    for (int z = 0; z < 36; ++z)
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 40; ++x)
                if ((x * 7 + y * 3 + z * 5) % 4 != 0) mask.Set(x, y, z, true);
    voxel::VoxelMask crumbs(glm::ivec3(50, 6, 50));
    for (int i = 0; i < 50; i += 7) crumbs.Set(i, 2, 49 - i, true);
    const std::vector<sim::WorldOp> ops = {
        sim::WorldOp::Carve(voxel::SphereQ{glm::ivec3(31, 38, 1) * 16, 10 * 16}, 400),
        sim::WorldOp::Fill(voxel::SphereQ{glm::ivec3(-31, 44, 30) * 16, 6 * 16}, whole.ids.gold),
        sim::WorldOp::Swap(voxel::SphereQ{glm::ivec3(2, 39, -2) * 16, 13 * 16}, whole.ids.grass, whole.ids.dirt),
        sim::WorldOp::Box(glm::ivec3(-40, 30, -8), glm::ivec3(10, 45, 40), whole.ids.stone),
        sim::WorldOp::Detach(glm::ivec3(-20, 30, -18), mask, glm::vec3(1.0f), 7),
        sim::WorldOp::Clear(glm::ivec3(5, 36, -25), crumbs),
    };
    for (const sim::WorldOp& op : ops) {
        std::vector<voxel::VoxelEdit> a, b;
        const sim::OpResult ra = sim::ApplyOp(*whole.world, op, &a);
        glm::ivec3 lo, hi;
        ASSERT_TRUE(sim::OpChunkRange(op, lo, hi));
        std::vector<glm::ivec3> changed;
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const voxel::ChunkClip clip = voxel::ChunkClip::One({x, y, z});
                    const size_t before = b.size();
                    const sim::OpResult r = sim::ApplyOp(*clipped.world, op, &b, &clip);
                    changed.insert(changed.end(), r.report.changed.begin(), r.report.changed.end());
                    // A chunk the op changed is one it touches; one it does not touch it never changes.
                    if (b.size() > before) EXPECT_TRUE(sim::OpTouchesChunk(op, {x, y, z}));
                    if (!sim::OpTouchesChunk(op, {x, y, z})) EXPECT_EQ(b.size(), before);
                }
        EXPECT_GT(ra.changed, 0u) << static_cast<int>(op.kind);
        EXPECT_EQ(ra.report.changed, changed);
        ASSERT_EQ(a.size(), b.size());
        for (size_t i = 0; i < a.size(); ++i) {
            EXPECT_EQ(a[i].voxel, b[i].voxel);
            EXPECT_EQ(a[i].oldId, b[i].oldId);
            EXPECT_EQ(a[i].newId, b[i].newId);
        }
        EXPECT_EQ(whole.Hash(), clipped.Hash());
        if (op.kind == sim::WorldOpKind::Detach) {
            const voxel::VoxelVolume pa = sim::BuildDetachedPiece(op, a), pb = sim::BuildDetachedPiece(op, b);
            ASSERT_EQ(pa.Size(), pb.Size());
            size_t differ = 0, solid = 0;
            for (int z = 0; z < pa.Size().z; ++z)
                for (int y = 0; y < pa.Size().y; ++y)
                    for (int x = 0; x < pa.Size().x; ++x) {
                        differ += pa.Get(x, y, z) != pb.Get(x, y, z) ? 1u : 0u;
                        solid += pa.Get(x, y, z) != 0 ? 1u : 0u;
                    }
            EXPECT_EQ(differ, 0u);
            EXPECT_GT(solid, 0u);
        }
    }
    whole.jobs.WaitIdle();
    clipped.jobs.WaitIdle();
}
