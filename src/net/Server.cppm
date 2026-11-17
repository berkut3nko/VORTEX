module;

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

export module vortex.net:server;

import vortex.voxel;
import vortex.sim;
import :transport;
import :messages;
import :session;
import :entities;
import :sync;

export namespace vortex::net {

    /// @brief How a Server is built: the session, the simulation sizes, and the per-tick budgets.
    struct ServerSettings {
        SessionConfig session;
        /// Job threads and physics sizes; the world (voxel size, window, height range, memory
        /// only) comes from the session and sim::ServerConfig is applied on top.
        sim::SimulationConfig simulation;
        /// The event budget (docs/design/networking.md "Transport": est. 24 kB/s, burst 16 kB):
        /// credit per tick and its cap. A queued cast waits while the credit is spent. Terrain
        /// operations charge it; entity events (stage 6) do not (ServerStats::entityEventBytes).
        uint32_t eventBytesPerTick = 400;
        uint32_t eventBurstBytes = 16384;
        /// The entity event budget (review of stage 6), apart from the cast queue's (casts never wait
        /// for it: entity events follow from Jolt, shred.md R5): credit per tick and its cap. Every
        /// entity event charges it; while it is spent a cast makes no new server body (its pieces are
        /// client rubble, sim::PieceSlot::OverByteBudget: a terrain piece with no model on the wire, an
        /// object piece a fragment) and a damaged replicated object is let go as client rubble
        /// (EntityDespawn{ToRubble}, 6 bytes; a client keeps its copy as it was) instead of an
        /// ObjectModel with its whole model. Rests, wakes, despawns and placed objects are always sent (they are small or bounded
        /// by their own limits). 200 B a tick = 12 kB/s, half the terrain budget.
        uint32_t entityEventBytesPerTick = 200;
        uint32_t entityEventBurstBytes = 16384;
        /// The cast work budget per tick (sim::CastWork units, shred.md R5).
        uint64_t castWorkPerTick = sim::kCastWorkPerTick;
        /// A cast that has waited this many ticks runs whatever the budgets say (logged, counted).
        uint32_t maxCastWaitTicks = 6;
        /// Largest EventBatch the server makes (more events go into the next batch; one event
        /// larger than this is a batch of its own). A batch also holds at most kMaxMaskVoxels of
        /// masks (what a client decodes).
        uint32_t maxBatchBytes = 16384;
        /// Live events are not held by bytes in flight (stage 7a: a joiner starts at its join point,
        /// there is no join log to pace; plan amendment 2). A test may still hold a peer's next batch
        /// while this much is in flight to it (ITransport::ReliableBytesInFlight). 0 = never.
        uint32_t maxEventBytesInFlight = 0;
        /// A peer with more than this in flight (reliable bytes it has not taken) is disconnected
        /// (ServerStats::peersTooSlow): the memory bound of a client that stopped reading.
        size_t maxPeerBacklogBytes = size_t{8} << 20;
        /// Bulk (ChunkData, the entity baseline, model answers; stage 7a): the delay-based window
        /// (BulkPacer), an optional hard cap in bytes per second per peer (0 = none; tests, admins),
        /// and the distance (metres, horizontal) within which a peer's chunks go before its baseline.
        BulkPacerSettings bulkPacer;
        double maxBulkBytesPerSecond = 0.0;
        float bulkNearMetres = 32.0f;
        /// The join's baseline is sent nearest the spawn first; after the entities that come within
        /// this many metres of it (3D, the model's extent included) a BaselineNear opens the joiner's
        /// start gate (review of stage 7a: the far ones no longer hold the player).
        float baselineNearMetres = 32.0f;
        /// A peer's request answers (ChunkRequest coordinates, ModelRequest NetIds) queued at most;
        /// beyond, the peer is disconnected as for a broken sync message (review of stage 7a: a
        /// repeated request cannot grow the queue without bound). Repeats of a queued one are free.
        uint32_t maxQueuedAnswers = 1u << 17;
        /// Tests: count every ChunkRequest coordinate as received, per coordinate
        /// (Server::ChunkRequestCounts). Off by default: the map would grow with every coordinate any
        /// peer ever asked for (recheck of stage 7a, 4).
        bool countChunkRequests = false;
        /// A connected peer that has not sent a valid Hello after this many ticks is disconnected.
        uint32_t helloTimeoutTicks = 600;
        /// A refused peer is disconnected once its refusal has left the transport, or after this many ticks.
        uint32_t closeGraceTicks = 120;
        /// CastRecords kept (the most recent; running totals are in ServerStats).
        uint32_t castRecordLimit = 4096;
        /// Movement and cast tuning of every player; sent in Welcome (clients predict with it).
        sim::PlayerTuning player;
        /// Inputs a player may be ahead of the server (docs/design/networking.md "Time and ticks": the
        /// queue holds 16 per player; inputs further ahead are dropped - also the speed-hack bound).
        uint32_t inputQueueTicks = 16;
        /// A missing input repeats the last movement for this many ticks, then the player stands.
        uint32_t repeatInputTicks = 6;
        /// Players' crates (sim::PlayerButton_Crate, the game's E key; networking stage 6): a crate
        /// edge drops the playground's crate `crateDistance` metres along the look from the eye, at
        /// most one per player every `crateCooldownTicks`; a player's crate beyond
        /// `maxCratesPerPlayer` removes its oldest (EntityDespawn{Destroyed}). A crate is refused while
        /// sim::ReplicationConfig::maxAwakePieces replicated bodies are awake (the snapshot budget), and
        /// a player's crates go when the player leaves (EntityDespawn{Destroyed}).
        uint32_t crateCooldownTicks = 30;
        uint32_t maxCratesPerPlayer = 8;
        float crateDistance = 3.0f;
        /// Snapshots (stage 6): at most this many bytes; the awake bodies that fit after the players
        /// go by a priority that grows with every snapshot a body was left out of and shrinks with its
        /// distance from the client's player (1 + distance / bodyPriorityMetres).
        size_t snapshotBytes = kSnapshotBudgetBytes;
        float bodyPriorityMetres = 16.0f;
    };

    /// @brief One cast as the server ran it (for logs, tests and the stage-8 delay histogram).
    struct CastRecord {
        uint8_t player = 0;               ///< 0 = a server-side cast (QueueCast); else slot + 1 of the player who cast it
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
        /// Bytes of entity events (stage 6), part of eventBytes; not charged to the cast queue's event
        /// credit (they follow from physics: a replayed server must schedule casts by counts only).
        uint64_t entityEventBytes = 0;
        uint32_t oversizeEvents = 0;      ///< Events larger than the burst (sent anyway, logged).
        uint32_t searchErrors = 0;        ///< Sum of CastRecord::searchErrors.
        uint32_t incompleteOps = 0;       ///< Operations the server's own world could not apply in full: an error.
        uint32_t clientsJoined = 0, clientsRefused = 0;
        uint32_t clientsDisconnected = 0; ///< Joined clients that left (or were dropped when the session failed).
        uint32_t peersTimedOut = 0;       ///< Peers that sent no Hello in time, or never took their refusal.
        uint64_t sentBytes = 0;           ///< Event batches and status messages.
        uint32_t sendsHeld = 0;           ///< Times a peer's next batch waited for its bytes in flight to drop (maxEventBytesInFlight).
        uint32_t maxBacklogEvents = 0;    ///< Most events made but not yet handed to one peer's transport.

        // Chunk sync (stage 7a).
        uint32_t windowUpdates = 0, windowAcks = 0;
        uint64_t ackedModified = 0;       ///< entering chunks the acks marked modified (queued as ChunkData)
        uint64_t chunkData = 0, chunkDataBytes = 0;
        uint32_t chunksDropped = 0;       ///< queued chunks that had left the peer's window at dequeue
        uint32_t chunkRequests = 0, modelRequests = 0;   ///< coordinates / NetIds asked for
        uint32_t requestsIgnored = 0;     ///< ... outside the windows, or of NetIds that are gone
        uint32_t baselineEntities = 0;    ///< BaselineEntity messages (joins and model answers)
        uint64_t baselineBytes = 0;
        uint32_t baselineDropped = 0;     ///< baseline NetIds despawned before their dequeue
        uint64_t bulkBytes = 0;           ///< every bulk message
        uint64_t bulkHeld = 0;            ///< ticks a peer's bulk waited for its window (or the cap)
        uint32_t bulkOversize = 0;        ///< bulk messages over 16 KiB (one may overshoot the window)
        uint32_t peersTooSlow = 0;        ///< disconnected: over maxPeerBacklogBytes in flight
        uint32_t malformedRequests = 0;   ///< sync messages that did not decode or broke the window order: disconnected
        size_t minBulkWindow = 0, maxBulkWindow = 0;
        size_t maxBulkMessage = 0;        ///< the largest bulk message (one larger than the window goes alone)
        uint32_t bulkAlone = 0;           ///< bulk messages handed over past the window because no bulk was in flight
        /// Bulk messages handed over while the peer's bytes in flight (its live events) were already at
        /// or over the window: never (recheck of stage 7a, 1: the events alone filling the window hold
        /// the bulk, whatever the bulk in flight).
        uint32_t bulkOverWindow = 0;
        uint32_t modelRequestRepeats = 0; ///< ModelRequest NetIds already queued for that peer (ignored)
        uint32_t baselineNear = 0;        ///< BaselineNear markers sent
        /// Wall clock of the bulk encodes (EncodeChunkAt, baseline entities) per tick: sum and the
        /// largest tick (logged only; review of stage 7a).
        double encodeSeconds = 0.0, maxEncodeSecondsPerTick = 0.0;
        /// Wall clock of the casts (logged only, never read by the schedule).
        double castSeconds = 0.0, maxCastSeconds = 0.0;
        double maxCastMsPer10k = 0.0;     ///< The highest ms per 10 000 work units of one cast (of at least 1000 units).
        /// Running totals over every cast (CastRecords keep only the most recent).
        uint64_t islands = 0, crumbs = 0, inBoxIslands = 0, facePieces = 0, neckPieces = 0;

        // Players (stage 4).
        uint64_t inputPackets = 0;        ///< InputBatch messages received
        uint64_t malformedInputs = 0;     ///< state-channel messages from a joined client that did not decode (ignored)
        uint64_t inputsQueued = 0;        ///< inputs for a tick not simulated yet
        uint64_t inputsLate = 0;          ///< inputs for a tick already simulated: they only refreshed the last input
        uint64_t inputsDuplicate = 0;     ///< inputs the server already had (every packet repeats the unacknowledged ones)
        uint64_t inputsTooEarly = 0;      ///< inputs inputQueueTicks or more ahead: dropped
        uint64_t inputsBeforeJoin = 0;    ///< inputs for a tick before the player joined (no client makes them): ignored
        uint64_t ticksRepeated = 0;       ///< player ticks without an input: the last movement repeated
        uint64_t ticksStood = 0;          ///< ... and after repeatInputTicks of them: no movement
        uint64_t heldTicks = 0;           ///< player ticks the gate held (terrain without a collider)
        uint32_t castsFromInputs = 0;     ///< cast edges the cooldown accepted, captured and queued (stage 5: the ray waits for the dequeue)
        uint32_t castsFromLateInputs = 0; ///< ... of them, found in a late input (judged and captured at the input's tick)
        uint32_t castsOnCooldown = 0;     ///< cast edges the cooldown at their input's tick refused (CastRejection::Cooldown)
        uint32_t castsMissed = 0;         ///< captured casts whose ray, cast at dequeue, met no terrain within castRange
        uint32_t castsAtBodies = 0;       ///< ... whose ray met a replicated body before the terrain (the cast is centred there)
        /// Jump edges found in a late input: carried to the next tick the server simulates (the
        /// client jumped at the input's tick, so this is a correction of the jump's timing, not a
        /// missing jump; review of stage 4).
        uint32_t jumpsFromLateInputs = 0;
        uint64_t snapshots = 0, snapshotBytes = 0;

        // Replicated objects (stage 6).
        uint32_t objectSpawns = 0;        ///< EntitySpawn of objects with a NetId (template objects, crates, object pieces)
        uint32_t detachBodies = 0;        ///< Detach operations with a NetId (terrain pieces that are server bodies)
        uint32_t fragmentSpawns = 0;      ///< EntitySpawn with netId 0 (object fragments: client rubble)
        uint32_t objectModels = 0;        ///< ObjectModel events (a damaged object survived)
        uint32_t rests = 0;               ///< EntityRest events
        uint32_t wakes = 0;               ///< EntityWake events
        std::array<uint32_t, 4> despawns{};   ///< EntityDespawn by scene::DespawnReason (index 1..3)
        /// Damaged replicated objects let go as client rubble (ToRubble) because the entity byte budget
        /// was spent (no ObjectModel sent).
        uint32_t modelsToRubble = 0;
        uint32_t crates = 0;              ///< crates from players and SpawnCrate
        uint32_t cratesRefused = 0;       ///< crate edges in the cooldown or at the awake cap, or a crate that would leave the play bounds
        uint64_t bodySamples = 0;         ///< bodies sent in snapshots
        uint64_t bodiesLeftOut = 0;       ///< awake bodies a snapshot had no room for (summed over snapshots)
        uint32_t maxSnapshotBodies = 0;
        uint32_t maxAwakeBodies = 0;      ///< most replicated objects awake at the end of a tick
        size_t maxSnapshotBytes = 0;
    };

    /**
     * @brief The session server (docs/design/networking.md stages 3-7a): one sim::Simulation with a
     * fixed window (play bounds + 3 chunks, the region's height range), the template's operations
     * as opSeq 1..k, an event stream with a byte budget, a cast queue with a work budget, joiners
     * synced by chunk data (stage 7a: the modified map, WindowUpdate / WindowAck, ChunkData and the
     * entity baseline on paced bulk, encoded at dequeue), and one player per joined client moved by
     * its inputs. GPU-free.
     * @details Tick order (one call of Tick): receive (inputs into each player's queue; a cast
     * edge in a late input is judged and captured at the input's tick) -> streaming (colliders
     * around the players) -> sim::StepPlayer per player in slot order (its input for this tick, or
     * the last movement repeated; a cast edge the cooldown accepts is captured as {eye, look,
     * input tick, loadout} and queued) -> queued casts within the work and event budgets (a
     * player's ray is cast into the world when its cast runs) -> body guard,
     * physics, pose sync -> lifetimes -> each joined client's batches, then its bulk while its bytes
     * in flight are under its bulk window -> a snapshot per client on even ticks. Operations and casts given between ticks (ApplyOp, QueueCast) are applied / queued at
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

        /// @brief Admin / drill (stage 6): drops the playground's crate with its model origin at
        /// `origin` (world metres) as a replicated body. Its NetId, or 0 when it would leave the play
        /// bounds or the window's rows, or the session has failed.
        uint32_t SpawnCrate(const glm::dvec3& origin);
        /// @brief Admin / tests (stage 6): any voxel object (world palette) with its model origin at
        /// `origin`, a replicated placed object (Dynamic). Its NetId, or 0 as SpawnCrate.
        uint32_t SpawnObject(std::shared_ptr<voxel::VoxelModel> model, const glm::dvec3& origin);
        /// @brief Admin / drill: destroys every replicated object whose body overlaps the box (world
        /// metres; EntityDespawn{Destroyed}); how many.
        uint32_t ClearBodies(const glm::dvec3& min, const glm::dvec3& max);
        /// @brief The replicated objects now (the entity check: CompareEntities with a client's).
        std::vector<EntityInfo> Entities() const;
        /// @brief Replicated objects awake now (not rested since they last moved).
        size_t AwakeBodies() const;

        /// @brief Players in the session (one per joined client), and the state of the one in `slot`.
        size_t PlayerCount() const;
        std::optional<sim::PlayerState> GetPlayer(uint8_t slot) const;
        /// @brief Admin: puts the player of `slot` at `feet`, standing still (the owner is corrected by
        /// the next snapshot). false without such a player or for a non-finite position.
        bool TeleportPlayer(uint8_t slot, const glm::vec3& feet);

        uint32_t TickNumber() const;      ///< The tick that runs next.
        uint32_t LastOpSeq() const;       ///< 0 before the first event.
        size_t ClientCount() const;       ///< Joined clients.
        /// @brief Called when a joined client leaves (its slot), before anything of its player is
        /// removed: the server's opSeq is still the last one that client could have applied (its
        /// crates' despawns follow; tools/session-check.sh compares hashes at that opSeq).
        void SetOnClientLeft(std::function<void(uint8_t slot)> callback);
        bool Failed() const;              ///< A store slot failed, an operation was incomplete or the island search met an unloaded chunk: the session has ended.

        sim::Simulation& Simulation();
        const sim::RegionDesc& Region() const;
        const sim::RegionTemplate& Template() const;
        const std::vector<ProbeHash>& Probes() const;
        uint64_t BuildHashValue() const;
        voxel::WorldHashResult Hash() const;
        /// @brief WorldHash over a box of chunks (a window-sized client's sync box).
        voxel::WorldHashResult Hash(const SyncBox& box) const;
        /// @brief Chunks the modified map holds (every chunk an event changed, never cleared).
        size_t ModifiedChunks() const;
        /// @brief How many times each chunk coordinate came in a ChunkRequest (any peer, as received;
        /// tests: no chunk is requested twice). Empty unless ServerSettings::countChunkRequests.
        std::vector<std::pair<glm::ivec3, uint32_t>> ChunkRequestCounts() const;
        /// @brief The sync box of the joined client in `slot`: its last WindowUpdate's window inside
        /// the server's (what its Client::Hash covers); empty when there is no such client or it sent
        /// no window yet. Valid inside the SetOnClientLeft callback.
        SyncBox ClientSyncBox(uint8_t slot) const;
        /// @brief Changes ServerSettings::maxBulkBytesPerSecond from the next tick on (0 = no cap; an
        /// admin's knob, and tests that hold a chunk's data back).
        void SetMaxBulkBytesPerSecond(double bytesPerSecond);
        const ServerStats& Stats() const;
        const std::deque<CastRecord>& Casts() const;   ///< The most recent ServerSettings::castRecordLimit.
        /// @brief Events made but not yet handed to every joined peer, encoded (stage 7a: no join log).
        size_t BacklogBytes() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
