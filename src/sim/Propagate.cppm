module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

export module vortex.sim:propagate;

import vortex.voxel;
import :worldop;

export namespace vortex::sim {

    // --- the operators' world (docs/design/magic.md "Rules shared by every operator") ------------
    //  The operators read voxel ids only, through a per-cast chunk cache. Their results follow
    //  from the voxels and the cast's integers: never from the cache, the thread count or the
    //  load order. Every queue is keyed by integers that end in (z, y, x).

    /// @brief The operator's walls: the voxel box [min, max). Outside it every cell reads as support.
    struct OperatorWalls {
        glm::ivec3 min{0}, max{0};
    };
    /// @brief The offline walls: the world window's voxel box (a full-height window's top row ends at
    /// the build ceiling).
    OperatorWalls WindowWalls(const voxel::World& world);

    /// @brief Voxel ids for one cast: each chunk is read (a packed one decoded) once.
    class VoxelCache {
    public:
        static constexpr uint8_t kWallId = 0xFF;   ///< never a real id of the world palette here

        VoxelCache(const voxel::World& world, const OperatorWalls& walls);
        /// @brief The id; a voxel outside the walls or in a chunk that is not loaded reads kWallId.
        uint8_t Get(const glm::ivec3& v);
        bool IsWall(const glm::ivec3& v) { return Get(v) == kWallId; }
        /// @brief Distinct chunks Get read (loaded or not): a function of the cast, not of the cache.
        uint32_t ChunksRead() const { return static_cast<uint32_t>(m_Chunks.size()); }

    private:
        const voxel::World& m_World;
        OperatorWalls m_Walls;
        std::map<std::tuple<int, int, int>, std::unique_ptr<voxel::Chunk>> m_Chunks;   ///< nullptr: not loaded
    };

    /// @brief An operator moves through an open cell: air (films are open too, once they exist).
    /// kWallId is not open.
    constexpr bool IsOpenId(uint8_t id) { return id == 0; }

    /// @brief The hit face: `n` = the normal (one axis, with its sign); `u`, `v` = unit vectors of
    /// the other two axes, the lower axis first.
    struct FaceAxes {
        glm::ivec3 n{0, 1, 0}, u{1, 0, 0}, v{0, 0, 1};
    };
    /// @brief The axes of the face whose normal is `normal` (exactly one component +-1).
    FaceAxes FaceAxesOf(const glm::ivec3& normal);

    // --- Burst (magic.md "Burst (Torrent): a sweep in the hit face's plane") ------------------------

    struct BurstParams {
        glm::ivec3 seed{0};          ///< s0 = h + n, the open cell in front of the hit face
        FaceAxes axes;
        glm::ivec3 aim{0};           ///< D = round(direction * 16384)
        uint16_t halfWidth = 0;      ///< the strip across the aim, voxels
        uint16_t halfThickness = 0;  ///< the strip along the aim, voxels
        uint8_t stepMax = 0;         ///< voxels along the normal per step
        uint16_t pops = 0;
        uint16_t visitCap = 0;
        uint16_t plane = 0;          ///< the box: |du|, |dv| <= plane
        int16_t normalMin = 0;       ///< the box: normalMin <= dn <= normalMax
        int16_t normalMax = 0;
    };
    struct BurstResult {
        std::vector<glm::ivec3> cells;     ///< the strip, in pop order (all Front)
        std::vector<int32_t> lateral;      ///< per cell: |du * Dv - dv * Du| / |Dp| (0..halfWidth)
        uint32_t visits = 0;               ///< cells tested by the step search
        uint32_t chunksRead = 0;
    };
    /**
     * @brief The strip of a Burst: open cells with support under them (`c - n`), reached from the
     * seed by in-plane steps that climb or drop at most `stepMax` along the normal, inside the strip
     * footprint and the box. A min-heap on (du^2 + dv^2, du, dv, dn, z, y, x) decides the order.
     * Empty when the seed is not such a cell.
     */
    BurstResult BurstStrip(const voxel::World& world, const OperatorWalls& walls, const BurstParams& params);

    // --- Plume: the single column (magic.md "Plume (Flame): up, then highest-first under ceilings") ---

    /// @brief Inclusive cells [bottom, top] of one column.
    struct PlumeColumn {
        glm::ivec3 bottom{0}, top{0};
        uint32_t visits = 0;
        uint32_t chunksRead = 0;
    };
    /// @brief From `seed` up while the cell above is open, at most `riseMax` cells above the seed.
    /// A seed that is not open gives bottom = top = seed and no rise.
    PlumeColumn PlumeSingleColumn(const voxel::World& world, const OperatorWalls& walls, const glm::ivec3& seed, uint16_t riseMax);

    // --- Barricade Wave's fill ----------------------------------------------------------------

    /// @brief Cells of a fill by band (band 0 is the middle of the strip).
    struct FillBands {
        std::array<std::vector<glm::ivec3>, 6> bands;
        uint32_t cells = 0;
    };
    /**
     * @brief Barricade Wave's fill: each strip cell, in pop order, and above it along +n up to
     * `rimHeight - 1` more cells while they are open. A cell goes into band
     * `min(bandCount - 1, lateral * bandCount / (halfWidth + 1))` of its strip cell, once.
     */
    FillBands BarricadeFill(const voxel::World& world, const OperatorWalls& walls, const BurstResult& strip, const FaceAxes& axes,
                            uint8_t rimHeight, uint8_t bandCount, uint16_t halfWidth);

    /// @brief One MapMask of `map` over `cells` (its mask's box is the cells' bounding box). An
    /// empty list gives a MapMask with an empty mask.
    WorldOp MapCellsOp(uint8_t map, std::span<const glm::ivec3> cells);
}
