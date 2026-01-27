module;

#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

export module vortex.voxel:entity;

import :object;

namespace vortex::voxel {

    /**
     * @brief Represents a generic entity in the voxel world.
     * @details Can be a procedural object or a base for imported meshes.
     */
    export struct VoxelEntity {
        std::string name = "Entity";
        
        // Root transformation matrix (Position, Rotation, Scale)
        glm::mat4 transform{1.0f};

        // Constituent voxel parts (Chunks) relative to the root
        std::vector<std::shared_ptr<VoxelObject>> parts;

        // Local AABB bounds for raycasting/selection
        glm::vec3 localBoundsMin{0.0f};
        glm::vec3 localBoundsMax{0.0f};

        // Center of mass for Gizmo pivot
        glm::vec3 logicalCenter{0.0f};
        uint32_t totalVoxelCount{0};

        // --- Physics Properties ---
        bool isStatic = false;       ///< If true, object is immovable (Bedrock).
        bool isTrigger = false;      ///< If true, object detects overlaps but has no collision response.
        bool shouldRebuildPhysics = false; ///< Set to true to force physics body regeneration (e.g. after Re-mesh).

        virtual ~VoxelEntity() = default;

        /**
         * @brief Recalculates center of mass and bounds based on parts.
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
        
        glm::vec3 GetLocalCenter() const { return logicalCenter; }
    };
}