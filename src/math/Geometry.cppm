module;

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

export module vortex.math:geometry;

export namespace vortex::math {

    struct Ray {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};   ///< Expected to be normalized.
        glm::vec3 At(float t) const { return origin + direction * t; }
    };

    /// @brief Axis aligned bounding box. A default-constructed box is empty (min > max).
    struct AABB {
        glm::vec3 min{std::numeric_limits<float>::max()};
        glm::vec3 max{std::numeric_limits<float>::lowest()};

        AABB() = default;
        AABB(const glm::vec3& mn, const glm::vec3& mx) : min(mn), max(mx) {}

        bool IsEmpty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }
        void Grow(const glm::vec3& p) { min = glm::min(min, p); max = glm::max(max, p); }
        void Grow(const AABB& b) { min = glm::min(min, b.min); max = glm::max(max, b.max); }
        void Expand(float amount) { min -= glm::vec3(amount); max += glm::vec3(amount); }
        glm::vec3 Center() const { return (min + max) * 0.5f; }
        glm::vec3 Extent() const { return max - min; }
        bool Contains(const glm::vec3& p) const { return glm::all(glm::greaterThanEqual(p, min)) && glm::all(glm::lessThanEqual(p, max)); }
        bool Overlaps(const AABB& b) const {
            return min.x <= b.max.x && max.x >= b.min.x && min.y <= b.max.y && max.y >= b.min.y && min.z <= b.max.z && max.z >= b.min.z;
        }
        float SurfaceArea() const {
            if (IsEmpty()) return 0.0f;
            const glm::vec3 e = Extent();
            return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
        }

        /// @brief Bounding box of this box after an affine transform (Arvo's method, 9 mul + adds).
        AABB Transformed(const glm::mat4& m) const {
            const glm::vec3 c = Center();
            const glm::vec3 e = Extent() * 0.5f;
            const glm::vec3 nc = glm::vec3(m * glm::vec4(c, 1.0f));
            const glm::mat3 a(glm::abs(glm::vec3(m[0])), glm::abs(glm::vec3(m[1])), glm::abs(glm::vec3(m[2])));
            const glm::vec3 ne = a * e;
            return {nc - ne, nc + ne};
        }
    };

    /**
     * @brief Slab test.
     * @return true if the ray hits the box within [tMin, tMax]; tNear/tFar receive the clipped interval.
     */
    inline bool IntersectRayAABB(const Ray& ray, const AABB& box, float tMin, float tMax, float& tNear, float& tFar) {
        tNear = tMin;
        tFar = tMax;
        for (int a = 0; a < 3; ++a) {
            const float d = ray.direction[a];
            if (std::abs(d) < 1e-12f) {
                if (ray.origin[a] < box.min[a] || ray.origin[a] > box.max[a]) return false;
                continue;
            }
            const float inv = 1.0f / d;
            float t0 = (box.min[a] - ray.origin[a]) * inv;
            float t1 = (box.max[a] - ray.origin[a]) * inv;
            if (t0 > t1) std::swap(t0, t1);
            tNear = t0 > tNear ? t0 : tNear;
            tFar = t1 < tFar ? t1 : tFar;
            if (tNear > tFar) return false;
        }
        return true;
    }

    /// @brief Plane in the form dot(n, p) + d = 0, with n pointing to the "inside".
    struct Plane {
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        float d = 0.0f;
        float SignedDistance(const glm::vec3& p) const { return glm::dot(normal, p) + d; }
    };

    /**
     * @brief View frustum for culling. Built from a (reverse-Z, infinite far) view-projection
     * matrix; it has five planes: left, right, bottom, top, near.
     */
    struct Frustum {
        std::array<Plane, 5> planes{};

        static Frustum FromViewProjection(const glm::mat4& vp) {
            // Gribb/Hartmann: clip-space inequalities expressed through the rows of the matrix.
            auto row = [&vp](int i) { return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]); };
            const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
            const std::array<glm::vec4, 5> raw = {
                r3 + r0,   // left:    x >= -w
                r3 - r0,   // right:   x <=  w
                r3 + r1,   // bottom:  y >= -w
                r3 - r1,   // top:     y <=  w
                r3 - r2,   // near:    z <=  w  (reverse-Z: the near plane is z = w)
            };
            Frustum f;
            for (size_t i = 0; i < raw.size(); ++i) {
                const float len = glm::length(glm::vec3(raw[i]));
                const float inv = len > 0.0f ? 1.0f / len : 0.0f;
                f.planes[i].normal = glm::vec3(raw[i]) * inv;
                f.planes[i].d = raw[i].w * inv;
            }
            return f;
        }

        /// @brief Conservative test: false only if the box is completely outside one plane.
        bool Intersects(const AABB& box) const {
            for (const Plane& p : planes) {
                // Corner of the box that is furthest along the plane normal.
                const glm::vec3 positive(p.normal.x >= 0.0f ? box.max.x : box.min.x,
                                         p.normal.y >= 0.0f ? box.max.y : box.min.y,
                                         p.normal.z >= 0.0f ? box.max.z : box.min.z);
                if (p.SignedDistance(positive) < 0.0f) return false;
            }
            return true;
        }
    };
}
