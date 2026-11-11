# Notes: core, math, fx, editor, build

Maintenance notes read from the code on 2026-11-10. They add to `CLAUDE.md` and `docs/ARCHITECTURE.md`
and do not repeat them. Paths are relative to `engine/`; `file:N` is a line number at that date.
Every statement was re-checked against the source on the same day; nothing here was built or run while writing.
Line numbers drift: when one no longer matches, trust the symbol name and fix the number.

## 1. Map

**core** (`vortex.core`, re-exported partition by partition in `src/core/Core.cppm`)
- `Types.cppm` - `Handle<Tag>` (index + generation, invalid index `0xFFFFFFFF`), `NonCopyable`, `ScopeExit`, `_KiB/_MiB`.
- `Log.cppm` + `internal/Log.cpp` - spdlog behind `Log::Info(...)` (std::format); `Verify` (always on, aborts), `Assert` (off under `NDEBUG`).
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
- `Particles.cppm` + `internal/Particles.cpp` - `ParticleSystem`: fixed SoA pool, emitters, collision callback, `BuildRenderData`; `GpuParticle`.
- `Effects.cppm` + `internal/Effects.cpp` - `effects::*` recipes, `SpawnExplosion`, `SpawnVoxelBreak`.

**editor** (`vortex.editor`, part of `VortexEngine`; the "Save world" button of its World panel is enabled only when
`EngineConfig::world.saveDirectory` is set - the game sets none, so in Shredborne it is disabled)
- `View.cppm` + `internal/View.cpp` - screen <-> world, ImGuizmo matrices, sun angles, fly camera. No ImGui.
- `Tools.cppm` + `internal/Tools.cpp` - picking, brush, primitives, palettes, import, entity helpers. No ImGui.
- `Editor.cppm` + `internal/Editor.cpp` - `Editor` class: state, actions, input (`OnUpdate`), all ImGui/ImGuizmo code (`OnGui`).

**build**
- `../CMakeLists.txt` - game exe `Shredborne` (globs `src/*.cpp`, `src/*.cppm`), forces `VORTEX_BUILD_TESTS`.
- `../CMakePresets.json` - `dev` (`build/`, RelWithDebInfo), `debug` (`build-debug/`), `release` (`build-release/`, tests off).
- `CMakeLists.txt` - options, `vortex_options`, `vortex_warnings`, `vortex_add_module_library`, the five libraries (Core, Physics, Sim, Graphics, Engine),
  `vortex_assert_gpu_free` (configure-time check that Core, Physics, Sim and `VortexSimTests` link no renderer, window or UI library).
- `cmake/Dependencies.cmake` - every FetchContent dependency, pinned. `cmake/VortexShaders.cmake` - GLSL -> SPIR-V, `vortex_deploy_assets`.
- `tests/CMakeLists.txt` - `vortex_add_tests`: one test exe per `tests/<dir>`. `tools/CMakeLists.txt` - adds every subdirectory that has a CMakeLists.txt.

## 2. Invariants

### Job system (`src/core/internal/Jobs.cpp`)
- `Submit`, `PendingJobs`, `ThreadCount` are thread safe. Default thread count is `hardware_concurrency - 1`, at least 1 (`Jobs.cpp:61-64`).
- The destructor does **not** drop queued jobs: workers leave only when `stop` is set and the queue is empty (`Jobs.cpp:34-35`).
  Every queued job therefore runs, possibly after its owner is gone. In `sim::Simulation::Impl` (which holds what `Engine::Impl` held
  before M1 stage 1) `jobs` is declared before `world` / `terrain` (`src/sim/internal/Simulation.cpp:67-77`) and `Simulation::Shutdown`
  resets those first (`:114`), so load and cook jobs do run after `World` and `TerrainColliders` are destroyed.
- Hence the rule: a job captures a `shared_ptr` to state that outlives the owner, never `this` or a reference into the owner.
  Patterns to copy: `World::Shared` (`src/voxel/internal/World.cpp:25-44`, lambda at `:285`) and `TerrainColliders` `Shared` with a
  `cancelled` flag under a `shared_mutex` (`src/physics/internal/Terrain.cpp:88-100`, `:524`).
- Results go back through the owner's own mutex-protected vector and are integrated on the main thread (`World::IntegrateResults`).
  The pool returns nothing. Owners throttle themselves with an `inFlight` atomic against their own job limit
  (`World.cpp:279,284`; `Terrain.cpp:153` sets `maxJobs`, `:523` counts up). The job itself counts down as its last statement (`World.cpp:275`).
- A job must not wait for another job of the same pool. `ParallelFor` is the exception because the caller works too (`Jobs.cpp:130`).
- `WaitIdle` must never be called from inside a job: `running` cannot reach 0 while the caller is a running job (`Jobs.cpp:136-139`). Tests call it from the test thread.
- An exception escaping a `Submit`ted job is caught and logged (`Jobs.cpp:49-57`); the job's result is lost and the owner's `inFlight` is not decremented unless the job did it first.

### Profiler (`src/core/internal/Profiler.cpp`)
- Global state under one mutex; `AddSample` / `SetCounter` may be called from any thread. Lookup is a linear string compare per call.
- Samples with the same name within a frame are summed; `Profiler::EndFrame()` (main thread, once per frame, `Engine.cpp:281`) turns
  the sums into one history sample. A timer not touched in a frame records 0. Timers are never removed.
- Ring: `cursor` = next write index, `samples` = valid count (<= `kHistory` = 120). Newest sample is `history[(cursor - 1) mod 120]`.
  Both readers rely on this: the profiler panel (`src/editor/internal/Editor.cpp:793-794`) and the headless median (`Engine.cpp:302-309`).
- `ProfileScope` stores the `const char*`: pass a string literal.
- Naming convention in use: `"CPU: ..."` (`Engine.cpp:196,215,225`), `"GPU: <pass>"` (`src/graphics/internal/Renderer.cpp:1677`), `"Renderer::RenderFrame"` (`Renderer.cpp:1503`).

### Input (`src/core/Input.cppm`)
- Not thread safe; main thread only. `NewFrame()` must run before events are delivered: `Window::PollEvents` does it
  (`src/graphics/internal/Window.cpp:146`), a headless run does it in `Engine::Run` (`Engine.cpp:201`).
- Edges (`KeyPressed`, `MousePressed`, `*Released`) are true for one **frame**, not one fixed step.
- UI capture is set once per frame by `UiLayer::UpdateInputCapture` (`src/graphics/internal/Ui.cpp:250-252`, called at `Engine.cpp:211`)
  and masks `KeyDown/KeyPressed/MouseDown/MousePressed/ScrollY`. With ImGui input switched off (`Engine::SetImGuiInput(false)`, play
  mode) both capture flags are false. It does **not** mask `KeyReleased`, `MouseReleased`, `MouseX/Y`, `MouseDeltaX/Y`,
  `KeyDownRaw`, `MouseDownRaw`. The raw queries are for state that must not flip while an ImGui widget is active (the
  F3 overlay's held Left Alt: ImGui has the keyboard during a slider drag) and for `CursorCapture::Inputs::leftButtonDown`;
  never use them for actions.
- `Key` values are GLFW key codes, passed through unchanged (`Window.cpp:97-98`); GLFW repeat events are ignored there. Keys outside `[0, 512)` and buttons outside `[0, 8)` are dropped.
- Cursor units are window pixels, y down. `ResetCursorDelta` is called on every cursor-capture change (`Window.cpp:189`).

### HUD (`src/core/Hud.cppm`)
- `HudQuad` is mirrored by `VxHudQuad` (`shaders/include/common.glsl`); `BitmapFont::kGlyphWidth/Height` by
  `VX_HUD_CELLS_X/Y`. Both are contract-tested (`LayoutContract.HudQuadMatchesVxHudQuad`, `.HudCellGridMatchesTheFont`).
  Cell bit = row * 5 + column, row 0 at the top; at most 64 cells (two words).
- A glyph in `HudFont.inc` is 5-character rows of `.` / `#`, up to 9 rows (missing rows are empty), in ASCII order.
  `BuildGlyphs` is `consteval`: a wrong row length, a stray character or a character out of order is a compile error.
  `BitmapFont.EveryPrintableGlyphIsDistinctAndFitsTheGrid` fails if two glyphs are identical.
- HUD coordinates are framebuffer pixels; `Input::MouseX/Y` are window coordinates. Convert with
  `Engine::HudCursor()` (HiDPI / Wayland scaling), never compare them directly.
- Text and `PlaceRect` floor positions to whole pixels; quads of fractional size are allowed (rasterised by pixel centres).
- `Menu::HandleInput` order: Back wins, then a click (on an item: activate; elsewhere: nothing, and the frame's keys
  are ignored), then Up / Down, then Enter / Space. The pointer counts only once it has moved since `Open()` (or since a
  frame without `hasMouse`): a just-released cursor's position is stale. Hover takes the focus only on a frame with
  `mouseMoved`, so a resting pointer does not undo Up / Down. Call `Open()` when the menu is shown.
- `CursorCapture` owns the pause flag. Escape pauses only when not paused; resuming is the game's call (menu Back or
  Resume), so the game must pass `escape = false` on a frame the menu already handled. A capture starts only with
  `pointerInside` (Wayland locks on pointer entry) and is then kept while wanted. `Result::clicks` stays false after a
  capture until the left button has been up and `kClickGrace` (0.15 s) has passed: game clicks use it, look does not.

### Command line
- Engine flags are all in `EngineConfig::ApplyCommandLine` (`Engine.cpp:22-36`): `--headless --validation --frames N --screenshot F --width W
  --height H --scale S --ao-distance M --no-shadows --no-ao --no-emissive --no-taa --no-checkerboard`.
- Game flags are in `../src/main.cpp`: `--camera x,y,z[,yaw,pitch]` (`:481`, metres and radians), `--editor` (`:485`; with `--headless` it also opens every panel, `:488`).
- The screenshot is requested only on the last frame and only when `--frames` is non-zero (`Engine.cpp:270-271`).
- Headless uses `dt = fixedTimeStep` every frame (`Engine.cpp:206`): exactly one physics step per frame, reproducible. Return code 2 = validation errors (`Engine.cpp:314-316`).

### Particles (`src/fx/internal/Particles.cpp`)
- No locking anywhere: `ParticleSystem` is main-thread only. `BuildRenderData` is `const` but writes `sortScratch` (`:351`), so it is not safe to call concurrently either.
- Order in a frame (`Engine.cpp:243-253`): `app.OnUpdate` -> `particles.Update(dt, world.IsSolidAt)` -> `app.OnGui` -> gather -> `BuildRenderData(WindowOrigin, camera)`.
  Particles spawned in `OnUpdate` are simulated the same frame; anything spawned later in the frame (`OnGui`, or code run after rendering such as `scene.Update`) is first simulated next frame.
- Pool: SoA vectors sized to `capacity` at construction, `[0, alive)` live, swap-remove (`:117-133`). A new field needs a line in the `Impl` constructor, `Spawn` and `Remove`.
- Full pool: new particles are dropped, never the oldest; one warning per `ParticleSystem` lifetime (`:88-93`). Capacity comes from `EngineConfig::maxParticles` (32768).
- Units: positions `dvec3` world metres; velocity m/s; `size*` is the edge length in metres, the GPU gets the **half** size (`:371`); colours linear; `gravity` is a multiplier of the `Update` argument (-9.81).
- `Spawn` sanitises: non-finite position / velocity / lifetime or lifetime <= 0 is rejected; sizes, drag, emissive clamped >= 0; bounce clamped to [0,1] (`:95-112`).
- Determinism: `Randomise` always draws the same number of random values (`:137-173`); emitters run in slot order; the sort breaks ties by index (`:358-361`).
  Tested by `Particles.SameSeedAndCallsGiveIdenticalRenderData`. Any new random draw must be unconditional.
- Emitters: `EmitterId` = slot index + generation; `RemoveEmitter` bumps the generation and pushes the slot on a LIFO free list (`:282-288`);
  stale ids are ignored by every call. Emission that does not fit the pool is dropped, not owed (`:332-335`). `Clear()` removes particles, keeps emitters.
- `GpuParticle` (48 bytes) mirrors `VxParticle` in `shaders/include/common.glsl:108`; layout is tested by `LayoutContract.ParticleMatchesVxParticle`.
  Additive particles are sent with alpha 0, rgb pre-multiplied by opacity and emissive >= 1 (`:375-380`).
- Effect budgets are caps in code: `SpawnExplosion` <= 400 fire + 200 sparks + 200 smoke, radius clamped to [0.1, 8] m (`Effects.cpp:254-283`);
  `SpawnVoxelBreak` <= 12 debris + 4 dust (`Effects.cpp:303,309`). `Engine::PresentBlast` samples the carved voxels with stride `n / 400` (`Engine.cpp`).

### Editor
- Split: logic is free functions in `:view` / `:tools` (no ImGui, no GPU, unit tested); `Editor` adds input and widgets. ImGui / ImGuizmo headers appear only in `internal/Editor.cpp`.
- `Editor` is main-thread only and holds no engine pointer between calls. The game calls `OnUpdate` first in its `OnUpdate` (after
  its pause menu) and `OnGui` from its `OnGui`, then skips its own input while `CapturesInput()` (a headless run keeps the game's
  automation going even with the editor open). While the game's pause menu is open the game does not call `Editor::OnUpdate`
  (F1 and the tools do nothing); when the menu opens it calls `Editor::ReleaseInput`, which ends the right-button look (cursor
  released, the editor's `NoMouse` cleared), a brush stroke and the hover preview. A look after resume needs a new press.
- ImGui `NoMouse` has two writers: the editor (during its look, `noMouseSet`) and `UiLayer::SetInputEnabled` (play mode). Each
  clears only what it set: the UI layer remembers the bits it added (`m_AddedFlags`) and re-applies them at every `BeginFrame`
  while disabled; the editor sets `NoMouse` on every frame of its look, so a clear by the other owner is repaired next frame.
- Selection is a `scene::Entity` value validated on every read (`Editor::Selection`, `Editor.cpp:191-193`). Never cache component pointers across a call
  that can create or destroy entities (`Tools.cpp:330`, inspector buttons last at `Editor.cpp:700-706`).
- Every path that changes the selection or the tool, or closes the editor, sets `cancelGizmo` (`Editor.cpp:169,183,199`); `Impl::Gizmo` then resets ImGuizmo with `Enable(false/true)` (`:848-853`).
- Coordinates: cursor = ImGui pixels, y down; `CursorRay` origin is `dvec3` world metres; ImGuizmo gets `GizmoView` / `GizmoProjection` (world space, y up, 0..1 depth), never the engine's reverse-Z matrix.
  Voxel object `Transform.position` is the model's minimum corner, scale = voxel size; the gizmo pivot is half the model size in voxels (`Tools.cpp:321-325`).
- Clamps live in the logic, not the widgets: brush radius `[voxelSize, 10 m]` (`Tools.cpp:34-37`), spacing `[0.05, 4]` x radius and >= one voxel (`:116-119`), primitive size `[1, 256]` per axis (`:44-46`), reach >= 1 m (`Editor.cpp:324,410`).
- Actions (`DeleteSelection`, `SpawnPrimitive*`, `ImportFile`, `BrushAlong`, ...) need neither an open editor nor an ImGui frame; they report through `Status()`. The headless run depends on that.
- ImGui window names end in `##vortex.editor`; widget labels carry a `##<group>` suffix to stay unique.

### Build
- Layering is enforced by which directories each `vortex_add_module_library` call lists (`CMakeLists.txt:113,119,124,129,137`) and its link line,
  and for the GPU-free libraries by `vortex_assert_gpu_free` (`CMakeLists.txt:185-198`, `tests/CMakeLists.txt:35`): the configure step fails if
  `VortexCore`, `VortexPhysics`, `VortexSim` or `VortexSimTests` reach Graphics, Engine, Vulkan, GLFW, VMA or ImGui through any chain of link libraries.
- Everything importing engine modules must link a `Vortex::*` target so it inherits `vortex_options` (PUBLIC, `CMakeLists.txt:109`). `vortex_warnings` is PRIVATE to the libraries; tests add it themselves (`tests/CMakeLists.txt:22`).
- Dependencies are declared between the `CMAKE_CXX_SCAN_FOR_MODULES OFF` / restore lines (`Dependencies.cmake:18-19,208`).

## 3. Traps

- **`ParallelFor` and exceptions.** `fn` is captured by reference by helper jobs (`Jobs.cpp:110`). If `fn` throws on a worker the exception is swallowed, `done` never
  reaches `rangeCount` and the caller blocks forever (`:133`). If it throws on the calling thread, helpers keep a dangling reference. `fn` must not throw.
- **`ParallelFor` has no caller** outside core, and the particle simulation does not run on jobs although `Jobs.cppm:13-14` says so. There is no unit test for
  `JobSystem`, `Profiler`, `CommandLine` or `Input`; they are only exercised indirectly. Add tests before changing them (recipe 4.5).
- **`"CPU: frame"` lags one frame.** Its `ProfileScope` (`Engine.cpp:196`) ends after `Profiler::EndFrame()` (`:281`), so each sample lands in the next frame's sum. Harmless; do not "fix" it by moving `EndFrame` out of the loop body.
- **Renaming a GPU pass or the `"GPU: "` prefix** silently breaks the median in the headless log: `Engine.cpp:299-301` matches by string and falls back to the last frame with `over 0 frames`.
- **`Profiler::SetCounter` has no caller**; the editor's Counters section always shows "No counters" (`Editor.cpp:804`).
- **`Assert` is compiled out in the `dev` preset** (RelWithDebInfo defines `NDEBUG`); only `Verify` is always on. `VORTEX_DEBUG` (`CMakeLists.txt:55`) is defined in Debug but used nowhere in `src/`.
- **Input edges inside `OnFixedUpdate`.** The fixed loop runs 0..4 times per frame (`Engine.cpp:229`), so an edge read there is seen 0..4 times. Read edges in `OnUpdate` and latch them.
- **`KeyReleased` / `MouseReleased` ignore UI capture on purpose.** The editor's camera-look latch needs the release even when the cursor ends over a panel (`Editor.cpp:360-365`). Masking them would leave the cursor captured.
- **Mouse deltas are not masked** by UI capture. Gate them yourself (the editor uses `looking`, the game uses the right button).
- **The toggle key goes through `KeyPressed`** (`Editor.cpp:347`): F1 does nothing while an ImGui text field has the keyboard. Deliberate.
- **`CommandLine` is lenient.** Unknown or misspelt flags are ignored without a message. `--key value` is only read when the value does not start with `--` (`Args.cppm:39`).
  `GetInt` accepts trailing garbage (`12abc` -> 12). Negative values are cast to `uint32_t` (`--frames -1` = 4294967295, `Engine.cpp:25-28`). `GetFloat` uses `std::stof`
  (locale dependent, accepts `nan` / `inf`). Of the float flags only `--scale` is made safe downstream: the renderer clamps it to
  [0.25, 1] and treats non-finite as 1 (`Renderer.cpp:469-471`). The first occurrence of a repeated flag that carries a value wins.
- **`--no-checkerboard`** is implemented (`Engine.cpp:35`) but missing from the doc comment at `src/engine/Engine.cppm:51-52`.
- **`Paths::SetAssetRoot` is not thread safe** (`Paths.cpp:58-62`) and `AssetRoot()` returns a reference to the global: call it once at start-up before any job reads assets.
- **`Log::Write` reads `g_Logger` without the mutex** in its lazy-init check (`Log.cpp:85`). Engine start calls `Log::Init()` first (`Engine.cpp:92`); do not log from threads before that.
- **Particle collision is a point sample at the end position** (`Particles.cpp:312`), not a swept test: a particle moving more than a wall's thickness per frame passes through
  (rain at about 9 m/s moves 0.15 m per 60 Hz frame; a voxel is 0.1 m). Known weak spot, not a bug to patch locally.
- **`SolidQuery` cost.** One call per colliding particle per frame, up to five more when it hits (`Particles.cpp:206-230`). `World::IsSolidAt` is the callback (`Engine.cpp:244`); keep it cheap.
- **Embedded particles move freely** until they leave the solid (`Particles.cpp:203-209`); deliberate, tested by `Particles.EmbeddedParticlesTravelUntilTheyLeaveTheSolid`.
- **A full pool changes the random stream**: `Burst` stops drawing when the pool is full (`Particles.cpp:186-189`). Still deterministic for identical call sequences, but not comparable across capacities.
- **`BuildRenderData` sorts every live particle every frame** on the main thread (`Particles.cpp:358`); its cost at 32768 particles on the UHD 620 is not measured here. The renderer has no particle cap of its own: its buffer grows to
  whatever `BuildRenderData` returns (`Renderer.cpp:437-460,1183`), so the pool capacity is the only limit.
- **`AddEmitter` does not validate the recipe**; a non-finite one simply never emits (`Particles.cpp:328`, `:178-183`). Do not add "validation" that returns an invalid id unless callers are updated.
- **Parallel arrays in `Editor.cpp:39-44`** must match their enums by order: `kToolNames` <-> `Tool` + `kToolCount` (`Editor.cppm:19-24`), `kMotionNames` <-> `physics::MotionType`
  (`src/physics/PhysicsWorld.cppm:26`), `kDebugViewNames` <-> `graphics::DebugView` (`src/graphics/Frame.cppm:63`), brush / primitive names <-> `Tools.cppm:64-65,147`. `EnumCombo` clamps, so a mismatch shows the wrong label instead of crashing.
- **Tool shortcuts and the menu** assume tools are numbered from key `1` (`Editor.cpp:391-393,517`); the headless exercise cycles `frame % kToolCount` (`../src/main.cpp:251`).
- **`EditorPanels::SetAll`** (`Editor.cppm:59`) and the Panels menu (`Editor.cpp:523-529`) list the panels by hand. A panel missing from `SetAll` is not covered by the headless smoke run.
- **Brush spacing is measured against the re-cast hit on the edited terrain** (`Tools.cpp:143,153-155`), not between brush centres. Replacing it with a centre distance brings back drilling under a resting cursor (`EditorBrush.ARestingCursorAppliesOnceInsteadOfDrilling`).
- **The brush ignores hardness and the indestructible flag**: it calls `World::FillSphere/FillBox` (`Tools.cpp:107,111`), which take no
  `power`. Only `CarveSphere` / `CarveSphereQ` (used by `sim::Blast`, behind `Engine::Explode`) respects them. Since M1
  stage 2 `FillSphere` quantises centre and radius to 1/16 voxel and forwards to `FillSphereQ`, so the brush uses the
  integer sphere test too. Do not route gameplay damage through the brush functions.
- **`.vox` z offset uses the ceiling**, x and y the floor (`Tools.cpp:215-218`); looks inconsistent, is correct for the mirrored axis (`EditorImport.VoxModelsKeepTheirLayoutOnTheMirroredAxis`).
- **`ImportFile` can fail half way**: models created before running out of chunk slots stay, and the palette stays (`Editor.cpp:299-316`).
- **The documented editor smoke run stops too early.** `ExerciseEditor` acts at headless frames 10, 100, 110, 120, 130 (import), 150 (delete)
  and 160 (pick + the summary log line) (`../src/main.cpp:253-296`). `CLAUDE.md` runs `--frames 120 --editor`, which never reaches the
  import, the delete or the summary line. Use `--frames 170` when the change touches those. New exercise steps: keep them below the frame
  count you actually run, or raise the count in `CLAUDE.md` in the same commit.
- **Losing window focus fires every release edge.** `Window` calls `Input::Clear()` (`Window.cpp:123`), so all held keys and buttons report
  `KeyReleased` / `MouseReleased` on the next frame, unmasked by UI capture. Code that acts on a release (not only the editor look latch)
  must tolerate that.
- **Entities without a `RigidBody` cannot be picked** with the cursor (`Tools.cppm:54-55`); only the scene list selects them.
- **Inspector position edit teleports** (`SetTransform(..., true)`, `Editor.cpp:683`), the gizmo does not (`false`, `:868`); both zero the body velocity. Keep the difference.
- **`VORTEX_BUILD_TESTS` is overwritten with `FORCE`** from `SHREDBORNE_BUILD_ENGINE_TESTS` in the game build (`../CMakeLists.txt:10-11`); setting the engine option alone has no effect there.
- **`VORTEX_BUILD_TOOLS` is ON but `tools/` contains no tool** (only its `CMakeLists.txt`); the "CPU reference renderer" in the option text does not exist in this tree.
- **Source lists are globs** (`CONFIGURE_DEPENDS`): every `.cpp` / `.cppm` below a listed directory is compiled, wherever it sits. A stray scratch file in `src/` joins the library.
- **Editing any `shaders/include/*.glsl` or `shaders/shared/*.glsl` recompiles every shader** (`VortexShaders.cmake:74`); only top-level `*.vert/*.frag/*.comp` are entry points (`:54-58`, non-recursive).
- **`gtest_discover_tests` runs each test executable at build time** (`tests/CMakeLists.txt:27`): a test binary that crashes on start-up fails the build, not just ctest.

## 4. Recipes

**4.1 Add a command line flag.** Engine-wide: add the field to `EngineConfig` (`src/engine/Engine.cppm:24-49`) if needed, parse it in `ApplyCommandLine`
(`Engine.cpp:22-36`), add it to the doc comment at `Engine.cppm:51`. Clamp or validate at the parse site (see the unsigned casts above). Game-only: parse in
`../src/main.cpp` next to `--camera`. List it in `CLAUDE.md` if an assistant should use it for verification.

**4.2 Add a particle effect.** Declare the recipe in `src/fx/Effects.cppm`, implement in `internal/Effects.cpp` setting **every** `EmitterDesc` field like the existing ones;
run colours through `SafeColor`; any count that scales with input goes through `ScaledCount(base, perUnit, amount, cap)`. Extend `Effects.PresetsHaveFinitePositiveValues`
and `Effects.PresetsCarryTheDocumentedCharacter` in `tests/core/ParticleTests.cpp`, plus a bounded-count test for a `Spawn*` function. Check `docs/design/fx-budget.md` for the budget.

**4.3 Add a per-particle property.** `ParticleDesc` (`Particles.cppm:27-40`) -> SoA vector in `Impl` (resize in the constructor, copy in `Spawn` and `Remove`) -> use in `Update` or
`BuildRenderData`. If it reaches the GPU use the free `params.w` first; changing `GpuParticle` means changing `VxParticle` (`shaders/include/common.glsl:108`) and
`LayoutContract.ParticleMatchesVxParticle` together. Sanitise in `Spawn`. If it is randomised, draw unconditionally in `Randomise`.

**4.4 Add an editor tool or panel.**
- Logic first: a free function in `Tools.cppm` / `internal/Tools.cpp` taking `World` / `Scene` / `PhysicsWorld`, with tests in `tests/engine/EditorTests.cpp` using its `Fixture` (`:58`).
- Tool: add the enumerator to `Tool`, bump `kToolCount`, extend `kToolNames`, the shortcut block (`Editor.cpp:391-393`), the `switch` in `OnUpdate` (`:413`) and in `ToolsPanel` (`:550`); settings go in `EditorSettings`. Wrap it as an `Editor` action that sets `Status()`.
- Panel: add the flag to `EditorPanels` **and** `SetAll`, a `Begin/End` block in `OnGui` (`:467-503`) with `PlacePanel` and a `##vortex.editor` name, a menu item (`:523-529`), an `Impl` method.
- Call the new action from `ExerciseEditor` (`../src/main.cpp:250-299`) so the headless `--editor` run covers it (mind the frame count, see traps).

**4.5 Add a test.** Put `TEST(Suite, Name)` in an existing file, or a new `.cpp` under `tests/core` (links `VortexCore`), `tests/physics`, `tests/graphics` or `tests/engine`
(`tests/CMakeLists.txt:32-37`); new files are globbed. Includes first, then `import vortex.<module>;`. Suite and helper names must not collide with engine types.
Core utilities have no file yet: `tests/core/JobTests.cpp` etc. would be new. `VORTEX_TEST_SHADER_DIR` and `VORTEX_TEST_SOURCE_DIR` are predefined. Update the test count in `docs/ROADMAP.md` (329 after M1 stage 1).

**4.6 Add a module, a shader, a dependency.**
- Module partition: new `src/<dir>/Foo.cppm` with `export module vortex.<dir>:foo;`, add `export import :foo;` to the primary interface (e.g. `Core.cppm`), implementation in
  `src/<dir>/internal/Foo.cpp` starting with the global fragment and `module vortex.<dir>;`. No CMake edit (glob).
- New module directory: primary interface `src/<name>/<Name>.cppm` with `export module vortex.<name>;`, then add `<name>` to the right `vortex_add_module_library` call
  (`CMakeLists.txt:113-137`) according to the layering; add `export import vortex.<name>;` to `src/engine/Engine.cppm:14-21` if games should get it; add tests in the matching `tests/<dir>`.
  A module that a session server needs goes into `VortexSim` or below and must pass `vortex_assert_gpu_free`; `sim` was added this way in M1 stage 1.
- Shader: add `shaders/<name>.vert|.frag|.comp`; it is compiled to `<build>/vortex_assets/shaders/<name>.<ext>.spv` and copied to `<exe dir>/assets/shaders` by
  `vortex_deploy_assets`. Includes are relative to `shaders/` (`-I` is the shader directory, `VortexShaders.cmake:72`). A shader used by a pipeline must also be
  listed in `kShaderFiles` in `tests/graphics/ContractTests.cpp:124` or `ShaderContract.EveryPipelineShaderIsListed` fails. Loading it is the renderer's side (see `graphics.md` section 4).
- Dependency: `FetchContent_Declare` in `cmake/Dependencies.cmake` with a release tag (or a full commit hash plus a dated comment, like ImGuizmo at `:178`), switch its tests / examples /
  install off, and for libraries without usable CMake use `SOURCE_SUBDIR _vortex_headers_only_` plus an own `vortex_<name>` target with `SYSTEM` includes (`:64-73`). Link it `PRIVATE`
  to the lowest library that needs it. Compile its implementation in a plain non-module `.cpp`. A system package means one `sudo pacman -S` line for the owner.

## 5. Verifying a change here

Build and the two headless runs are in `CLAUDE.md`. What is specific to this area:

| Changed | Run | Look for |
|---|---|---|
| fx | `./build/engine/tests/VortexCoreTests --gtest_filter='Particles.*:Effects.*'` (30 tests) | all pass |
| `GpuParticle` / particle shaders | `VortexGraphicsTests --gtest_filter='LayoutContract.*:ShaderContract.*'` | `ParticleMatchesVxParticle` |
| editor logic | `VortexEngineTests --gtest_filter='Editor*'` (37 tests: `EditorView`, `EditorBrush`, `EditorPrimitive`, `EditorMaterials`, `EditorImport`, `EditorTools`, `EditorState`) | all pass |
| `Editor.cpp` widgets | headless run with `--editor --frames 170` (120 stops before import / delete / summary) | exit 0; log lines `Editor: ...` for each exercised action except the brush (not logged, `Editor.cpp:333`) and the final `Editor: N entities, last action '...'` without `(failed)` (`../src/main.cpp:293`) |
| jobs | `VortexCoreTests --gtest_filter='World.*'`, `VortexPhysicsTests --gtest_filter='TerrainColliders.*:TerrainGuard.*'` (the only users of `JobSystem`), then the headless game run | run ends (no hang at exit), `World: ... store a/b` line present |
| profiler | headless run | `GPU <pass> x ms (median y ms over N frames)` with N > 0 for every pass |
| input / command line | headless game run; flags by trying them | `Player feet at ..., on the ground`; a wrong flag gives no error, so check the effect |
| CMake, shaders, dependencies | fresh `cmake --preset dev` and full build | no warnings; `GLSL <name>` lines for a new shader; `ctest` count unchanged or higher |

- Test binaries are in `build/engine/tests/` (no output directory is set, so it follows the source tree); `ctest --test-dir build -R '^Particles\.'` works too.
- Particles in a screenshot: the automated blast happens at headless frame 90 (`../src/main.cpp`, `m_HeadlessFrame == 90`); use `--frames` a little above that to see fire and debris, and expect no
  `ParticleSystem: pool of ... is full` warning in the log of a normal run.
- Editor panels, gizmo, keyboard focus and cursor capture cannot be verified headless: say so and ask the owner.
