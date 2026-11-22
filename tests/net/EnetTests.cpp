// docs/design/networking.md stage 5: net::EnetTransport over real UDP sockets on 127.0.0.1. Loss is
// real where it matters: EnetSettings::receiveLoss throws incoming datagrams away before ENet sees
// them, so ENet itself resends what is reliable. These tests run on the wall clock (ENet's resend
// timers are real time): a few seconds each.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    using Clock = std::chrono::steady_clock;

    double Since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

    /// Polls `t` and keeps every event.
    void Drain(net::ITransport& t, std::vector<net::TransportEvent>& out) {
        net::TransportEvent e;
        while (t.Poll(e)) out.push_back(std::move(e));
    }

    /// A listening and a connecting transport on 127.0.0.1, connected (or a failed assertion).
    struct UdpPair {
        std::unique_ptr<net::EnetTransport> server, client;
        net::PeerId atServer = 0, atClient = 0;   // the id each side has for the other

        UdpPair(net::EnetSettings serverSettings, net::EnetSettings clientSettings) {
            std::string error;
            server = net::EnetTransport::Listen("127.0.0.1", 0, serverSettings, &error);
            EXPECT_TRUE(server) << error;
            if (!server) return;
            client = net::EnetTransport::Connect("127.0.0.1", server->Port(), clientSettings, &error);
            EXPECT_TRUE(client) << error;
            if (!client) return;
            const Clock::time_point start = Clock::now();
            while ((atServer == 0 || atClient == 0) && Since(start) < 5.0) {
                net::TransportEvent e;
                while (server->Poll(e))
                    if (e.type == net::TransportEvent::Type::Connected) atServer = e.peer;
                while (client->Poll(e))
                    if (e.type == net::TransportEvent::Type::Connected) atClient = e.peer;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        bool Connected() const { return atServer != 0 && atClient != 0; }
    };

    std::vector<uint8_t> Message(uint32_t index, size_t size) {
        std::vector<uint8_t> m(std::max<size_t>(size, 4));
        for (size_t i = 0; i < m.size(); ++i) m[i] = static_cast<uint8_t>(index * 31u + i);
        m[0] = static_cast<uint8_t>(index);
        m[1] = static_cast<uint8_t>(index >> 8);
        m[2] = static_cast<uint8_t>(index >> 16);
        m[3] = static_cast<uint8_t>(index >> 24);
        return m;
    }

    uint32_t IndexOf(const std::vector<uint8_t>& m) {
        return m.size() < 4 ? 0xFFFFFFFFu : static_cast<uint32_t>(m[0]) | static_cast<uint32_t>(m[1]) << 8 | static_cast<uint32_t>(m[2]) << 16 |
                                                static_cast<uint32_t>(m[3]) << 24;
    }

    bool Intact(const std::vector<uint8_t>& m) {
        const uint32_t index = IndexOf(m);
        for (size_t i = 4; i < m.size(); ++i)
            if (m[i] != static_cast<uint8_t>(index * 31u + i)) return false;
        return true;
    }
}

TEST(Enet, ParsesEndpoints) {
    std::string host;
    uint16_t port = 0;
    ASSERT_TRUE(net::ParseEndpoint("192.168.1.20:27016", net::kDefaultPort, host, port));
    EXPECT_EQ(host, "192.168.1.20");
    EXPECT_EQ(port, 27016);
    ASSERT_TRUE(net::ParseEndpoint("example.lan", net::kDefaultPort, host, port));
    EXPECT_EQ(host, "example.lan");
    EXPECT_EQ(port, net::kDefaultPort);
    EXPECT_FALSE(net::ParseEndpoint("", net::kDefaultPort, host, port));
    EXPECT_FALSE(net::ParseEndpoint(":27015", net::kDefaultPort, host, port));
    EXPECT_FALSE(net::ParseEndpoint("host:", net::kDefaultPort, host, port));
    EXPECT_FALSE(net::ParseEndpoint("host:0", net::kDefaultPort, host, port));
    EXPECT_FALSE(net::ParseEndpoint("host:65536", net::kDefaultPort, host, port));
    EXPECT_FALSE(net::ParseEndpoint("host:12a", net::kDefaultPort, host, port));
}

// The transport's contract (ITransport) over UDP with 5 % of the datagrams lost in each direction:
// every reliable message arrives exactly once, intact and in order on its channel, large ones
// (fragmented by ENet) included; the state channel loses some and never delivers an older one after
// a newer one (ENet sequences it); ReliableBytesInFlight counts what is not acknowledged yet and
// returns to 0.
TEST(Enet, ReliableMessagesArriveOnceInOrderThroughRealLoss) {
    net::EnetSettings a, b;
    a.receiveLoss = b.receiveLoss = 0.05;
    a.lossSeed = 3;
    b.lossSeed = 4;
    UdpPair pair(a, b);
    ASSERT_TRUE(pair.Connected());
    constexpr uint32_t kCount = 400;
    std::vector<net::TransportEvent> atServer, atClient;
    size_t peakInFlight = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
        const size_t size = i % 7 == 0 ? 3000 : 40 + i % 200;   // every 7th is larger than one datagram
        pair.client->Send(pair.atClient, net::Channel::Events, Message(i, size));
        pair.server->Send(pair.atServer, net::Channel::Events, Message(i, size));
        pair.client->Send(pair.atClient, net::Channel::State, Message(i, 60));
        peakInFlight = std::max(peakInFlight, pair.client->ReliableBytesInFlight(pair.atClient));
        if (i % 20 == 19) {
            pair.client->Flush();
            pair.server->Flush();
            Drain(*pair.server, atServer);
            Drain(*pair.client, atClient);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    auto reliableCount = [](const std::vector<net::TransportEvent>& events) {
        size_t n = 0;
        for (const net::TransportEvent& e : events) n += e.type == net::TransportEvent::Type::Received && e.channel == net::Channel::Events;
        return n;
    };
    const Clock::time_point start = Clock::now();
    while ((reliableCount(atServer) < kCount || reliableCount(atClient) < kCount || pair.client->ReliableBytesInFlight(pair.atClient) > 0 ||
            pair.server->ReliableBytesInFlight(pair.atServer) > 0) &&
           Since(start) < 15.0) {
        Drain(*pair.server, atServer);
        Drain(*pair.client, atClient);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (const std::vector<net::TransportEvent>* events : {&atServer, &atClient}) {
        uint32_t nextReliable = 0;
        int64_t lastState = -1;
        size_t states = 0;
        for (const net::TransportEvent& e : *events) {
            ASSERT_EQ(e.type, net::TransportEvent::Type::Received);
            ASSERT_TRUE(Intact(e.data));
            if (e.channel == net::Channel::Events) {
                EXPECT_EQ(IndexOf(e.data), nextReliable);   // once, in order
                ++nextReliable;
            } else {
                EXPECT_GT(static_cast<int64_t>(IndexOf(e.data)), lastState);   // never an older one after a newer one
                lastState = IndexOf(e.data);
                ++states;
            }
        }
        EXPECT_EQ(nextReliable, kCount);
        if (events == &atServer) {
            EXPECT_GT(states, kCount / 2);
            EXPECT_LE(states, kCount);
        }
    }
    std::printf("datagrams dropped: server %llu of %llu, client %llu of %llu; peak in flight %zu bytes\n",
                static_cast<unsigned long long>(pair.server->Stats().datagramsDropped),
                static_cast<unsigned long long>(pair.server->Stats().datagramsReceived),
                static_cast<unsigned long long>(pair.client->Stats().datagramsDropped),
                static_cast<unsigned long long>(pair.client->Stats().datagramsReceived), peakInFlight);
    EXPECT_GT(pair.server->Stats().datagramsDropped, 0u);
    EXPECT_GT(pair.client->Stats().datagramsDropped, 0u);
    EXPECT_GT(peakInFlight, 0u);
    EXPECT_EQ(pair.client->ReliableBytesInFlight(pair.atClient), 0u);   // everything acknowledged
    EXPECT_EQ(pair.server->ReliableBytesInFlight(pair.atServer), 0u);
    EXPECT_GT(pair.client->RoundTripSeconds(pair.atClient), 0.0);
    EXPECT_FALSE(pair.server->PeerAddress(pair.atServer).empty());
}

// Disconnect is graceful and both sides hear of it: the side that closes at once (as the loopback
// does), the other side when ENet's disconnect arrives. A connection to a port where nobody listens
// is reported as Disconnected once ENet gives up.
TEST(Enet, DisconnectsAndFailedConnectsAreReported) {
    UdpPair pair({}, {});
    ASSERT_TRUE(pair.Connected());
    EXPECT_EQ(pair.server->PeerCount(), 1u);
    pair.server->Send(pair.atServer, net::Channel::Events, Message(7, 100));   // still delivered before the close
    pair.server->Disconnect(pair.atServer);
    net::TransportEvent e;
    ASSERT_TRUE(pair.server->Poll(e));
    EXPECT_EQ(e.type, net::TransportEvent::Type::Disconnected);
    EXPECT_EQ(e.peer, pair.atServer);
    EXPECT_EQ(pair.server->PeerCount(), 0u);
    bool received = false, closed = false;
    const Clock::time_point start = Clock::now();
    while (!closed && Since(start) < 5.0) {
        pair.server->Poll(e);
        while (pair.client->Poll(e)) {
            received = received || (e.type == net::TransportEvent::Type::Received && IndexOf(e.data) == 7);
            closed = closed || (e.type == net::TransportEvent::Type::Disconnected && e.peer == pair.atClient);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(received);
    EXPECT_TRUE(closed);

    // Nobody listens on a port that was just freed.
    uint16_t port = 0;
    {
        std::string error;
        const std::unique_ptr<net::EnetTransport> gone = net::EnetTransport::Listen("127.0.0.1", 0, {}, &error);
        ASSERT_TRUE(gone) << error;
        port = gone->Port();
    }
    net::EnetSettings quick;
    quick.timeoutMinimumMs = 200;
    quick.timeoutMaximumMs = 1000;
    std::string error;
    const std::unique_ptr<net::EnetTransport> lonely = net::EnetTransport::Connect("127.0.0.1", port, quick, &error);
    ASSERT_TRUE(lonely) << error;
    bool failed = false;
    const Clock::time_point t0 = Clock::now();
    while (!failed && Since(t0) < 10.0) {
        while (lonely->Poll(e)) {
            EXPECT_NE(e.type, net::TransportEvent::Type::Connected);
            failed = failed || e.type == net::TransportEvent::Type::Disconnected;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("a connect to a closed port was given up after %.2f s\n", Since(t0));
    EXPECT_TRUE(failed);
    EXPECT_EQ(net::EnetTransport::Connect("no such host.invalid", 1, {}, &error), nullptr);
    EXPECT_FALSE(error.empty());
}

// Review of stage 5: ENet reassembles whatever a peer sends before any token is checked, and its
// defaults let one message be 32 MiB. A listening transport takes messages of at most
// EnetSettings::maxIncomingMessageBytes (64 KiB unless set; a client's largest message is under
// 1 KiB): a larger one is dropped by ENet before it allocates it, unacknowledged, so its sender's
// connection times out, and the server goes on with everyone else. The server itself still sends
// large messages (an event larger than a batch goes alone), and a connecting transport takes them.
TEST(Enet, AListeningTransportRefusesLargeMessagesButSendsThem) {
    EXPECT_EQ(net::kListenMaxMessageBytes, 64u * 1024u);
    constexpr size_t kLarge = 1u << 20;
    {
        UdpPair pair({}, {});
        ASSERT_TRUE(pair.Connected());
        pair.server->Send(pair.atServer, net::Channel::Events, Message(5, kLarge));
        std::vector<net::TransportEvent> atClient;
        const Clock::time_point start = Clock::now();
        while (atClient.empty() && Since(start) < 5.0) {
            pair.server->Flush();
            net::TransportEvent e;
            while (pair.server->Poll(e)) {}
            Drain(*pair.client, atClient);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(atClient.size(), 1u);
        EXPECT_EQ(atClient[0].data.size(), kLarge);
        EXPECT_TRUE(Intact(atClient[0].data));
    }
    net::EnetSettings quick;
    quick.timeoutMinimumMs = 200;
    quick.timeoutMaximumMs = 1000;
    UdpPair pair(quick, quick);
    ASSERT_TRUE(pair.Connected());
    pair.client->Send(pair.atClient, net::Channel::Events, Message(1, 100));
    pair.client->Send(pair.atClient, net::Channel::Events, Message(2, kLarge));
    std::vector<net::TransportEvent> atServer;
    bool clientClosed = false, serverClosed = false;
    const Clock::time_point start = Clock::now();
    while ((!clientClosed || !serverClosed) && Since(start) < 8.0) {
        pair.client->Flush();
        net::TransportEvent e;
        while (pair.server->Poll(e)) {
            serverClosed = serverClosed || e.type == net::TransportEvent::Type::Disconnected;
            atServer.push_back(std::move(e));
        }
        while (pair.client->Poll(e)) clientClosed = clientClosed || e.type == net::TransportEvent::Type::Disconnected;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("the large message's sender was dropped after %.2f s\n", Since(start));
    size_t small = 0, large = 0;
    for (const net::TransportEvent& e : atServer)
        if (e.type == net::TransportEvent::Type::Received) (e.data.size() > net::kListenMaxMessageBytes ? large : small) += 1;
    EXPECT_EQ(small, 1u);
    EXPECT_EQ(large, 0u);
    EXPECT_TRUE(clientClosed);
    EXPECT_TRUE(serverClosed);
    // The server takes the next client as if nothing happened.
    std::string error;
    const std::unique_ptr<net::EnetTransport> next = net::EnetTransport::Connect("127.0.0.1", pair.server->Port(), {}, &error);
    ASSERT_TRUE(next) << error;
    bool connected = false, received = false;
    const Clock::time_point t0 = Clock::now();
    while (!received && Since(t0) < 5.0) {
        net::TransportEvent e;
        while (next->Poll(e))
            if (e.type == net::TransportEvent::Type::Connected) {
                connected = true;
                next->Send(e.peer, net::Channel::Events, Message(3, 200));
            }
        next->Flush();
        while (pair.server->Poll(e)) received = received || (e.type == net::TransportEvent::Type::Received && IndexOf(e.data) == 3);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(connected);
    EXPECT_TRUE(received);
}

namespace {
    constexpr int kGround = 96;   // flat ground at 12 m (0.125 m voxels)

    net::Token SessionToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x23 + i * 5);
        return t;
    }

    sim::RegionDesc UdpRegion() {
        voxel::MaterialLibrary scratch;
        sim::RegionDesc r;
        r.templateId = sim::RegionTemplateId::Empty;
        r.seed = 5150;
        r.playMinChunk = {-4, -4};
        r.playMaxChunk = {3, 3};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = kGround;
        r.terrain.voxelSize = 0.125f;
        r.terrain.materials = voxel::RegisterDefaultWorldMaterials(scratch);
        return r;
    }

    sim::SimulationConfig SmallSimulation() {
        sim::SimulationConfig c;
        c.jobThreads = 2;
        c.chunkCapacity = 4096;
        c.physics.threads = 1;
        c.physics.maxBodies = 2048;
        c.physics.maxBodyPairs = 4096;
        c.physics.maxContactConstraints = 4096;
        return c;
    }
}

// The stage-5 acceptance without a GPU, on one machine: a session server on a UDP port of
// 127.0.0.1 and two clients, each on its own ENet socket, every socket losing 5 % of what it
// receives. The two players walk, run, jump and cast (bot pilots), the server blasts drill structures
// that free terrain, and both clients see each other. After a drain the clients' worlds are the
// server's (equal WorldHash at the same opSeq) and the prediction needed few corrections.
TEST(EnetSession, TwoPlayersWalkCastAndAgreeOverUdpWithLoss) {
    net::ServerSettings settings;
    settings.session.token = SessionToken();
    settings.session.region = UdpRegion();
    settings.session.packHidden = false;
    settings.simulation = SmallSimulation();
    net::EnetSettings serverNet;
    serverNet.receiveLoss = 0.05;
    serverNet.lossSeed = 11;
    std::string error;
    const std::unique_ptr<net::EnetTransport> listen = net::EnetTransport::Listen("127.0.0.1", 0, serverNet, &error);
    ASSERT_TRUE(listen) << error;
    net::Server server(settings, *listen);
    ASSERT_TRUE(server.Start());

    struct Player {
        std::unique_ptr<net::EnetTransport> transport;
        std::unique_ptr<net::Client> client;
        std::unique_ptr<sim::Simulation> simulation;
        std::unique_ptr<net::BotPilot> pilot;
        uint32_t casts = 0;
        bool sawOther = false;
    };
    std::vector<Player> players(2);
    net::PilotSettings pilotSettings;
    pilotSettings.casts = true;
    pilotSettings.castEveryTicks = 50;
    for (size_t i = 0; i < players.size(); ++i) {
        net::EnetSettings ns;
        ns.receiveLoss = 0.05;
        ns.lossSeed = 21 + i;
        players[i].transport = net::EnetTransport::Connect("127.0.0.1", listen->Port(), ns, &error);
        ASSERT_TRUE(players[i].transport) << error;
        net::ClientSettings cs;
        cs.token = SessionToken();
        cs.presentation = i == 0;
        players[i].client = std::make_unique<net::Client>(cs, *players[i].transport);
        players[i].pilot = std::make_unique<net::BotPilot>(server.Region(), 0x5eedu + i, pilotSettings);
    }
    net::BlastDrill drill(server.Region(), server.Simulation().WorldMaterialIds(), server.Simulation().World());
    constexpr uint32_t kDrillSteps = 9;

    bool draining = false;
    uint32_t drillStep = 0;
    Clock::time_point last = Clock::now();
    auto elapsed = [&last] {
        const Clock::time_point now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - last).count();
        last = now;
        return seconds;
    };
    // A player that loads its region keeps everyone else running: its Attach callback runs whole
    // passes without it (review of stage 5: before, the load stalled the server and the other
    // client, whose clock then took the whole gap at once and made a hard resync in every run).
    const Player* loading = nullptr;
    std::function<void(double)> pass;
    pass = [&](double seconds) {
        // A drill structure every 40 ticks: built, then shot (it frees an island, a bar or a neck tower).
        if (!draining && drillStep < kDrillSteps && server.TickNumber() >= 60 + 40 * drillStep) {
            const net::DrillStep s = drill.Step(drillStep++);
            net::RunDrillStep(server, s);
        }
        server.Advance(seconds);
        listen->Flush();
        for (Player& p : players) {
            if (&p == loading) continue;
            if (p.client->State() == net::ClientState::Welcomed && !p.simulation && loading == nullptr) {
                p.simulation = std::make_unique<sim::Simulation>(p.client->MakeSimulationConfig(SmallSimulation()));
                ASSERT_TRUE(p.simulation->Initialize(p.client->MakeGenerator()));
                loading = &p;
                const bool attached = p.client->Attach(*p.simulation, [&] {
                    p.client->Poll();
                    p.transport->Flush();
                    pass(elapsed());
                });
                pass(elapsed());                  // the rest of the load (its final checks) for everyone else
                loading = nullptr;
                ASSERT_TRUE(attached);
                seconds = 0.0;                    // the passes inside the load gave everyone that time
            }
            const uint32_t n = p.client->TicksDue(seconds);
            for (uint32_t k = 0; k < n; ++k) {
                const sim::PlayerInput in = p.client->Predicting() && !draining ? p.pilot->Next(p.client->Player()) : sim::PlayerInput{};
                p.casts += p.client->RunTick(in).cast ? 1u : 0u;
                if (p.simulation)
                    for (const sim::BlastResult& blast : p.client->TakePresentations()) sim::PresentBlast(*p.simulation, blast);
            }
            p.sawOther = p.sawOther || p.client->RemotePlayers().size() == 1;
            p.transport->Flush();
        }
    };

    // About 9 s of play on the wall clock (ENet's timers are real time).
    const Clock::time_point start = Clock::now();
    while (Since(start) < 9.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pass(elapsed());
        ASSERT_FALSE(server.Failed());
    }
    // Drain: the players stand, the cast queue empties and every client applies the server's last event.
    draining = true;
    const Clock::time_point drainStart = Clock::now();
    auto settled = [&] {
        if (server.QueuedCasts() > 0) return false;
        for (const Player& p : players)
            if (!p.simulation || p.client->AppliedOpSeq() != server.LastOpSeq()) return false;
        return true;
    };
    while (!settled() && Since(drainStart) < 10.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pass(elapsed());
    }
    pass(0.0);

    const net::ServerStats& st = server.Stats();
    uint32_t drillCasts = 0, playerCasts = 0;
    for (const net::CastRecord& c : server.Casts()) (c.player == 0 ? drillCasts : playerCasts) += 1;
    std::printf("server: %u ticks, %llu events, %u casts run (%u drill, %u players; %u missed, %u rejected), %llu Detach, %llu "
                "crumbs; datagrams dropped %llu of %llu\n",
                server.TickNumber(), static_cast<unsigned long long>(st.events), st.castsRun, drillCasts, playerCasts, st.castsMissed,
                st.castsRejected, static_cast<unsigned long long>(st.islands), static_cast<unsigned long long>(st.crumbs),
                static_cast<unsigned long long>(listen->Stats().datagramsDropped),
                static_cast<unsigned long long>(listen->Stats().datagramsReceived));
    const voxel::WorldHashResult serverHash = server.Hash();
    ASSERT_TRUE(serverHash.Complete());
    EXPECT_FALSE(server.Failed());
    EXPECT_EQ(drillCasts, kDrillSteps);
    EXPECT_GT(playerCasts, 0u);
    EXPECT_GT(st.islands, 0u);
    EXPECT_GT(listen->Stats().datagramsDropped, 0u);
    for (size_t i = 0; i < players.size(); ++i) {
        const Player& p = players[i];
        const net::PredictionStats& pr = p.client->Prediction();
        std::printf("client %zu: opSeq %u / %u, %llu compared, %llu corrected, %llu snaps, max error %.3f m, %u clock jumps, %u resyncs, "
                    "margin %d..%d, %u casts shown (%u rejected), datagrams dropped %llu of %llu, RTT %.1f ms\n",
                    i, p.client->AppliedOpSeq(), server.LastOpSeq(), static_cast<unsigned long long>(pr.compared),
                    static_cast<unsigned long long>(pr.corrections), static_cast<unsigned long long>(pr.snaps), pr.maxError, pr.clockJumps,
                    pr.hardResyncs, pr.minMargin, pr.maxMargin, p.casts, pr.castsRejected,
                    static_cast<unsigned long long>(p.transport->Stats().datagramsDropped),
                    static_cast<unsigned long long>(p.transport->Stats().datagramsReceived),
                    p.transport->RoundTripSeconds(1) * 1000.0);
        EXPECT_EQ(p.client->State(), net::ClientState::Playing);
        EXPECT_EQ(p.client->AppliedOpSeq(), server.LastOpSeq());
        const voxel::WorldHashResult h = p.client->Hash();
        EXPECT_TRUE(h.Complete());
        EXPECT_EQ(h.hash, serverHash.hash) << "client " << i;
        EXPECT_EQ(p.client->Stats().skippedChunks, 0u);
        EXPECT_EQ(p.client->Stats().unsyncedChunks, 0u);
        EXPECT_GT(pr.compared, 100u);
        // Prediction quality on the wall clock: the test runs alone (RUN_SERIAL in tests/CMakeLists.txt),
        // and no participant stalls the others any more, so a hard resync is a regression.
        EXPECT_EQ(pr.hardResyncs, 0u);
        EXPECT_EQ(pr.snaps, 0u);
        EXPECT_LE(pr.corrections, pr.compared / 10);       // bound set here (measured: see networking.md "Stage 5 as built")
        EXPECT_TRUE(p.sawOther);
        EXPECT_GT(p.casts, 0u);
        EXPECT_GT(p.transport->Stats().datagramsDropped, 0u);
    }
}

// rendezvous.md "Punch datagram": STUN answers and punch datagrams share the ENet host's socket, and
// plain ENet (the intercept's raw taking off) drops both before any peer changes: "VX" read as an ENet
// header is peer id 1624, a STUN answer's 0x0101 peer id 257, both past the host's peer count. With the
// intercept on (always, in every build of this) they go to TakeRaw instead: a punch datagram always, a
// STUN answer only to a request this transport sent.
TEST(Enet, APunchOrStunDatagramNeverReachesENet) {
    net::Punch p;
    p.kind = net::PunchKind::Probe;
    p.sender = net::PunchRole::Client;
    p.nonceClient = 1;
    const auto punch = net::EncodePunch(p);
    net::StunTransaction asked{};
    for (size_t i = 0; i < asked.size(); ++i) asked[i] = static_cast<uint8_t>(0xA0 + i);
    net::StunTransaction stranger = asked;
    stranger[0] ^= 0xFF;

    for (const bool intercept : {false, true}) {
        SCOPED_TRACE(intercept ? "the transport's intercept" : "plain ENet");
        net::EnetSettings serverSettings;
        serverSettings.interceptRaw = intercept;
        UdpPair pair(serverSettings, {});
        ASSERT_TRUE(pair.Connected());
        std::string error;
        const std::unique_ptr<net::EnetTransport> sender = net::EnetTransport::Open("127.0.0.1", 0, {}, &error);
        ASSERT_TRUE(sender) << error;
        const net::Endpoint serverAt{net::Ipv4(127, 0, 0, 1), pair.server->Port()};
        const net::Endpoint senderAt{net::Ipv4(127, 0, 0, 1), sender->Port()};
        // The server asked the sender a STUN question (the sender's host drops it: not its transaction).
        const auto request = net::MakeStunBindingRequest(asked);
        pair.server->SendRaw(senderAt, request);
        for (int i = 0; i < 5; ++i) {
            sender->SendRaw(serverAt, punch);
            sender->SendRaw(serverAt, net::MakeStunBindingResponse(asked, senderAt));
            sender->SendRaw(serverAt, net::MakeStunBindingResponse(stranger, senderAt));
        }
        std::vector<net::TransportEvent> atServer, atClient;
        const Clock::time_point start = Clock::now();
        while (Since(start) < 0.5) {
            Drain(*pair.server, atServer);
            Drain(*pair.client, atClient);
            Drain(*sender, atClient);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_TRUE(atServer.empty());
        EXPECT_EQ(pair.server->PeerCount(), 1u);
        const std::vector<net::Datagram> raw = pair.server->TakeRaw();
        EXPECT_EQ(raw.size(), intercept ? 10u : 0u);
        for (const net::Datagram& d : raw) {
            EXPECT_EQ(d.address, senderAt);
            net::StunTransaction t{};
            EXPECT_TRUE(net::HasPunchMagic(d.bytes) || (net::LooksLikeStun(d.bytes, &t) && t == asked));
        }
        EXPECT_TRUE(sender->TakeRaw().empty());
        // The connection still carries messages both ways.
        const std::vector<uint8_t> hello = Message(7, 32);
        pair.client->Send(pair.atClient, net::Channel::Events, hello);
        pair.client->Flush();
        bool arrived = false;
        const Clock::time_point again = Clock::now();
        while (!arrived && Since(again) < 3.0) {
            net::TransportEvent e;
            while (pair.server->Poll(e))
                if (e.type == net::TransportEvent::Type::Received && e.data == hello) arrived = true;
            pair.client->Poll(e);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(arrived);
    }
}

// A client host made by Open (punching on its socket) takes no ENet connection from anyone: a stranger
// connecting to its port never gets its one peer slot. After ConnectTo it talks to its server only,
// and punch datagrams still reach TakeRaw.
TEST(Enet, AClientHostTakesNoStrangerWhilePunching) {
    std::string error;
    const std::unique_ptr<net::EnetTransport> client = net::EnetTransport::Open("127.0.0.1", 0, {}, &error);
    ASSERT_TRUE(client) << error;
    const net::Endpoint clientAt{net::Ipv4(127, 0, 0, 1), client->Port()};
    net::EnetSettings quick;
    quick.timeoutMinimumMs = 1000;
    quick.timeoutMaximumMs = 2000;
    const std::unique_ptr<net::EnetTransport> stranger = net::EnetTransport::Connect("127.0.0.1", client->Port(), quick, &error);
    ASSERT_TRUE(stranger) << error;
    std::vector<net::TransportEvent> atClient, atStranger;
    Clock::time_point start = Clock::now();
    while (Since(start) < 1.5) {
        Drain(*client, atClient);
        Drain(*stranger, atStranger);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(atClient.empty());
    for (const net::TransportEvent& e : atStranger) EXPECT_NE(e.type, net::TransportEvent::Type::Connected);
    EXPECT_EQ(client->PeerCount(), 0u);
    EXPECT_GT(client->Stats().strangersDropped, 0u);

    // Its server: the connection is made, the stranger stays out, punch datagrams still arrive.
    const std::unique_ptr<net::EnetTransport> server = net::EnetTransport::Listen("127.0.0.1", 0, {}, &error);
    ASSERT_TRUE(server) << error;
    ASSERT_TRUE(client->ConnectTo({net::Ipv4(127, 0, 0, 1), server->Port()}, &error)) << error;
    EXPECT_FALSE(client->ConnectTo({net::Ipv4(127, 0, 0, 1), server->Port()}, &error));
    net::Punch p;
    p.nonceClient = 3;
    const auto punch = net::EncodePunch(p);
    bool clientConnected = false, serverConnected = false;
    start = Clock::now();
    while ((!clientConnected || !serverConnected) && Since(start) < 5.0) {
        net::TransportEvent e;
        while (client->Poll(e)) clientConnected |= e.type == net::TransportEvent::Type::Connected;
        while (server->Poll(e)) serverConnected |= e.type == net::TransportEvent::Type::Connected;
        Drain(*stranger, atStranger);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(clientConnected);
    EXPECT_TRUE(serverConnected);
    stranger->SendRaw(clientAt, punch);
    start = Clock::now();
    std::vector<net::Datagram> raw;
    while (raw.empty() && Since(start) < 2.0) {
        net::TransportEvent e;
        while (client->Poll(e)) {}
        raw = client->TakeRaw();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(raw.size(), 1u);
    EXPECT_EQ(raw[0].address.port, stranger->Port());
    EXPECT_EQ(client->PeerCount(), 1u);
    for (const net::TransportEvent& e : atStranger) EXPECT_NE(e.type, net::TransportEvent::Type::Connected);
}
