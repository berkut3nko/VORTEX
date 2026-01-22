module;

#include <cstdint>
#include <cstring> 

export module vortex.voxel:chunk;

import :hierarchy;

namespace vortex::voxel {

    constexpr int CHUNK_SIZE = 32;
    constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

    /**
     * @brief A single 32x32x32 voxel chunk.
     */
    export struct Chunk {
        uint32_t voxels[8192]; // Packed 4 voxels per uint32
        HierarchyMask optimization;

        Chunk() {
            Clear();
        }

        void Clear() {
            std::memset(voxels, 0, sizeof(voxels));
            std::memset(optimization.masks, 0, sizeof(optimization.masks));
        }

        void SetVoxel(int x, int y, int z, uint8_t id) {
            if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE) return;

            // 1. Set Voxel Data (Packed)
            uint32_t linearIdx = x + y * 32 + z * 1024;
            uint32_t arrayIdx = linearIdx >> 2; // / 4
            uint32_t shift = (linearIdx & 3) << 3; // % 4 * 8
            
            voxels[arrayIdx] &= ~(0xFF << shift);
            voxels[arrayIdx] |= (uint32_t(id) << shift);

            // 2. Update Hierarchy
            int bx = x >> 2;
            int by = y >> 2;
            int bz = z >> 2;
            
            if (id != 0) {
                optimization.SetBlockStatus(bx, by, bz, true);
            }
        }

        uint8_t GetVoxel(int x, int y, int z) const {
            if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE) return 0;
            uint32_t linearIdx = x + y * 32 + z * 1024;
            return (voxels[linearIdx >> 2] >> ((linearIdx & 3) << 3)) & 0xFF;
        }
    };
}