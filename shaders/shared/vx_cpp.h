// =============================================================================
//  C++ shim for the shared CPU/GPU code in shaders/shared/*.glsl
// =============================================================================
//  The files in this directory are written in the subset of GLSL that is also
//  valid C++ (with glm supplying the vector types and built-ins). The GPU
//  compiles them as GLSL; the engine compiles the very same text as member
//  functions of a C++ struct:
//
//      namespace mine {
//          using namespace glm;            // vec3, floor(), min() ...
//          using uint = unsigned int;
//          struct Kernel {
//              ... data the VX_* accessor macros read from ...
//              #include "shared/trace.glsl"
//          };
//      }
//
//  One implementation of the traversal therefore serves rendering, gameplay
//  ray casts, unit tests and the CPU reference renderer, and a bug fixed in
//  one place is fixed everywhere.
//
//  Rules for shared code (the C++ compiler enforces most of them):
//    * float literals carry an `f` suffix (1.0f) - legal GLSL, required in C++;
//    * no swizzles (.xyz); build vectors with constructors, e.g. vec3(v4);
//    * `out` / `inout` parameters are spelled VX_OUT(T) / VX_INOUT(T);
//    * file-scope constants are declared with VX_CONST_*;
//    * no global state: data is reached through the VX_* accessor macros.
// =============================================================================
#ifndef VX_CPP_H
#define VX_CPP_H

#include <glm/glm.hpp>

#define VX_CPP 1
#define VX_CONST_INT(name, value)   static constexpr int name = value;
#define VX_CONST_UINT(name, value)  static constexpr unsigned int name = value;
#define VX_CONST_FLOAT(name, value) static constexpr float name = value;
#define VX_OUT(T)   T&
#define VX_INOUT(T) T&

#endif // VX_CPP_H
