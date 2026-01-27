module;

#include <vulkan/vulkan.h>
#include <vector>

export module vortex.graphics:fxaapipeline;

import vortex.memory;

namespace vortex::graphics {

    /**
     * @brief Fast Approximate Anti-Aliasing (FXAA) Compute Pipeline.
     * @details A post-process effect that smooths edges based on contrast. Cheaper than TAA but less temporal stability.
     */
    export class FXAAPipeline {
    public:
        FXAAPipeline();
        ~FXAAPipeline();

        void Initialize(VkDevice device, uint32_t framesInFlight);
        void Shutdown();
        
        /**
         * @brief Dispatches the FXAA compute shader.
         * @param cmd Command Buffer.
         * @param frameIndex Frame index.
         * @param sampler Sampler for input texture.
         * @param input Source color image.
         * @param output Target storage image.
         * @param width Image width.
         * @param height Image height.
         */
        void Dispatch(VkCommandBuffer cmd, 
                      uint32_t frameIndex,
                      VkSampler sampler, 
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