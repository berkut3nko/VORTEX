// World operations and sim::Blast (docs/design/networking.md, stage 2): ApplyOp, the report of
// chunks an operation could not change, terrain islands as Detach operations, and the stage-2
// acceptance - two simulations fed the same operations end with equal world hashes.

#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.scene;
import vortex.sim;

using namespace vortex;
using scene::Entity;
using scene::kNoEntity;

namespace {
    constexpr float kVoxel = 0.1f;
    constexpr int kGroundVoxels = 80;                 // flat ground: its top is at y = 8 m
    const glm::dvec3 kFocus(0.0, 8.0, 0.0);           // window chunks (-3..2, -1..4, -3..2)

    sim::SimulationConfig ArenaConfig(unsigned jobThreads = 3) {
        sim::SimulationConfig config;
        config.chunkCapacity = 512;
        config.maxParticles = 4096;
        config.jobThreads = jobThreads;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = {6, 6, 6};
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        return config;
    }

    std::shared_ptr<voxel::IWorldGenerator> FlatGround() {
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
        return voxel::CreateFlatGenerator(kGroundVoxels, ids.grass, ids.stone);
    }

    /// One simulation on flat ground with its window loaded.
    struct Arena {
        explicit Arena(const sim::SimulationConfig& config = ArenaConfig()) : simulation(config) {
            initialized = simulation.Initialize(FlatGround());
            if (initialized) simulation.World().LoadBlocking(kFocus);
        }
        voxel::World& World() { return simulation.World(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        voxel::WorldHashResult Hash() const {
            const voxel::World& world = simulation.World();
            return voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize());
        }
        sim::Simulation simulation;
        bool initialized = false;
    };

    /// Two stone "mushrooms" on the ground: a 3 x 16 x 3 stem with an 11 x 3 x 11 cap each,
    /// 12 voxels apart. A blast between the stems at mid height cuts both: two islands.
    std::vector<sim::WorldOp> Mushrooms(const voxel::WorldMaterials& ids) {
        const int g = kGroundVoxels;
        return {
            sim::WorldOp::Box({0, g, 0}, {3, g + 16, 3}, ids.stone),
            sim::WorldOp::Box({-4, g + 16, -4}, {7, g + 19, 7}, ids.stone),
            sim::WorldOp::Box({12, g, 0}, {15, g + 16, 3}, ids.stone),
            sim::WorldOp::Box({9, g + 16, -4}, {20, g + 19, 7}, ids.planks),
            // A material swap on the second cap's top layer, so swaps are part of the history.
            sim::WorldOp::Swap(voxel::SphereQ{glm::ivec3(14 * 16 + 8, (g + 18) * 16 + 8, 16 + 8), 40}, ids.planks, ids.wood),
        };
    }

    /// The blast that cuts both stems: centre between them at stem mid height, 9 voxels, power 100.
    voxel::SphereQ StemCut() { return voxel::SphereQ{glm::ivec3(7 * 16 + 8, (kGroundVoxels + 8) * 16 + 8, 16 + 8), 9 * 16}; }
    constexpr uint16_t kStrong = 1600;

    void ApplyAll(voxel::World& world, std::span<const sim::WorldOp> ops) {
        for (const sim::WorldOp& op : ops) {
            const sim::OpResult r = sim::ApplyOp(world, op);
            ASSERT_TRUE(r.Complete());
        }
    }

    std::vector<sim::WorldOp> OpsOf(const sim::BlastResult& blast) {
        std::vector<sim::WorldOp> out;
        for (const sim::AppliedOp& applied : blast.ops) out.push_back(applied.op);
        return out;
    }
}

// --- ApplyOp -----------------------------------------------------------------------------------

TEST(WorldOps, ApplyOpIsTheWorldCallOfEachKind) {
    Arena a, b;
    ASSERT_TRUE(a.initialized && b.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    const voxel::SphereQ s{glm::ivec3(-40, kGroundVoxels * 16, 72), 70};

    std::vector<voxel::VoxelEdit> viaOp, direct;
    EXPECT_EQ(sim::ApplyOp(a.World(), sim::WorldOp::Carve(s, 32), &viaOp).changed, b.World().CarveSphereQ(s, 32, &direct));
    EXPECT_EQ(viaOp.size(), direct.size());
    EXPECT_GT(viaOp.size(), 0u);
    EXPECT_EQ(sim::ApplyOp(a.World(), sim::WorldOp::Fill(s, ids.brick)).changed, b.World().FillSphereQ(s, ids.brick));
    EXPECT_EQ(sim::ApplyOp(a.World(), sim::WorldOp::Swap(s, ids.brick, ids.gold)).changed, b.World().SwapSphereQ(s, ids.brick, ids.gold));
    EXPECT_EQ(sim::ApplyOp(a.World(), sim::WorldOp::Box({-3, 70, -3}, {4, 90, 2}, ids.iron)).changed,
              b.World().FillBox({-3, 70, -3}, {4, 90, 2}, ids.iron));
    voxel::VoxelMask mask({7, 20, 5});
    for (int y = 0; y < 20; y += 2) mask.Set(3, y, 2, true);
    EXPECT_EQ(sim::ApplyOp(a.World(), sim::WorldOp::Detach({-3, 70, -3}, mask, glm::vec3(0.0f))).changed,
              b.World().ClearMask({-3, 70, -3}, mask));
    voxel::VoxelMask crumbs({9, 3, 4});
    crumbs.Set(0, 0, 0, true);
    crumbs.Set(8, 2, 3, true);
    crumbs.Set(4, 1, 1, true);
    const uint32_t cleared = sim::ApplyOp(a.World(), sim::WorldOp::Clear({-5, 72, -4}, crumbs)).changed;
    EXPECT_EQ(cleared, b.World().ClearMask({-5, 72, -4}, crumbs));
    EXPECT_GT(cleared, 0u);
    EXPECT_EQ(a.Hash().hash, b.Hash().hash);

    // Bounds of each kind.
    glm::ivec3 mn, mx;
    sim::WorldOp::Box({1, 2, 3}, {4, 5, 6}, 1).Bounds(mn, mx);
    EXPECT_EQ(mn, glm::ivec3(1, 2, 3));
    EXPECT_EQ(mx, glm::ivec3(4, 5, 6));
    sim::WorldOp::Detach({-3, 70, -3}, mask, glm::vec3(0.0f)).Bounds(mn, mx);
    EXPECT_EQ(mx - mn, glm::ivec3(7, 20, 5));
    sim::WorldOp::Clear({-5, 72, -4}, crumbs).Bounds(mn, mx);
    EXPECT_EQ(mn, glm::ivec3(-5, 72, -4));
    EXPECT_EQ(mx, glm::ivec3(4, 75, 0));
    sim::WorldOp::Carve(voxel::SphereQ{glm::ivec3(8), 16}, 1).Bounds(mn, mx);
    EXPECT_EQ(mn, glm::ivec3(-1));
    EXPECT_EQ(mx, glm::ivec3(2));
}

TEST(WorldOps, OperationOnChunksThatAreNotLoadedIsReportedAsSkipped) {
    sim::Simulation simulation(ArenaConfig());
    ASSERT_TRUE(simulation.Initialize(FlatGround()));
    voxel::World& world = simulation.World();
    world.LoadBlocking(kFocus);
    const voxel::WorldMaterials& ids = simulation.WorldMaterialIds();

    // Outside the window (chunks x >= 3 are not part of it): nothing changes, the chunk is named.
    const sim::OpResult outside = sim::ApplyOp(world, sim::WorldOp::Box({100, 80, 0}, {104, 84, 4}, ids.stone));
    EXPECT_EQ(outside.changed, 0u);
    EXPECT_FALSE(outside.Complete());
    ASSERT_EQ(outside.report.skipped.size(), 1u);
    EXPECT_EQ(outside.report.skipped[0], glm::ivec3(3, 2, 0));

    // Window moved: the new chunks are Unloaded / Loading. A carve there is skipped as a whole.
    const glm::dvec3 far(401.65, 8.05, 1.65);                  // the middle of chunk (125, 2, 0)
    world.Update(far);
    const glm::ivec3 v = world.WorldToVoxel(far);
    const sim::OpResult loading = sim::ApplyOp(world, sim::WorldOp::Carve(voxel::SphereQ{v * 16 + glm::ivec3(8), 40}, kStrong));
    EXPECT_EQ(loading.changed, 0u);
    EXPECT_EQ(loading.report.skipped.size(), 1u);
    EXPECT_TRUE(loading.report.unsynced.empty());
    // Once the chunk is there the same operation applies (a client replays it).
    world.LoadBlocking(far);
    const sim::OpResult replay = sim::ApplyOp(world, sim::WorldOp::Carve(voxel::SphereQ{v * 16 + glm::ivec3(8), 40}, kStrong));
    EXPECT_TRUE(replay.Complete());
    EXPECT_GT(replay.changed, 0u);
}

TEST(WorldOps, DetachedPieceIsBuiltFromTheVoxelsItRemoved) {
    Arena a;
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    ApplyAll(a.World(), Mushrooms(ids));
    // The second cap: planks with a wood patch on top.
    const glm::ivec3 origin(9, kGroundVoxels + 16, -4);
    voxel::VoxelMask mask({11, 3, 11});
    for (int z = 0; z < 11; ++z)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 11; ++x) mask.Set(x, y, z, true);
    const sim::WorldOp op = sim::WorldOp::Detach(origin, mask, glm::vec3(0.0f, 1.0f, 0.0f));
    std::vector<voxel::VoxelEdit> removed;
    const sim::OpResult r = sim::ApplyOp(a.World(), op, &removed);
    EXPECT_EQ(r.changed, 11u * 3u * 11u);
    const voxel::VoxelVolume piece = sim::BuildDetachedPiece(op, removed);
    EXPECT_EQ(piece.Size(), glm::ivec3(11, 3, 11));
    EXPECT_EQ(piece.CountSolid(), r.changed);
    uint32_t wood = 0;
    for (const uint8_t id : piece.Data()) wood += id == ids.wood;
    EXPECT_GT(wood, 0u);
    EXPECT_EQ(piece.Get(0, 0, 0), ids.planks);
    for (int z = 0; z < 11; ++z)
        for (int x = 0; x < 11; ++x) EXPECT_EQ(a.World().GetVoxel(origin + glm::ivec3(x, 1, z)), 0);
}

// --- sim::Blast --------------------------------------------------------------------------------

TEST(Blast, IslandsBecomeDetachOperationsWithoutEntities) {
    Arena a;
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    ApplyAll(a.World(), Mushrooms(ids));
    const size_t objectsBefore = a.simulation.Scene().VoxelObjectCount();

    const sim::BlastResult blast = sim::Blast(a.simulation, StemCut(), kStrong);
    ASSERT_GE(blast.ops.size(), 3u);
    EXPECT_TRUE(blast.islandsSearched);
    EXPECT_TRUE(blast.Report().Complete());
    EXPECT_EQ(blast.ops[0].op, sim::WorldOp::Carve(StemCut(), kStrong));
    EXPECT_GT(blast.ops[0].result.changed, 0u);
    EXPECT_EQ(blast.ops[0].edits.size(), blast.ops[0].result.changed);
    ASSERT_EQ(blast.IslandCount(), 2u);

    uint32_t islandVoxels = 0;
    for (size_t i = 1; i < blast.ops.size(); ++i) {
        const sim::AppliedOp& applied = blast.ops[i];
        EXPECT_EQ(applied.op.kind, sim::WorldOpKind::Detach);
        EXPECT_EQ(applied.op.netId, 0u);
        // Everything the mask names was solid and is gone now.
        EXPECT_EQ(applied.result.changed, applied.op.mask.Count());
        const voxel::VoxelVolume piece = sim::BuildDetachedPiece(applied.op, applied.edits);
        EXPECT_EQ(piece.CountSolid(), applied.op.mask.Count());
        for (const voxel::VoxelEdit& e : applied.edits) EXPECT_EQ(a.World().GetVoxel(e.voxel), 0);
        // Pushed away from the blast and upwards a little, as the impulse would.
        EXPECT_GT(glm::length(applied.op.velocity), 0.0f);
        islandVoxels += applied.op.mask.Count();
    }
    // Both caps, each with what is left of its stem above the cut.
    EXPECT_GT(islandVoxels, 2u * 11u * 3u * 11u - 50u);
    // Islands are first in scan order: the cap at x = -4 starts before the one at x = 9.
    EXPECT_LT(blast.ops[1].op.min.x, blast.ops[2].op.min.x);
    // The caps no longer exist in the world, the stumps do.
    EXPECT_EQ(a.World().GetVoxel({0, kGroundVoxels + 17, 0}), 0);
    EXPECT_EQ(a.World().GetVoxel({0, kGroundVoxels + 1, 0}), ids.stone);
    // A simulation keeps no body for an island: rubble is made by whoever draws it.
    EXPECT_EQ(a.simulation.Scene().VoxelObjectCount(), objectsBefore);
    EXPECT_EQ(a.simulation.Particles().AliveCount(), 0u);        // and no particles either
}

TEST(Blast, IslandSearchThatCannotSeeTheWholeBoxEmitsNothing) {
    Arena a;
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    // A mushroom right at the window's edge (chunks x -3..2 = voxels -96..95): the island box
    // (radius + 24 voxels) reaches outside the window.
    ApplyAll(a.World(), std::vector<sim::WorldOp>{sim::WorldOp::Box({88, kGroundVoxels, 0}, {91, kGroundVoxels + 16, 3}, ids.stone),
                                                  sim::WorldOp::Box({86, kGroundVoxels + 16, -2}, {93, kGroundVoxels + 19, 5}, ids.stone)});
    const voxel::SphereQ cut{glm::ivec3(89 * 16 + 8, (kGroundVoxels + 8) * 16 + 8, 16 + 8), 40};
    const sim::BlastResult blast = sim::Blast(a.simulation, cut, kStrong);
    EXPECT_GT(blast.ops[0].result.changed, 0u);
    EXPECT_FALSE(blast.islandsSearched);
    EXPECT_EQ(blast.IslandCount(), 0u);
    EXPECT_EQ(a.World().GetVoxel({88, kGroundVoxels + 17, 0}), ids.stone);   // the cap still floats
}

// The search that could not see its box is not lost: the world loads the box, and the search
// runs once it is there (RetryIslandSearches), as a deferred result without a second explosion.
TEST(Blast, IslandSearchRunsLaterOnceItsBoxHasLoaded) {
    Arena a;
    ASSERT_TRUE(a.initialized);
    const voxel::WorldMaterials& ids = a.Ids();
    ApplyAll(a.World(), std::vector<sim::WorldOp>{sim::WorldOp::Box({88, kGroundVoxels, 0}, {91, kGroundVoxels + 16, 3}, ids.stone),
                                                  sim::WorldOp::Box({86, kGroundVoxels + 16, -2}, {93, kGroundVoxels + 19, 5}, ids.stone)});
    const voxel::SphereQ cut{glm::ivec3(89 * 16 + 8, (kGroundVoxels + 8) * 16 + 8, 16 + 8), 40};
    const sim::BlastResult blast = sim::Blast(a.simulation, cut, kStrong);
    ASSERT_FALSE(blast.islandsSearched);
    ASSERT_EQ(a.simulation.DeferredIslandSearches().size(), 1u);
    EXPECT_TRUE(sim::RetryIslandSearches(a.simulation).empty());       // the box is still partly outside the window
    EXPECT_EQ(a.simulation.DeferredIslandSearches().size(), 1u);

    // The window moves one chunk towards the mushroom: the whole box is inside and loads.
    a.World().LoadBlocking(kFocus + glm::dvec3(3.5, 0.0, 0.0));
    const std::vector<sim::BlastResult> late = sim::RetryIslandSearches(a.simulation);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_TRUE(late[0].deferred);
    EXPECT_TRUE(late[0].islandsSearched);
    EXPECT_EQ(late[0].IslandCount(), 1u);
    for (const sim::AppliedOp& applied : late[0].ops) EXPECT_NE(applied.op.kind, sim::WorldOpKind::CarveSphere);
    EXPECT_EQ(a.World().GetVoxel({88, kGroundVoxels + 17, 0}), 0);           // the cap fell
    EXPECT_EQ(a.World().GetVoxel({88, kGroundVoxels + 1, 0}), ids.stone);    // the stump stays
    EXPECT_TRUE(a.simulation.DeferredIslandSearches().empty());
    // Presented without an explosion: no particles (the cap is one piece of rubble).
    sim::PresentBlast(a.simulation, late[0]);
    EXPECT_EQ(a.simulation.Particles().AliveCount(), 0u);
    EXPECT_TRUE(sim::RetryIslandSearches(a.simulation).empty());
}

TEST(Blast, DamagesAndPushesObjectsWithoutTouchingTheirVoxelsInTheWorld) {
    Arena a;
    ASSERT_TRUE(a.initialized);
    scene::Scene& scene = a.simulation.Scene();
    voxel::VoxelVolume volume(glm::ivec3(8));
    volume.FillBox(glm::ivec3(0), glm::ivec3(8), a.Ids().wood);
    const Entity crate = scene.CreateVoxelObject(
        {.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, "crate"), .position = {0.0f, 8.0f, 0.0f}});
    ASSERT_NE(crate, kNoEntity);
    const voxel::WorldHashResult before = a.Hash();

    // A small blast at the crate's vertical edge through its corner (= the body position, which
    // the push is measured to), high enough above the ground to carve no terrain.
    const sim::BlastResult blast = sim::Blast(a.simulation, sim::BlastDesc{glm::dvec3(0.0, 8.3, 0.0), 0.25f, 1.0f});
    EXPECT_EQ(blast.ops.size(), 1u);
    EXPECT_EQ(blast.ops[0].result.changed, 0u);
    EXPECT_EQ(a.Hash().hash, before.hash);
    EXPECT_EQ(blast.objectsHit, 1u);
    EXPECT_GT(blast.objectVoxels, 0u);
    EXPECT_TRUE(blast.fragments.empty());                       // offline: every piece is an entity
    ASSERT_TRUE(scene.IsValid(crate));
    const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(crate);
    ASSERT_NE(body, nullptr);
    EXPECT_GT(glm::length(a.simulation.Physics().GetState(body->body).linearVelocity), 0.0f);
    // The quantised blast is what was applied.
    EXPECT_NEAR(blast.center.y, 8.3, 0.1 / 32.0);
    EXPECT_NEAR(blast.radius, 0.25f, 0.1f / 32.0f);
}

TEST(Blast, OnAServerSmallObjectPiecesComeBackAsFragmentsWithThePush) {
    Arena a(sim::ServerConfig(ArenaConfig()));
    ASSERT_TRUE(a.initialized);
    scene::Scene& scene = a.simulation.Scene();
    voxel::VoxelVolume volume(glm::ivec3(8));
    volume.FillBox(glm::ivec3(0), glm::ivec3(8), a.Ids().wood);
    const Entity crate = scene.CreateVoxelObject(
        {.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, "crate"), .position = {0.0f, 8.0f, 0.0f}});
    ASSERT_NE(crate, kNoEntity);
    const uint32_t bodies = a.simulation.Physics().BodyCount();

    const sim::BlastResult blast = sim::Blast(a.simulation, sim::BlastDesc{glm::dvec3(0.0, 8.3, 0.0), 0.25f, 1.0f});
    EXPECT_EQ(blast.objectsHit, 1u);
    // 512 voxels minus the bite: below the server threshold, so the crate is client rubble now.
    EXPECT_FALSE(scene.IsValid(crate));
    ASSERT_EQ(blast.fragments.size(), 1u);
    EXPECT_LE(blast.fragments[0].model->SolidCount() + blast.objectVoxels, 512u);
    EXPECT_GT(blast.fragments[0].model->SolidCount(), 400u);
    EXPECT_FALSE(blast.fragments[0].model->IsBaked());
    EXPECT_GT(glm::length(blast.fragments[0].linearVelocity), 0.0f);    // the push it would have got
    EXPECT_EQ(a.simulation.Physics().BodyCount(), bodies - 1u);
}

// --- the stage-2 acceptance --------------------------------------------------------------------

TEST(Blast, TwoSimulationsFedTheSameOperationsHaveEqualHashes) {
    // Different worker counts: load and collider timing differ, the voxels must not.
    Arena server(ArenaConfig(3)), client(ArenaConfig(1));
    ASSERT_TRUE(server.initialized && client.initialized);
    ASSERT_EQ(server.Hash().hash, client.Hash().hash);         // same generator, same window
    const std::vector<sim::WorldOp> setup = Mushrooms(server.Ids());
    ApplyAll(server.World(), setup);
    ApplyAll(client.World(), setup);

    // The server blasts twice: the stems, then a hole in the ground next to the stumps (no
    // island). The client only ever applies the operations, islands included.
    std::vector<sim::WorldOp> stream;
    for (const sim::BlastResult& blast : {sim::Blast(server.simulation, StemCut(), kStrong),
                                          sim::Blast(server.simulation, sim::BlastDesc{glm::dvec3(-0.73, 8.02, 0.41), 0.6f, 6.0f})}) {
        for (const sim::WorldOp& op : OpsOf(blast)) stream.push_back(op);
    }
    ASSERT_EQ(stream.size(), 4u);                              // carve, 2 x detach, carve
    std::vector<voxel::VoxelEdit> serverPieces;
    for (const sim::WorldOp& op : stream) {
        std::vector<voxel::VoxelEdit> edits;
        const sim::OpResult r = sim::ApplyOp(client.World(), op, &edits);
        EXPECT_TRUE(r.Complete());
        if (op.kind == sim::WorldOpKind::Detach) {
            // The client's piece has exactly the server's voxels.
            EXPECT_EQ(sim::BuildDetachedPiece(op, edits).CountSolid(), op.mask.Count());
        }
    }

    const voxel::WorldHashResult s = server.Hash(), c = client.Hash();
    ASSERT_TRUE(s.Complete());
    ASSERT_TRUE(c.Complete());
    EXPECT_EQ(s.hash, c.hash);
    EXPECT_EQ(s.solidChunks, c.solidChunks);

    // Replaying the stream on a third simulation with one operation slightly different (the
    // first carve 1/16 voxel larger) gives a different world.
    Arena other(ArenaConfig(2));
    ASSERT_TRUE(other.initialized);
    ApplyAll(other.World(), setup);
    std::vector<sim::WorldOp> changed = stream;
    changed[0].sphere.radius = static_cast<uint16_t>(changed[0].sphere.radius + 1);
    for (const sim::WorldOp& op : changed) sim::ApplyOp(other.World(), op);
    EXPECT_NE(other.Hash().hash, s.hash);

    // And without one of the Detach operations the island stays: different again.
    Arena missing(ArenaConfig(2));
    ASSERT_TRUE(missing.initialized);
    ApplyAll(missing.World(), setup);
    for (size_t i = 0; i < stream.size(); ++i) {
        if (i != 2) sim::ApplyOp(missing.World(), stream[i]);
    }
    EXPECT_NE(missing.Hash().hash, s.hash);
}

// --- crumbs: islands under scene::MinPieceVoxels of the world's size ----------------------------

namespace {
    /// The crumb rule of this 0.1 m arena: 16 voxels (0.016 m^3).
    constexpr uint32_t kMinPiece = scene::MinPieceVoxels(static_cast<double>(kVoxel));
    static_assert(kMinPiece == 16);

    /// Floating crumbs high above the left mushroom, all inside StemCut's island box (x -26..39,
    /// y g-25..g+40, z -32..33): two rows of 1 to 5 voxel bars, a bar along z of kMinPiece - 1
    /// voxels (the largest crumb) and an upright 1 x 2 x 2 square. 22 crumbs.
    std::vector<sim::WorldOp> Crumbs(const voxel::WorldMaterials& ids) {
        const int y = kGroundVoxels + 30;
        std::vector<sim::WorldOp> ops;
        for (int row = 0; row < 2; ++row) {
            const int z = row == 0 ? -10 : 10;
            for (int i = 0; i < 10; ++i) {
                const int x = -20 + 6 * i, length = 1 + i % 5;
                ops.push_back(sim::WorldOp::Box({x, y, z}, {x + length, y + 1, z + 1}, row == 0 ? ids.stone : ids.dirt));
            }
        }
        constexpr int kLargest = static_cast<int>(kMinPiece) - 1;
        ops.push_back(sim::WorldOp::Box({0, y + 4, -2}, {1, y + 5, -2 + kLargest}, ids.wood));
        ops.push_back(sim::WorldOp::Box({30, y - 6, 25}, {31, y - 4, 27}, ids.gold));
        return ops;
    }
    static_assert(kMinPiece > 5, "the 5-voxel bars must stay crumbs");
    constexpr uint32_t kCrumbCount = 22, kCrumbVoxels = 2 * 2 * (1 + 2 + 3 + 4 + 5) + (kMinPiece - 1) + 4;

    /// The left mushroom (one island when StemCut hits its stem) plus the crumbs.
    std::vector<sim::WorldOp> MushroomAndCrumbs(const voxel::WorldMaterials& ids) {
        std::vector<sim::WorldOp> ops = Mushrooms(ids);
        ops.resize(2);
        for (const sim::WorldOp& op : Crumbs(ids)) ops.push_back(op);
        return ops;
    }
}

TEST(Blast, CrumbsBecomeOneClearMaskAndLargerIslandsKeepTheirDetach) {
    Arena a;
    ASSERT_TRUE(a.initialized);
    ApplyAll(a.World(), MushroomAndCrumbs(a.Ids()));
    const size_t objectsBefore = a.simulation.Scene().VoxelObjectCount();
    const uint32_t bodiesBefore = a.simulation.Physics().BodyCount();

    const sim::BlastResult blast = sim::Blast(a.simulation, StemCut(), kStrong);
    ASSERT_TRUE(blast.islandsSearched);
    EXPECT_TRUE(blast.Report().Complete());
    // Carve, the cap's Detach, one ClearMask for the 22 crumbs - not 22 more Detach records.
    ASSERT_EQ(blast.ops.size(), 3u);
    EXPECT_EQ(blast.ops[0].op.kind, sim::WorldOpKind::CarveSphere);
    EXPECT_EQ(blast.ops[1].op.kind, sim::WorldOpKind::Detach);
    EXPECT_GE(blast.ops[1].op.mask.Count(), kMinPiece);
    EXPECT_EQ(blast.IslandCount(), 1u);
    EXPECT_EQ(blast.crumbs, kCrumbCount);

    const sim::AppliedOp& clear = blast.ops[2];
    ASSERT_EQ(clear.op.kind, sim::WorldOpKind::ClearMask);
    EXPECT_EQ(clear.op, sim::WorldOp::Clear(clear.op.min, clear.op.mask));      // only min and mask are set
    EXPECT_EQ(clear.op.mask.Count(), kCrumbVoxels);
    EXPECT_EQ(clear.result.changed, kCrumbVoxels);
    EXPECT_EQ(clear.edits.size(), kCrumbVoxels);
    EXPECT_EQ(blast.CrumbVoxels(), kCrumbVoxels);
    // The mask's box is the smallest one holding every crumb: x from the first bar to the end
    // of the last (-20..38), y from the square to the z bar (g+24..g+34), z from the first row to
    // the square (-10..26).
    EXPECT_EQ(clear.op.min, glm::ivec3(-20, kGroundVoxels + 24, -10));
    EXPECT_EQ(clear.op.mask.Size(), glm::ivec3(39 + 20, 35 - 24, 27 + 10));
    for (const voxel::VoxelEdit& e : clear.edits) {
        EXPECT_NE(e.oldId, 0);
        EXPECT_EQ(a.World().GetVoxel(e.voxel), 0);
    }
    for (const sim::WorldOp& op : Crumbs(a.Ids())) EXPECT_EQ(a.World().GetVoxel(op.min), 0);
    // The simulation makes nothing of them.
    EXPECT_EQ(a.simulation.Scene().VoxelObjectCount(), objectsBefore);
    EXPECT_EQ(a.simulation.Physics().BodyCount(), bodiesBefore);
    EXPECT_EQ(a.simulation.Particles().AliveCount(), 0u);
}

// The crumb rule is the piece volume (scene::kMinPieceVolume) in voxels of the world's size:
// 16 voxels in this 0.1 m arena, 8 at the engine's 0.125 m.
TEST(Blast, IslandOfMinPieceVoxelsIsAPieceAndOfOneLessACrumb) {
    for (const float voxelSize : {kVoxel, 0.125f, 0.15f}) {
        sim::SimulationConfig config = ArenaConfig();
        config.world.voxelSize = voxelSize;
        Arena a(config);
        ASSERT_TRUE(a.initialized);
        const voxel::WorldMaterials& ids = a.Ids();
        // 10 voxels above the ground: the island box (+-26 voxels around the blast) must fit in
        // the arena's 6 chunk rows, which end at voxel 127 at 0.15 m (rows -2..3 of 4.8 m).
        const int y = kGroundVoxels + 10;
        const uint32_t minPiece = scene::MinPieceVoxels(static_cast<double>(voxelSize));
        EXPECT_EQ(minPiece, voxelSize == kVoxel ? 16u : voxelSize == 0.125f ? 8u : 5u);
        const int k = static_cast<int>(minPiece);
        ApplyAll(a.World(), std::vector<sim::WorldOp>{sim::WorldOp::Box({-8, y, 0}, {-8 + k, y + 1, 1}, ids.stone),
                                                      sim::WorldOp::Box({-8, y, 4}, {-8 + k - 1, y + 1, 5}, ids.stone)});
        // A blast in the air next to them: nothing carved, both bars are found by the search.
        const sim::BlastResult blast = sim::Blast(a.simulation, voxel::SphereQ{glm::ivec3(8, (y + 8) * 16 + 8, 8), 32}, kStrong);
        EXPECT_EQ(blast.ops[0].result.changed, 0u);
        ASSERT_TRUE(blast.islandsSearched) << voxelSize;
        ASSERT_EQ(blast.ops.size(), 3u) << voxelSize;
        EXPECT_EQ(blast.ops[1].op.kind, sim::WorldOpKind::Detach);
        EXPECT_EQ(blast.ops[1].op.mask.Count(), minPiece);
        EXPECT_EQ(blast.ops[2].op.kind, sim::WorldOpKind::ClearMask);
        EXPECT_EQ(blast.ops[2].op.mask.Count(), minPiece - 1);
        EXPECT_EQ(blast.crumbs, 1u);
    }
}

TEST(Blast, OfflineCrumbsBecomeParticlesWithoutAnyEntityOrBody) {
    Arena a;                                                   // offline: markDebris, rubble entities on
    ASSERT_TRUE(a.initialized);
    ApplyAll(a.World(), Crumbs(a.Ids()));
    scene::Scene& scene = a.simulation.Scene();
    fx::ParticleSystem& particles = a.simulation.Particles();
    const size_t entitiesBefore = scene.EntityCount();
    const uint32_t bodiesBefore = a.simulation.Physics().BodyCount();

    // A blast in the air below the crumbs (StemCut 12 voxels higher): no carve, no island but
    // the crumbs.
    const voxel::SphereQ inTheAir{glm::ivec3(7 * 16 + 8, (kGroundVoxels + 20) * 16 + 8, 16 + 8), 9 * 16};
    const sim::BlastResult blast = sim::Blast(a.simulation, inTheAir, kStrong);
    EXPECT_EQ(blast.ops[0].result.changed, 0u);
    ASSERT_EQ(blast.ops.size(), 2u);
    ASSERT_EQ(blast.ops[1].op.kind, sim::WorldOpKind::ClearMask);
    EXPECT_EQ(blast.crumbs, kCrumbCount);

    sim::PresentBlast(a.simulation, blast);
    EXPECT_EQ(scene.VoxelObjectCount(), 0u);
    EXPECT_EQ(scene.DebrisCount(), 0u);
    EXPECT_EQ(scene.EntityCount(), entitiesBefore + 1u);        // the flash, nothing else
    EXPECT_EQ(a.simulation.Physics().BodyCount(), bodiesBefore);
    const uint32_t withCrumbs = particles.AliveCount();

    // The same blast without its ClearMask shows only the explosion: the crumbs made particles.
    particles.Clear();
    sim::BlastResult bare = blast;
    bare.ops.resize(1);
    sim::PresentBlast(a.simulation, bare);
    const uint32_t bursts = (kCrumbVoxels + scene.Config().debrisParticleStride - 1) / scene.Config().debrisParticleStride;
    EXPECT_GE(withCrumbs, particles.AliveCount() + bursts);
}

TEST(Blast, OfflineLargeIslandStillBecomesRubble) {
    Arena a;
    ASSERT_TRUE(a.initialized);
    ApplyAll(a.World(), MushroomAndCrumbs(a.Ids()));
    const uint32_t bodiesBefore = a.simulation.Physics().BodyCount();
    const sim::BlastResult blast = sim::Blast(a.simulation, StemCut(), kStrong);
    ASSERT_EQ(blast.IslandCount(), 1u);
    sim::PresentBlast(a.simulation, blast);
    // One rubble object (the cap) and nothing for the 22 crumbs.
    EXPECT_EQ(a.simulation.Scene().VoxelObjectCount(), 1u);
    EXPECT_EQ(a.simulation.Scene().DebrisCount(), 1u);          // 300+ voxels: a rubble body (shred.md stage 1e)
    EXPECT_EQ(a.simulation.Scene().Rubble().bodies, 1u);
    EXPECT_EQ(a.simulation.Physics().BodyCount(), bodiesBefore + 1u);
}

TEST(Blast, TwoSimulationsFedOperationsWithAClearMaskHaveEqualHashes) {
    Arena server(ArenaConfig(3)), client(ArenaConfig(1));
    ASSERT_TRUE(server.initialized && client.initialized);
    const std::vector<sim::WorldOp> setup = MushroomAndCrumbs(server.Ids());
    ApplyAll(server.World(), setup);
    ApplyAll(client.World(), setup);
    ASSERT_EQ(server.Hash().hash, client.Hash().hash);

    const std::vector<sim::WorldOp> stream = OpsOf(sim::Blast(server.simulation, StemCut(), kStrong));
    ASSERT_EQ(stream.size(), 3u);
    for (const sim::WorldOp& op : stream) {
        const sim::OpResult r = sim::ApplyOp(client.World(), op);
        EXPECT_TRUE(r.Complete());
        if (op.kind == sim::WorldOpKind::ClearMask) EXPECT_EQ(r.changed, kCrumbVoxels);
    }
    const voxel::WorldHashResult s = server.Hash(), c = client.Hash();
    ASSERT_TRUE(s.Complete() && c.Complete());
    EXPECT_EQ(s.hash, c.hash);
    EXPECT_EQ(s.solidChunks, c.solidChunks);

    // The blast is a function of the voxels: a second server gives the same operations.
    Arena again(ArenaConfig(2));
    ASSERT_TRUE(again.initialized);
    ApplyAll(again.World(), setup);
    EXPECT_EQ(OpsOf(sim::Blast(again.simulation, StemCut(), kStrong)), stream);

    // Without the ClearMask the crumbs stay: a different world.
    Arena missing(ArenaConfig(2));
    ASSERT_TRUE(missing.initialized);
    ApplyAll(missing.World(), setup);
    sim::ApplyOp(missing.World(), stream[0]);
    sim::ApplyOp(missing.World(), stream[1]);
    EXPECT_NE(missing.Hash().hash, s.hash);
}
