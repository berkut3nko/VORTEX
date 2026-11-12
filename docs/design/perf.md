# Frame rate on the UHD 620: what to do, in what order

Status: **draft for the owner, nothing built or measured for it.** Written read-only against the working tree of
2026-11-12 (shaders and `src/graphics` have no uncommitted changes, so their line numbers hold). Every saving is an
estimate **(e)** unless it cites a measurement; each item names the measurement that confirms or kills it. Machine:
i7-8650U, 4 cores / 8 threads, 15 W shared with the UHD 620 (max GPU clock 1150 MHz, `/sys/class/drm/card*/
gt_max_freq_mhz`).

Relation to other designs: [lod.md](lod.md) stage 1 (= [view-distance.md](view-distance.md) stage 4) is the only
frame-rate work already designed; ROADMAP.md:43-48 and :284-288 list three more ideas without a design. This draft
ranks those together with new items and keeps the label **planned** / **new** on each. Destruction spikes are in
[shred.md](shred.md) (same folder); effect costs in fights belong to [fx-budget.md](fx-budget.md) and are only
referenced here.

## Decision in brief

1. **Measure first (M0, about half a day).** The exit log prints GPU pass medians only (`engine/src/engine/internal/
   Engine.cpp:280-296`); CPU frame time, frames over budget and the present path are invisible. Without them nobody
   can say whether the owner's windowed 27 fps (owner report, not in the docs) is GPU time, CPU time or vsync.
2. **Then the cheap shader items (G1 + G2, about half a day together)**: about -1 to -3 ms on the far view (e) with
   no change to the picture. G3 (AO any-hit) is worth little on the idle view (0-0.3 ms, e) and is measured in a fight
   scene instead.
3. **Then lod.md stage 1 (planned)**, then **half-resolution lighting (planned in ROADMAP, no design yet)**: the only
   item large enough to bring the far view under 16.7 ms with margin.
4. **Render scale 0.5 is the stopgap** that exists today (`--scale 0.5`, F3 slider): -5 to -6.5 ms (e) at a visible
   blur cost, which TAA upsampling (planned) later buys back.

## Where the frame goes today (measured)

| View | gbuffer | lighting | composite | Source |
|---|---|---|---|---|
| Far: peak over the valley, `--camera=-1,61,65,-0.241,-0.55` | 12.20-12.21 ms | 7.28-7.38 ms | 1.24-1.40 ms | view-distance.md:425 (0.125 m column), re-checked :548 |
| Near: `--camera probe` | 6.23 ms | 8.88-9.33 ms | 1.83-2.15 ms | view-distance.md:424, :547 |
| HUD | 0.01 ms | | | ROADMAP.md:34 |

- TAA + present are not timed separately; "composite, TAA, present ~2.5 ms" (ROADMAP.md:33) puts them at
  about 1 ms (e). Far total about 21.5 ms = 46 fps of GPU time.
- Lighting features, measured: sun shadows ~4.7 ms, AO ~5.9 ms, emissive pickup ~1.5 ms (ROADMAP.md:40-41).
- While caves stream, gbuffer + lighting rise by +1.3 to +2.4 ms: the generation threads take the shared power
  budget (view-distance.md:315).
- `INTEL_DEBUG=fs`: `lighting.frag` about 3400 instructions, 9 spills, and `gbuffer_world.frag` both compile only
  8 pixels wide (ROADMAP.md:285-288).
- `build/` is optimised (`-O2 -g`, `build/CMakeCache.txt:60`), so CLAUDE.md:36 ("the dev build is unoptimised") is
  stale; CPU numbers from `build/` after 2026-11-12 are usable, those in the docs before that are -O0
  (CLAUDE.md:120-121).

### Why the windowed game can show 27 fps when the GPU needs 21.5 ms (hypotheses, M0 decides)

1. **FIFO quantisation.** `RenderSettings::vsync = true` (`graphics/Renderer.cppm:24`) selects
   `VK_PRESENT_MODE_FIFO_KHR` (`graphics/internal/Swapchain.cpp:145-146`); there is no `--no-vsync`
   (`Engine.cpp:22-36`). A 21.5 ms frame under 60 Hz FIFO lands on 33.3 ms intervals whenever the queue cannot run
   ahead, i.e. 30 fps or less; occasional longer frames give 27.
2. **The submit waits for the acquired image at `ALL_COMMANDS`** (`Renderer.cpp:1674`), so gbuffer, lighting,
   composite and TAA, which never touch the swapchain image, cannot start until it is acquired. With few images this
   adds up to one vblank per frame (e).
3. **CPU and GPU in series**: the fence wait sits inside `RenderFrame` after all CPU work (`Renderer.cpp:1585`), and
   14 CPU-bound threads compete for 8 logical CPUs: the world `JobSystem` takes hw-1 = 7 (`core/internal/
   Jobs.cpp:60-63`), Jolt another 7 (`physics/internal/PhysicsWorld.cpp:430-433`).
4. **Streaming**, as above (+1.3 to +2.4 ms GPU).

Consequence for ranking: under FIFO, a GPU saving shows on screen only if it crosses 16.7 ms (or if vsync is off).
The far view needs about -5 ms; items are ranked by ms per day of work, and the suggested order adds up to that.

## How every item is measured

- **Headless GPU** (frames 1200, no validation, compare only after `World: window filled`, medians of the last 120
  frames as `Engine.cpp:280-296` prints them, two runs each, as view-distance.md did):
  far `./build/Shredborne --headless --frames 1200 --camera=-1,61,65,-0.241,-0.55`, near `--camera probe`, medium
  `--camera probe-far`. Then one `--validation` run per change (must exit 0) and a screenshot of far and near.
- **Shader width**: `MESA_SHADER_CACHE_DISABLE=true INTEL_DEBUG=fs ./build/Shredborne --headless --frames 3 2>&1 |
  grep SIMD` (method in ARCHITECTURE.md:469-475): instruction count, spills, SIMD8 / SIMD16.
- **CPU**: the M0 timers (p50 / p99 / max over the whole run, frames over 25 / 33 ms), from `build/` (now optimised)
  or `build-release/`.
- **Streaming**: `--climb` and `--fly` gate lines must still pass (`0 unloads inside view radius`, `max 0 unfilled
  columns inside 0.85 R`), plus GPU medians during the climb.
- **Windowed only** (owner): present mode, real fps, judder, input latency. Headless never acquires or presents
  (`Renderer.cpp:1639`).
- Baselines must be re-taken once the 0.1 -> 0.125 m switch in the working tree lands.

## Ranked list

"ms" is the saving on the far view unless stated, as an estimate range; "ms/day" uses the midpoint and is only for
ordering. Savings are **not additive**: G1 removes far pixels that lod.md's AO cut and period 4 would otherwise
cheapen (every pixel past the fog radius R = 92 m is also past D = 50 m), and anything in the lighting pass is worth
about a quarter after H1 (a quarter of the pixels). The expected path under "Stages" applies each item to what is left
after the earlier ones.

| Rank | Item | Kind | Planned / new | Saving (e) | Effort (e) | ms/day | Risk to the look |
|---|---|---|---|---|---|---|---|
| 0 | M0 Measurement | CPU+GPU | new | 0 (enables the rest) | 0.5 d | - | none |
| 1 | P1 Present path: `--no-vsync`, `FIFO_RELAXED` option, acquire wait at colour output, 3 images | present | new | windowed fps, not GPU ms; up to 27 -> ~40-46 fps if hypothesis 1-2 holds | 0.5 d | - | tearing with vsync off |
| 2 | G1 Stop primary rays at the edge-fog radius | GPU | new | gbuffer 0.5-1.5, lighting 0.2-0.6; near 0 | 0.2 d | ~7 | none (see risk) |
| 3 | S1 Render scale 0.5 as the UHD 620 default | GPU | existing setting | 5-6.5 far, 4.5-5.5 near | 0 code | - | blurrier (bilinear present) |
| 4 | G2 One modulo per ray instead of six per grid step | GPU | new | gbuffer 0.2-0.8, lighting 0.1-0.4 | 0.35 d | ~2 | none (shared tests) |
| 5 | T1a World workers at lower priority, caves throttled after the surface fills | CPU -> GPU | new (`--climb` lesson kept) | 0.5-2 GPU while streaming, 0 at rest; smaller recentre hitches | 0.3 d | ~4 (streaming) | none |
| 6 | H1 Lighting at half resolution (a quarter of the pixels), upsampled in composite's depth-aware filter | GPU | planned (ROADMAP.md:48, :284), no design | about 0.75 x remaining lighting - 0.3-0.6 upsample: 4-6 today, about 3.5 after G1, G2, L1 | 3-5 d | ~1 | softer shadow edges, halos on 1-voxel features |
| 7 | L1 Distance budgets (period 4 beyond 30 m, AO fade / cut at D = 50 m, light shadows beyond D, caster prefix, sub-pixel culling) | GPU | **planned**: lod.md stage 1 = view-distance stage 4 | 1.5-2.5 far today (lod.md: 2-3 facing a hillside, 0.3 flat); about 1-2 after G1 | 2 d | ~0.75 | far lighting up to 4 frames late |
| 8 | G3 AO ray any-hit with emissive on | GPU | new | idle 0-0.3 (few instances in the BVH); fight unmeasured, est. 0.3-1.5 with 50-250 bodies | 0.15 d | ~1 idle | slight, next to emitters |
| 8b | T1b Jolt 1-2 threads on a session client | CPU | new; **after** shred.md stage 1e's rubble cap and a fight measurement | 0 idle; fight unknown | 0.2 d | - | none if the fight gate holds |
| 9 | B1 One trace call site in lighting, BVH stack 32 -> 16 | GPU | new (part of the ROADMAP SIMD16 idea) | 0.3-1.5, very uncertain | 1 d | ~0.9 | none |
| 10 | U1 Upload timer + byte budget per frame + partial chunk uploads | CPU+GPU | new; sized from shred.md stage 0's per-blast upload log and `--fly` | 0.3-1 GPU and 0.3-0.8 CPU on burst frames (streaming and blasts) | 1 d | burst only | holes while streaming if too tight |
| 11 | B2 Cheaper history fetches (gather for depth / count, one bilinear lighting fetch) | GPU | new | lighting 0.2-0.5 | 0.5 d | ~0.7 | tiny weight differences |
| 12 | C4 `gbuffer_world` 16 pixels wide (smaller traversal state, step counter only in a debug variant) | GPU | planned idea (ROADMAP.md:285-288) | 0-4 (only if SIMD16 is reached) | 2-4 d | ~0.7 | none |
| 13 | C3 Coarse depth pre-pass for primary rays | GPU | planned (ROADMAP.md:46-47), no design | net 1-3 far, 0.5-1 near | 3-5 d | ~0.5 | thin features lost if not conservative |
| 14 | C5 TAA upsampling instead of bilinear present | GPU | planned (ROADMAP.md:284) | makes S1 look like 0.6: net 4-5 vs today | 3-5 d | ~1.1 with S1 | ghosting |
| 15 | B5 AO ray 6 -> 4 m (existing `--ao-distance`) | GPU | existing setting | 0.5-1.5 | measure only | - | interiors leak sky light |
| 16 | A5 Checkerboard period 4 on every settled pixel | GPU | new, extends lod.md's rule to near pixels | 1.5-3 still camera, less moving | 0.3 d | high | moving shadows and flashes smear (~96 frames) |
| 17 | B4 5-tap filter on settled pixels | GPU | new | composite 0.1-0.3 far, 0.2-0.5 near | 0.5 d | ~0.5 | grainier AO when still |
| 18 | X1 Particle CPU in fights (radix key, template callable, parallel update) | CPU | new; next to fx-budget stage 2 | 2-4 at a full pool, 0 idle | 1 d | fight only | none |
| 19 | X2 Terrain-collider change list on shifts | CPU | **planned** (view-distance.md:356) | 0.1-1 on shift frames with parked bodies | 0.5 d | small | none |
| 20 | F1 Fence wait at the top of the frame | latency | new | 10-15 ms less input latency, 0 fps | 0.3 d | - | none |
| 21 | I1 Interpolate bodies between ticks | smoothness | new | 0 ms; removes 1-or-2-tick judder | 0.5 d | - | none |
| 22 | J1 Urgent / background job queues | CPU | new | 0 fps; fewer frames a body stays frozen | 0.5 d | - | none |
| 23 | C6 Dynamic resolution (fixed max targets + viewport) | GPU | new | 0 by itself; holds a target in fights | 2-3 d | - | resolution pumping |

### Details per item

**M0 Measurement** (new). `Engine.cpp:280-296`: add the median / p99 / max of `"CPU: frame"`, `"CPU: world"`,
`"CPU: simulation"` (`Engine.cpp:137, 157, 181`) over the whole run, the sum of GPU passes, and counts of frames over
25 / 33 ms. Split the fence wait out of the `Renderer::RenderFrame` scope (the wait at `Renderer.cpp:1585` is inside
it). Add an `upload` timestamp pair around the copies (they run before the gbuffer pass and no timer covers them).
Log `gt_act_freq_mhz` every 60 frames. Add `--debug-view steps` (`VX_DEBUG_STEPS` exists, `shaders/gbuffer_world.
frag:31-33`, with no flag). Accepted when: a headless far run prints CPU p50 / p99, GPU sum and frames over budget;
the owner reads the same line after a windowed session.

**P1 Present path** (new, windowed only). `--no-vsync` in `EngineConfig::ApplyCommandLine` (`Engine.cpp:22-36`) and
a vsync checkbox in F3 (`src/main.cpp:454-458`); `FIFO_RELAXED` as an option (no tearing when on time); submit wait
at `COLOR_ATTACHMENT_OUTPUT` with the swapchain transition's source stage to match (`Renderer.cpp:1649, 1674`,
`Resources.cpp:31-33`); `set_desired_min_image_count(3)` (`Swapchain.cpp:136-151`). Measured by the owner: F3 fps and
the `Swapchain:` log line (image count, mode) with vsync on / relaxed / off. Validation on the windowed path.

**G1 Edge-fog ray end** (new). `gbuffer_world.frag:18` traces to `tMax = 1.0e9`; composite fades a surface fully to
`SkyColor(rd)` at horizontal distance R (`composite.frag:299-303`, R = `jitter.w` set at `Renderer.cpp:1134-1139`).
Pass `tMax = R / (voxelSize * max(length(rd.xz), 1e-6))` when `jitter.w > 0`: a ray past R misses, lighting returns
at once for it (`lighting.frag:61-62`), and no shadow or AO ray is traced. About 28 % of the 192 m window lies outside
the 92 m circle (e), and the rays going there are the longest grazing ones. Risk: the sky path
(`composite.frag:237-242`) may differ from `SkyColor` (sun disc) and particles beyond R are no longer hidden by
terrain; check the far screenshot at low sun. Measured: far medians, far screenshot diff; near unchanged.

**G3 AO any-hit** (new; re-estimated after review). `lighting.frag:178` passes `anyHit = !emissive`. In
`VxTraceScene` that flag changes one thing: after a world hit, any-hit returns before `VxTraceInstances`
(`trace.glsl:387-392`), and inside the instance walk it stops at the first hit. The world DDA finds its first hit
either way, and `HitEmission`, which G3 keeps, is part of the measured 1.5 ms for `--no-emissive` (ROADMAP.md:40-41).
So the saving is the instance BVH walk (clipped to `tBest`) after a world hit: on the idle far view the BVH holds a
handful of objects, est. 0-0.3 ms. It pays off in fights with many rubble and server bodies. Always any-hit, keep
`HitEmission(hit)` for whatever was hit. Risk: emission picked up through a rubble piece in front of a glowing voxel,
first rather than nearest emitter among instances. Measured: M0's BVH leaf-visit counter (lod.md:146-147) on the far
view and in a `--stress-fx` or `--collapse` scene; lighting median both ways; screenshot next to the lava pit. Built
only if the counter shows instance visits matter.

**S1 Render scale** (existing). `--scale 0.5` (`Engine.cpp:29`), default 0.6 (`Renderer.cppm:23`): 640 x 360 is
0.69 of the pixels of 768 x 432 and every pass scales with pixels. The upscale is bilinear (`present.frag:20`).
Recommended as a preset for integrated GPUs until C5. Measured: far and near medians at 0.5 vs 0.6; screenshots.

**G2 Grid addressing** (new). `shaders/shared/trace.glsl:223` computes
`(((gridMin + cc) % gridSize) + gridSize) % gridSize` on every grid step (six integer modulos by a non-constant,
several ALU ops each on Gen9, e). Compute `phase = ((gridMin % gridSize) + gridSize) % gridSize` once before the loop
and use `w = phase + cc; w -= (w >= gridSize) ? gridSize : 0` (exact because `cc` stays in `[0, gridSize)`). The file
is shared with C++, so `ViewDistance.ColumnSkipMatchesTheTraversalWithoutIt`, `World.WindowSizeNeedNotBePowerOfTwo`
and the `Trace.*` tests cover it. Measured: medians, `INTEL_DEBUG=fs` instruction count.

**T1a Worker priority and cave throttle** (new; keeps the CLAUDE.md lesson that 2 jobs per worker starved
streaming). World workers at lower priority (`SCHED_BATCH` / `setpriority` in `WorkerLoop`); after `World: surface
filled`, cave rows limited to about two workers' share of `m_MaxJobs` (`World.cpp:145`, decided in `SubmitJobs`);
surface loading unchanged. Evidence of preemption: recentre is 0.16 ms of thread CPU time at the median but 1.3-2.3 ms
of wall time at the maximum in `--fly` (view-distance.md:313). Measured: `--fly` and `--climb` gate lines unchanged,
recentre max lower, GPU medians during the climb, `gt_act_freq_mhz` samples.

**T1b Jolt threads 1-2** (new, deferred). The draft argued "the client simulates few bodies, rubble capped at 192".
That is wrong today: the 192 cap and the lifetime apply only to `Debris` entities, which are pieces under
`debrisBodyVoxels` (`Scene.cpp:222`, `Scene.cppm:114-119`), while every island becomes rubble (`Blast.cpp:225-234`);
offline, the local `Simulation` is also the authority and simulates every body, and shred.md stage 1 frees many more
large pieces. So T1b is a **session-client setting only**, taken after shred.md stage 1e gives all rubble a global
cap, and only if a fight measurement (`--collapse` or `--stress-fx`: physics step p99 with 1-2 threads vs hw-1)
shows no loss. Offline keeps hw-1 Jolt threads.

**H1 Half-resolution lighting** (planned, needs its own design note first: it changes history formats). Trace and
accumulate at 384 x 216; upsample inside composite's `FilteredLighting` (`composite.frag:35-73`), which already
weights by depth and normal, so no extra pass. Saving (e) = about 0.75 x the lighting time left when H1 lands, minus
0.3-0.6 ms of upsample: from today's 7.3-9.3 ms that is 4-6 ms; after G1, G2 and L1 (far lighting about 5.2 ms, e)
it is about 3.5 ms.
History at half resolution (count R8 encoding, notes/graphics.md:93). Measured: lighting and composite medians far and
near, far and probe screenshots, a moving-camera check.

**L1 Distance budgets** (planned: lod.md:128-148, stage 1 at lod.md:183-188; view-distance.md stage 4 gate:
"lighting -2 ms facing a hillside, no change on a near view"). Unchanged here; the leaf-visit counter it asks for
first (lod.md:146-147) goes into M0.

**B1 Lighting trace call site** (new). `lighting.frag` inlines `VxTraceScene` three times (:152 sun, :178 AO, :239
point light); each copy carries `VxTraceWorld`, `VxTraceInstances` with `uint stack[32]` (`trace.glsl:46, 314`) and
`VxTraceChunk`. Build up to three ray descriptors and run one call in a loop, keeping the RNG draw order. Stack 16:
`BuildBvh` splits at the median, so 16 entries cover more than 30 000 instances; the overflow guard (`trace.glsl:335`)
stays. Measured: `INTEL_DEBUG=fs` (spills, SIMD width), lighting medians, `Trace.InstancesThroughBvhMatchBruteForce`.

**U1 Uploads** (new). Up to 128 chunk uploads per frame (`Renderer.cppm:42`) = 4.73 MB staged and copied on a UMA GPU
(`Renderer.cpp:953-971`); every edit re-uploads whole chunks. Add the timer (M0), then a byte budget nearest-first
when there is no backlog, then block-range uploads for edited chunks. The budget is sized from two measurements:
`--fly` (streaming) and shred.md stage 0's per-blast log (edited chunks and upload bytes per cast, est. 10-20 chunks =
0.4-0.75 MB for a blast that frees a tower), checked against fx-budget's 1 ms upload cap (fx-budget.md:60) at 10
casts/s. Measured: upload timer and max uploads per frame in both; `chunkUploadsPending` must drain.

**B2 History fetches** (new). The reprojection loop issues four `texelFetch`es per tap for 4 taps
(`lighting.frag:102-118`), skipped pixels included. `textureGather` for previous depth and count, one bilinear fetch of
`u_PrevLighting` when all four weights are valid. Measured: lighting median, moving-camera trails.

**C4, C3, C5** (planned ideas, each needs a design note): see ROADMAP.md:46-48, :284-288. C4's first step (step
counter `hit.steps` only in a debug variant, `trace.glsl:141, 219, 325`) is cheap and can be tried with B1. C3 must
be conservative and is tested on the CPU twin (hits equal with and without the start offset).

**B5, A5, B4** (settings or experiments): only after L1, because L1 already takes the far share of what they save;
A5 keeps period 2 inside any point light's radius (`lighting.frag:197-223`) or flashes smear.

**X1 Particles** (fights, with fx-budget): full `std::sort` of all live particles each frame (`fx/internal/
Particles.cpp:358`), a `std::function` solid query per colliding particle, est. 4-5 ms with the 32 768 pool full
(fx-budget.md:44). fx-budget stage 1-2 already shrinks the pool to 8192; do this only if `--stress-fx` shows particle
CPU over 1.5 ms after that (fx-budget.md:26).

**X2, F1, I1, J1** (CPU, small): X2 skips collider wake queries for chunks farther than `dropRadius` from every focus
and frozen body (`physics/internal/Terrain.cpp:581-592`). F1 adds `Renderer::WaitFrameSlot()` before `PollEvents`
(input is sampled at `Engine.cpp:139-147`, the wait comes after all CPU work). I1 keeps a per-tick previous pose and
draws `mix(previous, current, StepAlpha)`; today `GatherInstances` draws `transform.current` and only the player uses
`StepAlpha` (`src/main.cpp:338`). J1 splits the single FIFO job queue (`Jobs.cpp:82-88`) so collider cooks and island
retries do not wait behind up to 56 generation jobs.

**Blast spikes** are owned by shred.md, but they are what the owner feels as low frame rate in a fight, so this
draft depends on them: offline (today's game, where the client is the authority) one cast costs est. 5-20 ms of
main-thread CPU in one frame (carve, island search est. 4-12 ms optimised, fx-budget.md:41, face flood 0-6 ms,
`ClearMask`, rubble bodies, uploads), over fx-budget's 4 ms destruction cap by itself; a session client pays est.
0.5-3 ms per cast (shred.md, "Budgets", client table). shred.md stage 0 logs a per-blast total (CPU ms, edited chunks,
upload bytes) and **lands the safe speed-up now**: gathering the box by chunk instead of `GetVoxel` per voxel
(`voxel/internal/Connectivity.cpp:205-211`; keeps labels and order). Skipping the search when the carve removed few
voxels is **not** safe (one removed voxel can free an island). The fight gate (ROADMAP.md:162) cannot be judged until
that log and fx-budget's `--stress-fx 10` exist; U1 is sized from them.

## Stages

| # | Built | Accepted when | Effort (e) |
|---|---|---|---|
| 1 | M0 measurement | Headless far / near runs print CPU p50 / p99 / max, GPU sum, frames over 25 / 33 ms, upload ms; numbers recorded in this doc | 0.5 d |
| 2 | P1 present path | Owner: windowed fps with vsync on / relaxed / off recorded with the `Swapchain:` line; validation clean | 0.5 d |
| 3 | G1 + G2 | Far median at least -1.5 ms in total, near unchanged within 5 %; far and near screenshots equal; `Trace.*`, `ViewDistance.*` green; both headless runs exit 0 | 0.5-1 d |
| 4 | S1 preset + T1a | Scale 0.5 offered as the integrated-GPU default (owner decides); `--fly` and `--climb` gates pass, GPU medians during the climb within +0.5 ms of a filled window | 0.5 d |
| 5 | L1 = lod.md stage 1 (its debris threshold made presentation-only, shred.md Part 3) | lod.md's criteria (lighting -2 ms facing a hillside, no change near) | 2 d |
| 6 | H1 half-resolution lighting | Far frame <= 16.7 ms GPU at scale 0.6; near lighting -3 ms; screenshots reviewed by the owner | 3-5 d |
| 7 | B1, B2, U1, then C4 / C3 / C5 by measured need; G3 and T1b once a fight scene (`--stress-fx` / `--collapse`) exists | each: its own measurement above shows the saving; otherwise reverted | 1-2 weeks |

Expected path on the far view (e, midpoints, each item applied to what the earlier ones left; today gbuffer 12.2,
lighting 7.3, composite 1.3, TAA + present about 0.65 ms):

| After | gbuffer | lighting | Frame |
|---|---|---|---|
| today | 12.2 | 7.3 | 21.5 |
| stage 3: G1 (gbuffer -1.0, lighting -0.4), G2 (-0.5, -0.25) | 10.7 | 6.7 | about 19.4 |
| stage 5: L1 on what G1 left (lighting -1 to -2) | 10.7 | about 5.2 | about 17.9 |
| stage 6: H1 = 0.75 x 5.2 - 0.45 upsample | 10.7 | about 1.75 | **about 14.5 (14-15.5)** |

Stage 6's acceptance (far <= 16.7 ms) holds with about 2 ms of margin (e). After it, most of the far frame is gbuffer
(about 10.7 of 14.5 ms), which only G1, G2 and C3 / C4 touch; lighting items (B1, B2, A5, B4, G3) are then worth a
quarter of their listed saving. Stage 4's scale 0.5 alone reaches about 15-16.5 ms at once.

## Rejected or low value now

| Item | Why |
|---|---|
| Shorter sun shadow rays (`VX_SHADOW_DISTANCE = 1e4`, `lighting.frag:28`) | Rising rays already end above the content top and skip air by column / region prisms; 0-0.4 ms (e) for lost mountain shadows at low sun. |
| Coherent AO directions per 2x2 quad | -0.3 to -1 ms (e) for structured noise the 3x3 filter cannot remove. |
| Moving gbuffer / lighting to compute | `DescriptorWriter` has no storage-image writer (notes/graphics.md:178) and register pressure stays the same. |
| Per-pixel checkerboarding | No gain on Intel (CLAUDE.md lessons); the 8x8 tile pattern stays. |
| HUD work | 0.01 ms (ROADMAP.md:34). |
| More sky-pixel work | Sky pixels already return early in lighting (`lighting.frag:61-62`) and in the trace; G1 is the only sky-related win. |
| Point-light loop for the idle view | Matters in fights (fx-budget effect-light merge to 8), not on the idle peak view. |
| ImGui `NewFrame` / `Render` when hidden, `World::Stats` scans, `SubmitJobs` column scans, per-frame vector reallocations | Each under 0.1 ms (e); fold into whatever touches the code next. |

## Open questions for the owner (each with a recommendation)

1. **Vsync default on the UHD 620?** Recommendation: `FIFO_RELAXED` when available, FIFO otherwise; a vsync toggle in
   the pause menu later (game UI is HUD, not ImGui).
2. **Render scale 0.5 as the integrated-GPU default until TAA upsampling exists?** Recommendation: yes, after you look
   at a 0.5 vs 0.6 screenshot pair; keep 0.6 for the GTX 1650.
3. **Accept the slight emissive change of G3** (glow seen through a rubble piece)? Recommendation: decide only after
   the fight measurement; on the idle view it saves 0-0.3 ms (e), which is not worth a visible change. It is visible
   only next to emitters.
4. **Half-resolution lighting (H1) before or after lod.md stage 1?** Recommendation: after; L1 is designed and smaller,
   H1 needs a design note and changes history formats.
5. **Target for the far view on the UHD 620**: 60 fps (16.7 ms) or a stable 30 / 45 fps? Recommendation: a stable 16.7
   ms on the near view and under 20 ms far, with FIFO_RELAXED; fights are judged by fx-budget's own gate (no more
   than a fixed margin below idle, ROADMAP.md:160-163).
