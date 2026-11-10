// Ray traversal tests. The traversal under test is shaders/shared/trace.glsl compiled as C++
// - the same source the GPU runs - so these tests also cover the shader's logic.
// It is checked against an independent, textbook double-precision DDA.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {

    struct RefHit {
        bool hit = false;
        double t = 0.0;
        glm::ivec3 voxel{0};
        glm::ivec3 normal{0};
    };

    /// Amanatides & Woo voxel walk in double precision over the box [bmin, bmax).
    template <typename SolidFn>
    RefHit ReferenceDDA(const glm::dvec3& ro, const glm::dvec3& rd, double tMin, double tMax,
                        const glm::ivec3& bmin, const glm::ivec3& bmax, SolidFn&& solid) {
        RefHit out;
        double t0 = tMin, t1 = tMax;
        int axis = -1;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(rd[a]) < 1e-12) {
                if (ro[a] < bmin[a] || ro[a] > bmax[a]) return out;
                continue;
            }
            double ta = (bmin[a] - ro[a]) / rd[a];
            double tb = (bmax[a] - ro[a]) / rd[a];
            if (ta > tb) std::swap(ta, tb);
            if (ta > t0) {
                t0 = ta;
                axis = a;
            }
            t1 = std::min(t1, tb);
            if (t0 > t1) return out;
        }

        const glm::dvec3 p = ro + rd * t0;
        glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor(p)), bmin, bmax - glm::ivec3(1));
        if (axis >= 0) cell[axis] = rd[axis] > 0.0 ? bmin[axis] : bmax[axis] - 1;

        glm::ivec3 step(0);
        glm::dvec3 tNext(1e300), tDelta(1e300);
        for (int a = 0; a < 3; ++a) {
            if (std::abs(rd[a]) < 1e-12) continue;
            step[a] = rd[a] > 0.0 ? 1 : -1;
            const double boundary = rd[a] > 0.0 ? cell[a] + 1.0 : static_cast<double>(cell[a]);
            tNext[a] = (boundary - ro[a]) / rd[a];
            tDelta[a] = 1.0 / std::abs(rd[a]);
        }

        double t = t0;
        for (int guard = 0; guard < 100000; ++guard) {
            if (solid(cell)) {
                out.hit = true;
                out.t = t;
                out.voxel = cell;
                if (axis >= 0) out.normal[axis] = rd[axis] > 0.0 ? -1 : 1;
                return out;
            }
            axis = (tNext.x <= tNext.y && tNext.x <= tNext.z) ? 0 : (tNext.y <= tNext.z ? 1 : 2);
            t = tNext[axis];
            if (t > t1) return out;
            cell[axis] += step[axis];
            tNext[axis] += tDelta[axis];
            if (cell[axis] < bmin[axis] || cell[axis] >= bmax[axis]) return out;
        }
        return out;
    }

    glm::vec3 RandomDirection(Rng& rng) {
        for (;;) {
            const glm::vec3 v(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f));
            const float l = glm::length(v);
            if (l > 0.05f && l <= 1.0f) return v / l;
        }
    }

    enum class Fill { Sparse, Medium, Blobs, Shell };

    void FillChunk(Chunk& chunk, Fill fill, Rng& rng) {
        chunk.Clear();
        switch (fill) {
            case Fill::Sparse:
                for (int i = 0; i < 400; ++i)
                    chunk.Set(static_cast<int>(rng.NextBounded(32)), static_cast<int>(rng.NextBounded(32)), static_cast<int>(rng.NextBounded(32)),
                              static_cast<uint8_t>(1 + rng.NextBounded(200)));
                break;
            case Fill::Medium:
                for (int z = 0; z < 32; ++z)
                    for (int y = 0; y < 32; ++y)
                        for (int x = 0; x < 32; ++x)
                            if (rng.NextFloat() < 0.3f) chunk.Set(x, y, z, static_cast<uint8_t>(1 + rng.NextBounded(200)));
                break;
            case Fill::Blobs:
                for (int b = 0; b < 6; ++b) {
                    const glm::vec3 c(rng.Range(4.0f, 28.0f), rng.Range(4.0f, 28.0f), rng.Range(4.0f, 28.0f));
                    const float r = rng.Range(2.0f, 7.0f);
                    for (int z = 0; z < 32; ++z)
                        for (int y = 0; y < 32; ++y)
                            for (int x = 0; x < 32; ++x)
                                if (glm::distance(glm::vec3(x, y, z) + 0.5f, c) < r) chunk.Set(x, y, z, static_cast<uint8_t>(b + 1));
                }
                break;
            case Fill::Shell:
                for (int z = 0; z < 32; ++z)
                    for (int y = 0; y < 32; ++y)
                        for (int x = 0; x < 32; ++x)
                            if (x == 0 || y == 0 || z == 0 || x == 31 || y == 31 || z == 31) chunk.Set(x, y, z, 9);
                break;
        }
    }

    struct Mismatch {
        int total = 0;
        int bad = 0;
    };

    void Compare(const RefHit& ref, const RayHit& got, Mismatch& stats) {
        ++stats.total;
        bool same = ref.hit == got.hit;
        if (same && ref.hit) {
            same = ref.voxel == got.voxel && std::abs(ref.t - static_cast<double>(got.t)) < 2e-3 &&
                   (ref.normal == glm::ivec3(0) || glm::ivec3(glm::round(got.normal)) == ref.normal);
        }
        if (!same) ++stats.bad;
    }
}

TEST(Trace, SingleChunkMatchesReferenceDDA) {
    ChunkStore store(1);
    const uint32_t slot = store.Alloc();
    Rng rng(2024);
    Mismatch stats;

    for (Fill fill : {Fill::Sparse, Fill::Medium, Fill::Blobs, Fill::Shell}) {
        FillChunk(store.Get(slot), fill, rng);
        const Chunk& chunk = store.Get(slot);
        auto solid = [&chunk](const glm::ivec3& c) { return chunk.IsSolid(c.x, c.y, c.z); };

        for (int i = 0; i < 6000; ++i) {
            // Half the rays start outside the chunk, half inside it.
            const glm::vec3 origin = (i & 1) ? glm::vec3(rng.Range(-20.0f, 52.0f), rng.Range(-20.0f, 52.0f), rng.Range(-20.0f, 52.0f))
                                             : glm::vec3(rng.Range(0.1f, 31.9f), rng.Range(0.1f, 31.9f), rng.Range(0.1f, 31.9f));
            const glm::vec3 dir = RandomDirection(rng);
            const RefHit ref = ReferenceDDA(origin, dir, 0.0, 200.0, glm::ivec3(0), glm::ivec3(32), solid);
            const RayHit got = TraceChunk(store, slot, origin, dir, 0.0f, 200.0f);
            Compare(ref, got, stats);
            if (ref.hit && got.hit && ref.voxel == got.voxel) {
                EXPECT_EQ(got.material, chunk.Get(got.voxel.x, got.voxel.y, got.voxel.z));
            }
        }
    }
    // Rays grazing an edge or corner can legitimately resolve to a neighbouring voxel in float
    // vs double arithmetic; anything beyond a handful of such ties is a traversal bug.
    EXPECT_LE(stats.bad, stats.total / 2000) << stats.bad << " of " << stats.total << " rays disagree with the reference";
}

TEST(Trace, AxisAlignedRaysAndZeroComponents) {
    ChunkStore store(1);
    const uint32_t slot = store.Alloc();
    Chunk& chunk = store.Get(slot);
    chunk.Set(20, 10, 10, 5);
    chunk.Set(10, 25, 10, 6);
    chunk.Set(10, 10, 3, 7);

    RayHit h = TraceChunk(store, slot, glm::vec3(-5.0f, 10.5f, 10.5f), glm::vec3(1, 0, 0), 0.0f, 100.0f);
    ASSERT_TRUE(h.hit);
    EXPECT_EQ(h.voxel, glm::ivec3(20, 10, 10));
    EXPECT_NEAR(h.t, 25.0f, 1e-3f);
    EXPECT_EQ(glm::ivec3(glm::round(h.normal)), glm::ivec3(-1, 0, 0));
    EXPECT_EQ(h.material, 5);

    h = TraceChunk(store, slot, glm::vec3(10.5f, 40.0f, 10.5f), glm::vec3(0, -1, 0), 0.0f, 100.0f);
    ASSERT_TRUE(h.hit);
    EXPECT_EQ(h.voxel, glm::ivec3(10, 25, 10));
    EXPECT_NEAR(h.t, 14.0f, 1e-3f);
    EXPECT_EQ(glm::ivec3(glm::round(h.normal)), glm::ivec3(0, 1, 0));

    h = TraceChunk(store, slot, glm::vec3(10.5f, 10.5f, 31.5f), glm::vec3(0, 0, -1), 0.0f, 100.0f);
    ASSERT_TRUE(h.hit);
    EXPECT_EQ(h.voxel, glm::ivec3(10, 10, 3));
    EXPECT_NEAR(h.t, 27.5f, 1e-3f);

    // Miss: parallel to everything.
    h = TraceChunk(store, slot, glm::vec3(-5.0f, 1.5f, 1.5f), glm::vec3(1, 0, 0), 0.0f, 100.0f);
    EXPECT_FALSE(h.hit);
    // tMax is respected.
    h = TraceChunk(store, slot, glm::vec3(-5.0f, 10.5f, 10.5f), glm::vec3(1, 0, 0), 0.0f, 24.0f);
    EXPECT_FALSE(h.hit);
}

TEST(Trace, RayStartingInsideSolidHitsImmediately) {
    ChunkStore store(1);
    const uint32_t slot = store.Alloc();
    store.Get(slot).Fill(3);
    const RayHit h = TraceChunk(store, slot, glm::vec3(16.3f, 16.2f, 16.7f), glm::normalize(glm::vec3(0.3f, 0.9f, -0.2f)), 0.0f, 100.0f);
    ASSERT_TRUE(h.hit);
    EXPECT_NEAR(h.t, 0.0f, 1e-5f);
    EXPECT_EQ(h.voxel, glm::ivec3(16, 16, 16));
}

TEST(Trace, EmptySpaceSkippingIsEffective) {
    // A single voxel at the far corner: the ray must cross the chunk in a few big steps,
    // not ~90 single-voxel steps.
    ChunkStore store(1);
    const uint32_t slot = store.Alloc();
    store.Get(slot).Set(31, 31, 31, 1);
    const RayHit h = TraceChunk(store, slot, glm::vec3(0.4f, 0.3f, 0.2f), glm::normalize(glm::vec3(1.0f, 1.01f, 1.02f)), 0.0f, 100.0f);
    ASSERT_TRUE(h.hit);
    EXPECT_EQ(h.voxel, glm::ivec3(31, 31, 31));
    EXPECT_LT(h.steps, 30);
}

TEST(Trace, TightBoundsRestrictTheSearch) {
    ChunkStore store(1);
    const uint32_t slot = store.Alloc();
    Chunk& chunk = store.Get(slot);
    chunk.Set(5, 5, 5, 1);
    chunk.Set(20, 5, 5, 2);
    // With bounds [0,10) the second voxel is outside and must not be reported.
    RayHit h = TraceChunk(store, slot, glm::vec3(30.5f, 5.5f, 5.5f), glm::vec3(-1, 0, 0), 0.0f, 100.0f, glm::ivec3(0), glm::ivec3(10));
    ASSERT_TRUE(h.hit);
    EXPECT_EQ(h.voxel, glm::ivec3(5, 5, 5));
    h = TraceChunk(store, slot, glm::vec3(30.5f, 5.5f, 5.5f), glm::vec3(-1, 0, 0), 0.0f, 100.0f);
    ASSERT_TRUE(h.hit);
    EXPECT_EQ(h.voxel, glm::ivec3(20, 5, 5));
}

TEST(Bvh, IsWellFormed) {
    Rng rng(7);
    for (int n : {0, 1, 2, 3, 17, 256, 1000}) {
        std::vector<math::AABB> boxes;
        for (int i = 0; i < n; ++i) {
            const glm::vec3 c(rng.Range(-50.0f, 50.0f), rng.Range(-10.0f, 10.0f), rng.Range(-50.0f, 50.0f));
            const glm::vec3 e(rng.Range(0.1f, 3.0f), rng.Range(0.1f, 3.0f), rng.Range(0.1f, 3.0f));
            boxes.emplace_back(c - e, c + e);
        }
        std::vector<GpuBvhNode> nodes;
        BuildBvh(boxes, nodes);
        if (n == 0) {
            EXPECT_TRUE(nodes.empty());
            continue;
        }
        ASSERT_EQ(nodes.size(), static_cast<size_t>(2 * n - 1));

        std::vector<int> seen(static_cast<size_t>(n), 0);
        std::vector<int> depthOf(nodes.size(), 0);
        int maxDepth = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const GpuBvhNode& node = nodes[i];
            if (node.b != 0) {
                ASSERT_LT(node.a, static_cast<uint32_t>(n));
                ++seen[node.a];
                EXPECT_TRUE(glm::all(glm::lessThanEqual(node.min, boxes[node.a].min)) && glm::all(glm::greaterThanEqual(node.max, boxes[node.a].max)));
            } else {
                ASSERT_LT(node.a + 1, nodes.size());
                ASSERT_GT(node.a, i);   // children always come after their parent
                for (uint32_t c = node.a; c <= node.a + 1; ++c) {
                    EXPECT_TRUE(glm::all(glm::lessThanEqual(node.min, nodes[c].min)) && glm::all(glm::greaterThanEqual(node.max, nodes[c].max)));
                    depthOf[c] = depthOf[i] + 1;
                    maxDepth = std::max(maxDepth, depthOf[c]);
                }
            }
        }
        for (int count : seen) EXPECT_EQ(count, 1);
        EXPECT_LT(maxDepth, 30);   // must fit the fixed traversal stack of the shader (32)
    }
}

TEST(Trace, InstancesThroughBvhMatchBruteForce) {
    ChunkStore store(8);
    Rng rng(99);
    std::vector<uint32_t> slots;
    for (int i = 0; i < 4; ++i) {
        const uint32_t slot = store.Alloc();
        FillChunk(store.Get(slot), i % 2 ? Fill::Blobs : Fill::Sparse, rng);
        slots.push_back(slot);
    }

    // 40 instances scattered with arbitrary rotations and two voxel scales.
    std::vector<GpuInstance> instances;
    std::vector<math::AABB> bounds;
    for (int i = 0; i < 40; ++i) {
        math::Transform t;
        t.position = glm::vec3(rng.Range(-30.0f, 30.0f), rng.Range(-6.0f, 6.0f), rng.Range(-30.0f, 30.0f));
        t.rotation = glm::angleAxis(rng.Range(0.0f, 6.28f), RandomDirection(rng));
        t.scale = (i % 3 == 0) ? 0.25f : 0.1f;
        GpuInstance inst;
        inst.localToWorld = t.ToMatrix();
        inst.worldToLocal = t.ToInverseMatrix();
        inst.prevLocalToWorld = inst.localToWorld;
        inst.boundsMinSlot = glm::ivec4(0, 0, 0, static_cast<int>(slots[static_cast<size_t>(i) % slots.size()]));
        inst.boundsMaxPalette = glm::ivec4(32, 32, 32, 0);
        instances.push_back(inst);
        bounds.push_back(math::AABB(glm::vec3(0.0f), glm::vec3(32.0f)).Transformed(inst.localToWorld));
    }
    std::vector<GpuBvhNode> nodes;
    BuildBvh(bounds, nodes);

    TraceScene scene;
    scene.store = &store;
    scene.instances = instances;
    scene.bvh = nodes;

    int hits = 0;
    Mismatch stats;
    for (int i = 0; i < 4000; ++i) {
        const glm::vec3 origin(rng.Range(-40.0f, 40.0f), rng.Range(-10.0f, 10.0f), rng.Range(-40.0f, 40.0f));
        const glm::vec3 dir = RandomDirection(rng);
        const RayHit got = Trace(scene, origin, dir, 0.0f, 150.0f);

        // Brute force: every instance, in its own local space.
        float best = 150.0f;
        int bestInstance = -1;
        for (size_t k = 0; k < instances.size(); ++k) {
            const glm::vec3 lo = glm::vec3(instances[k].worldToLocal * glm::vec4(origin, 1.0f));
            const glm::vec3 ld = glm::vec3(instances[k].worldToLocal * glm::vec4(dir, 0.0f));   // not normalised: t stays in world units
            const RayHit h = TraceChunk(store, static_cast<uint32_t>(instances[k].boundsMinSlot.w), lo, ld, 0.0f, best);
            if (h.hit && h.t < best) {
                best = h.t;
                bestInstance = static_cast<int>(k);
            }
        }
        ++stats.total;
        const bool same = got.hit == (bestInstance >= 0) &&
                          (!got.hit || (static_cast<int>(got.instance) == bestInstance && std::abs(got.t - best) < 1e-3f));
        if (!same) ++stats.bad;
        if (got.hit) {
            ++hits;
            EXPECT_NEAR(glm::length(got.normal), 1.0f, 1e-3f);
            EXPECT_LT(glm::dot(got.normal, dir), 1e-3f) << "normal must face the ray";
        }
    }
    EXPECT_GT(hits, 100);
    EXPECT_LE(stats.bad, stats.total / 2000) << stats.bad << " of " << stats.total << " rays disagree with brute force";
}

TEST(Trace, AnyHitAgreesWithClosestHit) {
    ChunkStore store(2);
    Rng rng(5);
    const uint32_t slot = store.Alloc();
    FillChunk(store.Get(slot), Fill::Blobs, rng);
    GpuInstance inst;
    inst.boundsMinSlot = glm::ivec4(0, 0, 0, static_cast<int>(slot));
    inst.boundsMaxPalette = glm::ivec4(32, 32, 32, 0);
    std::vector<GpuInstance> instances{inst};
    std::vector<math::AABB> bounds{math::AABB(glm::vec3(0.0f), glm::vec3(32.0f))};
    std::vector<GpuBvhNode> nodes;
    BuildBvh(bounds, nodes);
    TraceScene scene;
    scene.store = &store;
    scene.instances = instances;
    scene.bvh = nodes;
    for (int i = 0; i < 2000; ++i) {
        const glm::vec3 origin(rng.Range(-10.0f, 42.0f), rng.Range(-10.0f, 42.0f), rng.Range(-10.0f, 42.0f));
        const glm::vec3 dir = RandomDirection(rng);
        EXPECT_EQ(Trace(scene, origin, dir, 0.0f, 100.0f, false).hit, Trace(scene, origin, dir, 0.0f, 100.0f, true).hit);
    }
}
