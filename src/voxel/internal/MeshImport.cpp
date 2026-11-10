module;

// This is the one translation unit of the engine that compiles the tinygltf / stb_image /
// stb_image_write implementations. Other libraries (the renderer's screenshot path) link
// against the stb symbols defined here, so they must keep external linkage.
// (implementations are compiled in ThirdPartyImpl.cpp, a plain translation unit)
#include "tiny_gltf.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

module vortex.voxel;

import vortex.core;

namespace vortex::voxel {

    namespace {

        // ---------------------------------------------------------------------------------
        //  Voxeliser
        // ---------------------------------------------------------------------------------

        bool IsFinite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        /// One separating-axis candidate of a triangle against a unit box: the box centre `c`
        /// overlaps on this axis iff lo <= dot(dir, c) <= hi (the triangle's projection widened
        /// by the box's projected half extent).
        struct SatAxis {
            glm::vec3 dir{0.0f};
            float lo = 0.0f, hi = 0.0f;
        };

        /// Axes that can separate a triangle from an axis-aligned unit cube besides the three
        /// cube axes: the triangle normal and the nine edge x cube-axis cross products.
        std::array<SatAxis, 10> BuildSatAxes(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
            const std::array<glm::vec3, 3> edges{b - a, c - b, a - c};
            std::array<SatAxis, 10> axes;
            axes[0].dir = glm::cross(edges[0], edges[1]);
            size_t n = 1;
            for (const glm::vec3& e : edges) {
                axes[n++].dir = glm::vec3(0.0f, -e.z, e.y);   // (1,0,0) x e
                axes[n++].dir = glm::vec3(e.z, 0.0f, -e.x);   // (0,1,0) x e
                axes[n++].dir = glm::vec3(-e.y, e.x, 0.0f);   // (0,0,1) x e
            }
            for (SatAxis& axis : axes) {
                const float pa = glm::dot(axis.dir, a), pb = glm::dot(axis.dir, b), pc = glm::dot(axis.dir, c);
                const float l1 = std::abs(axis.dir.x) + std::abs(axis.dir.y) + std::abs(axis.dir.z);
                // Half extent of the unit cube on this axis plus a little slack so rounding can
                // only add voxels (conservative), never open a hole in a surface.
                const float radius = 0.5f * l1 + 1.0e-4f * l1;
                axis.lo = std::min({pa, pb, pc}) - radius;
                axis.hi = std::max({pa, pb, pc}) + radius;
            }
            return axes;
        }

        /// Marks everything reachable from outside the volume (6-connectivity, starting in a
        /// 1-voxel border around it) and fills the rest: an unreached empty voxel is enclosed by
        /// the surface and takes the id of the nearest solid voxel towards -X in its row.
        void FillInterior(VoxelVolume& volume) {
            const glm::ivec3 size = volume.Size();
            const size_t px = static_cast<size_t>(size.x) + 2, py = static_cast<size_t>(size.y) + 2, pz = static_cast<size_t>(size.z) + 2;
            std::vector<uint8_t> outside(px * py * pz, 0);
            const std::vector<uint8_t>& data = volume.Data();

            // Padded coordinates: voxel (x, y, z) of the volume lives at (x + 1, y + 1, z + 1).
            auto blocked = [&](size_t x, size_t y, size_t z) {
                if (x == 0 || y == 0 || z == 0 || x == px - 1 || y == py - 1 || z == pz - 1) return false;
                return data[(x - 1) + static_cast<size_t>(size.x) * ((y - 1) + static_cast<size_t>(size.y) * (z - 1))] != 0;
            };

            std::vector<size_t> stack;
            stack.push_back(0);
            outside[0] = 1;
            auto visit = [&](size_t x, size_t y, size_t z) {
                const size_t index = x + px * (y + py * z);
                if (outside[index] || blocked(x, y, z)) return;
                outside[index] = 1;
                stack.push_back(index);
            };
            while (!stack.empty()) {
                const size_t index = stack.back();
                stack.pop_back();
                const size_t x = index % px, y = (index / px) % py, z = index / (px * py);
                if (x > 0) visit(x - 1, y, z);
                if (x + 1 < px) visit(x + 1, y, z);
                if (y > 0) visit(x, y - 1, z);
                if (y + 1 < py) visit(x, y + 1, z);
                if (z > 0) visit(x, y, z - 1);
                if (z + 1 < pz) visit(x, y, z + 1);
            }

            std::vector<uint8_t>& out = volume.Data();
            for (int z = 0; z < size.z; ++z)
                for (int y = 0; y < size.y; ++y) {
                    uint8_t nearest = 0;
                    for (int x = 0; x < size.x; ++x) {
                        uint8_t& voxel = out[volume.Index(x, y, z)];
                        if (voxel != 0) {
                            nearest = voxel;
                        } else if (!outside[(static_cast<size_t>(x) + 1) + px * ((static_cast<size_t>(y) + 1) + py * (static_cast<size_t>(z) + 1))]) {
                            // An enclosed voxel always has a solid voxel somewhere towards -X
                            // (otherwise the border would reach it); 1 is only a safety net.
                            voxel = nearest != 0 ? nearest : uint8_t{1};
                        }
                    }
                }
        }

        // ---------------------------------------------------------------------------------
        //  glTF
        // ---------------------------------------------------------------------------------

        float SrgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

        /// Raw view of an accessor's elements inside its buffer, validated against the buffer size.
        struct AccessorView {
            const uint8_t* data = nullptr;
            size_t count = 0;
            size_t stride = 0;
        };

        /// @param elementSize Bytes of one element (also the stride when the buffer view has none).
        bool ResolveAccessor(const tinygltf::Model& model, int accessorIndex, size_t elementSize, AccessorView& out) {
            if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) return false;
            const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
            if (accessor.bufferView < 0 || static_cast<size_t>(accessor.bufferView) >= model.bufferViews.size()) return false;
            const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
            if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= model.buffers.size()) return false;
            const std::vector<unsigned char>& bytes = model.buffers[static_cast<size_t>(view.buffer)].data;

            const size_t stride = view.byteStride != 0 ? view.byteStride : elementSize;
            if (stride < elementSize) return false;
            out.count = accessor.count;
            out.stride = stride;
            if (accessor.count == 0) return true;

            // Both offsets apply: the view's into the buffer and the accessor's into the view.
            if (view.byteOffset > bytes.size() || accessor.byteOffset > bytes.size() - view.byteOffset) return false;
            const size_t offset = view.byteOffset + accessor.byteOffset;
            const size_t available = bytes.size() - offset;
            if (available < elementSize || accessor.count - 1 > (available - elementSize) / stride) return false;
            out.data = bytes.data() + offset;
            return true;
        }

        glm::dmat4 LocalMatrix(const tinygltf::Node& node) {
            if (node.matrix.size() == 16) {
                glm::dmat4 m(1.0);
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) m[column][row] = node.matrix[static_cast<size_t>(column * 4 + row)];   // column-major, like glm
                return m;
            }
            glm::dmat4 m(1.0);   // T * R * S
            if (node.rotation.size() == 4) {
                const glm::dquat q(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);   // glTF stores x y z w
                if (glm::dot(q, q) > 0.0) m = glm::mat4_cast(glm::normalize(q));
            }
            if (node.scale.size() == 3) {
                m[0] *= node.scale[0];
                m[1] *= node.scale[1];
                m[2] *= node.scale[2];
            }
            if (node.translation.size() == 3) m[3] = glm::dvec4(node.translation[0], node.translation[1], node.translation[2], 1.0);
            return m;
        }

        /// Average colour (linear RGB) of a base colour texture, or nullopt if it has no usable pixels.
        std::optional<glm::vec3> AverageTextureColor(const tinygltf::Model& model, int textureIndex) {
            if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= model.textures.size()) return std::nullopt;
            const int source = model.textures[static_cast<size_t>(textureIndex)].source;
            if (source < 0 || static_cast<size_t>(source) >= model.images.size()) return std::nullopt;
            const tinygltf::Image& image = model.images[static_cast<size_t>(source)];
            if (image.as_is || image.width <= 0 || image.height <= 0 || image.component < 1 || image.component > 4) return std::nullopt;
            if (image.bits != 8 && image.bits != 16) return std::nullopt;

            const size_t width = static_cast<size_t>(image.width), height = static_cast<size_t>(image.height);
            const size_t channels = static_cast<size_t>(image.component), channelBytes = static_cast<size_t>(image.bits / 8);
            const size_t pixelBytes = channels * channelBytes;
            if (width > image.image.size() / pixelBytes / height) return std::nullopt;   // pixel data shorter than the header claims

            // The average of a few tens of thousands of evenly spread texels is as good as the
            // average of all of them and keeps 4K textures cheap.
            const size_t step = std::max<size_t>(1, static_cast<size_t>(std::sqrt(static_cast<double>(width * height) / 65536.0)));

            std::array<float, 256> lut{};
            for (size_t i = 0; i < lut.size(); ++i) lut[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
            auto channel = [&](const unsigned char* pixel, size_t c) {
                if (channelBytes == 1) return lut[pixel[c]];
                uint16_t value = 0;
                std::memcpy(&value, pixel + c * 2, sizeof(value));
                return SrgbToLinear(static_cast<float>(value) / 65535.0f);
            };

            glm::dvec3 sum(0.0);
            size_t samples = 0;
            for (size_t y = 0; y < height; y += step)
                for (size_t x = 0; x < width; x += step) {
                    const unsigned char* pixel = image.image.data() + (y * width + x) * pixelBytes;
                    if (channels >= 3) {
                        sum += glm::dvec3(channel(pixel, 0), channel(pixel, 1), channel(pixel, 2));
                    } else {
                        sum += glm::dvec3(channel(pixel, 0));   // grey or grey + alpha
                    }
                    ++samples;
                }
            if (samples == 0) return std::nullopt;
            return glm::vec3(sum / static_cast<double>(samples));
        }

        Material ConvertMaterial(const tinygltf::Model& model, const tinygltf::Material& source) {
            Material material;
            const tinygltf::PbrMetallicRoughness& pbr = source.pbrMetallicRoughness;
            glm::vec3 color(1.0f);
            if (pbr.baseColorFactor.size() >= 3)   // already linear
                color = glm::vec3(static_cast<float>(pbr.baseColorFactor[0]), static_cast<float>(pbr.baseColorFactor[1]), static_cast<float>(pbr.baseColorFactor[2]));
            if (const std::optional<glm::vec3> average = AverageTextureColor(model, pbr.baseColorTexture.index)) color *= *average;
            material.color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
            return material;
        }

        /// Appends the triangles of one primitive, transformed to voxel units.
        void AppendPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive, const glm::dmat4& transform,
                             std::vector<MeshTriangle>& triangles, size_t& skippedPrimitives) {
            // tinygltf leaves `mode` at -1 only when it was absent, which means TRIANGLES.
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1) return;
            const auto attribute = primitive.attributes.find("POSITION");
            if (attribute == primitive.attributes.end()) return;

            AccessorView positions;
            const int positionIndex = attribute->second;
            const bool positionsValid = positionIndex >= 0 && static_cast<size_t>(positionIndex) < model.accessors.size() &&
                                        model.accessors[static_cast<size_t>(positionIndex)].type == TINYGLTF_TYPE_VEC3 &&
                                        model.accessors[static_cast<size_t>(positionIndex)].componentType == TINYGLTF_COMPONENT_TYPE_FLOAT &&
                                        ResolveAccessor(model, positionIndex, 3 * sizeof(float), positions);
            if (!positionsValid) {
                ++skippedPrimitives;
                return;
            }

            AccessorView indices;
            size_t indexBytes = 0;
            if (primitive.indices >= 0) {
                if (static_cast<size_t>(primitive.indices) >= model.accessors.size()) {
                    ++skippedPrimitives;
                    return;
                }
                switch (model.accessors[static_cast<size_t>(primitive.indices)].componentType) {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  indexBytes = 1; break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: indexBytes = 2; break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   indexBytes = 4; break;
                    default: break;
                }
                if (indexBytes == 0 || !ResolveAccessor(model, primitive.indices, indexBytes, indices)) {
                    ++skippedPrimitives;
                    return;
                }
            }

            auto vertexIndex = [&](size_t i) -> size_t {
                if (indexBytes == 0) return i;
                const uint8_t* p = indices.data + i * indices.stride;
                if (indexBytes == 1) return *p;
                if (indexBytes == 2) {
                    uint16_t v = 0;
                    std::memcpy(&v, p, sizeof(v));
                    return v;
                }
                uint32_t v = 0;
                std::memcpy(&v, p, sizeof(v));
                return v;
            };
            auto position = [&](size_t vertex) {
                float p[3];
                std::memcpy(p, positions.data + vertex * positions.stride, sizeof(p));
                return glm::vec3(transform * glm::dvec4(p[0], p[1], p[2], 1.0));
            };

            const uint8_t material = static_cast<uint8_t>(std::clamp(primitive.material + 1, 1, 255));
            const size_t cornerCount = indexBytes == 0 ? positions.count : indices.count;
            triangles.reserve(triangles.size() + cornerCount / 3);
            for (size_t i = 0; i + 2 < cornerCount; i += 3) {
                const size_t ia = vertexIndex(i), ib = vertexIndex(i + 1), ic = vertexIndex(i + 2);
                if (ia >= positions.count || ib >= positions.count || ic >= positions.count) continue;   // corrupt index
                MeshTriangle triangle;
                triangle.a = position(ia);
                triangle.b = position(ib);
                triangle.c = position(ic);
                triangle.material = material;
                triangles.push_back(triangle);
            }
        }

        MeshImportResult ImportFailure(std::string message) {
            Log::Error("mesh import: {}", message);
            MeshImportResult result;
            result.error = std::move(message);
            return result;
        }
    }

    VoxelizeResult VoxelizeTriangles(std::span<const MeshTriangle> triangles, bool fillInterior, const glm::ivec3& maxSize) {
        VoxelizeResult result;

        // --- bounds (triangles with NaN / infinite corners are ignored throughout) ---
        glm::vec3 boundsMin(0.0f), boundsMax(0.0f);
        bool any = false;
        for (const MeshTriangle& t : triangles) {
            if (!IsFinite(t.a) || !IsFinite(t.b) || !IsFinite(t.c)) continue;
            const glm::vec3 lo = glm::min(t.a, glm::min(t.b, t.c)), hi = glm::max(t.a, glm::max(t.b, t.c));
            boundsMin = any ? glm::min(boundsMin, lo) : lo;
            boundsMax = any ? glm::max(boundsMax, hi) : hi;
            any = true;
        }
        if (!any) return result;

        const glm::vec3 origin = glm::floor(boundsMin);
        // A mesh lying exactly in a grid plane still occupies one layer of voxels.
        const glm::vec3 extent = glm::max(glm::ceil(boundsMax) - origin, glm::vec3(1.0f));
        if (!IsFinite(extent) || extent.x > static_cast<float>(maxSize.x) || extent.y > static_cast<float>(maxSize.y) ||
            extent.z > static_cast<float>(maxSize.z)) {
            Log::Warn("VoxelizeTriangles: mesh needs {:.0f} x {:.0f} x {:.0f} voxels, more than the allowed {} x {} x {}", extent.x, extent.y, extent.z,
                      maxSize.x, maxSize.y, maxSize.z);
            return result;
        }

        const glm::ivec3 size(extent);
        result.origin = origin;
        result.volume = VoxelVolume(size);
        std::vector<uint8_t>& data = result.volume.Data();

        // --- surface: every triangle tests only the voxels of its own bounding box ---
        for (const MeshTriangle& t : triangles) {
            if (!IsFinite(t.a) || !IsFinite(t.b) || !IsFinite(t.c)) continue;
            const glm::vec3 a = t.a - origin, b = t.b - origin, c = t.c - origin;
            const uint8_t id = t.material != 0 ? t.material : uint8_t{1};

            // Separating axes 1..3 (the cube axes): the voxels whose extent overlaps the
            // triangle's on x, y and z are exactly the ones of this range.
            const glm::ivec3 lo = glm::clamp(glm::ivec3(glm::floor(glm::min(a, glm::min(b, c)))), glm::ivec3(0), size - 1);
            const glm::ivec3 hi = glm::clamp(glm::ivec3(glm::floor(glm::max(a, glm::max(b, c)))), glm::ivec3(0), size - 1);
            // Axes 4..13: triangle normal and edge cross products.
            const std::array<SatAxis, 10> axes = BuildSatAxes(a, b, c);

            for (int z = lo.z; z <= hi.z; ++z)
                for (int y = lo.y; y <= hi.y; ++y)
                    for (int x = lo.x; x <= hi.x; ++x) {
                        uint8_t& voxel = data[result.volume.Index(x, y, z)];
                        if (voxel != 0) continue;   // first writer wins
                        const glm::vec3 centre(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f);
                        bool overlap = true;
                        for (const SatAxis& axis : axes) {
                            const float d = glm::dot(axis.dir, centre);
                            if (d < axis.lo || d > axis.hi) {
                                overlap = false;
                                break;
                            }
                        }
                        if (overlap) voxel = id;
                    }
        }

        if (fillInterior) FillInterior(result.volume);
        return result;
    }

    MeshImportResult ImportMesh(const MeshImportSettings& settings) {
        if (!(settings.voxelsPerUnit > 0.0f) || !std::isfinite(settings.voxelsPerUnit)) return ImportFailure("voxelsPerUnit must be a positive number");
        if (settings.maxVoxelsPerAxis <= 0) return ImportFailure("maxVoxelsPerAxis must be positive");

        std::error_code ec;
        if (!std::filesystem::is_regular_file(settings.file, ec)) return ImportFailure(std::format("file '{}' not found", settings.file.string()));

        std::string extension = settings.file.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension != ".glb" && extension != ".gltf")
            return ImportFailure(std::format("'{}' is neither a .glb nor a .gltf file", settings.file.string()));

        tinygltf::Model model;
        std::string loadError, loadWarning;
        bool loaded = false;
        try {
            tinygltf::TinyGLTF loader;
            loaded = extension == ".glb" ? loader.LoadBinaryFromFile(&model, &loadError, &loadWarning, settings.file.string())
                                         : loader.LoadASCIIFromFile(&model, &loadError, &loadWarning, settings.file.string());
        } catch (const std::exception& e) {
            loadError = e.what();
            loaded = false;
        }
        if (!loadWarning.empty()) Log::Warn("mesh import: '{}': {}", settings.file.string(), loadWarning);
        if (!loaded) return ImportFailure(std::format("cannot load '{}': {}", settings.file.string(), loadError.empty() ? "unknown error" : loadError));

        // --- roots: the default scene (or the first one); a file without scenes uses every parentless node ---
        std::vector<int> roots;
        if (!model.scenes.empty()) {
            const size_t scene = model.defaultScene >= 0 && static_cast<size_t>(model.defaultScene) < model.scenes.size()
                                     ? static_cast<size_t>(model.defaultScene) : size_t{0};
            roots = model.scenes[scene].nodes;
        } else {
            std::vector<bool> hasParent(model.nodes.size(), false);
            for (const tinygltf::Node& node : model.nodes)
                for (int child : node.children)
                    if (child >= 0 && static_cast<size_t>(child) < model.nodes.size()) hasParent[static_cast<size_t>(child)] = true;
            for (size_t i = 0; i < model.nodes.size(); ++i)
                if (!hasParent[i]) roots.push_back(static_cast<int>(i));
        }

        // --- node hierarchy -> triangles in voxel units ---
        std::vector<MeshTriangle> triangles;
        size_t skippedPrimitives = 0;
        std::vector<bool> visited(model.nodes.size(), false);   // a valid file is a forest; this also stops cycles in a broken one
        std::vector<std::pair<int, glm::dmat4>> stack;
        const glm::dmat4 toVoxels = glm::dmat4(glm::dmat3(static_cast<double>(settings.voxelsPerUnit)));
        for (auto root = roots.rbegin(); root != roots.rend(); ++root) stack.emplace_back(*root, toVoxels);
        while (!stack.empty()) {
            const auto [nodeIndex, parent] = stack.back();
            stack.pop_back();
            if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= model.nodes.size() || visited[static_cast<size_t>(nodeIndex)]) continue;
            visited[static_cast<size_t>(nodeIndex)] = true;
            const tinygltf::Node& node = model.nodes[static_cast<size_t>(nodeIndex)];
            const glm::dmat4 world = parent * LocalMatrix(node);

            if (node.mesh >= 0 && static_cast<size_t>(node.mesh) < model.meshes.size())
                for (const tinygltf::Primitive& primitive : model.meshes[static_cast<size_t>(node.mesh)].primitives)
                    AppendPrimitive(model, primitive, world, triangles, skippedPrimitives);
            for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) stack.emplace_back(*child, world);
        }
        if (skippedPrimitives != 0)
            Log::Warn("mesh import: '{}': skipped {} primitives with unsupported or corrupt accessors", settings.file.string(), skippedPrimitives);
        if (triangles.empty()) return ImportFailure(std::format("'{}' contains no triangles", settings.file.string()));

        VoxelizeResult voxelized = VoxelizeTriangles(triangles, settings.fillInterior, glm::ivec3(settings.maxVoxelsPerAxis));
        if (voxelized.volume.Size() == glm::ivec3(0))
            return ImportFailure(std::format("'{}' does not fit into {} voxels per axis at {} voxels per unit (or has no finite geometry)",
                                             settings.file.string(), settings.maxVoxelsPerAxis, settings.voxelsPerUnit));

        MeshImportResult result;
        result.ok = true;
        result.volume = std::move(voxelized.volume);
        // materials[i] belongs to voxel id i + 1 = glTF material i; ids stop at 255.
        const size_t materialCount = std::min<size_t>(model.materials.size(), 255);
        result.materials.reserve(std::max<size_t>(materialCount, 1));
        for (size_t i = 0; i < materialCount; ++i) result.materials.push_back(ConvertMaterial(model, model.materials[i]));
        if (result.materials.empty()) result.materials.emplace_back();   // primitives without a material use id 1
        Log::Info("mesh import: '{}' -> {} triangles, {} x {} x {} voxels, {} materials", settings.file.string(), triangles.size(),
                  result.volume.Size().x, result.volume.Size().y, result.volume.Size().z, result.materials.size());
        return result;
    }
}
