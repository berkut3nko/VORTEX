module;

#include <vector>
#include <cstdint>
#include <bit>
#include <cstring> // for memset

export module vortex.voxel:hierarchy;

import vortex.core;

namespace vortex::voxel {

    constexpr int CHUNK_SIZE = 32;
    constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

    /**
     * @brief Acceleration structure for Ray Tracing (Grid Hierarchy).
     * @details Represents a 32x32x32 chunk divided into 8x8x8 blocks (4x4x4 voxels each).
     * Each bit corresponds to one 4x4x4 block. 512 bits total (64 bytes).
     */
    export struct HierarchyMask {
        // 512 bits / 64 bits per uint64 = 8 integers.
        uint64_t masks[8]; 

        /**
         * @brief Checks if a 4x4x4 block is completely empty.
         * @param bx Block X (0-7)
         * @param by Block Y (0-7)
         * @param bz Block Z (0-7)
         * @return true if the block contains no voxels.
         */
        bool IsBlockEmpty(int bx, int by, int bz) const {
            // Map 3D block coord to linear bit index
            int blockIndex = bx + by * 8 + bz * 64;
            int uintIndex = blockIndex / 64;
            int bitIndex = blockIndex % 64;
            return !(masks[uintIndex] & (1ULL << bitIndex));
        }

        /**
         * @brief Updates the bitmask status for a block.
         * @param bx Block X (0-7)
         * @param by Block Y (0-7)
         * @param bz Block Z (0-7)
         * @param notEmpty If true, marks the block as occupied. If false, marks as empty.
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

    /**
     * @brief A 32^3 container of voxels with acceleration structure.
     */
    export struct Chunk {
        uint8_t voxels[CHUNK_VOLUME]; ///< Raw material indices (0 = air).
        HierarchyMask optimization;   ///< Optimization bitmask for empty space skipping.

        Chunk() {
            // Initialize with air
            std::memset(voxels, 0, CHUNK_VOLUME);
            std::memset(optimization.masks, 0, sizeof(optimization.masks));
        }

        /**
         * @brief Sets a voxel at local coordinates and updates the optimization mask.
         * @param localX Local X (0-31)
         * @param localY Local Y (0-31)
         * @param localZ Local Z (0-31)
         * @param materialIndex Material ID from the palette.
         */
        void SetVoxel(int localX, int localY, int localZ, uint8_t materialIndex) {
            if (localX < 0 || localX >= CHUNK_SIZE ||
                localY < 0 || localY >= CHUNK_SIZE ||
                localZ < 0 || localZ >= CHUNK_SIZE) {
                // Log::Warn("Chunk::SetVoxel: Coordinates out of bounds"); // Optional: prevent spam
                return;
            }

            int index = localX + localY * CHUNK_SIZE + localZ * CHUNK_SIZE * CHUNK_SIZE;
            
            // Only update mask if state changes from empty to non-empty or vice-versa
            // Note: Optimizing mask updates is crucial. Doing it per-voxel is slow for bulk updates.
            // For single voxel edits (gameplay), this is fine.
            voxels[index] = materialIndex;
            
            UpdateMaskForBlock(localX / 4, localY / 4, localZ / 4);
        }

        /**
         * @brief Recalculates the mask bit for a specific 4x4x4 block.
         */
        void UpdateMaskForBlock(int bx, int by, int bz) {
            bool notEmpty = false;
            
            // Scan the 4x4x4 region
            for(int z = bz*4; z < (bz+1)*4; ++z) {
                for(int y = by*4; y < (by+1)*4; ++y) {
                    for(int x = bx*4; x < (bx+1)*4; ++x) {
                        int idx = x + y * CHUNK_SIZE + z * CHUNK_SIZE * CHUNK_SIZE;
                        if (voxels[idx] != 0) {
                            notEmpty = true;
                            goto found; // Break out of nested loops
                        }
                    }
                }
            }
            found:
            optimization.SetBlockStatus(bx, by, bz, notEmpty);
        }
    };
}