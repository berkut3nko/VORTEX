module;

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <VkBootstrap.h> // Included for vkb types if needed
#include <vector>

module vortex.graphics;

import :ui;
import vortex.log;

namespace vortex::graphics {

    UIOverlay::~UIOverlay() { Shutdown(); }

    void UIOverlay::Initialize(VulkanContext& context, Window& window, VkFormat swapchainFormat, VkExtent2D extent, const std::vector<VkImageView>& views) {
        m_Device = context.GetDevice();
        m_Extent = extent;

        // 1. Pool
        VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 } };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1000;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = pool_sizes;
        vkCreateDescriptorPool(m_Device, &pool_info, nullptr, &m_ImguiPool);

        // 2. Render Pass & Framebuffers
        CreateRenderPass(swapchainFormat);
        CreateFramebuffers(views);

        // 3. ImGui Context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        
        ImGui_ImplGlfw_InitForVulkan(window.GetNativeHandle(), true);
        
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = context.GetInstance();
        init_info.PhysicalDevice = context.GetPhysicalDevice();
        init_info.Device = context.GetDevice();
        init_info.QueueFamily = context.GetQueueFamily();
        init_info.Queue = context.GetGraphicsQueue();
        init_info.DescriptorPool = m_ImguiPool;
        init_info.MinImageCount = 2;
        init_info.ImageCount = 2;
        init_info.RenderPass = m_RenderPass;
        ImGui_ImplVulkan_Init(&init_info);

        // Upload Fonts
        // ImGui_ImplVulkan_CreateFontsTexture handles command buffer creation/submission internally
        // using the queue provided in init_info.
        if (!ImGui_ImplVulkan_CreateFontsTexture()) {
            Log::Error("Failed to create ImGui font texture!");
        }
        
        // No need to manually destroy font upload objects here, ImGui handles it or it stays until shutdown.
        // Wait for idle to ensure fonts are uploaded before rendering
        vkDeviceWaitIdle(m_Device);
    }

    void UIOverlay::CreateRenderPass(VkFormat format) {
        VkAttachmentDescription attachment = {};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; 
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_attachment = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_attachment;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        vkCreateRenderPass(m_Device, &info, nullptr, &m_RenderPass);
    }

    void UIOverlay::CreateFramebuffers(const std::vector<VkImageView>& views) {
        m_Framebuffers.resize(views.size());
        for (size_t i = 0; i < views.size(); i++) {
            VkImageView attachments[] = { views[i] };
            VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fbInfo.renderPass = m_RenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = attachments;
            fbInfo.width = m_Extent.width;
            fbInfo.height = m_Extent.height;
            fbInfo.layers = 1;
            vkCreateFramebuffer(m_Device, &fbInfo, nullptr, &m_Framebuffers[i]);
        }
    }

    void UIOverlay::OnResize(VkExtent2D extent, const std::vector<VkImageView>& views) {
        for (auto fb : m_Framebuffers) vkDestroyFramebuffer(m_Device, fb, nullptr);
        m_Extent = extent;
        CreateFramebuffers(views);
    }

    void UIOverlay::Shutdown() {
        if (m_Device) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            
            for (auto fb : m_Framebuffers) vkDestroyFramebuffer(m_Device, fb, nullptr);
            if (m_RenderPass) vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
            if (m_ImguiPool) vkDestroyDescriptorPool(m_Device, m_ImguiPool, nullptr);
            m_Device = VK_NULL_HANDLE;
        }
    }

    void UIOverlay::BeginFrame() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void UIOverlay::Render(VkCommandBuffer cmd, uint32_t imageIndex) {
        ImGui::Render();
        VkRenderPassBeginInfo rpInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpInfo.renderPass = m_RenderPass;
        rpInfo.framebuffer = m_Framebuffers[imageIndex];
        rpInfo.renderArea.extent = m_Extent;
        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vkCmdEndRenderPass(cmd);
    }
}