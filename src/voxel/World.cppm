module;

#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>

// GLM Extensions require this define
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>

export module vortex.voxel:world;

import :object;
import :hierarchy;
import :palette;
import vortex.core;

namespace vortex::voxel {

    /**
     * @brief Static utility class for generating geometric voxel shapes into chunks.
     */
    export class ShapeBuilder {
    public:
        /**
         * @brief Fills a rectangular region with a specific material.
         * @param chunk The target chunk to modify.
         * @param min The minimum corner (local coordinates).
         * @param max The maximum corner (local coordinates).
         * @param matIndex The material index to fill.
         */
        static void MakeBox(Chunk& chunk, const glm::ivec3& min, const glm::ivec3& max, uint8_t matIndex) {
            for (int z = min.z; z <= max.z; ++z) {
                for (int y = min.y; y <= max.y; ++y) {
                    for (int x = min.x; x <= max.x; ++x) {
                        chunk.SetVoxel(x, y, z, matIndex);
                    }
                }
            }
        }

        /**
         * @brief Creates a solid sphere.
         * @param chunk The target chunk.
         * @param center Center of the sphere (local coordinates).
         * @param radius Radius of the sphere.
         * @param matIndex Material index.
         */
        static void MakeSphere(Chunk& chunk, const glm::vec3& center, float radius, uint8_t matIndex) {
            int minX = static_cast<int>(std::floor(center.x - radius));
            int maxX = static_cast<int>(std::ceil(center.x + radius));
            int minY = static_cast<int>(std::floor(center.y - radius));
            int maxY = static_cast<int>(std::ceil(center.y + radius));
            int minZ = static_cast<int>(std::floor(center.z - radius));
            int maxZ = static_cast<int>(std::ceil(center.z + radius));

            float rSq = radius * radius;

            for (int z = minZ; z <= maxZ; ++z) {
                for (int y = minY; y <= maxY; ++y) {
                    for (int x = minX; x <= maxX; ++x) {
                        float distSq = glm::distance2(glm::vec3(x, y, z), center);
                        if (distSq <= rSq) {
                            chunk.SetVoxel(x, y, z, matIndex);
                        }
                    }
                }
            }
        }

        /**
         * @brief Creates a cylinder (vertical along Y axis).
         * @param chunk The target chunk.
         * @param baseCenter Center of the base.
         * @param radius Radius of the cylinder.
         * @param height Height of the cylinder.
         * @param matIndex Material index.
         */
        static void MakeCylinder(Chunk& chunk, const glm::vec3& baseCenter, float radius, float height, uint8_t matIndex) {
            int minX = static_cast<int>(baseCenter.x - radius);
            int maxX = static_cast<int>(baseCenter.x + radius);
            int minZ = static_cast<int>(baseCenter.z - radius);
            int maxZ = static_cast<int>(baseCenter.z + radius);
            int minY = static_cast<int>(baseCenter.y);
            int maxY = static_cast<int>(baseCenter.y + height);

            float rSq = radius * radius;

            for (int y = minY; y < maxY; ++y) {
                for (int z = minZ; z <= maxZ; ++z) {
                    for (int x = minX; x <= maxX; ++x) {
                        float distSq = glm::distance2(glm::vec2(x, z), glm::vec2(baseCenter.x, baseCenter.z));
                        if (distSq <= rSq) {
                            chunk.SetVoxel(x, y, z, matIndex);
                        }
                    }
                }
            }
        }

        /**
         * @brief Creates a square based pyramid.
         * @param chunk The target chunk.
         * @param baseCenter Center of the base.
         * @param baseSize Full width of the base.
         * @param height Height of the pyramid.
         * @param matIndex Material index.
         */
        static void MakePyramid(Chunk& chunk, const glm::vec3& baseCenter, float baseSize, float height, uint8_t matIndex) {
            int minY = static_cast<int>(baseCenter.y);
            int maxY = static_cast<int>(baseCenter.y + height);

            for (int y = minY; y < maxY; ++y) {
                float progress = (float)(y - minY) / height;
                float currentSize = baseSize * (1.0f - progress);
                float halfSize = currentSize * 0.5f;

                int minX = static_cast<int>(baseCenter.x - halfSize);
                int maxX = static_cast<int>(baseCenter.x + halfSize);
                int minZ = static_cast<int>(baseCenter.z - halfSize);
                int maxZ = static_cast<int>(baseCenter.z + halfSize);

                for (int z = minZ; z <= maxZ; ++z) {
                    for (int x = minX; x <= maxX; ++x) {
                        chunk.SetVoxel(x, y, z, matIndex);
                    }
                }
            }
        }
    };

    /**
     * @brief Manages the lifecycle of Voxel Objects in the scene.
     */
    export class VoxelWorld {
    public:
        VoxelWorld() = default;

        /**
         * @brief Creates a new voxel object and adds it to the world.
         * @param position World position.
         * @return Shared pointer to the created object.
         */
        std::shared_ptr<VoxelObject> CreateObject(const glm::vec3& position) {
            auto obj = std::make_shared<VoxelObject>();
            obj->position = position;
            obj->chunk = std::make_shared<Chunk>(); // Allocate new chunk memory
            m_Objects.push_back(obj);
            
            Log::Info("VoxelObject created. Total objects: " + std::to_string(m_Objects.size()));
            return obj;
        }

        /**
         * @brief Removes a specific object from the world.
         * @param object The object to remove.
         */
        void DestroyObject(std::shared_ptr<VoxelObject> object) {
            auto it = std::find(m_Objects.begin(), m_Objects.end(), object);
            if (it != m_Objects.end()) {
                m_Objects.erase(it);
                Log::Info("VoxelObject destroyed. Total objects: " + std::to_string(m_Objects.size()));
            }
        }

        /**
         * @brief Removes all objects from the world.
         */
        void Clear() {
            m_Objects.clear();
            Log::Info("VoxelWorld cleared.");
        }

        /**
         * @brief Access to the list of objects (for rendering/physics).
         */
        const std::vector<std::shared_ptr<VoxelObject>>& GetObjects() const {
            return m_Objects;
        }

    private:
        std::vector<std::shared_ptr<VoxelObject>> m_Objects;
    };
}