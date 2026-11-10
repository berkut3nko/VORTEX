module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>

export module vortex.math:projection;

export namespace vortex::math {

    /**
     * @brief Perspective projection for Vulkan with reversed depth and an infinite far plane.
     * @details
     *  - NDC x right, y down (Vulkan), depth 1 at the near plane falling to 0 at infinity.
     *  - Reversed depth with a float depth buffer keeps precision almost constant over the whole
     *    view range, which a streaming open world needs (no far plane tuning, no z-fighting).
     *  - View space follows the usual right-handed convention: the camera looks down -Z.
     *
     *  clip.w = -z_view,  ndc.z = zNear / -z_view.
     */
    inline glm::mat4 PerspectiveReverseZ(float fovYRadians, float aspect, float zNear) {
        const float f = 1.0f / std::tan(fovYRadians * 0.5f);
        glm::mat4 m(0.0f);
        m[0][0] = f / aspect;
        m[1][1] = -f;        // flip Y: Vulkan clip space points down
        m[2][3] = -1.0f;     // w = -z_view
        m[3][2] = zNear;     // z_clip = zNear
        return m;
    }

    /// @brief Converts a reverse-Z depth buffer value to the distance along the view axis.
    inline float LinearizeReverseZ(float depth, float zNear) { return zNear / depth; }

    /// @brief Halton low-discrepancy sequence, element `index` (1-based) in the given base.
    inline float Halton(uint32_t index, uint32_t base) {
        float f = 1.0f;
        float r = 0.0f;
        while (index > 0) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(index % base);
            index /= base;
        }
        return r;
    }
}
