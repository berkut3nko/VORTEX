module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

export module vortex.sim:mechanism;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import :simulation;
import :player;
import :blast;
import :region;

/**
 * Doors, gates and levers ([mech-1], docs/design/mechanisms.md "Mechanisms"): kinematic voxel
 * objects whose travel position is an integer function of the tick (a ring of motion segments),
 * the same offline, on the server and on every client. The data types are scene components
 * (scene::Mechanism); the functions that pose, step, use and damage them are here.
 */
export namespace vortex::sim {

    using scene::MechanismKind;
    using scene::MechanismSegment;
    using scene::MechanismTrack;
    using scene::MechanismBox;
    using scene::MechanismDesc;
    using scene::Mechanism;
    using scene::kMechanismSegments;

    inline constexpr float kUseReach = 2.5f;                  ///< metres from the eye
    inline constexpr uint32_t kUseStaleTicks = 30;            ///< a use older than this many ticks is refused Stale
    inline constexpr uint32_t kMechanismBusyTicks = 12;       ///< ticks between two decisions on one mechanism
    inline constexpr uint32_t kMechanismReopenTicks = 20;     ///< ticks after a close arrives before an open is accepted
    inline constexpr float kMechanismHitRadius = 0.3f;        ///< metres
    inline constexpr uint32_t kMechanismLeadMin = 4, kMechanismLeadMax = 20, kOfflineMechanismLead = 2;
    inline constexpr uint32_t kMechanismCrushTicks = 6;       ///< the last ticks of a close that crush bodies
    inline constexpr float kUseBoxInflate = 0.1f, kPromptHysteresis = 0.2f;   ///< metres
    static_assert(kMechanismSegments >= (kMechanismLeadMax + kMechanismBusyTicks - 1) / kMechanismBusyTicks +
                                            (kUseStaleTicks + kMechanismBusyTicks - 1) / kMechanismBusyTicks + 1,
                  "the ring must hold every segment that may still start and every one a stale use may meet");

    /// @brief Why a use did nothing (snapshot owner block). Values are wire format.
    enum class UseResult : uint8_t { None = 0, Stale = 1, NoTarget = 2, Locked = 3, Busy = 4, Broken = 5, NotUsable = 6 };
    inline constexpr uint8_t kMaxUseResult = 6;
    /// @brief "accepted", "stale", "out of reach", "locked", "busy", "broken", "not usable".
    const char* ToString(UseResult result);

    extern const std::array<uint32_t, 65> kEaseOut;      ///< T[i] = 16 * (4096 - (64 - i)^2)
    extern const std::array<uint32_t, 65> kEaseSmooth;   ///< T[i] = (96 i^2 - i^3) / 2, integer division
    /// @brief 0..65536 at step `k` of `D` (k < D), integers only.
    uint32_t Ease(const std::array<uint32_t, 65>& table, uint32_t k, uint32_t D);
    /// @brief kEaseSmooth for a rising portcullis, kEaseOut for every other motion (design "Ease").
    const std::array<uint32_t, 65>& EaseTableFor(const MechanismDesc& desc, const MechanismSegment& segment);
    /// @brief A door opens when |p| grows; a portcullis and a lever when p grows.
    bool IsOpening(const MechanismDesc& desc, int16_t from, int16_t to);
    /// @brief Ticks from `from` to `to`: ceil(|to - from| x full-travel ticks / range), 0 when equal.
    uint16_t SegmentDuration(int16_t from, int16_t to, const MechanismDesc& desc);
    /// @brief The travel position at `tick`: an exact integer function of the track.
    int16_t PositionAt(const Mechanism& m, uint32_t tick);
    /// @brief The newest started segment has D > 0 and tick < start + D.
    bool IsMoving(const MechanismTrack& track, uint32_t tick);
    /// @brief Newest().startTick + Newest().durationTicks.
    uint32_t ArrivalTick(const MechanismTrack& track);
    /// @brief Appends `segment` (the oldest goes when the ring is full); sticky flags carry over.
    void PushSegment(MechanismTrack& track, const MechanismSegment& segment);
    /// @brief True when `desc` is a valid [mech-1] mechanism for a model of `modelSize` voxels; `why` gets the first reason.
    bool ValidateMechanismDesc(const MechanismDesc& desc, const glm::ivec3& modelSize, std::string* why = nullptr);

    struct MechanismPose {
        glm::dvec3 position{0.0};   ///< world metres of the CURRENT model origin (the body's shape origin)
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    /// @brief The world pose of `m` at travel position `p`.
    MechanismPose PoseAt(const Mechanism& m, int16_t p, float voxelSize);
    /// @brief The layer by the timetable: Dynamic at rest and while a portcullis rises, Rubble while moving otherwise.
    physics::Layer MechanismLayer(const Mechanism& m, uint32_t tick);
    /// @brief World bounds of the model over the whole travel, grown by one voxel.
    math::AABB SweptBounds(const Mechanism& m, const glm::ivec3& modelSize, float voxelSize);

    /// @brief Makes `entity` (a voxel object with a body) a mechanism at rest at `initialPos` from `tick`.
    bool MakeMechanism(Simulation& simulation, scene::Entity entity, const MechanismDesc& desc, const glm::ivec3& spawnVoxel,
                       uint8_t quarterTurns, int16_t initialPos, uint32_t tick);
    /// @brief Before every StepPlayer: mechanisms whose swept bounds meet `reach` take their exact pose, velocity and layer at `tick`.
    void PoseMechanisms(Simulation& simulation, uint32_t tick, const math::AABB& reach);
    /// @brief After PoseMechanisms: a mechanism that arrived at `tick` or `tick - 1` and overlaps the capsule pushes the feet out.
    bool PushOutOfMechanisms(Simulation& simulation, uint32_t tick, PlayerState& state, physics::CharacterId character,
                             const PlayerTuning& tuning);

    struct MechanismStepResult {
        std::vector<MechanismChange> changes;   ///< Freed by the anchors (authority only)
        uint32_t moving = 0;
        uint32_t crushed = 0;
        std::vector<glm::dvec3> crushedAt;      ///< where a crushed body was (offline puffs)
    };
    /// @brief Before StepPhysics: moving mechanisms MoveKinematic to pose(tick + 1); with `authority` the crush rule and anchors.
    MechanismStepResult StepMechanisms(Simulation& simulation, uint32_t tick, bool authority);

    struct UseTarget {
        scene::Entity entity = scene::kNoEntity;
        bool info = false;          ///< a non-usable mechanism (a portcullis): the prompt's info line
        float distance = 0.0f;      ///< metres from the eye
    };
    /// @brief The mechanism the look ray picks at `tick` (terrain occludes, bodies do not).
    bool FindUseTarget(const Simulation& simulation, const glm::dvec3& eye, const glm::vec3& dir, uint32_t tick, UseTarget& out);
    /// @brief The prompt's hysteresis: the ray still meets `entity`'s use box inflated by `inflate` metres.
    bool RayMeetsUseBox(const Simulation& simulation, scene::Entity entity, const glm::dvec3& eye, const glm::vec3& dir,
                        uint32_t tick, float inflate);
    /// @brief A use decided at `tick`: the segments it made (lever first, then its target) go to `changes`.
    UseResult ApplyUse(Simulation& simulation, const UseTarget& target, const glm::dvec3& eye, uint32_t tick, uint32_t leadTicks,
                       std::vector<MechanismChange>& changes);
    /// @brief HUD text of a target: "[E] Open", "[E] Close", "[E] Raise gate", "[E] Lower gate", "Gate - opened by a lever", "Locked".
    std::string UsePrompt(const Simulation& simulation, const UseTarget& target, uint32_t tick, bool& greyed);

    struct MechanismHit {
        uint32_t removed = 0;
        bool freed = false;         ///< door or portcullis off its hinge (MechanismFlag_Freed)
        bool broken = false;        ///< lever destroyed (MechanismFlag_Broken)
    };
    /// @brief The damage rule on one mechanism at `centre` (aimed: the caller checked kMechanismHitRadius).
    /// Appends to `into` (objectsHit, fragments, objectEvents, mechanismChanges).
    MechanismHit DamageMechanism(Simulation& simulation, scene::Entity entity, const glm::dvec3& centre, uint16_t powerQ,
                                 BlastResult& into);
    /// @brief A cast that resolved on a mechanism's body: no terrain operation, DamageMechanism, the push to other bodies.
    BlastResult MechanismBlast(Simulation& simulation, scene::Entity target, const voxel::SphereQ& sphere, uint16_t power);
    /// @brief Metres from `point` to the mechanism's posed model box (0 inside).
    float DistanceToMechanism(const Simulation& simulation, scene::Entity entity, const glm::dvec3& point);
    /// @brief Offline and server: frees a door or portcullis (Dynamic, no Mechanism component) or breaks a lever.
    void ReleaseMechanism(Simulation& simulation, scene::Entity entity, uint32_t tick, uint8_t flag, std::vector<MechanismChange>& changes);
}
