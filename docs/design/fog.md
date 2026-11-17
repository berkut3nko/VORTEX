# Fog: sunlit haze, fog that lies in valleys and craters, blast dust that drifts and settles

Status: **design, nothing built or measured for it; reviewed once (see "Review of the design", end of file).**
Owner's request (2026-11-17): "add some easy volumetric simulation of fog (maybe dependent on dense maps)". Written
read-only against engine `71dc23e` / game `ad0fe69` (networking stages 0-7b committed, stage 8 being built). Numbers
are **(m)** measured (quoted from perf.md / view-distance.md / lod.md / notes), **(c)** computed from sizes, **(e)**
estimate; every (e) names the measurement that replaces it. "Dense maps" in the request is read as **density maps**:
2D maps of fog density built from the terrain.

Relation to other designs: [perf.md](perf.md) (frame budget and the measuring method used here),
[fx-budget.md](fx-budget.md) (blast presentation, dust particles, the 4 ms effects cap, its planned effect lights in
composite), [view-distance.md](view-distance.md) (window, edge fog, `--climb` / `--fly` gates), [lod.md](lod.md)
(fog density and the level-0 edge), [networking.md](networking.md) "Stage 6 as built" onwards (what a client already
shares with the server), [worldgen.md](worldgen.md) (domination, sea level) and the game's `docs/world/setting.md`
(the Brodvel Fens: "Fog limits sight").

## Decision in brief

1. **Fog is presentation only.** It lives in `vortex.fx` (GPU-free maps and simulation, with tests) and
   `vortex.graphics` / `vortex.engine` (one new pass, a second composite pipeline). Nothing in `sim::`, no `WorldOp`,
   no `sim::Blast` field, no wire message, never in `WorldHash`, `ChunkHash`, checks, casts, bots or any server
   visibility test. `vortex_server` never calls it. The one change below the engine is a read-only, non-destructive
   per-column revision in `voxel::World` (the fog must never consume the colliders' or chunk sync's change queues).
2. **Fair in PvP without replication.** Fog hides players, so every client must see the same fog. The fog is a function
   of what clients already share: the chunk-synced terrain inside the window, the casts they apply (every live
   `CarveSphere` operation, complete here or not), the `RegionDesc` (seed, `seaLevel`) and the server tick
   (`net::ServerTime`). The one stateful part, the simulation, runs on a grid anchored to the **world window** (not
   the viewer), with the window's edge as an inflow boundary, and relaxes to a target computed from the terrain
   within seconds, so two clients that started at different times converge (tested). **In a session the fog cannot be
   switched off or thinned.** Presets change resolution, step count and colour noise only: **extinction is the same
   function on every preset** (noise never touches it), and the ground-hugging terms are integrated analytically per
   march segment, so the step count changes horizontal detail, never whether a layer is hit.
3. **Density = analytic floor + bounded ground mist + local field.** The floor is today's `VxFogAmount` (exponential
   height fog over the whole ray and the sky), unchanged. The ground mist is a new `VxFogAmountClamped` (constant
   below its base, so caves and deep craters never white out). The local field is marched to the window's **view
   radius R** (horizontal; R from `jitter.zw`, 92 m offline at 0.125 m, 88 m on the arena client, **56 m on the
   playground session**), fades with the edge fog's own fade (1 - smoothstep(0.85 R, R)), and is clipped to the slab
   below the highest fog top. Local terms: a **fog layer** on the ground (valleys, craters, over water), **blast
   dust**, **cave mist** under cover. A per-style cap bounds the **added** extinction (ground mist + every local term)
   at every sample; the style table states transmittance with all terms, floor included (default: T >= 0.5 over 30 m
   near sea level).
4. **Density maps from the terrain, on the CPU, over the window.** A **terrain map** (top of solid H and sun-shadow
   height S, one texel per 8^3-voxel cell column, from `ChunkOccupancy::cellMask`) and a **fog grid** of 2 cells per
   chunk side (2 m at 0.125 m; 96 x 96 offline, 60 x 60 on the playground) with ground G, layer top, dust. Both cover
   exactly the window and move with it; their storage is toroidal for cheap shifts, but any lookup outside the window
   returns no fog and no cover, and nothing ever wraps across the window edge.
5. **An easy volumetric simulation, 2.5D, on the CPU.** The fog grid is stepped at 10 Hz: wind advection
   (semi-Lagrangian, inflow = target at the window edge), flow to lower neighbours by ground + fog height (fog pools
   in valleys and fills new craters), relaxation to the terrain-derived target, dust injection from blasts and dust
   decay. 9216 cells offline, est. 0.1-0.25 ms per step (< 0.03 ms per frame averaged). Rejected: a 3D grid, a GPU
   solver (no compute path exists).
6. **Rendering: one new fullscreen pass "fog" at reduced resolution**, between lighting and composite: a ray march
   of 8-24 segments over [max(0.5 m, slab entry), min(surface, horizontal R, slab exit)] with single scattering (sun
   through S, sky ambient reduced under cover, Henyey-Greenstein phase), a fixed per-pixel offset (no per-frame
   jitter), writing in-scatter + transmittance (RGBA16F) and the march end distance (R16F). Composite applies it with
   a nearest-depth upsample on every preset (`color * T + inscatter`). No 3D images, no compute, no storage images,
   no fog history in the first build.
7. **Composite is split, so `off` is today's shader.** The body moves to `include/composite.glsl`; `composite.frag` is
   today's shader (no fog bindings, the `off` pipeline); `composite_fog.frag` (`#define VX_LOCAL_FOG`) is used by
   `low` / `medium` / `high`. Reason: composite's SIMD16 variant has no registers to spare on Intel (ARCHITECTURE.md
   5.7: a 12 % slowdown of every pixel once), and a pipeline that declares a binding the renderer does not provide
   draws nothing.
8. **Stage 1 (about 4 days, est.) ships the cheapest useful fog with no new pass:** the composite split, a sun
   in-scattering lobe on the existing fog, the bounded ground-mist layer per region style, and up to 8 analytic
   blast-dust puffs (closed form, CPU-culled to screen rectangles, summed and capped), also applied to particles.
   Est. composite +0.05-0.08 ms idle, +0.1-0.15 ms with 8 puffs in view. Stage 2 adds the maps and the march, stage 3
   the simulation.
9. **Presets** `off` (offline only, today's look and today's shader), `low` (UHD 620 default and the session minimum),
   `medium`, `high` (GTX 1650). Gates on the UHD 620: stage 1 composite idle <= +0.08 ms and with 8 puffs <= +0.15 ms;
   stage 2 `low` fog pass <= 0.25 ms and composite <= +0.2 ms idle in all; CPU fog work p99 <= 0.35 ms per frame.

## The renderer today (what fog plugs into)

Verified in the working tree.

| Fact | Where | Consequence |
|---|---|---|
| Pass order gbuffer -> lighting -> composite -> particles -> TAA -> present (HUD, ImGui inside present) | `Renderer.cpp:1623-1627` | The fog pass goes between `PassLighting` and `PassComposite`; it needs only the G-buffer depth |
| Timed passes: `enum class Pass { GBuffer, Lighting, Composite, Particles, Taa, Present, Hud, Imgui }`, `kPassNames` | `Renderer.cpp:65-69` | One new entry `Fog` / `"fog"`; the exit log prints its median with the others (`Engine.cpp:280-296`) |
| Render scale 0.6: 768 x 432 = 331 776 px at 1280 x 720 output | `Renderer.cppm:23` | Fog at 1/4 x 1/4 = 192 x 108 (low), 1/2 x 1/2 = 384 x 216 (medium, high) |
| No `vkCmdDispatch`; images are 2D only (`CreateImage2D`, usage flags only as the caller passes); `DescriptorWriter` writes buffers and sampled textures, not storage images | `Resources.cpp:160`, `Shader.cppm:152-175`, perf.md | Fog must be fragment passes into 2D targets and CPU uploads. Froxels would need new infrastructure (rejected) |
| **No CPU-to-image upload path.** The staging path records only `vkCmdCopyBuffer` (materials, occupancy, ids, grid, after a `reserved` area for materials + grid); the only image copy is the screenshot's `vkCmdCopyImageToBuffer`. `RecoverFailedSubmit` resets the voxel mirror and the targets only | `Renderer.cpp:926-952, 1064-1081, 1511, 1556-1569` | Stage 2 adds a `CopyBufferToImage` helper, `TRANSFER_DST` usage, transitions and a re-upload flag (see "Uploads") |
| Two samplers, nearest and linear, both `CLAMP_TO_EDGE` | `Renderer.cpp:1782-1783` | The fog maps are read with explicit window bounds, so CLAMP is right; no new sampler |
| A pipeline whose declared binding the renderer does not provide is not bound and draws nothing (logged once) | `Renderer.cpp:1310-1326` | Hence the composite split; particles get a neutral 1 x 1 fog texture and an empty puff buffer when fog is off |
| Composite's SIMD16 variant has no spare registers: one change made every pixel 12 % slower | ARCHITECTURE.md 5.7, `composite.frag:116-117`, notes/graphics.md | Fog code never enters `composite.frag`; both composite variants are checked with `INTEL_DEBUG=fs` |
| `VxFogAmount` (shared GLSL / C++, analytic exponential height fog, closed-form segment integral, **no lower bound on height**) applied in composite to surfaces (`ApplyFog`), to the sky with a 5000 m segment (`SkyColor`), per particle vertex | `shading.glsl:151-166`, `composite.frag:205-219, 295`, `particles.vert:51-58` | The floor stays exactly as is. A steep second layer through it would grow without limit below its base: the mist gets its own clamped function |
| Edge fog: smoothstep 0.85 R -> R by horizontal distance toward `SkyColor`, R in `jitter.zw`; R = (half window - 1 - slack) chunks (`World::ViewRadius`) | `composite.frag:297-304`, `World.cpp:175-181` | The local field uses the same fade and ends at R. R = 92 m offline at 0.125 m (73.6 m at 0.1 m), 88 m on the arena client, 56 m on the playground (window = play bounds +-12 chunks + 3 margin = 30 chunks, `Region.cpp:97-104`) (c) |
| `FrameUniforms`: 3 mat4 + 21 vec4, `static_assert(sizeof == 3*64 + 21*16)`; offsets checked against SPIR-V | `Frame.cppm:23-49`, `common.glsl:33-58`, `tests/graphics/ContractTests.cpp` | No spare field: new vec4s on both sides plus the static_assert |
| Texture bindings start at 16 and are declared per pass; composite uses 16-20 (`u_GMaterial`, `u_GNormal`, `u_GDepth`, `u_Lighting`, `u_LightCount`); buffers 0-10 (10 = `u_Hud`) | `common.glsl:22, 130-157`, `composite.frag:11-15` | Bindings are resolved by name: the new names below are the contract |
| `temporalParams.w` = time (s), `counts.w` = frame index, `sunDirection`, `sunRadiance`, sky colours | `common.glsl` | Enough for the phase; fog time in a session comes from the server tick (below), not this field; no per-frame jitter is used |
| The game sets the sun at (0.45, 0.7, 0.35): elevation 50.8 deg, tan 1.23 (c); **the editor's Environment panel can change it** (azimuth, elevation sliders) | `src/main.cpp:132`, `Editor.cpp:719-731` | S stores the sun it was built for and is rebuilt (within its budget) when the sun differs; in a session the sun never changes (no editor) |
| `ChunkOccupancy::cellMask`: 64 bits, one per 8^3-voxel cell (1 m at 0.125 m); **any non-zero id sets it, leaves included** | `Chunk.cppm:27-42` | H at 1 m from one word per dense chunk; H includes tree crowns, so cover is tested against G (trees filtered out), never H |
| Change feeds are single-consumer: `World::TakeChunkChanges` / `TakeChangedChunks` ("either call empties it") feed `TerrainColliders::ConsumeChanges`; `TakeLoadedChunks` feeds `net::Client`'s chunk sync; `ChunkStore::TakeDirty` feeds the renderer. Dense in-chunk edits do not change `GridRevision` | `World.cppm:442-446`, `World.cpp:659-670`, `Terrain.cpp:305-309`, `Client.cpp:1153, 1587, 1863`, `Renderer.cpp:916` | The fog may call none of them; it reads a new per-column revision (see "The terrain map") |
| TAA: 3x3 min/max clamp of render-resolution neighbours, history weight 0.9 | `taa.frag:49-77`, `Renderer.cpp` `kTaaHistoryWeight` | A 3x3 box is 0.75 of a 1/4-res fog texel: per-frame fog noise passes almost unfiltered (clamped away). Hence no per-frame jitter |
| Composite 1.24-1.40 ms (far) / 1.83-2.15 ms (probe) (m), about 15 fetches per pixel; frame about 21.5 ms (m) | perf.md | **Per full-screen fetch about 0.04 ms**: an extra `u_LightCount` fetch per spatial-filter tap (9 taps) cost +0.3 ms (m, notes/graphics.md) |
| Particles in a session: a client presents a cast only when the operation was complete there (`present && out.complete`), `present` = presentation on and `seq > joinOpSeq` | `Client.cpp:995` (and `present` above it) | Puffs cannot hang on `PresentBlast` in a session: they take every live `CarveSphere` with `present`, complete or not |

## Density sources

All densities are extinction in 1/m. Scattering albedo 0.9 for fog, 0.6 for dust (browner, darker).

| Term | Where it comes from | Shape | Changes when |
|---|---|---|---|
| **Floor** (global) | `VxFogAmount`, today's parameters (`Frame.cppm:105-112`: 0.006/m, falloff 0.03/m, base 0, start 8 m) | Whole ray, sky included | Never (preset-independent) |
| **Ground mist** (stage 1, analytic) | New `VxFogAmountClamped`: d0 * e^(-k * max(y - b, 0)), integrated by splitting the segment where it crosses y = b (below: d0 x length; above: today's closed form). Per region style: d0, k about 0.3/m, base b at `seaLevel` + 1 m | Whole ray; at most d0 anywhere | Style only |
| **Fog layer** (stage 2 static, stage 3 simulated) | Fog grid: slab from G to G + f with a 1 m linear soft top; `f` relaxes to a target from the terrain | sigma_layer inside the slab, integrated analytically per segment | Terrain edits, window moves, wind, time |
| **Blast dust** | Stage 1: analytic puffs (spheres) from the applied casts; stage 3: when a puff ends it deposits dust into the grid | Puff: closed-form sphere density, summed and capped; deposited: a slab 0-4 m above G, integrated analytically | Each cast; decays (puff 2.5 s, dust tau 6 s, (e) tuning) |
| **Cave / cover mist** | Covered = y < G - 1 m (G: the ground with trees filtered out, below). Under a crown, G is the ground, so a tree is never cover; under a hill or a roof wider than 8 m it is | sigma_cave * smoothstep(1 m, 3 m, G - y): a slab below G, integrated analytically | Terrain edits |
| **Colour detail** (medium, high) | A hash value noise in the shader (as `WaterHash`; no texture), scrolled by wind x time | Multiplies the **in-scatter colour** by 0.8-1.2; **never the extinction** | Time |

**The cap.** Every style has sigma_add_max, the largest **added** extinction (ground mist + layer + dust + cave) at a
sample. The march takes, per segment, tau_local = min(sum of the slab integrals, max(sigma_add_max - mist(y_mid), 0)
x segment length), and the mist alone is bounded by d0 <= sigma_add_max, so a simulation bug can never make a wall of
fog and the cap holds in caves. The floor is today's look and is not capped; the table's transmittances include it.
Puffs have their own cap (the summed puff depth along a ray keeps centre transmittance >= 0.35).

**The target of the fog layer** (per grid cell, from the terrain map, CPU):

- G = ground: the 8 m minimum filter of H (removes trees, poles and thin towers, so fog does not sit on canopies).
- Hollow = clamp((ringMean24(G) - G - 1 m) / 6 m, 0, 1): worldgen.md's "domination" with the sign turned, from the
  live terrain, so a new crater is a hollow too. Taps outside the window are left out of the mean.
- f_target = style.base + style.valley * Hollow + (G <= seaLevel + 0.5 m ? style.water : 0), times 0.6 where S says
  the ground is sunlit (fog burns off in the sun, lingers in shade), clamped to style.maxThickness.
- fogTop = max over the window of (G + f + 1 m) and (G + 4 m where dust > 0): one scalar per upload, like
  `VX_GRID_CONTENT_TOP`; the march never spends a segment above it.

**Region style.** A small table in `vortex.fx`, chosen on the client from what it already has: offline the game's
setting (`--fog-style` to preview), in a session derived from the `RegionDesc` (template / generator ids and
`terrain.seaLevel`), so no wire change. Values are level-design values, recorded with the reason. Transmittances are
for a horizontal 30 m ray from the camera at y = 0 (c), floor included (its 8 m start leaves 22 m of it):

| Style | Ground mist d0 | f_target base / valley / water | sigma_layer / sigma_cave | sigma_add_max | T over 30 m, all terms | Reason |
|---|---|---|---|---|---|---|
| `default` | 0.006/m (b = sea + 1 m) | 0 / 6 m / 2 m | 0.015 / 0.015 | 0.017/m | >= 0.52 at sea level; >= 0.47 in a cave at -20 m (floor 0.011/m there) | Fog colours valleys but never hides a fight outside designed places |
| `fen` (Brodvel Fens, W5) | 0.02/m | 1.5 m / 4 m / 3 m | 0.045 / 0.04 | 0.054/m | >= 0.17 | "Fog limits sight" (setting.md); the tiered tower is the one mark above it |
| `clear` (tests, editor) | 0 | 0 / 0 / 0 | 0 / 0 | 0 | floor only | Pictures and tests without local fog |

**Wind.** w(t) = style.windSpeed (1-3 m/s) in direction theta0(seed) + 0.5 rad * sin(2 pi t / 300 s). t is the fog
time: offline the simulation tick x tick length; in a session `net::ServerTime::Now()` x `tickSeconds` (it only moves
forward and is the same on every client up to its smoothing). Wind moves the fog grid and scrolls the colour noise; it
touches nothing else (no particles, no bodies).

## The simulation (stage 3)

**Grid.** 2 x 2 cells per window chunk column (2 m at 0.125 m, 1.6 / 2.4 m at 0.1 / 0.15 m): 96 x 96 offline and on
the arena client, 60 x 60 on the playground. It is **anchored to the world window** and recentres with it by whole
chunks (2 cells); storage is toroidal (positive modulo), so a shift rewrites only the entered rows and columns, which
start at their target (f = f_target, dust 0). Per cell (CPU floats): ground G, layer thickness f, dust d, target
f_target. Targets are rebuilt only where the terrain map changed.

**The edge.** The grid's world-space edge (the window's) is an **inflow boundary**: a back-trace that leaves the grid
reads the cell's own f_target and dust 0; no settle flux crosses the edge; nothing reads the opposite side. On the
playground every client's window is the region window, so every client runs the identical grid; on a moving window
(offline, the arena) the edge's influence reaches wind x tau_relax <= 3 m/s x 8 s = 24 m inward (c), at the window
edge, mostly under the edge fog.

**Step** (fixed dt = 0.1 s, 10 Hz on the fog time; at most 2 steps per frame, the rest dropped - only below 20 fps):

1. **Advect** f and d by the wind: semi-Lagrangian bilinear back-trace (Stam 2003), unconditionally stable; inflow at
   the edge as above.
2. **Settle**: for each 4-neighbour pair inside the grid, flux = k_flow * ((G + f)_i - (G + f)_j) * dt, out of the
   higher side only, limited to a quarter of the giver's f (mass-conserving, stable with k_flow * dt <= 0.25). Fog
   runs downhill, fills hollows and fresh craters, and spreads flat over level ground (this is the diffusion). Dust
   uses the same rule with k_flow / 4 (heavy dust creeps down slopes).
3. **Relax and decay**: f += (f_target - f) * (1 - e^(-dt / 8 s)); d *= e^(-dt / 6 s).
4. **Inject**: a puff ends after 2.5 s and deposits its dust (a Gaussian of radius r + 2 m) into d. Puffs come from
   the applied casts on every client (below), so every client injects the same dust at the same place.
5. **Clamp** f to [0, style.maxThickness], d to [0, dmax]. Non-finite values reset the cell to its target (a test
   makes sure it never happens).

**Why it stays fair.** Floats and different start times mean two clients' grids are not bit-equal, so the grid is
built to forget its history: the relaxation (tau 8 s) pulls any state to f_target, which is a pure function of the
shared terrain, and dust decays with tau 6 s from injections every client makes. The grid's position is the window's,
never the viewer's. Tests: `FogSim.ForgetsItsStart` (two grids, same window, different states, same inputs: layer tops
within 0.1 m after 40 s) and `FogSim.ViewerIndependent` (two windows 40 m apart over the same terrain, wind and time:
layer tops within 0.1 m after 40 s wherever both cells are more than 24 m from their edges); stage 3's session check
compares the clients' layer tops at fixed points.

**Cost (e).** About 80 operations per cell and step: 9216 x 80 = 0.74 M operations, est. 0.1-0.25 ms per step, at
10 Hz under 0.03 ms per frame averaged. Upload 96 x 96 x RGBA16F = 72 KiB per step (720 KiB/s) (c). The shader
blends the last two states (`u_FogSimPrev`, `u_FogSim`) by the step fraction so 10 Hz never shows. Measured by the
`Fog:` exit line (max and average step ms).

**Where it runs.** `vortex.fx:fog` (`src/fx/Fog.cppm`, `internal/Fog.cpp`; fx and voxel are both in
`Vortex::Core`, check at build time that `vortex.voxel` does not import `vortex.fx`). The engine owns one `FogField`,
feeds it the world, the fog time, the sun and the puffs, and gives its textures to the renderer. GPU-free and tested;
the server links Core but never constructs it.

## The terrain map

| Map | Texel | Size | Built from | Cost (e) |
|---|---|---|---|---|
| H: top of solid | One 8^3 cell column (1 m at 0.125 m; 0.8 / 1.2 m at 0.1 / 0.15 m) | Window chunks x 4: 192 x 192 offline, 120 x 120 on the playground | Walk each chunk column down from its column top: Empty continues, Uniform / Packed counts as solid (as the grid writes them), Dense reads `cellMask` (one 64-bit word gives the 16 cell columns' tops). Pending: the column top (a top may be too high, never too low, like the grid) | 2304 chunk columns x 1-3 chunks: < 0.1 ms full rebuild |
| S: sun shadow height | Same | Same | S(p) = max over s in (0, 64 m] of H(p + s * sunXZ) - s * tan(elevation), 64 taps at 1 m; the walk stops at the window edge (no wrap), as the lighting pass's shadow rays do. A point at height y is sunlit when y > S. 64 m reach covers 79 m of relief at 50.8 deg (c) | **2.36 M taps per full rebuild** (192^2 x 64), est. 2.4-4.7 ms at 1-2 ns per tap; incremental (below) |
| G, Hollow, f_target | The fog grid's cell | 96 x 96 / 60 x 60 | Min filter 8 m (van Herk, separable), ring mean 24 m (separable box), both over the window only | < 0.05 ms |

H and S go to the GPU as one RG16F texture `u_FogTerrain` (144 KiB offline (c)); half floats hold render-space
heights -48..128 m to 1/16 m at the top (c), finer than the 1 m texel. The window never moves vertically, so a height
in the map stays valid across shifts. H is used for S only; cover and the fog layer use G.

**Change feed (non-destructive).** `voxel::World` gets `ColumnRevision(chunkX, chunkZ)`: the largest
`ChunkRevision` of the column's cells, raised in `BumpRevision` (one `uint64_t` per window column: 2304 offline). The
`FogField` keeps its own copy and compares it every frame, est. < 0.01 ms; a changed column rebuilds its H, and
`WindowRevision` changes bring in the entered columns. The fog never calls `TakeChunkChanges`, `TakeChangedChunks`,
`TakeLoadedChunks` or `ChunkStore::TakeDirty` (each has one owner: colliders, chunk sync, the renderer). Tests:
`World.ColumnRevisionFollowsEveryCellChange` and `FogField.LeavesTheChangeQueuesAlone` (with a `FogField` attached,
`TerrainColliders` and `ChunkSync` receive the same lists as without it).

**S refresh.** S is dirty in a rectangle from the changed columns 64 m down-sun (sun xz (0.79, 0.61) (c)) and is
rebuilt row by row on the main thread from what is left of the **fog CPU budget of 0.3 ms per frame** after the H
rebuilds and this frame's simulation step (est. 0.1-0.25 ms on a step frame), at least one row (192 x 64 taps,
est. 12-25 us). A walking 4 m shift toward the sun dirties about 50 rows: **0.61 M taps**, est. 0.6-1.2 ms, so 3-12
frames; a blast dirties a strip of a few texels. Rows within R of the camera go first; entered rows lie at the
window edge, beyond R (96 m offline vs R = 92 m), where the local fog is 0, so a late S there is never seen. S stores
the sun direction it was built for; when `engine.sun.direction` differs (the editor), all of S is dirty and rebuilt
within the same budget (est. 2.4-4.7 ms, 10-40 frames). The exit line logs the rows behind at most (`--climb` /
`--fly` must show 0 within 0.85 R).

**Why not the grid's column tops**: one word per 4 x 4 m chunk column in 4 m rows: fog would pool in 4 m steps and
shafts would be blocky. **Why not a 3D density map** (96 x 88 x 96 at 2 m = 811k texels): rebuild cost and memory,
and a 3D image path the renderer lacks, for nothing the "below G - 1 m = under cover" rule does not already give.
Its errors: a sunlit cave mouth reads as covered; a cave within 4 m (horizontally) of lower ground, and a room
narrower than 8 m, read as open. Accepted.

## Rendering

**Stage 1 (composite, no new pass).**
- **The split.** `include/composite.glsl` holds today's body; `composite.frag` includes it unchanged (the `off`
  pipeline: its preprocessed source equals the previous commit's, checked in stage 1 by comparing SPIR-V); the new
  `composite_fog.frag` defines `VX_LOCAL_FOG` and adds everything below. The renderer picks the pipeline by preset.
- `ApplyFog` (fog variant) gets a sun lobe: fog colour + sunRadiance * fogSun.y * HG(dot(rd, sunDir), g = 0.6). The
  flat grey turns into a haze that glows toward the sun. Sky pixels get the same lobe.
- The ground mist: `VxFogAmountClamped`, mixed after the floor.
- **Puffs**: up to 8 `VxFogPuff`s. The CPU frustum-culls them and writes each one's bounding rectangle in render
  pixels; the shader skips a puff after 4 compares. Inside the rectangle, the closed-form optical depth of the sphere
  along [0, t] (Quilez, "sphere density"); `VxFogPuffDepth` sums all puffs and caps the sum at ln(1 / 0.35) before
  one exp, lit by sun + ambient, plus a short warm glow in the first 0.2 s (the flash, no light).
- **Particles** (`particles.vert`, one shader for every preset): the lobe, the clamped mist, and the summed, capped
  puff depth from the camera to the particle (the same `VxFogPuffDepth` loop), dimmed and tinted as composite does.
  With fog off the renderer binds an empty puff buffer and the lobe and mist scales are 0. Est. < 0.05 ms at 8192
  particles (fx-budget.md: about 800 per blast once its budget is in, 3300 before).
- **Cost (c / e).** 8 puffs over the whole screen would be 8 x about 20 ALU x 332k px = 53 M lane-operations, at
  least 0.24 ms at the UHD 620's about 221 G lane-operations/s (24 EU x 8 lanes x 1.15 GHz); the rectangles cut it
  to the covered pixels. Est. composite +0.05-0.08 ms idle (lobe + clamped mist), +0.1-0.15 ms with 8 puffs over a
  third of the screen.
- **Sessions.** A puff is made from every live `CarveSphere` operation the client applies with `present` (presentation
  on, `seq > joinOpSeq`), **whether or not `out.complete`**: it needs only the operation's sphere. A client still
  syncing or repairing a chunk under the cast shows the same smoke as everyone else. Offline: `Engine::PresentBlast`.

**Stage 2 (the fog pass).**

| Item | Choice | Reason |
|---|---|---|
| Place | New pass `fog` after lighting, before composite; its own timer | Needs only `u_GDepth`; composite applies it |
| Resolution | low 192 x 108 (1/4 x 1/4), medium and high 384 x 216 (1/2 x 1/2) | Fog is low-frequency; fill is what costs on this GPU |
| Depth per fog pixel | From its block (4 x 4 low, 2 x 2 medium / high) by an alternating checkerboard: even fog pixels take the block's nearest depth, odd ones its farthest | Near and far surfaces both get fog texels; the upsample picks the right one |
| March range | [max(0.5 m, slab entry), min(surface, the t where the ray's horizontal distance reaches R, slab exit)]; slab = y below `fogTop` | Steep and high rays spend no steps in clear air; nothing beyond R, where the edge fog has faded to sky |
| Segments | low 8, medium 12, high 24; exponential spacing over the range | Dense near the camera, where fog is seen through least depth |
| Per segment | Read G, f, d, H, S at the midpoint (1 `u_FogTerrain` fetch, 2 sim fetches prev / now); any xz outside the window gives no fog and no cover; integrate the slab terms (layer with its 1 m linear top, dust band, cave slab) **analytically over the segment's height range**; apply the cap; times the edge fade 1 - smoothstep(0.85 R, R, horizontal distance at the midpoint); sun = smoothstep(S - 0.5, S + 0.5, y_mid) * HG phase * sunRadiance; ambient = mix(skyHorizon, skyZenith, 0.5) * ambient intensity * (covered ? e^(-(G - 1 - y_mid) / 4 m) : 1); energy-conserving in-scatter (Hillaire 2015) | A layer is never hit-or-miss; shafts from mountains, towers and walls; dark mist in caves, none under trees |
| Offset | A fixed 4 x 4 interleaved per-pixel offset of the segment boundaries; **no per-frame term on any preset** | TAA's 3x3 clamp cannot remove per-frame noise at 1/4 or 1/2 resolution; a fixed pattern does not flicker, and the bilinear upsample blends neighbouring offsets |
| Output | MRT: RGBA16F (rgb in-scatter, a transmittance) + R16F march end distance, every preset | The distance drives the upsample |
| Temporal | **No fog history** (stages 2-3) | A history brings reprojection, blast-flash trails (UE's known issue) and another pair of targets; added only if the fixed pattern shows (open question 4) |
| Upsample in composite | Every preset: one `textureGather` of the 4 neighbouring march distances; if all 4 are within 10 % of the pixel's own reference distance (min(surface t, horizontal-R t)), one bilinear colour fetch, otherwise one `texelFetch` of the texel whose distance is closest | No fog outline on a near silhouette (a player 10-20 m away in front of valley fog); 2 fetches, est. 0.08 ms (about 0.04 ms per fetch, m-derived) |
| Composition | `color = ApplyFog(surface)` (floor + ground mist + puffs), then the edge fog, then `color * T + inscatter` | The local field already carries the edge fade, so it agrees with the edge fog and is 0 at R; surfaces and sky alike |
| Sky pixels | Marched to horizontal R (inside the slab) | That is where the shafts show |
| Particles | Stage 2: the vertex shader reads `u_Fog` (and its distance) at the particle's screen position, one fetch, and scales it to the particle's distance: T^(d_p / d_march) for transmittance, the same share of in-scatter (1 - T^(d_p / d_march)) / (1 - T) | Debris inside fog is dimmed like the surface behind it; a neutral 1 x 1 texture when fog is off |
| HUD | Untouched: drawn after present on the display values | The HUD must stay readable in any fog |
| Feature bit | `VX_FEATURE_LOCAL_FOG = 256`; off: the pass is skipped, its targets are not allocated, composite runs `composite.frag` | `off` costs 0 and is today's binary |

**Density function**, shared GLSL / C++ in a new `shaders/shared/fog.glsl` (compiled as C++ through `vx_cpp.h`, as
`trace.glsl`): `VxFogAmountClamped`, `VxFogPhase`, `VxFogPuffDepth` (summed, capped), `VxFogSlabDepth` (a linear
height segment through a slab with linear ramps), `VxFogLocalSegment(...)` and `VxFogMarch(...)`. The CPU tests
integrate the same code; the presets call the same functions with different segment counts.

**Uploads (stage 2, est. +0.5-1 day).** `u_FogTerrain`, `u_FogSim`, `u_FogSimPrev` are created with
`TRANSFER_DST | SAMPLED`; a new `CopyBufferToImage` helper takes per-row regions (a toroidal dirty band needs at most 2
regions); the images go `SHADER_READ_ONLY -> TRANSFER_DST -> SHADER_READ_ONLY` around the copies (`TransitionImage`
supports both). The fog bytes take a place in the staging `reserved` area next to the materials and the grid. A
"re-upload all" flag is set by `RecoverFailedSubmit` and whenever the images are (re)created, so rows recorded in a
frame that never ran are sent again. There is no noise texture (the noise is a hash in the shader).

## Quality presets

All costs on the UHD 620 at 768 x 432 internal unless said; all (e) until stage 2 measures them.

| Preset | What | GPU (e) | CPU (e) | Where |
|---|---|---|---|---|
| `off` | Today's floor fog only; today's `composite.frag`; no pass | 0 | 0 | Offline only. A session client asking for `off` gets `low` and logs it |
| `low` | Stage 1 terms + fog pass 192 x 108 x 8 segments, no colour noise, nearest-depth upsample, shafts on | pass 0.1-0.2 ms; composite +0.15-0.2 ms in all (stage 1 terms + upsample) | <= 0.3 ms per frame (budget), < 0.05 ms typical | **UHD 620 default; session minimum** |
| `medium` | 384 x 216 x 12 segments, one colour-noise octave | pass 0.3-0.7 ms, composite as low | same | UHD 620 when the frame has room (lod.md / perf.md savings) |
| `high` | 384 x 216 x 24 segments, two colour-noise octaves | UHD 620 0.9-1.6 ms (not for it); GTX 1650 0.15-0.35 ms | same | GTX 1650 default |

Why the presets are fair: same density function and cap, same maps, same simulation, noise on colour only, slab
terms integrated analytically. `FogShading.PresetsAgreeOnTransmittance` bounds what is left **per ray**: |T_8 -
T_256| <= 0.03 on every ray of a set crossing 2-10 m layers at 30 m to R with `fen` and dust densities over level
ground, and <= 0.1 per ray (mean within 0.02) on rays over generated terrain, where the 8-segment march reads G at
fewer places (est. bounds: the test's first run sets them, and a larger error means more segments for `low`, not a
looser bound).

**How each is measured** (perf.md's method): headless, `--frames 1200`, no validation, medians of the last 120 frames
after `World: window filled`, two runs each, far view `--camera valley` and near `--camera probe`, plus one
`--validation` run per change (exit 0) and screenshots. **The baseline is `--fog off` of the same build**, valid
because `off` runs `composite.frag`, whose SPIR-V equals the previous commit's (stage 1 checks it once, and its
medians against perf.md's far 1.24-1.40 / probe 1.83-2.15 ms). Shader width: `MESA_SHADER_CACHE_DISABLE=true
INTEL_DEBUG=fs` on `composite.frag`, `composite_fog.frag` and `fog.frag`: each `SIMD16` line must say `scheduled
with mode top-down` with no new spills. fx-budget.md's planned effect lights land in the same composite and need the
same check. On the GTX 1650 the owner reads the same `GPU fog` median; an assistant cannot run it.

**Gates** (UHD 620): stage 1 composite on the far and probe views within +0.08 ms of `--fog off`, and within
+0.15 ms with 8 live puffs over about a third of the screen (`--fog-blast 8`); stage 2 `low` fog pass <= 0.25 ms (m)
and composite within +0.2 ms of `--fog off` idle; `medium` pass <= 0.6 ms; CPU fog work p99 <= 0.35 ms per frame (the
0.3 ms budget plus one S row); flicker: `--fog-flicker` mean |fog_N - fog_(N-1)| under 2 % of the mean in-scatter on
a still camera; all existing headless gates unchanged (`Beam cut`, `HUD`, `on the ground`, `--climb`, `--fly`, the
session checks).

## Contracts touched (change both sides together)

| Contract | Change |
|---|---|
| `FrameUniforms` (`common.glsl` / `Frame.cppm`) | Six new members at the end: `fogSun` (x phase g, y sun in-scatter scale, z ambient scale, w `fogTop` (render-space m)), `fogMist` (the clamped mist: d0, k, base, start), `fogLocal` (x sigma_layer, y dust scale, z sigma_cave, w sigma_add_max), `fogMap` (xy window min xz in render space, zw window size m), `fogWind` (xy wind m/s, z sim blend fraction, w noise scale), `ivec4 fogCounts` (x puffs, y segments, z preset, w reserved). `static_assert(sizeof == 3*64 + 27*16)`; `FrameUniformsMatchTheGlslBlock` covers the new offsets |
| New buffer binding 11 | `layout(std430) readonly buffer FogPuffBuffer { VxFogPuff puffs[]; } u_FogPuffs;` with `VxFogPuff { vec4 centerRadius; vec4 colorDensity; vec4 pixelRect; }` mirrored in C++ and checked like `VxHudQuad`; at most 8; composite_fog and particles |
| New texture names (resolved by name: renaming breaks the C++) | fog pass: `u_GDepth` (16), `u_FogTerrain` (17), `u_FogSim` (18), `u_FogSimPrev` (19); `composite_fog` and particles: `u_Fog` (21), `u_FogDistance` (22) |
| Pipelines | `composite` (off) and `composite_fog` (low, medium, high) from `include/composite.glsl`; `fog` |
| `voxel::World` | `ColumnRevision(chunkX, chunkZ)`, raised in `BumpRevision`; read-only for every consumer |
| Feature bits | `VX_FEATURE_LOCAL_FOG = 256` in `common.glsl` and `FeatureFlags` |
| Debug view | `VX_DEBUG_FOG = 8` (transmittance as grey, in-scatter in colour) in `common.glsl` and `DebugView`, an early exit in `composite_fog.frag` |
| Pass timers | `Pass::Fog`, `"fog"` in `kPassNames` (the exit log and any script reading it) |
| `shading.glsl` / new `fog.glsl` | Shared GLSL / C++: only constructs valid in both languages (as `trace.glsl`) |
| `ARCHITECTURE.md` | Renderer section: the fog pass, the composite split, the textures, the fog field in `vortex.fx`, `ColumnRevision` |

**Not touched, on purpose:** `WorldOp`, `sim::Blast`, every message and `kProtocolVersion`, `WorldHash`,
`ChunkHash`, checks, `RegionDesc` (until open question 2), `vortex_server`, the layering asserts, every
single-consumer change queue.

## Headless flags (game)

| Flag | Meaning |
|---|---|
| `--fog off\|low\|medium\|high` | Preset (default `low`). With `--connect`, `off` becomes `low` and logs `Fog: off is offline only, low in a session` |
| `--fog-style default\|fen\|clear` | Offline only (refused in a session like `--voxel-size`): previews a region style |
| `--fog-wind X,Z` | Offline only: fixed wind in m/s (screenshots that do not move) |
| `--camera valley` | Alias of perf.md's far view `-1,61,65,-0.241,-0.55` ("peak over the valley"): the **performance** view only; its valley lies beyond R |
| `--camera fog-slope` | Stage 2: a pose 20-40 m above the lowest dry ground within 60 m of the spawn, looking at it (found from `TerrainHeight` like `--climb`; the found pose is written here); the fog acceptance view |
| `--fog-blast [N]` | Offline script: N blasts (default 3) in view of `--camera fog-slope` (stage 1: of `--camera valley`) at script frame 30 (with `--after-fill` after the window filled); logs `Fog: dust ... at +1 s / +4 s / +8 s` (grid dust total, puffs alive); `--fog-blast 8` is the puff cost gate |
| `--fog-flicker` | Offline: reads the fog target back on each of the last 120 frames and logs `Fog: flicker mean D of mean in-scatter I` (a still `--camera` only) |
| `--fog-debug` | Screenshot shows `VX_DEBUG_FOG` |

Exit log: `Fog: preset P style S, terrain map N column rebuilds (max X ms), S rows rebuilt R (behind at most B inside
0.85 R), sim N steps (avg / max ms), uploads K KiB, puffs at most P, CPU p99 C ms`, and `fog` in the GPU pass
medians.

Examples (for whoever builds it; not run for this design):

```
./build/Shredborne --headless --validation --frames 900 --after-fill --camera fog-slope --fog low --screenshot fog-low.png
./build/Shredborne --headless --frames 1200 --camera valley --fog off      # baseline medians
./build/Shredborne --headless --frames 1200 --camera valley --fog low      # fog + composite medians
./build/Shredborne --headless --frames 1200 --camera valley --fog-blast 8  # puff cost gate
./build/Shredborne --headless --validation --frames 900 --after-fill --camera fog-slope --fog-blast --screenshot dust.png
./build/Shredborne --headless --frames 600 --camera fog-slope --fog-flicker
./build/Shredborne --headless --validation --frames 600 --camera probe --fog medium --fog-style fen --screenshot fen.png
```

## Stages

Every stage passes the standard build (warning-free), `ctest`, both headless validation runs with their log lines,
`tools/session-check.sh` (`PASS`), and is reviewed before its commit; ROADMAP.md gets the state and test count.

1. **Analytic extras (about 4 days, est.).** The composite split (`include/composite.glsl`, `composite.frag`
   unchanged, `composite_fog.frag`, pipeline choice by preset); `fog.glsl` with `VxFogAmountClamped`, phase and the
   summed puff integral; `FrameUniforms` +6 members and the puff buffer (with pixel rectangles); sun lobe and clamped
   mist in `composite_fog` and particles; puffs in composite and particles; `FogPuffs` in `vortex.fx` (8 slots,
   lifetimes, oldest replaced, frustum culling and rectangles) fed by `Engine::PresentBlast` offline and by every
   applied `CarveSphere` with `present` in a session; presets and styles plumbing, `--fog`, `--fog-style`,
   `--camera valley`, `--fog-blast [N]` (puff part), the session rule for `off`; the `Fog:` line.
   *Cost (e):* composite +0.05-0.08 ms idle, +0.1-0.15 ms with 8 puffs; particles < 0.05 ms; CPU negligible.
   *Accept:* `FogShading.PhaseIntegratesToOne`, `FogShading.PuffMatchesNumericIntegral` (closed form vs 1024-step
   sum, within 1 %), `FogShading.OverlappingPuffsKeepTheCap` (8 identical puffs: centre T >= 0.35),
   `FogShading.MistBelowItsBaseIsBounded` (a horizontal ray 30 m long at y = -20 m: optical depth exactly 30 x d0),
   `FogShading.MistMatchesNumericIntegral` (rays crossing the base, within 1 %), the ContractTests; `composite.frag`'s
   SPIR-V unchanged; both composite variants SIMD16 top-down without new spills; the stage 1 gates above (m);
   screenshots: the haze toward the sun brighter than away from it, a puff at +1 s and gone at +8 s in `--fog-blast`
   with its debris dimmed inside it; a session client with `--fog off` logs `low`.
2. **Terrain map and the fog pass (6-8 days, est.).** `World::ColumnRevision`; `FogField` terrain map over the window
   (H, S with the sun check, incremental refresh, the shared 0.3 ms budget); the static fog grid on the window (f =
   f_target, no stepping) and `fogTop`; the upload path (helper, transitions, re-upload flag); the `fog` pass, MRT and
   timer, the nearest-depth upsample, particles reading `u_Fog`, presets `low` / `medium` / `high`, `VX_DEBUG_FOG`,
   `--camera fog-slope`, `--fog-flicker`.
   *Cost (e):* fog pass low 0.1-0.2 ms; composite +0.15-0.2 ms in all over `off`; CPU <= 0.3 ms per frame.
   *Accept:* `FogMap.HeightEqualsVoxelScan` (H equals a voxel-by-voxel scan rounded up to the cell, Packed and
   Uniform solid, pending at the column top), `FogMap.ShadowHeightMatchesRays` (S against brute-force rays toward
   the sun within the reach, stopping at the window edge), `FogMap.ShiftEqualsFullRebuild`,
   `FogMap.EditRebuildsOnlyItsColumns`, `FogMap.SunChangeRebuildsS`, `FogMap.TreesAreNotCover` (under a generated
   tree, 1.5 m below its crown: cave mist 0, ambient at full strength), `FogMap.CaveIsCover`,
   `FogMap.OutsideTheWindowIsClear` (a 30-chunk window as the playground's, viewer 12 m from its edge: every lookup
   beyond the edge gives no fog and no cover, and `VxFogMarch` adds nothing there),
   `World.ColumnRevisionFollowsEveryCellChange`, `FogField.LeavesTheChangeQueuesAlone`,
   `FogShading.PresetsAgreeOnTransmittance` (per ray, above), `FogShading.SlabMatchesNumericIntegral`,
   `FogShading.DensityNeverExceedsTheCap` (mist + local <= sigma_add_max at any height, caves included),
   `FogShading.LocalFogEndsAtR`; the gates above (m) on the UHD 620; `--climb` / `--fly` still pass and log 0 S rows
   behind inside 0.85 R; screenshots: fog lying in the valley from `--camera fog-slope`, shafts behind a ridge or the
   watch tower, dark mist in a cave and none under trees, the gold figure 10-20 m away in front of valley fog with no
   outline (a probe-area screenshot with the figure placed by the offline script), no halo around the probe
   furniture at `--camera probe`; `tools/session-check.sh`'s `a.png` / `b.png` show no fog past the window edge.
3. **The simulation (3-4 days, est.).** Stepping (advect with inflow, settle, relax, decay), dust deposit from ended
   puffs, wind from the fog time (`ServerTime` in a session), the two-state blend; `--fog-wind`; dust lines in
   `--fog-blast`.
   *Cost (e):* 0.1-0.25 ms per step, < 0.03 ms per frame averaged; upload 72 KiB per step.
   *Accept:* `FogSim.ConservesMassAtZeroWind` (no relaxation, decay or sources), `FogSim.InflowIsTheTarget`,
   `FogSim.FillsACrater` (a bowl's fog top flat within 0.2 m after 30 s), `FogSim.StableForAnyWind` (0-30 m/s,
   10 000 steps, finite and bounded), `FogSim.ForgetsItsStart`, `FogSim.ViewerIndependent`, `FogSim.DustHalfLife`,
   `FogSim.RecenterEqualsAFreshGrid`; `--fog-blast` logs dust rising after the puffs end and falling to under 10 %
   by +16 s; a screenshot after `--neck-blast` or the beam cut shows fog in the new crater; CPU p99 <= 0.35 ms per
   frame (m); `tools/session-check.sh` additionally compares both clients' fog layer tops at 8 fixed points after
   the drain: within 0.25 m (each client logs `Fog: probe tops ...`; on the playground both grids are the region
   window's).
4. **Only if the owner asks after seeing stage 3.** A fog history (reprojected, reset under flashes) if the fixed
   pattern shows; local fog on the editor's preview; a GTX 1650 tier with full-resolution fog; a way to place a
   session client's camera for an edge-of-play-area screenshot. Each with its own measured cost.

## Risks

- **The fixed per-pixel pattern** may show as a faint static texture on smooth fog; the analytic slabs leave little
  of it. Judged on screenshots of a moving camera (owner) and a still one plus `--fog-flicker` (headless). Fallback:
  stage 4's history.
- **Silhouettes**: the nearest-depth upsample removes the far-fog outline; a thin object narrower than a block (a
  pole, a rail) may still take a neighbour's fog. Accepted.
- **Bodies and players are not in H**: rubble casts no shafts and fog flows under a resting piece. Accepted.
- **Shafts through 1-voxel walls** are limited by the 1 m cell texel; `blockMask` (0.5 m) is the fallback if a
  reviewer sees light through walls in screenshots (est. 2x the H build, still < 0.2 ms).
- **Coarse far detail on `low`**: 8 segments over up to R read G at few places; the per-ray terrain bound catches a
  layer that is badly placed, and the answer is more segments for `low`.
- **Fog time in a session** follows `ServerTime`, which is smoothed; two clients' winds differ by their offsets
  (ticks), a few centimetres of drift. Accepted; the session check's 0.25 m tolerance covers it.
- **Moving windows** (offline, the arena): within 24 m of the window edge two clients' grids may differ (inflow);
  that band is mostly under the edge fog (0.85 R to R). Accepted (open question 8).
- **A terrain-mode client** (server terrain, networking 7b M-b) builds its maps from the same voxels; hidden Packed
  chunks read as solid on both sides, as they do in the grid.

## Rejected alternatives

| Rejected | Reason |
|---|---|
| Froxel volumes (Wronski 2014, Hillaire 2015, UE, Godot) | Est. 1-2 ms or more on the UHD 620 (UE High is 1 ms on a PS4, about 4x this GPU (e)); needs compute, storage images and 3D images the renderer lacks; reprojection trails behind blast flashes. Revisit as a GTX 1650 tier only if many lights in fog are wanted |
| A 3D density grid (2 m, 811k texels) | Memory, rebuild cost, a 3D image path; the "below G - 1 m is covered" rule gives caves and large interiors |
| A 3D or Navier-Stokes fog simulation (full Stam solver) | Pressure solve, cost, and nothing visible that the 2.5D layer misses for fog on the ground; harder to keep fair |
| Simulation on the GPU | No compute path; fragment-pass ping-pong is possible but untestable on the CPU and the grid costs est. < 0.25 ms on the CPU |
| Shadow maps for sun visibility in fog | A depth pass from the sun costs gbuffer-scale time; the height-map shadow (S) is exact enough for terrain |
| Screen-space god rays (radial blur from the sun) | Wrong when the sun is off-screen or occluded, and gives no fog in valleys |
| Fog from billboard particles | Fill rate is this GPU's limit (fx-budget.md: dust alone est. 3 screens of blending) |
| Replicating fog state from the server | Bytes and server work for a picture; the shared inputs already make it fair |
| Fog as a server-side visibility rule (bots, line of sight) | Not needed now; would make fog gameplay state and tie it to the server's terrain. Revisit with AI (M-later) |
| A fog setting that thins fog in a session | Unfair in PvP; presets change detail only |
| Noise in the extinction | Different presets would see through different patches (T 0.34 / 0.17 / 0.08 over 30 m of fen at noise 0.6 / 1 / 1.4); noise colours the in-scatter only |
| Point-sampled fog layer (hit or miss per step) | Biased toward clear on few steps (a 10 m pocket at 35-45 m: expected T 0.72 at 8 steps vs 0.55 true): `low` would see farther. Slabs are integrated per segment |
| Per-frame jitter with TAA as the denoiser | TAA's 3x3 render-pixel clamp is smaller than a fog texel and clamps the history away: strobing. Fixed per-pixel offsets instead |
| A fog grid centred on the camera / the own player | Wraps fog across its edge and makes fog depend on where each viewer stands; anchored to the window |
| A fixed 64 m fog range | Hides valley fog from every high view and overruns R on the playground (56 m) and at 0.1 m voxels (73.6 m); the range follows R |
| Fog code inside `composite.frag` behind runtime branches | `off` would run the same binary, and a register-allocation slowdown would hit both sides of the gate unseen |
| Reading `TakeChunkChanges` / `TakeLoadedChunks` for the terrain map | Single-consumer queues: colliders would miss edits, or chunk sync would miss loads |
| Per-pixel checkerboarding of the fog pass | No gain on Intel (CLAUDE.md lesson); the pass is already at reduced resolution |
| A tileable noise texture | Needs an image upload for 4 KiB of data; a shader hash (as `WaterHash`) costs a few ALU and no binding |

## Open questions (each with the recommended answer)

1. **May fog hide players outside designed fog regions?** Recommended: only softly. With every term counted (floor,
   ground mist, local field, cap), the `default` style keeps T >= 0.52 over a horizontal 30 m near sea level and
   about 0.47 in a cave at -20 m (c); only `fen` (and later designed styles) goes thicker, T >= 0.17 over 30 m. Puffs
   (a breach's smoke) are the one short exception, at centre T >= 0.35 however many overlap.
2. **Does the fog style travel on the wire?** Recommended: not now (derived from the `RegionDesc` the client has);
   add a `fogStyle` byte to `RegionDesc` with the first region that needs a different style (Brodvel Fens, W5), with
   a protocol bump then.
3. **Blast dust as cover: how dense and how long?** Recommended: summed puff depth capped at centre T >= 0.35, puff
   2.5 s, then low dust with tau 6 s under the style cap. Fun (a smoke screen for a breach) without a blind wall;
   tuned on screenshots.
4. **Fog history (temporal accumulation)?** Recommended: no, and no per-frame jitter either; a reprojected history
   (reset where the light changed fast) only if stage 2 screenshots of a moving camera show the fixed pattern.
5. **Will the sun move (day / night)?** Recommended: fixed in play, as now. The editor may move it: S is then rebuilt
   whole within the 0.3 ms budget (est. 2.4-4.7 ms over 10-40 frames). A day / night cycle would need S on a worker
   thread; decide with that feature.
6. **Colour noise on `medium` / `high`?** Recommended: yes, on the in-scatter colour only (0.8-1.2), never on the
   extinction, so it changes looks and not visibility.
7. **Default preset?** Recommended: `low` on every machine until the owner chooses; `high` named for the GTX 1650
   once its cost is measured there.
8. **Do the edge differences of moving windows matter?** Recommended: accept them. On the playground (fixed region
   window) all clients' grids are identical; on the arena and offline, two players' grids can differ only within
   24 m of their own window edges (wind x relax time), almost all under the edge fog. Revisit if a region wider than
   48 chunks becomes a PvP map with fog styles.
9. **Where does fog sit on the roadmap?** Recommended: its own small track after networking stage 8 closes M1 (one
   build directory; no wire change, so it never blocks or waits on networking); stage 1 first, the owner looks at it
   before stage 2.

## Review of the design (critic, 2026-11-17)

A critic read the first version; every finding was checked against the code (paths relative to the engine) and all
thirteen hold. What changed, and the parts not taken with the reason:

| # | Finding | Verified | Change |
|---|---|---|---|
| 1 | The ground mist grows without limit below its base (0.27/m at -10 m, 5.4/m at -20 m) in dry caves (`caveMaxDepth` 30 m, `Generator.cppm:91-99`); the stated transmittances left out floor and mist | `shading.glsl:151-166` has no lower bound; the figures were 0.5 / 0.17 for the capped field only | `VxFogAmountClamped`; the cap is on the added extinction (mist + local), the style values were lowered so the stated T (0.52 / 0.17, floor included) holds; `MistBelowItsBaseIsBounded`. Of the two offered cap options the first was taken; the floor stays uncapped (today's look) and is counted in the figures instead |
| 2 | A fixed 64 m range: valley fog unseen from high views, and past R (56 m) on the playground, reading wrapped terrain | perf.md's far view looks down 61 m at pitch -0.55 (ground about 110 m along the ray); playground window 30 chunks (`Region.cpp:97-104`), R 56 m (`World.cpp:175-181`) | Range = horizontal R with the edge fog's fade; slab clip by `fogTop`; maps and grid sized to the window, lookups outside it give nothing, S stops at the edge; acceptance view `--camera fog-slope`. **Not taken:** a playground-session screenshot from the edge of the play area - `--camera` is refused in a session and a client cannot be placed; replaced by `FogMap.OutsideTheWindowIsClear` on a 30-chunk window (the shader's code is the same `VxFogMarch`), and the session-check screenshots; placing a session camera is a stage 4 item |
| 3 | Noise only on medium / high changes visibility; the 8-step point-sampled march is biased toward clear | Design text; Jensen's inequality on exp of a hit-or-miss sample (expected T 0.72 vs 0.55) | Noise on colour only; slab terms integrated analytically per segment; per-ray test bounds |
| 4 | Fog code in the one `composite.frag` risks the SIMD16 schedule for every pixel and `off`; gates against `--fog off` of the same binary cannot see it; undeclared bindings draw nothing | ARCHITECTURE.md 5.7, `Renderer.cpp:1310-1326` | Composite split; SIMD16 check on both variants; fog targets not allocated when off. **Changed:** the baseline stays `--fog off` of the same build rather than a build of the previous commit, because after the split `off` is the previous commit's shader (SPIR-V compared once); this avoids rebuilding an old commit in the one build directory for every measurement |
| 5 | Puff cost 5-10x low; stacked puffs multiply to a blind wall; a client skips puffs for incomplete operations | 53 M lane-ops (c); per-puff cap only; `Client.cpp:995` | CPU rectangles, summed cap, an 8-puff gate, puffs from every live `CarveSphere` with `present` |
| 6 | A camera-centred toroidal grid wraps and depends on the viewer | Design text | Window-anchored grid, inflow edge, new tests replacing the mass test |
| 7 | Farthest-depth fog pixels plus one bilinear tap outline near silhouettes; the per-fetch calibration was inconsistent | Design text; 0.25-0.43 ns x 332k px = 0.08-0.14 ms, not 0.025-0.04 ms | Checkerboard near / far depths, R16F distance on every preset, nearest-depth upsample; per-fetch cost now cited from the measured +0.3 ms for 9 taps (about 0.04 ms) |
| 8 | "Below H = covered" puts dark cave mist under every tree | `cellMask` counts leaves (`Chunk.cppm:27-42`); trees on a 7 m grid, crowns to 7.5 m | Cover is y < G - 1 m; H for S only; `TreesAreNotCover`, `CaveIsCover`. The offered "ceiling" channel was not taken: G already exists and is enough |
| 9 | No non-destructive change feed; the obvious ones are single-consumer | `World.cpp:659-670`, `Terrain.cpp:305-309`, `Client.cpp:1153` | `World::ColumnRevision`; test that the queues are untouched. The offered change ring with cursors was not taken: one word per column is simpler and enough for a map that rebuilds whole columns |
| 10 | TAA cannot remove per-frame fog jitter at 1/4 resolution; float frame counter loses precision; no headless flicker check | `taa.frag:49-77` clamps to render-pixel 3x3 | Fixed 4 x 4 per-pixel offsets on every preset (no frame term, so no precision issue); `--fog-flicker` gate |
| 11 | No buffer-to-image upload path; failed submits would leave fog rows stale | `Renderer.cpp:1064-1081, 1511, 1556-1569` | "Uploads" plan (+0.5-1 day), re-upload flag; the noise texture is dropped for a shader hash |
| 12 | Particles ignore puffs and local fog | `particles.vert:51-58` | Stage 1: summed puff depth in particles; stage 2: particles read `u_Fog`. Open question 6 (particles) closed. The 3300-particle figure is fx-budget.md's pre-budget count (about 800 once its stage lands); the conclusion holds either way |
| 13 | S tap counts were half the real ones; S's budget alone filled the CPU gate; the editor can move the sun | 192^2 x 64 = 2.36 M (c); `Editor.cpp:719-731` | Numbers corrected; S takes what is left of one 0.3 ms fog budget after H and the sim step (instead of the offered 0.2 ms fixed share, which would idle on frames with no step); gate p99 0.35 ms; S stores its sun and rebuilds on a change |

