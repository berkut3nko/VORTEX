// The scene as a session server runs it (sim::ServerConfig): models are never baked into the
// chunk store, there are no particles and nothing is ever marked as rubble. The offline
// behaviour of the same functions is covered by tests/engine/SceneTests.cpp.

#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

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
    physics::PhysicsConfig SmallPhysics() {
        physics::PhysicsConfig config;
        config.maxBodies = 1024;
        config.maxBodyPairs = 4096;
        config.maxContactConstraints = 4096;
        config.threads = 1;
        return config;
    }

    /// The subsystems a scene references, then the scene itself (declared last: destroyed first).
    /// No world: the scene does not need one.
    struct Bench {
        explicit Bench(const scene::SceneConfig& config, uint32_t particleCapacity = 0)
            : store(64), particles(particleCapacity), scene(store, materials, physics, particles, config) {
            ids = voxel::RegisterDefaultWorldMaterials(materials);
            initialized = physics.Initialize(SmallPhysics());
        }

        std::shared_ptr<voxel::VoxelModel> Box(const glm::ivec3& size, const std::string& name = "box") {
            voxel::VoxelVolume volume(size);
            volume.FillBox(glm::ivec3(0), size, ids.wood);
            return std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, name);
        }

        voxel::ChunkStore store;
        voxel::MaterialLibrary materials;
        voxel::WorldMaterials ids;
        physics::PhysicsWorld physics;
        fx::ParticleSystem particles;
        scene::Scene scene;
        bool initialized = false;
    };

    scene::SceneConfig ServerScene() { return sim::ServerConfig().scene; }
}

TEST(ServerScene, ModelsAreNeverBakedAndUseNoChunkSlots) {
    Bench b(ServerScene());
    ASSERT_TRUE(b.initialized);
    const std::shared_ptr<voxel::VoxelModel> model = b.Box({24, 3, 3}, "beam");
    const Entity beam = b.scene.CreateVoxelObject({.model = model, .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(beam, kNoEntity);
    EXPECT_FALSE(model->IsBaked());
    EXPECT_EQ(b.store.UsedCount(), 0u);
    // It is a full object all the same: a body with the mass of its voxels.
    const scene::RigidBody* body = b.scene.TryGet<scene::RigidBody>(beam);
    ASSERT_NE(body, nullptr);
    EXPECT_TRUE(b.physics.IsValid(body->body));
    EXPECT_GT(b.physics.GetMass(body->body), 0.0f);

    // Cut in the middle: two objects, each with a body, still no chunk slot.
    const scene::DamageResult cut = b.scene.Damage(beam, glm::dvec3(1.23, 5.15, 0.15), 0.24f);
    EXPECT_GT(cut.removed, 0u);
    ASSERT_EQ(cut.pieces.size(), 1u);
    EXPECT_EQ(b.scene.VoxelObjectCount(), 2u);
    EXPECT_EQ(b.physics.BodyCount(), 2u);
    EXPECT_EQ(b.store.UsedCount(), 0u);
    uint32_t voxels = 0;
    for (const Entity entity : {beam, cut.pieces[0]}) {
        const scene::VoxelBody* part = b.scene.TryGet<scene::VoxelBody>(entity);
        ASSERT_NE(part, nullptr);
        ASSERT_NE(part->model, nullptr);
        EXPECT_FALSE(part->model->IsBaked());
        voxels += part->model->SolidCount();
    }
    EXPECT_EQ(voxels + cut.removed + cut.loose, 216u);          // nothing lost, nothing made up

    b.scene.Clear();
    EXPECT_EQ(b.physics.BodyCount(), 0u);
    EXPECT_EQ(b.store.UsedCount(), 0u);
}

TEST(ServerScene, WorksWithAChunkStoreThatHasNoRoomAtAll) {
    // What "no chunk slots" buys: the store of a server belongs to the terrain alone.
    Bench b(ServerScene());
    ASSERT_TRUE(b.initialized);
    while (b.store.Alloc() != voxel::ChunkStore::kInvalidSlot) {}
    ASSERT_EQ(b.store.UsedCount(), b.store.Capacity());

    const Entity beam = b.scene.CreateVoxelObject({.model = b.Box({24, 3, 3}), .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(beam, kNoEntity);
    const scene::DamageResult cut = b.scene.Damage(beam, glm::dvec3(1.23, 5.15, 0.15), 0.24f);
    EXPECT_EQ(cut.pieces.size(), 1u);
    EXPECT_EQ(b.physics.BodyCount(), 2u);
}

TEST(ServerScene, ModelBakedBySomebodyElseIsUsedAsItIs) {
    Bench b(ServerScene());
    ASSERT_TRUE(b.initialized);
    const std::shared_ptr<voxel::VoxelModel> model = b.Box({8, 8, 8});
    ASSERT_TRUE(model->Bake(b.store));
    const uint32_t slots = b.store.UsedCount();
    ASSERT_GT(slots, 0u);
    const Entity crate = b.scene.CreateVoxelObject({.model = model, .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(crate, kNoEntity);
    EXPECT_TRUE(model->IsBaked());
    EXPECT_EQ(b.store.UsedCount(), slots);
}

TEST(ServerScene, NothingIsEverMarkedAsRubble) {
    Bench b(ServerScene());
    ASSERT_TRUE(b.initialized);
    ASSERT_GT(b.scene.Config().debrisLifetime, 0.0f);

    // A small lump created as rubble (what a blast does with a floating bit of terrain): an
    // object, but not marked.
    const Entity lump = b.scene.CreateVoxelObject({.model = b.Box({3, 3, 3}, "lump"), .position = {5.0f, 2.0f, 0.0f}, .debris = true});
    ASSERT_NE(lump, kNoEntity);
    EXPECT_EQ(b.scene.TryGet<scene::Debris>(lump), nullptr);
    // Crumbs below the piece size never become entities, here as everywhere.
    EXPECT_EQ(b.scene.CreateVoxelObject({.model = b.Box({2, 2, 1}, "crumb"), .position = {7.0f, 2.0f, 0.0f}, .debris = true}), kNoEntity);

    // A small piece split off by damage (27 voxels of a beam): not marked either.
    const Entity beam = b.scene.CreateVoxelObject({.model = b.Box({24, 3, 3}), .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(beam, kNoEntity);
    const scene::DamageResult cut = b.scene.Damage(beam, glm::dvec3(0.55, 5.15, 0.15), 0.25f);
    ASSERT_EQ(cut.pieces.size(), 1u);
    EXPECT_EQ(b.scene.TryGet<scene::VoxelBody>(cut.pieces[0])->model->SolidCount(), 27u);
    EXPECT_EQ(b.scene.TryGet<scene::Debris>(cut.pieces[0]), nullptr);

    // What is left of an object that is shot down to a lump: not marked.
    const Entity crate = b.scene.CreateVoxelObject({.model = b.Box({4, 4, 4}, "crate"), .position = {10.0f, 5.0f, 0.0f}});
    ASSERT_NE(crate, kNoEntity);
    const scene::DamageResult hit = b.scene.Damage(crate, glm::dvec3(10.0, 5.2, 0.2), 0.25f);
    EXPECT_GT(hit.removed, 0u);
    ASSERT_TRUE(b.scene.IsValid(crate));
    ASSERT_LT(b.scene.TryGet<scene::VoxelBody>(crate)->model->SolidCount(), b.scene.Config().debrisBodyVoxels);
    EXPECT_EQ(b.scene.TryGet<scene::Debris>(crate), nullptr);
    EXPECT_EQ(b.scene.DebrisCount(), 0u);

    // So no amount of time, no viewer and no cap removes anything: what exists on a server is
    // decided by the server's rules alone.
    b.scene.SetViewer(glm::dvec3(1000.0, 0.0, 0.0));
    const size_t before = b.scene.EntityCount();
    for (int i = 0; i < 10; ++i) b.scene.Update(10.0f);
    EXPECT_EQ(b.scene.EntityCount(), before);
    EXPECT_TRUE(b.scene.IsValid(lump));
    EXPECT_TRUE(b.scene.IsValid(cut.pieces[0]));
    EXPECT_TRUE(b.scene.IsValid(crate));
}

TEST(ServerScene, MarkDebrisAloneSwitchesTheMarkingOff) {
    // The two switches are independent: with models baked and particles on, `markDebris = false`
    // still marks nothing, and `debrisRemnants` is then irrelevant.
    scene::SceneConfig config;
    config.markDebris = false;
    ASSERT_TRUE(config.debrisRemnants);
    Bench b(config, 1024);
    ASSERT_TRUE(b.initialized);
    const Entity crate = b.scene.CreateVoxelObject({.model = b.Box({4, 4, 4}, "crate"), .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(crate, kNoEntity);
    EXPECT_GT(b.store.UsedCount(), 0u);                          // baked as usual
    const scene::DamageResult hit = b.scene.Damage(crate, glm::dvec3(0.0, 5.2, 0.2), 0.25f);
    EXPECT_GT(hit.removed, 0u);
    EXPECT_GT(b.particles.AliveCount(), 0u);                     // debris particles as usual
    ASSERT_TRUE(b.scene.IsValid(crate));
    ASSERT_LT(b.scene.TryGet<scene::VoxelBody>(crate)->model->SolidCount(), b.scene.Config().debrisBodyVoxels);
    EXPECT_EQ(b.scene.DebrisCount(), 0u);
}

TEST(ServerScene, DebrisComponentAddedByHandIsStillCleanedUp) {
    Bench b(ServerScene());
    ASSERT_TRUE(b.initialized);
    const Entity lump = b.scene.CreateVoxelObject({.model = b.Box({3, 3, 3}, "lump"), .position = {0.0f, 2.0f, 0.0f}});
    ASSERT_NE(lump, kNoEntity);
    b.scene.Registry().emplace<scene::Debris>(lump);
    EXPECT_EQ(b.scene.DebrisCount(), 1u);
    b.scene.Update(b.scene.Config().debrisLifetime + 1.0f);
    EXPECT_FALSE(b.scene.IsValid(lump));
    EXPECT_EQ(b.physics.BodyCount(), 0u);
}

TEST(ServerScene, EffectsAskedOfAnEmptyParticlePoolAreDropped) {
    Bench b(ServerScene());
    ASSERT_TRUE(b.initialized);
    EXPECT_EQ(b.particles.Capacity(), 0u);
    const Entity crate = b.scene.CreateVoxelObject({.model = b.Box({8, 8, 8}, "crate"), .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(crate, kNoEntity);
    // Carved voxels, loose fragments and a crumbling lump all ask for particles.
    const scene::DamageResult hit = b.scene.Damage(crate, glm::dvec3(0.4, 5.4, 0.4), 0.45f);
    EXPECT_GT(hit.removed, 0u);
    EXPECT_EQ(b.scene.CreateVoxelObject({.model = b.Box({2, 2, 1}, "crumb"), .position = {7.0f, 2.0f, 0.0f}, .debris = true}), kNoEntity);
    b.particles.Update(1.0f / 60.0f);
    EXPECT_EQ(b.particles.AliveCount(), 0u);
}
