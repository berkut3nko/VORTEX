# =============================================================================
#  GLSL -> SPIR-V build step
# =============================================================================
#  Shaders are compiled at *build* time, so a broken shader is a build error
#  instead of a crash at start-up, and the engine does not have to link a GLSL
#  compiler. Sources live in engine/shaders:
#     *.vert / *.frag / *.comp   entry points (one SPIR-V module each)
#     shared/*.glsl              code shared with C++ (CPU reference tracer)
#     include/*.glsl             GPU-only includes
#
#  vortex_deploy_assets(<target>) copies the compiled shaders and the engine's
#  models next to the executable (assets/shaders, assets/models).
# =============================================================================

find_program(VORTEX_GLSL_COMPILER
    NAMES glslangValidator glslang
    HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
    DOC "GLSL to SPIR-V compiler (glslang)"
)

if(NOT VORTEX_GLSL_COMPILER)
    message(STATUS "VORTEX: glslangValidator not found, building glslang from source")
    include(FetchContent)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(_VORTEX_SAVED_SCAN ${CMAKE_CXX_SCAN_FOR_MODULES})
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG        vulkan-sdk-1.4.363.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(glslang)
    set(CMAKE_CXX_SCAN_FOR_MODULES ${_VORTEX_SAVED_SCAN})
    set(VORTEX_GLSL_COMPILER $<TARGET_FILE:glslang-standalone>)
    set(VORTEX_GLSL_COMPILER_DEP glslang-standalone)
else()
    message(STATUS "VORTEX: GLSL compiler: ${VORTEX_GLSL_COMPILER}")
    set(VORTEX_GLSL_COMPILER_DEP "")
endif()

set(VORTEX_SHADER_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../shaders")
cmake_path(NORMAL_PATH VORTEX_SHADER_SOURCE_DIR)
set(VORTEX_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/vortex_assets/shaders" CACHE INTERNAL "Compiled shader directory")
set(VORTEX_MODEL_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../models")
cmake_path(NORMAL_PATH VORTEX_MODEL_SOURCE_DIR)
set(VORTEX_MODEL_SOURCE_DIR "${VORTEX_MODEL_SOURCE_DIR}" CACHE INTERNAL "Engine model directory")

file(GLOB VORTEX_SHADER_ENTRY_POINTS CONFIGURE_DEPENDS
    "${VORTEX_SHADER_SOURCE_DIR}/*.vert"
    "${VORTEX_SHADER_SOURCE_DIR}/*.frag"
    "${VORTEX_SHADER_SOURCE_DIR}/*.comp"
)
file(GLOB_RECURSE VORTEX_SHADER_INCLUDES CONFIGURE_DEPENDS
    "${VORTEX_SHADER_SOURCE_DIR}/shared/*.glsl"
    "${VORTEX_SHADER_SOURCE_DIR}/include/*.glsl"
)

set(VORTEX_SPIRV_FILES "")
foreach(_src ${VORTEX_SHADER_ENTRY_POINTS})
    cmake_path(GET _src FILENAME _name)
    set(_out "${VORTEX_SHADER_OUTPUT_DIR}/${_name}.spv")
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VORTEX_SHADER_OUTPUT_DIR}"
        COMMAND ${VORTEX_GLSL_COMPILER} -V --target-env vulkan1.3 --quiet
                -I"${VORTEX_SHADER_SOURCE_DIR}"
                -o "${_out}" "${_src}"
        DEPENDS "${_src}" ${VORTEX_SHADER_INCLUDES} ${VORTEX_GLSL_COMPILER_DEP}
        COMMENT "GLSL ${_name}"
        VERBATIM
    )
    list(APPEND VORTEX_SPIRV_FILES "${_out}")
endforeach()

add_custom_target(VortexShaders ALL DEPENDS ${VORTEX_SPIRV_FILES})

# Copies engine assets next to an executable. Runs on every build (cheap,
# copy-if-different) so recompiled shaders are picked up without relinking.
function(vortex_deploy_assets target)
    get_target_property(_dir ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT _dir)
        get_target_property(_dir ${target} BINARY_DIR)
    endif()
    get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_multi)
        set(_dir "${_dir}/$<CONFIG>")
    endif()
    add_custom_target(${target}_assets
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}/assets/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                "${VORTEX_SHADER_OUTPUT_DIR}" "${_dir}/assets/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                "${VORTEX_MODEL_SOURCE_DIR}" "${_dir}/assets/models"
        DEPENDS VortexShaders
        COMMENT "Deploying VORTEX assets for ${target}"
        VERBATIM
    )
    add_dependencies(${target} ${target}_assets)
endfunction()
