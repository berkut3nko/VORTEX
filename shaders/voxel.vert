#version 460
#extension GL_EXT_scalar_block_layout : enable

/**
 * @brief Vertex Shader for Voxel Raymarching proxy geometry.
 * @details Generates screen-space geometry for the voxel chunks (AABBs).
 */

layout(location = 0) out flat uint v_InstanceIndex;
layout(location = 1) out vec3 v_LocalPos; 

/**
 * @brief Per-instance object data.
 */
struct Object {
    mat4 model;
    mat4 invModel;
    uint chunkIndex;
    uint paletteOffset;
    uint flags;
    uint _pad;
};

/**
 * @brief Camera Uniform Buffer Object.
 */
layout(binding = 0, std140) uniform Camera {
    mat4 viewInverse;
    mat4 projInverse;
    vec4 position;
    vec4 direction;
    uint objectCount;
    mat4 viewProj;
} cam;

layout(binding = 2, std430) readonly buffer Objects {
    Object objects[];
} objBuffer;

// Cube vertices (0..32 range)
const vec3 CUBE_VERTS[8] = vec3[](
    vec3(0, 0, 0), vec3(32, 0, 0), vec3(0, 32, 0), vec3(32, 32, 0),
    vec3(0, 0, 32), vec3(32, 0, 32), vec3(0, 32, 32), vec3(32, 32, 32)
);

/**
 * @brief Cube indices.
 * @warning Order fixed for Bottom face to point outward (Down).
 */
const int CUBE_INDICES[36] = int[](
    // Top (+Y)
    2,6,7, 7,3,2, 
    // Bottom (-Y) - FIXED WINDING (0,5,4 instead of 0,4,5)
    0,5,4, 0,1,5, 
    // Front (-Z)
    0,2,3, 3,1,0, 
    // Left (-X)
    4,6,2, 2,0,4, 
    // Back (+Z)
    5,7,6, 6,4,5, 
    // Right (+X)
    1,3,7, 7,5,1  
);

void main() {
    v_InstanceIndex = gl_InstanceIndex;
    
    Object obj = objBuffer.objects[v_InstanceIndex];
    
    int vertexIndex = CUBE_INDICES[gl_VertexIndex];
    vec3 localPos = CUBE_VERTS[vertexIndex];
    v_LocalPos = localPos;

    vec4 worldPos = obj.model * vec4(localPos, 1.0);
    gl_Position = cam.viewProj * worldPos;
}