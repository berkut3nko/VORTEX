// vortex.editor: the logic behind the editor tools - cursor rays, the matrices handed to ImGuizmo,
// brush strokes, primitives, palettes, import, entity helpers and the editor's own state. Nothing
// here creates a window, a Vulkan device or an ImGui context.

#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.graphics;
import vortex.scene;
import vortex.editor;

using namespace vortex;
using namespace vortex::editor;
using scene::Entity;
using scene::kNoEntity;

namespace {
    constexpr int kGround = 40;             // flat world: voxels with y < 40 are solid
    constexpr float kVoxel = 0.5f;          // so the surface is at y = 20 m
    constexpr double kSurfaceY = kGround * 0.5;
    constexpr uint8_t kSurface = 2;
    constexpr uint8_t kRock = 3;
    constexpr uint8_t kPaint = 7;
    const glm::vec2 kViewport(1280.0f, 720.0f);

    physics::PhysicsConfig SmallConfig() {
        physics::PhysicsConfig config;
        config.maxBodies = 1024;
        config.maxBodyPairs = 4096;
        config.maxContactConstraints = 4096;
        config.threads = 1;
        return config;
    }

    /// The flat world of tests/core/WorldTests.cpp (0.5 m voxels, hard rock under one surface
    /// layer) plus the objects a scene needs. The scene is declared last: destroyed first.
    struct Fixture {
        explicit Fixture(bool withPhysics = true)
            : store(1024), jobs(3), particles(1024),
              scene(store, materials, physics, particles, scene::SceneConfig{.voxelSize = kVoxel}) {
            voxel::WorldConfig config;
            config.voxelSize = kVoxel;
            config.windowSize = {8, 8, 8};
            voxel::Material hard;
            hard.hardness = 10.0f;
            materials.Set(voxel::kWorldPalette, kSurface, voxel::Material{}, "surface");
            materials.Set(voxel::kWorldPalette, kRock, hard, "rock");
            materials.Set(voxel::kWorldPalette, kPaint, voxel::Material{}, "paint");
            world = std::make_unique<voxel::World>(config, store, jobs, voxel::CreateFlatGenerator(kGround, kSurface, kRock), &materials);
            world->LoadBlocking(glm::dvec3(0.0, kSurfaceY, 0.0));
            if (withPhysics) initialized = physics.Initialize(SmallConfig());
        }

        std::shared_ptr<voxel::VoxelModel> Box(const glm::ivec3& size, const std::string& name = "box") {
            voxel::VoxelVolume volume(size);
            volume.FillBox(glm::ivec3(0), size, kSurface);
            return std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, name);
        }

        voxel::ChunkStore store;
        JobSystem jobs;
        voxel::MaterialLibrary materials;
        std::unique_ptr<voxel::World> world;
        physics::PhysicsWorld physics;
        fx::ParticleSystem particles;
        scene::Scene scene;
        bool initialized = false;
    };

    graphics::Camera MakeCamera(float yaw, float pitch, float fovY = 1.0472f, const glm::dvec3& position = glm::dvec3(3.0, 14.0, -7.0)) {
        graphics::Camera camera;
        camera.position = position;
        camera.yaw = yaw;
        camera.pitch = pitch;
        camera.fovY = fovY;
        return camera;
    }

    void ExpectVecNear(const glm::vec3& actual, const glm::vec3& expected, float tolerance) {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }

    float AngleBetween(const glm::vec3& a, const glm::vec3& b) {
        return std::acos(glm::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f));
    }

    const std::array<glm::vec2, 5> kOrientations = {glm::vec2(0.0f, 0.0f), glm::vec2(0.6f, -0.35f), glm::vec2(-2.4f, 0.9f),
                                                    glm::vec2(3.1f, -1.2f), glm::vec2(1.57f, 0.2f)};
}

// --- cursor ray ----------------------------------------------------------------------------------

TEST(EditorView, CentreOfTheScreenLooksAlongForward) {
    for (const glm::vec2& o : kOrientations) {
        const graphics::Camera camera = MakeCamera(o.x, o.y);
        const CursorRay ray = CursorToRay(camera, kViewport * 0.5f, kViewport);
        EXPECT_EQ(ray.origin, camera.position);
        ExpectVecNear(ray.direction, camera.Forward(), 1e-6f);
        EXPECT_NEAR(glm::length(ray.direction), 1.0f, 1e-6f);
    }
    // The engine convention: yaw 0, pitch 0 looks down -Z.
    ExpectVecNear(CursorToRay(MakeCamera(0.0f, 0.0f), kViewport * 0.5f, kViewport).direction, glm::vec3(0.0f, 0.0f, -1.0f), 1e-6f);
}

TEST(EditorView, EdgesMatchTheFieldOfViewAndYIsNotFlipped) {
    for (const float fov : {0.6f, 1.0472f, 1.9f}) {
        for (const glm::vec2& o : kOrientations) {
            const graphics::Camera camera = MakeCamera(o.x, o.y, fov);
            const float tanHalf = std::tan(fov * 0.5f);
            const float aspect = kViewport.x / kViewport.y;

            // Cursor y = 0 is the TOP of the screen: the ray goes up by half the vertical fov.
            const CursorRay top = CursorToRay(camera, glm::vec2(kViewport.x * 0.5f, 0.0f), kViewport);
            EXPECT_NEAR(AngleBetween(top.direction, camera.Forward()), fov * 0.5f, 1e-4f);
            EXPECT_GT(glm::dot(top.direction, camera.Up()), 0.0f);
            EXPECT_NEAR(glm::dot(top.direction, camera.Right()), 0.0f, 1e-5f);

            const CursorRay bottom = CursorToRay(camera, glm::vec2(kViewport.x * 0.5f, kViewport.y), kViewport);
            EXPECT_NEAR(AngleBetween(bottom.direction, camera.Forward()), fov * 0.5f, 1e-4f);
            EXPECT_LT(glm::dot(bottom.direction, camera.Up()), 0.0f);

            // Horizontal half angle follows from the aspect ratio.
            const CursorRay right = CursorToRay(camera, glm::vec2(kViewport.x, kViewport.y * 0.5f), kViewport);
            EXPECT_NEAR(AngleBetween(right.direction, camera.Forward()), std::atan(tanHalf * aspect), 1e-4f);
            EXPECT_GT(glm::dot(right.direction, camera.Right()), 0.0f);
            EXPECT_NEAR(glm::dot(right.direction, camera.Up()), 0.0f, 1e-5f);

            const CursorRay left = CursorToRay(camera, glm::vec2(0.0f, kViewport.y * 0.5f), kViewport);
            EXPECT_LT(glm::dot(left.direction, camera.Right()), 0.0f);
        }
    }
}

TEST(EditorView, CornersAndArbitraryPointsMatchTheShaderRay) {
    const graphics::Camera camera = MakeCamera(0.6f, -0.35f);
    const float tanHalf = std::tan(camera.fovY * 0.5f);
    const float aspect = kViewport.x / kViewport.y;
    const std::array<glm::vec2, 7> cursors = {glm::vec2(0.0f, 0.0f), glm::vec2(kViewport.x, 0.0f), glm::vec2(0.0f, kViewport.y), kViewport,
                                              glm::vec2(200.5f, 611.25f), glm::vec2(1000.0f, 33.0f), glm::vec2(-50.0f, 800.0f)};
    for (const glm::vec2& cursor : cursors) {
        // VxCameraRay in shaders/include/common.glsl, without jitter (cameraRight / cameraUp are
        // pre-scaled by tan(fov / 2) there).
        const glm::vec2 ndc = cursor / kViewport * 2.0f - 1.0f;
        const glm::vec3 expected = glm::normalize(camera.Forward() + ndc.x * tanHalf * aspect * camera.Right() - ndc.y * tanHalf * camera.Up());
        ExpectVecNear(CursorToRay(camera, cursor, kViewport).direction, expected, 1e-6f);
    }
    // Top-left corner in the camera's own basis: left, up, forward.
    const glm::vec3 corner = CursorToRay(camera, glm::vec2(0.0f), kViewport).direction;
    const float inv = 1.0f / std::sqrt(1.0f + tanHalf * tanHalf * (1.0f + aspect * aspect));
    EXPECT_NEAR(glm::dot(corner, camera.Right()), -tanHalf * aspect * inv, 1e-5f);
    EXPECT_NEAR(glm::dot(corner, camera.Up()), tanHalf * inv, 1e-5f);
    EXPECT_NEAR(glm::dot(corner, camera.Forward()), inv, 1e-5f);
}

TEST(EditorView, DegenerateViewportsAndCamerasAreSafe) {
    const graphics::Camera camera = MakeCamera(0.6f, -0.35f);
    EXPECT_FLOAT_EQ(ViewAspect(glm::vec2(1280.0f, 720.0f)), 1280.0f / 720.0f);
    EXPECT_FLOAT_EQ(ViewAspect(glm::vec2(0.0f, 720.0f)), 1.0f);
    EXPECT_FLOAT_EQ(ViewAspect(glm::vec2(1280.0f, -1.0f)), 1.0f);
    EXPECT_FLOAT_EQ(ViewAspect(glm::vec2(std::nanf(""), 720.0f)), 1.0f);
    ExpectVecNear(CursorToRay(camera, glm::vec2(10.0f, 10.0f), glm::vec2(0.0f)).direction, camera.Forward(), 1e-6f);
    ExpectVecNear(CursorToRay(camera, glm::vec2(std::nanf(""), 10.0f), kViewport).direction, camera.Forward(), 1e-6f);

    // The field of view is sanitised like the renderer does (clamped to [0.01, 3] rad).
    graphics::Camera wide = camera;
    wide.fovY = 100.0f;
    const CursorRay top = CursorToRay(wide, glm::vec2(kViewport.x * 0.5f, 0.0f), kViewport);
    EXPECT_NEAR(AngleBetween(top.direction, wide.Forward()), 1.5f, 1e-3f);
    graphics::Camera broken = camera;
    broken.fovY = std::nanf("");
    const CursorRay fallback = CursorToRay(broken, glm::vec2(kViewport.x * 0.5f, 0.0f), kViewport);
    EXPECT_NEAR(AngleBetween(fallback.direction, broken.Forward()), 1.0472f * 0.5f, 1e-3f);
}

// --- the matrices handed to ImGuizmo -------------------------------------------------------------

TEST(EditorView, GizmoProjectionIsConventional) {
    const graphics::Camera camera = MakeCamera(0.0f, 0.0f);
    const float aspect = 16.0f / 9.0f;
    const glm::mat4 projection = GizmoProjection(camera, aspect);
    EXPECT_GT(projection[1][1], 0.0f);                 // y up - the engine's Vulkan projection is y down
    EXPECT_FLOAT_EQ(projection[2][3], -1.0f);          // right-handed: w = -z_view
    EXPECT_FLOAT_EQ(projection[1][1], 1.0f / std::tan(camera.fovY * 0.5f));
    EXPECT_FLOAT_EQ(projection[0][0], projection[1][1] / aspect);
    EXPECT_LT(math::PerspectiveReverseZ(camera.fovY, aspect, camera.nearPlane)[1][1], 0.0f);

    // Depth 0 at the near plane, 1 at the far plane, growing with distance (not reversed).
    const auto depth = [&projection](float distance) {
        const glm::vec4 clip = projection * glm::vec4(0.0f, 0.0f, -distance, 1.0f);
        return clip.z / clip.w;
    };
    EXPECT_NEAR(depth(camera.nearPlane), 0.0f, 1e-5f);
    EXPECT_NEAR(depth(kGizmoFarPlane), 1.0f, 1e-4f);
    EXPECT_LT(depth(1.0f), depth(10.0f));
    // What ImGuizmo's "behind the camera" test looks at: clip z of a point in front is positive.
    EXPECT_GT((projection * glm::vec4(0.0f, 0.0f, -5.0f, 1.0f)).z, 0.001f);

    // A far plane closer than the near plane is raised; a broken aspect counts as square.
    const glm::mat4 tiny = GizmoProjection(camera, aspect, 0.0f);
    EXPECT_TRUE(std::isfinite(tiny[2][2]));
    EXPECT_TRUE(std::isfinite(tiny[3][2]));
    EXPECT_FLOAT_EQ(GizmoProjection(camera, 0.0f)[0][0], GizmoProjection(camera, 1.0f)[0][0]);
}

TEST(EditorView, GizmoViewIsLookAtInWorldSpace) {
    for (const glm::vec2& o : kOrientations) {
        const graphics::Camera camera = MakeCamera(o.x, o.y);
        const glm::vec3 eye(camera.position);
        const glm::mat4 expected = glm::lookAt(eye, eye + camera.Forward(), camera.Up());
        const glm::mat4 view = GizmoView(camera);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) EXPECT_NEAR(view[c][r], expected[c][r], 1e-4f) << "column " << c << " row " << r;
        // A point straight ahead ends up on the negative view z axis.
        const glm::vec4 ahead = view * glm::vec4(eye + camera.Forward() * 3.0f, 1.0f);
        ExpectVecNear(glm::vec3(ahead), glm::vec3(0.0f, 0.0f, -3.0f), 1e-4f);
    }
}

TEST(EditorView, PointsOnTheCursorRayProjectBackToTheCursor) {
    const std::array<glm::vec2, 8> cursors = {glm::vec2(640.0f, 360.0f), glm::vec2(0.0f, 0.0f), kViewport, glm::vec2(1280.0f, 0.0f),
                                              glm::vec2(17.5f, 701.0f), glm::vec2(903.25f, 122.75f), glm::vec2(300.0f, 360.0f), glm::vec2(640.0f, 50.0f)};
    for (const glm::vec2& o : kOrientations) {
        for (const float fov : {0.7f, 1.0472f, 1.6f}) {
            const graphics::Camera camera = MakeCamera(o.x, o.y, fov);
            const glm::mat4 view = GizmoView(camera);
            const glm::mat4 projection = GizmoProjection(camera, ViewAspect(kViewport));
            for (const glm::vec2& cursor : cursors) {
                const CursorRay ray = CursorToRay(camera, cursor, kViewport);
                for (const float t : {0.5f, 3.0f, 40.0f, 150.0f}) {
                    const std::optional<glm::vec2> back = ProjectToCursor(view, projection, glm::vec3(ray.At(t)), kViewport);
                    ASSERT_TRUE(back.has_value());
                    EXPECT_NEAR(back->x, cursor.x, 0.25f) << "t " << t;
                    EXPECT_NEAR(back->y, cursor.y, 0.25f) << "t " << t;

                    const std::optional<glm::vec2> world = WorldToCursor(camera, ray.At(t), kViewport);
                    ASSERT_TRUE(world.has_value());
                    EXPECT_NEAR(world->x, cursor.x, 0.05f);
                    EXPECT_NEAR(world->y, cursor.y, 0.05f);
                }
            }
        }
    }
}

TEST(EditorView, ProjectionRejectsPointsBehindTheCameraAndWorksFarFromTheOrigin) {
    const graphics::Camera camera = MakeCamera(0.6f, -0.35f);
    const glm::mat4 view = GizmoView(camera);
    const glm::mat4 projection = GizmoProjection(camera, ViewAspect(kViewport));
    EXPECT_FALSE(ProjectToCursor(view, projection, glm::vec3(camera.position) - camera.Forward() * 2.0f, kViewport).has_value());
    EXPECT_FALSE(WorldToCursor(camera, camera.position - glm::dvec3(camera.Forward()) * 5.0, kViewport).has_value());

    // WorldToCursor works relative to the camera: exact a thousand kilometres out.
    const graphics::Camera far = MakeCamera(-2.4f, 0.4f, 1.0472f, glm::dvec3(1.0e6, 50.0, -2.0e6));
    const glm::vec2 cursor(222.0f, 555.0f);
    const CursorRay ray = CursorToRay(far, cursor, kViewport);
    const std::optional<glm::vec2> back = WorldToCursor(far, ray.At(12.0f), kViewport);
    ASSERT_TRUE(back.has_value());
    EXPECT_NEAR(back->x, cursor.x, 0.05f);
    EXPECT_NEAR(back->y, cursor.y, 0.05f);
}

TEST(EditorView, GizmoMatrixRoundTrip) {
    math::Transform transform;
    transform.position = glm::vec3(4.0f, 21.5f, -3.25f);
    transform.rotation = glm::normalize(glm::angleAxis(0.8f, glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f))));
    transform.scale = 0.1f;
    const glm::vec3 pivot(4.0f, 9.0f, 2.0f);        // half of an 8 x 18 x 4 voxel model

    const glm::mat4 matrix = GizmoModelMatrix(transform, pivot);
    // The gizmo sits on the pivot, with unit scale.
    ExpectVecNear(glm::vec3(matrix[3]), transform.TransformPoint(pivot), 1e-5f);
    for (int c = 0; c < 3; ++c) EXPECT_NEAR(glm::length(glm::vec3(matrix[c])), 1.0f, 1e-5f);

    const math::Transform back = TransformFromGizmo(matrix, pivot, transform.scale);
    ExpectVecNear(back.position, transform.position, 1e-5f);
    EXPECT_NEAR(std::abs(glm::dot(back.rotation, transform.rotation)), 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(back.scale, 0.1f);

    // Translating the gizmo moves the object by the same amount.
    glm::mat4 moved = matrix;
    moved[3] += glm::vec4(1.0f, -2.0f, 0.5f, 0.0f);
    ExpectVecNear(TransformFromGizmo(moved, pivot, transform.scale).position, transform.position + glm::vec3(1.0f, -2.0f, 0.5f), 1e-5f);

    // Rotating the gizmo turns the object about its pivot: the pivot stays where it was.
    const glm::quat turn = glm::angleAxis(1.1f, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 turned = glm::mat4_cast(turn * transform.rotation);
    turned[3] = matrix[3];
    const math::Transform rotated = TransformFromGizmo(turned, pivot, transform.scale);
    ExpectVecNear(rotated.TransformPoint(pivot), transform.TransformPoint(pivot), 1e-5f);
    EXPECT_NEAR(std::abs(glm::dot(rotated.rotation, glm::normalize(turn * transform.rotation))), 1.0f, 1e-5f);

    // Scale and skew left behind by ImGuizmo do not leak into the rotation.
    glm::mat4 skewed = matrix;
    skewed[0] *= 1.3f;
    skewed[1] += skewed[0] * 0.05f;
    skewed[2] *= 0.8f;
    const math::Transform cleaned = TransformFromGizmo(skewed, pivot, transform.scale);
    EXPECT_NEAR(glm::length(cleaned.rotation), 1.0f, 1e-5f);
    EXPECT_NEAR(std::abs(glm::dot(cleaned.rotation, transform.rotation)), 1.0f, 1e-3f);

    // A degenerate matrix gives the identity rotation instead of NaN.
    glm::mat4 zero(0.0f);
    zero[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
    const math::Transform safe = TransformFromGizmo(zero, glm::vec3(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(safe.rotation.w, 1.0f);
    ExpectVecNear(safe.position, glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f);
}

// --- sun -----------------------------------------------------------------------------------------

TEST(EditorView, SunAnglesRoundTrip) {
    for (const float azimuth : {-3.0f, -1.2f, 0.0f, 0.7f, 2.9f}) {
        for (const float elevation : {-1.2f, -0.3f, 0.0f, 0.5f, 1.4f}) {
            const glm::vec3 direction = SunDirectionFromAngles({azimuth, elevation});
            EXPECT_NEAR(glm::length(direction), 1.0f, 1e-6f);
            // The camera convention: azimuth = yaw, elevation = pitch.
            ExpectVecNear(direction, MakeCamera(azimuth, elevation).Forward(), 1e-6f);

            const SunAngles back = SunAnglesFromDirection(direction);
            EXPECT_NEAR(back.azimuth, azimuth, 1e-4f);
            EXPECT_NEAR(back.elevation, elevation, 1e-4f);
            // The length of the vector does not matter (SunLight::direction is not normalised).
            const SunAngles scaled = SunAnglesFromDirection(direction * 37.5f);
            EXPECT_NEAR(scaled.azimuth, azimuth, 1e-4f);
            EXPECT_NEAR(scaled.elevation, elevation, 1e-4f);
        }
    }
    // Direction -> angles -> direction for the game's sun.
    const glm::vec3 sun(0.45f, 0.7f, 0.35f);
    ExpectVecNear(SunDirectionFromAngles(SunAnglesFromDirection(sun)), glm::normalize(sun), 1e-5f);
    // Azimuth 0 is towards -Z, +90 degrees towards -X.
    ExpectVecNear(SunDirectionFromAngles({0.0f, 0.0f}), glm::vec3(0.0f, 0.0f, -1.0f), 1e-6f);
    ExpectVecNear(SunDirectionFromAngles({math::kPi * 0.5f, 0.0f}), glm::vec3(-1.0f, 0.0f, 0.0f), 1e-6f);
}

TEST(EditorView, SunAtTheZenithAndDegenerateDirections) {
    const SunAngles up = SunAnglesFromDirection(glm::vec3(0.0f, 5.0f, 0.0f));
    EXPECT_NEAR(up.elevation, math::kPi * 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(up.azimuth, 0.0f);
    const SunAngles down = SunAnglesFromDirection(glm::vec3(0.0f, -1.0f, 0.0f));
    EXPECT_NEAR(down.elevation, -math::kPi * 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(down.azimuth, 0.0f);
    const SunAngles zero = SunAnglesFromDirection(glm::vec3(0.0f));
    EXPECT_NEAR(zero.elevation, math::kPi * 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(zero.azimuth, 0.0f);
    ExpectVecNear(SunDirectionFromAngles(up), glm::vec3(0.0f, 1.0f, 0.0f), 1e-6f);
}

// --- fly camera ----------------------------------------------------------------------------------

TEST(EditorView, FlyCameraLooksAndMoves) {
    const FlySettings settings;
    graphics::Camera camera = MakeCamera(0.0f, 0.0f, 1.0472f, glm::dvec3(0.0));

    // Mouse to the right turns right (towards +X), mouse up looks up.
    ApplyFlyCamera(camera, {.lookDelta = {100.0f, -40.0f}}, settings, 0.016f);
    EXPECT_FLOAT_EQ(camera.yaw, -100.0f * settings.lookSensitivity);
    EXPECT_FLOAT_EQ(camera.pitch, 40.0f * settings.lookSensitivity);
    EXPECT_GT(camera.Forward().x, 0.0f);
    EXPECT_GT(camera.Forward().y, 0.0f);
    EXPECT_EQ(camera.position, glm::dvec3(0.0));

    // The pitch stops at the limit.
    ApplyFlyCamera(camera, {.lookDelta = {0.0f, -100000.0f}}, settings, 0.016f);
    EXPECT_FLOAT_EQ(camera.pitch, settings.pitchLimit);
    ApplyFlyCamera(camera, {.lookDelta = {0.0f, 100000.0f}}, settings, 0.016f);
    EXPECT_FLOAT_EQ(camera.pitch, -settings.pitchLimit);

    // Forward, right and up, at the configured speed.
    camera = MakeCamera(0.6f, -0.35f, 1.0472f, glm::dvec3(0.0));
    ApplyFlyCamera(camera, {.move = {0.0f, 0.0f, 1.0f}}, settings, 0.5f);
    ExpectVecNear(glm::vec3(camera.position), camera.Forward() * (settings.speed * 0.5f), 1e-5f);
    camera.position = glm::dvec3(0.0);
    ApplyFlyCamera(camera, {.move = {-1.0f, 0.0f, 0.0f}}, settings, 0.5f);
    ExpectVecNear(glm::vec3(camera.position), -camera.Right() * (settings.speed * 0.5f), 1e-5f);
    camera.position = glm::dvec3(0.0);
    ApplyFlyCamera(camera, {.move = {0.0f, 1.0f, 0.0f}, .fast = true}, settings, 0.25f);
    ExpectVecNear(glm::vec3(camera.position), glm::vec3(0.0f, settings.speed * settings.fastMultiplier * 0.25f, 0.0f), 1e-5f);

    // Diagonal movement is not faster.
    camera.position = glm::dvec3(0.0);
    ApplyFlyCamera(camera, {.move = {1.0f, 1.0f, 1.0f}}, settings, 1.0f);
    EXPECT_NEAR(glm::length(glm::vec3(camera.position)), settings.speed, 1e-4f);

    // A broken time step moves nothing.
    camera.position = glm::dvec3(1.0, 2.0, 3.0);
    ApplyFlyCamera(camera, {.move = {0.0f, 0.0f, 1.0f}}, settings, -1.0f);
    ApplyFlyCamera(camera, {.move = {0.0f, 0.0f, 1.0f}}, settings, std::nanf(""));
    EXPECT_EQ(camera.position, glm::dvec3(1.0, 2.0, 3.0));
}

// --- brush ---------------------------------------------------------------------------------------

TEST(EditorBrush, CenterSitsOnTheSurfaceWhenAddingAndInItWhenRemoving) {
    BrushSettings brush;
    brush.radius = 2.0f;
    const glm::dvec3 hit(1.0, kSurfaceY, 1.0);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    brush.mode = BrushMode::Remove;
    EXPECT_EQ(BrushCenter(brush, hit, up, kVoxel), hit);
    brush.mode = BrushMode::Add;
    EXPECT_NEAR(BrushCenter(brush, hit, up, kVoxel).y, kSurfaceY + 1.5, 1e-9);      // radius - one voxel
    EXPECT_NEAR(BrushCenter(brush, hit, up * 4.0f, kVoxel).y, kSurfaceY + 1.5, 1e-6);   // the normal is normalised
    brush.radius = 0.01f;                                                           // clamped to one voxel: not lifted at all
    EXPECT_NEAR(BrushCenter(brush, hit, up, kVoxel).y, kSurfaceY, 1e-9);
    brush.radius = 1000.0f;                                                         // clamped to the maximum
    EXPECT_NEAR(BrushCenter(brush, hit, up, kVoxel).y, kSurfaceY + static_cast<double>(kMaxBrushRadius - kVoxel), 1e-6);
}

TEST(EditorBrush, RemoveCarvesASphereRegardlessOfHardness) {
    Fixture f(false);
    BrushSettings brush;
    brush.mode = BrushMode::Remove;
    brush.shape = BrushShape::Sphere;
    brush.radius = 2.0f;                                    // 4 voxels
    const glm::dvec3 center(1.0, kSurfaceY, 1.0);           // on the surface, at a voxel corner (2, 40, 2)

    ASSERT_EQ(f.world->GetVoxel({2, kGround - 1, 2}), kSurface);
    ASSERT_EQ(f.world->GetVoxel({2, kGround - 3, 2}), kRock);
    const uint32_t changed = ApplyBrush(*f.world, brush, center);

    // Half a sphere of radius 4 voxels is below the surface: 2/3 pi r^3 = 134 voxels.
    EXPECT_GT(changed, 100u);
    EXPECT_LT(changed, 170u);
    EXPECT_EQ(f.world->GetVoxel({2, kGround - 1, 2}), 0);
    EXPECT_EQ(f.world->GetVoxel({2, kGround - 3, 2}), 0);            // rock has hardness 10: the editor does not care
    EXPECT_EQ(f.world->GetVoxel({2, kGround - 6, 2}), kRock);        // below the sphere
    EXPECT_EQ(f.world->GetVoxel({8, kGround - 1, 2}), kSurface);     // beside it
    // The same spot again changes nothing.
    EXPECT_EQ(ApplyBrush(*f.world, brush, center), 0u);
}

TEST(EditorBrush, AddBuildsOnTheSurfaceWithTheChosenMaterial) {
    Fixture f(false);
    BrushSettings brush;
    brush.mode = BrushMode::Add;
    brush.shape = BrushShape::Sphere;
    brush.radius = 1.5f;                                    // 3 voxels
    brush.material = kPaint;
    const glm::dvec3 hit(1.0, kSurfaceY, 1.0);
    const glm::dvec3 center = BrushCenter(brush, hit, glm::vec3(0.0f, 1.0f, 0.0f), kVoxel);   // 1 m above the surface

    const uint32_t changed = ApplyBrush(*f.world, brush, center);
    EXPECT_GT(changed, 100u);                                        // a sphere of 4/3 pi 3^3 = 113 voxels, give or take
    EXPECT_LT(changed, 150u);                                        // the lattice, less the cap that is already ground
    EXPECT_EQ(f.world->GetVoxel({2, kGround, 2}), kPaint);           // attached to the ground it was painted on
    EXPECT_EQ(f.world->GetVoxel({2, kGround + 2, 2}), kPaint);       // the centre
    EXPECT_EQ(f.world->GetVoxel({2, kGround + 4, 2}), kPaint);
    EXPECT_EQ(f.world->GetVoxel({2, kGround + 6, 2}), 0);            // above the sphere
    EXPECT_EQ(f.world->GetVoxel({7, kGround, 2}), 0);
    EXPECT_EQ(f.world->GetVoxel({2, kGround - 3, 2}), kRock);        // nothing below changed

    // Material 0 makes Add a no-op.
    brush.material = 0;
    EXPECT_EQ(ApplyBrush(*f.world, brush, center + glm::dvec3(10.0, 0.0, 0.0)), 0u);
    EXPECT_EQ(f.world->GetVoxel({22, kGround + 2, 2}), 0);
}

TEST(EditorBrush, BoxCoversTheRoundedVoxelRange) {
    Fixture f(false);
    BrushSettings brush;
    brush.mode = BrushMode::Add;
    brush.shape = BrushShape::Box;
    brush.radius = 1.0f;                                    // half edge: 2 voxels
    brush.material = kPaint;

    // Centre (4, 22, 4) m = voxel corner (8, 44, 8): voxels [6, 10) x [42, 46) x [6, 10).
    EXPECT_EQ(ApplyBrush(*f.world, brush, glm::dvec3(4.0, 22.0, 4.0)), 64u);
    EXPECT_EQ(f.world->GetVoxel({6, 42, 6}), kPaint);
    EXPECT_EQ(f.world->GetVoxel({9, 45, 9}), kPaint);
    EXPECT_EQ(f.world->GetVoxel({5, 42, 6}), 0);
    EXPECT_EQ(f.world->GetVoxel({10, 45, 9}), 0);
    EXPECT_EQ(f.world->GetVoxel({6, 46, 6}), 0);
    EXPECT_EQ(f.world->GetVoxel({6, 41, 6}), 0);

    // Remove takes the same range out again, the smallest box is one voxel.
    brush.mode = BrushMode::Remove;
    EXPECT_EQ(ApplyBrush(*f.world, brush, glm::dvec3(4.0, 22.0, 4.0)), 64u);
    brush.mode = BrushMode::Add;
    brush.radius = 0.0f;                                    // clamped to the voxel size
    EXPECT_EQ(ApplyBrush(*f.world, brush, glm::dvec3(-3.0, 23.0, -3.0)), 8u);

    // Chunks that are not loaded are skipped.
    brush.radius = 1.0f;
    EXPECT_EQ(ApplyBrush(*f.world, brush, glm::dvec3(10000.0, 22.0, 0.0)), 0u);
}

TEST(EditorBrush, StrokeSpacingStopsRepeatedApplications) {
    Fixture f(false);
    BrushSettings brush;
    brush.mode = BrushMode::Remove;
    brush.radius = 2.0f;
    brush.spacing = 0.5f;                                   // one application per metre
    BrushStroke stroke;
    const glm::dvec3 start(1.0, kSurfaceY, 1.0);

    const BrushResult first = StrokeBrush(*f.world, brush, stroke, start);
    EXPECT_TRUE(first.applied);
    EXPECT_GT(first.changed, 0u);
    EXPECT_TRUE(stroke.active);
    EXPECT_EQ(stroke.lastCenter, start);

    // Held on the spot, or moved less than the spacing: nothing happens and the stroke stays put.
    for (const double step : {0.0, 0.3, 0.99}) {
        const BrushResult held = StrokeBrush(*f.world, brush, stroke, start + glm::dvec3(step, 0.0, 0.0));
        EXPECT_FALSE(held.applied) << step;
        EXPECT_EQ(held.changed, 0u);
        EXPECT_EQ(stroke.lastCenter, start);
    }
    EXPECT_EQ(f.world->GetVoxel({12, kGround - 1, 2}), kSurface);

    // Far enough: applied, and the stroke moves on.
    const glm::dvec3 next = start + glm::dvec3(5.0, 0.0, 0.0);
    const BrushResult moved = StrokeBrush(*f.world, brush, stroke, next);
    EXPECT_TRUE(moved.applied);
    EXPECT_GT(moved.changed, 0u);
    EXPECT_EQ(stroke.lastCenter, next);
    EXPECT_EQ(f.world->GetVoxel({12, kGround - 1, 2}), 0);

    // An application that changes nothing still counts (one edit per spot, not one per frame).
    stroke = BrushStroke{};
    const BrushResult again = StrokeBrush(*f.world, brush, stroke, next);
    EXPECT_TRUE(again.applied);                             // a new stroke always applies
    EXPECT_EQ(again.changed, 0u);
    EXPECT_FALSE(StrokeBrush(*f.world, brush, stroke, next).applied);

    // The spacing never drops below one voxel.
    brush.radius = kVoxel;
    brush.spacing = 0.0f;
    stroke = BrushStroke{};
    EXPECT_TRUE(StrokeBrush(*f.world, brush, stroke, glm::dvec3(-10.0, kSurfaceY, -10.0)).applied);
    EXPECT_FALSE(StrokeBrush(*f.world, brush, stroke, glm::dvec3(-10.0 + 0.4, kSurfaceY, -10.0)).applied);
    EXPECT_TRUE(StrokeBrush(*f.world, brush, stroke, glm::dvec3(-10.0 + 0.6, kSurfaceY, -10.0)).applied);
}

TEST(EditorBrush, ARestingCursorAppliesOnceInsteadOfDrilling) {
    // Each application moves the surface under the cursor: with the distance between consecutive
    // hits a held button would re-apply every frame along the ray. StrokeBrushAlongRay measures
    // from where the applied ray meets the edited terrain instead.
    Fixture f(false);
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    const glm::vec3 oblique = glm::normalize(glm::vec3(0.3f, -0.5f, -0.8f));
    double x = 0.0;
    for (const BrushMode mode : {BrushMode::Remove, BrushMode::Add}) {
        for (const BrushShape shape : {BrushShape::Sphere, BrushShape::Box}) {
            for (const glm::vec3& direction : {down, oblique * 3.0f}) {       // need not be normalised
                BrushSettings brush;
                brush.mode = mode;
                brush.shape = shape;
                brush.radius = 1.5f;
                brush.material = kPaint;
                BrushStroke stroke;
                CursorRay ray;
                ray.origin = glm::dvec3(x - 24.0, kSurfaceY + 6.0, 4.0);
                ray.direction = direction;
                x += 12.0;                                                    // untouched ground for every case

                uint32_t applications = 0;
                for (int frame = 0; frame < 30; ++frame) {
                    const voxel::WorldRayHit before = f.world->Raycast(ray.origin, glm::normalize(direction), 100.0f);
                    ASSERT_TRUE(before.hit);
                    const BrushResult result = StrokeBrushAlongRay(*f.world, brush, stroke, ray, 100.0f);
                    if (frame == 0) {
                        ASSERT_TRUE(result.applied);
                        EXPECT_GT(result.changed, 0u);
                        ASSERT_TRUE(stroke.resting);
                        // The surface under the cursor did move, by more than the spacing.
                        EXPECT_GT(glm::length(stroke.restPosition - before.position), 0.75);
                        EXPECT_LT(glm::length(stroke.lastCenter - BrushCenter(brush, before.position, before.normal, kVoxel)), 1e-4);
                    }
                    applications += result.applied ? 1u : 0u;
                }
                EXPECT_EQ(applications, 1u) << "mode " << static_cast<int>(mode) << " shape " << static_cast<int>(shape);

                // A cursor that moves on continues the stroke, a released button starts a new one.
                CursorRay beside = ray;
                beside.origin.z += 3.0;
                EXPECT_TRUE(StrokeBrushAlongRay(*f.world, brush, stroke, beside, 100.0f).applied);
                EXPECT_FALSE(StrokeBrushAlongRay(*f.world, brush, stroke, beside, 100.0f).applied);
                stroke = BrushStroke{};
                EXPECT_TRUE(StrokeBrushAlongRay(*f.world, brush, stroke, beside, 100.0f).applied);
            }
        }
    }

    // No terrain under the ray, no reach or no direction: nothing happens and the stroke is untouched.
    BrushSettings brush;
    BrushStroke stroke;
    CursorRay ray;
    ray.origin = glm::dvec3(0.0, kSurfaceY + 6.0, 0.0);
    ray.direction = glm::vec3(0.0f, 1.0f, 0.0f);
    EXPECT_FALSE(StrokeBrushAlongRay(*f.world, brush, stroke, ray, 100.0f).applied);
    ray.direction = down;
    EXPECT_FALSE(StrokeBrushAlongRay(*f.world, brush, stroke, ray, 2.0f).applied);      // the ground is 6 m away
    EXPECT_FALSE(StrokeBrushAlongRay(*f.world, brush, stroke, ray, 0.0f).applied);
    ray.direction = glm::vec3(0.0f);
    EXPECT_FALSE(StrokeBrushAlongRay(*f.world, brush, stroke, ray, 100.0f).applied);
    EXPECT_FALSE(stroke.active);
}

TEST(EditorBrush, StrokeIsContinuousAtGrazingAngles) {
    // Dragging the cursor away from a camera that is low above flat ground: the hit point travels
    // almost along the view ray. The applications must still follow each other closely enough to
    // overlap (centres less than a diameter apart), exactly like a sideways stroke.
    Fixture f(false);
    BrushSettings brush;
    brush.mode = BrushMode::Remove;
    brush.radius = 1.0f;
    brush.spacing = 0.5f;
    const double diameter = 2.0;

    const auto drag = [&](const glm::dvec3& eye, const glm::dvec3& from, const glm::dvec3& to) {
        BrushStroke stroke;
        std::vector<glm::dvec3> centers;
        const int steps = static_cast<int>(glm::length(to - from) / 0.05);
        for (int i = 0; i <= steps; ++i) {
            const glm::dvec3 target = glm::mix(from, to, static_cast<double>(i) / static_cast<double>(steps));
            CursorRay ray;
            ray.origin = eye;
            ray.direction = glm::vec3(glm::normalize(target - eye));
            if (StrokeBrushAlongRay(*f.world, brush, stroke, ray, 100.0f).applied) centers.push_back(stroke.lastCenter);
        }
        return centers;
    };

    // Away from the camera, 8 m to 24 m ahead of an eye 1.5 m above the ground (10.6 down to 3.6 degrees).
    const std::vector<glm::dvec3> away = drag(glm::dvec3(-20.0, kSurfaceY + 1.5, 20.0), glm::dvec3(-20.0, kSurfaceY, 12.0),
                                              glm::dvec3(-20.0, kSurfaceY, -4.0));
    ASSERT_GE(away.size(), 8u);                         // 16 m of travel with craters 2 m wide
    EXPECT_NEAR(away.front().z, 12.0, 0.01);
    EXPECT_LT(away.back().z, -4.0 + diameter);          // the stroke reaches its end
    for (size_t i = 1; i < away.size(); ++i) {
        EXPECT_LT(glm::length(away[i] - away[i - 1]), diameter) << "application " << i;
        EXPECT_LT(away[i].z, away[i - 1].z);            // and never goes back
    }

    // Sideways, seen from above: the same bound holds across the stroke (in depth the trench
    // gets deeper wherever an application lands inside the previous crater).
    const std::vector<glm::dvec3> sideways = drag(glm::dvec3(10.0, kSurfaceY + 10.0, 10.0), glm::dvec3(2.0, kSurfaceY, 10.0),
                                                  glm::dvec3(18.0, kSurfaceY, 10.0));
    ASSERT_GE(sideways.size(), 8u);
    for (size_t i = 1; i < sideways.size(); ++i) {
        EXPECT_LT(std::abs(sideways[i].x - sideways[i - 1].x), diameter) << "application " << i;
    }
    EXPECT_GT(sideways.back().x, 18.0 - diameter);
}

TEST(EditorBrush, CursorRayPaintsWhereItPoints) {
    // The whole chain a click goes through: cursor -> ray -> terrain hit -> brush.
    Fixture f(false);
    graphics::Camera camera = MakeCamera(0.3f, -0.9f, 1.0472f, glm::dvec3(0.0, kSurfaceY + 8.0, 0.0));
    const CursorRay ray = CursorToRay(camera, glm::vec2(800.0f, 300.0f), kViewport);
    const voxel::WorldRayHit hit = f.world->Raycast(ray.origin, ray.direction, 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.position.y, kSurfaceY, 1e-3);
    // The hit is on the ray, and it projects back to the cursor.
    const std::optional<glm::vec2> back = WorldToCursor(camera, hit.position, kViewport);
    ASSERT_TRUE(back.has_value());
    EXPECT_NEAR(back->x, 800.0f, 0.1f);
    EXPECT_NEAR(back->y, 300.0f, 0.1f);

    BrushSettings brush;
    brush.mode = BrushMode::Remove;
    brush.radius = 1.0f;
    ASSERT_TRUE(f.world->IsSolid(hit.voxel));
    EXPECT_GT(ApplyBrush(*f.world, brush, BrushCenter(brush, hit.position, hit.normal, kVoxel)), 0u);
    EXPECT_FALSE(f.world->IsSolid(hit.voxel));
}

// --- primitives ----------------------------------------------------------------------------------

TEST(EditorPrimitive, BoxIsSolid) {
    PrimitiveSettings settings;
    settings.shape = PrimitiveShape::Box;
    settings.size = {5, 3, 7};
    settings.material = 9;
    const voxel::VoxelVolume volume = BuildPrimitive(settings);
    EXPECT_EQ(volume.Size(), glm::ivec3(5, 3, 7));
    EXPECT_EQ(volume.CountSolid(), 105u);
    EXPECT_EQ(volume.Get(0, 0, 0), 9);
    EXPECT_EQ(volume.Get(4, 2, 6), 9);
}

TEST(EditorPrimitive, SphereIsSymmetricAndHasTheVolumeOfASphere) {
    PrimitiveSettings settings;
    settings.shape = PrimitiveShape::Sphere;
    settings.size = glm::ivec3(32);
    settings.material = 4;
    const voxel::VoxelVolume volume = BuildPrimitive(settings);
    ASSERT_EQ(volume.Size(), glm::ivec3(32));

    const double expected = 4.0 / 3.0 * 3.14159265358979 * 16.0 * 16.0 * 16.0;
    EXPECT_NEAR(static_cast<double>(volume.CountSolid()), expected, expected * 0.03);
    EXPECT_EQ(volume.Get(0, 0, 0), 0);                  // corners are outside
    EXPECT_EQ(volume.Get(16, 16, 16), 4);
    EXPECT_EQ(volume.Get(0, 16, 16), 4);                // the sphere touches every face
    EXPECT_EQ(volume.Get(16, 31, 16), 4);
    for (int z = 0; z < 32; ++z)
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const uint8_t id = volume.Get(x, y, z);
                ASSERT_EQ(id, volume.Get(31 - x, y, z));
                ASSERT_EQ(id, volume.Get(x, 31 - y, z));
                ASSERT_EQ(id, volume.Get(x, y, 31 - z));
            }

    // An ellipsoid: odd and uneven sizes stay symmetric, a one-voxel axis is a filled disc.
    settings.size = {9, 1, 5};
    const voxel::VoxelVolume disc = BuildPrimitive(settings);
    EXPECT_EQ(disc.Get(4, 0, 2), 4);
    EXPECT_EQ(disc.Get(0, 0, 2), 4);
    EXPECT_EQ(disc.Get(4, 0, 0), 4);
    EXPECT_EQ(disc.Get(0, 0, 0), 0);
    for (int z = 0; z < 5; ++z)
        for (int x = 0; x < 9; ++x) {
            ASSERT_EQ(disc.Get(x, 0, z), disc.Get(8 - x, 0, z));
            ASSERT_EQ(disc.Get(x, 0, z), disc.Get(x, 0, 4 - z));
        }
    EXPECT_NEAR(static_cast<double>(disc.CountSolid()), 3.14159 * 4.5 * 2.5, 6.0);
}

TEST(EditorPrimitive, SizesAreClampedAndTheSmallestIsOneVoxel) {
    PrimitiveSettings settings;
    settings.material = 2;
    for (const PrimitiveShape shape : {PrimitiveShape::Box, PrimitiveShape::Sphere}) {
        settings.shape = shape;
        settings.size = glm::ivec3(1);
        const voxel::VoxelVolume one = BuildPrimitive(settings);
        EXPECT_EQ(one.Size(), glm::ivec3(1));
        EXPECT_EQ(one.CountSolid(), 1u);

        settings.size = {0, -7, 100000};
        const voxel::VoxelVolume clamped = BuildPrimitive(settings);
        EXPECT_EQ(clamped.Size(), glm::ivec3(1, 1, kMaxPrimitiveSize));
        EXPECT_EQ(clamped.CountSolid(), static_cast<uint32_t>(kMaxPrimitiveSize));
    }
    // No material: the right size, nothing in it.
    settings.material = 0;
    settings.size = glm::ivec3(4);
    const voxel::VoxelVolume empty = BuildPrimitive(settings);
    EXPECT_EQ(empty.Size(), glm::ivec3(4));
    EXPECT_TRUE(empty.IsEmpty());
}

TEST(EditorPrimitive, PlaceOnSurface) {
    const glm::ivec3 size(8, 4, 2);                     // 0.8 x 0.4 x 0.2 m at 0.1 m voxels
    const glm::dvec3 anchor(10.0, 20.0, -5.0);
    // No normal: centred on the anchor.
    ExpectVecNear(PlaceOnSurface(size, 0.1f, anchor, glm::vec3(0.0f)), glm::vec3(9.6f, 19.8f, -5.1f), 1e-5f);
    // On the ground: the bottom face touches the surface.
    ExpectVecNear(PlaceOnSurface(size, 0.1f, anchor, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(9.6f, 20.0f, -5.1f), 1e-5f);
    // Against a wall facing -X: the +X face touches it; the normal's length does not matter.
    ExpectVecNear(PlaceOnSurface(size, 0.1f, anchor, glm::vec3(-3.0f, 0.0f, 0.0f)), glm::vec3(9.2f, 19.8f, -5.1f), 1e-5f);
    // Under a ceiling.
    ExpectVecNear(PlaceOnSurface(size, 0.1f, anchor, glm::vec3(0.0f, -1.0f, 0.0f)), glm::vec3(9.6f, 19.6f, -5.1f), 1e-5f);
    // A slope: lifted by the half extent along the normal, so no corner is inside the plane.
    const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
    const glm::vec3 corner = PlaceOnSurface(size, 0.1f, anchor, n);
    const glm::vec3 extent(0.8f, 0.4f, 0.2f);
    float lowest = 1.0e9f;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 p = corner + extent * glm::vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        lowest = std::min(lowest, glm::dot(p - glm::vec3(anchor), n));
    }
    EXPECT_NEAR(lowest, 0.0f, 1e-4f);
}

// --- materials -----------------------------------------------------------------------------------

TEST(EditorMaterials, ListsTheNamedEntriesOfAPalette) {
    voxel::MaterialLibrary library;
    EXPECT_TRUE(ListMaterials(library).empty());
    library.Set(voxel::kWorldPalette, 5, voxel::Material{}, "five");
    library.Set(voxel::kWorldPalette, 2, voxel::Material{}, "two");
    library.Set(voxel::kWorldPalette, 9, voxel::Material{});            // unnamed: not offered
    library.Set(voxel::kWorldPalette, 255, voxel::Material{}, "last");
    const std::vector<MaterialChoice> choices = ListMaterials(library);
    ASSERT_EQ(choices.size(), 3u);
    EXPECT_EQ(choices[0].id, 2);
    EXPECT_EQ(choices[0].name, "two");
    EXPECT_EQ(choices[1].id, 5);
    EXPECT_EQ(choices[2].id, 255);
    EXPECT_EQ(choices[2].name, "last");
    EXPECT_TRUE(ListMaterials(library, 42).empty());                    // no such palette

    const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(library);
    bool hasBrick = false;
    for (const MaterialChoice& choice : ListMaterials(library)) hasBrick |= choice.id == ids.brick;
    EXPECT_TRUE(hasBrick);
}

TEST(EditorMaterials, BuildsAPaletteFromImportedMaterials) {
    voxel::MaterialLibrary library;
    library.Set(voxel::kWorldPalette, 1, voxel::Material{.color = glm::vec3(0.1f, 0.2f, 0.3f)}, "ground");
    const uint32_t before = library.PaletteCount();

    std::vector<voxel::Material> materials(3);
    materials[0].color = glm::vec3(1.0f, 0.0f, 0.0f);
    materials[1].color = glm::vec3(0.0f, 1.0f, 0.0f);
    materials[1].roughness = 0.25f;
    materials[2].color = glm::vec3(0.0f, 0.0f, 1.0f);
    materials[2].emissive = 3.0f;

    const voxel::PaletteId palette = BuildImportPalette(library, materials, "robot");
    EXPECT_EQ(library.PaletteCount(), before + 1);
    EXPECT_NE(palette, voxel::kWorldPalette);
    EXPECT_EQ(library.PaletteName(palette), "robot");
    // materials[i] is voxel id i + 1, as voxel::MeshImportResult numbers them.
    EXPECT_EQ(library.Get(palette, 1).color, glm::vec3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(library.Get(palette, 2).color, glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_FLOAT_EQ(library.Get(palette, 2).roughness, 0.25f);
    EXPECT_EQ(library.Get(palette, 3).color, glm::vec3(0.0f, 0.0f, 1.0f));
    EXPECT_FLOAT_EQ(library.Get(palette, 3).emissive, 3.0f);
    EXPECT_EQ(library.Name(palette, 1), "robot 1");
    EXPECT_EQ(library.Name(palette, 3), "robot 3");
    EXPECT_TRUE(library.Name(palette, 4).empty());
    EXPECT_EQ(ListMaterials(library, palette).size(), 3u);
    // The GPU sees the colours at palette * 256 + id.
    const auto gpu = library.GpuData();
    ASSERT_EQ(gpu.size(), static_cast<size_t>(library.PaletteCount()) * voxel::kPaletteSize);
    EXPECT_FLOAT_EQ(gpu[voxel::MaterialLibrary::GlobalIndex(palette, 2)].colorEmissive.g, 1.0f);
    // The world palette is untouched.
    EXPECT_EQ(library.Get(voxel::kWorldPalette, 1).color, glm::vec3(0.1f, 0.2f, 0.3f));
    EXPECT_EQ(library.Name(voxel::kWorldPalette, 1), "ground");
    EXPECT_TRUE(library.Name(voxel::kWorldPalette, 2).empty());

    // More materials than a palette holds: the first 255 are kept.
    std::vector<voxel::Material> many(300);
    for (size_t i = 0; i < many.size(); ++i) many[i].color = glm::vec3(static_cast<float>(i));
    const voxel::PaletteId big = BuildImportPalette(library, many, "big");
    EXPECT_EQ(library.PaletteCount(), before + 2);
    EXPECT_EQ(library.Get(big, 1).color.r, 0.0f);
    EXPECT_EQ(library.Get(big, 255).color.r, 254.0f);
    EXPECT_EQ(ListMaterials(library, big).size(), 255u);
    EXPECT_EQ(library.Get(palette, 1).color, glm::vec3(1.0f, 0.0f, 0.0f));   // other palettes untouched

    // No materials: still a palette of its own.
    const voxel::PaletteId none = BuildImportPalette(library, {}, "empty");
    EXPECT_EQ(library.PaletteCount(), before + 3);
    EXPECT_TRUE(ListMaterials(library, none).empty());
}

// --- import --------------------------------------------------------------------------------------

TEST(EditorImport, VoxFileBecomesModelsWithTheirOwnPalette) {
    voxel::VoxFile file;
    file.palette[3] = glm::u8vec4(255, 0, 0, 255);
    file.palette[8] = glm::u8vec4(0, 0, 255, 255);
    voxel::VoxModel a;
    a.volume = voxel::VoxelVolume(glm::ivec3(4, 6, 2));
    a.volume.FillBox(glm::ivec3(0), glm::ivec3(4, 6, 2), 3);
    voxel::VoxModel hollow;                                     // no voxels: skipped
    hollow.volume = voxel::VoxelVolume(glm::ivec3(2));
    voxel::VoxModel b;
    b.volume = voxel::VoxelVolume(glm::ivec3(3, 3, 3));
    b.volume.Set(1, 1, 1, 8);
    file.models.push_back(std::move(a));
    file.models.push_back(std::move(hollow));
    file.models.push_back(std::move(b));

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vortex_editor_import_test.VOX";   // the extension is case-insensitive
    ASSERT_TRUE(voxel::SaveVox(path, file));

    voxel::MaterialLibrary library;
    const uint32_t before = library.PaletteCount();
    ImportSettings settings;
    settings.file = path;
    const ImportResult result = ImportModels(library, settings);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(library.PaletteCount(), before + 1);
    EXPECT_EQ(result.palette, before);
    ASSERT_EQ(result.models.size(), 2u);
    for (const ImportedModel& model : result.models) {
        ASSERT_NE(model.model, nullptr);
        EXPECT_EQ(model.model->Palette(), result.palette);
        EXPECT_FALSE(model.model->IsBaked());
        EXPECT_FALSE(model.model->Name().empty());
    }
    EXPECT_EQ(result.models[0].model->Size(), glm::ivec3(4, 6, 2));
    EXPECT_EQ(result.models[0].model->SolidCount(), 48u);
    EXPECT_EQ(result.models[0].model->Volume().Get(0, 0, 0), 3);
    EXPECT_EQ(result.models[0].offset, glm::ivec3(-2, -3, -1));          // translation (0) - size / 2 (VoxModelOffset)
    EXPECT_EQ(result.models[1].model->SolidCount(), 1u);
    EXPECT_EQ(result.models[1].model->Volume().Get(1, 1, 1), 8);
    EXPECT_EQ(result.models[1].offset, glm::ivec3(-1, -1, -2));          // odd depth on the mirrored axis: ceil
    // The file's colours are the palette of the models.
    EXPECT_GT(library.Get(result.palette, 3).color.r, 0.9f);
    EXPECT_LT(library.Get(result.palette, 3).color.b, 0.1f);
    EXPECT_GT(library.Get(result.palette, 8).color.b, 0.9f);
}

TEST(EditorImport, VoxModelsKeepTheirLayoutOnTheMirroredAxis) {
    // MagicaVoxel puts voxel floor(size / 2) of a model at its translation. Engine z is the
    // mirrored MagicaVoxel y, so there the minimum corner is translation - ceil(size / 2).
    EXPECT_EQ(VoxModelOffset(glm::ivec3(0), glm::ivec3(4, 6, 2)), glm::ivec3(-2, -3, -1));
    EXPECT_EQ(VoxModelOffset(glm::ivec3(0), glm::ivec3(3, 3, 3)), glm::ivec3(-1, -1, -2));
    EXPECT_EQ(VoxModelOffset(glm::ivec3(10, -4, 7), glm::ivec3(5, 1, 1)), glm::ivec3(8, -4, 6));

    // Two models that touch along MagicaVoxel y: A is 3 deep at y = 1 (voxels 0..2), B is 4 deep
    // at y = 5 (voxels 3..6). VoxModel::translation is (x, z, -y) and the engine depth is the
    // MagicaVoxel y size; in engine z they must be [-3, 0) and [-7, -3).
    const glm::ivec3 sizeA(2, 2, 3);
    const glm::ivec3 sizeB(2, 2, 4);
    const glm::ivec3 a = VoxModelOffset(glm::ivec3(0, 0, -1), sizeA);
    const glm::ivec3 b = VoxModelOffset(glm::ivec3(0, 0, -5), sizeB);
    EXPECT_EQ(a.z, -3);
    EXPECT_EQ(a.z + sizeA.z, 0);
    EXPECT_EQ(b.z, -7);
    EXPECT_EQ(b.z + sizeB.z, a.z);                      // no gap, no overlap
    // The same pair stacked along MagicaVoxel z (engine y, not mirrored): A at z = 1, B at z = 5.
    const glm::ivec3 upA = VoxModelOffset(glm::ivec3(0, 1, 0), glm::ivec3(2, 3, 2));
    const glm::ivec3 upB = VoxModelOffset(glm::ivec3(0, 5, 0), glm::ivec3(2, 4, 2));
    EXPECT_EQ(upA.y, 0);
    EXPECT_EQ(upA.y + 3, upB.y);
}

TEST(EditorImport, FailuresReportAnErrorAndCreateNoPalette) {
    voxel::MaterialLibrary library;
    const uint32_t before = library.PaletteCount();
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    ImportSettings settings;

    const auto expectFailure = [&](const std::filesystem::path& file) {
        settings.file = file;
        const ImportResult result = ImportModels(library, settings);
        EXPECT_FALSE(result.ok) << file.string();
        EXPECT_FALSE(result.error.empty()) << file.string();
        EXPECT_TRUE(result.models.empty());
        EXPECT_EQ(library.PaletteCount(), before) << file.string();
    };
    expectFailure({});                                              // nothing given
    expectFailure(temp / "vortex_editor_no_such_file.vox");
    expectFailure(temp / "vortex_editor_no_such_file.glb");
    expectFailure(temp / "vortex_editor_model.obj");                // unsupported type
    expectFailure(temp / "vortex_editor_model");                    // no extension

    // A .vox file without a single voxel.
    voxel::VoxFile file;
    voxel::VoxModel empty;
    empty.volume = voxel::VoxelVolume(glm::ivec3(2));
    file.models.push_back(std::move(empty));
    const std::filesystem::path path = temp / "vortex_editor_import_empty.vox";
    ASSERT_TRUE(voxel::SaveVox(path, file));
    expectFailure(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(EditorImport, GltfBecomesOneModelWithAPaletteFromItsMaterials) {
    const std::filesystem::path path = std::filesystem::path(VORTEX_TEST_SOURCE_DIR) / "models" / "frank.glb";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) GTEST_SKIP() << path.string() << " not present";

    voxel::MaterialLibrary library;
    const uint32_t before = library.PaletteCount();
    ImportSettings settings;
    settings.file = path;
    settings.voxelsPerUnit = 6.0f;
    settings.maxVoxelsPerAxis = 128;
    const ImportResult result = ImportModels(library, settings);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.models.size(), 1u);
    EXPECT_EQ(library.PaletteCount(), before + 1);
    EXPECT_EQ(result.models[0].model->Palette(), result.palette);
    EXPECT_EQ(result.models[0].offset, glm::ivec3(0));
    EXPECT_GT(result.models[0].model->SolidCount(), 0u);
    EXPECT_EQ(result.models[0].model->Name(), "frank");
    // Every voxel id of the model has an entry in the new palette.
    EXPECT_FALSE(ListMaterials(library, result.palette).empty());
    EXPECT_FALSE(library.Name(result.palette, 1).empty());

    // Too large for the limit: rejected, and no palette is left behind.
    settings.voxelsPerUnit = 200.0f;
    settings.maxVoxelsPerAxis = 8;
    const ImportResult rejected = ImportModels(library, settings);
    EXPECT_FALSE(rejected.ok);
    EXPECT_FALSE(rejected.error.empty());
    EXPECT_EQ(library.PaletteCount(), before + 1);
}

// --- picking and entities ------------------------------------------------------------------------

TEST(EditorTools, TraceSurfaceReturnsTheNearestOfTerrainAndEntities) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    // A 1 m cube (2 voxels of 0.5 m) resting 2 m above the ground, and one buried in it.
    const Entity crate = f.scene.CreateVoxelObject({.model = f.Box(glm::ivec3(2), "crate"),
                                                    .position = glm::vec3(0.0f, static_cast<float>(kSurfaceY) + 2.0f, 0.0f),
                                                    .motion = physics::MotionType::Static});
    const Entity buried = f.scene.CreateVoxelObject({.model = f.Box(glm::ivec3(2), "buried"),
                                                     .position = glm::vec3(10.0f, static_cast<float>(kSurfaceY) - 5.0f, 0.0f),
                                                     .motion = physics::MotionType::Static});
    ASSERT_NE(crate, kNoEntity);
    ASSERT_NE(buried, kNoEntity);
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    const double eye = kSurfaceY + 10.0;

    // Straight down onto the crate: its top is 3 m above the ground.
    const SurfaceHit onCrate = TraceSurface({glm::dvec3(0.5, eye, 0.5), down}, *f.world, f.physics, f.scene, 100.0f);
    ASSERT_TRUE(onCrate.hit);
    EXPECT_EQ(onCrate.entity, crate);
    EXPECT_NEAR(onCrate.distance, 7.0f, 0.05f);
    EXPECT_NEAR(onCrate.position.y, kSurfaceY + 3.0, 0.05);
    EXPECT_GT(onCrate.normal.y, 0.9f);

    // Beside it: the terrain, exact to the voxel.
    const SurfaceHit onGround = TraceSurface({glm::dvec3(4.3, eye, 0.5), down}, *f.world, f.physics, f.scene, 100.0f);
    ASSERT_TRUE(onGround.hit);
    EXPECT_EQ(onGround.entity, kNoEntity);
    EXPECT_NEAR(onGround.distance, 10.0f, 1e-3f);
    EXPECT_NEAR(onGround.position.y, kSurfaceY, 1e-3);
    EXPECT_EQ(onGround.normal, glm::vec3(0.0f, 1.0f, 0.0f));

    // An object behind the terrain is not picked through it.
    const SurfaceHit overBuried = TraceSurface({glm::dvec3(10.5, eye, 0.5), down}, *f.world, f.physics, f.scene, 100.0f);
    ASSERT_TRUE(overBuried.hit);
    EXPECT_EQ(overBuried.entity, kNoEntity);
    EXPECT_NEAR(overBuried.distance, 10.0f, 1e-3f);

    // Out of reach, no reach, no direction.
    EXPECT_FALSE(TraceSurface({glm::dvec3(0.5, eye, 0.5), down}, *f.world, f.physics, f.scene, 5.0f).hit);
    EXPECT_FALSE(TraceSurface({glm::dvec3(0.5, eye, 0.5), down}, *f.world, f.physics, f.scene, 0.0f).hit);
    EXPECT_FALSE(TraceSurface({glm::dvec3(0.5, eye, 0.5), glm::vec3(0.0f)}, *f.world, f.physics, f.scene, 100.0f).hit);
    // Into the sky.
    EXPECT_FALSE(TraceSurface({glm::dvec3(0.5, eye, 0.5), glm::vec3(0.0f, 1.0f, 0.0f)}, *f.world, f.physics, f.scene, 100.0f).hit);

    // Once the crate is gone the same ray reaches the ground.
    f.scene.Destroy(crate);
    const SurfaceHit after = TraceSurface({glm::dvec3(0.5, eye, 0.5), down}, *f.world, f.physics, f.scene, 100.0f);
    ASSERT_TRUE(after.hit);
    EXPECT_EQ(after.entity, kNoEntity);
    EXPECT_NEAR(after.distance, 10.0f, 1e-3f);
}

TEST(EditorTools, TraceSurfaceWorksWithoutPhysics) {
    Fixture f(false);
    const SurfaceHit hit = TraceSurface({glm::dvec3(0.5, kSurfaceY + 4.0, 0.5), glm::vec3(0.0f, -1.0f, 0.0f)}, *f.world, f.physics, f.scene, 50.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, kNoEntity);
    EXPECT_NEAR(hit.distance, 4.0f, 1e-3f);
}

TEST(EditorTools, CursorPicksTheEntityUnderIt) {
    // Looking straight at a crate: the ray through the screen centre selects it, and the crate's
    // centre projects to the screen centre - picking and the gizmo agree.
    Fixture f;
    ASSERT_TRUE(f.initialized);
    const glm::vec3 corner(6.0f, static_cast<float>(kSurfaceY) + 3.0f, -9.0f);
    const Entity crate = f.scene.CreateVoxelObject({.model = f.Box(glm::ivec3(2)), .position = corner, .motion = physics::MotionType::Static});
    const glm::dvec3 center = glm::dvec3(corner) + glm::dvec3(0.5);

    graphics::Camera camera;
    camera.position = glm::dvec3(0.0, kSurfaceY + 6.0, 0.0);
    const glm::dvec3 to = glm::normalize(center - camera.position);
    camera.pitch = static_cast<float>(std::asin(to.y));
    camera.yaw = static_cast<float>(std::atan2(-to.x, -to.z));

    const std::optional<glm::vec2> onScreen = WorldToCursor(camera, center, kViewport);
    ASSERT_TRUE(onScreen.has_value());
    EXPECT_NEAR(onScreen->x, kViewport.x * 0.5f, 0.5f);
    EXPECT_NEAR(onScreen->y, kViewport.y * 0.5f, 0.5f);
    EXPECT_EQ(TraceSurface(CursorToRay(camera, *onScreen, kViewport), *f.world, f.physics, f.scene, 200.0f).entity, crate);
    // A cursor well off the crate hits something else.
    EXPECT_NE(TraceSurface(CursorToRay(camera, glm::vec2(100.0f, 600.0f), kViewport), *f.world, f.physics, f.scene, 200.0f).entity, crate);

    // The gizmo matrix of the crate is centred on it.
    const scene::Transform* transform = f.scene.TryGet<scene::Transform>(crate);
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(GizmoPivot(f.scene, crate), glm::vec3(1.0f));
    ExpectVecNear(glm::vec3(GizmoModelMatrix(transform->current, GizmoPivot(f.scene, crate))[3]), glm::vec3(center), 1e-5f);
}

TEST(EditorTools, DescribeAndLabelEntities) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    const glm::quat rotation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    const Entity crate = f.scene.CreateVoxelObject({.model = f.Box(glm::ivec3(4, 2, 6), "crate"), .position = glm::vec3(1.0f, 30.0f, 2.0f),
                                                    .rotation = rotation, .motion = physics::MotionType::Kinematic});
    const Entity ghost = f.scene.CreateVoxelObject({.model = f.Box(glm::ivec3(2), ""), .position = glm::vec3(0.0f, 40.0f, 0.0f), .withBody = false});
    const Entity lamp = f.scene.CreateLight({.position = glm::dvec3(3.0, 25.0, 4.0)});

    const EntityInfo info = DescribeEntity(f.scene, crate);
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.name, "crate");
    EXPECT_TRUE(info.hasTransform);
    ExpectVecNear(info.position, glm::vec3(1.0f, 30.0f, 2.0f), 1e-6f);
    EXPECT_NEAR(std::abs(glm::dot(info.rotation, rotation)), 1.0f, 1e-6f);
    EXPECT_TRUE(info.hasVoxels);
    EXPECT_EQ(info.voxelCount, 48u);
    EXPECT_EQ(info.sizeVoxels, glm::ivec3(4, 2, 6));
    EXPECT_TRUE(info.hasBody);
    EXPECT_EQ(info.motion, physics::MotionType::Kinematic);
    EXPECT_FALSE(info.isLight);
    EXPECT_EQ(GizmoPivot(f.scene, crate), glm::vec3(2.0f, 1.0f, 3.0f));

    const EntityInfo visual = DescribeEntity(f.scene, ghost);
    EXPECT_TRUE(visual.valid);
    EXPECT_TRUE(visual.name.empty());
    EXPECT_TRUE(visual.hasVoxels);
    EXPECT_FALSE(visual.hasBody);

    const EntityInfo light = DescribeEntity(f.scene, lamp);
    EXPECT_TRUE(light.valid);
    EXPECT_TRUE(light.isLight);
    EXPECT_FALSE(light.hasVoxels);
    EXPECT_FALSE(light.hasBody);
    ExpectVecNear(light.position, glm::vec3(3.0f, 25.0f, 4.0f), 1e-6f);
    EXPECT_EQ(GizmoPivot(f.scene, lamp), glm::vec3(0.0f));

    EXPECT_EQ(EntityLabel(f.scene, crate), "crate #" + std::to_string(entt::to_entity(crate)));
    EXPECT_EQ(EntityLabel(f.scene, ghost), "object #" + std::to_string(entt::to_entity(ghost)));
    EXPECT_EQ(EntityLabel(f.scene, lamp), "light #" + std::to_string(entt::to_entity(lamp)));

    // Stale ids and kNoEntity describe nothing.
    f.scene.Destroy(crate);
    EXPECT_FALSE(DescribeEntity(f.scene, crate).valid);
    EXPECT_TRUE(DescribeEntity(f.scene, crate).name.empty());
    EXPECT_FALSE(DescribeEntity(f.scene, kNoEntity).valid);
    EXPECT_EQ(GizmoPivot(f.scene, crate), glm::vec3(0.0f));
    EXPECT_EQ(EntityLabel(f.scene, crate), "entity #" + std::to_string(entt::to_entity(crate)));
}

TEST(EditorTools, DuplicateSharesTheModelAndCopiesThePose) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    const std::shared_ptr<voxel::VoxelModel> model = f.Box(glm::ivec3(4, 2, 6), "crate");
    const glm::quat rotation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    const Entity crate = f.scene.CreateVoxelObject({.model = model, .position = glm::vec3(1.0f, 30.0f, 2.0f), .rotation = rotation,
                                                    .motion = physics::MotionType::Static});
    ASSERT_NE(crate, kNoEntity);
    ASSERT_EQ(model.use_count(), 2);
    const uint32_t slots = f.store.UsedCount();
    const uint32_t bodies = f.physics.BodyCount();

    const Entity copy = DuplicateEntity(f.scene, crate, glm::vec3(3.0f, 0.0f, 0.0f));
    ASSERT_NE(copy, kNoEntity);
    EXPECT_NE(copy, crate);
    EXPECT_EQ(model.use_count(), 3);                    // shared, not cloned
    EXPECT_EQ(f.store.UsedCount(), slots);              // so it costs no chunk slots
    EXPECT_EQ(f.physics.BodyCount(), bodies + 1);       // but it has a body of its own
    const EntityInfo info = DescribeEntity(f.scene, copy);
    EXPECT_EQ(info.name, "crate");
    ExpectVecNear(info.position, glm::vec3(4.0f, 30.0f, 2.0f), 1e-5f);
    EXPECT_NEAR(std::abs(glm::dot(info.rotation, rotation)), 1.0f, 1e-6f);
    EXPECT_TRUE(info.hasBody);
    EXPECT_EQ(info.motion, physics::MotionType::Static);
    EXPECT_EQ(f.scene.TryGet<scene::VoxelBody>(copy)->model, model);
    const scene::RigidBody* body = f.scene.TryGet<scene::RigidBody>(copy);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(f.scene.EntityFromBody(body->body), copy);

    // The original survives the copy being destroyed.
    f.scene.Destroy(copy);
    EXPECT_EQ(model.use_count(), 2);
    EXPECT_TRUE(f.scene.IsValid(crate));

    // A visual-only object stays visual-only (and keeps its visibility).
    const Entity ghost = f.scene.CreateVoxelObject({.model = model, .position = glm::vec3(0.0f, 40.0f, 0.0f), .withBody = false, .visible = false});
    const Entity ghostCopy = DuplicateEntity(f.scene, ghost, glm::vec3(0.0f, 1.0f, 0.0f));
    ASSERT_NE(ghostCopy, kNoEntity);
    EXPECT_EQ(f.scene.TryGet<scene::RigidBody>(ghostCopy), nullptr);
    EXPECT_FALSE(f.scene.TryGet<scene::VoxelBody>(ghostCopy)->visible);

    // Lights are copied as persistent lights with the same parameters.
    const Entity flash = f.scene.CreateLight({.position = glm::dvec3(3.0, 25.0, 4.0), .color = glm::vec3(0.2f, 0.4f, 0.6f), .intensity = 7.0f,
                                              .radius = 11.0f, .sourceRadius = 0.3f, .lifetime = 2.0f, .decay = 1.0f});
    const Entity lamp = DuplicateEntity(f.scene, flash, glm::vec3(0.0f, 0.0f, -2.0f));
    ASSERT_NE(lamp, kNoEntity);
    const scene::PointLight* light = f.scene.TryGet<scene::PointLight>(lamp);
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->color, glm::vec3(0.2f, 0.4f, 0.6f));
    EXPECT_FLOAT_EQ(light->intensity, 7.0f);
    EXPECT_FLOAT_EQ(light->radius, 11.0f);
    EXPECT_FLOAT_EQ(light->sourceRadius, 0.3f);
    EXPECT_EQ(f.scene.TryGet<scene::Lifetime>(lamp), nullptr);
    ExpectVecNear(DescribeEntity(f.scene, lamp).position, glm::vec3(3.0f, 25.0f, 2.0f), 1e-5f);

    // Nothing to copy: stale ids, kNoEntity.
    f.scene.Destroy(crate);
    const size_t count = f.scene.EntityCount();
    EXPECT_EQ(DuplicateEntity(f.scene, crate, glm::vec3(1.0f)), kNoEntity);
    EXPECT_EQ(DuplicateEntity(f.scene, kNoEntity, glm::vec3(1.0f)), kNoEntity);
    EXPECT_EQ(f.scene.EntityCount(), count);
}

// --- the editor's own state (no engine, no ImGui context) ----------------------------------------

TEST(EditorState, OpenToggleToolAndInputCapture) {
    Editor editor;
    EXPECT_FALSE(editor.IsOpen());
    EXPECT_FALSE(editor.CapturesInput());                   // a closed editor leaves the game alone
    EXPECT_EQ(editor.ActiveTool(), Tool::Select);
    EXPECT_TRUE(editor.Status().empty());
    EXPECT_FALSE(editor.StatusIsError());

    editor.SetOpen(true);
    EXPECT_TRUE(editor.IsOpen());
    EXPECT_TRUE(editor.CapturesInput());
    editor.Toggle();
    EXPECT_FALSE(editor.IsOpen());
    EXPECT_FALSE(editor.CapturesInput());
    editor.Toggle();
    EXPECT_TRUE(editor.CapturesInput());

    editor.SetTool(Tool::Brush);
    EXPECT_EQ(editor.ActiveTool(), Tool::Brush);
    editor.SetTool(static_cast<Tool>(kToolCount));          // not a tool: ignored
    EXPECT_EQ(editor.ActiveTool(), Tool::Brush);
    for (int i = 0; i < kToolCount; ++i) {
        editor.SetTool(static_cast<Tool>(i));
        EXPECT_EQ(editor.ActiveTool(), static_cast<Tool>(i));
    }

    // Settings are plain data the game may preset.
    editor.Settings().brush.radius = 3.0f;
    const Editor& constEditor = editor;
    EXPECT_FLOAT_EQ(constEditor.Settings().brush.radius, 3.0f);
    EXPECT_EQ(constEditor.Settings().toggleKey, Key::F1);
    EXPECT_TRUE(editor.Panels().tools);
}

TEST(EditorState, SelectionIsAnIdThatGoesStaleWithTheEntity) {
    Fixture f;
    ASSERT_TRUE(f.initialized);
    Editor editor;
    EXPECT_EQ(editor.Selection(f.scene), kNoEntity);

    const Entity crate = f.scene.CreateVoxelObject({.model = f.Box(glm::ivec3(2)), .position = glm::vec3(0.0f, 30.0f, 0.0f)});
    editor.Select(crate);
    EXPECT_EQ(editor.Selection(f.scene), crate);

    // Destroyed behind the editor's back: the selection reads as empty, nothing dangles.
    f.scene.Destroy(crate);
    EXPECT_EQ(editor.Selection(f.scene), kNoEntity);
    // Whatever reuses the index is a different entity and is not selected.
    const Entity other = f.scene.CreateVoxelObject({.model = f.Box(glm::ivec3(2)), .position = glm::vec3(0.0f, 30.0f, 0.0f)});
    EXPECT_EQ(editor.Selection(f.scene), kNoEntity);
    editor.Select(other);
    EXPECT_EQ(editor.Selection(f.scene), other);
    editor.Select(kNoEntity);
    EXPECT_EQ(editor.Selection(f.scene), kNoEntity);
}
