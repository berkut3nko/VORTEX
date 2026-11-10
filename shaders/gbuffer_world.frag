#version 460
#extension GL_GOOGLE_include_directive : require
// G-buffer, streaming world: one primary ray per pixel through the chunk grid.
#define VX_NEED_TRACE
#include "include/common.glsl"
#include "shared/shading.glsl"

layout(location = 0) in vec2 v_Uv;

layout(location = 0) out uint o_Material;
layout(location = 1) out vec4 o_Normal;
layout(location = 2) out vec2 o_Velocity;

void main() {
    vec3 rd = VxCameraRay(v_Uv);

    VxHit hit = VxMiss();
    if (!VxTraceWorld(u_Frame.cameraPosVoxel.xyz, rd, 0.0, 1.0e9, hit)) {
        discard;
    }

    // The world is traced in voxel units; everything downstream works in metres.
    float t = hit.t * u_Frame.cameraPosVoxel.w;
    vec3 pos = u_Frame.cameraPos.xyz + rd * t;

    if (u_Frame.settings.y == VX_DEBUG_STEPS) {
        o_Material = VX_GBUFFER_DEBUG;
        o_Normal = vec4(VxHeatColor(float(hit.steps) * (1.0 / 96.0)), 0.0);
    } else {
        // Variation is keyed on the absolute voxel so it does not change when the window moves.
        ivec3 absVoxel = hit.voxel + u_Frame.gridMinChunk.xyz * VX_CHUNK_SIZE;
        o_Material = VxPackMaterial(hit.material, VxVoxelHash(absVoxel));
        o_Normal = VxEncodeNormal(hit.normal);
    }

    // Static geometry: it was at the same render-space position last frame.
    o_Velocity = VxMotionVector(pos, pos);
    // Never 0 (that means sky) and never above the near plane value.
    gl_FragDepth = clamp(VxDepthFromRay(rd, t), 1.0e-7, 1.0);
}
