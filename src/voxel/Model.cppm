module;

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

export module vortex.voxel:model;

import :chunk;
import :store;
import :volume;
import :material;

export namespace vortex::voxel {

    /// @brief One baked chunk of a model.
    struct ModelPart {
        uint32_t slot = ChunkStore::kInvalidSlot;   ///< Chunk slot in the store.
        glm::ivec3 offset{0};                       ///< Position of the chunk inside the model, in voxels.
        glm::ivec3 boundsMin{0};                    ///< Tight bounds of the solid voxels inside the chunk, [min, max).
        glm::ivec3 boundsMax{0};
    };

    /**
     * @brief A voxel object as a shareable resource: the dense volume plus its baked chunks.
     * @details Many entities can reference the same model (a hundred identical barrels cost one
     * set of chunks). Destruction makes a private copy first (copy-on-write, see scene).
     *
     * `Revision()` changes whenever the baked data changes; systems that cache derived data
     * (physics shapes, mass) compare revisions instead of being called back.
     */
    class VoxelModel {
    public:
        VoxelModel(VoxelVolume volume, PaletteId palette, std::string name = {});
        ~VoxelModel();
        VoxelModel(const VoxelModel&) = delete;
        VoxelModel& operator=(const VoxelModel&) = delete;

        const std::string& Name() const { return m_Name; }
        const VoxelVolume& Volume() const { return m_Volume; }
        PaletteId Palette() const { return m_Palette; }
        const glm::ivec3& Size() const { return m_Volume.Size(); }
        uint32_t SolidCount() const { return m_SolidCount; }
        uint64_t Revision() const { return m_Revision; }

        /// @brief Replaces the voxel data and re-bakes if the model is already baked.
        void SetVolume(VoxelVolume volume);

        /**
         * @brief Splits the volume into chunks in `store` so it can be rendered and ray traced.
         * @return false if the store ran out of slots (the model is then partially baked).
         */
        bool Bake(ChunkStore& store);
        bool IsBaked() const { return m_Store != nullptr; }
        std::span<const ModelPart> Parts() const { return m_Parts; }

    private:
        void ReleaseParts();

        std::string m_Name;
        VoxelVolume m_Volume;
        PaletteId m_Palette;
        ChunkStore* m_Store = nullptr;
        std::vector<ModelPart> m_Parts;
        uint32_t m_SolidCount = 0;
        uint64_t m_Revision = 0;
    };
}
