// A small glTF 2.0 binary (.glb) made in code, for the glTF import tests (MeshImport, EditorImport) and
// for any headless script that needs a model to import: no asset file is kept in the repository.
//
// Header-only, standard library only, so a test of any library (or the game's editor script) can
// include it. Nothing here is engine code; the names live in vortex::fixtures so they never clash with
// an engine type.
//
// The model, in glTF units (y up), under one root node with three children:
//   node "base"    unit cube scaled (3, 1, 2) at (0, 0, 0)   material 0 "stone": baseColorFactor (0.2, 0.3, 0.4)
//   node "pillar"  unit cube scaled (1, 2, 1) at (0, 1, 0)   material 1 "brick": baseColorFactor (0.6, 0.2, 0.1)
//   node "crate"   unit cube at (2, 1, 1)                    material 2 "paint": factor 1 times a 2 x 2 RGB PNG
//                                                            texture (red, green, blue, white): linear average 0.5
// Bounds: (0, 0, 0) - (3, 3, 2). All three meshes share one position accessor (8 corners) and one
// 16-bit index accessor (36 indices); the PNG sits in its own buffer view (mimeType image/png).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace vortex::fixtures {

    /// Bounds of the model in glTF units, and the expected linear colour of each material.
    inline constexpr std::array<float, 3> kBoxesGlbSize{3.0f, 3.0f, 2.0f};
    inline constexpr std::array<std::array<float, 3>, 3> kBoxesGlbColors{{{0.2f, 0.3f, 0.4f}, {0.6f, 0.2f, 0.1f}, {0.5f, 0.5f, 0.5f}}};

    namespace gltf_fixture_detail {
        inline uint32_t Crc32(const uint8_t* data, size_t size) {
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < size; ++i) {
                crc ^= data[i];
                for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        inline void PutBigEndian(std::vector<uint8_t>& out, uint32_t value) {
            for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(value >> shift));
        }

        inline void PutChunk(std::vector<uint8_t>& png, std::string_view type, const std::vector<uint8_t>& data) {
            PutBigEndian(png, static_cast<uint32_t>(data.size()));
            std::vector<uint8_t> crcInput(type.begin(), type.end());
            crcInput.insert(crcInput.end(), data.begin(), data.end());
            png.insert(png.end(), crcInput.begin(), crcInput.end());
            PutBigEndian(png, Crc32(crcInput.data(), crcInput.size()));
        }

        /// An 8-bit RGB PNG of `width` x `height` pixels (row-major `rgb`), compressed with one stored
        /// deflate block (no compression), which every PNG decoder reads.
        inline std::vector<uint8_t> RgbPng(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb) {
            std::vector<uint8_t> raw;   // each row: filter type 0, then its pixels
            for (uint32_t y = 0; y < height; ++y) {
                raw.push_back(0);
                raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y * width * 3), rgb.begin() + static_cast<std::ptrdiff_t>((y + 1) * width * 3));
            }
            std::vector<uint8_t> zlib{0x78, 0x01, 0x01};   // zlib header; final stored block
            const uint16_t length = static_cast<uint16_t>(raw.size());
            zlib.push_back(static_cast<uint8_t>(length));
            zlib.push_back(static_cast<uint8_t>(length >> 8));
            zlib.push_back(static_cast<uint8_t>(~length));
            zlib.push_back(static_cast<uint8_t>(static_cast<uint16_t>(~length) >> 8));
            zlib.insert(zlib.end(), raw.begin(), raw.end());
            uint32_t a = 1, b = 0;   // Adler-32
            for (uint8_t byte : raw) {
                a = (a + byte) % 65521u;
                b = (b + a) % 65521u;
            }
            PutBigEndian(zlib, (b << 16) | a);

            std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            std::vector<uint8_t> header;
            PutBigEndian(header, width);
            PutBigEndian(header, height);
            header.insert(header.end(), {8, 2, 0, 0, 0});   // 8 bits, RGB, deflate, adaptive filter, no interlace
            PutChunk(png, "IHDR", header);
            PutChunk(png, "IDAT", zlib);
            PutChunk(png, "IEND", {});
            return png;
        }

        template <typename T>
        void Append(std::vector<uint8_t>& out, const T& value) {
            const size_t at = out.size();
            out.resize(at + sizeof(T));
            std::memcpy(out.data() + at, &value, sizeof(T));
        }

        inline void PadTo4(std::vector<uint8_t>& out, uint8_t fill) {
            while (out.size() % 4 != 0) out.push_back(fill);
        }
    }

    /// The model as the bytes of a .glb file.
    inline std::vector<uint8_t> BoxesGlb() {
        using namespace gltf_fixture_detail;

        // --- binary chunk: positions (8 x vec3), indices (36 x u16), the PNG ---
        std::vector<uint8_t> bin;
        for (uint16_t i = 0; i < 8; ++i) {
            Append(bin, static_cast<float>(i & 1));
            Append(bin, static_cast<float>((i >> 1) & 1));
            Append(bin, static_cast<float>((i >> 2) & 1));
        }
        const size_t positionBytes = bin.size();
        // Corner i is (i & 1, i >> 1 & 1, i >> 2 & 1); two triangles per face, counter-clockwise outside.
        constexpr std::array<uint16_t, 36> kIndices{0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
                                                    2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};
        for (uint16_t index : kIndices) Append(bin, index);
        const size_t indexBytes = bin.size() - positionBytes;
        PadTo4(bin, 0);
        const size_t imageOffset = bin.size();
        const std::vector<uint8_t> png = RgbPng(2, 2, {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255});
        bin.insert(bin.end(), png.begin(), png.end());
        const size_t imageBytes = png.size();
        PadTo4(bin, 0);

        // --- JSON chunk ---
        const std::string json =
            std::string(R"({"asset":{"version":"2.0","generator":"vortex tests/support/GltfFixture.h"},)") +
            R"("scene":0,"scenes":[{"nodes":[0]}],)" +
            R"("nodes":[{"name":"boxes","children":[1,2,3]},)" +
            R"({"name":"base","mesh":0,"scale":[3,1,2]},)" +
            R"({"name":"pillar","mesh":1,"translation":[0,1,0],"scale":[1,2,1]},)" +
            R"({"name":"crate","mesh":2,"translation":[2,1,1]}],)" +
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]},)" +
            R"({"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":1}]},)" +
            R"({"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":2}]}],)" +
            R"("materials":[{"name":"stone","pbrMetallicRoughness":{"baseColorFactor":[0.2,0.3,0.4,1]}},)" +
            R"({"name":"brick","pbrMetallicRoughness":{"baseColorFactor":[0.6,0.2,0.1,1]}},)" +
            R"({"name":"paint","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)" +
            R"("textures":[{"source":0}],)" +
            R"("images":[{"bufferView":2,"mimeType":"image/png"}],)" +
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":8,"type":"VEC3","min":[0,0,0],"max":[1,1,1]},)" +
            R"({"bufferView":1,"componentType":5123,"count":36,"type":"SCALAR"}],)" +
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" + std::to_string(positionBytes) + "}," +
            R"({"buffer":0,"byteOffset":)" + std::to_string(positionBytes) + R"(,"byteLength":)" + std::to_string(indexBytes) + "}," +
            R"({"buffer":0,"byteOffset":)" + std::to_string(imageOffset) + R"(,"byteLength":)" + std::to_string(imageBytes) + "}]," +
            R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]}";
        std::vector<uint8_t> jsonChunk(json.begin(), json.end());
        PadTo4(jsonChunk, ' ');

        // --- container: header, JSON chunk, BIN chunk ---
        std::vector<uint8_t> glb;
        Append(glb, uint32_t{0x46546C67});   // "glTF"
        Append(glb, uint32_t{2});
        Append(glb, static_cast<uint32_t>(12 + 8 + jsonChunk.size() + 8 + bin.size()));
        Append(glb, static_cast<uint32_t>(jsonChunk.size()));
        Append(glb, uint32_t{0x4E4F534A});   // "JSON"
        glb.insert(glb.end(), jsonChunk.begin(), jsonChunk.end());
        Append(glb, static_cast<uint32_t>(bin.size()));
        Append(glb, uint32_t{0x004E4942});   // "BIN\0"
        glb.insert(glb.end(), bin.begin(), bin.end());
        return glb;
    }

    /// Writes BoxesGlb() to `path` (a .glb); false if the file cannot be written.
    inline bool WriteBoxesGlb(const std::filesystem::path& path) {
        const std::vector<uint8_t> bytes = BoxesGlb();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    }
}
