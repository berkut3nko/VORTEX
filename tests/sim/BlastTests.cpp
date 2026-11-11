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
    EXPECT_EQ(a.Hash().hash, b.Hash().hash);

    // Bounds of each kind.
    glm::ivec3 mn, mx;
    sim::WorldOp::Box({1, 2, 3}, {4, 5, 6}, 1).Bounds(mn, mx);
    EXPECT_EQ(mn, glm::ivec3(1, 2, 3));
    EXPECT_EQ(mx, glm::ivec3(4, 5, 6));
    sim::WorldOp::Detach({-3, 70, -3}, mask, glm::vec3(0.0f)).Bounds(mn, mx);
    EXPECT_EQ(mx - mn, glm::ivec3(7, 20, 5));
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
