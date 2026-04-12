module;

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vector>

export module vortex.graphics:swapchain;

import :context;
import vortex.memory;

namespace vortex::graphics {

    /**
     * @brief Manages the Vulkan Swapchain and presentation images.
     * @details Handles double/triple buffering, image acquisition, and presentation to the surface.
     */
    export class Swapchain {
    public:
        Swapchain() = default;
        ~Swapchain();

        /**
         * @brief Initializes the swapchain for a specific window surface.
         * @param context The active Vulkan context.
         * @param surface The window surface to present to.
         * @param width Initial width of the swapchain.
         * @param height Initial height of the swapchain.
         * @return True if initialization succeeded, false otherwise.
         */
        bool Initialize(VulkanContext& context, VkSurfaceKHR surface, uint32_t width, uint32_t height);

        /**
         * @brief Recreates the swapchain (e.g., on window resize).
         * @param width New width.
         * @param height New height.
         * @warning This waits for the device to be idle before recreation.
         */
        void Recreate(uint32_t width, uint32_t height);

        /**
         * @brief Destroys the swapchain and its resources.
         */
        void Shutdown();

        /**
         * @brief Requests the index of the next available image from the swapchain.
         * @param semaphore A semaphore to signal when the image is available.
         * @return The index of the next image, or UINT32_MAX on error/out-of-date.
         */
        uint32_t AcquireNextImage(VkSemaphore semaphore);

        /**
         * @brief Presents a rendered image to the presentation queue.
         * @param queue The queue to submit the presentation request to.
         * @param imageIndex The index of the image to present.
         * @param renderFinishedSemaphore A semaphore to wait on (signaling render completion).
         * @return VK_SUCCESS or an error code (e.g., VK_ERROR_OUT_OF_DATE_KHR).
         */
        VkResult Present(VkQueue queue, uint32_t imageIndex, VkSemaphore renderFinishedSemaphore);

        VkFormat GetFormat() const { return m_SwapchainFormat; }
        VkFormat GetDepthFormat() const { return VK_FORMAT_D32_SFLOAT; } 
        VkExtent2D GetExtent() const { return m_SwapchainExtent; }
        const std::vector<VkImage>& GetImages() const { return m_Images; }
        const std::vector<VkImageView>& GetImageViews() const { return m_ImageViews; }
        
        /**
         * @brief Returns the depth image associated with the swapchain resolution.
         */
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

        memory::AllocatedImage m_DepthImage; 

        void Create(uint32_t width, uint32_t height);
        void DestroyInternal();
    };
}