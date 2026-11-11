// Plain (non-module) translation unit compiling the Vulkan Memory Allocator implementation
// once. It cannot live in a module's global module fragment (two-phase lookup of its
// file-static helpers fails there).
#define VMA_IMPLEMENTATION
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
