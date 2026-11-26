// The baked world map as voxels (docs/design/land.md "From the map to voxels", [land-3]): the sampling is pinned bit for
// bit, the column classes agree with the voxel loop, and water stands at the map's levels.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

import vortex.voxel;

using namespace vortex::voxel;

namespace {
    constexpr int kW = 24, kH = 20;
    constexpr int kLakeA = 10 + 8 * kW, kLakeB = 11 + 8 * kW;   // cells (10, 8) and (11, 8)
    constexpr float kLakeLevel = 20.0f;

    /// A hand-made map (no bake: the bake may differ between platforms, the sampling may not): a round island of
    /// forest, mountain and desert with hard bands, a two-cell lake, and the sea around it.
    std::shared_ptr<LandMap> SampleMap() {
        auto m = std::make_shared<LandMap>();
        m->width = kW;
        m->height = kH;
        m->cellSize = 4.0f;
        const size_t n = size_t(kW) * kH;
        m->heights.resize(n);
        m->water.assign(n, 0.0f);
        m->waterKind.assign(n, WaterKind::None);
        m->biomes.resize(n);
        m->regions.resize(n);
        m->hardness.resize(n);
        m->lakeOf.assign(n, 0);
        for (int z = 0; z < kH; ++z) {
            for (int x = 0; x < kW; ++x) {
                const uint32_t i = m->Index(x, z);
                const int r2 = (x - 12) * (x - 12) + (z - 10) * (z - 10);
                if (r2 > 64) {
                    m->heights[i] = -2.0f - float(r2 - 64) * 0.25f;
                    m->biomes[i] = uint8_t(LandBiome::Sea);
                    m->waterKind[i] = WaterKind::Sea;
                    continue;
                }
                m->heights[i] = 0.5f + float(64 - r2) * 0.4f;
                const LandBiome b = x < 12 ? LandBiome::Forest : (z < 10 ? LandBiome::Mountain : LandBiome::Desert);
                m->biomes[i] = uint8_t(b);
                m->regions[i] = uint8_t(b);
                m->hardness[i] = x % 3 == 0 ? 10 : 3;
            }
        }
        m->lakes.push_back({kLakeLevel, 2, 2.0f, true});
        for (uint32_t i : {uint32_t(kLakeA), uint32_t(kLakeB)}) {
            m->heights[i] = 18.0f;
            m->waterKind[i] = WaterKind::Lake;
            m->water[i] = kLakeLevel;
            m->biomes[i] = uint8_t(LandBiome::Lake);
            m->regions[i] = 0;
            m->lakeOf[i] = 1;
        }
        m->starterRegion = int(LandBiome::Forest);
        return m;
    }

    LandGeneratorSettings SampleSettings(const LandMap& m) {
        LandGeneratorSettings s;
        MaterialLibrary library;
        s.materials = RegisterDefaultWorldMaterials(library);
        s.origin = CenteredLandOrigin(m);
        return s;
    }

    glm::vec2 CellCentre(const LandMap& m, const LandGeneratorSettings& s, uint32_t cell) {
        return s.origin + glm::vec2((float(cell % uint32_t(m.width)) + 0.5f) * m.cellSize, (float(cell / uint32_t(m.width)) + 0.5f) * m.cellSize);
    }

    uint64_t Mix(uint64_t h, uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= h >> 31;
        h *= 0xbf58476d1ce4e5b9ull;
        return h ^ (h >> 29);
    }
}

TEST(LandSampling, GoldenHash) {
    const auto map = SampleMap();
    const LandGeneratorSettings s = SampleSettings(*map);
    uint64_t h = 0;
    // Heights and water on a 0.73 m grid over the map and 10 m past its edge.
    for (int k = 0; k < 160; ++k)
        for (int i = 0; i < 160; ++i) {
            const float x = -58.0f + float(i) * 0.73f, z = -50.0f + float(k) * 0.73f;
            h = Mix(h, std::bit_cast<uint32_t>(LandHeight(*map, s, x, z)));
            h = Mix(h, std::bit_cast<uint32_t>(LandWaterLevel(*map, s, x, z)));
        }
    // Every voxel of a few columns of chunks: the forest (with its trees), the lake, the mountain, the desert and the shore.
    const auto generator = CreateLandGenerator(map, s);
    auto chunk = std::make_unique<Chunk>();
    for (const glm::ivec2 column : {glm::ivec2(-2, -1), glm::ivec2(-1, -1), glm::ivec2(2, -2), glm::ivec2(1, 2), glm::ivec2(-7, 0), glm::ivec2(-3, 0)})
        for (int row = -4; row <= 8; ++row) {
            *chunk = Chunk{};
            const ChunkContent c = generator->Generate(glm::ivec3(column.x, row, column.y), *chunk);
            h = Mix(h, uint64_t(c.kind) << 8 | c.uniformId);
            if (c.kind == ChunkKind::Dense) h = Mix(h, ChunkHash(*chunk));
        }
    std::printf("LandSampling golden hash: 0x%016llxull\n", static_cast<unsigned long long>(h));
    EXPECT_EQ(h, kLandSamplingGoldenHash);
}

TEST(LandSampling, ClassifiedRowsAgreeWithTheVoxelLoop) {
    const auto map = SampleMap();
    LandGeneratorSettings s = SampleSettings(*map);
    const auto generator = CreateLandGenerator(map, s);
    s.skipClassifiedRows = false;
    const auto full = CreateLandGenerator(map, s);
    auto a = std::make_unique<Chunk>(), b = std::make_unique<Chunk>();
    int checked = 0;
    for (int cz = -11; cz <= 11; cz += 2)
        for (int cx = -13; cx <= 13; cx += 2) {
            const ColumnClass c = generator->ClassifyColumn(cx, cz);
            ASSERT_TRUE(c.known);
            ASSERT_LT(c.solidBelow, c.emptyFrom);
            for (int row = c.solidBelow - 2; row <= c.emptyFrom + 1; ++row) {
                *a = Chunk{};
                *b = Chunk{};
                const ChunkContent ca = generator->Generate(glm::ivec3(cx, row, cz), *a);
                const ChunkContent cb = full->Generate(glm::ivec3(cx, row, cz), *b);
                if (row >= c.emptyFrom || row < c.solidBelow) {
                    ASSERT_EQ(cb.kind, ChunkKind::Dense);
                    const uint8_t expected = row >= c.emptyFrom ? 0 : c.solidId;
                    for (uint8_t id : b->ids) ASSERT_EQ(id, expected) << cx << " " << row << " " << cz;
                    EXPECT_EQ(ca.kind, row >= c.emptyFrom ? ChunkKind::Empty : ChunkKind::Uniform);
                    ++checked;
                } else {
                    EXPECT_EQ(ChunkHash(*a), ChunkHash(*b));
                }
            }
        }
    EXPECT_GT(checked, 100);
}

TEST(LandSampling, WaterStandsAtTheMapsLevels) {
    const auto map = SampleMap();
    const LandGeneratorSettings s = SampleSettings(*map);
    for (uint32_t cell : {uint32_t(kLakeA), uint32_t(kLakeB)}) {
        const glm::vec2 p = CellCentre(*map, s, cell);
        EXPECT_EQ(LandWaterLevel(*map, s, p.x, p.y), kLakeLevel);
        EXPECT_LT(LandHeight(*map, s, p.x, p.y), kLakeLevel);
    }
    // The open sea, past the map's edge too.
    for (const glm::vec2 p : {glm::vec2(-45.0f, -38.0f), glm::vec2(500.0f, 0.0f)}) {
        EXPECT_EQ(LandWaterLevel(*map, s, p.x, p.y), 0.0f);
        EXPECT_LT(LandHeight(*map, s, p.x, p.y), 0.0f);
    }
    // Dry land in the middle of the island: no water, above the sea.
    const glm::vec2 dry = CellCentre(*map, s, map->Index(14, 12));
    EXPECT_EQ(LandWaterLevel(*map, s, dry.x, dry.y), kNoLandWater);
    EXPECT_GT(LandHeight(*map, s, dry.x, dry.y), 0.0f);
}

TEST(LandSampling, TheSpawnIsDryLevelGroundInTheStarterRegion) {
    const auto map = SampleMap();
    const LandGeneratorSettings s = SampleSettings(*map);
    const glm::vec2 p = LandSpawn(*map, s);
    const int x = int(std::floor((p.x - s.origin.x) / map->cellSize)), z = int(std::floor((p.y - s.origin.y) / map->cellSize));
    ASSERT_TRUE(x >= 0 && z >= 0 && x < map->width && z < map->height);
    EXPECT_EQ(map->regions[map->Index(x, z)], uint8_t(LandBiome::Forest));
    EXPECT_EQ(LandWaterLevel(*map, s, p.x, p.y), kNoLandWater);
}

TEST(LandSampling, EveryTreeVoxelJoinsTheGroundByFaces) {
    // A blast frees what no face path ties to the ground; a tree voxel that only touches its tree by an edge or a corner
    // (a thin branch, a ragged leaf rim) would stay in the air when its tree falls. Every wood and foliage voxel of the
    // trees in a block of the forest reaches the ground, or the block's edge, through faces.
    const auto map = SampleMap();
    const LandGeneratorSettings s = SampleSettings(*map);
    const auto generator = CreateLandGenerator(map, s);
    const WorldMaterials& m = s.materials;
    constexpr int kCx0 = -7, kCz0 = -3, kRow0 = -1, kNx = 6, kNz = 6, kNy = 12;
    constexpr int kX = kNx * kChunkSize, kY = kNy * kChunkSize, kZ = kNz * kChunkSize;
    std::vector<uint8_t> ids(size_t(kX) * kY * kZ, 0);
    const auto at = [&](int x, int y, int z) { return size_t(x) + size_t(kX) * (size_t(y) + size_t(kY) * size_t(z)); };
    auto chunk = std::make_unique<Chunk>();
    for (int cz = 0; cz < kNz; ++cz)
        for (int row = 0; row < kNy; ++row)
            for (int cx = 0; cx < kNx; ++cx) {
                *chunk = Chunk{};
                const ChunkContent c = generator->Generate(glm::ivec3(kCx0 + cx, kRow0 + row, kCz0 + cz), *chunk);
                for (int z = 0; z < kChunkSize; ++z)
                    for (int y = 0; y < kChunkSize; ++y)
                        for (int x = 0; x < kChunkSize; ++x)
                            ids[at(cx * kChunkSize + x, row * kChunkSize + y, cz * kChunkSize + z)] =
                                c.kind == ChunkKind::Dense ? chunk->ids[Chunk::VoxelIndex(x, y, z)] : c.uniformId;
            }
    const auto tree = [&](uint8_t v) {
        return v != 0 && (v == m.wood || v == m.barkPale || v == m.deadwood || v == m.leaves || v == m.leavesAutumn || v == m.needles);
    };
    // Seeds: every solid voxel that is not a tree's (the ground, rocks, ground plants) and every voxel on the block's edge.
    std::vector<uint8_t> reached(ids.size(), 0);
    std::vector<glm::ivec3> open;
    size_t trees = 0;
    for (int z = 0; z < kZ; ++z)
        for (int y = 0; y < kY; ++y)
            for (int x = 0; x < kX; ++x) {
                const uint8_t v = ids[at(x, y, z)];
                if (v == 0 || v == m.water) continue;
                trees += tree(v) ? 1 : 0;
                const bool edge = x == 0 || y == 0 || z == 0 || x == kX - 1 || y == kY - 1 || z == kZ - 1;
                if (!tree(v) || edge) {
                    reached[at(x, y, z)] = 1;
                    open.push_back(glm::ivec3(x, y, z));
                }
            }
    ASSERT_GT(trees, 2000u) << "the block holds trees";
    while (!open.empty()) {
        const glm::ivec3 p = open.back();
        open.pop_back();
        for (const glm::ivec3 d : {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0), glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1),
                                   glm::ivec3(0, 0, -1)}) {
            const glm::ivec3 q = p + d;
            if (q.x < 0 || q.y < 0 || q.z < 0 || q.x >= kX || q.y >= kY || q.z >= kZ) continue;
            const size_t i = at(q.x, q.y, q.z);
            if (reached[i] || ids[i] == 0 || ids[i] == m.water) continue;
            reached[i] = 1;
            open.push_back(q);
        }
    }
    size_t loose = 0;
    glm::ivec3 first(0);
    for (int z = 0; z < kZ; ++z)
        for (int y = 0; y < kY; ++y)
            for (int x = 0; x < kX; ++x)
                if (tree(ids[at(x, y, z)]) && !reached[at(x, y, z)] && loose++ == 0) first = glm::ivec3(x, y, z);
    EXPECT_EQ(loose, 0u) << "of " << trees << " tree voxels; the first at block voxel " << first.x << " " << first.y << " " << first.z;
}
