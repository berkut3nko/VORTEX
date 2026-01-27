module;

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <memory>

export module vortex.graphics:context;

import vortex.memory;

namespace vortex::graphics {

    /**
     * @brief Core Vulkan wrapper managing the Instance, Physical Device, and Logical Device.
     */
    export class VulkanContext {
    public:
        VulkanContext() = default;
        ~VulkanContext();

        /**
         * @brief Creates the Vulkan Instance.
         * @details This is the first step of initialization.
         * @param title The application name used for the instance.
         * @return True on success.
         */
        bool InitInstance(const char* title);
        
        /**
         * @brief Selects a Physical Device and creates the Logical Device.
         * @details Requires a valid surface to ensure the device supports presentation.
         * @param surface The window surface used for compatibility checks.
         * @return True on success.
         */
        bool InitDevice(VkSurfaceKHR surface);

        /**
         * @brief Destroys the Logical Device, Instance, and Allocator.
         */
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