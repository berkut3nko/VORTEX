module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.voxel;

import vortex.core;

// MagicaVoxel .vox reader / writer.
//
// File layout (all integers little endian):
//   "VOX " int32 version
//   chunk := id[4] int32 contentBytes int32 childrenBytes content[contentBytes] children[childrenBytes]
//   MAIN (no content) holds every other chunk as a child:
//     SIZE  int32 x y z                       dimensions of the next model
//     XYZI  int32 n, n * (x y z colourIndex)  its voxels, one byte per field
//     RGBA  256 * (r g b a)                   colour of voxel index i (1..255) is entry i - 1
//     nTRN / nGRP / nSHP                      scene graph (transform -> group | shape -> models)
//
// Scene graph handling: only translations are used. The translation of a model is the sum of
// the "_t" attributes (first frame) of all transform nodes on the path from the root to its
// shape node. Rotations ("_r") are ignored: a rotated model is imported unrotated at the right
// place. A model instanced by several shape nodes takes the translation of the first instance
// in traversal order. Layers, materials (MATL), cameras and other chunks are skipped.

namespace vortex::voxel {

    namespace {
        constexpr int kMaxModelAxis = 1024;                              ///< MagicaVoxel itself stops at 256.
        constexpr uint64_t kMaxModelVoxels = 512ull * 512ull * 512ull;   ///< Bounds the allocation a hostile SIZE chunk can trigger.
        constexpr uint64_t kMaxFileVoxels = 1ull << 30;                  ///< All models of one file together (a SIZE chunk costs 24 bytes, so a tiny file could otherwise ask for terabytes).
        constexpr int kMaxWriteAxis = 256;                               ///< XYZI coordinates are single bytes.
        constexpr int32_t kWriteVersion = 150;

        /// Bounds-checked little-endian cursor over a byte range. Every read reports failure
        /// instead of stepping outside the range.
        class Reader {
        public:
            explicit Reader(std::span<const uint8_t> bytes) : m_Bytes(bytes) {}

            size_t Remaining() const { return m_Bytes.size() - m_Pos; }
            bool AtEnd() const { return m_Pos == m_Bytes.size(); }

            bool ReadI32(int32_t& out) {
                if (Remaining() < 4) return false;
                const uint8_t* p = m_Bytes.data() + m_Pos;
                const uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
                out = static_cast<int32_t>(v);
                m_Pos += 4;
                return true;
            }

            /// Sub-range of `count` bytes; advances past it.
            bool ReadSpan(size_t count, std::span<const uint8_t>& out) {
                if (Remaining() < count) return false;
                out = m_Bytes.subspan(m_Pos, count);
                m_Pos += count;
                return true;
            }

            /// STRING := int32 length + bytes.
            bool ReadString(std::string& out) {
                int32_t length = 0;
                std::span<const uint8_t> chars;
                if (!ReadI32(length) || length < 0 || !ReadSpan(static_cast<size_t>(length), chars)) return false;
                out.assign(reinterpret_cast<const char*>(chars.data()), chars.size());
                return true;
            }

            /// DICT := int32 count + count * (STRING key, STRING value).
            bool ReadDict(std::vector<std::pair<std::string, std::string>>& out) {
                int32_t count = 0;
                if (!ReadI32(count) || count < 0) return false;
                // Each pair needs at least 8 bytes: a cheap sanity bound before reserving.
                if (static_cast<size_t>(count) > Remaining() / 8) return false;
                out.clear();
                out.reserve(static_cast<size_t>(count));
                for (int32_t i = 0; i < count; ++i) {
                    std::string key, value;
                    if (!ReadString(key) || !ReadString(value)) return false;
                    out.emplace_back(std::move(key), std::move(value));
                }
                return true;
            }

        private:
            std::span<const uint8_t> m_Bytes;
            size_t m_Pos = 0;
        };

        using Dict = std::vector<std::pair<std::string, std::string>>;

        const std::string* FindKey(const Dict& dict, std::string_view key) {
            for (const auto& [k, v] : dict)
                if (k == key) return &v;
            return nullptr;
        }

        /// Parses "x y z" (MagicaVoxel axes). Missing or malformed components are 0.
        glm::ivec3 ParseTranslation(std::string_view text) {
            glm::ivec3 t(0);
            size_t pos = 0;
            for (int i = 0; i < 3; ++i) {
                while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
                if (pos >= text.size()) break;
                int value = 0;
                const char* begin = text.data() + pos;
                const char* end = text.data() + text.size();
                const auto [ptr, ec] = std::from_chars(begin, end, value);
                if (ec != std::errc{}) break;
                t[i] = value;
                pos = static_cast<size_t>(ptr - text.data());
            }
            return t;
        }

        enum class NodeKind { Transform, Group, Shape };

        struct SceneNode {
            NodeKind kind = NodeKind::Transform;
            glm::ivec3 translation{0};      // Transform: MagicaVoxel axes
            std::string name;               // Transform
            std::vector<int32_t> children;  // Transform: one child; Group: children; Shape: model ids
        };

        struct RawModel {
            glm::ivec3 size{0};             // MagicaVoxel axes
            std::span<const uint8_t> voxels;   // n * 4 bytes
        };

        /// Stand-in for a missing RGBA chunk, shaped like MagicaVoxel's built-in palette: a
        /// 6 x 6 x 6 colour cube (white first, black left out) followed by red, green, blue and
        /// grey ramps.
        std::array<glm::u8vec4, 256> FallbackPalette() {
            std::array<glm::u8vec4, 256> palette{};
            uint32_t index = 1;
            for (int r = 0; r < 6; ++r)
                for (int g = 0; g < 6; ++g)
                    for (int b = 0; b < 6; ++b) {
                        if (r == 5 && g == 5 && b == 5) continue;
                        palette[index++] = glm::u8vec4(static_cast<uint8_t>(255 - r * 51), static_cast<uint8_t>(255 - g * 51),
                                                       static_cast<uint8_t>(255 - b * 51), uint8_t{255});
                    }
            constexpr std::array<uint8_t, 10> ramp{0xee, 0xdd, 0xbb, 0xaa, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11};
            for (uint8_t v : ramp) palette[index++] = glm::u8vec4(v, 0, 0, 255);
            for (uint8_t v : ramp) palette[index++] = glm::u8vec4(0, v, 0, 255);
            for (uint8_t v : ramp) palette[index++] = glm::u8vec4(0, 0, v, 255);
            for (uint8_t v : ramp) palette[index++] = glm::u8vec4(v, v, v, 255);
            return palette;   // 215 + 40 = 255 colours, entry 0 unused
        }

        std::nullopt_t Fail(std::string* error, std::string message) {
            Log::Error("vox: {}", message);
            if (error) *error = std::move(message);
            return std::nullopt;
        }

        float SrgbToLinear(uint8_t c) {
            const float v = static_cast<float>(c) / 255.0f;
            return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }

        void PutI32(std::vector<uint8_t>& out, int32_t value) {
            const uint32_t v = static_cast<uint32_t>(value);
            out.push_back(static_cast<uint8_t>(v & 0xffu));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
            out.push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
            out.push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
        }

        void PutChunkHeader(std::vector<uint8_t>& out, const char id[4], size_t contentBytes, size_t childrenBytes) {
            out.insert(out.end(), id, id + 4);
            PutI32(out, static_cast<int32_t>(contentBytes));
            PutI32(out, static_cast<int32_t>(childrenBytes));
        }
    }

    std::optional<VoxFile> ParseVox(std::span<const uint8_t> bytes, std::string* error) {
        Reader file(bytes);
        std::span<const uint8_t> magic;
        int32_t version = 0;
        if (!file.ReadSpan(4, magic) || std::memcmp(magic.data(), "VOX ", 4) != 0) return Fail(error, "not a MagicaVoxel file (missing 'VOX ' header)");
        if (!file.ReadI32(version)) return Fail(error, "truncated header");

        // --- MAIN ---
        std::span<const uint8_t> mainId, mainContent, mainChildren;
        int32_t contentBytes = 0, childrenBytes = 0;
        if (!file.ReadSpan(4, mainId) || !file.ReadI32(contentBytes) || !file.ReadI32(childrenBytes)) return Fail(error, "truncated MAIN chunk header");
        if (std::memcmp(mainId.data(), "MAIN", 4) != 0) return Fail(error, "MAIN chunk not found");
        if (contentBytes < 0 || childrenBytes < 0 || !file.ReadSpan(static_cast<size_t>(contentBytes), mainContent) ||
            !file.ReadSpan(static_cast<size_t>(childrenBytes), mainChildren))
            return Fail(error, "MAIN chunk exceeds the file size (truncated file)");

        std::vector<RawModel> rawModels;
        uint64_t totalVoxels = 0;
        std::optional<glm::ivec3> pendingSize;
        std::unordered_map<int32_t, SceneNode> nodes;
        std::vector<int32_t> nodeOrder;   // file order, for a deterministic choice of roots
        std::array<glm::u8vec4, 256> palette{};
        bool hasPalette = false;

        Reader chunks(mainChildren);
        while (!chunks.AtEnd()) {
            std::span<const uint8_t> id, content, children;
            int32_t contentSize = 0, childrenSize = 0;
            if (!chunks.ReadSpan(4, id) || !chunks.ReadI32(contentSize) || !chunks.ReadI32(childrenSize)) return Fail(error, "truncated chunk header");
            const std::string_view name(reinterpret_cast<const char*>(id.data()), 4);
            if (contentSize < 0 || childrenSize < 0 || !chunks.ReadSpan(static_cast<size_t>(contentSize), content) ||
                !chunks.ReadSpan(static_cast<size_t>(childrenSize), children))
                return Fail(error, std::format("chunk '{}' exceeds the file size (truncated file)", name));

            Reader r(content);
            if (name == "SIZE") {
                glm::ivec3 size(0);
                if (!r.ReadI32(size.x) || !r.ReadI32(size.y) || !r.ReadI32(size.z)) return Fail(error, "SIZE chunk too small");
                if (size.x <= 0 || size.y <= 0 || size.z <= 0 || size.x > kMaxModelAxis || size.y > kMaxModelAxis || size.z > kMaxModelAxis ||
                    static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z) > kMaxModelVoxels)
                    return Fail(error, std::format("unsupported model size {} x {} x {}", size.x, size.y, size.z));
                pendingSize = size;
            } else if (name == "XYZI") {
                int32_t count = 0;
                RawModel model;
                if (!pendingSize) return Fail(error, "XYZI chunk without a preceding SIZE chunk");
                if (!r.ReadI32(count) || count < 0 || static_cast<size_t>(count) > r.Remaining() / 4 ||
                    !r.ReadSpan(static_cast<size_t>(count) * 4, model.voxels))
                    return Fail(error, "XYZI chunk smaller than its voxel count");
                model.size = *pendingSize;
                totalVoxels += static_cast<uint64_t>(model.size.x) * static_cast<uint64_t>(model.size.y) * static_cast<uint64_t>(model.size.z);
                if (totalVoxels > kMaxFileVoxels) return Fail(error, "models of the file are larger than the supported total volume");
                pendingSize.reset();
                rawModels.push_back(model);
            } else if (name == "RGBA") {
                std::span<const uint8_t> colours;
                if (!r.ReadSpan(256 * 4, colours)) return Fail(error, "RGBA chunk too small");
                palette[0] = glm::u8vec4(0);
                for (size_t i = 0; i < 255; ++i)
                    palette[i + 1] = glm::u8vec4(colours[i * 4], colours[i * 4 + 1], colours[i * 4 + 2], colours[i * 4 + 3]);
                hasPalette = true;
            } else if (name == "nTRN") {
                int32_t nodeId = 0, childId = 0, reserved = 0, layer = 0, frames = 0;
                Dict attributes;
                if (!r.ReadI32(nodeId) || !r.ReadDict(attributes) || !r.ReadI32(childId) || !r.ReadI32(reserved) || !r.ReadI32(layer) ||
                    !r.ReadI32(frames) || frames < 0)
                    return Fail(error, "malformed nTRN chunk");
                SceneNode node;
                node.kind = NodeKind::Transform;
                node.children.push_back(childId);
                if (const std::string* n = FindKey(attributes, "_name")) node.name = *n;
                for (int32_t f = 0; f < frames; ++f) {
                    Dict frame;
                    if (!r.ReadDict(frame)) return Fail(error, "malformed nTRN frame");
                    if (f == 0)
                        if (const std::string* t = FindKey(frame, "_t")) node.translation = ParseTranslation(*t);
                }
                if (nodes.emplace(nodeId, std::move(node)).second) nodeOrder.push_back(nodeId);
            } else if (name == "nGRP") {
                int32_t nodeId = 0, count = 0;
                Dict attributes;
                if (!r.ReadI32(nodeId) || !r.ReadDict(attributes) || !r.ReadI32(count) || count < 0 || static_cast<size_t>(count) > r.Remaining() / 4)
                    return Fail(error, "malformed nGRP chunk");
                SceneNode node;
                node.kind = NodeKind::Group;
                node.children.resize(static_cast<size_t>(count));
                for (int32_t& child : node.children)
                    if (!r.ReadI32(child)) return Fail(error, "malformed nGRP chunk");
                if (nodes.emplace(nodeId, std::move(node)).second) nodeOrder.push_back(nodeId);
            } else if (name == "nSHP") {
                int32_t nodeId = 0, count = 0;
                Dict attributes;
                if (!r.ReadI32(nodeId) || !r.ReadDict(attributes) || !r.ReadI32(count) || count < 0 || static_cast<size_t>(count) > r.Remaining() / 8)
                    return Fail(error, "malformed nSHP chunk");
                SceneNode node;
                node.kind = NodeKind::Shape;
                for (int32_t i = 0; i < count; ++i) {
                    int32_t modelId = 0;
                    Dict modelAttributes;
                    if (!r.ReadI32(modelId) || !r.ReadDict(modelAttributes)) return Fail(error, "malformed nSHP chunk");
                    node.children.push_back(modelId);
                }
                if (nodes.emplace(nodeId, std::move(node)).second) nodeOrder.push_back(nodeId);
            }
            // Everything else (PACK, MATL, LAYR, rOBJ, rCAM, NOTE, IMAP ...) is skipped.
        }

        if (rawModels.empty()) return Fail(error, "file contains no models");

        VoxFile result;
        if (hasPalette) {
            result.palette = palette;
        } else {
            Log::Warn("vox: no RGBA chunk, using a fallback palette");
            result.palette = FallbackPalette();
        }

        // --- models: MagicaVoxel (x, y, z) -> engine (x, z, sizeY - 1 - y) ---
        result.models.reserve(rawModels.size());
        size_t skipped = 0;
        for (const RawModel& raw : rawModels) {
            VoxModel model;
            model.volume = VoxelVolume(glm::ivec3(raw.size.x, raw.size.z, raw.size.y));
            for (size_t i = 0; i + 3 < raw.voxels.size(); i += 4) {
                const int x = raw.voxels[i], y = raw.voxels[i + 1], z = raw.voxels[i + 2];
                const uint8_t colour = raw.voxels[i + 3];
                if (colour == 0 || x >= raw.size.x || y >= raw.size.y || z >= raw.size.z) {
                    ++skipped;
                    continue;
                }
                model.volume.Set(x, z, raw.size.y - 1 - y, colour);
            }
            result.models.push_back(std::move(model));
        }
        if (skipped != 0) Log::Warn("vox: skipped {} voxels outside their model or with colour index 0", skipped);

        // --- scene graph: accumulate translations from the roots down to the shapes ---
        if (!nodes.empty()) {
            std::unordered_map<int32_t, bool> isChild;
            for (const auto& [id, node] : nodes)
                if (node.kind != NodeKind::Shape)
                    for (int32_t child : node.children) isChild[child] = true;

            std::unordered_map<int32_t, bool> visited;   // also protects against cycles in broken files
            std::vector<bool> placed(result.models.size(), false);
            std::vector<std::pair<int32_t, glm::ivec3>> stack;
            std::vector<std::string> nameStack;          // name of the nearest transform, parallel to `stack`
            for (int32_t root : nodeOrder) {
                if (isChild.contains(root)) continue;
                stack.emplace_back(root, glm::ivec3(0));
                nameStack.emplace_back();
                while (!stack.empty()) {
                    const auto [id, parentTranslation] = stack.back();
                    std::string inheritedName = std::move(nameStack.back());
                    stack.pop_back();
                    nameStack.pop_back();

                    const auto it = nodes.find(id);
                    if (it == nodes.end() || visited[id]) continue;
                    visited[id] = true;
                    const SceneNode& node = it->second;

                    if (node.kind == NodeKind::Shape) {
                        for (int32_t modelId : node.children) {
                            if (modelId < 0 || static_cast<size_t>(modelId) >= result.models.size() || placed[static_cast<size_t>(modelId)]) continue;
                            placed[static_cast<size_t>(modelId)] = true;
                            VoxModel& model = result.models[static_cast<size_t>(modelId)];
                            model.translation = glm::ivec3(parentTranslation.x, parentTranslation.z, -parentTranslation.y);
                            model.name = inheritedName;
                        }
                        continue;
                    }

                    const glm::ivec3 translation = parentTranslation + (node.kind == NodeKind::Transform ? node.translation : glm::ivec3(0));
                    // A group does not rename its children: only the transform directly above a shape names it.
                    const std::string childName = node.kind == NodeKind::Transform ? node.name : std::string();
                    // Reverse order so children are visited in file order.
                    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
                        stack.emplace_back(*child, translation);
                        nameStack.push_back(childName);
                    }
                }
            }
        }

        return result;
    }

    std::optional<VoxFile> LoadVox(const std::filesystem::path& path, std::string* error) {
        // Checked first: a directory can be opened as a stream on some platforms and then
        // reports a nonsensical size.
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return Fail(error, std::format("cannot open '{}': not a file", path.string()));
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) return Fail(error, std::format("cannot read '{}': {}", path.string(), ec.message()));
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return Fail(error, std::format("cannot open '{}'", path.string()));
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!bytes.empty()) stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) return Fail(error, std::format("cannot read '{}'", path.string()));
        return ParseVox(bytes, error);
    }

    std::vector<uint8_t> WriteVox(const VoxFile& file) {
        std::vector<uint8_t> children;
        for (const VoxModel& model : file.models) {
            const glm::ivec3 full = model.volume.Size();
            // XYZI stores coordinates as bytes, so a model can be at most 256 voxels per axis.
            const glm::ivec3 size = glm::min(full, glm::ivec3(kMaxWriteAxis));
            if (size != full)
                Log::Warn("vox: model '{}' ({} x {} x {}) exceeds 256 voxels per axis and is cropped on export", model.name, full.x, full.y, full.z);

            // Engine (x, y, z) -> MagicaVoxel (x, sizeZ - 1 - z, y): the inverse of the import mapping.
            std::vector<uint8_t> voxels;
            for (int z = 0; z < size.z; ++z)
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x) {
                        const uint8_t id = model.volume.Get(x, y, z);
                        if (id == 0) continue;
                        voxels.push_back(static_cast<uint8_t>(x));
                        voxels.push_back(static_cast<uint8_t>(size.z - 1 - z));
                        voxels.push_back(static_cast<uint8_t>(y));
                        voxels.push_back(id);
                    }

            // A degenerate (zero-sized) volume is still written as a 1^3 model without voxels so
            // model indices survive a round trip.
            PutChunkHeader(children, "SIZE", 12, 0);
            PutI32(children, std::max(size.x, 1));
            PutI32(children, std::max(size.z, 1));
            PutI32(children, std::max(size.y, 1));

            PutChunkHeader(children, "XYZI", 4 + voxels.size(), 0);
            PutI32(children, static_cast<int32_t>(voxels.size() / 4));
            children.insert(children.end(), voxels.begin(), voxels.end());
        }

        // Colour of voxel index i is entry i - 1; the last entry is unused.
        PutChunkHeader(children, "RGBA", 256 * 4, 0);
        for (size_t i = 1; i < 256; ++i) {
            const glm::u8vec4& c = file.palette[i];
            children.insert(children.end(), {c.r, c.g, c.b, c.a});
        }
        children.insert(children.end(), {uint8_t{0}, uint8_t{0}, uint8_t{0}, uint8_t{0}});

        std::vector<uint8_t> out;
        out.reserve(children.size() + 20);
        out.insert(out.end(), {uint8_t{'V'}, uint8_t{'O'}, uint8_t{'X'}, uint8_t{' '}});
        PutI32(out, kWriteVersion);
        PutChunkHeader(out, "MAIN", 0, children.size());
        out.insert(out.end(), children.begin(), children.end());
        return out;
    }

    bool SaveVox(const std::filesystem::path& path, const VoxFile& file) {
        const std::vector<uint8_t> bytes = WriteVox(file);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            Log::Error("vox: cannot create '{}'", path.string());
            return false;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            Log::Error("vox: failed to write '{}'", path.string());
            return false;
        }
        return true;
    }

    PaletteId RegisterVoxPalette(MaterialLibrary& library, const VoxFile& file, std::string_view name, const Material& prototype) {
        const PaletteId palette = library.CreatePalette(name);
        for (uint32_t i = 1; i < kPaletteSize; ++i) {
            const glm::u8vec4& c = file.palette[i];
            Material material = prototype;
            material.color = glm::vec3(SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b));
            library.Set(palette, static_cast<uint8_t>(i), material, std::format("color_{}", i));
        }
        return palette;
    }
}
