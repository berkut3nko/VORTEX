module;

#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

export module vortex.voxel:mesh_converter;

import :material; 
import :object;

namespace vortex::voxel {

    /**
     * @brief Settings for importing a 3D mesh.
     */
    export struct MeshImportSettings {
        /// @brief Path to the GLTF/GLB file.
        std::string filePath;
        
        /// @brief Scale factor applied during voxelization.
        float scale = 1.0f;
    };

    /**
     * @brief Result of a mesh import operation.
     */
    export struct MeshImportResult {
        /// @brief Generated voxel objects (chunks).
        std::vector<std::shared_ptr<VoxelObject>> parts;
        
        /// @brief Extracted materials from the mesh, converted to PhysicalMaterial.
        std::vector<PhysicalMaterial> materials;
        
        /// @brief AABB Minimum bound of the imported mesh.
        glm::vec3 minBound;
        
        /// @brief AABB Maximum bound of the imported mesh.
        glm::vec3 maxBound;
    };

    /**
     * @brief Utility class to convert standard 3D meshes into Voxel Entities.
     */
    export class MeshConverter {
    public:
        /**
         * @brief Voxelizes a mesh and extracts simple materials (base color).
         * @details Uses a triangle-box intersection test to generate voxel data.
         * @param settings Configuration for the import process.
         * @return The result containing voxel chunks and materials.
         */
        static MeshImportResult Import(const MeshImportSettings& settings);
    };
}