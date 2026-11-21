// Edge cases of the replicated-operation contract (WorldOp.cppm): empty / inverted / out-of-mask
// operations. The wire carries exactly these WorldOps (a Detach's mask is a tight island bounds,
// but any peer or file can hand us an empty, inverted or degenerate one), and the contract says
// they must change nothing, report complete and never touch a world hash or a chunk outside the
// mask - while the tight bounds of a real island are named exactly by Bounds / OpTouchesChunk.

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

    /// The operation's box is empty on some axis (Bounds may keep a non-zero extent on another:
    /// an inverted FillBox clamps max up to min per axis), so it has no chunk range.
    void ExpectEmptyBounds(const sim::WorldOp& op) {
        glm::ivec3 lo{1}, hi{0};
        op.Bounds(lo, hi);
        EXPECT_TRUE(hi.x <= lo.x || hi.y <= lo.y || hi.z <= lo.z) << static_cast<int>(op.kind);
        EXPECT_TRUE(lo == glm::ivec3(0) || lo == op.min) << static_cast<int>(op.kind);
        glm::ivec3 clo, chi;
        EXPECT_FALSE(sim::OpChunkRange(op, clo, chi)) << static_cast<int>(op.kind);
    }

    /// OpTouchesChunk is false for every chunk of the operation's chunk range (exact, unlike Bounds).
    void ExpectTouchesNoChunk(const sim::WorldOp& op) {
        glm::ivec3 clo, chi;
        if (!sim::OpChunkRange(op, clo, chi)) return;
        for (int z = clo.z; z <= chi.z; ++z)
            for (int y = clo.y; y <= chi.y; ++y)
                for (int x = clo.x; x <= chi.x; ++x) EXPECT_FALSE(sim::OpTouchesChunk(op, glm::ivec3(x, y, z))) << static_cast<int>(op.kind);
    }
}

TEST(WorldOps, EmptyOperationsChangeNothingAndReportComplete) {
    Flat f;
    const uint64_t before = f.Hash();

    const sim::WorldOp ops[] = {
        sim::WorldOp::Carve(voxel::SphereQ{glm::ivec3(0), 0}, 400),  // radius 0, centre not at a voxel centre: contains no voxel
        sim::WorldOp::Fill(voxel::SphereQ{glm::ivec3(0), 0}, f.ids.gold),
        sim::WorldOp::Swap(voxel::SphereQ{glm::ivec3(0), 0}, f.ids.grass, f.ids.dirt),
        sim::WorldOp::Box(glm::ivec3(5), glm::ivec3(5), f.ids.stone),    // empty box
        sim::WorldOp::Box(glm::ivec3(5), glm::ivec3(2), f.ids.stone),    // inverted box
        sim::WorldOp::Detach(glm::ivec3(0), voxel::VoxelMask(glm::ivec3(0)), glm::vec3(0.0f), 1),  // empty mask
        sim::WorldOp::Clear(glm::ivec3(3, 3, 3), voxel::VoxelMask(glm::ivec3(4, 4, 4))),           // all bits clear
    };
    for (const sim::WorldOp& op : ops) {
        std::vector<voxel::VoxelEdit> edits;
        const sim::OpResult r = sim::ApplyOp(*f.world, op, &edits);
        EXPECT_EQ(r.changed, 0u) << static_cast<int>(op.kind);
        EXPECT_TRUE(r.Complete()) << static_cast<int>(op.kind);
        EXPECT_TRUE(edits.empty()) << static_cast<int>(op.kind);
        EXPECT_EQ(f.Hash(), before) << static_cast<int>(op.kind);
        ExpectTouchesNoChunk(op);
        // A mask op's Bounds is its mask box whatever bits are set (WorldOp::Bounds): the all-clear
        // ClearMask has a non-empty box and touches no chunk of it.
        if (op.kind != sim::WorldOpKind::ClearMask) ExpectEmptyBounds(op);
    }

    // A mask whose every bit is set outside the mask box (writes outside the box are ignored).
    voxel::VoxelMask outside(glm::ivec3(2, 2, 2));
    outside.Set(9, 9, 9, true);
    outside.Set(-1, 0, 0, true);
    EXPECT_EQ(outside.Count(), 0u);
    const sim::OpResult r = sim::ApplyOp(*f.world, sim::WorldOp::Clear(glm::ivec3(4), outside));
    EXPECT_EQ(r.changed, 0u);
    EXPECT_TRUE(r.Complete());
    EXPECT_EQ(f.Hash(), before);

    f.jobs.WaitIdle();
}

TEST(WorldOps, DegenerateOperationsBoundsAndTouchExactly) {
    // A radius-0 sphere contains a voxel only when its centre is exactly the voxel centre
    // (World.cppm: 16 v + 8 == center). Bounds and OpTouchesChunk must name that voxel's chunk and
    // no other.
    const voxel::SphereQ exact{glm::ivec3(328, -40, 24), 0};  // centre = 16*v + 8 for v = (20, -3, 1)
    const sim::WorldOp op = sim::WorldOp::Fill(exact, 7);
    glm::ivec3 lo{1}, hi{0};
    op.Bounds(lo, hi);
    EXPECT_EQ(lo, glm::ivec3(20, -3, 1));
    EXPECT_EQ(hi, lo + glm::ivec3(1));
    glm::ivec3 clo, chi;
    ASSERT_TRUE(sim::OpChunkRange(op, clo, chi));
    EXPECT_EQ(clo, voxel::VoxelToChunk(lo));
    EXPECT_EQ(chi, clo);
    EXPECT_TRUE(sim::OpTouchesChunk(op, clo));
    EXPECT_FALSE(sim::OpTouchesChunk(op, clo + glm::ivec3(1, 0, 0)));
    EXPECT_FALSE(sim::OpTouchesChunk(op, clo - glm::ivec3(0, 1, 0)));

    // An inverted FillBox: Bounds clamps max up to min - empty, touches nothing.
    const sim::WorldOp box = sim::WorldOp::Box(glm::ivec3(40, 40, 40), glm::ivec3(39, 41, 41), 3);
    ExpectEmptyBounds(box);
    EXPECT_FALSE(sim::OpTouchesChunk(box, voxel::VoxelToChunk(glm::ivec3(40, 40, 40))));

    // A mask op whose only bit sits on the last voxel column of a chunk: it touches that chunk,
    // never the next one.
    voxel::VoxelMask mask(glm::ivec3(8, 1, 1));
    mask.Set(7, 0, 0, true);
    const sim::WorldOp clear = sim::WorldOp::Clear(glm::ivec3(24, 0, 0), mask);  // bit at voxel 31: last column of chunk 0
    {
        glm::ivec3 blo, bhi;
        clear.Bounds(blo, bhi);
        EXPECT_EQ(blo, glm::ivec3(24, 0, 0));
        EXPECT_EQ(bhi, glm::ivec3(32, 1, 1));
        EXPECT_TRUE(sim::OpTouchesChunk(clear, glm::ivec3(0, 0, 0)));
        EXPECT_FALSE(sim::OpTouchesChunk(clear, glm::ivec3(1, 0, 0)));  // voxel 32 belongs to chunk 1
        glm::ivec3 rlo, rhi;
        ASSERT_TRUE(sim::OpChunkRange(clear, rlo, rhi));
        EXPECT_EQ(rlo, glm::ivec3(0, 0, 0));
        EXPECT_EQ(rhi, glm::ivec3(0, 0, 0));
    }
}

TEST(WorldOps, BuildDetachedPieceFromMissingEditsIsAir) {
    // A Detach whose chunk was skipped (no base yet): the removed-voxel edits never arrived. The
    // piece must still have the mask's size, with air where nothing was removed - no voxel exists
    // twice (WorldOp.cppm).
    voxel::VoxelMask mask(glm::ivec3(3, 2, 1));
    mask.Set(0, 0, 0, true);
    mask.Set(2, 1, 0, true);
    const sim::WorldOp op = sim::WorldOp::Detach(glm::ivec3(8, 8, 8), mask, glm::vec3(1.0f, 0.0f, 0.0f), 4);

    const voxel::VoxelVolume empty = sim::BuildDetachedPiece(op, {});
    EXPECT_EQ(empty.Size(), glm::ivec3(3, 2, 1));
    EXPECT_TRUE(empty.IsEmpty());

    std::vector<voxel::VoxelEdit> edits;
    edits.push_back(voxel::VoxelEdit{glm::ivec3(8, 8, 8), 5, 0});  // removed, old id 5
    const voxel::VoxelVolume part = sim::BuildDetachedPiece(op, edits);
    EXPECT_EQ(part.Size(), glm::ivec3(3, 2, 1));
    EXPECT_EQ(part.Get(0, 0, 0), 5u);
    EXPECT_EQ(part.Get(2, 1, 0), 0u);  // not removed: air, the voxels never exist twice
    EXPECT_EQ(part.CountSolid(), 1u);
}

TEST(WorldOps, MaskHelpersEdgeCases) {
    // Negative mask sizes count as 0 (VoxelMask.cppm).
    voxel::VoxelMask neg(glm::ivec3(-4, 3, -1));
    EXPECT_EQ(neg.Size(), glm::ivec3(0, 3, 0));
    EXPECT_EQ(neg.VoxelCount(), 0u);
    EXPECT_TRUE(neg.Bytes().empty());
    EXPECT_EQ(neg.Count(), 0u);
    EXPECT_FALSE(neg.Get(0, 0, 0));

    // AnyInRange / SetRange clamped past the end, empty ranges, ranges that cross byte borders.
    voxel::VoxelMask m(glm::ivec3(5, 1, 1));  // 5 voxels, 1 byte
    EXPECT_FALSE(m.AnyInRange(0, 0));
    EXPECT_FALSE(m.AnyInRange(9, 99));
    EXPECT_FALSE(m.AnyInRange(99, 3));
    m.SetRange(3, 99);
    EXPECT_TRUE(m.Get(3, 0, 0));
    EXPECT_TRUE(m.Get(4, 0, 0));
    EXPECT_FALSE(m.Get(2, 0, 0));
    EXPECT_EQ(m.Count(), 2u);
    EXPECT_TRUE(m.AnyInRange(0, 5));
    EXPECT_FALSE(m.AnyInRange(0, 3));
    EXPECT_FALSE(m.AnyInRange(2, 3));  // [2, 3) is bit 2 only
    EXPECT_TRUE(m.AnyInRange(2, 4));   // [2, 4) crosses into bit 3

    // FromVolume maps solid voxels to bits in Index order.
    voxel::VoxelVolume v(glm::ivec3(2, 2, 2));
    v.FillBox(glm::ivec3(1, 0, 0), glm::ivec3(2, 2, 2), 9);
    const voxel::VoxelMask bits = voxel::VoxelMask::FromVolume(v);
    EXPECT_EQ(bits.Size(), v.Size());
    EXPECT_EQ(bits.Count(), 4u);
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            EXPECT_EQ(bits.Get(0, y, z), false);
    EXPECT_TRUE(bits.Get(1, 1, 1));

    // Tight bounds of a sparse volume, and ClearMask over exactly those bounds.
    glm::ivec3 slo, shi;
    ASSERT_TRUE(v.SolidBounds(slo, shi));
    EXPECT_EQ(slo, glm::ivec3(1, 0, 0));
    EXPECT_EQ(shi, glm::ivec3(2, 2, 2));
    Flat f;
    f.world->FillBox(glm::ivec3(1, 1, 1), glm::ivec3(3, 3, 3), f.ids.stone);  // chunk 0, voxels 1..2
    const uint64_t before = f.Hash();
    // The mask box is (1,1,1)..(3,3,3); its set bits (1,y,z) are voxels (2, 1+y, 1+z), all four
    // inside the filled box: stone -> air. Clear bits (0,y,z) leave voxels (1, 1+y, 1+z) stone.
    std::vector<voxel::VoxelEdit> edits;
    const uint32_t changed = f.world->ClearMask(glm::ivec3(1, 1, 1), bits, &edits);
    EXPECT_EQ(changed, 4u);
    EXPECT_EQ(edits.size(), 4u);
    EXPECT_NE(f.Hash(), before);
    EXPECT_EQ(f.world->GetVoxel(glm::ivec3(1, 1, 1)), f.ids.stone);
    EXPECT_EQ(f.world->GetVoxel(glm::ivec3(1, 2, 2)), f.ids.stone);
    EXPECT_EQ(f.world->GetVoxel(glm::ivec3(2, 1, 1)), 0u);
    EXPECT_EQ(f.world->GetVoxel(glm::ivec3(2, 2, 2)), 0u);

    f.jobs.WaitIdle();
}
