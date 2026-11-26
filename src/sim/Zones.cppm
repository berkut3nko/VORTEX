module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

export module vortex.sim:zones;

import vortex.voxel;

export namespace vortex::sim {

    /// @brief What a zone does to the players in it (docs/design/magic.md "sim::ZoneSet"). Wire values.
    enum class ZoneKind : uint8_t {
        None = 0,
        Impulse = 1,   ///< on its start tick: a push away from its centre (a capsule: from the nearest point of its axis) and up
        Lift = 2,      ///< while live: a lower bound on the vertical speed, ramped at the rim, eased to a hover at the top
        Pin = 3,       ///< reserved: no behaviour yet
        Chill = 4,     ///< while live and inside: slowed (strength {slowQ in sixteenths, tail ticks})
        Root = 5,      ///< on its start tick, inside: rooted for its duration (not during the control guard)
        Release = 6,   ///< on its start tick, inside: root, pin and chill end (the control guard starts)
        Block = 7,     ///< reserved: no behaviour yet
        Shove = 8,     ///< on its start tick: a push along the capsule's axis plus a lift, both falling to 0 at the rim
        Rune = 9,      ///< reserved: no behaviour yet
    };

    /**
     * @brief A shape that acts on players on the ticks [startTick, EndTick()). Integers only: q = 1/16
     * voxel, strengths in cm/s. Every peer holds the same zones and tests its own players against them.
     */
    struct Zone {
        uint16_t zoneId = 0;
        ZoneKind kind = ZoneKind::Impulse;
        uint8_t teamMask = 0xFF;          ///< bit t: acts on team t
        voxel::ShapeQ shape;              ///< sphere, capsule or y-cylinder
        uint32_t startTick = 0;
        uint16_t duration = 1;
        int16_t strength[2] = {0, 0};     ///< cm/s: Impulse {away, up}; Shove {along, up}; Lift {target up, 0}
        uint32_t EndTick() const { return startTick + duration; }
        friend bool operator==(const Zone&, const Zone&) = default;
    };

    /// @brief The records a ZoneSet holds at most (a client's cap; a server allows fewer live or pending).
    inline constexpr size_t kMaxZoneRecords = 64;

    /// @brief The box of a zone's shape in q, [minQ, maxQ] inclusive. A box shape covers voxels [a, b).
    void ZoneBoundsQ(const Zone& zone, glm::ivec3& minQ, glm::ivec3& maxQ);
    /**
     * @brief Feet at `feetQ` (q) inside the zone's shape, the membership of the kinds that do not push
     * (Chill, Root, Release, Rune): a sphere or a capsule closer than its radius (the integer root of
     * the squared distance), a y-cylinder within its radius and half height, a box by the feet's voxel.
     */
    bool ZoneShapeHolds(const Zone& zone, const glm::ivec3& feetQ);

    /**
     * @brief The zones of a simulation, sorted by (startTick, zoneId).
     * @details Every lookup takes the tick being stepped: a replayed step passes its own tick, so a
     * zone must stay in the set while any tick that may still be stepped can see it. The owner prunes
     * (PruneEndedBefore) with the oldest tick it can still step: a server its current tick, a client
     * its oldest replayable tick.
     */
    class ZoneSet {
    public:
        /// @brief False when the set is full or a zone with the same id is present.
        bool Insert(const Zone& zone);
        /// @brief Truncates the zone to end at `endTick` at the latest (never lengthens it; the ticks
        /// before `endTick` are unchanged). False when the id is unknown.
        bool Cancel(uint16_t zoneId, uint32_t endTick);
        const Zone* Find(uint16_t zoneId) const;
        /// @brief Calls `fn(const Zone&)` for the zones live at `tick` whose box meets [reachMinQ,
        /// reachMaxQ] (q, inclusive), in (startTick, zoneId) order.
        template <class F>
        void ForEachLive(uint32_t tick, const glm::ivec3& reachMinQ, const glm::ivec3& reachMaxQ, F&& fn) const {
            for (const Zone& z : m_Zones) {
                if (z.startTick > tick) break;
                if (tick >= z.EndTick()) continue;
                glm::ivec3 lo, hi;
                ZoneBoundsQ(z, lo, hi);
                if (hi.x < reachMinQ.x || lo.x > reachMaxQ.x || hi.y < reachMinQ.y || lo.y > reachMaxQ.y || hi.z < reachMinQ.z ||
                    lo.z > reachMaxQ.z)
                    continue;
                fn(z);
            }
        }
        /// @brief Removes the zones with EndTick() <= tick.
        void PruneEndedBefore(uint32_t tick);
        size_t Size() const { return m_Zones.size(); }
        /// @brief The zones with EndTick() > tick.
        size_t LiveOrPendingAt(uint32_t tick) const;
        std::span<const Zone> All() const { return m_Zones; }
        void Clear() { m_Zones.clear(); }

    private:
        std::vector<Zone> m_Zones;
    };
}
