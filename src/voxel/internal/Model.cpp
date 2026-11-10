module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

module vortex.voxel;

import vortex.core;

namespace vortex::voxel {

    VoxelModel::VoxelModel(VoxelVolume volume, PaletteId palette, std::string name)
        : m_Name(std::move(name)), m_Volume(std::move(volume)), m_Palette(palette) {
        m_SolidCount = m_Volume.CountSolid();
    }

    VoxelModel::~VoxelModel() { ReleaseParts(); }

    void VoxelModel::ReleaseParts() {
        if (m_Store) {
            for (const ModelPart& part : m_Parts) {
                if (part.slot != ChunkStore::kInvalidSlot) m_Store->Free(part.slot);
            }
        }
        m_Parts.clear();
    }

    void VoxelModel::SetVolume(VoxelVolume volume) {
        m_Volume = std::move(volume);
        m_SolidCount = m_Volume.CountSolid();
        if (m_Store) {
            Bake(*m_Store);
        } else {
            ++m_Revision;
        }
    }

    bool VoxelModel::Bake(ChunkStore& store) {
        ReleaseParts();
        m_Store = &store;
        ++m_Revision;

        const glm::ivec3 size = m_Volume.Size();
        const glm::ivec3 chunks = (size + glm::ivec3(kChunkSize - 1)) / kChunkSize;
        const std::vector<uint8_t>& data = m_Volume.Data();
        bool complete = true;

        for (int cz = 0; cz < chunks.z; ++cz)
            for (int cy = 0; cy < chunks.y; ++cy)
                for (int cx = 0; cx < chunks.x; ++cx) {
                    const glm::ivec3 origin(cx * kChunkSize, cy * kChunkSize, cz * kChunkSize);
                    const glm::ivec3 end = glm::min(origin + glm::ivec3(kChunkSize), size);

                    // First pass: tight bounds (also tells us whether the chunk is needed at all).
                    glm::ivec3 mn(kChunkSize), mx(-1);
                    for (int z = origin.z; z < end.z; ++z)
                        for (int y = origin.y; y < end.y; ++y) {
                            const uint8_t* row = data.data() + m_Volume.Index(origin.x, y, z);
                            for (int x = origin.x; x < end.x; ++x) {
                                if (row[x - origin.x] == 0) continue;
                                const glm::ivec3 l(x - origin.x, y - origin.y, z - origin.z);
                                mn = glm::min(mn, l);
                                mx = glm::max(mx, l);
                            }
                        }
                    if (mx.x < 0) continue;

                    const uint32_t slot = store.Alloc();
                    if (slot == ChunkStore::kInvalidSlot) {
                        Log::Error("VoxelModel '{}': chunk store is full, model is incomplete", m_Name);
                        complete = false;
                        continue;
                    }
                    Chunk& chunk = store.Get(slot);
                    for (int z = origin.z; z < end.z; ++z)
                        for (int y = origin.y; y < end.y; ++y) {
                            const uint8_t* row = data.data() + m_Volume.Index(origin.x, y, z);
                            for (int x = origin.x; x < end.x; ++x) {
                                const uint8_t id = row[x - origin.x];
                                if (id != 0) chunk.ids[Chunk::VoxelIndex(x - origin.x, y - origin.y, z - origin.z)] = id;
                            }
                        }
                    chunk.RebuildOccupancy();
                    store.MarkDirty(slot);

                    ModelPart part;
                    part.slot = slot;
                    part.offset = origin;
                    part.boundsMin = mn;
                    part.boundsMax = mx + glm::ivec3(1);
                    m_Parts.push_back(part);
                }
        return complete;
    }
}
