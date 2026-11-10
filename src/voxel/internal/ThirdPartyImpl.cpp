// Plain (non-module) translation unit that compiles the tinygltf / stb_image / stb_image_write
// implementations exactly once for the whole engine. They cannot live in a module's global
// module fragment: tinygltf's templates rely on ordinary two-phase lookup of file-static helpers.
// The renderer's screenshot path links against stbi_write_png defined here.
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"
