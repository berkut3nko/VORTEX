// Hollowkeep's fort plan and stamper (docs/design/fortress.md, [fort-1]): the plan is pinned by a golden hash; walls
// stand on battered granite foundations with ashlar faces and a rubble core; the walks step with the ground; blocks are
// hollow with floors; the column classes still bound the stamped castle.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <tuple>
#include <set>
#include <vector>

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
        const ChunkContent content = generator.Generate(c, *chunk);
        if (content.kind != ChunkKind::Dense) return content.uniformId;   // an empty or uniform chunk leaves its ids unset
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
    const float along = len / float(buttresses + 1) * 0.5f;
    const glm::vec2 p = run.a + d * along;
    const float walk = FortWalk(run, along);
    const float g = LandHeight(*w.map, w.land, p.x, p.y);
    const float vs = w.land.voxelSize;
    const auto at = [&](float s, float y) {
        const glm::vec2 q = p + n * s;
        return VoxelAt(*w.generator, vs, glm::vec3(q.x, y, q.y));
    };
    EXPECT_EQ(at(0.0f, g - 1.0f), m.granite) << "the foundation reaches under the ground";
    const glm::vec2 foot = p + n * (run.half + 0.5f);
    {
        const uint8_t flare = at(run.half + 0.5f, LandHeight(*w.map, w.land, foot.x, foot.y) + 0.4f);
        EXPECT_TRUE(flare == m.granite || flare == m.graniteDamp) << "the batter flares at the foot (damp on a shaded face)";
    }
    EXPECT_NE(at(run.half + 0.5f, walk - 3.0f), m.granite) << "the batter stops above the foot";
    EXPECT_EQ(at(run.half - 0.3f, walk - 3.0f), m.ashlar) << "the outer face";
    EXPECT_EQ(at(0.0f, walk - 3.0f), m.rubbleCore) << "the core";
    EXPECT_EQ(at(-run.half + 0.3f, walk - 3.0f), m.ashlar) << "the inner face";
    EXPECT_EQ(at(run.half - 0.25f, walk + 0.5f), m.ashlar) << "the parapet";
    EXPECT_EQ(at(-run.half + 0.5f, walk + 0.5f), 0) << "the wall walk is open";
}

TEST(Fort, TheWallWalkStepsWithTheGround) {
    std::set<float> walks;
    std::vector<const FortRecord*> curtain;
    for (const FortRecord& r : FortSetup().plan.records)
        if (r.shape == FortShape::Wall) {
            walks.insert(r.hi);
            if (r.half > 1.2f) curtain.push_back(&r);
        }
    // Each run's walk follows its own highest ground: a castle on a slope has many walk heights, not one.
    EXPECT_GT(walks.size(), 6u);
    // And it is continuous: each run ends at the height the next one starts, so the walk has no step.
    ASSERT_EQ(curtain.size(), size_t(kSiteDirections));
    for (size_t k = 0; k < curtain.size(); ++k) EXPECT_EQ(curtain[k]->hi, curtain[(k + 1) % curtain.size()]->lo) << "run " << k;
    // No ramp of any walk climbs more than 1 in 1 (a short run beside a tower would otherwise make a wall of a step).
    for (const FortRecord& r : FortSetup().plan.records) {
        if (r.shape != FortShape::Wall) continue;
        const float ramp = std::max(0.0f, glm::length(r.b - r.a) - r.flatA - r.flatB);
        EXPECT_LE(std::abs(r.hi - r.lo), ramp + 1.0e-3f);
    }
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

TEST(Fort, TowersHaveSplayedSlitsAndASpiralStairToTheRoof) {
    const FortWorld& w = FortSetup();
    const float vs = w.land.voxelSize;
    const FortRecord* tower = nullptr;
    for (const FortRecord& r : w.plan.records)
        if (r.shape == FortShape::Block && r.block == FortBlock::Tower && tower == nullptr) tower = &r;
    ASSERT_NE(tower, nullptr);
    const glm::vec2 c = tower->a, o = tower->b, side(-o.y, o.x);
    const auto at = [&](glm::vec2 q, float y) {
        const glm::vec2 p = c + o * q.x + side * q.y;
        return VoxelAt(*w.generator, vs, glm::vec3(p.x, y, p.y));
    };
    // The second storey's front slit, 1.5 m over its floor.
    const float y = tower->lo + 3.25f + 1.5f;
    EXPECT_EQ(at(glm::vec2(tower->half - 0.06f, 0.0f), y), 0) << "the slit opens at the outer face";
    EXPECT_NE(at(glm::vec2(tower->half - 0.06f, 0.3f), y), 0) << "it is narrow outside";
    EXPECT_EQ(at(glm::vec2(tower->half - 1.44f, 0.4f), y), 0) << "it splays to 1 m inside";
    // Walk the stair: every step stands under 1.875 m of headroom, up to the roof.
    int steps = 0;
    for (int i = 0;; ++i) {
        const int s = i % 12;
        const float top = float(i / 12) * 3.0f + float(s + 1) * 0.25f;
        if (top > tower->hi - tower->lo) break;
        const float angle = (30.0f * float(s) + 15.0f) * 3.14159265f / 180.0f;
        const glm::vec2 q(0.6f * std::cos(angle), 0.6f * std::sin(angle));
        EXPECT_NE(at(q, tower->lo + top - 0.125f), 0) << "step " << i;
        for (float h = 0.125f; h < 1.875f; h += 0.25f) EXPECT_EQ(at(q, tower->lo + top + h), 0) << "headroom over step " << i;
        ++steps;
    }
    EXPECT_GE(steps, 50);
}

TEST(Fort, EveryFlankTowerOpensOntoBothWallWalks) {
    // From each walk, through the door on its centre line, there is 1.875 m of headroom just inside the tower.
    const FortWorld& w = FortSetup();
    const float vs = w.land.voxelSize;
    int doors = 0;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape != FortShape::Block || r.block != FortBlock::Tower) continue;
        const glm::vec2 side(-r.b.y, r.b.x);
        for (const int sign : {1, -1}) {
            const float walk = sign > 0 ? r.walkLeft : r.walkRight, u = sign > 0 ? r.walkLeftU : r.walkRightU;
            if (walk < -1.0e29f) continue;
            for (const float depth : {r.half + 0.2f, r.half - 1.0f, r.half - 2.25f}) {
                const glm::vec2 p = r.a + r.b * u + side * (float(sign) * depth);
                EXPECT_NE(VoxelAt(*w.generator, vs, glm::vec3(p.x, walk - 0.125f, p.y)), 0) << "the floor under the door, depth " << depth;
                for (float h = 0.125f; h < 1.875f; h += 0.25f)
                    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(p.x, walk + h, p.y)), 0) << "headroom at " << h << " m, depth " << depth << ", tower at (" << r.a.x << ", " << r.a.y << "), side " << sign;
            }
            ++doors;
        }
    }
    EXPECT_GE(doors, 8);
}

TEST(Fort, TheCulvertLeadsFromItsGrateToTheBailey) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize;
    ASSERT_TRUE(w.plan.culvert);
    std::vector<const FortRecord*> tunnels, blocks;
    const FortRecord* spiral = nullptr;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape == FortShape::Tunnel) tunnels.push_back(&r);
        if (r.shape == FortShape::Block) blocks.push_back(&r);
        if (r.shape == FortShape::Spiral) spiral = &r;
    }
    ASSERT_GE(tunnels.size(), 2u);
    const auto at = [&](const FortRecord& r, float t, float s, float overFloor) {
        const float len = glm::length(r.b - r.a);
        const glm::vec2 d = (r.b - r.a) / len, n(-d.y, d.x);
        const glm::vec2 p = r.a + d * t + n * s;
        return VoxelAt(*w.generator, vs, glm::vec3(p.x, r.lo + (r.hi - r.lo) * t / len + overFloor, p.y));
    };
    const FortRecord& crawl = *tunnels[0];
    EXPECT_EQ(at(crawl, -0.5f, 0.0f, 0.5f), 0) << "open air in front of the grate";
    EXPECT_EQ(at(crawl, 0.1f, 0.0625f, 0.5f), m.iron) << "a bar of the grate";
    EXPECT_EQ(at(crawl, 0.1f, 0.1875f, 0.5f), 0) << "a gap of the grate";
    EXPECT_EQ(at(crawl, 0.1f, 0.0f, -0.125f), m.granite) << "the grate's frame";
    for (float t = 1.0f; t < crawl.bulkheadAt; t += 0.5f) EXPECT_EQ(at(crawl, t, 0.0f, 0.5f), 0) << "the crawl at " << t << " m";
    EXPECT_EQ(at(crawl, crawl.bulkheadAt + 0.125f, 0.0f, 0.5f), m.oakIron) << "the bulkhead";
    const FortRecord& room = *tunnels[1];
    EXPECT_EQ(at(room, 0.1f, 0.0f, 0.5f), 0) << "the doorway from the crawl";
    EXPECT_EQ(at(room, 0.1f, 1.5f, 0.5f), m.ashlar) << "the room's wall beside it";
    EXPECT_EQ(at(room, 2.9f, 0.0f, 1.25f), m.ashlar) << "the room's far wall";
    // The spiral climbs from the room's floor to the bailey's ground (on a low site to the room's roof, a drop of
    // at most 2.75 m: under the 4 m where a fall starts to hurt).
    ASSERT_NE(spiral, nullptr);
    ASSERT_EQ(tunnels.size(), 2u);
    int steps = 0;
    for (int i = 0;; ++i) {
        const float top = float(i + 1) * 0.25f;
        if (spiral->lo + top > spiral->hi) break;
        const float angle = (30.0f * float(i % 12) + 15.0f) * 3.14159265f / 180.0f;
        const glm::vec2 q = spiral->a + glm::vec2(0.6f * std::cos(angle), 0.6f * std::sin(angle));
        EXPECT_NE(VoxelAt(*w.generator, vs, glm::vec3(q.x, spiral->lo + top - 0.125f, q.y)), 0) << "step " << i;
        EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(q.x, spiral->lo + top + 0.875f, q.y)), 0) << "headroom over step " << i;
        ++steps;
    }
    EXPECT_GE(steps, 8);
    const glm::vec2 beside = spiral->a + glm::vec2(1.75f, 0.0f);
    const float g = LandHeight(*w.map, w.land, beside.x, beside.y);
    EXPECT_GE(spiral->hi, g - 0.5f) << "the top step reaches the bailey's ground";
    EXPECT_LE(spiral->hi - g, 3.0f) << "a short drop at most";
    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(spiral->a.x + 0.6f, spiral->hi + 1.0f, spiral->a.y)), 0) << "open above the top";
    // The culvert keeps clear of every tower and the keep.
    for (const FortRecord* t : tunnels)
        for (const FortRecord* b : blocks)
            EXPECT_TRUE(t->max.x <= b->min.x || t->min.x >= b->max.x || t->max.z <= b->min.z || t->min.z >= b->max.z) << "a tunnel meets a block";
}

TEST(Fort, TheGatePassageIsVaultedWithMurderHoles) {
    const FortWorld& w = FortSetup();
    const float vs = w.land.voxelSize, g = w.plan.gateGround;
    const auto at = [&](float along, float lateral, float y) {
        const glm::vec2 p = w.plan.gatePoint + w.plan.axis * along + w.plan.across * lateral;
        return VoxelAt(*w.generator, vs, glm::vec3(p.x, g + y, p.y));
    };
    EXPECT_EQ(at(-2.0f, 0.0f, 4.0f), 0) << "under the vault's crown";
    EXPECT_NE(at(-2.0f, 1.9f, 4.0f), 0) << "the vault's haunch";
    EXPECT_EQ(at(-1.0f, 0.0f, 4.75f), 0) << "a murder hole through the upper room's floor";
    EXPECT_EQ(at(-1.75f, 0.0f, 4.75f), w.land.materials.planks) << "the floor between the holes";
    for (const float lateral : {-1.9f, 1.9f}) EXPECT_EQ(at(2.5f, lateral, 4.2f), 0) << "the portcullis's groove is 4 m wide";
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
                    for (uint8_t& id : a->ids) id = w.land.materials.dirt;   // a solid start: writes of air count too
                    for (uint8_t& id : b->ids) id = w.land.materials.dirt;
                    StampFort(one, w.land.materials, vs, glm::ivec3(cx, cy, cz), ground, *a);
                    StampFort(wide, w.land.materials, vs, glm::ivec3(cx, cy, cz), ground, *b);
                    ASSERT_EQ(ChunkHash(*a), ChunkHash(*b)) << "chunk (" << cx << ", " << cy << ", " << cz << "), record " << (&r - w.plan.records.data()) << " of shape " << int(r.shape);
                    ++compared;
                }
            }
    }
    EXPECT_GT(compared, 500);
}




TEST(Fort, RoofShellsAreThickEnoughForTheirPitch) {
    // worldgen.md C5: a stepped shell's vertical thickness is at least its rise per voxel of run plus one voxel, so each
    // step overlaps the next and the shell holds together.
    const FortWorld& w = FortSetup();
    const float vs = w.land.voxelSize;
    int roofs = 0;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape != FortShape::Roof) continue;
        const glm::vec2 side(-r.b.y, r.b.x);
        const glm::vec2 q = r.roof == FortRoof::Gable ? r.a + r.b * (r.length * 0.5f) + side * (r.half * 0.5f) : r.a + side * (r.half * 0.5f);
        const float d = r.roof == FortRoof::Gable ? r.half * 0.5f : r.half * 0.5f;
        const float surface = r.lo + r.height * (r.half - d);
        // From above the surface, find the first cover voxel of the column, then count the cover under it.
        float y = surface + 0.5f;
        while (y > surface - 1.0f && VoxelAt(*w.generator, vs, glm::vec3(q.x, y, q.y)) != r.id) y -= vs;
        int thick = 0;
        for (; y > surface - 2.0f && VoxelAt(*w.generator, vs, glm::vec3(q.x, y, q.y)) == r.id; y -= vs) ++thick;
        EXPECT_GE(thick, int(std::ceil(r.height)) + 1) << "the roof at (" << r.a.x << ", " << r.a.y << ")";
        ++roofs;
    }
    EXPECT_GE(roofs, 10);
}

TEST(Fort, TowersCarryRoofsOnTrussesOverAnOpenTopFloor) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize;
    const FortRecord* tower = nullptr;
    const FortRecord* roof = nullptr;
    for (size_t i = 0; i + 1 < w.plan.records.size() && tower == nullptr; ++i)
        if (w.plan.records[i].shape == FortShape::Block && w.plan.records[i].block == FortBlock::Tower) {
            tower = &w.plan.records[i];
            roof = &w.plan.records[i + 1];
        }
    ASSERT_NE(tower, nullptr);
    ASSERT_EQ(roof->shape, FortShape::Roof);
    const glm::vec2 o = tower->b, side(-o.y, o.x);
    const auto at = [&](float u, float v, float y) {
        const glm::vec2 p = tower->a + o * u + side * v;
        return VoxelAt(*w.generator, vs, glm::vec3(p.x, y, p.y));
    };
    EXPECT_EQ(at(0.0f, 1.0f, roof->lo + roof->height * (roof->half - 1.0f) - 0.2f), m.slate) << "the slate cone";
    EXPECT_EQ(at(1.0f, 0.0f, roof->lo + 0.125f), m.wood) << "a tie beam";
    for (float h = 0.125f; h < 1.8f; h += 0.25f) EXPECT_EQ(at(-1.5f, 1.5f, tower->hi + h), 0) << "headroom on the top floor at " << h << " m";
}

TEST(Fort, MachicolationsHaveDropHolesBetweenTheirCorbels) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize;
    const FortRecord* keep = nullptr;
    for (const FortRecord& r : w.plan.records)
        if (r.shape == FortShape::Block && r.block == FortBlock::Keep) keep = &r;
    ASSERT_NE(keep, nullptr);
    const glm::vec2 o = keep->b, side(-o.y, o.x);
    const auto at = [&](float u, float v, float y) {
        const glm::vec2 p = keep->a + o * u + side * v;
        return VoxelAt(*w.generator, vs, glm::vec3(p.x, y, p.y));
    };
    // On the face along +side, a corbel at u = -0.125 and a drop hole at u = 0.625 (corbels 0.75 m wide every 1.5 m).
    EXPECT_EQ(at(-0.125f, keep->half + 0.125f, keep->hi - 0.25f), m.ashlar) << "a corbel";
    for (float y = keep->hi - 1.5f; y < keep->hi; y += 0.25f) EXPECT_EQ(at(0.625f, keep->half + 0.125f, y), 0) << "the drop hole at " << y;
    EXPECT_EQ(at(0.625f, keep->half + 0.5f, keep->hi + 0.5f), m.ashlar) << "the parapet out on the corbels";
}

TEST(Fort, HoardingsHangOnTheCurtainByTheGate) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize;
    // The hoarded run with the longest stretch clear of the gatehouse's gap.
    const FortRecord* run = nullptr;
    float freeFrom = 0.0f, freeTo = 0.0f;
    int hoarded = 0;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape != FortShape::Wall || (r.trim & FortTrim_Hoarding) == 0) continue;
        ++hoarded;
        const float l = glm::length(r.b - r.a);
        float a0 = 0.0f, a1 = l;
        if (r.gapHi >= r.gapLo) {
            if (r.gapLo - 0.0f > l - r.gapHi) a1 = std::min(l, r.gapLo);
            else a0 = std::max(0.0f, r.gapHi);
        }
        if (a1 - a0 > freeTo - freeFrom) {
            run = &r;
            freeFrom = a0;
            freeTo = a1;
        }
    }
    EXPECT_EQ(hoarded, 4);
    ASSERT_NE(run, nullptr);
    ASSERT_GT(freeTo - freeFrom, 3.0f);
    const float len = glm::length(run->b - run->a);
    const glm::vec2 d = (run->b - run->a) / len;
    glm::vec2 n(d.y, -d.x);
    if (glm::dot(n, (run->a + run->b) * 0.5f - w.plan.keep) < 0.0f) n = -n;
    const auto at = [&](float t, float s, float overWalk) {
        const glm::vec2 p = run->a + d * t + n * s;
        return VoxelAt(*w.generator, vs, glm::vec3(p.x, FortWalk(*run, t) + overWalk, p.y));
    };
    // A point along the run where the 2 m pattern is at 0.7 m (floor, no slot, no slit), and one at 1.1 m (a beam).
    const float mid = (freeFrom + freeTo) * 0.5f;
    const float floorT = mid - (mid + 50.0f - 2.0f * std::floor((mid + 50.0f) / 2.0f)) + 0.7f;
    const float beamT = floorT + 0.4f;
    EXPECT_EQ(at(floorT, run->half + 0.75f, -0.125f), m.planks) << "the gallery's floor";
    EXPECT_EQ(at(floorT, run->half + 0.75f, 1.0f), 0) << "inside the gallery";
    EXPECT_EQ(at(floorT, run->half + 1.375f, 1.0f), m.planks) << "its outer wall";
    EXPECT_EQ(at(floorT, run->half + 0.75f, 2.6f - 0.75f * 0.6f - 0.125f), m.planks) << "its pent roof";
    EXPECT_EQ(at(beamT, run->half + 0.5f, -0.375f), m.wood) << "a putlog beam";
}

TEST(Fort, TheGatehouseHasAGableRoofAndABell) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize, t0 = w.plan.gateGround + 14.0f;
    const glm::vec2 ridge = glm::abs(w.plan.across);
    const auto at = [&](float along, float y) {
        const glm::vec2 p = w.plan.gatePoint + ridge * along;
        return VoxelAt(*w.generator, vs, glm::vec3(p.x, y, p.y));
    };
    EXPECT_EQ(at(3.0f, t0 + 3.0f - 0.0625f), m.roofTile) << "the ridge";
    EXPECT_EQ(at(5.875f, t0 + 1.0f), m.ashlar) << "a stone gable end";
    EXPECT_EQ(at(0.0f, t0 + 4.25f), m.iron) << "the bell";
}

TEST(Fort, LeanTosStandAgainstTheCurtainOnTimberFrames) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize;
    int buildings = 0;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape != FortShape::LeanTo) continue;
        ++buildings;
        const float len = glm::length(r.b - r.a);
        const glm::vec2 d = (r.b - r.a) / len;
        const float floorTop = r.lo + (r.building == FortBuilding::Granary ? 0.75f : 0.0f);
        const auto at = [&](float t, float across, float overFloor) {
            const glm::vec2 p = r.a + d * t + r.out * across;
            return VoxelAt(*w.generator, vs, glm::vec3(p.x, floorTop + overFloor, p.y));
        };
        const int doorBay = int(std::floor(len / 4.0f));
        EXPECT_EQ(at(2.0f + 0.1f, r.half - 0.125f, 1.0f), m.wood) << "a post of the front wall";
        int infill = 0;
        for (float t = 0.0f; t < len; t += 0.125f) infill += at(t, r.half - 0.125f, 0.5f) == m.wattleDaub ? 1 : 0;
        EXPECT_GT(infill, 4) << "wattle-and-daub between the frame's timbers";
        EXPECT_EQ(at(2.0f * float(doorBay) + 1.0f, r.half - 0.125f, 1.0f), 0) << "the door";
        bool roofed = false;
        for (float y = r.hi - floorTop + 0.25f; y > 0.0f && !roofed; y -= 0.125f) roofed = at(len * 0.5f, r.half * 0.5f, y) == r.id;
        EXPECT_TRUE(roofed) << "the roof over the middle";
    }
    EXPECT_GE(buildings, 3);   // the workshop first (the mid route), then as the runs clear of the towers allow
}

TEST(Fort, TheWorkshopsRoofReachesUnderTheWallWalk) {
    // The mid route M1: from the workshop's roof against the curtain, 1 m up onto the walk.
    const FortWorld& w = FortSetup();
    const float vs = w.land.voxelSize;
    const FortRecord* workshop = nullptr;
    for (const FortRecord& r : w.plan.records)
        if (r.shape == FortShape::LeanTo && r.building == FortBuilding::Workshop) workshop = &r;
    ASSERT_NE(workshop, nullptr);
    const glm::vec2 d = glm::normalize(workshop->b - workshop->a);
    const glm::vec2 p = workshop->a + d * 1.5f + workshop->out * 0.25f;
    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(p.x, workshop->hi - 0.25f, p.y)), workshop->id) << "the roof's top edge";
    // The walk over it: the wall's top 1-1.25 m above the roof's edge, nothing on the walk's inner side.
    const glm::vec2 q = p - workshop->out * 1.5f;   // on the walk, inside the parapet
    float top = workshop->hi;
    while (top < workshop->hi + 3.0f && VoxelAt(*w.generator, vs, glm::vec3(q.x, top + 0.0625f, q.y)) != 0) top += vs;
    EXPECT_LE(top - workshop->hi, 1.25f) << "the walk is a jump up from the roof";
}

TEST(Fort, ScaffoldsClimbFromTheGroundToTheirTopDeck) {
    // A player (1.8 m) walks every deck and climbs every flight with headroom; the keep's and the tower's doors open
    // under the lowest deck's ledgers.
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize;
    int scaffolds = 0;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape != FortShape::Scaffold) continue;
        ++scaffolds;
        const float len = glm::length(r.b - r.a);
        const glm::vec2 d = (r.b - r.a) / len;
        const auto at = [&](float t, float s, float y) {
            const glm::vec2 p = r.a + d * t + r.out * s;
            return VoxelAt(*w.generator, vs, glm::vec3(p.x, y, p.y));
        };
        const auto clear = [&](float t, float s, float floor) {
            for (float y = floor + 0.0625f; y < floor + 1.85f; y += vs)
                if (at(t, s, y) != 0) return false;
            return true;
        };
        const int lifts = int(std::lround((r.hi - r.lo) / 2.5f));
        ASSERT_GE(lifts, 1);
        for (int j = 0; j <= lifts; ++j) {
            const float deck = r.lo + 2.5f * float(j);
            // The lane beside the flights, along the whole deck.
            for (float t = 0.5f; t <= len - 0.5f; t += 0.5f) {
                EXPECT_EQ(at(t, 3.0f, deck + 0.125f), m.planks) << "site " << int(r.id) << " deck " << j << " at " << t;
                EXPECT_TRUE(clear(t, 3.0f, deck + 0.25f)) << "site " << int(r.id) << " headroom on deck " << j << " at " << t;
            }
            if (j == lifts) break;
            // The flight from this deck to the next: every step, with headroom over it.
            for (int k = 0; k < 10; ++k) {
                const float run = 0.125f + 0.25f * float(k), t = j % 2 == 0 ? 0.5f + run : len - 0.5f - run;
                const float step = deck + 0.25f * float(k + 1);
                EXPECT_EQ(at(t, 2.125f, step - 0.0625f), m.planks) << "site " << int(r.id) << " flight " << j << " step " << k;
                EXPECT_TRUE(clear(t, 2.125f, step)) << "site " << int(r.id) << " headroom on flight " << j << " step " << k;
            }
        }
        if (r.height > 0.0f) {
            // The flight from the ground at b's end: solid at its foot, open over its top steps.
            const float base = r.lo - r.height;
            EXPECT_NE(at(len - 0.5f - 0.125f, 2.125f, base + 0.125f), 0) << "the ground flight's foot";
            for (int k = int(std::lround(r.height / 0.25f)) - 4; k < int(std::lround(r.height / 0.25f)); ++k) {
                if (k < 0) continue;
                const float run = 0.125f + 0.25f * float(k), step = base + 0.25f * float(k + 1);
                EXPECT_TRUE(clear(len - 0.5f - run, 2.125f, step)) << "site " << int(r.id) << " headroom on the ground flight step " << k;
            }
        } else if (r.height == 0.0f) {
            EXPECT_NE(at(len * 0.5f, 3.0f, r.lo - 0.0625f), 0) << "site " << int(r.id) << ": the lowest deck stands on the ground";
            // A door in the middle of the face: out from it across the scaffold's foot.
            for (const float s : {0.5f, 1.0f, 2.125f, 3.0f})
                EXPECT_TRUE(clear(len * 0.5f + (s > 2.0f ? 0.5f : 0.0f), s, r.lo + 0.25f)) << "site " << int(r.id) << " out of the door at " << s;
        }
    }
    EXPECT_GE(scaffolds, 2);   // the keep's, the unfinished tower's and H1's where they fit
}

TEST(Fort, TheRepairPatchOpensTheParapetOverItsScaffold) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize;
    const FortRecord* run = nullptr;
    for (const FortRecord& r : w.plan.records)
        if (r.shape == FortShape::Wall && r.patchHi > r.patchLo) run = &r;
    ASSERT_NE(run, nullptr);
    const float len = glm::length(run->b - run->a), t = (run->patchLo + run->patchHi) * 0.5f;
    const glm::vec2 d = (run->b - run->a) / len;
    glm::vec2 n(d.y, -d.x);
    if (glm::dot(n, (run->a + run->b) * 0.5f - w.plan.keep) < 0.0f) n = -n;
    const glm::vec2 face = run->a + d * t + n * (run->half - 0.25f);
    const float walk = FortWalk(*run, t);
    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(face.x, walk - 1.0f, face.y)), m.brick) << "the brick patch";
    EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(face.x, walk + 0.5f, face.y)), 0) << "no parapet over it";
}

TEST(Fort, TheUnfinishedTowerHasARaggedTopAndNoRoof) {
    const FortWorld& w = FortSetup();
    const float vs = w.land.voxelSize;
    for (size_t i = 0; i < w.plan.records.size(); ++i) {
        const FortRecord& r = w.plan.records[i];
        if (r.shape != FortShape::Block || (r.trim & FortTrim_Unfinished) == 0) continue;
        ASSERT_TRUE(i + 1 == w.plan.records.size() || w.plan.records[i + 1].shape != FortShape::Roof) << "no roof";
        EXPECT_EQ(VoxelAt(*w.generator, vs, glm::vec3(r.a.x - r.b.x * 3.0f, r.hi + 0.5f, r.a.y - r.b.y * 3.0f)), 0) << "no crown";
        int built = 0, open = 0;
        for (float v = -3.0f; v <= 3.0f; v += 0.5f) {
            const glm::vec2 p = r.a - r.b * 3.0f + glm::vec2(-r.b.y, r.b.x) * v;
            (VoxelAt(*w.generator, vs, glm::vec3(p.x, r.hi - 1.0f, p.y)) != 0 ? built : open)++;
        }
        EXPECT_GT(built, 0) << "some of the top storey stands";
        EXPECT_GT(open, 0) << "and some is missing";
        return;
    }
    FAIL() << "no unfinished tower";
}




namespace {
    /// Generated voxels with the chunks kept, for tests that read many voxels near each other.
    struct CachedVoxels {
        const FortWorld& w;
        std::map<std::tuple<int, int, int>, std::pair<std::unique_ptr<Chunk>, ChunkContent>> chunks;
        uint8_t At(glm::vec3 world) {
            const float vs = w.land.voxelSize;
            const glm::ivec3 v(int(std::floor(world.x / vs)), int(std::floor(world.y / vs)), int(std::floor(world.z / vs)));
            const glm::ivec3 c(v.x >> 5, v.y >> 5, v.z >> 5);
            auto& [chunk, content] = chunks[{c.x, c.y, c.z}];
            if (!chunk) {
                chunk = std::make_unique<Chunk>();
                content = w.generator->Generate(c, *chunk);
            }
            if (content.kind != ChunkKind::Dense) return content.uniformId;
            return chunk->Get(v.x - c.x * kChunkSize, v.y - c.y * kChunkSize, v.z - c.z * kChunkSize);
        }
    };
}

TEST(Fort, AStoneStairClimbsToTheGuardroomOverTheGate) {
    // The guardroom (fortress.md "Vignettes and room kits"): a stair against the gatehouse's bailey face, a door, the
    // winch drum over the portcullis slot, the lever's spot on the floor clear to the ceiling.
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    CachedVoxels voxels{w, {}};
    // The room's kit is on the side of the gate the stair fits on: the lever's side.
    const glm::vec2 lever(w.plan.guardLever.x, w.plan.guardLever.z);
    const float side = glm::dot(lever - w.plan.gatePoint, w.plan.across) > 0.0f ? 1.0f : -1.0f;
    const glm::vec2 o = w.plan.gatePoint, u = w.plan.axis, v = w.plan.across * side;
    const float floor = w.plan.gateGround + 5.0f;
    const auto at = [&](float a, float b, float y) {
        const glm::vec2 p = o + u * a + v * b;
        return voxels.At(glm::vec3(p.x, y, p.y));
    };
    for (int k = 0; k < 20; ++k) {
        // A step's top is rounded down to the voxel grid; the lowest may lie under the bailey's ground.
        const float b = 9.25f - 0.25f * float(k) - 0.125f, top = w.plan.gateGround + 0.25f * float(k + 1);
        const glm::vec2 p = o + u * -4.75f + v * b;
        if (top < LandHeight(*w.map, w.land, p.x, p.y) + 0.25f) continue;
        EXPECT_EQ(at(-4.75f, b, top - 0.1875f), m.ashlar) << "step " << k;
        for (float y = top + 0.0625f; y < top + 1.85f; y += 0.125f) EXPECT_EQ(at(-4.75f, b, y), 0) << "headroom over step " << k << " at " << y - top;
    }
    // The floor and the door's head are rounded down to the voxel grid, as the lever's spawn voxel is.
    for (float y = floor + 0.0625f; y < floor + 2.0f; y += 0.125f) {
        EXPECT_EQ(at(-3.5f, 3.625f, y), 0) << "the door through the bailey wall at " << y - floor;
        EXPECT_EQ(at(-1.25f, 2.5f, y), 0) << "the lever's spot at " << y - floor;
    }
    EXPECT_EQ(at(-1.25f, 2.5f, floor - 0.1875f), m.planks) << "the guardroom's floor";
    EXPECT_EQ(at(1.75f, 0.0f, floor + 1.0f), m.wood) << "the winch drum";
    EXPECT_EQ(w.plan.guardLever.y, floor);
}

TEST(Fort, TheGreatHallHasAFireplaceAndTables) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    CachedVoxels voxels{w, {}};
    const glm::vec2 o = w.plan.keep, u = w.plan.axis, v(-u.y, u.x);
    const float floor = w.plan.keepGround + 11.25f;
    const auto at = [&](float a, float b, float y) {
        const glm::vec2 p = o + u * a + v * b;
        return voxels.At(glm::vec3(p.x, y, p.y));
    };
    EXPECT_EQ(at(-5.5f, 0.0f, floor - 0.0625f), m.brick) << "the hearth";
    EXPECT_EQ(at(-5.6875f, 0.0f, floor + 0.0625f), m.torch) << "its embers";
    EXPECT_EQ(at(-5.5f, 0.0f, floor + 1.875f), m.ashlar) << "the hood";
    EXPECT_EQ(at(-5.5f, 0.0f, floor + 0.75f), 0) << "the fireplace's mouth";
    EXPECT_NE(at(-6.5f, 0.0f, floor + 1.0f), 0) << "no window behind it";
    EXPECT_EQ(at(-6.125f, 0.0f, floor + 4.0f), m.soot) << "soot on the wall over the hood";
    for (const float side : {-2.0f, 2.0f}) EXPECT_EQ(at(1.0f, side + 0.8125f, floor + 0.25f), m.planks) << "a bench by the table at " << side;
    EXPECT_EQ(at(1.0f, 2.0f, floor + 1.0f), 0) << "room for a table";
}

TEST(Fort, EveryLightHangsInTheOpenBesideItsFlame) {
    // worldgen.md C8: each light's flame is a torch voxel; the light itself is in the air, not inside a wall.
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    const float vs = w.land.voxelSize;
    CachedVoxels voxels{w, {}};
    ASSERT_GE(w.plan.lights.size(), 8u);
    for (const FortLight& l : w.plan.lights) {
        EXPECT_EQ(voxels.At(l.at), 0) << "the light at " << l.at.x << " " << l.at.y << " " << l.at.z;
        bool flame = false;
        for (float dy = -1.0f; dy <= 0.25f && !flame; dy += vs)
            for (float dz = -0.5f; dz <= 0.5f && !flame; dz += vs)
                for (float dx = -0.5f; dx <= 0.5f && !flame; dx += vs) flame = voxels.At(l.at + glm::vec3(dx, dy, dz)) == m.torch;
        EXPECT_TRUE(flame) << "a flame under the light at " << l.at.x << " " << l.at.y << " " << l.at.z;
    }
}

TEST(Fort, TheLeanTosHoldTheirKits) {
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    CachedVoxels voxels{w, {}};
    int kits = 0;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape != FortShape::LeanTo) continue;
        const glm::vec2 d = glm::normalize(r.b - r.a);
        // An oblique piece holds the voxels whose middles lie in it: look a voxel around the point.
        const auto at = [&](float t, float wd, float y) {
            uint8_t found = 0;
            for (const float dy : {0.0f, -0.0625f, 0.0625f, -0.125f, 0.125f})
                for (const float dt : {0.0f, -0.0625f, 0.0625f})
                    for (const float dw : {0.0f, -0.0625f, 0.0625f}) {
                        const glm::vec2 p = r.a + d * (t + dt) + r.out * (wd + dw);
                        const uint8_t id = voxels.At(glm::vec3(p.x, r.lo + y + dy, p.y));
                        if (id != 0 && (found == 0 || id == m.iron || id == m.torch)) found = id;
                    }
            return found;
        };
        switch (r.building) {
        case FortBuilding::Forge:
            EXPECT_EQ(at(3.0f, 1.25f, 0.625f), m.iron) << "the anvil";
            EXPECT_EQ(at(1.0f, 1.25f, 0.8125f), m.torch) << "embers on the hearth";
            ++kits;
            break;
        case FortBuilding::Stables:
            EXPECT_EQ(at(2.0f, 1.0f, 0.5f), m.planks) << "a stall's partition";
            EXPECT_EQ(at(0.75f, 0.5f, 0.375f), m.thatch) << "hay";
            ++kits;
            break;
        case FortBuilding::Workshop:
            EXPECT_EQ(at(0.5f, 1.5f, 0.8125f), m.planks) << "the bench across the end wall";
            ++kits;
            break;
        default:
            break;
        }
    }
    EXPECT_GE(kits, 2);
}

TEST(Fort, DampStainsOnlyTheShadedBatters) {
    // Faces turned from the sun take damp low on their batter; faces toward it stay granite.
    const FortWorld& w = FortSetup();
    const WorldMaterials& m = w.land.materials;
    CachedVoxels voxels{w, {}};
    const glm::vec2 shade(-0.78935f, -0.61394f);
    int shaded = 0, sunny = 0;
    for (const FortRecord& r : w.plan.records) {
        if (r.shape != FortShape::Wall || r.half < 1.2f || r.gapHi >= r.gapLo || r.patchHi > r.patchLo) continue;
        const float len = glm::length(r.b - r.a);
        const glm::vec2 d = (r.b - r.a) / len;
        glm::vec2 n(d.y, -d.x);
        if (glm::dot(n, (r.a + r.b) * 0.5f - w.plan.keep) < 0.0f) n = -n;
        const float facing = glm::dot(n, shade);
        if (std::abs(facing) < 0.5f) continue;
        for (float t = 4.5f; t < len - 4.5f; t += 0.5f) {   // clear of the towers' batters at the ends
            // The batter's outer voxel, 0.25 m over the ground.
            const glm::vec2 foot = r.a + d * t + n * (r.half + 0.5f);
            const float y = LandHeight(*w.map, w.land, foot.x, foot.y) + 0.25f;
            for (float s = r.half + 1.5f; s > r.half; s -= 0.0625f) {
                const glm::vec2 p = r.a + d * t + n * s;
                const uint8_t id = voxels.At(glm::vec3(p.x, y, p.y));
                if (id == 0) continue;
                if (id == m.graniteDamp) (facing > 0.0f ? shaded : sunny)++;
                break;
            }
        }
    }
    EXPECT_GT(shaded, 0) << "damp on the shaded batters";
    EXPECT_EQ(sunny, 0) << "none on the sunny ones";
}

