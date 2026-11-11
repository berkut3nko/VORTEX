# VORTEX architecture

VORTEX is a voxel engine for large, fully editable worlds rendered by ray marching, in the
spirit of Teardown's renderer, with the scale and freedom of a Minecraft-like world. This
document is the reference for how the engine is put together and the contract its parts
share. If code and document disagree, fix one of them.

## 1. Goals and non-goals

| Goal | Consequence in the design |
|---|---|
| Huge, streaming, persistent world | Sparse chunk storage, a toroidal window that moves with the player, generation on worker threads, region files that store only what players changed. (Persistence is a feature of `voxel::World`, used by the offline game and the map editor; a networked session's world is memory only, 7.4.) |
| Everything destructible | Voxels are the single source of truth for rendering, collision and gameplay queries; edits are local and cheap (one 37 KiB chunk re-upload). |
| Teardown-style image | Geometry is ray marched (no meshing); lighting is ray traced through the same data (soft sun shadows, ambient occlusion, emissive voxels) and accumulated over time. |
| Runs on integrated GPUs | Rendering at a fraction of the output resolution, one ray of each kind per pixel per frame, every feature can be switched off. |
| Testable without a GPU | Everything except `graphics` builds and runs headless; the ray traversal is one source file compiled for both the GPU and the CPU, so unit tests cover the shader logic. |

Not goals (yet): level of detail cascades for very long view distances, transparency /
refraction, fluid simulation (water is a shaded surface, 5.7), skeletal animation, audio, networking. Section 10 lists what the design already
does to keep those doors open.

## 2. Layout and layering

```
engine/
  cmake/                 Dependencies.cmake (pinned), VortexShaders.cmake (GLSL -> SPIR-V at build time)
  shaders/
    shared/              code compiled by BOTH glslang and the C++ compiler (trace, shading, cube)
    include/             GPU-only includes; common.glsl is the C++ <-> GLSL contract
    *.vert *.frag        one SPIR-V module each
  src/
    core/     vortex.core      log, jobs, profiler, input state, paths, rng, args, game HUD
                               (canvas, bitmap font, menu, cursor capture: data only)
    math/     vortex.math      AABB / frustum / transform, reverse-Z projection, simplex noise
    voxel/    vortex.voxel     chunks, chunk store, world streaming, generator, regions, models,
                               ray queries, colliders, connectivity, .vox / glTF import
    fx/       vortex.fx        particle simulation and effect presets
    physics/  vortex.physics   Jolt behind an engine interface, terrain collider streaming
    scene/    vortex.scene     entities (EnTT): components, object lifetime, destruction
    sim/      vortex.sim       the simulation: world + physics + terrain colliders + scene on a numbered tick
    graphics/ vortex.graphics  Vulkan device, window, renderer, UI layer
    engine/   vortex.engine    application loop, render data of the scene, explosions, glue
    editor/   vortex.editor    in-game editor: tool logic (no ImGui), panels, gizmo
  tests/                 GoogleTest suites, none of which need a GPU
  tools/                 cpu_render: software renderer built on the shared shader code
```

Libraries and what they may depend on (a lower layer never imports a higher one):

```
Vortex::Core      core, math, voxel, fx        -> spdlog, glm, tinygltf      (no GPU, no window)
Vortex::Physics   physics                      -> Core, Jolt
Vortex::Sim       scene, sim                   -> Core, Physics, EnTT        (no GPU, no window)
Vortex::Graphics  graphics                     -> Core, Vulkan, VMA, vk-bootstrap, GLFW, ImGui
Vortex::Engine    engine, editor               -> all of the above, ImGuizmo
```

A dedicated server or a tool links `Vortex::Sim` (or less) and never sees Vulkan. This is checked
when the build is configured: `vortex_assert_gpu_free` (`CMakeLists.txt`) walks everything that
`VortexCore`, `VortexPhysics`, `VortexSim` and the test binary `VortexSimTests` link, directly or
through their dependencies, and stops with an error if Graphics, Engine, Vulkan, GLFW, VMA or
ImGui is among it.

Inside `Vortex::Sim`, `scene` is the lower layer: it imports voxel, fx and physics, nothing of
graphics and not `vortex.sim`. `sim` owns the scene together with the world, physics and the
terrain colliders (`sim::Simulation`, 7.4). In `Vortex::Engine`, `engine` owns one
`sim::Simulation`, steps it and draws it; the two functions that turn a scene into render data
(`GatherInstances`, `GatherLights`) live there because their result types are graphics types.
`editor` is the top layer: it imports `vortex.engine`, and nothing in `engine` or below knows it
exists - a game that wants the editor owns an `editor::Editor` and forwards two callbacks to it
(section 8).

## 3. Conventions

* **C++20 named modules**, Clang. One module per directory (`vortex.<dir>`), one partition per
  `.cppm` file, re-exported by the primary interface (`Voxel.cppm` ...). Implementation units
  (`internal/*.cpp`) start with `module vortex.<dir>;`.
  * All `#include`s go in the global module fragment (between `module;` and the module
    declaration), standard and third-party headers alike. Names from headers included by an
    imported module are **not** visible to the importer: include what you use.
  * Interfaces are contracts: implementation work must not change exported signatures.
* **Naming**: `PascalCase` types and functions, `m_PascalCase` private members, `camelCase`
  public fields and locals, `kPascalCase` constants. Namespaces `vortex`, `vortex::math`,
  `vortex::voxel`, `vortex::fx`, `vortex::physics`, `vortex::graphics`, `vortex::scene`, `vortex::sim`,
  `vortex::editor`.
* **Errors**: no exceptions across engine APIs. Functions that can fail return `bool` /
  `optional` / an invalid handle and log the reason (`Log::Error("... {}", x)`, std::format
  syntax). `Verify(cond, msg)` is for invariants whose violation would corrupt memory.
  Running out of a fixed budget (chunk slots, particles, GPU buffer space) must degrade
  gracefully - drop, log once - and never write out of bounds.
* **Threading**: public interfaces are main-thread only unless documented otherwise. Worker
  jobs (`JobSystem`) get their inputs by value and hand results back through a queue owned by
  the system that submitted them.
* **Units and axes**: metres, seconds, kilograms, radians. Right-handed, +Y up, the camera
  looks down -Z at yaw 0. Colours are linear RGB everywhere except in files and the final
  output transform.
* **Warnings**: engine code compiles clean with `-Wall -Wextra -Wpedantic`.

## 4. Voxel data

### 4.1 Chunk (36 936 bytes, identical on CPU and GPU)

A chunk is 32^3 voxels. `voxel::Chunk` = occupancy pyramid + one byte per voxel:

```
ChunkOccupancy  (4168 bytes = 1042 uint32, uploaded verbatim)
  word 0..1     cellMask   64 bits   8^3 cells   bit = cx + cy*4 + cz*16      (cx = x >> 3)
  word 2..17    blockMask  512 bits  4^3 blocks  bit = bx + by*8 + bz*64      (bx = x >> 2)
  word 18..1041 voxelMask  512 x 64  voxels      word 18 + block*2 + (bit >> 5), bit = lx + ly*4 + lz*16 (lx = x & 3)
ids[32768]      material id per voxel, block-ordered: index = block * 64 + bit
```

`Chunk::Set` maintains all three levels in both directions (removing the last voxel of a
block clears its bits). A ray walks only the pyramid and reads `ids` once, at the hit.

Voxel id 0 is empty. Ids 1..255 index the *palette* of whatever the chunk belongs to
(`MaterialLibrary`: palette 0 is the world palette, each imported model can bring its own).
On the GPU all palettes form one array: material index = `palette * 256 + id`.

### 4.2 Chunk store

`voxel::ChunkStore` is a pool of chunks addressed by **slot**. The slot is the identity of a
chunk everywhere: the world grid stores slots, objects reference slots, the CPU tracer resolves
slots and the renderer mirrors slot `i` at offset `i` of its GPU buffers. Anything that changes
a chunk calls `MarkDirty(slot)`; the renderer drains `TakeDirty()` once per frame.

### 4.3 World

`voxel::World` keeps a box of chunks around a focus point - the *window*, `windowSize` chunks. Each window cell is `Unloaded`, `Loading`, `Empty`, `Uniform` (one solid
material, no storage) or `Dense` (owns a slot). Cells are stored in a toroidal array
(`index = coord mod size`, `x + z*sx + y*sx*sz`), so moving the window re-targets one slab
of cells and moves no data.

`World::Grid()` is the same array in GPU encoding: `0` empty, `0x80000000 | id` uniform,
`slot + 1` dense. It is uploaded as is.

Loading runs on the job system (`RegionStore` first, generator second), nearest chunks
first; results are installed on the main thread with a per-frame budget. Cells carry a
generation counter so results for a chunk that has left the window are dropped.

Editing (`SetVoxel`, `FillBox`, `FillSphere`, `CarveSphere`) materialises Empty / Uniform
cells on demand, marks slots dirty, flags the cell as modified (it will be saved when it
leaves the window or on `Save()`), and records the chunk in `TakeChangedChunks()` for physics.

**Render space.** World positions are `double`. Everything a ray or the GPU touches is
expressed relative to `World::WindowOrigin()` (the window's minimum corner): *render space*.
Coordinates therefore stay below a few hundred metres and keep full float precision anywhere
in the world. When the window moves, the origin jumps by whole chunks; the renderer
re-expresses last frame's matrices in the new space.

### 4.4 Objects

A dynamic object is a `voxel::VoxelModel`: a dense `VoxelVolume` of any size plus the chunks
it was *baked* into (`ModelPart`: slot, offset, tight bounds). Models are shared between
entities; a model is never modified through an entity - destruction gives the damaged entity a
private new model (section 7). The chunk slots of a model go back to the store with its last
reference. Baking is all or nothing: if the store runs out of slots the model is left unbaked
and holds none. `Revision()` changes on every bake so dependants (physics shapes) know when to refresh.

For rendering and ray queries each part becomes a `GpuInstance` (transform + slot + bounds +
palette); a BVH over the instances (`BuildBvh`, rebuilt per frame) lets rays find them.

### 4.5 The shared traversal

`shaders/shared/trace.glsl` is written in the subset of GLSL that is also valid C++ (see
`shared/vx_cpp.h` for the rules). glslang compiles it into the shaders; the C++ compiler
compiles the same text as member functions of `voxel`'s `TraceKernel`, which backs
`voxel::Trace`, `World::Raycast` and the unit tests. One implementation, verified on the CPU
against a reference DDA, runs on the GPU.

* `VxTraceChunk` - hierarchical walk of one chunk: empty 8^3 cells and 4^3 blocks are left in
  one step. Exit positions are computed from the plane crossed (no accumulation, no epsilons).
* `VxTraceWorld` - DDA over the chunk grid in window-local voxel units; uniform chunks are hit
  as boxes, dense chunks descend into `VxTraceChunk`.
* `VxTraceInstances` - stack-based BVH traversal, then `VxTraceChunk` in each instance's
  local space. The local direction is not renormalised, so `t` stays comparable across objects.
* `VxTraceScene` - world + instances, closest or any hit. Inputs in render space, metres.

## 5. Rendering

Vulkan 1.3 with dynamic rendering and synchronization2. All passes are full-screen or
instanced draws with **one descriptor set (set 0)** whose layout is reflected from SPIR-V;
resources are bound by name (`DescriptorWriter`). `shaders/include/common.glsl` defines the
per-frame uniform block, every buffer binding and the G-buffer encoding; `graphics::FrameUniforms`
mirrors the block. Unit tests compare the two through reflection.

### 5.1 Conventions

* **Depth** is reversed with an infinite far plane (`math::PerspectiveReverseZ`): 1 at the
  near plane, 0 at infinity; the depth buffer is cleared to 0 and tested with `GREATER`.
  Ray marched fragments write `gl_FragDepth = near / (t * dot(rd, forward))`.
* **Jitter** (TAA) is added in clip space: `proj[2][0] = -jitter.x`, `proj[2][1] = -jitter.y`
  (jitter in NDC units, a Halton(2,3) offset of +-0.5 pixel = `(h - 0.5) * 2 / resolution`).
  `VxCameraRay(uv)` subtracts the same jitter, so ray marched and rasterised geometry agree.
* **Facing.** Vulkan decides facing from the on-screen winding, and the projection keeps the
  image upright, so geometry that is counter-clockwise seen from outside uses
  `VK_FRONT_FACE_COUNTER_CLOCKWISE`. `shared/cube.glsl` provides the proxy cube
  (`VxCubeCorner`), and a unit test checks its winding against the projection.
* **Colour**: lighting in linear HDR (`R16G16B16A16_SFLOAT`); the present pass applies
  exposure, tone mapping and the sRGB curve into a UNORM backbuffer (which is also what ImGui
  expects).

### 5.2 Buffers (set 0)

| binding | name | contents | written |
|---|---|---|---|
| 0 | `u_Frame` | `FrameUniforms` (UBO) | every frame |
| 1 | `u_Grid` | world grid, `uint[]` | when `World::GridRevision()` changes or uploads are pending |
| 2 | `u_Occ` | occupancy pyramids, 1042 words per slot | dirty slots |
| 3 | `u_Ids` | voxel ids, 8192 words per slot | dirty slots |
| 4 | `u_Materials` | `VxMaterial[]`, 256 per palette (`params.w` = surface kind: 0 solid, 1 liquid) | when `MaterialLibrary::Revision()` changes |
| 5 | `u_Instances` | `VxInstance[]` | every frame |
| 6 | `u_Bvh` | `VxBvhNode[]` | every frame |
| 7 | `u_DrawList` | instance indices to rasterise: front-facing boxes first, then boxes containing the camera | every frame |
| 8 | `u_Lights` | `VxLight[]` | every frame |
| 9 | `u_Particles` | `VxParticle[]` | every frame |
| 10 | `u_Hud` | `VxHudQuad[]` (= `vortex::HudQuad`, 32 bytes: rect in output pixels, RGBA8 colour, 45 cell bits) | every frame |
| 16+ | per-pass textures | declared by each shader | |

`u_Occ`, `u_Ids`, `u_Grid`, `u_Materials` are device-local and filled through a per-frame
staging buffer with `vkCmdCopyBuffer` at the start of the frame's command buffer (never
written while a frame in flight reads them). Chunk uploads are budgeted per frame
(`RenderSettings::maxChunkUploadsPerFrame`); the GPU copy of the grid only exposes a dense
chunk once its data has been uploaded (until then the entry is written as empty), so the GPU
never traverses a slot it has not received. Per-frame data (uniforms, instances, BVH, draw
list, lights, particles, HUD quads) lives in host-visible buffers owned by each frame-in-flight; they grow
(power of two) when needed and the old buffer goes to that frame's deletion queue. An
always-bound buffer is never zero-sized: empty arrays still bind a small valid buffer.

### 5.3 Render targets (R = render resolution = output * renderScale, O = output)

| target | format | size | content |
|---|---|---|---|
| gMaterial | `R32_UINT` | R | bits 0..23 global material index, 24..31 per-voxel variation; 0 = sky |
| gNormal[2] | `A2B10G10R10_UNORM_PACK32` | R | world normal * 0.5 + 0.5 (ping-pong: current / previous frame) |
| gVelocity | `R16G16_SFLOAT` | R | uv(current) - uv(previous) |
| depth[2] | `D32_SFLOAT` | R | reverse-Z, 0 = sky (ping-pong) |
| lighting[2] | `R16G16B16A16_SFLOAT` | R | rgb = accumulated diffuse irradiance, a = accumulated sun specular amount (liquids: accumulated sun visibility) |
| lightCount[2] | `R8_UNORM` | R | accumulated frame count / 255 |
| hdr | `R16G16B16A16_SFLOAT` | R | lit scene + particles |
| taa[2] | `R16G16B16A16_SFLOAT` | R | resolved history |
| backbuffer | swapchain image, or `R8G8B8A8_UNORM` offscreen when headless | O | |

### 5.4 Passes

Every pass that samples a target transitions it to `SHADER_READ_ONLY_OPTIMAL` first and every
pass that renders to one transitions it to its attachment layout (`TransitionImage` tracks the
current layout per image). All full-screen passes use `fullscreen.vert` (3 vertices, no buffers).

1. **gbuffer** - attachments gMaterial, gNormal[cur], gVelocity, depth[cur]; all cleared (depth 0).
   * `gbuffer_world.frag` (full-screen, depth test GREATER + write): marches
     `VxTraceWorld(u_Frame.cameraPosVoxel.xyz, VxCameraRay(uv), 0, big)`; a miss discards.
     Writes the packed material (world palette = 0, variation = `VxVoxelHash` of the absolute
     voxel), normal, motion vector (static world: previous position = current position) and
     `gl_FragDepth`.
   * `gbuffer_object.vert` + `gbuffer_object_front.frag` (instanced, 36 vertices, cull BACK):
     one proxy box per instance = its tight voxel bounds. The fragment shader takes the pixel's
     own view ray (`VxCameraRay`), transforms it into chunk-local space, clips it against the
     bounds and runs `VxTraceChunk`. `layout(depth_less)` lets early-Z reject hidden boxes.
     Motion vector uses `prevLocalToWorld`. Draws instances `[0, frontCount)` of the draw list.
   * `gbuffer_object_inside.frag` (same source, cull FRONT, no depth qualifier): for the few
     boxes that contain the camera or straddle the near plane. Draws `[frontCount, frontCount + insideCount)`.
   * With debug view `VX_DEBUG_STEPS` both write material `VX_GBUFFER_DEBUG` and a heat colour
     of the step count into the normal target.
2. **lighting** - attachments lighting[cur], lightCount[cur]. Per pixel with geometry: one
   jittered sun shadow ray (`VxTraceScene`, any hit), one cosine-weighted ambient ray of length
   `ambientParams.y` (miss -> sky radiance of that direction; hit an emissive voxel -> its
   emission, which is how glowing voxels light their surroundings), all point lights in range
   with a single stochastically chosen shadow ray, and a sun specular term. The result is
   blended with the reprojected history (lighting[prev], validated against depth[prev] and
   gNormal[prev]) with weight `1 / min(count + 1, maxHistory)`.
3. **composite** - attachment hdr. Sky for empty pixels; otherwise albedo (material colour
   with per-voxel variation) x filtered lighting + emission + specular, then height fog.
   The lighting is filtered with a small depth / normal aware kernel that widens while the
   history is short. Liquids are shaded here as an animated surface (5.7). Debug views are
   produced here.
4. **particles** - attachment hdr (load), depth[cur] as read-only test (no write). Instanced
   camera-facing quads from `u_Particles`, premultiplied blending (additive particles have
   opacity 0), sorted back to front on the CPU.
5. **taa** - attachment taa[cur]. Reprojects taa[prev] with the motion vector of the closest
   depth in a 3x3 neighbourhood, clamps it to the neighbourhood colour box, blends with weight
   `temporalParams.y`. Pass-through when TAA is off or the history is invalid.
6. **present** - attachment backbuffer (O). Bilinear upscale, exposure, tone mapping, sRGB
   curve, the HUD backdrop dim (`settings.w`, section 11), dither; then in the same rendering
   scope:
7. **hud** - the game interface (section 11): `hud.vert` / `hud.frag`, one instanced draw
   `vkCmdDraw(6, quadCount, 0, 0)` of `u_Hud`, premultiplied blending, display-space colours, no
   texture: each quad is a 5 x 9 cell grid and a cell is drawn when its bit is set (solid
   rectangles have all 45 bits, glyphs their bitmap). Works headless (it is in screenshots).
8. **imgui** - `UiLayer::Draw` (debug overlay and editor; records nothing when no window was
   submitted).

Passes 6..8 are three timestamp intervals inside one rendering scope.

After the frame the `cur` / `prev` indices swap. History is invalidated (`temporalParams.z = 0`)
on the first frame, after a resize and after a camera cut.

### 5.5 Frame

```
RenderFrame(input)
  resize / minimise handling, wait for this frame slot's fence, flush its deletion queue
  acquire swapchain image (windowed); out-of-date -> recreate and skip the frame
  begin command buffer, reset this slot's descriptor pool
  sync voxel data: staged copies for dirty slots (budgeted), grid, materials
  build frame data in render space: uniforms, instances (+ frustum culling into the draw list,
    front / inside classification), BVH, lights, particles
  passes 1..8 with GPU timestamps (6..8 share the backbuffer rendering scope)
  screenshot copy if requested
  submit, present (windowed); out-of-date -> recreate
```

Two frames in flight. Each owns: command pool + buffer, fence, image-acquired semaphore,
descriptor pool (reset each use), its dynamic buffers, a staging buffer, a timestamp query
pool and a deletion queue. Render-finished semaphores are per swapchain image.

Headless mode has no swapchain: the backbuffer is an offscreen image and "present" is a no-op;
`RequestScreenshot` writes it as PNG. This is how the renderer is exercised on CI.

### 5.6 Shader interface (the exact contract between `Renderer` and the shader files)

Pipelines, their shader files (compiled to `assets/shaders/<file>.spv`), state and resources.
Buffers are the blocks declared in `common.glsl`; textures are declared by the shader itself
with exactly these names, bindings and types. `nearest` / `linear` name the sampler the
renderer binds (both clamp-to-edge); G-buffer inputs are read with `texelFetch`.

| pipeline | vertex / fragment | colour attachments | depth | cull, front face | blend |
|---|---|---|---|---|---|
| gbuffer_world | `fullscreen.vert` / `gbuffer_world.frag` | R32_UINT, A2B10G10R10, R16G16_SFLOAT | D32, test GREATER, write | none | opaque |
| gbuffer_object_front | `gbuffer_object.vert` / `gbuffer_object_front.frag` | same | D32, test GREATER, write | BACK, CCW | opaque |
| gbuffer_object_inside | `gbuffer_object.vert` / `gbuffer_object_inside.frag` | same | D32, test GREATER, write | FRONT, CCW | opaque |
| lighting | `fullscreen.vert` / `lighting.frag` | RGBA16F, R8_UNORM | - | none | opaque |
| composite | `fullscreen.vert` / `composite.frag` | RGBA16F | - | none | opaque |
| particles | `particles.vert` / `particles.frag` | RGBA16F | D32, test GREATER, no write | none | premultiplied |
| taa | `fullscreen.vert` / `taa.frag` | RGBA16F | - | none | opaque |
| present | `fullscreen.vert` / `present.frag` | backbuffer format | - | none | opaque |
| hud | `hud.vert` / `hud.frag` | backbuffer format | - | none | premultiplied |

Draw calls: full-screen passes `vkCmdDraw(3, 1, 0, 0)`; object boxes `vkCmdDraw(36, count, 0, firstInstance)`
where the vertex shader reads `u_DrawList.indices[gl_InstanceIndex]` (in Vulkan `gl_InstanceIndex`
includes `firstInstance`); particles `vkCmdDraw(6, particleCount, 0, 0)` (two triangles per
instance, corner derived from `gl_VertexIndex`); HUD `vkCmdDraw(6, quadCount, 0, 0)` the same way.

`fullscreen.vert` outputs `layout(location = 0) out vec2 v_Uv` with (0,0) at the top-left.

Resources per shader (set 0):

| shader | buffers | textures (binding: name, type, sampler) |
|---|---|---|
| `gbuffer_world.frag` | `u_Frame`, `u_Grid`, `u_Occ`, `u_Ids`, `u_Instances`, `u_Bvh` (via `VX_NEED_TRACE`) | - |
| `gbuffer_object.vert` | `u_Frame`, `u_Instances`, `u_DrawList` | - |
| `gbuffer_object_*.frag` | `u_Frame`, `u_Grid`, `u_Occ`, `u_Ids`, `u_Instances`, `u_Bvh` | - |
| `lighting.frag` | `u_Frame`, trace buffers, `u_Materials`, `u_Lights` | 16 `u_GMaterial` usampler2D nearest; 17 `u_GNormal` sampler2D nearest; 18 `u_GDepth` sampler2D nearest; 19 `u_GVelocity` sampler2D nearest; 20 `u_PrevLighting` sampler2D linear; 21 `u_PrevLightCount` sampler2D linear; 22 `u_PrevDepth` sampler2D nearest; 23 `u_PrevNormal` sampler2D nearest |
| `composite.frag` | `u_Frame`, `u_Materials` | 16 `u_GMaterial` usampler2D nearest; 17 `u_GNormal` sampler2D nearest; 18 `u_GDepth` sampler2D nearest; 19 `u_Lighting` sampler2D nearest; 20 `u_LightCount` sampler2D nearest |
| `particles.vert` / `.frag` | `u_Frame`, `u_Particles` | - |
| `taa.frag` | `u_Frame` | 16 `u_Color` sampler2D nearest; 17 `u_History` sampler2D linear; 18 `u_GVelocity` sampler2D nearest; 19 `u_GDepth` sampler2D nearest |
| `present.frag` | `u_Frame` | 16 `u_Color` sampler2D linear |
| `hud.vert` / `.frag` | `u_Frame`, `u_Hud` | - |

A shader that includes the trace buffers but does not use all of them still declares them
(the reflected layout then contains them and the renderer binds them): the renderer binds
every binding a pipeline reflects, by name, and `DescriptorWriter` reports anything missing.
Depth targets are sampled as `sampler2D` (`.r`) while in `SHADER_READ_ONLY_OPTIMAL`; in the
particle pass the same image is the depth attachment in `DEPTH_READ_ONLY_OPTIMAL`.

G-buffer fragment outputs: `layout(location = 0) out uint o_Material; layout(location = 1) out vec4 o_Normal;
layout(location = 2) out vec2 o_Velocity;` plus `gl_FragDepth`. Lighting outputs:
`layout(location = 0) out vec4 o_Lighting; layout(location = 1) out float o_Count;`.

### 5.7 Liquids

There is no water simulation and no transparency: water and lava are ordinary opaque voxels
(`MaterialFlag_Liquid`), traced, lit and accumulated exactly like stone. `MaterialLibrary` packs
the flag into `GpuMaterial::params.w` (`kGpuMaterialLiquid` = `VX_MATERIAL_LIQUID`), and the
composite pass - which reads the material anyway - shades such pixels with `ShadeLiquid` instead
of the diffuse + specular sum. No extra pass, target, texture or uniform.

* **Animated normal.** On faces that look up, the flat G-buffer normal is replaced by the normal
  of a procedural height field (`WaterSlope`): one octave of value noise with an analytic gradient
  for the large, irregular swell, plus four directional sine ripples whose phase is bent by that
  noise so they never form a regular pattern. Everything drifts with `temporalParams.w` (time).
  It is evaluated in *world* space: render-space position + `(gridMinChunk.xz & 1023) * chunk
  size`. Render space jumps by whole chunks when the window moves and `gridMinChunk` jumps with
  it, so the sum stays put; the wrap keeps float precision (the pattern changes once, in one
  frame, per 1024 chunks travelled; time wraps once per hour the same way).
* **No shimmer.** Each octave fades out before its wavelength reaches the pixel's footprint on
  the surface (distance x pixel angle / sine of the grazing angle), so distant water is a calm
  mirror rather than sub-pixel noise for TAA to fight. The glint lobe has a minimum width for the
  same reason.
* **Colour.** `body x (1 - m) + sky(reflected) x m + sun glint x m`, `m` = Schlick Fresnel
  (F0 0.02) x smoothness^2. The reflection is the analytic sky (`VxSkyRadiance`, no ray);
  terrain and objects are not reflected. The body is `albedo x accumulated irradiance`, darker
  at grazing view angles and lighter looking down - the stand-in for depth, which an opaque
  liquid does not have - and slightly brighter on ripples tilted towards the sun.
* **Shadows and lights.** The lighting buffer's rgb is used as it is, so cast shadows, ambient
  occlusion, point lights and emissive voxels all reach the water through the body term. For
  liquid pixels the lighting pass stores the plain **sun visibility** in the alpha channel
  instead of the sun specular (which composite does not use for liquids): `sunVisible` gates the
  glint and takes a quarter of the reflection away in cast shadows. It comes from the shadow
  rays alone, so no torch, spell or explosion light can switch a sun glint on, and there is no
  division by a small sun term at sunrise. `skyVisible`, which scales the reflection (a pool in
  a cave reflects no sky), is the rest of the lighting - luminance minus the sun's share, which
  is exact because both were accumulated from the same rays - relative to what an open surface
  receives (`mix(horizon, zenith, 0.8) x intensity`, the cosine-weighted mean of the sky
  gradient). Known limit: point lights and emissive light are part of that rest, so enclosed
  water lit by a strong local light still shows a sky-coloured reflection at grazing angles;
  telling them apart needs a fifth lighting channel.
* **Temporal passes.** Depth, normal and motion vectors of a liquid pixel are those of the static
  flat surface, so the lighting history, the checkerboard and the spatial filter behave as on any
  other material; only the composite output moves. The waves are slow and low-contrast enough
  for the TAA neighbourhood clamp to follow them: with and without TAA the image matches.
* **Cost** (UHD 620, 768x432, median of 120 frames, composite pass): 1.16 ms -> 1.32 ms with
  water on more than half of the screen, 1.30 ms -> 1.42 ms with about 40 %; a view without
  liquids is unchanged (1.91 ms -> 1.90 ms looking at grass).
* **Keep it small.** The Intel compiler builds this pass 8 and 16 pixels wide and the 16-wide
  variant has no registers to spare. A first version (three noise octaves, debug views after the
  shading) pushed its register allocation into a fallback schedule and made *every* pixel of the
  screen 12 % slower, liquid or not. Hence the single noise octave, the debug views as an early
  exit, and `ShadeLiquid` reducing its inputs before evaluating the waves. Check with
  `MESA_SHADER_CACHE_DISABLE=true INTEL_DEBUG=fs`: the composite shader's `SIMD16` line must say
  `scheduled with mode top-down`.

Lava takes the same path; its roughness (0.6) leaves almost no reflection, its emission is added
on top.

## 6. Physics

`physics::PhysicsWorld` wraps Jolt: bodies are compounds of boxes (from `BuildColliderBoxes`,
densities from materials, so mass and inertia are real) or static triangle meshes. It is
stepped with a fixed time step by `sim::Simulation` (7.4), as often as the engine loop or a
server asks; after every step the scene copies body poses into the entities' transforms
(`Scene::SyncFromPhysics`).

Every body carries 64 bits of user data, returned by ray casts and contact events. Bodies of
scene entities store `scene::ToUserData(entity)` (a tag in the upper half, the entity id in the
lower), terrain colliders store 0, so `Scene::EntityFromUserData` can tell them apart.

`physics::TerrainColliders` streams static meshes for terrain chunks: it snapshots a chunk and
its neighbours' border voxels on the main thread, cooks the mesh (`BuildChunkCollisionMesh` +
Jolt mesh shape) on a worker thread and installs the body on the main thread. It consumes
`World::TakeChangedChunks()`. A chunk gets a collider for one of two reasons:

* it is within `EngineConfig::colliderRadius` of a *focus point* (the camera: the ground the
  player and things at rest nearby stand on), dropped again at 1.5x that distance;
* a **body needs it**: every awake dynamic body requests the chunks its bounds touch, swept
  along its velocity (0.75 s ahead, at most two chunks). These are cooked first and kept for
  120 updates after the last request. There is no limit on the number of bodies and the camera
  plays no part, so debris thrown 50 m away lands on real ground.

An edited chunk keeps its old collider until the re-cooked one is installed (removal and
creation happen in the same call), so there is no frame without ground under a resting body.
Three rules make this hold under continuous editing (a held brush, a channelled spell):

* A finished mesh is installed whenever it is newer than what is installed, even if the chunk
  was edited again while it cooked; the newest state is queued behind it. A chunk edited every
  frame therefore has a collider that is one frame old, instead of never getting one.
* The mesh of a chunk depends on its neighbours only through the collidability of the touching
  voxel layers. Each entry remembers those bits: a neighbour edit that leaves them unchanged
  does not re-cook the chunk at all.
* Border faces covered by a solid neighbour are not meshed. If carving a neighbour *uncovers*
  such faces (a shaft dug through the bottom of the chunk above), the collider has a hole
  there, so the chunk is **not ready** (6.1) until its re-cook is installed: bodies touching it
  wait a few frames with their velocity kept rather than drop into solid rock.

A changed chunk that has no collider entry (far from the camera) wakes the bodies parked on it
(`ActivateInBox`), so they ask for colliders and fall if their support was blasted away.

### 6.1 Bodies never simulate without terrain

Terrain collision is not always there: the chunk may be outside the world window, still
loading, its mesh still cooking, or its collider dropped because nothing needed it. A body
simulated in such a place falls through the world. `TerrainColliders::GuardBodies()` - run by
`Update()` and again by the engine right before every `PhysicsWorld::Step()` - therefore checks
every dynamic body (`PhysicsWorld::GetDynamicBodies`):

* The chunks touched by its bounds, grown by one voxel and by what it travels in 0.05 s, must
  all be *ready*: loaded, and either with their collider built or needing none (air, liquid,
  rock enclosed by rock). If one is not, the body is **frozen**; when all are, it is released.
* `PhysicsWorld::SetFrozen` makes the Jolt body static (keeping its collision layer): it stays
  exactly where it is, nothing can wake it, other bodies rest on it. **The velocities are kept**
  and given back on release, so a body frozen in flight continues its flight - zeroing them
  would make thrown debris drop straight down whenever it outruns the cooking jobs. While
  frozen, `GetState` reports zero velocity, `active = false` and `frozen = true`.
* Wake-ups are deferred, not lost: `Activate`, `ActivateInBox`, `AddImpulse`, `SetVelocity` and
  `SetTransform(activate)` on a frozen body mark it as *wanting to move* (impulses and velocities
  change what it resumes with). A body that was awake when frozen wants to move from the start.
* A frozen body that wants to move keeps requesting its colliders, so it resumes within the few
  frames the cooking takes. One that was asleep - rubble the camera left behind - requests
  nothing: it is *parked*, costs one static body and no collider, and is released (still asleep)
  when a focus point brings the terrain back, or when something wakes it.
* At the edge of the world window a body simply stops: the chunk beyond is not loaded. It
  continues if the window moves over it. Nothing can leave the loaded world by falling.

Kinematic and static bodies are not guarded. Character controllers are not bodies: a game holds
its character while `Engine::TerrainReady(bounds)` is false - the box version, given the
capsule's bounds, applies the same chunks-touched-plus-one-voxel test as for bodies. (The point
version only looks at the chunk containing the point, which for feet standing on a chunk
boundary is the empty chunk *above* the ground.)

`Scene::DestroyBelow(windowBottom - EngineConfig::fallOutDepth)` runs once per frame and removes
what fell out of the world: dynamic bodies far below the window that were *on their way down*
(awake, or frozen by the guard while moving - `BodyState::wantsToMove`), and bodies with a
non-finite position. Whatever rests down there - static and kinematic objects, sleeping or
parked dynamic ones - was put or left there on purpose and is never touched, so flying 80 m up
in the editor deletes nothing. Removals are logged.

Small bodies (`Layer::Debris`) and **thin** ones (solid extent below
`SceneConfig::thinBodyVoxels` = 6 voxels along any axis, whatever the voxel count) use
continuous collision detection: a 0.2 m fragment at 20 m/s, or a 0.1 m slab falling flat from
3 m, moves further than its own thickness per step and would pass through the terrain mesh
between two steps.

## 7. Scene and destruction

### 7.1 Scene

`scene::Scene` is the list of things in the game besides the terrain: an `entt::registry` plus
the few systems that tie voxel models, rigid bodies, lights and lifetimes together. It is
constructed from references to a `ChunkStore`, `MaterialLibrary`, `PhysicsWorld` and
`ParticleSystem` - not from `Engine` - and imports nothing of the renderer, so it runs, and is
unit tested, without a GPU or a window. In a running game those four belong to a
`sim::Simulation` (7.4), which owns the scene as well.

Components are plain structs (`scene/Components.cppm`); games add their own to the same registry.

| component | content |
|---|---|
| `Transform` | `current` and `previous` `math::Transform`. `previous` is the pose drawn last frame (motion vectors). For voxel objects the position is the model's minimum corner and the scale is the voxel size. |
| `Name` | label for tools and logs |
| `VoxelBody` | `shared_ptr<VoxelModel>` + `visible` |
| `RigidBody` | `physics::BodyId` + motion type. The body belongs to the component. |
| `PointLight` | colour, intensity, radius, source radius, offset in the entity's frame |
| `Lifetime` | seconds left, optional exponential fade of a light on the same entity |
| `Debris` | age in seconds; marks a voxel object as rubble that `Scene::Update` cleans up (7.3) |

There are no tag components: an object without a `RigidBody` is visual only (the player figure).

Rules everything relies on:

* **Entity lifetime is real.** `Scene::Destroy` (or `registry.destroy`) removes the entity now.
  The scene listens to `on_destroy<RigidBody>`, so however the component goes away - destroy,
  `remove<RigidBody>`, `clear` - its body leaves the physics world; dropping `VoxelBody` drops the
  model reference and with the last one the chunk slots. Nothing is flagged dead and kept.
* **Stale ids are safe.** An `Entity` is an `entt::entity` (index + version). Every `Scene`
  function accepts `kNoEntity` and ids of destroyed entities and does nothing / returns the
  default (`TryGet<T>` returns `nullptr`). Store ids, not component pointers.
* **Bodies map back to entities** through the body's user data (section 6):
  `EntityFromUserData(hit.userData)` / `EntityFromBody(body)` return a living entity or `kNoEntity`.
* **Snapshot before you create.** `Damage` and the `Create*` functions add entities and
  components, which invalidates references and iterators of registry views. Code that does
  something to "everything near a point" collects ids first (`QueryBodies`) and then walks the
  list; `Explode` is the model. New pieces are not in the snapshot, so nothing is hit twice.
* **Destruction order.** A scene must be cleared before `PhysicsWorld::Shutdown()` and before the
  chunk store goes away; `~Scene` calls `Clear()` (the registry's own destructor fires no signals),
  and `sim::Simulation` declares the scene after the subsystems it references and clears it
  first in `Shutdown()`.
* **EnTT is an implementation detail for most code.** `scene::Entity`, `kNoEntity`, `TryGet<>`
  and views obtained from `Registry()` work without including EnTT; only code that spells
  `entt::` names includes `<entt/entt.hpp>` (in its global module fragment).

Frame protocol, driven by `Engine::Run` through the three phases of `sim::Simulation` (7.4):

```
sim.UpdateStreaming(camera, {camera})        once per frame:
    world.Update(camera)                         the window follows the camera
    terrain.Update({camera}, colliderRadius)     collider streaming, body guard (6.1)
scene.BeginFrame()                 previous = current, once per rendered frame
for each fixed step:
    app.OnFixedUpdate()
    sim.StepPhysics()                        one tick:
        terrain.GuardBodies()                    freeze / release, before anything new is simulated
        physics.Step(fixedDt)
        scene.SyncFromPhysics()                  body pose -> Transform::current
        ++tick
app.OnUpdate()                     game moves body-less entities (Scene::SetTransform)
particles.Update(frameDt)
GatherInstances(scene) / GatherLights(scene) -> FrameInput -> Renderer::RenderFrame
scene.SetViewer(camera)
sim.UpdateLifetimes(frameDt)                 once per frame, after the frame was drawn:
    scene.Update(frameDt)                        lifetimes, light fades, rubble clean-up
    scene.DestroyBelow(window bottom - fallOutDepth)
```

`vortex::GatherInstances` (in `vortex.engine`; the scene does not know graphics types) emits one
`graphics::InstanceDesc` per baked chunk of every visible voxel object (transform x chunk offset,
slot, tight bounds, palette); `vortex::GatherLights` one `graphics::PointLight` per light with a
positive intensity. Both only read the scene's public registry. All lights are entities: persistent
ones have no `Lifetime`, an explosion flash is `Transform + PointLight + Lifetime`.

### 7.2 Destruction

Carving the world returns the removed voxels (for particles) and is followed by a bounded
search for terrain that lost its support (`ExtractFloatingIslands`), which becomes dynamic
voxel objects.

`Scene::Damage(entity, centre, radius)` carves a sphere out of a voxel object:

1. The sphere is applied to a private copy of the volume (voxels of indestructible materials
   stay); the shared model is not touched. Removed voxels throw debris particles.
2. The rest is labelled (`LabelComponents`, a linear flood fill) and split (`SplitComponents`).
   Components below `SceneConfig::minPieceVoxels` become particles that move with the object.
3. The largest piece keeps the entity - id, name, game components - with a new model and a new
   body (the old model is released first, so its chunk slots are available to the pieces).
   Every other piece becomes a new dynamic entity. A static or kinematic object keeps its
   motion type only while it stays in one piece; once cut apart all of it is dynamic. Each piece stays
   exactly where its voxels were, gets the velocity the original body had at its centre and the
   same angular velocity; `Transform::previous` is re-based the same way, so motion vectors
   continue through the cut.
4. If nothing big enough is left the entity is destroyed.

`Engine::Explode` combines the two: carve the terrain, detach floating islands (created with
`VoxelObjectDesc::debris`, so the rules of 7.3 apply to them), damage every object whose bounds
the blast reaches, push bodies away, wake what rested on the removed ground, and add a flash.

### 7.3 Rubble

Destruction must not fill the world with tiny rigid bodies: each costs a body, a chunk slot
(37 KiB on CPU and GPU), an instance and BVH work in every ray. Three sizes, all in
`scene::SceneConfig` (reachable as `EngineConfig::scene`):

| size (solid voxels) | what it is | default |
|---|---|---|
| `< minPieceVoxels` | never an entity: particles on the spot (`Damage` fragments, floating terrain crumbs) | 16 |
| `< debrisBodyVoxels` | a *small* body: `Layer::Debris` (no collisions among each other), continuous collision detection; rubble of this size gets the `Debris` component | 48 |
| larger | an ordinary object that stays | |

`Debris` is added to small pieces that `Damage` splits off, to small dynamic objects created
with `VoxelObjectDesc::debris`, and to the damaged entity itself once what is left of it is
small and dynamic (`SceneConfig::debrisRemnants`, on by default: otherwise every destroyed crate
and every large terrain island leaves one fist-sized body behind for good). A small object
placed by the game or the editor is not rubble until it is damaged; static and kinematic objects
never are. To keep a particular remnant, remove its `Debris` component after `Damage`.
`Scene::Update` then removes rubble, oldest first, each piece leaving `debrisDespawnBursts` (3)
particle bursts in its material's colour:

* older than `debrisLifetime` (15 s);
* resting - asleep, or frozen by the terrain guard - farther than `debrisFarDistance` (25 m) from
  the viewer (`Scene::SetViewer`, the camera) and older than `debrisFarLifetime` (2 s);
* beyond `maxDebris` (192) pieces alive: the oldest go.

The cap is applied once per frame, so one large blast can exceed it until the end of its frame.

With `SceneConfig::markDebris = false` the scene adds the `Debris` component to nothing, so none
of the three rules ever fires (a component the game adds by hand is still honoured). That is the
scene of a session server (7.4): what exists there must not depend on a viewer, an age or a cap.

### 7.4 Simulation

`sim::Simulation` (`vortex.sim`, library `Vortex::Sim`) is the simulated half of a game without
anything that draws it: job system, material library, chunk store, `voxel::World`,
`physics::PhysicsWorld`, `physics::TerrainColliders`, the particle pool and the `scene::Scene`,
plus a tick counter. It is what `vortex::Engine` owns and renders (every `Engine` accessor such as
`World()`, `Physics()`, `Scene()` is a shortcut into it), what a session server runs and what a
network client will predict with (`docs/design/networking.md`).

* **Tick.** A tick is one fixed step of `SimulationConfig::fixedTimeStep` (1/60 s). `Tick()` is
  the number of ticks simulated so far, which is the number of the tick simulated next; it starts
  at 0 and only a physics phase advances it. Inside `Application::OnFixedUpdate`, `Engine::Tick()`
  is the tick being simulated.
* **`Step(focusPoints)`** is one whole tick in the order a server uses:
  `UpdateStreaming(focusPoints)` -> guard, physics step, pose sync -> `UpdateLifetimes(fixedDt)`
  -> `++tick`. `focusPoints` are the positions that need terrain collision around them (the
  players) and may be empty.
* **Phases.** `UpdateStreaming`, `StepPhysics` (which counts the tick) and `UpdateLifetimes(dt)`
  are public so that an owner can interleave its own work; the engine does (frame protocol in
  7.1: streaming and lifetimes once per frame, 0 to 4 physics phases). Game code that changes the
  simulation in a tick runs after the streaming phase and before the physics phase, so the body
  guard sees what it made.
* **World window.** `Step` and `UpdateStreaming(focusPoints)` advance loading but never move the
  window: a server places it once (`World().LoadBlocking(centre)`). Only
  `UpdateStreaming(windowFocus, focusPoints)` recentres it; the engine passes the camera. A world
  whose window was never placed gets it with the minimum corner at chunk (0, 0, 0).
* **Persistence.** `WorldConfig::saveDirectory` is passed to the world as given: an offline game
  or the map editor (`EngineConfig::world.saveDirectory`, the editor's "Save world" button) keeps
  its edits. A session world is memory only - it is `template + seed`, and a world that loaded
  saved chunks would not be the one the other side generated - so `ServerConfig` clears the
  directory, and a network client must leave it empty (stage 3).
* **Not stepped: presentation.** `Step` neither advances particles nor calls
  `Scene::BeginFrame()`; the owner that draws does both. Lifetimes run per tick in `Step` but per
  rendered frame, with the frame time, in the engine - nothing a server and a client must agree
  on may depend on `Lifetime` or on rubble clean-up.
* **Server configuration.** `sim::ServerConfig(config)` returns the configuration with
  `maxParticles = 0` (every effect is dropped silently), `scene.bakeModels = false` (models keep
  their voxels and bodies but hold no chunk slots: the store belongs to the terrain),
  `scene.markDebris = false` and `scene.debrisRemnants = false` (nothing is rubble).
* **Lifetime.** `Initialize(generator)` registers the default world materials, creates the world,
  starts physics and the terrain colliders; `Shutdown()` (also the destructor) destroys entities,
  colliders, world and bodies in that order. Outside of that span `World()` and `Terrain()` must
  not be called and the stepping functions do nothing. Main-thread only.

## 8. Editor

`vortex.editor` is an in-game editor: it runs inside the game's own frame, on the live world.
It has three parts, one module partition each:

| partition | content | needs |
|---|---|---|
| `:view` | cursor ray, the matrices handed to ImGuizmo, gizmo <-> `Transform`, sun angles, fly camera | math, `graphics::Camera` |
| `:tools` | surface picking, brush, primitives, placement, material lists and import palettes, model import, entity description / duplication | voxel, physics, scene |
| `Editor` | input handling, ImGui panels, the ImGuizmo gizmo, the actions behind the buttons | `vortex.engine`, ImGui, ImGuizmo |

Everything that is logic rather than widgets is a free function of `:view` / `:tools` and needs
neither ImGui nor a GPU; `Editor` only adds input and widgets. That split is what makes the
editor testable (section 9).

**Protocol.** `Engine::Run` opens the ImGui frame before `Application::OnUpdate` and closes it
after `Application::OnGui`. The game calls

```
OnUpdate:  editor.OnUpdate(engine, dt);                 // first
           if (!editor.CapturesInput()) { game input, game camera, cursor capture }
OnGui:     game windows;  editor.OnGui(engine);
```

`OnUpdate` handles the toggle key (F1), the editor camera, shortcuts and the active tool's mouse
action, so edits happen where game logic edits the world - before the particle update and the
scene gather. `OnGui` starts the ImGuizmo frame, then submits the menu bar, the panels, the gizmo
and the tool overlays. Closed, the editor makes no ImGui call and only watches the toggle key.
Without a window (headless) both run unchanged and nothing is drawn, so
`Shredborne --headless --editor` is a smoke test of every panel.

**Input**, in priority order: the game's pause menu (while it is open the game does not call
`editor.OnUpdate` at all, and releases a cursor the editor holds; section 11) -> ImGui (the
`Input` object already hides what ImGui wants) ->
camera look (right button, latched; the cursor is captured and ImGui is told to ignore the
mouse) -> gizmo (left button while `ImGuizmo::IsOver()`) -> active tool -> game. While
`CapturesInput()` is true the game must not react to mouse or keyboard, move the camera or touch
the cursor capture.

**Cursor ray and gizmo matrices.** Cursor position and viewport size come from ImGui
(`io.MousePos`, `io.DisplaySize`), the same numbers ImGuizmo gets through `SetRect`.
`CursorToRay` is the renderer's primary ray (`VxCameraRay` without jitter):
`normalize(Forward + ndc.x * tan(fovY/2) * aspect * Right - ndc.y * tan(fovY/2) * Up)` with
`ndc = cursor / size * 2 - 1`; cursor y grows downwards, so the top of the screen looks up.
ImGuizmo is **never** given the engine's projection (reverse-Z, infinite far plane, y flipped
for Vulkan). It gets `GizmoView` - a plain look-at in single precision world space, the space of
`scene::Transform` and the physics bodies - and `GizmoProjection`, a conventional right-handed,
y-up, depth 0..1 perspective with a finite far plane. `ProjectToCursor` with those two is the
exact inverse of `CursorToRay`, which is what puts the gizmo where the object is drawn and makes
it react where it is seen. The object matrix is `GizmoModelMatrix(transform, pivot)` (rotation
and the model centre, unit scale); the result goes back through `TransformFromGizmo` and
`Scene::SetTransform(entity, position, rotation, teleport = false)`, and a dragged body is held
still.

**Selection** is a `scene::Entity` id, validated on every use (`Editor::Selection`), so an entity
destroyed by anything - the editor, an explosion, a lifetime - simply stops being selected. A
change of selection cancels a gizmo drag.

**Tools.** *Select*: click = `TraceSurface` (nearest of the voxel ray and the physics ray; the
body's entity through `Scene::EntityFromBody`), gizmo, inspector, delete, duplicate (the copy
shares the model). *Brush*: `World::FillSphere` / `FillBox` at the terrain hit - id 0 to remove,
ignoring hardness; adding sits on the surface along the hit normal - repeated while the button is
held, at most once per `spacing x radius` of travel of the hit point (`StrokeBrushAlongRay`
measures from where the last applied ray meets the terrain after its own edit, so a resting cursor
applies once instead of drilling and the spacing does not depend on the viewing angle). *Spawn*: a box / sphere primitive on the
surface under the cursor or in front of the camera; `.vox` (`LoadVox` + `RegisterVoxPalette`) and
glTF (`ImportMesh` + `BuildImportPalette`) files by path, each with a palette of its own.

**Panels**: Tools, Scene (entity list), Inspector, Environment (sun as azimuth / elevation, sky,
fog), Render (`RenderSettings`, debug view, GPU pass timings), Profiler (timer history, counters),
World (statistics, save).

## 9. Testing

* `VortexCoreTests` - chunks, store, traversal vs. a reference DDA, BVH, world streaming /
  editing / persistence, generator determinism, colliders, connectivity, importers, particles;
  the HUD: font bitmaps and measuring, anchors, canvas quads, menu input, cursor capture.
* `VortexPhysicsTests` - bodies, shapes, ray casts, characters, terrain colliders; the body guard
  (a woken body the camera left behind, a body spawned where nothing is cooked, an unloaded
  chunk, the window edge, 60 bodies spread over the map, velocity kept through a freeze, parked
  bodies costing no collider, an edited chunk keeping its collider) and frozen bodies.
* `VortexSimTests` - links `Vortex::Sim` only (no Vulkan, GLFW or ImGui in the binary; checked
  at configure time). `sim::Simulation`: 600 ticks with the offline and with the server
  configuration on flat ground (crates land on the terrain collider and fall asleep, a timed
  light fades and expires on its tick, model chunk slots and particles only offline), 240 ticks
  on the built-in terrain, what counts a tick, lifetimes per tick vs per phase, fall-out, the
  window held by `Step` and moved only by the other streaming call, the body guard
  (inside `Step` and inside `StepPhysics`), fall-out only in the lifetime phase, the persistent
  offline world and the memory-only server world, `ServerConfig`, calls before `Initialize` and after `Shutdown`. The server scene on its own:
  unbaked models through creation and damage (also with a full chunk store), nothing marked as
  rubble, effects dropped by an empty particle pool.
* `VortexGraphicsTests` - SPIR-V reflection of the real shader binaries, C++ / GLSL struct
  layout parity (`VxHudQuad` included), the material surface-kind and HUD cell-grid constants
  (C++ vs. the text of `common.glsl`) and
  their packing, proxy cube winding, pipeline description validation. No Vulkan device needed.
* `VortexEngineTests` - the scene with its offline configuration on real chunk store / material /
  physics / particle objects (no device, no window): entity lifetime releasing bodies and chunk slots, shared models
  surviving damage, splitting (a cut beam is two entities whose voxels add up and which inherit
  motion), loose fragments, stale ids through every function, instance and light gathering,
  lifetimes, body <-> entity mapping; rubble (what is marked, crumbs that never become entities,
  lifetime, far rule, cap, fast small bodies not tunnelling) and `DestroyBelow`. The editor logic on the same objects plus a flat-generator
  world (no ImGui context either): the cursor ray (screen centre = `Camera::Forward()`, edges at
  the field of view, y not flipped), points of a cursor ray projecting back to the cursor through
  the matrices handed to ImGuizmo, gizmo matrix round trips, brush application and stroke spacing,
  primitive volumes, placement, sun angles, import palettes, `.vox` / glTF import and its failure
  paths, picking, entity description and duplication, selection going stale.
* `tools/cpu_render` renders a frame on the CPU with the shared shader code and writes a PNG:
  a visual check of world data, traversal and shading that needs no GPU.
* With a GPU: `Shredborne --headless --frames N --screenshot out.png --validation` exercises
  the Vulkan path and fails on validation errors. `--camera x,y,z[,yaw,pitch]` holds the camera
  at a chosen pose for the run (for example `--camera 60,2.5,-100,-2.23,-0.15`: the sea, towards
  the sun). The run ends with the GPU time of every pass: last frame and the median of the last
  120 frames - compare medians. Adding `--editor` opens the editor with every
  panel, cycles the tools each frame and runs its actions (select, spawn, duplicate, brush,
  import, delete), so the editor UI is exercised on every frame of the run. The log ends with
  `HUD: N quads drawn on the last frame`; `--pause-menu` starts with the pause menu open and
  `--no-hud` draws no HUD (screenshot comparison, GPU time of the hud pass).

## 10. Where the design leaves room

* **Level of detail**: the grid descriptor (`size`, `minChunk`, `voxelSize`) is a parameter of
  `VxTraceWorld`; clip-map cascades are additional grids with doubled voxel size.
* **Bigger worlds**: world positions are already `double` / integer voxels; only render space
  is `float`. Physics is single precision and would need Jolt's double precision build far
  from the origin.
* **Networking / dedicated server**: `Vortex::Core`, `Vortex::Physics` and `Vortex::Sim` have no
  GPU or window dependency, `sim::Simulation` runs the whole simulation on a numbered fixed tick
  (7.4), generation is a pure function of the seed (pinned bit for bit by golden tests) and world
  state is stored as chunk deltas. The plan is `docs/design/networking.md`.
* **Memory**: chunks are dense; palette-compressed or brick-sparse chunk payloads can replace
  `ids[]` behind `VX_VOXEL_ID` without touching traversal.

## 11. Game interface and cursor capture

Design and judged decisions: `docs/design/game-ui.md`. ImGui is for debugging and the editor
only; what a player sees is drawn by the HUD.

* **Data (Core, `vortex.core:hud`, no GPU, no GLFW).** `HudCanvas` - the quads of one frame
  (cap 8192, overflow dropped and reported), `Rect`, `Frame`, `Text` (built-in 5 x 9 bitmap font,
  ASCII 32..126, integer scale, `TextScale()` = max(1, height / 360)), `Crosshair`,
  `DimBackground`. `PlaceRect` anchors. `Menu` (layout, `HandleInput(MenuInput) -> MenuEvent`,
  `Draw`; the pointer counts only once it moves after `Open()`). `CursorCapture` (captured =
  !headless && focused && !editorOpen && !paused && !freeCursor, started only with the pointer
  over the window; Escape pauses; edges; `clicks` once the capturing click is over). All unit tested.
* **Engine.** `Engine::Hud()` is cleared and sized to the output at the start of every frame and
  handed to the renderer as `FrameInput::hud` / `hudDim`. `HudCursor()` converts the cursor to
  framebuffer pixels (HiDPI), `WindowFocused()`, `SetImGuiInput(bool)` (`UiLayer::SetInputEnabled`:
  `ImGuiConfigFlags_NoMouse | NoMouseCursorChange` and no input capture flags; it clears only
  the bits it added, so it does not fight the editor's own `NoMouse` during its right-button look).
* **Contracts.** `HudQuad` == `VxHudQuad` (layout test), `BitmapFont::kGlyphWidth/Height` ==
  `VX_HUD_CELLS_X/Y` (source-text test), bit = row * 5 + column. `FrameUniforms::settings.w` = HUD
  backdrop dim 0..255, applied by `present.frag` (a full-screen blended quad cost 0.15 ms on the
  UHD 620; this costs nothing). HUD colours are display values (after tone mapping).
* **Cursor (game side, `src/main.cpp`).** The game owns a `CursorCapture` and applies its edges
  with `Window::SetCursorCaptured` (GLFW_CURSOR_DISABLED + raw motion; Wayland: pointer lock +
  relative pointer, X11: grab + warp). Mouse look uses `Input::MouseDelta*` only, and only while
  captured and not on the frame the capture started; terrain clicks only with
  `CursorCapture::Result::clicks`. Escape opens the pause menu (it does not stop the
  simulation: sessions are networked); the menu owns input while open and the editor's look is
  ended with `Editor::ReleaseInput`.

