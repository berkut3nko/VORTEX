module;

#include <glm/glm.hpp>

#include <cstdint>

module vortex.voxel;

namespace vortex::voxel {

    namespace {
        constexpr uint64_t kSeed = 0x564f52544558ull;   // "VORTEX"

        /// splitmix64 finaliser: a bijection on 64 bits with full avalanche.
        constexpr uint64_t Mix(uint64_t x) {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ull;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebull;
            x ^= x >> 31;
            return x;
        }

        /// Absorbs one maximal run: `length` (1..32768) voxels of material `id`.
        constexpr uint64_t AddRun(uint64_t h, uint8_t id, uint32_t length) {
            return Mix(h + 0x9e3779b97f4a7c15ull + (static_cast<uint64_t>(length) << 8 | id));
        }

        constexpr uint64_t Finish(uint64_t h) { return Mix(h ^ 0xd6e8feb86659fd93ull); }

        constexpr uint64_t UniformHash(uint8_t id) { return Finish(AddRun(kSeed, id, static_cast<uint32_t>(kChunkVolume))); }
    }

    static_assert(UniformHash(0) == kAirChunkHash, "kAirChunkHash must be the hash of an all-air chunk");

    uint64_t UniformChunkHash(uint8_t id) { return UniformHash(id); }

    uint64_t ChunkHash(const Chunk& chunk) {
        uint64_t h = kSeed;
        uint8_t id = chunk.ids[0];
        uint32_t length = 1;
        for (int i = 1; i < kChunkVolume; ++i) {
            const uint8_t next = chunk.ids[i];
            if (next == id) {
                ++length;
                continue;
            }
            h = AddRun(h, id, length);
            id = next;
            length = 1;
        }
        return Finish(AddRun(h, id, length));
    }

    uint64_t ChunkHash(ChunkKind kind, uint8_t uniformId, const Chunk* chunk) {
        switch (kind) {
            case ChunkKind::Uniform: return UniformHash(uniformId);
            case ChunkKind::Dense: return chunk ? ChunkHash(*chunk) : kAirChunkHash;
            case ChunkKind::Empty: break;
        }
        return kAirChunkHash;
    }

    uint64_t WorldHashTerm(const glm::ivec3& coord, uint64_t chunkHash) {
        if (chunkHash == kAirChunkHash) return 0;
        // Coordinates enter as their 32-bit two's complement value, one axis at a time.
        uint64_t h = Mix(kSeed + static_cast<uint32_t>(coord.x));
        h = Mix(h + static_cast<uint32_t>(coord.y));
        h = Mix(h + static_cast<uint32_t>(coord.z));
        return Mix(h ^ chunkHash);
    }

    WorldHashResult WorldHash(const World& world, const glm::ivec3& minChunk, const glm::ivec3& maxChunk) {
        WorldHashResult result;
        for (int z = minChunk.z; z < maxChunk.z; ++z)
            for (int y = minChunk.y; y < maxChunk.y; ++y)
                for (int x = minChunk.x; x < maxChunk.x; ++x) {
                    const glm::ivec3 coord(x, y, z);
                    uint64_t hash = kAirChunkHash;
                    switch (world.StateOf(coord)) {
                        case CellState::Unloaded:
                        case CellState::Loading: ++result.notLoaded; continue;
                        case CellState::Empty: break;
                        case CellState::Uniform: {
                            uint8_t id = 0;
                            world.GetChunk(coord, &id);
                            hash = UniformHash(id);
                            break;
                        }
                        case CellState::Dense:
                            if (const Chunk* chunk = world.GetChunk(coord)) hash = ChunkHash(*chunk);
                            break;
                    }
                    if (hash == kAirChunkHash) continue;
                    result.hash += WorldHashTerm(coord, hash);
                    ++result.solidChunks;
                }
        return result;
    }
}
