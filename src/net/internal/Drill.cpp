module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

module vortex.net;

import vortex.voxel;
import vortex.sim;

namespace vortex::net {

    const char* ToString(DrillKind kind) {
        switch (kind) {
            case DrillKind::Mushroom: return "mushroom";
            case DrillKind::Bar: return "bar";
            case DrillKind::NeckTower: return "neck tower";
        }
        return "?";
    }

    void BlastDrill::Layout(const sim::RegionDesc& region) {
        const glm::ivec2 lo = region.playMinChunk * voxel::kChunkSize;
        const glm::ivec2 span = (region.playMaxChunk - region.playMinChunk) * voxel::kChunkSize;
        const int nx = std::max(1, span.x / kSiteVoxels), nz = std::max(1, span.y / kSiteVoxels);
        // Centred in the play bounds.
        const glm::ivec2 offset = (span - glm::ivec2(nx, nz) * kSiteVoxels) / 2;
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x) {
                const glm::ivec2 p = lo + offset + glm::ivec2(x, z) * kSiteVoxels;
                m_Sites.push_back(glm::ivec3(p.x, 0, p.y));
            }
    }

    BlastDrill::BlastDrill(const sim::RegionDesc& region, const voxel::WorldMaterials& ids, int32_t groundVoxels) : m_Ids(ids) {
        Layout(region);
        for (glm::ivec3& s : m_Sites) s.y = groundVoxels;
    }

    BlastDrill::BlastDrill(const sim::RegionDesc& region, const voxel::WorldMaterials& ids, const voxel::World& world) : m_Ids(ids) {
        Layout(region);
        const int top = (world.WindowMinChunk().y + world.WindowSize().y) * voxel::kChunkSize - 1;
        const int bottom = world.WindowMinChunk().y * voxel::kChunkSize;
        for (glm::ivec3& s : m_Sites) {
            const int x = s.x + kSiteVoxels / 2, z = s.z + kSiteVoxels / 2;
            int y = top;
            while (y > bottom && world.GetVoxel(glm::ivec3(x, y, z)) == 0) --y;
            // Room for the structures under the build ceiling.
            s.y = std::min(y + 1, top - kSiteHeight - 8);
        }
    }

    DrillStep BlastDrill::Step(uint32_t index) const {
        DrillStep step;
        step.site = index % Sites();
        step.kind = static_cast<DrillKind>(index % 3);
        const glm::ivec3 o = m_Sites[step.site];
        const voxel::WorldMaterials& m = m_Ids;
        auto box = [&step, o](glm::ivec3 lo, glm::ivec3 hi, uint8_t id) { step.build.push_back(sim::WorldOp::Box(o + lo, o + hi, id)); };
        auto at = [o](glm::ivec3 v) { return (o + v) * voxel::kQuantaPerVoxel + glm::ivec3(voxel::kQuantaPerVoxel / 2); };
        constexpr uint16_t kStrong = 1600;     // 100 hardness: everything but bedrock

        box({0, 0, 0}, {kSiteVoxels, kSiteHeight, kSiteVoxels}, 0);   // clear what the last structure left
        box({0, -4, 0}, {kSiteVoxels, 0, kSiteVoxels}, m.stone);      // foundation
        switch (step.kind) {
            case DrillKind::Mushroom:
                box({48, 0, 48}, {51, 16, 51}, m.stone);                // stem
                box({44, 16, 44}, {55, 19, 55}, m.planks);              // cap
                box({57, 0, 49}, {58, 14, 50}, m.stone);                // a thin stalk beside it ...
                box({57, 14, 49}, {59, 15, 51}, m.wood);                // ... holding a 2 x 2 pebble: a crumb
                step.cast = voxel::SphereQ{at({49, 8, 49}), 9 * voxel::kQuantaPerVoxel};
                step.power = kStrong;
                break;
            case DrillKind::Bar:
                box({48, 0, 48}, {52, 20, 52}, m.stone);                // post
                box({13, 20, 48}, {83, 24, 52}, m.planks);              // bar, 35 voxels to either side
                step.cast = voxel::SphereQ{at({50, 8, 50}), 6 * voxel::kQuantaPerVoxel};
                step.power = kStrong;
                break;
            case DrillKind::NeckTower: {
                // The game's watch tower (tests/sim/NeckTests.cpp TowerOnANeck, 0.1 m units) on a
                // 2 x 2 neck at its north-west corner, rows 1..15.
                const glm::ivec3 c(2, 0, 92);
                auto b = [&box, c](glm::ivec3 lo, glm::ivec3 hi, uint8_t id) { box(c + lo, c + hi, id); };
                b({10, -1, -80}, {80, 0, -10}, m.stoneDark);
                b({30, 0, -60}, {58, 70, -32}, m.stone);
                b({33, 0, -57}, {55, 66, -35}, 0);
                b({41, 0, -35}, {47, 20, -32}, 0);
                b({28, 66, -62}, {60, 70, -30}, m.planks);
                for (int i = 0; i < 4; ++i) {
                    const glm::ivec3 p(i & 1 ? 58 : 28, 70, i & 2 ? -32 : -62);
                    b(p, p + glm::ivec3(2, 8, 2), m.wood);
                    b(p + glm::ivec3(0, 8, 0), p + glm::ivec3(2, 10, 2), m.torch);
                }
                b({30, 1, -60}, {58, 16, -32}, 0);
                b({30, 1, -60}, {32, 16, -58}, m.stone);
                step.cast = voxel::SphereQ{at(c + glm::ivec3(44, 5, -46)), 6 * voxel::kQuantaPerVoxel};
                step.power = voxel::QuantizePower(6.0f);
                break;
            }
        }
        return step;
    }
}
