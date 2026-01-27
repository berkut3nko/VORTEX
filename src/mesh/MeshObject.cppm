module;

#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

export module vortex.voxel:dynamic_mesh;

import :entity;
import :mesh_converter;
import :material; // Use internal PhysicalMaterial

namespace vortex::voxel {

    export struct DynamicMeshObject : public VoxelEntity {
        MeshImportSettings importSettings;

        // Materials specific to this mesh
        // These will be appended to the global palette during Scene Upload
        std::vector<PhysicalMaterial> materials;

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