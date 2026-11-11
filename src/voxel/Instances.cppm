module;

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

export module vortex.voxel:instances;

import vortex.math;

export namespace vortex::voxel {

    /**
     * @brief One chunk of a dynamic object placed in the scene (std430, 224 bytes).
     * @details "Local" space is the chunk's own voxel grid (0..32 on each axis); "world" is
     * whatever space the rays are in (render space on the GPU). Mirrors `VxInstance` in GLSL.
     */
    struct GpuInstance {
        glm::mat4 localToWorld{1.0f};
        glm::mat4 worldToLocal{1.0f};
        glm::mat4 prevLocalToWorld{1.0f};     ///< Last frame's transform (motion vectors).
        glm::ivec4 boundsMinSlot{0};          ///< xyz = voxel bounds min, w = chunk slot.
        glm::ivec4 boundsMaxPalette{0};       ///< xyz = voxel bounds max (exclusive), w = palette * 256.
    };
    static_assert(sizeof(GpuInstance) == 224);

    /**
     * @brief BVH node (std430, 32 bytes). Mirrors `VxBvhNode` in GLSL.
     * @details Leaf: `b != 0`, `a` = index of the primitive (instance).
     *          Inner: `b == 0`, children are nodes `a` and `a + 1`.
     */
    struct GpuBvhNode {
        glm::vec3 min{0.0f};
        uint32_t a = 0;
        glm::vec3 max{0.0f};
        uint32_t b = 0;
    };
    static_assert(sizeof(GpuBvhNode) == 32);

    /**
     * @brief Builds a BVH over a set of boxes (median split on the longest centroid axis).
     * @details O(n log n), no recursion, no per-node allocations: cheap enough to rebuild every
     * frame for a few thousand moving objects. Leaves hold exactly one primitive.
     */
    void BuildBvh(std::span<const math::AABB> bounds, std::vector<GpuBvhNode>& outNodes);
}
