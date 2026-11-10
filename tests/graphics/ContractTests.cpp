// Contract tests between the renderer (C++) and the shaders (GLSL), checked on the real SPIR-V
// binaries the build produces - ARCHITECTURE.md section 5.6 and shaders/include/common.glsl:
// binding numbers / names / types, fragment outputs, uniform block and struct layouts, the
// proxy cube's winding and the reverse-Z projection. No Vulkan device is created.

#include <gtest/gtest.h>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "shared/vx_cpp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.graphics;

using namespace vortex;
using namespace vortex::graphics;

namespace cube {
    // GLSL vocabulary for the shared code below (see shared/vx_cpp.h).
    using namespace glm;
    using uint = unsigned int;

    /// shaders/shared/cube.glsl compiled as C++: the very text the vertex shader runs.
    struct ProxyCube {
#include "shared/cube.glsl"
    };
}

namespace {

    // ---------------------------------------------------------------------------------------------
    //  The contract (ARCHITECTURE.md 5.2 and 5.6)
    // ---------------------------------------------------------------------------------------------
    struct BufferSpec {
        std::string_view name;
        uint32_t binding;
        BindingType type;
        std::string_view blockName;   ///< As declared in common.glsl.
    };

    const BufferSpec kBuffers[] = {
        {"u_Frame", 0, BindingType::UniformBuffer, "FrameUniforms"},
        {"u_Grid", 1, BindingType::StorageBuffer, "WorldGridBuffer"},
        {"u_Occ", 2, BindingType::StorageBuffer, "ChunkOccBuffer"},
        {"u_Ids", 3, BindingType::StorageBuffer, "ChunkIdBuffer"},
        {"u_Materials", 4, BindingType::StorageBuffer, "MaterialBuffer"},
        {"u_Instances", 5, BindingType::StorageBuffer, "InstanceBuffer"},
        {"u_Bvh", 6, BindingType::StorageBuffer, "BvhBuffer"},
        {"u_DrawList", 7, BindingType::StorageBuffer, "DrawListBuffer"},
        {"u_Lights", 8, BindingType::StorageBuffer, "LightBuffer"},
        {"u_Particles", 9, BindingType::StorageBuffer, "ParticleBuffer"},
    };
    constexpr uint32_t kFirstTextureBinding = 16;

    const BufferSpec* FindBufferSpec(std::string_view name) {
        for (const BufferSpec& spec : kBuffers) {
            if (spec.name == name) return &spec;
        }
        return nullptr;
    }

    struct TextureSpec {
        uint32_t binding;
        std::string name;
    };

    struct PipelineSpec {
        std::string name;
        std::string vertex;
        std::string fragment;
        std::vector<std::string> buffers;     ///< Union of both stages.
        std::vector<TextureSpec> textures;    ///< Declared by the fragment shader.
    };

    const std::vector<std::string> kTraceBuffers = {"u_Frame", "u_Grid", "u_Occ", "u_Ids", "u_Instances", "u_Bvh"};

    std::vector<std::string> With(std::vector<std::string> base, std::initializer_list<const char*> extra) {
        for (const char* e : extra) base.emplace_back(e);
        return base;
    }

    const std::vector<PipelineSpec>& Pipelines() {
        static const std::vector<PipelineSpec> pipelines = {
            {"gbuffer_world", "fullscreen.vert", "gbuffer_world.frag", kTraceBuffers, {}},
            {"gbuffer_object_front", "gbuffer_object.vert", "gbuffer_object_front.frag", With(kTraceBuffers, {"u_DrawList"}), {}},
            {"gbuffer_object_inside", "gbuffer_object.vert", "gbuffer_object_inside.frag", With(kTraceBuffers, {"u_DrawList"}), {}},
            {"lighting", "fullscreen.vert", "lighting.frag", With(kTraceBuffers, {"u_Materials", "u_Lights"}),
             {{16, "u_GMaterial"}, {17, "u_GNormal"}, {18, "u_GDepth"}, {19, "u_GVelocity"},
              {20, "u_PrevLighting"}, {21, "u_PrevLightCount"}, {22, "u_PrevDepth"}, {23, "u_PrevNormal"}}},
            {"composite", "fullscreen.vert", "composite.frag", {"u_Frame", "u_Materials"},
             {{16, "u_GMaterial"}, {17, "u_GNormal"}, {18, "u_GDepth"}, {19, "u_Lighting"}, {20, "u_LightCount"}}},
            {"particles", "particles.vert", "particles.frag", {"u_Frame", "u_Particles"}, {}},
            {"taa", "fullscreen.vert", "taa.frag", {"u_Frame"},
             {{16, "u_Color"}, {17, "u_History"}, {18, "u_GVelocity"}, {19, "u_GDepth"}}},
            {"present", "fullscreen.vert", "present.frag", {"u_Frame"}, {{16, "u_Color"}}},
        };
        return pipelines;
    }

    const char* const kShaderFiles[] = {
        "fullscreen.vert", "gbuffer_world.frag", "gbuffer_object.vert", "gbuffer_object_front.frag",
        "gbuffer_object_inside.frag", "lighting.frag", "composite.frag", "particles.vert",
        "particles.frag", "taa.frag", "present.frag",
    };

    bool IsFragment(std::string_view file) { return file.ends_with(".frag"); }

    // ---------------------------------------------------------------------------------------------
    //  Loading
    // ---------------------------------------------------------------------------------------------
    struct LoadedShader {
        bool ok = false;
        std::string problem;
        ShaderReflection reflection;
    };

    /// Loads and reflects "<VORTEX_TEST_SHADER_DIR>/<file>.spv" once per test run.
    const LoadedShader& Load(const std::string& file) {
        static std::map<std::string, LoadedShader> cache;
        const auto it = cache.find(file);
        if (it != cache.end()) return it->second;

        LoadedShader shader;
        const std::filesystem::path path = std::filesystem::path(VORTEX_TEST_SHADER_DIR) / (file + ".spv");
        const std::vector<uint32_t> words = LoadSpirvFile(path);
        if (words.empty()) {
            shader.problem = "cannot load " + path.string();
        } else {
            std::string error;
            shader.ok = ReflectSpirv(words, shader.reflection, &error);
            if (!shader.ok) shader.problem = "cannot reflect " + path.string() + ": " + error;
        }
        return cache.emplace(file, std::move(shader)).first->second;
    }

#define LOAD_SHADER(variable, file)                          \
    const LoadedShader& variable##Loaded = Load(file);       \
    ASSERT_TRUE(variable##Loaded.ok) << variable##Loaded.problem; \
    const ShaderReflection& variable = variable##Loaded.reflection

    bool IsBuffer(const ShaderBinding& b) {
        return b.type == BindingType::UniformBuffer || b.type == BindingType::StorageBuffer;
    }

    std::set<std::string> BufferNames(const ShaderReflection& r) {
        std::set<std::string> names;
        for (const ShaderBinding& b : r.bindings) {
            if (IsBuffer(b)) names.insert(b.name);
        }
        return names;
    }

    /// First shader (in kShaderFiles order) that declares the given block or struct.
    const BlockLayout* FindBlockInAnyShader(std::string_view name, bool needStride) {
        for (const char* file : kShaderFiles) {
            const LoadedShader& shader = Load(file);
            if (!shader.ok) continue;
            const BlockLayout* block = shader.reflection.FindBlock(name);
            if (block && (!needStride || block->arrayStride != 0)) return block;
        }
        return nullptr;
    }

    void ExpectMember(const BlockLayout& layout, std::string_view member, size_t expectedOffset) {
        const auto it = std::find_if(layout.members.begin(), layout.members.end(),
                                     [&](const BlockMember& m) { return m.name == member; });
        if (it == layout.members.end()) {
            ADD_FAILURE() << layout.blockName << " has no member '" << member << "'";
            return;
        }
        EXPECT_EQ(it->offset, expectedOffset) << layout.blockName << "." << member;
    }

    void ExpectOutput(const ShaderReflection& r, uint32_t location, OutputBaseType base, uint32_t components,
                      std::string_view file) {
        const auto it = std::find_if(r.outputs.begin(), r.outputs.end(),
                                     [&](const ShaderOutput& o) { return o.location == location; });
        if (it == r.outputs.end()) {
            ADD_FAILURE() << file << " has no output at location " << location;
            return;
        }
        EXPECT_EQ(it->base, base) << file << " output " << location;
        EXPECT_EQ(it->components, components) << file << " output " << location;
    }

    // ---------------------------------------------------------------------------------------------
    //  Geometry helpers
    // ---------------------------------------------------------------------------------------------
    constexpr int kCubeVertices = 36;

    /// Signed area of a triangle exactly as Vulkan computes it to decide facing (specification,
    /// "Basic Polygon Rasterization"): framebuffer coordinates, x to the right, y down. A positive
    /// area is front-facing for VK_FRONT_FACE_COUNTER_CLOCKWISE.
    float VulkanFacingArea(const glm::mat4& viewProj, const glm::vec3 triangle[3], const glm::vec2& viewport) {
        glm::vec2 fb[3];
        for (int i = 0; i < 3; ++i) {
            const glm::vec4 clip = viewProj * glm::vec4(triangle[i], 1.0f);
            EXPECT_GT(clip.w, 0.0f) << "test geometry must be in front of the camera";
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            fb[i] = (ndc * 0.5f + 0.5f) * viewport;
        }
        float sum = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            sum += fb[i].x * fb[j].y - fb[j].x * fb[i].y;
        }
        return -0.5f * sum;
    }
}

// -------------------------------------------------------------------------------------------------
//  Shader binaries
// -------------------------------------------------------------------------------------------------

TEST(ShaderContract, EveryShaderExistsAndReflects) {
    for (const char* file : kShaderFiles) {
        const LoadedShader& shader = Load(file);
        ASSERT_TRUE(shader.ok) << shader.problem;
        EXPECT_EQ(shader.reflection.entryPoint, "main") << file;
        EXPECT_EQ(shader.reflection.stage, IsFragment(file) ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT) << file;
    }
}

TEST(ShaderContract, EveryPipelineShaderIsListed) {
    const std::set<std::string> files(std::begin(kShaderFiles), std::end(kShaderFiles));
    for (const PipelineSpec& pipeline : Pipelines()) {
        EXPECT_TRUE(files.contains(pipeline.vertex)) << pipeline.vertex;
        EXPECT_TRUE(files.contains(pipeline.fragment)) << pipeline.fragment;
        EXPECT_FALSE(IsFragment(pipeline.vertex)) << pipeline.name;
        EXPECT_TRUE(IsFragment(pipeline.fragment)) << pipeline.name;
    }
}

TEST(ShaderContract, AllBindingsAreInSetZeroAndNamed) {
    for (const char* file : kShaderFiles) {
        LOAD_SHADER(r, file);
        std::set<std::string> names;
        std::set<uint32_t> numbers;
        for (const ShaderBinding& b : r.bindings) {
            EXPECT_EQ(b.set, 0u) << file << ": " << b.name;
            EXPECT_FALSE(b.name.empty()) << file << ": binding " << b.binding;
            EXPECT_EQ(b.count, 1u) << file << ": " << b.name;
            EXPECT_TRUE(names.insert(b.name).second) << file << ": duplicate name " << b.name;
            EXPECT_TRUE(numbers.insert(b.binding).second) << file << ": duplicate binding " << b.binding;
            EXPECT_EQ(b.stages, static_cast<VkShaderStageFlags>(r.stage)) << file << ": " << b.name;
        }
    }
}

TEST(ShaderContract, BuffersUseTheCommonBindingTable) {
    for (const char* file : kShaderFiles) {
        LOAD_SHADER(r, file);
        for (const ShaderBinding& b : r.bindings) {
            if (!IsBuffer(b)) {
                EXPECT_EQ(b.type, BindingType::CombinedImageSampler) << file << ": " << b.name;
                EXPECT_GE(b.binding, kFirstTextureBinding) << file << ": texture " << b.name;
                EXPECT_TRUE(b.blockName.empty()) << file << ": " << b.name;
                continue;
            }
            const BufferSpec* spec = FindBufferSpec(b.name);
            if (!spec) {
                ADD_FAILURE() << file << " declares a buffer '" << b.name << "' (binding " << b.binding
                              << ") that is not part of the contract";
                continue;
            }
            EXPECT_EQ(b.binding, spec->binding) << file << ": " << b.name;
            EXPECT_EQ(b.type, spec->type) << file << ": " << b.name;
            EXPECT_EQ(b.blockName, spec->blockName) << file << ": " << b.name;
            EXPECT_LT(b.binding, kFirstTextureBinding) << file << ": " << b.name;
        }
        // Looking a buffer up by name gives the same binding the table promises.
        for (const BufferSpec& spec : kBuffers) {
            const ShaderBinding* b = r.FindBinding(spec.name);
            if (b) EXPECT_EQ(b->binding, spec.binding) << file << ": " << spec.name;
        }
    }
}

TEST(ShaderContract, EveryBufferOfTheTableIsUsedSomewhere) {
    std::set<std::string> seen;
    for (const char* file : kShaderFiles) {
        LOAD_SHADER(r, file);
        const std::set<std::string> names = BufferNames(r);
        seen.insert(names.begin(), names.end());
    }
    for (const BufferSpec& spec : kBuffers) {
        EXPECT_TRUE(seen.contains(std::string(spec.name))) << spec.name << " is not declared by any shader";
    }
}

TEST(ShaderContract, PipelinesDeclareExactlyTheirBuffers) {
    for (const PipelineSpec& pipeline : Pipelines()) {
        LOAD_SHADER(vertex, pipeline.vertex);
        LOAD_SHADER(fragment, pipeline.fragment);
        const std::set<std::string> expected(pipeline.buffers.begin(), pipeline.buffers.end());

        // Each stage may only declare buffers the renderer binds for this pipeline ...
        std::set<std::string> declared;
        for (const ShaderReflection* stage : {&vertex, &fragment}) {
            for (const std::string& name : BufferNames(*stage)) {
                EXPECT_TRUE(expected.contains(name)) << pipeline.name << ": unexpected buffer " << name;
                declared.insert(name);
            }
        }
        // ... and together they declare all of them (DescriptorWriter rejects unknown names).
        for (const std::string& name : expected) {
            EXPECT_TRUE(declared.contains(name)) << pipeline.name << ": buffer " << name << " is not declared by either stage";
        }
        // The fragment shaders of 5.6 declare their whole buffer list themselves, except where
        // the list is shared with a dedicated vertex shader.
        if (pipeline.vertex == "fullscreen.vert") {
            EXPECT_EQ(BufferNames(fragment), expected) << pipeline.fragment;
        }
    }

    // Rows of the resource table that name a single stage.
    {
        LOAD_SHADER(r, "gbuffer_object.vert");
        EXPECT_EQ(BufferNames(r), (std::set<std::string>{"u_Frame", "u_Instances", "u_DrawList"}));
    }
    for (const char* file : {"gbuffer_object_front.frag", "gbuffer_object_inside.frag"}) {
        LOAD_SHADER(r, file);
        EXPECT_EQ(BufferNames(r), std::set<std::string>(kTraceBuffers.begin(), kTraceBuffers.end())) << file;
    }
}

TEST(ShaderContract, PipelinesDeclareExactlyTheirTextures) {
    for (const PipelineSpec& pipeline : Pipelines()) {
        LOAD_SHADER(vertex, pipeline.vertex);
        LOAD_SHADER(fragment, pipeline.fragment);

        for (const ShaderBinding& b : vertex.bindings) {
            EXPECT_TRUE(IsBuffer(b)) << pipeline.vertex << " must not declare textures (" << b.name << ")";
        }

        size_t textureCount = 0;
        for (const ShaderBinding& b : fragment.bindings) {
            if (!IsBuffer(b)) ++textureCount;
        }
        EXPECT_EQ(textureCount, pipeline.textures.size()) << pipeline.fragment;

        for (const TextureSpec& texture : pipeline.textures) {
            const ShaderBinding* b = fragment.FindBinding(texture.name);
            if (!b) {
                ADD_FAILURE() << pipeline.fragment << " does not declare texture " << texture.name;
                continue;
            }
            EXPECT_EQ(b->type, BindingType::CombinedImageSampler) << pipeline.fragment << ": " << texture.name;
            EXPECT_EQ(b->binding, texture.binding) << pipeline.fragment << ": " << texture.name;
            EXPECT_EQ(b->set, 0u) << pipeline.fragment << ": " << texture.name;
            EXPECT_EQ(b->count, 1u) << pipeline.fragment << ": " << texture.name;
        }
    }
}

TEST(ShaderContract, StagesOfAPipelineAgreeOnSharedBindings) {
    // What Pipeline::Create requires in order to merge the two stages into one set layout.
    for (const PipelineSpec& pipeline : Pipelines()) {
        LOAD_SHADER(vertex, pipeline.vertex);
        LOAD_SHADER(fragment, pipeline.fragment);
        for (const ShaderBinding& v : vertex.bindings) {
            for (const ShaderBinding& f : fragment.bindings) {
                if (v.binding == f.binding) {
                    EXPECT_EQ(v.type, f.type) << pipeline.name << ": binding " << v.binding;
                    EXPECT_EQ(v.name, f.name) << pipeline.name << ": binding " << v.binding;
                    EXPECT_EQ(v.count, f.count) << pipeline.name << ": binding " << v.binding;
                } else {
                    EXPECT_NE(v.name, f.name) << pipeline.name << ": one name on two bindings";
                }
            }
        }
    }
}

TEST(ShaderContract, GBufferShadersWriteTheGBuffer) {
    for (const char* file : {"gbuffer_world.frag", "gbuffer_object_front.frag", "gbuffer_object_inside.frag"}) {
        LOAD_SHADER(r, file);
        EXPECT_EQ(r.outputs.size(), 3u) << file;
        ExpectOutput(r, 0, OutputBaseType::UInt, 1, file);     // R32_UINT material
        ExpectOutput(r, 1, OutputBaseType::Float, 4, file);    // A2B10G10R10 normal
        ExpectOutput(r, 2, OutputBaseType::Float, 2, file);    // R16G16_SFLOAT velocity
        EXPECT_TRUE(r.writesDepth) << file;
    }
}

TEST(ShaderContract, LightingWritesIrradianceAndCount) {
    LOAD_SHADER(r, "lighting.frag");
    EXPECT_EQ(r.outputs.size(), 2u);
    ExpectOutput(r, 0, OutputBaseType::Float, 4, "lighting.frag");   // RGBA16F
    ExpectOutput(r, 1, OutputBaseType::Float, 1, "lighting.frag");   // R8_UNORM
    EXPECT_FALSE(r.writesDepth) << "the lighting pass has no depth attachment";
}

TEST(ShaderContract, SingleTargetPassesWriteOneFloatColour) {
    for (const char* file : {"composite.frag", "particles.frag", "taa.frag", "present.frag"}) {
        LOAD_SHADER(r, file);
        ASSERT_EQ(r.outputs.size(), 1u) << file;
        EXPECT_EQ(r.outputs[0].location, 0u) << file;
        EXPECT_EQ(r.outputs[0].base, OutputBaseType::Float) << file;
        EXPECT_EQ(r.outputs[0].components, 4u) << file;
    }
    // These passes render without a depth attachment, so they cannot write depth.
    for (const char* file : {"composite.frag", "taa.frag", "present.frag"}) {
        LOAD_SHADER(r, file);
        EXPECT_FALSE(r.writesDepth) << file;
    }
}

TEST(ShaderContract, VertexShadersHaveNoColourOutputs) {
    for (const char* file : kShaderFiles) {
        if (IsFragment(file)) continue;
        LOAD_SHADER(r, file);
        EXPECT_TRUE(r.outputs.empty()) << file;
        EXPECT_FALSE(r.writesDepth) << file;
    }
}

// -------------------------------------------------------------------------------------------------
//  C++ / GLSL layout parity
// -------------------------------------------------------------------------------------------------

TEST(LayoutContract, FrameUniformsMatchTheGlslBlock) {
    int checked = 0;
    for (const char* file : kShaderFiles) {
        LOAD_SHADER(r, file);
        const ShaderBinding* binding = r.FindBinding("u_Frame");
        if (!binding) continue;
        ++checked;
        const BlockLayout* blockPtr = r.FindBlock(binding->blockName);
        ASSERT_NE(blockPtr, nullptr) << file;
        const BlockLayout& block = *blockPtr;
        SCOPED_TRACE(file);

        EXPECT_EQ(block.members.size(), 24u);
        EXPECT_EQ(block.arrayStride, 0u);
#define VX_EXPECT_FRAME_MEMBER(member) ExpectMember(block, #member, offsetof(FrameUniforms, member))
        VX_EXPECT_FRAME_MEMBER(viewProj);
        VX_EXPECT_FRAME_MEMBER(viewProjNoJitter);
        VX_EXPECT_FRAME_MEMBER(prevViewProjNoJitter);
        VX_EXPECT_FRAME_MEMBER(cameraPos);
        VX_EXPECT_FRAME_MEMBER(cameraRight);
        VX_EXPECT_FRAME_MEMBER(cameraUp);
        VX_EXPECT_FRAME_MEMBER(cameraForward);
        VX_EXPECT_FRAME_MEMBER(cameraPosVoxel);
        VX_EXPECT_FRAME_MEMBER(renderSize);
        VX_EXPECT_FRAME_MEMBER(outputSize);
        VX_EXPECT_FRAME_MEMBER(jitter);
        VX_EXPECT_FRAME_MEMBER(sunDirection);
        VX_EXPECT_FRAME_MEMBER(sunRadiance);
        VX_EXPECT_FRAME_MEMBER(skyZenith);
        VX_EXPECT_FRAME_MEMBER(skyHorizon);
        VX_EXPECT_FRAME_MEMBER(skyGround);
        VX_EXPECT_FRAME_MEMBER(fogColor);
        VX_EXPECT_FRAME_MEMBER(fogParams);
        VX_EXPECT_FRAME_MEMBER(ambientParams);
        VX_EXPECT_FRAME_MEMBER(temporalParams);
        VX_EXPECT_FRAME_MEMBER(gridSize);
        VX_EXPECT_FRAME_MEMBER(gridMinChunk);
        VX_EXPECT_FRAME_MEMBER(counts);
        VX_EXPECT_FRAME_MEMBER(settings);
#undef VX_EXPECT_FRAME_MEMBER

        // The members are declared in the same order as in C++ and the block is as large as the struct.
        for (size_t i = 1; i < block.members.size(); ++i) {
            EXPECT_LT(block.members[i - 1].offset, block.members[i].offset) << block.members[i].name;
        }
        if (!block.members.empty()) {
            EXPECT_EQ(block.members.back().name, "settings");
            EXPECT_EQ(block.members.back().offset + sizeof(glm::ivec4), sizeof(FrameUniforms));
        }
    }
    EXPECT_GT(checked, 0) << "no shader declares u_Frame";
}

TEST(LayoutContract, InstanceMatchesVxInstance) {
    const BlockLayout* layout = FindBlockInAnyShader("VxInstance", true);
    ASSERT_NE(layout, nullptr) << "no shader declares VxInstance instances[]";
    EXPECT_EQ(layout->arrayStride, sizeof(voxel::GpuInstance));
    EXPECT_EQ(layout->members.size(), 5u);
    ExpectMember(*layout, "localToWorld", offsetof(voxel::GpuInstance, localToWorld));
    ExpectMember(*layout, "worldToLocal", offsetof(voxel::GpuInstance, worldToLocal));
    ExpectMember(*layout, "prevLocalToWorld", offsetof(voxel::GpuInstance, prevLocalToWorld));
    ExpectMember(*layout, "boundsMinSlot", offsetof(voxel::GpuInstance, boundsMinSlot));
    ExpectMember(*layout, "boundsMaxPalette", offsetof(voxel::GpuInstance, boundsMaxPalette));
}

TEST(LayoutContract, BvhNodeMatchesVxBvhNode) {
    const BlockLayout* layout = FindBlockInAnyShader("VxBvhNode", true);
    ASSERT_NE(layout, nullptr) << "no shader declares VxBvhNode nodes[]";
    EXPECT_EQ(layout->arrayStride, sizeof(voxel::GpuBvhNode));
    EXPECT_EQ(layout->members.size(), 4u);
    ExpectMember(*layout, "bmin", offsetof(voxel::GpuBvhNode, min));
    ExpectMember(*layout, "a", offsetof(voxel::GpuBvhNode, a));
    ExpectMember(*layout, "bmax", offsetof(voxel::GpuBvhNode, max));
    ExpectMember(*layout, "b", offsetof(voxel::GpuBvhNode, b));
}

TEST(LayoutContract, MaterialMatchesVxMaterial) {
    const BlockLayout* layout = FindBlockInAnyShader("VxMaterial", true);
    ASSERT_NE(layout, nullptr) << "no shader declares VxMaterial materials[]";
    EXPECT_EQ(layout->arrayStride, sizeof(voxel::GpuMaterial));
    EXPECT_EQ(layout->members.size(), 2u);
    ExpectMember(*layout, "colorEmissive", offsetof(voxel::GpuMaterial, colorEmissive));
    ExpectMember(*layout, "params", offsetof(voxel::GpuMaterial, params));
}

TEST(LayoutContract, LightMatchesVxLight) {
    const BlockLayout* layout = FindBlockInAnyShader("VxLight", true);
    ASSERT_NE(layout, nullptr) << "no shader declares VxLight lights[]";
    EXPECT_EQ(layout->arrayStride, sizeof(GpuLight));
    EXPECT_EQ(layout->members.size(), 2u);
    ExpectMember(*layout, "positionRadius", offsetof(GpuLight, positionRadius));
    ExpectMember(*layout, "colorSize", offsetof(GpuLight, colorSize));
}

TEST(LayoutContract, ParticleMatchesVxParticle) {
    const BlockLayout* layout = FindBlockInAnyShader("VxParticle", true);
    ASSERT_NE(layout, nullptr) << "no shader declares VxParticle particles[]";
    EXPECT_EQ(layout->arrayStride, sizeof(fx::GpuParticle));
    EXPECT_EQ(layout->members.size(), 3u);
    ExpectMember(*layout, "positionSize", offsetof(fx::GpuParticle, positionSize));
    ExpectMember(*layout, "color", offsetof(fx::GpuParticle, color));
    ExpectMember(*layout, "params", offsetof(fx::GpuParticle, params));
}

TEST(LayoutContract, StructLayoutsAgreeAcrossShaders) {
    // The same struct must not be laid out differently by two shaders.
    for (const std::string_view name : {"VxInstance", "VxBvhNode", "VxMaterial", "VxLight", "VxParticle", "FrameUniforms"}) {
        const BlockLayout* reference = nullptr;
        for (const char* file : kShaderFiles) {
            LOAD_SHADER(r, file);
            const BlockLayout* layout = r.FindBlock(name);
            if (!layout) continue;
            if (!reference) {
                reference = layout;
                continue;
            }
            ASSERT_EQ(layout->members.size(), reference->members.size()) << file << ": " << name;
            for (size_t i = 0; i < layout->members.size(); ++i) {
                EXPECT_EQ(layout->members[i].name, reference->members[i].name) << file << ": " << name;
                EXPECT_EQ(layout->members[i].offset, reference->members[i].offset) << file << ": " << name;
            }
            EXPECT_EQ(layout->arrayStride, reference->arrayStride) << file << ": " << name;
        }
        EXPECT_NE(reference, nullptr) << name << " is not declared by any shader";
    }
}

TEST(LayoutContract, WordBuffersAreTightlyPacked) {
    // u_Grid / u_Occ / u_Ids / u_DrawList are `uint data[]`: the array starts at offset 0.
    for (const std::string_view blockName : {"WorldGridBuffer", "ChunkOccBuffer", "ChunkIdBuffer", "DrawListBuffer"}) {
        const BlockLayout* layout = FindBlockInAnyShader(blockName, false);
        ASSERT_NE(layout, nullptr) << blockName;
        ASSERT_EQ(layout->members.size(), 1u) << blockName;
        EXPECT_EQ(layout->members[0].offset, 0u) << blockName;
        EXPECT_EQ(layout->arrayStride, 0u) << blockName;
    }
}

// -------------------------------------------------------------------------------------------------
//  Proxy cube (shaders/shared/cube.glsl)
// -------------------------------------------------------------------------------------------------

TEST(ProxyCube, CornersAreUnitCubeCorners) {
    cube::ProxyCube proxy;
    std::set<int> used;
    for (int i = 0; i < kCubeVertices; ++i) {
        const glm::vec3 c = proxy.VxCubeCorner(i);
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_TRUE(c[axis] == 0.0f || c[axis] == 1.0f) << "vertex " << i << " axis " << axis;
        }
        used.insert(static_cast<int>(c.x) + 2 * static_cast<int>(c.y) + 4 * static_cast<int>(c.z));
    }
    EXPECT_EQ(used.size(), 8u) << "all eight corners are used";
}

TEST(ProxyCube, FacesAreInTheDocumentedOrder) {
    cube::ProxyCube proxy;
    const glm::vec3 expected[6] = {
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},  {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f},
    };
    for (int i = 0; i < kCubeVertices; ++i) {
        EXPECT_EQ(proxy.VxCubeFaceNormal(i), expected[i / 6]) << "vertex " << i;
    }
}

TEST(ProxyCube, TrianglesAreCounterClockwiseSeenFromOutside) {
    cube::ProxyCube proxy;
    const glm::vec3 centre(0.5f);
    float faceArea[6] = {};

    for (int t = 0; t < kCubeVertices / 3; ++t) {
        const glm::vec3 v0 = proxy.VxCubeCorner(t * 3 + 0);
        const glm::vec3 v1 = proxy.VxCubeCorner(t * 3 + 1);
        const glm::vec3 v2 = proxy.VxCubeCorner(t * 3 + 2);
        const glm::vec3 cross = glm::cross(v1 - v0, v2 - v0);
        const float doubleArea = glm::length(cross);
        ASSERT_FLOAT_EQ(doubleArea, 1.0f) << "triangle " << t << " is half a unit square";
        const glm::vec3 normal = cross / doubleArea;
        faceArea[t / 2] += 0.5f * doubleArea;

        for (int k = 0; k < 3; ++k) {
            const int vertex = t * 3 + k;
            const glm::vec3 faceNormal = proxy.VxCubeFaceNormal(vertex);
            // Geometric normal (right-hand rule over the winding) == the face's outward normal.
            EXPECT_EQ(normal, faceNormal) << "vertex " << vertex;
            // The vertex lies on that face of the cube ...
            const glm::vec3 corner = proxy.VxCubeCorner(vertex);
            EXPECT_FLOAT_EQ(glm::dot(corner - centre, faceNormal), 0.5f) << "vertex " << vertex;
        }
        // ... and the normal points away from the cube's centre.
        const glm::vec3 centroid = (v0 + v1 + v2) / 3.0f;
        EXPECT_GT(glm::dot(normal, centroid - centre), 0.0f) << "triangle " << t;
    }
    for (int face = 0; face < 6; ++face) {
        EXPECT_FLOAT_EQ(faceArea[face], 1.0f) << "face " << face << " is covered by its two triangles";
    }

    // The two triangles of a face share exactly one edge (the diagonal): 4 distinct corners.
    for (int face = 0; face < 6; ++face) {
        std::set<int> corners;
        for (int k = 0; k < 6; ++k) {
            const glm::vec3 c = proxy.VxCubeCorner(face * 6 + k);
            corners.insert(static_cast<int>(c.x) + 2 * static_cast<int>(c.y) + 4 * static_cast<int>(c.z));
        }
        EXPECT_EQ(corners.size(), 4u) << "face " << face;
    }
}

TEST(ProxyCube, FacesTowardsTheCameraAreVulkanFrontFaces) {
    cube::ProxyCube proxy;
    const glm::vec2 viewport(1280.0f, 720.0f);
    const glm::mat4 proj = math::PerspectiveReverseZ(glm::radians(60.0f), viewport.x / viewport.y, 0.05f);

    struct View {
        glm::vec3 eye;
        glm::vec3 up;
    };
    // Cameras all around the cube, including straight above / below (with a matching up vector).
    const View views[] = {
        {{3.0f, 2.0f, 4.0f}, {0.0f, 1.0f, 0.0f}},    {{-2.5f, 1.7f, 3.1f}, {0.0f, 1.0f, 0.0f}},
        {{3.3f, -1.4f, -2.2f}, {0.0f, 1.0f, 0.0f}},  {{-2.0f, -2.5f, -3.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.4f, 0.6f, 5.0f}, {0.0f, 1.0f, 0.0f}},    {{0.3f, 0.7f, -4.0f}, {0.0f, 1.0f, 0.0f}},
        {{6.0f, 0.45f, 0.55f}, {0.0f, 1.0f, 0.0f}},  {{-5.0f, 0.2f, 0.8f}, {0.0f, 1.0f, 0.0f}},
        {{0.6f, 4.0f, 0.3f}, {0.0f, 0.0f, -1.0f}},   {{0.35f, -3.0f, 0.65f}, {0.0f, 0.0f, 1.0f}},
        {{1.6f, 1.2f, 1.4f}, {0.0f, 1.0f, 0.0f}},
    };

    // The cube as the engine draws it: unit cube -> instance bounds -> render space.
    const glm::mat4 models[] = {
        glm::mat4(1.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.2f, -0.1f, 0.3f)) *
            glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f))) *
            glm::translate(glm::mat4(1.0f), glm::vec3(0.5f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.9f, 0.4f, 0.6f)) *
            glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f)),
    };

    for (const glm::mat4& model : models) {
        const glm::vec3 centre = glm::vec3(model * glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
        for (const View& view : views) {
            const glm::mat4 viewProj = proj * glm::lookAt(view.eye, centre, view.up);
            int frontTriangles = 0;
            int decided = 0;
            for (int t = 0; t < kCubeVertices / 3; ++t) {
                glm::vec3 triangle[3];
                for (int k = 0; k < 3; ++k) {
                    triangle[k] = glm::vec3(model * glm::vec4(proxy.VxCubeCorner(t * 3 + k), 1.0f));
                }
                const glm::vec3 normal = glm::normalize(glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]));
                const float towardsCamera = glm::dot(normal, glm::normalize(view.eye - triangle[0]));
                if (std::abs(towardsCamera) < 0.02f) continue;   // edge-on: facing is numerically undecided
                ++decided;

                const float area = VulkanFacingArea(viewProj, triangle, viewport);
                if (towardsCamera > 0.0f) {
                    EXPECT_GT(area, 0.0f) << "triangle " << t << " faces the camera at (" << view.eye.x << ", "
                                          << view.eye.y << ", " << view.eye.z << ") but is a back face";
                    ++frontTriangles;
                } else {
                    EXPECT_LT(area, 0.0f) << "triangle " << t << " faces away from the camera at (" << view.eye.x
                                          << ", " << view.eye.y << ", " << view.eye.z << ") but is a front face";
                }
            }
            // From outside a convex box one to three faces (two triangles each) are visible.
            EXPECT_GE(decided, 10);
            EXPECT_GE(frontTriangles, 2);
            EXPECT_LE(frontTriangles, 6);
            EXPECT_EQ(frontTriangles % 2, 0);
        }
    }
}

// -------------------------------------------------------------------------------------------------
//  Projection (ARCHITECTURE.md 5.1)
// -------------------------------------------------------------------------------------------------

TEST(ReverseZ, NearPlaneIsOneAndFarPointsApproachZero) {
    const float zNear = 0.05f;
    const glm::mat4 proj = math::PerspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, zNear);

    const auto depthAt = [&](float distance) {
        const glm::vec4 clip = proj * glm::vec4(0.3f * distance, -0.2f * distance, -distance, 1.0f);
        EXPECT_FLOAT_EQ(clip.w, distance);   // w = distance along the view axis
        return clip.z / clip.w;
    };

    EXPECT_FLOAT_EQ(depthAt(zNear), 1.0f);
    EXPECT_FLOAT_EQ(depthAt(2.0f * zNear), 0.5f);
    EXPECT_FLOAT_EQ(depthAt(100.0f * zNear), 0.01f);

    // Strictly decreasing with distance, always inside (0, 1]: GREATER means closer, 0 is the sky.
    float previous = depthAt(zNear);
    for (float distance = zNear * 2.0f; distance < 1.0e6f; distance *= 3.0f) {
        const float depth = depthAt(distance);
        EXPECT_LT(depth, previous) << distance;
        EXPECT_GT(depth, 0.0f) << distance;
        EXPECT_FLOAT_EQ(math::LinearizeReverseZ(depth, zNear), distance);
        previous = depth;
    }
    EXPECT_LT(depthAt(1.0e6f), 1.0e-7f);

    // In front of the near plane the depth leaves the [0, 1] range and is clipped.
    EXPECT_GT(depthAt(0.5f * zNear), 1.0f);
}

TEST(ReverseZ, ImageIsUprightInVulkanClipSpace) {
    const float fovY = glm::radians(70.0f);
    const float aspect = 2.0f;
    const glm::mat4 proj = math::PerspectiveReverseZ(fovY, aspect, 0.1f);
    const float t = std::tan(fovY * 0.5f);

    // A point above the view axis lands in the upper half of the image: negative y in Vulkan NDC.
    const glm::vec4 up = proj * glm::vec4(0.0f, 1.0f, -5.0f, 1.0f);
    EXPECT_LT(up.y / up.w, 0.0f);
    // A point to the right stays on the right.
    const glm::vec4 right = proj * glm::vec4(1.0f, 0.0f, -5.0f, 1.0f);
    EXPECT_GT(right.x / right.w, 0.0f);

    // The edges of the field of view map to the edges of clip space.
    const glm::vec4 topEdge = proj * glm::vec4(0.0f, t * 5.0f, -5.0f, 1.0f);
    EXPECT_NEAR(topEdge.y / topEdge.w, -1.0f, 1.0e-5f);
    const glm::vec4 rightEdge = proj * glm::vec4(t * aspect * 5.0f, 0.0f, -5.0f, 1.0f);
    EXPECT_NEAR(rightEdge.x / rightEdge.w, 1.0f, 1.0e-5f);
}
