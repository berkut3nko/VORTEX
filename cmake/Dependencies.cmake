include(FetchContent)

# @brief Dependency Configuration
# @details Fetches and configures external libraries required for the VORTEX engine.

# ==========================================
# 1. CORE & SYSTEM
# ==========================================

find_package(Vulkan REQUIRED)

# vk-bootstrap
FetchContent_Declare(
    vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.3.240
)
FetchContent_MakeAvailable(vk-bootstrap)

# Vulkan Memory Allocator
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG master
)
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# GLFW
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

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.0
)
FetchContent_MakeAvailable(spdlog)


# ==========================================
# 2. TESTING & BENCHMARKING
# ==========================================
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

if(BUILD_VORTEX_TESTS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
    )
    FetchContent_MakeAvailable(benchmark)
endif()


# ==========================================
# 3. MATH & PHYSICS
# ==========================================

# GLM
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG master
)
FetchContent_MakeAvailable(glm)

if(NOT TARGET glm::glm)
    if(TARGET glm)
        add_library(glm::glm ALIAS glm)
    endif()
endif()

# Jolt Physics
# @note Jolt Physics places its CMakeLists.txt in the 'Build' subdirectory, not the root.
set(USE_SSE4_2 ON CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE) 
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    jolt_physics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.0.0 
)

FetchContent_GetProperties(jolt_physics)
if(NOT jolt_physics_POPULATED)
    FetchContent_Populate(jolt_physics)
    # CRITICAL FIX: The CMakeLists.txt is in the 'Build' folder!
    add_subdirectory(${jolt_physics_SOURCE_DIR}/Build ${jolt_physics_BINARY_DIR})
endif()

# Robust target check and alias creation
if(TARGET Jolt)
    if(NOT TARGET JoltPhysics)
        add_library(JoltPhysics ALIAS Jolt)
    endif()
    message(STATUS "VORTEX: Jolt Physics target 'Jolt' successfully aliased.")
elseif(TARGET jolt)
    if(NOT TARGET JoltPhysics)
        add_library(JoltPhysics ALIAS jolt)
    endif()
    message(STATUS "VORTEX: Jolt Physics target 'jolt' successfully aliased.")
else()
    message(FATAL_ERROR "VORTEX: Jolt Physics target not found! Checked 'Jolt' and 'jolt'.")
endif()


# ==========================================
# 4. ARCHITECTURE (ECS & SERIALIZATION)
# ==========================================

# EnTT
FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.14.0
)
FetchContent_MakeAvailable(entt)

# Bitsery
FetchContent_Declare(
    bitsery
    GIT_REPOSITORY https://github.com/fraillt/bitsery.git
    GIT_TAG master
)
FetchContent_MakeAvailable(bitsery)

if(NOT TARGET bitsery::bitsery)
    if(TARGET bitsery)
        add_library(bitsery::bitsery ALIAS bitsery)
    else()
        add_library(bitsery INTERFACE)
        target_include_directories(bitsery INTERFACE ${bitsery_SOURCE_DIR}/include)
        add_library(bitsery::bitsery ALIAS bitsery)
    endif()
endif()

# Flatbuffers
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG v24.3.25
)
FetchContent_MakeAvailable(flatbuffers)

if(NOT TARGET flatbuffers::flatbuffers)
    if(TARGET flatbuffers)
        add_library(flatbuffers::flatbuffers ALIAS flatbuffers)
    endif()
endif()


# ==========================================
# 5. ASSETS & GRAPHICS UTILS
# ==========================================

# stb (Headers Only - no CMakeLists.txt)
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

# TinyGLTF
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG release
)
FetchContent_MakeAvailable(tinygltf)

if(NOT TARGET tinygltf_lib)
    add_library(tinygltf_lib INTERFACE)
    target_include_directories(tinygltf_lib INTERFACE ${tinygltf_SOURCE_DIR})
endif()

# glslang
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG main
)
FetchContent_MakeAvailable(glslang)


# ==========================================
# 6. UI & TOOLS
# ==========================================

# Dear ImGui
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

# ImGuizmo
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
    target_link_libraries(imguizmo_lib PUBLIC imgui_lib)
endif()