// docs/design/networking.md stage 3: net::Client against a hand-written server endpoint on a raw
// loopback, so that the client meets what a real server never sends in the other tests: overlapping
// batches (an ENet resend after a lost ack), a gap in the stream, a forged Welcome, and events that
// are the join log rather than live.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

import vortex.core;
import vortex.voxel;
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
