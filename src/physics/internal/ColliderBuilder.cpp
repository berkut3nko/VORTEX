module;

#include <vector>
#include <vector>
#include <glm/glm.hpp>
#include <cstring>

module vortex.physics;

import :collider_builder;
import vortex.voxel;

namespace vortex::physics {

    // Helper to linearize 3D coordinates locally within the builder
    inline int GetIndex(int x, int y, int z) {
        return x + y * 32 + z * 1024;
    }

    std::vector<ColliderBox> VoxelColliderBuilder::Build(const vortex::voxel::Chunk& chunk) {
        std::vector<ColliderBox> boxes;
        
        // Mask to keep track of processed voxels. 
        // 32^3 = 32768 bools. std::vector<bool> is space efficient.
        std::vector<bool> visited(32 * 32 * 32, false);

        // Iterate over all voxels
        for (int z = 0; z < 32; ++z) {
            for (int y = 0; y < 32; ++y) {
                for (int x = 0; x < 32; ++x) {
                    
                    int idx = GetIndex(x, y, z);
                    if (visited[idx]) continue;

                    uint8_t matID = chunk.GetVoxel(x, y, z);
                    if (matID == 0) { // Empty
                        visited[idx] = true;
                        continue;
                    }

                    // Start Greedy Merging
                    // 1. Expand in X axis
                    int width = 1;
                    while (x + width < 32) {
                        int nextIdx = GetIndex(x + width, y, z);
                        if (visited[nextIdx] || chunk.GetVoxel(x + width, y, z) != matID) break;
                        width++;
                    }

                    // 2. Expand in Y axis (check the whole row of width)
                    int height = 1;
                    bool canExpandY = true;
                    while (y + height < 32) {
                        for (int w = 0; w < width; ++w) {
                            int nextIdx = GetIndex(x + w, y + height, z);
                            if (visited[nextIdx] || chunk.GetVoxel(x + w, y + height, z) != matID) {
                                canExpandY = false;
                                break;
                            }
                        }
                        if (!canExpandY) break;
                        height++;
                    }

                    // 3. Expand in Z axis (check the whole plane of width * height)
                    int depth = 1;
                    bool canExpandZ = true;
                    while (z + depth < 32) {
                        for (int h = 0; h < height; ++h) {
                            for (int w = 0; w < width; ++w) {
                                int nextIdx = GetIndex(x + w, y + h, z + depth);
                                if (visited[nextIdx] || chunk.GetVoxel(x + w, y + h, z + depth) != matID) {
                                    canExpandZ = false;
                                    break;
                                }
                            }
                            if (!canExpandZ) break;
                        }
                        if (!canExpandZ) break;
                        depth++;
                    }

                    // Mark all covered voxels as visited
                    for (int d = 0; d < depth; ++d) {
                        for (int h = 0; h < height; ++h) {
                            for (int w = 0; w < width; ++w) {
                                visited[GetIndex(x + w, y + h, z + d)] = true;
                            }
                        }
                    }

                    // Add the optimized box
                    ColliderBox box;
                    box.min = glm::vec3(x, y, z);
                    box.size = glm::vec3(width, height, depth);
                    box.materialID = matID;
                    boxes.push_back(box);
                }
            }
        }

        return boxes;
    }
}