module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

export module vortex.scene:components;

import vortex.math;
import vortex.voxel;
import vortex.physics;

export namespace vortex::scene {

    /**
     * @brief Identity of a scene entity: an EnTT entity (32 bits: 20 bit index + 12 bit version).
     * @details The alias is exported, so importers can store, compare and pass entities without
     * including EnTT. Ids of destroyed entities are *stale*: the version part makes them
     * distinguishable from whatever reuses the index, and every Scene function accepts them
     * (no-op / default result). Only code that spells `entt::` names itself (`entt::registry`,
     * `entt::null`, `entt::exclude` ...) has to `#include <entt/entt.hpp>` in its global module
     * fragment (or, for a plain .cpp, before the imports).
     */
    using Entity = entt::entity;

    /// @brief "No entity" (equal to entt::null). Never valid, accepted everywhere.
    inline constexpr Entity kNoEntity = entt::null;

    static_assert(sizeof(Entity) == sizeof(uint32_t), "entities must fit the low half of a body's user data");

    /// @brief Upper 32 bits of the user data of every body the scene creates. Bodies made by
    /// anyone else (terrain colliders use 0) can therefore never be mistaken for an entity -
    /// note that the very first entity has the integral value 0.
    inline constexpr uint64_t kEntityUserDataTag = 0x56585345ull << 32;     // "VXSE"

    /// @brief Packs an entity into physics::BodyDesc::userData.
    constexpr uint64_t ToUserData(Entity entity) {
        return kEntityUserDataTag | static_cast<uint64_t>(static_cast<uint32_t>(entity));
    }

    /// @brief Inverse of ToUserData: the entity id stored in a body's user data, or kNoEntity if
    /// the value was not written by a scene. The id may be stale: check it with Scene::IsValid,
    /// or use Scene::EntityFromUserData which does both.
    constexpr Entity EntityFromUserData(uint64_t userData) {
        return (userData & 0xFFFFFFFF00000000ull) == kEntityUserDataTag
                   ? static_cast<Entity>(static_cast<uint32_t>(userData))
                   : kNoEntity;
    }

    // ---------------------------------------------------------------------------------------------
    //  Components: plain data. Systems live in Scene.
    // ---------------------------------------------------------------------------------------------

    /**
     * @brief Where an entity is. Every entity created through Scene has one.
     * @details `current` is the simulation pose: for voxel objects `position` is the model origin
     * (its minimum corner) in world metres and `scale` is the voxel size; for other entities scale
     * is 1. `previous` is the pose the renderer drew last frame (Scene::BeginFrame copies
     * current -> previous); it becomes InstanceDesc::prevLocalToWorld, i.e. the motion vector.
     *
     * Entities with a RigidBody are driven by physics: Scene::SyncFromPhysics overwrites
     * `current` position and rotation after every step, so move them with Scene::SetTransform.
     * Entities without a body may be written directly; set `previous = current` as well when the
     * change is a teleport rather than a movement (or use Scene::SetTransform).
     */
    struct Transform {
        math::Transform current;
        math::Transform previous;
    };

    /// @brief Human readable label (editor, logs). Pieces split off an entity inherit it.
    struct Name {
        std::string value;
    };

    /**
     * @brief Makes an entity a voxel object: a shared model drawn at the entity's Transform.
     * @details The model is baked (has chunk slots in the scene's ChunkStore). Models are shared
     * and never modified through an entity: damage replaces `model` with a private new one. The
     * chunk slots go back to the store when the last reference to a model is dropped, which is
     * why destroying the last user of a model frees its slots.
     */
    struct VoxelBody {
        std::shared_ptr<voxel::VoxelModel> model;
        bool visible = true;          ///< false: not gathered for rendering (collision is unaffected).
    };

    /**
     * @brief The physics body of an entity. Owned by the entity.
     * @details Created by Scene (its user data is ToUserData(entity)) and destroyed with the
     * component: Scene listens to the registry, so `Scene::Destroy`, `registry.destroy`,
     * `registry.remove<RigidBody>` and `registry.clear` all release the body. Do not emplace this
     * component or overwrite `body` by hand - the previous body would leak.
     */
    struct RigidBody {
        physics::BodyId body;
        physics::MotionType motion = physics::MotionType::Dynamic;
    };

    /**
     * @brief Point light carried by an entity (needs a Transform).
     * @details World position = `current.position + current.rotation * offset` (offset in metres,
     * not scaled by the transform). Gathered into graphics::PointLight every frame; lights with
     * `intensity <= 0` are skipped.
     */
    struct PointLight {
        glm::vec3 color{1.0f, 0.8f, 0.6f};    ///< Linear RGB.
        float intensity = 5.0f;
        float radius = 8.0f;                  ///< Range of influence in metres.
        float sourceRadius = 0.1f;            ///< Size of the emitter (soft shadows).
        glm::vec3 offset{0.0f};               ///< Metres, in the entity's rotated frame.
        bool effect = false;                  ///< Unshadowed effect light: drawn in the composite pass ([wp-8]).
        glm::vec3 clipMin{0.0f}, clipMax{0.0f};   ///< effect only: reach box relative to the light, metres
    };

    /**
     * @brief Destroys the entity when the time is up (Scene::Update).
     * @details With `lightDecay > 0` the intensity of a PointLight on the same entity fades
     * exponentially while the entity lives: `intensity *= exp(-lightDecay * dt)` per update.
     * An explosion flash is Transform + PointLight + Lifetime.
     */
    struct Lifetime {
        float remaining = 1.0f;       ///< Seconds left; the entity is destroyed once this is <= 0.
        float lightDecay = 0.0f;      ///< Per second; 0 = the light keeps its intensity.
    };

    /// @brief Why a replicated entity went away: what its EntityDespawn tells the clients
    /// (docs/design/networking.md "Entity replication"). Values are wire format.
    enum class DespawnReason : uint8_t {
        Destroyed = 1,   ///< gone (fell out of the world, cleared by the server): a client removes its copy
        ToRubble = 2,    ///< evicted by the piece caps: a client keeps its copy as local rubble, where it is drawn
        Broken = 3,      ///< damaged apart: its remains follow as client-rubble fragments, a client removes its copy
    };

    /**
     * @brief A server entity that clients have a copy of (docs/design/networking.md stage 6):
     * server-allocated, counting up, never reused. Only a session server (and the copies a client
     * makes) carries it; an offline scene has none.
     * @details Destroying the entity by any path (Scene::Destroy, Damage, DestroyBelow, a registry
     * clear) is seen by an `on_destroy<NetId>` observer, which is how a server turns every
     * destruction into an EntityDespawn. `despawn` is what that event says: set it before destroying.
     */
    struct NetId {
        uint32_t value = 0;
        /// A piece (terrain that broke off, or a piece Damage split off): counted against the piece
        /// caps and evicted by them. false: a placed object (a template or player crate).
        bool piece = false;
        uint32_t bornTick = 0;        ///< Simulation tick it was made on: the eviction order (oldest first).
        DespawnReason despawn = DespawnReason::Destroyed;
    };

    // ---------------------------------------------------------------------------------------------
    //  Mechanisms ([mech-1], docs/design/mechanisms.md "Mechanisms"). The data lives here, the
    //  functions in vortex.sim (sim:mechanism re-exports these types).
    // ---------------------------------------------------------------------------------------------

    /// @brief Kinds of mechanism (docs/design/mechanisms.md "Mechanisms"). Values are wire format.
    enum class MechanismKind : uint8_t {
        Door = 1,         ///< hinge about a vertical axis, opens away from the user
        Portcullis = 2,   ///< slide up, driven by a lever (an Info target)
        Lever = 3,        ///< hinge, a small throw that drives its linked target
    };
    /// @brief State flags carried in every segment. Locked, Jammed, Freed and Broken stay set once set. Wire format.
    enum MechanismFlag : uint8_t {
        MechanismFlag_Locked = 1,
        MechanismFlag_Jammed = 2,
        MechanismFlag_Freed = 4,
        MechanismFlag_Broken = 8,
    };
    /// @brief Authoring flags of a MechanismDesc. Wire format.
    enum MechanismDescFlag : uint8_t {
        MechanismDesc_OneWay = 1,   ///< a door opens to maxPos only
        MechanismDesc_Usable = 2,   ///< E acts on it (doors, levers); a portcullis is an Info target
        MechanismDesc_Locked = 4,   ///< spawns with MechanismFlag_Locked
        MechanismDesc_Warded = 8,   ///< damage skips it entirely
    };

    /// @brief One motion of a mechanism (wire: 4 + 2 + 2 + 2 + 1 = 11 bytes).
    struct MechanismSegment {
        uint32_t startTick = 0;       ///< tick the motion starts
        int16_t startPos = 0;         ///< p at startTick
        int16_t targetPos = 0;        ///< == startPos when durationTicks == 0
        uint16_t durationTicks = 0;   ///< D; 0 = stopped at startPos
        uint8_t flags = 0;            ///< MechanismFlag
        friend bool operator==(const MechanismSegment&, const MechanismSegment&) = default;
    };

    inline constexpr uint32_t kMechanismSegments = 8;

    /// @brief The last kMechanismSegments segments, ring[0] oldest .. ring[count - 1] newest; count >= 1 once spawned.
    struct MechanismTrack {
        std::array<MechanismSegment, kMechanismSegments> ring{};
        uint8_t count = 0;
        const MechanismSegment& Newest() const { return ring[count - 1]; }
    };

    /// @brief A box of the original model, voxels [min, max) (wire: 6 x uint8).
    struct MechanismBox {
        glm::ivec3 min{0}, max{0};
        friend bool operator==(const MechanismBox&, const MechanismBox&) = default;
    };

    /// @brief What a mechanism is (wire: 63 bytes). Model-space fields are in the ORIGINAL model's voxels.
    struct MechanismDesc {
        MechanismKind kind = MechanismKind::Door;
        uint8_t flags = 0;                     ///< MechanismDescFlag
        glm::ivec3 pivotQ{0};                  ///< hinge point, 1/16 voxel (wire 3 x int16)
        uint8_t axis = 2;                      ///< motion axis: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z
        int16_t minPos = 0, maxPos = 0;        ///< travel range; hinge: 4096 = one turn; slide: 1/16 voxel
        uint16_t openTicks = 0;                ///< ticks for the whole range when opening
        uint16_t closeTicks = 0;               ///< ticks for the whole range when closing
        uint16_t autoReturnTicks = 0;          ///< 0 in [mech-1]
        MechanismBox useBox, blockBox, hingeBox;
        uint8_t pushAxis = 4;                  ///< model axis (0-5) a landing pushes players along
        uint8_t anchorCount = 0;               ///< 0 or 2
        std::array<glm::ivec3, 2> anchors{};   ///< world voxels the hinge rests in (wire 2 x 3 x int32)
        friend bool operator==(const MechanismDesc&, const MechanismDesc&) = default;
    };

    /// @brief A mechanism: a kinematic voxel object on a timetable (docs/design/mechanisms.md "The model").
    struct Mechanism {
        MechanismDesc desc;
        glm::ivec3 spawnVoxel{0};              ///< world voxel of the original model origin at p = 0
        uint8_t quarterTurns = 0;              ///< spawn rotation about +y, 0-3
        glm::ivec3 modelOffset{0};             ///< voxels: the sum of every DamageResult::keptOffset
        MechanismTrack track;
        Entity linked = kNoEntity;             ///< a lever's target ([mech-1]: at most one)
        uint32_t linkedNetId = 0;              ///< the same in a session (0 offline)
        uint32_t lastDecisionTick = 0;         ///< server and offline: the busy rule
        bool anyDecision = false;
        math::AABB swept;                      ///< world bounds over the whole travel
    };

    /// @brief Rope flags (docs/design/mechanisms.md "Ropes with physics").
    enum RopeFlag : uint8_t {
        RopeFlag_Cleat = 1,   ///< E on the anchor releases it ("Rope actions")
        RopeFlag_Chain = 2,   ///< a mechanism chain: drawn between the anchor and the mechanism's pose, no physics
    };

    /// @brief A rope ([mech-2]): a load rope holds a dynamic body on one distance constraint from a world-fixed
    /// anchor; a chain follows its mechanism. An entity of its own (no body); its presence is the rope.
    struct Rope {
        glm::ivec3 anchorVoxel{0};             ///< the world voxel it hangs from (the anchor point: its bottom face centre)
        Entity load = kNoEntity;               ///< the body it holds, or the mechanism of a chain
        glm::vec3 attach{0.0f};                ///< load: its current model's voxels; chain: the mechanism's ORIGINAL model voxels
        float length = 0.0f;                   ///< metres (a load rope's longest)
        uint8_t flags = 0;                     ///< RopeFlag
        physics::ConstraintId constraint;      ///< load ropes, while not cut
        physics::BodyId body;                  ///< the body the constraint holds (Scene::Damage makes a new one)
        glm::dvec3 lastAttach{0.0};            ///< the attach point's world position after the last step
        bool cut = false;
        float cutFraction = 0.0f;              ///< where it was cut: 0 the anchor end .. 1 the load end
        uint32_t cutTick = 0;
        uint32_t lastDecisionTick = 0;         ///< the busy rule of a release (12 ticks)
        bool anyDecision = false;
    };

    /// @brief A body a player holds (docs/design/mechanisms.md "One use key: E"). Its presence is the truth.
    struct Held {
        uint8_t holder = 0;                    ///< session slot; 0 offline
        uint16_t relYaw = 0;                   ///< object yaw - holder yaw at the pick-up (PlayerInput yaw units)
        uint32_t sinceTick = 0;                ///< the tick the hold started
        uint16_t pinnedTicks = 0;              ///< consecutive pinned ticks (authority only)
    };

    /**
     * @brief Marks a voxel object as rubble: a piece left over by destruction that nobody will
     * miss. Scene::Update removes rubble (see SceneConfig: the lifetimes by size,
     * debrisFarDistance, maxDebris / maxRubbleBodies / maxRubbleSlots) and leaves a puff of
     * particles in its place.
     * @details Added by the scene to every dynamic object created with VoxelObjectDesc::debris
     * (terrain that broke off, object fragments; docs/design/shred.md stage 1e), to the pieces
     * Scene::Damage splits off a piece of rubble, to small pieces Damage splits off anything, and
     * to what is left of the damaged object once that is small (SceneConfig::debrisRemnants).
     * Remove the component to keep such a piece for good; add it to any voxel object to have it
     * cleaned up.
     */
    struct Debris {
        float age = 0.0f;             ///< Seconds since it became rubble. The oldest go first.
        /// Seconds it lives: set by the scene from the piece's size (SceneConfig::debrisLifetime,
        /// rubbleMediumLifetime, rubbleLargeLifetime). < 0 (a Debris added by hand):
        /// SceneConfig::debrisLifetime. 0: no time limit (the caps still apply).
        float lifetime = -1.0f;
    };
}
