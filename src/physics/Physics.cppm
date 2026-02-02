module;

#include <memory>
#include <vector>
#include <glm/glm.hpp>

export module vortex.physics;

export import :collider_builder;
import vortex.voxel;


namespace vortex::physics {

    /**
     * @brief Handle for a physics body to allow updates/destruction later.
     */
    export struct BodyHandle {
        uint32_t id;
        bool IsValid() const { return id != 0xFFFFFFFF; }
    };

    /**
     * @brief Wrapper around the Jolt Physics System.
     */
    export class PhysicsSystem {
    public:
        PhysicsSystem();
        ~PhysicsSystem();

        /**
         * @brief Initializes the physics engine (allocators, job system, etc.).
         */
        void Initialize();

        /**
         * @brief Shuts down the physics engine.
         */
        void Shutdown();

        /**
         * @brief Steps the physics simulation.
         * @param deltaTime Time elapsed since last frame.
         */
        void Update(float deltaTime);

        /**
         * @brief Creates a physics body from a VoxelEntity.
         * @param entity The entity to physicalize.
         * @param isStatic Whether the body is immovable (Static) or movable (Dynamic).
         * @return A handle to the created body.
         */
        BodyHandle AddBody(std::shared_ptr<vortex::voxel::VoxelEntity> entity, bool isStatic);
        
        /**
         * @brief Removes and destroys a physics body from the simulation.
         * @param handle The handle of the body to remove.
         */
        void RemoveBody(BodyHandle handle);

        /**
         * @brief Syncs the physics body's transform back to the VoxelEntity (for dynamic objects).
         */
        void SyncBodyTransform(std::shared_ptr<vortex::voxel::VoxelEntity> entity, BodyHandle handle);

        /**
         * @brief Changes the body's motion type temporarily (e.g. for Gizmo manipulation).
         * @param isKinematic If true, the body is moved manually and ignores forces.
         */
        void SetBodyKinematic(BodyHandle handle, bool isKinematic);

        /**
         * @brief Sets the fundamental type of the body (Static vs Dynamic).
         */
        void SetBodyType(BodyHandle handle, bool isStatic);

        /**
         * @brief Sets whether the body is a trigger (Sensor).
         * @param isTrigger If true, body detects overlaps but has no collision response.
         */
        void SetBodySensor(BodyHandle handle, bool isTrigger);

        /**
         * @brief Forces the physics body to a new transform (Teleport).
         */
        void SetBodyTransform(BodyHandle handle, const glm::mat4& transform);

    private:
        struct InternalState;
        std::unique_ptr<InternalState> m_Internal;
    };
}