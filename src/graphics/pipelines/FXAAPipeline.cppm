module;

#include <vulkan/vulkan.h>
#include <vector>

export module vortex.graphics:fxaapipeline;

import vortex.memory;

namespace vortex::graphics {

    export class FXAAPipeline {
    public:
        FXAAPipeline();
        ~FXAAPipeline();

        // Initialize compute pipeline
        void Initialize(VkDevice device, uint32_t framesInFlight);
        void Shutdown();
        
        // Compute Dispatch
        void Dispatch(VkCommandBuffer cmd, 
                      uint32_t frameIndex,
                      VkSampler sampler, // <--- ADDED
                      const memory::AllocatedImage& input,
                      const memory::AllocatedImage& output,
                      uint32_t width, uint32_t height);

        bool IsInitialized() const { return m_Pipeline != VK_NULL_HANDLE; }

    private:
        VkDevice m_Device{VK_NULL_HANDLE};
        VkPipeline m_Pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_PipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout m_DescriptorSetLayout{VK_NULL_HANDLE};
        VkDescriptorPool m_DescriptorPool{VK_NULL_HANDLE};
        
        std::vector<VkDescriptorSet> m_DescriptorSets;
    };
}