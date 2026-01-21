module;

#include <vulkan/vulkan.h>
#define VMA_IMPLEMENTATION // Already defined in VulkanContext, but safe if guarded or separate TU. 
#include <vk_mem_alloc.h>
#include <stdexcept>

module vortex.memory;

import vortex.log; // For logging

namespace vortex::memory {

    MemoryAllocator::MemoryAllocator(VmaAllocator allocator, VkDevice device)
        : m_Allocator(allocator), m_Device(device) {}

    AllocatedBuffer MemoryAllocator::CreateBuffer(size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage) {
        AllocatedBuffer newBuffer;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;

        VmaAllocationCreateInfo vmaAllocInfo{};
        vmaAllocInfo.usage = memoryUsage;

        if (vmaCreateBuffer(m_Allocator, &bufferInfo, &vmaAllocInfo, 
            &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info) != VK_SUCCESS) {
            Log::Error("Failed to allocate buffer!");
            throw std::runtime_error("Buffer allocation failed");
        }

        return newBuffer;
    }

    void MemoryAllocator::DestroyBuffer(const AllocatedBuffer& buffer) {
        vmaDestroyBuffer(m_Allocator, buffer.buffer, buffer.allocation);
    }

    AllocatedImage MemoryAllocator::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage) {
        AllocatedImage newImage;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        VmaAllocationCreateInfo vmaAllocInfo{};
        vmaAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(m_Allocator, &imageInfo, &vmaAllocInfo, 
            &newImage.image, &newImage.allocation, nullptr) != VK_SUCCESS) {
            Log::Error("Failed to allocate image!");
            throw std::runtime_error("Image allocation failed");
        }

        // Create View
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = newImage.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_Device, &viewInfo, nullptr, &newImage.imageView) != VK_SUCCESS) {
            Log::Error("Failed to create image view!");
            throw std::runtime_error("Image view creation failed");
        }

        return newImage;
    }

    void MemoryAllocator::DestroyImage(const AllocatedImage& image) {
        vkDestroyImageView(m_Device, image.imageView, nullptr);
        vmaDestroyImage(m_Allocator, image.image, image.allocation);
    }

}