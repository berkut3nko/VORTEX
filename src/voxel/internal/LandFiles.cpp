module;

#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    namespace {
        /// The starter region's fine grid at 1 m: shaded relief, water, roads in brown, the site's ground lighter, and the
        /// points of interest as squares (castle red, village yellow, landing white, cave black, landmark violet,
        /// crossing cyan).
        bool WriteStarterPicture(const LandMap& m, const std::filesystem::path& path, std::string& error) {
            const LandFine& f = m.fine;
            const int w = f.width, h = f.height;
            std::vector<float> height(size_t(w) * size_t(h));
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i) {
                    const float mx = float(f.x0 + i) + 0.5f, mz = float(f.z0 + j) + 0.5f;
                    height[f.Index(i, j)] = LandCoarseHeight(m, mx, mz) + f.delta[f.Index(i, j)];
                }
            std::vector<uint8_t> rgb;
            rgb.reserve(height.size() * 3);
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i) {
                    const uint32_t k = f.Index(i, j);
                    const float mx = float(f.x0 + i) + 0.5f, mz = float(f.z0 + j) + 0.5f;
                    const int cx = std::clamp(int(mx / m.cellSize), 0, m.width - 1), cz = std::clamp(int(mz / m.cellSize), 0, m.height - 1);
                    const uint32_t cell = m.Index(cx, cz);
                    const float gx = (height[f.Index(std::min(w - 1, i + 1), j)] - height[f.Index(std::max(0, i - 1), j)]) * 0.5f;
                    const float gz = (height[f.Index(i, std::min(h - 1, j + 1))] - height[f.Index(i, std::max(0, j - 1))]) * 0.5f;
                    const float len = std::sqrt(gx * gx + 1.0f + gz * gz);
                    const float shade = std::clamp((gx * 0.577f + 0.577f + gz * 0.577f) / len * 1.25f, 0.2f, 1.3f);
                    std::array<float, 3> c = BiomeColour(m.biomes[cell]);
                    if (f.surface[k] == LandSurface::Site) c = {0.70f, 0.78f, 0.52f};
                    if (f.surface[k] == LandSurface::StreamBed) c = {0.78f, 0.74f, 0.60f};
                    if (f.surface[k] == LandSurface::Road) c = {0.55f, 0.40f, 0.25f};
                    for (float& v : c) v *= shade;
                    if (std::fmod(height[k], 5.0f) < 0.25f && height[k] > 0.0f) {
                        for (float& v : c) v *= 0.8f;
                    }
                    if (m.waterKind[cell] != WaterKind::None && height[k] < m.water[cell]) {
                        c = m.waterKind[cell] == WaterKind::Sea ? std::array<float, 3>{0.18f, 0.32f, 0.55f}
                                                                 : std::array<float, 3>{0.25f, 0.56f, 0.88f};
                        if (f.surface[k] == LandSurface::Road) c = {0.45f, 0.55f, 0.65f};
                    }
                    for (float v : c) rgb.push_back(uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                }
            for (const LandPoint& p : m.points) {
                std::array<uint8_t, 3> c{255, 255, 255};
                switch (p.kind) {
                case LandPointKind::Castle: c = {220, 30, 30}; break;
                case LandPointKind::Village: c = {250, 220, 40}; break;
                case LandPointKind::Landing: c = {255, 255, 255}; break;
                case LandPointKind::Cave: c = {10, 10, 10}; break;
                case LandPointKind::Landmark: c = {170, 60, 220}; break;
                case LandPointKind::Crossing: c = {40, 230, 230}; break;
                }
                const int pi = int(p.x) - f.x0, pj = int(p.z) - f.z0;
                for (int dj = -5; dj <= 5; ++dj)
                    for (int di = -5; di <= 5; ++di) {
                        const int i = pi + di, j = pj + dj;
                        if (i < 0 || j < 0 || i >= w || j >= h) continue;
                        const size_t at = size_t(f.Index(i, j)) * 3;
                        const bool rim = std::abs(di) == 5 || std::abs(dj) == 5;
                        for (int ch = 0; ch < 3; ++ch) rgb[at + size_t(ch)] = rim ? 0 : c[size_t(ch)];
                    }
            }
            return WritePng(path, w, h, 2, 8, rgb, error);
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
        const size_t starter = text.find("\"starter_region\"");
        if (starter != std::string::npos) out.starterRegion = std::atoi(text.c_str() + text.find(':', starter) + 1);
        out.width = w;
        out.height = h;
        out.heights.resize(heights.size());
        for (size_t i = 0; i < heights.size(); ++i) out.heights[i] = float(heights[i]) / kStepsPerMetre - kHeightOffset;
        out.biomes.assign(biomes.begin(), biomes.end());
        out.regions.assign(regions.begin(), regions.end());
        out.points.clear();
        std::error_code ec;
        if (std::filesystem::exists(dir / "points.json", ec)) {
            // {"points": [{"kind": "village", "x": 2450.0, "z": 760.0}, ...]}: each object's kind, then its x and z.
            std::ifstream file(dir / "points.json");
            const std::string points((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto number = [&](size_t from, size_t to, const char* name, float& value) {
                const size_t at = points.find(name, from);
                if (at == std::string::npos || at >= to) return false;
                value = std::strtof(points.c_str() + points.find(':', at) + 1, nullptr);
                return true;
            };
            for (size_t at = points.find("\"kind\""); at != std::string::npos;) {
                const size_t next = points.find("\"kind\"", at + 6);
                const size_t end = next == std::string::npos ? points.size() : next;
                const size_t open = points.find('"', points.find(':', at) + 1);
                const size_t close = points.find('"', open + 1);
                const std::string name = points.substr(open + 1, close - open - 1);
                LandPoint p;
                p.fromFile = true;
                bool known = false;
                for (uint8_t k = 0; k <= uint8_t(LandPointKind::Crossing); ++k) {
                    if (name == LandPointName(LandPointKind(k))) {
                        p.kind = LandPointKind(k);
                        known = true;
                    }
                }
                if (!known || !number(at, end, "\"x\"", p.x) || !number(at, end, "\"z\"", p.z)) {
                    error = (dir / "points.json").string() + ": a point needs a known kind, x and z ('" + name + "')";
                    return false;
                }
                if (p.kind != LandPointKind::Crossing) out.points.push_back(p);
                at = next;
            }
        }
        return true;
    }

    const char* LandPointName(LandPointKind kind) {
        switch (kind) {
        case LandPointKind::Castle: return "castle";
        case LandPointKind::Village: return "village";
        case LandPointKind::Landing: return "landing";
        case LandPointKind::Cave: return "cave";
        case LandPointKind::Landmark: return "landmark";
        case LandPointKind::Crossing: return "crossing";
        }
        return "?";
    }

    bool WriteLandPoints(const LandMap& m, const std::filesystem::path& path, std::string& error) {
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path);
        out << "{\n \"format\": 1,\n \"points\": [\n";
        for (size_t k = 0; k < m.points.size(); ++k) {
            const LandPoint& p = m.points[k];
            char line[160];
            std::snprintf(line, sizeof line, "  {\"kind\": \"%s\", \"x\": %.1f, \"z\": %.1f}%s\n", LandPointName(p.kind), double(p.x),
                          double(p.z), k + 1 < m.points.size() ? "," : "");
            out << line;
        }
        out << " ]\n}\n";
        if (!out) {
            error = path.string() + ": cannot write";
            return false;
        }
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
        if (!(WritePng(dir / "land_height.png", w, h, 0, 16, height16, error) &&
              WritePng(dir / "land_water.png", w, h, 0, 16, water16, error) &&
              WritePng(dir / "land_flow.png", w, h, 0, 8, flow8, error) &&
              WritePng(dir / "land_relief.png", w, h, 2, 8, relief, error) &&
              WritePng(dir / "land_change.png", w, h, 2, 8, change, error))) {
            return false;
        }
        return m.fine.Empty() || WriteStarterPicture(m, dir / "land_starter.png", error);
    }
}
