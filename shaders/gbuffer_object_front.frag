#version 460
#extension GL_GOOGLE_include_directive : require
// G-buffer, dynamic objects seen from outside: front faces of the proxy box (cull BACK).
// The surface is always at or behind the rasterised face, which is what the conservative
// depth qualifier in the shared source promises, so early-Z can reject hidden boxes.
#define VX_NEED_TRACE
#define VX_OBJECT_FRONT
#include "include/common.glsl"
#include "include/gbuffer_object.glsl"
