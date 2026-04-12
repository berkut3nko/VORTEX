module;

#include <cstdint>
#include <bit>
#include <cstring> 

export module vortex.voxel:hierarchy;

namespace vortex::voxel {

    /**
     * @brief Acceleration structure for Voxel Ray Tracing (Grid Hierarchy).
     * @details Represents a 32x32x32 chunk divided into 8x8x8 blocks (4x4x4 voxels each).
     * Each bit corresponds to one 4x4x4 block. If a bit is 0, the entire 4x4x4 block is empty, allow the raymarcher to skip it.
     * Total size: 512 bits (64 bytes).
     */
    export struct HierarchyMask {
        // 512 bits / 64 bits per uint64 = 8 integers.
        uint64_t masks[8]; 

        /**
         * @brief Checks if a specific 4x4x4 block is marked as empty.
         * @param bx Block X (0-7).
         * @param by Block Y (0-7).
         * @param bz Block Z (0-7).
         * @return True if the block is empty.
         */
        bool IsBlockEmpty(int bx, int by, int bz) const {
            int blockIndex = bx + by * 8 + bz * 64;
            int uintIndex = blockIndex / 64;
            int bitIndex = blockIndex % 64;
            return !(masks[uintIndex] & (1ULL << bitIndex));
        }

        /**
         * @brief Updates the occupancy status of a block.
         * @param bx Block X (0-7).
         * @param by Block Y (0-7).
         * @param bz Block Z (0-7).
         * @param notEmpty If true, marks the block as occupied.
         */
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