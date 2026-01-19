cmake_minimum_required(VERSION 3.28)

# @brief VORTEX Engine Project
# @details Defines the core library and internal dependencies.
project(VORTEX VERSION 0.1.0 LANGUAGES CXX C)

# --- C++20 Modules Support (Clang) ---
# Scanner logic is placed here as the engine provides the modules.
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

# --- Compiler Options ---
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# --- Dependency Management ---
# Include external dependencies (GLFW, ImGui, etc.)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

# --- Core Library Definition ---
add_library(VortexCore STATIC)

# Enforce C++20 features
target_compile_features(VortexCore PUBLIC cxx_std_20)

# C++ Sources
target_sources(VortexCore PRIVATE
    src/Core.cpp
    # Add other engine source files here
)

# C++ Modules (Interfaces)
# target_sources(VortexCore PUBLIC
#     FILE_SET CXX_MODULES FILES 
#     src/Engine.cppm
# )

# Enable module scanning for the library
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_target_properties(VortexCore PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

# Include Directories
target_include_directories(VortexCore PUBLIC 
    include
    ${CMAKE_CURRENT_BINARY_DIR} # For generated configuration files
)

# --- Dependency Linking ---
# PUBLIC linkage ensures the Game can access headers from these libraries
target_link_libraries(VortexCore PUBLIC
    Vulkan::Vulkan
    glfw
    imgui_lib
    tinygltf_lib
)

# --- Testing ---
if(BUILD_VORTEX_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()