module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

export module vortex.graphics:swapchain;

import :device;

export namespace vortex::graphics {

    /**
     * @brief Presentation surface images.
     * @details Colour format is a UNORM format (the final shader applies the sRGB curve itself,
     * which is also what Dear ImGui expects). Images are created with COLOR_ATTACHMENT and
     * TRANSFER_SRC usage (screenshots).
     */
    class Swapchain {
    public:
        Swapchain() = default;
        ~Swapchain();
        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;

        /// @param vsync true: FIFO. false: MAILBOX if available, else IMMEDIATE, else FIFO.
        bool Create(Device& device, uint32_t width, uint32_t height, bool vsync);
        /// @brief Recreates for a new size (waits for the device to be idle). Returns false on failure.
        bool Recreate(uint32_t width, uint32_t height, bool vsync);
        void Destroy();
        bool IsValid() const { return m_Swapchain != VK_NULL_HANDLE; }

        VkFormat Format() const { return m_Format; }
        VkExtent2D Extent() const { return m_Extent; }
        uint32_t ImageCount() const { return static_cast<uint32_t>(m_Images.size()); }
        VkImage Image(uint32_t index) const { return m_Images[index]; }
        VkImageView View(uint32_t index) const { return m_Views[index]; }

        /**
         * @brief Acquires the next image.
         * @return false if the swapchain is out of date and must be recreated before rendering
         * (nothing was acquired and `signal` is unsignalled).
         */
        bool Acquire(VkSemaphore signal, uint32_t& outIndex);
        /// @brief Presents. Returns false if the swapchain should be recreated (out of date / suboptimal).
        bool Present(VkSemaphore wait, uint32_t index);

    private:
        Device* m_Device = nullptr;
        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        VkFormat m_Format = VK_FORMAT_UNDEFINED;
        VkExtent2D m_Extent{0, 0};
        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_Views;
    };
}
