// GLSL side of the shared-code shim (see vx_cpp.h for the rules).
#ifndef VX_GLSL_GLSL
#define VX_GLSL_GLSL

#define VX_CONST_INT(name, value)   const int name = value;
#define VX_CONST_UINT(name, value)  const uint name = value;
#define VX_CONST_FLOAT(name, value) const float name = value;
#define VX_OUT(T)   out T
#define VX_INOUT(T) inout T

#endif
