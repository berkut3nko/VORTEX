module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

export module vortex.voxel:object;

import :chunk;

namespace vortex::voxel {

    /**
     * @brief Represents a single Voxel Object (typically one chunk) within an Entity.
     */
    export struct VoxelObject {
        /// @brief Position relative to the Entity root.
        glm::vec3 position{0.0f};
        /// @brief Rotation relative to the Entity root.
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Scale relative to the Entity root.
        glm::vec3 scale{1.0f};

        /// @brief Logical Center (Center of Mass) relative to Chunk Origin (0,0,0).
        glm::vec3 logicalCenter{16.0f}; // Default to center of 32x32x32 block
        
        /// @brief Number of solid voxels in this object.
        uint32_t voxelCount{0};

        /// @brief The actual voxel data container.
        std::shared_ptr<Chunk> chunk;
        
        /// @brief Per-object static flag (often overridden by Entity).
        bool isStatic = false;
        
        /**
         * @brief Computes the local transformation matrix for this part.
         * @return Model matrix (Translation * Rotation * Scale).
         */
        glm::mat4 GetTransformMatrix() const {
            glm::mat4 mat = glm::mat4(1.0f);
            mat = glm::translate(mat, position);
            mat = mat * glm::mat4_cast(rotation);
            mat = glm::scale(mat, scale);
            return mat;
        }

        /**
         * @brief Recalculates the logical center based on voxel positions within the chunk.
         * @details Should be called after modifying the chunk content.
         */
        void RecalculateCenter() {
            if (!chunk) return;

            glm::vec3 sumPos(0.0f);
            uint32_t count = 0;

            for (int z = 0; z < 32; ++z) {
                for (int y = 0; y < 32; ++y) {
                    for (int x = 0; x < 32; ++x) {
                        if (chunk->GetVoxel(x, y, z) != 0) {
                            // Add +0.5 to use the center of the voxel, not the corner
                            sumPos += glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f);
                            count++;
                        }
                    }
                }
            }

            if (count > 0) {
                logicalCenter = sumPos / (float)count;
            } else {
                logicalCenter = glm::vec3(16.0f); // Fallback to geometric center
            }
            voxelCount = count;
        }
    };
}