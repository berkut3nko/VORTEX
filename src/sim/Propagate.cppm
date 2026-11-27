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
import :blast;

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

    /// @brief An operator moves through an open cell: air, or a film of the world palette (pitch). kWallId is not open.
    bool IsOpenId(uint8_t id);

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
        uint16_t discRadius = 0;     ///< voxels: a disc footprint du^2 + dv^2 <= R^2 instead of the strip (0: the strip)
    };
    struct BurstResult {
        std::vector<glm::ivec3> cells;     ///< the strip or the disc, in pop order
        std::vector<int32_t> lateral;      ///< per cell: |du * Dv - dv * Du| / |Dp| (0..halfWidth)
        /// Per cell: 1 for Front, 0 for Path. A strip is all Front; a disc's Front is the leading half of its
        /// rim (du^2 + dv^2 > (R - 1)^2 and du * Du + dv * Dv >= 0; the whole rim when the aim is along the normal).
        std::vector<uint8_t> front;
        uint32_t visits = 0;               ///< cells tested by the step search
        uint32_t chunksRead = 0;
    };
    /**
     * @brief The strip (or the disc) of a Burst: open cells with support under them (`c - n`), reached
     * from the seed by in-plane steps that climb or drop at most `stepMax` along the normal, inside the
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

    // --- Plume, the full operator (magic.md "Plume (Flame): up, then highest-first under ceilings") ---

    struct PlumeParams {
        glm::ivec3 seed{0};          ///< s0 = h + n
        FaceAxes axes;
        uint8_t seedRadius = 0;      ///< rf: the seed disc in the hit face's plane
        uint16_t riseMax = 0;        ///< voxels above each seed (the spread phase: above s0)
        uint16_t runPops = 0;
        uint16_t lateral = 0;        ///< voxels from the hit column in x and z
        uint16_t visitCap = 0;
    };
    struct PlumeResult {
        std::vector<glm::ivec3> rise;      ///< the rise phase's cells below each column's top, seed order
        std::vector<glm::ivec3> run;       ///< spread-phase cells that are not Front
        /// Every column's top, then the under-ceiling cells of the spread phase, in heap order.
        std::vector<glm::ivec3> front;
        uint32_t visits = 0;
        uint32_t chunksRead = 0;
    };
    /**
     * @brief The Plume: each open seed of the disc rises while the cell above is open (at most riseMax);
     * columns stopped under a ceiling seed a min-heap on (-y, dx^2 + dz^2, z, x) from the hit column; a
     * popped cell rises if it can, else it spreads to its four sides (+x, -x, +z, -z) within `lateral`.
     * Never down. Its box is x / z +-(lateral + seedRadius + 1), y from seed - seedRadius - 4 to seed +
     * riseMax + seedRadius + 2.
     */
    PlumeResult PlumeOperator(const voxel::World& world, const OperatorWalls& walls, const PlumeParams& params);

    // --- Flow's arc (magic.md "Inputs every operator shares") ---------------------------------------------------

    /// Fine fixed point of the arc: 1/4096 voxel (q << 8), so that gravity per tick^2 does not round to 0.
    inline constexpr int32_t kArcFineShift = 8;

    struct ArcParams {
        glm::ivec3 startQ{0};          ///< the eye, q
        glm::i64vec3 velocity{0};      ///< 1/4096 voxel per tick at the launch
        int64_t gravity = 0;           ///< 1/4096 voxel per tick^2
        uint32_t maxTicks = 150;
    };
    struct ArcHit {
        bool hit = false;              ///< met a voxel that is not open within maxTicks
        bool wall = false;             ///< the voxel it met is a wall of the operators (outside the walls, not loaded)
        glm::ivec3 voxel{0};           ///< the voxel met
        glm::ivec3 normal{0, 1, 0};    ///< the face it entered through (the DDA's last step, pointing back)
        glm::ivec3 hitQ{0};            ///< the point on the arc where it entered that voxel, q
        uint32_t ticks = 0;            ///< the tick of the segment that met it (1 = the first)
        int32_t apexQ = 0;             ///< the highest point of the arc's positions, q
        uint32_t visits = 0;           ///< voxels the walk entered
        uint32_t chunksRead = 0;
    };
    /**
     * @brief The integer arc: P from the start (q << 8), V the launch velocity; the first step takes half gravity
     * (`V.y -= G / 2` once), then each tick `P += V; V.y -= G`. The segment of each tick, from P >> 8 to the next
     * P >> 8 (q), is walked voxel by voxel (an integer DDA: the axis whose boundary comes first, ties x before y
     * before z) until a voxel that is not open; at most maxTicks ticks. Integers only.
     */
    ArcHit TraceArc(const voxel::World& world, const OperatorWalls& walls, const ArcParams& params);
    /// @brief A tap's launch: `round(D * speedFine / 16384)` per axis (D = round(direction * 16384)).
    glm::i64vec3 ArcLaunch(const glm::ivec3& aim, int64_t speedFine);
    /**
     * @brief A lob from `fromQ` that reaches `toQ` after `ticks`: the horizontal distance over the ticks
     * (`ceil(distance / horizontalFine)`, within [minTicks, maxTicks]), and the vertical speed with which the arc's
     * height after those ticks is `toQ.y` (P_T.y = P_0.y + T V.y - G T^2 / 2), rounded half away from zero.
     */
    glm::i64vec3 LobLaunch(const glm::ivec3& fromQ, const glm::ivec3& toQ, int64_t horizontalFine, int64_t gravity, uint32_t minTicks,
                           uint32_t maxTicks, uint32_t& ticks);

    // --- Flow (magic.md "Flow (Liquid): down, then lowest-first") ----------------------------------

    struct FlowParams {
        glm::ivec3 seed{0};          ///< s0 = h + n
        FaceAxes axes;
        uint8_t seedRadius = 0;      ///< rf: the seed disc in the hit face's plane (0: the seed alone)
        uint16_t fallMax = 0;        ///< cells a column falls at most
        uint16_t runPops = 0;
        uint16_t lateral = 0;        ///< voxels from the hit column in x and z
        uint16_t frontCount = 0;
        uint32_t visitCap = 0;
    };
    struct FlowResult {
        std::vector<glm::ivec3> fall;      ///< the fall phase's cells above each landing, seed order
        std::vector<glm::ivec3> run;       ///< spread-phase cells that are not Front, in key order
        std::vector<glm::ivec3> front;     ///< the frontCount popped spread-phase cells with the smallest key, in key order
        std::vector<glm::ivec3> landings;  ///< where each falling column stopped (support below), seed order
        uint32_t visits = 0;
        uint32_t chunksRead = 0;
    };
    /// @brief The key of a Flow cell: (y, dx^2 + dz^2 from the hit column, z, x).
    using FlowKey = std::tuple<int32_t, int64_t, int32_t, int32_t>;
    FlowKey FlowKeyOf(const glm::ivec3& cell, const glm::ivec3& seed);
    /**
     * @brief The Flow: each open seed of the disc, in (a, b) order, falls while the cell below is open (at most
     * fallMax cells; a column that reaches a cell another column passed stops there); the cell where it stops is a
     * landing. Landings seed a min-heap on FlowKeyOf; a popped cell whose cell below is open and not visited pushes
     * only that cell (it falls), any other pushes its open, unvisited sides (+x, -x, +z, -z) within `lateral` of the
     * hit column. Never up. At most runPops pops and visitCap visits. Its box is x / z +-(lateral + seedRadius + 1),
     * y from seed - fallMax - seedRadius - 1 to seed + seedRadius + 1.
     */
    FlowResult FlowOperator(const voxel::World& world, const OperatorWalls& walls, const FlowParams& params);

    // --- Pierce (magic.md "Pierce (Solid): a bore that pays for every voxel it adds") ------------

    /// A bore reaches at most this many voxels from the hit, with a radius of at most kMaxPierceRadiusVoxels.
    inline constexpr int32_t kMaxBoreVoxels = 64;
    inline constexpr int32_t kMaxPierceRadiusVoxels = 4;
    /// Structure past a bore's end is seen for at least this many voxels before the flood box's face.
    inline constexpr int32_t kBoreProofMargin = 24;
    static_assert(kMaxBoreVoxels / 2 + 3 * kMaxPierceRadiusVoxels + kIslandSearchMargin + kBoreProofMargin <= voxel::kFloodHalfExtent);
    inline constexpr uint32_t kPierceVisitCap = 3000;   ///< new member voxels, sum |N_k|
    inline constexpr uint64_t kPierceSliceWork = 100;   ///< work per slice scanned

    struct PierceParams {
        glm::ivec3 hitQ{0};            ///< the ray's hit point, q
        glm::ivec3 aim{0};             ///< D = round(direction * 16384)
        uint16_t radiusQ = 0;          ///< at most 16 * kMaxPierceRadiusVoxels
        uint16_t power = 0;            ///< 1/16 hardness
        uint32_t budget = 0;
        uint16_t maxSlices = 0;
        const std::array<uint16_t, 256>* boreCost = nullptr;
        const voxel::MaterialLibrary* materials = nullptr;
        std::vector<glm::ivec3>* paidOut = nullptr;   ///< tests: the paid solid voxels in walk order
        /// The element's pre-map: each voxel is read through it before its cost and power are judged (Meltdown Beam
        /// reads metal through Heat, which its own op applies before the carve). Null: the ids as they are.
        const voxel::TurningMap* preMap = nullptr;
    };
    enum class PierceStop : uint8_t { Slices = 0, TooHard = 1, Budget = 2, Length = 3, Wall = 4, VisitCap = 5 };
    struct PierceResult {
        bool bored = false;            ///< false: slice 0 already stopped (nothing to carve)
        voxel::ShapeQ capsule;         ///< kind capsule: a = A, b = B_last, radiusQ
        glm::ivec3 step{0};            ///< S, q per slice (about 16 long)
        uint32_t slices = 0;           ///< k of B_last
        uint32_t visits = 0;           ///< sum |N_k| of the paid slices
        uint32_t scanned = 0;          ///< slices whose box was scanned (the work)
        uint32_t paidVoxels = 0;       ///< solid voxels paid (Path)
        uint32_t frontVoxels = 0;      ///< the stub: |N_k| of the slices 0..2r/16
        uint32_t budgetLeft = 0;
        uint32_t chunksRead = 0;
        PierceStop stop = PierceStop::Slices;
    };
    /**
     * @brief The bore: `S = round(16 D / |D|)`, `A = hitQ - (2 r S) / 16`, `B_k = A + k S`. Slice k adds
     * the voxels of Capsule(A, B_k, r) not in Capsule(A, B_{k-1}, r) (slice 0: the sphere at A), read in
     * (z, y, x) order over the box of Capsule(B_{k-1}, B_k, r). The walk stops before a slice that holds a
     * wall voxel, an indestructible voxel that is not a liquid or one too hard for `power`, costs more
     * than the budget left (air and liquids cost 0), passes maxSlices, ends further than kMaxBoreVoxels
     * from the hit or would pass kPierceVisitCap. CarveCapsule(A, B_last, r, power) then removes exactly
     * the paid solid voxels.
     */
    PierceResult Pierce(const voxel::World& world, const OperatorWalls& walls, const PierceParams& params);

    // --- the Shatter flood (magic.md "The element verbs") -----------------------------------------

    struct ShatterResult {
        std::vector<glm::ivec3> cells;     ///< in pop order: (depth, z, y, x)
        uint32_t visits = 0;
        uint32_t chunksRead = 0;
    };
    /**
     * @brief The shatterable voxels (those voxel::Map_Shatter changes: rimed ids, ice, floe) inside
     * `sphere` reached from the exposed ones (an air face neighbour) through face-connected
     * shatterable voxels inside the sphere, at most `maxDepth` voxels deep (the exposed ones are depth
     * 0), at most `visitCap` visits (cells popped and neighbours tested).
     */
    ShatterResult ShatterFlood(const voxel::World& world, const OperatorWalls& walls, const voxel::SphereQ& sphere, uint8_t maxDepth,
                               uint32_t visitCap);
}
