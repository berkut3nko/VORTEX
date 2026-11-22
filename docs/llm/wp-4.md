# [wp-4] Frame-rate block A and settings: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/perf|perf]] · [[../design/game-ui|game UI]]

| | |
|---|---|
| Stage | `[wp-4]` = `[perf-A]`: perf.md stages 1-3 (`[perf-M0]`, `[perf-P1]`, `[perf-G1]` + `[perf-G2]`) plus a HUD settings page |
| Design | [perf.md](../design/perf.md) "Details per item" (M0, P1, G1, G2), "Stages" rows 1-3, "How every item is measured"; the project review (game repository, `docs/history/reviews/project-review-2026-11-17.md`) section 4 "WP4" |
| Code read at | engine `bafca16`, game `0e27ea7` (branch `llm-tasks`, from `wip/nat-r0`) |
| Depends on | nothing in code; in the ROADMAP order it comes after `[nat-R1]` (only for the gate number of card 7) |
| Cards | 8 |
| Effort | 4-5 d (e) for the local model |
| Wire / rules / hashes | none: no `kProtocolVersion` bump, no `kPlayerRuleRevision` bump, no `buildHash` input. Shaders change (`trace.glsl`, `gbuffer_world.frag`): CPU and GPU together, as the contract wants |
| New gate | one, the per-machine GPU baseline (card 7): the next free number when card 7 lands, expected **G21** (G20 is `[nat-R1]`'s). The LLM guide's plan lists G21 for `[wp-7]`; card 7 moves the later planned gates up by one in the guide's table ("Values planned across the packs") in its commit |

## Before you start

- Committed: nothing is required in code. Check that you are on `llm-tasks` in both repositories and that the build is
  green (`DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` exits 0).
- What the code looks like today (the stage builds on these; `grep -n` them before a card):
  - `engine/src/engine/internal/Engine.cpp`: `Engine::Run` (the frame loop; `ProfileScope frameScope("CPU: frame")`,
    `ProfileScope scope("CPU: world")`, `ProfileScope scope("CPU: simulation")`; the exit statistics inside
    `if (s.config.exitAfterFrames != 0)`, ending with the `GPU {:<10} {:.2f} ms (median ...)` lines and
    `HUD: {} quads drawn on the last frame`), `EngineConfig::ApplyCommandLine`.
  - `engine/src/core/Profiler.cppm`: `vortex::Profiler` (120-frame history per timer), `ProfileScope`.
  - `engine/src/graphics/Renderer.cppm`: `RenderSettings` (`renderScale = 0.6f`, `bool vsync = true`, `debugView`),
    `RenderStats` (`passes`), `PassTiming`.
  - `engine/src/graphics/internal/Renderer.cpp`: `enum class Pass { GBuffer, Lighting, Composite, Particles, Taa,
    Present, Hud, Imgui, Count }`, `kPassNames`, `CollectTimings`, `BeginPass` / `EndPass`, `RenderFrame` (the
    `vkWaitForFences` on `frame.fence`, `vkCmdResetQueryPool`, `SyncVoxelData`, the submit with
    `waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`), `BuildFrameData` (edge fog in `u.jitter.zw`),
    `PrepareSurface` (`appliedVsync`).
  - `engine/src/graphics/internal/Swapchain.cpp`: `Swapchain::Recreate` (vk-bootstrap `SwapchainBuilder`,
    `set_desired_present_mode`, the `Swapchain: {}x{}, {} images, format {}, present mode {}` line printing a number).
  - `engine/src/graphics/internal/Resources.cpp`: `SyncForLayout`, the two `TransitionImage` overloads.
  - `engine/shaders/shared/trace.glsl`: `VxTraceWorld` (the line
    `ivec3 w = (((gridMin + cc) % gridSize) + gridSize) % gridSize;` inside the grid loop).
  - `engine/shaders/gbuffer_world.frag`: `VxTraceWorld(u_Frame.cameraPosVoxel.xyz, rd, 0.0, 1.0e9, hit)`.
  - `engine/shaders/composite.frag`: the edge fog (`if (u_Frame.jitter.w > 0.0) { float horizontal = length(rd.xz *
    t); ... smoothstep(u_Frame.jitter.z, u_Frame.jitter.w, horizontal) ...}`).
  - `src/main.cpp` (game): `ShredborneGame::OnUpdate` (pause menu: `m_PauseMenu`, `kMenuResume`, `kMenuQuit`),
    `DrawHud`, `DebugOverlay`, `kLookSensitivity`, `main` (`Log::Init(); Log::Info("Build: {}", ...)`).
  - `src/Session.cpp` (game): its own `m_PauseMenu`, `DrawHud` (top right `"%.0f fps"`), `RunSession`.
  - `engine/src/core/Hud.cppm`: `HudCanvas`, `Menu` (`AddItem`, `ClearItems`, `SetFocus`, `Open`, `Layout`,
    `HandleInput`, `Draw`), `MenuInput`, `MenuEvent`.
  - `tools/verify.sh`, `tools/lib/verify-gates.sh`, `tools/selftest-verify.sh`, `tools/tests/fixtures/verify/`.
- New `.cppm` / `.cpp` files and new test files are picked up by the CMake globs (`vortex_add_module_library`,
  `vortex_add_tests`): no CMake edit is needed for them. A new partition must still be added to its module's primary
  interface (`export import :runstats;` in `engine/src/core/Core.cppm`).
- New shader entry points (`*.frag`) are globbed too (`engine/cmake/VortexShaders.cmake`). This stage adds none.

## Decisions

Settled here (us, 2026-11-22; the owner may veto). Where perf.md or the review answers, that answer is taken.

1. **`RunStats` lives in `vortex.core`, partition `:runstats`, namespace `vortex`** (the core module's namespace, as
   `vortex::Profiler`). The LLM guide's `core::RunStats` means this class. It keeps raw `float` samples (exact
   percentiles, simple), at most `kMaxSamples = 1 << 20` per statistic (4 MiB; about 4.8 h at 60 fps); past the cap
   it still counts (`Count`, `Max`) but percentiles and `CountOver` cover the stored samples only.
2. **Percentiles are nearest-rank**: rank = ceil(p x n / 100) computed in integers as `(p * n + 99) / 100`, at least 1;
   the value is the rank-th smallest sample. p is an integer 0..100. Empty: 0.
3. **The `Perf:` line is printed at the end of every run**, windowed included, by `Engine::Run` after the loop
   (outside `if (exitAfterFrames != 0)`). Its exact format is in card 1. The GPU part lists the per-pass p50 over the
   whole run, in the renderer's pass order, without `upload`, and their sum; `upload` has its own field. The existing
   `GPU <pass> X ms (median Y ms over N frames)` lines (last 120 frames) stay unchanged: they are what is compared
   after "World: window filled".
4. **The fence wait is measured on its own** (`RenderStats::fenceWaitMs`, profiler timer `"Renderer: fence wait"`),
   and the `"Renderer::RenderFrame"` profiler scope starts after it.
5. **Present path (P1)**: `RenderSettings::vsync` is replaced by `RenderSettings::present`, a
   `PresentPreference { Fifo, FifoRelaxed, Immediate }`, default `FifoRelaxed` (perf.md question 1: FIFO_RELAXED
   preferred with a FIFO fallback). The engine picks the mode itself from the surface's list and hands vk-bootstrap
   exactly one: `Fifo` -> FIFO; `FifoRelaxed` -> FIFO_RELAXED, else FIFO; `Immediate` -> IMMEDIATE, else MAILBOX,
   else FIFO. Flags: `--no-vsync` (= `immediate`), `--present fifo|relaxed|immediate` (wins over `--no-vsync`). 3
   swapchain images asked for (`set_desired_min_image_count(3)`; the driver may give more).
6. **The submit waits for the acquired image at `COLOR_ATTACHMENT_OUTPUT`**, and the swapchain image's first
   transition (UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL) gets `COLOR_ATTACHMENT_OUTPUT` as its source stage (a new
   function `TransitionAcquiredImage`), so the layout change happens after the semaphore wait.
7. **G1**: one helper `VxEdgeFogTMax` in the shared `trace.glsl` (so CPU tests reach it through
   `voxel::EdgeFogTMax`); only the primary world ray of `gbuffer_world.frag` uses it. Objects, shadows, AO and light
   rays keep their ranges.
8. **G2**: the toroidal phase is computed once per call of `VxTraceWorld`; each step adds and wraps with one
   compare per axis, written with per-component ternaries (valid GLSL and C++).
9. **Game settings are engine code**: `vortex::GameSettings`, its parse / format and the HUD `SettingsPage` live in
   `vortex.core:settings` (`engine/src/core/Settings.cppm`), because the game has no test binary and the review
   wants GPU-free tests. The game only loads, applies and saves them.
10. **Settings values**: `renderScale` default 0.6 (perf.md question 2 is the owner's; 0.6 stays the default), the
    page cycles 0.5 -> 0.6 -> 0.75 -> 0.5; the file accepts 0.25..1.0. `vsync` on = `FifoRelaxed`, off = `Immediate`.
    `sensitivity` is a multiplier of the game's `kLookSensitivity`, default 1.0, file 0.25..4.0, page steps 0.5,
    0.75, 1.0, 1.25, 1.5, 2.0. `fovDegrees` is an integer, default 60 (today's `Camera::fovY` 1.0472 rad), file
    50..110, page steps 60, 70, 80, 90, 100.
11. **The settings file**: `$XDG_CONFIG_HOME/shredborne/settings.txt`, else `$HOME/.config/shredborne/settings.txt`;
    on Windows `%APPDATA%/Shredborne/settings.txt`; `--settings FILE` replaces the path. **Headless runs never read or
    write it** (gates must not depend on the owner's choices). Command-line flags win over the file.
12. **A log file per run**: `--log-file PATH` for any run; a windowed run without it writes
    `<executable dir>/logs/shredborne-<UTC yyyymmdd-hhmmss>.log` and keeps the 20 newest `shredborne-*.log` files
    there (older ones deleted, by name order). Headless runs write a file only with `--log-file`.
13. **GPU clock**: `gt_act_freq_mhz` is read every 60 frames from the first `/sys/class/drm/card*/gt_act_freq_mhz`
    found at start (sorted by path); missing file: one `GPU clock: not available` line and nothing more.
14. **`--debug-view NAME`** sets `RenderSettings::debugView` (`none albedo normal depth lighting steps motion
    material`); an unknown name is a `Log::Warn` and ignored (like `--voxel-size`).
15. **The HUD performance line in sessions**: the session's top-right `N fps` text becomes
    `N fps   cpu X ms   gpu Y ms` (card 6). Offline keeps `N fps` (F3 has the details).
16. **The GPU baseline gate (card 7)**: tier `full`, run alone (the alone lock exclusive, as G12 and G16), far
    (`--camera=-1,61,65,-0.241,-0.55`) and near (`--camera probe`) views, 1200 frames, no validation. A pass fails
    when its median is more than 5 % **and** more than 0.10 ms above the stored baseline; passes whose baseline is
    under 0.20 ms are not judged (noise). The first run on a machine records the baseline (a NOTE, not a failure);
    `VERIFY_GPU_BASELINE=record` records again (after an accepted change).
17. **Thread counts (card 8)**: measured only. perf.md T1b keeps hw-1 Jolt threads offline until a fight
    measurement; the new flags `--job-threads N` / `--physics-threads N` stay for that later decision. No default
    changes in this stage.
18. **Render scale 0.5 as the integrated-GPU default** stays open (the owner's look call, perf.md question 2): card 2
    takes the 0.5 / 0.6 screenshot pair for it.

## Skeleton

Card 1 lands the new files below with the bodies marked `// [wp-4] card N fills this` as stubs that compile and
return a neutral value, and every new test of the stage written in full, each disabled as `DISABLED_Name` until the
card that makes it pass renames it. Card 1 fills `RunStats` and `FormatPerfLine` for real. Changes to existing
declarations (`RenderSettings::vsync` -> `present`, `Swapchain::Create`) happen in their own cards, not here.

`engine/src/core/RunStats.cppm` (new; add `export import :runstats;` to `engine/src/core/Core.cppm`):

```cpp
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module vortex.core:runstats;

export namespace vortex {

    /// @brief Run-wide statistics of one timed quantity (ms): exact nearest-rank percentiles over the samples
    /// kept (at most kMaxSamples), plus count, max and budget counts. GPU-free; not thread safe.
    class RunStats {
    public:
        static constexpr uint64_t kMaxSamples = uint64_t{1} << 20;

        /// @brief Adds one sample in milliseconds. A non-finite value is ignored.
        void Add(double ms);
        /// @brief Samples added (also those past kMaxSamples).
        uint64_t Count() const;
        /// @brief Nearest-rank percentile, p in 0..100 (clamped), over the stored samples; 0 when empty.
        double Percentile(uint32_t p) const;
        /// @brief Largest sample ever added; 0 when empty.
        double Max() const;
        /// @brief Stored samples strictly greater than `ms`.
        uint64_t CountOver(double ms) const;
        void Clear();

    private:
        std::vector<float> m_Samples;
        uint64_t m_Count = 0;
        double m_Max = 0.0;
    };

    /// @brief What the `Perf:` exit line reports (Engine::Run fills it, one sample per frame).
    struct PerfSummary {
        RunStats cpuFrame;        ///< Wall time of one pass of the frame loop.
        RunStats cpuWorld;        ///< The "CPU: world" section.
        RunStats cpuSimulation;   ///< The "CPU: simulation" section.
        RunStats fenceWait;       ///< Renderer: the wait for the frame slot's fence.
        /// GPU passes in the order first seen (the renderer's pass order), "upload" included.
        std::vector<std::pair<std::string, RunStats>> gpuPasses;

        /// @brief The statistics of GPU pass `name`, appended at the end the first time.
        RunStats& GpuPass(std::string_view name);
        /// @brief nullptr if `name` was never added.
        const RunStats* FindGpuPass(std::string_view name) const;
    };

    /// @brief The `Perf:` line (card 1 gives its exact format). `presentMode`: "headless", "fifo", ...
    std::string FormatPerfLine(const PerfSummary& perf, std::string_view presentMode);
}
```

`engine/src/core/Settings.cppm` (new; add `export import :settings;` to `Core.cppm`; it imports `:hud`):

```cpp
module;

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

export module vortex.core:settings;

import :hud;

export namespace vortex {

    /// @brief The player's settings (docs: game-ui.md "Settings"). Saved as `key = value` lines.
    struct GameSettings {
        float renderScale = 0.6f;   ///< RenderSettings::renderScale; file 0.25..1.0
        bool vsync = true;          ///< true: PresentPreference::FifoRelaxed, false: Immediate
        float sensitivity = 1.0f;   ///< Multiplier of the game's look sensitivity; file 0.25..4.0
        int fovDegrees = 60;        ///< Vertical field of view; file 50..110
        friend bool operator==(const GameSettings&, const GameSettings&) = default;
    };

    /// @brief Parses `text`. On success writes `out` and returns true; on the first bad line returns false,
    /// leaves `out` unchanged and, if `error` is given, writes "line N: ...".
    bool ParseGameSettings(std::string_view text, GameSettings& out, std::string* error);
    /// @brief The file text: a comment line, then render_scale, vsync, sensitivity, fov (card 6 gives it).
    std::string FormatGameSettings(const GameSettings& settings);
    /// @brief The default file path (Decision 11); empty when no home directory is known.
    std::filesystem::path DefaultSettingsPath();
    /// @brief Reads and parses `path`; std::nullopt when it is missing or bad (logged as a warning).
    std::optional<GameSettings> LoadGameSettings(const std::filesystem::path& path);
    /// @brief Writes `settings` to `path` (parent directories created). false on failure (logged).
    bool SaveGameSettings(const std::filesystem::path& path, const GameSettings& settings);

    /// @brief The HUD settings page shown from the pause menu: one item per setting, Enter / click cycles it.
    class SettingsPage {
    public:
        enum Item : int { kRenderScale = 1, kVsync = 2, kSensitivity = 3, kFov = 4, kBack = 5 };
        struct Result {
            bool changed = false;   ///< A setting changed this frame (apply and save it).
            bool closed = false;    ///< Back or Escape: show the pause menu again.
        };

        void Open(const GameSettings& current);
        bool IsOpen() const { return m_Open; }
        void Close() { m_Open = false; }
        const GameSettings& Settings() const { return m_Settings; }
        /// @brief One frame of input (lays the page out for `screen` / `scale` first).
        Result HandleInput(const MenuInput& input, glm::vec2 screen, int scale);
        void Draw(HudCanvas& canvas, glm::vec2 screen, int scale);

    private:
        void Rebuild();             ///< Item labels from m_Settings, focus kept.
        Menu m_Menu;
        GameSettings m_Settings;
        bool m_Open = false;
    };
}
```

`engine/src/graphics/Swapchain.cppm` (additions only in card 1; `Create` / `Recreate` change in card 3):

```cpp
export namespace vortex::graphics {
    /// @brief What the player asked for; the swapchain picks a mode the surface offers (Decision 5).
    enum class PresentPreference : uint8_t { Fifo, FifoRelaxed, Immediate };
    /// @brief The first mode of the preference's order that `available` holds, else FIFO.
    VkPresentModeKHR ChoosePresentMode(PresentPreference preference, std::span<const VkPresentModeKHR> available);
    /// @brief "fifo", "fifo-relaxed", "immediate", "mailbox", else "other".
    const char* PresentModeName(VkPresentModeKHR mode);
    /// @brief "fifo", "relaxed", "immediate" (the --present values).
    const char* PresentPreferenceName(PresentPreference preference);
    /// @brief Parses a --present value; false for anything else.
    bool ParsePresentPreference(std::string_view text, PresentPreference& out);
}
```

`engine/src/voxel/Trace.cppm` (addition):

```cpp
    /// @brief The primary-ray end of perf.md G1, in voxel units: the ray parameter at which a ray of unit
    /// `direction` reaches the horizontal distance `radiusMetres`; 1.0e9 when radiusMetres <= 0
    /// (shaders/shared/trace.glsl VxEdgeFogTMax compiled as C++).
    float EdgeFogTMax(const glm::vec3& direction, float radiusMetres, float voxelSize);
```

New tests (file, suite, the card that enables them):

| Test | File | Card |
|---|---|---|
| `RunStats.PercentilesOfAKnownSeries`, `RunStats.CountsFramesOverABudget`, `RunStats.PerfLineNamesEveryPassInOrder` | `engine/tests/core/RunStatsTests.cpp` (new) | 1 |
| `Present.ChoosesThePreferredModeOrFifo`, `Present.NamesAndParsing` | `engine/tests/graphics/PresentTests.cpp` (new) | 3 |
| `Trace.EdgeFogTMaxIsTheHorizontalRadius` | `engine/tests/core/TraceTests.cpp` | 4 |
| `World.EdgeFogLimitEndsRaysAtTheHorizontalRadius` | `engine/tests/core/WorldTests.cpp` | 4 |
| `World.RaycastAfterAWindowMoveMatchesTheReference` | `engine/tests/core/WorldTests.cpp` | 5 (it passes before card 5 too: it guards the change) |
| `GameSettings.RoundTripsAndRefusesBadValues`, `GameSettings.UnknownKeysAndCommentsAreSkipped`, `SettingsPage.ActivatingCyclesThePresets`, `SettingsPage.BackClosesThePage` | `engine/tests/core/SettingsTests.cpp` (new) | 6 |
| The G21 judge's self-test cases | `tools/selftest-verify.sh` | 7 |

11 new `TEST(` macros in all.

## Card 1: RunStats and the M0 CPU timers with the `Perf:` exit line

| | |
|---|---|
| Goal | Every run, windowed included, ends with one `Perf:` line: CPU frame p50 / p99 / max, world and simulation sections, the fence wait, frames over 25 / 33 ms and the per-pass GPU medians, so a playtest leaves numbers. |
| Depends on | - |
| Effort | 1 d (e) |
| Touches | vortex.core (new partition), vortex.graphics (RenderStats, RenderFrame), vortex.engine (Run). Wire no, movement rule no, buildHash no |

**Read first** (about 45 KiB)
- `engine/src/core/Profiler.cppm` (whole, 2 KiB): the style of a core partition.
- `engine/src/core/Core.cppm` (whole).
- `engine/src/engine/internal/Engine.cpp`: `Engine::Run` (whole function), `struct Engine::Impl`.
- `engine/src/graphics/Renderer.cppm`: `RenderStats`.
- `engine/src/graphics/internal/Renderer.cpp`: `CollectTimings`, `RenderFrame` up to `CollectTimings(frame);`.
- `engine/docs/design/perf.md` "Details per item", the "M0 Measurement" paragraph.
- This pack's "Skeleton" (all of it: you land it).

**Files**
- new `engine/src/core/RunStats.cppm` (partition `vortex.core:runstats`), `engine/src/core/internal/RunStats.cpp`
  (`module;` includes `<algorithm> <cmath> <format> <string> <vector>`, then `module vortex.core;`).
- new `engine/src/core/Settings.cppm`, `engine/src/core/internal/Settings.cpp` (stubs, see "Skeleton").
- change `engine/src/core/Core.cppm`: `export import :runstats;` and `export import :settings;` (after `:hud`).
- change `engine/src/graphics/Swapchain.cppm` + `internal/Swapchain.cpp`: the four new functions as stubs
  (`ChoosePresentMode` returns `VK_PRESENT_MODE_FIFO_KHR`, names return `"fifo"`, `ParsePresentPreference`
  returns false). Add `#include <span>` / `<string_view>` to the global fragment if missing.
- change `engine/src/voxel/Trace.cppm` + `internal/Trace.cpp`: `EdgeFogTMax` stub returning `1.0e9f`.
- change `engine/src/graphics/Renderer.cppm`: `RenderStats` gets `float fenceWaitMs = 0.0f;` and
  `uint64_t timingSerial = 0;  ///< +1 each time CollectTimings read new pass times`.
- change `engine/src/graphics/internal/Renderer.cpp`: `CollectTimings`, `RenderFrame`.
- change `engine/src/engine/internal/Engine.cpp`: `Engine::Impl` (a `PerfSummary perf;` and
  `uint64_t lastTimingSerial = 0;`), `Engine::Run`.
- new `engine/tests/core/RunStatsTests.cpp`, `engine/tests/core/SettingsTests.cpp` (disabled),
  `engine/tests/graphics/PresentTests.cpp` (disabled); the disabled tests of cards 4-5 in `TraceTests.cpp` /
  `WorldTests.cpp`.

**Interfaces**: as in "Skeleton" (RunStats, PerfSummary, FormatPerfLine filled here).

**Steps**
1. `RunStats::Add(double ms)`: if `!std::isfinite(ms)` return. `++m_Count`; if `m_Count == 1 || ms > m_Max` then
   `m_Max = ms`; if `m_Samples.size() < kMaxSamples` push `static_cast<float>(ms)`.
2. `Count()` = `m_Count`. `Max()` = `m_Count ? m_Max : 0.0`.
3. `Percentile(p)`: `n = m_Samples.size()`; if n == 0 return 0. `p = min(p, 100)`.
   `rank = (uint64_t(p) * n + 99) / 100; if (rank < 1) rank = 1;` copy the samples into a local vector, run
   `std::nth_element(copy.begin(), copy.begin() + (rank - 1), copy.end())`, return `copy[rank - 1]` as double.
4. `CountOver(ms)`: count stored samples with `double(s) > ms` (strictly greater).
5. `Clear()`: all three members back to empty / 0.
6. `PerfSummary::GpuPass(name)`: linear search of `gpuPasses` by name; not found: `emplace_back(std::string(name),
   RunStats{})`; return the reference. `FindGpuPass`: linear search, nullptr if absent.
7. `FormatPerfLine(perf, presentMode)` builds exactly (each number `{:.2f}`, counts plain integers):

   ```
   Perf: {F} frames; CPU frame p50 {a} p99 {b} max {c} ms, world p50 {d} p99 {e} ms, simulation p50 {f} p99 {g} ms, fence wait p50 {h} p99 {i} ms; over 25 ms {j}, over 33 ms {k}; GPU sum p50 {S} ms ({pass} {x}, {pass} {x}, ...); upload p50 {u} ms; present {mode}
   ```
   F = `cpuFrame.Count()`; j = `cpuFrame.CountOver(25.0)`, k = `cpuFrame.CountOver(33.0)`. The GPU list walks
   `gpuPasses` in order, skips the entry named `"upload"`, writes `name p50` separated by `", "`; S is the sum of
   those p50 values. No GPU entry other than upload: `GPU sum p50 0.00 ms (none)`. u = p50 of `"upload"`, 0.00 when
   absent.
8. Renderer, `CollectTimings`: after the loop that fills `stats.passes`, `++stats.timingSerial;` (only on the path
   where `vkGetQueryPoolResults` returned `VK_SUCCESS`).
9. Renderer, `RenderFrame`: replace

   ```cpp
   ProfileScope profile("Renderer::RenderFrame");
   if (!PrepareSurface()) return false;
   ```
   so that the wait is timed alone: keep `if (!PrepareSurface()) return false;` first; around the existing
   `vkWaitForFences(dev, 1, &frame.fence, ...)` take `std::chrono::steady_clock::now()` before and after, set
   `stats.fenceWaitMs` (float ms) and `Profiler::AddSample("Renderer: fence wait", stats.fenceWaitMs)`; then put
   `ProfileScope profile("Renderer::RenderFrame");` on the line after the wait's error check. Set
   `stats.fenceWaitMs = 0.0f` at the top of `RenderFrame` so a skipped frame reports 0. Add `#include <chrono>` to the
   global fragment if missing.
10. Engine, `Run`: at the top of the `while` body (before `ProfileScope frameScope`) `const double frameStart =
    NowSeconds();`. Inside the world block take `NowSeconds()` at its first and last line and
    `s.perf.cpuWorld.Add(ms)`; the same for the simulation block (`s.perf.cpuSimulation`). After
    `s.renderer.RenderFrame(in);`:
    `s.perf.fenceWait.Add(s.renderer.Stats().fenceWaitMs);` and, when `Stats().timingSerial != s.lastTimingSerial`,
    store the serial and `s.perf.GpuPass(p.name).Add(p.milliseconds)` for every `p` of `Stats().passes`. Right
    before `if (lastFrame) break;` add `s.perf.cpuFrame.Add((NowSeconds() - frameStart) * 1000.0);`.
11. Engine, `Run`, after `app.OnShutdown(*this);` and **outside** `if (s.config.exitAfterFrames != 0)`, before the
    validation error count: `Log::Info("{}", FormatPerfLine(s.perf, s.config.headless ? "headless" : "window"));`.
    Card 3 replaces `"window"` with the swapchain's mode name.
12. Write the three RunStats tests (enabled) and all other new tests of the stage as `DISABLED_`.

**Tests**
- `RunStats.PercentilesOfAKnownSeries` (`engine/tests/core/RunStatsTests.cpp`): add 100.0, 99.0, ..., 1.0 (descending).
  Expect `Count() == 100`, `Percentile(0) == 1`, `Percentile(50) == 50`, `Percentile(90) == 90`,
  `Percentile(99) == 99`, `Percentile(100) == 100`, `Percentile(250) == 100` (clamped), `Max() == 100`. Then
  `Add(NAN)` leaves `Count() == 100`. A fresh `RunStats`: `Count() == 0`, `Percentile(50) == 0`, `Max() == 0`. After
  `Clear()` the first object is empty again.
- `RunStats.CountsFramesOverABudget`: samples 16.6, 25.0, 25.01, 33.0, 33.4, 50.0: `CountOver(25.0) == 4`,
  `CountOver(33.0) == 2`, `CountOver(100.0) == 0`.
- `RunStats.PerfLineNamesEveryPassInOrder`: cpuFrame 10, 20, 30, 40; cpuWorld 1, 2; cpuSimulation 3; fenceWait 0.5;
  `GpuPass("gbuffer").Add(12)`, `GpuPass("lighting").Add(7)`, `GpuPass("upload").Add(0.25)`. Expect exactly:
  `Perf: 4 frames; CPU frame p50 20.00 p99 40.00 max 40.00 ms, world p50 1.00 p99 2.00 ms, simulation p50 3.00 p99 3.00 ms, fence wait p50 0.50 p99 0.50 ms; over 25 ms 2, over 33 ms 1; GPU sum p50 19.00 ms (gbuffer 12.00, lighting 7.00); upload p50 0.25 ms; present fifo-relaxed`
  for `presentMode = "fifo-relaxed"`. And a default `PerfSummary` with `"headless"` gives
  `Perf: 0 frames; CPU frame p50 0.00 p99 0.00 max 0.00 ms, world p50 0.00 p99 0.00 ms, simulation p50 0.00 p99 0.00 ms, fence wait p50 0.00 p99 0.00 ms; over 25 ms 0, over 33 ms 0; GPU sum p50 0.00 ms (none); upload p50 0.00 ms; present headless`.

**Log lines and flags**
- `Perf: ...` (format above): read by the owner, cards 2 and 8, `[wp-8]` and `[fog-1]`. Not read by a gate yet.

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning.
2. `tools/ctest.sh -R 'RunStats\.' --output-on-failure` - 3 pass.
3. `tools/ctest.sh` - all pass (the new disabled tests are listed as disabled, not failed).
4. `tools/doc-check.sh --build build` - PASS.
5. `./build/Shredborne --headless --validation --frames 200 --screenshot /tmp/wp4-c1.png` - exit 0; the log ends with
   a `Perf: 200 frames; ...` line whose GPU list names gbuffer, lighting, composite, particles, taa, present, hud,
   imgui.
6. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS (the frame loop changed).

**Docs**
- `docs/verify.md` "Game (offline, headless)", the log-line list: add the `Perf:` line with its fields.
- `engine/docs/notes/graphics.md`, the item about end-of-run `GPU <pass>` lines: add the `Perf:` line and the
  `"Renderer: fence wait"` timer.
- ROADMAP "State": the test count (`ctest -N --test-dir build | tail -1`; GoogleTest cases = `TEST(` macros).

**Must not break**
- The existing `GPU <pass> X ms (median Y ms over N frames)` and `HUD: N quads drawn on the last frame` lines
  (G3's judge reads `HUD:`; card 7 reads `GPU`): unchanged text.
- `Profiler` behaviour (F3 and the editor's profiler panel read it).

**Done when**
- [ ] The three RunStats tests pass; every other new test of the stage exists as `DISABLED_` and compiles.
- [ ] A headless and a windowed run (owner) print one `Perf:` line at exit.
- [ ] verify quick PASS.

**Traps**
- `nth_element` reorders: work on a copy, `Percentile` is `const`.
- The frame scope ends after `Profiler::EndFrame()`; the RunStats sample is your own `NowSeconds()` difference, not
  the profiler's.
- `CollectTimings` reads the slot's results from two frames ago: only add GPU samples when `timingSerial` changed,
  or every value is counted twice.

## Card 2: M0 on the GPU side: upload timer, GPU clock, debug view, log file, baselines

| | |
|---|---|
| Goal | The chunk-upload copies get their own GPU timer, the GPU clock is logged, `--debug-view steps` shows the traversal cost, every windowed run leaves a log file, and perf.md records the far and near baselines this stage is judged against. |
| Depends on | card 1 |
| Effort | 1 d (e) |
| Touches | vortex.graphics (Pass enum), vortex.engine (Run, ApplyCommandLine), game `main.cpp`. Wire no |

**Read first** (about 35 KiB)
- `engine/src/graphics/internal/Renderer.cpp`: `enum class Pass`, `kPassNames`, `BeginPass` / `EndPass`,
  `RenderFrame` from `vkCmdResetQueryPool` to `SyncVoxelData(frame, input);`.
- `engine/src/graphics/Frame.cppm`: `enum class DebugView`.
- `engine/src/engine/internal/Engine.cpp`: `EngineConfig::ApplyCommandLine`, the frame loop.
- `engine/src/core/Log.cppm` and `internal/Log.cpp`: `Log::Init(LogLevel, const std::string& logFile)` (the file
  sink exists; it is only created on the first `Init`).
- `src/main.cpp`: `main` up to `Log::Info("Build: {}", ...)`.
- perf.md "How every item is measured".

**Files**
- change `engine/src/graphics/internal/Renderer.cpp`: `Pass` gets `Upload` before `Count`; `kPassNames` gets
  `"upload"` at the end (the array size follows `kPassCount`).
- change `engine/src/engine/Engine.cppm` (`EngineConfig`) and `internal/Engine.cpp`.
- change `src/main.cpp`.

**Steps**
1. Upload timer: in `RenderFrame`, right after `if (frame.queryPool != VK_NULL_HANDLE) vkCmdResetQueryPool(...)`,
   call `BeginPass(frame, Pass::Upload);`, and after `SyncVoxelData(frame, input);` call `EndPass(frame,
   Pass::Upload);`. Nothing else: `kQueryCount` follows `kPassCount`, `CollectTimings` loops over all passes, the
   exit lines print `GPU upload ...`, the `Perf:` line shows it in its own field (card 1 step 7).
2. `EngineConfig::ApplyCommandLine`: `--debug-view NAME`. Names in `DebugView` order: `none albedo normal depth
   lighting steps motion material`; match exactly (lower case) and set `render.debugView`; anything else:
   `Log::Warn("--debug-view expects none, albedo, normal, depth, lighting, steps, motion or material: '{}' ignored",
   name)`. Update the comment above `ApplyCommandLine` in `Engine.cppm`.
3. GPU clock, in `Engine::Impl`: `std::filesystem::path gpuClockFile; bool gpuClockChecked = false; RunStats
   gpuClock;`. In `Run` before the loop: list `/sys/class/drm/` entries named `card<digits>` (skip names with a
   `-`, those are connectors), sort by path, take the first whose `gt_act_freq_mhz` exists; none: `Log::Info("GPU
   clock: not available")`. Wrap every filesystem call in a `std::error_code` overload (no exceptions). In the loop,
   when `frame % 60 == 0` and the file is known: read one integer (an `std::ifstream`); on success
   `Log::Info("GPU clock: {} MHz (frame {})", mhz, frame)` and `gpuClock.Add(mhz)`. After the `Perf:` line, when
   `gpuClock.Count() > 0`: `Log::Info("GPU clock: {} samples, p50 {:.0f} MHz, min {:.0f}, max {:.0f}", count,
   p50, Percentile(0), Max())`. Read `gt_max_freq_mhz` once next to it and append `, gt_max {} MHz` when readable.
4. Log file (game `main.cpp`), before `Log::Init()`:
   - `--log-file PATH` given: pass it to `Log::Init(LogLevel::Info, path)`.
   - else, when `--headless` is absent and `--version` is absent: `dir = Paths::ExecutableDir() / "logs"`;
     `std::filesystem::create_directories(dir, ec)`; the name is `std::format("shredborne-{:%Y%m%d-%H%M%S}.log",
     std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()))` (UTC); then prune: list
     `shredborne-*.log` in `dir`, sort by file name ascending, while more than 19 remain remove the first (so the new
     one makes 20). Every call with `std::error_code`; a failure only logs a warning after `Log::Init` and the run
     goes on without a file.
   - After `Log::Info("Build: ...")` log `Log: writing PATH` when a file is used.
5. Baselines: run (after the build, the machine otherwise idle, on AC power) twice each, and write the medians down:
   - far: `./build/Shredborne --headless --frames 1200 --camera=-1,61,65,-0.241,-0.55`
   - near: `./build/Shredborne --headless --frames 1200 --camera probe`
   - scale pair for perf.md question 2: `./build/Shredborne --headless --validation --frames 600
     --camera=-1,61,65,-0.241,-0.55 --scale 0.5 --screenshot /tmp/wp4-scale-050.png`, and the same with `--scale 0.6`
     to `/tmp/wp4-scale-060.png`.
   Check in each log that `World: window filled in F frames` has F <= 1080. Record per view: gbuffer, lighting,
   composite, taa, present, upload medians (the `GPU ...` lines), the `Perf:` line's CPU p50 / p99 / max and frames
   over 25 / 33 ms.

**Tests**: none new (no logic beyond formatting; the flags are checked by the runs below).

**Log lines and flags**
- `GPU upload X ms (median Y ms over N frames)` (new pass in the existing format).
- `GPU clock: M MHz (frame N)` every 60 frames; `GPU clock: N samples, p50 M MHz, min A, max B[, gt_max C MHz]` at
  exit; `GPU clock: not available`.
- `Log: writing PATH`.
- Flags: `--debug-view NAME`, `--log-file PATH`.

**Checks**
1. Build - exit 0, no warning.
2. `tools/ctest.sh` - all pass.
3. `tools/doc-check.sh --build build` - PASS.
4. `./build/Shredborne --headless --validation --frames 200 --debug-view steps --screenshot /tmp/wp4-steps.png` -
   exit 0; look at the screenshot: a blue-green-red heat map of the terrain, not the normal picture.
5. `./build/Shredborne --headless --frames 200 --log-file /tmp/wp4-run.log` - exit 0; `/tmp/wp4-run.log` holds the
   same lines as the console, `Build:` first.
6. The baseline runs of step 5.
7. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS.

**Docs**
- `engine/docs/design/perf.md` "Where the frame goes today (measured)": add two rows dated today with the far and near
  medians (m), the build (`Build:` commits) and "headless, 1200 frames, medians of the last 120 after window filled".
  Add the CPU p50 / p99 under the table as a short paragraph. Put the two scale screenshots' paths in your report
  (they are not committed) and add to perf.md "Open questions" 2 one sentence: "Screenshot pair taken at `[wp-4]`
  card 2 (far view, 0.5 / 0.6); the owner's look decides."
- `docs/verify.md`: `--debug-view`, `--log-file`, the windowed default log folder, the `GPU clock:` lines, `GPU
  upload`.
- `engine/docs/notes/graphics.md`: the `upload` timer.

**Must not break**
- `ShaderContract.*`, `LayoutContract.*`: no shader changes here.
- The `GPU <pass>` line format (card 7 reads it).

**Done when**
- [ ] `GPU upload` appears in every headless run; `Perf:` shows `upload p50`.
- [ ] `--debug-view steps` shows the heat map; a windowed run leaves `build/logs/shredborne-*.log` (owner).
- [ ] perf.md has today's far / near baselines (m).

**Traps**
- `card0-eDP-1` and similar are connectors, not GPUs: take `card<digits>` only.
- `Log::Init` ignores a second call's file: the file must be passed on the first call in `main`.
- Do not put the log file under `build/` when `ExecutableDir` is elsewhere: it is wherever the binary is, by design.

## Card 3: P1 present path

| | |
|---|---|
| Goal | The windowed game no longer quantises to 30 fps: FIFO_RELAXED by default (FIFO where the driver lacks it), `--no-vsync` for measuring, 3 images, and the GPU starts on the next frame without waiting for the swapchain image; the `Swapchain:` line names the mode. |
| Depends on | card 1 |
| Effort | 0.5-1 d (e) |
| Touches | vortex.graphics (Swapchain, Renderer, Resources), vortex.engine (flags), vortex.editor (the VSync checkbox). Wire no |

**Read first** (about 40 KiB)
- `engine/src/graphics/Swapchain.cppm` (whole) and `internal/Swapchain.cpp` (`Create`, `Recreate`).
- `engine/src/graphics/internal/Resources.cpp`: `SyncForLayout`, `TransitionImage` (both); `Resources.cppm` their
  declarations.
- `engine/src/graphics/internal/Renderer.cpp`: `PrepareSurface`, the `Initialize` part that calls
  `swapchain.Create`, `RenderFrame` from `// --- present pass + screenshot` to `vkQueueSubmit2`.
- `engine/src/editor/internal/Editor.cpp`: the line `ImGui::Checkbox("VSync", &rs.vsync);`.
- perf.md "P1 Present path", "Why the windowed game can show 27 fps" hypotheses 1-2.

**Files**
- change `engine/src/graphics/Swapchain.cppm` / `internal/Swapchain.cpp`: fill the card-1 stubs; `Create(Device&,
  uint32_t, uint32_t, PresentPreference)` and `Recreate(uint32_t, uint32_t, PresentPreference)` replace the `bool
  vsync` versions; a member `VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;` and an accessor
  `VkPresentModeKHR PresentMode() const`.
- change `engine/src/graphics/Renderer.cppm`: `RenderSettings::vsync` removed, `PresentPreference present =
  PresentPreference::FifoRelaxed;` added (the Renderer partition must `import :swapchain;` if it does not see the
  enum; check the existing imports first); `Renderer` gets `const char* PresentModeLabel() const;` ("headless" when
  headless or no valid swapchain, else `PresentModeName(swapchain.PresentMode())`).
- change `engine/src/graphics/internal/Renderer.cpp`: `appliedVsync` -> `PresentPreference appliedPresent`; every
  `settings.vsync` use; the submit wait stage; the swapchain image transition.
- change `engine/src/graphics/Resources.cppm` / `internal/Resources.cpp`: new `TransitionAcquiredImage`.
- change `engine/src/editor/internal/Editor.cpp`: the VSync checkbox.
- change `engine/src/engine/internal/Engine.cpp` / `Engine.cppm`: flags, the `Perf:` present label.
- new tests enabled: `engine/tests/graphics/PresentTests.cpp`.

**Interfaces**

```cpp
// Resources.cppm (export namespace vortex::graphics)
/// @brief The first transition of an acquired swapchain image: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL with
/// COLOR_ATTACHMENT_OUTPUT as its source stage, so it is ordered after the image-acquired semaphore wait
/// (which the submit performs at COLOR_ATTACHMENT_OUTPUT).
void TransitionAcquiredImage(VkCommandBuffer cmd, VkImage image);
```

**Steps**
1. `ChoosePresentMode(preference, available)`: the order is `Fifo: {FIFO}`, `FifoRelaxed: {FIFO_RELAXED, FIFO}`,
   `Immediate: {IMMEDIATE, MAILBOX, FIFO}`; return the first of the order contained in `available`; if none,
   `VK_PRESENT_MODE_FIFO_KHR` (always supported by the spec).
2. `PresentModeName`: FIFO "fifo", FIFO_RELAXED "fifo-relaxed", IMMEDIATE "immediate", MAILBOX "mailbox", else "other".
   `PresentPreferenceName`: "fifo", "relaxed", "immediate". `ParsePresentPreference`: exactly those three strings.
3. `Swapchain::Recreate`: after the surface capabilities query, query the modes with
   `vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr)` then the list; on failure log an
   error and use `{VK_PRESENT_MODE_FIFO_KHR}`. `chosen = ChoosePresentMode(preference, modes)`. Replace the old
   `if (vsync) ... else ...` block with `builder.set_desired_present_mode(chosen);` and add
   `builder.set_desired_min_image_count(3);`. After a successful build set `m_PresentMode =
   swapchain.present_mode;`.
4. The log line becomes exactly
   `Swapchain: {w}x{h}, {n} images, format {f}, present mode {PresentModeName(m_PresentMode)} (asked {PresentPreferenceName(preference)})`.
5. Renderer: rename `appliedVsync` to `appliedPresent` (type `PresentPreference`, initial `FifoRelaxed`); replace
   every `settings.vsync` with `settings.present` (in `PrepareSurface`: `settings.present != appliedPresent`
   triggers the recreate as before).
6. Renderer, windowed branch of the present pass: replace
   `TransitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, layout);` with
   `TransitionAcquiredImage(cmd, image);` (layout stays `COLOR_ATTACHMENT_OPTIMAL` after it).
7. `TransitionAcquiredImage`: one `VkImageMemoryBarrier2` as `TransitionImage` builds it, with `srcStageMask =
   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT`, `srcAccessMask = VK_ACCESS_2_NONE`, `dstStageMask =
   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT`, `dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT`, `oldLayout = UNDEFINED`, `newLayout = COLOR_ATTACHMENT_OPTIMAL`, colour
   aspect, all mips and layers; `vkCmdPipelineBarrier2`.
8. Renderer, submit: `waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;` and replace the comment
   above it ("The whole command buffer waits ...") with: "Only colour output waits for the acquired image: gbuffer,
   lighting, composite and TAA run meanwhile. The image's first transition is ordered after this wait
   (TransitionAcquiredImage)." The signal stage stays `ALL_COMMANDS`.
9. Flags in `EngineConfig::ApplyCommandLine`: `if (args.Has("--no-vsync")) render.present =
   PresentPreference::Immediate;` then `--present NAME` through `ParsePresentPreference` (unknown: `Log::Warn` and
   ignored). Update the comment in `Engine.cppm`.
10. `Engine::Run`: the `Perf:` line's present label is `s.renderer.PresentModeLabel()` (replaces card 1's `"window"`).
11. Editor: `bool vsync = rs.present != PresentPreference::Immediate; if (ImGui::Checkbox("VSync", &vsync))
    rs.present = vsync ? PresentPreference::FifoRelaxed : PresentPreference::Immediate;`.
12. Enable and fill `PresentTests.cpp`.

**Tests**
- `Present.ChoosesThePreferredModeOrFifo` (`engine/tests/graphics/PresentTests.cpp`, `VortexGraphicsTests`): with
  `F = FIFO, R = FIFO_RELAXED, I = IMMEDIATE, M = MAILBOX`: `Choose(FifoRelaxed, {F, R}) == R`;
  `Choose(FifoRelaxed, {F, M, I}) == F`; `Choose(Fifo, {F, R, I, M}) == F`; `Choose(Immediate, {F, M, I}) == I`;
  `Choose(Immediate, {F, M}) == M`; `Choose(Immediate, {F}) == F`; `Choose(FifoRelaxed, {}) == F`.
- `Present.NamesAndParsing`: the four names and "other" for `VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR`;
  `ParsePresentPreference("relaxed")` gives `FifoRelaxed`; `"vsync"` and `""` return false.

**Log lines and flags**
- `Swapchain: WxH, N images, format F, present mode NAME (asked PREF)` (windowed only; the owner reads it).
- `Perf: ... present fifo-relaxed` (windowed) / `present headless`.
- Flags `--no-vsync`, `--present fifo|relaxed|immediate`.

**Checks**
1. Build - exit 0, no warning (the editor and every `vsync` use must compile: `grep -rn 'vsync' engine/src src`
   shows only the editor's local variable and comments).
2. `tools/ctest.sh -R 'Present\.' --output-on-failure` - pass; then `tools/ctest.sh` - all pass.
3. `tools/doc-check.sh --build build` - PASS.
4. `./build/Shredborne --headless --validation --frames 200 --screenshot /tmp/wp4-c3.png` - exit 0 (headless never
   acquires, but the renderer code paths compile and the validation layer stays clean).
5. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS.

**Docs**
- `docs/owner-checks.md` "Offline game": a new numbered check after the last one of that section: "Present path
  (`[wp-4]`): start `./build/Shredborne --validation` three times: default, `--present fifo`, `--no-vsync`. Each:
  read the `Swapchain:` line (mode and image count), play 60 s at the spawn and at a peak, note F3's fps, quit, and
  note the `Perf:` line (CPU p50 / p99, frames over 25 / 33 ms). Expected: exit 0 (validation clean); relaxed at or
  above FIFO's fps; no tearing with relaxed while on time." Renumber nothing that exists; append.
- `docs/verify.md`: the two flags.
- `engine/docs/notes/graphics.md`: in the frame steps, `PrepareSurface` reacts to a present change (not vsync); the
  submit wait at colour output and `TransitionAcquiredImage`.
- `README.md` (game) "Controls" stays as is (no key).

**Must not break**
- `LayoutContract.*`, `ShaderContract.*` (no shader change).
- Headless rendering (no swapchain): every change sits in `!headless` branches or in the swapchain.

**Done when**
- [ ] Present tests pass; no `vsync` member remains; headless runs unchanged.
- [ ] The owner check is in `docs/owner-checks.md`.

**Traps**
- vk-bootstrap silently falls back to FIFO when the desired mode is missing: always pass the mode you chose from the
  surface's list, and print `swapchain.present_mode` (what was built), never the preference.
- With the wait moved to colour output, a transition with `srcStage = NONE` would run before the image is acquired:
  the acquired image must use `TransitionAcquiredImage`.
- MAILBOX keeps rendering frames that are never shown: that is accepted for `--no-vsync` (measurement only).

## Card 4: G1: primary rays stop at the edge-fog radius

| | |
|---|---|
| Goal | The far view gets faster with the same picture: rays that would only show sky at the edge of the loaded window are not traced past the view radius. |
| Depends on | card 1 (the `Perf:` line to measure with) |
| Effort | 0.5 d (e) |
| Touches | shared shader code (`trace.glsl`, CPU and GPU), `gbuffer_world.frag`, vortex.voxel (one exported function). Wire no |

**Read first** (about 30 KiB)
- `engine/shaders/shared/trace.glsl`: the file header, `VxSafeDirection`, `VxTraceWorld` (whole).
- `engine/shaders/shared/vx_cpp.h` (the rules for shared code).
- `engine/shaders/gbuffer_world.frag` (whole).
- `engine/shaders/composite.frag`: the edge fog block at the end of `main`, and `SkyColor`.
- `engine/src/voxel/internal/Trace.cpp`: `kernel::TraceKernel`, `TraceWorld`.
- `engine/src/graphics/internal/Renderer.cpp`: the `edgeFog` block in `BuildFrameData` (`u.jitter = glm::vec4(jitter,
  edgeFog)`, `edgeFog = (0.85 R, R)` or 0).
- perf.md "G1 Edge-fog ray end".

**Files**
- change `engine/shaders/shared/trace.glsl`: new function after `VxSafeDirection`.
- change `engine/shaders/gbuffer_world.frag`.
- change `engine/src/voxel/internal/Trace.cpp`: fill `EdgeFogTMax`.
- change `engine/tests/core/TraceTests.cpp`, `engine/tests/core/WorldTests.cpp`: enable the two G1 tests.

**Interfaces**

```glsl
// trace.glsl (shared): the ray parameter, in voxel units, at which a ray of unit direction `rd` has gone
// `radiusMetres` horizontally; 1.0e9 for radiusMetres <= 0 (edge fog off).
float VxEdgeFogTMax(vec3 rd, float radiusMetres, float voxelSize) {
    if (radiusMetres <= 0.0f) return 1.0e9f;
    float horizontal = max(sqrt(rd.x * rd.x + rd.z * rd.z), 1.0e-6f);
    return radiusMetres / (voxelSize * horizontal);
}
```

**Steps**
1. Add `VxEdgeFogTMax` exactly as above (no swizzle: `rd.x`, `rd.z` are fine in both languages; `1.0e9f` and
   `1.0e-6f` carry `f`).
2. `gbuffer_world.frag`: replace the trace call with

   ```glsl
   // perf.md G1: past the edge-fog radius composite shows SkyColor anyway; such a ray is not traced there.
   float tMax = VxEdgeFogTMax(rd, u_Frame.jitter.w, u_Frame.cameraPosVoxel.w);
   if (!VxTraceWorld(u_Frame.cameraPosVoxel.xyz, rd, 0.0, tMax, hit)) {
       discard;
   }
   ```
   `jitter.w` is R in metres or 0 (edge fog off, fog off, or no world); `cameraPosVoxel.w` is the voxel size.
3. `voxel::EdgeFogTMax` in `Trace.cpp`: `static const TraceScene kNoScene{}; return kernel::TraceKernel{kNoScene}.
   VxEdgeFogTMax(direction, radiusMetres, voxelSize);` (the kernel's member functions see the shared text; if
   `TraceKernel` cannot be built from a `TraceScene` reference like that, stop and report the compiler error).
4. Measure (step "Checks" 5).

**Tests**
- `Trace.EdgeFogTMaxIsTheHorizontalRadius` (`engine/tests/core/TraceTests.cpp`): `EdgeFogTMax({1,0,0}, 92, 0.125)`
  == 736 (EXPECT_FLOAT_EQ); `EdgeFogTMax({0.6,0.8,0}, 92, 0.125)` within 1e-3 of 1226.667; `EdgeFogTMax({0,1,0}, 92,
  0.125)` == 92 / (0.125 x 1e-6) = 7.36e8 within 1e3; `EdgeFogTMax({1,0,0}, 0, 0.125)` == 1.0e9.
- `World.EdgeFogLimitEndsRaysAtTheHorizontalRadius` (`engine/tests/core/WorldTests.cpp`, its `Fixture` with defaults:
  voxel 0.5 m, flat ground at y = 20 m (kGround 40 x 0.5), window 8 x 8 x 8 chunks = 128 m): `LoadBlocking((0, 20,
  0))`. Origin (0, 30, 0) (10 m above ground); direction `normalize(1, -10/40, 0)` hits the ground 40 m away
  horizontally. With `maxDistance = EdgeFogTMax(dir, 50, 0.5) * 0.5` the `Raycast` hits (40 < 50); with `R = 35`
  it misses (40 > 35). Repeat with direction `normalize(0, -10/40, 1)`. (`World::Raycast` takes metres; the
  product converts voxel units back.)

**Log lines and flags**: none new.

**Checks**
1. Build - exit 0, no warning (glslang compiles the shader as part of the build).
2. `tools/ctest.sh -R 'Trace\.|ViewDistance\.|World\.' --output-on-failure` - all pass.
3. `tools/ctest.sh` - all pass. 4. `tools/doc-check.sh --build build` - PASS.
5. Measure, machine idle, on AC, twice each, before (checkout the previous commit's build? no: use card 2's recorded
   baselines) and after: far `./build/Shredborne --headless --frames 1200 --camera=-1,61,65,-0.241,-0.55` and near
   `--camera probe`: the gbuffer and lighting medians. Expected (e, perf.md): far gbuffer -0.5 to -1.5 ms, lighting
   -0.2 to -0.6 ms; near unchanged within 5 %.
6. Screenshots: `./build/Shredborne --headless --validation --frames 600 --camera=-1,61,65,-0.241,-0.55 --screenshot
   /tmp/wp4-g1-far.png` and the same with `--camera probe` to `/tmp/wp4-g1-near.png`. Compare with screenshots of
   the same commands from the build before this card (build the previous commit in a scratch copy only if card 2
   did not keep them: ask the owner instead of rebuilding). The far edge must look the same: horizon and fog band
   unchanged. If you cannot read images, say so.
7. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS.

**Docs**
- `engine/docs/design/perf.md`: in "Details per item", G1: one sentence "Built in `[wp-4]` card 4
  (`VxEdgeFogTMax`); measured: far gbuffer X -> Y ms, lighting X -> Y ms (m)".
- `engine/docs/notes/graphics.md`: G-buffer pass: the primary ray ends at R.
- ROADMAP: test count.

**Must not break**
- `Trace.SingleChunkMatchesReferenceDDA`, every `Trace.*` and `ViewDistance.*`, `World.GridEncodingMatchesShaderContract`
  (trace.glsl is shared: constructs valid in GLSL and C++ only).
- The edge fog itself (composite) is unchanged: pixels beyond R were already `SkyColor`.

**Done when**
- [ ] Both new tests pass; far medians measured and better; near within 5 %; screenshots look the same.

**Traps**
- R is a horizontal distance: dividing by the full ray length would cut steep rays short (holes in the ground under
  the camera). Divide by `length(rd.xz)` only.
- Do not apply it to `VxTraceScene` in `lighting.frag`: shadow and AO rays must still reach beyond R.
- Particles beyond R are no longer hidden by terrain there; check the far screenshot for floating dots at the
  horizon (expected none: nothing spawns there in a still view).

## Card 5: G2: one modulo per ray in the grid walk

| | |
|---|---|
| Goal | The world trace does less integer work per grid step: the far view gets a little faster with the identical picture. |
| Depends on | card 4 (measure on top of it) |
| Effort | 0.5 d (e) |
| Touches | shared shader code (`trace.glsl`). Wire no |

**Read first** (about 20 KiB)
- `engine/shaders/shared/trace.glsl`: `VxTraceWorld` (whole).
- `engine/tests/core/WorldTests.cpp`: `Fixture`, `ReferenceMarch`, `World.WindowSizeNeedNotBePowerOfTwo`.
- perf.md "G2 Grid addressing".

**Files**
- change `engine/shaders/shared/trace.glsl`: `VxTraceWorld`.
- change `engine/tests/core/WorldTests.cpp`: enable `World.RaycastAfterAWindowMoveMatchesTheReference`.

**Steps**
1. Write and enable the test first; it must pass on the old code.
2. In `VxTraceWorld`, after `ivec3 gridMin = VX_GRID_MIN_CHUNK;` add

   ```glsl
   // The toroidal phase of the window, once per ray: cell cc (always in [0, gridSize)) lives at
   // phase + cc, wrapped once (perf.md G2; the old form took six modulos per grid step).
   ivec3 phase = ((gridMin % gridSize) + gridSize) % gridSize;
   ```
3. Replace `ivec3 w = (((gridMin + cc) % gridSize) + gridSize) % gridSize;` with

   ```glsl
   ivec3 w = phase + cc;
   w.x -= (w.x >= gridSize.x) ? gridSize.x : 0;
   w.y -= (w.y >= gridSize.y) ? gridSize.y : 0;
   w.z -= (w.z >= gridSize.z) ? gridSize.z : 0;
   ```
   Keep the comment above it but change "(Positive modulo rather than a mask ...)" to "(The phase is a positive
   modulo, not a mask: the window size need not be a power of two.)".
4. Why it is exact: `phase` is in `[0, gridSize)` and `cc` in `[0, gridSize)` at every use (the start is clamped, a
   prism exit is clamped, a step out of range returns before the next use), so `phase + cc` is in
   `[0, 2 gridSize - 1)` and one subtraction wraps it. If you find a path where `cc` can leave the range before `w`
   is computed, stop and report it.

**Tests**
- `World.RaycastAfterAWindowMoveMatchesTheReference` (`engine/tests/core/WorldTests.cpp`): `Fixture f({}, {6, 5,
  12}, 512);` `LoadBlocking((-37, 20, 91))`, then `LoadBlocking((-37 + 16 x 7, 20, 91 - 16 x 5))` (the window moves
  in x and z across chunk coordinates that are not multiples of its size). `Rng rng(23)`; 400 rays: origin
  `(-37 + 112 + Range(-20, 20), 20 + Range(1, 15), 91 - 80 + Range(-40, 40))`, direction normalised `(Range(-1, 1),
  Range(-1, -0.2), Range(-1, 1))`, `Raycast(origin, dir, 30)` against `ReferenceMarch(..., 30, refT)`: same hit flag
  on every ray (ASSERT_EQ), distance within 0.05 m, at least 200 hits.

**Checks**
1. Build - exit 0, no warning. 2. `tools/ctest.sh -R 'Trace\.|ViewDistance\.|World\.' --output-on-failure` - all
pass. 3. `tools/ctest.sh` - all pass. 4. `tools/doc-check.sh --build build` - PASS.
5. Measure as card 4 (far and near, twice): gbuffer and lighting medians. Expected (e): far gbuffer -0.2 to -0.8 ms.
6. `MESA_SHADER_CACHE_DISABLE=true INTEL_DEBUG=fs ./build/Shredborne --headless --frames 3 2>&1 | grep -E
   'SIMD(8|16|32) shader'` before (card 4's build: run it before you edit) and after: note the instruction counts of
   the largest fragment shaders (gbuffer_world is one of the two biggest) and any change in spills.
7. Screenshots as card 4 step 6: the same picture.
8. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS.

**Docs**
- perf.md "Details per item", G2: "Built in `[wp-4]` card 5; measured: far gbuffer X -> Y ms (m); instructions A -> B".
- ROADMAP: test count.

**Must not break**
- `World.GridEncodingMatchesShaderContract`, `ViewDistance.ColumnSkipMatchesTheTraversalWithoutIt`,
  `World.WindowSizeNeedNotBePowerOfTwo`, `Trace.*`: the window stays toroidal (positive modulo) and column-major.

**Done when**
- [ ] All trace / world / view-distance tests pass; far gbuffer measured; the picture is the same.

**Traps**
- `%` of a negative number is negative in both languages: the phase needs the `+ gridSize) % gridSize` form once.
- `ivec3(bvec3)` conversions are not portable between glm and GLSL here: use the three ternaries.

## Card 6: Game settings and the HUD settings page

| | |
|---|---|
| Goal | The player sets render scale (0.5 / 0.6 / 0.75), vsync, mouse speed and field of view from the pause menu, they are kept between runs, and a session shows `fps / cpu / gpu` in the corner. |
| Depends on | card 3 (`PresentPreference`) |
| Effort | 1 d (e) |
| Touches | vortex.core (settings), vortex.engine (ApplySettings, LivePerformance), game `main.cpp`, `Session.cpp`. Wire no. No ImGui |

**Read first** (about 50 KiB)
- `engine/src/core/Hud.cppm`: `MenuInput`, `MenuEvent`, `Menu`, `HudCanvas` (public API only).
- `engine/tests/core/HudTests.cpp`: `HudMenu.KeyboardWrapsAndActivatesTheFocusedItem` (how a test drives a Menu).
- `engine/docs/design/game-ui.md` "API the game uses", "Input routing", "Rendering and cost".
- `src/main.cpp`: `ShredborneGame::OnInit` (the pause menu items), `OnUpdate` (the pause menu block and the mouse look
  using `kLookSensitivity`), `DrawHud` (the `if (paused)` block), `main` (where `config` is built and `Engine`
  created), `--pause-menu`.
- `src/Session.cpp`: the pause menu block in its update, `DrawHud` (top-right fps), `RunSession` (engine creation).

**Files**
- change `engine/src/core/Settings.cppm` / `internal/Settings.cpp`: fill the stubs.
- change `engine/src/engine/Engine.cppm` / `internal/Engine.cpp`: two functions.
- change `src/main.cpp`, `src/Session.cpp`.
- change `engine/tests/core/SettingsTests.cpp`: enable the four tests.

**Interfaces** (added to `Engine` in `Engine.cppm`)

```cpp
    /// @brief Applies the player's settings: render scale, present preference (vsync), camera field of view.
    /// Sensitivity is the game's (it scales its own mouse look).
    void ApplySettings(const GameSettings& settings);

    /// @brief What the session HUD shows: CPU frame (profiler average over its history, ms) and the GPU
    /// passes' last measured sum (ms).
    struct LivePerf { float cpuFrameMs = 0.0f; float gpuMs = 0.0f; };
    LivePerf LivePerformance() const;
```

**Steps**
1. `FormatGameSettings` writes exactly (`\n` line ends):

   ```
   # Shredborne settings (key = value)
   render_scale = 0.60
   vsync = on
   sensitivity = 1.00
   fov = 60
   ```
   (`{:.2f}` for the floats, `on` / `off`, the integer plainly.)
2. `ParseGameSettings(text, out, error)`: copy `out` to a local `s`; split on `\n` (strip a trailing `\r`); line
   numbers from 1. For each line: trim spaces and tabs; skip empty lines and lines starting with `#`. Find the first
   `=`; none: error "line N: expected key = value". Key and value trimmed. Keys: `render_scale` (float, finite, 0.25
   <= v <= 1.0), `vsync` (`on` or `off`), `sensitivity` (float, 0.25..4.0), `fov` (integer, 50..110, no fraction).
   Parse numbers with `std::from_chars` over the whole value (anything left over is an error). A bad value: error
   "line N: KEY: bad value 'V'" and return false without touching `out`. An unknown key: skip (newer files stay
   readable). End: `out = s`, return true.
3. `DefaultSettingsPath()`: Windows (`#ifdef _WIN32`): `%APPDATA%/Shredborne/settings.txt`; else
   `$XDG_CONFIG_HOME/shredborne/settings.txt` when set and non-empty, else `$HOME/.config/shredborne/settings.txt`;
   none of them: empty path. `LoadGameSettings`: missing file -> nullopt without a warning; read error or parse error
   -> `Log::Warn("Settings: {}: {}", path, error)` and nullopt. `SaveGameSettings`: `create_directories(parent, ec)`,
   write the text, false and a warning on failure. Every filesystem call with `std::error_code`.
4. `SettingsPage`:
   - `Open(current)`: `m_Settings = current; m_Open = true; m_Menu.SetTitle("SETTINGS");
     m_Menu.SetFooter("Enter / click changes   Esc back"); Rebuild(); m_Menu.Open();`
   - `Rebuild()`: `focus = m_Menu.Focus(); m_Menu.ClearItems();` add, in this order, with ids `kRenderScale ..
     kBack`: `std::format("Render scale   {:.2f}", renderScale)`, `std::format("VSync          {}", vsync ? "on" :
     "off")`, `std::format("Mouse speed    {:.2f}", sensitivity)`, `std::format("Field of view  {}", fovDegrees)`,
     `"Back"`; then `m_Menu.SetFocus(focus)`.
   - `HandleInput(input, screen, scale)`: if not open return {}. `m_Menu.Layout(screen, scale)`; `e =
     m_Menu.HandleInput(input)`. `Back` -> `m_Open = false`, `closed = true`. `Activated`: `kBack` -> as Back;
     `kRenderScale` -> next of {0.5, 0.6, 0.75}: the first preset greater than the current value + 0.001, else the
     first (0.5); `kVsync` -> toggle; `kSensitivity` -> next of {0.5, 0.75, 1.0, 1.25, 1.5, 2.0} the same way;
     `kFov` -> next of {60, 70, 80, 90, 100} (integers: the first greater, else 60). A change: `Rebuild()`,
     `changed = true`.
   - `Draw(canvas, screen, scale)`: if open, `m_Menu.Layout(screen, scale); m_Menu.Draw(canvas);`.
5. `Engine::ApplySettings(s)`: `renderer.Settings().renderScale = std::clamp(s.renderScale, 0.25f, 1.0f);`
   `renderer.Settings().present = s.vsync ? PresentPreference::FifoRelaxed : PresentPreference::Immediate;`
   `camera.fovY = glm::radians(static_cast<float>(std::clamp(s.fovDegrees, 50, 110)));`.
6. `Engine::LivePerformance()`: `cpuFrameMs` = the `avgMs` of the `Profiler::SnapshotTimers()` entry named
   `"CPU: frame"` (0 if absent); `gpuMs` = the sum of `renderer.Stats().passes[i].milliseconds` over every pass
   except `"upload"`.
7. Game `main.cpp`:
   - Members: `GameSettings m_Settings; SettingsPage m_SettingsPage; std::filesystem::path m_SettingsPath; bool
     m_SaveSettings = false;` and a setter `void SetSettings(const GameSettings&, std::filesystem::path path, bool
     save)`.
   - `main`: when not `--headless`: `path = --settings FILE` if given else `DefaultSettingsPath()`; `loaded =
     LoadGameSettings(path)`; settings = `loaded.value_or(GameSettings{})`. Headless: `GameSettings{}` and no path.
     Before `config.ApplyCommandLine(args)` set `config.render.renderScale = settings.renderScale` and
     `config.render.present = settings.vsync ? FifoRelaxed : Immediate` (so `--scale`, `--no-vsync` and `--present`
     still win). After `engine.Initialize()`: `engine.camera.fovY = radians(settings.fovDegrees)` and
     `game.SetSettings(settings, path, !headless)`.
   - Pause menu: items Resume (1), **Settings (3)**, Quit (2), in that order (`enum MenuId { kMenuResume = 1,
     kMenuQuit = 2, kMenuSettings = 3 }`). In `OnUpdate`'s paused block: if `m_SettingsPage.IsOpen()`, feed it
     instead of the pause menu: `r = m_SettingsPage.HandleInput(MenuInput::From(in, engine.HudCursor(), window !=
     nullptr), hud.Size(), hud.TextScale())`; `r.changed` -> `m_Settings = m_SettingsPage.Settings();
     engine.ApplySettings(m_Settings);` and, if `m_SaveSettings`, `SaveGameSettings(m_SettingsPath, m_Settings)`;
     `r.closed` -> `m_PauseMenu.Open()` (back to the pause menu, still paused). Else the pause menu as today, plus
     `Activated && id == kMenuSettings` -> `m_SettingsPage.Open(m_Settings)`. The cursor's Escape: the settings page
     consumes Escape as Back (it is inside the paused block, which already keeps Escape from the cursor:
     `capture.escape = !wasPaused && ...`).
   - When the game resumes (`m_Cursor.Resume()`), also `m_SettingsPage.Close()`.
   - Mouse look: `kLookSensitivity * m_Settings.sensitivity` in both lines.
   - `DrawHud`'s `if (paused)`: draw the settings page when it is open, else the pause menu.
   - New headless flag `--settings-menu`: like `--pause-menu`, and opens the settings page at start (for the
     screenshot check).
8. `Session.cpp`: the same pause-menu change (Resume, Settings, Leave the session), the same settings load in
   `RunSession` (not headless; the same path rules), the same sensitivity. Top-right text: `const Engine::LivePerf p
   = engine.LivePerformance();` and `std::format("{:.0f} fps   cpu {:.1f} ms   gpu {:.1f} ms", m_Fps, p.cpuFrameMs,
   p.gpuMs)` in place of the `"%.0f fps"` text.

**Tests** (`engine/tests/core/SettingsTests.cpp`, `VortexCoreTests`)
- `GameSettings.RoundTripsAndRefusesBadValues`: `{0.75, false, 1.5, 90}` -> Format -> Parse == the same struct;
  `GameSettings{}` -> Format equals the five-line text of step 1. Each of `render_scale = 2`, `render_scale = abc`,
  `vsync = yes`, `sensitivity = 0.1`, `fov = 49`, `fov = 70.5`, `fov 70` fails, leaves the struct unchanged, and the
  error starts with `"line 1: "` (for `fov 70`: `"line 1: expected key = value"`).
- `GameSettings.UnknownKeysAndCommentsAreSkipped`: text `"# x\n\n  colour = red\nfov = 80\r\n"` parses, `fovDegrees
  == 80`, the rest default.
- `SettingsPage.ActivatingCyclesThePresets`: `Open(GameSettings{})`; with the focus on item 0 (render scale), three
  `HandleInput` calls with `MenuInput{.activate = true}` give `renderScale` 0.75, 0.5, 0.6 and `changed == true`
  each time. Then `MenuInput{.down = true}` once and activate: vsync false. A page opened with `renderScale = 0.9`
  goes to 0.5 on activate.
- `SettingsPage.BackClosesThePage`: `MenuInput{.back = true}` gives `closed == true`, `IsOpen() == false`, settings
  unchanged; reopening, moving down 4 times to "Back" and activating does the same.
  (Layout needs a screen: pass `{1280, 720}` and scale 2.)

**Log lines and flags**
- `Settings: PATH: ERROR` (warning). Flags `--settings FILE`, `--settings-menu` (game).

**Checks**
1. Build - exit 0, no warning. 2. `tools/ctest.sh -R 'GameSettings\.|SettingsPage\.' --output-on-failure` - pass;
`tools/ctest.sh` - all pass. 3. `tools/doc-check.sh --build build` - PASS.
4. `./build/Shredborne --headless --validation --frames 60 --settings-menu --screenshot /tmp/wp4-settings.png` - exit
   0; look: a dimmed game with a "SETTINGS" panel of five items (Render scale 0.60, VSync on, Mouse speed 1.00, Field
   of view 60, Back).
5. `./build/Shredborne --headless --validation --frames 60 --pause-menu --screenshot /tmp/wp4-pause.png` - the pause
   menu shows Resume, Settings, Quit.
6. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS (G3's `HUD: N quads` still > 0).

**Docs**
- `engine/docs/design/game-ui.md`: a section "Settings" (the page, items, cycling, the file, headless never reads it,
  flags win) and the pause menu's new item; its "Tests" list gets the four test names.
- `README.md` (game) "Controls": Esc menu is "Resume / Settings / Quit" (session: "Resume / Settings / Leave the
  session"); a line on the settings file path.
- `docs/owner-checks.md`: a check "Settings page (`[wp-4]`): Esc, Settings, change each item with the mouse and the
  keyboard; render scale and FOV change at once, vsync recreates the swapchain (`Swapchain:` line), mouse speed
  changes the look; quit and restart: the values are kept (`~/.config/shredborne/settings.txt`). In a session the top
  right shows `fps / cpu / gpu`."
- `docs/verify.md`: `--settings`, `--settings-menu`.
- ROADMAP "State": Game UI row (the settings page exists; drop "no settings menu" from Gaps), the test count.

**Must not break**
- CLAUDE.md "HUD" contract: the game interface uses the HUD (`HudCanvas`, `Menu`); no ImGui for the page.
- Lesson L5 (ImGui gets no input in play mode): the page is HUD only, nothing changes `SetImGuiInput`.
- Lesson L6: the page uses `Menu::Draw` (which dims through `HudCanvas::DimBackground`), never a full-screen quad.
- Headless gates: headless runs never read the file, so G3-G10 see the defaults.

**Done when**
- [ ] Four tests pass; the two screenshots show the page and the menu; a windowed run keeps settings (owner).

**Traps**
- Rebuilding a `Menu` resets nothing but the items: keep the focus across `ClearItems` with `SetFocus`.
- The same Enter that opens the page must not also activate its first item: `Menu::Open()` and the pointer rule
  handle the mouse; for the keyboard, `HandleInput` sees the key on the next frame only if you return right after
  opening (do not call the page's `HandleInput` in the frame you opened it).
- Do not compare floats with `==` for the preset cycle: use the "+ 0.001" rule of step 4.

## Card 7: A per-machine GPU baseline gate for verify.sh

| | |
|---|---|
| Goal | A shader or renderer change that makes a pass more than 5 % slower on this machine fails `tools/verify.sh full` instead of being found weeks later. |
| Depends on | cards 2 and 4-5 (the baseline is taken on their result) |
| Effort | 1 d (e) |
| Touches | `tools/verify.sh`, `tools/lib/verify-gates.sh`, `tools/selftest-verify.sh`, fixtures, CLAUDE.md gate table. No engine code |

**Read first** (about 45 KiB)
- `tools/verify.sh`: the header comment, `QUICK_GATES` / `FULL_GATES` / `ALL_GATES`, `gate_tiers`, `gate_cmd`,
  `game_run`, `gate_G10` (a gate with several runs), `gate_G12` (an alone gate), the main loop's `if [ "$g" = G12 ]
  || [ "$g" = G16 ]` branch, `on_battery`.
- `tools/lib/verify-gates.sh`: the helper comment at the top, `need_exit`, `need_build`, `need_num`, `vg_note`,
  `vg_fail`, `vg_metric`, `_vg_where`, `judge_G12`.
- `tools/selftest-verify.sh`: `run_case`, the G12 cases.
- `docs/verify.md` "tools/verify.sh".
- CLAUDE.md section 2 (the gate table).

**Files**
- change `tools/verify.sh`, `tools/lib/verify-gates.sh`, `tools/selftest-verify.sh`.
- new `tools/tests/fixtures/verify/G21.log`, `tools/tests/fixtures/verify/G21-baseline.txt`.
- change CLAUDE.md (game) section 2: a G21 row; `docs/verify.md`; `engine/docs/llm/README.md` "Values planned across
  the packs", the Gates row.

**Steps**
1. Gate number: run `tools/verify.sh --list`; take the next free `G<number>` (expected G21). Use that number
   everywhere below in place of 21.
2. `verify-gates.sh`, new judge (same style as the others):

   ```bash
   # judge_G21 LOG EXIT BASELINE VIEW: the GPU medians of a 1200-frame run against the machine's baseline
   # (lines "VIEW PASS MEDIAN_MS"); a pass fails when more than 5 % and more than 0.10 ms slower; passes whose
   # baseline is under 0.20 ms are not judged. No baseline file or no line for VIEW: a note, nothing compared.
   judge_G21() {
       vg_begin G21 "$1" "$2"
       local base=$3 view=$4 v pass ms hit now frames
       need_exit 0
       need_build
       need_num "World: window filled in ([0-9]+) frames" "<=" 1080 "window filled by frame 1080 (medians are the last 120 frames)" fill_frames
       if ! grep -q "^$view " "$base" 2>/dev/null; then vg_note "no GPU baseline for $view in $base: nothing compared"; vg_result; return; fi
       while read -r v pass ms; do
           [ "$v" = "$view" ] || continue
           _vg_cmp "$ms" ">=" 0.20 || continue
           hit=$(grep -nE "GPU $pass +[0-9.]+ ms \(median [0-9.]+ ms over [0-9]+ frames\)" "$VG_LOG" 2>/dev/null | tail -1)
           if [ -z "$hit" ]; then vg_fail "missing: GPU $pass median line"; continue; fi
           now=$(printf '%s' "${hit#*:}" | sed -nE 's/.*\(median ([0-9.]+) ms over.*/\1/p')
           frames=$(printf '%s' "${hit#*:}" | sed -nE 's/.* over ([0-9]+) frames\).*/\1/p')
           vg_metric "${view}_$pass" "$now"
           if [ "$frames" != 120 ]; then vg_fail "$view $pass median over $frames frames, expected 120" "$(_vg_where "$hit")"; continue; fi
           if awk -v n="$now" -v b="$ms" 'BEGIN { exit !(n > b * 1.05 && n - b > 0.10) }'; then
               vg_fail "$view $pass median $now ms, baseline $ms ms: over +5 % and +0.10 ms" "$(_vg_where "$hit")"
           elif awk -v n="$now" -v b="$ms" 'BEGIN { exit !(n < b * 0.95 && b - n > 0.10) }'; then
               vg_note "$view $pass median $now ms, baseline $ms ms: faster (VERIFY_GPU_BASELINE=record keeps it)" "$(_vg_where "$hit")"
           fi
       done < <(grep -v '^#' "$base")
       vg_result
   }
   ```
3. `verify.sh`:
   - `FULL_GATES="$QUICK_GATES G13 G14a G14b G15 G16 G21 G18 G17"` (after G16, also an alone gate; before the
     short soaks); `gate_tiers`: G21 in `full`; `gate_cmd`: `G21) echo "Shredborne --headless --frames 1200
     --camera=-1,61,65,-0.241,-0.55, then --camera probe (alone; no validation); medians against the machine's GPU
     baseline";;`.
   - The alone branch of the main loop: `if [ "$g" = G12 ] || [ "$g" = G16 ] || [ "$g" = G21 ]; then`.
   - Environment, in the header comment and code: `VERIFY_GPU_BASELINE_DIR` (default
     `${XDG_STATE_HOME:-$HOME/.local/state}/vortex`), `VERIFY_GPU_BASELINE=record`.
   - New gate function:

     ```bash
     # The baseline file of this machine: the Vulkan device of the run's log and the host name.
     gpu_baseline_file() {   # gpu_baseline_file LOG
         local dev
         dev=$(sed -nE 's/.*Vulkan device: (.*) \(API.*/\1/p' "$1" | head -1 | tr -c 'A-Za-z0-9\n' '_')
         printf '%s/gpu-baseline-%s-%s.txt' "$GPU_BASELINE_DIR" "${dev:-unknown}" "$(uname -n | tr -c 'A-Za-z0-9\n' '_')"
     }
     record_gpu_baseline() {   # record_gpu_baseline LOG VIEW FILE: replaces VIEW's lines
         local log=$1 view=$2 file=$3 tmp
         mkdir -p "$(dirname "$file")" || return 1
         tmp=$(mktemp) || return 1
         { [ -s "$file" ] && grep -v "^$view " "$file" || echo "# GPU medians per view and pass (tools/verify.sh G21): $(uname -n), $(date '+%F')"
           sed -nE 's/.*GPU ([a-z]+) +[0-9.]+ ms \(median ([0-9.]+) ms over 120 frames\).*/'"$view"' \1 \2/p' "$log"; } > "$tmp"
         mv "$tmp" "$file"
     }
     gate_G21() {   # alone: the caller holds the alone lock exclusive
         G_LOG=$V/g21-far.log
         alone_held G21 || return 1
         local ok=0 view file
         for view in far near; do
             local log=$V/g21-$view.log
             if [ "$view" = far ]; then run "$(limit 300)" "$log" "$GAME" --headless --frames 1200 --camera=-1,61,65,-0.241,-0.55
             else run "$(limit 300)" "$log" "$GAME" --headless --frames 1200 --camera probe; fi
             local code=$RUN_EXIT
             file=$(gpu_baseline_file "$log")
             if [ "$code" = 0 ] && { [ "${VERIFY_GPU_BASELINE:-}" = record ] || ! grep -q "^$view " "$file" 2>/dev/null; }; then
                 record_gpu_baseline "$log" "$view" "$file"
                 vg_begin G21 "$log" "$code"; need_build; vg_note "GPU baseline for $view recorded: $file"; vg_result || ok=1
             else
                 judge_G21 "$log" "$code" "$file" "$view" || ok=1
             fi
         done
         if [ "$ok" != 0 ] && on_battery; then vg_note "the machine is on battery power: GPU medians are measured on AC"; fi
         G_LOG=$V/g21-far.log
         return $ok
     }
     ```
     Set `GPU_BASELINE_DIR=${VERIFY_GPU_BASELINE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/vortex}` next to
     `VERIFY_HISTORY`. Check how `run` sets `RUN_EXIT` and how `gate_G10` resets `VG_METRICS`; follow them.
4. Fixtures: run the far command once, take from its log the `Build:` line, the `Vulkan device:` line, the `World:
   window filled in ...` line and every `GPU ... (median ...)` line, in their order, into `G21.log`. Write
   `G21-baseline.txt` from the same values: a `#` line, then `far <pass> <median>` per pass.
5. Self-test cases (`tools/selftest-verify.sh`, next to the G12 ones):
   - `case_ok "G21 passes" G21 G21.log 0 "$FIX/G21-baseline.txt" far`
   - `case_bad "G21: lighting slower" G21 G21.log 0 "far lighting median 99.00 ms, baseline" "$FIX/G21-baseline.txt" far -- G21.log '/GPU lighting/s/median [0-9.]+ ms/median 99.00 ms/'`
   - `case_bad "G21: filled late" G21 G21.log 0 "window filled by frame 1080" "$FIX/G21-baseline.txt" far -- G21.log 's/window filled in [0-9]+ frames/window filled in 1100 frames/'`
   - `case_bad "G21: exit 2" G21 G21.log 2 "exit 2, expected 0" "$FIX/G21-baseline.txt" far`
   - `case_ok "G21: no baseline is a note" G21 G21.log 0 "$T/none.txt" far`
   Check the order of arguments in `run_case` (`case_bad NAME GATE FIXTURE EXIT MESSAGE_REGEX [JUDGE ARGS] [-- FILE
   SED]`) before you write them.
6. Docs (below), then run the checks.

**Log lines and flags**: none new in the game; the judge reads `Vulkan device: NAME (API ...`, `World: window filled
in F frames`, `GPU PASS X ms (median Y ms over N frames)`.

**Checks**
1. `tools/ctest.sh -R 'Tools\.' --output-on-failure` - `Tools.VerifyAssertions` (the new cases), `Tools.DocCheck`,
   `Tools.DocCheckSelftest` pass.
2. `tools/verify.sh --list` shows G21 in `full`; `tools/doc-check.sh --build build` - PASS (CLAUDE.md has the row).
3. `VERIFY_SLOT=1 tools/verify.sh full --only G21` twice: the first records (NOTE), the second compares and passes.
   Then `VERIFY_GPU_BASELINE_DIR=/tmp/wp4-base VERIFY_SLOT=1 tools/verify.sh full --only G21` records into the scratch
   folder (proves the variable).

**Docs**
- CLAUDE.md section 2, a row after G19 (and above any row a later stage added):
  `| G21 | `Shredborne --headless --frames 1200 --camera=-1,61,65,-0.241,-0.55`, then `--camera probe` (no validation; alone) | exit 0; window filled by frame 1080; every pass whose baseline is at least 0.20 ms within +5 % or +0.10 ms of this machine's baseline (`${XDG_STATE_HOME:-~/.local/state}/vortex/gpu-baseline-<device>-<host>.txt`; the first run records it, `VERIFY_GPU_BASELINE=record` records again after an accepted change); on AC power |`
- `docs/verify.md` "tools/verify.sh": the gate, the baseline file, the two variables, the full tier list.
- `engine/docs/llm/README.md` "Values planned across the packs", Gates row: G21 `[wp-4]` (the GPU baseline);
  `[wp-7]`'s 2v2 match G22; magic.md's proposed gates G23 `--siege-matrix` `[mag-1]`, G24 the spell drill `[mag-2]`.
  Say so in your report: later packs' cards that name G21-G23 for those gates now mean G22-G24.

**Must not break**
- CLAUDE.md section 5 "Verification": the judge, its fixture and its self-test change together; every check of the
  judge can fire (the planted faults).
- The other gates' order and ports; the alone lock is released (`vortex_alone_lock_try -s`) after G21 as after G12.

**Done when**
- [ ] `Tools.VerifyAssertions` passes with the new cases; G21 records then passes on this machine.

**Traps**
- No `--validation` in G21: the layer costs CPU time and changes nothing on the GPU, but the review's baseline is
  "no validation" (perf.md "How every item is measured"). Do not use `game_run`, which adds it.
- `need_num` fails a missing line: a run that never filled the window fails with that message, as intended.
- The baseline is per machine: never commit a real baseline file, only the fixture.

## Card 8: Close [wp-4]

| | |
|---|---|
| Goal | The stage is measured and recorded: the thread split, perf.md stages 1-3, ROADMAP's measured values, the owner's windowed run, and the full verify. |
| Depends on | cards 1-7 |
| Effort | 0.5-1 d (e) |
| Touches | vortex.engine (two flags), docs |

**Read first** (about 30 KiB)
- perf.md "Stages" (rows 1-3), "Why the windowed game can show 27 fps" item 3, "T1b Jolt threads".
- `engine/src/sim/Simulation.cppm`: `SimulationConfig::jobThreads`, `physics` (`physics::PhysicsConfig::threads`).
- `engine/src/engine/internal/Engine.cpp`: `Engine::Impl::SimulationConfigFor`, `ApplyCommandLine`.
- ROADMAP "Next" item 2, "State" Renderer row, "Performance and measured values" (table head and two rows).

**Files**
- change `engine/src/engine/Engine.cppm` / `internal/Engine.cpp`: `EngineConfig` gets `unsigned jobThreads = 0;`
  and `unsigned physicsThreads = 0;` (0 = the engine's default), flags `--job-threads N` and `--physics-threads N`;
  `SimulationConfigFor` copies them into `config.jobThreads` and `config.physics.threads` for an offline game; for a
  session (`c.simulation` set) copy them only when non-zero.
- docs listed below.

**Steps**
1. Add the two fields, flags (`args.GetInt`, negative -> ignored with a warning) and the copy.
2. Measure, idle machine on AC, two runs each, at 1200 frames:
   - A: defaults (jobs hw-1 = 7, physics hw-1 = 7).
   - B: `--job-threads 6 --physics-threads 2` (hw-2 and 2).
   For each: far `--camera=-1,61,65,-0.241,-0.55` and near `--camera probe`: the GPU medians, the `Perf:` line (CPU
   p50 / p99 / max, over 25 / 33 ms), `World: window filled in F frames (S s)`. And `./build/Shredborne --headless
   --validation --fly` plus `--climb` for both A and B: the gate lines (`max 0 unfilled columns inside 0.85 R`, `0
   unloads inside view radius`).
3. Decide nothing about the defaults (Decision 17). Record the numbers.
4. `VERIFY_SLOT=1 tools/verify.sh full --ci` - PASS. Then start `VERIFY_SLOT=1 tools/verify.sh soak` in the
   background and report its result when it ends.
5. `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage.

**Tests**: none new.

**Docs**
- perf.md: the header "Status" ("Stages 1-3 built in `[wp-4]`; ..."), "Next" (`[perf-B]`), the "Stages" rows 1-3 with
  **Built** and the measured result in "Accepted when" terms (far median change in ms, near change in %, the
  screenshots), and a short paragraph under "Why the windowed game can show 27 fps" item 3 with the A / B thread
  numbers (m). The owner's windowed result goes there once known.
- ROADMAP: "Next" item 2 removed (it moves to "Now" / done as the ROADMAP's own rules say: read "Now" first and
  follow its form); "State" Renderer row ("perf.md stages 1-3 built: M0, P1, G1, G2; block B next") and Game UI
  row; "Performance and measured values": rows dated today for far and near (GPU medians before card 4 from card 2 and
  after card 5), CPU p50 / p99, the thread comparison; the test count.
- `engine/docs/design/game-ui.md`: nothing new if card 6 did it.
- `docs/owner-checks.md` "Results": nothing yet; the checks of cards 3 and 6 wait for the owner. In your report ask
  the owner for: the three-mode windowed run (card 3's check) with the `Swapchain:` and `Perf:` lines, the settings
  page check, and the 0.5 / 0.6 screenshot look (perf.md question 2).
- `docs/verify.md`: `--job-threads`, `--physics-threads`.

**Must not break**
- Lesson L10 (`maxJobsInFlight` is per frame; 2 jobs per worker starved streaming): the thread flags change the
  worker count only, never `maxJobsInFlight`'s rule; B must still pass `--fly` and `--climb`.
- Lesson L8: Jolt thread count does not touch the guard.

**Done when**
- [ ] `verify.sh full --ci` PASS, soak started and reported; perf.md and ROADMAP updated; no `DISABLED_` test of the
  stage; the owner's questions are in the report.

**Traps**
- Streaming inflates GPU times 2-5x: compare only medians whose run logged `World: window filled` before frame 1080.
- Do not change the defaults even if B looks better: that is T1b's fight measurement (perf.md), not this stage.

## Values this stage lands

- No wire values, no `kProtocolVersion`, no `kPlayerRuleRevision`, no `buildHash` input: nothing for notes/net.md
  section 2.
- Gate: G21 (or the next free number) = the per-machine GPU baseline, tier `full`, alone. The LLM guide's Gates row
  moves `[wp-7]` to G22 and magic.md's to G23 / G24 (card 7).
- Shared names for later packs: `vortex::RunStats`, `vortex::PerfSummary`, `vortex::FormatPerfLine` (the `Perf:`
  line) in `engine/src/core/RunStats.cppm`; `vortex::GameSettings` / `SettingsPage` in
  `engine/src/core/Settings.cppm` (`[wp-5]` adds its boundary toggle as a new item: id 6, before Back, keeping Back
  last); `graphics::PresentPreference`.
