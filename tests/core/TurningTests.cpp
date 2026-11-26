// Magic's integer building blocks in Core (docs/design/magic.md "The Turning table", "Apply",
// "Determinism traps"): the Turning maps and their hash, knit iron in the world palette, the
// shapes of the map edits, and the run encoding of masks.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    using VoxelKey = std::tuple<int, int, int>;

    /// The voxels of [min, max) for which `in(v)` holds.
    template <typename In>
    std::set<VoxelKey> VoxelsIn(const glm::ivec3& min, const glm::ivec3& max, In&& in) {
        std::set<VoxelKey> out;
        for (int z = min.z; z < max.z; ++z)
            for (int y = min.y; y < max.y; ++y)
                for (int x = min.x; x < max.x; ++x)
                    if (in(glm::ivec3(x, y, z))) out.emplace(x, y, z);
        return out;
    }

    ShapeQ Capsule(const glm::ivec3& a, const glm::ivec3& b, uint16_t radiusQ) {
        ShapeQ s;
        s.kind = kShapeCapsule;
        s.a = a;
        s.b = b;
        s.radiusQ = radiusQ;
        return s;
    }
}

TEST(TurningTable, NeverNameBedrockAndIdentityElsewhere) {
    MaterialLibrary library;
    const WorldMaterials ids = RegisterDefaultWorldMaterials(library);
    const TurningTable table = BuildTurningTable(ids);
    ASSERT_NE(ids.knitIron, 0);
    for (uint32_t m = 0; m < kTurningMapCount; ++m) {
        const TurningMap& map = table.maps[m];
        EXPECT_EQ(map[ids.bedrock], ids.bedrock) << "map " << m;
        for (uint32_t id = 0; id < 256; ++id) {
            bool named = false;
            if (m == Map_Knit) named = id == 0;
            if (m == Map_Unknit || m == Map_Ablate) named = id == ids.knitIron;
            if (named) EXPECT_NE(map[id], id) << "map " << m << " id " << id;
            else EXPECT_EQ(map[id], id) << "map " << m << " id " << id;
        }
    }
    EXPECT_EQ(table.maps[Map_Knit][0], ids.knitIron);
    EXPECT_EQ(table.maps[Map_Unknit][ids.knitIron], 0);
    EXPECT_EQ(table.maps[Map_Ablate][ids.knitIron], 0);
    // The process-wide table is the default palette's.
    EXPECT_EQ(WorldTurningTable().maps, table.maps);
}

TEST(TurningTable, HashCoversEveryMap) {
    MaterialLibrary library;
    const TurningTable table = BuildTurningTable(RegisterDefaultWorldMaterials(library));
    const uint64_t base = TurningTableHash(table);
    EXPECT_EQ(base, TurningTableHash(WorldTurningTable()));
    for (const auto& [m, id] : {std::pair{0, 255}, std::pair{3, 0}, std::pair{15, 200}, std::pair{14, 1}, std::pair{9, 128}}) {
        TurningTable changed = table;
        changed.maps[m][id] = static_cast<uint8_t>(changed.maps[m][id] + 1);
        EXPECT_NE(TurningTableHash(changed), base) << "map " << m << " id " << id;
    }
}

TEST(Materials, KnitIronIsConjuredAndStoneStrong) {
    MaterialLibrary library;
    const WorldMaterials ids = RegisterDefaultWorldMaterials(library);
    // Appended after every id the palette had before: no id is renumbered.
    EXPECT_EQ(ids.knitIron, 44);
    EXPECT_EQ(ids.knitIron, ids.graniteDamp + 1);
    EXPECT_EQ(library.Name(kWorldPalette, ids.knitIron), "knit_iron");
    const Material& m = library.Get(kWorldPalette, ids.knitIron);
    EXPECT_EQ(m.hardness, 7.0f);
    EXPECT_EQ(m.density, 2000.0f);
    EXPECT_EQ(m.flags, static_cast<uint32_t>(MaterialFlag_Conjured));
    EXPECT_EQ(m.strength[0], 100000u);
    EXPECT_EQ(m.strength[1], 40000u);
    EXPECT_EQ(m.strength[2], 15000u);
    // No other material of the palette is conjured.
    for (uint32_t id = 1; id < 256; ++id) {
        if (id == ids.knitIron) continue;
        EXPECT_EQ(library.Get(kWorldPalette, static_cast<uint8_t>(id)).flags & MaterialFlag_Conjured, 0u) << id;
    }
}

TEST(ShapeQ, CapsuleOfZeroLengthIsASphere) {
    for (const auto& [centre, radius] : {std::pair{glm::ivec3(53, 37, 71), uint16_t{37}}, std::pair{glm::ivec3(-40, -9, 24), uint16_t{40}},
                                         std::pair{glm::ivec3(8, 8, 8), uint16_t{0}}}) {
        const ShapeQ capsule = Capsule(centre, centre, radius);
        const SphereQ sphere{centre, radius};
        const glm::ivec3 v0 = math::FloorDiv(centre, 16) - glm::ivec3(3);
        const auto inCapsule = VoxelsIn(v0, v0 + glm::ivec3(6), [&](const glm::ivec3& v) { return InShapeQ(capsule, v); });
        const auto inSphere = VoxelsIn(v0, v0 + glm::ivec3(6), [&](const glm::ivec3& v) { return InSphereQ(sphere, v); });
        EXPECT_EQ(inCapsule, inSphere);
        if (radius > 0) EXPECT_FALSE(inCapsule.empty());
    }
}

TEST(ShapeQ, CapsuleMatchesAChainOfSpheres) {
    const glm::ivec3 a(16 * 12 + 3, 16 * 20 + 5, 16 * 20 + 9);
    const glm::ivec3 box(40);
    // Along x every voxel's nearest axis point has an integer x: the chain of spheres at every q
    // step is the capsule exactly.
    {
        const ShapeQ capsule = Capsule(a, a + glm::ivec3(160, 0, 0), 51);
        const auto inCapsule = VoxelsIn(glm::ivec3(0), box, [&](const glm::ivec3& v) { return InShapeQ(capsule, v); });
        const auto inChain = VoxelsIn(glm::ivec3(0), box, [&](const glm::ivec3& v) {
            for (int k = 0; k <= 160; ++k)
                if (InSphereQ(SphereQ{a + glm::ivec3(k, 0, 0), 51}, v)) return true;
            return false;
        });
        EXPECT_FALSE(inCapsule.empty());
        EXPECT_EQ(inCapsule, inChain);
    }
    // A slanted axis: the spheres every 5 q lie inside the capsule; the gaps between them add at most 2 %.
    {
        const ShapeQ capsule = Capsule(a, a + glm::ivec3(96, 128, 0), 51);
        const auto inCapsule = VoxelsIn(glm::ivec3(0), box, [&](const glm::ivec3& v) { return InShapeQ(capsule, v); });
        const auto inChain = VoxelsIn(glm::ivec3(0), box, [&](const glm::ivec3& v) {
            for (int k = 0; k <= 32; ++k)
                if (InSphereQ(SphereQ{a + k * glm::ivec3(3, 4, 0), 51}, v)) return true;
            return false;
        });
        ASSERT_FALSE(inChain.empty());
        for (const VoxelKey& v : inChain) EXPECT_TRUE(inCapsule.contains(v));
        EXPECT_LE(inCapsule.size() * 100, inChain.size() * 102);
    }
}

TEST(ShapeQ, FarCapsuleCoordinatesStayExact) {
    const glm::ivec3 a(16 * 12 + 3, 16 * 20 + 5, 16 * 20 + 9);
    const ShapeQ near = Capsule(a, a + glm::ivec3(160, 0, 0), 51);
    const glm::ivec3 shiftQ(1 << 30, 0, 1 << 30);
    const glm::ivec3 shift(1 << 26, 0, 1 << 26);
    const ShapeQ far = Capsule(near.a + shiftQ, near.b + shiftQ, 51);
    const auto inNear = VoxelsIn(glm::ivec3(0), glm::ivec3(40), [&](const glm::ivec3& v) { return InShapeQ(near, v); });
    const auto inFar = VoxelsIn(shift, shift + glm::ivec3(40), [&](const glm::ivec3& v) { return InShapeQ(far, v); });
    ASSERT_EQ(inNear.size(), inFar.size());
    for (const auto& [x, y, z] : inNear) EXPECT_TRUE(inFar.contains({x + shift.x, y, z + shift.z}));
}

TEST(ShapeQ, BoundsHoldEveryVoxelOfEachKind) {
    std::vector<ShapeQ> shapes;
    shapes.push_back(ShapeQ{kShapeSphere, {-37, 21, 300}, glm::ivec3(0), 75, 0});
    shapes.push_back(Capsule({-100, 50, -7}, {60, -90, 41}, 30));
    shapes.push_back(Capsule({8, 8, 8}, {8, 8, 8}, 0));   // radius 0: the one voxel whose centre it is
    shapes.push_back(ShapeQ{kShapeBox, {-3, 2, -5}, {4, 6, 1}, 0, 0});
    shapes.push_back(ShapeQ{kShapeCylinder, {-50, 30, 19}, glm::ivec3(0), 45, 70});
    for (const ShapeQ& shape : shapes) {
        glm::ivec3 lo, hi;
        ShapeVoxelBounds(shape, lo, hi);
        ASSERT_TRUE(hi.x > lo.x && hi.y > lo.y && hi.z > lo.z) << static_cast<int>(shape.kind);
        const auto inside = VoxelsIn(lo, hi, [&](const glm::ivec3& v) { return InShapeQ(shape, v); });
        const auto around = VoxelsIn(lo - glm::ivec3(4), hi + glm::ivec3(4), [&](const glm::ivec3& v) { return InShapeQ(shape, v); });
        EXPECT_FALSE(inside.empty()) << static_cast<int>(shape.kind);
        EXPECT_EQ(inside, around) << static_cast<int>(shape.kind);
    }
    // The cylinder is vertical: its height follows halfHeightQ, its width radiusQ.
    const ShapeQ cylinder{kShapeCylinder, {8, 8, 8}, glm::ivec3(0), 16, 40};
    EXPECT_TRUE(InShapeQ(cylinder, {0, 2, 0}));
    EXPECT_FALSE(InShapeQ(cylinder, {0, 3, 0}));
    EXPECT_TRUE(InShapeQ(cylinder, {1, 0, 0}));
    EXPECT_FALSE(InShapeQ(cylinder, {1, 0, 1}));
    // A capsule longer than kMaxCapsuleAxisQ on an axis, and an unknown kind, hold no voxel.
    const ShapeQ tooLong = Capsule({-(1 << 30), 0, 0}, {(1 << 30) - 1, 0, 0}, 100);
    EXPECT_FALSE(InShapeQ(tooLong, {0, 0, 0}));
    glm::ivec3 lo, hi;
    ShapeVoxelBounds(tooLong, lo, hi);
    EXPECT_EQ(lo, hi);
    const ShapeQ unknown{9, {0, 0, 0}, {16, 16, 16}, 100, 100};
    EXPECT_FALSE(InShapeQ(unknown, {0, 0, 0}));
    ShapeVoxelBounds(unknown, lo, hi);
    EXPECT_EQ(lo, hi);
}

TEST(MapMask, AxisOrderGivesTheSameVoxels) {
    std::mt19937 random(7);
    VoxelMask mask(glm::ivec3(37, 13, 21));
    for (int z = 0; z < 21; ++z)
        for (int y = 0; y < 13; ++y)
            for (int x = 0; x < 37; ++x)
                if (random() % 3 == 0) mask.Set(x, y, z, true);
    for (uint8_t axis = 0; axis <= 2; ++axis) {
        const std::vector<uint32_t> runs = MaskRuns(mask, axis);
        ASSERT_FALSE(runs.empty());
        uint64_t sum = 0;
        for (size_t i = 0; i < runs.size(); ++i) {
            sum += runs[i];
            if (i > 0) EXPECT_GT(runs[i], 0u) << "only the first run may be empty";
        }
        EXPECT_EQ(sum, mask.VoxelCount());
        VoxelMask back;
        ASSERT_TRUE(MaskFromRuns(mask.Size(), axis, runs, back)) << static_cast<int>(axis);
        EXPECT_EQ(back, mask) << static_cast<int>(axis);
        size_t bytes = 0;
        for (const uint32_t run : runs) bytes += run < 128 ? 1 : (run < 16384 ? 2 : 3);
        EXPECT_EQ(MaskRunBytes(mask, axis), bytes);
        // Runs that do not add up to the box are refused.
        std::vector<uint32_t> bad = runs;
        bad.back() += 1;
        EXPECT_FALSE(MaskFromRuns(mask.Size(), axis, bad, back));
        EXPECT_EQ(back.Count(), 0u);
    }
    VoxelMask back;
    EXPECT_FALSE(MaskFromRuns(mask.Size(), 3, MaskRuns(mask, 0), back));
    EXPECT_TRUE(MaskRuns(mask, 3).empty());

    // Axis 0 is the mask's own bit order (x fastest).
    VoxelMask row(glm::ivec3(8, 8, 8)), column(glm::ivec3(8, 8, 8));
    for (int i = 0; i < 8; ++i) {
        row.Set(i, 3, 5, true);
        column.Set(3, i, 5, true);
    }
    EXPECT_EQ(MaskRuns(row, 0), (std::vector<uint32_t>{8u * (3 + 8 * 5), 8u, 512u - 8u * (3 + 8 * 5) - 8u}));
    EXPECT_EQ(ShortestRunAxis(row), 0);
    EXPECT_EQ(ShortestRunAxis(column), 1);
    EXPECT_EQ(ShortestRunAxis(VoxelMask(glm::ivec3(4, 4, 4))), 0) << "a tie goes to the lower axis";
}

TEST(ISqrt64, IsTheFloorRootOfEveryValue) {
    EXPECT_EQ(math::ISqrt64(0), 0u);
    EXPECT_EQ(math::ISqrt64(1), 1u);
    EXPECT_EQ(math::ISqrt64(3), 1u);
    EXPECT_EQ(math::ISqrt64(4), 2u);
    EXPECT_EQ(math::ISqrt64(std::numeric_limits<uint64_t>::max()), 0xFFFFFFFFull);
    for (const uint64_t r : {uint64_t{94906265}, uint64_t{3037000499}, uint64_t{4294967295}, uint64_t{123456789}}) {
        EXPECT_EQ(math::ISqrt64(r * r), r);
        EXPECT_EQ(math::ISqrt64(r * r - 1), r - 1);
        if (r < 4294967295u) EXPECT_EQ(math::ISqrt64((r + 1) * (r + 1) - 1), r);
    }
}
