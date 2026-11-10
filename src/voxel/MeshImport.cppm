module;

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

export module vortex.voxel:meshimport;

import :volume;
import :material;

export namespace vortex::voxel {

    /// @brief Triangle with a voxel id, input of the voxeliser.
    struct MeshTriangle {
        glm::vec3 a{0.0f}, b{0.0f}, c{0.0f};   ///< Positions in voxel units.
        uint8_t material = 1;
    };

    struct VoxelizeResult {
        VoxelVolume volume;
        glm::vec3 origin{0.0f};      ///< Position (voxel units, input space) of the volume's minimum corner.
    };

    /**
     * @brief Conservative surface voxelisation of a triangle soup (triangle/box overlap test),
     * optionally followed by an interior fill so closed meshes become solid objects.
     * @details Cost is proportional to the voxels each triangle touches (every triangle only
     * visits its own bounding box), not triangles x chunks.
     * @param fillInterior Flood-fills the outside from the volume border; everything not reached
     * is inside and receives the id of the nearest surface voxel on its row.
     * @param maxSize Volumes larger than this on any axis are rejected (returns an empty volume).
     */
    VoxelizeResult VoxelizeTriangles(std::span<const MeshTriangle> triangles, bool fillInterior = true,
                                     const glm::ivec3& maxSize = glm::ivec3(512));

    struct MeshImportSettings {
        std::filesystem::path file;         ///< .gltf or .glb
        float voxelsPerUnit = 10.0f;        ///< Mesh unit (metre) -> voxels.
        bool fillInterior = true;
        int maxVoxelsPerAxis = 512;
    };

    struct MeshImportResult {
        bool ok = false;
        std::string error;
        VoxelVolume volume;
        std::vector<Material> materials;    ///< Material of voxel id i + 1 (base colour from the glTF material / texture average).
    };

    /**
     * @brief Loads a glTF 2.0 model and voxelises it.
     * @details Honours node transforms (scene hierarchy), accessor byte strides and all index
     * component types; the colour of a textured material is the average of its base colour texture.
     */
    MeshImportResult ImportMesh(const MeshImportSettings& settings);
}
