# VORTEX architecture

VORTEX is a voxel engine for large, fully editable worlds rendered by ray marching, in the
spirit of Teardown's renderer, with the scale and freedom of a Minecraft-like world. This
document is the reference for how the engine is put together and the contract its parts
share. If code and document disagree, fix one of them.

## 1. Goals and non-goals

| Goal | Consequence in the design |
|---|---|
| Huge, streaming, persistent world | Sparse chunk storage, a toroidal window that moves with the player, generation on worker threads, region files that store only what players changed. |
| Everything destructible | Voxels are the single source of truth for rendering, collision and gameplay queries; edits are local and cheap (one 37 KiB chunk re-upload). |
| Teardown-style image | Geometry is ray marched (no meshing); lighting is ray traced through the same data (soft sun shadows, ambient occlusion, emissive voxels) and accumulated over time. |
| Runs on integrated GPUs | Rendering at a fraction of the output resolution, one ray of each kind per pixel per frame, every feature can be switched off. |
| Testable without a GPU | Everything except `graphics` builds and runs headless; the ray traversal is one source file compiled for both the GPU and the CPU, so unit tests cover the shader logic. |

Not goals (yet): level of detail cascades for very long view distances, transparency /
refraction, skeletal animation, audio, networking. Section 9 lists what the design already
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
    core/     vortex.core      log, jobs, profiler, input state, paths, rng, args
    math/     vortex.math      AABB / frustum / transform, reverse-Z projection, simplex noise
    voxel/    vortex.voxel     chunks, chunk store, world streaming, generator, regions, models,
                               ray queries, colliders, connectivity, .vox / glTF import
    fx/       vortex.fx        particle simulation and effect presets
    physics/  vortex.physics   Jolt behind an engine interface, terrain collider streaming
    graphics/ vortex.graphics  Vulkan device, window, renderer, UI layer
    scene/    vortex.scene     ECS components and systems (EnTT)
    engine/   vortex.engine    application loop, destruction, glue
    editor/   vortex.editor    in-game tools
  tests/                 GoogleTest suites, none of which need a GPU
  tools/                 cpu_render: software renderer built on the shared shader code
```

Libraries and what they may depend on (a lower layer never imports a higher one):

```
Vortex::Core      core, math, voxel, fx        -> spdlog, glm, tinygltf      (no GPU, no window)
Vortex::Physics   physics                      -> Core, Jolt
Vortex::Graphics  graphics                     -> Core, Vulkan, VMA, vk-bootstrap, GLFW, ImGui
Vortex::Engine    scene, engine, editor        -> all of the above, EnTT, ImGuizmo
```

A dedicated server or a tool links `Vortex::Core` (+ `Vortex::Physics`) and never sees Vulkan.

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
  `vortex::voxel`, `vortex::fx`, `vortex::physics`, `vortex::graphics`, `vortex::scene`, `vortex::editor`.
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

`voxel::World` keeps a box of chunks around a focus point - the *window*, `windowSize` chunks
(powers of two). Each window cell is `Unloaded`, `Loading`, `Empty`, `Uniform` (one solid
material, no storage) or `Dense` (owns a slot). Cells are stored in a toroidal array
(`index = coord & (size - 1)`, `x + z*sx + y*sx*sz`), so moving the window re-targets one slab
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
entities; modifying one (destruction) first makes a private copy. `Revision()` changes on every
bake so dependants (physics shapes) know when to refresh.

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
| 4 | `u_Materials` | `VxMaterial[]`, 256 per palette | when `MaterialLibrary::Revision()` changes |
| 5 | `u_Instances` | `VxInstance[]` | every frame |
| 6 | `u_Bvh` | `VxBvhNode[]` | every frame |
| 7 | `u_DrawList` | instance indices to rasterise: front-facing boxes first, then boxes containing the camera | every frame |
| 8 | `u_Lights` | `VxLight[]` | every frame |
| 9 | `u_Particles` | `VxParticle[]` | every frame |
| 16+ | per-pass textures | declared by each shader | |

`u_Occ`, `u_Ids`, `u_Grid`, `u_Materials` are device-local and filled through a per-frame
staging buffer with `vkCmdCopyBuffer` at the start of the frame's command buffer (never
written while a frame in flight reads them). Chunk uploads are budgeted per frame
(`RenderSettings::maxChunkUploadsPerFrame`); the GPU copy of the grid only exposes a dense
chunk once its data has been uploaded (until then the entry is written as empty), so the GPU
never traverses a slot it has not received. Per-frame data (uniforms, instances, BVH, draw
list, lights, particles) lives in host-visible buffers owned by each frame-in-flight; they grow
(power of two) when needed and the old buffer goes to that frame's deletion queue. An
always-bound buffer is never zero-sized: empty arrays still bind a small valid buffer.

### 5.3 Render targets (R = render resolution = output * renderScale, O = output)

| target | format | size | content |
|---|---|---|---|
| gMaterial | `R32_UINT` | R | bits 0..23 global material index, 24..31 per-voxel variation; 0 = sky |
| gNormal[2] | `A2B10G10R10_UNORM_PACK32` | R | world normal * 0.5 + 0.5 (ping-pong: current / previous frame) |
| gVelocity | `R16G16_SFLOAT` | R | uv(current) - uv(previous) |
| depth[2] | `D32_SFLOAT` | R | reverse-Z, 0 = sky (ping-pong) |
| lighting[2] | `R16G16B16A16_SFLOAT` | R | rgb = accumulated diffuse irradiance, a = accumulated sun specular amount |
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
   history is short. Debug views are produced here.
4. **particles** - attachment hdr (load), depth[cur] as read-only test (no write). Instanced
   camera-facing quads from `u_Particles`, premultiplied blending (additive particles have
   opacity 0), sorted back to front on the CPU.
5. **taa** - attachment taa[cur]. Reprojects taa[prev] with the motion vector of the closest
   depth in a 3x3 neighbourhood, clamps it to the neighbourhood colour box, blends with weight
   `temporalParams.y`. Pass-through when TAA is off or the history is invalid.
6. **present** - attachment backbuffer (O). Bilinear upscale, exposure, tone mapping, sRGB
   curve, dither; then the UI (`UiLayer::Draw`) in the same rendering scope.

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
  passes 1..6 with GPU timestamps
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

Draw calls: full-screen passes `vkCmdDraw(3, 1, 0, 0)`; object boxes `vkCmdDraw(36, count, 0, firstInstance)`
where the vertex shader reads `u_DrawList.indices[gl_InstanceIndex]` (in Vulkan `gl_InstanceIndex`
includes `firstInstance`); particles `vkCmdDraw(6, particleCount, 0, 0)` (two triangles per
instance, corner derived from `gl_VertexIndex`).

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

A shader that includes the trace buffers but does not use all of them still declares them
(the reflected layout then contains them and the renderer binds them): the renderer binds
every binding a pipeline reflects, by name, and `DescriptorWriter` reports anything missing.
Depth targets are sampled as `sampler2D` (`.r`) while in `SHADER_READ_ONLY_OPTIMAL`; in the
particle pass the same image is the depth attachment in `DEPTH_READ_ONLY_OPTIMAL`.

G-buffer fragment outputs: `layout(location = 0) out uint o_Material; layout(location = 1) out vec4 o_Normal;
layout(location = 2) out vec2 o_Velocity;` plus `gl_FragDepth`. Lighting outputs:
`layout(location = 0) out vec4 o_Lighting; layout(location = 1) out float o_Count;`.

## 6. Physics

`physics::PhysicsWorld` wraps Jolt: bodies are compounds of boxes (from `BuildColliderBoxes`,
densities from materials, so mass and inertia are real) or static triangle meshes. It is
stepped with a fixed time step by the engine loop; render transforms are interpolated.

`physics::TerrainColliders` streams static meshes for terrain chunks near "focus points"
(players, moving bodies): it snapshots a chunk and its neighbours' border voxels on the main
thread, cooks the mesh (`BuildChunkCollisionMesh` + Jolt mesh shape) on a worker thread and
installs the body on the main thread. It consumes `World::TakeChangedChunks()`.

## 7. Destruction

Carving the world returns the removed voxels (for particles) and is followed by a bounded
search for terrain that lost its support (`ExtractFloatingIslands`), which becomes dynamic
objects. Damaging an object edits its volume, labels connected components
(`LabelComponents`, a linear flood fill) and splits it (`SplitComponents`): the largest piece
keeps the entity, the others become new entities inheriting the velocity at their position,
and pieces below a size threshold turn into particles instead of bodies.

## 8. Testing

* `VortexCoreTests` - chunks, store, traversal vs. a reference DDA, BVH, world streaming /
  editing / persistence, generator determinism, colliders, connectivity, importers, particles.
* `VortexPhysicsTests` - bodies, shapes, ray casts, characters, terrain colliders.
* `VortexGraphicsTests` - SPIR-V reflection of the real shader binaries, C++ / GLSL struct
  layout parity, proxy cube winding, pipeline description validation. No Vulkan device needed.
* `VortexEngineTests` - scene and destruction logic.
* `tools/cpu_render` renders a frame on the CPU with the shared shader code and writes a PNG:
  a visual check of world data, traversal and shading that needs no GPU.
* With a GPU: `Shredborne --headless --frames N --screenshot out.png --validation` exercises
  the Vulkan path and fails on validation errors.

## 9. Where the design leaves room

* **Level of detail**: the grid descriptor (`size`, `minChunk`, `voxelSize`) is a parameter of
  `VxTraceWorld`; clip-map cascades are additional grids with doubled voxel size.
* **Bigger worlds**: world positions are already `double` / integer voxels; only render space
  is `float`. Physics is single precision and would need Jolt's double precision build far
  from the origin.
* **Networking / dedicated server**: `Vortex::Core` + `Vortex::Physics` have no GPU or window
  dependency, the simulation runs on a fixed step, generation is a pure function of the seed
  and world state is stored as chunk deltas.
* **Memory**: chunks are dense; palette-compressed or brick-sparse chunk payloads can replace
  `ids[]` behind `VX_VOXEL_ID` without touching traversal.
