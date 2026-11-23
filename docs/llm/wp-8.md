# [wp-8] Fight measurement, fx-budget stages 1-2, 10 clients: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/fx-budget|fx budget]]

| | |
|---|---|
| Stage | `[wp-8]`: fights stay playable on the UHD 620. Measure first (`--stress-fx N`, run-wide blast and particle timers, a 10-client session at 10 casts/s), then the cheap wins of fx-budget stage 2 (flash lights out of the ray-traced pass, pool 8192, 96 debris samples, alpha-only sort, flat shape varying, edited chunks uploaded first), then measure again |
| Design | [fx-budget.md](../design/fx-budget.md) "Stages" 1-2, "Effect lights", "Particle cost"; ROADMAP "Next" item [wp-8] and "M2 - Fight feel under authority" ("Fight performance"); the project review (game repository, `docs/history/reviews/project-review-2026-11-17.md`) "WP8" and section 3 "Measurement code in production paths" (the per-blast line) |
| Code read at | engine `ea41341`, game `58d7b87` (`[wp-4]` was being built in the working tree). `[wp-5]`, `[wp-6]` and `[wp-7]` land before this stage: find code with `grep -n`, never by line number |
| Depends on | `[wp-4]` (`vortex::RunStats`, `PerfSummary`, the `Perf:` line, G21), `[wp-7]` (fighting bots, charges, rounds), `[wp-5]` (`FrameUniforms::playBounds`, the last member before this stage's) |
| Cards | 7: skeleton (0.5 d); timers, `Fx:` line, `--stress-fx` (1 d); the 10-client run (1 d); effect lights in composite (1 d); particle wins (0.5 d); edited uploads first + collider starvation test (0.5 d); close (0.5 d) |
| Wire / rules / hashes | none: no `kProtocolVersion` bump, no `kPlayerRuleRevision` bump, `buildHash` unchanged, no new gate. Presentation only. One graphics contract changes: `FrameUniforms` gains `effectLights` (card 4). G21's baseline is recorded again at the close (lighting and composite times change on purpose) |

## Before you start

- `[wp-4]`, `[wp-5]`, `[wp-6]` and `[wp-7]` are committed in both repositories (`git log --oneline | grep '\[wp-7\]'`
  in the game and in `engine/`).
- What the code looks like at the start:
  - `engine/src/sim/internal/Blast.cpp` `sim::PresentBlast`: per `CarveSphere` a debris sample with
    `constexpr size_t kMaxBursts = 400` (`fx::SpawnVoxelBreak` per sampled voxel), per `ClearMask` the same cap with
    `SceneConfig::debrisParticleStride`, then `fx::SpawnExplosion`, then the flash: a `scene::LightDesc` with
    `intensity = 40 * radius`, `radius = radius * 8`, `decay = kFlashDecay (9)`, made by `scene.CreateLight`. The
    flash is a scene entity, gathered by `vortex::GatherLights` (`engine/src/engine/internal/Gather.cpp`) into
    `graphics::PointLight` and traced with shadows in `lighting.frag` (loop to `u_Frame.counts.z`, `kMaxLights =
    256` in `Renderer.cpp`).
  - `sim::BlastTimes` (`engine/src/sim/Blast.cppm`): `carveMs`, `searchMs`, `neckMs`, `opsMs`, `objectsMs`, ... per
    blast; `Engine::PresentBlast` (`engine/src/engine/internal/Engine.cpp`) prints the ~600-character `Blast:` line
    for every blast (offline and every presented cast of a session client).
  - `fx::ParticleSystem` (`engine/src/fx/Particles.cppm`): `BuildRenderData` sorts every live particle back to front
    (`std::sort` over `SortKey`); `ParticleFlag_Additive` particles are written with alpha 0. Pool size
    `EngineConfig::maxParticles = 32768` and `sim::SimulationConfig::maxParticles = 32768`.
  - `fx::SpawnVoxelBreak` (`engine/src/fx/internal/Effects.cpp`): 6 debris + 2 dust per call.
  - `voxel::ChunkStore` (`engine/src/voxel/Store.cppm`): `MarkDirty(slot)`, `TakeDirty()`; the renderer's
    `SyncVoxelData` enqueues `TakeDirty()` in order (`EnqueueSlot`).
  - `vortex_server` (`engine/tools/server/main.cpp`): `--clients`, `--bot-casts N` (`castEveryTicks`), the
    `Ticks: server: ... p99 under X ms ...` line; `vortex_bot --casts N`.
  - From `[wp-7]` (expected names; read the built code): `sim::FighterState` (`engine/src/sim/Combat.cppm`), the
    charge rule (3 charges, one back every 1.2 s = 72 ticks) with a session-file key (expected `charge_ticks`), bots
    that fight (`net::Bot` pilot setting, expected `fight`), gate G22.

## Decisions (us, 2026-11-23; the owner may veto)

1. **No replicated rule changes.** fx-budget stage 2 lists "island margin 12, early-out, minimum island size". The
   island box (`sim::kIslandSearchMargin = 24`, `static_assert`ed against `kServerWindowMarginChunks`) decides which
   voxels detach on every machine: changing it changes `WorldHash` results and would need a protocol bump. The graph
   search (shred.md stage 4) already replaced the 72^3 voxel flood the design measured. Not in this stage; card 2's
   numbers decide whether a later stage asks for it.
2. **Already built, only tested here:** `Lifetime` on pieces (shred.md stage 1e: `debrisLifetime`, caps) and the
   collider "newest result wins" fix (`TerrainColliders` `cookingEdits` / `Installed(generation)`). Card 6 adds the
   design's unit test for starvation; if it passes as the code is, no code change.
3. **The per-blast line** (review: "behind Debug or `--blast-log`"): `Engine::SetBlastLog(bool)`, default off. The
   game turns it on for offline headless runs and with `--blast-log`; `--stress-fx` turns it off unless `--blast-log`
   is also given. Sessions and windowed play print no `Blast:` lines. Every blast still feeds the `Fx:` exit line.
4. **`--stress-fx N`** (game, offline, headless or windowed): needs a fixed camera, default `--camera probe` when none
   is given. From script frame 30 (`--after-fill` counts from the filled window): N blasts in one frame, then 3 per
   second (one every 20 frames at the fixed 1/60 s) for 10 s (30 blasts). Each blast: `Engine::Explode(hit, 1.2f,
   6.0f)` at the hit of a camera ray through screen point k of a 5 x 3 grid (k = blast index mod 15; columns at 0.2,
   0.35, 0.5, 0.65, 0.8 of the width, rows at 0.45, 0.6, 0.75 of the height), `World::Raycast` up to 40 m; a miss
   takes the next point (at most 15 tries, then the blast is skipped and counted).
5. **The fight margin** (acceptance "within a fixed margin of idle"): over the stress window (first blast to 2 s
   after the last), CPU frame p99 <= idle CPU frame p50 + 8 ms and GPU sum p99 <= idle GPU sum p50 + 4 ms (the design's
   4 ms effects cap), idle = the 120 frames before the first blast. Measured on the UHD 620 after the wins (card 7).
6. **The 10-client run**: arena250, a session file `fight10.session` (a copy of `arena250.session` with `[wp-7]`'s
   fight rules and the charge refill at 60 ticks, so 10 players can sustain 10 casts/s). 10 `vortex_bot` processes
   `--casts 60` (plus `[wp-7]`'s fight setting) on 127.0.0.1 port 27065, one headless game client measured with its
   `Perf:` and `Fx:` lines, 600 s. If `[wp-7]` has no session key for the refill, the card is blocked (ask; do not
   change the charge rule). The server's tick p99 must stay under 16.7 ms (it is an upper bound: 11 processes share
   the laptop). The Windows laptop client (GTX-1650 class) is an owner check.
7. **Effect lights** follow the design: up to 8 after a CPU merge, unshadowed, evaluated in `composite.frag`, no
   history, range 4 x r (was 8 x r), axis clip box from 6 `World::Raycast` calls at spawn. They stay scene entities
   (the flash keeps its `Lifetime` and decay); `scene::PointLight` and `graphics::PointLight` get `effect` and a clip
   box. A light dropped by the merge is merged into the nearest slot (nothing is dropped; the glow sprite is stage 3).
   Shadowed lights keep `kMaxLights = 256`.
8. **Debris**: at most 96 bursts per `CarveSphere` and per `ClearMask` operation (was 400); dust on every fourth
   burst only (`SpawnVoxelBreak` gains `bool dust`). The pool is 8192 in both configs.
9. **Sorting**: blended particles sorted back to front as today, then additive ones appended unsorted in pool order.

## Skeleton

Card 1 lands these with stub bodies (`// [wp-8] card N fills this`) and every test below as `DISABLED_`.

```cpp
// engine/src/core/RunStats.cppm (export namespace vortex), after PerfSummary:
/// @brief What the `Fx:` exit line reports (Engine fills it; one sample per blast or per frame as noted).
struct FxSummary {
    uint32_t blasts = 0;            ///< Engine::PresentBlast calls (late searches included)
    uint32_t skipped = 0;           ///< --stress-fx blasts whose 15 rays all missed
    RunStats carve, search, neck, ops, objects, present;   ///< per blast, from sim::BlastTimes and the present time
    RunStats particlesUpdate;       ///< per frame: ParticleSystem::Update
    RunStats particlesBuild;        ///< per frame: ParticleSystem::BuildRenderData
    uint32_t particlesMax = 0;      ///< most particles alive in one frame
    uint32_t particlesCapacity = 0;
    uint32_t effectLightsMax = 0;   ///< most effect lights after the merge in one frame (card 4)
};
/// @brief `Fx: B blasts (S skipped); carve p50 a p99 b max c ms, search ..., neck ..., ops ..., objects ...,
/// present ...; particles update p50 .. p99 .. ms, build p50 .. p99 .. ms, alive at most M of C; effect lights at most E`.
std::string FormatFxLine(const FxSummary& fx);

// engine/src/fx/Effects.cppm (export namespace vortex::fx):
/// @brief An unshadowed effect light (blast flash) as the merge sees it. World metres.
struct EffectLightIn {
    glm::dvec3 position{0.0};
    glm::vec3 color{1.0f};
    float intensity = 0.0f;
    float radius = 0.0f;
    glm::dvec3 clipMin{0.0}, clipMax{0.0};   ///< world box the light reaches (from the axis rays)
};
inline constexpr uint32_t kMaxEffectLights = 8;
/// @brief Merges `lights` into at most kMaxEffectLights (Decision 7; card 4 gives the rule). Deterministic.
void MergeEffectLights(std::span<const EffectLightIn> lights, const glm::dvec3& camera,
                       std::vector<EffectLightIn>& out);
// SpawnVoxelBreak gains a last parameter `bool dust = true` (card 5).

// engine/src/fx/Particles.cppm, ParticleSystem: no new member (card 5 changes BuildRenderData only).

// engine/src/scene/Components.cppm, struct PointLight, appended:
bool effect = false;                          ///< unshadowed effect light (composite pass), card 4
glm::vec3 clipMin{0.0f}, clipMax{0.0f};       ///< effect only: reach box relative to the light (m)
// engine/src/scene/Scene.cppm, struct LightDesc, appended: the same three fields.

// engine/src/graphics/Frame.cppm, struct PointLight, appended:
bool effect = false;
glm::dvec3 clipMin{0.0}, clipMax{0.0};        ///< effect only: world box

// engine/src/voxel/Store.cppm, class ChunkStore:
/// @brief MarkDirty for an edit (a blast, a FillBox): TakeDirty returns edited slots first.
void MarkEdited(uint32_t slot);

// engine/src/engine/Engine.cppm, class Engine:
/// @brief Print the per-blast `Blast:` line (Decision 3). Default false.
void SetBlastLog(bool on);
/// @brief The run-wide effect statistics (the `Fx:` line is printed at exit when blasts > 0).
const FxSummary& Fx() const;
/// @brief For --stress-fx: blasts skipped by the script, added to the `Fx:` line.
void AddSkippedBlast();
```

Card 4 adds to `graphics::FrameUniforms` (after `playBounds`) and to `common.glsl`'s block, in the same order:

```cpp
glm::ivec4 effectLights{0};   ///< x effect light count (<= 8), y first record in u_Lights (= counts.z), zw 0
```

Tests of the stage (suite, file, card that enables it):

| Test | File | Card |
|---|---|---|
| `RunStats.FxLineNamesEveryPhase` | `engine/tests/core/RunStatsTests.cpp` | 2 |
| `EffectLights.AtMostEightAfterTheMerge` | `engine/tests/core/EffectLightTests.cpp` (new) | 4 |
| `EffectLights.NearLightsMergeIntensityWeighted` | same | 4 |
| `EffectLights.ClipBoxIsTheUnion` | same | 4 |
| `Blast.TheFlashIsAnEffectLightClippedByWalls` | `engine/tests/sim/BlastTests.cpp` | 4 |
| `LayoutContract.FrameUniformsMatchTheGlslBlock` (existing, new member) | `engine/tests/graphics/ContractTests.cpp` | 4 |
| `Particles.BlendedSortedAdditiveAppended` | `engine/tests/core/ParticleTests.cpp` | 5 |
| `Blast.PresentBlastSamplesAtMost96Bursts` | `engine/tests/sim/BlastTests.cpp` | 5 |
| `ChunkStore.EditedSlotsAreTakenFirst` | `engine/tests/core/ChunkTests.cpp` | 6 |
| `TerrainColliders.AChunkEditedEveryFrameGetsACollider` | `engine/tests/physics/TerrainColliderTests.cpp` | 6 |

New `.cpp` / `.cppm` files are picked up by the globs (`engine/CMakeLists.txt`, `engine/tests/CMakeLists.txt`).

## Card 1: Skeleton

| | |
|---|---|
| Goal | The stage's declarations compile with stub bodies; its tests exist, disabled |
| Depends on | `[wp-7]` committed |
| Effort | 0.5 d (e) |
| Touches | Core (`:runstats`, fx), Scene, Graphics (`PointLight` only), Engine; wire: no; buildHash: no |

**Read first**: `engine/src/core/RunStats.cppm` (whole), `engine/src/fx/Effects.cppm`, `scene::PointLight` /
`scene::LightDesc`, `graphics::PointLight`, `voxel::ChunkStore`, `class Engine` (public part).

**Steps**
1. Add every declaration of "Skeleton" except `FrameUniforms::effectLights` (card 4: a layout contract changes once).
2. Stubs: `FormatFxLine` returns `"Fx: 0 blasts"`; `MergeEffectLights` clears `out`; `MarkEdited` calls `MarkDirty`;
   `SetBlastLog` stores the flag (the `Blast:` line is still always printed until card 2); `Fx()` returns a member.
3. Write all tests of the table in full (asserts as their cards give), named `DISABLED_<Name>`.

**Checks**: build exit 0, no warning; `tools/ctest.sh` all pass (disabled tests are reported, not failed);
`tools/doc-check.sh --build build` PASS.

**Done when**: [ ] build clean; [ ] ten tests exist, disabled; [ ] no behaviour change (the `Blast:` line unchanged).

## Card 2: Timers, the `Fx:` line, `--blast-log`, `--stress-fx N`

| | |
|---|---|
| Goal | One headless command gives the fight cost table of fx-budget stage 1 |
| Depends on | card 1 |
| Effort | 1 d (e) |
| Touches | Core, Engine, game `src/main.cpp`; wire: no |

**Read first**: `Engine::PresentBlast` and `Engine::Run` (the `Perf:` line, `s.perf`, `sim.Particles().Update`,
`BuildRenderData`); `src/main.cpp` the script frame (`m_ScriptFrame`, `--after-fill`, `--probe-blast` as a model),
`--camera` parsing; `docs/verify.md` "Game (offline, headless)".

**Steps**
1. `FormatFxLine`: exactly
   `Fx: {blasts} blasts ({skipped} skipped); carve p50 {:.2f} p99 {:.2f} max {:.2f} ms, search ..., neck ..., ops ...,
   objects ..., present ...; particles update p50 {:.2f} p99 {:.2f} ms, build p50 {:.2f} p99 {:.2f} ms, alive at most
   {particlesMax} of {particlesCapacity}; effect lights at most {effectLightsMax}` (each "..." the same three numbers
   as carve). Enable `RunStats.FxLineNamesEveryPhase`: a summary with one sample of 1.0 in every RunStats, 2 blasts,
   alive 10 of 8192 -> the line contains each phase name once, in that order, and `alive at most 10 of 8192`.
2. `Engine::PresentBlast`: after `sim::PresentBlast`, add `blast`'s `BlastTimes` to `carve`, `search`, `neck`, `ops`,
   `objects` and the measured present time to `present`; `++blasts`. Print the `Blast:` line only when the flag is on.
3. `Engine::Run`: time `sim.Particles().Update(...)` and `BuildRenderData(...)` with `NowSeconds()` (as `cpuWorld`),
   add to `particlesUpdate` / `particlesBuild` every frame; `particlesMax = max(particlesMax, AliveCount())`;
   `particlesCapacity = Capacity()`. At exit, after the `Perf:` line: `if (fx.blasts > 0) Log::Info("{}",
   FormatFxLine(fx))`.
4. Game: `--blast-log` -> `engine.SetBlastLog(true)`; offline `--headless` runs also set it true (existing script
   output unchanged) unless `--stress-fx` is given without `--blast-log`.
5. Game: `--stress-fx [N]` (default 10), offline only (with `--connect`: exit 2, `--stress-fx is offline only`). No
   `--camera`: use `probe` (it implies `--probe`, as today). Script per Decision 4. Log at the start
   `FxStress: N first-frame blasts, then 3 per second for 10 s, camera C` and at script end (2 s after the last
   blast) `FxStress: idle CPU frame p50 a ms, GPU sum p50 b ms; fight CPU frame p99 c max d ms, GPU sum p99 e max f
   ms; particles alive at most M` (idle = 120 frames before the first blast; collect per-frame CPU frame and GPU sum
   in two local `RunStats` per phase; if the engine exposes no per-frame values, add
   `struct FrameTimes { double cpuFrameMs = 0, gpuSumMs = 0; };` and `FrameTimes Engine::LastFrameTimes() const`, filled
   where `Run` adds to `s.perf`). The run continues to `--frames`.
6. Run on the UHD 620, two runs each, after fill:
   `./build/Shredborne --headless --frames 1500 --after-fill --stress-fx 10` and with `--stress-fx 1`; and the far
   view `--camera=-1,61,65,-0.241,-0.55 --stress-fx 10`. Write the numbers into fx-budget.md (a new section
   "Measured (stage 1)": the table "Where one blast costs time" with a measured column; the GPU passes lighting,
   composite, particles idle vs fight from the `Perf:` line). Order of card 4-6 stays as planned unless a number shows
   a wrong order (say so in the report).

**Log lines**: `Fx: ...` (exit), `FxStress: ...` (two lines). No judge reads them; list them in docs/verify.md.

**Checks**: build; `tools/ctest.sh -R 'RunStats\.'`; `tools/ctest.sh`; doc-check; `VERIFY_SLOT=1 tools/verify.sh
quick` (G3-G10 must not change: their scripts are headless, so their `Blast:` lines stay); the three runs above exit
0 (also once with `--validation`).

**Must not break**: G3-G10 log lines; `Blast.OfflineCrumbsBecomeParticlesWithoutAnyEntityOrBody`; the `Perf:` line.

**Traps**: `PresentBlast` also runs for late searches (`RetryIslandSearches`): count them (they are blasts the
frame pays for). Do not time with `ProfileScope` (its samples are summed per frame, not per call).

## Card 3: The 10-client fight run

| | |
|---|---|
| Goal | A measured 10-bot session at 10 casts/s on arena250, with a game client's frame numbers and the server's tick p99 |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | `engine/tools/server/sessions/fight10.session` (new), docs; wire: no |

**Read first**: `engine/tools/server/sessions/arena250.session`; `[wp-7]`'s session keys (grep the session parser
for the charge key); `vortex_bot` header comment (`--connect`, `--token`, `--seconds`, `--casts`); `tools/soak.sh`
(how it starts bots, `--parent-pid`, `--end-at`); docs/verify.md G15.

**Steps**
1. `fight10.session` = `arena250.session` with `max_clients = 16`, `[wp-7]`'s fight rules on (its key for a team
   deathmatch fallback, two teams) and the charge refill at 60 ticks. Its own fresh token line (32 hex digits from
   `od -An -N16 -tx1 /dev/urandom`); the session parser must accept it (`vortex_server --session ... --ticks 60`
   exits 0).
2. The run (write it into docs/verify.md "Fight measurement", port 27065, never 27015):
   ```
   T=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
   build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/fight10.session \
       --listen 27065 --bind 127.0.0.1 --token $T --seconds 660 > server.log 2>&1 &
   END=$(( $(date +%s) + 600 ))
   for i in 0 1 2 3 4 5 6 7 8 9; do build/engine/tools/bot/vortex_bot --connect 127.0.0.1:27065 --token $T \
       --end-at $END --id $i --casts 60 <wp-7 fight flag> --parent-pid $$ > bot$i.log 2>&1 & done
   ./build/Shredborne --connect 127.0.0.1:27065 --token $T --headless --frames 34000 > client.log 2>&1
   ```
   (Use copies of the binaries as `tools/soak.sh` does if a build may run meanwhile.)
3. Read: server `Ticks: server: ... p99 under X ms`, `castsRun` over 600 s (>= 5700 = 9.5/s; if lower, say by how
   much and why: rejections by reason from the server's cast line), `hashes equal` / every `Entities: ... agree`
   where the bots print them; client `Perf:` and `Fx:` lines.
4. Record the numbers in fx-budget.md "Measured (stage 1)" and ROADMAP "Performance and measured values" (a row
   "10-client fight, arena250, before the wins").
5. Add owner check "10-client fight on the Windows laptop" to `docs/owner-checks.md` (the same server and bots on
   the Linux laptop, the Windows client joining over the LAN, its `Perf:` line).

**Checks**: the run exits 0 for the server, every bot and the client; no `Checks:` mismatch; doc-check.

**Done when**: [ ] numbers recorded with the command; [ ] tick p99 reported (pass or fail: the acceptance is judged
at card 7, after the wins).

**Traps**: in-process bots (`--clients 10`) share the server's cores and inflate its ticks: use separate
`vortex_bot` processes. Never leave processes behind: `--parent-pid` and `--end-at` on every bot.

## Card 4: Effect lights in composite

| | |
|---|---|
| Goal | A blast's flash is crisp, instant and no longer costs the ray-traced lighting pass |
| Depends on | card 2 (numbers before) |
| Effort | 1 d (e) |
| Touches | Core (fx), Scene, Sim (presentation only), Graphics, shaders, Engine; contract: `FrameUniforms` |

**Read first**: `sim::PresentBlast` (the flash block); `GatherLights`; `Renderer.cpp` the lights block (`lightOrder`,
`kMaxLights`, `Upload<GpuLight>`, the name lookup `"u_Lights"`), `PassComposite`; `composite.frag` (where the
surface colour is formed before `ApplyFog`); `shading.glsl` `VxLightFalloff`; notes/graphics.md section 4 recipe A.

**Steps**
1. `MergeEffectLights(lights, camera, out)`:
   1. `out.clear()`; for each input i: `score_i = intensity * radius^2 / max(distance(camera, position), 1)^2`.
   2. Order inputs by score descending, ties by input index ascending.
   3. For each in that order: find the first slot s in `out` with `distance(s.position, in.position) <= 0.5 *
      max(s.radius, in.radius)`. If none and `out.size() < 8`: append a copy. If none and full: s = the slot with the
      smallest centre distance (ties: lowest index). Merge into s: `w = s.intensity + in.intensity` (if w <= 0 skip);
      `position = (s.position * s.intensity + in.position * in.intensity) / w`; `color = (s.color * s.intensity +
      in.color * in.intensity) / w`; `intensity = w`; `radius = max`; `clipMin = min`, `clipMax = max` per axis.
   Tests: 20 lights at distinct far-apart points -> `out.size() == 8`; two lights 0.5 m apart, radius 4, intensities
   1 and 3 -> one light at 3/4 of the way, intensity 4; clip boxes [0,1]^3 and [2,3]^3 merged -> [0,3]^3.
2. `sim::PresentBlast` flash: `flash.radius = radius * 4` (was 8), `flash.effect = true`; clip: for each of the six
   axis directions `world.Raycast(center, dir, flash.radius)`; extent = `hit ? distance : flash.radius`;
   `clipMin = -(extents -x, -y, -z)`, `clipMax = (+x, +y, +z)` relative to the centre. Lifetime rule unchanged
   (`log(intensity / kFlashCutoff) / kFlashDecay`; with decay 9 and intensity 48 at r = 1.2: 0.76 s; set
   `kFlashDecay = 18` so it is under 0.4 s: 0.38 s). `scene::Scene::CreateLight` copies the three fields.
   Test `Blast.TheFlashIsAnEffectLightClippedByWalls`: flat ground, a FillBox wall 1 m east of the centre, blast r 1.2
   -> one `PointLight` with `effect`, `clipMax.x` within one voxel of 1 m, `clipMax.z == 4.8f`.
3. `GatherLights`: copy `effect` and the clip box (world = position + clip). Renderer: split `input.lights` into
   shadowed (as today, nearest 256) and effect; merge the effect ones (`MergeEffectLights`, camera = world camera);
   upload shadowed records first (count `counts.z`), then two records per effect light: A = `{positionRadius
   (render space), colorSize.rgb = color * intensity, a = 0}`, B = `{positionRadius.xyz = clipMin (render space),
   colorSize.xyz = clipMax (render space)}`. `effectLights = ivec4(E, counts.z, 0, 0)`. The lights buffer grows to
   `256 + 16` records.
4. `FrameUniforms` + `common.glsl`: append `effectLights` (C++ after `playBounds`; GLSL the same place, comment
   "x effect light count, y first record"). `static_assert` grows by 16 bytes. `LayoutContract.FrameUniformsMatchTheGlslBlock`
   passes with the new offset.
5. `composite.frag`: `#define VX_NEED_LIGHTS` before the include; after the surface colour (lit) and before
   `ApplyFog`:
   ```glsl
   for (int i = 0; i < u_Frame.effectLights.x; ++i) {
       int r = u_Frame.effectLights.y + 2 * i;
       vec4 pr = u_Lights.lights[r].positionRadius;
       vec3 lo = u_Lights.lights[r + 1].positionRadius.xyz, hi = u_Lights.lights[r + 1].colorSize.xyz;
       if (any(lessThan(p, lo)) || any(greaterThan(p, hi))) continue;   // the axis clip box
       vec3 l = pr.xyz - p; float d = length(l);
       color += albedo * u_Lights.lights[r].colorSize.rgb * VxLightFalloff(d, pr.w) * max(dot(n, l / max(d, 1e-4)), 0.0);
   }
   ```
   (names `p`, `n`, `albedo` as composite has them). `lighting.frag` is not changed: it only reads `[0, counts.z)`.
6. Shader width: `MESA_SHADER_CACHE_DISABLE=true INTEL_DEBUG=fs ./build/Shredborne --headless --frames 5 2>&1 |
   grep -A2 'composite'`: the SIMD16 line says `scheduled with mode top-down` with no new spills (ARCHITECTURE 5.7). If
   not: stop and report (blocked), do not restructure composite.
7. Engine: `fx.effectLightsMax` from the renderer's merged count (add `stats.effectLights` to the renderer stats).

**Checks**: build; `tools/ctest.sh -R 'EffectLights\.|Blast\.|LayoutContract\.|ShaderContract\.'`; all tests;
doc-check; verify quick (look at G3's screenshot); `--stress-fx 10` twice: GPU `lighting` p50 during the fight within
0.3 ms of idle (fx-budget stage 2 verify); a screenshot at the first blast frame + 2 (`--screenshot` frame option as
the scripts use) shows the flash.

**Must not break**: `FrameUniforms` contract (both sides), `ShaderContract.*` (bindings by name), L6 (no blended
full-screen quad), the HUD (drawn after; untouched).

**Docs**: ARCHITECTURE renderer section (effect lights in composite, the record pair), notes/graphics.md, fx-budget.md
stage row.

**Traps**: render space is relative to `World::WindowOrigin()`: convert the clip box like the position. The flash's
colour times intensity is radiance as `colorSize.rgb` already is for shadowed lights. Owner: the flash looks
different (review "look of the flash"): add an owner check line.

## Card 5: Particle wins

| | |
|---|---|
| Goal | Ten blasts fit in the pool and the last one is still visible; the sort costs less |
| Depends on | card 2 |
| Effort | 0.5 d (e) |
| Touches | Core (fx), Sim (presentation), Engine config, shaders `particles.vert/.frag`; wire: no |

**Steps**
1. `EngineConfig::maxParticles = 8192`, `sim::SimulationConfig::maxParticles = 8192`.
2. `SpawnVoxelBreak(..., bool dust = true)`: skip the 2 dust particles when false. `sim::PresentBlast`: `kMaxBursts =
   96` for `CarveSphere` and `ClearMask`; burst j (0-based, in visit order) passes `dust = (j % 4 == 0)`.
   Test `Blast.PresentBlastSamplesAtMost96Bursts`: a blast that carves > 2000 voxels; copy the result with `deferred
   = true` (no explosion, no flash); particles alive after `PresentBlast` <= 96 x 6 + 24 x 2 = 624 per carving op.
3. `BuildRenderData`: keys only for particles without `ParticleFlag_Additive`, sorted as today; then append every
   additive particle in pool order. Test `Particles.BlendedSortedAdditiveAppended`: 3 blended at distances 1, 3, 2
   and 2 additive -> output blended at 3, 2, 1 then the two additive in pool order.
4. Shape flag: `particles.vert` passes `params.y` as `layout(location = N) flat out float v_Shape`;
   `particles.frag` reads the varying instead of the SSBO (remove its `VX_NEED_PARTICLES` if nothing else uses it;
   the reflection then drops binding 9 from the fragment stage only).

**Checks**: build; `tools/ctest.sh -R 'Particles\.|Blast\.|ShaderContract\.'`; all; doc-check; verify quick (G7 reads
`Probe piece 0 keeps its push`: unchanged); `--stress-fx 10`: `alive at most` <= 8192 and the last blast's particles in
the end screenshot.

**Traps**: G7's probe chair test counts rubble pieces, not particles; if a game expectation counts particles, report
it, never change its value without the card.

## Card 6: Edited chunks uploaded first, collider starvation test

| | |
|---|---|
| Goal | A crater shows the next frame even while the world streams; a beam's chunk keeps getting colliders |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | Core (voxel store, world), Graphics (`SyncVoxelData`), Physics test; wire: no |

**Steps**
1. `ChunkStore`: a second list `m_EditedList` (with the same "already listed" bit as `m_DirtyList`); `MarkEdited`
   appends there (and removes nothing from the dirty list: `TakeDirty` skips duplicates). `TakeDirty()` returns
   edited slots first (in marking order), then the rest, each slot once. Test `ChunkStore.EditedSlotsAreTakenFirst`:
   MarkDirty 1, 2; MarkEdited 3; MarkDirty 3; MarkEdited 2 -> `{3, 2, 1}`; a second call -> empty.
2. `voxel::World`: the edit path (`EditRegion` and whatever marks dirty from an operation: grep `m_Store.MarkDirty` in
   `World.cpp`) calls `MarkEdited`; streaming installs keep `MarkDirty`.
3. Renderer `SyncVoxelData`: slots from `TakeDirty()` are enqueued; an edited slot goes to the front of
   `v.pending` (read `EnqueueSlot`: add a `front` flag). The per-frame upload budget is unchanged.
4. `TerrainColliders.AChunkEditedEveryFrameGetsACollider`: a flat world, a box body resting on it; for 60 frames
   carve one voxel 3 m away inside the same chunk, then run the colliders' update and `Jobs` drain as the other tests
   of the file do; assert the chunk has a ready collider (`HasCollider`) after frame 60 and the body is still above
   the ground. If it fails: fix in `Terrain.cpp` by accepting a result newer than the installed generation (fx-budget
   "Collider starvation"), keeping `Installed(generation)` semantics.

**Checks**: build; `tools/ctest.sh -R 'ChunkStore\.|TerrainColliders\.|World\.'`; all; doc-check; verify quick.

**Must not break**: "Every chunk edit goes through `World::EditRegion`" (CLAUDE.md 5), `ChunkRevision`; uploads are
presentation (no hash). `Movement.NoCorrectionAbove2cmOnFlatGround` stays 0 (colliders).

## Card 7: Close

| | |
|---|---|
| Goal | The stage is measured after the wins and documented |
| Effort | 0.5 d (e) |

**Steps**
1. `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage.
2. Repeat card 2's three runs and card 3's run. Acceptance: Decision 5's margins on the UHD 620; server tick p99 <
   16.7 ms; hashes and entities agree; `lighting` unchanged by casts within 0.3 ms. Write both tables (before /
   after) into fx-budget.md "Measured (stage 1)" and a row in ROADMAP "Performance and measured values". A missed
   acceptance is reported with its numbers (not loosened).
3. `VERIFY_GPU_BASELINE=record` G21 once, after the owner accepts the new picture (owner step: say so in the report).
4. ROADMAP: [wp-8] state "Built", test count line; fx-budget.md header Status / Next (`[fx-3]` FxDirector next when the
   numbers ask for it); docs/verify.md (`--stress-fx`, `--blast-log`, `Fx:`, `FxStress:`, the fight run);
   owner-checks (flash look, Windows client).
5. `VERIFY_SLOT=1 tools/verify.sh full --ci`; `VERIFY_SLOT=1 tools/verify.sh soak` in the background.

**Done when**: [ ] full --ci PASS; [ ] numbers before / after recorded; [ ] no `DISABLED_` of this stage.

## Values this stage lands

No registry line in notes/net.md section 2 (no wire change). README "Values planned": `[wp-8]` changes no byte, adds
no gate. Graphics: `FrameUniforms::effectLights` (one ivec4 after `playBounds`); `u_Lights` holds shadowed records
`[0, counts.z)` then 2 records per effect light. `[fog-1]` appends its six members after `effectLights`.
