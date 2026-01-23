module;

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>

module vortex.graphics;

import vortex.log;
import vortex.memory;
import vortex.voxel;
import :render_resources;
import :scene_manager;
import :taapipeline; 
import :fxaapipeline;

namespace vortex::graphics {

    constexpr int FRAMES_IN_FLIGHT = 2;

    struct GraphicsInternal {
        Window window;
        VulkanContext context;
        Swapchain swapchain;
        UIOverlay ui;
        
        // Modules
        RenderResources resources;
        SceneManager sceneManager;
        
        RasterPipeline rasterPipeline;
        TAAPipeline taaPipeline; 
        FXAAPipeline fxaaPipeline;

        // Synchronization
        VkCommandPool commandPool{VK_NULL_HANDLE};
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkSemaphore> imageAvailableSemaphores;
        std::vector<VkSemaphore> renderFinishedSemaphores;
        std::vector<VkFence> inFlightFences;

        // State
        uint32_t currentFrame = 0;
        uint32_t imageIndex = 0;
        uint64_t frameCounter = 0;

        Camera camera;
        float renderScale = 1.0f; 
        AntiAliasingMode currentAAMode = AntiAliasingMode::FXAA;
        
        VkSampler defaultSampler{VK_NULL_HANDLE};

        // --- Methods ---
        void InitSyncObjects();
        
        /**
         * @brief Records commands for the current frame.
         * @param cmd The command buffer to record into.
         */
        void RecordCommandBuffer(VkCommandBuffer cmd);
        
        // Render Passes
        void PassGeometry(VkCommandBuffer cmd, size_t visibleCount);
        void PassAA(VkCommandBuffer cmd, memory::AllocatedImage*& outImage);
        void PassBlit(VkCommandBuffer cmd, memory::AllocatedImage* source);
        void PassUI(VkCommandBuffer cmd);
        
        // Helpers
        void TransitionLayout(VkCommandBuffer cmd, VkImage img, VkImageLayout oldL, VkImageLayout newL);
    };

    GraphicsContext::GraphicsContext() : m_Internal(std::make_unique<GraphicsInternal>()) {}
    GraphicsContext::~GraphicsContext() { Shutdown(); }

    bool GraphicsContext::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        auto& i = *m_Internal;
        
        // 1. Core Vulkan Init
        if (!i.window.Initialize(title, width, height)) return false;
        if (!i.context.InitInstance(title.c_str())) return false;
        if (!i.context.InitDevice(i.window.CreateSurface(i.context.GetInstance()))) return false;
        if (!i.swapchain.Initialize(i.context, i.window.CreateSurface(i.context.GetInstance()), width, height)) return false;
        
        // 2. Command Pools
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

        // 3. Shared Resources
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR; samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(i.context.GetDevice(), &samplerInfo, nullptr, &i.defaultSampler);

        i.resources.Initialize(i.context.GetAllocator(), width, height);
        
        // 4. Scene Management
        i.sceneManager.Initialize(i.context.GetAllocator());

        // 5. Pipelines
        i.rasterPipeline.Initialize(i.context.GetDevice(), 
                                    VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_FORMAT_R16G16_SFLOAT,
                                    VK_FORMAT_D32_SFLOAT,
                                    FRAMES_IN_FLIGHT,
                                    i.sceneManager.GetCameraBuffer(), 
                                    i.sceneManager.GetMaterialBuffer(), 
                                    i.sceneManager.GetObjectBuffer(), 
                                    i.sceneManager.GetChunkBuffer());
        
        i.ui.Initialize(i.context, i.window, i.swapchain.GetFormat(), i.swapchain.GetExtent(), i.swapchain.GetImageViews());
        
        Log::Info("Graphics Subsystem Fully Initialized.");
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
            if (w > 0 && h > 0) {
                i.swapchain.Recreate(w, h);
                i.resources.Initialize(i.context.GetAllocator(), w, h);
                i.taaPipeline.Shutdown(); 
                i.fxaaPipeline.Shutdown();
                i.ui.OnResize(i.swapchain.GetExtent(), i.swapchain.GetImageViews());
            }
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
        i.RecordCommandBuffer(i.commandBuffers[i.currentFrame]);
        
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkSemaphore wait[] = {i.imageAvailableSemaphores[i.currentFrame]};
        VkPipelineStageFlags stage[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = wait; submit.pWaitDstStageMask = stage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &i.commandBuffers[i.currentFrame];
        VkSemaphore sig[] = {i.renderFinishedSemaphores[i.currentFrame]};
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = sig;
        
        vkQueueSubmit(i.context.GetGraphicsQueue(), 1, &submit, i.inFlightFences[i.currentFrame]);
        i.swapchain.Present(i.context.GetGraphicsQueue(), i.imageIndex, i.renderFinishedSemaphores[i.currentFrame]);
        
        i.currentFrame = (i.currentFrame + 1) % FRAMES_IN_FLIGHT;
        i.frameCounter++;
    }

    void GraphicsInternal::RecordCommandBuffer(VkCommandBuffer cmd) {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &begin);

        // 1. Scene Logic
        float aspect = (float)swapchain.GetExtent().width / (float)swapchain.GetExtent().height;
        size_t visibleCount = sceneManager.CullAndUpload(camera, aspect);

        // 2. Geometry Pass
        PassGeometry(cmd, visibleCount);

        // 3. Anti-Aliasing Pass
        memory::AllocatedImage* finalImage = nullptr;
        PassAA(cmd, finalImage);

        // 4. Blit to Swapchain
        PassBlit(cmd, finalImage);

        // 5. UI Overlay
        PassUI(cmd);

        // 6. Transition for Present
        TransitionLayout(cmd, swapchain.GetImages()[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        vkEndCommandBuffer(cmd);
    }

    void GraphicsInternal::PassGeometry(VkCommandBuffer cmd, size_t visibleCount) {
        TransitionLayout(cmd, resources.GetColor().image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        TransitionLayout(cmd, resources.GetVelocity().image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        TransitionLayout(cmd, resources.GetDepth().image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colors[2];
        colors[0] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[0].imageView = resources.GetColor().imageView;
        colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; 
        colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[0].clearValue.color = {{0.5f, 0.7f, 0.9f, 1.0f}}; 
        
        colors[1] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[1].imageView = resources.GetVelocity().imageView;
        colors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; 
        colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = resources.GetDepth().imageView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; 
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0}; 

        VkExtent2D renderExtent;
        renderExtent.width = (uint32_t)(swapchain.GetExtent().width * renderScale);
        renderExtent.height = (uint32_t)(swapchain.GetExtent().height * renderScale);
        
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

        if(visibleCount > 0) {
            rasterPipeline.Bind(cmd, currentFrame);
            vkCmdDraw(cmd, 36, (uint32_t)visibleCount, 0, 0);
        }
        
        vkCmdEndRendering(cmd);
    }

    void GraphicsInternal::PassAA(VkCommandBuffer cmd, memory::AllocatedImage*& outImage) {
        if (currentAAMode == AntiAliasingMode::FXAA) {
            if (!fxaaPipeline.IsInitialized()) fxaaPipeline.Initialize(context.GetDevice(), FRAMES_IN_FLIGHT);

            TransitionLayout(cmd, resources.GetColor().image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionLayout(cmd, resources.GetResolve().image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

            uint32_t w = (uint32_t)(swapchain.GetExtent().width * renderScale);
            uint32_t h = (uint32_t)(swapchain.GetExtent().height * renderScale);

            // FIX: Pass defaultSampler to avoid null sampler crash
            fxaaPipeline.Dispatch(cmd, currentFrame, defaultSampler, resources.GetColor(), resources.GetResolve(), w, h);
            outImage = const_cast<memory::AllocatedImage*>(&resources.GetResolve());
        } 
        else if (currentAAMode == AntiAliasingMode::TAA) {
            if (!taaPipeline.IsInitialized()) taaPipeline.Initialize(context.GetDevice(), FRAMES_IN_FLIGHT);

            auto& histRead = resources.GetHistoryRead();
            auto& histWrite = resources.GetHistoryWrite();

            TransitionLayout(cmd, resources.GetColor().image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionLayout(cmd, resources.GetVelocity().image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionLayout(cmd, resources.GetDepth().image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); 
            
            TransitionLayout(cmd, histWrite.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

            uint32_t w = (uint32_t)(swapchain.GetExtent().width * renderScale);
            uint32_t h = (uint32_t)(swapchain.GetExtent().height * renderScale);

            // FIX: Pass defaultSampler
            taaPipeline.Dispatch(cmd, currentFrame, defaultSampler, resources.GetColor(), histRead, resources.GetVelocity(), resources.GetDepth(), histWrite, w, h);
            
            outImage = const_cast<memory::AllocatedImage*>(&histWrite);
            resources.SwapHistory(); 
        } 
        else {
            outImage = const_cast<memory::AllocatedImage*>(&resources.GetColor());
        }
    }

    void GraphicsInternal::PassBlit(VkCommandBuffer cmd, memory::AllocatedImage* source) {
        VkImageLayout srcLayout = (source == &resources.GetColor()) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        if (currentAAMode == AntiAliasingMode::TAA) srcLayout = VK_IMAGE_LAYOUT_GENERAL; 
        
        TransitionLayout(cmd, source->image, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        TransitionLayout(cmd, swapchain.GetImages()[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageBlit blit{};
        blit.srcOffsets[1] = { (int)(swapchain.GetExtent().width * renderScale), (int)(swapchain.GetExtent().height * renderScale), 1 };
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = { (int)swapchain.GetExtent().width, (int)swapchain.GetExtent().height, 1 };
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        
        vkCmdBlitImage(cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain.GetImages()[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    void GraphicsInternal::PassUI(VkCommandBuffer cmd) {
        TransitionLayout(cmd, swapchain.GetImages()[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        
        VkRenderingAttachmentInfo col = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        col.imageView = swapchain.GetImageViews()[imageIndex];
        col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; 
        col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0,0}, swapchain.GetExtent()};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &col;

        vkCmdBeginRendering(cmd, &info);
        ui.Render(cmd, imageIndex);
        vkCmdEndRendering(cmd);
    }

    void GraphicsInternal::TransitionLayout(VkCommandBuffer cmd, VkImage img, VkImageLayout oldL, VkImageLayout newL) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = oldL;
        barrier.newLayout = newL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        if (newL == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || oldL == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void GraphicsContext::UploadCamera() { 
        auto& i = *m_Internal;
        bool useJitter = (i.currentAAMode == AntiAliasingMode::TAA);
        i.sceneManager.UploadCameraBuffer(i.camera, 
            i.swapchain.GetExtent().width, 
            i.swapchain.GetExtent().height, 
            i.frameCounter, 
            useJitter); 
    }

    void GraphicsContext::UploadScene(const std::vector<SceneObject>& o, 
                                      const std::vector<SceneMaterial>& m, 
                                      const std::vector<voxel::Chunk>& c) { 
        m_Internal->sceneManager.UploadSceneData(o, m, c); 
    }

    void GraphicsContext::SetAAMode(AntiAliasingMode mode) { 
        m_Internal->currentAAMode = mode; 
        m_Internal->resources.InvalidateHistory(); 
    }
    
    AntiAliasingMode GraphicsContext::GetAAMode() const { return m_Internal->currentAAMode; }
    Camera& GraphicsContext::GetCamera() { return m_Internal->camera; }
    GLFWwindow* GraphicsContext::GetWindow() { return m_Internal->window.GetNativeHandle(); }
    void GraphicsContext::Shutdown() { if(m_Internal) { vkDeviceWaitIdle(m_Internal->context.GetDevice()); m_Internal.reset(); } }
}