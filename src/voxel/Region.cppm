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
     * Thread safe. Save() only touches memory; Flush() writes dirty regions to disk
     * (atomically: temp file + rename).
     */
    class RegionStore {
    public:
        static constexpr int kRegionShift = 4;   ///< 16 chunks per region edge.

        /// @param directory Folder for the region files. Empty = keep everything in memory.
        explicit RegionStore(std::filesystem::path directory);
        ~RegionStore();

        std::optional<std::vector<uint8_t>> Load(const glm::ivec3& chunk);
        void Save(const glm::ivec3& chunk, std::vector<uint8_t> bytes);
        /// @brief Writes all modified regions. Returns the number of files written.
        size_t Flush();
        size_t StoredChunkCount() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
