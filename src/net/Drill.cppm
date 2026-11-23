module;

#include <algorithm>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

export module vortex.net:drill;

import vortex.core;
import vortex.voxel;
import vortex.sim;
import :server;

export namespace vortex::net {

    /// @brief What a drill step builds and shoots (the three ways a blast frees terrain).
    enum class DrillKind : uint8_t {
        Mushroom,    ///< a cap on a stem cut in the middle: an island inside the island box, plus a crumb
        Bar,         ///< a 70-voxel bar on a post cut below it: a face-flood island (it reaches past the island box)
        NeckTower,   ///< the watch tower on a 2 x 2 stone neck, a weak shot in its hollow: released by the load check
    };
    const char* ToString(DrillKind kind);

    struct DrillStep {
        DrillKind kind = DrillKind::Mushroom;
        uint32_t site = 0;
        std::vector<sim::WorldOp> build;  ///< Clear the site, lay its foundation, build the structure.
        voxel::SphereQ cast;              ///< Then this blast frees part of it.
        uint16_t power = 0;
        /// Stage 6: the site's box (voxels, [min, max)): the replicated objects the last structure
        /// left there are cleared with it (Server::ClearBodies) before `build`.
        glm::ivec3 clearMin{0}, clearMax{0};
        /// Stage 6: crates dropped after `build` (model origin, voxels): every step one beside the
        /// structure that its blast catches, and on a site's first step one in the margin beside the
        /// site, which falls a little, rests and stays for the whole drill.
        std::vector<glm::ivec3> crates;
    };

    /**
     * @brief A scripted series of server-side blasts that each free terrain (docs/design/networking.md
     * stage 3 acceptance; also `vortex_server --drill N`). Step i rebuilds site i mod Sites() with
     * structure kind i mod 3 and shoots it. Authored in voxels of the region (structures are about
     * 10 m tall at 0.125 m), integers only: the same steps on any machine.
     * @details Sites are 100 x 100 voxels on a grid inside the play bounds. The foundation's top is
     * the site's ground: given (flat ground), or the highest solid voxel of the site's middle column
     * in a loaded world.
     */
    class BlastDrill {
    public:
        static constexpr int32_t kSiteVoxels = 100;
        static constexpr int32_t kSiteHeight = 96;    ///< Air cleared above the foundation.

        /// @brief Sites on flat ground whose top voxel row is `groundVoxels - 1`.
        BlastDrill(const sim::RegionDesc& region, const voxel::WorldMaterials& ids, int32_t groundVoxels);
        /// @brief Sites whose ground is read from `world` (the server's, loaded).
        BlastDrill(const sim::RegionDesc& region, const voxel::WorldMaterials& ids, const voxel::World& world);

        uint32_t Sites() const { return static_cast<uint32_t>(m_Sites.size()); }
        /// @brief Uses only the first `count` sites (at least one): the play bounds' -x / -z corner,
        /// away from a template's middle (vortex_server --drill-sites, a drill beside a session).
        void KeepSites(uint32_t count) { m_Sites.resize(std::clamp<size_t>(count, 1, m_Sites.size())); }
        DrillStep Step(uint32_t index) const;

    private:
        void Layout(const sim::RegionDesc& region);
        voxel::WorldMaterials m_Ids;
        std::vector<glm::ivec3> m_Sites;  ///< minimum corner of each site, y = foundation top
        int32_t m_EdgeX = 0;              ///< the play bounds' -x edge (voxels): the sites' crates stand beside it
        int32_t m_Columns = 1;            ///< sites along x
    };

    /**
     * @brief Runs a drill step on a server: clears the replicated objects in the step's site box
     * (Server::ClearBodies), applies its build operations, drops its crates (Server::SpawnCrate) and
     * queues its cast. Returns QueueCast's answer.
     */
    bool RunDrillStep(Server& server, const DrillStep& step);

    /// @brief What a BotPilot does besides walking.
    struct PilotSettings {
        bool run = true;             ///< some segments run
        bool strafe = true;          ///< some segments strafe
        bool jumps = true;           ///< a jump every 1-2 s
        bool casts = false;          ///< a cast every `castEveryTicks`, at the ground ahead
        uint32_t castEveryTicks = 90;
        uint32_t crateEveryTicks = 0;  ///< a crate (PlayerButton_Crate) every this many ticks; 0 = none (stage 6)
        float castPitch = -0.45f;    ///< radians: where it looks while casting (down, ahead)
        float edgeMargin = 2.0f;     ///< metres inside the play bounds it turns back towards the middle
        /// Waypoint mode (stage 7a M-b: `vortex_server --walk-across`): when not empty the pilot runs
        /// to these points (metres, x / z) in order instead of wandering, jumping as set, and stands
        /// once it reached the last (Done). A waypoint it makes no metre of progress towards for
        /// `stuckTicks` is given up (counted, where: GiveUps).
        std::vector<glm::vec2> waypoints;
        float reachMetres = 2.0f;
        uint32_t stuckTicks = 300;
        /// Waypoint mode: after `detourAfterTicks` without a metre of progress it goes round what holds
        /// it (a tree's canopy at head height, a wall): it steps aside at right angles to the waypoint
        /// for `detourTicks`, heads for the waypoint again for `detourTicks`, then steps to the other
        /// side for twice as long, and so on (a metre of progress ends the detour). 0 = never (it
        /// pushes straight on until it gives up). Trees on the arena's middle z held a straight pusher
        /// for 5 s now and then (a contributing weakness of G19's failure on 2026-11-22; that run's
        /// decisive 150 s standing still is unexplained). A detour hides a walker that snags more
        /// often: the verdict bounds the detour share and the longest still stretch (BotGates).
        uint32_t detourAfterTicks = 45;
        uint32_t detourTicks = 30;
        /// Stage 8: after the last waypoint start again from the first (never Done).
        bool loopWaypoints = false;
        /// [wp-7] Fight mode (fight.md "Bots"; `vortex_server --fight`, `vortex_bot --fight`): with a target
        /// (BotPilot::SetTargets) it keeps its distance to the nearest enemy, strafes, aims at its feet and casts
        /// on edges; without one it wanders as above.
        bool fight = false;               ///< aim and cast at enemies (PilotTarget) instead of the ground ahead
        float fightRange = 30.0f;         ///< metres: casts only at targets this near
        float keepDistance = 12.0f;       ///< metres: closes in beyond +4, backs off under -4, strafes between
        uint32_t strafeTicks = 60;        ///< ticks per strafe direction
        uint32_t reactionTicks = 20;      ///< ticks between seeing a new target and the first cast at it
    };
    /// @brief [wp-7] An enemy a fighting pilot may aim at (feet, world metres).
    struct PilotTarget {
        uint8_t slot = 0;
        glm::vec3 feet{0.0f};
    };

    /**
     * @brief Scripted controls of a bot player (docs/design/networking.md stage 4: bots walking,
     * jumping and casting through blasts; `vortex_server` and the movement tests). Segments of
     * 20-80 ticks in a seeded direction, running or walking, sometimes strafing, a jump now and
     * then, a cast every `castEveryTicks`; near the edge of the play bounds it heads back to the
     * middle. Deterministic for a seed and the states it is given.
     */
    class BotPilot {
    public:
        BotPilot(const sim::RegionDesc& region, uint64_t seed, PilotSettings settings = {});
        /// @brief The controls for the next tick of a player now in `state` (its predicted state).
        sim::PlayerInput Next(const sim::PlayerState& state);
        /// @brief [wp-7] Fight mode: the living enemies (slot ascending) and whether it may cast now (alive, a
        /// charge, cooldown 0); set before each Next.
        void SetTargets(std::span<const PilotTarget> enemies) { m_Targets.assign(enemies.begin(), enemies.end()); }
        void SetCanCast(bool canCast) { m_CanCast = canCast; }
        /// @brief Cast edges made at targets (fight mode).
        uint32_t FightCasts() const { return m_FightCasts; }
        /// @brief The slot of the enemy it holds now (fight mode); -1 for none.
        int32_t TargetSlot() const { return m_HasTarget ? static_cast<int32_t>(m_TargetSlot) : -1; }
        uint32_t Casts() const { return m_Casts; }
        uint32_t Crates() const { return m_Crates; }
        uint32_t Jumps() const { return m_Jumps; }
        /// @brief Waypoint mode: every waypoint reached or given up.
        bool Done() const { return !m_Settings.waypoints.empty() && m_Waypoint >= m_Settings.waypoints.size(); }
        uint32_t WaypointsReached() const { return m_Reached; }
        uint32_t WaypointsGivenUp() const { return m_GivenUp; }
        /// @brief Where the feet were (x / z) when a waypoint was given up: the first kMaxGiveUps.
        const std::vector<glm::vec2>& GiveUps() const { return m_GiveUps; }
        static constexpr size_t kMaxGiveUps = 16;
        /// @brief Waypoint mode: detours begun (one per hold, however many legs aside it took) and the
        /// ticks spent in them (aside and the straight tries between, from detourAfterTicks on until a
        /// metre of progress or the give-up).
        uint32_t DetourTicks() const { return m_DetourTicks; }
        uint32_t Detours() const { return m_Detours; }
        /// @brief Waypoint mode: ticks it headed for a waypoint (not Done), the detour share's base.
        uint32_t WalkTicks() const { return m_WalkTicks; }
        /// @brief Waypoint mode: a run of ticks (Next calls, not Done) in which the feet stayed within
        /// a radius (x / z) of `at`, where they were at its first tick, `startTick` (Next calls so far);
        /// `heldTicks` of them were given a state with sim::PlayerFlag_Held (StepPlayer's collider gate
        /// held the step before: the player could not move).
        struct StillStretch {
            uint32_t ticks = 0;
            uint32_t startTick = 0;
            uint32_t heldTicks = 0;
            glm::vec3 at{0.0f};
        };
        /// The still stretch's radius (Still) and the wider one of a walker stuck but shuffling (Confined).
        static constexpr float kStillMetres = 1.0f;
        static constexpr float kConfinedMetres = 4.0f;
        /// @brief The still stretch (within kStillMetres) going on now (ticks 0 once Done) and the longest so far.
        const StillStretch& Still() const { return m_Still; }
        const StillStretch& LongestStill() const { return m_LongestStill; }
        /// @brief The same within kConfinedMetres: a walker held in a pit or a corner that shuffles more
        /// than 1 m (each step aside of a detour) restarts the still stretch, never this one.
        const StillStretch& Confined() const { return m_Confined; }
        const StillStretch& LongestConfined() const { return m_LongestConfined; }
        /// @brief Rounds begun again from the first waypoint (PilotSettings::loopWaypoints).
        uint32_t Laps() const { return m_Laps; }

    private:
        PilotSettings m_Settings;
        Rng m_Rng;
        uint64_t m_Seed = 0;
        glm::vec2 m_Min{0.0f}, m_Max{0.0f};   ///< play bounds shrunk by edgeMargin, metres (x, z)
        uint32_t m_Tick = 0;
        uint32_t m_SegmentLeft = 0;
        uint32_t m_NextJump = 60;
        uint16_t m_Yaw = 0;
        int8_t m_Strafe = 0;
        bool m_Run = false;
        uint32_t m_Casts = 0, m_Jumps = 0, m_Crates = 0;
        size_t m_Waypoint = 0;
        float m_BestDistance = 0.0f;
        uint32_t m_SinceProgress = 0;
        uint32_t m_Reached = 0, m_GivenUp = 0;
        uint32_t m_Laps = 0;
        std::vector<glm::vec2> m_GiveUps;
        uint32_t m_DetourTicks = 0, m_Detours = 0, m_WalkTicks = 0;
        bool m_InDetour = false;
        StillStretch m_Still, m_LongestStill, m_Confined, m_LongestConfined;
        // [wp-7] fight mode
        bool FightStep(const sim::PlayerState& state, sim::PlayerInput& in);
        std::vector<PilotTarget> m_Targets;
        bool m_CanCast = true;
        bool m_HasTarget = false;
        uint8_t m_TargetSlot = 0;
        uint32_t m_TargetHeld = 0;        ///< ticks the current target was held
        bool m_CastDown = false;          ///< the Cast bit of its last input
        uint32_t m_FightCasts = 0;
    };
}
