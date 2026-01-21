module;

#include <vulkan/vulkan.h>
#include <vector>

export module vortex.graphics:ui;

import :context;
import :window;

namespace vortex::graphics {

    /**
     * @brief Handles ImGui integration.
     */
    export class UIOverlay {
    public:
        UIOverlay() = default;
        ~UIOverlay();

        void Initialize(VulkanContext& context, Window& window, VkFormat swapchainFormat, VkExtent2D extent, const std::vector<VkImageView>& views);
        void Shutdown();
        
        // Call when swapchain resizes
        void OnResize(VkExtent2D extent, const std::vector<VkImageView>& views);

        void BeginFrame();
        void Render(VkCommandBuffer cmd, uint32_t imageIndex);

    private:
        VkDevice m_Device{VK_NULL_HANDLE};
        VkDescriptorPool m_ImguiPool{VK_NULL_HANDLE};
        VkRenderPass m_RenderPass{VK_NULL_HANDLE};
        std::vector<VkFramebuffer> m_Framebuffers;
        VkExtent2D m_Extent;

        void CreateRenderPass(VkFormat format);
        void CreateFramebuffers(const std::vector<VkImageView>& views);
    };
}