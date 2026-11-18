# Architecture history: the sections of ARCHITECTURE.md that the WP3 rewrite changed

Moved verbatim from `engine/docs/ARCHITECTURE.md` lines 1-112 (and 141-214, 683-729, 780-964, 1037-1165) at engine
`430fde0` on 2026-11-18 (WP3 document rewrite). The text below is unchanged; corrections go only under "Errata" at the
end.

---

# VORTEX architecture

VORTEX is a voxel engine for large, fully editable worlds rendered by ray marching, in the
spirit of Teardown's renderer, with the scale and freedom of a Minecraft-like world. This
document is the reference for how the engine is put together and the contract its parts
share. If code and document disagree, fix one of them.

## 1. Goals and non-goals

| Goal | Consequence in the design |
|---|---|
| Huge, streaming, persistent world | Sparse chunk storage, a toroidal window that moves with the player, generation on worker threads, region files that store only what players changed. (Persistence is a feature of `voxel::World`, used by the offline game and the map editor; a networked session's world is memory only, 7.4.) |
| Everything destructible | Voxels are the single source of truth for rendering, collision and gameplay queries; edits are local and cheap (one 37 KiB chunk re-upload). |
| Teardown-style image | Geometry is ray marched (no meshing); lighting is ray traced through the same data (soft sun shadows, ambient occlusion, emissive voxels) and accumulated over time. |
| Runs on integrated GPUs | Rendering at a fraction of the output resolution, one ray of each kind per pixel per frame, every feature can be switched off. |
| Testable without a GPU | Everything except `graphics` builds and runs headless; the ray traversal is one source file compiled for both the GPU and the CPU, so unit tests cover the shader logic. |

Not goals (yet): level of detail cascades for very long view distances, transparency /
refraction, fluid simulation (water is a shaded surface, 5.7), skeletal animation, audio, networking. Section 10 lists what the design already
does to keep those doors open.

## 2. Layout and layering

```
engine/
  cmake/                 Dependencies.cmake (pinned), VortexShaders.cmake (GLSL -> SPIR-V at build time)
  shaders/
    shared/              code compiled by BOTH glslang and the C++ compiler (trace, shading, cube)
    include/             GPU-only includes; common.glsl is the C++ <-> GLSL contract
    *.vert *.frag        one SPIR-V module each
  src/
    core/     vortex.core      log, jobs, profiler, input state, paths, rng, args, game HUD
                               (canvas, bitmap font, menu, cursor capture: data only)
    math/     vortex.math      AABB / frustum / transform, reverse-Z projection, simplex noise
    voxel/    vortex.voxel     chunks, chunk store, world streaming, generator, regions, models,
                               ray queries, colliders, connectivity, .vox / glTF import
    fx/       vortex.fx        particle simulation and effect presets
    physics/  vortex.physics   Jolt behind an engine interface, terrain collider streaming
    scene/    vortex.scene     entities (EnTT): components, object lifetime, destruction
    sim/      vortex.sim       the simulation: world + physics + terrain colliders + scene on a numbered tick;
                               world operations, blasts, regions and their templates, the player step,
                               NetIds and the piece budgets of a session server (replication)
    net/      vortex.net       the session protocol: transports (loopback, impaired, ENet / UDP), wire
                               format, messages, server, client, the client clock and prediction, replicated
                               bodies (copies, snapshots, the entity check), bot pilots, the shared bot
                               (net::Bot, DrainWatch, JudgeBot, exit lines) and the UDP relay (stage 8)
    graphics/ vortex.graphics  Vulkan device, window, renderer, UI layer
    engine/   vortex.engine    application loop, render data of the scene, explosions, glue
    editor/   vortex.editor    in-game editor: tool logic (no ImGui), panels, gizmo
  tests/                 GoogleTest suites, none of which need a GPU
  tools/                 server/: vortex_server (the session server, GPU-free) and example session files;
                         bot/: vortex_bot (one bot player in its own process over UDP, GPU-free; stage 8's soak)
```

Libraries and what they may depend on (a lower layer never imports a higher one):

```
Vortex::Core      core, math, voxel, fx        -> spdlog, glm, tinygltf      (no GPU, no window)
Vortex::Physics   physics                      -> Core, Jolt
Vortex::Sim       scene, sim                   -> Core, Physics, EnTT        (no GPU, no window)
Vortex::Net       net                          -> Sim; ENet (private)        (no GPU, no window)
Vortex::Graphics  graphics                     -> Core, Vulkan, VMA, vk-bootstrap, GLFW, ImGui
Vortex::Engine    engine, editor               -> all of the above, ImGuizmo
vortex_server     tools/server                 -> Net                        (no GPU, no window)
vortex_bot        tools/bot                    -> Net                        (no GPU, no window)
Shredborne (game) ../src                       -> Engine (and so Net and ENet)
```

A dedicated server or a tool links `Vortex::Net` (or less) and never sees Vulkan. This is checked
when the build is configured: `vortex_assert_gpu_free` (`CMakeLists.txt`) walks everything that
`VortexCore`, `VortexPhysics`, `VortexSim`, `VortexNet`, the test binaries `VortexSimTests` and
`VortexNetTests` and the executables `vortex_server` and `vortex_bot` link, directly or through their dependencies,
and stops with an error if Graphics, Engine, Vulkan, GLFW, VMA or ImGui is among it (`vortex_enet`, ENet built
from source, is checked too). A second check, `vortex_assert_no_network`, stops the build if `VortexCore`,
`VortexPhysics`, `VortexSim` or `VortexSimTests` link `VortexNet` or ENet: the network transport is linked privately
into `Vortex::Net` and reaches only what links that (the server, `VortexNetTests`, the engine and the game).

Inside `Vortex::Sim`, `scene` is the lower layer: it imports voxel, fx and physics, nothing of
graphics and not `vortex.sim`. `sim` owns the scene together with the world, physics and the
terrain colliders (`sim::Simulation`, 7.4). In `Vortex::Engine`, `engine` owns one
`sim::Simulation`, steps it and draws it; the two functions that turn a scene into render data
(`GatherInstances`, `GatherLights`) live there because their result types are graphics types.
`editor` is the top layer: it imports `vortex.engine`, and nothing in `engine` or below knows it
exists - a game that wants the editor owns an `editor::Editor` and forwards two callbacks to it
(section 8).

## 3. Conventions

* **C++20 named modules**, Clang. One module per directory (`vortex.<dir>`), one partition per
  `.cppm` file, re-exported by the primary interface (`Voxel.cppm` ...). Implementation units
  (`internal/*.cpp`) start with `module vortex.<dir>;`.
  * All `#include`s go in the global module fragment (between `module;` and the module
    declaration), standard and third-party headers alike. Names from headers included by an
    imported module are **not** visible to the importer: include what you use.
  * Interfaces are contracts: implementation work must not change exported signatures.
* **Naming**: `PascalCase` types and functions, `m_PascalCase` private members, `camelCase`
  public fields and locals, `kPascalCase` constants. Namespaces `vortex`, `vortex::math`,
  `vortex::voxel`, `vortex::fx`, `vortex::physics`, `vortex::graphics`, `vortex::scene`, `vortex::sim`,
  `vortex::editor`.
* **Errors**: no exceptions across engine APIs. Functions that can fail return `bool` /
  `optional` / an invalid handle and log the reason (`Log::Error("... {}", x)`, std::format
  syntax). `Verify(cond, msg)` is for invariants whose violation would corrupt memory.
  Running out of a fixed budget (chunk slots, particles, GPU buffer space) must degrade
  gracefully - drop, log once - and never write out of bounds.
* **Threading**: public interfaces are main-thread only unless documented otherwise. Worker
  jobs (`JobSystem`) get their inputs by value and hand results back through a queue owned by
  the system that submitted them.
* **Units and axes**: metres, seconds, kilograms, radians. Right-handed, +Y up, the camera
  looks down -Z at yaw 0. Colours are linear RGB everywhere except in files and the final
  output transform.
* **Warnings**: engine code compiles clean with `-Wall -Wextra -Wpedantic`.

<!-- source lines 141-214 -->

### 4.3 World

`voxel::World` keeps a box of chunks around a focus point - the *window*, `windowSize` chunks. Each window cell is
`Unloaded`, `Loading`, `Empty`, `Uniform` (one solid material, no storage), `Dense` (owns a slot) or `Packed` (dense
content no ray can reach: only its RLE bytes on the CPU, no slot). Cells are stored in a toroidal array
(`index = coord mod size`, **column-major**: `(x + z*sx) * sy + y`), so moving the window re-targets one slab of
columns, each a contiguous run, and moves no data.

**Full-height window** (`WorldConfig::fullHeight`, the game's setting; `docs/design/view-distance.md`, stage 1).
The window covers a fixed range of rows - `heightRange`, or the generator's `IWorldGenerator::VerticalBounds()` plus
`buildHeadroom` (32 m), rounded outwards to `heightSnap` (16 m): -48..128 m = 44 rows at the default 0.125 m (55 at 0.1 m) for the built-in
terrain - and follows the focus in x and z only, so climbing or flying never unloads anything. The top of the range
is the **build ceiling** (`World::BuildCeiling()`): an edit that would write a solid voxel above it is refused whole
(`EditReport::outOfBounds`, decided from integers), `Scene::CreateVoxelObject` refuses a model reaching above it, and
`Scene::DestroyAbove` is the kill plane for bodies thrown over it (the terrain guard treats the sky above the window
as ready). Without known bounds (or with `fullHeight` off: tests, a session server today) the window is camera-centred
in all three axes as before.

**Column pass.** Every column of the window first gets one job: `IWorldGenerator::ClassifyColumn(cx, cz)` (from the
height tile the terrain generator builds anyway) says which rows are certainly Empty (`>= emptyFrom`), certainly
Uniform (`< solidBelow`) and where the surface band starts (`surfaceFrom`); `Generate` makes its own early-outs from
the same function, so the two agree (tested). Sky and rock cells are installed without a job unless the
`RegionStore` holds a saved chunk there; band cells become `VX_GRID_PENDING` until generated. Jobs go out by
horizontal distance (a counting sort of the columns, rebuilt per window move): everything within `nearRadius`
(16 m) of the focus first, then classification and the surface band of every column, then the buried (cave) rows
(`surfaceFirstBacklog`, default 0 = all surfaces first). The integration budget counts Dense results only.

**Packed** (`WorldConfig::packHidden`, needs the column pass). A Dense result farther than `nearRadius` + half a
chunk from the focus whose row lies below the lowest surface voxel of every column of its 3 x 3 neighbourhood, that
is not from a save and has no face neighbour that is edited or from a save, keeps its `EncodeChunk` runs and shows
in the grid as `0x80000000 | most frequent id`. Columns outside the window are not known when a chunk at the edge is
judged: when a column is classified, the Packed chunks of its neighbours are judged again and the visible ones
decoded. A Packed chunk is made resident (decoded into a slot) when the focus comes within `nearRadius` (scanned
whenever the focus moved a quarter chunk), when an edit touches it (decoded at its first changed voxel, exactly like
an Empty / Uniform cell), for the six face neighbours of every chunk an edit changed or a saved chunk that loads, and
by `EnsureResident`. A resident chunk that is unedited and hidden keeps its runs and is packed again once the focus is
`repackMargin` (8 m) beyond `nearRadius` (so a session in one area does not fill the store). Packing and unpacking
are not content changes: `TakeChangedChunks` does not list them, `WorldHash` hashes the runs, `GetVoxel` /
`ReadChunk` read them, saves are unchanged. `GetChunk` returns nullptr for a Packed chunk (readers that need its
voxels use `ReadChunk`: physics, island search). A load result that finds the store full is kept Packed (drawn as
rock), never installed as Empty, so a saved chunk is never lost (`WorldStats::storeFullLoads`).

`World::Grid()` is the cell array in GPU encoding: `0` empty, `0x80000000 | id` uniform (or packed), `0x80000000`
(id 0) pending, `slot + 1` dense. `World::GridWords()` - what is uploaded - appends one **column top** per column
(window rows `>=` it are air in that column), the **content top** (their maximum) and one **region top** per 4 x 4
columns of the window (window-relative). Raised tops are written at once, lowered ones lazily (a top that is too
high only costs steps). `GridPageRevisions()` gives the revision at which each 4 KiB page last changed; the renderer
copies only those.

Loading runs on the job system (`RegionStore` first, generator second); results are installed on the main thread.
Cells and columns carry generation counters so results for a chunk or column that has left the window are dropped.

Editing (`SetVoxel`, `FillBox`, `FillSphere`, `CarveSphere` and the quantised `FillSphereQ`,
`CarveSphereQ`, `SwapSphereQ`, `ClearMask`) materialises Empty / Uniform cells on demand, marks
slots dirty, flags the cell as modified (it will be saved when it leaves the window or on
`Save()`), and records the chunk in `TakeChangedChunks()` for physics.

**Replicated edits are integer-exact** (`docs/design/networking.md`, "World operations"). A
`voxel::SphereQ` has its centre and radius in q = 1/16 voxel; a voxel is inside when
`d = 16 v + 8 - centre` gives `dot(d, d) <= radius^2` in int64. Carve power is in 1/16 hardness
units and compared as `hardness * 16 > power` (exact). The float `FillSphere` / `CarveSphere`
quantise (`QuantizeSphere`, `QuantizePower`: round to nearest, halves away from zero) and forward,
so every sphere edit is the same integer operation. Every edit visits chunks in (y, z, x) and
voxels in (z, y, x) order and decides from (voxel, old id) only. Given a `voxel::EditReport` it lists
the chunks it touches but could not change: `skipped` (outside the window, Unloaded, Loading) and
`unsynced` (Empty / Uniform with no free store slot); a chunk is changed completely or not at all.
`sim::WorldOp` / `sim::ApplyOp` (7.4) wrap these calls as the unit that travels.

**Render space.** World positions are `double`. Everything a ray or the GPU touches is
expressed relative to `World::WindowOrigin()` (the window's minimum corner): *render space*.
Coordinates therefore stay below a few hundred metres and keep full float precision anywhere
in the world. When the window moves, the origin jumps by whole chunks; the renderer
re-expresses last frame's matrices in the new space.

<!-- source lines 683-729 -->

### 7.2 Destruction

Carving the world returns the removed voxels (for particles) and is followed by a bounded
search for terrain that lost its support (`ExtractFloatingIslands`), which becomes dynamic
voxel objects.

`Scene::Damage(entity, centre, radius)` carves a sphere out of a voxel object:

1. The sphere is applied to a private copy of the volume (voxels of indestructible materials
   stay); the shared model is not touched. Removed voxels throw debris particles.
2. The rest is labelled (`LabelComponents`, a linear flood fill) and split (`SplitComponents`).
   Components below `SceneConfig::minPieceVoxels` become particles that move with the object.
3. The largest piece keeps the entity - id, name, game components - with a new model and a new
   body (the old model is released first, so its chunk slots are available to the pieces).
   Every other piece becomes a new dynamic entity. A static or kinematic object keeps its
   motion type only while it stays in one piece; once cut apart all of it is dynamic. Each piece stays
   exactly where its voxels were, gets the velocity the original body had at its centre and the
   same angular velocity; `Transform::previous` is re-based the same way, so motion vectors
   continue through the cut.
4. If nothing big enough is left the entity is destroyed.

A blast is split into simulation and presentation:

* `sim::Blast(simulation, sphereQ, power)` (or `BlastDesc` in metres, quantised first): apply
  `WorldOp::Carve`; search the box `floor(centre) +- (radius + 24)` voxels for floating terrain
  (`ExtractFloatingIslands` without removing) and turn each island of at least
  `scene::MinPieceVoxels(world voxel size)` (8 at 0.125 m) into a `WorldOp::Detach` (an occupancy mask, `netId = 0`, the
  blast's push at its corner as velocity) applied with `ApplyOp` = `World::ClearMask`; all smaller
  islands ("crumbs") together become one `WorldOp::Clear` (kind `ClearMask`: the union of their
  voxels over the smallest box holding them), applied last; `Scene::Damage` for every body in range (a snapshot first);
  push every body within three radii (`sim::BlastPush` x mass) and wake what is within two. No
  particles, no light, and offline no entity for an island; on a session server
  (`SimulationConfig::replication`) an island of the replicated size that gets a slot (`sim::TakePieceSlot`, largest
  first) carries a NetId in its `Detach` and becomes a server body made from the voxels it removed (7.5,
  networking.md stage 6). Returns the applied operations in order, the
  voxels each one changed, the object fragments (below the replicated threshold, 7.3) and the
  quantised centre / radius.
* `sim::PresentBlast(simulation, result)` (GPU-free, so unit tested; `Engine::PresentBlast` calls
  it): debris particles for a sample of the carved voxels, local rubble for every `Detach` with
  `netId = 0` built from the voxels it removed (`sim::BuildDetachedPiece`,
  `VoxelObjectDesc::debris`, so 7.3 applies, started with the operation's velocity), particles
  only for the crumbs of a `ClearMask` (no model, entity or body), rubble for the object
  fragments, the explosion burst and a flash.

`Engine::Explode` is `PresentBlast(sim::Blast(...))`. A server runs only the first half and sends
the operations; a client applies them with `ApplyOp` and runs the second half.

<!-- source lines 780-964 -->

### 7.4 Simulation

`sim::Simulation` (`vortex.sim`, library `Vortex::Sim`) is the simulated half of a game without
anything that draws it: job system, material library, chunk store, `voxel::World`,
`physics::PhysicsWorld`, `physics::TerrainColliders`, the particle pool and the `scene::Scene`,
plus a tick counter. It is what `vortex::Engine` owns and renders (every `Engine` accessor such as
`World()`, `Physics()`, `Scene()` is a shortcut into it), what a session server runs and what a
network client will predict with (`docs/design/networking.md`).

* **Tick.** A tick is one fixed step of `SimulationConfig::fixedTimeStep` (1/60 s). `Tick()` is
  the number of ticks simulated so far, which is the number of the tick simulated next; it starts
  at 0 and only a physics phase advances it. Inside `Application::OnFixedUpdate`, `Engine::Tick()`
  is the tick being simulated.
* **`Step(focusPoints)`** is one whole tick in the order a server uses:
  `UpdateStreaming(focusPoints)` -> guard, physics step, pose sync -> `UpdateLifetimes(fixedDt)`
  -> `++tick`. `focusPoints` are the positions that need terrain collision around them (the
  players) and may be empty.
* **Phases.** `UpdateStreaming`, `StepPhysics` (which counts the tick) and `UpdateLifetimes(dt)`
  are public so that an owner can interleave its own work; the engine does (frame protocol in
  7.1: streaming and lifetimes once per frame, 0 to 4 physics phases). Game code that changes the
  simulation in a tick runs after the streaming phase and before the physics phase, so the body
  guard sees what it made.
* **World window.** `Step` and `UpdateStreaming(focusPoints)` advance loading but never move the
  window: a server places it once (`World().LoadBlocking(centre)`). Only
  `UpdateStreaming(windowFocus, focusPoints)` recentres it; the engine passes the camera. A world
  whose window was never placed gets it with the minimum corner at chunk (0, 0, 0).
* **Persistence.** `WorldConfig::saveDirectory` is passed to the world as given: an offline game
  or the map editor (`EngineConfig::world.saveDirectory`, the editor's "Save world" button) keeps
  its edits. A session world is memory only - it is `template + seed`, and a world that loaded
  saved chunks would not be the one the other side generated - so `ServerConfig` clears the
  directory, and `net::Client::Attach` refuses a simulation that has one (7.5).
* **Not stepped: presentation.** `Step` neither advances particles nor calls
  `Scene::BeginFrame()`; the owner that draws does both. Lifetimes run per tick in `Step` but per
  rendered frame, with the frame time, in the engine - nothing a server and a client must agree
  on may depend on `Lifetime` or on rubble clean-up.
* **Server configuration.** `sim::ServerConfig(config)` returns the configuration with
  `maxParticles = 0` (every effect is dropped silently), `scene.bakeModels = false` (models keep
  their voxels and bodies but hold no chunk slots: the store belongs to the terrain),
  `scene.markDebris = false` and `scene.debrisRemnants = false` (nothing is rubble), and
  `scene.replicatedPieceVoxels = 512` (smaller object pieces are client rubble, no entity).
* **World operations.** `sim::WorldOp` is one replicated terrain change with integer parameters
  only: `CarveSphere`, `FillSphere`, `SwapSphere`, `FillBox`, `Detach`, `ClearMask` (kind values
  are wire format). `sim::ApplyOp(world, op, edits)` makes the matching `World` call and returns the voxels
  changed plus the skipped / unsynced chunks (4.3). The same operations in the same order on two
  worlds that were equal give equal voxels, whatever the job-thread timing (tested with
  `voxel::WorldHash`). `sim::Blast` (7.2) produces them.
* **Lifetime.** `Initialize(generator)` registers the default world materials, creates the world,
  starts physics and the terrain colliders; `Shutdown()` (also the destructor) destroys entities,
  colliders, world and bodies in that order. Outside of that span `World()` and `Terrain()` must
  not be called and the stepping functions do nothing. Main-thread only.

### 7.5 Sessions

`vortex.net` (library `Vortex::Net`, GPU-free) is the M1 protocol of `docs/design/networking.md`; it runs over ENet
(UDP) between `vortex_server` and the game (`Shredborne --connect`) and in one process over loopback transports for
the tests ("Stage 3 as built" to "Stage 8 as built" there have every choice). Stage 8: `vortex_bot` (`tools/bot`)
is one bot player per process over real UDP, its impairment a `net::UdpRelay` below ENet; `tools/soak.sh` (game
repository) runs a server and several bots for 1 or 10 minutes and compares their hashes and objects at exit.

* **Region.** `sim::RegionDesc` (`vortex.sim:region`) = template id, seed, play bounds (whole chunks), height
  range, generator and `TerrainSettings` (the session's voxel size included). `sim::RegionWorldConfig` turns it into
  the one `WorldConfig` both sides use: window = play bounds + `kServerWindowMarginChunks` (3) on every side, full
  height over the range rounded to 16 m, memory only. `sim::IsValid` must pass before anything is built from
  network or file input.
* **Template.** `sim::BuildRegionTemplate` returns `RegionTemplate{ops, lights, objects, spawn}` without changing
  the world (the playground plaza is `sim::PlaygroundTemplate`). A server applies `ops` as operations 1..k, sends
  lights as `EntitySpawn`, keeps `objects` as its bodies and sends `spawn` in `Welcome`; the offline game applies
  all of it locally (`ApplyTemplateOps`, `CreateTemplateLights`, `CreateTemplateObjects`).
* **Transport.** `net::ITransport` (`Send`, `Poll`, `RoundTripSeconds`, `ReliableBytesInFlight`, `Disconnect`,
  `Flush`; channels state / events / bulk). `EnetTransport` (stage 5) is UDP through ENet 1.3: `Listen` / `Connect`,
  one ENet channel per channel (state unreliable and sequenced, the others reliable), `ReliableBytesInFlight` exact
  (bytes until ENet releases the packet), graceful `Disconnect`, a test-only receive loss below ENet.
  `LoopbackTransport` links two endpoints in process; `ImpairedTransport` wraps any transport with seeded loss,
  delay, jitter, duplicates, reordering, a rate limit and outages, keeping ENet's promise on reliable channels
  (resent, in order).
* **Server.** `net::Server` owns one `sim::Simulation` built with `sim::ServerConfig` and the region's world. A tick:
  receive (handshake: `Hello` with protocol version, `net::BuildHash` and token -> `Welcome` with the join point
  `joinOpSeq`, or `Refuse`; a joined client's `WindowUpdate` / `ChunkRequest` / `ModelRequest`) -> streaming ->
  queued casts within the cast work budget (`sim::CastWorkOf`) and the event byte budget -> physics -> lifetimes ->
  every joined peer's `EventBatch`es (never held), then its bulk while its bytes in flight are under its
  `net::BulkPacer` window -> a `Snapshot` per client on even ticks; since stage 4 the inputs go into each player's queue
  on receive and every player steps (`sim::StepPlayer`, slot order) between streaming and the casts. Every event is
  encoded once and numbered (`opSeq`); the encoded events stay in a backlog only until every joined peer has them.
  Casts must fit the window
  (`sim::CastFitsRegion`). A full store, an incomplete operation or an island search that meets an unloaded chunk
  marks it `Failed`, which ends the session (peers disconnected, nothing more sent).
* **Client.** `net::Client` does not own a simulation: once `Welcomed`, the owner builds one from
  `MakeSimulationConfig` / `MakeGenerator` and gives it to `Attach`, which checks it against the session, loads the
  region window, checks that the store held it and that the probe chunks are its generator's, and applies the events
  after its join point in `opSeq` order through `net::ChunkSync` (duplicates ignored; a gap is a failure, and a failed
  client disconnects). Terrain is never predicted; no operation is ever sent.
* **Checks (stage 7b).** Every 2 s per client the server sends a `HashCheck` (the server's `ChunkHash`
  at opSeq C of the last operations' chunks, 8 modified chunks least recently checked and 12 pristine chunks near the
  player and on its path ahead, `net::HashCheckPlanner`) and an `EntityCheck` (count, xor of NetIds, xor of id-mixed
  model hashes), each right after the events up to C, so the client compares them at exactly C: only chunks current
  with a base at or below C; a chunk that differs becomes `Requested` and is asked for with `ChunkRequest{Repair}`
  (chunk sync's own path below). An entity mismatch asks for the `EntityList` (ids and model hashes at Q): copies the
  server does not have are dropped, missing or wrong ones asked for as `ModelRequest`s. Tombstones are a FIFO. Since
  the review of 7b an entry skipped because its chunk was still loading is kept and compared once the chunk is current
  (dropped by any operation over it), a chunk that differs again soon after its repair is logged instead of looped on,
  and the server sends at most one `EntityList` per client per second.
* **Server terrain mode (stage 7b, milestone M-b).** A client whose probe chunks differ at the `Welcome`, or whose
  checks found three pristine chunks it generated differently, stops generating the server's terrain (its
  `net::SessionGenerator` answers Empty inside the server's window) and flags every `WindowUpdate` with `kTerrainMode`
  (sticky; the server ends a peer that clears it). The server answers each with a `TerrainAck` instead of a
  `WindowAck`: every entering chunk of its window coded by its own cell state - uniform ones inline, Dense and Packed
  ones queued as `ChunkData` (nearest the player's eyes in 3D first). Protocol 10 (review of 7b): the ack codes only the
  entering cells, in `net::ForEachEnteringChunk` order (both sides walk the same cells), 0.7 KiB per window step; a
  window update prunes the queued chunks that left the window. The client installs them by the server's packing
  (`voxel::InstallMode::Content` / `HiddenContent`: a chunk the server packs is packed here, with no store slot, and
  stays packed by the server's bit, not by this client's column classes; shown whenever the server may show it: an
  edited face neighbour, or an operation near it that this client skipped). From then on it is chunk sync as below.
  The price is the join: 16 MiB on the playground's 30 x 44 x 30 window, 40 MiB for the game's 48 x 44 x 48 window on
  the 250 m arena and 0.8 MiB per two-chunk window step there (measured, "Stage 7b as built"), paced by the bulk window.
* **Chunk sync (stage 7a).** A joiner gets no history as events: its `WindowUpdate` (sent on `Welcome`) is answered by
  a `WindowAck{asOfOpSeq, modified bits}` on the events channel; the server keeps a sparse modified map (chunk ->
  last opSeq, fed from the chunks each operation changed, never cleared) and sends the modified chunks of the window as
  `ChunkData{chunk, asOfOpSeq, RLE}` on bulk, encoded when dequeued (nearest first, request answers ahead), then the
  entity baseline (`BaselineEntity` per live NetId with its own asOf and rest tick, nearest the spawn first,
  `BaselineNear` after those within 32 m, `BaselineEnd`). Bulk is paced per peer by a delay-based window over all its
  reliable bytes; a message that does not fit waits only for that peer's bulk in flight (`ITransport::ChannelBytesInFlight`). The client keeps a
  state and a base per chunk (`NoBase`, `Awaiting`, `Based`, `Requested`, `Unsynced`) and one rule: an operation is
  applied to a chunk only if the chunk is current and the opSeq is above its base, live or replayed from its ring of
  the last 1024 operations (`OpRing`); a chunk the ring no longer covers, or that found no store slot, is requested
  whole (`ChunkRequest`). Entity events for a NetId whose copy has not come yet are held; a replicated `Detach` over
  chunks without a base asks for its body (`ModelRequest`). The player starts once the chunks within 32 m are current
  and the baseline near the spawn is in (`BaselineNear`); hashes are compared over the client's sync box (its window inside the server's) once it
  is `Synced()`. The client window is the region's when it fits `ClientSettings::windowChunks` (the game: 48); a wider
  region (the 250 m arena) gets a `windowChunks`-wide window that follows the player with one chunk of slack
  (`sim::ClientWorldConfig`, `WorldConfig::recenterSlackChunks`): every step (`World::TakeWindowSteps`, taken at the start
  of `Poll`, two or more between `Poll`s each sent in order) is a `WindowUpdate`, whose ack marks the entering
  modified chunks; chunks that leave are dropped (`keepUnloadedEdits` false) and come back from the server; chunks
  beyond the server's window are never edited (current once loaded). An installed `ChunkData` is a content change of
  the whole chunk for the renderer (dirty slot) and for physics (the change's edit box is the chunk: no body is
  simulated against its old collider). A window-sized client's hash is `WorldHash` over its sync box, compared with the
  server's over the same box at the same opSeq once the client is `Synced()`.
* **Players (stage 4).** `sim::StepPlayer(physics, terrain, character, state, input, tuning, dt)` is the one
  movement rule (server, predicting client, offline game): integer inputs (`PlayerInput`: move axes, yaw, pitch,
  button levels), edges derived from the levels before, a cooldown, the `HasCollider` gate. A client sends the inputs
  the server has not acknowledged (at most 8, 30 Hz) on the state channel; the server queues 16 ticks ahead, repeats
  the last movement for a missing input and captures a cast edge as {eye, look, input tick, loadout} - a late one
  judged at its own tick from a ring of the player's states - and casts its ray when the cast runs (stage 5). Each
  `Snapshot` carries the tick, the
  event stream position at it, the newest input held, the margin of the newest input that arrived (and its tick,
  dropped ones included), the owner's state with the newest cast's input tick and why it did not run, if it did not,
  and the other players. The client's `TickClock` keeps inputs a target
  margin early (jump when late or well short, 2 % bleed / catch-up, hold the surplus for a hard resync); it predicts
  with its own character (`sim::kSessionCharacterLayers`, the server's mask; the tick length and gravity come in
  `Welcome`), with the colliders under the player flushed before every step on both sides, compares the newest
  snapshot whose events are applied with its history (one that waits 250 ms is given up and counted), and at 2 cm or
  more restores and replays (blended over 100 ms, snapped at 1.5 m). Other players are interpolated in the past
  (`RemotePlayers`) at a server time that only moves forward (`ServerTime`). Driven per tick by `TicksDue` +
  `RunTick` (tests, bots); the engine takes its ticks from `TicksDue(seconds, 8)` through `Engine::SetTickSource` (stage 5; the clock keeps
  what is over 8 for the next frames).
* **The game as a client (stage 5).** `Shredborne --connect HOST[:PORT] --token HEX` (`src/Session.cpp` of the game):
  handshake, then an `Engine` built from `EngineConfig::simulation` = `Client::MakeSimulationConfig(base, 12288)`
  (the store capped at the GPU buffer's size; `Attach` refuses a region that does not leave
  `ClientSettings::minFreeStoreSlots` free) and `windowFollowsCamera = false`; `Attach` loads the region while it
  polls; per tick `Poll`, presentations, `StepPlayer`. Other players are voxel figures; E asks the server for a crate
  (`PlayerButton_Crate`, stage 6); the editor, flight and the offline scripts are not available in a session.
* **Replicated bodies (stage 6).** A session server's bodies carry a `scene::NetId` (counting up, never reused;
  `sim::Replicate`, `sim::AllocateNetId`): the template's crates, players' crates (`PlayerButton_Crate`), terrain
  islands of at least `SceneConfig::replicatedPieceVoxels` (their `Detach` carries the NetId and the server makes the
  body from the voxels the `Detach` removed, as every client does) and object pieces `Scene::Damage` keeps as entities.
  `sim::Blast` asks `sim::TakePieceSlot` for every new piece (a cast's largest first): at most 2 new server bodies per
  tick, 64 awake replicated bodies (pieces and placed objects) and 256 live pieces (`sim::ReplicationConfig`), and none
  while the server's entity byte budget is spent; over a budget the piece is client rubble (`netId` 0 / a fragment),
  and the live cap evicts the oldest resting piece (else the smallest awake one) as `EntityDespawn{ToRubble}`. Every
  destruction of a NetId entity reaches the server's `on_destroy<scene::NetId>` observer and becomes an
  `EntityDespawn`; a body that falls asleep is announced with its pose (`EntityRest`), one that wakes with
  `EntityWake`; a damaged survivor is an `ObjectModel` (or, over the entity byte budget, let go `ToRubble`), one broken
  below the size is despawned `Broken` with its remains as `netId`-0 `EntitySpawn`s. A replicated body is in
  `Layer::Rubble` while it moves and `Layer::Dynamic` at rest or while it carries a player, on the server and every
  client. Awake bodies travel in the snapshots (31 B each, at most 1200 B per snapshot, by a priority of snapshots
  missed over distance); a client takes a snapshot's samples only after the events it was built after. A client
  holds kinematic copies (`net::Client::Entities`), moved to their samples drawn in the past (`net::RemoteBodies`:
  Hermite, 67-200 ms delay) and put at the `EntityRest` pose at rest; samples of the rest's tick or older are refused.
  A player's cast hits the replicated body in front of the terrain.
  Clients make their own rubble (shred.md stage 1e lifetimes and caps) from `netId`-0 pieces and fragments and from
  copies evicted `ToRubble`. `net::CompareEntities` / `EntityListHash` are the entity check (`vortex_server`,
  `tools/session-check.sh`); the wire `EntityCheck` is stage 7b's.
* **Agreement.** `net::RegionHash` is `voxel::WorldHash` over the region window; probe chunks in `Welcome` compare
  the generators; `buildHash` covers the generator's golden values (`voxel::kGeneratorGoldenHash`) and the material
  table (`voxel::MaterialTableHash`).

<!-- source lines 1037-1165 -->

## 9. Testing

* `VortexCoreTests` - chunks, store, traversal vs. a reference DDA, BVH, world streaming /
  editing / persistence, quantised edits (rounding, the int64 sphere test on its boundary and at
  the int limits, spheres vs. a reference across chunk borders and negative coordinates, exact
  hardness compare, swap, `ClearMask`, the skipped / unsynced report), generator determinism, colliders, connectivity, importers, particles;
  the HUD: font bitmaps and measuring, anchors, canvas quads, menu input, cursor capture.
* `VortexPhysicsTests` - bodies, shapes, ray casts, characters (restored states, layer masks, `Layer::Rubble`, no
  pushing with `characterMovesBodies` off), `MoveKinematic`, terrain colliders
  (the player gate `HasCollider`); the body guard
  (a woken body the camera left behind, a body spawned where nothing is cooked, an unloaded
  chunk, the window edge, 60 bodies spread over the map, velocity kept through a freeze, parked
  bodies costing no collider, an edited chunk keeping its collider) and frozen bodies.
* `VortexSimTests` - links `Vortex::Sim` only (no Vulkan, GLFW or ImGui in the binary; checked
  at configure time). `sim::Simulation`: 600 ticks with the offline and with the server
  configuration on flat ground (crates land on the terrain collider and fall asleep, a timed
  light fades and expires on its tick, model chunk slots and particles only offline), 240 ticks
  on the built-in terrain, what counts a tick, lifetimes per tick vs per phase, fall-out, the
  window held by `Step` and moved only by the other streaming call, the body guard
  (inside `Step` and inside `StepPhysics`), fall-out only in the lifetime phase, the persistent
  offline world and the memory-only server world, `ServerConfig`, calls before `Initialize` and after `Shutdown`. The server scene on its own:
  unbaked models through creation and damage (also with a full chunk store), nothing marked as
  rubble, effects dropped by an empty particle pool, the replicated-piece threshold (fragments,
  a survivor going to rubble, an anchored one staying). World operations: `ApplyOp` per kind,
  the skipped report for chunks outside the window and still loading, a detached piece built from
  the removed voxels; `sim::Blast`: islands as `Detach` without entities, crumbs under `scene::MinPieceVoxels(world voxel size)`
  as one `ClearMask` (the 7 / 8 boundary; offline `sim::PresentBlast` makes particles and no
  entity or body for them), an island box that is not loaded, objects damaged and pushed,
  fragments on a server; and the stage-2 acceptance: two simulations with different worker counts
  fed the same operations (islands and crumbs included) have equal world hashes, a slightly
  different stream does not. `sim::StepPlayer`: quantisers, edges and the cooldown, walk / run /
  jump / held, and two simulations with other worker counts stepped with the same inputs staying
  bit-equal (also with a restore every 30 ticks). Replication (stage 6): a piece of the replicated
  size is a server body with its `Detach`'s NetId and the removed voxels as its model, a smaller one
  client rubble; at most 2 new server bodies per tick; the awake cap and the live cap's eviction
  order; damaged replicated objects as models, spawns and broken fragments; nothing offline.
* `VortexNetTests` - links `Vortex::Net` only (checked at configure time). The wire format (every
  event at its byte size, masks, `RegionDesc` bit for bit, messages, forged bytes), the transports
  (reliable order through loss, jitter and an outage; the state channel's loss, duplicates and
  reordering; the rate limit), session files, the build hash and probe chunks, the handshake and
  its refusals, the client's world taken from `Welcome`, the server window, the template split,
  the cast budget (same ticks with other worker counts and a cold graph cache, a client following
  the delayed casts), the client against a hand-written server (overlapping batches, gaps, forged
  `Welcome`s, refused worlds), a failed server or client ending its part, peer timeouts, pacing of a
  late join, and the stage-3 acceptance: a server and two clients over 5 % loss agree after 200
  blasts that free terrain. Stage 4: the client clock and remote players from margin / sample
  sequences, and the movement acceptance with player bots on their own clocks (no correction on
  flat ground over 5 % loss, a blast at a chunk border beside a walking player, a +50 ms ping
  step, three lost input packets carrying a cast, a 150 ms server hitch, forced mispredictions,
  forged packets). Stage 5: casts judged at their input tick and rays cast at dequeue, the cast
  rejection in the owner block, and real UDP on 127.0.0.1 (wall clock, ENet resending through
  5 % receive loss): reliable order, exactly-once delivery and `ReliableBytesInFlight`, disconnects
  and failed connects, and a server with two walking, casting clients over three sockets ending
  with equal world hashes. Stage 6 (`EntityTests.cpp`): the scripted fight with islands and crates,
  a collapse with 64 awake pieces at 5 % loss (the awake and live caps), a split below the
  threshold, a player under falling pieces at 150 ms, resting crates blocking players on both
  sides; the rest rule against a hand-written server; `RemoteBodies` from sample sequences.
* `VortexGraphicsTests` - SPIR-V reflection of the real shader binaries, C++ / GLSL struct
  layout parity (`VxHudQuad` included), the material surface-kind and HUD cell-grid constants
  (C++ vs. the text of `common.glsl`) and
  their packing, proxy cube winding, pipeline description validation. No Vulkan device needed.
* `VortexEngineTests` - the scene with its offline configuration on real chunk store / material /
  physics / particle objects (no device, no window): entity lifetime releasing bodies and chunk slots, shared models
  surviving damage, splitting (a cut beam is two entities whose voxels add up and which inherit
  motion), loose fragments, stale ids through every function, instance and light gathering,
  lifetimes, body <-> entity mapping; rubble (what is marked, crumbs that never become entities,
  lifetime, far rule, cap, fast small bodies not tunnelling) and `DestroyBelow`. The editor logic on the same objects plus a flat-generator
  world (no ImGui context either): the cursor ray (screen centre = `Camera::Forward()`, edges at
  the field of view, y not flipped), points of a cursor ray projecting back to the cursor through
  the matrices handed to ImGuizmo, gizmo matrix round trips, brush application and stroke spacing,
  primitive volumes, placement, sun angles, import palettes, `.vox` / glTF import and its failure
  paths, picking, entity description and duplication, selection going stale.
* `tools/cpu_render` renders a frame on the CPU with the shared shader code and writes a PNG:
  a visual check of world data, traversal and shading that needs no GPU.
* With a GPU: `Shredborne --headless --frames N --screenshot out.png --validation` exercises
  the Vulkan path and fails on validation errors. `--camera x,y,z[,yaw,pitch]` holds the camera
  at a chosen pose for the run (for example `--camera 60,2.5,-100,-2.23,-0.15`: the sea, towards
  the sun). The run ends with the GPU time of every pass: last frame and the median of the last
  120 frames - compare medians. Adding `--editor` opens the editor with every
  panel, cycles the tools each frame and runs its actions (select, spawn, duplicate, brush,
  import, delete), so the editor UI is exercised on every frame of the run. The log ends with
  `HUD: N quads drawn on the last frame`; `--pause-menu` starts with the pause menu open and
  `--no-hud` draws no HUD (screenshot comparison, GPU time of the hud pass).

## 10. Where the design leaves room

* **Level of detail**: the grid descriptor (`size`, `minChunk`, `voxelSize`) is a parameter of
  `VxTraceWorld`; clip-map cascades are additional grids with doubled voxel size.
* **Bigger worlds**: world positions are already `double` / integer voxels; only render space
  is `float`. Physics is single precision and would need Jolt's double precision build far
  from the origin.
* **Networking / dedicated server**: `Vortex::Core`, `Vortex::Physics`, `Vortex::Sim` and
  `Vortex::Net` have no GPU or window dependency, `sim::Simulation` runs the whole simulation on a
  numbered fixed tick (7.4), generation is a pure function of the seed (pinned bit for bit by
  golden tests) and every terrain change is an integer operation (7.5). Stages 7-9 of
  `docs/design/networking.md` add chunk sync, a soak and the on-demand world (the transport, players and replicated
  bodies are built: stages 4-6).
* **Memory**: chunks are dense; palette-compressed or brick-sparse chunk payloads can replace
  `ids[]` behind `VX_VOXEL_ID` without touching traversal.

## 11. Game interface and cursor capture

Design and judged decisions: `docs/design/game-ui.md`. ImGui is for debugging and the editor
only; what a player sees is drawn by the HUD.

* **Data (Core, `vortex.core:hud`, no GPU, no GLFW).** `HudCanvas` - the quads of one frame
  (cap 8192, overflow dropped and reported), `Rect`, `Frame`, `Text` (built-in 5 x 9 bitmap font,
  ASCII 32..126, integer scale, `TextScale()` = max(1, height / 360)), `Crosshair`,
  `DimBackground`. `PlaceRect` anchors. `Menu` (layout, `HandleInput(MenuInput) -> MenuEvent`,
  `Draw`; the pointer counts only once it moves after `Open()`). `CursorCapture` (captured =
  !headless && focused && !editorOpen && !paused && !freeCursor, started only with the pointer
  over the window; Escape pauses; edges; `clicks` once the capturing click is over). All unit tested.
* **Engine.** `Engine::Hud()` is cleared and sized to the output at the start of every frame and
  handed to the renderer as `FrameInput::hud` / `hudDim`. `HudCursor()` converts the cursor to
  framebuffer pixels (HiDPI), `WindowFocused()`, `SetImGuiInput(bool)` (`UiLayer::SetInputEnabled`:
  `ImGuiConfigFlags_NoMouse | NoMouseCursorChange` and no input capture flags; it clears only
  the bits it added, so it does not fight the editor's own `NoMouse` during its right-button look).
* **Contracts.** `HudQuad` == `VxHudQuad` (layout test), `BitmapFont::kGlyphWidth/Height` ==
  `VX_HUD_CELLS_X/Y` (source-text test), bit = row * 5 + column. `FrameUniforms::settings.w` = HUD
  backdrop dim 0..255, applied by `present.frag` (a full-screen blended quad cost 0.15 ms on the
  UHD 620; this costs nothing). HUD colours are display values (after tone mapping).
* **Cursor (game side, `src/main.cpp`).** The game owns a `CursorCapture` and applies its edges
  with `Window::SetCursorCaptured` (GLFW_CURSOR_DISABLED + raw motion; Wayland: pointer lock +
  relative pointer, X11: grab + warp). Mouse look uses `Input::MouseDelta*` only, and only while
  captured and not on the frame the capture started; terrain clicks only with
  `CursorCapture::Result::clicks`. Escape opens the pause menu (it does not stop the
  simulation: sessions are networked); the menu owns input while open and the editor's look is
  ended with `Editor::ReleaseInput`.

