// docs/design/shred.md Part 4b (stage 4b): weak necks. The load check after a blast's graph island
// search: every judged node supplies its mass, every bond carries faces x strength by direction
// (compression / shear / tension), anchors are what reaches the flood box face; the largest
// strictly overloaded set is released. Tested against brute force (abstract networks), a per-voxel
// reference (random volumes), the owner's watch tower on a thin neck and the natural cases that
// must stand.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <initializer_list>
#include <set>
#include <string>
#include <tuple>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    constexpr uint64_t kM = uint64_t{1} << 17;      // the perturbation factor of shred.md Part 4b

    // --- abstract networks ------------------------------------------------------------------------

    /// The perturbed value of a set (bit i = node i): M x (mass - cap(out)) - (finite arcs out);
    /// INT64_MIN when an infinite arc leaves it.
    int64_t SetValue(const NeckNetwork& n, uint32_t set) {
        int64_t mass = 0, cap = 0, arcs = 0;
        for (uint32_t i = 0; i < n.supply.size(); ++i)
            if ((set >> i) & 1u) mass += static_cast<int64_t>(n.supply[i]);
        for (const NeckNetwork::Arc& a : n.arcs) {
            const bool inFrom = (set >> a.from) & 1u;
            const bool inTo = a.to != NeckNetwork::kNeckSink && ((set >> a.to) & 1u);
            if (inFrom && !inTo) {
                if (a.cap == NeckNetwork::kInfinite) return std::numeric_limits<int64_t>::min();
                cap += static_cast<int64_t>(a.cap);
                ++arcs;
            }
            if (a.to != NeckNetwork::kNeckSink && inTo && !inFrom) {
                if (a.capBack == NeckNetwork::kInfinite) return std::numeric_limits<int64_t>::min();
                cap += static_cast<int64_t>(a.capBack);
                ++arcs;
            }
        }
        return static_cast<int64_t>(kM) * (mass - cap) - arcs;
    }

    NeckNetwork RandomNetwork(std::mt19937& rng, uint32_t nodes) {
        NeckNetwork n;
        std::uniform_int_distribution<uint64_t> mass(0, 20), cap(0, 12);
        std::uniform_int_distribution<uint32_t> pick(0, nodes - 1), coin(0, 9);
        for (uint32_t i = 0; i < nodes; ++i) n.supply.push_back(coin(rng) < 2 ? 0 : mass(rng));
        const uint32_t arcs = nodes + pick(rng) * 2;
        for (uint32_t k = 0; k < arcs; ++k) {
            const uint32_t a = pick(rng), b = pick(rng);
            if (a == b) continue;
            n.arcs.push_back({a, b, cap(rng), cap(rng)});
        }
        for (uint32_t i = 0; i < nodes; ++i) {
            const uint32_t c = coin(rng);
            if (c < 3) n.arcs.push_back({i, NeckNetwork::kNeckSink, cap(rng) * 3, 0});
            else if (c == 3) n.arcs.push_back({i, NeckNetwork::kNeckSink, NeckNetwork::kInfinite, 0});
        }
        return n;
    }

    // --- worlds -----------------------------------------------------------------------------------

    /// Flat stone ground (voxels y < `ground`), a window of 8 x 8 x 8 chunks around `focus` (metres).
    struct NeckWorld {
        NeckWorld(float voxelSize, int ground, const glm::dvec3& focus, unsigned threads = 3) : store(4096), jobs(threads), vs(voxelSize) {
            WorldConfig config;
            config.voxelSize = voxelSize;
            config.windowSize = {8, 8, 8};
            ids = RegisterDefaultWorldMaterials(materials);
            world = std::make_unique<World>(config, store, jobs, CreateFlatGenerator(ground, ids.stone, ids.stone), &materials);
            world->LoadBlocking(focus);
            graphMaterials = MakeGraphMaterials(&materials, MaterialFlag_Liquid);
            neck = MakeNeckLimits(graphMaterials, voxelSize);
        }
        /// The island search with the load check of a dud (nothing carved) centred in voxel `at`.
        GraphSearchResult Check(const glm::ivec3& at, int radius = 1, const NeckLimits* limitsOverride = nullptr,
                                const GraphMaterials* materialsOverride = nullptr) {
            const SphereQ sphere{at * kQuantaPerVoxel + glm::ivec3(kQuantaPerVoxel / 2), static_cast<uint16_t>(radius * kQuantaPerVoxel)};
            const glm::ivec3 c = math::FloorDiv(sphere.center, kQuantaPerVoxel);
            const glm::ivec3 half(static_cast<int>(sphere.radius / kQuantaPerVoxel) + 24);
            GraphSearchLimits limits;
            limits.boxMin = c - half;
            limits.boxMax = c + half;
            limits.floodMin = c - glm::ivec3(kFloodHalfExtent);
            limits.floodMax = c + glm::ivec3(kFloodHalfExtent);
            limits.maxIslandVoxels = 60000;
            limits.maxNodeVisits = 32768;
            limits.neck = limitsOverride ? limitsOverride : &neck;
            GraphSearchResult r = SearchIslandsOnGraph(*world, cache, materialsOverride ? *materialsOverride : graphMaterials, limits);
            EXPECT_TRUE(r.searched);
            EXPECT_EQ(r.relabelMismatch, 0u);
            return r;
        }
        ChunkStore store;
        JobSystem jobs;
        float vs;
        MaterialLibrary materials;
        WorldMaterials ids;
        std::unique_ptr<World> world;
        GraphMaterials graphMaterials;
        NeckLimits neck;
        TerrainGraphCache cache;
    };

    uint32_t CountVoxels(const std::vector<FloatingIsland>& pieces, const glm::ivec3& lo, const glm::ivec3& hi) {
        uint32_t n = 0;
        for (const FloatingIsland& p : pieces) {
            const glm::ivec3 s = p.volume.Size();
            for (int z = 0; z < s.z; ++z)
                for (int y = 0; y < s.y; ++y)
                    for (int x = 0; x < s.x; ++x) {
                        if (p.volume.Get(x, y, z) == 0) continue;
                        const glm::ivec3 v = p.origin + glm::ivec3(x, y, z);
                        n += glm::all(glm::greaterThanEqual(v, lo)) && glm::all(glm::lessThan(v, hi)) ? 1u : 0u;
                    }
        }
        return n;
    }
    uint32_t CountWorld(const World& w, const glm::ivec3& lo, const glm::ivec3& hi) {
        uint32_t n = 0;
        for (int z = lo.z; z < hi.z; ++z)
            for (int y = lo.y; y < hi.y; ++y)
                for (int x = lo.x; x < hi.x; ++x) n += w.GetVoxel({x, y, z}) != 0 ? 1u : 0u;
        return n;
    }

    /// The game's watch tower (src/main.cpp BuildPlayground, authored in 0.1 m units, S(x) =
    /// lround(x x 0.1 / vs)) with its floor at world voxel `c` (ring row 0 at c.y), on a stone_dark
    /// paving row at c.y - 1.
    struct Tower {
        NeckWorld& w;
        glm::ivec3 c;
        double scale;
        Tower(NeckWorld& world, const glm::ivec3& floor) : w(world), c(floor), scale(0.1 / static_cast<double>(world.vs)) {}
        int S(int v) const { return static_cast<int>(std::lround(static_cast<double>(v) * scale)); }
        glm::ivec3 S(const glm::ivec3& v) const { return {S(v.x), S(v.y), S(v.z)}; }
        void Box(const glm::ivec3& lo, const glm::ivec3& hi, uint8_t id) {
            const glm::ivec3 a = S(lo);
            w.world->FillBox(c + a, c + glm::max(S(hi), a + 1), id);
        }
        void Build(uint8_t ringFloor = 0) {
            const WorldMaterials& m = w.ids;
            // Ground up to the floor, the paving, air above.
            w.world->FillBox(c + S(glm::ivec3(10, -40, -80)), c + S(glm::ivec3(80, 0, -10)), m.stone);
            Box({10, -1, -80}, {80, 0, -10}, m.stoneDark);
            Box({10, 0, -80}, {80, 100, -10}, 0);
            Box({30, 0, -60}, {58, 70, -32}, m.stone);
            Box({33, 0, -57}, {55, 66, -35}, 0);
            Box({41, 0, -35}, {47, 20, -32}, 0);
            Box({28, 66, -62}, {60, 70, -30}, m.planks);
            for (int i = 0; i < 4; ++i) {
                const glm::ivec3 p(i & 1 ? 58 : 28, 70, i & 2 ? -32 : -62);
                Box(p, p + glm::ivec3(2, 8, 2), m.wood);
                Box(p + glm::ivec3(0, 8, 0), p + glm::ivec3(2, 10, 2), m.torch);
            }
            if (ringFloor != 0) {
                // Ring row 0 of another material (the rest of the footprint stays as built).
                for (int z = c.z + S(-60); z < c.z + S(-32); ++z)
                    for (int x = c.x + S(30); x < c.x + S(58); ++x)
                        if (w.world->GetVoxel({x, c.y, z}) != 0) w.world->FillBox({x, c.y, z}, {x + 1, c.y + 1, z + 1}, ringFloor);
            }
        }
        int NeckRows() const { return static_cast<int>(std::lround(1.5 / static_cast<double>(w.vs))); }
        /// Rows 1..NeckRows of the ring to air except a stone segment 2 x `width` at the
        /// north-west corner inside the wall line (x S(30)..+2, z S(-60)..+width).
        void Neck(int width, int thick = 2) {
            const int top = NeckRows();
            w.world->FillBox(c + glm::ivec3(S(30), 1, S(-60)), c + glm::ivec3(S(58), top + 1, S(-32)), 0);
            w.world->FillBox(c + glm::ivec3(S(30), 1, S(-60)), c + glm::ivec3(S(30) + thick, top + 1, S(-60) + width), w.ids.stone);
        }
        /// A neck at each corner, thick x width.
        void FourNecks(int width, int thick = 2) {
            const int top = NeckRows();
            w.world->FillBox(c + glm::ivec3(S(30), 1, S(-60)), c + glm::ivec3(S(58), top + 1, S(-32)), 0);
            for (int i = 0; i < 4; ++i) {
                const int x = (i & 1) ? S(58) - thick : S(30), z = (i & 2) ? S(-32) - width : S(-60);
                w.world->FillBox(c + glm::ivec3(x, 1, z), c + glm::ivec3(x + thick, top + 1, z + width), w.ids.stone);
            }
        }
        /// The dud of the tests: in the hollow, 0.5 m above the floor.
        glm::ivec3 Hollow() const { return c + glm::ivec3(S(44), S(5), S(-46)); }
        glm::ivec3 Min() const { return c + glm::ivec3(S(28), 0, S(-62)); }
        glm::ivec3 Max() const { return c + glm::ivec3(S(60), S(80), S(-30)); }
        glm::ivec3 Roof() const { return c + S(glm::ivec3(44, 68, -46)); }
    };

    /// A floor corner whose x / z / y offsets mod 8 put the neck (x S(30), z S(-60), floor row)
    /// at the wanted layer of its cell, near voxel (0, 68, 0) (the windows are centred there).
    glm::ivec3 Floor(double scale, int neckX, int neckZ, int floorY) {
        const auto S = [scale](int v) { return static_cast<int>(std::lround(static_cast<double>(v) * scale)); };
        const auto Fit = [](int base, int offset, int layer) {
            const int at = ((base + offset) % 8 + 8) % 8;
            return base + ((layer - at) % 8 + 8) % 8;
        };
        return {Fit(-S(44), S(30), neckX), Fit(64, 0, floorY), Fit(S(46), S(-60), neckZ)};
    }
    const glm::dvec3 kFocusTower(0.0, 8.5, 0.0);    // metres at 0.125 m: voxel (0, 68, 0)
}

// --- the rule on abstract networks ----------------------------------------------------------------

TEST(Neck, MaxFlowReleasesTheLargestOverloadedSet) {
    std::mt19937 rng(4711);
    uint32_t released = 0, networks = 0;
    for (int trial = 0; trial < 240; ++trial) {
        const uint32_t nodes = 2 + static_cast<uint32_t>(trial % 15);
        const NeckNetwork n = RandomNetwork(rng, nodes);
        const NeckFlowResult r = SolveNeckNetwork(n, 1u << 24);
        ASSERT_TRUE(r.solved);
        ASSERT_EQ(r.released.size(), nodes);
        // Brute force: the union of every maximiser of the perturbed value - if any set is strictly
        // overloaded (best > 0). Otherwise all supply reaches the sink and nothing is released,
        // even a weightless part with no support at all (that is the island search's business).
        int64_t best = 0;
        for (uint32_t set = 1; set < (1u << nodes); ++set) best = std::max(best, SetValue(n, set));
        uint32_t expected = 0;
        for (uint32_t set = 1; set < (1u << nodes) && best > 0; ++set)
            if (SetValue(n, set) == best) expected |= set;
        uint32_t got = 0;
        for (uint32_t i = 0; i < nodes; ++i) got |= r.released[i] ? (1u << i) : 0u;
        EXPECT_EQ(got, expected) << "trial " << trial;
        if (got != expected) {
            for (uint32_t i = 0; i < nodes; ++i) std::printf("supply %u: %llu\n", i, static_cast<unsigned long long>(n.supply[i]));
            for (const NeckNetwork::Arc& a : n.arcs)
                std::printf("arc %u -> %d: %llu / %llu\n", a.from, a.to == NeckNetwork::kNeckSink ? -1 : static_cast<int>(a.to),
                            static_cast<unsigned long long>(a.cap), static_cast<unsigned long long>(a.capBack));
        }
        EXPECT_EQ(SetValue(n, got), best) << "trial " << trial;
        released += got != 0 ? 1u : 0u;
        ++networks;
    }
    EXPECT_GT(released, networks / 4);          // the networks exercise releases
    EXPECT_LT(released, networks);

    // Load equal to capacity holds; one more falls.
    NeckNetwork exact;
    exact.supply = {10, 5};
    exact.arcs = {{0, 1, 10, 10}, {1, NeckNetwork::kNeckSink, 15, 0}};
    EXPECT_EQ(SolveNeckNetwork(exact, 1000).released, (std::vector<uint8_t>{0, 0}));
    exact.supply[0] = 11;
    EXPECT_EQ(SolveNeckNetwork(exact, 1000).released, (std::vector<uint8_t>{1, 1}));   // 16 > 15 at the bottom
    exact.supply[0] = 8;
    exact.arcs[0].cap = 7;
    exact.arcs[1].cap = 100;
    EXPECT_EQ(SolveNeckNetwork(exact, 1000).released, (std::vector<uint8_t>{1, 0}));   // 8 > 7 at the top: node 0 only
    // A weightless node hanging only from a released one goes with it (the largest set).
    NeckNetwork leaf;
    leaf.supply = {9, 0};
    leaf.arcs = {{0, NeckNetwork::kNeckSink, 4, 0}, {1, 0, 3, 3}};
    EXPECT_EQ(SolveNeckNetwork(leaf, 1000).released, (std::vector<uint8_t>{1, 1}));
}

TEST(Neck, ReleasedSetIndependentOfArcOrder) {
    std::mt19937 rng(99);
    for (int trial = 0; trial < 150; ++trial) {
        NeckNetwork n = RandomNetwork(rng, 3 + static_cast<uint32_t>(trial % 30));
        const NeckFlowResult first = SolveNeckNetwork(n, 1u << 24);
        ASSERT_TRUE(first.solved);
        for (uint32_t seed = 1; seed <= 6; ++seed) {
            n.shuffleSeed = seed;
            const NeckFlowResult r = SolveNeckNetwork(n, 1u << 24);
            ASSERT_TRUE(r.solved);
            EXPECT_EQ(r.released, first.released) << "trial " << trial << " seed " << seed;
        }
    }
    // The same on the tower: the search's network shuffled gives the same pieces.
    NeckWorld w(0.125f, 64, kFocusTower);
    Tower t(w, Floor(0.8, 2, 2, 3));
    t.Build();
    t.Neck(2);
    const GraphSearchResult base = w.Check(t.Hollow());
    ASSERT_EQ(base.neckPieces.size(), 1u);
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        NeckLimits shuffled = w.neck;
        shuffled.shuffleForTesting = seed;
        const GraphSearchResult r = w.Check(t.Hollow(), 1, &shuffled);
        ASSERT_EQ(r.neckPieces.size(), 1u);
        EXPECT_EQ(r.neckPieces[0].origin, base.neckPieces[0].origin);
        EXPECT_EQ(r.neckPieces[0].volume.Data(), base.neckPieces[0].volume.Data());
        EXPECT_EQ(r.neckCuts[0].load, base.neckCuts[0].load);
        EXPECT_EQ(r.neckCuts[0].cut, base.neckCuts[0].cut);
    }
}

TEST(Neck, LimitsFollowTheVoxelSize) {
    GraphMaterials m = MakeGraphMaterials(nullptr, 0);
    EXPECT_EQ(VoxelSizeMicro(0.125f), 125000u);
    EXPECT_EQ(VoxelSizeMicro(0.1f), 100000u);
    EXPECT_EQ(VoxelSizeMicro(0.15f), 150000u);
    EXPECT_EQ(MakeNeckLimits(m, 0.125f).capFace[kCompression][1], 800000u);     // stone's default
    EXPECT_EQ(MakeNeckLimits(m, 0.1f).capFace[kCompression][1], 1000000u);
    EXPECT_EQ(MakeNeckLimits(m, 0.15f).capFace[kCompression][1], 666666u);
    EXPECT_EQ(MakeNeckLimits(m, 0.125f).capFace[kShear][1], 320000u);
    EXPECT_EQ(MakeNeckLimits(m, 0.125f).capFace[kTension][1], 120000u);
    EXPECT_EQ(StrengthClassOf(kFaceNegY), kCompression);
    EXPECT_EQ(StrengthClassOf(kFacePosY), kTension);
    EXPECT_EQ(StrengthClassOf(kFacePosX), kShear);
}

// --- the owner's tower ----------------------------------------------------------------------------

// The watch tower at 0.125 m on a 2 x 2 stone neck (rows 1..12), the floor at each of the 8 heights
// mod 8, the neck across an x cell face and across a z cell face. A dud in the hollow: one piece
// with everything above the neck and at most the 48 neck voxels; ring row 0 and the paving stay.
TEST(Neck, TowerOnATwoByTwoNeckFalls) {
    for (int across = 0; across < 2; ++across) {
        for (int floorLayer = 0; floorLayer < 8; ++floorLayer) {
            NeckWorld w(0.125f, 64, kFocusTower);
            // across 0: the 2-wide neck straddles an x cell face (x layers 7, 0), z inside a cell;
            // across 1: a z cell face.
            Tower t(w, Floor(0.8, across == 0 ? 7 : 2, across == 1 ? 7 : 2, floorLayer));
            t.Build();
            t.Neck(2);
            const int top = t.NeckRows();
            ASSERT_EQ(top, 12);
            const glm::ivec3 lo = t.Min(), hi = t.Max();
            const uint32_t above = CountWorld(*w.world, glm::ivec3(lo.x, t.c.y + top + 1, lo.z), hi);
            ASSERT_EQ(above, 8526u);
            const GraphSearchResult r = w.Check(t.Hollow());
            SCOPED_TRACE(testing::Message() << "across " << across << ", floor layer " << floorLayer);
            EXPECT_TRUE(r.neckRan);
            ASSERT_EQ(r.neckPieces.size(), 1u);
            ASSERT_EQ(r.neckCuts.size(), 1u);
            const uint32_t pieceAbove = CountVoxels(r.neckPieces, glm::ivec3(lo.x, t.c.y + top + 1, lo.z), hi);
            const uint32_t pieceNeck = CountVoxels(r.neckPieces, glm::ivec3(lo.x, t.c.y + 1, lo.z), glm::ivec3(hi.x, t.c.y + top + 1, hi.z));
            EXPECT_EQ(pieceAbove, 8526u);
            EXPECT_LE(pieceNeck, 48u);
            EXPECT_EQ(r.neckPieces[0].voxelCount, pieceAbove + pieceNeck);       // nothing else
            EXPECT_EQ(r.neckCuts[0].voxels, r.neckPieces[0].voxelCount);
            EXPECT_EQ(r.neckCuts[0].faces, 4u);
            EXPECT_EQ(r.neckCuts[0].cut, 3200000u);
            EXPECT_GT(r.neckCuts[0].load, r.neckCuts[0].cut);
            EXPECT_EQ(r.neckOversize, 0u);
            EXPECT_EQ(r.boxIslands.size() + r.faceIslands.size(), 0u);
            if (floorLayer == 7) {
                // Ring row 0 is the top layer of its cell: the cut is the cell face right above it.
                EXPECT_EQ(r.neckPieces[0].voxelCount, 8574u);
                EXPECT_EQ(r.neckCuts[0].load, 17986800u);
                EXPECT_FALSE(r.neckCuts[0].insideCell);
            }
        }
    }
}

// The 2 x 2 neck at x = -1 | 0 and z = -1 | 0: each of its four columns in another chunk (review: the
// tower's necks never crossed a chunk face). The floor row at layer 7, as the aligned case.
TEST(Neck, TowerNeckAcrossTwoChunkFacesFalls) {
    NeckWorld w(0.125f, 64, kFocusTower);
    Tower t(w, {-25, 71, 47});
    ASSERT_EQ(t.c.x + t.S(30), -1);
    ASSERT_EQ(t.c.z + t.S(-60), -1);
    t.Build();
    t.Neck(2);
    const GraphSearchResult r = w.Check(t.Hollow());
    EXPECT_TRUE(r.neckRan);
    ASSERT_EQ(r.neckPieces.size(), 1u);
    EXPECT_EQ(r.neckPieces[0].voxelCount, 8574u);
    EXPECT_EQ(r.neckCuts[0].load, 17986800u);
    EXPECT_EQ(r.neckCuts[0].faces, 4u);
    EXPECT_EQ(r.neckCuts[0].cut, 3200000u);
    EXPECT_EQ(CountVoxels(r.neckPieces, glm::ivec3(t.Min().x, t.c.y + t.NeckRows() + 1, t.Min().z), t.Max()), 8526u);
}

// A neck inside one cell: a block on a 2 x 2 x 2 stone neck between rows of the same cell. The node
// is split at its narrowest plane; the whole neck goes with the block (the lowest plane of equal
// strength).
TEST(Neck, NeckInsideACellIsSplit) {
    NeckWorld w(0.125f, 64, kFocusTower);
    const WorldMaterials& m = w.ids;
    // Ground to row 66 (layers 0-2 of the cell at rows 64..71), the neck rows 67-68, a 16 x 16
    // block from row 69 up to 76.
    w.world->FillBox({-8, 64, -8}, {24, 67, 24}, m.stone);
    w.world->FillBox({2, 67, 2}, {4, 69, 4}, m.stone);
    w.world->FillBox({-4, 69, -4}, {12, 77, 12}, m.stone);
    const GraphSearchResult r = w.Check({3, 70, 3});
    ASSERT_EQ(r.neckPieces.size(), 1u);
    EXPECT_EQ(r.neckPieces[0].voxelCount, 16u * 16u * 8u + 8u);
    EXPECT_EQ(r.neckCuts[0].faces, 4u);
    EXPECT_TRUE(r.neckCuts[0].insideCell);
    EXPECT_GE(r.neckSplit, 1u);
    EXPECT_GE(r.neckSplitRelabels, 1u);
    EXPECT_EQ(CountVoxels(r.neckPieces, {2, 67, 2}, {4, 69, 4}), 8u);
    EXPECT_EQ(CountVoxels(r.neckPieces, {-8, 64, -8}, {24, 67, 24}), 0u);
}

TEST(Neck, ThresholdIsExact) {
    // Stone C at 0.125 m: 800 000 MU per face. 2 x 11 (22 faces) falls (1.05x), 2 x 12 stands (0.97).
    for (const int width : {10, 11, 12}) {
        NeckWorld w(0.125f, 64, kFocusTower);
        Tower t(w, Floor(0.8, 2, 2, 7));
        t.Build();
        t.Neck(width);
        const GraphSearchResult r = w.Check(t.Hollow());
        SCOPED_TRACE(testing::Message() << "width " << width);
        if (width < 12) {
            ASSERT_EQ(r.neckPieces.size(), 1u);
            EXPECT_EQ(r.neckCuts[0].faces, static_cast<uint32_t>(2 * width));
            EXPECT_EQ(r.neckCuts[0].cut, 800000u * static_cast<uint64_t>(2 * width));
            EXPECT_EQ(r.neckCuts[0].load, 17862000u + 12u * 2600u * static_cast<uint64_t>(2 * width));
        } else {
            EXPECT_TRUE(r.neckPieces.empty());
        }
    }
    // A block of exactly n x capFace stands; one MU more falls (capacities set by hand).
    NeckWorld w(0.125f, 64, kFocusTower);
    w.world->FillBox({0, 64, 0}, {1, 66, 1}, w.ids.stone);          // a 1-voxel neck, 2 rows
    w.world->FillBox({-3, 66, -3}, {5, 74, 5}, w.ids.stone);        // an 8 x 8 x 8 block
    // The neck and the block share the cells of rows 64..71 (not split: the neck is the bottom of
    // the node), so the cut is the ground's top face, 1 voxel face, under 514 voxels of stone.
    NeckLimits limits = w.neck;
    const uint64_t load = (512u + 2u) * 2600u;
    limits.capFace[kCompression][w.ids.stone] = load;
    EXPECT_TRUE(w.Check({0, 68, 0}, 1, &limits).neckPieces.empty());
    limits.capFace[kCompression][w.ids.stone] = load - 1;
    const GraphSearchResult r = w.Check({0, 68, 0}, 1, &limits);
    ASSERT_EQ(r.neckPieces.size(), 1u);
    EXPECT_EQ(r.neckCuts[0].load, load);
    EXPECT_EQ(r.neckCuts[0].cut, load - 1);
}

TEST(Neck, FullBaseStands) {
    NeckWorld w(0.125f, 64, kFocusTower);
    Tower t(w, Floor(0.8, 2, 2, 3));
    t.Build();
    const GraphSearchResult r = w.Check(t.Hollow());
    EXPECT_TRUE(r.neckRan);
    EXPECT_TRUE(r.neckPieces.empty());
    EXPECT_GT(r.neckNodes, 100u);
    EXPECT_GE(r.neckLoads, 1u);              // the tower above the island box hangs on the ring
    EXPECT_GE(r.neckAnchors, 1u);            // the ground reaches the flood box face
}

TEST(Neck, ParallelNecksAddUp) {
    // Four 2 x 4 necks (32 faces) hold; one alone (8 faces) does not.
    for (const bool four : {true, false}) {
        NeckWorld w(0.125f, 64, kFocusTower);
        Tower t(w, Floor(0.8, 2, 2, 7));
        t.Build();
        if (four) t.FourNecks(4);
        else t.Neck(4);
        const GraphSearchResult r = w.Check(t.Hollow());
        EXPECT_EQ(r.neckPieces.size(), four ? 0u : 1u);
    }
}

TEST(Neck, BondIsTheWeakerMaterial) {
    // A 2 x 12 stone neck holds the tower on stone (24 x 800 000 > 18.6 M) but not on a planks
    // ring row 0 (24 x 400 000): the bond is as strong as the weaker side.
    for (const bool planks : {false, true}) {
        NeckWorld w(0.125f, 64, kFocusTower);
        Tower t(w, Floor(0.8, 2, 2, 7));
        t.Build(planks ? w.ids.planks : uint8_t{0});
        t.Neck(12);
        const GraphSearchResult r = w.Check(t.Hollow());
        ASSERT_EQ(r.neckPieces.size(), planks ? 1u : 0u);
        if (planks) EXPECT_EQ(r.neckCuts[0].cut, 24u * 400000u);
    }
}

TEST(Neck, VoxelSizeInvariant) {
    // At 0.1 / 0.125 / 0.15 m the 2 x 2 neck falls and the full base stands.
    for (const float vs : {0.1f, 0.125f, 0.15f}) {
        const double scale = 0.1 / static_cast<double>(vs);
        for (const bool neck : {true, false}) {
            NeckWorld w(vs, 64, glm::dvec3(0.0, 68.0 * vs, 0.0));
            Tower t(w, Floor(scale, 2, 2, 7));
            t.Build();
            if (neck) t.Neck(2);
            const GraphSearchResult r = w.Check(t.Hollow());
            SCOPED_TRACE(testing::Message() << vs << " m, " << (neck ? "neck" : "full base"));
            EXPECT_EQ(r.neckPieces.size(), neck ? 1u : 0u);
            if (neck && !r.neckCuts.empty()) {
                EXPECT_EQ(r.neckCuts[0].faces, 4u);
                EXPECT_GT(r.neckCuts[0].load, 4 * r.neckCuts[0].cut);
            }
        }
    }
}

// --- what must stand ------------------------------------------------------------------------------

namespace {
    /// A stone wall (part of the ground, it reaches the flood box face) with a slab sticking out
    /// along +x from x = 8, rows 80..: the root is the cell face x = 7 | 8, `rootFaces` voxel faces
    /// of a connector at x = 7 (in the wall's cell). Returns the dud point.
    /// `root` moves the root face to x = root - 1 | root (8: a cell face; 32: a chunk face).
    glm::ivec3 Cantilever(NeckWorld& w, uint8_t id, int length, int thick, int wide, int rootFaces, int root = 8) {
        const WorldMaterials& m = w.ids;
        const int y0 = 80;
        w.world->FillBox({-40, 64, -20}, {root - 1, 121, 20}, m.stone);
        int left = rootFaces;
        for (int y = y0; y < y0 + thick && left > 0; ++y)
            for (int z = 0; z < wide && left > 0; ++z, --left) w.world->FillBox({root - 1, y, z}, {root, y + 1, z + 1}, m.stone);
        w.world->FillBox({root, y0, 0}, {root + length, y0 + thick, wide}, id);
        return {root + length / 2, y0 + thick / 2, wide / 2};
    }
}

TEST(Neck, StoneOverhang3mStands) {
    NeckWorld w(0.125f, 64, kFocusTower);
    // 3 m out, 1 m thick, 2 m wide, its whole root attached (128 faces): 0.20 of its shear capacity.
    const glm::ivec3 at = Cantilever(w, w.ids.stone, 24, 8, 16, 128);
    const GraphSearchResult r = w.Check(at);
    EXPECT_TRUE(r.neckRan);
    EXPECT_TRUE(r.neckPieces.empty());
    // A natural mushroom: a 3 x 3 x 1 m cap on a 1 x 1 m stem 2 m tall.
    NeckWorld m(0.125f, 64, kFocusTower);
    m.world->FillBox({0, 64, 0}, {8, 80, 8}, m.ids.stone);
    m.world->FillBox({-8, 80, -8}, {16, 88, 16}, m.ids.stone);
    EXPECT_TRUE(m.Check({4, 82, 4}).neckPieces.empty());
}

TEST(Neck, OverhangRootThresholdIsExact) {
    // The 3 m slab (3072 voxels, 7 987 200 MU): 24 root faces x 320 000 fall, 25 stand.
    for (const int faces : {24, 25}) {
        NeckWorld w(0.125f, 64, kFocusTower);
        const glm::ivec3 at = Cantilever(w, w.ids.stone, 24, 8, 16, faces);
        const GraphSearchResult r = w.Check(at);
        SCOPED_TRACE(faces);
        if (faces == 24) {
            ASSERT_EQ(r.neckPieces.size(), 1u);
            EXPECT_EQ(r.neckPieces[0].voxelCount, 3072u);
            EXPECT_EQ(r.neckCuts[0].load, 7987200u);
            EXPECT_EQ(r.neckCuts[0].cut, 24u * 320000u);
        } else {
            EXPECT_TRUE(r.neckPieces.empty());
        }
    }
}

// The same slab with its root on the chunk face x = 31 | 32 (review: no exact shear cut across a
// chunk face was tested; BondVoxels across chunks there).
TEST(Neck, OverhangRootThresholdIsExactOnAChunkFace) {
    for (const int faces : {24, 25}) {
        NeckWorld w(0.125f, 64, glm::dvec3(5.5, 8.5, 0.0));
        const glm::ivec3 at = Cantilever(w, w.ids.stone, 24, 8, 16, faces, 32);
        const GraphSearchResult r = w.Check(at);
        SCOPED_TRACE(faces);
        EXPECT_TRUE(r.neckRan);
        EXPECT_FALSE(r.neckWaits);
        if (faces == 24) {
            ASSERT_EQ(r.neckPieces.size(), 1u);
            EXPECT_EQ(r.neckPieces[0].voxelCount, 3072u);
            EXPECT_EQ(r.neckCuts[0].load, 7987200u);
            EXPECT_EQ(r.neckCuts[0].cut, 24u * 320000u);
            EXPECT_EQ(r.neckCuts[0].faces, 24u);
            EXPECT_EQ(CountVoxels(r.neckPieces, {31, 0, -20}, {32, 128, 20}), 0u);     // the connector stays
        } else {
            EXPECT_TRUE(r.neckPieces.empty());
        }
    }
}

// Tension across the chunk face y = 95 | 96: an 8 x 15 x 8 stone block hangs from a ceiling by a
// 2 x 2 neck in its top row (row 95, in chunk row 2; the ceiling starts at row 96, chunk row 3).
// With the tension capacity set by hand to load / 4 per face it holds; one MU less, it falls.
TEST(Neck, HangingBlockTensionThresholdOnAChunkFace) {
    NeckWorld w(0.125f, 64, kFocusTower);
    const WorldMaterials& m = w.ids;
    w.world->FillBox({-20, 64, -8}, {-12, 104, 16}, m.stone);       // a wall on the ground
    w.world->FillBox({-12, 96, -8}, {16, 104, 16}, m.stone);        // the ceiling, held by the wall in shear
    w.world->FillBox({0, 80, 0}, {8, 95, 8}, m.stone);              // the block, rows 80..94
    w.world->FillBox({3, 95, 3}, {5, 96, 5}, m.stone);              // the neck, row 95
    const uint64_t load = (8u * 15u * 8u + 4u) * 2600u;             // 2 506 400 MU
    ASSERT_EQ(load % 4u, 0u);
    NeckLimits limits = w.neck;
    limits.capFace[kTension][m.stone] = load / 4u;
    const GraphSearchResult holds = w.Check({4, 92, 4}, 1, &limits);
    EXPECT_TRUE(holds.neckRan);
    EXPECT_TRUE(holds.neckPieces.empty());
    limits.capFace[kTension][m.stone] = load / 4u - 1u;
    const GraphSearchResult r = w.Check({4, 92, 4}, 1, &limits);
    ASSERT_EQ(r.neckPieces.size(), 1u);
    EXPECT_EQ(r.neckPieces[0].voxelCount, 964u);
    EXPECT_EQ(r.neckCuts[0].load, load);
    EXPECT_EQ(r.neckCuts[0].faces, 4u);
    EXPECT_EQ(r.neckCuts[0].cut, load - 4u);
    EXPECT_EQ(CountVoxels(r.neckPieces, {-12, 96, -8}, {16, 104, 16}), 0u);         // the ceiling stays
}

TEST(Neck, SandOverhangSlumps) {
    // Sand on stone, root fully attached: 2 m (16 voxels x 1600 = 25 600 > 24 000 per face) falls,
    // 1.5 m stands.
    for (const int length : {16, 12}) {
        NeckWorld w(0.125f, 64, kFocusTower);
        const glm::ivec3 at = Cantilever(w, w.ids.sand, length, 4, 8, 32);
        const GraphSearchResult r = w.Check(at);
        SCOPED_TRACE(length);
        EXPECT_EQ(r.neckPieces.size(), length == 16 ? 1u : 0u);
    }
}

TEST(Neck, WoodBeamBridgeStands) {
    // A 3 x 3 x 48 wood beam (6 m) butted into stone at both ends; then with one end cut free:
    // 9 shear faces still carry it (no bending: shred.md Part 4b failure mode 3).
    for (const bool oneEnd : {false, true}) {
        NeckWorld w(0.125f, 64, kFocusTower);
        w.world->FillBox({-30, 64, -8}, {-24, 90, 8}, w.ids.stone);
        if (!oneEnd) w.world->FillBox({24, 64, -8}, {30, 90, 8}, w.ids.stone);
        w.world->FillBox({-24, 84, 0}, {24, 87, 3}, w.ids.wood);
        const GraphSearchResult r = w.Check({-20, 85, 1});
        EXPECT_TRUE(r.neckPieces.empty()) << (oneEnd ? "one end" : "both ends");
    }
}

TEST(Neck, TreeWithOneVoxelTrunkStands) {
    // A 1-voxel wood trunk 4 m tall rooted in dirt under a 20 x 12 x 20 canopy of leaves.
    NeckWorld w(0.125f, 64, kFocusTower);
    w.world->FillBox({-20, 60, -20}, {20, 64, 20}, w.ids.dirt);
    w.world->FillBox({0, 64, 0}, {1, 96, 1}, w.ids.wood);
    w.world->FillBox({-10, 96, -10}, {10, 108, 10}, w.ids.leaves);
    EXPECT_TRUE(w.Check({0, 70, 0}).neckPieces.empty());
    // If leaves weighed 200 kg/m^3 the canopy alone (4800 x 200 MU on one face) would break it.
    GraphMaterials heavy = w.graphMaterials;
    heavy.densityQ[w.ids.leaves] = 200;
    EXPECT_FALSE(w.Check({0, 70, 0}, 1, nullptr, &heavy).neckPieces.empty());
}

// Review: the weakest material of a node rated a whole wall by an incidental voxel. A 6 m stone
// wall 2 voxels thick with a band of leaves against it (rows 72..79): leaves (80 000 MU per face)
// would rate every wall node of that band, 16 faces per cell under 1.66 M MU. Foliage is left out
// of a node's weakest material when it holds anything else: the wall stands.
TEST(Neck, StoneWallWithLeavesStands) {
    NeckWorld w(0.125f, 64, kFocusTower);
    w.world->FillBox({-16, 64, 0}, {16, 112, 2}, w.ids.stone);
    w.world->FillBox({-16, 72, 2}, {16, 80, 3}, w.ids.leaves);
    const GraphSearchResult r = w.Check({0, 74, 1});
    EXPECT_TRUE(r.neckRan);
    EXPECT_TRUE(r.neckPieces.empty()) << (r.neckCuts.empty() ? 0u : r.neckCuts[0].voxels) << " voxels released";
    // A node of leaves alone keeps their strength: stone resting on a canopy is rated by it.
    GraphMaterials m = w.graphMaterials;
    EXPECT_TRUE(m.foliage[w.ids.leaves]);
    EXPECT_FALSE(m.foliage[w.ids.stone]);
}

// A 6 m stone wall on a 3-voxel snow cover (snow's compression was 10 000 kg/m^2: 80 000 MU per face
// against 124 800 per face of the wall). Snow is now packed snow, compression 50 000 like dirt:
// the wall stands; snow still slumps in shear and tension.
TEST(Neck, StoneWallOnSnowStands) {
    NeckWorld w(0.125f, 64, kFocusTower);
    w.world->FillBox({-40, 61, -40}, {40, 64, 40}, w.ids.snow);
    w.world->FillBox({-16, 64, 0}, {16, 112, 2}, w.ids.stone);
    const GraphSearchResult r = w.Check({0, 66, 1});
    EXPECT_TRUE(r.neckRan);
    EXPECT_TRUE(r.neckPieces.empty()) << (r.neckCuts.empty() ? 0u : r.neckCuts[0].voxels) << " voxels released";
    EXPECT_EQ(w.graphMaterials.strength[kCompression][w.ids.snow], 50000u);
}

TEST(Structure, FoliageCarriesNoLoad) {
    MaterialLibrary lib;
    const WorldMaterials ids = RegisterDefaultWorldMaterials(lib);
    const GraphMaterials m = MakeGraphMaterials(&lib, MaterialFlag_Liquid);
    EXPECT_EQ(m.densityQ[ids.leaves], 0u);
    EXPECT_EQ(m.densityQ[ids.leavesAutumn], 0u);
    EXPECT_EQ(m.densityQ[ids.wood], 700u);
    EXPECT_TRUE(m.anchor[ids.bedrock]);
    EXPECT_FALSE(m.anchor[ids.water]);       // ignored, not an anchor
    EXPECT_FALSE(m.anchor[ids.stone]);
    EXPECT_EQ(m.strength[kCompression][ids.stone], 100000u);
    EXPECT_EQ(m.strength[kShear][ids.sand], 3000u);
    EXPECT_EQ(m.strength[kTension][ids.dirt], 2000u);
    // A chunk of leaves has nodes of mass 0.
    Chunk chunk;
    chunk.Clear();
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) chunk.ids[Chunk::VoxelIndex(x, y, z)] = (x + y) % 3 == 0 ? ids.leaves : ids.leavesAutumn;
    chunk.RebuildOccupancy();
    ChunkGraph g;
    BuildChunkGraph(g, {0, 0, 0}, &chunk, 0, m);
    ASSERT_EQ(g.nodes.size(), 1u);
    EXPECT_EQ(g.nodes[0].mass, 0u);
    EXPECT_EQ(g.nodes[0].weak[kCompression], std::min(ids.leaves, ids.leavesAutumn));
}

// --- locality, exterior, limits -------------------------------------------------------------------

TEST(Neck, ChecksOnlyInsideTheIslandBox) {
    // A thin neck high on a pillar (rows 110-111), a dud at the base: the part above the island box
    // (the neck included) is one exterior load on the pillar, which holds it.
    NeckWorld w(0.125f, 64, kFocusTower);
    w.world->FillBox({0, 64, 0}, {4, 110, 4}, w.ids.stone);
    w.world->FillBox({1, 110, 1}, {2, 112, 2}, w.ids.stone);
    w.world->FillBox({-6, 112, -6}, {10, 128, 10}, w.ids.stone);
    const GraphSearchResult low = w.Check({2, 66, 2});
    EXPECT_TRUE(low.neckPieces.empty());
    EXPECT_GE(low.neckLoads, 1u);
    // A dud next to the neck sees it.
    const GraphSearchResult high = w.Check({2, 111, 2});
    ASSERT_EQ(high.neckPieces.size(), 1u);
    EXPECT_EQ(high.neckCuts[0].faces, 1u);
}

TEST(Neck, ExteriorLoadIsPartOfThePiece) {
    NeckWorld w(0.125f, 64, kFocusTower);
    Tower t(w, Floor(0.8, 2, 2, 7));
    t.Build();
    t.Neck(2);
    const GraphSearchResult r = w.Check(t.Hollow());
    ASSERT_EQ(r.neckPieces.size(), 1u);
    // The roof and the torches are far above the island box (rows up to the hollow + 25).
    EXPECT_GT(t.Roof().y, t.Hollow().y + 25);
    EXPECT_EQ(CountVoxels(r.neckPieces, t.Roof(), t.Roof() + glm::ivec3(1)), 1u);
    EXPECT_EQ(CountVoxels(r.neckPieces, glm::ivec3(t.Min().x, t.Hollow().y + 26, t.Min().z), t.Max()),
              CountWorld(*w.world, glm::ivec3(t.Min().x, t.Hollow().y + 26, t.Min().z), t.Max()));
    EXPECT_GE(r.neckLoads, 1u);
}

TEST(Neck, OversizePieceStaysCounted) {
    // A 40 x 40 x 40 stone block (64 000 voxels) on a 2 x 2 neck: overloaded, but over the piece
    // limit, so it stays (counted); a 30^3 block on the same neck falls.
    for (const int size : {40, 30}) {
        NeckWorld w(0.125f, 64, kFocusTower);
        w.world->FillBox({0, 64, 0}, {2, 66, 2}, w.ids.stone);
        w.world->FillBox({-size / 2, 66, -size / 2}, {size / 2, 66 + size, size / 2}, w.ids.stone);
        const GraphSearchResult r = w.Check({1, 66 + size / 2, 1}, size / 2);
        SCOPED_TRACE(size);
        if (size == 40) {
            EXPECT_TRUE(r.neckPieces.empty());
            EXPECT_EQ(r.neckOversize, 1u);
        } else {
            EXPECT_EQ(r.neckPieces.size(), 1u);
            EXPECT_EQ(r.neckOversize, 0u);
        }
    }
}

TEST(Neck, ExactAtLargeLoads) {
    // A 38^3 gold block (54 872 voxels, 1 059 029 600 MU) on a layer of n iron voxels on stone at
    // 0.1 m (iron on stone: 1 000 000 MU per face; gold on iron 1 500 000): it falls while
    // 1 059 029 600 + 7800 n > 1 000 000 n, i.e. n <= 1067. The exact integers survive.
    for (const int faces : {1067, 1068}) {
        NeckWorld w(0.1f, 64, glm::dvec3(0.0, 8.5, 0.0));
        const int rows = faces / 38, rest = faces % 38;
        w.world->FillBox({-19, 64, -19}, {19, 65, -19 + rows}, w.ids.iron);
        w.world->FillBox({-19, 64, -19 + rows}, {-19 + rest, 65, -18 + rows}, w.ids.iron);
        w.world->FillBox({-19, 65, -19}, {19, 103, 19}, w.ids.gold);
        const GraphSearchResult r = w.Check({0, 84, 0}, 20);
        SCOPED_TRACE(faces);
        EXPECT_EQ(r.neckOversize, 0u);
        if (faces == 1067) {
            ASSERT_EQ(r.neckPieces.size(), 1u);
            EXPECT_EQ(r.neckPieces[0].voxelCount, 54872u + 1067u);
            EXPECT_EQ(r.neckCuts[0].load, 1059029600u + 1067u * 7800u);
            EXPECT_EQ(r.neckCuts[0].cut, 1067000000u);
        } else {
            EXPECT_TRUE(r.neckPieces.empty());
        }
    }
}

// The network at its node limit with the largest supplies a node can have (a 512-voxel node of
// density 65535) and arcs just under the "infinite" threshold (2^45 MU): every value of the rule is
// exact (checked in 128-bit integers), a load equal to the capacity holds, one MU more falls.
TEST(Neck, NetworkAtTheLimits) {
    constexpr uint32_t kNodes = 8192;
    constexpr uint64_t kSupply = 512ull * 65535ull;
    constexpr uint64_t kBig = (uint64_t{1} << 45) - 1;
    const unsigned __int128 total = static_cast<unsigned __int128>(kSupply) * kNodes;
    ASSERT_LT(total, static_cast<unsigned __int128>(kBig));
    for (const uint64_t sinkCap : {static_cast<uint64_t>(total), static_cast<uint64_t>(total) - 1}) {
        NeckNetwork n;
        n.supply.assign(kNodes, kSupply);
        for (uint32_t i = 0; i + 1 < kNodes; ++i) n.arcs.push_back({i, i + 1, kBig, kBig});
        n.arcs.push_back({kNodes - 1, NeckNetwork::kNeckSink, sinkCap, 0});
        // The perturbed values stay far below 2^63: supply x 2^17 and every arc x 2^17 + 1.
        const unsigned __int128 supply = total * kM, arc = static_cast<unsigned __int128>(kBig) * kM + 1;
        ASSERT_LT(supply + arc, static_cast<unsigned __int128>(1) << 63);
        const NeckFlowResult r = SolveNeckNetwork(n, 1u << 24);
        ASSERT_TRUE(r.solved);
        const uint32_t released = static_cast<uint32_t>(std::count(r.released.begin(), r.released.end(), uint8_t{1}));
        SCOPED_TRACE(sinkCap == total ? "equal" : "one MU less");
        // The whole chain against the sink: M x (supply - cap) - 1 arc: > 0 exactly when supply > cap.
        const __int128 value = static_cast<__int128>(kM) * (static_cast<__int128>(total) - static_cast<__int128>(sinkCap)) - 1;
        EXPECT_EQ(released, value > 0 ? kNodes : 0u);
        EXPECT_LT(r.work, 1u << 20);
    }
}

// A released part may be a load alone, with no judged node: 224 sand voxels hanging under a stone
// ceiling (outside the island box) in tension (16 faces x 4000 MU against 358 400 MU). The ceiling
// stays (review: the crumb bound and the design said a released part holds a judged node).
TEST(Neck, HangingLoadIsReleasedAloneItsNodeStays) {
    NeckWorld w(0.125f, 64, kFocusTower);
    const WorldMaterials& m = w.ids;
    w.world->FillBox({-40, 64, -20}, {-24, 121, 20}, m.stone);      // a wall on the ground
    w.world->FillBox({-24, 80, -8}, {16, 88, 8}, m.stone);          // a ceiling from it, rows 80..87
    w.world->FillBox({0, 66, 0}, {4, 80, 4}, m.sand);               // sand hanging under it, rows 66..79
    const GraphSearchResult r = w.Check({0, 105, 0});               // the island box: rows 80..129
    EXPECT_TRUE(r.neckRan);
    ASSERT_EQ(r.neckPieces.size(), 1u);
    EXPECT_EQ(r.neckPieces[0].voxelCount, 224u);
    EXPECT_EQ(r.neckCuts[0].load, 224u * 1600u);
    EXPECT_EQ(r.neckCuts[0].faces, 16u);
    EXPECT_EQ(r.neckCuts[0].cut, 16u * 4000u);
    EXPECT_EQ(CountVoxels(r.neckPieces, {-24, 80, -8}, {16, 88, 8}), 0u);          // the ceiling stays
    EXPECT_EQ(CountVoxels(r.neckPieces, {0, 66, 0}, {4, 80, 4}), 224u);
}

// What one check may release (review: there was no cap on the sum): three blocks of 1730 voxels on
// 1 x 1 necks. Parts are taken in piece order while they fit; the rest stays, counted, and a
// later check releases it.
TEST(Neck, ReleaseIsCappedPerCheck) {
    const auto Build = [](NeckWorld& w) {
        for (int i = 0; i < 3; ++i) {
            const int x0 = -20 + 16 * i;
            w.world->FillBox({x0 + 5, 64, 0}, {x0 + 6, 66, 1}, w.ids.stone);
            w.world->FillBox({x0, 66, -6}, {x0 + 12, 78, 6}, w.ids.stone);
        }
    };
    NeckWorld w(0.125f, 64, kFocusTower);
    Build(w);
    const GraphSearchResult all = w.Check({0, 70, 0});
    ASSERT_EQ(all.neckPieces.size(), 3u);
    EXPECT_EQ(all.neckDeferredParts, 0u);
    for (const NeckCut& c : all.neckCuts) EXPECT_EQ(c.voxels, 1730u);
    // Voxels: two fit exactly, the third waits.
    NeckLimits limits = w.neck;
    limits.maxReleasedVoxels = 2 * 1730;
    const GraphSearchResult two = w.Check({0, 70, 0}, 1, &limits);
    ASSERT_EQ(two.neckPieces.size(), 2u);
    EXPECT_EQ(two.neckDeferredParts, 1u);
    EXPECT_EQ(two.neckVoxels, 2u * 1730u);
    EXPECT_EQ(two.neckPieces[0].origin, all.neckPieces[0].origin);
    EXPECT_EQ(two.neckPieces[1].origin, all.neckPieces[1].origin);
    limits.maxReleasedVoxels = 2 * 1730 - 1;
    EXPECT_EQ(w.Check({0, 70, 0}, 1, &limits).neckPieces.size(), 1u);
    // Parts: one.
    limits = w.neck;
    limits.maxReleasedParts = 1;
    const GraphSearchResult one = w.Check({0, 70, 0}, 1, &limits);
    ASSERT_EQ(one.neckPieces.size(), 1u);
    EXPECT_EQ(one.neckDeferredParts, 2u);
    // What stayed is still overloaded: once the released ones are gone, the next check takes it.
    for (const FloatingIsland& p : two.neckPieces) {
        const glm::ivec3 sz = p.volume.Size();
        for (int z = 0; z < sz.z; ++z)
            for (int y = 0; y < sz.y; ++y)
                for (int x = 0; x < sz.x; ++x)
                    if (p.volume.Get(x, y, z) != 0) w.world->FillBox(p.origin + glm::ivec3(x, y, z), p.origin + glm::ivec3(x + 1, y + 1, z + 1), 0);
    }
    const GraphSearchResult rest = w.Check({0, 70, 0}, 1, &limits);
    ASSERT_EQ(rest.neckPieces.size(), 1u);
    EXPECT_EQ(rest.neckPieces[0].origin, all.neckPieces[2].origin);
    EXPECT_EQ(rest.neckDeferredParts, 0u);
}

// An exterior flood that enters a chunk solid through (32^3 solid voxels) ends there as ground
// (review: the cold cost of floods through buried chunks). A chunk-aligned 4 m stone cube on a
// 1 x 1 neck is therefore held; the same cube one voxel higher (no chunk solid through) falls.
TEST(Neck, SolidChunkIsGround) {
    for (const int lift : {0, 1}) {
        NeckWorld w(0.125f, 64, kFocusTower);
        w.world->FillBox({15, 64, 15}, {16, 96 + lift, 16}, w.ids.stone);
        w.world->FillBox({0, 96 + lift, 0}, {32, 128 + lift, 32}, w.ids.stone);
        const GraphSearchResult r = w.Check({15, 66, 15});          // the island box: rows 41..91
        SCOPED_TRACE(lift);
        EXPECT_TRUE(r.neckRan);
        if (lift == 0) {
            EXPECT_TRUE(r.neckPieces.empty());
            EXPECT_GE(r.neckSolidChunks, 1u);
        } else {
            ASSERT_EQ(r.neckPieces.size(), 1u);
            EXPECT_EQ(r.neckPieces[0].voxelCount, 32768u + 33u);      // (the ground under it is solid chunks too)
        }
    }
}

// Not a gate: what the check costs near its per-cast budgets (review: the design's "about 3-4 ms at
// the budgets" was never measured). Run with
//   VortexCoreTests --gtest_also_run_disabled_tests --gtest_filter='*CostAtTheBudgets*'
// (a) the preflow alone on random networks of 8192 nodes and 65 535 arc pairs, overloaded, at the
// flow budget; (b) a forest of 1-voxel stone pillars every 3 voxels through the whole island box of a
// 1.2 m blast and on up to the flood box (thousands of judged nodes, thousands of exterior visits),
// with a capacity so low that every pillar is overloaded (the release caps take 64 parts); (c) the
// 38^3 gold block (55 000 voxels re-labelled and released).
TEST(Neck, DISABLED_CostAtTheBudgets) {
    using Clock = std::chrono::steady_clock;
    {
        std::mt19937 rng(5);
        for (int run = 0; run < 3; ++run) {
            NeckNetwork n;
            std::uniform_int_distribution<uint32_t> pick(0, 8191);
            std::uniform_int_distribution<uint64_t> mass(0, 512u * 2600u), cap(0, 64u * 800000u);
            for (uint32_t i = 0; i < 8192; ++i) n.supply.push_back(mass(rng));
            while (n.arcs.size() < 65535 - 600) {
                const uint32_t a = pick(rng), b = pick(rng);
                if (a != b) n.arcs.push_back({a, b, cap(rng) / 8, cap(rng) / 8});
            }
            for (uint32_t i = 0; i < 600; ++i) n.arcs.push_back({pick(rng), NeckNetwork::kNeckSink, cap(rng), 0});
            const Clock::time_point t0 = Clock::now();
            const NeckFlowResult r = SolveNeckNetwork(n, 262144);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            const uint32_t released = static_cast<uint32_t>(std::count(r.released.begin(), r.released.end(), uint8_t{1}));
            std::printf("[ NeckCost ] (a) preflow, 8192 nodes, %zu arc pairs: %.2f ms, %llu arc scans, %s, %u released\n", n.arcs.size(), ms,
                        static_cast<unsigned long long>(r.work), r.solved ? "solved" : "BUDGET SPENT", released);
        }
    }
    for (int run = 0; run < 3; ++run) {
        NeckWorld w(0.125f, 64, glm::dvec3(0.0, 8.5, 0.0));
        const glm::ivec3 at(0, 68, 0);
        const int radius = 10;                                        // 1.2 m at 0.125 m
        for (int z = -33; z <= 33; z += 3)
            for (int x = -33; x <= 33; x += 3) w.world->FillBox({x, 64, z}, {x + 1, 68 + 90, z + 1}, w.ids.planks);
        NeckLimits limits = w.neck;
        limits.maxReleasedVoxels = 60000;
        limits.maxReleasedParts = 64;
        for (int k = 0; k < 3; ++k) limits.capFace[k][w.ids.planks] = 1000;    // every pillar overloaded on its one face
        w.cache.Clear();
        const GraphSearchResult cold = w.Check(at, radius, &limits);
        const GraphSearchResult warm = w.Check(at, radius, &limits);
        for (const GraphSearchResult* r : {&cold, &warm})
            std::printf("[ NeckCost ] (b) pillar forest %s: check %.2f ms + %.2f ms of graph builds; %u nodes (%u split), %u loads, %u anchors, "
                        "%u arcs, %u exterior visits, %u flow work, %zu released (%u voxels), %u left (cap), %u oversize; skipped %u, budgets %u / %u\n",
                        r == &cold ? "cold" : "warm", r->neckMs, r->neckBuildMs, r->neckNodes, r->neckSplit, r->neckLoads, r->neckAnchors, r->neckArcs,
                        r->neckExteriorVisits, r->neckFlowWork, r->neckPieces.size(), r->neckVoxels, r->neckDeferredParts, r->neckOversize,
                        r->neckSkipped, r->neckExteriorBudget, r->neckFlowBudget);
    }
    for (int run = 0; run < 3; ++run) {
        NeckWorld w(0.1f, 64, glm::dvec3(0.0, 8.5, 0.0));
        w.world->FillBox({-19, 64, -19}, {19, 65, 9}, w.ids.iron);
        w.world->FillBox({-19, 65, -19}, {19, 103, 19}, w.ids.gold);
        const GraphSearchResult r = w.Check({0, 84, 0}, 20);
        std::printf("[ NeckCost ] (c) gold block: check %.2f ms + %.2f ms of graph builds; %u nodes, %u arcs, %u flow work, %zu released (%u voxels)\n",
                    r.neckMs, r.neckBuildMs, r.neckNodes, r.neckArcs, r.neckFlowWork, r.neckPieces.size(), r.neckVoxels);
    }
}

TEST(Neck, StaleGraphNeverReleasesAttachedVoxels) {
    // The cache keeps the neck chunk's graph from when the tower stood on a 2 x 2 neck; the world
    // has its full base back. The released nodes are re-checked: the search runs again on fresh
    // graphs and releases nothing.
    NeckWorld w(0.125f, 64, kFocusTower);
    Tower t(w, Floor(0.8, 2, 2, 7));
    t.Build();
    t.Neck(2);
    ASSERT_EQ(w.Check(t.Hollow()).neckPieces.size(), 1u);
    // Keep every graph of the ring's chunks as they are now.
    std::vector<std::pair<glm::ivec3, ChunkGraph>> stale;
    const glm::ivec3 lo = VoxelToChunk(t.Min()), hi = VoxelToChunk(t.c + glm::ivec3(t.S(58), 20, t.S(-32)));
    for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x) {
                const ChunkGraph* g = w.cache.Get(*w.world, {x, y, z});
                if (g != nullptr) stale.emplace_back(glm::ivec3(x, y, z), *g);
            }
    t.Build();                                        // the full base again
    w.cache.BeginPass(*w.world, w.graphMaterials);
    for (auto& [chunk, graph] : stale) w.cache.InjectForTesting(*w.world, chunk, graph);
    GraphSearchLimits limits;
    const glm::ivec3 c = t.Hollow();
    limits.boxMin = c - glm::ivec3(25);
    limits.boxMax = c + glm::ivec3(25);
    limits.floodMin = c - glm::ivec3(kFloodHalfExtent);
    limits.floodMax = c + glm::ivec3(kFloodHalfExtent);
    limits.maxIslandVoxels = 60000;
    limits.maxNodeVisits = 32768;
    limits.neck = &w.neck;
    const GraphSearchResult r = SearchIslandsOnGraph(*w.world, w.cache, w.graphMaterials, limits);
    EXPECT_GT(r.relabelMismatch, 0u);
    EXPECT_EQ(r.staleGraphRetries, 1u);
    EXPECT_TRUE(r.neckPieces.empty());

    // Review: the guard compared voxels, face masks and bounds only, not what the check reads. The
    // tower on a 2 x 12 neck (24 faces) stands; graphs with the same voxels but sand as the weakest
    // material, or 10x the mass, would drop it. The re-checked nodes catch both.
    for (const int variant : {0, 1}) {
        NeckWorld v(0.125f, 64, kFocusTower);
        Tower u(v, Floor(0.8, 2, 2, 7));
        u.Build();
        u.Neck(12);
        ASSERT_TRUE(v.Check(u.Hollow()).neckPieces.empty());
        v.cache.BeginPass(*v.world, v.graphMaterials);
        const glm::ivec3 a = VoxelToChunk(u.Min()), b = VoxelToChunk(u.Max());
        for (int z = a.z; z <= b.z; ++z)
            for (int y = a.y; y <= b.y; ++y)
                for (int x = a.x; x <= b.x; ++x) {
                    const ChunkGraph* g = v.cache.Get(*v.world, {x, y, z});
                    if (g == nullptr || g->nodes.empty()) continue;
                    ChunkGraph wrong = *g;
                    wrong.chunk = glm::ivec3(x, y, z);
                    for (GraphNode& node : wrong.nodes) {
                        if (variant == 0) node.weak[kCompression] = node.weak[kShear] = node.weak[kTension] = v.ids.sand;
                        else node.mass *= 10u;
                    }
                    v.cache.InjectForTesting(*v.world, {x, y, z}, std::move(wrong));
                }
        GraphSearchLimits l = limits;
        const glm::ivec3 h = u.Hollow();
        l.boxMin = h - glm::ivec3(25);
        l.boxMax = h + glm::ivec3(25);
        l.floodMin = h - glm::ivec3(kFloodHalfExtent);
        l.floodMax = h + glm::ivec3(kFloodHalfExtent);
        l.neck = &v.neck;
        const GraphSearchResult q = SearchIslandsOnGraph(*v.world, v.cache, v.graphMaterials, l);
        SCOPED_TRACE(variant == 0 ? "weakest material" : "mass");
        EXPECT_GT(q.relabelMismatch, 0u);
        EXPECT_EQ(q.staleGraphRetries, 1u);
        EXPECT_TRUE(q.neckPieces.empty());
    }
}

TEST(Neck, BudgetsCountWorkNotTime) {
    const auto Counters = [](const GraphSearchResult& r) {
        return std::vector<uint64_t>{r.neckNodes,          r.neckSplit,          r.neckLoads,         r.neckAnchors, r.neckArcs,
                                     r.neckExteriorVisits, r.neckFlowWork,       r.neckSplitRelabels, r.neckVoxels,  r.neckOversize,
                                     r.neckSkipped,        r.neckExteriorBudget, r.neckFlowBudget,    r.chunkGraphs, r.neckPieces.size()};
    };
    // The full budgets, then each budget reached in turn: one worker cold, the same world warm,
    // three workers cold. Every counter is equal; only graphsBuilt differs.
    std::vector<std::vector<uint64_t>> first;
    for (const unsigned threads : {1u, 3u}) {
        NeckWorld w(0.125f, 64, kFocusTower, threads);
        Tower t(w, Floor(0.8, 2, 2, 3));
        t.Build();
        t.Neck(2);
        std::vector<NeckLimits> variants(4, w.neck);
        variants[1].maxExteriorVisits = 20;
        variants[2].maxFlowWork = 500;
        variants[3].maxNetworkNodes = 50;
        for (int pass = 0; pass < 2; ++pass) {
            if (threads == 3 && pass == 1) break;
            if (pass == 1) w.Check(t.Hollow());          // warm: every graph the full check reads is cached
            for (size_t v = 0; v < variants.size(); ++v) {
                if (pass == 0) w.cache.Clear();
                const GraphSearchResult r = w.Check(t.Hollow(), 1, &variants[v]);
                SCOPED_TRACE(testing::Message() << threads << " worker(s), pass " << pass << ", variant " << v);
                const std::vector<uint64_t> c = Counters(r);
                if (first.size() < variants.size()) {
                    first.push_back(c);
                    if (v == 0) EXPECT_EQ(r.neckPieces.size(), 1u);
                    if (v == 1) EXPECT_GT(r.neckExteriorBudget, 0u);
                    if (v == 2) {
                        EXPECT_EQ(r.neckFlowBudget, 1u);
                        EXPECT_TRUE(r.neckPieces.empty());
                    }
                    if (v == 3) {
                        EXPECT_EQ(r.neckSkipped, 1u);
                        EXPECT_TRUE(r.neckPieces.empty());
                    }
                } else {
                    EXPECT_EQ(c, first[v]);
                }
                if (pass == 0) EXPECT_GT(r.graphsBuilt, 0u);
                else EXPECT_EQ(r.graphsBuilt, 0u);
            }
        }
    }
}

// --- the node fields ------------------------------------------------------------------------------

namespace {
    /// Brute-force expectations for one node (z slices, bit x + 8y) and its voxel ids.
    struct NodeExpect {
        uint8_t cutFaces = kNoCut, plane = 0;
    };
    NodeExpect ExpectedCut(const std::array<uint64_t, 8>& n) {
        int c[7] = {}, a[8] = {};
        int ymin = 8, ymax = -1;
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    if (!((n[z] >> (x + 8 * y)) & 1u)) continue;
                    ++a[y];
                    ymin = std::min(ymin, y);
                    ymax = std::max(ymax, y);
                    if (y < 7 && ((n[z] >> (x + 8 * (y + 1))) & 1u)) ++c[y];
                }
        NodeExpect e;
        if (ymin >= ymax) return e;
        int minC = 1000;
        for (int k = ymin; k < ymax; ++k) minC = std::min(minC, c[k]);
        for (int k = ymin; k < ymax; ++k) {
            if (c[k] != minC) continue;
            int lower = 0, upper = 0;
            for (int y = ymin; y <= k; ++y) lower = std::max(lower, a[y]);
            for (int y = k + 1; y <= ymax; ++y) upper = std::max(upper, a[y]);
            if (!(c[k] < std::min(lower, upper))) continue;
            // Both halves 6-connected (a plain flood per half).
            for (int half = 0; half < 2; ++half) {
                std::vector<int> voxels;
                for (int z = 0; z < 8; ++z)
                    for (int y = half == 0 ? 0 : k + 1; y <= (half == 0 ? k : 7); ++y)
                        for (int x = 0; x < 8; ++x)
                            if ((n[z] >> (x + 8 * y)) & 1u) voxels.push_back(x + 8 * y + 64 * z);
                std::unordered_set<int> set(voxels.begin(), voxels.end()), seen{voxels[0]};
                std::deque<int> q{voxels[0]};
                while (!q.empty()) {
                    const int v = q.front();
                    q.pop_front();
                    const int x = v & 7, y = (v >> 3) & 7, z = v >> 6;
                    const int d[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
                    for (const auto& s : d) {
                        const int nx = x + s[0], ny = y + s[1], nz = z + s[2];
                        if (nx < 0 || ny < 0 || nz < 0 || nx > 7 || ny > 7 || nz > 7) continue;
                        const int w = nx + 8 * ny + 64 * nz;
                        if (set.count(w) && seen.insert(w).second) q.push_back(w);
                    }
                }
                if (seen.size() != set.size()) return e;
            }
            e.cutFaces = static_cast<uint8_t>(c[k]);
            e.plane = static_cast<uint8_t>(k);
            return e;
        }
        return e;
    }
}

TEST(Structure, NodePlaneCutWeakestMaterialAndAnchorBit) {
    MaterialLibrary lib;
    const WorldMaterials ids = RegisterDefaultWorldMaterials(lib);
    const GraphMaterials m = MakeGraphMaterials(&lib, MaterialFlag_Liquid);
    const uint8_t palette[] = {ids.stone, ids.dirt, ids.sand, ids.glass, ids.planks, ids.iron, ids.bedrock, ids.water, ids.leaves};
    std::mt19937 rng(2024);
    uint32_t constricted = 0, nodes = 0, mixed = 0, anchors = 0;
    for (int trial = 0; trial < 400; ++trial) {
        Chunk chunk;
        chunk.Clear();
        // Shapes in cell 0 (layers): hourglasses, columns, slabs, random noise, mixed ids.
        const int kind = trial % 4;
        std::uniform_int_distribution<int> d8(0, 7), dp(0, static_cast<int>(sizeof(palette)) - 1), coin(0, 3);
        const int neckY = d8(rng), neckW = 1 + d8(rng) % 4;
        const uint8_t base = trial % 3 == 0 ? palette[dp(rng)] : ids.stone;
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    bool on = false;
                    if (kind == 0) on = y != neckY || (x < neckW && z < neckW);          // hourglass
                    else if (kind == 1) on = (y < neckY || y > neckY + 1) || (x >= 3 && x < 3 + neckW && z >= 2 && z < 4);
                    else if (kind == 2) on = coin(rng) != 0;
                    else on = (x < 4) ? y <= neckY : (y > neckY && z < 5);                  // a step, a U, ...
                    if (!on) continue;
                    const uint8_t id = coin(rng) == 0 && trial % 2 == 0 ? palette[dp(rng)] : base;
                    chunk.ids[Chunk::VoxelIndex(x, y, z)] = id;
                }
        chunk.RebuildOccupancy();
        ChunkGraph g;
        BuildChunkGraph(g, {0, 0, 0}, &chunk, 0, m);
        for (const GraphNode& node : g.nodes) {
            if (node.cell != 0) continue;
            ++nodes;
            const std::array<uint64_t, 8> v = NodeVoxels(&chunk, 0, node, m);
            const NodeExpect e = ExpectedCut(v);
            EXPECT_EQ(node.cutFaces, e.cutFaces) << "trial " << trial;
            if (e.cutFaces != kNoCut) {
                ++constricted;
                EXPECT_EQ(node.cutPlane & 7u, e.plane) << "trial " << trial;
            }
            // Weakest material per class (ties to the smaller id; foliage only if the node holds
            // nothing else), the anchor bit, the mass.
            std::array<uint8_t, 3> weak{};
            std::array<uint64_t, 3> best{~0ull, ~0ull, ~0ull};
            bool anchor = false, other = false;
            uint32_t mass = 0;
            std::unordered_set<uint8_t> seen;
            for (int z = 0; z < 8; ++z)
                for (uint64_t bits = v[z]; bits != 0; bits &= bits - 1) {
                    const int b = std::countr_zero(bits);
                    const uint8_t id = chunk.Get(b & 7, b >> 3, z);
                    seen.insert(id);
                    other = other || !m.foliage[id];
                }
            for (int z = 0; z < 8; ++z)
                for (uint64_t bits = v[z]; bits != 0; bits &= bits - 1) {
                    const int b = std::countr_zero(bits);
                    const uint8_t id = chunk.Get(b & 7, b >> 3, z);
                    anchor = anchor || m.anchor[id];
                    mass += m.densityQ[id];
                    if (other && m.foliage[id]) continue;
                    for (int k = 0; k < 3; ++k) {
                        const uint64_t key = (static_cast<uint64_t>(m.strength[k][id]) << 8) | id;
                        if (key < best[k]) {
                            best[k] = key;
                            weak[k] = id;
                        }
                    }
                }
            mixed += seen.size() > 1 ? 1u : 0u;
            anchors += anchor ? 1u : 0u;
            EXPECT_EQ(node.mass, mass) << "trial " << trial;
            for (int k = 0; k < 3; ++k) EXPECT_EQ(node.weak[k], weak[k]) << "trial " << trial << " class " << k;
            EXPECT_EQ((node.cutPlane & kCutAnchorBit) != 0, anchor) << "trial " << trial;
        }
    }
    EXPECT_GT(constricted, 50u);
    EXPECT_GT(mixed, 50u);
    EXPECT_GT(anchors, 5u);
    std::printf("[ NeckNodes ] %u nodes, %u constricted, %u mixed, %u anchors\n", nodes, constricted, mixed, anchors);

    // The shared full Uniform graph: weakest = the id, the anchor bit from the id, never split.
    for (const uint8_t id : {ids.stone, ids.bedrock}) {
        ChunkGraph g;
        BuildChunkGraph(g, {0, 0, 0}, nullptr, id, m);
        ASSERT_EQ(g.nodes.size(), 64u);
        for (const GraphNode& node : g.nodes) {
            EXPECT_EQ(node.cutFaces, kNoCut);
            EXPECT_EQ(node.weak[0], id);
            EXPECT_EQ(node.weak[2], id);
            EXPECT_EQ((node.cutPlane & kCutAnchorBit) != 0, id == ids.bedrock);
        }
    }
}

// --- the network against a per-voxel reference ----------------------------------------------------

namespace {
    /// Which node of its chunk graph a voxel belongs to (cached per cell).
    struct NodeLookup {
        const World& world;
        TerrainGraphCache& cache;
        const GraphMaterials& materials;
        std::unordered_map<uint64_t, std::array<int16_t, 512>> cells;
        struct Hit {
            const ChunkGraph* graph = nullptr;
            int node = -1;
            glm::ivec3 chunk{0};
        };
        static uint64_t Key(const glm::ivec3& v) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(v.x) & 0x1FFFFFu)) | (static_cast<uint64_t>(static_cast<uint32_t>(v.y) & 0x1FFFFFu) << 21) |
                   (static_cast<uint64_t>(static_cast<uint32_t>(v.z) & 0x1FFFFFu) << 42);
        }
        Hit At(const glm::ivec3& v) {
            Hit h;
            h.chunk = VoxelToChunk(v);
            h.graph = cache.Get(world, h.chunk);
            if (h.graph == nullptr) return h;
            const glm::ivec3 local = v - ChunkToVoxel(h.chunk);
            const glm::ivec3 cell = local / kCellSize;
            const uint32_t ci = static_cast<uint32_t>(cell.x + 4 * (cell.y + 4 * cell.z));
            const uint64_t key = Key(ChunkToVoxel(h.chunk) + cell * kCellSize);
            auto it = cells.find(key);
            if (it == cells.end()) {
                std::array<int16_t, 512> map;
                map.fill(-1);
                uint8_t uniformId = 0;
                const Chunk* data = nullptr;
                if (world.StateOf(h.chunk) == CellState::Dense) data = world.GetChunk(h.chunk);
                else world.GetChunk(h.chunk, &uniformId);
                for (uint32_t n = h.graph->cellFirst[ci]; n < h.graph->cellFirst[ci + 1]; ++n) {
                    const std::array<uint64_t, 8> mask = NodeVoxels(data, uniformId, h.graph->nodes[n], materials);
                    for (int z = 0; z < 8; ++z)
                        for (uint64_t bits = mask[z]; bits != 0; bits &= bits - 1) map[std::countr_zero(bits) + 64 * z] = static_cast<int16_t>(n);
                }
                it = cells.emplace(key, map).first;
            }
            const glm::ivec3 l = local - cell * kCellSize;
            h.node = it->second[l.x + 8 * l.y + 64 * l.z];
            return h;
        }
    };

    /// A plain Dinic on 64-bit capacities: the released set (cannot reach the sink in the residual
    /// graph) of the perturbed network.
    struct RefFlow {
        struct E {
            uint32_t to;
            uint64_t cap;
        };
        explicit RefFlow(uint32_t n) : g(n) {}
        void Add(uint32_t a, uint32_t b, uint64_t ab, uint64_t ba) {
            g[a].push_back(static_cast<uint32_t>(e.size()));
            e.push_back({b, ab});
            g[b].push_back(static_cast<uint32_t>(e.size()));
            e.push_back({a, ba});
        }
        std::vector<uint8_t> CannotReach(uint32_t s, uint32_t t) {
            const uint32_t n = static_cast<uint32_t>(g.size());
            for (;;) {
                std::vector<int> level(n, -1);
                std::deque<uint32_t> q{s};
                level[s] = 0;
                while (!q.empty()) {
                    const uint32_t v = q.front();
                    q.pop_front();
                    for (const uint32_t i : g[v])
                        if (e[i].cap > 0 && level[e[i].to] < 0) {
                            level[e[i].to] = level[v] + 1;
                            q.push_back(e[i].to);
                        }
                }
                if (level[t] < 0) break;
                std::vector<size_t> it(n, 0);
                const std::function<uint64_t(uint32_t, uint64_t)> Dfs = [&](uint32_t v, uint64_t f) -> uint64_t {
                    if (v == t) return f;
                    for (size_t& k = it[v]; k < g[v].size(); ++k) {
                        E& x = e[g[v][k]];
                        if (x.cap == 0 || level[x.to] != level[v] + 1) continue;
                        const uint64_t d = Dfs(x.to, std::min(f, x.cap));
                        if (d == 0) continue;
                        x.cap -= d;
                        e[g[v][k] ^ 1u].cap += d;
                        return d;
                    }
                    return 0;
                };
                while (Dfs(s, ~0ull) != 0) {
                }
            }
            // Reverse search from t over residual arcs.
            std::vector<uint8_t> reach(n, 0);
            std::deque<uint32_t> q{t};
            reach[t] = 1;
            while (!q.empty()) {
                const uint32_t w = q.front();
                q.pop_front();
                for (const uint32_t i : g[w]) {
                    const uint32_t x = e[i].to;
                    if (!reach[x] && e[i ^ 1u].cap > 0) {
                        reach[x] = 1;
                        q.push_back(x);
                    }
                }
            }
            std::vector<uint8_t> out(n);
            for (uint32_t v = 0; v < n; ++v) out[v] = !reach[v];
            return out;
        }
        std::vector<std::vector<uint32_t>> g;
        std::vector<E> e;
    };
}

// Random volumes (stone, planks, dirt, sand, water; reaching past the island box) on bedrock at 0.1 m,
// with hourglass columns whose narrowest layer lies inside a cell (split nodes, side by side and
// across the chunk faces x = 0 / 32, z = 0 / 32, y = 96 / 128) and structures sticking out of the
// island box (loads). The same problem is built per voxel face (one unit per node or half, one per
// exterior component) and solved by a plain Dinic. Four capacity tables per trial: random per
// material, one uniform capacity, and the uniform capacity at which the heaviest released part
// would hold exactly (ceil(load / faces)) and one MU under it (review: the first version sat far
// from every threshold and compared only released voxels). Each released part must have the
// reference component's voxels, load, cut (MU) and cut faces.
TEST(Neck, NetworkEqualsVoxelReference) {
    std::mt19937 rng(777);
    uint32_t trialsReleasing = 0, releasedVoxels = 0, splits = 0, loads = 0, solves = 0, partsCompared = 0, nearThreshold = 0;
    for (int trial = 0; trial < 14; ++trial) {
        NeckWorld w(0.1f, 80, glm::dvec3(0.0, 9.0, 0.0));
        const WorldMaterials& ids = w.ids;
        // Bedrock ground (an anchor), then random structures in x, z -45..45, y 80..150.
        w.world->FillBox({-40, 60, -40}, {48, 80, 48}, ids.bedrock);
        const uint8_t mats[] = {ids.stone, ids.planks, ids.dirt, ids.sand, ids.water, ids.stone};
        std::uniform_int_distribution<int> px(-45, 45), py(80, 150), len(1, 14), thin(1, 3), mat(0, 5), coin(0, 3), d8(0, 7), wide(3, 6);
        for (int i = 0; i < 40; ++i) {
            const glm::ivec3 lo(px(rng), py(rng), px(rng));
            const bool column = coin(rng) == 0;
            const glm::ivec3 size = column ? glm::ivec3(thin(rng), len(rng) * 2, thin(rng)) : glm::ivec3(len(rng), thin(rng) + 1, len(rng));
            w.world->FillBox(lo, lo + size, mats[mat(rng)]);
        }
        // Columns from the ground so that much is held by something.
        for (int i = 0; i < 6; ++i) {
            const int x = px(rng) / 2, z = px(rng) / 2;
            w.world->FillBox({x, 80, z}, {x + thin(rng), 80 + len(rng) * 4, z + thin(rng)}, ids.stone);
        }
        // Hourglass columns, often in touching pairs, often straddling a chunk face: a w x w column
        // with one layer narrowed to n x n at a row inside a cell (local layer 1..6).
        const int faces[] = {0, 32, -32};
        for (int i = 0; i < 10; ++i) {
            const int cw = wide(rng);
            const int x = coin(rng) < 2 ? faces[coin(rng) % 3] - cw / 2 : px(rng) / 2;
            const int z = coin(rng) < 2 ? faces[coin(rng) % 3] - cw / 2 : px(rng) / 2;
            const int y0 = coin(rng) == 0 ? 80 : 84 + len(rng) * 2;
            const int y1 = std::min(150, y0 + 12 + len(rng) * 4);
            const uint8_t id = coin(rng) == 0 ? ids.planks : ids.stone;
            for (int pair = 0; pair < (coin(rng) < 2 ? 2 : 1); ++pair) {
                const int px0 = x + pair * cw;              // the second column touches the first along x
                w.world->FillBox({px0, y0, z}, {px0 + cw, y1, z + cw}, id);
                for (int necks = 0; necks < 2; ++necks) {
                    const int row = ((y0 + 9 + len(rng) * 3) / 8) * 8 + 1 + d8(rng) % 6;
                    if (row >= y1 - 1) continue;
                    const int n = 1 + d8(rng) % (cw - 1);
                    w.world->FillBox({px0, row, z}, {px0 + cw, row + 1, z + cw}, 0);
                    w.world->FillBox({px0 + (cw - n) / 2, row, z + (cw - n) / 2}, {px0 + (cw - n) / 2 + n, row + 1, z + (cw - n) / 2 + n}, id);
                }
            }
        }
        const glm::ivec3 at(2, 96, 2);          // island box y 64..127, flood box 0..191: the window's top
        const int radius = 8;
        NeckLimits limits = w.neck;
        limits.maxReleasedVoxels = limits.maxReleasedParts = std::numeric_limits<uint32_t>::max();
        std::uniform_int_distribution<uint64_t> capRange(800, 30000);
        for (int k = 0; k < 3; ++k)
            for (const uint8_t id : {ids.stone, ids.planks, ids.dirt, ids.sand, ids.bedrock}) limits.capFace[k][id] = capRange(rng);
        const GraphSearchResult first = w.Check(at, radius, &limits);
        ASSERT_TRUE(first.complete);
        ASSERT_EQ(first.neckSkipped + first.neckFlowBudget + first.neckExteriorBudget, 0u);
        splits += first.neckSplit;
        loads += first.neckLoads;

        // The reference network (what does not depend on the capacities).
        const glm::ivec3 boxMin = at - glm::ivec3(radius + 24), boxMax = at + glm::ivec3(radius + 24);
        const glm::ivec3 floodMin = at - glm::ivec3(kFloodHalfExtent), floodMax = at + glm::ivec3(kFloodHalfExtent);
        const glm::ivec3 innerMin = floodMin + glm::ivec3(1), innerMax = floodMax - glm::ivec3(2);
        std::unordered_set<uint64_t> island;
        for (const auto* list : {&first.boxIslands, &first.faceIslands})
            for (const FloatingIsland& p : *list) {
                const glm::ivec3 sz = p.volume.Size();
                for (int z = 0; z < sz.z; ++z)
                    for (int y = 0; y < sz.y; ++y)
                        for (int x = 0; x < sz.x; ++x)
                            if (p.volume.Get(x, y, z) != 0) island.insert(NodeLookup::Key(p.origin + glm::ivec3(x, y, z)));
            }
        NodeLookup look{*w.world, w.cache, w.graphMaterials, {}};
        const auto Solid = [&](const glm::ivec3& v) {
            const uint8_t id = w.world->GetVoxel(v);
            return id != 0 && !w.graphMaterials.ignored[id] && !island.count(NodeLookup::Key(v));
        };
        // A chunk solid through (32^3 solid voxels) is ground for an exterior flood.
        std::map<std::tuple<int, int, int>, bool> solidChunk;
        const auto SolidThrough = [&](const glm::ivec3& v) {
            const glm::ivec3 c = VoxelToChunk(v);
            const auto key = std::make_tuple(c.x, c.y, c.z);
            const auto it = solidChunk.find(key);
            if (it != solidChunk.end()) return it->second;
            bool all = true;
            const glm::ivec3 o = ChunkToVoxel(c);
            for (int z = 0; z < 32 && all; ++z)
                for (int y = 0; y < 32 && all; ++y)
                    for (int x = 0; x < 32 && all; ++x) {
                        const uint8_t id = w.world->GetVoxel(o + glm::ivec3(x, y, z));
                        all = id != 0 && !w.graphMaterials.ignored[id];
                    }
            solidChunk[key] = all;
            return all;
        };
        std::unordered_map<uint64_t, uint32_t> unitOf;          // voxel -> unit
        std::vector<uint64_t> mass;
        std::vector<uint32_t> voxels;
        std::vector<uint8_t> sink;                              // 1: infinite arc to the sink
        std::vector<std::vector<glm::ivec3>> members;
        // R: every node whose bounds meet the island box (the search's seeds) and that is not an
        // island; all its voxels, split at its plane.
        const glm::ivec3 cMin = VoxelToChunk(boxMin), cMax = VoxelToChunk(boxMax - glm::ivec3(1));
        for (int cz = cMin.z; cz <= cMax.z; ++cz)
            for (int cy = cMin.y; cy <= cMax.y; ++cy)
                for (int cx = cMin.x; cx <= cMax.x; ++cx) {
                    const glm::ivec3 chunk(cx, cy, cz);
                    const ChunkGraph* graph = w.cache.Get(*w.world, chunk);
                    ASSERT_NE(graph, nullptr);
                    uint8_t uniformId = 0;
                    const Chunk* data = w.world->StateOf(chunk) == CellState::Dense ? w.world->GetChunk(chunk) : nullptr;
                    if (!data) w.world->GetChunk(chunk, &uniformId);
                    for (size_t ni = 0; ni < graph->nodes.size(); ++ni) {
                        const GraphNode& node = graph->nodes[ni];
                        const glm::ivec3 cellOrigin = ChunkToVoxel(chunk) + glm::ivec3(node.cell & 3, (node.cell >> 2) & 3, node.cell >> 4) * kCellSize;
                        const glm::ivec3 lo = cellOrigin + glm::ivec3(node.boxMin[0], node.boxMin[1], node.boxMin[2]);
                        const glm::ivec3 hi = cellOrigin + glm::ivec3(node.boxMax[0], node.boxMax[1], node.boxMax[2]);
                        if (glm::any(glm::lessThan(hi, boxMin)) || glm::any(glm::greaterThanEqual(lo, boxMax))) continue;
                        const std::array<uint64_t, 8> mask = NodeVoxels(data, uniformId, node, w.graphMaterials);
                        const int fb = node.firstVoxel;
                        if (island.count(NodeLookup::Key(cellOrigin + glm::ivec3(fb & 7, (fb >> 3) & 7, fb >> 6)))) continue;
                        const bool split = node.cutFaces != kNoCut;
                        const uint32_t firstUnit = static_cast<uint32_t>(mass.size());
                        for (int u = 0; u < (split ? 2 : 1); ++u) {
                            mass.push_back(0);
                            voxels.push_back(0);
                            members.emplace_back();
                            sink.push_back((node.cutPlane & kCutAnchorBit) != 0);
                        }
                        for (int z8 = 0; z8 < 8; ++z8)
                            for (uint64_t bits = mask[z8]; bits != 0; bits &= bits - 1) {
                                const int b = std::countr_zero(bits);
                                const glm::ivec3 p = cellOrigin + glm::ivec3(b & 7, b >> 3, z8);
                                const uint32_t unit = firstUnit + (split && (b >> 3) > (node.cutPlane & 7) ? 1u : 0u);
                                unitOf[NodeLookup::Key(p)] = unit;
                                mass[unit] += w.graphMaterials.densityQ[w.world->GetVoxel(p)];
                                ++voxels[unit];
                                members[unit].push_back(p);
                                if (glm::any(glm::lessThan(p, innerMin)) || glm::any(glm::greaterThan(p, innerMax))) sink[unit] = 1;
                            }
                        // An anchored node anchors both halves.
                        if (split && (sink[firstUnit] || sink[firstUnit + 1])) sink[firstUnit] = sink[firstUnit + 1] = 1;
                    }
                }
        const uint32_t rUnits = static_cast<uint32_t>(mass.size());
        // Exterior components (BFS per voxel from R neighbours): anchor or load.
        constexpr uint32_t kAnchorUnit = 0xFFFFFFFFu;
        const glm::ivec3 dirs[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
        for (uint32_t u = 0; u < rUnits; ++u)
            for (const glm::ivec3& v : std::vector<glm::ivec3>(members[u]))
                for (const glm::ivec3& d : dirs) {
                    const glm::ivec3 n = v + d;
                    if (!Solid(n) || unitOf.count(NodeLookup::Key(n))) continue;
                    std::vector<glm::ivec3> comp{n};
                    std::unordered_set<uint64_t> seen{NodeLookup::Key(n)};
                    bool anchor = false;
                    for (size_t head = 0; head < comp.size() && !anchor; ++head) {
                        const glm::ivec3 c = comp[head];
                        if (glm::any(glm::lessThan(c, innerMin)) || glm::any(glm::greaterThan(c, innerMax)) ||
                            w.graphMaterials.anchor[w.world->GetVoxel(c)] || comp.size() > 60000 || SolidThrough(c)) {
                            anchor = true;
                            break;
                        }
                        for (const glm::ivec3& e : dirs) {
                            const glm::ivec3 m = c + e;
                            if (!Solid(m)) continue;
                            const auto known = unitOf.find(NodeLookup::Key(m));
                            if (known != unitOf.end()) {
                                // The rest of a component an earlier search stopped in as an anchor.
                                if (known->second == kAnchorUnit) anchor = true;
                                continue;
                            }
                            if (!seen.insert(NodeLookup::Key(m)).second) continue;
                            comp.push_back(m);
                        }
                    }
                    // A node with an anchor voxel makes its whole component an anchor (the graph's rule).
                    if (!anchor)
                        for (const glm::ivec3& c : comp) {
                            const NodeLookup::Hit h = look.At(c);
                            if (h.graph->nodes[static_cast<size_t>(h.node)].cutPlane & kCutAnchorBit) anchor = true;
                        }
                    if (anchor) {
                        for (const glm::ivec3& c : comp) unitOf[NodeLookup::Key(c)] = kAnchorUnit;
                        continue;
                    }
                    const uint32_t unit = static_cast<uint32_t>(mass.size());
                    mass.push_back(0);
                    voxels.push_back(0);
                    sink.push_back(0);
                    members.emplace_back();
                    for (const glm::ivec3& c : comp) {
                        unitOf[NodeLookup::Key(c)] = unit;
                        mass[unit] += w.graphMaterials.densityQ[w.world->GetVoxel(c)];
                        ++voxels[unit];
                        members[unit].push_back(c);
                    }
                }
        // Every voxel face out of an R unit: its two units, the class each way and the weakest ids.
        const uint32_t units = static_cast<uint32_t>(mass.size()), t = units, s = units + 1;
        struct BoundaryFace {
            uint32_t from, to;              // to == t: an anchor
            uint8_t k, kb;                  // class from -> to, to -> from
            uint8_t wk[2], wkb[2];          // weakest ids of both nodes for k and kb
        };
        std::vector<BoundaryFace> boundary;
        for (uint32_t u = 0; u < rUnits; ++u)
            for (const glm::ivec3& v : members[u]) {
                const NodeLookup::Hit hv = look.At(v);
                const GraphNode& nv = hv.graph->nodes[static_cast<size_t>(hv.node)];
                for (uint8_t f = 0; f < 6; ++f) {
                    const glm::ivec3 n = v + dirs[f];
                    const auto it = unitOf.find(NodeLookup::Key(n));
                    if (it == unitOf.end() || it->second == u || !Solid(n)) continue;
                    const NodeLookup::Hit hn = look.At(n);
                    const GraphNode& nn = hn.graph->nodes[static_cast<size_t>(hn.node)];
                    const uint8_t k = StrengthClassOf(f), kb = StrengthClassOf(f ^ 1u);
                    boundary.push_back({u, it->second == kAnchorUnit ? t : it->second, k, kb, {nv.weak[k], nn.weak[k]}, {nv.weak[kb], nn.weak[kb]}});
                }
            }

        struct RefPart {
            std::unordered_set<uint64_t> voxels;
            uint64_t load = 0, cut = 0, faces = 0;
        };
        // The reference for one capacity table: its released parts (connected over unit pairs).
        const auto Reference = [&](const NeckLimits& lim) {
            std::map<std::pair<uint32_t, uint32_t>, uint64_t> cap, faceCount;    // directed, to == t: the sink
            std::set<std::pair<uint32_t, uint32_t>> pairs;
            for (const BoundaryFace& b : boundary) {
                const uint64_t c = std::min(lim.capFace[b.k][b.wk[0]], lim.capFace[b.k][b.wk[1]]);
                const uint64_t cb = std::min(lim.capFace[b.kb][b.wkb[0]], lim.capFace[b.kb][b.wkb[1]]);
                cap[{b.from, b.to}] += c;
                ++faceCount[{b.from, b.to}];
                if (b.to >= rUnits && b.to != t) {          // loads: both ways (R-R pairs come from both sides)
                    cap[{b.to, b.from}] += cb;
                    ++faceCount[{b.to, b.from}];
                }
                pairs.insert({std::min(b.from, b.to), std::max(b.from, b.to)});
            }
            RefFlow flow(units + 2);
            for (uint32_t u = 0; u < units; ++u) {
                flow.Add(s, u, mass[u] * kM, 0);
                if (sink[u]) flow.Add(u, t, ~0ull >> 2, 0);
            }
            for (const auto& [a, b] : pairs) {
                if (b == t) {
                    flow.Add(a, t, cap[{a, t}] * kM + 1, 0);
                    continue;
                }
                flow.Add(a, b, cap[{a, b}] * kM + 1, cap[{b, a}] * kM + 1);
            }
            const std::vector<uint8_t> released = flow.CannotReach(s, t);
            std::vector<std::vector<uint32_t>> adjacent(units);
            for (const auto& [a, b] : pairs)
                if (b != t && released[a] && released[b]) {
                    adjacent[a].push_back(b);
                    adjacent[b].push_back(a);
                }
            std::vector<RefPart> parts;
            std::vector<int> comp(units, -1);
            for (uint32_t u = 0; u < units; ++u) {
                if (!released[u] || comp[u] >= 0) continue;
                std::vector<uint32_t> q{u};
                comp[u] = static_cast<int>(parts.size());
                for (size_t h = 0; h < q.size(); ++h)
                    for (const uint32_t o : adjacent[q[h]])
                        if (comp[o] < 0) {
                            comp[o] = comp[u];
                            q.push_back(o);
                        }
                RefPart part;
                uint64_t n = 0;
                for (const uint32_t x : q) {
                    n += voxels[x];
                    part.load += mass[x];
                }
                for (const auto& [key, c] : cap) {
                    const auto [a, b] = key;
                    if (comp[a] != comp[u] || (b != t && comp[b] == comp[u])) continue;
                    part.cut += c;
                    part.faces += faceCount[key];
                }
                parts.emplace_back();
                if (n > 60000) continue;                          // over the piece limit: it stays
                for (const uint32_t x : q)
                    for (const glm::ivec3& v : members[x]) part.voxels.insert(NodeLookup::Key(v));
                parts.back() = std::move(part);
            }
            std::erase_if(parts, [](const RefPart& p) { return p.voxels.empty(); });
            return parts;
        };
        // Compare a check with the reference: the same parts, voxel for voxel, load, cut and faces.
        const auto Compare = [&](const GraphSearchResult& r, const NeckLimits& lim, const char* what) {
            ++solves;
            const std::vector<RefPart> ref = Reference(lim);
            SCOPED_TRACE(testing::Message() << "trial " << trial << ", " << what);
            EXPECT_EQ(r.neckPieces.size(), ref.size());
            std::unordered_map<uint64_t, size_t> partOf;
            for (size_t i = 0; i < ref.size(); ++i)
                for (const uint64_t k : ref[i].voxels) partOf[k] = i;
            uint64_t got = 0, want = 0;
            for (const RefPart& p : ref) want += p.voxels.size();
            for (size_t i = 0; i < r.neckPieces.size(); ++i) {
                const FloatingIsland& p = r.neckPieces[i];
                std::unordered_set<uint64_t> mine;
                const glm::ivec3 sz = p.volume.Size();
                for (int z = 0; z < sz.z; ++z)
                    for (int y = 0; y < sz.y; ++y)
                        for (int x = 0; x < sz.x; ++x)
                            if (p.volume.Get(x, y, z) != 0) mine.insert(NodeLookup::Key(p.origin + glm::ivec3(x, y, z)));
                got += mine.size();
                const auto it = partOf.find(*mine.begin());
                if (it == partOf.end()) {
                    ADD_FAILURE() << "piece " << i << " (" << mine.size() << " voxels) is not released by the reference";
                    continue;
                }
                const RefPart& want1 = ref[it->second];
                uint32_t missing = 0;
                for (const uint64_t k : want1.voxels) missing += mine.count(k) ? 0u : 1u;
                EXPECT_EQ(mine.size(), want1.voxels.size()) << "piece " << i;
                EXPECT_EQ(missing, 0u) << "piece " << i;
                if (missing != 0 && std::getenv("NECK_DEBUG")) {
                    int shown = 0;
                    for (const uint64_t k : want1.voxels) {
                        if (mine.count(k) || shown++ > 12) continue;
                        const glm::ivec3 v(static_cast<int>((k & 0x1FFFFFu) << 11) >> 11, static_cast<int>(((k >> 21) & 0x1FFFFFu) << 11) >> 11,
                                           static_cast<int>(((k >> 42) & 0x1FFFFFu) << 11) >> 11);
                        const NodeLookup::Hit h = look.At(v);
                        const GraphNode& nd = h.graph->nodes[static_cast<size_t>(h.node)];
                        const auto u = unitOf.find(k);
                        std::printf("missing %d %d %d id %u unit %u (R %u) node cut %u plane %u box %d..%d\n", v.x, v.y, v.z, w.world->GetVoxel(v),
                                    u == unitOf.end() ? 9999u : u->second, rUnits, nd.cutFaces, nd.cutPlane & 7u, nd.boxMin[1], nd.boxMax[1]);
                    }
                }
                EXPECT_EQ(r.neckCuts[i].load, want1.load) << "piece " << i;
                EXPECT_EQ(r.neckCuts[i].cut, want1.cut) << "piece " << i;
                EXPECT_EQ(r.neckCuts[i].faces, want1.faces) << "piece " << i;
                ++partsCompared;
            }
            EXPECT_EQ(got, want);
            return ref;
        };

        const std::vector<RefPart> refFirst = Compare(first, limits, "random capacities");
        trialsReleasing += refFirst.empty() ? 0u : 1u;
        for (const RefPart& p : refFirst) releasedVoxels += static_cast<uint32_t>(p.voxels.size());
        // One capacity for every material and class: then the cut is faces x capacity.
        NeckLimits uniform = limits;
        const uint64_t c0 = std::uniform_int_distribution<uint64_t>(3000, 60000)(rng);
        for (int k = 0; k < 3; ++k) uniform.capFace[k].fill(c0);
        const std::vector<RefPart> refUniform = Compare(w.Check(at, radius, &uniform), uniform, "uniform capacity");
        // At the threshold of the heaviest part: it holds at ceil(load / faces), not one MU below
        // (whatever else the network does there, the reference must agree).
        const RefPart* heaviest = nullptr;
        for (const RefPart& p : refUniform)
            if (p.faces > 0 && (heaviest == nullptr || p.load > heaviest->load)) heaviest = &p;
        if (heaviest != nullptr) {
            const uint64_t c = (heaviest->load + heaviest->faces - 1) / heaviest->faces;
            ++nearThreshold;
            for (const uint64_t cap : {c, c - 1}) {
                for (int k = 0; k < 3; ++k) uniform.capFace[k].fill(cap);
                Compare(w.Check(at, radius, &uniform), uniform, cap == c ? "at the threshold" : "one MU under the threshold");
            }
        }
    }
    std::printf("[ NeckReference ] %u of 14 trials release, %u voxels, %u split nodes, %u loads; %u solves, %u parts compared, %u trials at "
                "a threshold\n",
                trialsReleasing, releasedVoxels, splits, loads, solves, partsCompared, nearThreshold);
    EXPECT_GE(trialsReleasing, 7u);
    EXPECT_GE(splits, 50u);
    EXPECT_GE(loads, 20u);
    EXPECT_GE(nearThreshold, 7u);
}

// --- the rock gate --------------------------------------------------------------------------------

namespace {
    /// The id of the top voxel of the built-in terrain's column at (x, z) metres, from the
    /// generator itself (one chunk).
    uint8_t SurfaceId(const IWorldGenerator& gen, const TerrainSettings& t, float x, float z) {
        const int vx = static_cast<int>(std::floor(x / t.voxelSize)), vz = static_cast<int>(std::floor(z / t.voxelSize));
        const int top = static_cast<int>(std::floor(TerrainHeight(t, (static_cast<float>(vx) + 0.5f) * t.voxelSize, (static_cast<float>(vz) + 0.5f) * t.voxelSize) /
                                                    t.voxelSize)) - 1;
        const glm::ivec3 v(vx, top, vz), c = VoxelToChunk(v), l = v - ChunkToVoxel(c);
        auto chunk = std::make_unique<Chunk>();
        chunk->Clear();
        const ChunkContent content = gen.Generate(c, *chunk);
        if (content.kind == ChunkKind::Uniform) return content.uniformId;
        if (content.kind == ChunkKind::Empty) return 0;
        return chunk->ids[Chunk::VoxelIndex(l.x, l.y, l.z)];
    }

    /// A column of the built-in terrain whose surface fits `fits(height, slope)` (metres, rise per
    /// metre) and whose top voxel is one of `ids` (the generator's cover rule has a noise term),
    /// searched outwards from the origin on a 6 m grid; false if none within 3 km.
    bool FindColumn(const TerrainSettings& t, const IWorldGenerator& gen, const std::function<bool(float, float)>& fits,
                    std::initializer_list<uint8_t> ids, glm::dvec3& out) {
        for (int ring = 0; ring < 500; ++ring)
            for (int i = -ring; i <= ring; ++i)
                for (int side = 0; side < 4; ++side) {
                    const int a = side < 2 ? i : (side == 2 ? -ring : ring), b = side < 2 ? (side == 0 ? -ring : ring) : i;
                    const float x = 6.0f * static_cast<float>(a), z = 6.0f * static_cast<float>(b);
                    const float h = TerrainHeight(t, x, z);
                    float slope = 0.0f;
                    for (const float d : {t.voxelSize, 0.25f, 1.0f})
                        slope = std::max(slope, std::max(std::abs(TerrainHeight(t, x + d, z) - TerrainHeight(t, x - d, z)),
                                                         std::abs(TerrainHeight(t, x, z + d) - TerrainHeight(t, x, z - d))) /
                                                    (2.0f * d));
                    if (!fits(h, slope)) continue;
                    const uint8_t id = SurfaceId(gen, t, x, z);
                    if (std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
                    out = glm::dvec3(x, h, z);
                    return true;
                }
        return false;
    }
}

// The built-in terrain (caves, water, trees, Packed chunks, full height) at 0.125, 0.1 and 0.15 m.
// Sites: three near the origin and, chosen by their cover (review), a sand beach just above the sea
// (sand to 1.5 m over stone), snow above the snow line, a rock slope. Duds at column edges -
// surfaces, overhangs, cave ceilings and floors, deep caves (more than 6 m under the surface) -
// release nothing, and the check ran in full there (no budget, no unloaded chunk). Power-6 1.2 m
// blasts, also three overlapping ones deepening a crater and two craters 2.5 m apart that leave a
// thin wall, followed by a dud: on rock, grass and in caves nothing is released; on sand and snow
// (soft ground slumps by design) only pieces within 2 m of the crater.
TEST(Neck, NaturalTerrainNeverFails) {
    uint32_t duds = 0, deepDuds = 0, blasts = 0, pieces = 0, judged = 0, sites = 0, beyond = 0;
    for (const float vs : {0.125f, 0.1f, 0.15f}) {
        TerrainSettings base;
        base.voxelSize = vs;
        std::vector<std::pair<std::string, glm::dvec3>> list;
        for (int site = 0; site < 3; ++site)
            list.emplace_back("origin", glm::dvec3(static_cast<double>(site) * 37.0 - 30.0, 0.0, static_cast<double>(site) * -23.0 + 11.0));
        glm::dvec3 at;
        const float sea = base.seaLevel, snow = base.snowLine;
        {
            MaterialLibrary scratch;
            TerrainSettings t = base;
            t.materials = RegisterDefaultWorldMaterials(scratch);
            const std::shared_ptr<IWorldGenerator> gen = CreateTerrainGenerator(t);
            const WorldMaterials& m = t.materials;
            ASSERT_TRUE(FindColumn(t, *gen, [sea](float h, float slope) { return h > sea + 0.15f && h < sea + 0.85f && slope < 0.5f; }, {m.sand}, at));
            list.emplace_back("sand", at);
            ASSERT_TRUE(FindColumn(t, *gen, [snow](float h, float slope) { return h > snow + 1.0f && slope < 0.8f; }, {m.snow}, at));
            list.emplace_back("snow", at);
            ASSERT_TRUE(FindColumn(t, *gen, [sea](float h, float slope) { return h > sea + 3.0f && slope > 1.5f; }, {m.stone, m.stoneDark}, at));
            list.emplace_back("rock", at);
        }
        for (size_t site = 0; site < list.size(); ++site) {
            const std::string& kind = list[site].first;
            const bool soft = kind == "sand" || kind == "snow";
            ChunkStore store(4096);
            JobSystem jobs(3);
            MaterialLibrary materials;
            WorldConfig config;
            config.voxelSize = vs;
            config.windowSize = {8, 8, 8};
            config.fullHeight = true;              // as the game: full height, buried chunks Packed
            config.packHidden = true;
            config.nearRadius = 6.0f;
            TerrainSettings terrain = base;
            terrain.seed = kind == "origin" ? 1337 + static_cast<uint32_t>(site) : base.seed;
            terrain.materials = RegisterDefaultWorldMaterials(materials);
            World world(config, store, jobs, CreateTerrainGenerator(terrain), &materials);
            const glm::dvec3 xz(list[site].second.x, 0.0, list[site].second.z);
            world.LoadBlocking(xz);
            const WorldRayHit down = world.Raycast(xz + glm::dvec3(0.0, 120.0, 0.0), glm::vec3(0, -1, 0), 250.0f);
            ASSERT_TRUE(down.hit) << kind;
            world.LoadBlocking(down.position);
            const glm::ivec3 focus = world.WorldToVoxel(down.position);
            const uint8_t top = world.GetVoxel(focus - glm::ivec3(0, 1, 0)), cover = world.GetVoxel(focus);
            std::printf("[ NeckTerrain ] %.3f m site %s at (%.0f, %.0f): surface id %u / %u\n", vs, kind.c_str(), xz.x, xz.z, top, cover);
            ++sites;
            const GraphMaterials gm = MakeGraphMaterials(&materials, MaterialFlag_Liquid);
            const NeckLimits limits = MakeNeckLimits(gm, vs);
            TerrainGraphCache cache;
            std::mt19937 rng(static_cast<uint32_t>(site) * 31u + 7u + static_cast<uint32_t>(vs * 1000.0f));
            std::uniform_int_distribution<int> off(-24, 24), pick(0, 1000);
            const auto Search = [&](const glm::ivec3& p, int radius) {
                GraphSearchLimits l;
                l.boxMin = p - glm::ivec3(radius + 24);
                l.boxMax = p + glm::ivec3(radius + 24);
                l.floodMin = p - glm::ivec3(kFloodHalfExtent);
                l.floodMax = p + glm::ivec3(kFloodHalfExtent);
                l.maxIslandVoxels = 60000;
                l.maxNodeVisits = 32768;
                l.neck = &limits;
                return SearchIslandsOnGraph(world, cache, gm, l);
            };
            // Points where solid meets air in a column: surfaces, cave ceilings and floors, overhangs;
            // deep ones (6 m or more under the column's top) kept apart, caves reach 30 m down.
            const int deepRows = static_cast<int>(std::lround(6.0 / static_cast<double>(vs)));
            const int scanRows = static_cast<int>(std::lround(30.0 / static_cast<double>(vs)));
            std::vector<glm::ivec3> points, deep;
            for (int i = 0; i < 600 && (points.size() < 26 || deep.size() < 8); ++i) {
                const int x = focus.x + off(rng), z = focus.z + off(rng);
                std::vector<glm::ivec3> edges, deepEdges;
                int surface = std::numeric_limits<int>::min();
                for (int y = focus.y + 40; y > focus.y - scanRows; --y) {
                    if ((world.GetVoxel({x, y, z}) != 0) == (world.GetVoxel({x, y - 1, z}) != 0)) continue;
                    surface = std::max(surface, y);
                    (y < surface - deepRows ? deepEdges : edges).push_back({x, y, z});
                }
                if (!edges.empty() && points.size() < 26) points.push_back(edges[static_cast<size_t>(pick(rng)) % edges.size()]);
                if (!deepEdges.empty() && deep.size() < 8) deep.push_back(deepEdges[static_cast<size_t>(pick(rng)) % deepEdges.size()]);
            }
            const auto Dud = [&](const glm::ivec3& p, const char* what) {
                const GraphSearchResult r = Search(p, 1);
                if (!r.searched || !r.complete) return false;
                beyond += r.neckBeyondWindow + r.neckLoading;
                EXPECT_TRUE(r.neckRan) << what;
                EXPECT_EQ(r.neckSkipped + r.neckFlowBudget + r.neckExteriorBudget, 0u) << vs << " m " << kind << " " << what;
                EXPECT_FALSE(r.neckWaits) << vs << " m " << kind << " " << what << ": an unloaded chunk was taken as support";
                judged += r.neckNodes;
                for (size_t k = 0; k < r.neckPieces.size(); ++k)
                    ADD_FAILURE() << vs << " m " << kind << " " << what << " at " << p.x << ", " << p.y << ", " << p.z << ": "
                                  << static_cast<double>(r.neckCuts[k].load) / static_cast<double>(std::max<uint64_t>(1, r.neckCuts[k].cut)) << "x, "
                                  << r.neckCuts[k].voxels << " voxels released";
                return true;
            };
            for (const glm::ivec3& p : points) duds += Dud(p, "dud") ? 1u : 0u;
            for (const glm::ivec3& p : deep) {
                const bool counted = Dud(p, "deep dud");
                duds += counted ? 1u : 0u;
                deepDuds += counted ? 1u : 0u;
            }
            // Power-6 blasts of 1.2 m: nothing released on hard ground; on soft ground only near the crater.
            const int radius = static_cast<int>(std::lround(1.2 / static_cast<double>(vs)));
            const int reach = radius + static_cast<int>(std::lround(2.0 / static_cast<double>(vs)));
            const auto Blast = [&](const glm::ivec3& p, const char* what) {
                const SphereQ sphere{p * kQuantaPerVoxel + glm::ivec3(8), static_cast<uint16_t>(radius * kQuantaPerVoxel)};
                world.CarveSphereQ(sphere, QuantizePower(6.0f));
                const GraphSearchResult r = Search(p, radius);
                if (!r.searched || !r.complete) return;
                ++blasts;
                EXPECT_TRUE(r.neckRan);
                EXPECT_EQ(r.neckSkipped + r.neckFlowBudget + r.neckExteriorBudget, 0u) << vs << " m " << kind << " " << what;
                beyond += r.neckBeyondWindow + r.neckLoading;
                for (size_t k = 0; k < r.neckPieces.size(); ++k) {
                    const FloatingIsland& piece = r.neckPieces[k];
                    ++pieces;
                    const glm::ivec3 lo = piece.origin - p, hi = piece.origin + piece.volume.Size() - glm::ivec3(1) - p;
                    const glm::ivec3 far = glm::max(glm::abs(lo), glm::abs(hi));
                    const int64_t dist2 = static_cast<int64_t>(far.x) * far.x + static_cast<int64_t>(far.y) * far.y + static_cast<int64_t>(far.z) * far.z;
                    std::printf("[ NeckTerrain ] %.3f m %s %s at (%d, %d, %d): piece of %u voxels, load %.2fx its cut (%u faces)\n", vs, kind.c_str(),
                                what, p.x, p.y, p.z, piece.voxelCount,
                                static_cast<double>(r.neckCuts[k].load) / static_cast<double>(std::max<uint64_t>(1, r.neckCuts[k].cut)), r.neckCuts[k].faces);
                    if (!soft) ADD_FAILURE() << vs << " m " << kind << " " << what << ": a piece released on hard ground";
                    else EXPECT_LE(dist2, static_cast<int64_t>(reach) * reach) << "a piece reaching more than 2 m past the crater";
                }
            };
            for (size_t i = 0; i < points.size(); i += 2) Blast(points[i], "blast");
            for (size_t i = 0; i < deep.size(); i += 2) Blast(deep[i], "deep blast");
            if (points.size() > 7) {
                // Three overlapping shots deepen one crater; then a dud at its bottom.
                const glm::ivec3 p = points[1];
                for (int k = 0; k < 3; ++k) Blast(p - glm::ivec3(0, k * radius / 2, 0), "overlapping blast");
                duds += Dud(p - glm::ivec3(0, radius + radius / 2, 0), "dud in the deepened crater") ? 1u : 0u;
                // Two craters 2.5 m apart leave a thin wall; then a dud on it.
                const glm::ivec3 q = points[3], step(static_cast<int>(std::lround(1.25 / static_cast<double>(vs))), 0, 0);
                Blast(q - step, "first of two craters");
                Blast(q + step, "second of two craters");
                duds += Dud(q, "dud on the wall between two craters") ? 1u : 0u;
            }
        }
    }
    std::printf("[ NeckTerrain ] %u sites, %u duds (%u deep), %u blasts, %u pieces released, %u judged nodes in the duds, %u unloaded "
                "chunks taken as support\n",
                sites, duds, deepDuds, blasts, pieces, judged, beyond);
    EXPECT_GE(duds, 300u);
    EXPECT_GE(deepDuds, 40u);
    EXPECT_GE(blasts, 200u);
}
