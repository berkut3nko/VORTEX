// Hollowkeep's greybox on the baked map (docs/design/land.md "Hollowkeep's greybox"): built on the natural slope, every
// wall and tower reaching below its own ground, the wall walk stepping with the ground, the portcullis and its lever.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>

import vortex.voxel;
import vortex.sim;

using namespace vortex;

namespace {
    /// A plain (the starter region) with a mountain knoll on it, a ridge to the west, the sea to the south.
    voxel::LandSource KnollOnAPlain() {
        voxel::LandSource s;
        s.width = 200;
        s.height = 160;
        s.metresPerPixel = 5.0f;
        s.heights.resize(size_t(200) * 160);
        s.biomes.resize(s.heights.size());
        s.regions.resize(s.heights.size());
        for (int z = 0; z < 160; ++z)
            for (int x = 0; x < 200; ++x) {
                const size_t i = size_t(z) * 200 + size_t(x);
                if (z >= 140) {
                    s.heights[i] = -6.0f;
                    s.biomes[i] = uint8_t(voxel::LandBiome::Sea);
                    continue;
                }
                if (x < 70) {
                    s.heights[i] = 20.0f + float(std::min(70 - x, 30)) * 3.0f;
                    s.biomes[i] = uint8_t(voxel::LandBiome::Mountain);
                    s.regions[i] = 6;
                    continue;
                }
                s.heights[i] = 3.0f + float(x - 70) * 0.02f + float(140 - z) * 0.03f;
                s.biomes[i] = uint8_t(voxel::LandBiome::Grassland);
                s.regions[i] = 2;
                const float dx = float(x - 110), dz = float(z - 60);
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d < 18.0f) {
                    s.heights[i] += 32.0f * (1.0f - d / 18.0f) - (dz > 0.0f ? dz * 0.2f : 0.0f);
                    s.biomes[i] = uint8_t(voxel::LandBiome::Mountain);
                    s.regions[i] = 6;
                }
            }
        s.starterRegion = 2;
        return s;
    }

    struct Baked {
        voxel::LandMap map;
        voxel::LandGeneratorSettings land;
        sim::RegionTemplate castle;
    };

    const Baked& Castle() {
        static const Baked b = [] {
            Baked out;
            out.map = voxel::BakeLand(KnollOnAPlain());
            voxel::MaterialLibrary library;
            out.land.materials = voxel::RegisterDefaultWorldMaterials(library);
            out.land.origin = voxel::LandWorldOrigin(out.map);
            out.castle = sim::HollowkeepTemplate(out.map, out.land);
            return out;
        }();
        return b;
    }
}

TEST(Hollowkeep, EveryTallStructureReachesBelowItsOwnGround) {
    const Baked& b = Castle();
    ASSERT_TRUE(b.map.site.placed);
    ASSERT_TRUE(b.castle.sited);
    const float vs = b.land.voxelSize;
    int tall = 0;
    for (const sim::WorldOp& op : b.castle.ops) {
        if (op.kind != sim::WorldOpKind::FillBox || op.id == 0) continue;
        const glm::ivec3 lo = op.min, hi = op.max;
        if (float(hi.y - lo.y) * vs < 6.0f) continue;   // walls, towers, the gatehouse, the keep
        // A box stacked on another of the same footprint (the gatehouse's and the keep's upper parts) stands on it: the
        // lowest box of the footprint is the one that reaches the ground.
        int foot = lo.y;
        for (const sim::WorldOp& other : b.castle.ops)
            if (other.kind == sim::WorldOpKind::FillBox && other.id != 0 && other.min.x == lo.x && other.min.z == lo.z && other.max.x == hi.x &&
                other.max.z == hi.z)
                foot = std::min(foot, other.min.y);
        const float cx = (float(lo.x + hi.x) * 0.5f) * vs, cz = (float(lo.z + hi.z) * 0.5f) * vs;
        const float ground = voxel::LandHeight(b.map, b.land, cx, cz);
        EXPECT_LE(float(foot) * vs, ground - 1.0f) << "a structure starts above the ground at (" << cx << ", " << cz << ")";
        ++tall;
    }
    EXPECT_GT(tall, 200);
}

TEST(Hollowkeep, TheWallWalkStepsWithTheGround) {
    const Baked& b = Castle();
    ASSERT_TRUE(b.castle.sited);
    std::set<int> tops;
    for (const sim::WorldOp& op : b.castle.ops)
        if (op.kind == sim::WorldOpKind::FillBox && op.id == b.land.materials.stone) tops.insert(op.max.y);
    // Each run's walk follows its own highest ground: a castle on a slope has many walk heights, not one.
    EXPECT_GT(tops.size(), 6u);
}

TEST(Hollowkeep, ThePortcullisAndItsLeverPassTheAuthoringRule) {
    const Baked& b = Castle();
    ASSERT_EQ(b.castle.mechanisms.size(), 2u);
    EXPECT_EQ(b.castle.mechanisms[0].desc.kind, sim::MechanismKind::Portcullis);
    EXPECT_EQ(b.castle.mechanisms[1].desc.kind, sim::MechanismKind::Lever);
    EXPECT_EQ(b.castle.mechanisms[1].link, 0);
    std::string why;
    EXPECT_TRUE(sim::CheckMechanismAuthoring(b.castle, b.land.voxelSize, &why)) << why;
}
