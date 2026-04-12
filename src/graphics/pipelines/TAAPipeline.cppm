module;

#include <vulkan/vulkan.h>
#include <vector>

export module vortex.graphics:taapipeline;

import vortex.memory;

namespace vortex::graphics {

    /**
     * @brief Temporal Anti-Aliasing (TAA) Compute Pipeline.
     * @details Resolves the current frame using history and velocity vectors to smooth edges and reduce noise.
     */
    export class TAAPipeline {
    public:
        TAAPipeline();
        ~TAAPipeline();

        void Initialize(VkDevice device, uint32_t framesInFlight);
        void Shutdown();
        
        /**
         * @brief Dispatches the TAA compute shader.
         * @param cmd Command Buffer.
         * @param frameIndex Frame index for descriptors.
         * @param sampler Sampler to use for textures.
         * @param colorInput Current raw frame color.
         * @param historyInput Previous frame's resolved color.
         * @param velocityInput Motion vectors.
         * @param depthInput Depth buffer.
         * @param output Target image for resolved color.
         * @param width Output width.
         * @param height Output height.
         */
        void Dispatch(VkCommandBuffer cmd, 
                      uint32_t frameIndex,
                      VkSampler sampler, 
                      const memory::AllocatedImage& colorInput,
                      const memory::AllocatedImage& historyInput,
                      const memory::AllocatedImage& velocityInput,
                      const memory::AllocatedImage& depthInput,
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