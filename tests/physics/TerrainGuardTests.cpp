// Bodies must never be simulated without terrain collision under them: TerrainColliders freezes
// dynamic bodies whose surroundings are not ready and resumes them when they are.

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
    constexpr float kRadius = 20.0f;

    /// A flat world (ground at y = 20 m, 16 m chunks, a window of 128 m) with physics and terrain colliders.
    struct Fixture {
        explicit Fixture(int groundVoxels = 40) : store(512), jobs(3), ground(static_cast<float>(groundVoxels) * 0.5f) {
            WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = {8, 8, 8};
            materials.Set(kWorldPalette, kSurface, Material{}, "surface");
            materials.Set(kWorldPalette, kRock, Material{}, "rock");
            world = std::make_unique<World>(config, store, jobs, CreateFlatGenerator(groundVoxels, kSurface, kRock), &materials);

            PhysicsConfig physicsConfig;
            physicsConfig.maxBodies = 2048;
            physicsConfig.maxBodyPairs = 4096;
            physicsConfig.maxContactConstraints = 4096;
            physicsConfig.threads = 1;
            initialized = physics.Initialize(physicsConfig);
            colliders = std::make_unique<TerrainColliders>(physics, *world, jobs, materials);
        }

        /// What the engine does every frame: stream the colliders around `focus`, then simulate.
        /// The cooking jobs get a moment to finish, as they would over the 16 ms of a real frame.
        void Frames(int count, const glm::dvec3& focus, float radius = kRadius) {
            const std::array<glm::dvec3, 1> points = {focus};
            for (int i = 0; i < count; ++i) {
                colliders->Update(points, radius);
                physics.Step(kStep);
                if (colliders->Stats().pending != 0) jobs.WaitIdle();
            }
        }

        BodyId DropBox(const glm::vec3& position, float halfExtent = 0.5f) {
            BodyDesc desc;
            desc.shape = physics.CreateBoxShape(glm::vec3(halfExtent));
            desc.position = position;
            return physics.CreateBody(desc);
        }

        /// A box that has come to rest (asleep) on the ground at (x, z).
        BodyId RestingBox(float x, float z, const glm::dvec3& focus) {
            const BodyId box = DropBox({x, ground + 0.6f, z});
            Frames(240, focus);
            EXPECT_NEAR(physics.GetState(box).position.y, ground + 0.5f, 0.05f);
            EXPECT_FALSE(physics.GetState(box).active);
            return box;
        }

        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        std::unique_ptr<World> world;
        PhysicsWorld physics;
        std::unique_ptr<TerrainColliders> colliders;
        float ground;
        bool initialized = false;
    };

    const glm::dvec3 kHome(0.0, 21.0, 0.0);
    const glm::dvec3 kAway(50.0, 21.0, 50.0);          // inside the window, far outside the collider radius
}

// --- reproductions of the reported bug (they fail without the guard) ---------------------------

TEST(TerrainGuard, RestingBodyDoesNotFallWhenTheFocusLeavesAndItIsWoken) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    const BodyId box = f.RestingBox(5.0f, 6.0f, kHome);

    // The camera walks away: the collider under the box is dropped (colliders a body asked
    // for are kept for a couple of seconds after it fell asleep).
    f.Frames(200, kAway);
    EXPECT_FALSE(f.physics.Raycast({9.0f, 30.0f, 9.0f}, {0.0f, -1.0f, 0.0f}, 20.0f).hit) << "the collider should be gone";

    // Something wakes the box (an explosion nearby, another body ...).
    f.physics.Activate(box);
    f.Frames(180, kAway);

    const BodyState state = f.physics.GetState(box);
    EXPECT_NEAR(state.position.y, f.ground + 0.5f, 0.05f) << "the box fell through terrain that had no collider";
    EXPECT_NEAR(state.position.x, 5.0f, 0.05f);
}

TEST(TerrainGuard, BodySpawnedWhereCollidersAreNotReadyDoesNotFallThrough) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    f.Frames(5, kAway);                                 // colliders exist around the camera only

    // Debris appears far from the camera, right above the surface.
    const BodyId box = f.DropBox({5.0f, f.ground + 0.6f, 6.0f});
    f.Frames(240, kAway);
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);
}

TEST(TerrainGuard, BodyDoesNotFallWhenItsChunkIsUnloaded) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    const BodyId box = f.RestingBox(5.0f, 6.0f, kHome);

    // The world window moves a kilometre away: the chunk under the box is unloaded.
    const glm::dvec3 far(1000.0, 21.0, 0.0);
    f.world->LoadBlocking(far);
    ASSERT_FALSE(f.world->IsChunkLoaded({0, 1, 0}));
    f.Frames(5, far);
    f.physics.Activate(box);
    f.Frames(120, far);
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f) << "the box fell out of the unloaded world";

    // Coming back: it is simulated again and still rests on the ground.
    f.world->LoadBlocking(kHome);
    f.Frames(240, kHome);
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);
}

// --- how the guard behaves ---------------------------------------------------------------------

TEST(TerrainGuard, ParkedBodyIsFrozenCostsNoColliderAndResumesAsleep) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    const BodyId box = f.RestingBox(5.0f, 6.0f, kHome);
    EXPECT_FALSE(f.physics.IsFrozen(box));
    EXPECT_EQ(f.colliders->Stats().frozenBodies, 0u);

    f.Frames(200, kAway);
    EXPECT_TRUE(f.physics.IsFrozen(box));
    EXPECT_TRUE(f.physics.GetState(box).frozen);
    EXPECT_FALSE(f.physics.GetState(box).active);
    EXPECT_EQ(f.colliders->Stats().frozenBodies, 1u);
    // A sleeping body left behind asks for nothing: no collider is kept under it.
    EXPECT_FALSE(f.colliders->IsReady({5.0, 19.0, 6.0}));
    EXPECT_FALSE(f.physics.Raycast({9.0f, 30.0f, 9.0f}, {0.0f, -1.0f, 0.0f}, 20.0f).hit);

    // The camera returns: the terrain is rebuilt and the body is released, still asleep.
    f.Frames(30, kHome);
    EXPECT_FALSE(f.physics.IsFrozen(box));
    EXPECT_FALSE(f.physics.GetState(box).active);
    EXPECT_EQ(f.colliders->Stats().frozenBodies, 0u);
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);
}

TEST(TerrainGuard, WokenParkedBodyBringsItsOwnCollidersBack) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    const BodyId box = f.RestingBox(5.0f, 6.0f, kHome);
    f.Frames(200, kAway);
    ASSERT_TRUE(f.physics.IsFrozen(box));

    // An explosion next to it: the impulse is kept and the body asks for its ground.
    f.physics.ActivateInBox(math::AABB(glm::vec3(4.0f, 20.0f, 5.0f), glm::vec3(6.0f, 22.0f, 7.0f)));
    f.physics.AddImpulse(box, glm::vec3(0.0f, 5.0f, 0.0f) * f.physics.GetMass(box));
    EXPECT_TRUE(f.physics.IsFrozen(box));                       // not woken while frozen

    float highest = 0.0f;
    int releasedAfter = -1;
    for (int i = 0; i < 240; ++i) {
        f.Frames(1, kAway);
        if (releasedAfter < 0 && !f.physics.IsFrozen(box)) {
            releasedAfter = i;
            EXPECT_TRUE(f.colliders->IsReady({5.0, 19.0, 6.0}));
            EXPECT_TRUE(f.physics.GetState(box).active);
        }
        highest = std::max(highest, f.physics.GetState(box).position.y);
    }
    EXPECT_GE(releasedAfter, 0);
    EXPECT_LT(releasedAfter, 10);                               // a few frames of cooking, not seconds
    EXPECT_GT(highest, f.ground + 1.0f);                        // the push was not lost: it jumped ...
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);   // ... and landed again
}

TEST(TerrainGuard, BodyFrozenInFlightKeepsItsVelocity) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    f.Frames(5, kAway);

    // Thrown sideways, high above terrain whose collider does not exist yet. Without job
    // threads being given time, the first guard must hold it.
    BodyDesc desc;
    desc.shape = f.physics.CreateBoxShape(glm::vec3(0.5f));
    desc.position = {5.0f, f.ground + 4.0f, 6.0f};
    desc.linearVelocity = {3.0f, 0.0f, 0.0f};
    const BodyId box = f.physics.CreateBody(desc);
    f.colliders->GuardBodies();
    ASSERT_TRUE(f.physics.IsFrozen(box));
    f.physics.Step(kStep);
    f.physics.Step(kStep);
    EXPECT_FLOAT_EQ(f.physics.GetState(box).position.x, 5.0f);  // held exactly in place
    EXPECT_FLOAT_EQ(f.physics.GetState(box).position.y, f.ground + 4.0f);
    EXPECT_EQ(f.physics.GetState(box).linearVelocity, glm::vec3(0.0f));

    f.colliders->Flush();                                       // the colliders it asked for are cooked
    ASSERT_FALSE(f.physics.IsFrozen(box));
    const BodyState resumed = f.physics.GetState(box);
    EXPECT_TRUE(resumed.active);
    EXPECT_NEAR(resumed.linearVelocity.x, 3.0f, 1e-4f);
    f.Frames(300, kAway);
    EXPECT_GT(f.physics.GetState(box).position.x, 6.0f);        // it flew on
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);
}

TEST(TerrainGuard, ManyBodiesFarFromTheFocusAllLand) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    f.Frames(5, kAway);

    // More bodies than the engine ever used as focus points, spread over many chunks.
    std::vector<BodyId> boxes;
    for (int i = 0; i < 60; ++i) {
        const float x = -60.0f + 9.5f * static_cast<float>(i % 10);
        const float z = -60.0f + 11.0f * static_cast<float>(i / 10);
        boxes.push_back(f.DropBox({x, f.ground + 1.0f + 0.1f * static_cast<float>(i % 7), z}, 0.3f));
    }
    f.Frames(90, kAway);
    EXPECT_EQ(f.colliders->Stats().frozenBodies, 0u);           // everything is being simulated by now
    f.Frames(210, kAway);
    for (const BodyId box : boxes) EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.3f, 0.05f);
}

TEST(TerrainGuard, BodyStopsAtTheEdgeOfTheWorldWindow) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    // The window is 8 chunks of 16 m around the focus: x in [-64, 64). A box slides out of it.
    BodyDesc desc;
    desc.shape = f.physics.CreateBoxShape(glm::vec3(0.5f));
    desc.position = {60.0f, f.ground + 6.0f, 0.0f};
    desc.linearVelocity = {12.0f, 0.0f, 0.0f};
    desc.gravityFactor = 0.0f;
    const BodyId box = f.physics.CreateBody(desc);
    f.Frames(120, kHome);
    EXPECT_TRUE(f.physics.IsFrozen(box));
    EXPECT_LT(f.physics.GetState(box).position.x, 64.0f);       // never left the loaded world
    EXPECT_GT(f.physics.GetState(box).position.x, 61.0f);
}

TEST(TerrainGuard, EditedChunkKeepsItsColliderUntilTheNewOneIsInstalled) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    const std::array<glm::dvec3, 1> points = {kHome};
    f.colliders->Update(points, kRadius);
    f.colliders->Flush();
    const BodyId box = f.DropBox({5.0f, f.ground + 0.6f, 6.0f});
    for (int i = 0; i < 240; ++i) f.physics.Step(kStep);
    ASSERT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);

    // Remove one layer of ground next to the box. Until the new mesh is installed (which only
    // happens inside Update / Flush) the old one must still be there and the body simulated.
    ASSERT_GT(f.world->FillBox({20, 39, 20}, {24, 40, 24}, 0), 0u);
    const uint32_t built = f.colliders->Stats().built;
    f.colliders->Update(points, kRadius);                       // starts the re-cook
    EXPECT_EQ(f.colliders->Stats().built, built);
    EXPECT_TRUE(f.colliders->IsReady({5.0, 19.0, 6.0}));
    EXPECT_NEAR(f.physics.Raycast({11.0f, 30.0f, 11.0f}, {0.0f, -1.0f, 0.0f}, 20.0f).position.y, f.ground, 2e-3f);   // old surface
    f.physics.Activate(box);
    f.colliders->GuardBodies();
    EXPECT_FALSE(f.physics.IsFrozen(box));
    for (int i = 0; i < 30; ++i) f.physics.Step(kStep);
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);

    f.colliders->Flush();
    EXPECT_GT(f.colliders->Stats().built, built);
    EXPECT_NEAR(f.physics.Raycast({11.0f, 30.0f, 11.0f}, {0.0f, -1.0f, 0.0f}, 20.0f).position.y, f.ground - 0.5f, 2e-3f);  // new surface
    EXPECT_FALSE(f.physics.IsFrozen(box));
}

// --- chunks that are edited again and again, and what editing does to the neighbours -----------

TEST(TerrainGuard, ChunkEditedEveryFrameStillGetsNewColliders) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    f.Frames(3, kHome);
    f.colliders->Flush();

    // A pit, then one more voxel of the same chunk every frame (a brush held down, a channelled
    // spell): there is never a frame without a newer edit than the mesh that just finished.
    ASSERT_GT(f.world->FillBox({20, 39, 20}, {24, 40, 24}, 0), 0u);
    for (int i = 0; i < 12; ++i) {
        f.Frames(1, kHome);
        ASSERT_TRUE(f.world->SetVoxel({2 + i, 39, 2}, 0));
    }
    // The stroke is still going on; the pit dug at its start must have collision by now.
    EXPECT_NEAR(f.physics.Raycast({11.0f, 30.0f, 11.0f}, {0.0f, -1.0f, 0.0f}, 20.0f).position.y, f.ground - 0.5f, 2e-3f);

    f.Frames(1, kHome);
    f.colliders->Flush();
    EXPECT_NEAR(f.physics.Raycast({11.0f, 30.0f, 11.0f}, {0.0f, -1.0f, 0.0f}, 20.0f).position.y, f.ground - 0.5f, 2e-3f);
    EXPECT_NEAR(f.physics.Raycast({6.75f, 30.0f, 1.25f}, {0.0f, -1.0f, 0.0f}, 20.0f).position.y, f.ground - 0.5f, 2e-3f);   // the last voxel
}

TEST(TerrainGuard, BodyLandsOnAChunkThatIsEditedEveryFrame) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    f.Frames(5, kHome);

    // Far from the camera: the chunk has no collider yet, the body has to ask for it - while
    // the chunk changes every frame.
    const BodyId box = f.DropBox({50.0f, f.ground + 3.0f, 50.0f});
    for (int i = 0; i < 150; ++i) {
        ASSERT_TRUE(f.world->SetVoxel({98 + i % 28, 39, 120 + i / 28}, 0));
        f.Frames(1, kHome);
    }
    EXPECT_FALSE(f.physics.IsFrozen(box)) << "the body waited for a collider that was never installed";
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground + 0.5f, 0.05f);
}

TEST(TerrainGuard, ChunkUncoveredByCarvingItsNeighbourIsNotReadyUntilRecooked) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    f.Frames(3, kHome);
    f.colliders->Flush();
    // Chunk y = 1 holds the ground (16 m .. 20 m), chunk y = 0 below is solid rock: its top is
    // covered by the ground above, so its collider has no faces there.
    ASSERT_TRUE(f.colliders->IsReady({11.0, 15.0, 11.0}));

    // A shaft through the whole ground layer of the upper chunk uncovers the lower one.
    ASSERT_GT(f.world->FillBox({20, 32, 20}, {24, 40, 24}, 0), 0u);
    const std::array<glm::dvec3, 1> points = {kHome};
    f.colliders->Update(points, kRadius);                       // both re-cooks are started, neither is installed
    EXPECT_TRUE(f.colliders->IsReady({11.0, 19.0, 11.0}));      // edited chunk: the old surface is still there
    EXPECT_FALSE(f.colliders->IsReady({11.0, 15.0, 11.0}));     // neighbour: has no surface where one is needed now

    // A body at the bottom of the shaft would drop into the rock: it is held ...
    const BodyId box = f.DropBox({11.0f, 16.6f, 11.0f});
    f.colliders->GuardBodies();
    EXPECT_TRUE(f.physics.IsFrozen(box));
    for (int i = 0; i < 20; ++i) f.physics.Step(kStep);
    EXPECT_NEAR(f.physics.GetState(box).position.y, 16.6f, 1e-4f);

    // ... until the lower chunk has its top faces.
    f.colliders->Flush();
    EXPECT_TRUE(f.colliders->IsReady({11.0, 15.0, 11.0}));
    EXPECT_FALSE(f.physics.IsFrozen(box));
    EXPECT_NEAR(f.physics.Raycast({10.2f, 30.0f, 10.2f}, {0.0f, -1.0f, 0.0f}, 20.0f).position.y, 16.0f, 2e-3f);   // beside the box
    f.Frames(120, kHome);
    EXPECT_NEAR(f.physics.GetState(box).position.y, 16.5f, 0.05f);
}

TEST(TerrainGuard, EditAwayFromTheBorderRecooksOnlyTheEditedChunk) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    f.Frames(3, kHome);
    f.colliders->Flush();
    const BodyId box = f.RestingBox(20.0f, 6.0f, kHome);        // on the neighbouring chunk (x = 16 m .. 32 m)
    f.colliders->Flush();

    const uint32_t built = f.colliders->Stats().built;
    ASSERT_TRUE(f.world->SetVoxel({10, 39, 10}, 0));            // chunk (0, 1, 0), not in a border layer
    const std::array<glm::dvec3, 1> points = {kHome};
    f.colliders->Update(points, kRadius);
    EXPECT_TRUE(f.colliders->IsReady({20.0, 19.0, 6.0}));
    f.colliders->Flush();
    EXPECT_EQ(f.colliders->Stats().built, built + 1);           // the four neighbours with a mesh kept theirs
    EXPECT_FALSE(f.physics.GetState(box).active);               // and nothing on them was disturbed

    // In the border layer the neighbour does change: one of its side faces is uncovered.
    ASSERT_TRUE(f.world->SetVoxel({31, 39, 10}, 0));
    f.colliders->Update(points, kRadius);
    EXPECT_FALSE(f.colliders->IsReady({20.0, 19.0, 6.0}));
    f.colliders->Flush();
    EXPECT_EQ(f.colliders->Stats().built, built + 3);
    EXPECT_TRUE(f.colliders->IsReady({20.0, 19.0, 6.0}));
    // Filling it again covers the face: the neighbour is re-cooked but stays ready meanwhile.
    ASSERT_TRUE(f.world->SetVoxel({31, 39, 10}, kSurface));
    f.colliders->Update(points, kRadius);
    EXPECT_TRUE(f.colliders->IsReady({20.0, 19.0, 6.0}));
    f.colliders->Flush();
}

TEST(TerrainGuard, ParkedBodyFallsWhenTheGroundUnderItIsCarvedFarFromTheCamera) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    const BodyId box = f.RestingBox(5.0f, 6.0f, kHome);
    f.Frames(200, kAway);                                       // parked: frozen asleep, no collider
    ASSERT_TRUE(f.physics.IsFrozen(box));
    ASSERT_FALSE(f.physics.Raycast({9.0f, 30.0f, 9.0f}, {0.0f, -1.0f, 0.0f}, 20.0f).hit);

    // A ranged blast takes a metre of ground from under it. Nothing is tracked there.
    ASSERT_GT(f.world->FillBox({7, 38, 9}, {14, 40, 16}, 0), 0u);
    f.Frames(180, kAway);
    EXPECT_NEAR(f.physics.GetState(box).position.y, f.ground - 0.5f, 0.05f) << "the parked body was left hanging in the air";
}

TEST(TerrainGuard, BoxReadinessSeesTheGroundChunkBelowTheFeet) {
    Fixture f(64);                                              // ground surface exactly on a chunk boundary (32 m)
    ASSERT_TRUE(f.initialized);
    const glm::dvec3 home(0.0, 33.0, 0.0), away(50.0, 33.0, 50.0);
    f.world->LoadBlocking(home);
    f.Frames(3, home);
    f.colliders->Flush();

    const glm::vec3 feet(5.0f, 32.0f, 6.0f);                    // standing on the ground: in the empty chunk above it
    const math::AABB capsule(feet - glm::vec3(0.3f, 0.0f, 0.3f), feet + glm::vec3(0.3f, 1.8f, 0.3f));
    EXPECT_TRUE(f.colliders->IsReady(glm::dvec3(feet)));
    EXPECT_TRUE(f.colliders->IsReady(capsule));

    f.Frames(200, away);                                        // the collider of the ground chunk is dropped
    ASSERT_FALSE(f.physics.Raycast({5.0f, 40.0f, 6.0f}, {0.0f, -1.0f, 0.0f}, 20.0f).hit);
    EXPECT_TRUE(f.colliders->IsReady(glm::dvec3(feet)));        // the point test cannot know
    EXPECT_FALSE(f.colliders->IsReady(capsule));
    EXPECT_FALSE(f.colliders->IsReady(math::AABB()));

    f.Frames(3, home);
    f.colliders->Flush();
    EXPECT_TRUE(f.colliders->IsReady(capsule));
}

TEST(TerrainGuard, KinematicAndStaticBodiesAreLeftAlone) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    f.world->LoadBlocking(kHome);
    BodyDesc desc;
    desc.shape = f.physics.CreateBoxShape(glm::vec3(0.5f));
    desc.position = {5.0f, f.ground + 3.0f, 6.0f};
    desc.motion = MotionType::Kinematic;
    const BodyId kinematic = f.physics.CreateBody(desc);
    desc.motion = MotionType::Static;
    desc.position.x = 9.0f;
    const BodyId fixed = f.physics.CreateBody(desc);
    f.Frames(10, kAway);
    EXPECT_FALSE(f.physics.IsFrozen(kinematic));
    EXPECT_FALSE(f.physics.IsFrozen(fixed));
    EXPECT_EQ(f.colliders->Stats().frozenBodies, 0u);
}

// --- PhysicsWorld::SetFrozen -------------------------------------------------------------------

namespace {
    struct Bare {
        Bare() {
            PhysicsConfig config;
            config.maxBodies = 256;
            config.threads = 1;
            initialized = physics.Initialize(config);
        }
        BodyId Box(const glm::vec3& position, MotionType motion = MotionType::Dynamic) {
            BodyDesc desc;
            desc.shape = physics.CreateBoxShape(glm::vec3(0.5f));
            desc.position = position;
            desc.motion = motion;
            return physics.CreateBody(desc);
        }
        void Step(int steps) {
            for (int i = 0; i < steps; ++i) physics.Step(kStep);
        }
        PhysicsWorld physics;
        bool initialized = false;
    };
}

TEST(FrozenBodies, AFrozenBodyDoesNotMoveAndCannotBeWoken) {
    Bare f;
    ASSERT_TRUE(f.initialized);
    const BodyId box = f.Box({0.0f, 10.0f, 0.0f});
    f.Step(30);
    const BodyState before = f.physics.GetState(box);
    ASSERT_LT(before.linearVelocity.y, -1.0f);

    f.physics.SetFrozen(box, true);
    EXPECT_TRUE(f.physics.IsFrozen(box));
    EXPECT_EQ(f.physics.FrozenBodyCount(), 1u);
    f.physics.Activate(box);
    f.physics.ActivateInBox(math::AABB(glm::vec3(-5.0f), glm::vec3(5.0f, 15.0f, 5.0f)));
    f.Step(60);
    const BodyState held = f.physics.GetState(box);
    EXPECT_EQ(held.position, before.position);
    EXPECT_FALSE(held.active);
    EXPECT_TRUE(held.frozen);
    EXPECT_EQ(f.physics.ActiveBodyCount(), 0u);

    std::vector<DynamicBodyInfo> infos;
    f.physics.GetDynamicBodies(infos);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].body, box);
    EXPECT_TRUE(infos[0].frozen);
    EXPECT_TRUE(infos[0].wantsToMove);
    EXPECT_NEAR(infos[0].linearVelocity.y, before.linearVelocity.y, 1e-5f);
    EXPECT_NEAR(infos[0].bounds.min.y, before.position.y - 0.5f, 1e-3f);

    // Released: it continues with the velocity it had.
    f.physics.SetFrozen(box, false);
    EXPECT_FALSE(f.physics.IsFrozen(box));
    EXPECT_EQ(f.physics.FrozenBodyCount(), 0u);
    const BodyState resumed = f.physics.GetState(box);
    EXPECT_TRUE(resumed.active);
    EXPECT_NEAR(resumed.linearVelocity.y, before.linearVelocity.y, 1e-5f);
    f.Step(10);
    EXPECT_LT(f.physics.GetState(box).position.y, before.position.y - 0.5f);
}

TEST(FrozenBodies, ASleepingBodyResumesAsleepUnlessSomethingWantedItAwake) {
    Bare f;
    ASSERT_TRUE(f.initialized);
    const BodyId floor = f.Box({0.0f, -0.5f, 0.0f}, MotionType::Static);
    const BodyId box = f.Box({0.0f, 0.6f, 0.0f});
    f.Step(240);
    ASSERT_FALSE(f.physics.GetState(box).active);

    f.physics.SetFrozen(box, true);
    f.physics.SetFrozen(box, false);
    EXPECT_FALSE(f.physics.GetState(box).active);

    // The floor disappears while the body is frozen and something tries to wake it.
    f.physics.SetFrozen(box, true);
    f.physics.DestroyBody(floor);
    f.physics.ActivateInBox(math::AABB(glm::vec3(-1.0f), glm::vec3(1.0f)));
    f.Step(30);
    EXPECT_NEAR(f.physics.GetState(box).position.y, 0.5f, 0.02f);
    f.physics.SetFrozen(box, false);
    EXPECT_TRUE(f.physics.GetState(box).active);
    f.Step(30);
    EXPECT_LT(f.physics.GetState(box).position.y, 0.0f);
}

TEST(FrozenBodies, ImpulsesAndVelocitiesAreKeptForLater) {
    Bare f;
    ASSERT_TRUE(f.initialized);
    const BodyId box = f.Box({0.0f, 10.0f, 0.0f});
    f.physics.SetFrozen(box, true);
    const float mass = f.physics.GetMass(box);
    ASSERT_GT(mass, 0.0f);

    f.physics.SetVelocity(box, {1.0f, 0.0f, 0.0f}, glm::vec3(0.0f));
    f.physics.AddImpulse(box, glm::vec3(0.0f, 0.0f, 2.0f) * mass);
    EXPECT_EQ(f.physics.GetState(box).linearVelocity, glm::vec3(0.0f));     // frozen bodies report no motion
    f.physics.SetFrozen(box, false);
    const BodyState state = f.physics.GetState(box);
    EXPECT_NEAR(state.linearVelocity.x, 1.0f, 1e-4f);
    EXPECT_NEAR(state.linearVelocity.z, 2.0f, 1e-4f);
    EXPECT_TRUE(state.active);
}

TEST(FrozenBodies, OtherBodiesRestOnAFrozenBody) {
    Bare f;
    ASSERT_TRUE(f.initialized);
    const BodyId lower = f.Box({0.0f, 5.0f, 0.0f});
    f.physics.SetFrozen(lower, true);
    const BodyId upper = f.Box({0.0f, 6.2f, 0.0f});
    f.Step(240);
    EXPECT_FLOAT_EQ(f.physics.GetState(lower).position.y, 5.0f);
    EXPECT_NEAR(f.physics.GetState(upper).position.y, 6.0f, 0.05f);
}

TEST(FrozenBodies, OnlyDynamicBodiesFreezeAndBookkeepingFollowsTheBody) {
    Bare f;
    ASSERT_TRUE(f.initialized);
    const BodyId fixed = f.Box({0.0f, 0.0f, 0.0f}, MotionType::Static);
    const BodyId kinematic = f.Box({3.0f, 0.0f, 0.0f}, MotionType::Kinematic);
    f.physics.SetFrozen(fixed, true);
    f.physics.SetFrozen(kinematic, true);
    f.physics.SetFrozen(BodyId{}, true);
    EXPECT_FALSE(f.physics.IsFrozen(fixed));
    EXPECT_FALSE(f.physics.IsFrozen(kinematic));
    EXPECT_FALSE(f.physics.IsFrozen(BodyId{}));
    EXPECT_EQ(f.physics.FrozenBodyCount(), 0u);

    std::vector<DynamicBodyInfo> infos;
    f.physics.GetDynamicBodies(infos);
    EXPECT_TRUE(infos.empty());

    // A static body made dynamic is guarded from then on; a destroyed frozen body is forgotten.
    f.physics.SetMotionType(fixed, MotionType::Dynamic);
    f.physics.GetDynamicBodies(infos);
    ASSERT_EQ(infos.size(), 1u);
    f.physics.SetFrozen(fixed, true);
    EXPECT_EQ(f.physics.FrozenBodyCount(), 1u);
    f.physics.DestroyBody(fixed);
    EXPECT_EQ(f.physics.FrozenBodyCount(), 0u);
    EXPECT_FALSE(f.physics.IsFrozen(fixed));

    // Turning a frozen body into a static one releases the record, not the body.
    const BodyId box = f.Box({0.0f, 10.0f, 0.0f});
    f.physics.SetFrozen(box, true);
    f.physics.SetMotionType(box, MotionType::Static);
    EXPECT_EQ(f.physics.FrozenBodyCount(), 0u);
    f.Step(30);
    EXPECT_FLOAT_EQ(f.physics.GetState(box).position.y, 10.0f);
    f.physics.SetMotionType(box, MotionType::Dynamic);
    f.Step(30);
    EXPECT_LT(f.physics.GetState(box).position.y, 10.0f);
}
