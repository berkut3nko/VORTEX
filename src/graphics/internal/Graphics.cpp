module;

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>

module vortex.graphics;

import vortex.log;
import vortex.memory;
import vortex.voxel;

namespace vortex::graphics {

    constexpr int FRAMES_IN_FLIGHT = 2;

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

        Camera camera;
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
        vkCreateCommandPool(i.context.GetDevice(), &poolInfo, nullptr, &i.commandPool);

        i.commandBuffers.resize(FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = i.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = FRAMES_IN_FLIGHT;
        vkAllocateCommandBuffers(i.context.GetDevice(), &allocInfo, i.commandBuffers.data());

        i.InitSyncObjects();

        auto* mem = i.context.GetAllocator();
        
        i.cameraUBO = mem->CreateBuffer(sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.materialsSSBO = mem->CreateBuffer(sizeof(voxel::PhysicalMaterial) * 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.objectsSSBO = mem->CreateBuffer(65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        i.chunksSSBO = mem->CreateBuffer(1024*1024*32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

        i.rasterPipeline.Initialize(i.context.GetDevice(), 
                                    i.swapchain.GetFormat(), 
                                    i.swapchain.GetDepthFormat(),
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
            i.rasterPipeline.UpdateDescriptors();
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

        VkImageMemoryBarrier toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; 
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.image = i.swapchain.GetImages()[i.imageIndex];
        toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toColor.srcAccessMask = 0;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        VkImageMemoryBarrier toDepth{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toDepth.image = i.swapchain.GetDepthImage().image;
        toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        toDepth.srcAccessMask = 0;
        toDepth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkImageMemoryBarrier barriers[] = {toColor, toDepth};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

        VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = i.swapchain.GetImageViews()[i.imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{0.5f, 0.7f, 0.9f, 1.0f}};

        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = i.swapchain.GetDepthImage().imageView;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderInfo.renderArea = {{0, 0}, i.swapchain.GetExtent()};
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttachment;
        renderInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(cmd, &renderInfo);

        VkViewport viewport{0.0f, 0.0f, (float)i.swapchain.GetExtent().width, (float)i.swapchain.GetExtent().height, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, i.swapchain.GetExtent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        i.rasterPipeline.Bind(cmd);
        vkCmdDraw(cmd, 36, i.objectCount, 0, 0);

        vkCmdEndRendering(cmd);

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
        ubo.viewProj = proj * view;

        void* data;
        vmaMapMemory(i.context.GetVmaAllocator(), i.cameraUBO.allocation, &data);
        memcpy(data, &ubo, sizeof(CameraUBO));
        vmaUnmapMemory(i.context.GetVmaAllocator(), i.cameraUBO.allocation);
    }

    void GraphicsContext::UploadScene(
        const std::vector<graphics::SceneObject>& objects, 
        const std::vector<graphics::SceneMaterial>& materials,
        const std::vector<voxel::Chunk>& chunks 
    ) {
        auto& i = *m_Internal;
        i.objectCount = (uint32_t)objects.size();
        
        void* data;
        
        // --- 1. Upload Materials (FIXED) ---
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
        // Копіюємо ПОВНИЙ розмір масиву PhysicalMaterial
        memcpy(data, gpuMaterials.data(), gpuMaterials.size() * sizeof(voxel::PhysicalMaterial));
        vmaUnmapMemory(i.context.GetVmaAllocator(), i.materialsSSBO.allocation);

        // --- 2. Upload Objects ---
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
            gpuObjects[k].chunkIndex = objects[k].materialIdx; 
            gpuObjects[k].paletteOffset = 0; 
            gpuObjects[k].flags = 0;
        }

        vmaMapMemory(i.context.GetVmaAllocator(), i.objectsSSBO.allocation, &data);
        memcpy(data, gpuObjects.data(), gpuObjects.size() * sizeof(GPUObject));
        vmaUnmapMemory(i.context.GetVmaAllocator(), i.objectsSSBO.allocation);

        // --- 3. Upload Chunks ---
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
