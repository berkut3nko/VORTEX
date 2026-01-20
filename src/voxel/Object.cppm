module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

export module vortex.voxel:object;

import :hierarchy; // Import Chunk definition

namespace vortex::voxel {

    /**
     * @brief Represents an instance of a voxel grid in the world.
     * @details Can be dynamic (physics-enabled) or static.
     */
    export struct VoxelObject {
        // --- Transform ---
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // Identity quaternion
        glm::vec3 scale{1.0f};

        // --- Data Reference ---
        // Using shared_ptr allows multiple objects to share the same voxel model (instancing).
        // For destructible objects, this should likely be unique_ptr or a copy-on-write mechanism.
        std::shared_ptr<Chunk> chunk;

        // --- Metadata ---
        bool isStatic = false;
        
        /**
         * @brief Computes the model matrix for rendering/physics.
         */
        glm::mat4 GetTransformMatrix() const {
            glm::mat4 mat = glm::mat4(1.0f);
            mat = glm::translate(mat, position);
            mat = mat * glm::mat4_cast(rotation);
            mat = glm::scale(mat, scale);
            return mat;
        }
    };
}