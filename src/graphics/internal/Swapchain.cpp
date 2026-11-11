module;

#include <vulkan/vulkan.h>

#include <VkBootstrap.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

module vortex.graphics;

import vortex.core;

namespace vortex::graphics {

    namespace {

        constexpr VkImageUsageFlags kSwapchainUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        /// Colour formats we accept, best first. Never an _SRGB format: the present pass encodes itself.
        constexpr VkSurfaceFormatKHR kSurfaceFormats[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };

        void DestroyViews(VkDevice device, std::vector<VkImageView>& views) {
            for (VkImageView view : views) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
            }
            views.clear();
        }

        /// @brief Picks the first of kSurfaceFormats the surface supports.
        bool ChooseSurfaceFormat(VkPhysicalDevice physical, VkSurfaceKHR surface, VkSurfaceFormatKHR& outFormat) {
            uint32_t count = 0;
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr) != VK_SUCCESS || count == 0) {
                return false;
            }
            std::vector<VkSurfaceFormatKHR> available(count);
            const VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, available.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) return false;
            available.resize(count);

            for (const VkSurfaceFormatKHR& wanted : kSurfaceFormats) {
                for (const VkSurfaceFormatKHR& format : available) {
                    if (format.format == wanted.format && format.colorSpace == wanted.colorSpace) {
                        outFormat = wanted;
                        return true;
                    }
                }
            }
            return false;
        }

        VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
            constexpr VkCompositeAlphaFlagBitsKHR kOrder[] = {
                VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            };
            for (VkCompositeAlphaFlagBitsKHR mode : kOrder) {
                if ((supported & mode) != 0) return mode;
            }
            return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        }
    }

    Swapchain::~Swapchain() {
        Destroy();
    }

    bool Swapchain::Create(Device& device, uint32_t width, uint32_t height, bool vsync) {
        if (!device.IsInitialized() || device.Surface() == VK_NULL_HANDLE) {
            Log::Error("Swapchain::Create needs an initialised device with a window surface");
            return false;
        }
        if (m_Device != nullptr && m_Device != &device) {
            // The existing swapchain belongs to another device and cannot be handed over.
            Destroy();
        }
        m_Device = &device;
        return Recreate(width, height, vsync);
    }

    bool Swapchain::Recreate(uint32_t width, uint32_t height, bool vsync) {
        if (m_Device == nullptr || !m_Device->IsInitialized() || m_Device->Surface() == VK_NULL_HANDLE) {
            Log::Error("Swapchain::Recreate called without a device (Create has to succeed first)");
            return false;
        }
        const VkDevice device = m_Device->Handle();
        const VkPhysicalDevice physical = m_Device->Physical();
        const VkSurfaceKHR surface = m_Device->Surface();

        // Nothing may still use the old images / views when they are destroyed below.
        m_Device->WaitIdle();

        // --- everything that can fail without touching the current swapchain --------------
        if (width == 0 || height == 0) {
            Log::Debug("Swapchain not (re)created: requested extent is {}x{}", width, height);
            return false;
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        const VkResult capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities);
        if (capsResult != VK_SUCCESS) {
            Log::Error("Failed to query the surface capabilities (VkResult {})", static_cast<int>(capsResult));
            return false;
        }

        // Same rule vk-bootstrap applies: a defined currentExtent is mandatory, otherwise clamp.
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<uint32_t>::max()) {
            extent.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) {
            Log::Debug("Swapchain not (re)created: the surface is zero-sized (window minimised)");
            return false;
        }

        VkSurfaceFormatKHR surfaceFormat{};
        if (!ChooseSurfaceFormat(physical, surface, surfaceFormat)) {
            Log::Error("The window surface offers neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM (sRGB non-linear)");
            return false;
        }

        // --- build ------------------------------------------------------------------------
        const uint32_t queueFamily = m_Device->GraphicsQueueFamily();
        vkb::SwapchainBuilder builder(physical, device, surface, queueFamily, queueFamily);
        builder.set_desired_format(surfaceFormat)
            .set_desired_extent(extent.width, extent.height)
            .set_image_usage_flags(kSwapchainUsage)
            .set_composite_alpha_flags(ChooseCompositeAlpha(capabilities.supportedCompositeAlpha))
            .set_old_swapchain(m_Swapchain);
        for (const VkSurfaceFormatKHR& fallback : kSurfaceFormats) {
            if (fallback.format != surfaceFormat.format) builder.add_fallback_format(fallback);
        }
        if (vsync) {
            builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
        } else {
            // vk-bootstrap falls back to FIFO (always available) when neither is supported.
            builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
                .add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR);
        }

        auto built = builder.build();

        // The old swapchain is retired by the creation attempt whether it succeeded or not.
        DestroyViews(device, m_Views);
        m_Images.clear();
        if (m_Swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;
        }
        m_Format = VK_FORMAT_UNDEFINED;
        m_Extent = VkExtent2D{0, 0};

        if (!built) {
            Log::Error("Failed to create the swapchain ({}x{}): {} (VkResult {})", extent.width, extent.height,
                       built.error().message(), static_cast<int>(built.vk_result()));
            return false;
        }

        vkb::Swapchain swapchain = built.value();
        const VkFormat format = swapchain.image_format;
        const bool formatOk = std::any_of(std::begin(kSurfaceFormats), std::end(kSurfaceFormats),
                                          [format](const VkSurfaceFormatKHR& f) { return f.format == format; });
        auto images = swapchain.get_images();
        if (!formatOk || !images || images.value().empty() || swapchain.extent.width == 0 || swapchain.extent.height == 0) {
            Log::Error("The created swapchain is unusable (format {}, {}x{})", static_cast<int>(format),
                       swapchain.extent.width, swapchain.extent.height);
            vkDestroySwapchainKHR(device, swapchain.swapchain, nullptr);
            return false;
        }

        std::vector<VkImageView> views;
        views.reserve(images.value().size());
        for (VkImage image : images.value()) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.components = VkComponentMapping{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            VkImageView view = VK_NULL_HANDLE;
            const VkResult viewResult = vkCreateImageView(device, &viewInfo, nullptr, &view);
            if (viewResult != VK_SUCCESS) {
                Log::Error("Failed to create a swapchain image view (VkResult {})", static_cast<int>(viewResult));
                DestroyViews(device, views);
                vkDestroySwapchainKHR(device, swapchain.swapchain, nullptr);
                return false;
            }
            views.push_back(view);
        }

        m_Swapchain = swapchain.swapchain;
        m_Format = format;
        m_Extent = swapchain.extent;
        m_Images = std::move(images.value());
        m_Views = std::move(views);

        for (size_t i = 0; i < m_Images.size(); ++i) {
            const std::string name = "Swapchain image " + std::to_string(i);
            m_Device->SetDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(m_Images[i]), name);
            m_Device->SetDebugName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(m_Views[i]), name);
        }

        Log::Info("Swapchain: {}x{}, {} images, format {}, present mode {}", m_Extent.width, m_Extent.height,
                  m_Images.size(), static_cast<int>(m_Format), static_cast<int>(swapchain.present_mode));
        return true;
    }

    void Swapchain::Destroy() {
        if (m_Device != nullptr && m_Device->Handle() != VK_NULL_HANDLE) {
            const VkDevice device = m_Device->Handle();
            if (m_Swapchain != VK_NULL_HANDLE || !m_Views.empty()) {
                m_Device->WaitIdle();
            }
            DestroyViews(device, m_Views);
            if (m_Swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
            }
        }
        // If the device is already gone its objects went with it; only forget the handles.
        m_Views.clear();
        m_Images.clear();
        m_Swapchain = VK_NULL_HANDLE;
        m_Format = VK_FORMAT_UNDEFINED;
        m_Extent = VkExtent2D{0, 0};
        m_Device = nullptr;
    }

    bool Swapchain::Acquire(VkSemaphore signal, uint32_t& outIndex) {
        if (m_Device == nullptr || m_Swapchain == VK_NULL_HANDLE) return false;

        uint32_t index = 0;
        const VkResult result = vkAcquireNextImageKHR(m_Device->Handle(), m_Swapchain,
                                                      std::numeric_limits<uint64_t>::max(), signal, VK_NULL_HANDLE, &index);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            // Suboptimal still acquired an image and signals the semaphore: render this frame,
            // Present() reports that the swapchain should be recreated.
            outIndex = index;
            return true;
        }
        if (result != VK_ERROR_OUT_OF_DATE_KHR) {
            Log::Error("Failed to acquire a swapchain image (VkResult {})", static_cast<int>(result));
        }
        return false;
    }

    bool Swapchain::Present(VkSemaphore wait, uint32_t index) {
        if (m_Device == nullptr || m_Swapchain == VK_NULL_HANDLE || index >= m_Images.size()) return false;

        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1u : 0u;
        info.pWaitSemaphores = wait != VK_NULL_HANDLE ? &wait : nullptr;
        info.swapchainCount = 1;
        info.pSwapchains = &m_Swapchain;
        info.pImageIndices = &index;

        const VkResult result = vkQueuePresentKHR(m_Device->GraphicsQueue(), &info);
        if (result == VK_SUCCESS) return true;
        if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR) {
            Log::Error("Failed to present (VkResult {})", static_cast<int>(result));
        }
        return false;
    }
}
