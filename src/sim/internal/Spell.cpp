module;

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
            h = voxel::HashMix64(h, s.riseMax);
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
            h = MixZone(h, s.zone);
            h = voxel::HashMix64(h, s.directDamage);
            h = voxel::HashMix64(h, s.zoneDamage);
            h = voxel::HashMix64(h, s.maxBodies);
            h = voxel::HashMix64(h, s.maxBodyKickCmS);
            h = voxel::HashMix64(h, s.dispelRadiusQ);
            return h;
        }
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
            s.dashTicks = 18;
            s.dashSpeedCmS = 2670;
            s.dashEndPushCmS = 400;
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
        return h;
    }
}
