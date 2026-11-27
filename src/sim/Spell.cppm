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

    inline constexpr SpellId kMoltenPour = MakeSpell(Form::Flow, Element::Metal);         // 0
    inline constexpr SpellId kPitchfall = MakeSpell(Form::Flow, Element::Thermal);         // 1
    inline constexpr SpellId kMeteorLeap = MakeSpell(Form::Flow, Element::Pressure);       // 2
    inline constexpr SpellId kRuneTrap = MakeSpell(Form::Flow, Element::Frost);           // 3
    inline constexpr SpellId kRisingSpire = MakeSpell(Form::Plume, Element::Metal);       // 4
    inline constexpr SpellId kWildfire = MakeSpell(Form::Plume, Element::Thermal);       // 5
    inline constexpr SpellId kUpdraft = MakeSpell(Form::Plume, Element::Pressure);       // 6
    inline constexpr SpellId kPermafrostClimb = MakeSpell(Form::Plume, Element::Frost);  // 7
    inline constexpr SpellId kRebarLance = MakeSpell(Form::Pierce, Element::Metal);      // 8
    inline constexpr SpellId kMeltdownBeam = MakeSpell(Form::Pierce, Element::Thermal);  // 9
    inline constexpr SpellId kDash = MakeSpell(Form::Pierce, Element::Pressure);         // 10
    inline constexpr SpellId kShatterSpike = MakeSpell(Form::Pierce, Element::Frost);    // 11
    inline constexpr SpellId kBarricadeWave = MakeSpell(Form::Burst, Element::Metal);    // 12
    inline constexpr SpellId kFirestormSweep = MakeSpell(Form::Burst, Element::Thermal); // 13
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
        SpireLift = 7,       ///< a y-cylinder of `radiusQ` over the spire's column, up to `hoverQ` over its top (Rising Spire)
        RuneFloor = 8,       ///< a y-cylinder of `radiusQ` and `halfHeightQ` standing on the landing's floor (Rune Trap)
        LeapSlam = 9,        ///< a y-cylinder of `radiusQ` and `halfHeightQ` standing on the floor where the leap lands (Meteor Leap)
        SweepHalf = 10,      ///< a capsule of `radiusQ` over one half of Firestorm Sweep's path, started as the front reaches its middle
    };
    /// @brief Which players a zone acts on, seen from the caster's team.
    enum class ZoneTarget : uint8_t { Enemies = 0, Everyone = 1, Allies = 2 };

    /// @brief Per form: its projectile's speed (magic.md "Casting: mana, gestures and projectiles").
    struct FormDef {
        uint8_t speedMs = 0;   ///< m/s from the hand to the target (Flow: horizontal, over a parabola)
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
        // Flow (magic.md "Flow (Liquid): down, then lowest-first")
        uint8_t flowSeedRadius = 0;        ///< voxels: the seed disc (0: the single seed under the hit)
        uint16_t fallMax = 0;              ///< voxels a column falls at most
        uint16_t flowRunPops = 0;
        uint16_t flowLateral = 0;          ///< voxels from the hit column in x and z
        uint16_t frontCount = 0;           ///< the lowest popped cells: the pool
        uint16_t flowVisitCap = 0;
        uint16_t fillCap = 0;              ///< voxels a Flow fill (slag, the rune's ice) takes at most, lowest key first
        uint8_t setDelayTicks = 0;         ///< Molten Pour: the slag sets into knit this long after it lands
        uint16_t splashRadiusQ = 0;        ///< Molten Pour: enemies within this of the landing take splashDamage
        uint16_t splashDamage = 0;
        // Rising Spire (magic.md spell 2): ledges every `ledgeSpacing` voxels of height on the side facing the caster
        uint8_t ledgeSpacing = 0;
        uint8_t ledgeWidth = 0;            ///< voxels across
        uint8_t ledgeOut = 0;              ///< voxels out of the column
        uint8_t ledgeThick = 0;            ///< voxels
        uint8_t bandVoxels = 0;            ///< voxels of height per band (band k: [k, k + 1) of them over the seed)
        // Rising Spire's Rise gesture: stairs or a bridge (magic.md "Casting: mana, gestures and projectiles")
        bool drawsRise = false;
        uint8_t stepRise = 0, stepRun = 0, stepWidth = 0;    ///< voxels
        uint8_t deckWidth = 0, deckThick = 0;                ///< voxels
        uint16_t stairsMinRiseQ = 0;       ///< E at least this above S: stairs
        uint16_t bridgeLevelQ = 0;         ///< E within this of S's height ...
        uint16_t bridgeMinQ = 0;           ///< ... and at least this far: a bridge
        uint16_t bridgeEndQ = 0;           ///< each end of a bridge this close to a solid voxel
        // Rebar Lance (magic.md spell 3): two sockets along the face and their pegs
        uint8_t socketSpacing = 0;         ///< voxels from the lower socket to the upper one
        uint16_t pegLengthQ = 0;           ///< the pegs reach this far out of the face
        bool aimLevel = false;             ///< the bore's aim is made horizontal (yaw only)
        // Rune Trap (magic.md spell 13)
        uint16_t armTicks = 0;             ///< the rune arms this long after the cast
        uint16_t burstRadiusQ = 0;         ///< the burst's zones and its ice round the rune
        // Meteor Leap (magic.md spell 9): the caster's own arc, traced at most this many ticks
        uint16_t leapMaxTicks = 0;
        // Thermal's verbs (magic.md "The sixteen spells"): Heat on the matter a spell touches (Cool after
        // heatLifetimeTicks), Meltdown Beam's Heat capsule round its bore, the damage of a scorch (Wildfire's plume,
        // Firestorm Sweep's disc or curtain) and the Burning a hit starts (server only)
        uint16_t heatLifetimeTicks = 0;
        uint16_t heatRadiusQ = 0;
        uint16_t scorchDamage = 0;
        uint16_t burnTicks = 0;
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
        ZoneDef zone3;                     ///< a third zone (Rune Trap's burst: zone2 the Root, zone3 the Chill)
        // the server only
        uint16_t directDamage = 0;
        uint16_t zoneDamage = 0;
        uint8_t maxBodies = 0;             ///< bodies woken by one cast
        uint16_t maxBodyKickCmS = 0;
        uint16_t dispelRadiusQ = 0;
        // casting (magic.md "Casting: mana, gestures and projectiles")
        uint16_t manaCost = 0;             ///< 1/60 mana: the cost of a tap (a drawn shape costs by its length)
        bool noProjectile = false;         ///< the caster is the projectile (Punch, Dash): no flight
        bool drawsLine = false;            ///< a Line gesture draws the shape along its polyline
        uint16_t drawHalfWidthQ = 0;       ///< the drawn shape across the line (the wall's half thickness, the ice strip's half width)
        uint8_t drawHeight = 0;            ///< voxels of a drawn wall above the ground (0: one cell on the ground)
        uint16_t drawMinQ = 0;             ///< the drawn shape's length: at least this (the last segment is extended)
        uint16_t drawMaxQ = 0;             ///< and at most this (the polyline is cut)
        uint16_t drawZoneRadiusQ = 0;      ///< the zones of a drawn shape: a capsule from its first to its last point
        // Firestorm Sweep's front (magic.md spell 8): it starts sweepStartQ in front of the feet and moves in steps of
        // sweepStepQ at sweepSpeedMs; drawHalfWidthQ, drawHeight and drawMaxQ give its width, height and travel.
        uint16_t sweepStartQ = 0;
        uint16_t sweepStepQ = 0;
        uint8_t sweepSpeedMs = 0;
    };

    /// @brief One integer table that holds every number of the spells (magic.md "sim::SpellTable").
    struct SpellTable {
        float voxelSize = 0.125f;
        std::array<FormDef, 4> forms{};
        std::array<SpellDef, kSpellCount> spells{};
        uint16_t ablateRadiusQ = 0;        ///< the absorb rule's patch (0.25 m)
        uint32_t zoneLeadTicks = 24;
        /// q per metre, rounded (lengths and flights are integers on the quantised points).
        uint16_t metreQ = 128;
        // Mana (server only; 1/60 mana units) and the gap between spells.
        uint16_t manaPool = 6000;
        uint16_t manaPerTick = 10;
        uint16_t spellGapTicks = 12;
        uint16_t drawBaseMana = 600;       ///< a drawn shape: this plus drawManaPerMetre per metre of its length
        uint16_t drawManaPerMetre = 300;
        // Projectiles.
        uint8_t maxFlightTicks = 60;
        uint8_t lineSpeedMs = 20;          ///< a drawn shape grows along its line at this speed
        // Flow's arc (magic.md "Inputs every operator shares"): 1/4096 voxel units.
        uint32_t arcSpeedFine = 0;         ///< a tap's launch speed per tick (22 m/s)
        uint32_t arcGravity = 0;           ///< per tick^2: round(9.81 / voxelSize * 4096 / 3600)
        uint16_t arcMaxTicks = 150;        ///< the trace stops after this many ticks
        uint8_t lobMinTicks = 6;           ///< a lob to a gesture's release point flies at least this long
        // The server's hazard scan (magic.md "Damage and hazards"): every hazardTicks, hazardDamage per Hot contact.
        uint8_t hazardTicks = 6;
        uint8_t hazardDamage = 1;
        // Pitch under or beside a player, and Burning (server only); the hazard scan deals at most hazardCapHpPerSecond in all.
        uint8_t pitchHpPerSecond = 15;
        uint8_t burnHpPerSecond = 8;
        uint8_t hazardCapHpPerSecond = 15;
        // The server's gesture checks (q).
        uint32_t gestureReachQ = 0;        ///< each point within this of the eye
        uint32_t gestureStepQ = 0;         ///< each point at most this from the last
        uint32_t gesturePathQ = 0;         ///< the path at most this long
        uint16_t gestureSupportQ = 0;      ///< a Line point and a Rise's start this close to a solid voxel
        /// What a bore pays per voxel of each world id: `round(hardness * 16)`, authored in code; 0 for air
        /// and liquids, kBoreStop for the indestructible ids that are not liquids (bedrock, floe).
        std::array<uint16_t, 256> boreCost{};
    };

    /**
     * @brief The table at `voxelSize`: metres become voxels (`lround(m / vs)`) and q
     * (`lround(m / vs * 16)`) once, in double from the float voxel size, so both sides of a
     * session hold the same integers. Built: all sixteen spells of the matrix (Thermal without fire spread).
     */
    SpellTable BuildSpellTable(float voxelSize);
    /// @brief The bore cost of every id of the default world palette (SpellTable::boreCost).
    std::array<uint16_t, 256> BoreCostTable();
    /// @brief A hash of every field of the table.
    uint64_t SpellTableHash(const SpellTable& table);
}
