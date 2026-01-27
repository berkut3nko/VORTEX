module;

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector> 

export module vortex.graphics:pipeline;

import vortex.memory;
import :camera_struct; // Import CameraUBO from here

namespace vortex::graphics {

    // REMOVED local CameraUBO definition to fix redefinition error

    export class RasterPipeline {
    public:
        RasterPipeline();
        ~RasterPipeline();

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
    };
}