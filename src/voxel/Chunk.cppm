module;

#include <cstdint>
#include <cstring> 

export module vortex.voxel:chunk;

// Hierarchy logic is now inlined for SoA layout
// import :hierarchy; 

namespace vortex::voxel {

    constexpr int CHUNK_SIZE = 32;

    /**
     * @brief A single 32x32x32 voxel chunk (SoA Layout).
     * @details
     * - voxelIDs: 8 bits per voxel (Material Index). 32KB.
     * - voxelFlags: 2 bits per voxel (State flags). 8KB.
     * - hierarchy: 1 bit per 4x4x4 block (Occupancy). 64 Bytes.
     * Total Size: ~40KB per chunk.
     */
    export struct Chunk {
        // Material Indices (8 bits * 32768 voxels)
        // Packed 4 per uint32: [Mat3][Mat2][Mat1][Mat0]
        uint32_t voxelIDs[8192]; 

        // Voxel Flags (2 bits * 32768 voxels)
        // Packed 16 per uint32
        uint32_t voxelFlags[2048]; 

        // Acceleration Structure (512 bits)
        // 1 bit per 4x4x4 block
        uint32_t hierarchy[16]; 

        Chunk() {
            Clear();
        }

        void Clear() {
            std::memset(voxelIDs, 0, sizeof(voxelIDs));
            std::memset(voxelFlags, 0, sizeof(voxelFlags));
            std::memset(hierarchy, 0, sizeof(hierarchy));
        }

        /**
         * @brief Sets a voxel's material and flags.
         * @param flags Optional 2-bit flag (0-3).
         */
        void SetVoxel(int x, int y, int z, uint8_t id, uint8_t flags = 0) {
            if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE) return;

            uint32_t linearIdx = x + y * 32 + z * 1024;

            // 1. Set Material ID (8 bits)
            uint32_t idArrIdx = linearIdx >> 2; // / 4
            uint32_t idShift = (linearIdx & 3) << 3; // % 4 * 8
            
            voxelIDs[idArrIdx] &= ~(0xFFu << idShift);
            voxelIDs[idArrIdx] |= (uint32_t(id) << idShift);

            // 2. Set Flags (2 bits)
            uint32_t flagArrIdx = linearIdx >> 4; // / 16
            uint32_t flagShift = (linearIdx & 15) << 1; // % 16 * 2
            
            voxelFlags[flagArrIdx] &= ~(0x3u << flagShift);
            voxelFlags[flagArrIdx] |= (uint32_t(flags & 0x3) << flagShift);

            // 3. Update Hierarchy (Occupancy Bit)
            // Block coordinate (0..7)
            int bx = x >> 2;
            int by = y >> 2;
            int bz = z >> 2;
            
            // Linear hierarchy index (0..511)
            int hIdx = bx + by * 8 + bz * 64;
            int hArrIdx = hIdx >> 5; // / 32
            int hBitIdx = hIdx & 31; // % 32
            
            if (id != 0) {
                hierarchy[hArrIdx] |= (1u << hBitIdx);
            }
        }

        uint8_t GetVoxel(int x, int y, int z) const {
            if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE) return 0;
            uint32_t linearIdx = x + y * 32 + z * 1024;
            return (voxelIDs[linearIdx >> 2] >> ((linearIdx & 3) << 3)) & 0xFF;
        }

        uint8_t GetFlags(int x, int y, int z) const {
            if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE) return 0;
            uint32_t linearIdx = x + y * 32 + z * 1024;
            return (voxelFlags[linearIdx >> 4] >> ((linearIdx & 15) << 1)) & 0x3;
        }
    };
}