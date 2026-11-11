module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

module vortex.voxel;

import vortex.math;

namespace vortex::voxel {

    void BuildBvh(std::span<const math::AABB> bounds, std::vector<GpuBvhNode>& nodes) {
        nodes.clear();
        const uint32_t count = static_cast<uint32_t>(bounds.size());
        if (count == 0) return;

        std::vector<uint32_t> order(count);
        std::vector<glm::vec3> centroids(count);
        for (uint32_t i = 0; i < count; ++i) {
            order[i] = i;
            centroids[i] = bounds[i].Center();
        }

        struct Task {
            uint32_t node, begin, end;
        };
        std::vector<Task> tasks;
        tasks.reserve(64);
        nodes.reserve(static_cast<size_t>(count) * 2);
        nodes.emplace_back();
        tasks.push_back({0, 0, count});

        while (!tasks.empty()) {
            const Task task = tasks.back();
            tasks.pop_back();

            math::AABB box;
            math::AABB centroidBox;
            for (uint32_t i = task.begin; i < task.end; ++i) {
                box.Grow(bounds[order[i]]);
                centroidBox.Grow(centroids[order[i]]);
            }
            nodes[task.node].min = box.min;
            nodes[task.node].max = box.max;

            if (task.end - task.begin == 1) {
                nodes[task.node].a = order[task.begin];
                nodes[task.node].b = 1;
                continue;
            }

            const glm::vec3 extent = centroidBox.Extent();
            int axis = 0;
            if (extent.y > extent.x) axis = 1;
            if (extent.z > extent[axis]) axis = 2;

            const uint32_t mid = task.begin + (task.end - task.begin) / 2;
            std::nth_element(order.begin() + task.begin, order.begin() + mid, order.begin() + task.end,
                             [&centroids, axis](uint32_t l, uint32_t r) { return centroids[l][axis] < centroids[r][axis]; });

            const uint32_t left = static_cast<uint32_t>(nodes.size());
            nodes.emplace_back();
            nodes.emplace_back();
            nodes[task.node].a = left;
            nodes[task.node].b = 0;
            tasks.push_back({left, task.begin, mid});
            tasks.push_back({left + 1, mid, task.end});
        }
    }
}
