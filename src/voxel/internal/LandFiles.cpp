module;

#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

module vortex.voxel;

// Reading the drawn map's files and writing the baked map and its review pictures (land.md, world-map.md).

namespace vortex::voxel {

    namespace {
        constexpr float kHeightOffset = 64.0f;   // the world map tool's encoding: value = (m + 64) * 256
        constexpr float kStepsPerMetre = 256.0f;

        bool LoadGrey(const std::filesystem::path& path, bool sixteen, int& w, int& h, std::vector<uint16_t>& out,
                      std::string& error) {
            int comp = 0;
            const std::string name = path.string();
            if (sixteen) {
                stbi_us* data = stbi_load_16(name.c_str(), &w, &h, &comp, 1);
                if (!data) {
                    error = name + ": " + stbi_failure_reason();
                    return false;
                }
                out.assign(data, data + size_t(w) * size_t(h));
                stbi_image_free(data);
            } else {
                stbi_uc* data = stbi_load(name.c_str(), &w, &h, &comp, 1);
                if (!data) {
                    error = name + ": " + stbi_failure_reason();
                    return false;
                }
                out.assign(data, data + size_t(w) * size_t(h));
                stbi_image_free(data);
            }
            return true;
        }

        uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
            static const std::array<uint32_t, 256> table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i) {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            crc = ~crc;
            for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
            return ~crc;
        }

        void Be32(std::vector<uint8_t>& v, uint32_t x) {
            v.push_back(uint8_t(x >> 24));
            v.push_back(uint8_t(x >> 16));
            v.push_back(uint8_t(x >> 8));
            v.push_back(uint8_t(x));
        }

        /// A PNG with stored (uncompressed) deflate blocks: no zlib needed, any bit depth.
        bool WritePng(const std::filesystem::path& path, int w, int h, uint8_t colour, uint8_t depth,
                      const std::vector<uint8_t>& pixels, std::string& error) {
            const size_t stride = pixels.size() / size_t(h);
            std::vector<uint8_t> raw;
            raw.reserve(pixels.size() + size_t(h));
            for (int z = 0; z < h; ++z) {
                raw.push_back(0);
                raw.insert(raw.end(), pixels.begin() + std::ptrdiff_t(size_t(z) * stride),
                           pixels.begin() + std::ptrdiff_t(size_t(z + 1) * stride));
            }
            std::vector<uint8_t> zlib = {0x78, 0x01};
            uint32_t a = 1, b = 0;
            for (uint8_t c : raw) {
                a = (a + c) % 65521;
                b = (b + a) % 65521;
            }
            for (size_t pos = 0; pos < raw.size() || pos == 0;) {
                const size_t len = std::min<size_t>(65535, raw.size() - pos);
                const bool last = pos + len >= raw.size();
                zlib.push_back(last ? 1 : 0);
                zlib.push_back(uint8_t(len));
                zlib.push_back(uint8_t(len >> 8));
                zlib.push_back(uint8_t(~len));
                zlib.push_back(uint8_t(~len >> 8));
                zlib.insert(zlib.end(), raw.begin() + std::ptrdiff_t(pos), raw.begin() + std::ptrdiff_t(pos + len));
                pos += len;
                if (last) break;
            }
            Be32(zlib, (b << 16) | a);

            std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
            auto chunk = [&](const char* kind, const std::vector<uint8_t>& body) {
                Be32(file, uint32_t(body.size()));
                const size_t at = file.size();
                file.insert(file.end(), kind, kind + 4);
                file.insert(file.end(), body.begin(), body.end());
                Be32(file, Crc32(file.data() + at, file.size() - at));
            };
            std::vector<uint8_t> ihdr;
            Be32(ihdr, uint32_t(w));
            Be32(ihdr, uint32_t(h));
            ihdr.insert(ihdr.end(), {depth, colour, 0, 0, 0});
            chunk("IHDR", ihdr);
            chunk("IDAT", zlib);
            chunk("IEND", {});
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
            if (!out) {
                error = path.string() + ": cannot write";
                return false;
            }
            return true;
        }

        uint16_t EncodeHeight(float metres) {
            return uint16_t(std::clamp(std::lround((metres + kHeightOffset) * kStepsPerMetre), 0L, 65535L));
        }

        void Put16(std::vector<uint8_t>& v, uint16_t x) {
            v.push_back(uint8_t(x >> 8));
            v.push_back(uint8_t(x));
        }

        std::array<float, 3> BiomeColour(uint8_t b) {
            switch (LandBiome(b)) {
            case LandBiome::Grassland: return {0.55f, 0.68f, 0.36f};
            case LandBiome::Forest: return {0.27f, 0.47f, 0.26f};
            case LandBiome::DarkForest: return {0.18f, 0.34f, 0.22f};
            case LandBiome::Desert: return {0.82f, 0.71f, 0.48f};
            case LandBiome::Mountain: return {0.60f, 0.57f, 0.53f};
            case LandBiome::RockIsle: return {0.52f, 0.50f, 0.48f};
            default: return {0.50f, 0.60f, 0.40f};
            }
        }
    }

    bool LoadLandSource(const std::filesystem::path& dir, LandSource& out, std::string& error) {
        int w = 0, h = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0;
        std::vector<uint16_t> heights, biomes, regions;
        if (!LoadGrey(dir / "height.png", true, w, h, heights, error)) return false;
        if (!LoadGrey(dir / "biome_ids.png", false, w2, h2, biomes, error)) return false;
        if (!LoadGrey(dir / "region_ids.png", false, w3, h3, regions, error)) return false;
        if (w2 != w || h2 != h || w3 != w || h3 != h) {
            error = "the map's height, biome and region images differ in size";
            return false;
        }
        std::ifstream json(dir / "world.json");
        const std::string text((std::istreambuf_iterator<char>(json)), std::istreambuf_iterator<char>());
        const size_t key = text.find("\"metres_per_px\"");
        if (key == std::string::npos) {
            error = (dir / "world.json").string() + ": no metres_per_px";
            return false;
        }
        const size_t colon = text.find(':', key);
        out.metresPerPixel = std::strtof(text.c_str() + colon + 1, nullptr);
        if (!(out.metresPerPixel > 0.0f)) {
            error = (dir / "world.json").string() + ": bad metres_per_px";
            return false;
        }
        out.width = w;
        out.height = h;
        out.heights.resize(heights.size());
        for (size_t i = 0; i < heights.size(); ++i) out.heights[i] = float(heights[i]) / kStepsPerMetre - kHeightOffset;
        out.biomes.assign(biomes.begin(), biomes.end());
        out.regions.assign(regions.begin(), regions.end());
        return true;
    }

    bool WriteLandFiles(const LandMap& m, const std::filesystem::path& dir, std::string& error) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const int w = m.width, h = m.height;
        const size_t n = size_t(w) * size_t(h);
        std::vector<uint8_t> height16, water16, flow8, relief, change;
        height16.reserve(n * 2);
        water16.reserve(n * 2);
        flow8.reserve(n);
        relief.reserve(n * 3);
        change.reserve(n * 3);
        const float maxFlow = std::log(std::max(1.0f, *std::max_element(m.flow.begin(), m.flow.end())));
        const float minFlow = std::log(m.cellSize * m.cellSize);
        for (int z = 0; z < h; ++z) {
            for (int x = 0; x < w; ++x) {
                const uint32_t i = m.Index(x, z);
                Put16(height16, EncodeHeight(m.heights[i]));
                Put16(water16, m.waterKind[i] == WaterKind::None ? 0 : EncodeHeight(m.water[i]));
                const float f = (std::log(std::max(1.0f, m.flow[i])) - minFlow) / std::max(1.0e-3f, maxFlow - minFlow);
                flow8.push_back(uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f));

                // Shaded relief, light from the north-west.
                const float hl = m.heights[m.Index(std::max(0, x - 1), z)], hr = m.heights[m.Index(std::min(w - 1, x + 1), z)];
                const float hu = m.heights[m.Index(x, std::max(0, z - 1))], hd = m.heights[m.Index(x, std::min(h - 1, z + 1))];
                const float gx = (hr - hl) / (2.0f * m.cellSize), gz = (hd - hu) / (2.0f * m.cellSize);
                const float len = std::sqrt(gx * gx + 1.0f + gz * gz);
                const float shade = std::clamp((gx * 0.577f + 0.577f + gz * 0.577f) / len * 1.25f, 0.25f, 1.25f);
                std::array<float, 3> c = BiomeColour(m.biomes[i]);
                for (float& v : c) v *= shade;
                if (m.heights[i] > 0.0f && std::fmod(m.heights[i], 10.0f) < 0.6f) {
                    for (float& v : c) v *= 0.85f;
                }
                switch (m.waterKind[i]) {
                case WaterKind::Sea: {
                    const float depth = std::clamp(-m.heights[i] / 40.0f, 0.0f, 1.0f);
                    c = {0.20f - 0.10f * depth, 0.36f - 0.15f * depth, 0.58f - 0.18f * depth};
                    break;
                }
                case WaterKind::Lake: c = {0.27f, 0.50f, 0.74f}; break;
                case WaterKind::River: c = {0.25f, 0.58f, 0.90f}; break;
                default: break;
                }
                for (float v : c) relief.push_back(uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f));

                const float d = m.heights[i] - m.rawHeights[i];
                const uint8_t grey = m.waterKind[i] == WaterKind::Sea ? 40 : 110;
                const uint8_t cut = uint8_t(std::clamp(-d / 5.0f, 0.0f, 1.0f) * 145.0f);
                const uint8_t raised = uint8_t(std::clamp(d / 5.0f, 0.0f, 1.0f) * 145.0f);
                change.push_back(uint8_t(grey + cut));
                change.push_back(uint8_t(grey + raised));
                change.push_back(grey);
            }
        }
        return WritePng(dir / "land_height.png", w, h, 0, 16, height16, error) &&
               WritePng(dir / "land_water.png", w, h, 0, 16, water16, error) &&
               WritePng(dir / "land_flow.png", w, h, 0, 8, flow8, error) &&
               WritePng(dir / "land_relief.png", w, h, 2, 8, relief, error) &&
               WritePng(dir / "land_change.png", w, h, 2, 8, change, error);
    }
}
