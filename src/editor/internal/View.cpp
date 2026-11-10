module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

module vortex.editor;

import vortex.math;
import vortex.graphics;

namespace vortex::editor {

    namespace {

        /// The renderer's SanitizeCamera (graphics/internal/Renderer.cpp): the editor has to agree
        /// with what is drawn.
        graphics::Camera Sanitize(graphics::Camera camera) {
            camera.fovY = std::isfinite(camera.fovY) ? std::clamp(camera.fovY, 0.01f, 3.0f) : 1.0472f;
            camera.nearPlane = std::isfinite(camera.nearPlane) ? std::max(camera.nearPlane, 1.0e-4f) : 0.05f;
            return camera;
        }

        bool ValidViewport(const glm::vec2& size) {
            return std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0f && size.y > 0.0f;
        }

        /// lookAt for an eye at `eye` with the camera's basis. Written out so that it is exactly
        /// the basis CursorToRay uses (rows: right, up, -forward).
        glm::mat4 ViewFromBasis(const graphics::Camera& camera, const glm::vec3& eye) {
            const glm::vec3 r = camera.Right();
            const glm::vec3 u = camera.Up();
            const glm::vec3 f = camera.Forward();
            glm::mat4 m(1.0f);
            m[0][0] = r.x;  m[1][0] = r.y;  m[2][0] = r.z;  m[3][0] = -glm::dot(r, eye);
            m[0][1] = u.x;  m[1][1] = u.y;  m[2][1] = u.z;  m[3][1] = -glm::dot(u, eye);
            m[0][2] = -f.x; m[1][2] = -f.y; m[2][2] = -f.z; m[3][2] = glm::dot(f, eye);
            return m;
        }
    }

    float ViewAspect(const glm::vec2& viewportSize) {
        if (!ValidViewport(viewportSize)) return 1.0f;
        const float aspect = viewportSize.x / viewportSize.y;
        return std::isfinite(aspect) && aspect > 0.0f ? aspect : 1.0f;
    }

    CursorRay CursorToRay(const graphics::Camera& camera, const glm::vec2& cursor, const glm::vec2& viewportSize) {
        const graphics::Camera cam = Sanitize(camera);
        CursorRay ray;
        ray.origin = cam.position;
        ray.direction = cam.Forward();
        if (!ValidViewport(viewportSize) || !std::isfinite(cursor.x) || !std::isfinite(cursor.y)) return ray;

        const glm::vec2 ndc = cursor / viewportSize * 2.0f - 1.0f;
        const float tanHalf = std::tan(cam.fovY * 0.5f);
        const glm::vec3 direction = cam.Forward() + cam.Right() * (ndc.x * tanHalf * ViewAspect(viewportSize)) - cam.Up() * (ndc.y * tanHalf);
        const float length = glm::length(direction);
        if (std::isfinite(length) && length > 0.0f) ray.direction = direction / length;
        return ray;
    }

    glm::mat4 GizmoView(const graphics::Camera& camera) {
        return ViewFromBasis(camera, glm::vec3(camera.position));
    }

    glm::mat4 GizmoProjection(const graphics::Camera& camera, float aspect, float farPlane) {
        const graphics::Camera cam = Sanitize(camera);
        if (!std::isfinite(aspect) || aspect <= 0.0f) aspect = 1.0f;
        const float zNear = cam.nearPlane;
        const float zFar = std::isfinite(farPlane) ? std::max(farPlane, 2.0f * zNear) : std::max(kGizmoFarPlane, 2.0f * zNear);
        const float f = 1.0f / std::tan(cam.fovY * 0.5f);

        glm::mat4 m(0.0f);
        m[0][0] = f / aspect;
        m[1][1] = f;                                  // y up: ImGuizmo flips it itself
        m[2][2] = zFar / (zNear - zFar);
        m[2][3] = -1.0f;                              // w = -z_view (right-handed)
        m[3][2] = -(zFar * zNear) / (zFar - zNear);
        return m;
    }

    std::optional<glm::vec2> ProjectToCursor(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& point,
                                             const glm::vec2& viewportSize) {
        const glm::vec4 clip = projection * (view * glm::vec4(point, 1.0f));
        if (!(clip.w > 1.0e-6f)) return std::nullopt;             // also rejects NaN
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 uv(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
        return uv * viewportSize;
    }

    std::optional<glm::vec2> WorldToCursor(const graphics::Camera& camera, const glm::dvec3& point, const glm::vec2& viewportSize) {
        // The same projection as ProjectToCursor(GizmoView, GizmoProjection, point), evaluated
        // relative to the camera so that it stays exact far away from the world origin.
        const glm::mat4 view = ViewFromBasis(camera, glm::vec3(0.0f));
        const glm::mat4 projection = GizmoProjection(camera, ViewAspect(viewportSize));
        return ProjectToCursor(view, projection, glm::vec3(point - camera.position), viewportSize);
    }

    glm::mat4 GizmoModelMatrix(const math::Transform& transform, const glm::vec3& pivotLocal) {
        glm::mat4 m = glm::mat4_cast(glm::normalize(transform.rotation));
        m[3] = glm::vec4(transform.TransformPoint(pivotLocal), 1.0f);
        return m;
    }

    math::Transform TransformFromGizmo(const glm::mat4& gizmoMatrix, const glm::vec3& pivotLocal, float scale) {
        // Gram-Schmidt on the upper 3x3: ImGuizmo may leave a little scale or skew behind.
        glm::vec3 x(gizmoMatrix[0]);
        glm::vec3 y(gizmoMatrix[1]);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        const float lx = glm::length(x);
        if (std::isfinite(lx) && lx > 1.0e-6f) {
            x /= lx;
            y -= x * glm::dot(x, y);
            const float ly = glm::length(y);
            if (std::isfinite(ly) && ly > 1.0e-6f) {
                y /= ly;
                const glm::vec3 z = glm::cross(x, y);         // keeps the basis right-handed
                const glm::quat q = glm::quat_cast(glm::mat3(x, y, z));
                const float lq = glm::length(q);
                if (std::isfinite(lq) && lq > 1.0e-6f) rotation = q / lq;
            }
        }

        math::Transform out;
        out.rotation = rotation;
        out.scale = scale;
        out.position = glm::vec3(gizmoMatrix[3]) - rotation * (pivotLocal * scale);
        return out;
    }

    glm::vec3 SunDirectionFromAngles(const SunAngles& angles) {
        const float c = std::cos(angles.elevation);
        return {-std::sin(angles.azimuth) * c, std::sin(angles.elevation), -std::cos(angles.azimuth) * c};
    }

    SunAngles SunAnglesFromDirection(const glm::vec3& direction) {
        SunAngles angles;
        const float length = glm::length(direction);
        if (!std::isfinite(length) || length <= 0.0f) {
            angles.elevation = math::kPi * 0.5f;
            return angles;
        }
        angles.elevation = std::asin(std::clamp(direction.y / length, -1.0f, 1.0f));
        const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z) / length;
        angles.azimuth = horizontal > 1.0e-6f ? std::atan2(-direction.x, -direction.z) : 0.0f;
        return angles;
    }

    void ApplyFlyCamera(graphics::Camera& camera, const FlyInput& input, const FlySettings& settings, float dt) {
        if (std::isfinite(input.lookDelta.x) && std::isfinite(input.lookDelta.y)) {
            const float limit = std::max(settings.pitchLimit, 0.0f);
            camera.yaw -= input.lookDelta.x * settings.lookSensitivity;
            camera.pitch = std::clamp(camera.pitch - input.lookDelta.y * settings.lookSensitivity, -limit, limit);
            // Keep the angle small: precision of sin / cos does not degrade after hours of turning.
            if (std::abs(camera.yaw) > math::kTwoPi) camera.yaw = std::fmod(camera.yaw, math::kTwoPi);
        }

        if (!std::isfinite(dt) || dt <= 0.0f) return;
        glm::vec3 move = camera.Right() * input.move.x + glm::vec3(0.0f, 1.0f, 0.0f) * input.move.y + camera.Forward() * input.move.z;
        const float length = glm::length(move);
        if (!std::isfinite(length) || length <= 0.0f) return;
        if (length > 1.0f) move /= length;
        const float speed = settings.speed * (input.fast ? settings.fastMultiplier : 1.0f);
        camera.position += glm::dvec3(move * (speed * dt));
    }
}
