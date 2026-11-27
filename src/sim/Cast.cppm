module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

export module vortex.sim:cast;

import vortex.voxel;
import :simulation;
import :worldop;
import :blast;
import :zones;
import :player;
import :rules;
import :spell;
import :gesture;
import :propagate;
import :knit;

export namespace vortex::sim {

    // --- the cast pipeline (docs/design/magic.md "The cast pipeline") ----------------------------
    //  CastSpell plans a spell on the simulation as it is at T_run, checks the spawn guard and the
    //  caps, and only then applies it: a refused cast changes nothing. Follow-ups (bands, a Punch's
    //  blow, the zone ticks) and the expiries of conjured matter run in RunScheduled on their ticks.
    //  Voxel changes are integer WorldOps and zones are integers; the hit tests on players and
    //  bodies are server decisions and use floats.

    /// @brief Why a spell was refused. A refusal keeps the gap spent and costs no mana.
    enum class SpellRefusal : uint8_t {
        None = 0,
        NotBuilt,          ///< not a spell of the table, or not built
        TooSoon,           ///< inside the gap after the caster's last spell
        NotAttuned,        ///< the element is not one of the caster's two
        Missed,            ///< no hit within the range (or a Dash with no horizontal aim)
        Outside,           ///< the hit is outside the operator's walls
        TooMuchConjured,   ///< the region holds kMaxConjuredPerRegion conjured voxels
        TooManyZones,      ///< the region's or the caster's live or pending zones are at their cap
        NearSpawn,         ///< an op box or a zone box meets an enemy spawn area
        Rooted,            ///< a Dash while the caster is rooted or pinned
        NoMana,            ///< the cost is over the caster's mana at T_run
        BadGesture,        ///< the gesture failed the server's checks, or its form is not the spell's
    };
    const char* ToString(SpellRefusal refusal);

    /// @brief One captured cast of a spell.
    struct SpellCast {
        uint8_t player = 0;                   ///< slot + 1; 0 = a script (no player)
        TeamId team = kNoTeam;
        SpellId spell = 0;
        uint32_t inputTick = 0;
        glm::dvec3 eye{0.0};                  ///< metres: the eye at the input's tick
        glm::vec3 dir{0.0f, 0.0f, -1.0f};     ///< the look direction
        /// The caster's state: a Dash starts its move in it (StartDash). Null where the caster's own
        /// step starts the dash at the cast edge, or for a script with no player.
        PlayerState* casterState = nullptr;
        /// The gesture of the cast (no points: a tap along `dir`). The server checks it (CheckGesture) and its form
        /// must be the spell's (BadGesture).
        SpellGesture gesture;
    };

    /// @brief A living player at the tick of the call: the server's hit tests and damage use it.
    struct SpellTarget {
        uint8_t player = 0;                   ///< slot + 1
        TeamId team = kNoTeam;
        glm::vec3 feet{0.0f};
    };
    /// @brief Damage for the caller to apply through its damage path.
    struct SpellDamage {
        uint8_t player = 0;                   ///< slot + 1
        uint16_t amount = 0;
        uint8_t source = 0;                   ///< the caster (slot + 1; 0 a script)
        SpellId spell = 0;
        uint8_t cause = 1;                    ///< a sim::DamageCause: Blast (1), or Lava (3) for the hazard scan's slag
    };
    /// @brief A team's spawn area: the voxel box [min, max) (magic.md "Readability rules" 10).
    struct SpawnArea {
        TeamId team = kNoTeam;
        glm::ivec3 min{0}, max{0};
    };
    /// @brief A caster's mana, its gap and its two attuned elements (Metal and Pressure by default). The mana is
    /// `mana` at `manaTick`, plus SpellTable::manaPerTick each tick since, up to the pool (ManaAt): a new caster is full.
    struct CasterState {
        uint16_t mana = 0xFFFF;
        uint32_t manaTick = 0;
        uint32_t spellReadyTick = 0;          ///< no Spell edge is captured before this tick (the gap)
        uint8_t elementA = static_cast<uint8_t>(Element::Metal);
        uint8_t elementB = static_cast<uint8_t>(Element::Pressure);
    };
    /// @brief Who started a zone (server state, never in the zone).
    struct ZoneOwner {
        TeamId casterTeam = kNoTeam;
        uint8_t caster = 0;                   ///< slot + 1; 0 a script
        bool dispellable = false;             ///< a kind that lasts (Updraft's Lift, a Chill, a rune): a Punch ends it
        bool rune = false;                    ///< a Rune Trap's rune: the trigger test in RunScheduled reads it
        glm::ivec3 runeFloor{0};              ///< the voxel the rune lies on: when it turns to air, the rune breaks
    };

    enum class ScheduledKind : uint8_t {
        Band = 0,       ///< a band of conjured matter: its MapMask, less the cells players and bodies take at its tick
        ZoneTick = 1,   ///< a zone's start tick: its damage and its body impulses
        Blow = 2,       ///< a Punch's blast at the fist point
        Shatter = 3,    ///< the Shatter flood in `op.sphere` (its radius) at the zone tick: one MapMask(Shatter)
        Op = 4,         ///< an op planned at T_run, applied on its arrival (recorded under `owner` when `hasOwner`)
        Bore = 5,       ///< a bore planned at T_run (`site`), carved with its island search on its arrival
        Hit = 6,        ///< a projectile's direct damage to `target` on its arrival
        Splash = 7,     ///< `damage` to the enemies within `op.sphere` (its centre and radius, q) on its arrival
        Scorch = 8,     ///< `damage` to the enemies whose feet lie in `op.shape` (a box, a cylinder or a capsule), and the spell's Burning
    };
    /// @brief A follow-up, run by RunScheduled in (tick, seq) order.
    struct ScheduledAction {
        uint32_t tick = 0;
        uint32_t seq = 0;
        ScheduledKind kind = ScheduledKind::Band;
        uint32_t owner = 0;                   ///< Band: the registry owner
        bool hasOwner = true;                 ///< Op: record the op under `owner`
        WorldOp op;                           ///< Band: its MapMask; Blow: its CarveSphere
        uint16_t zoneId = 0;                  ///< ZoneTick
        uint16_t damage = 0;                  ///< ZoneTick: to the targets inside the zone at its tick
        SpellId spell = 0;
        TeamId casterTeam = kNoTeam;
        uint8_t caster = 0;
        bool hasHitVoxel = false;             ///< Blow: the fist met terrain at `hitVoxel` (the absorb rule reads it)
        glm::ivec3 hitVoxel{0};
        BlastSite site;                       ///< Bore
        uint8_t target = 0;                   ///< Hit: slot + 1
    };

    /// @brief A player on fire (server only): Burning deals SpellTable::burnHpPerSecond until `untilTick`.
    struct Burning {
        uint32_t untilTick = 0;
        uint8_t source = 0;                   ///< the caster (slot + 1; 0 a script)
        TeamId sourceTeam = kNoTeam;
        SpellId spell = 0;
    };

    /// @brief The server-only state of spells, owned by whoever casts (the offline game, a server).
    struct CastContext {
        KnitRegistry registry;
        StructureCredit structure;
        std::vector<ScheduledAction> schedule;          ///< sorted by (tick, seq)
        uint32_t nextSeq = 1;
        uint16_t nextZoneId = 1;                        ///< wraps to 1, never 0
        std::map<uint16_t, ZoneOwner> zoneOwners;
        std::map<uint8_t, CasterState> casters;
        std::vector<SpawnArea> spawns;
        OperatorWalls walls;                            ///< WindowWalls offline; a server's play bounds
        PlayerTuning tuning;                            ///< the capsule, the eye height and castRange
        uint32_t maxZones = 32;                         ///< live or pending in the region
        uint32_t maxZonesPerPlayer = 4;                 ///< live or pending per player (not for scripts)
        uint32_t hotUntil = 0;                          ///< the hazard scan runs up to this tick (slag, pitch or hot metal may lie until then)
        std::map<uint8_t, Burning> burning;             ///< by player (slot + 1); ends at its tick or when the player is not a target
    };

    /// @brief Counters of one cast (logged; `visits` and `chunksRead` are charged as work).
    struct SpellStats {
        uint32_t visits = 0;
        uint32_t chunksRead = 0;
        uint32_t planned = 0;                 ///< conjured voxels planned (Barricade Wave, the ice of Frost)
        uint32_t ops = 0;                     ///< ops applied now and scheduled
        uint32_t bytes = 0;                   ///< wire bytes of the ops, the zone starts, the cancels and the cast's fx
        uint32_t path = 0;                    ///< operator `Path` cells (Updraft: the rise; Dash: the kept steps; Shatter Spike: the paid voxels)
        uint32_t front = 0;                   ///< operator `Front` cells (Updraft: the column's top; Barricade Wave: the strip)
        uint32_t boreSlices = 0;              ///< Shatter Spike: the slices the bore paid for
        uint32_t released = 0;                ///< Detach operations of the bore's island search
        uint32_t dashSteps = 0;               ///< Dash: path steps the trace kept
        uint32_t drawnLengthQ = 0;            ///< a drawn shape: its length after the 1.5-10 m rule (q)
        uint32_t steps = 0;                   ///< Rising Spire's stairs: the steps
        uint8_t riseShape = 0;                ///< a RiseKind: what Rising Spire made
        uint32_t sockets = 0;                 ///< Rebar Lance: the sockets bored (pegs set)
        uint32_t arcTicks = 0;                ///< a Flow spell: the ticks of its arc to the hit
        uint32_t heated = 0;                  ///< Thermal: voxels its Heat op will turn (Meltdown Beam: the capsule's metal, ice and rime)
    };

    struct CastSpellResult {
        SpellRefusal refusal = SpellRefusal::None;
        uint32_t runTick = 0;                 ///< T_run
        uint32_t zoneTick = 0;                ///< T_z = T_run + max(the flight, the zone lead)
        uint8_t flightTicks = 0;              ///< the projectile's flight: its effects land at T_run + flightTicks
        uint16_t manaCost = 0;                ///< charged at T_run (1/60 mana); 0 for a script
        /// The gesture's points for CastFx when the spell drew a shape along them (else empty).
        std::vector<glm::ivec3> drawnPointsQ;
        glm::ivec3 hitQ{0};                   ///< the hit (Punch: the fist point; Dash: the path's end), q
        uint8_t face = 0;                     ///< the hit face: axis * 2 + (sign < 0)
        bool dashStarted = false;             ///< the caster's own move (a Dash, a Meteor Leap) started in SpellCast::casterState
        std::vector<BlastResult> groups;      ///< ops applied now (no flight), one group each, in order
        std::vector<Zone> zonesStarted;
        std::vector<std::pair<uint16_t, uint32_t>> zonesCancelled;   ///< (zoneId, endTick)
        std::vector<SpellDamage> damage;
        SpellStats stats;
        CastWork work;
        double ms = 0.0;                      ///< wall time, logged only
    };

    struct ScheduledResult {
        std::vector<BlastResult> groups;      ///< bands, blows and expiries applied, then the site checks that ran
        std::vector<SpellDamage> damage;
        uint32_t sitesRun = 0;
        uint32_t bodiesPushed = 0;
        std::vector<Zone> zonesStarted;                                  ///< a rune's burst: its Root and Chill
        std::vector<std::pair<uint16_t, uint32_t>> zonesCancelled;       ///< (zoneId, endTick): a rune that burst or broke
        std::vector<std::pair<uint16_t, uint8_t>> runeBursts;            ///< (the rune's zoneId, the player under it)
        uint32_t runesBroken = 0;
        uint32_t hazardHits = 0;              ///< the hazard scan's damage entries (Burning included)
    };

    /// @brief What Rising Spire's Rise gesture makes (magic.md "Casting: mana, gestures and projectiles").
    enum class RiseKind : uint8_t { None = 0, Stairs = 1, Bridge = 2, Pillar = 3 };
    struct RiseShape {
        RiseKind kind = RiseKind::None;
        uint32_t lengthQ = 0;                 ///< the stairs' or the deck's length (q), at most drawMaxQ
        uint32_t steps = 0;                   ///< the stairs' steps
    };
    /**
     * @brief The shape of a Rise gesture from its first point S to its last point E, from the points only: stairs when E
     * is stairsMinRiseQ or more above S (ceil(rise / stepRise) steps of stepRun, cut to drawMaxQ along their slope);
     * a bridge when E is within bridgeLevelQ of S's height and bridgeMinQ or more away horizontally (the deck cut to
     * drawMaxQ); else the pillar at S. None for a spell that draws no rise, another gesture or fewer than 2 points.
     * CastSpell also makes a bridge a pillar when one of its ends lies over nothing.
     */
    RiseShape RiseShapeOf(const SpellTable& table, SpellId spell, const SpellGesture& gesture);

    /// @brief The zone lead's start tick of a cast at `runTick`.
    uint32_t ZoneStartTick(const SpellTable& table, uint32_t runTick);

    /**
     * @brief The capture checks of a cast at `tick` (changes nothing): NotBuilt, TooSoon (the gap). Every
     * element is open to every caster (the owner's decision of 2026-11-27); NotAttuned is no longer given. On None the caller calls
     * TakeCapture (the gap is never given back).
     */
    SpellRefusal CheckCapture(const CastContext& ctx, const SpellTable& table, const SpellCast& cast, uint32_t tick);
    /// @brief Starts the gap of a capture at `tick`: no Spell edge is captured before tick + spellGapTicks.
    void TakeCapture(CastContext& ctx, const SpellTable& table, const SpellCast& cast, uint32_t tick);

    // --- mana (magic.md "Casting: mana, gestures and projectiles"): server only, offline the same code ---

    /// @brief The caster's mana at `tick` (tick >= its manaTick): its mana plus manaPerTick per tick, at most the pool.
    uint16_t ManaAt(const SpellTable& table, const CasterState& caster, uint32_t tick);
    /// @brief A full pool from `tick` on (a spawn, a respawn).
    void RefillMana(const SpellTable& table, CasterState& caster, uint32_t tick);
    /// @brief The length of the shape a Line draws for `spell` (q): the gesture's path within the spell's
    /// [drawMinQ, drawMaxQ]; 0 when the spell draws no line or the gesture is not a Line of two points or more.
    uint32_t DrawnLengthQ(const SpellTable& table, SpellId spell, const SpellGesture& gesture);
    /// @brief The mana a cast costs (1/60 mana): a drawn shape drawBaseMana + drawManaPerMetre per metre of
    /// DrawnLengthQ (rounded down), or of a Rise's stairs or bridge (RiseShapeOf), else the spell's tap cost.
    uint32_t SpellManaCost(const SpellTable& table, SpellId spell, const SpellGesture& gesture);

    /**
     * @brief The server's checks of a gesture's points (BadGesture when one fails): at most kMaxGesturePoints,
     * each within gestureReachQ of `eyeQ`, at most gestureStepQ from the last, the path at most
     * gesturePathQ; every point of a Line and the first point of a Rise within gestureSupportQ of a voxel
     * that is not air (the operator's walls count). A gesture with no points passes.
     * @param chunksRead Chunks the support test read (charged as the cast's work).
     */
    SpellRefusal CheckGesture(const voxel::World& world, const OperatorWalls& walls, const SpellTable& table, const glm::ivec3& eyeQ,
                              const SpellGesture& gesture, uint32_t* chunksRead = nullptr);

    /**
     * @brief The projectile's flight in ticks from `fromQ` to `toQ` (q) at `speedMs`:
     * ceil(dist * 60 / speed), dist the floored quantised distance (horizontal only for `horizontal`), at most
     * maxFlightTicks; 0 for a speed of 0.
     */
    uint8_t FlightTicks(const SpellTable& table, const glm::ivec3& fromQ, const glm::ivec3& toQ, uint32_t speedMs, bool horizontal = false);

    /**
     * @brief Casts a spell at T_run = `simulation.Tick()`: checks its gesture, plans it, checks the spawn guard,
     * the caps and the caster's mana (charged then; a script pays none), then applies what lands now and schedules
     * what lands later (docs/design/magic.md "The cast pipeline", "Casting: mana, gestures and projectiles").
     * @details The projectile: every effect lands at T_run + flightTicks (FlightTicks from the eye to the hit at the
     * form's speed; none for Punch and Dash) and every zone starts at max(that, T_run + the lead). A gesture with
     * points: its form must be the spell's and it must pass CheckGesture (BadGesture). A Line draws Barricade Wave
     * (a knit wall drawHeight high and 2 x drawHalfWidthQ thick on the ground under the line) and Flash Freeze (an
     * ice strip, floe over water, Root and Chill in a capsule from its first to its last point) along the polyline
     * cut to [drawMinQ, drawMaxQ]; segment s lands at its distance along the line at lineSpeedMs after the flight to
     * the first point. A Line points Punch's shove along its last segment. Any other spell with points casts at
     * the release point (a Dash keeps its look).
     * @details Updraft: a Lift y-cylinder over the Plume column from the air cell in front of the
     * hit, everyone. Dash: StartDash in `cast.casterState`, a capsule trace of the path from the
     * feet (at most dashRangeQ, stopped before the first step that meets a collider), an Impulse
     * capsule along it for enemies. Punch: the fist point (the first terrain or enemy capsule within
     * fistRangeQ of the eye, else that far out), a blow at T_z, a Shove capsule for enemies at T_z,
     * and the dispel of enemy dispellable zones live at T_z whose centre (a capsule's segment, a
     * cylinder's axis) is within dispelRadiusQ of the fist, an ally Release of the Shove's capsule and the Shatter flood
     * round the fist at T_z. Shatter Spike: a Pierce bore (its island search and load check run on its arrival, paid from the structure credit, or
     * now on the cast's work when it has no flight), a Rime capsule from the hit to the bore's end (Thaw at its lifetime), a Chill round the hit; a
     * direct hit on an enemy in front of the terrain bores nothing. Flash Freeze: a Burst disc; water under the hit
     * becomes a floe slab (FreezeWater over a y-cylinder, two voxels deep), the disc's Path cells over dry ground an
     * ice film (Freeze); Root, then Chill over the disc. Permafrost Climb: the full Plume; ice on its Rise cells
     * near support and its Front cells that are skin or join the ice, a Rime box over the face; Root, then Chill
     * over the plume's box. Molten Pour and Rune Trap: the integer arc (TraceArc; a lob to a gesture's release
     * point, else launched along the aim) gives the hit and the flight. Molten Pour: the Flow operator; slag over its
     * Fall skin cells and its pool, lowest key first (fillCap), Set into knit setDelayTicks after the landing, a splash
     * round the landing; the hazard scan burns while slag lies. Rune Trap: the single seed's fall gives the landing; a
     * Rune zone on its floor, armed armTicks after the cast; the caster's older rune ends now. Rising Spire: the
     * Plume filled with knit in bands, ledges on the caster's side (a floor hit), a Lift over the column whose
     * floor each band waits for (the band timing rule), ending a lead after the last band; a Rise gesture makes
     * stairs or a bridge instead (RiseShapeOf). Rebar Lance: two level bores a socketSpacing apart up the face and one
     * knit band over both sockets and their pegs, a Root at the hit; a direct hit on an enemy bores nothing.
     * Meteor Leap: the launch of LeapPushOf along `dir` from the feet under the eye (StartLeap in `cast.casterState` when
     * one is given; else the Spell edge's step already started it and the eye is after that step), traced with TraceArc
     * to its landing (the floor under the cell it meets), then at max(the landing tick, T_run + the lead) an Impulse
     * and a Pin y-cylinder round the landing for enemies, its damage with the Pin, and the Shatter flood there.
     * Thermal (pitch expires with Unknit after lifetimeTicks, or with SimulationConfig::spread burns out by the fire rule
     * and lights timber; Heat's metal cools after heatLifetimeTicks): Pitchfall, a lob as Molten Pour, pitch over the Fall and Run skin cells and the
     * pool, Heat over the support voxels they touch that Heat changes, a splash round the landing. Wildfire: the full
     * Plume (a Rise gesture: from S up to E's height), pitch over its Rise and Run skin cells and the Front cells that
     * are skin or join it, Heat on what they touch, a scorch over the plume's box on the arrival. Meltdown Beam: a bore
     * that reads metal through Heat, preceded on its arrival by a Heat capsule of heatRadiusQ round it; a direct hit on
     * an enemy in front of the terrain deals directDamage and Burning. Firestorm Sweep: a Burst disc whose leading
     * half-rim is extruded rimHeight up into pitch, Heat under the sweep, a scorch over the disc at T_z with Burning;
     * a Line draws the curtain on the ground under the polyline, its scorch a capsule from its first to its last point.
     * Barricade Wave: a Burst strip extruded into knit bands at their ticks; no band cell
     * in a player's capsule grown by one voxel, a rigid body's box or a spawn area (a cell in an
     * enemy's refuses the cast). Work: operator visits and chunks read, and every op now or
     * scheduled, charged at T_run.
     * Zones that act on players are placed in feet space: the membership test reads the quantised
     * feet, so an aim line is moved down by the eye height.
     * @param targets The living players at T_run.
     */
    CastSpellResult CastSpell(Simulation& simulation, const SpellTable& table, const SpellCast& cast, CastContext& ctx,
                              std::span<const SpellTarget> targets);

    /**
     * @brief The follow-ups due at `tick` in (tick, seq) order, the runes (one whose floor voxel turned to air or holds a
     * film over it breaks;
     * the first target of the rune's teams whose feet lie in it bursts it: the rune ends at `tick`, Root and Chill round it
     * and the damage start a lead later, an ice band on the floor's skin lands then), the hazard scan (every hazardTicks
     * while slag, pitch or hot metal may lie: the strongest of pitch and a Hot id within two voxels of a capsule, and
     * Burning, at most hazardCapHpPerSecond in all), then the registry's expiries, the site
     * checks on the structure credit (it refills once per call: call it once per tick, after the
     * players' step of that tick) and the pruning of ended zones. A blow's island search is not
     * known at T_run: its work is paid from the structure credit.
     * @param targets The living players at `tick`.
     */
    ScheduledResult RunScheduled(Simulation& simulation, const SpellTable& table, CastContext& ctx, uint32_t tick,
                                 std::span<const SpellTarget> targets);

    /// @brief The work of a cast as CastSpell charged it.
    CastWork CastWorkOf(const CastSpellResult& spell);

    /**
     * @brief Spawn areas from team spawn points (team, feet in metres): a box 6 m around each in x and
     * z (V(6) voxels per side), over the window's rows.
     */
    std::vector<SpawnArea> SpawnAreasOf(std::span<const std::pair<TeamId, glm::dvec3>> spawns, const voxel::World& world);

    /// @brief The team bits of a zone that targets `target` as seen from `casterTeam`: bit t acts on team t.
    uint8_t ZoneTeamMask(ZoneTarget target, TeamId casterTeam);
    /// @brief The zone membership of feet at `feetQ` (q): inside its shape, as StepPlayer tests it.
    bool ZoneHoldsFeet(const Zone& zone, const glm::ivec3& feetQ);
}
