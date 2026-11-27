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
import :server_impl;

namespace vortex::net {
    using namespace detail;

    Server::Impl::Impl(ServerSettings s, ITransport& t) : settings(std::move(s)), transport(t), region(settings.session.region) {}

    Server::Impl::~Impl() {
        // The scene is cleared when the simulation goes: no despawn may reach members already gone.
        if (simulation) simulation->Scene().Registry().on_destroy<scene::NetId>().disconnect<&Impl::OnNetIdDestroyed>(*this);
    }

    void Server::Impl::RefreshHashes(size_t budget) {
        for (auto it = hashStale.begin(); it != hashStale.end() && budget > 0; --budget) {
            (void)CachedHash(*it, *it + glm::ivec3(1));
            it = hashStale.erase(it);
        }
    }

    voxel::WorldHashResult Server::Impl::CachedHash(const glm::ivec3& min, const glm::ivec3& max) const {
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

    std::vector<glm::dvec3> Server::Impl::ColliderFocus() const {
        std::vector<glm::dvec3> focus = PlayerFocus();
        focus.push_back(glm::dvec3(tmpl.spawn));
        // [wp-7] The team spawns too: a join or a respawn finds their ground built.
        if (settings.session.fight.Active())
            for (const glm::vec2& at : settings.session.fight.teamSpawns)
                focus.push_back(glm::dvec3(at.x, static_cast<double>(tmpl.spawn.y), at.y));
        return focus;
    }

    uint32_t Server::Impl::Emit(const Event& event) {
        ByteWriter w;
        WriteEvent(w, event);
        const size_t bytes = w.Size();
        // Zone events and CastFx follow from a spell's integers, as its operations do: the cast queue's credit pays them.
        const bool entity = !std::holds_alternative<sim::WorldOp>(event) && !std::holds_alternative<ZoneStartEvent>(event) &&
                            !std::holds_alternative<ZoneCancelEvent>(event) && !std::holds_alternative<CastFx>(event);
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

    uint32_t Server::Impl::EmitOp(const sim::WorldOp& op, const sim::OpResult& result) {
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

    bool Server::Impl::CheckOp(const sim::OpResult& r) {
        if (r.Complete()) return true;
        ++stats.incompleteOps;
        Fail("an operation met chunks that are not loaded or have no store slot in the server's own window");
        return false;
    }

    void Server::Impl::Fail(const char* why) {
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

    void Server::Impl::Send(PeerId peer, Channel channel, const std::vector<uint8_t>& bytes) {
        transport.Send(peer, channel, bytes);
        stats.sentBytes += bytes.size();
        if (channel == Channel::Events)
            if (const auto it = peers.find(peer); it != peers.end()) it->second.eventBytesSince += bytes.size();
    }

    uint32_t Server::Impl::SendBatch(PeerId peer, uint32_t from, uint32_t to) {
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

    void Server::Impl::Pump(PeerId id, Peer& p) {
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

    void Server::Impl::Receive() {
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
                        else if (type == MessageType::Attune) ok = OnAttune(it->second, e.data);
                        else if (type == MessageType::SpellGesture) ok = OnSpellGesture(it->second, e.data);
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

    void Server::Impl::ReceiveFailed() {
        TransportEvent e;
        while (transport.Poll(e))
            if (e.type == TransportEvent::Type::Connected) transport.Disconnect(e.peer);
    }

    void Server::Impl::DropPeer(PeerId id) {
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

    void Server::Impl::Flush() {
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

    void Server::Impl::TrimBacklog() {
        uint32_t keepAfter = lastOpSeq;
        for (const auto& [id, p] : peers)
            if (p.joined) keepAfter = std::min(keepAfter, p.sentUpTo);
        while (!backlog.empty() && backlogFirst <= keepAfter) {
            backlogBytes -= backlog.front().bytes.size();
            backlog.pop_front();
            ++backlogFirst;
        }
    }

    void Server::Impl::CheckStore() {
        const voxel::WorldStats s = simulation->World().Stats();
        if (s.storeFullLoads > 0 || s.storeFullDecodes > 0) Fail("the chunk store is full");
    }

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
        s.aimRules = sim::SessionAimRules(s.settings.player, s.region);
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
        s.StartMagic();

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
        // [wp-7] Felling: each team's tower, stream operations after the template's (team 1's, then team 2's).
        s.BuildFellingTowers();
        if (s.failed) return false;
        if (s.settings.session.fight.Active())
            Log::Info("Fight: mode {}, score limit {}, round {} s, respawn {} ticks, {} charges every {} ticks, {} players to start",
                      sim::ToString(s.settings.session.fight.mode), s.settings.session.fight.scoreLimit, s.settings.session.fight.roundTicks / 60,
                      s.settings.session.fight.respawnTicks, s.settings.session.fight.maxCharges, s.settings.session.fight.chargeTicks,
                      s.settings.session.fight.minPlayers);
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
        // [mech-1] The mechanism yard (session key `mechanisms`).
        s.BuildYard();
        if (s.failed) return false;
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
        // [wp-7] A session whose round ended with end_session is over: nobody joins it again, nothing runs.
        if (s.sessionOver && s.settings.session.fight.endSession) return s.ReceiveFailed();
        const auto start = std::chrono::steady_clock::now();
        s.tickEvents = 0;
        s.tickFlushCooks = 0;
        s.Receive();
        const std::vector<glm::dvec3> focus = s.ColliderFocus();
        s.simulation->UpdateStreaming(focus);
        // Fight order per tick (fight.md "Rules"; ServerImpl.cppm): StepPlayers, the casts, StepFight, snapshots.
        s.StepPlayers();
        s.RunCasts();
        s.RunScheduledSpells();
        s.StepFight();
        const uint32_t tick = s.Tick();
        // [mech-1] Held bodies follow their holders, then the mechanisms' motion of this physics step (crushed bodies
        // are despawned by DrainDespawns below).
        s.StepHolds();
        if (!s.mechanisms.empty()) {
            const sim::MechanismStepResult moved = sim::StepMechanisms(*s.simulation, tick, true);
            s.stats.mechanismCrushed += moved.crushed;
            s.EmitMechanismChanges(moved.changes);
        }
        s.simulation->StepPhysics();
        s.steppedTick = tick;
        s.simulation->UpdateLifetimes(s.simulation->FixedTimeStep());
        // What fell out of the world is despawned; what came to rest is announced (stage 6).
        s.DrainDespawns();
        if (!s.failed) s.TrackBodies(tick);
        s.Flush();
        if (tick % 2 == 0 && !s.failed) s.SendSnapshots(tick);
        if (s.endNow) {
            // [wp-7] One round per session (fight.md "Decision"): the last RoundUpdate went with this Flush; every
            // peer is disconnected and nobody joins this session again.
            s.endNow = false;
            std::vector<PeerId> ids;
            for (const auto& [id, p] : s.peers) ids.push_back(id);
            for (const PeerId id : ids) s.DropPeer(id);
        }
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

    uint32_t Server::Advance(double seconds, uint32_t maxTicks, uint32_t maxBacklogTicks) {
        Impl& s = *m_Impl;
        if (!s.started) return 0;
        const double dt = static_cast<double>(s.simulation->FixedTimeStep());
        s.accumulator += seconds;
        uint32_t ran = 0;
        while (s.accumulator >= dt && ran < maxTicks) {
            Tick();
            s.accumulator -= dt;
            ++ran;
        }
        s.accumulator = std::min(s.accumulator, static_cast<double>(maxBacklogTicks) * dt);
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
            e.resting = it != s.tracks.end() && it->second.resting && !it->second.held;
            out.push_back(e);
        }
        std::sort(out.begin(), out.end(), [](const EntityInfo& a, const EntityInfo& b) { return a.netId < b.netId; });
        return out;
    }

    size_t Server::AwakeBodies() const {
        size_t n = 0;
        for (const auto& [id, t] : m_Impl->tracks) n += t.resting || t.held ? 0u : 1u;
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
