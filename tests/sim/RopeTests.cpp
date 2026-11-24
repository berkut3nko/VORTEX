// Ropes in the simulation ([mech-2] offline part, docs/design/mechanisms.md "Ropes with physics", "Rope actions"):
// a load rope holds a body on one distance constraint, casts cut ropes by ray and sphere, a lost anchor or load cuts
// them, E on a cleat releases one, a cut chain jams its portcullis. GPU-free.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;

using namespace vortex;
using scene::Entity;
using scene::kNoEntity;

namespace {
    constexpr float kRopeVoxel = 0.1f;

    sim::SimulationConfig RopeYardConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.maxParticles = 0;
        config.jobThreads = 2;
        config.world.voxelSize = kRopeVoxel;
        config.world.windowSize = {8, 6, 8};
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        return config;
    }

    /// Flat stone ground (top at y = 10 m) and a stone beam at y 14.0 .. 14.2 m over x -2 .. 2 m, z -0.2 .. 0.2 m.
    struct RopeYard {
        RopeYard() : simulation(RopeYardConfig()) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(100, ids.stone, ids.stone));
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(0.0, 10.0, 0.0));
            EXPECT_TRUE(sim::ApplyOp(simulation.World(), sim::WorldOp::Box({-20, 140, -2}, {20, 142, 2}, simulation.WorldMaterialIds().stone)).Complete());
            const std::array<glm::dvec3, 1> focus = {glm::dvec3(0.0, 10.0, 0.0)};
            simulation.UpdateStreaming(focus);
            simulation.Terrain().Flush();
        }
        RopeYard(const RopeYard&) = delete;
        RopeYard& operator=(const RopeYard&) = delete;

        scene::Scene& Scene() { return simulation.Scene(); }
        /// A crate (8^3 voxels) hung 2 m below the anchor voxel (x, 140, 0) by its top centre.
        Entity HangCrate(int anchorX, uint8_t flags, Entity* load = nullptr) {
            const auto model = sim::CrateModel(simulation.WorldMaterialIds(), sim::PlaygroundScale(kRopeVoxel));
            const glm::dvec3 anchor = sim::RopeAnchorPoint(simulation.World(), {anchorX, 140, 0});
            const Entity crate = Scene().CreateVoxelObject({.model = model, .position = glm::vec3(anchor) - glm::vec3(0.4f, 2.8f, 0.4f)});
            if (load) *load = crate;
            return sim::CreateLoadRope(simulation, {anchorX, 140, 0}, crate, glm::vec3(4.0f, 8.0f, 4.0f), flags);
        }
        void Step(int ticks) {
            const std::array<glm::dvec3, 1> focus = {glm::dvec3(0.0, 10.0, 0.0)};
            for (int i = 0; i < ticks; ++i) {
                sim::StepRopes(simulation, simulation.Tick(), &cuts);
                simulation.Step(focus);
            }
        }
        glm::dvec3 Position(Entity e) { return glm::dvec3(Scene().TryGet<scene::Transform>(e)->current.position); }
        const scene::Rope& RopeOf(Entity rope) { return Scene().Registry().get<scene::Rope>(rope); }

        sim::Simulation simulation;
        bool initialized = false;
        std::vector<sim::RopeCutEvent> cuts;
    };
}

TEST(Rope, LoadHangsAtItsLength) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    Entity crate = kNoEntity;
    const Entity rope = y.HangCrate(0, 0, &crate);
    ASSERT_NE(rope, kNoEntity);
    EXPECT_NEAR(y.RopeOf(rope).length, 2.0f, 1.0e-3f);
    const glm::dvec3 start = y.Position(crate);
    y.Step(120);
    EXPECT_NEAR(y.Position(crate).y, start.y, 0.02) << "the rope holds it up";
    // Pushed sideways, it swings and stays within the rope's length of the anchor.
    y.simulation.Physics().AddImpulse(y.Scene().TryGet<scene::RigidBody>(crate)->body, glm::vec3(200.0f, 0.0f, 0.0f));
    for (int i = 0; i < 60; ++i) {
        y.Step(1);
        glm::dvec3 a, b;
        ASSERT_TRUE(sim::RopeEnds(y.simulation, rope, a, b));
        EXPECT_LE(glm::length(b - a), 2.0 + 0.02) << i;
    }
    EXPECT_TRUE(y.cuts.empty());
}

TEST(Rope, DirectShotCutsAHangingRope) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    Entity crate = kNoEntity;
    const Entity rope = y.HangCrate(0, 0, &crate);
    ASSERT_NE(rope, kNoEntity);
    y.Step(5);
    // A shot across the rope's middle (y 13.0), from 4 m south, aimed 5 cm beside it.
    std::vector<sim::RopeCutEvent> cuts;
    EXPECT_EQ(sim::CutRopesAlongRay(y.simulation, glm::dvec3(0.1, 13.0, 4.0), glm::vec3(0.0f, 0.0f, -1.0f), 20.0f, 10, &cuts), 1u);
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_EQ(cuts[0].cause, sim::RopeCutCause::Ray);
    EXPECT_NEAR(cuts[0].fraction, 0.5f, 0.05f);
    EXPECT_TRUE(y.RopeOf(rope).cut);
    EXPECT_FALSE(y.simulation.Physics().IsValid(y.RopeOf(rope).constraint));
    const double before = y.Position(crate).y;
    y.Step(40);
    EXPECT_LT(y.Position(crate).y, before - 0.5) << "the crate falls";
}

TEST(Rope, SkyBehindStillCutsAndMissesStayUncut) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    const Entity rope = y.HangCrate(0, 0);
    ASSERT_NE(rope, kNoEntity);
    EXPECT_EQ(sim::CutRopesAlongRay(y.simulation, glm::dvec3(0.4, 13.0, 4.0), glm::vec3(0.0f, 0.0f, -1.0f), 60.0f, 10), 0u)
        << "0.35 m beside it: a miss";
    EXPECT_FALSE(y.RopeOf(rope).cut);
    EXPECT_EQ(sim::CutRopesAlongRay(y.simulation, glm::dvec3(0.05, 13.0, 4.0), glm::vec3(0.0f, 0.0f, -1.0f), 60.0f, 10), 1u)
        << "a ray into the sky behind the rope cuts it";
}

TEST(Rope, RopeDoesNotStopTheCast) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    const Entity rope = y.HangCrate(0, 0);
    ASSERT_NE(rope, kNoEntity);
    // A stone wall 1.5 m behind the rope; the cast's ray passes the rope and hits the wall.
    ASSERT_TRUE(sim::ApplyOp(y.simulation.World(), sim::WorldOp::Box({-20, 100, -20}, {20, 140, -15}, y.simulation.WorldMaterialIds().stone))
                    .Complete());
    sim::ActionRequest request;
    request.eye = glm::dvec3(0.05, 13.0, 4.0);
    request.dir = glm::vec3(0.0f, 0.0f, -1.0f);
    sim::ActionAim aim;
    ASSERT_EQ(sim::AimAction(y.simulation, request, sim::OfflineAimRules(), aim), sim::ActionRefusal::None);
    EXPECT_NEAR(aim.point.z, -1.5, 0.01) << "the wall takes the cast";
    const sim::ActionOutcome outcome = sim::ResolveAction(y.simulation, sim::RulesContext{sim::PlayerTuning{}, {}, 10}, request, aim);
    ASSERT_EQ(outcome.blasts.size(), 1u);
    ASSERT_EQ(outcome.ropeCuts.size(), 1u);
    EXPECT_EQ(outcome.ropeCuts[0].rope, rope);
    EXPECT_TRUE(y.RopeOf(rope).cut);
}

TEST(Rope, SphereCutsARope) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    const Entity rope = y.HangCrate(0, 0);
    ASSERT_NE(rope, kNoEntity);
    const auto sphere = [](const glm::dvec3& c, double r) { return voxel::QuantizeSphere(c / static_cast<double>(kRopeVoxel), r / kRopeVoxel); };
    EXPECT_EQ(sim::CutRopesBySphere(y.simulation, sphere({1.5, 13.0, 0.0}, 1.2), 10), 0u) << "1.45 m away";
    EXPECT_FALSE(y.RopeOf(rope).cut);
    std::vector<sim::RopeCutEvent> cuts;
    EXPECT_EQ(sim::CutRopesBySphere(y.simulation, sphere({1.0, 13.5, 0.0}, 1.2), 10, &cuts), 1u);
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_EQ(cuts[0].cause, sim::RopeCutCause::Sphere);
    EXPECT_NEAR(cuts[0].fraction, 0.25f, 0.05f) << "the nearest point of the rope";
}

TEST(Rope, AnchorAirOrLoadGoneCutsTheRope) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    const Entity first = y.HangCrate(0, 0);
    Entity crate = kNoEntity;
    const Entity second = y.HangCrate(10, 0, &crate);
    ASSERT_NE(first, kNoEntity);
    ASSERT_NE(second, kNoEntity);
    ASSERT_TRUE(sim::ApplyOp(y.simulation.World(), sim::WorldOp::Box({-1, 140, -2}, {2, 142, 2}, 0)).Complete());
    y.Scene().Destroy(crate);
    y.Step(1);
    ASSERT_EQ(y.cuts.size(), 2u);
    EXPECT_EQ(y.cuts[0].rope, first);
    EXPECT_EQ(y.cuts[0].cause, sim::RopeCutCause::Anchor);
    EXPECT_FLOAT_EQ(y.cuts[0].fraction, 0.0f);
    EXPECT_EQ(y.cuts[1].rope, second);
    EXPECT_EQ(y.cuts[1].cause, sim::RopeCutCause::LoadGone);
    EXPECT_FLOAT_EQ(y.cuts[1].fraction, 1.0f);
}

TEST(Rope, CleatReleasesWithE) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    Entity crate = kNoEntity;
    const Entity rope = y.HangCrate(0, scene::RopeFlag_Cleat, &crate);
    const Entity plain = y.HangCrate(10, 0);
    ASSERT_NE(rope, kNoEntity);
    ASSERT_NE(plain, kNoEntity);
    EXPECT_FALSE(sim::IsHoldable(y.simulation, crate, false)) << "a body on a rope is not a pick-up";

    // The eye 1.5 m south of the anchor, looking at it.
    const glm::dvec3 anchor = sim::RopeAnchorPoint(y.simulation.World(), {0, 140, 0});
    const glm::dvec3 eye = anchor + glm::dvec3(0.0, -0.8, 1.5);
    const glm::vec3 dir = glm::normalize(glm::vec3(anchor - eye));
    sim::UseTarget t;
    ASSERT_TRUE(sim::FindUseTarget(y.simulation, eye, dir, 10, t));
    EXPECT_EQ(t.kind, sim::UseTargetKind::Rope);
    EXPECT_EQ(t.entity, rope);
    bool greyed = true;
    EXPECT_EQ(sim::UsePrompt(y.simulation, t, 10, greyed), "[E] Release rope");
    EXPECT_FALSE(greyed);

    std::vector<sim::RopeCutEvent> cuts;
    EXPECT_EQ(sim::ReleaseRope(y.simulation, rope, 10, &cuts), sim::UseResult::None);
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_EQ(cuts[0].cause, sim::RopeCutCause::Released);
    EXPECT_FLOAT_EQ(cuts[0].fraction, 0.0f);
    EXPECT_EQ(sim::ReleaseRope(y.simulation, rope, 30, &cuts), sim::UseResult::NoTarget) << "a cut rope is no target";
    EXPECT_EQ(sim::ReleaseRope(y.simulation, plain, 30, &cuts), sim::UseResult::NoTarget) << "no cleat";
    sim::UseTarget after;
    EXPECT_FALSE(sim::FindUseTarget(y.simulation, eye, dir, 11, after) && after.kind == sim::UseTargetKind::Rope);
    EXPECT_TRUE(sim::IsHoldable(y.simulation, crate, false)) << "released, it can be picked up";
}

TEST(Rope, CutChainJamsTheGate) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    // A portcullis (16 x 24 x 1 voxels) raised 2 m, its chain from the beam to its top.
    scene::MechanismDesc d;
    d.kind = scene::MechanismKind::Portcullis;
    d.axis = 2;
    d.minPos = 0;
    d.maxPos = 320;
    d.openTicks = 180;
    d.closeTicks = 45;
    d.useBox = d.blockBox = {glm::ivec3(0), glm::ivec3(16, 24, 1)};
    d.hingeBox = {glm::ivec3(0, 23, 0), glm::ivec3(16, 24, 1)};
    d.pushAxis = 4;
    voxel::VoxelVolume volume(glm::ivec3(16, 24, 1));
    volume.FillBox(glm::ivec3(0), glm::ivec3(16, 24, 1), y.simulation.WorldMaterialIds().wood);
    const auto model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, "gate");
    const Entity gate = y.Scene().CreateVoxelObject({.model = model, .position = {-0.8f, 10.0f, 1.0f}, .motion = physics::MotionType::Kinematic});
    ASSERT_TRUE(sim::MakeMechanism(y.simulation, gate, d, {-8, 100, 10}, 0, 320, 0));
    const Entity chain = sim::CreateChainRope(y.simulation, {0, 140, 0}, gate, glm::vec3(8.0f, 24.0f, 0.5f));
    ASSERT_NE(chain, kNoEntity);
    EXPECT_GT(y.RopeOf(chain).length, 0.5f);

    std::vector<sim::RopeCutEvent> cuts;
    sim::CutRope(y.simulation, chain, 0.5f, sim::RopeCutCause::Ray, 100, &cuts);
    const scene::Mechanism& m = *y.Scene().TryGet<scene::Mechanism>(gate);
    EXPECT_EQ(m.track.Newest().targetPos, 0) << "the gate drops";
    EXPECT_EQ(m.track.Newest().startTick, 101u);
    EXPECT_NE(m.track.Newest().flags & scene::MechanismFlag_Jammed, 0) << "and jams";
    EXPECT_EQ(sim::PositionAt(m, 101 + 45), 0);
}

TEST(Rope, DamagedLoadKeepsItsRope) {
    RopeYard y;
    ASSERT_TRUE(y.initialized);
    Entity crate = kNoEntity;
    const Entity rope = y.HangCrate(0, 0, &crate);
    ASSERT_NE(rope, kNoEntity);
    y.Step(2);
    // A dent at the crate's bottom corner: the body is re-made, the attach point (top centre) stays.
    const glm::dvec3 corner = y.Position(crate) + glm::dvec3(0.05, 0.05, 0.05);
    ASSERT_GT(y.Scene().Damage(crate, corner, 0.25f).removed, 0u);
    y.Step(30);
    EXPECT_FALSE(y.RopeOf(rope).cut);
    EXPECT_TRUE(y.simulation.Physics().IsValid(y.RopeOf(rope).constraint));
    // A hit at the attach point: the rope loses its hold.
    glm::dvec3 a, b;
    ASSERT_TRUE(sim::RopeEnds(y.simulation, rope, a, b));
    y.Scene().Damage(crate, b - glm::dvec3(0.0, 0.05, 0.0), 0.3f);
    y.Step(1);
    EXPECT_TRUE(y.RopeOf(rope).cut);
}
