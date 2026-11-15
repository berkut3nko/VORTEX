// sim::StepPlayer: the one movement rule of a server, a predicting client and the offline game
// (docs/design/networking.md "Player movement", stage 4). Integer inputs, a fixed tick, a gate that
// does not wait for re-cooks.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.sim;

using namespace vortex;

namespace {
    constexpr float kVoxel = 0.125f;
    constexpr int kGroundVoxels = 96;                 // flat ground: top at y = 12 m
    constexpr float kGround = kGroundVoxels * kVoxel;

    sim::SimulationConfig PlayerConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.maxParticles = 0;
        config.jobThreads = 2;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = {8, 6, 8};
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        return config;
    }

    std::shared_ptr<voxel::IWorldGenerator> FlatGround() {
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
        return voxel::CreateFlatGenerator(kGroundVoxels, ids.grass, ids.stone);
    }

    /// A simulation on flat ground with one player.
    struct Field {
        explicit Field(glm::vec3 spawn = {0.3f, kGround, 0.3f}) : simulation(PlayerConfig()) {
            initialized = simulation.Initialize(FlatGround());
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(spawn));
            state.position = spawn;
            character = simulation.Physics().CreateCharacter(sim::PlayerCharacterDesc(spawn, tuning));
        }

        void Cook() {
            const std::array<glm::dvec3, 1> focus = {glm::dvec3(state.position)};
            simulation.Terrain().Update(focus, simulation.Config().colliderRadius);
            simulation.Terrain().Flush();
        }

        /// One tick in the server's order; cooking gets the time a real tick would give it.
        sim::PlayerStepResult Tick(const sim::PlayerInput& input) {
            const std::array<glm::dvec3, 1> focus = {glm::dvec3(state.position)};
            simulation.UpdateStreaming(focus);
            const sim::PlayerStepResult r =
                sim::StepPlayer(simulation.Physics(), simulation.Terrain(), character, state, input, tuning, simulation.FixedTimeStep());
            simulation.StepPhysics();
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
            return r;
        }

        sim::Simulation simulation;
        sim::PlayerTuning tuning;
        sim::PlayerState state;
        physics::CharacterId character;
        bool initialized = false;
    };

    sim::PlayerInput Walk(float yaw, bool run = false, uint8_t extra = 0) {
        sim::PlayerInput in;
        in.moveZ = 127;
        in.yaw = sim::QuantizeYaw(yaw);
        in.buttons = static_cast<uint8_t>((run ? sim::PlayerButton_Run : 0) | extra);
        return in;
    }

    /// A walk that turns, runs, strafes and jumps: the same on every call.
    sim::PlayerInput Script(int i) {
        sim::PlayerInput in;
        in.moveZ = static_cast<int8_t>(i % 90 < 70 ? 127 : -127);
        in.moveX = static_cast<int8_t>(i % 50 < 20 ? 127 : 0);
        in.yaw = static_cast<uint16_t>(i * 97);
        in.buttons = static_cast<uint8_t>((i % 120 < 60 ? sim::PlayerButton_Run : 0) | (i % 45 == 10 ? sim::PlayerButton_Jump : 0));
        return in;
    }
}

TEST(Player, InputsQuantiseRadiansAndAxes) {
    for (const float yaw : {0.0f, 0.6f, -1.5707963f, 3.1f, 6.2f, -12.0f}) {
        const float back = sim::YawRadians(sim::QuantizeYaw(yaw));
        const float diff = std::remainder(back - yaw, 2.0f * std::numbers::pi_v<float>);
        EXPECT_LT(std::abs(diff), 1.0e-4f) << yaw;
    }
    EXPECT_EQ(sim::QuantizeYaw(2.0f * std::numbers::pi_v<float>), 0u);
    EXPECT_EQ(sim::QuantizePitch(10.0f), 32767);
    EXPECT_EQ(sim::QuantizePitch(-10.0f), -32767);
    EXPECT_NEAR(sim::PitchRadians(sim::QuantizePitch(-1.2f)), -1.2f, 1.0e-4f);
    EXPECT_NEAR(sim::PitchRadians(-32768), -0.5f * std::numbers::pi_v<float>, 1.0e-6f);
    EXPECT_EQ(sim::QuantizeAxis(1.5f), 127);
    EXPECT_EQ(sim::QuantizeAxis(-1.0f), -127);
    EXPECT_EQ(sim::QuantizeAxis(std::nanf("")), 0);

    // The look vector is the camera's Forward(); the move vector its horizontal part, diagonals normalised.
    const float yaw = 0.6f, pitch = -0.3f;
    const glm::vec3 look = sim::LookDirection(sim::QuantizeYaw(yaw), sim::QuantizePitch(pitch));
    const glm::vec3 camera(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
    EXPECT_LT(glm::length(look - camera), 1.0e-4f);
    sim::PlayerInput in = Walk(yaw);
    EXPECT_NEAR(glm::length(sim::MoveDirection(in)), 1.0f, 1.0e-6f);
    EXPECT_LT(glm::length(sim::MoveDirection(in) - glm::vec3(-std::sin(yaw), 0.0f, -std::cos(yaw))), 1.0e-4f);
    in.moveX = -128;
    EXPECT_NEAR(glm::length(sim::MoveDirection(in)), 1.0f, 1.0e-6f);
    in.moveZ = 0;
    in.moveX = 64;
    EXPECT_NEAR(glm::length(sim::MoveDirection(in)), 64.0f / 127.0f, 1.0e-6f);

    std::string why;
    EXPECT_TRUE(sim::IsValid(sim::PlayerTuning{}, &why)) << why;
    sim::PlayerTuning bad;
    bad.runSpeed = std::nanf("");
    EXPECT_FALSE(sim::IsValid(bad));
    bad = {};
    bad.eyeHeight = 3.0f;
    EXPECT_FALSE(sim::IsValid(bad));
}

// Buttons are levels; edges come from the levels before. A repeated input never makes an edge, and
// the cooldown takes one cast in castCooldownTicks.
TEST(Player, EdgesComeFromLevelsAndTheCooldownTakesOneCast) {
    sim::PlayerState s;
    sim::PlayerTuning t;
    EXPECT_EQ(sim::PressButtons(s, sim::PlayerButton_Cast | sim::PlayerButton_Run), sim::PlayerButton_Cast | sim::PlayerButton_Run);
    EXPECT_EQ(sim::PressButtons(s, sim::PlayerButton_Cast | sim::PlayerButton_Run), 0);      // repeated: no edge
    EXPECT_EQ(sim::PressButtons(s, sim::PlayerButton_Run | 0xF0), 0);                         // unknown bits ignored
    EXPECT_EQ(s.lastButtons, sim::PlayerButton_Run);
    EXPECT_EQ(sim::PressButtons(s, sim::PlayerButton_Cast | sim::PlayerButton_Jump), sim::PlayerButton_Cast | sim::PlayerButton_Jump);

    EXPECT_TRUE(sim::TakeCast(s, sim::PlayerButton_Cast, t));
    EXPECT_EQ(s.cooldownTicks, t.castCooldownTicks);
    EXPECT_FALSE(sim::TakeCast(s, sim::PlayerButton_Cast, t));
    EXPECT_FALSE(sim::TakeCast(s, sim::PlayerButton_Jump, t));
}

TEST(Player, WalksRunsJumpsAndIsHeldWithoutACollider) {
    Field f;
    ASSERT_TRUE(f.initialized);
    // No collider cooked yet: held, nothing moves, but edges and the cooldown still count.
    sim::PlayerInput first = Walk(0.0f, false, sim::PlayerButton_Cast);
    const sim::PlayerStepResult held =
        sim::StepPlayer(f.simulation.Physics(), f.simulation.Terrain(), f.character, f.state, first, f.tuning, f.simulation.FixedTimeStep());
    EXPECT_TRUE(held.held);
    EXPECT_TRUE(held.cast);
    EXPECT_TRUE(f.state.flags & sim::PlayerFlag_Held);
    EXPECT_EQ(f.state.position, glm::vec3(0.3f, kGround, 0.3f));

    f.Cook();
    for (int i = 0; i < 10; ++i) f.Tick(sim::PlayerInput{});
    ASSERT_TRUE(f.state.OnGround());
    EXPECT_FALSE(f.state.flags & sim::PlayerFlag_Held);
    EXPECT_NEAR(f.state.position.y, kGround, 0.03f);

    // 60 ticks at walk speed along -z, then 60 at run speed along +x (yaw -pi/2).
    glm::vec3 start = f.state.position;
    for (int i = 0; i < 60; ++i) f.Tick(Walk(0.0f));
    EXPECT_NEAR(start.z - f.state.position.z, f.tuning.walkSpeed, 0.1f);
    start = f.state.position;
    for (int i = 0; i < 60; ++i) f.Tick(Walk(-0.5f * std::numbers::pi_v<float>, true));
    EXPECT_NEAR(f.state.position.x - start.x, f.tuning.runSpeed, 0.15f);
    EXPECT_NEAR(f.state.position.y, kGround, 0.03f);

    // A jump is an edge: holding the button does not jump again on landing.
    float highest = 0.0f;
    bool jumped = false;
    for (int i = 0; i < 120; ++i) {
        const sim::PlayerStepResult r = f.Tick(Walk(0.0f, false, sim::PlayerButton_Jump));
        jumped = jumped || r.jumped;
        highest = std::max(highest, f.state.position.y - kGround);
        if (i == 80) {
            EXPECT_TRUE(f.state.OnGround());
            EXPECT_NEAR(f.state.position.y, kGround, 0.03f);
        }
    }
    EXPECT_TRUE(jumped);
    EXPECT_NEAR(highest, f.tuning.jumpSpeed * f.tuning.jumpSpeed / (2.0f * 9.81f), 0.12f);
    EXPECT_TRUE(f.state.OnGround());
}

// The determinism a reconciliation needs: two simulations stepped with the same inputs (one with
// one worker, one with three, so colliders are cooked in another order) stay within a hair of each
// other over a walk across chunk borders with jumps; restoring one from the other every 30 ticks and
// replaying (what a correction does) keeps it there.
TEST(Player, SameInputsGiveTheSameWalkAndAReplayFromARestoredStateStaysClose) {
    sim::SimulationConfig one = PlayerConfig(), three = PlayerConfig();
    one.jobThreads = 1;
    three.jobThreads = 3;
    struct Side {
        explicit Side(const sim::SimulationConfig& c) : simulation(c) {}
        sim::Simulation simulation;
        sim::PlayerState state;
        physics::CharacterId character;
    };
    Side a(one), b(three);
    const sim::PlayerTuning tuning;
    for (Side* s : {&a, &b}) {
        ASSERT_TRUE(s->simulation.Initialize(FlatGround()));
        s->simulation.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
        s->state.position = glm::vec3(0.3f, kGround, 0.3f);
        s->character = s->simulation.Physics().CreateCharacter(sim::PlayerCharacterDesc(s->state.position, tuning));
        const std::array<glm::dvec3, 1> focus = {glm::dvec3(s->state.position)};
        s->simulation.Terrain().Update(focus, 12.0f);
        s->simulation.Terrain().Flush();
    }
    auto tick = [&tuning](Side& s, const sim::PlayerInput& in) {
        const std::array<glm::dvec3, 1> focus = {glm::dvec3(s.state.position)};
        s.simulation.UpdateStreaming(focus);
        sim::StepPlayer(s.simulation.Physics(), s.simulation.Terrain(), s.character, s.state, in, tuning, s.simulation.FixedTimeStep());
        s.simulation.StepPhysics();
        s.simulation.Terrain().Flush();
    };
    float worst = 0.0f;
    for (int i = 0; i < 600; ++i) {
        tick(a, Script(i));
        tick(b, Script(i));
        worst = std::max(worst, glm::length(a.state.position - b.state.position));
    }
    EXPECT_LT(worst, 1.0e-4f);
    EXPECT_FALSE(a.state.flags & sim::PlayerFlag_Held);
    EXPECT_GT(glm::length(glm::vec2(a.state.position.x, a.state.position.z)), 2.0f);   // it went somewhere

    // B is restored from A's state every 30 ticks, then both go on with the same inputs.
    float worstReplay = 0.0f;
    for (int i = 600; i < 1200; ++i) {
        if (i % 30 == 0) {
            b.state = a.state;
            sim::RestorePlayer(b.simulation.Physics(), b.character, b.state);
        }
        tick(a, Script(i));
        tick(b, Script(i));
        worstReplay = std::max(worstReplay, glm::length(a.state.position - b.state.position));
    }
    EXPECT_LT(worstReplay, 0.02f);
    std::printf("Two simulations: %.2e m apart at most, %.2e m with a restore every 30 ticks\n", worst, worstReplay);
}
