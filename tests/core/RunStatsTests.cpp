// Run-wide frame statistics and the `Perf:` exit line (perf.md M0, docs/verify.md).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

import vortex.core;

using namespace vortex;

TEST(RunStats, PercentilesOfAKnownSeries) {
    RunStats stats;
    for (int v = 100; v >= 1; --v) stats.Add(static_cast<double>(v));
    EXPECT_EQ(stats.Count(), 100u);
    EXPECT_EQ(stats.Percentile(0), 1.0);
    EXPECT_EQ(stats.Percentile(50), 50.0);
    EXPECT_EQ(stats.Percentile(90), 90.0);
    EXPECT_EQ(stats.Percentile(99), 99.0);
    EXPECT_EQ(stats.Percentile(100), 100.0);
    EXPECT_EQ(stats.Percentile(250), 100.0);   // clamped to 100
    EXPECT_EQ(stats.Max(), 100.0);
    stats.Add(NAN);
    stats.Add(INFINITY);
    EXPECT_EQ(stats.Count(), 100u);

    const RunStats fresh;
    EXPECT_EQ(fresh.Count(), 0u);
    EXPECT_EQ(fresh.Percentile(50), 0.0);
    EXPECT_EQ(fresh.Max(), 0.0);

    stats.Clear();
    EXPECT_EQ(stats.Count(), 0u);
    EXPECT_EQ(stats.Percentile(50), 0.0);
    EXPECT_EQ(stats.Max(), 0.0);
    EXPECT_EQ(stats.CountOver(0.0), 0u);
}

TEST(RunStats, CountsFramesOverABudget) {
    RunStats stats;
    for (double v : {16.6, 25.0, 25.01, 33.0, 33.4, 50.0}) stats.Add(v);
    EXPECT_EQ(stats.CountOver(25.0), 4u);   // strictly greater
    EXPECT_EQ(stats.CountOver(33.0), 2u);
    EXPECT_EQ(stats.CountOver(100.0), 0u);
}

TEST(RunStats, PerfLineNamesEveryPassInOrder) {
    PerfSummary perf;
    for (double v : {10.0, 20.0, 30.0, 40.0}) perf.cpuFrame.Add(v);
    perf.cpuWorld.Add(1.0);
    perf.cpuWorld.Add(2.0);
    perf.cpuSimulation.Add(3.0);
    perf.fenceWait.Add(0.5);
    perf.GpuPass("gbuffer").Add(12.0);
    perf.GpuPass("lighting").Add(7.0);
    perf.GpuPass("upload").Add(0.25);
    EXPECT_EQ(perf.GpuPass("gbuffer").Count(), 1u);   // found again, not appended
    EXPECT_EQ(perf.gpuPasses.size(), 3u);
    EXPECT_EQ(perf.FindGpuPass("taa"), nullptr);
    EXPECT_EQ(FormatPerfLine(perf, "fifo-relaxed"),
              "Perf: 4 frames; CPU frame p50 20.00 p99 40.00 max 40.00 ms, world p50 1.00 p99 2.00 ms, simulation p50 3.00 p99 3.00 ms, "
              "fence wait p50 0.50 p99 0.50 ms; over 25 ms 2, over 33 ms 1; GPU sum p50 19.00 ms (gbuffer 12.00, lighting 7.00); "
              "upload p50 0.25 ms; present fifo-relaxed");
    EXPECT_EQ(FormatPerfLine(PerfSummary{}, "headless"),
              "Perf: 0 frames; CPU frame p50 0.00 p99 0.00 max 0.00 ms, world p50 0.00 p99 0.00 ms, simulation p50 0.00 p99 0.00 ms, "
              "fence wait p50 0.00 p99 0.00 ms; over 25 ms 0, over 33 ms 0; GPU sum p50 0.00 ms (none); upload p50 0.00 ms; "
              "present headless");
}
