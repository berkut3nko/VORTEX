module;

#include <glm/glm.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

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
            // Eight voxels at a time while the run goes on (the same runs, so the same hash: a run of
            // air or rock is most of a chunk; stage 7b's HashChecks hash up to 24 chunks per check).
            uint64_t word = 0;
            const uint64_t same = 0x0101010101010101ull * id;
            while (i + 8 <= kChunkVolume && (std::memcpy(&word, &chunk.ids[i], 8), word == same)) {
                length += 8;
                i += 8;
            }
            if (i >= kChunkVolume) break;
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

    namespace {
        /// ChunkHash of the chunk an EncodeChunk(Dense) byte string decodes to, without decoding:
        /// adjacent runs of one id are merged, so any run split gives the maximal-run hash.
        uint64_t EncodedChunkHash(std::span<const uint8_t> bytes) {
            uint64_t h = kSeed;
            bool open = false;
            uint8_t id = 0;
            uint32_t length = 0;
            for (size_t p = 1; p + 2 < bytes.size(); p += 3) {
                const uint8_t next = bytes[p];
                const uint32_t run = static_cast<uint32_t>(bytes[p + 1] | (bytes[p + 2] << 8));
                if (open && next == id) {
                    length += run;
                    continue;
                }
                if (open) h = AddRun(h, id, length);
                open = true;
                id = next;
                length = run;
            }
            return open ? Finish(AddRun(h, id, length)) : kAirChunkHash;
        }
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

    bool ChunkHashAt(const World& world, const glm::ivec3& coord, uint64_t& out) {
        switch (world.StateOf(coord)) {
            case CellState::Unloaded:
            case CellState::Loading: return false;
            case CellState::Empty: out = kAirChunkHash; return true;
            case CellState::Uniform: {
                uint8_t id = 0;
                world.GetChunk(coord, &id);
                out = UniformHash(id);
                return true;
            }
            case CellState::Dense: {
                const Chunk* chunk = world.GetChunk(coord);
                out = chunk ? ChunkHash(*chunk) : kAirChunkHash;
                return true;
            }
            case CellState::Packed:
                // Hashed from its runs: packing is not a content change.
                out = EncodedChunkHash(world.PackedData(coord));
                return true;
        }
        return false;
    }

    WorldHashResult WorldHash(const World& world, const glm::ivec3& minChunk, const glm::ivec3& maxChunk) {
        WorldHashResult result;
        for (int z = minChunk.z; z < maxChunk.z; ++z)
            for (int y = minChunk.y; y < maxChunk.y; ++y)
                for (int x = minChunk.x; x < maxChunk.x; ++x) {
                    const glm::ivec3 coord(x, y, z);
                    uint64_t hash = kAirChunkHash;
                    if (!ChunkHashAt(world, coord, hash)) {
                        ++result.notLoaded;
                        continue;
                    }
                    if (hash == kAirChunkHash) continue;
                    result.hash += WorldHashTerm(coord, hash);
                    ++result.solidChunks;
                }
        return result;
    }

    uint64_t HashMix64(uint64_t h, uint64_t value) { return Mix(h + 0x9e3779b97f4a7c15ull + value); }

    uint64_t MaterialTableHash(const MaterialLibrary& materials) {
        uint64_t h = Mix(kSeed ^ 0x4d4154ull);   // "MAT"
        for (uint32_t id = 1; id < kPaletteSize; ++id) {
            const Material& m = materials.Get(kWorldPalette, static_cast<uint8_t>(id));
            h = HashMix64(h, std::bit_cast<uint32_t>(m.hardness));
            h = HashMix64(h, m.flags);
            h = HashMix64(h, std::bit_cast<uint32_t>(m.density));
            for (const uint32_t s : m.strength) h = HashMix64(h, s);
        }
        return Finish(h);
    }
}
