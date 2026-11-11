#version 460
#extension GL_GOOGLE_include_directive : require
// G-buffer, dynamic objects: one proxy box (the instance's tight voxel bounds) per instance.
// vkCmdDraw(36, count, 0, firstInstance); gl_InstanceIndex includes firstInstance and indexes
// the draw list.
#define VX_NEED_INSTANCES
#define VX_NEED_DRAW_LIST
#include "include/common.glsl"
#include "shared/cube.glsl"

layout(location = 0) flat out uint v_Instance;

void main() {
    uint instance = u_DrawList.indices[gl_InstanceIndex];
    vec3 boundsMin = vec3(u_Instances.instances[instance].boundsMinSlot.xyz);
    vec3 boundsMax = vec3(u_Instances.instances[instance].boundsMaxPalette.xyz);

    vec3 corner = VxCubeCorner(gl_VertexIndex);
    vec3 local = mix(boundsMin, boundsMax, corner);
    vec4 world = u_Instances.instances[instance].localToWorld * vec4(local, 1.0);

    v_Instance = instance;
    gl_Position = u_Frame.viewProj * vec4(world.xyz, 1.0);
}
