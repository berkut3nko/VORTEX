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
    src/core/internal/Engine.cpp
    src/graphics/internal/VulkanContext.cpp
    src/memory/internal/MemoryAllocator.cpp
    src/graphics/internal/ShaderCompiler.cpp
)

# C++ Modules (Interfaces)
target_sources(VortexCore PUBLIC
    FILE_SET CXX_MODULES FILES
    
    src/core/Core.cppm
    src/core/Logger.cppm
    src/core/Profiller.cppm

    src/graphics/Graphics.cppm
    
    src/graphics/Shader.cppm

    src/memory/Memory.cppm
    
    src/voxel/BeamOptimization.cppm
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