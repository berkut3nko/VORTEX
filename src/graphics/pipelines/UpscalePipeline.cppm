module;

#include <vulkan/vulkan.h>
#include <vector>

export module vortex.graphics:upscalepipeline;

import vortex.memory;

namespace vortex::graphics {

    /**
     * @brief Pipeline for reconstructing high-res image from low-res inputs.
     */
    export class UpscalePipeline {
    public:
        UpscalePipeline();
        ~UpscalePipeline();

        void Initialize(VkDevice device, uint32_t framesInFlight);
        void Shutdown();
        
        /**
         * @brief Dispatches the reconstruction compute shader.
         * @param cmd Command Buffer.
         * @param frameIndex Frame index.
         * @param input Low resolution source image.
         * @param output High resolution target image.
         * @param lowResWidth Width of the input.
         * @param lowResHeight Height of the input.
         * @param highResWidth Width of the output.
         * @param highResHeight Height of the output.
         */
        void Dispatch(VkCommandBuffer cmd, 
                      uint32_t frameIndex,
                      const memory::AllocatedImage& input,
                      const memory::AllocatedImage& output,
                      uint32_t lowResWidth, uint32_t lowResHeight,
                      uint32_t highResWidth, uint32_t highResHeight);

        bool IsInitialized() const { return m_Pipeline != VK_NULL_HANDLE; }

    private:
        VkDevice m_Device{VK_NULL_HANDLE};
        VkPipeline m_Pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_PipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout m_DescriptorSetLayout{VK_NULL_HANDLE};
        VkDescriptorPool m_DescriptorPool{VK_NULL_HANDLE};
        VkSampler m_Sampler{VK_NULL_HANDLE};
        
        std::vector<VkDescriptorSet> m_DescriptorSets;
    };
}