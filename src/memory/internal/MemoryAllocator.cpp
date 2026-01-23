module;
// ВИЗНАЧАЄМО РЕАЛІЗАЦІЮ VMA ТУТ (ТІЛЬКИ В ОДНОМУ ФАЙЛІ ПРОЕКТУ)
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>
#include <stdexcept>
module vortex.memory;

namespace vortex::memory {

    MemoryAllocator::MemoryAllocator(VmaAllocator allocator, VkDevice device) 
        : m_Allocator(allocator), m_Device(device) {}

    AllocatedBuffer MemoryAllocator::CreateBuffer(size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage) {
        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        
        VmaAllocationCreateInfo vmaallocInfo = {};
        vmaallocInfo.usage = memoryUsage;

        AllocatedBuffer newBuffer;
        if (vmaCreateBuffer(m_Allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create buffer");
        }
        return newBuffer;
    }

    AllocatedImage MemoryAllocator::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VmaMemoryUsage memoryUsage) {
        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        
        // Critical for performance on integrated GPUs
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL; 
        
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo vmaallocInfo = {};
        vmaallocInfo.usage = memoryUsage;
        
        // Ensure DEVICE_LOCAL is set for GPU_ONLY usage
        if (memoryUsage == VMA_MEMORY_USAGE_GPU_ONLY) {
            vmaallocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }

        AllocatedImage newImage;
        if (vmaCreateImage(m_Allocator, &imageInfo, &vmaallocInfo, &newImage.image, &newImage.allocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image");
        }
        
        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = newImage.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        
        if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM) {
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        } else {
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_Device, &viewInfo, nullptr, &newImage.imageView) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image view");
        }

        return newImage;
    }

    void MemoryAllocator::DestroyBuffer(const AllocatedBuffer& buffer) {
        vmaDestroyBuffer(m_Allocator, buffer.buffer, buffer.allocation);
    }

    void MemoryAllocator::DestroyImage(const AllocatedImage& image) {
        if (image.imageView) vkDestroyImageView(m_Device, image.imageView, nullptr);
        if (image.image) vmaDestroyImage(m_Allocator, image.image, image.allocation);
    }
}