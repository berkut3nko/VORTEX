module;

#include <glm/glm.hpp>

#include <cstdint>

export module vortex.voxel:hash;

import :chunk;
import :generator;
import :material;
import :world;

export namespace vortex::voxel {

    /**
     * @brief 64-bit hash of the *content* of a chunk, independent of how it is stored.
     * @details The hash is defined over the 32768 material ids in `Chunk::VoxelIndex` order, taken
     * as maximal runs of equal ids (the same canonical form the region codec uses). Only `ids` is
     * read, one byte at a time: the occupancy pyramid, slot numbers, pointers, struct padding and
     * host byte order have no influence. An `Empty` cell, a `Uniform` cell and a `Dense` chunk
     * whose voxels all carry the same id therefore hash alike.
     *
     * Two machines agree on a chunk exactly when they agree on this value (up to 64-bit
     * collisions); it is what server and clients compare to prove terrain agreement. The function
     * is part of the network contract: changing it changes every checked-in golden value.
     */
    uint64_t ChunkHash(const Chunk& chunk);
    /// @brief Hash of a chunk in which every voxel has material `id` (0 = air). O(1).
    uint64_t UniformChunkHash(uint8_t id);
    /// @brief Hash of a chunk given as generator / codec output. `chunk` is read only for Dense
    /// (a null Dense chunk counts as air); `uniformId` only for Uniform.
    uint64_t ChunkHash(ChunkKind kind, uint8_t uniformId, const Chunk* chunk);

    /// @brief ChunkHash of a chunk that holds nothing but air.
    inline constexpr uint64_t kAirChunkHash = 0x304bb2631647de62ull;

    /**
     * @brief Contribution of one chunk to a WorldHash: mix(coord, chunkHash).
     * @details Air chunks contribute 0, so a world hash does not depend on how much empty sky the
     * hashed box contains. Contributions are combined by wrapping 64-bit addition, which makes the
     * result independent of the order the chunks are visited in.
     */
    uint64_t WorldHashTerm(const glm::ivec3& coord, uint64_t chunkHash);

    struct WorldHashResult {
        uint64_t hash = 0;        ///< Sum of WorldHashTerm over the non-air chunks of the box.
        uint32_t solidChunks = 0; ///< Chunks that contributed.
        uint32_t notLoaded = 0;   ///< Chunks of the box that were Unloaded / Loading (counted as air).
        bool Complete() const { return notLoaded == 0; }
    };

    /**
     * @brief Hash of the chunk-coordinate box [minChunk, maxChunk) of a world. Main thread only.
     * @details Chunks that are not loaded contribute nothing and are counted in `notLoaded`; two
     * hashes are only comparable when both are Complete().
     */
    WorldHashResult WorldHash(const World& world, const glm::ivec3& minChunk, const glm::ivec3& maxChunk);

    /// @brief Absorbs `value` into the running hash `h` (splitmix64 finaliser; not commutative):
    /// for hashes that must be equal on every machine, such as `Hello::buildHash`.
    uint64_t HashMix64(uint64_t h, uint64_t value);

    /**
     * @brief Hash of every material property of the world palette that a replicated decision
     * reads (docs/design/shred.md R6): per id 1..255 `hardness` (raw float bits: CarveSphereQ
     * compares `hardness * 16` with the power), `flags` (indestructible, liquid, foliage), `density`
     * (raw bits: the graph's mass) and `strength[3]` (the load check). Equal hashes mean the
     * carve, the island search and the load check decide alike. Part of `Hello::buildHash`.
     */
    uint64_t MaterialTableHash(const MaterialLibrary& materials);

    /**
     * @brief Names the generator this build was made with: a hash of the checked-in golden values
     * of `Terrain.GoldenChunkHashes`, `Terrain.GoldenNoiseBits` and `Terrain.GoldenHeightBits`
     * (`Terrain.GeneratorGoldenHashNamesTheGoldenTables` recomputes it from the tables). It changes
     * exactly when the goldens are regenerated, i.e. when the generator changed on purpose; part of
     * `Hello::buildHash`. A build that rounds differently has the same value (the golden tests
     * fail on it; at run time the probe hashes catch it).
     */
    inline constexpr uint64_t kGeneratorGoldenHash = 0x79cd3f1a21fc15c0ull;
}
