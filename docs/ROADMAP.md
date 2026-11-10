# VORTEX roadmap

What exists today, and what was designed for but is not built yet. See
[ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit together.

## Working now

| Area | State |
|---|---|
| Build | Pinned dependencies, Vulkan headers fetched, shaders compiled to SPIR-V at build time, `cmake --preset dev`. |
| Core | Logging, job system, profiler, input state, RNG, command line. |
| Voxel world | Streaming window of sparse 32^3 chunks, generation on worker threads, editing (set / fill / carve with material hardness), region-file persistence of modified chunks. |
| Terrain | Built-in generator: hills, mountains, beaches, sea, caves, trees. Custom generators through `IWorldGenerator`. |
| Ray queries | One traversal shared by GPU and CPU (`shaders/shared/trace.glsl`), world + dynamic objects through a BVH. |
| Objects | Voxel models of any size shared between instances, collider boxes, mass from material density. |
| Destruction | Connected-component splitting, floating-terrain detection, `Engine::Explode`. |
| Import | MagicaVoxel `.vox` read / write, glTF voxelisation. |
| Physics | Jolt behind an engine interface, fixed time step, terrain colliders streamed around moving things, character controller. |
| Particles | CPU pool with emitters, voxel collision, effect presets. |
| Renderer | Ray-marched G-buffer, ray-traced sun shadows, ambient occlusion, emissive voxel light, point lights, temporal accumulation, fog, particles, TAA, tone mapping, ImGui. Headless mode with screenshots. Validation-clean on Intel UHD 620. |
| Tests | 198 unit tests, none needing a GPU. |

## Planned, not built

### Engine structure
- **Scene / ECS** (`vortex.scene`, EnTT is already a dependency): entities with transform, voxel body,
  rigid body, light, emitter and lifetime components. Today `Engine` keeps a plain object list.
- **Editor** (`vortex.editor`, ImGuizmo is already a dependency): selection and gizmo, voxel brush for
  world and objects, importer UI, lighting and render settings panels, profiler view.
- **Object destruction wired into gameplay**: `SplitComponents` exists and is tested, but only terrain
  is destructible through `Engine::Explode`; damaging an object does not yet split it.
- **CPU reference renderer** (`tools/cpu_render`): software frame from the shared shader code, for
  image checks on machines without a GPU.

### World
- **Level of detail**: clip-map cascades with doubled voxel size for long view distances. The current
  view distance is the window (about 100 m at 0.1 m voxels).
- **Chunk memory**: chunks are dense (37 KiB each). Palette-compressed or brick-sparse payloads.
- **Water and transparency**: water is an opaque material; no refraction, no flow.
- **Biomes, structures, ore veins** in the generator; a data-driven material and generator description.
- **Background saving** and save-format versioning.

### Rendering
- **Reflections** for smooth materials, **volumetric light shafts**, **bloom**, depth of field.
- **Shadowed, lit particles** (particles currently use simple sun + sky lighting).
- **Multi-bounce diffuse light** (only emissive voxels light their surroundings today).
- **Upscaling** better than bilinear, and half-resolution lighting for integrated GPUs.
- **Textured / detailed materials** beyond colour, roughness, metallic and emission.

### Gameplay foundations
- **Animation**: rigs made of voxel parts, skeletal animation, inverse kinematics for characters.
- **Audio**: impact and break sounds per material (ids are reserved in the material).
- **Fire, fluids, structural stress** simulation.
- **Game UI** layer separate from the debug UI; input action mapping and gamepad support.
- **Networking**: server-authoritative simulation. `Vortex::Core` and `Vortex::Physics` already build
  without GPU or window, the simulation runs on a fixed step and the world is stored as chunk deltas.
- **Double-precision physics** for worlds far from the origin (rendering is already origin-relative).

### Tooling
- GCC / MSVC builds (only Clang is tested), CI running the tests and a headless validation render,
  image-comparison tests against stored screenshots.
