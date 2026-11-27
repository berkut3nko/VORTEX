module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module vortex.net:client_impl;

import vortex.core;
import vortex.math;
import vortex.fx;
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
import :client;

// The client's state and the declarations of its member functions (an internal partition: nothing outside
// vortex.net sees it). docs/design/rules.md "File map" lists which unit defines which function. [wp-6]
namespace vortex::net {

    namespace detail {
        inline bool SameWorldMaterials(const voxel::WorldMaterials& a, const voxel::WorldMaterials& b) {
            sim::RegionDesc x, y;
            x.terrain.materials = a;
            y.terrain.materials = b;
            return x == y;
        }

        /// Ticks of inputs and predicted states a client keeps (networking.md: 128).
        inline constexpr uint32_t kHistoryTicks = 128;

        inline ClockSettings ClockOf(const ClientSettings& s) {
            ClockSettings c = s.clock;
            c.packetTicks = static_cast<int32_t>(std::max(1u, s.inputSendInterval));
            return c;
        }

        /// A snapshot waiting for its events (networking.md "Player movement").
        struct WaitingSnapshot {
            Snapshot snap;
            uint32_t age = 0;             ///< client ticks it has waited
        };
        /// Snapshots kept while their events lag (about eventWaitTicks / 2 are ever waiting).
        inline constexpr size_t kMaxWaitingSnapshots = 32;

        /// A snapshot's body samples waiting for the events the snapshot was built after (review of
        /// stage 6): a sample is taken only once every spawn, model, rest and wake before it is applied.
        struct WaitingSamples {
            uint32_t tick = 0;
            uint32_t lastOpSeq = 0;
            std::vector<BodySample> bodies;
        };

        /// What the events channel carries in order after the Welcome: the stream and the window acks
        /// (an ack is processed in stream order: with every event before it applied).
        /// Stage 7b: the checks and the entity list ride the same channel right after the events up to
        /// their opSeq, so a client processes them exactly at that stream position.
        /// Stage 7b M-b: a terrain-mode window's TerrainAck takes a WindowAck's place.
        using StreamItem = std::variant<EventBatch, WindowAck, HashCheck, EntityCheck, EntityList, TerrainAck>;

        /// Entity events a client holds for a NetId whose copy does not exist yet (stage 7a).
        inline constexpr size_t kMaxHeldPerId = 64;
        inline constexpr size_t kMaxHeldEvents = 4096;

        /// The NetId of an entity event about one replicated body (not a spawn or a despawn).
        inline uint32_t EntityEventNetId(const Event& event) {
            if (const EntityRest* e = std::get_if<EntityRest>(&event)) return e->netId;
            if (const EntityWake* e = std::get_if<EntityWake>(&event)) return e->netId;
            if (const ObjectModel* e = std::get_if<ObjectModel>(&event)) return e->netId;
            if (const MechanismMove* e = std::get_if<MechanismMove>(&event)) return e->netId;
            if (const EntityHold* e = std::get_if<EntityHold>(&event)) return e->netId;
            if (const EntityRelease* e = std::get_if<EntityRelease>(&event)) return e->netId;
            return 0;
        }

        struct HistoryEntry {
            bool valid = false;
            uint32_t tick = 0;
            sim::PlayerInput input;
            sim::PlayerState state;      ///< after the tick
            bool spellMove = true;       ///< [mag-2] a Spell edge of this tick could start the own move (kept for replays)
        };

        /// [mag-2] CastFx events kept for the owner's presentation (Client::TakeCastFx), at most this many.
        inline constexpr size_t kMaxCastFx = 64;
    }
    using namespace detail;

    struct Client::Impl {
        Impl(ClientSettings s, ITransport& t);

        ClientSettings settings;
        ITransport& transport;
        voxel::WorldMaterials ids;
        uint64_t buildHash = 0;
        ClientState state = ClientState::Connecting;
        RefuseReason refusal = RefuseReason::Malformed;
        PeerId server = 0;
        Welcome welcome;
        sim::PlayArea playArea;             // sim::PlayAreaOf(welcome.region, welcome.player): StepPlayer's area
        sim::Simulation* simulation = nullptr;
        std::deque<StreamItem> pending;   // received before Attach (or not yet applied)
        std::deque<std::vector<uint8_t>> pendingBulk;   // bulk messages not processed yet
        // Chunk sync (stage 7a).
        ChunkSync sync;
        SyncBox syncBox;                  // the window this client asked for, inside the server's
        uint32_t windowSeq = 0;
        bool moving = false;              // the window follows the player (stage 7a M-b)
        uint64_t windowRevision = 0;      // World::WindowRevision of the window last sent
        uint32_t unsyncedLogged = 0;      // ChunkSync unsynced count last logged
        uint32_t syncPolls = 0;
        std::chrono::steady_clock::time_point welcomed;   // the join's wall clock (logged only)
        double SinceWelcome() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - welcomed).count(); }
        bool baselineEnded = false;
        bool baselineNear = false;        // BaselineNear (or BaselineEnd) arrived: the start gate may open
        std::unordered_map<uint32_t, uint32_t> staleBefore;   // NetId -> newest held event dropped (overflow)
        std::map<uint32_t, std::vector<std::pair<uint32_t, Event>>> held;   // NetId -> held entity events
        size_t heldCount = 0;
        std::unordered_set<uint32_t> asked;          // NetIds of ModelRequests out
        std::vector<uint32_t> modelsToAsk;
        uint32_t applied = 0;
        bool anyTerrainOp = false;
        uint32_t lastTerrainOpTick = 0;   // nextTick when the newest terrain operation was applied (LogCorrection)
        uint32_t serverLast = 0;
        ClientStats stats;
        std::unordered_map<uint32_t, scene::Entity> entities;   // NetId -> light entity
        /// A kinematic copy of a replicated server object (stage 6).
        struct Copy {
            scene::Entity entity = scene::kNoEntity;
            bool resting = false;
            uint64_t modelHash = 0;
            uint32_t base = 0;            ///< the opSeq its state is as of (its spawn, Detach, model or baseline)
            uint8_t motion = 2;           ///< [mech-1] the server body's physics::MotionType (only a Dynamic one is holdable)
            bool mechanism = false;       ///< [mech-1] posed from its scene::Mechanism: never sampled, rested or woken
            bool held = false;            ///< [mech-1] a hold is applied (the server's, or this client's predicted pick-up)
            bool holdConfirmed = false;   ///< ... and it is the server's (EntityHold): the checks' HeldTerm
            uint8_t holder = 0;
            uint16_t relYaw = 0;
            uint32_t holdSince = 0;
        };
        std::unordered_map<uint32_t, Copy> copies;               // NetId -> copy
        std::unordered_set<uint32_t> tombstones;                 // despawned NetIds (a later spawn of one is discarded)
        std::deque<uint32_t> tombstoneOrder;                     // ... oldest first (bounded: maxTombstones, stage 7b)
        // The checks (stage 7b).
        uint32_t entityAsOf = 0;          // the newest asOf of entity data processed from bulk (amendment 3)
        bool listOpen = false;            // an EntityListRequest is out, its EntityList not processed
        uint32_t listsInARow = 0;         // lists asked for since the last matching EntityCheck
        uint32_t lastListTick = 0;        // server tick (newest snapshot) of the last request
        bool listToSend = false;
        uint32_t listCheckSeq = 0;        // the check the request is about
        std::unordered_set<glm::ivec3, ChunkCoordHash> pristineFailures;   // distinct chunks this client generated differently
        std::unordered_set<glm::ivec3, ChunkCoordHash> mismatched;         // distinct chunks a HashCheck found different (stage 8: the soak's verdict)
        std::unordered_map<glm::ivec3, uint64_t, ChunkCoordHash> chunkDataSeen;  // coordinate -> FNV-1a of the last ChunkData's RLE (stage 8, O3)
        // Server terrain mode (stage 7b M-b).
        std::shared_ptr<SessionGenerator> sessionGenerator;   // what MakeGenerator hands out (from the Welcome)
        bool terrainMode = false;
        TerrainModeReason terrainReason = TerrainModeReason::None;
        RemoteBodies bodies;
        std::deque<WaitingSamples> waitingSamples;               // snapshot samples whose events are not applied yet
        double bodyTime = 0.0;                                   // server tick the copies are moved to (per tick)
        bool bodyTimeStarted = false;
        std::vector<sim::BlastResult> presentations;
        bool groupOpen = false;
        // The sphere of the newest live cast shown: an object it broke (EntityDespawn{Broken}) bursts there.
        bool castShown = false;
        glm::dvec3 castCenter{0.0};
        float castRadius = 0.0f;
        std::vector<ProbeHash> ownProbes;   // the Welcome's probe chunks as this client's generator makes them

        // The own player (stage 4).
        physics::CharacterId character;
        sim::PlayerState player;
        bool predicting = false;
        uint32_t nextTick = 0;              // the tick the next StepPlayer predicts
        uint32_t ackTick = 0;               // the newest input the server has
        uint32_t sinceSend = 0;
        std::array<HistoryEntry, kHistoryTicks> history{};
        std::deque<WaitingSnapshot> waiting;   // snapshots not compared yet, oldest first
        bool anySnapshot = false;
        uint32_t newestSnapshot = 0;
        /// Server ticks since the Welcome as far as this client knows (its newest snapshot), at least 1.
        uint32_t TicksSinceWelcome() const { return newestSnapshot > welcome.tick ? newestSnapshot - welcome.tick : 1u; }
        ServerTime serverTime;              // where other players are drawn (only moves forward)
        TickClock clock;
        glm::vec3 offset{0.0f};             // visual error being blended out
        uint32_t blendLeft = 0;
        net::RemotePlayers remotes;
        PredictionStats prediction;
        Client::CastStatus lastCast;
        // [wp-7] The fight (fight.md "Wire"): taken on receipt (not by opSeq), the owner block from the newest snapshot.
        FightView fight;
        Roster roster;
        RoundUpdate roundState;
        std::deque<KillNotice> kills;
        bool lifeKnown = false;           ///< a lifeSeq was seen
        uint32_t lifeStartTick = 0;       ///< the snapshot tick of the current life (older snapshots are not compared)
        /// A new lifeSeq was seen at `lifeStartTick` and is taken by Reconcile, once a snapshot of the new life has its
        /// events applied (review of [wp-7]: the replay runs on the terrain the server stepped). Until then the player
        /// is not stepped, as while dead.
        bool respawnPending = false;
        /// The owner block of a new snapshot: a death stops the prediction (the owner's state is taken), a new
        /// lifeSeq is a respawn (recorded here; Reconcile takes the owner's state and replays the inputs since:
        /// never a correction).
        void OnOwnerFight(const Snapshot& snap);
        void Respawned(const Snapshot& snap);
        bool syncHeld = false;              // the reach holds a chunk that is not current: not moved
        bool needResume = false;            // replay the inputs after `lastTaken` once current again
        uint32_t lastTaken = 0;

        /// The session's tick (Welcome; the attached simulation has the same, Attach checks it).
        float Dt() const { return simulation ? simulation->FixedTimeStep() : welcome.tickSeconds; }

        /// The colliders a step from where the player is can touch hold every edit applied so far,
        /// whatever the workers have finished (the server does the same before its steps).
        void FreshColliders() { simulation->Terrain().FlushBox(sim::PlayerReach(player.position, welcome.player)); }

        /// Puts the character into `state` on up-to-date colliders.
        void Restore(const sim::PlayerState& state);

        /// Predicts tick `nextTick` with `input` and keeps it.
        /// The chunks of the own player's reach are current (stage 7a): a step over a chunk without a
        /// base (its pristine stand-in) would walk where the server's player does not.
        bool ReachCurrent() const;

        /// Every chunk within `startRadius` (3D, metres) of `feet` inside the window is current.
        bool NearCurrent(const glm::vec3& feet) const { return NotCurrentNear(feet, true) == 0; }

        /// Chunks within `startRadius` (3D, metres) of `feet` inside the window that are not current
        /// (with `first`: 0 or 1, it stops at the first).
        uint32_t NotCurrentNear(const glm::vec3& feet, bool first) const;

        sim::PlayerStepResult PredictTick(sim::PlayerInput input);

        void SendInputs();

        /// The first snapshot whose events are applied: the player is where the server has it, and
        /// the client runs ahead by the round trip plus the clock's target. The ticks in between
        /// are what the server simulates without inputs from here (it repeats none: the player
        /// stands), so they are predicted that way and never sent.
        void StartPrediction(const Snapshot& snap);

        /// networking.md "Player movement": the server's state at `snap.tick` against the prediction
        /// of that tick; at or above the threshold: restore, replay the later inputs, blend.
        void Reconcile(const Snapshot& snap);

        /// One line per correction, the first kLoggedCorrections of a session (G16's corrections are
        /// read from them: where, how far ahead, on the ground or not, after which terrain operation,
        /// how far from the play area's edge).
        void LogCorrection(const Snapshot& snap, const sim::PlayerState& predicted, float error);

        void OnSnapshot(const std::vector<uint8_t>& bytes);

        /// The newest snapshot whose events are applied is compared (older ones are superseded);
        /// one that has waited eventWaitTicks for its events since it arrived is given up and
        /// counted. The age is the oldest waiting one's, so a stream of newer snapshots that all
        /// wait cannot hide a stall (review of stage 4).
        void TryReconcile();

        /// The region's generator, wrapped when a test or a broken bot asks for it.
        std::shared_ptr<voxel::IWorldGenerator> Generator() const;

        /// A failed client leaves the session: its slot is freed on the server (until stage 7 there
        /// is no resync).
        void Fail(const std::string& why);

        void Leave();

        void OnWelcome(const std::vector<uint8_t>& bytes);

        /// WindowUpdate seq + 1 for `window`: cells that entered since the last one (all of them with
        /// `reset`) wait for its ack.
        void SendWindow(const SyncBox& window, bool reset);

        /// Server terrain mode (stage 7b): from now on the server's terrain is not generated here
        /// (the SessionGenerator answers Empty inside the server's window) and every WindowUpdate
        /// asks for the whole entering set (TerrainAck); data installs as content. Sticky.
        void StartTerrainMode(TerrainModeReason why);

        /// The world's window moved since the last WindowUpdate (stage 7a M-b): the next one. Runs at
        /// the start of every Poll, before anything is applied: the world moves only in streaming,
        /// between Polls. Two moves since the last check: a chunk may have left and come back (it was
        /// regenerated), so the update resets the whole window.
        void CheckWindow();

        void CloseGroup() { groupOpen = false; }

        void Present(const sim::WorldOp& op, sim::OpResult result, std::vector<voxel::VoxelEdit> edits);

        // --- replicated objects (stage 6) ---------------------------------------------------------

        physics::BodyId BodyOf(scene::Entity entity) const;

        /// The layer of a moving copy: Rubble (no predicted player meets it), unless the own player
        /// stands on it - then it stays Dynamic and carries the player, as the server keeps a moving
        /// body under a player solid (review of stage 6; Server TrackBodies).
        physics::Layer MovingLayer(physics::BodyId body) const;

        /// A despawned NetId: remembered (a later spawn or answer of it is discarded), at most
        /// maxTombstones of them, oldest forgotten first (stage 7b).
        void Tombstone(uint32_t netId);
        void Untombstone(uint32_t netId);

        /// A kinematic copy of a server object: Layer::Dynamic at rest (players collide with it),
        /// Layer::Rubble while it moves (no predicted player does).
        bool MakeCopy(uint32_t netId, std::shared_ptr<voxel::VoxelModel> model, const glm::vec3& position, const glm::quat& rotation,
                      bool resting, uint32_t base, uint8_t motion = 2);

        // --- mechanisms and holding ([mech-1], docs/design/mechanisms.md "Replication", "Prediction and replication") ---
        struct HoldPending {
            uint32_t netId = 0;
            uint32_t inputTick = 0;
            bool drop = false;
            glm::vec3 position{0.0f};     ///< a pick-up: the copy's pose before it (the undo)
            glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        };
        std::optional<HoldPending> holdPending;
        uint32_t heldNetId = 0;           // the own held copy (confirmed or predicted); 0 none
        uint8_t latchedEdges = 0;         // Use / Cast presses made while a pick-up is pending
        uint8_t latchedOut = 0;           // ... sent with the next input once the pick-up is confirmed
        uint8_t lastControls = 0;         // the controls' Use / Cast levels of the last step (presses while pending)
        struct PendingRelease {
            EntityRelease release;
            uint32_t waited = 0;          ///< client ticks since it arrived
        };
        std::map<uint32_t, PendingRelease> pendingReleases;   // another holder's releases, waiting for the drawn holder
        bool heldPinned = false;
        std::string holdNotice;
        uint32_t holdNoticeTicks = 0;
        uint32_t lastUseTick = 0;
        sim::UseResult lastUseResult = sim::UseResult::None;

        /// The tick a mechanism is posed at outside a step: the predicted tick, else the newest server tick.
        uint32_t PoseTick() const { return predicting ? nextTick : std::max(newestSnapshot, welcome.tick); }
        /// The model term of the checks: VolumeHash, with a mechanism's MechanismTerm or a confirmed hold's HeldTerm.
        uint64_t CheckHash(const Copy& c) const;
        /// Makes the copy of `netId` (just made, kinematic) a mechanism with the server's state.
        void InstallMechanism(uint32_t netId, const MechanismState& state);
        /// Every mechanism copy's `linked` from its linkedNetId (a copy re-made by a model gets a new entity).
        void RelinkMechanisms();
        void PoseMechanismCopy(const Copy& c, uint32_t tick);
        /// FlushBox, the mechanisms' pose at `tick` and the push-out: before every StepPlayer (the server's order).
        void PrepareStep(uint32_t tick);
        /// The zones a StepPlayer of `tick` reads: the predicted tick, or a replayed entry's own tick, and whether a
        /// Spell edge may start the own move (a replay passes its entry's).
        sim::PlayerZones ZonesAt(uint32_t tick, bool spellMove = true) const;
        // [mag-2] Zones by opSeq and the spell's own move.
        /// A Spell edge may start the own move: alive and, in a fight past its warmup, a charge as the owner block said.
        bool SpellMoveAllowed(sim::SpellId spell) const;
        /// A ZoneStart, a ZoneCancel or a CastFx at its stream position.
        void ApplyMagic(const Event& event);
        /// Cancels of zones this client does not hold yet (a joiner's baseline zones come later): applied on insert.
        std::map<uint16_t, uint32_t> earlyCancels;
        std::deque<CastFx> castFx;
        Client::MagicView magic;
        /// [mag-8] The gesture of the next Spell edge (Client::SetSpellGesture), sent with that edge's input.
        std::optional<sim::SpellGesture> pendingGesture;
        uint32_t NetIdOfEntity(scene::Entity entity) const;
        /// The server's hold of a copy (EntityHold, or a spawn / baseline with kObjectHeld).
        void ApplyHold(uint32_t netId, uint8_t holder, uint16_t relYaw, uint32_t tick);
        /// An EntityRelease applied: the copy at the release pose, moving, waiting for samples after its tick.
        void ApplyRelease(const EntityRelease& r);
        /// The own predicted pick-up and drop for the newest tick (never in replays).
        void PredictHold(const sim::PlayerStepResult& r, const sim::PlayerInput& input);
        /// A pending pick-up the server answered without a hold: undone.
        void ResolveHold(const Snapshot& snap);
        void UndoPickup();
        /// Held copies posed from their holders (own: the predicted player; others: as they are drawn); other
        /// holders' releases applied once the drawn holder reached their tick.
        void PoseHeld(const sim::PlayerInput& input);
        /// The use target rules of this client: a copy is holdable when its server body is Dynamic and not held.
        sim::UseTargetHooks MakeUseHooks() const;
        /// A little dust where a mechanism starts to move (a live MechanismMove, presenting): the lead cue.
        void ShowLeadCue(const Copy& c);

        /// Local rubble (presentation): an object fragment, or a copy the server let go of.
        void MakeRubble(std::shared_ptr<voxel::VoxelModel> model, const glm::vec3& position, const glm::quat& rotation,
                        const glm::vec3& linear, const glm::vec3& angular);

        void ApplyEntity(uint32_t seq, const Event& event);

        /// Particles for the voxels of `before` (a copy's model at `position` / `rotation`) that the
        /// ObjectModel `after` no longer has; at most kMaxDamageBursts, every stride-th one.
        void ShowRemovedVoxels(const voxel::VoxelVolume& before, const ObjectModel& after, const glm::vec3& position, const glm::quat& rotation);

        /// Particles for the voxels of `volume` (a copy at `position` / `rotation`) inside the newest
        /// cast's sphere; at most kMaxDamageBursts, every stride-th one.
        void ShowVoxelsInSphere(const voxel::VoxelVolume& volume, const glm::vec3& position, const glm::quat& rotation);

        /// Snapshot samples of the copies. Rest and wake are events: a resting copy takes no sample
        /// (a sample of a woken body waits for its EntityWake).
        void TakeBodySamples(uint32_t tick, const std::vector<BodySample>& samples);

        /// The waiting samples whose snapshot's events are applied, oldest first (snapshots are taken
        /// in tick order and their lastOpSeq never falls).
        void TakeReadySamples();

        /// One tick of the copies: each moving one is driven (MoveKinematic) to where its samples put
        /// it at the end of the tick, drawn in the past by the interpolation delay.
        void MoveCopies();

        void Apply(uint32_t seq, const Event& event);

        /// An entity event for a NetId whose copy may still come (the join's baseline is open, or a
        /// model was asked for): kept in order, applied after the copy if newer than its base.
        void Hold(uint32_t netId, uint32_t seq, const Event& event);

        /// A BaselineEntity: the join's baseline, or the answer to a ModelRequest.
        void OnBaselineEntity(const BaselineEntity& b);

        /// Bulk messages (ChunkData, BaselineEntity, BaselineEnd), once attached.
        void ProcessBulk();

        /// Load reports, then the requests owed: chunks (the ring no longer covered them, or a slot
        /// freed for an Unsynced one) and models.
        void SyncHousekeeping();

        /// The join's "synced after" (logged only): the first time Synced() was seen true - every 16th
        /// Poll, or when a caller asked (Client::Synced: a client that syncs during a drain's last
        /// Polls would otherwise log 0 s).
        void NoteSynced();

        bool Synced() const;

        // --- the checks (stage 7b) ---------------------------------------------------------------

        void AddCheckTime(std::chrono::steady_clock::time_point start);

        /// A HashCheck at its stream position: chunks that differ become Requested (reason Repair).
        void OnHashCheck(const HashCheck& check);

        /// Entries of earlier HashChecks whose chunks were still loading here, compared once the chunks
        /// are current (review of stage 7b; ChunkSync::CheckDeferred).
        void CheckDeferred();

        /// What a HashCheck (or a deferred entry) found different: counted, logged; chunks asked for again
        /// (by ChunkSync) unless a repeat was refused.
        void OnMismatches(const HashCheckOutcome& o, uint32_t opSeq);

        /// This client's copies as an EntityCheck counts them; NetIds asked for count as present.
        EntityCheck OwnEntityCheck(bool withAsked) const;

        /// An EntityCheck at its stream position (amendments 3 and 8): compared when the baseline has
        /// ended, no list is out and no entity data ahead of it was processed; while models are asked
        /// for only the count and ids (the asked ones counted as present). A mismatch asks for the list.
        void OnEntityCheck(const EntityCheck& check);

        /// A NetId whose body this client lacks: asked for (7a's ModelRequest; its events are held).
        void AskModel(uint32_t netId);

        /// Removes a copy (no tombstone; its samples go too).
        void RemoveCopy(uint32_t netId);

        /// The EntityList at its stream position Q (amendment 2): copies the list does not have and
        /// that are as old as it are dropped; listed NetIds without a copy, or whose copy has another
        /// model, are asked for (their bodies come as answers, with the exact rest state); copies newer
        /// than the list are left alone.
        void OnEntityList(const EntityList& list);

        void ApplyPending();

        void Receive();
    };
}
