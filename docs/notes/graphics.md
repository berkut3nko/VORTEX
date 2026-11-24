# Graphics module notes (`vortex.graphics`)

[[../../../00 Docs Map|Docs map]] · [[../ARCHITECTURE|architecture]] · [[../design/fx-budget|fx budget]] · [[../design/perf|perf]]

Checked against engine `430fde0`.

Read `CLAUDE.md` and `docs/ARCHITECTURE.md` section 5 first; this file only adds what they do not say. Paths are
relative to `engine/`. `R.cpp` = `src/graphics/internal/Renderer.cpp`. Code is cited by file and symbol; search for
the symbol. The text before this rewrite, with its line numbers and measurements, is
`docs/history/notes/graphics-2026-11-18.md`.

## 1. Map

| file | owns |
|---|---|
| `src/graphics/Graphics.cppm` | primary module interface; re-exports the partitions below |
| `src/graphics/Window.cppm`, `internal/Window.cpp` | GLFW window, framebuffer size, resize / minimise flags (`ConsumeResized`, `IsMinimized`) |
| `internal/VmaImpl.cpp` | the single translation unit that compiles the VMA implementation |
| `src/graphics/Device.cppm`, `internal/Device.cpp` | instance, physical / logical device (features in `Device::Initialize`), one graphics queue, VMA allocator, surface, validation error counter |
| `src/graphics/Swapchain.cppm`, `internal/Swapchain.cpp` | swapchain images + views, format choice (`ChooseSurfaceFormat`), acquire / present |
| `src/graphics/Resources.cppm`, `internal/Resources.cpp` | `Buffer`, `Image`, `CreateBuffer` / `CreateImage2D` / `CreateSampler`, `TransitionImage`, `DeletionQueue` |
| `src/graphics/Shader.cppm` | reflection types, `PipelineDesc`, `Pipeline`, `DescriptorWriter` (declarations) |
| `internal/ShaderReflection.cpp` | `ReflectSpirv`, `LoadSpirvFile`: pure CPU SPIR-V parser (bindings, fragment outputs, block offsets) |
| `internal/Pipeline.cpp` | `Pipeline::Create` (layout from reflection + validation of outputs), `DescriptorWriter` |
| `src/graphics/Frame.cppm` | `FrameUniforms`, `FeatureFlags`, `DebugView`, `GpuLight`, `Camera`, `FrameInput` and its parts |
| `src/graphics/Renderer.cppm`, `R.cpp` | `RenderSettings`, `RenderStats`, the whole frame: targets, GPU voxel mirror (`VoxelGpu`), passes, timers, screenshot |
| `src/graphics/Ui.cppm`, `internal/Ui.cpp` | Dear ImGui context + backends; `UiLayer::Draw` is called inside the present pass; `SetInputEnabled` (play mode: no mouse for ImGui) |
| `shaders/hud.vert`, `shaders/hud.frag` | game HUD: instanced quads from `u_Hud` (binding 10), 5 x 9 cell bits, premultiplied, after tone mapping |
| `shaders/include/common.glsl` | `FrameUniforms` block, buffer binding table 0..10 (textures from 16), G-buffer packing, camera helpers, the `VX_GRID_*` accessors |
| `shaders/include/gbuffer_object.glsl` | shared body of the two object fragment shaders (`VX_OBJECT_FRONT` switch) |
| `shaders/shared/*.glsl`, `vx_cpp.h` | code compiled as GLSL and C++ (`trace`, `shading`, `cube`, `bounds`: the play-bounds curtain of [wp-5], tested as `PlayBounds.*`) |
| `shaders/*.vert, *.frag` | 13 entry points, 9 pipelines (`PipelineId`, `MakePipelineDescs` in `R.cpp`): gbuffer world / object front / object inside, lighting, composite, particles, taa, present, hud |
| `cmake/VortexShaders.cmake` | glslang build step, `vortex_deploy_assets` |
| `tests/graphics/ContractTests.cpp` | shader <-> C++ contract (bindings, layouts, outputs, proxy cube, reverse-Z) |
| `tests/graphics/ReflectionTests.cpp` | reflection parser on hand-built SPIR-V, corrupt input, `PipelineDesc` defaults |

## 2. Invariants

Ownership and lifetimes
- `Renderer` borrows `Device`, `Window`, `UiLayer` (raw pointers in its `Impl`). Shutdown order is renderer before
  device; `Renderer::Shutdown` waits idle and tolerates a dead device.
- Everything in `FrameInput` must outlive `RenderFrame` only; nothing is retained except the *address* of
  `materials`, compared (never dereferenced later) to detect a different source (`materialSource`), and the
  world's `World::InstanceId()` (`gridWorld`): a new World at the same address is still a different one.
- Two frames in flight (`kFramesInFlight`). A `FrameContext` may only be touched after its fence was waited (in
  `RenderFrame`). A buffer a previous frame may still read is never destroyed directly: use `DeferDestroy`; the queue
  is flushed when the same slot comes round again.
- Device-local buffers (`u_Occ` / `u_Ids` / `u_Grid` / `u_Materials`) are written only by `vkCmdCopyBuffer` between
  the two `TransferBarrier` calls in `SyncVoxelData`. Never map or write them from the CPU.
- Render targets are destroyed only with the device idle (`RecreateTargets`; its callers wait first).
- `Image::layout` is the CPU-side record of the GPU layout. It is only right if every recorded command buffer is
  executed; that is why a failed submit rebuilds all targets (`RecoverFailedSubmit`).
- Pipelines are replaced as a whole set (`PipelineSet`, `BuildPipelines`); a partial set is never installed.

Threading
- `Renderer`, `Swapchain`, `DescriptorWriter` have no locks: one thread calls all of them (the engine loop,
  `Engine::Run`). Thread-safe exceptions: the validation counter is atomic (`Device.cpp`, `validationErrors`) and
  `Pipeline.cpp` `ReportOnce` has its own mutex.
- `ChunkStore::TakeDirty()` is consumed inside `RenderFrame` (`SyncVoxelData`). The store is main thread only
  (`src/voxel/Store.cppm`) and has no locks: chunk edits must not run concurrently.

Order inside `RenderFrame` - keep it
1. `PrepareSurface`: minimised / resize / present change (`RenderSettings::present`) / dirty swapchain skip the
   frame; a render-scale change recreates the targets (device idle) and the frame goes on.
2. wait fence (timed alone: `RenderStats::fenceWaitMs`) -> flush deletions -> `CollectTimings` -> reset pools ->
   begin command buffer.
3. acquire the swapchain image *after* everything that can fail without side effects.
4. `SyncVoxelData` (materials, budgeted chunks, then grid) -> `ClearHistoryTargets` if needed -> `BuildFrameData` ->
   gbuffer, lighting, composite, particles, taa (offscreen command buffer) -> present (+ HUD, + UI) -> screenshot
   copy (present command buffer).
5. submit, present, blocking screenshot write, then swap `parity`, advance `frameSlot`, `++frameIndex`.
   An early `return false` leaves `parity`, `frameSlot`, `frameIndex`, `prevCamera` untouched.
- Between the swapchain acquire and the submit there must be no plain `return`: commands that were recorded but not
  executed leave `Image::layout`, the `uploaded` flags and the fence wrong. The only exits there go through
  `RecoverFailedSubmit`, which rebuilds the sync objects and the targets (history lost, `parity` = 0), re-queues every
  allocated chunk and marks the swapchain dirty. A new failure path after the acquire must call it too.
- The grid is written after the chunk copies of the same frame on purpose: an entry is exposed only when
  `uploaded[slot]` is set (`SyncVoxelData`). `gridStale` forces a rewrite whenever an `uploaded` flag flips even
  though `World::GridRevision()` did not change.
- Instances whose slot is not uploaded are dropped for the frame and re-queued (`BuildFrameData`).

The grid mirror [vd-1]
- `u_Grid` is `World::GridWords()`: cells (column-major), then column tops, content top, region tops; the shader
  macros `VX_GRID_COLUMN_TOP` / `VX_GRID_CONTENT_TOP` / `VX_GRID_REGION_TOP` in `common.glsl` index past the
  `gridSize.x * y * z` cells. A new tail section must be added in `World`, those macros and `Trace.cpp` together.
- Grid entries: 0 = empty, `0x80000000` = pending cell, bit 31 set = uniform or packed stand-in (passed through),
  otherwise `slot + 1`.
- Grid upload: only pages whose `World::GridPageRevisions()` entry is newer than the last copy, plus pages of cells
  hidden while their slot was not uploaded (`VoxelGpu::hiddenAt`, per slot the grid word that was replaced). A
  hidden cell shows `World::GridFallback(cell)`, what it showed before it became dense (pending, its packed /
  uniform stand-in, or air): never a hole where something was, never a dense word for a slot that is not there. A
  new world (`InstanceId`), a grown buffer or `InvalidateVoxelMirror` rewrites everything.
  `RenderStats::gridBytesUploaded` shows the bytes per frame.
- A pending cell (`0x80000000`, material 0) is written by `gbuffer_world.frag` as `VX_GBUFFER_SKY` with its depth
  and normal `VX_NORMAL_UNLIT` (alpha 1 in the 2-bit alpha of the normal target; surfaces write 0): lighting skips
  it, composite shows the sky colour, objects behind it are hidden. The spatial filter (`composite.frag`) and the
  lighting history (`lighting.frag`) skip taps with that alpha; they read the normal anyway, so the test costs no
  fetch (a separate `u_LightCount` fetch per tap was measured slower). Without it ground next to a pending cell
  averaged in its zero lighting (dark halos while streaming).
- The primary world ray of `gbuffer_world.frag` ends at the edge-fog radius R (perf.md G1, `VxEdgeFogTMax` in the
  shared `trace.glsl`, `voxel::EdgeFogTMax` on the CPU): `tMax` is where the ray has gone R metres *horizontally*
  (R = `u_Frame.jitter.w`, 0 when edge fog or fog is off or there is no world: then 1e9). Past R composite showed
  `SkyColor` anyway, so the picture is the same. Objects, shadow, AO and light rays keep their ranges: never apply
  it to `VxTraceScene` in `lighting.frag`.

Spaces, units, ranges
- Render space = world metres minus `World::WindowOrigin()` (double subtraction: `Rebase`, the camera and the lights
  in `BuildFrameData`). Particles arrive already rebased by the caller
  (`ParticleSystem::BuildRenderData(world.WindowOrigin(), ...)` in `Engine::Run`) and must use the same origin. With
  no usable world the origin is 0 and the voxel size 1.
- `prevViewProjNoJitter` is built from last frame's camera in *this* frame's render space; `prevLocalToWorld` is
  rebased with the current origin too. So a window shift needs no special handling for motion vectors.
- `fogParams.y` is render-space Y (`baseHeight - origin.y`). `temporalParams.w` is `time mod 3600`. `counts.w` is
  `frameIndex & 0x7FFFFFFF` (RNG seed and checkerboard phase).
- `cameraRight` / `cameraUp` are scaled by tan(half fov); normalise before using as directions (`particles.vert`).
- uv (0,0) is top-left; `VxCameraRay` (`common.glsl`) subtracts the jitter. Jitter is NDC units, Halton(2,3) over
  `kTaaJitterFrames` = 8, zero when TAA is off. The rasteriser gets the same jitter through
  `projection[2][0..1] = -jitter` (`ViewProjection`); the two signs belong together: change one without the other
  and object boxes no longer line up with the traced world.
- Depth: 0 = sky, never written as 0 by geometry (`clamp(..., 1e-7, 1)` in `gbuffer_world.frag` and
  `gbuffer_object.glsl`). Material word 0 = sky, `0x00FFFFFF` = debug colour in the normal target.
- `lightCount` is `R8_UNORM` holding frames/255, so `lightingHistory` is clamped to 1..255 on both sides
  (`BuildFrameData`, `lighting.frag`). Lighting samples are clamped to 64 (`VX_MAX_SAMPLE`).
- `renderScale` is clamped to 0.25..1, non-finite -> 1 (`DesiredRenderExtent`). Camera fov / near are sanitised
  (`SanitizeCamera`). Degenerate instance transforms are skipped.
- At most `kMaxLights` = 256 shadowed lights, the nearest to the camera; the shader has the same bound as a
  constant (`VX_MAX_LIGHTS`, `lighting.frag`). Change both.
- Effect lights (`PointLight::effect`, the blast flash) never reach `lighting.frag`: `BuildFrameData` merges them
  into at most `fx::kMaxEffectLights` = 8 and writes two records each after the shadowed ones (A position and
  radius, radiance; B clip box min in `positionRadius.xyz`, max in `colorSize.xyz`; render space, like the
  position). `FrameUniforms::effectLights` = (count, first record = `counts.z`, 0, 0); `composite.frag` reads only
  those records. A light lost to a truncated upload lowers the count, never splits a record pair.
- Draw list = front-drawn indices then inside-drawn ones; the inside draw uses `firstInstance = frontCount`. A
  truncated BVH is published as 0 nodes.
- Slot reuse: a freed slot found in the pending queue clears `uploaded` and marks the grid stale. `ChunkStore::Free`
  does not mark anything dirty; `Adopt` / `Alloc` mark the reused slot dirty (`src/voxel/internal/Store.cpp`), so it
  is uploaded again. Until that upload happens `uploaded` is still 1 from the previous owner: with an upload backlog a
  reused slot can be traced with the old chunk's voxels for a few frames (read from the code, not observed). Keep this
  in mind before lowering `maxChunkUploadsPerFrame`.

## 3. Traps

- **SPIR-V `discard` needs `shaderDemoteToHelperInvocation`.** `gbuffer_world.frag` discards a missed ray; the
  device enables the feature in `Device::Initialize` (`features13`). A new shader that discards relies on it; do not
  drop the feature (CLAUDE.md lessons).
- **No camera-cut detection exists.** ARCHITECTURE 5.4 says history is invalidated after a camera cut; the code only
  clears `historyValid` in `RecreateTargets` and `Initialize`. A teleport relies on the per-tap depth / normal
  rejection in `lighting.frag` and the colour clamp in `taa.frag`.
- The `particles` comment in `Frame.cppm` mentions `Renderer::RenderOrigin`; no such function exists. The origin is
  `World::WindowOrigin()`.
- `jitter.zw` is the horizontal edge fog (start, end) in metres, 0 = off (`RenderSettings::edgeFog`, 0.85 R .. R
  with R = `World::ViewRadius()`), read by `composite.frag`, which mixes surfaces towards `SkyColor(rd)` (the sky +
  atmospheric fog a sky pixel gets) by horizontal distance only; sky pixels are unchanged [vd-1]. The previous
  frame's jitter it used to hold was never read.
- `playBounds` (the last member, [wp-5]): a session's play bounds in render space (min x, min z, max x, max z), zero
  when there are none (offline, or `RenderSettings::playBounds` off), recomputed every frame from
  `FrameInput::playBounds` (world metres, `Engine::SetPlayBounds`) because a moving window moves the render origin.
  `composite.frag` draws the curtain (`shared/bounds.glsl`, `VxPlayBoundsCurtain`) after the edge fog, and over the sky
  at `VX_SKY_DISTANCE`; never in the debug views. With the camera inside the bounds (a session, always) it tests only
  the wall the ray leaves through (the same branch for every pixel): four wall tests per pixel cost +0.17 ms on the
  UHD 620 at a wall 10 m ahead, the one-wall path +0.09-0.12 ms (budget 0.15 ms, L6).
- Spare fields, written but read by no shader: `settings.z` (front count). `outputSize` is read by `hud.vert`
  (pixels -> NDC) and `settings.w` by `present.frag` (HUD backdrop dim, 0..255). `FrameInput::deltaTime` is not read
  by the renderer. Reuse spare fields before adding new ones.
- `FrameUniforms` may only contain mat4 / vec4 / ivec4 (comment and `static_assert` in `Frame.cppm`); a vec3 or float
  breaks the "C++ layout == std140" assumption silently on the C++ side (the contract test catches the offsets).
- A shader that declares a buffer it does not use still gets it in the layout, and `BindPipeline` refuses to draw the
  pass if any reflected binding has no provider: the pass goes missing with one log line (`declares binding ... which
  the renderer does not provide`), not a crash. The line is printed once per pipeline and binding until
  `ReloadShaders` clears the list. New buffer names must be added to `FindBuffer`; new textures to the pass's
  `TextureBinding` array.
- Checkerboard is 8x8 tiles, keyed on `counts.w` parity (`lighting.frag`). A skipped pixel copies the reprojected
  history and does not raise its count. It needs count >= 6 first. Do not go per-pixel (CLAUDE.md lessons).
- The lighting alpha channel has two meanings: sun specular amount, or plain sun visibility for liquids
  (`lighting.frag`), read back in `composite.frag`. The spatial filter blends alpha across a water / shore edge only
  if depth and normal match; do not relax those weights without looking at shores.
- `composite.frag`: debug views are an early exit *before* the shading and `ShadeLiquid` is kept small on purpose
  (Intel SIMD16 register pressure, ARCHITECTURE 5.7). Adding work here can slow every pixel.
- `VX_WATER_CHUNK_VOXELS` (`composite.frag`) duplicates `VX_CHUNK_SIZE`; a contract test compares them.
- Metals keep 65 % diffuse (`composite.frag`): deliberate until reflections exist.
- Sky pixels have no velocity (cleared to 0); `taa.frag` derives sky motion from camera rotation. TAA uses the
  velocity of the closest depth in 3x3 and an upper weight cap of 0.98.
- `gbuffer_object_front` promises `depth_less` and enforces it with `min(depth, gl_FragCoord.z)`
  (`gbuffer_object.glsl`). The inside variant must not get the qualifier. The inside / front split uses a margin of
  `2 * near + 0.05` m (`insideMargin` in `BuildFrameData`); shrinking it makes boxes vanish when the camera touches
  them.
- Particles: lighting is per vertex (sun wrap + mean sky, no shadows); additive = opacity 0 with emission; depth test
  only, no write, no soft fade; sorted on the CPU by the fx module. Zero particles still writes both timestamps but
  skips the rendering scope (`PassParticles`).
- Present: data debug views (normal, depth, steps, motion, material) bypass tone mapping (`present.frag`). The
  backbuffer is UNORM and the shader applies the sRGB curve; never pick an sRGB swapchain format
  (`ChooseSurfaceFormat` accepts only two UNORM formats).
- Pass timers: results belong to the frame this slot submitted two frames ago (`CollectTimings`). Timestamps are
  taken at `BOTTOM_OF_PIPE`; the `SyncVoxelData` copies (chunks, grid pages, materials) are the `upload` interval
  (`Pass::Upload`, the last of the enum, timed at the start of the command buffer); `ClearHistoryTargets` is outside
  every interval. `present`,
  `hud` and `imgui` are three intervals *inside* the one backbuffer rendering scope (`PassPresent`); a new draw there
  gets its own `Pass` or goes into one of them, never between them. `kPassNames` must stay in `Pass` enum order; the
  engine finds the median by the profiler name `"GPU: <name>"` (`Engine::Run`, exit statistics).
- Chunk uploads per frame = min(`maxChunkUploadsPerFrame`, what fits in the staging buffer after the material and
  grid bytes). Staging is 8 MiB (`kStagingBytes`, about 227 chunks of 36 936 bytes) and only grows to
  `tables + one chunk`, so raising the budget above ~200 does nothing by itself.
- Descriptor pool per frame (`CreateFrames`): 32 sets, 32 UBO, 160 SSBO, 96 samplers. Each of the nine pipelines binds
  one set per frame. A new pass fits; a loop that binds per object does not.
- A frame is two submit batches in one `vkQueueSubmit2` (perf.md P1), each with its own command buffer of the
  frame slot (`FrameContext::offscreenCmd`, `presentCmd`; `FrameContext::cmd` is the one being recorded): uploads,
  gbuffer, lighting, composite, particles and TAA wait for nothing; present, HUD, ImGui and the screenshot copy wait
  for the image-acquired semaphore at `COLOR_ATTACHMENT_OUTPUT`, signal `renderFinished` and, with the first batch,
  the fence. One batch would not do: a semaphore wait holds back every command of its batch at the wait's stage, so
  the offscreen passes' colour writes would wait for the image too. Barriers in the second batch see the first
  batch's writes (submission order). The swapchain image's first transition is `TransitionAcquiredImage` (source
  stage `COLOR_ATTACHMENT_OUTPUT`, chained to that wait); a transition with a `NONE` source stage would run before
  the acquire. A failure between the two command buffers goes through `RecoverFailedSubmit` like any after the
  acquire. The present mode is chosen by `ChoosePresentMode` from the
  surface's list and handed to vk-bootstrap alone (it falls back to FIFO silently); the `Swapchain:` line prints the
  mode that was built.
- Resize / present change skip that frame (`PrepareSurface` returns false); a render-scale change does not. A pending
  screenshot stays requested until a frame is actually rendered, but the engine asks for it only once, on the last
  frame (`Engine::Run`), and does not check the result of `RenderFrame`.
- HUD: one `vkCmdDraw(6, hudCount)` with no vertex buffer; nothing is drawn (both timestamps still written) when the
  canvas is empty. Quad colours are display values written into the UNORM backbuffer after the sRGB curve: do not
  tone map them. The backdrop of a menu is dimmed in `present.frag`, not with a full-screen quad (measured too slow on
  the UHD 620: CLAUDE.md lessons). `UiLayer::Draw` returns early when ImGui produced no vertices (play mode, overlay
  hidden).
- If the swapchain format changes on recreate, the renderer rebuilds its pipelines (`RecreateSurface`) but `UiLayer`
  was initialised once with the old format (`UiLayer::Initialize`, called from `Engine::Initialize`). Weak spot,
  never exercised headless. If that rebuild fails `pipelines` is null and every later frame is skipped with no
  further log.
- Screenshot readback (`WriteScreenshot`) assumes 4 bytes per pixel and swaps B/R only for the two BGRA formats; it
  blocks on the frame fence.
- `TransitionImage` (`Resources.cpp`) supports a fixed list of layouts; an unknown one logs an error and uses a full
  barrier. A depth image sampled by a shader goes to `SHADER_READ_ONLY_OPTIMAL`; only the particle pass uses
  `DEPTH_READ_ONLY_OPTIMAL` (needs `separateDepthStencilLayouts`, enabled in `Device::Initialize`).
- `Pipeline::Create` only knows the colour formats in `kColorFormats` (`Pipeline.cpp`). A new target format must be
  added there with its base type or the pipeline is refused.
- `ReflectSpirv` rejects unsized descriptor arrays and resources without set / binding decorations; `Pipeline::Create`
  refuses bindings outside set 0, unnamed bindings and two stages that disagree on a binding. Only uniform / storage
  blocks and combined image samplers can be bound by `DescriptorWriter` (`StorageImage` is reflected but has no
  writer method).
- `Pipeline::Create` also checks every fragment output against the attachment list (location, base type;
  `ValidateOutputs`); an attachment no output writes is only a warning, so read the warnings after adding a target.
- Per-frame storage buffers grow through `EnsureCapacity`: power of two, contents not kept. If growing fails
  `Upload<T>` writes only what fits and returns the smaller count; callers must use the returned count (as the BVH
  does: a short tree is published as 0 nodes), never the size of the source.
- Staging layout is `[materials][grid][chunks...]` and the grid is written through a `uint32_t*` at offset
  `materialBytes` (`SyncVoxelData`): that only stays aligned while `sizeof(GpuMaterial)` is a multiple of 4.
- Shaders are compiled with `--target-env vulkan1.3`, no optimiser (`ENABLE_OPT OFF` when glslang is built from
  source). A new include directory must be added to the `GLOB_RECURSE` in `VortexShaders.cmake` or edits there will
  not trigger a rebuild. Entry points are globbed from `shaders/*.{vert,frag,comp}` only.

## 4. Recipes

A. Add a field to `FrameUniforms`
1. Prefer a spare component (see traps). Otherwise append a vec4 / ivec4 at the same position in `Frame.cppm`
   (`FrameUniforms`) and `common.glsl` (the `FrameUniforms` block), and fix the `static_assert` size.
2. Fill it in `BuildFrameData`.
3. `tests/graphics/ContractTests.cpp` `LayoutContract.FrameUniformsMatchTheGlslBlock`: member count (26), one
   `VX_EXPECT_FRAME_MEMBER` line, and the "last member is `effectLights`" check if you append after it.
4. Update the table comment in `common.glsl` and ARCHITECTURE 5 if the meaning is a contract.

B. Add a feature toggle
1. `RenderSettings` (`Renderer.cppm`), `FeatureFlags` (`Frame.cppm`), `VX_FEATURE_*` (`common.glsl`), the
   `features |=` list in `BuildFrameData`. Bits must match by hand: no test compares them. The same holds for
   `DebugView` (`Frame.cppm`) and `VX_DEBUG_*` (`common.glsl`); a new debug view that shows data rather than light
   must also be added to the list in `present.frag` or it gets tone mapped.
2. Branch in the shader with `VxFeature(...)`; measure the off path too.

C. Add a texture input to an existing pass
1. Declare `layout(set = 0, binding = 16+n) uniform sampler2D u_Name;` in the fragment shader.
2. Transition the image to `SHADER_READ_ONLY_OPTIMAL` in the pass function and add a `TextureBinding` with the same
   name (as `PassLighting` does). Check that no later pass expects the old layout untracked.
3. Add `{binding, "u_Name"}` to that pipeline in `Pipelines()` (ContractTests.cpp) and to ARCHITECTURE 5.6.

D. Add a scene buffer (binding 11..15)
1. Block in `common.glsl` behind a `VX_NEED_*` define; mirrored C++ struct with a `static_assert` on size.
2. Per-frame data: a `Buffer` in `FrameContext`, create in `CreateFrames`, destroy in `DestroyFrames`, fill with
   `Upload<T>` in `BuildFrameData`, add to `UpdateMemoryStats`. Never bind a zero-sized buffer: create it with a
   minimum size and pass a count in `FrameUniforms`.
3. Name in `FindBuffer`.
4. Tests: `kBuffers`, each pipeline's buffer list in `Pipelines()`, and a `LayoutContract` test like
   `LightMatchesVxLight`. `EveryBufferOfTheTableIsUsedSomewhere` fails if no shader declares it.

E. Add a render target or a full-screen pass
1. Target: member in `RenderTargets` *and* in its `ForEach`, create in `RecreateTargets`, include in the `ok` check.
   Ping-pong pairs that are sampled before first written also go in `ClearHistoryTargets`. A format that is not 4
   bytes per pixel needs a case in `BytesPerPixel` for the memory stat.
2. Pass: `Pass` enum + `kPassNames` (same order), `PipelineId` + a desc in `MakePipelineDescs`, a `PassX` function
   (BeginPass, transitions, `BeginRendering`, `BindPipeline`, draw, EndPass), call it in `RenderFrame` in order.
   Format must be in `kColorFormats`.
3. Shader file in `shaders/`; add it to `kShaderFiles` and `Pipelines()` in ContractTests
   (`EveryPipelineShaderIsListed`, `SingleTargetPassesWriteOneFloatColour`).
4. Update ARCHITECTURE 5.3 / 5.4 / 5.6 and measure the pass on the UHD 620 (the headless log prints it).

F. Change temporal accumulation or the checkerboard
1. Constants live at the top of `lighting.frag` and `composite.frag`; history length comes from
   `RenderSettings::lightingHistory`. Keep `count` in 0..255 and the R8 encoding.
2. Verify with debug view `Lighting` (`DebugView::Lighting`) and with `checkerboardLighting` on and off; compare a
   still camera at frame 200 against a moving one (`--camera`), looking for trails and noise at silhouettes. No unit
   test covers image quality.

## 5. Verifying a change here

- Gates G1 (warning-free build) and G2 (`ctest`). The suites of this module are `ShaderContract`, `LayoutContract`,
  `ProxyCube`, `ReverseZ`, `ShaderReflection`, `SpirvFile`, `Pipeline` (`VortexGraphicsTests`, no GPU; they read the
  compiled `.spv` from `VORTEX_TEST_SHADER_DIR`, so a shader that does not compile fails the build first).
- Gates G3 and G4 (both headless runs with `--validation`; exit code 2 = validation errors, counted by
  `Device::ValidationErrorCount`, or no validation layer: `graphics::DecideValidation` fails the start,
  `Device::ValidationMissing`, `Validation.ARequestWithoutTheLayerFailsStartUp`). Search the log for `Vulkan:`, `Renderer:` and `Pipeline '` error lines; a pass
  that silently went missing shows up as `declares binding ... which the renderer does not provide` or `could not grow buffer`.
- Start-up line `Renderer: headless mode, output WxH, render WxH, N chunk slots, X MiB GPU memory` (`Initialize`)
  confirms sizes; `Screenshot saved: file (WxH)` confirms the readback.
- End-of-run lines `GPU <pass> a ms (median b ms over n frames)` for gbuffer, lighting, composite, particles, taa,
  present, hud, imgui (always 0 headless: no ImGui backend), upload, then `HUD: N quads drawn on the last frame` (G3 needs
  N > 0): compare medians before and after. Reference values are in ROADMAP "Performance". All zero means timestamps
  are unsupported or too few frames ran.
- The `Perf:` line (every run, windowed included, after the lines above when `--frames` is given): whole-run
  percentiles of the CPU frame, the world and simulation sections, the fence wait and the per-pass GPU p50
  (`vortex::PerfSummary`, filled by `Engine::Run`; a pass reading is counted once per `RenderStats::timingSerial`).
  The fence wait is its own profiler timer `"Renderer: fence wait"` (`RenderStats::fenceWaitMs`); the
  `"Renderer::RenderFrame"` scope starts after it, so it is CPU work only.
- Measure GPU passes as CLAUDE.md's measuring rules say: timings while the world streams are inflated (the generation
  threads take the UHD 620's shared power budget); compare runs with a fixed `--camera` once the log says
  `World: window filled`.
- `store a/b` in the `World:` line and `chunkUploadsPending` in `RenderStats` show an upload backlog; holes in terrain
  in an early screenshot that fill later are the upload budget, not a tracing bug.
- Look at the screenshot (part of G3): a black frame usually means a pass was skipped (targets are cleared to 0);
  noisy lighting that never settles means history is being rejected; water must look the same with TAA on and off.
- Windowed paths (swapchain recreate, vsync toggle, UI, F5 reload) cannot be checked headless: say so
  (`docs/owner-checks.md` in the game repository holds the owner's results).
