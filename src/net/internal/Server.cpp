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

module vortex.net;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;

namespace vortex::net {

    namespace {
        /// Server::Impl::tickEvents (stage 8): ticks that did join or leave work, timed apart.
        constexpr uint8_t kTickHello = 1, kTickFirstWindow = 2, kTickLeave = 4;

        struct LoggedEvent {
            uint32_t tick = 0;
            uint64_t maskVoxels = 0;      ///< Detach / ClearMask: what a client decodes (a batch holds at most kMaxMaskVoxels)
            std::vector<uint8_t> bytes;
        };

        /// Ticks of player states the server keeps: a late input older than this (it would also be
        /// older than the input queue, 16 ticks) captures its cast from the current state.
        constexpr uint32_t kStateHistory = 32;

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
            glm::dvec3 origin{0.0};       ///< the eye at the input's tick (ray)
            glm::vec3 direction{0.0f};    ///< LookDirection of the input (ray)
            uint32_t inputTick = 0;       ///< the input the cast edge came in (ray)
            uint8_t loadout = 0;          ///< 0 = the tuning's cast (one loadout in M1)
        };

        /// A terrain-mode peer's bulk goes nearest its eyes first: this far above the feet.
        constexpr double kEyeMetres = 1.6;

        /// Margins further off than this (17 s) are not reported: no clock is, a forged tick is.
        constexpr int64_t kMaxReportedMargin = 1024;

        uint64_t MaskVoxels(const Event& event) {
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

    struct Server::Impl {
        Impl(ServerSettings s, ITransport& t) : settings(std::move(s)), transport(t), region(settings.session.region) {}
        ~Impl() {
            // The scene is cleared when the simulation goes: no despawn may reach members already gone.
            if (simulation) simulation->Scene().Registry().on_destroy<scene::NetId>().disconnect<&Impl::OnNetIdDestroyed>(*this);
        }

        ServerSettings settings;
        ITransport& transport;
        sim::RegionDesc region;
        std::unique_ptr<sim::Simulation> simulation;
        sim::RegionTemplate tmpl;
        std::vector<ProbeHash> probes;
        uint64_t buildHash = 0;
        sim::PlayArea playArea;           // sim::PlayAreaOf(region, settings.player): every player's StepPlayer area
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

        void RefreshHashes(size_t budget) {
            for (auto it = hashStale.begin(); it != hashStale.end() && budget > 0; --budget) {
                (void)CachedHash(*it, *it + glm::ivec3(1));
                it = hashStale.erase(it);
            }
        }

        /// WorldHash over [min, max) with each chunk's hash cached by its revision (stage 8: a leave's
        /// hash of a quiet window costs lookups). Equal to voxel::WorldHash by construction.
        voxel::WorldHashResult CachedHash(const glm::ivec3& min, const glm::ivec3& max) const {
            const voxel::World& w = simulation->World();
            const glm::ivec3 wmin = w.WindowMinChunk(), wsize = w.WindowSize();
            if (hashCache.size() != static_cast<size_t>(wsize.x) * static_cast<size_t>(wsize.y) * static_cast<size_t>(wsize.z))
                hashCache.assign(static_cast<size_t>(wsize.x) * static_cast<size_t>(wsize.y) * static_cast<size_t>(wsize.z), {});
            voxel::WorldHashResult result;
            for (int z = min.z; z < max.z; ++z)
                for (int y = min.y; y < max.y; ++y)
                    for (int x = min.x; x < max.x; ++x) {
                        const glm::ivec3 c(x, y, z), local = c - wmin;
                        uint64_t hash = voxel::kAirChunkHash;
                        const uint64_t revision = w.ChunkRevision(c);
                        const bool inside = glm::all(glm::greaterThanEqual(local, glm::ivec3(0))) && glm::all(glm::lessThan(local, wsize));
                        if (revision == 0 || !inside) {
                            if (!voxel::ChunkHashAt(w, c, hash)) {
                                ++result.notLoaded;
                                continue;
                            }
                        } else {
                            HashEntry& e = hashCache[(static_cast<size_t>(local.z) * static_cast<size_t>(wsize.y) + static_cast<size_t>(local.y)) *
                                                          static_cast<size_t>(wsize.x) +
                                                      static_cast<size_t>(local.x)];
                            if (e.revision != revision) {
                                if (!voxel::ChunkHashAt(w, c, e.hash)) {
                                    ++result.notLoaded;
                                    continue;
                                }
                                e.revision = revision;
                                ++hashesComputed;
                            }
                            hash = e.hash;
                        }
                        if (hash == voxel::kAirChunkHash) continue;
                        result.hash += voxel::WorldHashTerm(c, hash);
                        ++result.solidChunks;
                    }
            return result;
        }

        /// The colliders the server keeps: every player's, and the template's spawn for good (stage 8:
        /// a join finds the ground of its spawn built; before, the join tick cooked it, 120 ms).
        std::vector<glm::dvec3> ColliderFocus() const {
            std::vector<glm::dvec3> focus = PlayerFocus();
            focus.push_back(glm::dvec3(tmpl.spawn));
            return focus;
        }
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
        void OnNetIdDestroyed(entt::registry& registry, entt::entity entity) {
            const scene::NetId& id = registry.get<scene::NetId>(entity);
            despawned.push_back({id.value, id.despawn});
        }

        void DrainDespawns() {
            std::vector<EntityDespawn> queue = std::move(despawned);
            despawned.clear();
            for (const EntityDespawn& d : queue) {
                tracks.erase(d.netId);
                for (auto& [pid, p] : peers)
                    if (p.player) std::erase(p.player->crates, d.netId);
                if (failed) continue;
                Emit(d);
                ++stats.despawns[std::min<size_t>(static_cast<size_t>(d.reason), 3)];
            }
        }

        /// The ObjectSpawn of a replicated entity as it is now (its body's pose and velocities).
        bool DescribeObject(scene::Entity entity, ObjectSpawn& out) {
            scene::Scene& scene = simulation->Scene();
            const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
            const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(entity);
            if (!voxels || !voxels->model || !body) return false;
            const physics::PhysicsWorld& physics = simulation->Physics();
            const physics::BodyState state = physics.GetState(body->body);
            out.position = state.position;
            out.rotation = state.rotation;
            out.linearVelocity = state.frozen ? state.keptLinearVelocity : state.linearVelocity;
            out.angularVelocity = state.angularVelocity;
            out.motion = static_cast<uint8_t>(body->motion);
            out.flags = sim::IsAwake(physics, body->body) ? 0 : kObjectResting;
            out.volume = voxels->model->Volume();
            return true;
        }

        /// EntitySpawn of a replicated entity (a template object, a crate, an object piece).
        void EmitObjectSpawn(uint32_t netId, scene::Entity entity) {
            EntitySpawn spawn;
            spawn.netId = netId;
            spawn.kind = EntityKind::Object;
            if (!DescribeObject(entity, spawn.object)) return;
            BodyTrack& t = tracks[netId];
            t.resting = (spawn.object.flags & kObjectResting) != 0;
            if (t.resting) {
                // Created at rest: its rest is the spawn (no EntityRest follows until it moved).
                t.restTick = Tick();
                t.restPosition = spawn.object.position;
                t.restRotation = spawn.object.rotation;
            }
            Emit(spawn);
            ++stats.objectSpawns;
        }

        /// The object events of a cast after its operations: models, spawns, fragments, lost pieces,
        /// then every despawn the cast caused (evictions, objects broken apart).
        void EmitObjectEvents(const sim::BlastResult& r) {
            for (const sim::AppliedOp& applied : r.ops)
                if (applied.op.kind == sim::WorldOpKind::Detach && applied.op.netId != 0) ++stats.detachBodies;
            for (const sim::ObjectEvent& e : r.objectEvents) {
                if (failed || !e.model) continue;
                if (e.kind == sim::ObjectEvent::Kind::Model) {
                    // Over the entity byte budget the survivor is let go as client rubble (6 bytes)
                    // instead of resending its whole model (review of stage 6).
                    if (entityCredit <= 0 && simulation->Scene().IsValid(e.entity)) {
                        sim::Despawn(*simulation, e.entity, scene::DespawnReason::ToRubble);
                        ++stats.modelsToRubble;
                        continue;
                    }
                    ObjectModel m;
                    m.netId = e.netId;
                    m.offset = e.offset;
                    m.motion = static_cast<uint8_t>(e.motion);
                    m.volume = e.model->Volume();
                    Emit(m);
                    ++stats.objectModels;
                    continue;
                }
                EntitySpawn spawn;
                spawn.netId = e.netId;
                spawn.kind = EntityKind::Object;
                spawn.object.position = e.pose.position;
                spawn.object.rotation = e.pose.rotation;
                spawn.object.linearVelocity = e.linearVelocity;
                spawn.object.angularVelocity = e.angularVelocity;
                spawn.object.motion = static_cast<uint8_t>(e.motion);
                spawn.object.volume = e.model->Volume();
                tracks[e.netId].resting = false;
                Emit(spawn);
                ++stats.objectSpawns;
            }
            for (const scene::DamageFragment& f : r.fragments) {
                if (failed || !f.model) continue;
                EntitySpawn spawn;
                spawn.kind = EntityKind::Object;
                spawn.object.position = f.pose.position;
                spawn.object.rotation = f.pose.rotation;
                spawn.object.linearVelocity = f.linearVelocity;
                spawn.object.angularVelocity = f.angularVelocity;
                spawn.object.volume = f.model->Volume();
                Emit(spawn);
                ++stats.fragmentSpawns;
            }
            for (const uint32_t lost : r.lostNetIds)
                if (!failed) {
                    Emit(EntityDespawn{lost, scene::DespawnReason::Destroyed});
                    ++stats.despawns[1];
                }
            DrainDespawns();
        }

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
        void TrackBodies(uint32_t tick) {
            physics::PhysicsWorld& physics = simulation->Physics();
            std::vector<physics::BodyId> grounds;
            for (const auto& [pid, p] : peers)
                if (p.player)
                    if (const physics::BodyId g = physics.GetCharacterState(p.player->character).groundBody; g.IsValid()) grounds.push_back(g);
            uint32_t awake = 0;
            for (const auto [entity, id, body] : simulation->Scene().Registry().view<const scene::NetId, const scene::RigidBody>().each()) {
                const bool moving = sim::IsAwake(physics, body.body);
                BodyTrack& t = tracks[id.value];
                awake += moving ? 1u : 0u;
                if (moving) {
                    if (t.resting) {
                        t.resting = false;
                        Emit(EntityWake{id.value, tick});
                        ++stats.wakes;
                    }
                    const bool carries = std::find(grounds.begin(), grounds.end(), body.body) != grounds.end();
                    if (body.motion != physics::MotionType::Static)
                        physics.SetLayer(body.body, carries ? physics::Layer::Dynamic : physics::Layer::Rubble);
                } else if (!t.resting) {
                    t.resting = true;
                    if (body.motion != physics::MotionType::Static) physics.SetLayer(body.body, physics::Layer::Dynamic);
                    const physics::BodyState state = physics.GetState(body.body);
                    t.restTick = tick;
                    t.restPosition = state.position;
                    t.restRotation = state.rotation;
                    Emit(EntityRest{id.value, tick, state.position, state.rotation});
                    ++stats.rests;
                }
            }
            stats.maxAwakeBodies = std::max(stats.maxAwakeBodies, awake);
        }

        scene::Entity EntityOf(uint32_t netId) {
            for (const auto [entity, id] : simulation->Scene().Registry().view<const scene::NetId>().each())
                if (id.value == netId) return entity;
            return scene::kNoEntity;
        }

        /// A crate with its model origin at `corner` (stage 6): the playground's crate, a replicated
        /// placed object. 0 when it would leave the play bounds or the window's rows.
        uint32_t SpawnCrateAt(const glm::dvec3& corner, Peer* owner) {
            if (!crateModel) crateModel = sim::CrateModel(simulation->WorldMaterialIds(), sim::PlaygroundScale(simulation->World().VoxelSize()));
            const uint32_t netId = SpawnObjectAt(crateModel, corner, owner);
            if (netId != 0) ++stats.crates;
            return netId;
        }

        /// A replicated placed object with its model origin at `corner` (a crate, an admin object).
        /// 0 when it would leave the play bounds or the window's rows.
        uint32_t SpawnObjectAt(const std::shared_ptr<voxel::VoxelModel>& model, const glm::dvec3& corner, Peer* owner) {
            voxel::World& world = simulation->World();
            const double vs = static_cast<double>(world.VoxelSize());
            const glm::dvec3 size = glm::dvec3(model->Volume().Size()) * vs;
            const double chunk = static_cast<double>(voxel::kChunkSize) * vs;
            const glm::dvec2 lo = glm::dvec2(region.playMinChunk) * chunk, hi = glm::dvec2(region.playMaxChunk) * chunk;
            const glm::ivec2 rows = sim::RegionRows(region);
            const bool inside = std::isfinite(corner.x) && std::isfinite(corner.y) && std::isfinite(corner.z) && corner.x >= lo.x &&
                                corner.z >= lo.y && corner.x + size.x <= hi.x && corner.z + size.z <= hi.y &&
                                corner.y >= static_cast<double>(rows.x) * chunk && corner.y + size.y <= static_cast<double>(rows.y) * chunk;
            // A player's crate shares the snapshots with every awake body: none at the awake cap.
            const bool room = !owner || sim::CountPieces(*simulation).awakeBodies < simulation->Config().replication.maxAwakePieces;
            const scene::Entity entity =
                inside && room ? simulation->Scene().CreateVoxelObject({.model = model, .position = glm::vec3(corner)}) : scene::kNoEntity;
            if (entity == scene::kNoEntity) {
                ++stats.cratesRefused;
                return 0;
            }
            const uint32_t netId = sim::Replicate(*simulation, entity, false);
            // Moving: no player collides with it until it rests (TrackBodies), here as on every client.
            if (const scene::RigidBody* body = simulation->Scene().TryGet<scene::RigidBody>(entity))
                simulation->Physics().SetLayer(body->body, physics::Layer::Rubble);
            EmitObjectSpawn(netId, entity);
            if (owner && owner->player) {
                owner->player->crates.push_back(netId);
                while (owner->player->crates.size() > settings.maxCratesPerPlayer) {
                    const uint32_t oldest = owner->player->crates.front();
                    owner->player->crates.pop_front();
                    sim::Despawn(*simulation, EntityOf(oldest), scene::DespawnReason::Destroyed);
                }
            }
            DrainDespawns();
            return netId;
        }

        /// A crate edge of a player (its state after the input's tick, the input's look).
        void PlayerCrate(Peer& p, const sim::PlayerInput& input, const sim::PlayerState& at) {
            Player& pl = *p.player;
            const uint32_t now = Tick();
            if (pl.anyCrate && now - pl.lastCrateTick < settings.crateCooldownTicks) {
                ++stats.cratesRefused;
                return;
            }
            if (!crateModel) crateModel = sim::CrateModel(simulation->WorldMaterialIds(), sim::PlaygroundScale(simulation->World().VoxelSize()));
            const glm::dvec3 eye = glm::dvec3(at.position) + glm::dvec3(0.0, static_cast<double>(settings.player.eyeHeight), 0.0);
            const glm::dvec3 centre = eye + glm::dvec3(sim::LookDirection(input.yaw, input.pitch)) * static_cast<double>(settings.crateDistance);
            const glm::dvec3 half = glm::dvec3(crateModel->Volume().Size()) * (0.5 * static_cast<double>(simulation->World().VoxelSize()));
            if (SpawnCrateAt(centre - half, &p) != 0) {
                pl.anyCrate = true;
                pl.lastCrateTick = now;
            }
        }

        /// The awake replicated bodies (snapshot candidates), in NetId order.
        std::vector<BodySample> AwakeSamples() {
            std::vector<BodySample> out;
            const physics::PhysicsWorld& physics = simulation->Physics();
            for (const auto [entity, id, body] : simulation->Scene().Registry().view<const scene::NetId, const scene::RigidBody>().each()) {
                const auto it = tracks.find(id.value);
                if (it == tracks.end() || it->second.resting) continue;
                const physics::BodyState state = physics.GetState(body.body);
                BodySample b;
                b.netId = id.value;
                b.position = state.position;
                b.rotation = state.rotation;
                b.velocity = state.frozen ? glm::vec3(0.0f) : state.linearVelocity;
                out.push_back(b);
            }
            std::sort(out.begin(), out.end(), [](const BodySample& a, const BodySample& b) { return a.netId < b.netId; });
            return out;
        }

        uint32_t Tick() const { return simulation ? simulation->Tick() : 0; }
        uint32_t LastOpSeq() const { return lastOpSeq; }

        /// Logs and numbers one event. Only terrain operations charge the event credit (the cast
        /// queue's byte budget): entity events follow from Jolt's float poses and sleep (where a
        /// piece rests, which crate a blast catches), and a replayed server must put every cast on
        /// the same tick (shred.md R5; networking.md "Stage 6 as built"). They are counted apart.
        uint32_t Emit(const Event& event) {
            ByteWriter w;
            WriteEvent(w, event);
            const size_t bytes = w.Size();
            const bool entity = !std::holds_alternative<sim::WorldOp>(event);
            if (bytes > settings.eventBurstBytes) {
                ++stats.oversizeEvents;
                Log::Warn("Server: event {} is {} bytes, more than the {}-byte event burst (sent anyway)", lastOpSeq + 1, bytes,
                          settings.eventBurstBytes);
            }
            if (backlog.empty()) backlogFirst = lastOpSeq + 1;
            std::vector<uint8_t> encoded = w.Take();
            if (!encoded.empty() && encoded[0] < stats.eventBytesByKind.size()) {
                // Stage 8: bytes by event kind (the first byte of an event is its kind).
                stats.eventBytesByKind[encoded[0]] += bytes;
                ++stats.eventsByKind[encoded[0]];
                stats.maxEventBytesByKind[encoded[0]] = std::max<uint32_t>(stats.maxEventBytesByKind[encoded[0]], static_cast<uint32_t>(bytes));
            }
            backlog.push_back({Tick(), MaskVoxels(event), std::move(encoded)});
            backlogBytes += bytes;
            ++lastOpSeq;
            ++stats.events;
            stats.eventBytes += bytes;
            if (entity) {
                stats.entityEventBytes += bytes;
                entityCredit -= static_cast<int64_t>(bytes);
            } else {
                eventCredit -= static_cast<int64_t>(bytes);
            }
            return LastOpSeq();
        }

        /// A terrain operation applied to the server's world: numbered, and every chunk it changed is
        /// marked modified at its opSeq (stage 7a; a chunk it did not change still equals the
        /// generator's output where it did before).
        uint32_t EmitOp(const sim::WorldOp& op, const sim::OpResult& result) {
            const uint32_t seq = Emit(op);
            for (const glm::ivec3& c : result.report.changed) {
                modified[c] = seq;
                recentChanged.emplace_back(seq, c);
                if (!hashCache.empty()) hashStale.insert(c);
            }
            constexpr size_t kRecentChanged = 64;
            if (recentChanged.size() > kRecentChanged)
                recentChanged.erase(recentChanged.begin(), recentChanged.begin() + static_cast<std::ptrdiff_t>(recentChanged.size() - kRecentChanged));
            return seq;
        }

        /// False (and the session fails) when the server's own world could not apply `r` in full.
        bool CheckOp(const sim::OpResult& r) {
            if (r.Complete()) return true;
            ++stats.incompleteOps;
            Fail("an operation met chunks that are not loaded or have no store slot in the server's own window");
            return false;
        }

        /// networking.md "Server world without a camera": a broken server world ends the session.
        /// Every peer is disconnected at once (what was not delivered yet is dropped), nothing more
        /// is made or sent, queued casts are dropped and new peers are turned away (Tick).
        void Fail(const char* why) {
            if (failed) return;
            Log::Error("Server: the session is broken: {}; ending it ({} client(s) disconnected)", why, JoinedCount());
            failed = true;
            for (auto& [id, p] : peers) {
                stats.clientsDisconnected += p.joined ? 1u : 0u;
                DropPlayer(p);
                transport.Disconnect(id);
            }
            peers.clear();
            queue.clear();
            despawned.clear();
        }

        void DropPlayer(Peer& p) {
            if (!p.player) return;
            // Its crates go with it (review of stage 6: a reconnect loop left 8 bodies per connection).
            const std::deque<uint32_t> crates = p.player->crates;
            for (const uint32_t netId : crates)
                if (const scene::Entity e = EntityOf(netId); e != scene::kNoEntity) sim::Despawn(*simulation, e, scene::DespawnReason::Destroyed);
            simulation->Physics().DestroyCharacter(p.player->character);
            p.player.reset();
        }

        /// The joined peers with a player, in slot order (the order players are stepped in).
        std::vector<std::pair<PeerId, Peer*>> PlayersBySlot() {
            std::vector<std::pair<PeerId, Peer*>> out;
            for (auto& [id, p] : peers)
                if (p.joined && p.player) out.emplace_back(id, &p);
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second->slot < b.second->slot; });
            return out;
        }

        std::vector<glm::dvec3> PlayerFocus() const {
            std::vector<glm::dvec3> focus;
            for (const auto& [id, p] : peers)
                if (p.player) focus.push_back(glm::dvec3(p.player->state.position));
            return focus;
        }

        /// A cast edge the cooldown accepted, captured as {origin, direction, tick, loadout}
        /// (networking.md "Time and ticks"): the eye of the player's state after the input's tick
        /// (`at`) and the look of `input`. The ray is cast and the blast run when the cast queue
        /// reaches it (RunCasts), on the world as it is then.
        void CaptureCast(Peer& p, const sim::PlayerInput& input, const sim::PlayerState& at, bool late) {
            const sim::PlayerTuning& t = settings.player;
            QueuedCast q;
            q.ray = true;
            q.origin = glm::dvec3(at.position) + glm::dvec3(0.0, static_cast<double>(t.eyeHeight), 0.0);
            q.direction = sim::LookDirection(input.yaw, input.pitch);
            q.inputTick = input.tick;
            q.power = voxel::QuantizePower(t.castPower);
            q.queuedTick = Tick();
            q.player = static_cast<uint8_t>(p.slot + 1);
            ++stats.castsFromInputs;
            stats.castsFromLateInputs += late ? 1u : 0u;
            ++stats.castsQueued;
            p.player->lastCastTick = input.tick;
            p.player->castRejected = CastRejection::None;
            queue.push_back(q);
        }

        /// A cast edge the cooldown refused: reported to its owner (it predicted the same cooldown,
        /// so this is a misprediction or a forged input).
        void RejectCast(Peer& p, uint32_t inputTick) {
            ++stats.castsOnCooldown;
            p.player->lastCastTick = inputTick;
            p.player->castRejected = CastRejection::Cooldown;
        }

        bool Queue(const voxel::SphereQ& sphere, uint16_t power, uint8_t player) {
            ++stats.castsQueued;
            if (!sim::CastFitsRegion(region, simulation->World(), sphere)) {
                ++stats.castsRejected;
                return false;
            }
            QueuedCast q;
            q.sphere = sphere;
            q.power = power;
            q.queuedTick = Tick();
            q.player = player;
            queue.push_back(q);
            return true;
        }

        Peer* PeerOfSlot(uint8_t player) {
            if (player == 0) return nullptr;
            for (auto& [id, p] : peers)
                if (p.joined && p.player && p.slot + 1u == player) return &p;
            return nullptr;
        }

        /// A player's cast at dequeue: its ray into the world as it is now. False (counted, reported
        /// to the owner if it is still there) when it meets no terrain within castRange or the hit
        /// does not fit the region (sim::CastFitsRegion).
        bool ResolveRay(QueuedCast& q) {
            const sim::PlayerTuning& t = settings.player;
            voxel::WorldRayHit hit = simulation->World().Raycast(q.origin, q.direction, t.castRange);
            // A replicated body in front of the terrain takes the cast (a crate, a falling piece), as
            // offline (review of stage 6). Every body of the server's in these layers is replicated.
            const physics::RayHit body = simulation->Physics().Raycast(
                glm::vec3(q.origin), q.direction, hit.hit ? static_cast<float>(hit.distance) : t.castRange,
                physics::LayerBit(physics::Layer::Dynamic) | physics::LayerBit(physics::Layer::Rubble));
            if (body.hit && simulation->Scene().TryGet<scene::NetId>(simulation->Scene().EntityFromUserData(body.userData))) {
                hit.hit = true;
                hit.position = glm::dvec3(body.position);
                hit.distance = body.distance;
                ++stats.castsAtBodies;
            }
            CastRejection why = CastRejection::None;
            if (!hit.hit) {
                ++stats.castsMissed;
                why = CastRejection::Missed;
            } else {
                const double vs = static_cast<double>(simulation->World().VoxelSize());
                q.sphere = voxel::QuantizeSphere(hit.position / vs, static_cast<double>(t.castRadius) / vs);
                if (!sim::CastFitsRegion(region, simulation->World(), q.sphere)) {
                    ++stats.castsRejected;
                    why = CastRejection::Outside;
                }
            }
            if (why == CastRejection::None) return true;
            if (Peer* p = PeerOfSlot(q.player); p != nullptr && p->player->lastCastTick == q.inputTick) p->player->castRejected = why;
            return false;
        }

        /// A cast edge in a late input (tick `in.tick` < `now`): judged and captured at the input's
        /// tick, as its client predicted it (networking.md "Time and ticks", stage 5). The cooldown is
        /// the one the player had after that tick; if it is taken, the cooldown now is what it would
        /// be had the cast been taken then (the owner predicted exactly that). Origin: the state
        /// after the input's tick, or the current one if that is no longer kept.
        void LateCast(Peer& p, const sim::PlayerInput& in, uint32_t now) {
            Player& pl = *p.player;
            const sim::PlayerState* at = pl.StateAt(in.tick);
            const uint8_t cooldownThen = at != nullptr ? at->cooldownTicks : pl.state.cooldownTicks;
            if (cooldownThen > 0 || pl.state.cooldownTicks > 0) return RejectCast(p, in.tick);
            const uint32_t elapsed = now - 1 - in.tick;   // ticks simulated after the input's tick
            const uint32_t full = settings.player.castCooldownTicks;
            pl.state.cooldownTicks = static_cast<uint8_t>(full > elapsed ? full - elapsed : 0u);
            CaptureCast(p, in, at != nullptr ? *at : pl.state, true);
            // The kept states of the ticks since then get the cooldown they would have had, so that a
            // later late edge in the same stream is judged as its client predicted it (review of
            // stage 5: they still said 0, and a second edge a few ticks later was taken).
            // (StepPlayer counts down before it takes a cast: after tick in.tick + j the cooldown is full - j.)
            for (uint32_t k = std::max(in.tick, now > kStateHistory ? now - kStateHistory : 0u); k < now; ++k)
                if (sim::PlayerState* kept = pl.StateAt(k))
                    kept->cooldownTicks = static_cast<uint8_t>(full > k - in.tick ? full - (k - in.tick) : 0u);
        }

        /// An InputBatch from a joined client (state channel: lost, duplicated, reordered).
        void OnInputs(Peer& p, std::span<const uint8_t> bytes) {
            InputBatch batch;
            if (!p.player || !Decode(bytes, batch)) {
                ++stats.malformedInputs;
                return;
            }
            ++stats.inputPackets;
            Player& pl = *p.player;
            const uint32_t now = Tick();   // the tick simulated next
            uint32_t newest = 0;
            bool any = false;
            for (const sim::PlayerInput& in : batch.inputs) {
                const uint32_t t = in.tick;
                if (pl.anyInput && t <= pl.lastRealTick) {
                    ++stats.inputsDuplicate;
                } else if (t < pl.joinTick) {
                    // No client makes an input for a tick before its player existed (it predicts from
                    // its first snapshot on): a forged or broken one is ignored.
                    ++stats.inputsBeforeJoin;
                    continue;
                } else if (t < now) {
                    // Too late for its tick: it only refreshes the last input. Its edges still count
                    // (a cast in it is captured once), derived against the levels the repeats kept.
                    ++stats.inputsLate;
                    const uint8_t pressed = sim::PressButtons(pl.state, in.buttons);
                    if ((pressed & sim::PlayerButton_Cast) != 0) LateCast(p, in, now);
                    if ((pressed & sim::PlayerButton_Crate) != 0) PlayerCrate(p, in, pl.state);
                    // A jump edge is not spent here without a jump: the next step makes it.
                    if ((pressed & sim::PlayerButton_Jump) != 0 && !pl.lateJump) {
                        pl.lateJump = true;
                        ++stats.jumpsFromLateInputs;
                    }
                    pl.last = in;
                    pl.lastRealTick = t;
                    pl.anyInput = true;
                    pl.queue.erase(pl.queue.begin(), pl.queue.upper_bound(t));
                } else if (t - now >= settings.inputQueueTicks) {
                    ++stats.inputsTooEarly;
                    continue;
                } else if (!pl.queue.emplace(t, in).second) {
                    ++stats.inputsDuplicate;
                } else {
                    ++stats.inputsQueued;
                }
                newest = any ? std::max(newest, t) : t;
                any = true;
            }
            if (batch.inputs.empty()) return;
            if (any) pl.ackTick = std::max(pl.ackTick, newest);
            // The margin of the newest input of the packet, dropped ones too (the client is too far
            // ahead: exactly what its clock must hear), unless a packet with newer inputs already
            // arrived (reordered or duplicated: it would pair an older, smaller margin with the tick).
            const uint32_t last = batch.inputs.back().tick;
            if (pl.margin != kNoInputMargin && last <= pl.marginTick) return;
            const int64_t margin = static_cast<int64_t>(last) - static_cast<int64_t>(now);
            // No client is that far off: a forged tick must not become the newest for good.
            if (margin > kMaxReportedMargin || margin < -kMaxReportedMargin) return;
            pl.margin = static_cast<int16_t>(std::clamp<int64_t>(margin, -32767, 32767));
            pl.marginTick = last;
        }

        /// Every player, in slot order, steps with its input for this tick or the last movement repeated.
        void StepPlayers() {
            const uint32_t now = Tick();
            const float dt = simulation->FixedTimeStep();
            for (auto& [id, peer] : PlayersBySlot()) {
                Player& pl = *peer->player;
                sim::PlayerInput input;
                const auto it = pl.queue.find(now);
                if (it != pl.queue.end()) {
                    input = it->second;
                    pl.last = input;
                    pl.lastRealTick = now;
                    pl.anyInput = true;
                } else {
                    // Missing: the last movement for repeatInputTicks, then none. The levels stay those of
                    // the last input, so a repeat never makes an edge.
                    input = pl.last;
                    input.buttons = pl.state.lastButtons;
                    ++stats.ticksRepeated;
                    if (!pl.anyInput || now - pl.lastRealTick > settings.repeatInputTicks) {
                        input.moveX = input.moveZ = 0;
                        ++stats.ticksStood;
                    }
                }
                pl.queue.erase(pl.queue.begin(), pl.queue.upper_bound(now));
                input.tick = now;
                // The colliders this step can touch hold every edit made so far (never a worker's timing).
                tickFlushCooks += simulation->Terrain().FlushBox(sim::PlayerReach(pl.state.position, settings.player));
                const uint8_t levels = input.buttons;
                const bool lateJump = pl.lateJump;
                if (lateJump) {
                    // The edge of a late input's jump is made now; the levels stay this input's.
                    pl.state.lastButtons = static_cast<uint8_t>(pl.state.lastButtons & ~sim::PlayerButton_Jump);
                    input.buttons = static_cast<uint8_t>(input.buttons | sim::PlayerButton_Jump);
                    pl.lateJump = false;
                }
                const sim::PlayerStepResult r = sim::StepPlayer(simulation->Physics(), simulation->Terrain(), pl.character, pl.state, input,
                                                                settings.player, playArea, dt);
                if (lateJump) pl.state.lastButtons = static_cast<uint8_t>(levels & sim::kPlayerButtonMask);
                stats.heldTicks += r.held ? 1u : 0u;
                pl.Store(now);
                if (r.cast) CaptureCast(*peer, input, pl.state, false);
                else if ((r.pressed & sim::PlayerButton_Cast) != 0) RejectCast(*peer, now);
                if ((r.pressed & sim::PlayerButton_Crate) != 0) PlayerCrate(*peer, input, pl.state);
            }
        }

        void SendSnapshots(uint32_t tick) {
            const std::vector<std::pair<PeerId, Peer*>> players = PlayersBySlot();
            const std::vector<BodySample> awake = AwakeSamples();
            for (const auto& [id, peer] : players) {
                Snapshot snap;
                snap.tick = tick;
                snap.lastOpSeq = LastOpSeq();
                snap.ackInputTick = peer->player->ackTick;
                snap.marginTick = peer->player->marginTick;
                snap.inputMargin = peer->player->margin;
                snap.hasOwner = true;
                snap.owner = peer->player->state;
                snap.lastCastTick = peer->player->lastCastTick;
                snap.castRejected = peer->player->castRejected;
                for (const auto& [otherId, other] : players) {
                    if (otherId == id) continue;
                    snap.others.push_back({other->slot, other->player->state.position, other->player->last.yaw, other->player->state.flags});
                }
                // Bodies by priority: what was left out longest, nearest first, as many as fit.
                const size_t fixed = kSnapshotHeaderBytes + kSnapshotOwnerBytes + snap.others.size() * kSnapshotPlayerBytes;
                const size_t room = settings.snapshotBytes > fixed ? std::min<size_t>((settings.snapshotBytes - fixed) / kSnapshotBodyBytes, 255) : 0;
                std::unordered_map<uint32_t, float> priority;
                std::vector<std::pair<float, size_t>> order;
                order.reserve(awake.size());
                const float scale = std::max(settings.bodyPriorityMetres, 0.001f);
                for (size_t i = 0; i < awake.size(); ++i) {
                    const auto it = peer->priority.find(awake[i].netId);
                    const float waited = (it != peer->priority.end() ? it->second : 0.0f) + 1.0f;
                    priority[awake[i].netId] = waited;
                    const float distance = glm::distance(awake[i].position, peer->player->state.position);
                    order.emplace_back(waited / (1.0f + distance / scale), i);
                }
                std::sort(order.begin(), order.end(), [&awake](const auto& a, const auto& b) {
                    return a.first != b.first ? a.first > b.first : awake[a.second].netId < awake[b.second].netId;
                });
                for (size_t k = 0; k < order.size() && k < room; ++k) {
                    const BodySample& b = awake[order[k].second];
                    snap.bodies.push_back(b);
                    priority[b.netId] = 0.0f;
                }
                peer->priority = std::move(priority);
                stats.bodySamples += snap.bodies.size();
                stats.bodiesLeftOut += awake.size() - snap.bodies.size();
                stats.maxSnapshotBodies = std::max(stats.maxSnapshotBodies, static_cast<uint32_t>(snap.bodies.size()));
                const std::vector<uint8_t> bytes = Encode(snap);
                stats.maxSnapshotBytes = std::max(stats.maxSnapshotBytes, bytes.size());
                ++stats.snapshotHistogram[std::min<size_t>(bytes.size() / 64, stats.snapshotHistogram.size() - 1)];
                ++stats.snapshots;
                stats.snapshotBytes += bytes.size();
                Send(id, Channel::State, bytes);
            }
        }

        uint32_t JoinedCount() const {
            uint32_t n = 0;
            for (const auto& [id, p] : peers) n += p.joined ? 1u : 0u;
            return n;
        }

        void Send(PeerId peer, Channel channel, const std::vector<uint8_t>& bytes) {
            transport.Send(peer, channel, bytes);
            stats.sentBytes += bytes.size();
            if (channel == Channel::Events)
                if (const auto it = peers.find(peer); it != peers.end()) it->second.eventBytesSince += bytes.size();
        }

        /// Sends the batch that starts after `from` (events of one tick, at most maxBatchBytes and
        /// kMaxMaskVoxels of masks; one larger event goes alone) and returns its last opSeq.
        uint32_t SendBatch(PeerId peer, uint32_t from, uint32_t to) {
            const uint32_t seq = from + 1;
            const auto at = [this](uint32_t opSeq) -> const LoggedEvent& { return backlog[opSeq - backlogFirst]; };
            const uint32_t tick = at(seq).tick;
            std::vector<uint8_t> bytes;
            uint64_t maskVoxels = 0;
            uint32_t count = 0;
            uint32_t s = seq;
            while (s <= to && at(s).tick == tick && count < kMaxBatchEvents) {
                const LoggedEvent& e = at(s);
                if (count > 0 && (bytes.size() + e.bytes.size() > settings.maxBatchBytes || maskVoxels + e.maskVoxels > kMaxMaskVoxels)) break;
                bytes.insert(bytes.end(), e.bytes.begin(), e.bytes.end());
                maskVoxels += e.maskVoxels;
                ++count;
                ++s;
            }
            Send(peer, Channel::Events, EncodeBatch(seq, tick, count, bytes));
            return s - 1;
        }

        /// Hands a peer its next batches (all of them, unless a test holds them while
        /// maxEventBytesInFlight is on its way to it).
        void Pump(PeerId id, Peer& p) {
            const uint32_t last = LastOpSeq();
            const size_t limit = settings.maxEventBytesInFlight;
            while (p.sentUpTo < last) {
                if (limit > 0 && transport.ReliableBytesInFlight(id) >= limit) {
                    ++stats.sendsHeld;
                    break;
                }
                p.sentUpTo = SendBatch(id, p.sentUpTo, last);
            }
            stats.maxBacklogEvents = std::max(stats.maxBacklogEvents, last - p.sentUpTo);
        }

        void Refuse(PeerId peer, Peer& p, RefuseReason reason) {
            Send(peer, Channel::Events, Encode(net::Refuse{reason}));
            p.closing = true;
            p.closingTick = Tick();
            ++stats.clientsRefused;
            Log::Info("Server: refused a client: {}", ToString(reason));
        }

        void OnHello(PeerId peer, Peer& p, std::span<const uint8_t> bytes) {
            Hello hello;
            if (!Decode(bytes, hello)) return Refuse(peer, p, RefuseReason::Malformed);
            if (hello.protocolVersion != kProtocolVersion) return Refuse(peer, p, RefuseReason::Protocol);
            if (hello.buildHash != buildHash) return Refuse(peer, p, RefuseReason::Build);
            if (hello.token != settings.session.token) return Refuse(peer, p, RefuseReason::Token);
            uint32_t joined = 0;
            std::vector<bool> used(256, false);
            for (const auto& [id, other] : peers)
                if (other.joined) {
                    ++joined;
                    used[other.slot] = true;
                }
            if (joined >= settings.session.maxClients) return Refuse(peer, p, RefuseReason::Full);
            uint8_t slot = 0;
            while (used[slot]) ++slot;
            Welcome welcome;
            welcome.slot = slot;
            welcome.tick = Tick();
            welcome.region = region;
            welcome.spawn = tmpl.spawn;
            welcome.player = settings.player;
            welcome.tickSeconds = simulation->FixedTimeStep();
            welcome.gravity = simulation->Config().physics.gravity;
            welcome.joinOpSeq = LastOpSeq();
            welcome.probes = probes;
            Send(peer, Channel::Events, Encode(welcome));
            // Join by chunk sync (stage 7a): the live stream from the join point on; what was made
            // before comes as ChunkData (its WindowUpdate says where) and the entity baseline.
            p.joined = true;
            p.slot = slot;
            p.sentUpTo = LastOpSeq();
            p.pacer = std::make_unique<BulkPacer>(settings.bulkPacer);
            p.planner = HashCheckPlanner(settings.hashCheck);
            p.join = JoinRecord{};
            p.join.joinTick = Tick();
            p.join.joinOpSeq = welcome.joinOpSeq;
            QueueBaseline(p, glm::dvec3(tmpl.spawn));
            // Its player stands at the spawn point; the ground under every player gets its collider
            // before the next step (a gate that held the player here would hold it for another
            // number of ticks than its client's): the spawn is a permanent collider focus (built at
            // Start, kept current by the background cooks) and this tick's StepPlayers cooks whatever
            // of the player's reach is still stale (FlushBox). Stage 8: no global Terrain().Flush()
            // here any more - it cooked every pending collider of the window in the join tick (126 ms
            // in the owner's run, the first join after the spawn's colliders were dropped).
            p.player = std::make_unique<Player>();
            p.player->joinTick = Tick();          // Receive runs at the start of the tick that steps it first
            p.player->state.position = glm::vec3(tmpl.spawn);
            p.player->character = simulation->Physics().CreateCharacter(sim::PlayerCharacterDesc(p.player->state.position, settings.player));
            tickEvents |= kTickHello;
            ++stats.clientsJoined;
            Log::Info("Server: client {} joined at tick {} (slot {}, join point opSeq {}, {} entities in its baseline, {} modified chunks)",
                      peer, Tick(), slot, LastOpSeq(), p.baseline.size(), modified.size());
        }

        // --- chunk sync (stage 7a) ---------------------------------------------------------------

        /// The NetIds alive now (template lights, replicated objects), nearest `at` first: the join's
        /// baseline, made into BaselineEntity messages when they are dequeued.
        void QueueBaseline(Peer& p, const glm::dvec3& at) {
            // Nearest first by the distance to the nearest point of the model's bounding sphere (the
            // body's origin is a corner of its model): the near set is a prefix of the order.
            std::vector<std::pair<double, uint32_t>> ids;
            for (const auto& [netId, light] : lights) ids.emplace_back(glm::distance(glm::dvec3(light.position), at), netId);
            const physics::PhysicsWorld& physics = simulation->Physics();
            const double voxelSize = static_cast<double>(simulation->World().VoxelSize());
            const scene::Scene& scene = simulation->Scene();
            for (const auto [entity, id, body] : scene.Registry().view<const scene::NetId, const scene::RigidBody>().each()) {
                double extent = 0.0;
                if (const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity); voxels && voxels->model)
                    extent = glm::length(glm::dvec3(voxels->model->Volume().Size())) * voxelSize;
                ids.emplace_back(std::max(0.0, glm::distance(glm::dvec3(physics.GetState(body.body).position), at) - extent), id.value);
            }
            std::sort(ids.begin(), ids.end());
            p.baseline.clear();
            p.nearLeft = 0;
            p.nearSent = false;
            for (const auto& [d, netId] : ids) {
                p.baseline.push_back(netId);
                p.nearLeft += d <= static_cast<double>(settings.baselineNearMetres) ? 1u : 0u;
            }
            p.baselineOpen = true;
            p.baselineSent = 0;
        }

        /// A replicated entity as it is now (light or object; a resting object at its EntityRest pose
        /// bit for bit, zero velocity). False when it is gone.
        bool MakeBaselineEntity(uint32_t netId, BaselineEntity& out) {
            out = BaselineEntity{};
            out.asOfOpSeq = LastOpSeq();
            // The tick whose physics made the state (Flush runs after StepPhysics, which already
            // counted the next tick): snapshots, EntityRest and the rest tick label it the same
            // (review of stage 7a).
            out.tick = steppedTick;
            out.spawn.netId = netId;
            if (const auto light = lights.find(netId); light != lights.end()) {
                out.spawn.kind = EntityKind::Light;
                out.spawn.light = light->second;
                return true;
            }
            const scene::Entity entity = EntityOf(netId);
            if (entity == scene::kNoEntity) return false;
            out.spawn.kind = EntityKind::Object;
            if (!DescribeObject(entity, out.spawn.object)) return false;
            const auto t = tracks.find(netId);
            const bool resting = t != tracks.end() && t->second.resting;
            if (resting) {
                out.restTick = t->second.restTick;
                out.spawn.object.position = t->second.restPosition;
                out.spawn.object.rotation = t->second.restRotation;
                out.spawn.object.linearVelocity = glm::vec3(0.0f);
                out.spawn.object.angularVelocity = glm::vec3(0.0f);
                out.spawn.object.flags = kObjectResting;
            } else {
                out.spawn.object.flags = 0;
            }
            return true;
        }

        bool QueueChunk(Peer& p, const glm::ivec3& c) {
            if (!p.queued.insert(c).second) return false;
            p.chunks.push_back(c);
            p.sortedTail = 0;             // the back is no longer the nearest: selected again before use
            return true;
        }

        /// Horizontal distance (metres) from a chunk's centre to the peer's player; for a peer in
        /// server terrain mode the 3D distance from its eyes (stage 7b choice 6: the surface it sees
        /// before the buried rock under it).
        double ChunkDistance(const Peer& p, const glm::ivec3& c) const {
            const double size = static_cast<double>(voxel::kChunkSize) * static_cast<double>(simulation->World().VoxelSize());
            const glm::dvec3 player = p.player ? glm::dvec3(p.player->state.position) : glm::dvec3(tmpl.spawn);
            if (p.terrainMode) {
                return glm::length((glm::dvec3(c) + glm::dvec3(0.5)) * size - (player + glm::dvec3(0.0, kEyeMetres, 0.0)));
            }
            const glm::dvec2 centre = (glm::dvec2(c.x, c.z) + glm::dvec2(0.5)) * size;
            return glm::length(centre - glm::dvec2(player.x, player.z));
        }

        /// The queue's nearest `kSortedTail` entries, selected and sorted at its back (pop_back = nearest),
        /// again when the player's chunk changed, something was added or the sorted tail ran out (review
        /// of stage 7b: a terrain-mode queue holds about 20 000 chunks; selecting is O(n), a full sort per
        /// chunk crossing cost 2-4 ms there). The key is the squared distance (ChunkDistance's order
        /// without a square root), ties in (y, z, x): the order is the same on every run.
        static constexpr size_t kSortedTail = 1024;
        void SortChunks(Peer& p, bool force) {
            const glm::ivec3 at = p.player ? voxel::VoxelToChunk(simulation->World().WorldToVoxel(glm::dvec3(p.player->state.position)))
                                           : glm::ivec3(0);
            if (!force && at == p.sortedFor && (p.sortedTail > 0 || p.chunks.empty())) return;
            p.sortedFor = at;
            if (p.chunks.empty()) {
                p.sortedTail = 0;
                return;
            }
            const auto start = std::chrono::steady_clock::now();
            const double size = static_cast<double>(voxel::kChunkSize) * static_cast<double>(simulation->World().VoxelSize());
            const glm::dvec3 player = p.player ? glm::dvec3(p.player->state.position) : glm::dvec3(tmpl.spawn);
            const glm::dvec3 eye = player + glm::dvec3(0.0, kEyeMetres, 0.0);
            const auto key = [&](const glm::ivec3& c) {
                if (p.terrainMode) {
                    const glm::dvec3 d = (glm::dvec3(c) + glm::dvec3(0.5)) * size - eye;
                    return glm::dot(d, d);
                }
                const glm::dvec2 d = (glm::dvec2(c.x, c.z) + glm::dvec2(0.5)) * size - glm::dvec2(player.x, player.z);
                return glm::dot(d, d);
            };
            std::vector<std::pair<double, glm::ivec3>> keyed;
            keyed.reserve(p.chunks.size());
            for (const glm::ivec3& c : p.chunks) keyed.emplace_back(key(c), c);
            const auto farther = [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first > b.first;
                return std::tie(a.second.y, a.second.z, a.second.x) > std::tie(b.second.y, b.second.z, b.second.x);
            };
            if (keyed.size() > kSortedTail) {
                const auto tail = keyed.end() - static_cast<std::ptrdiff_t>(kSortedTail);
                std::nth_element(keyed.begin(), tail, keyed.end(), farther);
                std::sort(tail, keyed.end(), farther);
                p.sortedTail = kSortedTail;
            } else {
                std::sort(keyed.begin(), keyed.end(), farther);
                p.sortedTail = keyed.size();
            }
            for (size_t i = 0; i < keyed.size(); ++i) p.chunks[i] = keyed[i].second;
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            stats.queueSortSeconds += seconds;
            stats.maxQueueSortSeconds = std::max(stats.maxQueueSortSeconds, seconds);
        }

        /// The nearest queued chunk is at the back (the sorted tail selected again when it ran out).
        void PopChunk(Peer& p) {
            p.chunks.pop_back();
            if (p.sortedTail > 0) --p.sortedTail;
        }

        /// Queued chunks that left the peer's new window go now (review of stage 7b: on a slow link a
        /// walker's left-behind slabs stayed queued, at the far end of the queue, for the session).
        /// Request answers in `front` are left to the dequeue. Keeps the queue's order.
        void PruneQueue(Peer& p, const SyncBox& box) {
            const size_t before = p.chunks.size();
            std::erase_if(p.chunks, [&](const glm::ivec3& c) {
                if (box.Contains(c)) return false;
                p.queued.erase(c);
                if (p.terrainMode) TerrainChunkGone(p, c, 0);
                return true;
            });
            stats.queuePruned += before - p.chunks.size();
            if (p.chunks.size() != before) p.sortedTail = 0;     // selected again before the next use
        }

        /// A terrain-mode chunk queued by update `seq` (its bytes count to that update when it goes).
        void TerrainQueued(Peer& p, const glm::ivec3& c, uint32_t seq) {
            if (const auto it = p.terrainQueuedBy.find(c); it != p.terrainQueuedBy.end()) {
                if (it->second == seq) return;
                TerrainChunkGone(p, c, 0);
            }
            p.terrainQueuedBy[c] = seq;
            ++p.terrainSteps[seq].pending;
        }

        /// A terrain-mode chunk left the queue: sent (`bytes`), pruned or dropped (0). Its update is
        /// finished once nothing it queued is left.
        void TerrainChunkGone(Peer& p, const glm::ivec3& c, uint64_t bytes) {
            const auto it = p.terrainQueuedBy.find(c);
            if (it == p.terrainQueuedBy.end()) return;
            const uint32_t seq = it->second;
            p.terrainQueuedBy.erase(it);
            if (seq == p.terrainFirstSeq) p.terrainJoinDataBytes += bytes;
            const auto step = p.terrainSteps.find(seq);
            if (step == p.terrainSteps.end()) return;
            step->second.bytes += bytes;
            if (step->second.pending > 0) --step->second.pending;
            if (step->second.pending == 0) FinishTerrainStep(p, seq);
        }

        void FinishTerrainStep(Peer& p, uint32_t seq) {
            const auto step = p.terrainSteps.find(seq);
            if (step == p.terrainSteps.end()) return;
            if (seq != p.terrainFirstSeq) {
                ++p.terrainStepCount;
                p.terrainStepBytes += step->second.bytes;
                p.terrainMaxStepBytes = std::max(p.terrainMaxStepBytes, step->second.bytes);
            }
            p.terrainSteps.erase(step);
        }

        /// WindowUpdate: entering chunks (all of them with reset or for the first) the modified map
        /// holds are queued and get their bit in the ack; the ack goes at once, after the events made
        /// so far (so a client applies it with every event up to its asOf applied).
        bool OnWindowUpdate(PeerId id, Peer& p, std::span<const uint8_t> bytes) {
            WindowUpdate u;
            if (!Decode(bytes, u) || u.windowSeq != p.windowSeq + 1) return false;
            // Server terrain mode is sticky (stage 7b choice 12): a peer that asked for it once and
            // then clears the flag would disagree with the server about what the answer means.
            const bool terrain = (u.flags & WindowUpdate::kTerrainMode) != 0;
            if (p.terrainMode && !terrain) return false;
            const glm::ivec2 rows = sim::RegionRows(region);
            if (u.minChunk.y != rows.x || u.size.y != rows.y - rows.x) return false;
            ++stats.windowUpdates;
            if (!p.hasWindow) tickEvents |= kTickFirstWindow;
            const SyncBox box{u.minChunk, u.size};
            const bool all = WindowEntersWhole(!p.hasWindow, (u.flags & WindowUpdate::kReset) != 0, p.window, box);
            if (terrain) return OnTerrainWindow(id, p, u, box, all);
            if (p.hasWindow) PruneQueue(p, box);
            WindowAck ack;
            ack.windowSeq = u.windowSeq;
            ack.modified = voxel::VoxelMask(u.size);
            uint32_t marked = 0;
            const SyncBox server = RegionSyncBox(region);
            const SyncBox inside = Intersect(box, server);
            size_t fresh = 0;
            auto mark = [&](const glm::ivec3& c) {
                ack.modified.Set(c - box.min, true);
                ++marked;
                fresh += QueueChunk(p, c) ? 1u : 0u;
            };
            const size_t cells = static_cast<size_t>(inside.size.x) * static_cast<size_t>(inside.size.y) * static_cast<size_t>(inside.size.z);
            if (inside.Empty()) {
                // Nothing of the server's window: every chunk of it is pristine for good.
            } else if (modified.size() < cells) {
                // Fewer modified chunks than cells in the box (early in a session, a large window): one
                // pass over the map. The queue's order does not depend on the map's (SortChunks).
                for (const auto& [c, seq] : modified)
                    if (inside.Contains(c) && (all || !p.window.Contains(c))) mark(c);
            } else {
                for (int y = inside.min.y; y < inside.Max().y; ++y)
                    for (int z = inside.min.z; z < inside.Max().z; ++z)
                        for (int x = inside.min.x; x < inside.Max().x; ++x) {
                            const glm::ivec3 c(x, y, z);
                            if (!all && p.window.Contains(c)) continue;
                            if (modified.contains(c)) mark(c);
                        }
            }
            p.hasWindow = true;
            p.window = box;
            p.windowSeq = u.windowSeq;
            if (fresh > 0) SortChunks(p, true);
            p.maxQueued = std::max(p.maxQueued, static_cast<uint32_t>(p.chunks.size()));
            // The events made so far go first: a client takes the ack with them applied.
            Pump(id, p);
            ack.asOfOpSeq = LastOpSeq();
            Send(id, Channel::Events, Encode(ack));
            ++stats.windowAcks;
            stats.ackedModified += marked;
            if (u.windowSeq == 1) {
                p.join.windowChunks = static_cast<uint32_t>(box.size.x * box.size.y * box.size.z);
                p.join.modifiedChunks = marked;
            }
            return true;
        }

        /// A terrain-mode WindowUpdate (stage 7b M-b, server terrain mode): every entering chunk inside
        /// the server's window is described as the server holds it now - uniform ones in the ack, the
        /// others queued as ChunkData (code 1 Dense, code 3 Packed: the client packs those as the
        /// server does) - modified or not. The ack goes at once after the events made so far, like a
        /// WindowAck, so the client takes it with every event up to its asOf applied.
        bool OnTerrainWindow(PeerId id, Peer& p, const WindowUpdate& u, const SyncBox& box, bool all) {
            const auto start = std::chrono::steady_clock::now();
            if (p.hasWindow) PruneQueue(p, box);
            const bool step = p.terrainMode && !all;
            const size_t direction = box.min.x != p.window.min.x ? (box.min.z != p.window.min.z ? 2u : 0u) : 1u;
            if (!p.terrainMode) {
                p.terrainMode = true;
                p.terrainFirstSeq = u.windowSeq;
                ++stats.terrainModePeers;
                if (settings.terrainBulkCeilingBytes > settings.bulkPacer.ceilingBytes) {
                    // Stage 8, O2: a larger bulk window for a peer that receives its whole window.
                    BulkPacerSettings ts = settings.bulkPacer;
                    ts.ceilingBytes = settings.terrainBulkCeilingBytes;
                    p.pacer = std::make_unique<BulkPacer>(ts);
                }
                Log::Info("Server: client {} asks for server terrain mode at window update {}: the whole window comes from here", id,
                          u.windowSeq);
            }
            const voxel::World& world = simulation->World();
            const SyncBox server = RegionSyncBox(region);
            TerrainAck ack;
            ack.windowSeq = u.windowSeq;
            ack.size = u.size;
            uint32_t queuedNow = 0, uniform = 0;
            const auto add = [&ack](TerrainCode code, uint8_t id) {
                if (!ack.runs.empty() && ack.runs.back().code == code && ack.runs.back().id == id) {
                    ++ack.runs.back().count;
                    return;
                }
                ack.runs.push_back({1, code, id});
            };
            // One code per entering cell in ForEachEnteringChunk order (protocol 10, review of stage 7b):
            // a step visits and codes its entering slab only, never the whole box.
            ForEachEnteringChunk(box, p.window, all, [&](const glm::ivec3& c) {
                if (!server.Contains(c)) {
                    add(TerrainCode::NotInAck, 0);
                    return;
                }
                switch (world.StateOf(c)) {
                    case voxel::CellState::Empty:
                        add(TerrainCode::Uniform, 0);
                        ++uniform;
                        return;
                    case voxel::CellState::Uniform: {
                        uint8_t id = 0;
                        (void)world.GetChunk(c, &id);
                        add(TerrainCode::Uniform, id);
                        ++uniform;
                        return;
                    }
                    case voxel::CellState::Packed:
                        add(TerrainCode::HiddenData, 0);
                        ++stats.terrainHidden;
                        break;
                    default:          // Dense (the server's window is always loaded)
                        add(TerrainCode::Data, 0);
                        ++stats.terrainData;
                        break;
                }
                if (QueueChunk(p, c)) ++queuedNow;
                TerrainQueued(p, c, u.windowSeq);
            });
            stats.terrainUniform += uniform;
            p.hasWindow = true;
            p.window = box;
            p.windowSeq = u.windowSeq;
            if (queuedNow > 0) SortChunks(p, true);
            p.maxQueued = std::max(p.maxQueued, static_cast<uint32_t>(p.chunks.size()));
            Pump(id, p);
            ack.asOfOpSeq = LastOpSeq();
            const std::vector<uint8_t> bytes = Encode(ack);
            Send(id, Channel::Events, bytes);
            ++stats.windowAcks;
            ++stats.terrainAcks;
            stats.terrainAckBytes += bytes.size();
            p.terrainSteps[u.windowSeq].bytes += bytes.size();
            if (step) {
                ++p.ackSteps[direction];
                p.ackStepBytes[direction] += bytes.size();
                p.maxAckStepBytes[direction] = std::max<uint64_t>(p.maxAckStepBytes[direction], bytes.size());
            }
            if (p.terrainSteps[u.windowSeq].pending == 0) FinishTerrainStep(p, u.windowSeq);
            if (u.windowSeq == p.terrainFirstSeq) {
                p.terrainJoinAckBytes = bytes.size();
                p.terrainJoinChunks = queuedNow;
                p.terrainJoinUniform = uniform;
            }
            if (u.windowSeq == 1) {
                p.join.windowChunks = static_cast<uint32_t>(box.size.x * box.size.y * box.size.z);
                p.join.modifiedChunks = queuedNow;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            stats.terrainAckSeconds += seconds;
            stats.maxTerrainAckSeconds = std::max(stats.maxTerrainAckSeconds, seconds);
            if (step) {
                ++stats.terrainStepAcks;
                stats.terrainStepAckSeconds += seconds;
                stats.maxTerrainStepAckSeconds = std::max(stats.maxTerrainStepAckSeconds, seconds);
            }
            return true;
        }

        bool OnChunkRequest(Peer& p, std::span<const uint8_t> bytes) {
            ChunkRequest r;
            if (!Decode(bytes, r)) return false;
            const SyncBox server = RegionSyncBox(region);
            const bool repair = r.reason == ChunkRequestReason::Repair;
            for (const glm::ivec3& c : r.chunks) {
                ++stats.chunkRequests;
                stats.repairsRequested += repair ? 1u : 0u;
                ++p.join.requests;
                if (settings.countChunkRequests) ++requestCounts[c];
                if (!p.hasWindow || !p.window.Contains(c) || !server.Contains(c)) {
                    ++stats.requestsIgnored;
                    continue;
                }
                // Answers go first; one queued further back moves to the front.
                if (p.queued.contains(c)) {
                    const auto it = std::find(p.chunks.begin(), p.chunks.end(), c);
                    if (it == p.chunks.end()) continue;      // already in the front
                    if (static_cast<size_t>(p.chunks.end() - it) <= p.sortedTail) --p.sortedTail;
                    p.chunks.erase(it);
                }
                p.queued.insert(c);
                p.front.push_back({BulkItem::Kind::Chunk, c, 0, repair});
                if (p.front.size() > settings.maxQueuedAnswers) return false;
            }
            return true;
        }

        /// EntityListRequest (stage 7b): the list goes right after the peer's events of the next Flush,
        /// one per peer at a time (repeats merged).
        bool OnEntityListRequest(Peer& p, std::span<const uint8_t> bytes) {
            EntityListRequest r;
            if (!Decode(bytes, r)) return false;
            if (p.listOwed) ++stats.entityListsMerged;
            else ++stats.entityListRequests;
            p.listOwed = true;
            return true;
        }

        // --- the checks (stage 7b) ---------------------------------------------------------------

        /// The replicated objects' check values and list at the last opSeq (objects with a body and a
        /// model, as Entities(); lights are not counted). Every change of the set or of a model is an
        /// event, so they are computed once per opSeq.
        void EntityValues() {
            if (entityValuesValid && entityValuesAt == LastOpSeq()) return;
            entityValuesValid = true;
            entityValuesAt = LastOpSeq();
            entityValues = EntityCheck{};
            entityList.clear();
            const entt::registry& registry = simulation->Scene().Registry();
            std::unordered_map<uint32_t, ModelHash> seen;
            seen.reserve(modelHashes.size());
            for (const auto [entity, id, voxels, body] : registry.view<const scene::NetId, const scene::VoxelBody, const scene::RigidBody>().each()) {
                if (!voxels.model) continue;
                ModelHash cached;
                if (const auto it = modelHashes.find(id.value); it != modelHashes.end() && it->second.model.lock() == voxels.model) {
                    cached = it->second;
                } else {
                    cached.model = voxels.model;
                    cached.hash = VolumeHash(voxels.model->Volume());
                }
                entityList.push_back({id.value, cached.hash});
                seen.emplace(id.value, std::move(cached));
            }
            modelHashes = std::move(seen);      // gone objects are forgotten
            std::sort(entityList.begin(), entityList.end(), [](const EntityListEntry& a, const EntityListEntry& b) { return a.netId < b.netId; });
            for (const EntityListEntry& e : entityList) {
                ++entityValues.count;
                entityValues.idXor ^= e.netId;
                entityValues.modelXor ^= EntityModelTerm(e.netId, e.modelHash);
            }
        }

        /// A peer's HashCheck: the planner's chunks and the server's hashes of them after the last event.
        void SendHashCheck(PeerId id, Peer& p) {
            voxel::World& world = simulation->World();
            HashCheckInput in;
            in.box = Intersect(p.window, RegionSyncBox(region));
            in.modified = &modified;
            in.recent = recentChanged;
            in.dense = [&world](const glm::ivec3& c) {
                const voxel::CellState state = world.StateOf(c);
                return state == voxel::CellState::Dense || state == voxel::CellState::Packed;
            };
            in.skip = [&p](const glm::ivec3& c) { return p.queued.contains(c); };
            in.feet = p.player ? glm::dvec3(p.player->state.position) : glm::dvec3(tmpl.spawn);
            in.velocity = p.player ? glm::dvec3(p.player->state.velocity) : glm::dvec3(0.0);
            in.chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(world.VoxelSize());
            in.tick = steppedTick;
            HashCheck check;
            check.asOfOpSeq = LastOpSeq();
            for (const HashCheckPick& pick : p.planner.Plan(in)) {
                HashCheckEntry e;
                e.chunk = pick.chunk;
                if (!voxel::ChunkHashAt(world, pick.chunk, e.hash)) continue;    // never in the server's window
                e.pristine = !modified.contains(pick.chunk);
                check.entries.push_back(e);
                ++stats.hashCheckEntries[static_cast<size_t>(pick.kind)];
                if (check.entries.size() >= kMaxHashCheckEntries) break;
            }
            if (check.entries.empty()) return;
            Send(id, Channel::Events, Encode(check));
            ++stats.hashChecksSent;
        }

        /// The checks and the entity list of one peer, right after its events (Pump): a client processes
        /// them with every event up to LastOpSeq applied and none after (networking.md "Stage 7b plan").
        void SendChecks(PeerId id, Peer& p) {
            if (p.sentUpTo != LastOpSeq()) return;     // events held (a test's maxEventBytesInFlight): later
            const auto start = std::chrono::steady_clock::now();
            // At most one list per peer per half check period (review of stage 7b): a client that asks
            // every tick gets one now and the rest merged into the next.
            const uint32_t listGap = settings.entityCheckTicks / 2;
            const bool listDue = !p.listSent || steppedTick - p.lastListTick >= listGap;
            if (p.listOwed && !listDue && !p.listDelayed) {
                p.listDelayed = true;     // once per delayed list, not per tick it waits (recheck of stage 7b)
                ++stats.entityListsDelayed;
            }
            if (p.listOwed && listDue) {
                p.listOwed = false;
                p.listDelayed = false;
                p.listSent = true;
                p.lastListTick = steppedTick;
                EntityValues();
                EntityList list;
                list.asOfOpSeq = LastOpSeq();
                list.entries = entityList;
                const std::vector<uint8_t> bytes = Encode(list);
                Send(id, Channel::Events, bytes);
                ++stats.entityListsSent;
                stats.entityListBytes += bytes.size();
            }
            const uint32_t tick = steppedTick;
            const uint32_t stagger = 17u * p.slot;
            if (const uint32_t n = settings.hashCheckTicks; n > 0 && p.hasWindow && tick % n == stagger % n) SendHashCheck(id, p);
            if (const uint32_t n = settings.entityCheckTicks; n > 0 && p.hasWindow && !p.baselineOpen && tick % n == (stagger + n / 2) % n) {
                EntityValues();
                EntityCheck check = entityValues;
                check.asOfOpSeq = LastOpSeq();
                Send(id, Channel::Events, Encode(check));
                ++stats.entityChecksSent;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            stats.checkSeconds += seconds;
            stats.maxCheckSeconds = std::max(stats.maxCheckSeconds, seconds);
        }

        bool OnModelRequest(Peer& p, std::span<const uint8_t> bytes) {
            ModelRequest r;
            if (!Decode(bytes, r)) return false;
            for (const uint32_t netId : r.netIds) {
                ++stats.modelRequests;
                if (!lights.contains(netId) && EntityOf(netId) == scene::kNoEntity) {
                    ++stats.requestsIgnored;      // gone: its EntityDespawn is in the stream
                    continue;
                }
                // An answer still queued is encoded at its dequeue, after this request arrived: it
                // answers this one too (review of stage 7a).
                if (!p.modelsQueued.insert(netId).second) {
                    ++stats.modelRequestRepeats;
                    continue;
                }
                p.front.push_back({BulkItem::Kind::Entity, glm::ivec3(0), netId});
                if (p.front.size() > settings.maxQueuedAnswers) return false;
            }
            return true;
        }

        /// The next bulk message of a peer, encoded now (asOf = the last event made): request answers,
        /// chunks within bulkNearMetres of its player, the baseline, then the other chunks nearest first.
        /// Empty when nothing is left; `dropped` when the item had left the window or was gone.
        std::vector<uint8_t> NextBulk(Peer& p, bool& any) {
            any = true;
            BulkItem item;
            bool end = false;
            SortChunks(p, false);         // the sorted tail may have run out
            if (!p.front.empty()) {
                item = p.front.front();
                p.front.pop_front();
                if (item.kind == BulkItem::Kind::Entity) p.modelsQueued.erase(item.netId);
            } else if (!p.chunks.empty() && ChunkDistance(p, p.chunks.back()) <= static_cast<double>(settings.bulkNearMetres)) {
                item = {BulkItem::Kind::Chunk, p.chunks.back(), 0};
                PopChunk(p);
            } else if (p.baselineOpen && !p.nearSent && p.nearLeft == 0) {
                // The entities near the spawn are out: the joiner's start gate may open (the far
                // ones follow; their events are held meanwhile).
                p.nearSent = true;
                ++stats.baselineNear;
                return Encode(BaselineNear{LastOpSeq(), p.baselineSent});
            } else if (p.baselineOpen && !p.baseline.empty()) {
                item = {BulkItem::Kind::Entity, glm::ivec3(0), p.baseline.front()};
                p.baseline.pop_front();
                ++p.baselineSent;
                if (p.nearLeft > 0) --p.nearLeft;
            } else if (p.baselineOpen) {
                end = true;
            } else if (!p.chunks.empty()) {
                item = {BulkItem::Kind::Chunk, p.chunks.back(), 0};
                PopChunk(p);
            } else {
                any = false;
                return {};
            }
            if (end) {
                p.baselineOpen = false;
                return Encode(BaselineEnd{LastOpSeq(), p.baselineSent});
            }
            if (item.kind == BulkItem::Kind::Chunk) {
                p.queued.erase(item.chunk);
                ChunkData data;
                data.chunk = item.chunk;
                data.asOfOpSeq = LastOpSeq();
                if (!p.window.Contains(item.chunk) || !simulation->World().EncodeChunkAt(item.chunk, data.rle)) {
                    ++stats.chunksDropped;
                    if (p.terrainMode) TerrainChunkGone(p, item.chunk, 0);
                    return {};
                }
                std::vector<uint8_t> bytes = Encode(data);
                if (p.terrainMode) {
                    ++stats.terrainChunkData;
                    stats.terrainChunkBytes += bytes.size();
                    TerrainChunkGone(p, item.chunk, bytes.size());
                }
                ++stats.chunkData;
                stats.repairsSent += item.repair ? 1u : 0u;
                stats.chunkDataBytes += bytes.size();
                ++p.join.chunkData;
                p.join.chunkBytes += bytes.size();
                return bytes;
            }
            BaselineEntity e;
            if (!MakeBaselineEntity(item.netId, e)) {
                ++stats.baselineDropped;
                if (p.baselineOpen && p.baselineSent > 0) --p.baselineSent;
                return {};
            }
            std::vector<uint8_t> bytes = Encode(e);
            ++stats.baselineEntities;
            stats.baselineBytes += bytes.size();
            ++p.join.baseline;
            p.join.baselineBytes += bytes.size();
            return bytes;
        }

        /// Bulk of one peer, after its events: while its bytes in flight are under the pacer's window
        /// and within the optional hard cap. A message that does not fit what is left of the window
        /// waits for the peer's bulk in flight to drain, then goes alone while the bytes in flight
        /// (the live events) are under the window (review of stage 7a, 3); while the events alone fill
        /// the window nothing goes (recheck of stage 7a, 1: that is the pacer's back-off).
        void PumpBulk(PeerId id, Peer& p) {
            if (!p.pacer) return;
            const double dt = static_cast<double>(simulation->FixedTimeStep());
            size_t inFlight = transport.ReliableBytesInFlight(id);
            size_t bulkInFlight = transport.ChannelBytesInFlight(id, Channel::Bulk);
            // The live events handed over since the last tick count in what drained (the window is the
            // peer's whole reliable traffic; review of stage 7a).
            p.pacer->Sent(p.eventBytesSince);
            p.eventBytesSince = 0;
            p.pacer->Update(inFlight, transport.RoundTripSeconds(id), dt);
            const size_t window = p.pacer->Window();
            stats.minBulkWindow = stats.minBulkWindow == 0 ? window : std::min(stats.minBulkWindow, window);
            stats.maxBulkWindow = std::max(stats.maxBulkWindow, window);
            p.join.minWindow = p.join.minWindow == 0 ? window : std::min(p.join.minWindow, window);
            p.join.maxWindow = std::max(p.join.maxWindow, window);
            const double cap = settings.maxBulkBytesPerSecond;
            if (cap > 0.0) p.bulkCredit = std::min(p.bulkCredit + cap * dt, std::max(cap * dt, 4096.0));
            SortChunks(p, false);
            const auto encodeStart = std::chrono::steady_clock::now();
            bool any = true;
            while (any) {
                const bool pending = !p.heldBulk.empty() || !p.front.empty() || !p.chunks.empty() || p.baselineOpen;
                if (!pending) break;
                // Under the window, always (recheck of stage 7a, 1): a few events in flight are under it,
                // so they never keep the bulk from moving; events that fill it by themselves do.
                if (!p.pacer->MayHandOver(inFlight) || (cap > 0.0 && p.bulkCredit <= 0.0)) {
                    ++stats.bulkHeld;
                    break;
                }
                std::vector<uint8_t> bytes = std::move(p.heldBulk);
                p.heldBulk.clear();
                if (bytes.empty()) bytes = NextBulk(p, any);
                if (bytes.empty()) continue;
                // A message that does not fit the window waits until the bulk in flight has drained,
                // then goes alone (the bytes in flight are under the window here): the window is never
                // overshot by more than one message (plus the live events, which are never held).
                const bool fits = inFlight + bytes.size() <= window;
                if (!fits && bulkInFlight > 0) {
                    p.heldBulk = std::move(bytes);
                    ++stats.bulkHeld;
                    break;
                }
                stats.bulkAlone += fits ? 0u : 1u;
                stats.bulkOverWindow += inFlight >= window ? 1u : 0u;
                Send(id, Channel::Bulk, bytes);
                inFlight += bytes.size();
                bulkInFlight += bytes.size();
                p.pacer->Sent(bytes.size());
                if (cap > 0.0) p.bulkCredit -= static_cast<double>(bytes.size());
                stats.bulkBytes += bytes.size();
                stats.bulkOversize += bytes.size() > 16384 ? 1u : 0u;
                stats.maxBulkMessage = std::max(stats.maxBulkMessage, bytes.size());
                if (Tick() - p.join.secondStart >= 60) {
                    p.join.secondStart = Tick();
                    p.join.secondBytes = 0;
                }
                p.join.secondBytes += bytes.size();
                p.join.peakRate = std::max(p.join.peakRate, static_cast<double>(p.join.secondBytes));
            }
            const double encoded = std::chrono::duration<double>(std::chrono::steady_clock::now() - encodeStart).count();
            encodeTickSeconds += encoded;
            stats.encodeSeconds += encoded;
            p.join.encodeSeconds += encoded;
            const bool empty = p.heldBulk.empty() && p.front.empty() && p.chunks.empty() && !p.baselineOpen;
            // Stage 8: the join ends when its data is acknowledged, not when it was handed over (on a
            // slow link the last window of bulk is still in flight then).
            const size_t bulkNow = transport.ChannelBytesInFlight(id, Channel::Bulk);
            stats.maxBulkInFlight = std::max(stats.maxBulkInFlight, bulkNow);
            if (empty && p.hasWindow && !p.join.done && bulkNow == 0) p.join.done = true;
            if (empty && p.hasWindow && !p.join.logged) {
                p.join.logged = true;
                const double seconds = static_cast<double>(Tick() - p.join.joinTick) * dt;
                Log::Info("Join: client {} (slot {}): window {} chunks, {} modified; {} ChunkData / {:.1f} KiB, {} baseline entities / {:.1f} "
                          "KiB, {} requests; queue empty after {:.2f} s; bulk window {:.1f}..{:.1f} KiB, peak {:.1f} KiB/s; bulk work {:.1f} ms "
                          "(the server's busiest tick so far {:.2f} ms)",
                          id, p.slot, p.join.windowChunks, p.join.modifiedChunks, p.join.chunkData,
                          static_cast<double>(p.join.chunkBytes) / 1024.0, p.join.baseline, static_cast<double>(p.join.baselineBytes) / 1024.0,
                          p.join.requests, seconds, static_cast<double>(p.join.minWindow) / 1024.0, static_cast<double>(p.join.maxWindow) / 1024.0,
                          p.join.peakRate / 1024.0, p.join.encodeSeconds * 1000.0, stats.maxEncodeSecondsPerTick * 1000.0);
            }
        }

        void Receive() {
            TransportEvent e;
            std::vector<PeerId> broken;
            while (transport.Poll(e)) {
                switch (e.type) {
                    case TransportEvent::Type::Connected: {
                        Peer peer;
                        peer.connectTick = Tick();
                        peers[e.peer] = std::move(peer);
                        break;
                    }
                    case TransportEvent::Type::Disconnected:
                        if (const auto it = peers.find(e.peer); it != peers.end()) {
                            if (it->second.joined) {
                                ++stats.clientsDisconnected;
                                Log::Info("Server: client {} left (slot {})", e.peer, it->second.slot);
                                tickEvents |= kTickLeave;
                                if (onClientLeft) onClientLeft(it->second.slot);
                            }
                            DropPlayer(it->second);
                            peers.erase(it);
                        }
                        break;
                    case TransportEvent::Type::Received: {
                        const auto it = peers.find(e.peer);
                        if (it == peers.end() || it->second.closing) break;
                        if (!it->second.joined && e.channel == Channel::Events) {
                            if (PeekType(e.data) == MessageType::Hello) OnHello(e.peer, it->second, e.data);
                            else Refuse(e.peer, it->second, RefuseReason::Malformed);
                        } else if (it->second.joined && e.channel == Channel::State) {
                            // Unreliable: anything that is not a valid InputBatch is counted and ignored.
                            OnInputs(it->second, e.data);
                        } else if (it->second.joined && e.channel == Channel::Events) {
                            // Chunk sync (stage 7a). A broken reliable stream ends the peer's session.
                            const std::optional<MessageType> type = PeekType(e.data);
                            bool ok = false;
                            if (type == MessageType::WindowUpdate) ok = OnWindowUpdate(e.peer, it->second, e.data);
                            else if (type == MessageType::ChunkRequest) ok = OnChunkRequest(it->second, e.data);
                            else if (type == MessageType::ModelRequest) ok = OnModelRequest(it->second, e.data);
                            else if (type == MessageType::EntityListRequest) ok = OnEntityListRequest(it->second, e.data);
                            if (!ok) {
                                ++stats.malformedRequests;
                                Log::Warn("Server: client {} sent a malformed or out-of-order sync message: disconnected", e.peer);
                                broken.push_back(e.peer);
                            }
                        }
                        break;
                    }
                }
            }
            for (const PeerId id : broken) DropPeer(id);
            // A refused peer is disconnected once its refusal has left (ENet's disconnect_later), or
            // after a grace period; a peer that never says Hello, after the Hello timeout.
            std::vector<PeerId> close;
            const uint32_t now = Tick();
            for (const auto& [id, p] : peers) {
                const bool refusedGone = p.closing && transport.ReliableBytesInFlight(id) == 0;
                const bool refusedStuck = p.closing && now - p.closingTick >= settings.closeGraceTicks;
                const bool silent = !p.joined && !p.closing && now - p.connectTick >= settings.helloTimeoutTicks;
                if (!refusedGone && (refusedStuck || silent)) {
                    ++stats.peersTimedOut;
                    Log::Info("Server: disconnected peer {}: {}", id, silent ? "no Hello in time" : "its refusal was never taken");
                }
                if (refusedGone || refusedStuck || silent) close.push_back(id);
            }
            for (const PeerId id : close) {
                transport.Disconnect(id);
                DropPlayer(peers[id]);
                peers.erase(id);
            }
        }

        /// A failed session turns every new peer away.
        void ReceiveFailed() {
            TransportEvent e;
            while (transport.Poll(e))
                if (e.type == TransportEvent::Type::Connected) transport.Disconnect(e.peer);
        }

        void RunCast(const QueuedCast& q, bool overdue) {
            const uint32_t tick = Tick();
            const uint32_t firstOpSeq = LastOpSeq() + 1;
            // Wall time is logged only (shred.md R5): nothing below reads it to schedule.
            const auto start = std::chrono::steady_clock::now();
            const sim::BlastResult r = sim::Blast(*simulation, q.sphere, q.power);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            CastRecord rec;
            rec.player = q.player;
            rec.queuedTick = q.queuedTick;
            rec.tick = tick;
            rec.sphere = q.sphere;
            rec.power = q.power;
            rec.firstOpSeq = firstOpSeq;
            rec.overdue = overdue;
            rec.seconds = seconds;
            rec.searchErrors = r.search.islandBoxUnloaded + r.search.attachedByUnloaded + r.search.neckUnloaded;
            if (rec.searchErrors > 0) Fail("an island search met a chunk that is not loaded (the window must hold every flood box)");
            const int64_t creditBefore = eventCredit;
            // A cast the server could not run in full is not sent (the session has ended).
            for (const sim::AppliedOp& applied : r.ops) {
                if (failed || !CheckOp(applied.result)) break;
                EmitOp(applied.op, applied.result);
                ++rec.opCount;
            }
            // The objects it changed or made, and every despawn it caused (stage 6).
            EmitObjectEvents(r);
            rec.eventBytes = static_cast<uint32_t>(creditBefore - eventCredit);
            rec.work = sim::CastWorkOf(r);
            rec.islands = r.IslandCount();
            rec.crumbs = r.crumbs;
            rec.facePieces = r.search.facePieces;
            rec.neckPieces = r.search.neckPieces;
            rec.neckCrumbs = r.search.neckCrumbs;
            rec.inBoxIslands = rec.islands - std::min(rec.islands, rec.facePieces + rec.neckPieces);
            const uint64_t work = rec.work.Total();
            castCredit -= static_cast<int64_t>(work);
            ++stats.castsRun;
            stats.castsOverdue += overdue ? 1u : 0u;
            stats.castWork += work;
            stats.maxCastWork = std::max(stats.maxCastWork, work);
            stats.searchErrors += rec.searchErrors;
            stats.castSeconds += seconds;
            stats.maxCastSeconds = std::max(stats.maxCastSeconds, seconds);
            if (work >= 1000) {
                const double per10k = seconds * 1000.0 * 10000.0 / static_cast<double>(work);
                stats.maxCastMsPer10k = std::max(stats.maxCastMsPer10k, per10k);
                ++stats.castCostHistogram[std::min<size_t>(static_cast<size_t>(per10k * 10.0), stats.castCostHistogram.size() - 1)];
            }
            stats.islands += rec.islands;
            stats.crumbs += rec.crumbs;
            stats.inBoxIslands += rec.inBoxIslands;
            stats.facePieces += rec.facePieces;
            stats.neckPieces += rec.neckPieces;
            const uint32_t delay = tick - q.queuedTick;
            ++stats.castDelay[std::min<uint32_t>(delay, 7)];
            stats.maxCastDelay = std::max(stats.maxCastDelay, delay);
            if (overdue)
                Log::Warn("Server: a cast waited {} ticks and ran past the budgets (work credit {}, event credit {} bytes)", delay,
                          castCredit + static_cast<int64_t>(work), creditBefore);
            casts.push_back(std::move(rec));
            while (casts.size() > settings.castRecordLimit) casts.pop_front();
        }

        void RunCasts() {
            const int64_t perTick = static_cast<int64_t>(settings.castWorkPerTick);
            const int64_t maxWait = static_cast<int64_t>(settings.maxCastWaitTicks);
            // Credit is never banked beyond one tick; debt (a cast larger than a tick) carries over,
            // bounded so that a long overload cannot stall the queue for longer than the wait limit.
            castCredit = std::clamp(castCredit + perTick, -perTick * maxWait, perTick);
            eventCredit = std::clamp(eventCredit + static_cast<int64_t>(settings.eventBytesPerTick),
                                     -static_cast<int64_t>(settings.eventBurstBytes), static_cast<int64_t>(settings.eventBurstBytes));
            // The entity budget: debt (an event larger than the credit) carries over, bounded.
            const int64_t entityBurst = static_cast<int64_t>(settings.entityEventBurstBytes);
            entityCredit = std::clamp(entityCredit + static_cast<int64_t>(settings.entityEventBytesPerTick), -8 * entityBurst, entityBurst);
            bool waitedForWork = false, waitedForBytes = false;
            while (!queue.empty() && !failed) {
                QueuedCast q = queue.front();
                const bool overdue = static_cast<int64_t>(Tick() - q.queuedTick) >= maxWait;
                if (!overdue && castCredit <= 0) {
                    waitedForWork = true;
                    break;
                }
                if (!overdue && eventCredit <= 0) {
                    waitedForBytes = true;
                    break;
                }
                queue.pop_front();
                // A player's ray is cast now, when the cast runs, on the world as the casts before it
                // left it; one that casts nothing costs nothing and the next cast is looked at.
                if (q.ray && !ResolveRay(q)) continue;
                // No new server body while the entity budget is spent: its pieces are client rubble.
                simulation->Replication().refuseNewPieces = entityCredit <= 0;
                RunCast(q, overdue && (castCredit <= 0 || eventCredit <= 0));
                simulation->Replication().refuseNewPieces = false;
            }
            stats.ticksWaitedForWork += waitedForWork ? 1u : 0u;
            stats.ticksWaitedForBytes += waitedForBytes ? 1u : 0u;
        }

        /// Disconnects a joined peer at once (a broken sync stream, too slow).
        void DropPeer(PeerId id) {
            const auto it = peers.find(id);
            if (it == peers.end()) return;
            if (it->second.joined) {
                ++stats.clientsDisconnected;
                Log::Info("Server: client {} left (slot {})", id, it->second.slot);
                if (onClientLeft) onClientLeft(it->second.slot);
            }
            transport.Disconnect(id);
            DropPlayer(it->second);
            peers.erase(it);
        }

        void Flush() {
            encodeTickSeconds = 0.0;
            std::vector<PeerId> slow;
            for (auto& [id, p] : peers) {
                if (!p.joined) continue;
                if (transport.ReliableBytesInFlight(id) > settings.maxPeerBacklogBytes) {
                    slow.push_back(id);
                    continue;
                }
                Pump(id, p);
                if (!failed) SendChecks(id, p);
                PumpBulk(id, p);
            }
            for (const PeerId id : slow) {
                ++stats.peersTooSlow;
                Log::Warn("Server: client {} takes nothing: over {:.1f} MiB in flight to it, disconnected", id,
                          static_cast<double>(settings.maxPeerBacklogBytes) / 1048576.0);
                DropPeer(id);
            }
            stats.maxEncodeSecondsPerTick = std::max(stats.maxEncodeSecondsPerTick, encodeTickSeconds);
            TrimBacklog();
        }

        /// Events every joined peer has: gone (a joiner starts at the newest one).
        void TrimBacklog() {
            uint32_t keepAfter = lastOpSeq;
            for (const auto& [id, p] : peers)
                if (p.joined) keepAfter = std::min(keepAfter, p.sentUpTo);
            while (!backlog.empty() && backlogFirst <= keepAfter) {
                backlogBytes -= backlog.front().bytes.size();
                backlog.pop_front();
                ++backlogFirst;
            }
        }

        void CheckStore() {
            const voxel::WorldStats s = simulation->World().Stats();
            if (s.storeFullLoads > 0 || s.storeFullDecodes > 0) Fail("the chunk store is full");
        }
    };

    Server::Server(ServerSettings settings, ITransport& transport) : m_Impl(std::make_unique<Impl>(std::move(settings), transport)) {}
    Server::~Server() = default;

    bool Server::Start() {
        Impl& s = *m_Impl;
        if (s.started) return true;
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
        s.region.terrain.materials = ids;
        std::string why;
        if (!sim::IsValid(s.region, &why)) {
            Log::Error("Server: invalid region: {}", why);
            return false;
        }
        // The region's world first, then ServerConfig: its metre thresholds (the replicated piece
        // size) are taken in voxels of the session's size (review of stage 6: they were the default
        // size's, 262 voxels in a 0.1 m session instead of 512).
        sim::SimulationConfig config = s.settings.simulation;
        voxel::WorldConfig world = config.world;
        world.packHidden = s.settings.session.packHidden;
        config.world = sim::RegionWorldConfig(s.region, world);
        config = sim::ServerConfig(config);
        config.chunkCapacity = s.settings.session.chunkCapacity != 0 ? s.settings.session.chunkCapacity : sim::RegionChunkCapacity(s.region);
        s.simulation = std::make_unique<sim::Simulation>(config);
        const std::shared_ptr<voxel::IWorldGenerator> generator = sim::MakeRegionGenerator(s.region, ids);
        if (!s.simulation->Initialize(generator)) {
            Log::Error("Server: the simulation could not be started");
            return false;
        }
        s.buildHash = BuildHash(s.simulation->Materials());
        s.playArea = sim::PlayAreaOf(s.region, s.settings.player);
        sim::SetPlayAreaWalls(s.simulation->Physics(), s.playArea, s.settings.player);   // StepPlayer's walls (WP1)
        s.entityCredit = static_cast<int64_t>(s.settings.entityEventBurstBytes);
        // Every destruction of a replicated entity becomes an EntityDespawn (stage 6).
        s.simulation->Scene().Registry().on_destroy<scene::NetId>().connect<&Impl::OnNetIdDestroyed>(s);

        const auto start = std::chrono::steady_clock::now();
        voxel::World& w = s.simulation->World();
        w.LoadBlocking(sim::RegionWindowFocus(s.region));
        s.stats.preloadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const glm::ivec3 expect = sim::RegionWindowMinChunk(s.region);
        if (!sim::WindowIsRegions(w, s.region)) {
            Log::Error("Server: the window landed at ({}, {}, {}), not at the region's ({}, {}, {})", w.WindowMinChunk().x,
                       w.WindowMinChunk().y, w.WindowMinChunk().z, expect.x, sim::RegionRows(s.region).x, expect.z);
            return false;
        }
        s.started = true;

        // The template: operations 1..k, lights as EntitySpawn, objects as server bodies.
        s.tmpl = sim::BuildRegionTemplate(*s.simulation, s.region);
        if (!s.tmpl.sited) {
            Log::Error("Server: the template found no site for itself inside the play bounds (a playground needs level ground above "
                       "the sea within about 30 m of the origin)");
            return false;
        }
        for (const sim::WorldOp& op : s.tmpl.ops) ApplyOp(op);
        s.stats.templateOps = static_cast<uint32_t>(s.tmpl.ops.size());
        if (s.failed) return false;
        for (const sim::TemplateLight& light : s.tmpl.lights) {
            EntitySpawn spawn;
            spawn.netId = sim::AllocateNetId(*s.simulation);
            spawn.light = light;
            s.lights[spawn.netId] = light;
            s.Emit(spawn);
        }
        // The template's objects (the crates) are replicated server bodies (stage 6).
        for (const scene::Entity entity : sim::CreateTemplateObjects(*s.simulation, s.tmpl))
            if (entity != scene::kNoEntity) s.EmitObjectSpawn(sim::Replicate(*s.simulation, entity, false), entity);
        s.probes = ProbeChunks(s.region, *generator);
        // Stage 8: the spawn's colliders, built once now and kept (ColliderFocus): a join never cooks them.
        {
            const auto cookStart = std::chrono::steady_clock::now();
            const uint64_t builtBefore = s.simulation->Terrain().Stats().built;
            s.simulation->Terrain().Update(s.ColliderFocus(), s.simulation->Config().colliderRadius);
            s.simulation->Terrain().Flush();
            s.stats.spawnColliders = static_cast<uint32_t>(s.simulation->Terrain().Stats().built - builtBefore);
            s.stats.spawnColliderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - cookStart).count();
        }
        s.CheckStore();
        const voxel::WorldStats ws = w.Stats();
        Log::Info("Server: region of {} x {} chunks (+{} margin), {} rows at {} m, loaded in {:.2f} s: {} dense / {} packed chunks "
                  "({:.1f} MiB RLE), {} template operations, {} lights, {} objects",
                  s.region.playMaxChunk.x - s.region.playMinChunk.x, s.region.playMaxChunk.y - s.region.playMinChunk.y,
                  sim::kServerWindowMarginChunks, w.WindowSize().y, w.VoxelSize(), s.stats.preloadSeconds, ws.denseChunks, ws.packedChunks,
                  static_cast<double>(ws.packedBytes) / (1024.0 * 1024.0), s.tmpl.ops.size(), s.tmpl.lights.size(), s.tmpl.objects.size());
        return !s.failed;
    }

    void Server::Tick() {
        Impl& s = *m_Impl;
        if (!s.started) return;
        if (s.failed) return s.ReceiveFailed();
        const auto start = std::chrono::steady_clock::now();
        s.tickEvents = 0;
        s.tickFlushCooks = 0;
        s.Receive();
        const std::vector<glm::dvec3> focus = s.ColliderFocus();
        s.simulation->UpdateStreaming(focus);
        s.StepPlayers();
        s.RunCasts();
        const uint32_t tick = s.Tick();
        s.simulation->StepPhysics();
        s.steppedTick = tick;
        s.simulation->UpdateLifetimes(s.simulation->FixedTimeStep());
        // What fell out of the world is despawned; what came to rest is announced (stage 6).
        s.DrainDespawns();
        if (!s.failed) s.TrackBodies(tick);
        s.Flush();
        if (tick % 2 == 0 && !s.failed) s.SendSnapshots(tick);
        // World::Stats walks every cell (about 0.3 ms for a 250 m region): twice a second is enough,
        // an edit that found no slot is caught at once by CheckOp.
        if (tick % 30 == 0) s.CheckStore();
        s.RefreshHashes(32);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        s.stats.tickSeconds += seconds;
        s.stats.maxTickSeconds = std::max(s.stats.maxTickSeconds, seconds);
        ++s.stats.ticksTimed;
        s.stats.ticksOverBudget += seconds > 1.0 / 60.0 ? 1u : 0u;
        ++s.stats.tickHistogram[std::min<size_t>(static_cast<size_t>(seconds / 0.00025), s.stats.tickHistogram.size() - 1)];
        // Stage 8: join and leave ticks apart (the owner's 126 ms join and 207 ms leave passes).
        s.stats.flushBoxCooks += s.tickFlushCooks;
        if (s.tickEvents != 0) {
            const uint32_t t = s.Tick() - 1;
            if (s.tickEvents & kTickHello) {
                ++s.stats.helloTicks;
                s.stats.maxHelloTickSeconds = std::max(s.stats.maxHelloTickSeconds, seconds);
                s.stats.maxHelloTickCooks = std::max(s.stats.maxHelloTickCooks, s.tickFlushCooks);
            }
            if (s.tickEvents & kTickFirstWindow) {
                ++s.stats.firstWindowTicks;
                s.stats.maxFirstWindowTickSeconds = std::max(s.stats.maxFirstWindowTickSeconds, seconds);
            }
            if (s.tickEvents & kTickLeave) {
                ++s.stats.leaveTicks;
                s.stats.maxLeaveTickSeconds = std::max(s.stats.maxLeaveTickSeconds, seconds);
            }
            Log::Info("Ticks: tick {} ({}{}{}) took {:.2f} ms, {} collider(s) cooked on this thread", t,
                      (s.tickEvents & kTickHello) ? "a join" : "", (s.tickEvents & kTickFirstWindow) ? ((s.tickEvents & kTickHello) ? ", a first window" : "a first window") : "",
                      (s.tickEvents & kTickLeave) ? ((s.tickEvents & (kTickHello | kTickFirstWindow)) ? ", a leave" : "a leave") : "", seconds * 1000.0,
                      s.tickFlushCooks);
        }
    }

    uint32_t Server::Advance(double seconds) {
        Impl& s = *m_Impl;
        if (!s.started) return 0;
        const double dt = static_cast<double>(s.simulation->FixedTimeStep());
        s.accumulator += seconds;
        uint32_t ran = 0;
        while (s.accumulator >= dt && ran < 2) {
            Tick();
            s.accumulator -= dt;
            ++ran;
        }
        return ran;
    }

    uint32_t Server::ApplyOp(const sim::WorldOp& op) {
        Impl& s = *m_Impl;
        if (!s.started || s.failed) return 0;
        const sim::OpResult r = sim::ApplyOp(s.simulation->World(), op);
        if (!s.CheckOp(r)) return 0;
        return s.EmitOp(op, r);
    }

    bool Server::QueueCast(const voxel::SphereQ& sphere, uint16_t power) {
        Impl& s = *m_Impl;
        if (!s.started || s.failed) return false;
        return s.Queue(sphere, power, 0);
    }

    bool Server::QueueCast(const sim::BlastDesc& desc) {
        Impl& s = *m_Impl;
        if (!s.started) return false;
        const double vs = static_cast<double>(s.simulation->World().VoxelSize());
        return QueueCast(voxel::QuantizeSphere(desc.center / vs, static_cast<double>(desc.radius) / vs), voxel::QuantizePower(desc.power));
    }

    size_t Server::QueuedCasts() const { return m_Impl->queue.size(); }

    uint32_t Server::SpawnCrate(const glm::dvec3& origin) {
        Impl& s = *m_Impl;
        if (!s.started || s.failed) return 0;
        return s.SpawnCrateAt(origin, nullptr);
    }

    uint32_t Server::SpawnObject(std::shared_ptr<voxel::VoxelModel> model, const glm::dvec3& origin) {
        Impl& s = *m_Impl;
        if (!s.started || s.failed || !model || model->SolidCount() == 0) return 0;
        return s.SpawnObjectAt(model, origin, nullptr);
    }

    uint32_t Server::ClearBodies(const glm::dvec3& min, const glm::dvec3& max) {
        Impl& s = *m_Impl;
        if (!s.started || s.failed) return 0;
        std::vector<scene::Entity> found;
        s.simulation->Scene().QueryBodies(math::AABB(glm::vec3(min), glm::vec3(max)), found);
        uint32_t n = 0;
        for (const scene::Entity e : found) {
            if (!s.simulation->Scene().TryGet<scene::NetId>(e)) continue;
            sim::Despawn(*s.simulation, e, scene::DespawnReason::Destroyed);
            ++n;
        }
        s.DrainDespawns();
        return n;
    }

    std::vector<EntityInfo> Server::Entities() const {
        const Impl& s = *m_Impl;
        std::vector<EntityInfo> out;
        if (!s.simulation) return out;
        const physics::PhysicsWorld& physics = const_cast<sim::Simulation&>(*s.simulation).Physics();
        const entt::registry& registry = s.simulation->Scene().Registry();
        for (const auto [entity, id, voxels, body] : registry.view<const scene::NetId, const scene::VoxelBody, const scene::RigidBody>().each()) {
            if (!voxels.model) continue;
            const physics::BodyState state = physics.GetState(body.body);
            EntityInfo e;
            e.netId = id.value;
            e.modelHash = VolumeHash(voxels.model->Volume());
            e.position = state.position;
            e.rotation = state.rotation;
            const auto it = s.tracks.find(id.value);
            e.resting = it != s.tracks.end() && it->second.resting;
            out.push_back(e);
        }
        std::sort(out.begin(), out.end(), [](const EntityInfo& a, const EntityInfo& b) { return a.netId < b.netId; });
        return out;
    }

    size_t Server::AwakeBodies() const {
        size_t n = 0;
        for (const auto& [id, t] : m_Impl->tracks) n += t.resting ? 0u : 1u;
        return n;
    }

    size_t Server::PlayerCount() const {
        size_t n = 0;
        for (const auto& [id, p] : m_Impl->peers) n += p.player ? 1u : 0u;
        return n;
    }

    std::optional<sim::PlayerState> Server::GetPlayer(uint8_t slot) const {
        for (const auto& [id, p] : m_Impl->peers)
            if (p.player && p.slot == slot) return p.player->state;
        return std::nullopt;
    }

    bool Server::TeleportPlayer(uint8_t slot, const glm::vec3& feet) {
        Impl& s = *m_Impl;
        if (!std::isfinite(feet.x) || !std::isfinite(feet.y) || !std::isfinite(feet.z)) return false;
        for (auto& [id, p] : s.peers)
            if (p.player && p.slot == slot) {
                p.player->state.position = feet;
                p.player->state.velocity = glm::vec3(0.0f);
                p.player->state.flags = 0;
                sim::RestorePlayer(s.simulation->Physics(), p.player->character, p.player->state);
                return true;
            }
        return false;
    }
    uint32_t Server::TickNumber() const { return m_Impl->Tick(); }
    uint32_t Server::LastOpSeq() const { return m_Impl->LastOpSeq(); }
    size_t Server::ClientCount() const { return m_Impl->JoinedCount(); }
    void Server::SetOnClientLeft(std::function<void(uint8_t)> callback) { m_Impl->onClientLeft = std::move(callback); }
    bool Server::Failed() const { return m_Impl->failed; }
    sim::Simulation& Server::Simulation() { return *m_Impl->simulation; }
    const sim::RegionDesc& Server::Region() const { return m_Impl->region; }
    const sim::RegionTemplate& Server::Template() const { return m_Impl->tmpl; }
    const std::vector<ProbeHash>& Server::Probes() const { return m_Impl->probes; }
    uint64_t Server::BuildHashValue() const { return m_Impl->buildHash; }
    voxel::WorldHashResult Server::Hash() const {
        const SyncBox box = RegionSyncBox(m_Impl->region);
        return m_Impl->CachedHash(box.min, box.Max());
    }
    uint64_t Server::ChunkHashesComputed() const { return m_Impl->hashesComputed; }
    size_t Server::JoiningClients() const {
        size_t n = 0;
        for (const auto& [id, p] : m_Impl->peers) n += p.joined && !p.join.done ? 1u : 0u;
        return n;
    }
    const ServerStats& Server::Stats() const { return m_Impl->stats; }
    const std::deque<CastRecord>& Server::Casts() const { return m_Impl->casts; }
    size_t Server::BacklogBytes() const { return m_Impl->backlogBytes; }
    voxel::WorldHashResult Server::Hash(const SyncBox& box) const { return m_Impl->CachedHash(box.min, box.Max()); }
    size_t Server::ModifiedChunks() const { return m_Impl->modified.size(); }
    bool Server::IsModified(const glm::ivec3& chunk) const { return m_Impl->modified.contains(chunk); }
    std::vector<std::pair<uint32_t, glm::ivec3>> Server::RecentChanged() const { return m_Impl->recentChanged; }
    std::vector<std::pair<glm::ivec3, uint32_t>> Server::ChunkRequestCounts() const {
        return {m_Impl->requestCounts.begin(), m_Impl->requestCounts.end()};
    }
    void Server::SetMaxBulkBytesPerSecond(double bytesPerSecond) { m_Impl->settings.maxBulkBytesPerSecond = std::max(0.0, bytesPerSecond); }

    TerrainPeerCost Server::TerrainCost(uint8_t slot) const {
        const Impl& s = *m_Impl;
        TerrainPeerCost out;
        for (const auto& [id, p] : s.peers) {
            if (!p.joined || p.slot != slot || !p.terrainMode) continue;
            out.terrainMode = true;
            out.firstSeq = p.terrainFirstSeq;
            out.joinAckBytes = p.terrainJoinAckBytes;
            out.joinDataBytes = p.terrainJoinDataBytes;
            out.joinChunks = p.terrainJoinChunks;
            out.joinUniform = p.terrainJoinUniform;
            out.steps = p.terrainStepCount;
            out.stepBytes = p.terrainStepBytes;
            out.maxStepBytes = p.terrainMaxStepBytes;
            for (const auto& [seq, step] : p.terrainSteps) {      // updates still sending
                if (seq == p.terrainFirstSeq) continue;
                ++out.steps;
                out.stepBytes += step.bytes;
                out.maxStepBytes = std::max(out.maxStepBytes, step.bytes);
            }
            out.queuedChunks = static_cast<uint32_t>(p.queued.size());
            for (const glm::ivec3& c : p.chunks) out.queuedOutsideWindow += p.window.Contains(c) ? 0u : 1u;
            out.maxQueuedChunks = p.maxQueued;
            out.ackSteps = p.ackSteps;
            out.ackStepBytes = p.ackStepBytes;
            out.maxAckStepBytes = p.maxAckStepBytes;
        }
        return out;
    }

    bool Server::LastHashChecked(uint8_t slot, const glm::ivec3& chunk, uint32_t& tick) const {
        for (const auto& [id, p] : m_Impl->peers)
            if (p.joined && p.slot == slot) return p.planner.LastChecked(chunk, tick);
        return false;
    }

    bool Server::ClientJoinPoint(uint8_t slot, uint32_t& opSeq, uint32_t& tick) const {
        for (const auto& [id, p] : m_Impl->peers)
            if (p.joined && p.slot == slot) {
                opSeq = p.join.joinOpSeq;
                tick = p.join.joinTick;
                return true;
            }
        return false;
    }

    SyncBox Server::ClientSyncBox(uint8_t slot) const {
        const Impl& s = *m_Impl;
        for (const auto& [id, p] : s.peers)
            if (p.joined && p.slot == slot) return p.hasWindow ? Intersect(p.window, RegionSyncBox(s.region)) : SyncBox{};
        return {};
    }
}
