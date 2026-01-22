#version 460
#extension GL_EXT_scalar_block_layout : enable

/**
 * @brief Vertex Shader for Voxel Raymarching (Proxy Geometry).
 * @details Renders a cube that serves as a proxy volume for the raymarching fragment shader.
 * Handles transform and vertex generation using hardcoded arrays.
 */

layout(location = 0) out flat uint v_InstanceIndex;
layout(location = 1) out vec3 v_LocalPos; // Entry point (back face) or vertex pos

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
 * @brief Camera Uniform Buffer Object (std140).
 */
layout(binding = 0, std140) uniform Camera {
    mat4 viewInverse;
    mat4 projInverse;
    vec4 position;
    vec4 direction;
    uint objectCount;
    mat4 viewProj; // Combined View-Projection matrix
} cam;

/**
 * @brief Storage buffer containing all scene objects.
 */
layout(binding = 2, std430) readonly buffer Objects {
    Object objects[];
} objBuffer;

// Cube vertices (0..32 range) to match chunk dimensions
const vec3 CUBE_VERTS[8] = vec3[](
    vec3(0, 0, 0), vec3(32, 0, 0), vec3(0, 32, 0), vec3(32, 32, 0),
    vec3(0, 0, 32), vec3(32, 0, 32), vec3(0, 32, 32), vec3(32, 32, 32)
);

/**
 * @brief Cube indices defining the triangles.
 * @warning Winding order must be consistent (CCW for outward faces).
 */
const int CUBE_INDICES[36] = int[](
    // Top (+Y) - Normal Up
    2,6,7, 7,3,2, 
    
    // Bottom (-Y) - Normal Down (FIXED: Winding flipped to point outward)
    0,5,4, 0,1,5, 
    
    // Front (-Z) - Normal Out
    0,2,3, 3,1,0, 
    
    // Left (-X) - Normal Out
    4,6,2, 2,0,4, 
    
    // Back (+Z) - Normal Out
    5,7,6, 6,4,5, 
    
    // Right (+X) - Normal Out
    1,3,7, 7,5,1  
);

void main() {
    v_InstanceIndex = gl_InstanceIndex;
    
    // 1. Get Object Data
    Object obj = objBuffer.objects[v_InstanceIndex];
    
    // 2. Get Vertex Position (Local 0..32 space)
    int vertexIndex = CUBE_INDICES[gl_VertexIndex];
    vec3 localPos = CUBE_VERTS[vertexIndex];
    v_LocalPos = localPos;

    // 3. Transform to Clip Space
    vec4 worldPos = obj.model * vec4(localPos, 1.0);
    gl_Position = cam.viewProj * worldPos;
}