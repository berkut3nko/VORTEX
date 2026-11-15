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
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
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
        };

        /// A replicated object's rest state as the clients were told it (EntityRest).
        struct BodyTrack {
            bool resting = false;
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
        bool started = false;
        bool failed = false;

        std::vector<LoggedEvent> log;     // log[i] has opSeq i + 1
        std::map<PeerId, Peer> peers;
        std::deque<QueuedCast> queue;
        int64_t castCredit = 0;
        int64_t eventCredit = 0;
        double accumulator = 0.0;
        ServerStats stats;
        std::deque<CastRecord> casts;
        size_t logBytes = 0;
        // Replicated objects (stage 6).
        std::vector<EntityDespawn> despawned;              // the on_destroy<NetId> observer's queue
        std::unordered_map<uint32_t, BodyTrack> tracks;   // NetId -> rest state the clients know
        std::shared_ptr<voxel::VoxelModel> crateModel;

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
            tracks[netId].resting = (spawn.object.flags & kObjectResting) != 0;
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
        /// announced with its pose (EntityRest); one that wakes shows up in snapshots again.
        ///
        /// The layer follows the same rule as a client's copy (networking.md "Stage 6 as built"):
        /// Layer::Rubble while it moves, Layer::Dynamic at rest, so that players on both sides collide
        /// with exactly the bodies at rest (a moving piece passes through a player everywhere; a
        /// client learns of a rest one event later, and only that window can cost a correction).
        void TrackBodies(uint32_t tick) {
            physics::PhysicsWorld& physics = simulation->Physics();
            uint32_t awake = 0;
            for (const auto [entity, id, body] : simulation->Scene().Registry().view<const scene::NetId, const scene::RigidBody>().each()) {
                const bool moving = sim::IsAwake(physics, body.body);
                BodyTrack& t = tracks[id.value];
                awake += moving ? 1u : 0u;
                if (moving) {
                    t.resting = false;
                    if (body.motion != physics::MotionType::Static) physics.SetLayer(body.body, physics::Layer::Rubble);
                } else if (!t.resting) {
                    t.resting = true;
                    if (body.motion != physics::MotionType::Static) physics.SetLayer(body.body, physics::Layer::Dynamic);
                    const physics::BodyState state = physics.GetState(body.body);
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
            voxel::World& world = simulation->World();
            if (!crateModel) crateModel = sim::CrateModel(simulation->WorldMaterialIds(), sim::PlaygroundScale(world.VoxelSize()));
            const double vs = static_cast<double>(world.VoxelSize());
            const glm::dvec3 size = glm::dvec3(crateModel->Volume().Size()) * vs;
            const double chunk = static_cast<double>(voxel::kChunkSize) * vs;
            const glm::dvec2 lo = glm::dvec2(region.playMinChunk) * chunk, hi = glm::dvec2(region.playMaxChunk) * chunk;
            const glm::ivec2 rows = sim::RegionRows(region);
            const bool inside = std::isfinite(corner.x) && std::isfinite(corner.y) && std::isfinite(corner.z) && corner.x >= lo.x &&
                                corner.z >= lo.y && corner.x + size.x <= hi.x && corner.z + size.z <= hi.y &&
                                corner.y >= static_cast<double>(rows.x) * chunk && corner.y + size.y <= static_cast<double>(rows.y) * chunk;
            const scene::Entity entity =
                inside ? simulation->Scene().CreateVoxelObject({.model = crateModel, .position = glm::vec3(corner)}) : scene::kNoEntity;
            if (entity == scene::kNoEntity) {
                ++stats.cratesRefused;
                return 0;
            }
            const uint32_t netId = sim::Replicate(*simulation, entity, false);
            // Moving: no player collides with it until it rests (TrackBodies), here as on every client.
            if (const scene::RigidBody* body = simulation->Scene().TryGet<scene::RigidBody>(entity))
                simulation->Physics().SetLayer(body->body, physics::Layer::Rubble);
            EmitObjectSpawn(netId, entity);
            ++stats.crates;
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
        uint32_t LastOpSeq() const { return static_cast<uint32_t>(log.size()); }

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
                Log::Warn("Server: event {} is {} bytes, more than the {}-byte event burst (sent anyway)", log.size() + 1, bytes,
                          settings.eventBurstBytes);
            }
            log.push_back({Tick(), MaskVoxels(event), w.Take()});
            if (logBytes <= settings.joinLogWarnBytes && logBytes + bytes > settings.joinLogWarnBytes)
                Log::Warn("Server: the event log passed {:.1f} MiB: every joiner receives all of it (until networking stage 7); joins are "
                          "refused above {:.1f} MiB",
                          static_cast<double>(settings.joinLogWarnBytes) / 1048576.0, static_cast<double>(settings.maxJoinLogBytes) / 1048576.0);
            logBytes += bytes;
            ++stats.events;
            stats.eventBytes += bytes;
            if (entity) stats.entityEventBytes += bytes;
            else eventCredit -= static_cast<int64_t>(bytes);
            return LastOpSeq();
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
            const voxel::WorldRayHit hit = simulation->World().Raycast(q.origin, q.direction, t.castRange);
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
                simulation->Terrain().FlushBox(sim::PlayerReach(pl.state.position, settings.player));
                const uint8_t levels = input.buttons;
                const bool lateJump = pl.lateJump;
                if (lateJump) {
                    // The edge of a late input's jump is made now; the levels stay this input's.
                    pl.state.lastButtons = static_cast<uint8_t>(pl.state.lastButtons & ~sim::PlayerButton_Jump);
                    input.buttons = static_cast<uint8_t>(input.buttons | sim::PlayerButton_Jump);
                    pl.lateJump = false;
                }
                const sim::PlayerStepResult r = sim::StepPlayer(simulation->Physics(), simulation->Terrain(), pl.character, pl.state, input,
                                                                settings.player, dt);
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
        }

        /// Sends the batch that starts after `from` (events of one tick, at most maxBatchBytes and
        /// kMaxMaskVoxels of masks; one larger event goes alone) and returns its last opSeq.
        uint32_t SendBatch(PeerId peer, uint32_t from, uint32_t to) {
            const uint32_t seq = from + 1;
            const uint32_t tick = log[seq - 1].tick;
            std::vector<uint8_t> bytes;
            uint64_t maskVoxels = 0;
            uint32_t count = 0;
            uint32_t s = seq;
            while (s <= to && log[s - 1].tick == tick && count < kMaxBatchEvents) {
                const LoggedEvent& e = log[s - 1];
                if (count > 0 && (bytes.size() + e.bytes.size() > settings.maxBatchBytes || maskVoxels + e.maskVoxels > kMaxMaskVoxels)) break;
                bytes.insert(bytes.end(), e.bytes.begin(), e.bytes.end());
                maskVoxels += e.maskVoxels;
                ++count;
                ++s;
            }
            Send(peer, Channel::Events, EncodeBatch(seq, tick, count, bytes));
            return s - 1;
        }

        /// Hands a peer its next batches while less than maxEventBytesInFlight is on its way to it
        /// (all of them when unpaced): a joiner's catch-up waits here, in the log.
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
            // Until stage 7 a joiner receives the whole log: past the limit it would load for minutes.
            if (logBytes > settings.maxJoinLogBytes) {
                Log::Warn("Server: a join refused: the event log is {:.1f} MiB, over the {:.1f} MiB a joiner may be sent (networking stage 7)",
                          static_cast<double>(logBytes) / 1048576.0, static_cast<double>(settings.maxJoinLogBytes) / 1048576.0);
                return Refuse(peer, p, RefuseReason::LogTooLarge);
            }
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
            welcome.logEnd = LastOpSeq();
            welcome.probes = probes;
            Send(peer, Channel::Events, Encode(welcome));
            // Join = the whole event log (until stage 7's chunk sync), on the same ordered channel,
            // paced by Flush like the live stream behind it.
            p.joined = true;
            p.slot = slot;
            p.sentUpTo = 0;
            // Its player stands at the spawn point; the ground under every player gets its collider
            // before the next step (a gate that held the player here would hold it for another
            // number of ticks than its client's).
            p.player = std::make_unique<Player>();
            p.player->joinTick = Tick();          // Receive runs at the start of the tick that steps it first
            p.player->state.position = glm::vec3(tmpl.spawn);
            p.player->character = simulation->Physics().CreateCharacter(sim::PlayerCharacterDesc(p.player->state.position, settings.player));
            const std::vector<glm::dvec3> focus = PlayerFocus();
            simulation->Terrain().Update(focus, simulation->Config().colliderRadius);
            simulation->Terrain().Flush();
            ++stats.clientsJoined;
            Log::Info("Server: client {} joined at tick {} (slot {}, {} events / {} bytes of log)", peer, Tick(), slot, LastOpSeq(), logBytes);
        }

        void Receive() {
            TransportEvent e;
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
                        }
                        break;
                    }
                }
            }
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
                Emit(applied.op);
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
            if (work >= 1000) stats.maxCastMsPer10k = std::max(stats.maxCastMsPer10k, seconds * 1000.0 * 10000.0 / static_cast<double>(work));
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
                RunCast(q, overdue && (castCredit <= 0 || eventCredit <= 0));
            }
            stats.ticksWaitedForWork += waitedForWork ? 1u : 0u;
            stats.ticksWaitedForBytes += waitedForBytes ? 1u : 0u;
        }

        void Flush() {
            for (auto& [id, p] : peers)
                if (p.joined) Pump(id, p);
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
        sim::SimulationConfig config = sim::ServerConfig(s.settings.simulation);
        voxel::WorldConfig world = config.world;
        world.packHidden = s.settings.session.packHidden;
        config.world = sim::RegionWorldConfig(s.region, world);
        config.chunkCapacity = s.settings.session.chunkCapacity != 0 ? s.settings.session.chunkCapacity : sim::RegionChunkCapacity(s.region);
        s.simulation = std::make_unique<sim::Simulation>(config);
        const std::shared_ptr<voxel::IWorldGenerator> generator = sim::MakeRegionGenerator(s.region, ids);
        if (!s.simulation->Initialize(generator)) {
            Log::Error("Server: the simulation could not be started");
            return false;
        }
        s.buildHash = BuildHash(s.simulation->Materials());
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
            s.Emit(spawn);
        }
        // The template's objects (the crates) are replicated server bodies (stage 6).
        for (const scene::Entity entity : sim::CreateTemplateObjects(*s.simulation, s.tmpl))
            if (entity != scene::kNoEntity) s.EmitObjectSpawn(sim::Replicate(*s.simulation, entity, false), entity);
        s.probes = ProbeChunks(s.region, *generator);
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
        s.Receive();
        const std::vector<glm::dvec3> focus = s.PlayerFocus();
        s.simulation->UpdateStreaming(focus);
        s.StepPlayers();
        s.RunCasts();
        const uint32_t tick = s.Tick();
        s.simulation->StepPhysics();
        s.simulation->UpdateLifetimes(s.simulation->FixedTimeStep());
        // What fell out of the world is despawned; what came to rest is announced (stage 6).
        s.DrainDespawns();
        if (!s.failed) s.TrackBodies(tick);
        s.Flush();
        if (tick % 2 == 0 && !s.failed) s.SendSnapshots(tick);
        // World::Stats walks every cell (about 0.3 ms for a 250 m region): twice a second is enough,
        // an edit that found no slot is caught at once by CheckOp.
        if (tick % 30 == 0) s.CheckStore();
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
        if (!s.CheckOp(sim::ApplyOp(s.simulation->World(), op))) return 0;
        return s.Emit(op);
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
    bool Server::Failed() const { return m_Impl->failed; }
    sim::Simulation& Server::Simulation() { return *m_Impl->simulation; }
    const sim::RegionDesc& Server::Region() const { return m_Impl->region; }
    const sim::RegionTemplate& Server::Template() const { return m_Impl->tmpl; }
    const std::vector<ProbeHash>& Server::Probes() const { return m_Impl->probes; }
    uint64_t Server::BuildHashValue() const { return m_Impl->buildHash; }
    voxel::WorldHashResult Server::Hash() const { return RegionHash(m_Impl->simulation->World(), m_Impl->region); }
    const ServerStats& Server::Stats() const { return m_Impl->stats; }
    const std::deque<CastRecord>& Server::Casts() const { return m_Impl->casts; }
    size_t Server::LogBytes() const { return m_Impl->logBytes; }
}
