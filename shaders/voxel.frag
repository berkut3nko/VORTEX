#version 460
#extension GL_EXT_scalar_block_layout : enable

/**
<<<<<<< Updated upstream
 * @brief Voxel Raymarching Fragment Shader (Optimized Single-Level DDA + SoA).
 * @details 
 * 1. Uses Single-Level DDA for geometric stability (no artifacts on steps).
 * 2. Uses Structure of Arrays (SoA) memory layout for better cache efficiency.
 * 3. OPTIMIZATION: Uses Hierarchy Bitmask to skip Voxel Memory Fetch in empty areas.
=======
 * @brief Voxel Raymarching Fragment Shader.
 * @details Performs DDA (Digital Differential Analyzer) traversal through the voxel grid.
 * Supports:
 * - Local space raymarching (Object space).
 * - Hierarchy-based skipping (Occupancy Map).
 * - Palette Offset for material mapping.
 * - Blinn-Phong Lighting Model.
>>>>>>> Stashed changes
 */

layout (depth_greater) out float gl_FragDepth;

// Inputs from Vertex Shader
layout(location = 0) in flat uint v_InstanceIndex;
layout(location = 1) in vec3 v_LocalPos; 

// Output Color
layout(location = 0) out vec4 outColor;

// --- Structures (Must match C++ std430 layout) ---

struct Material {
    vec4 color;
    float density; float friction; float restitution; float hardness;
    float health; float flammability; float heatRes; uint flags;
    uint textureIndex; uint soundImpact; uint soundDestroy; uint _pad;
};

struct Object {
    mat4 model;
    mat4 invModel;
    uint chunkIndex;
    uint paletteOffset; // Offset into the global material palette
    uint flags;
    uint _pad;
};

<<<<<<< Updated upstream
// --- UPDATED Chunk Layout (SoA) ---
// Matches src/voxel/Chunk.cppm
struct Chunk {
    uint voxelIDs[8192];   // Material Indices (8 bits/voxel). Packed 4 per uint.
    uint voxelFlags[2048]; // Flags (2 bits/voxel). Packed 16 per uint.
    uint hierarchy[16];    // 512-bit Hierarchy Mask.
=======
// SoA Layout for 32^3 Chunk
struct Chunk {
    uint voxelIDs[8192];   // 8 bits per voxel (Packed 4 per uint)
    uint voxelFlags[2048]; // 2 bits per voxel (Packed 16 per uint)
    uint hierarchy[16];    // 512-bit Hierarchy Mask (1 bit per 4x4x4 block)
>>>>>>> Stashed changes
};

// --- Bindings ---

layout(binding = 0, std140) uniform Camera {
    mat4 viewInverse;
    mat4 projInverse;
    vec4 position;
    vec4 direction;
    uint objectCount;
    mat4 viewProj;
} cam;

layout(binding = 1, std430) readonly buffer Materials {
    Material materials[];
} matBuffer;

layout(binding = 2, std430) readonly buffer Objects {
    Object objects[];
} objBuffer;

layout(binding = 3, std430) readonly buffer Chunks {
    Chunk chunks[];
} chunkBuffer;

// --- Helpers ---

/**
 * @brief Checks if a 4x4x4 block contains any voxels.
 */
bool IsBlockOccupied(uint chunkIdx, ivec3 mapPos) {
    ivec3 bPos = mapPos >> 2; // Divide by 4
    uint hIndex = bPos.x + bPos.y * 8 + bPos.z * 64;
    return (chunkBuffer.chunks[chunkIdx].hierarchy[hIndex >> 5] & (1u << (hIndex & 31))) != 0;
}

/**
 * @brief Retrieves the voxel ID at a specific position.
 */
uint GetVoxel(uint chunkIdx, ivec3 pos) {
    uint index = pos.x + pos.y * 32 + pos.z * 1024;
<<<<<<< Updated upstream
    // Access voxelIDs array (SoA layout)
=======
    // Extract byte from packed uint array
>>>>>>> Stashed changes
    return (chunkBuffer.chunks[chunkIdx].voxelIDs[index >> 2] >> ((index & 3) << 3)) & 0xFF;
}

/**
 * @brief Intersects a ray with an Axis-Aligned Bounding Box.
 */
bool IntersectAABB(vec3 ro, vec3 rd, vec3 boxMin, vec3 boxMax, out float tNear, out float tFar) {
    vec3 invDir = 1.0 / rd;
    vec3 tbot = invDir * (boxMin - ro);
    vec3 ttop = invDir * (boxMax - ro);
    
    vec3 tmin = min(ttop, tbot);
    vec3 tmax = max(ttop, tbot);
    
    float t0 = max(max(tmin.x, tmin.y), tmin.z);
    float t1 = min(min(tmax.x, tmax.y), tmax.z);
    
    tNear = t0;
    tFar = t1;
    return t1 >= t0 && t1 > 0.0;
}

// --- Main ---

void main() {
    // 1. Setup Ray
    Object obj = objBuffer.objects[v_InstanceIndex];

    // Transform camera position to Object Local Space
    vec3 localCamPos = (obj.invModel * vec4(cam.position.xyz, 1.0)).xyz;
<<<<<<< Updated upstream
    // Safe normalize (avoid NaN if pos == camPos, unlikely but safe)
=======
    
    // Calculate Ray Direction in Local Space
>>>>>>> Stashed changes
    vec3 rawDir = v_LocalPos - localCamPos;
    if (dot(rawDir, rawDir) < 1e-6) discard;
    vec3 localDir = normalize(rawDir);

    // 2. Intersect with Chunk AABB (0,0,0 to 32,32,32)
    float tNear, tFar;
    if (!IntersectAABB(localCamPos, localDir, vec3(0.0), vec3(32.0), tNear, tFar)) {
        discard;
    }
    
    // Start raymarching from the entry point
    float tStart = max(0.0, tNear);
    vec3 rayPos = localCamPos + localDir * (tStart + 1e-4);
    
    ivec3 mapPos = ivec3(floor(rayPos));
    mapPos = clamp(mapPos, ivec3(0), ivec3(31));

<<<<<<< Updated upstream
    // Avoid division by zero in deltaDist
    vec3 safeDir = localDir;
=======
    // 3. Prepare DDA
    vec3 safeDir = localDir;
    // Prevent division by zero
>>>>>>> Stashed changes
    if (abs(safeDir.x) < 1e-6) safeDir.x = 1e-6;
    if (abs(safeDir.y) < 1e-6) safeDir.y = 1e-6;
    if (abs(safeDir.z) < 1e-6) safeDir.z = 1e-6;

    vec3 deltaDist = abs(1.0 / safeDir);
    ivec3 stepDir = ivec3(sign(safeDir));
    
    vec3 sideDist;
    sideDist.x = (sign(localDir.x) * (vec3(mapPos).x - rayPos.x) + (sign(localDir.x) * 0.5) + 0.5) * deltaDist.x;
    sideDist.y = (sign(localDir.y) * (vec3(mapPos).y - rayPos.y) + (sign(localDir.y) * 0.5) + 0.5) * deltaDist.y;
    sideDist.z = (sign(localDir.z) * (vec3(mapPos).z - rayPos.z) + (sign(localDir.z) * 0.5) + 0.5) * deltaDist.z;

    ivec3 mask = ivec3(0);

<<<<<<< Updated upstream
    // --- 2. Optimized Single-Level DDA Loop ---
    for (int i = 0; i < 64; i++) {
        // Bounds Check
=======
    // 4. Raymarching Loop
    const int MAX_STEPS = 128; // Increased steps for safety
    for (int i = 0; i < MAX_STEPS; i++) {
        // Exit if outside chunk
>>>>>>> Stashed changes
        if (mapPos.x < 0 || mapPos.x >= 32 || mapPos.y < 0 || mapPos.y >= 32 || mapPos.z < 0 || mapPos.z >= 32) break;

        // Hierarchical Optimization
        bool possibleHit = IsBlockOccupied(obj.chunkIndex, mapPos);

        if (possibleHit) {
            uint voxel = GetVoxel(obj.chunkIndex, mapPos);
            
            if (voxel != 0) {
<<<<<<< Updated upstream
                // --- HIT RESPONSE ---
                vec4 color = matBuffer.materials[voxel].color;
                if (color.a < 0.01) color = vec4(1.0, 0.0, 1.0, 1.0); // Debug pink for invisible materials
=======
                // --- Intersection Found ---
                
                // Calculate Global Material ID using Offset
                uint globalMatID = voxel + obj.paletteOffset;
                
                // Fetch Material Data
                vec4 baseColor = vec4(1.0, 0.0, 1.0, 1.0); // Error Pink
                if (globalMatID < matBuffer.materials.length()) {
                    baseColor = matBuffer.materials[globalMatID].color;
                }
>>>>>>> Stashed changes

                // Calculate Normal
                vec3 normal = vec3(0.0);
                if (mask.x != 0) normal.x = -float(stepDir.x);
                else if (mask.y != 0) normal.y = -float(stepDir.y);
                else if (mask.z != 0) normal.z = -float(stepDir.z);
                if (mask == ivec3(0)) normal = -localDir; // Fallback

<<<<<<< Updated upstream
                // Simple lighting
                float diff = max(0.0, dot(normal, normalize(vec3(0.5, 1.0, 0.5))));
                outColor = vec4(color.rgb * (0.3 + diff * 0.7), 1.0);
=======
                // --- Lighting (Blinn-Phong) ---
                vec3 lightPos = vec3(50.0, 100.0, 50.0); // Virtual Light (Local Space)
                vec3 lightDir = normalize(lightPos - rayPos);
                vec3 viewDir = -localDir;
                vec3 halfwayDir = normalize(lightDir + viewDir);

                // Ambient
                float ambientStrength = 0.3;
                vec3 ambient = ambientStrength * baseColor.rgb;
  
                // Diffuse
                float diff = max(dot(normal, lightDir), 0.0);
                vec3 diffuse = diff * baseColor.rgb;
>>>>>>> Stashed changes
                
                // Specular
                float specularStrength = 0.2;
                float spec = pow(max(dot(normal, halfwayDir), 0.0), 16.0);
                vec3 specular = specularStrength * spec * vec3(1.0); // White specular

                outColor = vec4(ambient + diffuse + specular, baseColor.a);
                
                // Depth Reconstruction
                float tHit = tStart;
                if (mask.x != 0) {
                     float planeX = float(mapPos.x) + (localDir.x < 0.0 ? 1.0 : 0.0);
                     tHit = (planeX - localCamPos.x) / localDir.x;
                } else if (mask.y != 0) {
                     float planeY = float(mapPos.y) + (localDir.y < 0.0 ? 1.0 : 0.0);
                     tHit = (planeY - localCamPos.y) / localDir.y;
                } else if (mask.z != 0) {
                     float planeZ = float(mapPos.z) + (localDir.z < 0.0 ? 1.0 : 0.0);
                     tHit = (planeZ - localCamPos.z) / localDir.z;
                }
                
                vec3 hitWorldPos = (obj.model * vec4(localCamPos + localDir * tHit, 1.0)).xyz;
                vec4 clipPos = cam.viewProj * vec4(hitWorldPos, 1.0);
                gl_FragDepth = clipPos.z / clipPos.w;
                
                return;
            }
        }

<<<<<<< Updated upstream
        // --- STEP (Single Level) ---
        // Even if block is empty, we step one voxel at a time.
        // This is slower than Hierarchical leaping but visually robust.
=======
        // --- Step Forward ---
>>>>>>> Stashed changes
        if (sideDist.x < sideDist.y) {
            if (sideDist.x < sideDist.z) {
                sideDist.x += deltaDist.x;
                mapPos.x += stepDir.x;
                mask = ivec3(1, 0, 0);
            } else {
                sideDist.z += deltaDist.z;
                mapPos.z += stepDir.z;
                mask = ivec3(0, 0, 1);
            }
        } else {
            if (sideDist.y < sideDist.z) {
                sideDist.y += deltaDist.y;
                mapPos.y += stepDir.y;
                mask = ivec3(0, 1, 0);
            } else {
                sideDist.z += deltaDist.z;
                mapPos.z += stepDir.z;
                mask = ivec3(0, 0, 1);
            }
        }
    }

    discard;
}