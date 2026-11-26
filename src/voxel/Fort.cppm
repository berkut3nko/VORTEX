module;

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

export module vortex.voxel:fort;

import :chunk;
import :material;
import :land;

// Hollowkeep as stamped generator output (engine/docs/design/fortress.md, [fort-1]): a plan of records made from the
// baked land map's site, and a stamper that writes the records' voxels inside chunk generation. Like the bake, the
// plan uses only +, -, *, /, sqrt, floor and ceil on floats, so every machine makes the same records.

export namespace vortex::voxel {

    /// @brief What a fort record draws (fortress.md "Architectural grammar and module rules").
    enum class FortShape : uint8_t {
        Box = 0,      ///< a box of one material (0: air) in world metres
        Wall = 1,     ///< a wall run: foundation, batter, faces and core, buttresses, string courses, parapet, merlons
        Block = 2,    ///< a tower, a gate tower or the keep: batter, shell, floors, a flat top, parapet, merlons
        Trench = 3,   ///< the ditch: air down to a level floor with 1:1 sides, in the curtain's sectors at the gate
    };

    /// @brief The kinds of Block records.
    enum class FortBlock : uint8_t {
        Tower = 0,       ///< a D-shaped flank tower: square at the back, half an octagon outward
        Keep = 1,        ///< the keep: square, a granite lower shell, corner turrets, the sanctum's keel floor
        GateTower = 2,   ///< a round-fronted gate tower: as a flank tower, facing along the gate's axis
    };

    /// @brief One record of the plan. All lengths are world metres; `min` / `max` bound the voxels it writes.
    struct FortRecord {
        FortShape shape = FortShape::Box;
        FortBlock block = FortBlock::Tower;
        uint8_t id = 0;              ///< Box: the material (0 is air)
        glm::vec2 a{0.0f};           ///< Box: min (x, z). Wall: the run's start. Block: the centre. Trench: unused
        glm::vec2 b{0.0f};           ///< Box: max (x, z). Wall: the run's end. Block: the outward unit vector
        float lo = 0.0f;             ///< Box: the bottom. Block: the floor level. Trench: the floor
        float hi = 0.0f;             ///< Box: the top. Wall: the wall walk. Block: the top of the shell
        float half = 0.0f;           ///< Wall: half the thickness. Block: half the size
        float gapLo = 0.0f;          ///< Wall: no wall where the distance along the run lies in [gapLo, gapHi]
        float gapHi = -1.0f;
        glm::ivec3 min{0};           ///< voxel bounds of what the record writes, [min, max)
        glm::ivec3 max{0};
    };

    /// @brief The castle's plan: its records in stamp order and what the template needs (the gate, the keep).
    struct FortPlan {
        bool placed = false;
        std::vector<FortRecord> records;
        LandSite site;                       ///< the bake's site (map metres)
        glm::vec2 origin{0.0f};              ///< the land generator's origin (world metres of the map's corner)
        glm::vec2 keep{0.0f};                ///< world metres
        glm::vec2 gatePoint{0.0f};           ///< the gate's point on the curtain, world metres
        glm::vec2 axis{0.0f}, across{0.0f};  ///< the gate's axis (outward) and the direction across it
        glm::vec2 innerGate{0.0f};
        float keepGround = 0.0f, gateGround = 0.0f, bridgeSpan = 0.0f;
    };

    /// @brief Plans the castle on the map's site (none when the bake placed no site).
    FortPlan PlanFort(const LandMap& map, const LandGeneratorSettings& land);

    /// @brief A hash of the plan's records (every field's bits, in order).
    uint64_t FortPlanHash(const FortPlan& plan);

    /// @brief The lowest and highest voxel rows the plan writes in voxel columns [x0, x1) x [z0, z1); false if none.
    bool FortRows(const FortPlan& plan, int x0, int z0, int x1, int z1, int& lo, int& hi);

    /// @brief Writes the plan's voxels into one chunk. `ground` holds the 32 x 32 columns' ground in voxels (x fastest),
    /// the same heights the land generator filled the chunk with.
    void StampFort(const FortPlan& plan, const WorldMaterials& materials, float voxelSize, const glm::ivec3& coord, const float* ground,
                   Chunk& chunk);

}
