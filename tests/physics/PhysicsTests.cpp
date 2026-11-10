// Rigid bodies, shapes, queries, contact events and characters of physics::PhysicsWorld.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.physics;

using namespace vortex;
using namespace vortex::physics;

namespace {
    constexpr float kStep = 1.0f / 60.0f;
    constexpr float kGroundTop = 0.5f;       // the fixture's ground slab is 1 m thick, centred on the origin

    PhysicsConfig SmallConfig() {
        PhysicsConfig config;
        config.maxBodies = 1024;
        config.maxBodyPairs = 4096;
        config.maxContactConstraints = 4096;
        config.threads = 1;
        return config;
    }

    struct Fixture {
        explicit Fixture(const PhysicsConfig& config = SmallConfig()) { initialized = physics.Initialize(config); }

        void Step(int steps) {
            for (int i = 0; i < steps; ++i) physics.Step(kStep);
        }

        /// Static slab whose top face is at y = kGroundTop.
        BodyId AddGround(uint64_t userData = 0) {
            BodyDesc desc;
            desc.shape = physics.CreateBoxShape({20.0f, 0.5f, 20.0f});
            desc.motion = MotionType::Static;
            desc.layer = Layer::Static;
            desc.userData = userData;
            return physics.CreateBody(desc);
        }

        BodyId AddBox(const glm::vec3& position, float halfExtent = 0.5f, Layer layer = Layer::Dynamic) {
            BodyDesc desc;
            desc.shape = physics.CreateBoxShape(glm::vec3(halfExtent));
            desc.position = position;
            desc.layer = layer;
            return physics.CreateBody(desc);
        }

        PhysicsWorld physics;
        bool initialized = false;
    };

    void ExpectVecNear(const glm::vec3& actual, const glm::vec3& expected, float tolerance) {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }
}

TEST(PhysicsWorld, InitializeAndShutdown) {
    PhysicsWorld physics;
    EXPECT_FALSE(physics.IsInitialized());
    // Nothing works before Initialize, and nothing crashes either.
    EXPECT_EQ(physics.CreateBoxShape(glm::vec3(0.5f)), nullptr);
    EXPECT_FALSE(physics.CreateBody(BodyDesc{}).IsValid());
    EXPECT_FALSE(physics.CreateCharacter(CharacterDesc{}).IsValid());
    physics.Step(kStep);
    EXPECT_EQ(physics.BodyCount(), 0u);
    EXPECT_FALSE(physics.Raycast({0, 5, 0}, {0, -1, 0}, 10.0f).hit);

    PhysicsConfig config = SmallConfig();
    config.gravity = {0.0f, -3.0f, 0.0f};
    ASSERT_TRUE(physics.Initialize(config));
    EXPECT_TRUE(physics.IsInitialized());
    ExpectVecNear(physics.Gravity(), config.gravity, 1e-6f);

    physics.Shutdown();
    EXPECT_FALSE(physics.IsInitialized());
    physics.Shutdown();     // twice is fine

    ASSERT_TRUE(physics.Initialize(SmallConfig()));     // and it can come back
    EXPECT_EQ(physics.BodyCount(), 0u);
}

TEST(PhysicsWorld, WorldsAndShapesShareTheRuntime) {
    ShapeRef survivor;
    {
        Fixture a;
        Fixture b;
        ASSERT_TRUE(a.initialized);
        ASSERT_TRUE(b.initialized);
        survivor = a.physics.CreateBoxShape(glm::vec3(0.5f));
        ASSERT_NE(survivor, nullptr);

        a.physics.Shutdown();
        // The second world is unaffected by the first one going away ...
        b.AddGround();
        const BodyId box = b.AddBox({0.0f, 2.0f, 0.0f});
        b.Step(120);
        EXPECT_NEAR(b.physics.GetState(box).position.y, kGroundTop + 0.5f, 0.05f);

        // ... and can use a shape made by it.
        BodyDesc desc;
        desc.shape = survivor;
        desc.position = {5.0f, 3.0f, 0.0f};
        EXPECT_TRUE(b.physics.CreateBody(desc).IsValid());
    }
    survivor.reset();       // released after every world is gone

    Fixture again;
    ASSERT_TRUE(again.initialized);
    EXPECT_TRUE(again.AddBox({0.0f, 1.0f, 0.0f}).IsValid());
}

TEST(PhysicsWorld, BoxComesToRestOnStaticBox) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    const BodyId ground = f.AddGround();
    const BodyId box = f.AddBox({0.3f, 3.0f, -0.2f});
    ASSERT_TRUE(ground.IsValid());
    ASSERT_TRUE(box.IsValid());
    EXPECT_NE(ground, box);
    EXPECT_EQ(f.physics.BodyCount(), 2u);
    EXPECT_TRUE(f.physics.GetState(box).active);

    f.Step(10);
    EXPECT_LT(f.physics.GetState(box).position.y, 3.0f);          // it is falling
    EXPECT_LT(f.physics.GetState(box).linearVelocity.y, -0.5f);

    f.Step(290);
    const BodyState state = f.physics.GetState(box);
    EXPECT_NEAR(state.position.y, kGroundTop + 0.5f, 0.03f);
    EXPECT_NEAR(state.position.x, 0.3f, 0.02f);
    EXPECT_NEAR(state.position.z, -0.2f, 0.02f);
    EXPECT_LT(glm::length(state.linearVelocity), 0.05f);
    EXPECT_FALSE(state.active);                                   // asleep by now
    EXPECT_EQ(f.physics.ActiveBodyCount(), 0u);

    // The static body never moved.
    ExpectVecNear(f.physics.GetState(ground).position, glm::vec3(0.0f), 1e-6f);

    const math::AABB bounds = f.physics.GetBounds(box);
    ExpectVecNear(bounds.Center(), state.position, 0.02f);
    ExpectVecNear(bounds.Extent(), glm::vec3(1.0f), 0.05f);
}

TEST(PhysicsWorld, CompoundMassIsDensityTimesVolume) {
    Fixture f;
    ASSERT_TRUE(f.initialized);

    EXPECT_EQ(f.physics.CreateCompoundShape({}), nullptr);

    const std::array<BoxDesc, 3> boxes = {
        BoxDesc{{0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1000.0f},      // 1 m^3    -> 1000 kg
        BoxDesc{{1.5f, 0.0f, 0.0f}, {0.25f, 0.5f, 1.0f}, 2000.0f},     // 1 m^3    -> 2000 kg
        BoxDesc{{0.0f, 0.55f, 0.0f}, {0.05f, 0.05f, 0.05f}, 500.0f},   // 0.001 m^3 -> 0.5 kg (a single small voxel)
    };
    BodyDesc desc;
    desc.shape = f.physics.CreateCompoundShape(boxes);
    ASSERT_NE(desc.shape, nullptr);
    desc.position = {0.0f, 5.0f, 0.0f};
    const BodyId body = f.physics.CreateBody(desc);
    ASSERT_TRUE(body.IsValid());
    EXPECT_NEAR(f.physics.GetMass(body), 3000.5f, 0.5f);

    // A compound of one box is that box.
    const std::array<BoxDesc, 1> single = {BoxDesc{{0.3f, 0.2f, 0.1f}, {0.1f, 0.2f, 0.3f}, 800.0f}};
    desc.shape = f.physics.CreateCompoundShape(single);
    ASSERT_NE(desc.shape, nullptr);
    desc.position = {10.0f, 5.0f, 0.0f};
    const BodyId one = f.physics.CreateBody(desc);
    ASSERT_TRUE(one.IsValid());
    EXPECT_NEAR(f.physics.GetMass(one), 800.0f * 0.2f * 0.4f * 0.6f, 0.01f);

    // Plain shapes.
    desc.shape = f.physics.CreateBoxShape({0.5f, 1.0f, 1.5f}, 700.0f);
    desc.position = {20.0f, 5.0f, 0.0f};
    EXPECT_NEAR(f.physics.GetMass(f.physics.CreateBody(desc)), 700.0f * 1.0f * 2.0f * 3.0f, 0.5f);
    desc.shape = f.physics.CreateSphereShape(0.5f, 1000.0f);
    desc.position = {30.0f, 5.0f, 0.0f};
    EXPECT_NEAR(f.physics.GetMass(f.physics.CreateBody(desc)), 1000.0f * 4.0f / 3.0f * 3.14159265f * 0.125f, 0.5f);

    // Boxes without a volume are ignored; nothing usable gives no shape.
    const std::array<BoxDesc, 1> flat = {BoxDesc{{0.0f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.5f}, 1000.0f}};
    EXPECT_EQ(f.physics.CreateCompoundShape(flat), nullptr);
    EXPECT_EQ(f.physics.CreateBoxShape({0.5f, -1.0f, 0.5f}), nullptr);
    EXPECT_EQ(f.physics.CreateSphereShape(0.0f), nullptr);
}

TEST(PhysicsWorld, BodyPositionIsTheShapeOrigin) {
    Fixture f;
    ASSERT_TRUE(f.initialized);

    // Centre of mass far from the shape origin.
    const std::array<BoxDesc, 2> offCentre = {
        BoxDesc{{2.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1000.0f},
        BoxDesc{{3.0f, 0.5f, 0.0f}, {0.5f, 0.25f, 0.5f}, 3000.0f},
    };
    BodyDesc desc;
    desc.shape = f.physics.CreateCompoundShape(offCentre);
    ASSERT_NE(desc.shape, nullptr);
    desc.position = {5.0f, 7.0f, -3.0f};
    desc.gravityFactor = 0.0f;
    const BodyId floating = f.physics.CreateBody(desc);
    ASSERT_TRUE(floating.IsValid());
    ExpectVecNear(f.physics.GetState(floating).position, desc.position, 1e-4f);
    f.Step(30);
    ExpectVecNear(f.physics.GetState(floating).position, desc.position, 1e-4f);

    // The geometry is where the description says: boxes at origin + (2..3.5, -0.5..0.75, -0.5..0.5).
    math::AABB bounds = f.physics.GetBounds(floating);
    ExpectVecNear(bounds.min, desc.position + glm::vec3(1.5f, -0.5f, -0.5f), 0.02f);
    ExpectVecNear(bounds.max, desc.position + glm::vec3(3.5f, 0.75f, 0.5f), 0.02f);

    // Rotated by 90 degrees about +Y, local +X points to world -Z; the origin stays put.
    const glm::quat quarterTurn = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 moved{-4.0f, 2.0f, 6.0f};
    f.physics.SetTransform(floating, moved, quarterTurn);
    BodyState state = f.physics.GetState(floating);
    ExpectVecNear(state.position, moved, 1e-4f);
    EXPECT_NEAR(std::abs(glm::dot(state.rotation, quarterTurn)), 1.0f, 1e-4f);
    bounds = f.physics.GetBounds(floating);
    ExpectVecNear(bounds.min, moved + glm::vec3(-0.5f, -0.5f, -3.5f), 0.02f);
    ExpectVecNear(bounds.max, moved + glm::vec3(0.5f, 0.75f, -1.5f), 0.02f);

    // A body whose origin is at its base, created standing on the ground, reports that position at rest.
    f.AddGround();
    const std::array<BoxDesc, 2> standing = {
        BoxDesc{{0.0f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.5f}, 1000.0f},
        BoxDesc{{1.0f, 0.25f, 0.0f}, {0.5f, 0.25f, 0.5f}, 1000.0f},
    };
    BodyDesc resting;
    resting.shape = f.physics.CreateCompoundShape(standing);
    ASSERT_NE(resting.shape, nullptr);
    resting.position = {8.0f, kGroundTop, 2.0f};
    const BodyId onGround = f.physics.CreateBody(resting);
    ASSERT_TRUE(onGround.IsValid());
    f.Step(180);
    state = f.physics.GetState(onGround);
    ExpectVecNear(state.position, resting.position, 0.03f);
    EXPECT_NEAR(std::abs(state.rotation.w), 1.0f, 1e-3f);
}

TEST(PhysicsWorld, StaleAndInvalidIdsAreHarmless) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.AddGround();
    const BodyId body = f.AddBox({0.0f, 2.0f, 0.0f});
    ASSERT_TRUE(f.physics.IsValid(body));
    const uint32_t countBefore = f.physics.BodyCount();

    f.physics.DestroyBody(body);
    EXPECT_FALSE(f.physics.IsValid(body));
    EXPECT_EQ(f.physics.BodyCount(), countBefore - 1u);

    // The slot is reused by the next body, but the old handle does not alias it.
    const BodyId replacement = f.AddBox({3.0f, 2.0f, 0.0f});
    ASSERT_TRUE(f.physics.IsValid(replacement));
    EXPECT_NE(replacement, body);
    EXPECT_FALSE(f.physics.IsValid(body));

    const ShapeRef shape = f.physics.CreateBoxShape(glm::vec3(0.25f));
    const BodyId bad[] = {body, BodyId{}, BodyId{123456u}, BodyId{0x80000001u}, BodyId{0x7FFFFFFFu}};
    for (const BodyId id : bad) {
        EXPECT_FALSE(f.physics.IsValid(id));
        f.physics.DestroyBody(id);
        f.physics.SetTransform(id, {1.0f, 2.0f, 3.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        f.physics.SetVelocity(id, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
        f.physics.AddImpulse(id, {10.0f, 0.0f, 0.0f});
        f.physics.AddImpulse(id, {10.0f, 0.0f, 0.0f}, glm::vec3(0.0f, 1.0f, 0.0f));
        f.physics.SetMotionType(id, MotionType::Kinematic);
        f.physics.SetShape(id, shape);
        f.physics.Activate(id);

        const BodyState state = f.physics.GetState(id);
        ExpectVecNear(state.position, glm::vec3(0.0f), 0.0f);
        ExpectVecNear(state.linearVelocity, glm::vec3(0.0f), 0.0f);
        EXPECT_FALSE(state.active);
        EXPECT_EQ(f.physics.GetMass(id), 0.0f);
        EXPECT_EQ(f.physics.GetUserData(id), 0u);
        ExpectVecNear(f.physics.GetPointVelocity(id, {1.0f, 1.0f, 1.0f}), glm::vec3(0.0f), 0.0f);
        EXPECT_TRUE(f.physics.GetBounds(id).IsEmpty());
    }

    // Nothing above touched the live bodies.
    EXPECT_EQ(f.physics.BodyCount(), countBefore);
    ExpectVecNear(f.physics.GetState(replacement).position, {3.0f, 2.0f, 0.0f}, 1e-5f);
    f.Step(120);
    EXPECT_NEAR(f.physics.GetState(replacement).position.y, kGroundTop + 0.5f, 0.05f);

    // Bad input to live bodies is rejected, not simulated.
    const float nan = std::nanf("");
    f.physics.SetTransform(replacement, {nan, 0.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    f.physics.SetVelocity(replacement, {nan, 0.0f, 0.0f}, glm::vec3(0.0f));
    f.physics.SetShape(replacement, nullptr);
    f.Step(5);
    EXPECT_NEAR(f.physics.GetState(replacement).position.x, 3.0f, 0.02f);

    BodyDesc noShape;
    EXPECT_FALSE(f.physics.CreateBody(noShape).IsValid());
}

TEST(PhysicsWorld, BodyBudgetRunsOutGracefully) {
    PhysicsConfig config = SmallConfig();
    config.maxBodies = 4;
    Fixture f(config);
    ASSERT_TRUE(f.initialized);
    int created = 0;
    for (int i = 0; i < 10; ++i) {
        if (f.AddBox({static_cast<float>(i) * 3.0f, 0.0f, 0.0f}).IsValid()) ++created;
    }
    EXPECT_EQ(created, 4);
    EXPECT_EQ(f.physics.BodyCount(), 4u);
    f.Step(5);
}

TEST(PhysicsWorld, RaycastReturnsClosestHitWithUserDataAndNormal) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    const BodyId ground = f.AddGround(42);

    BodyDesc wall;
    wall.shape = f.physics.CreateBoxShape({0.5f, 2.0f, 2.0f});
    wall.position = {6.0f, 3.0f, 0.0f};
    wall.motion = MotionType::Static;
    wall.layer = Layer::Static;
    wall.userData = 7;
    const BodyId wallBody = f.physics.CreateBody(wall);

    // A sensor floating above the ground must not stop rays.
    BodyDesc sensor;
    sensor.shape = f.physics.CreateBoxShape({1.0f, 0.25f, 1.0f});
    sensor.position = {0.0f, 4.0f, 0.0f};
    sensor.motion = MotionType::Static;
    sensor.sensor = true;
    sensor.userData = 99;
    ASSERT_TRUE(f.physics.CreateBody(sensor).IsValid());

    RayHit hit = f.physics.Raycast({0.2f, 10.0f, -0.3f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, ground);
    EXPECT_EQ(hit.userData, 42u);
    EXPECT_NEAR(hit.distance, 10.0f - kGroundTop, 1e-3f);
    ExpectVecNear(hit.position, {0.2f, kGroundTop, -0.3f}, 1e-3f);
    ExpectVecNear(hit.normal, {0.0f, 1.0f, 0.0f}, 1e-3f);

    // The direction does not have to be normalised to get distances in metres.
    hit = f.physics.Raycast({0.0f, 3.0f, 0.5f}, {4.0f, 0.0f, 0.0f}, 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, wallBody);
    EXPECT_EQ(hit.userData, 7u);
    EXPECT_NEAR(hit.distance, 5.5f, 1e-3f);
    ExpectVecNear(hit.normal, {-1.0f, 0.0f, 0.0f}, 1e-3f);

    // Closest of several: the wall is in front of nothing else, the ground is behind it from above.
    hit = f.physics.Raycast({6.0f, 20.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, wallBody);
    EXPECT_NEAR(hit.distance, 15.0f, 1e-3f);

    // Too short, pointing away, or degenerate.
    EXPECT_FALSE(f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 9.0f).hit);
    EXPECT_FALSE(f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 100.0f).hit);
    EXPECT_FALSE(f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f).hit);
    EXPECT_FALSE(f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 0.0f).hit);
}

TEST(PhysicsWorld, MeshShapesAreStaticOnly) {
    Fixture f;
    ASSERT_TRUE(f.initialized);

    // A 10 x 10 m quad at y = 0 facing up (counter-clockwise seen from above).
    const std::array<glm::vec3, 4> vertices = {
        glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(10.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, 10.0f)};
    const std::array<uint32_t, 6> indices = {0, 2, 1, 0, 3, 2};
    const ShapeRef mesh = f.physics.CreateMeshShape(vertices, indices);
    ASSERT_NE(mesh, nullptr);

    BodyDesc desc;
    desc.shape = mesh;
    desc.position = {100.0f, 2.0f, 100.0f};     // body position = mesh origin
    desc.layer = Layer::Static;
    desc.motion = MotionType::Dynamic;
    EXPECT_FALSE(f.physics.CreateBody(desc).IsValid());
    desc.motion = MotionType::Kinematic;
    EXPECT_FALSE(f.physics.CreateBody(desc).IsValid());
    desc.motion = MotionType::Static;
    const BodyId floor = f.physics.CreateBody(desc);
    ASSERT_TRUE(floor.IsValid());
    EXPECT_EQ(f.physics.GetMass(floor), 0.0f);

    f.physics.SetMotionType(floor, MotionType::Dynamic);       // refused
    f.Step(10);
    ExpectVecNear(f.physics.GetState(floor).position, desc.position, 1e-5f);

    const RayHit hit = f.physics.Raycast({105.0f, 10.0f, 105.0f}, {0.0f, -1.0f, 0.0f}, 50.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, floor);
    EXPECT_NEAR(hit.position.y, 2.0f, 1e-3f);
    ExpectVecNear(hit.normal, {0.0f, 1.0f, 0.0f}, 1e-3f);

    const BodyId box = f.AddBox({105.0f, 4.0f, 105.0f});
    f.physics.SetShape(box, mesh);                              // refused: the box is dynamic
    f.Step(180);
    EXPECT_NEAR(f.physics.GetState(box).position.y, 2.5f, 0.05f);

    // Unusable meshes.
    EXPECT_EQ(f.physics.CreateMeshShape({}, {}), nullptr);
    EXPECT_EQ(f.physics.CreateMeshShape(vertices, {}), nullptr);
    const std::array<uint32_t, 3> outOfRange = {0, 1, 9};
    EXPECT_EQ(f.physics.CreateMeshShape(vertices, outOfRange), nullptr);
    const std::array<uint32_t, 4> notTriangles = {0, 1, 2, 3};
    EXPECT_EQ(f.physics.CreateMeshShape(vertices, notTriangles), nullptr);
    const std::array<uint32_t, 3> degenerate = {0, 1, 1};
    EXPECT_EQ(f.physics.CreateMeshShape(vertices, degenerate), nullptr);
}

TEST(PhysicsWorld, DebrisIgnoresDebrisButRestsOnStatic) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.AddGround();

    // Two fragments in the same column: the upper one falls through the lower one.
    const BodyId lower = f.AddBox({0.0f, kGroundTop + 0.5f, 0.0f}, 0.5f, Layer::Debris);
    const BodyId upper = f.AddBox({0.0f, kGroundTop + 3.0f, 0.0f}, 0.5f, Layer::Debris);
    // An ordinary body does land on a fragment.
    const BodyId fragment = f.AddBox({6.0f, kGroundTop + 0.5f, 0.0f}, 0.5f, Layer::Debris);
    const BodyId crate = f.AddBox({6.0f, kGroundTop + 3.0f, 0.0f}, 0.5f, Layer::Dynamic);

    f.Step(240);
    EXPECT_NEAR(f.physics.GetState(lower).position.y, kGroundTop + 0.5f, 0.05f);
    EXPECT_NEAR(f.physics.GetState(upper).position.y, kGroundTop + 0.5f, 0.05f);
    EXPECT_NEAR(f.physics.GetState(upper).position.x, 0.0f, 0.05f);
    EXPECT_NEAR(f.physics.GetState(fragment).position.y, kGroundTop + 0.5f, 0.05f);
    EXPECT_NEAR(f.physics.GetState(crate).position.y, kGroundTop + 1.5f, 0.08f);
}

TEST(PhysicsWorld, SensorsDoNotBlock) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.AddGround();
    BodyDesc sensor;
    sensor.shape = f.physics.CreateBoxShape({2.0f, 0.5f, 2.0f});
    sensor.position = {0.0f, 2.5f, 0.0f};
    sensor.motion = MotionType::Static;
    sensor.layer = Layer::Dynamic;
    sensor.sensor = true;
    ASSERT_TRUE(f.physics.CreateBody(sensor).IsValid());

    const BodyId box = f.AddBox({0.0f, 5.0f, 0.0f});
    f.Step(240);
    EXPECT_NEAR(f.physics.GetState(box).position.y, kGroundTop + 0.5f, 0.05f);
}

TEST(PhysicsWorld, ContactEventsReportFastImpactsOnly) {
    {
        Fixture f;
        ASSERT_TRUE(f.initialized);
        const BodyId ground = f.AddGround(11);
        BodyDesc desc;
        desc.shape = f.physics.CreateBoxShape(glm::vec3(0.5f));
        desc.position = {0.0f, 3.0f, 0.0f};
        desc.linearVelocity = {0.0f, -8.0f, 0.0f};
        desc.userData = 22;
        const BodyId box = f.physics.CreateBody(desc);

        f.Step(60);
        const std::vector<ContactEvent> events = f.physics.TakeContactEvents();
        ASSERT_FALSE(events.empty());
        const ContactEvent& impact = events.front();
        // The pair is reported once, whichever body comes first.
        const bool groundFirst = impact.bodyA == ground;
        EXPECT_EQ(groundFirst ? impact.bodyA : impact.bodyB, ground);
        EXPECT_EQ(groundFirst ? impact.bodyB : impact.bodyA, box);
        EXPECT_EQ(groundFirst ? impact.userDataA : impact.userDataB, 11u);
        EXPECT_EQ(groundFirst ? impact.userDataB : impact.userDataA, 22u);
        EXPECT_GT(impact.approachSpeed, 7.0f);
        EXPECT_LT(impact.approachSpeed, 12.0f);
        // From A towards B: up if the ground is A, down if the box is A.
        EXPECT_NEAR(impact.normal.y, groundFirst ? 1.0f : -1.0f, 0.05f);
        EXPECT_NEAR(impact.position.y, kGroundTop, 0.2f);
        EXPECT_NEAR(impact.position.x, 0.0f, 0.6f);

        // Taking the events empties the list; a body at rest produces no new ones.
        EXPECT_TRUE(f.physics.TakeContactEvents().empty());
        f.Step(60);
        EXPECT_TRUE(f.physics.TakeContactEvents().empty());
    }
    {
        // Set down gently: the contact begins far below the reporting threshold (2 m/s).
        Fixture f;
        ASSERT_TRUE(f.initialized);
        f.AddGround();
        const BodyId box = f.AddBox({0.0f, kGroundTop + 0.5f + 0.02f, 0.0f});
        f.Step(120);
        EXPECT_NEAR(f.physics.GetState(box).position.y, kGroundTop + 0.5f, 0.03f);
        EXPECT_TRUE(f.physics.TakeContactEvents().empty());
    }
    {
        // The threshold is configurable.
        PhysicsConfig config = SmallConfig();
        config.contactEventMinSpeed = 0.05f;
        Fixture f(config);
        ASSERT_TRUE(f.initialized);
        f.AddGround();
        f.AddBox({0.0f, kGroundTop + 0.5f + 0.2f, 0.0f});       // lands at about 2 m/s
        f.Step(120);
        const std::vector<ContactEvent> events = f.physics.TakeContactEvents();
        ASSERT_FALSE(events.empty());
        EXPECT_GT(events.front().approachSpeed, 0.05f);
        EXPECT_LT(events.front().approachSpeed, 3.0f);
    }
}

TEST(PhysicsWorld, SetShapeKeepsPoseAndVelocity) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    BodyDesc desc;
    desc.shape = f.physics.CreateBoxShape(glm::vec3(0.5f), 1000.0f);
    desc.position = {1.0f, 10.0f, -2.0f};
    desc.rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f)));
    desc.linearVelocity = {1.0f, 2.0f, 3.0f};
    desc.gravityFactor = 0.0f;
    desc.linearDamping = 0.0f;
    desc.angularDamping = 0.0f;
    const BodyId body = f.physics.CreateBody(desc);
    ASSERT_TRUE(body.IsValid());
    EXPECT_NEAR(f.physics.GetMass(body), 1000.0f, 0.5f);

    // The replacement has its centre of mass away from the origin and a different mass.
    const std::array<BoxDesc, 2> pieces = {
        BoxDesc{{0.0f, 0.0f, 0.0f}, {0.25f, 0.25f, 0.25f}, 1000.0f},
        BoxDesc{{0.5f, 0.0f, 0.0f}, {0.25f, 0.25f, 0.25f}, 1000.0f},
    };
    f.physics.SetShape(body, f.physics.CreateCompoundShape(pieces));

    const BodyState state = f.physics.GetState(body);
    ExpectVecNear(state.position, desc.position, 1e-4f);
    EXPECT_NEAR(std::abs(glm::dot(state.rotation, desc.rotation)), 1.0f, 1e-4f);
    ExpectVecNear(state.linearVelocity, desc.linearVelocity, 1e-4f);
    ExpectVecNear(state.angularVelocity, glm::vec3(0.0f), 1e-4f);
    EXPECT_NEAR(f.physics.GetMass(body), 250.0f, 0.5f);

    // It keeps flying exactly as before.
    f.Step(60);
    ExpectVecNear(f.physics.GetState(body).position, desc.position + desc.linearVelocity, 0.01f);
}

TEST(PhysicsWorld, VelocityImpulseAndPointVelocity) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    BodyDesc desc;
    desc.shape = f.physics.CreateBoxShape(glm::vec3(0.5f), 1000.0f);     // 1000 kg
    desc.position = {0.0f, 10.0f, 0.0f};
    desc.gravityFactor = 0.0f;
    desc.linearDamping = 0.0f;
    desc.angularDamping = 0.0f;
    desc.userData = 0xABCDEF0123ull;
    const BodyId body = f.physics.CreateBody(desc);
    EXPECT_EQ(f.physics.GetUserData(body), 0xABCDEF0123ull);

    f.physics.AddImpulse(body, {2000.0f, 0.0f, 0.0f});
    ExpectVecNear(f.physics.GetState(body).linearVelocity, {2.0f, 0.0f, 0.0f}, 1e-4f);

    f.physics.SetVelocity(body, {0.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f});     // spinning about +Y
    ExpectVecNear(f.physics.GetState(body).angularVelocity, {0.0f, 2.0f, 0.0f}, 1e-4f);
    // v = w x r: a point 1 m along +X moves towards -Z.
    ExpectVecNear(f.physics.GetPointVelocity(body, desc.position + glm::vec3(1.0f, 0.0f, 0.0f)), {0.0f, 0.0f, -2.0f}, 1e-3f);

    // An off-centre impulse also spins the body.
    f.physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
    f.physics.AddImpulse(body, {0.0f, 0.0f, 500.0f}, desc.position + glm::vec3(0.5f, 0.0f, 0.0f));
    const BodyState state = f.physics.GetState(body);
    EXPECT_NEAR(state.linearVelocity.z, 0.5f, 1e-3f);
    EXPECT_LT(state.angularVelocity.y, -0.1f);

    // Static bodies ignore all of it.
    const BodyId ground = f.AddGround();
    f.physics.AddImpulse(ground, {1000.0f, 0.0f, 0.0f});
    f.physics.SetVelocity(ground, {1.0f, 0.0f, 0.0f}, glm::vec3(0.0f));
    f.physics.Activate(ground);
    f.Step(10);
    ExpectVecNear(f.physics.GetState(ground).position, glm::vec3(0.0f), 1e-6f);
    EXPECT_FALSE(f.physics.GetState(ground).active);
}

TEST(PhysicsWorld, MotionTypeCanChange) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.AddGround();

    BodyDesc desc;
    desc.shape = f.physics.CreateBoxShape(glm::vec3(0.5f));
    desc.position = {0.0f, 4.0f, 0.0f};
    desc.motion = MotionType::Static;
    desc.layer = Layer::Static;
    const BodyId body = f.physics.CreateBody(desc);
    ASSERT_TRUE(body.IsValid());
    EXPECT_NEAR(f.physics.GetMass(body), 1000.0f, 0.5f);       // what it will weigh once it moves

    f.Step(30);
    ExpectVecNear(f.physics.GetState(body).position, desc.position, 1e-6f);

    // Released: it falls and lands on the static ground.
    f.physics.SetMotionType(body, MotionType::Dynamic);
    EXPECT_TRUE(f.physics.GetState(body).active);
    f.Step(240);
    EXPECT_NEAR(f.physics.GetState(body).position.y, kGroundTop + 0.5f, 0.05f);
    EXPECT_NEAR(f.physics.GetMass(body), 1000.0f, 0.5f);

    // Frozen in place again.
    f.physics.SetTransform(body, {0.0f, 6.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    f.physics.SetMotionType(body, MotionType::Static);
    f.Step(30);
    ExpectVecNear(f.physics.GetState(body).position, {0.0f, 6.0f, 0.0f}, 1e-5f);
    EXPECT_FALSE(f.physics.GetState(body).active);

    // Kinematic bodies move with their velocity and ignore gravity.
    f.physics.SetMotionType(body, MotionType::Kinematic);
    f.physics.SetVelocity(body, {1.0f, 0.0f, 0.0f}, glm::vec3(0.0f));
    f.Step(60);
    ExpectVecNear(f.physics.GetState(body).position, {1.0f, 6.0f, 0.0f}, 0.02f);
}

TEST(PhysicsWorld, SleepingBodiesWakeUpInBox) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.AddGround();
    const BodyId near = f.AddBox({0.0f, kGroundTop + 0.5f, 0.0f});
    const BodyId far = f.AddBox({10.0f, kGroundTop + 0.5f, 0.0f});
    BodyDesc restless;
    restless.shape = f.physics.CreateBoxShape(glm::vec3(0.5f));
    restless.position = {-10.0f, kGroundTop + 0.5f, 0.0f};
    restless.allowSleeping = false;
    const BodyId awake = f.physics.CreateBody(restless);

    f.Step(240);
    EXPECT_FALSE(f.physics.GetState(near).active);
    EXPECT_FALSE(f.physics.GetState(far).active);
    EXPECT_TRUE(f.physics.GetState(awake).active);            // never allowed to sleep
    std::vector<BodyId> active;
    f.physics.GetActiveBodies(active);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0], awake);
    EXPECT_EQ(f.physics.ActiveBodyCount(), 1u);

    f.physics.ActivateInBox(math::AABB({-1.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}));
    EXPECT_TRUE(f.physics.GetState(near).active);
    EXPECT_FALSE(f.physics.GetState(far).active);
    f.physics.GetActiveBodies(active);
    EXPECT_EQ(active.size(), 2u);

    f.physics.ActivateInBox(math::AABB());                    // an empty box wakes nothing
    EXPECT_FALSE(f.physics.GetState(far).active);
    f.physics.Activate(far);
    EXPECT_TRUE(f.physics.GetState(far).active);
}

TEST(PhysicsWorld, ContinuousBodiesDoNotTunnel) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    BodyDesc plate;
    plate.shape = f.physics.CreateBoxShape({5.0f, 0.05f, 5.0f});
    plate.motion = MotionType::Static;
    plate.layer = Layer::Static;
    ASSERT_TRUE(f.physics.CreateBody(plate).IsValid());

    // 200 m/s: more than 3 m per step against a 10 cm plate.
    BodyDesc bullet;
    bullet.shape = f.physics.CreateSphereShape(0.05f, 8000.0f);
    bullet.position = {0.0f, 2.0f, 0.0f};
    bullet.linearVelocity = {0.0f, -200.0f, 0.0f};
    bullet.continuous = true;
    bullet.restitution = 0.0f;
    const BodyId body = f.physics.CreateBody(bullet);
    ASSERT_TRUE(body.IsValid());

    f.Step(60);
    EXPECT_GT(f.physics.GetState(body).position.y, 0.0f);
}

TEST(PhysicsWorld, CharacterStandsJumpsAndLands) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.AddGround();

    CharacterDesc desc;
    desc.position = {0.0f, kGroundTop + 0.3f, 0.0f};       // dropped from 30 cm
    const CharacterId character = f.physics.CreateCharacter(desc);
    ASSERT_TRUE(character.IsValid());
    EXPECT_FALSE(f.physics.GetCharacterState(character).onGround);

    for (int i = 0; i < 60; ++i) f.physics.UpdateCharacter(character, kStep, glm::vec3(0.0f));
    CharacterState state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_NEAR(state.position.y, kGroundTop, 0.03f);       // the feet are on the floor
    EXPECT_NEAR(state.velocity.y, 0.0f, 0.3f);              // the fall speed did not accumulate

    // The y component of the desired velocity is ignored.
    for (int i = 0; i < 60; ++i) f.physics.UpdateCharacter(character, kStep, {0.0f, 50.0f, 0.0f});
    state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_NEAR(state.position.y, kGroundTop, 0.03f);

    // Jump: up, airborne (where a second jump does nothing), and back down.
    f.physics.UpdateCharacter(character, kStep, glm::vec3(0.0f), 5.0f);
    float highest = 0.0f;
    bool leftGround = false;
    for (int i = 0; i < 20; ++i) {
        f.physics.UpdateCharacter(character, kStep, glm::vec3(0.0f), 5.0f);
        state = f.physics.GetCharacterState(character);
        highest = std::max(highest, state.position.y - kGroundTop);
        leftGround = leftGround || !state.onGround;
    }
    for (int i = 0; i < 100; ++i) {
        f.physics.UpdateCharacter(character, kStep, glm::vec3(0.0f));
        highest = std::max(highest, f.physics.GetCharacterState(character).position.y - kGroundTop);
    }
    EXPECT_TRUE(leftGround);
    // v^2 / 2g = 1.27 m for a single 5 m/s jump; a double jump would go far beyond that.
    EXPECT_GT(highest, 0.9f);
    EXPECT_LT(highest, 1.5f);
    state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_NEAR(state.position.y, kGroundTop, 0.03f);

    // Teleporting.
    f.physics.SetCharacterPosition(character, {3.0f, kGroundTop + 5.0f, 1.0f});
    state = f.physics.GetCharacterState(character);
    ExpectVecNear(state.position, {3.0f, kGroundTop + 5.0f, 1.0f}, 1e-4f);
    EXPECT_FALSE(state.onGround);

    // Destroyed and invalid characters are ignored.
    f.physics.DestroyCharacter(character);
    const CharacterId reused = f.physics.CreateCharacter(desc);
    ASSERT_TRUE(reused.IsValid());
    EXPECT_NE(reused.value, character.value);
    for (const CharacterId id : {character, CharacterId{}, CharacterId{123456u}}) {
        f.physics.UpdateCharacter(id, kStep, {1.0f, 0.0f, 0.0f}, 5.0f);
        f.physics.SetCharacterPosition(id, {9.0f, 9.0f, 9.0f});
        f.physics.DestroyCharacter(id);
        const CharacterState none = f.physics.GetCharacterState(id);
        ExpectVecNear(none.position, glm::vec3(0.0f), 0.0f);
        EXPECT_FALSE(none.onGround);
    }
    ExpectVecNear(f.physics.GetCharacterState(reused).position, desc.position, 1e-4f);
}

TEST(PhysicsWorld, CharacterIsBlockedByBodies) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.AddGround();
    BodyDesc wall;
    wall.shape = f.physics.CreateBoxShape({0.5f, 2.0f, 5.0f});
    wall.position = {4.0f, kGroundTop + 2.0f, 0.0f};         // its face is at x = 3.5
    wall.motion = MotionType::Static;
    wall.layer = Layer::Static;
    ASSERT_TRUE(f.physics.CreateBody(wall).IsValid());

    CharacterDesc desc;
    desc.position = {0.0f, kGroundTop, 0.0f};
    const CharacterId character = f.physics.CreateCharacter(desc);
    for (int i = 0; i < 180; ++i) f.physics.UpdateCharacter(character, kStep, {3.0f, 0.0f, 0.0f});

    const CharacterState state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_NEAR(state.position.x, 3.5f - desc.radius, 0.06f);    // stopped with its side against the wall
    EXPECT_NEAR(state.position.y, kGroundTop, 0.03f);
}

TEST(PhysicsWorld, CharacterIgnoresCharacterLayerBodies) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.AddGround();
    // A kinematic proxy in the Character layer (what a game attaches to a controller so that
    // debris and ray casts see it) must not block character controllers.
    BodyDesc proxy;
    proxy.shape = f.physics.CreateBoxShape({0.5f, 2.0f, 5.0f});
    proxy.position = {4.0f, kGroundTop + 2.0f, 0.0f};
    proxy.motion = MotionType::Kinematic;
    proxy.layer = Layer::Character;
    ASSERT_TRUE(f.physics.CreateBody(proxy).IsValid());

    CharacterDesc desc;
    desc.position = {0.0f, kGroundTop, 0.0f};
    const CharacterId character = f.physics.CreateCharacter(desc);
    for (int i = 0; i < 180; ++i) f.physics.UpdateCharacter(character, kStep, {3.0f, 0.0f, 0.0f});

    const CharacterState state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_GT(state.position.x, 7.0f);                           // walked straight through: 9 m in 3 s
    EXPECT_NEAR(state.position.y, kGroundTop, 0.03f);
}
