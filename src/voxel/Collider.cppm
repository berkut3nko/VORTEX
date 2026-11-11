module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

export module vortex.voxel:collider;

import :chunk;
import :volume;
import :material;

export namespace vortex::voxel {

    /// @brief Axis aligned box of solid voxels inside a volume (voxel units).
    struct ColliderBox {
        glm::ivec3 min{0};
        glm::ivec3 size{0};
        uint8_t material = 0;      ///< Voxel id of the box (first voxel's id when materials are merged).
    };

    /**
     * @brief Greedy decomposition of a volume into axis aligned boxes covering exactly its solid voxels.
     * @details The boxes are disjoint and their union equals the solid set (this is unit tested),
     * so they can be used both as a physics compound shape and to compute mass properties.
     * @param mergeMaterials true: a box may span different materials (fewest boxes, good for
     * collision). false: every box has a single material (needed for per-material density).
     */
    std::vector<ColliderBox> BuildColliderBoxes(const VoxelVolume& volume, bool mergeMaterials = true);

    /// @brief Mass and centre of mass of a volume.
    struct MassProperties {
        float mass = 0.0f;                 ///< Kilograms.
        glm::vec3 centerOfMass{0.0f};      ///< Voxel units, relative to the volume origin.
        uint32_t solidVoxels = 0;
    };

    /// @brief Sums material densities (kg/m^3) over all voxels: mass = sum(density) * voxelSize^3.
    MassProperties ComputeMassProperties(const VoxelVolume& volume, const MaterialLibrary& materials,
                                         PaletteId palette, float voxelSize);

    /// @brief Which voxel ids take part in collision (liquids, for example, do not).
    struct CollisionFilter {
        std::array<bool, 256> collidable;
        CollisionFilter() {
            collidable.fill(true);
            collidable[0] = false;
        }
        /// @brief Marks ids whose material has any of `excludedFlags` (e.g. MaterialFlag_Liquid) as non-collidable.
        static CollisionFilter FromMaterials(const MaterialLibrary& materials, PaletteId palette, uint32_t excludedFlags);
    };

    /// @brief Triangle mesh of the exposed surface of a chunk (static terrain collision).
    struct ChunkMesh {
        std::vector<glm::vec3> vertices;   ///< Chunk-local voxel units (0..32).
        std::vector<uint32_t> indices;     ///< Triangle list, counter-clockwise seen from outside (empty side).
        bool Empty() const { return indices.empty(); }
    };

    /**
     * @brief Builds the collision surface of a chunk with greedy quad merging.
     * @details A face is emitted between a collidable voxel and a non-collidable neighbour.
     * Neighbours outside the chunk are queried through `outside` (local coordinates with one
     * component equal to -1 or 32) so that faces on a chunk border are only generated where the
     * neighbouring chunk is actually open. Coplanar adjacent faces are merged into larger quads
     * regardless of material, so a flat floor becomes two triangles.
     * @param outside Returns true if the voxel at the given out-of-chunk local position is collidable.
     */
    ChunkMesh BuildChunkCollisionMesh(const Chunk& chunk, const CollisionFilter& filter,
                                      const std::function<bool(const glm::ivec3& local)>& outside);
}
