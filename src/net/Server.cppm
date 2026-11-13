module;

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

export module vortex.net:server;

import vortex.voxel;
import vortex.sim;
import :transport;
import :messages;
import :session;

export namespace vortex::net {

    /// @brief How a Server is built: the session, the simulation sizes, and the per-tick budgets.
    struct ServerSettings {
        SessionConfig session;
        /// Job threads and physics sizes; the world (voxel size, window, height range, memory
        /// only) comes from the session and sim::ServerConfig is applied on top.
        sim::SimulationConfig simulation;
        /// The event budget (docs/design/networking.md "Transport": est. 24 kB/s, burst 16 kB):
        /// credit per tick and its cap. A queued cast waits while the credit is spent.
        uint32_t eventBytesPerTick = 400;
        uint32_t eventBurstBytes = 16384;
        /// The cast work budget per tick (sim::CastWork units, shred.md R5).
        uint64_t castWorkPerTick = sim::kCastWorkPerTick;
        /// A cast that has waited this many ticks runs whatever the budgets say (logged, counted).
        uint32_t maxCastWaitTicks = 6;
        /// Largest EventBatch the server makes (more events go into the next batch; one event
        /// larger than this is a batch of its own). A batch also holds at most kMaxMaskVoxels of
        /// masks (what a client decodes).
        uint32_t maxBatchBytes = 16384;
        /// Pacing per peer (networking.md "Transport": everything sent has a byte budget): the next
        /// batch for a peer is handed to the transport only while less than this is in flight to
        /// it (ITransport::ReliableBytesInFlight), so a joiner's catch-up waits in the server's log,
        /// not in the transport ahead of everyone's live events. 0 = unpaced (tests).
        uint32_t maxEventBytesInFlight = 32768;
        /// A connected peer that has not sent a valid Hello after this many ticks is disconnected.
        uint32_t helloTimeoutTicks = 600;
        /// A refused peer is disconnected once its refusal has left the transport, or after this many ticks.
        uint32_t closeGraceTicks = 120;
        /// CastRecords kept (the most recent; running totals are in ServerStats).
        uint32_t castRecordLimit = 4096;
    };

    /// @brief One cast as the server ran it (for logs, tests and the stage-8 delay histogram).
    struct CastRecord {
        uint32_t queuedTick = 0;
        uint32_t tick = 0;                ///< The tick it ran on.
        voxel::SphereQ sphere;
        uint16_t power = 0;
        uint32_t firstOpSeq = 0;          ///< Its carve.
        uint32_t opCount = 0;
        uint32_t eventBytes = 0;
        sim::CastWork work;
        bool overdue = false;             ///< Ran past the budgets after maxCastWaitTicks.
        /// Wall time of sim::Blast (shred.md R5: logged to see a miscalibration). Never read by
        /// the schedule: a replayed server must put every cast on the same tick.
        double seconds = 0.0;
        uint32_t islands = 0;             ///< Detach operations (pieces of every kind).
        uint32_t crumbs = 0;              ///< Islands in its ClearMask.
        uint32_t inBoxIslands = 0;        ///< Detach of islands inside the island box.
        uint32_t facePieces = 0;          ///< Detach of face-flood islands.
        uint32_t neckPieces = 0;          ///< Detach of parts the load check released.
        uint32_t neckCrumbs = 0;
        uint32_t searchErrors = 0;        ///< islandBoxUnloaded + attachedByUnloaded + neckUnloaded: must be 0.
    };

    struct ServerStats {
        double preloadSeconds = 0.0;      ///< LoadBlocking of the whole window at start.
        uint32_t templateOps = 0;         ///< Operations 1..k of the template.
        uint32_t castsQueued = 0, castsRun = 0, castsRejected = 0, castsOverdue = 0;
        /// Ticks a cast waited in the queue: [0..6] and 7 or more.
        std::array<uint32_t, 8> castDelay{};
        uint32_t maxCastDelay = 0;
        uint32_t ticksWaitedForWork = 0;  ///< Ticks that ended with a cast waiting for the work budget.
        uint32_t ticksWaitedForBytes = 0; ///< ... for the event byte budget.
        uint64_t castWork = 0, maxCastWork = 0;
        uint64_t events = 0, eventBytes = 0;
        uint32_t oversizeEvents = 0;      ///< Events larger than the burst (sent anyway, logged).
        uint32_t searchErrors = 0;        ///< Sum of CastRecord::searchErrors.
        uint32_t incompleteOps = 0;       ///< Operations the server's own world could not apply in full: an error.
        uint32_t clientsJoined = 0, clientsRefused = 0;
        uint32_t clientsDisconnected = 0; ///< Joined clients that left (or were dropped when the session failed).
        uint32_t peersTimedOut = 0;       ///< Peers that sent no Hello in time, or never took their refusal.
        uint64_t sentBytes = 0;           ///< Event batches and status messages.
        uint32_t sendsHeld = 0;           ///< Times a peer's next batch waited for its bytes in flight to drop.
        uint32_t maxBacklogEvents = 0;    ///< Most events made but not yet handed to one peer's transport.
        /// Wall clock of the casts (logged only, never read by the schedule).
        double castSeconds = 0.0, maxCastSeconds = 0.0;
        double maxCastMsPer10k = 0.0;     ///< The highest ms per 10 000 work units of one cast (of at least 1000 units).
        /// Running totals over every cast (CastRecords keep only the most recent).
        uint64_t islands = 0, crumbs = 0, inBoxIslands = 0, facePieces = 0, neckPieces = 0;
    };

    /**
     * @brief The session server (docs/design/networking.md stage 3): one sim::Simulation with a
     * fixed window (play bounds + 3 chunks, the region's height range), the template's operations
     * as opSeq 1..k, an event stream with a byte budget, a cast queue with a work budget, and
     * joiners who receive the whole event log. GPU-free.
     * @details Tick order (one call of Tick): receive -> streaming -> queued casts within the
     * work and event budgets -> body guard, physics, pose sync -> lifetimes -> each joined client's
     * next batches while its bytes in flight are under maxEventBytesInFlight -> a status on even
     * ticks. Operations and casts given between ticks (ApplyOp, QueueCast) are applied / queued at
     * once and go out with the next tick. Once Failed, the session is over: peers are
     * disconnected, Tick only turns new peers away, ApplyOp and QueueCast refuse.
     */
    class Server {
    public:
        Server(ServerSettings settings, ITransport& transport);
        ~Server();
        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        /// @brief Builds the world, loads the whole window, applies the template (operations 1..k,
        /// lights as EntitySpawn, objects as server bodies) and the probe hashes. false on an
        /// invalid region or a failed simulation (logged).
        bool Start();
        /// @brief One tick.
        void Tick();
        /// @brief Wall-clock driving: adds `seconds` and runs due ticks, at most 2 per call (a
        /// hitch is caught up over later calls, never dropped). Returns the ticks run.
        uint32_t Advance(double seconds);

        /// @brief A server-side operation (template, script, admin): applied now, sent with the next
        /// tick; its opSeq. 0 when the session has failed, or fails because the server's own world
        /// could not apply it in full (it is then never sent).
        uint32_t ApplyOp(const sim::WorldOp& op);
        /// @brief Queues a cast for the next tick's cast phase. false (counted) when it does not
        /// fit the region (sim::CastFitsRegion: centre in the play bounds, carve inside the rows,
        /// radius at most sim::kMaxCastRadiusQ) or the session has failed.
        bool QueueCast(const voxel::SphereQ& sphere, uint16_t power);
        bool QueueCast(const sim::BlastDesc& desc);
        size_t QueuedCasts() const;

        uint32_t TickNumber() const;      ///< The tick that runs next.
        uint32_t LastOpSeq() const;       ///< 0 before the first event.
        size_t ClientCount() const;       ///< Joined clients.
        bool Failed() const;              ///< A store slot failed, an operation was incomplete or the island search met an unloaded chunk: the session has ended.

        sim::Simulation& Simulation();
        const sim::RegionDesc& Region() const;
        const sim::RegionTemplate& Template() const;
        const std::vector<ProbeHash>& Probes() const;
        uint64_t BuildHashValue() const;
        voxel::WorldHashResult Hash() const;
        const ServerStats& Stats() const;
        const std::deque<CastRecord>& Casts() const;   ///< The most recent ServerSettings::castRecordLimit.
        /// @brief Every event made so far, encoded (WriteEvent), with its tick: the join log.
        size_t LogBytes() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
