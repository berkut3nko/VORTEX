// The spell table (docs/design/magic.md "sim::SpellTable"): spell ids are form x 4 + element,
// sizes are converted from metres once and hold within one voxel at 0.1-0.15 m, and the hash
// sees every field.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

import vortex.voxel;
import vortex.sim;

using namespace vortex;

namespace {
    /// |value - metres| is at most one voxel.
    void ExpectWithinAVoxel(double value, double metres, float voxelSize, const char* what) {
        EXPECT_LE(std::abs(value - metres), static_cast<double>(voxelSize) + 1e-9) << what << " at " << voxelSize << " m";
    }
    double Metres(uint32_t q, float voxelSize) { return static_cast<double>(q) * static_cast<double>(voxelSize) / 16.0; }
}

TEST(SpellTable, IdsAreFormTimesFourPlusElement) {
    EXPECT_EQ(sim::kUpdraft, 6);
    EXPECT_EQ(sim::kDash, 10);
    EXPECT_EQ(sim::kBarricadeWave, 12);
    EXPECT_EQ(sim::kPunch, 14);
    for (uint8_t f = 0; f < 4; ++f)
        for (uint8_t e = 0; e < 4; ++e) {
            const sim::SpellId id = sim::MakeSpell(static_cast<sim::Form>(f), static_cast<sim::Element>(e));
            EXPECT_EQ(id, f * 4 + e);
            EXPECT_EQ(sim::FormOf(id), static_cast<sim::Form>(f));
            EXPECT_EQ(sim::ElementOf(id), static_cast<sim::Element>(e));
        }
}

TEST(SpellTable, SizesHoldWithinOneVoxelAcrossVoxelSizes) {
    for (const float vs : {0.1f, 0.125f, 0.15f}) {
        const sim::SpellTable t = sim::BuildSpellTable(vs);
        EXPECT_EQ(t.voxelSize, vs);
        const sim::SpellDef& wall = t.spells[sim::kBarricadeWave];
        const sim::SpellDef& updraft = t.spells[sim::kUpdraft];
        const sim::SpellDef& dash = t.spells[sim::kDash];
        const sim::SpellDef& punch = t.spells[sim::kPunch];
        ExpectWithinAVoxel(wall.rimHeight * static_cast<double>(vs), 1.5, vs, "Barricade Wave's rim");
        ExpectWithinAVoxel(wall.burstHalfWidth * static_cast<double>(vs), 2.25, vs, "Barricade Wave's half width");
        ExpectWithinAVoxel(updraft.riseMax * static_cast<double>(vs), 10.0, vs, "Updraft's column");
        ExpectWithinAVoxel(Metres(updraft.zone.radiusQ, vs), 1.0, vs, "Updraft's radius");
        ExpectWithinAVoxel(Metres(dash.dashRangeQ, vs), 8.0, vs, "the dash's range");
        ExpectWithinAVoxel(Metres(dash.zone.radiusQ, vs), 1.0, vs, "the dash's shove radius");
        ExpectWithinAVoxel(Metres(punch.fistRangeQ, vs), 2.5, vs, "the fist's range");
        ExpectWithinAVoxel(Metres(punch.blowRadiusQ, vs), 0.75, vs, "the blow's radius");
        ExpectWithinAVoxel(Metres(punch.zone.farQ, vs), 3.0, vs, "the shove's end");
        ExpectWithinAVoxel(Metres(punch.dispelRadiusQ, vs), 2.5, vs, "the dispel radius");
        ExpectWithinAVoxel(Metres(t.ablateRadiusQ, vs), 0.25, vs, "the ablated patch");
        const sim::SpellDef& spike = t.spells[sim::kShatterSpike];
        const sim::SpellDef& freeze = t.spells[sim::kFlashFreeze];
        const sim::SpellDef& climb = t.spells[sim::kPermafrostClimb];
        ExpectWithinAVoxel(spike.pierceMaxSlices * static_cast<double>(vs), 3.0, vs, "the spike's slices");
        ExpectWithinAVoxel(Metres(spike.rimeRadiusQ, vs), 0.75, vs, "the spike's Rime capsule");
        ExpectWithinAVoxel(freeze.burstDiscRadius * static_cast<double>(vs), 3.0, vs, "Flash Freeze's disc");
        ExpectWithinAVoxel(Metres(freeze.floeRadiusQ, vs), 3.0, vs, "the floe slab");
        ExpectWithinAVoxel(climb.riseMax * static_cast<double>(vs), 6.0, vs, "Permafrost Climb's rise");
        ExpectWithinAVoxel(Metres(punch.shatterRadiusQ, vs), 1.0, vs, "Punch's Shatter flood");
        EXPECT_EQ(spike.pierceRadiusQ, 24) << "a bore's radius is counted in voxels";
        for (sim::SpellId id = 0; id < sim::kSpellCount; ++id) {
            const bool built = id == sim::kUpdraft || id == sim::kDash || id == sim::kBarricadeWave || id == sim::kPunch ||
                               id == sim::kShatterSpike || id == sim::kFlashFreeze || id == sim::kPermafrostClimb;
            EXPECT_EQ(t.spells[id].built, built) << "spell " << int(id);
        }
    }

    const sim::SpellTable t = sim::BuildSpellTable(0.125f);
    EXPECT_EQ(t.forms[static_cast<size_t>(sim::Form::Flow)].speedMs, 20);
    EXPECT_EQ(t.forms[static_cast<size_t>(sim::Form::Plume)].speedMs, 30);
    EXPECT_EQ(t.forms[static_cast<size_t>(sim::Form::Pierce)].speedMs, 80);
    EXPECT_EQ(t.forms[static_cast<size_t>(sim::Form::Burst)].speedMs, 40);
    EXPECT_EQ(t.metreQ, 128);
    EXPECT_EQ(t.manaPool, 6000);
    EXPECT_EQ(t.manaPerTick, 10);
    EXPECT_EQ(t.spellGapTicks, 12);
    EXPECT_EQ(t.spells[sim::kDash].manaCost, 1200);
    EXPECT_EQ(t.spells[sim::kBarricadeWave].manaCost, 1950);
    EXPECT_EQ(t.gestureReachQ, 3968u);
    EXPECT_EQ(t.gestureStepQ, 256u);
    EXPECT_EQ(t.gesturePathQ, 1536u);
    EXPECT_EQ(t.ablateRadiusQ, 32);
    EXPECT_EQ(t.zoneLeadTicks, 24u);

    const sim::SpellDef& wall = t.spells[sim::kBarricadeWave];
    EXPECT_EQ(wall.burstHalfWidth, 18);
    EXPECT_EQ(wall.burstHalfThickness, 1);
    EXPECT_EQ(wall.stepMax, 3);
    EXPECT_EQ(wall.burstPops, 4000);
    EXPECT_EQ(wall.burstVisitCap, 6000);
    EXPECT_EQ(wall.burstPlane, 34);
    EXPECT_EQ(wall.burstNormalMin, -4);
    EXPECT_EQ(wall.burstNormalMax, 14);
    EXPECT_EQ(wall.fillMap, voxel::Map_Knit);
    EXPECT_EQ(wall.rimHeight, 12);
    EXPECT_EQ(wall.bandCount, 4);
    EXPECT_EQ(wall.bandFirstTick, 18);
    EXPECT_EQ(wall.bandSpacingTicks, 4);
    EXPECT_EQ(wall.lifetimeTicks, 1200);
    EXPECT_EQ(wall.expiryMap, voxel::Map_Unknit);
    EXPECT_EQ(wall.zone.kind, sim::ZoneKind::None);

    const sim::SpellDef& updraft = t.spells[sim::kUpdraft];
    EXPECT_EQ(updraft.riseMax, 80);
    EXPECT_EQ(updraft.zone.kind, sim::ZoneKind::Lift);
    EXPECT_EQ(updraft.zone.place, sim::ZonePlace::UpdraftColumn);
    EXPECT_EQ(updraft.zone.target, sim::ZoneTarget::Everyone);
    EXPECT_EQ(updraft.zone.durationTicks, 90);
    EXPECT_EQ(updraft.zone.strength[0], 900);
    EXPECT_EQ(updraft.zone.strength[1], 0);
    EXPECT_EQ(updraft.zone.radiusQ, 128);
    EXPECT_EQ(updraft.zone.rampQ, 32);
    EXPECT_EQ(updraft.zone.hoverQ, 64);
    EXPECT_EQ(updraft.maxBodies, 8);
    EXPECT_EQ(updraft.maxBodyKickCmS, 1200);

    const sim::SpellDef& dash = t.spells[sim::kDash];
    EXPECT_EQ(dash.dashTicks, 18);
    EXPECT_EQ(dash.dashSpeedCmS, 2670);
    EXPECT_EQ(dash.dashEndPushCmS, 400);
    EXPECT_EQ(dash.dashRangeQ, 1024);
    EXPECT_EQ(dash.dashStepQ, 16);
    EXPECT_EQ(dash.zone.kind, sim::ZoneKind::Impulse);
    EXPECT_EQ(dash.zone.place, sim::ZonePlace::DashPath);
    EXPECT_EQ(dash.zone.target, sim::ZoneTarget::Enemies);
    EXPECT_EQ(dash.zone.durationTicks, 1);
    EXPECT_EQ(dash.zone.strength[0], 600);
    EXPECT_EQ(dash.zone.strength[1], 200);
    EXPECT_EQ(dash.zone.radiusQ, 128);
    EXPECT_EQ(dash.directDamage, 0);
    EXPECT_EQ(dash.zoneDamage, 0);
    EXPECT_EQ(dash.maxBodies, 8);
    EXPECT_EQ(dash.maxBodyKickCmS, 1200);

    const sim::SpellDef& punch = t.spells[sim::kPunch];
    EXPECT_EQ(punch.fistRangeQ, 320);
    EXPECT_EQ(punch.blowRadiusQ, 96);
    EXPECT_EQ(punch.blowPower, 40);
    EXPECT_EQ(punch.zone.kind, sim::ZoneKind::Shove);
    EXPECT_EQ(punch.zone.place, sim::ZonePlace::PunchShove);
    EXPECT_EQ(punch.zone.target, sim::ZoneTarget::Enemies);
    EXPECT_EQ(punch.zone.durationTicks, 1);
    EXPECT_EQ(punch.zone.strength[0], 1200);
    EXPECT_EQ(punch.zone.strength[1], 300);
    EXPECT_EQ(punch.zone.radiusQ, 128);
    EXPECT_EQ(punch.zone.nearQ, 64);
    EXPECT_EQ(punch.zone.farQ, 384);
    EXPECT_EQ(punch.zoneDamage, 25);
    EXPECT_EQ(punch.dispelRadiusQ, 320);
    EXPECT_EQ(punch.maxBodies, 8);
    EXPECT_EQ(punch.maxBodyKickCmS, 1200);
}

TEST(SpellTable, HashCoversEveryField) {
    const sim::SpellTable base = sim::BuildSpellTable(0.125f);
    const uint64_t baseHash = sim::SpellTableHash(base);
    EXPECT_EQ(sim::SpellTableHash(sim::BuildSpellTable(0.125f)), baseHash);
    EXPECT_NE(sim::SpellTableHash(sim::BuildSpellTable(0.1f)), baseHash);

    using Change = std::pair<std::string, std::function<void(sim::SpellTable&)>>;
    const auto S = [](sim::SpellTable& t) -> sim::SpellDef& { return t.spells[sim::kPunch]; };
    const std::vector<Change> changes = {
        {"voxelSize", [](sim::SpellTable& t) { t.voxelSize = 0.126f; }},
        {"zoneLeadTicks", [](sim::SpellTable& t) { ++t.zoneLeadTicks; }},
        {"ablateRadiusQ", [](sim::SpellTable& t) { ++t.ablateRadiusQ; }},
        {"forms[2]", [](sim::SpellTable& t) { ++t.forms[2].speedMs; }},
        {"forms[3]", [](sim::SpellTable& t) { ++t.forms[3].speedMs; }},
        {"metreQ", [](sim::SpellTable& t) { ++t.metreQ; }},
        {"manaPool", [](sim::SpellTable& t) { ++t.manaPool; }},
        {"manaPerTick", [](sim::SpellTable& t) { ++t.manaPerTick; }},
        {"spellGapTicks", [](sim::SpellTable& t) { ++t.spellGapTicks; }},
        {"drawBaseMana", [](sim::SpellTable& t) { ++t.drawBaseMana; }},
        {"drawManaPerMetre", [](sim::SpellTable& t) { ++t.drawManaPerMetre; }},
        {"maxFlightTicks", [](sim::SpellTable& t) { ++t.maxFlightTicks; }},
        {"lineSpeedMs", [](sim::SpellTable& t) { ++t.lineSpeedMs; }},
        {"gestureReachQ", [](sim::SpellTable& t) { ++t.gestureReachQ; }},
        {"gestureStepQ", [](sim::SpellTable& t) { ++t.gestureStepQ; }},
        {"gesturePathQ", [](sim::SpellTable& t) { ++t.gesturePathQ; }},
        {"gestureSupportQ", [](sim::SpellTable& t) { ++t.gestureSupportQ; }},
        {"manaCost", [&S](sim::SpellTable& t) { ++S(t).manaCost; }},
        {"noProjectile", [&S](sim::SpellTable& t) { S(t).noProjectile = !S(t).noProjectile; }},
        {"drawsLine", [&S](sim::SpellTable& t) { S(t).drawsLine = !S(t).drawsLine; }},
        {"drawHalfWidthQ", [&S](sim::SpellTable& t) { ++S(t).drawHalfWidthQ; }},
        {"drawHeight", [&S](sim::SpellTable& t) { ++S(t).drawHeight; }},
        {"drawMinQ", [&S](sim::SpellTable& t) { ++S(t).drawMinQ; }},
        {"drawMaxQ", [&S](sim::SpellTable& t) { ++S(t).drawMaxQ; }},
        {"drawZoneRadiusQ", [&S](sim::SpellTable& t) { ++S(t).drawZoneRadiusQ; }},
        {"a spell not built", [](sim::SpellTable& t) { t.spells[0].built = true; }},
        {"built", [&S](sim::SpellTable& t) { S(t).built = false; }},
        {"burstHalfWidth", [&S](sim::SpellTable& t) { ++S(t).burstHalfWidth; }},
        {"burstHalfThickness", [&S](sim::SpellTable& t) { ++S(t).burstHalfThickness; }},
        {"burstPops", [&S](sim::SpellTable& t) { ++S(t).burstPops; }},
        {"burstVisitCap", [&S](sim::SpellTable& t) { ++S(t).burstVisitCap; }},
        {"stepMax", [&S](sim::SpellTable& t) { ++S(t).stepMax; }},
        {"burstPlane", [&S](sim::SpellTable& t) { ++S(t).burstPlane; }},
        {"burstNormalMin", [&S](sim::SpellTable& t) { --S(t).burstNormalMin; }},
        {"burstNormalMax", [&S](sim::SpellTable& t) { --S(t).burstNormalMax; }},
        {"riseMax", [&S](sim::SpellTable& t) { ++S(t).riseMax; }},
        {"dashTicks", [&S](sim::SpellTable& t) { ++S(t).dashTicks; }},
        {"dashSpeedCmS", [&S](sim::SpellTable& t) { ++S(t).dashSpeedCmS; }},
        {"dashEndPushCmS", [&S](sim::SpellTable& t) { ++S(t).dashEndPushCmS; }},
        {"dashRangeQ", [&S](sim::SpellTable& t) { ++S(t).dashRangeQ; }},
        {"dashStepQ", [&S](sim::SpellTable& t) { ++S(t).dashStepQ; }},
        {"fistRangeQ", [&S](sim::SpellTable& t) { ++S(t).fistRangeQ; }},
        {"blowRadiusQ", [&S](sim::SpellTable& t) { ++S(t).blowRadiusQ; }},
        {"blowPower", [&S](sim::SpellTable& t) { ++S(t).blowPower; }},
        {"fillMap", [&S](sim::SpellTable& t) { ++S(t).fillMap; }},
        {"rimHeight", [&S](sim::SpellTable& t) { ++S(t).rimHeight; }},
        {"bandCount", [&S](sim::SpellTable& t) { ++S(t).bandCount; }},
        {"bandFirstTick", [&S](sim::SpellTable& t) { ++S(t).bandFirstTick; }},
        {"bandSpacingTicks", [&S](sim::SpellTable& t) { ++S(t).bandSpacingTicks; }},
        {"lifetimeTicks", [&S](sim::SpellTable& t) { ++S(t).lifetimeTicks; }},
        {"expiryMap", [&S](sim::SpellTable& t) { ++S(t).expiryMap; }},
        {"zone.kind", [&S](sim::SpellTable& t) { S(t).zone.kind = sim::ZoneKind::Impulse; }},
        {"zone.place", [&S](sim::SpellTable& t) { S(t).zone.place = sim::ZonePlace::DashPath; }},
        {"zone.target", [&S](sim::SpellTable& t) { S(t).zone.target = sim::ZoneTarget::Allies; }},
        {"zone.durationTicks", [&S](sim::SpellTable& t) { ++S(t).zone.durationTicks; }},
        {"zone.strength[0]", [&S](sim::SpellTable& t) { S(t).zone.strength[0] = -S(t).zone.strength[0]; }},
        {"zone.strength[1]", [&S](sim::SpellTable& t) { ++S(t).zone.strength[1]; }},
        {"zone.radiusQ", [&S](sim::SpellTable& t) { ++S(t).zone.radiusQ; }},
        {"zone.nearQ", [&S](sim::SpellTable& t) { ++S(t).zone.nearQ; }},
        {"zone.farQ", [&S](sim::SpellTable& t) { ++S(t).zone.farQ; }},
        {"zone.rampQ", [&S](sim::SpellTable& t) { ++S(t).zone.rampQ; }},
        {"zone.hoverQ", [&S](sim::SpellTable& t) { ++S(t).zone.hoverQ; }},
        {"directDamage", [&S](sim::SpellTable& t) { ++S(t).directDamage; }},
        {"zoneDamage", [&S](sim::SpellTable& t) { ++S(t).zoneDamage; }},
        {"maxBodies", [&S](sim::SpellTable& t) { ++S(t).maxBodies; }},
        {"maxBodyKickCmS", [&S](sim::SpellTable& t) { ++S(t).maxBodyKickCmS; }},
        {"dispelRadiusQ", [&S](sim::SpellTable& t) { ++S(t).dispelRadiusQ; }},
        {"burstDiscRadius", [&S](sim::SpellTable& t) { ++S(t).burstDiscRadius; }},
        {"plumeSeedRadius", [&S](sim::SpellTable& t) { ++S(t).plumeSeedRadius; }},
        {"plumeRunPops", [&S](sim::SpellTable& t) { ++S(t).plumeRunPops; }},
        {"plumeLateral", [&S](sim::SpellTable& t) { ++S(t).plumeLateral; }},
        {"plumeVisitCap", [&S](sim::SpellTable& t) { ++S(t).plumeVisitCap; }},
        {"supportReach", [&S](sim::SpellTable& t) { ++S(t).supportReach; }},
        {"pierceRadiusQ", [&S](sim::SpellTable& t) { ++S(t).pierceRadiusQ; }},
        {"piercePower", [&S](sim::SpellTable& t) { ++S(t).piercePower; }},
        {"pierceBudget", [&S](sim::SpellTable& t) { ++S(t).pierceBudget; }},
        {"pierceMaxSlices", [&S](sim::SpellTable& t) { ++S(t).pierceMaxSlices; }},
        {"floeLifetimeTicks", [&S](sim::SpellTable& t) { ++S(t).floeLifetimeTicks; }},
        {"floeRadiusQ", [&S](sim::SpellTable& t) { ++S(t).floeRadiusQ; }},
        {"rimeRadiusQ", [&S](sim::SpellTable& t) { ++S(t).rimeRadiusQ; }},
        {"rimeDepth", [&S](sim::SpellTable& t) { ++S(t).rimeDepth; }},
        {"rimeLifetimeTicks", [&S](sim::SpellTable& t) { ++S(t).rimeLifetimeTicks; }},
        {"shatterRadiusQ", [&S](sim::SpellTable& t) { ++S(t).shatterRadiusQ; }},
        {"shatterDepth", [&S](sim::SpellTable& t) { ++S(t).shatterDepth; }},
        {"shatterVisitCap", [&S](sim::SpellTable& t) { ++S(t).shatterVisitCap; }},
        {"zone.halfHeightQ", [&S](sim::SpellTable& t) { ++S(t).zone.halfHeightQ; }},
        {"zone2.kind", [&S](sim::SpellTable& t) { S(t).zone2.kind = sim::ZoneKind::Chill; }},
        {"zone2.durationTicks", [&S](sim::SpellTable& t) { ++S(t).zone2.durationTicks; }},
        {"release.kind", [&S](sim::SpellTable& t) { S(t).release.kind = sim::ZoneKind::None; }},
        {"release.target", [&S](sim::SpellTable& t) { S(t).release.target = sim::ZoneTarget::Everyone; }},
        {"boreCost", [](sim::SpellTable& t) { ++t.boreCost[3]; }},
    };
    for (const auto& [name, change] : changes) {
        sim::SpellTable copy = base;
        change(copy);
        EXPECT_NE(sim::SpellTableHash(copy), baseHash) << name;
    }
}
