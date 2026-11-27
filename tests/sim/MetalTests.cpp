// Flow, climbing, the rune and the rest of Metal (docs/design/magic.md "Flow (Liquid): down, then lowest-first",
// "Inputs every operator shares", spells 1-3 and 13, "Casting: mana, gestures and projectiles"): the integer arc, the Flow operator, Molten
// Pour's slag and its Set, the hazard scan, Rising Spire's bands, ledges and Lift, the Rise gesture's stairs and bridge,
// Rebar Lance's sockets and pegs, and Rune Trap's trigger and burst.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
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
    constexpr double kEye = 1.62;
    using VoxelKey = std::tuple<int, int, int>;

    sim::SimulationConfig MetalConfig(float vs) {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.maxParticles = 0;
        config.jobThreads = 2;
        config.world.voxelSize = vs;
        config.world.windowSize = {8, 8, 8};   // the 0.1 m spire and its rider stay under the window's top
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        return config;
    }

    struct Fighter {
        uint8_t player = 0;
        sim::TeamId team = sim::kNoTeam;
        sim::PlayerState state;
        physics::CharacterId character;
        sim::PlayerInput input;
    };

    /// Flat ground about 12 m up at `vs` (G voxels): players stepped by the movement rule, spells cast and followed up
    /// as a server does.
    struct MetalYard {
        explicit MetalYard(float voxelSize = 0.125f)
            : vs(voxelSize), G(static_cast<int>(std::lround(12.0 / voxelSize))), ground(G * static_cast<double>(voxelSize)),
              simulation(MetalConfig(voxelSize)) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(G, ids.grass, ids.stone));
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(0.0, ground, 0.0));
            table = sim::BuildSpellTable(voxelSize);
            ctx.walls = sim::WindowWalls(simulation.World());
        }

        Fighter& Add(uint8_t player, sim::TeamId team, const glm::vec3& feet) {
            Fighter& f = fighters.emplace_back();
            f.player = player;
            f.team = team;
            f.state.position = feet;
            f.character = simulation.Physics().CreateCharacter(sim::PlayerCharacterDesc(feet, ctx.tuning));
            return f;
        }

        std::vector<sim::SpellTarget> Targets() const {
            std::vector<sim::SpellTarget> out;
            for (const Fighter& f : fighters) out.push_back({f.player, f.team, f.state.position});
            return out;
        }

        /// One server tick: the players' step, then the follow-ups (with `extra` as more targets that are not stepped).
        sim::ScheduledResult Tick(const std::vector<sim::SpellTarget>& extra = {}) {
            std::vector<glm::dvec3> focus{glm::dvec3(0.0, ground, 0.0)};
            for (const Fighter& f : fighters) focus.push_back(glm::dvec3(f.state.position));
            simulation.UpdateStreaming(focus);
            for (Fighter& f : fighters) {
                simulation.Terrain().FlushBox(sim::PlayerReach(f.state.position, ctx.tuning));
                const sim::PlayerZones zones{&simulation.Zones(), &simulation.World(), simulation.Tick(), static_cast<uint8_t>(1u << f.team)};
                sim::StepPlayer(simulation.Physics(), simulation.Terrain(), f.character, f.state, f.input, ctx.tuning, sim::PlayArea{}, zones,
                                simulation.FixedTimeStep());
            }
            std::vector<sim::SpellTarget> targets = Targets();
            targets.insert(targets.end(), extra.begin(), extra.end());
            sim::ScheduledResult r = sim::RunScheduled(simulation, table, ctx, simulation.Tick(), targets);
            simulation.StepPhysics();
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
            return r;
        }

        /// Ticks up to `last` (the follow-ups of `last` included); every tick's result.
        std::vector<sim::ScheduledResult> RunTo(uint32_t last, const std::vector<sim::SpellTarget>& extra = {}) {
            std::vector<sim::ScheduledResult> out;
            while (simulation.Tick() <= last) out.push_back(Tick(extra));
            return out;
        }

        void Settle() {
            std::vector<glm::dvec3> focus{glm::dvec3(0.0, ground, 0.0)};
            for (const Fighter& f : fighters) focus.push_back(glm::dvec3(f.state.position));
            simulation.Terrain().Update(focus, simulation.Config().colliderRadius);
            simulation.Terrain().Flush();
            for (int i = 0; i < 10; ++i) Tick();
        }

        sim::CastSpellResult Cast(sim::SpellId spell, uint8_t player, sim::TeamId team, const glm::dvec3& eye, const glm::dvec3& target,
                                  const sim::SpellGesture& gesture = {}, const std::vector<sim::SpellTarget>& extra = {}) {
            sim::SpellCast c;
            c.player = player;
            c.team = team;
            c.spell = spell;
            c.eye = eye;
            c.dir = glm::vec3(glm::normalize(target - eye));
            c.gesture = gesture;
            std::vector<sim::SpellTarget> targets = Targets();
            targets.insert(targets.end(), extra.begin(), extra.end());
            return sim::CastSpell(simulation, table, c, ctx, targets);
        }

        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        uint32_t Count(const glm::ivec3& min, const glm::ivec3& max, uint8_t id) {
            uint32_t n = 0;
            for (int z = min.z; z < max.z; ++z)
                for (int y = min.y; y < max.y; ++y)
                    for (int x = min.x; x < max.x; ++x) n += World().GetVoxel({x, y, z}) == id ? 1u : 0u;
            return n;
        }
        /// Metres to voxels.
        int V(double metres) const { return static_cast<int>(std::lround(metres / static_cast<double>(vs))); }
        /// Metres to q.
        glm::ivec3 Q(const glm::dvec3& metres) const {
            const double q = 16.0 / static_cast<double>(vs);
            return {static_cast<int32_t>(std::llround(metres.x * q)), static_cast<int32_t>(std::llround(metres.y * q)),
                    static_cast<int32_t>(std::llround(metres.z * q))};
        }

        float vs;
        int G;
        double ground;
        sim::Simulation simulation;
        sim::SpellTable table;
        sim::CastContext ctx;
        std::deque<Fighter> fighters;
        bool initialized = false;
    };

    sim::SpellGesture GestureOf(sim::GestureKind kind, std::vector<glm::ivec3> points) {
        sim::SpellGesture g;
        g.kind = kind;
        g.pointsQ = std::move(points);
        return g;
    }

    /// D = round(direction * 16384), as the cast takes its aim.
    glm::ivec3 AimOf(const glm::dvec3& dir) {
        const glm::dvec3 d = glm::normalize(dir);
        return {static_cast<int32_t>(std::lround(d.x * 16384.0)), static_cast<int32_t>(std::lround(d.y * 16384.0)),
                static_cast<int32_t>(std::lround(d.z * 16384.0))};
    }

    /// The launch velocity in 1/4096 voxel per tick of `metresPerSecond` at `vs`.
    glm::i64vec3 FineVelocity(const glm::dvec3& metresPerSecond, float vs) {
        const glm::dvec3 f = metresPerSecond / static_cast<double>(vs) / 60.0 * 4096.0;
        return {std::llround(f.x), std::llround(f.y), std::llround(f.z)};
    }

    sim::FlowParams PourParams(const sim::SpellTable& table, const glm::ivec3& seed) {
        const sim::SpellDef& d = table.spells[sim::kMoltenPour];
        sim::FlowParams p;
        p.seed = seed;
        p.axes = sim::FaceAxesOf({0, 1, 0});
        p.seedRadius = d.flowSeedRadius;
        p.fallMax = d.fallMax;
        p.runPops = d.flowRunPops;
        p.lateral = d.flowLateral;
        p.frontCount = d.frontCount;
        p.visitCap = d.flowVisitCap;
        return p;
    }
}

// --- Flow's arc ---------------------------------------------------------------------------------------------------

// The arc is integer only: two worlds and two traces give the same hit, and the hit lies on the tick's segment of the
// closed form P_k = P_0 + k V - G k^2 / 2 (the first step's half gravity makes the sum exact).
TEST(Flow, ArcTraceIsIntegerAndReproducible) {
    MetalYard a, b;
    ASSERT_TRUE(a.initialized && b.initialized);
    sim::ArcParams p;
    p.startQ = a.Q({0.0625, a.ground + kEye, 0.0625});
    p.velocity = sim::ArcLaunch(AimOf({1.0, -0.1, 0.3}), a.table.arcSpeedFine);
    p.gravity = a.table.arcGravity;
    const sim::ArcHit ha = sim::TraceArc(a.World(), a.ctx.walls, p);
    const sim::ArcHit hb = sim::TraceArc(b.World(), b.ctx.walls, p);
    const sim::ArcHit again = sim::TraceArc(a.World(), a.ctx.walls, p);
    ASSERT_TRUE(ha.hit);
    EXPECT_FALSE(ha.wall);
    for (const sim::ArcHit* h : {&hb, &again}) {
        EXPECT_EQ(h->voxel, ha.voxel);
        EXPECT_EQ(h->normal, ha.normal);
        EXPECT_EQ(h->hitQ, ha.hitQ);
        EXPECT_EQ(h->ticks, ha.ticks);
        EXPECT_EQ(h->apexQ, ha.apexQ);
        EXPECT_EQ(h->visits, ha.visits);
    }
    EXPECT_EQ(ha.voxel.y, a.G - 1) << "it lands on the ground";
    EXPECT_EQ(ha.normal, glm::ivec3(0, 1, 0));
    EXPECT_EQ(ha.hitQ.y, a.G * 16);
    const auto at = [&](int64_t k) {
        const glm::i64vec3 fine = glm::i64vec3(p.startQ) * int64_t{256} + p.velocity * k - glm::i64vec3(0, p.gravity * k * k / 2, 0);
        return glm::i64vec3(fine.x >> 8, fine.y >> 8, fine.z >> 8);
    };
    const glm::i64vec3 from = at(ha.ticks - 1), to = at(ha.ticks);
    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(ha.hitQ[i], std::min(from[i], to[i]) - 1) << i;
        EXPECT_LE(ha.hitQ[i], std::max(from[i], to[i]) + 1) << i;
    }
}

// The arc follows the parabola of its launch under 9.81 m/s^2 within one voxel at every voxel size: the apex and the
// landing of a lob, and where a 30 degree throw at Flow's 22 m/s meets a wall 8 m away.
TEST(Flow, ArcMatchesTheParabola) {
    for (const float vs : {0.1f, 0.125f, 0.15f}) {
        MetalYard yard(vs);
        ASSERT_TRUE(yard.initialized);
        const double q = 16.0 / static_cast<double>(vs), g = 9.81;
        const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
        sim::ArcParams p;
        p.startQ = yard.Q(eye);
        p.gravity = yard.table.arcGravity;
        // A lob: 3 m/s along x, 8 m/s up. Its apex 3.26 m over the eye, its landing where it falls 1.62 m under it.
        p.velocity = FineVelocity({3.0, 8.0, 0.0}, vs);
        const sim::ArcHit lob = sim::TraceArc(yard.World(), yard.ctx.walls, p);
        ASSERT_TRUE(lob.hit);
        const double vx = static_cast<double>(p.velocity.x) / 4096.0 * 60.0 * static_cast<double>(vs);
        const double vy = static_cast<double>(p.velocity.y) / 4096.0 * 60.0 * static_cast<double>(vs);
        const double apex = eye.y + vy * vy / (2.0 * g);
        EXPECT_NEAR(lob.apexQ / q, apex, static_cast<double>(vs)) << "vs " << vs;
        const double t = (vy + std::sqrt(vy * vy + 2.0 * g * kEye)) / g;
        EXPECT_NEAR(lob.hitQ.x / q, eye.x + vx * t, static_cast<double>(vs)) << "vs " << vs;
        EXPECT_EQ(lob.hitQ.y, yard.G * 16);

        // A 30 degree throw at 22 m/s into a wall 8 m east.
        const int wall = yard.V(8.0);
        yard.World().FillBox({wall, yard.G, -yard.V(4.0)}, {wall + 4, yard.G + yard.V(10.0), yard.V(4.0)}, yard.Ids().stone);
        const double angle = 30.0 * 3.14159265358979 / 180.0;
        p.velocity = sim::ArcLaunch(AimOf({std::cos(angle), std::sin(angle), 0.0}), yard.table.arcSpeedFine);
        const sim::ArcHit throwHit = sim::TraceArc(yard.World(), yard.ctx.walls, p);
        ASSERT_TRUE(throwHit.hit);
        EXPECT_EQ(throwHit.voxel.x, wall);
        EXPECT_EQ(throwHit.normal, glm::ivec3(-1, 0, 0));
        const double tw = (wall * static_cast<double>(vs) - eye.x) / (22.0 * std::cos(angle));
        EXPECT_NEAR(throwHit.hitQ.y / q, eye.y + 22.0 * std::sin(angle) * tw - 0.5 * g * tw * tw, static_cast<double>(vs)) << "vs " << vs;
    }
}

// A lob to a point reaches it: the arc of LobLaunch lands within a voxel of its target on flat ground.
TEST(Flow, ALobLandsAtItsTarget) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::ivec3 from = yard.Q({0.0625, yard.ground + kEye, 0.0625});
    for (const glm::dvec3& to : {glm::dvec3(3.0, yard.ground, 1.0), glm::dvec3(-12.0, yard.ground, 5.0), glm::dvec3(0.5, yard.ground, -0.5)}) {
        uint32_t ticks = 0;
        const int64_t horizontal = (int64_t{20} * yard.table.metreQ << sim::kArcFineShift) / 60;
        sim::ArcParams p;
        p.startQ = from;
        p.gravity = yard.table.arcGravity;
        p.velocity = sim::LobLaunch(from, yard.Q(to), horizontal, p.gravity, yard.table.lobMinTicks, yard.table.maxFlightTicks, ticks);
        const sim::ArcHit hit = sim::TraceArc(yard.World(), yard.ctx.walls, p);
        ASSERT_TRUE(hit.hit);
        EXPECT_LE(std::abs(hit.hitQ.x - yard.Q(to).x), 16) << to.x;
        EXPECT_LE(std::abs(hit.hitQ.z - yard.Q(to).z), 16) << to.z;
        EXPECT_LE(hit.ticks, ticks + 1u);
        EXPECT_GE(hit.ticks + 1u, ticks);
    }
}

// --- the Flow operator ----------------------------------------------------------------------------------------------

// The Flow never moves up, and it fills the deepest basin it reaches from the bottom: on flat ground beside a pit 2 m
// deep, its pool (Front) is the pit's floor.
TEST(Propagate, FlowNeverRisesAndFillsABasinBottomUp) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const int G = yard.G;
    yard.World().FillBox({8, G - 16, -8}, {24, G, 8}, 0);
    const glm::ivec3 seed(0, G, 0);
    const sim::FlowResult flow = sim::FlowOperator(yard.World(), yard.ctx.walls, PourParams(yard.table, seed));
    ASSERT_EQ(flow.front.size(), yard.table.spells[sim::kMoltenPour].frontCount);
    for (const auto* cells : {&flow.fall, &flow.run, &flow.front})
        for (const glm::ivec3& c : *cells) EXPECT_LE(c.y, seed.y) << "never up";
    for (const glm::ivec3& c : flow.front) {
        EXPECT_EQ(c.y, G - 16) << "the pool is the pit's floor";
        EXPECT_GE(c.x, 8);
        EXPECT_LT(c.x, 24);
    }
    // Key order: the front's keys are the smallest of every popped cell.
    const sim::FlowKey last = sim::FlowKeyOf(flow.front.back(), seed);
    for (const glm::ivec3& c : flow.run) EXPECT_LE(last, sim::FlowKeyOf(c, seed));
    // The same world and seed give the same roles.
    const sim::FlowResult again = sim::FlowOperator(yard.World(), yard.ctx.walls, PourParams(yard.table, seed));
    EXPECT_EQ(again.front, flow.front);
    EXPECT_EQ(again.run, flow.run);
    EXPECT_EQ(again.fall, flow.fall);
}

// Poured on a platform near its edge, the Flow runs to the edge, falls down its face and pools on the ground below.
TEST(Propagate, FlowPoursOverAnEdge) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const int G = yard.G;
    yard.World().FillBox({-16, G, -16}, {16, G + 16, 16}, yard.Ids().stone);
    const glm::ivec3 seed(12, G + 16, 0);
    const sim::FlowResult flow = sim::FlowOperator(yard.World(), yard.ctx.walls, PourParams(yard.table, seed));
    ASSERT_FALSE(flow.front.empty());
    for (const glm::ivec3& c : flow.front) EXPECT_EQ(c.y, G) << "the pool is on the ground below";
    bool overTheEdge = false;
    for (const glm::ivec3& c : flow.fall) overTheEdge = overTheEdge || (c.x >= 16 && c.y < G + 16);
    for (const glm::ivec3& c : flow.run) overTheEdge = overTheEdge || (c.x >= 16 && c.y < G + 16);
    EXPECT_TRUE(overTheEdge) << "it falls down the face";
}

// --- Molten Pour ----------------------------------------------------------------------------------------------------

// Lobbed at a wall, Molten Pour lands slag down the face and pools it at the foot; the slag sets into knit a second
// after it lands and all of it is gone at the lifetime.
TEST(Magic, MoltenPourPlatesAWallAndSets) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const int G = yard.G;
    yard.World().FillBox({32, G, -48}, {40, G + 32, 48}, yard.Ids().stone);   // wider than the pool
    const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
    const sim::SpellGesture fall = GestureOf(sim::GestureKind::Fall, {yard.Q({4.0, yard.ground + 3.0, 0.0625}), yard.Q({4.0, yard.ground + 2.5, 0.0625})});
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kMoltenPour, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0), fall);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_EQ(r.face, 1) << "the wall's -x face";
    EXPECT_NEAR(r.hitQ.y / 128.0, yard.ground + 2.5, 0.25) << "lobbed at the release point";
    EXPECT_GT(r.flightTicks, 0);
    EXPECT_EQ(r.flightTicks, r.stats.arcTicks);
    EXPECT_GT(r.stats.planned, 100u);
    EXPECT_LE(r.stats.planned, yard.table.spells[sim::kMoltenPour].fillCap);
    EXPECT_EQ(r.manaCost, 1800);
    const uint8_t slag = yard.Ids().slag, knit = yard.Ids().knitIron;
    const glm::ivec3 lo(-8, G - 2, -32), hi(40, G + 40, 32);
    yard.RunTo(t0 + r.flightTicks - 1);
    EXPECT_EQ(yard.Count(lo, hi, slag), 0u) << "in flight";
    yard.RunTo(t0 + r.flightTicks);
    EXPECT_EQ(yard.Count(lo, hi, slag), r.stats.planned) << "landed";
    EXPECT_EQ(yard.World().GetVoxel({31, G + 4, 0}), slag) << "a shell down the face";
    EXPECT_EQ(yard.World().GetVoxel({31, G, 0}), slag) << "the pool at its foot";
    yard.RunTo(t0 + r.flightTicks + 59);
    EXPECT_EQ(yard.Count(lo, hi, knit), 0u);
    yard.RunTo(t0 + r.flightTicks + 60);
    EXPECT_EQ(yard.Count(lo, hi, slag), 0u) << "set";
    EXPECT_EQ(yard.Count(lo, hi, knit), r.stats.planned);
    yard.RunTo(t0 + yard.table.spells[sim::kMoltenPour].lifetimeTicks);
    EXPECT_EQ(yard.Count(lo, hi, knit), 0u) << "gone at its lifetime";
    EXPECT_EQ(yard.ctx.registry.LiveVoxels(0), 0u);
}

// Slag burns: an enemy within 1 m of the landing takes the splash on arrival, and the hazard scan burns one standing
// in the slag every 6 ticks while it is slag, credited to the caster; set knit burns nobody.
TEST(Magic, SlagBurnsOnContactUntilItSets) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const int G = yard.G;
    yard.World().FillBox({32, G, -48}, {40, G + 32, 48}, yard.Ids().stone);   // wider than the pool
    const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
    // The enemy stands at the wall's foot under the pour, the ally beside it.
    const std::vector<sim::SpellTarget> standing = {{2, 2, glm::vec3(3.8f, static_cast<float>(yard.ground), 0.06f)},
                                                    {3, 1, glm::vec3(3.8f, static_cast<float>(yard.ground), 0.5f)}};
    const sim::SpellGesture fall = GestureOf(sim::GestureKind::Fall, {yard.Q({4.0, yard.ground + 1.5, 0.0625}), yard.Q({4.0, yard.ground + 1.0, 0.0625})});
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kMoltenPour, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0), fall, standing);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    const uint32_t landed = t0 + r.flightTicks;
    uint32_t splash = 0, burns = 0;
    for (const sim::ScheduledResult& s : yard.RunTo(landed + 120, standing)) {
        for (const sim::SpellDamage& d : s.damage) {
            EXPECT_EQ(d.player, 2) << "the enemy only";
            EXPECT_EQ(d.source, 1);
            if (d.cause == 1) {
                EXPECT_EQ(d.amount, 20);
                ++splash;
            } else {
                EXPECT_EQ(d.cause, 3);
                EXPECT_EQ(d.amount, 1);
                ++burns;
            }
        }
    }
    EXPECT_EQ(splash, 1u);
    EXPECT_GE(burns, 8u) << "about ten scans in the slag's second";
    EXPECT_LE(burns, 11u) << "none once it has set";
}

// --- Rising Spire ---------------------------------------------------------------------------------------------------

// Cast at the caster's own feet, Rising Spire's Lift carries it up ahead of each band: when the Lift ends it stands on the
// spire's top with knit under its whole column, and a ledge stands out every metre on its side, at every voxel size.
TEST(Magic, RisingSpireLiftsItsCaster) {
    for (const float vs : {0.1f, 0.125f, 0.15f}) {
        MetalYard yard(vs);
        ASSERT_TRUE(yard.initialized);
        const float ground = static_cast<float>(yard.ground);
        Fighter& rider = yard.Add(1, 1, {0.3f, ground, 0.3f});
        yard.Settle();
        const glm::dvec3 eye = glm::dvec3(rider.state.position) + glm::dvec3(0.0, kEye, 0.0);
        // Straight down, a little toward -x: the ledges stand out on the +x side (toward the caster's eye).
        const sim::CastSpellResult r = yard.Cast(sim::kRisingSpire, 1, 1, eye, eye + glm::dvec3(-0.02, -1.0, 0.0));
        ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
        ASSERT_EQ(r.zonesStarted.size(), 1u);
        const sim::Zone lift = r.zonesStarted[0];
        EXPECT_EQ(lift.kind, sim::ZoneKind::Lift);
        EXPECT_EQ(lift.teamMask, 0xFF) << "everyone";
        yard.RunTo(lift.EndTick() + 90);
        const float top = rider.state.position.y;
        const int G = yard.G;
        const glm::ivec3 column = yard.World().WorldToVoxel(glm::dvec3(rider.state.position));
        int knitTop = G - 1;
        while (yard.World().GetVoxel({column.x, knitTop + 1, column.z}) == yard.Ids().knitIron) ++knitTop;
        EXPECT_GE(knitTop - G + 1, yard.V(5.5)) << "vs " << vs << ": a spire about 6 m tall under the rider";
        for (int y = G; y <= knitTop; ++y) EXPECT_EQ(yard.World().GetVoxel({column.x, y, column.z}), yard.Ids().knitIron) << "vs " << vs << " y " << y;
        EXPECT_NEAR(top, static_cast<float>((knitTop + 1) * static_cast<double>(vs)), 0.1f) << "vs " << vs << ": the rider stands on the top";
        // A ledge a metre up on the +x side, out of the column.
        const glm::ivec3 seed = r.hitQ >> 4;
        const sim::SpellDef& d = yard.table.spells[sim::kRisingSpire];
        const int ledgeY = seed.y + d.ledgeSpacing - 1;
        EXPECT_EQ(yard.World().GetVoxel({seed.x + d.plumeSeedRadius + 2, ledgeY, seed.z}), yard.Ids().knitIron) << "vs " << vs;
        EXPECT_EQ(yard.World().GetVoxel({seed.x - d.plumeSeedRadius - 2, ledgeY, seed.z}), 0) << "vs " << vs << ": not on the far side";
    }
}

// The bands land in order from the bottom, each once the Lift's floor is above it (the timing rule), and the Lift ends a
// lead after the last band.
TEST(Magic, RisingSpireBandsFollowTheLift) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(2.0625, yard.ground + kEye, 0.0625);
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kRisingSpire, 1, 1, eye, glm::dvec3(0.0625, yard.ground, 0.0625));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    std::vector<uint32_t> bandTicks;
    std::vector<int> bandBottoms;
    for (uint32_t t = t0; t <= t0 + 200; ++t)
        for (const sim::BlastResult& g : yard.Tick().groups)
            for (const sim::AppliedOp& a : g.ops)
                if (a.op.kind == sim::WorldOpKind::MapMask && a.op.map == voxel::Map_Knit) {
                    bandTicks.push_back(t);
                    bandBottoms.push_back(a.op.min.y);
                }
    ASSERT_EQ(bandTicks.size(), 6u);
    for (size_t k = 1; k < bandTicks.size(); ++k) {
        EXPECT_GT(bandTicks[k], bandTicks[k - 1]);
        EXPECT_GT(bandBottoms[k], bandBottoms[k - 1]);
    }
    EXPECT_GE(bandTicks[0], r.zoneTick + 7u) << "the floor is a metre and two voxels up";
    EXPECT_EQ(r.zonesStarted[0].EndTick(), bandTicks.back() + yard.table.zoneLeadTicks);
}

// --- the Rise gesture -----------------------------------------------------------------------------------------------

// A Rise whose end is a metre or more above its start makes knit stairs from the start toward the end: steps 1 m wide,
// 0.375 m rise and 0.5 m run, up to the end's height; the mana counts their length.
TEST(Gesture, ARiseMakesStairs) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const int G = yard.G;
    yard.World().FillBox({48, G, -24}, {64, G + 24, 24}, yard.Ids().stone);   // a block 3 m tall from x = 6 m
    const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
    const sim::SpellGesture rise = GestureOf(sim::GestureKind::Rise, {yard.Q({2.0, yard.ground, 0.0625}), yard.Q({3.25, yard.ground + 1.0, 0.0625}),
                                                                      yard.Q({4.5, yard.ground + 2.0, 0.0625}), yard.Q({5.5, yard.ground + 2.8, 0.0625}),
                                                                      yard.Q({6.25, yard.ground + 3.0, 0.0625})});
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kRisingSpire, 1, 1, eye, glm::dvec3(2.0, yard.ground, 0.0625), rise);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_EQ(r.stats.riseShape, static_cast<uint8_t>(sim::RiseKind::Stairs));
    EXPECT_EQ(r.stats.steps, 8u) << "3 m in steps of 0.375 m";
    EXPECT_TRUE(r.zonesStarted.empty()) << "stairs lift nobody";
    EXPECT_EQ(r.hitQ, rise.pointsQ.front()) << "the projectile flies to S";
    EXPECT_EQ(r.manaCost, sim::SpellManaCost(yard.table, sim::kRisingSpire, rise));
    EXPECT_EQ(r.manaCost, 600u + 300u * r.stats.drawnLengthQ / 128u);
    yard.RunTo(t0 + 120);
    const uint8_t knit = yard.Ids().knitIron;
    for (int i = 0; i < 8; ++i) {
        const int x = 16 + 4 * i + 2, tread = G + 3 * (i + 1);
        EXPECT_EQ(yard.World().GetVoxel({x, tread - 1, 0}), knit) << "step " << i;
        EXPECT_EQ(yard.World().GetVoxel({x, tread, 0}), 0) << "over step " << i;
        EXPECT_EQ(yard.World().GetVoxel({x, tread - 1, 3}), knit) << "1 m wide";
        EXPECT_NE(yard.World().GetVoxel({x, tread - 1, 5}), knit) << "1 m wide";
    }
    EXPECT_EQ(yard.Count({16, G, -8}, {48, G + 24, 8}, knit), r.stats.planned);
}

// A Rise that ends within a metre of its start's height and 3 m or more away makes a knit deck between them, level at
// the lower end, 1.5 m wide and 0.25 m thick, when both ends have ground within 0.5 m; an end over a moat makes the
// pillar instead.
TEST(Gesture, ALevelRiseMakesABridge) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const int G = yard.G;
    yard.World().FillBox({24, G - 16, -24}, {56, G, 24}, 0);   // a moat from 3 to 7 m, 2 m deep
    const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
    const sim::SpellGesture rise = GestureOf(sim::GestureKind::Rise, {yard.Q({2.0, yard.ground, 0.0625}), yard.Q({3.5, yard.ground, 0.0625}),
                                                                      yard.Q({5.0, yard.ground, 0.0625}), yard.Q({6.5, yard.ground, 0.0625}),
                                                                      yard.Q({8.0, yard.ground, 0.0625})});
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kRisingSpire, 1, 1, eye, glm::dvec3(2.0, yard.ground, 0.0625), rise);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_EQ(r.stats.riseShape, static_cast<uint8_t>(sim::RiseKind::Bridge));
    EXPECT_EQ(r.stats.drawnLengthQ, 768u) << "6 m";
    yard.RunTo(t0 + 120);
    const uint8_t knit = yard.Ids().knitIron;
    for (int x = 24; x < 56; x += 4) {
        EXPECT_EQ(yard.World().GetVoxel({x, G - 1, 0}), knit) << x;
        EXPECT_EQ(yard.World().GetVoxel({x, G - 2, 0}), knit) << x;
        EXPECT_NE(yard.World().GetVoxel({x, G - 3, 0}), knit) << x << ": 0.25 m thick";
        EXPECT_EQ(yard.World().GetVoxel({x, G - 1, 5}), knit) << x << ": 1.5 m wide";
        EXPECT_NE(yard.World().GetVoxel({x, G - 1, 7}), knit) << x;
        EXPECT_EQ(yard.World().GetVoxel({x, G, 0}), 0) << x << ": level with the banks";
    }

    // The end over the moat's middle has no ground within 0.5 m: the pillar at S, with its Lift.
    const sim::SpellGesture short_ = GestureOf(sim::GestureKind::Rise, {yard.Q({-6.0, yard.ground, 3.0}), yard.Q({-4.1, yard.ground, 3.0}),
                                                                        yard.Q({-2.2, yard.ground, 3.0}), yard.Q({-0.3, yard.ground + 0.6, 3.0})});
    yard.World().FillBox({-40, G - 16, 8}, {0, G, 40}, 0);
    const sim::CastSpellResult pillar = yard.Cast(sim::kRisingSpire, 2, 1, eye, glm::dvec3(-6.0, yard.ground, 3.0), short_);
    EXPECT_EQ(pillar.stats.riseShape, static_cast<uint8_t>(sim::RiseKind::Pillar));
    if (pillar.refusal == sim::SpellRefusal::None) {
        ASSERT_EQ(pillar.zonesStarted.size(), 1u);
        EXPECT_EQ(pillar.zonesStarted[0].kind, sim::ZoneKind::Lift);
        EXPECT_EQ(pillar.manaCost, yard.table.spells[sim::kRisingSpire].manaCost);
    }
}

// --- Rebar Lance ----------------------------------------------------------------------------------------------------

// Rebar Lance bores two level sockets 1 m apart up a stone wall and fills each with knit that stands 1.25 m out of the
// face: two pegs. Over an iron row the upper socket is not made: one peg.
TEST(Magic, RebarLanceSetsTwoPegs) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const int G = yard.G;
    yard.World().FillBox({32, G, -24}, {48, G + 40, 24}, yard.Ids().stone);
    const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
    const uint32_t t0 = yard.simulation.Tick();
    // Aimed a little down at the face: the bore is level all the same.
    const sim::CastSpellResult r = yard.Cast(sim::kRebarLance, 1, 1, eye, glm::dvec3(4.0, yard.ground + 1.0, 0.0625));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_EQ(r.stats.sockets, 2u);
    ASSERT_EQ(r.zonesStarted.size(), 1u);
    EXPECT_EQ(r.zonesStarted[0].kind, sim::ZoneKind::Root);
    yard.RunTo(t0 + r.flightTicks + 1);
    const uint8_t knit = yard.Ids().knitIron;
    const int y0 = r.hitQ.y >> 4, z = r.hitQ.z >> 4;
    for (const int y : {y0, y0 + 8}) {
        for (int x = 22; x < 32; ++x) EXPECT_EQ(yard.World().GetVoxel({x, y, z}), knit) << "peg out of the face at x " << x << " y " << y;
        EXPECT_NE(yard.World().GetVoxel({20, y, z}), knit) << "1.25 m out at most";
        EXPECT_EQ(yard.World().GetVoxel({33, y, z}), knit) << "the socket is filled";
    }
    EXPECT_EQ(yard.World().GetVoxel({28, y0 + 4, z}), 0) << "air between the pegs";

    // Over an iron row a metre above the hit: one socket, one peg.
    yard.World().FillBox({32, G + 24, 8}, {48, G + 40, 24}, yard.Ids().iron);
    const sim::CastSpellResult one = yard.Cast(sim::kRebarLance, 1, 1, glm::dvec3(0.0625, yard.ground + 2.6, 2.0625),
                                               glm::dvec3(4.0, yard.ground + 2.2, 2.0625));
    ASSERT_EQ(one.refusal, sim::SpellRefusal::None) << sim::ToString(one.refusal);
    EXPECT_EQ(one.stats.sockets, 1u);
}

// --- Rune Trap ------------------------------------------------------------------------------------------------------

// A rune arms a second after its cast and bursts under the first enemy whose feet lie in it, once: the rune ends at that
// tick, Root and Chill round it start a lead later with 15 damage, and an ice patch forms round the victim. An ally on
// the rune sets nothing off.
TEST(Magic, ARuneBurstsOnceUnderTheFirstEnemy) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
    const sim::SpellGesture fall = GestureOf(sim::GestureKind::Fall, {yard.Q({6.0, yard.ground + 0.5, 0.0625}), yard.Q({6.0, yard.ground, 0.0625})});
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kRuneTrap, 1, 1, eye, glm::dvec3(6.0, yard.ground, 0.0625), fall);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    ASSERT_EQ(r.zonesStarted.size(), 1u);
    const sim::Zone rune = r.zonesStarted[0];
    EXPECT_EQ(rune.kind, sim::ZoneKind::Rune);
    EXPECT_EQ(rune.startTick, t0 + 60u) << "armed after a second";
    EXPECT_EQ(rune.duration, 3600u) << "60 s";
    EXPECT_EQ(rune.teamMask, sim::ZoneTeamMask(sim::ZoneTarget::Enemies, 1));
    const glm::vec3 onRune(6.0f, static_cast<float>(yard.ground), 0.06f);
    // An ally stands on the rune from the start; the enemy walks onto it at t0 + 90.
    std::vector<sim::SpellTarget> targets = {{3, 1, onRune}, {2, 2, glm::vec3(10.0f, static_cast<float>(yard.ground), 0.0f)}};
    for (const sim::ScheduledResult& s : yard.RunTo(t0 + 89, targets)) EXPECT_TRUE(s.runeBursts.empty());
    targets[1].feet = onRune + glm::vec3(0.4f, 0.0f, 0.0f);
    const sim::ScheduledResult burst = yard.Tick(targets);
    const uint32_t t = t0 + 90;
    ASSERT_EQ(burst.runeBursts.size(), 1u);
    EXPECT_EQ(burst.runeBursts[0], std::make_pair(rune.zoneId, uint8_t{2}));
    ASSERT_EQ(burst.zonesCancelled.size(), 1u);
    EXPECT_EQ(burst.zonesCancelled[0], std::make_pair(rune.zoneId, t));
    ASSERT_EQ(burst.zonesStarted.size(), 2u);
    EXPECT_EQ(burst.zonesStarted[0].kind, sim::ZoneKind::Root);
    EXPECT_EQ(burst.zonesStarted[1].kind, sim::ZoneKind::Chill);
    for (const sim::Zone& z : burst.zonesStarted) EXPECT_EQ(z.startTick, t + yard.table.zoneLeadTicks);
    uint32_t damage = 0, bursts = 0;
    for (const sim::ScheduledResult& s : yard.RunTo(t + 300, targets)) {
        bursts += static_cast<uint32_t>(s.runeBursts.size());
        for (const sim::SpellDamage& d : s.damage) {
            EXPECT_EQ(d.player, 2);
            damage += d.amount;
        }
    }
    EXPECT_EQ(bursts, 0u) << "once";
    EXPECT_EQ(damage, 15u);
    const uint32_t ice = yard.Count({28, yard.G - 1, -20}, {68, yard.G + 2, 20}, yard.Ids().ice);
    EXPECT_GT(ice, 100u);
    EXPECT_LE(ice, 400u);
    EXPECT_NE(yard.World().GetVoxel(yard.World().WorldToVoxel(glm::dvec3(targets[1].feet) + glm::dvec3(0.0, 0.5, 0.0))), yard.Ids().ice)
        << "the victim is never entombed";
}

// One rune per caster: a second Rune Trap ends the caster's first; another caster's rune stays.
TEST(Magic, ANewRuneCancelsTheCastersOld) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
    const auto runeAt = [&](uint8_t player, double x, double z) {
        const sim::SpellGesture fall = GestureOf(sim::GestureKind::Fall, {yard.Q({x, yard.ground + 0.5, z}), yard.Q({x, yard.ground, z})});
        return yard.Cast(sim::kRuneTrap, player, 1, eye, glm::dvec3(x, yard.ground, z), fall);
    };
    const sim::CastSpellResult first = runeAt(1, 5.0, 0.0);
    const sim::CastSpellResult other = runeAt(2, 5.0, 4.0);
    ASSERT_EQ(first.refusal, sim::SpellRefusal::None);
    ASSERT_EQ(other.refusal, sim::SpellRefusal::None);
    yard.RunTo(first.runTick + 70);
    const sim::CastSpellResult second = runeAt(1, -5.0, 0.0);
    ASSERT_EQ(second.refusal, sim::SpellRefusal::None);
    ASSERT_EQ(second.zonesCancelled.size(), 1u);
    EXPECT_EQ(second.zonesCancelled[0].first, first.zonesStarted[0].zoneId);
    EXPECT_EQ(second.zonesCancelled[0].second, second.runTick);
    yard.Tick();
    EXPECT_EQ(yard.simulation.Zones().Find(first.zonesStarted[0].zoneId), nullptr);
    EXPECT_NE(yard.simulation.Zones().Find(other.zonesStarted[0].zoneId), nullptr);
    // A rune not yet armed ends before it starts.
    const sim::CastSpellResult third = runeAt(1, -5.0, 4.0);
    ASSERT_EQ(third.zonesCancelled.size(), 1u);
    EXPECT_EQ(third.zonesCancelled[0].second, second.zonesStarted[0].startTick);
}

// A carve that takes the voxel under the rune breaks it: it ends with no burst, even when an enemy steps on it after.
TEST(Magic, ACarveUnderARuneBreaksIt) {
    MetalYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(0.0625, yard.ground + kEye, 0.0625);
    const sim::SpellGesture fall = GestureOf(sim::GestureKind::Fall, {yard.Q({5.0, yard.ground + 0.5, 0.0625}), yard.Q({5.0, yard.ground, 0.0625})});
    const sim::CastSpellResult r = yard.Cast(sim::kRuneTrap, 1, 1, eye, glm::dvec3(5.0, yard.ground, 0.0625), fall);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    yard.RunTo(r.runTick + 70);
    const glm::ivec3 floor = yard.World().WorldToVoxel(glm::dvec3(5.0, yard.ground - 0.05, 0.0625));
    const voxel::SphereQ hole = voxel::QuantizeSphere(glm::dvec3(floor) + glm::dvec3(0.5), 1.0 / static_cast<double>(yard.vs));
    sim::BlastAt(yard.simulation, hole, voxel::QuantizePower(6.0f), yard.World().GetVoxel(floor));
    ASSERT_EQ(yard.World().GetVoxel(floor), 0);
    const std::vector<sim::SpellTarget> enemy = {{2, 2, glm::vec3(5.0f, static_cast<float>(yard.ground), 0.06f)}};
    uint32_t broken = 0, bursts = 0;
    for (const sim::ScheduledResult& s : yard.RunTo(r.runTick + 120, enemy)) {
        broken += s.runesBroken;
        bursts += static_cast<uint32_t>(s.runeBursts.size());
    }
    EXPECT_EQ(broken, 1u);
    EXPECT_EQ(bursts, 0u);
}
