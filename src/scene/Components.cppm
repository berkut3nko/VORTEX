module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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

    /**
     * @brief Marks a voxel object as rubble: a small piece left over by destruction that nobody
     * will miss. Scene::Update removes rubble (see SceneConfig: debrisLifetime, debrisFarDistance,
     * maxDebris) and leaves a puff of particles in its place.
     * @details Added by the scene to small pieces that Scene::Damage splits off, to what is left
     * of the damaged object once that is small (SceneConfig::debrisRemnants) and to small
     * objects created with VoxelObjectDesc::debris. Remove the component to keep such a piece
     * for good; add it to any voxel object to have it cleaned up.
     */
    struct Debris {
        float age = 0.0f;             ///< Seconds since it became rubble. The oldest go first.
    };
}
