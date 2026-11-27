module;

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

export module vortex.sim:knit;

import vortex.voxel;
import :simulation;
import :worldop;
import :blast;

export namespace vortex::sim {

    // --- conjured matter (docs/design/magic.md "Conjured matter", "Caps") -----------------------

    /// Live conjured voxels per player: over it, the player's oldest conjurings crumble at once.
    inline constexpr uint32_t kMaxConjuredPerPlayer = 6000;
    /// Live conjured voxels per region: over it a cast is refused.
    inline constexpr uint32_t kMaxConjuredPerRegion = 60000;
    /// Live turned voxels (rime, heat) per region: over it a cast is refused.
    inline constexpr uint32_t kMaxTurnedPerRegion = 40000;

    /// @brief One cast's matter of one layer: when and how it expires.
    struct KnitOwnerInfo {
        uint8_t layer = 0;        ///< 0 conjure (knit and the other conjured ids), 1 turn (rime, heat)
        uint8_t expiryMap = 0;    ///< a voxel::TurningMapId: Map_Unknit for conjured matter; 0: it expires with no op (offline pitch)
        uint8_t caster = 0;       ///< player slot + 1; 0 none
        uint8_t casterTeam = 0;   ///< the caster's sim::TeamId (0: no team)
        uint8_t spell = 0;        ///< the sim::SpellId that made it (the hazard scan credits its damage to it)
        uint32_t dueTick = 0;
        bool hasShape = false;    ///< its matter came from one MapShape: it can expire with that shape
        voxel::ShapeQ shape;
    };

    /**
     * @brief Who owns each conjured voxel, and the expiry of each owner (server and offline only;
     * never replicated).
     * @details Per chunk, a sorted vector of (voxel index, owner) per layer; a newer owner takes a
     * voxel from an older one. An owner expires with one op: a MapShape over its shape or its box
     * when no other owner of its layer ever shared its box (its `overlapped` bit is clear; the bit
     * stays once set), else a MapMask of the voxels it still owns that still hold an id its map
     * changes. Due owners run in (dueTick, owner) order.
     */
    class KnitRegistry {
    public:
        /// @brief A new owner; ids count up from 1.
        uint32_t NewOwner(const KnitOwnerInfo& info);
        /// @brief Records the voxels an op of `owner` changed (the edits of ApplyOp): into a conjured id for
    /// the conjure layer, any change for the turn layer.
        void Record(uint32_t owner, std::span<const voxel::VoxelEdit> edits, const voxel::MaterialLibrary& materials);
        /// @brief Due owners in (dueTick, owner) order: one expiry op each, applied. The applied ops
        /// in order (an owner with nothing left gives none).
        std::vector<AppliedOp> RunExpiries(Simulation& simulation, uint32_t tick);
        /// @brief Expires `caster`'s oldest conjure owners now until `incoming` more voxels fit
        /// kMaxConjuredPerPlayer. The applied ops in order.
        std::vector<AppliedOp> MakeRoom(Simulation& simulation, uint8_t caster, uint32_t incoming);
        uint32_t LiveVoxels(uint8_t layer) const;
        uint32_t LiveVoxelsOf(uint8_t caster) const;     ///< its conjure layer
        /// @brief The owner of voxel `v` in `layer` (null: none).
        const KnitOwnerInfo* OwnerAt(const glm::ivec3& v, uint8_t layer) const;
        size_t Owners() const { return m_Owners.size(); }

    private:
        struct Owner {
            KnitOwnerInfo info;
            uint32_t live = 0;
            glm::ivec3 boxMin{0}, boxMax{0};      ///< the voxels it recorded, [min, max)
            glm::ivec3 reachMin{0}, reachMax{0};  ///< the box joined with its shape's: what its expiry can touch
            bool overlapped = false;
            bool any = false;
        };
        using Entry = std::pair<uint16_t, uint32_t>;   ///< chunk-local voxel index, owner
        using ChunkKey = std::tuple<int, int, int>;

        /// Applies the owner's expiry op (into `out` when there is one), then forgets the owner.
        void Expire(Simulation& simulation, uint32_t id, std::vector<AppliedOp>& out);
        void Forget(uint32_t id, const Owner& owner);

        std::map<ChunkKey, std::array<std::vector<Entry>, 2>> m_Chunks;
        std::map<uint32_t, Owner> m_Owners;
        uint32_t m_NextOwner = 1;
    };

    /// @brief A voxel a player collides with: not air, not a liquid, not a film.
    bool IsCollider(const voxel::MaterialLibrary& materials, uint8_t id);

    /**
     * @brief The minimal structure credit (shred.md SI-2): site checks run one chunk at a time on
     * their own work budget, never the cast's.
     * @details A site check is `Blast(ChunkSite(chunk), {.noCarve = true})`: the island search and
     * the load check seeded at that chunk, no carve, no objects, no push. The queue is FIFO, holds
     * a chunk once and at most kMaxQueued chunks (a full queue drops the new chunk, counted).
     */
    struct StructureCredit {
        static constexpr int64_t kWorkPerTick = 15000;
        static constexpr size_t kMaxQueued = 256;
        int64_t credit = 0;
        uint32_t dropped = 0;                 ///< chunks not queued: the queue was full
        std::deque<glm::ivec3> queue;

        void QueueSite(const glm::ivec3& chunk);
        /// @brief Queues the chunk of every edit that turned a collider into a non-collider.
        void QueueFromEdits(std::span<const voxel::VoxelEdit> edits, const voxel::MaterialLibrary& materials);
        /**
         * @brief Refills once (call it once per tick: `credit = clamp(credit + kWorkPerTick,
         * -6 kWorkPerTick, kWorkPerTick)`), then runs sites while `credit > 0`, each charged
         * `CastWorkOf(result).Total()`. Appends the results in run order; returns how many ran.
         */
        uint32_t Run(Simulation& simulation, std::vector<BlastResult>& out);
    };
}
