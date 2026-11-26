// Hollowkeep's fort plan and stamper (docs/design/fortress.md, [fort-1]): the plan is pinned by a golden hash; walls
// stand on battered granite foundations with ashlar faces and a rubble core; the walks step with the ground; blocks are
// hollow with floors; the column classes still bound the stamped castle.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>

import vortex.voxel;

using namespace vortex::voxel;

namespace {
    /// A plain (the starter region) with a mountain knoll on it, a ridge to the west, the sea to the south.
    LandSource FortKnoll() {
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
            }
        s.starterRegion = 2;
        return s;
    }

    struct FortWorld {
        std::shared_ptr<const LandMap> map;
        LandGeneratorSettings land;
        FortPlan plan;
        std::shared_ptr<IWorldGenerator> generator;
    };

    const FortWorld& FortSetup() {
        static const FortWorld w = [] {
            FortWorld out;
            out.map = std::make_shared<const LandMap>(BakeLand(FortKnoll()));
            MaterialLibrary library;
            out.land.materials = RegisterDefaultWorldMaterials(library);
            out.land.origin = LandWorldOrigin(*out.map);
            out.plan = PlanFort(*out.map, out.land);
            out.generator = CreateLandGenerator(out.map, out.land);
            return out;
        }();
        return w;
    }

    uint8_t VoxelAt(const IWorldGenerator& generator, float voxelSize, glm::vec3 world) {
        const glm::ivec3 v(int(std::floor(world.x / voxelSize)), int(std::floor(world.y / voxelSize)), int(std::floor(world.z / voxelSize)));
        const glm::ivec3 c(v.x >> 5, v.y >> 5, v.z >> 5);
        auto chunk = std::make_unique<Chunk>();
        generator.Generate(c, *chunk);
        return chunk->Get(v.x - c.x * kChunkSize, v.y - c.y * kChunkSize, v.z - c.z * kChunkSize);
    }

    /// The longest curtain run without a gap, or nullptr.
    const FortRecord* LongCurtainRun(const FortPlan& plan) {
        const FortRecord* best = nullptr;
        float bestLen = 0.0f;
        for (const FortRecord& r : plan.records) {
            if (r.shape != FortShape::Wall || r.half < 1.2f || r.gapHi >= r.gapLo) continue;
            const float len = glm::length(r.b - r.a);
            if (len > bestLen) {
                bestLen = len;
                best = &r;
            }
        }
        return best;
    }

    /// Hollowkeep's stamped voxels around a point: the chunk's hash (part of the golden hash).
    uint64_t StampedChunk(const FortWorld& w, glm::vec3 world) {
        const float vs = w.land.voxelSize;
        const glm::ivec3 v(int(std::floor(world.x / vs)), int(std::floor(world.y / vs)), int(std::floor(world.z / vs)));
        auto chunk = std::make_unique<Chunk>();
        w.generator->Generate(glm::ivec3(v.x >> 5, v.y >> 5, v.z >> 5), *chunk);
        return ChunkHash(*chunk);
    }
}

TEST(Fort, GoldenHash) {
    const FortWorld& w = FortSetup();
    ASSERT_TRUE(w.plan.placed);
    EXPECT_EQ(FortPlanHash(w.plan), FortPlanHash(PlanFort(*w.map, w.land)));
    // The plan's records and the stamper's voxels at the gate, a tower and the keep: a change to either changes it.
    uint64_t hash = FortPlanHash(w.plan);
    const FortRecord* tower = nullptr;
    for (const FortRecord& r : w.plan.records)
        if (r.shape == FortShape::Block && r.block == FortBlock::Tower && tower == nullptr) tower = &r;
    ASSERT_NE(tower, nullptr);
    for (const glm::vec3 at : {glm::vec3(w.plan.gatePoint.x, w.plan.gateGround + 2.0f, w.plan.gatePoint.y),
                               glm::vec3(tower->a.x, tower->lo + 4.0f, tower->a.y), glm::vec3(w.plan.keep.x, w.plan.keepGround + 2.0f, w.plan.keep.y)})
        hash = hash * 0x100000001b3ull ^ StampedChunk(w, at);
    std::printf("Fort golden hash: 0x%016llxull\n", static_cast<unsigned long long>(hash));
    EXPECT_EQ(hash, kFortGoldenHash);
}

TEST(Fort, WallsStandOnBatteredGraniteFoundationsWithAshlarFacesAndARubbleCore) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const FortRecord* longest = LongCurtainRun(w.plan);
    ASSERT_NE(longest, nullptr);
    const FortRecord& run = *longest;
    const float len = glm::length(run.b - run.a);
    ASSERT_GT(len, 6.0f);
    const glm::vec2 d = (run.b - run.a) / len;
    glm::vec2 n(d.y, -d.x);
    if (glm::dot(n, (run.a + run.b) * 0.5f - w.plan.keep) < 0.0f) n = -n;
    // Half way between the run's start and its first buttress (the stamper's spacing).
    const int buttresses = len > 10.0f ? int(std::floor(len / 7.0f)) : 0;
    const glm::vec2 p = run.a + d * (len / float(buttresses + 1) * 0.5f);
    const float g = LandHeight(*w.map, w.land, p.x, p.y);
    const float vs = w.land.voxelSize;
    const auto at = [&](float s, float y) {
        const glm::vec2 q = p + n * s;
        return VoxelAt(*w.generator, vs, glm::vec3(q.x, y, q.y));
    };
    EXPECT_EQ(at(0.0f, g - 1.0f), m.granite) << "the foundation reaches under the ground";
    const glm::vec2 foot = p + n * (run.half + 0.5f);
    EXPECT_EQ(at(run.half + 0.5f, LandHeight(*w.map, w.land, foot.x, foot.y) + 0.4f), m.granite) << "the batter flares at the foot";
    EXPECT_NE(at(run.half + 0.5f, run.hi - 3.0f), m.granite) << "the batter stops above the foot";
    EXPECT_EQ(at(run.half - 0.3f, run.hi - 3.0f), m.ashlar) << "the outer face";
    EXPECT_EQ(at(0.0f, run.hi - 3.0f), m.rubbleCore) << "the core";
    EXPECT_EQ(at(-run.half + 0.3f, run.hi - 3.0f), m.ashlar) << "the inner face";
    EXPECT_EQ(at(run.half - 0.25f, run.hi + 0.5f), m.ashlar) << "the parapet";
    EXPECT_EQ(at(-run.half + 0.5f, run.hi + 0.5f), 0) << "the wall walk is open";
}

TEST(Fort, TheWallWalkStepsWithTheGround) {
    std::set<float> walks;
    for (const FortRecord& r : FortSetup().plan.records)
        if (r.shape == FortShape::Wall) walks.insert(r.hi);
    // Each run's walk follows its own highest ground: a castle on a slope has many walk heights, not one.
    EXPECT_GT(walks.size(), 6u);
}

TEST(Fort, TheKeepIsHollowOnItsKeelWithTimberFloors) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize, g0 = w.plan.keepGround;
    const glm::vec2 c = w.plan.keep;
    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(c.x, g0 - 0.25f, c.y)), m.bedrock) << "the sanctum's keel floor";
    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(c.x, g0 + 2.0f, c.y)), 0) << "the sanctum is open";
    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(c.x, g0 + 5.1f, c.y)), m.planks) << "the first timber floor";
    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(c.x + 7.9f, g0 + 2.0f, c.y + 7.9f)), m.granite) << "the lower shell";
}

TEST(Fort, ColumnClassesBoundTheStampedCastle) {
    // Every chunk the castle crosses is the same whether or not classified rows are skipped: the column classes
    // (empty above, solid below) never cut a stamped voxel off.
    const FortWorld& w = FortSetup();
    LandGeneratorSettings full = w.land;
    full.skipClassifiedRows = false;
    const auto every = CreateLandGenerator(w.map, full);
    int castle = 0;
    auto a = std::make_unique<Chunk>(), b = std::make_unique<Chunk>();
    for (const FortRecord& r : w.plan.records) {
        if (r.shape == FortShape::Box) continue;
        int compared = 0;   // per record, so every wall, tower and the keep is compared, not only the ditch
        const glm::ivec2 lo(r.min.x >> 5, r.min.z >> 5), hi((r.max.x - 1) >> 5, (r.max.z - 1) >> 5);
        for (int cz = lo.y; cz <= hi.y; ++cz)
            for (int cx = lo.x; cx <= hi.x; ++cx) {
                const ColumnClass c = w.generator->ClassifyColumn(cx, cz);
                ASSERT_TRUE(c.known);
                for (int row = c.solidBelow - 1; row <= c.emptyFrom && compared < 40; ++row) {
                    *a = Chunk{};
                    *b = Chunk{};
                    const ChunkContent ca = w.generator->Generate(glm::ivec3(cx, row, cz), *a);
                    every->Generate(glm::ivec3(cx, row, cz), *b);
                    if (row >= c.emptyFrom || row < c.solidBelow) {
                        const uint8_t expected = row >= c.emptyFrom ? 0 : c.solidId;
                        for (uint8_t id : b->ids) ASSERT_EQ(id, expected) << cx << " " << row << " " << cz;
                        EXPECT_NE(ca.kind, ChunkKind::Dense);
                    } else {
                        EXPECT_EQ(ChunkHash(*a), ChunkHash(*b)) << cx << " " << row << " " << cz;
                    }
                    for (uint8_t id : b->ids) castle += id == w.land.materials.ashlar ? 1 : 0;
                    ++compared;
                }
            }
    }
    EXPECT_GT(castle, 10);
}

TEST(Fort, EveryRecordsBoundsHoldAllItWrites) {
    // Each shaped record alone, stamped with its bounds and with them widened by 8 m, writes the same voxels: no record
    // writes past its bounds (a bound too tight would cut a batter, a corner or a foundation off silently). A Box
    // fills its bounds by definition and is left out.
    const FortWorld& w = FortSetup();
    const float vs = w.land.voxelSize;
    auto a = std::make_unique<Chunk>(), b = std::make_unique<Chunk>();
    int compared = 0;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape == FortShape::Box) continue;
        FortPlan one = w.plan, wide = w.plan;
        one.records = {r};
        wide.records = {r};
        wide.records[0].min -= glm::ivec3(64);
        wide.records[0].max += glm::ivec3(64);
        const glm::ivec3 lo = (r.min - 32) >> 5, hi = (r.max + 32) >> 5;
        for (int cz = lo.z; cz <= hi.z; ++cz)
            for (int cx = lo.x; cx <= hi.x; ++cx) {
                float ground[kChunkSize * kChunkSize];
                for (int z = 0; z < kChunkSize; ++z)
                    for (int x = 0; x < kChunkSize; ++x)
                        ground[x + z * kChunkSize] =
                            LandHeight(*w.map, w.land, (float(cx * kChunkSize + x) + 0.5f) * vs, (float(cz * kChunkSize + z) + 0.5f) * vs) / vs;
                for (int cy = lo.y; cy <= hi.y; ++cy) {
                    *a = Chunk{};
                    *b = Chunk{};
                    StampFort(one, w.land.materials, vs, glm::ivec3(cx, cy, cz), ground, *a);
                    StampFort(wide, w.land.materials, vs, glm::ivec3(cx, cy, cz), ground, *b);
                    ASSERT_EQ(ChunkHash(*a), ChunkHash(*b)) << "chunk (" << cx << ", " << cy << ", " << cz << "), record " << (&r - w.plan.records.data()) << " of shape " << int(r.shape);
                    ++compared;
                }
            }
    }
    EXPECT_GT(compared, 500);
}
