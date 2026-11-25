// The starter region in detail (docs/design/land.md L6-L8, [land-4]): Hollowkeep's site keeps the castle draft's
// heights, every road reaches its point within the grade, the cave lies under rock, and points.json places points.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

import vortex.voxel;

using namespace vortex::voxel;

namespace {
    constexpr int kW = 200, kH = 160;    // 5 m pixels: 1000 x 800 m

    /// A ridge in the west (mountain, region 6), a grass plain east of it (region 2, the starter region), the sea in the
    /// south.
    LandSource RidgeAndPlain() {
        LandSource s;
        s.width = kW;
        s.height = kH;
        s.metresPerPixel = 5.0f;
        const size_t n = size_t(kW) * kH;
        s.heights.resize(n);
        s.biomes.resize(n);
        s.regions.resize(n);
        for (int z = 0; z < kH; ++z)
            for (int x = 0; x < kW; ++x) {
                const size_t i = size_t(z) * kW + size_t(x);
                if (z >= 140) {
                    s.heights[i] = -6.0f;
                    s.biomes[i] = uint8_t(LandBiome::Sea);
                } else if (x < 70) {
                    s.heights[i] = 20.0f + float(std::min(70 - x, 30)) * 3.0f;
                    s.biomes[i] = uint8_t(LandBiome::Mountain);
                    s.regions[i] = 6;
                } else {
                    s.heights[i] = 3.0f + float(x - 70) * 0.02f + float(140 - z) * 0.03f;
                    s.biomes[i] = uint8_t(LandBiome::Grassland);
                    s.regions[i] = 2;
                }
            }
        s.starterRegion = 2;
        return s;
    }

    const LandMap& Baked() {
        static const LandMap map = BakeLand(RidgeAndPlain());
        return map;
    }

    float Ground(const LandMap& m, float mx, float mz) { return LandCoarseHeight(m, mx, mz) + LandFineDelta(m, mx, mz); }

    /// Map metres of a draft point (X east, Z south of the castle's middle), turned by the site's rotation (0: the
    /// draft's north faces west).
    glm::vec2 Draft(const LandSite& site, float X, float Z) {
        switch (site.rotation & 3) {
        case 0: return glm::vec2(site.x + Z, site.z - X);
        case 1: return glm::vec2(site.x + X, site.z + Z);
        case 2: return glm::vec2(site.x - Z, site.z + X);
        default: return glm::vec2(site.x - X, site.z - Z);
        }
    }

    const LandPoint* Find(const LandMap& m, LandPointKind kind) {
        for (const LandPoint& p : m.points)
            if (p.kind == kind) return &p;
        return nullptr;
    }
}

// The starter bake (site search, points, A* roads, cave) is as deterministic as the coarse bake: pinned on every
// platform. A change to it changes this value on purpose; the new value is printed.
TEST(LandStarter, GoldenHash) {
    const uint64_t hash = LandHash(Baked());
    std::printf("LandStarter golden hash: 0x%016llxull\n", static_cast<unsigned long long>(hash));
    EXPECT_EQ(hash, kLandStarterGoldenHash);
}

TEST(LandStarter, TheSiteKeepsTheCastleDraftsHeights) {
    const LandMap& m = Baked();
    ASSERT_FALSE(m.fine.Empty());
    ASSERT_TRUE(m.site.placed);
    // castle-layout-draft.md section 5, in metres above the approach field.
    struct Spot {
        float X, Z, height;
        const char* what;
    };
    // The approach field and the castle's footprint are set exactly.
    for (const Spot s : {Spot{0, -60, 0, "approach field"}, Spot{-20, -20, 0, "outer ward"}, Spot{10, 20, 5, "inner ward"}}) {
        const glm::vec2 p = Draft(m.site, s.X, s.Z);
        EXPECT_NEAR(Ground(m, p.x, p.y), m.site.fieldHeight + s.height, 0.02f) << s.what;
    }
    // The east slope, 6 m down over 14 m, its edge moved by up to 4 m of noise: at X = 40 it is 4-12 m out.
    const glm::vec2 east = Draft(m.site, 40.0f, 0.0f);
    EXPECT_LE(Ground(m, east.x, east.y), m.site.fieldHeight - 6.0f * 4.0f / 14.0f + 0.02f);
    EXPECT_GE(Ground(m, east.x, east.y), m.site.fieldHeight - 6.0f * 12.0f / 14.0f - 0.02f);
    // The ravines below the cliffs: at the draft's depth or at most 4 m deeper.
    for (const Spot s : {Spot{0, 50, -12, "south ravine"}, Spot{-42, 0, -11, "west stream bed"}}) {
        const glm::vec2 p = Draft(m.site, s.X, s.Z);
        EXPECT_LE(Ground(m, p.x, p.y), m.site.fieldHeight + s.height + 0.02f) << s.what;
        EXPECT_GE(Ground(m, p.x, p.y), m.site.fieldHeight + s.height - 4.02f) << s.what;
    }
    // The neck faces higher ground than the tip (the spur leads down from the ridge).
    const glm::vec2 neck = Draft(m.site, 0.0f, -100.0f), tip = Draft(m.site, 0.0f, 70.0f);
    EXPECT_GT(Ground(m, neck.x, neck.y), Ground(m, tip.x, tip.y));
    // The field stands above the plain the castle looks over.
    EXPECT_GT(m.site.fieldHeight, 10.0f);
}

TEST(LandStarter, EveryRoadReachesItsPointWithinTheGrade) {
    const LandMap& m = Baked();
    const LandSettings st;
    for (LandPointKind kind : {LandPointKind::Village, LandPointKind::Landing, LandPointKind::Cave, LandPointKind::Castle}) {
        ASSERT_NE(Find(m, kind), nullptr) << LandPointName(kind);
    }
    int toCastle = 0, toLanding = 0, toCave = 0;
    for (const LandRoad& road : m.roads) {
        const LandPoint& from = m.points[road.from];
        const LandPoint& to = m.points[road.to];
        EXPECT_EQ(from.kind, LandPointKind::Village);
        ASSERT_GE(road.x.size(), 2u);
        EXPECT_LT(std::abs(road.x.front() - from.x) + std::abs(road.z.front() - from.z), 2.0f);
        glm::vec2 target(to.x, to.z);
        if (to.kind == LandPointKind::Castle) {
            target = Draft(m.site, 0.0f, -60.0f);   // the road ends on the approach field
            ++toCastle;
        }
        toLanding += to.kind == LandPointKind::Landing;
        toCave += to.kind == LandPointKind::Cave;
        EXPECT_LT(std::abs(road.x.back() - target.x) + std::abs(road.z.back() - target.y), 2.0f) << LandPointName(to.kind);
        for (size_t a = 1; a < road.x.size(); ++a) {
            // At most a cell's diagonal from the last point, never up or down more than the grade.
            const float dx = std::abs(road.x[a] - road.x[a - 1]), dz = std::abs(road.z[a] - road.z[a - 1]);
            ASSERT_TRUE(dx <= 1.0f && dz <= 1.0f);
            EXPECT_LE(std::abs(road.bed[a] - road.bed[a - 1]), st.roadMaxGrade * std::sqrt(dx * dx + dz * dz) + 1.0e-4f);
        }
        EXPECT_LE(road.maxGrade, st.roadMaxGrade + 1.0e-4f);
    }
    EXPECT_EQ(toCastle, 1);
    EXPECT_EQ(toLanding, 1);
    EXPECT_EQ(toCave, 1);
}

TEST(LandStarter, TheCaveLiesUnderRockAndIsCarved) {
    const auto map = std::make_shared<const LandMap>(Baked());
    const LandCave& cave = map->cave;
    ASSERT_TRUE(cave.placed);
    EXPECT_GE(Ground(*map, cave.x[1], cave.z[1]), cave.y[1] + cave.chamberRadius + 2.0f);
    // The chamber's middle is air in the generated chunk, the rock beside the tunnel is not.
    LandGeneratorSettings s;
    MaterialLibrary library;
    s.materials = RegisterDefaultWorldMaterials(library);
    s.origin = CenteredLandOrigin(*map);
    const auto generator = CreateLandGenerator(map, s);
    auto voxelAt = [&](glm::vec3 world) {
        const glm::ivec3 v(int(std::floor(world.x / s.voxelSize)), int(std::floor(world.y / s.voxelSize)), int(std::floor(world.z / s.voxelSize)));
        const glm::ivec3 c(v.x >> 5, v.y >> 5, v.z >> 5);
        auto chunk = std::make_unique<Chunk>();
        generator->Generate(c, *chunk);
        return chunk->Get(v.x - c.x * kChunkSize, v.y - c.y * kChunkSize, v.z - c.z * kChunkSize);
    };
    const glm::vec3 chamber(cave.x[1] + s.origin.x, cave.y[1], cave.z[1] + s.origin.y);
    EXPECT_EQ(voxelAt(chamber), 0);
    EXPECT_NE(voxelAt(chamber + glm::vec3(0.0f, cave.chamberRadius + 1.0f, 0.0f)), 0);
}

TEST(LandStarter, ColumnClassesAgreeAroundTheCaveAndTheSite) {
    const auto map = std::make_shared<const LandMap>(Baked());
    LandGeneratorSettings s;
    MaterialLibrary library;
    s.materials = RegisterDefaultWorldMaterials(library);
    s.origin = CenteredLandOrigin(*map);
    const auto generator = CreateLandGenerator(map, s);
    s.skipClassifiedRows = false;
    const auto full = CreateLandGenerator(map, s);
    auto a = std::make_unique<Chunk>(), b = std::make_unique<Chunk>();
    const float chunkMetres = kChunkSize * s.voxelSize;
    for (const glm::vec2 at : {glm::vec2(map->cave.x[1], map->cave.z[1]), glm::vec2(map->cave.x[0], map->cave.z[0]),
                               glm::vec2(map->site.x - 40.0f, map->site.z)}) {
        const int cx = int(std::floor((at.x + s.origin.x) / chunkMetres)), cz = int(std::floor((at.y + s.origin.y) / chunkMetres));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const ColumnClass c = generator->ClassifyColumn(cx + dx, cz + dz);
                for (int row = c.solidBelow - 1; row <= c.emptyFrom; ++row) {
                    *a = Chunk{};
                    *b = Chunk{};
                    const ChunkContent ca = generator->Generate(glm::ivec3(cx + dx, row, cz + dz), *a);
                    full->Generate(glm::ivec3(cx + dx, row, cz + dz), *b);
                    if (row < c.solidBelow || row >= c.emptyFrom) {
                        // A classified row: every voxel of the full loop is what the class says.
                        EXPECT_EQ(ca.kind, row < c.solidBelow ? ChunkKind::Uniform : ChunkKind::Empty);
                        const uint8_t expected = row < c.solidBelow ? c.solidId : 0;
                        for (uint8_t id : b->ids) ASSERT_EQ(id, expected) << cx + dx << " " << row << " " << cz + dz;
                    } else {
                        ASSERT_EQ(ChunkHash(*a), ChunkHash(*b)) << cx + dx << " " << row << " " << cz + dz;
                    }
                }
            }
    }
}

TEST(LandStarter, PointsJsonPlacesPointsAndReadsBack) {
    const LandMap& m = Baked();
    const LandPoint* village = Find(m, LandPointKind::Village);
    ASSERT_NE(village, nullptr);
    // The owner moves the village 60 m east: the bake takes it from the file and leads the roads from there.
    LandSource source = RidgeAndPlain();
    source.points.push_back({LandPointKind::Village, village->x + 60.0f, village->z, true});
    const LandMap moved = BakeLand(source);
    const LandPoint* placed = Find(moved, LandPointKind::Village);
    ASSERT_NE(placed, nullptr);
    EXPECT_TRUE(placed->fromFile);
    EXPECT_EQ(placed->x, village->x + 60.0f);
    ASSERT_FALSE(moved.roads.empty());
    EXPECT_LT(std::abs(moved.roads.front().x.front() - placed->x), 1.0f);

    // points.json written by the bake reads back with every kind but the crossings.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vortex_land_points_test";
    std::filesystem::remove_all(dir);
    std::string error;
    ASSERT_TRUE(WriteLandFiles(m, dir, error)) << error;
    ASSERT_TRUE(WriteLandPoints(m, dir / "points.json", error)) << error;
    std::filesystem::rename(dir / "land_height.png", dir / "height.png");
    std::filesystem::copy_file(dir / "land_flow.png", dir / "biome_ids.png");
    std::filesystem::copy_file(dir / "land_flow.png", dir / "region_ids.png");
    std::ofstream(dir / "world.json") << "{\n \"metres_per_px\": 4.0\n}\n";
    LandSource back;
    ASSERT_TRUE(LoadLandSource(dir, back, error)) << error;
    size_t expected = 0;
    for (const LandPoint& p : m.points) expected += p.kind != LandPointKind::Crossing;
    ASSERT_EQ(back.points.size(), expected);
    size_t k = 0;
    for (const LandPoint& p : m.points) {
        if (p.kind == LandPointKind::Crossing) continue;
        EXPECT_EQ(back.points[k].kind, p.kind);
        EXPECT_NEAR(back.points[k].x, p.x, 0.05f);
        EXPECT_NEAR(back.points[k].z, p.z, 0.05f);
        EXPECT_TRUE(back.points[k].fromFile);
        ++k;
    }
    std::filesystem::remove_all(dir);
}
