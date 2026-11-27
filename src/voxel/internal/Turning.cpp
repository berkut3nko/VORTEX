module;

#include <cstdint>
#include <utility>

module vortex.voxel;

namespace vortex::voxel {

    TurningTable BuildTurningTable(const WorldMaterials& ids) {
        TurningTable table;
        for (TurningMap& map : table.maps) {
            for (uint32_t id = 0; id < map.size(); ++id) map[id] = static_cast<uint8_t>(id);
        }
        // Maps without an entry here stay the identity until the ids they name exist.
        table.maps[Map_Knit][0] = ids.knitIron;
        table.maps[Map_Ablate][ids.knitIron] = 0;
        table.maps[Map_Ablate][ids.slag] = 0;
        // Molten Pour: air into slag, and slag sets into knit.
        table.maps[Map_Slag][0] = ids.slag;
        table.maps[Map_Set][ids.slag] = ids.knitIron;
        // Every conjuring's expiry: knit (plain or rimed), slag and ice go, floe turns back into water.
        TurningMap& unknit = table.maps[Map_Unknit];
        unknit[ids.knitIron] = 0;
        unknit[ids.knitIronRimed] = 0;
        unknit[ids.ice] = 0;
        unknit[ids.slag] = 0;
        unknit[ids.floe] = ids.water;
        // Rime and its Thaw: each rimeable id and its rimed twin; Shatter breaks every rimed id.
        const std::pair<uint8_t, uint8_t> rimed[] = {{ids.stone, ids.stoneRimed},
                                                     {ids.stoneDark, ids.stoneDarkRimed},
                                                     {ids.brick, ids.brickRimed},
                                                     {ids.iron, ids.ironRimed},
                                                     {ids.knitIron, ids.knitIronRimed}};
        for (const auto& [base, frosted] : rimed) {
            table.maps[Map_Rime][base] = frosted;
            table.maps[Map_Thaw][frosted] = base;
            table.maps[Map_Shatter][frosted] = 0;
        }
        // Freeze: air into ice and water into floe; FreezeWater leaves the air alone.
        table.maps[Map_Freeze][0] = ids.ice;
        table.maps[Map_Freeze][ids.water] = ids.floe;
        table.maps[Map_FreezeWater][ids.water] = ids.floe;
        table.maps[Map_Shatter][ids.ice] = 0;
        table.maps[Map_Shatter][ids.floe] = ids.water;
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
