#version 460
#extension GL_EXT_scalar_block_layout : enable

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

uint GetVoxel(uint chunkIdx, ivec3 pos) {
    if (pos.x < 0 || pos.x >= 32 || pos.y < 0 || pos.y >= 32 || pos.z < 0 || pos.z >= 32) return 0;
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

    // 1. Ray Setup
    vec3 localCamPos = (obj.invModel * vec4(cam.position.xyz, 1.0)).xyz;
    vec3 localDir = normalize(v_LocalPos - localCamPos);

    // 2. Intersect Box (0..32)
    float tNear, tFar;
    if (!IntersectAABB(localCamPos, localDir, vec3(0.0), vec3(32.0), tNear, tFar)) {
        discard; 
    }

    // 3. Robust Entry Point
    // Epsilon must be slightly larger to ensure we step INSIDE the box boundary
    float epsilon = 0.001; 
    float tCurrent = max(0.0, tNear);
    
    // Move slightly along the ray to avoid precision issues at the exact boundary
    vec3 rayPos = localCamPos + localDir * (tCurrent + epsilon);
    
    ivec3 mapPos = ivec3(floor(rayPos));
    // Clamp is still good, but now rayPos is safer
    mapPos = clamp(mapPos, ivec3(0), ivec3(31));

    vec3 deltaDist = abs(1.0 / localDir);
    ivec3 stepDir = ivec3(sign(localDir));
    
    // Robust sideDist calculation
    vec3 sideDist;
    sideDist.x = (sign(localDir.x) * (vec3(mapPos).x - rayPos.x) + (sign(localDir.x) * 0.5) + 0.5) * deltaDist.x;
    sideDist.y = (sign(localDir.y) * (vec3(mapPos).y - rayPos.y) + (sign(localDir.y) * 0.5) + 0.5) * deltaDist.y;
    sideDist.z = (sign(localDir.z) * (vec3(mapPos).z - rayPos.z) + (sign(localDir.z) * 0.5) + 0.5) * deltaDist.z;

    ivec3 mask = ivec3(0);
    
    // Ensure we don't march forever if something goes wrong
    int maxSteps = 128; 

    // 4. Raymarch
    for (int i = 0; i < maxSteps; i++) {
        // Bounds Check
        if (mapPos.x < 0 || mapPos.x >= 32 || mapPos.y < 0 || mapPos.y >= 32 || mapPos.z < 0 || mapPos.z >= 32) break;

        // Voxel Check
        uint voxel = GetVoxel(obj.chunkIndex, mapPos);
        if (voxel != 0) {
            vec4 color = matBuffer.materials[voxel].color;
            
            // Debug: Fallback to Magenta if color is somehow zero (though C++ fix handles this)
            if (length(color.rgb) < 0.01 && color.a < 0.01) color = vec4(1.0, 0.0, 1.0, 1.0);

            // Calculate Normal
            vec3 normal = vec3(0.0);
            if (mask.x != 0) normal.x = -float(stepDir.x);
            else if (mask.y != 0) normal.y = -float(stepDir.y);
            else if (mask.z != 0) normal.z = -float(stepDir.z);
            
            // Initial hit case (ray starts inside/on surface)
            if (mask == ivec3(0)) normal = -localDir;

            // Simple Lighting
            vec3 lightDir = normalize(vec3(0.5, 1.0, 0.5));
            float diff = max(0.0, dot(normal, lightDir));
            float ambient = 0.3;
            float light = ambient + diff * 0.7;
            
            outColor = vec4(color.rgb * light, 1.0);
            
            // Depth Calculation
            float tHit = tCurrent;
            // Fix: Add distance ONLY if we actually stepped
            if (mask.x != 0) tHit += sideDist.x - deltaDist.x;
            else if (mask.y != 0) tHit += sideDist.y - deltaDist.y;
            else if (mask.z != 0) tHit += sideDist.z - deltaDist.z;
            
            vec3 hitWorldPos = (obj.model * vec4(localCamPos + localDir * tHit, 1.0)).xyz;
            vec4 clipPos = cam.viewProj * vec4(hitWorldPos, 1.0);
            gl_FragDepth = clipPos.z / clipPos.w;
            
            return;
        }

        // Step
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