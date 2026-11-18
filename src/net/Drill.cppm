module;

#include <algorithm>

#include <glm/glm.hpp>

#include <cstdint>
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
        /// `stuckTicks` is given up (counted).
        std::vector<glm::vec2> waypoints;
        float reachMetres = 2.0f;
        uint32_t stuckTicks = 300;
        /// Stage 8: after the last waypoint start again from the first (never Done).
        bool loopWaypoints = false;
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
        uint32_t Casts() const { return m_Casts; }
        uint32_t Crates() const { return m_Crates; }
        uint32_t Jumps() const { return m_Jumps; }
        /// @brief Waypoint mode: every waypoint reached or given up.
        bool Done() const { return !m_Settings.waypoints.empty() && m_Waypoint >= m_Settings.waypoints.size(); }
        uint32_t WaypointsReached() const { return m_Reached; }
        uint32_t WaypointsGivenUp() const { return m_GivenUp; }
        /// @brief Rounds begun again from the first waypoint (PilotSettings::loopWaypoints).
        uint32_t Laps() const { return m_Laps; }

    private:
        PilotSettings m_Settings;
        Rng m_Rng;
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
    };
}
