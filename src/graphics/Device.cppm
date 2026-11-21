module;

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

export module vortex.graphics:device;

import :window;

export namespace vortex::graphics {

    struct DeviceConfig {
        std::string appName = "VORTEX";
        bool validation = false;        ///< The Khronos validation layer: start-up fails without it (ValidationMissing()).
        std::string preferredGpu;       ///< Substring of the device name to prefer ("" = best available).
    };

    /// @brief What a validation request comes to (DecideValidation).
    struct ValidationDecision {
        bool enable = false;   ///< Create the instance with the validation layer and its messenger.
        bool fail = false;     ///< Requested, but the layer (or its debug utils extension) is missing: start-up fails.
    };
    /**
     * @brief The validation rule (WP1): a request without the layer fails start-up instead of
     * running without it, so an automated run that exits 0 under --validation proves the layer
     * ran (exit code 2, the same as a run with validation errors).
     */
    ValidationDecision DecideValidation(bool requested, bool layerAvailable, bool debugUtilsAvailable);

    /**
     * @brief Vulkan instance, physical + logical device, the graphics queue and the allocator.
     * @details Requires Vulkan 1.3 (dynamic rendering, synchronization2). With `window == nullptr`
     * the device is created headless: no surface, no swapchain extension requirement - used for
     * automated runs, screenshots and tests on machines without a display.
     *
     * Validation messages are routed to the engine log; errors are counted so an automated run
     * can fail when the renderer does anything invalid.
     */
    class Device {
    public:
        Device();
        ~Device();
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        bool Initialize(const DeviceConfig& config, Window* window);
        void Shutdown();
        bool IsInitialized() const;

        VkInstance Instance() const;
        VkPhysicalDevice Physical() const;
        VkDevice Handle() const;
        VkQueue GraphicsQueue() const;
        uint32_t GraphicsQueueFamily() const;
        VmaAllocator Allocator() const;
        /// @brief The window surface (VK_NULL_HANDLE when headless). Owned by the device.
        VkSurfaceKHR Surface() const;

        bool IsHeadless() const;
        std::string_view GpuName() const;
        const VkPhysicalDeviceProperties& Properties() const;
        /// @brief Nanoseconds per timestamp query tick.
        float TimestampPeriod() const;

        bool ValidationEnabled() const;
        /// @brief Initialize failed because validation was requested and the layer is not installed.
        bool ValidationMissing() const;
        /// @brief Number of validation errors reported since start-up.
        uint32_t ValidationErrorCount() const;

        /// @brief Names a Vulkan object for validation messages and RenderDoc (no-op without debug utils).
        void SetDebugName(VkObjectType type, uint64_t handle, std::string_view name) const;
        void WaitIdle() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
