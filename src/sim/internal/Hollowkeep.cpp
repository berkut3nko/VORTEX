module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

module vortex.sim;

import vortex.voxel;
import vortex.scene;

// Hollowkeep on the baked map (engine/docs/design/fortress.md): the land generator stamps the castle's voxels from
// `voxel::PlanFort`; this template adds what is not voxels: the portcullis and its lever, the spawn.

namespace vortex::sim {

    RegionTemplate HollowkeepTemplate(const voxel::LandMap& map, const voxel::LandGeneratorSettings& land) {
        RegionTemplate t;
        const voxel::FortPlan plan = voxel::PlanFort(map, land);
        if (!plan.placed) {
            t.sited = false;
            return t;
        }
        const voxel::WorldMaterials& m = land.materials;
        const float vs = land.voxelSize;
        const auto V = [vs](float metres) { return int(std::floor(metres / vs)); };
        const glm::vec2 axis = plan.axis, across = plan.across, gatePoint = plan.gatePoint;
        const float gateGround = plan.gateGround;
        const bool alongX = std::abs(axis.x) > 0.5f;

        // The portcullis ([mech-1]): a 4 x 4 m iron lattice in the gatehouse slot's plane (2.5 m out from the gate's
        // middle), rising 3.5 m; the lever on its post 6 m inside the gate, beside the passage, runs it.
        const int n4 = std::max(1, V(4.0f));
        const glm::ivec3 size = alongX ? glm::ivec3(1, n4, n4) : glm::ivec3(n4, n4, 1);
        voxel::VoxelVolume lattice(size);
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) {
                    const int u = alongX ? z : x;
                    if (u % 3 == 0 || y % 4 == 0 || y == 0) lattice.Set(x, y, z, m.iron);
                }
        TemplateMechanism gate;
        gate.model = std::make_shared<voxel::VoxelModel>(std::move(lattice), voxel::kWorldPalette, "hollowkeep portcullis");
        const glm::vec2 slot = gatePoint + axis * 2.5f;
        const glm::vec2 corner = slot - glm::abs(across) * 2.0f;
        gate.spawnVoxel = glm::ivec3(V(corner.x), V(gateGround), V(corner.y));
        gate.desc.kind = MechanismKind::Portcullis;
        gate.desc.axis = 2;
        gate.desc.minPos = 0;
        gate.desc.maxPos = static_cast<int16_t>(V(3.5f) * 16);
        gate.desc.openTicks = 180;
        gate.desc.closeTicks = 45;
        gate.desc.useBox = gate.desc.blockBox = {glm::ivec3(0), size};
        gate.desc.hingeBox = {glm::ivec3(0, size.y - 1, 0), size};
        gate.desc.pushAxis = alongX ? 0 : 4;
        t.mechanisms.push_back(std::move(gate));

        const glm::vec2 post = gatePoint - axis * 6.0f + across * 3.0f;
        const float postGround = voxel::LandHeight(map, land, post.x, post.y);
        const glm::ivec3 leverSize(2, 6, 2);
        voxel::VoxelVolume stick(leverSize);
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 6; ++y)
                for (int x = 0; x < 2; ++x) stick.Set(x, y, z, m.wood);
        TemplateMechanism lever;
        lever.model = std::make_shared<voxel::VoxelModel>(std::move(stick), voxel::kWorldPalette, "hollowkeep lever");
        lever.spawnVoxel = glm::ivec3(V(post.x) - 1, V(postGround + 1.0f), V(post.y) - 1);
        lever.desc.kind = MechanismKind::Lever;
        lever.desc.flags = scene::MechanismDesc_Usable;
        lever.desc.pivotQ = {16, 0, 16};
        lever.desc.axis = 0;
        lever.desc.minPos = 0;
        lever.desc.maxPos = 512;
        lever.desc.openTicks = lever.desc.closeTicks = 12;
        lever.desc.useBox = lever.desc.blockBox = {glm::ivec3(0), leverSize};
        lever.desc.hingeBox = {glm::ivec3(0), glm::ivec3(leverSize.x, 1, leverSize.z)};
        lever.desc.pushAxis = 4;
        lever.link = 0;
        t.mechanisms.push_back(std::move(lever));

        // The player arrives on the castle road, past the bridge over the ditch.
        const glm::vec2 arrive = gatePoint + axis * (plan.bridgeSpan + 2.0f);
        t.spawn = glm::dvec3(arrive.x, voxel::LandHeight(map, land, arrive.x, arrive.y) + 0.1f, arrive.y);
        t.anchor = glm::ivec3(V(plan.keep.x), V(plan.keepGround), V(plan.keep.y));
        return t;
    }
}
