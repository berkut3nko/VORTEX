module;

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

module vortex.graphics;

import :taapipeline;
import :shader;
import vortex.log;
import vortex.memory;

namespace vortex::graphics {

    TAAPipeline::TAAPipeline() {}
    TAAPipeline::~TAAPipeline() { Shutdown(); }

    static std::string ReadFile(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return "";
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    void TAAPipeline::Initialize(VkDevice device) {
        m_Device = device;

        // Bindings:
        // 0: Color Input (Sampler)
        // 1: History Input (Sampler)
        // 2: Velocity Input (Sampler)
        // 3: Depth Input (Sampler)
        // 4: Output (Storage Image)
        std::vector<VkDescriptorSetLayoutBinding> bindings(5);
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = (uint32_t)bindings.size();
        layoutInfo.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

        VkPipelineLayoutCreateInfo pipeLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
        vkCreatePipelineLayout(m_Device, &pipeLayoutInfo, nullptr, &m_PipelineLayout);

        auto source = ReadFile("assets/shaders/taa.comp");
        if(source.empty()) { Log::Error("TAA shader missing!"); return; }

        auto spv = ShaderCompiler::Compile(ShaderStage::Compute, source);
        
        VkShaderModuleCreateInfo modInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        modInfo.codeSize = spv.size() * 4;
        modInfo.pCode = spv.data();
        VkShaderModule module;
        vkCreateShaderModule(m_Device, &modInfo, nullptr, &module);

        VkComputePipelineCreateInfo pipeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeInfo.layout = m_PipelineLayout;
        pipeInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        
        vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &m_Pipeline);
        vkDestroyShaderModule(m_Device, module, nullptr);

        // Pool
        VkDescriptorPoolSize poolSizes[] = { 
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool);

        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_DescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_DescriptorSetLayout;
        vkAllocateDescriptorSets(m_Device, &allocInfo, &m_DescriptorSet);
    }

    void TAAPipeline::Dispatch(VkCommandBuffer cmd, 
                      const memory::AllocatedImage& colorInput,
                      const memory::AllocatedImage& historyInput,
                      const memory::AllocatedImage& velocityInput,
                      const memory::AllocatedImage& depthInput,
                      const memory::AllocatedImage& output,
                      uint32_t width, uint32_t height) {

        // Use correct member names: imageView (not view) and sampler (now exists)
        VkDescriptorImageInfo colorInfo{colorInput.sampler ? colorInput.sampler : VK_NULL_HANDLE, colorInput.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo histInfo{historyInput.sampler ? historyInput.sampler : VK_NULL_HANDLE, historyInput.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo velInfo{velocityInput.sampler ? velocityInput.sampler : VK_NULL_HANDLE, velocityInput.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depthInfo{depthInput.sampler ? depthInput.sampler : VK_NULL_HANDLE, depthInput.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, output.imageView, VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet writes[5];
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &histInfo, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &velInfo, nullptr, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr};
        writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo, nullptr, nullptr};

        vkUpdateDescriptorSets(m_Device, 5, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
        
        vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    }

    void TAAPipeline::Shutdown() {
        if(m_Device) {
            vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
            vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);
            vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
            m_Device = VK_NULL_HANDLE;
        }
    }
}