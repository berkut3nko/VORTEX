module;

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

export module vortex.voxel:connectivity;

import :volume;
import :world;
import :material;

export namespace vortex::voxel {

    /// @brief Connected component labelling of a volume (6-connectivity: faces only).
    struct ComponentLabels {
        glm::ivec3 size{0};
        std::vector<uint32_t> labels;          ///< Per voxel (same indexing as VoxelVolume): 0 = empty, 1..count.
        uint32_t count = 0;
        std::vector<uint32_t> voxelCounts;     ///< [count + 1], entry 0 unused.
        std::vector<glm::ivec3> boundsMin;     ///< [count + 1], inclusive.
        std::vector<glm::ivec3> boundsMax;     ///< [count + 1], exclusive.
    };

    /**
     * @brief Labels the connected components of the solid voxels of a volume.
     * @details Linear-time flood fill over the dense array (no hashing), a 32^3 object takes
     * microseconds. This is what decides whether a damaged object breaks apart.
     */
    ComponentLabels LabelComponents(const VoxelVolume& volume);

    /// @brief One piece of a volume that fell apart.
    struct VolumePiece {
        VoxelVolume volume;            ///< Cropped to the piece's bounds.
        glm::ivec3 offset{0};          ///< Position of the piece's minimum corner inside the source volume.
        uint32_t voxelCount = 0;
    };

    /// @brief A voxel that was too small a fragment to become an object (turn it into a particle).
    struct LooseVoxel {
        glm::ivec3 position{0};        ///< In the source volume.
        uint8_t material = 0;
    };

    struct SplitResult {
        std::vector<VolumePiece> pieces;     ///< Sorted by voxel count, largest first.
        std::vector<LooseVoxel> loose;       ///< Voxels of components smaller than minVoxels.
    };

    /**
     * @brief Splits a volume into its connected components.
     * @param minVoxels Components with fewer voxels are not returned as pieces but as `loose` voxels.
     */
    SplitResult SplitComponents(const VoxelVolume& volume, uint32_t minVoxels = 1);

    /// @brief A chunk of terrain that lost its connection to the rest of the world.
    struct FloatingIsland {
        glm::ivec3 origin{0};          ///< World voxel coordinate of the volume's minimum corner.
        VoxelVolume volume;
        uint32_t voxelCount = 0;
    };

    /**
     * @brief Finds terrain that is no longer attached to anything after an edit, and optionally
     * removes it from the world so it can be turned into falling dynamic objects.
     * @details Only the box [regionMin, regionMax) is examined (keep it modest, e.g. the edit's
     * bounds grown by a few metres). A component counts as attached if it touches the boundary
     * of the box - i.e. it continues outside the examined region - so the search can never
     * rip out a hillside just because the box was small. Voxels whose material has any of
     * `ignoreFlags` (liquids by default) neither support nor belong to islands.
     * @param maxIslandVoxels Larger floating components are left in place.
     *
     * The result depends only on the voxels of the box: components are found by a single-threaded
     * scan in VoxelVolume::Index order and returned in the order of their first voxel, so two
     * worlds with equal voxels in the box give equal islands in equal order.
     * @param removeFromWorld Clear the island voxels in the world (World::SetVoxel).
     * @param searched If not null: set to true when the box was examined, false when the search
     * was refused (box empty or larger than 192 voxels per axis, or a chunk of it not loaded) -
     * an empty result then proves nothing.
     */
    std::vector<FloatingIsland> ExtractFloatingIslands(World& world, const MaterialLibrary* materials,
                                                       const glm::ivec3& regionMin, const glm::ivec3& regionMax,
                                                       uint32_t maxIslandVoxels, bool removeFromWorld = true,
                                                       uint32_t ignoreFlags = MaterialFlag_Liquid, bool* searched = nullptr);
}
