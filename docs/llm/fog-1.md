# [fog-1] Analytic fog extras: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/fog|fog]]

| | |
|---|---|
| Stage | `[fog-1]`: haze that glows toward the sun, a bounded ground mist per region style, and up to 8 blast-dust puffs (closed form, CPU-culled to screen rectangles, summed and capped), also on particles. No new pass. `--fog off` keeps today's shader byte for byte |
| Design | [fog.md](../design/fog.md) "Stages" 1, "Decision in brief" 7-9, "Density sources" (ground mist, blast dust, the cap, region style), "Rendering" "Stage 1", "Quality presets", "Contracts touched", "Headless flags"; ROADMAP "Next" [fog-1] |
| Code read at | engine `ea41341`, game `58d7b87`. `[wp-4]` ... `[wp-8]`, `[mag-1]`, `[mag-2]`, `[si-2]` land before this stage: find code with `grep -n`, never by line number |
| Depends on | `[wp-4]` (the `Perf:` line, pass medians, G21); `[wp-5]` (`FrameUniforms::playBounds`, the curtain in `composite.frag`); `[wp-6]` (`sim::EffectCue`, `sim::BlastCueOf`); `[wp-8]` (`FrameUniforms::effectLights`, the effect-light loop in `composite.frag`, the `--stress-fx` screen-grid helper) |
| Cards | 6: skeleton (0.5 d); the composite split (0.5 d); shared fog math (1 d); puffs, styles, uniforms and the puff buffer (1 d); the shaders (1 d); flags, sessions, measurement and close (1 d) |
| Wire / rules / hashes | none: presentation only. No `kProtocolVersion`, `kPlayerRuleRevision`, `buildHash`, `WorldHash` or `ChunkHash` change; `vortex_server` never calls fog code; no new gate. Graphics contracts change: `FrameUniforms` +6 members, buffer binding 11 `u_FogPuffs`, pipeline `composite_fog` |

## Before you start

- Every stage above is committed in both repositories (`git log --oneline | grep '\[wp-8\]'` and in `engine/`).
- What the code looks like at the start:
  - `engine/shaders/composite.frag`: sky path (`SkyColor(rd)` = sky + `ApplyFog(sky, rd, VX_SKY_DISTANCE)`), surface
    path (lit colour, `[wp-8]`'s effect-light loop, `ApplyFog(color, rd, t)`), `[wp-5]`'s curtain, the horizontal edge
    fog last (`u_Frame.jitter.zw`). `ApplyFog` calls `VxFogAmount` (`engine/shaders/shared/shading.glsl`: analytic
    exponential height fog, stable form "largest value times (1 - e^-|k|) / |k|").
  - `engine/shaders/particles.vert`: per vertex `VxFogAmount` from the camera to the particle.
  - `engine/src/graphics/Frame.cppm`: `FrameUniforms` (last member `effectLights` after `[wp-8]`), `FogSettings`
    (`color`, `density`, `heightFalloff`, `baseHeight`, `startDistance`, `maxOpacity`), `FrameInput`.
  - `engine/src/graphics/internal/Renderer.cpp`: `PipelineId::Composite` made by
    `fullscreen(PipelineId::Composite, "composite", "composite.frag.spv", {kHdrFormat})`, `PassComposite`
    (`BindPipeline(frame, PipelineId::Composite, ...)`), the buffer lookup by name (`if (name == "u_Lights") return
    &frame.lights;`), `BuildFrameData` (`u.fogParams = ... baseHeight - origin.y` - render space is relative to
    `World::WindowOrigin()`).
  - `engine/tests/graphics/ContractTests.cpp`: `LayoutContract.*`, the `BufferSpec` table, and the pattern of
    compiling shared GLSL as C++ inside a struct (`cube::ProxyCube` with `#include "shared/cube.glsl"`, vocabulary
    from `shared/vx_cpp.h`: `VX_CONST_FLOAT`, `VX_OUT`).
  - `engine/cmake/VortexShaders.cmake`: every `shaders/*.frag` is an entry point compiled to
    `build/vortex_assets/shaders/<name>.spv` (no `-g`); `shared/*.glsl` and `include/*.glsl` are dependencies.
  - Sessions: `net::Client::Impl` presents an applied operation only when complete (`if (present && out.complete)
    Present(...)`); `sim::BlastCueOf(const WorldOp&)` makes a `sim::EffectCue {kind, centreQ, radiusQ}`
    (`[wp-6]`); the game's `src/Session.cpp` feeds `engine.PresentBlast` from `m_Client.TakePresentations()`.
  - Sea level: `voxel::TerrainSettings::seaLevel` (offline: the game's terrain settings; session:
    `RegionDesc::terrain.seaLevel`).

## Decisions (us, 2026-11-23; the owner may veto)

1. **Default preset `low`** on every machine, offline and in sessions (open question 7). Stage 1 has no fog pass, so
   `low`, `medium` and `high` all run `composite_fog` and differ in nothing yet; `off` runs today's `composite.frag`.
2. **Region style in a session is `default`** for every region (open question 2: no wire field until W5). Offline
   `default`, or `--fog-style default|fen|clear`.
3. **Numbers** (fog.md "Density sources", tuned on screenshots by the owner later; record any change in fog.md):

   | Style | mist d0 (1/m) | mist k (1/m) | mist base | sigma_add_max (1/m) |
   |---|---|---|---|---|
   | `default` | 0.006 | 0.3 | seaLevel + 1 m | 0.017 |
   | `fen` | 0.02 | 0.3 | seaLevel + 1 m | 0.054 |
   | `clear` | 0 | 0.3 | seaLevel + 1 m | 0 |

   Mist start distance = the floor's (`FogSettings::startDistance`, 8 m). Sun lobe: phase g = 0.6, sun in-scatter
   scale 0.12 (`fogSun.xy`). Mist colour = the fog colour plus the lobe.
4. **Puffs**: 8 slots, oldest replaced (smallest birth time, ties lowest index). Lifetime 2.5 s. Radius r(t) =
   1.5 x blast radius x (1 + 0.6 x t / 2.5). Strength a(t) = min(t / 0.2, 1) x clamp(1 - (t - 0.5) / 2.0, 0, 1).
   Density sigma(t) = a(t) x 0.75 / r(t) (a fresh puff's centre chord 2r gives depth sigma x 4r / 3 = a). Colour:
   dust (0.42, 0.38, 0.33) linear, plus a warm glow (1.0, 0.55, 0.25) x (1 - t / 0.2) for t < 0.2 s. Summed depth
   capped at ln(1 / 0.35) = 1.0498 (centre T >= 0.35).
5. **Session puffs come from cues**, not presentations: the client keeps an `EffectCue` for every `CarveSphere` it
   applies with `present` (complete or not); the game feeds `Engine::AddFogPuff` from them and tells the engine
   (`SetFogPuffsFromCues(true)`) that `PresentBlast` must not add a second puff. Offline `Engine::PresentBlast` adds
   the puff (not for `deferred` results: no explosion, no puff).
6. **Puff time** is the engine's frame time (`FrameInput::time`): presentation only; a few frames of difference
   between clients are accepted.
7. **The settings page** gets no fog row in this stage (stage 2 adds it with the presets that differ).

## Skeleton

Card 1 lands these with stubs (`// [fog-1] card N fills this`) and every test as `DISABLED_`.

```glsl
// engine/shaders/shared/fog.glsl (new; GLSL / C++ common subset as trace.glsl; included after vx_glsl.glsl / vx_cpp.h)
VX_CONST_FLOAT(VX_FOG_PUFF_MAX_DEPTH, 1.0498f)   // ln(1 / 0.35)
VX_CONST_INT(VX_MAX_FOG_PUFFS, 8)
/// Henyey-Greenstein phase, normalised over the sphere (1/sr).
float VxFogPhase(float cosTheta, float g);
/// Optical depth of the clamped mist d0 * e^(-k * max(y - base, 0)) along ro + t rd, t in [start, dist].
float VxFogMistDepth(vec3 ro, vec3 rd, float dist, float d0, float k, float base, float start);
/// Optical depth along ro + t rd, t in [0, tMax], of a sphere with density sigma * (1 - s^2 / r^2) inside.
float VxFogPuffDepth(vec3 ro, vec3 rd, float tMax, vec4 centerRadius, float sigma);
```

```cpp
// engine/src/fx/Fog.cppm (new partition vortex.fx:fog; add `export import :fog;` to Fx.cppm)
export namespace vortex::fx {
    /// @brief One puff on the GPU (std430, 48 bytes). Mirrors VxFogPuff.
    struct GpuFogPuff {
        glm::vec4 centerRadius{0.0f};   ///< xyz render space (m), w radius (m)
        glm::vec4 colorDensity{0.0f};   ///< rgb linear colour, w sigma (1/m)
        glm::vec4 pixelRect{0.0f};      ///< x0, y0, x1, y1 in render pixels (gl_FragCoord), x1 <= x0: none
    };
    static_assert(sizeof(GpuFogPuff) == 48);

    enum class FogStyleId : uint8_t { Default = 0, Fen = 1, Clear = 2 };
    struct FogStyle {
        float mistDensity = 0.0f, mistFalloff = 0.3f, mistBaseAboveSea = 1.0f, addMax = 0.0f;
    };
    /// @brief Decision 3's table.
    FogStyle FogStyleOf(FogStyleId id);
    /// @brief "default" / "fen" / "clear"; std::nullopt otherwise.
    std::optional<FogStyleId> ParseFogStyle(std::string_view name);
    std::string_view FogStyleName(FogStyleId id);

    /// @brief Blast-dust puffs (fog.md "Blast dust", stage 1): 8 slots, closed form, presentation only.
    class FogPuffs {
    public:
        static constexpr uint32_t kSlots = 8;
        static constexpr float kLifetime = 2.5f;
        /// @brief A puff for a blast of `blastRadius` (m) at `center` (world), born at `time` (s).
        void Add(const glm::dvec3& center, float blastRadius, double time);
        /// @brief Frees puffs older than kLifetime at `time`.
        void Update(double time);
        uint32_t Alive() const;
        uint32_t MaxAlive() const;   ///< most alive after any Add or Update since construction
        uint64_t Added() const;
        uint64_t Replaced() const;   ///< Adds that took the oldest live slot
        /// @brief GPU records of the live puffs that may be on screen, oldest first. `viewProj`: render space
        /// -> clip without jitter; `cameraRender`: camera in render space; `renderSize` in pixels.
        void Build(const glm::dvec3& renderOrigin, const glm::mat4& viewProj, const glm::vec3& cameraRender,
                   glm::vec2 renderSize, double time, std::vector<GpuFogPuff>& out) const;
    private:
        struct Slot { glm::dvec3 center{0.0}; float blastRadius = 0.0f; double born = 0.0; bool live = false; };
        std::array<Slot, kSlots> m_Slots{};
        uint32_t m_MaxAlive = 0; uint64_t m_Added = 0, m_Replaced = 0;
    };
}
```

```cpp
// engine/src/graphics/Frame.cppm, FrameUniforms, appended after effectLights (and the same in common.glsl):
glm::vec4 fogSun{0.0f};     ///< x phase g, y sun in-scatter scale, z ambient scale (stage 2), w fog top (stage 2)
glm::vec4 fogMist{0.0f};    ///< x d0 (1/m), y k (1/m), z base (render space y), w start (m)
glm::vec4 fogLocal{0.0f};   ///< x sigma_layer, y dust scale, z sigma_cave (stage 2), w sigma_add_max
glm::vec4 fogMap{0.0f};     ///< stage 2: window min xz (render space), size xz (m)
glm::vec4 fogWind{0.0f};    ///< stage 3: wind xy (m/s), z sim blend, w noise scale
glm::ivec4 fogCounts{0};    ///< x puffs, y segments (stage 2), z preset (0 off .. 3 high), w 0
// the static_assert grows by 6 * 16 bytes.

// FogSettings, appended:
float mistDensity = 0.0f, mistFalloff = 0.3f, mistBase = 1.0f;   ///< world y of the mist base
float phaseG = 0.6f, sunScatter = 0.0f, addMax = 0.0f;
// FrameInput, after particles:
std::span<const fx::GpuFogPuff> fogPuffs;   ///< at most 8 (FogPuffs::Build)

// engine/src/graphics/Renderer.cppm:
enum class FogPreset : uint8_t { Off = 0, Low = 1, Medium = 2, High = 3 };
// RenderSettings, appended:
FogPreset fog = FogPreset::Low;   ///< Off: composite.frag (today's); else composite_fog.frag

// engine/src/engine/Engine.cppm, class Engine:
void SetFogStyle(fx::FogStyleId style, float seaLevel);     ///< fills FogSettings' mist fields from the table
void AddFogPuff(const glm::dvec3& center, float blastRadius);
void SetFogPuffsFromCues(bool fromCues);                    ///< session: PresentBlast adds no puff
const fx::FogPuffs& FogPuffs() const;

// engine/src/net/Client.cppm, class Client:
/// @brief The cues of every CarveSphere applied with presentation on since the last call, complete or not.
std::vector<sim::EffectCue> TakeBlastCues();
// engine/src/sim/Blast.cppm (or Rules.cppm beside BlastCueOf):
/// @brief World centre (m) of a blast cue: centreQ / 16 voxels x voxelSize, as voxel::QuantizeSphere inverts.
glm::dvec3 CueCentre(const EffectCue& cue, float voxelSize);
float CueRadius(const EffectCue& cue, float voxelSize);
```

Tests of the stage:

| Test | File | Card |
|---|---|---|
| `FogShading.PhaseIntegratesToOne` | `engine/tests/graphics/FogShadingTests.cpp` (new) | 3 |
| `FogShading.MistBelowItsBaseIsBounded` | same | 3 |
| `FogShading.MistMatchesNumericIntegral` | same | 3 |
| `FogShading.PuffMatchesNumericIntegral` | same | 3 |
| `FogShading.OverlappingPuffsKeepTheCap` | same | 3 |
| `FogPuffs.OldestIsReplacedWhenFull` | `engine/tests/core/FogPuffTests.cpp` (new) | 4 |
| `FogPuffs.ExpireAfterTheirLifetime` | same | 4 |
| `FogPuffs.RectangleContainsTheSphere` | same | 4 |
| `FogPuffs.BehindTheCameraIsCulled` | same | 4 |
| `LayoutContract.FogPuffMatchesVxFogPuff` | `engine/tests/graphics/ContractTests.cpp` | 4 |
| `LayoutContract.FrameUniformsMatchTheGlslBlock` (existing, new members) | same | 4 |
| `Blast.CueCentreIsTheBlastCentre` | `engine/tests/sim/BlastTests.cpp` | 4 |

## Card 1: Skeleton

| | |
|---|---|
| Goal | Declarations compile with stubs; the tests exist, disabled |
| Effort | 0.5 d (e) |
| Touches | Core (fx), Graphics (no `FrameUniforms` change yet), Engine, Net, Sim; wire: no |

**Steps**
1. Add every C++ declaration of "Skeleton" except the `FrameUniforms` members (card 4). `fog.glsl` with the three
   functions returning 0 (not included by any shader yet).
2. Stubs return neutral values (`FogStyleOf` returns the `clear` row; `Build` clears `out`; `TakeBlastCues` returns
   `{}`; `CueCentre` returns `{}`).
3. Write all tests in full as `DISABLED_` (asserts as their cards give).

**Checks**: build clean; `tools/ctest.sh` all pass; doc-check PASS. **Done when**: no behaviour change.

## Card 2: The composite split

| | |
|---|---|
| Goal | `composite.frag` is today's shader byte for byte; `composite_fog.frag` exists and is used by every preset but `off` |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | shaders, Graphics (pipeline choice), game flag `--fog`; contract: a new pipeline |

**Steps**
1. Before any change: `cp build/vortex_assets/shaders/composite.frag.spv <scratch>/composite.before.spv`.
2. Move everything of `composite.frag` after `#extension ...` (the defines, includes, bindings, functions, `main`)
   into `engine/shaders/include/composite.glsl` unchanged. `composite.frag` becomes `#version 460`, the
   `#extension` line, the leading comment, `#include "include/composite.glsl"`. Build; `cmp` the new
   `composite.frag.spv` with the saved one: equal. If not: `spirv-dis` both (if installed) and compare without
   `OpSource*` / `OpString` lines; if still different, stop (blocked) and quote the diff head.
3. `engine/shaders/composite_fog.frag`: `#version 460`, the extension, `#define VX_LOCAL_FOG`, the include. (Card 5
   adds the fog code under `#ifdef VX_LOCAL_FOG` inside `composite.glsl`.)
4. Renderer: `PipelineId::CompositeFog`, created like `Composite` from `composite_fog.frag.spv`; `PassComposite` binds
   `CompositeFog` when `settings.fog != FogPreset::Off`. The pass timer stays `composite`.
5. Game `--fog off|low|medium|high` (default low) -> `RenderSettings::fog`; anything else: exit 2 with
   `--fog: expected off, low, medium or high`. With `--connect`, `off` becomes `low` and logs
   `Fog: off is offline only, low in a session`.

**Checks**: build; `tools/ctest.sh -R 'ShaderContract\.|LayoutContract\.'`; all; doc-check; verify quick (G3 exit 0
under `--validation` with the fog pipeline; screenshot unchanged by eye: the fog code is empty).

**Must not break**: `ShaderContract.*` (names); the `off` pipeline's SPIR-V equals the previous commit's.

## Card 3: Shared fog math

| | |
|---|---|
| Goal | The three fog functions are exact and tested on the CPU with the same text the shaders run |
| Depends on | card 1 |
| Effort | 1 d (e) |
| Touches | `shaders/shared/fog.glsl`, graphics tests; no runtime change |

**Steps**
1. `VxFogPhase(c, g)`: `(1 - g^2) / (4 pi (1 + g^2 - 2 g c)^1.5)`, denominator floored at 1e-6.
2. `VxFogMistDepth(ro, rd, dist, d0, k, base, start)`:
   1. `a = max(start, 0)`, `b = max(dist, a)`; if `d0 <= 0` or `b <= a`: return 0.
   2. `y0 = ro.y + rd.y * a - base`, `y1 = ro.y + rd.y * b - base` (heights above the base at both ends).
   3. If `abs(rd.y) < 1e-6`: return `d0 * (b - a) * (y0 <= 0 ? 1 : exp(-k * y0))`.
   4. `tc = a + clamp(-y0 / rd.y, 0, b - a)` is where y crosses 0 (clamped to the segment). The part below the base
      has length `Lb` = (y0 <= 0 ? tc - a : b - tc) when the ends are on different sides, `b - a` when both are <= 0,
      0 when both are > 0. Its depth is `d0 * Lb`.
   5. The part above: length `La = (b - a) - Lb`, heights from `hLow = max(min(y0, y1), 0)` to `hHigh = max(y0, y1)`.
      Depth = `d0 * exp(-k * hLow) * La * F(k * (hHigh - hLow))` with `F(x) = x > 1e-3 ? (1 - exp(-x)) / x : 1 - x / 2`
      (shading.glsl's stable form). Clamp exponents to [-80, 60].
   6. Return the sum.
3. `VxFogPuffDepth(ro, rd, tMax, cr, sigma)`: `m = ro - cr.xyz`, `b = dot(m, rd)`, `c = dot(m, m) - r^2`,
   `h = b^2 - c`; if `h <= 0` or `sigma <= 0`: 0. `t0 = max(-b - sqrt(h), 0)`, `t1 = min(-b + sqrt(h), tMax)`; if
   `t1 <= t0`: 0. Return `sigma / r^2 * -( (t1^3 - t0^3) / 3 + b (t1^2 - t0^2) + c (t1 - t0) )` (the integral of
   `sigma (1 - s^2 / r^2)`), max 0. `rd` is unit length.
4. Tests (`FogShadingTests.cpp`: `#include "shared/vx_cpp.h"`, then `namespace fogcpp { using namespace glm; using
   uint = unsigned int; struct FogKernel {` `#include "shared/fog.glsl"` `}; }` as `cube::ProxyCube`):
   - `PhaseIntegratesToOne`: g in {0, 0.6, -0.3}, a 512 x 1024 midpoint sum over the sphere: within 1 %.
   - `MistBelowItsBaseIsBounded`: ro (0, -20, 0), rd (1, 0, 0), dist 30, d0 0.006, k 0.3, base 1, start 0 ->
     exactly `30 * 0.006` (EXPECT_FLOAT_EQ).
   - `MistMatchesNumericIntegral`: 20 rays from y in {-5, 0.5, 3, 12} with rd.y in {-0.6, -0.1, 0.05, 0.4}, dist 60,
     start 8: within 1 % (or 1e-6 absolute) of a 4096-step midpoint sum.
   - `PuffMatchesNumericIntegral`: centre (0, 0, 10) r 2 sigma 0.4; rays through the centre, at 1 m and 1.9 m offset,
     origin inside the sphere, and `tMax` inside it: within 1 % of a 1024-step sum. Through the centre: `sigma * 4r/3`.
   - `OverlappingPuffsKeepTheCap`: 8 identical fresh puffs (Decision 4 at t = 0.5 s) summed then `min(sum,
     VX_FOG_PUFF_MAX_DEPTH)`: `exp(-depth) >= 0.35` through the centre.

**Checks**: build; `tools/ctest.sh -R 'FogShading\.'`; all; doc-check.

**Traps**: only constructs valid in both languages (no `const` locals of array type, no GLSL-only built-ins outside
vx_cpp.h's vocabulary, `f` suffixes as shading.glsl uses). No shader includes `fog.glsl` until card 5.

## Card 4: Puffs, styles, uniforms and the puff buffer

| | |
|---|---|
| Goal | The CPU side of stage 1: puffs live and are culled, styles fill the uniforms, the buffer reaches the shaders |
| Depends on | cards 1-3 |
| Effort | 1 d (e) |
| Touches | Core (fx), Graphics (`FrameUniforms`, binding 11), Engine, Sim, Net (client cues); contracts as listed |

**Steps**
1. `FogStyleOf`, `ParseFogStyle`, `FogStyleName` per Decision 3.
2. `FogPuffs`:
   - `Add`: if a free slot exists take the lowest free index, else the live slot with the smallest `born` (ties
     lowest index) and `++m_Replaced`; `++m_Added`; update `m_MaxAlive`. A non-finite centre or radius <= 0: ignored.
   - `Update(time)`: a slot with `time - born >= kLifetime` is freed.
   - `Build`: for each live slot in `born` order (ties index): `t = time - born`; r, a, sigma and colour by
     Decision 4; centre render = `center - renderOrigin` (in double, then float). Culling and rectangle: if
     `length(cameraRender - c) < 1.05 r`: rect = whole screen. Else project the 8 corners of the cube `c +- r`: if
     any has clip `w <= 1e-4`, rect = whole screen; else if all 8 are outside the same clip plane (`x < -w`,
     `x > w`, `y < -w`, `y > w`), cull; else rect = the min / max of `(ndc.xy * 0.5 + 0.5) * renderSize`, clamped to
     `[0, renderSize]`. Skip a puff with a = 0.
   - Tests: `OldestIsReplacedWhenFull` (9 adds at times 0..8 -> slot of time 0 replaced, `Replaced() == 1`, alive 8);
     `ExpireAfterTheirLifetime` (add at 0; `Update(2.49)` alive 1, `Update(2.5)` alive 0); `RectangleContainsTheSphere`
     (a perspective `glm::perspective` x `lookAt`; 200 points on the sphere surface projected the same way fall inside
     the rect); `BehindTheCameraIsCulled` (centre 20 m behind, r 2 -> `out` empty).
3. `FrameUniforms` + `common.glsl`: the six members (Skeleton), `static_assert` + 6 x 16. In `common.glsl`:
   ```glsl
   struct VxFogPuff { vec4 centerRadius; vec4 colorDensity; vec4 pixelRect; };   // mirrors fx::GpuFogPuff (48 bytes)
   #ifdef VX_NEED_FOG_PUFFS
   layout(set = 0, binding = 11, std430) readonly buffer FogPuffBuffer { VxFogPuff puffs[]; } u_FogPuffs;
   #endif
   ```
   `LayoutContract.FogPuffMatchesVxFogPuff` (offsets of the three members, as `HudQuadMatchesVxHudQuad`) and a
   `BufferSpec{"u_FogPuffs", 11}` row.
4. Renderer: a per-frame buffer `frame.fogPuffs` (8 x 48 bytes, CpuToGpu), name lookup `"u_FogPuffs"`; upload
   `input.fogPuffs` (an empty span uploads nothing and `fogCounts.x = 0`, the buffer stays bound). `BuildFrameData`:
   `fogSun = (fog.phaseG, preset == Off ? 0 : fog.sunScatter, 1, 0)`, `fogMist = (preset == Off ? 0 :
   fog.mistDensity, fog.mistFalloff, fog.mistBase - origin.y, fog.startDistance)`, `fogLocal.w = fog.addMax`,
   `fogCounts = (puffs, 0, int(preset), 0)`; `fogMap`, `fogWind` zero.
5. Engine: `Impl` owns `fx::FogPuffs` and the style; `SetFogStyle` fills `FogSettings` mist fields (`mistBase =
   seaLevel + mistBaseAboveSea`) and `sunScatter = 0.12`, `phaseG = 0.6`; each frame `fogPuffs.Update(time)` and
   `Build(...)` into a member vector passed as `FrameInput::fogPuffs` (empty when the preset is Off).
   `PresentBlast`: `if (!fromCues && !blast.deferred && blast.radius > 0) fogPuffs.Add(blast.center, blast.radius,
   time)`.
6. `sim::CueCentre` / `CueRadius`: invert `voxel::QuantizeSphere` (read it: the centre in 1/16 voxel of absolute
   voxel coordinates; add half a voxel only if QuantizeSphere subtracted one). Test `Blast.CueCentreIsTheBlastCentre`:
   blasts at (1.3, 2.0, -4.7) and (-12.06, 5.5, 3.01), r 1.2, voxel size 0.125 and 0.1 -> the cue of the result's
   `CarveSphere` gives the centre within 1/16 voxel and the radius within 1/16 voxel.
7. `net::Client`: where the op is applied (`sync.Apply(...)` before `if (present && out.complete) Present(...)`): `if
   (present && op->kind == sim::WorldOpKind::CarveSphere) blastCues.push_back(sim::BlastCueOf(*op));`.
   `TakeBlastCues` swaps the vector out. Cap it at 64 (drop the oldest; presentation only).

**Checks**: build; `tools/ctest.sh -R 'FogPuffs\.|LayoutContract\.|ShaderContract\.|Blast\.'`; all (the net suites
must not change: the cue list is presentation only); doc-check; verify quick.

**Must not break**: `FrameUniforms` contract both sides; HUD binding 10 unchanged; Net and Sim stay GPU-free (`fx` is
Core: allowed); nothing in `sim::Blast`, `WorldOp` or a message; `Client::TakePresentations()` unchanged.

## Card 5: The shaders

| | |
|---|---|
| Goal | The haze glows toward the sun, mist lies low, puffs hide a breach for two seconds and dim the debris in them |
| Depends on | cards 2-4 |
| Effort | 1 d (e) |
| Touches | `include/composite.glsl` (under `VX_LOCAL_FOG` only), `particles.vert`; no C++ except as noted |

**Steps**
1. In `composite.glsl`, at the top under `#ifdef VX_LOCAL_FOG`: `#define VX_NEED_FOG_PUFFS` (before the common
   include) and `#include "shared/fog.glsl"` (after it).
2. `ApplyFog(color, rd, dist)` under `VX_LOCAL_FOG` (the `#else` branch is today's body, unchanged):
   1. `fogCol = u_Frame.fogColor.rgb + u_Frame.sunRadiance.rgb * u_Frame.fogSun.y * VxFogPhase(dot(rd,
      u_Frame.sunDirection.xyz), u_Frame.fogSun.x)`.
   2. Floor: today's `VxFogAmount(...)` amount, `color = mix(color, fogCol, amount)`.
   3. Mist: `tau = min(VxFogMistDepth(cameraPos, rd, dist, fogMist.x, fogMist.y, fogMist.z, fogMist.w),
      fogLocal.w * max(dist - fogMist.w, 0))` (the cap), `color = mix(color, fogCol, 1 - exp(-tau))`.
3. Puffs (`VX_LOCAL_FOG`), a function `vec4 PuffFog(vec3 ro, vec3 rd, float tMax, vec2 px)` returning (colour,
   depth): loop `i < u_Frame.fogCounts.x`; skip unless `px` is inside `pixelRect` (4 compares); `d =
   VxFogPuffDepth(...)`; accumulate `depth += d`, `col += colorDensity.rgb * d`. After the loop: `col /= max(depth,
   1e-6)`, `depth = min(depth, VX_FOG_PUFF_MAX_DEPTH)`; lit colour = `col * (u_Frame.fogColor.rgb +
   u_Frame.sunRadiance.rgb * 0.1)`. Apply on surfaces (`tMax = t`) and the sky (`tMax = VX_SKY_DISTANCE`) after
   `ApplyFog`, before the curtain and the edge fog: `color = color * exp(-depth) + lit * (1 - exp(-depth))`.
4. `particles.vert`: `#define VX_NEED_FOG_PUFFS`, include `fog.glsl`; after today's floor fog: the lobe colour, the
   mist (same cap) and the puff loop from the camera to the particle (no rectangle test in the vertex shader),
   applied to the vertex colour as composite does. With `off` the uniforms make all three zero.
5. Shader width: `MESA_SHADER_CACHE_DISABLE=true INTEL_DEBUG=fs` on a short headless run: `composite_fog`'s SIMD16
   line says `scheduled with mode top-down` without new spills; `composite.frag`'s is unchanged. If not: stop and
   report (blocked).

**Checks**: build; `tools/ctest.sh -R 'ShaderContract\.|LayoutContract\.'`; all; doc-check; verify quick (look at
G3's screenshot: haze brighter toward the sun); a headless `--validation --fog off` run equals the previous look.

**Traps**: composite's sky path returns early: give it the lobe through `SkyColor` (which calls `ApplyFog`) and the
puffs explicitly. `particles.vert` serves every preset: the pipeline always declares binding 11, so the renderer must
always bind `u_FogPuffs` (card 4).

## Card 6: Flags, sessions, measurement, close

| | |
|---|---|
| Goal | The stage is driven from the command line, works in sessions, is measured against its gates and documented |
| Depends on | cards 1-5 |
| Effort | 1 d (e) |
| Touches | game `src/main.cpp`, `src/Session.cpp`; docs |

**Steps**
1. Game flags (fog.md "Headless flags", stage 1 part): `--fog-style default|fen|clear` (offline only; with
   `--connect` exit 2 `--fog-style is offline only`), `engine.SetFogStyle(style, seaLevel)` offline from the game's
   terrain settings; `--camera valley` = `-1,61,65,-0.241,-0.55`.
2. `--fog-blast [N]` (offline; default 3, at most 8): at script frame 30 N blasts `Engine::Explode(hit, 1.2f, 6.0f)` at
   the hits of `[wp-8]`'s screen-grid rays (its helper; default camera `valley`). Log at +60, +240, +480 frames:
   `Fog: blast N; puffs alive A1 at +1 s, A4 at +4 s, A8 at +8 s` once at +8 s. Expectations (exit 3): `A1 == N`,
   `A4 == 0`, `A8 == 0`. Screenshot frame: the script's +1 s frame when `--screenshot` is given.
3. Session: after joining, `engine.SetFogStyle(Default, region.terrain.seaLevel)` and `SetFogPuffsFromCues(true)`;
   each frame `for (cue : m_Client.TakeBlastCues()) engine.AddFogPuff(sim::CueCentre(cue, vs), sim::CueRadius(cue,
   vs))`; also drain it in the branch that discards presentations (`(void)m_Client.TakePresentations()`), so it does
   not grow.
4. Exit line (engine, every run): `Fog: preset P style S, puffs at most M (added A, replaced R)`. List it, `--fog`,
   `--fog-style`, `--fog-blast` and `--camera valley` in docs/verify.md and README "Controls" flags if listed there.
5. Measure on the UHD 620 (fog.md "How each is measured"): headless, no `--validation`, `--frames 1200`, two runs
   each, composite p50 from the `Perf:` line:
   ```
   ./build/Shredborne --headless --frames 1200 --camera valley --fog off
   ./build/Shredborne --headless --frames 1200 --camera valley --fog low
   ./build/Shredborne --headless --frames 1200 --camera probe --fog off      # and --fog low
   ./build/Shredborne --headless --frames 1200 --camera valley --fog-blast 8
   ```
   Gates: composite (far and probe) within +0.08 ms of `--fog off`; with 8 puffs within +0.15 ms; `--fog off`
   medians within perf.md's far 1.24-1.40 / probe 1.83-2.15 ms (or the current ROADMAP values). A missed gate is
   reported with its numbers (not loosened). Screenshots: `--validation --frames 900 --after-fill --camera valley
   --fog-blast --screenshot dust.png` (a puff at +1 s, debris dimmed in it), `--fog-style fen --camera probe
   --screenshot fen.png`, and the haze toward / away from the sun (two `--camera` yaws).
6. Session check: `tools/session-check.sh` PASS; both clients log `Fog: ... added A` with A > 0 after its drill;
   `--connect ... --fog off` logs the low line.
7. Close: `grep -rn 'DISABLED_' engine/tests tests` shows none of this stage; ROADMAP ([fog-1] "Built", test count,
   "Performance and measured values" rows), fog.md header Status / Next (`[fog-2]` after the owner looks) and a
   "Measured (stage 1)" note; ARCHITECTURE renderer section (the composite split, binding 11, `fx:fog`); CLAUDE.md
   section 5 "Rendering and data" one bullet: "`composite.frag` (off) and `composite_fog.frag` share
   `include/composite.glsl`; fog code only under `VX_LOCAL_FOG`; `fx::GpuFogPuff` == `VxFogPuff` (binding 11).
   `LayoutContract.FogPuffMatchesVxFogPuff`"; owner check "fog look: haze, mist, puffs" in docs/owner-checks.md.
   G21 compares composite against the old baseline: if it flags composite, say so; the owner records a new baseline
   (`VERIFY_GPU_BASELINE=record`) after accepting the look.
8. `VERIFY_SLOT=1 tools/verify.sh full --ci`; `VERIFY_SLOT=1 tools/verify.sh soak` in the background.

**Must not break**: every existing gate line (`Beam cut`, `HUD`, `on the ground`, `--climb`, the session checks);
`composite.frag`'s SPIR-V; nothing in Sim / Net decides from fog (fog.md "Not touched, on purpose").

**Done when**: [ ] gates measured and recorded; [ ] `--fog-blast` expectations pass; [ ] session-check PASS with
puffs on both clients; [ ] full --ci PASS.

## Values this stage lands

No registry line in notes/net.md section 2 (no wire change). README "Values planned": `[fog-1]` changes no byte,
adds no gate. Graphics: `FrameUniforms` + `fogSun`, `fogMist`, `fogLocal`, `fogMap`, `fogWind`, `fogCounts` after
`[wp-8]`'s `effectLights`; buffer binding 11 `u_FogPuffs`; pipelines `composite` (off) and `composite_fog`. Stage 2
adds textures 17-22, `VX_FEATURE_LOCAL_FOG = 256`, `VX_DEBUG_FOG = 8` and the `fog` pass timer (fog.md "Contracts
touched").
