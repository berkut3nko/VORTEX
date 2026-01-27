module;

#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include <limits>

export module vortex.voxel:entity;

import :object;

namespace vortex::voxel {

    /**
     * @brief Represents a generic entity in the voxel world.
     * @details Can be a procedural object or a base for imported meshes.
     * Acts as the root container for multiple VoxelObjects (parts).
     */
    export struct VoxelEntity {
        /// @brief Display name of the entity.
        std::string name = "Entity";
        
        /// @brief Root transformation matrix (Position, Rotation, Scale).
        glm::mat4 transform{1.0f};

        /// @brief Constituent voxel parts (Chunks) relative to the root.
        std::vector<std::shared_ptr<VoxelObject>> parts;

        /// @brief Local AABB minimum bound for raycasting/selection.
        glm::vec3 localBoundsMin{0.0f};
        /// @brief Local AABB maximum bound for raycasting/selection.
        glm::vec3 localBoundsMax{0.0f};

        /// @brief Center of mass used as the Gizmo pivot point.
        glm::vec3 logicalCenter{0.0f};
        
        /// @brief Total number of solid voxels across all parts.
        uint32_t totalVoxelCount{0};

        // --- Physics Properties ---
        
        /// @brief If true, object is immovable (Bedrock) and ignores forces.
        bool isStatic = false;
        
        /// @brief If true, object detects overlaps but has no collision response (Ghost/Sensor).
        bool isTrigger = false;
        
        /// @brief Flag to signal the physics system to regenerate the body (e.g., after re-meshing).
        bool shouldRebuildPhysics = false;

        virtual ~VoxelEntity() = default;

        /**
         * @brief Recalculates the center of mass and bounding box based on constituent parts.
         * @details Should be called after modifying parts or their chunks.
         */
        void RecalculateStats() {
            totalVoxelCount = 0;
            glm::vec3 weightedCenterSum(0.0f);
            
            // Reset bounds
            localBoundsMin = glm::vec3(std::numeric_limits<float>::max());
            localBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());
            bool hasParts = false;

            for(auto& p : parts) {
                if(p && p->chunk) {
                    hasParts = true;
                    p->RecalculateCenter();
                    
                    // Center in entity space
                    glm::vec3 partCenter = p->position + p->logicalCenter;
                    weightedCenterSum += partCenter * static_cast<float>(p->voxelCount);
                    totalVoxelCount += p->voxelCount;

                    // Update bounds (approximate via chunks for now, or explicit)
                    // Assuming p->position is the corner of the chunk
                    glm::vec3 pMin = p->position;
                    glm::vec3 pMax = p->position + glm::vec3(32.0f);
                    
                    localBoundsMin = glm::min(localBoundsMin, pMin);
                    localBoundsMax = glm::max(localBoundsMax, pMax);
                }
            }

            if (totalVoxelCount > 0) {
                logicalCenter = weightedCenterSum / static_cast<float>(totalVoxelCount);
            } else if (hasParts) {
                logicalCenter = (localBoundsMin + localBoundsMax) * 0.5f;
            } else {
                logicalCenter = glm::vec3(0.0f);
                localBoundsMin = glm::vec3(0.0f);
                localBoundsMax = glm::vec3(0.0f);
            }
        }
        
        /**
         * @brief Gets the calculated local center (center of mass).
         */
        glm::vec3 GetLocalCenter() const { return logicalCenter; }
    };
}