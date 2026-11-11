// scene::Scene: entity lifetime, physics bodies, destruction, render data. Everything here runs
// on real ChunkStore / MaterialLibrary / PhysicsWorld / ParticleSystem objects and needs no GPU.

#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.graphics;
import vortex.scene;
import vortex.engine;      // GatherInstances / GatherLights: the scene's render data

using namespace vortex;
using scene::Entity;
using scene::kNoEntity;

namespace {
    constexpr float kStep = 1.0f / 60.0f;
    constexpr float kVoxel = 0.1f;

    physics::PhysicsConfig SmallConfig() {
        physics::PhysicsConfig config;
        config.maxBodies = 1024;
        config.maxBodyPairs = 4096;
        config.maxContactConstraints = 4096;
        config.threads = 1;
        return config;
    }

    /// The subsystems a scene references, then the scene itself (declared last: destroyed first).
    struct Fixture {
        explicit Fixture(uint32_t chunkCapacity = 256, bool withPhysics = true, const scene::SceneConfig& config = {},
                         const physics::PhysicsConfig& physicsConfig = SmallConfig())
            : store(chunkCapacity), particles(4096), scene(store, materials, physics, particles, config) {
            ids = voxel::RegisterDefaultWorldMaterials(materials);
            if (withPhysics) initialized = physics.Initialize(physicsConfig);
        }

        std::shared_ptr<voxel::VoxelModel> Box(const glm::ivec3& size, uint8_t id = 0, const std::string& name = "box") {
            voxel::VoxelVolume volume(size);
            volume.FillBox(glm::ivec3(0), size, id != 0 ? id : ids.wood);
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

    void ExpectVecNear(const glm::vec3& actual, const glm::vec3& expected, float tolerance) {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }

    void ExpectMatNear(const glm::mat4& actual, const glm::mat4& expected, float tolerance) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) EXPECT_NEAR(actual[c][r], expected[c][r], tolerance) << "column " << c << " row " << r;
    }

    using VoxelSet = std::set<std::tuple<int, int, int>>;

    /// Solid voxels of an entity expressed in the voxel grid of `frame` (the pose of the object
    /// they were cut from; its scale is the voxel size).
    void CollectVoxels(const scene::Scene& scene, Entity entity, const math::Transform& frame, VoxelSet& out, size_t& duplicates) {
        const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
        const scene::VoxelBody* body = scene.TryGet<scene::VoxelBody>(entity);
        ASSERT_NE(transform, nullptr);
        ASSERT_NE(body, nullptr);
        const voxel::VoxelVolume& volume = body->model->Volume();
        for (int z = 0; z < volume.Size().z; ++z)
            for (int y = 0; y < volume.Size().y; ++y)
                for (int x = 0; x < volume.Size().x; ++x) {
                    if (volume.Get(x, y, z) == 0) continue;
                    const glm::vec3 world = transform->current.TransformPoint(glm::vec3(x, y, z) + glm::vec3(0.5f));
                    const glm::ivec3 cell = glm::ivec3(glm::floor(frame.InverseTransformPoint(world)));
                    if (!out.insert({cell.x, cell.y, cell.z}).second) ++duplicates;
                }
    }

    /// Same, for an unrotated object whose minimum corner is at `origin`.
    void CollectVoxels(const scene::Scene& scene, Entity entity, const glm::vec3& origin, VoxelSet& out, size_t& duplicates) {
        math::Transform frame;
        frame.position = origin;
        frame.scale = kVoxel;
        CollectVoxels(scene, entity, frame, out, duplicates);
    }

    void ExpectQuatNear(const glm::quat& actual, const glm::quat& expected, float tolerance) {
        // q and -q are the same rotation.
        EXPECT_NEAR(std::abs(glm::dot(actual, expected)), 1.0f, tolerance);
    }
}

// --- lifetime ------------------------------------------------------------------------------------

TEST(Scene, CreateAndDestroyReleaseBodyAndChunkSlots) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    EXPECT_EQ(f.scene.EntityCount(), 0u);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 0u);
    EXPECT_FLOAT_EQ(f.scene.Config().voxelSize, kVoxel);

    const Entity entity = f.scene.CreateVoxelObject({.model = f.Box({40, 8, 8}), .position = {1.0f, 2.0f, 3.0f}, .name = "plank"});
    ASSERT_NE(entity, kNoEntity);
    EXPECT_TRUE(f.scene.IsValid(entity));
    EXPECT_EQ(f.scene.EntityCount(), 1u);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 1u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    EXPECT_EQ(f.store.UsedCount(), 2u);          // 40 voxels long = two chunks

    const scene::Transform* transform = f.scene.TryGet<scene::Transform>(entity);
    ASSERT_NE(transform, nullptr);
    ExpectVecNear(transform->current.position, {1.0f, 2.0f, 3.0f}, 0.0f);
    EXPECT_FLOAT_EQ(transform->current.scale, kVoxel);
    ExpectVecNear(transform->previous.position, transform->current.position, 0.0f);
    ASSERT_NE(f.scene.TryGet<scene::Name>(entity), nullptr);
    EXPECT_EQ(f.scene.TryGet<scene::Name>(entity)->value, "plank");
    const scene::RigidBody* body = f.scene.TryGet<scene::RigidBody>(entity);
    ASSERT_NE(body, nullptr);
    EXPECT_TRUE(f.physics.IsValid(body->body));
    EXPECT_EQ(body->motion, physics::MotionType::Dynamic);
    // Mass is real: 40 x 8 x 8 voxels of wood.
    const float expectedMass = 40.0f * 8.0f * 8.0f * kVoxel * kVoxel * kVoxel * f.materials.Get(voxel::kWorldPalette, f.ids.wood).density;
    EXPECT_NEAR(f.physics.GetMass(body->body), expectedMass, expectedMass * 0.01f);

    f.scene.Destroy(entity);
    EXPECT_FALSE(f.scene.IsValid(entity));
    EXPECT_EQ(f.scene.EntityCount(), 0u);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 0u);
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    EXPECT_EQ(f.store.UsedCount(), 0u);
}

TEST(Scene, ModelNameIsTheDefaultNameAndBodyIsOptional) {
    Fixture f;
    const Entity named = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}, 0, "crate"), .withBody = false});
    ASSERT_NE(named, kNoEntity);
    ASSERT_NE(f.scene.TryGet<scene::Name>(named), nullptr);
    EXPECT_EQ(f.scene.TryGet<scene::Name>(named)->value, "crate");
    EXPECT_EQ(f.scene.TryGet<scene::RigidBody>(named), nullptr);
    EXPECT_EQ(f.physics.BodyCount(), 0u);

    const Entity anonymous = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}, 0, ""), .withBody = false});
    ASSERT_NE(anonymous, kNoEntity);
    EXPECT_EQ(f.scene.TryGet<scene::Name>(anonymous), nullptr);

    // A visual-only object works without any physics world at all.
    Fixture noPhysics(256, false);
    EXPECT_NE(noPhysics.scene.CreateVoxelObject({.model = noPhysics.Box({4, 4, 4}), .withBody = false}), kNoEntity);
}

TEST(Scene, SharedModelKeepsItsSlotsUntilTheLastUserIsGone) {
    Fixture f;
    std::shared_ptr<voxel::VoxelModel> model = f.Box({8, 8, 8});
    const Entity a = f.scene.CreateVoxelObject({.model = model, .position = {0.0f, 0.0f, 0.0f}});
    const Entity b = f.scene.CreateVoxelObject({.model = model, .position = {5.0f, 0.0f, 0.0f}});
    std::weak_ptr<voxel::VoxelModel> watch = model;
    EXPECT_EQ(f.store.UsedCount(), 1u);          // one set of chunks for both
    EXPECT_EQ(f.physics.BodyCount(), 2u);

    f.scene.Destroy(a);
    EXPECT_EQ(f.store.UsedCount(), 1u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    f.scene.Destroy(b);
    EXPECT_EQ(f.store.UsedCount(), 1u);          // the test still holds a reference
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    model.reset();
    EXPECT_TRUE(watch.expired());
    EXPECT_EQ(f.store.UsedCount(), 0u);
}

TEST(Scene, RegistryOperationsReleaseBodiesToo) {
    Fixture f;
    const Entity a = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4})});
    const Entity b = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {3.0f, 0.0f, 0.0f}});
    const Entity c = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {6.0f, 0.0f, 0.0f}});
    f.scene.CreateLight({});
    EXPECT_EQ(f.physics.BodyCount(), 3u);
    EXPECT_EQ(f.scene.EntityCount(), 4u);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 3u);

    f.scene.Registry().destroy(a);
    EXPECT_EQ(f.physics.BodyCount(), 2u);
    EXPECT_EQ(f.store.UsedCount(), 2u);

    f.scene.Registry().remove<scene::RigidBody>(b);          // becomes visual only
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    EXPECT_TRUE(f.scene.IsValid(b));
    EXPECT_EQ(f.store.UsedCount(), 2u);

    f.scene.Clear();
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    EXPECT_EQ(f.store.UsedCount(), 0u);
    EXPECT_EQ(f.scene.EntityCount(), 0u);
    EXPECT_FALSE(f.scene.IsValid(b));
    EXPECT_FALSE(f.scene.IsValid(c));
}

TEST(Scene, DestructorReleasesEverything) {
    voxel::ChunkStore store(64);
    voxel::MaterialLibrary materials;
    const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(materials);
    physics::PhysicsWorld physics;
    ASSERT_TRUE(physics.Initialize(SmallConfig()));
    fx::ParticleSystem particles(64);
    {
        scene::Scene scene(store, materials, physics, particles);
        voxel::VoxelVolume volume(glm::ivec3(4));
        volume.FillBox(glm::ivec3(0), glm::ivec3(4), ids.stone);
        ASSERT_NE(scene.CreateVoxelObject({.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette)}), kNoEntity);
        EXPECT_EQ(physics.BodyCount(), 1u);
        EXPECT_EQ(store.UsedCount(), 1u);
    }
    EXPECT_EQ(physics.BodyCount(), 0u);
    EXPECT_EQ(store.UsedCount(), 0u);
}

TEST(Scene, CreationFailuresReturnNoEntityAndLeakNothing) {
    {
        Fixture f;
        EXPECT_EQ(f.scene.CreateVoxelObject({}), kNoEntity);          // no model
        EXPECT_EQ(f.scene.EntityCount(), 0u);
    }
    {
        Fixture f(1);                                                 // one chunk slot, the model needs two
        std::shared_ptr<voxel::VoxelModel> model = f.Box({40, 4, 4});
        EXPECT_EQ(f.scene.CreateVoxelObject({.model = model}), kNoEntity);
        EXPECT_EQ(f.scene.EntityCount(), 0u);
        EXPECT_EQ(f.physics.BodyCount(), 0u);
        // The failed bake is rolled back even while the caller still holds the model.
        EXPECT_EQ(f.store.UsedCount(), 0u);
        EXPECT_FALSE(model->IsBaked());
        EXPECT_TRUE(model->Parts().empty());
    }
    {
        Fixture f(256, false);                                        // physics never initialised
        std::shared_ptr<voxel::VoxelModel> model = f.Box({4, 4, 4});
        EXPECT_EQ(f.scene.CreateVoxelObject({.model = model}), kNoEntity);
        EXPECT_EQ(f.scene.EntityCount(), 0u);
        EXPECT_FALSE(model->IsBaked());                               // rejected before anything was allocated
        EXPECT_EQ(f.store.UsedCount(), 0u);
    }
    {
        Fixture f;                                                    // nothing solid: an object without a body
        voxel::VoxelVolume empty(glm::ivec3(4));
        const Entity entity = f.scene.CreateVoxelObject({.model = std::make_shared<voxel::VoxelModel>(std::move(empty), voxel::kWorldPalette)});
        EXPECT_NE(entity, kNoEntity);
        EXPECT_EQ(f.scene.TryGet<scene::RigidBody>(entity), nullptr);
        EXPECT_EQ(f.physics.BodyCount(), 0u);
    }
}

TEST(Scene, ModelWhoseBakeFailedCanBeUsedOnceTheStoreHasRoom) {
    Fixture f(2);
    const Entity blocker = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4})});          // takes one of the two slots
    ASSERT_NE(blocker, kNoEntity);
    const std::shared_ptr<voxel::VoxelModel> plank = f.Box({40, 4, 4});                     // a shared model that needs two
    EXPECT_EQ(f.scene.CreateVoxelObject({.model = plank}), kNoEntity);
    EXPECT_EQ(f.store.UsedCount(), 1u);                                                     // only the blocker's
    EXPECT_FALSE(plank->IsBaked());
    EXPECT_EQ(f.scene.EntityCount(), 1u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);

    f.scene.Destroy(blocker);
    const Entity first = f.scene.CreateVoxelObject({.model = plank});
    const Entity second = f.scene.CreateVoxelObject({.model = plank, .position = {0.0f, 3.0f, 0.0f}});
    ASSERT_NE(first, kNoEntity);
    ASSERT_NE(second, kNoEntity);
    EXPECT_TRUE(plank->IsBaked());
    EXPECT_EQ(plank->Parts().size(), 2u);                                                   // complete, not a leftover half
    EXPECT_EQ(f.store.UsedCount(), 2u);
    std::vector<graphics::InstanceDesc> instances;
    GatherInstances(f.scene, instances);
    EXPECT_EQ(instances.size(), 4u);
}

TEST(Scene, StaleAndNullEntitiesAreSafeEverywhere) {
    Fixture f;
    const Entity stale = f.scene.CreateVoxelObject({.model = f.Box({8, 8, 8}), .position = {0.0f, 1.0f, 0.0f}});
    ASSERT_NE(stale, kNoEntity);
    f.scene.Destroy(stale);
    // The index is recycled with a new version: the old id must not alias the new entity.
    const Entity reused = f.scene.CreateVoxelObject({.model = f.Box({8, 8, 8}), .position = {0.0f, 1.0f, 0.0f}});
    ASSERT_NE(reused, kNoEntity);
    EXPECT_NE(reused, stale);
    const uint32_t particlesBefore = f.particles.AliveCount();

    for (const Entity entity : {stale, kNoEntity}) {
        EXPECT_FALSE(f.scene.IsValid(entity));
        EXPECT_EQ(f.scene.TryGet<scene::Transform>(entity), nullptr);
        EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(entity), nullptr);
        EXPECT_EQ(std::as_const(f.scene).TryGet<scene::RigidBody>(entity), nullptr);
        f.scene.SetTransform(entity, {9.0f, 9.0f, 9.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        const scene::DamageResult result = f.scene.Damage(entity, glm::dvec3(0.4, 1.4, 0.4), 5.0f);
        EXPECT_EQ(result.removed, 0u);
        EXPECT_EQ(result.loose, 0u);
        EXPECT_FALSE(result.destroyed);
        EXPECT_TRUE(result.pieces.empty());
        f.scene.Destroy(entity);
        EXPECT_EQ(f.scene.EntityFromUserData(scene::ToUserData(entity)), kNoEntity);
    }

    // Nothing above touched the living entity.
    EXPECT_TRUE(f.scene.IsValid(reused));
    EXPECT_EQ(f.scene.EntityCount(), 1u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    EXPECT_EQ(f.particles.AliveCount(), particlesBefore);
    ExpectVecNear(f.scene.TryGet<scene::Transform>(reused)->current.position, {0.0f, 1.0f, 0.0f}, 0.0f);
    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(reused)->model->SolidCount(), 512u);

    // Per-frame systems and gathering with dead ids around.
    f.scene.BeginFrame();
    f.physics.Step(kStep);
    f.scene.SyncFromPhysics();
    f.scene.Update(kStep);
    std::vector<graphics::InstanceDesc> instances;
    GatherInstances(f.scene, instances);
    EXPECT_EQ(instances.size(), 1u);
}

// --- physics <-> entities ------------------------------------------------------------------------

TEST(Scene, BodiesMapBackToEntities) {
    Fixture f;
    // The first entity has the integral value 0, which is also the user data of foreign bodies.
    const Entity first = f.scene.CreateVoxelObject({.model = f.Box({10, 10, 10}), .position = {0.0f, 0.0f, 0.0f}, .motion = physics::MotionType::Static});
    const Entity second = f.scene.CreateVoxelObject({.model = f.Box({10, 10, 10}), .position = {5.0f, 0.0f, 0.0f}, .motion = physics::MotionType::Static});
    ASSERT_NE(first, kNoEntity);
    ASSERT_NE(second, kNoEntity);
    const physics::BodyId firstBody = f.scene.TryGet<scene::RigidBody>(first)->body;
    const physics::BodyId secondBody = f.scene.TryGet<scene::RigidBody>(second)->body;

    physics::BodyDesc foreign;                    // e.g. a terrain collider: user data 0
    foreign.shape = f.physics.CreateBoxShape(glm::vec3(0.5f));
    foreign.position = {10.0f, 0.5f, 0.5f};
    foreign.motion = physics::MotionType::Static;
    foreign.layer = physics::Layer::Static;
    const physics::BodyId foreignBody = f.physics.CreateBody(foreign);
    ASSERT_TRUE(foreignBody.IsValid());

    EXPECT_EQ(f.scene.EntityFromBody(firstBody), first);
    EXPECT_EQ(f.scene.EntityFromBody(secondBody), second);
    EXPECT_EQ(f.scene.EntityFromBody(foreignBody), kNoEntity);
    EXPECT_EQ(f.scene.EntityFromBody(physics::BodyId{}), kNoEntity);

    EXPECT_EQ(f.physics.GetUserData(firstBody), scene::ToUserData(first));
    EXPECT_EQ(scene::EntityFromUserData(scene::ToUserData(second)), second);
    EXPECT_EQ(f.scene.EntityFromUserData(scene::ToUserData(first)), first);
    EXPECT_EQ(f.scene.EntityFromUserData(0), kNoEntity);
    EXPECT_EQ(f.scene.EntityFromUserData(static_cast<uint64_t>(static_cast<uint32_t>(second))), kNoEntity);   // untagged
    EXPECT_EQ(scene::EntityFromUserData(scene::ToUserData(kNoEntity)), kNoEntity);

    // Ray casts report the user data of what they hit.
    physics::RayHit hit = f.physics.Raycast({5.5f, 5.0f, 0.5f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(f.scene.EntityFromUserData(hit.userData), second);
    EXPECT_EQ(f.scene.EntityFromBody(hit.body), second);
    hit = f.physics.Raycast({10.0f, 5.0f, 0.5f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(f.scene.EntityFromUserData(hit.userData), kNoEntity);

    // A dead entity no longer resolves, neither by body nor by user data.
    const uint64_t userData = scene::ToUserData(second);
    f.scene.Destroy(second);
    EXPECT_EQ(f.scene.EntityFromBody(secondBody), kNoEntity);
    EXPECT_EQ(f.scene.EntityFromUserData(userData), kNoEntity);
    EXPECT_FALSE(f.physics.Raycast({5.5f, 5.0f, 0.5f}, {0.0f, -1.0f, 0.0f}, 20.0f).hit);

    // A body the scene did not create never resolves, even if its user data names a living entity.
    physics::BodyDesc impostor = foreign;
    impostor.position = {20.0f, 0.5f, 0.5f};
    impostor.userData = scene::ToUserData(first);
    const physics::BodyId impostorBody = f.physics.CreateBody(impostor);
    ASSERT_TRUE(impostorBody.IsValid());
    EXPECT_EQ(f.scene.EntityFromUserData(f.physics.GetUserData(impostorBody)), first);      // the value alone does decode
    EXPECT_EQ(f.scene.EntityFromBody(impostorBody), kNoEntity);
    EXPECT_EQ(f.scene.EntityFromBody(firstBody), first);

    f.physics.DestroyBody(foreignBody);
    f.physics.DestroyBody(impostorBody);
}

TEST(Scene, QueryBodiesReturnsOverlappingEntitiesOnly) {
    Fixture f;
    const Entity nearOne = f.scene.CreateVoxelObject({.model = f.Box({10, 10, 10}), .position = {0.0f, 0.0f, 0.0f}});
    const Entity farOne = f.scene.CreateVoxelObject({.model = f.Box({10, 10, 10}), .position = {8.0f, 0.0f, 0.0f}});
    const Entity visual = f.scene.CreateVoxelObject({.model = f.Box({10, 10, 10}), .position = {0.0f, 0.0f, 0.0f}, .withBody = false});
    ASSERT_NE(visual, kNoEntity);

    std::vector<Entity> found{kNoEntity};        // appended to, not cleared
    f.scene.QueryBodies(math::AABB(glm::vec3(0.8f, 0.2f, 0.2f), glm::vec3(1.5f, 0.6f, 0.6f)), found);
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[1], nearOne);

    found.clear();
    f.scene.QueryBodies(math::AABB(glm::vec3(-1.0f), glm::vec3(10.0f)), found);
    EXPECT_EQ(found.size(), 2u);
    EXPECT_EQ(std::set<Entity>(found.begin(), found.end()), (std::set<Entity>{nearOne, farOne}));

    found.clear();
    f.scene.QueryBodies(math::AABB(glm::vec3(3.0f), glm::vec3(4.0f)), found);
    EXPECT_TRUE(found.empty());
}

TEST(Scene, SetTransformMovesTheBody) {
    Fixture f;
    const Entity entity = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {0.0f, 1.0f, 0.0f}, .linearVelocity = {1.0f, 0.0f, 0.0f}});
    const physics::BodyId body = f.scene.TryGet<scene::RigidBody>(entity)->body;
    const glm::quat turned = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));

    f.scene.SetTransform(entity, {2.0f, 3.0f, 4.0f}, turned, false);
    const scene::Transform* transform = f.scene.TryGet<scene::Transform>(entity);
    ExpectVecNear(transform->current.position, {2.0f, 3.0f, 4.0f}, 0.0f);
    ExpectVecNear(transform->previous.position, {0.0f, 1.0f, 0.0f}, 0.0f);          // shows up as motion
    EXPECT_FLOAT_EQ(transform->current.scale, kVoxel);
    const physics::BodyState state = f.physics.GetState(body);
    ExpectVecNear(state.position, {2.0f, 3.0f, 4.0f}, 1e-5f);
    EXPECT_NEAR(std::abs(glm::dot(state.rotation, turned)), 1.0f, 1e-5f);
    ExpectVecNear(state.linearVelocity, {1.0f, 0.0f, 0.0f}, 1e-5f);                 // velocities are kept

    f.scene.SetTransform(entity, {5.0f, 5.0f, 5.0f}, turned);                       // teleport
    ExpectVecNear(transform->previous.position, {5.0f, 5.0f, 5.0f}, 0.0f);
}

// --- frame protocol ------------------------------------------------------------------------------

TEST(Scene, PhysicsDrivesTransformsAndPreviousIsLastFrame) {
    Fixture f;
    const Entity falling = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {0.0f, 10.0f, 0.0f}});
    const Entity pinned = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {3.0f, 10.0f, 0.0f}, .motion = physics::MotionType::Static});
    const Entity visual = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {6.0f, 10.0f, 0.0f}, .withBody = false});

    for (int frame = 0; frame < 3; ++frame) {
        f.scene.BeginFrame();
        for (int step = 0; step < 10; ++step) {
            f.physics.Step(kStep);
            f.scene.SyncFromPhysics();
        }
    }
    const scene::Transform* transform = f.scene.TryGet<scene::Transform>(falling);
    const physics::BodyState state = f.physics.GetState(f.scene.TryGet<scene::RigidBody>(falling)->body);
    ExpectVecNear(transform->current.position, state.position, 0.0f);
    EXPECT_LT(transform->current.position.y, 9.0f);                                  // half a second of free fall
    // `previous` is the pose at the start of the frame, however many steps the frame had.
    EXPECT_GT(transform->previous.position.y, transform->current.position.y + 0.3f);
    EXPECT_LT(transform->previous.position.y, 10.0f);

    f.scene.BeginFrame();                                                            // a frame without steps: no motion
    ExpectVecNear(transform->previous.position, transform->current.position, 0.0f);

    ExpectVecNear(f.scene.TryGet<scene::Transform>(pinned)->current.position, {3.0f, 10.0f, 0.0f}, 1e-6f);
    ExpectVecNear(f.scene.TryGet<scene::Transform>(visual)->current.position, {6.0f, 10.0f, 0.0f}, 0.0f);
}

TEST(Scene, SyncFromPhysicsCopiesRotation) {
    Fixture f;
    const glm::quat start = glm::angleAxis(0.7f, glm::normalize(glm::vec3(0.2f, 1.0f, 0.1f)));
    const Entity tumbling = f.scene.CreateVoxelObject(
        {.model = f.Box({8, 4, 2}), .position = {0.0f, 10.0f, 0.0f}, .rotation = start, .angularVelocity = {1.0f, 3.0f, -2.0f}});
    ASSERT_NE(tumbling, kNoEntity);
    const physics::BodyId body = f.scene.TryGet<scene::RigidBody>(tumbling)->body;
    ExpectQuatNear(f.physics.GetState(body).rotation, start, 1e-5f);

    f.scene.BeginFrame();
    for (int step = 0; step < 20; ++step) {
        f.physics.Step(kStep);
        f.scene.SyncFromPhysics();
    }
    const scene::Transform* transform = f.scene.TryGet<scene::Transform>(tumbling);
    const physics::BodyState state = f.physics.GetState(body);
    ExpectQuatNear(transform->current.rotation, state.rotation, 1e-6f);
    ExpectVecNear(transform->current.position, state.position, 0.0f);
    EXPECT_LT(std::abs(glm::dot(transform->current.rotation, start)), 0.95f);       // it did turn
    ExpectQuatNear(transform->previous.rotation, start, 1e-5f);                     // the pose of the frame start
    EXPECT_FLOAT_EQ(transform->current.scale, kVoxel);
}

TEST(Scene, LifetimeExpiresAndLightsFade) {
    Fixture f;
    const Entity persistent = f.scene.CreateLight({.position = {1.0, 2.0, 3.0}, .color = {0.1f, 0.2f, 0.3f}, .intensity = 4.0f, .radius = 6.0f, .sourceRadius = 0.25f});
    const Entity flash = f.scene.CreateLight({.position = {0.0, 5.0, 0.0}, .intensity = 10.0f, .lifetime = 1.0f, .decay = 2.0f});
    const Entity steady = f.scene.CreateLight({.intensity = 3.0f, .lifetime = 0.5f});        // limited life, no fade
    EXPECT_EQ(f.scene.TryGet<scene::Lifetime>(persistent), nullptr);
    ASSERT_NE(f.scene.TryGet<scene::Lifetime>(flash), nullptr);
    EXPECT_EQ(f.scene.EntityCount(), 3u);

    f.scene.Update(0.25f);
    EXPECT_NEAR(f.scene.TryGet<scene::PointLight>(flash)->intensity, 10.0f * std::exp(-0.5f), 1e-4f);
    EXPECT_NEAR(f.scene.TryGet<scene::Lifetime>(flash)->remaining, 0.75f, 1e-6f);
    EXPECT_FLOAT_EQ(f.scene.TryGet<scene::PointLight>(steady)->intensity, 3.0f);
    EXPECT_FLOAT_EQ(f.scene.TryGet<scene::PointLight>(persistent)->intensity, 4.0f);

    f.scene.Update(0.25f);
    EXPECT_NEAR(f.scene.TryGet<scene::PointLight>(flash)->intensity, 10.0f * std::exp(-1.0f), 1e-4f);
    EXPECT_FALSE(f.scene.IsValid(steady));                                           // 0.5 s are up

    f.scene.Update(0.6f);
    EXPECT_FALSE(f.scene.IsValid(flash));
    EXPECT_TRUE(f.scene.IsValid(persistent));
    EXPECT_EQ(f.scene.EntityCount(), 1u);

    // A lifetime works on any entity: a voxel object that expires releases its body and slots.
    const Entity debris = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4})});
    f.scene.Registry().emplace<scene::Lifetime>(debris, 0.1f, 0.0f);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    f.scene.Update(0.2f);
    EXPECT_FALSE(f.scene.IsValid(debris));
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    EXPECT_EQ(f.store.UsedCount(), 0u);
}

// --- render data ---------------------------------------------------------------------------------

TEST(Scene, GatherLightsUsesTransformAndOffset) {
    Fixture f;
    const Entity light = f.scene.CreateLight({.position = {1.0, 2.0, 3.0}, .color = {0.1f, 0.2f, 0.3f}, .intensity = 4.0f, .radius = 6.0f, .sourceRadius = 0.25f});
    const Entity dark = f.scene.CreateLight({.intensity = 0.0f});
    ASSERT_NE(dark, kNoEntity);

    std::vector<graphics::PointLight> lights(1);          // appended to
    GatherLights(f.scene, lights);
    ASSERT_EQ(lights.size(), 2u);
    EXPECT_NEAR(lights[1].position.x, 1.0, 1e-6);
    EXPECT_NEAR(lights[1].position.y, 2.0, 1e-6);
    EXPECT_NEAR(lights[1].position.z, 3.0, 1e-6);
    ExpectVecNear(lights[1].color, {0.1f, 0.2f, 0.3f}, 0.0f);
    EXPECT_FLOAT_EQ(lights[1].intensity, 4.0f);
    EXPECT_FLOAT_EQ(lights[1].radius, 6.0f);
    EXPECT_FLOAT_EQ(lights[1].sourceRadius, 0.25f);

    // A lamp carried by an object: the offset is in metres in the entity's rotated frame.
    f.scene.Destroy(light);
    const Entity lamp = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {10.0f, 0.0f, 0.0f},
                                                   .rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)), .withBody = false});
    scene::PointLight carried;
    carried.offset = {1.0f, 0.5f, 0.0f};
    f.scene.Registry().emplace<scene::PointLight>(lamp, carried);
    lights.clear();
    GatherLights(f.scene, lights);
    ASSERT_EQ(lights.size(), 1u);
    ExpectVecNear(glm::vec3(lights[0].position), {10.0f, 0.5f, -1.0f}, 1e-5f);     // +X turned a quarter around +Y is -Z
}

TEST(Scene, GatherInstancesDescribesEveryPartOfVisibleObjects) {
    Fixture f;
    const voxel::PaletteId palette = f.materials.CreatePalette("props");
    voxel::Material paint;
    const uint8_t id = f.materials.Add(palette, paint, "paint");
    ASSERT_NE(id, 0);

    // 40 voxels long: two chunks, the second one only 8 voxels wide.
    voxel::VoxelVolume volume(glm::ivec3(40, 6, 5));
    volume.FillBox({2, 1, 1}, {40, 6, 4}, id);
    auto model = std::make_shared<voxel::VoxelModel>(std::move(volume), palette, "bar");
    const glm::quat rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(0.2f, 1.0f, 0.1f)));
    const glm::vec3 start(1.0f, 2.0f, 3.0f), moved(1.5f, 2.0f, 2.0f);
    const Entity entity = f.scene.CreateVoxelObject({.model = model, .position = start, .rotation = rotation, .withBody = false});
    ASSERT_NE(entity, kNoEntity);
    ASSERT_EQ(model->Parts().size(), 2u);

    f.scene.BeginFrame();
    f.scene.SetTransform(entity, moved, rotation, false);        // moved since the last frame

    std::vector<graphics::InstanceDesc> instances(1);            // appended to
    GatherInstances(f.scene, instances);
    ASSERT_EQ(instances.size(), 1u + model->Parts().size());

    math::Transform current;
    current.position = moved;
    current.rotation = rotation;
    current.scale = kVoxel;
    math::Transform previous = current;
    previous.position = start;
    for (size_t i = 0; i < model->Parts().size(); ++i) {
        const voxel::ModelPart& part = model->Parts()[i];
        const graphics::InstanceDesc& instance = instances[1 + i];
        const glm::mat4 offset = glm::translate(glm::mat4(1.0f), glm::vec3(part.offset));
        ExpectMatNear(instance.localToWorld, current.ToMatrix() * offset, 1e-5f);
        ExpectMatNear(instance.prevLocalToWorld, previous.ToMatrix() * offset, 1e-5f);
        EXPECT_EQ(instance.slot, part.slot);
        EXPECT_TRUE(f.store.IsAllocated(instance.slot));
        EXPECT_EQ(instance.boundsMin, part.boundsMin);
        EXPECT_EQ(instance.boundsMax, part.boundsMax);
        EXPECT_EQ(instance.palette, palette);
    }
    EXPECT_EQ(instances[1].boundsMin, glm::ivec3(2, 1, 1));
    EXPECT_EQ(instances[1].boundsMax, glm::ivec3(32, 6, 4));
    EXPECT_EQ(instances[2].boundsMin, glm::ivec3(0, 1, 1));
    EXPECT_EQ(instances[2].boundsMax, glm::ivec3(8, 6, 4));
    // The first voxel of the second chunk is 32 voxels along the object's X axis.
    ExpectVecNear(glm::vec3(instances[2].localToWorld[3]), moved + rotation * glm::vec3(3.2f, 0.0f, 0.0f), 1e-5f);

    // The next frame without movement has no motion left.
    f.scene.BeginFrame();
    instances.clear();
    GatherInstances(f.scene, instances);
    ASSERT_EQ(instances.size(), 2u);
    ExpectMatNear(instances[0].prevLocalToWorld, instances[0].localToWorld, 0.0f);

    // Hidden objects are not gathered; other objects are unaffected.
    const Entity other = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4})});
    ASSERT_NE(other, kNoEntity);
    f.scene.TryGet<scene::VoxelBody>(entity)->visible = false;
    instances.clear();
    GatherInstances(f.scene, instances);
    ASSERT_EQ(instances.size(), 1u);
    EXPECT_EQ(instances[0].palette, voxel::kWorldPalette);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 2u);                   // hidden still counts

    const Entity hidden = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .withBody = false, .visible = false});
    ASSERT_NE(hidden, kNoEntity);
    instances.clear();
    GatherInstances(f.scene, instances);
    EXPECT_EQ(instances.size(), 1u);
}

// --- destruction ---------------------------------------------------------------------------------

TEST(SceneDamage, SharedModelIsNeverModified) {
    Fixture f;
    const std::shared_ptr<voxel::VoxelModel> model = f.Box({8, 8, 8}, 0, "crate");
    const Entity hit = f.scene.CreateVoxelObject({.model = model, .position = {0.0f, 0.0f, 0.0f}});
    const Entity bystander = f.scene.CreateVoxelObject({.model = model, .position = {5.0f, 0.0f, 0.0f}});
    const std::vector<uint8_t> voxelsBefore = model->Volume().Data();
    const uint64_t revisionBefore = model->Revision();
    const uint32_t slotBefore = model->Parts()[0].slot;
    const long usersBefore = model.use_count();

    const scene::DamageResult result = f.scene.Damage(hit, glm::dvec3(0.0, 0.0, 0.0), 0.35f);     // bites off a corner
    EXPECT_GT(result.removed, 0u);
    EXPECT_FALSE(result.destroyed);
    EXPECT_TRUE(result.pieces.empty());

    EXPECT_EQ(model->Volume().Data(), voxelsBefore);
    EXPECT_EQ(model->Revision(), revisionBefore);
    EXPECT_EQ(model->SolidCount(), 512u);
    ASSERT_EQ(model->Parts().size(), 1u);
    EXPECT_EQ(model->Parts()[0].slot, slotBefore);
    EXPECT_EQ(model.use_count(), usersBefore - 1);               // the damaged entity let go of it

    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(bystander)->model, model);
    const std::shared_ptr<voxel::VoxelModel> damaged = f.scene.TryGet<scene::VoxelBody>(hit)->model;
    EXPECT_NE(damaged, model);
    EXPECT_EQ(damaged->SolidCount(), 512u - result.removed);
    EXPECT_TRUE(damaged->IsBaked());
    EXPECT_EQ(damaged->Name(), "crate");
    EXPECT_EQ(f.store.UsedCount(), 2u);
    EXPECT_EQ(f.physics.BodyCount(), 2u);

    // The damaged entity keeps its identity and has a new body that maps back to it.
    const scene::RigidBody* body = f.scene.TryGet<scene::RigidBody>(hit);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(f.scene.EntityFromBody(body->body), hit);
    EXPECT_EQ(f.scene.TryGet<scene::Name>(hit)->value, "crate");
}

TEST(SceneDamage, RemovingEverythingDestroysTheEntity) {
    Fixture f;
    const Entity entity = f.scene.CreateVoxelObject({.model = f.Box({6, 6, 6}), .position = {2.0f, 2.0f, 2.0f}});
    const Entity other = f.scene.CreateVoxelObject({.model = f.Box({6, 6, 6}), .position = {20.0f, 2.0f, 2.0f}});
    EXPECT_EQ(f.store.UsedCount(), 2u);

    const scene::DamageResult result = f.scene.Damage(entity, glm::dvec3(2.3, 2.3, 2.3), 2.0f);
    EXPECT_EQ(result.removed, 216u);
    EXPECT_EQ(result.loose, 0u);
    EXPECT_TRUE(result.destroyed);
    EXPECT_TRUE(result.pieces.empty());
    EXPECT_FALSE(f.scene.IsValid(entity));
    EXPECT_TRUE(f.scene.IsValid(other));
    EXPECT_EQ(f.scene.VoxelObjectCount(), 1u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    EXPECT_EQ(f.store.UsedCount(), 1u);
    EXPECT_GT(f.particles.AliveCount(), 0u);
}

TEST(SceneDamage, MissesAndImmuneEntitiesAreUntouched) {
    Fixture f;
    const std::shared_ptr<voxel::VoxelModel> model = f.Box({8, 8, 8});
    const Entity solid = f.scene.CreateVoxelObject({.model = model});
    const Entity visual = f.scene.CreateVoxelObject({.model = model, .withBody = false});
    const Entity light = f.scene.CreateLight({});
    const physics::BodyId body = f.scene.TryGet<scene::RigidBody>(solid)->body;

    EXPECT_EQ(f.scene.Damage(solid, glm::dvec3(5.0, 5.0, 5.0), 0.5f).removed, 0u);          // too far away
    EXPECT_EQ(f.scene.Damage(solid, glm::dvec3(0.4, 0.4, 0.4), 0.0f).removed, 0u);          // no radius
    EXPECT_EQ(f.scene.Damage(solid, glm::dvec3(0.4, 0.4, 0.4), -1.0f).removed, 0u);
    EXPECT_EQ(f.scene.Damage(visual, glm::dvec3(0.4, 0.4, 0.4), 1.0f).removed, 0u);         // no body: immune
    EXPECT_EQ(f.scene.Damage(light, glm::dvec3(0.0), 1.0f).removed, 0u);                    // not a voxel object

    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(solid)->model, model);
    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(visual)->model, model);
    EXPECT_EQ(f.scene.TryGet<scene::RigidBody>(solid)->body, body);
    EXPECT_TRUE(f.physics.IsValid(body));
    EXPECT_EQ(f.particles.AliveCount(), 0u);
    EXPECT_EQ(f.scene.EntityCount(), 3u);
}

TEST(SceneDamage, IndestructibleVoxelsStay) {
    Fixture f;
    voxel::VoxelVolume volume(glm::ivec3(6, 6, 6));
    volume.FillBox({0, 0, 0}, {6, 6, 6}, f.ids.wood);
    volume.FillBox({0, 0, 0}, {6, 2, 6}, f.ids.bedrock);
    const Entity entity = f.scene.CreateVoxelObject({.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette)});

    const scene::DamageResult result = f.scene.Damage(entity, glm::dvec3(0.3, 0.3, 0.3), 3.0f);
    EXPECT_EQ(result.removed, 144u);             // all the wood
    EXPECT_FALSE(result.destroyed);
    ASSERT_TRUE(f.scene.IsValid(entity));
    const std::shared_ptr<voxel::VoxelModel> left = f.scene.TryGet<scene::VoxelBody>(entity)->model;
    EXPECT_EQ(left->SolidCount(), 72u);
    EXPECT_EQ(left->Size(), glm::ivec3(6, 2, 6));

    // A second blast finds nothing it can remove and changes nothing.
    const physics::BodyId body = f.scene.TryGet<scene::RigidBody>(entity)->body;
    EXPECT_EQ(f.scene.Damage(entity, glm::dvec3(0.3, 0.3, 0.3), 3.0f).removed, 0u);
    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(entity)->model, left);
    EXPECT_EQ(f.scene.TryGet<scene::RigidBody>(entity)->body, body);
}

TEST(SceneDamage, CutBeamBecomesTwoEntitiesThatInheritMotion) {
    Fixture f;
    const glm::vec3 origin(1.0f, 5.0f, -2.0f);
    const glm::vec3 velocity(1.0f, 2.0f, 3.0f);
    const std::shared_ptr<voxel::VoxelModel> model = f.Box({24, 3, 3}, 0, "beam");
    const Entity beam = f.scene.CreateVoxelObject({.model = model, .position = origin, .linearVelocity = velocity});
    ASSERT_NE(beam, kNoEntity);
    f.scene.Registry().emplace<int>(beam, 42);                   // a game component on the entity

    // What the cut must leave: every voxel whose centre is outside the sphere.
    // (Centre and radius are chosen so that no voxel centre lies on the sphere itself.)
    const glm::vec3 centerVoxels(12.3f, 1.5f, 1.5f);
    const float radiusVoxels = 2.4f;
    VoxelSet expected;
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 24; ++x) {
                const glm::vec3 d = glm::vec3(x, y, z) + glm::vec3(0.5f) - centerVoxels;
                if (glm::dot(d, d) > radiusVoxels * radiusVoxels) expected.insert({x, y, z});
            }
    ASSERT_LT(expected.size(), 216u);

    const scene::DamageResult result = f.scene.Damage(beam, glm::dvec3(origin) + glm::dvec3(centerVoxels) * static_cast<double>(kVoxel), radiusVoxels * kVoxel);
    EXPECT_EQ(result.removed, 216u - expected.size());
    EXPECT_EQ(result.loose, 0u);
    EXPECT_FALSE(result.destroyed);
    ASSERT_EQ(result.pieces.size(), 1u);
    const Entity piece = result.pieces[0];
    ASSERT_TRUE(f.scene.IsValid(beam));
    ASSERT_TRUE(f.scene.IsValid(piece));
    EXPECT_NE(piece, beam);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 2u);
    EXPECT_EQ(f.physics.BodyCount(), 2u);

    // The two models together are exactly the remaining voxels, each where it was.
    const voxel::VoxelModel* keptModel = f.scene.TryGet<scene::VoxelBody>(beam)->model.get();
    const voxel::VoxelModel* pieceModel = f.scene.TryGet<scene::VoxelBody>(piece)->model.get();
    EXPECT_NE(keptModel, model.get());
    EXPECT_NE(pieceModel, keptModel);
    EXPECT_EQ(keptModel->SolidCount() + pieceModel->SolidCount(), 216u - result.removed);
    EXPECT_GE(keptModel->SolidCount(), pieceModel->SolidCount());        // the largest piece keeps the entity
    VoxelSet found;
    size_t duplicates = 0;
    CollectVoxels(f.scene, beam, origin, found, duplicates);
    CollectVoxels(f.scene, piece, origin, found, duplicates);
    EXPECT_EQ(duplicates, 0u);
    EXPECT_EQ(found, expected);

    // Both halves move like the beam did, and both bodies map to their entity.
    for (const Entity entity : {beam, piece}) {
        const scene::RigidBody* body = f.scene.TryGet<scene::RigidBody>(entity);
        ASSERT_NE(body, nullptr);
        EXPECT_EQ(body->motion, physics::MotionType::Dynamic);
        const physics::BodyState state = f.physics.GetState(body->body);
        ExpectVecNear(state.linearVelocity, velocity, 1e-4f);
        ExpectVecNear(state.angularVelocity, glm::vec3(0.0f), 1e-4f);
        EXPECT_EQ(f.scene.EntityFromBody(body->body), entity);
        ExpectVecNear(state.position, f.scene.TryGet<scene::Transform>(entity)->current.position, 1e-5f);
        ASSERT_NE(f.scene.TryGet<scene::Name>(entity), nullptr);
        EXPECT_EQ(f.scene.TryGet<scene::Name>(entity)->value, "beam");
    }
    // Other components stay with the entity that was damaged and are not copied to the piece.
    ASSERT_NE(f.scene.TryGet<int>(beam), nullptr);
    EXPECT_EQ(*f.scene.TryGet<int>(beam), 42);
    EXPECT_EQ(f.scene.TryGet<int>(piece), nullptr);

    // The original model is gone with its last user; destroying the halves frees everything.
    EXPECT_EQ(f.store.UsedCount(), 3u);          // the test's `model` + two halves
    f.scene.Destroy(beam);
    f.scene.Destroy(piece);
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    EXPECT_EQ(f.store.UsedCount(), 1u);
}

TEST(SceneDamage, CutRotatedBeamKeepsEveryVoxelInPlace) {
    Fixture f;
    math::Transform frame;                        // the pose of the beam: tilted about a skewed axis
    frame.position = {1.0f, 5.0f, -2.0f};
    frame.rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(0.2f, 1.0f, 0.1f)));
    frame.scale = kVoxel;
    const glm::vec3 velocity(-2.0f, 1.0f, 0.5f);
    const Entity beam = f.scene.CreateVoxelObject(
        {.model = f.Box({24, 3, 3}, 0, "beam"), .position = frame.position, .rotation = frame.rotation, .linearVelocity = velocity});
    ASSERT_NE(beam, kNoEntity);

    const glm::vec3 centerVoxels(12.3f, 1.5f, 1.5f);
    const float radiusVoxels = 2.4f;
    VoxelSet expected;
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 24; ++x) {
                const glm::vec3 d = glm::vec3(x, y, z) + glm::vec3(0.5f) - centerVoxels;
                if (glm::dot(d, d) > radiusVoxels * radiusVoxels) expected.insert({x, y, z});
            }

    // The sphere centre goes through the same rotation as the voxels.
    const scene::DamageResult result = f.scene.Damage(beam, glm::dvec3(frame.TransformPoint(centerVoxels)), radiusVoxels * kVoxel);
    EXPECT_EQ(result.removed, 216u - expected.size());
    EXPECT_FALSE(result.destroyed);
    ASSERT_EQ(result.pieces.size(), 1u);
    const Entity piece = result.pieces[0];

    VoxelSet found;
    size_t duplicates = 0;
    CollectVoxels(f.scene, beam, frame, found, duplicates);
    CollectVoxels(f.scene, piece, frame, found, duplicates);
    EXPECT_EQ(duplicates, 0u);
    EXPECT_EQ(found, expected);

    // The far half sits along the rotated beam axis, not along world x.
    const glm::vec3 between = f.scene.TryGet<scene::Transform>(piece)->current.position - f.scene.TryGet<scene::Transform>(beam)->current.position;
    const glm::vec3 axis = frame.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    EXPECT_GT(glm::length(between), 1.0f);
    EXPECT_NEAR(std::abs(glm::dot(glm::normalize(between), axis)), 1.0f, 1e-4f);

    for (const Entity entity : {beam, piece}) {
        const scene::Transform* transform = f.scene.TryGet<scene::Transform>(entity);
        const physics::BodyState state = f.physics.GetState(f.scene.TryGet<scene::RigidBody>(entity)->body);
        ExpectQuatNear(transform->current.rotation, frame.rotation, 1e-6f);
        ExpectQuatNear(transform->previous.rotation, frame.rotation, 1e-6f);
        ExpectQuatNear(state.rotation, frame.rotation, 1e-5f);
        ExpectVecNear(state.position, transform->current.position, 1e-5f);
        ExpectVecNear(state.linearVelocity, velocity, 1e-4f);
    }
}

TEST(SceneDamage, PiecesOfASpinningObjectKeepSpinningAndMotionVectors) {
    Fixture f;
    const glm::vec3 start(0.0f, 5.0f, 0.0f), moved(0.25f, 5.0f, 0.0f);
    const glm::vec3 spin(0.0f, 2.0f, 0.0f);
    const Entity beam = f.scene.CreateVoxelObject({.model = f.Box({24, 3, 3}), .position = start, .angularVelocity = spin});
    f.scene.BeginFrame();
    f.scene.SetTransform(beam, moved, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), false);        // moved since the last frame
    const physics::BodyId oldBody = f.scene.TryGet<scene::RigidBody>(beam)->body;
    const glm::vec3 centreOfMass = moved + glm::vec3(1.2f, 0.15f, 0.15f);

    const scene::DamageResult result = f.scene.Damage(beam, glm::dvec3(centreOfMass), 0.25f);
    ASSERT_EQ(result.pieces.size(), 1u);
    EXPECT_FALSE(f.physics.IsValid(oldBody));

    for (const Entity entity : {beam, result.pieces[0]}) {
        const scene::Transform* transform = f.scene.TryGet<scene::Transform>(entity);
        const std::shared_ptr<voxel::VoxelModel> model = f.scene.TryGet<scene::VoxelBody>(entity)->model;
        const physics::BodyState state = f.physics.GetState(f.scene.TryGet<scene::RigidBody>(entity)->body);
        ExpectVecNear(state.angularVelocity, spin, 1e-4f);
        // Linear velocity = velocity of the original body at the centre of the piece.
        const glm::vec3 centre = transform->current.TransformPoint(glm::vec3(model->Size()) * 0.5f);
        ExpectVecNear(state.linearVelocity, glm::cross(spin, centre - centreOfMass), 1e-3f);
        EXPECT_GT(glm::length(state.linearVelocity), 0.5f);
        // The pose drawn last frame is re-based, not reset: the motion vector survives the cut.
        ExpectVecNear(transform->current.position - transform->previous.position, moved - start, 1e-5f);
        EXPECT_FLOAT_EQ(transform->previous.scale, kVoxel);
    }
    // The halves are on either side of the cut.
    const float keptX = f.scene.TryGet<scene::Transform>(beam)->current.position.x;
    const float pieceX = f.scene.TryGet<scene::Transform>(result.pieces[0])->current.position.x;
    EXPECT_GT(std::abs(keptX - pieceX), 1.0f);
}

TEST(SceneDamage, DentedStaticObjectStaysStatic) {
    Fixture f;
    const Entity wall = f.scene.CreateVoxelObject({.model = f.Box({24, 3, 3}), .position = {0.0f, 5.0f, 0.0f}, .motion = physics::MotionType::Static});
    // A bite out of one end: voxels are removed but the rest is still one piece.
    const scene::DamageResult result = f.scene.Damage(wall, glm::dvec3(0.0, 5.15, 0.15), 0.24f);
    EXPECT_GT(result.removed, 0u);
    EXPECT_TRUE(result.pieces.empty());
    EXPECT_FALSE(result.destroyed);
    ASSERT_NE(f.scene.TryGet<scene::RigidBody>(wall), nullptr);
    EXPECT_EQ(f.scene.TryGet<scene::RigidBody>(wall)->motion, physics::MotionType::Static);
    const glm::vec3 placed = f.scene.TryGet<scene::Transform>(wall)->current.position;

    for (int i = 0; i < 30; ++i) {
        f.physics.Step(kStep);
        f.scene.SyncFromPhysics();
    }
    ExpectVecNear(f.scene.TryGet<scene::Transform>(wall)->current.position, placed, 1e-5f);
}

TEST(SceneDamage, StaticObjectCutApartBecomesDynamic) {
    Fixture f;
    const Entity wall = f.scene.CreateVoxelObject({.model = f.Box({24, 3, 3}), .position = {0.0f, 5.0f, 0.0f}, .motion = physics::MotionType::Static});
    // Cut off-centre: the long part (15 voxels) keeps the entity. The scene cannot know which
    // part was the anchored one, so neither is left hanging: both fall.
    const scene::DamageResult result = f.scene.Damage(wall, glm::dvec3(1.73, 5.15, 0.15), 0.24f);
    ASSERT_EQ(result.pieces.size(), 1u);
    EXPECT_EQ(f.scene.TryGet<scene::RigidBody>(wall)->motion, physics::MotionType::Dynamic);
    EXPECT_EQ(f.scene.TryGet<scene::RigidBody>(result.pieces[0])->motion, physics::MotionType::Dynamic);

    for (int i = 0; i < 30; ++i) {
        f.physics.Step(kStep);
        f.scene.SyncFromPhysics();
    }
    EXPECT_LT(f.scene.TryGet<scene::Transform>(wall)->current.position.y, 4.5f);
    EXPECT_LT(f.scene.TryGet<scene::Transform>(result.pieces[0])->current.position.y, 4.5f);
}

TEST(SceneDamage, WorksInAFullChunkStore) {
    Fixture f(1);                                 // exactly the one slot the object needs
    const Entity box = f.scene.CreateVoxelObject({.model = f.Box({8, 8, 8}), .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(box, kNoEntity);
    EXPECT_EQ(f.store.UsedCount(), 1u);

    // The old model gives its slot back before the damaged one is baked.
    const scene::DamageResult result = f.scene.Damage(box, glm::dvec3(0.0, 5.0, 0.0), 0.3f);
    EXPECT_GT(result.removed, 0u);
    EXPECT_TRUE(result.pieces.empty());
    ASSERT_TRUE(f.scene.IsValid(box));
    const std::shared_ptr<voxel::VoxelModel> model = f.scene.TryGet<scene::VoxelBody>(box)->model;
    ASSERT_NE(model, nullptr);
    EXPECT_TRUE(model->IsBaked());
    EXPECT_EQ(model->Parts().size(), 1u);
    EXPECT_EQ(model->SolidCount(), 512u - result.removed);
    EXPECT_EQ(f.store.UsedCount(), 1u);
    std::vector<graphics::InstanceDesc> instances;
    GatherInstances(f.scene, instances);
    EXPECT_EQ(instances.size(), 1u);              // still drawn
}

TEST(SceneDamage, PieceWithoutABodyIsDropped) {
    physics::PhysicsConfig config = SmallConfig();
    config.maxBodies = 1;                         // room for the beam, not for a second half
    Fixture f(256, true, {}, config);
    ASSERT_TRUE(f.initialized);
    const Entity beam = f.scene.CreateVoxelObject({.model = f.Box({24, 3, 3}), .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(beam, kNoEntity);

    const scene::DamageResult result = f.scene.Damage(beam, glm::dvec3(1.73, 5.15, 0.15), 0.24f);
    EXPECT_GT(result.removed, 0u);
    // The largest piece reuses the body the beam had; the other half cannot be simulated and is
    // dropped completely - no entity, no model, no chunk slot.
    EXPECT_FALSE(result.destroyed);
    EXPECT_TRUE(result.pieces.empty());
    ASSERT_TRUE(f.scene.IsValid(beam));
    EXPECT_EQ(f.scene.EntityCount(), 1u);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 1u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    EXPECT_EQ(f.store.UsedCount(), 1u);
    const scene::RigidBody* body = f.scene.TryGet<scene::RigidBody>(beam);
    ASSERT_NE(body, nullptr);
    EXPECT_TRUE(f.physics.IsValid(body->body));
    EXPECT_EQ(f.scene.EntityFromBody(body->body), beam);
    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(beam)->model->SolidCount(), 15u * 9u);

    f.scene.Destroy(beam);
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    EXPECT_EQ(f.store.UsedCount(), 0u);
}

TEST(SceneDamage, LooseFragmentsBecomeParticles) {
    // A block with a two-voxel nub hanging on one bridge voxel; the control has no nub.
    auto build = [](Fixture& f, bool withNub) {
        voxel::VoxelVolume volume(glm::ivec3(12, 3, 3));
        volume.FillBox({0, 0, 0}, {8, 3, 3}, f.ids.stone);
        volume.Set(8, 1, 1, f.ids.stone);
        if (withNub) volume.FillBox({9, 1, 1}, {11, 2, 2}, f.ids.stone);
        return f.scene.CreateVoxelObject({.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette),
                                          .position = {0.0f, 3.0f, 0.0f}});
    };
    const glm::dvec3 bridge(0.85, 3.15, 0.15);

    Fixture control;
    const Entity plain = build(control, false);
    const scene::DamageResult plainResult = control.scene.Damage(plain, bridge, 0.06f);
    EXPECT_EQ(plainResult.removed, 1u);
    EXPECT_EQ(plainResult.loose, 0u);
    const uint32_t carveParticles = control.particles.AliveCount();
    EXPECT_GT(carveParticles, 0u);

    Fixture f;
    const Entity entity = build(f, true);
    const scene::DamageResult result = f.scene.Damage(entity, bridge, 0.06f);
    EXPECT_EQ(result.removed, 1u);
    EXPECT_EQ(result.loose, 2u);                 // too small to be a piece
    EXPECT_TRUE(result.pieces.empty());
    EXPECT_FALSE(result.destroyed);
    EXPECT_GT(f.particles.AliveCount(), carveParticles);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 1u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(entity)->model->SolidCount(), 72u);
    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(entity)->model->Size(), glm::ivec3(8, 3, 3));

    // With a lower threshold the same nub is a piece of its own instead.
    scene::SceneConfig config;
    config.minPieceVoxels = 2;
    Fixture lenient(256, true, config);
    const Entity again = build(lenient, true);
    const scene::DamageResult split = lenient.scene.Damage(again, bridge, 0.06f);
    EXPECT_EQ(split.loose, 0u);
    ASSERT_EQ(split.pieces.size(), 1u);
    EXPECT_EQ(lenient.scene.TryGet<scene::VoxelBody>(split.pieces[0])->model->SolidCount(), 2u);
    ExpectVecNear(lenient.scene.TryGet<scene::Transform>(split.pieces[0])->current.position, {0.9f, 3.1f, 0.1f}, 1e-5f);
}

TEST(SceneDamage, OnlyFragmentsLeftDestroysTheEntity) {
    Fixture f;
    voxel::VoxelVolume volume(glm::ivec3(5, 1, 1));
    volume.FillBox({0, 0, 0}, {5, 1, 1}, f.ids.stone);
    const Entity entity = f.scene.CreateVoxelObject({.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette)});
    const scene::DamageResult result = f.scene.Damage(entity, glm::dvec3(0.25, 0.05, 0.05), 0.06f);
    EXPECT_EQ(result.removed, 1u);
    EXPECT_EQ(result.loose, 4u);
    EXPECT_TRUE(result.destroyed);
    EXPECT_FALSE(f.scene.IsValid(entity));
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    EXPECT_EQ(f.store.UsedCount(), 0u);
}

TEST(SceneDamage, SnapshotThenDamageHitsEveryObjectOnce) {
    // The pattern Engine::Explode uses: QueryBodies, then Damage each entry.
    Fixture f;
    for (int i = 0; i < 3; ++i) {
        ASSERT_NE(f.scene.CreateVoxelObject({.model = f.Box({24, 3, 3}), .position = {0.0f, 1.0f + static_cast<float>(i), 0.0f}}), kNoEntity);
    }
    const glm::dvec3 center(1.2, 2.15, 0.15);
    const float radius = 0.25f;
    std::vector<Entity> caught;
    f.scene.QueryBodies(math::AABB(glm::vec3(center) - glm::vec3(radius), glm::vec3(center) + glm::vec3(radius)), caught);
    ASSERT_EQ(caught.size(), 1u);                // only the middle beam is in range

    uint32_t removed = 0;
    for (const Entity entity : caught) removed += f.scene.Damage(entity, center, radius).removed;
    EXPECT_GT(removed, 0u);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 4u);
    EXPECT_EQ(f.physics.BodyCount(), 4u);

    // Damaging through a stale snapshot is harmless.
    f.scene.Clear();
    for (const Entity entity : caught) EXPECT_EQ(f.scene.Damage(entity, center, radius).removed, 0u);
    EXPECT_EQ(f.scene.EntityCount(), 0u);
}

// --- rubble ----------------------------------------------------------------------------------------

namespace {
    /// A 3^3 lump (27 voxels: a piece, but small) created as rubble.
    Entity Lump(Fixture& f, const glm::vec3& position) {
        return f.scene.CreateVoxelObject({.model = f.Box({3, 3, 3}, 0, "lump"), .position = position, .debris = true});
    }
}

TEST(SceneDebris, SmallPiecesSplitOffAreRubbleTheDamagedEntityIsNot) {
    Fixture f;
    const Entity beam = f.scene.CreateVoxelObject({.model = f.Box({24, 3, 3}), .position = {0.0f, 5.0f, 0.0f}});
    ASSERT_NE(beam, kNoEntity);
    EXPECT_EQ(f.scene.DebrisCount(), 0u);

    // Cut near one end: 27 voxels come off, 144 stay.
    const scene::DamageResult cut = f.scene.Damage(beam, glm::dvec3(0.55, 5.15, 0.15), 0.25f);
    ASSERT_EQ(cut.pieces.size(), 1u);
    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(cut.pieces[0])->model->SolidCount(), 27u);
    EXPECT_NE(f.scene.TryGet<scene::Debris>(cut.pieces[0]), nullptr);
    EXPECT_EQ(f.scene.TryGet<scene::Debris>(beam), nullptr);
    EXPECT_EQ(f.scene.DebrisCount(), 1u);

    // Cut in the middle of what is left: both halves are big, neither is rubble.
    const scene::DamageResult middle = f.scene.Damage(beam, glm::dvec3(1.6, 5.15, 0.15), 0.25f);
    ASSERT_EQ(middle.pieces.size(), 1u);
    EXPECT_GE(f.scene.TryGet<scene::VoxelBody>(middle.pieces[0])->model->SolidCount(), f.scene.Config().debrisBodyVoxels);
    EXPECT_EQ(f.scene.TryGet<scene::Debris>(middle.pieces[0]), nullptr);
    EXPECT_EQ(f.scene.DebrisCount(), 1u);
}

TEST(SceneDebris, DefaultsKeepTinyFragmentsOutOfTheScene) {
    const scene::SceneConfig defaults;
    EXPECT_GE(defaults.minPieceVoxels, 6u);                    // nothing of 1-5 voxels is ever an object
    EXPECT_GT(defaults.debrisBodyVoxels, defaults.minPieceVoxels);
    EXPECT_GT(defaults.debrisLifetime, 0.0f);
    EXPECT_GT(defaults.maxDebris, 0u);

    // A 12-voxel end of a beam is below the default piece size: particles, not an entity.
    Fixture f;
    voxel::VoxelVolume volume(glm::ivec3(24, 2, 2));
    volume.FillBox({0, 0, 0}, {24, 2, 2}, f.ids.stone);
    const Entity beam = f.scene.CreateVoxelObject({.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette),
                                                   .position = {0.0f, 5.0f, 0.0f}});
    const scene::DamageResult cut = f.scene.Damage(beam, glm::dvec3(0.4, 5.1, 0.1), 0.12f);
    EXPECT_GT(cut.removed, 0u);
    EXPECT_GT(cut.loose, 0u);
    EXPECT_LT(cut.loose, defaults.minPieceVoxels);
    EXPECT_TRUE(cut.pieces.empty());
    EXPECT_EQ(f.scene.VoxelObjectCount(), 1u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
}

TEST(SceneDebris, RubbleTooSmallToBeAPieceCrumblesInsteadOfBecomingAnEntity) {
    Fixture f;
    // What Engine::Explode does with a floating crumb of terrain.
    const Entity crumb = f.scene.CreateVoxelObject({.model = f.Box({2, 2, 1}, 0, "crumb"), .position = {0.0f, 2.0f, 0.0f}, .debris = true});
    EXPECT_EQ(crumb, kNoEntity);
    EXPECT_EQ(f.scene.EntityCount(), 0u);
    EXPECT_EQ(f.physics.BodyCount(), 0u);
    EXPECT_EQ(f.store.UsedCount(), 0u);
    EXPECT_GT(f.particles.AliveCount(), 0u);

    // The same model as an ordinary object is an ordinary object.
    const Entity pebble = f.scene.CreateVoxelObject({.model = f.Box({2, 2, 1}), .position = {0.0f, 2.0f, 0.0f}});
    ASSERT_NE(pebble, kNoEntity);
    EXPECT_EQ(f.scene.TryGet<scene::Debris>(pebble), nullptr);

    // Rubble that is a piece but small is marked; a big one is not, nor is static or body-less rubble.
    const Entity lump = Lump(f, {1.0f, 2.0f, 0.0f});
    ASSERT_NE(lump, kNoEntity);
    EXPECT_NE(f.scene.TryGet<scene::Debris>(lump), nullptr);
    const Entity boulder = f.scene.CreateVoxelObject({.model = f.Box({8, 8, 8}), .position = {3.0f, 2.0f, 0.0f}, .debris = true});
    EXPECT_EQ(f.scene.TryGet<scene::Debris>(boulder), nullptr);
    const Entity fixed = f.scene.CreateVoxelObject(
        {.model = f.Box({2, 2, 1}), .position = {5.0f, 2.0f, 0.0f}, .motion = physics::MotionType::Static, .debris = true});
    ASSERT_NE(fixed, kNoEntity);
    EXPECT_EQ(f.scene.TryGet<scene::Debris>(fixed), nullptr);
    EXPECT_EQ(f.scene.DebrisCount(), 1u);
}

TEST(SceneDebris, RubbleExpiresIntoParticlesAndReleasesEverything) {
    scene::SceneConfig config;
    config.debrisLifetime = 2.0f;
    Fixture f(256, true, config);
    const Entity lump = Lump(f, {0.0f, 2.0f, 0.0f});
    const Entity crate = f.scene.CreateVoxelObject({.model = f.Box({3, 3, 3}, 0, "crate"), .position = {2.0f, 2.0f, 0.0f}});   // small, not rubble
    ASSERT_NE(lump, kNoEntity);
    ASSERT_NE(crate, kNoEntity);
    ASSERT_EQ(f.physics.BodyCount(), 2u);
    const uint32_t slots = f.store.UsedCount();

    f.scene.Update(1.5f);
    EXPECT_TRUE(f.scene.IsValid(lump));
    EXPECT_NEAR(f.scene.TryGet<scene::Debris>(lump)->age, 1.5f, 1e-6f);
    EXPECT_EQ(f.particles.AliveCount(), 0u);

    f.scene.Update(0.6f);
    EXPECT_FALSE(f.scene.IsValid(lump));
    EXPECT_TRUE(f.scene.IsValid(crate));                        // a small object nobody called rubble stays
    EXPECT_EQ(f.scene.DebrisCount(), 0u);
    EXPECT_EQ(f.physics.BodyCount(), 1u);
    EXPECT_LT(f.store.UsedCount(), slots);
    EXPECT_GT(f.particles.AliveCount(), 0u);                    // it went in a puff

    f.scene.Update(1000.0f);
    EXPECT_TRUE(f.scene.IsValid(crate));

    // No bursts configured: it just disappears. No lifetime configured: it stays.
    scene::SceneConfig silent;
    silent.debrisLifetime = 1.0f;
    silent.debrisDespawnBursts = 0;
    Fixture quiet(256, true, silent);
    const Entity gone = Lump(quiet, {0.0f, 2.0f, 0.0f});
    quiet.scene.Update(1.5f);
    EXPECT_FALSE(quiet.scene.IsValid(gone));
    EXPECT_EQ(quiet.particles.AliveCount(), 0u);

    scene::SceneConfig forever;
    forever.debrisLifetime = 0.0f;
    Fixture keep(256, true, forever);
    const Entity kept = Lump(keep, {0.0f, 2.0f, 0.0f});
    keep.scene.Update(1.0e6f);
    EXPECT_TRUE(keep.scene.IsValid(kept));
}

TEST(SceneDebris, CapRemovesTheOldestFirst) {
    scene::SceneConfig config;
    config.maxDebris = 3;
    config.debrisLifetime = 100.0f;
    Fixture f(256, true, config);
    std::vector<Entity> lumps;
    for (int i = 0; i < 5; ++i) {
        lumps.push_back(Lump(f, {static_cast<float>(i), 2.0f, 0.0f}));
        ASSERT_NE(lumps.back(), kNoEntity);
        if (i < 2) f.scene.Update(0.1f);                        // 0 and 1 are older than the rest
    }
    EXPECT_EQ(f.scene.DebrisCount(), 5u);                       // the cap is applied by Update

    f.scene.Update(0.1f);
    EXPECT_EQ(f.scene.DebrisCount(), 3u);
    EXPECT_FALSE(f.scene.IsValid(lumps[0]));
    EXPECT_FALSE(f.scene.IsValid(lumps[1]));
    for (int i = 2; i < 5; ++i) EXPECT_TRUE(f.scene.IsValid(lumps[static_cast<size_t>(i)]));
    EXPECT_EQ(f.physics.BodyCount(), 3u);

    // Objects that are not rubble do not count.
    for (int i = 0; i < 4; ++i) ASSERT_NE(f.scene.CreateVoxelObject({.model = f.Box({3, 3, 3}), .position = {0.0f, 4.0f, static_cast<float>(i)}}), kNoEntity);
    f.scene.Update(0.1f);
    EXPECT_EQ(f.scene.DebrisCount(), 3u);
    EXPECT_EQ(f.scene.VoxelObjectCount(), 7u);
}

TEST(SceneDebris, RestingRubbleFarFromTheViewerGoesSooner) {
    scene::SceneConfig config;
    config.debrisLifetime = 100.0f;
    config.debrisFarDistance = 20.0f;
    config.debrisFarLifetime = 1.0f;
    Fixture f(256, true, config);

    const Entity nearResting = Lump(f, {0.0f, 2.0f, 0.0f});
    const Entity farResting = Lump(f, {50.0f, 2.0f, 0.0f});
    const Entity farMoving = Lump(f, {50.0f, 2.0f, 5.0f});      // bodies are created awake
    // "Resting": asleep or held by the terrain guard. Freezing is the quick way to get there.
    f.physics.SetFrozen(f.scene.TryGet<scene::RigidBody>(nearResting)->body, true);
    f.physics.SetFrozen(f.scene.TryGet<scene::RigidBody>(farResting)->body, true);

    // Without a viewer the rule is off.
    f.scene.Update(2.0f);
    EXPECT_EQ(f.scene.DebrisCount(), 3u);

    f.scene.SetViewer({0.0, 2.0, 0.0});
    f.scene.Update(0.1f);
    EXPECT_TRUE(f.scene.IsValid(nearResting));
    EXPECT_FALSE(f.scene.IsValid(farResting));
    EXPECT_TRUE(f.scene.IsValid(farMoving));                    // still flying: let it land first

    // Young rubble is given debrisFarLifetime before the rule applies.
    const Entity fresh = Lump(f, {60.0f, 2.0f, 0.0f});
    f.physics.SetFrozen(f.scene.TryGet<scene::RigidBody>(fresh)->body, true);
    f.scene.Update(0.5f);
    EXPECT_TRUE(f.scene.IsValid(fresh));
    f.scene.Update(0.6f);
    EXPECT_FALSE(f.scene.IsValid(fresh));

    // The viewer walks away from the near one.
    f.scene.SetViewer({0.0, 2.0, 40.0});
    f.scene.Update(0.1f);
    EXPECT_FALSE(f.scene.IsValid(nearResting));
}

TEST(SceneDebris, SmallBodiesDoNotTunnelThroughThinGround) {
    Fixture f;
    // A 0.1 m thick static slab, and a 0.2 m lump shot at it at 60 m/s: 1 m per step.
    const Entity slab = f.scene.CreateVoxelObject(
        {.model = f.Box({40, 1, 40}, f.ids.stone, "slab"), .position = {-2.0f, 0.0f, -2.0f}, .motion = physics::MotionType::Static});
    const Entity lump = f.scene.CreateVoxelObject(
        {.model = f.Box({2, 2, 2}, f.ids.stone, "shot"), .position = {0.0f, 5.6f, 0.0f}, .linearVelocity = {0.0f, -60.0f, 0.0f}});
    ASSERT_NE(slab, kNoEntity);
    ASSERT_NE(lump, kNoEntity);
    for (int i = 0; i < 120; ++i) {
        f.physics.Step(kStep);
        f.scene.SyncFromPhysics();
    }
    EXPECT_GT(f.scene.TryGet<scene::Transform>(lump)->current.position.y, 0.0f);
}

TEST(SceneDebris, ThinBodiesDoNotTunnelWhateverTheirVoxelCount) {
    // A 0.1 m thick static floor and a 0.1 m thick plate of 64 voxels - not "small" - falling
    // flat at 30 m/s: 0.5 m per step, five times its thickness. Without the thin-body rule
    // (thinBodyVoxels = 0) it is below the floor after one step and never comes back.
    for (const bool thinRule : {true, false}) {
        scene::SceneConfig config;
        if (!thinRule) config.thinBodyVoxels = 0;
        Fixture f(256, true, config);
        const Entity floor = f.scene.CreateVoxelObject(
            {.model = f.Box({40, 1, 40}, f.ids.stone, "floor"), .position = {-2.0f, 0.0f, -2.0f}, .motion = physics::MotionType::Static});
        const Entity plate = f.scene.CreateVoxelObject(
            {.model = f.Box({8, 1, 8}, f.ids.stone, "plate"), .position = {0.0f, 5.33f, 0.0f}, .linearVelocity = {0.0f, -30.0f, 0.0f}});
        ASSERT_NE(floor, kNoEntity);
        ASSERT_NE(plate, kNoEntity);
        ASSERT_GE(f.scene.TryGet<scene::VoxelBody>(plate)->model->SolidCount(), f.scene.Config().debrisBodyVoxels);
        for (int i = 0; i < 120; ++i) {
            f.physics.Step(kStep);
            f.scene.SyncFromPhysics();
        }
        EXPECT_EQ(f.scene.TryGet<scene::Transform>(plate)->current.position.y > 0.0f, thinRule);
    }
}

TEST(SceneDebris, RemnantOfADamagedObjectBecomesRubbleOnceItIsSmall) {
    for (const bool remnants : {true, false}) {
        scene::SceneConfig config;
        config.debrisRemnants = remnants;
        Fixture f(256, true, config);
        const Entity crate = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {0.0f, 5.0f, 0.0f}});
        const Entity fixed = f.scene.CreateVoxelObject(
            {.model = f.Box({4, 4, 4}), .position = {3.0f, 5.0f, 0.0f}, .motion = physics::MotionType::Static});
        ASSERT_NE(crate, kNoEntity);
        ASSERT_NE(fixed, kNoEntity);

        // A dent: 64 voxels minus a corner is still an object.
        ASSERT_GT(f.scene.Damage(crate, glm::dvec3(0.0, 5.0, 0.0), 0.12f).removed, 0u);
        ASSERT_GE(f.scene.TryGet<scene::VoxelBody>(crate)->model->SolidCount(), config.debrisBodyVoxels);
        EXPECT_EQ(f.scene.TryGet<scene::Debris>(crate), nullptr);

        // Half of it gone: what is left is a lump (two of the four layers, 32 voxels).
        ASSERT_GT(f.scene.Damage(crate, glm::dvec3(0.2, 5.4, 0.2), 0.3f).removed, 0u);
        ASSERT_GT(f.scene.Damage(fixed, glm::dvec3(3.2, 5.4, 0.2), 0.3f).removed, 0u);
        ASSERT_TRUE(f.scene.IsValid(crate));
        ASSERT_TRUE(f.scene.IsValid(fixed));
        const uint32_t left = f.scene.TryGet<scene::VoxelBody>(crate)->model->SolidCount();
        ASSERT_LT(left, config.debrisBodyVoxels);
        ASSERT_GE(left, config.minPieceVoxels);
        EXPECT_EQ(f.scene.TryGet<scene::Debris>(crate) != nullptr, remnants);
        EXPECT_EQ(f.scene.TryGet<scene::Debris>(fixed), nullptr);          // dented, still static: never rubble
        EXPECT_EQ(f.scene.DebrisCount(), remnants ? 1u : 0u);

        // Damaging rubble again keeps the one component (and its age).
        if (remnants) {
            f.scene.Update(1.0f);
            f.scene.Damage(crate, glm::dvec3(0.0, 5.0, 0.0), 0.12f);
            if (f.scene.IsValid(crate)) {
                ASSERT_NE(f.scene.TryGet<scene::Debris>(crate), nullptr);
                EXPECT_NEAR(f.scene.TryGet<scene::Debris>(crate)->age, 1.0f, 1e-6f);
            }
        }
    }
}

TEST(Scene, DestroyBelowRemovesWhatFellOutOfTheWorld) {
    Fixture f;
    const Entity above = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {0.0f, 5.0f, 0.0f}});
    const Entity straddling = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {2.0f, -50.2f, 0.0f}});
    const Entity below = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {4.0f, -80.0f, 0.0f}});
    const Entity fixedBelow = f.scene.CreateVoxelObject(
        {.model = f.Box({4, 4, 4}), .position = {6.0f, -90.0f, 0.0f}, .motion = physics::MotionType::Static});
    const Entity movedBelow = f.scene.CreateVoxelObject(
        {.model = f.Box({4, 4, 4}), .position = {10.0f, -90.0f, 0.0f}, .motion = physics::MotionType::Kinematic});
    const Entity light = f.scene.CreateLight({.position = {0.0, -500.0, 0.0}});
    const Entity figure = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {8.0f, -500.0f, 0.0f}, .withBody = false});

    // Resting down there: a crate asleep on a floor, then parked by the terrain guard ...
    const Entity floor = f.scene.CreateVoxelObject(
        {.model = f.Box({20, 2, 20}, f.ids.stone), .position = {19.0f, -100.0f, -1.0f}, .motion = physics::MotionType::Static});
    const Entity resting = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {20.0f, -99.79f, 0.0f}});
    // ... and one stopped by the guard in flight.
    const Entity stopped = f.scene.CreateVoxelObject({.model = f.Box({4, 4, 4}), .position = {30.0f, -80.0f, 0.0f}});
    ASSERT_NE(floor, kNoEntity);
    for (const Entity held : {above, straddling, below, stopped}) {                // keep these where they are meanwhile
        f.physics.SetFrozen(f.scene.TryGet<scene::RigidBody>(held)->body, true);
    }
    for (int i = 0; i < 240; ++i) f.physics.Step(kStep);
    f.scene.SyncFromPhysics();
    const physics::BodyId restingBody = f.scene.TryGet<scene::RigidBody>(resting)->body;
    ASSERT_FALSE(f.physics.GetState(restingBody).active);
    for (const Entity released : {above, straddling, below}) {                     // falling again
        f.physics.SetFrozen(f.scene.TryGet<scene::RigidBody>(released)->body, false);
    }
    EXPECT_TRUE(f.physics.GetState(f.scene.TryGet<scene::RigidBody>(stopped)->body).wantsToMove);

    for (const bool parked : {false, true}) {
        f.physics.SetFrozen(restingBody, parked);
        EXPECT_FALSE(f.physics.GetState(restingBody).wantsToMove);
        EXPECT_EQ(f.scene.DestroyBelow(-50.0f), parked ? 0u : 2u);
        EXPECT_TRUE(f.scene.IsValid(above));
        EXPECT_TRUE(f.scene.IsValid(straddling));               // its top is still above the line
        EXPECT_FALSE(f.scene.IsValid(below));                   // awake and below: it is falling
        EXPECT_FALSE(f.scene.IsValid(stopped));                 // was falling when the guard stopped it
        EXPECT_TRUE(f.scene.IsValid(fixedBelow));               // placed there: static ...
        EXPECT_TRUE(f.scene.IsValid(movedBelow));               // ... kinematic ...
        EXPECT_TRUE(f.scene.IsValid(floor));
        EXPECT_TRUE(f.scene.IsValid(resting));                  // ... or simply at rest, asleep or parked
        EXPECT_TRUE(f.scene.IsValid(light));                    // only bodies fall
        EXPECT_TRUE(f.scene.IsValid(figure));
    }
    // Once something sets it in motion down there, it is on its way out as well.
    f.physics.Activate(restingBody);
    EXPECT_EQ(f.scene.DestroyBelow(-50.0f), 1u);
    EXPECT_FALSE(f.scene.IsValid(resting));
}
