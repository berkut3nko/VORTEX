# Effects that stay fast in fights

Status: design decision, nothing built or measured for it yet. "Measured" = `docs/ROADMAP.md`
(UHD 620, 768x432 internal, 21 ms GPU per frame). "Est." = from reading the code; stage 1
replaces every estimate with a number. Related: `collapse.md` (object damage, debris lifetimes),
`lod.md` (distance budgets).

## Decision

A cast is split into two halves with opposite overload rules:

- **Simulation** (carve, object damage, island search, colliders, bodies): queued, coalesced,
  bounded per frame, **never dropped**.
- **Presentation** (particles, light): same frame as the cast, **scaled or dropped, never delayed**.

One engine-side class, `FxDirector`, owns the queues and fixed per-frame caps. Effect lights
leave the ray-traced lighting pass and are evaluated unshadowed in `composite.frag` (at most 8
after merging). Particles stay on the existing CPU pool, shrunk to 8192, with effect LOD, a load
factor and a pixel overdraw budget. Effects become data (`EffectDesc`) with caps the director
enforces.

Rejected, and why:

| Rejected | Reason |
|---|---|
| Stateless GPU particles (closed-form emitters, proposal B) | Removes CPU cost that is est. 1 ms at 8192 particles; the limit on this GPU is fill, which it does not touch. New binding, shared evaluator, per-emitter sort. Revisit only if stage 1 shows particle CPU > 1.5 ms after stage 2. |
| Tiled effect-light grid (B) | With 8 merged lights a plain loop is est. < 0.5 ms; a tile buffer adds two bindings for no gain at that count. |
| Learned-cost budget + GPU governor (B) | Static caps plus the pool load factor bound the frame; a feedback loop is hard to test headless. Caps are `EngineConfig` fields, so a governor can drive them later. |
| `EffectEvent` network record, server-side library (B) | No multiplayer on the roadmap. The simulation/presentation split keeps the door open. |
| Half-resolution particle target, async island search | Kept as conditional stage 6: only if stage 1 numbers demand them. |
| Deferring nothing but shrinking counts (A, stage 1 only) | Does not bound the worst frame: ten casts still run ten island searches and N whole-volume splits in one frame. |

## How it works

### Where one blast costs time today (verified in code)

Reference: `engine.Explode(pos, 1.2f, 6.0f)`, 12-voxel radius, all synchronous in `Engine::Explode`.

| # | Cost | Fact | Size |
|---|---|---|---|
| 1 | Island search | `ExtractFloatingIslands` with margin `radiusVoxels + 24`; runs per blast, always; no minimum island size | 72^3 = 373k `GetVoxel` + flood fill; est. 4-12 ms. Cubic in radius; refused above 192 per axis |
| 2 | Object damage | `Scene::Damage` copies the whole volume (`model->Volume()`) and labels all of it per hit | Scales with object size, not blast size; five hits on one object = five full passes |
| 3 | Flash light | Scene entity, radius `8 * r`; `lighting.frag` loops to `VX_MAX_LIGHTS = 256` and traces one extra shadow ray for the chosen light; result goes through 24-frame history | Est. 2-4 ms when the flash covers the view (sun shadow ray: 4.7 ms measured); flash arrives smeared |
| 4 | Particles | Up to 400 `SpawnVoxelBreak` (6 debris + 2 dust each) + about 120 from `SpawnExplosion` | About 3300 per blast; 10 blasts fill the 32768 pool, later casts are invisible. Dust alone is est. 3 screens of RGBA16F blending at 5 m |
| 5 | Particle CPU | `std::sort` over all live particles; `std::function` solid query per colliding particle per frame; `particles.frag` reads the SSBO per fragment for the shape flag | Est. 4-5 ms with a full pool |
| 6 | Colliders | Up to 32 active bodies become focus points with the full 12 m radius; `NeedsScan` is true whenever the focus count changes; every edit dirties all 6 neighbours | Est. 2-4 ms scans while debris flies |

Defects found on the way (all affect the owner's point 1):

- **Collider starvation.** `MarkDirty` bumps `generation` and clears `cooking`; `InstallResults`
  drops any result that is not the newest. A chunk edited every frame (a beam) never gets a new
  collider while a cook takes longer than a frame: bodies rest on stale ground.
- **Damaged objects blink.** `Damage` bakes pieces into new slots; the renderer skips instances
  whose slot is not uploaded yet (`Renderer.cpp:1101`), so the object vanishes for 1+ frames.
- **Debris never dies.** Nothing gives a piece or a terrain island a `Lifetime`; a 1-voxel island
  becomes an entity with a body and a 37 KiB chunk slot.

### Targets (UHD 620, fight = 30 fps)

GPU: effects add at most 4 ms (particles 2.5, effect lights 0.5, uploads 1.0, lighting pass 0).
CPU main thread: destruction + effects at most 4 ms per frame.

### `FxDirector` and its caps

`Engine::Explode` keeps its signature. It does the presentation half, the impulse and wake
immediately, and enqueues the rest; `Engine::Run` calls `director.Tick()` after the fixed-step
block. Caps are `EngineConfig::fx` fields (start values, tuned in stage 1):

| Work | Cap per frame | Coalescing | Over the cap |
|---|---|---|---|
| Terrain carve | 150k voxels visited (about eight 1.2 m blasts) | - | FIFO, max 6 frames late |
| Island search | 1 region, 2 frames after its last carve | Overlapping pending regions merge into one box (max 192 per axis); margin 24 -> 12 voxels (72^3 -> 48^3, 3.4x less); skipped if the carve removed < 32 voxels | Queue of 8; when full the two oldest merge |
| Object damage | 3 entities, nearest first | All shapes hitting one entity in a frame: one copy, one label pass | Max 0.25 s late, then run without debris particles |
| New bodies | 8 | Pieces/islands under `minPieceVoxels` become one particle puff, never an entity | Wait |
| Collider cooks from edits | 4 submitted | Neighbour re-cooked only if the edit box touches that face | Nearest first |
| Edited chunk uploads | 16, ahead of streaming uploads | - | Wait |
| Particle spawns | 1500; 8192 alive | - | Dropped; additive layers before dust |
| Effect lights | 8 on GPU | Merged | Weakest dropped, replaced by a glow sprite |

Live caps and lifetimes use the values of `collapse.md` (`minPieceVoxels = 24`,
`debrisLifetime = 8 s`, `rubbleLifetime = 30 s`, expire only when asleep, oldest sleeping body
evicted above `maxLiveCollapseBodies`); terrain islands follow the same rules.

Latency cost: falling pieces appear 1-3 frames after the hit (hidden by the fireball). The
nearest three damaged objects are always same-frame.

### Effect LOD and load factor

Chosen once at spawn (no mid-life popping):
`s = boundRadius * renderHeight / (2 * distance * tan(fovY / 2))` pixels.

| LOD | Condition | Particles | Colliding debris | Dust/smoke | Light |
|---|---|---|---|---|---|
| 0 | s > 60 px, or the player's own cast | x1 | yes | yes | yes |
| 1 | 15-60 px | x0.5 | no | x0.5, lifetime x0.7 | yes |
| 2 | 4-15 px | x0.15, fire only | no | no | low merge priority |
| 3 | < 4 px, or behind the camera beyond 20 m | 0 | - | - | none |

Load factor `k = clamp(1 - (alive / capacity - 0.5) * 1.6, 0.2, 1)` multiplies every count: the
tenth simultaneous blast spawns a fifth of the first instead of nothing. Debris sampling drops
from 400 to 96 voxels with dust on every fourth: about 800 particles per LOD 0 blast instead of
3300, so ten blasts fit in the 8192 pool.

### Particle cost

- Simulation stays on the CPU. Sort only alpha-blended particles; additive ones are appended
  unsorted. One draw call as today.
- Overdraw budget: while building render data, sum `(2 * halfSize * pxPerMetre / dist)^2`.
  Budget 2.0 screens (660k px). Over it, alpha particles are thinned by a stride, oldest first,
  with opacity raised by the same factor so smoke thins instead of popping.
- Sprite clamp: projected size at most 96 px; opacity fades to 0 between 1.0 m and 0.4 m from the
  camera (no full-screen quads when standing in smoke).
- Shape flag as a flat varying. No soft particles, no shadowed particles on this tier; the
  per-vertex sun + sky term stays.

### Effect lights

- Fixed pool of 32 `{dvec3 pos; vec3 color; float intensity, radius, decay, age; float clip[6]}`
  in `vortex.fx`; not entities, no shadow ray, no temporal history.
- `graphics::PointLight` gains `bool effect`. The renderer uploads shadowed lights first, effect
  lights after; `counts.z` stays the shadowed count, the unused `settings.w` carries the effect
  count. `FrameUniforms` layout and `lighting.frag` do not change.
- `composite.frag` defines `VX_NEED_LIGHTS` and loops over at most 8 effect lights:
  `diffuseColor * color * VxLightFalloff * max(dot(n, l), 0)` from the position and normal it
  already has. Est. < 0.5 ms, 0 with no effect lights. Flash is crisp and instant.
- Leak control: range `4 * r` instead of 8, lifetime under 0.4 s, and 6 axis `World::Raycast`
  calls at spawn give a free distance per axis; the light is attenuated outside that box
  (packed into a second light record, no new binding). A blast in a room does not light the far
  side of the wall.
- Merge per frame on the CPU: score `intensity * radius^2 / max(dist, 1)^2`; a light within half
  the larger radius of a slot merges (colour x intensity summed, position intensity-weighted,
  radius and clip box = max); else free slot; else nearest slot. A dropped light draws one
  additive glow sprite.
- Shadowed lights are capped at the nearest 16 on this tier (256 today).

### Authoring

```
struct EffectLayer { fx::EmitterDesc emitter; float countBase, countPerRadius, radiusExponent;
                     uint32_t countCap; uint8_t maxLod; bool collide; };
struct EffectLight { glm::vec3 color; float intensityPerRadius, radiusScale, decay; };
struct EffectDesc  { std::string name; uint8_t priority; std::vector<EffectLayer> layers;
                     std::optional<EffectLight> light; };
```

`SpawnExplosion` / `SpawnVoxelBreak` become library entries built from the presets in
`Effects.cpp` (no visual change). Later: `assets/effects/*.json` via the `json.hpp` shipped with
tinygltf, editor reload, and a load-time check that rejects an effect whose LOD 0 fill at 3 m
exceeds 40% of the overdraw budget. Designers set counts and caps, never budgets.

## What changes in the engine

| File | Change |
|---|---|
| `src/engine/Engine.cppm` | `EngineConfig::fx` (`FxBudget` caps); `maxParticles` default 8192; later `PlayEffect(const fx::EffectDesc&, position, radius)` beside `Explode` (additions only) |
| `src/engine/internal/FxDirector.cpp` (new, engine-internal) | Queues, caps, region merge, LOD + load factor, scratch vectors kept as members |
| `src/engine/internal/Engine.cpp` | `Explode` becomes a producer; `Run` calls `Tick`; body focus points use a 2 m radius, camera/player keep 12 m |
| `src/fx/Particles.cppm`, `internal/Particles.cpp` | Alpha-only sort, overdraw budget + size clamp, `Stats` gains fill pixels |
| `src/fx/Effects.cppm`, `internal/Effects.cpp` | `EffectDesc`, `EffectLibrary`, `Spawn(desc, pos, radius, lod, load)`; effect light pool + merge |
| `src/graphics/Renderer.cppm`, `internal/Renderer.cpp` | `PointLight::effect`, upload order, `settings.w`, shadowed cap 16; edited slots uploaded first; `IsSlotUploaded(slot)` |
| `shaders/composite.frag`, `particles.vert/.frag`, `include/common.glsl` | Effect light loop; flat shape varying; `settings.w` documented. ARCHITECTURE 5.x updated |
| `src/physics/Terrain.cppm`, `internal/Terrain.cpp` | `installedGeneration`: accept a result newer than what is installed; `Update` overload with a radius per focus; face-aware neighbour dirtying; cook cap |
| `src/scene/internal/Scene.cpp` | Per-entity damage coalescing; old model drawn until all new slots are uploaded; `Lifetime` on pieces (shared with `collapse.md` stage 1) |
| `src/voxel/internal/Connectivity.cpp`, `World` | Minimum island size parameter; `TakeChangedRegions()` for face-aware dirtying |

## Stages

1. **Measure.** `ProfileScope` around carve, island search, damage, particle update/sort,
   collider scan; `--stress-fx N` (N blasts in one frame, then 3 per second for 10 s); log
   "particles", "lighting", "composite" pass times. *Verify:* headless run prints the table;
   numbers replace the estimates above and fix the order of stage 2 items.
2. **Cheap wins, no new system.** Effect lights in composite with merge and axis clip; debris
   sample 400 -> 96; island margin 12, early-out, minimum island size; alpha-only sort; flat
   varying; pool 8192; `Lifetime` on pieces and islands; collider `installedGeneration` fix.
   *Verify:* `--stress-fx 10`: "lighting" pass time unchanged by casts (within 0.3 ms),
   particles alive <= 8192 with the last blast still visible in a screenshot, island scope >= 3x
   cheaper; unit test: chunk edited every frame for 60 frames receives a collider; a 5-voxel
   island yields no entity; a sleeping piece is gone after its lifetime.
3. **`FxDirector`.** Queues and caps, region merge, per-entity damage coalescing, LOD + load
   factor, overdraw budget and clamp, swap on upload, edited uploads first. *Verify:*
   `--stress-fx 10` worst CPU frame for destruction + effects <= 4 ms, GPU effects <= 4 ms; all
   ten carves applied within 6 frames; no frame where a damaged object has zero instances.
4. **Collider churn.** Per-focus radius, face-aware neighbour dirtying, cook cap. *Verify:*
   cooks per blast and scan time in the stage 1 table drop; existing headless run still reports
   the player "on the ground" and the beam cut.
5. **Authoring.** `EffectLibrary` in C++, then JSON + editor reload + fill read-out, `PlayEffect`.
   *Verify:* explosion screenshot unchanged against stage 3; an over-budget file is rejected with
   a logged reason.
6. **Only if stage 1/3 numbers demand it.** Island search as a frontier flood on occupancy masks
   on a worker (snapshot by value, revision check on install); half-resolution target for large
   soft sprites with depth-aware upsample; stateless GPU emitters for continuous effects.

Every stage also passes the standard build, `ctest` and both headless validation runs.

## Open questions for the game

- **Frame target in fights:** locked 30 fps on the UHD 620 tier, or is 60 required on some
  higher tier that should get its own caps (more particles, shadowed hero flash)?
- **How many simultaneous casts must look full?** The caps assume about 10 blasts within 1.5 s;
  beyond that the newest thin out. Is a 20+ caster fight planned?
- **Is a 1-3 frame delay between hit and falling pieces acceptable**, and up to 0.25 s for
  objects beyond the nearest three? Or must some spells (beam cut) always be instant?
- **Unshadowed spell light:** acceptable that flashes do not cast shadows from characters and
  objects? If not, one shadowed "hero" light per frame costs est. 2-4 ms.
- **Debris chips bouncing on the ground:** only for the player's own nearby casts (LOD 0), or
  also for enemies' casts?
- **Largest spell radius:** island search is refused above 192 voxels per axis (about 8 m
  radius with the 12-voxel margin). Are larger blasts planned?
- **Continuous effects** (auras, torches, projectile trails): how many on screen at once? This
  decides whether stage 6 stateless emitters are needed.
- **Who authors effects:** programmers in C++ is enough for now, or is the JSON + editor panel
  needed early for a designer?
