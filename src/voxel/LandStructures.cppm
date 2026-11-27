module;

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

export module vortex.voxel:landstructures;

import :chunk;
import :material;
import :land;
import :fort;

// Small themed structures over the starter region (engine/docs/design/land.md "Wayside structures"): ruins of the
// Unmaking and outposts of setting.md 5.1 and 5.7, planned from the baked map and stamped inside chunk generation with
// the fort's records. Like the fort plan, the plan uses only +, -, *, /, sqrt and floor on floats and integer hashes, so
// every machine makes the same records.

export namespace vortex::voxel {

    /// @brief The structure types. The values name them in logs and tests only (nothing is sent).
    enum class LandStructureKind : uint8_t {
        Farmstead = 0,    ///< a burned farmstead: a stone ground floor, its upper storey left as charred post stumps
        BurnedRow = 1,    ///< a fragment of a burned timber quarter: 2-3 narrow plots side by side, facing a road
        TowerStump = 2,   ///< a felled stage tower: a hollow stone stump, its fallen stage lying in a rubble line
        ChimeTower = 3,   ///< a wayside log chime tower: 4 posts, braces, a platform, a pyramid roof and an iron bell
        Waystone = 4,     ///< a Ledgerwards waystone beside a road, sometimes under a small roofed shrine
        HillTower = 5,    ///< an open-backed hill-fort tower on a slope, with stubs of its curtain wall
        WallRun = 6,      ///< a breached border wall: a rubble-core run with a slumped breach and rubble mounds
        Well = 7,         ///< a roadside well: a stone curb, a shaft, posts, a crossbeam and a small roof
    };
    inline constexpr int kLandStructureKinds = 8;

    /// @brief The name of a kind in logs ("farmstead", "burned row", ...).
    const char* LandStructureName(LandStructureKind kind);

    /// @brief One placed structure: what it is, where, and its records (Box, Beam and Roof only).
    struct LandStructureSite {
        LandStructureKind kind = LandStructureKind::Farmstead;
        glm::vec2 at{0.0f};       ///< world metres of the footprint's middle
        int turn = 0;             ///< quarter turns (0..3) of the front, which faces +x at turn 0 and turns towards +z
        float ground = 0.0f;      ///< the floor level in metres (the footprint's lowest ground)
        float radius = 0.0f;      ///< metres from `at` that hold every record's footprint
        FortPlan plan;            ///< `records` in stamp order; the other fields stay unset
        glm::ivec3 min{0}, max{0};   ///< voxel bounds of every record, [min, max)
    };

    /// @brief Every structure of a map and a spatial index of their voxel bounds.
    struct LandStructures {
        std::vector<LandStructureSite> sites;
        int bucket = 256;                     ///< voxels per index cell along x and z
        glm::ivec2 cellMin{0}, cells{0};      ///< the index's first cell and size
        std::vector<std::vector<uint32_t>> index;   ///< site indexes per cell (x fastest)
    };

    /**
     * @brief Places the structures on the starter region's fine grid (land.md "Wayside structures"): spots beside the
     * roads first, then a 64 m grid with a few jittered tries per cell; each type by its rule (relief, distance to a
     * road), 45 m apart, clear of water, roads, Hollowkeep's site and its approach, and the village, landing, cave and
     * landmark. None without a fine grid.
     */
    LandStructures PlanLandStructures(const LandMap& map, const LandGeneratorSettings& land);

    /// @brief A hash of every site and record (the kinds, the places, every record field's bits).
    uint64_t LandStructuresHash(const LandStructures& structures);

    /// @brief The sites whose voxel bounds meet columns [x0, x1) x [z0, z1), in plan order.
    void LandStructuresIn(const LandStructures& structures, int x0, int z0, int x1, int z1, std::vector<uint32_t>& out);

    /// @brief The lowest and highest voxel rows the structures write in columns [x0, x1) x [z0, z1); false if none.
    bool LandStructureRows(const LandStructures& structures, int x0, int z0, int x1, int z1, int& lo, int& hi);

    /// @brief True when world metres (x, z) lie within `margin` of a structure's voxel bounds.
    bool NearLandStructure(const LandStructures& structures, float voxelSize, float x, float z, float margin);

    /// @brief Writes the structures' voxels into one chunk (after the ground and the plants: they replace them).
    void StampLandStructures(const LandStructures& structures, const WorldMaterials& materials, float voxelSize, const glm::ivec3& coord,
                             const float* ground, Chunk& chunk);
}
