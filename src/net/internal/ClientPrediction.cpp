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

// Client::Impl: predicted movement, inputs, snapshots and reconciliation (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    void Client::Impl::Restore(const sim::PlayerState& state) {
        player = state;
        FreshColliders();
        sim::RestorePlayer(simulation->Physics(), character, player);
    }

    bool Client::Impl::ReachCurrent() const {
        const math::AABB box = sim::PlayerReach(player.position, welcome.player);
        const voxel::World& world = simulation->World();
        const glm::ivec3 lo = voxel::VoxelToChunk(world.WorldToVoxel(glm::dvec3(box.min)));
        const glm::ivec3 hi = voxel::VoxelToChunk(world.WorldToVoxel(glm::dvec3(box.max)));
        return sync.AllCurrent(lo, hi);
    }

    uint32_t Client::Impl::NotCurrentNear(const glm::vec3& feet, bool first) const {
        const double size = static_cast<double>(voxel::kChunkSize) * static_cast<double>(welcome.region.terrain.voxelSize);
        const double r = static_cast<double>(settings.startRadius);
        const glm::dvec3 p(feet);
        const glm::ivec3 lo = glm::ivec3(glm::floor((p - glm::dvec3(r)) / size)), hi = glm::ivec3(glm::floor((p + glm::dvec3(r)) / size));
        uint32_t n = 0;
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const glm::ivec3 c(x, y, z);
                    if (!sync.Window().Contains(c)) continue;
                    const glm::dvec3 cmin = glm::dvec3(c) * size;
                    if (glm::length(p - glm::clamp(p, cmin, cmin + glm::dvec3(size))) > r) continue;
                    if (sync.IsCurrent(c)) continue;
                    if (first) return 1;
                    ++n;
                }
        return n;
    }

    sim::PlayerStepResult Client::Impl::PredictTick(sim::PlayerInput input) {
        input.tick = nextTick;
        PrepareStep(nextTick);
        const sim::PlayerStepResult r = sim::StepPlayer(simulation->Physics(), simulation->Terrain(), character, player, input,
                                                        welcome.player, playArea, Dt());
        history[nextTick % kHistoryTicks] = {true, nextTick, input, player};
        ++nextTick;
        return r;
    }

    void Client::Impl::SendInputs() {
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

    void Client::Impl::StartPrediction(const Snapshot& snap) {
        Restore(snap.owner);
        predicting = true;
        nextTick = snap.tick + 1;
        clock.DropBacklog();
        // The transport's recent round trip: one that a stall of this side does not inflate
        // (ITransport::RoundTripSeconds; review of stage 5).
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

    void Client::Impl::OnOwnerFight(const Snapshot& snap) {
        const Snapshot::Fight& f = snap.fight;
        const bool wasDead = fight.dead;
        fight.health = f.health;
        fight.charges = f.charges;
        fight.chargeTicks = f.chargeTicks;
        fight.dead = (f.flags & Snapshot::Fight::kDead) != 0;
        fight.protectedNow = (f.flags & Snapshot::Fight::kProtected) != 0;
        fight.spectator = (f.flags & Snapshot::Fight::kSpectator) != 0;
        fight.respawnTicks = f.respawnTicks;
        fight.hits = f.hits;
        fight.tick = snap.tick;
        const bool newLife = lifeKnown && f.lifeSeq != fight.lifeSeq;
        fight.lifeSeq = f.lifeSeq;
        if (!lifeKnown) {
            lifeKnown = true;
            lifeStartTick = snap.tick;
            return;
        }
        if (!predicting || !simulation) {
            if (newLife) lifeStartTick = snap.tick;
            return;
        }
        if (newLife && !fight.dead) {
            // A respawn (decision 7): taken by Reconcile once a snapshot of this life is ready (its events applied),
            // so that its replay steps on the terrain the server stepped (review of [wp-7]).
            lifeStartTick = snap.tick;
            respawnPending = true;
            needResume = false;
        } else if (fight.dead && !wasDead) {
            // A death (fight.md "Rules"): the server froze the player where it died; so does the prediction. The
            // inputs predicted after this snapshot keep their place in the history (the server may respawn the
            // player before their ticks and step them: Respawned replays them), with the frozen state; a resume
            // left over from a hold is dropped (review of [wp-7]: it replayed them a second time after the respawn).
            ++prediction.deathsSeen;
            Restore(snap.owner);
            for (uint32_t k = snap.tick + 1; k < nextTick; ++k) {
                HistoryEntry& e = history[k % kHistoryTicks];
                if (e.valid && e.tick == k) e.state = player;
            }
            respawnPending = false;
            needResume = false;
            offset = glm::vec3(0.0f);
            blendLeft = 0;
            Log::Info("Client: died at tick {} (life {})", snap.tick, f.lifeSeq);
        }
    }

    void Client::Impl::Respawned(const Snapshot& snap) {
        // The server's teleport (decision 7): its state at S (a snapshot of the new life, its events applied) is
        // taken and the inputs after S replayed as Reconcile replays them; no correction, no snap and no blend (the
        // camera cuts). Over a reach without a base the state is taken as the held path takes it, and the replay
        // waits for the resume (plan amendment 7).
        ++prediction.respawns;
        respawnPending = false;
        needResume = false;
        const uint32_t t = snap.tick;
        Restore(snap.owner);
        offset = glm::vec3(0.0f);
        blendLeft = 0;
        if (t >= nextTick) {
            nextTick = t + 1;
            history[t % kHistoryTicks] = {true, t, sim::PlayerInput{}, player};
        } else {
            history[t % kHistoryTicks].valid = true;
            history[t % kHistoryTicks].tick = t;
            history[t % kHistoryTicks].state = player;
            if (!ReachCurrent()) {
                ++prediction.syncTaken;
                lastTaken = t;
                needResume = true;
            } else {
                for (uint32_t k = t + 1; k < nextTick; ++k) {
                    HistoryEntry& e = history[k % kHistoryTicks];
                    if (!e.valid || e.tick != k) continue;
                    PrepareStep(k);
                    sim::StepPlayer(simulation->Physics(), simulation->Terrain(), character, player, e.input, welcome.player, playArea, Dt());
                    e.state = player;
                }
            }
        }
        Log::Info("Client: respawned at tick {} (life {})", t, fight.lifeSeq);
    }

    void Client::Impl::Reconcile(const Snapshot& snap) {
        // [wp-7] A dead player is not predicted, and a snapshot of an earlier life is not compared (fight.md "Rules").
        if (fight.dead || (snap.fight.flags & Snapshot::Fight::kDead) != 0 || snap.tick < lifeStartTick) return;
        if (respawnPending) {
            Respawned(snap);
            return;
        }
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
        if (syncHeld) {
            // Held over chunks without a base (plan amendment 7): the server's state is taken as it
            // is, nothing is counted as a correction; the inputs after it are replayed once the
            // reach is current again.
            ++prediction.syncTaken;
            Restore(snap.owner);
            history[t % kHistoryTicks].state = player;
            lastTaken = t;
            needResume = true;
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
        {
            // [mech-1] A correction where a mechanism can be: the event came after its segment's start (late).
            const math::AABB reach = sim::PlayerReach(snap.owner.position, welcome.player);
            for (const auto& [netId, c] : copies)
                if (c.mechanism)
                    if (const scene::Mechanism* m = simulation->Scene().TryGet<scene::Mechanism>(c.entity); m && m->swept.Overlaps(reach)) {
                        ++stats.mechanismCorrections;
                        break;
                    }
        }
        LogCorrection(snap, h.state, error);
        const glm::vec3 before = player.position;
        Restore(snap.owner);
        history[t % kHistoryTicks].state = player;
        const auto start = std::chrono::steady_clock::now();
        uint32_t replayed = 0;
        for (uint32_t k = t + 1; k < nextTick; ++k) {
            HistoryEntry& e = history[k % kHistoryTicks];
            PrepareStep(k);
            sim::StepPlayer(simulation->Physics(), simulation->Terrain(), character, player, e.input, welcome.player, playArea, Dt());
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

    void Client::Impl::LogCorrection(const Snapshot& snap, const sim::PlayerState& predicted, float error) {
        constexpr uint64_t kLoggedCorrections = 16;
        if (prediction.corrections > kLoggedCorrections) return;
        const glm::vec3 p = predicted.position, q = snap.owner.position;
        const glm::vec3 pv = predicted.velocity, qv = snap.owner.velocity;
        const float edge = std::min(std::min(q.x - playArea.min.x, playArea.max.x - q.x), std::min(q.z - playArea.min.y, playArea.max.y - q.z));
        const std::string sinceOp = anyTerrainOp ? std::to_string(nextTick - lastTerrainOpTick) + " ticks" : "none";
        Log::Info("Client: correction {} at tick {} ({} ahead): error {:.3f} m; predicted ({:.2f}, {:.2f}, {:.2f}) v ({:.2f}, {:.2f}, {:.2f}) {}; "
                  "server ({:.2f}, {:.2f}, {:.2f}) v ({:.2f}, {:.2f}, {:.2f}) {}; opSeq {} here / {} in the snapshot, last terrain op {} "
                  "ago; {:.1f} m inside the play area{}",
                  prediction.corrections, snap.tick, nextTick - 1 - snap.tick, error, p.x, p.y, p.z, pv.x, pv.y, pv.z,
                  predicted.OnGround() ? "on the ground" : "in the air", q.x, q.y, q.z, qv.x, qv.y, qv.z,
                  snap.owner.OnGround() ? "on the ground" : "in the air", applied, snap.lastOpSeq, sinceOp, edge,
                  prediction.corrections == kLoggedCorrections ? " (later corrections are not logged)" : "");
    }

    void Client::Impl::OnSnapshot(const std::vector<uint8_t>& bytes) {
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
        // Body samples are taken after the snapshot's events, as the owner block is compared
        // (review of stage 6): a stale rest, a model or a spawn never lands on a newer sample.
        if (!snap.bodies.empty()) {
            if (simulation && state == ClientState::Playing) {
                waitingSamples.push_back({snap.tick, snap.lastOpSeq, std::move(snap.bodies)});
                while (waitingSamples.size() > kMaxWaitingSnapshots) {
                    stats.bodySamplesIgnored += waitingSamples.front().bodies.size();
                    waitingSamples.pop_front();
                }
            } else {
                stats.bodySamplesIgnored += snap.bodies.size();
            }
        }
        if (!snap.hasOwner) return;
        if (snap.lastCastTick != lastCast.tick || snap.castRejected != lastCast.rejected) {
            if (snap.castRejected != CastRejection::None) {
                ++prediction.castsRejected;
                Log::Info("Client: the server did not run the cast of tick {}: {}", snap.lastCastTick, ToString(snap.castRejected));
            }
            lastCast = {snap.lastCastTick, snap.castRejected};
        }
        OnOwnerFight(snap);
        lastUseTick = snap.lastUseTick;
        lastUseResult = snap.useResult;
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

    void Client::Impl::TryReconcile() {
        if (waiting.empty() || state != ClientState::Playing || !simulation) return;
        // The start gate (stage 7a, plan amendment 8): the player starts once the chunks within
        // startRadius of it are current and the entity baseline near the spawn is in (resting
        // copies block players; review of stage 7a: BaselineNear, the far ones do not wait). While it is closed every waiting snapshot is dropped, so the fresh-snapshot
        // rule below starts its age bound again when it opens.
        if (!predicting && !(baselineNear && NearCurrent(waiting.back().snap.owner.position))) {
            ++prediction.startGateDrops;
            waiting.clear();
            return;
        }
        size_t ready = waiting.size();
        for (size_t i = waiting.size(); i-- > 0;)
            if (waiting[i].snap.lastOpSeq <= applied) {
                ready = i;
                break;
            }
        // A joiner starts predicting from a snapshot close to the newest one (review of stage 6): one
        // that became ready only after a long, paced catch-up of the join log is that much in the
        // past, and a prediction started there sends inputs that arrive tens of ticks late (a clock
        // jump, then a hard resync). It waits for a fresher one, at most startWaitTicks.
        if (ready < waiting.size() && !predicting && newestSnapshot - waiting[ready].snap.tick > settings.startFreshTicks &&
            waiting.front().age < settings.startWaitTicks) {
            ++prediction.staleStarts;
            waiting.erase(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(ready) + 1);
            ready = waiting.size();
        }
        if (ready < waiting.size()) {
            const WaitingSnapshot w = std::move(waiting[ready]);
            waiting.erase(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(ready) + 1);
            if (w.age > 0) ++prediction.waitedForEvents;
            if (!predicting) {
                StartPrediction(w.snap);
                stats.joinStartTicks = std::max(1u, newestSnapshot - welcome.tick);
                stats.joinStartSeconds = SinceWelcome();
            }
            else {
                Reconcile(w.snap);
                ResolveHold(w.snap);
            }
        }
        // Before the prediction starts a joiner waits for its log as long as it takes.
        while (predicting && !waiting.empty() && waiting.front().age >= settings.eventWaitTicks) {
            ++prediction.eventTimeouts;
            waiting.pop_front();
        }
    }
}
