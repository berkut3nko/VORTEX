module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module vortex.net;

import vortex.core;
import vortex.voxel;
import vortex.sim;

namespace vortex::net {

    // --- Bot -------------------------------------------------------------------------------------------

    Bot::Bot(BotSettings settings, ITransport& link) : m_Settings(std::move(settings)), m_Link(link) {
        ClientSettings cs = m_Settings.client;
        if (cs.windowChunks > 0 && cs.minFreeStoreSlots == 0) cs.minFreeStoreSlots = kGameStoreHeadroom;
        // The injected faults, when the owner did not wrap the generator itself (vortex_bot): the region
        // is the Welcome's (the wrap is first called when the Welcome is taken), the ids the defaults.
        if (!cs.wrapGenerator && (m_Settings.hasBrokenChunk || m_Settings.brokenGenerator)) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            if (m_Settings.brokenGenerator) {
                cs.wrapGenerator = [this, ids](std::shared_ptr<voxel::IWorldGenerator>) {
                    sim::RegionDesc other = m_Client->Region();
                    other.terrain.seed += 1;
                    other.flatGroundVoxels += 12;
                    return sim::MakeRegionGenerator(other, ids);
                };
            } else {
                const glm::ivec3 chunk = m_Settings.brokenChunk;
                cs.wrapGenerator = [chunk, ids](std::shared_ptr<voxel::IWorldGenerator> g) { return BreakChunks(std::move(g), {chunk}, ids.stone); };
            }
        }
        m_Settings.client = cs;
        m_Client = std::make_unique<Client>(cs, link);
    }

    Bot::~Bot() = default;

    namespace {
        std::string MaterialName(const voxel::WorldMaterials& m, uint8_t id) {
            if (id == 0) return "air";
            const std::pair<uint8_t, const char*> names[] = {
                {m.grass, "grass"},   {m.grassDry, "dry grass"}, {m.dirt, "dirt"},   {m.stone, "stone"},     {m.stoneDark, "dark stone"},
                {m.sand, "sand"},     {m.snow, "snow"},          {m.water, "water"}, {m.wood, "wood"},       {m.leaves, "leaves"},
                {m.leavesAutumn, "autumn leaves"}, {m.planks, "planks"},          {m.brick, "brick"}, {m.iron, "iron"},
                {m.gold, "gold"},     {m.crystal, "crystal"},    {m.lava, "lava"},   {m.torch, "torch"},     {m.glass, "glass"},
                {m.bedrock, "bedrock"}};
            for (const auto& [value, name] : names)
                if (value == id) return name;
            return std::format("material {}", id);
        }

        /// What is at a standing walker's capsule now (Bot::LongestStillContext): cheap, once per stretch.
        /// Held: the predicted state carries sim::PlayerFlag_Held (StepPlayer's gate found no terrain
        /// collider under the capsule and did not move it), with the stretch's held ticks so far.
        std::string StillContextOf(const Client& c, const sim::Simulation& simulation, const BotPilot::StillStretch& stretch) {
            const sim::PlayerState& p = c.Player();
            const bool held = (p.flags & sim::PlayerFlag_Held) != 0;
            const voxel::World& w = simulation.World();
            const double half = 0.5 * static_cast<double>(w.VoxelSize());
            const glm::dvec3 feet(p.position);
            const uint8_t under = w.GetVoxel(w.WorldToVoxel(feet - glm::dvec3(0.0, half, 0.0)));
            const uint8_t head = w.GetVoxel(w.WorldToVoxel(feet + glm::dvec3(0.0, static_cast<double>(c.Tuning().height) + half, 0.0)));
            const EntityInfo* nearest = nullptr;
            float best = 0.0f;
            const std::vector<EntityInfo> entities = c.Entities();
            for (const EntityInfo& e : entities) {
                const float d = glm::length(e.position - p.position);
                if (nearest == nullptr || d < best) {
                    nearest = &e;
                    best = d;
                }
            }
            const voxel::WorldMaterials& ids = simulation.WorldMaterialIds();
            return std::format("{} ({} of the stretch's {} ticks so far held), {}, under the feet {}, above the head {}, nearest replicated object {}",
                               held ? "held: no terrain collider under the capsule yet (StepPlayer's gate)" : "not held", stretch.heldTicks,
                               stretch.ticks, p.OnGround() ? "on the ground" : "not on the ground", MaterialName(ids, under), MaterialName(ids, head),
                               nearest == nullptr ? std::string("none")
                                                  : std::format("{} at {:.1f} m from the feet (its origin; {})", nearest->netId, best,
                                                                nearest->resting ? "resting" : "awake"));
        }
    }

    void Bot::NoteStill() {
        // A walker's still stretch (BotPilot::Still: within 1 m): what is around it 1 s in, kept for the
        // longest stretch, and a line once one lasts 10 s (its time stamp says when; the bound is 20 s).
        // Its confined stretch (within 4 m): 2 s in (running, it leaves 4 m in 0.6 s), a line at 20 s
        // (the bound is 45 s).
        NoteStretch(m_Still, m_Pilot->Still(), m_Pilot->LongestStill(), 1.0, 10.0, "still");
        NoteStretch(m_Confined, m_Pilot->Confined(), m_Pilot->LongestConfined(), 2.0, 20.0, "within 4 m");
    }

    void Bot::NoteStretch(StretchNote& note, const BotPilot::StillStretch& now, const BotPilot::StillStretch& longest, double contextSeconds,
                          double logSeconds, const char* what) {
        const double tick = static_cast<double>(m_Simulation->FixedTimeStep());
        const uint32_t contextAt = static_cast<uint32_t>(std::lround(contextSeconds / tick));
        if (now.ticks == contextAt) {
            note.context = StillContextOf(*m_Client, *m_Simulation, now);
            note.start = now.startTick;
        }
        if (longest.startTick == note.start && longest.ticks >= contextAt && note.longestStart != note.start) {
            note.longestContext = note.context;
            note.longestStart = note.start;
        }
        if (now.ticks == static_cast<uint32_t>(std::lround(logSeconds / tick)))
            Log::Info("Client {} walk: {} for {:.0f} s at ({:.1f}, {:.1f}, {:.1f}) since pilot tick {}, {} ticks held; {:.0f} s in: {}",
                      m_Settings.id, what, logSeconds, now.at.x, now.at.y, now.at.z, now.startTick, now.heldTicks, contextSeconds,
                      note.start == now.startTick ? note.context : std::string("-"));
    }

    bool Bot::WaitingForRepair() const { return m_Settings.hasBrokenChunk && !m_Repaired; }

    void Bot::BuildAndAttach(const std::function<void()>& whileLoading) {
        Client& c = *m_Client;
        const bool window = m_Settings.client.windowChunks > 0;
        sim::SimulationConfig base;
        base.jobThreads = m_Settings.jobThreads > 0 ? m_Settings.jobThreads : window ? 2u : 1u;
        base.physics.threads = 1;
        if (m_Settings.chunkCapacity != 0) base.chunkCapacity = m_Settings.chunkCapacity;   // else the region's (MakeSimulationConfig)
        // A window: the game's world (its packing, its store and headroom; stage 7a plan amendment 6).
        if (window) {
            base.world.packHidden = m_Settings.packHidden;
            base.chunkCapacity = kGameStoreSlots;
        }
        // The pilot, from the session's region.
        PilotSettings ps = m_Settings.pilot;
        if (m_Settings.walkAcross) {
            // The middle z, from the -x edge to the +x edge and back to the middle (metres).
            const sim::RegionDesc& r = c.Region();
            const float chunk = static_cast<float>(voxel::kChunkSize) * r.terrain.voxelSize;
            const float x0 = static_cast<float>(r.playMinChunk.x) * chunk + 4.0f, x1 = static_cast<float>(r.playMaxChunk.x) * chunk - 4.0f;
            const float z = 0.5f * static_cast<float>(r.playMinChunk.y + r.playMaxChunk.y) * chunk;
            ps.waypoints = {{x0, z}, {x1, z}, {0.5f * (x0 + x1), z}};
            ps.casts = false;
        }
        m_Pilot = std::make_unique<BotPilot>(c.Region(), m_Settings.seed != 0 ? m_Settings.seed : 0xb07u + m_Settings.id, ps);
        m_Simulation = std::make_unique<sim::Simulation>(c.MakeSimulationConfig(base, window ? kGameStoreSlots : 0));
        m_Loading = true;
        const auto t0 = std::chrono::steady_clock::now();
        const bool initialized = m_Simulation->Initialize(c.MakeGenerator());
        const auto t1 = std::chrono::steady_clock::now();
        const bool attached = initialized && c.Attach(*m_Simulation, [this, &whileLoading] {
                                  m_Client->Poll();
                                  m_Link.Flush();
                                  if (whileLoading) whileLoading();
                              });
        m_Loading = false;
        m_InitSeconds = std::chrono::duration<double>(t1 - t0).count();
        m_AttachSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        Log::Info("Client {}: simulation initialised in {:.3f} s, region loaded and attached in {:.3f} s", m_Settings.id, m_InitSeconds,
                  m_AttachSeconds);
        if (!attached) {
            Log::Error("Client: could not attach a simulation");
            m_Simulation.reset();
            m_AttachFailed = true;
        }
    }

    void Bot::Pass(double seconds, bool acting, const std::function<void()>& whileLoading) {
        Client& c = *m_Client;
        if (c.State() == ClientState::Welcomed && !m_Simulation && !m_AttachFailed) BuildAndAttach(whileLoading);
        if (m_Settings.hasBrokenChunk && !m_Repaired && c.State() == ClientState::Playing && c.Sync().Stats().repairsInstalled > 0 &&
            c.Sync().FromData(m_Settings.brokenChunk))
            m_Repaired = true;
        // The client's own clock (kept ahead of the server) says how many ticks it runs now.
        const uint32_t due = c.TicksDue(seconds);
        for (uint32_t k = 0; k < due; ++k) {
            const bool wanted = acting && !m_Settings.stand && !WaitingForRepair() && m_Pilot;
            const bool act = wanted && c.Predicting();
            m_Predicted = m_Predicted || c.Predicting();
            m_WantedTicks += wanted && m_Predicted ? 1u : 0u;
            m_ActedTicks += act ? 1u : 0u;
            const sim::PlayerInput controls = act ? m_Pilot->Next(c.Player()) : sim::PlayerInput{};
            if (act && m_Settings.walkAcross && m_Simulation) NoteStill();
            const sim::PlayerStepResult r = c.RunTick(controls);
            m_Casts += r.cast ? 1u : 0u;
            if (c.Predicting()) {
                m_MinX = std::min(m_MinX, c.Player().position.x);
                m_MaxX = std::max(m_MaxX, c.Player().position.x);
            }
            if (m_Simulation)
                for (const sim::BlastResult& blast : c.TakePresentations()) sim::PresentBlast(*m_Simulation, blast);
        }
        m_Link.Flush();
    }

    double Bot::PassOnClock(const std::function<double()>& clock, bool acting, const std::function<void()>& whileLoading) {
        const double now = clock();
        const bool hadSimulation = m_Simulation != nullptr;
        Pass(m_LastPassAt < 0.0 ? 0.0 : now - m_LastPassAt, acting, whileLoading);
        const bool loaded = !hadSimulation && m_Simulation != nullptr;
        // From this pass's start (review of stage 8: vortex_bot took the time after the pass, so the work
        // inside every pass was lost: the clock ran slow and jumped forward 1-4 times a second); after the
        // pass that loaded the region, from its end (its load is not owed as catch-up ticks).
        m_LastPassAt = loaded ? clock() : now;
        return now;
    }

    void Bot::RecordApplied(double now) {
        const Client& c = *m_Client;
        if (!m_Simulation || c.State() != ClientState::Playing) return;
        if (!m_Counting) {
            m_Counting = true;
            m_Seen = c.JoinOpSeq();
            m_PlayingSince = now;
        }
        for (; m_Seen < c.AppliedOpSeq(); ++m_Seen) m_Applied.emplace_back(m_Seen + 1, now);
    }

    // --- DrainWatch ------------------------------------------------------------------------------------

    bool DrainWatch::Update(const Client& client, double now) {
        const uint32_t server = client.ServerOpSeq();
        // Every copy rests, or stands within 1 mm of where it was when the hold began (a frozen server
        // body: see the class comment); a copy that moved or is new since then restarts the hold.
        const std::vector<EntityInfo> entities = client.Entities();
        bool still = true;
        m_NotResting = 0;
        m_Moved = 0;
        for (const EntityInfo& e : entities) {
            if (e.resting) continue;
            ++m_NotResting;
            const auto it = m_Still.find(e.netId);
            if (it == m_Still.end() || glm::any(glm::greaterThan(glm::abs(it->second - e.position), glm::vec3(0.001f)))) {
                still = false;
                ++m_Moved;
            }
        }
        m_Applied = client.AppliedOpSeq();
        m_Server = server;
        m_Synced = client.Synced();
        m_Playing = client.State() == ClientState::Playing;
        const bool holds = m_Playing && m_Applied == server && still && m_Synced;
        if (!holds || server != m_Seen || !m_CaughtUp) {
            // The hold (re)starts here: the positions it measures stillness from are taken now (the first
            // Update that sees a non-resting copy records it and does not hold).
            m_Since = now;
            m_Still.clear();
            for (const EntityInfo& e : entities)
                if (!e.resting) m_Still.emplace(e.netId, e.position);
        }
        m_Seen = server;
        m_CaughtUp = holds;
        return holds && now - m_Since >= m_Hold;
    }

    std::string DrainWatch::Describe(double now) const {
        return std::format("applied opSeq {} of {}, {}{}, {} copies not resting ({} moved or new since the hold began), held for {:.1f} s",
                           m_Applied, m_Server, m_Synced ? "synced" : "NOT SYNCED", m_Playing ? "" : ", not playing", m_NotResting, m_Moved,
                           m_CaughtUp ? now - m_Since : 0.0);
    }

    // --- numbers and lines -----------------------------------------------------------------------------

    Percentiles PercentilesOf(std::vector<double> values) {
        Percentiles p;
        p.count = values.size();
        if (values.empty()) return p;
        std::sort(values.begin(), values.end());
        const auto rank = [&values](double f) {
            const size_t r = static_cast<size_t>(std::ceil(f * static_cast<double>(values.size())));
            return values[std::min(values.size() - 1, r == 0 ? 0 : r - 1)];
        };
        p.p50 = rank(0.50);
        p.p99 = rank(0.99);
        p.max = values.back();
        return p;
    }

    void DurationHistogram::Add(double seconds) {
        const double b = std::max(0.0, seconds) / m_Width;
        const size_t last = m_Counts.size() - 1;
        ++m_Counts[b >= static_cast<double>(last) ? last : static_cast<size_t>(b)];
        ++m_Count;
        m_Max = std::max(m_Max, seconds);
    }

    double DurationHistogram::Percentile(double fraction) const {
        if (m_Count == 0) return 0.0;
        // PercentilesOf's rank: the ceil(f * n)-th smallest value (at least the first).
        const uint64_t rank = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(fraction * static_cast<double>(m_Count))));
        uint64_t seen = 0;
        for (size_t i = 0; i < m_Counts.size(); ++i) {
            seen += m_Counts[i];
            if (seen >= rank) return i + 1 == m_Counts.size() ? m_Max : std::min(m_Max, static_cast<double>(i + 1) * m_Width);
        }
        return m_Max;
    }

    Percentiles DurationHistogram::Summary() const {
        Percentiles p;
        p.count = static_cast<size_t>(m_Count);
        p.p50 = Percentile(0.50);
        p.p99 = Percentile(0.99);
        p.max = m_Max;
        return p;
    }

    uint64_t EntityRestHash(const std::vector<EntityInfo>& entities) {
        // Order-free: a sum of mixed terms, as EntityListHash.
        uint64_t sum = 0;
        uint64_t resting = 0;
        for (const EntityInfo& e : entities) {
            if (!e.resting) continue;
            ++resting;
            glm::quat q = e.rotation;
            if (q.w < 0.0f) q = -q;
            uint64_t h = voxel::HashMix64(0x7e57ull, e.netId);
            for (int i = 0; i < 3; ++i) h = voxel::HashMix64(h, static_cast<uint64_t>(std::llround(static_cast<double>(e.position[i]) * 1000.0)));
            for (int i = 0; i < 4; ++i) h = voxel::HashMix64(h, static_cast<uint64_t>(std::llround(static_cast<double>(q[i]) * 10000.0)));
            sum += h;
        }
        return voxel::HashMix64(sum, resting);
    }

    std::string SessionHashLine(const Client& client) {
        const voxel::WorldHashResult h = client.Hash();
        const SyncBox box = client.SyncWindow();
        const glm::ivec3 boxMax = box.Max();
        return std::format("Session: slot {} applied opSeq {} of the server's {}; world hash {:016x} ({} chunks{}) over [{},{},{}..{},{},{}); "
                           "joined at opSeq {}, tick {}",
                           client.Slot(), client.AppliedOpSeq(), client.ServerOpSeq(), h.hash, h.solidChunks,
                           h.Complete() ? "" : ", INCOMPLETE", box.min.x, box.min.y, box.min.z, boxMax.x, boxMax.y, boxMax.z,
                           client.JoinOpSeq(), client.JoinTick());
    }

    std::string ServerLeaveLine(Server& server, uint8_t slot) {
        // Stage 8: both hashes come from the server's per-chunk cache (warmed at start): a leave costs
        // lookups and the chunks changed since, not the 207 ms of two full hashes (owner's run).
        // Stage 7a: and the hash over that client's sync box (its window inside the server's), the box its
        // own hash covers (the region's window for a client whose window is the region's: one hash is both).
        const SyncBox box = server.ClientSyncBox(slot);
        const SyncBox all = RegionSyncBox(server.Region());
        // A client whose window moves (a region wider than its window) compares its window hash only: the
        // region's hash is not taken for it (stage 8: 209 000 lookups on the arena, 13 ms in the leave tick).
        const bool whole = box.min == all.min && box.size == all.size;
        const voxel::WorldHashResult wh = whole ? server.Hash() : server.Hash(box);
        const glm::ivec3 max = box.Max();
        uint32_t joinOpSeq = 0, joinTick = 0;
        const bool known = server.ClientJoinPoint(slot, joinOpSeq, joinTick);
        return std::format("Session: the client of slot {} left; server at opSeq {}, tick {}, {}; window hash {:016x} over [{},{},{}..{},{},{}); {}",
                           slot, server.LastOpSeq(), server.TickNumber(),
                           whole ? std::format("world hash {:016x} ({} chunks)", wh.hash, wh.solidChunks)
                                 : std::string("world hash not taken (its window moves)"),
                           wh.hash, box.min.x, box.min.y, box.min.z, max.x, max.y, max.z,
                           known ? std::format("joined at opSeq {}, tick {}", joinOpSeq, joinTick) : std::string("join point unknown"));
    }

    std::string SessionEntitiesLine(const Client& client) {
        const std::vector<EntityInfo> entities = client.Entities();
        size_t resting = 0;
        for (const EntityInfo& e : entities) resting += e.resting ? 1u : 0u;
        return std::format("Session: slot {} entities at opSeq {}: {} object(s), {} resting, id hash {:016x}, rest hash {:016x}", client.Slot(),
                           client.AppliedOpSeq(), entities.size(), resting, EntityListHash(entities), EntityRestHash(entities));
    }

    std::string ServerEntitiesLine(const Server& server) {
        const std::vector<EntityInfo> entities = server.Entities();
        size_t resting = 0;
        for (const EntityInfo& e : entities) resting += e.resting ? 1u : 0u;
        return std::format("Session: entities at opSeq {}: {} object(s), {} resting, id hash {:016x}, rest hash {:016x}", server.LastOpSeq(),
                           entities.size(), resting, EntityListHash(entities), EntityRestHash(entities));
    }

    std::string AwakeEntitiesText(const std::vector<EntityInfo>& entities) {
        std::vector<const EntityInfo*> awake;
        for (const EntityInfo& e : entities)
            if (!e.resting) awake.push_back(&e);
        std::sort(awake.begin(), awake.end(), [](const EntityInfo* a, const EntityInfo* b) { return a->netId < b->netId; });
        std::string text = std::format("{} awake", awake.size());
        for (size_t i = 0; i < awake.size(); ++i)
            text += std::format("{}{} ({:.3f}, {:.3f}, {:.3f})", i == 0 ? ": " : "; ", awake[i]->netId, awake[i]->position.x, awake[i]->position.y,
                                awake[i]->position.z);
        return text;
    }

    std::string SessionAwakeLine(const Client& client) {
        return std::format("Session: slot {} awake at opSeq {}: {}", client.Slot(), client.AppliedOpSeq(), AwakeEntitiesText(client.Entities()));
    }

    std::string ServerAwakeLine(const Server& server) {
        return std::format("Session: awake at opSeq {}: {}", server.LastOpSeq(), AwakeEntitiesText(server.Entities()));
    }

    std::string DescribeBotMovement(const Bot& bot, double tickSeconds) {
        const Client& c = bot.GetClient();
        const PredictionStats& p = c.Prediction();
        const BotPilot* pilot = bot.Pilot();
        return std::format("Client {} movement: {} ticks predicted ({} held), {} snapshots compared: {} within 2 cm, {} corrected ({:.2f} per "
                           "minute), {} snapped, max error {:.3f} m; errors <1 mm {}, <2 cm {}, <5 cm {}, <20 cm {}, <1.5 m {}, more {}; replays "
                           "{} ticks (max {}, {:.3f} ms max); waited for events {}, gave up {}; clock {} jumps / {} resyncs, margin {}..{}, "
                           "target {}; {} casts shown ({} the server did not run), pilot cast {} / jumped {}; final error {:.3f} m",
                           bot.Settings().id, p.ticks, p.heldTicks, p.compared, p.within, p.corrections, p.CorrectionsPerMinute(tickSeconds),
                           p.snaps, p.maxError, p.errorBuckets[0], p.errorBuckets[1], p.errorBuckets[2], p.errorBuckets[3], p.errorBuckets[4],
                           p.errorBuckets[5], p.replayTicks, p.maxReplayTicks, p.maxReplaySeconds * 1000.0, p.waitedForEvents,
                           p.eventTimeouts, p.clockJumps, p.hardResyncs, p.minMargin, p.maxMargin, c.Clock().Target(), bot.CastsShown(),
                           p.castsRejected, pilot ? pilot->Casts() : 0u, pilot ? pilot->Jumps() : 0u, p.lastError);
    }

    std::string DescribeBotObjects(const Bot& bot) {
        const Client& c = bot.GetClient();
        const ClientStats& cs = c.Stats();
        const BotPilot* pilot = bot.Pilot();
        return std::format("Client {} objects: {} copies made ({} failed, {} lost), {} despawned ({} kept as rubble), {} rests ({} stale), {} "
                           "wakes, {} models, {} fragments; {} body samples taken, {} ignored, largest sample gap {} ticks; {} ticks a copy "
                           "carried the player; pilot crates {}",
                           bot.Settings().id, cs.copiesMade, cs.copiesFailed, cs.copiesLost, cs.despawns, cs.toRubble, cs.rests, cs.staleRests,
                           cs.wakes, cs.models, cs.fragments, cs.bodySamples, cs.bodySamplesIgnored, c.Bodies().MaxGap(), cs.heldUnderPlayer,
                           pilot ? pilot->Crates() : 0u);
    }

    std::string DescribeBotJoin(const Bot& bot, double tickSeconds) {
        const Client& c = bot.GetClient();
        const ClientStats& cs = c.Stats();
        return std::format(
            "Join: bot {} started after {:.2f} s of server ticks ({:.2f} s wall; chunks within 32 m current, entities within 32 m of the "
            "spawn in (BaselineNear)), join data in after {:.2f} s ({:.2f} s wall; no chunk waiting for the server), synced after {:.2f} s "
            "({:.2f} s wall; every chunk current), {:.1f} KiB bulk received ({} ChunkData, {} baseline entities, {} discarded), {} chunk / {} "
            "model requests, {} ops skipped for chunks without a base, {} replayed; CPU: sync {:.3f} ms per Poll (max {:.2f}), bulk installs "
            "{:.1f} ms in all (max {:.2f} ms, {} in one Poll)",
            bot.Settings().id, static_cast<double>(cs.joinStartTicks) * tickSeconds, cs.joinStartSeconds,
            static_cast<double>(cs.joinDataTicks) * tickSeconds, cs.joinDataSeconds, static_cast<double>(cs.joinSyncTicks) * tickSeconds,
            cs.joinSyncSeconds, static_cast<double>(cs.bulkBytes) / 1024.0, cs.chunkData, cs.baselineEntities, cs.baselineDiscarded,
            cs.chunkRequests, cs.modelRequests, c.Sync().Stats().chunkSkips, c.Sync().Stats().replays,
            cs.polls > 0 ? cs.syncSeconds * 1000.0 / static_cast<double>(cs.polls) : 0.0, cs.maxSyncSeconds * 1000.0, cs.bulkSeconds * 1000.0,
            cs.maxBulkSeconds * 1000.0, cs.maxBulkInPoll);
    }

    namespace {
        bool StoreOk(const Bot& bot) {
            sim::Simulation* simulation = bot.GetSimulation();
            if (simulation == nullptr) return false;
            const voxel::WorldStats ws = simulation->World().Stats();
            const ClientStats& cs = bot.GetClient().Stats();
            return ws.storeFullLoads == 0 && ws.storeFullDecodes == 0 && cs.unsyncedChunks == 0 &&
                   (bot.Settings().client.windowChunks == 0 || cs.maxStoreUsed + kGameStoreHeadroom <= simulation->Chunks().Capacity());
        }
    }

    std::vector<std::string> DescribeBotWindow(const Bot& bot) {
        std::vector<std::string> lines;
        sim::Simulation* simulation = bot.GetSimulation();
        if (simulation == nullptr) return lines;
        const Client& c = bot.GetClient();
        const ClientStats& cs = c.Stats();
        const uint32_t id = bot.Settings().id;
        const voxel::WorldStats ws = simulation->World().Stats();
        const SyncBox w = c.Window(), sbx = c.SyncWindow();
        lines.push_back(std::format("Client {} window: {} x {} x {} chunks at ({}, {}, {}){}, sync box {} x {} x {}; {} window updates ({} "
                                    "resets); store {} / {} slots, at most {} used, store full {} / {}{}; {} dense / {} packed chunks; {} "
                                    "ChunkData installed, {} stale; re-sent: {} ChunkData for chunks received before ({:.1f} KiB), {} of them unchanged ({:.1f} KiB)",
                                    id, w.size.x, w.size.y, w.size.z, w.min.x, w.min.y, w.min.z, c.WindowMoves() ? " (follows the player)" : "",
                                    sbx.size.x, sbx.size.y, sbx.size.z, cs.windowUpdates, cs.windowResets, simulation->Chunks().UsedCount(),
                                    simulation->Chunks().Capacity(), cs.maxStoreUsed, ws.storeFullLoads, ws.storeFullDecodes,
                                    StoreOk(bot) ? "" : " - OVER THE GATE", ws.denseChunks, ws.packedChunks, c.Sync().Stats().chunksInstalled,
                                    c.Sync().Stats().staleChunkData, cs.chunkDataRepeats,
                                    static_cast<double>(cs.chunkDataRepeatBytes) / 1024.0, cs.chunkDataUnchanged,
                                    static_cast<double>(cs.chunkDataUnchangedBytes) / 1024.0));
        if (!c.Synced()) {
            const ChunkSyncCounts n = c.Sync().Counts();
            lines.push_back(std::format("Client {} is not synced: {} chunks current, {} without a base, {} awaiting data, {} based but not "
                                        "loaded ({} pending in the world), {} requested, {} unsynced",
                                        id, n.current, n.noBase, n.awaiting, n.basedNotReady, ws.pendingChunks, n.requested, n.unsynced));
        }
        if (bot.Settings().walkAcross && bot.Pilot() != nullptr) {
            const BotPilot& pilot = *bot.Pilot();
            // Where it gave up (x / z, the first 8): whatever held it there (a tree, a wall, a pit, a body).
            std::string where;
            for (size_t i = 0; i < pilot.GiveUps().size() && i < 8; ++i)
                where += std::format("{}({:.1f}, {:.1f})", i == 0 ? " at " : ", ", pilot.GiveUps()[i].x, pilot.GiveUps()[i].y);
            if (pilot.GiveUps().size() > 8) where += ", ...";
            // The longest still stretch (within 1 m while it headed for a waypoint) and the longest confined
            // one (within 4 m): how long, how much of it held by the collider gate, where, from which pilot
            // tick, and what was around it 1 s / 2 s in (the 2026-11-22 failure stood 150 s for no known reason).
            const double tick = static_cast<double>(simulation->FixedTimeStep());
            const auto stretch = [tick](const char* what, const BotPilot::StillStretch& st, const std::string& context) {
                return std::format("longest {} {:.1f} s ({:.1f} s of it held) at ({:.1f}, {:.1f}, {:.1f}) from pilot tick {}{}", what, st.ticks * tick,
                                   st.heldTicks * tick, st.at.x, st.at.y, st.at.z, st.startTick, context.empty() ? std::string() : ": " + context);
            };
            lines.push_back(std::format("Client {} walk: {} of {} waypoints reached{}, {} given up{}, {} detours ({} ticks, {:.1f} % of {} walking), "
                                        "{}; {}; x from {:.1f} to {:.1f} m, "
                                        "feet at ({:.1f}, {:.1f}, {:.1f}); {} ticks held over chunks without a base, {} snapshots taken as they "
                                        "were, {} resumes",
                                        id, pilot.WaypointsReached(), 3 * (pilot.Laps() + 1),
                                        pilot.Laps() > 0 ? std::format(" ({} laps begun again, --loop)", pilot.Laps()) : std::string(),
                                        pilot.WaypointsGivenUp(), where, pilot.Detours(), pilot.DetourTicks(),
                                        pilot.WalkTicks() > 0 ? 100.0 * pilot.DetourTicks() / pilot.WalkTicks() : 0.0, pilot.WalkTicks(),
                                        stretch("still", pilot.LongestStill(), bot.LongestStillContext()),
                                        stretch("within 4 m", pilot.LongestConfined(), bot.LongestConfinedContext()), bot.MinX(),
                                        bot.MaxX(), c.Player().position.x, c.Player().position.y, c.Player().position.z,
                                        c.Prediction().syncHeldTicks, c.Prediction().syncTaken, c.Prediction().syncResumes));
        }
        return lines;
    }

    BotVerdict JudgeBot(const Bot& bot, const BotGates& gates) {
        BotVerdict v;
        const auto fail = [&v](std::string why) {
            v.ok = false;
            v.reasons.push_back(std::move(why));
        };
        const Client& c = bot.GetClient();
        const ClientStats& cs = c.Stats();
        const ChunkSyncStats& ss = c.Sync().Stats();
        const BotSettings& s = bot.Settings();
        if (bot.GetSimulation() == nullptr) fail("not attached");
        if (c.State() != ClientState::Playing) fail("not playing at the end");
        if (!c.Synced()) fail("not synced");
        if (bot.GetSimulation() != nullptr && !c.Hash().Complete()) fail("its hash is incomplete");
        if (c.AppliedOpSeq() != c.ServerOpSeq()) fail(std::format("not drained (opSeq {} of {})", c.AppliedOpSeq(), c.ServerOpSeq()));
        if (cs.skippedChunks != 0) fail(std::format("{} skipped chunks", cs.skippedChunks));
        if (cs.unsyncedChunks != 0) fail(std::format("{} unsynced chunks", cs.unsyncedChunks));
        // Server terrain mode exactly when its generator was broken (review of the stage 8 plan: a bot whose
        // own generation regressed must not pass by playing on the server's terrain); no check out of order,
        // no entity mismatch; no chunk difference unless a broken chunk was injected (judged below).
        for (std::string& why : ClientCheckFailures(c, s.brokenGenerator))
            if (!s.hasBrokenChunk || why.find("differed") == std::string::npos) fail(std::move(why));
        const std::vector<glm::ivec3> mismatched = c.MismatchedChunks();
        if (s.hasBrokenChunk) {
            const glm::ivec3 b = s.brokenChunk;
            // Judged by its coordinate, whatever class its mismatch had: exactly the injected chunk, repaired from data.
            const bool exact = mismatched.size() == 1 && mismatched[0] == b;
            const bool repaired = exact && ss.repairsInstalled >= 1 && c.Sync().FromData(b);
            // Review of stage 8: and only before its repair. Once from data it is the server's chunk (a fixed
            // window never generates it again): differing again is a replication bug, whether or not a
            // second repair lands before the end.
            const bool again = ss.repeatRepairs != 0 || ss.repairsRefused != 0 || ss.stuckMismatches != 0 || ss.mismatches > ss.repairsInstalled + 1u ||
                               (!c.WindowMoves() && (ss.mismatches > 1u || ss.repairsInstalled > 1u));
            if (again)
                fail(std::format("its broken chunk ({}, {}, {}) differed again after its repair ({} mismatches, {} repairs installed, {} repeated, "
                                 "{} refused, {} stuck)",
                                 b.x, b.y, b.z, ss.mismatches, ss.repairsInstalled, ss.repeatRepairs, ss.repairsRefused, ss.stuckMismatches));
            if (repaired && !again) {
                v.checksNote = std::format(" - its broken chunk ({}, {}, {}) was found and repaired", b.x, b.y, b.z);
            } else {
                v.checksNote = std::format(" - ITS BROKEN CHUNK ({}, {}, {}) WAS NOT REPAIRED", b.x, b.y, b.z);
                fail(std::format("its broken chunk ({}, {}, {}) was not found exactly and repaired ({} chunk(s) differed, {} repairs installed)",
                                 b.x, b.y, b.z, mismatched.size(), ss.repairsInstalled));
            }
        } else if (!mismatched.empty() || ss.mismatches != 0) {
            v.checksNote = " - A CHECK FOUND A DIFFERENCE NO FAULT MADE";
        }
        if (s.brokenGenerator)
            v.checksNote += c.TerrainMode() ? " - its broken generator put it in server terrain mode"
                                            : " - ITS BROKEN GENERATOR DID NOT PUT IT IN SERVER TERRAIN MODE";
        if (cs.hashChecks < gates.minHashChecks) fail(std::format("{} HashChecks compared, fewer than {}", cs.hashChecks, gates.minHashChecks));
        if (cs.entityChecks + cs.entityChecksPartial < gates.minEntityChecks)
            fail(std::format("{} EntityChecks compared, fewer than {}", cs.entityChecks + cs.entityChecksPartial, gates.minEntityChecks));
        const PredictionStats& p = c.Prediction();
        if (p.compared > 0 && p.lastError > gates.maxFinalError)
            fail(std::format("its own position is {:.3f} m off the server's after the drain (over {:.2f})", p.lastError, gates.maxFinalError));
        if (gates.storeGate && bot.GetSimulation() != nullptr && !StoreOk(bot)) fail("over the store gate");
        // Review of stage 8: it played as it was told (a bot that stops predicting, casting or walking
        // only makes the run easier, so it must not pass silently).
        const BotPilot* pilot = bot.Pilot();
        const double acted = static_cast<double>(bot.ActedTicks());
        if (gates.minActedShare > 0.0f && acted < gates.minActedShare * static_cast<double>(bot.WantedTicks()))
            fail(std::format("its pilot drove {} of the {} ticks it was asked to act on (it stopped predicting?)", bot.ActedTicks(),
                             bot.WantedTicks()));
        if (gates.minPilotShare > 0.0f && pilot != nullptr) {
            const PilotSettings& ps = s.pilot;
            if (ps.casts && ps.castEveryTicks > 0 && !s.walkAcross &&
                pilot->Casts() < static_cast<uint32_t>(gates.minPilotShare * acted / static_cast<double>(ps.castEveryTicks)))
                fail(std::format("its pilot cast {} times in {} ticks (one every {} asked)", pilot->Casts(), bot.ActedTicks(), ps.castEveryTicks));
            if (ps.crateEveryTicks > 0 &&
                pilot->Crates() < static_cast<uint32_t>(gates.minPilotShare * acted / static_cast<double>(ps.crateEveryTicks)))
                fail(std::format("its pilot dropped {} crates in {} ticks (one every {} asked)", pilot->Crates(), bot.ActedTicks(),
                                 ps.crateEveryTicks));
        }
        if (pilot != nullptr && p.castsRejected > static_cast<uint64_t>(gates.maxRejectedCastShare * static_cast<float>(pilot->Casts())) + 2u)
            fail(std::format("the server did not run {} of its {} casts", p.castsRejected, pilot->Casts()));
        if (gates.minWalkShare > 0.0f && s.walkAcross && pilot != nullptr) {
            const sim::RegionDesc& r = c.Region();
            const float chunk = static_cast<float>(voxel::kChunkSize) * r.terrain.voxelSize;
            // The waypoints' span (BuildAndAttach) less what the pilot leaves at each end: it counts a waypoint
            // reached within reachMetres (recheck of stage 8: 4 of a 7-chunk region's 20 m, so a walker that
            // reached every waypoint failed a share over 0.8 there).
            const float waypoints = static_cast<float>(r.playMaxChunk.x - r.playMinChunk.x) * chunk - 8.0f;
            const float span = std::max(0.0f, waypoints - 2.0f * s.pilot.reachMetres);
            if (bot.MaxX() - bot.MinX() < gates.minWalkShare * span)
                fail(std::format("it walked x from {:.1f} to {:.1f} m, under {:.0f} % of the {:.1f} m its waypoints allow", bot.MinX(),
                                 bot.MaxX(), gates.minWalkShare * 100.0f, span));
            if (pilot->WaypointsReached() < 2 || pilot->WaypointsGivenUp() >= pilot->WaypointsReached())
                fail(std::format("it reached {} waypoints and gave up {}", pilot->WaypointsReached(), pilot->WaypointsGivenUp()));
            if (c.WindowMoves() && cs.windowUpdates == 0) fail("its window never moved");
        }
        if (s.walkAcross && pilot != nullptr) {
            const double tick = bot.GetSimulation() != nullptr ? static_cast<double>(bot.GetSimulation()->FixedTimeStep()) : 1.0 / 60.0;
            for (std::string& why : WalkerSnagFailures(*pilot, gates, tick)) fail(std::move(why));
        }
        return v;
    }

    std::vector<std::string> WalkerSnagFailures(const BotPilot& pilot, const BotGates& gates, double tickSeconds) {
        std::vector<std::string> reasons;
        const BotPilot::StillStretch& still = pilot.LongestStill();
        const double stillSeconds = static_cast<double>(still.ticks) * tickSeconds;
        if (gates.maxStillSeconds > 0.0f && stillSeconds > static_cast<double>(gates.maxStillSeconds))
            reasons.push_back(std::format("it stood still {:.1f} s at ({:.1f}, {:.1f}, {:.1f}) from pilot tick {} (over {:.0f} s; {:.1f} s of it held)",
                                          stillSeconds, still.at.x, still.at.y, still.at.z, still.startTick, gates.maxStillSeconds,
                                          static_cast<double>(still.heldTicks) * tickSeconds));
        const double share = pilot.WalkTicks() > 0 ? static_cast<double>(pilot.DetourTicks()) / static_cast<double>(pilot.WalkTicks()) : 0.0;
        if (gates.maxDetourShare > 0.0f && share > static_cast<double>(gates.maxDetourShare))
            reasons.push_back(std::format("it spent {} of its {} walking ticks detouring ({:.1f} %, over {:.0f} %; {} detours)", pilot.DetourTicks(),
                                          pilot.WalkTicks(), 100.0 * share, 100.0 * gates.maxDetourShare, pilot.Detours()));
        const BotPilot::StillStretch& confined = pilot.LongestConfined();
        const double confinedSeconds = static_cast<double>(confined.ticks) * tickSeconds;
        if (gates.maxConfinedSeconds > 0.0f && confinedSeconds > static_cast<double>(gates.maxConfinedSeconds))
            reasons.push_back(std::format("it stayed within {:.0f} m {:.1f} s at ({:.1f}, {:.1f}, {:.1f}) from pilot tick {} (over {:.0f} s; {:.1f} s of it held)",
                                          BotPilot::kConfinedMetres, confinedSeconds, confined.at.x, confined.at.y, confined.at.z,
                                          confined.startTick, gates.maxConfinedSeconds, static_cast<double>(confined.heldTicks) * tickSeconds));
        return reasons;
    }

    std::vector<std::string> ClientCheckFailures(const Client& c, bool terrainModeExpected) {
        std::vector<std::string> reasons;
        const ClientStats& cs = c.Stats();
        const ChunkSyncStats& ss = c.Sync().Stats();
        if (c.TerrainMode() != terrainModeExpected)
            reasons.push_back(c.TerrainMode() ? "in server terrain mode without an injected fault"
                                              : "its broken generator did not put it in server terrain mode");
        if (cs.checksOutOfOrder != 0) reasons.push_back(std::format("{} check(s) out of order", cs.checksOutOfOrder));
        if (cs.entityMismatches != 0) reasons.push_back(std::format("{} entity mismatch(es)", cs.entityMismatches));
        const std::vector<glm::ivec3> mismatched = c.MismatchedChunks();
        if (!mismatched.empty() || ss.mismatches != 0)
            reasons.push_back(std::format("{} chunk(s) differed with no fault injected ({} mismatches)", mismatched.size(), ss.mismatches));
        return reasons;
    }

    // --- the broken-chunk candidate --------------------------------------------------------------------

    bool UsableBrokenChunk(Server& server, const glm::ivec3& c, std::string* why) {
        const voxel::World& w = server.Simulation().World();
        const glm::dvec3 spawn = server.Template().spawn;
        const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(w.VoxelSize());
        const double reach = static_cast<double>(HashCheckSettings{}.pristineMetres);
        const double distance = glm::distance((glm::dvec3(c) + glm::dvec3(0.5)) * chunkMetres, spawn);
        const char* reason = nullptr;
        if (w.StateOf(c) != voxel::CellState::Dense) reason = "not Dense on the server";
        else if (server.IsModified(c)) reason = "modified (it would come as data, never generated)";
        else if (distance > reach) reason = "farther than 12 m from the spawn (a standing bot's checks would never reach it)";
        for (const ProbeHash& p : server.Probes())
            if (p.chunk == c) reason = "a probe chunk";
        if (reason && why) *why = reason;
        return reason == nullptr;
    }

    bool BrokenChunkCandidate(Server& server, glm::ivec3& out, std::string* why, double buriedMetres) {
        const voxel::World& w = server.Simulation().World();
        const glm::dvec3 spawn = server.Template().spawn;
        const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(w.VoxelSize());
        const double reach = static_cast<double>(HashCheckSettings{}.pristineMetres);
        const int top = (w.WindowMinChunk().y + w.WindowSize().y) * voxel::kChunkSize;
        // The ground of a chunk's middle column: its highest solid voxel (voxels).
        const auto ground = [&w, top](const glm::ivec3& c) {
            const int x = c.x * voxel::kChunkSize + voxel::kChunkSize / 2, z = c.z * voxel::kChunkSize + voxel::kChunkSize / 2;
            for (int y = top - 1; y >= w.WindowMinChunk().y * voxel::kChunkSize; --y)
                if (w.IsSolid({x, y, z})) return y;
            return w.WindowMinChunk().y * voxel::kChunkSize;
        };
        const glm::ivec3 at = voxel::VoxelToChunk(w.WorldToVoxel(spawn));
        const int r = static_cast<int>(std::ceil(reach / chunkMetres)) + 1;
        const int buried = static_cast<int>(std::ceil(buriedMetres / static_cast<double>(w.VoxelSize())));
        double best = 1e30;
        bool found = false;
        for (int dy = -r; dy <= r; ++dy)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    const glm::ivec3 c = at + glm::ivec3(dx, dy, dz);
                    const double d = glm::distance((glm::dvec3(c) + glm::dvec3(0.5)) * chunkMetres, spawn);
                    if (d >= best || !UsableBrokenChunk(server, c, nullptr)) continue;
                    // Its top row at least `buried` voxels under the ground of its column.
                    if ((c.y + 1) * voxel::kChunkSize - 1 > ground(c) - buried) continue;
                    best = d;
                    out = c;
                    found = true;
                }
        if (!found && why)
            *why = std::format("no pristine server-Dense chunk within 12 m of the spawn, {:.1f} m under the ground, that is not a probe", buriedMetres);
        return found;
    }

    bool ParseTimeline(const std::string& text, std::vector<TimelineEntry>& out, std::string* error) {
        out.clear();
        size_t pos = 0, line = 0;
        while (pos < text.size()) {
            const size_t end = text.find('\n', pos);
            ++line;
            if (end == std::string::npos) break;   // the server is still writing it: ignored
            const std::string row = text.substr(pos, end - pos);
            pos = end + 1;
            if (row.empty() || row[0] == '#') continue;
            TimelineEntry e;
            unsigned long long ns = 0;
            unsigned a = 0, j = 0;
            char tail = 0;
            if (std::sscanf(row.c_str(), "ops %u %llu %u %c", &a, &ns, &j, &tail) != 3) {
                if (error) *error = std::format("timeline line {} is not 'ops A NS J': '{}'", line, row);
                return false;
            }
            e.opSeq = a;
            e.nanoseconds = static_cast<int64_t>(ns);
            e.joining = j;
            if (!out.empty() && (e.opSeq < out.back().opSeq || e.nanoseconds < out.back().nanoseconds)) {
                if (error) *error = std::format("timeline line {} goes back in opSeq or time", line);
                return false;
            }
            out.push_back(e);
        }
        return true;
    }

    EventLatency LatencyFromTimeline(const std::vector<TimelineEntry>& timeline, const std::vector<std::pair<uint32_t, double>>& applied,
                                     double playingSince) {
        EventLatency r;
        std::vector<double> all, joins, quiet;
        for (const auto& [opSeq, at] : applied) {
            const auto it = std::lower_bound(timeline.begin(), timeline.end(), opSeq,
                                             [](const TimelineEntry& e, uint32_t v) { return e.opSeq < v; });
            if (it == timeline.end()) {
                ++r.unknown;
                continue;
            }
            const double made = static_cast<double>(it->nanoseconds) * 1e-9;
            const double ms = std::max(0.0, (at - made) * 1000.0);
            if (made < playingSince) {
                ++r.beforePlaying;
                r.beforePlayingMaxMs = std::max(r.beforePlayingMaxMs, ms);
                continue;
            }
            all.push_back(ms);
            (it->joining > 0 ? joins : quiet).push_back(ms);
        }
        r.all = PercentilesOf(std::move(all));
        r.duringJoins = PercentilesOf(std::move(joins));
        r.quiet = PercentilesOf(std::move(quiet));
        return r;
    }

    double ResidentMiB() {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("VmRSS:", 0) == 0) return std::atof(line.c_str() + 6) / 1024.0;
        }
        return 0.0;
    }
}
