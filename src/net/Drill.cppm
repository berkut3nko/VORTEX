module;

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

export module vortex.net:drill;

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
}
