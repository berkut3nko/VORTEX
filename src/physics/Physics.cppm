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
     * @brief Wrapper around Jolt Physics System.
     */
    export class PhysicsSystem {
    public:
        PhysicsSystem();
        ~PhysicsSystem();

        void Initialize();
        void Shutdown();
        void Update(float deltaTime);

        BodyHandle AddBody(std::shared_ptr<vortex::voxel::VoxelEntity> entity, bool isStatic);
        
        /**
         * @brief Removes and destroys a physics body from the simulation.
         */
        void RemoveBody(BodyHandle handle);

        void SyncBodyTransform(std::shared_ptr<vortex::voxel::VoxelEntity> entity, BodyHandle handle);

        /**
         * @brief Changes the body's motion type temporarily (e.g. for Gizmo manipulation).
         */
        void SetBodyKinematic(BodyHandle handle, bool isKinematic);

        /**
         * @brief Sets the fundamental type of the body (Static vs Dynamic).
         */
        void SetBodyType(BodyHandle handle, bool isStatic);

        /**
         * @brief Sets whether the body is a trigger (Sensor).
         * @param isTrigger If true, body detects overlaps but doesn't block.
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