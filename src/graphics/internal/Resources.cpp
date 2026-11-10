module;

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <string>
#include <string_view>

module vortex.graphics;

import vortex.core;

namespace vortex::graphics {

    namespace {

        /// Pipeline stages and access types that use an image while it is in a given layout.
        struct LayoutSync {
            VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 access = VK_ACCESS_2_NONE;
        };

        /**
         * @brief The synchronisation scope of `layout` on one side of a transition.
         * @param asSource true for the old layout (what has to finish before the transition),
         *                 false for the new layout (what has to wait for it).
         */
        LayoutSync SyncForLayout(VkImageLayout layout, bool asSource) {
            switch (layout) {
                case VK_IMAGE_LAYOUT_UNDEFINED:
                    // Only valid as a source: nothing to wait for, the contents are discarded.
                    return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};

                case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                    return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};

                case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
                    return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};

                case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
                    // Depth test without writes, and / or sampled by a fragment shader.
                    return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

                case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                    return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

                case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                    return {VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};

                case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                    return {VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};

                case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                    // The presentation engine is outside the pipeline: no access on either side.
                    // As a destination nothing in this queue waits for the transition. As a source
                    // the transition is ordered after the stage the image-acquired semaphore is
                    // conventionally waited at.
                    return {asSource ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_NONE,
                            VK_ACCESS_2_NONE};

                case VK_IMAGE_LAYOUT_GENERAL:
                    return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};

                default:
                    // Not a layout the engine uses. Synchronise against everything: slow but correct.
                    Log::Error("TransitionImage: unsupported image layout {}, using a full barrier", static_cast<int>(layout));
                    return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
            }
        }

        VkBufferUsageFlags TransferUsage(BufferKind kind) {
            switch (kind) {
                case BufferKind::GpuOnly:  return VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferKind::CpuToGpu: return 0;
                case BufferKind::Staging:  return VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                case BufferKind::Readback: return VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            }
            return 0;
        }

        VmaAllocationCreateInfo AllocationInfo(BufferKind kind) {
            VmaAllocationCreateInfo info{};
            switch (kind) {
                case BufferKind::GpuOnly:
                    info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                    break;
                case BufferKind::CpuToGpu:
                    info.usage = VMA_MEMORY_USAGE_AUTO;
                    info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                    break;
                case BufferKind::Staging:
                    info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                    info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                    break;
                case BufferKind::Readback:
                    // Read by the CPU: needs cached (random access) host memory.
                    info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                    info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                    break;
            }
            return info;
        }
    }

    Buffer CreateBuffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, BufferKind kind, std::string_view debugName) {
        const VmaAllocator allocator = device.Allocator();
        if (allocator == VK_NULL_HANDLE) {
            Log::Error("CreateBuffer '{}': the device is not initialised", debugName);
            return {};
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage | TransferUsage(kind);
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (bufferInfo.size == 0 || bufferInfo.usage == 0) {
            Log::Error("CreateBuffer '{}': size ({}) and usage must not be zero", debugName, size);
            return {};
        }

        const VmaAllocationCreateInfo allocationInfo = AllocationInfo(kind);
        Buffer buffer;
        VmaAllocationInfo allocated{};
        const VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &buffer.handle, &buffer.allocation, &allocated);
        if (result != VK_SUCCESS) {
            Log::Error("CreateBuffer '{}': allocating {} bytes failed (VkResult {})", debugName, size, static_cast<int>(result));
            return {};
        }
        buffer.size = size;
        buffer.mapped = kind == BufferKind::GpuOnly ? nullptr : allocated.pMappedData;

        if (kind != BufferKind::GpuOnly && buffer.mapped == nullptr) {
            Log::Error("CreateBuffer '{}': the buffer could not be mapped", debugName);
            DestroyBuffer(device, buffer);
            return {};
        }

        if (!debugName.empty()) {
            const std::string name(debugName);
            vmaSetAllocationName(allocator, buffer.allocation, name.c_str());
            device.SetDebugName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer.handle), debugName);
        }
        return buffer;
    }

    void DestroyBuffer(Device& device, Buffer& buffer) {
        const VmaAllocator allocator = device.Allocator();
        if (allocator != VK_NULL_HANDLE && (buffer.handle != VK_NULL_HANDLE || buffer.allocation != VK_NULL_HANDLE)) {
            vmaDestroyBuffer(allocator, buffer.handle, buffer.allocation);
        }
        buffer = Buffer{};
    }

    Image CreateImage2D(Device& device, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, std::string_view debugName) {
        const VmaAllocator allocator = device.Allocator();
        if (allocator == VK_NULL_HANDLE || device.Handle() == VK_NULL_HANDLE) {
            Log::Error("CreateImage2D '{}': the device is not initialised", debugName);
            return {};
        }
        if (extent.width == 0 || extent.height == 0 || usage == 0 || format == VK_FORMAT_UNDEFINED) {
            Log::Error("CreateImage2D '{}': extent ({}x{}), usage and format must not be zero", debugName,
                       extent.width, extent.height);
            return {};
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = VkExtent3D{extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        Image image;
        const VkResult result = vmaCreateImage(allocator, &imageInfo, &allocationInfo, &image.handle, &image.allocation, nullptr);
        if (result != VK_SUCCESS) {
            Log::Error("CreateImage2D '{}': creating a {}x{} image of format {} failed (VkResult {})", debugName,
                       extent.width, extent.height, static_cast<int>(format), static_cast<int>(result));
            return {};
        }
        image.format = format;
        image.extent = extent;
        image.aspect = IsDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        image.layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.handle;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.components = VkComponentMapping{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                 VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        viewInfo.subresourceRange.aspectMask = image.aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        const VkResult viewResult = vkCreateImageView(device.Handle(), &viewInfo, nullptr, &image.view);
        if (viewResult != VK_SUCCESS) {
            Log::Error("CreateImage2D '{}': creating the image view failed (VkResult {})", debugName, static_cast<int>(viewResult));
            image.view = VK_NULL_HANDLE;
            DestroyImage(device, image);
            return {};
        }

        if (!debugName.empty()) {
            const std::string name(debugName);
            vmaSetAllocationName(allocator, image.allocation, name.c_str());
            device.SetDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image.handle), debugName);
            device.SetDebugName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(image.view), debugName);
        }
        return image;
    }

    void DestroyImage(Device& device, Image& image) {
        if (image.view != VK_NULL_HANDLE && device.Handle() != VK_NULL_HANDLE) {
            vkDestroyImageView(device.Handle(), image.view, nullptr);
        }
        const VmaAllocator allocator = device.Allocator();
        if (allocator != VK_NULL_HANDLE && (image.handle != VK_NULL_HANDLE || image.allocation != VK_NULL_HANDLE)) {
            vmaDestroyImage(allocator, image.handle, image.allocation);
        }
        image = Image{};
    }

    VkSampler CreateSampler(Device& device, VkFilter filter, VkSamplerAddressMode addressMode) {
        if (device.Handle() == VK_NULL_HANDLE) {
            Log::Error("CreateSampler: the device is not initialised");
            return VK_NULL_HANDLE;
        }

        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = addressMode;
        info.addressModeV = addressMode;
        info.addressModeW = addressMode;
        info.mipLodBias = 0.0f;
        info.anisotropyEnable = VK_FALSE;
        info.maxAnisotropy = 1.0f;
        info.compareEnable = VK_FALSE;
        info.compareOp = VK_COMPARE_OP_ALWAYS;
        info.minLod = 0.0f;
        info.maxLod = VK_LOD_CLAMP_NONE;
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;

        VkSampler sampler = VK_NULL_HANDLE;
        const VkResult result = vkCreateSampler(device.Handle(), &info, nullptr, &sampler);
        if (result != VK_SUCCESS) {
            Log::Error("CreateSampler failed (VkResult {})", static_cast<int>(result));
            return VK_NULL_HANDLE;
        }
        return sampler;
    }

    bool IsDepthFormat(VkFormat format) {
        switch (format) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return true;
            default:
                return false;
        }
    }

    void TransitionImage(VkCommandBuffer cmd, Image& image, VkImageLayout newLayout) {
        if (cmd == VK_NULL_HANDLE || image.handle == VK_NULL_HANDLE || image.layout == newLayout) return;
        if (newLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Log::Error("TransitionImage: an image cannot be transitioned to UNDEFINED");
            return;
        }
        TransitionImage(cmd, image.handle, image.aspect, image.layout, newLayout);
        image.layout = newLayout;
    }

    void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout, VkImageLayout newLayout) {
        if (cmd == VK_NULL_HANDLE || image == VK_NULL_HANDLE || oldLayout == newLayout) return;
        if (newLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            Log::Error("TransitionImage: an image cannot be transitioned to UNDEFINED");
            return;
        }

        const LayoutSync source = SyncForLayout(oldLayout, true);
        const LayoutSync destination = SyncForLayout(newLayout, false);

        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = source.stage;
        barrier.srcAccessMask = source.access;
        barrier.dstStageMask = destination.stage;
        barrier.dstAccessMask = destination.access;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
}
