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
#    Vortex::Net       the session protocol: transports, messages, server, client
#                      (no GPU, no window: vortex_server links only this)
#    Vortex::Graphics  Vulkan renderer, window, UI              (VORTEX_GRAPHICS)
#    Vortex::Audio     sound: manifest, mixer, miniaudio output  (VORTEX_GRAPHICS; no GPU)
#    Vortex::Engine    application loop, editor                <- games link this (VORTEX_GRAPHICS)
#    VortexShaders     GLSL -> SPIR-V                           (VORTEX_GRAPHICS)
#    vortex_cpu_check  start-up CPU check, linked into every executable
# =============================================================================
project(VORTEX VERSION 0.2.0 LANGUAGES CXX C)

# An optimised build type whose flags carry no -O level builds at -O0 without a word (a build
# directory whose first configure failed early cached empty CMAKE_CXX_FLAGS_<CONFIG>). Every
# CPU timing from such a build is wrong, so refuse it.
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _vortex_config)
    if(NOT "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_vortex_config}}" MATCHES "(^| )[-/]O[1-3sz]")
        message(FATAL_ERROR "VORTEX: build type ${CMAKE_BUILD_TYPE} has no optimisation flag "
            "(CMAKE_CXX_FLAGS_${_vortex_config} = '${CMAKE_CXX_FLAGS_${_vortex_config}}'). "
            "Reset it: cmake -U 'CMAKE_*_FLAGS_*' --preset <preset>, or delete the build directory.")
    endif()
endif()

option(VORTEX_BUILD_TESTS "Build the engine unit tests" ${PROJECT_IS_TOP_LEVEL})
option(VORTEX_BUILD_TOOLS "Build the engine tools (vortex_server, vortex_bot)" ON)
option(VORTEX_WARNINGS_AS_ERRORS "Treat engine warnings as errors" OFF)
# OFF builds only what needs no GPU and no window: Core, Physics, Sim, Net, vortex_server, vortex_bot and
# their tests (a server host, a CI job without a Vulkan loader). Read by Dependencies.cmake: keep it here.
option(VORTEX_GRAPHICS "Build the renderer, window, UI, engine and editor (Vulkan, GLFW, ImGui)" ON)

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
if(VORTEX_GRAPHICS)
    include(VortexShaders)
else()
    message(STATUS "VORTEX: VORTEX_GRAPHICS OFF: Core, Physics, Sim, Net, vortex_server, vortex_bot and their tests only")
endif()

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
# The start-up CPU check and BuildInfo assume AVX2: an x86 build without Jolt's -mavx2 is a configure mistake
# (an empty CMAKE_SYSTEM_PROCESSOR, see cmake/Dependencies.cmake), never a build to run.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT MSVC AND NOT "-mavx2" IN_LIST _VORTEX_JOLT_OPTIONS)
    message(FATAL_ERROR "VORTEX: Jolt publishes no -mavx2 for '${CMAKE_SYSTEM_PROCESSOR}' (options: '${_VORTEX_JOLT_OPTIONS}')")
endif()
# Determinism floor (docs/design/networking.md, stage 0): Jolt's flags include -mfma, and Clang
# by default contracts a*b+c into one fused operation where it sees fit, which rounds differently
# from the two separate operations. With contraction off every engine TU computes the same floats
# on every build, so generated terrain is bit-identical between server and clients. Jolt itself
# is not touched (it does not link vortex_options).
# clang-cl reports the compiler id "Clang" too but silently ignores a bare -ffp-contract=off
# (only a warning), so it gets the flag through /clang: and an unknown argument is made an error.
# Tests Terrain.GoldenNoiseBits and Terrain.GoldenHeightBits (tests/core/HashTests.cpp) fail on a build that contracts.
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

# The start-up CPU check (src/startup/CpuCheck.c, plain C, built without vortex_options' -mavx2 ...):
# every executable links it, and refuses to start on a CPU without the instructions the build
# assumes (exit code 4 with a message) instead of crashing on the first one. An OBJECT library, so
# the linker keeps its constructor.
add_library(vortex_cpu_check OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/src/startup/CpuCheck.c")
target_include_directories(vortex_cpu_check PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/startup")
set_target_properties(vortex_cpu_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

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
target_link_libraries(vortex_cpu_check PRIVATE vortex_warnings)

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
include(BuildInfo)   # vortex::GetBuildInfo: the commits, build type, compiler and CPU flags

# --- Physics -------------------------------------------------------------------
vortex_add_module_library(VortexPhysics physics)
target_link_libraries(VortexPhysics PUBLIC VortexCore PRIVATE Jolt)
add_library(Vortex::Physics ALIAS VortexPhysics)

# --- Simulation: scene + sim, still no GPU and no window --------------------------
vortex_add_module_library(VortexSim scene sim)
target_link_libraries(VortexSim PUBLIC VortexCore VortexPhysics EnTT::EnTT)
add_library(Vortex::Sim ALIAS VortexSim)

# --- Net: the session protocol (docs/design/networking.md), still no GPU and no window --
vortex_add_module_library(VortexNet net)
# ENet (stage 5) is private: only VortexNet's implementation includes it; what links VortexNet (the
# server, the game, VortexNetTests) gets the static library, Core / Physics / Sim never do.
target_link_libraries(VortexNet PUBLIC VortexSim PRIVATE vortex_enet)
if(WIN32)
    # rendezvous.md [nat-R0]: lan candidates from GetAdaptersAddresses (src/net/internal/Address.cpp).
    target_link_libraries(VortexNet PRIVATE iphlpapi ws2_32)
endif()
add_library(Vortex::Net ALIAS VortexNet)

# --- Graphics (VORTEX_GRAPHICS) -------------------------------------------------
if(VORTEX_GRAPHICS)
vortex_add_module_library(VortexGraphics graphics)
target_link_libraries(VortexGraphics
    PUBLIC  VortexCore vortex_imgui
    PRIVATE Vulkan::Vulkan vk-bootstrap::vk-bootstrap vortex_vma glfw vortex_tinygltf
)
add_library(Vortex::Graphics ALIAS VortexGraphics)

# --- Audio (docs/design/audio.md): built with the graphics because only the engine plays sound,
# but it needs no GPU and no window, and its tests need no audio device --------------------------
# miniaudio and stb_vorbis: one plain C translation unit, warnings off (third-party code).
add_library(vortex_miniaudio STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/internal/MiniaudioImpl.c")
target_include_directories(vortex_miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
# The same configuration for the implementation and every file that includes miniaudio.h: Ogg Vorbis (stb_vorbis)
# and WAV only.
target_compile_definitions(vortex_miniaudio PUBLIC MA_NO_MP3 MA_NO_FLAC MA_NO_ENCODING)
set_target_properties(vortex_miniaudio PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vortex_miniaudio PRIVATE -w)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(vortex_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

vortex_add_module_library(VortexAudio audio)
target_link_libraries(VortexAudio PUBLIC VortexCore PRIVATE vortex_miniaudio)
add_library(Vortex::Audio ALIAS VortexAudio)

# --- Engine (what games link) ----------------------------------------------------
vortex_add_module_library(VortexEngine engine editor)
target_link_libraries(VortexEngine
    PUBLIC  VortexCore VortexPhysics VortexSim VortexNet VortexGraphics VortexAudio EnTT::EnTT vortex_imgui
    PRIVATE vortex_imguizmo glfw
)
add_dependencies(VortexEngine VortexShaders)
add_library(Vortex::Engine ALIAS VortexEngine)
endif()

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
vortex_assert_gpu_free(VortexNet)
vortex_assert_gpu_free(vortex_enet)

# The network transport (ENet, networking.md stage 5) belongs to VortexNet and what links it (the
# session server, the game, VortexNetTests): the simulation below must stay free of sockets.
function(vortex_assert_no_network target)
    vortex_collect_link_closure(${target} _closure)
    foreach(_item ${_closure})
        if(_item MATCHES "^(VortexNet|Vortex::Net|vortex_enet|enet)$")
            message(FATAL_ERROR "VORTEX: ${target} must not link '${_item}': the network transport belongs to "
                                "VortexNet only (docs/design/networking.md, stage 5).")
        endif()
    endforeach()
endfunction()

vortex_assert_no_network(VortexCore)
vortex_assert_no_network(VortexPhysics)
vortex_assert_no_network(VortexSim)

# rendezvous.md: HTTPS (libcurl) belongs to vortex_https, linked only by vortex_server and the game.
function(vortex_assert_no_https target)
    vortex_collect_link_closure(${target} _closure)
    foreach(_item ${_closure})
        if(_item MATCHES "^(vortex_https|CURL::libcurl|curl|libcurl)$" OR _item MATCHES "libcurl")
            message(FATAL_ERROR "VORTEX: ${target} must not link '${_item}': HTTPS belongs to vortex_https only "
                                "(docs/design/rendezvous.md \"The engine side\").")
        endif()
    endforeach()
endfunction()
foreach(_t VortexCore VortexPhysics VortexSim VortexNet)
    vortex_assert_no_https(${_t})
endforeach()

# audio.md: sound belongs to VortexAudio and the engine (presentation). The simulation, the session protocol, the
# server and the bot must stay free of it (a session server has no device; sim must not depend on what is heard).
function(vortex_assert_no_audio target)
    if(NOT TARGET ${target})
        return()
    endif()
    vortex_collect_link_closure(${target} _closure)
    foreach(_item ${_closure})
        if(_item MATCHES "^(VortexAudio|Vortex::Audio|vortex_miniaudio)$")
            message(FATAL_ERROR "VORTEX: ${target} must not link '${_item}': sound is presentation, it belongs to "
                                "VortexAudio and the engine only (docs/design/audio.md \"Layering\").")
        endif()
    endforeach()
endfunction()
foreach(_t VortexCore VortexPhysics VortexSim VortexNet)
    vortex_assert_no_audio(${_t})
endforeach()

# spread.md "Determinism traps": the fire rule and its driver decide from voxel ids, the step index and positions only.
# A source of chance in them would split peers once fire runs in a session; the configure step refuses one.
foreach(_file src/voxel/internal/Spread.cpp src/sim/internal/Spread.cpp)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${_file}" _hits REGEX "Hash3|rand\\(|random|mt19937")
    if(_hits)
        message(FATAL_ERROR "VORTEX: ${_file} must not use Hash3, rand(, random or mt19937 (spread.md \"Determinism traps\"): ${_hits}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
endforeach()

# --- Tools & tests ---------------------------------------------------------------
if(VORTEX_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(VORTEX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
