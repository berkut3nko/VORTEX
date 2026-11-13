// docs/design/networking.md stage 3: the loopback transport and the impaired decorator. Reliable
// channels keep ENet's promise (every message, once, in order) under loss, jitter and an outage;
// the state channel loses, duplicates and reorders.

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

import vortex.core;
import vortex.net;

using namespace vortex;

namespace {
    std::vector<uint8_t> Packet(uint32_t n) { return {static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n >> 16)}; }
    uint32_t Number(const std::vector<uint8_t>& p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
}

TEST(Transport, LoopbackConnectsDeliversInOrderAndDisconnects) {
    net::LoopbackTransport a, b;
    const net::PeerId bOfA = net::LoopbackTransport::Connect(a, b);
    net::TransportEvent e;
    ASSERT_TRUE(a.Poll(e));
    EXPECT_EQ(e.type, net::TransportEvent::Type::Connected);
    EXPECT_EQ(e.peer, bOfA);
    ASSERT_TRUE(b.Poll(e));
    ASSERT_EQ(e.type, net::TransportEvent::Type::Connected);
    const net::PeerId aOfB = e.peer;
    for (uint32_t i = 0; i < 10; ++i) a.Send(bOfA, net::Channel::Events, Packet(i));
    EXPECT_EQ(a.ReliableBytesInFlight(bOfA), 30u);
    for (uint32_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(b.Poll(e));
        EXPECT_EQ(e.peer, aOfB);
        EXPECT_EQ(Number(e.data), i);
    }
    EXPECT_EQ(a.ReliableBytesInFlight(bOfA), 0u);
    EXPECT_FALSE(b.Poll(e));
    a.Disconnect(bOfA);
    ASSERT_TRUE(b.Poll(e));
    EXPECT_EQ(e.type, net::TransportEvent::Type::Disconnected);
    a.Send(bOfA, net::Channel::Events, Packet(1));      // gone: ignored
    EXPECT_FALSE(b.Poll(e));
}

TEST(Transport, ImpairedKeepsReliableOrderAndLosesOnlyStatePackets) {
    double clock = 0.0;
    net::LoopbackTransport a, b;
    net::Impairment im;
    im.loss = 0.05;
    im.delaySeconds = 0.04;
    im.jitterSeconds = 0.03;
    im.duplicate = 0.02;
    im.reorder = 0.05;
    im.outages.push_back({2.0, 2.2});
    net::ImpairedTransport link(a, im, 1234, [&clock] { return clock; });
    const net::PeerId peer = net::LoopbackTransport::Connect(a, b);
    net::TransportEvent e;
    while (link.Poll(e)) {}
    while (b.Poll(e)) {}

    std::vector<uint32_t> reliable, state;
    double maxLatency = 0.0;
    std::vector<double> sentAt;
    for (uint32_t tick = 0; tick < 600; ++tick) {    // 10 s at 60 Hz
        link.Send(peer, net::Channel::Events, Packet(tick));
        link.Send(peer, net::Channel::State, Packet(tick));
        sentAt.push_back(clock);
        clock += 1.0 / 60.0;
        link.Poll(e);                                // flushes what is due (a has nothing to read)
        while (b.Poll(e)) {
            if (e.type != net::TransportEvent::Type::Received) continue;
            if (e.channel == net::Channel::Events) {
                reliable.push_back(Number(e.data));
                maxLatency = std::max(maxLatency, clock - sentAt[Number(e.data)]);
            } else {
                state.push_back(Number(e.data));
            }
        }
    }
    for (int i = 0; i < 120; ++i) {                  // drain
        clock += 1.0 / 60.0;
        link.Poll(e);
        while (b.Poll(e))
            if (e.type == net::TransportEvent::Type::Received && e.channel == net::Channel::Events) reliable.push_back(Number(e.data));
    }
    ASSERT_EQ(reliable.size(), 600u);
    for (uint32_t i = 0; i < 600; ++i) EXPECT_EQ(reliable[i], i);
    EXPECT_EQ(link.Queued(), 0u);
    const net::ImpairmentStats& s = link.Stats();
    EXPECT_GT(s.resent, 15u);                        // 5 % of 600 plus the outage
    EXPECT_GT(s.lost, 15u);
    EXPECT_GT(s.duplicated, 3u);
    EXPECT_GT(s.reordered, 10u);
    EXPECT_LT(state.size(), 600u);
    bool outOfOrder = false;
    for (size_t i = 1; i < state.size(); ++i) outOfOrder = outOfOrder || state[i] <= state[i - 1];
    EXPECT_TRUE(outOfOrder);                         // reordered or duplicated
    // Head-of-line blocking behind the 200 ms outage: latency above the outage, below a second.
    EXPECT_GT(maxLatency, 0.2);
    EXPECT_LT(maxLatency, 1.0);
}

TEST(Transport, RateLimitQueuesPackets) {
    double clock = 0.0;
    net::LoopbackTransport a, b;
    net::Impairment im;
    im.bytesPerSecond = 1000.0;
    net::ImpairedTransport link(a, im, 1, [&clock] { return clock; });
    const net::PeerId peer = net::LoopbackTransport::Connect(a, b);
    net::TransportEvent e;
    while (b.Poll(e)) {}
    const std::vector<uint8_t> kilobyte(1000, 7);
    for (int i = 0; i < 5; ++i) link.Send(peer, net::Channel::Bulk, kilobyte);
    EXPECT_EQ(link.ReliableBytesInFlight(peer), 5000u);
    int received = 0;
    for (int step = 0; step < 6; ++step) {
        clock = step + 0.5;                          // 1 kB per second
        link.Poll(e);
        while (b.Poll(e)) received += e.type == net::TransportEvent::Type::Received ? 1 : 0;
        EXPECT_EQ(received, std::min(step, 5));
    }
}
