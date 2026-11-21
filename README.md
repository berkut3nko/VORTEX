# VORTEX

A voxel engine for large, editable worlds rendered by ray marching (Vulkan 1.3, C++20 modules), with a GPU-free
simulation and a session protocol for destructible multiplayer worlds.

- [docs/README.md](docs/README.md) - the document index: what to read for a task, stage IDs, history
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - how the engine is built and the contracts between its parts
- [docs/ROADMAP.md](docs/ROADMAP.md) - what works today, what comes next, measured numbers

## Libraries

| Library | Modules | Links | GPU / window |
|---|---|---|---|
| `Vortex::Core` | `vortex.core`, `math`, `voxel`, `fx` | - | no |
| `Vortex::Physics` | `vortex.physics` (Jolt) | Core | no |
| `Vortex::Sim` | `vortex.scene`, `vortex.sim` (world operations, blasts, regions, the player step) | Core, Physics | no |
| `Vortex::Net` | `vortex.net` (transports, wire format, server, client, bots; ENet private) | Sim | no |
| `Vortex::Graphics` | `vortex.graphics` (Vulkan renderer) | Core | yes |
| `Vortex::Engine` | `vortex.engine`, `vortex.editor` (what a game links) | Core, Physics, Sim, Net, Graphics | yes |

The configure step fails if Core, Physics, Sim, Net or the GPU-free tools ever link Vulkan, GLFW or ImGui
(`vortex_assert_gpu_free`), and if Core, Physics or Sim link Net or ENet (`vortex_assert_no_network`).
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) section 2.

Tools (`tools/`, link `Vortex::Net` only, so they build and run without a GPU or a window):

- `vortex_server` - the session server: a session file (`tools/server/sessions/*.session`), UDP on `--listen`, or
  headless runs with in-process bot clients (drills, the arena walk). Its usage is at the top of
  `tools/server/main.cpp`.
- `vortex_bot` - one bot player in its own process over UDP, for soaks and measurements. Usage at the top of
  `tools/bot/main.cpp`.

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

The in-game editor is opt-in: `import vortex.editor;`, own a `vortex::editor::Editor`, call
`editor.OnUpdate(engine, dt)` first in `OnUpdate`, skip your own input handling while
`editor.CapturesInput()`, and call `editor.OnGui(engine)` from `OnGui`. F1 toggles it; hold the
right mouse button + WASD to fly, 1 / 2 / 3 choose the select / brush / spawn tool, G / R the
gizmo mode, Del deletes the selection. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) section 8.

## Building

Tested toolchain: Clang 23, CMake 4.4, Ninja (Arch Linux). Minimums: Clang >= 18 (C++20 named modules; another
compiler gets a configure warning), CMake >= 3.28, Ninja.

Requirements:
- A Vulkan 1.3 driver and the Vulkan loader (not with `VORTEX_GRAPHICS=OFF`: see Options).
- `glslangValidator` (built from source automatically if missing).
- An x86-64 CPU with AVX2, FMA, F16C, BMI, LZCNT and POPCNT: Jolt's flags, applied to every target through
  `vortex_options` so the module interfaces agree. Every executable links `vortex_cpu_check` (`src/startup/CpuCheck.c`),
  which refuses an older CPU at start (exit code 4, the missing features named).
- All other dependencies are fetched at pinned versions (`cmake/Dependencies.cmake`).

Options: `VORTEX_BUILD_TESTS` (default on when the engine is the top-level project), `VORTEX_BUILD_TOOLS`
(`vortex_server`, `vortex_bot`; on), `VORTEX_WARNINGS_AS_ERRORS` (off), `VORTEX_GRAPHICS` (on; off builds only Core,
Physics, Sim, Net, the tools and their tests, with no Vulkan loader, GLFW or ImGui: a server host or a CI job without
a GPU). Tests are GoogleTest suites per library (`VortexCoreTests`, `VortexPhysicsTests`, `VortexSimTests`,
`VortexNetTests`, `VortexGraphicsTests`, `VortexEngineTests`; the last two only with `VORTEX_GRAPHICS`) plus
`CpuCheck.RefusesAMissingFeature`; run them with `ctest`. Every program logs its build first (`Build: ...`, both
commits, build type, compiler, CPU flags: `cmake/BuildInfo.cmake`) and prints it with `--version`.

API documentation: `doxygen` in this directory writes HTML to `engine/build/doxygen/html/` (untracked).

MIT licensed, see [LICENSE](LICENSE).
