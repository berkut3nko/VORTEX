// Physics and budget of what a blast frees (docs/design/shred.md stages 1d and 1e): a detached
// piece keeps the blast's push although the terrain collider still holds its former voxels, and
// rubble of every size has a lifetime and caps on bodies and chunk-store slots; a piece that
// does not fit becomes particles, never a silent loss.

#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.scene;
import vortex.sim;

using namespace vortex;

namespace {
    constexpr float kVoxel = 0.1f;
    constexpr int g = 80;                              // flat ground: voxels y < 80, its top at 8 m
    const glm::dvec3 kFocus(0.0, 8.0, 0.0);            // window chunks (-3..2, -1..4, -3..2): voxels x, z -96..95
    constexpr uint16_t kStrong = 1600;

    sim::SimulationConfig RubbleConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.maxParticles = 8192;
        config.jobThreads = 3;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = {6, 6, 6};
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        config.checkIslandSearch = true;      // the graph search is compared with the voxel search (shred.md stage 4)
        return config;
    }

    std::shared_ptr<voxel::IWorldGenerator> Ground() {
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
        return voxel::CreateFlatGenerator(g, ids.grass, ids.stone);
    }

    struct RubbleArena {
        explicit RubbleArena(const sim::SimulationConfig& config = RubbleConfig()) : simulation(config) {
            initialized = simulation.Initialize(Ground());
            if (initialized) simulation.World().LoadBlocking(kFocus);
        }
        ~RubbleArena() {
            const sim::Simulation::IslandSearchChecks& checks = simulation.IslandSearchCheck();
            EXPECT_EQ(checks.mismatches, 0u) << "the graph and the voxel island search differed";
            EXPECT_EQ(checks.searches, checks.run) << "island searches run without the comparison (checkIslandSearch lost on the way?)";
        }
        RubbleArena(const RubbleArena&) = delete;
        RubbleArena& operator=(const RubbleArena&) = delete;
        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        /// A frame of the engine: streaming around the focus, one physics tick, lifetimes.
        void Frame() {
            simulation.UpdateStreaming(std::span<const glm::dvec3>(&kFocus, 1));
            simulation.StepPhysics();
            simulation.UpdateLifetimes(simulation.FixedTimeStep());
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
        }
        sim::Simulation simulation;
        bool initialized = false;
    };

    void Apply(voxel::World& world, const sim::WorldOp& op) { ASSERT_TRUE(sim::ApplyOp(world, op).Complete()); }

    /// The entity of the one rubble body made from a Detach (the only voxel object of the scene).
    scene::Entity OnlyVoxelObject(scene::Scene& scene) {
        scene::Entity found = scene::kNoEntity;
        for (const auto [entity, body] : scene.Registry().view<const scene::VoxelBody>().each()) found = entity;
        return found;
    }
}

// --- stage 1d: the push of a detached piece -------------------------------------------------------

// The piece is made where its voxels were, right after the Detach removed them. The collider of
// its chunk still holds those voxels until it is re-cooked: the piece must not be simulated against
// them, but wait, frozen, for the new collider and keep its velocity. Before stage 1d the guard
// counted the stale collider as ready and stepped the piece at once (this failed on IsFrozen); the
// lost push itself was not reproduced in isolation (shred.md "Stages 1d-1e as built").
TEST(Rubble, DetachedPieceWaitsFrozenWithItsPushOverAStaleCollider) {
    RubbleArena a;
    ASSERT_TRUE(a.initialized);
    // A 3 x 14 x 3 pillar in the middle of one chunk (voxels 0..31, rows 64..95), next to the
    // focus; its colliders are built before the blast. Nothing of the blast reaches another
    // chunk, so no neighbour's collider becomes "not ready" (that would hold the piece anyway).
    Apply(a.World(), sim::WorldOp::Box({12, g, 12}, {15, g + 14, 15}, a.Ids().stone));
    a.Frame();
    a.simulation.Terrain().Flush();

    // Cut at the base from the side (centre (9.5, g + 4.5, 13.5) voxels, 6 voxels): the piece is pushed sideways.
    const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(9 * 16 + 8, (g + 4) * 16 + 8, 13 * 16 + 8), 6 * 16}, kStrong);
    ASSERT_EQ(blast.IslandCount(), 1u);
    sim::PresentBlast(a.simulation, blast);
    const scene::Entity piece = OnlyVoxelObject(a.simulation.Scene());
    ASSERT_NE(piece, scene::kNoEntity);
    physics::PhysicsWorld& physics = a.simulation.Physics();
    const physics::BodyId body = a.simulation.Scene().TryGet<scene::RigidBody>(piece)->body;
    const glm::vec3 push = blast.ops[1].op.velocity;
    ASSERT_GT(std::abs(push.x), 0.5f);

    // A blast in a fixed update is followed by the tick's physics step, before any streaming:
    // the guard of that step already sees the edit.
    a.simulation.StepPhysics();
    ASSERT_TRUE(physics.IsFrozen(body)) << "the piece was simulated inside the collider of its own former voxels";
    EXPECT_NEAR(physics.GetState(body).keptLinearVelocity.x, push.x, 1e-4f);
    EXPECT_NEAR(physics.GetState(body).position.y, a.World().VoxelToWorld(blast.ops[1].op.min).y, 1e-4);

    // Velocity the body has or will resume with, after each frame.
    const auto velocity = [&] {
        const physics::BodyState s = physics.GetState(body);
        return s.frozen ? s.keptLinearVelocity : s.linearVelocity;
    };
    for (int frame = 1; frame <= 5; ++frame) {
        a.Frame();
        const glm::vec3 v = velocity();
        EXPECT_NEAR(v.x, push.x, 0.1f) << "the push was lost at +" << frame;
        EXPECT_NEAR(v.z, push.z, 0.1f) << "the push was lost at +" << frame;
    }
    EXPECT_FALSE(physics.IsFrozen(body)) << "the piece waited for colliders that never came";
}

// --- stage 1e: the rubble budget -------------------------------------------------------------------

// shred.md "Tests for stage 1": 20 tower cuts in a row. Every Detach becomes a rubble body or, when
// its caps are full of moving rubble, particles - never nothing; the live rubble and the chunk-store
// slots it holds never exceed the caps. Caps are lowered so that 20 cuts reach them.
TEST(Rubble, TowerCutsStayUnderCaps) {
    sim::SimulationConfig config = RubbleConfig();
    config.scene.maxRubbleBodies = 6;
    config.scene.maxRubbleSlots = 8;
    RubbleArena a(config);
    ASSERT_TRUE(a.initialized);
    scene::Scene& scene = a.simulation.Scene();
    fx::ParticleSystem& particles = a.simulation.Particles();

    uint32_t detaches = 0, entities = 0;
    for (int i = 0; i < 20; ++i) {
        // Towers on a 5 x 4 grid, 1.4 m apart; every other one tall enough to need 3 model chunks.
        const int x = -28 + 14 * (i % 5), z = -21 + 14 * (i / 5);
        const int height = (i % 2) ? 75 : 40;
        Apply(a.World(), sim::WorldOp::Box({x, g, z}, {x + 3, g + height, z + 3}, a.Ids().stone));
        const voxel::SphereQ cut{glm::ivec3((x + 1) * 16 + 8, (g + 4) * 16 + 8, (z + 1) * 16 + 8), 64};
        const sim::BlastResult blast = sim::Blast(a.simulation, cut, kStrong);
        ASSERT_TRUE(blast.islandsSearched);
        ASSERT_EQ(blast.IslandCount(), 1u) << "tower " << i;

        const scene::RubbleStats before = scene.Rubble();
        const size_t objectsBefore = scene.VoxelObjectCount();
        particles.Clear();                                       // nothing updates the pool here
        sim::PresentBlast(a.simulation, blast);
        const uint32_t withPiece = particles.AliveCount();
        const scene::RubbleStats after = scene.Rubble();
        const uint32_t made = static_cast<uint32_t>(scene.VoxelObjectCount() + (after.removedByCap - before.removedByCap) - objectsBefore);
        const uint32_t crumbled = static_cast<uint32_t>((after.crumbledByCap - before.crumbledByCap) + (after.crumbledByBake - before.crumbledByBake));
        EXPECT_EQ(made + crumbled, 1u) << "tower " << i << ": the piece was lost";
        if (crumbled > 0) {
            // The same blast without its Detach shows fewer particles: the piece became particles.
            sim::BlastResult bare = blast;
            bare.ops.resize(1);
            particles.Clear();
            sim::PresentBlast(a.simulation, bare);
            EXPECT_GT(withPiece, particles.AliveCount() + 20u) << "tower " << i;
        }
        detaches += 1;
        entities += made;

        EXPECT_LE(after.bodies, config.scene.maxRubbleBodies) << "tower " << i;
        EXPECT_LE(after.slots, config.scene.maxRubbleSlots) << "tower " << i;
        for (int frame = 0; frame < 60; ++frame) a.Frame();     // a second between shots: some rubble comes to rest
        EXPECT_LE(scene.Rubble().bodies, config.scene.maxRubbleBodies);
        EXPECT_LE(scene.Rubble().slots, config.scene.maxRubbleSlots);
    }
    const scene::RubbleStats end = scene.Rubble();
    EXPECT_EQ(detaches, 20u);
    EXPECT_GT(end.removedByCap, 0u);                             // the caps were reached and made room
    EXPECT_EQ(entities + end.crumbledByCap + end.crumbledByBake, detaches);
    EXPECT_EQ(end.crumbledByBake, 0u);
    // Every rubble body has a lifetime: 20 s (medium) at these sizes.
    for (const auto [entity, debris] : scene.Registry().view<const scene::Debris>().each()) EXPECT_GT(debris.lifetime, 0.0f);
}
