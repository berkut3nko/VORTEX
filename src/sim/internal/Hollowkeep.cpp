module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

module vortex.sim;

import vortex.voxel;
import vortex.scene;

// Hollowkeep's greybox on the baked map (engine/docs/design/land.md "Hollowkeep's greybox"): boxes only, built on the
// natural slope. Every wall, tower and building stands on its own ground and reaches 1.5 m below it; nothing raises the
// terrain. The bake leveled only the keep's and the gatehouse's pads.

namespace vortex::sim {

    namespace {
        constexpr float kFoundation = 1.5f;   // metres every structure reaches below its ground
        constexpr float kCurtainWalk = 8.8f, kInnerWalk = 8.0f, kParapet = 1.2f;
    }

    RegionTemplate HollowkeepTemplate(const voxel::LandMap& map, const voxel::LandGeneratorSettings& land) {
        RegionTemplate t;
        const voxel::LandSite& site = map.site;
        if (!site.placed) {
            t.sited = false;
            return t;
        }
        const voxel::WorldMaterials& m = land.materials;
        const float vs = land.voxelSize;
        const auto V = [vs](float metres) { return int(std::floor(metres / vs)); };
        const glm::vec2 keep = land.origin + glm::vec2(site.x, site.z);
        const auto ground = [&](glm::vec2 p) { return voxel::LandHeight(map, land, p.x, p.y); };
        // A box in world metres (min corner inclusive, max exclusive, at least one voxel per axis).
        const auto box = [&](glm::vec3 lo, glm::vec3 hi, uint8_t id) {
            const glm::ivec3 a(V(lo.x), V(lo.y), V(lo.z));
            const glm::ivec3 b = glm::max(glm::ivec3(V(hi.x), V(hi.y), V(hi.z)), a + 1);
            t.ops.push_back(WorldOp::Box(a, b, id));
        };
        const auto column = [&](glm::vec2 p, float half, float bottom, float top, uint8_t id) {
            box(glm::vec3(p.x - half, bottom, p.y - half), glm::vec3(p.x + half, top, p.y + half), id);
        };
        const auto vertex = [&](const std::array<float, voxel::kSiteDirections>& radii, int k) {
            const int i = ((k % voxel::kSiteDirections) + voxel::kSiteDirections) % voxel::kSiteDirections;
            return keep + voxel::LandSiteDirection(i) * radii[size_t(i)];
        };
        const glm::vec2 gd = voxel::LandSiteDirection(site.gate);
        const bool alongX = std::abs(gd.x) >= std::abs(gd.y);
        // The gate's axis, squared to the grid: the gatehouse, its passage and the portcullis are axis-aligned.
        const glm::vec2 axis = voxel::LandSiteGateAxis(site);
        const glm::vec2 across(-axis.y, axis.x);
        const glm::vec2 gatePoint = vertex(site.curtain, site.gate);
        const float gateGround = ground(gatePoint);
        const glm::vec2 innerGate = vertex(site.inner, site.gate);

        // A wall along a closed polygon: one column of stone every 0.5 m from 1.5 m under the ground to the run's wall walk
        // (the highest ground of the run plus the walk height: the walk steps down the slope run by run), a battered dark
        // plinth, and a parapet with merlons every 2 m on the outer side. `gap`: no wall within that distance of `gapAt`.
        const auto wall = [&](const std::array<float, voxel::kSiteDirections>& radii, float half, float walk, glm::vec2 gapAt, float gap) {
            for (int k = 0; k < voxel::kSiteDirections; ++k) {
                const glm::vec2 a = vertex(radii, k), b = vertex(radii, k + 1);
                const float length = glm::length(b - a);
                const int steps = std::max(1, int(std::ceil(length / 0.5f)));
                float runTop = -1.0e9f;
                for (int s = 0; s <= steps; ++s) runTop = std::max(runTop, ground(a + (b - a) * (float(s) / float(steps))));
                runTop += walk;
                for (int s = 0; s <= steps; ++s) {
                    const glm::vec2 p = a + (b - a) * (float(s) / float(steps));
                    if (glm::length(p - gapAt) < gap) continue;
                    const float g = ground(p);
                    column(p, half, g - kFoundation, runTop, m.stone);
                    column(p, half + 0.5f, g - kFoundation, g + 1.2f, m.stoneDark);           // the battered plinth
                    const glm::vec2 out = glm::normalize(p - keep);
                    const glm::vec2 q = p + out * (half - 0.3f);
                    const bool merlon = (s / 4) % 2 == 0;
                    column(q, 0.3f, runTop, runTop + (merlon ? kParapet + 0.6f : kParapet), m.stone);
                }
            }
        };

        // 1. The ditch across the neck: one trench outside the curtain's 4 edges around the gate, 4-10 m out along the
        // ray from the keep. Its floor is level, 5 m under the gate's ground, 3 m wide, with 1:1 sides; where the ground
        // already lies at the trench's depth nothing is dug. It is cut on a 0.5 m grid of 0.6 m columns, so no ground is
        // left standing between the cuts. A level timber bridge on the gate's line spans it from the gatehouse to 1 m
        // past the trench (`voxel::LandSiteBridgeSpan`), on two trestles; the castle road ends there at the same level
        // (a drawbridge waits for [mech-3]).
        const float span = voxel::LandSiteBridgeSpan(site);
        const glm::vec2 arrive = gatePoint + axis * (span + 2.0f);   // the player arrives on the road, past the bridge
        {
            // How far p lies outside the curtain along the ray from the keep, in the 4 sectors around the gate.
            const auto outside = [&](glm::vec2 p, float& off) {
                return voxel::LandSiteOutsideCurtain(site, p.x - land.origin.x, p.y - land.origin.y, off);
            };
            const float floor = gateGround - 5.0f;
            glm::vec2 lo(1.0e9f), hi(-1.0e9f);
            for (int k = -2; k <= 2; ++k) {
                const glm::vec2 v = vertex(site.curtain, site.gate + k), out = voxel::LandSiteDirection(site.gate + k);
                lo = glm::min(lo, glm::min(v, v + out * 11.0f));
                hi = glm::max(hi, glm::max(v, v + out * 11.0f));
            }
            lo = glm::floor(lo - 2.0f);
            hi = glm::ceil(hi + 2.0f);
            for (float z = lo.y; z <= hi.y; z += 0.5f)
                for (float x = lo.x; x <= hi.x; x += 0.5f) {
                    const glm::vec2 p(x, z);
                    float off = 0.0f;
                    if (!outside(p, off) || off < 4.0f || off > 10.0f) continue;
                    const float g = ground(p);
                    const float bottom = floor + std::max(0.0f, std::abs(off - 7.0f) - 1.5f);
                    if (g <= bottom + 0.25f) continue;
                    column(p, 0.3f, bottom, g + 2.0f, 0);
                }

            // The bridge: from 3 m out of the gate to 1 m past the trench's outer edge, level with the gate.
            const glm::vec2 width = glm::abs(across) * 2.0f;
            const glm::vec2 d0 = glm::min(gatePoint + axis * 3.0f, gatePoint + axis * span) - width;
            const glm::vec2 d1 = glm::max(gatePoint + axis * 3.0f, gatePoint + axis * span) + width;
            box(glm::vec3(d0.x, gateGround - 0.3f, d0.y), glm::vec3(d1.x, gateGround, d1.y), m.planks);
            // Open to the sky over the deck: where the far bank rises, the way is a cutting, not a tunnel.
            const auto groundTop = [&](glm::vec2 c) {
                float top = ground(c);
                for (const float side : {-2.0f, -1.0f, 1.0f, 2.0f}) top = std::max(top, ground(c + across * side));
                return top + 1.0f;
            };
            for (float t = 3.0f; t < span; t += 0.5f) {
                const glm::vec2 a = gatePoint + axis * t, b = gatePoint + axis * std::min(span, t + 0.5f);
                const glm::vec2 c0 = glm::min(a, b) - width, c1 = glm::max(a, b) + width;
                const float top = std::max(gateGround + 3.0f, groundTop((a + b) * 0.5f));
                box(glm::vec3(c0.x, gateGround, c0.y), glm::vec3(c1.x, top, c1.y), 0);
            }
            for (const float t : {0.4f, 0.7f}) {
                const glm::vec2 c = gatePoint + axis * (3.0f + (span - 3.0f) * t);
                for (const float side : {-1.6f, 1.6f}) column(c + across * side, 0.25f, floor - 0.5f, gateGround - 0.3f, m.wood);
                column(c, 0.25f, gateGround - 1.3f, gateGround - 0.3f, m.wood);
            }
        }

        // 2. The curtain and the inner wall, gaps at the gatehouse and the inner gate.
        wall(site.curtain, 1.25f, kCurtainWalk, gatePoint, 7.5f);
        wall(site.inner, 1.0f, kInnerWalk, innerGate, 1.6f);

        // 3. Towers at every third corner of the curtain (none beside the gate): 6 x 6 m, 14 m over their highest ground,
        // hollow from the bailey's ground up, two timber floors, a door on the bailey side.
        for (int k = 0; k < voxel::kSiteDirections; k += 3) {
            const int dk = std::abs(((k - site.gate) % voxel::kSiteDirections + voxel::kSiteDirections + 12) % voxel::kSiteDirections - 12);
            if (dk <= 1) continue;
            const glm::vec2 c = vertex(site.curtain, k);
            float lo = 1.0e9f, hi = -1.0e9f;
            for (const glm::vec2 o : {glm::vec2(0.0f), glm::vec2(3.0f, 3.0f), glm::vec2(-3.0f, 3.0f), glm::vec2(3.0f, -3.0f), glm::vec2(-3.0f, -3.0f)}) {
                lo = std::min(lo, ground(c + o));
                hi = std::max(hi, ground(c + o));
            }
            const glm::vec2 in = glm::normalize(keep - c);
            const float floorLevel = ground(c + in * 3.5f);
            column(c, 3.0f, lo - kFoundation, hi + 14.0f, m.stone);
            column(c, 3.5f, lo - kFoundation, lo + 1.2f, m.stoneDark);
            column(c, 2.0f, floorLevel, hi + 13.0f, 0);
            column(c, 2.0f, floorLevel + 4.5f, floorLevel + 4.75f, m.planks);
            column(c, 2.0f, floorLevel + 9.0f, floorLevel + 9.25f, m.planks);
            column(c + in * 2.5f, 1.0f, floorLevel, floorLevel + 2.4f, 0);
        }

        // 4. The gatehouse: 14 m across the gate and 8 m along it, 14 m high, on its pad; a 4 m passage, 4.5 m high; the
        // timber upper room (N3) over it; the portcullis slot by the outer face.
        {
            const glm::vec2 ext = glm::abs(across) * 7.0f + glm::abs(axis) * 4.0f;
            const glm::vec2 lo = gatePoint - ext, hi = gatePoint + ext;
            box(glm::vec3(lo.x, gateGround - kFoundation, lo.y), glm::vec3(hi.x, gateGround + 4.0f, hi.y), m.stoneDark);
            box(glm::vec3(lo.x, gateGround + 4.0f, lo.y), glm::vec3(hi.x, gateGround + 14.0f, hi.y), m.stone);
            const glm::vec2 pass = glm::abs(across) * 2.0f + glm::abs(axis) * 5.0f;
            box(glm::vec3(gatePoint.x - pass.x, gateGround, gatePoint.y - pass.y), glm::vec3(gatePoint.x + pass.x, gateGround + 4.5f, gatePoint.y + pass.y), 0);
            const glm::vec2 room = glm::abs(across) * 5.0f + glm::abs(axis) * 3.0f;
            box(glm::vec3(gatePoint.x - room.x, gateGround + 5.0f, gatePoint.y - room.y), glm::vec3(gatePoint.x + room.x, gateGround + 13.0f, gatePoint.y + room.y), 0);
            box(glm::vec3(gatePoint.x - room.x, gateGround + 4.5f, gatePoint.y - room.y), glm::vec3(gatePoint.x + room.x, gateGround + 5.0f, gatePoint.y + room.y),
                m.planks);
            // The slot: 2.5 m out from the middle, 4.5 m wide, 4 m above the passage.
            const glm::vec2 slot = gatePoint + axis * 2.5f;
            const glm::vec2 sext = glm::abs(across) * 2.25f + glm::abs(axis) * 0.2f;
            box(glm::vec3(slot.x - sext.x, gateGround + 4.5f, slot.y - sext.y), glm::vec3(slot.x + sext.x, gateGround + 8.5f, slot.y + sext.y), 0);

            // The portcullis ([mech-1]): a 4 x 4 m iron lattice in the slot's plane, rising 3.5 m; the lever on a post 6 m
            // inside the gate, beside the passage, runs it.
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
            const float postGround = ground(post);
            column(post, 0.15f, postGround - 0.5f, postGround + 1.0f, m.wood);
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
        }

        // 5. The keep on its pad: 16 x 16 m, 22 m high; walls 3 m thick, dark stone to 8 m and brick above; the sanctum's
        // keel floor (bedrock) and timber floors at 5, 11 and 17 m; a door towards the gate; a crenellated top.
        {
            const float g0 = site.keepGround;
            column(keep, 8.0f, g0 - kFoundation, g0 + 8.0f, m.stoneDark);
            column(keep, 8.0f, g0 + 8.0f, g0 + 22.0f, m.brick);
            column(keep, 5.0f, g0, g0 + 21.0f, 0);
            column(keep, 5.0f, g0 - 0.5f, g0, m.bedrock);
            for (const float f : {5.0f, 11.0f, 17.0f}) column(keep, 5.0f, g0 + f, g0 + f + 0.25f, m.planks);
            const glm::vec2 door = keep + axis * 6.5f;
            const glm::vec2 dext = glm::abs(across) * 1.0f + glm::abs(axis) * 2.0f;
            box(glm::vec3(door.x - dext.x, g0, door.y - dext.y), glm::vec3(door.x + dext.x, g0 + 2.4f, door.y + dext.y), 0);
            for (int i = -4; i <= 4; i += 2)
                for (const glm::vec2 side : {axis, -axis, across, -across}) {
                    const glm::vec2 c = keep + side * 7.6f + glm::vec2(-side.y, side.x) * float(2 * i);
                    column(c, 0.4f, g0 + 22.0f, g0 + 23.0f, m.brick);
                }
        }

        // The player arrives on the approach, beyond the ditch.
        t.spawn = glm::dvec3(arrive.x, ground(arrive) + 0.1f, arrive.y);
        t.anchor = glm::ivec3(V(keep.x), V(site.keepGround), V(keep.y));
        return t;
    }
}
