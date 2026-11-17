module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.net;

import vortex.voxel;

namespace vortex::net {

    std::string EntityComparison::Describe() const {
        return std::format("{} / {} object(s), {} missing, {} extra, {} model mismatch(es), {} rest-state mismatch(es); {} resting on "
                           "both within {:.4f} m / {:.5f} rad, {} moving within {:.2f} m",
                           server, client, missing, extra, modelMismatches, restStateMismatches, restingCompared, maxRestError, maxRestAngle,
                           movingCompared, maxMovingError);
    }

    EntityComparison CompareEntities(std::span<const EntityInfo> server, std::span<const EntityInfo> client) {
        EntityComparison c;
        c.server = server.size();
        c.client = client.size();
        std::unordered_map<uint32_t, const EntityInfo*> theirs;
        for (const EntityInfo& e : client) theirs[e.netId] = &e;
        for (const EntityInfo& s : server) {
            const auto it = theirs.find(s.netId);
            if (it == theirs.end()) {
                ++c.missing;
                continue;
            }
            const EntityInfo& k = *it->second;
            theirs.erase(it);
            if (k.modelHash != s.modelHash) ++c.modelMismatches;
            const float d = glm::distance(k.position, s.position);
            if (s.resting != k.resting) ++c.restStateMismatches;
            if (s.resting && k.resting) {
                ++c.restingCompared;
                c.maxRestError = std::max(c.maxRestError, d);
                // The angle between the two rotations (q and -q are the same rotation), in double and
                // through atan2 of the relative rotation: acos of a float dot product near 1 reads a few
                // ulps of normalisation as 0.7-1.4 mrad (stage 7b: an arena run failed the 1 mrad
                // tolerance on two rotations that agree to float precision).
                const glm::dquat a = glm::normalize(glm::dquat(s.rotation)), b = glm::normalize(glm::dquat(k.rotation));
                const glm::dquat r = glm::conjugate(a) * b;
                const double angle = 2.0 * std::atan2(glm::length(glm::dvec3(r.x, r.y, r.z)), std::abs(r.w));
                c.maxRestAngle = std::max(c.maxRestAngle, static_cast<float>(angle));
            } else {
                ++c.movingCompared;
                c.maxMovingError = std::max(c.maxMovingError, d);
            }
        }
        c.extra = static_cast<uint32_t>(theirs.size());
        return c;
    }

    uint64_t EntityListHash(std::span<const EntityInfo> entities) {
        std::vector<std::pair<uint32_t, uint64_t>> ids;
        ids.reserve(entities.size());
        for (const EntityInfo& e : entities) ids.emplace_back(e.netId, e.modelHash);
        std::sort(ids.begin(), ids.end());
        uint64_t h = voxel::HashMix64(0x656e74697479ull, ids.size());
        for (const auto& [id, model] : ids) h = voxel::HashMix64(voxel::HashMix64(h, id), model);
        return h;
    }
}
