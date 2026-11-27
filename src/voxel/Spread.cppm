module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

export module vortex.voxel:spread;

import :material;
import :chunk;

export namespace vortex::voxel {

    // --- fire spread (docs/design/spread.md "Cell model and data", "The rules") ---------------------
    //  The fire state lives only in voxel ids: a burning id names its class and stage. One step of a
    //  chunk decides every voxel from the old ids of the chunk and its padded neighbourhood, the step
    //  index `s` and the world coordinates only. Integers only; no randomness.

    enum class SpreadKind : uint8_t { None = 0, Flammable = 1, Burning = 2, GroundCover = 3, WetSource = 4 };
    /// Fire classes. Pitch is laid burning (no kindle stages) and never catches from a neighbour.
    enum SpreadClass : uint8_t { Class_None = 0, Class_Planks = 1, Class_Wood = 2, Class_Foliage = 3, Class_Pitch = 4 };
    inline constexpr uint32_t kSpreadClassCount = 5;

    inline constexpr int kSpreadPad = 4;
    inline constexpr int kSpreadBoxEdge = kChunkSize + 2 * kSpreadPad;                     ///< 40
    inline constexpr uint32_t kSpreadBoxVolume = kSpreadBoxEdge * kSpreadBoxEdge * kSpreadBoxEdge;   ///< 64 000 B

    struct SpreadClassDef {
        uint8_t K = 0, B = 0, pace = 1;                                ///< kindle stages, burn stages, burn pace (steps)
        /// Ignition delays of a target of this class: the source's stage must reach them (0 = never).
        uint8_t dBelow = 0, dReach = 0, dSlope = 0, dSide = 0, dDiag = 0, dJump = 0;
        uint8_t baseId = 0, firstKindle = 0, firstBurn = 0;            ///< ids
        uint8_t residue = 0, quenchEarly = 0, quenchLate = 0;          ///< ids
    };

    struct SpreadTable {
        std::array<SpreadKind, 256> kind{};
        std::array<uint8_t, 256> cls{};          ///< SpreadClass of a flammable or burning id
        std::array<uint8_t, 256> stage{};        ///< burning: k_i -> i, b_j -> K + j; else 0
        std::array<uint8_t, 256> next{};         ///< burning: the id after advancing (the residue after bB); else the id
        std::array<SpreadClassDef, kSpreadClassCount> classes{};
        /// The burn phase pattern [a][y & 3][c] with a = min(x & 3, 3 - (x & 3)), c likewise for z: 16 distinct 0..15.
        std::array<std::array<std::array<uint8_t, 2>, 4>, 2> kPhase{};
        uint8_t bedrock = 0, air = 0, charred = 0;
    };

    SpreadTable BuildSpreadTable(const WorldMaterials& ids);
    /// @brief The table of the default world palette (built once from a scratch library, thread-safe).
    const SpreadTable& WorldSpreadTable();
    /// @brief Every field in a fixed order (logged; a session's build hash takes it when fire runs in sessions).
    uint64_t SpreadTableHash(const SpreadTable& table);
    /// @brief The burn phase of world voxel `v` (two's complement & 3 on each axis).
    uint8_t SpreadPhase(const SpreadTable& table, const glm::ivec3& v);

    /// @brief A chunk and its pad of 4 voxels: ids[x + 40 * (y + 40 * z)], chunk-local voxel (0, 0, 0) at (4, 4, 4).
    struct SpreadBox {
        std::array<uint8_t, kSpreadBoxVolume> ids;
        static constexpr uint32_t Index(int x, int y, int z) {
            return static_cast<uint32_t>((x + kSpreadPad) + kSpreadBoxEdge * ((y + kSpreadPad) + kSpreadBoxEdge * (z + kSpreadPad)));
        }
    };

    struct SpreadCounts {
        uint32_t evaluated = 0, ignited = 0, advanced = 0, burnedOut = 0, quenched = 0;
        int lowestIgnited = kChunkSize;   ///< the lowest chunk-local y a voxel caught at (kChunkSize: none)
        bool shapeChanged = false;   ///< some voxel changed occupancy (air <-> solid)
        bool site = false;           ///< a structural voxel became air, or a voxel entered its last burn stage or charred wood
    };

    /**
     * @brief One synchronous fire step of `chunk` from `in` (old ids only).
     * @param out kChunkVolume ids in Chunk::ids order; the caller fills it with the chunk's old ids. Only the voxels of
     * the blocks set in `evalBlocks` are decided; a voxel's new id is written only where it differs.
     * @param s The step index: a burn stage advances when (s + phase) % pace == 0.
     */
    void StepBox(const SpreadTable& table, const SpreadBox& in, const glm::ivec3& chunk, uint32_t s, const BlockMask& evalBlocks,
                 uint8_t* out, SpreadCounts& counts);
    /// @brief The blocks of `chunk` that hold a burning id (into `out`); returns the burning voxel count.
    uint32_t LiveBlocks(const SpreadTable& table, const Chunk& chunk, BlockMask& out);
    /// @brief `mask` grown by one block inside the chunk (26-neighbourhood).
    BlockMask DilateBlocks(const BlockMask& mask);
}
