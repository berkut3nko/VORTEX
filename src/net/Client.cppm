module;

#include <glm/glm.hpp>

#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

export module vortex.net:client;

import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;
import :transport;
import :wire;
import :messages;
import :prediction;
import :session;
import :entities;
import :sync;

export namespace vortex::net {

    struct ClientSettings {
        Token token{};
        /// Hello::buildHash to send; 0 = BuildHash of the default world materials (what
        /// sim::Simulation::Initialize registers). Attach checks the simulation against it.
        uint64_t buildHash = 0;
        /// Keep the applied operations for the owner to show (TakePresentations -> sim::PresentBlast).
        bool presentation = false;
        /// Inputs per packet at most (the newest unacknowledged ones) and one packet every this many
        /// ticks (networking.md: 30 Hz packets, at most 8 inputs).
        uint32_t maxInputsPerPacket = 8;
        uint32_t inputSendInterval = 2;
        ClockSettings clock;
        /// Reconciliation (networking.md "Player movement"): an error below `ignoreError` metres is
        /// left alone; above it the client restores the server's state and replays its inputs, and
        /// blends the visual jump out over `blendTicks`; above `snapError` it snaps.
        float ignoreError = 0.02f;
        float snapError = 1.5f;
        uint32_t blendTicks = 6;
        /// A snapshot whose events have not arrived this many ticks (250 ms) after it did is given
        /// up: counted, no replay (the newest snapshot whose events are applied is compared meanwhile).
        uint32_t eventWaitTicks = 15;
        /// A joiner starts predicting from a snapshot at most this many ticks older than the newest
        /// it received (review of stage 6: one that became ready after a long catch-up of the join log
        /// is that far in the past); it waits for a fresher one, but not longer than startWaitTicks.
        uint32_t startFreshTicks = 8;
        uint32_t startWaitTicks = 120;
        /// Attach refuses a world that leaves fewer free chunk-store slots than this after loading
        /// the region (0 = no check). A client whose store is capped below the region's cell count
        /// (MakeSimulationConfig's `storeLimit`: the game's GPU store) needs room for what a session
        /// adds: chunks a carve materialises or decodes, its own rubble models (networking.md
        /// "Stage 5 as built").
        uint32_t minFreeStoreSlots = 0;
        /// Chunk sync (stage 7a): the player starts once every chunk within `startRadius` metres of it
        /// is current and the entity baseline has ended; Unsynced chunks are requested only while the
        /// store has more than `requestReserveSlots` free; the op ring's bounds.
        float startRadius = 32.0f;
        uint32_t requestReserveSlots = 64;
        size_t ringOps = 1024;
        size_t ringBytes = size_t{1} << 20;
        /// The widest window (chunks, x and z) this client may hold (stage 7a M-b): 0 = the region's
        /// window, fixed (every region fits). When the region's window is wider
        /// (sim::ClientWindowMoves) the client's world is `windowChunks` x the region's rows x
        /// `windowChunks`, follows its player (RunTick; the game: the engine's window follows the
        /// camera) and is kept in sync chunk by chunk (WindowUpdate on every move). At most
        /// kMaxClientWindowChunks.
        int32_t windowChunks = 0;
        /// Bulk messages (ChunkData, BaselineEntity) installed per Poll at most, in arrival order; the
        /// rest wait for the next Poll (review of stage 7a: after the game's 3 s Attach hundreds arrive
        /// at once). A count, not a time: runs stay reproducible. 0 = no limit.
        uint32_t maxBulkPerPoll = 64;

        // Stage 7b: the checks.
        /// Despawned NetIds kept (a FIFO; review: stage 6's set grew for the session). An evicted one
        /// can only let a stale answer re-create a dead copy, which the next EntityCheck finds and the
        /// list removes (networking.md "Stage 7b plan", choice 11).
        size_t maxTombstones = 4096;
        /// After this many entity lists asked for in a row without a matching check in between, the
        /// next request waits `entityListBackoffTicks`, then three times that (server ticks): a client
        /// that cannot make a copy (bodies full) does not ask every 2 s.
        uint32_t entityListsBeforeBackoff = 3;
        uint32_t entityListBackoffTicks = 600;
        /// Wraps the region's generator (MakeGenerator, and the probe hashes at the Welcome): tests and
        /// vortex_server's broken bots (--bot-broken-chunk) give a client another generator. Empty = none.
        std::function<std::shared_ptr<voxel::IWorldGenerator>(std::shared_ptr<voxel::IWorldGenerator>)> wrapGenerator;
        /// The world map this client bakes from its own files (land.md "Decision" 3): called once, when a Welcome
        /// names a land region; it may wait for a bake started earlier. The client compares its baked map hash with
        /// the server's (sim::CheckRegionLand) and fails with the reason when they differ. Unset: a land region fails.
        std::function<std::shared_ptr<const voxel::LandMap>()> landMap;
        /// Tests: a live entity event (EntitySpawn of a NetId, EntityDespawn, EntityRest, ObjectModel,
        /// EntityWake) for which this returns true is dropped as if it never came (an EntityCheck
        /// mismatch to repair). Empty = none.
        std::function<bool(uint32_t seq, const Event& event)> testDropEntityEvent;
        /// Server terrain mode (stage 7b M-b): a client whose probe chunks differ from the server's at
        /// the Welcome (with this on), or whose HashChecks found this many distinct pristine chunks its
        /// generator made differently, stops generating the server's terrain and receives its whole
        /// window from the server (TerrainAck, ChunkData), for the rest of the session.
        bool terrainModeOnProbeMismatch = true;
        uint32_t pristineFailuresForTerrainMode = 3;
    };

    /// @brief Why a client is in server terrain mode (stage 7b).
    enum class TerrainModeReason : uint8_t {
        None,             ///< it generates the server's terrain itself
        Probes,           ///< probe chunks differed at the Welcome: from the first WindowUpdate on
        PristineChecks,   ///< HashChecks found ClientSettings::pristineFailuresForTerrainMode pristine chunks it made differently
    };

    enum class ClientState : uint8_t {
        Connecting,   ///< waiting for the transport's Connected
        Joining,      ///< Hello sent
        Welcomed,     ///< the region is known: build a Simulation from it and Attach
        Playing,      ///< attached: events are applied as they arrive
        Refused,      ///< the server said no (RefusalReason)
        Closed,       ///< disconnected
        Failed,       ///< a malformed message, a gap in the stream, an invalid region, an ack it did not ask for (logged; it disconnects)
    };

    struct ClientStats {
        uint64_t eventsApplied = 0;
        uint64_t opsApplied = 0;
        uint64_t lightsSpawned = 0;
        uint64_t duplicateEvents = 0;     ///< opSeq already applied: ignored
        uint64_t eventBytes = 0;          ///< Event batch bytes received
        uint32_t snapshotsReceived = 0;
        uint32_t snapshotsStale = 0;      ///< older than one already received (reordered or duplicated): only their margin is ignored too
        uint32_t malformedSnapshots = 0;  ///< state-channel messages that are not a valid snapshot (ignored)
        /// Chunks the World skipped although chunk sync held them current (not loaded after all): they
        /// catch up when they load. 0 in a fixed window (loaded whole before anything is applied).
        uint32_t skippedChunks = 0;
        /// Chunks an operation found without a store slot (they became Unsynced: requested whole once
        /// slots free). 0 with a store sized for the window.
        uint32_t unsyncedChunks = 0;
        uint32_t probeMismatches = 0;     ///< Probe chunks this client generates differently (stage 7: server terrain mode)

        // Replicated objects (stage 6).
        uint32_t copiesMade = 0;          ///< copies of server bodies made (spawns and Detach pieces with a NetId)
        uint32_t copiesFailed = 0;        ///< ... that could not be made (store or bodies full): the entity check sees it
        uint32_t copiesLost = 0;          ///< ... removed by this scene itself (fell out of the window first): the entity check sees it
        uint32_t despawns = 0;
        uint32_t rests = 0;
        uint32_t staleRests = 0;          ///< EntityRest older than a sample already taken: the copy keeps moving
        uint32_t wakes = 0;               ///< EntityWake events applied
        uint32_t models = 0;              ///< ObjectModel events applied
        uint32_t fragments = 0;           ///< netId 0 spawns (client rubble when presenting)
        uint32_t toRubble = 0;            ///< copies kept as local rubble (EntityDespawn{ToRubble}, presenting)
        uint32_t ignoredEntityEvents = 0; ///< events for unknown or tombstoned NetIds (ignored)
        // [mech-1] Mechanisms and holding.
        uint32_t mechanismMoves = 0;      ///< MechanismMove events applied
        uint32_t mechanismLateEvents = 0; ///< ... that came after their segment's start (the lead was short)
        uint32_t mechanismCorrections = 0;   ///< corrections where a mechanism's travel meets the player's reach
        uint32_t mechanismsRepaired = 0;  ///< mechanism copies the entity list found different (asked for again)
        uint32_t holdsRepaired = 0;       ///< held copies the entity list found different (asked for again)
        uint32_t pickupsPredicted = 0, dropsPredicted = 0;
        uint32_t holdsConfirmed = 0;      ///< the own EntityHold events
        uint32_t holdReleases = 0;        ///< the own EntityRelease events
        uint32_t holdRefused = 0;         ///< predicted pick-ups the server did not make (undone)
        uint64_t bodySamples = 0;         ///< snapshot samples taken
        uint64_t bodySamplesIgnored = 0;  ///< ... ignored: no copy, or not newer than its rest or newest sample, or dropped waiting
        uint32_t heldUnderPlayer = 0;     ///< ticks a moving copy stayed solid because the own player stands on it

        // Chunk sync and the join (stage 7a).
        uint64_t bulkBytes = 0;           ///< bulk messages received
        uint32_t chunkData = 0;           ///< ChunkData received
        /// Stage 8 (O3, re-sent edge chunks): ChunkData for a coordinate this client had received before
        /// (any reason: it changed, left the window and came back, a repair), and those whose RLE bytes
        /// equal the last ones received for it (sent again unchanged: the waste a "have" list would save).
        uint32_t chunkDataRepeats = 0, chunkDataUnchanged = 0;
        uint64_t chunkDataRepeatBytes = 0, chunkDataUnchangedBytes = 0;
        uint32_t baselineEntities = 0;    ///< BaselineEntity received (the join's baseline and model answers)
        uint32_t baselineDiscarded = 0;   ///< ... for a tombstoned NetId or one with a copy already
        uint32_t heldEntityEvents = 0;    ///< entity events held for a NetId whose copy did not exist yet
        uint32_t droppedHeldEvents = 0;   ///< ... dropped (at or below the copy's base, or never claimed)
        uint32_t staleEntityEvents = 0;   ///< live entity events at or below their copy's base (the baseline holds them)
        uint32_t chunkRequests = 0, modelRequests = 0;   ///< coordinates / NetIds asked for
        /// Server ticks from the Welcome to the prediction's start (32 m current, baseline ended), to
        /// the join's data being in (the baseline ended and no chunk of the window waiting for the
        /// server: none without a base, awaiting data, requested or unsynced; chunks may still be
        /// generating here), and to the first Synced() (every chunk current: for a window that follows a
        /// walking player that is only once it stops); 0 until then.
        uint32_t joinStartTicks = 0, joinDataTicks = 0, joinSyncTicks = 0;
        /// Server ticks from the Welcome to the baseline's near-set marker (BaselineNear) and to its
        /// end (BaselineEnd) being processed; 0 until then (review of stage 7a).
        uint32_t baselineNearTicks = 0, baselineEndTicks = 0;
        /// The same on the wall clock (steady_clock, logged only: a server catching up after its
        /// preload runs ticks faster than real time).
        double joinStartSeconds = 0.0, joinDataSeconds = 0.0, joinSyncSeconds = 0.0;
        /// WindowUpdates sent (the first at Welcome), and those that reset the window (the world moved
        /// twice between two checks: every chunk of it counts as entering again).
        uint32_t windowUpdates = 0, windowResets = 0;
        uint32_t maxStoreUsed = 0;        ///< the most chunk-store slots in use at a Poll after Attach
        /// Model answers older than entity events this client dropped for that NetId (a held-event
        /// overflow): discarded, the re-asked answer is newer (review of stage 7a).
        uint32_t staleAnswers = 0;
        /// Wall clock per Poll (logged only; review of stage 7a): chunk sync's housekeeping (window
        /// steps, load reports and their replays, requests) and the bulk installs (ChunkData with its
        /// ring replay, baseline entities), summed and the largest Poll; Polls counted.
        double syncSeconds = 0.0, maxSyncSeconds = 0.0;
        double bulkSeconds = 0.0, maxBulkSeconds = 0.0;
        uint64_t polls = 0;
        uint32_t maxBulkInPoll = 0;       ///< bulk messages processed in one Poll at most

        // Checks (stage 7b). The HashCheck entry counts are ChunkSync::Stats() (checkEqual, checkAhead, ...).
        uint32_t hashChecks = 0;          ///< HashChecks compared at their stream position
        uint32_t checksOutOfOrder = 0;    ///< HashChecks / EntityChecks / EntityLists not at applied == their opSeq (expected 0)
        uint32_t mismatchesModified = 0;  ///< HashCheck entries that differed, of chunks the server modified
        uint32_t mismatchesPristine = 0;  ///< ... of chunks the server never modified
        uint32_t pristineFailures = 0;    ///< distinct pristine chunks this client's generator made differently (terrain mode's trigger, M-b)
        uint32_t repairsRequested = 0;    ///< coordinates sent in ChunkRequests with reason Repair
        uint32_t entityChecks = 0;        ///< EntityChecks compared in full (count, ids, models)
        uint32_t entityChecksPartial = 0; ///< ... compared on count and ids only (model requests out: amendment 8)
        uint32_t entityChecksSkipped = 0; ///< ... skipped: the baseline open or a list asked for
        uint32_t entityChecksAhead = 0;   ///< ... skipped: entity data ahead of the check was processed (amendment 3)
        uint32_t entityMismatches = 0;    ///< EntityChecks that did not match
        uint32_t entityListRequests = 0;  ///< EntityListRequests sent
        uint32_t entityListsBackedOff = 0;   ///< mismatches that sent no request (the back-off)
        uint32_t entityLists = 0;         ///< EntityLists applied
        uint32_t copiesRestored = 0;      ///< listed NetIds without a copy: asked for (ModelRequest)
        uint32_t copiesReplaced = 0;      ///< copies with another model than the list's: removed and asked for
        uint32_t copiesDropped = 0;       ///< copies the list does not have (as old as the list): removed and tombstoned
        uint32_t tombstonesEvicted = 0;   ///< despawned NetIds forgotten (maxTombstones)
        uint32_t testDroppedEvents = 0;   ///< entity events dropped by ClientSettings::testDropEntityEvent
        // [mag-2] Zones and spells (magic.md "sim::ZoneSet").
        uint32_t zonesApplied = 0;        ///< ZoneStart events (and a joiner's baseline zones) put into the simulation's zones
        uint32_t zonesIgnored = 0;        ///< ... refused: a known id, the records full, a cancel before the start
        uint32_t zoneCancels = 0;         ///< ZoneCancel events applied
        uint32_t castFx = 0;              ///< CastFx events received
        double checkSeconds = 0.0, maxCheckSeconds = 0.0;   ///< wall clock of the checks (logged only)

        // Server terrain mode (stage 7b M-b); the acks' outcomes are ChunkSync::Stats().
        uint32_t terrainModeTick = 0;     ///< server tick (newest snapshot) terrain mode started at; 0 = at the Welcome or never
        uint32_t terrainAcks = 0;
        uint64_t terrainAckBytes = 0;
        uint32_t terrainChunkData = 0;    ///< ChunkData received while in terrain mode
        uint64_t terrainChunkBytes = 0;
    };

    /// @brief How the own player's prediction went (networking.md stage 4: "correction rate logged").
    struct PredictionStats {
        uint64_t ticks = 0;               ///< Ticks predicted (StepPlayer calls that moved the player or held it).
        uint64_t heldTicks = 0;           ///< ... held by the gate (terrain without a collider)
        uint64_t inputPackets = 0, inputsSent = 0;
        uint64_t compared = 0;            ///< Snapshots compared with the prediction.
        uint64_t within = 0;              ///< ... whose error was under ignoreError: left alone
        uint64_t corrections = 0;         ///< ... restored and replayed (error at or above ignoreError)
        uint64_t snaps = 0;               ///< ... of them above snapError: no blend
        /// Errors by size: < 1 mm, < 2 cm, < 5 cm, < 20 cm, < 1.5 m, more.
        std::array<uint64_t, 6> errorBuckets{};
        float maxError = 0.0f, lastError = 0.0f;
        uint64_t replayTicks = 0;         ///< StepPlayer calls of replays
        uint32_t maxReplayTicks = 0;
        double replaySeconds = 0.0, maxReplaySeconds = 0.0;   ///< wall clock (logged only)
        uint64_t waitedForEvents = 0;     ///< Snapshots compared only after their events arrived
        uint64_t eventTimeouts = 0;       ///< Snapshots given up eventWaitTicks after they arrived (not compared)
        uint64_t staleStarts = 0;         ///< Ready snapshots a joiner did not start predicting from: too far behind the newest
        uint64_t tooOld = 0;              ///< Snapshots older than the kept history (not compared)
        uint64_t behind = 0;              ///< Snapshots at or past the tick being predicted: the state was taken as it is
        uint32_t clockJumps = 0, clockJumpTicks = 0;
        uint32_t hardResyncs = 0, resyncHoldTicks = 0;
        uint32_t castsRejected = 0;       ///< Casts the server reported it did not run (Snapshot::castRejected, stage 5)
        uint64_t respawns = 0;            ///< [wp-7] lives started after the first (a new lifeSeq): never a correction
        uint64_t deathsSeen = 0;          ///< [wp-7] deaths of the own player (the owner block said dead)
        uint64_t bleedTicks = 0;          ///< Ticks run while the clock bled a surplus
        uint64_t catchUpTicks = 0;        ///< Ticks run while the clock made up a deficit
        /// Chunk sync (stage 7a): ticks the own player was held because a chunk of its reach was not
        /// current (inputs still sent), snapshots taken as they are meanwhile, and the replays after.
        uint64_t syncHeldTicks = 0, syncTaken = 0, syncResumes = 0;
        uint64_t startGateDrops = 0;      ///< Polls that dropped the waiting snapshots: the start gate was closed
        int32_t lastMargin = 0, minMargin = 0, maxMargin = 0;
        bool anyMargin = false;
        /// Corrections per minute of prediction.
        double CorrectionsPerMinute(double tickSeconds) const {
            return ticks == 0 ? 0.0 : static_cast<double>(corrections) * 60.0 / (static_cast<double>(ticks) * tickSeconds);
        }
    };

    /// @brief [wp-7] The own player's fight as the server's owner block and Welcome say it (fight.md "Wire").
    struct FightView {
        sim::GameMode mode = sim::GameMode::Sandbox;
        uint8_t health = 100, maxHealth = 100, charges = 0, maxCharges = 0, chargeTicks = 0;
        uint16_t chargeTicksFull = 0;           ///< Welcome chargeTicks (for the pip's fill)
        uint16_t respawnTicksFull = 0;          ///< Welcome respawnTicks
        bool dead = false, protectedNow = false, spectator = false;
        uint8_t respawnTicks = 0, lifeSeq = 0, hits = 0;
        uint32_t tick = 0;                      ///< the snapshot it is from
        bool Active() const { return mode != sim::GameMode::Sandbox; }
    };

    /**
     * @brief A session client (docs/design/networking.md stages 3-4): handshake, then the region
     * from `Welcome`, then the opSeq stream applied in order with the same sim::ApplyOp the server
     * ran, and the own player predicted with sim::StepPlayer and reconciled with the server's
     * snapshots. Terrain is never predicted and no operation is ever sent. Replicated objects
     * (stage 6) are kinematic copies: made from EntitySpawn or from the voxels a Detach with a NetId
     * removed, moved every tick (StepPlayer) to their snapshot samples drawn in the past
     * (RemoteBodies) in Layer::Rubble, put at their EntityRest pose in Layer::Dynamic (players
     * collide with them there), re-modelled by ObjectModel, removed by EntityDespawn (kept as local
     * rubble for ToRubble when presenting); netId 0 spawns are local rubble when presenting.
     * @details The owner creates the client's Simulation once the state is Welcomed, from
     * MakeSimulationConfig and MakeGenerator (never from its own settings: the voxel size, window
     * and height range are the session's), and gives it to Attach. Events that arrive before are
     * kept. With `presentation` the applied operations are grouped per cast for sim::PresentBlast.
     *
     * Driving it, per frame of the owner: `n = TicksDue(seconds)` (the clock, kept ahead of the
     * server), then n ticks of: the simulation's streaming phase, `Poll` (events, snapshots,
     * reconciliation), `StepPlayer(controls)` (the predicted tick; it sends the inputs), the
     * simulation's physics and lifetime phases. `RunTick` is that tick on the attached simulation.
     */
    class Client {
    public:
        Client(ClientSettings settings, ITransport& transport);
        ~Client();
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        /// @brief Receives and applies whatever arrived; compares the newest snapshot whose events
        /// are applied with the prediction and corrects it.
        void Poll();

        /// @brief The clock: ticks to run now for `seconds` of the owner's time (rate 1 before the
        /// prediction starts; then kept ahead of the server: jumps, a 2 % bleed, holds), at most
        /// `maxTicks`: what is over is kept for the next calls (TickClock), so an owner that can run
        /// only so many ticks a frame (the engine: 8) asks for that many and must run every tick it
        /// is given - a tick handed out and not run is lost to the clock (review of stage 5).
        uint32_t TicksDue(double seconds, uint32_t maxTicks = 30);
        /**
         * @brief One predicted tick of the own player, between the simulation's streaming and
         * physics phases: `controls` (its `tick` is ignored) become the input of the next tick,
         * sim::StepPlayer moves the player, the input and the state are kept for replays, and every
         * inputSendInterval ticks the unacknowledged inputs are sent. Before the first snapshot
         * (the server's state of the player) nothing happens.
         */
        sim::PlayerStepResult StepPlayer(const sim::PlayerInput& controls);
        /// @brief One whole tick on the attached simulation: streaming (colliders around the own
        /// player), Poll, StepPlayer, physics, lifetimes. Before Attach: Poll only.
        sim::PlayerStepResult RunTick(const sim::PlayerInput& controls);

        bool Predicting() const;
        /// @brief The predicted state of the own player (after the last StepPlayer).
        const sim::PlayerState& Player() const;
        /// @brief Where to draw the own player: the prediction plus the correction being blended out.
        glm::vec3 PlayerViewPosition() const;
        /// @brief The server tick the next StepPlayer predicts.
        uint32_t PredictTick() const;
        /// @brief The other players at the server time this client draws them (in the past):
        /// net::ServerTime, advanced by TicksDue, so it only moves forward.
        std::vector<RemotePlayerView> RemotePlayers() const;
        /// @brief The server's word on this player's newest cast (the newest snapshot's owner block):
        /// the input tick of the cast edge it took (0 = none yet) and, if it was not run, why.
        struct CastStatus {
            uint32_t tick = 0;
            CastRejection rejected = CastRejection::None;
        };
        CastStatus LastCast() const;
        /// @brief [wp-7] The fight (fight.md "Wire"): the own player's (newest snapshot), the roster and the round
        /// (newest message), and the kill notices since the last call (arrival order, at most 32 kept).
        const FightView& Fight() const;
        const Roster& RosterOf() const;
        const RoundUpdate& RoundOf() const;
        std::vector<KillNotice> TakeKillNotices();
        /// @brief [mag-2] The owner block's magic state of the newest snapshot (the HUD's spell bar, mana bar and element
        /// panel): [mag-8]'s mana (1/60 mana) and spell gap (ticks) and the attunement byte (Snapshot::attune).
        struct MagicView {
            uint16_t mana = 0;
            uint8_t spellGap = 0;
            uint8_t attune = 0;
            uint32_t tick = 0;            ///< the snapshot it is from
        };
        const MagicView& Magic() const;
        /// @brief [mag-2] Sends Attune{craftA, craftB} (sim::Element values): the server takes it while this player is
        /// dead or before its first spawn, for the next life.
        void SendAttune(uint8_t craftA, uint8_t craftB);
        /// @brief [mag-8] The gesture of the next Spell edge: it is sent (SpellGesture, reliable) with the first predicted
        /// input that holds a Spell edge, paired by that input's tick, then cleared. The input's spell must be the one
        /// the gesture's form gives (the server refuses it BadGesture). A new call replaces a gesture not sent yet.
        void SetSpellGesture(const sim::SpellGesture& gesture);
        /// @brief [mag-2] The CastFx events applied since the last call (stream order; ClientSettings::presentation
        /// only, at most 64 kept).
        std::vector<CastFx> TakeCastFx();
        /// @brief The roster's team of `slot` (0 when it is not listed).
        sim::TeamId TeamOf(uint8_t slot) const;
        /// @brief The session's player tuning (from Welcome).
        const sim::PlayerTuning& Tuning() const;
        const PredictionStats& Prediction() const;
        const TickClock& Clock() const;

        ClientState State() const;
        RefuseReason RefusalReason() const;
        const sim::RegionDesc& Region() const;            ///< From Welcomed on.
        glm::dvec3 Spawn() const;
        uint8_t Slot() const;
        /// @brief `base` with the session's world: memory only, the region's voxel size and height
        /// range, the client's window (sim::ClientWorldConfig: the region's, or a `windowChunks` window
        /// that follows the player), load reports, a store of at least sim::ClientChunkCapacity slots
        /// (every cell of that window), the server's tick length and gravity
        /// (Welcome), the session's character mask (sim::kSessionCharacterLayers: terrain and Dynamic,
        /// as on the server) and own rubble in physics::Layer::Rubble (SceneConfig::rubbleLayer: it
        /// never blocks the predicted player). Valid from Welcomed on.
        /// @param storeLimit 0, or the most slots this client's store may have (the game's GPU chunk
        /// buffer mirrors the store): the store is then min(region cells, limit), and the region fits
        /// only if its loaded window leaves ClientSettings::minFreeStoreSlots free (Attach checks).
        sim::SimulationConfig MakeSimulationConfig(sim::SimulationConfig base = {}, uint32_t storeLimit = 0) const;
        /// @brief The session's generator for this client's world (from Welcomed on): a SessionGenerator
        /// around the region's generator (ClientSettings::wrapGenerator applied), which stops
        /// generating the server's terrain once server terrain mode starts (stage 7b). Every call
        /// returns the same generator.
        std::shared_ptr<voxel::IWorldGenerator> MakeGenerator() const;
        /// @brief Server terrain mode (stage 7b): this client receives its whole window from the
        /// server instead of generating it (sticky for the session).
        bool TerrainMode() const;
        TerrainModeReason TerrainModeWhy() const;
        /// @brief The generator MakeGenerator hands out (null before the Welcome).
        std::shared_ptr<const SessionGenerator> ClientGenerator() const;
        /**
         * @brief Takes the simulation the events are applied to: refused (false, logged; the state
         * stays Welcomed) when it is not initialised, has a save directory (a session world is
         * memory only), or its voxel size, window, height range, material table, tick length,
         * gravity or character mask differ from the session's. Loads the whole window (World::LoadBlocking;
         * a moving window: placed at the spawn, its surface and the chunks near the spawn) and refuses it when the window did
         * not land where it should, the store filled, or a loaded probe chunk is not what this client's
         * generator makes (another generator, or edited before Attach), or it leaves fewer than
         * ClientSettings::minFreeStoreSlots free. Then applies the events received so far.
         * @param whileLoading Called between streaming steps while the window loads (the game polls
         * its transport there, so a long load does not look like a dead peer); without it the
         * window is loaded in one blocking call.
         */
        bool Attach(sim::Simulation& simulation, const std::function<void()>& whileLoading = {});

        uint32_t AppliedOpSeq() const;    ///< The last event applied.
        uint32_t ServerOpSeq() const;     ///< The last event the server said it made (status or batch).
        uint32_t JoinOpSeq() const;       ///< The join point (Welcome): the stream started after it.
        uint32_t JoinTick() const;        ///< The server tick of its Welcome (with the slot it names this client: Server::ClientJoinPoint).
        /// @brief Every chunk of the window current, no chunk or model request out, the entity baseline
        /// ended and no entity event held: Hash() is comparable with the server's (at the same opSeq).
        bool Synced() const;
        /// @brief The box Hash() covers: the window intersected with the server's (the region's
        /// window for a fixed-window client).
        SyncBox SyncWindow() const;
        /// @brief The whole window of the last WindowUpdate (the world's; SyncWindow is its part inside
        /// the server's window).
        SyncBox Window() const;
        /// @brief The window follows the player (sim::ClientWindowMoves with ClientSettings::windowChunks).
        bool WindowMoves() const;
        /// @brief Chunks within ClientSettings::startRadius of the player (the spawn before the
        /// prediction starts) that are not current yet: what the start gate waits for (the game's
        /// JOINING line). 0 once predicting.
        uint32_t ChunksToStart() const;
        const ChunkSync& Sync() const;
        const ClientStats& Stats() const;
        /// @brief This client's copies of the server's replicated objects (the entity check).
        std::vector<EntityInfo> Entities() const;
        /// @brief True when `entity` is this client's copy of a server object (what a cast can hit;
        /// its own rubble is not).
        bool IsCopy(scene::Entity entity) const;
        /// @brief The snapshot samples of the copies (largest sample gap per body: RemoteBodies::MaxGap).
        const RemoteBodies& Bodies() const;
        /// @brief [mech-1] The own held object's NetId (the server's or a predicted pick-up); 0 none.
        uint32_t HeldNetId() const;
        /// @brief [mech-1] A pick-up or a drop waits for the server's answer.
        bool HoldPending() const;
        /// @brief [mech-1] The own held object is pinned (the HUD's amber prompt).
        bool HeldPinned() const;
        /// @brief [mech-1] "Occupied", "Dropped - blocked", "Dropped - hit" for about 1 s; empty otherwise.
        const std::string& HoldNotice() const;
        struct UseStatus {
            uint32_t tick = 0;
            sim::UseResult result = sim::UseResult::None;
        };
        /// @brief [mech-1] The newest snapshot's owner block: the last Use edge's input tick and the server's answer.
        UseStatus LastUse() const;
        /// @brief The copy of a replicated server object (kNoEntity without one).
        scene::Entity CopyOf(uint32_t netId) const;
        /// @brief [mech-1] The use target rules of this client for sim::FindUseTarget (the game's prompt): a copy is
        /// holdable when its server body is Dynamic and nobody holds it.
        sim::UseTargetHooks UseHooks() const;
        /// @brief The own predicted player's character (its ground body: what the own feet stand on).
        physics::CharacterId Character() const;
        /// @brief The applied live operations (after JoinOpSeq) since the last call, one BlastResult per
        /// cast (a carve and the Detach / ClearMask after it; `deferred` when there is no carve).
        /// Empty without `presentation`; the join log is never presented.
        std::vector<sim::BlastResult> TakePresentations();
        /// @brief WorldHash over SyncWindow(); every chunk there that is not current counts in
        /// `notLoaded` (incomplete: not comparable).
        voxel::WorldHashResult Hash() const;
        /// @brief Despawned NetIds remembered now (at most ClientSettings::maxTombstones).
        size_t Tombstones() const;
        /// @brief The distinct chunks a HashCheck found different here (any class; stage 8: a bot with an
        /// injected fault must have found exactly its own).
        std::vector<glm::ivec3> MismatchedChunks() const;
        /// @brief Tests: this client loses its copy of `netId` (as a copy its own scene removed:
        /// counted in copiesLost); false without such a copy. The next EntityCheck sees it missing.
        bool TestDestroyCopy(uint32_t netId);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

    /// @brief The client's check counters as one line of text (stage 7b; vortex_server's and the game's
    /// `Checks:` lines, which tools/session-check.sh reads: "N hash mismatches", "N entity
    /// mismatches", "N out of order").
    std::string DescribeChecks(const Client& client);
    /// @brief One line on server terrain mode (stage 7b): "no", or why and since when, the acks, the
    /// data received and the ticks the player was held (vortex_server's and the game's `Terrain:` lines).
    std::string DescribeTerrainMode(const Client& client);
}
