module;

#include <glm/glm.hpp>

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
            if (logBytes <= settings.joinLogWarnBytes && logBytes + bytes > settings.joinLogWarnBytes)
                Log::Warn("Server: the event log passed {:.1f} MiB: every joiner receives all of it (until networking stage 7); joins are "
                          "refused above {:.1f} MiB",
                          static_cast<double>(settings.joinLogWarnBytes) / 1048576.0, static_cast<double>(settings.maxJoinLogBytes) / 1048576.0);
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
            for (auto& [id, p] : peers) {
                stats.clientsDisconnected += p.joined ? 1u : 0u;
                DropPlayer(p);
                transport.Disconnect(id);
            }
            peers.clear();
            queue.clear();
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
            }
        }

        void SendSnapshots(uint32_t tick) {
            const std::vector<std::pair<PeerId, Peer*>> players = PlayersBySlot();
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
                const std::vector<uint8_t> bytes = Encode(snap);
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
        const std::vector<glm::dvec3> focus = s.PlayerFocus();
        s.simulation->UpdateStreaming(focus);
        s.StepPlayers();
        s.RunCasts();
        const uint32_t tick = s.Tick();
        s.simulation->StepPhysics();
        s.simulation->UpdateLifetimes(s.simulation->FixedTimeStep());
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
