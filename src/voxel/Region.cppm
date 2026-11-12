module;

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

export module vortex.voxel:region;

import :chunk;
import :generator;

export namespace vortex::voxel {

    /// @brief Run-length encodes a chunk (typically 20-100x smaller than the raw 32 KiB).
    std::vector<uint8_t> EncodeChunk(ChunkKind kind, uint8_t uniformId, const Chunk* chunk);
    /// @brief Inverse of EncodeChunk. Returns false on malformed data. Rebuilds the occupancy pyramid.
    bool DecodeChunk(std::span<const uint8_t> bytes, ChunkKind& kind, uint8_t& uniformId, Chunk& chunk);

    /**
     * @brief Persistent storage for chunks that differ from what the generator produces.
     * @details Only player-modified chunks are stored, so a world save is proportional to what
     * the players built or destroyed, not to how far they travelled. Chunks are grouped into
     * region files of 16^3 chunks (`r.x.y.z.vxr`).
     *
     * Every region file records the voxel size it was written at (exact float bits; files of
     * the older "VXR1" format have none and count as 0.1 m, the only size they were written at).
     * A file of another size is refused whole: its chunks are not loaded (the generator's terrain
     * shows instead), and the file is never overwritten - edits made there stay in memory for the
     * session and are not flushed (Flush warns, UnwrittenRegionCount counts them). The voxel ids
     * of one size mean nothing at another. A file this build cannot read (another magic, such as
     * a newer format, or a header cut short) is refused the same way; only an empty file is
     * replaced.
     *
     * Thread safe. Save() only touches memory; Flush() writes dirty regions to disk
     * (atomically: temp file + rename).
     */
    class RegionStore {
    public:
        static constexpr int kRegionShift = 4;   ///< 16 chunks per region edge.
        static constexpr float kLegacyVoxelSize = 0.1f;   ///< What a "VXR1" file (no voxel size) was written at.

        /// @param directory Folder for the region files. Empty = keep everything in memory.
        /// @param voxelSize Metres per voxel of the world: written into every file, and a file of
        /// another size is refused.
        RegionStore(std::filesystem::path directory, float voxelSize);
        ~RegionStore();

        /// @brief Region files read so far that were refused (another voxel size, or unreadable).
        size_t RefusedRegionCount() const;
        /// @brief Refused regions that hold edits of this session, which Flush does not write.
        size_t UnwrittenRegionCount() const;

        std::optional<std::vector<uint8_t>> Load(const glm::ivec3& chunk);
        /// @brief True if a chunk is stored (reads the region file on first use, like Load).
        bool Contains(const glm::ivec3& chunk);
        void Save(const glm::ivec3& chunk, std::vector<uint8_t> bytes);
        /// @brief Writes all modified regions. Returns the number of files written.
        size_t Flush();
        size_t StoredChunkCount() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
