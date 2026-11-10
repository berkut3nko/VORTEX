#version 460
#extension GL_GOOGLE_include_directive : require
// G-buffer, dynamic objects whose box contains the camera or straddles the near plane: back
// faces of the proxy box (cull FRONT). The surface is in front of the rasterised face, so no
// conservative depth qualifier can be given.
#define VX_NEED_TRACE
#include "include/common.glsl"
#include "include/gbuffer_object.glsl"
