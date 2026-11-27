// Thermal (docs/design/magic.md "The sixteen spells", spells 5-8, "Damage and hazards"), without fire spread: pitch
// burns where it lands and expires, Heat makes metal hot (half its hardness, so a blast carves it) until a Cool, melts
// ice and thaws rime; Frost quenches hot metal and freezes pitch into ice; Meltdown Beam bores through what its Heat
// softened; the hazard scan burns players in pitch and against hot metal, and Burning burns on.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
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
    constexpr float kVs = 0.125f;
    constexpr int G = 96;                         // flat ground: voxels y < 96, its top at 12 m
    constexpr double kGround = G * 0.125;

    sim::SimulationConfig ThermalConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.maxParticles = 0;
        config.jobThreads = 2;
        config.world.voxelSize = kVs;
        config.world.windowSize = {8, 6, 8};
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        return config;
    }

    /// Flat ground at 0.125 m: spells cast and followed up as a server does, players standing as targets.
    struct ThermalYard {
        ThermalYard() : simulation(ThermalConfig()) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(G, ids.grass, ids.stone));
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
            table = sim::BuildSpellTable(kVs);
            ctx.walls = sim::WindowWalls(simulation.World());
        }

        /// One tick's follow-ups with `targets` standing, then physics.
        sim::ScheduledResult Tick(const std::vector<sim::SpellTarget>& targets = {}) {
            sim::ScheduledResult r = sim::RunScheduled(simulation, table, ctx, simulation.Tick(), targets);
            simulation.StepPhysics();
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
            return r;
        }

        /// Ticks up to `last` (the follow-ups of `last` included); every tick's result.
        std::vector<sim::ScheduledResult> RunTo(uint32_t last, const std::vector<sim::SpellTarget>& targets = {}) {
            std::vector<sim::ScheduledResult> out;
            while (simulation.Tick() <= last) out.push_back(Tick(targets));
            return out;
        }

        sim::CastSpellResult Cast(sim::SpellId spell, uint8_t player, sim::TeamId team, const glm::dvec3& eye, const glm::dvec3& target,
                                  const sim::SpellGesture& gesture = {}, const std::vector<sim::SpellTarget>& targets = {}) {
            sim::SpellCast c;
            c.player = player;
            c.team = team;
            c.spell = spell;
            c.eye = eye;
            c.dir = glm::vec3(glm::normalize(target - eye));
            c.gesture = gesture;
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
        static glm::ivec3 Q(const glm::dvec3& metres) {
            return {static_cast<int32_t>(std::llround(metres.x * 128.0)), static_cast<int32_t>(std::llround(metres.y * 128.0)),
                    static_cast<int32_t>(std::llround(metres.z * 128.0))};
        }
        uint64_t Hash() const {
            const voxel::World& world = simulation.World();
            return voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize()).hash;
        }

        sim::Simulation simulation;
        sim::SpellTable table;
        sim::CastContext ctx;
        bool initialized = false;
    };

    sim::SpellGesture GestureOf(sim::GestureKind kind, std::vector<glm::ivec3> points) {
        sim::SpellGesture g;
        g.kind = kind;
        g.pointsQ = std::move(points);
        return g;
    }

    /// The damage of `results` to `player` by cause: {spell hits (cause 1), hazard and Burning (cause 3)}.
    std::pair<uint32_t, uint32_t> DamageTo(const std::vector<sim::ScheduledResult>& results, uint8_t player) {
        std::pair<uint32_t, uint32_t> out{0, 0};
        for (const sim::ScheduledResult& r : results)
            for (const sim::SpellDamage& d : r.damage) {
                if (d.player != player) continue;
                (d.cause == 3 ? out.second : out.first) += d.amount;
            }
        return out;
    }
}

// Pitchfall lobbed onto open ground: a carpet of pitch lands on the arrival; an enemy standing in it takes the splash
// and about 15 HP/s, an ally nothing; it burns out with its lifetime (no spread) and the hazard stops with it.
TEST(Magic, PitchBurnsThenExpires) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(0.0625, kGround + kEye, 0.0625);
    const std::vector<sim::SpellTarget> standing = {{2, 2, glm::vec3(4.1f, static_cast<float>(kGround), 0.3f)},
                                                    {3, 1, glm::vec3(4.1f, static_cast<float>(kGround), -0.4f)}};
    const sim::SpellGesture fall = GestureOf(sim::GestureKind::Fall, {ThermalYard::Q({4.0, kGround + 0.5, 0.0625}), ThermalYard::Q({4.0, kGround, 0.0625})});
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kPitchfall, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0), fall, standing);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_GT(r.flightTicks, 0);
    EXPECT_GT(r.stats.planned, 100u);
    EXPECT_EQ(r.manaCost, 1800);
    const uint8_t pitch = yard.Ids().pitch;
    const glm::ivec3 lo(-16, G - 2, -48), hi(80, G + 8, 48);
    const uint32_t landed = t0 + r.flightTicks;
    yard.RunTo(landed - 1, standing);
    EXPECT_EQ(yard.Count(lo, hi, pitch), 0u) << "in flight";
    const std::vector<sim::ScheduledResult> second = yard.RunTo(landed + 60, standing);
    const uint32_t burning = yard.Count(lo, hi, pitch);
    EXPECT_GT(burning, 100u) << "landed";
    EXPECT_LE(burning, r.stats.planned);
    EXPECT_EQ(yard.World().GetVoxel({28, G, 0}), pitch) << "on the floor where it landed";
    const auto [enemyHit, enemyBurn] = DamageTo(second, 2);
    EXPECT_EQ(enemyHit, 20u) << "the splash";
    EXPECT_GE(enemyBurn, 13u) << "about 15 HP in its first second";
    EXPECT_LE(enemyBurn, 17u);
    EXPECT_EQ(DamageTo(second, 3), (std::pair<uint32_t, uint32_t>{0, 0})) << "friendly fire is off";
    // The pitch is no collider: a player walks through it.
    EXPECT_FALSE(sim::IsCollider(yard.simulation.Materials(), pitch));
    EXPECT_TRUE(sim::IsOpenId(pitch));
    yard.RunTo(t0 + yard.table.spells[sim::kPitchfall].lifetimeTicks, standing);
    EXPECT_EQ(yard.Count(lo, hi, pitch), 0u) << "burnt out at its lifetime";
    EXPECT_EQ(yard.ctx.registry.LiveVoxels(0), 0u);
    const std::vector<sim::ScheduledResult> after = yard.RunTo(yard.simulation.Tick() + 60, standing);
    EXPECT_EQ(DamageTo(after, 2).second, 0u) << "nothing burns once it is gone";
    std::printf("pitchfall: %u planned, %u burning, flight %u ticks\n", r.stats.planned, burning, r.flightTicks);
}

// Heat makes knit blastable: a basic blast on fresh knit is absorbed (no carve, a 0.25 m ablation), but where Meltdown
// Beam's Heat has turned the plate hot (3.5) the same blast carves it; after 4 s the rest cools back into plain knit.
TEST(Magic, HeatMakesKnitBlastable) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& ids = yard.Ids();
    yard.World().FillBox({32, G, -16}, {36, G + 24, 16}, ids.knitIron);   // a knit plate 0.5 m thick, x 4..4.5 m
    const uint32_t power = voxel::QuantizePower(6.0f);
    // Cold: the absorb rule.
    const sim::BlastResult cold = sim::BlastAt(yard.simulation, voxel::QuantizeSphere(glm::dvec3(4.0, kGround + 2.5, -1.4) / 0.125, 1.2 / 0.125),
                                               static_cast<uint16_t>(power), ids.knitIron);
    ASSERT_FALSE(cold.ops.empty());
    EXPECT_EQ(cold.ops[0].op.kind, sim::WorldOpKind::MapShape) << "absorbed: an Ablate patch, no carve";
    // The beam at the plate at eye height.
    const glm::dvec3 eye(0.0625, kGround + kEye, 0.5);
    const sim::CastSpellResult beam = yard.Cast(sim::kMeltdownBeam, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(beam.refusal, sim::SpellRefusal::None) << sim::ToString(beam.refusal);
    EXPECT_GT(beam.stats.heated, 0u);
    const uint32_t landed = yard.simulation.Tick() + beam.flightTicks;
    yard.RunTo(landed);
    const glm::ivec3 lo(28, G, -16), hi(40, G + 24, 16);
    const uint32_t hot = yard.Count(lo, hi, ids.knitIronHot);
    EXPECT_GT(hot, 0u) << "the Heat round the bore";
    EXPECT_EQ(yard.World().GetVoxel({33, static_cast<int>((kGround + kEye) / 0.125), 4}), 0u) << "bored through the plate";
    // A blast at the hot rim beside the bore carves it.
    const glm::ivec3 rim(32, static_cast<int>((kGround + kEye) / 0.125) + 3, 4);
    ASSERT_EQ(yard.World().GetVoxel(rim), ids.knitIronHot);
    const sim::BlastResult blast = sim::BlastAt(yard.simulation, voxel::QuantizeSphere((glm::dvec3(rim) + glm::dvec3(0.0, 0.5, 0.5)) / 1.0, 1.2 / 0.125),
                                                static_cast<uint16_t>(power), ids.knitIronHot);
    ASSERT_FALSE(blast.ops.empty());
    EXPECT_EQ(blast.ops[0].op.kind, sim::WorldOpKind::CarveSphere) << "hot knit is not absorbed";
    uint32_t carvedHot = 0;
    for (const voxel::VoxelEdit& e : blast.ops[0].edits) carvedHot += e.oldId == ids.knitIronHot ? 1u : 0u;
    EXPECT_GT(carvedHot, 0u);
    // It cools after 4 s: no hot knit left, the rest plain knit again.
    yard.RunTo(yard.simulation.Tick() + yard.table.spells[sim::kMeltdownBeam].heatLifetimeTicks);
    EXPECT_EQ(yard.Count(lo, hi, ids.knitIronHot), 0u) << "cooled";
    EXPECT_EQ(yard.ctx.registry.LiveVoxels(1), 0u);
    std::printf("heat: %u hot knit voxels, the blast carved %u\n", hot, carvedHot);
}

// Frost quenches hot metal: an iron block heated by Meltdown Beam, then rimed by Shatter Spike's capsule: the hot iron in
// the capsule is plain iron again, at once (the Rime map's quench rows).
TEST(Magic, FrostQuenchesHotMetal) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& ids = yard.Ids();
    yard.World().FillBox({32, G, -16}, {96, G + 24, 16}, ids.iron);   // x 4..12 m
    const glm::dvec3 eye(0.0625, kGround + kEye, 0.5);
    const sim::CastSpellResult beam = yard.Cast(sim::kMeltdownBeam, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(beam.refusal, sim::SpellRefusal::None) << sim::ToString(beam.refusal);
    EXPECT_GT(beam.stats.boreSlices, 4u) << "iron read through Heat: the beam bores it";
    yard.RunTo(yard.simulation.Tick() + beam.flightTicks + 1);
    const glm::ivec3 lo(28, G, -16), hi(48, G + 24, 16);
    const uint32_t hot = yard.Count(lo, hi, ids.ironHot);
    ASSERT_GT(hot, 0u);
    // The spike along the same line: its Rime capsule quenches what it reaches.
    const sim::CastSpellResult spike = yard.Cast(sim::kShatterSpike, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(spike.refusal, sim::SpellRefusal::None) << sim::ToString(spike.refusal);
    yard.RunTo(yard.simulation.Tick() + spike.flightTicks + 1);
    const uint32_t left = yard.Count(lo, hi, ids.ironHot);
    EXPECT_LT(left, hot) << "quenched";
    // Inside the spike's capsule no voxel is hot.
    const sim::SpellDef& d = yard.table.spells[sim::kShatterSpike];
    const glm::ivec3 axis = glm::ivec3(spike.hitQ) >> 4;
    for (int dx = 0; dx < 8; ++dx)
        for (int dy = -2; dy <= 2; ++dy)
            for (int dz = -2; dz <= 2; ++dz) {
                if (dy * dy + dz * dz > (d.rimeRadiusQ / 16 - 1) * (d.rimeRadiusQ / 16 - 1)) continue;
                EXPECT_NE(yard.World().GetVoxel(axis + glm::ivec3(dx, dy, dz)), ids.ironHot) << dx << " " << dy << " " << dz;
            }
    std::printf("quench: %u hot iron voxels, %u left after the spike\n", hot, left);
}

// Frost freezes pitch into ice: Flash Freeze over a burning carpet turns its pitch into walkable, slick ice.
TEST(Magic, FrostFreezesPitchIntoIce) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& ids = yard.Ids();
    const glm::dvec3 eye(0.0625, kGround + kEye, 0.0625);
    const sim::SpellGesture fall = GestureOf(sim::GestureKind::Fall, {ThermalYard::Q({4.0, kGround + 0.5, 0.0625}), ThermalYard::Q({4.0, kGround, 0.0625})});
    const sim::CastSpellResult pitchfall = yard.Cast(sim::kPitchfall, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0), fall);
    ASSERT_EQ(pitchfall.refusal, sim::SpellRefusal::None);
    yard.RunTo(yard.simulation.Tick() + pitchfall.flightTicks + 1);
    const glm::ivec3 lo(-16, G - 2, -48), hi(80, G + 8, 48);
    const uint32_t pitch = yard.Count(lo, hi, ids.pitch);
    ASSERT_GT(pitch, 100u);
    const uint32_t iceBefore = yard.Count(lo, hi, ids.ice);
    const sim::CastSpellResult freeze = yard.Cast(sim::kFlashFreeze, 1, 1, eye, glm::dvec3(4.0, kGround, 0.0625));
    ASSERT_EQ(freeze.refusal, sim::SpellRefusal::None) << sim::ToString(freeze.refusal);
    yard.RunTo(yard.simulation.Tick() + freeze.flightTicks + 1);
    const uint32_t pitchLeft = yard.Count(lo, hi, ids.pitch);
    const uint32_t ice = yard.Count(lo, hi, ids.ice);
    EXPECT_LT(pitchLeft, pitch / 2) << "the disc froze the carpet";
    EXPECT_GE(ice - iceBefore, (pitch - pitchLeft) / 2) << "into ice";
    EXPECT_EQ(voxel::WorldTurningTable().maps[voxel::Map_Freeze][ids.pitch], ids.ice);
    std::printf("freeze: %u pitch, %u left, %u ice; the disc: path %u, front %u, planned %u, hit (%d, %d, %d) face %u\n", pitch, pitchLeft, ice,
                freeze.stats.path, freeze.stats.front, freeze.stats.planned, freeze.hitQ.x >> 4, freeze.hitQ.y >> 4, freeze.hitQ.z >> 4, freeze.face);
}

// Meltdown Beam bores through knit and the stone behind it (metal read through its own Heat), carves the bore with its
// island search on the arrival, and an enemy in front of the terrain takes 45 and burns on: 8 HP/s for 3 s.
TEST(Magic, MeltdownBeamBoresAndBurns) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& ids = yard.Ids();
    yard.World().FillBox({32, G, -16}, {36, G + 24, 16}, ids.knitIron);   // knit, 0.5 m
    yard.World().FillBox({36, G, -16}, {80, G + 24, 16}, ids.stone);      // stone behind it
    const glm::dvec3 eye(0.0625, kGround + kEye, 0.5);
    const sim::CastSpellResult beam = yard.Cast(sim::kMeltdownBeam, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(beam.refusal, sim::SpellRefusal::None) << sim::ToString(beam.refusal);
    EXPECT_EQ(beam.manaCost, 1200);
    yard.RunTo(yard.simulation.Tick() + beam.flightTicks);
    const int row = static_cast<int>((kGround + kEye) / 0.125);
    int depth = 0;
    while (depth < 80 && yard.World().GetVoxel({32 + depth, row, 4}) == 0) ++depth;
    EXPECT_GE(depth * 0.125, 1.5) << "through 0.5 m of knit and on into the stone";
    std::printf("meltdown: bored %.2f m (%u slices), %u heated\n", depth * 0.125, beam.stats.boreSlices, beam.stats.heated);

    // A direct hit: the enemy in front of the wall takes 45 on the arrival, then burns about 24 over 3 s.
    const std::vector<sim::SpellTarget> enemy = {{2, 2, glm::vec3(2.0f, static_cast<float>(kGround), -1.0f)}};
    const glm::dvec3 eye2(0.0625, kGround + kEye, -1.0);
    const sim::CastSpellResult hit = yard.Cast(sim::kMeltdownBeam, 1, 1, eye2, eye2 + glm::dvec3(1.0, -0.3, 0.0), {}, enemy);
    ASSERT_EQ(hit.refusal, sim::SpellRefusal::None);
    const std::vector<sim::ScheduledResult> fight = yard.RunTo(yard.simulation.Tick() + hit.flightTicks + 200, enemy);
    const auto [direct, burn] = DamageTo(fight, 2);
    EXPECT_EQ(direct, 45u);
    EXPECT_GE(burn, 22u);
    EXPECT_LE(burn, 25u);
    EXPECT_TRUE(yard.ctx.burning.empty()) << "it burnt out";
}

// Firestorm Sweep drawn along a Line on the floor: a curtain of pitch 0.75 m high stands along the polyline, the floor
// under it is no different (stone does not heat), and an enemy beside the line takes 30 at the zone tick and burns on.
TEST(Magic, FirestormSweepAlongALine) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& ids = yard.Ids();
    const glm::dvec3 eye(0.0625, kGround + kEye, 0.0625);
    const sim::SpellGesture line = GestureOf(sim::GestureKind::Line, {ThermalYard::Q({3.0, kGround, -2.0}), ThermalYard::Q({3.4, kGround, -0.7}),
                                                                      ThermalYard::Q({3.4, kGround, 0.7}), ThermalYard::Q({3.0, kGround, 2.0})});
    const std::vector<sim::SpellTarget> enemy = {{2, 2, glm::vec3(4.0f, static_cast<float>(kGround), 0.0f)}};
    const uint32_t t0 = yard.simulation.Tick();
    const sim::CastSpellResult r = yard.Cast(sim::kFirestormSweep, 1, 1, eye, glm::dvec3(3.0, kGround, -2.0), line, enemy);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_GE(r.stats.drawnLengthQ, 4u * 128u);
    EXPECT_EQ(r.drawnPointsQ.size(), 4u);
    const std::vector<sim::ScheduledResult> first = yard.RunTo(r.zoneTick + 1, enemy);
    const glm::ivec3 lo(16, G, -32), hi(40, G + 12, 32);
    const uint32_t curtain = yard.Count(lo, hi, ids.pitch);
    EXPECT_GT(curtain, 150u) << "a curtain about 4 m long and 6 voxels high";
    EXPECT_EQ(yard.Count({16, G + 6, -32}, {40, G + 12, 32}, ids.pitch), 0u) << "0.75 m high";
    const auto [scorch, burn] = DamageTo(first, 2);
    EXPECT_EQ(scorch, 30u) << "the scorch at the zone tick";
    EXPECT_TRUE(yard.ctx.burning.contains(2));
    yard.RunTo(t0 + 400, enemy);
    EXPECT_EQ(yard.Count(lo, hi, ids.pitch), 0u) << "it burns out";
    std::printf("firestorm: curtain %u pitch voxels over %.2f m\n", curtain, r.stats.drawnLengthQ / 128.0);
}

// Wildfire climbs a stone face from S to E on a Rise: pitch up the face, none hanging in the air, and Heat melts the ice
// it touches (a collider that becomes air: a site check).
TEST(Magic, WildfireClimbsAFaceAndMeltsIce) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& ids = yard.Ids();
    yard.World().FillBox({32, G, -16}, {40, G + 40, 16}, ids.stone);   // a wall, its face at x = 4 m
    yard.World().FillBox({31, G + 8, -2}, {32, G + 12, 2}, ids.ice);   // ice on the face 1 m up
    const glm::dvec3 eye(0.0625, kGround + kEye, 0.0625);
    const sim::SpellGesture rise = GestureOf(sim::GestureKind::Rise, {ThermalYard::Q({4.0, kGround + 0.25, 0.0625}), ThermalYard::Q({4.0, kGround + 1.5, 0.0625}),
                                                                      ThermalYard::Q({4.0, kGround + 3.0, 0.0625})});
    const sim::CastSpellResult r = yard.Cast(sim::kWildfire, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0), rise);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_GT(r.stats.heated, 0u) << "the ice on the face";
    yard.RunTo(yard.simulation.Tick() + r.flightTicks + 1);
    const glm::ivec3 lo(24, G, -16), hi(32, G + 48, 16);
    const uint32_t pitch = yard.Count(lo, hi, ids.pitch);
    EXPECT_GT(pitch, 50u);
    int top = 0;
    for (int y = G; y < G + 48; ++y)
        if (yard.Count({24, y, -16}, {32, y + 1, 16}, ids.pitch) > 0) top = y;
    EXPECT_GE((top + 1 - G) * 0.125, 2.5) << "up the face toward E";
    EXPECT_LE((top + 1 - G) * 0.125, 3.6);
    EXPECT_LE(yard.Count({31, G + 8, -2}, {32, G + 12, 2}, ids.ice), 4u) << "melted where the pitch touched it";
    for (int y = G; y < G + 48; ++y)
        for (int z = -16; z < 16; ++z)
            for (int x = 24; x < 31; ++x)
                if (yard.World().GetVoxel({x, y, z}) == ids.pitch)
                    EXPECT_TRUE(yard.World().GetVoxel({x + 1, y, z}) != 0 || yard.World().GetVoxel({x, y - 1, z}) != 0 ||
                                yard.World().GetVoxel({x - 1, y, z}) != 0 || yard.World().GetVoxel({x, y + 1, z}) != 0 ||
                                yard.World().GetVoxel({x, y, z + 1}) != 0 || yard.World().GetVoxel({x, y, z - 1}) != 0)
                        << "pitch hangs at " << x << " " << y << " " << z;
    std::printf("wildfire: %u pitch voxels up to %.2f m, %u heated\n", pitch, (top + 1 - G) * 0.125, r.stats.heated);
}

// Hot metal burns: a player standing against hot iron takes 10 HP/s from the hazard scan until it cools.
TEST(Magic, HotMetalBurnsOnContactUntilItCools) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& ids = yard.Ids();
    yard.World().FillBox({32, G, -16}, {40, G + 24, 16}, ids.iron);
    const std::vector<sim::SpellTarget> enemy = {{2, 2, glm::vec3(3.68f, static_cast<float>(kGround), 1.5f)}};
    const glm::dvec3 eye(0.0625, kGround + kEye, 1.5);
    const sim::CastSpellResult beam = yard.Cast(sim::kMeltdownBeam, 1, 1, eye, eye + glm::dvec3(1.0, -0.05, 0.0));
    ASSERT_EQ(beam.refusal, sim::SpellRefusal::None);
    const uint32_t landed = yard.simulation.Tick() + beam.flightTicks;
    yard.RunTo(landed, enemy);
    const std::vector<sim::ScheduledResult> hot = yard.RunTo(landed + 60, enemy);
    EXPECT_GE(DamageTo(hot, 2).second, 8u);
    EXPECT_LE(DamageTo(hot, 2).second, 11u);
    yard.RunTo(landed + yard.table.spells[sim::kMeltdownBeam].heatLifetimeTicks + 1, enemy);
    const std::vector<sim::ScheduledResult> cold = yard.RunTo(yard.simulation.Tick() + 60, enemy);
    EXPECT_EQ(DamageTo(cold, 2).second, 0u) << "cooled";
}

// The ops of Thermal replay the same: two simulations fed the same casts end with the same voxels.
TEST(Magic, ThermalSpellsGiveTheSameVoxelsTwice) {
    ThermalYard a, b;
    ASSERT_TRUE(a.initialized);
    ASSERT_TRUE(b.initialized);
    for (ThermalYard* y : {&a, &b}) {
        y->World().FillBox({32, G, -16}, {36, G + 24, 16}, y->Ids().knitIron);
        const glm::dvec3 eye(0.0625, kGround + kEye, 0.5);
        const sim::SpellGesture fall = GestureOf(sim::GestureKind::Fall, {ThermalYard::Q({3.0, kGround + 0.5, -3.0}), ThermalYard::Q({3.0, kGround, -3.0})});
        ASSERT_EQ(y->Cast(sim::kMeltdownBeam, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0)).refusal, sim::SpellRefusal::None);
        ASSERT_EQ(y->Cast(sim::kPitchfall, 2, 1, eye, eye + glm::dvec3(1.0, 0.0, -1.0), fall).refusal, sim::SpellRefusal::None);
        ASSERT_EQ(y->Cast(sim::kFirestormSweep, 3, 1, eye, glm::dvec3(-3.0, kGround, 2.0)).refusal, sim::SpellRefusal::None);
        y->RunTo(y->simulation.Tick() + 120);
    }
    EXPECT_EQ(a.Hash(), b.Hash());
}

// Heat under a floe bridge releases what it carried (magic.md "Expiry and structure"): a knit block held only by a floe
// slab on a pool; Firestorm Sweep over the slab heats the floe back into water (a collider into a non-collider: a site
// check), and the block, now held by nothing, is released.
TEST(Magic, HeatUnderAFloeBridgeReleasesWhatItCarried) {
    ThermalYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& ids = yard.Ids();
    yard.World().FillBox({8, G - 4, -24}, {56, G, 24}, ids.water);          // a pool, its surface the top row of the ground
    yard.World().FillBox({16, G - 1, -8}, {32, G, 8}, ids.floe);           // the floe slab
    yard.World().FillBox({32, G - 1, -4}, {40, G + 8, 4}, ids.knitIron);   // the block beside it, standing in the water
    const glm::dvec3 eye(0.0625, kGround + kEye, 0.0625);
    const sim::CastSpellResult r = yard.Cast(sim::kFirestormSweep, 1, 1, eye, glm::dvec3(3.0, kGround, 0.0625));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_GT(r.stats.heated, 100u) << "the slab under the sweep";
    uint32_t released = 0;
    for (const sim::ScheduledResult& s : yard.RunTo(yard.simulation.Tick() + r.flightTicks + 60))
        for (const sim::BlastResult& g : s.groups)
            for (const sim::AppliedOp& a : g.ops) released += a.op.kind == sim::WorldOpKind::Detach ? 1u : 0u;
    EXPECT_EQ(yard.Count({16, G - 1, -8}, {32, G, 8}, ids.floe), 0u) << "melted back into water";
    EXPECT_GE(released, 1u) << "the block it held is released";
}
