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

module vortex.graphics;

import :scenemanager;
import vortex.memory;
import :camera_struct;
import vortex.log;
import vortex.voxel;

namespace vortex::graphics {

    float Halton(int index, int base) {
        float f = 1.0f; float r = 0.0f;
        while (index > 0) { f = f / (float)base; r = r + f * (index % base); index = index / base; }
        return r;
    }

    SceneManager::~SceneManager() { Shutdown(); }

    void SceneManager::Initialize(memory::MemoryAllocator* allocator) {
        m_Allocator = allocator;
        
        m_CameraUBO = allocator->CreateBuffer(sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        // Ensure buffer size is enough for the larger material structure
        m_MaterialsSSBO = allocator->CreateBuffer(sizeof(voxel::PhysicalMaterial) * 2048, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        m_ObjectsSSBO = allocator->CreateBuffer(sizeof(GPUObject) * 10000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        
        VkResult result = vmaMapMemory(m_Allocator->GetVmaAllocator(), m_ObjectsSSBO.allocation, &m_MappedObjectBuffer);
        if (result != VK_SUCCESS) Log::Error("Failed to map object buffer!");

        m_ChunksSSBO = allocator->CreateBuffer(1024*1024*64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    void SceneManager::Shutdown() {
        if (m_Allocator) {
            if (m_MappedObjectBuffer) {
                vmaUnmapMemory(m_Allocator->GetVmaAllocator(), m_ObjectsSSBO.allocation);
                m_MappedObjectBuffer = nullptr;
            }
            m_Allocator->DestroyBuffer(m_CameraUBO);
            m_Allocator->DestroyBuffer(m_MaterialsSSBO);
            m_Allocator->DestroyBuffer(m_ObjectsSSBO);
            m_Allocator->DestroyBuffer(m_ChunksSSBO);
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

        return m_VisibleGPUObjects.size();
    }
}