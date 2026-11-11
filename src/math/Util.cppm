module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>

export module vortex.math:util;

export namespace vortex::math {

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;

    /// @brief Integer division rounding towards negative infinity (b > 0).
    constexpr int32_t FloorDiv(int32_t a, int32_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }
    /// @brief Remainder that is always in [0, b) (b > 0).
    constexpr int32_t FloorMod(int32_t a, int32_t b) { const int32_t m = a % b; return m < 0 ? m + b : m; }

    inline glm::ivec3 FloorDiv(const glm::ivec3& a, int32_t b) { return {FloorDiv(a.x, b), FloorDiv(a.y, b), FloorDiv(a.z, b)}; }
    inline glm::ivec3 FloorMod(const glm::ivec3& a, int32_t b) { return {FloorMod(a.x, b), FloorMod(a.y, b), FloorMod(a.z, b)}; }

    constexpr float Saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    constexpr float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    inline float SmoothStep(float e0, float e1, float x) {
        const float t = Saturate((x - e0) / (e1 - e0));
        return t * t * (3.0f - 2.0f * t);
    }
    constexpr bool IsPowerOfTwo(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

    /**
     * @brief Rigid transform with uniform scale: the only kind of transform voxel objects use.
     * @details Uniform scale keeps voxels cubic, which ray marching and physics both rely on.
     */
    struct Transform {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float scale = 1.0f;

        glm::mat4 ToMatrix() const {
            glm::mat4 m = glm::mat4_cast(rotation);
            m[0] *= scale;
            m[1] *= scale;
            m[2] *= scale;
            m[3] = glm::vec4(position, 1.0f);
            return m;
        }

        /// @brief Inverse matrix computed analytically (cheaper and more accurate than glm::inverse).
        glm::mat4 ToInverseMatrix() const {
            const float invScale = 1.0f / scale;
            const glm::mat3 r = glm::transpose(glm::mat3_cast(rotation)) * invScale;
            glm::mat4 m(r);
            m[3] = glm::vec4(-(r * position), 1.0f);
            return m;
        }

        glm::vec3 TransformPoint(const glm::vec3& p) const { return position + rotation * (p * scale); }
        glm::vec3 InverseTransformPoint(const glm::vec3& p) const { return (glm::inverse(rotation) * (p - position)) / scale; }
        glm::vec3 TransformDirection(const glm::vec3& d) const { return rotation * d; }

        static Transform Interpolate(const Transform& a, const Transform& b, float t) {
            Transform out;
            out.position = glm::mix(a.position, b.position, t);
            out.rotation = glm::slerp(a.rotation, b.rotation, t);
            out.scale = Lerp(a.scale, b.scale, t);
            return out;
        }
    };
}
