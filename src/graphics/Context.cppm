module;

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <memory>

export module vortex.graphics:context;

import vortex.memory;

namespace vortex::graphics {

    export class VulkanContext {
    public:
        VulkanContext() = default;
        ~VulkanContext();

        // Етап 1: Створення Instance (не потребує Surface)
        bool InitInstance(const char* title);
        
        // Етап 2: Вибір GPU та створення Device (потребує Surface)
        bool InitDevice(VkSurfaceKHR surface);

        void Shutdown();

        VkInstance GetInstance() const { return m_Instance.instance; }
        VkDevice GetDevice() const { return m_Device.device; }
        VkPhysicalDevice GetPhysicalDevice() const { return m_Device.physical_device; }
        VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
        uint32_t GetQueueFamily() const { return m_QueueFamily; }
        
        vkb::Instance& GetVkbInstance() { return m_Instance; }
        vkb::Device& GetVkbDevice() { return m_Device; }

        memory::MemoryAllocator* GetAllocator() { return m_MemoryAllocator.get(); }
        VmaAllocator GetVmaAllocator() { return m_VmaAllocator; }

    private:
        vkb::Instance m_Instance;
        vkb::Device m_Device;
        
        VkQueue m_GraphicsQueue{VK_NULL_HANDLE};
        uint32_t m_QueueFamily{0};

        VmaAllocator m_VmaAllocator{VK_NULL_HANDLE};
        std::unique_ptr<memory::MemoryAllocator> m_MemoryAllocator;
    };
}