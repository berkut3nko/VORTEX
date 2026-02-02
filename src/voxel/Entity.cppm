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
     */
    export struct VoxelEntity {
        std::string name = "Entity";
        glm::mat4 transform{1.0f};
        std::vector<std::shared_ptr<VoxelObject>> parts;

        glm::vec3 localBoundsMin{0.0f};
        glm::vec3 localBoundsMax{0.0f};
        glm::vec3 logicalCenter{0.0f};
        uint32_t totalVoxelCount{0};

        // --- Destruction & Physics ---
        
        /// @brief If true, the engine will run SHRED analysis on this object.
        bool isDestructible = true;
        
        /// @brief If true, object is immovable and ignores forces.
        bool isStatic = false;
        
        bool isTrigger = false;
        bool shouldRebuildPhysics = false;

        // --- Velocity Inheritance ---
        /// @brief Temporary storage for linear velocity to apply on physics creation.
        glm::vec3 cachedLinearVelocity{0.0f};
        
        /// @brief Temporary storage for angular velocity.
        glm::vec3 cachedAngularVelocity{0.0f};

        virtual ~VoxelEntity() = default;

        /**
         * @brief @brief Recalculates stats.
         */
        void RecalculateStats() {
            totalVoxelCount = 0;
            glm::vec3 weightedCenterSum(0.0f);
            localBoundsMin = glm::vec3(std::numeric_limits<float>::max());
            localBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());

            for(auto& p : parts) {
                if(p && p->chunk) {
                    p->RecalculateCenter();
                    weightedCenterSum += (p->position + p->logicalCenter) * static_cast<float>(p->voxelCount);
                    totalVoxelCount += p->voxelCount;

                    localBoundsMin = glm::min(localBoundsMin, p->position);
                    localBoundsMax = glm::max(localBoundsMax, p->position + glm::vec3(32.0f));
                }
            }

            if (totalVoxelCount > 0) logicalCenter = weightedCenterSum / static_cast<float>(totalVoxelCount);
        }
        
        glm::vec3 GetLocalCenter() const { return logicalCenter; }
    };
}