// =============================================================================
//  Proxy cube - shared by the GPU shaders and C++ (see shared/vx_cpp.h).
// =============================================================================
#ifndef VX_SHARED_CUBE_GLSL
#define VX_SHARED_CUBE_GLSL

// Corner (each component 0 or 1) of vertex `vertexIndex` (0..35) of a unit cube drawn as 12
// triangles. In right-handed coordinates every triangle is counter-clockwise when seen from
// outside the cube, i.e. its geometric normal points outwards.
// Faces: 0 = -X, 1 = +X, 2 = -Y, 3 = +Y, 4 = -Z, 5 = +Z (six vertices each).
vec3 VxCubeCorner(int vertexIndex) {
    int face = vertexIndex / 6;
    int k = vertexIndex - face * 6;
    // Two triangles over the unit square: (0,0) (1,0) (1,1) and (0,0) (1,1) (0,1).
    float a = (k == 1 || k == 2 || k == 4) ? 1.0f : 0.0f;
    float b = (k == 2 || k == 4 || k == 5) ? 1.0f : 0.0f;
    int axis = face / 2;
    bool positive = (face - axis * 2) == 1;
    // On a +axis face (u, v) follow the next two axes in cyclic order, so u x v = +axis.
    // Swapping them on the -axis face reverses the winding and with it the normal.
    float u = positive ? a : b;
    float v = positive ? b : a;
    float w = positive ? 1.0f : 0.0f;
    if (axis == 0) return vec3(w, u, v);
    if (axis == 1) return vec3(v, w, u);
    return vec3(u, v, w);
}

// Outward unit normal of the face a cube vertex belongs to.
vec3 VxCubeFaceNormal(int vertexIndex) {
    int face = vertexIndex / 6;
    int axis = face / 2;
    float s = ((face - axis * 2) == 1) ? 1.0f : -1.0f;
    if (axis == 0) return vec3(s, 0.0f, 0.0f);
    if (axis == 1) return vec3(0.0f, s, 0.0f);
    return vec3(0.0f, 0.0f, s);
}

#endif // VX_SHARED_CUBE_GLSL
