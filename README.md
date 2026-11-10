# VORTEX

A voxel engine for large, editable worlds rendered by ray marching (Vulkan 1.3, C++20 modules).

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - how the engine is built and the contracts between its parts
- [docs/ROADMAP.md](docs/ROADMAP.md) - what works today and what is planned

## Using it from a game

```cmake
add_subdirectory(engine)
add_executable(MyGame src/main.cpp)
target_link_libraries(MyGame PRIVATE Vortex::Engine)
vortex_deploy_assets(MyGame)        # copies compiled shaders and models next to the executable
```

```cpp
import vortex.engine;

class MyGame : public vortex::Application {
    void OnUpdate(vortex::Engine& engine, float dt) override { /* input, camera, gameplay */ }
};

int main(int argc, char** argv) {
    vortex::EngineConfig config;
    config.ApplyCommandLine(vortex::CommandLine(argc, argv));
    vortex::Engine engine(config);
    if (!engine.Initialize()) return 1;
    MyGame game;
    return engine.Run(game);
}
```

Requirements: Clang >= 18, CMake >= 3.28, Ninja, a Vulkan 1.3 driver, `glslangValidator` (built from
source automatically if missing). All other dependencies are fetched at pinned versions.

Libraries: `Vortex::Core` (voxel world, no GPU), `Vortex::Physics`, `Vortex::Graphics`, `Vortex::Engine`.

MIT licensed, see [LICENSE](LICENSE).
