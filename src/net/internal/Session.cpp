module;

#include <glm/glm.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

module vortex.net;

import vortex.core;
import vortex.voxel;
import vortex.sim;

namespace vortex::net {

    namespace {
        std::string_view Trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
            return s;
        }

        std::vector<std::string_view> Words(std::string_view s) {
            std::vector<std::string_view> out;
            size_t i = 0;
            while (i < s.size()) {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                const size_t start = i;
                while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
                if (i > start) out.push_back(s.substr(start, i - start));
            }
            return out;
        }

        template <typename T>
        bool Number(std::string_view text, T& out) {
            const char* end = text.data() + text.size();
            const auto r = std::from_chars(text.data(), end, out);
            return r.ec == std::errc() && r.ptr == end;
        }

        template <typename T>
        bool Numbers(std::string_view text, T* out, size_t count) {
            const std::vector<std::string_view> words = Words(text);
            if (words.size() != count) return false;
            for (size_t i = 0; i < count; ++i)
                if (!Number(words[i], out[i])) return false;
            return true;
        }

        bool Bool(std::string_view text, bool& out) {
            if (text == "1" || text == "true" || text == "yes") out = true;
            else if (text == "0" || text == "false" || text == "no") out = false;
            else return false;
            return true;
        }
    }

    bool ParseToken(std::string_view hex, Token& out) {
        if (hex.size() != 32) return false;
        for (size_t i = 0; i < 16; ++i) {
            uint8_t byte = 0;
            const auto r = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, byte, 16);
            if (r.ec != std::errc() || r.ptr != hex.data() + 2 * i + 2) return false;
            out[i] = byte;
        }
        return true;
    }

    std::string FormatToken(const Token& token) {
        std::string out;
        for (const uint8_t b : token) out += std::format("{:02x}", b);
        return out;
    }

    bool ParseSession(std::string_view text, SessionConfig& out, std::string& error) {
        SessionConfig config;
        bool tokenSet = false;
        int lineNumber = 0;
        while (!text.empty()) {
            const size_t eol = text.find('\n');
            std::string_view line = text.substr(0, eol);
            text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
            ++lineNumber;
            if (const size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
            line = Trim(line);
            if (line.empty()) continue;
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos) {
                error = std::format("line {}: expected 'key = value'", lineNumber);
                return false;
            }
            const std::string_view key = Trim(line.substr(0, eq));
            const std::string_view value = Trim(line.substr(eq + 1));
            sim::RegionDesc& r = config.region;
            bool ok = true;
            if (key == "token") {
                ok = ParseToken(value, config.token);
                tokenSet = ok;
            } else if (key == "template") {
                if (value == "empty") r.templateId = sim::RegionTemplateId::Empty;
                else if (value == "playground") r.templateId = sim::RegionTemplateId::Playground;
                else ok = false;
            } else if (key == "seed") {
                ok = Number(value, r.seed);
            } else if (key == "terrain_seed") {
                ok = Number(value, r.terrain.seed);
            } else if (key == "play_min") {
                int v[2];
                ok = Numbers(value, v, 2);
                if (ok) r.playMinChunk = glm::ivec2(v[0], v[1]);
            } else if (key == "play_max") {
                int v[2];
                ok = Numbers(value, v, 2);
                if (ok) r.playMaxChunk = glm::ivec2(v[0], v[1]);
            } else if (key == "height") {
                float v[2];
                ok = Numbers(value, v, 2);
                if (ok) r.heightRange = glm::vec2(v[0], v[1]);
            } else if (key == "voxel_size") {
                ok = Number(value, r.terrain.voxelSize);
            } else if (key == "generator") {
                if (value == "terrain") r.generator = sim::RegionGenerator::Terrain;
                else if (value == "flat") r.generator = sim::RegionGenerator::Flat;
                else ok = false;
            } else if (key == "flat_ground") {
                ok = Number(value, r.flatGroundVoxels);
            } else if (key == "max_clients") {
                ok = Number(value, config.maxClients) && config.maxClients >= 1 && config.maxClients <= 255;
            } else if (key == "chunk_capacity") {
                ok = Number(value, config.chunkCapacity);
            } else if (key == "pack_hidden") {
                ok = Bool(value, config.packHidden);
            } else if (key == "port") {
                ok = Number(value, config.port);
            } else {
                error = std::format("line {}: unknown key '{}'", lineNumber, key);
                return false;
            }
            if (!ok) {
                error = std::format("line {}: bad value '{}' for '{}'", lineNumber, value, key);
                return false;
            }
        }
        if (!tokenSet) {
            error = "no token (32 hex digits)";
            return false;
        }
        std::string why;
        if (!sim::IsValid(config.region, &why)) {
            error = "invalid region: " + why;
            return false;
        }
        out = config;
        return true;
    }

    bool LoadSessionFile(const std::filesystem::path& path, SessionConfig& out, std::string& error) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = "cannot open " + path.string();
            return false;
        }
        std::stringstream text;
        text << file.rdbuf();
        const std::string s = text.str();
        if (!ParseSession(s, out, error)) {
            error = path.string() + ": " + error;
            return false;
        }
        return true;
    }

    std::string FormatSession(const SessionConfig& c) {
        const sim::RegionDesc& r = c.region;
        std::string s;
        s += std::format("token = {}\n", FormatToken(c.token));
        s += std::format("template = {}\n", r.templateId == sim::RegionTemplateId::Playground ? "playground" : "empty");
        s += std::format("seed = {}\n", r.seed);
        s += std::format("terrain_seed = {}\n", r.terrain.seed);
        s += std::format("play_min = {} {}\n", r.playMinChunk.x, r.playMinChunk.y);
        s += std::format("play_max = {} {}\n", r.playMaxChunk.x, r.playMaxChunk.y);
        s += std::format("height = {} {}\n", r.heightRange.x, r.heightRange.y);
        s += std::format("voxel_size = {}\n", r.terrain.voxelSize);
        s += std::format("generator = {}\n", r.generator == sim::RegionGenerator::Flat ? "flat" : "terrain");
        s += std::format("flat_ground = {}\n", r.flatGroundVoxels);
        s += std::format("max_clients = {}\n", c.maxClients);
        s += std::format("chunk_capacity = {}\n", c.chunkCapacity);
        s += std::format("pack_hidden = {}\n", c.packHidden ? 1 : 0);
        s += std::format("port = {}\n", c.port);
        return s;
    }

    uint64_t BuildHash(const voxel::MaterialLibrary& materials) {
        uint64_t h = voxel::HashMix64(0x56425549ull, kProtocolVersion);   // "VBUI"
        h = voxel::HashMix64(h, voxel::kGeneratorGoldenHash);
        return voxel::HashMix64(h, voxel::MaterialTableHash(materials));
    }

    namespace {
        uint64_t PristineHash(const voxel::IWorldGenerator& generator, const glm::ivec3& coord, voxel::Chunk& scratch) {
            scratch.Clear();
            const voxel::ChunkContent content = generator.Generate(coord, scratch);
            return voxel::ChunkHash(content.kind, content.uniformId, &scratch);
        }

    }

    std::vector<ProbeHash> ProbeChunks(const sim::RegionDesc& region, const voxel::IWorldGenerator& generator, uint32_t count) {
        std::vector<ProbeHash> out;
        const glm::ivec3 min = sim::RegionWindowMinChunk(region);
        const glm::ivec3 size = sim::RegionWindowSize(region);
        const glm::ivec2 rows = sim::RegionRows(region);
        const int rowMin = rows.x, rowMax = rows.y;
        Rng rng(static_cast<uint64_t>(region.seed) * 0x9e3779b97f4a7c15ull + 0x50524f42ull, 0x70726f6265ull);   // "PROB", "probe"
        auto chunk = std::make_unique<voxel::Chunk>();
        for (uint32_t i = 0; i < count; ++i) {
            const int cx = min.x + static_cast<int>(rng.NextBounded(static_cast<uint32_t>(size.x)));
            const int cz = min.z + static_cast<int>(rng.NextBounded(static_cast<uint32_t>(size.z)));
            int lo = rowMin, hi = rowMax;
            const voxel::ColumnClass column = generator.ClassifyColumn(cx, cz);
            if (column.known) {
                // The row of the column's lowest surface voxel and the one above: terrain, not sky.
                lo = std::clamp(column.surfaceFrom + 1, rowMin, rowMax - 1);
                hi = std::min({lo + 2, rowMax, std::max(column.emptyFrom, lo + 1)});
            }
            const int cy = lo + static_cast<int>(rng.NextBounded(static_cast<uint32_t>(hi - lo)));
            const glm::ivec3 coord(cx, cy, cz);
            out.push_back({coord, PristineHash(generator, coord, *chunk)});
        }
        return out;
    }

    uint32_t CheckProbes(const std::vector<ProbeHash>& probes, const voxel::IWorldGenerator& generator) {
        auto chunk = std::make_unique<voxel::Chunk>();
        uint32_t mismatches = 0;
        for (const ProbeHash& p : probes) mismatches += PristineHash(generator, p.chunk, *chunk) != p.hash ? 1u : 0u;
        return mismatches;
    }

    std::vector<ProbeHash> RehashProbes(const std::vector<ProbeHash>& probes, const voxel::IWorldGenerator& generator) {
        auto chunk = std::make_unique<voxel::Chunk>();
        std::vector<ProbeHash> out = probes;
        for (ProbeHash& p : out) p.hash = PristineHash(generator, p.chunk, *chunk);
        return out;
    }

    uint32_t CheckProbes(const std::vector<ProbeHash>& probes, const voxel::World& world) {
        uint32_t mismatches = 0;
        for (const ProbeHash& p : probes) {
            // A one-chunk WorldHash: the same per-state hashing (Packed chunks from their runs) as the session's hash.
            const voxel::WorldHashResult h = voxel::WorldHash(world, p.chunk, p.chunk + glm::ivec3(1));
            const uint64_t expect = p.hash == voxel::kAirChunkHash ? 0 : voxel::WorldHashTerm(p.chunk, p.hash);
            mismatches += !h.Complete() || h.hash != expect ? 1u : 0u;
        }
        return mismatches;
    }

    voxel::WorldHashResult RegionHash(const voxel::World& world, const sim::RegionDesc& region) {
        const glm::ivec2 rows = sim::RegionRows(region);
        glm::ivec3 min = sim::RegionWindowMinChunk(region);
        min.y = rows.x;
        const glm::ivec3 size = sim::RegionWindowSize(region);
        return voxel::WorldHash(world, min, min + glm::ivec3(size.x, rows.y - rows.x, size.z));
    }

    namespace {
        class BrokenChunkGenerator final : public voxel::IWorldGenerator {
        public:
            BrokenChunkGenerator(std::shared_ptr<voxel::IWorldGenerator> inner, std::vector<glm::ivec3> chunks, uint8_t stone)
                : m_Inner(std::move(inner)), m_Chunks(std::move(chunks)), m_Stone(stone) {}

            voxel::ChunkContent Generate(const glm::ivec3& coord, voxel::Chunk& chunk) const override {
                voxel::ChunkContent content = m_Inner->Generate(coord, chunk);
                if (std::find(m_Chunks.begin(), m_Chunks.end(), coord) == m_Chunks.end()) return content;
                if (content.kind != voxel::ChunkKind::Dense) {
                    std::fill(std::begin(chunk.ids), std::end(chunk.ids), content.kind == voxel::ChunkKind::Uniform ? content.uniformId : uint8_t{0});
                    content.kind = voxel::ChunkKind::Dense;
                }
                // 64 voxels, every 512th of the chunk's indices (spread over its blocks).
                for (int i = 0; i < voxel::kChunkVolume; i += voxel::kChunkVolume / 64) chunk.ids[i] = chunk.ids[i] == 0 ? m_Stone : uint8_t{0};
                return content;
            }
            voxel::HeightBounds VerticalBounds() const override { return m_Inner->VerticalBounds(); }
            voxel::ColumnClass ClassifyColumn(int cx, int cz) const override { return m_Inner->ClassifyColumn(cx, cz); }

        private:
            std::shared_ptr<voxel::IWorldGenerator> m_Inner;
            std::vector<glm::ivec3> m_Chunks;
            uint8_t m_Stone = 1;
        };
    }

    std::shared_ptr<voxel::IWorldGenerator> BreakChunks(std::shared_ptr<voxel::IWorldGenerator> inner, std::vector<glm::ivec3> chunks,
                                                        uint8_t stoneId) {
        return std::make_shared<BrokenChunkGenerator>(std::move(inner), std::move(chunks), stoneId);
    }

    voxel::ChunkContent SessionGenerator::Generate(const glm::ivec3& coord, voxel::Chunk& chunk) const {
        const bool inside = coord.x >= m_Min.x && coord.y >= m_Min.y && coord.z >= m_Min.z && coord.x < m_Max.x && coord.y < m_Max.y &&
                            coord.z < m_Max.z;
        if (inside && m_TerrainMode.load()) {
            m_Skipped.fetch_add(1, std::memory_order_relaxed);
            return {};          // Empty: the server's TerrainAck and ChunkData give this chunk
        }
        const voxel::ChunkContent content = m_Inner->Generate(coord, chunk);
        if (inside && content.kind == voxel::ChunkKind::Dense && m_TerrainMode.load()) m_DenseInside.fetch_add(1, std::memory_order_relaxed);
        return content;
    }
}
