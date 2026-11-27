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
            h = voxel::HashMix64(h, s.manaCost);
            h = voxel::HashMix64(h, s.noProjectile ? 1u : 0u);
            h = voxel::HashMix64(h, s.drawsLine ? 1u : 0u);
            h = voxel::HashMix64(h, s.drawHalfWidthQ);
            h = voxel::HashMix64(h, s.drawHeight);
            h = voxel::HashMix64(h, s.drawMinQ);
            h = voxel::HashMix64(h, s.drawMaxQ);
            h = voxel::HashMix64(h, s.drawZoneRadiusQ);
            h = voxel::HashMix64(h, s.sweepStartQ);
            h = voxel::HashMix64(h, s.sweepStepQ);
            h = voxel::HashMix64(h, s.sweepSpeedMs);
            for (const uint32_t v : {uint32_t{s.flowSeedRadius}, uint32_t{s.fallMax}, uint32_t{s.flowRunPops}, uint32_t{s.flowLateral},
                                     uint32_t{s.frontCount}, uint32_t{s.flowVisitCap}, uint32_t{s.fillCap}, uint32_t{s.setDelayTicks},
                                     uint32_t{s.splashRadiusQ}, uint32_t{s.splashDamage}, uint32_t{s.ledgeSpacing}, uint32_t{s.ledgeWidth},
                                     uint32_t{s.ledgeOut}, uint32_t{s.ledgeThick}, uint32_t{s.bandVoxels}, uint32_t{s.drawsRise ? 1u : 0u},
                                     uint32_t{s.stepRise}, uint32_t{s.stepRun}, uint32_t{s.stepWidth}, uint32_t{s.deckWidth}, uint32_t{s.deckThick},
                                     uint32_t{s.stairsMinRiseQ}, uint32_t{s.bridgeLevelQ}, uint32_t{s.bridgeMinQ}, uint32_t{s.bridgeEndQ},
                                     uint32_t{s.socketSpacing}, uint32_t{s.pegLengthQ}, uint32_t{s.aimLevel ? 1u : 0u}, uint32_t{s.armTicks},
                                     uint32_t{s.burstRadiusQ}, uint32_t{s.leapMaxTicks}, uint32_t{s.heatLifetimeTicks},
                                     uint32_t{s.heatRadiusQ}, uint32_t{s.scorchDamage}, uint32_t{s.burnTicks}})
                h = voxel::HashMix64(h, v);
            h = MixZone(h, s.zone3);
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
            {m.stoneDarkRimed, 32}, {m.brickRimed, 20}, {m.ironRimed, 64}, {m.slag, 112},
            {m.knitIronHot, 56}, {m.ironHot, 64}, {m.pitch, 0},
        };
        for (const auto& [id, cost] : costs) c[id] = cost;
        // Fire stages (spread.md): a kindle stage keeps its base's hardness, a burn stage 0.6 of it.
        struct Run {
            uint8_t first;
            int count;
            uint16_t cost;
        };
        const Run fire[] = {{m.planksK1, 6, 19}, {m.planksB1, 6, 12}, {m.woodK1, 8, 24}, {m.woodB1, 8, 14}, {m.foliageK1, 3, 3}, {m.foliageB1, 2, 2}};
        for (const Run& run : fire)
            for (int i = 0; i < run.count; ++i) c[static_cast<uint8_t>(run.first + i)] = run.cost;
        c[m.charredWood] = 10;
        c[m.scorched] = 13;
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
        t.forms[static_cast<size_t>(Form::Flow)].speedMs = 20;
        t.forms[static_cast<size_t>(Form::Plume)].speedMs = 30;
        t.forms[static_cast<size_t>(Form::Pierce)].speedMs = 80;
        t.forms[static_cast<size_t>(Form::Burst)].speedMs = 40;
        t.metreQ = Q(1.0);
        t.gestureReachQ = static_cast<uint32_t>(std::lround(31.0 / vs * 16.0));
        t.gestureStepQ = Q(2.0);
        t.gesturePathQ = Q(12.0);
        t.gestureSupportQ = Q(1.0);
        t.ablateRadiusQ = Q(0.25);
        // Tap costs in 1/60 mana (magic.md "Casting: mana, gestures and projectiles"). A shape spell's tap costs as a
        // drawn shape of its usual size.
        const auto Mana = [](double mana) { return static_cast<uint16_t>(std::lround(mana * 60.0)); };
        t.boreCost = BoreCostTable();
        // Flow's arc in 1/4096 voxel: 22 m/s per tick, and gravity per tick^2 (89 at 0.125 m).
        t.arcSpeedFine = static_cast<uint32_t>(std::lround(22.0 / vs / 60.0 * 4096.0));
        t.arcGravity = static_cast<uint32_t>(std::lround(9.81 / vs * 4096.0 / 3600.0));
        // A bore's budget per metre of stone, scaled with the slices of a metre (8 at 0.125 m).
        const auto Budget = [&V](uint32_t perMetre) { return static_cast<uint32_t>(perMetre * static_cast<uint32_t>(V(1.0)) / 8u); };

        {   // Molten Pour: a lobbed Flow; slag over its Fall skin cells and its pool, set into knit after a second.
            SpellDef& s = t.spells[kMoltenPour];
            s.built = true;
            s.flowSeedRadius = U8(V(0.5));
            s.fallMax = U16(V(8.0));
            s.flowRunPops = 1200;
            s.flowLateral = U16(V(3.0));
            s.frontCount = 240;
            s.flowVisitCap = 12000;
            // The cap is a volume: 900 voxels at 0.125 m.
            s.fillCap = U16(static_cast<int32_t>(std::lround(900.0 * (0.125 / vs) * (0.125 / vs) * (0.125 / vs))));
            s.fillMap = voxel::Map_Slag;
            s.setDelayTicks = 60;
            s.lifetimeTicks = 1500;
            s.expiryMap = voxel::Map_Unknit;
            s.splashRadiusQ = Q(1.0);
            s.splashDamage = 20;
            s.manaCost = Mana(30.0);
        }
        {   // Rising Spire: the Plume filled with knit in bands, ledges every metre, a Lift that carries a rider up.
            SpellDef& s = t.spells[kRisingSpire];
            s.built = true;
            s.plumeSeedRadius = U8(V(0.375));
            s.riseMax = U16(V(6.0));
            s.plumeRunPops = 400;
            s.plumeLateral = U16(V(2.0));
            // The whole column is filled: the cap holds every seed's rise (one test per cell) and the spread's pops.
            int32_t seeds = 0;
            for (int32_t a = -s.plumeSeedRadius; a <= s.plumeSeedRadius; ++a)
                for (int32_t b = -s.plumeSeedRadius; b <= s.plumeSeedRadius; ++b) seeds += a * a + b * b <= s.plumeSeedRadius * s.plumeSeedRadius ? 1 : 0;
            s.plumeVisitCap = U16(std::min(60000, seeds * (s.riseMax + 2) + 6 * s.plumeRunPops));
            s.fillMap = voxel::Map_Knit;
            s.bandCount = 6;
            s.bandVoxels = U8(V(1.0));
            s.bandFirstTick = 18;
            s.lifetimeTicks = 900;
            s.expiryMap = voxel::Map_Unknit;
            s.ledgeSpacing = U8(V(1.0));
            s.ledgeWidth = U8(V(0.75));
            s.ledgeOut = U8(V(0.5));
            s.ledgeThick = U8(std::max(1, V(0.25)));
            s.zone.kind = ZoneKind::Lift;
            s.zone.place = ZonePlace::SpireLift;
            s.zone.target = ZoneTarget::Everyone;
            s.zone.strength[0] = 860;
            s.zone.radiusQ = Q(0.375 + 0.3);
            s.zone.rampQ = Q(0.25);
            s.zone.hoverQ = Q(0.5);
            s.manaCost = Mana(35.0);
            s.drawsRise = true;
            s.stepRise = U8(V(0.375));
            s.stepRun = U8(V(0.5));
            s.stepWidth = U8(V(1.0));
            s.deckWidth = U8(V(1.5));
            s.deckThick = U8(std::max(1, V(0.25)));
            s.stairsMinRiseQ = Q(1.0);
            s.bridgeLevelQ = Q(1.0);
            s.bridgeMinQ = Q(3.0);
            s.bridgeEndQ = Q(0.5);
            s.drawMaxQ = Q(10.0);
        }
        {   // Rebar Lance: two level bores 1 m apart up the face, knit filling both sockets and two pegs out of it.
            SpellDef& s = t.spells[kRebarLance];
            s.built = true;
            s.aimLevel = true;
            s.pierceRadiusQ = 24;   // 1.5 voxels: a bore is counted in voxels
            s.piercePower = 64;
            s.pierceMaxSlices = U16(V(3.0));
            // 1.25 m of stone at 0.125 m: 10 slices of about 7 voxels at 48.
            s.pierceBudget = Budget(3360);
            s.socketSpacing = U8(V(1.0));
            s.pegLengthQ = Q(1.25);
            s.fillMap = voxel::Map_Knit;
            s.lifetimeTicks = 1200;
            s.expiryMap = voxel::Map_Unknit;
            s.zone.kind = ZoneKind::Root;
            s.zone.place = ZonePlace::HitCylinder;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 45;
            s.zone.radiusQ = Q(0.5);
            s.zone.halfHeightQ = Q(2.0);
            s.directDamage = 30;
            s.manaCost = Mana(20.0);
        }
        {   // Rune Trap: a lobbed Flow lands a static rune; the first enemy on it bursts it: Root, Chill and an ice patch.
            SpellDef& s = t.spells[kRuneTrap];
            s.built = true;
            s.fallMax = U16(V(8.0));
            s.armTicks = 60;
            s.zone.kind = ZoneKind::Rune;
            s.zone.place = ZonePlace::RuneFloor;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 3600;
            s.zone.radiusQ = Q(1.5);
            s.zone.halfHeightQ = Q(1.0);
            s.burstRadiusQ = Q(2.0);
            s.zone2.kind = ZoneKind::Root;
            s.zone2.place = ZonePlace::RuneFloor;
            s.zone2.target = ZoneTarget::Enemies;
            s.zone2.durationTicks = 90;
            s.zone2.radiusQ = Q(2.0);
            s.zone2.halfHeightQ = Q(1.0);
            s.zone3 = s.zone2;
            s.zone3.kind = ZoneKind::Chill;
            s.zone3.durationTicks = 180;
            s.zone3.strength[0] = 6;
            s.zone3.strength[1] = 60;
            s.zoneDamage = 15;
            s.fillMap = voxel::Map_Freeze;
            s.fillCap = 400;
            s.lifetimeTicks = 720;
            s.expiryMap = voxel::Map_Unknit;
            s.manaCost = Mana(30.0);
        }

        // Thermal (magic.md "The sixteen spells"): pitch burns 3.5 s where it lands, hot metal cools after 4 s. Without fire
        // spread a pitch fill's registry expiry (Unknit) stands in for the burn stages of its fire class.
        constexpr uint16_t kPitchTicks = 210, kHeatTicks = 240;
        {   // Pitchfall: a lobbed Flow; pitch over its Fall and Run skin cells and its pool, Heat on what they touch.
            SpellDef& s = t.spells[kPitchfall];
            s.built = true;
            s.flowSeedRadius = U8(V(0.5));
            s.fallMax = U16(V(8.0));
            s.flowRunPops = 1200;
            s.flowLateral = U16(V(3.0));
            s.frontCount = 240;
            s.flowVisitCap = 12000;
            s.fillCap = U16(static_cast<int32_t>(std::lround(1600.0 * (0.125 / vs) * (0.125 / vs) * (0.125 / vs))));
            s.fillMap = voxel::Map_Pitch;
            s.lifetimeTicks = kPitchTicks;
            s.expiryMap = voxel::Map_Unknit;
            s.heatLifetimeTicks = kHeatTicks;
            s.splashRadiusQ = Q(1.0);
            s.splashDamage = 20;
            s.manaCost = Mana(30.0);
        }
        {   // Wildfire: the full Plume; pitch up its Rise and Run skin cells and its joined Front, Heat on what they touch.
            SpellDef& s = t.spells[kWildfire];
            s.built = true;
            s.plumeSeedRadius = U8(V(0.375));
            s.riseMax = U16(V(6.0));
            s.plumeRunPops = 400;
            s.plumeLateral = U16(V(2.0));
            s.plumeVisitCap = 4000;
            s.fillMap = voxel::Map_Pitch;
            s.lifetimeTicks = kPitchTicks;
            s.expiryMap = voxel::Map_Unknit;
            s.heatLifetimeTicks = kHeatTicks;
            s.zone.radiusQ = U16(V(0.5));        // the scorch: the plume's box grown sideways (voxels) ...
            s.zone.halfHeightQ = U16(V(2.0));    // ... and down (voxels)
            s.scorchDamage = 20;
            s.manaCost = Mana(30.0);
        }
        {   // Meltdown Beam: Heat round the bore, then the bore through what the Heat softened; 45 and Burning.
            SpellDef& s = t.spells[kMeltdownBeam];
            s.built = true;
            s.pierceRadiusQ = 32;   // 2 voxels: a bore is counted in voxels
            s.piercePower = 80;
            s.pierceMaxSlices = 64;
            // Three metres of stone at 0.125 m (24 slices of about 12.6 voxels at 48), scaled with the slices of a metre.
            s.pierceBudget = Budget(14400);
            s.heatRadiusQ = 64;     // 4 voxels round the bore's axis
            s.heatLifetimeTicks = kHeatTicks;
            s.directDamage = 45;
            s.burnTicks = 180;
            s.manaCost = Mana(20.0);
        }
        {   // Firestorm Sweep (the owner's rework request of 2026-11-27): a front 4 m wide and 1.5 m high travels 8 m at 8 m/s from
            // 1.5 m in front of the caster, laying a burning band of pitch per 0.5 m step (Unknit 1 s after it lands) and Heat on
            // what it touches; 15 once and Burning 3 s to the enemies it passes, and a shove along the sweep. A Line steers it.
            SpellDef& s = t.spells[kFirestormSweep];
            s.built = true;
            s.fillMap = voxel::Map_Pitch;
            s.lifetimeTicks = 60;
            s.expiryMap = voxel::Map_Unknit;
            s.heatLifetimeTicks = kHeatTicks;
            s.scorchDamage = 15;
            s.burnTicks = 180;
            s.manaCost = Mana(35.0);
            s.noProjectile = true;
            s.drawHalfWidthQ = Q(2.0);
            s.drawHeight = U8(V(1.5));
            s.drawMinQ = Q(1.5);
            s.drawMaxQ = Q(8.0);
            s.sweepStartQ = Q(1.5);
            s.sweepStepQ = Q(0.5);
            s.sweepSpeedMs = 8;
            s.zone.kind = ZoneKind::Shove;
            s.zone.place = ZonePlace::SweepHalf;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 1;
            s.zone.strength[0] = 400;
            s.zone.strength[1] = 100;
            s.zone.radiusQ = Q(2.0);
        }
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
            s.manaCost = Mana(20.0);
            s.maxBodies = 8;
            s.maxBodyKickCmS = 1200;
        }
        {   // Meteor Leap: the caster's own arc at the Spell edge; at the landing a Pin and a knockback round it, 15 damage,
            // and the Shatter flood of rimed matter and ice.
            SpellDef& s = t.spells[kMeteorLeap];
            s.built = true;
            s.fallMax = U16(V(8.0));
            s.leapMaxTicks = 240;
            s.manaCost = Mana(45.0);
            s.noProjectile = true;
            s.zone.kind = ZoneKind::Pin;
            s.zone.place = ZonePlace::LeapSlam;
            s.zone.target = ZoneTarget::Enemies;
            s.zone.durationTicks = 60;
            s.zone.strength[1] = 30;    // the tail: a pinned player stays pinned 0.5 s after the ring
            s.zone.radiusQ = Q(2.5);
            s.zone.halfHeightQ = Q(1.0);
            s.zone2 = s.zone;
            s.zone2.kind = ZoneKind::Impulse;
            s.zone2.durationTicks = 1;
            s.zone2.strength[0] = 500;
            s.zone2.strength[1] = 200;
            s.zoneDamage = 15;
            s.shatterRadiusQ = Q(1.5);
            s.shatterDepth = U8(std::max(1, V(0.5)));
            s.shatterVisitCap = 4000;
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
            s.manaCost = Mana(20.0);
            s.noProjectile = true;
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
            s.manaCost = Mana(32.5);
            s.drawsLine = true;
            s.drawHalfWidthQ = Q(0.25);
            s.drawHeight = U8(V(1.5));
            s.drawMinQ = Q(1.5);
            s.drawMaxQ = Q(10.0);
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
            s.manaCost = Mana(20.0);
            s.noProjectile = true;
        }
        {   // Shatter Spike: a bore, a Rime capsule round it and past its end, a Chill at the hit.
            SpellDef& s = t.spells[kShatterSpike];
            s.built = true;
            s.pierceRadiusQ = 24;   // 1.5 voxels: a bore is counted in voxels
            s.piercePower = 64;
            s.pierceMaxSlices = U16(V(3.0));
            // One metre of stone at 0.125 m (8 slices of about 7 voxels at 48), scaled with the slices of a metre.
            s.pierceBudget = Budget(2688);
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
            s.manaCost = Mana(20.0);
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
            s.manaCost = Mana(30.0);
            s.drawsLine = true;
            s.drawHalfWidthQ = Q(1.0);
            s.drawMinQ = Q(1.0);
            s.drawMaxQ = Q(12.0);
            s.drawZoneRadiusQ = Q(1.5);
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
            s.manaCost = Mana(30.0);
        }
        return t;
    }

    uint64_t SpellTableHash(const SpellTable& table) {
        uint64_t h = voxel::HashMix64(0x5350454cull, kSpellCount);   // "SPEL"
        h = voxel::HashMix64(h, std::bit_cast<uint32_t>(table.voxelSize));
        h = voxel::HashMix64(h, table.zoneLeadTicks);
        h = voxel::HashMix64(h, table.ablateRadiusQ);
        for (const FormDef& form : table.forms) h = voxel::HashMix64(h, form.speedMs);
        for (const uint32_t v : {uint32_t{table.metreQ}, uint32_t{table.manaPool}, uint32_t{table.manaPerTick}, uint32_t{table.spellGapTicks},
                                 uint32_t{table.drawBaseMana}, uint32_t{table.drawManaPerMetre}, uint32_t{table.maxFlightTicks},
                                 uint32_t{table.lineSpeedMs}, table.gestureReachQ, table.gestureStepQ, table.gesturePathQ,
                                 uint32_t{table.gestureSupportQ}, table.arcSpeedFine, table.arcGravity, uint32_t{table.arcMaxTicks},
                                 uint32_t{table.lobMinTicks}, uint32_t{table.hazardTicks}, uint32_t{table.hazardDamage}, uint32_t{table.pitchHpPerSecond},
                                 uint32_t{table.burnHpPerSecond}, uint32_t{table.hazardCapHpPerSecond}})
            h = voxel::HashMix64(h, v);
        for (const SpellDef& spell : table.spells) h = MixSpell(h, spell);
        for (const uint16_t cost : table.boreCost) h = voxel::HashMix64(h, cost);
        return h;
    }
}
