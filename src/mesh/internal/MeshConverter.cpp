module;

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

module vortex.voxel;

import :mesh_converter;
import :chunk;
import :material;
import vortex.log;
// DO NOT import vortex.graphics;

namespace vortex::voxel {

    static bool CheckAxis(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& halfSize, const glm::vec3& axis) {
        float p0 = glm::dot(v0, axis);
        float p1 = glm::dot(v1, axis);
        float p2 = glm::dot(v2, axis);
        float r = halfSize.x * std::abs(glm::dot(glm::vec3(1,0,0), axis)) +
                  halfSize.y * std::abs(glm::dot(glm::vec3(0,1,0), axis)) +
                  halfSize.z * std::abs(glm::dot(glm::vec3(0,0,1), axis));
        return std::max(-std::max(p0, std::max(p1, p2)), std::min(p0, std::min(p1, p2))) > r;
    }

    static bool TriBoxOverlap(glm::vec3 boxCenter, glm::vec3 boxHalfSize, glm::vec3 v0, glm::vec3 v1, glm::vec3 v2) {
        v0 -= boxCenter; v1 -= boxCenter; v2 -= boxCenter;
        glm::vec3 f0 = v1 - v0, f1 = v2 - v1, f2 = v0 - v2;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(1,0,0), f0))) return false;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(1,0,0), f1))) return false;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(1,0,0), f2))) return false;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(0,1,0), f0))) return false;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(0,1,0), f1))) return false;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(0,1,0), f2))) return false;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(0,0,1), f0))) return false;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(0,0,1), f1))) return false;
        if (CheckAxis(v0, v1, v2, boxHalfSize, glm::cross(glm::vec3(0,0,1), f2))) return false;
        return true;
    }

    struct Triangle { 
        glm::vec3 v0, v1, v2;
        int materialIdx; 
    };

    MeshImportResult MeshConverter::Import(const MeshImportSettings& settings) {
        MeshImportResult result;
        
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;
        std::string err, warn;

        bool ret = false;
        if (settings.filePath.ends_with(".glb")) ret = loader.LoadBinaryFromFile(&model, &err, &warn, settings.filePath);
        else ret = loader.LoadASCIIFromFile(&model, &err, &warn, settings.filePath);

        if (!warn.empty()) Log::Warn("TinyGLTF Warning: " + warn);
        if (!err.empty()) Log::Error("TinyGLTF Error: " + err);
        if (!ret) return result;

        if (model.materials.empty()) {
            PhysicalMaterial defMat{};
            defMat.color = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
            defMat.density = 1.0f; defMat.friction = 0.5f; defMat.hardness = 1.0f;
            result.materials.push_back(defMat);
        } else {
            for (const auto& mat : model.materials) {
                PhysicalMaterial physMat{};
                
                std::vector<double> baseColor = mat.pbrMetallicRoughness.baseColorFactor;
                if (baseColor.size() == 4) {
                    physMat.color = glm::vec4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
                } else {
                    physMat.color = glm::vec4(1.0f);
                }

                int texIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
                if (texIndex >= 0 && texIndex < (int)model.textures.size()) {
                    int imgIndex = model.textures[texIndex].source;
                    if (imgIndex >= 0 && imgIndex < (int)model.images.size()) {
                        const auto& img = model.images[imgIndex];
                        if (!img.image.empty() && img.width > 0 && img.height > 0 && img.component >= 3) {
                            int cx = img.width / 2;
                            int cy = img.height / 2;
                            int pixelIdx = (cy * img.width + cx) * img.component;
                            
                            if (pixelIdx + 2 < (int)img.image.size()) {
                                float r = img.image[pixelIdx] / 255.0f;
                                float g = img.image[pixelIdx+1] / 255.0f;
                                float b = img.image[pixelIdx+2] / 255.0f;
                                physMat.color *= glm::vec4(r, g, b, 1.0f);
                            }
                        }
                    }
                }
                
                physMat.density = 1.0f;
                physMat.friction = 0.5f;
                physMat.hardness = 1.0f;

                result.materials.push_back(physMat);
            }
        }

        std::vector<Triangle> triangles;
        glm::vec3 minBound(std::numeric_limits<float>::max());
        glm::vec3 maxBound(std::numeric_limits<float>::lowest());

        for (const auto& mesh : model.meshes) {
            for (const auto& primitive : mesh.primitives) {
                auto posIt = primitive.attributes.find("POSITION");
                if (posIt == primitive.attributes.end()) continue;

                const tinygltf::Accessor& posAccessor = model.accessors[posIt->second];
                const tinygltf::BufferView& posView = model.bufferViews[posAccessor.bufferView];
                const tinygltf::Buffer& posBuffer = model.buffers[posView.buffer];
                const float* positions = reinterpret_cast<const float*>(&posBuffer.data[posView.byteOffset + posAccessor.byteOffset]);
                
                int matId = primitive.material + 1;
                if (matId < 1) matId = 1;
                if (matId > (int)result.materials.size()) matId = 1;

                std::vector<uint32_t> indices;
                if (primitive.indices >= 0) {
                    const tinygltf::Accessor& idxAccessor = model.accessors[primitive.indices];
                    const tinygltf::BufferView& idxView = model.bufferViews[idxAccessor.bufferView];
                    const tinygltf::Buffer& idxBuffer = model.buffers[idxView.buffer];
                    
                    if (idxAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        const uint16_t* buf = reinterpret_cast<const uint16_t*>(&idxBuffer.data[idxView.byteOffset + idxAccessor.byteOffset]);
                        for (size_t i = 0; i < idxAccessor.count; ++i) indices.push_back(buf[i]);
                    } else if (idxAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        const uint32_t* buf = reinterpret_cast<const uint32_t*>(&idxBuffer.data[idxView.byteOffset + idxAccessor.byteOffset]);
                        for (size_t i = 0; i < idxAccessor.count; ++i) indices.push_back(buf[i]);
                    }
                }

                size_t triCount = indices.empty() ? posAccessor.count / 3 : indices.size() / 3;
                for (size_t i = 0; i < triCount; ++i) {
                    glm::vec3 v0, v1, v2;
                    if (indices.empty()) {
                        v0 = glm::make_vec3(&positions[(i * 3 + 0) * 3]);
                        v1 = glm::make_vec3(&positions[(i * 3 + 1) * 3]);
                        v2 = glm::make_vec3(&positions[(i * 3 + 2) * 3]);
                    } else {
                        v0 = glm::make_vec3(&positions[indices[i * 3 + 0] * 3]);
                        v1 = glm::make_vec3(&positions[indices[i * 3 + 1] * 3]);
                        v2 = glm::make_vec3(&positions[indices[i * 3 + 2] * 3]);
                    }

                    v0 *= settings.scale; v1 *= settings.scale; v2 *= settings.scale;
                    triangles.push_back({v0, v1, v2, matId});
                    minBound = glm::min(minBound, glm::min(v0, glm::min(v1, v2)));
                    maxBound = glm::max(maxBound, glm::max(v0, glm::max(v1, v2)));
                }
            }
        }

        result.minBound = minBound;
        result.maxBound = maxBound;

        int chunksX = std::max(1, (int)ceil((maxBound.x - minBound.x) / 32.0f));
        int chunksY = std::max(1, (int)ceil((maxBound.y - minBound.y) / 32.0f));
        int chunksZ = std::max(1, (int)ceil((maxBound.z - minBound.z) / 32.0f));

        Log::Info("Voxelizing mesh: " + std::to_string(chunksX) + "x" + std::to_string(chunksY) + "x" + std::to_string(chunksZ));

        for (int cz = 0; cz < chunksZ; ++cz) {
            for (int cy = 0; cy < chunksY; ++cy) {
                for (int cx = 0; cx < chunksX; ++cx) {
                    glm::vec3 chunkOrigin = minBound + glm::vec3(cx * 32, cy * 32, cz * 32);
                    
                    bool chunkNotEmpty = false;
                    auto chunkObj = std::make_shared<VoxelObject>();
                    chunkObj->chunk = std::make_shared<Chunk>();
                    
                    for (const auto& tri : triangles) {
                        glm::vec3 tv0 = tri.v0 - chunkOrigin;
                        glm::vec3 tv1 = tri.v1 - chunkOrigin;
                        glm::vec3 tv2 = tri.v2 - chunkOrigin;

                        if (TriBoxOverlap(glm::vec3(16.0f), glm::vec3(16.0f), tv0, tv1, tv2)) {
                             glm::vec3 localMin = glm::min(tv0, glm::min(tv1, tv2));
                             glm::vec3 localMax = glm::max(tv0, glm::max(tv1, tv2));
                             int minX = std::max(0, (int)floor(localMin.x)); int maxX = std::min(31, (int)ceil(localMax.x));
                             int minY = std::max(0, (int)floor(localMin.y)); int maxY = std::min(31, (int)ceil(localMax.y));
                             int minZ = std::max(0, (int)floor(localMin.z)); int maxZ = std::min(31, (int)ceil(localMax.z));

                             for (int z = minZ; z <= maxZ; ++z) {
                                for (int y = minY; y <= maxY; ++y) {
                                    for (int x = minX; x <= maxX; ++x) {
                                        if(chunkObj->chunk->GetVoxel(x, y, z) != 0) continue;
                                        if (TriBoxOverlap(glm::vec3(x+0.5f, y+0.5f, z+0.5f), glm::vec3(0.5f), tv0, tv1, tv2)) {
                                            chunkObj->chunk->SetVoxel(x, y, z, (uint8_t)tri.materialIdx);
                                            chunkNotEmpty = true;
                                        }
                                    }
                                }
                             }
                        }
                    }

                    if (chunkNotEmpty) {
                        chunkObj->position = glm::vec3(cx * 32, cy * 32, cz * 32); 
                        chunkObj->scale = glm::vec3(1.0f); 
                        result.parts.push_back(chunkObj);
                    }
                }
            }
        }
        return result;
    }
}