module;

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

module vortex.voxel;

import vortex.core;

namespace vortex::voxel {

    ChunkStore::ChunkStore(uint32_t capacity) {
        m_Chunks.resize(capacity);
        m_DirtyFlag.assign(capacity, 0);
        m_FreeList.reserve(capacity);
        // Pop from the back: hand out low slot indices first so GPU buffers stay compact.
        for (uint32_t i = capacity; i-- > 0;) m_FreeList.push_back(i);
    }

    uint32_t ChunkStore::Alloc() { return Adopt(std::make_unique<Chunk>()); }

    uint32_t ChunkStore::Adopt(std::unique_ptr<Chunk> chunk) {
        if (m_FreeList.empty() || !chunk) return kInvalidSlot;
        const uint32_t slot = m_FreeList.back();
        m_FreeList.pop_back();
        m_Chunks[slot] = std::move(chunk);
        ++m_Used;
        if (slot + 1 > m_HighWater) m_HighWater = slot + 1;
        MarkDirty(slot);
        return slot;
    }

    void ChunkStore::Free(uint32_t slot) {
        if (!IsAllocated(slot)) {
            Log::Warn("ChunkStore::Free: slot {} is not allocated", slot);
            return;
        }
        m_Chunks[slot].reset();
        m_FreeList.push_back(slot);
        --m_Used;
    }

    void ChunkStore::MarkDirty(uint32_t slot) {
        if (slot >= m_DirtyFlag.size() || m_DirtyFlag[slot]) return;
        m_DirtyFlag[slot] = 1;
        m_DirtyList.push_back(slot);
    }

    std::vector<uint32_t> ChunkStore::TakeDirty() {
        std::vector<uint32_t> out = std::move(m_DirtyList);
        m_DirtyList.clear();
        for (uint32_t slot : out) m_DirtyFlag[slot] = 0;
        return out;
    }
}
