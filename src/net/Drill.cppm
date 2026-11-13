module;

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

export module vortex.net:drill;

import vortex.core;
import vortex.voxel;
import vortex.sim;

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
        DrillStep Step(uint32_t index) const;

    private:
        void Layout(const sim::RegionDesc& region);
        voxel::WorldMaterials m_Ids;
        std::vector<glm::ivec3> m_Sites;  ///< minimum corner of each site, y = foundation top
    };

    /// @brief What a BotPilot does besides walking.
    struct PilotSettings {
        bool run = true;             ///< some segments run
        bool strafe = true;          ///< some segments strafe
        bool jumps = true;           ///< a jump every 1-2 s
        bool casts = false;          ///< a cast every `castEveryTicks`, at the ground ahead
        uint32_t castEveryTicks = 90;
        float castPitch = -0.45f;    ///< radians: where it looks while casting (down, ahead)
        float edgeMargin = 2.0f;     ///< metres inside the play bounds it turns back towards the middle
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
        uint32_t Jumps() const { return m_Jumps; }

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
        uint32_t m_Casts = 0, m_Jumps = 0;
    };
}
