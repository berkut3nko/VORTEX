module;

#include <vulkan/vulkan.h>

// The one translation unit that carries the VulkanMemoryAllocator implementation.
#include <vk_mem_alloc.h>

#include <VkBootstrap.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

module vortex.graphics;

import vortex.core;

namespace vortex::graphics {

    namespace {

        /// Validation layer / driver messages -> engine log. `userData` is the error counter.
        VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                              void* userData) {
            const char* message = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "(no message)";
            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                if (userData != nullptr) {
                    static_cast<std::atomic<uint32_t>*>(userData)->fetch_add(1, std::memory_order_relaxed);
                }
                Log::Error("Vulkan: {}", message);
            } else {
                Log::Warn("Vulkan: {}", message);
            }
            return VK_FALSE;   // never abort the call that triggered the message
        }

        std::string ToLower(std::string_view text) {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        /// @brief True if the Vulkan implementation itself (loader, drivers, implicit layers)
        /// provides an instance extension, i.e. it can be enabled without enabling a layer.
        bool ImplementationHasInstanceExtension(std::string_view name) {
            uint32_t count = 0;
            if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
                return false;
            }
            std::vector<VkExtensionProperties> extensions(count);
            const VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) return false;
            extensions.resize(count);
            return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
                return name == extension.extensionName;
            });
        }

        template <typename T>
        std::string DescribeError(const vkb::Result<T>& result) {
            std::string text = result.error().message();
            for (const std::string& reason : result.detailed_failure_reasons()) {
                text += "\n  ";
                text += reason;
            }
            return text;
        }
    }

    struct Device::Impl {
        vkb::Instance instance;
        vkb::Device device;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkQueue graphicsQueue = VK_NULL_HANDLE;
        uint32_t graphicsQueueFamily = 0;
        VmaAllocator allocator = VK_NULL_HANDLE;

        VkPhysicalDeviceProperties properties{};
        std::string gpuName;
        bool initialized = false;
        bool headless = true;
        bool validation = false;
        PFN_vkSetDebugUtilsObjectNameEXT setObjectName = nullptr;   ///< Null unless debug utils is enabled.

        /// Outlives Shutdown() on purpose: destruction-time validation errors are still counted.
        std::atomic<uint32_t> validationErrors{0};
    };

    Device::Device() : m_Impl(std::make_unique<Impl>()) {}

    Device::~Device() {
        Shutdown();
    }

    bool Device::Initialize(const DeviceConfig& config, Window* window) {
        Impl& impl = *m_Impl;
        if (impl.initialized) {
            Log::Error("Device::Initialize called on a device that is already initialised");
            return false;
        }

        // --- instance ---------------------------------------------------------------------
        auto systemInfo = vkb::SystemInfo::get_system_info();
        if (!systemInfo) {
            Log::Error("Vulkan is not available: {}", DescribeError(systemInfo));
            return false;
        }
        // vk-bootstrap also counts extensions that only a layer provides. That is fine together
        // with the validation layer; without it the extension must come from the implementation.
        const bool debugUtils = systemInfo.value().debug_utils_available;
        const bool validation = config.validation && systemInfo.value().validation_layers_available && debugUtils;
        if (config.validation && !validation) {
            Log::Warn("Vulkan validation was requested but the validation layer{} is not installed; continuing without it",
                      systemInfo.value().validation_layers_available ? "'s debug utils extension" : "");
        }

        vkb::InstanceBuilder instanceBuilder;
        instanceBuilder.set_app_name(config.appName)
            .set_engine_name("VORTEX")
            .require_api_version(1, 3, 0)
            // The surface extensions are taken from the window system (GLFW) below instead of
            // vk-bootstrap's own guess, so the builder itself always runs in headless mode.
            .set_headless(true);

        if (window != nullptr) {
            const std::vector<const char*> windowExtensions = Window::RequiredInstanceExtensions();
            if (windowExtensions.empty()) {
                Log::Error("The window system reports no Vulkan surface support");
                return false;
            }
            instanceBuilder.enable_extensions(windowExtensions);
        }

        if (validation) {
            // Setting a callback also makes vk-bootstrap enable VK_EXT_debug_utils.
            instanceBuilder.request_validation_layers(true)
                .set_debug_callback(DebugMessengerCallback)
                .set_debug_callback_user_data_pointer(&impl.validationErrors)
                .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
        }
        const bool debugUtilsEnabled =
            validation || ImplementationHasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (!validation && debugUtilsEnabled) {
            // Object names are still useful to capture tools (RenderDoc) without validation.
            instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        auto instanceResult = instanceBuilder.build();
        if (!instanceResult) {
            Log::Error("Failed to create the Vulkan instance: {}", DescribeError(instanceResult));
            return false;
        }
        impl.instance = instanceResult.value();
        impl.validation = validation;
        impl.headless = window == nullptr;
        if (debugUtilsEnabled) {
            impl.setObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetInstanceProcAddr(impl.instance.instance, "vkSetDebugUtilsObjectNameEXT"));
        }

        // --- surface ----------------------------------------------------------------------
        if (window != nullptr) {
            impl.surface = window->CreateSurface(impl.instance.instance);
            if (impl.surface == VK_NULL_HANDLE) {
                Shutdown();
                return false;
            }
        }

        // --- physical device --------------------------------------------------------------
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features13.shaderDemoteToHelperInvocation = VK_TRUE;   // GLSL `discard` in SPIR-V 1.6

        // The engine's depth layouts (DEPTH_ATTACHMENT_OPTIMAL / DEPTH_READ_ONLY_OPTIMAL) belong to
        // this feature, which every Vulkan 1.2+ implementation is required to support.
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.separateDepthStencilLayouts = VK_TRUE;

        vkb::PhysicalDeviceSelector selector(impl.instance, impl.surface);
        selector.set_minimum_version(1, 3)
            .set_required_features_12(features12)
            .set_required_features_13(features13)
            .require_present(window != nullptr)
            .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
            .allow_any_gpu_device_type(true);

        // Suitable devices, preferred type first.
        auto candidates = selector.select_devices();
        if (!candidates || candidates.value().empty()) {
            Log::Error("No suitable Vulkan 1.3 GPU found: {}",
                       candidates ? std::string("no devices") : DescribeError(candidates));
            Shutdown();
            return false;
        }

        size_t chosen = 0;
        if (!config.preferredGpu.empty()) {
            const std::string wanted = ToLower(config.preferredGpu);
            bool found = false;
            for (size_t i = 0; i < candidates.value().size(); ++i) {
                if (ToLower(candidates.value()[i].name).find(wanted) != std::string::npos) {
                    chosen = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                Log::Warn("Preferred GPU '{}' not found among the suitable devices; using '{}'",
                          config.preferredGpu, candidates.value()[0].name);
            }
        }
        const vkb::PhysicalDevice physicalDevice = candidates.value()[chosen];

        // --- logical device ---------------------------------------------------------------
        vkb::DeviceBuilder deviceBuilder(physicalDevice);
        auto deviceResult = deviceBuilder.build();
        if (!deviceResult) {
            Log::Error("Failed to create the Vulkan device on '{}': {}", physicalDevice.name, DescribeError(deviceResult));
            Shutdown();
            return false;
        }
        impl.device = deviceResult.value();
        impl.physical = physicalDevice.physical_device;
        impl.properties = physicalDevice.properties;
        impl.gpuName = physicalDevice.name;

        auto queue = impl.device.get_queue(vkb::QueueType::graphics);
        auto queueFamily = impl.device.get_queue_index(vkb::QueueType::graphics);
        if (!queue || !queueFamily) {
            Log::Error("'{}' has no graphics queue", impl.gpuName);
            Shutdown();
            return false;
        }
        impl.graphicsQueue = queue.value();
        impl.graphicsQueueFamily = queueFamily.value();

        if (impl.surface != VK_NULL_HANDLE) {
            // The engine presents from its single graphics queue.
            VkBool32 canPresent = VK_FALSE;
            const VkResult supportResult = vkGetPhysicalDeviceSurfaceSupportKHR(
                impl.physical, impl.graphicsQueueFamily, impl.surface, &canPresent);
            if (supportResult != VK_SUCCESS || canPresent != VK_TRUE) {
                Log::Error("The graphics queue family of '{}' cannot present to the window surface", impl.gpuName);
                Shutdown();
                return false;
            }
        }

        // --- allocator --------------------------------------------------------------------
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = impl.physical;
        allocatorInfo.device = impl.device.device;
        allocatorInfo.instance = impl.instance.instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        const VkResult allocatorResult = vmaCreateAllocator(&allocatorInfo, &impl.allocator);
        if (allocatorResult != VK_SUCCESS) {
            Log::Error("Failed to create the GPU memory allocator (VkResult {})", static_cast<int>(allocatorResult));
            impl.allocator = VK_NULL_HANDLE;
            Shutdown();
            return false;
        }

        impl.initialized = true;
        Log::Info("Vulkan device: {} (API {}.{}.{}){}{}", impl.gpuName,
                  VK_API_VERSION_MAJOR(impl.properties.apiVersion), VK_API_VERSION_MINOR(impl.properties.apiVersion),
                  VK_API_VERSION_PATCH(impl.properties.apiVersion),
                  impl.headless ? ", headless" : "", impl.validation ? ", validation on" : "");
        return true;
    }

    void Device::Shutdown() {
        Impl& impl = *m_Impl;

        if (impl.device.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(impl.device.device);
        }
        if (impl.allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(impl.allocator);
            impl.allocator = VK_NULL_HANDLE;
        }
        if (impl.device.device != VK_NULL_HANDLE) {
            vkb::destroy_device(impl.device);
        }
        impl.device = vkb::Device{};
        impl.graphicsQueue = VK_NULL_HANDLE;
        impl.graphicsQueueFamily = 0;
        impl.physical = VK_NULL_HANDLE;

        if (impl.instance.instance != VK_NULL_HANDLE) {
            if (impl.surface != VK_NULL_HANDLE) {
                vkb::destroy_surface(impl.instance, impl.surface);
            }
            vkb::destroy_instance(impl.instance);   // also destroys the debug messenger
        }
        impl.surface = VK_NULL_HANDLE;
        impl.instance = vkb::Instance{};

        impl.setObjectName = nullptr;
        impl.properties = VkPhysicalDeviceProperties{};
        impl.gpuName.clear();
        impl.initialized = false;
        impl.headless = true;
        impl.validation = false;
    }

    bool Device::IsInitialized() const { return m_Impl->initialized; }

    VkInstance Device::Instance() const { return m_Impl->instance.instance; }
    VkPhysicalDevice Device::Physical() const { return m_Impl->physical; }
    VkDevice Device::Handle() const { return m_Impl->device.device; }
    VkQueue Device::GraphicsQueue() const { return m_Impl->graphicsQueue; }
    uint32_t Device::GraphicsQueueFamily() const { return m_Impl->graphicsQueueFamily; }
    VmaAllocator Device::Allocator() const { return m_Impl->allocator; }
    VkSurfaceKHR Device::Surface() const { return m_Impl->surface; }

    bool Device::IsHeadless() const { return m_Impl->headless; }
    std::string_view Device::GpuName() const { return m_Impl->gpuName; }
    const VkPhysicalDeviceProperties& Device::Properties() const { return m_Impl->properties; }
    float Device::TimestampPeriod() const { return m_Impl->properties.limits.timestampPeriod; }

    bool Device::ValidationEnabled() const { return m_Impl->validation; }
    uint32_t Device::ValidationErrorCount() const {
        return m_Impl->validationErrors.load(std::memory_order_relaxed);
    }

    void Device::SetDebugName(VkObjectType type, uint64_t handle, std::string_view name) const {
        const Impl& impl = *m_Impl;
        if (impl.setObjectName == nullptr || impl.device.device == VK_NULL_HANDLE) return;
        if (handle == 0 || type == VK_OBJECT_TYPE_UNKNOWN || name.empty()) return;

        const std::string text(name);   // the API needs a null-terminated string
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = text.c_str();
        impl.setObjectName(impl.device.device, &info);
    }

    void Device::WaitIdle() const {
        if (m_Impl->device.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_Impl->device.device);
        }
    }
}
