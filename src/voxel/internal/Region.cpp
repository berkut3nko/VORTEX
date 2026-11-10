module;

#include <glm/glm.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

module vortex.voxel;

import vortex.core;

namespace vortex::voxel {

    // -----------------------------------------------------------------------------------------
    //  Chunk codec
    //    byte 0        kind (0 = empty, 1 = uniform, 2 = dense)
    //    uniform:      byte 1 = material id
    //    dense:        runs of [id u8][length u16 LE] over the block-ordered id array
    // -----------------------------------------------------------------------------------------
    std::vector<uint8_t> EncodeChunk(ChunkKind kind, uint8_t uniformId, const Chunk* chunk) {
        std::vector<uint8_t> out;
        if (kind == ChunkKind::Dense && chunk == nullptr) kind = ChunkKind::Empty;
        out.push_back(static_cast<uint8_t>(kind));
        if (kind == ChunkKind::Uniform) {
            out.push_back(uniformId);
        } else if (kind == ChunkKind::Dense) {
            out.reserve(1024);
            int i = 0;
            while (i < kChunkVolume) {
                const uint8_t id = chunk->ids[i];
                int run = 1;
                while (i + run < kChunkVolume && run < 0xFFFF && chunk->ids[i + run] == id) ++run;
                out.push_back(id);
                out.push_back(static_cast<uint8_t>(run & 0xFF));
                out.push_back(static_cast<uint8_t>(run >> 8));
                i += run;
            }
        }
        return out;
    }

    bool DecodeChunk(std::span<const uint8_t> bytes, ChunkKind& kind, uint8_t& uniformId, Chunk& chunk) {
        if (bytes.empty() || bytes[0] > 2) return false;
        kind = static_cast<ChunkKind>(bytes[0]);
        uniformId = 0;
        if (kind == ChunkKind::Empty) return bytes.size() == 1;
        if (kind == ChunkKind::Uniform) {
            if (bytes.size() != 2 || bytes[1] == 0) return false;
            uniformId = bytes[1];
            return true;
        }
        if ((bytes.size() - 1) % 3 != 0) return false;
        int i = 0;
        for (size_t p = 1; p + 2 < bytes.size(); p += 3) {
            const uint8_t id = bytes[p];
            const int run = bytes[p + 1] | (bytes[p + 2] << 8);
            if (run == 0 || i + run > kChunkVolume) return false;
            std::memset(chunk.ids + i, id, static_cast<size_t>(run));
            i += run;
        }
        if (i != kChunkVolume) return false;
        chunk.RebuildOccupancy();
        return true;
    }

    // -----------------------------------------------------------------------------------------
    //  Region files
    // -----------------------------------------------------------------------------------------
    struct RegionStore::Impl {
        struct Region {
            std::unordered_map<uint16_t, std::vector<uint8_t>> chunks;
            bool dirty = false;
        };

        std::filesystem::path directory;
        mutable std::mutex mutex;
        std::unordered_map<uint64_t, Region> regions;

        static constexpr int kRegionMask = (1 << kRegionShift) - 1;

        static uint64_t Key(const glm::ivec3& r) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(r.x) & 0x1FFFFFu)) |
                   (static_cast<uint64_t>(static_cast<uint32_t>(r.y) & 0x1FFFFFu) << 21) |
                   (static_cast<uint64_t>(static_cast<uint32_t>(r.z) & 0x1FFFFFu) << 42);
        }
        static glm::ivec3 Unkey(uint64_t key) {
            auto sign21 = [](uint64_t v) {
                const int32_t i = static_cast<int32_t>(v & 0x1FFFFFu);
                return (i & 0x100000) ? i - 0x200000 : i;
            };
            return {sign21(key), sign21(key >> 21), sign21(key >> 42)};
        }
        static uint16_t LocalIndex(const glm::ivec3& chunk) {
            const glm::ivec3 l = chunk & kRegionMask;
            return static_cast<uint16_t>(l.x + (l.y << kRegionShift) + (l.z << (2 * kRegionShift)));
        }

        std::filesystem::path FilePath(const glm::ivec3& r) const {
            return directory / ("r." + std::to_string(r.x) + "." + std::to_string(r.y) + "." + std::to_string(r.z) + ".vxr");
        }

        // Must be called with the mutex held.
        Region& Acquire(const glm::ivec3& r) {
            const uint64_t key = Key(r);
            auto it = regions.find(key);
            if (it != regions.end()) return it->second;
            Region& region = regions[key];
            if (!directory.empty()) ReadFile(r, region);
            return region;
        }

        void ReadFile(const glm::ivec3& r, Region& region) const {
            std::ifstream in(FilePath(r), std::ios::binary);
            if (!in) return;
            char magic[4];
            uint32_t count = 0;
            in.read(magic, 4);
            in.read(reinterpret_cast<char*>(&count), 4);
            if (!in || std::memcmp(magic, "VXR1", 4) != 0 || count > 4096) {
                Log::Warn("Region file {} is corrupt and was ignored", FilePath(r).string());
                return;
            }
            for (uint32_t i = 0; i < count; ++i) {
                uint16_t local = 0;
                uint32_t size = 0;
                in.read(reinterpret_cast<char*>(&local), 2);
                in.read(reinterpret_cast<char*>(&size), 4);
                if (!in || size > 3u * kChunkVolume + 16u) {
                    Log::Warn("Region file {} is truncated", FilePath(r).string());
                    return;
                }
                std::vector<uint8_t> bytes(size);
                in.read(reinterpret_cast<char*>(bytes.data()), size);
                if (!in) return;
                region.chunks[local] = std::move(bytes);
            }
        }

        bool WriteFile(const glm::ivec3& r, const Region& region) const {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            const std::filesystem::path path = FilePath(r);
            std::filesystem::path tmp = path;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) return false;
                const uint32_t count = static_cast<uint32_t>(region.chunks.size());
                out.write("VXR1", 4);
                out.write(reinterpret_cast<const char*>(&count), 4);
                for (const auto& [local, bytes] : region.chunks) {
                    const uint32_t size = static_cast<uint32_t>(bytes.size());
                    out.write(reinterpret_cast<const char*>(&local), 2);
                    out.write(reinterpret_cast<const char*>(&size), 4);
                    out.write(reinterpret_cast<const char*>(bytes.data()), size);
                }
                if (!out) return false;
            }
            // Rename is atomic: a crash mid-save never leaves a half-written region behind.
            std::filesystem::rename(tmp, path, ec);
            return !ec;
        }
    };

    RegionStore::RegionStore(std::filesystem::path directory) : m_Impl(std::make_unique<Impl>()) {
        m_Impl->directory = std::move(directory);
    }

    RegionStore::~RegionStore() = default;

    std::optional<std::vector<uint8_t>> RegionStore::Load(const glm::ivec3& chunk) {
        std::scoped_lock lock(m_Impl->mutex);
        Impl::Region& region = m_Impl->Acquire(chunk >> kRegionShift);
        auto it = region.chunks.find(Impl::LocalIndex(chunk));
        if (it == region.chunks.end()) return std::nullopt;
        return it->second;
    }

    void RegionStore::Save(const glm::ivec3& chunk, std::vector<uint8_t> bytes) {
        std::scoped_lock lock(m_Impl->mutex);
        Impl::Region& region = m_Impl->Acquire(chunk >> kRegionShift);
        region.chunks[Impl::LocalIndex(chunk)] = std::move(bytes);
        region.dirty = true;
    }

    size_t RegionStore::Flush() {
        std::scoped_lock lock(m_Impl->mutex);
        if (m_Impl->directory.empty()) return 0;
        size_t written = 0;
        for (auto& [key, region] : m_Impl->regions) {
            if (!region.dirty) continue;
            const glm::ivec3 r = Impl::Unkey(key);
            if (m_Impl->WriteFile(r, region)) {
                region.dirty = false;
                ++written;
            } else {
                Log::Error("Failed to write region file {}", m_Impl->FilePath(r).string());
            }
        }
        return written;
    }

    size_t RegionStore::StoredChunkCount() const {
        std::scoped_lock lock(m_Impl->mutex);
        size_t n = 0;
        for (const auto& [key, region] : m_Impl->regions) n += region.chunks.size();
        return n;
    }
}
