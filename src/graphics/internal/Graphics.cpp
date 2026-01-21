module;

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <vk_mem_alloc.h> // Fixed: Added include
#include <GLFW/glfw3.h>   // Fixed: Added include

module vortex.graphics;

import vortex.log;
import vortex.memory;

namespace vortex::graphics {

    constexpr int FRAMES_IN_FLIGHT = 2;

    struct GraphicsInternal {
        Window window;
        VulkanContext context;
        Swapchain swapchain;
        UIOverlay ui;
        RayTracingPipeline rtPipeline;
        
        VkCommandPool commandPool{VK_NULL_HANDLE};
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkSemaphore> imageAvailableSemaphores;
        std::vector<VkSemaphore> renderFinishedSemaphores;
        std::vector<VkFence> inFlightFences;

        uint32_t currentFrame = 0;
        uint32_t imageIndex = 0;

        Camera camera;
        bool hasLoggedDispatch = false;
        
        // Fixed: Added member variable
        uint32_t objectCount = 0;

        memory::AllocatedBuffer cameraUBO;
        memory::AllocatedBuffer materialsSSBO;
        memory::AllocatedBuffer objectsSSBO;
        memory::AllocatedBuffer chunksSSBO;

        void InitSyncObjects();
    };

    GraphicsContext::GraphicsContext() : m_Internal(std::make_unique<GraphicsInternal>()) {}
    GraphicsContext::~GraphicsContext() { Shutdown(); }

    bool GraphicsContext::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        auto& i = *m_Internal;
        
        if (!i.window.Initialize(title, width, height)) return false;
        if (!i.context.InitInstance(title.c_str())) return false;

        VkSurfaceKHR surface = i.window.CreateSurface(i.context.GetInstance());
        if (surface == VK_NULL_HANDLE) return false;

        if (!i.context.InitDevice(surface)) return false;
        if (!i.swapchain.Initialize(i.context, surface, width, height)) return false;
        
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = i.context.GetQueueFamily();
        if (vkCreateCommandPool(i.context.GetDevice(), &poolInfo, nullptr, &i.commandPool) != VK_SUCCESS) {
            Log::Error("Failed to create Command Pool");
            return false;
        }

        i.commandBuffers.resize(FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = i.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = FRAMES_IN_FLIGHT;
        if (vkAllocateCommandBuffers(i.context.GetDevice(), &allocInfo, i.commandBuffers.data()) != VK_SUCCESS) {
             Log::Error("Failed to allocate Command Buffers");
             return false;
        }

        i.InitSyncObjects();

        auto* mem = i.context.GetAllocator();
        i.cameraUBO = mem->CreateBuffer(sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.materialsSSBO = mem->CreateBuffer(65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.objectsSSBO = mem->CreateBuffer(65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.chunksSSBO = mem->CreateBuffer(1024*1024*32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

        i.rtPipeline.Initialize(i.context.GetDevice(), i.swapchain.GetRenderTarget(), i.cameraUBO, i.materialsSSBO, i.objectsSSBO, i.chunksSSBO);
        
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
            i.rtPipeline.UpdateDescriptors(i.swapchain.GetRenderTarget());
            i.ui.OnResize(i.swapchain.GetExtent(), i.swapchain.GetImageViews());
            i.window.wasResized = false;
        }

        vkWaitForFences(i.context.GetDevice(), 1, &i.inFlightFences[i.currentFrame], VK_TRUE, UINT64_MAX);
        
        i.imageIndex = i.swapchain.AcquireNextImage(i.imageAvailableSemaphores[i.currentFrame]);
        if (i.imageIndex == UINT32_MAX) {
            int w, h; i.window.GetFramebufferSize(w, h);
            i.swapchain.Recreate(w, h);
            return true; 
        }

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

        // 1. Compute Raytracing
        VkImageMemoryBarrier toCompute{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toCompute.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; 
        toCompute.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toCompute.image = i.swapchain.GetRenderTarget().image;
        toCompute.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toCompute.srcAccessMask = 0;
        toCompute.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toCompute);

        uint32_t width = i.swapchain.GetExtent().width;
        uint32_t height = i.swapchain.GetExtent().height;

        if (!i.hasLoggedDispatch) {
            Log::Info("Dispatching Compute: " + std::to_string(width) + "x" + std::to_string(height));
            i.hasLoggedDispatch = true;
        }

        i.rtPipeline.Dispatch(cmd, width, height);

        // 2. Blit (Compute -> Swapchain)
        VkImageMemoryBarrier toTransferSrc = toCompute;
        toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransferSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toTransferSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkImageMemoryBarrier toTransferDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; 
        toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransferDst.image = i.swapchain.GetImages()[i.imageIndex];
        toTransferDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toTransferDst.srcAccessMask = 0;
        toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransferSrc);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransferDst);

        VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 1.0f}};
        vkCmdClearColorImage(cmd, i.swapchain.GetImages()[i.imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &toTransferDst.subresourceRange);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)width, (int32_t)height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {(int32_t)width, (int32_t)height, 1};
        
        vkCmdBlitImage(cmd, 
            i.swapchain.GetRenderTarget().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 
            i.swapchain.GetImages()[i.imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 
            1, &blit, VK_FILTER_NEAREST);

        // 3. UI Render Pass
        VkImageMemoryBarrier toColorAtt = toTransferDst;
        toColorAtt.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toColorAtt.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColorAtt.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toColorAtt.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toColorAtt);

        i.ui.Render(cmd, i.imageIndex);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkSemaphore wait[] = {i.imageAvailableSemaphores[i.currentFrame]};
        VkPipelineStageFlags stage[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = wait;
        submit.pWaitDstStageMask = stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VkSemaphore sig[] = {i.renderFinishedSemaphores[i.currentFrame]};
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = sig;

        if (vkQueueSubmit(i.context.GetGraphicsQueue(), 1, &submit, i.inFlightFences[i.currentFrame]) != VK_SUCCESS) {
            Log::Error("Failed to submit draw command buffer!");
        }
        
        i.swapchain.Present(i.context.GetGraphicsQueue(), i.imageIndex, i.renderFinishedSemaphores[i.currentFrame]);
        i.currentFrame = (i.currentFrame + 1) % FRAMES_IN_FLIGHT;
    }

    void GraphicsContext::UploadCamera() {
        auto& i = *m_Internal;
        
        CameraUBO ubo{};
        ubo.position = glm::vec4(i.camera.position, 1.0f);
        ubo.direction = glm::vec4(i.camera.front, 0.0f);
        
        glm::mat4 view = glm::lookAt(i.camera.position, i.camera.position + i.camera.front, i.camera.up);
        float aspect = (float)i.swapchain.GetExtent().width / (float)i.swapchain.GetExtent().height;
        
        glm::mat4 proj = glm::perspective(glm::radians(i.camera.fov), aspect, 0.1f, 100.0f);
        proj[1][1] *= -1; 

        ubo.viewInverse = glm::inverse(view);
        ubo.projInverse = glm::inverse(proj);
        ubo.objectCount = i.objectCount;

        void* data;
        vmaMapMemory(i.context.GetVmaAllocator(), i.cameraUBO.allocation, &data);
        memcpy(data, &ubo, sizeof(CameraUBO));
        vmaUnmapMemory(i.context.GetVmaAllocator(), i.cameraUBO.allocation);
    }

    void GraphicsContext::UploadScene(const std::vector<SceneObject>& objects, const std::vector<SceneMaterial>& materials) {
        auto& i = *m_Internal;
        i.objectCount = (uint32_t)objects.size();
        void* data;
        
        vmaMapMemory(i.context.GetVmaAllocator(), i.materialsSSBO.allocation, &data);
        memcpy(data, materials.data(), materials.size() * sizeof(SceneMaterial));
        vmaUnmapMemory(i.context.GetVmaAllocator(), i.materialsSSBO.allocation);

        struct GPUObject {
            glm::mat4 model;
            glm::mat4 invModel;
            uint32_t chunkIndex;
            uint32_t paletteOffset;
            uint32_t flags;
            uint32_t pad;
        };
        std::vector<GPUObject> gpuObjects(objects.size());
        for(size_t k=0; k<objects.size(); ++k) {
            gpuObjects[k].model = objects[k].model;
            gpuObjects[k].invModel = glm::inverse(objects[k].model);
            gpuObjects[k].chunkIndex = 0; 
            gpuObjects[k].paletteOffset = 0; 
            gpuObjects[k].flags = 0;
        }

        vmaMapMemory(i.context.GetVmaAllocator(), i.objectsSSBO.allocation, &data);
        memcpy(data, gpuObjects.data(), gpuObjects.size() * sizeof(GPUObject));
        vmaUnmapMemory(i.context.GetVmaAllocator(), i.objectsSSBO.allocation);
    }

    GLFWwindow* GraphicsContext::GetWindow() {
        return m_Internal->window.GetNativeHandle();
    }

    void GraphicsContext::Shutdown() {
        if (!m_Internal) return;
        
        if (m_Internal->context.GetDevice() != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_Internal->context.GetDevice());
        }

        m_Internal->ui.Shutdown();
        m_Internal->rtPipeline.Shutdown();
        m_Internal->swapchain.Shutdown();
        m_Internal->window.Shutdown();
        m_Internal->context.Shutdown();
        m_Internal.reset();
    }
    
    Camera& GraphicsContext::GetCamera() { return m_Internal->camera; }
}