module;

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>

module vortex.graphics;

import :pipeline;
import :shader; 
import vortex.log;
import vortex.memory;

namespace vortex::graphics {

    RasterPipeline::RasterPipeline() {}
    RasterPipeline::~RasterPipeline() { Shutdown(); }

    static std::string ReadFile(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return "";
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    void RasterPipeline::Initialize(VkDevice device, 
                                    VkFormat colorFormat, 
                                    VkFormat velocityFormat, 
                                    VkFormat depthFormat,
                                    uint32_t framesInFlight, 
                                    const memory::AllocatedBuffer& cameraBuffer,
                                    const memory::AllocatedBuffer& materialBuffer,
                                    const memory::AllocatedBuffer& objectBuffer,
                                    const memory::AllocatedBuffer& chunkBuffer,
                                    const memory::AllocatedBuffer& lightBuffer) {
        m_Device = device;
        m_CameraBuffer = cameraBuffer.buffer;
        m_MaterialBuffer = materialBuffer.buffer;
        m_ObjectBuffer = objectBuffer.buffer;
        m_ChunkBuffer = chunkBuffer.buffer;
        m_LightBuffer = lightBuffer.buffer;

        // Increased bindings to 5
        std::vector<VkDescriptorSetLayoutBinding> bindings(5);
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        // Light Buffer Binding
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = (uint32_t)bindings.size();
        layoutInfo.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

        VkPipelineLayoutCreateInfo pipeLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
        vkCreatePipelineLayout(m_Device, &pipeLayoutInfo, nullptr, &m_PipelineLayout);

        auto vertSource = ReadFile("assets/shaders/voxel.vert");
        auto fragSource = ReadFile("assets/shaders/voxel.frag");

        auto vertSpv = ShaderCompiler::Compile(ShaderStage::Vertex, vertSource);
        auto fragSpv = ShaderCompiler::Compile(ShaderStage::Fragment, fragSource);

        VkShaderModule vertMod, fragMod;
        VkShaderModuleCreateInfo modInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        modInfo.codeSize = vertSpv.size() * 4;
        modInfo.pCode = vertSpv.data();
        vkCreateShaderModule(m_Device, &modInfo, nullptr, &vertMod);

        modInfo.codeSize = fragSpv.size() * 4;
        modInfo.pCode = fragSpv.data();
        vkCreateShaderModule(m_Device, &modInfo, nullptr, &fragMod);

        VkPipelineShaderStageCreateInfo stages[] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main", nullptr}
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT; 
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS; 
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState blendStates[2] = {};
        blendStates[0].colorWriteMask = 0xF; blendStates[0].blendEnable = VK_FALSE;
        blendStates[1].colorWriteMask = 0xF; blendStates[1].blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 2;
        colorBlending.pAttachments = blendStates;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkFormat colorFormats[] = { colorFormat, velocityFormat };
        VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount = 2;
        renderingInfo.pColorAttachmentFormats = colorFormats;
        renderingInfo.depthAttachmentFormat = depthFormat;

        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_PipelineLayout;
        
        vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline);

        vkDestroyShaderModule(m_Device, vertMod, nullptr);
        vkDestroyShaderModule(m_Device, fragMod, nullptr);

        // Descriptors
        VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * framesInFlight }, // Increased for light buffer
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * framesInFlight }
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = framesInFlight;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool);

        std::vector<VkDescriptorSetLayout> layouts(framesInFlight, m_DescriptorSetLayout);
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_DescriptorPool;
        allocInfo.descriptorSetCount = framesInFlight;
        allocInfo.pSetLayouts = layouts.data();
        
        m_DescriptorSets.resize(framesInFlight);
        vkAllocateDescriptorSets(m_Device, &allocInfo, m_DescriptorSets.data());

        for(uint32_t i=0; i<framesInFlight; i++) {
            UpdateDescriptors(i);
        }
        
        Log::Info("Raster Pipeline initialized.");
    }

    void RasterPipeline::UpdateDescriptors(uint32_t frameIndex) {
        VkDescriptorBufferInfo camInfo{m_CameraBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo matInfo{m_MaterialBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo objInfo{m_ObjectBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo chkInfo{m_ChunkBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo lightInfo{m_LightBuffer, 0, VK_WHOLE_SIZE};

        std::vector<VkWriteDescriptorSet> writes(5);
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSets[frameIndex], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &camInfo, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSets[frameIndex], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &matInfo, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSets[frameIndex], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objInfo, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSets[frameIndex], 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &chkInfo, nullptr};
        writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_DescriptorSets[frameIndex], 4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &lightInfo, nullptr};

        vkUpdateDescriptorSets(m_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }

    void RasterPipeline::Bind(VkCommandBuffer cmd, uint32_t frameIndex) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1, &m_DescriptorSets[frameIndex], 0, nullptr);
    }

    void RasterPipeline::Shutdown() {
        if (m_Device) {
            vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
            vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);
            vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
            m_Device = VK_NULL_HANDLE;
        }
    }
}