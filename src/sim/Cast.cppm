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
import :propagate;
import :knit;

export namespace vortex::sim {

    // --- the cast pipeline (docs/design/magic.md "The cast pipeline") ----------------------------
    //  CastSpell plans a spell on the simulation as it is at T_run, checks the spawn guard and the
    //  caps, and only then applies it: a refused cast changes nothing. Follow-ups (bands, a Punch's
    //  blow, the zone ticks) and the expiries of conjured matter run in RunScheduled on their ticks.
    //  Voxel changes are integer WorldOps and zones are integers; the hit tests on players and
    //  bodies are server decisions and use floats.

    /// @brief Why a spell was refused. A refusal keeps the charge and the form cooldown spent.
    enum class SpellRefusal : uint8_t {
        None = 0,
        NotBuilt,          ///< not a spell of the table, or not built
        FormCooling,       ///< the form's cooldown runs
        NotAttuned,        ///< the element is not one of the caster's two
        Missed,            ///< no hit within the range (or a Dash with no horizontal aim)
        Outside,           ///< the hit is outside the operator's walls
        TooMuchConjured,   ///< the region holds kMaxConjuredPerRegion conjured voxels
        TooManyZones,      ///< the region's or the caster's live or pending zones are at their cap
        NearSpawn,         ///< an op box or a zone box meets an enemy spawn area
        Rooted,            ///< a Dash while the caster is rooted or pinned
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
    };
    /// @brief A team's spawn area: the voxel box [min, max) (magic.md "Readability rules" 10).
    struct SpawnArea {
        TeamId team = kNoTeam;
        glm::ivec3 min{0}, max{0};
    };
    /// @brief A caster's form cooldowns and its two attuned elements (Metal and Pressure by default).
    struct CasterState {
        std::array<uint32_t, 4> formReadyTick{};
        uint8_t elementA = static_cast<uint8_t>(Element::Metal);
        uint8_t elementB = static_cast<uint8_t>(Element::Pressure);
    };
    /// @brief Who started a zone (server state, never in the zone).
    struct ZoneOwner {
        TeamId casterTeam = kNoTeam;
        uint8_t caster = 0;                   ///< slot + 1; 0 a script
        bool dispellable = false;             ///< a kind that lasts (Updraft's Lift): a Punch ends it
    };

    enum class ScheduledKind : uint8_t {
        Band = 0,       ///< a band of conjured matter: its MapMask, less the cells players and bodies take at its tick
        ZoneTick = 1,   ///< a zone's start tick: its damage and its body impulses
        Blow = 2,       ///< a Punch's blast at the fist point
    };
    /// @brief A follow-up, run by RunScheduled in (tick, seq) order.
    struct ScheduledAction {
        uint32_t tick = 0;
        uint32_t seq = 0;
        ScheduledKind kind = ScheduledKind::Band;
        uint32_t owner = 0;                   ///< Band: the registry owner
        WorldOp op;                           ///< Band: its MapMask; Blow: its CarveSphere
        uint16_t zoneId = 0;                  ///< ZoneTick
        SpellId spell = 0;
        TeamId casterTeam = kNoTeam;
        uint8_t caster = 0;
        bool hasHitVoxel = false;             ///< Blow: the fist met terrain at `hitVoxel` (the absorb rule reads it)
        glm::ivec3 hitVoxel{0};
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
    };

    /// @brief Counters of one cast (logged; `visits` and `chunksRead` are charged as work).
    struct SpellStats {
        uint32_t visits = 0;
        uint32_t chunksRead = 0;
        uint32_t planned = 0;                 ///< conjured voxels planned (Barricade Wave)
        uint32_t ops = 0;                     ///< ops applied now and scheduled
        uint32_t bytes = 0;                   ///< wire bytes of the ops, the zone starts, the cancels and the cast's fx
        uint32_t dashSteps = 0;               ///< Dash: path steps the trace kept
    };

    struct CastSpellResult {
        SpellRefusal refusal = SpellRefusal::None;
        uint32_t runTick = 0;                 ///< T_run
        uint32_t zoneTick = 0;                ///< T_z = T_run + the zone lead
        glm::ivec3 hitQ{0};                   ///< the hit (Punch: the fist point; Dash: the path's end), q
        uint8_t face = 0;                     ///< the hit face: axis * 2 + (sign < 0)
        bool dashStarted = false;
        std::vector<BlastResult> groups;      ///< ops applied now, one group each, in order
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
    };

    /// @brief The zone lead's start tick of a cast at `runTick`.
    uint32_t ZoneStartTick(const SpellTable& table, uint32_t runTick);

    /**
     * @brief The capture checks of a cast at `tick` (changes nothing): NotBuilt, NotAttuned (a caster
     * not in `ctx.casters` has the default elements), FormCooling. On None the caller sets the
     * caster's `formReadyTick[form] = tick + forms[form].cooldownTicks` (never given back).
     */
    SpellRefusal CheckCapture(const CastContext& ctx, const SpellTable& table, const SpellCast& cast, uint32_t tick);

    /**
     * @brief Casts a spell at T_run = `simulation.Tick()`: plans it, checks the spawn guard and the
     * caps, then applies it (docs/design/magic.md "The cast pipeline").
     * @details Updraft: a Lift y-cylinder over the Plume column from the air cell in front of the
     * hit, everyone. Dash: StartDash in `cast.casterState`, a capsule trace of the path from the
     * feet (at most dashRangeQ, stopped before the first step that meets a collider), an Impulse
     * capsule along it for enemies. Punch: the fist point (the first terrain or enemy capsule within
     * fistRangeQ of the eye, else that far out), a blow at T_z, a Shove capsule for enemies at T_z,
     * and the dispel of enemy dispellable zones live at T_z whose centre (a capsule's segment, a
     * cylinder's axis) is within dispelRadiusQ of the fist. Barricade Wave: a Burst strip extruded into knit bands at their ticks; no band cell
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
     * @brief The follow-ups due at `tick` in (tick, seq) order, then the registry's expiries, the site
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
