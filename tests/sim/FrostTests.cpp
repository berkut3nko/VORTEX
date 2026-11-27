// Frost, the bore and the Shatter combo (docs/design/magic.md "Pierce (Solid): a bore that pays for every voxel it
// adds", "The Turning table", "The element verbs", "Players: zones, statuses and the movement rule", spells 14-16,
// "Combos and counters"): the Pierce walk and its CarveCapsule, the Shatter flood, the full Plume, the Frost spells, the combos, and the player's statuses
// (Chill, Root, Release, the control guard, Fresh) and slick ground.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <set>
#include <span>
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
    constexpr double kEye = 1.62;
    using VoxelKey = std::tuple<int, int, int>;

    sim::SimulationConfig FrostConfig() {
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
        return voxel::CreateFlatGenerator(kG, ids.grass, ids.stone);
    }

    struct Fighter {
        uint8_t player = 0;
        sim::TeamId team = sim::kNoTeam;
        sim::PlayerState state;
        physics::CharacterId character;
        sim::PlayerInput input;
    };

    /// Flat ground at 0.125 m: players stepped by the movement rule, spells cast and followed up as a server does.
    struct FrostYard {
        FrostYard() : simulation(FrostConfig()) {
            initialized = simulation.Initialize(FlatGround());
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
            table = sim::BuildSpellTable(kVoxel);
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

        sim::ScheduledResult Tick() {
            std::vector<glm::dvec3> focus{glm::dvec3(0.0, kGround, 0.0)};
            for (const Fighter& f : fighters) focus.push_back(glm::dvec3(f.state.position));
            simulation.UpdateStreaming(focus);
            for (Fighter& f : fighters) {
                simulation.Terrain().FlushBox(sim::PlayerReach(f.state.position, ctx.tuning));
                const sim::PlayerZones zones{&simulation.Zones(), &simulation.World(), simulation.Tick(), static_cast<uint8_t>(1u << f.team)};
                sim::StepPlayer(simulation.Physics(), simulation.Terrain(), f.character, f.state, f.input, ctx.tuning, sim::PlayArea{}, zones,
                                simulation.FixedTimeStep());
            }
            sim::ScheduledResult r = sim::RunScheduled(simulation, table, ctx, simulation.Tick(), Targets());
            simulation.StepPhysics();
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
            return r;
        }

        void Settle() {
            std::vector<glm::dvec3> focus{glm::dvec3(0.0, kGround, 0.0)};
            for (const Fighter& f : fighters) focus.push_back(glm::dvec3(f.state.position));
            simulation.Terrain().Update(focus, simulation.Config().colliderRadius);
            simulation.Terrain().Flush();
            for (int i = 0; i < 10; ++i) Tick();
        }

        sim::CastSpellResult Cast(sim::SpellId spell, uint8_t player, sim::TeamId team, const glm::dvec3& eye, const glm::dvec3& target) {
            sim::SpellCast c;
            c.player = player;
            c.team = team;
            c.spell = spell;
            c.eye = eye;
            c.dir = glm::vec3(glm::normalize(target - eye));
            const std::vector<sim::SpellTarget> targets = Targets();
            return sim::CastSpell(simulation, table, c, ctx, targets);
        }

        /// Ticks until the cast's projectile has landed (its effects run at T_run + flightTicks); the groups that ran.
        std::vector<sim::BlastResult> Land(const sim::CastSpellResult& r) {
            std::vector<sim::BlastResult> groups;
            while (simulation.Tick() <= r.runTick + r.flightTicks)
                for (sim::BlastResult& g : Tick().groups) groups.push_back(std::move(g));
            return groups;
        }

        /// The game's left-click blast (power 6, r 1.2 m) where the ray from `eye` toward `target` meets the terrain.
        sim::BlastResult RayBlast(const glm::dvec3& eye, const glm::dvec3& target) {
            const voxel::WorldRayHit hit = World().Raycast(eye, glm::vec3(glm::normalize(target - eye)), 60.0f);
            EXPECT_TRUE(hit.hit);
            const glm::dvec3 centre = hit.position / static_cast<double>(kVoxel);
            const voxel::SphereQ sphere = voxel::QuantizeSphere(centre, 1.2 / static_cast<double>(kVoxel));
            return sim::BlastAt(simulation, sphere, voxel::QuantizePower(6.0f), World().GetVoxel(hit.voxel));
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

        uint64_t Hash() const {
            const voxel::World& world = simulation.World();
            return voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize()).hash;
        }

        sim::Simulation simulation;
        sim::SpellTable table;
        sim::CastContext ctx;
        std::deque<Fighter> fighters;
        bool initialized = false;
    };

    glm::ivec3 QOf(const glm::dvec3& metres) {
        const double q = 16.0 / static_cast<double>(kVoxel);
        return {static_cast<int32_t>(std::llround(metres.x * q)), static_cast<int32_t>(std::llround(metres.y * q)),
                static_cast<int32_t>(std::llround(metres.z * q))};
    }

    /// Shatter Spike's Pierce parameters from the table.
    sim::PierceParams SpikeParams(FrostYard& yard, const glm::ivec3& hitQ, const glm::ivec3& aim) {
        const sim::SpellDef& s = yard.table.spells[sim::kShatterSpike];
        sim::PierceParams p;
        p.hitQ = hitQ;
        p.aim = aim;
        p.radiusQ = s.pierceRadiusQ;
        p.power = s.piercePower;
        p.budget = s.pierceBudget;
        p.maxSlices = s.pierceMaxSlices;
        p.boreCost = &yard.table.boreCost;
        p.materials = &yard.simulation.Materials();
        return p;
    }

    /// D = round(direction * 16384), as the cast takes its aim.
    glm::ivec3 AimOf(const glm::vec3& dir) {
        const glm::dvec3 d = glm::normalize(glm::dvec3(dir));
        return {static_cast<int32_t>(std::lround(d.x * 16384.0)), static_cast<int32_t>(std::lround(d.y * 16384.0)),
                static_cast<int32_t>(std::lround(d.z * 16384.0))};
    }

    /// Voxels along +x from `face` (inclusive) at row (y, z) that are air, counted until the first solid one.
    int AirRun(voxel::World& world, int face, int y, int z, int limit) {
        int n = 0;
        while (n < limit && world.GetVoxel({face + n, y, z}) == 0) ++n;
        return n;
    }

    /// One flat-ground player with zones, stepped alone (the movement rule's statuses).
    struct Field {
        Field() : simulation(FrostConfig()) {
            initialized = simulation.Initialize(FlatGround());
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
            state.position = {0.3f, kGround, 0.3f};
            character = simulation.Physics().CreateCharacter(sim::PlayerCharacterDesc(state.position, tuning));
            const std::array<glm::dvec3, 1> focus = {glm::dvec3(state.position)};
            simulation.Terrain().Update(focus, simulation.Config().colliderRadius);
            simulation.Terrain().Flush();
        }

        sim::PlayerStepResult Tick(const sim::PlayerInput& input = {}) {
            const std::array<glm::dvec3, 1> focus = {glm::dvec3(state.position)};
            simulation.UpdateStreaming(focus);
            simulation.Terrain().FlushBox(sim::PlayerReach(state.position, tuning));
            const sim::PlayerZones zones{&simulation.Zones(), &simulation.World(), simulation.Tick(), 1u};
            const sim::PlayerStepResult r =
                sim::StepPlayer(simulation.Physics(), simulation.Terrain(), character, state, input, tuning, sim::PlayArea{}, zones,
                                simulation.FixedTimeStep());
            simulation.StepPhysics();
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
            return r;
        }

        /// A zone of `kind` round the player's feet (a y-cylinder of 2 m), starting at `start`.
        sim::Zone ZoneHere(uint16_t id, sim::ZoneKind kind, uint32_t start, uint16_t duration, int16_t s0 = 0, int16_t s1 = 0) const {
            sim::Zone z;
            z.zoneId = id;
            z.kind = kind;
            z.teamMask = 0xFF;
            z.shape.kind = voxel::kShapeCylinder;
            z.shape.a = QOf(glm::dvec3(state.position));
            z.shape.radiusQ = 256;
            z.shape.halfHeightQ = 256;
            z.startTick = start;
            z.duration = duration;
            z.strength[0] = s0;
            z.strength[1] = s1;
            return z;
        }

        sim::Simulation simulation;
        sim::PlayerTuning tuning;
        sim::PlayerState state;
        physics::CharacterId character;
        bool initialized = false;
    };

    sim::PlayerInput WalkX() {
        sim::PlayerInput in;
        in.moveX = 127;   // yaw 0 looks along -z: strafing right is +x
        return in;
    }

    float SpeedXZ(const sim::PlayerState& s) { return std::sqrt(s.velocity.x * s.velocity.x + s.velocity.z * s.velocity.z); }
}

// --- the bore -----------------------------------------------------------------------------------------------------

TEST(Materials, BoreCostMatchesHardness) {
    voxel::MaterialLibrary library;
    const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(library);
    const std::array<uint16_t, 256> cost = sim::BoreCostTable();
    for (uint32_t id = 1; id < 256; ++id) {
        if (library.Name(voxel::kWorldPalette, static_cast<uint8_t>(id)).empty()) {
            EXPECT_EQ(cost[id], 0u) << id;
            continue;
        }
        const voxel::Material& m = library.Get(voxel::kWorldPalette, static_cast<uint8_t>(id));
        if ((m.flags & voxel::MaterialFlag_Liquid) != 0) EXPECT_EQ(cost[id], 0u) << id;
        else if ((m.flags & voxel::MaterialFlag_Indestructible) != 0) EXPECT_EQ(cost[id], sim::kBoreStop) << id;
        else EXPECT_EQ(cost[id], voxel::QuantizePower(m.hardness)) << library.Name(voxel::kWorldPalette, static_cast<uint8_t>(id));
    }
    EXPECT_EQ(cost[ids.stone], 48u);
    EXPECT_EQ(cost[ids.stoneRimed], 24u);
    EXPECT_EQ(cost[ids.floe], sim::kBoreStop);
    EXPECT_EQ(cost[0], 0u);
}

TEST(Pierce, WalkAndCapsuleAgree) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    // A wall 3 m thick: brick, stone, planks, with one iron row.
    yard.World().FillBox({16, kG, -48}, {20, kG + 40, 48}, m.brick);
    yard.World().FillBox({20, kG, -48}, {32, kG + 40, 48}, m.stone);
    yard.World().FillBox({32, kG, -48}, {40, kG + 40, 48}, m.planks);
    yard.World().FillBox({22, kG + 20, -48}, {24, kG + 21, 48}, m.iron);
    const std::array<glm::dvec3, 8> dirs = {glm::dvec3(1, 0, 0),      glm::dvec3(1, 0.3, 0.2),  glm::dvec3(1, -0.2, -0.4), glm::dvec3(1, 0.5, 0),
                                            glm::dvec3(1, 0, 0.7),    glm::dvec3(1, -0.1, 0.1), glm::dvec3(1, 0.05, -0.6), glm::dvec3(1, 0.2, 0.4)};
    for (size_t i = 0; i < dirs.size(); ++i) {
        const glm::dvec3 eye(0.0, kGround + 2.5, -4.0 + static_cast<double>(i));
        const glm::vec3 dir(glm::normalize(dirs[i]));
        const voxel::WorldRayHit hit = yard.World().Raycast(eye, dir, 60.0f);
        ASSERT_TRUE(hit.hit) << i;
        sim::PierceParams p = SpikeParams(yard, QOf(hit.position), AimOf(dir));
        p.budget = 100000;
        p.maxSlices = 64;
        std::vector<glm::ivec3> paid;
        p.paidOut = &paid;
        const sim::PierceResult bore = sim::Pierce(yard.World(), sim::WindowWalls(yard.World()), p);
        ASSERT_TRUE(bore.bored) << i;
        EXPECT_EQ(bore.paidVoxels, paid.size());
        std::vector<voxel::VoxelEdit> removed;
        const sim::OpResult r = sim::ApplyOp(yard.World(), sim::WorldOp::Bore(bore.capsule, p.power), &removed);
        EXPECT_TRUE(r.Complete());
        std::set<VoxelKey> walked, carved;
        for (const glm::ivec3& v : paid) walked.emplace(v.x, v.y, v.z);
        for (const voxel::VoxelEdit& e : removed) carved.emplace(e.voxel.x, e.voxel.y, e.voxel.z);
        EXPECT_EQ(walked, carved) << "aim " << i << ": " << paid.size() << " paid, " << removed.size() << " carved";
        EXPECT_GT(carved.size(), 0u) << i;
    }
}

TEST(Propagate, PierceStopsAtBedrockAndWhenTheBudgetIsSpent) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    // Four blocks 6 m thick, side by side in z: stone, rimed stone, iron, bedrock.
    const std::array<uint8_t, 4> ids = {m.stone, m.stoneRimed, m.iron, m.bedrock};
    std::array<int, 4> depth{};
    for (int i = 0; i < 4; ++i) {
        const int z0 = -48 + 24 * i;
        yard.World().FillBox({16, kG, z0}, {64, kG + 24, z0 + 16}, ids[i]);
        const glm::dvec3 eye(0.0, kGround + 1.5, (z0 + 8) * kVoxel);
        const sim::CastSpellResult r = yard.Cast(sim::kShatterSpike, 0, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
        ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << i;
        yard.Land(r);
        depth[i] = AirRun(yard.World(), 16, kG + 12, z0 + 8, 60);
    }
    std::printf("Pierce: Shatter Spike bores stone %d, rimed stone %d, iron %d, bedrock %d voxels\n", depth[0], depth[1], depth[2], depth[3]);
    EXPECT_GE(depth[0], 6);
    EXPECT_LE(depth[0], 16) << "1-2 m of stone";
    EXPECT_GE(depth[1], depth[0] * 3 / 2) << "rimed stone costs half";
    EXPECT_LE(depth[1], 24) << "at most 3 m";
    EXPECT_EQ(depth[2], 0) << "iron is harder than the spike";
    EXPECT_EQ(depth[3], 0) << "bedrock stops it";
    // The walk itself names the stop.
    const glm::ivec3 face = QOf(glm::dvec3(2.0, kGround + 1.5, (-48 + 72 + 8) * kVoxel));
    const sim::PierceResult atBedrock =
        sim::Pierce(yard.World(), sim::WindowWalls(yard.World()), SpikeParams(yard, face, glm::ivec3(16384, 0, 0)));
    EXPECT_EQ(atBedrock.stop, sim::PierceStop::TooHard);
    EXPECT_EQ(atBedrock.paidVoxels, 0u);
    const glm::ivec3 stoneFace = QOf(glm::dvec3(2.0, kGround + 2.5, -5.0));
    const sim::PierceResult inStone =
        sim::Pierce(yard.World(), sim::WindowWalls(yard.World()), SpikeParams(yard, stoneFace, glm::ivec3(16384, 0, 0)));
    EXPECT_EQ(inStone.stop, sim::PierceStop::Budget);
}

TEST(Pierce, BoreStaysInsideItsFloodBox) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    // A bore through air: only its length (64 voxels from the hit) stops it.
    const glm::ivec3 hitQ = QOf(glm::dvec3(-6.0, kGround + 2.0, 0.3));
    for (const glm::ivec3& aim : {glm::ivec3(16384, 0, 0), glm::ivec3(9000, 9000, 9000), glm::ivec3(-3000, 16000, 2000)}) {
        sim::PierceParams p = SpikeParams(yard, hitQ, aim);
        p.budget = 1000000;
        p.maxSlices = 500;
        const sim::PierceResult bore = sim::Pierce(yard.World(), sim::WindowWalls(yard.World()), p);
        ASSERT_TRUE(bore.bored);
        EXPECT_TRUE(bore.stop == sim::PierceStop::Length || bore.stop == sim::PierceStop::Wall) << int(bore.stop);
        const glm::ivec3 d = bore.capsule.b - hitQ;
        EXPECT_LE(int64_t{d.x} * d.x + int64_t{d.y} * d.y + int64_t{d.z} * d.z, int64_t{16 * sim::kMaxBoreVoxels} * (16 * sim::kMaxBoreVoxels));
        const sim::BlastSite site = sim::CapsuleSite(yard.World(), bore.capsule, p.power, hitQ);
        EXPECT_TRUE(glm::all(glm::greaterThanEqual(site.islandMin, site.floodMin)));
        EXPECT_TRUE(glm::all(glm::lessThanEqual(site.islandMax, site.floodMax)));
        // Structure past the end is seen for at least kBoreProofMargin voxels inside the flood box.
        const glm::ivec3 end = bore.capsule.b >> 4;
        EXPECT_TRUE(glm::all(glm::greaterThanEqual(end - glm::ivec3(sim::kBoreProofMargin), site.floodMin)));
        EXPECT_TRUE(glm::all(glm::lessThanEqual(end + glm::ivec3(sim::kBoreProofMargin), site.floodMax)));
    }
}

TEST(Propagate, PierceWorkIsEqualColdAndWarm) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    yard.World().FillBox({16, kG, -16}, {40, kG + 24, 16}, yard.Ids().stone);
    const glm::ivec3 hitQ = QOf(glm::dvec3(2.0, kGround + 1.5, 0.3));
    const sim::PierceParams p = SpikeParams(yard, hitQ, glm::ivec3(16384, 0, 0));
    const sim::PierceResult cold = sim::Pierce(yard.World(), sim::WindowWalls(yard.World()), p);
    sim::Blast(yard.simulation, voxel::SphereQ{QOf(glm::dvec3(4.0, kGround + 2.5, -1.0)), 16}, 96);   // warms the graphs
    const sim::PierceResult warm = sim::Pierce(yard.World(), sim::WindowWalls(yard.World()), p);
    EXPECT_EQ(cold.visits, warm.visits);
    EXPECT_EQ(cold.slices, warm.slices);
    EXPECT_EQ(cold.scanned, warm.scanned);
    EXPECT_EQ(cold.chunksRead, warm.chunksRead);
    EXPECT_EQ(cold.capsule, warm.capsule);
}

TEST(CarveCapsule, MatchesAChainOfSpheres) {
    FrostYard a, b;
    ASSERT_TRUE(a.initialized && b.initialized);
    for (FrostYard* yard : {&a, &b}) yard->World().FillBox({-20, kG, -20}, {20, kG + 30, 20}, yard->Ids().stone);
    voxel::ShapeQ capsule;
    capsule.kind = voxel::kShapeCapsule;
    capsule.a = {-80 + 3, (kG + 12) * 16 + 5, 7};
    capsule.b = capsule.a + glm::ivec3(160, 0, 0);
    capsule.radiusQ = 51;
    const sim::OpResult bore = sim::ApplyOp(a.World(), sim::WorldOp::Bore(capsule, 1000));
    uint32_t spheres = 0;
    for (int k = 0; k <= 160; ++k)
        spheres += sim::ApplyOp(b.World(), sim::WorldOp::Carve({capsule.a + glm::ivec3(k, 0, 0), 51}, 1000)).changed;
    EXPECT_GT(bore.changed, 0u);
    EXPECT_EQ(bore.changed, spheres);
    EXPECT_EQ(a.Hash(), b.Hash());
}

TEST(CarveCapsule, ClipEqualsWholeAndKeepsTheHardVoxels) {
    FrostYard a, b;
    ASSERT_TRUE(a.initialized && b.initialized);
    for (FrostYard* yard : {&a, &b}) {
        yard->World().FillBox({-40, kG, -40}, {40, kG + 40, 40}, yard->Ids().stone);
        yard->World().FillBox({0, kG, -40}, {2, kG + 40, 40}, yard->Ids().iron);
    }
    voxel::ShapeQ capsule;
    capsule.kind = voxel::kShapeCapsule;
    capsule.a = {-30 * 16, (kG + 4) * 16, -20 * 16};
    capsule.b = {30 * 16 + 5, (kG + 36) * 16, 20 * 16};
    capsule.radiusQ = 40;
    const sim::WorldOp op = sim::WorldOp::Bore(capsule, 64);
    const sim::OpResult whole = sim::ApplyOp(a.World(), op);
    glm::ivec3 lo, hi;
    ASSERT_TRUE(sim::OpChunkRange(op, lo, hi));
    uint32_t clipped = 0;
    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x) {
                const voxel::ChunkClip clip = voxel::ChunkClip::One({x, y, z});
                clipped += sim::ApplyOp(b.World(), op, nullptr, &clip).changed;
            }
    EXPECT_GT(whole.changed, 0u);
    EXPECT_EQ(whole.changed, clipped);
    EXPECT_EQ(a.Hash(), b.Hash());
    EXPECT_EQ(a.Count({0, kG, -40}, {2, kG + 40, 40}, a.Ids().iron), 2u * 40u * 80u) << "iron is harder than power 64";
}

TEST(CarveCapsule, FarCoordinatesStayExact) {
    voxel::ShapeQ near;
    near.kind = voxel::kShapeCapsule;
    near.a = {3, 5, 7};
    near.b = {163, 101, -40};
    near.radiusQ = 51;
    voxel::ShapeQ far = near;
    const glm::ivec3 shift(1 << 26, 0, 1 << 26);   // voxels
    far.a += shift * 16;
    far.b += shift * 16;
    glm::ivec3 lo, hi;
    voxel::ShapeVoxelBounds(near, lo, hi);
    uint32_t inside = 0;
    for (int z = lo.z; z < hi.z; ++z)
        for (int y = lo.y; y < hi.y; ++y)
            for (int x = lo.x; x < hi.x; ++x) {
                const glm::ivec3 v(x, y, z);
                const bool in = voxel::InShapeQ(near, v);
                inside += in ? 1u : 0u;
                EXPECT_EQ(voxel::InShapeQ(far, v + shift), in);
            }
    EXPECT_GT(inside, 100u);
}

// --- the Shatter flood ---------------------------------------------------------------------------------------------

TEST(Propagate, ShatterReachesOnlyExposedRime) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    // Rimed stone with only its west face open inside the sphere; a knit shell covers a second block of it.
    yard.World().FillBox({16, kG, -40}, {40, kG + 40, 40}, m.stoneRimed);
    yard.World().FillBox({15, kG, 50}, {41, kG + 41, 92}, m.knitIron);
    yard.World().FillBox({16, kG, 51}, {40, kG + 40, 91}, m.stoneRimed);
    const sim::OperatorWalls walls = sim::WindowWalls(yard.World());
    const voxel::SphereQ open{QOf(glm::dvec3(2.0, kGround + 1.0, 0.0)), static_cast<uint16_t>(16 * 16)};
    const sim::ShatterResult flood = sim::ShatterFlood(yard.World(), walls, open, 4, 4000);
    ASSERT_GT(flood.cells.size(), 0u);
    for (const glm::ivec3& c : flood.cells) {
        EXPECT_LT(c.x, 20) << "at most 4 voxels from the exposed face";
        EXPECT_TRUE(voxel::InSphereQ(open, c));
    }
    const voxel::SphereQ plated{QOf(glm::dvec3(2.0, kGround + 1.0, 8.75)), static_cast<uint16_t>(16 * 16)};
    EXPECT_TRUE(sim::ShatterFlood(yard.World(), walls, plated, 4, 4000).cells.empty()) << "a knit plate over rime protects it";
}

// --- the spells ----------------------------------------------------------------------------------------------------

TEST(Magic, ShatterSpikeBoresRimesAndThaws) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    yard.World().FillBox({16, kG, -16}, {48, kG + 24, 16}, m.stone);
    Fighter& enemy = yard.Add(1, 2, {1.5f, kGround, 0.6f});
    yard.Settle();
    const glm::dvec3 eye(0.0, kGround + kEye, 0.0625);
    const sim::CastSpellResult r = yard.Cast(sim::kShatterSpike, 0, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    EXPECT_TRUE(r.groups.empty()) << "the spike lands after its flight";
    const std::vector<sim::BlastResult> landed = yard.Land(r);
    ASSERT_GE(landed.size(), 2u);
    EXPECT_EQ(landed[0].ops[0].op.kind, sim::WorldOpKind::CarveCapsule);
    EXPECT_EQ(landed[1].ops[0].op.kind, sim::WorldOpKind::MapShape);
    EXPECT_EQ(landed[1].ops[0].op.map, voxel::Map_Rime);
    const uint32_t rimed = yard.Count({16, kG, -16}, {32, kG + 24, 16}, m.stoneRimed);
    EXPECT_GT(rimed, 100u);
    ASSERT_EQ(r.zonesStarted.size(), 1u);
    EXPECT_EQ(r.zonesStarted[0].kind, sim::ZoneKind::Chill);
    for (uint32_t t = 0; t < 40; ++t) yard.Tick();
    EXPECT_GT(enemy.state.slowLeft, 0) << "the enemy beside the hit is chilled";
    EXPECT_EQ(enemy.state.slowQ, 6);
    while (yard.simulation.Tick() <= r.runTick + yard.table.spells[sim::kShatterSpike].rimeLifetimeTicks) yard.Tick();
    EXPECT_EQ(yard.Count({16, kG, -16}, {32, kG + 24, 16}, m.stoneRimed), 0u) << "the Thaw";
    EXPECT_EQ(yard.ctx.registry.LiveVoxels(1), 0u);
}

TEST(Magic, FlashFreezeFilmsDryGroundAndRootsThenChills) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    Fighter& enemy = yard.Add(1, 2, {3.0f, kGround, 0.4f});
    yard.Settle();
    const glm::dvec3 eye(0.0, kGround + kEye, 0.0);
    const sim::CastSpellResult r = yard.Cast(sim::kFlashFreeze, 0, 1, eye, glm::dvec3(3.0, kGround, 0.0));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    yard.Land(r);
    const uint32_t ice = yard.Count({-4, kG, -28}, {52, kG + 2, 28}, yard.Ids().ice);
    EXPECT_GT(ice, 800u);
    EXPECT_EQ(r.zonesStarted.size(), 2u);
    // The enemy's own capsule keeps its cells: never entombed.
    for (int y = kG; y < kG + 15; ++y) EXPECT_EQ(yard.World().GetVoxel({24, y, 3}), 0) << y;
    while (yard.simulation.Tick() <= r.zoneTick) yard.Tick();
    EXPECT_GT(enemy.state.rootLeft, 0);
    EXPECT_GT(enemy.state.slowLeft, 0);
    const glm::vec3 rooted = enemy.state.position;
    enemy.input = WalkX();
    for (int i = 0; i < 30; ++i) yard.Tick();
    EXPECT_NEAR(enemy.state.position.x, rooted.x, 0.05f) << "rooted for 1 s";
    while (yard.simulation.Tick() <= r.runTick + yard.table.spells[sim::kFlashFreeze].lifetimeTicks) yard.Tick();
    EXPECT_EQ(yard.Count({-4, kG, -28}, {52, kG + 2, 28}, yard.Ids().ice), 0u) << "the ice is gone at 12 s";
}

TEST(Magic, PermafrostClimbIcesAWallFaceAndRimesIt) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    yard.World().FillBox({24, kG, -24}, {32, kG + 64, 24}, m.stone);
    // A murder hole 2 voxels deep, 3 m up.
    yard.World().FillBox({24, kG + 24, -1}, {26, kG + 26, 1}, 0);
    const glm::dvec3 eye(0.0, kGround + kEye, 0.0625);
    const sim::CastSpellResult r = yard.Cast(sim::kPermafrostClimb, 0, 1, eye, glm::dvec3(3.0, kGround + 0.5, 0.0625));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    yard.Land(r);
    const uint32_t ice = yard.Count({18, kG, -24}, {24, kG + 64, 24}, m.ice);
    EXPECT_GT(ice, 100u) << "a sheet of ice up the face";
    EXPECT_GT(yard.Count({24, kG, -24}, {27, kG + 64, 24}, m.stoneRimed), 100u);
    std::printf("Permafrost: ice %u, path %u, front %u\n", ice, r.stats.path, r.stats.front);
    ASSERT_EQ(r.zonesStarted.size(), 2u);
    EXPECT_EQ(r.zonesStarted[0].kind, sim::ZoneKind::Root);
    EXPECT_EQ(r.zonesStarted[1].kind, sim::ZoneKind::Chill);
    while (yard.simulation.Tick() <= r.runTick + yard.table.spells[sim::kPermafrostClimb].lifetimeTicks) yard.Tick();
    EXPECT_EQ(yard.Count({18, kG, -24}, {32, kG + 64, 24}, m.ice), 0u);
    EXPECT_EQ(yard.Count({18, kG, -24}, {32, kG + 64, 24}, m.stoneRimed), 0u);
}

TEST(Propagate, PlumeLeavesNoFrontCellHanging) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 eye(0.0, kGround + kEye, 0.0);
    const sim::CastSpellResult r = yard.Cast(sim::kPermafrostClimb, 0, 1, eye, glm::dvec3(3.0, kGround, 0.0));
    ASSERT_EQ(r.refusal, sim::SpellRefusal::None) << sim::ToString(r.refusal);
    yard.Land(r);
    // Every ice voxel joins the ground through ice: a flood from the ground's ice reaches all of them.
    const uint8_t ice = yard.Ids().ice;
    std::set<VoxelKey> all, reached;
    std::vector<glm::ivec3> todo;
    for (int z = -16; z < 16; ++z)
        for (int y = kG; y < kG + 60; ++y)
            for (int x = 8; x < 40; ++x)
                if (yard.World().GetVoxel({x, y, z}) == ice) {
                    all.emplace(x, y, z);
                    if (y == kG) {
                        reached.emplace(x, y, z);
                        todo.push_back({x, y, z});
                    }
                }
    ASSERT_GT(all.size(), 0u);
    while (!todo.empty()) {
        const glm::ivec3 c = todo.back();
        todo.pop_back();
        for (const glm::ivec3& f : {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0), glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1),
                                    glm::ivec3(0, 0, -1)}) {
            const glm::ivec3 n = c + f;
            if (all.contains({n.x, n.y, n.z}) && reached.emplace(n.x, n.y, n.z).second) todo.push_back(n);
        }
    }
    EXPECT_EQ(reached.size(), all.size()) << "no ice voxel hangs in the air";
    int top = 0;
    for (const VoxelKey& k : all) top = std::max(top, std::get<1>(k));
    EXPECT_LE(top, kG + 3) << "in the open the ice is a ring at the base";
}

// --- the combos ----------------------------------------------------------------------------------------------------

TEST(Combo, ShatterSpikeDeepensABlast) {
    FrostYard plain, spiked;
    ASSERT_TRUE(plain.initialized && spiked.initialized);
    for (FrostYard* yard : {&plain, &spiked}) yard->World().FillBox({16, kG, -24}, {64, kG + 32, 24}, yard->Ids().stone);
    const glm::dvec3 eye(0.0, kGround + kEye, 0.0625);
    const glm::dvec3 ahead = eye + glm::dvec3(1.0, 0.0, 0.0);
    const sim::CastSpellResult spike = spiked.Cast(sim::kShatterSpike, 0, 1, eye, ahead);
    ASSERT_EQ(spike.refusal, sim::SpellRefusal::None);
    spiked.Land(spike);
    const int row = static_cast<int>(std::floor((kGround + kEye) / kVoxel));
    const int bored = AirRun(spiked.World(), 16, row, 0, 60);
    plain.RayBlast(eye, ahead);
    spiked.RayBlast(eye, ahead);
    const int a = AirRun(plain.World(), 16, row, 0, 60), b = AirRun(spiked.World(), 16, row, 0, 60);
    std::printf("Combo: blast depth %.2f m (rimed %.2f m), bore %.2f m\n", a * kVoxel, b * kVoxel, bored * kVoxel);
    EXPECT_GT(a, 0);
    EXPECT_GE(b * 2, a * 3) << "at least 1.5 times as deep";
}

TEST(Combo, RimedIronIsCarvedByABlast) {
    FrostYard plain, spiked;
    ASSERT_TRUE(plain.initialized && spiked.initialized);
    for (FrostYard* yard : {&plain, &spiked}) yard->World().FillBox({16, kG, -24}, {32, kG + 32, 24}, yard->Ids().iron);
    const glm::dvec3 eye(0.0, kGround + kEye, 0.0625);
    const glm::dvec3 ahead = eye + glm::dvec3(1.0, 0.0, 0.0);
    const sim::CastSpellResult spike = spiked.Cast(sim::kShatterSpike, 0, 1, eye, ahead);
    ASSERT_EQ(spike.refusal, sim::SpellRefusal::None);
    spiked.Land(spike);
    EXPECT_GT(spiked.Count({16, kG, -24}, {32, kG + 32, 24}, spiked.Ids().ironRimed), 50u);
    const sim::BlastResult a = plain.RayBlast(eye, ahead), b = spiked.RayBlast(eye, ahead);
    ASSERT_FALSE(a.ops.empty());
    ASSERT_FALSE(b.ops.empty());
    EXPECT_EQ(a.ops[0].result.changed, 0u) << "iron is harder than a blast";
    EXPECT_GT(b.ops[0].result.changed, 20u) << "rimed iron is not";
}

TEST(Combo, PunchShattersTheFloeBridge) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    // A moat 2 m deep across the yard, z 2..6 m.
    yard.World().FillBox({-40, kG - 16, 16}, {40, kG, 48}, m.water);
    const glm::dvec3 eye(0.0, kGround + kEye, 0.0);
    const sim::CastSpellResult freeze = yard.Cast(sim::kFlashFreeze, 0, 1, eye, glm::dvec3(0.0625, kGround, 4.0));
    ASSERT_EQ(freeze.refusal, sim::SpellRefusal::None) << sim::ToString(freeze.refusal);
    yard.Land(freeze);
    const glm::ivec3 lo(-40, kG - 16, 16), hi(40, kG, 48);
    const uint32_t floe = yard.Count(lo, hi, m.floe);
    EXPECT_GT(floe, 2000u) << "a slab two voxels thick over about 3 m";
    EXPECT_EQ(yard.Count({-40, kG - 16, 16}, {40, kG - 2, 48}, m.floe), 0u) << "only the top two layers";
    EXPECT_EQ(yard.World().GetVoxel({0, kG - 1, 32}), m.floe);
    for (int i = 0; i < 5; ++i) yard.Tick();
    // The floe is an anchor: no island search frees it, a blast does not hole it.
    const sim::BlastResult blast = yard.RayBlast(glm::dvec3(0.0625, kGround + 2.0, 4.0), glm::dvec3(0.0625, kGround, 4.0));
    EXPECT_EQ(yard.World().GetVoxel({0, kG - 1, 32}), m.floe);
    (void)blast;

    const glm::dvec3 above(0.0625, kGround + kEye, 4.0625);
    const sim::CastSpellResult punch = yard.Cast(sim::kPunch, 0, 1, above, glm::dvec3(0.0625, kGround - 0.5, 4.0625));
    ASSERT_EQ(punch.refusal, sim::SpellRefusal::None) << sim::ToString(punch.refusal);
    while (yard.simulation.Tick() <= punch.zoneTick) yard.Tick();
    const uint32_t after = yard.Count(lo, hi, m.floe);
    EXPECT_LT(after, floe);
    EXPECT_GT(floe - after, 20u);
    EXPECT_EQ(yard.World().GetVoxel({0, kG - 1, 32}), m.water) << "under the fist the floe is water again";
    EXPECT_EQ(yard.World().GetVoxel({0, kG - 2, 32}), m.water) << "both layers";
}

TEST(Magic, FloeThawsBackToWater) {
    FrostYard yard;
    ASSERT_TRUE(yard.initialized);
    const voxel::WorldMaterials& m = yard.Ids();
    yard.World().FillBox({-40, kG - 16, 16}, {40, kG, 48}, m.water);
    const glm::ivec3 lo(-40, kG - 16, 16), hi(40, kG, 48);
    const uint32_t water = yard.Count(lo, hi, m.water);
    const glm::dvec3 eye(0.0, kGround + kEye, 0.0);
    const sim::CastSpellResult freeze = yard.Cast(sim::kFlashFreeze, 0, 1, eye, glm::dvec3(0.0625, kGround, 4.0));
    ASSERT_EQ(freeze.refusal, sim::SpellRefusal::None);
    yard.Land(freeze);
    EXPECT_GT(yard.Count(lo, hi, m.floe), 0u);
    while (yard.simulation.Tick() <= freeze.runTick + yard.table.spells[sim::kFlashFreeze].floeLifetimeTicks) yard.Tick();
    EXPECT_EQ(yard.Count(lo, hi, m.floe), 0u);
    EXPECT_EQ(yard.Count(lo, hi, m.water), water);
}

// --- the player's statuses -----------------------------------------------------------------------------------------

TEST(Player, RootAndPinCannotChainPastTheGuard) {
    Field f;
    ASSERT_TRUE(f.initialized);
    for (int i = 0; i < 10; ++i) f.Tick();
    ASSERT_TRUE(f.state.OnGround());
    uint32_t t = f.simulation.Tick();
    ASSERT_TRUE(f.simulation.Zones().Insert(f.ZoneHere(1, sim::ZoneKind::Root, t, 30)));
    const glm::vec3 start = f.state.position;
    for (int i = 0; i < 30; ++i) {
        f.Tick(WalkX());
        EXPECT_TRUE(i == 29 || (f.state.status & sim::PlayerStatus_Rooted) != 0) << i;
    }
    EXPECT_NEAR(f.state.position.x, start.x, 0.02f) << "rooted for its 30 ticks";
    EXPECT_EQ(f.state.rootLeft, 0);
    EXPECT_EQ(f.state.ccGuardLeft, sim::kControlGuardTicks) << "the guard starts as the root ends";
    // A second root during the guard does not hold.
    t = f.simulation.Tick();
    ASSERT_TRUE(f.simulation.Zones().Insert(f.ZoneHere(2, sim::ZoneKind::Root, t, 30)));
    const glm::vec3 before = f.state.position;
    for (int i = 0; i < 10; ++i) f.Tick(WalkX());
    EXPECT_EQ(f.state.rootLeft, 0);
    EXPECT_GT(f.state.position.x - before.x, 0.4f);
    // Once the guard has run out, a root holds again.
    while (f.state.ccGuardLeft > 0) f.Tick();
    EXPECT_EQ(f.state.status & sim::PlayerStatus_Free, 0);
    t = f.simulation.Tick();
    ASSERT_TRUE(f.simulation.Zones().Insert(f.ZoneHere(3, sim::ZoneKind::Root, t, 30)));
    f.Tick(WalkX());
    EXPECT_GT(f.state.rootLeft, 0);
}

TEST(Player, ARespawnedPlayerIgnoresRootPinAndImpulse) {
    Field f;
    ASSERT_TRUE(f.initialized);
    for (int i = 0; i < 10; ++i) f.Tick();
    sim::StartFreshLife(f.state);
    EXPECT_NE(f.state.status & sim::PlayerStatus_Fresh, 0);
    const uint32_t t = f.simulation.Tick();
    sim::Zone impulse = f.ZoneHere(1, sim::ZoneKind::Impulse, t, 1, 900, 300);
    impulse.shape.a.x -= 64;   // its centre 0.5 m beside the feet
    ASSERT_TRUE(f.simulation.Zones().Insert(impulse));
    ASSERT_TRUE(f.simulation.Zones().Insert(f.ZoneHere(2, sim::ZoneKind::Root, t, 30)));
    f.Tick();
    EXPECT_EQ(f.state.rootLeft, 0);
    EXPECT_EQ(f.state.push[0], 0);
    EXPECT_EQ(f.state.push[1], 0);
    EXPECT_NE(f.state.status & sim::PlayerStatus_Fresh, 0);
    // A cast ends Fresh; the guard runs on, so a root still does not hold.
    sim::PlayerInput cast;
    cast.buttons = sim::PlayerButton_Cast;
    EXPECT_TRUE(f.Tick(cast).cast);
    EXPECT_EQ(f.state.status & sim::PlayerStatus_Fresh, 0);
    EXPECT_GT(f.state.ccGuardLeft, 0);
    const uint32_t t2 = f.simulation.Tick();
    ASSERT_TRUE(f.simulation.Zones().Insert(f.ZoneHere(3, sim::ZoneKind::Root, t2, 30)));
    f.Tick();
    EXPECT_EQ(f.state.rootLeft, 0);
    // Without a cast, Fresh ends with the guard (2 s).
    sim::PlayerState fresh;
    sim::StartFreshLife(fresh);
    EXPECT_EQ(fresh.ccGuardLeft, sim::kFreshGuardTicks);
}

TEST(Player, ReleaseFreesARootedAlly) {
    Field f;
    ASSERT_TRUE(f.initialized);
    for (int i = 0; i < 10; ++i) f.Tick();
    const uint32_t t = f.simulation.Tick();
    ASSERT_TRUE(f.simulation.Zones().Insert(f.ZoneHere(1, sim::ZoneKind::Root, t, 90)));
    ASSERT_TRUE(f.simulation.Zones().Insert(f.ZoneHere(2, sim::ZoneKind::Chill, t, 90, 6, 60)));
    ASSERT_TRUE(f.simulation.Zones().Insert(f.ZoneHere(3, sim::ZoneKind::Release, t + 10, 1)));
    for (int i = 0; i < 10; ++i) f.Tick();
    EXPECT_GT(f.state.rootLeft, 0);
    EXPECT_GT(f.state.slowLeft, 0);
    f.Tick();
    EXPECT_EQ(f.state.rootLeft, 0) << "released";
    EXPECT_EQ(f.state.ccGuardLeft, sim::kControlGuardTicks - 1) << "the guard starts at the release";
    const glm::vec3 before = f.state.position;
    for (int i = 0; i < 10; ++i) f.Tick(WalkX());
    EXPECT_GT(f.state.position.x - before.x, 0.2f) << "walks again (still chilled while inside the Chill)";
}

TEST(Player, AChillSlowsTheWishAndEnds) {
    Field slow, plain;
    ASSERT_TRUE(slow.initialized && plain.initialized);
    for (Field* f : {&slow, &plain})
        for (int i = 0; i < 10; ++i) f->Tick();
    const uint32_t t = slow.simulation.Tick();
    ASSERT_TRUE(slow.simulation.Zones().Insert(slow.ZoneHere(1, sim::ZoneKind::Chill, t, 20, 6, 30)));
    const float x0 = slow.state.position.x, y0 = plain.state.position.x;
    for (int i = 0; i < 20; ++i) {
        slow.Tick(WalkX());
        plain.Tick(WalkX());
    }
    const float ratio = (slow.state.position.x - x0) / (plain.state.position.x - y0);
    EXPECT_NEAR(ratio, 10.0f / 16.0f, 0.08f) << "slowQ 6: speed x 0.625";
    EXPECT_NE(slow.state.status & sim::PlayerStatus_Chilled, 0);
    for (int i = 0; i < 40; ++i) slow.Tick(WalkX());
    EXPECT_EQ(slow.state.slowLeft, 0);
    EXPECT_EQ(slow.state.slowQ, 0);
    EXPECT_EQ(slow.state.status & sim::PlayerStatus_Chilled, 0);
}

TEST(Player, SlickGroundReadsTheSameVoxelAtASurface) {
    for (const float vs : {0.1f, 0.125f, 0.15f}) {
        for (const int k : {96, 97, -40}) {
            for (const float e : {-1.0e-4f, 0.0f, 1.0e-4f}) {
                const glm::vec3 feet(3.3f, static_cast<float>(k) * vs + e, -2.7f);
                EXPECT_EQ(sim::GroundVoxel(feet, vs).y, k - 1) << vs << " " << k << " " << e;
            }
        }
    }
    Field f;
    ASSERT_TRUE(f.initialized);
    f.simulation.World().FillBox({0, kG - 1, 0}, {8, kG, 8}, f.simulation.WorldMaterialIds().ice);
    for (const float e : {-1.0e-4f, 0.0f, 1.0e-4f}) {
        EXPECT_TRUE(sim::OnSlickGround(&f.simulation.World(), {0.5f, kGround + e, 0.5f})) << e;
        EXPECT_FALSE(sim::OnSlickGround(&f.simulation.World(), {2.0f, kGround + e, 2.0f})) << e;
    }
    EXPECT_FALSE(sim::OnSlickGround(nullptr, {0.5f, kGround, 0.5f}));
}

TEST(Player, SlickGroundChangesOnlyAcceleration) {
    Field f;
    ASSERT_TRUE(f.initialized);
    f.simulation.World().FillBox({-32, kG - 1, -32}, {160, kG, 32}, f.simulation.WorldMaterialIds().ice);
    for (int i = 0; i < 10; ++i) f.Tick();
    ASSERT_TRUE(f.state.OnGround());
    ASSERT_TRUE(sim::OnSlickGround(&f.simulation.World(), f.state.position));
    f.Tick(WalkX());
    EXPECT_LT(SpeedXZ(f.state), 0.2f) << "3 m/s^2: about 0.05 m/s after one tick";
    for (int i = 0; i < 120; ++i) f.Tick(WalkX());
    EXPECT_GT(SpeedXZ(f.state), f.tuning.walkSpeed) << "the wish x 1.3 after 2 s";
    EXPECT_LE(SpeedXZ(f.state), f.tuning.runSpeed * 1.4f + 0.01f);
    for (int i = 0; i < 10; ++i) f.Tick();
    EXPECT_GT(SpeedXZ(f.state), 3.0f) << "momentum carries on ice";
}
