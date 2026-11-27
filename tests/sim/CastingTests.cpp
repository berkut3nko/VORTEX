// Casting by mana, gestures and projectiles (docs/design/magic.md "Casting: mana, gestures and projectiles"): the mana
// pool and its costs, the gap, the gesture classification and the server's gesture checks, the shapes drawn along a
// Line, and the effects that land when the projectile arrives.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;

using namespace vortex;

namespace {
    constexpr float kVoxel = 0.125f;
    constexpr int kG = 96;                       // flat ground: voxels y < 96, its top at 12 m
    constexpr float kGround = kG * kVoxel;
    constexpr int32_t kTopQ = kG * 16;           // the ground's top face in q
    using VoxelKey = std::tuple<int, int, int>;

    sim::SimulationConfig YardConfig() {
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

    /// Flat ground at 0.125 m; spells cast and followed up as a server does, with no players.
    struct CastYard {
        CastYard() : simulation(YardConfig()) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(kG, ids.grass, ids.stone));
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
            table = sim::BuildSpellTable(kVoxel);
            ctx.walls = sim::WindowWalls(simulation.World());
        }

        sim::CastSpellResult Cast(sim::SpellId spell, uint8_t player, const glm::dvec3& eye, const glm::dvec3& target,
                                  const sim::SpellGesture& gesture = {}) {
            sim::SpellCast c;
            c.player = player;
            c.team = 1;
            c.spell = spell;
            c.eye = eye;
            c.dir = glm::vec3(glm::normalize(target - eye));
            c.gesture = gesture;
            return sim::CastSpell(simulation, table, c, ctx, {});
        }

        /// Runs the follow-ups of the ticks after the current one up to `last`, stepping the simulation's tick.
        void RunTo(uint32_t last) {
            while (simulation.Tick() < last) {
                simulation.StepPhysics();
                sim::RunScheduled(simulation, table, ctx, simulation.Tick(), {});
            }
        }

        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }

        std::set<VoxelKey> VoxelsOf(uint8_t id, const glm::ivec3& min, const glm::ivec3& max) {
            std::set<VoxelKey> out;
            for (int z = min.z; z < max.z; ++z)
                for (int y = min.y; y < max.y; ++y)
                    for (int x = min.x; x < max.x; ++x)
                        if (World().GetVoxel({x, y, z}) == id) out.emplace(x, y, z);
            return out;
        }

        sim::Simulation simulation;
        sim::SpellTable table;
        sim::CastContext ctx;
        bool initialized = false;
    };

    glm::ivec3 GroundQ(double x, double z) { return {static_cast<int32_t>(x * 128.0), kTopQ, static_cast<int32_t>(z * 128.0)}; }

    sim::SpellGesture LineOf(std::vector<glm::ivec3> points) {
        sim::SpellGesture g;
        g.kind = sim::GestureKind::Line;
        g.pointsQ = std::move(points);
        return g;
    }

    std::vector<sim::GestureAngles> Pitches(std::initializer_list<int> degrees, int yawDegrees = 0) {
        std::vector<sim::GestureAngles> out;
        for (const int d : degrees)
            out.push_back({static_cast<int16_t>(d * 32767 / 90), static_cast<uint16_t>(yawDegrees * 65536 / 360)});
        return out;
    }
}

// --- mana ---------------------------------------------------------------------------------------------------------

TEST(Mana, RegeneratesTenPerTickUpToThePool) {
    const sim::SpellTable t = sim::BuildSpellTable(kVoxel);
    sim::CasterState fresh;
    EXPECT_EQ(sim::ManaAt(t, fresh, 0), 6000) << "a new caster is full";
    sim::CasterState c;
    c.mana = 0;
    c.manaTick = 100;
    EXPECT_EQ(sim::ManaAt(t, c, 100), 0);
    EXPECT_EQ(sim::ManaAt(t, c, 150), 500) << "10 units (1/6 mana) per tick";
    EXPECT_EQ(sim::ManaAt(t, c, 699), 5990);
    EXPECT_EQ(sim::ManaAt(t, c, 700), 6000) << "a full pool refills in 10 s";
    EXPECT_EQ(sim::ManaAt(t, c, 100000), 6000);
    sim::RefillMana(t, c, 800);
    EXPECT_EQ(c.mana, 6000);
    EXPECT_EQ(c.manaTick, 800u);
}

TEST(Mana, CostsFollowTheTable) {
    const sim::SpellTable t = sim::BuildSpellTable(kVoxel);
    const sim::SpellGesture tap;
    EXPECT_EQ(sim::SpellManaCost(t, sim::kShatterSpike, tap), 1200u) << "Pierce 20";
    EXPECT_EQ(sim::SpellManaCost(t, sim::kUpdraft, tap), 1200u);
    EXPECT_EQ(sim::SpellManaCost(t, sim::kPunch, tap), 1200u);
    EXPECT_EQ(sim::SpellManaCost(t, sim::kDash, tap), 1200u);
    EXPECT_EQ(sim::SpellManaCost(t, sim::kBarricadeWave, tap), 1950u) << "a tapped wall costs as a drawn 4.5 m";
    // A drawn shape: 10 + 5 per metre of its length (a 4.5 m wall 32.5, an 8 m one 50), within 1.5-10 m for a wall.
    EXPECT_EQ(sim::SpellManaCost(t, sim::kBarricadeWave, LineOf({GroundQ(0, 0), GroundQ(2, 0), GroundQ(4, 0), GroundQ(4.5, 0)})), 1950u);
    EXPECT_EQ(sim::SpellManaCost(t, sim::kBarricadeWave, LineOf({GroundQ(0, 0), GroundQ(2, 0), GroundQ(4, 0), GroundQ(6, 0), GroundQ(8, 0)})),
              3000u);
    EXPECT_EQ(sim::SpellManaCost(t, sim::kBarricadeWave, LineOf({GroundQ(0, 0), GroundQ(1, 0)})), 1050u) << "extended to 1.5 m";
    std::vector<glm::ivec3> twelve;
    for (int i = 0; i <= 6; ++i) twelve.push_back(GroundQ(2.0 * i, 0));
    EXPECT_EQ(sim::DrawnLengthQ(t, sim::kBarricadeWave, LineOf(twelve)), 1280u) << "cut at 10 m";
    EXPECT_EQ(sim::SpellManaCost(t, sim::kBarricadeWave, LineOf(twelve)), 3600u);
    EXPECT_EQ(sim::SpellManaCost(t, sim::kFlashFreeze, LineOf(twelve)), 600u + 300u * 12u) << "an ice strip up to 12 m";
    sim::SpellGesture rise = LineOf({GroundQ(0, 0), GroundQ(4, 0)});
    rise.kind = sim::GestureKind::Rise;
    EXPECT_EQ(sim::SpellManaCost(t, sim::kBarricadeWave, rise), 1950u) << "only a Line draws";
    EXPECT_EQ(sim::SpellManaCost(t, sim::kPunch, LineOf({GroundQ(0, 0), GroundQ(4, 0)})), 1200u) << "Punch's Line is a tap";
}

// The cost is charged at T_run after every other check: a refused cast costs nothing, a script pays nothing, and a
// cast over the caster's mana is refused NoMana and changes nothing.
TEST(Mana, ChargedAtRunAfterEveryOtherCheck) {
    CastYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(-3.0, kGround + 1.6, 0.0625);
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult lift = yard.Cast(sim::kUpdraft, 1, eye, glm::dvec3(0.0625, kGround, 0.0625));
    ASSERT_EQ(lift.refusal, sim::SpellRefusal::None);
    EXPECT_EQ(lift.manaCost, 1200);
    EXPECT_EQ(sim::ManaAt(yard.table, yard.ctx.casters[1], t0), 4800);

    const sim::CastSpellResult missed = yard.Cast(sim::kUpdraft, 1, eye, eye + glm::dvec3(0.0, 1.0, 0.0));
    EXPECT_EQ(missed.refusal, sim::SpellRefusal::Missed);
    EXPECT_EQ(sim::ManaAt(yard.table, yard.ctx.casters[1], t0), 4800) << "a refusal costs nothing";

    const sim::CastSpellResult script = yard.Cast(sim::kUpdraft, 0, eye, glm::dvec3(4.0625, kGround, 4.0625));
    EXPECT_EQ(script.refusal, sim::SpellRefusal::None);
    EXPECT_EQ(script.manaCost, 0) << "a script is no player";

    yard.ctx.casters[2].mana = 1000;
    yard.ctx.casters[2].manaTick = t0;
    const size_t zones = yard.simulation.Zones().All().size();
    const sim::CastSpellResult poor = yard.Cast(sim::kUpdraft, 2, eye, glm::dvec3(2.0625, kGround, 2.0625));
    EXPECT_EQ(poor.refusal, sim::SpellRefusal::NoMana);
    EXPECT_EQ(yard.simulation.Zones().All().size(), zones) << "no zone of a refused cast";
    EXPECT_EQ(yard.ctx.casters[2].mana, 1000);
    yard.RunTo(t0 + 20);
    EXPECT_EQ(yard.Cast(sim::kUpdraft, 2, eye, glm::dvec3(2.0625, kGround, 2.0625)).refusal, sim::SpellRefusal::None)
        << "20 ticks later the pool holds 1200";
    EXPECT_EQ(yard.ctx.casters[2].mana, 0);
}

// --- gestures ------------------------------------------------------------------------------------------------------

TEST(Gesture, ClassifiesTapRiseFallAndLine) {
    const uint32_t m = 128;
    const std::vector<glm::ivec3> none;
    const std::vector<glm::ivec3> shortPath = {GroundQ(0, 0), GroundQ(0.5, 0)};
    const std::vector<glm::ivec3> longPath = {GroundQ(0, 0), GroundQ(1.5, 0), GroundQ(3, 0)};
    // Tap: held under 12 ticks, whatever was drawn; or short with little pitch change.
    EXPECT_EQ(sim::ClassifyGesture(longPath, Pitches({0, 40}), 11, m), sim::GestureKind::Tap);
    EXPECT_EQ(sim::ClassifyGesture(shortPath, Pitches({0, 11}), 30, m), sim::GestureKind::Tap);
    EXPECT_EQ(sim::ClassifyGesture(none, {}, 30, m), sim::GestureKind::Tap);
    // Rise and Fall: 12 degrees or more of pitch, more pitch sweep than yaw sweep (a short path too).
    EXPECT_EQ(sim::ClassifyGesture(shortPath, Pitches({0, 6, 13}), 30, m), sim::GestureKind::Rise);
    EXPECT_EQ(sim::ClassifyGesture(longPath, Pitches({10, -5}), 30, m), sim::GestureKind::Fall);
    // A large turn sideways beats the pitch: a Line.
    std::vector<sim::GestureAngles> turn = Pitches({0, 13});
    turn[1].yaw = static_cast<uint16_t>(40 * 65536 / 360);
    EXPECT_EQ(sim::ClassifyGesture(longPath, turn, 30, m), sim::GestureKind::Line);
    // The yaw sweep unwraps across 0: 350 to 10 degrees is 20 degrees, not 340.
    std::vector<sim::GestureAngles> wrap = Pitches({0, 15});
    wrap[0].yaw = static_cast<uint16_t>(350 * 65536 / 360);
    wrap[1].yaw = static_cast<uint16_t>(10 * 65536 / 360);
    EXPECT_EQ(sim::ClassifyGesture(longPath, wrap, 30, m), sim::GestureKind::Line);
    wrap[1].pitch = static_cast<int16_t>(25 * 32767 / 90);
    EXPECT_EQ(sim::ClassifyGesture(longPath, wrap, 30, m), sim::GestureKind::Rise);
    // Line: anything else 1 m or longer; a short one with a sideways pitch-free sweep stays a tap.
    EXPECT_EQ(sim::ClassifyGesture(longPath, Pitches({0, 2}), 30, m), sim::GestureKind::Line);
    EXPECT_EQ(sim::ClassifyGesture(shortPath, Pitches({0, 2}), 30, m), sim::GestureKind::Tap);
    // 12 degrees is the threshold exactly.
    std::vector<sim::GestureAngles> twelve(2);
    twelve[1].pitch = sim::kGesturePitchUnits;
    EXPECT_EQ(sim::ClassifyGesture(shortPath, twelve, 30, m), sim::GestureKind::Rise);
    twelve[1].pitch = sim::kGesturePitchUnits - 1;
    EXPECT_EQ(sim::ClassifyGesture(shortPath, twelve, 30, m), sim::GestureKind::Tap);
    // AUTO's forms.
    EXPECT_EQ(sim::AutoFormOf(sim::GestureKind::Tap), sim::Form::Pierce);
    EXPECT_EQ(sim::AutoFormOf(sim::GestureKind::Rise), sim::Form::Plume);
    EXPECT_EQ(sim::AutoFormOf(sim::GestureKind::Fall), sim::Form::Flow);
    EXPECT_EQ(sim::AutoFormOf(sim::GestureKind::Line), sim::Form::Burst);
    sim::SpellGesture forced;
    forced.kind = sim::GestureKind::Line;
    forced.form = static_cast<uint8_t>(sim::Form::Plume);
    EXPECT_EQ(sim::FormOfGesture(forced), sim::Form::Plume);
}

TEST(Gesture, ServerChecksThePoints) {
    CastYard yard;
    ASSERT_TRUE(yard.initialized);
    const sim::SpellTable& t = yard.table;
    const glm::ivec3 eye = GroundQ(0, 0) + glm::ivec3(0, 207, 0);
    const auto check = [&](const sim::SpellGesture& g) { return sim::CheckGesture(yard.World(), yard.ctx.walls, t, eye, g); };
    EXPECT_EQ(check(LineOf({GroundQ(1, 0), GroundQ(3, 0), GroundQ(5, 0)})), sim::SpellRefusal::None);
    EXPECT_EQ(check(sim::SpellGesture{}), sim::SpellRefusal::None) << "no points: a plain tap";
    EXPECT_EQ(check(LineOf({GroundQ(1, 0), GroundQ(3.1, 0)})), sim::SpellRefusal::BadGesture) << "2.1 m between two points";
    EXPECT_EQ(check(LineOf({GroundQ(31.5, 0)})), sim::SpellRefusal::BadGesture) << "beyond 31 m of the eye";
    std::vector<glm::ivec3> long13;
    for (int i = 0; i <= 7; ++i) long13.push_back(GroundQ(-6.0 + 1.9 * i, 1.0));
    EXPECT_EQ(check(LineOf(long13)), sim::SpellRefusal::BadGesture) << "a path over 12 m";
    std::vector<glm::ivec3> seventeen;
    for (int i = 0; i < 17; ++i) seventeen.push_back(GroundQ(0.5 * i, 0));
    EXPECT_EQ(check(LineOf(seventeen)), sim::SpellRefusal::BadGesture) << "17 points";
    // A Line's points and a Rise's first point must be within 1 m of a solid voxel.
    const glm::ivec3 up(0, 16 * 9, 0);   // 1.125 m above the ground
    EXPECT_EQ(check(LineOf({GroundQ(1, 0), GroundQ(2, 0) + up})), sim::SpellRefusal::BadGesture) << "a Line point in the air";
    EXPECT_EQ(check(LineOf({GroundQ(1, 0), GroundQ(2, 0) + glm::ivec3(0, 120, 0)})), sim::SpellRefusal::None) << "0.94 m up";
    sim::SpellGesture rise = LineOf({GroundQ(1, 0), GroundQ(1, 0) + up});
    rise.kind = sim::GestureKind::Rise;
    EXPECT_EQ(check(rise), sim::SpellRefusal::None) << "a Rise's end may be in the air";
    rise.pointsQ = {GroundQ(1, 0) + up, GroundQ(1, 0) + up + up};
    EXPECT_EQ(check(rise), sim::SpellRefusal::BadGesture) << "a Rise's start in the air";
    sim::SpellGesture fall = rise;
    fall.kind = sim::GestureKind::Fall;
    EXPECT_EQ(check(fall), sim::SpellRefusal::None) << "a Fall's points need no support";
}

// A gesture whose form is not the spell's is refused, and so is one that fails the checks; neither costs mana.
TEST(Gesture, ItsFormMustBeTheSpells) {
    CastYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(-3.0, kGround + 1.6, 0.0625);
    const sim::SpellGesture line = LineOf({GroundQ(0, -1), GroundQ(0, 1)});
    EXPECT_EQ(yard.Cast(sim::kUpdraft, 1, eye, glm::dvec3(0.0, kGround, 0.0), line).refusal, sim::SpellRefusal::BadGesture)
        << "a Line is Burst in AUTO";
    sim::SpellGesture forced = line;
    forced.form = static_cast<uint8_t>(sim::Form::Plume);
    EXPECT_EQ(yard.Cast(sim::kUpdraft, 1, eye, glm::dvec3(0.0, kGround, 0.0), forced).refusal, sim::SpellRefusal::None)
        << "a forced Plume with a Line casts Updraft at the release point";
    sim::SpellGesture far = LineOf({GroundQ(0, -1), GroundQ(0, 2)});
    EXPECT_EQ(yard.Cast(sim::kBarricadeWave, 1, eye, glm::dvec3(0.0, kGround, 0.0), far).refusal, sim::SpellRefusal::BadGesture);
    EXPECT_EQ(sim::ManaAt(yard.table, yard.ctx.casters[1], yard.simulation.Tick()), 6000 - 1200);
}

// A Line draws Barricade Wave as a knit wall 1.5 m high and 0.5 m thick on the ground under the polyline; each segment
// rises when the line reaches it (20 m/s after the projectile's flight, never before the ghost's 18 ticks).
TEST(Gesture, ADrawnWallStandsAlongTheLine) {
    CastYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(-4.0, kGround + 1.6, 0.0625);
    // An L: 4 m along +x at z = 2, then 2 m along +z.
    const sim::SpellGesture g = LineOf({GroundQ(-1, 2), GroundQ(1, 2), GroundQ(3, 2), GroundQ(3, 4)});
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kBarricadeWave, 1, eye, glm::dvec3(3.0, kGround, 4.0), g);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    EXPECT_EQ(r.stats.drawnLengthQ, 768u);
    EXPECT_EQ(r.manaCost, 600 + 300 * 6);
    EXPECT_EQ(r.drawnPointsQ, g.pointsQ);
    EXPECT_EQ(r.hitQ, g.pointsQ.front()) << "the projectile flies to the first point";
    EXPECT_GT(r.flightTicks, 0);
    EXPECT_TRUE(r.groups.empty()) << "nothing lands at T_run";
    const uint8_t knit = yard.Ids().knitIron;
    const glm::ivec3 lo(-16, kG, 8), hi(32, kG + 16, 40);
    yard.RunTo(t0 + 17);
    EXPECT_TRUE(yard.VoxelsOf(knit, lo, hi).empty()) << "the ghost's lead";
    yard.RunTo(t0 + 80);
    const std::set<VoxelKey> wall = yard.VoxelsOf(knit, lo, hi);
    EXPECT_EQ(wall.size(), r.stats.planned);
    // Along x: z from 14 to 17 (0.5 m thick round z = 16), y from 96 to 107 (1.5 m), x from -8 to 24.
    for (int x = -8; x < 24; ++x)
        for (int z = 14; z < 18; ++z)
            for (int y = kG; y < kG + 12; ++y) EXPECT_TRUE(wall.contains({x, y, z})) << x << " " << y << " " << z;
    EXPECT_FALSE(wall.contains({0, kG + 12, 16})) << "1.5 m high";
    EXPECT_FALSE(wall.contains({0, kG, 18})) << "0.5 m thick";
    EXPECT_FALSE(wall.contains({0, kG, 13}));
    // Along z: x from 22 to 25 round x = 24.
    for (int z = 18; z < 32; ++z) EXPECT_TRUE(wall.contains({23, kG + 5, z})) << z;
    EXPECT_FALSE(wall.contains({23, kG + 5, 34}));
}

// The drawn segments land in line order: the far end of a 10 m wall rises after its near end.
TEST(Gesture, DrawnSegmentsLandInLineOrder) {
    CastYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(-4.0, kGround + 1.6, -2.0);
    std::vector<glm::ivec3> points;
    for (int i = 0; i <= 6; ++i) points.push_back(GroundQ(-2.0 + 2.0 * i, 0));   // 12 m drawn: cut at 10 m
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kBarricadeWave, 1, eye, glm::dvec3(0.0, kGround, 0.0), LineOf(points));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    EXPECT_EQ(r.stats.drawnLengthQ, 1280u);
    ASSERT_EQ(r.drawnPointsQ.size(), 6u);
    EXPECT_EQ(r.drawnPointsQ.back(), GroundQ(8, 0));
    const uint8_t knit = yard.Ids().knitIron;
    uint32_t nearTick = 0, farTick = 0;
    for (uint32_t t = t0 + 1; t <= t0 + 90 && farTick == 0; ++t) {
        yard.RunTo(t);
        if (nearTick == 0 && yard.World().GetVoxel({-8, kG + 2, 0}) == knit) nearTick = t;
        if (farTick == 0 && yard.World().GetVoxel({60, kG + 2, 0}) == knit) farTick = t;
    }
    ASSERT_NE(nearTick, 0u);
    ASSERT_NE(farTick, 0u);
    EXPECT_EQ(nearTick, t0 + 18) << "the first segment waits for the ghost's lead";
    EXPECT_EQ(farTick, t0 + r.flightTicks + 24) << "the last segment starts 8 m along the line: 24 ticks at 20 m/s";
    EXPECT_NE(yard.World().GetVoxel({70, kG + 2, 0}), knit) << "nothing past 10 m";
}

// A Line draws Flash Freeze as an ice strip on the ground under the polyline, its Root and Chill in a capsule along it.
TEST(Gesture, FlashFreezeDrawsAnIceStrip) {
    CastYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(-4.0, kGround + 1.6, 0.0625);
    const sim::SpellGesture g = LineOf({GroundQ(0, -3), GroundQ(0, -1), GroundQ(0, 1), GroundQ(0, 3)});
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kFlashFreeze, 1, eye, glm::dvec3(0.0, kGround, 3.0), g);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    ASSERT_EQ(r.zonesStarted.size(), 2u);
    EXPECT_EQ(r.zonesStarted[0].kind, sim::ZoneKind::Root);
    EXPECT_EQ(r.zonesStarted[0].shape.kind, voxel::kShapeCapsule);
    EXPECT_EQ(r.zonesStarted[0].shape.a, g.pointsQ.front());
    EXPECT_EQ(r.zonesStarted[0].shape.b, g.pointsQ.back());
    EXPECT_EQ(r.zonesStarted[0].startTick, std::max<uint32_t>(t0 + r.flightTicks, t0 + yard.table.zoneLeadTicks));
    yard.RunTo(t0 + 80);
    const std::set<VoxelKey> ice = yard.VoxelsOf(yard.Ids().ice, {-16, kG - 2, -32}, {16, kG + 4, 32});
    EXPECT_EQ(ice.size(), r.stats.planned);
    // 2 m wide (16 voxels) and 6 m long, one voxel on the ground.
    EXPECT_EQ(ice.size(), 16u * 49u);
    for (const auto& [x, y, z] : ice) {
        EXPECT_EQ(y, kG);
        EXPECT_GE(x, -8);
        EXPECT_LT(x, 8);
    }
}

// A Line points Punch's shove along the stroke's last segment; the fist stays at the release point's aim.
TEST(Gesture, PunchShovesAlongTheLastSegment) {
    CastYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(0.0625, kGround + 1.6, 0.0625);
    const sim::SpellGesture g = LineOf({GroundQ(2, -1), GroundQ(2, 0), GroundQ(2, 1)});   // drawn along +z
    const sim::CastSpellResult r = yard.Cast(sim::kPunch, 1, eye, glm::dvec3(2.0, kGround, 1.0), g);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    EXPECT_EQ(r.flightTicks, 0) << "the caster is the projectile";
    ASSERT_FALSE(r.zonesStarted.empty());
    const sim::Zone& shove = r.zonesStarted[0];
    ASSERT_EQ(shove.kind, sim::ZoneKind::Shove);
    const glm::ivec3 axis = shove.shape.b - shove.shape.a;
    EXPECT_EQ(axis.x, 0);
    EXPECT_EQ(axis.y, 0);
    EXPECT_GT(axis.z, 0);
    EXPECT_EQ(r.manaCost, 1200);
}

// --- projectiles ---------------------------------------------------------------------------------------------------

// Every effect of a spell lands when its projectile arrives: flight = ceil(dist * 60 / speed) ticks (Burst 40 m/s,
// Pierce 80 m/s), the ops at T_run + flight, the zones at max(that, T_run + the lead).
TEST(Projectile, EffectsLandOnArrival) {
    CastYard yard;
    ASSERT_TRUE(yard.initialized);
    // Flash Freeze 20 m away along the ground: the eye 1.6 m up; the hit about 20.06 m away: 31 ticks at 40 m/s.
    const glm::dvec3 eye(-10.0, kGround + 1.6, 0.0625);
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kFlashFreeze, 1, eye, glm::dvec3(10.0, kGround, 0.0625));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    EXPECT_EQ(r.flightTicks, sim::FlightTicks(yard.table, glm::ivec3(glm::round(eye * 128.0)), r.hitQ, 40));
    EXPECT_GE(r.flightTicks, 30);
    EXPECT_LE(r.flightTicks, 32);
    EXPECT_EQ(r.zoneTick, t0 + r.flightTicks);
    for (const sim::Zone& z : r.zonesStarted) EXPECT_EQ(z.startTick, t0 + r.flightTicks);
    EXPECT_TRUE(r.groups.empty()) << "nothing lands at T_run";
    const uint8_t ice = yard.Ids().ice;
    const glm::ivec3 lo(48, kG - 2, -40), hi(112, kG + 4, 40);
    yard.RunTo(t0 + r.flightTicks - 1);
    EXPECT_TRUE(yard.VoxelsOf(ice, lo, hi).empty()) << "in flight";
    yard.RunTo(t0 + r.flightTicks);
    EXPECT_EQ(yard.VoxelsOf(ice, lo, hi).size(), r.stats.planned) << "landed on arrival";

    // Shatter Spike at a wall 12 m away: 9 ticks at 80 m/s; the bore and the rime land together.
    for (int y = kG; y < kG + 24; ++y)
        for (int z = -8; z < 8; ++z)
            for (int x = 96; x < 104; ++x) yard.World().SetVoxel({x, y, z}, yard.Ids().stone);
    const glm::dvec3 eye2(0.0625, kGround + 1.0, 0.0625);
    const uint32_t t1 = yard.simulation.Tick();
    const sim::CastSpellResult spike = yard.Cast(sim::kShatterSpike, 1, eye2, eye2 + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(spike.refusal, sim::SpellRefusal::None);
    EXPECT_EQ(spike.flightTicks, 9);
    EXPECT_EQ(spike.zoneTick, t1 + 24) << "the lead is longer than the flight";
    const glm::ivec3 face(96, kG + 8, 0);
    EXPECT_EQ(yard.World().GetVoxel(face), yard.Ids().stone);
    yard.RunTo(t1 + 8);
    EXPECT_EQ(yard.World().GetVoxel(face), yard.Ids().stone) << "in flight";
    yard.RunTo(t1 + 9);
    EXPECT_NE(yard.World().GetVoxel(face), yard.Ids().stone) << "bored or rimed on arrival";

    // Punch and Dash fly nothing.
    const sim::CastSpellResult punch = yard.Cast(sim::kPunch, 1, eye2, eye2 + glm::dvec3(0.0, 0.0, 1.0));
    EXPECT_EQ(punch.flightTicks, 0);
}
