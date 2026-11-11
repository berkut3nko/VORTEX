module;

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

export module vortex.voxel:trace;

import :chunk;
import :store;
import :instances;

export namespace vortex::voxel {

    /// @brief Result of a CPU ray query.
    struct RayHit {
        bool hit = false;
        float t = 0.0f;                      ///< Distance along the ray (ray space units).
        glm::vec3 position{0.0f};            ///< Hit point in ray space.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};  ///< Face normal in ray space.
        glm::ivec3 voxel{0};                 ///< World voxel (world hit) or chunk-local voxel (instance hit).
        uint32_t slot = ChunkStore::kInvalidSlot;
        uint32_t instance = 0xFFFFFFFFu;     ///< Index into TraceScene::instances, or ~0 for the world.
        uint8_t material = 0;                ///< Voxel id at the hit.
        int steps = 0;                       ///< Traversal iterations spent.

        bool HitWorld() const { return hit && instance == 0xFFFFFFFFu; }
    };

    /**
     * @brief Non-owning view of everything a ray can hit.
     * @details This is the CPU twin of the buffers the shaders bind: the same world grid, the
     * same chunk slots, the same instance and BVH arrays, traversed by the same code
     * (shaders/shared/trace.glsl compiled as C++).
     */
    struct TraceScene {
        const ChunkStore* store = nullptr;

        // World window. `grid` holds gridSize.x * gridSize.y * gridSize.z entries, toroidally addressed.
        const uint32_t* grid = nullptr;
        glm::ivec3 gridSize{0};
        glm::ivec3 gridMinChunk{0};
        glm::vec3 gridOrigin{0.0f};          ///< Ray-space position of the window's minimum corner.
        float voxelSize = 1.0f;              ///< Ray-space edge length of a world voxel.

        // Dynamic objects (may be empty).
        std::span<const GpuInstance> instances;
        std::span<const GpuBvhNode> bvh;
    };

    /**
     * @brief Closest hit (or any hit) of a ray against world + instances.
     * @param direction Must be normalized.
     * @param anyHit Stop at the first intersection found (occlusion queries).
     */
    RayHit Trace(const TraceScene& scene, const glm::vec3& origin, const glm::vec3& direction,
                 float tMin, float tMax, bool anyHit = false);

    /// @brief Ray against the world only.
    RayHit TraceWorld(const TraceScene& scene, const glm::vec3& origin, const glm::vec3& direction,
                      float tMin, float tMax);

    /// @brief Ray against a single chunk, in chunk-local voxel units, restricted to [boundsMin, boundsMax).
    RayHit TraceChunk(const ChunkStore& store, uint32_t slot, const glm::vec3& origin, const glm::vec3& direction,
                      float tMin, float tMax, const glm::ivec3& boundsMin = glm::ivec3(0),
                      const glm::ivec3& boundsMax = glm::ivec3(kChunkSize));
}
