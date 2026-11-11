// sim::Simulation: the simulated half of the game - world, physics, terrain colliders and the
// scene on a numbered fixed tick. This binary links Vortex::Sim only (the build stops at
// configure time if Vulkan, GLFW or ImGui get into it, see vortex_assert_gpu_free), so every
// test here is also the proof that a session server can be built without a renderer.

#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.scene;
import vortex.sim;

using namespace vortex;
using scene::Entity;
using scene::kNoEntity;

namespace {
    constexpr float kVoxel = 0.1f;
    constexpr int kGroundVoxels = 80;                 // flat ground: its top is at y = 8 m
    constexpr float kGround = kGroundVoxels * kVoxel;
    const glm::dvec3 kFocus(0.0, 8.0, 0.0);

    /// A small world: 6^3 chunks of 3.2 m (19.2 m across), one physics thread, three workers.
    sim::SimulationConfig SmallConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 512;
        config.maxParticles = 4096;
        config.jobThreads = 3;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = {6, 6, 6};
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        return config;
    }

    /// Flat ground made of two of the default world materials (their ids do not depend on the
    /// library they are registered in).
    std::shared_ptr<voxel::IWorldGenerator> FlatGround() {
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
        return voxel::CreateFlatGenerator(kGroundVoxels, ids.grass, ids.stone);
    }

    /// A simulation on flat ground with the window loaded around kFocus.
    struct FlatWorld {
        explicit FlatWorld(const sim::SimulationConfig& config = SmallConfig()) : simulation(config) {
            initialized = simulation.Initialize(FlatGround());
            if (initialized) simulation.World().LoadBlocking(kFocus);
        }

        /// One tick as a server runs it. Collider cooking gets the time it would have over the
        /// 16 ms of a real tick, so the outcome does not depend on how fast the test machine is.
        void StepOnce(std::span<const glm::dvec3> focus) {
            simulation.Step(focus);
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
        }
        void Ticks(int count) {
            const std::array<glm::dvec3, 1> focus = {kFocus};
            for (int i = 0; i < count; ++i) StepOnce(focus);
        }

        std::shared_ptr<voxel::VoxelModel> Box(const glm::ivec3& size, const std::string& name = "box") {
            voxel::VoxelVolume volume(size);
            volume.FillBox(glm::ivec3(0), size, simulation.WorldMaterialIds().wood);
            return std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, name);
        }

        /// A wooden 8^3 crate whose minimum corner is `height` metres above the ground.
        Entity Crate(float x, float z, float height) {
            return simulation.Scene().CreateVoxelObject({.model = Box(glm::ivec3(8), "crate"), .position = {x, kGround + height, z}});
        }

        physics::BodyState StateOf(Entity entity) {
            const scene::RigidBody* body = simulation.Scene().TryGet<scene::RigidBody>(entity);
            return body ? simulation.Physics().GetState(body->body) : physics::BodyState{};
        }

        sim::Simulation simulation;
        bool initialized = false;
    };

    /// The script of the 600-tick tests: three crates dropped from different heights, a static
    /// block, a lamp and a flash that lives for two seconds.
    struct Playground {
        explicit Playground(FlatWorld& w) {
            crates[0] = w.Crate(-2.0f, 0.5f, 1.0f);
            crates[1] = w.Crate(0.0f, 0.5f, 2.5f);
            crates[2] = w.Crate(2.0f, 0.5f, 4.0f);
            block = w.simulation.Scene().CreateVoxelObject(
                {.model = w.Box({10, 4, 10}, "block"), .position = {0.0f, kGround, -3.0f}, .motion = physics::MotionType::Static});
            lamp = w.simulation.Scene().CreateLight({.position = {0.0, kGround + 3.0, 0.0}});
            flash = w.simulation.Scene().CreateLight({.position = {1.0, kGround + 1.0, 1.0}, .lifetime = 2.0f, .decay = 3.0f});
        }

        std::array<Entity, 3> crates{kNoEntity, kNoEntity, kNoEntity};
        Entity block = kNoEntity;
        Entity lamp = kNoEntity;
        Entity flash = kNoEntity;
    };

    void ExpectPlaygroundAtRest(FlatWorld& w, const Playground& p) {
        scene::Scene& scene = w.simulation.Scene();
        for (const Entity crate : p.crates) {
            ASSERT_TRUE(scene.IsValid(crate));
            const physics::BodyState state = w.StateOf(crate);
            // Fell, landed on the terrain collider and went to sleep there.
            EXPECT_NEAR(state.position.y, kGround, 0.03f);
            EXPECT_FALSE(state.active);
            EXPECT_FALSE(state.frozen);
            // The scene follows physics: the entity's transform is the body's pose.
            const scene::Transform* transform = scene.TryGet<scene::Transform>(crate);
            ASSERT_NE(transform, nullptr);
            EXPECT_EQ(transform->current.position, state.position);
        }
        EXPECT_TRUE(scene.IsValid(p.block));
        EXPECT_NEAR(w.StateOf(p.block).position.y, kGround, 1.0e-4f);
        EXPECT_TRUE(scene.IsValid(p.lamp));
        EXPECT_FALSE(scene.IsValid(p.flash));                    // two seconds are 120 ticks
        EXPECT_EQ(scene.EntityCount(), 5u);
        EXPECT_EQ(w.simulation.Physics().FrozenBodyCount(), 0u);
        EXPECT_GT(w.simulation.Terrain().Stats().colliders, 0u);
    }
}

// --- the stage's acceptance: 600 ticks without a renderer ----------------------------------------

TEST(Simulation, Steps600TicksWithoutARenderer) {
    FlatWorld w;
    ASSERT_TRUE(w.initialized);
    const Playground p(w);
    for (const Entity crate : p.crates) ASSERT_NE(crate, kNoEntity);
    ASSERT_NE(p.block, kNoEntity);
    EXPECT_EQ(w.simulation.Tick(), 0u);

    // The flash is still there just before its two seconds are over...
    w.Ticks(110);
    EXPECT_EQ(w.simulation.Tick(), 110u);
    EXPECT_TRUE(w.simulation.Scene().IsValid(p.flash));
    const scene::PointLight* flash = w.simulation.Scene().TryGet<scene::PointLight>(p.flash);
    ASSERT_NE(flash, nullptr);
    EXPECT_LT(flash->intensity, 5.0f * 0.01f);                   // faded on the tick: exp(-3 * 110 / 60) = 0.004

    // ...and after ten seconds everything has come to rest.
    w.Ticks(490);
    EXPECT_EQ(w.simulation.Tick(), 600u);
    ExpectPlaygroundAtRest(w, p);

    // An offline simulation is one that gets drawn: the four models are baked and hold chunk
    // slots next to the terrain.
    const uint32_t terrainSlots = w.simulation.World().Stats().denseChunks;
    EXPECT_EQ(w.simulation.Chunks().UsedCount(), terrainSlots + 4u);
}

TEST(Simulation, ServerConfigurationSteps600TicksWithoutParticlesOrBakedModels) {
    FlatWorld w(sim::ServerConfig(SmallConfig()));
    ASSERT_TRUE(w.initialized);
    const Playground p(w);
    for (const Entity crate : p.crates) ASSERT_NE(crate, kNoEntity);

    // A blast-sized hole in the first crate while it falls: it loses voxels and stays one piece.
    scene::Scene& scene = w.simulation.Scene();
    const glm::vec3 corner = scene.TryGet<scene::Transform>(p.crates[0])->current.position;
    const scene::DamageResult hit = scene.Damage(p.crates[0], glm::dvec3(corner), 0.35f);
    EXPECT_GT(hit.removed, 0u);
    EXPECT_FALSE(hit.destroyed);

    w.Ticks(600);
    EXPECT_EQ(w.simulation.Tick(), 600u);
    for (const Entity crate : p.crates) {
        ASSERT_TRUE(scene.IsValid(crate));
        const physics::BodyState state = w.StateOf(crate);
        EXPECT_LT(state.position.y, kGround + 0.9f);             // landed (the damaged one may lie tilted)
        EXPECT_GT(state.position.y, kGround - 0.9f);
        EXPECT_FALSE(state.active);
        EXPECT_FALSE(state.frozen);
    }
    EXPECT_FALSE(scene.IsValid(p.flash));
    EXPECT_TRUE(scene.IsValid(p.lamp));

    // Nothing is drawn on a server: no particle ever existed, no model holds a chunk slot (the
    // store holds the terrain and nothing else), nothing is rubble.
    EXPECT_EQ(w.simulation.Particles().Capacity(), 0u);
    EXPECT_EQ(w.simulation.Particles().AliveCount(), 0u);
    EXPECT_EQ(w.simulation.Chunks().UsedCount(), w.simulation.World().Stats().denseChunks);
    for (const auto [entity, voxels] : scene.Registry().view<const scene::VoxelBody>().each()) {
        ASSERT_NE(voxels.model, nullptr);
        EXPECT_FALSE(voxels.model->IsBaked());
    }
    EXPECT_EQ(scene.DebrisCount(), 0u);
}

TEST(Simulation, RunsOnTheBuiltInTerrain) {
    sim::SimulationConfig config = SmallConfig();
    config.world.windowSize = {4, 4, 4};                         // 64 chunks of generated terrain
    sim::Simulation simulation(config);
    ASSERT_TRUE(simulation.Initialize());                        // nullptr = the built-in generator
    const voxel::WorldMaterials& ids = simulation.WorldMaterialIds();
    EXPECT_NE(ids.stone, 0);

    // The same settings Initialize() builds its generator from.
    voxel::TerrainSettings settings;
    settings.voxelSize = kVoxel;
    settings.materials = ids;
    const float surface = voxel::TerrainHeight(settings, 0.5f, 0.5f);
    const glm::dvec3 focus(0.5, static_cast<double>(surface), 0.5);
    simulation.World().LoadBlocking(focus);
    EXPECT_GT(simulation.World().Stats().denseChunks, 0u);
    const voxel::WorldRayHit ground = simulation.World().Raycast(focus + glm::dvec3(0.0, 4.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    ASSERT_TRUE(ground.hit);

    // A crate dropped two metres above whatever is there (ground, or the crown of a tree).
    voxel::VoxelVolume volume(glm::ivec3(8));
    volume.FillBox(glm::ivec3(0), glm::ivec3(8), ids.wood);
    const Entity crate = simulation.Scene().CreateVoxelObject(
        {.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, "crate"),
         .position = glm::vec3(ground.position) + glm::vec3(-0.4f, 2.0f, -0.4f)});
    ASSERT_NE(crate, kNoEntity);

    const std::array<glm::dvec3, 1> points = {focus};
    for (int i = 0; i < 240; ++i) {
        simulation.Step(points);
        if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
    }
    EXPECT_EQ(simulation.Tick(), 240u);
    ASSERT_TRUE(simulation.Scene().IsValid(crate));              // did not fall out of the world
    const scene::Transform* transform = simulation.Scene().TryGet<scene::Transform>(crate);
    ASSERT_NE(transform, nullptr);
    // It fell, and something stopped it: it is below where it started and not far under the
    // point the ray found (it may have slid down a slope or off a branch).
    EXPECT_LT(transform->current.position.y, static_cast<float>(ground.position.y) + 1.9f);
    EXPECT_GT(transform->current.position.y, static_cast<float>(ground.position.y) - 6.0f);
    EXPECT_EQ(simulation.Physics().FrozenBodyCount(), 0u);
}

// --- the tick ------------------------------------------------------------------------------------

TEST(Simulation, TickCountsFixedStepsAndNothingElse) {
    FlatWorld w;
    ASSERT_TRUE(w.initialized);
    const std::array<glm::dvec3, 1> focus = {kFocus};
    EXPECT_EQ(w.simulation.Tick(), 0u);
    EXPECT_FLOAT_EQ(w.simulation.FixedTimeStep(), 1.0f / 60.0f);

    w.simulation.Step(focus);                                    // a whole tick
    EXPECT_EQ(w.simulation.Tick(), 1u);

    // The phases, as the engine uses them: only the physics phase is a tick.
    w.simulation.UpdateStreaming(focus);
    w.simulation.UpdateStreaming(kFocus, focus);
    EXPECT_EQ(w.simulation.Tick(), 1u);
    w.simulation.StepPhysics();
    w.simulation.StepPhysics();
    EXPECT_EQ(w.simulation.Tick(), 3u);
    w.simulation.UpdateLifetimes(0.25f);
    EXPECT_EQ(w.simulation.Tick(), 3u);

    w.simulation.Step({});                                       // no focus point at all is fine
    EXPECT_EQ(w.simulation.Tick(), 4u);
}

TEST(Simulation, StepPhysicsMovesBodiesByOneFixedStep) {
    FlatWorld w;
    ASSERT_TRUE(w.initialized);
    const Entity crate = w.Crate(0.0f, 0.0f, 5.0f);
    ASSERT_NE(crate, kNoEntity);
    w.Ticks(1);                                                  // colliders under the crate exist from here on
    w.simulation.Terrain().Flush();
    ASSERT_FALSE(w.StateOf(crate).frozen);

    const float before = w.StateOf(crate).linearVelocity.y;
    w.simulation.StepPhysics();
    // One step of free fall: gravity times the fixed step (Jolt damps a little).
    EXPECT_NEAR(w.StateOf(crate).linearVelocity.y - before, -9.81f / 60.0f, 0.01f);
    // ...and the scene has the new pose already.
    EXPECT_EQ(w.simulation.Scene().TryGet<scene::Transform>(crate)->current.position, w.StateOf(crate).position);
}

TEST(Simulation, LifetimesRunOnTheTickInStepAndOnlyWhenAskedInThePhases) {
    FlatWorld w;
    ASSERT_TRUE(w.initialized);
    scene::Scene& scene = w.simulation.Scene();
    const Entity onTick = scene.CreateLight({.position = {0.0, 10.0, 0.0}, .lifetime = 0.5f});

    w.Ticks(29);
    EXPECT_TRUE(scene.IsValid(onTick));                          // 29 / 60 s
    w.Ticks(2);
    EXPECT_FALSE(scene.IsValid(onTick));                         // 31 / 60 s

    // Driven by phases, time for lifetimes passes only in UpdateLifetimes (the engine gives it
    // the frame time once per rendered frame).
    const Entity perFrame = scene.CreateLight({.position = {0.0, 10.0, 0.0}, .lifetime = 0.5f});
    for (int i = 0; i < 100; ++i) w.simulation.StepPhysics();
    EXPECT_TRUE(scene.IsValid(perFrame));
    w.simulation.UpdateLifetimes(0.4f);
    EXPECT_TRUE(scene.IsValid(perFrame));
    w.simulation.UpdateLifetimes(0.2f);
    EXPECT_FALSE(scene.IsValid(perFrame));
}

TEST(Simulation, WhatFallsOutOfTheWorldIsDestroyedOnTheTick) {
    FlatWorld w;
    ASSERT_TRUE(w.initialized);
    scene::Scene& scene = w.simulation.Scene();
    const float bottom = static_cast<float>(w.simulation.World().WindowOrigin().y);
    const float lost = bottom - w.simulation.Config().fallOutDepth - 5.0f;

    // Falling, far below the loaded world (there is no terrain there: the guard holds it).
    const Entity falling = scene.CreateVoxelObject({.model = w.Box(glm::ivec3(8)), .position = {0.0f, lost, 0.0f}, .linearVelocity = {0.0f, -5.0f, 0.0f}});
    // Put there on purpose.
    const Entity placed = scene.CreateVoxelObject({.model = w.Box(glm::ivec3(8)), .position = {3.0f, lost, 0.0f}, .motion = physics::MotionType::Static});
    // Falling too, but still inside the margin.
    const Entity inside = scene.CreateVoxelObject({.model = w.Box(glm::ivec3(8)), .position = {6.0f, bottom - 10.0f, 0.0f}, .linearVelocity = {0.0f, -5.0f, 0.0f}});
    ASSERT_NE(falling, kNoEntity);
    ASSERT_NE(placed, kNoEntity);
    ASSERT_NE(inside, kNoEntity);

    w.Ticks(1);
    EXPECT_FALSE(scene.IsValid(falling));
    EXPECT_TRUE(scene.IsValid(placed));
    EXPECT_TRUE(scene.IsValid(inside));

    // Driven by phases (the engine): the physics phase never destroys anything, the lifetime
    // phase does - whatever frame time it is given.
    const Entity alsoFalling = scene.CreateVoxelObject({.model = w.Box(glm::ivec3(8)), .position = {-3.0f, lost, 0.0f}, .linearVelocity = {0.0f, -5.0f, 0.0f}});
    ASSERT_NE(alsoFalling, kNoEntity);
    for (int i = 0; i < 3; ++i) w.simulation.StepPhysics();
    EXPECT_TRUE(scene.IsValid(alsoFalling));
    w.simulation.UpdateLifetimes(0.0f);
    EXPECT_FALSE(scene.IsValid(alsoFalling));
    EXPECT_TRUE(scene.IsValid(placed));
    EXPECT_TRUE(scene.IsValid(inside));
}

// --- the body guard: no dynamic body simulates over terrain without colliders ---------------------

namespace {
    // Far from the crates of the guard tests below (more than colliderRadius + slack away), but
    // inside the world window: its colliders never reach them.
    const glm::dvec3 kFarFocus(-8.0, 8.0, -8.0);
}

TEST(Simulation, StepFreezesABodyWhoseGroundHasNoColliderYet) {
    FlatWorld w;
    ASSERT_TRUE(w.initialized);
    // Nothing was stepped yet, so no collider exists anywhere.
    ASSERT_EQ(w.simulation.Terrain().Stats().colliders, 0u);
    const Entity crate = w.Crate(6.0f, 6.0f, 1.0f);
    ASSERT_NE(crate, kNoEntity);
    const glm::vec3 start = w.StateOf(crate).position;

    // A tick whose focus point is elsewhere and that does not wait for collider cooking: the
    // crate's own collider requests cannot be installed before the physics step.
    const std::array<glm::dvec3, 1> far = {kFarFocus};
    w.simulation.Step(far);
    EXPECT_TRUE(w.StateOf(crate).frozen);
    EXPECT_EQ(w.StateOf(crate).position, start);

    // Once its ground is there it resumes by itself and lands on it.
    w.Ticks(240);
    EXPECT_EQ(w.simulation.Terrain().Stats().pending, 0u);
    EXPECT_FALSE(w.StateOf(crate).frozen);
    EXPECT_NEAR(w.StateOf(crate).position.y, kGround, 0.03f);
    EXPECT_EQ(w.simulation.Physics().FrozenBodyCount(), 0u);
}

TEST(Simulation, StepPhysicsGuardsWhatGameCodeCreatedAfterTheStreamingPhase) {
    FlatWorld w;
    ASSERT_TRUE(w.initialized);
    // A frame as the engine runs it: streaming, then game code that spawns a crate where there
    // is no collider, then the physics phase. Only the guard inside StepPhysics sees the crate.
    const std::array<glm::dvec3, 1> far = {kFarFocus};
    w.simulation.UpdateStreaming(far);
    const Entity crate = w.Crate(6.0f, 6.0f, 1.0f);
    ASSERT_NE(crate, kNoEntity);
    const glm::vec3 start = w.StateOf(crate).position;
    ASSERT_FALSE(w.StateOf(crate).frozen);

    w.simulation.StepPhysics();
    EXPECT_TRUE(w.StateOf(crate).frozen);
    EXPECT_EQ(w.StateOf(crate).position, start);
    EXPECT_EQ(w.simulation.Scene().TryGet<scene::Transform>(crate)->current.position, start);
}

// --- the world window ----------------------------------------------------------------------------

TEST(Simulation, StepHoldsTheWorldWindowWhereverItWasPlaced) {
    sim::Simulation simulation(SmallConfig());
    ASSERT_TRUE(simulation.Initialize(FlatGround()));
    voxel::World& world = simulation.World();

    // Far from the origin and off-centre in its chunk, in every direction.
    for (const glm::dvec3 place : {glm::dvec3(40.0, 8.0, -25.0), glm::dvec3(-131.3, 3.1, 77.7), glm::dvec3(0.01, -0.01, 3.19)}) {
        world.LoadBlocking(place);
        const glm::ivec3 window = world.WindowMinChunk();
        const uint64_t revision = world.GridRevision();
        const std::array<glm::dvec3, 2> players = {place, place + glm::dvec3(30.0, 0.0, 0.0)};
        for (int i = 0; i < 5; ++i) simulation.Step(players);    // focus points move colliders, never the window
        simulation.UpdateStreaming(players);
        EXPECT_EQ(world.WindowMinChunk(), window);
        EXPECT_EQ(world.GridRevision(), revision);               // nothing was unloaded or reloaded
        EXPECT_EQ(world.Stats().pendingChunks, 0u);
    }

    // The other streaming call is the one that moves it (a client following its camera).
    const glm::ivec3 before = world.WindowMinChunk();
    simulation.UpdateStreaming(glm::dvec3(40.0, 8.0, -25.0), {});
    EXPECT_NE(world.WindowMinChunk(), before);
    EXPECT_EQ(world.WindowMinChunk(), voxel::VoxelToChunk(world.WorldToVoxel(glm::dvec3(40.0, 8.0, -25.0))) - world.WindowSize() / 2);
}

TEST(Simulation, OfflineWorldKeepsItsEditsInTheSaveDirectory) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "vortex_sim_test_save";
    std::filesystem::remove_all(directory);
    sim::SimulationConfig config = SmallConfig();
    config.world.saveDirectory = directory;
    const glm::ivec3 marker(1, kGroundVoxels, 2);                // the first voxel of air above the ground
    {
        sim::Simulation simulation(config);
        EXPECT_EQ(simulation.Config().world.saveDirectory, directory);
        ASSERT_TRUE(simulation.Initialize(FlatGround()));
        // What the editor's "Save world" button is enabled by.
        EXPECT_EQ(simulation.World().Config().saveDirectory, directory);
        simulation.World().LoadBlocking(kFocus);
        ASSERT_EQ(simulation.World().GetVoxel(marker), 0);
        EXPECT_GT(simulation.World().FillBox(glm::ivec3(0, kGroundVoxels, 0), glm::ivec3(4, kGroundVoxels + 4, 4), simulation.WorldMaterialIds().stone), 0u);
        simulation.World().Save();
        EXPECT_TRUE(std::filesystem::exists(directory));
        EXPECT_FALSE(std::filesystem::is_empty(directory));
    }
    {
        // The next session on the same directory starts from the edited terrain.
        sim::Simulation simulation(config);
        ASSERT_TRUE(simulation.Initialize(FlatGround()));
        simulation.World().LoadBlocking(kFocus);
        EXPECT_EQ(simulation.World().GetVoxel(marker), simulation.WorldMaterialIds().stone);
    }
    std::filesystem::remove_all(directory);
}

TEST(Simulation, ServerWorldIsMemoryOnlyWhateverTheConfigurationAsks) {
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    const std::filesystem::path directory = temp / "vortex_sim_test_server_save";
    const std::filesystem::path workingDirectory = temp / "vortex_sim_test_server_cwd";
    std::filesystem::remove_all(directory);
    std::filesystem::remove_all(workingDirectory);
    std::filesystem::create_directories(workingDirectory);

    // An empty working directory, so that a save to some default place would show up there.
    struct WorkingDirectory {
        explicit WorkingDirectory(const std::filesystem::path& path) : previous(std::filesystem::current_path()) { std::filesystem::current_path(path); }
        ~WorkingDirectory() { std::filesystem::current_path(previous); }
        std::filesystem::path previous;
    };
    {
        const WorkingDirectory cwd(workingDirectory);
        sim::SimulationConfig config = SmallConfig();
        config.world.saveDirectory = directory;
        sim::Simulation simulation(sim::ServerConfig(config));
        EXPECT_TRUE(simulation.Config().world.saveDirectory.empty());
        ASSERT_TRUE(simulation.Initialize(FlatGround()));
        EXPECT_TRUE(simulation.World().Config().saveDirectory.empty());

        // A modified chunk is what a persistent world would write, on Save() and when it goes away.
        simulation.World().LoadBlocking(kFocus);
        EXPECT_GT(simulation.World().FillBox(glm::ivec3(0, kGroundVoxels, 0), glm::ivec3(4, kGroundVoxels + 4, 4), simulation.WorldMaterialIds().stone), 0u);
        simulation.World().Save();
    }
    EXPECT_FALSE(std::filesystem::exists(directory));
    EXPECT_TRUE(std::filesystem::is_empty(workingDirectory));
    std::filesystem::remove_all(workingDirectory);
}

// --- configuration and lifetime of the object ----------------------------------------------------

TEST(Simulation, SceneUsesTheVoxelSizeOfTheWorld) {
    sim::SimulationConfig config = SmallConfig();
    config.world.voxelSize = 0.25f;
    config.scene.voxelSize = 0.1f;                               // overwritten
    sim::Simulation simulation(config);
    EXPECT_FLOAT_EQ(simulation.Config().scene.voxelSize, 0.25f);
    EXPECT_FLOAT_EQ(simulation.Scene().Config().voxelSize, 0.25f);
}

TEST(Simulation, ServerConfigChangesOnlyWhatAServerMustNotHave) {
    sim::SimulationConfig base = SmallConfig();
    base.scene.minPieceVoxels = 20;
    base.fallOutDepth = 33.0f;
    base.world.saveDirectory = "somewhere";
    const sim::SimulationConfig server = sim::ServerConfig(base);
    EXPECT_EQ(server.maxParticles, 0u);
    EXPECT_TRUE(server.world.saveDirectory.empty());
    EXPECT_FALSE(server.scene.bakeModels);
    EXPECT_FALSE(server.scene.debrisRemnants);
    EXPECT_FALSE(server.scene.markDebris);
    // Everything else is kept.
    EXPECT_EQ(server.chunkCapacity, base.chunkCapacity);
    EXPECT_EQ(server.scene.minPieceVoxels, 20u);
    EXPECT_FLOAT_EQ(server.fallOutDepth, 33.0f);
    EXPECT_FLOAT_EQ(server.fixedTimeStep, base.fixedTimeStep);
    EXPECT_EQ(server.world.windowSize, base.world.windowSize);
    EXPECT_EQ(server.physics.threads, base.physics.threads);

    // The defaults are those of a game that draws.
    const sim::SimulationConfig offline;
    EXPECT_GT(offline.maxParticles, 0u);
    EXPECT_TRUE(offline.scene.bakeModels);
    EXPECT_TRUE(offline.scene.markDebris);
    EXPECT_TRUE(offline.scene.debrisRemnants);
}

TEST(Simulation, DoesNothingBeforeInitializeAndAfterShutdown) {
    sim::Simulation simulation(SmallConfig());
    const std::array<glm::dvec3, 1> focus = {kFocus};
    EXPECT_FALSE(simulation.IsInitialized());
    simulation.Step(focus);
    simulation.UpdateStreaming(focus);
    simulation.UpdateStreaming(kFocus, focus);
    simulation.StepPhysics();
    simulation.UpdateLifetimes(1.0f);
    EXPECT_EQ(simulation.Tick(), 0u);

    ASSERT_TRUE(simulation.Initialize(FlatGround()));
    EXPECT_TRUE(simulation.IsInitialized());
    EXPECT_TRUE(simulation.Initialize(FlatGround()));            // already initialised: nothing happens
    simulation.World().LoadBlocking(kFocus);
    voxel::VoxelVolume volume(glm::ivec3(8));
    volume.FillBox(glm::ivec3(0), glm::ivec3(8), simulation.WorldMaterialIds().wood);
    const Entity crate = simulation.Scene().CreateVoxelObject(
        {.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette), .position = {0.0f, kGround + 1.0f, 0.0f}});
    ASSERT_NE(crate, kNoEntity);
    simulation.Step(focus);
    simulation.Step(focus);
    EXPECT_EQ(simulation.Tick(), 2u);
    EXPECT_GT(simulation.Chunks().UsedCount(), 0u);

    simulation.Shutdown();
    EXPECT_FALSE(simulation.IsInitialized());
    EXPECT_FALSE(simulation.Scene().IsValid(crate));             // entities, bodies and chunk slots are gone
    EXPECT_EQ(simulation.Scene().EntityCount(), 0u);
    EXPECT_EQ(simulation.Physics().BodyCount(), 0u);
    EXPECT_EQ(simulation.Chunks().UsedCount(), 0u);
    simulation.Step(focus);
    simulation.StepPhysics();
    simulation.UpdateLifetimes(1.0f);
    EXPECT_EQ(simulation.Tick(), 2u);                            // kept, not advanced
    simulation.Shutdown();                                       // twice is harmless; the destructor is the third time
}
