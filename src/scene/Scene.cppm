module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

export module vortex.scene;

export import :components;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;

export namespace vortex::scene {

    /// @brief Tuning of a scene. The defaults reproduce the engine's destruction behaviour.
    struct SceneConfig {
        float voxelSize = 0.1f;               ///< Metres per voxel of every voxel object (Transform scale). Use the world's.
        /// Connected pieces with fewer voxels never become entities: they turn into particles at
        /// once (Damage, CreateVoxelObject with `debris`). 16 voxels of 0.1 m are a lump the size
        /// of a fist; anything smaller is not worth a rigid body and chunk memory.
        uint32_t minPieceVoxels = 16;
        /// Non-static bodies with fewer solid voxels are "small": they use physics::Layer::Debris
        /// (no collisions among each other) with continuous collision detection (they are thin
        /// enough to tunnel through the terrain mesh otherwise), and - if they are rubble - get
        /// the Debris component.
        uint32_t debrisBodyVoxels = 48;
        /// Dynamic bodies whose solid voxels span fewer voxels than this along any axis are
        /// "thin" and use continuous collision detection as well, whatever their voxel count: a
        /// slab or plank falling flat passes through the terrain mesh within one step just like a
        /// small lump. 6 voxels of 0.1 m need 36 m/s (a 66 m fall) to do that.
        uint32_t thinBodyVoxels = 6;
        /// When Damage leaves less than `debrisBodyVoxels` of a dynamic object, the remnant - the
        /// damaged entity itself - becomes rubble like the pieces that came off, and is cleaned
        /// up. false: the damaged entity never becomes rubble (remnants of every destroyed crate
        /// then stay for good). To keep one particular entity, remove its Debris component after
        /// Damage; static and kinematic objects are never affected.
        bool debrisRemnants = true;
        /// false: the scene never adds the Debris component by itself - not to small pieces that
        /// Damage splits off, not to the remnant, not to objects created with
        /// VoxelObjectDesc::debris - so Update() removes nothing as rubble (a Debris component
        /// the game adds by hand still works). Pieces below `minPieceVoxels` still never become
        /// entities. This is the scene of a session server (sim::ServerConfig): which entities
        /// exist there is decided by the server's rules, never by a viewer-dependent clean-up.
        bool markDebris = true;
        /// false: the scene never bakes a model into the chunk store (CreateVoxelObject, the
        /// pieces made by Damage). Models keep their voxels and get their bodies; they just hold
        /// no chunk slots, because nothing is drawn or ray traced: a session server. A model
        /// that somebody else baked is used as it is.
        bool bakeModels = true;
        /**
         * Replicated-piece threshold (docs/design/networking.md, "Entity replication"). 0 = off:
         * Damage makes an entity of every piece of at least `minPieceVoxels` (an offline game).
         * Otherwise a piece with fewer solid voxels is not made into an entity: Damage returns it
         * in DamageResult::fragments (model, pose, velocities) for whoever shows rubble - a
         * client, or Engine::PresentBlast offline - to create locally. The damaged entity itself
         * goes the same way when what is left of it is below the threshold and Dynamic (a
         * Static / Kinematic object only dented stays). sim::ServerConfig sets 512.
         */
        uint32_t replicatedPieceVoxels = 0;
        uint32_t debrisParticleStride = 3;    ///< One debris particle burst per this many carved voxels (>= 1).

        // --- rubble clean-up (entities with the Debris component), applied by Update() ---
        float debrisLifetime = 15.0f;         ///< Seconds a piece of rubble lives. <= 0: no time limit.
        /// Rubble that rests (asleep or frozen) farther than this from the viewer (SetViewer) is
        /// removed as soon as it is older than debrisFarLifetime. <= 0, or no viewer: rule off.
        float debrisFarDistance = 25.0f;
        float debrisFarLifetime = 2.0f;       ///< Seconds; see debrisFarDistance.
        uint32_t maxDebris = 192;             ///< Most rubble entities alive at once; the oldest are removed first.
        uint32_t debrisDespawnBursts = 3;     ///< Particle bursts (fx::SpawnVoxelBreak) left by removed rubble; 0 = none.
    };

    /// @brief What to create with Scene::CreateVoxelObject.
    struct VoxelObjectDesc {
        std::shared_ptr<voxel::VoxelModel> model;       ///< Required. Baked into the scene's store if it is not baked yet (SceneConfig::bakeModels).
        glm::vec3 position{0.0f};                       ///< World metres: where the model origin (minimum corner) goes.
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        physics::MotionType motion = physics::MotionType::Dynamic;
        bool withBody = true;                           ///< false: visual only (no collision, immune to damage).
        glm::vec3 linearVelocity{0.0f};                 ///< Initial velocities of the body (ignored without one).
        glm::vec3 angularVelocity{0.0f};
        bool visible = true;
        std::string name;                               ///< Empty: the model's name. No Name component if both are empty.
        /// The object is rubble (terrain or wreckage that broke off). A dynamic one with a body
        /// then follows the debris rules of SceneConfig: below `minPieceVoxels` no entity is
        /// created at all - the voxels become particles and kNoEntity is returned, which is not
        /// an error; below `debrisBodyVoxels` it gets the Debris component and is cleaned up
        /// (unless SceneConfig::markDebris is off).
        bool debris = false;
    };

    /// @brief What to create with Scene::CreateLight.
    struct LightDesc {
        glm::dvec3 position{0.0};             ///< World metres.
        glm::vec3 color{1.0f, 0.8f, 0.6f};
        float intensity = 5.0f;
        float radius = 8.0f;
        float sourceRadius = 0.1f;
        float lifetime = 0.0f;                ///< Seconds; 0 = persistent (no Lifetime component).
        float decay = 0.0f;                   ///< Lifetime::lightDecay; only used when lifetime > 0.
    };

    /// @brief A piece of a damaged object below SceneConfig::replicatedPieceVoxels: no entity
    /// was made for it. Everything needed to create it as local rubble (VoxelObjectDesc with
    /// `debris = true`, `position` / `rotation` from `pose`, the two velocities).
    struct DamageFragment {
        std::shared_ptr<voxel::VoxelModel> model;   ///< The piece, cropped; never baked.
        math::Transform pose;                       ///< World pose of the model's minimum corner (scale = voxel size).
        glm::vec3 linearVelocity{0.0f};             ///< Of the original body at the piece's centre.
        glm::vec3 angularVelocity{0.0f};
    };

    /// @brief Outcome of Scene::Damage.
    struct DamageResult {
        uint32_t removed = 0;             ///< Voxels carved out by the sphere. 0 = nothing happened at all.
        uint32_t loose = 0;               ///< Further voxels that fell off as fragments too small to be a piece (particles).
        /// The damaged entity no longer exists: nothing big enough was left, or (out of physics
        /// bodies) its piece could not get a body and was dropped. In the second case `pieces`
        /// can still list entities that did get one, so do not read this as "nothing remains".
        bool destroyed = false;
        std::vector<Entity> pieces;       ///< NEW entities split off (largest first). The largest piece overall keeps the damaged entity and is not listed.
        /// Pieces below SceneConfig::replicatedPieceVoxels, largest first (empty while it is 0).
        std::vector<DamageFragment> fragments;
        /// The damaged entity itself was below the threshold: it was destroyed (`destroyed`) and
        /// its remaining voxels are the first entry of `fragments`.
        bool toRubble = false;
    };

    /**
     * @brief The entities of a game world: an EnTT registry plus the systems that tie voxel
     * models, rigid bodies, lights and lifetimes together.
     * @details
     *  - The scene only *references* the chunk store, material library, physics world and
     *    particle system it is constructed from. The module imports nothing of the renderer
     *    (it is part of the GPU-free library Vortex::Sim), so everything here runs in unit
     *    tests and on a session server. What the renderer needs is read from the registry by
     *    vortex::GatherInstances / vortex::GatherLights in vortex.engine.
     *  - **Destruction order**: a Scene must be destroyed (or `Clear()`ed) before the ChunkStore
     *    and before `PhysicsWorld::Shutdown()` / destruction, because entities own bodies and
     *    model references (chunk slots). `~Scene` calls `Clear()`.
     *  - **Entity lifetime is real**: destroying an entity destroys its body and drops its model
     *    reference; ids are recycled with a new version. Every function taking an Entity accepts
     *    kNoEntity and stale ids and then does nothing / returns the documented default.
     *  - **Bodies map back to entities**: the user data of every body created here is
     *    `ToUserData(entity)`, so `physics::RayHit::userData` and `ContactEvent::userDataA/B`
     *    resolve through `EntityFromUserData`.
     *  - **Creating while iterating**: `Damage` and the `Create*` functions add entities and
     *    components. Never call them from inside a registry view / `each` loop over the same
     *    components (references and iterators may be invalidated, and new pieces could be visited
     *    again). Take a snapshot first - `QueryBodies` exists for exactly that - and iterate the
     *    snapshot; entries that died in the meantime are harmless because stale ids are safe.
     *  - Main-thread only.
     *
     * Frame protocol (what vortex::Engine does; the physics and lifetime steps are
     * sim::Simulation::StepPhysics and UpdateLifetimes):
     * @code
     *   scene.BeginFrame();                    // previous = current, once per rendered frame
     *   for each fixed step:
     *       app.OnFixedUpdate(); terrain.GuardBodies(); physics.Step(dt);
     *       scene.SyncFromPhysics();           // bodies -> Transform::current
     *   app.OnUpdate();
     *   GatherInstances(scene, instances); GatherLights(scene, lights);  -> renderer (vortex.engine)
     *   scene.SetViewer(camera); scene.Update(frameDt);   // lifetimes, light fades, rubble (after the frame was drawn)
     * @endcode
     * Without a renderer (sim::Simulation::Step, a session server) there is no BeginFrame and no
     * viewer: every tick is SyncFromPhysics, then Update(fixed step).
     */
    class Scene {
    public:
        Scene(voxel::ChunkStore& store, const voxel::MaterialLibrary& materials, physics::PhysicsWorld& physics,
              fx::ParticleSystem& particles, const SceneConfig& config = {});
        ~Scene();
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        const SceneConfig& Config() const;

        // --- entities ----------------------------------------------------------------------------

        /**
         * @brief Creates a voxel object: Transform (previous = current, scale = voxel size),
         * VoxelBody, Name (if any) and - with `withBody` - a RigidBody.
         * @details The body is a compound of the model's collider boxes with material densities
         * (mass and inertia are real); layer Static for static bodies, Debris below
         * `debrisBodyVoxels` solid voxels, Dynamic otherwise. A model without solid voxels gets
         * no body. Requesting a body before the physics world is initialised is an error.
         * With SceneConfig::bakeModels off the model is not baked and no chunk slot is used.
         * @return kNoEntity (and a log line) if the model is null, could not be baked because
         * the store ran out of slots, or the requested body could not be created; nothing is
         * leaked in that case (a model that could not be baked is left unbaked and holds no
         * chunk slots, so the same model can be used again once the store has room).
         */
        Entity CreateVoxelObject(const VoxelObjectDesc& desc);

        /// @brief Creates a light entity: Transform (scale 1) + PointLight, plus Lifetime when
        /// `desc.lifetime > 0` (an explosion flash). Never fails.
        Entity CreateLight(const LightDesc& desc);

        /// @brief Destroys an entity now: its body is removed from the physics world and its
        /// model reference released. No-op for kNoEntity / stale ids.
        void Destroy(Entity entity);

        /// @brief Destroys every entity (bodies and model references included).
        void Clear();

        /// @brief True if the id refers to a living entity of this scene.
        bool IsValid(Entity entity) const;

        size_t EntityCount() const;          ///< Living entities of any kind.
        size_t VoxelObjectCount() const;     ///< Living entities with a VoxelBody (visible or not).
        size_t DebrisCount() const;          ///< Living entities with the Debris component.

        /**
         * @brief Direct access to the registry for game components, views and anything the
         * functions below do not cover.
         * @details Rules: RigidBody is managed by the scene (see the component); destroy entities
         * with `Destroy` or `registry.destroy` at will; do not call scene functions that create
         * entities while iterating a view.
         */
        entt::registry& Registry();
        const entt::registry& Registry() const;

        /// @brief Component of an entity, or nullptr if the entity is invalid / stale or has no
        /// such component. The pointer is invalidated by anything that creates or destroys
        /// entities or components of that type: do not keep it.
        template <typename Component>
        Component* TryGet(Entity entity) {
            entt::registry& registry = Registry();
            return registry.valid(entity) ? registry.try_get<Component>(entity) : nullptr;
        }
        template <typename Component>
        const Component* TryGet(Entity entity) const {
            const entt::registry& registry = Registry();
            return registry.valid(entity) ? registry.try_get<Component>(entity) : nullptr;
        }

        /**
         * @brief Places an entity: writes Transform::current (scale untouched) and moves its body,
         * if it has one (velocities are kept, the body is woken up).
         * @param teleport true: previous = current as well, so the renderer sees no motion (use it
         * for cuts and for entities you position every frame yourself); false: the change shows
         * up as motion from the pose drawn last frame.
         */
        void SetTransform(Entity entity, const glm::vec3& position, const glm::quat& rotation, bool teleport = true);

        // --- physics <-> entities ----------------------------------------------------------------

        /// @brief Living entity behind a body's user data (ray hits, contact events); kNoEntity
        /// if the value is not from a scene or the entity is gone.
        Entity EntityFromUserData(uint64_t userData) const;

        /// @brief Living entity that owns `body`; kNoEntity for invalid, foreign or dead bodies.
        Entity EntityFromBody(physics::BodyId body) const;

        /**
         * @brief Snapshot of the entities whose rigid body bounds overlap a world-space box.
         * @details Appends to `out`. This is the safe way to "do something to everything near a
         * point" when the something creates or destroys entities (Damage): collect first, then act
         * on the list. Pieces created while acting are not in the list, so nothing is hit twice.
         */
        void QueryBodies(const math::AABB& box, std::vector<Entity>& out) const;

        // --- destruction -------------------------------------------------------------------------

        /**
         * @brief Carves a sphere (world metres) out of a voxel object and lets it fall apart.
         * @details
         *  - Only entities with a VoxelBody AND a RigidBody are affected (visual-only objects are
         *    immune). Voxels of materials flagged MaterialFlag_Indestructible stay.
         *  - The shared model is never modified: the entity gets a private new model, other users
         *    of the old model are untouched (and the old one is freed with its last user).
         *  - The remaining voxels are split into face-connected pieces. The largest keeps this
         *    entity - id, Name and any other components; every other piece becomes a new Dynamic
         *    entity (Transform, VoxelBody, RigidBody, Name copy). The entity keeps its motion type
         *    only while it stays in one piece: a Static / Kinematic object that is cut apart
         *    becomes Dynamic as a whole, so no severed part is left hanging in the air. All pieces
         *    stay exactly where their voxels were and inherit the motion of the original body:
         *    linear velocity = velocity of the original at the piece's centre, same angular velocity.
         *  - `Transform::previous` of every piece is the original's previous pose re-based by the
         *    piece's voxel offset, so motion vectors continue seamlessly through the cut.
         *  - Pieces smaller than `minPieceVoxels` turn into particles moving with the object;
         *    carved voxels throw debris particles away from `center`.
         *  - New pieces smaller than `debrisBodyVoxels` are rubble: they get the Debris component
         *    and are cleaned up by Update(). So does the damaged entity itself once what is left
         *    of it is that small and dynamic (`SceneConfig::debrisRemnants`); otherwise it keeps
         *    its identity and components, Debris included if it had it. With
         *    `SceneConfig::markDebris` off nothing is marked: every piece of at least
         *    `minPieceVoxels` stays until somebody destroys it.
         *  - If nothing big enough is left the entity is destroyed (`destroyed = true`).
         *  - With `SceneConfig::replicatedPieceVoxels` > 0, pieces below it get no entity and are
         *    returned in `fragments` instead; so is the damaged entity's own piece when it is that
         *    small and Dynamic (`toRubble`, `destroyed`). The particles of this call are spawned
         *    as before (into an empty pool on a server).
         *  - The old model is released before the pieces are baked, so an unshared object never
         *    runs out of chunk slots by being damaged. A piece whose body cannot be created
         *    (PhysicsConfig::maxBodies reached) is dropped with a log line.
         * @return What happened; `removed == 0` (and no other effect) if the sphere touched no
         * destructible voxel or the entity is invalid, stale or not damageable.
         */
        DamageResult Damage(Entity entity, const glm::dvec3& center, float radius);

        // --- per frame ---------------------------------------------------------------------------

        /// @brief Remembers what was drawn: `previous = current` for every Transform. Call once
        /// per rendered frame, before that frame's simulation steps and game update.
        void BeginFrame();

        /// @brief Pulls position and rotation of every entity with a valid RigidBody from the
        /// physics world into Transform::current. Call after each PhysicsWorld::Step.
        void SyncFromPhysics();

        /**
         * @brief Advances lifetimes by `dt` seconds: fades lights (Lifetime::lightDecay) and
         * destroys entities whose time is up. Then cleans up rubble (Debris component): pieces
         * older than `debrisLifetime`, resting pieces far from the viewer, and the oldest pieces
         * beyond `maxDebris` are destroyed, each leaving `debrisDespawnBursts` particle bursts.
         */
        void Update(float dt);

        /// @brief Where the player looks from (world metres); enables the SceneConfig::debrisFarDistance rule.
        void SetViewer(const glm::dvec3& position);

        /**
         * @brief Destroys the entities that fell out of the world: those whose dynamic body lies
         * entirely below `height` (world metres, +Y up) and is on its way down - awake, or frozen
         * by the terrain guard while it was moving - and those whose body has a non-finite
         * position. Static and kinematic bodies and dynamic ones that rest there (asleep, or
         * parked asleep by the guard) were put or left there on purpose and stay. Logs the count.
         * @return Number of entities destroyed.
         */
        uint32_t DestroyBelow(float height);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
