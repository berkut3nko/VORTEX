// =============================================================================
//  Voxel ray traversal - shared by the GPU shaders and the C++ engine.
// =============================================================================
//  (Written in the GLSL/C++ common subset, see shared/vx_cpp.h.)
//
//  Data model
//    chunk   32^3 voxels with a 3-level occupancy pyramid (8^3 cells, 4^3 blocks, voxels)
//    world   a toroidal grid of chunk slots around the camera ("window")
//    object  one or more chunks with an affine transform, organised in a BVH
//
//  The including side provides the data through these macros:
//    VX_GRID_ENTRY(i)                 uint   world grid entry (0 = empty,
//                                            bit 31 set = uniform solid chunk, low byte = id,
//                                            otherwise chunk slot + 1)
//    VX_GRID_SIZE                     ivec3  window size in chunks (powers of two)
//    VX_GRID_MIN_CHUNK                ivec3  absolute chunk coordinate of the window minimum
//    VX_GRID_ORIGIN                   vec3   position of the window minimum in ray space
//    VX_VOXEL_SIZE / VX_INV_VOXEL_SIZE float edge length of a world voxel in ray space units
//    VX_OCC_WORD(slot, w)             uint   word `w` of the occupancy pyramid of a chunk
//    VX_VOXEL_ID(slot, k)             uint   material id of voxel `k` (block-ordered index)
//    VX_BVH_NODE_COUNT                uint
//    VX_BVH_MIN(n) / VX_BVH_MAX(n)    vec3   node bounds
//    VX_BVH_A(n) / VX_BVH_B(n)        uint   leaf: A = instance, B != 0; inner: children A, A+1
//    VX_INSTANCE_WORLD_TO_LOCAL(i)    mat4   ray space -> chunk-local voxel space
//    VX_INSTANCE_BOUNDS_MIN/MAX(i)    ivec3  voxel bounds inside the chunk, [min, max)
//    VX_INSTANCE_SLOT(i)              uint   chunk slot
// =============================================================================
#ifndef VX_SHARED_TRACE_GLSL
#define VX_SHARED_TRACE_GLSL

VX_CONST_INT(VX_CHUNK_SIZE, 32)
VX_CONST_UINT(VX_OCC_BLOCK_OFFSET, 2u)       // word offset of the 4^3 block mask
VX_CONST_UINT(VX_OCC_VOXEL_OFFSET, 18u)      // word offset of the per-voxel masks
VX_CONST_UINT(VX_GRID_UNIFORM_FLAG, 0x80000000u)
VX_CONST_UINT(VX_INVALID, 0xFFFFFFFFu)
VX_CONST_INT(VX_MAX_CHUNK_STEPS, 192)
VX_CONST_INT(VX_MAX_GRID_STEPS, 512)
VX_CONST_INT(VX_BVH_STACK_SIZE, 32)

struct VxHit {
    float t;         // ray parameter at the hit
    vec3  normal;    // unit normal of the face that was hit, in ray space
    ivec3 voxel;     // voxel coordinate: window-local for the world, chunk-local for instances
    uint  slot;      // chunk slot (VX_INVALID for a uniform chunk)
    uint  material;  // voxel id, 1..255
    uint  instance;  // instance index, VX_INVALID when the world was hit
    int   steps;     // traversal iterations (profiling / debug views)
};

VxHit VxMiss() {
    VxHit h;
    h.t = 3.0e38f;
    h.normal = vec3(0.0f, 1.0f, 0.0f);
    h.voxel = ivec3(0);
    h.slot = VX_INVALID;
    h.material = 0u;
    h.instance = VX_INVALID;
    h.steps = 0;
    return h;
}

// A zero direction component would turn into inf/NaN in the slab maths. Nudging it to a tiny
// value keeps every expression finite and changes the ray by less than a millionth.
vec3 VxSafeDirection(vec3 d) {
    float eps = 1.0e-6f;
    vec3 r = d;
    if (abs(r.x) < eps) r.x = (r.x < 0.0f) ? -eps : eps;
    if (abs(r.y) < eps) r.y = (r.y < 0.0f) ? -eps : eps;
    if (abs(r.z) < eps) r.z = (r.z < 0.0f) ? -eps : eps;
    return r;
}

// Index of the smallest component.
int VxMinAxis(vec3 v) {
    if (v.x <= v.y && v.x <= v.z) return 0;
    return (v.y <= v.z) ? 1 : 2;
}

// Normal of the face a ray crossed when it stepped along `axis`. axis < 0 means the ray
// started inside solid matter; the best we can do is oppose its dominant direction.
vec3 VxFaceNormal(int axis, vec3 rd) {
    int a = axis;
    if (a < 0) {
        vec3 m = abs(rd);
        a = (m.x >= m.y && m.x >= m.z) ? 0 : ((m.y >= m.z) ? 1 : 2);
    }
    vec3 n = vec3(0.0f);
    n[a] = (rd[a] > 0.0f) ? -1.0f : 1.0f;
    return n;
}

// Slab test against an axis aligned box. `axis` receives the axis of the entry face.
bool VxRayBox(vec3 ro, vec3 invRd, vec3 boxMin, vec3 boxMax,
              VX_OUT(float) tEnter, VX_OUT(float) tExit, VX_OUT(int) axis) {
    vec3 t0 = (boxMin - ro) * invRd;
    vec3 t1 = (boxMax - ro) * invRd;
    vec3 tn = min(t0, t1);
    vec3 tf = max(t0, t1);
    tEnter = max(tn.x, max(tn.y, tn.z));
    tExit = min(tf.x, min(tf.y, tf.z));
    axis = (tn.x >= tn.y && tn.x >= tn.z) ? 0 : ((tn.y >= tn.z) ? 1 : 2);
    return tExit >= tEnter;
}

vec3 VxTransformPoint(mat4 m, vec3 p) { return vec3(m * vec4(p, 1.0f)); }
vec3 VxTransformVector(mat4 m, vec3 v) { return vec3(m * vec4(v, 0.0f)); }

// -----------------------------------------------------------------------------
//  One chunk.
//
//  The ray is given in chunk-local voxel units and is already known to be inside the box
//  [bmin, bmax) for t in [tStart, tEnd]. entryAxis is the axis of the face it came in
//  through (-1 if it starts inside).
//
//  Each iteration looks at the cell the ray is in. If the 8^3 cell, the 4^3 block or the voxel
//  is empty, the ray jumps to the exit face of that whole cube - so empty space costs one
//  step per cube, not one per voxel. The exit position is recomputed from the plane it
//  crosses (never accumulated), and the next cell is derived from it with the crossed axis
//  set exactly, so there is no drift and no epsilon tuning.
// -----------------------------------------------------------------------------
bool VxTraceChunk(uint slot, vec3 ro, vec3 rd, vec3 invRd, float tStart, float tEnd,
                  int entryAxis, ivec3 bmin, ivec3 bmax, VX_INOUT(VxHit) hit) {
    ivec3 cell = clamp(ivec3(floor(ro + rd * tStart)), bmin, bmax - ivec3(1));
    if (entryAxis >= 0) {
        cell[entryAxis] = (rd[entryAxis] > 0.0f) ? bmin[entryAxis] : bmax[entryAxis] - 1;
    }

    vec3 stepPos = vec3(rd.x >= 0.0f ? 1.0f : 0.0f, rd.y >= 0.0f ? 1.0f : 0.0f, rd.z >= 0.0f ? 1.0f : 0.0f);
    float t = tStart;
    int axis = entryAxis;

    for (int i = 0; i < VX_MAX_CHUNK_STEPS; ++i) {
        hit.steps += 1;

        int size = 1;
        ivec3 c8 = cell >> 3;
        uint ci = uint(c8.x + c8.y * 4 + c8.z * 16);
        if (((VX_OCC_WORD(slot, ci >> 5u) >> (ci & 31u)) & 1u) == 0u) {
            size = 8;
        } else {
            ivec3 b4 = cell >> 2;
            uint bi = uint(b4.x + b4.y * 8 + b4.z * 64);
            if (((VX_OCC_WORD(slot, VX_OCC_BLOCK_OFFSET + (bi >> 5u)) >> (bi & 31u)) & 1u) == 0u) {
                size = 4;
            } else {
                ivec3 l = cell & 3;
                uint li = uint(l.x + l.y * 4 + l.z * 16);
                if (((VX_OCC_WORD(slot, VX_OCC_VOXEL_OFFSET + bi * 2u + (li >> 5u)) >> (li & 31u)) & 1u) != 0u) {
                    hit.t = t;
                    hit.normal = VxFaceNormal(axis, rd);
                    hit.voxel = cell;
                    hit.slot = slot;
                    hit.material = VX_VOXEL_ID(slot, bi * 64u + li);
                    return true;
                }
            }
        }

        // Leave the empty cube of edge `size` that contains `cell`.
        ivec3 cubeMin = cell & ivec3(~(size - 1));
        vec3 exitPlane = vec3(cubeMin) + stepPos * float(size);
        vec3 tAxis = (exitPlane - ro) * invRd;
        axis = VxMinAxis(tAxis);
        float tNext = tAxis[axis];
        if (tNext >= tEnd) return false;
        t = max(t, tNext);

        ivec3 next = clamp(ivec3(floor(ro + rd * t)), cubeMin, cubeMin + ivec3(size - 1));
        next[axis] = (rd[axis] >= 0.0f) ? cubeMin[axis] + size : cubeMin[axis] - 1;
        if (next[axis] < bmin[axis] || next[axis] >= bmax[axis]) return false;
        cell = next;
    }
    return false;
}

// -----------------------------------------------------------------------------
//  The world: a DDA over the chunk grid, descending into VxTraceChunk for chunks that
//  contain geometry. `ro` is in window-local voxel units (the window minimum is the origin).
// -----------------------------------------------------------------------------
bool VxTraceWorld(vec3 ro, vec3 rdIn, float tMin, float tMax, VX_INOUT(VxHit) hit) {
    vec3 rd = VxSafeDirection(rdIn);
    vec3 invRd = vec3(1.0f) / rd;
    ivec3 gridSize = VX_GRID_SIZE;
    float cs = float(VX_CHUNK_SIZE);

    float tEnter;
    float tExit;
    int axis;
    if (!VxRayBox(ro, invRd, vec3(0.0f), vec3(gridSize) * cs, tEnter, tExit, axis)) return false;
    float t = max(tEnter, tMin);
    float tEnd = min(tExit, tMax);
    if (t >= tEnd) return false;
    if (tEnter <= tMin) axis = -1;

    ivec3 cc = clamp(ivec3(floor((ro + rd * t) / cs)), ivec3(0), gridSize - ivec3(1));
    if (axis >= 0) cc[axis] = (rd[axis] > 0.0f) ? 0 : gridSize[axis] - 1;

    vec3 stepPos = vec3(rd.x >= 0.0f ? 1.0f : 0.0f, rd.y >= 0.0f ? 1.0f : 0.0f, rd.z >= 0.0f ? 1.0f : 0.0f);
    ivec3 stepDir = ivec3(rd.x >= 0.0f ? 1 : -1, rd.y >= 0.0f ? 1 : -1, rd.z >= 0.0f ? 1 : -1);
    ivec3 gridMin = VX_GRID_MIN_CHUNK;

    for (int i = 0; i < VX_MAX_GRID_STEPS; ++i) {
        hit.steps += 1;

        vec3 exitPlane = (vec3(cc) + stepPos) * cs;
        vec3 tAxis = (exitPlane - ro) * invRd;
        int exitAxis = VxMinAxis(tAxis);
        float tCellExit = tAxis[exitAxis];

        // The grid is addressed toroidally, so moving the window never copies entries.
        ivec3 w = (gridMin + cc) & (gridSize - ivec3(1));
        uint entry = VX_GRID_ENTRY(uint(w.x + w.z * gridSize.x + w.y * gridSize.x * gridSize.z));
        if (entry != 0u) {
            ivec3 chunkMin = cc * VX_CHUNK_SIZE;
            if ((entry & VX_GRID_UNIFORM_FLAG) != 0u) {
                // A chunk made of a single material has no storage: its surface is its box.
                ivec3 v = clamp(ivec3(floor(ro + rd * t)), chunkMin, chunkMin + ivec3(VX_CHUNK_SIZE - 1));
                if (axis >= 0) v[axis] = (rd[axis] > 0.0f) ? chunkMin[axis] : chunkMin[axis] + VX_CHUNK_SIZE - 1;
                hit.t = t;
                hit.normal = VxFaceNormal(axis, rd);
                hit.voxel = v;
                hit.slot = VX_INVALID;
                hit.material = entry & 0xFFu;
                hit.instance = VX_INVALID;
                return true;
            }
            if (VxTraceChunk(entry - 1u, ro - vec3(chunkMin), rd, invRd, t, min(tCellExit, tEnd), axis,
                             ivec3(0), ivec3(VX_CHUNK_SIZE), hit)) {
                hit.voxel += chunkMin;
                hit.instance = VX_INVALID;
                return true;
            }
        }

        if (tCellExit >= tEnd) return false;
        t = max(t, tCellExit);
        axis = exitAxis;
        cc[axis] += stepDir[axis];
        if (cc[axis] < 0 || cc[axis] >= gridSize[axis]) return false;
    }
    return false;
}

// -----------------------------------------------------------------------------
//  Dynamic objects: BVH over instance bounds, then VxTraceChunk in each candidate's
//  local space. Rays keep their world parametrisation (the local direction is not
//  re-normalised), so `t` values from different instances are directly comparable.
// -----------------------------------------------------------------------------
bool VxTraceInstances(vec3 ro, vec3 rdIn, float tMin, float tMax, bool anyHit, VX_INOUT(VxHit) hit) {
    uint nodeCount = VX_BVH_NODE_COUNT;
    if (nodeCount == 0u) return false;

    vec3 rd = VxSafeDirection(rdIn);
    vec3 invRd = vec3(1.0f) / rd;

    uint stack[VX_BVH_STACK_SIZE];
    int sp = 0;
    stack[sp] = 0u;
    sp += 1;

    bool found = false;
    float tBest = tMax;

    while (sp > 0) {
        sp -= 1;
        uint node = stack[sp];
        hit.steps += 1;

        float tn;
        float tf;
        int ax;
        if (!VxRayBox(ro, invRd, VX_BVH_MIN(node), VX_BVH_MAX(node), tn, tf, ax)) continue;
        if (tf < tMin || tn > tBest) continue;

        uint a = VX_BVH_A(node);
        if (VX_BVH_B(node) == 0u) {
            if (sp + 2 <= VX_BVH_STACK_SIZE) {
                stack[sp] = a;
                stack[sp + 1] = a + 1u;
                sp += 2;
            }
            continue;
        }

        mat4 toLocal = VX_INSTANCE_WORLD_TO_LOCAL(a);
        vec3 lro = VxTransformPoint(toLocal, ro);
        vec3 lrd = VxSafeDirection(VxTransformVector(toLocal, rd));
        vec3 linv = vec3(1.0f) / lrd;
        ivec3 bmin = VX_INSTANCE_BOUNDS_MIN(a);
        ivec3 bmax = VX_INSTANCE_BOUNDS_MAX(a);

        float te;
        float tx;
        int entryAxis;
        if (!VxRayBox(lro, linv, vec3(bmin), vec3(bmax), te, tx, entryAxis)) continue;
        float ts = max(te, tMin);
        float tEnd = min(tx, tBest);
        if (ts >= tEnd) continue;
        if (te <= tMin) entryAxis = -1;

        VxHit h = hit;
        if (VxTraceChunk(VX_INSTANCE_SLOT(a), lro, lrd, linv, ts, tEnd, entryAxis, bmin, bmax, h)) {
            // Normals transform with the inverse transpose of local->world, i.e. the
            // transpose of the 3x3 part of world->local.
            vec3 n = vec3(dot(vec3(toLocal[0]), h.normal), dot(vec3(toLocal[1]), h.normal), dot(vec3(toLocal[2]), h.normal));
            h.normal = normalize(n);
            h.instance = a;
            hit = h;
            tBest = h.t;
            found = true;
            if (anyHit) return true;
        } else {
            hit.steps = h.steps;
        }
    }
    return found;
}

// -----------------------------------------------------------------------------
//  World + objects. `ro` / `rd` are in ray space (render space on the GPU), distances in
//  the same units; rd must be normalised.
// -----------------------------------------------------------------------------
bool VxTraceScene(vec3 ro, vec3 rd, float tMin, float tMax, bool anyHit, VX_INOUT(VxHit) hit) {
    bool found = false;
    float tBest = tMax;
    float inv = VX_INV_VOXEL_SIZE;

    VxHit wh = hit;
    if (VxTraceWorld((ro - VX_GRID_ORIGIN) * inv, rd, tMin * inv, tMax * inv, wh)) {
        wh.t = wh.t * VX_VOXEL_SIZE;
        hit = wh;
        tBest = wh.t;
        found = true;
        if (anyHit) return true;
    } else {
        hit.steps = wh.steps;
    }

    VxHit ih = hit;
    if (VxTraceInstances(ro, rd, tMin, tBest, anyHit, ih)) {
        hit = ih;
        found = true;
    } else {
        hit.steps = ih.steps;
    }
    return found;
}

#endif // VX_SHARED_TRACE_GLSL
