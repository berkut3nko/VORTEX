module;

#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <algorithm>

export module vortex.voxel:shapebuilder;

import :chunk;
import :palette;

namespace vortex::voxel {

    /**
     * @brief Helper class to procedurally generate voxel shapes into Chunks.
     * @details Updates logical center and voxel count incrementally.
     */
    export class ShapeBuilder {
    public:
        /**
         * @brief Fills a chunk region with a solid box and updates object stats.
         * @param chunk Target chunk.
         * @param logicalCenter Reference to the object's logical center (will be updated).
         * @param voxelCount Reference to the object's voxel count (will be updated).
         * @param min Minimum corner coordinates (inclusive).
         * @param max Maximum corner coordinates (exclusive).
         * @param materialId Voxel material ID (0 to remove).
         */
        static void CreateBox(Chunk& chunk, glm::vec3& logicalCenter, uint32_t& voxelCount, const glm::ivec3& min, const glm::ivec3& max, uint8_t materialId) {
            glm::vec3 deltaPosSum(0.0f);
            int32_t deltaN = 0;

            for (int z = min.z; z < max.z; ++z) {
                for (int y = min.y; y < max.y; ++y) {
                    for (int x = min.x; x < max.x; ++x) {
                        if (x >= 0 && x < 32 && y >= 0 && y < 32 && z >= 0 && z < 32) {
                            uint8_t oldVoxel = chunk.GetVoxel(x, y, z);
                            bool wasSolid = (oldVoxel != 0);
                            bool willBeSolid = (materialId != 0);

                            if (oldVoxel == materialId) continue; // No change

                            chunk.SetVoxel(x, y, z, materialId);

                            glm::vec3 voxelCenter = glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f);

                            if (!wasSolid && willBeSolid) {
                                // Adding a voxel
                                deltaN++;
                                deltaPosSum += voxelCenter;
                            } else if (wasSolid && !willBeSolid) {
                                // Removing a voxel
                                deltaN--;
                                deltaPosSum -= voxelCenter;
                            }
                            // If just changing material (solid -> solid), center of mass doesn't change
                        }
                    }
                }
            }

            ApplyUpdates(logicalCenter, voxelCount, deltaPosSum, deltaN);
        }

        /**
         * @brief Fills a chunk region with a sphere and updates object stats.
         */
        static void CreateSphere(Chunk& chunk, glm::vec3& logicalCenter, uint32_t& voxelCount, const glm::vec3& center, float radius, uint8_t materialId) {
            int minX = std::max(0, (int)std::floor(center.x - radius));
            int maxX = std::min(32, (int)std::ceil(center.x + radius));
            int minY = std::max(0, (int)std::floor(center.y - radius));
            int maxY = std::min(32, (int)std::ceil(center.y + radius));
            int minZ = std::max(0, (int)std::floor(center.z - radius));
            int maxZ = std::min(32, (int)std::ceil(center.z + radius));

            float r2 = radius * radius;
            
            glm::vec3 deltaPosSum(0.0f);
            int32_t deltaN = 0;

            for (int z = minZ; z < maxZ; ++z) {
                for (int y = minY; y < maxY; ++y) {
                    for (int x = minX; x < maxX; ++x) {
                        float dx = x - center.x;
                        float dy = y - center.y;
                        float dz = z - center.z;
                        if (dx*dx + dy*dy + dz*dz <= r2) {
                            uint8_t oldVoxel = chunk.GetVoxel(x, y, z);
                            bool wasSolid = (oldVoxel != 0);
                            bool willBeSolid = (materialId != 0);

                            if (oldVoxel == materialId) continue;

                            chunk.SetVoxel(x, y, z, materialId);

                            glm::vec3 voxelCenter = glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f);

                            if (!wasSolid && willBeSolid) {
                                deltaN++;
                                deltaPosSum += voxelCenter;
                            } else if (wasSolid && !willBeSolid) {
                                deltaN--;
                                deltaPosSum -= voxelCenter;
                            }
                        }
                    }
                }
            }

            ApplyUpdates(logicalCenter, voxelCount, deltaPosSum, deltaN);
        }

    private:
        static void ApplyUpdates(glm::vec3& logicalCenter, uint32_t& voxelCount, glm::vec3 deltaPosSum, int32_t deltaN) {
            if (deltaN == 0 && glm::length(deltaPosSum) < 0.0001f) return;

            // Calculate current sum of positions
            glm::vec3 currentSum = logicalCenter * static_cast<float>(voxelCount);
            
            // Update sum and count
            currentSum += deltaPosSum;
            int32_t newCount = static_cast<int32_t>(voxelCount) + deltaN;

            if (newCount > 0) {
                voxelCount = static_cast<uint32_t>(newCount);
                logicalCenter = currentSum / static_cast<float>(newCount);
            } else {
                voxelCount = 0;
                logicalCenter = glm::vec3(16.0f); // Reset to geometric center if empty
            }
        }
    };
}