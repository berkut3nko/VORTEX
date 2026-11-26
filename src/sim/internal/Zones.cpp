module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

module vortex.sim;

import vortex.voxel;

namespace vortex::sim {

    namespace {
        int32_t Clamp32(int64_t v) {
            return static_cast<int32_t>(std::clamp<int64_t>(v, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
        }

        glm::ivec3 Offset(const glm::ivec3& p, int64_t dx, int64_t dy, int64_t dz) {
            return {Clamp32(int64_t{p.x} + dx), Clamp32(int64_t{p.y} + dy), Clamp32(int64_t{p.z} + dz)};
        }

        bool Before(const Zone& a, const Zone& b) {
            return a.startTick != b.startTick ? a.startTick < b.startTick : a.zoneId < b.zoneId;
        }
    }

    void ZoneBoundsQ(const Zone& zone, glm::ivec3& minQ, glm::ivec3& maxQ) {
        const voxel::ShapeQ& s = zone.shape;
        const int64_t r = s.radiusQ;
        switch (s.kind) {
        case voxel::kShapeCapsule:
            minQ = Offset(glm::min(s.a, s.b), -r, -r, -r);
            maxQ = Offset(glm::max(s.a, s.b), r, r, r);
            return;
        case voxel::kShapeBox:
            minQ = Offset(glm::ivec3(0), int64_t{s.a.x} * 16, int64_t{s.a.y} * 16, int64_t{s.a.z} * 16);
            maxQ = Offset(glm::ivec3(0), int64_t{s.b.x} * 16 - 1, int64_t{s.b.y} * 16 - 1, int64_t{s.b.z} * 16 - 1);
            return;
        case voxel::kShapeCylinder:
            minQ = Offset(s.a, -r, -int64_t{s.halfHeightQ}, -r);
            maxQ = Offset(s.a, r, s.halfHeightQ, r);
            return;
        default:   // a sphere
            minQ = Offset(s.a, -r, -r, -r);
            maxQ = Offset(s.a, r, r, r);
            return;
        }
    }

    bool ZoneSet::Insert(const Zone& zone) {
        if (m_Zones.size() >= kMaxZoneRecords || Find(zone.zoneId) != nullptr) return false;
        m_Zones.insert(std::upper_bound(m_Zones.begin(), m_Zones.end(), zone, Before), zone);
        return true;
    }

    bool ZoneSet::Cancel(uint16_t zoneId, uint32_t endTick) {
        for (Zone& z : m_Zones) {
            if (z.zoneId != zoneId) continue;
            if (endTick <= z.startTick)
                z.duration = 0;
            else
                z.duration = static_cast<uint16_t>(std::min<uint32_t>(z.duration, endTick - z.startTick));
            return true;
        }
        return false;
    }

    const Zone* ZoneSet::Find(uint16_t zoneId) const {
        for (const Zone& z : m_Zones)
            if (z.zoneId == zoneId) return &z;
        return nullptr;
    }

    void ZoneSet::PruneEndedBefore(uint32_t tick) {
        std::erase_if(m_Zones, [tick](const Zone& z) { return z.EndTick() <= tick; });
    }

    size_t ZoneSet::LiveOrPendingAt(uint32_t tick) const {
        return static_cast<size_t>(std::count_if(m_Zones.begin(), m_Zones.end(), [tick](const Zone& z) { return z.EndTick() > tick; }));
    }
}
