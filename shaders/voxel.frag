#version 460
#extension GL_EXT_scalar_block_layout : enable

/**
 * @brief Voxel Raymarching Fragment Shader.
 * @details Supports Global Inter-Object Shadows via BVH TLAS Traversal.
 */

layout (depth_greater) out float gl_FragDepth;

layout(location = 0) in flat uint v_InstanceIndex;
layout(location = 1) in vec3 v_LocalPos; 

layout(location = 0) out vec4 outColor;

// --- Structures ---

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
    uint voxelIDs[8192];  
    uint voxelFlags[2048]; 
    uint hierarchy[16];    
};

struct BVHNode {
    vec3 min; 
    uint leftOrInstance; 
    vec3 max; 
    uint rightOrCount; 
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

layout(binding = 4, std140) uniform Light {
    vec4 direction; // xyz = dir, w = intensity
    vec4 color;     // rgb = color, a = ambient
} sun;

layout(binding = 5, std430) readonly buffer TLAS {
    BVHNode nodes[];
} tlas;

// --- Helpers ---

bool IsBlockOccupied(uint chunkIdx, ivec3 mapPos) {
    ivec3 bPos = mapPos >> 2; 
    uint hIndex = bPos.x + bPos.y * 8 + bPos.z * 64;
    return (chunkBuffer.chunks[chunkIdx].hierarchy[hIndex >> 5] & (1u << (hIndex & 31))) != 0;
}

uint GetVoxel(uint chunkIdx, ivec3 pos) {
    uint index = pos.x + pos.y * 32 + pos.z * 1024;
    return (chunkBuffer.chunks[chunkIdx].voxelIDs[index >> 2] >> ((index & 3) << 3)) & 0xFF;
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

bool TraceShadowChunk(vec3 ro, vec3 rd, uint chunkIdx, float maxDist) {
    ivec3 mapPos = ivec3(floor(ro));
    
    if (mapPos.x < 0 || mapPos.x >= 32 || mapPos.y < 0 || mapPos.y >= 32 || mapPos.z < 0 || mapPos.z >= 32) {
       // Should ideally march to entry point if starting outside, but AABB intersection usually handles this.
    }

    vec3 deltaDist = abs(1.0 / rd);
    ivec3 stepDir = ivec3(sign(rd));
    
    vec3 sideDist;
    sideDist.x = (sign(rd.x) * (vec3(mapPos).x - ro.x) + (sign(rd.x) * 0.5) + 0.5) * deltaDist.x;
    sideDist.y = (sign(rd.y) * (vec3(mapPos).y - ro.y) + (sign(rd.y) * 0.5) + 0.5) * deltaDist.y;
    sideDist.z = (sign(rd.z) * (vec3(mapPos).z - ro.z) + (sign(rd.z) * 0.5) + 0.5) * deltaDist.z;

    float distTraveled = 0.0;
    const int MAX_SHADOW_STEPS = 64; 

    for (int i = 0; i < MAX_SHADOW_STEPS; i++) {
        if (mapPos.x < 0 || mapPos.x >= 32 || mapPos.y < 0 || mapPos.y >= 32 || mapPos.z < 0 || mapPos.z >= 32) return false;
        
        if (IsBlockOccupied(chunkIdx, mapPos)) {
            uint voxel = GetVoxel(chunkIdx, mapPos);
            if (voxel != 0) return true; // Shadow Hit
        }

        if (sideDist.x < sideDist.y) {
            if (sideDist.x < sideDist.z) {
                distTraveled = sideDist.x;
                sideDist.x += deltaDist.x;
                mapPos.x += stepDir.x;
            } else {
                distTraveled = sideDist.z;
                sideDist.z += deltaDist.z;
                mapPos.z += stepDir.z;
            }
        } else {
            if (sideDist.y < sideDist.z) {
                distTraveled = sideDist.y;
                sideDist.y += deltaDist.y;
                mapPos.y += stepDir.y;
            } else {
                distTraveled = sideDist.z;
                sideDist.z += deltaDist.z;
                mapPos.z += stepDir.z;
            }
        }
        
        if (distTraveled > maxDist) return false;
    }
    return false;
}

/**
 * @brief Checks global shadows using the TLAS (Top-Level Acceleration Structure).
 * @details Replaces O(N) loop with O(log N) stack-based traversal.
 */
float CalculateGlobalShadow(vec3 worldPos, vec3 lightDir) {
    if (cam.objectCount == 0) return 1.0;

    vec3 shadowRoWorld = worldPos + lightDir * 0.05; 
    vec3 invDir = 1.0 / lightDir;

    // TLAS Traversal Stack
    int stack[32];
    int stackPtr = 0;
    stack[stackPtr++] = 0; // Push Root Node (Index 0)

    while (stackPtr > 0) {
        int nodeIdx = stack[--stackPtr];
        BVHNode node = tlas.nodes[nodeIdx];

        // 1. Intersect AABB
        vec3 t0s = (node.min - shadowRoWorld) * invDir;
        vec3 t1s = (node.max - shadowRoWorld) * invDir;
        vec3 tsmaller = min(t0s, t1s);
        vec3 tbigger = max(t0s, t1s);
        float tmin = max(tsmaller.x, max(tsmaller.y, tsmaller.z));
        float tmax = min(tbigger.x, min(tbigger.y, tbigger.z));

        if (tmax >= tmin && tmax > 0.0) {
            
            // Check if Leaf (Sentinel 0xFFFFFFFF)
            if (node.rightOrCount == 0xFFFFFFFF) {
                // Leaf Node -> Test Object
                uint objIdx = node.leftOrInstance;
                Object obj = objBuffer.objects[objIdx];
                
                // Transform to Local
                vec3 localRo = (obj.invModel * vec4(shadowRoWorld, 1.0)).xyz;
                vec3 localRd = normalize(mat3(obj.invModel) * lightDir);

                // Detailed Chunk Trace
                float tn, tf;
                if (IntersectAABB(localRo, localRd, vec3(0.0), vec3(32.0), tn, tf)) {
                    vec3 marchStart = localRo;
                    if (tn > 0.0) marchStart += localRd * (tn + 1e-4);
                    float maxDist = (tn > 0.0) ? (tf - tn) : tf;

                    if (TraceShadowChunk(marchStart, localRd, obj.chunkIndex, maxDist)) {
                        return 0.0; // Shadow Hit
                    }
                }

            } else {
                // Internal Node -> Push Children
                stack[stackPtr++] = int(node.rightOrCount);
                stack[stackPtr++] = int(node.leftOrInstance);
            }
        }
    }
    
    return 1.0; // Lit
}

// --- Main ---

void main() {
    Object obj = objBuffer.objects[v_InstanceIndex];

    vec3 localCamPos = (obj.invModel * vec4(cam.position.xyz, 1.0)).xyz;
    vec3 rawDir = v_LocalPos - localCamPos;
    if (dot(rawDir, rawDir) < 1e-6) discard;
    vec3 localDir = normalize(rawDir);

    float tNear, tFar;
    if (!IntersectAABB(localCamPos, localDir, vec3(0.0), vec3(32.0), tNear, tFar)) {
        discard;
    }
    
    float tStart = max(0.0, tNear);
    vec3 rayPos = localCamPos + localDir * (tStart + 1e-4);
    
    ivec3 mapPos = ivec3(floor(rayPos));
    mapPos = clamp(mapPos, ivec3(0), ivec3(31));

    vec3 safeDir = localDir;
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

    const int MAX_STEPS = 128; 
    for (int i = 0; i < MAX_STEPS; i++) {
        if (mapPos.x < 0 || mapPos.x >= 32 || mapPos.y < 0 || mapPos.y >= 32 || mapPos.z < 0 || mapPos.z >= 32) break;

        if (IsBlockOccupied(obj.chunkIndex, mapPos)) {
            uint voxel = GetVoxel(obj.chunkIndex, mapPos);
            
            if (voxel != 0) {
                uint globalMatID = voxel + obj.paletteOffset;
                vec4 baseColor = vec4(1.0, 0.0, 1.0, 1.0); 
                if (globalMatID < matBuffer.materials.length()) {
                    baseColor = matBuffer.materials[globalMatID].color;
                }

                vec3 normal = vec3(0.0);
                if (mask.x != 0) normal.x = -float(stepDir.x);
                else if (mask.y != 0) normal.y = -float(stepDir.y);
                else if (mask.z != 0) normal.z = -float(stepDir.z);
                if (mask == ivec3(0)) normal = -localDir; 

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

                vec3 lightDir = normalize(sun.direction.xyz);
                
                // --- FIX: Use Transpose Inverse for Correct Normal Transformation ---
                // obj.invModel is already the Inverse. We just transpose it.
                // This correctly handles non-uniform scaling and rotation.
                vec3 worldNormal = normalize(transpose(mat3(obj.invModel)) * normal);
                
                float diff = max(dot(worldNormal, lightDir), 0.0);
                
                float shadow = 1.0;
                if (diff > 0.0) {
                    shadow = CalculateGlobalShadow(hitWorldPos, lightDir);
                }

                vec3 ambient = sun.color.a * baseColor.rgb;
                vec3 diffuse = diff * sun.color.rgb * sun.direction.w * shadow * baseColor.rgb;
                
                outColor = vec4(ambient + diffuse, baseColor.a);
                
                vec4 clipPos = cam.viewProj * vec4(hitWorldPos, 1.0);
                gl_FragDepth = clipPos.z / clipPos.w;
                
                return;
            }
        }

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