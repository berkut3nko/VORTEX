#version 460
#extension GL_EXT_scalar_block_layout : enable

/**
 * @brief Voxel Raymarching Fragment Shader (Optimized Single-Level DDA).
 * @details 
 * 1. Uses Single-Level DDA for geometric stability (no artifacts).
 * 2. Uses Geometric Depth Reconstruction for smooth surfaces.
 * 3. OPTIMIZATION: Uses Hierarchy Bitmask to skip Voxel Memory Fetch in empty areas.
 */

layout (depth_greater) out float gl_FragDepth;

layout(location = 0) in flat uint v_InstanceIndex;
layout(location = 1) in vec3 v_LocalPos; 

layout(location = 0) out vec4 outColor;

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
    uint paletteOffset;
    uint flags;
    uint _pad;
};

struct Chunk {
    uint voxels[8192];   
    uint hierarchy[16]; 
};

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
 * @brief Checks if a 4x4x4 block contains ANY voxels using the bitmask.
 * @return true if the block might contain voxels, false if definitely empty.
 */
bool IsBlockOccupied(uint chunkIdx, ivec3 mapPos) {
    // MapPos (0..31) -> BlockPos (0..7)
    ivec3 bPos = mapPos >> 2; 
    uint hIndex = bPos.x + bPos.y * 8 + bPos.z * 64;
    // Check specific bit in the packed hierarchy array
    return (chunkBuffer.chunks[chunkIdx].hierarchy[hIndex >> 5] & (1u << (hIndex & 31))) != 0;
}

uint GetVoxel(uint chunkIdx, ivec3 pos) {
    uint index = pos.x + pos.y * 32 + pos.z * 1024;
    return (chunkBuffer.chunks[chunkIdx].voxels[index >> 2] >> ((index & 3) << 3)) & 0xFF;
}

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

void main() {
    Object obj = objBuffer.objects[v_InstanceIndex];

    // --- 1. Ray Setup ---
    vec3 localCamPos = (obj.invModel * vec4(cam.position.xyz, 1.0)).xyz;
    vec3 localDir = normalize(v_LocalPos - localCamPos);

    float tNear, tFar;
    if (!IntersectAABB(localCamPos, localDir, vec3(0.0), vec3(32.0), tNear, tFar)) {
        discard; 
    }
    
    float tStart = max(0.0, tNear);
    float epsilon = 1e-4; 
    vec3 rayPos = localCamPos + localDir * (tStart + epsilon);
    
    ivec3 mapPos = ivec3(floor(rayPos));
    mapPos = clamp(mapPos, ivec3(0), ivec3(31));

    vec3 deltaDist = abs(1.0 / localDir);
    ivec3 stepDir = ivec3(sign(localDir));
    
    vec3 sideDist;
    sideDist.x = (sign(localDir.x) * (vec3(mapPos).x - rayPos.x) + (sign(localDir.x) * 0.5) + 0.5) * deltaDist.x;
    sideDist.y = (sign(localDir.y) * (vec3(mapPos).y - rayPos.y) + (sign(localDir.y) * 0.5) + 0.5) * deltaDist.y;
    sideDist.z = (sign(localDir.z) * (vec3(mapPos).z - rayPos.z) + (sign(localDir.z) * 0.5) + 0.5) * deltaDist.z;

    ivec3 mask = ivec3(0);

    // --- 2. Optimized DDA Loop ---
    for (int i = 0; i < 128; i++) {
        // Bounds Check
        if (mapPos.x < 0 || mapPos.x >= 32 || mapPos.y < 0 || mapPos.y >= 32 || mapPos.z < 0 || mapPos.z >= 32) break;

        // --- OPTIMIZATION: Hierarchy Check ---
        // Before fetching the actual voxel data (expensive memory read),
        // check if the 4x4x4 block is empty using the cached bitmask.
        bool possibleHit = IsBlockOccupied(obj.chunkIndex, mapPos);

        if (possibleHit) {
            // Only perform GetVoxel if the hierarchy says there might be something here
            uint voxel = GetVoxel(obj.chunkIndex, mapPos);
            
            if (voxel != 0) {
                // --- HIT RESPONSE ---
                vec4 color = matBuffer.materials[voxel].color;
                if (color.a < 0.01) color = vec4(1.0, 0.0, 1.0, 1.0); 

                vec3 normal = vec3(0.0);
                if (mask.x != 0) normal.x = -float(stepDir.x);
                else if (mask.y != 0) normal.y = -float(stepDir.y);
                else if (mask.z != 0) normal.z = -float(stepDir.z);
                if (mask == ivec3(0)) normal = -localDir;

                float diff = max(0.0, dot(normal, normalize(vec3(0.5, 1.0, 0.5))));
                outColor = vec4(color.rgb * (0.3 + diff * 0.7), 1.0);
                
                // Geometric Reconstruction for smooth depth
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

        // --- STEP ---
        // Even if block is empty, we do standard small steps.
        // This ensures we don't skip corners or create artifacts,
        // but the loop runs much faster because we skip the heavy GetVoxel() call.
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