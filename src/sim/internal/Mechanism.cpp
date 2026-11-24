module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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

    // [mech-1] card 2 fills these
    const std::array<uint32_t, 65> kEaseOut{};
    const std::array<uint32_t, 65> kEaseSmooth{};

    uint32_t Ease(const std::array<uint32_t, 65>& /*table*/, uint32_t /*k*/, uint32_t /*D*/) {
        // [mech-1] card 2 fills this
        return 0;
    }

    const std::array<uint32_t, 65>& EaseTableFor(const MechanismDesc& /*desc*/, const MechanismSegment& /*segment*/) {
        // [mech-1] card 2 fills this
        return kEaseOut;
    }

    bool IsOpening(const MechanismDesc& /*desc*/, int16_t /*from*/, int16_t /*to*/) {
        // [mech-1] card 2 fills this
        return false;
    }

    uint16_t SegmentDuration(int16_t /*from*/, int16_t /*to*/, const MechanismDesc& /*desc*/) {
        // [mech-1] card 2 fills this
        return 0;
    }

    int16_t PositionAt(const Mechanism& /*m*/, uint32_t /*tick*/) {
        // [mech-1] card 2 fills this
        return 0;
    }

    bool IsMoving(const MechanismTrack& /*track*/, uint32_t /*tick*/) {
        // [mech-1] card 2 fills this
        return false;
    }

    uint32_t ArrivalTick(const MechanismTrack& /*track*/) {
        // [mech-1] card 2 fills this
        return 0;
    }

    void PushSegment(MechanismTrack& /*track*/, const MechanismSegment& /*segment*/) {
        // [mech-1] card 2 fills this
    }

    bool ValidateMechanismDesc(const MechanismDesc& /*desc*/, const glm::ivec3& /*modelSize*/, std::string* /*why*/) {
        // [mech-1] card 2 fills this
        return false;
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
