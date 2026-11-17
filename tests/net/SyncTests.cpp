// Stage 7a (docs/design/networking.md "Stage 7a plan" and its amendments): the pieces of chunk sync
// tested alone - the client's base rule and op ring against a World without a transport, and the
// server's bulk pacer against a simulated link.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    constexpr int kGround = 40;

    /// A flat generator that can hold one chunk's generation (a chunk "still loading").
    class GatedGenerator final : public voxel::IWorldGenerator {
    public:
        explicit GatedGenerator(const voxel::WorldMaterials& ids) : m_Flat(voxel::CreateFlatGenerator(kGround, ids.grass, ids.stone)) {}
        voxel::ChunkContent Generate(const glm::ivec3& coord, voxel::Chunk& chunk) const override {
            std::unique_lock lock(m_Mutex);
            m_Cv.wait(lock, [&] { return !m_Holding || coord != m_Held; });
            lock.unlock();
            return m_Flat->Generate(coord, chunk);
        }
        voxel::HeightBounds VerticalBounds() const override { return {-16.0f, 47.0f}; }
        voxel::ColumnClass ClassifyColumn(int cx, int cz) const override { return m_Flat->ClassifyColumn(cx, cz); }
        void Hold(const glm::ivec3& c) {
            std::scoped_lock lock(m_Mutex);
            m_Holding = true;
            m_Held = c;
        }
        void Release() {
            {
                std::scoped_lock lock(m_Mutex);
                m_Holding = false;
            }
            m_Cv.notify_all();
        }

    private:
        std::shared_ptr<voxel::IWorldGenerator> m_Flat;
        mutable std::mutex m_Mutex;
        mutable std::condition_variable m_Cv;
        bool m_Holding = false;
        glm::ivec3 m_Held{0};
    };

    /// Releases a held generator however the test leaves its scope (a failed ASSERT with it held
    /// would hang the World's jobs instead of failing; review of stage 7a).
    struct ReleaseOnExit {
        GatedGenerator& generator;
        ~ReleaseOnExit() { generator.Release(); }
    };

    /// A full-height flat world at 0.5 m (16 m chunks), window 6 x 4 x 6 chunks around the origin.
    struct Flat {
        Flat() : store(2048), jobs(3) {
            ids = voxel::RegisterDefaultWorldMaterials(materials);
            generator = std::make_shared<GatedGenerator>(ids);
            voxel::WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = {6, 4, 6};
            config.fullHeight = true;
            config.heightRange = {-16.0f, 48.0f};
            config.reportLoads = true;
            world = std::make_unique<voxel::World>(config, store, jobs, generator, &materials);
        }
        net::SyncBox Box() const { return {world->WindowMinChunk(), world->WindowSize()}; }
        uint64_t ChunkHashAt(const glm::ivec3& c) const {
            voxel::Chunk chunk;
            EXPECT_TRUE(world->ReadChunk(c, chunk));
            return voxel::ChunkHash(chunk);
        }
        voxel::ChunkStore store;
        JobSystem jobs;
        voxel::MaterialLibrary materials;
        voxel::WorldMaterials ids;
        std::shared_ptr<GatedGenerator> generator;
        std::unique_ptr<voxel::World> world;
    };

    /// Op i (1-based): stone -> gold and then air -> stone swaps alternating on one wide sphere at
    /// the surface across four chunks (none of the pairs is idempotent), so a chunk's content tells
    /// which ops reached it and in which order.
    sim::WorldOp Op(const Flat& f, uint32_t i) {
        const voxel::SphereQ s{glm::ivec3(0, kGround, 0) * 16, static_cast<uint16_t>((6 + i) * 16)};
        return i % 2 == 1 ? sim::WorldOp::Swap(s, f.ids.stone, f.ids.gold) : sim::WorldOp::Swap(s, 0, f.ids.stone);
    }

    net::WindowAck AckOf(const net::SyncBox& box, uint32_t asOf, const std::vector<glm::ivec3>& modified = {}) {
        net::WindowAck ack;
        ack.windowSeq = 1;
        ack.asOfOpSeq = asOf;
        ack.modified = voxel::VoxelMask(box.size);
        for (const glm::ivec3& c : modified) ack.modified.Set(c - box.min, true);
        return ack;
    }
}

// The one rule: an operation reaches a chunk only if the chunk is current and the operation's opSeq is
// above its base. A pristine chunk based at 0 gets every op; a chunk installed as of 3 gets only 4..5
// (the counters tell `>` from `>=`: with `>=` op 3 would be applied once more); a chunk without a base
// gets none and keeps the generator's content.
TEST(ChunkSync, AnEventIsAppliedOnlyAboveTheBase) {
    Flat ref, client;
    ref.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    client.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    const glm::ivec3 pristine(0, 1, 0), data(-1, 1, 0), none(0, 1, -1);
    std::vector<std::vector<uint8_t>> at3;
    for (uint32_t i = 1; i <= 5; ++i) {
        sim::ApplyOp(*ref.world, Op(ref, i));
        if (i == 3) {
            at3.emplace_back();
            ASSERT_TRUE(ref.world->EncodeChunkAt(data, at3.back()));
        }
    }
    net::ChunkSync sync;
    sync.Start(0, client.Box());
    sync.BeginWindow(client.Box(), 1, false);
    ASSERT_TRUE(sync.OnAck(*client.world, AckOf(client.Box(), 0, {data, none}), 0));
    EXPECT_EQ(sync.StateOf(none), net::ChunkSyncState::Awaiting);
    EXPECT_TRUE(sync.IsCurrent(pristine));
    EXPECT_EQ(sync.OnChunkData(*client.world, net::ChunkData{data, 3, at3[0]}, 0), net::ChunkSync::DataOutcome::Installed);
    for (uint32_t i = 1; i <= 5; ++i) {
        const net::ChunkSync::OpOutcome out = sync.Apply(*client.world, i, Op(client, i), nullptr);
        EXPECT_FALSE(out.complete) << "op " << i << " touches the chunk without a base";
    }
    EXPECT_EQ(client.ChunkHashAt(pristine), ref.ChunkHashAt(pristine));
    EXPECT_EQ(client.ChunkHashAt(data), ref.ChunkHashAt(data));
    EXPECT_EQ(sync.SkippedAtOrBelowBase(data), 3u);
    EXPECT_EQ(sync.AppliedOps(data), 2u);
    EXPECT_EQ(sync.AppliedOps(pristine), 5u);
    Flat fresh;
    fresh.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    EXPECT_EQ(client.ChunkHashAt(none), fresh.ChunkHashAt(none)) << "a chunk without a base was changed";
    EXPECT_NE(client.ChunkHashAt(none), ref.ChunkHashAt(none));
    EXPECT_FALSE(sync.Synced());
    // A data message older than the base is dropped; the data of the missing chunk makes it current.
    EXPECT_EQ(sync.OnChunkData(*client.world, net::ChunkData{data, 2, at3[0]}, 5), net::ChunkSync::DataOutcome::Stale);
    std::vector<uint8_t> at5;
    ASSERT_TRUE(ref.world->EncodeChunkAt(none, at5));
    EXPECT_EQ(sync.OnChunkData(*client.world, net::ChunkData{none, 5, at5}, 5), net::ChunkSync::DataOutcome::Installed);
    EXPECT_TRUE(sync.Synced());
    EXPECT_EQ(voxel::WorldHash(*client.world, client.Box().min, client.Box().Max()).hash,
              voxel::WorldHash(*ref.world, ref.Box().min, ref.Box().Max()).hash);
    ref.jobs.WaitIdle();
    client.jobs.WaitIdle();
    fresh.jobs.WaitIdle();
}

// A chunk still loading when operations cross it: the World skips it, and its load report replays
// them from the ring in order. With a ring of 2 ops the base is no longer covered: the chunk is
// requested (once), never replayed in part.
TEST(ChunkSync, AChunkStillLoadingGetsItsEventsWhenItLoads) {
    for (const size_t ring : {size_t{1024}, size_t{2}}) {
        Flat ref, client;
        ref.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
        const glm::ivec3 held(0, 1, 0);
        client.generator->Hold(held);
        const ReleaseOnExit release{*client.generator};
        // Load everything but the held chunk.
        for (int i = 0; i < 4000; ++i) {
            client.world->Update(glm::dvec3(0.0, 20.0, 0.0));
            const voxel::WorldStats ws = client.world->Stats();
            if (ws.pendingChunks == 1 && client.world->StateOf(held) == voxel::CellState::Loading) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(client.world->StateOf(held), voxel::CellState::Loading);
        net::ChunkSync sync(ring, size_t{1} << 20);
        sync.Start(0, client.Box());
        sync.BeginWindow(client.Box(), 1, false);
        sync.OnLoaded(*client.world, client.world->TakeLoadedChunks(), 0);
        ASSERT_TRUE(sync.OnAck(*client.world, AckOf(client.Box(), 0), 0));
        EXPECT_FALSE(sync.IsCurrent(held));
        for (uint32_t i = 1; i <= 3; ++i) {
            sim::ApplyOp(*ref.world, Op(ref, i));
            const net::ChunkSync::OpOutcome out = sync.Apply(*client.world, i, Op(client, i), nullptr);
            EXPECT_FALSE(out.complete);
        }
        client.generator->Release();
        for (int i = 0; i < 4000 && !client.world->IsChunkLoaded(held); ++i) {
            client.world->Update(glm::dvec3(0.0, 20.0, 0.0));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        sync.OnLoaded(*client.world, client.world->TakeLoadedChunks(), 3);
        if (ring == 1024) {
            EXPECT_TRUE(sync.IsCurrent(held));
            EXPECT_EQ(client.ChunkHashAt(held), ref.ChunkHashAt(held));
            EXPECT_TRUE(sync.Synced());
            EXPECT_EQ(sync.Stats().replays, 3u);
        } else {
            EXPECT_EQ(sync.StateOf(held), net::ChunkSyncState::Requested);
            EXPECT_EQ(sync.Stats().replays, 0u) << "a partial replay";
            const std::vector<glm::ivec3> asked = sync.TakeRequests(1000, 64, held, 256);
            ASSERT_EQ(asked.size(), 1u);
            EXPECT_EQ(asked[0], held);
            EXPECT_TRUE(sync.TakeRequests(1000, 64, held, 256).empty()) << "requested twice";
            EXPECT_FALSE(sync.Synced());
        }
        ref.jobs.WaitIdle();
        client.jobs.WaitIdle();
    }
}

// The ring holds at most 1024 operations or 1 MiB of their wire bytes, whichever is less; its floor
// rises with every eviction (a base below it is not covered); a Detach is kept as voxels only.
TEST(ChunkSync, RingKeeps1024OpsOr1MiB) {
    net::OpRing ring;
    ring.Reset(10);
    for (uint32_t i = 11; i <= 1110; ++i) ring.Push(i, sim::WorldOp::Box({0, 0, 0}, {1, 1, 1}, 3));
    EXPECT_EQ(ring.Count(), 1024u);
    EXPECT_EQ(ring.Floor(), 86u);
    EXPECT_TRUE(ring.Covers(86));
    EXPECT_FALSE(ring.Covers(85));

    net::OpRing small(1024, 4096);
    small.Reset(0);
    voxel::VoxelMask mask(glm::ivec3(40, 40, 40));
    for (int z = 0; z < 20; ++z)                       // a 20^3 block: 8000 B of raw bits, about 1 KB of runs
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) mask.Set(x + 3, y + 5, z + 7, true);
    small.Push(1, sim::WorldOp::Detach({5, 6, 7}, mask, glm::vec3(3.0f), 42));
    EXPECT_EQ(small.Count(), 1u);
    EXPECT_LT(small.Bytes(), mask.Bytes().size() / 4) << "counted by its wire bytes, not its raw bits";
    uint32_t seen = 0;
    small.ForEachAfter(0, voxel::VoxelToChunk(glm::ivec3(5, 6, 7)), [&](uint32_t seq, const sim::WorldOp& op) {
        ++seen;
        EXPECT_EQ(seq, 1u);
        EXPECT_EQ(op.kind, sim::WorldOpKind::Detach);
        EXPECT_EQ(op.netId, 0u);
        EXPECT_EQ(op.velocity, glm::vec3(0.0f));
        EXPECT_EQ(op.mask, mask);
        EXPECT_EQ(op.min, glm::ivec3(5, 6, 7));
    });
    EXPECT_EQ(seen, 1u);
    for (uint32_t i = 2; i <= 6; ++i) small.Push(i, sim::WorldOp::Detach({5, 6, 7}, mask, glm::vec3(0.0f), 0));
    EXPECT_LE(small.Bytes(), 4096u);
    EXPECT_LT(small.Count(), 6u);
    EXPECT_GE(small.Floor(), 1u) << "evicted by bytes";
    EXPECT_FALSE(small.Covers(0));
}

// Recheck of stage 7a, 3: chunks that catch up together (one ack, one load report, one Poll's
// ChunkData) walk the ring once and decode each operation once, however many of them it touches. With a
// per-chunk walk and a cache of the last 16 decodes, 8 chunks under 20 overlapping operations decoded
// all 20 again for every chunk (a cyclic scan of 20 over a FIFO of 16 never hits): 160.
TEST(ChunkSync, ABatchCatchingUpDecodesEachRingOpOnce) {
    Flat ref, client;
    ref.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    client.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    // 20 operations over the 8 chunks x -1..0, y 0..1, z -1..0 (16 m chunks, voxels -8..7 / 28..43):
    // gold boxes and clear masks alternating, each across every border, so a chunk's content tells the
    // order they reached it in.
    std::vector<sim::WorldOp> ops;
    for (int i = 1; i <= 20; ++i) {
        if (i % 2 == 1) {
            const int k = (i / 2) % 4;      // every box still crosses the y = 32 border
            ops.push_back(sim::WorldOp::Box(glm::ivec3(-8 + k, 28 + k, -8 + k), glm::ivec3(8 - k, 44 - k, 8 - k), i % 4 == 1 ? client.ids.gold : client.ids.stone));
        } else {
            voxel::VoxelMask mask(glm::ivec3(16, 16, 16));
            for (int z = 0; z < 16; ++z)
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x)
                        if ((x + 2 * y + 3 * z + i) % 5 == 0) mask.Set(x, y, z, true);
            ops.push_back(sim::WorldOp::Clear(glm::ivec3(-8, 28, -8), std::move(mask)));
        }
    }
    net::ChunkSync sync;
    sync.Start(0, client.Box());
    sync.BeginWindow(client.Box(), 1, false);
    for (uint32_t i = 1; i <= 20; ++i) {
        ASSERT_GT(sim::ApplyOp(*ref.world, ops[i - 1]).changed, 0u) << "op " << i;
        EXPECT_FALSE(sync.Apply(*client.world, i, ops[i - 1], nullptr).complete) << "no chunk has a base yet";
    }
    // One ack bases every chunk at 0: the 8 the operations touch replay all 20 from the ring.
    ASSERT_TRUE(sync.OnAck(*client.world, AckOf(client.Box(), 0), 20));
    const net::ChunkSyncStats& st = sync.Stats();
    std::printf("[ ring ] 8 chunks caught up with 20 ops: %llu replays, %llu decodes\n", static_cast<unsigned long long>(st.replays),
                static_cast<unsigned long long>(st.opsDecoded));
    EXPECT_EQ(st.replays, 160u);
    EXPECT_EQ(st.opsDecoded, 20u);
    for (int y = 0; y <= 1; ++y)
        for (int z = -1; z <= 0; ++z)
            for (int x = -1; x <= 0; ++x) {
                const glm::ivec3 c(x, y, z);
                EXPECT_TRUE(sync.IsCurrent(c));
                EXPECT_EQ(client.ChunkHashAt(c), ref.ChunkHashAt(c)) << "chunk (" << x << ", " << y << ", " << z << ")";
            }
    EXPECT_TRUE(sync.Synced());
    EXPECT_EQ(voxel::WorldHash(*client.world, client.Box().min, client.Box().Max()).hash,
              voxel::WorldHash(*ref.world, ref.Box().min, ref.Box().Max()).hash);
    ref.jobs.WaitIdle();
    client.jobs.WaitIdle();
}

// Plan amendment 1: the window must not grow with the queue it makes. A link of 64 kB/s with a 100 ms
// base round trip; the RTT seen grows with the bytes queued on the link (ENet's does). The pacer keeps
// about rate x (base RTT + target) in flight and the queue short; the formula of the first plan (the
// RTT as seen, no back-off) grows to its ceiling.
TEST(BulkPacer, TheWindowDoesNotGrowWithItsOwnQueue) {
    struct Link {
        double rate = 64.0 * 1024.0, baseRtt = 0.1;
        double queue = 0.0;                                // bytes not yet on the wire
        std::deque<std::pair<double, double>> unacked;    // (ack time, bytes)
        double delivered = 0.0;
        double InFlight() const {
            double b = queue;
            for (const auto& [t, n] : unacked) b += n;
            return b;
        }
        double Rtt() const { return baseRtt + queue / rate; }
        void Step(double now, double dt) {
            const double sent = std::min(queue, rate * dt);
            queue -= sent;
            if (sent > 0.0) unacked.emplace_back(now + baseRtt, sent);
            while (!unacked.empty() && unacked.front().first <= now) {
                delivered += unacked.front().second;
                unacked.pop_front();
            }
        }
    };
    const double dt = 1.0 / 60.0;
    auto run = [&](bool planned, size_t& peakWindow, double& maxQueueDelay, double& throughput) {
        Link link;
        net::BulkPacer pacer;
        double rate = 32768.0, window = 8192.0, last = 0.0;
        peakWindow = 0;
        maxQueueDelay = 0.0;
        for (int tick = 0; tick < 60 * 30; ++tick) {
            const double now = tick * dt;
            link.Step(now, dt);
            const double inFlight = link.InFlight();
            if (planned) {
                // The first plan: window = drainRate x (RTT as seen + 100 ms), clamped to 4-32 KiB.
                if (last > 0.0) rate += ((std::max(0.0, last - inFlight) / dt) - rate) / 8.0;
                window = std::clamp(rate * (link.Rtt() + 0.1), 4096.0, 32768.0);
            } else {
                pacer.Update(static_cast<size_t>(inFlight), link.Rtt(), dt);
                window = static_cast<double>(pacer.Window());
            }
            double f = inFlight;
            while (f < window) {
                link.queue += 1000.0;
                f += 1000.0;
                if (!planned) pacer.Sent(1000);
            }
            last = f;
            if (tick > 60 * 10) {
                peakWindow = std::max(peakWindow, static_cast<size_t>(window));
                maxQueueDelay = std::max(maxQueueDelay, link.queue / link.rate);
            }
        }
        throughput = link.delivered / 30.0;
    };
    size_t peak = 0, plannedPeak = 0;
    double delay = 0.0, plannedDelay = 0.0, rate = 0.0, plannedRate = 0.0;
    run(false, peak, delay, rate);
    run(true, plannedPeak, plannedDelay, plannedRate);
    std::printf("[ pacer ] window up to %zu B, queue up to %.0f ms, %.1f KiB/s delivered; the first plan's: %zu B, %.0f ms, %.1f KiB/s\n",
                peak, delay * 1000.0, rate / 1024.0, plannedPeak, plannedDelay * 1000.0, plannedRate / 1024.0);
    const double bdp = 64.0 * 1024.0 * (0.1 + 2.0 / 60.0);
    EXPECT_LE(static_cast<double>(peak), bdp * 1.5 + 1200.0);
    EXPECT_LT(delay, 0.1) << "the bulk queue in front of live events";
    EXPECT_GT(rate, 0.6 * 64.0 * 1024.0) << "the link is kept busy";
    EXPECT_GE(plannedPeak, 32768u) << "the first plan's window should grow to its ceiling: this test would not see the feedback";
    EXPECT_GT(plannedDelay, delay * 2.0);
}

// Review of stage 7a: the base RTT ages. A path whose RTT rises 60 ms for good (a route change, a
// Wi-Fi roam) must not pin the window to its floor for the rest of the session: within the ageing
// period (12 periods of 10 s) the risen RTT is the base and the window opens again. Halving happens at
// most once per round trip (it shows in the RTT only a round trip later).
TEST(BulkPacer, ALastingRiseOfTheRttBecomesTheNewBase) {
    struct Link {
        double rate = 64.0 * 1024.0, baseRtt = 0.1;
        double queue = 0.0;
        std::deque<std::pair<double, double>> unacked;
        double InFlight() const {
            double b = queue;
            for (const auto& [t, n] : unacked) b += n;
            return b;
        }
        double Rtt() const { return baseRtt + queue / rate; }
        void Step(double now, double dt) {
            const double sent = std::min(queue, rate * dt);
            queue -= sent;
            if (sent > 0.0) unacked.emplace_back(now + baseRtt, sent);
            while (!unacked.empty() && unacked.front().first <= now) unacked.pop_front();
        }
    };
    const double dt = 1.0 / 60.0;
    Link link;
    net::BulkPacer pacer;
    const net::BulkPacerSettings settings;
    size_t floorTicks = 0, lateWindow = 0;
    uint32_t halvingsInFirstSecond = 0, halvingsBefore = 0;
    const int stepTick = 60 * 30, endTick = stepTick + 60 * 150;
    for (int tick = 0; tick < endTick; ++tick) {
        const double now = tick * dt;
        if (tick == stepTick) {
            link.baseRtt = 0.16;
            halvingsBefore = pacer.Halvings();
        }
        if (tick == stepTick + 60) halvingsInFirstSecond = pacer.Halvings() - halvingsBefore;
        link.Step(now, dt);
        const double inFlight = link.InFlight();
        pacer.Update(static_cast<size_t>(inFlight), link.Rtt(), dt);
        const double window = static_cast<double>(pacer.Window());
        for (double f = inFlight; f < window; f += 1000.0) {
            link.queue += 1000.0;
            pacer.Sent(1000);
        }
        if (tick > stepTick && pacer.Window() <= settings.floorBytes) ++floorTicks;
        if (tick > endTick - 60 * 10) lateWindow = std::max(lateWindow, pacer.Window());
    }
    const double bdp = 64.0 * 1024.0 * (0.16 + settings.targetDelay);
    std::printf("[ pacer ] RTT 100 -> 160 ms: %u halvings in the first second, %.1f s at the floor, window up to %zu B in the last 10 s "
                "(bdp %.0f B), base %.0f ms\n",
                halvingsInFirstSecond, static_cast<double>(floorTicks) * dt, lateWindow, bdp, pacer.BaseRtt() * 1000.0);
    EXPECT_LE(halvingsInFirstSecond, 8u) << "at most one halving per round trip (160 ms)";
    EXPECT_GT(lateWindow, 4u * settings.floorBytes) << "the window stayed at its floor: the base RTT never aged";
    EXPECT_LE(static_cast<double>(lateWindow), bdp * 1.5 + 1200.0);
    EXPECT_NEAR(pacer.BaseRtt(), 0.16, 0.035);
}

// Review of stage 7a: a step of the game's window (48 x 44 x 48 = 101 376 records) costs its entering
// slab - in BeginWindow and in its ack - not the whole table, and a Poll with nothing Unsynced does not
// walk the table looking for requests.
TEST(ChunkSync, AWindowStepVisitsOnlyItsEnteringSlab) {
    Flat world;                                     // only asked whether a chunk is loaded (none of these is)
    const glm::ivec3 size(48, 44, 48);
    const int64_t cells = static_cast<int64_t>(size.x) * size.y * size.z;
    net::ChunkSync sync;
    sync.Start(0, net::SyncBox{{1000, -12, 1000}, {200, 44, 200}});
    net::SyncBox w{{1024, -12, 1024}, size};
    uint32_t seq = 0;
    auto step = [&](const glm::ivec3& by) {
        w.min += by;
        sync.BeginWindow(w, ++seq, false);
        net::WindowAck ack;
        ack.windowSeq = seq;
        ack.asOfOpSeq = seq * 10;
        ack.modified = voxel::VoxelMask(size);
        ASSERT_TRUE(sync.OnAck(*world.world, ack, seq * 10));
    };
    step(glm::ivec3(0));
    EXPECT_EQ(sync.Stats().recordsVisited, static_cast<uint64_t>(2 * cells)) << "the first window: every record";
    uint64_t before = sync.Stats().recordsVisited;
    EXPECT_TRUE(sync.TakeRequests(5000, 64, w.min, 256).empty());
    EXPECT_EQ(sync.Stats().recordsVisited, before) << "nothing Unsynced: the table is not walked";
    step(glm::ivec3(1, 0, 0));
    EXPECT_EQ(sync.Stats().recordsVisited - before, static_cast<uint64_t>(2 * size.y * size.z)) << "one slab, twice (update and ack)";
    before = sync.Stats().recordsVisited;
    step(glm::ivec3(-1, 0, 1));
    EXPECT_EQ(sync.Stats().recordsVisited - before, static_cast<uint64_t>(2 * size.y * (size.x + size.z - 1)));
    // The records are right: the entering slab is based on its ack, a cell that stayed keeps its base.
    EXPECT_EQ(sync.StateOf(w.min + glm::ivec3(0, 0, size.z - 1)), net::ChunkSyncState::Based);
    EXPECT_EQ(sync.BaseOf(w.min + glm::ivec3(0, 0, size.z - 1)), 30u);
    EXPECT_EQ(sync.BaseOf(w.min + glm::ivec3(5, 3, 5)), 10u);
    EXPECT_EQ(sync.BaseOf(w.min + glm::ivec3(size.x - 1, 0, 0)), 10u);
    EXPECT_EQ(sync.Counts().noBase, 0u);
    EXPECT_EQ(sync.Counts().basedNotReady, static_cast<uint32_t>(cells));
    world.jobs.WaitIdle();
}
