module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.graphics;

// SPIR-V reflection: a small, self-contained reader for exactly the parts of a module the
// renderer needs (descriptor bindings, fragment outputs, block member offsets). It never trusts
// the input: every instruction is bounds-checked before its operands are read and all id
// lookups go through maps, so a corrupt file produces an error string, not a crash.

namespace vortex::graphics {

    namespace {

        constexpr uint32_t kSpirvMagic = 0x07230203u;
        constexpr size_t kHeaderWords = 5;

        // Opcodes (SPIR-V specification, section 3.32).
        constexpr uint32_t kOpName = 5;
        constexpr uint32_t kOpMemberName = 6;
        constexpr uint32_t kOpEntryPoint = 15;
        constexpr uint32_t kOpTypeInt = 21;
        constexpr uint32_t kOpTypeFloat = 22;
        constexpr uint32_t kOpTypeVector = 23;
        constexpr uint32_t kOpTypeMatrix = 24;
        constexpr uint32_t kOpTypeImage = 25;
        constexpr uint32_t kOpTypeSampler = 26;
        constexpr uint32_t kOpTypeSampledImage = 27;
        constexpr uint32_t kOpTypeArray = 28;
        constexpr uint32_t kOpTypeRuntimeArray = 29;
        constexpr uint32_t kOpTypeStruct = 30;
        constexpr uint32_t kOpTypePointer = 32;
        constexpr uint32_t kOpConstant = 43;
        constexpr uint32_t kOpVariable = 59;
        constexpr uint32_t kOpDecorate = 71;
        constexpr uint32_t kOpMemberDecorate = 72;

        // Decorations.
        constexpr uint32_t kDecorationBlock = 2;
        constexpr uint32_t kDecorationBufferBlock = 3;
        constexpr uint32_t kDecorationArrayStride = 6;
        constexpr uint32_t kDecorationBuiltIn = 11;
        constexpr uint32_t kDecorationLocation = 30;
        constexpr uint32_t kDecorationBinding = 33;
        constexpr uint32_t kDecorationDescriptorSet = 34;
        constexpr uint32_t kDecorationOffset = 35;

        constexpr uint32_t kBuiltInFragDepth = 22;

        // Storage classes.
        constexpr uint32_t kStorageUniformConstant = 0;
        constexpr uint32_t kStorageUniform = 2;
        constexpr uint32_t kStorageOutput = 3;
        constexpr uint32_t kStorageBuffer = 12;

        // Execution models.
        constexpr uint32_t kModelVertex = 0;
        constexpr uint32_t kModelFragment = 4;
        constexpr uint32_t kModelGLCompute = 5;

        /// Longest chain of array / vector / matrix wrappers followed when resolving a type.
        /// Real shaders need two or three steps; the bound makes cyclic (corrupt) input terminate.
        constexpr int kMaxTypeDepth = 16;

        enum class TypeKind : uint8_t {
            Int, Float, Vector, Matrix, Image, Sampler, SampledImage, Array, RuntimeArray, Struct, Pointer
        };

        struct Type {
            TypeKind kind = TypeKind::Int;
            uint32_t element = 0;        ///< Component / column / element / image / pointee type id.
            uint32_t count = 0;          ///< Vector or matrix size; for Array the id of the length constant.
            uint32_t storageClass = 0;   ///< Pointer only.
            uint32_t sampled = 0;        ///< Image only: 1 = used with a sampler, 2 = storage image.
            bool isSigned = false;       ///< Int only.
            std::vector<uint32_t> members;   ///< Struct only.
        };

        struct Decorations {
            bool block = false;
            bool bufferBlock = false;
            bool hasLocation = false;
            bool hasBinding = false;
            bool hasSet = false;
            bool hasBuiltIn = false;
            uint32_t location = 0;
            uint32_t binding = 0;
            uint32_t set = 0;
            uint32_t builtIn = 0;
            uint32_t arrayStride = 0;
        };

        struct Variable {
            uint32_t id = 0;
            uint32_t pointerType = 0;
            uint32_t storageClass = 0;
        };

        struct Module {
            std::unordered_map<uint32_t, std::string> names;
            std::unordered_map<uint64_t, std::string> memberNames;     ///< Key: MemberKey(struct, member).
            std::unordered_map<uint64_t, uint32_t> memberOffsets;      ///< Key: MemberKey(struct, member).
            std::unordered_map<uint32_t, Type> types;
            std::unordered_map<uint32_t, Decorations> decorations;
            std::unordered_map<uint32_t, uint32_t> constants;          ///< Low word of scalar constants.
            std::vector<Variable> variables;                           ///< In declaration order.
            std::vector<uint32_t> structs;                             ///< In declaration order.
            bool hasEntryPoint = false;
            uint32_t executionModel = 0;
            std::string entryPoint;

            const Type* FindType(uint32_t id) const {
                const auto it = types.find(id);
                return it == types.end() ? nullptr : &it->second;
            }
            const Decorations* FindDecorations(uint32_t id) const {
                const auto it = decorations.find(id);
                return it == decorations.end() ? nullptr : &it->second;
            }
            std::string NameOf(uint32_t id) const {
                const auto it = names.find(id);
                return it == names.end() ? std::string{} : it->second;
            }
        };

        constexpr uint64_t MemberKey(uint32_t structId, uint32_t member) {
            return (static_cast<uint64_t>(structId) << 32) | member;
        }

        /// Decodes a SPIR-V literal string (UTF-8, little-endian packed, nul-terminated).
        /// Returns false if the terminator is missing within `words`.
        bool ReadString(std::span<const uint32_t> words, std::string& out) {
            out.clear();
            for (const uint32_t word : words) {
                for (uint32_t shift = 0; shift < 32; shift += 8) {
                    const char c = static_cast<char>((word >> shift) & 0xFFu);
                    if (c == '\0') return true;
                    out.push_back(c);
                }
            }
            return false;
        }

        /// First pass: walks the instruction stream and records everything reflection needs.
        bool ParseModule(std::span<const uint32_t> spirv, Module& m, std::string& error) {
            if (spirv.size() < kHeaderWords) {
                error = std::format("module has {} words, a SPIR-V header alone needs {}", spirv.size(), kHeaderWords);
                return false;
            }
            if (spirv[0] != kSpirvMagic) {
                error = std::format("bad magic number 0x{:08X} (expected 0x{:08X})", spirv[0], kSpirvMagic);
                return false;
            }

            size_t pos = kHeaderWords;
            while (pos < spirv.size()) {
                const uint32_t first = spirv[pos];
                const uint32_t opcode = first & 0xFFFFu;
                const size_t wordCount = first >> 16;
                if (wordCount == 0) {
                    error = std::format("instruction at word {} has a word count of zero", pos);
                    return false;
                }
                if (wordCount > spirv.size() - pos) {
                    error = std::format("instruction at word {} (opcode {}) needs {} words but only {} remain",
                                        pos, opcode, wordCount, spirv.size() - pos);
                    return false;
                }
                // The whole instruction, including its first word: w[1] is the first operand.
                const std::span<const uint32_t> w = spirv.subspan(pos, wordCount);
                pos += wordCount;

                const auto tooShort = [&](size_t needed) {
                    if (wordCount >= needed) return false;
                    error = std::format("instruction with opcode {} has {} words, expected at least {}",
                                        opcode, wordCount, needed);
                    return true;
                };
                const auto badString = [&]() {
                    error = std::format("instruction with opcode {} contains an unterminated string", opcode);
                    return false;
                };

                switch (opcode) {
                    case kOpName: {
                        if (tooShort(3)) return false;
                        std::string name;
                        if (!ReadString(w.subspan(2), name)) return badString();
                        m.names[w[1]] = std::move(name);
                        break;
                    }
                    case kOpMemberName: {
                        if (tooShort(4)) return false;
                        std::string name;
                        if (!ReadString(w.subspan(3), name)) return badString();
                        m.memberNames[MemberKey(w[1], w[2])] = std::move(name);
                        break;
                    }
                    case kOpEntryPoint: {
                        if (tooShort(4)) return false;
                        std::string name;
                        if (!ReadString(w.subspan(3), name)) return badString();
                        if (!m.hasEntryPoint) {
                            m.hasEntryPoint = true;
                            m.executionModel = w[1];
                            m.entryPoint = std::move(name);
                        }
                        break;
                    }
                    case kOpTypeInt: {
                        if (tooShort(4)) return false;
                        Type t;
                        t.kind = TypeKind::Int;
                        t.isSigned = w[3] != 0;
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypeFloat: {
                        if (tooShort(3)) return false;
                        Type t;
                        t.kind = TypeKind::Float;
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypeVector:
                    case kOpTypeMatrix: {
                        if (tooShort(4)) return false;
                        Type t;
                        t.kind = opcode == kOpTypeVector ? TypeKind::Vector : TypeKind::Matrix;
                        t.element = w[2];
                        t.count = w[3];
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypeImage: {
                        // result, sampled type, dim, depth, arrayed, multisampled, sampled, format
                        if (tooShort(9)) return false;
                        Type t;
                        t.kind = TypeKind::Image;
                        t.element = w[2];
                        t.sampled = w[7];
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypeSampler: {
                        if (tooShort(2)) return false;
                        Type t;
                        t.kind = TypeKind::Sampler;
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypeSampledImage: {
                        if (tooShort(3)) return false;
                        Type t;
                        t.kind = TypeKind::SampledImage;
                        t.element = w[2];
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypeArray: {
                        if (tooShort(4)) return false;
                        Type t;
                        t.kind = TypeKind::Array;
                        t.element = w[2];
                        t.count = w[3];
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypeRuntimeArray: {
                        if (tooShort(3)) return false;
                        Type t;
                        t.kind = TypeKind::RuntimeArray;
                        t.element = w[2];
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypeStruct: {
                        if (tooShort(2)) return false;
                        Type t;
                        t.kind = TypeKind::Struct;
                        t.members.assign(w.begin() + 2, w.end());
                        if (m.types.find(w[1]) == m.types.end()) m.structs.push_back(w[1]);
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpTypePointer: {
                        if (tooShort(4)) return false;
                        Type t;
                        t.kind = TypeKind::Pointer;
                        t.storageClass = w[2];
                        t.element = w[3];
                        m.types[w[1]] = std::move(t);
                        break;
                    }
                    case kOpConstant: {
                        if (tooShort(4)) return false;
                        m.constants[w[2]] = w[3];
                        break;
                    }
                    case kOpVariable: {
                        if (tooShort(4)) return false;
                        m.variables.push_back(Variable{w[2], w[1], w[3]});
                        break;
                    }
                    case kOpDecorate: {
                        if (tooShort(3)) return false;
                        const uint32_t decoration = w[2];
                        const bool needsOperand =
                            decoration == kDecorationArrayStride || decoration == kDecorationBuiltIn ||
                            decoration == kDecorationLocation || decoration == kDecorationBinding ||
                            decoration == kDecorationDescriptorSet;
                        if (needsOperand && tooShort(4)) return false;
                        // Only create an entry for decorations that are actually recorded.
                        switch (decoration) {
                            case kDecorationBlock: m.decorations[w[1]].block = true; break;
                            case kDecorationBufferBlock: m.decorations[w[1]].bufferBlock = true; break;
                            case kDecorationArrayStride: m.decorations[w[1]].arrayStride = w[3]; break;
                            case kDecorationBuiltIn: {
                                Decorations& d = m.decorations[w[1]];
                                d.hasBuiltIn = true;
                                d.builtIn = w[3];
                                break;
                            }
                            case kDecorationLocation: {
                                Decorations& d = m.decorations[w[1]];
                                d.hasLocation = true;
                                d.location = w[3];
                                break;
                            }
                            case kDecorationBinding: {
                                Decorations& d = m.decorations[w[1]];
                                d.hasBinding = true;
                                d.binding = w[3];
                                break;
                            }
                            case kDecorationDescriptorSet: {
                                Decorations& d = m.decorations[w[1]];
                                d.hasSet = true;
                                d.set = w[3];
                                break;
                            }
                            default: break;
                        }
                        break;
                    }
                    case kOpMemberDecorate: {
                        if (tooShort(4)) return false;
                        if (w[3] == kDecorationOffset) {
                            if (tooShort(5)) return false;
                            m.memberOffsets[MemberKey(w[1], w[2])] = w[4];
                        }
                        break;
                    }
                    default:
                        break;   // Everything else (code, debug info, extensions) is irrelevant here.
                }
            }
            return true;
        }

        /// Scalar base type and component count of a (possibly wrapped) numeric type.
        bool ResolveNumeric(const Module& m, uint32_t typeId, OutputBaseType& base, uint32_t& components) {
            components = 1;
            for (int depth = 0; depth < kMaxTypeDepth; ++depth) {
                const Type* t = m.FindType(typeId);
                if (!t) return false;
                switch (t->kind) {
                    case TypeKind::Float:
                        base = OutputBaseType::Float;
                        return true;
                    case TypeKind::Int:
                        base = t->isSigned ? OutputBaseType::Int : OutputBaseType::UInt;
                        return true;
                    case TypeKind::Vector:
                        components = t->count;
                        typeId = t->element;
                        break;
                    case TypeKind::Matrix:
                    case TypeKind::Array:
                        typeId = t->element;
                        break;
                    default:
                        return false;
                }
            }
            return false;
        }

        /// Classifies one resource variable. Returns false (and `error`) for anything the
        /// renderer cannot bind.
        bool ReflectBinding(const Module& m, const Variable& var, const Type& pointer,
                            VkShaderStageFlagBits stage, ShaderBinding& out, std::string& error) {
            const std::string variableName = m.NameOf(var.id);
            const auto describe = [&]() {
                return variableName.empty() ? std::format("resource variable %{}", var.id)
                                            : std::format("resource '{}'", variableName);
            };

            const Decorations* decorations = m.FindDecorations(var.id);
            if (!decorations || !decorations->hasSet || !decorations->hasBinding) {
                error = std::format("{} has no DescriptorSet / Binding decoration", describe());
                return false;
            }

            // Peel descriptor arrays: `uniform sampler2D u_Tex[4]` is one binding with count 4.
            uint64_t count = 1;
            uint32_t typeId = pointer.element;
            const Type* type = m.FindType(typeId);
            for (int depth = 0; type && type->kind == TypeKind::Array; ++depth) {
                const auto length = m.constants.find(type->count);
                if (depth >= kMaxTypeDepth || length == m.constants.end() || length->second == 0) {
                    error = std::format("{} is an array whose size is not a positive constant", describe());
                    return false;
                }
                count *= length->second;
                if (count > std::numeric_limits<uint32_t>::max()) {
                    error = std::format("{} has an absurd descriptor count", describe());
                    return false;
                }
                typeId = type->element;
                type = m.FindType(typeId);
            }
            if (!type) {
                error = std::format("{} refers to an undefined type", describe());
                return false;
            }
            if (type->kind == TypeKind::RuntimeArray) {
                error = std::format("{} is an unsized descriptor array, which is not supported", describe());
                return false;
            }

            out = ShaderBinding{};
            out.set = decorations->set;
            out.binding = decorations->binding;
            out.count = static_cast<uint32_t>(count);
            out.stages = static_cast<VkShaderStageFlags>(stage);

            if (type->kind == TypeKind::Struct) {
                const Decorations* structDecorations = m.FindDecorations(typeId);
                const bool block = structDecorations && structDecorations->block;
                const bool bufferBlock = structDecorations && structDecorations->bufferBlock;
                if (var.storageClass == kStorageBuffer || (var.storageClass == kStorageUniform && bufferBlock)) {
                    out.type = BindingType::StorageBuffer;
                } else if (var.storageClass == kStorageUniform && block) {
                    out.type = BindingType::UniformBuffer;
                } else {
                    error = std::format("{} is a struct that is neither a uniform block nor a storage block", describe());
                    return false;
                }
                out.blockName = m.NameOf(typeId);
                out.name = variableName.empty() ? out.blockName : variableName;
                return true;
            }

            if (var.storageClass == kStorageUniformConstant && type->kind == TypeKind::SampledImage) {
                out.type = BindingType::CombinedImageSampler;
                out.name = variableName;
                return true;
            }
            if (var.storageClass == kStorageUniformConstant && type->kind == TypeKind::Image && type->sampled == 2) {
                out.type = BindingType::StorageImage;
                out.name = variableName;
                return true;
            }

            error = std::format("{} has a resource type that is not supported (only uniform / storage blocks, "
                                "combined image samplers and storage images are)", describe());
            return false;
        }

        void ReflectBlocks(const Module& m, std::vector<BlockLayout>& out) {
            // `T items[]` members: remember the stride of T as laid out in that array.
            std::unordered_map<uint32_t, uint32_t> elementStride;
            for (const uint32_t structId : m.structs) {
                const Type* st = m.FindType(structId);
                if (!st || st->kind != TypeKind::Struct) continue;
                for (const uint32_t memberType : st->members) {
                    const Type* array = m.FindType(memberType);
                    if (!array || array->kind != TypeKind::RuntimeArray) continue;
                    const Type* element = m.FindType(array->element);
                    if (!element || element->kind != TypeKind::Struct) continue;
                    const Decorations* d = m.FindDecorations(memberType);
                    if (d && d->arrayStride != 0) elementStride.try_emplace(array->element, d->arrayStride);
                }
            }

            for (const uint32_t structId : m.structs) {
                const Type* st = m.FindType(structId);
                if (!st || st->kind != TypeKind::Struct) continue;
                BlockLayout layout;
                for (uint32_t i = 0; i < st->members.size(); ++i) {
                    const auto offset = m.memberOffsets.find(MemberKey(structId, i));
                    if (offset == m.memberOffsets.end()) continue;
                    BlockMember member;
                    member.offset = offset->second;
                    const auto name = m.memberNames.find(MemberKey(structId, i));
                    if (name != m.memberNames.end()) member.name = name->second;
                    layout.members.push_back(std::move(member));
                }
                if (layout.members.empty()) continue;
                layout.blockName = m.NameOf(structId);
                const auto stride = elementStride.find(structId);
                if (stride != elementStride.end()) layout.arrayStride = stride->second;
                out.push_back(std::move(layout));
            }
        }
    }

    const ShaderBinding* ShaderReflection::FindBinding(std::string_view name) const {
        for (const ShaderBinding& b : bindings) {
            if (b.name == name) return &b;
        }
        return nullptr;
    }

    const BlockLayout* ShaderReflection::FindBlock(std::string_view blockOrStructName) const {
        for (const BlockLayout& b : blocks) {
            if (b.blockName == blockOrStructName) return &b;
        }
        return nullptr;
    }

    bool ReflectSpirv(std::span<const uint32_t> spirv, ShaderReflection& out, std::string* error) {
        out = ShaderReflection{};
        std::string message;
        const auto fail = [&]() {
            if (error) *error = std::move(message);
            return false;
        };

        Module m;
        if (!ParseModule(spirv, m, message)) return fail();

        if (!m.hasEntryPoint) {
            message = "module has no entry point";
            return fail();
        }

        ShaderReflection result;
        switch (m.executionModel) {
            case kModelVertex: result.stage = VK_SHADER_STAGE_VERTEX_BIT; break;
            case kModelFragment: result.stage = VK_SHADER_STAGE_FRAGMENT_BIT; break;
            case kModelGLCompute: result.stage = VK_SHADER_STAGE_COMPUTE_BIT; break;
            default:
                message = std::format("unsupported execution model {}", m.executionModel);
                return fail();
        }
        result.entryPoint = m.entryPoint;

        for (const Variable& var : m.variables) {
            const bool isResource = var.storageClass == kStorageUniformConstant ||
                                    var.storageClass == kStorageUniform || var.storageClass == kStorageBuffer;
            const bool isFragmentOutput = var.storageClass == kStorageOutput && result.stage == VK_SHADER_STAGE_FRAGMENT_BIT;
            if (!isResource && !isFragmentOutput) continue;

            const Type* pointer = m.FindType(var.pointerType);
            if (!pointer || pointer->kind != TypeKind::Pointer) {
                message = std::format("variable %{} does not have a pointer type", var.id);
                return fail();
            }

            if (isResource) {
                ShaderBinding binding;
                if (!ReflectBinding(m, var, *pointer, result.stage, binding, message)) return fail();
                for (const ShaderBinding& other : result.bindings) {
                    if (other.set == binding.set && other.binding == binding.binding) {
                        message = std::format("'{}' and '{}' both use set {} binding {}",
                                              other.name, binding.name, binding.set, binding.binding);
                        return fail();
                    }
                }
                result.bindings.push_back(std::move(binding));
                continue;
            }

            const Decorations* d = m.FindDecorations(var.id);
            if (!d) continue;
            if (d->hasBuiltIn) {
                if (d->builtIn == kBuiltInFragDepth) result.writesDepth = true;
                continue;
            }
            if (!d->hasLocation) continue;
            ShaderOutput output;
            output.location = d->location;
            output.name = m.NameOf(var.id);
            if (!ResolveNumeric(m, pointer->element, output.base, output.components)) {
                message = std::format("fragment output '{}' (location {}) is not a scalar or vector",
                                      output.name, output.location);
                return fail();
            }
            result.outputs.push_back(std::move(output));
        }

        std::sort(result.bindings.begin(), result.bindings.end(), [](const ShaderBinding& a, const ShaderBinding& b) {
            return a.set != b.set ? a.set < b.set : a.binding < b.binding;
        });
        std::stable_sort(result.outputs.begin(), result.outputs.end(),
                         [](const ShaderOutput& a, const ShaderOutput& b) { return a.location < b.location; });

        ReflectBlocks(m, result.blocks);

        out = std::move(result);
        return true;
    }

    std::vector<uint32_t> LoadSpirvFile(const std::filesystem::path& path) {
        std::error_code ec;
        const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
        if (ec || bytes < kHeaderWords * sizeof(uint32_t) || bytes % sizeof(uint32_t) != 0) return {};

        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        std::vector<uint32_t> words(static_cast<size_t>(bytes / sizeof(uint32_t)));
        file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));
        if (file.gcount() != static_cast<std::streamsize>(bytes) || words[0] != kSpirvMagic) return {};
        return words;
    }
}
