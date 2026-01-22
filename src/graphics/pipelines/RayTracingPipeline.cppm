module;

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

export module vortex.graphics:pipeline;

import vortex.memory;

namespace vortex::graphics {

    /**
     * @brief GPU Camera structure (std140 layout).
     * @details Aligned manually to 16 bytes.
     */
    export struct CameraUBO {
        glm::mat4 viewInverse;
        glm::mat4 projInverse;
        glm::vec4 position;
        glm::vec4 direction;
        uint32_t objectCount;
        uint32_t _pad0;
        uint32_t _pad1;
        uint32_t _pad2; 
    };

    /**
     * @brief Manages the Compute Ray Tracing Pipeline.
     * @details Handles Shader compilation, Pipeline creation, and Descriptor Sets management.
     */
    export class RayTracingPipeline {
    public:
        RayTracingPipeline();
        ~RayTracingPipeline();

        /**
         * @brief Initializes the compute pipeline and descriptor sets.
         * @param device Vulkan Logical Device.
         * @param outputImage The storage image to write raytracing results to.
         * @param cameraBuffer UBO for camera data.
         * @param materialBuffer SSBO for material data.
         * @param objectBuffer SSBO for object transforms.
         * @param chunkBuffer SSBO for voxel data (massive buffer).
         */
        void Initialize(VkDevice device, 
                        const memory::AllocatedImage& outputImage,
                        const memory::AllocatedBuffer& cameraBuffer,
                        const memory::AllocatedBuffer& materialBuffer,
                        const memory::AllocatedBuffer& objectBuffer,
                        const memory::AllocatedBuffer& chunkBuffer);

        /**
         * @brief Cleans up pipeline resources.
         */
        void Shutdown();

        /**
         * @brief Dispatches the compute shader.
         * @param cmd Command Buffer to record into.
         * @param width Render target width.
         * @param height Render target height.
         */
        void Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height);
        
        /**
         * @brief Updates descriptor sets when the render target (swapchain) is resized.
         * @param outputImage New storage image.
         */
        void UpdateDescriptors(const memory::AllocatedImage& outputImage);

    private:
        VkDevice m_Device{VK_NULL_HANDLE};
        VkPipeline m_Pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_PipelineLayout{VK_NULL_HANDLE};
        
        VkDescriptorSetLayout m_DescriptorSetLayout{VK_NULL_HANDLE};
        VkDescriptorPool m_DescriptorPool{VK_NULL_HANDLE};
        VkDescriptorSet m_DescriptorSet{VK_NULL_HANDLE};

        // Cached buffer handles for efficient descriptor updates
        VkBuffer m_CameraBuffer{VK_NULL_HANDLE};
        VkBuffer m_MaterialBuffer{VK_NULL_HANDLE};
        VkBuffer m_ObjectBuffer{VK_NULL_HANDLE};
        VkBuffer m_ChunkBuffer{VK_NULL_HANDLE};
    };
}