module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module vortex.graphics:shader;

import :device;
import :resources;

export namespace vortex::graphics {

    // ---------------------------------------------------------------------------------------------
    //  SPIR-V reflection (pure CPU: unit testable without a GPU)
    // ---------------------------------------------------------------------------------------------
    enum class BindingType { UniformBuffer, StorageBuffer, CombinedImageSampler, StorageImage };

    struct ShaderBinding {
        uint32_t set = 0;
        uint32_t binding = 0;
        BindingType type = BindingType::UniformBuffer;
        /// Name the renderer binds by: the GLSL instance name (`u_Frame`), or the block name if
        /// the block has no instance name, or the uniform name for samplers / images.
        std::string name;
        std::string blockName;          ///< GLSL block type name (empty for samplers / images).
        uint32_t count = 1;             ///< Array size.
        VkShaderStageFlags stages = 0;
    };

    enum class OutputBaseType { Float, Int, UInt };

    struct ShaderOutput {
        uint32_t location = 0;
        OutputBaseType base = OutputBaseType::Float;
        uint32_t components = 4;
        std::string name;
    };

    struct BlockMember {
        std::string name;
        uint32_t offset = 0;
    };

    /// @brief Top-level members of a uniform / storage block, or of the struct its array element is made of.
    struct BlockLayout {
        std::string blockName;                 ///< Block or struct type name.
        std::vector<BlockMember> members;
        uint32_t arrayStride = 0;              ///< For `struct T items[]` blocks: stride of T; 0 otherwise.
    };

    struct ShaderReflection {
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
        std::string entryPoint = "main";
        std::vector<ShaderBinding> bindings;
        std::vector<ShaderOutput> outputs;     ///< Fragment shader colour outputs.
        std::vector<BlockLayout> blocks;       ///< Every struct type that carries Offset decorations.
        bool writesDepth = false;              ///< Fragment shader writes gl_FragDepth.

        const ShaderBinding* FindBinding(std::string_view name) const;
        const BlockLayout* FindBlock(std::string_view blockOrStructName) const;
    };

    /**
     * @brief Extracts the interface of a SPIR-V module: descriptor bindings, fragment outputs
     * and block member offsets.
     * @details This is what lets pipeline layouts be derived from the shaders instead of being
     * written twice, and what the unit tests use to prove that C++ structs and GLSL blocks agree.
     * @return false (and `error`) for malformed input.
     */
    bool ReflectSpirv(std::span<const uint32_t> spirv, ShaderReflection& out, std::string* error = nullptr);

    /// @brief Reads a .spv file. Returns an empty vector if it is missing or not SPIR-V.
    std::vector<uint32_t> LoadSpirvFile(const std::filesystem::path& path);

    // ---------------------------------------------------------------------------------------------
    //  Pipelines
    // ---------------------------------------------------------------------------------------------
    enum class BlendMode {
        Opaque,          ///< No blending.
        Premultiplied,   ///< src + dst * (1 - src.a): covers both alpha and additive particles.
        Additive         ///< src + dst.
    };

    struct PipelineDesc {
        std::string name;                          ///< For logs and debug labels.
        std::string vertexShader;                  ///< File name under assets/shaders, e.g. "fullscreen.vert.spv".
        std::string fragmentShader;
        std::vector<VkFormat> colorFormats;        ///< One per colour attachment (dynamic rendering).
        std::vector<BlendMode> blend;              ///< Per colour attachment; missing entries = Opaque.
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        bool depthTest = false;
        bool depthWrite = false;
        VkCompareOp depthCompare = VK_COMPARE_OP_GREATER;   ///< Reverse-Z: greater = closer.
        VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    };

    /**
     * @brief Graphics pipeline for dynamic rendering with one descriptor set (set 0) whose
     * layout is reflected from the two shader stages. Viewport and scissor are dynamic.
     * @details Create() validates the shader interface against the description and refuses to
     * build a pipeline that would be invalid: every fragment output location must have a colour
     * attachment whose format class (float / int / uint) matches the output type.
     */
    class Pipeline {
    public:
        Pipeline() = default;
        ~Pipeline();
        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;

        bool Create(Device& device, const PipelineDesc& desc);
        void Destroy();
        bool IsValid() const { return m_Pipeline != VK_NULL_HANDLE; }

        VkPipeline Handle() const { return m_Pipeline; }
        VkPipelineLayout Layout() const { return m_Layout; }
        VkDescriptorSetLayout SetLayout() const { return m_SetLayout; }
        const std::string& Name() const { return m_Name; }
        /// @brief Union of the bindings of both stages.
        std::span<const ShaderBinding> Bindings() const { return m_Bindings; }
        const ShaderBinding* FindBinding(std::string_view name) const;

    private:
        Device* m_Device = nullptr;
        std::string m_Name;
        VkPipeline m_Pipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_Layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
        std::vector<ShaderBinding> m_Bindings;
    };

    /**
     * @brief Fills and binds the descriptor set of a pipeline, addressing resources by name.
     * @code
     *   DescriptorWriter(device, frame.descriptorPool, pipeline)
     *       .Buffer("u_Frame", frame.uniforms)
     *       .Texture("u_Depth", depth.view, nearestSampler)
     *       .Bind(cmd);
     * @endcode
     * Bind() reports (once per pipeline and name) any binding the shaders declare that was not
     * provided, any name the shaders do not have and any type mismatch - turning a class of
     * silent GPU errors into log messages, even without the validation layer.
     */
    class DescriptorWriter {
    public:
        DescriptorWriter(Device& device, VkDescriptorPool pool, const Pipeline& pipeline);

        DescriptorWriter& Buffer(std::string_view name, const graphics::Buffer& buffer,
                                 VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE);
        DescriptorWriter& Texture(std::string_view name, VkImageView view, VkSampler sampler,
                                  VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        /// @brief Allocates the set from the pool, writes it and binds pipeline + set. Returns false on any error.
        bool Bind(VkCommandBuffer cmd);

    private:
        struct Pending {
            uint32_t binding = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            VkDescriptorBufferInfo buffer{};
            VkDescriptorImageInfo image{};
        };
        Device& m_Device;
        VkDescriptorPool m_Pool;
        const Pipeline& m_Pipeline;
        std::vector<Pending> m_Writes;
        bool m_Failed = false;
    };
}
