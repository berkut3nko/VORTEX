module;

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

export module vortex.sim:spread;

import vortex.voxel;
import :simulation;
import :worldop;
import :blast;

export namespace vortex::sim {

    // --- fire offline (docs/design/spread.md "The step schedule and budgets", "Destruction and the load check") ----
    //  The driver keeps an index of the chunks that hold burning ids and advances the fire in logical steps, one in
    //  every kSpreadSlotTicks ticks while its work credit allows: every stepped chunk is decided from the old ids of it
    //  and its neighbours (voxel::StepBox) and written whole. A burnout queues a site check (the island search and the
    //  load check of the chunk), and a released piece that holds fire lights the floor under it when it would land.
    //  The index, the credit and the queues decide only when and where the rule runs, never what a voxel becomes.

    inline constexpr uint32_t kSpreadSlotTicks = 6;               ///< a logical step at most every 6 ticks: 10 Hz at 60 Hz
    inline constexpr int64_t kSpreadWorkPerStep = 2'000'000;      ///< the credit one slot adds (work units)
    inline constexpr uint32_t kMaxSitesPerStep = 8, kMaxQueuedSites = 256;
    /// Kindle caps: refused while this many chunks within kKindleLocalMetres of the hit hold fire, or this many anywhere.
    inline constexpr uint32_t kKindleLocalChunks = 16, kKindleGlobalChunks = 128;
    inline constexpr double kKindleLocalMetres = 32.0, kKindleRadiusMetres = 0.5, kLandingRadiusMetres = 0.375;
    /// A released piece lights the floor only when it holds this many burning voxels.
    inline constexpr uint32_t kLandingMinBurning = 8;
    /// The longest a pitch voxel burns while the credit never holds a slot: b1 waits at most a pace, b2..b4 a pace
    /// each (4 x 10 steps), plus the slot the first step waits for.
    inline constexpr uint32_t kPitchBurnoutTicks = (4u * 10u + 1u) * kSpreadSlotTicks;

    struct SpreadStats {
        uint32_t steps = 0;           ///< logical steps with something awake
        uint32_t slotsWaited = 0;     ///< slots skipped: the credit was spent
        uint32_t hardCapSteps = 0;    ///< steps that left the neighbours out: over twice the credit
        uint32_t silentSteps = 0;     ///< steps with nothing awake (the step index still advances)
        uint32_t awakeMax = 0, steppedMax = 0;
        uint64_t evaluated = 0, ignited = 0, burnedOut = 0, quenched = 0;
        uint64_t recooks = 0, recooksSkipped = 0;   ///< written chunks whose collider shape changed / did not
        uint32_t sites = 0, sitesDropped = 0, releases = 0;
        uint32_t landingsEmitted = 0, landingsDropped = 0;
        uint32_t kindles = 0, kindlesRefused = 0;
        double msSum = 0.0, msMax = 0.0;   ///< wall time of the steps (logged only)
        uint32_t peakBurningVoxels = 0;
        /// The lowest world voxel row a step lit (Kindles and landings not counted): fire never goes down.
        int32_t lowestStepIgnition = 2147483647;
        /// The lowest world voxel row a Kindle or a landing lit.
        int32_t lowestKindleRow = 2147483647;
    };

    enum class KindleResult : uint8_t { Done, TooMuchFire, Nothing };

    class SpreadDriver {
    public:
        /**
         * @brief Once per tick of a simulation with SimulationConfig::spread, between the game's step and the physics.
         * @details Every tick: the landing ignitions due. Every kSpreadSlotTicks ticks: the chunks edits changed are
         * re-counted, the credit refills, and one logical step runs when it is not spent (nothing burning: the step
         * index advances alone), then up to kMaxSitesPerStep site checks.
         * @param stepEdits If not null, every voxel the step and the landing ignitions changed (for PresentSpread).
         * @return The site checks that ran and the landing ignitions, in order (present them as blasts).
         */
        std::vector<BlastResult> Tick(Simulation& simulation, std::vector<voxel::VoxelEdit>* stepEdits = nullptr);
        /// @brief Applies MapShape(Map_Ignite, a sphere of `radiusMetres` at `center`) unless a cap refuses it; indexes
        /// what it lit. Nothing: no voxel changed.
        KindleResult Kindle(Simulation& simulation, const glm::dvec3& center, double radiusMetres = kKindleRadiusMetres,
                            std::vector<voxel::VoxelEdit>* edits = nullptr);
        uint32_t StepIndex() const { return m_S; }      ///< s of the last logical step
        uint32_t BurningVoxels() const;
        uint32_t BurningChunks() const;
        const SpreadStats& Stats() const { return m_Stats; }
        /// @brief Sets the credit (tests: a spent credit holds slots).
        void SetCredit(int64_t credit) { m_Credit = credit; }
        /// @brief Tests: compute and write the stepped chunks in reverse order (the voxels must not change).
        bool reverseOrder = false;

    private:
        using Key = std::tuple<int, int, int>;   ///< (y, z, x): the step's chunk order
        struct Entry {
            uint32_t burning = 0;
            voxel::BlockMask live{};
        };
        struct Landing {
            glm::ivec3 from{0};                  ///< the lowest burning voxel of the piece
        };
        static Key KeyOf(const glm::ivec3& c) { return {c.y, c.z, c.x}; }
        static glm::ivec3 ChunkOf(const Key& k) { return {std::get<2>(k), std::get<0>(k), std::get<1>(k)}; }
        void Recount(const voxel::World& world, const glm::ivec3& chunk);
        void Step(Simulation& simulation, std::vector<voxel::VoxelEdit>* stepEdits);
        void QueueSite(const glm::ivec3& chunk);
        void RunSites(Simulation& simulation, std::vector<BlastResult>& out);
        void RunLandings(Simulation& simulation, uint32_t tick, std::vector<BlastResult>& out, std::vector<voxel::VoxelEdit>* stepEdits);
        bool CapsRefuse(const voxel::World& world, const glm::dvec3& center) const;

        std::map<Key, Entry> m_Index;
        uint32_t m_S = 0;
        int64_t m_Credit = kSpreadWorkPerStep;
        std::deque<glm::ivec3> m_Sites;
        std::set<Key> m_SiteSet;
        std::multimap<uint32_t, Landing> m_Landings;
        int64_t m_FallSq1024 = 0;
        SpreadStats m_Stats;
    };

    /// @brief `Spread: s S, steps N (waited W, silent Q, hard cap H), awake A max, stepped T max, evaluated E, ignited I,
    /// burned out B, quenched Q, recooks R (skipped K), sites X (dropped Y), releases Z, landings L (dropped M), ms avg U
    /// max V, table T` (T = SpreadTableHash in hex).
    std::string FormatSpreadLine(const SpreadStats& stats, uint32_t s);
}
