// docs/design/networking.md stage 3: net::Client against a hand-written server endpoint on a raw
// loopback, so that the client meets what a real server never sends in the other tests: overlapping
// batches (an ENet resend after a lost ack), a gap in the stream, a forged Welcome, and events that
// are the join log rather than live.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    constexpr int kGround = 96;

    net::Token Token() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x11 * (i + 1));
        return t;
    }

    voxel::WorldMaterials Ids() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    /// 4 x 4 chunks of play at 0.125 m, flat ground at 12 m: 10 x 8 x 10 chunks of window.
    sim::RegionDesc SmallRegion() {
        sim::RegionDesc r;
        r.seed = 99;
        r.playMinChunk = {-2, -2};
        r.playMaxChunk = {2, 2};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = kGround;
        r.terrain.voxelSize = 0.125f;
        r.terrain.materials = Ids();
        return r;
    }

    /// Event i (1-based): a one-voxel gold box on the ground, each at its own place.
    sim::WorldOp BoxEvent(uint32_t i) {
        const glm::ivec3 p(static_cast<int>(i) * 2, kGround, 3);
        return sim::WorldOp::Box(p, p + glm::ivec3(1), Ids().gold);
    }

    /// Event i: a small carve into the ground (something to present).
    sim::WorldOp CarveEvent(uint32_t i) {
        const glm::ivec3 centre(static_cast<int>(i) * 12 * 16, (kGround - 2) * 16, 8 * 16);
        return sim::WorldOp::Carve(voxel::SphereQ{centre, 4 * 16}, 1600);
    }

    /// A server endpoint written by hand: the test decides every byte the client receives.
    struct FakeServer {
        explicit FakeServer(net::ClientSettings settings = {}) {
            settings.token = Token();
            client = std::make_unique<net::Client>(settings, clientEnd);
            peer = net::LoopbackTransport::Connect(serverEnd, clientEnd);
            client->Poll();                       // Connected -> Hello
            net::TransportEvent e;
            while (serverEnd.Poll(e)) {}          // Connected, Hello
            base.jobThreads = 1;
            base.physics.threads = 1;
            base.physics.maxBodies = 256;
            base.physics.maxBodyPairs = 512;
            base.physics.maxContactConstraints = 512;
        }

        net::Welcome MakeWelcome(uint32_t joinOpSeq = 0) const {
            net::Welcome w;
            w.region = SmallRegion();
            w.joinOpSeq = joinOpSeq;
            w.probes = net::ProbeChunks(w.region, *sim::MakeRegionGenerator(w.region, Ids()));
            return w;
        }

        void Send(const std::vector<uint8_t>& bytes) { serverEnd.Send(peer, net::Channel::Events, bytes); }
        void SendBulk(const std::vector<uint8_t>& bytes) { serverEnd.Send(peer, net::Channel::Bulk, bytes); }

        /// The join of stage 7a: Welcome, the ack of the client's first WindowUpdate (the region's
        /// window; `modified` chunks will come as ChunkData, the rest are pristine at `asOf`), and
        /// optionally the end of an empty entity baseline.
        void Join(const net::Welcome& w, bool baselineEnd = true, const std::vector<glm::ivec3>& modified = {}) {
            Send(net::Encode(w));
            Ack(w.joinOpSeq, modified);
            if (baselineEnd) SendBulk(net::Encode(net::BaselineEnd{w.joinOpSeq, 0}));
        }
        void Ack(uint32_t asOf, const std::vector<glm::ivec3>& modified = {}, uint32_t windowSeq = 1) {
            const net::SyncBox box = net::RegionSyncBox(SmallRegion());
            net::WindowAck ack;
            ack.windowSeq = windowSeq;
            ack.asOfOpSeq = asOf;
            ack.modified = voxel::VoxelMask(box.size);
            for (const glm::ivec3& c : modified) ack.modified.Set(c - box.min, true);
            Send(net::Encode(ack));
        }
        /// `chunk` as `world` has it, on the bulk channel.
        void SendChunk(const voxel::World& world, const glm::ivec3& chunk, uint32_t asOf) {
            net::ChunkData d;
            d.chunk = chunk;
            d.asOfOpSeq = asOf;
            EXPECT_TRUE(world.EncodeChunkAt(chunk, d.rle));
            SendBulk(net::Encode(d));
        }
        /// Reads what the client sent on the events channel: requests are counted per coordinate / id.
        void ReadRequests() {
            net::TransportEvent e;
            while (serverEnd.Poll(e)) {
                if (e.type == net::TransportEvent::Type::Disconnected) left = true;
                if (e.type != net::TransportEvent::Type::Received || e.channel != net::Channel::Events) continue;
                net::ChunkRequest cr;
                net::ModelRequest mr;
                if (net::Decode(e.data, cr)) chunkRequests.insert(chunkRequests.end(), cr.chunks.begin(), cr.chunks.end());
                if (net::Decode(e.data, mr)) modelRequests.insert(modelRequests.end(), mr.netIds.begin(), mr.netIds.end());
            }
        }

        /// Events first..last as one batch.
        void SendBatch(uint32_t first, uint32_t last, sim::WorldOp (*make)(uint32_t) = BoxEvent) {
            net::EventBatch batch;
            batch.firstOpSeq = first;
            for (uint32_t i = first; i <= last; ++i) batch.events.push_back(make(i));
            Send(net::Encode(batch));
        }

        bool Attach() {
            client->Poll();
            if (client->State() != net::ClientState::Welcomed) return false;
            simulation = std::make_unique<sim::Simulation>(client->MakeSimulationConfig(base));
            if (!simulation->Initialize(client->MakeGenerator())) return false;
            return client->Attach(*simulation);
        }

        /// True once the client has closed the connection (the server end saw Disconnected).
        bool ClientLeft() {
            net::TransportEvent e;
            while (serverEnd.Poll(e))
                if (e.type == net::TransportEvent::Type::Disconnected) left = true;
            return left;
        }

        net::LoopbackTransport serverEnd, clientEnd;
        std::unique_ptr<net::Client> client;
        net::PeerId peer = 0;
        sim::SimulationConfig base;
        std::unique_ptr<sim::Simulation> simulation;
        bool left = false;
        std::vector<glm::ivec3> chunkRequests;
        std::vector<uint32_t> modelRequests;
    };

    /// A world with events 1..count applied once each (a server's, for chunk data).
    std::unique_ptr<FakeServer> ReferenceWorld(uint32_t count, sim::WorldOp (*make)(uint32_t) = BoxEvent) {
        auto ref = std::make_unique<FakeServer>();
        ref->Join(ref->MakeWelcome());
        EXPECT_TRUE(ref->Attach());
        for (uint32_t i = 1; i <= count; ++i) sim::ApplyOp(ref->simulation->World(), make(i));
        return ref;
    }

    /// The hash of the region after events 1..count applied once each, on a fresh world.
    uint64_t ReferenceHash(uint32_t count, sim::WorldOp (*make)(uint32_t) = BoxEvent) {
        FakeServer ref;
        ref.Join(ref.MakeWelcome());
        EXPECT_TRUE(ref.Attach());
        for (uint32_t i = 1; i <= count; ++i) sim::ApplyOp(ref.simulation->World(), make(i));
        return net::RegionHash(ref.simulation->World(), SmallRegion()).hash;
    }
}

// An overlapping batch (events 5..9 after 1..7, as an ENet resend after a lost ack would give) is
// applied from 8 on; batches that arrive before Attach are kept and applied by it.
TEST(Client, OverlappingBatchesApplyEachEventOnce) {
    FakeServer s;
    s.Join(s.MakeWelcome());
    s.SendBatch(1, 7);
    s.SendBatch(5, 9);
    ASSERT_TRUE(s.Attach());
    EXPECT_EQ(s.client->AppliedOpSeq(), 9u);
    EXPECT_EQ(s.client->Stats().duplicateEvents, 3u);
    EXPECT_EQ(s.client->Stats().opsApplied, 9u);
    s.SendBatch(9, 10);                          // one more duplicate, live
    s.client->Poll();
    EXPECT_EQ(s.client->State(), net::ClientState::Playing);
    EXPECT_EQ(s.client->AppliedOpSeq(), 10u);
    EXPECT_EQ(s.client->Stats().duplicateEvents, 4u);
    EXPECT_EQ(s.client->Stats().opsApplied, 10u);
    EXPECT_EQ(s.client->Hash().hash, ReferenceHash(10));
}

// A gap fails the client, and a failed client leaves the session (its slot is freed on the server).
TEST(Client, AGapInTheStreamFailsTheClientAndItLeaves) {
    FakeServer s;
    s.Join(s.MakeWelcome());
    s.SendBatch(1, 3);
    ASSERT_TRUE(s.Attach());
    s.SendBatch(5, 6);                           // 4 is missing
    s.client->Poll();
    EXPECT_EQ(s.client->State(), net::ClientState::Failed);
    EXPECT_EQ(s.client->AppliedOpSeq(), 3u);
    EXPECT_TRUE(s.ClientLeft());
}

// Welcome is network input: probe chunks outside the region's window (or too many of them) are
// refused before any generator runs on them.
TEST(Client, AWelcomeWithProbesOutsideTheWindowIsRefused) {
    {
        FakeServer s;
        net::Welcome w = s.MakeWelcome();
        w.probes[3].chunk = glm::ivec3(1 << 30, 0, 0);
        s.Send(net::Encode(w));
        s.client->Poll();
        EXPECT_EQ(s.client->State(), net::ClientState::Failed);
        EXPECT_TRUE(s.ClientLeft());
    }
    {
        FakeServer s;
        net::Welcome w = s.MakeWelcome();
        w.probes[0].chunk.y = sim::RegionRows(w.region).y;        // one row above the window
        s.Send(net::Encode(w));
        s.client->Poll();
        EXPECT_EQ(s.client->State(), net::ClientState::Failed);
    }
    {
        FakeServer s;
        net::Welcome w = s.MakeWelcome();
        while (w.probes.size() <= net::kProbeChunks) w.probes.push_back(w.probes[0]);
        s.Send(net::Encode(w));
        s.client->Poll();
        EXPECT_EQ(s.client->State(), net::ClientState::Failed);
    }
    {
        FakeServer s;                             // the unforged one is taken
        s.Join(s.MakeWelcome());
        s.client->Poll();
        EXPECT_EQ(s.client->State(), net::ClientState::Welcomed);
        EXPECT_EQ(s.client->Stats().probeMismatches, 0u);
    }
}

// Stage 7a: a joiner's stream starts after its join point J. What was made before reaches it as
// chunk data (the ack marks the chunks ops 1..3 changed; their ChunkData follows on bulk); only events
// after J are presented, and the world equals the server's.
TEST(Client, ALateJoinerPresentsOnlyEventsAfterItsJoinPoint) {
    std::unique_ptr<FakeServer> ref = ReferenceWorld(3, CarveEvent);
    std::vector<glm::ivec3> changed;
    for (uint32_t i = 1; i <= 3; ++i) {
        glm::ivec3 lo, hi;
        ASSERT_TRUE(sim::OpChunkRange(CarveEvent(i), lo, hi));
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) changed.push_back({x, y, z});
    }
    net::ClientSettings presenting;
    presenting.presentation = true;
    FakeServer s(presenting);
    s.Join(s.MakeWelcome(3), true, changed);
    for (const glm::ivec3& c : changed) s.SendChunk(ref->simulation->World(), c, 3);
    s.SendBatch(1, 3, CarveEvent);               // nothing at or before J is ever sent: duplicates if it were
    ASSERT_TRUE(s.Attach());
    EXPECT_EQ(s.client->AppliedOpSeq(), 3u);
    EXPECT_EQ(s.client->Stats().duplicateEvents, 3u);
    EXPECT_TRUE(s.client->TakePresentations().empty());
    EXPECT_TRUE(s.client->Synced());
    s.SendBatch(4, 4, CarveEvent);
    s.client->Poll();
    const std::vector<sim::BlastResult> live = s.client->TakePresentations();
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0].ops.size(), 1u);
    EXPECT_EQ(s.client->Hash().hash, ReferenceHash(4, CarveEvent));
    EXPECT_TRUE(s.client->Hash().Complete());
}

// The world the client is given must be the one its generator makes, untouched: a simulation built
// with another generator, or one already edited, is refused (its probe chunks differ).
TEST(Client, AttachRefusesAWorldItsGeneratorDidNotMake) {
    {
        FakeServer s;
        s.Join(s.MakeWelcome());
        s.client->Poll();
        ASSERT_EQ(s.client->State(), net::ClientState::Welcomed);
        sim::Simulation other(s.client->MakeSimulationConfig(s.base));
        ASSERT_TRUE(other.Initialize(voxel::CreateFlatGenerator(kGround + 40, Ids().grass, Ids().stone)));
        EXPECT_FALSE(s.client->Attach(other));
        EXPECT_EQ(s.client->State(), net::ClientState::Welcomed);
    }
    {
        FakeServer s;
        s.Join(s.MakeWelcome());
        s.client->Poll();
        ASSERT_EQ(s.client->State(), net::ClientState::Welcomed);
        sim::Simulation edited(s.client->MakeSimulationConfig(s.base));
        ASSERT_TRUE(edited.Initialize(s.client->MakeGenerator()));
        const sim::RegionDesc region = SmallRegion();
        edited.World().LoadBlocking(sim::RegionWindowFocus(region));
        // Gold over every chunk of the rows that hold the ground and the air above it.
        const glm::ivec3 min = voxel::ChunkToVoxel(sim::RegionWindowMinChunk(region));
        const glm::ivec3 size = sim::RegionWindowSize(region) * voxel::kChunkSize;
        for (int y = 0; y < 8 * voxel::kChunkSize; y += 16)
            sim::ApplyOp(edited.World(), sim::WorldOp::Box(glm::ivec3(min.x, y, min.z), glm::ivec3(min.x + size.x, y + 1, min.z + size.z),
                                                           Ids().gold));
        EXPECT_FALSE(s.client->Attach(edited));
        sim::Simulation right(s.client->MakeSimulationConfig(s.base));
        ASSERT_TRUE(right.Initialize(s.client->MakeGenerator()));
        EXPECT_TRUE(s.client->Attach(right));
    }
}

// The region window must fit the client's store: a store that fills while the window loads is
// refused, and the session's own config sizes the store for the region.
TEST(Client, AttachRefusesAStoreTooSmallForTheRegion) {
    FakeServer s;
    s.Join(s.MakeWelcome());
    s.client->Poll();
    ASSERT_EQ(s.client->State(), net::ClientState::Welcomed);
    sim::SimulationConfig small = s.client->MakeSimulationConfig(s.base);
    small.chunkCapacity = 32;                    // the ground layer alone is 100 dense chunks
    sim::Simulation cramped(small);
    ASSERT_TRUE(cramped.Initialize(s.client->MakeGenerator()));
    EXPECT_FALSE(s.client->Attach(cramped));
    EXPECT_EQ(s.client->State(), net::ClientState::Welcomed);

    sim::SimulationConfig tiny = s.base;
    tiny.chunkCapacity = 16;                     // whatever the caller asked for, the region's need wins
    EXPECT_GE(s.client->MakeSimulationConfig(tiny).chunkCapacity, sim::RegionChunkCapacity(SmallRegion()));
    ASSERT_TRUE(s.Attach());
    EXPECT_EQ(s.client->State(), net::ClientState::Playing);
}

// Review of stage 5: Attach polls the transport while the region loads (the game and the bots do),
// so the session can end during the load. A server that disconnects, or a malformed batch that fails
// the client, must leave the client Closed / Failed and Attach must say so; before the fix Attach
// set Playing whatever happened meanwhile and returned true.
TEST(Client, ASessionThatEndsWhileTheRegionLoadsIsNotAttached) {
    for (const bool disconnect : {true, false}) {
        FakeServer s;
        s.Join(s.MakeWelcome());
        s.client->Poll();
        ASSERT_EQ(s.client->State(), net::ClientState::Welcomed);
        sim::Simulation simulation(s.client->MakeSimulationConfig(s.base));
        ASSERT_TRUE(simulation.Initialize(s.client->MakeGenerator()));
        uint32_t calls = 0;
        const bool attached = s.client->Attach(simulation, [&] {
            if (calls++ == 0) {
                if (disconnect) {
                    s.serverEnd.Disconnect(s.peer);                       // the server is stopped
                } else {
                    std::vector<uint8_t> bytes = net::Encode(net::EventBatch{});
                    bytes.push_back(0xEE);                                // trailing bytes: malformed
                    s.Send(bytes);
                }
            }
            s.client->Poll();
        });
        EXPECT_GT(calls, 0u) << "the load ended before the callback ran: the test proves nothing";
        EXPECT_FALSE(attached) << (disconnect ? "disconnect" : "malformed batch");
        EXPECT_EQ(s.client->State(), disconnect ? net::ClientState::Closed : net::ClientState::Failed);
        s.client->Poll();                                                 // nothing turns it into Playing later
        EXPECT_NE(s.client->State(), net::ClientState::Playing);
    }
}

// Review of stage 5: the engine runs at most 8 ticks a frame. The client clock must keep what it
// cannot run now for the next frames (TicksDue's cap), not hand out 30 and have the caller drop
// the rest: 0.3 s at 60 Hz is 18 ticks, as 8 + 8 + 2.
TEST(Client, TicksAboveTheCallersCapAreCarriedToTheNextFrames) {
    FakeServer s;
    s.Join(s.MakeWelcome());
    s.client->Poll();
    ASSERT_EQ(s.client->State(), net::ClientState::Welcomed);
    EXPECT_EQ(s.client->TicksDue(0.3 + 1.0e-6, 8), 8u);
    EXPECT_EQ(s.client->TicksDue(0.0, 8), 8u);
    EXPECT_EQ(s.client->TicksDue(0.0, 8), 2u);
    EXPECT_EQ(s.client->TicksDue(0.0, 8), 0u);
}

// Stage 6, the rest rule (networking.md "Entity replication": "samples with tick <= restTick are
// dropped; rest ends only on a newer sample with the awake flag"): a copy that came to rest stays at
// the EntityRest pose, in Layer::Dynamic (players collide with it), when a snapshot from before the
// rest arrives after it (the state channel is unordered against the events); a newer sample starts it
// moving again (Layer::Rubble: no predicted player meets it).
TEST(Client, ALateSnapshotAfterAnEntityRestKeepsTheRestPose) {
    FakeServer s;
    s.Join(s.MakeWelcome());
    ASSERT_TRUE(s.Attach());
    voxel::VoxelVolume crate(glm::ivec3(4));
    crate.FillBox(glm::ivec3(0), glm::ivec3(4), Ids().planks);
    net::EntitySpawn spawn;
    spawn.netId = 5;
    spawn.kind = net::EntityKind::Object;
    spawn.object.position = glm::vec3(1.0f, 13.0f, 1.0f);
    spawn.object.volume = crate;
    net::EventBatch batch;
    batch.firstOpSeq = 1;
    batch.events = {spawn};
    s.Send(net::Encode(batch));
    auto snapshot = [&](uint32_t tick, const glm::vec3& at) {
        net::Snapshot snap;
        snap.tick = tick;
        snap.lastOpSeq = s.client->AppliedOpSeq();
        net::BodySample b;
        b.netId = 5;
        b.position = at;
        b.velocity = glm::vec3(0.0f, -1.0f, 0.0f);
        snap.bodies = {b};
        s.serverEnd.Send(s.peer, net::Channel::State, net::Encode(snap));
        s.client->Poll();
    };
    auto layer = [&] {
        for (const auto [entity, body] : s.simulation->Scene().Registry().view<const scene::RigidBody>().each())
            return s.simulation->Physics().GetLayer(body.body);
        return physics::Layer::Static;
    };
    s.client->Poll();
    ASSERT_EQ(s.client->Entities().size(), 1u);
    EXPECT_FALSE(s.client->Entities()[0].resting);
    EXPECT_EQ(layer(), physics::Layer::Rubble);
    snapshot(10, glm::vec3(1.0f, 12.8f, 1.0f));
    EXPECT_EQ(s.client->Stats().bodySamples, 1u);

    const glm::vec3 restAt(1.5f, 12.0f, 1.25f);
    const glm::quat restRotation = glm::normalize(glm::quat(0.9f, 0.0f, 0.3f, 0.0f));
    batch.firstOpSeq = 2;
    batch.events = {net::EntityRest{5, 20, restAt, restRotation}};
    s.Send(net::Encode(batch));
    s.client->Poll();
    ASSERT_EQ(s.client->Entities().size(), 1u);
    EXPECT_TRUE(s.client->Entities()[0].resting);
    EXPECT_EQ(s.client->Entities()[0].position, restAt);
    EXPECT_NEAR(std::abs(glm::dot(s.client->Entities()[0].rotation, restRotation)), 1.0f, 1.0e-6f);
    EXPECT_EQ(layer(), physics::Layer::Dynamic);

    snapshot(18, glm::vec3(4.0f, 15.0f, 4.0f));                  // late: from before the rest
    for (int i = 0; i < 10; ++i) s.client->RunTick({});
    EXPECT_EQ(s.client->Stats().bodySamplesIgnored, 1u);
    EXPECT_TRUE(s.client->Entities()[0].resting);
    EXPECT_EQ(s.client->Entities()[0].position, restAt);          // bit for bit
    EXPECT_EQ(s.client->Entities()[0].rotation, restRotation);
    EXPECT_EQ(layer(), physics::Layer::Dynamic);

    // Woken on the server (review of stage 6: waking is an event, as resting is).
    batch.firstOpSeq = 3;
    batch.events = {net::EntityWake{5, 21}};
    s.Send(net::Encode(batch));
    s.client->Poll();
    EXPECT_FALSE(s.client->Entities()[0].resting);
    EXPECT_EQ(layer(), physics::Layer::Rubble);
    EXPECT_EQ(s.client->Stats().wakes, 1u);
    snapshot(22, glm::vec3(1.5f, 12.5f, 1.25f));
    EXPECT_EQ(s.client->Stats().bodySamples, 2u);

    // Gone: the copy is removed and its NetId tombstoned (a later spawn of it is ignored).
    batch.firstOpSeq = 4;
    batch.events = {net::EntityDespawn{5, scene::DespawnReason::Destroyed}, spawn};
    s.Send(net::Encode(batch));
    s.client->Poll();
    EXPECT_TRUE(s.client->Entities().empty());
    EXPECT_EQ(s.client->Stats().despawns, 1u);
    EXPECT_EQ(s.client->Stats().ignoredEntityEvents, 1u);
    EXPECT_EQ(s.simulation->Scene().VoxelObjectCount(), 0u);
}

namespace {
    /// The FakeServer side of a replicated object: a 4^3 planks copy (netId 5) at `at`, moving.
    net::EntitySpawn CrateSpawn(const glm::vec3& at, uint32_t netId = 5) {
        voxel::VoxelVolume crate(glm::ivec3(4));
        crate.FillBox(glm::ivec3(0), glm::ivec3(4), Ids().planks);
        net::EntitySpawn spawn;
        spawn.netId = netId;
        spawn.kind = net::EntityKind::Object;
        spawn.object.position = at;
        spawn.object.volume = crate;
        return spawn;
    }

    std::vector<uint8_t> BodySnapshot(uint32_t tick, uint32_t lastOpSeq, uint32_t netId, const glm::vec3& at) {
        net::Snapshot snap;
        snap.tick = tick;
        snap.lastOpSeq = lastOpSeq;
        net::BodySample b;
        b.netId = netId;
        b.position = at;
        snap.bodies = {b};
        return net::Encode(snap);
    }

    physics::Layer CopyLayer(const sim::Simulation& simulation) {
        for (const auto [entity, body] : simulation.Scene().Registry().view<const scene::RigidBody>().each())
            return const_cast<sim::Simulation&>(simulation).Physics().GetLayer(body.body);
        return physics::Layer::Static;
    }
}

// Review of stage 6: a snapshot's body samples are taken only after the events it was built after
// (Snapshot::lastOpSeq), as the owner block is compared. The order that broke it: a body rested at
// tick 10, woke, rested at 20 and woke at 21 on the server; one poll brings the snapshot of tick 22
// (sent unreliably, so ahead of the reliable batch: a resend, head-of-line blocking, a late joiner)
// and then the batch with those events. Taken at once, the sample would land before the stale rest,
// which then parked the copy at the pose of tick 20, solid (Layer::Dynamic), while the server's body
// moves (Layer::Rubble). Now the sample waits, and the copy ends moving and follows it.
TEST(Client, ASnapshotsSamplesWaitForItsEvents) {
    FakeServer s;
    s.Join(s.MakeWelcome());
    ASSERT_TRUE(s.Attach());
    net::EventBatch batch;
    batch.firstOpSeq = 1;
    batch.events = {CrateSpawn(glm::vec3(1.0f, 13.0f, 1.0f)), net::EntityRest{5, 10, glm::vec3(1.0f, 12.0f, 1.0f), glm::quat(1, 0, 0, 0)}};
    s.Send(net::Encode(batch));
    s.client->Poll();
    ASSERT_EQ(s.client->Entities().size(), 1u);
    ASSERT_TRUE(s.client->Entities()[0].resting);

    // One poll: the snapshot of tick 22 (lastOpSeq 5) first, then its events.
    const glm::vec3 moving(2.0f, 12.0f, 1.0f);
    s.serverEnd.Send(s.peer, net::Channel::State, BodySnapshot(22, 5, 5, moving));
    batch.firstOpSeq = 3;
    batch.events = {net::EntityWake{5, 12}, net::EntityRest{5, 20, glm::vec3(1.5f, 12.0f, 1.0f), glm::quat(1, 0, 0, 0)},
                    net::EntityWake{5, 21}};
    s.Send(net::Encode(batch));
    s.client->Poll();
    EXPECT_EQ(s.client->AppliedOpSeq(), 5u);
    EXPECT_EQ(s.client->Stats().bodySamples, 1u);                 // taken after the events
    EXPECT_EQ(s.client->Stats().staleRests, 0u);
    ASSERT_EQ(s.client->Entities().size(), 1u);
    EXPECT_FALSE(s.client->Entities()[0].resting);
    EXPECT_EQ(CopyLayer(*s.simulation), physics::Layer::Rubble);
    for (int i = 0; i < 6; ++i) s.client->RunTick({});
    EXPECT_LT(glm::distance(s.client->Entities()[0].position, moving), 1.0e-3f);

    // A snapshot whose events have not come waits for them: no sample yet, then one.
    s.serverEnd.Send(s.peer, net::Channel::State, BodySnapshot(26, 6, 5, moving + glm::vec3(0.5f, 0.0f, 0.0f)));
    s.client->Poll();
    EXPECT_EQ(s.client->Stats().bodySamples, 1u);
    batch.firstOpSeq = 6;
    batch.events = {sim::WorldOp::Box(glm::ivec3(0, kGround, 0), glm::ivec3(1, kGround + 1, 1), Ids().gold)};
    s.Send(net::Encode(batch));
    s.client->Poll();
    EXPECT_EQ(s.client->Stats().bodySamples, 2u);
}

// Review of stage 6: an ObjectModel and the snapshot after it in one poll. The server's survivor is
// a new body at the shifted origin (Scene::Damage), awake; its sample in the snapshot is already in
// the new frame. Taken before the ObjectModel it was shifted a second time (the copy drawn the
// offset away); and a resting copy was re-made resting, solid where the server's body moves. Now the
// sample waits for the model, the copy is re-made moving, and it stays on the sample.
TEST(Client, AnObjectModelAndItsSnapshotInOnePollKeepTheCopyOnTheSample) {
    FakeServer s;
    s.Join(s.MakeWelcome());
    ASSERT_TRUE(s.Attach());
    const glm::vec3 restAt(1.0f, 12.0f, 1.0f);
    net::EventBatch batch;
    batch.firstOpSeq = 1;
    batch.events = {CrateSpawn(glm::vec3(1.0f, 13.0f, 1.0f)), net::EntityRest{5, 8, restAt, glm::quat(1, 0, 0, 0)}};
    s.Send(net::Encode(batch));
    s.client->Poll();
    ASSERT_TRUE(s.client->Entities()[0].resting);

    // The damage at tick 12 took the crate's -x half: the survivor's origin moved 2 voxels in +x.
    net::ObjectModel model;
    model.netId = 5;
    model.offset = glm::ivec3(2, 0, 0);
    model.motion = static_cast<uint8_t>(physics::MotionType::Dynamic);
    model.volume = voxel::VoxelVolume(glm::ivec3(2, 4, 4));
    model.volume.FillBox(glm::ivec3(0), glm::ivec3(2, 4, 4), Ids().planks);
    const glm::vec3 shifted = restAt + glm::vec3(2.0f * 0.125f, 0.0f, 0.0f);
    s.serverEnd.Send(s.peer, net::Channel::State, BodySnapshot(12, 4, 5, shifted));
    batch.firstOpSeq = 3;
    batch.events = {model, net::EntityWake{5, 12}};
    s.Send(net::Encode(batch));
    s.client->Poll();
    EXPECT_EQ(s.client->Stats().models, 1u);
    EXPECT_EQ(s.client->Stats().bodySamples, 1u);
    ASSERT_EQ(s.client->Entities().size(), 1u);
    EXPECT_FALSE(s.client->Entities()[0].resting);
    EXPECT_EQ(s.client->Entities()[0].modelHash, net::VolumeHash(model.volume));
    EXPECT_EQ(CopyLayer(*s.simulation), physics::Layer::Rubble);
    for (int i = 0; i < 6; ++i) {
        (void)s.client->TicksDue(1.0 / 60.0);
        s.client->RunTick({});
    }
    EXPECT_LT(glm::distance(s.client->Entities()[0].position, shifted), 1.0e-3f);

    // Without a wake (a static model is the only survivor that may rest), a moving model alone is
    // re-made moving too: the server's survivor is always a new, awake body.
    batch.firstOpSeq = 5;
    batch.events = {net::EntityRest{5, 30, shifted, glm::quat(1, 0, 0, 0)}, model};
    s.Send(net::Encode(batch));
    s.client->Poll();
    EXPECT_FALSE(s.client->Entities()[0].resting);
    EXPECT_EQ(CopyLayer(*s.simulation), physics::Layer::Rubble);
}

// Review of stage 6 (seen in the drill's late joiner): a joiner starts predicting from the newest
// snapshot whose events it has applied. After a long, paced catch-up of the join log that snapshot was
// tens of ticks behind the newest one; a prediction started there sent inputs that reached the server
// 50 ticks late, the clock jumped, overshot and held (a hard resync, 27 corrections and a 5 m snap).
// Now a joiner waits for a snapshot close to the newest one (ClientSettings::startFreshTicks).
TEST(Client, AJoinerStartsPredictingFromAFreshSnapshot) {
    FakeServer s;
    s.Join(s.MakeWelcome());
    ASSERT_TRUE(s.Attach());
    auto snapshot = [&](uint32_t tick, uint32_t lastOpSeq) {
        net::Snapshot snap;
        snap.tick = tick;
        snap.lastOpSeq = lastOpSeq;
        snap.hasOwner = true;
        snap.owner.position = glm::vec3(s.client->Spawn());
        s.serverEnd.Send(s.peer, net::Channel::State, net::Encode(snap));
    };
    // Snapshots of ticks 2..60, one event every two ticks; the events lag.
    for (uint32_t t = 2; t <= 60; t += 2) {
        snapshot(t, t / 2);
        s.client->RunTick({});
    }
    EXPECT_FALSE(s.client->Predicting());
    s.SendBatch(1, 10);                         // the snapshots up to tick 20 are ready: 40 ticks old
    s.client->RunTick({});
    EXPECT_FALSE(s.client->Predicting());
    EXPECT_GT(s.client->Prediction().staleStarts, 0u);
    s.SendBatch(11, 30);                        // caught up: the newest is ready
    s.client->RunTick({});
    ASSERT_TRUE(s.client->Predicting());
    EXPECT_GT(s.client->PredictTick(), 60u);
}

// Stage 7a, review case of the plan (amendment 8): the start gate (chunks within 32 m current, the
// entity baseline ended) is evaluated before the fresh-snapshot rule, and while it is closed the
// waiting snapshots are dropped, so a gate closed for longer than startWaitTicks does not let a stale
// snapshot start the player when it opens.
TEST(Client, AJoinerHeldByTheStartGateStartsFromAFreshSnapshot) {
    FakeServer s;
    s.Join(s.MakeWelcome(), false);              // the baseline has not ended
    ASSERT_TRUE(s.Attach());
    auto snapshot = [&](uint32_t tick) {
        net::Snapshot snap;
        snap.tick = tick;
        snap.hasOwner = true;
        snap.owner.position = glm::vec3(s.client->Spawn());
        s.serverEnd.Send(s.peer, net::Channel::State, net::Encode(snap));
    };
    uint32_t t = 2;
    for (; t <= 300; t += 2) {                   // 150 snapshots: longer than startWaitTicks (120)
        snapshot(t);
        s.client->RunTick({});
    }
    EXPECT_FALSE(s.client->Predicting());
    EXPECT_GT(s.client->Prediction().startGateDrops, 100u);
    s.SendBulk(net::Encode(net::BaselineEnd{0, 0}));
    snapshot(t);
    s.client->RunTick({});
    ASSERT_TRUE(s.client->Predicting());
    EXPECT_GT(s.client->PredictTick(), t) << "started from a stale snapshot";
    EXPECT_EQ(s.client->Prediction().staleStarts, 0u);
}

namespace {
    /// 1: a netId-0 Detach (client rubble), 2: the Detach of server body 9; both of 6 x 4 x 6 stone
    /// voxels of chunk (0, 2, 0).
    sim::WorldOp DetachEvent(uint32_t i) {
        voxel::VoxelMask m(glm::ivec3(6, 4, 6));
        for (int z = 0; z < 6; ++z)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 6; ++x) m.Set(x, y, z, true);
        return sim::WorldOp::Detach(i == 1 ? glm::ivec3(4, 90, 4) : glm::ivec3(14, 90, 14), m, glm::vec3(0.0f), i == 2 ? 9u : 0u);
    }

    /// A sphere half in the ground, half in the air, at (20, 96, 20) voxels: chunks (0, 2..3, 0).
    voxel::SphereQ GroundSphere() { return voxel::SphereQ{glm::ivec3(20, 96, 20) * 16 + glm::ivec3(8), 5 * 16}; }
    /// 1: stone -> gold, 2: air -> stone, on the same voxels. The pair is not idempotent: applying
    /// 1..2 again on the result turns the former air into gold (where the reference has stone).
    sim::WorldOp SwapPair(uint32_t i) {
        return i == 1 ? sim::WorldOp::Swap(GroundSphere(), Ids().stone, Ids().gold) : sim::WorldOp::Swap(GroundSphere(), 0, Ids().stone);
    }
    std::vector<glm::ivec3> RangeOf(const sim::WorldOp& op) {
        std::vector<glm::ivec3> out;
        glm::ivec3 lo, hi;
        if (!sim::OpChunkRange(op, lo, hi)) return out;
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) out.push_back({x, y, z});
        return out;
    }
}

// Stage 7a (networking.md "Testing": bulk delivered ahead of events with a pair on the same voxels;
// plan amendment 4): chunk data and events reach a client in either order and the base rule makes the
// world equal: data as of 2 before the batch 1..2 (both events skipped for those chunks: seq <= base),
// or the batch before data as of 1 (installed, then op 2 replayed from the ring).
TEST(Client, BulkAheadOfEventsWithASwapPairOnTheSameVoxels) {
    std::unique_ptr<FakeServer> at1 = ReferenceWorld(1, SwapPair), at2 = ReferenceWorld(2, SwapPair);
    const uint64_t expect = net::RegionHash(at2->simulation->World(), SmallRegion()).hash;
    ASSERT_NE(expect, net::RegionHash(at1->simulation->World(), SmallRegion()).hash);
    const std::vector<glm::ivec3> chunks = RangeOf(SwapPair(1));
    {
        FakeServer s;                              // data as of 2 first, then the events
        s.Join(s.MakeWelcome());
        ASSERT_TRUE(s.Attach());
        for (const glm::ivec3& c : chunks) s.SendChunk(at2->simulation->World(), c, 2);
        s.client->Poll();
        s.SendBatch(1, 2, SwapPair);
        s.client->Poll();
        EXPECT_EQ(s.client->Hash().hash, expect);
        for (const glm::ivec3& c : chunks) {
            if (!sim::OpTouchesChunk(SwapPair(1), c)) continue;
            EXPECT_EQ(s.client->Sync().SkippedAtOrBelowBase(c), 2u) << "an event at or below its chunk's base was applied";
            EXPECT_EQ(s.client->Sync().AppliedOps(c), 0u);
        }
        EXPECT_TRUE(s.client->Synced());
    }
    {
        FakeServer s;                              // the events first, then data as of 1
        s.Join(s.MakeWelcome());
        ASSERT_TRUE(s.Attach());
        s.SendBatch(1, 2, SwapPair);
        s.client->Poll();
        for (const glm::ivec3& c : chunks) s.SendChunk(at1->simulation->World(), c, 1);
        s.client->Poll();
        EXPECT_EQ(s.client->Hash().hash, expect);
        EXPECT_GT(s.client->Sync().Stats().replays, 0u);
        EXPECT_TRUE(s.client->Synced());
    }
}

// Stage 7a (networking.md "Testing": a client with a deliberately small store): an operation that
// needs a slot the store does not have marks its chunks Unsynced (no Fail); nothing is requested while
// the store is full; once slots are free each chunk is requested exactly once, and its data makes the
// hash equal again.
TEST(Client, ASmallStoreMarksChunksUnsyncedAndRequestsEachOnceWhenSlotsFree) {
    // Boxes in the air above the ground (Empty chunks: each needs a slot to change).
    auto airBox = [](uint32_t i) {
        const glm::ivec3 p(static_cast<int>(i) * 40 - 120, 140, 0);
        return sim::WorldOp::Box(p, p + glm::ivec3(2), Ids().gold);
    };
    std::unique_ptr<FakeServer> ref = ReferenceWorld(5, airBox);
    FakeServer s;
    s.Join(s.MakeWelcome());
    ASSERT_TRUE(s.Attach());
    std::vector<uint32_t> taken;
    for (uint32_t slot; (slot = s.simulation->Chunks().Alloc()) != voxel::ChunkStore::kInvalidSlot;) taken.push_back(slot);
    s.SendBatch(1, 5, airBox);
    for (int i = 0; i < 5; ++i) s.client->Poll();
    s.ReadRequests();
    EXPECT_EQ(s.client->State(), net::ClientState::Playing);
    EXPECT_EQ(s.client->Stats().unsyncedChunks, 5u);
    EXPECT_EQ(s.chunkRequests.size(), 0u) << "requested while the store is full";
    EXPECT_FALSE(s.client->Synced());
    EXPECT_FALSE(s.client->Hash().Complete());
    for (const uint32_t slot : taken) (void)s.simulation->Chunks().Release(slot);
    for (int i = 0; i < 5; ++i) s.client->Poll();
    s.ReadRequests();
    ASSERT_EQ(s.chunkRequests.size(), 5u);
    std::set<std::tuple<int, int, int>> unique;
    for (const glm::ivec3& c : s.chunkRequests) unique.insert({c.x, c.y, c.z});
    EXPECT_EQ(unique.size(), 5u) << "a chunk requested twice";
    for (const glm::ivec3& c : s.chunkRequests) s.SendChunk(ref->simulation->World(), c, 5);
    for (int i = 0; i < 5; ++i) s.client->Poll();
    s.ReadRequests();
    EXPECT_EQ(s.chunkRequests.size(), 5u);
    EXPECT_TRUE(s.client->Synced());
    EXPECT_EQ(s.client->Hash().hash, net::RegionHash(ref->simulation->World(), SmallRegion()).hash);
}

// Stage 7a (networking.md "Entity replication": "an ObjectModel for an unknown id waits for its spawn
// and replaces an older baseline model"): entity events of a NetId the joiner has no copy of yet are
// held while its baseline is open, applied after the BaselineEntity when newer than its asOf, dropped
// when not; a tombstoned id's baseline is discarded; after BaselineEnd unknown ids are ignored.
TEST(Client, EntityEventsForAnUnknownIdWaitForItsSpawn) {
    voxel::VoxelVolume cube(glm::ivec3(4)), smaller(glm::ivec3(3));
    cube.FillBox(glm::ivec3(0), glm::ivec3(4), Ids().planks);
    smaller.FillBox(glm::ivec3(0), glm::ivec3(3), Ids().planks);
    for (const uint32_t asOf : {4u, 7u}) {
        FakeServer s;
        s.Join(s.MakeWelcome(4), false);
        ASSERT_TRUE(s.Attach());
        net::ObjectModel model;
        model.netId = 9;
        model.volume = smaller;
        const net::EntityRest rest{9, 40, glm::vec3(1.25f, 12.0f, -2.5f), glm::normalize(glm::quat(0.9f, 0.1f, 0.3f, 0.0f))};
        net::EventBatch batch;
        batch.firstOpSeq = 5;
        batch.events = {model, rest, net::EntityDespawn{11, scene::DespawnReason::Destroyed}};
        s.Send(net::Encode(batch));
        s.client->Poll();
        EXPECT_TRUE(s.client->Entities().empty());
        EXPECT_EQ(s.client->Stats().heldEntityEvents, 2u);
        net::BaselineEntity b;
        b.asOfOpSeq = asOf;
        b.tick = 30;
        b.spawn.netId = 9;
        b.spawn.kind = net::EntityKind::Object;
        b.spawn.object.position = glm::vec3(1.0f, 13.0f, 1.0f);
        b.spawn.object.volume = cube;
        s.SendBulk(net::Encode(b));
        net::BaselineEntity dead = b;              // despawned at 7: its baseline is discarded
        dead.spawn.netId = 11;
        s.SendBulk(net::Encode(dead));
        s.SendBulk(net::Encode(net::BaselineEnd{asOf, 2}));
        s.client->Poll();
        const std::vector<net::EntityInfo> copies = s.client->Entities();
        ASSERT_EQ(copies.size(), 1u);
        EXPECT_EQ(copies[0].netId, 9u);
        if (asOf == 4) {
            EXPECT_EQ(copies[0].modelHash, net::VolumeHash(smaller)) << "the newer model replaces the baseline's";
            EXPECT_TRUE(copies[0].resting);
            EXPECT_EQ(copies[0].position, rest.position);
            EXPECT_EQ(copies[0].rotation, rest.rotation);
        } else {
            EXPECT_EQ(copies[0].modelHash, net::VolumeHash(cube)) << "an event the baseline already holds was applied";
            EXPECT_FALSE(copies[0].resting);
            EXPECT_EQ(s.client->Stats().droppedHeldEvents, 2u);
        }
        EXPECT_EQ(s.client->Stats().baselineDiscarded, 1u);
        const uint32_t ignored = s.client->Stats().ignoredEntityEvents;
        batch.firstOpSeq = 8;
        batch.events = {net::EntityRest{12, 41, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}};
        s.Send(net::Encode(batch));
        s.client->Poll();
        EXPECT_EQ(s.client->Stats().ignoredEntityEvents, ignored + 1);
        EXPECT_TRUE(s.client->Synced());
    }
}

// Stage 7a (networking.md "World operations" 3: "if a touched chunk has no base yet, netId = 0 makes
// no rubble and netId != 0 sends ModelRequest"): a Detach over a chunk that waits for its data builds
// no piece from a partial volume; a replicated one asks for its model once, and the copy comes from
// the answer with the server's model; the chunk's data later replays nothing into a second copy.
TEST(Client, ADetachOverChunksWithoutABaseAsksForTheModel) {
    net::ClientSettings presenting;
    presenting.presentation = true;
    FakeServer s(presenting);
    const glm::ivec3 chunk(0, 2, 0);
    s.Join(s.MakeWelcome(), true, {chunk});      // the chunk is modified at 0: its data is coming
    ASSERT_TRUE(s.Attach());
    net::EventBatch batch;
    batch.firstOpSeq = 1;
    batch.events = {DetachEvent(1), DetachEvent(2)};
    s.Send(net::Encode(batch));
    s.client->Poll();
    s.ReadRequests();
    EXPECT_TRUE(s.client->Entities().empty());
    ASSERT_EQ(s.modelRequests.size(), 1u);
    EXPECT_EQ(s.modelRequests[0], 9u);
    EXPECT_TRUE(s.client->TakePresentations().empty()) << "rubble from a partial volume";
    EXPECT_FALSE(s.client->Synced());
    // The answer: the server's body of piece 9 (its model is the voxels the Detach removed there).
    voxel::VoxelVolume piece(DetachEvent(2).mask.Size());
    piece.FillBox(glm::ivec3(0), piece.Size(), Ids().stone);
    net::BaselineEntity b;
    b.asOfOpSeq = 2;
    b.tick = 10;
    b.spawn.netId = 9;
    b.spawn.kind = net::EntityKind::Object;
    b.spawn.object.position = glm::vec3(0.5f, 11.25f, 0.5f);
    b.spawn.object.volume = piece;
    s.SendBulk(net::Encode(b));
    std::unique_ptr<FakeServer> ref = ReferenceWorld(2, DetachEvent);
    s.SendChunk(ref->simulation->World(), chunk, 1);   // data as of 1: op 2 is replayed (voxels only)
    for (int i = 0; i < 3; ++i) s.client->Poll();
    s.ReadRequests();
    const std::vector<net::EntityInfo> copies = s.client->Entities();
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(copies[0].modelHash, net::VolumeHash(piece));
    EXPECT_EQ(s.client->Stats().copiesMade, 1u);
    EXPECT_EQ(s.modelRequests.size(), 1u);
    EXPECT_TRUE(s.client->Synced());
    EXPECT_EQ(s.client->Hash().hash, net::RegionHash(ref->simulation->World(), SmallRegion()).hash);
}

// Review of stage 7a: a NetId whose held events overflow (64) is asked for again even though a request
// is out - an answer encoded before the dropped events would miss them - and an answer older than the
// newest dropped event is discarded: the copy comes from the newer one (here with the model the dropped
// ObjectModel carried).
TEST(Client, AnAnswerOlderThanDroppedHeldEventsIsDiscarded) {
    FakeServer s;
    const glm::ivec3 chunk(0, 2, 0);
    s.Join(s.MakeWelcome(), true, {chunk});       // the chunk waits for its data: the Detach of 9 asks for it
    ASSERT_TRUE(s.Attach());
    net::EventBatch batch;
    batch.firstOpSeq = 1;
    batch.events = {DetachEvent(2)};
    s.Send(net::Encode(batch));
    s.client->Poll();
    s.ReadRequests();
    ASSERT_EQ(s.modelRequests.size(), 1u);
    // 64 rest / wake events of 9 are held; the 65th, a new model, overflows: all dropped, asked again.
    voxel::VoxelVolume cube(glm::ivec3(4)), smaller(glm::ivec3(3));
    cube.FillBox(glm::ivec3(0), glm::ivec3(4), Ids().planks);
    smaller.FillBox(glm::ivec3(0), glm::ivec3(3), Ids().planks);
    batch.firstOpSeq = 2;
    batch.events.clear();
    for (uint32_t i = 0; i < 64; ++i) {
        if (i % 2 == 0) batch.events.push_back(net::EntityWake{9, 10 + i});
        else batch.events.push_back(net::EntityRest{9, 10 + i, glm::vec3(0.5f, 12.0f, 0.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    }
    net::ObjectModel model;
    model.netId = 9;
    model.volume = smaller;
    batch.events.push_back(model);                // opSeq 66
    s.Send(net::Encode(batch));
    s.client->Poll();
    s.client->Poll();
    s.ReadRequests();
    EXPECT_EQ(s.client->Stats().droppedHeldEvents, 65u);
    ASSERT_EQ(s.modelRequests.size(), 2u) << "asked again although a request was out";
    // The answer to the first request, encoded at opSeq 1 (before the model): discarded.
    net::BaselineEntity old;
    old.asOfOpSeq = 1;
    old.tick = 5;
    old.restTick = 5;
    old.spawn.netId = 9;
    old.spawn.kind = net::EntityKind::Object;
    old.spawn.object.position = glm::vec3(0.5f, 12.0f, 0.5f);
    old.spawn.object.volume = cube;
    old.spawn.object.flags = net::kObjectResting;
    s.SendBulk(net::Encode(old));
    s.client->Poll();
    EXPECT_TRUE(s.client->Entities().empty()) << "a copy from an answer that misses a dropped event";
    EXPECT_EQ(s.client->Stats().staleAnswers, 1u);
    // The answer to the second, as of 66: the copy, with the newer model.
    net::BaselineEntity fresh = old;
    fresh.asOfOpSeq = 66;
    fresh.tick = 80;
    fresh.restTick = 73;
    fresh.spawn.object.volume = smaller;
    s.SendBulk(net::Encode(fresh));
    std::unique_ptr<FakeServer> ref = ReferenceWorld(2, DetachEvent);
    s.SendChunk(ref->simulation->World(), chunk, 66);
    s.client->Poll();
    const std::vector<net::EntityInfo> copies = s.client->Entities();
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(copies[0].modelHash, net::VolumeHash(smaller));
    EXPECT_TRUE(s.client->Synced());
}

// Review of stage 7a: bulk installs are bounded per Poll (ClientSettings::maxBulkPerPoll), in arrival
// order; the rest wait for the next Poll.
TEST(Client, BulkInstallsAreBoundedPerPoll) {
    net::ClientSettings settings;
    settings.maxBulkPerPoll = 4;
    FakeServer s(settings);
    std::vector<glm::ivec3> chunks;
    for (int x = -2; x < 2; ++x)
        for (int z = -2; z < 1; ++z) chunks.emplace_back(x, 2, z);
    s.Join(s.MakeWelcome(), false, chunks);
    ASSERT_TRUE(s.Attach());
    std::unique_ptr<FakeServer> ref = ReferenceWorld(0);
    for (const glm::ivec3& c : chunks) s.SendChunk(ref->simulation->World(), c, 0);
    s.SendBulk(net::Encode(net::BaselineEnd{0, 0}));
    uint32_t polls = 0;
    while (!s.client->Synced() && polls < 10) {
        s.client->Poll();
        ++polls;
    }
    EXPECT_TRUE(s.client->Synced());
    EXPECT_EQ(s.client->Stats().maxBulkInPoll, 4u);
    EXPECT_GE(polls, 3u);
    EXPECT_EQ(s.client->Sync().Stats().chunksInstalled, static_cast<uint32_t>(chunks.size()));
}
