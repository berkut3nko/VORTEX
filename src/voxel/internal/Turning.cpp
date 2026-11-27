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
        // Thermal: Pitch lays a burning film on air; Heat makes metal hot (half its hardness), melts ice and floe and thaws
        // rime; Cool and Rime's quench make hot metal plain again; Freeze turns pitch into ice. A fill takes a film's cell,
        // and an expiry takes hot knit and pitch away.
        table.maps[Map_Pitch][0] = ids.pitch;
        TurningMap& heat = table.maps[Map_Heat];
        heat[ids.knitIron] = ids.knitIronHot;
        heat[ids.knitIronRimed] = ids.knitIronHot;
        heat[ids.iron] = ids.ironHot;
        heat[ids.ironRimed] = ids.ironHot;
        heat[ids.ice] = 0;
        heat[ids.floe] = ids.water;
        for (const auto& [base, frosted] : rimed)
            if (base != ids.iron && base != ids.knitIron) heat[frosted] = base;
        table.maps[Map_Cool][ids.knitIronHot] = ids.knitIron;
        table.maps[Map_Cool][ids.ironHot] = ids.iron;
        table.maps[Map_Rime][ids.knitIronHot] = ids.knitIron;
        table.maps[Map_Rime][ids.ironHot] = ids.iron;
        unknit[ids.knitIronHot] = 0;
        for (uint32_t stage = ids.pitch; stage != 0 && stage <= ids.pitchLast; ++stage) {
            table.maps[Map_Freeze][stage] = ids.ice;
            table.maps[Map_Knit][stage] = ids.knitIron;
            table.maps[Map_Slag][stage] = ids.slag;
            unknit[stage] = 0;
        }
        // Fire (spread.md): Ignite lights timber and leaves at their first kindle stage and scorches ground cover. Douse
        // saves a kindling voxel (its base), chars a burning one and puts out burning leaves and pitch; Rime and Freeze
        // carry Douse's rows for timber and leaves (Frost puts fire out; Freeze already turns pitch into ice).
        TurningMap& ignite = table.maps[Map_Ignite];
        ignite[ids.planks] = ids.planksK1;
        ignite[ids.wood] = ids.woodK1;
        ignite[ids.leaves] = ids.foliageK1;
        ignite[ids.leavesAutumn] = ids.foliageK1;
        ignite[ids.grass] = ids.scorched;
        ignite[ids.grassDry] = ids.scorched;
        struct FireRun {
            uint8_t first, count, to;
        };
        const FireRun douse[] = {{ids.planksK1, 6, ids.planks},     {ids.planksB1, 6, ids.charredWood}, {ids.woodK1, 8, ids.wood},
                                 {ids.woodB1, 8, ids.charredWood}, {ids.foliageK1, 3, 0},             {ids.foliageB1, 2, 0}};
        for (const FireRun& run : douse) {
            if (run.first == 0) continue;
            for (uint32_t i = 0; i < run.count; ++i) {
                const uint32_t id = run.first + i;
                table.maps[Map_Douse][id] = run.to;
                table.maps[Map_Rime][id] = run.to;
                table.maps[Map_Freeze][id] = run.to;
            }
        }
        for (uint32_t stage = ids.pitch; stage != 0 && stage <= ids.pitchLast; ++stage) table.maps[Map_Douse][stage] = 0;
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
