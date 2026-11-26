// sim::ZoneSet: the zones every peer evaluates inside StepPlayer (docs/design/magic.md "sim::ZoneSet").
// A sorted set; a cancel only truncates; every lookup takes the tick being stepped.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

import vortex.voxel;
import vortex.sim;

using namespace vortex;

namespace {
    sim::Zone TestZone(uint16_t id, uint32_t start, uint16_t duration = 1, sim::ZoneKind kind = sim::ZoneKind::Impulse) {
        sim::Zone z;
        z.zoneId = id;
        z.kind = kind;
        z.startTick = start;
        z.duration = duration;
        z.shape.kind = voxel::kShapeSphere;
        z.shape.a = {0, 0, 0};
        z.shape.radiusQ = 64;
        return z;
    }

    uint32_t CountLive(const sim::ZoneSet& set, uint32_t tick) {
        uint32_t n = 0;
        set.ForEachLive(tick, glm::ivec3(-16), glm::ivec3(16), [&n](const sim::Zone&) { ++n; });
        return n;
    }
}

TEST(ZoneSet, ZonesAreKeptInStartThenIdOrder) {
    sim::ZoneSet set;
    EXPECT_TRUE(set.Insert(TestZone(5, 10)));
    EXPECT_TRUE(set.Insert(TestZone(3, 10)));
    EXPECT_TRUE(set.Insert(TestZone(9, 4)));
    EXPECT_FALSE(set.Insert(TestZone(3, 50))) << "an id already present";
    ASSERT_EQ(set.Size(), 3u);
    EXPECT_EQ(set.All()[0].zoneId, 9);
    EXPECT_EQ(set.All()[1].zoneId, 3);
    EXPECT_EQ(set.All()[2].zoneId, 5);

    std::vector<uint16_t> seen;
    set.ForEachLive(10, glm::ivec3(-16), glm::ivec3(16), [&seen](const sim::Zone& z) { seen.push_back(z.zoneId); });
    EXPECT_EQ(seen, (std::vector<uint16_t>{3, 5}));
}

TEST(ZoneSet, CancelOnlyTruncates) {
    sim::ZoneSet set;
    ASSERT_TRUE(set.Insert(TestZone(1, 100, 90, sim::ZoneKind::Lift)));
    EXPECT_TRUE(set.Cancel(1, 130));
    EXPECT_EQ(set.Find(1)->duration, 30);
    EXPECT_TRUE(set.Cancel(1, 200));
    EXPECT_EQ(set.Find(1)->duration, 30) << "a cancel never lengthens";
    EXPECT_TRUE(set.Cancel(1, 90));
    EXPECT_EQ(set.Find(1)->duration, 0);
    EXPECT_EQ(CountLive(set, 100), 0u);
    EXPECT_FALSE(set.Cancel(2, 120));
    EXPECT_EQ(set.Find(2), nullptr);
}

TEST(ZoneSet, LookupsUseTheGivenTick) {
    sim::ZoneSet set;
    ASSERT_TRUE(set.Insert(TestZone(1, 100, 90, sim::ZoneKind::Lift)));
    EXPECT_EQ(CountLive(set, 99), 0u);
    EXPECT_EQ(CountLive(set, 100), 1u);
    EXPECT_EQ(CountLive(set, 189), 1u);
    EXPECT_EQ(CountLive(set, 190), 0u);
    EXPECT_EQ(set.LiveOrPendingAt(50), 1u);
    EXPECT_EQ(set.LiveOrPendingAt(190), 0u);

    // A reach box that misses the zone's box sees nothing, at any tick.
    uint32_t far = 0;
    set.ForEachLive(120, glm::ivec3(65), glm::ivec3(200), [&far](const sim::Zone&) { ++far; });
    EXPECT_EQ(far, 0u);

    set.PruneEndedBefore(150);
    EXPECT_EQ(set.Size(), 1u) << "kept while a tick that may still be stepped sees it";
    set.PruneEndedBefore(190);
    EXPECT_EQ(set.Size(), 0u);
}

TEST(ZoneSet, FullSetRefuses) {
    sim::ZoneSet set;
    for (uint16_t i = 0; i < sim::kMaxZoneRecords; ++i) EXPECT_TRUE(set.Insert(TestZone(i, i)));
    EXPECT_FALSE(set.Insert(TestZone(1000, 5)));
    EXPECT_EQ(set.Size(), sim::kMaxZoneRecords);
}

TEST(ZoneSet, BoundsCoverEachShape) {
    sim::Zone z = TestZone(1, 0);
    glm::ivec3 lo, hi;
    z.shape = {};
    z.shape.kind = voxel::kShapeSphere;
    z.shape.a = {10, 20, 30};
    z.shape.radiusQ = 5;
    sim::ZoneBoundsQ(z, lo, hi);
    EXPECT_EQ(lo, glm::ivec3(5, 15, 25));
    EXPECT_EQ(hi, glm::ivec3(15, 25, 35));

    z.shape.kind = voxel::kShapeCapsule;
    z.shape.b = {0, 40, 30};
    sim::ZoneBoundsQ(z, lo, hi);
    EXPECT_EQ(lo, glm::ivec3(-5, 15, 25));
    EXPECT_EQ(hi, glm::ivec3(15, 45, 35));

    z.shape.kind = voxel::kShapeCylinder;
    z.shape.halfHeightQ = 7;
    sim::ZoneBoundsQ(z, lo, hi);
    EXPECT_EQ(lo, glm::ivec3(5, 13, 25));
    EXPECT_EQ(hi, glm::ivec3(15, 27, 35));

    z.shape.kind = voxel::kShapeBox;
    z.shape.a = {1, 2, 3};
    z.shape.b = {2, 4, 6};
    sim::ZoneBoundsQ(z, lo, hi);
    EXPECT_EQ(lo, glm::ivec3(16, 32, 48));
    EXPECT_EQ(hi, glm::ivec3(31, 63, 95));
}
