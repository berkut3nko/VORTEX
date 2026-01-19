module;

// Global Module Fragment
#include <memory>
#include <iostream>
#include <vector>
#include <algorithm>
#include <optional>
#include <array>

// Vulkan & GLFW
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

// Utils
#include <VkBootstrap.h>

// VMA Implementation
// Note: Warnings are suppressed globally in engine/CMakeLists.txt
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

// ImGui
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

module vortex.graphics;

namespace vortex::graphics {

    // Number of frames to process concurrently (Double Buffering)
    constexpr uint32_t FRAMES_IN_FLIGHT = 2;

    struct GraphicsContext::InternalState {
        // --- Core ---
        GLFWwindow* window{nullptr};
        vkb::Instance vkbInstance;
        vkb::PhysicalDevice vkbPhysicalDevice;
        vkb::Device vkbDevice;
        vkb::Swapchain vkbSwapchain;

        VkInstance instance{VK_NULL_HANDLE};
        VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
        VkDevice device{VK_NULL_HANDLE};
        VkSurfaceKHR surface{VK_NULL_HANDLE};

        // --- Swapchain ---
        VkSwapchainKHR swapchain{VK_NULL_HANDLE};
        std::vector<VkImage> swapchainImages;
        std::vector<VkImageView> swapchainImageViews;
        VkFormat swapchainImageFormat;
        VkExtent2D swapchainExtent;

        // --- Commands & Sync ---
        VkQueue graphicsQueue{VK_NULL_HANDLE};
        uint32_t graphicsQueueFamily{0};

        VkCommandPool commandPool{VK_NULL_HANDLE};
        std::vector<VkCommandBuffer> commandBuffers;

        // Sync Objects per frame
        std::vector<VkSemaphore> imageAvailableSemaphores;
        std::vector<VkSemaphore> renderFinishedSemaphores;
        std::vector<VkFence> inFlightFences;

        uint32_t currentFrame = 0;

        // --- Resources ---
        VkDescriptorPool imguiPool{VK_NULL_HANDLE};
        VmaAllocator allocator{VK_NULL_HANDLE};
    };

    GraphicsContext::GraphicsContext() : m_State(std::make_unique<InternalState>()) {}
    
    GraphicsContext::~GraphicsContext() { 
        Shutdown(); 
    }

    bool GraphicsContext::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        // 1. Initialize GLFW
        if (!glfwInit()) return false;

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        m_State->window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title.c_str(), nullptr, nullptr);
        if (!m_State->window) return false;

        // 2. Initialize Vulkan Instance
        vkb::InstanceBuilder builder;
        auto instance_ret = builder.set_app_name(title.c_str())
            .request_validation_layers(true)
            .require_api_version(1, 3, 0) // Require Vulkan 1.3 for Dynamic Rendering
            .use_default_debug_messenger()
            .build();

        if (!instance_ret) return false;
        m_State->vkbInstance = instance_ret.value();
        m_State->instance = m_State->vkbInstance.instance;

        // 3. Surface
        glfwCreateWindowSurface(m_State->instance, m_State->window, nullptr, &m_State->surface);

        // 4. Physical Device
        // Note: Enabling 'dynamic_rendering' feature is crucial here
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{m_State->vkbInstance};
        auto phys_ret = selector
            .set_surface(m_State->surface)
            .set_minimum_version(1, 3)
            .set_required_features_13(features13) 
            .select();

        if (!phys_ret) return false;
        m_State->vkbPhysicalDevice = phys_ret.value();
        m_State->physicalDevice = m_State->vkbPhysicalDevice.physical_device;

        // 5. Logical Device
        vkb::DeviceBuilder device_builder{m_State->vkbPhysicalDevice};
        auto device_ret = device_builder.build();
        if (!device_ret) return false;

        m_State->vkbDevice = device_ret.value();
        m_State->device = m_State->vkbDevice.device;

        // Queues
        auto graphics_queue_ret = m_State->vkbDevice.get_queue(vkb::QueueType::graphics);
        m_State->graphicsQueue = graphics_queue_ret.value();
        m_State->graphicsQueueFamily = m_State->vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        // 6. VMA
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.physicalDevice = m_State->physicalDevice;
        allocatorInfo.device = m_State->device;
        allocatorInfo.instance = m_State->instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        vmaCreateAllocator(&allocatorInfo, &m_State->allocator);

        // 7. Swapchain
        vkb::SwapchainBuilder swapchain_builder{m_State->vkbDevice};
        auto swap_ret = swapchain_builder.build();
        if (!swap_ret) return false;

        m_State->vkbSwapchain = swap_ret.value();
        m_State->swapchain = m_State->vkbSwapchain.swapchain;
        m_State->swapchainImages = m_State->vkbSwapchain.get_images().value();
        m_State->swapchainImageViews = m_State->vkbSwapchain.get_image_views().value();
        m_State->swapchainImageFormat = m_State->vkbSwapchain.image_format;
        m_State->swapchainExtent = m_State->vkbSwapchain.extent;

        // 8. Command Pool
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_State->graphicsQueueFamily;
        if (vkCreateCommandPool(m_State->device, &poolInfo, nullptr, &m_State->commandPool) != VK_SUCCESS) {
            return false;
        }

        // 9. Command Buffers
        m_State->commandBuffers.resize(FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_State->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)m_State->commandBuffers.size();
        vkAllocateCommandBuffers(m_State->device, &allocInfo, m_State->commandBuffers.data());

        // 10. Sync Objects
        m_State->imageAvailableSemaphores.resize(FRAMES_IN_FLIGHT);
        m_State->renderFinishedSemaphores.resize(FRAMES_IN_FLIGHT);
        m_State->inFlightFences.resize(FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled so we don't wait on first frame

        for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
            vkCreateSemaphore(m_State->device, &semaphoreInfo, nullptr, &m_State->imageAvailableSemaphores[i]);
            vkCreateSemaphore(m_State->device, &semaphoreInfo, nullptr, &m_State->renderFinishedSemaphores[i]);
            vkCreateFence(m_State->device, &fenceInfo, nullptr, &m_State->inFlightFences[i]);
        }

        // 11. ImGui
        VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = pool_sizes;
        vkCreateDescriptorPool(m_State->device, &pool_info, nullptr, &m_State->imguiPool);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        
        ImGui_ImplGlfw_InitForVulkan(m_State->window, true);
        
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = m_State->instance;
        init_info.PhysicalDevice = m_State->physicalDevice;
        init_info.Device = m_State->device;
        init_info.QueueFamily = m_State->graphicsQueueFamily;
        init_info.Queue = m_State->graphicsQueue;
        init_info.DescriptorPool = m_State->imguiPool;
        init_info.MinImageCount = 3; 
        init_info.ImageCount = 3;
        init_info.UseDynamicRendering = true;
        // Old ImGui workaround:
        // init_info.ColorAttachmentFormat = m_State->swapchainImageFormat;

        ImGui_ImplVulkan_Init(&init_info);

        return true;
    }

    void GraphicsContext::Shutdown() {
        if (!m_State) return;

        vkDeviceWaitIdle(m_State->device);

        // Cleanup Sync
        for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(m_State->device, m_State->imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(m_State->device, m_State->renderFinishedSemaphores[i], nullptr);
            vkDestroyFence(m_State->device, m_State->inFlightFences[i], nullptr);
        }

        vkDestroyCommandPool(m_State->device, m_State->commandPool, nullptr);

        // Cleanup ImGui
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(m_State->device, m_State->imguiPool, nullptr);
        
        vkb::destroy_swapchain(m_State->vkbSwapchain);
        vmaDestroyAllocator(m_State->allocator);
        
        vkb::destroy_device(m_State->vkbDevice);
        vkb::destroy_surface(m_State->vkbInstance, m_State->surface);
        vkb::destroy_instance(m_State->vkbInstance);

        glfwDestroyWindow(m_State->window);
        glfwTerminate();

        m_State.reset();
    }

    bool GraphicsContext::BeginFrame() {
        if (glfwWindowShouldClose(m_State->window)) return false;
        glfwPollEvents();

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        return true;
    }

    void GraphicsContext::EndFrame() {
        // 1. Render ImGui
        ImGui::Render();

        // 2. Wait for previous frame
        vkWaitForFences(m_State->device, 1, &m_State->inFlightFences[m_State->currentFrame], VK_TRUE, UINT64_MAX);

        // 3. Acquire next image
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(m_State->device, m_State->swapchain, UINT64_MAX, 
                                              m_State->imageAvailableSemaphores[m_State->currentFrame], 
                                              VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Recreate swapchain logic should go here, skipping for brevity
            return; 
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return;
        }

        vkResetFences(m_State->device, 1, &m_State->inFlightFences[m_State->currentFrame]);

        // 4. Record Command Buffer
        VkCommandBuffer cmd = m_State->commandBuffers[m_State->currentFrame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // -- Transition: Undefined -> Color Attachment --
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_State->swapchainImages[imageIndex];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // -- Begin Dynamic Rendering --
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = m_State->swapchainImageViews[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        
        VkClearValue clearColor = {{{0.1f, 0.1f, 0.12f, 1.0f}}}; // Dark gray background
        colorAttachment.clearValue = clearColor;

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = { {0, 0}, m_State->swapchainExtent };
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderInfo);

        // Render ImGui
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        vkCmdEndRendering(cmd);

        // -- Transition: Color Attachment -> Present --
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);

        // 5. Submit
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = {m_State->imageAvailableSemaphores[m_State->currentFrame]};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkSemaphore signalSemaphores[] = {m_State->renderFinishedSemaphores[m_State->currentFrame]};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        vkQueueSubmit(m_State->graphicsQueue, 1, &submitInfo, m_State->inFlightFences[m_State->currentFrame]);

        // 6. Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        VkSwapchainKHR swapchains[] = {m_State->swapchain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;

        vkQueuePresentKHR(m_State->graphicsQueue, &presentInfo);

        m_State->currentFrame = (m_State->currentFrame + 1) % FRAMES_IN_FLIGHT;
    }

}