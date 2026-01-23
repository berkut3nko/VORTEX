module;

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector> // Додано для std::vector

export module vortex.graphics:pipeline;

import vortex.memory;

namespace vortex::graphics {

    export struct CameraUBO {
        glm::mat4 viewInverse;
        glm::mat4 projInverse;
        glm::vec4 position;
        glm::vec4 direction;
        uint32_t objectCount;
        float _pad[3]; 
        glm::mat4 viewProj; 
        glm::mat4 prevViewProj; 
        glm::vec4 jitter;       
    };

    export class RasterPipeline {
    public:
        RasterPipeline();
        ~RasterPipeline();

        // Додано параметр framesInFlight
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
        
        // Додано frameIndex
        void Bind(VkCommandBuffer cmd, uint32_t frameIndex);
        void UpdateDescriptors(uint32_t frameIndex);

    private:
        VkDevice m_Device{VK_NULL_HANDLE};
        VkPipeline m_Pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_PipelineLayout{VK_NULL_HANDLE};
        
        VkDescriptorSetLayout m_DescriptorSetLayout{VK_NULL_HANDLE};
        VkDescriptorPool m_DescriptorPool{VK_NULL_HANDLE};
        
        // Змінено на вектор
        std::vector<VkDescriptorSet> m_DescriptorSets;

        VkBuffer m_CameraBuffer{VK_NULL_HANDLE};
        VkBuffer m_MaterialBuffer{VK_NULL_HANDLE};
        VkBuffer m_ObjectBuffer{VK_NULL_HANDLE};
        VkBuffer m_ChunkBuffer{VK_NULL_HANDLE};
    };
}