module;

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vector>

export module vortex.graphics:swapchain;

import :context;
import vortex.memory;

namespace vortex::graphics {

    /**
     * @brief Manages the Swapchain and the intermediate Render Target for raytracing.
     */
    export class Swapchain {
    public:
        Swapchain() = default;
        ~Swapchain();

        /**
         * @brief Initializes the swapchain.
         */
        bool Initialize(VulkanContext& context, VkSurfaceKHR surface, uint32_t width, uint32_t height);
        
        /**
         * @brief Recreates swapchain (e.g., after resize).
         */
        void Recreate(uint32_t width, uint32_t height);
        
        /**
         * @brief Cleans up resources.
         */
        void Shutdown();

        /**
         * @brief Acquires the next image index.
         * @return The index, or UINT32_MAX if out of date.
         */
        uint32_t AcquireNextImage(VkSemaphore semaphore);

        /**
         * @brief Presents the image.
         */
        VkResult Present(VkQueue queue, uint32_t imageIndex, VkSemaphore renderFinishedSemaphore);

        VkFormat GetFormat() const { return m_SwapchainFormat; }
        VkExtent2D GetExtent() const { return m_SwapchainExtent; }
        const std::vector<VkImage>& GetImages() const { return m_Images; }
        const std::vector<VkImageView>& GetImageViews() const { return m_ImageViews; }
        const memory::AllocatedImage& GetRenderTarget() const { return m_RenderTarget; }

    private:
        VulkanContext* m_Context{nullptr};
        VkSurfaceKHR m_Surface{VK_NULL_HANDLE}; // Not owned
        
        vkb::Swapchain m_VkbSwapchain;
        VkSwapchainKHR m_Swapchain{VK_NULL_HANDLE};
        
        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;
        VkExtent2D m_SwapchainExtent{0, 0};
        VkFormat m_SwapchainFormat{VK_FORMAT_UNDEFINED};

        memory::AllocatedImage m_RenderTarget;

        void Create(uint32_t width, uint32_t height);
        void DestroyInternal();
    };
}