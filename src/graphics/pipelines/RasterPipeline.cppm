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
     * @details Uses a full-screen triangle or cube bounding boxes to trigger the fragment shader raymarcher.
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
                        const memory::AllocatedBuffer& chunkBuffer);

        void Shutdown();
        
        /**
         * @brief Binds the pipeline and descriptor sets for the current frame.
         */
        void Bind(VkCommandBuffer cmd, uint32_t frameIndex);
        
        /**
         * @brief Updates descriptor sets with current buffer handles.
         */
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
    };
}