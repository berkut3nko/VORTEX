# Fast start
Ensure you have the Vulkan SDK, compiler, and necessary libraries installed:
```bash
sudo apt install cmake ninja-build clang-18 lld-18 libvulkan-dev vulkan-tools libwayland-dev libxkbcommon-dev xorg-dev
```
`Note: Requires a GPU with Vulkan support.`

## Adding engine into your game (with Cmake)
- Clone to your game directory
```bash
git clone https://github.com/berkut3nko/VORTEX
```
- Add sub-directory
```php
add_subdirectory(engine)
```
- Link dependencies and engine
```php
# --- Linking ---
# The game depends ONLY on the engine core.
# Transitive dependencies (Vulkan, GLFW, ImGui) are propagated via PUBLIC links in the engine.
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE VortexCore)
```
- Copy engine assets
```php
# --- Post-Build Steps ---
# Create an assets directory in the binary output folder
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:Shredborne>/assets
    COMMENT "Creating assets directory..."
)
# --- Shaders Copy ---
# Copies shaders from engine/shaders to build/assets/shaders
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/assets"
    COMMENT "Copying assets to build directory..."
)
```
- In code change current folder or run directly from build (helps find assets)
```C++
#include <filesystem>

    std::filesystem::path exePath = std::filesystem::canonical(argv[0]);
    std::filesystem::path exeDir = exePath.parent_path();
    std::filesystem::current_path(exeDir);
```

# Roadmap
[GitHub Project](https://github.com/users/berkut3nko/projects/2/views/1)

# Code structure
```text
engine/
├── cmake
│   └── Dependencies.cmake
├── CMakeLists.txt
├── LICENSE
├── models
│   └── frank.glb
├── README.md
├── shaders
│   ├── fxaa.comp
│   ├── taa.comp
│   ├── voxel.frag
│   └── voxel.vert
├── src
│   ├── core
│   │   ├── CameraController.cppm
│   │   ├── Core.cppm
│   │   ├── internal
│   │   │   ├── CameraController.cpp
│   │   │   ├── Engine.cpp
│   │   │   └── Profiler.cpp
│   │   ├── Logger.cppm
│   │   └── Profiler.cppm
│   ├── editor
│   │   ├── Editor.cppm
│   │   └── internal
│   │       └── Editor.cpp
│   ├── graphics
│   │   ├── CameraStruct.cppm
│   │   ├── Context.cppm
│   │   ├── Graphics.cppm
│   │   ├── internal
│   │   │   ├── Context.cpp
│   │   │   ├── Graphics.cpp
│   │   │   ├── RenderResources.cpp
│   │   │   ├── SceneManager.cpp
│   │   │   ├── ShaderCompiler.cpp
│   │   │   ├── Swapchain.cpp
│   │   │   ├── UI.cpp
│   │   │   └── Window.cpp
│   │   ├── pipelines
│   │   │   ├── FXAAPipeline.cppm
│   │   │   ├── internal
│   │   │   │   ├── FXAAPipeline.cpp
│   │   │   │   ├── RasterPipeline.cpp
│   │   │   │   └── TAAPipeline.cpp
│   │   │   ├── RasterPipeline.cppm
│   │   │   └── TAAPipeline.cppm
│   │   ├── RenderResources.cppm
│   │   ├── SceneManager.cppm
│   │   ├── Shader.cppm
│   │   ├── Swapchain.cppm
│   │   ├── UI.cppm
│   │   └── Window.cppm
│   ├── memory
│   │   ├── internal
│   │   │   └── MemoryAllocator.cpp
│   │   └── Memory.cppm
│   ├── mesh
│   │   ├── internal
│   │   │   └── MeshConverter.cpp
│   │   ├── MeshConverter.cppm
│   │   └── MeshObject.cppm
│   ├── physics
│   │   ├── ColliderBuilder.cppm
│   │   ├── internal
│   │   │   ├── ColliderBuilder.cpp
│   │   │   └── Jolt.cpp
│   │   └── Physics.cppm
│   └── voxel
│       ├── Chunk.cppm
│       ├── Entity.cppm
│       ├── Hierarchy.cppm
│       ├── Material.cppm
│       ├── Object.cppm
│       ├── Palette.cppm
│       ├── ShapeBuilder.cppm
│       ├── Voxel.cppm
│       └── World.cppm
└── tests
    ├── CMakeLists.txt
    └── test_main.cpp
```

# LICENSE
VORTEX is licensed under the MIT License, see [LICENSE.txt](LICENSE) for more information.