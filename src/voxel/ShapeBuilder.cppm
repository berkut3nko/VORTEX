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
     */
    export class ShapeBuilder {
    public:
        // Заповнює чанк суцільним кубом (або частиною)
        static void CreateBox(Chunk& chunk, const glm::ivec3& min, const glm::ivec3& max, uint8_t materialId) {
            for (int z = min.z; z < max.z; ++z) {
                for (int y = min.y; y < max.y; ++y) {
                    for (int x = min.x; x < max.x; ++x) {
                        if (x >= 0 && x < 32 && y >= 0 && y < 32 && z >= 0 && z < 32) {
                            chunk.SetVoxel(x, y, z, materialId);
                        }
                    }
                }
            }
        }

        // Заповнює чанк сферою
        static void CreateSphere(Chunk& chunk, const glm::vec3& center, float radius, uint8_t materialId) {
            int minX = std::max(0, (int)std::floor(center.x - radius));
            int maxX = std::min(32, (int)std::ceil(center.x + radius));
            int minY = std::max(0, (int)std::floor(center.y - radius));
            int maxY = std::min(32, (int)std::ceil(center.y + radius));
            int minZ = std::max(0, (int)std::floor(center.z - radius));
            int maxZ = std::min(32, (int)std::ceil(center.z + radius));

            float r2 = radius * radius;

            for (int z = minZ; z < maxZ; ++z) {
                for (int y = minY; y < maxY; ++y) {
                    for (int x = minX; x < maxX; ++x) {
                        float dx = x - center.x;
                        float dy = y - center.y;
                        float dz = z - center.z;
                        if (dx*dx + dy*dy + dz*dz <= r2) {
                            chunk.SetVoxel(x, y, z, materialId);
                        }
                    }
                }
            }
        }
    };
}