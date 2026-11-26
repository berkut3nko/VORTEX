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

        /// The same tick with the simulation's zones, looked up at the tick being stepped.
        sim::PlayerStepResult ZoneTick(const sim::PlayerInput& input, uint8_t teamBit = 1) {
            const std::array<glm::dvec3, 1> focus = {glm::dvec3(state.position)};
            simulation.UpdateStreaming(focus);
            const sim::PlayerZones zones{&simulation.Zones(), &simulation.World(), simulation.Tick(), teamBit};
            const sim::PlayerStepResult r = sim::StepPlayer(simulation.Physics(), simulation.Terrain(), character, state, input, tuning, area,
                                                            zones, simulation.FixedTimeStep());
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
    EXPECT_EQ(sim::PressButtons(s, sim::PlayerButton_Run | 0xC0), 0);                         // unknown bits (above Spell) ignored
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

TEST(Player, PlayBoundsOfARegionAreItsChunkBounds) {
    // [wp-5]: what the client draws is the play bounds themselves; the feet area is 0.3 m inside.
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
        const sim::PlayArea b = sim::PlayBoundsOf(region);
        const float chunk = 32.0f * voxelSize;
        EXPECT_TRUE(b.Bounded());
        EXPECT_FLOAT_EQ(b.min.x, -4.0f * chunk);
        EXPECT_FLOAT_EQ(b.min.y, -5.0f * chunk);
        EXPECT_FLOAT_EQ(b.max.x, 3.0f * chunk);
        EXPECT_FLOAT_EQ(b.max.y, 2.0f * chunk);
        EXPECT_NEAR(sim::PlayAreaOf(region, sim::PlayerTuning{}).min.x, b.min.x + 0.3f, 1.0e-5f);
        EXPECT_NEAR(sim::PlayAreaOf(region, sim::PlayerTuning{}).max.y, b.max.y - 0.3f, 1.0e-5f);
    }
}

TEST(Player, DistanceToThePlayAreaEdge) {
    // [wp-5]: the HUD hint's distance (presentation only).
    sim::PlayArea a;
    a.min = {-10.0f, -20.0f};
    a.max = {10.0f, 20.0f};
    EXPECT_FLOAT_EQ(sim::DistanceToPlayAreaEdge(a, {0.0f, 0.0f}), 10.0f);
    EXPECT_FLOAT_EQ(sim::DistanceToPlayAreaEdge(a, {9.0f, 0.0f}), 1.0f);
    EXPECT_FLOAT_EQ(sim::DistanceToPlayAreaEdge(a, {0.0f, 19.5f}), 0.5f);
    EXPECT_FLOAT_EQ(sim::DistanceToPlayAreaEdge(a, {-9.75f, 19.0f}), 0.25f);
    EXPECT_FLOAT_EQ(sim::DistanceToPlayAreaEdge(a, {11.0f, 0.0f}), -1.0f);
    EXPECT_FLOAT_EQ(sim::DistanceToPlayAreaEdge(a, {0.0f, -25.0f}), -5.0f);
    EXPECT_TRUE(std::isinf(sim::DistanceToPlayAreaEdge(sim::PlayArea{}, {0.0f, 0.0f})));
}

// Zones, push and the dash (docs/design/magic.md "PlayerState, StepPlayer and the physics call"):
// integers from feet quantised to q, at the tick being stepped.
namespace {
    constexpr double kQPerMetre = 16.0 / kVoxel;

    glm::ivec3 MetresToQ(const glm::vec3& m) {
        return {static_cast<int>(std::llround(m.x * kQPerMetre)), static_cast<int>(std::llround(m.y * kQPerMetre)),
                static_cast<int>(std::llround(m.z * kQPerMetre))};
    }

    sim::Zone SphereImpulse(uint16_t id, const glm::ivec3& centreQ, uint16_t radiusQ, int16_t out, int16_t up, uint32_t start) {
        sim::Zone z;
        z.zoneId = id;
        z.kind = sim::ZoneKind::Impulse;
        z.shape.kind = voxel::kShapeSphere;
        z.shape.a = centreQ;
        z.shape.radiusQ = radiusQ;
        z.startTick = start;
        z.strength[0] = out;
        z.strength[1] = up;
        return z;
    }

    /// The push one ApplyZones gives a player standing at `feet` (metres) at `tick`.
    std::array<int16_t, 3> PushAt(const sim::ZoneSet& set, const glm::vec3& feet, uint32_t tick, uint8_t teamBit = 1) {
        sim::PlayerState s;
        s.position = feet;
        sim::ApplyZones(s, sim::PlayerZones{&set, nullptr, tick, teamBit}, sim::PlayerTuning{});
        return {s.push[0], s.push[1], s.push[2]};
    }

    void Settle(Field& f) {
        f.Cook();
        for (int i = 0; i < 10; ++i) f.Tick(sim::PlayerInput{});
    }
}

TEST(Player, PushDecaysToZeroInBoundedTicks) {
    Field f;
    ASSERT_TRUE(f.initialized);
    Settle(f);
    ASSERT_TRUE(f.state.OnGround());
    f.state.push[0] = 1500;
    const float x0 = f.state.position.x;
    int ticks = 0;
    while (f.state.push[0] != 0 && ticks < 100) {
        f.Tick(sim::PlayerInput{});   // no zones: the push still decays
        ++ticks;
    }
    EXPECT_LE(ticks, 34) << "1500 / 45 rounded up";
    const float moved = f.state.position.x - x0;
    EXPECT_GT(moved, 1.5f);
    EXPECT_LT(moved, 4.5f);   // (15 m/s)^2 / (2 * 27 m/s^2) = 4.2 m
    EXPECT_EQ(f.state.push[1], 0);
    EXPECT_EQ(f.state.push[2], 0);
}

TEST(Player, ZonesAreEvaluatedInZoneOrderFromQuantisedFeet) {
    sim::ZoneSet set;
    // Inserted in reverse id order; both start at tick 50.
    ASSERT_TRUE(set.Insert(SphereImpulse(7, {1408, 1600, 1280}, 256, 900, 0, 50)));
    ASSERT_TRUE(set.Insert(SphereImpulse(4, {1280, 1536, 1280}, 256, 600, 200, 50)));
    ASSERT_EQ(set.All()[0].zoneId, 4);
    // Feet (10.3, 12, 10.2) m = (1318, 1536, 1306) q. By hand, in id order:
    // zone 4: d = (38, 0, 26), len 46, f = 600 * 210 / 256 = 492: x 38 * 492 / 46 = 406, z 26 * 492 / 46 = 278, up 200 * 210 / 256 = 164.
    // zone 7: d = (-90, -64, 26), len 113, f = 900 * 143 / 256 = 502: x -90 * 502 / 113 = -399, z 26 * 502 / 113 = 115.
    const std::array<int16_t, 3> push = PushAt(set, {10.3f, 12.0f, 10.2f}, 50);
    EXPECT_EQ(push, (std::array<int16_t, 3>{7, 164, 393}));
    // Feet 0.1 mm away quantise to the same q: the same push.
    EXPECT_EQ(PushAt(set, {10.3001f, 12.0f, 10.2001f}, 50), push);
}

TEST(Player, ZoneEdgeAndCentreAreContinuous) {
    sim::ZoneSet set;
    const glm::ivec3 c(1280, 1536, 1280);
    const int R = 256;   // 2 m: the dead radius is 32 q
    ASSERT_TRUE(set.Insert(SphereImpulse(1, c, static_cast<uint16_t>(R), 600, 0, 10)));
    const auto feetAt = [&c](int k) {
        return glm::vec3(static_cast<float>((c.x + k) / kQPerMetre), static_cast<float>(c.y / kQPerMetre), static_cast<float>(c.z / kQPerMetre));
    };
    std::array<int16_t, 3> last = PushAt(set, feetAt(-48), 10);
    int worst = 0;
    for (int k = -47; k <= 48; ++k) {
        const std::array<int16_t, 3> p = PushAt(set, feetAt(k), 10);
        worst = std::max(worst, std::abs(p[0] - last[0]) + std::abs(p[2] - last[2]));
        last = p;
    }
    EXPECT_LE(worst, 30) << "across the dead radius, 1 q steps";
    EXPECT_EQ(PushAt(set, feetAt(0), 10)[0], 0) << "nothing at the centre";
    EXPECT_GT(PushAt(set, feetAt(1), 10)[0], 0);
    EXPECT_LT(PushAt(set, feetAt(-1), 10)[0], 0);
    // At the rim it falls to 0.
    EXPECT_LE(std::abs(PushAt(set, feetAt(R - 1), 10)[0]), 10);
    EXPECT_EQ(PushAt(set, feetAt(R), 10)[0], 0);
    for (int k = R - 20; k < R; ++k) EXPECT_LE(std::abs(PushAt(set, feetAt(k + 1), 10)[0] - PushAt(set, feetAt(k), 10)[0]), 30);
}

TEST(Player, ImpulseActsOnlyOnItsStartTickAndOnlyOnItsTeams) {
    sim::ZoneSet set;
    sim::Zone z = SphereImpulse(1, {1280, 1536, 1280}, 256, 900, 300, 20);
    z.teamMask = 0b10;
    ASSERT_TRUE(set.Insert(z));
    const glm::vec3 feet(10.5f, 12.0f, 10.0f);
    EXPECT_EQ(PushAt(set, feet, 20, 1u << 0), (std::array<int16_t, 3>{0, 0, 0})) << "team 0 untouched";
    const std::array<int16_t, 3> hit = PushAt(set, feet, 20, 1u << 1);
    EXPECT_GT(hit[0], 0);
    EXPECT_GT(hit[1], 0);
    EXPECT_EQ(hit[2], 0);
    EXPECT_EQ(PushAt(set, feet, 21, 1u << 1), (std::array<int16_t, 3>{0, 0, 0})) << "the tick after its start adds nothing";
    EXPECT_EQ(PushAt(set, feet, 19, 1u << 1), (std::array<int16_t, 3>{0, 0, 0}));
}

TEST(Player, AShovePushesAlongItsAxis) {
    // A capsule from (10, 12.5, 10) to (13, 12.5, 10) m, r 1 m; the feet (11, 12, 10.4) m are 0.5 m below
    // and 0.4 m beside its axis. Nearest axis point (1408, 1600, 1280), d = (0, -64, 51), len 81.
    sim::Zone z;
    z.zoneId = 1;
    z.shape.kind = voxel::kShapeCapsule;
    z.shape.a = MetresToQ({10.0f, 12.5f, 10.0f});
    z.shape.b = MetresToQ({13.0f, 12.5f, 10.0f});
    z.shape.radiusQ = 128;
    z.startTick = 5;
    z.strength[0] = 1200;
    z.strength[1] = 300;
    const glm::vec3 feet(11.0f, 12.0f, 10.4f);

    z.kind = sim::ZoneKind::Shove;
    sim::ZoneSet shove;
    ASSERT_TRUE(shove.Insert(z));
    // Along the axis: 1200 * 47 / 128 = 440, up 300 * 47 / 128 = 110, nothing across it.
    EXPECT_EQ(PushAt(shove, feet, 5), (std::array<int16_t, 3>{440, 110, 0}));

    // The same capsule as an Impulse pushes away from its axis instead (the shove of a dash).
    z.kind = sim::ZoneKind::Impulse;
    sim::ZoneSet impulse;
    ASSERT_TRUE(impulse.Insert(z));
    const std::array<int16_t, 3> aside = PushAt(impulse, feet, 5);
    EXPECT_EQ(aside[0], 0);
    EXPECT_GT(aside[2], 0);
    EXPECT_EQ(aside[1], 110);

    // A player stepped through the Shove moves along +x.
    Field f({11.0f, kGround, 10.4f});
    ASSERT_TRUE(f.initialized);
    Settle(f);
    z.kind = sim::ZoneKind::Shove;
    z.shape.a = MetresToQ({10.0f, kGround + 0.5f, 10.0f});
    z.shape.b = MetresToQ({13.0f, kGround + 0.5f, 10.0f});
    z.startTick = f.simulation.Tick();
    ASSERT_TRUE(f.simulation.Zones().Insert(z));
    const glm::vec3 start = f.state.position;
    for (int i = 0; i < 30; ++i) f.ZoneTick(sim::PlayerInput{});
    EXPECT_GT(f.state.position.x - start.x, 0.5f);
    EXPECT_NEAR(f.state.position.z, start.z, 1.0e-3f);
}

TEST(Player, LiftCarriesAPlayerToTheTopAndHovers) {
    Field f;
    ASSERT_TRUE(f.initialized);
    Settle(f);
    ASSERT_TRUE(f.state.OnGround());
    const glm::vec3 start = f.state.position;
    // An Updraft column: r 1 m, 6 m tall from the hit voxel (the top voxel of the ground, so the
    // standing feet, a hair under the surface, are inside it), 9 m/s, 90 ticks.
    sim::Zone z;
    z.zoneId = 3;
    z.kind = sim::ZoneKind::Lift;
    z.shape.kind = voxel::kShapeCylinder;
    z.shape.a = MetresToQ({start.x, kGround - kVoxel + 3.0f, start.z});
    z.shape.radiusQ = 128;
    z.shape.halfHeightQ = 384;
    z.startTick = f.simulation.Tick();
    z.duration = 90;
    z.strength[0] = 900;
    ASSERT_TRUE(f.simulation.Zones().Insert(z));
    bool lifted = false;
    for (int i = 0; i < 90; ++i) {
        f.ZoneTick(sim::PlayerInput{});
        lifted = lifted || (f.state.status & sim::PlayerStatus_Lifted) != 0;
    }
    EXPECT_TRUE(lifted);
    EXPECT_TRUE(f.state.status & sim::PlayerStatus_Lifted);
    const float height = f.state.position.y - start.y;
    EXPECT_GT(height, 5.0f);
    EXPECT_LT(height, 6.2f);
    // The zone has ended: the player falls and lands, and Lifted is cleared on the ground.
    for (int i = 0; i < 150; ++i) f.ZoneTick(sim::PlayerInput{});
    EXPECT_TRUE(f.state.OnGround());
    EXPECT_NEAR(f.state.position.y, kGround, 0.03f);
    EXPECT_FALSE(f.state.status & sim::PlayerStatus_Lifted);
}

TEST(Player, KnockbackStopsAtThePlayArea) {
    Field f({1.7f, kGround, 0.3f});
    ASSERT_TRUE(f.initialized);
    f.Bound(glm::vec2(-2.0f), glm::vec2(2.0f));
    Settle(f);
    ASSERT_TRUE(f.state.OnGround());
    // An outward (+x) Impulse 0.7 m behind the player: 900 * (256 - 90) / 256 = 583 cm/s.
    ASSERT_TRUE(f.simulation.Zones().Insert(SphereImpulse(1, MetresToQ({1.0f, kGround, 0.3f}), 256, 900, 0, f.simulation.Tick())));
    bool cut = false;
    for (int i = 0; i < 60; ++i) {
        const int16_t before = f.state.push[0];
        f.ZoneTick(sim::PlayerInput{});
        EXPECT_LE(f.state.position.x, 2.0f + 1.0e-6f) << i;
        if (i == 0) EXPECT_GT(f.state.push[0], 500);
        // A push that reaches 0 from above the ground decay was cut at the edge.
        if (before > f.tuning.pushDecayGround && f.state.push[0] == 0) cut = true;
    }
    EXPECT_TRUE(cut);
    EXPECT_EQ(f.state.push[0], 0);
    EXPECT_NEAR(f.state.position.x, 2.0f, 2.0e-3f);
}

TEST(Player, ADashOnFlatGroundCoversEightMetresIn18Ticks) {
    Field f({-4.0f, kGround, 0.3f});
    ASSERT_TRUE(f.initialized);
    Settle(f);
    ASSERT_TRUE(f.state.OnGround());

    sim::PlayerState rooted = f.state;
    rooted.rootLeft = 3;
    EXPECT_FALSE(sim::StartDash(rooted, {1.0f, 0.0f, 0.0f}));
    EXPECT_EQ(rooted.dashLeft, 0);
    sim::PlayerState straightUp = f.state;
    EXPECT_FALSE(sim::StartDash(straightUp, {0.0f, 1.0f, 0.0f})) << "no horizontal direction";

    ASSERT_TRUE(sim::StartDash(f.state, {2.0f, -0.7f, 0.0f}));   // the pitch is ignored
    EXPECT_EQ(f.state.dashLeft, sim::kDashTicks);
    EXPECT_EQ(f.state.dashDir[0], 127);
    EXPECT_EQ(f.state.dashDir[1], 0);
    const glm::vec3 start = f.state.position;
    for (int i = 0; i < sim::kDashTicks; ++i) {
        EXPECT_GT(f.state.dashLeft, 0) << i;
        f.ZoneTick(sim::PlayerInput{});
    }
    EXPECT_EQ(f.state.dashLeft, 0);
    EXPECT_NEAR(f.state.position.x - start.x, 8.0f, 0.5f);
    EXPECT_NEAR(f.state.position.z, start.z, 1.0e-3f);
    EXPECT_NEAR(f.state.position.y, kGround, 0.03f);
    // The carry: 4 m/s along the dash, decaying as a knockback does.
    EXPECT_EQ(f.state.push[0], sim::kDashEndPushCmS);
    EXPECT_EQ(f.state.push[2], 0);
    for (int i = 0; i < 20; ++i) f.ZoneTick(sim::PlayerInput{});
    EXPECT_EQ(f.state.push[0], 0);
}

TEST(Player, ADashStopsAtAWallAndKeepsItsCarry) {
    Field f({-1.0f, kGround, 0.3f});
    ASSERT_TRUE(f.initialized);
    // A stone wall with its face at x = 3 m, 3 m tall, 6 m wide.
    f.simulation.World().FillBox({24, kGroundVoxels, -24}, {28, kGroundVoxels + 24, 24}, f.simulation.WorldMaterialIds().stone);
    Settle(f);
    ASSERT_TRUE(f.state.OnGround());
    ASSERT_TRUE(sim::StartDash(f.state, {1.0f, 0.0f, 0.0f}));
    int ticks = 0;
    while (f.state.dashLeft > 0 && ticks < 30) {
        f.ZoneTick(sim::PlayerInput{});
        ++ticks;
    }
    EXPECT_LT(ticks, sim::kDashTicks) << "the wall ended the dash";
    EXPECT_LT(f.state.position.x, 3.0f - f.tuning.radius + 0.02f);
    EXPECT_GT(f.state.position.x, 3.0f - f.tuning.radius - 0.1f);
    EXPECT_EQ(f.state.push[0], sim::kDashEndPushCmS) << "the carry is kept";
    // It presses on the wall and decays; nothing passes the wall.
    for (int i = 0; i < 15; ++i) {
        f.ZoneTick(sim::PlayerInput{});
        EXPECT_LT(f.state.position.x, 3.0f - f.tuning.radius + 0.02f);
    }
    EXPECT_EQ(f.state.push[0], 0);
}

// The replay rule: a dash and a zone stepped again from a restored state, each step with its own tick,
// give the same states bit for bit.
TEST(Player, ADashAndAZoneReplayedGiveTheSameStates) {
    Field f({-4.0f, kGround, 0.3f});
    ASSERT_TRUE(f.initialized);
    Settle(f);
    const std::array<glm::dvec3, 2> focus = {glm::dvec3(-4.0, kGround, 0.3), glm::dvec3(4.0, kGround, 0.3)};
    f.simulation.Terrain().Update(focus, 12.0f);
    f.simulation.Terrain().Flush();

    const uint32_t base = 1000;
    // A shove beside the path, on the 9th tick of the dash.
    ASSERT_TRUE(f.simulation.Zones().Insert(SphereImpulse(9, MetresToQ({-0.5f, kGround, -0.5f}), 256, 700, 150, base + 8)));
    const auto step = [&f](uint32_t tick) {
        f.simulation.Terrain().FlushBox(sim::PlayerReach(f.state.position, f.tuning));
        const sim::PlayerZones zones{&f.simulation.Zones(), &f.simulation.World(), tick, 1};
        sim::StepPlayer(f.simulation.Physics(), f.simulation.Terrain(), f.character, f.state, sim::PlayerInput{}, f.tuning, f.area, zones,
                        f.simulation.FixedTimeStep());
        f.simulation.StepPhysics();
    };
    ASSERT_TRUE(sim::StartDash(f.state, {1.0f, 0.0f, 0.0f}));
    std::vector<sim::PlayerState> recorded(30);
    for (uint32_t k = 0; k < 30; ++k) {
        step(base + k);
        recorded[k] = f.state;
    }
    EXPECT_NE(recorded[8].push[2], 0) << "the zone acted";
    EXPECT_EQ(recorded[17].dashLeft, 0);

    f.state = recorded[3];
    sim::RestorePlayer(f.simulation.Physics(), f.character, f.state);
    for (uint32_t k = 4; k < 30; ++k) {
        step(base + k);
        EXPECT_EQ(f.state, recorded[k]) << "replayed tick " << k;
    }
}

TEST(Player, CastCooldownFollowsTheSlow) {
    sim::PlayerTuning t;
    sim::PlayerState s;
    EXPECT_EQ(sim::CastCooldownFor(s, t), t.castCooldownTicks);
    s.slowQ = 6;
    EXPECT_EQ(sim::CastCooldownFor(s, t), t.castCooldownTicks * 19 / 16);
    EXPECT_TRUE(sim::TakeCast(s, sim::PlayerButton_Cast, t));
    EXPECT_EQ(s.cooldownTicks, t.castCooldownTicks * 19 / 16);
    t.castCooldownTicks = 250;
    s.slowQ = 255;
    EXPECT_EQ(sim::CastCooldownFor(s, t), 255);
}

// [mag-2] (magic.md "PlayerState, StepPlayer and the physics call" step 1): the Spell edge goes through the cast's
// cooldown; a step with both edges takes the Cast and drops the Spell.
TEST(Player, SpellEdgeSharesTheCastCooldown) {
    Field f;
    ASSERT_TRUE(f.initialized);
    Settle(f);
    sim::PlayerInput spell;
    spell.buttons = sim::PlayerButton_Spell;
    spell.spell = sim::kUpdraft;
    const sim::PlayerInput none;
    for (int tick = 0; tick < 20; ++tick) {
        const bool press = tick == 10 || tick == 12;
        const sim::PlayerStepResult r = f.Tick(press ? spell : none);
        EXPECT_FALSE(r.cast) << tick;
        if (tick == 10) {
            EXPECT_TRUE(r.spell) << "the first Spell edge is taken";
            EXPECT_EQ(f.state.cooldownTicks, f.tuning.castCooldownTicks);
        } else if (tick == 12) {
            EXPECT_EQ(r.pressed & sim::PlayerButton_Spell, sim::PlayerButton_Spell);
            EXPECT_FALSE(r.spell) << "the cooldown refuses the second";
        } else {
            EXPECT_FALSE(r.spell) << tick;
        }
        EXPECT_FALSE(r.spellMove) << "Updraft has no own move";
    }
    // Both edges on one tick: the Cast is taken, the Spell is not.
    for (int i = 0; i < 20; ++i) f.Tick(none);
    sim::PlayerInput both = spell;
    both.buttons = static_cast<uint8_t>(sim::PlayerButton_Spell | sim::PlayerButton_Cast);
    const sim::PlayerStepResult r = f.Tick(both);
    EXPECT_TRUE(r.cast);
    EXPECT_FALSE(r.spell);
}

// [mag-2]: a Dash's Spell edge starts the dash inside StepPlayer at the input's tick, along the input's look, unless
// the stepping side does not allow it (no charge, dead, the form cooling) or the state refuses it (rooted).
TEST(Player, ADashSpellEdgeStartsTheDashInsideTheStep) {
    Field f({-4.0f, kGround, 0.3f});
    ASSERT_TRUE(f.initialized);
    Settle(f);
    sim::PlayerInput dash;
    dash.buttons = sim::PlayerButton_Spell;
    dash.spell = sim::kDash;
    dash.yaw = sim::QuantizeYaw(-1.5707963f);     // looking along +x
    const glm::vec3 start = f.state.position;
    sim::PlayerStepResult r = f.ZoneTick(dash);
    EXPECT_TRUE(r.spell);
    EXPECT_TRUE(r.spellMove);
    EXPECT_EQ(f.state.dashLeft, sim::kDashTicks - 1) << "the dash moved on the edge's own tick";
    EXPECT_EQ(f.state.dashDir[0], 127);
    EXPECT_GT(f.state.position.x - start.x, 0.3f);
    for (int i = 0; i < 30; ++i) f.ZoneTick(sim::PlayerInput{});
    EXPECT_NEAR(f.state.position.x - start.x, 8.0f, 0.8f);

    // Not allowed by the stepping side: the edge still takes the cooldown, nothing moves.
    for (int i = 0; i < 30; ++i) f.ZoneTick(sim::PlayerInput{});
    const std::array<glm::dvec3, 1> focus = {glm::dvec3(f.state.position)};
    f.simulation.UpdateStreaming(focus);
    sim::PlayerZones refused{&f.simulation.Zones(), &f.simulation.World(), f.simulation.Tick(), 1};
    refused.spellMoveAllowed = false;
    r = sim::StepPlayer(f.simulation.Physics(), f.simulation.Terrain(), f.character, f.state, dash, f.tuning, f.area, refused,
                        f.simulation.FixedTimeStep());
    f.simulation.StepPhysics();
    EXPECT_TRUE(r.spell);
    EXPECT_FALSE(r.spellMove);
    EXPECT_EQ(f.state.dashLeft, 0);
    EXPECT_EQ(f.state.cooldownTicks, f.tuning.castCooldownTicks);

    // Rooted: the state refuses it.
    for (int i = 0; i < 30; ++i) f.ZoneTick(sim::PlayerInput{});
    f.state.rootLeft = 5;
    r = f.ZoneTick(dash);
    EXPECT_TRUE(r.spell);
    EXPECT_FALSE(r.spellMove);
    EXPECT_EQ(f.state.dashLeft, 0);
    EXPECT_FALSE(sim::StartSpellMove(f.state, sim::kPunch, {1.0f, 0.0f, 0.0f})) << "Punch has no own move";
}
