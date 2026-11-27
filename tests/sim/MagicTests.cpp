// The cast pipeline in sim (docs/design/magic.md "The cast pipeline", "Caps", "Players: zones,
// statuses and the movement rule"): CastSpell plans, guards and applies Updraft, Dash, Punch and
// Barricade Wave; RunScheduled runs their bands, blows and zone ticks and the expiries.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <span>
#include <utility>
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

    sim::SimulationConfig MagicConfig() {
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
    };

    /// Flat ground at 0.125 m with players stepped by the movement rule and the cast pipeline's follow-ups.
    struct MagicYard {
        explicit MagicYard(bool server = false) : simulation(server ? sim::ServerConfig(MagicConfig()) : MagicConfig()) {
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

        std::vector<glm::dvec3> Focus() const {
            std::vector<glm::dvec3> focus{glm::dvec3(0.0, kGround, 0.0)};
            for (const Fighter& f : fighters) focus.push_back(glm::dvec3(f.state.position));
            return focus;
        }

        /// One tick in the server's order: the players' steps, then the follow-ups, then physics.
        sim::ScheduledResult Tick() { return TickWith(nullptr, sim::PlayerInput{}, [] {}); }

        /// The same tick with `input` for `who` (the others stand), and `afterSteps` between the steps and the
        /// follow-ups (where a server runs its casts).
        template <class F>
        sim::ScheduledResult TickWith(const Fighter* who, const sim::PlayerInput& input, F&& afterSteps) {
            const std::vector<glm::dvec3> focus = Focus();
            simulation.UpdateStreaming(focus);
            for (Fighter& f : fighters) {
                simulation.Terrain().FlushBox(sim::PlayerReach(f.state.position, ctx.tuning));
                const sim::PlayerZones zones{&simulation.Zones(), &simulation.World(), simulation.Tick(), static_cast<uint8_t>(1u << f.team)};
                sim::StepPlayer(simulation.Physics(), simulation.Terrain(), f.character, f.state, &f == who ? input : sim::PlayerInput{}, ctx.tuning,
                                sim::PlayArea{}, zones, simulation.FixedTimeStep());
            }
            afterSteps();
            sim::ScheduledResult r = sim::RunScheduled(simulation, table, ctx, simulation.Tick(), Targets());
            simulation.StepPhysics();
            if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
            return r;
        }

        void Settle() {
            const std::vector<glm::dvec3> focus = Focus();
            simulation.Terrain().Update(focus, simulation.Config().colliderRadius);
            simulation.Terrain().Flush();
            for (int i = 0; i < 10; ++i) Tick();
        }

        sim::CastSpellResult Cast(sim::SpellId spell, uint8_t player, sim::TeamId team, const glm::dvec3& eye, const glm::dvec3& target,
                                  sim::PlayerState* state = nullptr) {
            sim::SpellCast c;
            c.player = player;
            c.team = team;
            c.spell = spell;
            c.eye = eye;
            c.dir = glm::vec3(glm::normalize(target - eye));
            c.casterState = state;
            const std::vector<sim::SpellTarget> targets = Targets();
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

    /// The top centre of ground voxel (x, kG - 1, z), metres.
    glm::dvec3 GroundAt(int x, int z) { return {(x + 0.5) * kVoxel, kGround, (z + 0.5) * kVoxel}; }

    /// The voxel's centre is inside `feet`'s capsule bounds grown by one voxel.
    bool InGrownBounds(const glm::ivec3& v, const glm::vec3& feet, const sim::PlayerTuning& tuning) {
        const math::AABB b = sim::PlayerBounds(feet, tuning);
        for (int i = 0; i < 3; ++i) {
            const float c = (static_cast<float>(v[i]) + 0.5f) * kVoxel;
            if (c < b.min[i] - kVoxel || c > b.max[i] + kVoxel) return false;
        }
        return true;
    }

    const glm::ivec3 kWallBoxMin(-24, kG, -48), kWallBoxMax(25, kG + 24, 48);
}

// --- Updraft ------------------------------------------------------------------------------------

TEST(Magic, UpdraftLiftsEveryoneInItsColumnToAHover) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    Fighter& own = yard.Add(1, 1, {0.0625f, kGround, 0.0625f});
    Fighter& enemy = yard.Add(2, 2, {0.5f, kGround, 0.3f});
    Fighter& away = yard.Add(3, 2, {3.0f, kGround, 0.0f});
    yard.Settle();
    ASSERT_TRUE(own.state.OnGround());
    const float y0 = own.state.position.y;

    const sim::CastSpellResult cast = yard.Cast(sim::kUpdraft, 0, 1, {-2.0, kGround + 2.0, 0.0625}, GroundAt(0, 0));
    ASSERT_EQ(cast.refusal, sim::SpellRefusal::None) << sim::ToString(cast.refusal);
    ASSERT_EQ(cast.zonesStarted.size(), 1u);
    const sim::Zone& z = cast.zonesStarted[0];
    EXPECT_EQ(z.kind, sim::ZoneKind::Lift);
    EXPECT_EQ(z.teamMask, 0xFF);
    EXPECT_EQ(z.startTick, cast.runTick + 24);
    EXPECT_EQ(z.duration, 90);
    EXPECT_EQ(z.shape.kind, voxel::kShapeCylinder);
    EXPECT_EQ(z.shape.a.y - z.shape.halfHeightQ, (kG - 1) * 16) << "from one voxel under the seed cell: the feet rest in the hit face";
    EXPECT_EQ(2 * z.shape.halfHeightQ, 82 * 16) << "the seed cell and 80 cells above it";
    EXPECT_EQ(cast.stats.bytes, 32u + 34u);

    // Nothing moves before T_z: the lead.
    while (yard.simulation.Tick() < z.startTick) yard.Tick();
    EXPECT_NEAR(own.state.position.y, y0, 0.01f);
    float topOwn = 0.0f, topEnemy = 0.0f;
    while (yard.simulation.Tick() < z.EndTick()) {
        yard.Tick();
        topOwn = std::max(topOwn, own.state.position.y - y0);
        topEnemy = std::max(topEnemy, enemy.state.position.y - y0);
    }
    const float columnTop = 81 * kVoxel;
    std::printf("Updraft: own %.2f m, enemy %.2f m, end speed %.2f m/s (column %.2f m)\n", topOwn, topEnemy, own.state.velocity.y, columnTop);
    EXPECT_GT(topOwn, 6.0f);
    EXPECT_GT(topEnemy, 6.0f);
    EXPECT_LT(topOwn, columnTop + 0.05f) << "never past the top (the feet start 2 cm in the ground)";
    EXPECT_LT(own.state.velocity.y, 2.5f) << "eased toward a hover at the top";
    EXPECT_TRUE(own.state.status & sim::PlayerStatus_Lifted);
    EXPECT_NEAR(away.state.position.y, y0, 0.01f) << "outside the column";
    // The zone has ended and is pruned with its owner.
    yard.Tick();
    EXPECT_EQ(yard.simulation.Zones().Find(z.zoneId), nullptr);
    EXPECT_TRUE(yard.ctx.zoneOwners.empty());
}

TEST(Magic, AnUpdraftKicksTheEightNearestBodiesUpOnce) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    scene::Scene& scene = yard.simulation.Scene();
    std::vector<scene::Entity> crates;
    for (const float x : {-0.6f, -0.2f, 0.2f, 0.6f})
        for (const float z : {-0.4f, 0.0f, 0.4f}) {
            voxel::VoxelVolume volume(glm::ivec3(2));
            volume.FillBox(glm::ivec3(0), glm::ivec3(2), yard.Ids().wood);
            const scene::Entity crate = scene.CreateVoxelObject(
                {.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, "crate"),
                 .position = {x, kGround + 0.01f, z}});
            ASSERT_NE(crate, scene::kNoEntity);
            crates.push_back(crate);
        }
    yard.Settle();
    const sim::CastSpellResult cast = yard.Cast(sim::kUpdraft, 0, 1, {-2.0, kGround + 2.0, 0.0625}, GroundAt(0, 0));
    ASSERT_EQ(cast.refusal, sim::SpellRefusal::None);
    const glm::vec2 axis(0.0625f, 0.0625f);
    while (yard.simulation.Tick() < cast.zoneTick) EXPECT_EQ(yard.Tick().bodiesPushed, 0u);
    EXPECT_EQ(yard.Tick().bodiesPushed, 8u) << "at T_z, at most 8";
    float nearestUnkicked = 1.0e9f, farthestKicked = 0.0f;
    uint32_t kicked = 0;
    physics::PhysicsWorld& physics = yard.simulation.Physics();
    for (const scene::Entity crate : crates) {
        const physics::BodyState s = physics.GetState(scene.TryGet<scene::RigidBody>(crate)->body);
        const float d = glm::length(glm::vec2(s.position.x, s.position.z) - axis);
        if (s.linearVelocity.y > 3.0f) {
            ++kicked;
            farthestKicked = std::max(farthestKicked, d);
        } else {
            nearestUnkicked = std::min(nearestUnkicked, d);
        }
    }
    EXPECT_EQ(kicked, 8u);
    EXPECT_LE(farthestKicked, nearestUnkicked) << "nearest first";
    for (int i = 0; i < 30; ++i) EXPECT_EQ(yard.Tick().bodiesPushed, 0u) << "one kick, on the start tick";
}

// --- Dash ---------------------------------------------------------------------------------------

TEST(Magic, DashMovesItsCasterAndShovesEnemiesBesideThePath) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    Fighter& caster = yard.Add(1, 1, {-4.0f, kGround, 0.5f});
    Fighter& ally = yard.Add(2, 1, {0.0f, kGround, 1.2f});
    Fighter& enemy = yard.Add(3, 2, {1.5f, kGround, -0.2f});
    yard.Settle();
    ASSERT_TRUE(caster.state.OnGround());
    const glm::vec3 start = caster.state.position;
    const glm::dvec3 eye = glm::dvec3(start) + glm::dvec3(0.0, kEye, 0.0);

    const sim::CastSpellResult cast = yard.Cast(sim::kDash, 1, 1, eye, eye + glm::dvec3(1.0, -0.2, 0.0), &caster.state);
    ASSERT_EQ(cast.refusal, sim::SpellRefusal::None) << sim::ToString(cast.refusal);
    EXPECT_TRUE(cast.dashStarted);
    EXPECT_EQ(caster.state.dashLeft, sim::kDashTicks);
    EXPECT_EQ(cast.stats.dashSteps, 64u) << "8 m in 0.125 m steps on open ground";
    ASSERT_EQ(cast.zonesStarted.size(), 1u);
    const sim::Zone& z = cast.zonesStarted[0];
    EXPECT_EQ(z.kind, sim::ZoneKind::Impulse);
    EXPECT_EQ(z.shape.kind, voxel::kShapeCapsule);
    EXPECT_EQ(z.shape.b.x - z.shape.a.x, 8 * 128);
    EXPECT_EQ(z.teamMask, static_cast<uint8_t>(0xFF & ~(1u << 1)));
    EXPECT_EQ(cast.stats.bytes, 42u + 34u);

    for (int i = 0; i < sim::kDashTicks; ++i) yard.Tick();
    EXPECT_EQ(caster.state.dashLeft, 0);
    EXPECT_NEAR(caster.state.position.x - start.x, 8.0f, 0.5f);
    while (yard.simulation.Tick() < z.startTick) yard.Tick();
    EXPECT_EQ(enemy.state.push[2], 0);
    const glm::vec3 enemyBefore = enemy.state.position, allyBefore = ally.state.position;
    yard.Tick();   // T_z: the shove acts in the step
    EXPECT_LT(enemy.state.push[2], -100) << "away from the path's axis (z 0.5)";
    EXPECT_EQ(ally.state.push[0], 0);
    EXPECT_EQ(ally.state.push[2], 0);
    for (int i = 0; i < 20; ++i) yard.Tick();
    EXPECT_LT(enemy.state.position.z, enemyBefore.z - 0.05f);
    EXPECT_NEAR(ally.state.position.z, allyBefore.z, 1.0e-3f);

    // A wall cuts the traced path: the shove stops before it.
    yard.World().FillBox({8, kG, -24}, {12, kG + 24, 24}, yard.Ids().stone);   // its face at x = 1 m
    const glm::dvec3 eye2(-3.0, kGround + kEye, 0.5);
    const sim::CastSpellResult blocked = yard.Cast(sim::kDash, 0, 1, eye2, eye2 + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(blocked.refusal, sim::SpellRefusal::None);
    EXPECT_FALSE(blocked.dashStarted) << "a script has no state to move";
    EXPECT_LE(-3.0 + blocked.stats.dashSteps * kVoxel + 0.3, 1.0) << "the capsule stops before the wall";
    EXPECT_GE(-3.0 + blocked.stats.dashSteps * kVoxel + 0.3, 1.0 - kVoxel);
}

// --- Punch --------------------------------------------------------------------------------------

TEST(Magic, PunchBreaksPlanksNotStoneAndShovesAnEnemyAlongTheAim) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint8_t planks = yard.Ids().planks, stone = yard.Ids().stone;
    yard.World().FillBox({16, kG, -16}, {18, kG + 24, 0}, planks);    // x 2.0-2.25 m
    yard.World().FillBox({-18, kG, -16}, {-16, kG + 24, 0}, stone);   // x -2.25..-2.0 m
    Fighter& enemy = yard.Add(1, 2, {0.0625f, kGround, 1.5f});
    Fighter& ally = yard.Add(2, 1, {0.6f, kGround, 1.5f});
    yard.Settle();

    const glm::dvec3 eye(0.0625, kGround + kEye, -0.9375);
    const sim::CastSpellResult atPlanks = yard.Cast(sim::kPunch, 0, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    const sim::CastSpellResult atStone = yard.Cast(sim::kPunch, 0, 1, eye, eye + glm::dvec3(-1.0, 0.0, 0.0));
    const glm::dvec3 eye2(0.0625, kGround + kEye, 0.0625);
    const sim::CastSpellResult atEnemy = yard.Cast(sim::kPunch, 0, 1, eye2, eye2 + glm::dvec3(0.0, 0.0, 1.0));
    ASSERT_EQ(atPlanks.refusal, sim::SpellRefusal::None) << sim::ToString(atPlanks.refusal);
    ASSERT_EQ(atStone.refusal, sim::SpellRefusal::None);
    ASSERT_EQ(atEnemy.refusal, sim::SpellRefusal::None);
    EXPECT_EQ(atPlanks.hitQ.x, 16 * 16) << "the fist is the plank face";
    EXPECT_EQ(atStone.hitQ.x, -16 * 16);
    EXPECT_NEAR(atEnemy.hitQ.z / 128.0, 1.2, 0.05) << "the enemy's capsule";
    EXPECT_TRUE(atPlanks.groups.empty()) << "the blow lands at T_z";

    const glm::ivec3 plank(16, kG + 12, -8), stoneVoxel(-17, kG + 12, -8);
    const uint32_t tZ = atPlanks.zoneTick;
    uint32_t damaged = 0;
    while (yard.simulation.Tick() < tZ) {
        const sim::ScheduledResult r = yard.Tick();
        EXPECT_TRUE(r.groups.empty());
        EXPECT_EQ(yard.World().GetVoxel(plank), planks);
    }
    const glm::vec3 enemyBefore = enemy.state.position, allyBefore = ally.state.position;
    const sim::ScheduledResult blow = yard.Tick();
    EXPECT_EQ(yard.World().GetVoxel(plank), 0) << "planks break";
    EXPECT_EQ(yard.World().GetVoxel(stoneVoxel), stone) << "stone holds";
    EXPECT_GT(enemy.state.push[2], 1000) << "shoved along the aim";
    EXPECT_GT(enemy.state.push[1] + enemy.state.velocity.y * 100.0f, 0.0f);
    EXPECT_EQ(ally.state.push[2], 0);
    for (const sim::SpellDamage& d : blow.damage) {
        EXPECT_EQ(d.player, enemy.player);
        EXPECT_EQ(d.amount, 25);
        ++damaged;
    }
    EXPECT_EQ(damaged, 1u) << "one Punch caught the enemy; the ally takes nothing";
    for (int i = 0; i < 30; ++i) yard.Tick();
    EXPECT_GT(enemy.state.position.z - enemyBefore.z, 1.0f);
    EXPECT_NEAR(ally.state.position.z, allyBefore.z, 1.0e-3f);
}

TEST(Magic, PunchTruncatesLiveEnemyZones) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 above(0.0, kGround + 4.0, -3.0);
    // Updrafts at x = 1 m (team 2 and team 1, 1 m apart in z) and a far one of team 2 at x = -8 m.
    const sim::CastSpellResult enemyLift = yard.Cast(sim::kUpdraft, 0, 2, above, GroundAt(8, 0));
    const sim::CastSpellResult ownLift = yard.Cast(sim::kUpdraft, 0, 1, above, GroundAt(8, 8));
    const sim::CastSpellResult farLift = yard.Cast(sim::kUpdraft, 0, 2, above, GroundAt(-64, 0));
    ASSERT_EQ(enemyLift.refusal, sim::SpellRefusal::None);
    ASSERT_EQ(ownLift.refusal, sim::SpellRefusal::None);
    ASSERT_EQ(farLift.refusal, sim::SpellRefusal::None);
    const uint16_t enemyId = enemyLift.zonesStarted[0].zoneId, ownId = ownLift.zonesStarted[0].zoneId, farId = farLift.zonesStarted[0].zoneId;
    EXPECT_TRUE(yard.ctx.zoneOwners.at(enemyId).dispellable);
    for (int i = 0; i < 30; ++i) yard.Tick();   // the Updrafts are live

    const glm::dvec3 eye(0.0625, kGround + kEye, 0.0625);
    const sim::CastSpellResult punch = yard.Cast(sim::kPunch, 0, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(punch.refusal, sim::SpellRefusal::None);
    ASSERT_EQ(punch.zonesCancelled.size(), 1u);
    EXPECT_EQ(punch.zonesCancelled[0], (std::pair<uint16_t, uint32_t>(enemyId, punch.zoneTick)));
    EXPECT_EQ(yard.simulation.Zones().Find(enemyId)->EndTick(), punch.zoneTick);
    EXPECT_EQ(yard.simulation.Zones().Find(ownId)->EndTick(), ownLift.zonesStarted[0].EndTick()) << "an own-team zone stays";
    EXPECT_EQ(yard.simulation.Zones().Find(farId)->EndTick(), farLift.zonesStarted[0].EndTick()) << "out of reach";
    // The Shove and the ally Release (capsules), the cancel, the CastFx and the blow; the Shatter flood's mask is
    // counted when it runs at T_z.
    EXPECT_EQ(punch.stats.bytes, 42u + 42u + 7u + 34u + static_cast<uint32_t>(sim::OpWireBytes(sim::WorldOp::Carve({}, 0))));

    // A truncated zone that ends at T_z is not live then: a second Punch cancels nothing.
    const sim::CastSpellResult again = yard.Cast(sim::kPunch, 0, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    ASSERT_EQ(again.refusal, sim::SpellRefusal::None);
    EXPECT_TRUE(again.zonesCancelled.empty());
    while (yard.simulation.Tick() <= punch.zoneTick) yard.Tick();
    EXPECT_EQ(yard.simulation.Zones().Find(enemyId), nullptr) << "pruned once it ended";
    EXPECT_NE(yard.simulation.Zones().Find(ownId), nullptr);
}

// --- Barricade Wave -----------------------------------------------------------------------------

TEST(Magic, BarricadeRisesInFourBandsAndCrumblesAt20s) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint8_t knit = yard.Ids().knitIron;
    const sim::CastSpellResult cast = yard.Cast(sim::kBarricadeWave, 0, 1, {-3.0, kGround + 2.0, 0.0625}, GroundAt(0, 0));
    ASSERT_EQ(cast.refusal, sim::SpellRefusal::None) << sim::ToString(cast.refusal);
    EXPECT_GE(cast.stats.planned, 1250u);
    EXPECT_LE(cast.stats.planned, 1340u);
    EXPECT_TRUE(cast.zonesStarted.empty());
    EXPECT_TRUE(cast.groups.empty()) << "bands go out on their ticks";
    EXPECT_EQ(yard.ctx.schedule.size(), 4u);
    EXPECT_GT(cast.work.ops, 4 * sim::kOpWork);

    const uint32_t t0 = cast.runTick;
    std::array<uint32_t, 4> afterBand{};
    uint32_t last = 0;
    for (uint32_t t = t0; t <= t0 + 30; ++t) {
        sim::RunScheduled(yard.simulation, yard.table, yard.ctx, t, {});
        const uint32_t n = yard.Count(kWallBoxMin, kWallBoxMax, knit);
        const bool bandTick = t >= t0 + 18 && (t - t0 - 18) % 4 == 0;
        if (bandTick) {
            EXPECT_GT(n, last) << "band at T_run + " << t - t0;
            afterBand[(t - t0 - 18) / 4] = n;
        } else {
            EXPECT_EQ(n, last) << "T_run + " << t - t0;
        }
        last = n;
    }
    std::printf("Barricade: knit after each band %u %u %u %u\n", afterBand[0], afterBand[1], afterBand[2], afterBand[3]);
    EXPECT_EQ(afterBand[3], cast.stats.planned);
    EXPECT_TRUE(yard.ctx.schedule.empty());
    EXPECT_EQ(yard.ctx.registry.LiveVoxels(0), cast.stats.planned);

    for (uint32_t t = t0 + 31; t < t0 + 1200; ++t) sim::RunScheduled(yard.simulation, yard.table, yard.ctx, t, {});
    EXPECT_EQ(yard.Count(kWallBoxMin, kWallBoxMax, knit), cast.stats.planned) << "still standing at 19.98 s";
    const sim::ScheduledResult expiry = sim::RunScheduled(yard.simulation, yard.table, yard.ctx, t0 + 1200, {});
    EXPECT_EQ(yard.Count(kWallBoxMin, kWallBoxMax, knit), 0u) << "gone at 20 s";
    EXPECT_EQ(yard.ctx.registry.Owners(), 0u);
    EXPECT_GT(expiry.sitesRun, 0u) << "the expiry turned colliders into air: site checks";
}

TEST(Magic, FillNeverEntombsAPlayer) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint8_t knit = yard.Ids().knitIron;
    sim::PlayerTuning tuning;
    // A player stands on the line, then walks out along it one band at a time.
    std::vector<sim::SpellTarget> targets{{1, 2, {0.0625f, kGround, 0.0625f}}};
    const glm::dvec3 eye(-3.0, kGround + 2.0, 0.0625);
    sim::SpellCast c;
    c.spell = sim::kBarricadeWave;
    c.team = 1;
    c.eye = eye;
    c.dir = glm::vec3(glm::normalize(GroundAt(0, 0) - eye));
    const sim::CastSpellResult cast = sim::CastSpell(yard.simulation, yard.table, c, yard.ctx, targets);
    ASSERT_EQ(cast.refusal, sim::SpellRefusal::None);
    EXPECT_LT(cast.stats.planned, 1250u) << "the cells round the player were left out";

    const uint32_t t0 = cast.runTick;
    uint32_t bands = 0, filled = 0;
    for (uint32_t t = t0; t <= t0 + 30; ++t) {
        const uint32_t k = t >= t0 + 18 ? (t - t0 - 18) / 4 : 0;
        targets[0].feet.z = 0.0625f + 0.6f * static_cast<float>(k);
        const sim::ScheduledResult r = sim::RunScheduled(yard.simulation, yard.table, yard.ctx, t, targets);
        for (const sim::BlastResult& group : r.groups)
            for (const sim::AppliedOp& applied : group.ops) {
                ++bands;
                for (const voxel::VoxelEdit& e : applied.edits) {
                    ++filled;
                    EXPECT_FALSE(InGrownBounds(e.voxel, targets[0].feet, tuning)) << "band at T_run + " << t - t0;
                }
            }
    }
    EXPECT_EQ(bands, 4u);
    EXPECT_GT(filled, 400u);
    // Where it stood at the cast there is a gap the height of the wall.
    for (int y = kG; y < kG + 12; ++y) EXPECT_NE(yard.World().GetVoxel({0, y, 0}), knit) << y;
}

// --- the spawn guard and the caps ---------------------------------------------------------------

TEST(Magic, NoSpellReachesAnEnemySpawn) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    const std::array<std::pair<sim::TeamId, glm::dvec3>, 2> spawns = {std::pair<sim::TeamId, glm::dvec3>{1, {-8.0, kGround, 0.0}},
                                                                     std::pair<sim::TeamId, glm::dvec3>{2, {6.0, kGround, 0.0}}};
    yard.ctx.spawns = sim::SpawnAreasOf(spawns, yard.World());
    ASSERT_EQ(yard.ctx.spawns.size(), 2u);
    EXPECT_EQ(yard.ctx.spawns[1].min.x, 0);
    EXPECT_EQ(yard.ctx.spawns[1].max.x, 97);
    EXPECT_EQ(yard.ctx.spawns[0].max.x, -15);
    const uint8_t knit = yard.Ids().knitIron;
    const uint64_t before = yard.Hash();

    // Into the enemy's area: refused, nothing planned is applied or scheduled.
    const sim::CastSpellResult wall = yard.Cast(sim::kBarricadeWave, 1, 1, {-1.0, kGround + 2.0, 0.0625}, GroundAt(16, 0));
    const sim::CastSpellResult lift = yard.Cast(sim::kUpdraft, 1, 1, {-3.0, kGround + 2.0, 0.0625}, GroundAt(-4, 0));
    const glm::dvec3 eye(-0.5, kGround + kEye, 0.0625);
    const sim::CastSpellResult punch = yard.Cast(sim::kPunch, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    const sim::CastSpellResult dash = yard.Cast(sim::kDash, 1, 1, eye, eye + glm::dvec3(1.0, 0.0, 0.0));
    for (const sim::CastSpellResult* r : {&wall, &lift, &punch, &dash}) {
        EXPECT_EQ(r->refusal, sim::SpellRefusal::NearSpawn) << sim::ToString(r->refusal);
        EXPECT_TRUE(r->groups.empty());
        EXPECT_TRUE(r->zonesStarted.empty());
    }
    EXPECT_TRUE(yard.ctx.schedule.empty());
    EXPECT_EQ(yard.simulation.Zones().Size(), 0u);
    EXPECT_EQ(yard.ctx.registry.Owners(), 0u);
    EXPECT_EQ(yard.Hash(), before);
    // The enemy may cast there.
    EXPECT_EQ(yard.Cast(sim::kUpdraft, 2, 2, {-3.0, kGround + 2.0, 0.0625}, GroundAt(-4, 0)).refusal, sim::SpellRefusal::None);

    // At the own area's edge: cast, with no fill cell inside the area.
    const sim::CastSpellResult edge = yard.Cast(sim::kBarricadeWave, 1, 1, {1.2, kGround + 2.0, 0.0625}, GroundAt(-15, 0));
    ASSERT_EQ(edge.refusal, sim::SpellRefusal::None) << sim::ToString(edge.refusal);
    EXPECT_GT(edge.stats.planned, 0u);
    for (uint32_t t = edge.runTick; t <= edge.runTick + 30; ++t) sim::RunScheduled(yard.simulation, yard.table, yard.ctx, t, {});
    const sim::SpawnArea& own = yard.ctx.spawns[0];
    EXPECT_EQ(yard.Count({own.min.x, kG, -48}, {own.max.x, kG + 24, 48}, knit), 0u);
    EXPECT_EQ(yard.Count({own.max.x, kG, -48}, {own.max.x + 8, kG + 24, 48}, knit), edge.stats.planned);
}

TEST(Magic, CastRejectedOverTheCaps) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    const glm::dvec3 above(0.0, kGround + 4.0, -3.0);
    const auto lift = [&yard, &above](uint8_t player) {
        return yard.Cast(sim::kUpdraft, player, static_cast<sim::TeamId>(1 + player % 2), above, GroundAt(0, 0)).refusal;
    };
    // 4 zones per player.
    for (int i = 0; i < 4; ++i) EXPECT_EQ(lift(1), sim::SpellRefusal::None);
    EXPECT_EQ(lift(1), sim::SpellRefusal::TooManyZones);
    // 32 in the region: players 2..8 fill it, the 33rd zone (player 9's first) is refused.
    for (uint8_t p = 2; p <= 8; ++p)
        for (int i = 0; i < 4; ++i) EXPECT_EQ(lift(p), sim::SpellRefusal::None) << int{p};
    EXPECT_EQ(yard.simulation.Zones().LiveOrPendingAt(yard.simulation.Tick()), 32u);
    EXPECT_EQ(lift(9), sim::SpellRefusal::TooManyZones);
    EXPECT_EQ(yard.simulation.Zones().Size(), 32u);
    // Scripts have no per-player cap, but the region's holds.
    EXPECT_EQ(lift(0), sim::SpellRefusal::TooManyZones);

    // The region's conjured voxels: 59 000 live, a wall of about 1 300 more is refused.
    sim::KnitOwnerInfo info;
    info.expiryMap = voxel::Map_Unknit;
    info.caster = 50;
    info.dueTick = 100000;
    const uint32_t owner = yard.ctx.registry.NewOwner(info);
    std::vector<voxel::VoxelEdit> edits;
    ASSERT_TRUE(sim::ApplyOp(yard.World(), sim::WorldOp::Box({40, kG + 4, -30}, {90, kG + 24, 29}, yard.Ids().knitIron), &edits).Complete());
    yard.ctx.registry.Record(owner, edits, yard.simulation.Materials());
    ASSERT_EQ(yard.ctx.registry.LiveVoxels(0), 59000u);
    const size_t scheduled = yard.ctx.schedule.size();
    const sim::CastSpellResult wall = yard.Cast(sim::kBarricadeWave, 0, 1, {-3.0, kGround + 2.0, 8.0625}, GroundAt(0, 64));
    EXPECT_EQ(wall.refusal, sim::SpellRefusal::TooMuchConjured);
    EXPECT_EQ(yard.ctx.schedule.size(), scheduled);
    EXPECT_EQ(yard.ctx.registry.Owners(), 1u);
}

TEST(Magic, TheKnitCapCrumblesTheCastersOldestWall) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint8_t knit = yard.Ids().knitIron;
    std::vector<sim::CastSpellResult> walls;
    for (const int z : {-80, -40, 0, 40, 80}) {
        sim::RefillMana(yard.table, yard.ctx.casters[4], yard.simulation.Tick());   // the knit cap, not the mana, is under test
        walls.push_back(yard.Cast(sim::kBarricadeWave, 4, 1, {-3.0, kGround + 2.0, (z + 0.5) * kVoxel}, GroundAt(0, z)));
        ASSERT_EQ(walls.back().refusal, sim::SpellRefusal::None);
        if (walls.size() < 5) EXPECT_TRUE(walls.back().groups.empty());
        if (walls.size() == 5) break;
        for (int i = 0; i < 31; ++i) yard.Tick();
    }
    // 4 walls stand (about 5 300 voxels); a fifth passes the 6 000 cap: the first crumbles at the cast.
    ASSERT_EQ(walls[4].groups.size(), 1u);
    EXPECT_EQ(walls[4].groups[0].ops[0].op.map, voxel::Map_Unknit);
    EXPECT_EQ(walls[4].groups[0].ops[0].result.changed, walls[0].stats.planned);
    EXPECT_EQ(yard.Count({-24, kG, -100}, {25, kG + 24, -60}, knit), 0u);
    EXPECT_EQ(yard.ctx.registry.LiveVoxelsOf(4), walls[1].stats.planned + walls[2].stats.planned + walls[3].stats.planned);
    EXPECT_FALSE(yard.ctx.structure.queue.empty()) << "the crumble queued its site checks";
}

TEST(Magic, CaptureChecksTheGapAndOpensEveryElement) {
    sim::CastContext ctx;
    const sim::SpellTable table = sim::BuildSpellTable(kVoxel);
    sim::SpellCast c;
    c.player = 3;
    c.spell = sim::kPunch;
    EXPECT_EQ(sim::CheckCapture(ctx, table, c, 10), sim::SpellRefusal::None) << "Metal and Pressure by default";
    c.spell = 16;   // spread's Kindle: never built for play
    EXPECT_EQ(sim::CheckCapture(ctx, table, c, 10), sim::SpellRefusal::NotBuilt);
    sim::CasterState frost;
    frost.elementB = static_cast<uint8_t>(sim::Element::Frost);
    ctx.casters[3] = frost;
    c.spell = sim::kPunch;
    EXPECT_EQ(sim::CheckCapture(ctx, table, c, 10), sim::SpellRefusal::None) << "every element is open, attuned or not";
    c.spell = sim::kBarricadeWave;
    sim::TakeCapture(ctx, table, c, 10);
    EXPECT_EQ(sim::CheckCapture(ctx, table, c, 11), sim::SpellRefusal::TooSoon);
    c.spell = sim::kFlashFreeze;
    EXPECT_EQ(sim::CheckCapture(ctx, table, c, 21), sim::SpellRefusal::TooSoon) << "the gap is every form's";
    EXPECT_EQ(sim::CheckCapture(ctx, table, c, 22), sim::SpellRefusal::None) << "12 ticks";
}

// --- work and determinism -----------------------------------------------------------------------

namespace {
    /// The four spells cast at fixed places; the ops of the casts and of their follow-ups to T_run + 40.
    struct FourCasts {
        std::vector<sim::CastSpellResult> casts;
        std::vector<sim::WorldOp> ops;
        std::vector<uint64_t> followUpWork;
    };

    FourCasts CastTheFour(MagicYard& yard) {
        FourCasts out;
        yard.World().FillBox({16, kG, -40}, {18, kG + 24, -24}, yard.Ids().planks);
        const glm::dvec3 fist(0.0625, kGround + kEye, -4.0);
        const glm::dvec3 dash(-6.0, kGround + kEye, 6.0);
        out.casts.push_back(yard.Cast(sim::kBarricadeWave, 0, 1, {-3.0, kGround + 2.0, 0.0625}, GroundAt(0, 0)));
        out.casts.push_back(yard.Cast(sim::kUpdraft, 0, 1, {4.0, kGround + 2.0, 4.0}, GroundAt(48, 32)));
        out.casts.push_back(yard.Cast(sim::kDash, 0, 1, dash, dash + glm::dvec3(1.0, 0.0, 0.0)));
        out.casts.push_back(yard.Cast(sim::kPunch, 0, 1, fist, fist + glm::dvec3(1.0, 0.0, 0.0)));
        for (const sim::CastSpellResult& c : out.casts)
            for (const sim::BlastResult& g : c.groups)
                for (const sim::AppliedOp& a : g.ops) out.ops.push_back(a.op);
        const uint32_t t0 = yard.simulation.Tick();
        for (uint32_t t = t0; t <= t0 + 40; ++t) {
            const sim::ScheduledResult r = sim::RunScheduled(yard.simulation, yard.table, yard.ctx, t, {});
            for (const sim::BlastResult& g : r.groups) {
                out.followUpWork.push_back(sim::CastWorkOf(g).Total());
                for (const sim::AppliedOp& a : g.ops) out.ops.push_back(a.op);
            }
        }
        return out;
    }
}

TEST(CastBudget, SpellWorkIsEqualColdAndWarm) {
    MagicYard cold, warm;
    ASSERT_TRUE(cold.initialized);
    ASSERT_TRUE(warm.initialized);
    // The warm yard's graph cache already holds the chunks the casts read (a site check changes no voxel).
    sim::BlastOptions noCarve;
    noCarve.noCarve = true;
    for (int x = -1; x <= 1; ++x)
        for (int z = -1; z <= 0; ++z) sim::Blast(warm.simulation, sim::ChunkSite(warm.World(), {x, 2, z}), noCarve);
    ASSERT_EQ(cold.Hash(), warm.Hash());
    const FourCasts a = CastTheFour(cold);
    const FourCasts b = CastTheFour(warm);
    ASSERT_EQ(a.casts.size(), b.casts.size());
    for (size_t i = 0; i < a.casts.size(); ++i) {
        EXPECT_EQ(a.casts[i].refusal, sim::SpellRefusal::None) << i;
        EXPECT_GT(sim::CastWorkOf(a.casts[i]).Total(), 0u) << i;
        EXPECT_EQ(sim::CastWorkOf(a.casts[i]).Total(), sim::CastWorkOf(b.casts[i]).Total()) << i;
        EXPECT_EQ(a.casts[i].stats.chunksRead, b.casts[i].stats.chunksRead) << i;
        std::printf("spell %u: work %llu (search %llu, ops %llu), %u B\n", static_cast<unsigned>(i),
                    static_cast<unsigned long long>(a.casts[i].work.Total()), static_cast<unsigned long long>(a.casts[i].work.search),
                    static_cast<unsigned long long>(a.casts[i].work.ops), a.casts[i].stats.bytes);
    }
    EXPECT_EQ(a.followUpWork, b.followUpWork);
    EXPECT_EQ(a.ops, b.ops);
    EXPECT_EQ(cold.Hash(), warm.Hash());
}

TEST(Magic, OfflineAndServerEmitTheSameOps) {
    MagicYard offline(false), server(true);
    ASSERT_TRUE(offline.initialized);
    ASSERT_TRUE(server.initialized);
    const FourCasts a = CastTheFour(offline);
    const FourCasts b = CastTheFour(server);
    EXPECT_GE(a.ops.size(), 5u) << "four bands and the blow";
    EXPECT_EQ(a.ops, b.ops);
    for (size_t i = 0; i < a.casts.size(); ++i) EXPECT_EQ(a.casts[i].zonesStarted, b.casts[i].zonesStarted) << i;
    EXPECT_EQ(offline.Hash(), server.Hash());
}

// --- Meteor Leap (magic.md spell 9) --------------------------------------------------------------

// The Spell edge's step launches the caster along the look a client solved for the target (sim::LeapAimToward); the
// cast that follows traces the same arc from the eye after that step: the caster lands at the target, and the slam
// starts where and when it lands. On flat ground 9.5 m away, and on a block 1.5 m high 8 m away.
TEST(Magic, MeteorLeapLandsAtTheTarget) {
    struct Case {
        glm::dvec3 target;
        bool block;
    };
    for (const Case& c : {Case{{4.0, kGround, 0.5}, false}, Case{{2.5, kGround + 1.5, 0.5}, true}}) {
        MagicYard yard;
        ASSERT_TRUE(yard.initialized);
        if (c.block) yard.World().FillBox({16, kG, -16}, {32, kG + 12, 16}, yard.Ids().stone);   // x 2..4 m, its top at 13.5 m
        Fighter& caster = yard.Add(1, 1, {-5.5f, kGround, 0.5f});
        yard.Settle();
        ASSERT_TRUE(caster.state.OnGround());
        sim::PlayerInput in;
        sim::AimAngles(sim::LeapAimToward(caster.state.position, c.target), in.yaw, in.pitch);
        in.buttons = sim::PlayerButton_Spell;
        in.spell = sim::kMeteorLeap;
        const uint32_t edge = yard.simulation.Tick();
        sim::CastSpellResult cast;
        yard.TickWith(&caster, in, [&] {
            EXPECT_NE(caster.state.status & sim::PlayerStatus_Leaping, 0) << "launched in the edge's step";
            sim::SpellCast sc;
            sc.player = 1;
            sc.team = 1;
            sc.spell = sim::kMeteorLeap;
            sc.inputTick = edge;
            sc.eye = glm::dvec3(caster.state.position) + glm::dvec3(0.0, kEye, 0.0);
            sc.dir = sim::LookDirection(in.yaw, in.pitch);
            const std::vector<sim::SpellTarget> targets = yard.Targets();
            cast = sim::CastSpell(yard.simulation, yard.table, sc, yard.ctx, targets);
        });
        ASSERT_EQ(cast.refusal, sim::SpellRefusal::None) << sim::ToString(cast.refusal);
        EXPECT_EQ(cast.manaCost, 45u * 60u);
        EXPECT_EQ(cast.flightTicks, 0u) << "the caster is the projectile";
        ASSERT_EQ(cast.zonesStarted.size(), 2u);
        EXPECT_EQ(cast.zonesStarted[0].kind, sim::ZoneKind::Impulse);
        EXPECT_EQ(cast.zonesStarted[1].kind, sim::ZoneKind::Pin);
        uint32_t landTick = 0;
        for (int i = 0; i < 240 && landTick == 0; ++i) {
            const uint32_t t = yard.simulation.Tick();
            yard.Tick();
            if ((caster.state.status & sim::PlayerStatus_Leaping) == 0) landTick = t;
        }
        ASSERT_NE(landTick, 0u) << "the leap landed";
        EXPECT_TRUE(caster.state.OnGround());
        EXPECT_EQ(caster.state.push[0], 0) << "the landing ends the leap's push";
        EXPECT_EQ(caster.state.status & sim::PlayerStatus_Lifted, 0);
        const glm::dvec3 feet(caster.state.position);
        EXPECT_NEAR(feet.y, c.target.y, 0.05);
        EXPECT_LT(glm::length(glm::dvec2(feet.x - c.target.x, feet.z - c.target.z)), 1.0) << "lands at the target";
        const glm::dvec3 slam = glm::dvec3(cast.hitQ) * (kVoxel / 16.0);
        EXPECT_LT(glm::length(glm::dvec2(slam.x - feet.x, slam.z - feet.z)), 0.75) << "the slam is where it lands";
        EXPECT_NEAR(slam.y, feet.y, 0.05);
        const uint32_t slamTick = cast.zonesStarted[1].startTick;
        EXPECT_GE(slamTick, edge + yard.table.zoneLeadTicks);
        EXPECT_LE(slamTick > landTick ? slamTick - landTick : landTick - slamTick, 3u) << "the slam starts when it lands";
        std::printf("leap to (%.2f, %.2f): landed at (%.2f, %.2f, %.2f) %u ticks after the edge, slam at %u, %u arc ticks\n", c.target.x,
                    c.target.z, feet.x, feet.y, feet.z, landTick - edge, slamTick - edge, cast.stats.arcTicks);
    }
}

// The slam: at the landing an enemy is knocked back, pinned and takes 15; an ally is not; exposed ice and rimed
// stone in reach shatter.
TEST(Magic, MeteorLeapSlamPinsAndShatters) {
    MagicYard yard;
    ASSERT_TRUE(yard.initialized);
    // Ice and rimed stone beside the landing at x 4 m.
    yard.World().FillBox({36, kG, -4}, {40, kG + 4, 0}, yard.Ids().ice);          // x 4.5..5 m
    yard.World().FillBox({24, kG, 8}, {28, kG + 4, 12}, yard.Ids().stoneRimed);   // x 3..3.5 m, z 1..1.5 m
    Fighter& caster = yard.Add(1, 1, {-5.5f, kGround, 0.5f});
    Fighter& enemy = yard.Add(3, 2, {4.0f, kGround, -1.0f});
    Fighter& ally = yard.Add(2, 1, {5.4f, kGround, 1.8f});
    yard.Settle();
    const uint32_t iceBefore = yard.Count({36, kG, -4}, {40, kG + 4, 0}, yard.Ids().ice);
    const uint32_t rimedBefore = yard.Count({24, kG, 8}, {28, kG + 4, 12}, yard.Ids().stoneRimed);
    const glm::dvec3 eye = glm::dvec3(caster.state.position) + glm::dvec3(0.0, kEye, 0.0);
    const glm::vec3 aim = sim::LeapAimToward(caster.state.position, {4.0, kGround, 0.5});
    const sim::CastSpellResult cast = yard.Cast(sim::kMeteorLeap, 1, 1, eye, eye + glm::dvec3(aim), &caster.state);
    ASSERT_EQ(cast.refusal, sim::SpellRefusal::None) << sim::ToString(cast.refusal);
    EXPECT_TRUE(cast.dashStarted) << "the script's caster leaps from its state";
    EXPECT_NE(caster.state.status & sim::PlayerStatus_Lifted, 0);
    ASSERT_EQ(cast.zonesStarted.size(), 2u);
    EXPECT_TRUE(yard.ctx.zoneOwners.at(cast.zonesStarted[1].zoneId).dispellable) << "a Punch ends a Pin";
    const uint32_t slam = cast.zonesStarted[1].startTick;
    std::printf("slam: tick %u (now %u), at (%.2f, %.2f, %.2f), %u arc ticks\n", slam, yard.simulation.Tick(), cast.hitQ.x / 128.0, cast.hitQ.y / 128.0, cast.hitQ.z / 128.0, cast.stats.arcTicks);
    uint32_t enemyDamage = 0, allyDamage = 0;
    while (yard.simulation.Tick() <= slam + 2) {
        const sim::ScheduledResult r = yard.Tick();
        for (const sim::SpellDamage& d : r.damage) {
            if (d.player == enemy.player) enemyDamage += d.amount;
            if (d.player == ally.player) allyDamage += d.amount;
        }
    }
    EXPECT_EQ(enemyDamage, 15u);
    EXPECT_EQ(allyDamage, 0u);
    EXPECT_GT(enemy.state.pinLeft, 0) << "pinned";
    EXPECT_NE(enemy.state.status & sim::PlayerStatus_Pinned, 0);
    EXPECT_EQ(ally.state.pinLeft, 0);
    EXPECT_LT(enemy.state.position.z, -1.0f) << "knocked back away from the landing";
    EXPECT_LT(yard.Count({36, kG, -4}, {40, kG + 4, 0}, yard.Ids().ice), iceBefore) << "the exposed ice shattered";
    EXPECT_LT(yard.Count({24, kG, 8}, {28, kG + 4, 12}, yard.Ids().stoneRimed), rimedBefore) << "the exposed rime shattered";
    // The pin's tail: pinned while the zone lives, then 30 ticks more, then the control guard.
    for (int i = 0; i < 100; ++i) yard.Tick();
    EXPECT_EQ(enemy.state.pinLeft, 0);
    EXPECT_GT(enemy.state.ccGuardLeft, 0);
}
