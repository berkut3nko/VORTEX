module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.voxel;
import vortex.scene;

// Hollowkeep on the baked map (engine/docs/design/fortress.md): the land generator stamps the castle's voxels from
// `voxel::PlanFort`; this template adds what is not voxels: the portcullis and the guardroom's lever, the lights, the
// movable props, the spawn.

namespace vortex::sim {

    namespace {
        /// The voxels the land generator makes (the castle stamped in), chunk by chunk as they are asked for.
        class GeneratedVoxels {
        public:
            explicit GeneratedVoxels(std::shared_ptr<voxel::IWorldGenerator> generator) : m_Generator(std::move(generator)) {}

            uint8_t At(const glm::ivec3& v) {
                const glm::ivec3 c(FloorDiv(v.x), FloorDiv(v.y), FloorDiv(v.z));
                const auto key = std::make_tuple(c.x, c.y, c.z);
                auto it = m_Chunks.find(key);
                if (it == m_Chunks.end()) {
                    Entry e;
                    e.chunk = std::make_unique<voxel::Chunk>();
                    e.content = m_Generator->Generate(c, *e.chunk);
                    it = m_Chunks.emplace(key, std::move(e)).first;
                }
                const Entry& e = it->second;
                if (e.content.kind != voxel::ChunkKind::Dense) return e.content.uniformId;
                const glm::ivec3 l = v - c * voxel::kChunkSize;
                return e.chunk->ids[voxel::Chunk::VoxelIndex(l.x, l.y, l.z)];
            }

        private:
            static int FloorDiv(int a) { return a >= 0 ? a / voxel::kChunkSize : -((-a + voxel::kChunkSize - 1) / voxel::kChunkSize); }
            struct Entry {
                std::unique_ptr<voxel::Chunk> chunk;
                voxel::ChunkContent content;
            };
            std::shared_ptr<voxel::IWorldGenerator> m_Generator;
            std::map<std::tuple<int, int, int>, Entry> m_Chunks;
        };

        /// A prop's model along x (fortress.md "Movable props"), at the land's voxel size.
        voxel::VoxelVolume PropVolume(voxel::FortProp kind, const voxel::WorldMaterials& m, float vs) {
            const auto V = [vs](float metres) { return std::max(1, int(std::lround(metres / vs))); };
            switch (kind) {
            case voxel::FortProp::Plank: {
                voxel::VoxelVolume v(glm::ivec3(V(2.0f), 1, V(0.25f)));
                v.FillBox(glm::ivec3(0), v.Size(), m.planks);
                return v;
            }
            case voxel::FortProp::Beam: {
                voxel::VoxelVolume v(glm::ivec3(V(2.5f), V(0.25f), V(0.25f)));
                v.FillBox(glm::ivec3(0), v.Size(), m.wood);
                return v;
            }
            case voxel::FortProp::Ladder: {
                // 2 rails and a rung every 0.375 m, lying flat.
                const glm::ivec3 size(V(3.0f), 1, V(0.5f));
                voxel::VoxelVolume v(size);
                for (int x = 0; x < size.x; ++x)
                    for (int z = 0; z < size.z; ++z)
                        if (z == 0 || z == size.z - 1 || x % V(0.375f) == 0) v.Set(x, 0, z, m.wood);
                return v;
            }
            case voxel::FortProp::Crate: {
                const glm::ivec3 size(V(0.75f));
                voxel::VoxelVolume v(size);
                for (int z = 0; z < size.z; ++z)
                    for (int y = 0; y < size.y; ++y)
                        for (int x = 0; x < size.x; ++x) {
                            const int edges = int(x == 0 || x == size.x - 1) + int(y == 0 || y == size.y - 1) + int(z == 0 || z == size.z - 1);
                            v.Set(x, y, z, edges >= 2 ? m.wood : m.planks);
                        }
                return v;
            }
            case voxel::FortProp::Barrel: {
                const glm::ivec3 size(V(0.625f), V(0.875f), V(0.625f));
                voxel::VoxelVolume v(size);
                const float r = 0.5f * float(size.x);
                for (int z = 0; z < size.z; ++z)
                    for (int y = 0; y < size.y; ++y)
                        for (int x = 0; x < size.x; ++x) {
                            const float dx = float(x) + 0.5f - r, dz = float(z) + 0.5f - r;
                            if (dx * dx + dz * dz > r * r) continue;
                            v.Set(x, y, z, y == 1 || y == size.y - 2 ? m.iron : m.wood);   // 2 iron hoops
                        }
                return v;
            }
            case voxel::FortProp::Table: {
                // A plank top 0.8 m up on 4 legs.
                const glm::ivec3 size(V(2.5f), V(0.8f), V(0.875f));
                voxel::VoxelVolume v(size);
                v.FillBox(glm::ivec3(0, size.y - 1, 0), size, m.planks);
                for (const int x : {1, size.x - 2})
                    for (const int z : {1, size.z - 2}) v.FillBox(glm::ivec3(x, 0, z), glm::ivec3(x + 1, size.y - 1, z + 1), m.wood);
                return v;
            }
            case voxel::FortProp::Cart:
            case voxel::FortProp::HayWagon: {
                // A plank bed with low sides on 2 wheels; the hay wagon carries a load of hay.
                const bool hay = kind == voxel::FortProp::HayWagon;
                const glm::ivec3 size(V(2.0f), V(hay ? 1.75f : 0.875f), V(1.25f));
                voxel::VoxelVolume v(size);
                const int bed = V(0.375f), rim = V(0.75f), wheelR = V(0.375f), hub = size.x / 2;
                for (int z = 1; z < size.z - 1; ++z)
                    for (int x = 0; x < size.x; ++x) {
                        v.Set(x, bed, z, m.planks);
                        const bool side = x == 0 || x == size.x - 1 || z == 1 || z == size.z - 2;
                        for (int y = bed + 1; y <= rim && y < size.y; ++y)
                            if (side) v.Set(x, y, z, m.planks);
                            else if (hay) v.Set(x, y, z, m.thatch);
                        if (hay)
                            for (int y = rim + 1; y < size.y; ++y)
                                if (x > 0 && x < size.x - 1) v.Set(x, y, z, m.thatch);
                    }
                // A leg under the front end: parked on it and the wheels, it cannot tip onto its bed.
                for (int z = size.z / 2 - 1; z <= size.z / 2; ++z)
                    for (int y = 0; y < bed; ++y) v.Set(1, y, z, m.wood);
                for (const int z : {0, size.z - 1})
                    for (int y = 0; y <= 2 * wheelR && y < size.y; ++y)
                        for (int x = hub - wheelR; x <= hub + wheelR; ++x) {
                            const int dx = x - hub, dy = y - wheelR;
                            if (x >= 0 && x < size.x && dx * dx + dy * dy <= wheelR * wheelR) v.Set(x, y, z, m.wood);
                        }
                return v;
            }
            }
            return voxel::VoxelVolume(glm::ivec3(1));
        }

        /// The same model turned a quarter about +y (its x along z).
        voxel::VoxelVolume AlongZ(const voxel::VoxelVolume& a) {
            const glm::ivec3 s = a.Size();
            voxel::VoxelVolume b(glm::ivec3(s.z, s.y, s.x));
            for (int z = 0; z < s.z; ++z)
                for (int y = 0; y < s.y; ++y)
                    for (int x = 0; x < s.x; ++x) b.Set(z, y, x, a.Get(x, y, z));
            return b;
        }
    }

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
        // middle), rising 3.5 m into the guardroom; the lever on the guardroom's floor runs it.
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

        const glm::ivec3 leverSize(2, 6, 2);
        voxel::VoxelVolume stick(leverSize);
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 6; ++y)
                for (int x = 0; x < 2; ++x) stick.Set(x, y, z, m.wood);
        TemplateMechanism lever;
        lever.model = std::make_shared<voxel::VoxelModel>(std::move(stick), voxel::kWorldPalette, "hollowkeep lever");
        lever.spawnVoxel = glm::ivec3(V(plan.guardLever.x) - 1, V(plan.guardLever.y), V(plan.guardLever.z) - 1);
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

        for (const voxel::FortLight& l : plan.lights)
            t.lights.push_back({.position = glm::dvec3(l.at), .color = l.color, .intensity = l.intensity, .radius = l.radius});

        // The props: each dropped from 1.5 m over its spot onto the first solid voxels under its footprint, kept only when
        // its whole box is clear there, it lands within 1.5 m under the spot (not inside a wall, not down a hole) and it
        // stands level.
        {
            GeneratedVoxels voxels(voxel::CreateLandGenerator(std::make_shared<const voxel::LandMap>(map), land));
            std::map<std::pair<int, bool>, std::shared_ptr<voxel::VoxelModel>> models;
            std::vector<std::pair<glm::ivec3, glm::ivec3>> placed;   // the props made so far (voxel boxes): the next lands on them
            const auto solid = [&](const glm::ivec3& v) {
                for (const auto& [lo, hi] : placed)
                    if (glm::all(glm::greaterThanEqual(v, lo)) && glm::all(glm::lessThan(v, hi))) return true;
                const uint8_t id = voxels.At(v);
                return id != 0 && id != m.water;
            };
            for (const voxel::FortPropSpot& spot : plan.props) {
                std::shared_ptr<voxel::VoxelModel>& model = models[{int(spot.kind), spot.alongX}];
                if (!model) {
                    voxel::VoxelVolume v = PropVolume(spot.kind, m, vs);
                    model = std::make_shared<voxel::VoxelModel>(spot.alongX ? std::move(v) : AlongZ(v), voxel::kWorldPalette, "hollowkeep prop");
                }
                const glm::ivec3 s = model->Volume().Size();
                const glm::ivec3 o(V(spot.at.x) - s.x / 2, 0, V(spot.at.z) - s.z / 2);
                const auto layerClear = [&](int y) {
                    for (int z = 0; z < s.z; ++z)
                        for (int x = 0; x < s.x; ++x)
                            if (solid(glm::ivec3(o.x + x, y, o.z + z))) return false;
                    return true;
                };
                int y = V(spot.at.y + 1.5f);
                const int lowest = V(spot.at.y - 1.5f);
                bool clear = true;
                for (int k = 0; k < s.y && clear; ++k) clear = layerClear(y + k);
                if (!clear) continue;
                while (y > lowest && layerClear(y - 1)) --y;
                if (y <= lowest) continue;
                // Level support: every corner of the footprint stands on something within 2 voxels (not on a slope it would
                // slide down).
                bool level = true;
                for (const int cz : {std::min(1, s.z - 1), std::max(0, s.z - 2)})
                    for (const int cx : {std::min(1, s.x - 1), std::max(0, s.x - 2)}) {
                        const glm::ivec3 c(o.x + cx, y, o.z + cz);
                        level = level && (solid(c - glm::ivec3(0, 1, 0)) || solid(c - glm::ivec3(0, 2, 0)));
                    }
                if (!level) continue;
                placed.emplace_back(glm::ivec3(o.x, y, o.z), glm::ivec3(o.x, y, o.z) + s);
                t.objects.push_back({.model = model, .position = glm::dvec3(glm::vec3(float(o.x), float(y), float(o.z)) * vs)});
            }
        }

        // The player arrives on the castle road, past the bridge over the ditch.
        const glm::vec2 arrive = gatePoint + axis * (plan.bridgeSpan + 2.0f);
        t.spawn = glm::dvec3(arrive.x, voxel::LandHeight(map, land, arrive.x, arrive.y) + 0.1f, arrive.y);
        t.anchor = glm::ivec3(V(plan.keep.x), V(plan.keepGround), V(plan.keep.y));
        return t;
    }
}
