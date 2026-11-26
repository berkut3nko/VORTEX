module;

#include <array>
#include <cstdint>

export module vortex.sim:spell;


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
    inline constexpr SpellId kDash = MakeSpell(Form::Pierce, Element::Pressure);         // 10
    inline constexpr SpellId kBarricadeWave = MakeSpell(Form::Burst, Element::Metal);    // 12
    inline constexpr SpellId kPunch = MakeSpell(Form::Burst, Element::Pressure);         // 14

    /// @brief What a zone does to the players in it (magic.md "sim::ZoneSet"). Wire values.
    enum class ZoneKind : uint8_t {
        None = 0, Impulse = 1, Lift = 2, Pin = 3, Chill = 4, Root = 5, Release = 6, Block = 7,
        Shove = 8,   ///< a push along the capsule's axis plus a lift, both falling to 0 at the rim
        Rune = 9,    ///< reserved: no behaviour yet
    };
    /// @brief Where a spell puts its zone.
    enum class ZonePlace : uint8_t {
        None = 0,
        UpdraftColumn = 1,   ///< a y-cylinder over the Plume column
        DashPath = 2,        ///< a capsule along the traced dash path
        PunchShove = 3,      ///< a capsule along the aim, from `nearQ` to `farQ` out of the eye
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
        // Plume (the single column)
        uint16_t riseMax = 0;              ///< voxels
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
        ZoneDef zone;
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
    };

    /**
     * @brief The table at `voxelSize`: metres become voxels (`lround(m / vs)`) and q
     * (`lround(m / vs * 16)`) once, in double from the float voxel size, so both sides of a
     * session hold the same integers. Only Updraft, Dash, Barricade Wave and Punch are built.
     */
    SpellTable BuildSpellTable(float voxelSize);
    /// @brief A hash of every field of the table.
    uint64_t SpellTableHash(const SpellTable& table);
}
