cmake_minimum_required(VERSION 3.28)

# =============================================================================
#  VORTEX voxel engine
# =============================================================================
#  Targets
#    Vortex::Core      core / math / voxel / fx    (no GPU, no window: usable by
#                                                    tools, tests, dedicated servers)
#    Vortex::Physics   Jolt integration
#    Vortex::Graphics  Vulkan renderer, window, UI
#    Vortex::Engine    scene (ECS), application loop, editor  <- games link this
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

# --- Graphics ------------------------------------------------------------------
vortex_add_module_library(VortexGraphics graphics)
target_link_libraries(VortexGraphics
    PUBLIC  VortexCore vortex_imgui
    PRIVATE Vulkan::Vulkan vk-bootstrap::vk-bootstrap vortex_vma glfw vortex_tinygltf
)
add_library(Vortex::Graphics ALIAS VortexGraphics)

# --- Engine (what games link) ----------------------------------------------------
vortex_add_module_library(VortexEngine scene engine editor)
target_link_libraries(VortexEngine
    PUBLIC  VortexCore VortexPhysics VortexGraphics EnTT::EnTT vortex_imgui
    PRIVATE vortex_imguizmo glfw
)
add_dependencies(VortexEngine VortexShaders)
add_library(Vortex::Engine ALIAS VortexEngine)

# --- Tools & tests ---------------------------------------------------------------
if(VORTEX_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(VORTEX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
