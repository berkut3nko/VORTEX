# Graphics module notes (`vortex.graphics`)
Read `CLAUDE.md` and `docs/ARCHITECTURE.md` section 5 first; this file only adds what they do not say.
Paths are relative to `engine/`. `R.cpp` = `src/graphics/internal/Renderer.cpp`. Line numbers are as
of commit ac17bcf (all re-checked against the code at 7a95975); search for the symbol if they have moved.

## 1. Map

| file | owns |
|---|---|
| `src/graphics/Graphics.cppm` | primary module interface; re-exports the partitions below |
| `src/graphics/Window.cppm`, `internal/Window.cpp` | GLFW window, framebuffer size, resize / minimise flags (`ConsumeResized`, `IsMinimized`) |
| `internal/VmaImpl.cpp` | the single translation unit that compiles the VMA implementation |
| `src/graphics/Device.cppm`, `internal/Device.cpp` | instance, physical/logical device, one graphics queue, VMA allocator, surface, validation error counter |
| `src/graphics/Swapchain.cppm`, `internal/Swapchain.cpp` | swapchain images + views, format choice, acquire / present |
| `src/graphics/Resources.cppm`, `internal/Resources.cpp` | `Buffer`, `Image`, `CreateBuffer/CreateImage2D/CreateSampler`, `TransitionImage`, `DeletionQueue` |
| `src/graphics/Shader.cppm` | reflection types, `PipelineDesc`, `Pipeline`, `DescriptorWriter` (declarations) |
| `internal/ShaderReflection.cpp` | `ReflectSpirv`, `LoadSpirvFile`: pure CPU SPIR-V parser (bindings, fragment outputs, block offsets) |
| `internal/Pipeline.cpp` | `Pipeline::Create` (layout from reflection + validation of outputs), `DescriptorWriter` |
| `src/graphics/Frame.cppm` | `FrameUniforms`, `FeatureFlags`, `DebugView`, `GpuLight`, `Camera`, `FrameInput` and its parts |
| `src/graphics/Renderer.cppm`, `R.cpp` | `RenderSettings`, `RenderStats`, the whole frame: targets, GPU voxel mirror, passes, timers, screenshot |
| `src/graphics/Ui.cppm`, `internal/Ui.cpp` | Dear ImGui context + backends; `Draw` is called inside the present pass; `SetInputEnabled` (play mode: no mouse for ImGui) |
| `shaders/hud.vert`, `shaders/hud.frag` | game HUD: instanced quads from `u_Hud` (binding 10), 5 x 9 cell bits, premultiplied, after tone mapping |
| `shaders/include/common.glsl` | `FrameUniforms` block, buffer binding table 0..9, G-buffer packing, camera helpers |
| `shaders/include/gbuffer_object.glsl` | shared body of the two object fragment shaders (`VX_OBJECT_FRONT` switch) |
| `shaders/shared/*.glsl`, `vx_cpp.h` | code compiled as GLSL and C++ (`trace`, `shading`, `cube`) |
| `shaders/*.vert, *.frag` | 11 entry points, 8 pipelines (`MakePipelineDescs`, R.cpp:204) |
| `cmake/VortexShaders.cmake` | glslang build step, `vortex_deploy_assets` |
| `tests/graphics/ContractTests.cpp` | shader <-> C++ contract (bindings, layouts, outputs, proxy cube, reverse-Z) |
| `tests/graphics/ReflectionTests.cpp` | reflection parser on hand-built SPIR-V, corrupt input, `PipelineDesc` defaults |

## 2. Invariants

Ownership and lifetimes
- `Renderer` borrows `Device`, `Window`, `UiLayer` (raw pointers, R.cpp:360). Shutdown order is renderer
  before device; `Renderer::Shutdown` waits idle and tolerates a dead device (R.cpp:1735).
- Everything in `FrameInput` must outlive `RenderFrame` only; nothing is retained except the *addresses* of
  `world` and `materials`, compared (never dereferenced later) to detect a different source
  (`gridWorld`, `materialSource`, R.cpp:334/340).
- Two frames in flight (`kFramesInFlight`, R.cpp:45). A `FrameContext` (R.cpp:272) may only be touched after
  its fence was waited (R.cpp:1511). A buffer a previous frame may still read is never destroyed directly:
  use `DeferDestroy` (R.cpp:426); the queue is flushed when the same slot comes round again (R.cpp:1516).
- Device-local buffers (`u_Occ/u_Ids/u_Grid/u_Materials`) are written only by `vkCmdCopyBuffer` between the
  two `TransferBarrier` calls in `SyncVoxelData` (R.cpp:1011-1029). Never map or write them from the CPU.
- Render targets are destroyed only with the device idle (`RecreateTargets`, callers wait first: R.cpp:761, 793, 1483).
- `Image::layout` is the CPU-side record of the GPU layout. It is only right if every recorded command
  buffer is executed; that is why a failed submit rebuilds all targets (`RecoverFailedSubmit`, R.cpp:1482).
- Pipelines are replaced as a whole set (`PipelineSet`); a partial set is never installed (R.cpp:705-729).

Threading
- `Renderer`, `Swapchain`, `DescriptorWriter` have no locks: one thread calls all of them (the engine loop,
  `src/engine/internal/Engine.cpp:272`). Thread-safe exceptions: the validation counter is atomic
  (Device.cpp:36) and `Pipeline.cpp` `ReportOnce` has its own mutex (Pipeline.cpp:360).
- `ChunkStore::TakeDirty()` is consumed inside `RenderFrame` (R.cpp:896). The store is documented as
  main thread only (`src/voxel/Store.cppm:20`) and has no locks: chunk edits must not run concurrently.

Order inside `RenderFrame` (R.cpp:1497) - keep it
1. `PrepareSurface` (R.cpp:781): minimised / resize / vsync change / dirty swapchain skip the frame; a
   render-scale change recreates the targets (device idle) and the frame goes on.
2. wait fence -> flush deletions -> `CollectTimings` -> reset pools -> begin command buffer.
3. acquire the swapchain image *after* everything that can fail without side effects (comment R.cpp:1533).
4. `SyncVoxelData` (materials, budgeted chunks, then grid) -> `ClearHistoryTargets` if needed ->
   `BuildFrameData` -> gbuffer, lighting, composite, particles, taa, present (+ UI) -> screenshot copy.
5. submit, present, blocking screenshot write, then swap `parity`, advance `frameSlot`, `++frameIndex`.
   An early `return false` leaves `parity`, `frameSlot`, `frameIndex`, `prevCamera` untouched.
- Between the swapchain acquire (R.cpp:1536) and the submit there must be no plain `return`: commands that
  were recorded but not executed leave `Image::layout`, the `uploaded` flags and the fence wrong. The only
  exits there go through `RecoverFailedSubmit` (R.cpp:1482), which rebuilds the sync objects and the targets
  (history lost, `parity` = 0), re-queues every allocated chunk and marks the swapchain dirty. A new failure
  path after the acquire must call it too.
- The grid is written after the chunk copies of the same frame on purpose: an entry is exposed only when
  `uploaded[slot]` is set, otherwise written as 0 (R.cpp:983-993). `gridStale` forces a rewrite whenever an
  `uploaded` flag flips even though `World::GridRevision()` did not change.
- Instances whose slot is not uploaded are dropped for the frame and re-queued (R.cpp:1101).

Spaces, units, ranges
- Render space = world metres minus `World::WindowOrigin()` (double subtraction, `Rebase` R.cpp:181,
  camera R.cpp:1053, lights R.cpp:1176). Particles arrive already rebased by the caller
  (`ParticleSystem::BuildRenderData(world.WindowOrigin(), ...)`, Engine.cpp:253) and must use the same origin.
  With no usable world the origin is 0 and the voxel size 1 (R.cpp:1045).
- `prevViewProjNoJitter` is built from last frame's camera in *this* frame's render space (R.cpp:1070);
  `prevLocalToWorld` is rebased with the current origin too (R.cpp:1115). So a window shift needs no
  special handling for motion vectors.
- `fogParams.y` is render-space Y (`baseHeight - origin.y`, R.cpp:1088). `temporalParams.w` is
  `time mod 3600`. `counts.w` is `frameIndex & 0x7FFFFFFF` (RNG seed and checkerboard phase).
- `cameraRight/cameraUp` are scaled by tan(half fov) (R.cpp:1072); normalise before using as directions
  (`particles.vert:31`).
- uv (0,0) is top-left; `VxCameraRay` subtracts the jitter (`common.glsl:194`). Jitter is NDC units,
  Halton(2,3) over `kTaaJitterFrames` = 8, zero when TAA is off (R.cpp:1060). The rasteriser gets the same
  jitter through `projection[2][0..1] = -jitter` (R.cpp:199); the two signs belong together: change one
  without the other and object boxes no longer line up with the traced world.
- Depth: 0 = sky, never written as 0 by geometry (`clamp(..., 1e-7, 1)`, `gbuffer_world.frag:39`,
  `gbuffer_object.glsl:82`). Material word 0 = sky, `0x00FFFFFF` = debug colour in the normal target.
- `lightCount` is `R8_UNORM` holding frames/255, so `lightingHistory` is clamped to 1..255 on both sides
  (R.cpp:1091, `lighting.frag:249`). Lighting samples are clamped to 64 (`VX_MAX_SAMPLE`).
- `renderScale` is clamped to 0.25..1, non-finite -> 1 (`DesiredRenderExtent`, R.cpp:468). Camera fov/near are
  sanitised (`SanitizeCamera`, R.cpp:188). Degenerate instance transforms are skipped (R.cpp:1113).
- At most `kMaxLights` = 256 lights, the nearest to the camera (R.cpp:1162); the shader has the same bound
  as a constant (`VX_MAX_LIGHTS`, `lighting.frag:27`). Change both.
- Draw list = front-drawn indices then inside-drawn ones; the inside draw uses `firstInstance = frontCount`
  (R.cpp:1292). A truncated BVH is published as 0 nodes (R.cpp:1148).
- Grid entries (R.cpp:983): 0 = empty, bit 31 set = uniform chunk (passed through), otherwise `slot + 1`.
- Slot reuse: a freed slot found in the pending queue clears `uploaded` and marks the grid stale
  (R.cpp:939-946). `ChunkStore::Free` does not mark anything dirty; `Adopt` / `Alloc` mark the reused slot
  dirty (`src/voxel/internal/Store.cpp:31`), so it is uploaded again. Until that upload happens `uploaded`
  is still 1 from the previous owner: with an upload backlog a reused slot can be traced with the old
  chunk's voxels for a few frames (read from the code, not observed). Keep this in mind before lowering
  `maxChunkUploadsPerFrame`.

## 3. Traps

- **No camera-cut detection exists.** ARCHITECTURE 5.4 says history is invalidated after a camera cut;
  the code only clears `historyValid` in `RecreateTargets` and `Initialize` (R.cpp:640, 1682). A teleport
  relies on the per-tap depth/normal rejection in `lighting.frag:88-121` and the colour clamp in `taa.frag:77`.
- `Frame.cppm:144` mentions `Renderer::RenderOrigin`; no such function exists. The origin is
  `World::WindowOrigin()`.
- Spare fields, written but read by no shader: `jitter.zw`, `settings.z` (front count). `outputSize` is read by
  `hud.vert` (pixels -> NDC) and `settings.w` by `present.frag` (HUD backdrop dim, 0..255). `FrameInput::deltaTime` is
  not read by the renderer. Reuse spare fields before adding new ones.
- `FrameUniforms` may only contain mat4 / vec4 / ivec4 (`Frame.cppm:22`); a vec3 or float breaks the
  "C++ layout == std140" assumption silently on the C++ side (the contract test catches the offsets).
- A shader that declares a buffer it does not use still gets it in the layout, and `BindPipeline`
  (R.cpp:1230) refuses to draw the pass if any reflected binding has no provider: the pass goes missing
  with one log line (`declares binding ... which the renderer does not provide`), not a crash. The line
  is printed once per pipeline and binding until `ReloadShaders` clears the list (R.cpp:726).
  New buffer names must be added to `FindBuffer` (R.cpp:1212); new textures to the pass's `TextureBinding` array.
- Checkerboard is 8x8 tiles, keyed on `counts.w` parity (`lighting.frag:129`). A skipped pixel copies the
  reprojected history and does not raise its count. It needs count >= 6 first. Do not go per-pixel (CLAUDE.md).
- The lighting alpha channel has two meanings: sun specular amount, or plain sun visibility for liquids
  (`lighting.frag:156`), read back in `composite.frag:157`. The spatial filter blends alpha across a
  water/shore edge only if depth and normal match; do not relax those weights without looking at shores.
- `composite.frag`: debug views are an early exit *before* the shading and `ShadeLiquid` is kept small on
  purpose (Intel SIMD16 register pressure, ARCHITECTURE 5.7). Adding work here can slow every pixel.
- `VX_WATER_CHUNK_VOXELS` (`composite.frag:86`) duplicates `VX_CHUNK_SIZE`; a contract test compares them.
- Metals keep 65 % diffuse (`composite.frag:282`): deliberate until reflections exist.
- Sky pixels have no velocity (cleared to 0); `taa.frag:20` derives sky motion from camera rotation. TAA uses
  the velocity of the closest depth in 3x3 and an upper weight cap of 0.98.
- `gbuffer_object_front` promises `depth_less` and enforces it with `min(depth, gl_FragCoord.z)`
  (`gbuffer_object.glsl:25, 85`). The inside variant must not get the qualifier. The inside/front split uses a
  margin of `2 * near + 0.05` m (R.cpp:1129); shrinking it makes boxes vanish when the camera touches them.
- Particles: lighting is per vertex (sun wrap + mean sky, no shadows); additive = opacity 0 with emission;
  depth test only, no write, no soft fade; sorted on the CPU by the fx module. Zero particles still writes
  both timestamps but skips the rendering scope (R.cpp:1360).
- Present: data debug views (normal, depth, steps, motion, material) bypass tone mapping
  (`present.frag:25`). The backbuffer is UNORM and the shader applies the sRGB curve; never pick an sRGB
  swapchain format (`ChooseSurfaceFormat`, Swapchain.cpp:40 only accepts two UNORM formats).
- Pass timers: results belong to the frame this slot submitted two frames ago (`CollectTimings`,
  R.cpp:600). Timestamps are taken at `BOTTOM_OF_PIPE`; `SyncVoxelData` copies and `ClearHistoryTargets`
  are outside every interval. `present`, `hud` and `imgui` are three intervals *inside* the one backbuffer rendering
  scope (`PassPresent`); a new draw there gets its own `Pass` or goes into one of them, never between them. `kPassNames` must stay in `Pass` enum order
  (R.cpp:64-67); the engine finds the median by the profiler name `"GPU: <name>"` (Engine.cpp:299).
- Chunk uploads per frame = min(`maxChunkUploadsPerFrame`, what fits in the staging buffer after the
  material and grid bytes). Staging is 8 MiB (about 227 chunks of 36 936 bytes) and only grows to
  `tables + one chunk` (R.cpp:908), so raising the budget above ~200 does nothing by itself.
- Descriptor pool per frame: 32 sets, 32 UBO, 160 SSBO, 96 samplers (R.cpp:515). Today a frame uses at
  most 8 sets, 8 UBO, 26 SSBO, 18 samplers. A new pass fits; a loop that binds per object does not.
- Resize / vsync change skip that frame (`PrepareSurface` returns false, R.cpp:781); a render-scale change
  does not. A pending screenshot stays requested until a frame is actually rendered, but the engine asks
  for it only once, on the last frame (Engine.cpp:271), and does not check the result of `RenderFrame`.
- HUD: one `vkCmdDraw(6, hudCount)` with no vertex buffer; nothing is drawn (both timestamps still written) when the
  canvas is empty. Quad colours are display values written into the UNORM backbuffer after the sRGB curve: do not
  tone map them. The backdrop of a menu is dimmed in `present.frag`, not with a full-screen quad (that cost 0.15 ms
  on the UHD 620). `UiLayer::Draw` returns early when ImGui produced no vertices (play mode, overlay hidden).
- If the swapchain format changes on recreate, the renderer rebuilds its pipelines (R.cpp:769) but `UiLayer`
  was initialised once with the old format (Engine.cpp:119, Ui.cpp:107). Weak spot, never exercised headless.
  If that rebuild fails `pipelines` is null and every later frame is skipped with no further log.
- Screenshot readback assumes 4 bytes per pixel and swaps B/R only for the two BGRA formats (R.cpp:1449);
  it blocks on the frame fence.
- `TransitionImage` supports a fixed list of layouts (Resources.cpp:29); an unknown one logs an error and
  uses a full barrier. A depth image sampled by a shader goes to `SHADER_READ_ONLY_OPTIMAL`; only the
  particle pass uses `DEPTH_READ_ONLY_OPTIMAL` (needs `separateDepthStencilLayouts`, Device.cpp:195).
- `Pipeline::Create` only knows the colour formats in `kColorFormats` (Pipeline.cpp:40). A new target format
  must be added there with its base type or the pipeline is refused.
- `ReflectSpirv` rejects unsized descriptor arrays and resources without set / binding decorations
  (ShaderReflection.cpp:417-444); `Pipeline::Create` refuses bindings outside set 0, unnamed bindings and
  two stages that disagree on a binding (Pipeline.cpp:174-211). Only uniform/storage blocks and combined
  image samplers can be bound by `DescriptorWriter` (`StorageImage` is reflected but has no writer method).
- `Pipeline::Create` also checks every fragment output against the attachment list (location, base type);
  an attachment no output writes is only a warning (Pipeline.cpp:254), so read the warnings after adding
  a target.
- Per-frame storage buffers grow through `EnsureCapacity` (R.cpp:437): power of two, contents not kept. If
  growing fails `Upload<T>` writes only what fits and returns the smaller count; callers must use the
  returned count (as the BVH does: a short tree is published as 0 nodes), never the size of the source.
- Staging layout is `[materials][grid][chunks...]` and the grid is written through a `uint32_t*` at offset
  `materialBytes` (R.cpp:980): that only stays aligned while `sizeof(GpuMaterial)` is a multiple of 4.
- Shaders are compiled with `--target-env vulkan1.3`, no optimiser (`ENABLE_OPT OFF` when glslang is built
  from source). A new include directory must be added to the `GLOB_RECURSE` in `VortexShaders.cmake` or
  edits there will not trigger a rebuild. Entry points are globbed from `shaders/*.{vert,frag,comp}` only.

## 4. Recipes

A. Add a field to `FrameUniforms`
1. Prefer a spare component (see traps). Otherwise append a vec4/ivec4 at the same position in
   `Frame.cppm:22` and `common.glsl:32`, and fix the `static_assert` size.
2. Fill it in `BuildFrameData` (R.cpp:1067-1094 or 1195-1197).
3. `tests/graphics/ContractTests.cpp` `LayoutContract.FrameUniformsMatchTheGlslBlock`: member count (24),
   one `VX_EXPECT_FRAME_MEMBER` line, and the "last member is `settings`" check if you append after it.
4. Update the table comment in `common.glsl` and ARCHITECTURE 5 if the meaning is a contract.

B. Add a feature toggle
1. `RenderSettings` (`Renderer.cppm`), `FeatureFlags` (`Frame.cppm:51`), `VX_FEATURE_*` (`common.glsl:59`),
   the `features |=` list (R.cpp:1186). Bits must match by hand: no test compares them. The same holds for
   `DebugView` (`Frame.cppm:63`) and `VX_DEBUG_*` (`common.glsl:68`); a new debug view that shows data
   rather than light must also be added to the list in `present.frag:25` or it gets tone mapped.
2. Branch in the shader with `VxFeature(...)`; measure the off path too.

C. Add a texture input to an existing pass
1. Declare `layout(set = 0, binding = 16+n) uniform sampler2D u_Name;` in the fragment shader.
2. Transition the image to `SHADER_READ_ONLY_OPTIMAL` in the pass function and add a `TextureBinding`
   with the same name (R.cpp:1318 style). Check that no later pass expects the old layout untracked.
3. Add `{binding, "u_Name"}` to that pipeline in `Pipelines()` (ContractTests.cpp:106) and to ARCHITECTURE 5.6.

D. Add a scene buffer (binding 10..15)
1. Block in `common.glsl` behind a `VX_NEED_*` define; mirrored C++ struct with a `static_assert` on size.
2. Per-frame data: a `Buffer` in `FrameContext`, create in `CreateFrames`, destroy in `DestroyFrames`,
   fill with `Upload<T>` in `BuildFrameData`, add to `UpdateMemoryStats`. Never bind a zero-sized buffer:
   create it with a minimum size and pass a count in `FrameUniforms`.
3. Name in `FindBuffer` (R.cpp:1212).
4. Tests: `kBuffers`, each pipeline's buffer list in `Pipelines()`, and a `LayoutContract` test like
   `LightMatchesVxLight`. `EveryBufferOfTheTableIsUsedSomewhere` fails if no shader declares it.

E. Add a render target or a full-screen pass
1. Target: member in `RenderTargets` *and* in `ForEach` (R.cpp:306), create in `RecreateTargets`, include in
   the `ok` check. Ping-pong pairs that are sampled before first written also go in `ClearHistoryTargets`.
   A format that is not 4 bytes per pixel needs a case in `BytesPerPixel` (R.cpp:172) for the memory stat.
2. Pass: `Pass` enum + `kPassNames` (same order), `PipelineId` + a desc in `MakePipelineDescs`, a
   `PassX` function (BeginPass, transitions, `BeginRendering`, `BindPipeline`, draw, EndPass), call it in
   `RenderFrame` in order. Format must be in `kColorFormats`.
3. Shader file in `shaders/`; add it to `kShaderFiles` and `Pipelines()` in ContractTests
   (`EveryPipelineShaderIsListed`, `SingleTargetPassesWriteOneFloatColour`).
4. Update ARCHITECTURE 5.3 / 5.4 / 5.6 and measure the pass on the UHD 620 (headless log prints it).

F. Change temporal accumulation or the checkerboard
1. Constants live at `lighting.frag:27-32` and `composite.frag:21-24`; history length comes from
   `RenderSettings::lightingHistory`. Keep `count` in 0..255 and the R8 encoding.
2. Verify with debug view `Lighting` (`DebugView::Lighting`) and with `checkerboardLighting` on and off;
   compare a still camera at frame 200 against a moving one (`--camera`), looking for trails and noise at
   silhouettes. No unit test covers image quality.

## 5. Verify

- `ctest --test-dir build -R 'ShaderContract|LayoutContract|ProxyCube|ReverseZ'` and
  `-R 'ShaderReflection|SpirvFile|Pipeline'` (gtest suites of `VortexGraphicsTests`, no GPU; they read the
  compiled `.spv` from `VORTEX_TEST_SHADER_DIR`, so a shader that does not compile fails the build first).
- Both headless runs from CLAUDE.md with `--validation`; exit code 2 = validation errors
  (`Engine.cpp:316`). Search the log for `Vulkan:`, `Renderer:` and `Pipeline '` error lines; a pass that
  silently went missing shows up as `declares binding` / `could not grow buffer` / `not provided`.
- Start-up line `Renderer: headless mode, output WxH, render WxH, N chunk slots, X MiB GPU memory`
  (R.cpp:1729) confirms sizes; `Screenshot saved: file (WxH)` confirms the readback.
- End-of-run lines `GPU <pass> a ms (median b ms over n frames)` for gbuffer, lighting, composite,
  particles, taa, present, hud, imgui (always 0 headless: no ImGui backend), then `HUD: N quads drawn on the last
  frame`: compare medians before and after (reference on the UHD 620 is in CLAUDE.md).
  All zero means timestamps are unsupported or too few frames ran.
- `store a/b` in the `World:` line and `chunkUploadsPending` in `RenderStats` show an upload backlog; holes
  in terrain in an early screenshot that fill later are the upload budget, not a tracing bug.
- Look at the screenshot: a black frame usually means a pass was skipped (targets are cleared to 0); noisy
  lighting that never settles means history is being rejected; water must look the same with TAA on and off.
- Windowed paths (swapchain recreate, vsync toggle, UI, F5 reload) cannot be checked headless: say so.
