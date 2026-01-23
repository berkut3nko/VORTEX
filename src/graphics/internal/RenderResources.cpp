module;

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

module vortex.graphics;

import :render_resources;
import vortex.memory;

namespace vortex::graphics {

    RenderResources::~RenderResources() { Shutdown(); }

    void RenderResources::Initialize(memory::MemoryAllocator* allocator, uint32_t width, uint32_t height) {
        m_Allocator = allocator;
        if (width == 0) width = 1;
        if (height == 0) height = 1;

        // Create Sampler if not exists (Lazy for simplicity here, ideally passed from context)
        if (m_DefaultSampler == VK_NULL_HANDLE) {
            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            // Assuming device access via allocator or context, but for now strict separation:
            // In real code, pass Device to Initialize. Here we assume allocator wraps it or we skip sampler creation inside this specific class 
            // and pass sampler. For this refactor, let's assume images store the sampler passed later or created externally.
            // *Correction*: To keep it simple, we will reuse the sampler logic from Graphics.cpp or just create images.
        }

        // Helper for GPU-only images
        auto CreateGPUImage = [&](VkFormat fmt, VkImageUsageFlags usage) {
            auto img = m_Allocator->CreateImage(width, height, fmt, usage, VMA_MEMORY_USAGE_GPU_ONLY);
            // img.sampler = m_DefaultSampler; // Assign if we had it
            return img;
        };

        m_ColorTarget = CreateGPUImage(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        
        m_VelocityTarget = CreateGPUImage(VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        
        m_DepthTarget = CreateGPUImage(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        
        m_ResolveTarget = CreateGPUImage(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

        // TAA History (Ping-Pong)
        VkImageUsageFlags histUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        m_HistoryTexture[0] = CreateGPUImage(VK_FORMAT_R8G8B8A8_UNORM, histUsage);
        m_HistoryTexture[1] = CreateGPUImage(VK_FORMAT_R8G8B8A8_UNORM, histUsage);

        m_HistoryValid = false;
    }

    void RenderResources::Shutdown() {
        if (!m_Allocator) return;

        m_Allocator->DestroyImage(m_ColorTarget);
        m_Allocator->DestroyImage(m_VelocityTarget);
        m_Allocator->DestroyImage(m_DepthTarget);
        m_Allocator->DestroyImage(m_ResolveTarget);
        m_Allocator->DestroyImage(m_HistoryTexture[0]);
        m_Allocator->DestroyImage(m_HistoryTexture[1]);
    }
}