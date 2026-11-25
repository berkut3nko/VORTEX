module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module vortex.net:server_impl;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;
import :transport;
import :wire;
import :messages;
import :session;
import :sync;
import :entities;
import :server;

// The server's state and the declarations of its member functions (an internal partition: nothing outside
// vortex.net sees it). docs/design/rules.md "File map" lists which unit defines which function. [wp-6]
namespace vortex::net {

    namespace detail {
        /// Server::Impl::tickEvents (stage 8): ticks that did join or leave work, timed apart.
        inline constexpr uint8_t kTickHello = 1, kTickFirstWindow = 2, kTickLeave = 4;

        struct LoggedEvent {
            uint32_t tick = 0;
            uint64_t maskVoxels = 0;      ///< Detach / ClearMask: what a client decodes (a batch holds at most kMaxMaskVoxels)
            std::vector<uint8_t> bytes;
        };

        /// Ticks of player states the server keeps: a late input older than this (it would also be
        /// older than the input queue, 16 ticks) captures its cast from the current state.
        inline constexpr uint32_t kStateHistory = 32;

        /// A joined client's player (docs/design/networking.md "Player movement", "Time and ticks").
        struct Player {
            physics::CharacterId character;
            sim::PlayerState state;
            std::map<uint32_t, sim::PlayerInput> queue;   ///< inputs for ticks not simulated yet
            sim::PlayerInput last;          ///< the last real input: its movement is repeated while inputs are missing
            bool anyInput = false;
            uint32_t lastRealTick = 0;      ///< tick of `last` (a duplicate or older input is ignored)
            uint32_t ackTick = 0;           ///< newest input tick the server has (queued, used or late)
            /// The margin and the input it is about: the newest input of the packet with the newest
            /// last input so far, dropped ones included (Snapshot::marginTick). A reordered or
            /// duplicated older packet changes neither.
            uint32_t marginTick = 0;
            int16_t margin = kNoInputMargin;
            bool lateJump = false;          ///< a jump edge from a late input, made on the next step
            /// The state after each of the last kStateHistory ticks: a late input's cast is captured
            /// from the state of its own tick (networking.md "Time and ticks", stage 5). An entry
            /// never written has the tick kNoTick (review of stage 5: value-initialised entries said
            /// "tick 0, at the origin, cooldown 0").
            static constexpr uint32_t kNoTick = 0xFFFFFFFFu;
            std::array<std::pair<uint32_t, sim::PlayerState>, kStateHistory> history = [] {
                std::array<std::pair<uint32_t, sim::PlayerState>, kStateHistory> h{};
                for (auto& e : h) e.first = kNoTick;
                return h;
            }();
            /// The first tick this player was stepped: an input for an earlier tick no client makes.
            uint32_t joinTick = 0;
            /// The owner block of its snapshots: the newest cast edge's input tick and its fate.
            uint32_t lastCastTick = 0;
            CastRejection castRejected = CastRejection::None;
            /// Its crates (stage 6): the newest crate's tick and the NetIds of those alive, oldest first.
            bool anyCrate = false;
            uint32_t lastCrateTick = 0;
            std::deque<uint32_t> crates;
            /// [mech-1] The owner block's use result: the newest Use edge's input tick and its fate.
            uint32_t lastUseTick = 0;
            sim::UseResult useResult = sim::UseResult::None;
            /// [mech-1] Holding (mechanisms.md "One use key: E"): the held body (its scene::Held component is the
            /// truth), the busy rule's last change, the echo window after an auto-drop (0: none) and the
            /// replicated body it stood on last.
            scene::Entity held = scene::kNoEntity;
            uint32_t holdChangeTick = 0;
            bool anyHoldChange = false;
            uint32_t autoReleaseTick = 0;
            uint32_t autoReleaseEcho = 0;
            scene::Entity groundEntity = scene::kNoEntity;
            uint32_t groundTick = 0;
            /// An edge of an input made before its client saw the auto-drop: consumed, never acted on.
            bool InEcho(uint32_t inputTick) const { return autoReleaseEcho != 0 && inputTick <= autoReleaseTick + autoReleaseEcho; }

            /// The state after `tick`, if it is still kept.
            const sim::PlayerState* StateAt(uint32_t tick) const {
                const auto& h = history[tick % kStateHistory];
                return tick != kNoTick && h.first == tick ? &h.second : nullptr;
            }
            sim::PlayerState* StateAt(uint32_t tick) {
                auto& h = history[tick % kStateHistory];
                return tick != kNoTick && h.first == tick ? &h.second : nullptr;
            }
            void Store(uint32_t tick) { history[tick % kStateHistory] = {tick, state}; }
        };

        /// One bulk message waiting for a peer (stage 7a), encoded when it is dequeued.
        struct BulkItem {
            enum class Kind : uint8_t { Chunk, Entity } kind = Kind::Chunk;
            glm::ivec3 chunk{0};
            uint32_t netId = 0;
            bool repair = false;          ///< the answer to a ChunkRequest with reason Repair (stage 7b)
        };

        /// What a join cost (the `Join:` log line; stage 7a).
        struct JoinRecord {
            uint32_t joinTick = 0;
            uint32_t joinOpSeq = 0;       ///< the Welcome's (recheck of stage 8: Server::ClientJoinPoint)
            uint32_t windowChunks = 0, modifiedChunks = 0;
            uint32_t chunkData = 0;
            uint64_t chunkBytes = 0;
            uint32_t baseline = 0;
            uint64_t baselineBytes = 0;
            uint32_t requests = 0;
            size_t minWindow = 0, maxWindow = 0;
            double peakRate = 0.0;        ///< bulk bytes per second, the largest one-second sum
            double encodeSeconds = 0.0;   ///< wall clock of its bulk encodes and hand-overs (logged only)
            uint64_t secondBytes = 0;
            uint32_t secondStart = 0;
            bool logged = false;
            /// Stage 8: the join data is all acknowledged (queue empty and no bulk byte in flight):
            /// the end of the join for Server::JoiningClients (latched).
            bool done = false;
        };

        struct Peer {
            bool joined = false;
            bool closing = false;         ///< refused: disconnect once the refusal is delivered
            uint8_t slot = 0;
            uint32_t sentUpTo = 0;        ///< last opSeq handed to the transport for it
            uint32_t connectTick = 0;     ///< Hello timeout
            uint32_t closingTick = 0;     ///< refusal grace period
            std::unique_ptr<Player> player;   ///< from the Welcome on
            /// Snapshot priority of each awake body for this client (stage 6): grows by one every
            /// snapshot that left it out, back to 0 when it was sent.
            std::unordered_map<uint32_t, float> priority;

            // Chunk sync (stage 7a).
            bool hasWindow = false;
            uint32_t windowSeq = 0;       ///< the last WindowUpdate taken
            SyncBox window;
            std::deque<BulkItem> front;   ///< request answers, first in first out
            std::vector<glm::ivec3> chunks;   ///< queued chunks, farthest first (the nearest is at the back)
            std::unordered_set<glm::ivec3, ChunkCoordHash> queued;   ///< in `chunks` or in `front`
            glm::ivec3 sortedFor{0};      ///< the player's chunk when `chunks` was sorted
            /// The entries at the back of `chunks` that are the nearest, sorted (review of stage 7b: only
            /// they are kept in order, the rest is selected again when they run out). 0: select now.
            size_t sortedTail = 0;
            std::deque<uint32_t> baseline;    ///< NetIds of the join's baseline, nearest first
            bool baselineOpen = false;    ///< BaselineEnd not sent yet
            uint32_t baselineSent = 0;
            uint32_t nearLeft = 0;        ///< baseline NetIds within baselineNearMetres not dequeued yet
            bool nearSent = false;        ///< BaselineNear sent
            std::unordered_set<uint32_t> modelsQueued;   ///< NetIds of ModelRequest answers in `front`
            std::unique_ptr<BulkPacer> pacer;
            std::vector<uint8_t> heldBulk;    ///< encoded, waiting for room in the window
            size_t eventBytesSince = 0;   ///< reliable non-bulk bytes sent to it since its last PumpBulk (the pacer's drain)
            double bulkCredit = 0.0;      ///< maxBulkBytesPerSecond
            JoinRecord join;
            // The checks (stage 7b).
            HashCheckPlanner planner;     ///< which chunks its HashChecks pick (when each was last checked)
            bool listOwed = false;        ///< an EntityListRequest came: the list goes with the next Flush
            // Server terrain mode (stage 7b M-b): sticky once a WindowUpdate carried kTerrainMode.
            bool terrainMode = false;
            uint32_t terrainFirstSeq = 0;
            /// Chunks a TerrainAck queued, by the update that queued them (their bytes are counted to
            /// that update when they are sent: the join's and each step's cost).
            std::unordered_map<glm::ivec3, uint32_t, ChunkCoordHash> terrainQueuedBy;
            /// The updates whose chunks are still queued: bytes so far (ack + data sent) and chunks left.
            /// An update leaves the map once nothing it queued is left (sent, or pruned with its chunk):
            /// bounded by the queue, which is bounded by the window (review of stage 7b).
            struct TerrainStep {
                uint64_t bytes = 0;
                uint32_t pending = 0;
            };
            std::unordered_map<uint32_t, TerrainStep> terrainSteps;
            uint32_t terrainStepCount = 0;            ///< finished steps (updates after the first)
            uint64_t terrainStepBytes = 0, terrainMaxStepBytes = 0;
            uint64_t terrainJoinAckBytes = 0, terrainJoinDataBytes = 0;
            uint32_t terrainJoinChunks = 0, terrainJoinUniform = 0;
            uint32_t maxQueued = 0;               ///< the most chunks queued after a window update
            std::array<uint32_t, 3> ackSteps{};   ///< later TerrainAcks by direction: x only, z only, both
            std::array<uint64_t, 3> ackStepBytes{}, maxAckStepBytes{};
            uint32_t lastListTick = 0;            ///< when its last EntityList went
            bool listSent = false;
            bool listDelayed = false;             ///< the owed list was counted in entityListsDelayed
        };

        /// A replicated object's rest state as the clients were told it (EntityRest), and the rest's
        /// tick and pose (a joiner's baseline puts a resting copy exactly there; stage 7a).
        struct BodyTrack {
            bool resting = false;
            uint32_t restTick = 0;
            glm::vec3 restPosition{0.0f};
            glm::quat restRotation{1.0f, 0.0f, 0.0f, 0.0f};
            bool held = false;            ///< [mech-1] held by a player: no rest, no wake, no sample
        };

        /// A cast waiting for the cast phase. A player's cast is a ray (captured at its input tick:
        /// eye and look of that tick), cast into the world when it is dequeued; a server-side cast
        /// (QueueCast) has its sphere already.
        struct QueuedCast {
            voxel::SphereQ sphere;
            uint16_t power = 0;
            uint32_t queuedTick = 0;
            uint8_t player = 0;           ///< CastRecord::player
            bool ray = false;
            sim::ActionRequest request;   ///< the captured action (ray casts; a server cast keeps the defaults)
            scene::Entity body = scene::kNoEntity;   ///< the body the ray resolved on (a mechanism takes a MechanismBlast)
        };

        /// A terrain-mode peer's bulk goes nearest its eyes first: this far above the feet.
        inline constexpr double kEyeMetres = 1.6;

        /// Margins further off than this (17 s) are not reported: no clock is, a forged tick is.
        inline constexpr int64_t kMaxReportedMargin = 1024;

        inline uint64_t MaskVoxels(const Event& event) {
            auto volumeVoxels = [](const voxel::VoxelVolume& v) {
                return static_cast<uint64_t>(v.Size().x) * static_cast<uint64_t>(v.Size().y) * static_cast<uint64_t>(v.Size().z);
            };
            if (const EntitySpawn* spawn = std::get_if<EntitySpawn>(&event))
                return spawn->kind == EntityKind::Object ? volumeVoxels(spawn->object.volume) : 0;
            if (const ObjectModel* model = std::get_if<ObjectModel>(&event)) return volumeVoxels(model->volume);
            const sim::WorldOp* op = std::get_if<sim::WorldOp>(&event);
            if (!op || (op->kind != sim::WorldOpKind::Detach && op->kind != sim::WorldOpKind::ClearMask)) return 0;
            return op->mask.VoxelCount();
        }
    }
    using namespace detail;
    /// [wp-7] The `Fight:` line of a server's stats (fight.md "Rules"; G22 reads it).
    std::string FightLineOf(const ServerStats& st);

    struct Server::Impl {
        Impl(ServerSettings s, ITransport& t);
        ~Impl();

        ServerSettings settings;
        ITransport& transport;
        sim::RegionDesc region;
        std::unique_ptr<sim::Simulation> simulation;
        sim::RegionTemplate tmpl;
        std::vector<ProbeHash> probes;
        uint64_t buildHash = 0;
        sim::PlayArea playArea;           // sim::PlayAreaOf(region, settings.player): every player's StepPlayer area
        sim::AimRules aimRules;           // sim::SessionAimRules(settings.player, region): every player's cast
        bool started = false;
        bool failed = false;

        // Events made but not handed to every joined peer yet: backlog[i] has opSeq backlogFirst + i
        // (stage 7a: no join log; a joiner starts at its join point).
        std::deque<LoggedEvent> backlog;
        uint32_t backlogFirst = 1;
        uint32_t lastOpSeq = 0;
        uint32_t steppedTick = 0;         // the tick whose physics was stepped last (the bodies' state is of it)
        double encodeTickSeconds = 0.0;   // bulk encodes of the current tick (wall clock, logged only)
        size_t backlogBytes = 0;
        // Every chunk an event changed and the last such event (never cleared; sparse: stage 9's
        // on-demand world needs nothing else). What a joiner or a moving window is sent.
        std::unordered_map<glm::ivec3, uint32_t, ChunkCoordHash> modified;
        std::unordered_map<glm::ivec3, uint32_t, ChunkCoordHash> requestCounts;   // ChunkRequest coordinates received (tests only)
        std::map<uint32_t, sim::TemplateLight> lights;    // the template's lights (a joiner's baseline)
        std::map<PeerId, Peer> peers;
        std::deque<QueuedCast> queue;
        int64_t castCredit = 0;
        int64_t eventCredit = 0;
        int64_t entityCredit = 0;         // the entity event budget (set to its burst at Start)
        double accumulator = 0.0;
        ServerStats stats;
        std::deque<CastRecord> casts;
        // Replicated objects (stage 6).
        std::vector<EntityDespawn> despawned;              // the on_destroy<NetId> observer's queue
        std::unordered_map<uint32_t, BodyTrack> tracks;   // NetId -> rest state the clients know
        std::shared_ptr<voxel::VoxelModel> crateModel;
        std::function<void(uint8_t)> onClientLeft;
        // Stage 8: what the current tick did besides its routine (kTickHello / kTickFirstWindow /
        // kTickLeave) and the colliders FlushBox cooked on this thread in it; the hash cache.
        uint8_t tickEvents = 0;
        uint32_t tickFlushCooks = 0;
        struct HashEntry {
            uint64_t revision = 0;        ///< World::ChunkRevision it was taken at (0: none)
            uint64_t hash = 0;
        };
        mutable std::vector<HashEntry> hashCache;   // per cell of the server's fixed window
        mutable uint64_t hashesComputed = 0;
        /// Chunks operations changed since the cache was filled: re-hashed a few per tick (RefreshHashes),
        /// so a leave's hash finds them current (stage 8: the first leave after a fight took 21 ms).
        std::unordered_set<glm::ivec3, ChunkCoordHash> hashStale;

        void RefreshHashes(size_t budget);

        /// WorldHash over [min, max) with each chunk's hash cached by its revision (stage 8: a leave's
        /// hash of a quiet window costs lookups). Equal to voxel::WorldHash by construction.
        voxel::WorldHashResult CachedHash(const glm::ivec3& min, const glm::ivec3& max) const;

        /// The colliders the server keeps: every player's, and the template's spawn for good (stage 8:
        /// a join finds the ground of its spawn built; before, the join tick cooked it, 120 ms).
        std::vector<glm::dvec3> ColliderFocus() const;
        // The checks (stage 7b): the chunks the last operations changed, oldest first (at most 64), and
        // the replicated objects' check values as of `entityValuesAt` (computed once per opSeq).
        std::vector<std::pair<uint32_t, glm::ivec3>> recentChanged;
        bool entityValuesValid = false;
        uint32_t entityValuesAt = 0;
        EntityCheck entityValues;
        std::vector<EntityListEntry> entityList;
        /// VolumeHash per NetId with the model it was taken of (a model is never changed in place:
        /// ObjectModel / ReplaceModel put a new one; the weak pointer cannot be fooled by a new model at
        /// a freed one's address). Spares hashing every model at every opSeq (stage 7b: up to 5 ms in
        /// one tick on the arena).
        struct ModelHash {
            std::weak_ptr<const voxel::VoxelModel> model;
            uint64_t hash = 0;
        };
        std::unordered_map<uint32_t, ModelHash> modelHashes;

        /// networking.md "Entity replication": every destruction of a replicated entity, by any path,
        /// becomes an EntityDespawn (emitted in order by DrainDespawns).
        void OnNetIdDestroyed(entt::registry& registry, entt::entity entity);

        void DrainDespawns();

        /// The ObjectSpawn of a replicated entity as it is now (its body's pose and velocities).
        bool DescribeObject(scene::Entity entity, ObjectSpawn& out);

        /// EntitySpawn of a replicated entity (a template object, a crate, an object piece).
        void EmitObjectSpawn(uint32_t netId, scene::Entity entity);

        /// The object events of a cast after its operations: models, spawns, fragments, lost pieces,
        /// then every despawn the cast caused (evictions, objects broken apart).
        void EmitObjectEvents(const sim::BlastResult& r);

        /// Rest transitions after the tick's physics: a replicated object that fell asleep is
        /// announced with its pose (EntityRest), one that wakes with EntityWake (review of stage 6),
        /// both in the opSeq stream, and it shows up in snapshots while it moves.
        ///
        /// The layer follows the same rule as a client's copy (networking.md "Stage 6 as built",
        /// "Review of stage 6"): Layer::Rubble while it moves, Layer::Dynamic at rest - and while it
        /// moves under a player who stands on it (a client keeps its copy solid under its own player
        /// the same way), so that players on both sides collide with the same bodies at the same
        /// opSeq (a moving piece passes through a player everywhere; a player standing on a crate a
        /// blast wakes stays on it).
        void TrackBodies(uint32_t tick);

        scene::Entity EntityOf(uint32_t netId);

        /// A crate with its model origin at `corner` (stage 6): the playground's crate, a replicated
        /// placed object. 0 when it would leave the play bounds or the window's rows.
        uint32_t SpawnCrateAt(const glm::dvec3& corner, Peer* owner);

        /// A replicated placed object with its model origin at `corner` (a crate, an admin object).
        /// 0 when it would leave the play bounds or the window's rows.
        uint32_t SpawnObjectAt(const std::shared_ptr<voxel::VoxelModel>& model, const glm::dvec3& corner, Peer* owner);

        /// A crate edge of a player (its state after the input's tick, the input's look).
        void PlayerCrate(Peer& p, const sim::PlayerInput& input, const sim::PlayerState& at);

        /// The awake replicated bodies (snapshot candidates), in NetId order.
        std::vector<BodySample> AwakeSamples();

        uint32_t Tick() const { return simulation ? simulation->Tick() : 0; }
        uint32_t LastOpSeq() const { return lastOpSeq; }

        /// Logs and numbers one event. Only terrain operations charge the event credit (the cast
        /// queue's byte budget): entity events follow from Jolt's float poses and sleep (where a
        /// piece rests, which crate a blast catches), and a replayed server must put every cast on
        /// the same tick (shred.md R5; networking.md "Stage 6 as built"). They are counted apart.
        uint32_t Emit(const Event& event);

        /// A terrain operation applied to the server's world: numbered, and every chunk it changed is
        /// marked modified at its opSeq (stage 7a; a chunk it did not change still equals the
        /// generator's output where it did before).
        uint32_t EmitOp(const sim::WorldOp& op, const sim::OpResult& result);

        /// False (and the session fails) when the server's own world could not apply `r` in full.
        bool CheckOp(const sim::OpResult& r);

        /// networking.md "Server world without a camera": a broken server world ends the session.
        /// Every peer is disconnected at once (what was not delivered yet is dropped), nothing more
        /// is made or sent, queued casts are dropped and new peers are turned away (Tick).
        void Fail(const char* why);

        void DropPlayer(Peer& p);

        /// The joined peers with a player, in slot order (the order players are stepped in).
        std::vector<std::pair<PeerId, Peer*>> PlayersBySlot();

        std::vector<glm::dvec3> PlayerFocus() const;

        /// A cast edge the cooldown accepted, captured as a sim::ActionRequest (rules.md "The seam";
        /// networking.md "Time and ticks"): the eye of the player's state after the input's tick (`at`)
        /// and the look of `input`. The ray is cast and the blast run when the cast queue reaches it
        /// (RunCasts), on the world as it is then.
        void CaptureCast(Peer& p, const sim::PlayerInput& input, const sim::PlayerState& at, bool late);

        /// A cast edge the cooldown refused: reported to its owner (it predicted the same cooldown,
        /// so this is a misprediction or a forged input).
        void RejectCast(Peer& p, uint32_t inputTick);

        bool Queue(const voxel::SphereQ& sphere, uint16_t power, uint8_t player);

        Peer* PeerOfSlot(uint8_t player);

        /// A player's cast at dequeue: its ray into the world as it is now. False (counted, reported
        /// to the owner if it is still there) when it meets no terrain within castRange or the hit
        /// does not fit the region (sim::CastFitsRegion). A replicated body in front of the terrain
        /// takes the cast (a crate, a falling piece), as offline (review of stage 6). (sim::AimAction,
        /// rules.md "Aim rules")
        bool ResolveRay(QueuedCast& q);

        /// A cast edge in a late input (tick `in.tick` < `now`): judged and captured at the input's
        /// tick, as its client predicted it (networking.md "Time and ticks", stage 5). The cooldown is
        /// the one the player had after that tick; if it is taken, the cooldown now is what it would
        /// be had the cast been taken then (the owner predicted exactly that). Origin: the state
        /// after the input's tick, or the current one if that is no longer kept.
        void LateCast(Peer& p, const sim::PlayerInput& in, uint32_t now);

        /// An InputBatch from a joined client (state channel: lost, duplicated, reordered).
        void OnInputs(Peer& p, std::span<const uint8_t> bytes);

        /// Every player, in slot order, steps with its input for this tick or the last movement repeated.
        void StepPlayers();

        void SendSnapshots(uint32_t tick);

        uint32_t JoinedCount() const;

        void Send(PeerId peer, Channel channel, const std::vector<uint8_t>& bytes);

        /// Sends the batch that starts after `from` (events of one tick, at most maxBatchBytes and
        /// kMaxMaskVoxels of masks; one larger event goes alone) and returns its last opSeq.
        uint32_t SendBatch(PeerId peer, uint32_t from, uint32_t to);

        /// Hands a peer its next batches (all of them, unless a test holds them while
        /// maxEventBytesInFlight is on its way to it).
        void Pump(PeerId id, Peer& p);

        void Refuse(PeerId peer, Peer& p, RefuseReason reason);

        void OnHello(PeerId peer, Peer& p, std::span<const uint8_t> bytes);

        // --- chunk sync (stage 7a) ---------------------------------------------------------------

        /// The NetIds alive now (template lights, replicated objects), nearest `at` first: the join's
        /// baseline, made into BaselineEntity messages when they are dequeued.
        void QueueBaseline(Peer& p, const glm::dvec3& at);

        /// A replicated entity as it is now (light or object; a resting object at its EntityRest pose
        /// bit for bit, zero velocity). False when it is gone.
        bool MakeBaselineEntity(uint32_t netId, BaselineEntity& out);

        bool QueueChunk(Peer& p, const glm::ivec3& c);

        /// Horizontal distance (metres) from a chunk's centre to the peer's player; for a peer in
        /// server terrain mode the 3D distance from its eyes (stage 7b choice 6: the surface it sees
        /// before the buried rock under it).
        double ChunkDistance(const Peer& p, const glm::ivec3& c) const;

        /// The queue's nearest `kSortedTail` entries, selected and sorted at its back (pop_back = nearest),
        /// again when the player's chunk changed, something was added or the sorted tail ran out (review
        /// of stage 7b: a terrain-mode queue holds about 20 000 chunks; selecting is O(n), a full sort per
        /// chunk crossing cost 2-4 ms there). The key is the squared distance (ChunkDistance's order
        /// without a square root), ties in (y, z, x): the order is the same on every run.
        static constexpr size_t kSortedTail = 1024;
        void SortChunks(Peer& p, bool force);

        /// The nearest queued chunk is at the back (the sorted tail selected again when it ran out).
        void PopChunk(Peer& p);

        /// Queued chunks that left the peer's new window go now (review of stage 7b: on a slow link a
        /// walker's left-behind slabs stayed queued, at the far end of the queue, for the session).
        /// Request answers in `front` are left to the dequeue. Keeps the queue's order.
        void PruneQueue(Peer& p, const SyncBox& box);

        /// A terrain-mode chunk queued by update `seq` (its bytes count to that update when it goes).
        void TerrainQueued(Peer& p, const glm::ivec3& c, uint32_t seq);

        /// A terrain-mode chunk left the queue: sent (`bytes`), pruned or dropped (0). Its update is
        /// finished once nothing it queued is left.
        void TerrainChunkGone(Peer& p, const glm::ivec3& c, uint64_t bytes);

        void FinishTerrainStep(Peer& p, uint32_t seq);

        /// WindowUpdate: entering chunks (all of them with reset or for the first) the modified map
        /// holds are queued and get their bit in the ack; the ack goes at once, after the events made
        /// so far (so a client applies it with every event up to its asOf applied).
        bool OnWindowUpdate(PeerId id, Peer& p, std::span<const uint8_t> bytes);

        /// A terrain-mode WindowUpdate (stage 7b M-b, server terrain mode): every entering chunk inside
        /// the server's window is described as the server holds it now - uniform ones in the ack, the
        /// others queued as ChunkData (code 1 Dense, code 3 Packed: the client packs those as the
        /// server does) - modified or not. The ack goes at once after the events made so far, like a
        /// WindowAck, so the client takes it with every event up to its asOf applied.
        bool OnTerrainWindow(PeerId id, Peer& p, const WindowUpdate& u, const SyncBox& box, bool all);

        bool OnChunkRequest(Peer& p, std::span<const uint8_t> bytes);

        /// EntityListRequest (stage 7b): the list goes right after the peer's events of the next Flush,
        /// one per peer at a time (repeats merged).
        bool OnEntityListRequest(Peer& p, std::span<const uint8_t> bytes);

        // --- the checks (stage 7b) ---------------------------------------------------------------

        /// The replicated objects' check values and list at the last opSeq (objects with a body and a
        /// model, as Entities(); lights are not counted). Every change of the set or of a model is an
        /// event, so they are computed once per opSeq.
        void EntityValues();

        /// A peer's HashCheck: the planner's chunks and the server's hashes of them after the last event.
        void SendHashCheck(PeerId id, Peer& p);

        /// The checks and the entity list of one peer, right after its events (Pump): a client processes
        /// them with every event up to LastOpSeq applied and none after (networking.md "Stage 7b plan").
        void SendChecks(PeerId id, Peer& p);

        bool OnModelRequest(Peer& p, std::span<const uint8_t> bytes);

        /// The next bulk message of a peer, encoded now (asOf = the last event made): request answers,
        /// chunks within bulkNearMetres of its player, the baseline, then the other chunks nearest first.
        /// Empty when nothing is left; `dropped` when the item had left the window or was gone.
        std::vector<uint8_t> NextBulk(Peer& p, bool& any);

        /// Bulk of one peer, after its events: while its bytes in flight are under the pacer's window
        /// and within the optional hard cap. A message that does not fit what is left of the window
        /// waits for the peer's bulk in flight to drain, then goes alone while the bytes in flight
        /// (the live events) are under the window (review of stage 7a, 3); while the events alone fill
        /// the window nothing goes (recheck of stage 7a, 1: that is the pacer's back-off).
        void PumpBulk(PeerId id, Peer& p);

        void Receive();

        /// A failed session turns every new peer away.
        void ReceiveFailed();

        void RunCast(const QueuedCast& q, bool overdue);

        void RunCasts();

        /// Disconnects a joined peer at once (a broken sync stream, too slow).
        void DropPeer(PeerId id);

        void Flush();

        /// Events every joined peer has: gone (a joiner starts at the newest one).
        void TrimBacklog();

        void CheckStore();

        // --- the fight ([wp-7], docs/design/fight.md; ServerFight.cpp) ---------------------------------------
        // Fight order per tick (Server::Tick): StepPlayers (the dead are not stepped, their cast edges refused
        // Dead; the living take environment damage after their step while Live), CaptureCast (a charge per cast
        // in Countdown / Live / Ended, else NoCharge), RunCasts (blast damage through DealDamage while Live),
        // StepFight (charges, respawns, the round), SendSnapshots (owner block and remote record).
        std::array<sim::FighterState, 256> fighters{};      // by slot
        sim::Round round;
        std::array<uint32_t, 2> teamSpawnCount{};
        struct Crown {
            glm::ivec3 min{0}, max{0};
            uint32_t voxels = 0;
        };
        std::array<Crown, 2> crowns{};                       // Felling: each team's crown box
        std::vector<sim::WorldOp> towerOps;                  // Felling: both towers' boxes, built at the start, again when Live begins
        bool rosterChanged = false;                          // a Roster is sent at the end of StepFight (once per tick at most)
        std::array<bool, 2> felled{};
        bool sessionOver = false;                            // the Ended phase ran out
        bool endNow = false;                                 // ... with end_session: disconnect every peer after this tick's Flush
        sim::RoundPhase loggedPhase = sim::RoundPhase::Warmup;
        const sim::FightRules& Rules() const { return settings.session.fight; }
        bool Fighting() const { return settings.session.fight.Active(); }
        /// The team a joiner gets (fewer players, ties to 1; kNoTeam in a sandbox).
        sim::TeamId JoinTeam(uint8_t slot);
        void FightJoin(Peer& p);                             // log, RoundUpdate + Roster to the joiner, Roster to the others
        void FightLeave(uint8_t slot);
        void DealDamage(const sim::DamageEvent& event);      // decision 16: the one damage entry
        void EnvironmentDamage(uint8_t slot, const sim::PlayerState& before, const sim::PlayerState& after);
        void Respawn(uint8_t slot);
        void StepFight();                                    // charges, respawns, round; after RunCasts; then the roster
        void StepRoundAndLives();
        std::vector<sim::PlayerView> PlayerViews();          // slot ascending, joined players
        std::array<uint32_t, 2> TeamPlayers();
        void SendToJoined(const std::vector<uint8_t>& bytes);
        Roster MakeRoster();
        RoundUpdate MakeRoundUpdate() const;
        void SendRoster();                                   // marks the roster changed (FlushRoster sends it)
        void FlushRoster();
        void SendRound();
        void SendKill(const KillNotice& kill);
        void LogRound();
        /// Dead now, or `inputTick` is not of its current life (at or before its last spawn). False outside a fight.
        bool NotAliveAt(uint8_t slot, uint32_t inputTick) const;
        /// A cast edge of a player not alive at its input's tick (NotAliveAt): refused Dead (fight.md "Rules").
        /// True when it was refused.
        bool RefuseDeadCast(Peer& p, uint32_t inputTick);
        Snapshot::Fight OwnerFightOf(uint8_t slot) const;
        uint8_t RemoteFightOf(uint8_t slot) const;
        void BuildFellingTowers();
        void CheckCrowns();

        // --- mechanisms and holding ([mech-1], docs/design/mechanisms.md; ServerMechanisms.cpp) ---------------------
        // Order per tick: StepPlayers (PoseMechanisms and PushOutOfMechanisms before each StepPlayer; after it the
        // throw, then the Use edge), RunCasts (a blast's releases before its object events), StepHolds, StepMechanisms.
        std::vector<scene::Entity> mechanisms;             // the yard's, in creation order
        std::vector<glm::vec3> holdFeet;                   // HoldEnvFor()'s storage
        uint32_t NetIdOf(scene::Entity entity) const;      // 0 without a NetId
        void BuildYard();                                  // at Start, with SessionConfig::mechanisms and the playground
        /// The mechanism's state with its ring's segments still running at `tick` (always the newest), oldest first.
        bool MakeMechanismState(scene::Entity entity, uint32_t tick, MechanismState& out) const;
        void EmitMechanismSpawn(uint32_t netId, scene::Entity entity);
        void EmitMechanismChanges(std::span<const sim::MechanismChange> changes);
        uint32_t LeadOf(PeerId id, const Player& pl) const;   // ceil(RTT / dt) + max(margin, 0) + 2, clamped
        uint32_t UseLead();                                // the largest LeadOf over the joined players
        /// A Use edge (`at`: the state after the input's tick; nullptr when it is no longer kept).
        void PlayerUse(Peer& p, const sim::PlayerInput& in, const sim::PlayerState* at, uint32_t now);
        /// The Cast edge while holding and the edges of the echo window (step order of mechanisms.md "Where it runs"):
        /// true when the edges are consumed (no cast, no use).
        bool HoldEdges(Peer& p, const sim::PlayerInput& in, uint8_t pressed);
        sim::HoldEnv HoldEnvFor();
        void EmitHold(const sim::HoldChange& c);
        /// Ends `p`'s hold for `reason` at the hold pose of its state now (the holder's anchor fix-up).
        void ReleaseHeld(Peer& p, sim::HoldEnd reason, uint16_t yaw, int16_t pitch);
        /// Ends `p`'s hold where the body is now (HolderGone: no fix-up).
        void ReleaseGone(Peer& p);
        /// A release the player did not ask for: its echo window opens.
        void AutoReleased(uint8_t slot);
        void EmitHoldChanges(std::span<const sim::HoldChange> changes);
        void ClearLostHolds();                             // a held entity gone or without scene::Held
        void StepHolds();
        /// The model term of the checks: VolumeHash, a mechanism's MechanismTerm or a held body's HeldTerm mixed in.
        uint64_t CheckModelHash(scene::Entity entity, uint64_t volumeHash) const;
    };
}
