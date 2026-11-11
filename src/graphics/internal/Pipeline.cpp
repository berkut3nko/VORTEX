module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module vortex.graphics;

import vortex.core;

namespace vortex::graphics {

    namespace {

        // -----------------------------------------------------------------------------------------
        //  Formats
        // -----------------------------------------------------------------------------------------
        struct ColorFormatInfo {
            VkFormat format;
            OutputBaseType base;       ///< Numeric class a fragment output must have to be written to it.
            uint32_t components;
            std::string_view name;
        };

        // Every colour format the renderer uses (ARCHITECTURE.md 5.3, swapchain formats) plus the
        // other common attachment formats. UNORM / SNORM / SRGB / SFLOAT formats take float outputs.
        constexpr std::array kColorFormats = {
            ColorFormatInfo{VK_FORMAT_R8_UNORM, OutputBaseType::Float, 1, "R8_UNORM"},
            ColorFormatInfo{VK_FORMAT_R8_SNORM, OutputBaseType::Float, 1, "R8_SNORM"},
            ColorFormatInfo{VK_FORMAT_R8G8_UNORM, OutputBaseType::Float, 2, "R8G8_UNORM"},
            ColorFormatInfo{VK_FORMAT_R8G8_SNORM, OutputBaseType::Float, 2, "R8G8_SNORM"},
            ColorFormatInfo{VK_FORMAT_R8G8B8A8_UNORM, OutputBaseType::Float, 4, "R8G8B8A8_UNORM"},
            ColorFormatInfo{VK_FORMAT_R8G8B8A8_SNORM, OutputBaseType::Float, 4, "R8G8B8A8_SNORM"},
            ColorFormatInfo{VK_FORMAT_R8G8B8A8_SRGB, OutputBaseType::Float, 4, "R8G8B8A8_SRGB"},
            ColorFormatInfo{VK_FORMAT_B8G8R8A8_UNORM, OutputBaseType::Float, 4, "B8G8R8A8_UNORM"},
            ColorFormatInfo{VK_FORMAT_B8G8R8A8_SRGB, OutputBaseType::Float, 4, "B8G8R8A8_SRGB"},
            ColorFormatInfo{VK_FORMAT_A8B8G8R8_UNORM_PACK32, OutputBaseType::Float, 4, "A8B8G8R8_UNORM_PACK32"},
            ColorFormatInfo{VK_FORMAT_A8B8G8R8_SRGB_PACK32, OutputBaseType::Float, 4, "A8B8G8R8_SRGB_PACK32"},
            ColorFormatInfo{VK_FORMAT_A2B10G10R10_UNORM_PACK32, OutputBaseType::Float, 4, "A2B10G10R10_UNORM_PACK32"},
            ColorFormatInfo{VK_FORMAT_A2R10G10B10_UNORM_PACK32, OutputBaseType::Float, 4, "A2R10G10B10_UNORM_PACK32"},
            ColorFormatInfo{VK_FORMAT_B10G11R11_UFLOAT_PACK32, OutputBaseType::Float, 3, "B10G11R11_UFLOAT_PACK32"},
            ColorFormatInfo{VK_FORMAT_R16_UNORM, OutputBaseType::Float, 1, "R16_UNORM"},
            ColorFormatInfo{VK_FORMAT_R16G16_UNORM, OutputBaseType::Float, 2, "R16G16_UNORM"},
            ColorFormatInfo{VK_FORMAT_R16G16B16A16_UNORM, OutputBaseType::Float, 4, "R16G16B16A16_UNORM"},
            ColorFormatInfo{VK_FORMAT_R16_SFLOAT, OutputBaseType::Float, 1, "R16_SFLOAT"},
            ColorFormatInfo{VK_FORMAT_R16G16_SFLOAT, OutputBaseType::Float, 2, "R16G16_SFLOAT"},
            ColorFormatInfo{VK_FORMAT_R16G16B16A16_SFLOAT, OutputBaseType::Float, 4, "R16G16B16A16_SFLOAT"},
            ColorFormatInfo{VK_FORMAT_R32_SFLOAT, OutputBaseType::Float, 1, "R32_SFLOAT"},
            ColorFormatInfo{VK_FORMAT_R32G32_SFLOAT, OutputBaseType::Float, 2, "R32G32_SFLOAT"},
            ColorFormatInfo{VK_FORMAT_R32G32B32A32_SFLOAT, OutputBaseType::Float, 4, "R32G32B32A32_SFLOAT"},

            ColorFormatInfo{VK_FORMAT_R8_UINT, OutputBaseType::UInt, 1, "R8_UINT"},
            ColorFormatInfo{VK_FORMAT_R8G8_UINT, OutputBaseType::UInt, 2, "R8G8_UINT"},
            ColorFormatInfo{VK_FORMAT_R8G8B8A8_UINT, OutputBaseType::UInt, 4, "R8G8B8A8_UINT"},
            ColorFormatInfo{VK_FORMAT_A2B10G10R10_UINT_PACK32, OutputBaseType::UInt, 4, "A2B10G10R10_UINT_PACK32"},
            ColorFormatInfo{VK_FORMAT_R16_UINT, OutputBaseType::UInt, 1, "R16_UINT"},
            ColorFormatInfo{VK_FORMAT_R16G16_UINT, OutputBaseType::UInt, 2, "R16G16_UINT"},
            ColorFormatInfo{VK_FORMAT_R16G16B16A16_UINT, OutputBaseType::UInt, 4, "R16G16B16A16_UINT"},
            ColorFormatInfo{VK_FORMAT_R32_UINT, OutputBaseType::UInt, 1, "R32_UINT"},
            ColorFormatInfo{VK_FORMAT_R32G32_UINT, OutputBaseType::UInt, 2, "R32G32_UINT"},
            ColorFormatInfo{VK_FORMAT_R32G32B32A32_UINT, OutputBaseType::UInt, 4, "R32G32B32A32_UINT"},

            ColorFormatInfo{VK_FORMAT_R8_SINT, OutputBaseType::Int, 1, "R8_SINT"},
            ColorFormatInfo{VK_FORMAT_R8G8_SINT, OutputBaseType::Int, 2, "R8G8_SINT"},
            ColorFormatInfo{VK_FORMAT_R8G8B8A8_SINT, OutputBaseType::Int, 4, "R8G8B8A8_SINT"},
            ColorFormatInfo{VK_FORMAT_R16_SINT, OutputBaseType::Int, 1, "R16_SINT"},
            ColorFormatInfo{VK_FORMAT_R16G16_SINT, OutputBaseType::Int, 2, "R16G16_SINT"},
            ColorFormatInfo{VK_FORMAT_R16G16B16A16_SINT, OutputBaseType::Int, 4, "R16G16B16A16_SINT"},
            ColorFormatInfo{VK_FORMAT_R32_SINT, OutputBaseType::Int, 1, "R32_SINT"},
            ColorFormatInfo{VK_FORMAT_R32G32_SINT, OutputBaseType::Int, 2, "R32G32_SINT"},
            ColorFormatInfo{VK_FORMAT_R32G32B32A32_SINT, OutputBaseType::Int, 4, "R32G32B32A32_SINT"},
        };

        const ColorFormatInfo* FindColorFormat(VkFormat format) {
            for (const ColorFormatInfo& info : kColorFormats) {
                if (info.format == format) return &info;
            }
            return nullptr;
        }

        std::string_view BaseTypeName(OutputBaseType base) {
            switch (base) {
                case OutputBaseType::Float: return "float";
                case OutputBaseType::Int: return "int";
                case OutputBaseType::UInt: return "uint";
            }
            return "?";
        }

        std::string_view BindingTypeName(BindingType type) {
            switch (type) {
                case BindingType::UniformBuffer: return "uniform buffer";
                case BindingType::StorageBuffer: return "storage buffer";
                case BindingType::CombinedImageSampler: return "combined image sampler";
                case BindingType::StorageImage: return "storage image";
            }
            return "?";
        }

        VkDescriptorType ToDescriptorType(BindingType type) {
            switch (type) {
                case BindingType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                case BindingType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                case BindingType::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                case BindingType::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            }
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }

        /// Vulkan handles are pointers on 64-bit targets and integers elsewhere.
        template <typename T>
        uint64_t HandleValue(T handle) {
            if constexpr (std::is_pointer_v<T>) {
                return reinterpret_cast<uint64_t>(handle);
            } else {
                return static_cast<uint64_t>(handle);
            }
        }

        // -----------------------------------------------------------------------------------------
        //  Shader stages
        // -----------------------------------------------------------------------------------------
        struct StageSource {
            std::vector<uint32_t> code;
            ShaderReflection reflection;
        };

        bool LoadStage(std::string_view pipeline, std::string_view file, VkShaderStageFlagBits expectedStage,
                       std::string_view stageName, StageSource& out) {
            if (file.empty()) {
                Log::Error("Pipeline '{}': no {} shader file given", pipeline, stageName);
                return false;
            }
            const std::filesystem::path path = Paths::Asset("shaders") / std::filesystem::path(file);
            out.code = LoadSpirvFile(path);
            if (out.code.empty()) {
                Log::Error("Pipeline '{}': cannot load {} shader '{}' (missing or not a SPIR-V file)",
                           pipeline, stageName, path.string());
                return false;
            }
            std::string error;
            if (!ReflectSpirv(out.code, out.reflection, &error)) {
                Log::Error("Pipeline '{}': cannot reflect {} shader '{}': {}", pipeline, stageName, path.string(), error);
                return false;
            }
            if (out.reflection.stage != expectedStage) {
                Log::Error("Pipeline '{}': '{}' is not a {} shader", pipeline, path.string(), stageName);
                return false;
            }
            return true;
        }

        /// Union of the bindings of both stages, sorted by binding number. Fails on anything that
        /// could not be expressed as one descriptor set addressed by name.
        bool MergeBindings(std::string_view pipeline, std::span<const StageSource* const> stages,
                           std::vector<ShaderBinding>& merged) {
            merged.clear();
            for (const StageSource* stage : stages) {
                for (const ShaderBinding& b : stage->reflection.bindings) {
                    if (b.set != 0) {
                        Log::Error("Pipeline '{}': '{}' is in descriptor set {}, only set 0 is supported",
                                   pipeline, b.name, b.set);
                        return false;
                    }
                    if (b.name.empty()) {
                        Log::Error("Pipeline '{}': binding {} has no name and cannot be bound", pipeline, b.binding);
                        return false;
                    }
                    if (b.count == 0) {
                        Log::Error("Pipeline '{}': binding {} '{}' has a descriptor count of zero", pipeline, b.binding, b.name);
                        return false;
                    }
                    const auto existing = std::find_if(merged.begin(), merged.end(),
                                                       [&](const ShaderBinding& m) { return m.binding == b.binding; });
                    if (existing == merged.end()) {
                        merged.push_back(b);
                        continue;
                    }
                    if (existing->type != b.type || existing->count != b.count) {
                        Log::Error("Pipeline '{}': binding {} is declared as {} '{}' x{} in one stage and as {} '{}' x{} in the other",
                                   pipeline, b.binding, BindingTypeName(existing->type), existing->name, existing->count,
                                   BindingTypeName(b.type), b.name, b.count);
                        return false;
                    }
                    if (existing->name != b.name) {
                        Log::Error("Pipeline '{}': binding {} is named '{}' in one stage and '{}' in the other",
                                   pipeline, b.binding, existing->name, b.name);
                        return false;
                    }
                    existing->stages |= b.stages;
                }
            }
            std::sort(merged.begin(), merged.end(),
                      [](const ShaderBinding& a, const ShaderBinding& b) { return a.binding < b.binding; });
            for (size_t i = 0; i < merged.size(); ++i) {
                for (size_t j = i + 1; j < merged.size(); ++j) {
                    if (merged[i].name == merged[j].name) {
                        Log::Error("Pipeline '{}': bindings {} and {} are both named '{}'",
                                   pipeline, merged[i].binding, merged[j].binding, merged[i].name);
                        return false;
                    }
                }
            }
            return true;
        }

        /// Checks the fragment shader's outputs against the attachments the pipeline will render to.
        bool ValidateOutputs(const PipelineDesc& desc, const ShaderReflection& fragment) {
            for (size_t i = 0; i < desc.colorFormats.size(); ++i) {
                if (!FindColorFormat(desc.colorFormats[i])) {
                    Log::Error("Pipeline '{}': colour attachment {} has format {}, which is not a supported colour format",
                               desc.name, i, static_cast<int>(desc.colorFormats[i]));
                    return false;
                }
            }

            for (const ShaderOutput& output : fragment.outputs) {
                if (output.location >= desc.colorFormats.size()) {
                    Log::Error("Pipeline '{}': fragment output '{}' writes location {} but the pipeline has {} colour attachment(s)",
                               desc.name, output.name, output.location, desc.colorFormats.size());
                    return false;
                }
                const ColorFormatInfo& format = *FindColorFormat(desc.colorFormats[output.location]);
                if (format.base != output.base) {
                    Log::Error("Pipeline '{}': fragment output '{}' (location {}) is {} but attachment format {} needs {}",
                               desc.name, output.name, output.location, BaseTypeName(output.base), format.name,
                               BaseTypeName(format.base));
                    return false;
                }
                if (output.components < format.components) {
                    Log::Warn("Pipeline '{}': fragment output '{}' (location {}) has {} component(s), attachment format {} has {}: "
                              "the remaining components are undefined",
                              desc.name, output.name, output.location, output.components, format.name, format.components);
                }
            }

            for (size_t i = 0; i < desc.colorFormats.size(); ++i) {
                const bool written = std::any_of(fragment.outputs.begin(), fragment.outputs.end(),
                                                 [&](const ShaderOutput& o) { return o.location == i; });
                if (!written) {
                    Log::Warn("Pipeline '{}': colour attachment {} is not written by '{}': its contents will be undefined",
                              desc.name, i, desc.fragmentShader);
                }
            }

            if (fragment.writesDepth && desc.depthFormat == VK_FORMAT_UNDEFINED) {
                Log::Error("Pipeline '{}': '{}' writes gl_FragDepth but the pipeline has no depth attachment",
                           desc.name, desc.fragmentShader);
                return false;
            }
            if (fragment.writesDepth && !desc.depthWrite) {
                Log::Warn("Pipeline '{}': '{}' writes gl_FragDepth but depth writes are disabled", desc.name, desc.fragmentShader);
            }
            return true;
        }

        /// Checks the parts of the description that do not depend on the shaders.
        bool ValidateDescription(const Device& device, const PipelineDesc& desc) {
            const uint32_t maxColorAttachments = device.Properties().limits.maxColorAttachments;
            if (desc.colorFormats.size() > maxColorAttachments) {
                Log::Error("Pipeline '{}': {} colour attachments requested, the device supports {}",
                           desc.name, desc.colorFormats.size(), maxColorAttachments);
                return false;
            }
            if (desc.depthFormat != VK_FORMAT_UNDEFINED && !IsDepthFormat(desc.depthFormat)) {
                Log::Error("Pipeline '{}': depth format {} is not a depth format", desc.name, static_cast<int>(desc.depthFormat));
                return false;
            }
            if ((desc.depthTest || desc.depthWrite) && desc.depthFormat == VK_FORMAT_UNDEFINED) {
                Log::Error("Pipeline '{}': depth test / write enabled without a depth format", desc.name);
                return false;
            }
            if (desc.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {
                Log::Error("Pipeline '{}': patch topology needs tessellation stages, which are not supported", desc.name);
                return false;
            }
            if (desc.blend.size() > desc.colorFormats.size()) {
                Log::Warn("Pipeline '{}': {} blend modes given for {} colour attachment(s), the extra ones are ignored",
                          desc.name, desc.blend.size(), desc.colorFormats.size());
            }
            for (size_t i = 0; i < desc.colorFormats.size() && i < desc.blend.size(); ++i) {
                const ColorFormatInfo* format = FindColorFormat(desc.colorFormats[i]);
                if (desc.blend[i] != BlendMode::Opaque && format && format->base != OutputBaseType::Float) {
                    Log::Error("Pipeline '{}': colour attachment {} ({}) is an integer format and cannot be blended",
                               desc.name, i, format->name);
                    return false;
                }
            }
            return true;
        }

        VkPipelineColorBlendAttachmentState BlendState(BlendMode mode) {
            VkPipelineColorBlendAttachmentState state{};
            state.blendEnable = VK_FALSE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.colorBlendOp = VK_BLEND_OP_ADD;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.alphaBlendOp = VK_BLEND_OP_ADD;
            state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            switch (mode) {
                case BlendMode::Opaque:
                    break;
                case BlendMode::Premultiplied:
                    state.blendEnable = VK_TRUE;
                    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
                case BlendMode::Additive:
                    state.blendEnable = VK_TRUE;
                    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    break;
            }
            return state;
        }

        /// Shader modules are only needed while the pipeline is being created.
        struct ShaderModuleGuard {
            VkDevice device = VK_NULL_HANDLE;
            VkShaderModule handle = VK_NULL_HANDLE;

            ShaderModuleGuard() = default;
            ShaderModuleGuard(const ShaderModuleGuard&) = delete;
            ShaderModuleGuard& operator=(const ShaderModuleGuard&) = delete;
            ~ShaderModuleGuard() {
                if (handle != VK_NULL_HANDLE) vkDestroyShaderModule(device, handle, nullptr);
            }

            VkResult Create(VkDevice dev, const std::vector<uint32_t>& code) {
                device = dev;
                VkShaderModuleCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                info.codeSize = code.size() * sizeof(uint32_t);
                info.pCode = code.data();
                return vkCreateShaderModule(dev, &info, nullptr, &handle);
            }
        };

        // -----------------------------------------------------------------------------------------
        //  DescriptorWriter diagnostics
        // -----------------------------------------------------------------------------------------
        /// Logs a binding problem the first time it is seen for this (pipeline, name) pair, so a
        /// mistake made every frame produces one line instead of sixty per second.
        void ReportOnce(std::string_view pipeline, std::string_view name, std::string_view message) {
            static std::mutex mutex;
            static std::set<std::string> reported;
            std::string key;
            key.reserve(pipeline.size() + name.size() + 1);
            key.append(pipeline).push_back('\n');
            key.append(name);
            bool first = false;
            {
                std::scoped_lock lock(mutex);
                first = reported.insert(std::move(key)).second;
            }
            if (first) Log::Error("Pipeline '{}': '{}': {}", pipeline, name, message);
        }

        /// Looks up the binding a resource is being provided for and checks its kind.
        const ShaderBinding* ResolveBinding(const Pipeline& pipeline, std::string_view name, bool isBuffer) {
            const ShaderBinding* binding = pipeline.FindBinding(name);
            if (!binding) {
                ReportOnce(pipeline.Name(), name, "the shaders of this pipeline declare no binding with this name");
                return nullptr;
            }
            const bool wantsBuffer = binding->type == BindingType::UniformBuffer || binding->type == BindingType::StorageBuffer;
            const bool wantsTexture = binding->type == BindingType::CombinedImageSampler;
            if (isBuffer ? !wantsBuffer : !wantsTexture) {
                ReportOnce(pipeline.Name(), name,
                           std::format("the shader declares a {} but a {} was provided",
                                       BindingTypeName(binding->type), isBuffer ? "buffer" : "texture"));
                return nullptr;
            }
            if (binding->count != 1) {
                ReportOnce(pipeline.Name(), name,
                           std::format("the shader declares an array of {} descriptors, which cannot be bound by name", binding->count));
                return nullptr;
            }
            return binding;
        }
    }

    // ---------------------------------------------------------------------------------------------
    //  Pipeline
    // ---------------------------------------------------------------------------------------------
    Pipeline::~Pipeline() { Destroy(); }

    const ShaderBinding* Pipeline::FindBinding(std::string_view name) const {
        for (const ShaderBinding& b : m_Bindings) {
            if (b.name == name) return &b;
        }
        return nullptr;
    }

    void Pipeline::Destroy() {
        // A device that has already been shut down has taken its objects with it.
        const VkDevice device = m_Device ? m_Device->Handle() : VK_NULL_HANDLE;
        if (device != VK_NULL_HANDLE) {
            if (m_Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, m_Pipeline, nullptr);
            if (m_Layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_Layout, nullptr);
            if (m_SetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
        }
        m_Pipeline = VK_NULL_HANDLE;
        m_Layout = VK_NULL_HANDLE;
        m_SetLayout = VK_NULL_HANDLE;
        m_Bindings.clear();
        m_Device = nullptr;
    }

    bool Pipeline::Create(Device& device, const PipelineDesc& desc) {
        Destroy();
        m_Name = desc.name;

        const VkDevice vk = device.Handle();
        if (vk == VK_NULL_HANDLE) {
            Log::Error("Pipeline '{}': the device is not initialised", desc.name);
            return false;
        }
        if (!ValidateDescription(device, desc)) return false;

        // ---- Shaders and their interface -----------------------------------------------------
        StageSource vertex;
        StageSource fragment;
        if (!LoadStage(desc.name, desc.vertexShader, VK_SHADER_STAGE_VERTEX_BIT, "vertex", vertex)) return false;
        if (!LoadStage(desc.name, desc.fragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT, "fragment", fragment)) return false;

        std::vector<ShaderBinding> bindings;
        const std::array<const StageSource*, 2> stageSources = {&vertex, &fragment};
        if (!MergeBindings(desc.name, stageSources, bindings)) return false;
        if (!ValidateOutputs(desc, fragment.reflection)) return false;

        // From here on Vulkan objects exist: every failure path goes through Destroy().
        m_Device = &device;
        const auto fail = [this]() {
            Destroy();
            return false;
        };

        // ---- Descriptor set layout + pipeline layout -------------------------------------------
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        layoutBindings.reserve(bindings.size());
        for (const ShaderBinding& b : bindings) {
            VkDescriptorSetLayoutBinding lb{};
            lb.binding = b.binding;
            lb.descriptorType = ToDescriptorType(b.type);
            lb.descriptorCount = b.count;
            lb.stageFlags = b.stages;
            lb.pImmutableSamplers = nullptr;
            layoutBindings.push_back(lb);
        }

        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
        setLayoutInfo.pBindings = layoutBindings.empty() ? nullptr : layoutBindings.data();
        VkResult result = vkCreateDescriptorSetLayout(vk, &setLayoutInfo, nullptr, &m_SetLayout);
        if (result != VK_SUCCESS) {
            m_SetLayout = VK_NULL_HANDLE;
            Log::Error("Pipeline '{}': vkCreateDescriptorSetLayout failed (VkResult {})", desc.name, static_cast<int>(result));
            return fail();
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_SetLayout;
        layoutInfo.pushConstantRangeCount = 0;
        layoutInfo.pPushConstantRanges = nullptr;
        result = vkCreatePipelineLayout(vk, &layoutInfo, nullptr, &m_Layout);
        if (result != VK_SUCCESS) {
            m_Layout = VK_NULL_HANDLE;
            Log::Error("Pipeline '{}': vkCreatePipelineLayout failed (VkResult {})", desc.name, static_cast<int>(result));
            return fail();
        }

        // ---- Shader modules ----------------------------------------------------------------------
        ShaderModuleGuard vertexModule;
        ShaderModuleGuard fragmentModule;
        result = vertexModule.Create(vk, vertex.code);
        if (result != VK_SUCCESS) {
            Log::Error("Pipeline '{}': vkCreateShaderModule failed for '{}' (VkResult {})",
                       desc.name, desc.vertexShader, static_cast<int>(result));
            return fail();
        }
        result = fragmentModule.Create(vk, fragment.code);
        if (result != VK_SUCCESS) {
            Log::Error("Pipeline '{}': vkCreateShaderModule failed for '{}' (VkResult {})",
                       desc.name, desc.fragmentShader, static_cast<int>(result));
            return fail();
        }

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule.handle;
        stages[0].pName = vertex.reflection.entryPoint.c_str();
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule.handle;
        stages[1].pName = fragment.reflection.entryPoint.c_str();

        // ---- Fixed-function state ----------------------------------------------------------------
        // No vertex buffers: vertices are generated from gl_VertexIndex / gl_InstanceIndex.
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = desc.topology;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Viewport and scissor are dynamic: only their count is part of the pipeline.
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.depthClampEnable = VK_FALSE;
        raster.rasterizerDiscardEnable = VK_FALSE;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = desc.cullMode;
        raster.frontFace = desc.frontFace;
        raster.depthBiasEnable = VK_FALSE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisample.sampleShadingEnable = VK_FALSE;
        multisample.minSampleShading = 1.0f;

        // Vulkan only writes depth while the depth test is enabled, so "write without test" is
        // expressed as a test that always passes.
        const bool depthEnabled = desc.depthTest || desc.depthWrite;
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = depthEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = desc.depthTest ? desc.depthCompare : VK_COMPARE_OP_ALWAYS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;
        depthStencil.front.failOp = VK_STENCIL_OP_KEEP;
        depthStencil.front.passOp = VK_STENCIL_OP_KEEP;
        depthStencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
        depthStencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depthStencil.back = depthStencil.front;
        depthStencil.minDepthBounds = 0.0f;
        depthStencil.maxDepthBounds = 1.0f;

        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
        blendAttachments.reserve(desc.colorFormats.size());
        for (size_t i = 0; i < desc.colorFormats.size(); ++i) {
            blendAttachments.push_back(BlendState(i < desc.blend.size() ? desc.blend[i] : BlendMode::Opaque));
        }
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.logicOpEnable = VK_FALSE;
        colorBlend.logicOp = VK_LOGIC_OP_COPY;
        colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
        colorBlend.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();

        const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        // Dynamic rendering: the attachment formats replace the render pass. The engine never
        // binds a stencil attachment, so the stencil format stays undefined even for combined
        // depth / stencil formats.
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.viewMask = 0;
        rendering.colorAttachmentCount = static_cast<uint32_t>(desc.colorFormats.size());
        rendering.pColorAttachmentFormats = desc.colorFormats.empty() ? nullptr : desc.colorFormats.data();
        rendering.depthAttachmentFormat = desc.depthFormat;
        rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pTessellationState = nullptr;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamic;
        info.layout = m_Layout;
        info.renderPass = VK_NULL_HANDLE;
        info.subpass = 0;
        info.basePipelineHandle = VK_NULL_HANDLE;
        info.basePipelineIndex = -1;

        result = vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &info, nullptr, &m_Pipeline);
        if (result != VK_SUCCESS) {
            m_Pipeline = VK_NULL_HANDLE;
            Log::Error("Pipeline '{}': vkCreateGraphicsPipelines failed (VkResult {})", desc.name, static_cast<int>(result));
            return fail();
        }

        m_Bindings = std::move(bindings);

        if (!desc.name.empty()) {
            device.SetDebugName(VK_OBJECT_TYPE_PIPELINE, HandleValue(m_Pipeline), desc.name);
            device.SetDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, HandleValue(m_Layout), desc.name);
            device.SetDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, HandleValue(m_SetLayout), desc.name);
        }
        Log::Debug("Pipeline '{}' created: {} + {}, {} binding(s), {} colour attachment(s)",
                   desc.name, desc.vertexShader, desc.fragmentShader, m_Bindings.size(), desc.colorFormats.size());
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    //  DescriptorWriter
    // ---------------------------------------------------------------------------------------------
    DescriptorWriter::DescriptorWriter(Device& device, VkDescriptorPool pool, const Pipeline& pipeline)
        : m_Device(device), m_Pool(pool), m_Pipeline(pipeline) {
        m_Writes.reserve(pipeline.Bindings().size());
    }

    DescriptorWriter& DescriptorWriter::Buffer(std::string_view name, const graphics::Buffer& buffer,
                                               VkDeviceSize offset, VkDeviceSize range) {
        const ShaderBinding* binding = ResolveBinding(m_Pipeline, name, true);
        if (!binding) {
            m_Failed = true;
            return *this;
        }
        const auto reject = [&](std::string_view message) -> DescriptorWriter& {
            ReportOnce(m_Pipeline.Name(), name, message);
            m_Failed = true;
            return *this;
        };

        for (const Pending& write : m_Writes) {
            if (write.binding == binding->binding) return reject("a resource was provided more than once");
        }
        if (buffer.handle == VK_NULL_HANDLE || buffer.size == 0) return reject("the buffer provided is null or empty");
        if (offset >= buffer.size) {
            return reject(std::format("offset {} is outside the buffer ({} bytes)", offset, buffer.size));
        }
        if (range != VK_WHOLE_SIZE && (range == 0 || range > buffer.size - offset)) {
            return reject(std::format("range {} at offset {} does not fit the buffer ({} bytes)", range, offset, buffer.size));
        }

        const VkPhysicalDeviceLimits& limits = m_Device.Properties().limits;
        const bool uniform = binding->type == BindingType::UniformBuffer;
        const VkDeviceSize alignment = uniform ? limits.minUniformBufferOffsetAlignment : limits.minStorageBufferOffsetAlignment;
        const VkDeviceSize maxRange = uniform ? limits.maxUniformBufferRange : limits.maxStorageBufferRange;
        const VkDeviceSize effectiveRange = range == VK_WHOLE_SIZE ? buffer.size - offset : range;
        if (alignment > 1 && offset % alignment != 0) {
            return reject(std::format("offset {} is not a multiple of the device's {} offset alignment ({})",
                                      offset, BindingTypeName(binding->type), alignment));
        }
        if (maxRange != 0 && effectiveRange > maxRange) {
            return reject(std::format("range {} exceeds the device's maximum {} range ({})",
                                      effectiveRange, BindingTypeName(binding->type), maxRange));
        }

        Pending write;
        write.binding = binding->binding;
        write.type = ToDescriptorType(binding->type);
        write.buffer.buffer = buffer.handle;
        write.buffer.offset = offset;
        write.buffer.range = range;
        m_Writes.push_back(write);
        return *this;
    }

    DescriptorWriter& DescriptorWriter::Texture(std::string_view name, VkImageView view, VkSampler sampler,
                                                VkImageLayout layout) {
        const ShaderBinding* binding = ResolveBinding(m_Pipeline, name, false);
        if (!binding) {
            m_Failed = true;
            return *this;
        }
        const auto reject = [&](std::string_view message) -> DescriptorWriter& {
            ReportOnce(m_Pipeline.Name(), name, message);
            m_Failed = true;
            return *this;
        };

        for (const Pending& write : m_Writes) {
            if (write.binding == binding->binding) return reject("a resource was provided more than once");
        }
        if (view == VK_NULL_HANDLE) return reject("the image view provided is null");
        if (sampler == VK_NULL_HANDLE) return reject("the sampler provided is null");

        Pending write;
        write.binding = binding->binding;
        write.type = ToDescriptorType(binding->type);
        write.image.sampler = sampler;
        write.image.imageView = view;
        write.image.imageLayout = layout;
        m_Writes.push_back(write);
        return *this;
    }

    bool DescriptorWriter::Bind(VkCommandBuffer cmd) {
        const std::string& pipelineName = m_Pipeline.Name();
        if (!m_Pipeline.IsValid()) {
            ReportOnce(pipelineName, "<pipeline>", "cannot bind a pipeline that was not created");
            return false;
        }
        const VkDevice device = m_Device.Handle();
        if (device == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || m_Pool == VK_NULL_HANDLE) {
            ReportOnce(pipelineName, "<descriptor set>", "cannot bind without a device, command buffer and descriptor pool");
            return false;
        }

        // Every binding the shaders declare must have received its resource.
        bool complete = !m_Failed;
        for (const ShaderBinding& binding : m_Pipeline.Bindings()) {
            const bool provided = std::any_of(m_Writes.begin(), m_Writes.end(),
                                              [&](const Pending& w) { return w.binding == binding.binding; });
            if (!provided) {
                ReportOnce(pipelineName, binding.name,
                           std::format("no resource was provided for this {} (binding {})",
                                       BindingTypeName(binding.type), binding.binding));
                complete = false;
            }
        }
        if (!complete) return false;

        const VkDescriptorSetLayout setLayout = m_Pipeline.SetLayout();
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_Pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &set);
        if (result != VK_SUCCESS || set == VK_NULL_HANDLE) {
            ReportOnce(pipelineName, "<descriptor set>",
                       std::format("vkAllocateDescriptorSets failed (VkResult {}): the frame's descriptor pool is too small",
                                   static_cast<int>(result)));
            return false;
        }

        // m_Writes is not modified any more, so pointers into it stay valid for the update below.
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(m_Writes.size());
        for (const Pending& pending : m_Writes) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = pending.binding;
            write.dstArrayElement = 0;
            write.descriptorCount = 1;
            write.descriptorType = pending.type;
            if (pending.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                write.pImageInfo = &pending.image;
            } else {
                write.pBufferInfo = &pending.buffer;
            }
            writes.push_back(write);
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline.Handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline.Layout(), 0, 1, &set, 0, nullptr);
        return true;
    }
}
