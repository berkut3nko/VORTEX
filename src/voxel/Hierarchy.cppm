module;

#include <cstdint>
#include <bit>
#include <cstring> 

export module vortex.voxel:hierarchy;

namespace vortex::voxel {

    /**
     * @brief Acceleration structure for Ray Tracing (Grid Hierarchy).
     * @details Represents a 32x32x32 chunk divided into 8x8x8 blocks (4x4x4 voxels each).
     * Each bit corresponds to one 4x4x4 block. 512 bits total (64 bytes).
     */
    export struct HierarchyMask {
        // 512 bits / 64 bits per uint64 = 8 integers.
        uint64_t masks[8]; 

        bool IsBlockEmpty(int bx, int by, int bz) const {
            int blockIndex = bx + by * 8 + bz * 64;
            int uintIndex = blockIndex / 64;
            int bitIndex = blockIndex % 64;
            return !(masks[uintIndex] & (1ULL << bitIndex));
        }

        void SetBlockStatus(int bx, int by, int bz, bool notEmpty) {
            int blockIndex = bx + by * 8 + bz * 64;
            int uintIndex = blockIndex / 64;
            int bitIndex = blockIndex % 64;
            
            if (notEmpty) {
                masks[uintIndex] |= (1ULL << bitIndex);
            } else {
                masks[uintIndex] &= ~(1ULL << bitIndex);
            }
        }
    };
}