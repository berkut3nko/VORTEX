module;

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>

export module vortex.memory;

namespace vortex::memory {

    /**
     * @brief Represents a buffer allocated on GPU/CPU visible memory.
     */
    export struct AllocatedBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VmaAllocationInfo info{};
    };

    /**
     * @brief Represents an image allocated on GPU.
     */
    export struct AllocatedImage {
        VkImage image{VK_NULL_HANDLE};
        VkImageView imageView{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
    };

    /**
     * @brief abstraction for VMA allocator operations.
     */
    export class MemoryAllocator {
    public:
        MemoryAllocator(VmaAllocator allocator, VkDevice device);
        ~MemoryAllocator() = default;

        AllocatedBuffer CreateBuffer(size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
        void DestroyBuffer(const AllocatedBuffer& buffer);

        AllocatedImage CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
        void DestroyImage(const AllocatedImage& image);

    private:
        VmaAllocator m_Allocator;
        VkDevice m_Device;
    };
}