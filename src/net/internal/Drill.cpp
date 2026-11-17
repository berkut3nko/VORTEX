module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

module vortex.net;

import vortex.core;
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
        m_EdgeX = lo.x;
        m_Columns = nx;
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
        step.clearMin = o + glm::ivec3(0, -4, 0);                      // ... and the bodies it left (stage 6)
        step.clearMax = o + glm::ivec3(kSiteVoxels, kSiteHeight, kSiteVoxels);
        step.crates.push_back(o + glm::ivec3(41, 2, 41));              // beside the structure, in the blast
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
        if (index < Sites()) {
            // The site's own crate, in the margin at the play bounds' -x edge (outside every site's
            // box), half a metre above the site's ground: it falls, rests and stays for the whole drill.
            step.crates.push_back(glm::ivec3(m_EdgeX + 2, o.y + 4, o.z + 10 + 12 * (static_cast<int>(step.site) % m_Columns)));
        }
        return step;
    }

    bool RunDrillStep(Server& server, const DrillStep& step) {
        const voxel::World& world = server.Simulation().World();
        server.ClearBodies(world.VoxelToWorld(step.clearMin), world.VoxelToWorld(step.clearMax));
        for (const sim::WorldOp& op : step.build) server.ApplyOp(op);
        for (const glm::ivec3& crate : step.crates) server.SpawnCrate(world.VoxelToWorld(crate));
        return server.QueueCast(step.cast, step.power);
    }

    BotPilot::BotPilot(const sim::RegionDesc& region, uint64_t seed, PilotSettings settings)
        : m_Settings(settings), m_Rng(seed, 0x9e3779b97f4a7c15ull) {
        const float chunk = static_cast<float>(voxel::kChunkSize) * region.terrain.voxelSize;
        m_Min = glm::vec2(region.playMinChunk) * chunk + glm::vec2(settings.edgeMargin);
        m_Max = glm::vec2(region.playMaxChunk) * chunk - glm::vec2(settings.edgeMargin);
        m_Max = glm::max(m_Max, m_Min);
    }

    sim::PlayerInput BotPilot::Next(const sim::PlayerState& state) {
        sim::PlayerInput in;
        ++m_Tick;
        const glm::vec2 feet(state.position.x, state.position.z);
        if (!m_Settings.waypoints.empty()) {
            // Waypoint mode: run to the next point; stand once the last is behind.
            while (m_Waypoint < m_Settings.waypoints.size()) {
                const float d = glm::length(m_Settings.waypoints[m_Waypoint] - feet);
                if (d <= m_Settings.reachMetres) {
                    ++m_Reached;
                } else if (m_SinceProgress >= m_Settings.stuckTicks) {
                    ++m_GivenUp;
                } else {
                    if (m_SinceProgress == 0 && m_BestDistance == 0.0f) m_BestDistance = d;
                    if (d < m_BestDistance - 1.0f) {
                        m_BestDistance = d;
                        m_SinceProgress = 0;
                    } else {
                        ++m_SinceProgress;
                    }
                    break;
                }
                ++m_Waypoint;
                m_BestDistance = 0.0f;
                m_SinceProgress = 0;
            }
            if (Done()) return in;
            const glm::vec2 to = m_Settings.waypoints[m_Waypoint] - feet;
            in.yaw = sim::QuantizeYaw(std::atan2(-to.x, -to.y));
            in.moveZ = 127;
            in.buttons = m_Settings.run ? sim::PlayerButton_Run : 0;
            // Jump now and then, and whenever it stops making progress (a step, a bank).
            const bool stuck = m_SinceProgress > 30 && m_SinceProgress % 30 < 2;
            if ((m_Settings.jumps && m_Tick >= m_NextJump) || stuck) {
                in.buttons |= sim::PlayerButton_Jump;
                if (m_Tick >= m_NextJump + 1) {
                    m_NextJump = m_Tick + 60 + m_Rng.NextBounded(61);
                    ++m_Jumps;
                }
            }
            return in;
        }
        if (m_SegmentLeft == 0) {
            m_SegmentLeft = 20 + m_Rng.NextBounded(61);
            m_Yaw = static_cast<uint16_t>(m_Rng.NextU32());
            m_Run = m_Settings.run && m_Rng.NextBounded(2) == 0;
            const uint32_t strafe = m_Settings.strafe ? m_Rng.NextBounded(4) : 0;
            m_Strafe = static_cast<int8_t>(strafe == 1 ? 127 : strafe == 2 ? -127 : 0);
        }
        --m_SegmentLeft;
        // Outside the inner bounds: head for the middle (and keep heading there for a while).
        if (feet.x < m_Min.x || feet.y < m_Min.y || feet.x > m_Max.x || feet.y > m_Max.y) {
            const glm::vec2 to = 0.5f * (m_Min + m_Max) - feet;
            m_Yaw = sim::QuantizeYaw(std::atan2(-to.x, -to.y));
            m_Strafe = 0;
            m_SegmentLeft = std::max<uint32_t>(m_SegmentLeft, 30);
        }
        in.moveZ = 127;
        in.moveX = m_Strafe;
        in.yaw = m_Yaw;
        in.buttons = m_Run ? sim::PlayerButton_Run : 0;
        if (m_Settings.jumps && m_Tick >= m_NextJump) {
            in.buttons |= sim::PlayerButton_Jump;               // held for two ticks: one edge
            if (m_Tick >= m_NextJump + 1) {
                m_NextJump = m_Tick + 60 + m_Rng.NextBounded(61);
                ++m_Jumps;
            }
        }
        if (m_Settings.casts && m_Settings.castEveryTicks > 0) {
            const uint32_t phase = m_Tick % m_Settings.castEveryTicks;
            if (phase < 4) in.pitch = sim::QuantizePitch(m_Settings.castPitch);   // look down first, then press
            if (phase == 2 || phase == 3) in.buttons |= sim::PlayerButton_Cast;
            if (phase == 3) ++m_Casts;
        }
        if (m_Settings.crateEveryTicks > 0) {
            const uint32_t phase = (m_Tick + m_Settings.crateEveryTicks / 2) % m_Settings.crateEveryTicks;
            if (phase < 2) in.buttons |= sim::PlayerButton_Crate;    // held for two ticks: one edge
            if (phase == 1) ++m_Crates;
        }
        return in;
    }
}
