module;

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

export module vortex.graphics:resources;

import :device;

export namespace vortex::graphics {

    /// @brief Where a buffer lives and how it is written.
    enum class BufferKind {
        GpuOnly,     ///< Device local. Filled with vkCmdCopyBuffer from a staging buffer.
        CpuToGpu,    ///< Host visible, persistently mapped, written sequentially by the CPU every frame.
        Staging,     ///< Host visible, persistently mapped, TRANSFER_SRC.
        Readback     ///< Host visible, persistently mapped, TRANSFER_DST (screenshots).
    };

    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;              ///< Valid for every kind except GpuOnly.
        explicit operator bool() const { return handle != VK_NULL_HANDLE; }
    };

    struct Image {
        VkImage handle = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{0, 0};
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   ///< Current layout, maintained by TransitionImage.
        explicit operator bool() const { return handle != VK_NULL_HANDLE; }
    };

    /// @brief Creates a buffer. `usage` is extended with the transfer bits the kind implies.
    Buffer CreateBuffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, BufferKind kind, std::string_view debugName);
    void DestroyBuffer(Device& device, Buffer& buffer);

    /// @brief Creates a 2D image with one mip level and a view covering it. Depth formats get a depth aspect.
    Image CreateImage2D(Device& device, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, std::string_view debugName);
    void DestroyImage(Device& device, Image& image);

    VkSampler CreateSampler(Device& device, VkFilter filter, VkSamplerAddressMode addressMode);

    /// @brief True for D16 / D32 / D24S8 / D32S8.
    bool IsDepthFormat(VkFormat format);

    /**
     * @brief Records a layout transition with precise stage / access masks for both layouts
     * (VK_KHR_synchronization2) and updates `image.layout`. No-op if the layout already matches.
     * @details Supported layouts: UNDEFINED (as source only), COLOR_ATTACHMENT_OPTIMAL,
     * DEPTH_ATTACHMENT_OPTIMAL, DEPTH_READ_ONLY_OPTIMAL, SHADER_READ_ONLY_OPTIMAL,
     * TRANSFER_SRC_OPTIMAL, TRANSFER_DST_OPTIMAL, PRESENT_SRC_KHR, GENERAL.
     * Transitioning from UNDEFINED discards the contents.
     */
    void TransitionImage(VkCommandBuffer cmd, Image& image, VkImageLayout newLayout);
    /// @brief Same for an image the engine does not wrap (swapchain images).
    void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout, VkImageLayout newLayout);

    /**
     * @brief Deferred destruction. Resources that a frame in flight may still use are destroyed
     * when the same frame slot comes around again (its fence has been waited on by then).
     */
    class DeletionQueue {
    public:
        void Push(std::function<void()> destroy) { m_Pending.push_back(std::move(destroy)); }
        void Flush() {
            for (auto& fn : m_Pending) fn();
            m_Pending.clear();
        }
        bool Empty() const { return m_Pending.empty(); }
    private:
        std::vector<std::function<void()>> m_Pending;
    };
}
