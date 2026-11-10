module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>

export module vortex.editor:view;

import vortex.math;
import vortex.graphics;

export namespace vortex::editor {

    // ---------------------------------------------------------------------------------------------
    //  Screen <-> world. No ImGui, no GPU: everything here is covered by tests/engine/EditorTests.cpp.
    //
    //  "Cursor" coordinates are pixels with (0,0) at the top-left corner of the viewport, x to the
    //  right and y DOWN - what ImGui (io.MousePos) and GLFW report. `viewportSize` is the size of
    //  the same area in the same units (io.DisplaySize). Positions are continuous: the centre of
    //  the screen is `viewportSize * 0.5`.
    //
    //  The mapping is the one the renderer uses for its primary rays (VxCameraRay in
    //  shaders/include/common.glsl, without the sub-pixel TAA jitter):
    //      ndc = cursor / viewportSize * 2 - 1
    //      dir = normalize(Forward + ndc.x * tan(fovY/2) * aspect * Right - ndc.y * tan(fovY/2) * Up)
    //  so what is under the cursor on screen is what the ray hits.
    //
    //  All functions sanitise the camera exactly like the renderer does: fovY clamped to
    //  [0.01, 3.0] rad (60 degrees if not finite), nearPlane >= 1e-4 (0.05 if not finite). A
    //  viewport with a non-positive or non-finite side counts as square (aspect 1), and cursor
    //  rays through it go straight ahead.
    // ---------------------------------------------------------------------------------------------

    /// @brief Far plane of the projection handed to ImGuizmo, in metres. Only the gizmo sees it;
    /// the renderer's own projection has no far plane.
    inline constexpr float kGizmoFarPlane = 2000.0f;

    /// @brief A ray in world space: double precision origin (world metres), unit direction.
    struct CursorRay {
        glm::dvec3 origin{0.0};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};     ///< Normalised.
        glm::dvec3 At(float distance) const { return origin + glm::dvec3(direction) * static_cast<double>(distance); }
    };

    /// @brief Width / height of a viewport; 1 if either side is not positive or not finite.
    float ViewAspect(const glm::vec2& viewportSize);

    /**
     * @brief World-space ray from the camera through a cursor position.
     * @details origin = camera.position. The centre of the viewport gives exactly
     * `camera.Forward()`; the middle of the top edge is `fovY / 2` above it (y is not flipped: a
     * smaller cursor y looks further up), the middle of the right edge is
     * `atan(tan(fovY / 2) * aspect)` to the right. Cursor positions outside the viewport are
     * extrapolated, not clamped.
     */
    CursorRay CursorToRay(const graphics::Camera& camera, const glm::vec2& cursor, const glm::vec2& viewportSize);

    /**
     * @brief View matrix for ImGuizmo: `glm::lookAt(eye, eye + Forward(), Up())` with
     * `eye = vec3(camera.position)`, i.e. plain single precision WORLD space.
     * @details World space (not the renderer's window-relative render space) because the gizmo
     * edits scene::Transform, whose positions are single precision world metres like the physics
     * bodies behind them; a frame that could move during a drag would also break ImGuizmo, which
     * remembers the drag start in the space of the matrices it was given.
     */
    glm::mat4 GizmoView(const graphics::Camera& camera);

    /**
     * @brief Conventional perspective projection for ImGuizmo (NOT the engine's projection).
     * @details ImGuizmo (see worldToPos / ComputeCameraRay in ImGuizmo.cpp) expects: clip-space y
     * UP, a finite depth range it can unproject at z = 0 and z = 1, positive clip z in front of
     * the camera ("behind camera" test) and `m[2][3] < 0` for a right-handed view space. The
     * engine's math::PerspectiveReverseZ flips y for Vulkan and has an infinite, reversed depth
     * range, so it must never be passed to ImGuizmo. This is the right-handed, y-up, depth 0
     * (near) .. 1 (far) matrix, written out so it does not depend on GLM configuration macros:
     * @code
     *   f = 1 / tan(fovY / 2)
     *   m[0][0] = f / aspect      m[1][1] = f
     *   m[2][2] = far / (near - far)            m[2][3] = -1
     *   m[3][2] = -(far * near) / (far - near)  everything else 0
     * @endcode
     * `near` = the camera's (sanitised) near plane; `farPlane` is raised to at least 2 x near.
     * GLM and ImGuizmo share the memory layout (column-major, 16 floats): pass
     * `glm::value_ptr(matrix)`.
     */
    glm::mat4 GizmoProjection(const graphics::Camera& camera, float aspect, float farPlane = kGizmoFarPlane);

    /**
     * @brief Projects a point to cursor coordinates exactly as ImGuizmo's worldToPos does:
     * `clip = projection * view * point`, `cursor = ((clip.x / clip.w) * 0.5 + 0.5,
     * 1 - ((clip.y / clip.w) * 0.5 + 0.5)) * viewportSize`.
     * @return nullopt if the point is not in front of the camera (`clip.w <= 1e-6`).
     * @details With GizmoView / GizmoProjection this is the inverse of CursorToRay: every point of
     * a cursor ray (beyond the near plane) projects back to that cursor position. That identity
     * is what guarantees the gizmo is drawn, and reacts, where the scene is rendered.
     */
    std::optional<glm::vec2> ProjectToCursor(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& point,
                                             const glm::vec2& viewportSize);

    /// @brief ProjectToCursor with GizmoView(camera) and GizmoProjection(camera, ViewAspect(viewportSize)).
    /// Used for overlays (brush preview, selection marker).
    std::optional<glm::vec2> WorldToCursor(const graphics::Camera& camera, const glm::dvec3& point, const glm::vec2& viewportSize);

    // ---------------------------------------------------------------------------------------------
    //  Gizmo <-> scene::Transform
    // ---------------------------------------------------------------------------------------------

    /**
     * @brief Model matrix handed to ImGuizmo::Manipulate for an entity: rotation and translation
     * only (unit scale), with the gizmo placed at a pivot inside the object.
     * @param transform The entity's `Transform::current` (position = model minimum corner for
     * voxel objects, uniform scale = voxel size).
     * @param pivotLocal Pivot in the transform's local units (voxels for voxel objects: pass half
     * the model size to put the gizmo at the centre; zero for lights).
     * @details matrix = translate(transform.TransformPoint(pivotLocal)) * mat4(rotation). The
     * scale is left out on purpose: ImGuizmo orthonormalises the matrix it works on, and voxel
     * objects never change scale.
     */
    glm::mat4 GizmoModelMatrix(const math::Transform& transform, const glm::vec3& pivotLocal);

    /**
     * @brief Inverse of GizmoModelMatrix: the transform whose pivot and rotation are those of
     * `gizmoMatrix` (as modified by ImGuizmo).
     * @param scale Scale of the resulting transform (the entity's unchanged scale).
     * @details The rotation is taken from the orthonormalised upper 3x3 (ImGuizmo may leave
     * numerical scale / skew behind) and is always a unit quaternion; position =
     * pivot - rotation * (pivotLocal * scale). `TransformFromGizmo(GizmoModelMatrix(t, p), p,
     * t.scale)` reproduces `t` up to rounding.
     */
    math::Transform TransformFromGizmo(const glm::mat4& gizmoMatrix, const glm::vec3& pivotLocal, float scale);

    // ---------------------------------------------------------------------------------------------
    //  Sun
    // ---------------------------------------------------------------------------------------------

    /// @brief Direction towards the sun as two angles (radians).
    struct SunAngles {
        float azimuth = 0.0f;       ///< Around +Y, same sense as Camera::yaw: 0 = towards -Z, +pi/2 = towards -X. Range (-pi, pi].
        float elevation = 0.0f;     ///< Above the horizon: 0 = horizon, +pi/2 = zenith, negative = below. Range [-pi/2, pi/2].
    };

    /// @brief Unit vector towards the sun: `(-sin(az) * cos(el), sin(el), -cos(az) * cos(el))`
    /// - the same formula as Camera::Forward() with yaw = azimuth, pitch = elevation.
    glm::vec3 SunDirectionFromAngles(const SunAngles& angles);

    /**
     * @brief Inverse of SunDirectionFromAngles for any non-zero vector (it need not be
     * normalised, graphics::SunLight::direction is not).
     * @details elevation = asin(y / length), azimuth = atan2(-x, -z). Straight up or down (no
     * horizontal part) and the zero vector have no azimuth: 0 is returned (zero vector:
     * elevation pi/2, the sun at the zenith).
     */
    SunAngles SunAnglesFromDirection(const glm::vec3& direction);

    // ---------------------------------------------------------------------------------------------
    //  Fly camera
    // ---------------------------------------------------------------------------------------------

    struct FlySettings {
        float speed = 8.0f;                  ///< Metres per second.
        float fastMultiplier = 4.0f;         ///< While `FlyInput::fast`.
        float lookSensitivity = 0.0025f;     ///< Radians per pixel of mouse movement.
        float pitchLimit = 1.5f;             ///< |pitch| never exceeds this (radians).
    };

    /// @brief One frame of fly-camera input, already separated from the device.
    struct FlyInput {
        glm::vec2 lookDelta{0.0f};      ///< Mouse movement in pixels (x right, y down); zero while not looking.
        glm::vec3 move{0.0f};           ///< x = right (D - A), y = world up (Space - Ctrl), z = forward (W - S); each in [-1, 1].
        bool fast = false;              ///< Shift.
    };

    /**
     * @brief Advances a free-flying camera by one frame.
     * @details Look first: `yaw -= lookDelta.x * sensitivity`, `pitch -= lookDelta.y *
     * sensitivity` clamped to +-pitchLimit (mouse right turns right, mouse up looks up - the
     * signs the game uses). Then move along the NEW orientation: `Right() * move.x + (0,1,0) *
     * move.y + Forward() * move.z`, normalised when longer than 1, times speed (x
     * fastMultiplier) x dt. A non-finite or negative dt moves nothing.
     */
    void ApplyFlyCamera(graphics::Camera& camera, const FlyInput& input, const FlySettings& settings, float dt);
}
