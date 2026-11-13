module;

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

module vortex.net;

import vortex.core;
import vortex.voxel;
import vortex.scene;
import vortex.sim;

namespace vortex::net {

    namespace {
        struct LoggedEvent {
            uint32_t tick = 0;
            uint64_t maskVoxels = 0;      ///< Detach / ClearMask: what a client decodes (a batch holds at most kMaxMaskVoxels)
            std::vector<uint8_t> bytes;
        };

        struct Peer {
            bool joined = false;
            bool closing = false;         ///< refused: disconnect once the refusal is delivered
            uint8_t slot = 0;
            uint32_t sentUpTo = 0;        ///< last opSeq handed to the transport for it
            uint32_t connectTick = 0;     ///< Hello timeout
            uint32_t closingTick = 0;     ///< refusal grace period
        };

        struct QueuedCast {
            voxel::SphereQ sphere;
            uint16_t power = 0;
            uint32_t queuedTick = 0;
        };

        uint64_t MaskVoxels(const Event& event) {
            const sim::WorldOp* op = std::get_if<sim::WorldOp>(&event);
            if (!op || (op->kind != sim::WorldOpKind::Detach && op->kind != sim::WorldOpKind::ClearMask)) return 0;
            return op->mask.VoxelCount();
        }
    }

    struct Server::Impl {
        Impl(ServerSettings s, ITransport& t) : settings(std::move(s)), transport(t), region(settings.session.region) {}

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
        uint32_t nextNetId = 1;
        double accumulator = 0.0;
        ServerStats stats;
        std::deque<CastRecord> casts;
        size_t logBytes = 0;

        uint32_t Tick() const { return simulation ? simulation->Tick() : 0; }
        uint32_t LastOpSeq() const { return static_cast<uint32_t>(log.size()); }

        uint32_t Emit(const Event& event) {
            ByteWriter w;
            WriteEvent(w, event);
            const size_t bytes = w.Size();
            if (bytes > settings.eventBurstBytes) {
                ++stats.oversizeEvents;
                Log::Warn("Server: event {} is {} bytes, more than the {}-byte event burst (sent anyway)", log.size() + 1, bytes,
                          settings.eventBurstBytes);
            }
            log.push_back({Tick(), MaskVoxels(event), w.Take()});
            logBytes += bytes;
            ++stats.events;
            stats.eventBytes += bytes;
            eventCredit -= static_cast<int64_t>(bytes);
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
            for (const auto& [id, p] : peers) {
                stats.clientsDisconnected += p.joined ? 1u : 0u;
                transport.Disconnect(id);
            }
            peers.clear();
            queue.clear();
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
            uint8_t slot = 0;
            while (used[slot]) ++slot;
            Welcome welcome;
            welcome.slot = slot;
            welcome.tick = Tick();
            welcome.region = region;
            welcome.spawn = tmpl.spawn;
            welcome.logEnd = LastOpSeq();
            welcome.probes = probes;
            Send(peer, Channel::Events, Encode(welcome));
            // Join = the whole event log (until stage 7's chunk sync), on the same ordered channel,
            // paced by Flush like the live stream behind it.
            p.joined = true;
            p.slot = slot;
            p.sentUpTo = 0;
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
                        peers[e.peer] = peer;
                        break;
                    }
                    case TransportEvent::Type::Disconnected:
                        if (const auto it = peers.find(e.peer); it != peers.end()) {
                            if (it->second.joined) {
                                ++stats.clientsDisconnected;
                                Log::Info("Server: client {} left (slot {})", e.peer, it->second.slot);
                            }
                            peers.erase(it);
                        }
                        break;
                    case TransportEvent::Type::Received: {
                        const auto it = peers.find(e.peer);
                        if (it == peers.end() || it->second.closing) break;
                        if (!it->second.joined && e.channel == Channel::Events) {
                            if (PeekType(e.data) == MessageType::Hello) OnHello(e.peer, it->second, e.data);
                            else Refuse(e.peer, it->second, RefuseReason::Malformed);
                        }
                        // Stage 3 clients send nothing else (inputs are stage 4).
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
                const QueuedCast q = queue.front();
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
            spawn.netId = s.nextNetId++;
            spawn.light = light;
            s.Emit(spawn);
        }
        sim::CreateTemplateObjects(*s.simulation, s.tmpl);
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
        s.simulation->UpdateStreaming(std::span<const glm::dvec3>{});
        s.RunCasts();
        const uint32_t tick = s.Tick();
        s.simulation->StepPhysics();
        s.simulation->UpdateLifetimes(s.simulation->FixedTimeStep());
        s.Flush();
        if (tick % 2 == 0 && !s.failed) {
            const std::vector<uint8_t> status = Encode(ServerStatus{tick, s.LastOpSeq()});
            for (const auto& [id, p] : s.peers)
                if (p.joined) s.Send(id, Channel::State, status);
        }
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
        ++s.stats.castsQueued;
        if (!sim::CastFitsRegion(s.region, s.simulation->World(), sphere)) {
            ++s.stats.castsRejected;
            return false;
        }
        s.queue.push_back({sphere, power, s.Tick()});
        return true;
    }

    bool Server::QueueCast(const sim::BlastDesc& desc) {
        Impl& s = *m_Impl;
        if (!s.started) return false;
        const double vs = static_cast<double>(s.simulation->World().VoxelSize());
        return QueueCast(voxel::QuantizeSphere(desc.center / vs, static_cast<double>(desc.radius) / vs), voxel::QuantizePower(desc.power));
    }

    size_t Server::QueuedCasts() const { return m_Impl->queue.size(); }
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
