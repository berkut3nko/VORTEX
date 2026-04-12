module;

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

module vortex.graphics;

import :fxaapipeline;
import :shader;
import vortex.log;
import vortex.memory;

namespace vortex::graphics {

    FXAAPipeline::FXAAPipeline() {}
    FXAAPipeline::~FXAAPipeline() { Shutdown(); }

    static std::string ReadFile(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return "";
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    void FXAAPipeline::Initialize(VkDevice device, uint32_t framesInFlight) {
        m_Device = device;

        // Binding 0: Input Texture (Sampler)
        // Binding 1: Output Texture (Storage Image)
        std::vector<VkDescriptorSetLayoutBinding> bindings(2);
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = (uint32_t)bindings.size();
        layoutInfo.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

        VkPipelineLayoutCreateInfo pipeLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
        vkCreatePipelineLayout(m_Device, &pipeLayoutInfo, nullptr, &m_PipelineLayout);

        auto source = ReadFile("assets/shaders/fxaa.comp");
        if(source.empty()) { Log::Error("FXAA shader missing!"); return; }

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
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 * framesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 * framesInFlight}
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = framesInFlight;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool);

        std::vector<VkDescriptorSetLayout> layouts(framesInFlight, m_DescriptorSetLayout);
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_DescriptorPool;
        allocInfo.descriptorSetCount = framesInFlight;
        allocInfo.pSetLayouts = layouts.data();
        
        m_DescriptorSets.resize(framesInFlight);
        vkAllocateDescriptorSets(m_Device, &allocInfo, m_DescriptorSets.data());
        
        Log::Info("FXAA Pipeline Initialized (Lazy Load)");
    }

    void FXAAPipeline::Dispatch(VkCommandBuffer cmd, 
                      uint32_t frameIndex,
                      VkSampler sampler, // <--- USED HERE
                      const memory::AllocatedImage& input,
                      const memory::AllocatedImage& output,
                      uint32_t width, uint32_t height) {

        // FIX: Use passed sampler, fallback to image's sampler if passed is null (rare), but ensure not null
        VkSampler samplerToUse = sampler ? sampler : input.sampler;
        if (samplerToUse == VK_NULL_HANDLE) {
             // Fallback or crash prevention, though caller should provide valid sampler
             // Assuming caller provides valid sampler from GraphicsContext
        }

        VkDescriptorImageInfo inInfo{samplerToUse, input.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, output.imageView, VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet writes[2];
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSets[frameIndex], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &inInfo, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSets[frameIndex], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo, nullptr, nullptr};

        vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSets[frameIndex], 0, nullptr);
        
        vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    }

    void FXAAPipeline::Shutdown() {
        if(m_Device) {
            if(m_Pipeline) vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
            if(m_PipelineLayout) vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
            if(m_DescriptorSetLayout) vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);
            if(m_DescriptorPool) vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
            m_Pipeline = VK_NULL_HANDLE; 
        }
    }
}