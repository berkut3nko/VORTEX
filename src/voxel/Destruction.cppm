module;

#include <vector>
#include <memory>
#include <glm/glm.hpp>

export module vortex.voxel:destruction;

import :entity;
import :chunk;
import :object;
import :palette; // Import Palette to access material properties

namespace vortex::voxel {

    /**
     * @brief Represents a fragment of a voxel entity discovered during connectivity analysis.
     */
    export struct Island {
        std::vector<glm::ivec3> voxelPositions;
        std::vector<uint8_t> materialIDs;
        bool isAnchored = false;
    };

    /**
     * @brief SHRED (Structural Hierarchy for Real-time Entity Destruction) system.
     * @details Analyzes voxel connectivity and structural integrity. 
     */
    export class SHREDSystem {
    public:
        SHREDSystem() = default;

        /**
         * @brief Analyzes an entity for disconnected parts (islands).
         * @param entity The entity to analyze.
         * @return A list of discovered islands.
         */
        static std::vector<Island> AnalyzeConnectivity(const std::shared_ptr<VoxelEntity>& entity);

        /**
         * @brief Splits an entity into multiple new entities based on discovered islands.
         * @param original The source entity.
         * @param islands List of fragments to convert into new entities.
         * @return Vector of new standalone entities.
         */
        static std::vector<std::shared_ptr<VoxelEntity>> SplitEntity(const std::shared_ptr<VoxelEntity>& original, const std::vector<Island>& islands);

        /**
         * @brief Checks the structural integrity of the entity considering material weight and strength.
         * @details Uses a load distribution algorithm. If stress exceeds strength, voxels are destroyed.
         * @param entity The entity to check.
         * @param palette The material palette to lookup density and strength.
         * @return True if any voxels were broken (structure changed), requiring a re-split.
         */
        static bool ValidateStructuralIntegrity(std::shared_ptr<VoxelEntity> entity, const MaterialPalette& palette);

        /**
         * @brief Checks if a voxel position is considered "anchored" to the ground.
         */
        static bool CheckAnchoring(const glm::vec3& worldPos);
    };
}