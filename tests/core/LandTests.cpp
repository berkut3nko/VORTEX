// The world map bake (docs/design/land.md, [land-2]): erosion and water at fixed levels on a small drawn island.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

import vortex.voxel;

using namespace vortex::voxel;

namespace {
    constexpr int kSize = 160;            // pixels; 5 m each, so 200 x 200 cells of 4 m
    constexpr int kLakeX = 62, kLakeZ = 70, kLakeRadius = 9;

    /// A round island: a mountain ridge across the middle, grassland around it, a drawn lake in a bowl.
    LandSource TestIsland() {
        LandSource s;
        s.width = s.height = kSize;
        s.metresPerPixel = 5.0f;
        const size_t n = size_t(kSize) * kSize;
        s.heights.resize(n);
        s.biomes.resize(n);
        s.regions.resize(n);
        for (int z = 0; z < kSize; ++z) {
            for (int x = 0; x < kSize; ++x) {
                const size_t i = size_t(z) * kSize + size_t(x);
                const float dx = float(x - kSize / 2), dz = float(z - kSize / 2);
                const float d = std::sqrt(dx * dx + dz * dz);
                const float radius = 62.0f + 6.0f * std::sin(std::atan2(dz, dx) * 5.0f);
                if (d > radius) {
                    s.heights[i] = -4.0f - (d - radius) * 0.5f;
                    s.biomes[i] = uint8_t(LandBiome::Sea);
                    continue;
                }
                const float rise = (radius - d) * 0.6f;
                const float ridge = std::max(0.0f, 1.0f - std::abs(dz - 0.3f * dx) / 14.0f);
                float h = 0.5f + rise + 55.0f * ridge * (rise / 37.0f) + 2.0f * std::sin(float(x) * 0.7f) * std::cos(float(z) * 0.5f);
                uint8_t biome = ridge > 0.3f ? uint8_t(LandBiome::Mountain) : uint8_t(LandBiome::Grassland);
                const float lx = float(x - kLakeX), lz = float(z - kLakeZ);
                const float ld = std::sqrt(lx * lx + lz * lz);
                if (ld < float(kLakeRadius)) {
                    h = 14.0f - 3.0f * (1.0f - ld / float(kLakeRadius));
                    biome = uint8_t(LandBiome::Lake);
                } else if (ld < float(kLakeRadius) + 4.0f) {
                    h = std::max(h, 18.0f);
                }
                s.heights[i] = std::max(0.5f, h);
                s.biomes[i] = biome;
                s.regions[i] = biome == uint8_t(LandBiome::Lake) ? 0 : biome;
            }
        }
        return s;
    }

    /// The island is small (0.3 km^2 of land): rivers start at a smaller drainage area than on the drawn map.
    LandSettings TestSettings() {
        LandSettings st;
        st.riverArea = 8000.0f;
        return st;
    }

    const LandMap& Baked() {
        static const LandMap map = BakeLand(TestIsland(), TestSettings());
        return map;
    }
}

// The map is the seed: the same files must bake to the same world on every machine. The bake uses no libm function
// (land.md "The bake as built"), so this value is the same on every platform; a change to the bake changes it on
// purpose, and the new value is printed.
TEST(Land, BakeGoldenHash) {
    const uint64_t hash = LandHash(Baked());
    std::printf("Land bake golden hash: 0x%016llxull\n", static_cast<unsigned long long>(hash));
    EXPECT_EQ(hash, 0x61fc51804ff5f1feull);
}

TEST(Land, TheSameSourceBakesTheSameMapTwice) {
    EXPECT_EQ(LandHash(Baked()), LandHash(BakeLand(TestIsland(), TestSettings())));
}

TEST(Land, TheBakedMapKeepsItsRules) {
    const LandMap& m = Baked();
    EXPECT_EQ(m.width, 200);
    EXPECT_EQ(m.height, 200);
    EXPECT_EQ(CheckLand(m), "");
    ASSERT_FALSE(m.rivers.empty());
    bool drawnLake = false;
    for (const LandLake& lake : m.lakes) drawnLake = drawnLake || lake.drawn;
    EXPECT_TRUE(drawnLake);
}

TEST(Land, EveryRiverReachesTheSeaOrALake) {
    const LandMap& m = Baked();
    for (const LandRiver& river : m.rivers) {
        const uint32_t end = river.endCell;
        switch (river.end) {
        case RiverEnd::Sea: EXPECT_TRUE(m.waterKind[end] == WaterKind::Sea || end == river.cells.back()); break;
        case RiverEnd::Lake: EXPECT_EQ(m.waterKind[end], WaterKind::Lake); break;
        case RiverEnd::River: EXPECT_EQ(m.waterKind[end], WaterKind::River); break;
        }
        for (size_t k = 1; k < river.reaches.size(); ++k) {
            EXPECT_LT(river.reaches[k].level, river.reaches[k - 1].level);
        }
    }
}

TEST(Land, ALakesDryShoreNeverLiesBelowItsLevel) {
    const LandMap& m = Baked();
    ASSERT_FALSE(m.lakes.empty());
    for (int z = 1; z + 1 < m.height; ++z) {
        for (int x = 1; x + 1 < m.width; ++x) {
            const uint32_t i = m.Index(x, z);
            if (m.waterKind[i] != WaterKind::Lake) continue;
            const float level = m.lakes[m.lakeOf[i] - 1].level;
            EXPECT_EQ(m.water[i], level);
            for (uint32_t n : {m.Index(x + 1, z), m.Index(x - 1, z), m.Index(x, z + 1), m.Index(x, z - 1)}) {
                if (m.waterKind[n] == WaterKind::None) EXPECT_GE(m.heights[n], level - 0.02f) << "cell " << n;
            }
        }
    }
}

TEST(Land, ErosionCutsValleysWithinItsCaps) {
    const LandMap& m = Baked();
    const LandSettings st;
    float deepest = 0.0f;
    for (size_t i = 0; i < m.heights.size(); ++i) {
        if (m.biomes[i] == uint8_t(LandBiome::Sea) || m.biomes[i] == uint8_t(LandBiome::Lake)) continue;
        const float cut = m.rawHeights[i] - m.eroded[i];
        deepest = std::max(deepest, cut);
        EXPECT_LE(cut, st.maxStreamCut + 1.0e-3f) << "cell " << i;
    }
    EXPECT_GT(deepest, 1.0f);
}

TEST(Land, TheChecksFire) {
    LandMap m = Baked();
    ASSERT_FALSE(m.lakes.empty());
    ASSERT_FALSE(m.rivers.empty());
    {
        LandMap bad = m;
        for (size_t i = 0; i < bad.heights.size(); ++i) {
            if (bad.waterKind[i] == WaterKind::Lake) {
                bad.water[i] += 0.5f;
                break;
            }
        }
        EXPECT_NE(CheckLand(bad).find("two levels"), std::string::npos);
    }
    {
        LandMap bad = m;
        const auto river = std::find_if(bad.rivers.begin(), bad.rivers.end(), [](const LandRiver& r) { return r.cells.size() >= 2; });
        ASSERT_NE(river, bad.rivers.end());
        bad.water[river->cells.back()] = bad.water[river->cells.front()] + 1.0f;
        EXPECT_NE(CheckLand(bad).find("rises downstream"), std::string::npos);
    }
    {
        LandMap bad = m;
        bad.rivers.front().end = RiverEnd::River;
        bad.rivers.front().endCell = bad.rivers.front().cells.front();   // a river that joins itself
        EXPECT_NE(CheckLand(bad).find("does not reach"), std::string::npos);
    }
    {
        LandMap bad = m;
        for (size_t i = 0; i < bad.heights.size(); ++i) {
            if (bad.waterKind[i] == WaterKind::Sea) {
                bad.heights[i] = 1.0f;
                break;
            }
        }
        EXPECT_NE(CheckLand(bad).find("sea cell rose"), std::string::npos);
    }
}

TEST(Land, WrittenHeightsReadBackWithinAStep) {
    const LandMap& m = Baked();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vortex_land_test";
    std::filesystem::remove_all(dir);
    std::string error;
    ASSERT_TRUE(WriteLandFiles(m, dir, error)) << error;
    std::filesystem::rename(dir / "land_height.png", dir / "height.png");
    std::filesystem::copy_file(dir / "land_flow.png", dir / "biome_ids.png");
    std::filesystem::copy_file(dir / "land_flow.png", dir / "region_ids.png");
    std::ofstream(dir / "world.json") << "{\n \"metres_per_px\": 4.0\n}\n";
    LandSource back;
    ASSERT_TRUE(LoadLandSource(dir, back, error)) << error;
    ASSERT_EQ(back.width, m.width);
    ASSERT_EQ(back.height, m.height);
    EXPECT_EQ(back.metresPerPixel, 4.0f);
    for (size_t i = 0; i < m.heights.size(); ++i) {
        ASSERT_NEAR(back.heights[i], m.heights[i], 0.5f / 256.0f + 1.0e-4f) << "cell " << i;
    }
    std::filesystem::remove_all(dir);
}
