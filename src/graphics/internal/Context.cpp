module;

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <memory> 

module vortex.graphics;

import :context;
import vortex.log;
import vortex.memory; 

namespace vortex::graphics {

    VulkanContext::~VulkanContext() { Shutdown(); }

    bool VulkanContext::InitInstance(const char* title) {
        vkb::InstanceBuilder builder;
        auto inst_ret = builder.set_app_name(title)
                               .request_validation_layers(false) 
                               .require_api_version(1, 3, 0)
                               .use_default_debug_messenger()
                               .build();
        if (!inst_ret) {
            Log::Error("Failed to create Vulkan Instance: " + inst_ret.error().message());
            return false;
        }
        m_Instance = inst_ret.value();
        return true;
    }

    bool VulkanContext::InitDevice(VkSurfaceKHR surface) {
        VkPhysicalDeviceVulkan13Features features13{};
        features13.dynamicRendering = VK_TRUE; 
        features13.synchronization2 = VK_TRUE;
        features13.maintenance4 = VK_TRUE;

        VkPhysicalDeviceVulkan12Features features12{};
        features12.scalarBlockLayout = VK_TRUE; 
        features12.bufferDeviceAddress = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{m_Instance};
        auto phys_ret = selector.set_surface(surface)
                                .set_minimum_version(1, 3)
                                .set_required_features_13(features13)
                                .set_required_features_12(features12)
                                .select();
        if (!phys_ret) {
            Log::Error("Failed to select Physical Device: " + phys_ret.error().message());
            return false;
        }

        vkb::DeviceBuilder dev_builder{phys_ret.value()};
        auto dev_ret = dev_builder.build();
        if (!dev_ret) {
             Log::Error("Failed to create Logical Device");
             return false;
        }
        m_Device = dev_ret.value();

        m_GraphicsQueue = m_Device.get_queue(vkb::QueueType::graphics).value();
        m_QueueFamily = m_Device.get_queue_index(vkb::QueueType::graphics).value();

        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.physicalDevice = m_Device.physical_device;
        allocatorInfo.device = m_Device.device;
        allocatorInfo.instance = m_Instance.instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        vmaCreateAllocator(&allocatorInfo, &m_VmaAllocator);
        m_MemoryAllocator = std::make_unique<memory::MemoryAllocator>(m_VmaAllocator, m_Device.device);

        Log::Info("Vulkan Context initialized.");
        return true;
    }

    void VulkanContext::Shutdown() {
        if (m_MemoryAllocator) {
            m_MemoryAllocator.reset();
            vmaDestroyAllocator(m_VmaAllocator);
            m_VmaAllocator = VK_NULL_HANDLE;
        }
        if (m_Device.device != VK_NULL_HANDLE) {
            vkb::destroy_device(m_Device);
            m_Device.device = VK_NULL_HANDLE;
        }
        if (m_Instance.instance != VK_NULL_HANDLE) {
            vkb::destroy_instance(m_Instance);
            m_Instance.instance = VK_NULL_HANDLE;
        }
    }
}