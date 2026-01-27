cmake_minimum_required(VERSION 3.28)

# @brief VORTEX Engine Project
# @details Defines the core library and internal dependencies.
project(VORTEX VERSION 0.1.0 LANGUAGES CXX C)

# --- C++20 Modules Support (Clang) ---
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API "2182bf5c-ef0d-489a-91da-49dbc3090d2a")
    set(CMAKE_EXPERIMENTAL_CXX_MODULE_DYNDEP ON)

    find_program(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS 
        NAMES clang-scan-deps-18 clang-scan-deps
        DOC "Path to clang-scan-deps"
    )

    if(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS)
        message(STATUS "Using C++ Module Scanner: ${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}")
    else()
        message(WARNING "Clang-scan-deps not found. Modules might not work correctly.")
    endif()
endif()


# --- Dependency Management ---
# Include dependencies BEFORE defining the target so FetchContent logic runs first
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

# --- Core Library Definition ---
add_library(VortexCore STATIC)

# Enforce C++20 features
target_compile_features(VortexCore PUBLIC cxx_std_20)

# --- Compiler Options & Warning Suppression ---
# 1. Base warnings for strict code quality
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(VortexCore PRIVATE 
        -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
    )
endif()

# 2. Suppress specific Clang warnings caused by external libs (VMA, ImGui macros)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(VortexCore PRIVATE
        -Wno-nullability-completeness
        -Wno-nullability-extension
        -Wno-unused-private-field
        -Wno-missing-field-initializers
        -Wno-unused-variable
        -Wno-unused-parameter
        -Wno-gnu-zero-variadic-macro-arguments
    )
endif()

# C++ Sources
target_sources(VortexCore PRIVATE
    src/editor/internal/Editor.cpp

    src/core/internal/Engine.cpp
    src/core/internal/Profiler.cpp
    src/core/internal/CameraController.cpp
    src/memory/internal/MemoryAllocator.cpp

    src/graphics/internal/ShaderCompiler.cpp
    src/graphics/internal/Graphics.cpp
    src/graphics/internal/UI.cpp
    src/graphics/internal/Swapchain.cpp
    src/graphics/internal/Window.cpp
    src/graphics/internal/Context.cpp
    src/graphics/internal/SceneManager.cpp
    src/graphics/internal/RenderResources.cpp

    src/graphics/pipelines/internal/RasterPipeline.cpp
    src/graphics/pipelines/internal/TAAPipeline.cpp
    src/graphics/pipelines/internal/FXAAPipeline.cpp

    src/mesh/internal/MeshConverter.cpp
)

# C++ Modules (Interfaces)
target_sources(VortexCore PUBLIC
    FILE_SET CXX_MODULES FILES
    
    # Editor
    src/editor/Editor.cppm

    # Core
    src/core/Core.cppm
    src/core/CameraController.cppm
    src/core/Logger.cppm
    src/core/Profiler.cppm
    src/memory/Memory.cppm

    # Graphics Modules
    src/graphics/Graphics.cppm
    src/graphics/UI.cppm
    src/graphics/Swapchain.cppm
    src/graphics/Window.cppm
    src/graphics/Context.cppm
    src/graphics/Shader.cppm
    src/graphics/RenderResources.cppm
    src/graphics/CameraStruct.cppm
    src/graphics/SceneManager.cppm

    #src/graphics/pipelines/RayTracingPipeline.cppm
    src/graphics/pipelines/RasterPipeline.cppm
    src/graphics/pipelines/TAAPipeline.cppm
    src/graphics/pipelines/FXAAPipeline.cppm
    
    # Voxel Engine Modules
    src/voxel/Voxel.cppm
    src/voxel/Chunk.cppm
    src/voxel/World.cppm
    src/voxel/Material.cppm
    src/voxel/Palette.cppm
    src/voxel/Hierarchy.cppm
    src/voxel/Object.cppm
    src/voxel/ShapeBuilder.cppm
    src/voxel/Entity.cppm

    # Dynamic Mesh Objects
    src/mesh/MeshObject.cppm
    src/mesh/MeshConverter.cppm
    
)


# Enable module scanning for the library
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_target_properties(VortexCore PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

target_include_directories(VortexCore PUBLIC 
    include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# --- Dependency Linking ---
# PUBLIC linkage ensures the Game can access headers from these libraries
target_link_libraries(VortexCore PUBLIC
    # Graphics
    Vulkan::Vulkan
    vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glfw
    glslang::glslang
    
    # Math & Physics
    glm::glm
    JoltPhysics
    
    # ECS & Utils
    EnTT::EnTT
    spdlog::spdlog
    bitsery::bitsery
    flatbuffers::flatbuffers
    stb_lib
    
    # UI
    imgui_lib
    imguizmo_lib
    tinygltf_lib
)

# --- Testing ---
if(BUILD_VORTEX_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()