module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

export module vortex.voxel:store;

import :chunk;

export namespace vortex::voxel {

    /**
     * @brief Pool of chunks addressed by slot index.
     * @details The slot index is the identity of a chunk everywhere: the world grid stores slots,
     * dynamic objects reference slots, the CPU tracer resolves slots, and the renderer mirrors
     * slot `i` at offset `i` of its GPU buffers. Because the indices are shared, keeping the
     * GPU in sync is just "re-upload the slots in the dirty list".
     *
     * Main thread only. Worker threads build chunks on their own and hand them over via Adopt().
     */
    class ChunkStore {
    public:
        static constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;

        explicit ChunkStore(uint32_t capacity);

        /// @brief Allocates an empty chunk. Returns kInvalidSlot when the pool is exhausted.
        uint32_t Alloc();
        /// @brief Takes ownership of a chunk built elsewhere (e.g. by a generation job).
        uint32_t Adopt(std::unique_ptr<Chunk> chunk);
        void Free(uint32_t slot);
        /// @brief Free() that hands the chunk's memory to the caller (to release it off the main thread).
        std::unique_ptr<Chunk> Release(uint32_t slot);

        bool IsAllocated(uint32_t slot) const { return slot < m_Chunks.size() && m_Chunks[slot] != nullptr; }
        Chunk& Get(uint32_t slot) { return *m_Chunks[slot]; }
        const Chunk& Get(uint32_t slot) const { return *m_Chunks[slot]; }
        const Chunk* TryGet(uint32_t slot) const { return slot < m_Chunks.size() ? m_Chunks[slot].get() : nullptr; }

        /// @brief Flags a slot for GPU re-upload. Call after modifying its chunk.
        void MarkDirty(uint32_t slot);
        /// @brief MarkDirty for an edit (a blast, a FillBox): TakeDirty returns edited slots first.
        void MarkEdited(uint32_t slot);
        /// @brief Returns and clears the slots modified since the last call, each slot once: the
        /// MarkEdited slots first (in marking order), then the other MarkDirty slots. When
        /// `editedFirst` is given, it receives how many leading entries are edited slots.
        std::vector<uint32_t> TakeDirty(size_t* editedFirst = nullptr);
        bool HasDirty() const { return !m_DirtyList.empty() || !m_EditedList.empty(); }

        uint32_t Capacity() const { return static_cast<uint32_t>(m_Chunks.size()); }
        uint32_t UsedCount() const { return m_Used; }
        /// @brief One past the highest slot ever allocated (upper bound for GPU buffer sizing).
        uint32_t HighWaterMark() const { return m_HighWater; }

    private:
        std::vector<std::unique_ptr<Chunk>> m_Chunks;
        std::vector<uint32_t> m_FreeList;      // LIFO, lowest slots first after construction
        // Per slot: bit 0 listed in m_DirtyList, bit 1 listed in m_EditedList; kTaken only inside TakeDirty.
        static constexpr uint8_t kListedDirty = 1, kListedEdited = 2, kTaken = 4;
        std::vector<uint8_t> m_DirtyFlag;
        std::vector<uint32_t> m_DirtyList;
        std::vector<uint32_t> m_EditedList;
        uint32_t m_Used = 0;
        uint32_t m_HighWater = 0;
    };
}
