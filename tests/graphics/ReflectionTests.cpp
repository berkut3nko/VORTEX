// SPIR-V reflection tests on hand-assembled modules: every construct the reflection has to
// understand (blocks, storage buffers old and new style, samplers, arrays, storage images,
// fragment outputs, gl_FragDepth) and every way a module can be malformed. No GPU involved.

#include <gtest/gtest.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

import vortex.core;
import vortex.graphics;

using namespace vortex;
using namespace vortex::graphics;

namespace {

    constexpr uint32_t kMagic = 0x07230203u;

    // Opcodes, decorations, storage classes and execution models used by the test modules.
    enum : uint32_t {
        OpNop = 0, OpName = 5, OpMemberName = 6, OpEntryPoint = 15,
        OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampler = 26,
        OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30,
        OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71, OpMemberDecorate = 72,
    };
    enum : uint32_t {
        DecBlock = 2, DecBufferBlock = 3, DecArrayStride = 6, DecBuiltIn = 11, DecLocation = 30,
        DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35,
    };
    enum : uint32_t { ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScFunction = 7, ScStorageBuffer = 12 };
    enum : uint32_t { ModelVertex = 0, ModelGeometry = 3, ModelFragment = 4 };
    constexpr uint32_t kBuiltInFragDepth = 22;

    /// Minimal SPIR-V assembler: header + instructions, nothing validated (on purpose).
    class SpirvBuilder {
    public:
        SpirvBuilder() : m_Words{kMagic, 0x00010600u, 0u, 128u, 0u} {}

        void Op(uint32_t opcode, std::initializer_list<uint32_t> operands) {
            m_Words.push_back((static_cast<uint32_t>(operands.size() + 1) << 16) | opcode);
            m_Words.insert(m_Words.end(), operands.begin(), operands.end());
        }

        /// Instruction of the form: leading operands, literal string, trailing operands.
        void OpString(uint32_t opcode, std::initializer_list<uint32_t> leading, std::string_view text,
                      std::initializer_list<uint32_t> trailing = {}) {
            std::vector<uint32_t> operands(leading);
            // Nul-terminated, padded with zeros to a whole number of words.
            const size_t stringWords = text.size() / 4 + 1;
            const size_t base = operands.size();
            operands.resize(base + stringWords, 0u);
            for (size_t i = 0; i < text.size(); ++i) {
                operands[base + i / 4] |= static_cast<uint32_t>(static_cast<unsigned char>(text[i])) << (8 * (i % 4));
            }
            operands.insert(operands.end(), trailing.begin(), trailing.end());
            m_Words.push_back((static_cast<uint32_t>(operands.size() + 1) << 16) | opcode);
            m_Words.insert(m_Words.end(), operands.begin(), operands.end());
        }

        void Name(uint32_t id, std::string_view name) { OpString(OpName, {id}, name); }
        void MemberName(uint32_t id, uint32_t member, std::string_view name) { OpString(OpMemberName, {id, member}, name); }
        void Decorate(uint32_t id, uint32_t decoration) { Op(OpDecorate, {id, decoration}); }
        void Decorate(uint32_t id, uint32_t decoration, uint32_t value) { Op(OpDecorate, {id, decoration, value}); }
        void MemberOffset(uint32_t id, uint32_t member, uint32_t offset) { Op(OpMemberDecorate, {id, member, DecOffset, offset}); }
        void Resource(uint32_t id, uint32_t set, uint32_t binding) {
            Decorate(id, DecDescriptorSet, set);
            Decorate(id, DecBinding, binding);
        }

        const std::vector<uint32_t>& Words() const { return m_Words; }
        std::vector<uint32_t>& Words() { return m_Words; }

    private:
        std::vector<uint32_t> m_Words;
    };

    // Ids of the reference module below.
    enum : uint32_t {
        IdMain = 1, IdFloat, IdVec4, IdUint, IdInt, IdIVec2,
        IdFrameBlock, IdFrameBlockPtr, IdFrameVar,
        IdItem, IdItemArray, IdItemBuffer, IdItemBufferPtr, IdItemBufferVar,
        IdLegacyArray, IdLegacyBuffer, IdLegacyPtr, IdLegacyVar,
        IdImage, IdSampledImage, IdSampledImagePtr, IdTexVar,
        IdThree, IdTexArray, IdTexArrayPtr, IdTexArrayVar,
        IdStorageImage, IdStorageImagePtr, IdStorageImageVar,
        IdOutUintPtr, IdOutIdVar, IdOutIVec2Ptr, IdOutOffsetVar, IdOutVec4Ptr, IdOutColorVar,
        IdOutFloatPtr, IdFragDepthVar, IdLocalStruct, IdFunctionPtr, IdFunctionVar, IdInVec4Ptr, IdInVar,
    };

    /// A module exercising everything the reflection supports. GLSL equivalent (fragment):
    ///   layout(set=0,binding=0) uniform FrameBlock { vec4 first; uint second; } u_Frame;
    ///   struct Item { vec4 p; uint q; };
    ///   layout(set=0,binding=3) buffer ItemBuffer { Item items[]; };              // no instance name
    ///   layout(set=0,binding=4) buffer LegacyBuffer { uint data[]; } u_Legacy;    // SPIR-V 1.0 style (BufferBlock)
    ///   layout(set=0,binding=16) uniform sampler2D u_Tex;
    ///   layout(set=0,binding=17) uniform sampler2D u_TexArray[3];
    ///   layout(set=1,binding=18, rgba8) uniform image2D u_Image;
    ///   layout(location=0) out uint o_Id; layout(location=1) out vec4 o_Color; layout(location=2) out ivec2 o_Offset;
    ///   + gl_FragDepth
    SpirvBuilder ReferenceModule(uint32_t executionModel = ModelFragment) {
        SpirvBuilder b;
        b.OpString(OpEntryPoint, {executionModel, IdMain}, "main", {IdOutIdVar, IdOutColorVar});

        b.Name(IdFrameBlock, "FrameBlock");
        b.MemberName(IdFrameBlock, 0, "first");
        b.MemberName(IdFrameBlock, 1, "second");
        b.Name(IdFrameVar, "u_Frame");
        b.Name(IdItem, "Item");
        b.MemberName(IdItem, 0, "p");
        b.MemberName(IdItem, 1, "q");
        b.Name(IdItemBuffer, "ItemBuffer");
        b.MemberName(IdItemBuffer, 0, "items");
        b.Name(IdItemBufferVar, "");                      // glslang names instance-less blocks ""
        b.Name(IdLegacyBuffer, "LegacyBuffer");
        b.MemberName(IdLegacyBuffer, 0, "data");
        b.Name(IdLegacyVar, "u_Legacy");
        b.Name(IdTexVar, "u_Tex");
        b.Name(IdTexArrayVar, "u_TexArray");
        b.Name(IdStorageImageVar, "u_Image");
        b.Name(IdOutIdVar, "o_Id");
        b.Name(IdOutColorVar, "o_Color");
        b.Name(IdOutOffsetVar, "o_Offset");
        b.Name(IdFragDepthVar, "gl_FragDepth");
        b.Name(IdLocalStruct, "Local");
        b.MemberName(IdLocalStruct, 0, "value");

        b.Decorate(IdFrameBlock, DecBlock);
        b.MemberOffset(IdFrameBlock, 0, 0);
        b.MemberOffset(IdFrameBlock, 1, 16);
        b.Resource(IdFrameVar, 0, 0);

        b.MemberOffset(IdItem, 0, 0);
        b.MemberOffset(IdItem, 1, 16);
        b.Decorate(IdItemArray, DecArrayStride, 32);
        b.Decorate(IdItemBuffer, DecBlock);
        b.MemberOffset(IdItemBuffer, 0, 0);
        b.Resource(IdItemBufferVar, 0, 3);

        b.Decorate(IdLegacyArray, DecArrayStride, 4);
        b.Decorate(IdLegacyBuffer, DecBufferBlock);
        b.MemberOffset(IdLegacyBuffer, 0, 0);
        b.Resource(IdLegacyVar, 0, 4);

        b.Resource(IdTexVar, 0, 16);
        b.Resource(IdTexArrayVar, 0, 17);
        b.Resource(IdStorageImageVar, 1, 18);

        b.Decorate(IdOutIdVar, DecLocation, 0);
        b.Decorate(IdOutColorVar, DecLocation, 1);
        b.Decorate(IdOutOffsetVar, DecLocation, 2);
        b.Decorate(IdFragDepthVar, DecBuiltIn, kBuiltInFragDepth);
        b.Decorate(IdInVar, DecLocation, 0);

        b.Op(OpTypeFloat, {IdFloat, 32});
        b.Op(OpTypeVector, {IdVec4, IdFloat, 4});
        b.Op(OpTypeInt, {IdUint, 32, 0});
        b.Op(OpTypeInt, {IdInt, 32, 1});
        b.Op(OpTypeVector, {IdIVec2, IdInt, 2});

        b.Op(OpTypeStruct, {IdFrameBlock, IdVec4, IdUint});
        b.Op(OpTypePointer, {IdFrameBlockPtr, ScUniform, IdFrameBlock});
        b.Op(OpVariable, {IdFrameBlockPtr, IdFrameVar, ScUniform});

        b.Op(OpTypeStruct, {IdItem, IdVec4, IdUint});
        b.Op(OpTypeRuntimeArray, {IdItemArray, IdItem});
        b.Op(OpTypeStruct, {IdItemBuffer, IdItemArray});
        b.Op(OpTypePointer, {IdItemBufferPtr, ScStorageBuffer, IdItemBuffer});
        b.Op(OpVariable, {IdItemBufferPtr, IdItemBufferVar, ScStorageBuffer});

        b.Op(OpTypeRuntimeArray, {IdLegacyArray, IdUint});
        b.Op(OpTypeStruct, {IdLegacyBuffer, IdLegacyArray});
        b.Op(OpTypePointer, {IdLegacyPtr, ScUniform, IdLegacyBuffer});
        b.Op(OpVariable, {IdLegacyPtr, IdLegacyVar, ScUniform});

        // result, sampled type, dim (1 = 2D), depth, arrayed, multisampled, sampled, format
        b.Op(OpTypeImage, {IdImage, IdFloat, 1, 0, 0, 0, 1, 0});
        b.Op(OpTypeSampledImage, {IdSampledImage, IdImage});
        b.Op(OpTypePointer, {IdSampledImagePtr, ScUniformConstant, IdSampledImage});
        b.Op(OpVariable, {IdSampledImagePtr, IdTexVar, ScUniformConstant});

        b.Op(OpConstant, {IdUint, IdThree, 3});
        b.Op(OpTypeArray, {IdTexArray, IdSampledImage, IdThree});
        b.Op(OpTypePointer, {IdTexArrayPtr, ScUniformConstant, IdTexArray});
        b.Op(OpVariable, {IdTexArrayPtr, IdTexArrayVar, ScUniformConstant});

        b.Op(OpTypeImage, {IdStorageImage, IdFloat, 1, 0, 0, 0, 2, 4});
        b.Op(OpTypePointer, {IdStorageImagePtr, ScUniformConstant, IdStorageImage});
        b.Op(OpVariable, {IdStorageImagePtr, IdStorageImageVar, ScUniformConstant});

        b.Op(OpTypePointer, {IdOutUintPtr, ScOutput, IdUint});
        b.Op(OpVariable, {IdOutUintPtr, IdOutIdVar, ScOutput});
        b.Op(OpTypePointer, {IdOutIVec2Ptr, ScOutput, IdIVec2});
        b.Op(OpVariable, {IdOutIVec2Ptr, IdOutOffsetVar, ScOutput});
        b.Op(OpTypePointer, {IdOutVec4Ptr, ScOutput, IdVec4});
        b.Op(OpVariable, {IdOutVec4Ptr, IdOutColorVar, ScOutput});
        b.Op(OpTypePointer, {IdOutFloatPtr, ScOutput, IdFloat});
        b.Op(OpVariable, {IdOutFloatPtr, IdFragDepthVar, ScOutput});

        // Things that must be ignored: a plain struct without offsets, a function-local
        // variable and a stage input.
        b.Op(OpTypeStruct, {IdLocalStruct, IdVec4});
        b.Op(OpTypePointer, {IdFunctionPtr, ScFunction, IdLocalStruct});
        b.Op(OpVariable, {IdFunctionPtr, IdFunctionVar, ScFunction});
        b.Op(OpTypePointer, {IdInVec4Ptr, ScInput, IdVec4});
        b.Op(OpVariable, {IdInVec4Ptr, IdInVar, ScInput});
        return b;
    }

    /// The smallest module the reflection accepts: a header and an entry point.
    SpirvBuilder EmptyModule(uint32_t executionModel = ModelFragment) {
        SpirvBuilder b;
        b.OpString(OpEntryPoint, {executionModel, IdMain}, "main");
        return b;
    }

    void ExpectRejected(const std::vector<uint32_t>& words, std::string_view what) {
        ShaderReflection reflection;
        reflection.writesDepth = true;   // must be reset on failure
        reflection.bindings.resize(2);
        std::string error;
        EXPECT_FALSE(ReflectSpirv(words, reflection, &error)) << what;
        EXPECT_FALSE(error.empty()) << what;
        EXPECT_TRUE(reflection.bindings.empty()) << what;
        EXPECT_TRUE(reflection.outputs.empty()) << what;
        EXPECT_TRUE(reflection.blocks.empty()) << what;
        EXPECT_FALSE(reflection.writesDepth) << what;
        // The error string is optional.
        EXPECT_FALSE(ReflectSpirv(words, reflection)) << what;
    }

    const ShaderBinding* BindingAt(const ShaderReflection& r, uint32_t set, uint32_t binding) {
        for (const ShaderBinding& b : r.bindings) {
            if (b.set == set && b.binding == binding) return &b;
        }
        return nullptr;
    }

    std::filesystem::path TempFile(std::string_view name) {
        return std::filesystem::temp_directory_path() / std::filesystem::path(name);
    }

    void WriteBytes(const std::filesystem::path& path, const void* data, size_t bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good()) << path.string();
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        ASSERT_TRUE(file.good()) << path.string();
    }
}

// -------------------------------------------------------------------------------------------------
//  Well-formed modules
// -------------------------------------------------------------------------------------------------

TEST(ShaderReflection, ReportsStageAndEntryPoint) {
    ShaderReflection fragment;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ReferenceModule(ModelFragment).Words(), fragment, &error)) << error;
    EXPECT_EQ(fragment.stage, VK_SHADER_STAGE_FRAGMENT_BIT);
    EXPECT_EQ(fragment.entryPoint, "main");

    ShaderReflection vertex;
    ASSERT_TRUE(ReflectSpirv(ReferenceModule(ModelVertex).Words(), vertex, &error)) << error;
    EXPECT_EQ(vertex.stage, VK_SHADER_STAGE_VERTEX_BIT);

    SpirvBuilder renamed;
    renamed.OpString(OpEntryPoint, {ModelVertex, IdMain}, "VertexMain");   // longer than two words
    ShaderReflection other;
    ASSERT_TRUE(ReflectSpirv(renamed.Words(), other, &error)) << error;
    EXPECT_EQ(other.entryPoint, "VertexMain");
    EXPECT_TRUE(other.bindings.empty());
    EXPECT_TRUE(other.outputs.empty());
    EXPECT_TRUE(other.blocks.empty());
    EXPECT_FALSE(other.writesDepth);
}

TEST(ShaderReflection, ClassifiesBindings) {
    ShaderReflection r;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ReferenceModule().Words(), r, &error)) << error;
    ASSERT_EQ(r.bindings.size(), 6u);

    const ShaderBinding* frame = BindingAt(r, 0, 0);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->type, BindingType::UniformBuffer);
    EXPECT_EQ(frame->name, "u_Frame");
    EXPECT_EQ(frame->blockName, "FrameBlock");
    EXPECT_EQ(frame->count, 1u);
    EXPECT_EQ(frame->stages, static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_FRAGMENT_BIT));

    // A block without an instance name is bound by its block name.
    const ShaderBinding* items = BindingAt(r, 0, 3);
    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items->type, BindingType::StorageBuffer);
    EXPECT_EQ(items->name, "ItemBuffer");
    EXPECT_EQ(items->blockName, "ItemBuffer");

    // SPIR-V 1.0 style storage buffer: Uniform storage class + BufferBlock.
    const ShaderBinding* legacy = BindingAt(r, 0, 4);
    ASSERT_NE(legacy, nullptr);
    EXPECT_EQ(legacy->type, BindingType::StorageBuffer);
    EXPECT_EQ(legacy->name, "u_Legacy");
    EXPECT_EQ(legacy->blockName, "LegacyBuffer");

    const ShaderBinding* tex = BindingAt(r, 0, 16);
    ASSERT_NE(tex, nullptr);
    EXPECT_EQ(tex->type, BindingType::CombinedImageSampler);
    EXPECT_EQ(tex->name, "u_Tex");
    EXPECT_TRUE(tex->blockName.empty());
    EXPECT_EQ(tex->count, 1u);

    const ShaderBinding* texArray = BindingAt(r, 0, 17);
    ASSERT_NE(texArray, nullptr);
    EXPECT_EQ(texArray->type, BindingType::CombinedImageSampler);
    EXPECT_EQ(texArray->name, "u_TexArray");
    EXPECT_EQ(texArray->count, 3u);

    const ShaderBinding* image = BindingAt(r, 1, 18);
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->type, BindingType::StorageImage);
    EXPECT_EQ(image->name, "u_Image");

    // Sorted by (set, binding).
    for (size_t i = 1; i < r.bindings.size(); ++i) {
        const ShaderBinding& a = r.bindings[i - 1];
        const ShaderBinding& b = r.bindings[i];
        EXPECT_TRUE(a.set < b.set || (a.set == b.set && a.binding < b.binding));
    }

    EXPECT_EQ(r.FindBinding("u_Tex"), tex);
    EXPECT_EQ(r.FindBinding("ItemBuffer"), items);
    EXPECT_EQ(r.FindBinding("u_Missing"), nullptr);
    EXPECT_EQ(r.FindBinding(""), nullptr);
}

TEST(ShaderReflection, ReportsFragmentOutputsAndDepth) {
    ShaderReflection r;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ReferenceModule().Words(), r, &error)) << error;

    ASSERT_EQ(r.outputs.size(), 3u);   // gl_FragDepth is not a colour output
    EXPECT_EQ(r.outputs[0].location, 0u);
    EXPECT_EQ(r.outputs[0].base, OutputBaseType::UInt);
    EXPECT_EQ(r.outputs[0].components, 1u);
    EXPECT_EQ(r.outputs[0].name, "o_Id");

    EXPECT_EQ(r.outputs[1].location, 1u);
    EXPECT_EQ(r.outputs[1].base, OutputBaseType::Float);
    EXPECT_EQ(r.outputs[1].components, 4u);
    EXPECT_EQ(r.outputs[1].name, "o_Color");

    EXPECT_EQ(r.outputs[2].location, 2u);
    EXPECT_EQ(r.outputs[2].base, OutputBaseType::Int);
    EXPECT_EQ(r.outputs[2].components, 2u);
    EXPECT_EQ(r.outputs[2].name, "o_Offset");

    EXPECT_TRUE(r.writesDepth);
}

TEST(ShaderReflection, OutputsOfOtherStagesAreNotColourOutputs) {
    ShaderReflection r;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ReferenceModule(ModelVertex).Words(), r, &error)) << error;
    EXPECT_TRUE(r.outputs.empty());
    EXPECT_FALSE(r.writesDepth);
    EXPECT_EQ(r.bindings.size(), 6u);
    for (const ShaderBinding& b : r.bindings) {
        EXPECT_EQ(b.stages, static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT)) << b.name;
    }
}

TEST(ShaderReflection, ReportsBlockLayouts) {
    ShaderReflection r;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ReferenceModule().Words(), r, &error)) << error;

    // FrameBlock, Item, ItemBuffer, LegacyBuffer carry offsets; "Local" does not.
    EXPECT_EQ(r.blocks.size(), 4u);
    EXPECT_EQ(r.FindBlock("Local"), nullptr);
    EXPECT_EQ(r.FindBlock("NoSuchBlock"), nullptr);

    const BlockLayout* frame = r.FindBlock("FrameBlock");
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->members.size(), 2u);
    EXPECT_EQ(frame->members[0].name, "first");
    EXPECT_EQ(frame->members[0].offset, 0u);
    EXPECT_EQ(frame->members[1].name, "second");
    EXPECT_EQ(frame->members[1].offset, 16u);
    EXPECT_EQ(frame->arrayStride, 0u);

    // The element struct of `Item items[]` knows its stride in that array.
    const BlockLayout* item = r.FindBlock("Item");
    ASSERT_NE(item, nullptr);
    ASSERT_EQ(item->members.size(), 2u);
    EXPECT_EQ(item->members[0].name, "p");
    EXPECT_EQ(item->members[1].name, "q");
    EXPECT_EQ(item->members[1].offset, 16u);
    EXPECT_EQ(item->arrayStride, 32u);

    const BlockLayout* buffer = r.FindBlock("ItemBuffer");
    ASSERT_NE(buffer, nullptr);
    ASSERT_EQ(buffer->members.size(), 1u);
    EXPECT_EQ(buffer->members[0].name, "items");
    EXPECT_EQ(buffer->members[0].offset, 0u);
    EXPECT_EQ(buffer->arrayStride, 0u);

    // An array of scalars gives no struct a stride.
    const BlockLayout* legacy = r.FindBlock("LegacyBuffer");
    ASSERT_NE(legacy, nullptr);
    EXPECT_EQ(legacy->arrayStride, 0u);
}

TEST(ShaderReflection, UnnamedVariablesAndOtherBuiltIns) {
    // A module without OpName for the variable or OpMemberName for the member (stripped debug
    // info), an output decorated with a built-in other than FragDepth and an undecorated output.
    constexpr uint32_t kBuiltInSampleMask = 20;
    SpirvBuilder b = EmptyModule();
    b.Name(IdFrameBlock, "FrameBlock");
    b.Decorate(IdFrameBlock, DecBlock);
    b.MemberOffset(IdFrameBlock, 0, 8);
    b.Resource(IdFrameVar, 0, 2);
    b.Decorate(IdFragDepthVar, DecBuiltIn, kBuiltInSampleMask);
    b.Op(OpTypeFloat, {IdFloat, 32});
    b.Op(OpTypeStruct, {IdFrameBlock, IdFloat});
    b.Op(OpTypePointer, {IdFrameBlockPtr, ScUniform, IdFrameBlock});
    b.Op(OpVariable, {IdFrameBlockPtr, IdFrameVar, ScUniform});
    b.Op(OpTypePointer, {IdOutFloatPtr, ScOutput, IdFloat});
    b.Op(OpVariable, {IdOutFloatPtr, IdFragDepthVar, ScOutput});
    b.Op(OpVariable, {IdOutFloatPtr, IdOutColorVar, ScOutput});

    ShaderReflection r;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(b.Words(), r, &error)) << error;

    ASSERT_EQ(r.bindings.size(), 1u);
    EXPECT_EQ(r.bindings[0].type, BindingType::UniformBuffer);
    EXPECT_EQ(r.bindings[0].binding, 2u);
    EXPECT_EQ(r.bindings[0].name, "FrameBlock");        // bound by the block name
    EXPECT_EQ(r.bindings[0].blockName, "FrameBlock");

    const BlockLayout* block = r.FindBlock("FrameBlock");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->members.size(), 1u);
    EXPECT_TRUE(block->members[0].name.empty());
    EXPECT_EQ(block->members[0].offset, 8u);

    EXPECT_TRUE(r.outputs.empty());      // neither output has a Location
    EXPECT_FALSE(r.writesDepth);         // only BuiltIn FragDepth counts
}

TEST(ShaderReflection, IgnoresUnknownInstructions) {
    SpirvBuilder b = ReferenceModule();
    b.Op(OpNop, {});
    b.Op(400, {1, 2, 3, 4, 5});          // an opcode the reflection knows nothing about
    b.Op(0xFFFF, {0xFFFFFFFFu});
    ShaderReflection r;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(b.Words(), r, &error)) << error;
    EXPECT_EQ(r.bindings.size(), 6u);
    EXPECT_EQ(r.outputs.size(), 3u);
}

TEST(ShaderReflection, IsDeterministic) {
    const std::vector<uint32_t> words = ReferenceModule().Words();
    ShaderReflection a, b;
    ASSERT_TRUE(ReflectSpirv(words, a));
    ASSERT_TRUE(ReflectSpirv(words, b));
    ASSERT_EQ(a.bindings.size(), b.bindings.size());
    for (size_t i = 0; i < a.bindings.size(); ++i) {
        EXPECT_EQ(a.bindings[i].name, b.bindings[i].name);
        EXPECT_EQ(a.bindings[i].binding, b.bindings[i].binding);
    }
    ASSERT_EQ(a.blocks.size(), b.blocks.size());
    for (size_t i = 0; i < a.blocks.size(); ++i) EXPECT_EQ(a.blocks[i].blockName, b.blocks[i].blockName);
}

// -------------------------------------------------------------------------------------------------
//  Malformed modules
// -------------------------------------------------------------------------------------------------

TEST(ShaderReflection, RejectsEmptyAndTruncatedHeaders) {
    ExpectRejected({}, "empty");
    ExpectRejected({kMagic}, "magic only");
    ExpectRejected({kMagic, 0x00010600u, 0u, 8u}, "four header words");
}

TEST(ShaderReflection, RejectsBadMagic) {
    std::vector<uint32_t> words = EmptyModule().Words();
    ShaderReflection ok;
    ASSERT_TRUE(ReflectSpirv(words, ok));

    words[0] = 0x03022307u;   // the magic number of a byte-swapped module
    ExpectRejected(words, "byte-swapped magic");
    words[0] = 0u;
    ExpectRejected(words, "zero magic");
    words[0] = kMagic + 1;
    ExpectRejected(words, "off-by-one magic");
}

TEST(ShaderReflection, RejectsZeroWordCount) {
    SpirvBuilder b = EmptyModule();
    b.Words().push_back(OpName);          // word count 0 would never advance
    b.Words().push_back(0u);
    ExpectRejected(b.Words(), "zero word count");
}

TEST(ShaderReflection, RejectsTruncatedInstruction) {
    SpirvBuilder b = EmptyModule();
    b.Words().push_back((4u << 16) | OpTypeVector);   // announces 4 words, 2 follow
    b.Words().push_back(2u);
    ExpectRejected(b.Words(), "instruction runs past the end");

    SpirvBuilder huge = EmptyModule();
    huge.Words().push_back((0xFFFFu << 16) | OpDecorate);
    ExpectRejected(huge.Words(), "maximum word count at the end");

    // Cutting a valid module anywhere inside its last instruction.
    std::vector<uint32_t> cut = ReferenceModule().Words();
    cut.pop_back();
    ExpectRejected(cut, "last instruction cut short");
}

TEST(ShaderReflection, RejectsInstructionsTooShortForTheirOpcode) {
    {
        SpirvBuilder b = EmptyModule();
        b.Op(OpDecorate, {IdFrameVar, DecBinding});       // Binding without its operand
        ExpectRejected(b.Words(), "OpDecorate Binding without value");
    }
    {
        SpirvBuilder b = EmptyModule();
        b.Op(OpMemberDecorate, {IdFrameBlock, 0, DecOffset});
        ExpectRejected(b.Words(), "OpMemberDecorate Offset without value");
    }
    {
        SpirvBuilder b = EmptyModule();
        b.Op(OpTypeImage, {IdImage, IdFloat, 1, 0});
        ExpectRejected(b.Words(), "short OpTypeImage");
    }
    {
        SpirvBuilder b = EmptyModule();
        b.Op(OpVariable, {IdFrameBlockPtr, IdFrameVar});
        ExpectRejected(b.Words(), "short OpVariable");
    }
    {
        SpirvBuilder b = EmptyModule();
        b.Op(OpTypePointer, {IdFrameBlockPtr});
        ExpectRejected(b.Words(), "short OpTypePointer");
    }
    {
        SpirvBuilder b;
        b.Op(OpEntryPoint, {ModelFragment, IdMain});      // no name
        ExpectRejected(b.Words(), "short OpEntryPoint");
    }
}

TEST(ShaderReflection, RejectsUnterminatedStrings) {
    {
        SpirvBuilder b = EmptyModule();
        b.Op(OpName, {IdFrameVar, 0x41414141u, 0x42424242u});   // "AAAABBBB" without terminator
        ExpectRejected(b.Words(), "OpName without terminator");
    }
    {
        SpirvBuilder b;
        b.Op(OpEntryPoint, {ModelFragment, IdMain, 0x6E69616Du});   // "main" without terminator
        ExpectRejected(b.Words(), "OpEntryPoint without terminator");
    }
}

TEST(ShaderReflection, RejectsModulesItCannotDescribe) {
    {
        SpirvBuilder b;   // header only
        ExpectRejected(b.Words(), "no entry point");
    }
    ExpectRejected(EmptyModule(ModelGeometry).Words(), "unsupported execution model");
    {
        // A uniform block without set / binding cannot be put into a descriptor set layout.
        SpirvBuilder b = EmptyModule();
        b.Decorate(IdFrameBlock, DecBlock);
        b.Op(OpTypeFloat, {IdFloat, 32});
        b.Op(OpTypeStruct, {IdFrameBlock, IdFloat});
        b.Op(OpTypePointer, {IdFrameBlockPtr, ScUniform, IdFrameBlock});
        b.Op(OpVariable, {IdFrameBlockPtr, IdFrameVar, ScUniform});
        ExpectRejected(b.Words(), "resource without binding");
    }
    {
        // A resource variable whose type is never defined.
        SpirvBuilder b = EmptyModule();
        b.Resource(IdFrameVar, 0, 0);
        b.Op(OpVariable, {IdFrameBlockPtr, IdFrameVar, ScUniform});
        ExpectRejected(b.Words(), "variable with undefined type");
    }
    {
        // A separate sampler is not something the renderer binds.
        SpirvBuilder b = EmptyModule();
        b.Resource(IdTexVar, 0, 1);
        b.Op(OpTypeSampler, {IdImage});
        b.Op(OpTypePointer, {IdSampledImagePtr, ScUniformConstant, IdImage});
        b.Op(OpVariable, {IdSampledImagePtr, IdTexVar, ScUniformConstant});
        ExpectRejected(b.Words(), "separate sampler");
    }
    {
        // Two resources on the same binding.
        SpirvBuilder b = ReferenceModule();
        b.Decorate(IdTexArrayVar, DecBinding, 16);
        ExpectRejected(b.Words(), "duplicate binding");
    }
    {
        // A type that refers to itself must not hang the reflection.
        SpirvBuilder b = EmptyModule();
        b.Resource(IdTexVar, 0, 1);
        b.Op(OpConstant, {IdUint, IdThree, 3});
        b.Op(OpTypeArray, {IdTexArray, IdTexArray, IdThree});
        b.Op(OpTypePointer, {IdTexArrayPtr, ScUniformConstant, IdTexArray});
        b.Op(OpVariable, {IdTexArrayPtr, IdTexVar, ScUniformConstant});
        ExpectRejected(b.Words(), "self-referential array");
    }
}

TEST(ShaderReflection, SurvivesArbitraryCorruption) {
    const std::vector<uint32_t> reference = ReferenceModule().Words();
    ShaderReflection r;
    std::string error;

    // Every prefix of a valid module: accepted or rejected, never a crash or a hang.
    for (size_t length = 0; length <= reference.size(); ++length) {
        const std::vector<uint32_t> prefix(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(length));
        const bool ok = ReflectSpirv(prefix, r, &error);
        if (!ok) {
            EXPECT_FALSE(error.empty()) << "prefix " << length;
            EXPECT_TRUE(r.bindings.empty()) << "prefix " << length;
        }
    }

    // Every word replaced by extreme values, one at a time.
    const uint32_t patterns[] = {0u, 1u, 0xFFFFFFFFu, 0x0001FFFFu, 0xFFFF0000u, 0x7FFFFFFFu};
    for (size_t i = 0; i < reference.size(); ++i) {
        for (const uint32_t pattern : patterns) {
            std::vector<uint32_t> words = reference;
            words[i] = pattern;
            const bool ok = ReflectSpirv(words, r, &error);
            if (!ok) EXPECT_FALSE(error.empty()) << "word " << i << " = " << pattern;
        }
    }

    // A deterministic pseudo-random scramble of the body.
    uint32_t state = 0x1234ABCDu;
    for (int round = 0; round < 200; ++round) {
        std::vector<uint32_t> words = reference;
        for (int k = 0; k < 4; ++k) {
            state = state * 1664525u + 1013904223u;
            const size_t index = 5 + (state >> 8) % (words.size() - 5);
            state = state * 1664525u + 1013904223u;
            words[index] = state;
        }
        ReflectSpirv(words, r, &error);
    }
    SUCCEED();
}

// -------------------------------------------------------------------------------------------------
//  Files
// -------------------------------------------------------------------------------------------------

TEST(SpirvFile, MissingFileGivesEmptyVector) {
    EXPECT_TRUE(LoadSpirvFile(TempFile("vortex_reflection_no_such_file.spv")).empty());
    EXPECT_TRUE(LoadSpirvFile(std::filesystem::temp_directory_path()).empty());   // a directory
    EXPECT_TRUE(LoadSpirvFile({}).empty());
}

TEST(SpirvFile, RoundTripsAValidModule) {
    const std::vector<uint32_t> words = ReferenceModule().Words();
    const std::filesystem::path path = TempFile("vortex_reflection_roundtrip.spv");
    WriteBytes(path, words.data(), words.size() * sizeof(uint32_t));

    const std::vector<uint32_t> loaded = LoadSpirvFile(path);
    EXPECT_EQ(loaded, words);

    ShaderReflection r;
    std::string error;
    EXPECT_TRUE(ReflectSpirv(loaded, r, &error)) << error;
    EXPECT_EQ(r.bindings.size(), 6u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SpirvFile, RejectsFilesThatAreNotSpirv) {
    const std::filesystem::path path = TempFile("vortex_reflection_not_spirv.spv");
    std::error_code ec;

    const std::string text = "#version 460\nvoid main() {}\n....";   // 32 bytes of GLSL, not SPIR-V
    ASSERT_EQ(text.size() % 4, 0u);
    WriteBytes(path, text.data(), text.size());
    EXPECT_TRUE(LoadSpirvFile(path).empty());

    // Right magic, but not a whole number of words.
    std::vector<uint32_t> words = EmptyModule().Words();
    WriteBytes(path, words.data(), words.size() * sizeof(uint32_t) - 1);
    EXPECT_TRUE(LoadSpirvFile(path).empty());

    // Shorter than a header.
    WriteBytes(path, words.data(), 3 * sizeof(uint32_t));
    EXPECT_TRUE(LoadSpirvFile(path).empty());

    // Empty file.
    WriteBytes(path, words.data(), 0);
    EXPECT_TRUE(LoadSpirvFile(path).empty());

    std::filesystem::remove(path, ec);
}

// -------------------------------------------------------------------------------------------------
//  Pipeline object without a device
// -------------------------------------------------------------------------------------------------

TEST(Pipeline, DefaultStateIsInvalidAndDestroyIsIdempotent) {
    Pipeline pipeline;
    EXPECT_FALSE(pipeline.IsValid());
    EXPECT_TRUE(pipeline.Bindings().empty());
    EXPECT_EQ(pipeline.FindBinding("u_Frame"), nullptr);
    EXPECT_TRUE(pipeline.Handle() == VK_NULL_HANDLE);
    EXPECT_TRUE(pipeline.Layout() == VK_NULL_HANDLE);
    EXPECT_TRUE(pipeline.SetLayout() == VK_NULL_HANDLE);

    pipeline.Destroy();
    pipeline.Destroy();
    EXPECT_FALSE(pipeline.IsValid());
}

TEST(Pipeline, DescriptionDefaultsMatchTheRenderConventions) {
    const PipelineDesc desc;
    EXPECT_EQ(desc.depthCompare, VK_COMPARE_OP_GREATER);              // reverse-Z
    EXPECT_EQ(desc.frontFace, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    EXPECT_EQ(desc.topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    EXPECT_EQ(desc.depthFormat, VK_FORMAT_UNDEFINED);
    EXPECT_FALSE(desc.depthTest);
    EXPECT_FALSE(desc.depthWrite);
    EXPECT_TRUE(desc.colorFormats.empty());
    EXPECT_TRUE(desc.blend.empty());
}
