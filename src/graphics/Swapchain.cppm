module;

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vector>

export module vortex.graphics:swapchain;

import :context;
import vortex.memory;

namespace vortex::graphics {

    export class Swapchain {
    public:
        Swapchain() = default;
        ~Swapchain();

        bool Initialize(VulkanContext& context, VkSurfaceKHR surface, uint32_t width, uint32_t height);
        void Recreate(uint32_t width, uint32_t height);
        void Shutdown();

        uint32_t AcquireNextImage(VkSemaphore semaphore);
        VkResult Present(VkQueue queue, uint32_t imageIndex, VkSemaphore renderFinishedSemaphore);

        VkFormat GetFormat() const { return m_SwapchainFormat; }
        VkFormat GetDepthFormat() const { return VK_FORMAT_D32_SFLOAT; } // Standard depth format
        VkExtent2D GetExtent() const { return m_SwapchainExtent; }
        const std::vector<VkImage>& GetImages() const { return m_Images; }
        const std::vector<VkImageView>& GetImageViews() const { return m_ImageViews; }
        
        // Return Depth Image for attachment
        const memory::AllocatedImage& GetDepthImage() const { return m_DepthImage; }

    private:
        VulkanContext* m_Context{nullptr};
        VkSurfaceKHR m_Surface{VK_NULL_HANDLE};
        
        vkb::Swapchain m_VkbSwapchain;
        VkSwapchainKHR m_Swapchain{VK_NULL_HANDLE};
        
        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;
        VkExtent2D m_SwapchainExtent{0, 0};
        VkFormat m_SwapchainFormat{VK_FORMAT_UNDEFINED};

        memory::AllocatedImage m_DepthImage; // NEW

        void Create(uint32_t width, uint32_t height);
        void DestroyInternal();
    };
}