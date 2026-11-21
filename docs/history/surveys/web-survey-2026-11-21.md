# Web survey of 2026-11-21 (engine documents)

Moved verbatim from the engine documents named in each heading below, as the owner added them in engine `4f7c3fe`
("docs: restructorized", 2026-11-21, on `9cbfdaa`), when that commit was merged into `engine-rewrite` on 2026-11-21.
The owner decided that current documents keep no survey text (decision 4 of 2026-11-21); each section sat at the top
of its document, under the title. The game's survey sections (its `docs/two-pc-test-windows.md`, `docs/world/setting.md`
and the LLM-generated `docs/ARCHITECTURE.md`) are in the game repository's
`docs/history/surveys/web-survey-2026-11-21.md`. The text below is unchanged; corrections go only under "Errata" at
the end.

## Note: claims a read-only audit (2026-11-21) found false or unverifiable

No claim was checked online (the audit had no network). Against the repositories:

- **False.** ARCHITECTURE.md, "Pinned third parties": `cmake/Dependencies.cmake` does not use `EXCLUDE_FROM_ALL`
  (0 occurrences; its `FetchContent_Declare` calls give `GIT_TAG` only).
- **Dangling reference.** ARCHITECTURE.md, "Vulkan setup boilerplate": `docs/ARCHITECTURE.md "Simplification
  opportunities" 1 and 5` is a section of the game's LLM-generated `docs/ARCHITECTURE.md` (itself moved to the game's
  history), not of the engine's ARCHITECTURE.md.
- **Stale reference.** perf.md: "roadmap cost list #2" (the half-resolution depth pre-pass) is no longer in ROADMAP.md
  after the WP3 rewrite (history/roadmap/before-rewrite-2026-11-18.md); the item is perf.md C3.
- **Wrong reference.** view-distance.md: "stage 5 there" - view-distance stage 5 (`[vd-5]`) is the exact visibility
  flood; the distance cascade / LOD levels are `[vd-6]` and lod.md.
- **Unsourced or unmeasured numbers and judgements.** perf.md: TSR "near-native quality at roughly half the frame
  time"; shred.md: the load check is "more principled and cheaper per shot" (not measured) and Teardown's physics is
  "deterministic" (not verified); collapse.md: "industry-consensus" (no source); worldgen.md: Nested WFC cuts
  generation "from exponential to polynomial time" (arXiv:2509.09919, not checked) and "Albion Online's ~430 zones"
  (a fan site); ROADMAP.md: the Khronos quote (not checked), "25 546 stars, last push 2026-10-15" (volatile, not
  checkable) and Gambetta's series giving "latency budgets in ticks/ms" (doubtful); networking.md: "shipped voxel
  MMOs send authoritative terrain to joiners" (no source).
- **Weak sources.** deepwiki.com (an AI-generated wiki) and a personal blog (ARCHITECTURE.md); devops-geek.net and a
  Unity forum thread (shred.md); reddit and bugnet.io (game-ui.md, perf.md); salivity.github.io (fx-budget.md);
  mysimulator.uk and albiondatabase.com (worldgen.md); a personal site and a commercial asset's documentation (lod.md,
  view-distance.md).

# From `docs/ARCHITECTURE.md` (lines 10-34 at `4f7c3fe`)

## How this build layout is done optimally elsewhere (web survey, 2026-11-21)

- **GPU-free testable core, thin GPU shell.** Khronos' *Building a Simple Game Engine* architecture series splits an
  engine into non-graphical services (state, resources, task system) with the renderer behind a narrow interface, so
  the logic builds and runs without a device ([architecture introduction](https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/01_introduction.html),
  [architectural patterns](https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/02_architectural_patterns.html)).
  A multi-API RHI layer (Unreal's RHI thread, [parallel rendering](https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine))
  is what an engine pays when it must ship several backends; for one backend the consensus is one backend plus a
  headless path. This engine buys the same boundary cheaper: `vortex_assert_gpu_free` / `vortex_assert_no_network`
  enforce it when CMake configures (section 2), no abstraction layer.
- **Vulkan setup boilerplate.** The mainstream answer is Vulkan-Hpp RAII handles ([vk::raii guide](https://github.com/KhronosGroup/Vulkan-Hpp/blob/main/docs/VkRaiiProgrammingGuide.md),
  [RAII wrappers](https://deepwiki.com/KhronosGroup/Vulkan-Hpp/4.1-raii-wrappers)) or a bootstrapper ([vk-bootstrap](https://charles-lunarg.github.io/vk-bootstrap/), already pinned here). Both
  trade per-handle dispatchers and error handling for less `vkCreate*`; keeping raw handles in the hot render path and
  VMA for memory is still normal practice, so the split that matters is setup vs. per-frame recording
  (docs/ARCHITECTURE.md "Simplification opportunities" 1 and 5).
- **Named modules, one per directory** (interface `.cppm` + `internal/*.cpp`): matches the toolchain vendors' guidance
  (named modules over header units once C++20 is available, [MS comparison of inclusion methods](https://learn.microsoft.com/en-us/cpp/build/compare-inclusion-methods),
  [a practice guide](https://daniel-petrovic.github.io/blog/cpp20-modules-practical-guide/)). The trap they all name
  is the BMI/toolchain mismatch, which is why every target here links `vortex_options`.
- **Pinned third parties**: FetchContent at a tag with `EXCLUDE_FROM_ALL` is the standard CMake form
  ([FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html)); `cmake/Dependencies.cmake` follows it.
- **One traversal source for CPU and GPU** (`shaders/shared/trace.glsl` compiled twice) is the cheap variant of the
  "single source of truth for data layout" the Khronos series pushes; where others generate the CPU mirror from the
  shader side with a compiler, this engine guards the mirror with a layout test (`tests/graphics/ContractTests.cpp`)
  and a DDA reference.

# From `docs/ROADMAP.md` (lines 8-26 at `4f7c3fe`)

## How this kind of roadmap is run optimally elsewhere (web survey, 2026-11-21)

- **Write the architecture down first and keep it as the contract** - the Khronos engine-architecture series states
  the same reason this project writes a design doc before each stage ("good architecture creates a foundation that
  supports your development process rather than fighting against it",
  [introduction](https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/01_introduction.html)).
  The extra step this roadmap already takes - every stage ends in a second reader's review - is the version of code
  review that single-author engines skip and pay for later.
- **Milestones ordered by what unblocks measurements**: the shipped voxel-server practice is to separate what a peer
  must *see* from what the sim must *tick* (Minecraft's `view-distance` vs `simulation-distance`,
  [server optimization guide](https://github.com/YouHaveTrouble/minecraft-optimization)); M1 (netcode) before M3
  (collapse) and the 48-chunk window with a 32 m "current" gate follow the same rule - pay for the near ring only.
- **Numeric gates instead of adjectives** (join p99 < 15 ticks, preload < 10 s / 150 MiB RLE, `hashes equal`) are the
  form the netcode literature uses too (Gambetta's series gives latency budgets in ticks/ms,
  [client-server architecture](https://www.gabrielgambetta.com/client-server-game-architecture.html)); a gate a
  headless run can fail is worth more than a milestone described in words.
- **Reference implementations stay the maintained ones**: the worldgen design's WFC reference is still the live one
  ([mxgmn/WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse), 25 546 stars, last push 2026-10-15 as
  checked here), so M4 needs no new external dependency.

# From `docs/design/networking.md` (lines 5-9 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- Authoritative server + client prediction keyed by input sequence numbers + reconciliation by replaying unacked inputs + remote-entity interpolation + rewind hit tests is the canonical recipe ([Gambetta, Fast-Paced Multiplayer II](https://www.gabrielgambetta.com/client-side-prediction-server-reconciliation.html), [IV, lag compensation](https://www.gabrielgambetta.com/lag-compensation.html)). This design already matches it (integer inputs per tick, 30 Hz snapshots, own player predicted through `sim::StepPlayer`, others interpolated); rewind hit registration is the open piece (M2 "lag-compensated hits").
- Session-per-region servers at MMO scale are a shipped pattern: per-zone instances with a decoupled simulation layer on Photon ([Unity's Albion Online case study](https://unity.com/blog/albion-online-cross-platform-pvp-mmo-architecture), [architecture talk, Quo Vadis 2016](https://www.slideshare.net/slideshow/albion-online-software-architecture-of-an-mmo-talk-at-quo-vadis-2016-berlin/62724504)) — the same shape as M1: one authoritative server per region instance, clients stream their own window.
- Joining by chunk sync (stage 7a) is the standard answer to "bit-identical generation across toolchains is fragile": shipped voxel MMOs send authoritative terrain to joiners rather than trusting both sides' generators. Server terrain mode (7b) is that fallback, and keeping the terrain golden tests as a release gate (two-pc-test-windows.md gate 2) matches treating identical generation as an optimization, not an assumption.

# From `docs/design/shred.md` (lines 5-9 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- Teardown is the shipped reference for arbitrary voxel destruction: connectivity search over voxel data plus rigid bodies for released chunks, no per-voxel stress analysis, deterministic voxel physics as the engine's core asset ([engine autopsy](https://devops-geek.net/nerd-space/dissecting-the-teardown-engine-a-technical-autopsy-of-voxel-destruction-pipelines/), [Tuxedo Labs developer on destruction + multiplayer](https://80.lv/articles/teardown-developer-breaks-down-multiplayer-and-voxel-destruction-tech)).
- A per-voxel "structural integrity" value recalculated on damage is the naive community variant ([Unity discussions thread](https://discussions.unity.com/t/want-to-recreate-teardown-destructable-voxel-physics/828648)); the cell-graph + max-flow load check here is more principled and cheaper per shot, and its integer-exactness is what a session server needs on top.
- Where server-authoritative collapse is done at all, the consensus cheap variant is: bake support data per prefab at build time, the server releases only cells / a few large bodies, clients add local debris — the same route as collapse.md M3's baked support graph and this design's piece budgets (2 per tick, 64 awake).

# From `docs/design/collapse.md` (lines 5-7 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- See [shred.md](shred.md) § "How others solve this optimally": shipped voxel games (Teardown) do connectivity search + released rigid bodies and bake support data per prefab; per-voxel integrity values are the naive variant. M3's baked support graph per prefab is the industry-consensus cheap path.

# From `docs/design/view-distance.md` (lines 5-8 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- Coarse voxel LOD beyond the full-resolution chunk ring is the standard pattern ("Super Chunks": render-only stepped terrain LOD built on worker threads past the visible distance, [Voxel Play 4 docs](https://kronnect.com/docs/voxel-play-4/super-chunks/)). Packed RLE columns + the region-top tables are the same idea already present as data — a distance cascade read from the pyramid tops is the natural next level (stage 5 there).
- Sparse octrees with per-node distance LOD on a deferred pipeline ([Voxceleron2](https://voxceleron2.ayanali.net/)) and GPU-driven voxel streaming + LOD on consumer hardware ([Aokana, arXiv:2505.02017](https://arxiv.org/html/2505.02017v1)) both converge on the same policy this design uses: stream what the camera needs, coarsen the rest, load surfaces before caves.

# From `docs/design/lod.md` (lines 5-7 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- The industry pattern is a small number of coarse, render-only voxel LOD levels beyond the full-res ring, built off the main thread ([Super Chunks](https://kronnect.com/docs/voxel-play-4/super-chunks/)), octree splits by camera distance ([Voxceleron2](https://voxceleron2.ayanali.net/)), or GPU-driven LOD + streaming ([Aokana](https://arxiv.org/html/2505.02017v1)). Distance budgets on the existing chunk pyramid (stage 1 here) is the cheaper first step all of these converge to; none of them keep full-resolution detail at range.

# From `docs/design/fx-budget.md` (lines 5-8 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- The modern default for large particle counts: simulate in a compute shader, render with one instanced indirect draw whose instance count is written by the GPU, cull with frustum planes + a Hi-Z depth pyramid, and stream-compaction the survivors into a packed visible buffer — no CPU readback ([GPU instancing and indirect drawing for particle systems](https://salivity.github.io/game-development/article/gpu-instancing-and-indirect-drawing-for-particle-systems)).
- A CPU pool with one instanced draw is the cheap variant this engine picks for the UHD 620; the budget rules are the same everywhere: cull live particles by distance / occlusion before shading them, cap per effect and globally, give everything a lifetime. The piece this design has not yet adopted from the consensus list is occlusion / distance culling of the live set before the draw — the natural step when stage 2 moves effect lights out of the traced pass.

# From `docs/design/perf.md` (lines 5-8 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- The standard weak-GPU answer is to render well below the output resolution and reconstruct temporally: TAAU / TSR / DLSS / FSR / XeSS all work this way, TSR reaching near-native quality at roughly half the frame time ([Unreal's anti-aliasing and upscaling docs](https://dev.epicgames.com/documentation/unreal-engine/anti-aliasing-and-upscaling-in-unreal-engine)). 768x432 internal at 1280x720 output with temporal accumulation is already this pattern; the known next gain is making the last step an upscale rather than plain AA. TAA robustness state of the art: [NVIDIA RTR temporal-antialiasing survey](https://research.nvidia.com/labs/rtr/publication/yang2020survey/).
- Draw-call discipline is the same on every integrated GPU: collapse many small quads / instances into single instanced draws, atlas what shares a texture ([draw-call optimization overview](https://bugnet.io/blog/optimizing-draw-calls-for-better-game-performance)). The HUD, particles and voxel chunk uploads here already follow it; a half-resolution depth pre-pass (roadmap cost list #2) is the other standard item these guides converge on.

# From `docs/design/game-ui.md` (lines 5-7 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- Efficient game HUDs converge on: one instanced draw for all HUD quads, a single glyph/icon atlas, and minimal interleaving of distinct textures or fonts ([draw-call optimization](https://bugnet.io/blog/optimizing-draw-calls-for-better-game-performance), [glyph-atlas text rendering, r/opengl](https://www.reddit.com/r/opengl/comments/15s4h0d/strategies_for_efficient_text_rendering/), [instancing basics, LearnOpenGL](https://learnopengl.com/advanced-opengl/instancing)). The HUD here (data-only canvas + one instanced draw after tone mapping + the 5 x 9 bitmap font + backdrop dimming in the present pass) is exactly this recipe; keeping game UI out of an immediate-mode debug GUI is the same separation everyone else enforces.

# From `docs/design/worldgen.md` (lines 5-8 at `4f7c3fe`)

## How others solve this optimally (web survey, 2026-11-21)

- Rule-driven assembly of hand-made parts is a shipped pattern, and WFC is its most general solver: extract adjacency rules from a small hand-authored sample, collapse the lowest-entropy cell first, and on a contradiction restart with a new seed — fast restart beats backtracking at game scale; shipped in Bad North, Caves of Qud, Townscaper ([mxgmn/WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse), [practical builder's guide](https://www.mysimulator.uk/content/articles/wfc-procedural.html)). Nested WFC cuts large-scene generation from exponential to polynomial time ([arXiv:2509.09919](https://arxiv.org/pdf/2509.09919)) — the same hierarchy as RegionPlan -> plot -> structure here.
- The integer `RegionPlan` stamped identically on every side is stronger than typical WFC usage: determinism by construction instead of by seed. Region types (arena / zone) with player caps, spawn points and bedrock as data, not code, matches what shipped zone-based MMOs do (Albion Online's ~430 zones, [zone map DB](https://www.albiondatabase.com/interactive-map)).

# Errata

None yet.
