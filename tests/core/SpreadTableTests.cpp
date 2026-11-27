// Fire spread's integer rule in Core (docs/design/spread.md "Cell model and data", "The rules"): the fire ids and
// their values, the Ignite and Douse maps, the SpreadTable and its hash, and voxel::StepBox on one chunk.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

import vortex.core;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    /// One chunk (world chunk 0) stepped by StepBox; outside it every voxel is `pad` (bedrock unless set).
    struct FireYard {
        FireYard() : ids(static_cast<size_t>(kChunkVolume), 0) {
            MaterialLibrary library;
            m = RegisterDefaultWorldMaterials(library);
            table = BuildSpreadTable(m);
            pad = table.bedrock;
            all.fill(~uint64_t{0});
        }
        uint8_t& At(int x, int y, int z) { return ids[Chunk::VoxelIndex(x, y, z)]; }
        uint8_t Get(int x, int y, int z) const { return ids[Chunk::VoxelIndex(x, y, z)]; }
        void Fill(const glm::ivec3& lo, const glm::ivec3& hi, uint8_t id) {
            for (int z = lo.z; z < hi.z; ++z)
                for (int y = lo.y; y < hi.y; ++y)
                    for (int x = lo.x; x < hi.x; ++x) At(x, y, z) = id;
        }
        /// One step with s + 1; returns its counts.
        SpreadCounts Step() {
            auto box = std::make_unique<SpreadBox>();
            for (int z = -kSpreadPad; z < kChunkSize + kSpreadPad; ++z)
                for (int y = -kSpreadPad; y < kChunkSize + kSpreadPad; ++y)
                    for (int x = -kSpreadPad; x < kChunkSize + kSpreadPad; ++x)
                        box->ids[SpreadBox::Index(x, y, z)] = Chunk::InBounds(x, y, z) ? Get(x, y, z) : pad;
            std::vector<uint8_t> out = ids;
            SpreadCounts counts;
            ++s;
            StepBox(table, *box, glm::ivec3(0), s, all, out.data(), counts);
            ids = std::move(out);
            return counts;
        }
        uint32_t Count(const std::function<bool(uint8_t)>& keep) const {
            uint32_t n = 0;
            for (const uint8_t id : ids) n += keep(id) ? 1u : 0u;
            return n;
        }
        bool Burning(uint8_t id) const { return table.kind[id] == SpreadKind::Burning; }

        WorldMaterials m;
        SpreadTable table;
        std::vector<uint8_t> ids;
        uint8_t pad = 0;
        uint32_t s = 0;
        BlockMask all{};
    };
}

TEST(Materials, FireIdsAreAppendedInOrder) {
    MaterialLibrary library;
    const WorldMaterials ids = RegisterDefaultWorldMaterials(library);
    // Appended after the Thermal ids (pitch_b4 = 58): no id is renumbered.
    EXPECT_EQ(ids.bedrock, 20);
    EXPECT_EQ(ids.knitIron, 44);
    EXPECT_EQ(ids.pitchLast, 58);
    std::vector<std::string> names;
    for (int i = 1; i <= 6; ++i) names.push_back("planks_k" + std::to_string(i));
    for (int i = 1; i <= 6; ++i) names.push_back("planks_b" + std::to_string(i));
    for (int i = 1; i <= 8; ++i) names.push_back("wood_k" + std::to_string(i));
    for (int i = 1; i <= 8; ++i) names.push_back("wood_b" + std::to_string(i));
    for (int i = 1; i <= 3; ++i) names.push_back("foliage_k" + std::to_string(i));
    for (int i = 1; i <= 2; ++i) names.push_back("foliage_b" + std::to_string(i));
    names.push_back("charred_wood");
    names.push_back("scorched");
    ASSERT_EQ(names.size(), 35u);
    for (size_t i = 0; i < names.size(); ++i) EXPECT_EQ(library.Name(kWorldPalette, static_cast<uint8_t>(59 + i)), names[i]) << i;
    EXPECT_EQ(ids.planksK1, 59);
    EXPECT_EQ(ids.planksB1, 65);
    EXPECT_EQ(ids.woodK1, 71);
    EXPECT_EQ(ids.woodB1, 79);
    EXPECT_EQ(ids.foliageK1, 87);
    EXPECT_EQ(ids.foliageB1, 90);
    EXPECT_EQ(ids.charredWood, 92);
    EXPECT_EQ(ids.scorched, 93);
    // Burning stages carry the presentation flag and nothing flammable; foliage stays foliage (weightless, no collider).
    for (uint32_t id = 59; id <= 91; ++id) {
        const uint32_t flags = library.Get(kWorldPalette, static_cast<uint8_t>(id)).flags;
        EXPECT_NE(flags & MaterialFlag_Burning, 0u) << id;
        EXPECT_EQ(flags & MaterialFlag_Flammable, 0u) << id;
        EXPECT_EQ((flags & MaterialFlag_Foliage) != 0, id >= 87u) << id;
    }
    EXPECT_EQ(library.Get(kWorldPalette, ids.charredWood).flags, 0u);
    EXPECT_EQ(library.Get(kWorldPalette, ids.scorched).flags, 0u);
}

TEST(Materials, BurnStagesWeaken) {
    MaterialLibrary library;
    const WorldMaterials ids = RegisterDefaultWorldMaterials(library);
    const Material& planks = library.Get(kWorldPalette, ids.planks);
    const Material& last = library.Get(kWorldPalette, static_cast<uint8_t>(ids.planksB1 + 5));
    for (int k = 0; k < 3; ++k) {
        EXPECT_EQ(last.strength[k], planks.strength[k] / 6) << k;
        EXPECT_EQ(library.Get(kWorldPalette, ids.planksK1).strength[k], planks.strength[k]) << k;
        EXPECT_EQ(library.Get(kWorldPalette, ids.planksB1).strength[k], planks.strength[k]) << k;   // b1 of 6: 6/6
    }
    EXPECT_FLOAT_EQ(last.hardness, 0.6f * planks.hardness);
    EXPECT_FLOAT_EQ(library.Get(kWorldPalette, ids.planksK1).hardness, planks.hardness);
    const Material& wood = library.Get(kWorldPalette, ids.wood);
    EXPECT_EQ(library.Get(kWorldPalette, ids.charredWood).strength[0], wood.strength[0] / 4);
    EXPECT_FLOAT_EQ(library.Get(kWorldPalette, ids.charredWood).density, 280.0f);
}

TEST(TurningTable, IgniteMap) {
    MaterialLibrary library;
    const WorldMaterials ids = RegisterDefaultWorldMaterials(library);
    const TurningTable table = BuildTurningTable(ids);
    const TurningMap& ignite = table.maps[Map_Ignite];
    EXPECT_EQ(ignite[ids.planks], ids.planksK1);
    EXPECT_EQ(ignite[ids.wood], ids.woodK1);
    EXPECT_EQ(ignite[ids.leaves], ids.foliageK1);
    EXPECT_EQ(ignite[ids.leavesAutumn], ids.foliageK1);
    EXPECT_EQ(ignite[ids.grass], ids.scorched);
    EXPECT_EQ(ignite[ids.grassDry], ids.scorched);
    EXPECT_EQ(ignite[ids.bedrock], ids.bedrock);
    EXPECT_EQ(ignite[ids.stone], ids.stone);
    EXPECT_EQ(ignite[0], 0);
    uint32_t named = 0;
    for (uint32_t id = 0; id < 256; ++id) named += ignite[id] != id ? 1u : 0u;
    EXPECT_EQ(named, 6u);
}

TEST(SpreadTable, BedrockIsInert) {
    MaterialLibrary library;
    const WorldMaterials ids = RegisterDefaultWorldMaterials(library);
    const SpreadTable t = BuildSpreadTable(ids);
    EXPECT_EQ(t.bedrock, ids.bedrock);
    EXPECT_EQ(t.kind[ids.bedrock], SpreadKind::None);
    EXPECT_EQ(t.next[ids.bedrock], ids.bedrock);
    for (uint32_t id = 0; id < 256; ++id)
        if (id != ids.bedrock) EXPECT_NE(t.next[id], ids.bedrock) << id;
    for (const SpreadClassDef& c : t.classes) {
        EXPECT_NE(c.residue, ids.bedrock);
        EXPECT_NE(c.quenchEarly, ids.bedrock);
        EXPECT_NE(c.quenchLate, ids.bedrock);
        EXPECT_NE(c.firstKindle, ids.bedrock);
        EXPECT_NE(c.firstBurn, ids.bedrock);
    }
    // The classes as spread.md and magic.md give them; pitch is laid burning and never catches.
    EXPECT_EQ(t.kind[ids.pitch], SpreadKind::Burning);
    EXPECT_EQ(t.stage[ids.pitch], 1);
    EXPECT_EQ(t.next[ids.pitchLast], 0);
    EXPECT_EQ(t.classes[Class_Pitch].K, 0);
    EXPECT_EQ(t.classes[Class_Pitch].B, 4);
    EXPECT_EQ(t.kind[ids.water], SpreadKind::WetSource);
    EXPECT_EQ(t.kind[ids.grass], SpreadKind::GroundCover);
    EXPECT_EQ(t.next[static_cast<uint8_t>(ids.woodB1 + 7)], ids.charredWood);
    EXPECT_EQ(t.next[static_cast<uint8_t>(ids.planksK1 + 5)], ids.planksB1);
}

TEST(SpreadTable, PhaseIsSixteenDistinctAndMirrored) {
    const SpreadTable& t = WorldSpreadTable();
    std::set<uint8_t> values;
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y)
            for (int z = 0; z < 4; ++z) {
                const uint8_t p = SpreadPhase(t, {x, y, z});
                EXPECT_LT(p, 16);
                values.insert(p);
                EXPECT_EQ(p, SpreadPhase(t, {3 - x, y, z}));
                EXPECT_EQ(p, SpreadPhase(t, {x, y, 3 - z}));
                // Two's complement on negative coordinates: the pattern repeats every 4 voxels.
                EXPECT_EQ(p, SpreadPhase(t, {x - 64, y - 128, z - 4}));
            }
    EXPECT_EQ(values.size(), 16u);
}

TEST(SpreadTable, HashCoversEveryField) {
    const SpreadTable base = WorldSpreadTable();
    const uint64_t h = SpreadTableHash(base);
    EXPECT_EQ(h, SpreadTableHash(WorldSpreadTable()));
    std::vector<std::function<void(SpreadTable&)>> flips = {
        [](SpreadTable& t) { t.kind[200] = SpreadKind::Flammable; },
        [](SpreadTable& t) { t.cls[200] = 1; },
        [](SpreadTable& t) { t.stage[200] = 1; },
        [](SpreadTable& t) { t.next[200] = 1; },
        [](SpreadTable& t) { t.classes[Class_Wood].dJump = 11; },
        [](SpreadTable& t) { t.classes[Class_Pitch].pace = 9; },
        [](SpreadTable& t) { t.classes[Class_Planks].quenchLate = 1; },
        [](SpreadTable& t) { t.kPhase[1][3][1] = 0; },
        [](SpreadTable& t) { t.bedrock = 1; },
        [](SpreadTable& t) { t.air = 1; },
        [](SpreadTable& t) { t.charred = 1; },
    };
    for (size_t i = 0; i < flips.size(); ++i) {
        SpreadTable t = base;
        flips[i](t);
        EXPECT_NE(SpreadTableHash(t), h) << "flip " << i;
    }
}

// A block lit at one voxel of its middle layer: 300 steps, and no voxel under that layer ever leaves planks.
TEST(SpreadRule, FireNeverIgnitesBelow) {
    FireYard yard;
    yard.Fill({10, 10, 10}, {22, 22, 22}, yard.m.planks);
    yard.At(16, 16, 16) = yard.m.planksK1;
    for (int step = 0; step < 300; ++step) {
        yard.Step();
        for (int z = 10; z < 22; ++z)
            for (int y = 10; y < 16; ++y)
                for (int x = 10; x < 22; ++x) ASSERT_EQ(yard.Get(x, y, z), yard.m.planks) << "step " << step << " at " << x << ", " << y << ", " << z;
    }
    // The layer and above burnt.
    EXPECT_EQ(yard.Get(21, 21, 21), 0);
}

// One voxel per delay: a planks row sideways (dSide 3), a planks column upward (dBelow 2).
TEST(SpreadRule, FrontSpeedsMatchTheTable) {
    const auto farthest = [](const FireYard& yard, bool up) {
        int far = -1;
        for (int i = 0; i < 30; ++i) {
            const uint8_t id = up ? yard.Get(8, i, 8) : yard.Get(i, 8, 8);
            if (id != yard.m.planks) far = i;
        }
        return far;
    };
    for (const bool up : {false, true}) {
        FireYard yard;
        if (up) yard.Fill({8, 0, 8}, {9, 30, 9}, yard.m.planks);
        else yard.Fill({0, 8, 8}, {30, 9, 9}, yard.m.planks);
        if (up) yard.At(8, 0, 8) = yard.m.planksK1;
        else yard.At(0, 8, 8) = yard.m.planksK1;
        const int delay = up ? 2 : 3;
        int steps = 0;
        for (const int target : {10, 20, 40}) {
            while (steps < target) {
                yard.Step();
                ++steps;
            }
            const int expected = std::min(steps / delay, 29);
            EXPECT_NEAR(farthest(yard, up), expected, 1) << (up ? "up" : "side") << " after " << steps;
        }
    }
}

// Fire jumps one voxel of air sideways late in its burn, never two; leaves never jump.
TEST(SpreadRule, JumpsOneGapNotTwo) {
    for (const int gap : {1, 2}) {
        FireYard yard;
        yard.Fill({10, 4, 4}, {11, 12, 12}, yard.m.planksK1);
        yard.Fill({11 + gap, 4, 4}, {12 + gap, 12, 12}, yard.m.planks);
        bool caught = false;
        int when = -1;
        for (int step = 1; step <= 100 && !caught; ++step) {
            yard.Step();
            for (int z = 4; z < 12 && !caught; ++z)
                for (int y = 4; y < 12 && !caught; ++y)
                    if (yard.Get(11 + gap, y, z) != yard.m.planks) {
                        caught = true;
                        when = step;
                    }
        }
        if (gap == 1) {
            EXPECT_TRUE(caught);
            EXPECT_LE(when, 40);
        } else {
            EXPECT_FALSE(caught) << "a 2-voxel gap is a firebreak";
        }
    }
    FireYard leaves;
    leaves.Fill({10, 4, 4}, {11, 12, 12}, leaves.m.foliageK1);
    leaves.Fill({12, 4, 4}, {13, 12, 12}, leaves.m.leaves);
    for (int step = 0; step < 20; ++step) leaves.Step();
    for (int z = 4; z < 12; ++z)
        for (int y = 4; y < 12; ++y) EXPECT_EQ(leaves.Get(12, y, z), leaves.m.leaves);
}

// A one-voxel leaf sheet lit at one voxel: nothing beside it catches; a leaf column above the lit voxel burns up.
TEST(SpreadRule, FoliageOnlyClimbs) {
    FireYard yard;
    yard.Fill({4, 8, 4}, {20, 9, 20}, yard.m.leaves);
    yard.Fill({12, 9, 12}, {13, 15, 13}, yard.m.leaves);
    yard.At(12, 8, 12) = yard.m.foliageK1;
    for (int step = 0; step < 50; ++step) yard.Step();
    for (int z = 4; z < 20; ++z)
        for (int x = 4; x < 20; ++x)
            if (x != 12 || z != 12) EXPECT_EQ(yard.Get(x, 8, z), yard.m.leaves) << x << ", " << z;
    for (int y = 8; y < 15; ++y) EXPECT_EQ(yard.Get(12, y, 12), 0) << "the column burnt to its top: " << y;
}

// Mirrored about x = 15.5 and lit mirrored: every step the ids mirror (the rule reads old ids only and the phase is
// mirror-symmetric).
TEST(SpreadRule, SymmetricSetupStaysSymmetric) {
    FireYard yard;
    yard.Fill({8, 4, 8}, {24, 14, 16}, yard.m.planks);
    yard.Fill({12, 6, 10}, {20, 8, 12}, yard.m.wood);
    yard.At(10, 4, 11) = yard.m.planksK1;
    yard.At(21, 4, 11) = yard.m.planksK1;
    for (int step = 0; step < 120; ++step) {
        yard.Step();
        for (int z = 0; z < 32; ++z)
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 16; ++x) ASSERT_EQ(yard.Get(x, y, z), yard.Get(31 - x, y, z)) << "step " << step;
    }
}

TEST(SpreadRule, EveryFireBurnsOutWithinBound) {
    struct Case {
        uint8_t lit;
        uint32_t bound;
        uint8_t residue;
    };
    FireYard probe;
    const WorldMaterials& m = probe.m;
    for (const Case c : {Case{m.planksK1, 66, 0}, Case{m.woodK1, 88, m.charredWood}, Case{m.foliageK1, 9, 0}, Case{m.pitch, 40, 0}}) {
        for (int x = 0; x < 4; ++x) {   // every phase column of a block's first row
            FireYard yard;
            yard.At(8 + x, 9, 8) = c.lit;
            uint32_t steps = 0;
            while (steps < 200 && yard.Get(8 + x, 9, 8) != c.residue) {
                yard.Step();
                ++steps;
            }
            EXPECT_LE(steps, c.bound) << "id " << int{c.lit} << " x " << x;
        }
    }
}

TEST(SpreadRule, QuenchEarlySavesQuenchLateChars) {
    FireYard yard;
    yard.At(8, 8, 8) = yard.m.water;
    yard.At(9, 8, 8) = static_cast<uint8_t>(yard.m.planksK1 + 1);
    yard.At(7, 8, 8) = static_cast<uint8_t>(yard.m.planksB1 + 1);
    yard.At(8, 9, 8) = yard.m.foliageK1;
    yard.At(8, 8, 9) = yard.m.pitch;
    const SpreadCounts counts = yard.Step();
    EXPECT_EQ(counts.quenched, 4u);
    EXPECT_EQ(yard.Get(9, 8, 8), yard.m.planks) << "caught early: saved";
    EXPECT_EQ(yard.Get(7, 8, 8), yard.m.charredWood) << "caught late: charred";
    EXPECT_EQ(yard.Get(8, 9, 8), 0) << "burning leaves go out as air";
    EXPECT_EQ(yard.Get(8, 8, 9), 0) << "pitch goes out";
}

TEST(SpreadRule, WetBlocksIgnition) {
    FireYard yard;
    yard.At(8, 8, 8) = yard.m.planks;
    yard.At(8, 7, 8) = static_cast<uint8_t>(yard.m.planksB1 + 4);   // b5 under it: lights it at once when dry
    yard.At(9, 8, 8) = yard.m.water;
    for (int step = 0; step < 5; ++step) {
        yard.Step();
        EXPECT_EQ(yard.Get(8, 8, 8), yard.m.planks);
    }
    yard.At(9, 8, 8) = 0;
    yard.At(8, 7, 8) = static_cast<uint8_t>(yard.m.planksB1 + 4);
    yard.Step();
    EXPECT_EQ(yard.Get(8, 8, 8), yard.m.planksK1) << "dry: lit";
}

// A block lit whole at once: without the phase pattern every voxel would burn out on one step; with it the burnouts
// spread over the pace's ten steps, none of them more than twice the mean.
TEST(SpreadRule, BurnoutsSpreadEvenly) {
    FireYard yard;
    yard.Fill({4, 4, 4}, {28, 28, 28}, yard.m.planksK1);
    std::vector<uint32_t> burnouts;
    for (int step = 0; step < 80; ++step) burnouts.push_back(yard.Step().burnedOut);
    uint64_t sum = 0;
    uint32_t stepsWith = 0, most = 0;
    for (const uint32_t b : burnouts) {
        sum += b;
        stepsWith += b > 0 ? 1u : 0u;
        most = std::max(most, b);
    }
    ASSERT_EQ(sum, 24u * 24u * 24u);
    EXPECT_GE(stepsWith, 10u);
    EXPECT_LE(static_cast<uint64_t>(most) * stepsWith, 2 * sum);
}

// A fire inside the chunk reads the same with a bedrock pad and with an air pad.
TEST(SpreadRule, PadIsInert) {
    FireYard rock, air;
    air.pad = 0;
    for (FireYard* yard : {&rock, &air}) {
        yard->Fill({8, 8, 8}, {24, 24, 24}, yard->m.planks);
        yard->At(12, 8, 12) = yard->m.planksK1;
    }
    for (int step = 0; step < 100; ++step) {
        rock.Step();
        air.Step();
        ASSERT_EQ(rock.ids, air.ids) << "step " << step;
    }
}
