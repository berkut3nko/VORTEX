module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    const char* ToString(UseResult result) {
        switch (result) {
            case UseResult::None: return "accepted";
            case UseResult::Stale: return "stale";
            case UseResult::NoTarget: return "out of reach";
            case UseResult::Locked: return "locked";
            case UseResult::Busy: return "busy";
            case UseResult::Broken: return "broken";
            case UseResult::NotUsable: return "not usable";
        }
        return "unknown";
    }

    namespace {
        constexpr std::array<uint32_t, 65> MakeEaseOut() {
            std::array<uint32_t, 65> t{};
            for (int64_t i = 0; i < 65; ++i) t[static_cast<size_t>(i)] = static_cast<uint32_t>(16 * (4096 - (64 - i) * (64 - i)));
            return t;
        }

        constexpr std::array<uint32_t, 65> MakeEaseSmooth() {
            std::array<uint32_t, 65> t{};
            for (int64_t i = 0; i < 65; ++i) t[static_cast<size_t>(i)] = static_cast<uint32_t>((96 * i * i - i * i * i) / 2);
            return t;
        }

        constexpr bool EaseTableIsValid(const std::array<uint32_t, 65>& t) {
            if (t[0] != 0 || t[64] != 65536) return false;
            for (size_t i = 1; i < t.size(); ++i)
                if (t[i] < t[i - 1]) return false;
            return true;
        }

        static_assert(EaseTableIsValid(MakeEaseOut()));
        static_assert(EaseTableIsValid(MakeEaseSmooth()));
        static_assert(MakeEaseOut()[1] == 2032 && MakeEaseOut()[32] == 49152 && MakeEaseOut()[63] == 65520);
        static_assert(MakeEaseSmooth()[1] == 47 && MakeEaseSmooth()[32] == 32768 && MakeEaseSmooth()[63] == 65488);

        int32_t AbsOf(int32_t v) { return v < 0 ? -v : v; }

        /// The newest segment with startTick <= tick, or nullptr when every segment starts later.
        const MechanismSegment* SegmentAt(const MechanismTrack& track, uint32_t tick) {
            for (uint32_t i = track.count; i > 0; --i)
                if (track.ring[i - 1].startTick <= tick) return &track.ring[i - 1];
            return nullptr;
        }

        bool BoxIsValid(const scene::MechanismBox& box) {
            for (int a = 0; a < 3; ++a)
                if (box.min[a] < 0 || box.min[a] >= box.max[a] || box.max[a] > 255) return false;
            return true;
        }

        bool Refuse(std::string* why, const char* reason) {
            if (why) *why = reason;
            return false;
        }
    }

    const std::array<uint32_t, 65> kEaseOut = MakeEaseOut();
    const std::array<uint32_t, 65> kEaseSmooth = MakeEaseSmooth();

    uint32_t Ease(const std::array<uint32_t, 65>& table, uint32_t k, uint32_t D) {
        const uint64_t f = static_cast<uint64_t>(k) * 64u;
        const uint64_t i = f / D;
        const uint64_t r = f % D;
        const int64_t lo = table[i];
        const int64_t hi = table[i + 1];
        return static_cast<uint32_t>(lo + (hi - lo) * static_cast<int64_t>(r) / static_cast<int64_t>(D));
    }

    const std::array<uint32_t, 65>& EaseTableFor(const MechanismDesc& desc, const MechanismSegment& segment) {
        if (desc.kind == MechanismKind::Portcullis && segment.targetPos > segment.startPos) return kEaseSmooth;
        return kEaseOut;
    }

    bool IsOpening(const MechanismDesc& desc, int16_t from, int16_t to) {
        if (desc.kind == MechanismKind::Door) return AbsOf(to) > AbsOf(from);
        return to > from;
    }

    uint16_t SegmentDuration(int16_t from, int16_t to, const MechanismDesc& desc) {
        if (from == to) return 0;
        const int64_t range = static_cast<int64_t>(desc.maxPos) - desc.minPos;
        if (range <= 0) return 0;
        const int64_t full = IsOpening(desc, from, to) ? desc.openTicks : desc.closeTicks;
        const int64_t delta = AbsOf(static_cast<int32_t>(to) - static_cast<int32_t>(from));
        const int64_t d = (delta * full + range - 1) / range;
        return static_cast<uint16_t>(std::min<int64_t>(d, 65535));
    }

    int16_t PositionAt(const Mechanism& m, uint32_t tick) {
        const MechanismSegment* s = SegmentAt(m.track, tick);
        if (!s) return m.track.ring[0].startPos;
        const uint32_t k = tick - s->startTick;
        if (k >= s->durationTicks) return s->targetPos;
        const int64_t e = Ease(EaseTableFor(m.desc, *s), k, s->durationTicks);
        return static_cast<int16_t>(s->startPos + (static_cast<int64_t>(s->targetPos) - s->startPos) * e / 65536);
    }

    bool IsMoving(const MechanismTrack& track, uint32_t tick) {
        const MechanismSegment* s = SegmentAt(track, tick);
        return s && s->durationTicks > 0 && tick < s->startTick + s->durationTicks;
    }

    uint32_t ArrivalTick(const MechanismTrack& track) {
        if (track.count == 0) return 0;
        return track.Newest().startTick + track.Newest().durationTicks;
    }

    void PushSegment(MechanismTrack& track, const MechanismSegment& segment) {
        constexpr uint8_t kSticky = scene::MechanismFlag_Locked | scene::MechanismFlag_Jammed | scene::MechanismFlag_Freed |
                                    scene::MechanismFlag_Broken;
        MechanismSegment s = segment;
        if (track.count > 0) s.flags = static_cast<uint8_t>(s.flags | (track.Newest().flags & kSticky));
        if (track.count == kMechanismSegments) {
            for (uint32_t i = 1; i < kMechanismSegments; ++i) track.ring[i - 1] = track.ring[i];
            track.ring[kMechanismSegments - 1] = s;
        } else {
            track.ring[track.count++] = s;
        }
    }

    bool ValidateMechanismDesc(const MechanismDesc& desc, const glm::ivec3& /*modelSize*/, std::string* why) {
        const uint8_t kind = static_cast<uint8_t>(desc.kind);
        if (kind < static_cast<uint8_t>(MechanismKind::Door) || kind > static_cast<uint8_t>(MechanismKind::Lever))
            return Refuse(why, "unknown kind");
        if (desc.minPos >= desc.maxPos) return Refuse(why, "minPos is not below maxPos");
        if (desc.axis > 5) return Refuse(why, "axis is not 0-5");
        if (desc.pushAxis > 5) return Refuse(why, "pushAxis is not 0-5");
        if (desc.autoReturnTicks != 0) return Refuse(why, "autoReturnTicks is not 0");
        if (desc.openTicks == 0 || desc.closeTicks == 0) return Refuse(why, "a full-travel time is 0");
        if (!BoxIsValid(desc.useBox)) return Refuse(why, "use box is empty or out of 0-255");
        if (!BoxIsValid(desc.blockBox)) return Refuse(why, "block box is empty or out of 0-255");
        if (!BoxIsValid(desc.hingeBox)) return Refuse(why, "hinge box is empty or out of 0-255");
        const bool usable = (desc.flags & scene::MechanismDesc_Usable) != 0;
        if (desc.kind == MechanismKind::Portcullis && usable) return Refuse(why, "a portcullis is not usable");
        if (desc.kind != MechanismKind::Portcullis && !usable) return Refuse(why, "a door or a lever must be usable");
        if (desc.anchorCount != 0 && desc.anchorCount != 2) return Refuse(why, "anchorCount is not 0 or 2");
        return true;
    }

    MechanismPose PoseAt(const Mechanism& /*m*/, int16_t /*p*/, float /*voxelSize*/) {
        // [mech-1] card 3 fills this
        return {};
    }

    physics::Layer MechanismLayer(const Mechanism& /*m*/, uint32_t /*tick*/) {
        // [mech-1] card 3 fills this
        return physics::Layer::Dynamic;
    }

    math::AABB SweptBounds(const Mechanism& /*m*/, const glm::ivec3& /*modelSize*/, float /*voxelSize*/) {
        // [mech-1] card 3 fills this
        return {};
    }

    bool MakeMechanism(Simulation& /*simulation*/, scene::Entity /*entity*/, const MechanismDesc& /*desc*/, const glm::ivec3& /*spawnVoxel*/,
                       uint8_t /*quarterTurns*/, int16_t /*initialPos*/, uint32_t /*tick*/) {
        // [mech-1] card 3 fills this
        return false;
    }

    void PoseMechanisms(Simulation& /*simulation*/, uint32_t /*tick*/, const math::AABB& /*reach*/) {
        // [mech-1] card 3 fills this
    }

    bool PushOutOfMechanisms(Simulation& /*simulation*/, uint32_t /*tick*/, PlayerState& /*state*/, physics::CharacterId /*character*/,
                             const PlayerTuning& /*tuning*/) {
        // [mech-1] card 3 fills this
        return false;
    }

    MechanismStepResult StepMechanisms(Simulation& /*simulation*/, uint32_t /*tick*/, bool /*authority*/) {
        // [mech-1] card 3 fills this
        return {};
    }

    bool FindUseTarget(const Simulation& /*simulation*/, const glm::dvec3& /*eye*/, const glm::vec3& /*dir*/, uint32_t /*tick*/,
                       UseTarget& /*out*/) {
        // [mech-1] card 4 fills this
        return false;
    }

    bool RayMeetsUseBox(const Simulation& /*simulation*/, scene::Entity /*entity*/, const glm::dvec3& /*eye*/, const glm::vec3& /*dir*/,
                        uint32_t /*tick*/, float /*inflate*/) {
        // [mech-1] card 4 fills this
        return false;
    }

    UseResult ApplyUse(Simulation& /*simulation*/, const UseTarget& /*target*/, const glm::dvec3& /*eye*/, uint32_t /*tick*/,
                       uint32_t /*leadTicks*/, std::vector<MechanismChange>& /*changes*/) {
        // [mech-1] card 4 fills this
        return UseResult::NoTarget;
    }

    std::string UsePrompt(const Simulation& /*simulation*/, const UseTarget& /*target*/, uint32_t /*tick*/, bool& greyed) {
        // [mech-1] card 4 fills this
        greyed = false;
        return {};
    }

    MechanismHit DamageMechanism(Simulation& /*simulation*/, scene::Entity /*entity*/, const glm::dvec3& /*centre*/, uint16_t /*powerQ*/,
                                 BlastResult& /*into*/) {
        // [mech-1] card 5 fills this
        return {};
    }

    BlastResult MechanismBlast(Simulation& /*simulation*/, scene::Entity /*target*/, const voxel::SphereQ& /*sphere*/, uint16_t /*power*/) {
        // [mech-1] card 5 fills this
        return {};
    }

    float DistanceToMechanism(const Simulation& /*simulation*/, scene::Entity /*entity*/, const glm::dvec3& /*point*/) {
        // [mech-1] card 5 fills this
        return 0.0f;
    }

    void ReleaseMechanism(Simulation& /*simulation*/, scene::Entity /*entity*/, uint32_t /*tick*/, uint8_t /*flag*/,
                          std::vector<MechanismChange>& /*changes*/) {
        // [mech-1] card 3 fills this
    }
}
