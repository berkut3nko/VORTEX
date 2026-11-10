// =============================================================================
//  VORTEX shader common declarations (GPU only)
// =============================================================================
//  THE CONTRACT between the renderer (C++) and every shader:
//    * the per-frame uniform block (mirrors graphics::FrameUniforms, std140, only
//      mat4 / vec4 / ivec4 members so C++ and GLSL layouts are identical by construction),
//    * the binding numbers of all buffers (set 0),
//    * the layout of the G-buffer,
//    * the accessor macros that plug the scene buffers into shared/trace.glsl.
//
//  A shader #defines which resources it needs BEFORE including this file:
//      VX_NEED_TRACE       world grid + chunk occupancy + chunk ids + instances + BVH
//      VX_NEED_MATERIALS   material table
//      VX_NEED_INSTANCES   instance table (implied by VX_NEED_TRACE)
//      VX_NEED_DRAW_LIST   instance draw list (object rasterisation)
//      VX_NEED_LIGHTS      point lights
//      VX_NEED_PARTICLES   particle instances
//  Only declared resources end up in the pipeline layout (layouts are reflected from SPIR-V),
//  and the renderer binds resources BY NAME (u_Frame, u_Grid, ...), so names here matter.
//
//  Textures use bindings 16.. and are declared by each pass itself.
//  Unit tests (tests/core/ShaderContractTests) verify the offsets below against C++.
// =============================================================================
#ifndef VX_COMMON_GLSL
#define VX_COMMON_GLSL

#include "shared/vx_glsl.glsl"

// ---- Per-frame uniforms -------------------------------------------------------------------
// Spaces: "render space" = world metres relative to the minimum corner of the streaming
// window (so coordinates stay small and precise anywhere in the world).
layout(set = 0, binding = 0, std140) uniform FrameUniforms {
    mat4 viewProj;               // render space -> clip, with TAA jitter (reverse-Z, infinite far)
    mat4 viewProjNoJitter;       // same without jitter (motion vectors)
    mat4 prevViewProjNoJitter;   // previous frame's, expressed in the CURRENT render space
    vec4 cameraPos;              // xyz = camera position (render space, m), w = near plane distance (m)
    vec4 cameraRight;            // xyz = unit right   * tan(fovX / 2)
    vec4 cameraUp;               // xyz = unit up      * tan(fovY / 2)
    vec4 cameraForward;          // xyz = unit forward
    vec4 cameraPosVoxel;         // xyz = camera position in window-local voxel units, w = voxel size (m)
    vec4 renderSize;             // xy = render resolution (px), zw = 1 / resolution
    vec4 outputSize;             // xy = output resolution (px), zw = 1 / resolution
    vec4 jitter;                 // xy = this frame's jitter in NDC units, zw = previous frame's
    vec4 sunDirection;           // xyz = unit vector TOWARDS the sun, w = tan(angular radius)
    vec4 sunRadiance;            // rgb = sun radiance (linear)
    vec4 skyZenith;              // rgb
    vec4 skyHorizon;             // rgb
    vec4 skyGround;              // rgb = colour of the lower hemisphere
    vec4 fogColor;               // rgb = fog colour, w = density (1/m)
    vec4 fogParams;              // x = height falloff (1/m), y = fog base height (render space y), z = start distance (m), w = max opacity
    vec4 ambientParams;          // x = ambient intensity, y = ambient/AO ray length (m), z = emissive scale, w = exposure
    vec4 temporalParams;         // x = max lighting history length (frames), y = TAA history weight (0..1), z = history valid (0/1), w = time (s)
    ivec4 gridSize;              // xyz = window size in chunks
    ivec4 gridMinChunk;          // xyz = absolute chunk coordinate of the window minimum
    ivec4 counts;                // x = instance count, y = BVH node count, z = light count, w = frame index
    ivec4 settings;              // x = feature flags (VX_FEATURE_*), y = debug view (VX_DEBUG_*), z = front-face draw count, w = unused
} u_Frame;

const int VX_FEATURE_SUN_SHADOWS      = 1;
const int VX_FEATURE_AMBIENT_OCCLUSION = 2;
const int VX_FEATURE_EMISSIVE_LIGHT   = 4;    // ambient rays pick up light from emissive voxels
const int VX_FEATURE_LIGHT_SHADOWS    = 8;    // point light shadows
const int VX_FEATURE_TAA              = 16;
const int VX_FEATURE_FOG              = 32;
const int VX_FEATURE_SPATIAL_FILTER   = 64;   // edge-aware blur of the lighting buffer

const int VX_DEBUG_NONE       = 0;
const int VX_DEBUG_ALBEDO     = 1;
const int VX_DEBUG_NORMAL     = 2;
const int VX_DEBUG_DEPTH      = 3;
const int VX_DEBUG_LIGHTING   = 4;   // accumulated irradiance only
const int VX_DEBUG_STEPS      = 5;   // traversal cost heat map
const int VX_DEBUG_MOTION     = 6;
const int VX_DEBUG_MATERIAL   = 7;

bool VxFeature(int flag) { return (u_Frame.settings.x & flag) != 0; }

// ---- Scene buffers --------------------------------------------------------------------------
struct VxMaterial {          // mirrors voxel::GpuMaterial (32 bytes)
    vec4 colorEmissive;      // rgb = albedo (linear), a = emissive strength
    vec4 params;             // x = roughness, y = metallic, z = per-voxel variation, w = unused
};

struct VxInstance {          // mirrors voxel::GpuInstance (224 bytes)
    mat4 localToWorld;       // chunk-local voxel units -> render space
    mat4 worldToLocal;
    mat4 prevLocalToWorld;   // previous frame, in the CURRENT render space
    ivec4 boundsMinSlot;     // xyz = voxel bounds min, w = chunk slot
    ivec4 boundsMaxPalette;  // xyz = voxel bounds max (exclusive), w = palette * 256
};

struct VxBvhNode {           // mirrors voxel::GpuBvhNode (32 bytes)
    vec3 bmin; uint a;       // leaf: a = instance index, b != 0 ; inner: children a and a + 1, b == 0
    vec3 bmax; uint b;
};

struct VxLight {             // mirrors graphics::GpuLight (32 bytes)
    vec4 positionRadius;     // xyz = position (render space), w = radius of influence (m)
    vec4 colorSize;          // rgb = radiant intensity, a = source radius (m) for soft shadows
};

struct VxParticle {          // mirrors fx::GpuParticle (48 bytes)
    vec4 positionSize;       // xyz = position (render space), w = half size (m)
    vec4 color;              // linear rgb, a = opacity
    vec4 params;             // x = emissive multiplier, y = shape (0 square, 1 soft disc), z = unlit
};

const uint VX_OCC_STRIDE = 1042u;    // uint32 words per chunk in u_Occ (voxel::kOccupancyWords)
const uint VX_ID_STRIDE  = 8192u;    // uint32 words per chunk in u_Ids (32768 ids, 4 per word)

#ifdef VX_NEED_TRACE
#define VX_NEED_INSTANCES
layout(set = 0, binding = 1, std430) readonly buffer WorldGridBuffer { uint data[]; } u_Grid;
layout(set = 0, binding = 2, std430) readonly buffer ChunkOccBuffer  { uint data[]; } u_Occ;
layout(set = 0, binding = 3, std430) readonly buffer ChunkIdBuffer   { uint data[]; } u_Ids;
layout(set = 0, binding = 6, std430) readonly buffer BvhBuffer       { VxBvhNode nodes[]; } u_Bvh;
#endif

#ifdef VX_NEED_MATERIALS
layout(set = 0, binding = 4, std430) readonly buffer MaterialBuffer  { VxMaterial materials[]; } u_Materials;
#endif

#ifdef VX_NEED_INSTANCES
layout(set = 0, binding = 5, std430) readonly buffer InstanceBuffer  { VxInstance instances[]; } u_Instances;
#endif

#ifdef VX_NEED_DRAW_LIST
layout(set = 0, binding = 7, std430) readonly buffer DrawListBuffer  { uint indices[]; } u_DrawList;
#endif

#ifdef VX_NEED_LIGHTS
layout(set = 0, binding = 8, std430) readonly buffer LightBuffer     { VxLight lights[]; } u_Lights;
#endif

#ifdef VX_NEED_PARTICLES
layout(set = 0, binding = 9, std430) readonly buffer ParticleBuffer  { VxParticle particles[]; } u_Particles;
#endif

// ---- Accessors for shared/trace.glsl ---------------------------------------------------------
#ifdef VX_NEED_TRACE
#define VX_GRID_ENTRY(i)                 u_Grid.data[(i)]
#define VX_GRID_SIZE                     u_Frame.gridSize.xyz
#define VX_GRID_MIN_CHUNK                u_Frame.gridMinChunk.xyz
#define VX_GRID_ORIGIN                   vec3(0.0)
#define VX_VOXEL_SIZE                    u_Frame.cameraPosVoxel.w
#define VX_INV_VOXEL_SIZE                (1.0 / u_Frame.cameraPosVoxel.w)
#define VX_OCC_WORD(slot, w)             u_Occ.data[(slot) * VX_OCC_STRIDE + (w)]
#define VX_VOXEL_ID(slot, k)             ((u_Ids.data[(slot) * VX_ID_STRIDE + ((k) >> 2u)] >> (((k) & 3u) * 8u)) & 0xFFu)
#define VX_BVH_NODE_COUNT                uint(u_Frame.counts.y)
#define VX_BVH_MIN(n)                    u_Bvh.nodes[(n)].bmin
#define VX_BVH_MAX(n)                    u_Bvh.nodes[(n)].bmax
#define VX_BVH_A(n)                      u_Bvh.nodes[(n)].a
#define VX_BVH_B(n)                      u_Bvh.nodes[(n)].b
#define VX_INSTANCE_WORLD_TO_LOCAL(i)    u_Instances.instances[(i)].worldToLocal
#define VX_INSTANCE_BOUNDS_MIN(i)        u_Instances.instances[(i)].boundsMinSlot.xyz
#define VX_INSTANCE_BOUNDS_MAX(i)        u_Instances.instances[(i)].boundsMaxPalette.xyz
#define VX_INSTANCE_SLOT(i)              uint(u_Instances.instances[(i)].boundsMinSlot.w)
#include "shared/trace.glsl"
#endif

// ---- G-buffer ---------------------------------------------------------------------------------
//   attachment 0  R32_UINT             bits 0..23 global material index (palette * 256 + voxel id),
//                                      bits 24..31 per-voxel variation byte
//   attachment 1  A2B10G10R10_UNORM    rgb = world normal * 0.5 + 0.5
//   attachment 2  R16G16_SFLOAT        screen-space motion: uv(current) - uv(previous)
//   depth         D32_SFLOAT           reverse-Z: near / viewDistance, 0 = sky
const uint VX_GBUFFER_SKY   = 0u;            // material word of a pixel nothing was drawn to
const uint VX_GBUFFER_DEBUG = 0x00FFFFFFu;   // "unlit debug colour stored in the normal target"

uint VxPackMaterial(uint globalMaterial, uint variation) { return (globalMaterial & 0x00FFFFFFu) | (variation << 24u); }
uint VxMaterialIndex(uint packed) { return packed & 0x00FFFFFFu; }
float VxMaterialVariation(uint packed) { return float(packed >> 24u) * (1.0 / 255.0); }
vec4 VxEncodeNormal(vec3 n) { return vec4(n * 0.5 + 0.5, 0.0); }
vec3 VxDecodeNormal(vec4 v) { return normalize(v.xyz * 2.0 - 1.0); }

// Stable per-voxel hash (0..255) used for the material "variation" brightness jitter.
uint VxVoxelHash(ivec3 v) {
    uint h = uint(v.x) * 0x9E3779B1u ^ uint(v.y) * 0x85EBCA77u ^ uint(v.z) * 0xC2B2AE3Du;
    h ^= h >> 15u; h *= 0x2C1B3C6Du; h ^= h >> 12u;
    return h & 0xFFu;
}

// ---- Camera helpers -----------------------------------------------------------------------------
// uv in [0,1]^2 with (0,0) at the top-left of the image (Vulkan convention).
// Returns the normalised view ray through that point, including this frame's TAA jitter, so it
// matches what the rasteriser does with u_Frame.viewProj.
vec3 VxCameraRay(vec2 uv) {
    vec2 ndc = uv * 2.0 - 1.0 - u_Frame.jitter.xy;
    return normalize(u_Frame.cameraForward.xyz + ndc.x * u_Frame.cameraRight.xyz - ndc.y * u_Frame.cameraUp.xyz);
}

// Reverse-Z depth for a point at distance t along a normalised view ray `rd`.
float VxDepthFromRay(vec3 rd, float t) {
    return u_Frame.cameraPos.w / max(t * dot(rd, u_Frame.cameraForward.xyz), 1.0e-6);
}

// Inverse of VxDepthFromRay: distance along the view ray for a depth buffer value (> 0).
float VxRayDistanceFromDepth(vec3 rd, float depth) {
    return u_Frame.cameraPos.w / (depth * max(dot(rd, u_Frame.cameraForward.xyz), 1.0e-6));
}

// Screen-space motion vector (uv units) of a render-space point that was at prevPos last frame.
vec2 VxMotionVector(vec3 pos, vec3 prevPos) {
    vec4 c = u_Frame.viewProjNoJitter * vec4(pos, 1.0);
    vec4 p = u_Frame.prevViewProjNoJitter * vec4(prevPos, 1.0);
    return (c.xy / c.w - p.xy / p.w) * 0.5;
}

#endif // VX_COMMON_GLSL
