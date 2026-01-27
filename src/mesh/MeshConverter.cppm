module;

#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

export module vortex.voxel:mesh_converter;

import :material; // Use internal PhysicalMaterial instead of SceneMaterial
import :object;

namespace vortex::voxel {

    export struct MeshImportSettings {
        std::string filePath;
        float scale = 1.0f;
    };

    export struct MeshImportResult {
        std::vector<std::shared_ptr<VoxelObject>> parts;
        // Use PhysicalMaterial to avoid dependency on vortex.graphics
        std::vector<PhysicalMaterial> materials;
        glm::vec3 minBound;
        glm::vec3 maxBound;
    };

    export class MeshConverter {
    public:
        /**
         * @brief Voxelizes a mesh and extracts simple materials (base color).
         */
        static MeshImportResult Import(const MeshImportSettings& settings);
    };
}