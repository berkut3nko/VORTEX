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
| Sessions | `vortex.net` (`Vortex::Net`, GPU-free): the M1 protocol over in-process loopback transports (`ImpairedTransport`: seeded loss, delay, jitter, duplicates, reordering, rate limit, outages), `net::Server` (fixed region window, template as operations 1..k, event stream with a byte budget, cast queue with a work budget, join = full event log, one player per client moved by its inputs, snapshots at 30 Hz) and `net::Client` (handshake with token and build hash, region from `Welcome`, operations applied in `opSeq` order, the own player predicted with `sim::StepPlayer` and reconciled with the snapshots on a clock kept ahead of the server, other players interpolated); `vortex_server` runs a session file headless with in-process bot players. No real network transport yet (ENet, stage 5); the game does not connect yet. |
| Tests | 593 unit tests (six disabled measurements), none needing a GPU. |

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

State (stages are those of [design/networking.md](design/networking.md)):
- **Stage 0, determinism floor - done on Linux, Windows run still open.** Engine code is compiled
  with `-ffp-contract=off` (`vortex_options`; spelled `/clang:-ffp-contract=off` for clang-cl, which
  ignores the bare flag; Jolt keeps its own flags). `voxel::ChunkHash` / `voxel::WorldHash`
  (`src/voxel/Hash.cppm`) hash canonical chunk content. Three golden tests pin the built-in
  generator for 3 seeds: `Terrain.GoldenChunkHashes` (64 chunks per seed, incl. snow columns and
  chunks 1 km out), `Terrain.GoldenNoiseBits` and `Terrain.GoldenHeightBits` (raw float bits). Checked
  by building once without the flag: both float-bit tests fail and 1 of 192 chunk hashes differs, so
  the float-bit tests are the ones that detect a differently rounding build.
  `voxel::TerrainHeight` and `voxel::IsValid(TerrainSettings)` are new; nothing calls `IsValid` yet.
  Measured on the owner's laptop (5184 chunks, best of 7, three runs each): 1.57 ms per chunk before the
  flag, 1.59-1.65 ms after, and 1.60-1.64 ms with the flag removed again in the same session, so the
  cost is inside the run-to-run noise (about 5 %).
  **Not done:** `VortexCore` and `VortexCoreTests` have not been built or run on Windows (the
  clang-cl flag was checked with the compiler driver only); until the golden tests are green there,
  terrain agreement across platforms is unproven. Known risks: `design/networking.md`.
- **Stage 1, simulation without a renderer - done.** New library `Vortex::Sim` = `vortex.scene` + `vortex.sim`
  (Core + Physics + EnTT; a configure-time check, `vortex_assert_gpu_free`, fails the build if Vulkan, GLFW or ImGui
  get into it). `sim::Simulation` holds what used to be the simulation members of `Engine::Impl` - job system,
  materials, chunk store, world, physics, terrain colliders, particle pool, scene - plus a `uint32_t` tick.
  `Step(focusPoints)` is one server tick (streaming without moving the window, body guard, physics, pose sync,
  lifetimes, fall-out); the engine calls its three phases (`UpdateStreaming`, `StepPhysics`, `UpdateLifetimes`) where
  it called the parts before, so the frame loop and the game are unchanged. The scene no longer imports
  `vortex.graphics`: `GatherInstances` / `GatherLights` are free functions of `vortex.engine`.
  `sim::ServerConfig` gives the server scene: no particles, `SceneConfig::bakeModels = false` (models hold no chunk
  slots), `markDebris = false` and `debrisRemnants = false` (nothing is rubble). `ServerConfig` also makes the
  world memory only (clears `saveDirectory`); any other simulation, the engine's included, keeps it, so the
  editor's "Save world" works whenever the app sets a directory.
  Verified: the 310 earlier tests pass; 22 new tests in `VortexSimTests`, which links `VortexSim`, Jolt, glm, spdlog
  and GoogleTest only, step a `Simulation` 600 ticks with the offline and with the server configuration (crates
  land and fall asleep on the terrain collider, a timed light expires on its tick) and 240 ticks on the built-in
  terrain; both headless game runs exit 0 with the expected log lines. "Game unchanged" was checked against a build of
  the previous commit: same blast (7 -> 7 objects), same beam cut (37 voxels, 1 new object), player on the ground, the
  same picture; chunk and body counts vary between runs of either build by a few percent (streaming timing).
  **Not done / open:** the tick-rate scale of `Engine` (stage 4, with time sync); nothing yet stops a network client
  from running with a `saveDirectory` (stage 3: `net::Client` must refuse or clear it); on a server scene pieces of
  16 voxels (0.1 m) and more stay for good until `Scene::Damage` gets its threshold (stage 2 / 6); lifetimes run per tick
  in `Step` but per rendered frame in the engine, so nothing replicated may depend on them. The choices made where
  the design was open are in `design/networking.md`, "Stage 1 as built".
- **Stage 2, world operations and the blast split - done.** Replicated terrain edits take integer parameters only:
  `voxel::SphereQ` (centre and radius in 1/16 voxel, int64 sphere test), power in 1/16 hardness units, and
  `World::CarveSphereQ / FillSphereQ / SwapSphereQ / ClearMask` (the float `CarveSphere` / `FillSphere` quantise and
  forward). Every edit can report the chunks it could not apply to (`voxel::EditReport`: `skipped` = not loaded,
  `unsynced` = no store slot; all-or-nothing per chunk). `sim::WorldOp` (carve, fill, swap, box, detach) and
  `sim::ApplyOp` are what server and clients run. `sim::Blast` = carve, island search, one `Detach` per island (an
  occupancy mask, netId 0, the blast's push as velocity; no entity), `Scene::Damage` per object, impulses; it spawns
  no particles. `Engine::PresentBlast` makes the particles, the island rubble (from the voxels the Detach removed)
  and the flash; `Engine::Explode` = both, and the game runs on it. `SceneConfig::replicatedPieceVoxels` (0 offline,
  512 from `ServerConfig`): smaller object pieces - and a dynamic survivor below it - come back as fragments
  without an entity (client rubble). Verified: 22 new tests (385 in all); two simulations with different worker
  counts fed the same operations, two islands included, end with equal `WorldHash`, and a stream with one carve
  1/16 voxel larger or one Detach missing does not; headless game, editor and pause-menu runs exit 0 with the same
  crate blast (5 objects lose 558 voxels, no terrain) and the same beam cut (37 voxels, 1 new object) as before,
  and the crate area looks the same before and after. One offline change: a fresh island is no longer damaged by
  the blast that freed it (it is still pushed the same way). **Not done / open:** nothing acts on the skipped /
  unsynced report yet (stage 7); object particles are still spawned inside `Damage`; no `opSeq` or encoding
  (stage 3). Choices where the design was open: `design/networking.md`, "Stage 2 as built".
  **Crumbs (follow-up):** islands under the piece volume (0.016 m^3, `scene::MinPieceVoxels`; 16 voxels of 0.1 m then) are no longer a `Detach` each but one
  `ClearMask` per blast over their tight box, shown as particles only (`sim::PresentBlast`, now GPU-free and tested;
  no model, entity or body). Measured on generated terrain: 2051 operations for 44 blasts (worst 299) became 75 (at
  most 2), est. 65 KB -> 7.2 KB of wire. Found, open: the island search takes 70-200 ms per 1.2 m blast in the `dev`
  build, and that build has no `-O` flag at all (empty `CMAKE_CXX_FLAGS_RELWITHDEBINFO` with CMake 4.4).
- **Stage 3, the protocol on a loopback - done.** Library `Vortex::Net` (`vortex.net`, links `Vortex::Sim` only;
  `vortex_assert_gpu_free` covers it, `VortexNetTests` and `vortex_server`): `ITransport` with `LoopbackTransport`
  and the `ImpairedTransport` decorator (ENet semantics: reliable channels resend and keep order, the state channel
  loses, duplicates and reorders); byte reader / writer, events (world operations at 16-29 bytes plus RLE masks,
  `EntitySpawn` for lights), `Hello` / `Welcome` / `Refuse` / `EventBatch` / `ServerStatus`, every decoder refusing
  bad input; session files (`tools/server/sessions/*.session`); `BuildHash` = protocol version + the generator's
  golden hash + the material table (strengths included, shred.md R6); 64 probe chunks from the seed in `Welcome`.
  `net::Server`: window = play bounds + 3 chunks at the region's height range, loaded whole at start; the template's
  operations as 1..k (the game's plaza is now `sim::PlaygroundTemplate`, applied the same way offline), lights as
  `EntitySpawn`, crates as server bodies; per tick receive -> streaming -> casts within the cast work budget
  (`sim::CastWorkOf`, 60 000 units of about 0.1 us, calibrated) and the event byte budget (24 kB/s, 16 kB burst) ->
  physics -> lifetimes -> batches; joiners get the whole log. `net::Client`: refuses a world with a save directory or
  another voxel size / window / material table, loads the region window, applies events in order, groups them for
  `sim::PresentBlast`. `sim::kNeckFlowWorkBudget` lowered to 65 536 (shred.md question 7). Verified: 26 new tests
  (548); the acceptance - an in-process server and two clients, 5 % loss with delay, jitter and an outage, 200
  server-side blasts that free terrain (67 in-box islands with crumbs, 67 face-flood islands, 66 weak-neck releases),
  one client joining after 100 blasts from the full log - ends with equal world hashes; a server replayed with other
  worker counts and an emptied graph cache runs every cast on the same tick. `vortex_server --drill 200 --clients 2
  --loss 0.05` exits 0 with equal hashes; on a 250 m terrain region the server preloads in 9.6-10.4 s (gate 10 s)
  with 48.5 MiB of RLE and 463-488 MiB RSS. The game's headless runs are unchanged. **Not done / open:** ENet and the
  game's `--connect` (stage 5), inputs and snapshots (4), objects and pieces replicated (6), chunk sync and server
  terrain mode (7; a probe mismatch is only logged), `vortex_bot` (8); a stage-3 client holds the whole region; the
  join log grows for the session; R6's integer `MaterialPhysicsQ`. Choices: `design/networking.md`, "Stage 3 as built".
  Review of stage 3 (17 findings, all fixed, 13 new tests, 561 in all): a client refuses a store too small for the
  region or a world its generator did not make and fails (and leaves) on an operation it cannot apply; casts must
  fit the window (`sim::CastFitsRegion`, radius <= 9 m at 0.125 m); a failed server ends the session; only live
  events are presented; `Welcome` probes are range-checked; sends are paced per peer by `ReliableBytesInFlight`
  (32 kB); batches decode at most 16 MiB of masks; silent and refused peers time out; the default store is capped at
  2 GiB of chunks; the playground must find its site inside the play bounds; cast wall time is logged.
- **Stage 4, players - done.** `sim::StepPlayer` (`vortex.sim:player`): integer inputs (move axes, yaw, pitch,
  button levels with derived edges), a cast cooldown, the gate `TerrainColliders::HasCollider` (chunks with a
  collider built once: no wait for re-cooks, whose timing differs per side); `PhysicsWorld::SetCharacterState`;
  `PhysicsConfig::characterLayers` (a client's character collides with terrain only until stage 6). Messages
  `InputBatch` (the unacknowledged inputs, at most 8, 30 Hz) and `Snapshot` (tick, event position, input ack and
  margin, owner state, other players; replaces `ServerStatus`), the tuning in `Welcome`, protocol 2. Server: inputs
  queued up to 16 ticks ahead, a missing one repeats the last movement for 6 ticks, a late one refreshes it and its
  cast counts once; a cast edge is captured as a ray into the server's world and queued. Client: `TickClock` (jump
  after three late reports, 2 % bleed and - not in the design - 2 % catch-up, a hold as hard resync), prediction
  with 128 ticks of history, reconciliation after the snapshot's events (2 cm ignore, replay, 100 ms blend, 1.5 m
  snap, 250 ms timeout counted), `RemotePlayers`. `net::BotPilot`; `vortex_server`'s clients walk, jump and (with
  `--bot-casts N`) cast, and the report gives each one's correction rate. The offline game moves its player with
  `StepPlayer`. Verified: 23 new tests (584 then); over the seeded impaired loopback no correction above 2 cm on flat
  ground in 30 s at 5 % loss (0.05 per player-minute over 10 seeds), none beside 80 blasts at a chunk border, the
  +50 ms ping step never late and no resync, three lost packets with a cast give one cast, a 150 ms hitch no resync,
  a wall and a teleport converge, forged packets harmless; the drill exits with equal hashes, 6 and 1 corrections in
  20 s. **Not done / open:** the engine driven by the client clock and `--connect` (stage 5), capture at the input
  tick and loadouts (5), replicated bodies in the character mask (6), the visual-only timeout branch (7), errors up to
  10 mm after blasts near a player (not isolated). Choices: `design/networking.md`, "Stage 4 as built".
  **Review of stage 4** (15 findings, none rejected, 9 new tests, 593): the clock now hears margins of dropped
  too-early inputs (`Snapshot::marginTick`, protocol 3; a client placed far too early never recovered), the hard
  resync can no longer wrap to 4e9 ticks, three margins 2+ short of the target jump the clock (a +50 ms ping step:
  back in 0.52 s instead of 2.95 s; the 0.5 s acceptance is met from the first short report, from the step itself
  only with 0.6 s - an owner question), colliders under a player are flushed before every step on both sides
  (`TerrainColliders::FlushBox`; slopes went from 1 mm - 2 cm errors to bit-equal), tick length and gravity travel in
  `Welcome`, both sides use one character mask, remote players never step back (`ServerTime`), the 250 ms event wait
  times out while snapshots keep coming, a late jump is jumped, and the border, ping-step and flat-ground tests assert
  what they claim. See "Review of stage 4".
- Stages 5-9: not started. Before stage 3: shred.md stage 1 changed the search result (face flood), the server
  window margin (3 chunks, flood box) and the cast budget (a work budget); shred.md stage 4 (built, the owner's answer
  to its question 4) moved the island search onto the cell graph - same results except where a work budget runs out,
  node visits and chunk graphs used (at a cold price; never graphs built, which depend on the cache) to count in stage
  3's cast budget; shred.md stage 4b (built) adds the load check: more `Detach` operations per blast (a tower on a
  thin neck), no new op kind, its counted work joins the cast budget and the strength table must reach `buildHash`;
  see networking.md "Changed by shred.md".

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

### Drafts awaiting the owner (2026-11-12)
- **Destruction** (`design/shred.md`; the owner took the recommended answers to questions 1, 2, 3 and 7): the "second
  shot" bug was the island search box - a piece that reaches past +-(r/16 + 24) voxels of a blast counted as attached
  and was never searched again. **Stages 0, 1, 1b, 1c, 1d and 1e are built** (shred.md "Stage 0-1c as built",
  "Stages 1d-1e as built"): a `Blast:` log
  line per blast (ms per phase, flood counters, chunks to upload), the island box gathered by chunk (2-7x faster),
  the face flood (`voxel::ProveFaceComponents`) inside the fixed flood box with a 240 000-visit budget, offline
  deferral / server error counter, rows above the build ceiling as air, the carve replayed before a late search,
  counted and logged deferred-search drops / give-ups (given up only once the flood box left the window) and oversize
  islands; `--tower-blast` (the watch tower falls on the shot that cuts its ring: shot 7 of 7) and `--castle-blast`.
  Server window margin 3 chunks (`sim::kServerWindowMarginChunks`, networking.md updated). **Measured: search + flood
  per cast is 3.3-5.6 ms at the tower and castle cases (-O2, quiet) and the island search alone 9-12 ms underground,
  over the 3 ms rule: stage 4 (cell graph) should move before networking stage 3 - owner question 4, open.** Stage
  1d: a body is not simulated where its chunk's collider still holds edited voxels (the freed piece waits frozen,
  velocity kept: tower 2.92 m/s at +1 frame, 2.71 at +5; probe piece keeps 3.18 of 3.19 m/s), and a body parked
  over ground edited out of sight is woken on that chunk's first collider build. Stage 1e: every rubble piece has a
  lifetime by size (15 / 20 / 30 s), caps of 48 rubble bodies and 600 store slots besides the 192 small pieces
  (oldest resting removed first, else the new piece becomes particles), a failed bake crumbles instead of
  vanishing (`Scene::Rubble()` counts it all; the tower run ends at store 5273/12288). The lost push itself did not
  reproduce in isolation (shred.md). Review of 0-1e (shred.md "Review of stages 0-1e", 14 new tests): rows below a
  full-height window's bottom are ground for the island search (a blast near -48 m deferred for good), a server never
  defers (island box not loaded = counted error), the carve replay touches only the chunks it skipped, as they load,
  the far-rest rule is for small rubble only, removing a body wakes what rests on it, pieces of old rubble start a new
  lifetime, the split cap removes resting rubble first, the unseen-edit list keeps only chunks under frozen bodies,
  the per-blast log no longer sorts one entry per voxel. **Stage 4 is built** (owner: yes to question 4; shred.md
  "Stage 4 as built"): `vortex.voxel:structure` - a node list per terrain chunk (every component of an 8^3 cell is a
  node, so the graph equals voxel connectivity, over-full cells included: 22 in 1301 Dense / Packed chunks of game
  terrain), with voxel counts, integer masses and per-link bonds stored for a later load check ("a tower on a
  4-voxel bridge must not hold", stage 7), cached in the `Simulation` and rebuilt per chunk on `World::ChunkRevision`;
  `sim::Blast` searches islands on it (seeds in the island box, box components, floods over nodes inside the same
  flood box, only released nodes re-labelled per voxel) and emits exactly the stage-1 operations (every stage-1 test
  checks both searches; `Structure.GraphFloatingEqualsFaceFlood`, `GraphNeverDetachesWhatTheVoxelSearchKeeps`,
  `GraphEqualsLabelComponents*`, `RefreshEqualsRebuild`, `EvictionNeverChangesAGraph`,
  `StaleGraphNeverDetachesAttachedVoxels`; 12 new tests). Measured island search per cast: 0.06-1.0 ms quiet on flat
  ground and game terrain, cold graph builds included (voxel search 3.3-19 ms), in game 0.24-0.75 ms at a site already
  searched and 0.5-3.7 ms at the first blast of an untouched site, where cold graph builds (18-45 chunks, 16-120 us
  each in game, 15-27 us quiet) are most of it; 4.0-4.75 ms while streaming (`--climb`): over the 0.5 ms target and,
  while streaming, over 3 ms for the first blast at a site (pre-building graphs on a worker would fix it; an owner
  question in shred.md). Reviewed by three readers (shred.md "Review of stage 4"): a stale graph now never detaches
  (it corrupted the heap before), Uniform graphs are shared, cache memory is measured (5-7 KB per surface chunk
  graph, about 5 MB typical, 27 MB at most at the cap). `--after-fill`
  runs the blast scripts after the window has filled. **Stage 4b (weak necks) is built** (owner: "Yes, weak necks
  next"; shred.md Part 4b, "Stage 4b as built" and "Review of stage 4b"): integer bond strengths per material (compression /
  shear / tension, `Material::strength`), per graph node its weakest materials, an anchor bit and its narrowest
  horizontal plane (still 72 bytes; builds 29-31 us per Dense chunk, +3-6 us), and after every graph island search a
  max-flow load check in the island box (loads and supports from exterior floods in the flood box): the largest
  strictly overloaded set falls as ordinary `Detach` operations, nothing new on the wire. The watch tower on a
  2 x 2 stone neck falls (5.61x over), on 2 x 11 too (1.04x), on 2 x 12 it stands; also at 0.1 and 0.15 m; the full
  base, rock overhangs, the mushroom rock, beams, trees, walls on snow or against leaves, and 648 duds (144 in deep
  caves) + 396 blasts on game terrain at 0.1 / 0.125 / 0.15 m (beach, snow field, rock slope included) release
  nothing; sand overhangs over 1.9 m slump. `--neck-blast [W]` shows it; `--tower-blast` now frees the tower at shot 6 by the
  load check (2 faces left), not at shot 7. 45 new tests (brute-force max-flow, a per-voxel reference network that
  checks cut and faces at thresholds, chunk-face cuts, determinism over workers and cache state, budgets). Reviewed by
  three readers: one check releases at most 64 parts / 60 000 voxels; an unloaded chunk is support for that cast and a
  late search re-checks once it loaded (islands are never held back by the check, chunks beyond the window included);
  leaves no longer rate a wall, snow's compression is 50 000; the stale-graph guard covers mass and strengths; a chunk
  solid through is ground for exterior floods. Cost (quiet, same world as the search): 0.13-0.73 ms per cast at a
  cached site (2-5x the search), plus up to 1.8 ms of graph builds at a cold one; in game 0.3-2.9 ms + builds up to
  2.8 ms. At its budgets the preflow alone takes 5-6 ms (est. 8-12 ms with every budget full): stage 3 must charge
  it or the flow budget come down (shred.md open question 7). Open: later stages 2, 3, 4c (castle bricks), 5-7;
  prebuilding graphs (the only fix found for the cold cost); the windowed fall of towers and walls and their push are
  not confirmed by the owner; graph builds and the load check are not yet charged to a work budget (stage 3).
- **Frame rate** (`design/perf.md`, draft): measure the present path and CPU frame time first, then fog-radius ray
  cut and one modulo per ray, then lod.md stage 1 and half-resolution lighting. The owner calls the 0.125 m view
  radius (92 m) the sweet spot on the UHD 620: frame rate comes before more distance there.

### World
- **View distance** (`design/view-distance.md`): stage 1 (full-height window, column pass, Packed chunks,
  column / region-top skip, dirty grid pages, edge fog, `--climb` / `--fly`) is **done**; measured numbers and what
  differs from the design are in its "Stage 1 as built", the fixes of its review (edge chunks judged again, crater
  walls never packed, buried chunks packed again behind the focus, a full store never drops a saved chunk, deferred
  island searches, near-sphere slack, pending pixels out of the lighting filter, renderer fallback words) in "Review
  fixes". Open in it: recentre p90 0.35 ms (gate 0.2 ms, met at the median), the carve of a blast into cave rows not
  loaded yet is replayed on those chunks as they load, before its late island search (shred.md 1b), session `heightRange` not wired, windowed climbing and the
  streaming look (no dark halos, no see-through flashes) not confirmed by the owner. Stage 2 (voxel preview) is
  **done**: `--voxel-size` (0.1-0.15 m, offline only) in the game, a furniture probe beside the plaza built on request
  (`--probe`, `--camera probe` / `probe-far`, `--probe-blast`), generation chunks/s logged; per-size numbers and
  screenshots in its "Stage 2 as built" (0.1 / 0.125 / 0.15 m: view radius 73.6 / 92 / 110 m, far-view gbuffer
  10.6 / 12.2 / 11.5 ms; at 0.15 m thin boards merge and shelf pitches go uneven). Found there: a terrain piece that
  breaks off loses its blast push when its first physics step is not frozen (stale terrain collider, open).
  The owner chose 0.125 m. Stage 3 (switch to 0.125 m) is **done**: engine defaults 0.125 m (`voxel::kDefaultVoxelSize`;
  the game no longer sets a size, `--voxel-size` stays an offline preview), debris thresholds in metres (8 / 25 / 5
  voxels, server 262; every `SimulationConfig` resolves them from its world's size, and the blast's crumb rule
  follows the session's size), tree trunks sized in metres (1 or 3 voxels), region files record the voxel size and
  refuse another size or an unreadable file without ever overwriting it (saves report regions left unwritten), `Terrain.GoldenChunkHashes` regenerated (probes mapped to 4 m chunks; noise and height bits unchanged), lod.md
  rebased; numbers in its "Stage 3 as built" (timers within 5 % of the stage-2 0.125 m run). Owner decisions taken
  with it: furniture stays blocky world voxels, step height stays 0.35 m. Next: the distance budgets (stage 4), then
  the LOD levels (stage 6) that give the 2x view distance. Not checked: windowed play at 0.125 m (the owner).
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
- **Networking**: server-authoritative simulation (M1, stages 2-9). `Vortex::Core`, `Vortex::Physics` and
  `Vortex::Sim` build without GPU or window, the simulation runs on a numbered fixed tick and the world is stored
  as chunk deltas.
- **Double-precision physics** for worlds far from the origin (rendering is already origin-relative).

### Tooling
- GCC / MSVC builds (only Clang is tested), CI running the tests and a headless validation render,
  image-comparison tests against stored screenshots.
