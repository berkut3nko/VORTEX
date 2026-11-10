module;

#include <glm/glm.hpp>

#include "shared/vx_cpp.h"

#include <cstdint>
#include <span>

module vortex.voxel;

namespace vortex::voxel {

    namespace kernel {
        // GLSL vocabulary for the shared code below.
        using namespace glm;
        using uint = unsigned int;

        /// The shared traversal compiled as C++: the accessor macros read from this object.
        struct TraceKernel {
            const TraceScene& scene;

            uint OccWord(uint slot, uint word) const { return scene.store->Get(slot).OccupancyWord(word); }
            uint VoxelId(uint slot, uint index) const { return scene.store->Get(slot).ids[index]; }

#define VX_GRID_ENTRY(i) (scene.grid[(i)])
#define VX_GRID_SIZE (scene.gridSize)
#define VX_GRID_MIN_CHUNK (scene.gridMinChunk)
#define VX_GRID_ORIGIN (scene.gridOrigin)
#define VX_VOXEL_SIZE (scene.voxelSize)
#define VX_INV_VOXEL_SIZE (1.0f / scene.voxelSize)
#define VX_OCC_WORD(slot, w) OccWord((slot), (w))
#define VX_VOXEL_ID(slot, k) VoxelId((slot), (k))
#define VX_BVH_NODE_COUNT (static_cast<uint>(scene.bvh.size()))
#define VX_BVH_MIN(n) (scene.bvh[(n)].min)
#define VX_BVH_MAX(n) (scene.bvh[(n)].max)
#define VX_BVH_A(n) (scene.bvh[(n)].a)
#define VX_BVH_B(n) (scene.bvh[(n)].b)
#define VX_INSTANCE_WORLD_TO_LOCAL(i) (scene.instances[(i)].worldToLocal)
#define VX_INSTANCE_BOUNDS_MIN(i) (ivec3(scene.instances[(i)].boundsMinSlot))
#define VX_INSTANCE_BOUNDS_MAX(i) (ivec3(scene.instances[(i)].boundsMaxPalette))
#define VX_INSTANCE_SLOT(i) (static_cast<uint>(scene.instances[(i)].boundsMinSlot.w))

#include "shared/trace.glsl"
        };

        RayHit ToRayHit(const TraceKernel::VxHit& h, bool found, const glm::vec3& origin, const glm::vec3& direction) {
            RayHit out;
            out.hit = found;
            out.steps = h.steps;
            if (!found) return out;
            out.t = h.t;
            out.position = origin + direction * h.t;
            out.normal = h.normal;
            out.voxel = h.voxel;
            out.slot = h.slot;
            out.instance = h.instance;
            out.material = static_cast<uint8_t>(h.material);
            return out;
        }
    }

    RayHit Trace(const TraceScene& scene, const glm::vec3& origin, const glm::vec3& direction,
                 float tMin, float tMax, bool anyHit) {
        kernel::TraceKernel k{scene};
        auto h = k.VxMiss();
        bool found = false;
        if (scene.grid && scene.store) {
            found = k.VxTraceScene(origin, direction, tMin, tMax, anyHit, h);
        } else if (scene.store) {
            found = k.VxTraceInstances(origin, direction, tMin, tMax, anyHit, h);
        }
        RayHit out = kernel::ToRayHit(h, found, origin, direction);
        if (out.HitWorld()) out.voxel += scene.gridMinChunk * kChunkSize;   // window-local -> world voxel
        return out;
    }

    RayHit TraceWorld(const TraceScene& scene, const glm::vec3& origin, const glm::vec3& direction,
                      float tMin, float tMax) {
        RayHit out;
        if (!scene.grid || !scene.store) return out;
        kernel::TraceKernel k{scene};
        auto h = k.VxMiss();
        const float inv = 1.0f / scene.voxelSize;
        const bool found = k.VxTraceWorld((origin - scene.gridOrigin) * inv, direction, tMin * inv, tMax * inv, h);
        h.t *= scene.voxelSize;
        out = kernel::ToRayHit(h, found, origin, direction);
        if (out.hit) out.voxel += scene.gridMinChunk * kChunkSize;
        return out;
    }

    RayHit TraceChunk(const ChunkStore& store, uint32_t slot, const glm::vec3& origin, const glm::vec3& direction,
                      float tMin, float tMax, const glm::ivec3& boundsMin, const glm::ivec3& boundsMax) {
        RayHit out;
        if (!store.IsAllocated(slot)) return out;
        TraceScene scene;
        scene.store = &store;
        kernel::TraceKernel k{scene};

        const glm::vec3 rd = k.VxSafeDirection(direction);
        const glm::vec3 inv = glm::vec3(1.0f) / rd;
        float tEnter = 0.0f, tExit = 0.0f;
        int axis = -1;
        if (!k.VxRayBox(origin, inv, glm::vec3(boundsMin), glm::vec3(boundsMax), tEnter, tExit, axis)) return out;
        const float tStart = tEnter > tMin ? tEnter : tMin;
        const float tEnd = tExit < tMax ? tExit : tMax;
        if (tStart >= tEnd) return out;
        if (tEnter <= tMin) axis = -1;

        auto h = k.VxMiss();
        const bool found = k.VxTraceChunk(slot, origin, rd, inv, tStart, tEnd, axis, boundsMin, boundsMax, h);
        return kernel::ToRayHit(h, found, origin, direction);
    }
}
