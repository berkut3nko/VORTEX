// Hollowkeep's template on the baked map (docs/design/fortress.md): the land generator stamps the castle's voxels, so
// the template carries only the portcullis and its lever and the spawn past the bridge.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
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

TEST(Hollowkeep, TheTemplateAddsOnlyWhatIsNotVoxels) {
    const Baked& b = Castle();
    ASSERT_TRUE(b.map.site.placed);
    ASSERT_TRUE(b.castle.sited);
    EXPECT_TRUE(b.castle.ops.empty()) << "the land generator stamps the castle";
    // The player arrives on the castle road past the bridge, on the ground.
    const voxel::FortPlan plan = voxel::PlanFort(b.map, b.land);
    const glm::vec2 spawn(float(b.castle.spawn.x), float(b.castle.spawn.z));
    EXPECT_GT(glm::dot(spawn - plan.gatePoint, plan.axis), plan.bridgeSpan);
    EXPECT_NEAR(float(b.castle.spawn.y), voxel::LandHeight(b.map, b.land, spawn.x, spawn.y) + 0.1f, 1.0e-3f);
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

TEST(Hollowkeep, TheBasicBlastBreachesTheCastleWithoutReleasingIt) {
    // Tier 2 ashlar breaks under the basic blast (power 6.0), and the load check releases nothing from a 1.2 m hole in a
    // curtain face, a tower's shell or the keep's upper shell (fortress.md "Material and structural integrity matrix").
    const Baked& b = Castle();
    const auto map = std::make_shared<const voxel::LandMap>(b.map);
    const voxel::FortPlan plan = voxel::PlanFort(b.map, b.land);
    const float vs = b.land.voxelSize;
    std::vector<glm::vec3> shots;
    for (const voxel::FortRecord& r : plan.records) {
        if (r.shape == voxel::FortShape::Wall && r.half > 1.2f && r.gapHi < r.gapLo && shots.empty()) {
            const glm::vec2 d = glm::normalize(r.b - r.a);
            glm::vec2 n(d.y, -d.x);
            if (glm::dot(n, (r.a + r.b) * 0.5f - plan.keep) < 0.0f) n = -n;
            const glm::vec2 p = (r.a + r.b) * 0.5f + d * 1.5f + n * (r.half - 0.2f);
            shots.push_back(glm::vec3(p.x, voxel::FortWalk(r, glm::length(r.b - r.a) * 0.5f + 1.5f) - 3.0f, p.y));
        }
    }
    for (const voxel::FortRecord& r : plan.records) {
        if (r.shape != voxel::FortShape::Block) continue;
        if (r.block == voxel::FortBlock::Tower && shots.size() == 1) {
            const glm::vec2 p = r.a + r.b * (r.half - 0.3f);
            shots.push_back(glm::vec3(p.x, r.lo + 6.0f, p.y));
        }
        if (r.block == voxel::FortBlock::Keep) {
            const glm::vec2 p = r.a + glm::vec2(-r.b.y, r.b.x) * (r.half - 0.4f);
            shots.push_back(glm::vec3(p.x, r.lo + 15.0f, p.y));
        }
    }
    ASSERT_EQ(shots.size(), 3u);
    for (const glm::vec3& shot : shots) {
        sim::SimulationConfig config;
        config.chunkCapacity = 1536;
        config.maxParticles = 4096;
        config.jobThreads = 3;
        config.world.voxelSize = vs;
        config.world.windowSize = glm::ivec3(8, 8, 8);
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        sim::Simulation simulation(config);
        ASSERT_TRUE(simulation.Initialize(voxel::CreateLandGenerator(map, b.land)));
        simulation.World().LoadBlocking(glm::dvec3(shot));
        const voxel::SphereQ sphere{glm::ivec3(glm::floor(shot / vs * 16.0f)), uint16_t(std::lround(1.2f / vs * 16.0f))};
        const sim::BlastResult blast = sim::Blast(simulation, sphere, voxel::QuantizePower(6.0f));
        ASSERT_FALSE(blast.ops.empty());
        EXPECT_TRUE(blast.islandsSearched) << "at (" << shot.x << ", " << shot.y << ", " << shot.z << ")";
        EXPECT_GT(blast.ops[0].edits.size(), 500u) << "the blast breaks the ashlar at (" << shot.x << ", " << shot.y << ", " << shot.z << ")";
        EXPECT_EQ(simulation.IslandSearchCheck().neckReleases, 0u) << "at (" << shot.x << ", " << shot.y << ", " << shot.z << ")";
        // The load check judged the network: nothing skipped or cut short by a budget (the ring of walls running out of
        // the window is support, as in the game's window).
        EXPECT_EQ(blast.search.neckSkipped, 0u);
        EXPECT_EQ(blast.search.neckFlowBudget, 0u);
        EXPECT_EQ(blast.search.neckExteriorBudget, 0u);
        EXPECT_FALSE(blast.search.neckDeferred);
    }
}

TEST(Hollowkeep, ASiegeShotOpensTheCulvertGrate) {
    // The low route (fortress.md L1): the basic blast (power 6.0) leaves the iron grate and its granite frame; a
    // siege-strength shot (power 9.0, over iron's 8.0) opens the crawl behind it.
    const Baked& b = Castle();
    const auto map = std::make_shared<const voxel::LandMap>(b.map);
    const voxel::FortPlan plan = voxel::PlanFort(b.map, b.land);
    ASSERT_TRUE(plan.culvert);
    const float vs = b.land.voxelSize;
    const glm::vec3 grate(plan.culvertMouth.x, plan.culvertFloor + 0.5f, plan.culvertMouth.y);
    const voxel::FortRecord* crawl = nullptr;
    for (const voxel::FortRecord& r : plan.records)
        if (r.shape == voxel::FortShape::Tunnel && crawl == nullptr) crawl = &r;
    ASSERT_NE(crawl, nullptr);
    // A bar of the grate, 0.1 m into the crawl from its mouth.
    const glm::vec2 d = glm::normalize(crawl->b - crawl->a), n(-d.y, d.x);
    const glm::vec2 barXZ = crawl->a + d * 0.1f + n * 0.0625f;
    const glm::ivec3 bar(int(std::floor(barXZ.x / vs)), int(std::floor((crawl->lo + 0.5f) / vs)), int(std::floor(barXZ.y / vs)));
    for (const float power : {6.0f, 9.0f}) {
        sim::SimulationConfig config;
        config.chunkCapacity = 1536;
        config.maxParticles = 4096;
        config.jobThreads = 3;
        config.world.voxelSize = vs;
        config.world.windowSize = glm::ivec3(8, 8, 8);
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        sim::Simulation simulation(config);
        ASSERT_TRUE(simulation.Initialize(voxel::CreateLandGenerator(map, b.land)));
        simulation.World().LoadBlocking(glm::dvec3(grate));
        ASSERT_EQ(simulation.World().GetVoxel(bar), b.land.materials.iron);
        // The shot lands just outside the grate, on the crawl's axis.
        const glm::vec2 at = crawl->a - d * 0.3f;
        const glm::vec3 shot(at.x, crawl->lo + 0.5f, at.y);
        const voxel::SphereQ sphere{glm::ivec3(glm::floor(shot / vs * 16.0f)), uint16_t(std::lround(1.2f / vs * 16.0f))};
        sim::Blast(simulation, sphere, voxel::QuantizePower(power));
        if (power < 7.0f)
            EXPECT_EQ(simulation.World().GetVoxel(bar), b.land.materials.iron) << "the basic blast leaves the grate";
        else
            EXPECT_EQ(simulation.World().GetVoxel(bar), 0) << "a siege shot opens it";
        EXPECT_EQ(simulation.IslandSearchCheck().neckReleases, 0u);
    }
}
