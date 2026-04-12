module;

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector> 

export module vortex.graphics:pipeline;

import vortex.memory;
import :camera_struct;

namespace vortex::graphics {

    /**
     * @brief Manages the main rasterization pipeline for Voxel Raymarching.
     */
    export class RasterPipeline {
    public:
        RasterPipeline();
        ~RasterPipeline();

        /**
         * @brief Initializes the graphics pipeline, layout, and descriptors.
         */
        void Initialize(VkDevice device, 
                        VkFormat colorFormat,
                        VkFormat velocityFormat, 
                        VkFormat depthFormat,
                        uint32_t framesInFlight,
                        const memory::AllocatedBuffer& cameraBuffer,
                        const memory::AllocatedBuffer& materialBuffer,
                        const memory::AllocatedBuffer& objectBuffer,
                        const memory::AllocatedBuffer& chunkBuffer,
                        const memory::AllocatedBuffer& lightBuffer,
                        const memory::AllocatedBuffer& tlasBuffer); // Added TLAS buffer

        void Shutdown();
        
        void Bind(VkCommandBuffer cmd, uint32_t frameIndex);
        void UpdateDescriptors(uint32_t frameIndex);

    private:
        VkDevice m_Device{VK_NULL_HANDLE};
        VkPipeline m_Pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_PipelineLayout{VK_NULL_HANDLE};
        
        VkDescriptorSetLayout m_DescriptorSetLayout{VK_NULL_HANDLE};
        VkDescriptorPool m_DescriptorPool{VK_NULL_HANDLE};
        
        std::vector<VkDescriptorSet> m_DescriptorSets;

        VkBuffer m_CameraBuffer{VK_NULL_HANDLE};
        VkBuffer m_MaterialBuffer{VK_NULL_HANDLE};
        VkBuffer m_ObjectBuffer{VK_NULL_HANDLE};
        VkBuffer m_ChunkBuffer{VK_NULL_HANDLE};
        VkBuffer m_LightBuffer{VK_NULL_HANDLE};
        VkBuffer m_TLASBuffer{VK_NULL_HANDLE};
    };
}