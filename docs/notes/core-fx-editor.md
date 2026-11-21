# Notes: core, math, fx, editor, build

[[../../../00 Docs Map|Docs map]] · [[../ARCHITECTURE|architecture]] · [[../design/game-ui|game UI]]

Checked against engine `430fde0`.

These notes add to `CLAUDE.md` and `docs/ARCHITECTURE.md` and do not repeat them. Paths are relative to `engine/`.
Code is cited by file and symbol; search for the symbol. The text before this rewrite, with its line numbers, is
`docs/history/notes/core-fx-editor-2026-11-18.md`.

## 1. Map

**core** (`vortex.core`, re-exported partition by partition in `src/core/Core.cppm`)
- `Types.cppm` - `Handle<Tag>` (index + generation, invalid index `0xFFFFFFFF`), `NonCopyable`, `ScopeExit`,
  `_KiB/_MiB`.
- `Log.cppm` + `internal/Log.cpp` - spdlog behind `Log::Info(...)` (std::format); `Verify` (always on, aborts),
  `Assert` (off under `NDEBUG`).
- `Time.cppm` - `NowSeconds()` (steady clock, zero = first call), `Stopwatch`.
- `Random.cppm` - `Hash32/Hash3/HashCombine/HashToFloat`, `Rng` (PCG32); all `constexpr`, platform independent.
- `Jobs.cppm` + `internal/Jobs.cpp` - `JobSystem`: FIFO thread pool, `Submit`, `ParallelFor`, `WaitIdle`.
- `Profiler.cppm` + `internal/Profiler.cpp` - global named timers (120-frame ring) and counters; `ProfileScope`.
- `Paths.cppm` + `internal/Paths.cpp` - executable directory, asset root (`<exe dir>/assets`).
- `Args.cppm` - `CommandLine` (header only): `--flag`, `--key=value`, `--key value`.
- `Input.cppm` - `Key` / `MouseButton` (GLFW codes), `Input` per-frame state with UI capture masking.
- `Hud.cppm` + `internal/Hud.cpp` + `internal/HudFont.inc` - the game HUD as data (`docs/design/game-ui.md`):
  `Rgba`, `HudRect`, `PlaceRect`, `HudQuad` (GPU layout), `BitmapFont` (5 x 9, ASCII 32..126, glyph art parsed at
  compile time), `HudCanvas`, `Menu` / `MenuInput` / `MenuEvent`, `CursorCapture`. No GPU, no GLFW. Tests:
  `tests/core/HudTests.cpp` (suites `BitmapFont`, `HudLayout`, `HudCanvas`, `HudMenu`, `CursorCapture`).

**math** (`vortex.math`, header only, namespace `vortex::math`)
- `Util.cppm` - `FloorDiv/FloorMod`, `Saturate/Lerp/SmoothStep`, `Transform` (position, quat, uniform scale).
- `Geometry.cppm` - `Ray`, `AABB` (default = empty), `IntersectRayAABB`, `Plane`, 5-plane `Frustum`.
- `Projection.cppm` - `PerspectiveReverseZ`, `LinearizeReverseZ`, `Halton`.
- `Noise.cppm` - `SimplexNoise` (2D/3D, `Fbm`, `Ridged`), hash based, pure function of (position, seed).

**fx** (`vortex.fx`)
- `Particles.cppm` + `internal/Particles.cpp` - `ParticleSystem`: fixed SoA pool, emitters, collision callback,
  `BuildRenderData`; `GpuParticle`.
- `Effects.cppm` + `internal/Effects.cpp` - `effects::*` recipes, `SpawnExplosion`, `SpawnVoxelBreak`.

**editor** (`vortex.editor`, part of `VortexEngine`; the "Save world" button of its World panel is enabled only when
`EngineConfig::world.saveDirectory` is set - the game sets none, so in Shredborne it is disabled)
- `View.cppm` + `internal/View.cpp` - screen <-> world, ImGuizmo matrices, sun angles, fly camera. No ImGui.
- `Tools.cppm` + `internal/Tools.cpp` - picking, brush, primitives, palettes, import, entity helpers. No ImGui.
- `Editor.cppm` + `internal/Editor.cpp` - `Editor` class: state, actions, input (`OnUpdate`), all ImGui / ImGuizmo
  code (`OnGui`).

**build**
- `../CMakeLists.txt` - game exe `Shredborne` (globs `src/*.cpp`, `src/*.cppm`), sets `VORTEX_BUILD_TESTS` from
  `SHREDBORNE_BUILD_ENGINE_TESTS`.
- `../CMakePresets.json` - `dev` (`build/`, RelWithDebInfo), `debug` (`build-debug/`), `release` (`build-release/`,
  tests off).
- `CMakeLists.txt` - options, `vortex_options`, `vortex_warnings`, `vortex_add_module_library`, the six libraries
  (`VortexCore` core math voxel fx, `VortexPhysics`, `VortexSim` scene sim, `VortexNet`, `VortexGraphics`,
  `VortexEngine` engine editor), `vortex_assert_gpu_free` and `vortex_assert_no_network` (configure-time link checks,
  section 2 "Build").
- `cmake/Dependencies.cmake` - every FetchContent dependency, pinned. `cmake/VortexShaders.cmake` - GLSL -> SPIR-V,
  `vortex_deploy_assets`.
- `tests/CMakeLists.txt` - `vortex_add_tests`: one test exe per `tests/<dir>` (core, physics, sim, net, graphics,
  engine), each test with a 180 s timeout (`VORTEX_TEST_TIMEOUT`), wall-clock tests listed in
  `VORTEX_SERIAL_TESTS_<name>` run serially. `tools/CMakeLists.txt` - adds every subdirectory that has a
  CMakeLists.txt: `tools/server` (`vortex_server`) and `tools/bot` (`vortex_bot`), both GPU-free (`notes/net.md`).

## 2. Invariants

### Job system (`src/core/internal/Jobs.cpp`)
- `Submit`, `PendingJobs`, `ThreadCount` are thread safe. Default thread count is `hardware_concurrency - 1`, at
  least 1 (`JobSystem` constructor).
- The destructor does **not** drop queued jobs: workers leave only when `stop` is set and the queue is empty. Every
  queued job therefore runs, possibly after its owner is gone. In `sim::Simulation::Impl` `jobs` is declared before
  `world` / `terrain` and `Simulation::Shutdown` resets those first, so load and cook jobs do run after `World` and
  `TerrainColliders` are destroyed.
- Hence the rule: a job captures a `shared_ptr` to state that outlives the owner, never `this` or a reference into the
  owner. Patterns to copy: `World::Shared` (`src/voxel/internal/World.cpp`, the `LoadJob` lambda) and
  `TerrainColliders`' `Shared` with a `cancelled` flag under a `shared_mutex` (`src/physics/internal/Terrain.cpp`).
- Results go back through the owner's own mutex-protected vector and are integrated on the main thread
  (`World::IntegrateResults`). The pool returns nothing. Owners throttle themselves with an `inFlight` atomic against
  their own job limit (`World`'s `m_MaxJobs`, `TerrainColliders`' `maxJobs`). The job itself counts down as its last
  statement (a `ScopeExit` in the terrain's `CookJob`).
- A job must not wait for another job of the same pool. `ParallelFor` is the exception because the caller works too.
- `WaitIdle` must never be called from inside a job: `running` cannot reach 0 while the caller is a running job. Tests
  call it from the test thread.
- An exception escaping a `Submit`ted job is caught and logged; the job's result is lost and the owner's `inFlight` is
  not decremented unless the job did it first.

### Profiler (`src/core/internal/Profiler.cpp`)
- Global state under one mutex; `AddSample` / `SetCounter` may be called from any thread. Lookup is a linear string
  compare per call.
- Samples with the same name within a frame are summed; `Profiler::EndFrame()` (main thread, once per frame, at the end
  of the `Engine::Run` loop) turns the sums into one history sample. A timer not touched in a frame records 0. Timers
  are never removed.
- Ring: `cursor` = next write index, `samples` = valid count (<= `kHistory` = 120). Newest sample is
  `history[(cursor - 1) mod 120]`. Both readers rely on this: the profiler panel (`Editor.cpp`) and the headless
  median (`Engine::Run` exit statistics).
- `ProfileScope` stores the `const char*`: pass a string literal.
- Naming convention in use: `"CPU: ..."` (`Engine::Run`: frame, world, simulation), `"GPU: <pass>"` (the renderer's
  timers), `"Renderer::RenderFrame"`.

### Input (`src/core/Input.cppm`)
- Not thread safe; main thread only. `NewFrame()` must run before events are delivered: `Window::PollEvents` does it,
  a headless run does it in `Engine::Run`.
- Edges (`KeyPressed`, `MousePressed`, `*Released`) are true for one **frame**, not one fixed step.
- UI capture is set once per frame by `UiLayer::UpdateInputCapture` (called from `Engine::Run`) and masks
  `KeyDown/KeyPressed/MouseDown/MousePressed/ScrollY`. With ImGui input switched off (`Engine::SetImGuiInput(false)`,
  play mode) both capture flags are false. It does **not** mask `KeyReleased`, `MouseReleased`, `MouseX/Y`,
  `MouseDeltaX/Y`, `KeyDownRaw`, `MouseDownRaw`. The raw queries are for state that must not flip while an ImGui
  widget is active (the F3 overlay's held Left Alt: ImGui has the keyboard during a slider drag) and for
  `CursorCapture::Inputs::leftButtonDown`; never use them for actions.
- `Key` values are GLFW key codes, passed through unchanged (`Window.cpp` key callback); GLFW repeat events are
  ignored there. Keys outside `[0, 512)` and buttons outside `[0, 8)` are dropped.
- Cursor units are window pixels, y down. `ResetCursorDelta` is called on every cursor-capture change
  (`Window::SetCursorCaptured`).

### HUD (`src/core/Hud.cppm`)
- `HudQuad` is mirrored by `VxHudQuad` (`shaders/include/common.glsl`); `BitmapFont::kGlyphWidth/Height` by
  `VX_HUD_CELLS_X/Y`. Both are contract-tested (`LayoutContract.HudQuadMatchesVxHudQuad`,
  `.HudCellGridMatchesTheFont`). Cell bit = row * 5 + column, row 0 at the top; at most 64 cells (two words).
- A glyph in `HudFont.inc` is 5-character rows of `.` / `#`, up to 9 rows (missing rows are empty), in ASCII order.
  `BuildGlyphs` is `consteval`: a wrong row length, a stray character or a character out of order is a compile error.
  `BitmapFont.EveryPrintableGlyphIsDistinctAndFitsTheGrid` fails if two glyphs are identical.
- HUD coordinates are framebuffer pixels; `Input::MouseX/Y` are window coordinates. Convert with `Engine::HudCursor()`
  (HiDPI / Wayland scaling), never compare them directly.
- Text and `PlaceRect` floor positions to whole pixels; quads of fractional size are allowed (rasterised by pixel
  centres).
- `Menu::HandleInput` order: Back wins, then a click (on an item: activate; elsewhere: nothing, and the frame's keys
  are ignored), then Up / Down, then Enter / Space. The pointer counts only once it has moved since `Open()` (or since
  a frame without `hasMouse`): a just-released cursor's position is stale. Hover takes the focus only on a frame with
  `mouseMoved`, so a resting pointer does not undo Up / Down. Call `Open()` when the menu is shown.
- `CursorCapture` owns the pause flag. Escape pauses only when not paused; resuming is the game's call (menu Back or
  Resume), so the game must pass `escape = false` on a frame the menu already handled. A capture starts only with
  `pointerInside` (Wayland locks on pointer entry) and is then kept while wanted. `Result::clicks` stays false after a
  capture until the left button has been up and `kClickGrace` (0.15 s) has passed: game clicks use it, look does not.
- Game UI goes into the HUD; ImGui is only for the F3 overlay and the editor (CLAUDE.md contracts).

### Command line
- Engine flags are all in `EngineConfig::ApplyCommandLine` (`Engine.cpp`): `--headless --validation --frames N
  --screenshot F --width W --height H --scale S --ao-distance M --no-shadows --no-ao --no-emissive --no-taa
  --no-checkerboard`.
- Game flags are parsed in `main` of `../src/main.cpp` (`--camera x,y,z[,yaw,pitch]` through `ParseCameraPose`,
  metres and radians, or `probe` / `probe-far`; `--editor`, with `--headless` it also opens every panel; the headless
  scripts and measurements) and, for a session, in `RunSession` of `../src/Session.cpp` (`--connect`, `--token`,
  `--other-terrain`). What each flag and log line means is listed in the game repository's `docs/verify.md`.
- The screenshot is requested only on the last frame and only when `--frames` is non-zero (`Engine::Run`).
- Headless uses `dt = fixedTimeStep` every frame (`Engine::Run`): exactly one physics step per frame, reproducible.
  Return code 2 = validation errors (`Engine::Run`, `Device::ValidationErrorCount`).

### Particles (`src/fx/internal/Particles.cpp`)
- No locking anywhere: `ParticleSystem` is main-thread only. `BuildRenderData` is `const` but writes `sortScratch`,
  so it is not safe to call concurrently either.
- Order in a frame (`Engine::Run`): `app.OnUpdate` -> `particles.Update(dt, world.IsSolidAt)` -> `app.OnGui` ->
  gather -> `BuildRenderData(WindowOrigin, camera)`. Particles spawned in `OnUpdate` are simulated the same frame;
  anything spawned later in the frame (`OnGui`, or code run after rendering such as `scene.Update`) is first simulated
  next frame. A `sim::Simulation` stepped without an engine never updates its pool (`notes/scene-engine.md`).
- Pool: SoA vectors sized to `capacity` at construction, `[0, alive)` live, swap-remove. A new field needs a line in
  the `Impl` constructor, `Spawn` and `Remove`.
- Full pool: new particles are dropped, never the oldest; one warning per `ParticleSystem` lifetime. Capacity comes
  from `EngineConfig::maxParticles` (32768; a server's simulation uses 0).
- Units: positions `dvec3` world metres; velocity m/s; `size*` is the edge length in metres, the GPU gets the **half**
  size; colours linear; `gravity` is a multiplier of the `Update` argument (-9.81).
- `Spawn` sanitises: non-finite position / velocity / lifetime or lifetime <= 0 is rejected; sizes, drag, emissive
  clamped >= 0; bounce clamped to [0,1].
- Determinism: `Randomise` always draws the same number of random values; emitters run in slot order; the sort breaks
  ties by index. Tested by `Particles.SameSeedAndCallsGiveIdenticalRenderData`. Any new random draw must be
  unconditional.
- Emitters: `EmitterId` = slot index + generation; `RemoveEmitter` bumps the generation and pushes the slot on a LIFO
  free list; stale ids are ignored by every call. Emission that does not fit the pool is dropped, not owed. `Clear()`
  removes particles, keeps emitters.
- `GpuParticle` (48 bytes) mirrors `VxParticle` in `shaders/include/common.glsl`; layout is tested by
  `LayoutContract.ParticleMatchesVxParticle`. Additive particles are sent with alpha 0, rgb pre-multiplied by opacity
  and emissive >= 1.
- Effect budgets are caps in code (`ScaledCount` caps in `Effects.cpp`): `SpawnExplosion` <= 400 fire + 200 sparks +
  200 smoke, radius clamped to [0.1, 8] m; `SpawnVoxelBreak` <= 12 debris + 4 dust. `sim::PresentBlast` samples the
  carved voxels with stride `n / 400` and crumb voxels with a stride of at least `debrisParticleStride` and
  `ceil(n / 400)` (`kMaxBursts` in `sim/internal/Blast.cpp`). Particles are presentation: nothing in a session's hash
  or schedule may read them.

### Editor
- Split: logic is free functions in `:view` / `:tools` (no ImGui, no GPU, unit tested); `Editor` adds input and
  widgets. ImGui / ImGuizmo headers appear only in `internal/Editor.cpp`.
- `Editor` is main-thread only and holds no engine pointer between calls. The game calls `OnUpdate` first in its
  `OnUpdate` (after its pause menu) and `OnGui` from its `OnGui`, then skips its own input while `CapturesInput()` (a
  headless run keeps the game's automation going even with the editor open). While the game's pause menu is open the
  game does not call `Editor::OnUpdate` (F1 and the tools do nothing); when the menu opens it calls
  `Editor::ReleaseInput`, which ends the right-button look (cursor released, the editor's `NoMouse` cleared), a brush
  stroke and the hover preview. A look after resume needs a new press. The editor exists only offline: a session
  (`--connect`) refuses `--editor`.
- ImGui `NoMouse` has two writers: the editor (during its look, `noMouseSet`) and `UiLayer::SetInputEnabled` (play
  mode). Each clears only what it set: the UI layer remembers the bits it added (`m_AddedFlags`) and re-applies them at
  every `BeginFrame` while disabled; the editor sets `NoMouse` on every frame of its look, so a clear by the other
  owner is repaired next frame.
- Selection is a `scene::Entity` value validated on every read (`Editor::Selection`). Never cache component pointers
  across a call that can create or destroy entities (the entity helpers in `Tools.cpp`, the inspector buttons, which
  come last in their panel).
- Every path that changes the selection or the tool, or closes the editor, sets `cancelGizmo`; `Impl::Gizmo` then
  resets ImGuizmo with `Enable(false/true)`.
- Coordinates: cursor = ImGui pixels, y down; `CursorRay` origin is `dvec3` world metres; ImGuizmo gets `GizmoView` /
  `GizmoProjection` (world space, y up, 0..1 depth), never the engine's reverse-Z matrix. Voxel object
  `Transform.position` is the model's minimum corner, scale = voxel size; the gizmo pivot is half the model size in
  voxels.
- Clamps live in the logic, not the widgets: brush radius `[voxelSize, 10 m]`, spacing `[0.05, 4]` x radius and >= one
  voxel, primitive size `[1, 256]` per axis, reach >= 1 m.
- Actions (`DeleteSelection`, `SpawnPrimitive*`, `ImportFile`, `BrushAlong`, ...) need neither an open editor nor an
  ImGui frame; they report through `Status()`. The headless run depends on that.
- ImGui window names end in `##vortex.editor`; widget labels carry a `##<group>` suffix to stay unique.

### Build
- Layering is enforced by which directories each `vortex_add_module_library` call lists and its link line. The real
  graph: Core <- Physics <- Sim <- Net; Core <- Graphics; Engine links all five. Graphics does not depend on Physics,
  Sim or Net; the order of modules inside one library is not checked by anything.
- `vortex_assert_gpu_free` (`CMakeLists.txt`, `tests/CMakeLists.txt`, `tools/*/CMakeLists.txt`): the configure step
  fails if `VortexCore`, `VortexPhysics`, `VortexSim`, `VortexNet`, `vortex_enet`, `VortexSimTests`,
  `VortexNetTests`, `vortex_server` or `vortex_bot` reach Graphics, Engine, Vulkan, GLFW, vk-bootstrap, VMA, ImGui or
  ImGuizmo through any chain of link libraries.
- `vortex_assert_no_network`: the configure step fails if `VortexCore`, `VortexPhysics`, `VortexSim` or
  `VortexSimTests` link `VortexNet` or ENet. ENet (`vortex_enet`) is PRIVATE to `VortexNet`.
- Everything importing engine modules must link a `Vortex::*` target so it inherits `vortex_options` (PUBLIC through
  `vortex_add_module_library`), which carries Jolt's architecture flags and `-ffp-contract=off` (`notes/voxel.md`
  "Determinism"). `vortex_warnings` is PRIVATE to the libraries; tests add it themselves (`vortex_add_tests`).
- Dependencies are declared between the `CMAKE_CXX_SCAN_FOR_MODULES OFF` / restore lines in
  `cmake/Dependencies.cmake`.

## 3. Traps

- **`ParallelFor` and exceptions.** `fn` is captured by reference by helper jobs. If `fn` throws on a worker the
  exception is swallowed, `done` never reaches `rangeCount` and the caller blocks forever. If it throws on the calling
  thread, helpers keep a dangling reference. `fn` must not throw.
- **`ParallelFor` has no caller** outside core, and the particle simulation does not run on jobs although the
  `Jobs.cppm` comment says so. There is no unit test for `JobSystem`, `Profiler`, `CommandLine` or `Input`; they are
  only exercised indirectly. Add tests before changing them (recipe 4.5).
- **`"CPU: frame"` lags one frame.** Its `ProfileScope` (top of the `Engine::Run` loop) ends after
  `Profiler::EndFrame()`, so each sample lands in the next frame's sum. Harmless; do not "fix" it by moving `EndFrame`
  out of the loop body.
- **Renaming a GPU pass or the `"GPU: "` prefix** silently breaks the median in the headless log: `Engine::Run`
  matches by string and falls back to the last frame with `over 0 frames`.
- **`Profiler::SetCounter` has no caller**; the editor's Counters section always shows "No counters".
- **`Assert` is compiled out in the `dev` preset** (RelWithDebInfo defines `NDEBUG`); only `Verify` is always on.
  `VORTEX_DEBUG` (`vortex_options`, Debug only) is used nowhere in `src/`.
- **The `dev` build must be optimised.** `engine/CMakeLists.txt` refuses an optimised build type whose flags carry no
  `-O` (a cache with empty `CMAKE_CXX_FLAGS_RELWITHDEBINFO` once gave an -O0 dev build for weeks; CLAUDE.md lessons).
- **Input edges inside `OnFixedUpdate`.** The fixed loop runs 0..4 times per frame (0..8 with a session's tick
  source), so an edge read there is seen 0..n times. Read edges in `OnUpdate` and latch them (the game latches a jump
  press in `m_ButtonLatch`).
- **`KeyReleased` / `MouseReleased` ignore UI capture on purpose.** The editor's camera-look latch needs the release
  even when the cursor ends over a panel. Masking them would leave the cursor captured.
- **Mouse deltas are not masked** by UI capture. Gate them yourself (the editor uses `looking`, the game uses the
  cursor capture).
- **The toggle key goes through `KeyPressed`**: F1 does nothing while an ImGui text field has the keyboard.
  Deliberate.
- **ImGui keyboard navigation is off on purpose**: it swallowed game keys (CLAUDE.md lessons).
- **`CommandLine` is lenient.** Unknown or misspelt flags are ignored without a message. `--key value` is only read
  when the value does not start with `--`. `GetInt` accepts trailing garbage (`12abc` -> 12). Negative values are cast
  to `uint32_t` (`--frames -1` = 4294967295, `ApplyCommandLine`). `GetFloat` uses `std::stof` (locale dependent,
  accepts `nan` / `inf`). Of the float flags only `--scale` is made safe downstream: the renderer clamps it to
  [0.25, 1] and treats non-finite as 1 (`DesiredRenderExtent`). The first occurrence of a repeated flag that carries a
  value wins.
- **`--no-checkerboard`** is implemented but missing from the doc comment of `EngineConfig::ApplyCommandLine` in
  `src/engine/Engine.cppm`.
- **`Paths::SetAssetRoot` is not thread safe** and `AssetRoot()` returns a reference to the global: call it once at
  start-up before any job reads assets.
- **`Log::Write` reads `g_Logger` without the mutex** in its lazy-init check. Engine start calls `Log::Init()` first
  (`Engine::Initialize`); do not log from threads before that.
- **Particle collision is a point sample at the end position**, not a swept test: a particle moving more than a
  wall's thickness per frame passes through (rain at about 9 m/s moves 0.15 m per 60 Hz frame; a voxel is 0.125 m).
  Known weak spot, not a bug to patch locally.
- **`SolidQuery` cost.** One call per colliding particle per frame, up to five more when it hits. `World::IsSolidAt`
  is the callback (`Engine::Run`); keep it cheap.
- **Embedded particles move freely** until they leave the solid; deliberate, tested by
  `Particles.EmbeddedParticlesTravelUntilTheyLeaveTheSolid`.
- **A full pool changes the random stream**: `Burst` stops drawing when the pool is full. Still deterministic for
  identical call sequences, but not comparable across capacities.
- **`BuildRenderData` sorts every live particle every frame** on the main thread; its cost at 32768 particles on the
  UHD 620 is not measured. The renderer has no particle cap of its own: its buffer grows to whatever
  `BuildRenderData` returns (`EnsureCapacity` in `Renderer.cpp`), so the pool capacity is the only limit.
- **`AddEmitter` does not validate the recipe**; a non-finite one simply never emits. Do not add "validation" that
  returns an invalid id unless callers are updated.
- **Parallel arrays in `Editor.cpp`** must match their enums by order: `kToolNames` <-> `Tool` + `kToolCount`
  (`Editor.cppm`), `kMotionNames` <-> `physics::MotionType` (`PhysicsWorld.cppm`), `kDebugViewNames` <->
  `graphics::DebugView` (`Frame.cppm`), brush / primitive names <-> their enums in `Tools.cppm`. `EnumCombo` clamps,
  so a mismatch shows the wrong label instead of crashing.
- **Tool shortcuts and the menu** assume tools are numbered from key `1`; the headless exercise cycles
  `frame % kToolCount` (`ExerciseEditor` in `../src/main.cpp`).
- **`EditorPanels::SetAll`** (`Editor.cppm`) and the Panels menu (`Editor.cpp`) list the panels by hand. A panel
  missing from `SetAll` is not covered by the headless smoke run.
- **Brush spacing is measured against the re-cast hit on the edited terrain**, not between brush centres. Replacing it
  with a centre distance brings back drilling under a resting cursor
  (`EditorBrush.ARestingCursorAppliesOnceInsteadOfDrilling`).
- **The brush ignores hardness and the indestructible flag**: it calls `World::FillSphere` / `FillBox` (`Tools.cpp`),
  which take no `power`. Only `CarveSphere` / `CarveSphereQ` (used by `sim::Blast`, behind `Engine::Explode`) respects
  them. `FillSphere` quantises centre and radius to 1/16 voxel and forwards to `FillSphereQ`, so the brush uses the
  integer sphere test too. Do not route gameplay damage through the brush functions. The island search and the load
  check never run after editor edits (CLAUDE.md contracts).
- **`.vox` z offset uses the ceiling**, x and y the floor (`Tools.cpp` import); looks inconsistent, is correct for the
  mirrored axis (`EditorImport.VoxModelsKeepTheirLayoutOnTheMirroredAxis`).
- **`ImportFile` can fail half way**: models created before running out of chunk slots stay, and the palette stays.
- **The gate's editor smoke run must reach every step.** `ExerciseEditor` acts at headless frames 10, 100, 110, 120,
  130 (import), 150 (delete) and 160 (pick + the summary log line); gate G4 runs `--frames 170 --editor` (WP1; it ran
  120 before and never reached the import, the delete or the summary line). New exercise steps: keep them below 170,
  or raise the gate's count in `CLAUDE.md` in the same commit.
- **Losing window focus fires every release edge.** The window's focus callback calls `Input::Clear()`, so all held
  keys and buttons report `KeyReleased` / `MouseReleased` on the next frame, unmasked by UI capture. Code that acts on
  a release (not only the editor look latch) must tolerate that.
- **Entities without a `RigidBody` cannot be picked** with the cursor (`Tools.cppm`); only the scene list selects
  them.
- **Inspector position edit teleports** (`SetTransform(..., true)`), the gizmo does not (`false`); both zero the body
  velocity. Keep the difference.
- **`VORTEX_BUILD_TESTS` is overwritten with `FORCE`** from `SHREDBORNE_BUILD_ENGINE_TESTS` in the game build
  (`../CMakeLists.txt`); setting the engine option alone has no effect there.
- **`VORTEX_BUILD_TOOLS`'s option text** still says "CPU reference renderer"; the tools are `vortex_server` and
  `vortex_bot`, and no CPU reference renderer exists in this tree.
- **CMake variables used by `vortex_deploy_assets` are `CACHE INTERNAL`** because of scope (CLAUDE.md lessons).
- **Source lists are globs** (`CONFIGURE_DEPENDS`): every `.cpp` / `.cppm` below a listed directory is compiled,
  wherever it sits. A stray scratch file in `src/` joins the library.
- **Editing any `shaders/include/*.glsl` or `shaders/shared/*.glsl` recompiles every shader** (`VortexShaders.cmake`);
  only top-level `*.vert/*.frag/*.comp` are entry points (non-recursive).
- **`gtest_discover_tests` runs each test executable at build time** (`vortex_add_tests`): a test binary that crashes
  on start-up fails the build, not just ctest.

## 4. Recipes

**4.1 Add a command line flag.** Engine-wide: add the field to `EngineConfig` (`src/engine/Engine.cppm`) if needed,
parse it in `ApplyCommandLine`, add it to the doc comment of `ApplyCommandLine`. Clamp or validate at the parse site
(see the unsigned casts above). Game-only: parse in `main` of `../src/main.cpp` next to `--camera` (refuse it in a
session in `RunSession` if it changes the world). List it in the game's `docs/verify.md`, and in the `CLAUDE.md` gate
table if a gate uses it.

**4.2 Add a particle effect.** Declare the recipe in `src/fx/Effects.cppm`, implement in `internal/Effects.cpp`
setting **every** `EmitterDesc` field like the existing ones; run colours through `SafeColor`; any count that scales
with input goes through `ScaledCount(base, perUnit, amount, cap)`. Extend `Effects.PresetsHaveFinitePositiveValues`
and `Effects.PresetsCarryTheDocumentedCharacter` in `tests/core/ParticleTests.cpp`, plus a bounded-count test for a
`Spawn*` function. Check `docs/design/fx-budget.md` for the budget. A blast's particles belong in `sim::PresentBlast`,
never in `sim::Blast`.

**4.3 Add a per-particle property.** `ParticleDesc` (`Particles.cppm`) -> SoA vector in `Impl` (resize in the
constructor, copy in `Spawn` and `Remove`) -> use in `Update` or `BuildRenderData`. If it reaches the GPU use the free
`params.w` first; changing `GpuParticle` means changing `VxParticle` (`shaders/include/common.glsl`) and
`LayoutContract.ParticleMatchesVxParticle` together. Sanitise in `Spawn`. If it is randomised, draw unconditionally in
`Randomise`.

**4.4 Add an editor tool or panel.**
- Logic first: a free function in `Tools.cppm` / `internal/Tools.cpp` taking `World` / `Scene` / `PhysicsWorld`, with
  tests in `tests/engine/EditorTests.cpp` using its `Fixture`.
- Tool: add the enumerator to `Tool`, bump `kToolCount`, extend `kToolNames`, the shortcut block, the `switch` in
  `OnUpdate` and in `ToolsPanel`; settings go in `EditorSettings`. Wrap it as an `Editor` action that sets `Status()`.
- Panel: add the flag to `EditorPanels` **and** `SetAll`, a `Begin/End` block in `OnGui` with `PlacePanel` and a
  `##vortex.editor` name, a menu item, an `Impl` method.
- Call the new action from `ExerciseEditor` (`../src/main.cpp`) so the headless `--editor` run covers it (mind the
  frame count, see traps).

**4.5 Add a test.** Put `TEST(Suite, Name)` in an existing file, or a new `.cpp` under `tests/core` (links
`VortexCore`), `tests/physics`, `tests/sim` (links `VortexSim` only), `tests/net` (links `VortexNet`), `tests/graphics`
or `tests/engine` (`vortex_add_tests`); new files are globbed. Includes first, then `import vortex.<module>;`. Suite and
helper names must not collide with engine types (CLAUDE.md code rules). Core utilities have no file yet:
`tests/core/JobTests.cpp` etc. would be new. `VORTEX_TEST_SHADER_DIR` and `VORTEX_TEST_SOURCE_DIR` are predefined. A
test that measures on the wall clock goes into `VORTEX_SERIAL_TESTS_<binary>` (RUN_SERIAL); a measurement that should
not run by default is `DISABLED_`. ROADMAP "State" holds the test count: update it in the same commit.

**4.6 Add a module, a shader, a dependency.**
- Module partition: new `src/<dir>/Foo.cppm` with `export module vortex.<dir>:foo;`, add `export import :foo;` to the
  primary interface (e.g. `Core.cppm`), implementation in `src/<dir>/internal/Foo.cpp` starting with the global
  fragment and `module vortex.<dir>;`. No CMake edit (glob).
- New module directory: primary interface `src/<name>/<Name>.cppm` with `export module vortex.<name>;`, then add
  `<name>` to the right `vortex_add_module_library` call (`CMakeLists.txt`) according to the layering; add
  `export import vortex.<name>;` to `src/engine/Engine.cppm` if games should get it; add tests in the matching
  `tests/<dir>`. A module that a session server needs goes into `VortexNet` or below and must pass
  `vortex_assert_gpu_free`; anything below Net must also pass `vortex_assert_no_network`.
- Shader: add `shaders/<name>.vert|.frag|.comp`; it is compiled to `<build>/vortex_assets/shaders/<name>.<ext>.spv`
  and copied to `<exe dir>/assets/shaders` by `vortex_deploy_assets`. Includes are relative to `shaders/` (`-I` is the
  shader directory, `VortexShaders.cmake`). A shader used by a pipeline must also be listed in `kShaderFiles` in
  `tests/graphics/ContractTests.cpp` or `ShaderContract.EveryPipelineShaderIsListed` fails. Loading it is the
  renderer's side (see `notes/graphics.md` section 4).
- Dependency: `FetchContent_Declare` in `cmake/Dependencies.cmake` with a release tag (or a full commit hash plus a
  dated comment, like ImGuizmo), switch its tests / examples / install off, and for libraries without usable CMake use
  `SOURCE_SUBDIR _vortex_headers_only_` plus an own `vortex_<name>` target with `SYSTEM` includes. Link it `PRIVATE` to
  the lowest library that needs it. Compile its implementation in a plain non-module `.cpp`. A system package means
  one `sudo pacman -S` line for the owner.

## 5. Verifying a change here

Gates G1 (warning-free build) and G2 (all tests) always; G3 and G4 (the two headless runs) for anything the game
uses. What is specific to this area:

| Changed | Run | Look for |
|---|---|---|
| fx | `./build/engine/tests/VortexCoreTests --gtest_filter='Particles.*:Effects.*'` | all pass |
| `GpuParticle` / particle shaders | `VortexGraphicsTests --gtest_filter='LayoutContract.*:ShaderContract.*'` | `ParticleMatchesVxParticle` |
| HUD data | `VortexCoreTests --gtest_filter='BitmapFont.*:HudLayout.*:HudCanvas.*:HudMenu.*:CursorCapture.*'`, `LayoutContract.Hud*`; G3 | `HUD: N quads drawn on the last frame` with N > 0; look at the screenshot (`--pause-menu` for the menu) |
| editor logic | `VortexEngineTests --gtest_filter='Editor*'` (`EditorView`, `EditorBrush`, `EditorPrimitive`, `EditorMaterials`, `EditorImport`, `EditorTools`, `EditorState`) | all pass |
| `Editor.cpp` widgets | headless run with `--editor --frames 170` (G4's 120 stops before import / delete / summary) | exit 0; log lines `Editor: ...` for each exercised action except the brush (not logged) and the final `Editor: N entities, last action '...'` without `(failed)` |
| jobs | `VortexCoreTests --gtest_filter='World.*'`, `VortexPhysicsTests --gtest_filter='TerrainColliders.*:TerrainGuard.*'` (the only users of `JobSystem`), then G3 | run ends (no hang at exit), `World: ... store a/b` line present |
| profiler | G3 | `GPU <pass> x ms (median y ms over N frames)` with N > 0 for every pass |
| input / command line | G3; flags by trying them | `Player feet at ..., on the ground`; a wrong flag gives no error, so check the effect |
| CMake, shaders, dependencies | fresh `cmake --preset dev` and full build (G1) | no warnings; `GLSL <name>` lines for a new shader; the configure checks pass; the test count unchanged or higher |

- Test binaries are in `build/engine/tests/` (no output directory is set, so it follows the source tree);
  `ctest --test-dir build -R '^Particles\.'` works too.
- Particles in a screenshot: the automated blast happens at headless frame 90 (`m_HeadlessFrame == 90` in
  `../src/main.cpp`); use `--frames` a little above that to see fire and debris, and expect no
  `ParticleSystem: pool of ... is full` warning in the log of a normal run.
- Editor panels, gizmo, keyboard focus and cursor capture cannot be verified headless: say so and ask the owner.
