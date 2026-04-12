module;

#include <vector>
#include <memory>
#include <algorithm>
#include <glm/glm.hpp>

export module vortex.voxel:world;

import :object;
import :material;
import :palette;
import :chunk;

namespace vortex::voxel {

    /**
     * @brief Represents an instance of a voxel object in the world for rendering.
     */
    export struct ObjectInstance {
        glm::mat4 transform;
        uint32_t chunkIndex;
        uint32_t materialOffset;
    };

    /**
     * @brief Simple linear pool for managing chunk storage.
     */
    export struct ChunkPool {
        std::vector<Chunk> chunks;

        uint32_t AddChunk(const Chunk& chunk) {
            chunks.push_back(chunk);
            return (uint32_t)(chunks.size() - 1);
        }

        void Clear() {
            chunks.clear();
        }
    };

    /**
     * @brief High-level container for the Voxel World state.
     * @details Manages object instances, the chunk pool, and the material palette.
     */
    export class VoxelWorld {
    public:
        VoxelWorld() = default;

        /**
         * @brief Adds a voxel object to the world and registers its chunk.
         * @param obj The voxel object to add.
         */
        void AddObject(const VoxelObject& obj) {
            if (obj.chunk) {
                uint32_t index = m_Pool.AddChunk(*obj.chunk);
                
                ObjectInstance instance;
                instance.transform = obj.GetTransformMatrix();
                instance.chunkIndex = index;
                instance.materialOffset = 0;
                
                m_Objects.push_back(instance);
            }
        }

        const std::vector<ObjectInstance>& GetObjects() const { return m_Objects; }
        const std::vector<Chunk>& GetChunks() const { return m_Pool.chunks; }
        
        /**
         * @brief Returns the raw material data from the palette.
         */
        const std::vector<PhysicalMaterial>& GetMaterials() const { return m_Palette.GetData(); }

        MaterialPalette& GetPalette() { return m_Palette; }

    private:
        std::vector<ObjectInstance> m_Objects;
        ChunkPool m_Pool;
        MaterialPalette m_Palette;
    };
}