module;

#include <glm/glm.hpp>

#include <bit>
#include <cstdint>
#include <cstring>

export module vortex.voxel:chunk;

export namespace vortex::voxel {

    constexpr int kChunkSize = 32;                ///< Voxels per chunk edge.
    constexpr int kChunkShift = 5;
    constexpr int kChunkMask = kChunkSize - 1;
    constexpr int kChunkVolume = kChunkSize * kChunkSize * kChunkSize;
    constexpr int kBlocksPerChunk = 512;          ///< 8^3 blocks of 4^3 voxels.

    /**
     * @brief Three-level occupancy pyramid of a chunk (4168 bytes).
     * @details This is what a ray actually walks: it never touches material ids until it has
     * found a solid voxel, and it skips empty 8^3 and 4^3 regions in a single step.
     *
     *   cellMask   64 bits   one bit per 8^3 cell   (index = cx + cy*4 + cz*16)
     *   blockMask  512 bits  one bit per 4^3 block  (index = bx + by*8 + bz*64)
     *   voxelMask  512 * 64  one bit per voxel, grouped by block (bit = lx + ly*4 + lz*16)
     *
     * The layout is uploaded to the GPU verbatim as an array of 1042 uint32.
     */
    struct ChunkOccupancy {
        uint64_t cellMask = 0;
        uint64_t blockMask[8] = {};
        uint64_t voxelMask[kBlocksPerChunk] = {};
    };
    static_assert(sizeof(ChunkOccupancy) == 4168, "GPU layout: 1042 uint32 per chunk");
    constexpr uint32_t kOccupancyWords = sizeof(ChunkOccupancy) / 4;

    /**
     * @brief A 32^3 block of voxels: occupancy pyramid + one material id per voxel.
     * @details Ids are stored block-ordered (all 64 voxels of a 4^3 block are contiguous), which
     * keeps the id fetched after a hit in the same cache line as its neighbours and makes the
     * voxel index a few shifts instead of a Morton code.
     *
     * Chunks are the unit of storage for both terrain and dynamic objects, on the CPU and on
     * the GPU (36936 bytes each).
     */
    struct Chunk {
        ChunkOccupancy occ;
        uint8_t ids[kChunkVolume] = {};

        static constexpr uint32_t BlockIndex(int x, int y, int z) {
            return static_cast<uint32_t>((x >> 2) + ((y >> 2) << 3) + ((z >> 2) << 6));
        }
        static constexpr uint32_t LocalIndex(int x, int y, int z) {
            return static_cast<uint32_t>((x & 3) + ((y & 3) << 2) + ((z & 3) << 4));
        }
        static constexpr uint32_t VoxelIndex(int x, int y, int z) { return BlockIndex(x, y, z) * 64u + LocalIndex(x, y, z); }
        static constexpr bool InBounds(int x, int y, int z) {
            return (static_cast<unsigned>(x) | static_cast<unsigned>(y) | static_cast<unsigned>(z)) < static_cast<unsigned>(kChunkSize);
        }

        /// @brief Material id at local coordinates (must be in 0..31).
        uint8_t Get(int x, int y, int z) const { return ids[VoxelIndex(x, y, z)]; }
        bool IsSolid(int x, int y, int z) const {
            return (occ.voxelMask[BlockIndex(x, y, z)] >> LocalIndex(x, y, z)) & 1u;
        }
        /// @brief Bounds-checked read: 0 outside the chunk.
        uint8_t GetSafe(int x, int y, int z) const { return InBounds(x, y, z) ? Get(x, y, z) : uint8_t{0}; }

        /**
         * @brief Sets one voxel and keeps the whole occupancy pyramid consistent
         * (including clearing block / cell bits when the last voxel of a region is removed).
         * @return true if the voxel changed.
         */
        bool Set(int x, int y, int z, uint8_t id) {
            const uint32_t b = BlockIndex(x, y, z);
            const uint32_t l = LocalIndex(x, y, z);
            uint8_t& ref = ids[b * 64u + l];
            if (ref == id) return false;
            const bool was = ref != 0;
            const bool now = id != 0;
            ref = id;
            if (was == now) return true;

            if (now) {
                occ.voxelMask[b] |= uint64_t{1} << l;
                occ.blockMask[b >> 6] |= uint64_t{1} << (b & 63u);
                occ.cellMask |= uint64_t{1} << CellOfBlock(b);
            } else {
                occ.voxelMask[b] &= ~(uint64_t{1} << l);
                if (occ.voxelMask[b] == 0) {
                    occ.blockMask[b >> 6] &= ~(uint64_t{1} << (b & 63u));
                    const uint32_t c = CellOfBlock(b);
                    if (!CellHasBlocks(c)) occ.cellMask &= ~(uint64_t{1} << c);
                }
            }
            return true;
        }

        void Clear() {
            occ = ChunkOccupancy{};
            std::memset(ids, 0, sizeof(ids));
        }

        void Fill(uint8_t id) {
            std::memset(ids, id, sizeof(ids));
            RebuildOccupancy();
        }

        /// @brief Recomputes the pyramid from `ids`. Call after bulk writes through `ids`.
        void RebuildOccupancy() {
            occ.cellMask = 0;
            std::memset(occ.blockMask, 0, sizeof(occ.blockMask));
            for (uint32_t b = 0; b < kBlocksPerChunk; ++b) {
                const uint8_t* p = ids + static_cast<size_t>(b) * 64u;
                uint64_t mask = 0;
                for (uint32_t l = 0; l < 64u; ++l) mask |= static_cast<uint64_t>(p[l] != 0) << l;
                occ.voxelMask[b] = mask;
                if (mask != 0) {
                    occ.blockMask[b >> 6] |= uint64_t{1} << (b & 63u);
                    occ.cellMask |= uint64_t{1} << CellOfBlock(b);
                }
            }
        }

        bool IsEmpty() const { return occ.cellMask == 0; }

        uint32_t CountSolid() const {
            uint32_t n = 0;
            for (uint64_t m : occ.voxelMask) n += static_cast<uint32_t>(std::popcount(m));
            return n;
        }

        /// @brief The single id shared by every voxel, or -1 if the chunk is not uniform.
        int UniformId() const {
            const uint8_t first = ids[0];
            for (int i = 1; i < kChunkVolume; ++i) {
                if (ids[i] != first) return -1;
            }
            return first;
        }

        /// @brief Occupancy pyramid viewed as GPU words (see ChunkOccupancy).
        uint32_t OccupancyWord(uint32_t word) const {
            uint32_t w;
            std::memcpy(&w, reinterpret_cast<const unsigned char*>(&occ) + static_cast<size_t>(word) * 4u, 4);
            return w;
        }

    private:
        static constexpr uint32_t CellOfBlock(uint32_t b) {
            const uint32_t bx = b & 7u, by = (b >> 3) & 7u, bz = b >> 6;
            return (bx >> 1) + ((by >> 1) << 2) + ((bz >> 1) << 4);
        }
        bool CellHasBlocks(uint32_t c) const {
            const uint32_t bx = (c & 3u) << 1, by = ((c >> 2) & 3u) << 1, bz = (c >> 4) << 1;
            for (uint32_t dz = 0; dz < 2; ++dz)
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        const uint32_t b = (bx + dx) + ((by + dy) << 3) + ((bz + dz) << 6);
                        if ((occ.blockMask[b >> 6] >> (b & 63u)) & 1u) return true;
                    }
            return false;
        }
    };
    static_assert(sizeof(Chunk) == 4168 + 32768);

    /// @brief Chunk containing a world voxel (floor division; correct for negative coordinates).
    inline glm::ivec3 VoxelToChunk(const glm::ivec3& v) { return v >> kChunkShift; }
    /// @brief Position of a world voxel inside its chunk (0..31).
    inline glm::ivec3 VoxelToLocal(const glm::ivec3& v) { return v & kChunkMask; }
    /// @brief World voxel coordinate of a chunk's minimum corner.
    inline glm::ivec3 ChunkToVoxel(const glm::ivec3& c) { return c * kChunkSize; }
}
