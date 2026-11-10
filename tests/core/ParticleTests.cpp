// Particle simulation and effect presets: pool limits, integration, collision, emitters,
// determinism and the render data contract (GpuParticle).

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <vector>

import vortex.core;
import vortex.fx;

using namespace vortex;
using namespace vortex::fx;

namespace {

    const glm::dvec3 kZero(0.0);

    /// A particle that does nothing by itself: no gravity, no drag, long life.
    ParticleDesc Inert(const glm::dvec3& position = glm::dvec3(0.0)) {
        ParticleDesc d;
        d.position = position;
        d.gravity = 0.0f;
        d.drag = 0.0f;
        d.lifetime = 100.0f;
        return d;
    }

    /// Emitter recipe without any randomisation of lifetime / size / colour.
    EmitterDesc PlainEmitter() {
        EmitterDesc e;
        e.particle = Inert();
        e.lifetimeJitter = 0.0f;
        e.sizeJitter = 0.0f;
        e.colorJitter = 0.0f;
        return e;
    }

    std::vector<GpuParticle> Render(const ParticleSystem& ps, const glm::dvec3& origin = glm::dvec3(0.0),
                                    const glm::dvec3& camera = glm::dvec3(0.0)) {
        std::vector<GpuParticle> out;
        ps.BuildRenderData(origin, camera, out);
        return out;
    }

    bool IsFinite(const glm::vec4& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
    }

    bool AllFinite(const std::vector<GpuParticle>& data) {
        for (const GpuParticle& g : data) {
            if (!IsFinite(g.positionSize) || !IsFinite(g.color) || !IsFinite(g.params)) return false;
        }
        return true;
    }

    bool SameData(const std::vector<GpuParticle>& a, const std::vector<GpuParticle>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].positionSize != b[i].positionSize || a[i].color != b[i].color || a[i].params != b[i].params) {
                return false;
            }
        }
        return true;
    }

    bool BelowFloor(const glm::dvec3& p) { return p.y < 0.0; }

    void ExpectSanePreset(const EmitterDesc& e, const char* name) {
        SCOPED_TRACE(name);
        const ParticleDesc& p = e.particle;
        EXPECT_TRUE(IsFinite(p.colorStart));
        EXPECT_TRUE(IsFinite(p.colorEnd));
        for (int c = 0; c < 4; ++c) {
            EXPECT_GE(p.colorStart[c], 0.0f);
            EXPECT_GE(p.colorEnd[c], 0.0f);
        }
        EXPECT_GT(p.colorStart.a, 0.0f);
        EXPECT_TRUE(std::isfinite(p.sizeStart) && p.sizeStart > 0.0f);
        EXPECT_TRUE(std::isfinite(p.sizeEnd) && p.sizeEnd > 0.0f);
        EXPECT_LT(p.sizeStart, 10.0f);
        EXPECT_LT(p.sizeEnd, 10.0f);
        EXPECT_TRUE(std::isfinite(p.lifetime) && p.lifetime > 0.0f);
        EXPECT_TRUE(std::isfinite(p.gravity));
        EXPECT_TRUE(std::isfinite(p.drag) && p.drag >= 0.0f);
        EXPECT_TRUE(std::isfinite(p.bounce) && p.bounce >= 0.0f && p.bounce <= 1.0f);
        EXPECT_TRUE(std::isfinite(p.emissive) && p.emissive >= 0.0f);
        EXPECT_TRUE(std::isfinite(e.speedMin) && e.speedMin >= 0.0f);
        EXPECT_TRUE(std::isfinite(e.speedMax) && e.speedMax >= e.speedMin);
        EXPECT_TRUE(std::isfinite(e.spread) && e.spread > 0.0f);
        EXPECT_GT(glm::length(e.direction), 0.0f);
        for (int c = 0; c < 3; ++c) {
            EXPECT_TRUE(std::isfinite(e.positionJitter[c]) && e.positionJitter[c] >= 0.0f);
        }
        EXPECT_TRUE(e.lifetimeJitter >= 0.0f && e.lifetimeJitter < 1.0f);
        EXPECT_TRUE(e.sizeJitter >= 0.0f && e.sizeJitter < 1.0f);
        EXPECT_TRUE(e.colorJitter >= 0.0f && e.colorJitter < 1.0f);
        EXPECT_TRUE(std::isfinite(e.rate) && e.rate >= 0.0f);

        // The recipe actually produces particles that simulate to finite values.
        ParticleSystem ps(64, 5);
        EXPECT_EQ(ps.Burst(e, glm::dvec3(0.0, 5.0, 0.0), 32), 32u);
        for (int i = 0; i < 10; ++i) ps.Update(1.0f / 60.0f, BelowFloor);
        EXPECT_TRUE(AllFinite(Render(ps)));
    }
}

TEST(Particles, StartsEmpty) {
    ParticleSystem ps(16);
    EXPECT_EQ(ps.Capacity(), 16u);
    EXPECT_EQ(ps.AliveCount(), 0u);
    std::vector<GpuParticle> out(3);
    ps.BuildRenderData(kZero, kZero, out);
    EXPECT_TRUE(out.empty());
}

TEST(Particles, CapacityLimitDropsNewParticles) {
    ParticleSystem ps(4);
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(ps.Emit(Inert(glm::dvec3(i, 0.0, 0.0))));
    EXPECT_FALSE(ps.Emit(Inert(glm::dvec3(99.0, 0.0, 0.0))));
    EXPECT_EQ(ps.Burst(PlainEmitter(), kZero, 10), 0u);
    EXPECT_EQ(ps.AliveCount(), 4u);

    ps.Update(0.1f);
    EXPECT_EQ(ps.AliveCount(), 4u);

    // The original four are intact: x = 0, 1, 2, 3 in some order, nothing from the dropped ones.
    const std::vector<GpuParticle> data = Render(ps);
    ASSERT_EQ(data.size(), 4u);
    EXPECT_TRUE(AllFinite(data));
    float sum = 0.0f;
    for (const GpuParticle& g : data) {
        EXPECT_LE(g.positionSize.x, 3.0f);
        sum += g.positionSize.x;
    }
    EXPECT_FLOAT_EQ(sum, 6.0f);
}

TEST(Particles, BurstFillsOnlyTheFreeSpace) {
    ParticleSystem ps(8);
    EXPECT_TRUE(ps.Emit(Inert()));
    EXPECT_EQ(ps.Burst(PlainEmitter(), kZero, 100), 7u);
    EXPECT_EQ(ps.AliveCount(), 8u);

    ps.Clear();
    EXPECT_EQ(ps.AliveCount(), 0u);
    EXPECT_EQ(ps.Burst(PlainEmitter(), kZero, 3), 3u);
}

TEST(Particles, ZeroCapacityIsHarmless) {
    ParticleSystem ps(0);
    EXPECT_FALSE(ps.Emit(Inert()));
    EXPECT_EQ(ps.Burst(PlainEmitter(), kZero, 5), 0u);
    EmitterDesc e = PlainEmitter();
    e.rate = 100.0f;
    ps.AddEmitter(e, kZero);
    ps.Update(1.0f);
    EXPECT_EQ(ps.AliveCount(), 0u);
    EXPECT_TRUE(Render(ps).empty());
}

TEST(Particles, ExpireAfterTheirLifetime) {
    ParticleSystem ps(8);
    ParticleDesc shortLived = Inert();
    shortLived.lifetime = 0.5f;
    ParticleDesc longLived = Inert(glm::dvec3(7.0, 0.0, 0.0));
    longLived.lifetime = 2.0f;
    ASSERT_TRUE(ps.Emit(shortLived));
    ASSERT_TRUE(ps.Emit(longLived));
    ASSERT_TRUE(ps.Emit(shortLived));

    ps.Update(0.25f);
    EXPECT_EQ(ps.AliveCount(), 3u);
    ps.Update(0.3f);
    ASSERT_EQ(ps.AliveCount(), 1u);
    EXPECT_FLOAT_EQ(Render(ps)[0].positionSize.x, 7.0f);   // the survivor is the long-lived one
    ps.Update(1.5f);
    EXPECT_EQ(ps.AliveCount(), 0u);
}

TEST(Particles, FreeFallMatchesHalfGTSquared) {
    ParticleSystem ps(4);
    ParticleDesc d = Inert(glm::dvec3(0.0, 100.0, 0.0));
    d.gravity = 1.0f;
    ASSERT_TRUE(ps.Emit(d));

    const float dt = 1.0f / 240.0f;
    for (int i = 0; i < 240; ++i) ps.Update(dt);

    const std::vector<GpuParticle> data = Render(ps);
    ASSERT_EQ(data.size(), 1u);
    EXPECT_NEAR(data[0].positionSize.y, 100.0f - 0.5f * 9.81f, 0.05f);
    EXPECT_FLOAT_EQ(data[0].positionSize.x, 0.0f);
    EXPECT_FLOAT_EQ(data[0].positionSize.z, 0.0f);
}

TEST(Particles, GravityMultiplierAndDrag) {
    ParticleSystem ps(4);
    ParticleDesc rising = Inert();
    rising.gravity = -1.0f;
    ParticleDesc dragged = Inert();
    dragged.velocity = glm::vec3(10.0f, 0.0f, 0.0f);
    dragged.drag = 2.0f;
    ASSERT_TRUE(ps.Emit(rising));
    ASSERT_TRUE(ps.Emit(dragged));

    const float dt = 1.0f / 240.0f;
    for (int i = 0; i < 240; ++i) ps.Update(dt);

    // Camera far along -x: the dragged particle (x > 0) is the farthest, so it comes first.
    const std::vector<GpuParticle> data = Render(ps, kZero, glm::dvec3(-1000.0, 0.0, 0.0));
    ASSERT_EQ(data.size(), 2u);
    // x(t) = v0 / k * (1 - exp(-k t)) = 5 * (1 - e^-2)
    EXPECT_NEAR(data[0].positionSize.x, 5.0f * (1.0f - std::exp(-2.0f)), 0.05f);
    EXPECT_NEAR(data[1].positionSize.y, 0.5f * 9.81f, 0.05f);
}

TEST(Particles, CollidingParticlesStayAboveTheFloor) {
    ParticleSystem ps(256, 7);
    EmitterDesc e = PlainEmitter();
    e.particle.gravity = 1.0f;
    e.particle.bounce = 0.5f;
    e.particle.flags = ParticleFlag_Collide;
    e.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    e.spread = 3.14159265f;
    e.speedMin = 1.0f;
    e.speedMax = 8.0f;
    ASSERT_EQ(ps.Burst(e, glm::dvec3(0.0, 1.0, 0.0), 200), 200u);

    std::vector<GpuParticle> data;
    for (int step = 0; step < 180; ++step) {
        ps.Update(1.0f / 60.0f, BelowFloor);
        ps.BuildRenderData(kZero, kZero, data);
        for (const GpuParticle& g : data) ASSERT_GE(g.positionSize.y, 0.0f) << "step " << step;
    }
    EXPECT_EQ(ps.AliveCount(), 200u);
    EXPECT_TRUE(AllFinite(data));

    // After three seconds everything has come down again and only small bounces remain.
    for (const GpuParticle& g : data) EXPECT_LT(g.positionSize.y, 2.0f);
}

TEST(Particles, BounceReflectsTheBlockedAxisOnly) {
    ParticleSystem ps(4);
    ParticleDesc d = Inert(glm::dvec3(0.0, 0.05, 0.0));
    d.velocity = glm::vec3(1.0f, -1.0f, 0.0f);
    d.bounce = 0.5f;
    d.flags = ParticleFlag_Collide;
    ASSERT_TRUE(ps.Emit(d));

    ps.Update(0.1f, BelowFloor);   // would end at y = -0.05: blocked in y, free in x
    std::vector<GpuParticle> data = Render(ps);
    ASSERT_EQ(data.size(), 1u);
    EXPECT_NEAR(data[0].positionSize.x, 0.1f, 1e-5f);
    EXPECT_NEAR(data[0].positionSize.y, 0.05f, 1e-5f);

    ps.Update(0.1f, BelowFloor);   // now moving up at 0.5 m/s and along x at 0.8 m/s
    data = Render(ps);
    EXPECT_NEAR(data[0].positionSize.x, 0.18f, 1e-5f);
    EXPECT_NEAR(data[0].positionSize.y, 0.10f, 1e-5f);
}

TEST(Particles, WithoutCollideFlagOrQueryParticlesPassThrough) {
    ParticleSystem ps(4);
    ParticleDesc d = Inert(glm::dvec3(0.0, 1.0, 0.0));
    d.velocity = glm::vec3(0.0f, -5.0f, 0.0f);
    ASSERT_TRUE(ps.Emit(d));          // no Collide flag, query given
    ps.Update(1.0f, BelowFloor);
    EXPECT_NEAR(Render(ps)[0].positionSize.y, -4.0f, 1e-4f);

    ps.Clear();
    d.flags = ParticleFlag_Collide | ParticleFlag_DieOnCollide;
    ASSERT_TRUE(ps.Emit(d));          // Collide flag, no query
    ps.Update(1.0f);
    ASSERT_EQ(ps.AliveCount(), 1u);
    EXPECT_NEAR(Render(ps)[0].positionSize.y, -4.0f, 1e-4f);
}

TEST(Particles, DieOnCollideRemovesOnContact) {
    ParticleSystem ps(64);
    ParticleDesc falling = Inert(glm::dvec3(0.0, 1.0, 0.0));
    falling.velocity = glm::vec3(0.0f, -5.0f, 0.0f);
    falling.flags = ParticleFlag_Collide | ParticleFlag_DieOnCollide;
    for (int i = 0; i < 20; ++i) {
        falling.position.x = i;
        ASSERT_TRUE(ps.Emit(falling));
    }
    ParticleDesc bouncing = falling;
    bouncing.flags = ParticleFlag_Collide;
    ASSERT_TRUE(ps.Emit(bouncing));
    ParticleDesc hovering = Inert(glm::dvec3(0.0, 3.0, 0.0));
    hovering.flags = ParticleFlag_Collide | ParticleFlag_DieOnCollide;
    ASSERT_TRUE(ps.Emit(hovering));

    ps.Update(0.1f, BelowFloor);
    EXPECT_EQ(ps.AliveCount(), 22u);   // nothing has reached the floor yet
    for (int i = 0; i < 60; ++i) ps.Update(1.0f / 60.0f, BelowFloor);

    EXPECT_EQ(ps.AliveCount(), 2u);    // the bouncing one and the one that never touched anything
    for (const GpuParticle& g : Render(ps)) EXPECT_GE(g.positionSize.y, 0.0f);
}

TEST(Particles, EmbeddedParticlesTravelUntilTheyLeaveTheSolid) {
    ParticleSystem ps(4);
    ParticleDesc d = Inert(glm::dvec3(0.0, -0.25, 0.0));   // spawned inside the ground
    d.velocity = glm::vec3(0.0f, 1.0f, 0.0f);
    d.bounce = 0.5f;
    d.flags = ParticleFlag_Collide;
    ASSERT_TRUE(ps.Emit(d));

    ps.Update(0.125f, BelowFloor);   // still inside: keeps moving instead of freezing
    EXPECT_NEAR(Render(ps)[0].positionSize.y, -0.125f, 1e-5f);
    for (int i = 0; i < 3; ++i) ps.Update(0.125f, BelowFloor);
    EXPECT_NEAR(Render(ps)[0].positionSize.y, 0.25f, 1e-5f);   // out, velocity untouched
    EXPECT_TRUE(AllFinite(Render(ps)));
}

TEST(Particles, NonFiniteRecipesSpawnNothing) {
    ParticleSystem ps(64);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EmitterDesc e = PlainEmitter();
    e.particle.lifetime = nan;
    EXPECT_EQ(ps.Burst(e, kZero, 4000000000u), 0u);   // returns at once, no per-particle work
    e = PlainEmitter();
    e.speedMax = std::numeric_limits<float>::infinity();
    EXPECT_EQ(ps.Burst(e, kZero, 10), 0u);
    e = PlainEmitter();
    e.positionJitter.y = nan;
    EXPECT_EQ(ps.Burst(e, kZero, 10), 0u);
    EXPECT_EQ(ps.Burst(PlainEmitter(), glm::dvec3(nan, 0.0, 0.0), 10), 0u);
    EXPECT_EQ(ps.AliveCount(), 0u);

    // A degenerate direction falls back to straight up instead of producing NaNs.
    e = PlainEmitter();
    e.direction = glm::vec3(0.0f);
    e.spread = 0.0f;
    e.speedMin = 2.0f;
    e.speedMax = 2.0f;
    ASSERT_EQ(ps.Burst(e, kZero, 8), 8u);
    ps.Update(1.0f);
    for (const GpuParticle& g : Render(ps)) {
        EXPECT_NEAR(g.positionSize.x, 0.0f, 1e-4f);
        EXPECT_NEAR(g.positionSize.y, 2.0f, 1e-4f);
        EXPECT_NEAR(g.positionSize.z, 0.0f, 1e-4f);
    }
}

TEST(Particles, EmitterRateIsExactOverTime) {
    ParticleSystem ps(2000);
    EmitterDesc e = PlainEmitter();
    e.rate = 37.0f;
    const EmitterId id = ps.AddEmitter(e, kZero);
    ASSERT_TRUE(ps.IsEmitterValid(id));

    for (int i = 0; i < 600; ++i) ps.Update(1.0f / 60.0f);   // 10 seconds
    EXPECT_NEAR(static_cast<double>(ps.AliveCount()), 370.0, 1.0);

    // A rate below one particle per frame still adds up.
    ParticleSystem slow(100);
    e.rate = 2.5f;
    slow.AddEmitter(e, kZero);
    for (int i = 0; i < 480; ++i) slow.Update(1.0f / 120.0f);   // 4 seconds
    EXPECT_NEAR(static_cast<double>(slow.AliveCount()), 10.0, 1.0);
}

TEST(Particles, DisabledEmittersDoNotEmit) {
    ParticleSystem ps(1000);
    EmitterDesc e = PlainEmitter();
    e.rate = 60.0f;
    const EmitterId id = ps.AddEmitter(e, kZero);

    ps.SetEmitterEnabled(id, false);
    for (int i = 0; i < 60; ++i) ps.Update(1.0f / 60.0f);
    EXPECT_EQ(ps.AliveCount(), 0u);

    ps.SetEmitterEnabled(id, true);
    for (int i = 0; i < 60; ++i) ps.Update(1.0f / 60.0f);
    EXPECT_NEAR(static_cast<double>(ps.AliveCount()), 60.0, 1.0);   // no backlog from the disabled second
}

TEST(Particles, EmitterPositionIsUsedForNewParticles) {
    ParticleSystem ps(100);
    EmitterDesc e = PlainEmitter();
    e.rate = 10.0f;
    e.speedMin = 0.0f;
    e.speedMax = 0.0f;
    const EmitterId id = ps.AddEmitter(e, glm::dvec3(5.0, 0.0, 0.0));
    ps.Update(1.0f);
    const uint32_t first = ps.AliveCount();
    EXPECT_NEAR(static_cast<double>(first), 10.0, 1.0);

    ps.SetEmitterPosition(id, glm::dvec3(-5.0, 0.0, 0.0));
    ps.Update(1.0f);

    uint32_t left = 0, right = 0;
    for (const GpuParticle& g : Render(ps)) {
        if (g.positionSize.x == 5.0f) ++right;
        else if (g.positionSize.x == -5.0f) ++left;
    }
    EXPECT_EQ(right, first);
    EXPECT_EQ(left + right, ps.AliveCount());
    EXPECT_GT(left, 0u);
}

TEST(Particles, StaleEmitterIdsAreIgnored) {
    ParticleSystem ps(1000);
    EmitterDesc e = PlainEmitter();
    e.rate = 60.0f;

    const EmitterId invalid;
    EXPECT_FALSE(ps.IsEmitterValid(invalid));
    ps.SetEmitterEnabled(invalid, false);
    ps.SetEmitterPosition(invalid, glm::dvec3(1.0));
    ps.RemoveEmitter(invalid);
    EmitterId outOfRange;
    outOfRange.index = 12345;
    EXPECT_FALSE(ps.IsEmitterValid(outOfRange));
    ps.RemoveEmitter(outOfRange);

    const EmitterId first = ps.AddEmitter(e, kZero);
    ASSERT_TRUE(ps.IsEmitterValid(first));
    ps.RemoveEmitter(first);
    EXPECT_FALSE(ps.IsEmitterValid(first));
    ps.RemoveEmitter(first);   // twice is fine

    ps.Update(1.0f);
    EXPECT_EQ(ps.AliveCount(), 0u);   // a removed emitter emits nothing

    // The new emitter may reuse the slot; the stale id must not be able to reach it.
    const EmitterId second = ps.AddEmitter(e, kZero);
    ASSERT_TRUE(ps.IsEmitterValid(second));
    EXPECT_FALSE(first == second);
    EXPECT_FALSE(ps.IsEmitterValid(first));
    ps.SetEmitterEnabled(first, false);
    ps.SetEmitterPosition(first, glm::dvec3(1.0e6));
    ps.RemoveEmitter(first);
    EXPECT_TRUE(ps.IsEmitterValid(second));

    ps.Update(1.0f);
    EXPECT_NEAR(static_cast<double>(ps.AliveCount()), 60.0, 1.0);
    for (const GpuParticle& g : Render(ps)) EXPECT_LT(std::abs(g.positionSize.x), 100.0f);
}

TEST(Particles, BurstRespectsConeSpeedAndJitterBox) {
    ParticleSystem ps(512, 3);
    EmitterDesc e = PlainEmitter();
    e.direction = glm::vec3(3.0f, 0.0f, 0.0f);   // not normalised on purpose
    e.spread = 0.3f;
    e.speedMin = 2.0f;
    e.speedMax = 4.0f;
    ASSERT_EQ(ps.Burst(e, kZero, 400), 400u);
    ps.Update(1.0f);   // no gravity / drag: position = velocity

    float minSpeed = 1e9f, maxSpeed = 0.0f, maxAngle = 0.0f;
    for (const GpuParticle& g : Render(ps)) {
        const glm::vec3 v(g.positionSize);
        const float speed = glm::length(v);
        minSpeed = std::min(minSpeed, speed);
        maxSpeed = std::max(maxSpeed, speed);
        maxAngle = std::max(maxAngle, std::acos(std::min(v.x / speed, 1.0f)));
    }
    EXPECT_GE(minSpeed, 2.0f - 1e-3f);
    EXPECT_LE(maxSpeed, 4.0f + 1e-3f);
    EXPECT_LE(maxAngle, 0.3f + 1e-3f);
    EXPECT_GT(maxAngle, 0.15f);            // the cone is actually used, not just its axis
    EXPECT_GT(maxSpeed - minSpeed, 1.0f);

    // Full sphere: particles leave in every direction.
    ps.Clear();
    e.spread = 3.14159265f;
    ASSERT_EQ(ps.Burst(e, kZero, 400), 400u);
    ps.Update(1.0f);
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const GpuParticle& g : Render(ps)) {
        lo = glm::min(lo, glm::vec3(g.positionSize));
        hi = glm::max(hi, glm::vec3(g.positionSize));
    }
    for (int a = 0; a < 3; ++a) {
        EXPECT_LT(lo[a], -1.0f);
        EXPECT_GT(hi[a], 1.0f);
    }

    // Jitter box: spawn positions stay inside the half extents around the burst position.
    ps.Clear();
    e.speedMin = 0.0f;
    e.speedMax = 0.0f;
    e.positionJitter = glm::vec3(0.5f, 0.0f, 2.0f);
    ASSERT_EQ(ps.Burst(e, glm::dvec3(10.0, 20.0, 30.0), 400), 400u);
    float spanX = 0.0f, spanZ = 0.0f;
    for (const GpuParticle& g : Render(ps, glm::dvec3(10.0, 20.0, 30.0))) {
        EXPECT_LE(std::abs(g.positionSize.x), 0.5f + 1e-4f);
        EXPECT_FLOAT_EQ(g.positionSize.y, 0.0f);
        EXPECT_LE(std::abs(g.positionSize.z), 2.0f + 1e-4f);
        spanX = std::max(spanX, std::abs(g.positionSize.x));
        spanZ = std::max(spanZ, std::abs(g.positionSize.z));
    }
    EXPECT_GT(spanX, 0.4f);
    EXPECT_GT(spanZ, 1.6f);
}

TEST(Particles, BurstJitterStaysWithinTheDocumentedFractions) {
    ParticleSystem ps(512, 11);
    EmitterDesc e = PlainEmitter();
    e.particle.sizeStart = 0.2f;
    e.particle.sizeEnd = 0.2f;
    e.particle.colorStart = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    e.particle.colorEnd = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    e.particle.lifetime = 1.0f;
    e.lifetimeJitter = 0.5f;
    e.sizeJitter = 0.25f;
    e.colorJitter = 0.2f;
    e.speedMin = 0.0f;
    e.speedMax = 0.0f;
    ASSERT_EQ(ps.Burst(e, kZero, 400), 400u);

    float minHalf = 1e9f, maxHalf = 0.0f, minC = 1e9f, maxC = 0.0f;
    for (const GpuParticle& g : Render(ps)) {
        minHalf = std::min(minHalf, g.positionSize.w);
        maxHalf = std::max(maxHalf, g.positionSize.w);
        minC = std::min(minC, g.color.r);
        maxC = std::max(maxC, g.color.r);
        EXPECT_FLOAT_EQ(g.color.a, 1.0f);   // brightness jitter leaves the opacity alone
    }
    EXPECT_GE(minHalf, 0.1f * 0.75f - 1e-5f);
    EXPECT_LE(maxHalf, 0.1f * 1.25f + 1e-5f);
    EXPECT_GT(maxHalf - minHalf, 0.02f);
    EXPECT_GE(minC, 0.5f * 0.8f - 1e-5f);
    EXPECT_LE(maxC, 0.5f * 1.2f + 1e-5f);
    EXPECT_GT(maxC - minC, 0.1f);

    // Lifetimes lie in [0.5, 1.5]: nothing dies before 0.5 s, everything is gone after 1.5 s,
    // and some - not all - have gone by 1.0 s.
    ps.Update(0.49f);
    EXPECT_EQ(ps.AliveCount(), 400u);
    ps.Update(0.51f);
    EXPECT_GT(ps.AliveCount(), 100u);
    EXPECT_LT(ps.AliveCount(), 300u);
    ps.Update(0.51f);
    EXPECT_EQ(ps.AliveCount(), 0u);
}

TEST(Particles, SameSeedAndCallsGiveIdenticalRenderData) {
    const auto run = [](uint64_t seed) {
        ParticleSystem ps(600, seed);
        ps.Burst(effects::Debris(glm::vec3(0.4f, 0.3f, 0.2f)), glm::dvec3(0.0, 0.5, 0.0), 60);
        EmitterDesc flame = effects::Flame();
        flame.rate = 90.0f;
        const EmitterId id = ps.AddEmitter(flame, glm::dvec3(1.0, 0.2, 0.0));
        SpawnExplosion(ps, glm::dvec3(-2.0, 1.0, 0.5), 1.0f);
        for (int i = 0; i < 90; ++i) {
            if (i == 30) SpawnVoxelBreak(ps, glm::dvec3(0.5, 0.5, 0.5), glm::vec3(0.2f, 0.6f, 0.2f), 0.1f, glm::vec3(2.0f, 0.0f, 0.0f));
            if (i == 60) ps.RemoveEmitter(id);
            ps.Update(1.0f / 60.0f, BelowFloor);
        }
        return Render(ps, glm::dvec3(0.0), glm::dvec3(3.0, 2.0, 5.0));
    };

    const std::vector<GpuParticle> a = run(42);
    const std::vector<GpuParticle> b = run(42);
    const std::vector<GpuParticle> c = run(43);
    ASSERT_FALSE(a.empty());
    EXPECT_TRUE(AllFinite(a));
    EXPECT_TRUE(SameData(a, b));
    EXPECT_FALSE(SameData(a, c));
}

TEST(Particles, RenderDataIsRelativeToTheOrigin) {
    ParticleSystem ps(4);
    // Far from the world origin: only a subtraction in double keeps the quarter metre.
    ParticleDesc d = Inert(glm::dvec3(10000000.25, -5000000.5, 3000000.75));
    d.sizeStart = 0.2f;
    d.sizeEnd = 0.2f;
    ASSERT_TRUE(ps.Emit(d));

    const std::vector<GpuParticle> data = Render(ps, glm::dvec3(10000000.0, -5000000.0, 3000000.0));
    ASSERT_EQ(data.size(), 1u);
    EXPECT_FLOAT_EQ(data[0].positionSize.x, 0.25f);
    EXPECT_FLOAT_EQ(data[0].positionSize.y, -0.5f);
    EXPECT_FLOAT_EQ(data[0].positionSize.z, 0.75f);
    EXPECT_FLOAT_EQ(data[0].positionSize.w, 0.1f);   // half the edge length
}

TEST(Particles, RenderDataIsSortedBackToFront) {
    ParticleSystem ps(512, 9);
    EmitterDesc e = PlainEmitter();
    e.positionJitter = glm::vec3(20.0f, 5.0f, 20.0f);
    e.speedMin = 0.0f;
    e.speedMax = 3.0f;
    ASSERT_EQ(ps.Burst(e, kZero, 300), 300u);
    ps.Update(0.5f);

    const glm::dvec3 origin(100.0, 0.0, -50.0);
    const glm::dvec3 camera(4.0, 2.0, -7.0);
    const std::vector<GpuParticle> data = Render(ps, origin, camera);
    ASSERT_EQ(data.size(), 300u);

    const glm::vec3 cameraLocal(camera - origin);
    float previous = std::numeric_limits<float>::max();
    for (const GpuParticle& g : data) {
        const float distance = glm::distance(glm::vec3(g.positionSize), cameraLocal);
        EXPECT_LE(distance, previous + 1e-3f);
        previous = distance;
    }
    EXPECT_GT(glm::distance(glm::vec3(data.front().positionSize), cameraLocal),
              glm::distance(glm::vec3(data.back().positionSize), cameraLocal));
}

TEST(Particles, ColourAndSizeInterpolateOverTheLifetime) {
    ParticleSystem ps(4);
    ParticleDesc d = Inert();
    d.colorStart = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    d.colorEnd = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    d.sizeStart = 0.2f;
    d.sizeEnd = 0.4f;
    d.lifetime = 2.0f;
    d.emissive = 0.0f;
    ASSERT_TRUE(ps.Emit(d));

    std::vector<GpuParticle> data = Render(ps);
    ASSERT_EQ(data.size(), 1u);
    EXPECT_FLOAT_EQ(data[0].color.r, 1.0f);
    EXPECT_FLOAT_EQ(data[0].color.b, 0.0f);
    EXPECT_FLOAT_EQ(data[0].color.a, 1.0f);
    EXPECT_FLOAT_EQ(data[0].positionSize.w, 0.1f);
    EXPECT_FLOAT_EQ(data[0].params.x, 0.0f);   // not emissive
    EXPECT_FLOAT_EQ(data[0].params.y, 0.0f);   // square
    EXPECT_FLOAT_EQ(data[0].params.z, 0.0f);   // lit

    ps.Update(1.0f);
    data = Render(ps);
    ASSERT_EQ(data.size(), 1u);
    EXPECT_NEAR(data[0].color.r, 0.5f, 1e-5f);
    EXPECT_NEAR(data[0].color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(data[0].color.b, 0.5f, 1e-5f);
    EXPECT_NEAR(data[0].color.a, 0.5f, 1e-5f);
    EXPECT_NEAR(data[0].positionSize.w, 0.15f, 1e-5f);
}

TEST(Particles, AdditiveSoftDiscAndUnlitFlagsReachTheGpuData) {
    ParticleSystem ps(8);
    ParticleDesc additive = Inert(glm::dvec3(0.0, 0.0, -1.0));
    additive.colorStart = glm::vec4(1.0f, 0.5f, 0.25f, 1.0f);
    additive.colorEnd = glm::vec4(1.0f, 0.5f, 0.25f, 1.0f);
    additive.emissive = 0.0f;
    additive.flags = ParticleFlag_Additive;
    ParticleDesc bright = Inert(glm::dvec3(0.0, 0.0, -2.0));
    bright.emissive = 7.5f;
    bright.flags = ParticleFlag_Additive | ParticleFlag_SoftDisc | ParticleFlag_Unlit;
    ParticleDesc plain = Inert(glm::dvec3(0.0, 0.0, -3.0));
    plain.emissive = 2.0f;
    ASSERT_TRUE(ps.Emit(additive));
    ASSERT_TRUE(ps.Emit(bright));
    ASSERT_TRUE(ps.Emit(plain));

    const std::vector<GpuParticle> data = Render(ps);   // farthest first: plain, bright, additive
    ASSERT_EQ(data.size(), 3u);

    EXPECT_FLOAT_EQ(data[0].color.a, 1.0f);
    EXPECT_FLOAT_EQ(data[0].params.x, 2.0f);
    EXPECT_FLOAT_EQ(data[0].params.y, 0.0f);
    EXPECT_FLOAT_EQ(data[0].params.z, 0.0f);

    EXPECT_FLOAT_EQ(data[1].color.a, 0.0f);
    EXPECT_FLOAT_EQ(data[1].params.x, 7.5f);
    EXPECT_FLOAT_EQ(data[1].params.y, 1.0f);
    EXPECT_FLOAT_EQ(data[1].params.z, 1.0f);

    EXPECT_FLOAT_EQ(data[2].color.a, 0.0f);
    EXPECT_GE(data[2].params.x, 1.0f);       // additive particles always emit
    EXPECT_FLOAT_EQ(data[2].color.r, 1.0f);
    EXPECT_FLOAT_EQ(data[2].color.g, 0.5f);
    EXPECT_FLOAT_EQ(data[2].color.b, 0.25f);
    for (const GpuParticle& g : data) EXPECT_FLOAT_EQ(g.params.w, 0.0f);
}

TEST(Particles, InvalidInputIsRejectedOrIgnored) {
    ParticleSystem ps(8);
    ParticleDesc d = Inert();
    d.lifetime = 0.0f;
    EXPECT_FALSE(ps.Emit(d));
    d = Inert();
    d.position.x = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(ps.Emit(d));
    EXPECT_EQ(ps.AliveCount(), 0u);

    ASSERT_TRUE(ps.Emit(Inert(glm::dvec3(1.0, 2.0, 3.0))));
    ps.Update(0.0f);
    ps.Update(-1.0f);
    ps.Update(std::numeric_limits<float>::quiet_NaN());
    const std::vector<GpuParticle> data = Render(ps);
    ASSERT_EQ(data.size(), 1u);
    EXPECT_FLOAT_EQ(data[0].positionSize.x, 1.0f);
    EXPECT_FLOAT_EQ(data[0].positionSize.y, 2.0f);
    EXPECT_FLOAT_EQ(data[0].positionSize.z, 3.0f);
}

TEST(Effects, PresetsHaveFinitePositiveValues) {
    const glm::vec3 color(0.5f, 0.4f, 0.3f);
    ExpectSanePreset(effects::Debris(color), "Debris");
    ExpectSanePreset(effects::Debris(color, 0.25f), "Debris 0.25");
    ExpectSanePreset(effects::Dust(color), "Dust");
    ExpectSanePreset(effects::Smoke(), "Smoke");
    ExpectSanePreset(effects::FireBurst(), "FireBurst");
    ExpectSanePreset(effects::Sparks(), "Sparks");
    ExpectSanePreset(effects::Flame(), "Flame");
    ExpectSanePreset(effects::Magic(glm::vec3(0.3f, 0.5f, 1.0f)), "Magic");
    ExpectSanePreset(effects::Precipitation(false), "Rain");
    ExpectSanePreset(effects::Precipitation(true), "Snow");
}

TEST(Effects, PresetsCarryTheDocumentedCharacter) {
    EXPECT_NE(effects::FireBurst().particle.flags & ParticleFlag_Additive, 0u);
    EXPECT_NE(effects::Sparks().particle.flags & ParticleFlag_Additive, 0u);
    EXPECT_NE(effects::Sparks().particle.flags & ParticleFlag_Collide, 0u);
    EXPECT_NE(effects::Flame().particle.flags & ParticleFlag_Additive, 0u);
    EXPECT_NE(effects::Magic(glm::vec3(1.0f)).particle.flags & ParticleFlag_Additive, 0u);
    EXPECT_EQ(effects::Smoke().particle.flags & ParticleFlag_Additive, 0u);
    EXPECT_NE(effects::Smoke().particle.flags & ParticleFlag_SoftDisc, 0u);
    EXPECT_LT(effects::Smoke().particle.gravity, 0.0f);                       // rises
    EXPECT_EQ(effects::Debris(glm::vec3(1.0f)).particle.flags & ParticleFlag_SoftDisc, 0u);   // crisp cubes
    EXPECT_NE(effects::Debris(glm::vec3(1.0f)).particle.flags & ParticleFlag_Collide, 0u);
    EXPECT_LE(effects::Debris(glm::vec3(1.0f), 0.1f).particle.sizeStart, 0.1f);
    EXPECT_GT(effects::Flame().rate, 0.0f);
    for (bool snow : {false, true}) {
        const EmitterDesc p = effects::Precipitation(snow);
        EXPECT_GT(p.rate, 0.0f);
        EXPECT_LT(p.direction.y, 0.0f);
        EXPECT_NE(p.particle.flags & ParticleFlag_DieOnCollide, 0u);
        EXPECT_NE(p.particle.flags & ParticleFlag_Collide, 0u);
        EXPECT_GT(p.particle.gravity, 0.0f);
    }
}

TEST(Effects, ExplosionScalesWithRadiusButIsBounded) {
    ParticleSystem small(100000), medium(100000), huge(100000);
    SpawnExplosion(small, kZero, 0.5f);
    SpawnExplosion(medium, kZero, 3.0f);
    SpawnExplosion(huge, kZero, 1.0e6f);

    EXPECT_GT(small.AliveCount(), 0u);
    EXPECT_GT(medium.AliveCount(), small.AliveCount());
    EXPECT_GE(huge.AliveCount(), medium.AliveCount());
    EXPECT_LE(huge.AliveCount(), 1000u);

    for (ParticleSystem* ps : {&small, &medium, &huge}) {
        for (int i = 0; i < 30; ++i) ps->Update(1.0f / 60.0f, BelowFloor);
        EXPECT_TRUE(AllFinite(Render(*ps)));
    }

    // Degenerate radii spawn nothing and do not crash.
    ParticleSystem none(1000);
    SpawnExplosion(none, kZero, 0.0f);
    SpawnExplosion(none, kZero, -3.0f);
    SpawnExplosion(none, kZero, std::numeric_limits<float>::quiet_NaN());
    SpawnExplosion(none, kZero, std::numeric_limits<float>::infinity());
    EXPECT_EQ(none.AliveCount(), 0u);

    // A pool that is too small takes what fits.
    ParticleSystem tiny(10);
    SpawnExplosion(tiny, kZero, 2.0f);
    EXPECT_EQ(tiny.AliveCount(), 10u);
}

TEST(Effects, VoxelBreakSpawnsABoundedPuffAtTheVoxel) {
    ParticleSystem ps(10000);
    const glm::dvec3 center(100.05, 20.05, -30.05);
    SpawnVoxelBreak(ps, center, glm::vec3(0.5f, 0.4f, 0.3f), 0.1f);
    const uint32_t count = ps.AliveCount();
    EXPECT_GT(count, 0u);
    EXPECT_LE(count, 32u);

    for (const GpuParticle& g : Render(ps, center)) {
        EXPECT_LT(glm::length(glm::vec3(g.positionSize)), 0.5f);   // born at the voxel
        EXPECT_GT(g.positionSize.w, 0.0f);
    }

    // Odd sizes and impulses stay bounded and finite.
    ParticleSystem odd(10000);
    SpawnVoxelBreak(odd, center, glm::vec3(1.0f), 1000.0f, glm::vec3(1.0e6f, 0.0f, 0.0f));
    SpawnVoxelBreak(odd, center, glm::vec3(1.0f), 0.0f, glm::vec3(0.0f, -5.0f, 0.0f));
    SpawnVoxelBreak(odd, center, glm::vec3(1.0f), std::numeric_limits<float>::quiet_NaN());
    EXPECT_GT(odd.AliveCount(), 0u);
    EXPECT_LE(odd.AliveCount(), 3u * 32u);
    for (int i = 0; i < 30; ++i) odd.Update(1.0f / 60.0f);
    EXPECT_TRUE(AllFinite(Render(odd, center)));
}

TEST(Effects, ImpulseThrowsDebrisAlongIt) {
    ParticleSystem ps(1000, 21);
    SpawnVoxelBreak(ps, kZero, glm::vec3(0.5f), 0.1f, glm::vec3(6.0f, 0.0f, 0.0f));
    for (int i = 0; i < 12; ++i) ps.Update(1.0f / 60.0f);

    // Debris is the crisp (square) part of the effect.
    float sumX = 0.0f;
    uint32_t debris = 0;
    for (const GpuParticle& g : Render(ps)) {
        if (g.params.y != 0.0f) continue;
        sumX += g.positionSize.x;
        ++debris;
    }
    ASSERT_GT(debris, 0u);
    EXPECT_GT(sumX / static_cast<float>(debris), 0.2f);
}
