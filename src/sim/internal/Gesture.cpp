module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>

module vortex.sim;

import vortex.math;

namespace vortex::sim {

    Form AutoFormOf(GestureKind kind) {
        switch (kind) {
        case GestureKind::Rise: return Form::Plume;
        case GestureKind::Fall: return Form::Flow;
        case GestureKind::Line: return Form::Burst;
        case GestureKind::Tap: break;
        }
        return Form::Pierce;
    }

    Form FormOfGesture(const SpellGesture& gesture) {
        return gesture.form < kAutoForm ? static_cast<Form>(gesture.form) : AutoFormOf(gesture.kind);
    }

    uint32_t GesturePathQ(std::span<const glm::ivec3> pointsQ) {
        uint64_t path = 0;
        for (size_t i = 1; i < pointsQ.size(); ++i) {
            const glm::i64vec3 d = glm::i64vec3(pointsQ[i]) - glm::i64vec3(pointsQ[i - 1]);
            path += math::ISqrt64(static_cast<uint64_t>(d.x * d.x + d.y * d.y + d.z * d.z));
        }
        return static_cast<uint32_t>(std::min<uint64_t>(path, 0xFFFFFFFFu));
    }

    GestureKind ClassifyGesture(std::span<const glm::ivec3> pointsQ, std::span<const GestureAngles> angles, uint32_t heldTicks,
                                uint32_t metreQ) {
        const uint32_t path = GesturePathQ(pointsQ);
        int64_t pitchChange = 0, pitchSweep = 0, yawSweep = 0;
        if (!angles.empty()) {
            pitchChange = int64_t{angles.back().pitch} - int64_t{angles.front().pitch};
            int64_t pMin = angles.front().pitch, pMax = pMin;
            int64_t yaw = 0, yMin = 0, yMax = 0;
            for (size_t i = 0; i < angles.size(); ++i) {
                pMin = std::min<int64_t>(pMin, angles[i].pitch);
                pMax = std::max<int64_t>(pMax, angles[i].pitch);
                if (i > 0) {
                    // The shortest turn between two samples (yaw wraps at 65536).
                    yaw += static_cast<int16_t>(static_cast<uint16_t>(angles[i].yaw - angles[i - 1].yaw));
                    yMin = std::min(yMin, yaw);
                    yMax = std::max(yMax, yaw);
                }
            }
            pitchSweep = pMax - pMin;
            yawSweep = yMax - yMin;
        }
        if (heldTicks < kGestureTapTicks || (path < metreQ && std::llabs(pitchChange) < kGesturePitchUnits)) return GestureKind::Tap;
        // Degrees: pitch units x 90 / 32767 against yaw units x 360 / 65536.
        const bool pitchLeads = pitchSweep * 65536 > yawSweep * 4 * 32767;
        if (pitchChange >= kGesturePitchUnits && pitchLeads) return GestureKind::Rise;
        if (pitchChange <= -kGesturePitchUnits && pitchLeads) return GestureKind::Fall;
        if (path >= metreQ) return GestureKind::Line;
        return GestureKind::Tap;
    }
}
