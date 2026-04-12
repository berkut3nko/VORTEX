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
        VkSampler sampler{VK_NULL_HANDLE};
    };

    /**
     * @brief Abstraction for VMA allocator operations.
     */
    export class MemoryAllocator {
    public:
        MemoryAllocator(VmaAllocator allocator, VkDevice device);
        ~MemoryAllocator() = default;

        /**
         * @brief Allocates a Vulkan Buffer.
         * @param size Size in bytes.
         * @param usage Vulkan buffer usage flags.
         * @param memoryUsage VMA memory usage hint (e.g., GPU_ONLY).
         * @return The allocated buffer struct.
         */
        AllocatedBuffer CreateBuffer(size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
        
        /**
         * @brief Destroys a buffer and frees its memory.
         */
        void DestroyBuffer(const AllocatedBuffer& buffer);
        
        /**
         * @brief Allocates a Vulkan Image.
         * @param width Image width.
         * @param height Image height.
         * @param format Vulkan format.
         * @param usage Image usage flags.
         * @param memoryUsage VMA memory usage hint.
         * @return The allocated image struct.
         */
        AllocatedImage CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VmaMemoryUsage memoryUsage);
        
        /**
         * @brief Destroys an image and frees its memory.
         */
        void DestroyImage(const AllocatedImage& image);

        /**
         * @brief Returns the raw VMA allocator handle.
         * @return VmaAllocator handle.
         */
        VmaAllocator GetVmaAllocator() const { return m_Allocator; }

    private:
        VmaAllocator m_Allocator;
        VkDevice m_Device;
    };
}