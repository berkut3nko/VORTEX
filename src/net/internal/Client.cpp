module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
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
import vortex.math;
import vortex.voxel;
import vortex.physics;
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

        /// Ticks of inputs and predicted states a client keeps (networking.md: 128).
        constexpr uint32_t kHistoryTicks = 128;

        ClockSettings ClockOf(const ClientSettings& s) {
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
        constexpr size_t kMaxWaitingSnapshots = 32;

        struct HistoryEntry {
            bool valid = false;
            uint32_t tick = 0;
            sim::PlayerInput input;
            sim::PlayerState state;      ///< after the tick
        };
    }

    struct Client::Impl {
        Impl(ClientSettings s, ITransport& t) : settings(s), transport(t), clock(ClockOf(s)) {
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
        ServerTime serverTime;              // where other players are drawn (only moves forward)
        TickClock clock;
        glm::vec3 offset{0.0f};             // visual error being blended out
        uint32_t blendLeft = 0;
        net::RemotePlayers remotes;
        PredictionStats prediction;

        /// The session's tick (Welcome; the attached simulation has the same, Attach checks it).
        float Dt() const { return simulation ? simulation->FixedTimeStep() : welcome.tickSeconds; }

        /// The colliders a step from where the player is can touch hold every edit applied so far,
        /// whatever the workers have finished (the server does the same before its steps).
        void FreshColliders() { simulation->Terrain().FlushBox(sim::PlayerReach(player.position, welcome.player)); }

        /// Puts the character into `state` on up-to-date colliders.
        void Restore(const sim::PlayerState& state) {
            player = state;
            FreshColliders();
            sim::RestorePlayer(simulation->Physics(), character, player);
        }

        /// Predicts tick `nextTick` with `input` and keeps it.
        sim::PlayerStepResult PredictTick(sim::PlayerInput input) {
            input.tick = nextTick;
            FreshColliders();
            const sim::PlayerStepResult r = sim::StepPlayer(simulation->Physics(), simulation->Terrain(), character, player, input,
                                                            welcome.player, Dt());
            history[nextTick % kHistoryTicks] = {true, nextTick, input, player};
            ++nextTick;
            return r;
        }

        void SendInputs() {
            if (server == 0 || nextTick == 0) return;
            const uint32_t count = std::clamp<uint32_t>(settings.maxInputsPerPacket, 1, kMaxBatchInputs);
            uint32_t first = std::max(ackTick + 1, nextTick > count ? nextTick - count : 0u);
            while (first < nextTick && (!history[first % kHistoryTicks].valid || history[first % kHistoryTicks].tick != first)) ++first;
            if (first >= nextTick) return;
            InputBatch batch;
            batch.firstTick = first;
            for (uint32_t t = first; t < nextTick; ++t) batch.inputs.push_back(history[t % kHistoryTicks].input);
            transport.Send(server, Channel::State, Encode(batch));
            ++prediction.inputPackets;
            prediction.inputsSent += batch.inputs.size();
        }

        /// The first snapshot whose events are applied: the player is where the server has it, and
        /// the client runs ahead by the round trip plus the clock's target. The ticks in between
        /// are what the server simulates without inputs from here (it repeats none: the player
        /// stands), so they are predicted that way and never sent.
        void StartPrediction(const Snapshot& snap) {
            Restore(snap.owner);
            predicting = true;
            nextTick = snap.tick + 1;
            const double rtt = server != 0 ? transport.RoundTripSeconds(server) : 0.0;
            // The round trip, a tick each way for the sender's and the receiver's tick (a message waits
            // for the next one), and the clock's target.
            const uint32_t lead = static_cast<uint32_t>(std::ceil(std::max(0.0, rtt) / static_cast<double>(Dt()))) + 2u +
                                  static_cast<uint32_t>(std::max(0, clock.Target()));
            sim::PlayerInput stand;
            stand.buttons = player.lastButtons;
            for (uint32_t i = 0; i < lead; ++i) PredictTick(stand);
            ackTick = std::max(ackTick, nextTick - 1);
            Log::Info("Client: predicting from server tick {} ({} ticks ahead)", snap.tick, nextTick - 1 - snap.tick);
        }

        /// networking.md "Player movement": the server's state at `snap.tick` against the prediction
        /// of that tick; at or above the threshold: restore, replay the later inputs, blend.
        void Reconcile(const Snapshot& snap) {
            ++prediction.compared;
            const uint32_t t = snap.tick;
            if (t >= nextTick) {
                // The server is at or past what this client predicts: take its state as it is (the
                // clock jumps forward on the margins).
                ++prediction.behind;
                Restore(snap.owner);
                nextTick = t + 1;
                history[t % kHistoryTicks] = {true, t, sim::PlayerInput{}, player};
                return;
            }
            const HistoryEntry& h = history[t % kHistoryTicks];
            if (nextTick - t > kHistoryTicks || !h.valid || h.tick != t) {
                ++prediction.tooOld;
                return;
            }
            const float error = glm::length(h.state.position - snap.owner.position);
            prediction.lastError = error;
            prediction.maxError = std::max(prediction.maxError, error);
            constexpr std::array<float, 5> kBounds = {0.001f, 0.02f, 0.05f, 0.2f, 1.5f};
            size_t bucket = 0;
            while (bucket < kBounds.size() && !(error < kBounds[bucket])) ++bucket;
            ++prediction.errorBuckets[bucket];
            if (error < settings.ignoreError) {
                ++prediction.within;
                return;
            }
            ++prediction.corrections;
            const glm::vec3 before = player.position;
            Restore(snap.owner);
            history[t % kHistoryTicks].state = player;
            const auto start = std::chrono::steady_clock::now();
            uint32_t replayed = 0;
            for (uint32_t k = t + 1; k < nextTick; ++k) {
                HistoryEntry& e = history[k % kHistoryTicks];
                FreshColliders();
                sim::StepPlayer(simulation->Physics(), simulation->Terrain(), character, player, e.input, welcome.player, Dt());
                e.state = player;
                ++replayed;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            prediction.replayTicks += replayed;
            prediction.maxReplayTicks = std::max(prediction.maxReplayTicks, replayed);
            prediction.replaySeconds += seconds;
            prediction.maxReplaySeconds = std::max(prediction.maxReplaySeconds, seconds);
            if (error >= settings.snapError) {
                ++prediction.snaps;
                offset = glm::vec3(0.0f);
                blendLeft = 0;
            } else {
                offset += before - player.position;
                blendLeft = settings.blendTicks;
            }
        }

        void OnSnapshot(const std::vector<uint8_t>& bytes) {
            Snapshot snap;
            if (!Decode(bytes, snap)) {
                ++stats.malformedSnapshots;
                return;
            }
            ++stats.snapshotsReceived;
            if (anySnapshot && snap.tick <= newestSnapshot) {
                ++stats.snapshotsStale;
                return;
            }
            anySnapshot = true;
            newestSnapshot = snap.tick;
            serverTime.Observe(snap.tick);
            serverLast = std::max(serverLast, snap.lastOpSeq);
            remotes.Add(snap.tick, snap.others);
            if (!snap.hasOwner) return;
            // Both are ticks of inputs this client made: never at or past the tick it predicts next
            // (a forged one would stop its sends or blind its clock for good).
            if (predicting && snap.ackInputTick < nextTick) ackTick = std::max(ackTick, snap.ackInputTick);
            if (predicting && snap.inputMargin != kNoInputMargin && snap.marginTick < nextTick) {
                const int32_t m = snap.inputMargin;
                prediction.lastMargin = m;
                prediction.minMargin = prediction.anyMargin ? std::min(prediction.minMargin, m) : m;
                prediction.maxMargin = prediction.anyMargin ? std::max(prediction.maxMargin, m) : m;
                prediction.anyMargin = true;
                // The margin is about marginTick (dropped too-early inputs included), not about the
                // acknowledged input: a client far too early has nothing acknowledged.
                const ClockAction a = clock.Report(m, snap.marginTick, nextTick, snap.tick);
                if (a.jumpTicks > 0) {
                    ++prediction.clockJumps;
                    prediction.clockJumpTicks += a.jumpTicks;
                }
                if (a.holdTicks > 0) {
                    ++prediction.hardResyncs;
                    prediction.resyncHoldTicks += a.holdTicks;
                    Log::Warn("Client: hard resync: inputs arrive {} ticks early, the clock holds {} ticks", m, a.holdTicks);
                }
            }
            waiting.push_back({std::move(snap), 0});
            while (waiting.size() > kMaxWaitingSnapshots) {
                waiting.pop_front();
                prediction.eventTimeouts += predicting ? 1u : 0u;
            }
        }

        /// The newest snapshot whose events are applied is compared (older ones are superseded);
        /// one that has waited eventWaitTicks for its events since it arrived is given up and
        /// counted. The age is the oldest waiting one's, so a stream of newer snapshots that all
        /// wait cannot hide a stall (review of stage 4).
        void TryReconcile() {
            if (waiting.empty() || state != ClientState::Playing || !simulation) return;
            size_t ready = waiting.size();
            for (size_t i = waiting.size(); i-- > 0;)
                if (waiting[i].snap.lastOpSeq <= applied) {
                    ready = i;
                    break;
                }
            if (ready < waiting.size()) {
                const WaitingSnapshot w = std::move(waiting[ready]);
                waiting.erase(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(ready) + 1);
                if (w.age > 0) ++prediction.waitedForEvents;
                if (!predicting) StartPrediction(w.snap);
                else Reconcile(w.snap);
            }
            // Before the prediction starts a joiner waits for its log as long as it takes.
            while (predicting && !waiting.empty() && waiting.front().age >= settings.eventWaitTicks) {
                ++prediction.eventTimeouts;
                waiting.pop_front();
            }
        }

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
            if (!sim::IsValid(w.player, &why)) return Fail("the server's player tuning is invalid: " + why);
            if (!std::isfinite(w.tickSeconds) || w.tickSeconds < 0.001f || w.tickSeconds > 0.1f)
                return Fail("the server's tick length is invalid");
            if (!std::isfinite(w.gravity.x) || !std::isfinite(w.gravity.y) || !std::isfinite(w.gravity.z) ||
                glm::length(w.gravity) > 1000.0f)
                return Fail("the server's gravity is invalid");
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
                        if (e.channel == Channel::State) {
                            // Unreliable: only snapshots come this way (lost, duplicated, reordered; a
                            // stale one updates nothing). Anything else here is counted and ignored:
                            // the stream that must not be corrupted is the events channel.
                            if (*type == MessageType::Snapshot &&
                                (state == ClientState::Joining || state == ClientState::Welcomed || state == ClientState::Playing))
                                OnSnapshot(e.data);
                            else
                                ++stats.malformedSnapshots;
                            break;
                        }
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
        s.TryReconcile();
    }

    uint32_t Client::TicksDue(double seconds) {
        Impl& s = *m_Impl;
        // Other players are drawn on real time, not on the clock's ticks (which jump and hold).
        s.serverTime.Advance(seconds / static_cast<double>(s.Dt()));
        return s.clock.TicksDue(seconds, static_cast<double>(s.Dt()));
    }

    sim::PlayerStepResult Client::StepPlayer(const sim::PlayerInput& controls) {
        Impl& s = *m_Impl;
        for (WaitingSnapshot& w : s.waiting) ++w.age;
        if (s.blendLeft > 0) {
            s.offset -= s.offset / static_cast<float>(s.blendLeft);
            if (--s.blendLeft == 0) s.offset = glm::vec3(0.0f);
        }
        if (!s.predicting || s.state != ClientState::Playing || !s.simulation) return {};
        const sim::PlayerStepResult r = s.PredictTick(controls);
        ++s.prediction.ticks;
        s.prediction.heldTicks += r.held ? 1u : 0u;
        s.prediction.bleedTicks += s.clock.Rate() < 1.0 ? 1u : 0u;
        s.prediction.catchUpTicks += s.clock.Rate() > 1.0 ? 1u : 0u;
        if (++s.sinceSend >= std::max(1u, s.settings.inputSendInterval)) {
            s.sinceSend = 0;
            s.SendInputs();
        }
        return r;
    }

    sim::PlayerStepResult Client::RunTick(const sim::PlayerInput& controls) {
        Impl& s = *m_Impl;
        if (!s.simulation || s.state != ClientState::Playing) {
            Poll();
            return {};
        }
        sim::Simulation& simulation = *s.simulation;
        const std::array<glm::dvec3, 1> focus = {s.predicting ? glm::dvec3(s.player.position) : s.welcome.spawn};
        simulation.UpdateStreaming(focus);
        Poll();
        const sim::PlayerStepResult r = StepPlayer(controls);
        simulation.StepPhysics();
        simulation.UpdateLifetimes(simulation.FixedTimeStep());
        return r;
    }

    bool Client::Predicting() const { return m_Impl->predicting; }
    const sim::PlayerState& Client::Player() const { return m_Impl->player; }
    glm::vec3 Client::PlayerViewPosition() const { return m_Impl->player.position + m_Impl->offset; }
    uint32_t Client::PredictTick() const { return m_Impl->nextTick; }
    std::vector<RemotePlayerView> Client::RemotePlayers() const {
        const Impl& s = *m_Impl;
        return s.remotes.Sample(s.serverTime.Now());
    }
    const sim::PlayerTuning& Client::Tuning() const { return m_Impl->welcome.player; }
    const PredictionStats& Client::Prediction() const { return m_Impl->prediction; }
    const TickClock& Client::Clock() const { return m_Impl->clock; }

    ClientState Client::State() const { return m_Impl->state; }
    RefuseReason Client::RefusalReason() const { return m_Impl->refusal; }
    const sim::RegionDesc& Client::Region() const { return m_Impl->welcome.region; }
    glm::dvec3 Client::Spawn() const { return m_Impl->welcome.spawn; }
    uint8_t Client::Slot() const { return m_Impl->welcome.slot; }

    sim::SimulationConfig Client::MakeSimulationConfig(sim::SimulationConfig base) const {
        base.world = sim::RegionWorldConfig(m_Impl->welcome.region, base.world);
        base.sessionServer = false;
        // The session's tick and gravity: every predicted step depends on them.
        base.fixedTimeStep = m_Impl->welcome.tickSeconds;
        base.physics.gravity = m_Impl->welcome.gravity;
        // The server's character mask (terrain only until stage 6): every body on a client is its
        // own rubble, which the server's characters never meet.
        base.physics.characterLayers = sim::kSessionCharacterLayers;
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
        const physics::PhysicsConfig& physicsConfig = simulation.Config().physics;
        const bool sameSteps = std::bit_cast<uint32_t>(simulation.FixedTimeStep()) == std::bit_cast<uint32_t>(s.welcome.tickSeconds) &&
                               std::bit_cast<uint32_t>(physicsConfig.gravity.x) == std::bit_cast<uint32_t>(s.welcome.gravity.x) &&
                               std::bit_cast<uint32_t>(physicsConfig.gravity.y) == std::bit_cast<uint32_t>(s.welcome.gravity.y) &&
                               std::bit_cast<uint32_t>(physicsConfig.gravity.z) == std::bit_cast<uint32_t>(s.welcome.gravity.z) &&
                               physicsConfig.characterLayers == sim::kSessionCharacterLayers;
        if (!sameSteps) {
            Log::Error("Client: refused a simulation whose tick length, gravity or character layers are not the session's (build it "
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
        // The own player, at the spawn point until the first snapshot places it; the ground around
        // it gets its colliders now (the server cooks its own when the player joins). The character
        // belongs to the simulation (Shutdown destroys it).
        s.player = sim::PlayerState{};
        s.player.position = glm::vec3(s.welcome.spawn);
        s.character = simulation.Physics().CreateCharacter(sim::PlayerCharacterDesc(s.player.position, s.welcome.player));
        const std::array<glm::dvec3, 1> focus = {s.welcome.spawn};
        simulation.Terrain().Update(focus, simulation.Config().colliderRadius);
        simulation.Terrain().Flush();
        s.ApplyPending();
        s.TryReconcile();
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
