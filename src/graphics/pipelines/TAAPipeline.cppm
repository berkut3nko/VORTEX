module;

#include <vulkan/vulkan.h>

export module vortex.graphics:taapipeline;

import vortex.memory;

namespace vortex::graphics {

    export class TAAPipeline {
    public:
        TAAPipeline();
        ~TAAPipeline();

        void Initialize(VkDevice device);
        void Shutdown();
        
        void Dispatch(VkCommandBuffer cmd, 
                      const memory::AllocatedImage& colorInput,
                      const memory::AllocatedImage& historyInput,
                      const memory::AllocatedImage& velocityInput,
                      const memory::AllocatedImage& depthInput,
                      const memory::AllocatedImage& output,
                      uint32_t width, uint32_t height);

    private:
        VkDevice m_Device{VK_NULL_HANDLE};
        VkPipeline m_Pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_PipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout m_DescriptorSetLayout{VK_NULL_HANDLE};
        VkDescriptorPool m_DescriptorPool{VK_NULL_HANDLE};
        VkDescriptorSet m_DescriptorSet{VK_NULL_HANDLE};
    };
}