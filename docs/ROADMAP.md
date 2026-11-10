# VORTEX roadmap

What exists today, and what was designed for but is not built yet. See
[ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit together.

## Working now

| Area | State |
|---|---|
| Build | Pinned dependencies, Vulkan headers fetched, shaders compiled to SPIR-V at build time, `cmake --preset dev`. |
| Core | Logging, job system, profiler, input state, RNG, command line. |
| Voxel world | Streaming window of sparse 32^3 chunks, generation on worker threads, editing (set / fill / carve with material hardness), region-file persistence of modified chunks. |
| Terrain | Built-in generator: hills, mountains, beaches, sea, caves, trees. Custom generators through `IWorldGenerator`. |
| Ray queries | One traversal shared by GPU and CPU (`shaders/shared/trace.glsl`), world + dynamic objects through a BVH. |
| Objects | Voxel models of any size shared between instances, collider boxes, mass from material density. |
| Scene | `vortex.scene`: EnTT entities with transform, voxel body, rigid body, point light and lifetime components; destroying an entity releases its body and chunk slots; usable and tested without a GPU. |
| Destruction | Terrain carving with floating-terrain detection; objects lose voxels and split into pieces (`Scene::Damage`, `Engine::Explode`). Fragments below 16 voxels are particles, small pieces are rubble with a lifetime, a distance rule and a global cap. |
| Import | MagicaVoxel `.vox` read / write, glTF voxelisation. |
| Physics | Jolt behind an engine interface, fixed time step, character controller. Terrain colliders streamed around the camera and requested by every moving body; bodies without ready terrain are frozen in place and resume when it is there, so nothing falls through unloaded or uncooked terrain. |
| Particles | CPU pool with emitters, voxel collision, effect presets. |
| Renderer | Ray-marched G-buffer, ray-traced sun shadows, ambient occlusion, emissive voxel light, point lights, temporal accumulation, fog, particles, TAA, tone mapping, ImGui. Headless mode with screenshots. Validation-clean on Intel UHD 620. |
| Editor | `vortex.editor`: in-game editor toggled with F1 - fly camera, entity selection with a translate / rotate gizmo, inspector, terrain brush, primitive spawning, `.vox` / glTF import, environment / render / profiler / world panels. Tool logic is separate from the widgets and unit tested. |
| Tests | 299 unit tests, none needing a GPU. |

## Performance (Intel UHD 620, 1280x720 output, 768x432 internal)

| Pass | GPU time |
|---|---|
| gbuffer (primary rays) | ~9-11 ms |
| lighting (sun shadow + ambient ray, tile checkerboard) | ~9-10 ms |
| composite, TAA, present | ~2.5 ms |

World window 48 x 8 x 48 chunks (154 m across at 0.1 m voxels), about 8100 dense chunks, 473 MiB
of GPU memory. Measured cost of the lighting features: sun shadows ~4.7 ms, ambient occlusion
~5.9 ms, emissive pickup ~1.5 ms; each can be switched off (`--no-shadows`, `--no-ao`, `--no-emissive`).

Known costs worth attacking, in order of expected gain:
1. About half of the dense chunks are underground cave chunks nobody sees; they cost memory and
   upload time, not frame time. Lazy loading of chunks without a line to the surface would halve memory.
2. Primary rays: a half-resolution depth pre-pass (start each ray at the conservative depth of its
   2x2 tile) would cut most empty-space steps.
3. Lighting at half resolution with a depth-aware upsample.

## Game direction (decided with the project owner, 2026-11-10)

| Topic | Decision | What it means for the engine |
|---|---|---|
| Structure | Session-based matches with automatic matchmaking and seamless joining; progression is global, MMO-like. The world is a set of region maps loaded separately (Albion Online style). | A dedicated, GPU-free session server per region instance. No endless world: a region is finite. |
| Players per session | Mixed by region type: small instanced maps and larger shared zones. | Player cap is a property of the region type; the design case for fights is 5v5 at about 10 casts per second. |
| Persistence | A region resets every session. | A region is `template + seed`; damage is a temporary edit log. Everything expensive (support graphs, coarse LOD, colliders of prefabs) can be baked offline per prefab. |
| World content | Procedural terrain assembled with hand-made prefabs by rules. Small arenas (about 250 m) and big zones (about 1 km) from the same generator. | Prefab format with baked data; rule-driven region generator; the server holds a whole region, clients stream a window. |
| Combat | PvP and PvE, PvP first. | Server authority, prediction and lag compensation from the first networked build. Monsters, AI and navigation come after PvP works. |
| Camera | First person. | Close-up voxel detail matters most; the horizon is visible. |
| Hardware | Playable on Intel UHD 620, tuned for a GTX 1650 at 60 fps. Windows and Linux clients, Linux server. | Every feature has a cheap path. View distance is a setting: 77 m on integrated GPUs, 150 m and more on discrete ones. |
| Destruction | Whole castles and cities can collapse. Large falling pieces are real (server-simulated, can crush); small debris is cosmetic and local to each client. Rubble fades after seconds. | The server decides which part of a structure is released and simulates a few large bodies; clients add particles and small pieces. |
| Network stability | Large-scale destruction is limited by game rules rather than by technology: bedrock (indestructible) materials, regeneration of locations, destruction costs. | Indestructible material flag in generation rules; a per-region destruction budget; a regrowth system that restores prefabs from their template. |
| Building | Only in fights: created matter is temporary. Medium-sized conjured objects to climb on or use as a shield. | Spell-created voxels and objects carry a lifetime; no free-form persistent building, no player housing. |
| Magic | Create matter, move matter, change materials, spreading fire, summoning enemies. | A data-driven spell system built from a few world operations (carve, fill, swap material, impulse, spawn). |
| Characters | Rigid voxel parts on a skeleton. | Reuses the object renderer; limbs can be separated. No skinned-mesh path. |
| Online services | Later; stubbed now. | The session server reads players and loadouts from a file and writes results to one. |

## Milestones

Order follows the owner's priority: the networking skeleton first, because every later system
has to live under server authority.

### M1 - Networking skeleton
Goal: one server and two clients walk in the same region and see each other's destruction.
- `vortex_server`: an executable built from `Vortex::Core`, `Vortex::Physics` and the scene
  only (they already build and are tested without a GPU). Fixed tick; the client renders
  between ticks as today.
- Region start-up from `template + seed`: both sides generate the same terrain, so the
  server never sends terrain, only changes.
- World changes as operations (`carve sphere`, `fill box`, `swap material`), not as voxels:
  a blast is a few bytes. A late joiner receives modified chunks, then the live operation stream.
- Player movement with client prediction and server reconciliation; other players and
  server-owned bodies interpolated.
- Replication classes for bodies: server-owned (large pieces, conjured objects, players)
  and client-only (small debris, particles).
- Stub services: a session file with the player list and loadouts.
- Tests: server and two headless clients in one test process over a loopback transport,
  with packet loss and delay; the world hash of every side must match after a scripted fight.
- To decide in a design document first: transport library, tick rate, how generation is kept
  bit-identical across Windows and Linux (integer or fixed-point noise).

### M2 - Fight feel under authority
- Spell framework: data-driven casts (projectile, beam, area) producing world operations,
  damage, impulses and effects; lag-compensated hits.
- Conjured matter: temporary walls and medium objects with a lifetime, climbable, usable as cover.
- FX budget stages 1-2 from [design/fx-budget.md](design/fx-budget.md): measurement
  (`--stress-fx`), then effect lights out of the ray-traced pass, queued and coalesced damage.
  Acceptance: 5v5 with 10 casts per second holds 60 fps on a GTX 1650 and does not fall below
  the idle frame rate by more than a fixed margin on the UHD 620.
- LOD stage 1 from [design/lod.md](design/lod.md): distance budgets for lighting, shadow
  casters, debris and particles.
- Characters from rigid voxel parts with skeletal animation; a training dummy and a second player.

### M3 - Collapse of buildings
- Stage 1 from [design/collapse.md](design/collapse.md): the anchored part of a damaged static
  object stays static, only the unsupported part falls.
- Support graph of 8^3-voxel cells, baked per prefab at build time (possible because regions
  reset). The server solves support on the graph and sends "cells released"; it simulates the
  few large bricks, clients turn the rest into local debris and particles. Rubble fades.
- Structures stamped into terrain so castles and towers larger than 12.8 m can fall.
- Stability rules: bedrock materials, per-region destruction budget, regrowth of prefabs
  from their template when no player is near.

### M4 - Region generator
- Prefab library with baked data (support graph, colliders, palette), rule-driven placement,
  region types (arena about 250 m, zone about 1 km) with player caps, spawn points and
  bedrock layers. Editor work is aimed at prefabs and rules, not at painting whole maps.

### M5 - Magic that changes the world
- Moving matter (telekinesis, thrown pieces), material changes (freeze, melt, petrify),
  spreading fire as a budgeted server-side cellular simulation with flammability per material.
- Summoning, which starts the PvE track: enemy AI and navigation on destructible ground.

### M6 - Reach and platforms
- Far terrain level 1 (150 m) as a setting for discrete GPUs; later levels only if wanted.
- Windows client build, CI with tests and a headless validation render.

### M7 - Online services
- Accounts, matchmaking, global progression, seamless transfer between regions; replaces the
  session file from M1.

### Still open
- Player caps per region type, and whether big zones need interest management in M1 or later.
- Regrowth rules (what regrows, how fast, visible or only when unobserved).
- How much the client is trusted (anti-cheat depth) for PvP.
- Largest conjured object, and the limit of conjured matter per player.

## Done so far, with gaps

1. ~~**Player**: character controller on the terrain~~ - done in the game (walk, run, jump,
   first / third person, free flight). Still missing: a proper character model and animation.
2. ~~**Object destruction**~~ - done: `Scene::Damage` carves an object, splits it into
   connected pieces that inherit its motion and turns small fragments into particles; `Explode`
   applies it to every object in range. Still missing: damage from impacts (contact events).
3. ~~**Scene / ECS**~~ - done: `vortex.scene` replaced `Engine`'s object list. Objects, lights and
   explosion flashes are entities; destroyed objects give their body and chunk slots back;
   physics bodies map back to entities. Still missing: an emitter component (particle emitters
   are still handled by hand), parent / child transforms, routing contact events to entities,
   a spatial index for `QueryBodies` (it tests every body), and saving / loading entities.
   Debris clean-up is done (ARCHITECTURE.md 7.3), as is freezing bodies that have no terrain (6.1).
4. ~~**Editor**~~ - done: `vortex.editor` (ARCHITECTURE.md section 8). Select tool with an
   ImGuizmo translate / rotate gizmo, inspector, delete and duplicate; sphere / box terrain brush
   (add / remove, any world material, stroke spacing); box / sphere primitives and `.vox` / glTF
   import by path; environment, render, profiler and world panels; an editor fly camera. Still
   missing: undo / redo; a voxel brush for objects (the brush edits terrain only); a scale gizmo
   and snapping; a file dialog (paths are typed); reusing or freeing palettes (every import adds
   one of 256 GPU materials and nothing removes it); saving / loading entities (only terrain
   edits persist, through World::Save); pausing the simulation while editing; picking entities
   without a body (the player figure and lights are only selectable from the scene list);
   changing an entity's motion type, name or material from the inspector; a material editor.
5. **Memory**: skip unseen underground chunks, then palette-compressed chunks; then view distance
   through level-of-detail cascades.
6. **Look**: reflections (metals are faked with diffuse today), bloom, shadowed particles.

## Planned, not built

### Engine structure
- **Scene, next parts**: emitter component, transform hierarchy, contact events per entity,
  entity serialisation (the base layer exists, see step 3 above).
- **Physics guard, next parts**: parked objects are kept as static bodies for as long as the game
  runs (they should be saved and unloaded with their chunks once entities can be serialised);
  a chunk that gains terrain keeps its old collider for the few frames of re-cooking, so a body
  can sink into freshly added voxels; character controllers are still held by the game
  (`Engine::TerrainReady(bounds)`), not by the guard, and nothing keeps colliders around a
  character the camera has left (it is held until the camera returns); while a brush or spell
  keeps uncovering faces of a neighbouring chunk, bodies touching that chunk wait for the stroke
  to pause; water lit only by local lights in an enclosed place still reflects sky colour
  (the glint is correct; ARCHITECTURE.md 5.7).
- **Editor, next parts**: undo / redo, object voxel brush, scale gizmo and snapping, file dialog,
  palette reuse, entity serialisation, simulation pause (the base exists, see step 4 above).
- **CPU reference renderer** (`tools/cpu_render`): software frame from the shared shader code, for
  image checks on machines without a GPU.

### World
- **Level of detail**: clip-map cascades with doubled voxel size for long view distances. The current
  view distance is the window (about 100 m at 0.1 m voxels).
- **Chunk memory**: chunks are dense (37 KiB each). Palette-compressed or brick-sparse payloads.
- **Water**: liquids have an animated, sky-reflecting surface shaded in the composite pass
  (ARCHITECTURE.md 5.7; about 0.15 ms on a UHD 620 with half of the screen under water). Still opaque voxels:
  no refraction, no flow, no reflections of terrain or objects, no shoreline foam, no underwater
  look (a camera inside water sees it from within as a solid). Water next to a crater stands as a wall.
- **Transparency** (glass, refraction) is not started.
- **Biomes, structures, ore veins** in the generator; a data-driven material and generator description.
- **Background saving** and save-format versioning.

### Rendering
- **Reflections** for smooth materials, **volumetric light shafts**, **bloom**, depth of field.
- **Shadowed, lit particles** (particles currently use simple sun + sky lighting).
- **Multi-bounce diffuse light** (only emissive voxels light their surroundings today).
- **Upscaling** better than bilinear, and half-resolution lighting for integrated GPUs.
- **Shader width on Intel**: `INTEL_DEBUG=fs` shows `lighting.frag` (about 3400 instructions, 9 spills)
  and `gbuffer_world.frag` compiled 8 pixels wide only - the 16-wide variants fail register
  allocation. These are the two expensive passes; getting them 16 wide (smaller traversal state,
  splitting the lighting pass) is a likely large win that has not been tried.
- **Textured / detailed materials** beyond colour, roughness, metallic and emission.

### Gameplay foundations
- **Animation**: rigs made of voxel parts, skeletal animation, inverse kinematics for characters.
- **Audio**: impact and break sounds per material (ids are reserved in the material).
- **Fire, fluids, structural stress** simulation.
- **Game UI** layer separate from the debug UI; input action mapping and gamepad support.
- **Networking**: server-authoritative simulation. `Vortex::Core` and `Vortex::Physics` already build
  without GPU or window, the simulation runs on a fixed step and the world is stored as chunk deltas.
- **Double-precision physics** for worlds far from the origin (rendering is already origin-relative).

### Tooling
- GCC / MSVC builds (only Clang is tested), CI running the tests and a headless validation render,
  image-comparison tests against stored screenshots.
