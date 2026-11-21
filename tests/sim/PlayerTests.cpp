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

        /// A session's play area: the area and its walls (sim::SetPlayAreaWalls), as a server sets them.
        void Bound(glm::vec2 min, glm::vec2 max) {
            area.min = min;
            area.max = max;
            sim::SetPlayAreaWalls(simulation.Physics(), area, tuning);
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
                sim::StepPlayer(simulation.Physics(), simulation.Terrain(), character, state, input, tuning, area, simulation.FixedTimeStep());
            simulation.StepPhysics();
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
            return r;
        }

        sim::Simulation simulation;
        sim::PlayerTuning tuning;
        sim::PlayArea area;                       // unbounded: the offline game
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
        sim::StepPlayer(f.simulation.Physics(), f.simulation.Terrain(), f.character, f.state, first, f.tuning, f.area, f.simulation.FixedTimeStep());
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
        sim::StepPlayer(s.simulation.Physics(), s.simulation.Terrain(), s.character, s.state, in, tuning, sim::PlayArea{}, s.simulation.FixedTimeStep());
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

// WP1: the play area (sim::PlayArea, a session's play bounds less the capsule's radius). The feet
// stop at its edge the way they stop at a wall: the outward wish is cut before Jolt moves the
// character, so nothing is pushed back afterwards; sliding along the edge goes on; walking back is
// never held. Offline (the default, unbounded) nothing changes.
TEST(Player, ThePlayAreaStopsTheFeetAtItsEdgeAndOfflineHasNone) {
    Field f;
    ASSERT_TRUE(f.initialized);
    f.Bound(glm::vec2(-2.0f), glm::vec2(2.0f));
    f.Cook();
    for (int i = 0; i < 10; ++i) f.Tick(sim::PlayerInput{});
    ASSERT_TRUE(f.state.OnGround());
    const float east = -0.5f * std::numbers::pi_v<float>;
    int pushed = 0;
    for (int i = 0; i < 120; ++i) {
        const sim::PlayerStepResult r = f.Tick(Walk(east, true));
        EXPECT_FALSE(r.held);
        pushed += r.pushedBack ? 1 : 0;
        EXPECT_LE(f.state.position.x, 2.0f + 1.0e-6f);
        EXPECT_TRUE(f.state.OnGround()) << i;
    }
    EXPECT_NEAR(f.state.position.x, 2.0f, 2.0e-3f);
    EXPECT_NEAR(f.state.position.y, kGround, 0.03f);
    EXPECT_LE(f.state.velocity.x, 1.0e-3f);
    // Along the edge (north-east: half the wish goes out, half along -z): the feet slide along it.
    const float z0 = f.state.position.z;
    for (int i = 0; i < 30; ++i) f.Tick(Walk(-0.25f * std::numbers::pi_v<float>, true));
    EXPECT_NEAR(f.state.position.x, 2.0f, 2.0e-3f);
    EXPECT_LT(f.state.position.z, z0 - 2.0f);
    // Back west: nothing holds it.
    for (int i = 0; i < 60; ++i) EXPECT_FALSE(f.Tick(Walk(-east, true)).held);
    EXPECT_LT(f.state.position.x, -1.9f);
    EXPECT_GE(f.state.position.x, -2.0f - 1.0e-6f);
    std::printf("Play area: %d steps pushed back\n", pushed);

    // Offline: the same run passes x = 2 m.
    Field offline;
    ASSERT_TRUE(offline.initialized);
    EXPECT_FALSE(offline.area.Bounded());
    offline.Cook();
    for (int i = 0; i < 10; ++i) offline.Tick(sim::PlayerInput{});
    for (int i = 0; i < 120; ++i) offline.Tick(Walk(east, true));
    EXPECT_GT(offline.state.position.x, 10.0f);
}

// Review of the WP1 plan: where the ground falls away outward at the edge (a 45 degree ramp, a 1 m
// step down right under the edge), pressing against the edge must neither sink the feet into the
// ground nor make them bob or lift off: the wish is cut and the walls stop the rest.
TEST(Player, AtThePlayAreasEdgeAFallingGroundNeitherSinksNorBobsTheFeet) {
    for (const bool ramp : {true, false}) {
        Field f;
        ASSERT_TRUE(f.initialized);
        f.Bound(glm::vec2(-2.0f), glm::vec2(2.0f));
        // Ground top (voxels) of column x: the ramp falls 45 degrees from x = 1 m to 4 m; the step
        // drops 1 m at x = 2 m, right under the edge of the area.
        auto top = [ramp](int x) { return ramp ? kGroundVoxels - std::clamp(x - 7, 0, 24) : (x >= 16 ? kGroundVoxels - 8 : kGroundVoxels); };
        for (int x = 8; x < 40; ++x)
            if (top(x) < kGroundVoxels) f.simulation.World().FillBox({x, top(x), -40}, {x + 1, kGroundVoxels, 40}, 0);
        f.Cook();
        for (int i = 0; i < 10; ++i) f.Tick(sim::PlayerInput{});
        ASSERT_TRUE(f.state.OnGround());
        float worstBelow = 0.0f, worstBob = 0.0f, worstStep = 0.0f, previousY = f.state.position.y;
        int pressing = 0, airborne = 0, pushed = 0;
        for (int i = 0; i < 150; ++i) {
            const sim::PlayerStepResult r = f.Tick(Walk(-0.5f * std::numbers::pi_v<float>, false));
            EXPECT_FALSE(r.held);
            pushed += r.pushedBack ? 1 : 0;
            const glm::vec3 p = f.state.position;
            EXPECT_LE(p.x, 2.0f + 1.0e-6f);
            worstStep = std::max(worstStep, std::abs(p.y - previousY));
            // The lowest ground under the capsule's footprint: the feet never go below it by a voxel.
            const int lo = static_cast<int>(std::floor((p.x - f.tuning.radius) / kVoxel));
            const int hi = static_cast<int>(std::floor((p.x + f.tuning.radius) / kVoxel));
            int ground = kGroundVoxels;
            for (int x = lo; x <= hi; ++x) ground = std::min(ground, top(x));
            worstBelow = std::max(worstBelow, static_cast<float>(ground) * kVoxel - p.y);
            if (p.x > 2.0f - 0.01f) {
                // The first ticks at the edge finish the step down the voxel stairs under it.
                if (pressing > 2) worstBob = std::max(worstBob, std::abs(p.y - previousY));
                airborne += f.state.OnGround() ? 0 : 1;
                ++pressing;
            }
            previousY = p.y;
        }
        std::printf("%s: %d ticks pressing at the edge, %d airborne, %d pushed back, bob %.4f m (any tick %.4f m), feet %.4f m below "
                    "the lowest ground under them at worst, end (%.3f, %.3f)\n",
                    ramp ? "Ramp" : "Step", pressing, airborne, pushed, static_cast<double>(worstBob), static_cast<double>(worstStep),
                    static_cast<double>(worstBelow), static_cast<double>(f.state.position.x), static_cast<double>(f.state.position.y));
        EXPECT_GT(pressing, 60);
        EXPECT_LT(worstBelow, kVoxel);
        EXPECT_LT(worstBob, 0.01f);
        EXPECT_LE(worstStep, kVoxel);
        if (ramp) EXPECT_EQ(airborne, 0);
        // Along the edge (south, -z): the feet move on, the edge still holds them.
        const float z0 = f.state.position.z;
        for (int i = 0; i < 60; ++i) {
            f.Tick(Walk(-0.25f * std::numbers::pi_v<float>, false));
            EXPECT_LE(f.state.position.x, 2.0f + 1.0e-6f);
        }
        std::printf("%s: along the edge %.3f m in 60 ticks\n", ramp ? "Ramp" : "Step", static_cast<double>(z0 - f.state.position.z));
        EXPECT_LT(f.state.position.z, z0 - 1.5f);
        // And back: nothing holds it.
        for (int i = 0; i < 60; ++i) EXPECT_FALSE(f.Tick(Walk(0.5f * std::numbers::pi_v<float>, false)).held);
        EXPECT_LT(f.state.position.x, 0.0f);
    }
}

// Review of WP1: a face steeper than maxSlopeDegrees that falls outward across the edge (a crater
// wall a cast leaves). Jolt slid the feet down and out by more than the 1 cm StepPlayer took back,
// so it undid the whole move: the same position and velocity, so the next tick slid the same way and
// was undone again - the player hovered there for good (141 ticks in a row on the 4:1 face). Now the
// walls of the play area stop the capsule: the feet go down the face against the edge, never below
// the ground, never frozen in the air.
TEST(Player, AtThePlayAreasEdgeASteepFaceNeitherHoldsNorSinksTheFeet) {
    for (const int drop : {2, 4}) {   // voxels down per voxel: about 63 and 76 degrees
        Field f;
        ASSERT_TRUE(f.initialized);
        f.Bound(glm::vec2(-2.0f), glm::vec2(2.0f));
        // Ground top (voxels) of column x: the face falls from x = 1.5 m, 6 m down to a floor.
        auto top = [drop](int x) { return kGroundVoxels - std::min(48, drop * std::max(0, x - 11)); };
        for (int x = 12; x < 48; ++x) f.simulation.World().FillBox({x, top(x), -40}, {x + 1, kGroundVoxels, 40}, 0);
        f.Cook();
        for (int i = 0; i < 10; ++i) f.Tick(sim::PlayerInput{});
        ASSERT_TRUE(f.state.OnGround());
        float worstBelow = 0.0f;
        // Frozen in the air: off the ground, and the feet did not move this tick.
        int pushed = 0, frozenInARow = 0, worstInARow = 0;
        auto tick = [&](const sim::PlayerInput& in) {
            const glm::vec3 before = f.state.position;
            const sim::PlayerStepResult r = f.Tick(in);
            EXPECT_FALSE(r.held);
            pushed += r.pushedBack ? 1 : 0;
            const bool frozen = !f.state.OnGround() && f.state.position == before;
            frozenInARow = frozen ? frozenInARow + 1 : 0;
            worstInARow = std::max(worstInARow, frozenInARow);
            const glm::vec3 p = f.state.position;
            EXPECT_LE(p.x, 2.0f + 1.0e-6f);
            const int lo = static_cast<int>(std::floor((p.x - f.tuning.radius) / kVoxel));
            const int hi = static_cast<int>(std::floor((p.x + f.tuning.radius) / kVoxel));
            int ground = kGroundVoxels;
            for (int x = lo; x <= hi; ++x) ground = std::min(ground, top(x));
            worstBelow = std::max(worstBelow, static_cast<float>(ground) * kVoxel - p.y);
        };
        // Run east into the edge, over the top of the face.
        for (int i = 0; i < 60; ++i) tick(Walk(-0.5f * std::numbers::pi_v<float>, true));
        // Let go: the feet come to rest against the edge, on the face, not above it.
        for (int i = 0; i < 120; ++i) tick(sim::PlayerInput{});
        const glm::vec3 rest = f.state.position;
        int highest = 0;
        for (int x = static_cast<int>(std::floor((rest.x - f.tuning.radius) / kVoxel));
             x <= static_cast<int>(std::floor((rest.x + f.tuning.radius) / kVoxel)); ++x)
            highest = std::max(highest, top(x));
        const float above = rest.y - static_cast<float>(highest) * kVoxel;
        std::printf("Face %d:1: rests at (%.3f, %.3f), %.3f m above the highest ground under it, velocity y %.3f, on the ground %d, "
                    "%d pushed back, frozen in the air %d ticks in a row at worst, feet %.4f m below the lowest ground under them at worst\n",
                    drop, static_cast<double>(rest.x), static_cast<double>(rest.y), static_cast<double>(above),
                    static_cast<double>(f.state.velocity.y), f.state.OnGround() ? 1 : 0, pushed, worstInARow, static_cast<double>(worstBelow));
        EXPECT_LT(worstInARow, 2);
        EXPECT_LT(above, 0.05f);
        EXPECT_LT(std::abs(f.state.velocity.y), 0.5f);
        EXPECT_LT(worstBelow, kVoxel);
        // Walk back in: a 2:1 face is a staircase the character climbs (0.25 m risers under its
        // 0.35 m step); a 4:1 face is a pit wall it does not.
        for (int i = 0; i < 120; ++i) tick(Walk(0.5f * std::numbers::pi_v<float>, true));
        std::printf("Face %d:1: walking back ends at (%.3f, %.3f)\n", drop, static_cast<double>(f.state.position.x),
                    static_cast<double>(f.state.position.y));
        EXPECT_LT(worstInARow, 2);
        EXPECT_LT(worstBelow, kVoxel);
        if (drop == 2) EXPECT_LT(f.state.position.x, 0.0f);
    }
}

// sim::PlayAreaOf: the play bounds less the radius, and its reach (PlayerReach) a chunk inside the
// server's window at every session voxel size, where the gate finds colliders.
TEST(Player, PlayAreaOfARegionKeepsTheReachInsideTheWindow) {
    for (const float voxelSize : {0.1f, 0.125f, 0.15f}) {
        sim::RegionDesc region;
        region.templateId = sim::RegionTemplateId::Empty;
        region.playMinChunk = {-4, -5};
        region.playMaxChunk = {3, 2};
        region.heightRange = {0.0f, 32.0f};
        region.generator = sim::RegionGenerator::Flat;
        region.flatGroundVoxels = 64;
        region.terrain.voxelSize = voxelSize;
        {
            voxel::MaterialLibrary scratch;
            region.terrain.materials = voxel::RegisterDefaultWorldMaterials(scratch);
        }
        ASSERT_TRUE(sim::IsValid(region));
        const sim::PlayerTuning tuning;
        const sim::PlayArea area = sim::PlayAreaOf(region, tuning);
        const float chunk = 32.0f * voxelSize;
        EXPECT_TRUE(area.Bounded());
        EXPECT_FLOAT_EQ(area.min.x, -4.0f * chunk + 0.3f);
        EXPECT_FLOAT_EQ(area.min.y, -5.0f * chunk + 0.3f);
        EXPECT_FLOAT_EQ(area.max.x, 3.0f * chunk - 0.3f);
        EXPECT_FLOAT_EQ(area.max.y, 2.0f * chunk - 0.3f);

        sim::SimulationConfig config = PlayerConfig();
        config.chunkCapacity = sim::RegionChunkCapacity(region);
        config.world = sim::RegionWorldConfig(region, config.world);
        sim::Simulation simulation(config);
        ASSERT_TRUE(simulation.Initialize(sim::MakeRegionGenerator(region, region.terrain.materials)));
        simulation.World().LoadBlocking(sim::RegionWindowFocus(region));
        ASSERT_TRUE(sim::WindowIsRegions(simulation.World(), region));
        const glm::vec3 windowMin = glm::vec3(sim::RegionWindowMinChunk(region)) * chunk;
        const glm::vec3 windowMax = windowMin + glm::vec3(sim::RegionWindowSize(region)) * chunk;
        const float y = 64.0f * voxelSize;
        for (const glm::vec2 corner : {area.min, area.max, glm::vec2(area.min.x, area.max.y), glm::vec2(area.max.x, area.min.y)}) {
            const glm::vec3 feet(corner.x, y, corner.y);
            const math::AABB reach = sim::PlayerReach(feet, tuning);
            EXPECT_GE(reach.min.x, windowMin.x + chunk) << voxelSize;
            EXPECT_GE(reach.min.z, windowMin.z + chunk) << voxelSize;
            EXPECT_LE(reach.max.x, windowMax.x - chunk) << voxelSize;
            EXPECT_LE(reach.max.z, windowMax.z - chunk) << voxelSize;
            simulation.Terrain().FlushBox(reach);
            EXPECT_TRUE(simulation.Terrain().HasCollider(sim::PlayerBounds(feet, tuning))) << voxelSize << " at " << corner.x << ", " << corner.y;
        }
    }
}
