module;

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream> 
#include <vector>
#include <limits>

module vortex.graphics;

import :scenemanager;
import vortex.memory;
import :camera_struct;
import :light;
import vortex.log;
import vortex.voxel;

namespace vortex::graphics {

    float Halton(int index, int base) {
        float f = 1.0f; float r = 0.0f;
        while (index > 0) { f = f / (float)base; r = r + f * (index % base); index = index / base; }
        return r;
    }

    // AABB Helper
    struct AABB {
        glm::vec3 min{std::numeric_limits<float>::max()};
        glm::vec3 max{std::numeric_limits<float>::lowest()};
        
        void Grow(const glm::vec3& p) {
            min = glm::min(min, p);
            max = glm::max(max, p);
        }
        
        void Grow(const AABB& b) {
            min = glm::min(min, b.min);
            max = glm::max(max, b.max);
        }
        
        glm::vec3 Center() const { return (min + max) * 0.5f; }
        
        // Transform local AABB (0..32) by model matrix
        static AABB FromMatrix(const glm::mat4& m) {
            AABB box;
            // The chunk is 0..32 in local space. We transform the 8 corners.
            const glm::vec3 corners[8] = {
                {0,0,0}, {32,0,0}, {0,32,0}, {32,32,0},
                {0,0,32}, {32,0,32}, {0,32,32}, {32,32,32}
            };
            for(const auto& c : corners) {
                box.Grow(glm::vec3(m * glm::vec4(c, 1.0f)));
            }
            return box;
        }
    };

    SceneManager::~SceneManager() { Shutdown(); }

    void SceneManager::Initialize(memory::MemoryAllocator* allocator) {
        m_Allocator = allocator;
        
        m_CameraUBO = allocator->CreateBuffer(sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        m_LightUBO = allocator->CreateBuffer(sizeof(DirectionalLight), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

        m_MaterialsSSBO = allocator->CreateBuffer(sizeof(voxel::PhysicalMaterial) * 2048, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        m_ObjectsSSBO = allocator->CreateBuffer(sizeof(GPUObject) * 10000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        
        VkResult result = vmaMapMemory(m_Allocator->GetVmaAllocator(), m_ObjectsSSBO.allocation, &m_MappedObjectBuffer);
        if (result != VK_SUCCESS) Log::Error("Failed to map object buffer!");

        m_ChunksSSBO = allocator->CreateBuffer(1024*1024*64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        
        // Create TLAS Buffer (Size for ~4096 nodes, sufficient for ~2000 objects)
        m_TLASBuffer = allocator->CreateBuffer(sizeof(GPUBVHNode) * 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        vmaMapMemory(m_Allocator->GetVmaAllocator(), m_TLASBuffer.allocation, &m_MappedTLASBuffer);
    }

    void SceneManager::Shutdown() {
        if (m_Allocator) {
            if (m_MappedObjectBuffer) {
                vmaUnmapMemory(m_Allocator->GetVmaAllocator(), m_ObjectsSSBO.allocation);
                m_MappedObjectBuffer = nullptr;
            }
            if (m_MappedTLASBuffer) {
                vmaUnmapMemory(m_Allocator->GetVmaAllocator(), m_TLASBuffer.allocation);
                m_MappedTLASBuffer = nullptr;
            }
            m_Allocator->DestroyBuffer(m_CameraUBO);
            m_Allocator->DestroyBuffer(m_LightUBO);
            m_Allocator->DestroyBuffer(m_MaterialsSSBO);
            m_Allocator->DestroyBuffer(m_ObjectsSSBO);
            m_Allocator->DestroyBuffer(m_ChunksSSBO);
            m_Allocator->DestroyBuffer(m_TLASBuffer);
        }
    }

    void SceneManager::UploadSceneData(const std::vector<SceneObject>& objects, 
                                       const std::vector<vortex::voxel::PhysicalMaterial>& materials, 
                                       const std::vector<vortex::voxel::Chunk>& chunks) {
        m_CachedObjects = objects;

        if (!materials.empty()) {
            void* data;
            vmaMapMemory(m_Allocator->GetVmaAllocator(), m_MaterialsSSBO.allocation, &data);
            memcpy(data, materials.data(), materials.size() * sizeof(voxel::PhysicalMaterial));
            vmaUnmapMemory(m_Allocator->GetVmaAllocator(), m_MaterialsSSBO.allocation);
        }

        if (!chunks.empty()) {
            void* data;
            vmaMapMemory(m_Allocator->GetVmaAllocator(), m_ChunksSSBO.allocation, &data);
            memcpy(data, chunks.data(), chunks.size() * sizeof(vortex::voxel::Chunk));
            vmaUnmapMemory(m_Allocator->GetVmaAllocator(), m_ChunksSSBO.allocation);
        }
    }

    void SceneManager::SetObjectTransform(int index, const glm::mat4& newModel) {
        if (index >= 0 && index < (int)m_CachedObjects.size()) {
            m_CachedObjects[index].model = newModel;
        }
    }

    void SceneManager::UploadLightBuffer(const DirectionalLight& light) {
        void* data;
        vmaMapMemory(m_Allocator->GetVmaAllocator(), m_LightUBO.allocation, &data);
        memcpy(data, &light, sizeof(DirectionalLight));
        vmaUnmapMemory(m_Allocator->GetVmaAllocator(), m_LightUBO.allocation);
    }

    void SceneManager::UploadCameraBuffer(const Camera& camera, uint32_t width, uint32_t height, uint64_t frameCount, bool useJitter) {
        CameraUBO ubo{};
        ubo.position = glm::vec4(camera.position, 1.0f);
        ubo.direction = glm::vec4(camera.front, 0.0f);
        
        float aspect = (float)width / (float)height;
        glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
        glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.1f, 400.0f);
        proj[1][1] *= -1; 

        if (useJitter) {
            uint64_t sampleIdx = frameCount % 16;
            float jX = (Halton(sampleIdx + 1, 2) - 0.5f) / (float)width;
            float jY = (Halton(sampleIdx + 1, 3) - 0.5f) / (float)height;
            ubo.jitter = glm::vec4(jX, jY, 0.0f, 0.0f);
            
            glm::mat4 jitterProj = proj;
            jitterProj[2][0] += jX * 2.0f; 
            jitterProj[2][1] += jY * 2.0f;
            ubo.viewProj = jitterProj * view;
        } else {
            ubo.jitter = glm::vec4(0.0f);
            ubo.viewProj = proj * view;
        }
        
        ubo.view = view;
        ubo.proj = proj;

        ubo.projInverse = glm::inverse(ubo.viewProj);
        ubo.prevViewProj = m_PrevViewProj;
        m_PrevViewProj = ubo.viewProj; 
        ubo.viewInverse = glm::inverse(view);
        ubo.objectCount = (uint32_t)m_VisibleGPUObjects.size(); 

        void* data;
        vmaMapMemory(m_Allocator->GetVmaAllocator(), m_CameraUBO.allocation, &data);
        memcpy(data, &ubo, sizeof(CameraUBO));
        vmaUnmapMemory(m_Allocator->GetVmaAllocator(), m_CameraUBO.allocation);
    }

    // --- BVH Implementation ---

    int SceneManager::BuildBVHRecursive(std::vector<int>& objIndices, std::vector<BVHBuildNode>& nodes, const std::vector<SceneObject*>& objects) {
        BVHBuildNode node;
        
        // 1. Calculate AABB for this set of objects
        AABB box;
        for (int idx : objIndices) {
            box.Grow(AABB::FromMatrix(objects[idx]->model));
        }
        node.min = box.min;
        node.max = box.max;

        int nodeIndex = (int)nodes.size();
        nodes.push_back(node);

        // Leaf condition
        if (objIndices.size() == 1) {
            nodes[nodeIndex].objectIndex = objIndices[0]; // Original Index mapping handled in BuildTLAS
            return nodeIndex;
        }

        // Split
        glm::vec3 extent = box.max - box.min;
        int axis = 0;
        if (extent.y > extent.x) axis = 1;
        if (extent.z > extent.y && extent.z > extent.x) axis = 2;

        float splitPos = box.Center()[axis];

        std::vector<int> leftIndices, rightIndices;
        for (int idx : objIndices) {
            float center = AABB::FromMatrix(objects[idx]->model).Center()[axis];
            if (center < splitPos) leftIndices.push_back(idx);
            else rightIndices.push_back(idx);
        }

        // Fallback for unbalanced split (all in one side)
        if (leftIndices.empty() || rightIndices.empty()) {
            size_t mid = objIndices.size() / 2;
            leftIndices.assign(objIndices.begin(), objIndices.begin() + mid);
            rightIndices.assign(objIndices.begin() + mid, objIndices.end());
        }

        nodes[nodeIndex].left = BuildBVHRecursive(leftIndices, nodes, objects);
        nodes[nodeIndex].right = BuildBVHRecursive(rightIndices, nodes, objects);
        
        return nodeIndex;
    }

    void SceneManager::BuildTLAS(const std::vector<SceneObject*>& visibleObjects) {
        if (visibleObjects.empty()) return;

        std::vector<int> indices(visibleObjects.size());
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = i;

        std::vector<BVHBuildNode> buildNodes;
        buildNodes.reserve(visibleObjects.size() * 2);
        
        BuildBVHRecursive(indices, buildNodes, visibleObjects);

        // Flatten to GPU format
        std::vector<GPUBVHNode> gpuNodes;
        gpuNodes.reserve(buildNodes.size());

        for (const auto& bn : buildNodes) {
            GPUBVHNode gn;
            gn.aabbMin = bn.min;
            gn.aabbMax = bn.max;
            
            if (bn.objectIndex != -1) {
                // Leaf
                gn.leftChildOrInstance = (uint32_t)bn.objectIndex; // This is the index into m_VisibleGPUObjects
                gn.rightChildOrCount = 1; // Leaf marker (count > 0)
            } else {
                // Internal
                gn.leftChildOrInstance = (uint32_t)bn.left;
                gn.rightChildOrCount = (uint32_t)bn.right;
            }
            gpuNodes.push_back(gn);
        }
        
        // Fix up Leafs
        for (size_t i=0; i<buildNodes.size(); ++i) {
            if (buildNodes[i].objectIndex != -1) {
                gpuNodes[i].leftChildOrInstance = (uint32_t)buildNodes[i].objectIndex;
                gpuNodes[i].rightChildOrCount = 0xFFFFFFFF; // Mark as Leaf Sentinel
            }
        }

        if (m_MappedTLASBuffer) {
            memcpy(m_MappedTLASBuffer, gpuNodes.data(), gpuNodes.size() * sizeof(GPUBVHNode));
        }
    }

    size_t SceneManager::CullAndUpload(const Camera& camera, float aspectRatio) {
        if (m_CachedObjects.empty()) return 0;

        std::vector<SceneObject*> visiblePtrs;
        visiblePtrs.reserve(m_CachedObjects.size());
        
        for(auto& obj : m_CachedObjects) {
            visiblePtrs.push_back(&obj);
        }

        glm::vec3 camPos = camera.position;
        std::sort(visiblePtrs.begin(), visiblePtrs.end(), [camPos](SceneObject* a, SceneObject* b) {
            glm::vec3 posA = glm::vec3(a->model[3]);
            glm::vec3 posB = glm::vec3(b->model[3]);
            return glm::dot(posA - camPos, posA - camPos) < glm::dot(posB - camPos, posB - camPos);
        });

        m_VisibleGPUObjects.resize(visiblePtrs.size());
        for(size_t k=0; k < visiblePtrs.size(); ++k) {
            m_VisibleGPUObjects[k].model = visiblePtrs[k]->model;
            m_VisibleGPUObjects[k].invModel = glm::inverse(visiblePtrs[k]->model);
            m_VisibleGPUObjects[k].chunkIndex = visiblePtrs[k]->chunkIndex; 
            m_VisibleGPUObjects[k].paletteOffset = visiblePtrs[k]->paletteOffset; 
            m_VisibleGPUObjects[k].flags = 0;
        }

        if (!m_VisibleGPUObjects.empty() && m_MappedObjectBuffer) {
            memcpy(m_MappedObjectBuffer, m_VisibleGPUObjects.data(), m_VisibleGPUObjects.size() * sizeof(GPUObject));
        }

        // --- Build and Upload TLAS ---
        BuildTLAS(visiblePtrs);

        return m_VisibleGPUObjects.size();
    }
}