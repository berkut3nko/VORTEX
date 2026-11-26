// The operators of magic in sim (docs/design/magic.md "The four operators (exact)"): the Burst
// strip that follows the ground across the aim, Barricade Wave's fill in bands, Plume's single
// column; and the site-based blast that the sphere blast forwards to.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <span>
#include <tuple>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;

using namespace vortex;

namespace {
    constexpr float kVoxel = 0.125f;
    constexpr int g = 64;                             // flat ground: voxels y < 64, its top at 8 m
    const glm::dvec3 kFocus(0.0, 8.0, 0.0);           // window chunks (-3..2, -1..4, -3..2)

    sim::SimulationConfig YardConfig(float voxelSize = kVoxel) {
        sim::SimulationConfig config;
        config.chunkCapacity = 512;
        config.maxParticles = 1024;
        config.jobThreads = 2;
        config.world.voxelSize = voxelSize;
        config.world.windowSize = {6, 6, 6};
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        return config;
    }

    struct PropagateYard {
        explicit PropagateYard(const sim::SimulationConfig& config = YardConfig()) : simulation(config) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(g, ids.grass, ids.stone));
            if (initialized) simulation.World().LoadBlocking(kFocus);
        }
        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        void Box(const glm::ivec3& min, const glm::ivec3& max, uint8_t id) {
            ASSERT_TRUE(sim::ApplyOp(World(), sim::WorldOp::Box(min, max, id)).Complete());
        }
        uint64_t Hash() const {
            const voxel::World& world = simulation.World();
            return voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize()).hash;
        }
        sim::Simulation simulation;
        bool initialized = false;
    };

    /// Barricade Wave's strip from the table at 0.125 m, seeded at `seed` on a floor (n = +y).
    sim::BurstResult WallStrip(voxel::World& world, const glm::ivec3& seed, const glm::ivec3& aim) {
        const sim::SpellTable table = sim::BuildSpellTable(kVoxel);
        const sim::SpellDef& s = table.spells[sim::kBarricadeWave];
        sim::BurstParams p;
        p.seed = seed;
        p.axes = sim::FaceAxesOf({0, 1, 0});
        p.aim = aim;
        p.halfWidth = s.burstHalfWidth;
        p.halfThickness = s.burstHalfThickness;
        p.stepMax = s.stepMax;
        p.pops = s.burstPops;
        p.visitCap = s.burstVisitCap;
        p.plane = s.burstPlane;
        p.normalMin = s.burstNormalMin;
        p.normalMax = s.burstNormalMax;
        return sim::BurstStrip(world, sim::WindowWalls(world), p);
    }

    const glm::ivec3 kAimAlongZ(0, -6000, 15000);   // the strip lies across it: along x
    const glm::ivec3 kAimAlongX(15000, -6000, 0);   // the strip lies along z
}

TEST(Propagate, FaceAxesTakeTheLowerAxisFirst) {
    const sim::FaceAxes y = sim::FaceAxesOf({0, -1, 0});
    EXPECT_EQ(y.n, glm::ivec3(0, -1, 0));
    EXPECT_EQ(y.u, glm::ivec3(1, 0, 0));
    EXPECT_EQ(y.v, glm::ivec3(0, 0, 1));
    const sim::FaceAxes x = sim::FaceAxesOf({1, 0, 0});
    EXPECT_EQ(x.u, glm::ivec3(0, 1, 0));
    EXPECT_EQ(x.v, glm::ivec3(0, 0, 1));
    const sim::FaceAxes z = sim::FaceAxesOf({0, 0, -1});
    EXPECT_EQ(z.u, glm::ivec3(1, 0, 0));
    EXPECT_EQ(z.v, glm::ivec3(0, 1, 0));
}

TEST(Propagate, BurstCellsRestOnTheirSurface) {
    PropagateYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint8_t stone = yard.Ids().stone;
    // Stairs of 3 voxels (0.375 m) up along +x, a slope of 1 in 2 up along -x.
    for (int i = 1; i <= 6; ++i) yard.Box({4 * i, g, -40}, {40, g + 3 * i, 40}, stone);
    for (int x = -40; x < 0; ++x) yard.Box({x, g, -40}, {x + 1, g + (-x) / 2, 40}, stone);

    const sim::BurstResult strip = WallStrip(yard.World(), {0, g, 0}, kAimAlongZ);
    ASSERT_FALSE(strip.cells.empty());
    ASSERT_EQ(strip.cells.size(), strip.lateral.size());
    int minX = 0, maxX = 0;
    std::set<std::tuple<int, int, int>> unique;
    for (const glm::ivec3& c : strip.cells) {
        EXPECT_EQ(yard.World().GetVoxel(c), 0) << c.x << " " << c.y << " " << c.z;
        EXPECT_NE(yard.World().GetVoxel(c - glm::ivec3(0, 1, 0)), 0) << c.x << " " << c.y << " " << c.z;
        EXPECT_LE(std::abs(c.z), 1);
        EXPECT_TRUE(unique.emplace(c.x, c.y, c.z).second);
        minX = std::min(minX, c.x);
        maxX = std::max(maxX, c.x);
    }
    // It climbs the stairs and the slope to both ends of the strip.
    EXPECT_EQ(maxX, 18);
    EXPECT_EQ(minX, -18);
    EXPECT_GT(strip.visits, 0u);
    EXPECT_GT(strip.chunksRead, 0u);
    // The same strip twice: the order is a function of the voxels only.
    const sim::BurstResult again = WallStrip(yard.World(), {0, g, 0}, kAimAlongZ);
    EXPECT_EQ(again.cells, strip.cells);
    EXPECT_EQ(again.visits, strip.visits);
}

TEST(Propagate, BurstNeverStepsMoreThanStepMax) {
    {
        PropagateYard yard;
        ASSERT_TRUE(yard.initialized);
        yard.Box({6, g, -40}, {40, g + 4, 40}, yard.Ids().stone);   // a 0.5 m ledge across the strip
        const sim::BurstResult strip = WallStrip(yard.World(), {0, g, 0}, kAimAlongZ);
        ASSERT_FALSE(strip.cells.empty());
        for (const glm::ivec3& c : strip.cells) EXPECT_LT(c.x, 6) << "past the ledge at " << c.x << " " << c.y;
        // From the top of the ledge the strip does not drop down it either.
        const sim::BurstResult top = WallStrip(yard.World(), {10, g + 4, 0}, kAimAlongZ);
        ASSERT_FALSE(top.cells.empty());
        for (const glm::ivec3& c : top.cells) EXPECT_GE(c.x, 6) << "down the ledge at " << c.x << " " << c.y;
    }
    {
        PropagateYard yard;
        ASSERT_TRUE(yard.initialized);
        yard.Box({6, g, -40}, {40, g + 3, 40}, yard.Ids().stone);   // a 3-voxel step: it continues
        const sim::BurstResult strip = WallStrip(yard.World(), {0, g, 0}, kAimAlongZ);
        int maxX = 0;
        for (const glm::ivec3& c : strip.cells) {
            maxX = std::max(maxX, c.x);
            if (c.x >= 6) EXPECT_EQ(c.y, g + 3);
        }
        EXPECT_EQ(maxX, 18);
    }
}

TEST(Propagate, BarricadeIsAWallAcrossTheAim) {
    PropagateYard yard;
    ASSERT_TRUE(yard.initialized);
    const sim::SpellTable table = sim::BuildSpellTable(kVoxel);
    const sim::SpellDef& s = table.spells[sim::kBarricadeWave];
    const glm::ivec3 seed(0, g, 0);
    const sim::BurstResult strip = WallStrip(yard.World(), seed, kAimAlongX);
    const sim::FillBands fill =
        sim::BarricadeFill(yard.World(), sim::WindowWalls(yard.World()), strip, sim::FaceAxesOf({0, 1, 0}), s.rimHeight, s.bandCount, s.burstHalfWidth);

    glm::ivec3 lo(1 << 30), hi(-(1 << 30));
    size_t total = 0;
    for (const auto& band : fill.bands) {
        total += band.size();
        for (const glm::ivec3& c : band) {
            lo = glm::min(lo, c);
            hi = glm::max(hi, c);
        }
    }
    std::printf("Barricade Wave on a flat floor: %u cells, box %d x %d x %d\n", fill.cells, hi.x - lo.x + 1, hi.y - lo.y + 1,
                hi.z - lo.z + 1);
    EXPECT_EQ(total, fill.cells);
    EXPECT_EQ(hi.z - lo.z + 1, 37);
    EXPECT_EQ(hi.x - lo.x + 1, 3);
    EXPECT_EQ(hi.y - lo.y + 1, 12);
    EXPECT_GE(fill.cells, 1250u);
    EXPECT_LE(fill.cells, 1340u);
    for (int b = 0; b < 4; ++b) EXPECT_FALSE(fill.bands[static_cast<size_t>(b)].empty()) << "band " << b;
    for (size_t b = 4; b < fill.bands.size(); ++b) EXPECT_TRUE(fill.bands[b].empty());
    EXPECT_NE(std::find(fill.bands[0].begin(), fill.bands[0].end(), seed), fill.bands[0].end());
    EXPECT_NE(std::find(fill.bands[0].begin(), fill.bands[0].end(), seed + glm::ivec3(0, 11, 0)), fill.bands[0].end());
    EXPECT_NE(std::find(fill.bands[3].begin(), fill.bands[3].end(), seed + glm::ivec3(0, 0, 18)), fill.bands[3].end());

    // The bands go out as MapMask(Knit) ops: each changes exactly its cells.
    uint32_t knit = 0;
    for (size_t b = 0; b < 4; ++b) {
        const sim::WorldOp op = sim::MapCellsOp(voxel::Map_Knit, fill.bands[b]);
        EXPECT_EQ(op.kind, sim::WorldOpKind::MapMask);
        EXPECT_EQ(op.mask.Count(), fill.bands[b].size());
        const sim::OpResult r = sim::ApplyOp(yard.World(), op);
        EXPECT_EQ(r.changed, fill.bands[b].size());
        knit += r.changed;
    }
    EXPECT_EQ(knit, fill.cells);
    for (const auto& band : fill.bands)
        for (const glm::ivec3& c : band) EXPECT_EQ(yard.World().GetVoxel(c), yard.Ids().knitIron);
}

TEST(Propagate, PlumeColumnStopsUnderACeiling) {
    PropagateYard yard;
    ASSERT_TRUE(yard.initialized);
    yard.Box({-5, g + 24, -5}, {6, g + 26, 6}, yard.Ids().stone);   // a ceiling 3 m over the floor
    const uint16_t riseMax = sim::BuildSpellTable(kVoxel).spells[sim::kUpdraft].riseMax;
    const sim::PlumeColumn under = sim::PlumeSingleColumn(yard.World(), sim::WindowWalls(yard.World()), {0, g, 0}, riseMax);
    EXPECT_EQ(under.bottom, glm::ivec3(0, g, 0));
    EXPECT_EQ(under.top.y - under.bottom.y, 23);
    EXPECT_EQ(under.visits, 23u);
    const sim::PlumeColumn open = sim::PlumeSingleColumn(yard.World(), sim::WindowWalls(yard.World()), {40, g, 40}, riseMax);
    EXPECT_EQ(open.top.y - open.bottom.y, static_cast<int>(riseMax));
    const sim::PlumeColumn solid = sim::PlumeSingleColumn(yard.World(), sim::WindowWalls(yard.World()), {0, g - 1, 0}, riseMax);
    EXPECT_EQ(solid.top, solid.bottom);
    EXPECT_EQ(solid.visits, 0u);
    // The window's top row is a wall: a rise that would leave it stops below it.
    const sim::PlumeColumn high = sim::PlumeSingleColumn(yard.World(), sim::WindowWalls(yard.World()), {40, 120, 40}, riseMax);
    EXPECT_EQ(high.top.y, sim::WindowWalls(yard.World()).max.y - 1);
}

TEST(Blast, SiteOverloadEqualsTheSphereBlast) {
    PropagateYard a, b;
    ASSERT_TRUE(a.initialized && b.initialized);
    for (PropagateYard* yard : {&a, &b}) {
        yard->Box({0, g, 0}, {3, g + 16, 3}, yard->Ids().stone);
        yard->Box({-4, g + 16, -4}, {7, g + 19, 7}, yard->Ids().stone);
    }
    const voxel::SphereQ cut{glm::ivec3(1 * 16 + 8, (g + 8) * 16 + 8, 1 * 16 + 8), 4 * 16};
    const sim::BlastResult viaSphere = sim::Blast(a.simulation, cut, 1600);
    const sim::BlastSite site = sim::SphereSite(b.World(), cut, 1600);
    const sim::BlastResult viaSite = sim::Blast(b.simulation, site);
    ASSERT_EQ(viaSphere.ops.size(), viaSite.ops.size());
    ASSERT_GE(viaSphere.ops.size(), 2u);
    for (size_t i = 0; i < viaSphere.ops.size(); ++i) {
        EXPECT_EQ(viaSphere.ops[i].op, viaSite.ops[i].op);
        EXPECT_EQ(viaSphere.ops[i].result.changed, viaSite.ops[i].result.changed);
    }
    EXPECT_EQ(viaSphere.center, viaSite.center);
    EXPECT_EQ(viaSphere.radius, viaSite.radius);
    EXPECT_EQ(viaSphere.search.nodeVisits, viaSite.search.nodeVisits);
    EXPECT_EQ(a.Hash(), b.Hash());
    // The boxes are those of IslandBox and FloodBox.
    glm::ivec3 mn, mx;
    sim::IslandBox(cut, mn, mx);
    EXPECT_EQ(site.islandMin, mn);
    EXPECT_EQ(site.islandMax, mx);
    sim::FloodBox(cut, mn, mx);
    EXPECT_EQ(site.floodMin, mn);
    EXPECT_EQ(site.floodMax, mx);
}

TEST(Blast, ChunkSiteBoxesAndNoCarve) {
    PropagateYard yard;
    ASSERT_TRUE(yard.initialized);
    const sim::BlastSite site = sim::ChunkSite(yard.World(), {1, 2, -1});
    EXPECT_EQ(site.islandMin, glm::ivec3(32 - 24, 64 - 24, -32 - 24));
    EXPECT_EQ(site.islandMax, glm::ivec3(64 + 24, 96 + 24, 0 + 24));
    EXPECT_EQ(site.floodMin, glm::ivec3(48 - 96, 80 - 96, -16 - 96));
    EXPECT_EQ(site.floodMax, glm::ivec3(48 + 96, 80 + 96, -16 + 96));
    EXPECT_EQ(site.radius, 0.0f);
    // A floating block in the chunk: the site check detaches it and carves nothing.
    yard.Box({40, g + 10, -20}, {44, g + 14, -16}, yard.Ids().stone);
    sim::BlastOptions options;
    options.noCarve = true;
    const uint64_t before = yard.Hash();
    const sim::BlastResult r = sim::Blast(yard.simulation, site, options);
    EXPECT_TRUE(r.islandsSearched);
    ASSERT_EQ(r.ops.size(), 1u);
    EXPECT_EQ(r.ops[0].op.kind, sim::WorldOpKind::Detach);
    EXPECT_EQ(r.ops[0].result.changed, 64u);
    EXPECT_NE(yard.Hash(), before);
    // Again on the same chunk: nothing left to do.
    const sim::BlastResult none = sim::Blast(yard.simulation, site, options);
    EXPECT_TRUE(none.ops.empty());
}
