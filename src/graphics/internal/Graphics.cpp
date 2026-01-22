module;

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <algorithm> 
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
#include <array>

module vortex.graphics;

import vortex.log;
import vortex.memory;
import vortex.voxel;

namespace vortex::graphics {

    constexpr int FRAMES_IN_FLIGHT = 2;

    struct GPUObject {
        glm::mat4 model;
        glm::mat4 invModel;
        uint32_t chunkIndex;
        uint32_t paletteOffset;
        uint32_t flags;
        uint32_t pad;
    };

    // Halton Sequence Generator
    float Halton(int index, int base) {
        float f = 1.0f;
        float r = 0.0f;
        while (index > 0) {
            f = f / (float)base;
            r = r + f * (index % base);
            index = index / base;
        }
        return r;
    }
    
    // --- Frustum Culling Helpers ---
    struct ViewFrustum {
        std::array<glm::vec4, 6> planes;

        void Update(const glm::mat4& viewProj) {
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 2; ++j) {
                    planes[i * 2 + j].x = viewProj[0][3] + (j == 0 ? viewProj[0][i] : -viewProj[0][i]);
                    planes[i * 2 + j].y = viewProj[1][3] + (j == 0 ? viewProj[1][i] : -viewProj[1][i]);
                    planes[i * 2 + j].z = viewProj[2][3] + (j == 0 ? viewProj[2][i] : -viewProj[2][i]);
                    planes[i * 2 + j].w = viewProj[3][3] + (j == 0 ? viewProj[3][i] : -viewProj[3][i]);
                    float length = glm::length(glm::vec3(planes[i * 2 + j]));
                    planes[i * 2 + j] /= length;
                }
            }
        }

        bool IsSphereVisible(const glm::vec3& center, float radius) const {
            for (const auto& plane : planes) {
                if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) {
                    return false; 
                }
            }
            return true;
        }
    };

    struct GraphicsInternal {
        Window window;
        VulkanContext context;
        Swapchain swapchain;
        UIOverlay ui;
        
        RasterPipeline rasterPipeline;

        VkCommandPool commandPool{VK_NULL_HANDLE};
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkSemaphore> imageAvailableSemaphores;
        std::vector<VkSemaphore> renderFinishedSemaphores;
        std::vector<VkFence> inFlightFences;

        uint32_t currentFrame = 0;
        uint32_t imageIndex = 0;
        uint64_t frameCounter = 0;

        Camera camera;
        glm::mat4 prevViewProj{1.0f};

        float renderScale = 0.7f; 
        
        memory::AllocatedImage colorTarget;
        memory::AllocatedImage velocityTarget;
        memory::AllocatedImage depthTarget;
        
        memory::AllocatedImage historyTexture[2]; 
        int historyIndex = 0;
        
        VkSampler defaultSampler{VK_NULL_HANDLE};

        std::vector<SceneObject> cachedObjects;
        std::vector<GPUObject> visibleGPUObjects;

        memory::AllocatedBuffer cameraUBO;
        memory::AllocatedBuffer materialsSSBO;
        memory::AllocatedBuffer objectsSSBO;
        memory::AllocatedBuffer chunksSSBO;

        void InitSyncObjects();
        void CreateOffscreenResources(uint32_t w, uint32_t h);
    };

    GraphicsContext::GraphicsContext() : m_Internal(std::make_unique<GraphicsInternal>()) {}
    GraphicsContext::~GraphicsContext() { Shutdown(); }

    void GraphicsInternal::CreateOffscreenResources(uint32_t sw, uint32_t sh) {
        auto* mem = context.GetAllocator();
        uint32_t w = (uint32_t)(sw * renderScale);
        uint32_t h = (uint32_t)(sh * renderScale);
        
        colorTarget = mem->CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        velocityTarget = mem->CreateImage(w, h, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        depthTarget = mem->CreateImage(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

        historyTexture[0] = mem->CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        historyTexture[1] = mem->CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }

    bool GraphicsContext::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        auto& i = *m_Internal;
        
        if (!i.window.Initialize(title, width, height)) return false;
        if (!i.context.InitInstance(title.c_str())) return false;
        VkSurfaceKHR surface = i.window.CreateSurface(i.context.GetInstance());
        if (!i.context.InitDevice(surface)) return false;
        if (!i.swapchain.Initialize(i.context, surface, width, height)) return false;
        
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = i.context.GetQueueFamily();
        vkCreateCommandPool(i.context.GetDevice(), &poolInfo, nullptr, &i.commandPool);

        i.commandBuffers.resize(FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = i.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = FRAMES_IN_FLIGHT;
        vkAllocateCommandBuffers(i.context.GetDevice(), &allocInfo, i.commandBuffers.data());

        i.InitSyncObjects();

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(i.context.GetDevice(), &samplerInfo, nullptr, &i.defaultSampler);

        i.CreateOffscreenResources(width, height);

        auto* mem = i.context.GetAllocator();
        i.cameraUBO = mem->CreateBuffer(sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.materialsSSBO = mem->CreateBuffer(sizeof(voxel::PhysicalMaterial) * 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.objectsSSBO = mem->CreateBuffer(sizeof(GPUObject) * 10000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.chunksSSBO = mem->CreateBuffer(1024*1024*32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

        i.rasterPipeline.Initialize(i.context.GetDevice(), 
                                    VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_FORMAT_R16G16_SFLOAT,
                                    VK_FORMAT_D32_SFLOAT,
                                    i.cameraUBO, i.materialsSSBO, i.objectsSSBO, i.chunksSSBO);
        
        i.ui.Initialize(i.context, i.window, i.swapchain.GetFormat(), i.swapchain.GetExtent(), i.swapchain.GetImageViews());
        return true;
    }

    void GraphicsInternal::InitSyncObjects() {
        VkSemaphoreCreateInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo f = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        f.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        imageAvailableSemaphores.resize(FRAMES_IN_FLIGHT);
        renderFinishedSemaphores.resize(FRAMES_IN_FLIGHT);
        inFlightFences.resize(FRAMES_IN_FLIGHT);
        for(int i=0; i<FRAMES_IN_FLIGHT; i++) {
            vkCreateSemaphore(context.GetDevice(), &s, nullptr, &imageAvailableSemaphores[i]);
            vkCreateSemaphore(context.GetDevice(), &s, nullptr, &renderFinishedSemaphores[i]);
            vkCreateFence(context.GetDevice(), &f, nullptr, &inFlightFences[i]);
        }
    }

    bool GraphicsContext::BeginFrame() {
        auto& i = *m_Internal;
        if (i.window.ShouldClose()) return false;
        i.window.PollEvents();
        
        if (i.window.wasResized) {
            int w, h; i.window.GetFramebufferSize(w, h);
            i.swapchain.Recreate(w, h);
            
            auto* mem = i.context.GetAllocator();
            mem->DestroyImage(i.colorTarget);
            mem->DestroyImage(i.velocityTarget);
            mem->DestroyImage(i.depthTarget);
            mem->DestroyImage(i.historyTexture[0]);
            mem->DestroyImage(i.historyTexture[1]);
            i.CreateOffscreenResources(w, h);
            
            i.rasterPipeline.UpdateDescriptors();
            i.ui.OnResize(i.swapchain.GetExtent(), i.swapchain.GetImageViews());
            i.window.wasResized = false;
        }

        vkWaitForFences(i.context.GetDevice(), 1, &i.inFlightFences[i.currentFrame], VK_TRUE, UINT64_MAX);
        i.imageIndex = i.swapchain.AcquireNextImage(i.imageAvailableSemaphores[i.currentFrame]);
        if (i.imageIndex == UINT32_MAX) return true; 
        
        vkResetFences(i.context.GetDevice(), 1, &i.inFlightFences[i.currentFrame]);
        i.ui.BeginFrame();
        return true;
    }

    void GraphicsContext::EndFrame() {
        auto& i = *m_Internal;
        VkCommandBuffer cmd = i.commandBuffers[i.currentFrame];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &begin);

        // --- CULLING & SORTING ---
        i.visibleGPUObjects.clear();
        if (!i.cachedObjects.empty()) {
            float aspect = (float)i.swapchain.GetExtent().width / (float)i.swapchain.GetExtent().height;
            glm::mat4 view = glm::lookAt(i.camera.position, i.camera.position + i.camera.front, i.camera.up);
            glm::mat4 proj = glm::perspective(glm::radians(i.camera.fov), aspect, 0.1f, 100.0f);
            proj[1][1] *= -1;
            glm::mat4 viewProj = proj * view;

            ViewFrustum frustum;
            frustum.Update(viewProj);

            std::vector<SceneObject> visibleObjects;
            visibleObjects.reserve(i.cachedObjects.size());
            const float CHUNK_RADIUS = 28.0f; 

            for(const auto& obj : i.cachedObjects) {
                glm::vec3 worldCenter = glm::vec3(obj.model * glm::vec4(16.0f, 16.0f, 16.0f, 1.0f));
                float maxScale = glm::length(glm::vec3(obj.model[0])); 
                float radius = CHUNK_RADIUS * maxScale;

                if (frustum.IsSphereVisible(worldCenter, radius)) {
                    visibleObjects.push_back(obj);
                }
            }

            glm::vec3 camPos = i.camera.position;
            std::sort(visibleObjects.begin(), visibleObjects.end(), [camPos](const SceneObject& a, const SceneObject& b) {
                glm::vec3 posA = glm::vec3(a.model[3]);
                glm::vec3 posB = glm::vec3(b.model[3]);
                return glm::dot(posA - camPos, posA - camPos) < glm::dot(posB - camPos, posB - camPos);
            });

            i.visibleGPUObjects.resize(visibleObjects.size());
            for(size_t k=0; k<visibleObjects.size(); ++k) {
                i.visibleGPUObjects[k].model = visibleObjects[k].model;
                i.visibleGPUObjects[k].invModel = glm::inverse(visibleObjects[k].model);
                i.visibleGPUObjects[k].chunkIndex = visibleObjects[k].materialIdx; 
                i.visibleGPUObjects[k].paletteOffset = 0; 
                i.visibleGPUObjects[k].flags = 0;
            }

            if (!i.visibleGPUObjects.empty()) {
                vkCmdUpdateBuffer(cmd, i.objectsSSBO.buffer, 0, sizeof(GPUObject) * i.visibleGPUObjects.size(), i.visibleGPUObjects.data());

                VkBufferMemoryBarrier bufBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                bufBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bufBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                bufBarrier.buffer = i.objectsSSBO.buffer;
                bufBarrier.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 1, &bufBarrier, 0, nullptr);
            }
        }

        // --- 1. RENDER SCENE (To Offscreen) ---
        {
            VkImageMemoryBarrier bar[3];
            bar[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            bar[0].image = i.colorTarget.image; bar[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            bar[0].srcAccessMask = 0; bar[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            
            bar[1] = bar[0];
            bar[1].image = i.velocityTarget.image;
            
            bar[2] = bar[0];
            bar[2].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            bar[2].image = i.depthTarget.image;
            bar[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            bar[2].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 3, bar);

            VkRenderingAttachmentInfo colors[2];
            colors[0] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            colors[0].imageView = i.colorTarget.imageView;
            colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colors[0].clearValue.color = {{0.5f, 0.7f, 0.9f, 1.0f}};
            
            colors[1] = colors[0];
            colors[1].imageView = i.velocityTarget.imageView;
            colors[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

            VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depth.imageView = i.depthTarget.imageView;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};

            VkExtent2D renderExtent;
            renderExtent.width = (uint32_t)(i.swapchain.GetExtent().width * i.renderScale);
            renderExtent.height = (uint32_t)(i.swapchain.GetExtent().height * i.renderScale);

            VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
            renderInfo.renderArea = {{0,0}, renderExtent};
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 2;
            renderInfo.pColorAttachments = colors;
            renderInfo.pDepthAttachment = &depth;

            vkCmdBeginRendering(cmd, &renderInfo);
            VkViewport vp{0,0,(float)renderExtent.width, (float)renderExtent.height, 0, 1};
            VkRect2D sc{{0,0}, renderExtent};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            if(!i.visibleGPUObjects.empty()) {
                i.rasterPipeline.Bind(cmd);
                vkCmdDraw(cmd, 36, (uint32_t)i.visibleGPUObjects.size(), 0, 0);
            }
            vkCmdEndRendering(cmd);
        }

        // --- 2. TAA PASS (Simulated Blit) ---
        {
            VkImageMemoryBarrier srcBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            srcBar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            srcBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            srcBar.image = i.colorTarget.image;
            srcBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            srcBar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

            VkImageMemoryBarrier dstBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            dstBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            dstBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            dstBar.image = i.swapchain.GetImages()[i.imageIndex];
            dstBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            dstBar.srcAccessMask = 0;
            dstBar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            VkImageMemoryBarrier barriers[] = {srcBar, dstBar};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

            VkImageBlit blit{};
            blit.srcOffsets[1] = { (int)(i.swapchain.GetExtent().width * i.renderScale), (int)(i.swapchain.GetExtent().height * i.renderScale), 1 };
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = { (int)i.swapchain.GetExtent().width, (int)i.swapchain.GetExtent().height, 1 };
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

            vkCmdBlitImage(cmd, 
                i.colorTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                i.swapchain.GetImages()[i.imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, VK_FILTER_LINEAR); 
            
             VkImageMemoryBarrier toUI{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
             toUI.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
             toUI.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
             toUI.image = i.swapchain.GetImages()[i.imageIndex];
             toUI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
             toUI.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
             toUI.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
             vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toUI);
        }

        // --- 3. UI RENDER ---
        VkRenderingAttachmentInfo uiColor = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        uiColor.imageView = i.swapchain.GetImageViews()[i.imageIndex];
        uiColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        uiColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; 
        uiColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo uiInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        uiInfo.renderArea = {{0,0}, i.swapchain.GetExtent()};
        uiInfo.layerCount = 1;
        uiInfo.colorAttachmentCount = 1;
        uiInfo.pColorAttachments = &uiColor;

        vkCmdBeginRendering(cmd, &uiInfo);
        i.ui.Render(cmd, i.imageIndex);
        vkCmdEndRendering(cmd);
        
        VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.image = i.swapchain.GetImages()[i.imageIndex];
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

        vkEndCommandBuffer(cmd);
        
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkSemaphore wait[] = {i.imageAvailableSemaphores[i.currentFrame]};
        VkPipelineStageFlags stage[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = wait; submit.pWaitDstStageMask = stage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        VkSemaphore sig[] = {i.renderFinishedSemaphores[i.currentFrame]};
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = sig;
        vkQueueSubmit(i.context.GetGraphicsQueue(), 1, &submit, i.inFlightFences[i.currentFrame]);
        i.swapchain.Present(i.context.GetGraphicsQueue(), i.imageIndex, i.renderFinishedSemaphores[i.currentFrame]);
        i.currentFrame = (i.currentFrame + 1) % FRAMES_IN_FLIGHT;
        i.frameCounter++;
    }

    void GraphicsContext::UploadCamera() {
        auto& i = *m_Internal;
        
        CameraUBO ubo{};
        ubo.position = glm::vec4(i.camera.position, 1.0f);
        ubo.direction = glm::vec4(i.camera.front, 0.0f);
        
        float aspect = (float)i.swapchain.GetExtent().width / (float)i.swapchain.GetExtent().height;
        glm::mat4 view = glm::lookAt(i.camera.position, i.camera.position + i.camera.front, i.camera.up);
        glm::mat4 proj = glm::perspective(glm::radians(i.camera.fov), aspect, 0.1f, 100.0f);
        proj[1][1] *= -1; 

        uint64_t sampleIdx = i.frameCounter % 16;
        float jX = (Halton(sampleIdx + 1, 2) - 0.5f) / (float)(i.swapchain.GetExtent().width * i.renderScale);
        float jY = (Halton(sampleIdx + 1, 3) - 0.5f) / (float)(i.swapchain.GetExtent().height * i.renderScale);
        
        ubo.jitter = glm::vec4(jX, jY, 0.0f, 0.0f);

        glm::mat4 jitterProj = proj;
        jitterProj[2][0] += jX * 2.0f; 
        jitterProj[2][1] += jY * 2.0f;

        ubo.viewProj = jitterProj * view;
        ubo.prevViewProj = i.prevViewProj;
        
        i.prevViewProj = ubo.viewProj; 
        
        ubo.viewInverse = glm::inverse(view);
        ubo.projInverse = glm::inverse(jitterProj);
        ubo.objectCount = (uint32_t)i.visibleGPUObjects.size(); 

        void* data;
        vmaMapMemory(i.context.GetVmaAllocator(), i.cameraUBO.allocation, &data);
        memcpy(data, &ubo, sizeof(CameraUBO));
        vmaUnmapMemory(i.context.GetVmaAllocator(), i.cameraUBO.allocation);
    }

    void GraphicsContext::UploadScene(
        const std::vector<SceneObject>& objects, 
        const std::vector<SceneMaterial>& materials,
        const std::vector<voxel::Chunk>& chunks 
    ) {
        auto& i = *m_Internal;
        i.cachedObjects = objects;
        
        void* data;
        
        std::vector<voxel::PhysicalMaterial> gpuMaterials(materials.size());
        for(size_t k=0; k < materials.size(); ++k) {
            gpuMaterials[k].color = materials[k].color;
            gpuMaterials[k].density = 1.0f;
            gpuMaterials[k].friction = 0.5f;
            gpuMaterials[k].restitution = 0.0f;
            gpuMaterials[k].hardness = 1.0f;
            gpuMaterials[k].flags = 0;
        }

        vmaMapMemory(i.context.GetVmaAllocator(), i.materialsSSBO.allocation, &data);
        memcpy(data, gpuMaterials.data(), gpuMaterials.size() * sizeof(voxel::PhysicalMaterial));
        vmaUnmapMemory(i.context.GetVmaAllocator(), i.materialsSSBO.allocation);

        if (!chunks.empty()) {
            size_t chunkSize = sizeof(voxel::Chunk);
            size_t totalSize = chunks.size() * chunkSize;
            
            vmaMapMemory(i.context.GetVmaAllocator(), i.chunksSSBO.allocation, &data);
            memcpy(data, chunks.data(), totalSize);
            vmaUnmapMemory(i.context.GetVmaAllocator(), i.chunksSSBO.allocation);
        }
    }

    GLFWwindow* GraphicsContext::GetWindow() { return m_Internal->window.GetNativeHandle(); }

    void GraphicsContext::Shutdown() {
        if (!m_Internal) return;
        vkDeviceWaitIdle(m_Internal->context.GetDevice());
        m_Internal->ui.Shutdown();
        m_Internal->rasterPipeline.Shutdown();
        m_Internal->swapchain.Shutdown();
        m_Internal->window.Shutdown();
        m_Internal->context.Shutdown();
        m_Internal.reset();
    }
    
    Camera& GraphicsContext::GetCamera() { return m_Internal->camera; }
}