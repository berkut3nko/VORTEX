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

        net::Welcome MakeWelcome(uint32_t logEnd = 0) const {
            net::Welcome w;
            w.region = SmallRegion();
            w.logEnd = logEnd;
            w.probes = net::ProbeChunks(w.region, *sim::MakeRegionGenerator(w.region, Ids()));
            return w;
        }

        void Send(const std::vector<uint8_t>& bytes) { serverEnd.Send(peer, net::Channel::Events, bytes); }

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
    };

    /// The hash of the region after events 1..count applied once each, on a fresh world.
    uint64_t ReferenceHash(uint32_t count, sim::WorldOp (*make)(uint32_t) = BoxEvent) {
        FakeServer ref;
        ref.Send(net::Encode(ref.MakeWelcome()));
        EXPECT_TRUE(ref.Attach());
        for (uint32_t i = 1; i <= count; ++i) sim::ApplyOp(ref.simulation->World(), make(i));
        return net::RegionHash(ref.simulation->World(), SmallRegion()).hash;
    }
}

// An overlapping batch (events 5..9 after 1..7, as an ENet resend after a lost ack would give) is
// applied from 8 on; batches that arrive before Attach are kept and applied by it.
TEST(Client, OverlappingBatchesApplyEachEventOnce) {
    FakeServer s;
    s.Send(net::Encode(s.MakeWelcome()));
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
    s.Send(net::Encode(s.MakeWelcome()));
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
        s.Send(net::Encode(s.MakeWelcome()));
        s.client->Poll();
        EXPECT_EQ(s.client->State(), net::ClientState::Welcomed);
        EXPECT_EQ(s.client->Stats().probeMismatches, 0u);
    }
}

// A client that joins late presents only live events: the join log (and a template's operations
// in it) is history, not blasts to show again.
TEST(Client, ALateJoinerPresentsOnlyLiveEvents) {
    net::ClientSettings presenting;
    presenting.presentation = true;
    FakeServer s(presenting);
    s.Send(net::Encode(s.MakeWelcome(3)));
    s.SendBatch(1, 3, CarveEvent);
    ASSERT_TRUE(s.Attach());
    EXPECT_EQ(s.client->AppliedOpSeq(), 3u);
    EXPECT_TRUE(s.client->TakePresentations().empty());
    s.SendBatch(4, 4, CarveEvent);
    s.client->Poll();
    const std::vector<sim::BlastResult> live = s.client->TakePresentations();
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0].ops.size(), 1u);
    EXPECT_EQ(s.client->Hash().hash, ReferenceHash(4, CarveEvent));
}

// The world the client is given must be the one its generator makes, untouched: a simulation built
// with another generator, or one already edited, is refused (its probe chunks differ).
TEST(Client, AttachRefusesAWorldItsGeneratorDidNotMake) {
    {
        FakeServer s;
        s.Send(net::Encode(s.MakeWelcome()));
        s.client->Poll();
        ASSERT_EQ(s.client->State(), net::ClientState::Welcomed);
        sim::Simulation other(s.client->MakeSimulationConfig(s.base));
        ASSERT_TRUE(other.Initialize(voxel::CreateFlatGenerator(kGround + 40, Ids().grass, Ids().stone)));
        EXPECT_FALSE(s.client->Attach(other));
        EXPECT_EQ(s.client->State(), net::ClientState::Welcomed);
    }
    {
        FakeServer s;
        s.Send(net::Encode(s.MakeWelcome()));
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
    s.Send(net::Encode(s.MakeWelcome()));
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
        s.Send(net::Encode(s.MakeWelcome()));
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
    s.Send(net::Encode(s.MakeWelcome()));
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
    s.Send(net::Encode(s.MakeWelcome()));
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
    s.Send(net::Encode(s.MakeWelcome()));
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
    s.Send(net::Encode(s.MakeWelcome()));
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
    s.Send(net::Encode(s.MakeWelcome()));
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
