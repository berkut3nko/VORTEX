module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

export module vortex.physics:world;

import vortex.core;
import vortex.math;

export namespace vortex::physics {

    /// @brief Opaque handle of a rigid body. Stale handles are detected (index + sequence number).
    struct BodyId {
        uint32_t value = 0xFFFFFFFFu;
        bool IsValid() const { return value != 0xFFFFFFFFu; }
        friend bool operator==(const BodyId&, const BodyId&) = default;
    };

    enum class MotionType : uint8_t { Static, Kinematic, Dynamic };

    /// @brief Collision layers. Debris does not collide with other debris, which keeps large
    /// piles of fragments cheap.
    enum class Layer : uint8_t {
        Static,      ///< Terrain and immovable objects. Collides with everything that moves.
        Dynamic,     ///< Ordinary movable objects.
        Debris,      ///< Small fragments: collide with Static, Dynamic and Character, not with each other.
        Character,   ///< Character controllers.
    };

    /// @brief Box in body-local metres with a density; the building block of voxel object shapes.
    struct BoxDesc {
        glm::vec3 center{0.0f};
        glm::vec3 halfExtent{0.5f};
        float density = 1000.0f;      ///< kg / m^3
    };

    /// @brief Immutable, shareable collision shape (reference counted).
    class Shape;
    using ShapeRef = std::shared_ptr<const Shape>;

    struct BodyDesc {
        ShapeRef shape;
        glm::vec3 position{0.0f};                       ///< World metres: position of the shape's origin.
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        MotionType motion = MotionType::Dynamic;
        Layer layer = Layer::Dynamic;
        float friction = 0.6f;
        float restitution = 0.05f;
        glm::vec3 linearVelocity{0.0f};
        glm::vec3 angularVelocity{0.0f};
        float linearDamping = 0.05f;
        float angularDamping = 0.05f;
        float gravityFactor = 1.0f;
        bool sensor = false;                            ///< Detects overlaps, no collision response.
        bool continuous = false;                        ///< Continuous collision detection for fast, small bodies.
        bool allowSleeping = true;
        uint64_t userData = 0;                          ///< Returned by ray casts and contact events (e.g. an entity id).
    };

    struct BodyState {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 linearVelocity{0.0f};
        glm::vec3 angularVelocity{0.0f};
        bool active = false;                            ///< Awake. Never true while frozen.
        bool frozen = false;                            ///< Held in place by SetFrozen (velocities read as zero).
        /// Frozen bodies only: it was awake when it was frozen, or something tried to wake, push or
        /// move it since (DynamicBodyInfo::wantsToMove). False for a body parked asleep.
        bool wantsToMove = false;
    };

    /// @brief One movable body - dynamic, or dynamic and frozen - as listed by PhysicsWorld::GetDynamicBodies.
    struct DynamicBodyInfo {
        BodyId body;
        math::AABB bounds;                              ///< World-space bounds.
        glm::vec3 linearVelocity{0.0f};                 ///< Frozen bodies: the velocity they resume with.
        bool active = false;                            ///< Awake (never while frozen).
        bool frozen = false;
        /// Frozen bodies only: it was awake when it was frozen, or something tried to wake, push or
        /// move it since. It will be awake when it resumes.
        bool wantsToMove = false;
    };

    struct RayHit {
        bool hit = false;
        BodyId body;
        float distance = 0.0f;
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        uint64_t userData = 0;
    };

    /// @brief Two bodies started touching. Reported once per new contact, after Step().
    struct ContactEvent {
        BodyId bodyA, bodyB;
        uint64_t userDataA = 0, userDataB = 0;
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f};          ///< From A towards B.
        float approachSpeed = 0.0f;      ///< Relative speed along the normal at impact (m/s).
    };

    struct CharacterDesc {
        glm::vec3 position{0.0f};        ///< Feet position.
        float height = 1.8f;
        float radius = 0.3f;
        float maxSlopeDegrees = 50.0f;
        float stepHeight = 0.35f;        ///< Ledges up to this height are climbed automatically.
        float mass = 75.0f;
    };

    struct CharacterId {
        uint32_t value = 0xFFFFFFFFu;
        bool IsValid() const { return value != 0xFFFFFFFFu; }
    };

    struct CharacterState {
        glm::vec3 position{0.0f};        ///< Feet position.
        glm::vec3 velocity{0.0f};
        bool onGround = false;
    };

    struct PhysicsConfig {
        uint32_t maxBodies = 16384;
        uint32_t maxBodyPairs = 65536;
        uint32_t maxContactConstraints = 20480;
        unsigned threads = 0;                    ///< Worker threads for the solver (0 = hardware - 1, at most 8).
        glm::vec3 gravity{0.0f, -9.81f, 0.0f};
        float contactEventMinSpeed = 2.0f;       ///< Slower impacts are not reported.
    };

    /**
     * @brief Rigid body simulation (Jolt Physics behind an engine-owned interface).
     * @details
     *  - Step() must be called with a FIXED time step (the engine's accumulator does this).
     *  - Positions are world metres in single precision.
     *  - All functions are main-thread only, except the Create*Shape functions which are
     *    thread safe so shapes can be cooked on worker threads.
     *  - Every function taking a BodyId tolerates invalid / destroyed ids (no-op or default result).
     */
    class PhysicsWorld {
    public:
        PhysicsWorld();
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        bool Initialize(const PhysicsConfig& config = {});
        void Shutdown();
        bool IsInitialized() const;

        /// @brief Advances the simulation by `dt` seconds (fixed step) and collects contact events.
        void Step(float dt);
        glm::vec3 Gravity() const;

        // --- shapes (thread safe) --------------------------------------------------------------
        /// @brief Compound of boxes; mass and inertia follow from the box densities. Null if `boxes` is empty.
        ShapeRef CreateCompoundShape(std::span<const BoxDesc> boxes) const;
        /// @brief Triangle mesh for static bodies. Null if the mesh is empty or degenerate.
        ShapeRef CreateMeshShape(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices) const;
        ShapeRef CreateBoxShape(const glm::vec3& halfExtent, float density = 1000.0f) const;
        ShapeRef CreateSphereShape(float radius, float density = 1000.0f) const;

        // --- bodies ------------------------------------------------------------------------------
        BodyId CreateBody(const BodyDesc& desc);
        void DestroyBody(BodyId body);
        bool IsValid(BodyId body) const;

        BodyState GetState(BodyId body) const;
        void SetTransform(BodyId body, const glm::vec3& position, const glm::quat& rotation, bool activate = true);
        void SetVelocity(BodyId body, const glm::vec3& linear, const glm::vec3& angular);
        /// @brief Impulse in kg*m/s; applied at the centre of mass if `worldPoint` is not given.
        void AddImpulse(BodyId body, const glm::vec3& impulse, std::optional<glm::vec3> worldPoint = std::nullopt);
        void SetMotionType(BodyId body, MotionType motion);
        /// @brief Swaps the collision shape, keeping pose and velocities (mass is recomputed).
        void SetShape(BodyId body, const ShapeRef& shape);
        void Activate(BodyId body);
        /// @brief Wakes every body overlapping the box (call after the ground under them changed).
        void ActivateInBox(const math::AABB& box);

        /**
         * @brief Freezes a dynamic body in place, or lets it continue.
         * @details A frozen body is not simulated: it does not move, cannot be woken and other
         * bodies collide with it as with a static object. Its velocities are remembered and given
         * back when it is released, so a body frozen in flight continues its flight. Attempts to
         * wake or push it while frozen (Activate, ActivateInBox, AddImpulse, SetVelocity,
         * SetTransform with `activate`) are not lost: impulses and velocities change what it resumes
         * with, and it resumes awake. A body that was asleep when it was frozen and was left alone
         * resumes asleep. No-op for bodies that are not dynamic, and for invalid ids.
         *
         * This is what physics::TerrainColliders uses to keep bodies from falling through terrain
         * that has no collision yet (or no longer).
         */
        void SetFrozen(BodyId body, bool frozen);
        bool IsFrozen(BodyId body) const;

        float GetMass(BodyId body) const;
        uint64_t GetUserData(BodyId body) const;
        /// @brief Velocity of the material point of the body at a world position.
        glm::vec3 GetPointVelocity(BodyId body, const glm::vec3& worldPoint) const;
        math::AABB GetBounds(BodyId body) const;

        // --- queries -----------------------------------------------------------------------------
        /// @brief Closest hit along a ray (`direction` normalised). Sensors are ignored.
        RayHit Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;
        /// @brief Ids of all awake dynamic bodies.
        void GetActiveBodies(std::vector<BodyId>& out) const;
        /// @brief Every dynamic body, awake, asleep or frozen, in ascending id order. Replaces the content of `out`.
        void GetDynamicBodies(std::vector<DynamicBodyInfo>& out) const;
        /// @brief Contacts that began during the Step() calls since the last call.
        std::vector<ContactEvent> TakeContactEvents();
        uint32_t BodyCount() const;
        uint32_t ActiveBodyCount() const;
        uint32_t FrozenBodyCount() const;

        // --- characters --------------------------------------------------------------------------
        CharacterId CreateCharacter(const CharacterDesc& desc);
        void DestroyCharacter(CharacterId character);
        /**
         * @brief Moves a character for one fixed step.
         * @param desiredVelocity Horizontal velocity the character wants (m/s); y is ignored.
         * @param jumpSpeed If > 0 and the character is on the ground, it takes off with this vertical speed.
         */
        void UpdateCharacter(CharacterId character, float dt, const glm::vec3& desiredVelocity, float jumpSpeed = 0.0f);
        CharacterState GetCharacterState(CharacterId character) const;
        void SetCharacterPosition(CharacterId character, const glm::vec3& feetPosition);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
