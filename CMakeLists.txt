cmake_minimum_required(VERSION 3.28)

# =============================================================================
#  VORTEX voxel engine
# =============================================================================
#  Targets
#    Vortex::Core      core / math / voxel / fx    (no GPU, no window: usable by
#                                                    tools, tests, dedicated servers)
#    Vortex::Physics   Jolt integration
#    Vortex::Sim       scene (ECS) + the simulation: world, physics, entities on a
#                      numbered fixed tick (no GPU, no window: what a session server runs)
#    Vortex::Graphics  Vulkan renderer, window, UI
#    Vortex::Engine    application loop, editor                <- games link this
#    VortexShaders     GLSL -> SPIR-V
# =============================================================================
project(VORTEX VERSION 0.2.0 LANGUAGES CXX C)

option(VORTEX_BUILD_TESTS "Build the engine unit tests" ${PROJECT_IS_TOP_LEVEL})
option(VORTEX_BUILD_TOOLS "Build engine tools (CPU reference renderer)" ON)
option(VORTEX_WARNINGS_AS_ERRORS "Treat engine warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "VORTEX: no build type selected, defaulting to RelWithDebInfo")
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "VORTEX is built and tested with Clang >= 18 (C++20 named modules). "
                    "${CMAKE_CXX_COMPILER_ID} is untested.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(VortexShaders)

# -----------------------------------------------------------------------------
#  Common options shared by every engine target and everything that imports
#  engine modules. Clang requires module interfaces (BMIs) and their importers
#  to agree on target features, so the CPU flags Jolt publishes (-mavx2 ...)
#  are applied uniformly instead of leaking into only some translation units.
# -----------------------------------------------------------------------------
add_library(vortex_options INTERFACE)
target_compile_features(vortex_options INTERFACE cxx_std_20)
get_target_property(_VORTEX_JOLT_OPTIONS Jolt INTERFACE_COMPILE_OPTIONS)
if(_VORTEX_JOLT_OPTIONS)
    target_compile_options(vortex_options INTERFACE ${_VORTEX_JOLT_OPTIONS})
endif()
# Determinism floor (docs/design/networking.md, stage 0): Jolt's flags include -mfma, and Clang
# by default contracts a*b+c into one fused operation where it sees fit, which rounds differently
# from the two separate operations. With contraction off every engine TU computes the same floats
# on every build, so generated terrain is bit-identical between server and clients. Jolt itself
# is not touched (it does not link vortex_options).
# clang-cl reports the compiler id "Clang" too but silently ignores a bare -ffp-contract=off
# (only a warning), so it gets the flag through /clang: and an unknown argument is made an error.
# Tests Noise.GoldenFloatBits and Terrain.GoldenHeightBits fail on a build that contracts.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(vortex_options INTERFACE /clang:-ffp-contract=off -Werror=unknown-argument)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(vortex_options INTERFACE -ffp-contract=off -Werror=unknown-argument)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(vortex_options INTERFACE -ffp-contract=off)
else()
    message(WARNING "VORTEX: no floating-point contraction flag is known for ${CMAKE_CXX_COMPILER_ID}. "
                    "Generated terrain may differ from other builds; do not use this build as a "
                    "network server or client unless the golden tests in VortexCoreTests pass.")
endif()
target_compile_definitions(vortex_options INTERFACE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    $<$<CONFIG:Debug>:VORTEX_DEBUG>
)
target_link_libraries(vortex_options INTERFACE glm::glm)

add_library(vortex_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vortex_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wno-missing-field-initializers      # Vulkan create-info structs
        -Wno-gnu-zero-variadic-macro-arguments
        -Wno-nullability-extension -Wno-nullability-completeness   # VMA
    )
    if(VORTEX_WARNINGS_AS_ERRORS)
        target_compile_options(vortex_warnings INTERFACE -Werror)
    endif()
endif()

# Helper: a static library made of module interfaces (*.cppm) and
# implementation units (*.cpp) found below the given source directories.
function(vortex_add_module_library name)
    set(_ifaces "")
    set(_impls "")
    foreach(_dir ${ARGN})
        file(GLOB_RECURSE _i CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_dir}/*.cppm")
        file(GLOB_RECURSE _s CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_dir}/*.cpp")
        list(APPEND _ifaces ${_i})
        list(APPEND _impls ${_s})
    endforeach()
    add_library(${name} STATIC)
    target_sources(${name}
        PRIVATE ${_impls}
        PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${_ifaces}
    )
    target_link_libraries(${name} PUBLIC vortex_options PRIVATE vortex_warnings)
endfunction()

# --- Core: no GPU, no window ---------------------------------------------------
vortex_add_module_library(VortexCore core math voxel fx)
target_include_directories(VortexCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/shaders")   # shared CPU/GPU code
target_link_libraries(VortexCore PRIVATE spdlog::spdlog vortex_tinygltf)
add_library(Vortex::Core ALIAS VortexCore)

# --- Physics -------------------------------------------------------------------
vortex_add_module_library(VortexPhysics physics)
target_link_libraries(VortexPhysics PUBLIC VortexCore PRIVATE Jolt)
add_library(Vortex::Physics ALIAS VortexPhysics)

# --- Simulation: scene + sim, still no GPU and no window --------------------------
vortex_add_module_library(VortexSim scene sim)
target_link_libraries(VortexSim PUBLIC VortexCore VortexPhysics EnTT::EnTT)
add_library(Vortex::Sim ALIAS VortexSim)

# --- Graphics ------------------------------------------------------------------
vortex_add_module_library(VortexGraphics graphics)
target_link_libraries(VortexGraphics
    PUBLIC  VortexCore vortex_imgui
    PRIVATE Vulkan::Vulkan vk-bootstrap::vk-bootstrap vortex_vma glfw vortex_tinygltf
)
add_library(Vortex::Graphics ALIAS VortexGraphics)

# --- Engine (what games link) ----------------------------------------------------
vortex_add_module_library(VortexEngine engine editor)
target_link_libraries(VortexEngine
    PUBLIC  VortexCore VortexPhysics VortexSim VortexGraphics EnTT::EnTT vortex_imgui
    PRIVATE vortex_imguizmo glfw
)
add_dependencies(VortexEngine VortexShaders)
add_library(Vortex::Engine ALIAS VortexEngine)

# -----------------------------------------------------------------------------
#  The GPU-free libraries must stay GPU-free: a session server and its tests are
#  built from them (docs/design/networking.md). This walks everything a target
#  links, directly or through its dependencies, and stops the configure step if
#  a renderer, window or UI library has crept in.
# -----------------------------------------------------------------------------
function(vortex_collect_link_closure target out_var)
    set(_seen "")
    set(_queue ${target})
    while(_queue)
        list(POP_FRONT _queue _item)
        # $<LINK_ONLY:x> is how a static library records its PRIVATE dependencies.
        string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _item "${_item}")
        if(_item IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_item}")
        if(NOT TARGET "${_item}")
            continue()
        endif()
        get_target_property(_aliased "${_item}" ALIASED_TARGET)
        if(_aliased)
            list(APPEND _queue "${_aliased}")
            continue()
        endif()
        get_target_property(_type "${_item}" TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_direct "${_item}" LINK_LIBRARIES)
            if(_direct)
                list(APPEND _queue ${_direct})
            endif()
        endif()
        get_target_property(_interface "${_item}" INTERFACE_LINK_LIBRARIES)
        if(_interface)
            list(APPEND _queue ${_interface})
        endif()
    endwhile()
    set(${out_var} "${_seen}" PARENT_SCOPE)
endfunction()

function(vortex_assert_gpu_free target)
    vortex_collect_link_closure(${target} _closure)
    foreach(_item ${_closure})
        if(_item MATCHES "^(VortexGraphics|VortexEngine|Vortex::Graphics|Vortex::Engine|Vulkan.*|vortex_vulkan|glfw.*|vk-bootstrap.*|vortex_vma|vortex_imgui|vortex_imguizmo)$"
           OR _item MATCHES "lib(vulkan|glfw)")
            message(FATAL_ERROR "VORTEX: ${target} must not link '${_item}': it has to build and run "
                                "without a GPU or a window (docs/design/networking.md, stage 1).")
        endif()
    endforeach()
endfunction()

vortex_assert_gpu_free(VortexCore)
vortex_assert_gpu_free(VortexPhysics)
vortex_assert_gpu_free(VortexSim)

# --- Tools & tests ---------------------------------------------------------------
if(VORTEX_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(VORTEX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
