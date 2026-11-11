// Quantised world edits (docs/design/networking.md, "World operations"): integer spheres in
// 1/16 voxel, the int64 sphere test, swap, ClearMask and the report of chunks an edit could not
// apply to. Everything here must give the same voxels on every machine.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    constexpr int kGround = 40;          // flat world: voxels with y < 40 are solid
    constexpr uint8_t kSurface = 2;      // hardness 1 (default material)
    constexpr uint8_t kRock = 3;         // hardness 10
    constexpr uint8_t kSoft = 4;         // hardness 0.6: 9.6 power units, not a whole number
    constexpr uint8_t kSteel = 5;        // indestructible

    struct Bench {
        explicit Bench(uint32_t capacity = 512, glm::ivec3 window = {8, 8, 8}) : store(capacity), jobs(3) {
            WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = window;
            Material rock;
            rock.hardness = 10.0f;
            Material soft;
            soft.hardness = 0.6f;
            Material steel;
            steel.hardness = 0.0f;
            steel.flags = MaterialFlag_Indestructible;
            materials.Set(kWorldPalette, kSurface, Material{}, "surface");
            materials.Set(kWorldPalette, kRock, rock, "rock");
            materials.Set(kWorldPalette, kSoft, soft, "soft");
            materials.Set(kWorldPalette, kSteel, steel, "steel");
            world = std::make_unique<World>(config, store, jobs, CreateFlatGenerator(kGround, kSurface, kRock), &materials);
        }
        void Load() { world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0)); }   // chunks (-4..3, -3..4, -4..3)

        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        std::unique_ptr<World> world;
    };

    using VoxelKey = std::tuple<int, int, int>;
    VoxelKey Key(const glm::ivec3& v) { return {v.x, v.y, v.z}; }

    /// Independent reference of the sphere test: voxel centre and sphere in voxels, in double. All
    /// values are multiples of 1/16 well inside 2^20, so every product and sum is exact.
    bool ReferenceInside(const SphereQ& s, const glm::ivec3& v) {
        const glm::dvec3 c = glm::dvec3(s.center) / 16.0;
        const glm::dvec3 d = glm::dvec3(v) + glm::dvec3(0.5) - c;
        const double r = s.radius / 16.0;
        return d.x * d.x + d.y * d.y + d.z * d.z <= r * r;
    }

    std::set<VoxelKey> ReferenceSphere(const SphereQ& s) {
        std::set<VoxelKey> out;
        const glm::ivec3 c = math::FloorDiv(s.center, 16);
        const int reach = s.radius / 16 + 2;
        for (int z = c.z - reach; z <= c.z + reach; ++z)
            for (int y = c.y - reach; y <= c.y + reach; ++y)
                for (int x = c.x - reach; x <= c.x + reach; ++x)
                    if (ReferenceInside(s, {x, y, z})) out.insert({x, y, z});
        return out;
    }

    std::set<VoxelKey> Keys(const std::vector<VoxelEdit>& edits) {
        std::set<VoxelKey> out;
        for (const VoxelEdit& e : edits) out.insert(Key(e.voxel));
        return out;
    }

    std::set<VoxelKey> ChunkKeys(const std::vector<glm::ivec3>& chunks) {
        std::set<VoxelKey> out;
        for (const glm::ivec3& c : chunks) out.insert(Key(c));
        return out;
    }
}

// --- quantisation ----------------------------------------------------------------------------

TEST(QuantizedEdits, QuantizeRoundsHalvesAwayFromZeroAndClamps) {
    // 1.03125 voxels = 16.5 q: halves round away from zero, on both sides of the origin.
    SphereQ s = QuantizeSphere(glm::dvec3(1.03125, -1.03125, 0.0), 2.0);
    EXPECT_EQ(s.center, glm::ivec3(17, -17, 0));
    EXPECT_EQ(s.radius, 32);
    // Just below / above a half: nearest.
    s = QuantizeSphere(glm::dvec3(-0.03, 0.03, -100.5), 0.5);
    EXPECT_EQ(s.center, glm::ivec3(0, 0, -1608));
    EXPECT_EQ(s.radius, 8);
    // Radius: negative -> 0, huge -> 65535.
    EXPECT_EQ(QuantizeSphere(glm::dvec3(0.0), -3.0).radius, 0);
    EXPECT_EQ(QuantizeSphere(glm::dvec3(0.0), 1.0e9).radius, 65535);
    // Centre clamped to the int32 range of q; non-finite input gives an empty sphere.
    s = QuantizeSphere(glm::dvec3(1.0e12, -1.0e12, 0.0), 1.0);
    EXPECT_EQ(s.center.x, std::numeric_limits<int32_t>::max());
    EXPECT_EQ(s.center.y, -std::numeric_limits<int32_t>::max());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    s = QuantizeSphere(glm::dvec3(nan, 1.0, 1.0), 4.0);
    EXPECT_EQ(s.radius, 0);
    EXPECT_EQ(s.center.x, 0);
    EXPECT_EQ(QuantizeSphere(glm::dvec3(1.0), std::numeric_limits<double>::infinity()).radius, 0);

    EXPECT_EQ(QuantizePower(6.0f), 96);
    EXPECT_EQ(QuantizePower(0.6f), 10);                 // 9.6 -> 10
    EXPECT_EQ(QuantizePower(-1.0f), 0);
    EXPECT_EQ(QuantizePower(std::numeric_limits<float>::quiet_NaN()), 0);
    EXPECT_EQ(QuantizePower(1.0e9f), 65535);
}

TEST(QuantizedEdits, SphereTestIsExactOnTheBoundary) {
    // Centre on the centre of voxel 0 (8 q): a voxel exactly one radius away is inside.
    const SphereQ s{glm::ivec3(8), 16};
    EXPECT_TRUE(InSphereQ(s, {0, 0, 0}));
    EXPECT_TRUE(InSphereQ(s, {1, 0, 0}));
    EXPECT_TRUE(InSphereQ(s, {-1, 0, 0}));
    EXPECT_TRUE(InSphereQ(s, {0, 0, -1}));
    EXPECT_FALSE(InSphereQ(s, {1, 1, 0}));              // 16 * sqrt(2) > 16
    EXPECT_FALSE(InSphereQ(SphereQ{glm::ivec3(8), 15}, {1, 0, 0}));
    // Radius 0 holds exactly the voxel whose centre is the sphere centre, and nothing else.
    EXPECT_TRUE(InSphereQ(SphereQ{glm::ivec3(-24, 8, 40), 0}, {-2, 0, 2}));
    EXPECT_FALSE(InSphereQ(SphereQ{glm::ivec3(-23, 8, 40), 0}, {-2, 0, 2}));
    // 3-4-5 triangle across negative coordinates: 48^2 + 64^2 = 80^2.
    const SphereQ t{glm::ivec3(-8, -8, 0), 80};          // centre of voxel -1 on x and y, z on a face
    EXPECT_FALSE(InSphereQ(t, {-1 + 3, -1 + 4, 0}));     // z adds 8^2
    EXPECT_TRUE(InSphereQ(SphereQ{glm::ivec3(-8, -8, 8), 80}, {2, 3, 0}));
    EXPECT_FALSE(InSphereQ(SphereQ{glm::ivec3(-8, -8, 8), 79}, {2, 3, 0}));
    // No overflow at the ends of the int range.
    const int big = std::numeric_limits<int>::max();
    EXPECT_FALSE(InSphereQ(SphereQ{glm::ivec3(0), 65535}, {big, big, big}));
    EXPECT_FALSE(InSphereQ(SphereQ{glm::ivec3(0), 65535}, {-big - 1, 0, 0}));
}

TEST(QuantizedEdits, BoundsHoldTheWholeSphere) {
    Rng rng(5);
    for (int i = 0; i < 300; ++i) {
        SphereQ s;
        s.center = glm::ivec3(static_cast<int>(rng.Range(-2000.0f, 2000.0f)), static_cast<int>(rng.Range(-2000.0f, 2000.0f)),
                              static_cast<int>(rng.Range(-2000.0f, 2000.0f)));
        s.radius = static_cast<uint16_t>(rng.Range(0.0f, 120.0f));
        glm::ivec3 mn, mx;
        SphereVoxelBounds(s, mn, mx);
        const std::set<VoxelKey> reference = ReferenceSphere(s);
        if (reference.empty()) continue;      // a tiny sphere between voxel centres
        // Every voxel inside is in the box; the box is the per-axis bound |d| <= r, so it is at
        // most one voxel larger than the tight bounds on each side.
        glm::ivec3 lo(std::numeric_limits<int>::max()), hi(std::numeric_limits<int>::min());
        for (const auto& [x, y, z] : reference) {
            lo = glm::min(lo, glm::ivec3(x, y, z));
            hi = glm::max(hi, glm::ivec3(x, y, z) + glm::ivec3(1));
        }
        for (int a = 0; a < 3; ++a) {
            EXPECT_LE(mn[a], lo[a]) << "sphere " << i;
            EXPECT_GE(mx[a], hi[a]) << "sphere " << i;
            EXPECT_GE(mn[a], lo[a] - 1) << "sphere " << i;
            EXPECT_LE(mx[a], hi[a] + 1) << "sphere " << i;
        }
        // And per axis it is exact: a voxel on the box face along the axis through the centre
        // column is inside exactly when |d| <= r on that axis alone.
        for (int a = 0; a < 3; ++a) {
            const int64_t dLo = 16LL * mn[a] + 8 - s.center[a];
            const int64_t dOut = 16LL * (mn[a] - 1) + 8 - s.center[a];
            EXPECT_LE(-dLo, static_cast<int64_t>(s.radius));
            EXPECT_GT(-dOut, static_cast<int64_t>(s.radius));
        }
    }
}

// --- world edits -----------------------------------------------------------------------------

TEST(QuantizedEdits, FillSphereMatchesTheReferenceAcrossChunkBordersAndNegativeCoordinates) {
    Bench b;
    b.Load();
    Rng rng(17);
    for (int i = 0; i < 40; ++i) {
        // Centres near the chunk corner (0, 64, 0) and (-32, 64, -32), off-grid in q.
        SphereQ s;
        const glm::ivec3 corner = (i & 1) ? glm::ivec3(-32, 64, -32) : glm::ivec3(0, 64, 0);
        s.center = corner * 16 + glm::ivec3(static_cast<int>(rng.Range(-60.0f, 60.0f)), static_cast<int>(rng.Range(-60.0f, 60.0f)),
                                            static_cast<int>(rng.Range(-60.0f, 60.0f)));
        s.radius = static_cast<uint16_t>(rng.Range(0.0f, 140.0f));
        const uint8_t id = (i & 2) ? kRock : kSurface;
        std::vector<VoxelEdit> edits;
        EditReport report;
        b.world->FillSphereQ(s, 0, nullptr);                  // start from air
        const uint32_t changed = b.world->FillSphereQ(s, id, &edits, &report);
        EXPECT_TRUE(report.Complete());
        const std::set<VoxelKey> reference = ReferenceSphere(s);
        EXPECT_EQ(Keys(edits), reference) << "sphere " << i;
        EXPECT_EQ(changed, reference.size());
        for (const auto& [x, y, z] : reference) EXPECT_EQ(b.world->GetVoxel({x, y, z}), id);
        b.world->FillSphereQ(s, 0, nullptr);
    }
}

TEST(QuantizedEdits, FloatEditsQuantiseAndForward) {
    Bench a, b;
    a.Load();
    b.Load();
    std::vector<VoxelEdit> fromFloat, fromQ;
    a.world->CarveSphere(glm::vec3(-3.3f, kGround - 1.7f, 31.9f), 5.3f, 2.0f, &fromFloat);
    b.world->CarveSphereQ(QuantizeSphere(glm::dvec3(glm::vec3(-3.3f, kGround - 1.7f, 31.9f)), static_cast<double>(5.3f)), QuantizePower(2.0f), &fromQ);
    ASSERT_FALSE(fromFloat.empty());
    ASSERT_EQ(fromFloat.size(), fromQ.size());
    for (size_t i = 0; i < fromQ.size(); ++i) {
        EXPECT_EQ(fromFloat[i].voxel, fromQ[i].voxel);
        EXPECT_EQ(fromFloat[i].oldId, fromQ[i].oldId);
    }
    // A whole-voxel float sphere is the same set as the old float test gave.
    std::vector<VoxelEdit> filled;
    a.world->FillSphere(glm::vec3(10.5f, kGround + 10.5f, -10.5f), 4.0f, kRock, &filled);
    EXPECT_EQ(Keys(filled), ReferenceSphere(SphereQ{glm::ivec3(168, (kGround + 10) * 16 + 8, -168), 64}));
}

TEST(QuantizedEdits, CarveComparesHardnessWithoutRounding) {
    Bench b;
    b.Load();
    const glm::ivec3 at(5, kGround + 5, 5);
    ASSERT_EQ(b.world->FillBox(at, at + glm::ivec3(1, 4, 1), kSoft), 4u);
    ASSERT_TRUE(b.world->SetVoxel(at + glm::ivec3(0, 3, 0), kSteel));
    ASSERT_TRUE(b.world->SetVoxel(at + glm::ivec3(0, 2, 0), kRock));
    const SphereQ sphere{at * 16 + glm::ivec3(8, 8, 8), 16};    // voxels at, at+y (centre one apart)

    // Soft: hardness 0.6 = 9.6 units. Power 9 does not remove it, power 10 does.
    EXPECT_EQ(b.world->CarveSphereQ(sphere, 9), 0u);
    EXPECT_EQ(b.world->GetVoxel(at), kSoft);
    EXPECT_EQ(b.world->CarveSphereQ(sphere, 10), 2u);
    EXPECT_EQ(b.world->GetVoxel(at), 0);
    // Rock: hardness 10 = 160 units; equal power removes (hardness > power survives).
    const SphereQ upper{(at + glm::ivec3(0, 2, 0)) * 16 + glm::ivec3(8), 16};
    EXPECT_EQ(b.world->CarveSphereQ(upper, 159), 0u);
    EXPECT_EQ(b.world->CarveSphereQ(upper, 160), 1u);
    EXPECT_EQ(b.world->GetVoxel(at + glm::ivec3(0, 2, 0)), 0);
    // Indestructible survives any power.
    EXPECT_EQ(b.world->CarveSphereQ(upper, 65535), 0u);
    EXPECT_EQ(b.world->GetVoxel(at + glm::ivec3(0, 3, 0)), kSteel);
}

TEST(QuantizedEdits, SwapReplacesOnlyTheGivenMaterial) {
    Bench b;
    b.Load();
    // A sphere through the surface layer and the rock below it, around a chunk border (x = 0).
    const SphereQ s{glm::ivec3(0, (kGround - 1) * 16, 8), 64};
    std::vector<VoxelEdit> edits;
    EditReport report;
    const uint32_t swapped = b.world->SwapSphereQ(s, kSurface, kSoft, &edits, &report);
    EXPECT_TRUE(report.Complete());
    ASSERT_GT(swapped, 0u);
    for (const VoxelEdit& e : edits) {
        EXPECT_EQ(e.oldId, kSurface);
        EXPECT_EQ(e.newId, kSoft);
        EXPECT_TRUE(InSphereQ(s, e.voxel));
    }
    // Every surface voxel of the sphere was swapped; rock and air were not touched.
    for (const auto& [x, y, z] : ReferenceSphere(s)) {
        const uint8_t id = b.world->GetVoxel({x, y, z});
        if (y == kGround - 1) EXPECT_EQ(id, kSoft);
        else if (y < kGround - 1) EXPECT_EQ(id, kRock);
        else EXPECT_EQ(id, 0);
    }
    EXPECT_EQ(b.world->SwapSphereQ(s, kSoft, kSoft), 0u);       // from == to
    // from = air fills only the air part of the sphere.
    const uint32_t filled = b.world->SwapSphereQ(s, 0, kRock);
    for (const auto& [x, y, z] : ReferenceSphere(s)) {
        if (y >= kGround) EXPECT_EQ(b.world->GetVoxel({x, y, z}), kRock);
    }
    EXPECT_GT(filled, 0u);
    EXPECT_EQ(b.world->GetVoxel({0, kGround - 1, 0}), kSoft);
}

TEST(QuantizedEdits, ClearMaskRemovesExactlyTheMaskedVoxels) {
    Bench b;
    b.Load();
    // A box across the chunk borders x = 0, z = 0 and y = 32 (rock below is a Uniform chunk).
    const glm::ivec3 origin(-3, 30, -2);
    VoxelMask mask({6, 5, 4});
    for (int z = 0; z < 4; ++z)
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 6; ++x) mask.Set(x, y, z, ((x + y + z) % 3) == 0);
    const uint32_t bits = mask.Count();
    ASSERT_GT(bits, 0u);
    ASSERT_EQ(b.world->StateOf(VoxelToChunk(origin)), CellState::Uniform);

    std::vector<VoxelEdit> removed;
    EditReport report;
    const uint32_t cleared = b.world->ClearMask(origin, mask, &removed, &report);
    EXPECT_TRUE(report.Complete());
    EXPECT_EQ(cleared, bits);                                  // all solid rock here
    for (int z = 0; z < 4; ++z)
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 6; ++x)
                EXPECT_EQ(b.world->GetVoxel(origin + glm::ivec3(x, y, z)), mask.Get(x, y, z) ? 0 : kRock);
    for (const VoxelEdit& e : removed) {
        EXPECT_EQ(e.oldId, kRock);
        EXPECT_EQ(e.newId, 0);
    }
    // Again: masked voxels that are air already are no change.
    EXPECT_EQ(b.world->ClearMask(origin, mask), 0u);
    // Empty or out-of-range masks do nothing.
    EXPECT_EQ(b.world->ClearMask(origin, VoxelMask{}), 0u);
    EXPECT_EQ(b.world->ClearMask(glm::ivec3(kMaxEditVoxel - 2, 0, 0), mask), 0u);
}

TEST(QuantizedEdits, MaskBitsArePackedInVolumeOrder) {
    VoxelVolume volume({3, 2, 2});
    volume.Set(0, 0, 0, 7);
    volume.Set(2, 1, 0, 1);      // index 5
    volume.Set(1, 0, 1, 9);      // index 7
    volume.Set(2, 1, 1, 3);      // index 11
    const VoxelMask mask = VoxelMask::FromVolume(volume);
    ASSERT_EQ(mask.Bytes().size(), 2u);
    EXPECT_EQ(mask.Bytes()[0], 0b10100001);
    EXPECT_EQ(mask.Bytes()[1], 0b00001000);
    EXPECT_EQ(mask.Count(), 4u);
    EXPECT_TRUE(mask.Get(1, 0, 1));
    EXPECT_FALSE(mask.Get(1, 1, 1));
    EXPECT_FALSE(mask.Get(-1, 0, 0));
    VoxelMask copy = mask;
    EXPECT_EQ(copy, mask);
    copy.Set(1, 0, 1, false);
    EXPECT_NE(copy, mask);
    EXPECT_EQ(copy.Count(), 3u);
}

// --- what an edit could not do -----------------------------------------------------------------

TEST(QuantizedEdits, ChunksOutsideTheWindowAreReportedAsSkipped) {
    Bench b;
    b.Load();
    // Window chunks x -4..3: voxel 128 is the first one outside. A sphere centred on that border
    // reaches into chunks 3 and 4, in y 1 and 2 (border at voxel 64).
    const SphereQ s{glm::ivec3(128 * 16, 64 * 16, 16 * 16), 5 * 16};
    std::vector<VoxelEdit> edits;
    EditReport report;
    const uint32_t changed = b.world->FillSphereQ(s, kRock, &edits, &report);
    EXPECT_GT(changed, 0u);                                  // the loaded half was filled
    for (const VoxelEdit& e : edits) EXPECT_LT(e.voxel.x, 128);
    EXPECT_EQ(ChunkKeys(report.skipped), (std::set<VoxelKey>{{4, 1, 0}, {4, 2, 0}}));
    EXPECT_TRUE(report.unsynced.empty());

    // Exactness: a chunk inside the sphere's bounding box that no voxel of the sphere reaches is
    // not reported. Bounds of this sphere reach chunk (4, 2, 1) at a corner only.
    const SphereQ corner{glm::ivec3(126 * 16, 62 * 16, 30 * 16), 3 * 16};
    glm::ivec3 mn, mx;
    SphereVoxelBounds(corner, mn, mx);
    ASSERT_EQ(VoxelToChunk(mx - glm::ivec3(1)), glm::ivec3(4, 2, 1));
    EditReport cornerReport;
    b.world->FillSphereQ(corner, kRock, nullptr, &cornerReport);
    EXPECT_EQ(ChunkKeys(cornerReport.skipped), (std::set<VoxelKey>{{4, 1, 0}}));
}

TEST(QuantizedEdits, LoadingChunksAreSkippedAndNothingChanges) {
    Bench b;
    b.Load();
    // Move the window far away; the new chunks are Unloaded or Loading until integrated.
    b.world->Update(glm::dvec3(5000.0, 20.0, 0.0));
    const glm::ivec3 v = b.world->WorldToVoxel(glm::dvec3(5000.0, 20.0, 0.0));
    const glm::ivec3 chunk = VoxelToChunk(v);
    const CellState state = b.world->StateOf(chunk);
    ASSERT_TRUE(state == CellState::Loading || state == CellState::Unloaded);

    EditReport report;
    const uint32_t changed = b.world->FillBox(v, v + glm::ivec3(1), kRock, nullptr, &report);
    EXPECT_EQ(changed, 0u);
    ASSERT_EQ(report.skipped.size(), 1u);
    EXPECT_EQ(report.skipped[0], chunk);
    b.jobs.WaitIdle();
}

TEST(QuantizedEdits, FullStoreMarksTheChunkUnsyncedAndLeavesItUntouched) {
    Bench b(512);
    b.Load();
    while (b.store.Alloc() != ChunkStore::kInvalidSlot) {}
    // Deep rock is a Uniform chunk: editing it needs a slot.
    const glm::ivec3 rock(5, -40, 5);
    ASSERT_EQ(b.world->StateOf(VoxelToChunk(rock)), CellState::Uniform);
    EditReport report;
    std::vector<VoxelEdit> edits;
    const uint32_t changed = b.world->CarveSphereQ(SphereQ{rock * 16 + glm::ivec3(8), 48}, 65535, &edits, &report);
    EXPECT_EQ(changed, 0u);
    EXPECT_TRUE(edits.empty());
    EXPECT_TRUE(report.skipped.empty());
    ASSERT_EQ(report.unsynced.size(), 1u);
    EXPECT_EQ(report.unsynced[0], VoxelToChunk(rock));
    EXPECT_EQ(b.world->StateOf(VoxelToChunk(rock)), CellState::Uniform);
    EXPECT_EQ(b.world->GetVoxel(rock), kRock);
}
