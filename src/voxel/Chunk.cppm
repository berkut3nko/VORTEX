module;

#include <vector>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>

export module vortex.voxel:chunk;

namespace vortex::voxel {

    // --- Configuration ---
    // Set to true to enable Z-Order Curve (Morton) memory layout.
    // Must match the shader definition!
    constexpr bool USE_MORTON_LAYOUT = true;

    // --- Helpers ---

    // Expands 10-bit integer to 30-bit by inserting 2 zeros after each bit.
    // Matches GLSL implementation.
    constexpr uint32_t ExpandBits(uint32_t v) {
        v = (v * 0x00010001u) & 0xFF0000FFu;
        v = (v * 0x00000101u) & 0x0F00F00Fu;
        v = (v * 0x00000011u) & 0xC30C30C3u;
        v = (v * 0x00000005u) & 0x49249249u;
        return v;
    }

    // Calculates 3D Morton Code (Z-Order Curve index)
    constexpr uint32_t Morton3D(uint32_t x, uint32_t y, uint32_t z) {
        return ExpandBits(x) | (ExpandBits(y) << 1) | (ExpandBits(z) << 2);
    }

    /**
     * @brief Represents a 32x32x32 block of voxels.
     * @details Uses SoA (Structure of Arrays) layout aligned for GPU consumption.
     * Data layout for voxelIDs can be Linear or Morton (Z-Order) for cache optimization.
     */
    export struct Chunk {
        // 32^3 = 32768 voxels.
        // Packed 4 voxels per uint (8 bits each).
        uint32_t voxelIDs[8192];
        
        // Flags: 2 bits per voxel? Currently reserved/unused or used for other properties.
        uint32_t voxelFlags[2048]; 
        
        // Hierarchy: 1 bit per 4x4x4 block.
        // 32/4 = 8 blocks per axis. 8^3 = 512 bits = 16 uints.
        uint32_t hierarchy[16];

        Chunk() {
            std::memset(voxelIDs, 0, sizeof(voxelIDs));
            std::memset(voxelFlags, 0, sizeof(voxelFlags));
            std::memset(hierarchy, 0, sizeof(hierarchy));
        }

        /**
         * @brief Sets a voxel at local coordinates (0..31).
         * @details Automatically handles Linear vs Morton addressing.
         */
        void SetVoxel(int x, int y, int z, uint8_t id) {
            if (x < 0 || x >= 32 || y < 0 || y >= 32 || z < 0 || z >= 32) return;

            uint32_t index;
            if constexpr (USE_MORTON_LAYOUT) {
                index = Morton3D((uint32_t)x, (uint32_t)y, (uint32_t)z);
            } else {
                index = x + y * 32 + z * 1024;
            }

            // Pack into 32-bit array (4 voxels per uint)
            uint32_t arrayIdx = index >> 2;       // index / 4
            uint32_t shift = (index & 3) << 3;    // (index % 4) * 8
            
            // Clear old value
            voxelIDs[arrayIdx] &= ~(0xFF << shift);
            // Set new value
            voxelIDs[arrayIdx] |= (static_cast<uint32_t>(id) << shift);

            // Update Hierarchy (Linear mapping used for hierarchy to match simple shader logic)
            // Hierarchy tracks 4x4x4 blocks.
            if (id != 0) {
                int bx = x >> 2;
                int by = y >> 2;
                int bz = z >> 2;
                uint32_t hIndex = bx + by * 8 + bz * 64;
                
                hierarchy[hIndex >> 5] |= (1u << (hIndex & 31));
            } else {
                // Note: Clearing hierarchy bit requires checking all 64 voxels in the block.
                // Skipped for performance in simple Setter. Rebuild hierarchy if mass clearing.
            }
        }

        /**
         * @brief Gets a voxel ID at local coordinates.
         */
        uint8_t GetVoxel(int x, int y, int z) const {
            if (x < 0 || x >= 32 || y < 0 || y >= 32 || z < 0 || z >= 32) return 0;

            uint32_t index;
            if constexpr (USE_MORTON_LAYOUT) {
                index = Morton3D((uint32_t)x, (uint32_t)y, (uint32_t)z);
            } else {
                index = x + y * 32 + z * 1024;
            }

            uint32_t arrayIdx = index >> 2;
            uint32_t shift = (index & 3) << 3;

            return (voxelIDs[arrayIdx] >> shift) & 0xFF;
        }
    };
}