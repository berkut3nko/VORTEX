// net::TickClock and net::RemotePlayers (docs/design/networking.md stage 4, "Time and ticks"):
// the client clock's asymmetric control fed with margin sequences, and other players drawn in
// the past.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

import vortex.core;
import vortex.net;

using namespace vortex;

namespace {
    constexpr double kTick = 1.0 / 60.0;

    /// Ticks the clock gives for `passes` passes of one tick each.
    uint32_t RunPasses(net::TickClock& clock, int passes) {
        uint32_t ticks = 0;
        for (int i = 0; i < passes; ++i) ticks += clock.TicksDue(kTick, kTick);
        return ticks;
    }
}

TEST(Clock, RunsOneTickPerTickWithoutReports) {
    net::TickClock clock;
    EXPECT_EQ(RunPasses(clock, 600), 600u);
    EXPECT_EQ(clock.Rate(), 1.0);
    EXPECT_EQ(clock.Target(), 2);
    // Half a tick at a time: one tick every second call.
    EXPECT_EQ(clock.TicksDue(kTick / 2, kTick), 0u);
    EXPECT_EQ(clock.TicksDue(kTick / 2, kTick), 1u);
    EXPECT_EQ(clock.TicksDue(-1.0, kTick), 0u);
}

// Margins below 0 in three consecutive reports: the clock jumps forward at once by what is
// missing to the target; reports about inputs made before the jump are ignored.
TEST(Clock, JumpsForwardAfterThreeLateReports) {
    net::TickClock clock;
    uint32_t next = 100;
    EXPECT_EQ(clock.Report(-3, 90, next, 80).jumpTicks, 0u);
    EXPECT_EQ(clock.Report(-3, 92, next, 82).jumpTicks, 0u);
    EXPECT_EQ(clock.Report(2, 94, next, 84).jumpTicks, 0u);      // one on time resets the run
    EXPECT_EQ(clock.Report(-3, 96, next, 86).jumpTicks, 0u);
    EXPECT_EQ(clock.Report(-3, 98, next, 88).jumpTicks, 0u);
    const net::ClockAction a = clock.Report(-3, 99, next, 90);
    EXPECT_EQ(a.jumpTicks, static_cast<uint32_t>(clock.Target() + 3));
    EXPECT_EQ(clock.TicksDue(kTick, kTick), 1u + a.jumpTicks);
    // Inputs made before the jump still report late margins: ignored.
    for (int i = 0; i < 5; ++i) EXPECT_EQ(clock.Report(-3, 99, next, 92 + 2 * static_cast<uint32_t>(i)).jumpTicks, 0u);
    next += 1 + a.jumpTicks;
    EXPECT_EQ(clock.Report(2, next, next, 100).jumpTicks, 0u);
}

// A surplus is bled off at 2 %: the clock runs slow while every margin of the window is above the target.
// Snapshots repeat the last margin while no newer input has arrived (an outage on the way up):
// the repeats are not new reports, so three of them about one late input make no jump.
TEST(Clock, RepeatedReportsAboutOneInputCountOnce) {
    net::TickClock clock;
    for (uint32_t i = 0; i < 10; ++i) EXPECT_EQ(clock.Report(-2, 500, 520, 480 + 2 * i).jumpTicks, 0u);
    EXPECT_EQ(clock.Report(-2, 502, 522, 500).jumpTicks, 0u);
    EXPECT_GT(clock.Report(-2, 504, 524, 502).jumpTicks, 0u);    // the third new late input
}

TEST(Clock, BleedsASurplusAtTwoPercent) {
    net::TickClock clock;
    for (uint32_t i = 0; i < 16; ++i) clock.Report(5, 100 + 2 * i, 120, 90 + 2 * i);
    EXPECT_EQ(clock.Rate(), 0.98);
    EXPECT_NEAR(static_cast<double>(RunPasses(clock, 500)), 490.0, 1.0);
    // A margin of 2 ends it, and since the jump from 5 raised the jitter estimate (target 3), it is
    // now a deficit: made up at 2 % (a clock placed short would otherwise stay short for good).
    clock.Report(2, 140, 640, 600);
    EXPECT_EQ(clock.Target(), 3);
    EXPECT_EQ(clock.Rate(), 1.02);
    EXPECT_NEAR(static_cast<double>(RunPasses(clock, 500)), 510.0, 1.0);
    // At the target: neither.
    net::TickClock steady;
    for (uint32_t i = 0; i < 16; ++i) steady.Report(2, 150 + 2 * i, 700, 610 + 2 * i);
    EXPECT_EQ(steady.Rate(), 1.0);
}

// The server reports the margin of a packet's newest input; the oldest new one in it is
// packetTicks - 1 less early, which the target adds.
TEST(Clock, TargetCoversThePacketInterval) {
    net::ClockSettings settings;
    settings.packetTicks = 2;
    net::TickClock clock(settings);
    EXPECT_EQ(clock.Target(), 3);
}

// Above 8 ticks for a second (60 server ticks): hard resync, the clock holds until the margin is
// back at the target. A server hitch recovers before that and makes no resync.
TEST(Clock, HoldsAfterASecondFarAheadButNotForAHitch) {
    net::TickClock hitch;
    uint32_t snap = 1000;
    for (int i = 0; i < 20; ++i, snap += 2) EXPECT_EQ(hitch.Report(12, snap + 12, snap + 20, snap).holdTicks, 0u);   // 40 ticks
    for (int i = 0; i < 30; ++i, snap += 2) EXPECT_EQ(hitch.Report(3, snap + 3, snap + 10, snap).holdTicks, 0u);
    for (int i = 0; i < 25; ++i, snap += 2) EXPECT_EQ(hitch.Report(12, snap + 12, snap + 20, snap).holdTicks, 0u);   // 48 ticks again

    net::TickClock ahead;
    snap = 1000;
    net::ClockAction a;
    int reports = 0;
    while (a.holdTicks == 0 && reports < 100) {
        a = ahead.Report(14, snap + 14, snap + 20, snap);
        snap += 2;
        ++reports;
    }
    EXPECT_EQ(reports, 31);                                      // 60 server ticks after the first
    EXPECT_EQ(a.holdTicks, static_cast<uint32_t>(14 - ahead.Target()));
    EXPECT_EQ(RunPasses(ahead, 30), 30u - a.holdTicks);
}

// Review of stage 4: with jitter the target can be above the resync bound (8). A margin of 9-13
// is then not a surplus, so it must neither start the hold timer nor ask for a hold of
// `margin - target` < 0 ticks (it wrapped to about 4e9 and froze the clock for good).
TEST(Clock, AHoldNeverExceedsTheSurplusWhenTheTargetIsAboveTheResyncBound) {
    net::ClockSettings settings;
    settings.packetTicks = 2;
    net::TickClock clock(settings);
    uint32_t snap = 1000;
    uint32_t held = 0;
    for (int i = 0; i < 120; ++i, snap += 2) {
        const int32_t margin = i % 2 == 0 ? 9 : 13;                 // a change of 4 every report: jitter -> 4
        const net::ClockAction a = clock.Report(margin, snap + 20, snap + 21, snap);
        EXPECT_LE(a.holdTicks, 13u);
        held += a.holdTicks;
    }
    EXPECT_GT(clock.Target(), 8);
    EXPECT_EQ(held, 0u);                                             // every margin of 9 is below the target
    EXPECT_GE(RunPasses(clock, 60), 55u);                            // the clock runs
    // A steady surplus above both the bound and the target is still held off, by the surplus.
    net::TickClock ahead(settings);
    snap = 1000;
    net::ClockAction a;
    for (int i = 0; i < 40 && a.holdTicks == 0; ++i, snap += 2) a = ahead.Report(20, snap + 20, snap + 30, snap);
    EXPECT_EQ(a.holdTicks, static_cast<uint32_t>(20 - ahead.Target()));
}

// The server time a client draws other players at (review of stage 4): it only moves forward,
// however late, early or out of order the snapshots arrive, and it follows the snapshots' ticks.
TEST(ServerTime, MovesOnlyForwardWithJitteredSnapshots) {
    net::ServerTime time;
    Rng rng(99);
    std::vector<std::pair<uint32_t, uint32_t>> inFlight;   // (arrival tick, snapshot tick)
    double previous = -1.0;
    uint32_t newest = 0;
    bool any = false;
    double worstLag = 0.0;
    for (uint32_t t = 0; t < 1200; ++t) {
        time.Advance(1.0);
        if (t % 2 == 0) inFlight.push_back({t + 3 + rng.NextBounded(5), t});   // 3-7 ticks on the way
        for (auto it = inFlight.begin(); it != inFlight.end();) {
            if (it->first != t) {
                ++it;
                continue;
            }
            if (!any || it->second > newest) {
                any = true;
                newest = it->second;
                time.Observe(it->second);
            }
            it = inFlight.erase(it);
        }
        if (!any) continue;
        EXPECT_GT(time.Now(), previous);
        previous = time.Now();
        if (t > 300) worstLag = std::max(worstLag, std::abs(time.Now() - (static_cast<double>(t) - 5.0)));
    }
    EXPECT_LT(worstLag, 2.5);   // about the mean delay behind the server, within the jitter
}

// The target follows the measured jitter: the mean change of consecutive margins.
TEST(Clock, TargetGrowsWithJitter) {
    net::TickClock clock;
    for (uint32_t i = 0; i < 200; ++i) clock.Report(i % 2 == 0 ? 2 : 4, 100 + 2 * i, 100 + 2 * i, 2 * i);
    EXPECT_GT(clock.Jitter(), 1.5f);
    EXPECT_EQ(clock.Target(), 2 + 4);
}

TEST(RemotePlayers, InterpolatesInThePastExtrapolatesBrieflyAndForgetsWhoLeft) {
    net::RemotePlayers players;
    for (uint32_t t = 0; t <= 20; t += 2) {
        std::vector<net::RemotePlayer> others = {{3, glm::vec3(static_cast<float>(t), 0.0f, 0.0f), 100, 1}};
        if (t < 10) others.push_back({5, glm::vec3(0.0f, 0.0f, static_cast<float>(t)), 0, 0});
        players.Add(t, others);
    }
    EXPECT_EQ(players.Count(), 1u);                              // slot 5 left at tick 10
    // Updates every 2 ticks: drawn 4 ticks (67 ms) in the past.
    std::vector<net::RemotePlayerView> v = players.Sample(15.0);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].slot, 3);
    EXPECT_NEAR(v[0].position.x, 11.0f, 1.0e-4f);
    // Past the newest sample: one interval of extrapolation, then hold.
    EXPECT_NEAR(players.Sample(25.0)[0].position.x, 21.0f, 1.0e-4f);
    EXPECT_NEAR(players.Sample(40.0)[0].position.x, 22.0f, 1.0e-4f);
    // Before the oldest kept sample (8 are kept): the oldest.
    EXPECT_NEAR(players.Sample(0.0)[0].position.x, 6.0f, 1.0e-4f);
    // A stale or repeated tick adds nothing.
    players.Add(18, {{3, glm::vec3(100.0f), 0, 0}});
    EXPECT_NEAR(players.Sample(15.0)[0].position.x, 11.0f, 1.0e-4f);
}

// Review of stage 5: a caller that can run only `maxTicks` a frame (the engine: 8) gets the rest of a
// long frame and of a jump on the next frames, never fewer ticks in all: a 0.4 s frame then 1/60 s
// frames gives as many ticks as 1/60 s frames all along, and a 20-tick jump is run as 8 + 8 + 4.
TEST(Clock, TicksAboveTheCapAreCarriedToTheNextCalls) {
    net::TickClock smooth, capped;
    uint32_t a = 0, b = 0;
    for (int i = 0; i < 24; ++i) a += smooth.TicksDue(kTick, kTick, 8);
    b += capped.TicksDue(24 * kTick + 1.0e-9, kTick, 8);   // a 0.4 s frame
    EXPECT_EQ(b, 8u);
    for (int i = 0; i < 6; ++i) b += capped.TicksDue(0.0, kTick, 8);
    EXPECT_EQ(a, b);
    EXPECT_EQ(capped.TicksDue(0.0, kTick, 8), 0u);

    net::TickClock clock;
    uint32_t next = 100;
    (void)clock.Report(-18, 90, next, 80);
    (void)clock.Report(-18, 92, next, 82);
    const net::ClockAction jump = clock.Report(-18, 94, next, 84);
    ASSERT_EQ(jump.jumpTicks, static_cast<uint32_t>(clock.Target() + 18));
    ASSERT_EQ(jump.jumpTicks, 20u);
    EXPECT_EQ(clock.TicksDue(0.0, kTick, 8), 8u);
    EXPECT_EQ(clock.TicksDue(0.0, kTick, 8), 8u);
    EXPECT_EQ(clock.TicksDue(0.0, kTick, 8), 4u);
    EXPECT_EQ(clock.TicksDue(0.0, kTick, 8), 0u);
}
