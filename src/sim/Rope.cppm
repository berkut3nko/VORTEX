module;

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

export module vortex.sim:rope;

import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import :simulation;
import :mechanism;

/**
 * Ropes ([mech-2], docs/design/mechanisms.md "Ropes with physics"), the offline part: a load rope holds a dynamic
 * body on one distance constraint from a world-fixed anchor; a chain follows its mechanism and is drawn only. A cast's
 * ray or sphere cuts ropes, an anchor that turns to air or a load that goes cuts its rope, and E on a cleat releases
 * it. A cut chain jams its portcullis shut. The data is scene::Rope; the functions are here.
 */
export namespace vortex::sim {

    using scene::Rope;

    inline constexpr float kRopeRadius = 0.02f;          ///< metres (the drawn rope is about 4 cm thick)
    inline constexpr float kRopeAimForgiveness = 0.1f;   ///< metres a direct shot may miss a rope by and still cut it
    inline constexpr uint32_t kMaxRopeCutsPerCast = 4;
    inline constexpr uint32_t kRopeBusyTicks = 12;

    /// @brief Why a rope was cut. Values are the planned wire values of RopeCut ([mech-2] session part).
    enum class RopeCutCause : uint8_t { Ray = 1, Sphere = 2, Anchor = 3, LoadGone = 4, Released = 5 };

    struct RopeCutEvent {
        scene::Entity rope = scene::kNoEntity;
        uint32_t tick = 0;
        float fraction = 0.0f;                 ///< 0 the anchor end .. 1 the load end
        RopeCutCause cause = RopeCutCause::Ray;
    };

    /// @brief The world point a rope hangs from: the centre of the anchor voxel's bottom face.
    glm::dvec3 RopeAnchorPoint(const voxel::World& world, const glm::ivec3& anchorVoxel);
    /// @brief A load rope from `anchorVoxel` to the point `attachVoxel` (the load's current model voxels) of the dynamic
    /// body `load`, as long as they are apart now. kNoEntity when the load has no dynamic body.
    scene::Entity CreateLoadRope(Simulation& simulation, const glm::ivec3& anchorVoxel, scene::Entity load, const glm::vec3& attachVoxel,
                                 uint8_t flags = 0);
    /// @brief A chain from `anchorVoxel` to the point `attachVoxel` (the mechanism's original model voxels) of a mechanism.
    scene::Entity CreateChainRope(Simulation& simulation, const glm::ivec3& anchorVoxel, scene::Entity mechanism, const glm::vec3& attachVoxel);
    /// @brief The two ends of an uncut rope now (anchor, attach); false when the load or mechanism is gone.
    bool RopeEnds(const Simulation& simulation, scene::Entity rope, glm::dvec3& anchor, glm::dvec3& attach);
    /// @brief Every rope entity in a fixed order (entity id).
    std::vector<scene::Entity> RopesInOrder(const scene::Scene& scene);

    /// @brief Cuts the rope at `fraction`: the constraint goes, the load falls; a cut chain jams its portcullis shut
    /// (a segment to minPos at its drop speed with MechanismFlag_Jammed).
    void CutRope(Simulation& simulation, scene::Entity rope, float fraction, RopeCutCause cause, uint32_t tick,
                 std::vector<RopeCutEvent>* events = nullptr);
    /// @brief A direct shot: every uncut rope the segment eye .. eye + dir * maxDistance passes within kRopeRadius +
    /// kRopeAimForgiveness of is cut, nearest along the ray first, at most kMaxRopeCutsPerCast. Ropes do not stop the
    /// ray. Positions are quantised to 1/16 voxel first.
    uint32_t CutRopesAlongRay(Simulation& simulation, const glm::dvec3& eye, const glm::vec3& dir, float maxDistance, uint32_t tick,
                              std::vector<RopeCutEvent>* events = nullptr);
    /// @brief A cast's sphere: every uncut rope within the sphere's radius + kRopeRadius is cut at its nearest point.
    uint32_t CutRopesBySphere(Simulation& simulation, const voxel::SphereQ& sphere, uint32_t tick, std::vector<RopeCutEvent>* events = nullptr);
    /// @brief Every tick (authority): an anchor voxel turned to air cuts at 0, a load that is gone cuts at 1, a load whose
    /// body was re-made (Scene::Damage) keeps its rope when the attach point still lies in a solid voxel of its model.
    void StepRopes(Simulation& simulation, uint32_t tick, std::vector<RopeCutEvent>* events = nullptr);
    /// @brief The world box E meets for a rope's cleat: the anchor voxel grown by 2 voxels on every side.
    math::AABB RopeCleatBox(const voxel::World& world, const Rope& rope);
    /// @brief E on a cleat: the rope is released (cut at 0, Released). Busy within kRopeBusyTicks of the last decision;
    /// NoTarget for a cut rope or one without a cleat.
    UseResult ReleaseRope(Simulation& simulation, scene::Entity rope, uint32_t tick, std::vector<RopeCutEvent>* events = nullptr);
}
