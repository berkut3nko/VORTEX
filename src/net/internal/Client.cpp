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

        /// A snapshot's body samples waiting for the events the snapshot was built after (review of
        /// stage 6): a sample is taken only once every spawn, model, rest and wake before it is applied.
        struct WaitingSamples {
            uint32_t tick = 0;
            uint32_t lastOpSeq = 0;
            std::vector<BodySample> bodies;
        };

        /// What the events channel carries in order after the Welcome: the stream and the window acks
        /// (an ack is processed in stream order: with every event before it applied).
        /// Stage 7b: the checks and the entity list ride the same channel right after the events up to
        /// their opSeq, so a client processes them exactly at that stream position.
        /// Stage 7b M-b: a terrain-mode window's TerrainAck takes a WindowAck's place.
        using StreamItem = std::variant<EventBatch, WindowAck, HashCheck, EntityCheck, EntityList, TerrainAck>;

        /// Entity events a client holds for a NetId whose copy does not exist yet (stage 7a).
        constexpr size_t kMaxHeldPerId = 64;
        constexpr size_t kMaxHeldEvents = 4096;

        struct HistoryEntry {
            bool valid = false;
            uint32_t tick = 0;
            sim::PlayerInput input;
            sim::PlayerState state;      ///< after the tick
        };
    }

    struct Client::Impl {
        Impl(ClientSettings s, ITransport& t) : settings(s), transport(t), sync(s.ringOps, s.ringBytes), clock(ClockOf(s)) {
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
        sim::PlayArea playArea;             // sim::PlayAreaOf(welcome.region, welcome.player): StepPlayer's area
        sim::Simulation* simulation = nullptr;
        std::deque<StreamItem> pending;   // received before Attach (or not yet applied)
        std::deque<std::vector<uint8_t>> pendingBulk;   // bulk messages not processed yet
        // Chunk sync (stage 7a).
        ChunkSync sync;
        SyncBox syncBox;                  // the window this client asked for, inside the server's
        uint32_t windowSeq = 0;
        bool moving = false;              // the window follows the player (stage 7a M-b)
        uint64_t windowRevision = 0;      // World::WindowRevision of the window last sent
        uint32_t unsyncedLogged = 0;      // ChunkSync unsynced count last logged
        uint32_t syncPolls = 0;
        std::chrono::steady_clock::time_point welcomed;   // the join's wall clock (logged only)
        double SinceWelcome() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - welcomed).count(); }
        bool baselineEnded = false;
        bool baselineNear = false;        // BaselineNear (or BaselineEnd) arrived: the start gate may open
        std::unordered_map<uint32_t, uint32_t> staleBefore;   // NetId -> newest held event dropped (overflow)
        std::map<uint32_t, std::vector<std::pair<uint32_t, Event>>> held;   // NetId -> held entity events
        size_t heldCount = 0;
        std::unordered_set<uint32_t> asked;          // NetIds of ModelRequests out
        std::vector<uint32_t> modelsToAsk;
        uint32_t applied = 0;
        bool anyTerrainOp = false;
        uint32_t lastTerrainOpTick = 0;   // nextTick when the newest terrain operation was applied (LogCorrection)
        uint32_t serverLast = 0;
        ClientStats stats;
        std::unordered_map<uint32_t, scene::Entity> entities;   // NetId -> light entity
        /// A kinematic copy of a replicated server object (stage 6).
        struct Copy {
            scene::Entity entity = scene::kNoEntity;
            bool resting = false;
            uint64_t modelHash = 0;
            uint32_t base = 0;            ///< the opSeq its state is as of (its spawn, Detach, model or baseline)
        };
        std::unordered_map<uint32_t, Copy> copies;               // NetId -> copy
        std::unordered_set<uint32_t> tombstones;                 // despawned NetIds (a later spawn of one is discarded)
        std::deque<uint32_t> tombstoneOrder;                     // ... oldest first (bounded: maxTombstones, stage 7b)
        // The checks (stage 7b).
        uint32_t entityAsOf = 0;          // the newest asOf of entity data processed from bulk (amendment 3)
        bool listOpen = false;            // an EntityListRequest is out, its EntityList not processed
        uint32_t listsInARow = 0;         // lists asked for since the last matching EntityCheck
        uint32_t lastListTick = 0;        // server tick (newest snapshot) of the last request
        bool listToSend = false;
        uint32_t listCheckSeq = 0;        // the check the request is about
        std::unordered_set<glm::ivec3, ChunkCoordHash> pristineFailures;   // distinct chunks this client generated differently
        std::unordered_set<glm::ivec3, ChunkCoordHash> mismatched;         // distinct chunks a HashCheck found different (stage 8: the soak's verdict)
        std::unordered_map<glm::ivec3, uint64_t, ChunkCoordHash> chunkDataSeen;  // coordinate -> FNV-1a of the last ChunkData's RLE (stage 8, O3)
        // Server terrain mode (stage 7b M-b).
        std::shared_ptr<SessionGenerator> sessionGenerator;   // what MakeGenerator hands out (from the Welcome)
        bool terrainMode = false;
        TerrainModeReason terrainReason = TerrainModeReason::None;
        RemoteBodies bodies;
        std::deque<WaitingSamples> waitingSamples;               // snapshot samples whose events are not applied yet
        double bodyTime = 0.0;                                   // server tick the copies are moved to (per tick)
        bool bodyTimeStarted = false;
        std::vector<sim::BlastResult> presentations;
        bool groupOpen = false;
        // The sphere of the newest live cast shown: an object it broke (EntityDespawn{Broken}) bursts there.
        bool castShown = false;
        glm::dvec3 castCenter{0.0};
        float castRadius = 0.0f;
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
        /// Server ticks since the Welcome as far as this client knows (its newest snapshot), at least 1.
        uint32_t TicksSinceWelcome() const { return newestSnapshot > welcome.tick ? newestSnapshot - welcome.tick : 1u; }
        ServerTime serverTime;              // where other players are drawn (only moves forward)
        TickClock clock;
        glm::vec3 offset{0.0f};             // visual error being blended out
        uint32_t blendLeft = 0;
        net::RemotePlayers remotes;
        PredictionStats prediction;
        Client::CastStatus lastCast;
        bool syncHeld = false;              // the reach holds a chunk that is not current: not moved
        bool needResume = false;            // replay the inputs after `lastTaken` once current again
        uint32_t lastTaken = 0;

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
        /// The chunks of the own player's reach are current (stage 7a): a step over a chunk without a
        /// base (its pristine stand-in) would walk where the server's player does not.
        bool ReachCurrent() const {
            const math::AABB box = sim::PlayerReach(player.position, welcome.player);
            const voxel::World& world = simulation->World();
            const glm::ivec3 lo = voxel::VoxelToChunk(world.WorldToVoxel(glm::dvec3(box.min)));
            const glm::ivec3 hi = voxel::VoxelToChunk(world.WorldToVoxel(glm::dvec3(box.max)));
            return sync.AllCurrent(lo, hi);
        }

        /// Every chunk within `startRadius` (3D, metres) of `feet` inside the window is current.
        bool NearCurrent(const glm::vec3& feet) const { return NotCurrentNear(feet, true) == 0; }

        /// Chunks within `startRadius` (3D, metres) of `feet` inside the window that are not current
        /// (with `first`: 0 or 1, it stops at the first).
        uint32_t NotCurrentNear(const glm::vec3& feet, bool first) const {
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

        sim::PlayerStepResult PredictTick(sim::PlayerInput input) {
            input.tick = nextTick;
            FreshColliders();
            const sim::PlayerStepResult r = sim::StepPlayer(simulation->Physics(), simulation->Terrain(), character, player, input,
                                                            welcome.player, playArea, Dt());
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
            LogCorrection(snap, h.state, error);
            const glm::vec3 before = player.position;
            Restore(snap.owner);
            history[t % kHistoryTicks].state = player;
            const auto start = std::chrono::steady_clock::now();
            uint32_t replayed = 0;
            for (uint32_t k = t + 1; k < nextTick; ++k) {
                HistoryEntry& e = history[k % kHistoryTicks];
                FreshColliders();
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

        /// One line per correction, the first kLoggedCorrections of a session (G16's corrections are
        /// read from them: where, how far ahead, on the ground or not, after which terrain operation,
        /// how far from the play area's edge).
        void LogCorrection(const Snapshot& snap, const sim::PlayerState& predicted, float error) {
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
                else Reconcile(w.snap);
            }
            // Before the prediction starts a joiner waits for its log as long as it takes.
            while (predicting && !waiting.empty() && waiting.front().age >= settings.eventWaitTicks) {
                ++prediction.eventTimeouts;
                waiting.pop_front();
            }
        }

        /// The region's generator, wrapped when a test or a broken bot asks for it.
        std::shared_ptr<voxel::IWorldGenerator> Generator() const {
            std::shared_ptr<voxel::IWorldGenerator> g = sim::MakeRegionGenerator(welcome.region, ids);
            return settings.wrapGenerator ? settings.wrapGenerator(std::move(g)) : g;
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
            playArea = sim::PlayAreaOf(welcome.region, welcome.player);
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

        /// WindowUpdate seq + 1 for `window`: cells that entered since the last one (all of them with
        /// `reset`) wait for its ack.
        void SendWindow(const SyncBox& window, bool reset) {
            ++windowSeq;
            sync.BeginWindow(window, windowSeq, reset);
            syncBox = Intersect(window, RegionSyncBox(welcome.region));
            WindowUpdate u;
            u.windowSeq = windowSeq;
            u.flags = reset ? WindowUpdate::kReset : uint8_t{0};
            if (terrainMode) u.flags |= WindowUpdate::kTerrainMode;      // sticky: every update from the switch on
            u.minChunk = window.min;
            u.size = window.size;
            transport.Send(server, Channel::Events, Encode(u));
            ++stats.windowUpdates;
            stats.windowResets += reset ? 1u : 0u;
        }

        /// Server terrain mode (stage 7b): from now on the server's terrain is not generated here
        /// (the SessionGenerator answers Empty inside the server's window) and every WindowUpdate
        /// asks for the whole entering set (TerrainAck); data installs as content. Sticky.
        void StartTerrainMode(TerrainModeReason why) {
            if (terrainMode) return;
            terrainMode = true;
            terrainReason = why;
            if (sessionGenerator) sessionGenerator->SetTerrainMode(true);
            sync.SetTerrainMode(true);
            stats.terrainModeTick = why == TerrainModeReason::PristineChecks ? newestSnapshot : 0u;
        }

        /// The world's window moved since the last WindowUpdate (stage 7a M-b): the next one. Runs at
        /// the start of every Poll, before anything is applied: the world moves only in streaming,
        /// between Polls. Two moves since the last check: a chunk may have left and come back (it was
        /// regenerated), so the update resets the whole window.
        void CheckWindow() {
            if (state != ClientState::Playing || !simulation || server == 0) return;
            voxel::World& world = simulation->World();
            const uint64_t revision = world.WindowRevision();
            const std::vector<glm::ivec3> steps = world.TakeWindowSteps();
            if (revision == windowRevision) return;
            const uint64_t moves = revision - windowRevision;
            windowRevision = revision;
            if (steps.size() == moves) {
                // Every step in order (review of stage 7a: the game streams every frame and polls on
                // ticks, so two steps can fall between two Polls): a chunk that left at one step and
                // came back at the next enters again with the second update, and the server sends it.
                for (const glm::ivec3& min : steps) {
                    const SyncBox window{min, world.WindowSize()};
                    if (window != sync.Window()) SendWindow(window, false);
                }
                return;
            }
            // Steps this client did not see apart (never in play: every Recenter is listed): a chunk
            // may have left and come back regenerated, so the whole window is synced again.
            Log::Info("Client: the window moved {} times between two checks ({} steps listed): every chunk of it is synced again", moves,
                      steps.size());
            SendWindow(SyncBox{world.WindowMinChunk(), world.WindowSize()}, true);
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
                castShown = true;
                castCenter = blast.center;
                castRadius = blast.radius;
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

        // --- replicated objects (stage 6) ---------------------------------------------------------

        physics::BodyId BodyOf(scene::Entity entity) const {
            const scene::RigidBody* body = simulation->Scene().TryGet<scene::RigidBody>(entity);
            return body ? body->body : physics::BodyId{};
        }

        /// The layer of a moving copy: Rubble (no predicted player meets it), unless the own player
        /// stands on it - then it stays Dynamic and carries the player, as the server keeps a moving
        /// body under a player solid (review of stage 6; Server TrackBodies).
        physics::Layer MovingLayer(physics::BodyId body) const {
            if (!predicting || !body.IsValid()) return physics::Layer::Rubble;
            return simulation->Physics().GetCharacterState(character).groundBody == body ? physics::Layer::Dynamic : physics::Layer::Rubble;
        }

        /// A despawned NetId: remembered (a later spawn or answer of it is discarded), at most
        /// maxTombstones of them, oldest forgotten first (stage 7b).
        void Tombstone(uint32_t netId) {
            if (!tombstones.insert(netId).second) return;
            tombstoneOrder.push_back(netId);
            while (tombstoneOrder.size() > std::max<size_t>(1, settings.maxTombstones)) {
                tombstones.erase(tombstoneOrder.front());
                tombstoneOrder.pop_front();
                ++stats.tombstonesEvicted;
            }
        }
        void Untombstone(uint32_t netId) {
            if (tombstones.erase(netId) == 0) return;
            if (const auto it = std::find(tombstoneOrder.begin(), tombstoneOrder.end(), netId); it != tombstoneOrder.end()) tombstoneOrder.erase(it);
        }

        /// A kinematic copy of a server object: Layer::Dynamic at rest (players collide with it),
        /// Layer::Rubble while it moves (no predicted player does).
        bool MakeCopy(uint32_t netId, std::shared_ptr<voxel::VoxelModel> model, const glm::vec3& position, const glm::quat& rotation,
                      bool resting, uint32_t base) {
            if (copies.contains(netId) || tombstones.contains(netId)) {
                ++stats.ignoredEntityEvents;
                return false;
            }
            const uint64_t hash = VolumeHash(model->Volume());
            scene::Scene& scene = simulation->Scene();
            const scene::Entity entity = scene.CreateVoxelObject(
                {.model = std::move(model), .position = position, .rotation = rotation, .motion = physics::MotionType::Kinematic});
            if (entity == scene::kNoEntity) {
                ++stats.copiesFailed;
                Log::Error("Client: no copy of server object {} could be made (store or bodies full)", netId);
                return false;
            }
            simulation->Physics().SetLayer(BodyOf(entity), resting ? physics::Layer::Dynamic : physics::Layer::Rubble);
            copies[netId] = {entity, resting, hash, base};
            ++stats.copiesMade;
            return true;
        }

        /// Local rubble (presentation): an object fragment, or a copy the server let go of.
        void MakeRubble(std::shared_ptr<voxel::VoxelModel> model, const glm::vec3& position, const glm::quat& rotation,
                        const glm::vec3& linear, const glm::vec3& angular) {
            simulation->Scene().CreateVoxelObject({.model = std::move(model),
                                                   .position = position,
                                                   .rotation = rotation,
                                                   .linearVelocity = linear,
                                                   .angularVelocity = angular,
                                                   .debris = true});
        }

        void ApplyEntity(uint32_t seq, const Event& event) {
            const bool live = settings.presentation && seq > welcome.joinOpSeq;
            scene::Scene& scene = simulation->Scene();
            physics::PhysicsWorld& physics = simulation->Physics();
            if (const EntitySpawn* spawn = std::get_if<EntitySpawn>(&event)) {
                if (spawn->kind == EntityKind::Light) {
                    if (!entities.contains(spawn->netId)) {
                        const sim::TemplateLight& l = spawn->light;
                        entities[spawn->netId] =
                            scene.CreateLight({.position = l.position, .color = l.color, .intensity = l.intensity, .radius = l.radius});
                        ++stats.lightsSpawned;
                    }
                    return;
                }
                const ObjectSpawn& o = spawn->object;
                auto model = std::make_shared<voxel::VoxelModel>(o.volume, voxel::kWorldPalette, spawn->netId != 0 ? "object" : "debris");
                if (spawn->netId == 0) {
                    ++stats.fragments;
                    if (live) MakeRubble(std::move(model), o.position, o.rotation, o.linearVelocity, o.angularVelocity);
                    return;
                }
                MakeCopy(spawn->netId, std::move(model), o.position, o.rotation, (o.flags & kObjectResting) != 0, seq);
                return;
            }
            // Rest, wake and model of a NetId: dropped when its copy is already as of this event (the
            // baseline or a model answer was encoded after it), held while its copy may still come.
            if (!std::holds_alternative<EntityDespawn>(event)) {
                const uint32_t netId = std::holds_alternative<EntityRest>(event)   ? std::get<EntityRest>(event).netId
                                       : std::holds_alternative<EntityWake>(event) ? std::get<EntityWake>(event).netId
                                                                                    : std::get<ObjectModel>(event).netId;
                const auto copy = copies.find(netId);
                if (copy != copies.end() && seq <= copy->second.base) {
                    ++stats.staleEntityEvents;
                    return;
                }
                if (copy == copies.end() && !tombstones.contains(netId) && (!baselineEnded || asked.contains(netId))) {
                    Hold(netId, seq, event);
                    return;
                }
            }
            if (const EntityDespawn* d = std::get_if<EntityDespawn>(&event)) {
                Tombstone(d->netId);
                bodies.Remove(d->netId);
                asked.erase(d->netId);
                staleBefore.erase(d->netId);
                if (const auto h = held.find(d->netId); h != held.end()) {
                    stats.droppedHeldEvents += static_cast<uint32_t>(h->second.size());
                    heldCount -= h->second.size();
                    held.erase(h);
                }
                const auto it = copies.find(d->netId);
                if (it == copies.end()) {
                    ++stats.ignoredEntityEvents;
                    return;
                }
                const scene::Entity entity = it->second.entity;
                copies.erase(it);
                ++stats.despawns;
                if (d->reason == scene::DespawnReason::Broken && live && castShown) {
                    // Broken by a blast: what the blast took of it bursts (its fragments, the rest,
                    // came as netId-0 spawns), as Scene::Damage shows offline.
                    const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
                    const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
                    if (voxels && voxels->model && transform)
                        ShowVoxelsInSphere(voxels->model->Volume(), transform->current.position, transform->current.rotation);
                }
                if (d->reason == scene::DespawnReason::ToRubble && live) {
                    // The server let go of it (piece caps): it stays here as this client's rubble.
                    const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
                    const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
                    if (voxels && voxels->model && transform) {
                        const physics::BodyState state = physics.GetState(BodyOf(entity));
                        MakeRubble(voxels->model, transform->current.position, transform->current.rotation, state.linearVelocity,
                                   state.angularVelocity);
                        ++stats.toRubble;
                    }
                }
                scene.Destroy(entity);
                return;
            }
            if (const EntityRest* r = std::get_if<EntityRest>(&event)) {
                const bool rests = bodies.Rest(r->netId, r->tick);
                const auto it = copies.find(r->netId);
                if (it == copies.end()) {
                    ++stats.ignoredEntityEvents;
                    return;
                }
                if (!rests) {
                    // A sample newer than the rest was taken already (it cannot be, as samples wait for
                    // their snapshot's events; kept as a guard): the copy keeps moving.
                    ++stats.staleRests;
                    return;
                }
                Copy& c = it->second;
                c.resting = true;
                const physics::BodyId body = BodyOf(c.entity);
                scene.SetTransform(c.entity, r->position, r->rotation, false);
                physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
                physics.SetLayer(body, physics::Layer::Dynamic);
                ++stats.rests;
                return;
            }
            if (const EntityWake* k = std::get_if<EntityWake>(&event)) {
                const auto it = copies.find(k->netId);
                if (it == copies.end()) {
                    ++stats.ignoredEntityEvents;
                    return;
                }
                // Moving from here on: not solid for a predicted player (unless it carries this
                // one), still where it rested until its samples come (RemoteBodies keeps the rest's
                // tick, so older samples stay refused).
                Copy& c = it->second;
                c.resting = false;
                const physics::BodyId body = BodyOf(c.entity);
                physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
                physics.SetLayer(body, MovingLayer(body));
                ++stats.wakes;
                return;
            }
            const ObjectModel& m = std::get<ObjectModel>(event);
            const auto it = copies.find(m.netId);
            if (it == copies.end()) {
                ++stats.ignoredEntityEvents;
                return;
            }
            // The survivor of a damage: a new model whose origin moved by `offset` voxels of the old
            // one (in its frame). A new copy there; its samples move with it.
            const Copy old = it->second;
            copies.erase(it);
            const scene::Transform* transform = scene.TryGet<scene::Transform>(old.entity);
            const glm::vec3 shift = glm::vec3(m.offset) * simulation->World().VoxelSize();
            glm::vec3 position = transform ? transform->current.position : glm::vec3(0.0f);
            const glm::quat rotation = transform ? transform->current.rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            if (live) {
                // What the damage removed, as particles (Scene::Damage spawns them offline; a client
                // never runs it): the old model's voxels the new one does not have.
                const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(old.entity);
                if (voxels && voxels->model && transform) ShowRemovedVoxels(voxels->model->Volume(), m, position, rotation);
            }
            position += rotation * shift;
            scene.Destroy(old.entity);
            // Only samples taken before this event are kept (later ones wait for it): all in the old
            // frame, so all are shifted.
            bodies.Shift(m.netId, shift);
            // The server's survivor is a new body, awake (Scene::Damage), unless it is static: a
            // resting copy is re-made moving, and the server's EntityWake / EntityRest follow.
            const bool staticModel = m.motion == static_cast<uint8_t>(physics::MotionType::Static);
            MakeCopy(m.netId, std::make_shared<voxel::VoxelModel>(m.volume, voxel::kWorldPalette, "object"), position, rotation,
                     staticModel && old.resting, seq);
            ++stats.models;
        }

        /// Particles for the voxels of `before` (a copy's model at `position` / `rotation`) that the
        /// ObjectModel `after` no longer has; at most kMaxDamageBursts, every stride-th one.
        void ShowRemovedVoxels(const voxel::VoxelVolume& before, const ObjectModel& after, const glm::vec3& position, const glm::quat& rotation) {
            constexpr size_t kMaxDamageBursts = 64;
            const float vs = simulation->World().VoxelSize();
            std::vector<glm::ivec3> removed;
            const glm::ivec3 size = before.Size();
            for (int z = 0; z < size.z; ++z)
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x) {
                        const glm::ivec3 v(x, y, z);
                        if (before.Get(x, y, z) == 0) continue;
                        const glm::ivec3 n = v - after.offset;
                        const glm::ivec3 ns = after.volume.Size();
                        const bool kept = n.x >= 0 && n.y >= 0 && n.z >= 0 && n.x < ns.x && n.y < ns.y && n.z < ns.z && after.volume.Get(n.x, n.y, n.z) != 0;
                        if (!kept) removed.push_back(v);
                    }
            const size_t stride = std::max<size_t>(1, (removed.size() + kMaxDamageBursts - 1) / kMaxDamageBursts);
            for (size_t i = 0; i < removed.size(); i += stride) {
                const glm::ivec3& v = removed[i];
                const glm::vec3 local = (glm::vec3(v) + glm::vec3(0.5f)) * vs;
                const glm::dvec3 at = glm::dvec3(position + rotation * local);
                fx::SpawnVoxelBreak(simulation->Particles(), at, simulation->Materials().Get(voxel::kWorldPalette, before.Get(v.x, v.y, v.z)).color, vs);
            }
        }

        /// Particles for the voxels of `volume` (a copy at `position` / `rotation`) inside the newest
        /// cast's sphere; at most kMaxDamageBursts, every stride-th one.
        void ShowVoxelsInSphere(const voxel::VoxelVolume& volume, const glm::vec3& position, const glm::quat& rotation) {
            constexpr size_t kMaxDamageBursts = 64;
            const float vs = simulation->World().VoxelSize();
            const double reach = static_cast<double>(castRadius) + 0.5 * static_cast<double>(vs);
            std::vector<std::pair<glm::dvec3, uint8_t>> inside;
            const glm::ivec3 size = volume.Size();
            for (int z = 0; z < size.z; ++z)
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x) {
                        const uint8_t id = volume.Get(x, y, z);
                        if (id == 0) continue;
                        const glm::dvec3 at = glm::dvec3(position + rotation * ((glm::vec3(x, y, z) + glm::vec3(0.5f)) * vs));
                        if (glm::length(at - castCenter) <= reach) inside.emplace_back(at, id);
                    }
            const size_t stride = std::max<size_t>(1, (inside.size() + kMaxDamageBursts - 1) / kMaxDamageBursts);
            for (size_t i = 0; i < inside.size(); i += stride)
                fx::SpawnVoxelBreak(simulation->Particles(), inside[i].first, simulation->Materials().Get(voxel::kWorldPalette, inside[i].second).color, vs);
        }

        /// Snapshot samples of the copies. Rest and wake are events: a resting copy takes no sample
        /// (a sample of a woken body waits for its EntityWake).
        void TakeBodySamples(uint32_t tick, const std::vector<BodySample>& samples) {
            for (const BodySample& b : samples) {
                const auto it = copies.find(b.netId);
                if (it == copies.end() || it->second.resting || !bodies.Add(b.netId, tick, BodyPose{b.position, b.rotation, b.velocity})) {
                    ++stats.bodySamplesIgnored;
                    continue;
                }
                ++stats.bodySamples;
            }
        }

        /// The waiting samples whose snapshot's events are applied, oldest first (snapshots are taken
        /// in tick order and their lastOpSeq never falls).
        void TakeReadySamples() {
            if (state != ClientState::Playing || !simulation) return;
            while (!waitingSamples.empty() && waitingSamples.front().lastOpSeq <= applied) {
                TakeBodySamples(waitingSamples.front().tick, waitingSamples.front().bodies);
                waitingSamples.pop_front();
            }
        }

        /// One tick of the copies: each moving one is driven (MoveKinematic) to where its samples put
        /// it at the end of the tick, drawn in the past by the interpolation delay.
        void MoveCopies() {
            const double now = serverTime.Now();
            if (!bodyTimeStarted) {
                bodyTime = now;
                bodyTimeStarted = serverTime.Started();
            } else {
                // One tick per call, pulled gently towards the server time (it advances per frame).
                const double error = now - bodyTime;
                bodyTime += std::abs(error) > 30.0 ? error : 1.0 + std::clamp(error * 0.1, -0.5, 0.5);
            }
            physics::PhysicsWorld& physics = simulation->Physics();
            const float dt = Dt();
            const physics::BodyId ground = predicting ? physics.GetCharacterState(character).groundBody : physics::BodyId{};
            // A copy only the server may remove; if this scene removed one by another path (it fell
            // below the window first, drawn in the past), it is lost here: counted, and the entity
            // check sees it missing until the server's despawn comes.
            for (auto it = copies.begin(); it != copies.end();) {
                if (simulation->Scene().IsValid(it->second.entity)) {
                    ++it;
                    continue;
                }
                ++stats.copiesLost;
                bodies.Remove(it->first);
                it = copies.erase(it);
            }
            for (const auto& [netId, c] : copies) {
                if (c.resting) continue;
                const physics::BodyId body = BodyOf(c.entity);
                // Solid only while it carries the own player (MovingLayer; the server's rule).
                const physics::Layer layer = ground.IsValid() && ground == body ? physics::Layer::Dynamic : physics::Layer::Rubble;
                if (physics.GetLayer(body) != layer) physics.SetLayer(body, layer);
                stats.heldUnderPlayer += layer == physics::Layer::Dynamic ? 1u : 0u;
                BodyPose pose;
                if (bodies.Sample(netId, bodyTime + 1.0, static_cast<double>(dt), pose)) physics.MoveKinematic(body, pose.position, pose.rotation, dt);
                else physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
            }
        }

        void Apply(uint32_t seq, const Event& event) {
            if (const sim::WorldOp* op = std::get_if<sim::WorldOp>(&event)) {
                // Only live events are shown: what came before the join point is history.
                const bool present = settings.presentation && seq > welcome.joinOpSeq;
                // A piece with a NetId is a copy of a server body, made from the voxels it removed.
                const bool replicated = op->kind == sim::WorldOpKind::Detach && op->netId != 0;
                std::vector<voxel::VoxelEdit> edits;
                // The base rule (stage 7a): applied only to the chunks that are current with a base
                // below `seq`; an operation that skipped a chunk it touches is not complete here.
                const ChunkSync::OpOutcome out = sync.Apply(simulation->World(), seq, *op, present || replicated ? &edits : nullptr);
                const sim::OpResult& r = out.result;
                stats.skippedChunks += static_cast<uint32_t>(r.report.skipped.size());
                stats.unsyncedChunks += static_cast<uint32_t>(r.report.unsynced.size());
                ++stats.opsApplied;
                if (replicated && !tombstones.contains(op->netId) && !copies.contains(op->netId)) {
                    if (out.complete) {
                        voxel::VoxelVolume piece = sim::BuildDetachedPiece(*op, edits);
                        if (!piece.IsEmpty())
                            MakeCopy(op->netId, std::make_shared<voxel::VoxelModel>(std::move(piece), voxel::kWorldPalette, "piece"),
                                     glm::vec3(simulation->World().VoxelToWorld(op->min)), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), false, seq);
                    } else if (asked.insert(op->netId).second) {
                        // Some of its voxels are not here as the server had them: never a piece from a
                        // partial volume; the server sends the body (a BaselineEntity on bulk).
                        modelsToAsk.push_back(op->netId);
                    }
                }
                if (present && out.complete) Present(*op, r, std::move(edits));
                else CloseGroup();
                return;
            }
            CloseGroup();
            ApplyEntity(seq, event);
        }

        /// An entity event for a NetId whose copy may still come (the join's baseline is open, or a
        /// model was asked for): kept in order, applied after the copy if newer than its base.
        void Hold(uint32_t netId, uint32_t seq, const Event& event) {
            std::vector<std::pair<uint32_t, Event>>& list = held[netId];
            if (list.size() >= kMaxHeldPerId || heldCount >= kMaxHeldEvents) {
                // Too many: they are dropped and the model is asked for again, also when a request is
                // out already - an answer encoded before this event would miss it (its server dedups
                // a request whose answer is still queued). An answer older than this event is
                // discarded when it comes (review of stage 7a).
                stats.droppedHeldEvents += static_cast<uint32_t>(list.size()) + 1u;
                heldCount -= list.size();
                held.erase(netId);
                uint32_t& newest = staleBefore[netId];
                newest = std::max(newest, seq);
                asked.insert(netId);
                modelsToAsk.push_back(netId);
                return;
            }
            list.emplace_back(seq, event);
            ++heldCount;
            ++stats.heldEntityEvents;
        }

        /// A BaselineEntity: the join's baseline, or the answer to a ModelRequest.
        void OnBaselineEntity(const BaselineEntity& b) {
            ++stats.baselineEntities;
            const uint32_t netId = b.spawn.netId;
            if (const auto stale = staleBefore.find(netId); stale != staleBefore.end()) {
                if (b.asOfOpSeq < stale->second && !tombstones.contains(netId)) {
                    // Made before events of it this client dropped: the re-asked answer follows.
                    ++stats.staleAnswers;
                    return;
                }
                staleBefore.erase(stale);
            }
            entityAsOf = std::max(entityAsOf, b.asOfOpSeq);
            asked.erase(netId);
            if (b.spawn.kind == EntityKind::Light) {
                if (!entities.contains(netId) && !tombstones.contains(netId)) {
                    const sim::TemplateLight& l = b.spawn.light;
                    entities[netId] = simulation->Scene().CreateLight({.position = l.position, .color = l.color, .intensity = l.intensity, .radius = l.radius});
                    ++stats.lightsSpawned;
                } else {
                    ++stats.baselineDiscarded;
                }
                return;
            }
            const ObjectSpawn& o = b.spawn.object;
            const bool resting = b.restTick != BaselineEntity::kAwake;
            if (!MakeCopy(netId, std::make_shared<voxel::VoxelModel>(o.volume, voxel::kWorldPalette, "object"), o.position, o.rotation, resting,
                          b.asOfOpSeq)) {
                ++stats.baselineDiscarded;
            } else if (resting) {
                bodies.Rest(netId, b.restTick);      // samples of its rest's tick or older stay refused
            } else {
                bodies.Add(netId, b.tick, BodyPose{o.position, o.rotation, o.linearVelocity});
            }
            // The events held for it: newer than the copy's state applied, the others dropped.
            const auto h = held.find(netId);
            if (h == held.end()) return;
            std::vector<std::pair<uint32_t, Event>> list = std::move(h->second);
            heldCount -= list.size();
            held.erase(h);
            for (const auto& [seq, event] : list) {
                if (copies.contains(netId) && seq > copies[netId].base) ApplyEntity(seq, event);
                else ++stats.droppedHeldEvents;
            }
        }

        /// Bulk messages (ChunkData, BaselineEntity, BaselineEnd), once attached.
        void ProcessBulk() {
            if (state != ClientState::Playing || !simulation) return;
            const auto start = std::chrono::steady_clock::now();
            uint32_t installs = 0;
            // Consecutive ChunkData are installed together: their chunks catch up with one walk of
            // the ring (recheck of stage 7a, 3). Flushed before any other message keeps its place.
            std::vector<ChunkData> chunks;
            const auto flush = [&] {
                if (chunks.empty()) return;
                sync.OnChunkData(simulation->World(), chunks, applied);
                chunks.clear();
            };
            while (!pendingBulk.empty() && state == ClientState::Playing) {
                const std::optional<MessageType> next = PeekType(pendingBulk.front());
                const bool install = next == MessageType::ChunkData || next == MessageType::BaselineEntity;
                // A budget per Poll, in arrival order (the server sends nearest first; the markers
                // keep their place after the entities before them).
                if (install && settings.maxBulkPerPoll > 0 && installs >= settings.maxBulkPerPoll) break;
                installs += install ? 1u : 0u;
                const std::vector<uint8_t> bytes = std::move(pendingBulk.front());
                pendingBulk.pop_front();
                const std::optional<MessageType> type = PeekType(bytes);
                if (type != MessageType::ChunkData) flush();
                if (type == MessageType::BaselineNear) {
                    BaselineNear n;
                    if (!Decode(bytes, n)) return Fail("malformed BaselineNear");
                    baselineNear = true;
                    entityAsOf = std::max(entityAsOf, n.asOfOpSeq);
                    if (stats.baselineNearTicks == 0) stats.baselineNearTicks = TicksSinceWelcome();
                } else if (type == MessageType::ChunkData) {
                    ChunkData d;
                    if (!Decode(bytes, d)) return Fail("malformed ChunkData");
                    ++stats.chunkData;
                    {
                        // Stage 8 (O3): sent before, and sent again unchanged (the same RLE bytes).
                        uint64_t h = 0xcbf29ce484222325ull;
                        for (const uint8_t b : d.rle) h = (h ^ b) * 0x100000001b3ull;
                        const auto [seen, fresh] = chunkDataSeen.try_emplace(d.chunk, h);
                        if (!fresh) {
                            ++stats.chunkDataRepeats;
                            stats.chunkDataRepeatBytes += bytes.size();
                            if (seen->second == h) {
                                ++stats.chunkDataUnchanged;
                                stats.chunkDataUnchangedBytes += bytes.size();
                            }
                            seen->second = h;
                        }
                    }
                    if (terrainMode) {
                        ++stats.terrainChunkData;
                        stats.terrainChunkBytes += bytes.size();
                    }
                    chunks.push_back(std::move(d));
                } else if (type == MessageType::BaselineEntity) {
                    BaselineEntity b;
                    if (!Decode(bytes, b)) return Fail("malformed BaselineEntity");
                    OnBaselineEntity(b);
                } else if (type == MessageType::BaselineEnd) {
                    BaselineEnd e;
                    if (!Decode(bytes, e)) return Fail("malformed BaselineEnd");
                    baselineEnded = true;
                    entityAsOf = std::max(entityAsOf, e.asOfOpSeq);
                    baselineNear = true;
                    const uint32_t ticks = TicksSinceWelcome();
                    if (stats.baselineNearTicks == 0) stats.baselineNearTicks = ticks;
                    if (stats.baselineEndTicks == 0) stats.baselineEndTicks = ticks;
                    // Held events of NetIds that no baseline holds and nobody asked for: their copies
                    // will not come (they died, or were never replicated to this client).
                    for (auto it = held.begin(); it != held.end();) {
                        if (asked.contains(it->first)) {
                            ++it;
                            continue;
                        }
                        stats.droppedHeldEvents += static_cast<uint32_t>(it->second.size());
                        heldCount -= it->second.size();
                        it = held.erase(it);
                    }
                } else {
                    return Fail("an unknown message on the bulk channel");
                }
            }
            flush();
            if (installs > 0) {
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                stats.bulkSeconds += seconds;
                stats.maxBulkSeconds = std::max(stats.maxBulkSeconds, seconds);
                stats.maxBulkInPoll = std::max(stats.maxBulkInPoll, installs);
            }
        }

        /// Load reports, then the requests owed: chunks (the ring no longer covered them, or a slot
        /// freed for an Unsynced one) and models.
        void SyncHousekeeping() {
            if (state != ClientState::Playing || !simulation || server == 0) return;
            voxel::World& world = simulation->World();
            const std::vector<glm::ivec3> loaded = world.TakeLoadedChunks();
            if (!loaded.empty()) sync.OnLoaded(world, loaded, applied);
            const uint32_t capacity = simulation->Chunks().Capacity(), used = simulation->Chunks().UsedCount();
            stats.maxStoreUsed = std::max(stats.maxStoreUsed, used);
            // A full store (networking.md "Chunk sync"): such chunks are Unsynced - not trusted, not
            // hashed, not walkable - until slots free and they are requested whole. Logged as it grows.
            if (const uint32_t unsynced = sync.Stats().unsynced; unsynced != unsyncedLogged) {
                Log::Warn("Client: {} chunk(s) found no store slot ({} of {} slots used): unsynced until slots free ({} in all)",
                          unsynced - unsyncedLogged, used, capacity, unsynced);
                unsyncedLogged = unsynced;
            }
            const glm::ivec3 near = voxel::VoxelToChunk(world.WorldToVoxel(glm::dvec3(player.position)));
            const std::vector<glm::ivec3> chunks =
                sync.TakeRequests(capacity - std::min(used, capacity), settings.requestReserveSlots, near, kMaxChunkRequest);
            if (!chunks.empty()) {
                stats.chunkRequests += static_cast<uint32_t>(chunks.size());
                transport.Send(server, Channel::Events, Encode(ChunkRequest{chunks}));
            }
            // Check entries kept while their chunks loaded (review of stage 7b), then the HashCheck
            // mismatches (stage 7b): the chunks again, with reason Repair (the server counts them).
            CheckDeferred();
            if (state != ClientState::Playing) return;
            if (const std::vector<glm::ivec3> repairs = sync.TakeRepairs(kMaxChunkRequest); !repairs.empty()) {
                stats.chunkRequests += static_cast<uint32_t>(repairs.size());
                stats.repairsRequested += static_cast<uint32_t>(repairs.size());
                transport.Send(server, Channel::Events, Encode(ChunkRequest{repairs, ChunkRequestReason::Repair}));
            }
            if (listToSend) {
                listToSend = false;
                ++stats.entityListRequests;
                transport.Send(server, Channel::Events, Encode(EntityListRequest{listCheckSeq}));
            }
            for (size_t i = 0; i < modelsToAsk.size(); i += kMaxModelRequest) {
                ModelRequest m;
                m.netIds.assign(modelsToAsk.begin() + static_cast<std::ptrdiff_t>(i),
                                modelsToAsk.begin() + static_cast<std::ptrdiff_t>(std::min(modelsToAsk.size(), i + kMaxModelRequest)));
                stats.modelRequests += static_cast<uint32_t>(m.netIds.size());
                transport.Send(server, Channel::Events, Encode(m));
            }
            modelsToAsk.clear();
            // Synced() walks the window's records (101 376 for the game's window): looked at every 16th
            // Poll only until the join has synced once.
            if (stats.joinSyncTicks == 0 && ++syncPolls % 16 == 0) {
                if (stats.joinDataTicks == 0 && baselineEnded) {
                    const ChunkSyncCounts n = sync.Counts();
                    if (n.noBase == 0 && n.awaiting == 0 && n.requested == 0 && n.unsynced == 0) {
                        stats.joinDataTicks = std::max(1u, newestSnapshot - welcome.tick);
                        stats.joinDataSeconds = SinceWelcome();
                    }
                }
                if (Synced()) NoteSynced();
            }
        }

        /// The join's "synced after" (logged only): the first time Synced() was seen true - every 16th
        /// Poll, or when a caller asked (Client::Synced: a client that syncs during a drain's last
        /// Polls would otherwise log 0 s).
        void NoteSynced() {
            if (stats.joinSyncTicks != 0) return;
            stats.joinSyncTicks = std::max(1u, newestSnapshot - welcome.tick);
            stats.joinSyncSeconds = SinceWelcome();
            if (stats.joinDataTicks == 0) {
                stats.joinDataTicks = stats.joinSyncTicks;
                stats.joinDataSeconds = stats.joinSyncSeconds;
            }
        }

        bool Synced() const {
            return state == ClientState::Playing && simulation && baselineEnded && asked.empty() && held.empty() && modelsToAsk.empty() &&
                   !listOpen && sync.Synced();
        }

        // --- the checks (stage 7b) ---------------------------------------------------------------

        void AddCheckTime(std::chrono::steady_clock::time_point start) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            stats.checkSeconds += seconds;
            stats.maxCheckSeconds = std::max(stats.maxCheckSeconds, seconds);
        }

        /// A HashCheck at its stream position: chunks that differ become Requested (reason Repair).
        void OnHashCheck(const HashCheck& check) {
            const auto start = std::chrono::steady_clock::now();
            const HashCheckOutcome o = sync.OnHashCheck(simulation->World(), check, applied);
            if (!o.compared) {
                ++stats.checksOutOfOrder;
                Log::Warn("Client: a HashCheck at opSeq {} came at opSeq {}: ignored", check.asOfOpSeq, applied);
                return;
            }
            ++stats.hashChecks;
            OnMismatches(o, check.asOfOpSeq);
            AddCheckTime(start);
        }

        /// Entries of earlier HashChecks whose chunks were still loading here, compared once the chunks
        /// are current (review of stage 7b; ChunkSync::CheckDeferred).
        void CheckDeferred() {
            if (!sync.HasDeferredChecks()) return;
            const auto start = std::chrono::steady_clock::now();
            const uint32_t before = sync.Stats().deferredEqual + sync.Stats().deferredMismatches;
            const HashCheckOutcome o = sync.CheckDeferred(simulation->World(), applied);
            OnMismatches(o, applied);
            if (sync.Stats().deferredEqual + sync.Stats().deferredMismatches != before) AddCheckTime(start);
        }

        /// What a HashCheck (or a deferred entry) found different: counted, logged; chunks asked for again
        /// (by ChunkSync) unless a repeat was refused.
        void OnMismatches(const HashCheckOutcome& o, uint32_t opSeq) {
            for (const HashCheckOutcome::Mismatch& m : o.mismatches) {
                mismatched.insert(m.chunk);
                if (m.pristine) ++stats.mismatchesPristine;
                else ++stats.mismatchesModified;
                // A pristine chunk this client's generator made differently: the generator is suspect
                // (3 distinct ones start server terrain mode, M-b). Re-checks count once.
                if (m.pristine && m.generated) {
                    pristineFailures.insert(m.chunk);
                    stats.pristineFailures = static_cast<uint32_t>(pristineFailures.size());
                }
                if (m.refused) {
                    Log::Error("Client: chunk ({}, {}, {}) differs from the server's again at opSeq {} soon after its repair ({}): not asked "
                               "for again - a bug to look at (its later differences are only counted)",
                               m.chunk.x, m.chunk.y, m.chunk.z, opSeq, m.pristine ? "pristine" : "modified");
                    continue;
                }
                Log::Warn("Client: chunk ({}, {}, {}) differs from the server's at opSeq {} ({}{}{}{}): asked for again", m.chunk.x, m.chunk.y,
                          m.chunk.z, opSeq, m.pristine ? "pristine" : "modified", m.generated ? ", generated here" : ", from data",
                          m.repeat ? ", repaired before" : "", m.deferred ? ", compared once it had loaded" : "");
            }
            // Too many pristine chunks made differently here: this generator is not trusted any more
            // (stage 7b M-b). The whole window again, as the server holds it: a reset, with the flag.
            if (!terrainMode && settings.pristineFailuresForTerrainMode > 0 && pristineFailures.size() >= settings.pristineFailuresForTerrainMode) {
                StartTerrainMode(TerrainModeReason::PristineChecks);
                Log::Warn("Client: {} pristine chunks differed from the server's (tick {}): server terrain mode - the whole window is "
                          "asked for again",
                          pristineFailures.size(), stats.terrainModeTick);
                SendWindow(sync.Window(), true);
            }
        }

        /// This client's copies as an EntityCheck counts them; NetIds asked for count as present.
        EntityCheck OwnEntityCheck(bool withAsked) const {
            EntityCheck c;
            for (const auto& [netId, copy] : copies) {
                ++c.count;
                c.idXor ^= netId;
                c.modelXor ^= EntityModelTerm(netId, copy.modelHash);
            }
            if (withAsked)
                for (const uint32_t netId : asked)
                    if (!copies.contains(netId)) {
                        ++c.count;
                        c.idXor ^= netId;
                    }
            return c;
        }

        /// An EntityCheck at its stream position (amendments 3 and 8): compared when the baseline has
        /// ended, no list is out and no entity data ahead of it was processed; while models are asked
        /// for only the count and ids (the asked ones counted as present). A mismatch asks for the list.
        void OnEntityCheck(const EntityCheck& check) {
            const auto start = std::chrono::steady_clock::now();
            if (applied != check.asOfOpSeq) {
                ++stats.checksOutOfOrder;
                Log::Warn("Client: an EntityCheck at opSeq {} came at opSeq {}: ignored", check.asOfOpSeq, applied);
                return;
            }
            if (!baselineEnded || listOpen) {
                ++stats.entityChecksSkipped;
                return;
            }
            if (check.asOfOpSeq < entityAsOf) {
                ++stats.entityChecksAhead;
                return;
            }
            const bool partial = !asked.empty() || !held.empty() || !modelsToAsk.empty();
            const EntityCheck own = OwnEntityCheck(partial);
            ++(partial ? stats.entityChecksPartial : stats.entityChecks);
            const bool match = own.count == check.count && own.idXor == check.idXor && (partial || own.modelXor == check.modelXor);
            AddCheckTime(start);
            if (match) {
                listsInARow = 0;
                return;
            }
            ++stats.entityMismatches;
            // Back-off: a client that cannot make a copy (bodies full) does not ask every 2 s.
            const uint32_t now = newestSnapshot;
            if (listsInARow >= settings.entityListsBeforeBackoff) {
                const uint32_t wait = settings.entityListBackoffTicks * (listsInARow == settings.entityListsBeforeBackoff ? 1u : 3u);
                if (now - lastListTick < wait) {
                    ++stats.entityListsBackedOff;
                    return;
                }
            }
            Log::Warn("Client: the replicated objects differ from the server's at opSeq {} ({} here{}, {} there): asking for the list",
                      check.asOfOpSeq, own.count, partial ? " with models asked for" : "", check.count);
            ++listsInARow;
            lastListTick = now;
            listOpen = true;
            listToSend = true;
            listCheckSeq = check.asOfOpSeq;
        }

        /// A NetId whose body this client lacks: asked for (7a's ModelRequest; its events are held).
        void AskModel(uint32_t netId) {
            if (asked.insert(netId).second) modelsToAsk.push_back(netId);
        }

        /// Removes a copy (no tombstone; its samples go too).
        void RemoveCopy(uint32_t netId) {
            const auto it = copies.find(netId);
            if (it == copies.end()) return;
            simulation->Scene().Destroy(it->second.entity);
            bodies.Remove(netId);
            copies.erase(it);
        }

        /// The EntityList at its stream position Q (amendment 2): copies the list does not have and
        /// that are as old as it are dropped; listed NetIds without a copy, or whose copy has another
        /// model, are asked for (their bodies come as answers, with the exact rest state); copies newer
        /// than the list are left alone.
        void OnEntityList(const EntityList& list) {
            const auto start = std::chrono::steady_clock::now();
            listOpen = false;
            if (applied != list.asOfOpSeq) {
                ++stats.checksOutOfOrder;
                Log::Warn("Client: an EntityList at opSeq {} came at opSeq {}: ignored", list.asOfOpSeq, applied);
                return;
            }
            ++stats.entityLists;
            std::unordered_map<uint32_t, uint64_t> listed;
            listed.reserve(list.entries.size());
            for (const EntityListEntry& e : list.entries) listed.emplace(e.netId, e.modelHash);
            std::vector<uint32_t> drop;
            for (const auto& [netId, copy] : copies)
                if (!listed.contains(netId) && copy.base <= list.asOfOpSeq) drop.push_back(netId);
            std::sort(drop.begin(), drop.end());
            for (const uint32_t netId : drop) {
                RemoveCopy(netId);
                Tombstone(netId);
                ++stats.copiesDropped;
            }
            // A NetId asked for that the list does not have died at or before Q (every ask came from an
            // event at or below Q, and a despawn processed since would have cleared it): no answer
            // will come (the server answers nothing for a gone NetId), so it is forgotten - else
            // Synced() would wait for it for ever (review of 7b M-a).
            std::vector<uint32_t> gone;
            for (const uint32_t netId : asked)
                if (!listed.contains(netId) && !copies.contains(netId)) gone.push_back(netId);
            std::sort(gone.begin(), gone.end());
            for (const uint32_t netId : gone) {
                asked.erase(netId);
                std::erase(modelsToAsk, netId);
                if (const auto h = held.find(netId); h != held.end()) {
                    heldCount -= h->second.size();
                    stats.droppedHeldEvents += static_cast<uint32_t>(h->second.size());
                    held.erase(h);
                }
                Tombstone(netId);
                ++stats.copiesDropped;
            }
            for (const EntityListEntry& e : list.entries) {
                if (const auto it = copies.find(e.netId); it != copies.end()) {
                    if (it->second.base > list.asOfOpSeq || it->second.modelHash == e.modelHash) continue;
                    RemoveCopy(e.netId);
                    AskModel(e.netId);
                    ++stats.copiesReplaced;
                    continue;
                }
                if (asked.contains(e.netId)) continue;      // its answer is on the way
                Untombstone(e.netId);                       // alive at Q on the server: the list wins
                AskModel(e.netId);
                ++stats.copiesRestored;
            }
            Log::Info("Client: entity list at opSeq {}: {} object(s); {} dropped, {} asked for again", list.asOfOpSeq, list.entries.size(),
                      drop.size(), modelsToAsk.size());
            AddCheckTime(start);
        }

        void ApplyPending() {
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
    };

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
        if (!s.predicting || s.state != ClientState::Playing || !s.simulation) return {};
        sim::PlayerStepResult r;
        s.syncHeld = !s.ReachCurrent();
        if (s.syncHeld) {
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
                    s.FreshColliders();
                    sim::StepPlayer(s.simulation->Physics(), s.simulation->Terrain(), s.character, s.player, e.input, s.welcome.player, s.playArea, s.Dt());
                    e.state = s.player;
                }
            }
            r = s.PredictTick(controls);
        }
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
