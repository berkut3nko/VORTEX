// =============================================================================
//  Play-bounds curtain ([wp-5]) - shared by composite.frag and C++ (see shared/vx_cpp.h).
// =============================================================================
//  A session's play bounds are four vertical walls (x = min x, x = max x, z = min z, z = max z).
//  Where the view ray crosses one in front of the scene it shows a faint sheet with a grid of
//  lines, strongest near the camera and near eye height. Render space, metres.
#ifndef VX_SHARED_BOUNDS_GLSL
#define VX_SHARED_BOUNDS_GLSL

VX_CONST_FLOAT(VX_CURTAIN_NEAR, 6.0f)          // full strength up to this distance (m)
VX_CONST_FLOAT(VX_CURTAIN_FAR, 24.0f)          // gone from this distance on (m)
VX_CONST_FLOAT(VX_CURTAIN_BAND_NEAR, 3.0f)     // full strength up to this height above / below the eye (m)
VX_CONST_FLOAT(VX_CURTAIN_BAND_FAR, 9.0f)      // gone from this height difference on (m)
VX_CONST_FLOAT(VX_CURTAIN_BASE, 0.10f)         // opacity of the sheet between the lines
VX_CONST_FLOAT(VX_CURTAIN_LINE, 0.45f)         // opacity added on a line
VX_CONST_FLOAT(VX_CURTAIN_MIN_LINE, 0.02f)     // a line is at least this wide on each side (m)

// Distance (m) from x to the nearest multiple of spacing.
float VxGridDistance(float x, float spacing) {
    return abs(fract(x / spacing + 0.5f) - 0.5f) * spacing;
}

// Opacity of the curtain where the ray crosses a wall at distance t: `along` is the horizontal
// coordinate on the wall, y the height of the crossing, eyeY the camera's height.
float VxCurtainAt(float t, float along, float y, float eyeY, float spacing, float pixelAngle) {
    float fade = (1.0f - smoothstep(VX_CURTAIN_NEAR, VX_CURTAIN_FAR, t)) *
                 (1.0f - smoothstep(VX_CURTAIN_BAND_NEAR, VX_CURTAIN_BAND_FAR, abs(y - eyeY)));
    if (fade <= 0.0f) return 0.0f;
    float width = max(t * pixelAngle, VX_CURTAIN_MIN_LINE);
    float d = min(VxGridDistance(along, spacing), VxGridDistance(y, spacing));
    float line = 1.0f - smoothstep(width, 2.0f * width, d);
    return fade * (VX_CURTAIN_BASE + VX_CURTAIN_LINE * line);
}

// One wall, the plane where axis A equals `wall`; B is the other horizontal axis. Counts when the
// crossing lies in front of the camera and of the scene (0 < t < tScene) and B there is in [lo, hi].
float VxCurtainWall(float wall, float roA, float rdA, float roB, float rdB, float lo, float hi,
                    float roY, float rdY, float tScene, float spacing, float pixelAngle) {
    if (abs(rdA) < 1.0e-6f) return 0.0f;
    float t = (wall - roA) / rdA;
    if (!(t > 0.0f) || !(t < tScene)) return 0.0f;
    float along = roB + rdB * t;
    if (along < lo || along > hi) return 0.0f;
    return VxCurtainAt(t, along, roY + rdY * t, roY, spacing, pixelAngle);
}

// Opacity (0..1) of the curtain along the ray ro + rd * t (rd a unit vector) up to the scene at tScene.
// bounds = (min x, min z, max x, max z); none (0) when max <= min. Overlapping walls combine as
// layers: 1 - (1 - a0)(1 - a1)...
float VxPlayBoundsCurtain(vec3 ro, vec3 rd, float tScene, vec4 bounds, float spacing, float pixelAngle) {
    if (!(bounds.z > bounds.x) || !(bounds.w > bounds.y)) return 0.0f;
    // The camera inside the bounds (a session's player, always): the ray leaves through exactly one
    // wall, the nearer of the x wall and the z wall it heads for. The same for every pixel of the
    // frame, so no divergence; one VxCurtainAt instead of four wall tests.
    if (ro.x >= bounds.x && ro.x <= bounds.z && ro.z >= bounds.y && ro.z <= bounds.w) {
        float tx = abs(rd.x) < 1.0e-6f ? 1.0e30f : ((rd.x > 0.0f ? bounds.z : bounds.x) - ro.x) / rd.x;
        float tz = abs(rd.z) < 1.0e-6f ? 1.0e30f : ((rd.z > 0.0f ? bounds.w : bounds.y) - ro.z) / rd.z;
        float t = min(tx, tz);
        if (!(t < tScene) || !(t < VX_CURTAIN_FAR)) return 0.0f;
        float along = tx < tz ? ro.z + rd.z * t : ro.x + rd.x * t;
        return VxCurtainAt(t, along, ro.y + rd.y * t, ro.y, spacing, pixelAngle);
    }
    // Outside (an offline --play-bounds camera): every wall the ray crosses in front of the scene.
    float keep = 1.0f;
    keep *= 1.0f - VxCurtainWall(bounds.x, ro.x, rd.x, ro.z, rd.z, bounds.y, bounds.w, ro.y, rd.y, tScene, spacing, pixelAngle);
    keep *= 1.0f - VxCurtainWall(bounds.z, ro.x, rd.x, ro.z, rd.z, bounds.y, bounds.w, ro.y, rd.y, tScene, spacing, pixelAngle);
    keep *= 1.0f - VxCurtainWall(bounds.y, ro.z, rd.z, ro.x, rd.x, bounds.x, bounds.z, ro.y, rd.y, tScene, spacing, pixelAngle);
    keep *= 1.0f - VxCurtainWall(bounds.w, ro.z, rd.z, ro.x, rd.x, bounds.x, bounds.z, ro.y, rd.y, tScene, spacing, pixelAngle);
    return 1.0f - keep;
}

#endif // VX_SHARED_BOUNDS_GLSL
