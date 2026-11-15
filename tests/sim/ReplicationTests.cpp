// docs/design/networking.md stage 6 in the simulation: what a session server makes into server
// bodies with a scene::NetId (sim::Blast with SimulationConfig::replication), within the piece
// budgets (shred.md Part 4: at most 2 new server bodies per tick; networking.md: 256 live, 64 awake),
// and what stays client rubble. GPU-free; the network half is tests/net/EntityTests.cpp.

#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
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
    constexpr float kVoxel = 0.1f;                    // replicated pieces: 512 voxels (0.512 m^3)
    constexpr int kGround = 80;                       // flat ground: its top is at y = 8 m
    constexpr uint16_t kStrong = 1600;
    constexpr uint32_t kReplicated = scene::VoxelsForVolume(sim::kServerReplicatedPieceVolume, static_cast<double>(kVoxel));
    static_assert(kReplicated == 512);

    sim::SimulationConfig ServerArenaConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.jobThreads = 2;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = {8, 8, 8};          // every flood box of the stems below is loaded
        config.physics.maxBodies = 2048;
        config.physics.maxBodyPairs = 4096;
        config.physics.maxContactConstraints = 4096;
        config.physics.threads = 1;
        return sim::ServerConfig(config);
    }

    /// A server simulation on flat ground with its window loaded, and the despawns its
    /// on_destroy<NetId> observer saw (what net::Server turns into EntityDespawn events).
    struct ServerArena {
        explicit ServerArena(const sim::SimulationConfig& config = ServerArenaConfig()) : simulation(config) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(kGround, ids.grass, ids.stone));
            if (!initialized) return;
            simulation.World().LoadBlocking(glm::dvec3(0.0, 8.0, 0.0));
            simulation.Scene().Registry().on_destroy<scene::NetId>().connect<&ServerArena::OnDespawn>(*this);
        }
        ~ServerArena() { simulation.Scene().Registry().on_destroy<scene::NetId>().disconnect<&ServerArena::OnDespawn>(*this); }
        ServerArena(const ServerArena&) = delete;
        ServerArena& operator=(const ServerArena&) = delete;

        void OnDespawn(entt::registry& registry, entt::entity entity) {
            const scene::NetId& id = registry.get<scene::NetId>(entity);
            despawns.emplace_back(id.value, id.despawn);
        }

        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }

        /// A stone stem 3 x 30 x 3 at (x, ground, 0) with a planks cap `cap` x 4 x `cap` on top
        /// (12: 576 voxels, over the replicated size; 7: 196, under it).
        void Mushroom(int x, int cap = 12) {
            const int g = kGround;
            Apply(sim::WorldOp::Box({x, g, 0}, {x + 3, g + 30, 3}, Ids().stone));
            const int lo = x + 1 - cap / 2;
            Apply(sim::WorldOp::Box({lo, g + 30, 1 - cap / 2}, {lo + cap, g + 34, 1 - cap / 2 + cap}, Ids().planks));
        }
        void Apply(const sim::WorldOp& op) { ASSERT_TRUE(sim::ApplyOp(simulation.World(), op).Complete()); }

        /// The blast that cuts the stem of the mushroom at `x` below its cap.
        sim::BlastResult Cut(int x) {
            const voxel::SphereQ sphere{glm::ivec3((x + 1) * 16 + 8, (kGround + 24) * 16 + 8, 16 + 8), 3 * 16};
            return sim::Blast(simulation, sphere, kStrong);
        }

        /// Ticks of physics (streaming for the colliders the bodies ask for) until no piece is awake.
        bool SettlePieces(int maxTicks = 900) {
            for (int i = 0; i < maxTicks; ++i) {
                if (sim::CountPieces(simulation).awake == 0) return true;
                simulation.Step({});
            }
            return sim::CountPieces(simulation).awake == 0;
        }

        scene::Entity EntityOf(uint32_t netId) {
            for (const auto [entity, id] : simulation.Scene().Registry().view<const scene::NetId>().each())
                if (id.value == netId) return entity;
            return scene::kNoEntity;
        }

        sim::Simulation simulation;
        bool initialized = false;
        std::vector<std::pair<uint32_t, scene::DespawnReason>> despawns;
    };

    const sim::AppliedOp* FindDetach(const sim::BlastResult& r) {
        for (const sim::AppliedOp& a : r.ops)
            if (a.op.kind == sim::WorldOpKind::Detach) return &a;
        return nullptr;
    }
}

// A piece of the replicated size is a server body whose Detach carries its NetId; the body sits
// where the voxels were and its model is what the Detach removed (what a client builds from its own
// world). A smaller piece is client rubble: netId 0, no entity.
TEST(Replication, ALargePieceIsAServerBodyAndASmallOneClientRubble) {
    ServerArena a;
    ASSERT_TRUE(a.initialized);
    a.Mushroom(-30, 12);
    a.Mushroom(30, 7);
    const sim::BlastResult big = a.Cut(-30);
    const sim::AppliedOp* piece = FindDetach(big);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(piece->op.netId, 0u);
    EXPECT_GE(piece->result.changed, kReplicated);
    const scene::Entity entity = a.EntityOf(piece->op.netId);
    ASSERT_NE(entity, scene::kNoEntity);
    const scene::NetId& id = *a.simulation.Scene().TryGet<scene::NetId>(entity);
    EXPECT_TRUE(id.piece);
    EXPECT_EQ(id.bornTick, a.simulation.Tick());
    const scene::RigidBody* body = a.simulation.Scene().TryGet<scene::RigidBody>(entity);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(a.simulation.Physics().GetState(body->body).position, glm::vec3(a.simulation.World().VoxelToWorld(piece->op.min)));
    const voxel::VoxelVolume expected = sim::BuildDetachedPiece(piece->op, piece->edits);
    const scene::VoxelBody* voxels = a.simulation.Scene().TryGet<scene::VoxelBody>(entity);
    ASSERT_NE(voxels, nullptr);
    EXPECT_EQ(voxels->model->Volume().Data(), expected.Data());
    EXPECT_TRUE(big.lostNetIds.empty());

    const sim::BlastResult small = a.Cut(30);
    const sim::AppliedOp* rubble = FindDetach(small);
    ASSERT_NE(rubble, nullptr);
    EXPECT_EQ(rubble->op.netId, 0u);
    EXPECT_EQ(sim::CountPieces(a.simulation).live, 1u);
    EXPECT_EQ(a.simulation.Replication().stats.pieces, 1u);
}

// shred.md Part 4: at most 2 new server bodies per tick; the third piece of the tick is client
// rubble (netId 0), and the next tick has its budget again.
TEST(Replication, AtMostTwoNewServerBodiesPerTick) {
    ServerArena a;
    ASSERT_TRUE(a.initialized);
    for (const int x : {-60, -30, 0, 30}) a.Mushroom(x);
    std::vector<uint32_t> ids;
    for (const int x : {-60, -30, 0}) ids.push_back(FindDetach(a.Cut(x))->op.netId);
    EXPECT_NE(ids[0], 0u);
    EXPECT_NE(ids[1], 0u);
    EXPECT_EQ(ids[2], 0u);
    EXPECT_EQ(a.simulation.Replication().stats.overTickBudget, 1u);
    a.simulation.Step({});
    EXPECT_NE(FindDetach(a.Cut(30))->op.netId, 0u);
    EXPECT_EQ(sim::CountPieces(a.simulation).live, 3u);
}

// networking.md "Entity replication": with the awake cap reached and nothing asleep a new piece is
// client rubble; with the live cap reached the oldest resting piece is evicted ToRubble (every
// client keeps its copy as rubble), and when nothing rests the smallest awake one.
TEST(Replication, TheCapsLeaveNewPiecesToRubbleAndEvictTheOldestResting) {
    sim::SimulationConfig config = ServerArenaConfig();
    config.replication.bodiesPerTick = 8;
    config.replication.maxAwakePieces = 2;
    config.replication.maxLivePieces = 3;
    ServerArena a(config);
    ASSERT_TRUE(a.initialized);
    for (const int x : {-90, -60, -30, 0, 30, 60}) a.Mushroom(x);
    const uint32_t first = FindDetach(a.Cut(-90))->op.netId;
    const uint32_t second = FindDetach(a.Cut(-60))->op.netId;
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    EXPECT_EQ(FindDetach(a.Cut(-30))->op.netId, 0u);              // two awake, none asleep: rubble
    EXPECT_EQ(a.simulation.Replication().stats.overAwakeCap, 1u);
    ASSERT_TRUE(a.SettlePieces());
    const uint32_t third = FindDetach(a.Cut(0))->op.netId;          // live 3
    ASSERT_NE(third, 0u);
    EXPECT_TRUE(a.despawns.empty());
    const uint32_t fourth = FindDetach(a.Cut(30))->op.netId;        // evicts the oldest resting: the first
    ASSERT_NE(fourth, 0u);
    ASSERT_EQ(a.despawns.size(), 1u);
    EXPECT_EQ(a.despawns[0].first, first);
    EXPECT_EQ(a.despawns[0].second, scene::DespawnReason::ToRubble);
    EXPECT_EQ(a.simulation.Replication().stats.evictedResting, 1u);
    EXPECT_EQ(sim::CountPieces(a.simulation).live, 3u);

    // Nothing rests: the smallest awake piece goes (a fifth, smaller cap's piece is not it: the
    // piece being made is not a candidate).
    sim::SimulationConfig awakeConfig = ServerArenaConfig();
    awakeConfig.replication.bodiesPerTick = 8;
    awakeConfig.replication.maxLivePieces = 2;
    ServerArena b(awakeConfig);
    ASSERT_TRUE(b.initialized);
    b.Mushroom(-60, 14);
    b.Mushroom(0, 12);
    b.Mushroom(60, 13);
    const uint32_t large = FindDetach(b.Cut(-60))->op.netId;
    const uint32_t smallest = FindDetach(b.Cut(0))->op.netId;
    b.simulation.Step({});
    ASSERT_NE(large, 0u);
    ASSERT_NE(smallest, 0u);
    EXPECT_NE(FindDetach(b.Cut(60))->op.netId, 0u);
    ASSERT_EQ(b.despawns.size(), 1u);
    EXPECT_EQ(b.despawns[0].first, smallest);
    EXPECT_EQ(b.despawns[0].second, scene::DespawnReason::ToRubble);
    EXPECT_EQ(b.simulation.Replication().stats.evictedAwake, 1u);
}

// A replicated object a blast damages: the survivor is an ObjectEvent::Model (its new model and the
// offset of its origin), a piece of the replicated size an ObjectEvent::Spawn with a new NetId, and
// an object left under the threshold breaks apart (despawned Broken, its remains fragments).
TEST(Replication, DamagedObjectsGiveModelsSpawnsAndFragments) {
    ServerArena a;
    ASSERT_TRUE(a.initialized);
    // A dumbbell: two 8^3 cubes (512 voxels each: the replicated size) joined by a 2-voxel neck.
    voxel::VoxelVolume volume(glm::ivec3(18, 8, 8));
    volume.FillBox(glm::ivec3(0), glm::ivec3(8), a.Ids().planks);
    volume.FillBox(glm::ivec3(10, 0, 0), glm::ivec3(18, 8, 8), a.Ids().planks);
    volume.FillBox(glm::ivec3(8, 4, 4), glm::ivec3(10, 5, 5), a.Ids().planks);
    auto model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, "dumbbell");
    const glm::vec3 origin(-1.0f, 8.0f, -0.4f);
    const scene::Entity beam = a.simulation.Scene().CreateVoxelObject({.model = model, .position = origin});
    const uint32_t beamId = sim::Replicate(a.simulation, beam, false);
    ASSERT_NE(beamId, 0u);
    EXPECT_EQ(sim::Replicate(a.simulation, beam, true), beamId);    // once
    // The neck cut: two pieces of 512 voxels.
    const sim::BlastResult cut = sim::Blast(a.simulation, sim::BlastDesc{glm::dvec3(origin) + glm::dvec3(0.9, 0.45, 0.45), 0.1f, 6.0f});
    ASSERT_EQ(cut.objectEvents.size(), 2u);
    const sim::ObjectEvent& kept = cut.objectEvents[0];
    EXPECT_EQ(kept.kind, sim::ObjectEvent::Kind::Model);
    EXPECT_EQ(kept.netId, beamId);
    ASSERT_TRUE(kept.model);
    const sim::ObjectEvent& spawn = cut.objectEvents[1];
    EXPECT_EQ(spawn.kind, sim::ObjectEvent::Kind::Spawn);
    EXPECT_GT(spawn.netId, beamId);
    ASSERT_TRUE(spawn.model);
    EXPECT_GE(spawn.model->SolidCount(), kReplicated);
    EXPECT_TRUE(a.simulation.Scene().TryGet<scene::NetId>(spawn.entity)->piece);
    EXPECT_TRUE(cut.fragments.empty());
    EXPECT_TRUE(a.despawns.empty());
    // Under the threshold: the survivor of the new piece breaks into fragments.
    const glm::vec3 at = a.simulation.Physics().GetState(a.simulation.Scene().TryGet<scene::RigidBody>(spawn.entity)->body).position;
    const sim::BlastResult broken = sim::Blast(a.simulation, sim::BlastDesc{glm::dvec3(at) + glm::dvec3(0.05, 0.05, 0.05), 0.12f, 6.0f});
    EXPECT_FALSE(broken.fragments.empty());
    ASSERT_FALSE(a.despawns.empty());
    EXPECT_EQ(a.despawns.back().first, spawn.netId);
    EXPECT_EQ(a.despawns.back().second, scene::DespawnReason::Broken);
}

// Offline nothing is replicated: every piece is netId 0 local rubble, no entity has a NetId (the
// offline game is unchanged).
TEST(Replication, OfflineBlastsReplicateNothing) {
    sim::SimulationConfig config = ServerArenaConfig();
    sim::SimulationConfig offline;
    offline.chunkCapacity = config.chunkCapacity;
    offline.jobThreads = config.jobThreads;
    offline.world = config.world;
    offline.physics.maxBodies = config.physics.maxBodies;
    offline.physics.maxBodyPairs = config.physics.maxBodyPairs;
    offline.physics.maxContactConstraints = config.physics.maxContactConstraints;
    offline.physics.threads = 1;
    ServerArena a(offline);
    ASSERT_TRUE(a.initialized);
    a.Mushroom(0, 12);
    const sim::BlastResult r = a.Cut(0);
    ASSERT_NE(FindDetach(r), nullptr);
    EXPECT_EQ(FindDetach(r)->op.netId, 0u);
    EXPECT_TRUE(r.objectEvents.empty());
    EXPECT_TRUE(a.simulation.Scene().Registry().view<scene::NetId>().empty());
    EXPECT_EQ(a.simulation.Replication().stats.pieces, 0u);
}
