module;

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>

module vortex.graphics;

import :pipeline;
import :shader; // ShaderCompiler
import vortex.log;
import vortex.memory;

namespace vortex::graphics {

    RayTracingPipeline::RayTracingPipeline() {}
    RayTracingPipeline::~RayTracingPipeline() { Shutdown(); }

    static std::string ReadFile(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return "";
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    void RayTracingPipeline::Initialize(VkDevice device, 
                                        const memory::AllocatedImage& outputImage,
                                        const memory::AllocatedBuffer& cameraBuffer,
                                        const memory::AllocatedBuffer& materialBuffer,
                                        const memory::AllocatedBuffer& objectBuffer,
                                        const memory::AllocatedBuffer& chunkBuffer) {
        m_Device = device;
        // Cache buffer handles for Re-binding
        m_CameraBuffer = cameraBuffer.buffer;
        m_MaterialBuffer = materialBuffer.buffer;
        m_ObjectBuffer = objectBuffer.buffer;
        m_ChunkBuffer = chunkBuffer.buffer;

        // 1. Descriptor Set Layout
        // Binding 0: Output Image (Storage Image)
        // Binding 1: Camera Data (Uniform Buffer)
        // Binding 2: Materials (Storage Buffer)
        // Binding 3: Objects (Storage Buffer)
        // Binding 4: Chunks/Voxels (Storage Buffer)
        std::vector<VkDescriptorSetLayoutBinding> bindings(5);
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = (uint32_t)bindings.size();
        layoutInfo.pBindings = bindings.data();
        
        if (vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create descriptor set layout");
        }

        // 2. Pipeline Layout
        VkPipelineLayoutCreateInfo pipeLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
        
        if (vkCreatePipelineLayout(m_Device, &pipeLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
             throw std::runtime_error("Failed to create pipeline layout");
        }

        // 3. Shader Compilation & Pipeline Creation
        std::string source = ReadFile("raytracing.comp");
        if (source.empty()) {
            // Fallback path check
            source = ReadFile("assets/shaders/raytracing.comp");
        }
        
        if (source.empty()) {
            Log::Error("Failed to load shader: raytracing.comp");
            throw std::runtime_error("Shader file missing");
        }

        // Compile GLSL to SPIR-V at runtime
        auto spv = ShaderCompiler::Compile(ShaderStage::Compute, source);
        
        VkShaderModuleCreateInfo modInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        modInfo.codeSize = spv.size() * 4;
        modInfo.pCode = spv.data();
        
        VkShaderModule module;
        if (vkCreateShaderModule(m_Device, &modInfo, nullptr, &module) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create shader module");
        }

        VkPipelineShaderStageCreateInfo stageInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = module;
        stageInfo.pName = "main";

        VkComputePipelineCreateInfo pipeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeInfo.layout = m_PipelineLayout;
        pipeInfo.stage = stageInfo;
        
        if (vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &m_Pipeline) != VK_SUCCESS) {
            vkDestroyShaderModule(m_Device, module, nullptr);
            throw std::runtime_error("Failed to create compute pipeline");
        }
        vkDestroyShaderModule(m_Device, module, nullptr);

        // 4. Descriptor Pool
        VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 }
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes = sizes;
        
        if (vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create descriptor pool");
        }

        // 5. Allocate Descriptor Set
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_DescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_DescriptorSetLayout;
        
        if (vkAllocateDescriptorSets(m_Device, &allocInfo, &m_DescriptorSet) != VK_SUCCESS) {
             throw std::runtime_error("Failed to allocate descriptor sets");
        }

        // Initial write
        UpdateDescriptors(outputImage);
        
        Log::Info("RayTracing Pipeline initialized.");
    }

    void RayTracingPipeline::UpdateDescriptors(const memory::AllocatedImage& outputImage) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageView = outputImage.imageView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo camInfo{m_CameraBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo matInfo{m_MaterialBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo objInfo{m_ObjectBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo chkInfo{m_ChunkBuffer, 0, VK_WHOLE_SIZE};

        std::vector<VkWriteDescriptorSet> writes(5);
        
        // Binding 0: Output Image
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imgInfo, nullptr, nullptr};
        // Binding 1: Camera UBO
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &camInfo, nullptr};
        // Binding 2: Materials SSBO
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &matInfo, nullptr};
        // Binding 3: Objects SSBO
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objInfo, nullptr};
        // Binding 4: Chunks SSBO
        writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &chkInfo, nullptr};

        vkUpdateDescriptorSets(m_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }

    void RayTracingPipeline::Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
        
        // Calculate workgroups (8x8 local size)
        vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    }

    void RayTracingPipeline::Shutdown() {
        if (m_Device) {
            vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
            vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);
            vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
            m_Device = VK_NULL_HANDLE;
            Log::Info("RayTracing Pipeline destroyed.");
        }
    }
}