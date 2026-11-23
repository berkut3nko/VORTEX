module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

export module vortex.graphics:swapchain;

import :device;

export namespace vortex::graphics {

    /// @brief What the player asked for; the swapchain picks a mode the surface offers (perf.md P1).
    enum class PresentPreference : uint8_t { Fifo, FifoRelaxed, Immediate };
    /// @brief The first mode of the preference's order that `available` holds, else FIFO.
    /// Orders: Fifo {FIFO}; FifoRelaxed {FIFO_RELAXED, FIFO}; Immediate {IMMEDIATE, MAILBOX, FIFO}.
    VkPresentModeKHR ChoosePresentMode(PresentPreference preference, std::span<const VkPresentModeKHR> available);
    /// @brief "fifo", "fifo-relaxed", "immediate", "mailbox", else "other".
    const char* PresentModeName(VkPresentModeKHR mode);
    /// @brief "fifo", "relaxed", "immediate" (the --present values).
    const char* PresentPreferenceName(PresentPreference preference);
    /// @brief Parses a --present value; false for anything else.
    bool ParsePresentPreference(std::string_view text, PresentPreference& out);

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

        /// @param present The player's preference; the mode is ChoosePresentMode of it and the surface's modes.
        /// Three images are asked for (the driver may give more).
        bool Create(Device& device, uint32_t width, uint32_t height, PresentPreference present);
        /// @brief Recreates for a new size or present preference (waits for the device to be idle). Returns false on
        /// failure.
        bool Recreate(uint32_t width, uint32_t height, PresentPreference present);
        void Destroy();
        bool IsValid() const { return m_Swapchain != VK_NULL_HANDLE; }

        VkFormat Format() const { return m_Format; }
        VkExtent2D Extent() const { return m_Extent; }
        uint32_t ImageCount() const { return static_cast<uint32_t>(m_Images.size()); }
        VkImage Image(uint32_t index) const { return m_Images[index]; }
        VkImageView View(uint32_t index) const { return m_Views[index]; }
        /// @brief The mode the swapchain was built with (FIFO before the first build).
        VkPresentModeKHR PresentMode() const { return m_PresentMode; }

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
        VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_Views;
    };
}
