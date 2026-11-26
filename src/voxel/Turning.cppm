module;

#include <array>
#include <cstdint>

export module vortex.voxel:turning;

import :material;

export namespace vortex::voxel {

    /// @brief A voxel's new id is `map[old]`; a map is the identity on every id it does not name
    /// (docs/design/magic.md "The Turning table").
    using TurningMap = std::array<uint8_t, 256>;

    /// @brief Map numbers. Wire values: never renumber.
    enum TurningMapId : uint8_t {
        Map_Ignite = 1,
        Map_Douse = 2,
        Map_Knit = 3,
        Map_Slag = 4,
        Map_Set = 5,
        Map_Pitch = 6,
        Map_Heat = 7,
        Map_Cool = 8,
        Map_Rime = 9,
        Map_Thaw = 10,
        Map_Freeze = 11,
        Map_FreezeWater = 12,
        Map_Shatter = 13,
        Map_Unknit = 14,
        Map_Ablate = 15,
    };
    /// Maps in a table; map 0 is the identity and is never sent.
    inline constexpr uint8_t kTurningMapCount = 16;

    struct TurningTable {
        std::array<TurningMap, kTurningMapCount> maps{};
    };

    /// @brief The table for the world palette of RegisterDefaultWorldMaterials (ids by name,
    /// integers authored in code). No map names bedrock.
    TurningTable BuildTurningTable(const WorldMaterials& ids);
    /// @brief The process-wide table of the default world palette (built once, thread-safe).
    const TurningTable& WorldTurningTable();
    /// @brief A hash of every entry of every map.
    uint64_t TurningTableHash(const TurningTable& table);
}
