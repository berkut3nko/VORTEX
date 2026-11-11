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
#  Windowing / logging / math
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

# Silence warnings coming from third-party sources we compile ourselves.
foreach(_t vortex_imgui vortex_imguizmo)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${_t} PRIVATE -w)
    endif()
endforeach()

set(CMAKE_CXX_SCAN_FOR_MODULES ${_VORTEX_SAVED_SCAN})
