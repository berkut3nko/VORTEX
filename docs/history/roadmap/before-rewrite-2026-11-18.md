# Roadmap history: sections replaced or edited by the 2026-11-18 rewrite

Moved verbatim from `docs/ROADMAP.md` lines 1-92, 446-543 and 632-668 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

# VORTEX roadmap

What exists today, and what was designed for but is not built yet. See
[ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit together.

## Working now

| Area | State |
|---|---|
| Build | Pinned dependencies, Vulkan headers fetched, shaders compiled to SPIR-V at build time, `cmake --preset dev`. |
| Core | Logging, job system, profiler, input state, RNG, command line. |
| Game UI | Play mode locks the mouse (GLFW_CURSOR_DISABLED + raw motion; pointer lock on Wayland, grab on X11) and looks around without a button; Escape opens a pause menu (Resume / Quit). A HUD drawn without ImGui (`vortex.core:hud` data + one instanced draw after tone mapping, built-in 5 x 9 bitmap font): crosshair, mode, tool, frame rate, key hints, menu. ImGui is only the F3 debug overlay (hidden by default) and the F1 editor. Windowed behaviour (lock on Wayland / X11, focus, menu clicks) is not confirmed yet: headless runs cannot exercise it. |
| Voxel world | Streaming window of sparse 32^3 chunks, generation on worker threads, editing (set / fill / carve with material hardness), region-file persistence of modified chunks (offline game and map editor, when `saveDirectory` is set; a session server's world is memory only; a region file records its voxel size and a world of another size refuses it). The game's window is **full height** (view distance stage 1): a fixed -48..128 m (44 rows at 0.125 m) that follows the camera in x / z only, so climbing never unloads anything; a column pass installs sky and rock without jobs; buried chunks farther than 16 m stay **Packed** (RLE bytes, no slot); a build ceiling at 128 m. |
| Terrain | Built-in generator: hills, mountains, beaches, sea, caves, trees. Custom generators through `IWorldGenerator`. The voxel size is a world setting: **0.125 m** by default since view distance stage 3 (4 m chunks; the game's `--voxel-size` still previews 0.1 / 0.15 m offline). Content and debris thresholds keep their metre size. |
| Ray queries | One traversal shared by GPU and CPU (`shaders/shared/trace.glsl`), world + dynamic objects through a BVH. |
| Objects | Voxel models of any size shared between instances, collider boxes, mass from material density. |
| Scene | `vortex.scene`: EnTT entities with transform, voxel body, rigid body, point light and lifetime components; destroying an entity releases its body and chunk slots; part of the GPU-free `Vortex::Sim` and tested without a GPU. |
| Destruction | Terrain carving with floating-terrain detection; objects lose voxels and split into pieces (`Scene::Damage`). A blast is `sim::Blast` (integer-exact world operations: carve, one `Detach` per floating island of 8+ voxels (0.016 m^3), one `ClearMask` for all smaller ones - islands found in the island box and, for parts that reach past it, proven by a bounded face flood inside a +-96-voxel flood box, so a tower falls on the shot that cuts it; then a max-flow load check on the cell graph releases parts whose weight their bonds cannot carry, so a tower on a thin neck falls too (shred.md stage 4b); object damage, impulses) plus `sim::PresentBlast` via `Engine::PresentBlast` (particles, local rubble, flash); `Engine::Explode` does both. Fragments below 8 voxels are particles; every piece of rubble has a lifetime by size (15 s under 25 voxels, 20 s under 1 m^3, 30 s larger), a distance rule for small rubble and global caps (192 small, 48 bodies, 600 chunk-store slots); a piece that does not fit, or cannot be baked, becomes particles. |
| Import | MagicaVoxel `.vox` read / write, glTF voxelisation. |
| Physics | Jolt behind an engine interface, fixed time step, character controller (moved by `sim::StepPlayer`, the movement rule a session shares; a state can be restored for a replay). Terrain colliders streamed around the camera and requested by every moving body; bodies without ready terrain are frozen in place and resume when it is there, so nothing falls through unloaded or uncooked terrain; a body overlapping edits its chunk's collider does not show yet waits too, frozen with its velocity, and a body parked over ground edited out of sight is woken when that ground gets its collider. |
| Particles | CPU pool with emitters, voxel collision, effect presets. |
| Renderer | Ray-marched G-buffer, ray-traced sun shadows, ambient occlusion, emissive voxel light, point lights, temporal accumulation, fog, particles, TAA, tone mapping, ImGui. Headless mode with screenshots. Validation-clean on Intel UHD 620. |
| Editor | `vortex.editor`: in-game editor toggled with F1 - fly camera, entity selection with a translate / rotate gizmo, inspector, terrain brush, primitive spawning, `.vox` / glTF import, environment / render / profiler / world panels. Tool logic is separate from the widgets and unit tested. |
| Simulation | `vortex.sim`: `sim::Simulation` owns world, physics, terrain colliders and scene and steps them on a numbered fixed tick; `sim::WorldOp` / `ApplyOp` replay terrain changes exactly; `sim::Blast`. Library `Vortex::Sim` links no renderer or window. `Engine` owns one and draws it. |
| Sessions | `vortex.net` (`Vortex::Net`, GPU-free): the M1 protocol over ENet (UDP, `EnetTransport`) and in-process loopback transports (`ImpairedTransport`: seeded loss, delay, jitter, duplicates, reordering, rate limit, outages), `net::Server` (fixed region window, template as operations 1..k, event stream with a byte budget, cast queue with a work budget, join by chunk sync since stage 7a (modified chunks as paced bulk, the entity baseline), one player per client moved by its inputs, snapshots at 30 Hz) and `net::Client` (handshake with token and build hash, region from `Welcome`, operations applied in `opSeq` order above each chunk's base, a window that follows its player on regions wider than 48 chunks (stage 7a), the own player predicted with `sim::StepPlayer` and reconciled with the snapshots on a clock kept ahead of the server, other players interpolated); `vortex_server` runs a session file headless, listens on UDP (`--listen`, ENet) and runs in-process bot players over loopback or UDP. The game joins a session with `Shredborne --connect HOST[:PORT] --token HEX` (stage 5): its engine is built from the server's region and ticks on the client clock, its player is predicted, other players are voxel figures, every hole is the server's (one round trip after the click). Since stage 6 crates (the plaza's and E), terrain pieces of 0.512 m^3 or more and object pieces are server bodies with a NetId (at most 2 new per tick, 64 awake, 256 live), sent as entity events and in the snapshots; clients hold kinematic copies drawn in the past and put exactly at their rest poses, players collide with them at rest on both sides, smaller pieces are each client's own rubble. Tested on one machine (server + two headless game clients on localhost: equal world hashes and entity lists), and by the owner on two machines over a LAN on 2026-11-17 (stage 7b's commits): "seamless", 24 ms ping, 43 fps windowed on the UHD 620. |
| Tests | 722 unit tests (six disabled measurements), none needing a GPU (the ENet and relay ones use UDP sockets on 127.0.0.1). Stage 8's soak (`tools/soak.sh`: `vortex_server` and `vortex_bot` processes over real UDP with loss, jitter, an outage and a slow downlink) runs for 1 or 10 minutes, with a headless game client joining at the end (`GAME_CLIENT=1`). |

## Performance (Intel UHD 620, 1280x720 output, 768x432 internal)

| Pass | GPU time |
|---|---|
| gbuffer (primary rays) | ~9-11 ms |
| lighting (sun shadow + ambient ray, tile checkerboard) | ~9-10 ms |
| composite, TAA, present | ~2.5 ms |
| HUD (play: ~150 quads; pause menu: ~70 quads, backdrop dimmed in the present pass) | 0.01 ms (median of 120 frames, measured headless at 1280x720) |

World window 48 x 44 x 48 chunks (192 m across, -48..128 m, at 0.125 m voxels; view radius 92 m with an edge fog
over its last 15 %), 5300-5600 dense chunks and about 14 800 Packed ones (23-24 MiB of RLE) once filled, 473 MiB
of GPU memory; gbuffer / lighting 6.2 / 8.9-9.2 ms at the furniture probe, 12.2 / 7.3-7.6 ms on the peak looking
over the valley (view-distance.md, "Stage 3 as built"). (At 0.1 m: 48 x 55 x 48, 154 m across, 5500-6600 dense;
before view distance stage 1: 48 x 8 x 48 around the camera, about 8100 dense chunks.) Measured cost of the lighting features: sun shadows ~4.7 ms, ambient occlusion
~5.9 ms, emissive pickup ~1.5 ms; each can be switched off (`--no-shadows`, `--no-ao`, `--no-emissive`).

Known costs worth attacking, in order of expected gain:
1. (Done in view distance stage 1: buried chunks are Packed. Stage 5 of that design replaces the geometric rule
   by an exact visibility flood and stops plugging cave mouths.)
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
- `vortex_server`: an executable built from `Vortex::Core`, `Vortex::Physics`, `Vortex::Sim`
  (scene + simulation) and `Vortex::Net` (the protocol) only; they build and are tested without a GPU. Fixed tick; the client renders
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


<!-- source lines 446-543 -->

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
- Design: [design/worldgen.md](design/worldgen.md) (2026-11-17): the server plans an integer `RegionPlan` and every
  side stamps it into chunk generation (pristine towns, no join bytes). It covers building kits drawn from real-world
  traditions, standing proven by construction rules, and stages W1-W6. The setting and story are in the game's
  `docs/world/setting.md`.
- Prefab library with baked data (support graph, colliders, palette), rule-driven placement,
  region types (arena about 250 m, zone about 1 km) with player caps, spawn points and
  bedrock layers. Editor work is aimed at prefabs and rules, not at painting whole maps.

### M5 - Magic that changes the world
- Moving matter (telekinesis, thrown pieces), material changes (freeze, melt, petrify),
  spreading fire as a budgeted server-side cellular simulation with flammability per material.
- Summoning, which starts the PvE track: enemy AI and navigation on destructible ground.

### M6 - Reach and platforms
- Far terrain level 1 (150 m) as a setting for discrete GPUs; later levels only if wanted.
- Windows client build, CI with tests and a headless validation render. (2026-11-17: the owner built the game on a
  Windows laptop and played a LAN session against the Linux server, so the probe chunks agreed. Still missing: a
  documented, repeatable build, the golden tests run on Windows, and CI.)

### M7 - Online services
- Accounts, matchmaking, global progression, seamless transfer between regions; replaces the
  session file from M1.

### Still open
- Player caps per region type. (M1 is accepted on a 250 m arena; owner answers are in design/networking.md.)
- Regrowth rules (what regrows, how fast, visible or only when unobserved).
- How much the client is trusted (anti-cheat depth) for PvP.
- Largest conjured object, and the limit of conjured matter per player.

## Done so far, with gaps

1. ~~**Player**: character controller on the terrain~~ - done in the game (walk, run, jump,
   first / third person, free flight). Still missing: a proper character model and animation.
2. ~~**Object destruction**~~ - done: `Scene::Damage` carves an object, splits it into
   connected pieces that inherit its motion and turns small fragments into particles; `sim::Blast`
   (behind `Engine::Explode`) applies it to every object in range. Still missing: damage from impacts (contact events).
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
   one of 256 GPU materials and nothing removes it); saving / loading entities (terrain is saved by "Save world" when
   `EngineConfig::world.saveDirectory` is set; the game sets none, so the button is disabled there); pausing the simulation while editing; picking entities
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
  (`sim::StepPlayer`'s `HasCollider` gate since networking stage 4), not by the guard, and nothing keeps colliders around a
  character the camera has left (it is held until the camera returns); while a brush or spell
  keeps uncovering faces of a neighbouring chunk, bodies touching that chunk wait for the stroke
  to pause; water lit only by local lights in an enclosed place still reflects sky colour
  (the glint is correct; ARCHITECTURE.md 5.7).
- **Editor, next parts**: undo / redo, object voxel brush, scale gizmo and snapping, file dialog,
  palette reuse, entity serialisation, simulation pause (the base exists, see step 4 above).
- **CPU reference renderer** (`tools/cpu_render`): software frame from the shared shader code, for
  image checks on machines without a GPU.


<!-- source lines 632-668 -->

- **Level of detail**: clip-map cascades with doubled voxel size for long view distances. The current
  view radius is 92 m (the 48-chunk window at 0.125 m voxels).
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
- **Game UI**: the HUD layer exists (ARCHITECTURE.md section 11). Still missing: input action mapping and
  key rebinding, a settings menu (mouse sensitivity is a constant), gamepad navigation, Unicode / TTF text,
  textured widgets, in-game chat (M1).
- **Networking**: server-authoritative simulation (M1, stages 2-9; 9 left (behind M2); 8 built, verified on one machine, not committed; M1's acceptance is the owner's LAN run). `Vortex::Core`, `Vortex::Physics` and
  `Vortex::Sim` build without GPU or window, the simulation runs on a numbered fixed tick and the world is stored
  as chunk deltas.
- **Double-precision physics** for worlds far from the origin (rendering is already origin-relative).

### Tooling
- GCC / MSVC builds (only Clang is tested), CI running the tests and a headless validation render,
  image-comparison tests against stored screenshots.

---

## Errata

- Relative links above were written for `engine/docs/ROADMAP.md`: from this file, prefix them with `../../`
  (for example `../../ARCHITECTURE.md`).
