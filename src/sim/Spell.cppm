module;

#include <array>
#include <cstdint>

export module vortex.sim:spell;

import :zones;

export namespace vortex::sim {

    // --- the two axes (docs/design/magic.md "The two axes as data") --------------------------------
    //  A spell is a form (how it moves through the world) times an element (what it does). The
    //  numbers below are wire values: never renumber them.

    enum class Form : uint8_t { Flow = 0, Plume = 1, Pierce = 2, Burst = 3 };
    enum class Element : uint8_t { Metal = 0, Thermal = 1, Pressure = 2, Frost = 3 };
    using SpellId = uint8_t;
    /// 0..15 the matrix; 16 Kindle and 17 Pour are spread.md's drills (not built here).
    inline constexpr SpellId kSpellCount = 18;

    constexpr SpellId MakeSpell(Form f, Element e) { return static_cast<SpellId>(static_cast<uint8_t>(f) * 4 + static_cast<uint8_t>(e)); }
    constexpr Form FormOf(SpellId s) { return static_cast<Form>(s / 4); }
    constexpr Element ElementOf(SpellId s) { return static_cast<Element>(s % 4); }

    inline constexpr SpellId kUpdraft = MakeSpell(Form::Plume, Element::Pressure);       // 6
    inline constexpr SpellId kPermafrostClimb = MakeSpell(Form::Plume, Element::Frost);  // 7
    inline constexpr SpellId kDash = MakeSpell(Form::Pierce, Element::Pressure);         // 10
    inline constexpr SpellId kShatterSpike = MakeSpell(Form::Pierce, Element::Frost);    // 11
    inline constexpr SpellId kBarricadeWave = MakeSpell(Form::Burst, Element::Metal);    // 12
    inline constexpr SpellId kPunch = MakeSpell(Form::Burst, Element::Pressure);         // 14
    inline constexpr SpellId kFlashFreeze = MakeSpell(Form::Burst, Element::Frost);      // 15

    /// @brief SpellTable::boreCost of an id a bore never passes (indestructible and not a liquid).
    inline constexpr uint16_t kBoreStop = 0xFFFF;

    /// @brief Where a spell puts its zone.
    enum class ZonePlace : uint8_t {
        None = 0,
        UpdraftColumn = 1,   ///< a y-cylinder over the Plume column
        DashPath = 2,        ///< a capsule along the traced dash path
        PunchShove = 3,      ///< a capsule along the aim, from `nearQ` to `farQ` out of the eye
        HitCylinder = 4,     ///< a y-cylinder of `radiusQ` and `halfHeightQ` round the hit (Shatter Spike)
        DiscCylinder = 5,    ///< a y-cylinder of `radiusQ` and `halfHeightQ` over the Burst disc's seed
        PlumeBox = 6,        ///< the Plume's cells' box, grown by `radiusQ` (voxels) sideways and `halfHeightQ` (voxels) down
    };
    /// @brief Which players a zone acts on, seen from the caster's team.
    enum class ZoneTarget : uint8_t { Enemies = 0, Everyone = 1, Allies = 2 };

    /// @brief Per form: the cooldown both of its elements share (magic.md "Resources").
    struct FormDef {
        uint16_t cooldownTicks = 0;
    };

    /// @brief How a spell's zone is placed and what it does. Integers only; q = 1/16 voxel.
    struct ZoneDef {
        ZoneKind kind = ZoneKind::None;
        ZonePlace place = ZonePlace::None;
        ZoneTarget target = ZoneTarget::Enemies;
        uint16_t durationTicks = 0;
        int16_t strength[2] = {0, 0};   ///< cm/s: Impulse {away from the axis, up}; Shove {along, up}; Lift {target up, 0}
        uint16_t radiusQ = 0;
        uint16_t nearQ = 0;             ///< PunchShove: the capsule's start, out of the eye
        uint16_t farQ = 0;              ///< PunchShove: the capsule's end, out of the eye
        uint16_t rampQ = 0;             ///< Lift: the rim's ramp width (the strength falls to 0 over it)
        uint16_t hoverQ = 0;            ///< Lift: the ease below the top
        uint16_t halfHeightQ = 0;       ///< DiscCylinder: the cylinder's half height (PlumeBox: voxels down)
    };

    /// @brief Every number of one spell at the session's voxel size (0 where it has no use).
    struct SpellDef {
        bool built = false;
        // Burst: the strip across the aim (magic.md "Burst (Torrent): a sweep in the hit face's plane")
        uint16_t burstHalfWidth = 0;       ///< voxels
        uint16_t burstHalfThickness = 0;   ///< voxels
        uint16_t burstPops = 0;
        uint16_t burstVisitCap = 0;
        uint8_t stepMax = 0;               ///< voxels along the normal per step
        uint16_t burstPlane = 0;           ///< the box's in-plane half extent, voxels
        int16_t burstNormalMin = 0;        ///< the box along the normal, voxels
        int16_t burstNormalMax = 0;
        uint16_t burstDiscRadius = 0;      ///< voxels: a disc footprint (0: the strip)
        // Plume (the single column, or the full operator when plumeSeedRadius > 0)
        uint16_t riseMax = 0;              ///< voxels
        uint8_t plumeSeedRadius = 0;       ///< voxels: the seed disc
        uint16_t plumeRunPops = 0;
        uint16_t plumeLateral = 0;         ///< voxels from the hit column
        uint16_t plumeVisitCap = 0;
        uint8_t supportReach = 0;          ///< voxels: a Rise cell this close to support takes the fill
        // Pierce (magic.md "Pierce (Solid): a bore that pays for every voxel it adds")
        uint16_t pierceRadiusQ = 0;
        uint16_t piercePower = 0;          ///< 1/16 hardness
        uint32_t pierceBudget = 0;         ///< bore cost units (SpellTable::boreCost)
        uint16_t pierceMaxSlices = 0;
        // the caster's dash
        uint8_t dashTicks = 0;
        uint16_t dashSpeedCmS = 0;
        uint16_t dashEndPushCmS = 0;       ///< `push` along the dash on its last tick
        uint16_t dashRangeQ = 0;           ///< the server's path trace: at most this far
        uint16_t dashStepQ = 0;            ///< the path trace's step
        // the blow
        uint16_t fistRangeQ = 0;           ///< the fist point: the first hit within this of the eye
        uint16_t blowRadiusQ = 0;          ///< the blast at the fist point
        uint16_t blowPower = 0;            ///< 1/16 hardness
        // the element's verbs
        uint8_t fillMap = 0;               ///< a voxel::TurningMapId
        uint8_t rimHeight = 0;             ///< voxels, the strip cell included
        uint8_t bandCount = 0;
        uint8_t bandFirstTick = 0;         ///< after the dequeue tick
        uint8_t bandSpacingTicks = 0;
        uint16_t lifetimeTicks = 0;        ///< of the conjured matter
        uint8_t expiryMap = 0;             ///< a voxel::TurningMapId
        // Frost's verbs
        uint16_t floeLifetimeTicks = 0;    ///< Flash Freeze's floe slab on water
        uint16_t floeRadiusQ = 0;
        uint16_t rimeRadiusQ = 0;          ///< Shatter Spike's Rime capsule around the bore
        uint8_t rimeDepth = 0;             ///< Permafrost Climb's Rime box: voxels into the face
        uint16_t rimeLifetimeTicks = 0;    ///< until the Thaw
        // the Shatter flood (Punch's, at the zone tick)
        uint16_t shatterRadiusQ = 0;
        uint8_t shatterDepth = 0;          ///< voxels from an exposed face
        uint16_t shatterVisitCap = 0;
        ZoneDef zone;
        ZoneDef zone2;                     ///< a second zone of the same place (a Chill after a Root)
        ZoneDef release;                   ///< an ally Release of the same place (Punch)
        // the server only
        uint16_t directDamage = 0;
        uint16_t zoneDamage = 0;
        uint8_t maxBodies = 0;             ///< bodies woken by one cast
        uint16_t maxBodyKickCmS = 0;
        uint16_t dispelRadiusQ = 0;
    };

    /// @brief One integer table that holds every number of the spells (magic.md "sim::SpellTable").
    struct SpellTable {
        float voxelSize = 0.125f;
        std::array<FormDef, 4> forms{};
        std::array<SpellDef, kSpellCount> spells{};
        uint16_t ablateRadiusQ = 0;        ///< the absorb rule's patch (0.25 m)
        uint32_t zoneLeadTicks = 24;
        /// What a bore pays per voxel of each world id: `round(hardness * 16)`, authored in code; 0 for air
        /// and liquids, kBoreStop for the indestructible ids that are not liquids (bedrock, floe).
        std::array<uint16_t, 256> boreCost{};
    };

    /**
     * @brief The table at `voxelSize`: metres become voxels (`lround(m / vs)`) and q
     * (`lround(m / vs * 16)`) once, in double from the float voxel size, so both sides of a
     * session hold the same integers. Built: Updraft, Permafrost Climb, Dash, Shatter Spike, Barricade
     * Wave, Punch and Flash Freeze.
     */
    SpellTable BuildSpellTable(float voxelSize);
    /// @brief The bore cost of every id of the default world palette (SpellTable::boreCost).
    std::array<uint16_t, 256> BoreCostTable();
    /// @brief A hash of every field of the table.
    uint64_t SpellTableHash(const SpellTable& table);
}
