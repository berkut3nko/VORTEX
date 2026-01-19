include(FetchContent)

# @brief Dependency Configuration
# @details Fetches and configures external libraries required by the engine.

# ==========================================
# 1. CORE & SYSTEM
# ==========================================

# --- Vulkan (System Package) ---
find_package(Vulkan REQUIRED)

# --- vk-bootstrap (Vulkan Setup Helper) ---
FetchContent_Declare(
    vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.3.6
)
FetchContent_MakeAvailable(vk-bootstrap)

# --- Vulkan Memory Allocator (VMA) ---
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG master
)
# VMA is header-only, but provides a CMake interface
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_AMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# --- GLFW (Windowing) ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG master
)
FetchContent_MakeAvailable(glfw)

# --- spdlog (Logging) ---
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
)
FetchContent_MakeAvailable(spdlog)


# ==========================================
# 2. MATH & PHYSICS
# ==========================================

# --- GLM (Mathematics) ---
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG master
)
FetchContent_MakeAvailable(glm)

# --- Jolt Physics ---
# Optimize Jolt build options for game dev
set(USE_SSE4_2 ON CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    jolt_physics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG master 
)
FetchContent_MakeAvailable(jolt_physics)


# ==========================================
# 3. ARCHITECTURE (ECS & SERIALIZATION)
# ==========================================

# --- EnTT (ECS) ---
FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.13.1
)
FetchContent_MakeAvailable(entt)

# --- Bitsery (Serialization) ---
FetchContent_Declare(
    bitsery
    GIT_REPOSITORY https://github.com/fraillt/bitsery.git
    GIT_TAG master
)
FetchContent_MakeAvailable(bitsery)

# --- Flatbuffers (Serialization) ---
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG v23.5.26
)
FetchContent_MakeAvailable(flatbuffers)


# ==========================================
# 4. ASSETS & GRAPHICS UTILS
# ==========================================

# --- stb (Image loading, etc) ---
# stb is a collection of headers, no standard CMake. We fetch it and make an interface lib.
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

if(NOT TARGET stb_lib)
    add_library(stb_lib INTERFACE)
    target_include_directories(stb_lib INTERFACE ${stb_SOURCE_DIR})
endif()

# --- TinyGLTF ---
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG release
)
FetchContent_MakeAvailable(tinygltf)

# --- glslang (Shader Compilation) ---
# Warning: This can take a while to build.
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE) # Disable spirv-opt to save build time if not needed
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG main
)
FetchContent_MakeAvailable(glslang)


# ==========================================
# 5. UI & TOOLS
# ==========================================

# --- Dear ImGui ---
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

if(NOT TARGET imgui_lib)
    add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    )
    target_include_directories(imgui_lib PUBLIC 
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui_lib PUBLIC glfw Vulkan::Vulkan)
endif()

# --- ImGuizmo ---
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG master
)
FetchContent_GetProperties(imguizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(imguizmo)
endif()

if(NOT TARGET imguizmo_lib)
    add_library(imguizmo_lib STATIC
        ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp
    )
    target_include_directories(imguizmo_lib PUBLIC ${imguizmo_SOURCE_DIR})
    # ImGuizmo depends on ImGui
    target_link_libraries(imguizmo_lib PUBLIC imgui_lib)
endif()


# ==========================================
# 6. NETWORKING & TESTING
# ==========================================

# --- GameNetworkingSockets ---
# Note: This is a heavy dependency. Ensure requirements (protobuf, openssl) are met in system or fetched.
# For simplicity in this script, we fetch it, but in production, you might want to use a precompiled version.
set(GNS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GNS_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    GameNetworkingSockets
    GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
    GIT_TAG master
)
# Note: Commenting out MakeAvailable for now as GNS often requires strict environment setup.
# Uncomment if you have Protobuf and OpenSSL installed.
# FetchContent_MakeAvailable(GameNetworkingSockets)


# --- GoogleTest ---
if(BUILD_VORTEX_TESTS)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
endif()

# --- Google Benchmark ---
if(BUILD_VORTEX_TESTS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    FetchContent_MakeAvailable(benchmark)
endif()