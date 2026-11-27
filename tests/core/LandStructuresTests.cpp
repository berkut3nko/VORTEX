// The wayside structures (docs/design/land.md "Wayside structures"): the plan and its stamped voxels are pinned by a
// golden hash; a chunk's voxels do not depend on the order or the threads that make the chunks; no structure meets
// Hollowkeep, its approach, its siege spawn or the village; each stands on a foundation in dry ground; and the column
// classes still bound what they stamp.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <thread>
#include <vector>

import vortex.voxel;

using namespace vortex::voxel;

namespace {
    /// The fort tests' map: a plain (the starter region) with a mountain knoll, a ridge to the west, the sea to the south,
    /// and a rough patch in the plain's north-east (hills for the slope types).
    LandSource Knoll() {
        LandSource s;
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
                    s.biomes[i] = uint8_t(LandBiome::Sea);
                    continue;
                }
                if (x < 70) {
                    s.heights[i] = 20.0f + float(std::min(70 - x, 30)) * 3.0f;
                    s.biomes[i] = uint8_t(LandBiome::Mountain);
                    s.regions[i] = 6;
                    continue;
                }
                s.heights[i] = 3.0f + float(x - 70) * 0.02f + float(140 - z) * 0.03f;
                s.biomes[i] = uint8_t(LandBiome::Grassland);
                s.regions[i] = 2;
                const float dx = float(x - 110), dz = float(z - 60);
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d < 18.0f) {
                    s.heights[i] += 32.0f * (1.0f - d / 18.0f) - (dz > 0.0f ? dz * 0.2f : 0.0f);
                    s.biomes[i] = uint8_t(LandBiome::Mountain);
                    s.regions[i] = 6;
                }
                const float hx = float(x - 150), hz = float(z - 40);
                const float hd = std::sqrt(hx * hx + hz * hz);
                if (hd < 14.0f) s.heights[i] += 12.0f * (1.0f - hd / 14.0f);
            }
        s.starterRegion = 2;
        return s;
    }

    struct StructureWorld {
        std::shared_ptr<const LandMap> map;
        LandGeneratorSettings land;
        LandStructures plan;
        FortPlan fort;
        std::shared_ptr<IWorldGenerator> generator;
    };

    const StructureWorld& StructureSetup() {
        static const StructureWorld w = [] {
            StructureWorld out;
            out.map = std::make_shared<const LandMap>(BakeLand(Knoll()));
            MaterialLibrary library;
            out.land.materials = RegisterDefaultWorldMaterials(library);
            out.land.origin = LandWorldOrigin(*out.map);
            out.plan = PlanLandStructures(*out.map, out.land);
            out.fort = PlanFort(*out.map, out.land);
            out.generator = CreateLandGenerator(out.map, out.land);
            return out;
        }();
        return w;
    }

    /// Every chunk a site's records reach.
    std::vector<glm::ivec3> ChunksOf(const LandStructureSite& site) {
        std::vector<glm::ivec3> out;
        const auto chunk = [](int v) { return v >> 5; };
        for (int z = chunk(site.min.z); z <= chunk(site.max.z - 1); ++z)
            for (int y = chunk(site.min.y); y <= chunk(site.max.y - 1); ++y)
                for (int x = chunk(site.min.x); x <= chunk(site.max.x - 1); ++x) out.emplace_back(x, y, z);
        return out;
    }

    uint64_t HashOf(const IWorldGenerator& generator, const glm::ivec3& c) {
        auto chunk = std::make_unique<Chunk>();
        const ChunkContent content = generator.Generate(c, *chunk);
        if (content.kind != ChunkKind::Dense) return 0x100ull | content.uniformId;
        return ChunkHash(*chunk);
    }
}

TEST(LandStructures, GoldenHash) {
    const StructureWorld& w = StructureSetup();
    ASSERT_GE(w.plan.sites.size(), 4u);
    EXPECT_EQ(LandStructuresHash(w.plan), LandStructuresHash(PlanLandStructures(*w.map, w.land)));
    // The plan and the stamped voxels in the middle of the first 4 sites: a change to either changes it.
    uint64_t hash = LandStructuresHash(w.plan);
    const float vs = w.land.voxelSize;
    for (size_t n = 0; n < 4; ++n) {
        const LandStructureSite& s = w.plan.sites[n];
        const glm::ivec3 v(int(std::floor(s.at.x / vs)), int(std::floor((s.ground + 1.0f) / vs)), int(std::floor(s.at.y / vs)));
        hash = hash * 0x100000001b3ull ^ HashOf(*w.generator, glm::ivec3(v.x >> 5, v.y >> 5, v.z >> 5));
    }
    std::printf("LandStructures golden hash: 0x%016llxull\n", static_cast<unsigned long long>(hash));
    EXPECT_EQ(hash, kLandStructuresGoldenHash);
}

TEST(LandStructures, ChunksAreTheSameInAnyOrderAndOnAnyThread) {
    const StructureWorld& w = StructureSetup();
    std::vector<glm::ivec3> chunks;
    for (size_t n = 0; n < std::min<size_t>(3, w.plan.sites.size()); ++n)
        for (const glm::ivec3& c : ChunksOf(w.plan.sites[n])) chunks.push_back(c);
    ASSERT_FALSE(chunks.empty());
    std::vector<uint64_t> forward(chunks.size()), backward(chunks.size()), threaded(chunks.size());
    {
        const auto generator = CreateLandGenerator(w.map, w.land);
        for (size_t k = 0; k < chunks.size(); ++k) forward[k] = HashOf(*generator, chunks[k]);
    }
    {
        const auto generator = CreateLandGenerator(w.map, w.land);
        for (size_t k = chunks.size(); k-- > 0;) backward[k] = HashOf(*generator, chunks[k]);
    }
    {
        // Four threads share one generator (its tile cache), each taking every fourth chunk.
        const auto generator = CreateLandGenerator(w.map, w.land);
        std::vector<std::thread> threads;
        for (size_t t = 0; t < 4; ++t)
            threads.emplace_back([&, t] {
                for (size_t k = t; k < chunks.size(); k += 4) threaded[k] = HashOf(*generator, chunks[k]);
            });
        for (std::thread& t : threads) t.join();
    }
    EXPECT_EQ(forward, backward);
    EXPECT_EQ(forward, threaded);
}

TEST(LandStructures, NoneMeetsHollowkeepItsApproachItsSiegeSpawnOrTheVillage) {
    const StructureWorld& w = StructureSetup();
    ASSERT_TRUE(w.fort.placed);
    const int margin = int(std::ceil(4.0f / w.land.voxelSize));
    // The siege's attacking spawn is the template's arrival on the castle road past the bridge (sim::HollowkeepTemplate),
    // with its 6 m spawn area; the defenders spawn in the sanctum, inside the keep's records.
    const glm::vec2 arrive = w.fort.gatePoint + w.fort.axis * (w.fort.bridgeSpan + 2.0f);
    glm::vec2 village(0.0f);
    for (const LandPoint& p : w.map->points)
        if (p.kind == LandPointKind::Village) village = w.land.origin + glm::vec2(p.x, p.z);
    for (const LandStructureSite& s : w.plan.sites) {
        for (const FortRecord& r : w.fort.records) {
            const bool apart = s.max.x + margin <= r.min.x || s.min.x - margin >= r.max.x || s.max.z + margin <= r.min.z ||
                               s.min.z - margin >= r.max.z;
            ASSERT_TRUE(apart) << LandStructureName(s.kind) << " at " << s.at.x << ", " << s.at.y;
        }
        EXPECT_GT(glm::length(s.at - arrive), s.radius + 6.0f) << LandStructureName(s.kind);
        EXPECT_GT(glm::length(s.at - glm::vec2(w.fort.keep)), s.radius + 30.0f) << LandStructureName(s.kind);
        EXPECT_GT(glm::length(s.at - village), 50.0f) << LandStructureName(s.kind);
    }
}

TEST(LandStructures, EachStandsOnAFoundationInDryGroundApartFromTheOthers) {
    const StructureWorld& w = StructureSetup();
    const float vs = w.land.voxelSize;
    std::set<LandStructureKind> kinds;
    for (size_t n = 0; n < w.plan.sites.size(); ++n) {
        const LandStructureSite& s = w.plan.sites[n];
        kinds.insert(s.kind);
        EXPECT_LE(s.min.y, int(std::floor((s.ground - 1.0f) / vs))) << LandStructureName(s.kind) << ": the foundation reaches under the ground";
        EXPECT_EQ(LandWaterLevel(*w.map, w.land, s.at.x, s.at.y), kNoLandWater) << LandStructureName(s.kind);
        EXPECT_GT(s.ground, 1.5f) << LandStructureName(s.kind) << ": above the shore";
        EXPECT_FALSE(s.plan.records.empty());
        for (size_t m = n + 1; m < w.plan.sites.size(); ++m) EXPECT_GE(glm::length(s.at - w.plan.sites[m].at), 45.0f);
    }
    EXPECT_GE(kinds.size(), 4u) << "the types vary";
}

TEST(LandStructures, ColumnClassesBoundWhatTheyStamp) {
    const StructureWorld& w = StructureSetup();
    LandGeneratorSettings s = w.land;
    s.skipClassifiedRows = false;
    const auto full = CreateLandGenerator(w.map, s);
    auto a = std::make_unique<Chunk>(), b = std::make_unique<Chunk>();
    int checked = 0;
    for (size_t n = 0; n < std::min<size_t>(4, w.plan.sites.size()); ++n) {
        std::set<std::pair<int, int>> columns;
        for (const glm::ivec3& c : ChunksOf(w.plan.sites[n])) columns.emplace(c.x, c.z);
        for (const auto& [cx, cz] : columns) {
            const ColumnClass c = w.generator->ClassifyColumn(cx, cz);
            for (int row = c.solidBelow - 1; row <= c.emptyFrom; ++row) {
                *a = Chunk{};
                *b = Chunk{};
                const ChunkContent ca = w.generator->Generate(glm::ivec3(cx, row, cz), *a);
                full->Generate(glm::ivec3(cx, row, cz), *b);
                if (row < c.solidBelow || row >= c.emptyFrom) {
                    EXPECT_EQ(ca.kind, row < c.solidBelow ? ChunkKind::Uniform : ChunkKind::Empty);
                    const uint8_t expected = row < c.solidBelow ? c.solidId : 0;
                    for (uint8_t id : b->ids) ASSERT_EQ(id, expected) << cx << " " << row << " " << cz;
                } else {
                    ASSERT_EQ(ChunkHash(*a), ChunkHash(*b)) << cx << " " << row << " " << cz;
                }
                ++checked;
            }
        }
    }
    EXPECT_GT(checked, 20);
}
