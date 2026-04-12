module;

#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

export module vortex.voxel:dynamic_mesh;

import :entity;
import :mesh_converter;
import :material; 

namespace vortex::voxel {

    /**
     * @brief Specialization of VoxelEntity representing an imported 3D mesh.
     * @details Stores import settings to allow re-meshing at runtime.
     */
    export struct DynamicMeshObject : public VoxelEntity {
        /// @brief Configuration used to import this mesh.
        MeshImportSettings importSettings;

        /// @brief Materials specific to this mesh instance.
        /// @details These will be appended to the global palette during Scene Upload.
        std::vector<PhysicalMaterial> materials;

        /**
         * @brief Re-runs the voxelization process using the current settings.
         * @details Updates `parts` and `materials`, then recalculates statistics.
         * Can be slow for large meshes.
         */
        void Remesh() {
            MeshImportResult result = MeshConverter::Import(importSettings);
            
            parts = result.parts;
            materials = result.materials;

            // Normalize bounds to local space
            glm::vec3 size = result.maxBound - result.minBound;
            localBoundsMin = glm::vec3(0.0f);
            localBoundsMax = size;

            RecalculateStats();
        }
    };
}