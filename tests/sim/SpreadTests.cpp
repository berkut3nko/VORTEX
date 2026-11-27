// Fire offline (docs/design/spread.md stage S1): the driver's logical steps and credit, the step's independence of chunk
// order and threads, fire across chunk faces, the Kindle caps, stage changes that re-cook nothing, a burnout that
// releases what it held, a burning piece that lights the floor where it lands, and pitch (magic.md's Thermal) that
// burns out by the rule and lights timber offline while a session keeps its expiry.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
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
    constexpr float kVs = 0.125f;
    constexpr int G = 96;                         // flat stone ground: voxels y < 96, its top at 12 m
    constexpr double kGround = G * 0.125;
    constexpr double kEye = 1.62;

    sim::SimulationConfig SpreadConfig(unsigned threads = 2, bool spread = true) {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.maxParticles = 0;
        config.jobThreads = threads;
        config.world.voxelSize = kVs;
        config.world.windowSize = {8, 6, 8};
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        config.spread = spread;
        return config;
    }

    /// Flat stone ground, the window loaded round the origin; the fire driven tick by tick as the engine does.
    struct SpreadYard {
        explicit SpreadYard(unsigned threads = 2, bool spread = true) : simulation(SpreadConfig(threads, spread)) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(G, ids.stone, ids.stone));
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
        }
        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }

        /// One tick: the driver, then physics (or a whole Step with `focus`, so terrain colliders follow).
        std::vector<sim::BlastResult> Tick(const glm::dvec3* focus = nullptr) {
            std::vector<sim::BlastResult> out = driver.Tick(simulation, &edits);
            if (focus != nullptr) {
                const glm::dvec3 points[] = {*focus};
                simulation.Step(points);
            } else {
                simulation.StepPhysics();
            }
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
            return out;
        }
        /// Ticks until the driver has run `steps` more logical steps.
        void Steps(uint32_t steps) {
            const uint32_t target = driver.StepIndex() + steps;
            while (driver.StepIndex() < target) Tick();
        }
        uint32_t Count(const glm::ivec3& min, const glm::ivec3& max, const std::function<bool(uint8_t)>& keep) {
            uint32_t n = 0;
            for (int z = min.z; z < max.z; ++z)
                for (int y = min.y; y < max.y; ++y)
                    for (int x = min.x; x < max.x; ++x) n += keep(World().GetVoxel({x, y, z})) ? 1u : 0u;
            return n;
        }
        bool Burning(uint8_t id) const { return voxel::WorldSpreadTable().kind[id] == voxel::SpreadKind::Burning; }
        uint64_t Hash() const {
            const voxel::World& world = simulation.World();
            return voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize()).hash;
        }
        static glm::dvec3 M(const glm::ivec3& v) { return (glm::dvec3(v) + glm::dvec3(0.5)) * static_cast<double>(kVs); }

        sim::Simulation simulation;
        sim::SpreadDriver driver;
        std::vector<voxel::VoxelEdit> edits;
        bool initialized = false;
    };

    sim::SpellGesture GestureOf(sim::GestureKind kind, std::vector<glm::ivec3> points) {
        sim::SpellGesture g;
        g.kind = kind;
        g.pointsQ = std::move(points);
        return g;
    }
    glm::ivec3 Q(const glm::dvec3& metres) {
        return {static_cast<int32_t>(std::llround(metres.x * 128.0)), static_cast<int32_t>(std::llround(metres.y * 128.0)),
                static_cast<int32_t>(std::llround(metres.z * 128.0))};
    }
}

// A stage change keeps every collider: the terrain re-cooks nothing; a burnout re-cooks the chunk.
TEST(Spread, ShapeUnchangedSkipsRecook) {
    SpreadYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    yard.World().FillBox({8, G, 8}, {16, G + 8, 16}, m.planks);
    const glm::dvec3 focus = SpreadYard::M({12, G + 4, 12});
    // The colliders round the block are built and installed: until no build happens for 12 ticks.
    uint32_t quiet = 0, built = yard.simulation.Terrain().Stats().built;
    for (int i = 0; i < 600 && quiet < 12; ++i) {
        yard.Tick(&focus);
        const uint32_t now = yard.simulation.Terrain().Stats().built;
        quiet = now == built ? quiet + 1 : 0;
        built = now;
    }
    const physics::TerrainColliderStats before = yard.simulation.Terrain().Stats();
    ASSERT_EQ(yard.driver.Kindle(yard.simulation, SpreadYard::M({8, G, 8}), 0.3), sim::KindleResult::Done);
    while (yard.driver.Stats().burnedOut == 0 && yard.driver.StepIndex() < 30) {
        yard.Tick(&focus);
        if (yard.driver.Stats().burnedOut == 0) {
            EXPECT_EQ(yard.simulation.Terrain().Stats().built, before.built) << "no re-cook while only stages change";
        }
    }
    const physics::TerrainColliderStats staged = yard.simulation.Terrain().Stats();
    EXPECT_GT(staged.recooksSkipped, before.recooksSkipped);
    EXPECT_GT(yard.driver.Stats().recooksSkipped, 0u);
    // Burn on until something burnt out: then its chunk is re-cooked.
    while (yard.driver.Stats().burnedOut == 0) yard.Tick(&focus);
    for (int i = 0; i < 3; ++i) yard.Tick(&focus);
    EXPECT_GT(yard.simulation.Terrain().Stats().built, staged.built);
    EXPECT_GT(yard.driver.Stats().recooks, 0u);
}

// The step index counts logical steps only: a spent credit holds slots and the fire with them; a wood voxel still
// burns out within K + B x pace = 88 logical steps.
TEST(Spread, StepClockCountsLogicalSteps) {
    SpreadYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    yard.World().FillBox({8, G, 8}, {20, G + 4, 20}, m.wood);
    const glm::ivec3 lit(12, G, 12);
    ASSERT_EQ(yard.driver.Kindle(yard.simulation, SpreadYard::M(lit), 0.1), sim::KindleResult::Done);
    ASSERT_EQ(yard.World().GetVoxel(lit), m.woodK1);
    yard.driver.SetCredit(-4 * sim::kSpreadWorkPerStep);   // refills of one step each: three slots wait
    const uint32_t s0 = yard.driver.StepIndex();
    for (uint32_t i = 0; i < 3 * sim::kSpreadSlotTicks; ++i) yard.Tick();
    EXPECT_EQ(yard.driver.StepIndex(), s0) << "three slots waited";
    EXPECT_EQ(yard.driver.Stats().slotsWaited, 3u);
    EXPECT_EQ(yard.World().GetVoxel(lit), m.woodK1) << "nothing moved while the credit was spent";
    uint32_t steps = 0;
    while (yard.World().GetVoxel(lit) != m.charredWood && steps < 200) {
        yard.Steps(1);
        ++steps;
    }
    EXPECT_LE(steps, 88u);
    std::printf("wood voxel: charred after %u logical steps\n", steps);
}

// Nothing burning: every slot is still a logical step (the index advances alone), so a fire lit later meets the same
// phase as on any other machine.
TEST(Spread, PhaseGatedContentAdvancesWithNothingElseAwake) {
    SpreadYard yard;
    ASSERT_TRUE(yard.initialized);
    for (uint32_t i = 0; i < 10 * sim::kSpreadSlotTicks; ++i) yard.Tick();
    EXPECT_EQ(yard.driver.StepIndex(), 10u);
    EXPECT_EQ(yard.driver.Stats().silentSteps, 10u);
    EXPECT_EQ(yard.driver.Stats().steps, 0u);
}

// A timber yard over 3 x 2 x 3 chunks: one worker, four workers, four workers in reverse chunk order: equal worlds.
TEST(Spread, ResultIndependentOfChunkOrderAndThreads) {
    std::vector<uint64_t> hashes;
    std::vector<uint64_t> burnt;
    for (const auto& [threads, reverse] : {std::pair{1u, false}, std::pair{4u, false}, std::pair{4u, true}}) {
        SpreadYard yard(threads);
        ASSERT_TRUE(yard.initialized);
        yard.driver.reverseOrder = reverse;
        const voxel::WorldMaterials& m = yard.Ids();
        // Planks floors and walls, wood posts, a leaf layer: timber across chunk faces, edges and corners.
        yard.World().FillBox({8, G, 8}, {88, G + 1, 88}, m.planks);
        for (int x = 8; x < 88; x += 12) yard.World().FillBox({x, G + 1, 8}, {x + 2, G + 40, 88}, m.planks);
        for (int z = 10; z < 88; z += 16) yard.World().FillBox({8, G + 1, z}, {88, G + 30, z + 2}, m.wood);
        yard.World().FillBox({8, G + 40, 8}, {88, G + 41, 88}, m.leaves);
        ASSERT_EQ(yard.driver.Kindle(yard.simulation, SpreadYard::M({8, G, 8}), 0.5), sim::KindleResult::Done);
        yard.Steps(100);
        hashes.push_back(yard.Hash());
        burnt.push_back(yard.driver.Stats().burnedOut);
        EXPECT_GT(yard.driver.Stats().awakeMax, 1u);
    }
    EXPECT_GT(burnt[0], 0u);
    EXPECT_EQ(hashes[0], hashes[1]) << "threads";
    EXPECT_EQ(hashes[0], hashes[2]) << "chunk order";
    EXPECT_EQ(burnt[0], burnt[2]);
}

// A planks slab across a chunk face, lit in the first chunk: the second catches.
TEST(Spread, FireCrossesChunkFaces) {
    SpreadYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    yard.World().FillBox({20, G, 4}, {44, G + 1, 12}, m.planks);
    ASSERT_EQ(yard.driver.Kindle(yard.simulation, SpreadYard::M({20, G, 8}), 0.2), sim::KindleResult::Done);
    yard.Steps(60);
    const uint32_t beyond = yard.Count({32, G, 4}, {44, G + 1, 12}, [&m](uint8_t id) { return id != m.planks; });
    EXPECT_GT(beyond, 0u) << "the fire crossed x = 32";
}

// Sixteen burning chunks within 32 m of a hit: a Kindle there is refused, and lights nothing.
TEST(Spread, KindleCapsRefuse) {
    SpreadYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    for (int cx = 0; cx < 4; ++cx)
        for (int cz = 0; cz < 4; ++cz) yard.World().FillBox({cx * 32 + 4, G, cz * 32 + 4}, {cx * 32 + 6, G + 2, cz * 32 + 6}, m.woodB1);
    yard.World().FillBox({60, G, 60}, {64, G + 4, 64}, m.planks);
    for (uint32_t i = 0; i <= sim::kSpreadSlotTicks; ++i) yard.Tick();   // a slot indexes what the edits lit
    EXPECT_EQ(yard.driver.BurningChunks(), 16u);
    EXPECT_EQ(yard.driver.Kindle(yard.simulation, SpreadYard::M({62, G, 62}), 0.5), sim::KindleResult::TooMuchFire);
    EXPECT_EQ(yard.World().GetVoxel({62, G, 62}), m.planks);
    EXPECT_EQ(yard.driver.Stats().kindlesRefused, 1u);
}

// Four planks posts under a heavy stone slab, one post kindled at its foot: before that post is gone, the load check
// finds the other three cannot carry the slab and releases it.
TEST(Spread, BurnoutReleasesAStructure) {
    SpreadYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    const int x0 = 16, z0 = 16, side = 24, post = 2, high = 24;
    for (const int x : {x0, x0 + side - post})
        for (const int z : {z0, z0 + side - post}) yard.World().FillBox({x, G, z}, {x + post, G + high, z + post}, m.planks);
    yard.World().FillBox({x0, G + high, z0}, {x0 + side, G + high + 4, z0 + side}, m.stone);
    ASSERT_EQ(yard.driver.Kindle(yard.simulation, SpreadYard::M({x0, G, z0}), 0.3), sim::KindleResult::Done);
    bool released = false;
    for (uint32_t step = 0; step < 300 && !released; ++step) {
        yard.Steps(1);
        released = yard.driver.Stats().releases > 0;
    }
    EXPECT_TRUE(released);
    EXPECT_GT(yard.simulation.IslandSearchCheck().neckReleases, 0u) << "the load check released it";
    EXPECT_EQ(yard.World().GetVoxel({x0 + side / 2, G + high + 1, z0 + side / 2}), 0) << "the slab fell";
}

// A burning wood beam on a support that burns out: the site check releases it, and when it would have fallen to the
// planks floor below, the floor under its lowest burning voxel catches.
TEST(Spread, FallingBurningPieceLightsFloor) {
    SpreadYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    yard.World().FillBox({4, G, 4}, {60, G + 1, 40}, m.planks);                       // the floor
    yard.World().FillBox({8, G + 1, 20}, {12, G + 40, 24}, m.stone);                  // a stone pillar
    const uint8_t lastPlanks = static_cast<uint8_t>(m.planksB1 + 5);
    yard.World().FillBox({10, G + 40, 20}, {12, G + 43, 22}, lastPlanks);             // a support about to burn out
    yard.World().FillBox({10, G + 43, 20}, {40, G + 45, 22}, m.woodB1);               // the beam on it, past the pillar
    yard.World().FillBox({30, G + 38, 20}, {40, G + 43, 22}, m.woodB1);               // its far end hangs lower
    uint32_t releaseTick = 0;
    for (uint32_t step = 0; step < 40 && releaseTick == 0; ++step) {
        yard.Steps(1);
        if (yard.driver.Stats().releases > 0) releaseTick = yard.simulation.Tick();
    }
    ASSERT_NE(releaseTick, 0u) << "the beam was released";
    EXPECT_EQ(yard.World().GetVoxel({20, G + 43, 20}), 0) << "the beam left the terrain";
    // Its lowest burning voxel is (30, G + 38, 20) (lowest y, then z, then x); the floor under it at y = G.
    const int64_t drop = 38;
    const int64_t fallSq1024 = std::llround(2.0 * kVs / 9.81 / (1.0 / 3600.0) * 1024.0);
    const uint32_t fallTicks = static_cast<uint32_t>(sim::ISqrt64(drop * fallSq1024 / 1024)) + 1u;
    bool caught = false;
    while (yard.simulation.Tick() <= releaseTick + fallTicks + 12 && !caught) {
        yard.Tick();
        caught = yard.Burning(yard.World().GetVoxel({30, G, 20}));
    }
    EXPECT_TRUE(caught) << "the floor under the beam caught within " << fallTicks + 12 << " ticks of the release";
    EXPECT_EQ(yard.driver.Stats().landingsEmitted, 1u);
    std::printf("landing: released at tick %u, fall %u ticks\n", releaseTick, fallTicks);
}

// Pitch offline: laid at its first burn stage, it burns out by the rule within 4 x 10 steps; as a source it lights
// the planks wall beside it, and the fire climbs the wall to its top; a wood post on its other side never catches
// (pitch's last stage is under wood's side delay), and nothing under it burns.
TEST(Spread, PitchClimbsAndLightsTimber) {
    SpreadYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    yard.World().FillBox({40, G, 0}, {41, G + 24, 16}, m.planks);       // a planks wall, 3 m high
    // Elsewhere: pitch on a planks block, wood beside the pitch.
    yard.World().FillBox({20, G, 20}, {24, G + 2, 24}, m.planks);
    yard.World().FillBox({21, G + 2, 21}, {23, G + 3, 23}, m.pitch);
    yard.World().FillBox({23, G + 2, 21}, {24, G + 3, 23}, m.wood);
    voxel::VoxelMask mask(glm::ivec3(1, 1, 16));
    for (int z = 0; z < 16; ++z) mask.Set(glm::ivec3(0, 0, z), true);
    const sim::WorldOp pitch = sim::WorldOp::MapMasked(voxel::Map_Pitch, glm::ivec3(39, G, 0), mask);
    ASSERT_EQ(sim::ApplyOp(yard.World(), pitch).changed, 16u);
    EXPECT_EQ(yard.World().GetVoxel({39, G, 8}), m.pitch);
    yard.Steps(45);
    EXPECT_EQ(yard.Count({39, G, 0}, {40, G + 1, 16}, [&m](uint8_t id) { return id >= m.pitch && id <= m.pitchLast; }), 0u)
        << "the pitch burnt out";
    EXPECT_GT(yard.Count({40, G, 0}, {41, G + 1, 16}, [&m](uint8_t id) { return id != m.planks; }), 0u) << "the wall's foot caught";
    yard.Steps(80);
    EXPECT_GT(yard.Count({40, G + 20, 0}, {41, G + 24, 16}, [&m](uint8_t id) { return id != m.planks; }), 0u) << "the fire climbed the wall";
    EXPECT_EQ(yard.Count({23, G + 2, 21}, {24, G + 3, 23}, [&m](uint8_t id) { return id == m.wood; }), 2u) << "pitch never lights wood beside it";
    EXPECT_EQ(yard.Count({20, G, 20}, {24, G + 2, 24}, [&m](uint8_t id) { return id == m.planks; }), 32u) << "nor the planks under it";
    EXPECT_GE(yard.driver.Stats().lowestStepIgnition, G);
}

// A session world (sim::ServerConfig: no fire) keeps pitch's expiry: two servers fed the same Pitchfall have equal
// worlds, the pitch goes with Unknit at its lifetime and no edit list is kept for a fire driver. Offline the same cast
// leaves the pitch to the fire rule: no expiry op takes it.
TEST(Spread, SessionsKeepPitchExpiryAndNeverStep) {
    sim::SimulationConfig asked = SpreadConfig(2, true);
    EXPECT_FALSE(sim::ServerConfig(asked).spread);
    const auto castPitchfall = [](sim::Simulation& simulation, sim::CastContext& ctx, const sim::SpellTable& table) {
        sim::SpellCast c;
        c.player = 1;
        c.team = 1;
        c.spell = sim::kPitchfall;
        c.eye = glm::dvec3(0.0625, kGround + kEye, 0.0625);
        c.dir = glm::vec3(1.0f, 0.0f, 0.0f);
        c.gesture = GestureOf(sim::GestureKind::Fall, {Q({4.0, kGround + 0.5, 0.0625}), Q({4.0, kGround, 0.0625})});
        return sim::CastSpell(simulation, table, c, ctx, {});
    };
    const sim::SpellTable table = sim::BuildSpellTable(kVs);
    const auto pitchIn = [](sim::Simulation& simulation) {
        const voxel::WorldMaterials& m = simulation.WorldMaterialIds();
        uint32_t n = 0;
        for (int z = -48; z < 48; ++z)
            for (int y = G - 2; y < G + 8; ++y)
                for (int x = -16; x < 80; ++x) {
                    const uint8_t id = simulation.World().GetVoxel({x, y, z});
                    n += id >= m.pitch && id <= m.pitchLast ? 1u : 0u;
                }
        return n;
    };
    // Two session servers.
    std::vector<uint64_t> hashes;
    for (int i = 0; i < 2; ++i) {
        sim::Simulation server(sim::ServerConfig(SpreadConfig(2, true)));
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
        ASSERT_TRUE(server.Initialize(voxel::CreateFlatGenerator(G, ids.grass, ids.stone)));
        server.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
        sim::CastContext ctx;
        ctx.walls = sim::WindowWalls(server.World());
        const uint32_t t0 = server.Tick();
        const sim::CastSpellResult r = castPitchfall(server, ctx, table);
        ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
        uint32_t burning = 0;
        while (server.Tick() <= t0 + table.spells[sim::kPitchfall].lifetimeTicks) {
            sim::RunScheduled(server, table, ctx, server.Tick(), {});
            if (server.Tick() == t0 + r.flightTicks + 1) burning = pitchIn(server);
            server.StepPhysics();
        }
        EXPECT_GT(burning, 100u);
        EXPECT_EQ(pitchIn(server), 0u) << "Unknit took it at its lifetime";
        EXPECT_TRUE(server.World().TakeEditedChunks().empty()) << "no edit list without fire";
        hashes.push_back(voxel::WorldHash(server.World(), server.World().WindowMinChunk(),
                                          server.World().WindowMinChunk() + server.World().WindowSize()).hash);
    }
    EXPECT_EQ(hashes[0], hashes[1]);
    // Offline with fire: no Unknit at the lifetime; the driver burns it out.
    sim::Simulation offline(SpreadConfig(2, true));
    voxel::MaterialLibrary scratch;
    const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
    ASSERT_TRUE(offline.Initialize(voxel::CreateFlatGenerator(G, ids.grass, ids.stone)));
    offline.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
    sim::CastContext ctx;
    ctx.walls = sim::WindowWalls(offline.World());
    sim::SpreadDriver driver;
    const uint32_t t0 = offline.Tick();
    const sim::CastSpellResult r = castPitchfall(offline, ctx, table);
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None);
    const uint32_t landed = t0 + r.flightTicks;
    uint32_t atLifetime = 0;
    while (offline.Tick() <= landed + sim::kPitchBurnoutTicks + sim::kSpreadSlotTicks) {
        sim::RunScheduled(offline, table, ctx, offline.Tick(), {});
        driver.Tick(offline);
        if (offline.Tick() == t0 + table.spells[sim::kPitchfall].lifetimeTicks + 1) atLifetime = pitchIn(offline);
        offline.StepPhysics();
    }
    EXPECT_EQ(pitchIn(offline), 0u) << "the fire rule burnt it out";
    EXPECT_GT(driver.Stats().steps, 0u);
    std::printf("offline pitch: %u voxels still burning at the session lifetime\n", atLifetime);
}
