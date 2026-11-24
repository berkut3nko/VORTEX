module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
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

    /// @brief What E meets (docs/design/mechanisms.md "One use key: E"). Not wire format.
    enum class UseTargetKind : uint8_t { None = 0, Mechanism = 1, Info = 2, Holdable = 3, Rope = 4, HeldByOther = 5 };

    struct UseTarget {
        scene::Entity entity = scene::kNoEntity;
        bool info = false;          ///< kind == Info: a non-usable mechanism (a portcullis), the prompt's info line
        float distance = 0.0f;      ///< metres from the eye
        UseTargetKind kind = UseTargetKind::None;
    };

    /// @brief What differs per side in FindUseTarget (design "The order rule", "What can be held").
    struct UseTargetHooks {
        /// The owner test of a holdable body (after IsHoldableShape). Empty: IsHoldable(sim, e, false).
        std::function<bool(scene::Entity)> holdable;
        /// The tie key. Empty: the NetId value when the entity has one, else the entity id.
        std::function<uint32_t(scene::Entity)> key;
    };
    /// @brief The nearest target of any kind the look ray picks at `tick`: a usable mechanism's use box, a
    /// holdable body; terrain, block boxes and bodies held by others occlude. Info and HeldByOther when only
    /// a non-usable mechanism or a held body is in front. `hooks`: the per-side owner test and tie key.
    bool FindUseTarget(const Simulation& simulation, const glm::dvec3& eye, const glm::vec3& dir, uint32_t tick, UseTarget& out,
                       const UseTargetHooks* hooks = nullptr);
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

    // ---------------------------------------------------------------------------------------------
    //  Holding a small object with E (docs/design/mechanisms.md "One use key: E").
    // ---------------------------------------------------------------------------------------------

    inline constexpr float kHoldDistance = 1.5f, kHoldClearance = 0.1f, kHoldMaxExtent = 1.25f;   ///< metres
    inline constexpr float kHoldDrop = 0.5f;               ///< metres: the hold anchor below the eye
    inline constexpr float kHoldMaxVolume = 0.6f;          ///< m^3 of solid voxels
    inline constexpr uint32_t kHoldPinnedTicks = 30, kHoldBusyTicks = 12;
    inline constexpr uint32_t kHoldGroundTicks = 30;       ///< a body stood on this recently is not a pick-up
    inline constexpr int16_t kHoldPitchLimit = 16384;      ///< 45 deg in PlayerInput pitch units
    inline constexpr float kThrowSpeed = 6.0f;             ///< m/s along the look
    inline constexpr float kHoldReleaseMin = 0.3f;         ///< metres: the release fix-up's nearest centre
    static_assert(kHoldDistance >= 0.8660254f * kHoldMaxExtent + kHoldClearance);

    /// @brief Why a hold ended (EntityRelease). Values are wire format.
    enum class HoldEnd : uint8_t { Drop = 1, Throw = 2, HolderDead = 3, HolderGone = 4, Damaged = 5, Pinned = 6 };
    inline constexpr uint8_t kMaxHoldEnd = 6;

    /// @brief Where a hold is posed and released besides the holder: the play area and the other players.
    struct HoldEnv {
        PlayerTuning tuning;
        PlayArea area;                         ///< sim::PlayAreaOf(region, tuning) in a session; PlayArea{} offline
        std::span<const glm::vec3> playerFeet; ///< every player's feet (the release fix-up's capsules)
    };

    /// @brief clamp(pitch, -kHoldPitchLimit, kHoldPitchLimit) / 256.
    int8_t HoldPitch(int16_t pitch);
    /// @brief llround(kHoldMaxVolume / vs^3): the most solid voxels a holdable body may have.
    uint32_t HoldMaxVoxels(float voxelSize);
    /// @brief The object's yaw at the pick-up less the holder's, in PlayerInput yaw units.
    uint16_t RelYawAtPickup(const glm::quat& rotation, uint16_t holderYaw);
    /// @brief feet + (0, eyeHeight - kHoldDrop, 0): where a hold hangs from.
    glm::dvec3 HoldAnchor(const glm::vec3& feet, const PlayerTuning& tuning);
    /// @brief Every side: VoxelBody and RigidBody, no Mechanism, no Held, every extent at most kHoldMaxExtent,
    /// solid voxels in [debrisBodyVoxels, HoldMaxVoxels].
    bool IsHoldableShape(const Simulation& simulation, scene::Entity entity);
    /// @brief Server and offline: the shape test, RigidBody::motion == Dynamic, and with `replicatedOnly` a NetId != 0.
    bool IsHoldable(const Simulation& simulation, scene::Entity entity, bool replicatedOnly);
    /// @brief The pick-up path: the sphere of the object's radius swept from `anchor` to its centre meets no solid
    /// voxel and no mechanism block box.
    bool PickupPathClear(const Simulation& simulation, scene::Entity entity, const glm::dvec3& anchor);

    struct HeldPose {
        MechanismPose pose;          ///< shape origin and rotation
        float distance = 0.0f;       ///< d: anchor to the object's centre
        bool pinned = false;
    };
    /// @brief The pose of `held` for a holder at `feet` looking (yaw, holdPitch * 256); mechanisms at `tick` clamp it.
    HeldPose HeldPoseAt(const Simulation& simulation, scene::Entity held, const glm::vec3& feet, uint16_t yaw, int8_t holdPitch,
                        const HoldEnv& env, uint32_t tick);
    /// @brief Authority: picks `entity` up for `holder` (None, Busy or NoTarget); `out` is the EntityHold. `stoodOn`:
    /// a player stood on the body within kHoldGroundTicks (the caller knows; offline the own ground body now).
    UseResult BeginHold(Simulation& simulation, scene::Entity entity, uint8_t holder, uint16_t yaw, uint32_t tick,
                        uint32_t lastChangeTick, bool anyChange, bool stoodOn, bool replicated, HoldChange& out);
    /// @brief Every tick after the players: MoveKinematic to the pose; with `authority` counts pinned ticks and
    /// returns true at kHoldPinnedTicks (the caller ends the hold Pinned).
    bool StepHeld(Simulation& simulation, scene::Entity entity, const PlayerState& holder, uint16_t yaw, int8_t holdPitch,
                  const HoldEnv& env, uint32_t tick, bool authority);
    /// @brief Server and offline: ends the hold at `pose` (a fix-up out of walls, bodies and players), Dynamic,
    /// quantised `velocity`; `out` is the EntityRelease. `anchor` (the holder's; nullptr for Damaged and
    /// HolderGone) enables the fix-up.
    void EndHold(Simulation& simulation, scene::Entity entity, HoldEnd reason, const MechanismPose& pose, const glm::vec3& velocity,
                 const glm::dvec3* anchor, const HoldEnv& env, uint32_t tick, bool replicated, HoldChange& out);
    /// @brief Throw: the holder's velocity plus kThrowSpeed along the look; every other reason: the holder's velocity.
    glm::vec3 ReleaseVelocity(HoldEnd reason, const glm::vec3& holderVelocity, uint16_t yaw, int16_t pitch);
    /// @brief A client copy (kinematic, never simulated): `hold` emplaces Held and Layer::Held; a release removes
    /// Held and teleports to the release pose. The layer after a release and the samples are the caller's (net).
    void ApplyHoldChange(Simulation& simulation, scene::Entity entity, const HoldChange& change);
    /// @brief "[E] Drop   [LMB] Throw".
    std::string HoldPrompt();
    /// @brief A velocity as the wire carries it: int16 per axis in 1/256 m/s (net::QuantizeVelocity forwards here).
    glm::vec3 QuantizeVelocity(const glm::vec3& velocity);
}
