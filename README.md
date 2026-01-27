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
├── cmake/
├── shaders/
├── src/
│   ├── core/
│   │   ├── internal/
│   │   │   └── Engine.cpp
│   │   ├── Core.cppm
│   │   ├── Logger.cppm
│   │   └── Profiler.cppm
│   │   
│   ├── math/
│   │   ├── Math.cppm
│   │   └── VoxelMath.cppm
│   │   
│   ├── voxel/
│   │   ├── Voxel.cppm
│   │   ├── Data.cppm
│   │   ├── meshing/
│   │   │   ├── MesherInterface.cppm
│   │   │   ├── Greedy.cpp
│   │   │   ├── Marching.cpp
│   │   │   └── Naive.cpp
│   │   └── storage/
│   │   
│   ├── graphics/
│   │   ├── rhi/
│   │   ├── pipelines/
│   │   ├── internal/
│   │   │   ├── ShaderCompiler.cpp
│   │   │   └── VulkanContext.cpp
│   │   └── Graphics.cppm
│   ├── memory/
│   │   ├── internal/
│   │   │   └── MemoryAllocator.cpp
│   │   └── Memory.cppm
│   │   
│   └── physics/
│       ├── Physics.cppm
│       └── Body.cppm
│        
└── main.cpp
│
└── tests/
```

# LICENSE
VORTEX is licensed under the MIT License, see [LICENSE.txt](LICENSE) for more information.