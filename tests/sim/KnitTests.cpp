// Conjured matter in sim (docs/design/magic.md "The absorb rule, ablative", "Conjured matter",
// "Expiry and structure"): a blast on fresh knit chips it instead of carving; the registry expires
// exactly its own voxels; an expiry queues site checks that run on the structure credit; a
// conjured piece never becomes a server body.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.scene;
import vortex.sim;

using namespace vortex;

namespace {
    constexpr float kVoxel = 0.125f;
    constexpr int g = 64;                             // flat ground: voxels y < 64, its top at 8 m
    const glm::dvec3 kFocus(0.0, 8.0, 0.0);           // window chunks (-3..2, -1..4, -3..2)

    sim::SimulationConfig YardConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 512;
        config.maxParticles = 1024;
        config.jobThreads = 2;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = {6, 6, 6};
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        return config;
    }

    struct KnitYard {
        KnitYard() : simulation(YardConfig()) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(g, ids.grass, ids.stone));
            if (initialized) simulation.World().LoadBlocking(kFocus);
        }
        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        const voxel::MaterialLibrary& Materials() { return simulation.Materials(); }
        void Box(const glm::ivec3& min, const glm::ivec3& max, uint8_t id) {
            ASSERT_TRUE(sim::ApplyOp(World(), sim::WorldOp::Box(min, max, id)).Complete());
        }
        /// Applies `op` for `owner` and records what it changed.
        sim::OpResult Conjure(sim::KnitRegistry& registry, uint32_t owner, const sim::WorldOp& op) {
            std::vector<voxel::VoxelEdit> edits;
            const sim::OpResult r = sim::ApplyOp(World(), op, &edits);
            EXPECT_TRUE(r.Complete());
            registry.Record(owner, edits, Materials());
            return r;
        }
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
        bool initialized = false;
    };

    voxel::ShapeQ BoxShape(const glm::ivec3& min, const glm::ivec3& max) {
        voxel::ShapeQ s;
        s.kind = voxel::kShapeBox;
        s.a = min;
        s.b = max;
        return s;
    }

    sim::KnitOwnerInfo Conjuring(uint8_t caster, uint32_t dueTick) {
        sim::KnitOwnerInfo info;
        info.layer = 0;
        info.expiryMap = voxel::Map_Unknit;
        info.caster = caster;
        info.dueTick = dueTick;
        return info;
    }

    sim::KnitOwnerInfo ShapedConjuring(uint8_t caster, uint32_t dueTick, const voxel::ShapeQ& shape) {
        sim::KnitOwnerInfo info = Conjuring(caster, dueTick);
        info.hasShape = true;
        info.shape = shape;
        return info;
    }

    // The plate: knit x 10..12 (3 voxels) in front of a stone block x 13..19, y g..g+15, z -8..7.
    const glm::ivec3 kPlateMin(10, g, -8), kPlateMax(13, g + 16, 8);
    const glm::ivec3 kStoneMin(13, g, -8), kStoneMax(20, g + 16, 8);
    constexpr int kHitY = g + 8, kHitZ = 0;
    /// A 1 m blast of power 6 (under knit's 7, over stone's 3) centred on the face x = `faceX`.
    voxel::SphereQ BlastOnFace(int faceX) { return voxel::SphereQ{glm::ivec3(faceX * 16, kHitY * 16 + 8, kHitZ * 16 + 8), 8 * 16}; }
    constexpr uint16_t kPower6 = 96;

    /// The first solid voxel along +x on the hit row, from x = 0.
    int FirstSolidX(voxel::World& world) {
        for (int x = 0; x < 40; ++x)
            if (world.GetVoxel({x, kHitY, kHitZ}) != 0) return x;
        return -1;
    }
}

// --- the absorb rule ------------------------------------------------------------------------------

TEST(Magic, ABlastOnFreshKnitIsAbsorbed) {
    KnitYard yard;
    ASSERT_TRUE(yard.initialized);
    yard.Box(kStoneMin, kStoneMax, yard.Ids().stone);
    yard.Box(kPlateMin, kPlateMax, yard.Ids().knitIron);
    const uint32_t stone = yard.Count(kStoneMin, kStoneMax, yard.Ids().stone);
    ASSERT_EQ(stone, 7u * 16u * 16u);

    EXPECT_TRUE(sim::AbsorbsBlast(yard.Materials(), yard.Ids().knitIron, kPower6));
    EXPECT_FALSE(sim::AbsorbsBlast(yard.Materials(), yard.Ids().knitIron, 112));   // hardness 7 x 16: carved
    EXPECT_FALSE(sim::AbsorbsBlast(yard.Materials(), yard.Ids().stone, kPower6));
    EXPECT_FALSE(sim::AbsorbsBlast(yard.Materials(), 0, kPower6));

    const sim::BlastResult r = sim::BlastAt(yard.simulation, BlastOnFace(10), kPower6, yard.Ids().knitIron);
    EXPECT_TRUE(r.absorbed);
    EXPECT_TRUE(r.islandsSearched);
    ASSERT_FALSE(r.ops.empty());
    EXPECT_EQ(r.ops[0].op.kind, sim::WorldOpKind::MapShape);
    EXPECT_EQ(r.ops[0].op.map, voxel::Map_Ablate);
    EXPECT_EQ(r.ops[0].op.shape.radiusQ, sim::BuildSpellTable(kVoxel).ablateRadiusQ);
    EXPECT_GT(r.ops[0].result.changed, 0u);
    for (const sim::AppliedOp& applied : r.ops) EXPECT_NE(applied.op.kind, sim::WorldOpKind::CarveSphere);
    EXPECT_EQ(yard.Count(kStoneMin, kStoneMax, yard.Ids().stone), stone);
    EXPECT_GT(r.radius, 0.9f);   // the objects and the push still reach the blast's radius
}

TEST(Magic, AnAbsorbedBlastAblatesThePlate) {
    KnitYard yard;
    ASSERT_TRUE(yard.initialized);
    yard.Box(kStoneMin, kStoneMax, yard.Ids().stone);
    yard.Box(kPlateMin, kPlateMax, yard.Ids().knitIron);
    const uint8_t knit = yard.Ids().knitIron;

    const voxel::SphereQ first = BlastOnFace(10);
    ASSERT_EQ(FirstSolidX(yard.World()), 10);
    const sim::BlastResult r1 = sim::BlastAt(yard.simulation, first, kPower6, yard.World().GetVoxel({10, kHitY, kHitZ}));
    ASSERT_TRUE(r1.absorbed);
    voxel::ShapeQ patch;
    patch.kind = voxel::kShapeSphere;
    patch.a = first.center;
    patch.radiusQ = 32;
    uint32_t ablated = 0;
    for (int z = kPlateMin.z; z < kPlateMax.z; ++z)
        for (int y = kPlateMin.y; y < kPlateMax.y; ++y)
            for (int x = kPlateMin.x; x < kPlateMax.x; ++x) {
                const bool inPatch = voxel::InShapeQ(patch, {x, y, z});
                EXPECT_EQ(yard.World().GetVoxel({x, y, z}), inPatch ? 0 : knit) << x << " " << y << " " << z;
                ablated += inPatch ? 1u : 0u;
            }
    EXPECT_EQ(r1.ops[0].result.changed, ablated);
    std::printf("an absorbed blast ablated %u knit voxels\n", ablated);

    // The next blast on the spot hits the knit left behind: absorbed again.
    const int second = FirstSolidX(yard.World());
    ASSERT_EQ(second, 12);
    const sim::BlastResult r2 = sim::BlastAt(yard.simulation, BlastOnFace(second), kPower6, yard.World().GetVoxel({second, kHitY, kHitZ}));
    EXPECT_TRUE(r2.absorbed);
    EXPECT_EQ(yard.World().GetVoxel({12, kHitY, kHitZ}), 0);
    EXPECT_EQ(yard.World().GetVoxel({13, kHitY, kHitZ}), yard.Ids().stone);

    // Then the stone: an ordinary carving blast.
    const int third = FirstSolidX(yard.World());
    ASSERT_EQ(third, 13);
    const sim::BlastResult r3 = sim::BlastAt(yard.simulation, BlastOnFace(third), kPower6, yard.World().GetVoxel({third, kHitY, kHitZ}));
    EXPECT_FALSE(r3.absorbed);
    ASSERT_FALSE(r3.ops.empty());
    EXPECT_EQ(r3.ops[0].op.kind, sim::WorldOpKind::CarveSphere);
    EXPECT_EQ(yard.World().GetVoxel({13, kHitY, kHitZ}), 0);
}

// --- the registry ---------------------------------------------------------------------------------

TEST(KnitRegistry, ExpiryRemovesOnlyItsOwnMatter) {
    KnitYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint8_t knit = yard.Ids().knitIron;
    const glm::ivec3 wallMin(0, g, 0), wallMax(10, g + 6, 3);
    sim::KnitRegistry registry;
    const uint32_t wall = registry.NewOwner(ShapedConjuring(1, 100, BoxShape(wallMin, wallMax)));
    EXPECT_EQ(wall, 1u);
    EXPECT_EQ(yard.Conjure(registry, wall, sim::WorldOp::Map(voxel::Map_Knit, BoxShape(wallMin, wallMax))).changed, 180u);
    EXPECT_EQ(registry.LiveVoxelsOf(1), 180u);

    // A hole through the wall, then another caster's plug in it.
    std::vector<voxel::VoxelEdit> hole;
    sim::ApplyOp(yard.World(), sim::WorldOp::Carve(voxel::SphereQ{glm::ivec3(5 * 16 + 8, (g + 3) * 16 + 8, 1 * 16 + 8), 24}, 1600), &hole);
    ASSERT_GT(hole.size(), 4u);
    std::vector<glm::ivec3> plug;
    for (const voxel::VoxelEdit& e : hole) plug.push_back(e.voxel);
    const uint32_t patch = registry.NewOwner(Conjuring(2, 200));
    EXPECT_EQ(yard.Conjure(registry, patch, sim::MapCellsOp(voxel::Map_Knit, plug)).changed, plug.size());
    EXPECT_EQ(registry.LiveVoxelsOf(1), 180u - plug.size());
    EXPECT_EQ(registry.LiveVoxelsOf(2), plug.size());

    EXPECT_TRUE(registry.RunExpiries(yard.simulation, 99).empty());
    const std::vector<sim::AppliedOp> first = registry.RunExpiries(yard.simulation, 100);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].op.kind, sim::WorldOpKind::MapMask);   // the boxes met: the mask form
    EXPECT_EQ(first[0].result.changed, 180u - plug.size());
    EXPECT_EQ(yard.Count(wallMin, wallMax, knit), plug.size());
    for (const glm::ivec3& v : plug) EXPECT_EQ(yard.World().GetVoxel(v), knit);
    EXPECT_EQ(registry.Owners(), 1u);

    const std::vector<sim::AppliedOp> second = registry.RunExpiries(yard.simulation, 200);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].op.kind, sim::WorldOpKind::MapMask);
    EXPECT_EQ(second[0].result.changed, plug.size());
    EXPECT_EQ(yard.Count(wallMin - glm::ivec3(2), wallMax + glm::ivec3(2), knit), 0u);
    EXPECT_EQ(registry.Owners(), 0u);
    EXPECT_EQ(registry.LiveVoxels(0), 0u);
}

TEST(KnitRegistry, ShapeExpiryOnlyWithoutOverlap) {
    KnitYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint8_t knit = yard.Ids().knitIron;
    sim::KnitRegistry registry;
    // Alone, from one MapShape: its shape.
    const voxel::ShapeQ shape = BoxShape({-40, g, -40}, {-30, g + 12, -37});
    const uint32_t a = registry.NewOwner(ShapedConjuring(1, 10, shape));
    yard.Conjure(registry, a, sim::WorldOp::Map(voxel::Map_Knit, shape));
    // Alone, from a mask: its box.
    std::vector<glm::ivec3> cells;
    for (int x = 30; x < 40; ++x)
        for (int y = g; y < g + 1 + (x - 30); ++y) cells.push_back({x, y, 30});
    const uint32_t b = registry.NewOwner(Conjuring(1, 10));
    yard.Conjure(registry, b, sim::MapCellsOp(voxel::Map_Knit, cells));
    // Two whose boxes meet: masks.
    const uint32_t c = registry.NewOwner(Conjuring(2, 10));
    yard.Conjure(registry, c, sim::WorldOp::Map(voxel::Map_Knit, BoxShape({0, g, 0}, {6, g + 6, 2})));
    const uint32_t d = registry.NewOwner(Conjuring(3, 10));
    yard.Conjure(registry, d, sim::WorldOp::Map(voxel::Map_Knit, BoxShape({5, g, 1}, {12, g + 6, 3})));

    const std::vector<sim::AppliedOp> ops = registry.RunExpiries(yard.simulation, 10);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0].op, sim::WorldOp::Map(voxel::Map_Unknit, shape));
    EXPECT_EQ(ops[1].op.kind, sim::WorldOpKind::MapShape);
    EXPECT_EQ(ops[1].op.shape.kind, voxel::kShapeBox);
    EXPECT_EQ(ops[1].op.shape.a, glm::ivec3(30, g, 30));
    EXPECT_EQ(ops[1].op.shape.b, glm::ivec3(40, g + 10, 31));
    EXPECT_EQ(ops[1].result.changed, cells.size());
    for (int i = 0; i < 2; ++i) {
        const size_t bytes = sim::OpWireBytes(ops[static_cast<size_t>(i)].op);
        EXPECT_GE(bytes, 17u);
        EXPECT_LE(bytes, 29u);
    }
    EXPECT_EQ(ops[2].op.kind, sim::WorldOpKind::MapMask);
    EXPECT_EQ(ops[3].op.kind, sim::WorldOpKind::MapMask);
    EXPECT_EQ(yard.Count({-50, g, -50}, {50, g + 20, 50}, knit), 0u);
}

namespace {
    /// The same script of conjurings, a carve and expiries on any yard: the ops it expired.
    std::vector<sim::WorldOp> RegistryScript(KnitYard& yard) {
        sim::KnitRegistry registry;
        std::vector<sim::WorldOp> out;
        const auto take = [&out](const std::vector<sim::AppliedOp>& applied) {
            for (const sim::AppliedOp& a : applied) out.push_back(a.op);
        };
        const voxel::ShapeQ wall = BoxShape({-20, g, -4}, {20, g + 12, -1});
        const uint32_t a = registry.NewOwner(ShapedConjuring(1, 30, wall));
        yard.Conjure(registry, a, sim::WorldOp::Map(voxel::Map_Knit, wall));
        std::vector<glm::ivec3> stair;
        for (int i = 0; i < 12; ++i)
            for (int x = -3; x <= 3; ++x) stair.push_back({x, g + i, -6 + i});
        const uint32_t b = registry.NewOwner(Conjuring(2, 30));   // crosses the wall: some cells are knit already
        yard.Conjure(registry, b, sim::MapCellsOp(voxel::Map_Knit, stair));
        sim::ApplyOp(yard.World(), sim::WorldOp::Carve(voxel::SphereQ{glm::ivec3(-10 * 16, (g + 6) * 16, -2 * 16), 40}, 1600));
        const uint32_t c = registry.NewOwner(ShapedConjuring(1, 20, BoxShape({30, g, 30}, {34, g + 4, 34})));
        yard.Conjure(registry, c, sim::WorldOp::Map(voxel::Map_Knit, BoxShape({30, g, 30}, {34, g + 4, 34})));
        take(registry.RunExpiries(yard.simulation, 20));
        take(registry.MakeRoom(yard.simulation, 1, sim::kMaxConjuredPerPlayer));
        take(registry.RunExpiries(yard.simulation, 30));
        return out;
    }
}

TEST(KnitRegistry, ReplayGivesTheSameOps) {
    KnitYard one, two;
    ASSERT_TRUE(one.initialized && two.initialized);
    const std::vector<sim::WorldOp> a = RegistryScript(one);
    const std::vector<sim::WorldOp> b = RegistryScript(two);
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(a, b);
    EXPECT_EQ(one.Hash(), two.Hash());
    // Applying the same expiry ops to a third world gives the same voxels again.
    KnitYard three;
    ASSERT_TRUE(three.initialized);
    const voxel::ShapeQ wall = BoxShape({-20, g, -4}, {20, g + 12, -1});
    sim::ApplyOp(three.World(), sim::WorldOp::Map(voxel::Map_Knit, wall));
    std::vector<glm::ivec3> stair;
    for (int i = 0; i < 12; ++i)
        for (int x = -3; x <= 3; ++x) stair.push_back({x, g + i, -6 + i});
    sim::ApplyOp(three.World(), sim::MapCellsOp(voxel::Map_Knit, stair));
    sim::ApplyOp(three.World(), sim::WorldOp::Carve(voxel::SphereQ{glm::ivec3(-10 * 16, (g + 6) * 16, -2 * 16), 40}, 1600));
    sim::ApplyOp(three.World(), sim::WorldOp::Map(voxel::Map_Knit, BoxShape({30, g, 30}, {34, g + 4, 34})));
    for (const sim::WorldOp& op : a) sim::ApplyOp(three.World(), op);
    EXPECT_EQ(three.Hash(), one.Hash());
}

TEST(KnitRegistry, PlayerCapExpiresTheOldestFirst) {
    KnitYard yard;
    ASSERT_TRUE(yard.initialized);
    sim::KnitRegistry registry;
    std::vector<uint32_t> owners;
    for (int i = 0; i < 4; ++i) {
        const glm::ivec3 min(-80 + 40 * i, g, 40), max(-55 + 40 * i, g + 6, 50);   // 25 x 6 x 10 = 1 500 voxels
        const uint32_t id = registry.NewOwner(ShapedConjuring(3, 1000, BoxShape(min, max)));
        EXPECT_EQ(yard.Conjure(registry, id, sim::WorldOp::Map(voxel::Map_Knit, BoxShape(min, max))).changed, 1500u);
        owners.push_back(id);
    }
    const uint32_t other = registry.NewOwner(ShapedConjuring(4, 1000, BoxShape({0, g, 0}, {5, g + 5, 5})));
    yard.Conjure(registry, other, sim::WorldOp::Map(voxel::Map_Knit, BoxShape({0, g, 0}, {5, g + 5, 5})));
    ASSERT_EQ(registry.LiveVoxelsOf(3), 6000u);

    EXPECT_TRUE(registry.MakeRoom(yard.simulation, 3, 0).empty());
    const std::vector<sim::AppliedOp> ops = registry.MakeRoom(yard.simulation, 3, 1500);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].result.changed, 1500u);
    EXPECT_EQ(registry.LiveVoxelsOf(3), 4500u);
    EXPECT_EQ(registry.LiveVoxelsOf(4), 125u);
    EXPECT_EQ(registry.Owners(), 4u);
    EXPECT_EQ(yard.Count({-80, g, 40}, {-55, g + 6, 50}, yard.Ids().knitIron), 0u);    // the first wall
    EXPECT_EQ(yard.Count({-40, g, 40}, {-15, g + 6, 50}, yard.Ids().knitIron), 1500u); // the second stays
}

// --- site checks and the structure credit ------------------------------------------------------------

TEST(KnitRegistry, FillRunsNoLoadCheckAndExpiryRunsASiteCheck) {
    KnitYard yard;
    ASSERT_TRUE(yard.initialized);
    const uint8_t stone = yard.Ids().stone;
    // A stone slab 3 x 1 x 3 m on a 0.25 m square stone neck (it alone cannot hold it) and a knit beam.
    yard.Box({2, g, 2}, {4, g + 8, 4}, stone);
    yard.Box({0, g + 8, 0}, {24, g + 16, 24}, stone);
    const voxel::ShapeQ beam = BoxShape({16, g, 16}, {22, g + 8, 22});
    sim::KnitRegistry registry;
    sim::StructureCredit credit;
    const uint32_t owner = registry.NewOwner(ShapedConjuring(1, 10, beam));
    std::vector<voxel::VoxelEdit> fill;
    ASSERT_TRUE(sim::ApplyOp(yard.World(), sim::WorldOp::Map(voxel::Map_Knit, beam), &fill).Complete());
    registry.Record(owner, fill, yard.Materials());
    credit.QueueFromEdits(fill, yard.Materials());
    EXPECT_TRUE(credit.queue.empty()) << "a knit fill runs no load check";

    const std::vector<sim::AppliedOp> expiry = registry.RunExpiries(yard.simulation, 10);
    ASSERT_EQ(expiry.size(), 1u);
    credit.QueueFromEdits(expiry[0].edits, yard.Materials());
    ASSERT_EQ(credit.queue.size(), 1u);
    EXPECT_EQ(credit.queue.front(), glm::ivec3(0, 2, 0));

    std::vector<sim::BlastResult> out;
    bool released = false;
    for (int tick = 0; tick < 3 && !released; ++tick) {
        credit.Run(yard.simulation, out);
        for (const sim::BlastResult& r : out)
            for (const sim::AppliedOp& a : r.ops) released = released || a.op.kind == sim::WorldOpKind::Detach;
    }
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(out[0].search.neckRan);
    EXPECT_TRUE(released);
    for (const sim::BlastResult& r : out)
        for (const sim::AppliedOp& a : r.ops) EXPECT_NE(a.op.kind, sim::WorldOpKind::CarveSphere);
    EXPECT_EQ(yard.World().GetVoxel({12, g + 12, 12}), 0);   // the slab fell
}

TEST(StructureCredit, QueueIsBoundedAndDeduplicated) {
    sim::StructureCredit credit;
    for (int i = 0; i < 300; ++i) credit.QueueSite({i, 0, -i});
    EXPECT_EQ(credit.queue.size(), sim::StructureCredit::kMaxQueued);
    EXPECT_EQ(credit.dropped, 44u);
    EXPECT_EQ(credit.queue.front(), glm::ivec3(0, 0, 0));
    EXPECT_EQ(credit.queue.back(), glm::ivec3(255, 0, -255));
    credit.QueueSite({7, 0, -7});
    EXPECT_EQ(credit.queue.size(), sim::StructureCredit::kMaxQueued);
    EXPECT_EQ(credit.dropped, 44u);

    sim::StructureCredit small;
    small.QueueSite({1, 2, 3});
    small.QueueSite({1, 2, 3});
    small.QueueSite({1, 2, 4});
    EXPECT_EQ(small.queue.size(), 2u);
    EXPECT_EQ(small.dropped, 0u);
}

TEST(StructureCredit, CreditRefillsAndCarriesDebt) {
    KnitYard yard;
    ASSERT_TRUE(yard.initialized);
    sim::StructureCredit credit;
    for (int i = 0; i < 6; ++i) credit.QueueSite({i % 3 - 1, 2, i / 3 - 1});   // island boxes inside the window

    // Tick 1: the refill to 15 000; sites run while the credit is above 0, each charged its work.
    std::vector<sim::BlastResult> out;
    const uint32_t ran = credit.Run(yard.simulation, out);
    ASSERT_GE(ran, 1u);
    ASSERT_EQ(out.size(), ran);
    int64_t expected = sim::StructureCredit::kWorkPerTick;
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_GT(expected, 0) << "site " << i << " ran without credit";
        const int64_t work = static_cast<int64_t>(sim::CastWorkOf(out[i]).Total());
        EXPECT_GT(work, 0);
        std::printf("site check %zu: work %lld\n", i, static_cast<long long>(work));
        expected -= work;
    }
    EXPECT_EQ(credit.credit, expected);
    if (credit.queue.size() > 0) EXPECT_LE(credit.credit, 0);

    // A debt of 29 000 (a site of work 44 000): the next tick refills to -14 000 and runs none; the
    // tick after to 1 000 and runs one (each site here costs more than 1 000).
    credit.credit = -29000;
    out.clear();
    EXPECT_EQ(credit.Run(yard.simulation, out), 0u);
    EXPECT_EQ(credit.credit, -14000);
    const size_t queued = credit.queue.size();
    ASSERT_GE(queued, 1u);
    EXPECT_EQ(credit.Run(yard.simulation, out), 1u);
    EXPECT_EQ(credit.credit, 1000 - static_cast<int64_t>(sim::CastWorkOf(out[0]).Total()));
    EXPECT_EQ(credit.queue.size(), queued - 1);

    // The debt is bounded at six ticks of work, the credit at one.
    credit.credit = -1000000;
    credit.queue.clear();
    credit.Run(yard.simulation, out);
    EXPECT_EQ(credit.credit, -6 * sim::StructureCredit::kWorkPerTick);
    credit.credit = sim::StructureCredit::kWorkPerTick;
    credit.Run(yard.simulation, out);
    EXPECT_EQ(credit.credit, sim::StructureCredit::kWorkPerTick);
}

// --- conjured pieces ------------------------------------------------------------------------------

namespace {
    constexpr float kServerVoxel = 0.1f;             // replicated pieces: 512 voxels
    constexpr int kServerGround = 80;

    sim::SimulationConfig ServerYardConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.jobThreads = 2;
        config.world.voxelSize = kServerVoxel;
        config.world.windowSize = {8, 8, 8};
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        return sim::ServerConfig(config);
    }
}

TEST(Magic, ConjuredPiecesNeverTakeASlot) {
    sim::Simulation simulation(ServerYardConfig());
    voxel::MaterialLibrary scratch;
    const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
    ASSERT_TRUE(simulation.Initialize(voxel::CreateFlatGenerator(kServerGround, ids.grass, ids.stone)));
    simulation.World().LoadBlocking(glm::dvec3(0.0, 8.0, 0.0));
    const int gr = kServerGround;
    const auto mushroom = [&](int x, uint8_t cap) {
        ASSERT_TRUE(sim::ApplyOp(simulation.World(), sim::WorldOp::Box({x, gr, 0}, {x + 3, gr + 30, 3}, ids.stone)).Complete());
        ASSERT_TRUE(sim::ApplyOp(simulation.World(), sim::WorldOp::Box({x - 5, gr + 30, -5}, {x + 7, gr + 34, 7}, cap)).Complete());
    };
    const auto cut = [&](int x) {
        return sim::Blast(simulation, voxel::SphereQ{glm::ivec3((x + 1) * 16 + 8, (gr + 24) * 16 + 8, 16 + 8), 3 * 16}, 1600);
    };
    const auto detach = [](const sim::BlastResult& r) -> const sim::AppliedOp* {
        for (const sim::AppliedOp& a : r.ops)
            if (a.op.kind == sim::WorldOpKind::Detach) return &a;
        return nullptr;
    };
    mushroom(-30, ids.knitIron);
    mushroom(30, ids.stone);

    const sim::BlastResult knit = cut(-30);
    const sim::AppliedOp* knitPiece = detach(knit);
    ASSERT_NE(knitPiece, nullptr);
    EXPECT_GE(knitPiece->result.changed, 576u);   // over the replicated size
    EXPECT_EQ(knitPiece->op.netId, 0u);

    const sim::BlastResult stone = cut(30);
    const sim::AppliedOp* stonePiece = detach(stone);
    ASSERT_NE(stonePiece, nullptr);
    EXPECT_NE(stonePiece->op.netId, 0u);
    EXPECT_EQ(sim::CountPieces(simulation).live, 1u);
}
