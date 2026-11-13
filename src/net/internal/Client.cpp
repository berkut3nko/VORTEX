module;

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
        bool SameWorldMaterials(const voxel::WorldMaterials& a, const voxel::WorldMaterials& b) {
            sim::RegionDesc x, y;
            x.terrain.materials = a;
            y.terrain.materials = b;
            return x == y;
        }
    }

    struct Client::Impl {
        Impl(ClientSettings s, ITransport& t) : settings(s), transport(t) {
            voxel::MaterialLibrary scratch;
            ids = voxel::RegisterDefaultWorldMaterials(scratch);
            buildHash = settings.buildHash != 0 ? settings.buildHash : BuildHash(scratch);
        }

        ClientSettings settings;
        ITransport& transport;
        voxel::WorldMaterials ids;
        uint64_t buildHash = 0;
        ClientState state = ClientState::Connecting;
        RefuseReason refusal = RefuseReason::Malformed;
        PeerId server = 0;
        Welcome welcome;
        sim::Simulation* simulation = nullptr;
        std::deque<EventBatch> pending;   // received before Attach (or not yet applied)
        uint32_t applied = 0;
        uint32_t serverLast = 0;
        ClientStats stats;
        std::unordered_map<uint32_t, scene::Entity> entities;   // NetId -> entity
        std::vector<sim::BlastResult> presentations;
        bool groupOpen = false;
        std::vector<ProbeHash> ownProbes;   // the Welcome's probe chunks as this client's generator makes them

        /// A failed client leaves the session: its slot is freed on the server (until stage 7 there
        /// is no resync).
        void Fail(const std::string& why) {
            Log::Error("Client: {}", why);
            state = ClientState::Failed;
            Leave();
        }

        void Leave() {
            if (server == 0) return;
            transport.Disconnect(server);
            server = 0;
        }

        void OnWelcome(const std::vector<uint8_t>& bytes) {
            Welcome w;
            if (!Decode(bytes, w)) return Fail("malformed Welcome");
            std::string why;
            if (!sim::IsValid(w.region, &why)) return Fail("the server's region is invalid: " + why);
            if (!SameWorldMaterials(w.region.terrain.materials, ids)) return Fail("the server's material ids differ from this build's");
            // Probe coordinates are network input too: a generator only ever runs inside the window.
            if (w.probes.size() > kProbeChunks) return Fail("malformed Welcome: too many probe chunks");
            const glm::ivec3 min = sim::RegionWindowMinChunk(w.region), size = sim::RegionWindowSize(w.region);
            const glm::ivec2 rows = sim::RegionRows(w.region);
            for (const ProbeHash& p : w.probes)
                if (p.chunk.x < min.x || p.chunk.x >= min.x + size.x || p.chunk.z < min.z || p.chunk.z >= min.z + size.z ||
                    p.chunk.y < rows.x || p.chunk.y >= rows.y)
                    return Fail("malformed Welcome: a probe chunk outside the region's window");
            welcome = std::move(w);
            // Terrain agreement: the probe chunks from this client's own generator.
            const std::shared_ptr<voxel::IWorldGenerator> generator = sim::MakeRegionGenerator(welcome.region, ids);
            ownProbes = RehashProbes(welcome.probes, *generator);
            stats.probeMismatches = 0;
            for (size_t i = 0; i < ownProbes.size(); ++i) stats.probeMismatches += ownProbes[i].hash != welcome.probes[i].hash ? 1u : 0u;
            if (stats.probeMismatches > 0)
                Log::Error("Client: {} of {} probe chunks differ from the server's: this build generates other terrain (server terrain "
                           "mode is networking stage 7; the session goes on and may diverge)",
                           stats.probeMismatches, welcome.probes.size());
            serverLast = std::max(serverLast, welcome.logEnd);
            state = ClientState::Welcomed;
        }

        void CloseGroup() { groupOpen = false; }

        void Present(const sim::WorldOp& op, sim::OpResult result, std::vector<voxel::VoxelEdit> edits) {
            sim::AppliedOp applied{op, std::move(result), std::move(edits)};
            const voxel::World& world = simulation->World();
            if (op.kind == sim::WorldOpKind::CarveSphere) {
                sim::BlastResult blast;
                constexpr double kInvQ = 1.0 / static_cast<double>(voxel::kQuantaPerVoxel);
                const double vs = static_cast<double>(world.VoxelSize());
                blast.center = glm::dvec3(op.sphere.center) * (kInvQ * vs);
                blast.radius = static_cast<float>(static_cast<double>(op.sphere.radius) * kInvQ * vs);
                blast.islandsSearched = true;
                blast.ops.push_back(std::move(applied));
                presentations.push_back(std::move(blast));
                groupOpen = true;
            } else if (op.kind == sim::WorldOpKind::Detach || op.kind == sim::WorldOpKind::ClearMask) {
                if (!groupOpen) {
                    sim::BlastResult late;
                    late.deferred = true;
                    presentations.push_back(std::move(late));
                    groupOpen = true;
                }
                if (op.kind == sim::WorldOpKind::ClearMask) presentations.back().crumbs += 1;
                presentations.back().ops.push_back(std::move(applied));
            } else {
                CloseGroup();
            }
        }

        void Apply(uint32_t seq, const Event& event) {
            if (const sim::WorldOp* op = std::get_if<sim::WorldOp>(&event)) {
                // Only live events are shown: the join log (template operations included) is history.
                const bool present = settings.presentation && seq > welcome.logEnd;
                std::vector<voxel::VoxelEdit> edits;
                const sim::OpResult r = sim::ApplyOp(simulation->World(), *op, present ? &edits : nullptr);
                stats.skippedChunks += static_cast<uint32_t>(r.report.skipped.size());
                stats.unsyncedChunks += static_cast<uint32_t>(r.report.unsynced.size());
                ++stats.opsApplied;
                if (!r.Complete())
                    return Fail("operation " + std::to_string(seq) + " could not be applied in full (" + std::to_string(r.report.skipped.size()) +
                                " chunk(s) not loaded, " + std::to_string(r.report.unsynced.size()) +
                                " without a store slot): this world no longer follows the server's (resync is networking stage 7)");
                if (present) Present(*op, r, std::move(edits));
                else CloseGroup();
                return;
            }
            const EntitySpawn& spawn = std::get<EntitySpawn>(event);
            CloseGroup();
            if (spawn.kind == EntityKind::Light && !entities.contains(spawn.netId)) {
                const sim::TemplateLight& l = spawn.light;
                entities[spawn.netId] =
                    simulation->Scene().CreateLight({.position = l.position, .color = l.color, .intensity = l.intensity, .radius = l.radius});
                ++stats.lightsSpawned;
            }
        }

        void ApplyPending() {
            if (state != ClientState::Playing) return;
            while (!pending.empty() && state == ClientState::Playing) {
                const EventBatch& batch = pending.front();
                for (size_t i = 0; i < batch.events.size(); ++i) {
                    const uint32_t seq = batch.firstOpSeq + static_cast<uint32_t>(i);
                    if (seq <= applied) {
                        ++stats.duplicateEvents;
                        continue;
                    }
                    if (seq != applied + 1) return Fail("a gap in the event stream: expected " + std::to_string(applied + 1) + ", got " +
                                                        std::to_string(seq));
                    Apply(seq, batch.events[i]);
                    if (state != ClientState::Playing) return;
                    applied = seq;
                    ++stats.eventsApplied;
                }
                pending.pop_front();
            }
        }

        void Receive() {
            TransportEvent e;
            while (transport.Poll(e)) {
                if (state == ClientState::Failed || state == ClientState::Refused) continue;
                switch (e.type) {
                    case TransportEvent::Type::Connected:
                        if (state == ClientState::Connecting) {
                            server = e.peer;
                            Hello hello;
                            hello.buildHash = buildHash;
                            hello.token = settings.token;
                            transport.Send(server, Channel::Events, Encode(hello));
                            state = ClientState::Joining;
                        }
                        break;
                    case TransportEvent::Type::Disconnected:
                        if (e.peer == server && state != ClientState::Refused) {
                            state = ClientState::Closed;
                            server = 0;
                        }
                        break;
                    case TransportEvent::Type::Received: {
                        if (e.peer != server) break;
                        const std::optional<MessageType> type = PeekType(e.data);
                        if (!type) break;
                        if (*type == MessageType::Refuse) {
                            Refuse r;
                            refusal = Decode(e.data, r) ? r.reason : RefuseReason::Malformed;
                            state = ClientState::Refused;
                            Log::Warn("Client: the server refused the session: {}", ToString(refusal));
                            Leave();
                            return;
                        } else if (*type == MessageType::Welcome && state == ClientState::Joining) {
                            OnWelcome(e.data);
                        } else if (*type == MessageType::EventBatch) {
                            EventBatch batch;
                            if (!Decode(e.data, batch)) return Fail("malformed event batch");
                            stats.eventBytes += e.data.size();
                            if (!batch.events.empty())
                                serverLast = std::max(serverLast, batch.firstOpSeq + static_cast<uint32_t>(batch.events.size()) - 1);
                            pending.push_back(std::move(batch));
                        } else if (*type == MessageType::ServerStatus) {
                            ServerStatus status;
                            if (Decode(e.data, status)) {   // unreliable: a stale or duplicate one only raises nothing
                                serverLast = std::max(serverLast, status.lastOpSeq);
                                ++stats.statusReceived;
                            }
                        }
                        break;
                    }
                }
            }
        }
    };

    Client::Client(ClientSettings settings, ITransport& transport) : m_Impl(std::make_unique<Impl>(settings, transport)) {}
    Client::~Client() = default;

    void Client::Poll() {
        Impl& s = *m_Impl;
        s.Receive();
        s.ApplyPending();
    }

    ClientState Client::State() const { return m_Impl->state; }
    RefuseReason Client::RefusalReason() const { return m_Impl->refusal; }
    const sim::RegionDesc& Client::Region() const { return m_Impl->welcome.region; }
    glm::dvec3 Client::Spawn() const { return m_Impl->welcome.spawn; }
    uint8_t Client::Slot() const { return m_Impl->welcome.slot; }

    sim::SimulationConfig Client::MakeSimulationConfig(sim::SimulationConfig base) const {
        base.world = sim::RegionWorldConfig(m_Impl->welcome.region, base.world);
        base.sessionServer = false;
        // The client holds the whole region window until stage 7: its store must hold it too.
        base.chunkCapacity = std::max(base.chunkCapacity, sim::RegionChunkCapacity(m_Impl->welcome.region));
        return base;
    }

    std::shared_ptr<voxel::IWorldGenerator> Client::MakeGenerator() const {
        return sim::MakeRegionGenerator(m_Impl->welcome.region, m_Impl->ids);
    }

    bool Client::Attach(sim::Simulation& simulation) {
        Impl& s = *m_Impl;
        if (s.state != ClientState::Welcomed) {
            Log::Error("Client: Attach needs the Welcomed state");
            return false;
        }
        const sim::RegionDesc& region = s.welcome.region;
        if (!simulation.IsInitialized()) {
            Log::Error("Client: Attach needs an initialised simulation");
            return false;
        }
        const voxel::WorldConfig& config = simulation.World().Config();
        // A session world is memory only (networking.md "Stage 1 as built"): a world that loads
        // saved chunks is not the world the server generated.
        if (!simulation.Config().world.saveDirectory.empty()) {
            Log::Error("Client: refused a simulation with a save directory: a session world is memory only");
            return false;
        }
        const voxel::WorldConfig expect = sim::RegionWorldConfig(region, config);
        const bool sameWorld = std::bit_cast<uint32_t>(config.voxelSize) == std::bit_cast<uint32_t>(region.terrain.voxelSize) &&
                               simulation.World().FullHeight() && config.windowSize.x == expect.windowSize.x &&
                               config.windowSize.z == expect.windowSize.z &&
                               std::bit_cast<uint32_t>(config.heightRange.x) == std::bit_cast<uint32_t>(region.heightRange.x) &&
                               std::bit_cast<uint32_t>(config.heightRange.y) == std::bit_cast<uint32_t>(region.heightRange.y) &&
                               config.heightSnap == expect.heightSnap;
        if (!sameWorld) {
            Log::Error("Client: refused a simulation whose world is not the session's (voxel size, window or height range; build it "
                       "with MakeSimulationConfig)");
            return false;
        }
        if (BuildHash(simulation.Materials()) != s.buildHash) {
            Log::Error("Client: refused a simulation whose material table is not the one this client announced");
            return false;
        }
        voxel::World& world = simulation.World();
        world.LoadBlocking(sim::RegionWindowFocus(region));
        if (!sim::WindowIsRegions(world, region)) {
            Log::Error("Client: refused a simulation whose window landed at ({}, {}, {}), not on the region's", world.WindowMinChunk().x,
                       world.WindowMinChunk().y, world.WindowMinChunk().z);
            return false;
        }
        const voxel::WorldStats ws = world.Stats();
        if (ws.storeFullLoads > 0 || ws.storeFullDecodes > 0) {
            Log::Error("Client: refused a simulation whose chunk store ({} slots) cannot hold the region's window (store full {} / {} "
                       "times; build it with MakeSimulationConfig)",
                       simulation.Chunks().Capacity(), ws.storeFullLoads, ws.storeFullDecodes);
            return false;
        }
        // The world must be what this client's generator makes, untouched: another generator, or a
        // world edited before Attach, would follow the server's operations into another world.
        if (const uint32_t differ = CheckProbes(s.ownProbes, world); differ > 0) {
            Log::Error("Client: refused a simulation whose world differs from its generator's in {} of {} probe chunks (another "
                       "generator, or edited before Attach)",
                       differ, s.ownProbes.size());
            return false;
        }
        s.simulation = &simulation;
        s.state = ClientState::Playing;
        s.ApplyPending();
        return s.state == ClientState::Playing;
    }

    uint32_t Client::AppliedOpSeq() const { return m_Impl->applied; }
    uint32_t Client::ServerOpSeq() const { return m_Impl->serverLast; }
    uint32_t Client::LogEnd() const { return m_Impl->welcome.logEnd; }
    const ClientStats& Client::Stats() const { return m_Impl->stats; }

    std::vector<sim::BlastResult> Client::TakePresentations() {
        std::vector<sim::BlastResult> out = std::move(m_Impl->presentations);
        m_Impl->presentations.clear();
        m_Impl->groupOpen = false;
        return out;
    }

    voxel::WorldHashResult Client::Hash() const {
        if (!m_Impl->simulation) return {};
        return RegionHash(m_Impl->simulation->World(), m_Impl->welcome.region);
    }
}
