include(FetchContent)

# =============================================================================
#  VORTEX third-party dependencies
# =============================================================================
#  Rules:
#   * Every dependency is pinned to a release tag or an exact commit. Never track
#     a moving branch (master/main): a build that worked yesterday must work
#     tomorrow.
#   * Only what the engine actually uses is fetched.
#   * Dependency targets are built quietly (no warnings-as-errors, no tests,
#     no examples, no install rules) and never scanned for C++ modules.
# =============================================================================

set(FETCHCONTENT_QUIET ON)

# Dependencies must not inherit the engine's module scanning or warning flags.
set(_VORTEX_SAVED_SCAN ${CMAKE_CXX_SCAN_FOR_MODULES})
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Vulkan, vk-bootstrap, VMA and GLFW, ImGui and ImGuizmo: only with VORTEX_GRAPHICS (engine/CMakeLists.txt).
# Without it no Vulkan loader is needed and none of them is fetched.
if(VORTEX_GRAPHICS)
# -----------------------------------------------------------------------------
#  Vulkan (headers are fetched so the build does not depend on an installed
#  SDK; only the system loader `libvulkan` is required at link time)
# -----------------------------------------------------------------------------
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.4.363
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(VulkanHeaders)   # provides Vulkan::Headers

find_library(VORTEX_VULKAN_LOADER
    NAMES vulkan vulkan-1 libvulkan.so.1
    HINTS "$ENV{VULKAN_SDK}/lib" "$ENV{VULKAN_SDK}/Lib"
    DOC "Vulkan loader library"
)
if(NOT VORTEX_VULKAN_LOADER)
    message(FATAL_ERROR
        "VORTEX: Vulkan loader (libvulkan) not found.\n"
        "  Arch:   sudo pacman -S vulkan-icd-loader <vulkan-intel|vulkan-radeon|nvidia-utils>\n"
        "  Ubuntu: sudo apt install libvulkan-dev\n"
        "  Other:  install the LunarG Vulkan SDK and set VULKAN_SDK.")
endif()
message(STATUS "VORTEX: Vulkan loader: ${VORTEX_VULKAN_LOADER}")

add_library(vortex_vulkan INTERFACE)
target_link_libraries(vortex_vulkan INTERFACE Vulkan::Headers ${VORTEX_VULKAN_LOADER})
if(NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan ALIAS vortex_vulkan)
endif()

# vk-bootstrap (instance / device / swapchain boilerplate)
set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_INSTALL OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG        v1.4.363
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(vk-bootstrap)

# Vulkan Memory Allocator (header only)
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.2.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _vortex_headers_only_   # skip its CMake (it wants find_package(Vulkan))
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
add_library(vortex_vma INTERFACE)
target_include_directories(vortex_vma SYSTEM INTERFACE ${vulkanmemoryallocator_SOURCE_DIR}/include)
target_link_libraries(vortex_vma INTERFACE Vulkan::Headers)

# -----------------------------------------------------------------------------
#  Windowing (VORTEX_GRAPHICS)
# -----------------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw)
endif()   # VORTEX_GRAPHICS

# -----------------------------------------------------------------------------
#  Logging / math
# -----------------------------------------------------------------------------
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# -----------------------------------------------------------------------------
#  Physics (Jolt keeps its CMakeLists.txt in Build/)
# -----------------------------------------------------------------------------
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)                 # no -Werror in a dependency
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)                  # keep our CMAKE_CXX_FLAGS_*
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)              # CMAKE_BUILD_TYPE decides
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)                     # we derive from Jolt interfaces
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)                        # GPU compute backends are unused
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
# Jolt picks its instruction-set flags (-mavx2 ...) by CMAKE_SYSTEM_PROCESSOR. On Windows CMake reads it from
# the environment once, at the first configure, and keeps it in CMakeSystem.cmake: a first configure from a
# shell without PROCESSOR_ARCHITECTURE left it empty for good, and the build lost every -m flag. Take the
# compiler's target instead (OS_PLATFORM reads the same environment variable).
if(NOT CMAKE_SYSTEM_PROCESSOR AND NOT CMAKE_CROSSCOMPILING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine OUTPUT_VARIABLE _vortex_target
                    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _vortex_target_result)
    if(_vortex_target_result EQUAL 0 AND _vortex_target MATCHES "^([^-]+)-")
        set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_MATCH_1}")
        message(STATUS "VORTEX: CMAKE_SYSTEM_PROCESSOR was empty; the compiler targets '${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
endif()
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.6.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
)
FetchContent_MakeAvailable(JoltPhysics)

# -----------------------------------------------------------------------------
#  ECS (header only)
# -----------------------------------------------------------------------------
FetchContent_Declare(EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.15.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(EnTT)

# -----------------------------------------------------------------------------
#  Network transport: ENet (reliable / unreliable channels over UDP; C, built from
#  source). docs/design/networking.md stage 5. Linked PRIVATE into VortexNet only:
#  Core, Physics and Sim must never see it (checked below the targets in
#  CMakeLists.txt, vortex_assert_no_network).
# -----------------------------------------------------------------------------
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _vortex_headers_only_   # its CMake sets global include dirs; the library is built below
)
FetchContent_MakeAvailable(enet)
add_library(vortex_enet STATIC
    ${enet_SOURCE_DIR}/callbacks.c
    ${enet_SOURCE_DIR}/compress.c
    ${enet_SOURCE_DIR}/host.c
    ${enet_SOURCE_DIR}/list.c
    ${enet_SOURCE_DIR}/packet.c
    ${enet_SOURCE_DIR}/peer.c
    ${enet_SOURCE_DIR}/protocol.c
    ${enet_SOURCE_DIR}/unix.c
    ${enet_SOURCE_DIR}/win32.c
)
target_include_directories(vortex_enet SYSTEM PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(vortex_enet PUBLIC ws2_32 winmm)
else()
    # What ENet's own configure step detects; every one of them exists on Linux (glibc) and macOS.
    target_compile_definitions(vortex_enet PRIVATE
        HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1 HAS_GETHOSTBYNAME_R=1 HAS_GETHOSTBYADDR_R=1
        HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
endif()
set_target_properties(vortex_enet PROPERTIES POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
#  Asset import: tinygltf (bundles stb_image / stb_image_write / json)
# -----------------------------------------------------------------------------
FetchContent_Declare(tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.9.7
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _vortex_headers_only_
)
FetchContent_MakeAvailable(tinygltf)
add_library(vortex_tinygltf INTERFACE)
target_include_directories(vortex_tinygltf SYSTEM INTERFACE ${tinygltf_SOURCE_DIR})

# -----------------------------------------------------------------------------
#  UI: Dear ImGui + ImGuizmo (no upstream CMake)
# -----------------------------------------------------------------------------
if(VORTEX_GRAPHICS)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9b
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _vortex_headers_only_
)
FetchContent_MakeAvailable(imgui)

add_library(vortex_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(vortex_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(vortex_imgui PUBLIC glfw Vulkan::Vulkan)

FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        105e4fde45e4e74aae8973d98750820ef7aca7e2   # master @ 2026-11
    SOURCE_SUBDIR  _vortex_headers_only_
)
FetchContent_MakeAvailable(imguizmo)
add_library(vortex_imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(vortex_imguizmo SYSTEM PUBLIC ${imguizmo_SOURCE_DIR}/src)
target_link_libraries(vortex_imguizmo PUBLIC vortex_imgui)
endif()   # VORTEX_GRAPHICS

# -----------------------------------------------------------------------------
#  Audio: miniaudio (single header, C) with stb_vorbis from its extras (VORTEX_GRAPHICS:
#  only the engine plays sound; docs/design/audio.md). Built below as vortex_miniaudio from
#  one plain C file (src/audio/internal/MiniaudioImpl.c) and linked PRIVATE into VortexAudio
#  only: Core, Physics, Sim, Net, the server and the bot never see it (vortex_assert_no_audio).
# -----------------------------------------------------------------------------
if(VORTEX_GRAPHICS)
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.25
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _vortex_headers_only_   # its CMake builds examples and tests; the library is built in CMakeLists.txt
)
FetchContent_MakeAvailable(miniaudio)
endif()   # VORTEX_GRAPHICS

# -----------------------------------------------------------------------------
#  Tests
# -----------------------------------------------------------------------------
if(VORTEX_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

# Silence warnings coming from third-party sources we compile ourselves (googletest's own CMake adds
# -Wall -Wconversion ... and clang warns in gtest-printers.h: a fresh build would not be warning-free).
foreach(_t vortex_imgui vortex_imguizmo vortex_enet gtest gtest_main gmock gmock_main)
    if(TARGET ${_t} AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${_t} PRIVATE -w)
    endif()
endforeach()

set(CMAKE_CXX_SCAN_FOR_MODULES ${_VORTEX_SAVED_SCAN})
