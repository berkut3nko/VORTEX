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

module vortex.net;

import vortex.core;
import vortex.math;
import vortex.fx;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;
import :client_impl;

namespace vortex::net {
    using namespace detail;


    Client::Impl::Impl(ClientSettings s, ITransport& t) : settings(s), transport(t), sync(s.ringOps, s.ringBytes), clock(ClockOf(s)) {
        voxel::MaterialLibrary scratch;
        ids = voxel::RegisterDefaultWorldMaterials(scratch);
        buildHash = settings.buildHash != 0 ? settings.buildHash : BuildHash(scratch);
    }

    std::shared_ptr<voxel::IWorldGenerator> Client::Impl::Generator() const {
        std::shared_ptr<voxel::IWorldGenerator> g = sim::MakeRegionGenerator(welcome.region, ids);
        return settings.wrapGenerator ? settings.wrapGenerator(std::move(g)) : g;
    }

    void Client::Impl::Fail(const std::string& why) {
        Log::Error("Client: {}", why);
        state = ClientState::Failed;
        Leave();
    }

    void Client::Impl::Leave() {
        if (server == 0) return;
        transport.Disconnect(server);
        server = 0;
    }

    void Client::Impl::OnWelcome(const std::vector<uint8_t>& bytes) {
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
        // The world map: no map travels; this client bakes its own and compares (land.md "Decision" 3).
        if (w.region.generator == sim::RegionGenerator::Land) {
            std::shared_ptr<const voxel::LandMap> own = settings.landMap ? settings.landMap() : nullptr;
            if (!sim::CheckRegionLand(w.region, std::move(own), why)) return Fail(why);
        }
        // Probe coordinates are network input too: a generator only ever runs inside the window.
        if (w.probes.size() > kProbeChunks) return Fail("malformed Welcome: too many probe chunks");
        const glm::ivec3 min = sim::RegionWindowMinChunk(w.region), size = sim::RegionWindowSize(w.region);
        const glm::ivec2 rows = sim::RegionRows(w.region);
        for (const ProbeHash& p : w.probes)
            if (p.chunk.x < min.x || p.chunk.x >= min.x + size.x || p.chunk.z < min.z || p.chunk.z >= min.z + size.z ||
                p.chunk.y < rows.x || p.chunk.y >= rows.y)
                return Fail("malformed Welcome: a probe chunk outside the region's window");
        welcome = std::move(w);
        playArea = sim::PlayAreaOf(welcome.region, welcome.player);
        // [wp-7] The session's fight (fight.md "Wire").
        fight.mode = welcome.fight.mode;
        fight.maxHealth = welcome.fight.maxHealth;
        fight.maxCharges = welcome.fight.maxCharges;
        fight.chargeTicksFull = welcome.fight.chargeTicks;
        fight.respawnTicksFull = welcome.fight.respawnTicks;
        roundState.mode = welcome.fight.mode;
        // Terrain agreement: the probe chunks from this client's own generator.
        const std::shared_ptr<voxel::IWorldGenerator> generator = Generator();
        ownProbes = RehashProbes(welcome.probes, *generator);
        stats.probeMismatches = 0;
        for (size_t i = 0; i < ownProbes.size(); ++i) stats.probeMismatches += ownProbes[i].hash != welcome.probes[i].hash ? 1u : 0u;
        const SyncBox serverWindow = RegionSyncBox(welcome.region);
        sessionGenerator = std::make_shared<SessionGenerator>(generator, serverWindow.min, serverWindow.Max());
        serverLast = std::max(serverLast, welcome.joinOpSeq);
        applied = welcome.joinOpSeq;
        welcomed = std::chrono::steady_clock::now();
        state = ClientState::Welcomed;
        // Chunk sync (stage 7a, plan amendment 12): the window is known now (the region's, or a
        // moving one centred on the spawn, where Attach places it); its WindowUpdate goes at once,
        // so only chunks changed after the ack need data.
        if (settings.windowChunks < 0 || settings.windowChunks > kMaxClientWindowChunks)
            return Fail("ClientSettings::windowChunks " + std::to_string(settings.windowChunks) + " is outside 0.." +
                        std::to_string(kMaxClientWindowChunks));
        moving = sim::ClientWindowMoves(welcome.region, settings.windowChunks);
        sync.Start(welcome.joinOpSeq, RegionSyncBox(welcome.region));
        if (stats.probeMismatches > 0) {
            if (settings.terrainModeOnProbeMismatch) {
                Log::Warn("Client: {} of {} probe chunks differ from the server's: this build generates other terrain - server terrain "
                          "mode (the server sends the whole window)",
                          stats.probeMismatches, welcome.probes.size());
                StartTerrainMode(TerrainModeReason::Probes);
            } else {
                Log::Error("Client: {} of {} probe chunks differ from the server's: this build generates other terrain (terrain mode "
                           "off by its settings; the session goes on and may diverge)",
                           stats.probeMismatches, welcome.probes.size());
            }
        }
        SyncBox window;
        window.min = sim::ClientWindowMinChunk(welcome.region, settings.windowChunks, welcome.spawn);
        window.size = sim::ClientWindowSize(welcome.region, settings.windowChunks);
        if (window.size.x > kMaxClientWindowChunks || window.size.z > kMaxClientWindowChunks)
            return Fail("the region's window is " + std::to_string(window.size.x) + " x " + std::to_string(window.size.z) +
                        " chunks, more than a WindowUpdate may name (" + std::to_string(kMaxClientWindowChunks) +
                        "): this client needs a moving window (ClientSettings::windowChunks)");
        SendWindow(window, false);
    }

    void Client::Impl::ApplyPending() {
        if (state != ClientState::Playing) return;
        while (!pending.empty() && state == ClientState::Playing) {
            if (const WindowAck* ack = std::get_if<WindowAck>(&pending.front())) {
                if (!sync.OnAck(simulation->World(), *ack, applied)) return Fail("a WindowAck for a window this client did not send");
                pending.pop_front();
                continue;
            }
            if (const TerrainAck* ack = std::get_if<TerrainAck>(&pending.front())) {
                if (!sync.OnTerrainAck(simulation->World(), *ack, applied))
                    return Fail("a TerrainAck for a window this client did not send, or one that leaves an entering chunk undescribed");
                pending.pop_front();
                continue;
            }
            if (const HashCheck* check = std::get_if<HashCheck>(&pending.front())) {
                OnHashCheck(*check);
                pending.pop_front();
                continue;
            }
            if (const EntityCheck* check = std::get_if<EntityCheck>(&pending.front())) {
                OnEntityCheck(*check);
                pending.pop_front();
                continue;
            }
            if (const EntityList* list = std::get_if<EntityList>(&pending.front())) {
                OnEntityList(*list);
                pending.pop_front();
                continue;
            }
            const EventBatch& batch = std::get<EventBatch>(pending.front());
            for (size_t i = 0; i < batch.events.size(); ++i) {
                const uint32_t seq = batch.firstOpSeq + static_cast<uint32_t>(i);
                if (seq <= applied) {
                    ++stats.duplicateEvents;
                    continue;
                }
                if (seq != applied + 1) return Fail("a gap in the event stream: expected " + std::to_string(applied + 1) + ", got " +
                                                    std::to_string(seq));
                if (settings.testDropEntityEvent && !std::holds_alternative<sim::WorldOp>(batch.events[i]) &&
                    settings.testDropEntityEvent(seq, batch.events[i])) {
                    ++stats.testDroppedEvents;      // a test's fault: as if it never came
                } else {
                    Apply(seq, batch.events[i]);
                    if (std::holds_alternative<sim::WorldOp>(batch.events[i])) {
                        anyTerrainOp = true;
                        lastTerrainOpTick = nextTick;
                    }
                }
                if (state != ClientState::Playing) return;
                applied = seq;
                ++stats.eventsApplied;
            }
            pending.pop_front();
        }
    }

    void Client::Impl::Receive() {
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
                    // While connecting there is no server peer yet: the connection failed.
                    if (state == ClientState::Connecting) {
                        state = ClientState::Closed;
                    } else if (e.peer == server && state != ClientState::Refused) {
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
                    } else if (e.channel == Channel::Bulk) {
                        // ChunkData, BaselineEntity, BaselineEnd: decoded when processed (attached). The
                        // bulk channel is not ordered against the events channel: its first message
                        // may arrive before the Welcome.
                        if (state != ClientState::Joining && state != ClientState::Welcomed && state != ClientState::Playing) break;
                        stats.bulkBytes += e.data.size();
                        pendingBulk.push_back(std::move(e.data));
                    } else if (*type == MessageType::EventBatch) {
                        EventBatch batch;
                        if (!Decode(e.data, batch)) return Fail("malformed event batch");
                        stats.eventBytes += e.data.size();
                        if (!batch.events.empty())
                            serverLast = std::max(serverLast, batch.firstOpSeq + static_cast<uint32_t>(batch.events.size()) - 1);
                        pending.push_back(std::move(batch));
                    } else if (*type == MessageType::WindowAck) {
                        WindowAck ack;
                        if (!Decode(e.data, ack)) return Fail("malformed WindowAck");
                        pending.push_back(std::move(ack));
                    } else if (*type == MessageType::HashCheck) {
                        HashCheck check;
                        if (!Decode(e.data, check)) return Fail("malformed HashCheck");
                        pending.push_back(std::move(check));
                    } else if (*type == MessageType::EntityCheck) {
                        EntityCheck check;
                        if (!Decode(e.data, check)) return Fail("malformed EntityCheck");
                        pending.push_back(check);
                    } else if (*type == MessageType::EntityList) {
                        EntityList list;
                        if (!Decode(e.data, list)) return Fail("malformed EntityList");
                        pending.push_back(std::move(list));
                    } else if (*type == MessageType::Roster) {
                        // [wp-7] The fight's messages are taken on receipt (fight.md "Wire"), not by opSeq.
                        if (!Decode(e.data, roster)) return Fail("malformed Roster");
                    } else if (*type == MessageType::RoundUpdate) {
                        if (!Decode(e.data, roundState)) return Fail("malformed RoundUpdate");
                    } else if (*type == MessageType::KillNotice) {
                        KillNotice kill;
                        if (!Decode(e.data, kill)) return Fail("malformed KillNotice");
                        kills.push_back(kill);
                        while (kills.size() > 32) kills.pop_front();
                    } else if (*type == MessageType::TerrainAck) {
                        // Only for a terrain-mode WindowUpdate (stage 7b M-b).
                        if (!terrainMode) return Fail("a TerrainAck for a window this client did not send in terrain mode");
                        TerrainAck ack;
                        if (!Decode(e.data, ack)) return Fail("malformed TerrainAck");
                        ++stats.terrainAcks;
                        stats.terrainAckBytes += e.data.size();
                        pending.push_back(std::move(ack));
                    }
                    break;
                }
            }
        }
    }

    Client::Client(ClientSettings settings, ITransport& transport) : m_Impl(std::make_unique<Impl>(settings, transport)) {}
    Client::~Client() = default;

    void Client::Poll() {
        Impl& s = *m_Impl;
        using Clock = std::chrono::steady_clock;
        s.Receive();
        const Clock::time_point t0 = Clock::now();
        s.CheckWindow();
        if (s.state == ClientState::Playing && s.simulation) {
            const std::vector<glm::ivec3> loaded = s.simulation->World().TakeLoadedChunks();
            if (!loaded.empty()) s.sync.OnLoaded(s.simulation->World(), loaded, s.applied);
        }
        const Clock::time_point t1 = Clock::now();
        s.ApplyPending();
        s.ProcessBulk();
        const Clock::time_point t2 = Clock::now();
        s.SyncHousekeeping();
        // Chunk sync's own cost (review of stage 7a), the bulk installs are timed in ProcessBulk.
        if (s.state == ClientState::Playing) {
            const double seconds = std::chrono::duration<double>((t1 - t0) + (Clock::now() - t2)).count();
            s.stats.syncSeconds += seconds;
            s.stats.maxSyncSeconds = std::max(s.stats.maxSyncSeconds, seconds);
            ++s.stats.polls;
        }
        s.TakeReadySamples();
        s.TryReconcile();
    }

    uint32_t Client::TicksDue(double seconds, uint32_t maxTicks) {
        Impl& s = *m_Impl;
        // Other players are drawn on real time, not on the clock's ticks (which jump and hold).
        s.serverTime.Advance(seconds / static_cast<double>(s.Dt()));
        return s.clock.TicksDue(seconds, static_cast<double>(s.Dt()), maxTicks);
    }

    sim::PlayerStepResult Client::StepPlayer(const sim::PlayerInput& controls) {
        Impl& s = *m_Impl;
        for (WaitingSnapshot& w : s.waiting) ++w.age;
        if (s.blendLeft > 0) {
            s.offset -= s.offset / static_cast<float>(s.blendLeft);
            if (--s.blendLeft == 0) s.offset = glm::vec3(0.0f);
        }
        if (s.state == ClientState::Playing && s.simulation) s.MoveCopies();
        // [mech-1] The mechanisms' motion of the coming physics step (no crush, no anchors on a client); held copies
        // posed from their holders first.
        const auto stepMechanisms = [&s](const sim::PlayerInput& input) {
            if (s.state != ClientState::Playing || !s.simulation) return;
            s.PoseHeld(input);
            sim::StepMechanisms(*s.simulation, s.predicting ? s.nextTick - 1 : s.PoseTick(), false);
        };
        if (!s.predicting || s.state != ClientState::Playing || !s.simulation) {
            stepMechanisms(controls);
            return {};
        }
        constexpr uint8_t kHoldButtons = sim::PlayerButton_Use | sim::PlayerButton_Cast;
        const uint8_t holdLevels = static_cast<uint8_t>(controls.buttons & kHoldButtons);
        const uint8_t holdPresses = static_cast<uint8_t>(holdLevels & ~s.lastControls);
        s.lastControls = holdLevels;
        sim::PlayerStepResult r;
        s.syncHeld = !s.ReachCurrent();
        if (s.fight.dead || s.respawnPending) {
            // [wp-7] Dead (fight.md "Rules"): the input is kept and sent as today, the player is not stepped (the
            // server does not step it either); a respawn replays the inputs after the server's teleport (also those
            // made while it waits for the respawn snapshot's events).
            sim::PlayerInput input = controls;
            input.tick = s.nextTick;
            s.history[s.nextTick % kHistoryTicks] = {true, s.nextTick, input, s.player};
            ++s.nextTick;
        } else if (s.syncHeld) {
            // A chunk of its reach has no base (plan amendment 7): the player is not moved, the input
            // is kept and sent (the server moves its player), snapshots are taken as they are.
            sim::PlayerInput input = controls;
            input.tick = s.nextTick;
            s.history[s.nextTick % kHistoryTicks] = {true, s.nextTick, input, s.player};
            ++s.nextTick;
            ++s.prediction.syncHeldTicks;
        } else {
            if (s.needResume) {
                // Current again: the inputs after the last snapshot taken are replayed once.
                s.needResume = false;
                ++s.prediction.syncResumes;
                for (uint32_t k = s.lastTaken + 1; k < s.nextTick; ++k) {
                    HistoryEntry& e = s.history[k % kHistoryTicks];
                    if (!e.valid || e.tick != k) continue;
                    s.PrepareStep(k);
                    sim::StepPlayer(s.simulation->Physics(), s.simulation->Terrain(), s.character, s.player, e.input, s.welcome.player, s.playArea,
                                    s.ZonesAt(e.tick, e.spellMove), s.Dt());
                    e.state = s.player;
                }
            }
            // [mech-1] While a pick-up is pending, Use and Cast are latched (sent once it is confirmed, dropped if it
            // is refused); a confirmed pick-up's latched presses go with the next input.
            sim::PlayerInput input = controls;
            if (s.holdPending && !s.holdPending->drop) {
                s.latchedEdges = static_cast<uint8_t>(s.latchedEdges | holdPresses);
                input.buttons = static_cast<uint8_t>(input.buttons & ~kHoldButtons);
            } else if (s.latchedOut != 0) {
                input.buttons = static_cast<uint8_t>(input.buttons | s.latchedOut);
                s.latchedOut = 0;
            }
            const bool holding = s.heldNetId != 0 || s.holdPending.has_value();
            r = s.PredictTick(input);
            s.PredictHold(r, s.history[(s.nextTick - 1) % kHistoryTicks].input);
            if (holding || s.holdPending) r.cast = false;    // a throw or a latched press casts nothing
        }
        ++s.prediction.ticks;
        s.prediction.heldTicks += r.held ? 1u : 0u;
        s.prediction.bleedTicks += s.clock.Rate() < 1.0 ? 1u : 0u;
        s.prediction.catchUpTicks += s.clock.Rate() > 1.0 ? 1u : 0u;
        if (++s.sinceSend >= std::max(1u, s.settings.inputSendInterval)) {
            s.sinceSend = 0;
            s.SendInputs();
        }
        stepMechanisms(s.history[(s.nextTick - 1) % kHistoryTicks].input);
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
        // A moving window follows the player (stage 7a M-b); Poll sends its WindowUpdate.
        if (s.moving) simulation.UpdateStreaming(focus[0], focus);
        else simulation.UpdateStreaming(focus);
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
    Client::CastStatus Client::LastCast() const { return m_Impl->lastCast; }
    const FightView& Client::Fight() const { return m_Impl->fight; }
    const Roster& Client::RosterOf() const { return m_Impl->roster; }
    const RoundUpdate& Client::RoundOf() const { return m_Impl->roundState; }
    std::vector<KillNotice> Client::TakeKillNotices() {
        std::vector<KillNotice> out(m_Impl->kills.begin(), m_Impl->kills.end());
        m_Impl->kills.clear();
        return out;
    }
    const Client::MagicView& Client::Magic() const { return m_Impl->magic; }
    void Client::SendAttune(uint8_t craftA, uint8_t craftB) {
        if (m_Impl->server == 0 || m_Impl->state != ClientState::Playing) return;
        m_Impl->transport.Send(m_Impl->server, Channel::Events, Encode(Attune{craftA, craftB}));
    }
    std::vector<CastFx> Client::TakeCastFx() {
        std::vector<CastFx> out(m_Impl->castFx.begin(), m_Impl->castFx.end());
        m_Impl->castFx.clear();
        return out;
    }
    sim::TeamId Client::TeamOf(uint8_t slot) const {
        for (const RosterEntry& e : m_Impl->roster.players)
            if (e.slot == slot) return e.team;
        return sim::kNoTeam;
    }
    const PredictionStats& Client::Prediction() const { return m_Impl->prediction; }
    const TickClock& Client::Clock() const { return m_Impl->clock; }

    ClientState Client::State() const { return m_Impl->state; }
    RefuseReason Client::RefusalReason() const { return m_Impl->refusal; }
    const sim::RegionDesc& Client::Region() const { return m_Impl->welcome.region; }
    glm::dvec3 Client::Spawn() const { return m_Impl->welcome.spawn; }
    uint8_t Client::Slot() const { return m_Impl->welcome.slot; }

    sim::SimulationConfig Client::MakeSimulationConfig(sim::SimulationConfig base, uint32_t storeLimit) const {
        // The client's window (stage 7a M-b): the region's, or one that follows the player; load
        // reports on, unloaded edits dropped (a chunk that comes back is the server's to send).
        base.world = sim::ClientWorldConfig(m_Impl->welcome.region, m_Impl->settings.windowChunks, base.world);
        base.sessionServer = false;
        // The session's tick and gravity: every predicted step depends on them.
        base.fixedTimeStep = m_Impl->welcome.tickSeconds;
        base.physics.gravity = m_Impl->welcome.gravity;
        // The server's character mask (terrain and Dynamic bodies, stage 6): here the Dynamic layer
        // holds only copies of server bodies at rest; this client's own rubble is Debris or Rubble.
        base.physics.characterLayers = sim::kSessionCharacterLayers;
        base.physics.characterMovesBodies = false;     // as the server's (its bodies are copies here)
        // This client's own rubble never blocks its predicted player (the server has none of it).
        base.scene.rubbleLayer = true;
        // The store holds every cell of the window.
        base.chunkCapacity = std::max(base.chunkCapacity, sim::ClientChunkCapacity(m_Impl->welcome.region, m_Impl->settings.windowChunks));
        // ... unless the owner cannot have that many (the game's GPU store): then Attach decides
        // whether the region fits.
        if (storeLimit != 0) base.chunkCapacity = std::min(base.chunkCapacity, storeLimit);
        return base;
    }

    std::shared_ptr<voxel::IWorldGenerator> Client::MakeGenerator() const {
        if (m_Impl->sessionGenerator) return m_Impl->sessionGenerator;
        return m_Impl->Generator();
    }
    bool Client::TerrainMode() const { return m_Impl->terrainMode; }
    TerrainModeReason Client::TerrainModeWhy() const { return m_Impl->terrainReason; }
    std::shared_ptr<const SessionGenerator> Client::ClientGenerator() const { return m_Impl->sessionGenerator; }

    bool Client::Attach(sim::Simulation& simulation, const std::function<void()>& whileLoading) {
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
        const voxel::WorldConfig expect = sim::ClientWorldConfig(region, s.settings.windowChunks, config);
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
                               physicsConfig.characterLayers == sim::kSessionCharacterLayers && !physicsConfig.characterMovesBodies &&
                               simulation.Config().scene.rubbleLayer;
        if (!sameSteps) {
            Log::Error("Client: refused a simulation whose tick length, gravity, character layers or rubble layer are not the session's "
                       "(build it with MakeSimulationConfig)");
            return false;
        }
        if (!config.reportLoads || config.keepUnloadedEdits) {
            Log::Error("Client: refused a simulation whose world keeps no load reports or keeps unloaded edits (build it with "
                       "MakeSimulationConfig)");
            return false;
        }
        if (BuildHash(simulation.Materials()) != s.buildHash) {
            Log::Error("Client: refused a simulation whose material table is not the one this client announced");
            return false;
        }
        voxel::World& world = simulation.World();
        // A fixed window is loaded whole; a moving one (stage 7a M-b) is placed at the spawn and
        // loads its surface and everything near the spawn, the rest streams in play.
        const glm::dvec3 windowFocus = s.moving ? s.welcome.spawn : sim::RegionWindowFocus(region);
        const voxel::LoadScope scope = s.moving ? voxel::LoadScope::Surface : voxel::LoadScope::Everything;
        if (whileLoading) {
            // Load in streaming steps so the owner can keep its connection alive; LoadBlocking then
            // only finishes what is left (nothing, normally).
            uint32_t step = 0;
            for (world.Update(windowFocus); !world.IsFilled(scope); world.Update(windowFocus)) {
                // World::Stats walks every cell: a store that fills is looked for now and then only.
                if (++step % 64 == 0) {
                    const voxel::WorldStats ws = world.Stats();
                    if (ws.storeFullLoads > 0 || ws.storeFullDecodes > 0) break;
                }
                whileLoading();
                // The callback polls: the session may have ended meanwhile (a disconnect, a refusal,
                // a malformed message that failed this client).
                if (s.state != ClientState::Welcomed) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (s.state != ClientState::Welcomed) {
                Log::Error("Client: the session ended while the region loaded ({}): not attached",
                           s.state == ClientState::Failed ? "this client failed" : s.state == ClientState::Refused ? "refused" : "disconnected");
                return false;
            }
        }
        world.LoadBlocking(windowFocus, scope);
        const glm::ivec3 expectMin = sim::ClientWindowMinChunk(region, s.settings.windowChunks, windowFocus);
        if (world.WindowMinChunk() != expectMin || world.WindowSize() != sim::ClientWindowSize(region, s.settings.windowChunks)) {
            Log::Error("Client: refused a simulation whose window landed at ({}, {}, {}), not at ({}, {}, {})", world.WindowMinChunk().x,
                       world.WindowMinChunk().y, world.WindowMinChunk().z, expectMin.x, expectMin.y, expectMin.z);
            return false;
        }
        const voxel::WorldStats ws = world.Stats();
        if (ws.storeFullLoads > 0 || ws.storeFullDecodes > 0) {
            Log::Error("Client: refused a simulation whose chunk store ({} slots) cannot hold the region's window (store full {} / {} "
                       "times): the region is too large for this client (a smaller session region, or networking stage 7's client "
                       "window)",
                       simulation.Chunks().Capacity(), ws.storeFullLoads, ws.storeFullDecodes);
            return false;
        }
        const uint32_t used = simulation.Chunks().UsedCount(), capacity = simulation.Chunks().Capacity();
        if (capacity - std::min(used, capacity) < s.settings.minFreeStoreSlots) {
            Log::Error("Client: the region's window takes {} of this client's {} chunk-store slots, leaving fewer than the {} a "
                       "session needs: the region is too large for this client (a smaller session region, or networking stage 7's "
                       "client window)",
                       used, capacity, s.settings.minFreeStoreSlots);
            return false;
        }
        // The world must be what this client's generator makes, untouched: another generator, or a
        // world edited before Attach, would follow the server's operations into another world.
        // A moving window checks the probes it has loaded (the others are checked against the
        // generator at the Welcome).
        // In server terrain mode (stage 7b) the world inside the server's window is the server's to
        // give: its probe chunks are the SessionGenerator's air until then, nothing to compare.
        std::vector<ProbeHash> loadedProbes;
        for (const ProbeHash& p : s.ownProbes)
            if (!s.terrainMode && world.IsChunkLoaded(p.chunk)) loadedProbes.push_back(p);
        if (const uint32_t differ = CheckProbes(loadedProbes, world); differ > 0) {
            Log::Error("Client: refused a simulation whose world differs from its generator's in {} of {} probe chunks (another "
                       "generator, or edited before Attach)",
                       differ, loadedProbes.size());
            return false;
        }
        if (!s.moving && !s.terrainMode && loadedProbes.size() != s.ownProbes.size()) {
            Log::Error("Client: {} of {} probe chunks are not loaded after the region's window was", s.ownProbes.size() - loadedProbes.size(),
                       s.ownProbes.size());
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
        sim::SetPlayAreaWalls(simulation.Physics(), s.playArea, s.welcome.player);   // as the server's (WP1)
        const std::array<glm::dvec3, 1> focus = {s.welcome.spawn};
        simulation.Terrain().Update(focus, simulation.Config().colliderRadius);
        simulation.Terrain().Flush();
        // Nothing is based yet (the ack, kept in the stream, bases what is loaded). The window is the
        // one the Welcome's WindowUpdate named; should it not be, the next update says so (nothing has
        // been applied to any chunk yet, so no reset is needed).
        s.windowRevision = world.WindowRevision();
        (void)world.TakeWindowSteps();       // the placement: named by the Welcome's update (or the next line)
        if (const SyncBox window{world.WindowMinChunk(), world.WindowSize()}; window != s.sync.Window()) s.SendWindow(window, false);
        s.sync.OnLoaded(world, world.TakeLoadedChunks(), s.applied);
        s.ApplyPending();
        s.ProcessBulk();
        s.SyncHousekeeping();
        s.TakeReadySamples();
        s.TryReconcile();
        return s.state == ClientState::Playing;
    }

    uint32_t Client::AppliedOpSeq() const { return m_Impl->applied; }
    uint32_t Client::ServerOpSeq() const { return m_Impl->serverLast; }
    uint32_t Client::JoinOpSeq() const { return m_Impl->welcome.joinOpSeq; }
    uint32_t Client::JoinTick() const { return m_Impl->welcome.tick; }
    bool Client::Synced() const {
        const bool synced = m_Impl->Synced();
        if (synced) m_Impl->NoteSynced();     // the join's log time only (see NoteSynced)
        return synced;
    }
    SyncBox Client::SyncWindow() const { return m_Impl->syncBox; }
    SyncBox Client::Window() const { return m_Impl->sync.Window(); }
    bool Client::WindowMoves() const { return m_Impl->moving; }
    uint32_t Client::ChunksToStart() const {
        const Impl& s = *m_Impl;
        if (s.predicting) return 0;
        if (s.state != ClientState::Playing || !s.simulation) return s.sync.NotCurrent();
        // The server holds the player at the spawn until its client predicts (it sends no inputs).
        return s.NotCurrentNear(glm::vec3(s.welcome.spawn), false);
    }
    const ChunkSync& Client::Sync() const { return m_Impl->sync; }
    const ClientStats& Client::Stats() const { return m_Impl->stats; }

    std::vector<EntityInfo> Client::Entities() const {
        const Impl& s = *m_Impl;
        std::vector<EntityInfo> out;
        if (!s.simulation) return out;
        for (const auto& [netId, c] : s.copies) {
            const scene::Transform* transform = s.simulation->Scene().TryGet<scene::Transform>(c.entity);
            if (!transform) continue;
            EntityInfo e;
            e.netId = netId;
            e.modelHash = c.modelHash;
            e.position = transform->current.position;
            e.rotation = transform->current.rotation;
            e.resting = c.resting;
            out.push_back(e);
        }
        std::sort(out.begin(), out.end(), [](const EntityInfo& a, const EntityInfo& b) { return a.netId < b.netId; });
        return out;
    }

    bool Client::IsCopy(scene::Entity entity) const {
        if (entity == scene::kNoEntity) return false;
        for (const auto& [netId, c] : m_Impl->copies)
            if (c.entity == entity) return true;
        return false;
    }

    const RemoteBodies& Client::Bodies() const { return m_Impl->bodies; }
    uint32_t Client::HeldNetId() const { return m_Impl->heldNetId; }
    bool Client::HoldPending() const { return m_Impl->holdPending.has_value(); }
    bool Client::HeldPinned() const { return m_Impl->heldPinned; }
    const std::string& Client::HoldNotice() const { return m_Impl->holdNotice; }
    Client::UseStatus Client::LastUse() const { return {m_Impl->lastUseTick, m_Impl->lastUseResult}; }
    sim::UseTargetHooks Client::UseHooks() const { return m_Impl->MakeUseHooks(); }
    physics::CharacterId Client::Character() const { return m_Impl->character; }
    scene::Entity Client::CopyOf(uint32_t netId) const {
        const auto it = m_Impl->copies.find(netId);
        return it != m_Impl->copies.end() ? it->second.entity : scene::kNoEntity;
    }

    std::vector<sim::BlastResult> Client::TakePresentations() {
        std::vector<sim::BlastResult> out = std::move(m_Impl->presentations);
        m_Impl->presentations.clear();
        m_Impl->groupOpen = false;
        return out;
    }

    size_t Client::Tombstones() const { return m_Impl->tombstones.size(); }
    std::vector<glm::ivec3> Client::MismatchedChunks() const { return {m_Impl->mismatched.begin(), m_Impl->mismatched.end()}; }

    bool Client::TestDestroyCopy(uint32_t netId) {
        Impl& s = *m_Impl;
        if (!s.simulation || !s.copies.contains(netId)) return false;
        s.RemoveCopy(netId);
        ++s.stats.copiesLost;
        return true;
    }

    voxel::WorldHashResult Client::Hash() const {
        const Impl& s = *m_Impl;
        if (!s.simulation) return {};
        voxel::WorldHashResult h = voxel::WorldHash(s.simulation->World(), s.syncBox.min, s.syncBox.Max());
        // A chunk without a current base is not this client's copy of the server's (stage 7a).
        h.notLoaded += s.sync.NotCurrent();
        return h;
    }

    std::string DescribeChecks(const Client& client) {
        const ClientStats& c = client.Stats();
        const ChunkSyncStats& s = client.Sync().Stats();
        return std::format("{} hash checks ({} equal, {} ahead, {} not current ({} kept until loaded, {} equal then), {} outside), {} hash "
                           "mismatches ({} pristine, {} modified), {} repairs requested / {} installed, {} repeated ({} refused), {} pristine "
                           "failures; {} entity checks + {} partial ({} skipped, {} "
                           "ahead), {} entity mismatches, {} lists ({} restored / {} replaced / {} dropped, {} backed off); {} out of order; "
                           "tombstones {} ({} evicted); checks {:.2f} ms max, {:.1f} ms in all",
                           c.hashChecks, s.checkEqual, s.checkAhead, s.checkNotCurrent, s.checkDeferred, s.deferredEqual, s.checkOutside, s.mismatches,
                           c.mismatchesPristine, c.mismatchesModified, c.repairsRequested, s.repairsInstalled, s.repeatRepairs, s.repairsRefused,
                           c.pristineFailures, c.entityChecks,
                           c.entityChecksPartial, c.entityChecksSkipped, c.entityChecksAhead, c.entityMismatches, c.entityLists, c.copiesRestored,
                           c.copiesReplaced, c.copiesDropped, c.entityListsBackedOff, c.checksOutOfOrder, client.Tombstones(),
                           c.tombstonesEvicted, c.maxCheckSeconds * 1000.0, c.checkSeconds * 1000.0);
    }

    std::string DescribeTerrainMode(const Client& client) {
        if (!client.TerrainMode()) return "no";
        const ClientStats& c = client.Stats();
        const ChunkSyncStats& s = client.Sync().Stats();
        const std::shared_ptr<const SessionGenerator> g = client.ClientGenerator();
        const std::string why = client.TerrainModeWhy() == TerrainModeReason::Probes
                                    ? std::format("since the Welcome ({} of {} probes differ)", c.probeMismatches, kProbeChunks)
                                    : std::format("at tick {} (pristine chunks made differently)", c.terrainModeTick);
        return std::format("{}; {} acks / {:.1f} KiB, {} uniform based without data + {} installed, {} awaiting data ({} hidden); {} ChunkData / "
                           "{:.2f} MiB received ({} installed packed, {} shown); {} chunks answered Empty by the generator, {} dense made "
                           "inside the server's window; player held {} ticks",
                           why, c.terrainAcks, static_cast<double>(c.terrainAckBytes) / 1024.0, s.uniformBased, s.uniformInstalled, s.awaitingData,
                           s.awaitingHidden, c.terrainChunkData, static_cast<double>(c.terrainChunkBytes) / (1024.0 * 1024.0), s.installedHidden,
                           s.installedShown, g ? g->Skipped() : 0, g ? g->DenseInsideInTerrainMode() : 0, client.Prediction().syncHeldTicks);
    }
}
