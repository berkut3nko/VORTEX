module;

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

module vortex.sim;

import vortex.voxel;

namespace vortex::sim {

    namespace {
        uint64_t Signed(int16_t v) { return static_cast<uint64_t>(static_cast<uint16_t>(v)); }

        uint64_t MixZone(uint64_t h, const ZoneDef& z) {
            h = voxel::HashMix64(h, static_cast<uint8_t>(z.kind));
            h = voxel::HashMix64(h, static_cast<uint8_t>(z.place));
            h = voxel::HashMix64(h, static_cast<uint8_t>(z.target));
            h = voxel::HashMix64(h, z.durationTicks);
            h = voxel::HashMix64(h, Signed(z.strength[0]));
            h = voxel::HashMix64(h, Signed(z.strength[1]));
            h = voxel::HashMix64(h, z.radiusQ);
            h = voxel::HashMix64(h, z.nearQ);
            h = voxel::HashMix64(h, z.farQ);
            h = voxel::HashMix64(h, z.rampQ);
            h = voxel::HashMix64(h, z.hoverQ);
            h = voxel::HashMix64(h, z.halfHeightQ);
            return h;
        }

        /// Every field of SpellDef in declaration order: a field added to the struct is added here.
        uint64_t MixSpell(uint64_t h, const SpellDef& s) {
            h = voxel::HashMix64(h, s.built ? 1u : 0u);
            h = voxel::HashMix64(h, s.burstHalfWidth);
            h = voxel::HashMix64(h, s.burstHalfThickness);
            h = voxel::HashMix64(h, s.burstPops);
            h = voxel::HashMix64(h, s.burstVisitCap);
            h = voxel::HashMix64(h, s.stepMax);
            h = voxel::HashMix64(h, s.burstPlane);
            h = voxel::HashMix64(h, Signed(s.burstNormalMin));
            h = voxel::HashMix64(h, Signed(s.burstNormalMax));
            h = voxel::HashMix64(h, s.burstDiscRadius);
            h = voxel::HashMix64(h, s.riseMax);
            h = voxel::HashMix64(h, s.plumeSeedRadius);
            h = voxel::HashMix64(h, s.plumeRunPops);
            h = voxel::HashMix64(h, s.plumeLateral);
            h = voxel::HashMix64(h, s.plumeVisitCap);
            h = voxel::HashMix64(h, s.supportReach);
            h = voxel::HashMix64(h, s.pierceRadiusQ);
            h = voxel::HashMix64(h, s.piercePower);
            h = voxel::HashMix64(h, s.pierceBudget);
            h = voxel::HashMix64(h, s.pierceMaxSlices);
            h = voxel::HashMix64(h, s.dashTicks);
            h = voxel::HashMix64(h, s.dashSpeedCmS);
            h = voxel::HashMix64(h, s.dashEndPushCmS);
            h = voxel::HashMix64(h, s.dashRangeQ);
            h = voxel::HashMix64(h, s.dashStepQ);
            h = voxel::HashMix64(h, s.fistRangeQ);
            h = voxel::HashMix64(h, s.blowRadiusQ);
            h = voxel::HashMix64(h, s.blowPower);
            h = voxel::HashMix64(h, s.fillMap);
            h = voxel::HashMix64(h, s.rimHeight);
            h = voxel::HashMix64(h, s.bandCount);
            h = voxel::HashMix64(h, s.bandFirstTick);
            h = voxel::HashMix64(h, s.bandSpacingTicks);
            h = voxel::HashMix64(h, s.lifetimeTicks);
            h = voxel::HashMix64(h, s.expiryMap);
            h = voxel::HashMix64(h, s.floeLifetimeTicks);
            h = voxel::HashMix64(h, s.floeRadiusQ);
            h = voxel::HashMix64(h, s.rimeRadiusQ);
            h = voxel::HashMix64(h, s.rimeDepth);
            h = voxel::HashMix64(h, s.rimeLifetimeTicks);
            h = voxel::HashMix64(h, s.shatterRadiusQ);
            h = voxel::HashMix64(h, s.shatterDepth);
            h = voxel::HashMix64(h, s.shatterVisitCap);
            h = MixZone(h, s.zone);
            h = MixZone(h, s.zone2);
            h = MixZone(h, s.release);
            h = voxel::HashMix64(h, s.directDamage);
            h = voxel::HashMix64(h, s.zoneDamage);
            h = voxel::HashMix64(h, s.maxBodies);
            h = voxel::HashMix64(h, s.maxBodyKickCmS);
            h = voxel::HashMix64(h, s.dispelRadiusQ);
            return h;
        }
    }

    std::array<uint16_t, 256> BoreCostTable() {
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials m = voxel::RegisterDefaultWorldMaterials(scratch);
        std::array<uint16_t, 256> c{};
        // round(hardness * 16), written out per id (Materials.BoreCostMatchesHardness checks them).
        const std::pair<uint8_t, uint16_t> costs[] = {
            {m.grass, 10}, {m.grassDry, 10}, {m.dirt, 13}, {m.stone, 48}, {m.stoneDark, 64}, {m.sand, 6}, {m.snow, 3},
            {m.water, 0}, {m.wood, 24}, {m.leaves, 3}, {m.leavesAutumn, 3}, {m.planks, 19}, {m.brick, 40}, {m.iron, 128},
            {m.gold, 40}, {m.crystal, 48}, {m.lava, 0}, {m.torch, 5}, {m.glass, 8}, {m.bedrock, kBoreStop},
            {m.moss, 10}, {m.litter, 10}, {m.mud, 8}, {m.gravel, 16}, {m.deadwood, 16}, {m.needles, 3}, {m.reed, 2},
            {m.fern, 2}, {m.flowerYellow, 2}, {m.flowerViolet, 2}, {m.mushroom, 2}, {m.barkPale, 22},
            {m.granite, 120}, {m.graniteQuoin, 120}, {m.ashlar, 48}, {m.rubbleCore, 32}, {m.oakIron, 104},
            {m.slate, 13}, {m.roofTile, 13}, {m.thatch, 5}, {m.wattleDaub, 10}, {m.soot, 48}, {m.graniteDamp, 120},
            {m.knitIron, 112}, {m.ice, 16}, {m.floe, kBoreStop}, {m.knitIronRimed, 56}, {m.stoneRimed, 24},
            {m.stoneDarkRimed, 32}, {m.brickRimed, 20}, {m.ironRimed, 64},
        };
        for (const auto& [id, cost] : costs) c[id] = cost;
        return c;
    }

    SpellTable BuildSpellTable(float voxelSize) {
        const double vs = static_cast<double>(voxelSize);
        // Metres to voxels and to q, once; std::lround rounds halves away from zero.
        const auto V = [vs](double m) { return static_cast<int32_t>(std::lround(m / vs)); };
        const auto Q = [vs](double m) { return static_cast<uint16_t>(std::lround(m / vs * 16.0)); };
        const auto U16 = [](int32_t v) { return static_cast<uint16_t>(v); };
        const auto U8 = [](int32_t v) { return static_cast<uint8_t>(v); };

        SpellTable t;
        t.voxelSize = voxelSize;
        t.forms[static_cast<size_t>(Form::Flow)].cooldownTicks = 540;
        t.forms[static_cast<size_t>(Form::Plume)].cooldownTicks = 540;
        t.forms[static_cast<size_t>(Form::Pierce)].cooldownTicks = 300;
        t.forms[static_cast<size_t>(Form::Burst)].cooldownTicks = 420;
        t.ablateRadiusQ = Q(0.25);
        t.boreCost = BoreCostTable();

        {   // Updraft: a Lift over a single Plume column, everyone.
            SpellDef& s = t.spells[kUpdraft];
            s.built = true;
            s.riseMax = U16(V(10.0));
            s.zone.kind = ZoneKind::Lift;
            s.zone.place = ZonePlace::UpdraftColumn;
            s.zone.target = ZoneTarget::Everyone;
            s.zone.durationTicks = 90;
            s.zone.strength[0] = 900;
            s.zone.radiusQ = Q(1.0);
            s.zone.rampQ = Q(0.25);
            s.zone.hoverQ = Q(0.5);
            s.maxBodies = 8;
            s.maxBodyKickCmS = 1200;
        }
        {   // Dash: the caster moves; an Impulse capsule along the traced path shoves enemies aside.
            SpellDef& s = t.spells[kDash];
            s.built = true;
            s.dashTicks = kDashTicks;
            s.dashSpeedCmS = kDashSpeedCmS;
            s.dashEndPushCmS = kDashEndPushCmS;
            s.dashRangeQ = Q(8.0);
            s.dashStepQ = Q(0.125);
            s.zone.kind = ZoneKind::Impulse;
            s.zone.place = ZonePlace::DashPath;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 1;
            s.zone.strength[0] = 600;
            s.zone.strength[1] = 200;
            s.zone.radiusQ = Q(1.0);
            s.maxBodies = 8;
            s.maxBodyKickCmS = 1200;
        }
        {   // Barricade Wave: a Burst strip across the aim, extruded into a knit wall in bands.
            SpellDef& s = t.spells[kBarricadeWave];
            s.built = true;
            s.burstHalfWidth = U16(V(2.25));
            s.burstHalfThickness = U16(std::max(1, V(0.125)));
            s.stepMax = U8(V(0.375));
            s.burstPops = 4000;
            s.burstVisitCap = 6000;
            s.burstPlane = U16(V(4.25));
            s.burstNormalMin = static_cast<int16_t>(-V(0.5));
            s.burstNormalMax = static_cast<int16_t>(V(1.75));
            s.fillMap = voxel::Map_Knit;
            s.rimHeight = U8(V(1.5));
            s.bandCount = 4;
            s.bandFirstTick = 18;
            s.bandSpacingTicks = 4;
            s.lifetimeTicks = 1200;
            s.expiryMap = voxel::Map_Unknit;
        }
        {   // Punch: a blow at the fist point and a Shove capsule along the aim; dispels lasting zones.
            SpellDef& s = t.spells[kPunch];
            s.built = true;
            s.fistRangeQ = Q(2.5);
            s.blowRadiusQ = Q(0.75);
            s.blowPower = 40;
            s.zone.kind = ZoneKind::Shove;
            s.zone.place = ZonePlace::PunchShove;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 1;
            s.zone.strength[0] = 1200;
            s.zone.strength[1] = 300;
            s.zone.radiusQ = Q(1.0);
            s.zone.nearQ = Q(0.5);
            s.zone.farQ = Q(3.0);
            s.zoneDamage = 25;
            s.dispelRadiusQ = Q(2.5);
            s.maxBodies = 8;
            s.maxBodyKickCmS = 1200;
            // An ally Release of the same capsule, and the Shatter flood round the fist, at T_z.
            s.release = s.zone;
            s.release.kind = ZoneKind::Release;
            s.release.target = ZoneTarget::Allies;
            s.release.strength[0] = s.release.strength[1] = 0;
            s.shatterRadiusQ = Q(1.0);
            s.shatterDepth = U8(std::max(1, V(0.5)));
            s.shatterVisitCap = 4000;
        }
        {   // Shatter Spike: a bore, a Rime capsule round it and past its end, a Chill at the hit.
            SpellDef& s = t.spells[kShatterSpike];
            s.built = true;
            s.pierceRadiusQ = 24;   // 1.5 voxels: a bore is counted in voxels
            s.piercePower = 64;
            s.pierceMaxSlices = U16(V(3.0));
            // One metre of stone at 0.125 m (8 slices of about 7 voxels at 48), scaled with the slices of a metre.
            s.pierceBudget = static_cast<uint32_t>(2688 * V(1.0) / 8);
            s.rimeRadiusQ = Q(0.75);
            s.rimeLifetimeTicks = 480;
            s.zone.kind = ZoneKind::Chill;
            s.zone.place = ZonePlace::HitCylinder;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 180;
            s.zone.strength[0] = 6;
            s.zone.strength[1] = 60;
            s.zone.radiusQ = Q(1.0);
            s.zone.halfHeightQ = Q(2.0);   // feet below a hit on a wall at eye height are inside
            s.directDamage = 25;
        }
        {   // Flash Freeze: a Burst disc; water under it turns into a floe slab, dry cells into an ice film.
            SpellDef& s = t.spells[kFlashFreeze];
            s.built = true;
            s.burstDiscRadius = U16(V(3.0));
            s.stepMax = U8(V(0.375));
            s.burstPops = 4000;
            // A disc tests about 4 cells per popped cell: the cap follows its area at the voxel size.
            s.burstVisitCap = U16(std::min(60000, 16 * V(3.0) * V(3.0)));
            s.burstPlane = U16(V(3.0) + 2);
            s.burstNormalMin = static_cast<int16_t>(-V(0.5));
            s.burstNormalMax = static_cast<int16_t>(V(1.75));
            s.fillMap = voxel::Map_Freeze;
            s.lifetimeTicks = 720;
            s.expiryMap = voxel::Map_Unknit;
            s.floeLifetimeTicks = 900;
            s.floeRadiusQ = Q(3.0);
            s.zone.kind = ZoneKind::Root;
            s.zone.place = ZonePlace::DiscCylinder;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 60;
            s.zone.radiusQ = Q(3.0);
            s.zone.halfHeightQ = Q(1.0);
            s.zone2 = s.zone;
            s.zone2.kind = ZoneKind::Chill;
            s.zone2.durationTicks = 180;
            s.zone2.strength[0] = 6;
            s.zone2.strength[1] = 60;
            s.zoneDamage = 10;
        }
        {   // Permafrost Climb: the full Plume; ice on its Rise cells near support and on joined Front cells.
            SpellDef& s = t.spells[kPermafrostClimb];
            s.built = true;
            s.plumeSeedRadius = U8(V(0.375));
            s.riseMax = U16(V(6.0));
            s.plumeRunPops = 400;
            s.plumeLateral = U16(V(2.0));
            s.plumeVisitCap = 4000;
            s.supportReach = U8(std::max(1, V(0.25)));
            s.fillMap = voxel::Map_Freeze;
            s.lifetimeTicks = 600;
            s.expiryMap = voxel::Map_Unknit;
            s.rimeDepth = U8(std::max(1, V(0.25)));
            s.rimeLifetimeTicks = 480;
            s.zone.kind = ZoneKind::Root;
            s.zone.place = ZonePlace::PlumeBox;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 72;
            s.zone.radiusQ = U16(V(0.5));
            s.zone.halfHeightQ = U16(V(2.0));
            s.zone2 = s.zone;
            s.zone2.kind = ZoneKind::Chill;
            s.zone2.durationTicks = 180;
            s.zone2.strength[0] = 6;
            s.zone2.strength[1] = 60;
            s.zoneDamage = 15;
        }
        return t;
    }

    uint64_t SpellTableHash(const SpellTable& table) {
        uint64_t h = voxel::HashMix64(0x5350454cull, kSpellCount);   // "SPEL"
        h = voxel::HashMix64(h, std::bit_cast<uint32_t>(table.voxelSize));
        h = voxel::HashMix64(h, table.zoneLeadTicks);
        h = voxel::HashMix64(h, table.ablateRadiusQ);
        for (const FormDef& form : table.forms) h = voxel::HashMix64(h, form.cooldownTicks);
        for (const SpellDef& spell : table.spells) h = MixSpell(h, spell);
        for (const uint16_t cost : table.boreCost) h = voxel::HashMix64(h, cost);
        return h;
    }
}
