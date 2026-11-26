module;

#include <cstdint>

module vortex.voxel;

namespace vortex::voxel {

    TurningTable BuildTurningTable(const WorldMaterials& ids) {
        TurningTable table;
        for (TurningMap& map : table.maps) {
            for (uint32_t id = 0; id < map.size(); ++id) map[id] = static_cast<uint8_t>(id);
        }
        // Maps without an entry here stay the identity until the ids they name exist.
        table.maps[Map_Knit][0] = ids.knitIron;
        table.maps[Map_Unknit][ids.knitIron] = 0;
        table.maps[Map_Ablate][ids.knitIron] = 0;
        return table;
    }

    const TurningTable& WorldTurningTable() {
        static const TurningTable table = [] {
            MaterialLibrary scratch;
            return BuildTurningTable(RegisterDefaultWorldMaterials(scratch));
        }();
        return table;
    }

    uint64_t TurningTableHash(const TurningTable& table) {
        uint64_t h = HashMix64(0x5455524eull, kTurningMapCount);   // "TURN"
        for (const TurningMap& map : table.maps) {
            for (const uint8_t id : map) h = HashMix64(h, id);
        }
        return h;
    }
}
