module;

#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <cmath>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>

module vortex.voxel;

import :destruction;
import :entity;
import :chunk;
import :object;
import :palette;
import vortex.log;

namespace vortex::voxel {

    // --- Helpers ---

    struct VoxelHasher {
        size_t operator()(const glm::ivec3& v) const {
            return std::hash<int>()(v.x) ^ (std::hash<int>()(v.y) << 1) ^ (std::hash<int>()(v.z) << 2);
        }
    };

    struct VoxelNode {
        glm::ivec3 pos;
        uint8_t materialID;
        int distanceToAnchor; 
        float currentLoad;    
        
        std::vector<glm::ivec3> parents; 
        std::vector<glm::ivec3> children;
    };

    // --- Implementation ---

    std::vector<Island> SHREDSystem::AnalyzeConnectivity(const std::shared_ptr<VoxelEntity>& entity) {
        std::vector<Island> islands;
        if (!entity || entity->parts.empty()) return islands;

        std::unordered_set<glm::ivec3, VoxelHasher> unvisited;
        std::unordered_map<glm::ivec3, uint8_t, VoxelHasher> voxelData;

        // Collect voxels from ALL parts into a single local space (entity space)
        for (const auto& part : entity->parts) {
            if (!part->chunk) continue;
            
            glm::ivec3 offset = glm::ivec3(part->position);
            for (int z = 0; z < 32; ++z) {
                for (int y = 0; y < 32; ++y) {
                    for (int x = 0; x < 32; ++x) {
                        uint8_t mat = part->chunk->GetVoxel(x, y, z);
                        if (mat != 0) {
                            glm::ivec3 localPos = offset + glm::ivec3(x, y, z);
                            unvisited.insert(localPos);
                            voxelData[localPos] = mat;
                        }
                    }
                }
            }
        }

        while (!unvisited.empty()) {
            Island island;
            std::queue<glm::ivec3> q;
            
            auto start = *unvisited.begin();
            q.push(start);
            unvisited.erase(unvisited.begin());

            while (!q.empty()) {
                glm::ivec3 current = q.front();
                q.pop();

                island.voxelPositions.push_back(current);
                island.materialIDs.push_back(voxelData[current]);

                const glm::ivec3 neighbors[6] = {
                    {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
                };

                for (const auto& dir : neighbors) {
                    glm::ivec3 neighborPos = current + dir;
                    auto it = unvisited.find(neighborPos);
                    if (it != unvisited.end()) {
                        q.push(neighborPos);
                        unvisited.erase(it);
                    }
                }

                if (CheckAnchoring(glm::vec3(entity->transform * glm::vec4(current, 1.0f)))) {
                     island.isAnchored = true;
                }
            }

            if (!island.voxelPositions.empty()) {
                islands.push_back(island);
            }
        }

        return islands;
    }

    std::vector<std::shared_ptr<VoxelEntity>> SHREDSystem::SplitEntity(const std::shared_ptr<VoxelEntity>& original, const std::vector<Island>& islands) {
        std::vector<std::shared_ptr<VoxelEntity>> newEntities;

        for (size_t i = 0; i < islands.size(); ++i) {
            const auto& island = islands[i];
            
            auto newFrag = std::make_shared<VoxelEntity>();
            newFrag->name = original->name + "_frag_" + std::to_string(i);
            newFrag->transform = original->transform;
            newFrag->isStatic = original->isStatic && island.isAnchored;

            auto part = std::make_shared<VoxelObject>();
            part->chunk = std::make_shared<Chunk>();
            
            glm::ivec3 minB(1000000), maxB(-1000000);
            for(const auto& p : island.voxelPositions) {
                minB = glm::min(minB, p);
                maxB = glm::max(maxB, p);
            }

            // Move the part position to the bottom-left of the island
            part->position = glm::vec3(minB);

            for (size_t v = 0; v < island.voxelPositions.size(); ++v) {
                glm::ivec3 relativePos = island.voxelPositions[v] - minB;
                // Bounds check logic: if island is > 32 voxels, this will clip.
                // TODO: Support multi-chunk entities in future.
                if (relativePos.x >= 0 && relativePos.x < 32 && 
                    relativePos.y >= 0 && relativePos.y < 32 && 
                    relativePos.z >= 0 && relativePos.z < 32) 
                {
                    part->chunk->SetVoxel(relativePos.x, relativePos.y, relativePos.z, island.materialIDs[v]);
                } else {
                    // Log warning if voxel is lost due to single-chunk limit
                    // Log::Warn("Voxel clipped during split! Island too big for single chunk.");
                }
            }

            newFrag->parts.push_back(part);
            newFrag->RecalculateStats();
            newEntities.push_back(newFrag);
        }

        return newEntities;
    }

    bool SHREDSystem::ValidateStructuralIntegrity(std::shared_ptr<VoxelEntity> entity, const MaterialPalette& palette) {
        if (!entity || entity->parts.empty() || entity->isStatic) return false;

        std::unordered_map<glm::ivec3, VoxelNode, VoxelHasher> nodes;
        std::vector<glm::ivec3> anchors;

        for (const auto& part : entity->parts) {
            if (!part->chunk) continue;
            glm::ivec3 offset = glm::ivec3(part->position);
            for (int z = 0; z < 32; ++z) {
                for (int y = 0; y < 32; ++y) {
                    for (int x = 0; x < 32; ++x) {
                        uint8_t matID = part->chunk->GetVoxel(x, y, z);
                        if (matID != 0) {
                            glm::ivec3 localPos = offset + glm::ivec3(x, y, z);
                            VoxelNode node;
                            node.pos = localPos;
                            node.materialID = matID;
                            node.distanceToAnchor = -1;
                            node.currentLoad = 0.0f;
                            
                            nodes[localPos] = node;

                            glm::vec3 worldPos = glm::vec3(entity->transform * glm::vec4(localPos, 1.0f));
                            if (CheckAnchoring(worldPos)) {
                                anchors.push_back(localPos);
                            }
                        }
                    }
                }
            }
        }

        if (anchors.empty()) return false; 

        std::queue<glm::ivec3> q;
        for (const auto& a : anchors) {
            nodes[a].distanceToAnchor = 0;
            q.push(a);
        }

        int maxDist = 0;
        while (!q.empty()) {
            glm::ivec3 currPos = q.front();
            q.pop();

            int currDist = nodes[currPos].distanceToAnchor;
            maxDist = std::max(maxDist, currDist);

            const glm::ivec3 neighbors[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
            for (const auto& dir : neighbors) {
                glm::ivec3 nPos = currPos + dir;
                auto it = nodes.find(nPos);
                if (it != nodes.end()) {
                    auto& neighborNode = it->second;
                    if (neighborNode.distanceToAnchor == -1) {
                        neighborNode.distanceToAnchor = currDist + 1;
                        neighborNode.parents.push_back(currPos);
                        nodes[currPos].children.push_back(nPos);
                        q.push(nPos);
                    }
                    else if (neighborNode.distanceToAnchor == currDist - 1) {
                         bool known = false;
                         for(auto& p : nodes[currPos].parents) if(p == nPos) known = true;
                         if(!known) nodes[currPos].parents.push_back(nPos);
                         neighborNode.children.push_back(currPos);
                    }
                }
            }
        }

        std::vector<std::vector<glm::ivec3>> layers(maxDist + 1);
        for (const auto& [pos, node] : nodes) {
            if (node.distanceToAnchor != -1) {
                layers[node.distanceToAnchor].push_back(pos);
            }
        }

        bool hasBrokenVoxels = false;

        // Iterate Leaves -> Roots
        for (int d = maxDist; d >= 0; --d) {
            for (const auto& pos : layers[d]) {
                auto& node = nodes[pos];
                
                const auto& mat = palette.Get(node.materialID);
                float selfWeight = mat.density; 
                float strength = mat.structuralHealth * 10.0f; 

                float totalLoadToDistribute = selfWeight + node.currentLoad;

                if (totalLoadToDistribute > strength) {
                    // BREAK
                    for (auto& part : entity->parts) {
                         glm::ivec3 offset = glm::ivec3(part->position);
                         glm::ivec3 localInPart = pos - offset;
                         if (localInPart.x >= 0 && localInPart.x < 32 &&
                             localInPart.y >= 0 && localInPart.y < 32 &&
                             localInPart.z >= 0 && localInPart.z < 32) {
                                 part->chunk->SetVoxel(localInPart.x, localInPart.y, localInPart.z, 0); 
                                 hasBrokenVoxels = true;
                                 break;
                             }
                    }
                    // IMPORTANT: We do not pass load to parents if this voxel breaks.
                    // This localizes the failure to the weakest points.
                    continue; 
                }

                if (!node.parents.empty()) {
                    float loadPerParent = totalLoadToDistribute / (float)node.parents.size();
                    for (const auto& pPos : node.parents) {
                        nodes[pPos].currentLoad += loadPerParent;
                    }
                }
            }
        }

        return hasBrokenVoxels;
    }

    bool SHREDSystem::CheckAnchoring(const glm::vec3& worldPos) {
        return worldPos.y <= 0.1f;
    }
}