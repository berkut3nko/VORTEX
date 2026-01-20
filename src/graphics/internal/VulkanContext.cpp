module;

#include <memory>
#include <iostream>
#include <vector>
#include <algorithm>
#include <optional>
#include <array>
#include <fstream>
#include <sstream>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>

#include <vk_mem_alloc.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

module vortex.graphics;

// Import partition directly because we are inside the module implementation
import :shader; 

import vortex.memory;
import vortex.core; 

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

        // --- Memory ---
        std::unique_ptr<memory::MemoryAllocator> memoryAllocator;

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

        // --- Ray Tracing Pipeline ---
        VkPipeline rayTracingPipeline{VK_NULL_HANDLE};
        VkPipelineLayout rayTracingPipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout rayTracingDescriptorSetLayout{VK_NULL_HANDLE};
        VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
        VkDescriptorSet rayTracingDescriptorSet{VK_NULL_HANDLE};

        memory::AllocatedImage renderTarget; 

        // --- Resources ---
        VkDescriptorPool imguiPool{VK_NULL_HANDLE};
        VmaAllocator allocator{VK_NULL_HANDLE}; 
    };

    GraphicsContext::GraphicsContext() : m_State(std::make_unique<InternalState>()) {}
    
    GraphicsContext::~GraphicsContext() { 
        Shutdown(); 
    }

    std::string ReadFile(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            Log::Error("Failed to open shader file: " + filepath);
            return "";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    bool GraphicsContext::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        if (!glfwInit()) return false;

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        m_State->window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title.c_str(), nullptr, nullptr);
        if (!m_State->window) return false;

        vkb::InstanceBuilder builder;
        auto instance_ret = builder.set_app_name(title.c_str())
            .request_validation_layers(true)
            .require_api_version(1, 3, 0)
            .use_default_debug_messenger()
            .build();

        if (!instance_ret) return false;
        m_State->vkbInstance = instance_ret.value();
        m_State->instance = m_State->vkbInstance.instance;

        glfwCreateWindowSurface(m_State->instance, m_State->window, nullptr, &m_State->surface);

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

        vkb::DeviceBuilder device_builder{m_State->vkbPhysicalDevice};
        auto device_ret = device_builder.build();
        if (!device_ret) return false;

        m_State->vkbDevice = device_ret.value();
        m_State->device = m_State->vkbDevice.device;

        auto graphics_queue_ret = m_State->vkbDevice.get_queue(vkb::QueueType::graphics);
        m_State->graphicsQueue = graphics_queue_ret.value();
        m_State->graphicsQueueFamily = m_State->vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.physicalDevice = m_State->physicalDevice;
        allocatorInfo.device = m_State->device;
        allocatorInfo.instance = m_State->instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        vmaCreateAllocator(&allocatorInfo, &m_State->allocator);

        m_State->memoryAllocator = std::make_unique<memory::MemoryAllocator>(m_State->allocator, m_State->device);

        vkb::SwapchainBuilder swapchain_builder{m_State->vkbDevice};
        auto swap_ret = swapchain_builder
            .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            .build();
        
        if (!swap_ret) return false;

        m_State->vkbSwapchain = swap_ret.value();
        m_State->swapchain = m_State->vkbSwapchain.swapchain;
        m_State->swapchainImages = m_State->vkbSwapchain.get_images().value();
        m_State->swapchainImageViews = m_State->vkbSwapchain.get_image_views().value();
        m_State->swapchainImageFormat = m_State->vkbSwapchain.image_format;
        m_State->swapchainExtent = m_State->vkbSwapchain.extent;

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_State->graphicsQueueFamily;
        if (vkCreateCommandPool(m_State->device, &poolInfo, nullptr, &m_State->commandPool) != VK_SUCCESS) {
            return false;
        }

        m_State->commandBuffers.resize(FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_State->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)m_State->commandBuffers.size();
        vkAllocateCommandBuffers(m_State->device, &allocInfo, m_State->commandBuffers.data());

        m_State->imageAvailableSemaphores.resize(FRAMES_IN_FLIGHT);
        m_State->renderFinishedSemaphores.resize(FRAMES_IN_FLIGHT);
        m_State->inFlightFences.resize(FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
            vkCreateSemaphore(m_State->device, &semaphoreInfo, nullptr, &m_State->imageAvailableSemaphores[i]);
            vkCreateSemaphore(m_State->device, &semaphoreInfo, nullptr, &m_State->renderFinishedSemaphores[i]);
            vkCreateFence(m_State->device, &fenceInfo, nullptr, &m_State->inFlightFences[i]);
        }

        // ============================
        // RAY TRACING INIT
        // ============================
        
        // A. Create Render Target Image (RGBA8)
        m_State->renderTarget = m_State->memoryAllocator->CreateImage(
            m_State->swapchainExtent.width,
            m_State->swapchainExtent.height,
            VK_FORMAT_R8G8B8A8_UNORM, 
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        );

        // B. Descriptor Set Layout
        VkDescriptorSetLayoutBinding binding0{};
        binding0.binding = 0;
        binding0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding0.descriptorCount = 1;
        binding0.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding0;

        vkCreateDescriptorSetLayout(m_State->device, &layoutInfo, nullptr, &m_State->rayTracingDescriptorSetLayout);

        // C. Pipeline Layout
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_State->rayTracingDescriptorSetLayout;
        
        vkCreatePipelineLayout(m_State->device, &pipelineLayoutInfo, nullptr, &m_State->rayTracingPipelineLayout);

        // D. Compile Shader
        std::string shaderSource = ReadFile("assets/shaders/raytracing.comp");
        if (shaderSource.empty()) {
            Log::Error("Raytracing shader source is empty!");
            return false;
        }
        std::vector<uint32_t> spv = ShaderCompiler::Compile(ShaderStage::Compute, shaderSource);

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = spv.size() * sizeof(uint32_t);
        createInfo.pCode = spv.data();

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(m_State->device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            return false;
        }

        // E. Compute Pipeline
        VkPipelineShaderStageCreateInfo shaderStageInfo{};
        shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shaderStageInfo.module = shaderModule;
        shaderStageInfo.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout = m_State->rayTracingPipelineLayout;
        pipelineInfo.stage = shaderStageInfo;

        if (vkCreateComputePipelines(m_State->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_State->rayTracingPipeline) != VK_SUCCESS) {
            return false;
        }
        vkDestroyShaderModule(m_State->device, shaderModule, nullptr);

        // F. Descriptors
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10 }
        };
        VkDescriptorPoolCreateInfo descPoolInfo{};
        descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descPoolInfo.maxSets = 10;
        descPoolInfo.poolSizeCount = 1;
        descPoolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(m_State->device, &descPoolInfo, nullptr, &m_State->descriptorPool);

        VkDescriptorSetAllocateInfo allocInfoDS{};
        allocInfoDS.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfoDS.descriptorPool = m_State->descriptorPool;
        allocInfoDS.descriptorSetCount = 1;
        allocInfoDS.pSetLayouts = &m_State->rayTracingDescriptorSetLayout;
        vkAllocateDescriptorSets(m_State->device, &allocInfoDS, &m_State->rayTracingDescriptorSet);

        // G. Update Descriptor Set
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageInfo.imageView = m_State->renderTarget.imageView;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_State->rayTracingDescriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_State->device, 1, &descriptorWrite, 0, nullptr);

        VkDescriptorPoolSize imgui_pool_sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
        VkDescriptorPoolCreateInfo imgui_pool_info = {};
        imgui_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        imgui_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        imgui_pool_info.maxSets = 1;
        imgui_pool_info.poolSizeCount = 1;
        imgui_pool_info.pPoolSizes = imgui_pool_sizes;
        vkCreateDescriptorPool(m_State->device, &imgui_pool_info, nullptr, &m_State->imguiPool);

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
        // init_info.ColorAttachmentFormat = m_State->swapchainImageFormat; // Commented out due to compilation error in current ImGui version
        ImGui_ImplVulkan_Init(&init_info);

        return true;
    }

    void GraphicsContext::Shutdown() {
        if (!m_State) return;

        vkDeviceWaitIdle(m_State->device);

        if (m_State->renderTarget.image) m_State->memoryAllocator->DestroyImage(m_State->renderTarget);
        vkDestroyPipeline(m_State->device, m_State->rayTracingPipeline, nullptr);
        vkDestroyPipelineLayout(m_State->device, m_State->rayTracingPipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_State->device, m_State->rayTracingDescriptorSetLayout, nullptr);
        vkDestroyDescriptorPool(m_State->device, m_State->descriptorPool, nullptr);

        for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(m_State->device, m_State->imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(m_State->device, m_State->renderFinishedSemaphores[i], nullptr);
            vkDestroyFence(m_State->device, m_State->inFlightFences[i], nullptr);
        }

        vkDestroyCommandPool(m_State->device, m_State->commandPool, nullptr);

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
        ImGui::Render();

        vkWaitForFences(m_State->device, 1, &m_State->inFlightFences[m_State->currentFrame], VK_TRUE, UINT64_MAX);

        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(m_State->device, m_State->swapchain, UINT64_MAX, 
                                              m_State->imageAvailableSemaphores[m_State->currentFrame], 
                                              VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result != VK_SUCCESS) return;

        vkResetFences(m_State->device, 1, &m_State->inFlightFences[m_State->currentFrame]);

        VkCommandBuffer cmd = m_State->commandBuffers[m_State->currentFrame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Dispatch Ray Tracing
        VkImageMemoryBarrier computeBarrier{};
        computeBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        computeBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        computeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        computeBarrier.image = m_State->renderTarget.image;
        computeBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        computeBarrier.srcAccessMask = 0;
        computeBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &computeBarrier);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_State->rayTracingPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_State->rayTracingPipelineLayout, 
            0, 1, &m_State->rayTracingDescriptorSet, 0, nullptr);

        vkCmdDispatch(cmd, 
            (m_State->swapchainExtent.width + 7) / 8, 
            (m_State->swapchainExtent.height + 7) / 8, 
            1);

        // Blit to Swapchain
        VkImageMemoryBarrier blitSrcBarrier = computeBarrier;
        blitSrcBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        blitSrcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        blitSrcBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        blitSrcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkImageMemoryBarrier blitDstBarrier{};
        blitDstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        blitDstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        blitDstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        blitDstBarrier.image = m_State->swapchainImages[imageIndex];
        blitDstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        blitDstBarrier.srcAccessMask = 0;
        blitDstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        VkImageMemoryBarrier barriers[] = { blitSrcBarrier, blitDstBarrier };
        vkCmdPipelineBarrier(cmd, 
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageBlit blitRegion{};
        blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blitRegion.srcOffsets[1] = { (int32_t)m_State->swapchainExtent.width, (int32_t)m_State->swapchainExtent.height, 1 };
        blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blitRegion.dstOffsets[1] = { (int32_t)m_State->swapchainExtent.width, (int32_t)m_State->swapchainExtent.height, 1 };

        vkCmdBlitImage(cmd, 
            m_State->renderTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_State->swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blitRegion, VK_FILTER_NEAREST);

        // ImGui Overlay
        VkImageMemoryBarrier imguiBarrier = blitDstBarrier;
        imguiBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imguiBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imguiBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imguiBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &imguiBarrier);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = m_State->swapchainImageViews[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; 
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = { {0, 0}, m_State->swapchainExtent };
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderInfo);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier presentBarrier = imguiBarrier;
        presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        presentBarrier.dstAccessMask = 0;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &presentBarrier);

        vkEndCommandBuffer(cmd);

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