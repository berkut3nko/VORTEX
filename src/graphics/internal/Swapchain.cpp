module;

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>

module vortex.graphics;

import :swapchain;
import vortex.log;

namespace vortex::graphics {

    Swapchain::~Swapchain() { Shutdown(); }

    bool Swapchain::Initialize(VulkanContext& context, VkSurfaceKHR surface, uint32_t width, uint32_t height) {
        m_Context = &context;
        m_Surface = surface;
        Create(width, height);
        return true;
    }

    void Swapchain::Create(uint32_t width, uint32_t height) {
        vkb::SwapchainBuilder swap_builder{m_Context->GetVkbDevice()};
        auto swap_ret = swap_builder
            .set_desired_extent(width, height)
            .set_old_swapchain(m_Swapchain)
            // .set_surface(m_Surface) // REMOVED: vkb::SwapchainBuilder infers surface from Device
            .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
            .build();

        if (!swap_ret) {
            Log::Error("Failed to create swapchain: " + swap_ret.error().message());
            return;
        }

        if (m_Swapchain != VK_NULL_HANDLE) {
            DestroyInternal(); // Clean up old views/target, keep swapchain handle for set_old_swapchain above logic
        }

        m_VkbSwapchain = swap_ret.value();
        m_Swapchain = m_VkbSwapchain.swapchain;
        m_Images = m_VkbSwapchain.get_images().value();
        m_ImageViews = m_VkbSwapchain.get_image_views().value();
        m_SwapchainExtent = m_VkbSwapchain.extent;
        m_SwapchainFormat = m_VkbSwapchain.image_format;

        // Create RayTracing Render Target
        m_RenderTarget = m_Context->GetAllocator()->CreateImage(
            m_SwapchainExtent.width, m_SwapchainExtent.height, 
            VK_FORMAT_R8G8B8A8_UNORM, 
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        );
    }

    void Swapchain::DestroyInternal() {
         // vkb::destroy_swapchain destroys views, but we manually destroy render target
         if (m_RenderTarget.image != VK_NULL_HANDLE) {
             m_Context->GetAllocator()->DestroyImage(m_RenderTarget);
             m_RenderTarget = {};
         }
         // Note: We don't destroy m_Swapchain here if we are recreating, vkb handles logic
    }

    void Swapchain::Recreate(uint32_t width, uint32_t height) {
        vkDeviceWaitIdle(m_Context->GetDevice());
        // In vkb, to recreate, we usually destroy the old wrapper but keep the handle for the builder?
        // Actually vkb::destroy_swapchain kills the handle too. 
        // Simplification: Full destroy then create.
        DestroyInternal();
        vkb::destroy_swapchain(m_VkbSwapchain);
        m_Swapchain = VK_NULL_HANDLE; 
        
        Create(width, height);
        Log::Info("Swapchain recreated: " + std::to_string(width) + "x" + std::to_string(height));
    }

    void Swapchain::Shutdown() {
        DestroyInternal();
        if (m_Swapchain) {
            vkb::destroy_swapchain(m_VkbSwapchain);
            m_Swapchain = VK_NULL_HANDLE;
        }
    }

    uint32_t Swapchain::AcquireNextImage(VkSemaphore semaphore) {
        uint32_t idx;
        VkResult res = vkAcquireNextImageKHR(m_Context->GetDevice(), m_Swapchain, UINT64_MAX, semaphore, VK_NULL_HANDLE, &idx);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) return UINT32_MAX;
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return UINT32_MAX;
        return idx;
    }

    VkResult Swapchain::Present(VkQueue queue, uint32_t imageIndex, VkSemaphore renderFinishedSemaphore) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinishedSemaphore;
        present.swapchainCount = 1;
        present.pSwapchains = &m_Swapchain;
        present.pImageIndices = &imageIndex;
        return vkQueuePresentKHR(queue, &present);
    }
}