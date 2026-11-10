// Terrain collider streaming (physics::TerrainColliders) and characters walking on the result.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;

using namespace vortex;
using namespace vortex::voxel;
using namespace vortex::physics;

namespace {
    constexpr float kStep = 1.0f / 60.0f;
    constexpr uint8_t kSurface = 2;
    constexpr uint8_t kRock = 3;
    constexpr uint8_t kWater = 4;

    /// A flat world (voxels below `groundVoxels` are solid) with physics and terrain colliders.
    struct Fixture {
        explicit Fixture(int groundVoxels = 40, float voxelSize = 0.5f, uint8_t surfaceId = kSurface)
            : store(512), jobs(3), ground(static_cast<float>(groundVoxels) * voxelSize) {
            WorldConfig config;
            config.voxelSize = voxelSize;
            config.windowSize = {8, 8, 8};
            Material hard;
            hard.hardness = 10.0f;
            Material liquid;
            liquid.flags = MaterialFlag_Liquid;
            materials.Set(kWorldPalette, kSurface, Material{}, "surface");
            materials.Set(kWorldPalette, kRock, hard, "rock");
            materials.Set(kWorldPalette, kWater, liquid, "water");
            world = std::make_unique<World>(config, store, jobs, CreateFlatGenerator(groundVoxels, surfaceId, kRock), &materials);

            PhysicsConfig physicsConfig;
            physicsConfig.maxBodies = 2048;
            physicsConfig.maxBodyPairs = 4096;
            physicsConfig.maxContactConstraints = 4096;
            physicsConfig.threads = 1;
            initialized = physics.Initialize(physicsConfig);
            colliders = std::make_unique<TerrainColliders>(physics, *world, jobs, materials);
        }

        /// Loads the world around `focus` and builds every collider within `radius` of it.
        void Stream(const glm::dvec3& focus, float radius) {
            world->LoadBlocking(focus);
            Sync(focus, radius);
        }

        /// Brings the colliders up to date with the world (after edits).
        void Sync(const glm::dvec3& focus, float radius) {
            const std::array<glm::dvec3, 1> points = {focus};
            colliders->Update(points, radius);
            colliders->Flush();
        }

        void Step(int steps) {
            for (int i = 0; i < steps; ++i) physics.Step(kStep);
        }

        BodyId DropBox(const glm::vec3& position, float halfExtent = 0.5f) {
            BodyDesc desc;
            desc.shape = physics.CreateBoxShape(glm::vec3(halfExtent));
            desc.position = position;
            return physics.CreateBody(desc);
        }

        float RayDown(float x, float z, float fromY) const {
            const physics::RayHit hit = physics.Raycast({x, fromY, z}, {0.0f, -1.0f, 0.0f}, 200.0f);
            return hit.hit ? hit.position.y : -1000.0f;
        }

        // Destruction order matters: colliders before physics and the world, the world before
        // the store and the job system.
        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        std::unique_ptr<World> world;
        PhysicsWorld physics;
        std::unique_ptr<TerrainColliders> colliders;
        float ground;                 // world height of the surface, metres
        bool initialized = false;
    };
}

TEST(TerrainColliders, RayHitsFlatGroundAtWorldHeight) {
    Fixture f;                                    // ground at y = 20 m, chunks are 16 m
    ASSERT_TRUE(f.initialized);
    const glm::dvec3 focus(0.0, 21.0, 0.0);

    EXPECT_EQ(f.colliders->Stats().colliders, 0u);
    f.Stream(focus, 20.0f);

    const TerrainColliderStats stats = f.colliders->Stats();
    EXPECT_GT(stats.colliders, 0u);
    EXPECT_EQ(stats.pending, 0u);
    EXPECT_EQ(stats.built, stats.colliders);
    EXPECT_EQ(f.physics.BodyCount(), stats.colliders);

    // Straight down at several places, including across chunk borders (x = 0, z = -16 ...).
    const glm::vec2 probes[] = {{1.3f, -2.1f}, {0.0f, 0.0f}, {-7.9f, 5.2f}, {15.99f, 3.0f}, {16.01f, -16.0f}, {-12.5f, -15.5f}};
    for (const glm::vec2& p : probes) {
        const physics::RayHit hit = f.physics.Raycast({p.x, f.ground + 10.0f, p.y}, {0.0f, -1.0f, 0.0f}, 100.0f);
        ASSERT_TRUE(hit.hit) << "at " << p.x << ", " << p.y;
        EXPECT_NEAR(hit.distance, 10.0f, 2e-3f);
        EXPECT_NEAR(hit.position.y, f.ground, 2e-3f);
        EXPECT_NEAR(hit.normal.y, 1.0f, 1e-3f);
        EXPECT_FALSE(f.physics.GetState(hit.body).active);      // a static body
    }

    // The physics surface is the voxel surface.
    const WorldRayHit voxelHit = f.world->Raycast(glm::dvec3(1.3, f.ground + 10.0, -2.1), glm::vec3(0, -1, 0), 100.0f);
    ASSERT_TRUE(voxelHit.hit);
    EXPECT_NEAR(voxelHit.position.y, f.RayDown(1.3f, -2.1f, f.ground + 10.0f), 2e-3);

    // Only the surface layer has colliders: solid rock all around needs none, nor does air.
    // 20 m (+ a quarter chunk) around the focus reaches 3 x 3 columns of the one dense layer at most.
    EXPECT_LE(stats.colliders, 16u);

    // Updating again without changes builds nothing new.
    f.Sync(focus, 20.0f);
    EXPECT_EQ(f.colliders->Stats().built, stats.built);
    EXPECT_EQ(f.colliders->Stats().colliders, stats.colliders);
}

TEST(TerrainColliders, DynamicBoxRestsOnTerrain) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.Stream({0.0, 21.0, 0.0}, 20.0f);

    // One in the middle of a chunk, one straddling the corner where four chunks meet.
    const BodyId middle = f.DropBox({5.3f, f.ground + 2.0f, 6.1f});
    const BodyId corner = f.DropBox({0.1f, f.ground + 2.0f, -0.2f});
    f.Step(240);

    for (const BodyId body : {middle, corner}) {
        const BodyState state = f.physics.GetState(body);
        EXPECT_NEAR(state.position.y, f.ground + 0.5f, 0.05f);
        EXPECT_LT(glm::length(state.linearVelocity), 0.05f);
    }
    EXPECT_NEAR(f.physics.GetState(middle).position.x, 5.3f, 0.05f);
    EXPECT_NEAR(f.physics.GetState(corner).position.z, -0.2f, 0.05f);
}

TEST(TerrainColliders, SurfaceOnChunkBoundaryCollides) {
    // 64 voxels = exactly two chunks: the top face of the terrain lies on a chunk boundary and
    // the chunk above it is empty.
    {
        Fixture f(64);                             // surface layer differs from the rock: a full dense chunk
        ASSERT_TRUE(f.initialized);
        ASSERT_FLOAT_EQ(f.ground, 32.0f);
        f.Stream({0.0, 33.0, 0.0}, 20.0f);
        ASSERT_EQ(f.world->StateOf({0, 1, 0}), CellState::Dense);
        ASSERT_EQ(f.world->StateOf({0, 2, 0}), CellState::Empty);

        EXPECT_NEAR(f.RayDown(3.3f, 4.4f, 40.0f), 32.0f, 2e-3f);
        EXPECT_NEAR(f.RayDown(0.0f, 0.0f, 40.0f), 32.0f, 2e-3f);
        const BodyId box = f.DropBox({3.3f, 34.0f, 4.4f});
        f.Step(240);
        EXPECT_NEAR(f.physics.GetState(box).position.y, 32.5f, 0.05f);
    }
    {
        Fixture f(64, 0.5f, kRock);                // all rock: the chunk under the surface is uniform, without voxel storage
        ASSERT_TRUE(f.initialized);
        f.Stream({0.0, 33.0, 0.0}, 20.0f);
        ASSERT_EQ(f.world->StateOf({0, 1, 0}), CellState::Uniform);
        EXPECT_GT(f.colliders->Stats().colliders, 0u);

        EXPECT_NEAR(f.RayDown(3.3f, 4.4f, 40.0f), 32.0f, 2e-3f);
        EXPECT_NEAR(f.RayDown(-16.0f, 15.9f, 40.0f), 32.0f, 2e-3f);
        const BodyId box = f.DropBox({-3.0f, 34.0f, 7.0f});
        f.Step(240);
        EXPECT_NEAR(f.physics.GetState(box).position.y, 32.5f, 0.05f);
    }
}

TEST(TerrainColliders, CarvingUnderRestingBoxMakesItFall) {
    Fixture f;                                    // ground at voxel 40 (y = 20 m) inside chunk y = 1
    ASSERT_TRUE(f.initialized);
    const glm::dvec3 focus(0.0, 21.0, 0.0);
    f.Stream(focus, 20.0f);

    const glm::vec3 spot(4.25f, f.ground, 4.25f);             // voxel (8.5, 40, 8.5)
    const BodyId box = f.DropBox(spot + glm::vec3(0.0f, 1.0f, 0.0f));
    f.Step(240);
    ASSERT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);
    ASSERT_FALSE(f.physics.GetState(box).active);             // asleep: it has to be woken up
    const uint32_t builtBefore = f.colliders->Stats().built;

    // A 3 m sphere centred 1 m below the surface. It reaches down to voxel 32, the bottom of the
    // dense chunk, and so also uncovers the top of the uniform rock chunk below.
    const uint32_t removed = f.world->CarveSphere(glm::vec3(8.5f, 38.0f, 8.5f), 6.0f, 100.0f);
    ASSERT_GT(removed, 0u);
    ASSERT_EQ(f.world->GetVoxel({8, 32, 8}), 0);
    ASSERT_NE(f.world->GetVoxel({8, 31, 8}), 0);

    f.Sync(focus, 20.0f);
    EXPECT_GT(f.colliders->Stats().built, builtBefore);
    EXPECT_TRUE(f.physics.GetState(box).active);              // woken by the rebuilt collider
    f.Step(300);

    const BodyState state = f.physics.GetState(box);
    EXPECT_LT(state.position.y, f.ground - 1.0f);             // it fell into the hole ...
    EXPECT_GT(state.position.y, 16.0f + 0.4f);                // ... and was caught by its floor (voxel 32 = 16 m)
    EXPECT_NEAR(state.position.x, spot.x, 1.5f);

    // The collision surface follows the voxels: the bottom of the hole (with the box out of
    // the way of the ray), and untouched ground next to it.
    f.physics.DestroyBody(box);
    EXPECT_NEAR(f.RayDown(4.25f, 4.25f, 30.0f), 16.0f, 2e-3f);
    EXPECT_NEAR(f.RayDown(12.0f, 12.0f, 30.0f), f.ground, 2e-3f);

    // Filling the hole again closes the surface.
    f.world->FillBox({0, 32, 0}, {20, 40, 20}, kRock);
    f.Sync(focus, 20.0f);
    EXPECT_NEAR(f.RayDown(4.25f, 4.25f, 30.0f), f.ground, 2e-3f);
}

TEST(TerrainColliders, BuildingOnTheGroundAddsCollision) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    const glm::dvec3 focus(0.0, 21.0, 0.0);
    f.Stream(focus, 20.0f);

    // A pillar that grows from the dense surface chunk (voxels 40..63) into the empty chunk above (64..69).
    f.world->FillBox({4, 40, 4}, {8, 70, 8}, kRock);
    // Water is not solid for physics.
    f.world->FillBox({-10, 40, -10}, {-4, 44, -4}, kWater);
    f.Sync(focus, 20.0f);

    EXPECT_NEAR(f.RayDown(3.0f, 3.0f, 50.0f), 35.0f, 2e-3f);              // top of the pillar: voxel 70
    EXPECT_NEAR(f.RayDown(-3.5f, -3.5f, 50.0f), f.ground, 2e-3f);         // through the water to the ground

    // The pillar's side, hit horizontally above and below the chunk boundary at y = 32 m.
    for (const float y : {25.0f, 33.5f}) {
        const physics::RayHit side = f.physics.Raycast({10.0f, y, 3.0f}, {-1.0f, 0.0f, 0.0f}, 50.0f);
        ASSERT_TRUE(side.hit);
        EXPECT_NEAR(side.position.x, 4.0f, 2e-3f);
        EXPECT_NEAR(side.normal.x, 1.0f, 1e-3f);
    }
}

TEST(TerrainColliders, CollidersFollowTheFocus) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    const glm::dvec3 here(-40.0, 21.0, -40.0);
    const glm::dvec3 there(40.0, 21.0, 40.0);
    f.world->LoadBlocking({0.0, 21.0, 0.0});       // the window covers -64 .. 64 m

    f.Sync(here, 10.0f);
    const uint32_t first = f.colliders->Stats().colliders;
    EXPECT_GT(first, 0u);
    EXPECT_NEAR(f.RayDown(-40.0f, -40.0f, 30.0f), f.ground, 2e-3f);
    EXPECT_EQ(f.RayDown(40.0f, 40.0f, 30.0f), -1000.0f);          // nothing over there yet

    // Moving a little keeps what is there (hysteresis) ...
    f.Sync(here + glm::dvec3(1.0, 0.0, 1.0), 10.0f);
    EXPECT_GE(f.colliders->Stats().colliders, first);
    EXPECT_NEAR(f.RayDown(-40.0f, -40.0f, 30.0f), f.ground, 2e-3f);

    // ... moving far away drops it and builds around the new place.
    f.Sync(there, 10.0f);
    EXPECT_GT(f.colliders->Stats().colliders, 0u);
    EXPECT_EQ(f.RayDown(-40.0f, -40.0f, 30.0f), -1000.0f);
    EXPECT_NEAR(f.RayDown(40.0f, 40.0f, 30.0f), f.ground, 2e-3f);
    EXPECT_EQ(f.physics.BodyCount(), f.colliders->Stats().colliders);

    // Several focus points at once.
    const std::array<glm::dvec3, 2> both = {here, there};
    f.colliders->Update(both, 10.0f);
    f.colliders->Flush();
    EXPECT_NEAR(f.RayDown(-40.0f, -40.0f, 30.0f), f.ground, 2e-3f);
    EXPECT_NEAR(f.RayDown(40.0f, 40.0f, 30.0f), f.ground, 2e-3f);

    // No focus points: no colliders.
    f.colliders->Update({}, 10.0f);
    f.colliders->Flush();
    EXPECT_EQ(f.colliders->Stats().colliders, 0u);
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    EXPECT_GT(f.colliders->Stats().built, 0u);                     // the total keeps counting
}

TEST(TerrainColliders, CollidersGoAwayWithUnloadedChunks) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.Stream({0.0, 21.0, 0.0}, 20.0f);
    ASSERT_GT(f.colliders->Stats().colliders, 0u);

    // The world window moves 1 km away: every chunk that had a collider is unloaded. The
    // focus point of the colliders stays behind, but there is nothing left to collide with.
    f.world->LoadBlocking({1000.0, 21.0, 0.0});
    f.Sync({0.0, 21.0, 0.0}, 20.0f);
    EXPECT_EQ(f.colliders->Stats().colliders, 0u);
    EXPECT_EQ(f.RayDown(0.0f, 0.0f, 30.0f), -1000.0f);
    EXPECT_FALSE(f.colliders->IsReady({0.0, 19.0, 0.0}));

    // And they are rebuilt around the new position.
    f.Sync({1000.0, 21.0, 0.0}, 20.0f);
    EXPECT_GT(f.colliders->Stats().colliders, 0u);
    EXPECT_NEAR(f.RayDown(1000.0f, 0.0f, 30.0f), f.ground, 5e-3f);
}

TEST(TerrainColliders, IsReadyTellsWhenTerrainCanBeCollidedWith) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    const glm::dvec3 focus(0.0, 21.0, 0.0);
    const glm::dvec3 inGround(1.0, 19.0, 1.0);        // inside the dense surface chunk
    const glm::dvec3 inAir(1.0, 40.0, 1.0);           // an empty chunk above it
    const glm::dvec3 deep(1.0, 5.0, 1.0);             // uniform rock, enclosed by rock and the surface chunk

    // Nothing is loaded yet.
    EXPECT_FALSE(f.colliders->IsReady(inGround));
    EXPECT_FALSE(f.colliders->IsReady(inAir));

    f.world->LoadBlocking(focus);
    // Loaded, but the surface chunk has no collider yet. Air never needs one.
    EXPECT_FALSE(f.colliders->IsReady(inGround));
    EXPECT_TRUE(f.colliders->IsReady(inAir));

    f.Sync(focus, 20.0f);
    EXPECT_TRUE(f.colliders->IsReady(inGround));
    EXPECT_TRUE(f.colliders->IsReady(inAir));
    EXPECT_TRUE(f.colliders->IsReady(deep));          // needs none: nothing can reach it

    // Loaded terrain outside the collider radius is not ready; outside the window even less.
    EXPECT_FALSE(f.colliders->IsReady({50.0, 19.0, 50.0}));
    EXPECT_FALSE(f.colliders->IsReady({5000.0, 19.0, 0.0}));

    // A radius of zero asks for nothing.
    f.Sync(focus, 0.0f);
    EXPECT_EQ(f.colliders->Stats().colliders, 0u);
    EXPECT_FALSE(f.colliders->IsReady(inGround));
}

TEST(TerrainColliders, DestroyedWhileCookingIsSafe) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking({0.0, 21.0, 0.0});
    const std::array<glm::dvec3, 1> points = {glm::dvec3(0.0, 21.0, 0.0)};
    f.colliders->Update(points, 40.0f);         // jobs are queued and running now
    f.colliders.reset();                        // ... and their owner goes away
    f.jobs.WaitIdle();
    EXPECT_EQ(f.physics.BodyCount(), 0u);

    // A new instance starts from scratch.
    f.colliders = std::make_unique<TerrainColliders>(f.physics, *f.world, f.jobs, f.materials);
    f.Sync({0.0, 21.0, 0.0}, 20.0f);
    EXPECT_GT(f.colliders->Stats().colliders, 0u);
    EXPECT_NEAR(f.RayDown(0.5f, 0.5f, 30.0f), f.ground, 2e-3f);
}

TEST(TerrainColliders, CharacterWalksAndClimbsSmallStep) {
    Fixture f(40, 0.25f);                         // 25 cm voxels: ground at y = 10 m, chunks are 8 m
    ASSERT_TRUE(f.initialized);
    ASSERT_FLOAT_EQ(f.ground, 10.0f);
    const glm::dvec3 focus(4.0, 11.0, 0.0);
    f.world->LoadBlocking(focus);
    // One voxel (25 cm) high platform from x = 6 m to x = 14 m, and a 1 m wall at x = 12 m on it.
    f.world->FillBox({24, 40, -16}, {56, 41, 16}, kRock);
    f.world->FillBox({48, 41, -16}, {50, 45, 16}, kRock);
    f.Sync(focus, 24.0f);

    CharacterDesc desc;
    desc.position = {1.0f, f.ground + 0.2f, 0.5f};
    const CharacterId character = f.physics.CreateCharacter(desc);     // default step height: 35 cm
    ASSERT_TRUE(character.IsValid());

    // Settles on the ground.
    for (int i = 0; i < 30; ++i) f.physics.UpdateCharacter(character, kStep, glm::vec3(0.0f));
    CharacterState state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_NEAR(state.position.y, f.ground, 0.03f);
    EXPECT_NEAR(state.position.x, 1.0f, 0.02f);

    // Walks 4 m across flat terrain in 2 s, crossing nothing but coplanar triangles.
    int groundedSteps = 0;
    for (int i = 0; i < 120; ++i) {
        f.physics.UpdateCharacter(character, kStep, {2.0f, 0.0f, 0.0f});
        if (f.physics.GetCharacterState(character).onGround) ++groundedSteps;
    }
    state = f.physics.GetCharacterState(character);
    EXPECT_EQ(groundedSteps, 120);
    EXPECT_NEAR(state.position.x, 5.0f, 0.15f);
    EXPECT_NEAR(state.position.y, f.ground, 0.03f);
    EXPECT_NEAR(state.position.z, 0.5f, 0.05f);
    EXPECT_NEAR(state.velocity.x, 2.0f, 0.2f);

    // Keeps walking: climbs the 25 cm step at x = 6 m without being told to jump.
    for (int i = 0; i < 120; ++i) f.physics.UpdateCharacter(character, kStep, {2.0f, 0.0f, 0.0f});
    state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_GT(state.position.x, 7.5f);
    EXPECT_NEAR(state.position.y, f.ground + 0.25f, 0.03f);

    // ... but not the 1 m wall at x = 12 m.
    for (int i = 0; i < 240; ++i) f.physics.UpdateCharacter(character, kStep, {2.0f, 0.0f, 0.0f});
    state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_NEAR(state.position.x, 12.0f - desc.radius, 0.08f);
    EXPECT_NEAR(state.position.y, f.ground + 0.25f, 0.03f);

    // Walking back steps down the ledge and stays on the ground.
    for (int i = 0; i < 240; ++i) f.physics.UpdateCharacter(character, kStep, {-2.0f, 0.0f, 0.0f});
    state = f.physics.GetCharacterState(character);
    EXPECT_TRUE(state.onGround);
    EXPECT_LT(state.position.x, 5.5f);
    EXPECT_NEAR(state.position.y, f.ground, 0.03f);
}
