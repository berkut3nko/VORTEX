# Notes: scene, simulation, engine loop, game (`src/scene`, `src/sim`, `src/engine`, `tests/sim`, `tests/engine`, game `src/main.cpp`)

Checked against engine `430fde0`.

Read `CLAUDE.md` and `docs/ARCHITECTURE.md` section 7 (7.4 for the simulation) first; nothing from them is repeated
here. Paths are relative to `engine/`; `main.cpp` always means the game's `src/main.cpp`, one directory up. Code is
cited by file and symbol; search for the symbol. Sessions (`src/net`, `src/sim/Region.cppm`, `src/sim/Player.cppm`,
`src/sim/Replication.cppm`) are in `notes/net.md`. The text before this rewrite, with its line numbers and
measurements, is `docs/history/notes/scene-engine-2026-11-18.md`.

## 1. Map

- `src/scene/Components.cppm` - `Entity`, `kNoEntity`, user-data packing (`ToUserData`, `EntityFromUserData`,
  `kEntityUserDataTag`), the plain components (`Transform`, `VoxelBody`, `RigidBody`, `Name`, `PointLight`,
  `Lifetime`, `Debris`), `NetId` and `DespawnReason` [net-6].
- `src/scene/Scene.cppm` - `SceneConfig` (all piece / rubble thresholds, the server and client switches),
  `ResolveThresholds`, `MinPieceVoxels` / `VoxelsForVolume` / `VoxelsForLength`, `VoxelObjectDesc`, `LightDesc`,
  `DamageResult`, `RubbleStats`, the `Scene` interface. Its doc comments are the contract.
- `src/scene/internal/Scene.cpp` - `Impl::CreateBody` (layer, CCD, density), `Crumble` / `CrumblePiece`, `MakeRoom`,
  `ListRubble`, `DespawnDebris`, `UpdateDebris`, `Damage`, `DestroyBelow`, `DestroyAbove`. No render data: the module
  imports nothing of `vortex.graphics`.
- `src/sim/Sim.cppm` - primary unit of `vortex.sim`; re-exports `:simulation`, `:worldop`, `:blast`, `:region`,
  `:player`, `:replication` (see `notes/net.md` for the last three and `:region`).
- `src/sim/WorldOp.cppm`, `internal/WorldOp.cpp` - `WorldOpKind` (wire values), `WorldOp` + factories + `Bounds`,
  `OpResult`, `ApplyOp`, `BuildDetachedPiece`. Kinds 1-6, the last `ClearMask` (`WorldOp::Clear`: a blast's crumbs).
- `src/sim/Blast.cppm`, `internal/Blast.cpp` - `BlastDesc`, `AppliedOp`, `BlastResult`, `BlastPush`, `Blast` (the
  simulation half of a blast), `PresentBlast` (the presentation half, GPU-free; `Engine::PresentBlast` forwards),
  `kMaxIslandVoxels`, `kIslandSearchMargin`, `kFloodVisitBudget`, `kMaxDeferredIslandSearches`,
  `kServerWindowMarginChunks`, `kGraphNodeVisitBudget`, the `kNeck*` budgets of the load check, `IslandBox` /
  `FloodBox`, `BlastTimes` / `IslandSearchStats` [shred-0..1e, shred-4, shred-4b], `RetryIslandSearches`, and at the
  end of the constants the cast work budget (`CastWork`, `CastWorkOf`, `OpWork`, the weights) [net-3]. In Blast.cpp
  the search is `GraphSearch` (default) or `VoxelSearch` (the stage-1 reference), both returning a `Found`;
  `SearchIslands` emits the operations from either. The crumb threshold is `scene::MinPieceVoxels(world.VoxelSize())`
  (8 at 0.125 m).
- `src/sim/Simulation.cppm` - `SimulationConfig`, `ServerConfig`, `DeferredIslandSearch`, `IslandSearchChecks`, the
  `Simulation` interface. Its doc comment is the contract (tick, phases, what is not stepped).
- `src/sim/internal/Simulation.cpp` - `Resolve` (the config overrides), `Impl` (member order), `HeldWindowFocus`,
  `Stream` / `SimulateBodies` / `Lifetimes` (the three phases), `Initialize`, `Shutdown`.
- `src/engine/Engine.cppm` - `EngineConfig`, `GatherInstances` / `GatherLights` (free functions), `Application`
  (5 callbacks), `Engine` accessors (shortcuts into its `Simulation()`), `Tick`, `TerrainReady`, `StepAlpha`,
  `Explode`, `PresentBlast`, `SetTickSource`, `Hud`, `SetImGuiInput`.
- `src/engine/internal/Engine.cpp` - `ApplyCommandLine`, `Impl` (config, input, the simulation, then the graphics
  objects), `Initialize`, `Explode` (= `PresentBlast(sim::Blast(...))`), `PresentBlast` (the `Blast:` line), `Run`
  (the only frame loop), exit statistics, exit code.
- `src/engine/internal/Gather.cpp` - the two gather functions; they read `Scene::Registry()` only.
- Libraries: `scene` + `sim` are `VortexSim` (Core + Physics + EnTT, no GPU); `engine` + `editor` are `VortexEngine`.
- `tests/sim/BlastTests.cpp` - suites `WorldOps` and `Blast` (incl. the two-simulation hash acceptance); helpers
  `Arena` (simulation on flat ground, window loaded), `Mushrooms` (set-up operations with two islands to be),
  `StemCut`. `tests/sim/FaceFloodTests.cpp` - `Blast.*` of shred.md stages 1-1c (face flood, carve replay, deferred
  searches) and the disabled cost case `Blast.DISABLED_CostOfTheTowerAndCastleCases`; helpers `FloodArena`,
  `UnclassifiedGround` (no column pass: a chunk that enters the window stays Loading until the next `Update`),
  `MushroomGround` (a generated mushroom, for a carve into a loading chunk), `Pillar`, `kBaseCut`; and
  `Structure.GraphFloatingEqualsFaceFlood` (every stage-1 case in a graph and a voxel simulation, `Stage1Cases`).
  `tests/sim/NeckTests.cpp` - `Blast.Neck*` (the load check in `sim::Blast`: the release is an ordinary Detach, two
  simulations and replay, server / offline unloaded chunk - support plus a late re-check, face islands not held, a
  chunk beyond the window - the rule switched off), helpers `NeckArena` (window 6 x 8 x 6: the tower's flood box
  reaches y 181), `TowerOnANeck`, `NeckShot`. `tests/sim/RubbleTests.cpp` - `Rubble.*` (pieces over stale colliders,
  the rubble caps through `sim::Blast`). `tests/sim/WorldOpClipTests.cpp` - clipped operations.
- Search checks in the arenas: `FloodArena`, `Arena` (BlastTests) and `RubbleArena` set `checkIslandSearch` and fail
  a test whose searches differ or that ran a search without comparing it (`IslandSearchChecks::run`;
  `FloodArena::expectChecked = false` for the arenas that run one search on purpose); voxel-search counters are in
  `BlastResult::reference` there. The three arenas also fail when the load check released anything
  (`IslandSearchChecks::neckReleases`; `Arena` / `RubbleArena::expectNeckReleases` for the tests that want a
  release). `Structure.GraphFloatingEqualsFaceFlood` runs with `neckRule = false` (the voxel reference has no load
  check).
- `tests/sim/SimulationTests.cpp` - suite `Simulation`; helpers `FlatWorld` (a simulation on flat ground, window
  loaded) and `Playground` (crates, a static block, a lamp, a flash). `tests/sim/ServerSceneTests.cpp` - suite
  `ServerScene`; helper `Bench` (store + materials + physics + particles + scene, no world). All of `tests/sim` is
  `VortexSimTests`, which links `VortexSim` only.
- `tests/engine/SceneTests.cpp` - suites `Scene`, `SceneDamage`, `SceneDebris`; `Fixture` builds store + materials +
  physics + particles + scene without a GPU. It stays in `VortexEngineTests` because it also tests the gather
  functions (`import vortex.engine`).
- `tests/engine/EditorTests.cpp` - editor logic (`notes/core-fx-editor.md`); it uses the scene.
- `main.cpp` - `ShredborneGame`: playground (`BuildPlayground`), player (`SpawnPlayer`, `OnFixedUpdate` through
  `sim::StepPlayer`), headless self-test and scripts (`--climb` / `--fly` in `UpdateScript`, probe, tower, castle,
  neck), `--camera` / `--editor` / `--pause-menu` / `--no-hud` parsing in `main`, play-mode cursor (`CursorCapture`),
  pause menu, HUD (`DrawHud`), the F3 ImGui debug overlay (`DebugOverlay`). The session application is
  `../src/Session.cpp` (`SessionGame`, `RunSession`).
- `Engine` has no unit test (needs a device): `Run` and the game are checked only by the headless runs. Both halves
  of a blast, `sim::Blast` and `sim::PresentBlast`, are unit tested in `tests/sim`. What `Run` does to the simulation
  is three `Simulation` calls, and those are unit tested.

## 2. Invariants

Ownership and release
- A body belongs to the `RigidBody` component. The only release path is the `on_destroy<RigidBody>` hook
  (`Impl::OnRigidBodyDestroyed`, connected in the `Impl` constructor). It fires for `destroy`, `remove<RigidBody>`,
  `clear`; it does not fire for `emplace_or_replace`, `replace`, `patch` or assigning `.body` - those leak the old
  body.
- Chunk slots belong to `voxel::VoxelModel`, shared through `VoxelBody::model`. Any other holder of the pointer (the
  game's `m_Crate`; a test local) keeps the slots in use.
- A baked model points at its store (`Model.cppm`) and must die before it. The game relies on `ShredborneGame game`
  being declared after `Engine engine` (`main`).
- `entt::registry`'s destructor fires no signals, so `~Scene` calls `Clear()`. `Simulation::Shutdown` (also the
  destructor): `scene.Clear()`, terrain, world, `physics.Shutdown()`. `Engine::~Engine`: device idle,
  `sim.Shutdown()`, then ui, renderer, device, window. `Simulation::Impl` declares `scene` after `store`, `physics`,
  `particles` because it is constructed from references to them, and `jobs` first (queued jobs run in its
  destructor, after everything else).
- `Engine::Impl` declares `sim` before the graphics objects: the renderer is handed the chunk store, materials and
  world of the simulation every frame.
- Failure leaks nothing: `CreateVoxelObject` returns `kNoEntity` before creating the entity if the bake fails and
  destroys the entity if the body fails.

Entity ids
- 32 bits: 20 index + 12 version (`Components.cppm`). Body user data = tag "VXSE" in the high half, entity in the low
  half. Entity 0 is valid; terrain colliders use user data 0.
- Every `Scene` function accepts `kNoEntity` and stale ids (`Scene.StaleAndNullEntitiesAreSafeEverywhere`); a new
  public function starts with `registry.valid(entity)`.
- Every voxel object has `Transform::scale == SceneConfig::voxelSize`, and `Simulation` overwrites that with
  `world.voxelSize` (`Resolve`). `Damage` and `CreateBody` read the scale as the voxel size; do not write it.
- Never keep component pointers across a call that creates or destroys entities or components; `Damage` copies
  everything out first.

Threading: scene, simulation, engine and all `Application` callbacks are main-thread only (`Scene.cppm`), nothing
here locks; jobs run inside `World::Update` / `TerrainColliders::Update` - never touch the scene from a job.

The simulation (`sim::Simulation`)
- It owns, in this order: job system, material library, chunk store, world, physics, terrain colliders, particle
  pool, scene, tick counter, and the terrain graph cache (`TerrainGraphs()`), the deferred island searches and the
  replication state. `Engine::World()`, `Physics()`, `Scene()` ... return its members.
- `Tick()` = fixed steps simulated so far = number of the next tick. Only `Step` and `StepPhysics` add one. Inside
  `OnFixedUpdate`, `Engine::Tick()` is the tick being simulated; in a headless run it is the 0-based frame number.
- `Step(focusPoints)` = `Stream(HeldWindowFocus(), focusPoints)` -> `SimulateBodies()` -> `Lifetimes(fixedDt)` ->
  `++tick`. The public phases are the same three helpers; `StepPhysics` adds the `++tick`.
- `Lifetimes` = `scene.Update(dt)` -> `DestroyBelow(window origin y - fallOutDepth)` -> `DestroyAbove(BuildCeiling())`
  [vd-1]: `Simulation::Initialize` calls `Scene::SetBuildCeiling` with `World::BuildCeiling()` (+infinity without a
  fixed height range); `CreateVoxelObject` refuses an object whose rotated box would reach above it (`Log::Warn`,
  `kNoEntity`); `DestroyAbove` removes awake dynamic bodies entirely above the ceiling
  (`Simulation.ModelsAboveTheBuildCeilingAreRefusedAndTheSkyAboveItIsAKillPlane`).
- The window: `Step` and `UpdateStreaming(focusPoints)` call `World::Update` with the centre of the chunk the window
  is already centred on (`HeldWindowFocus`), so nothing recentres. Only `UpdateStreaming(windowFocus, focusPoints)`
  moves it. `focusPoints` go to `TerrainColliders::Update` only.
- `Resolve` forces `scene.voxelSize = world.voxelSize` whatever the caller configured (and so the scene's thresholds
  resolve at the world's size). The world's `saveDirectory` is passed through (offline game, map editor);
  `ServerConfig` clears it (a session world is memory only). Tests: `OfflineWorldKeepsItsEditsInTheSaveDirectory`,
  `ServerWorldIsMemoryOnlyWhateverTheConfigurationAsks`.
- The chunk graphs live in the `Simulation` (`TerrainGraphs()`, cleared by `Shutdown`); nothing else invalidates
  them than `World::ChunkRevision` (`notes/voxel.md` "Traps").
- All stepping functions return at once before `Initialize` and after `Shutdown`; `World()` and `Terrain()`
  dereference null there, exactly as `Engine::World()` always did.

Order within a frame (`Engine::Run`)
1. `PollEvents` (windowed) or `input.NewFrame()` (headless); `hud.Begin`, `ui.BeginFrame`, `UpdateInputCapture`.
2. `sim.UpdateStreaming(camera, {camera})` = `world->Update(camera.position)`; `terrain->Update({camera},
   colliderRadius)`. A session (`EngineConfig::windowFollowsCamera = false`) streams with
   `sim.UpdateStreaming({camera})`. Then `sim::RetryIslandSearches` and `PresentBlast` of each late result.
3. `scene.BeginFrame()` once (previous = current).
4. Up to 4 ticks: `OnFixedUpdate(fixedDt)` -> `sim.StepPhysics()` = `terrain->GuardBodies()` -> `physics.Step` ->
   `scene.SyncFromPhysics()` -> `++tick`. After 4 ticks the accumulator is zeroed (time is dropped). With a tick source
   (`Engine::SetTickSource`, a network session) the source decides instead: it gets the frame's wall-clock seconds
   (even headless; at most 0.25 s) and `kMaxSourceTicksPerFrame` (8), returns at most that many and keeps the rest
   (the client clock does; every tick it returns is run), and the accumulator only feeds `StepAlpha`.
5. `OnUpdate(dt)`, `particles.Update`, `OnGui`, `ui.EndFrame`.
6. Gather instances / lights / particles, `RenderFrame`.
7. `scene.SetViewer(camera)`, `sim.UpdateLifetimes(dt)` (the `Lifetimes` phase above).
- `GuardBodies` stays between game code and `Step`: bodies made in `OnFixedUpdate`, or in the previous frame's
  `OnUpdate` / `Explode`, are checked before they are simulated.
- `scene.Update` runs after rendering so a flash is drawn once at full intensity. ImGui calls are legal only in
  `OnGui` (between `ui.BeginFrame` and `ui.EndFrame`).
- `Engine::Hud()` is cleared (`HudCanvas::Begin(renderer.OutputSize())`) right before `ui.BeginFrame` every frame and
  handed to `RenderFrame` as `FrameInput::hud` / `hudDim`; draw into it in `OnUpdate` / `OnGui`, never keep quads
  across frames. `SetImGuiInput` changes ImGui flags at once and the capture flags from the next
  `UpdateInputCapture`.
- Game input routing (`main.cpp` `OnUpdate`): pause menu -> editor -> `CursorCapture::Update` (applies edges to the
  window; on `justPaused` `Menu::Open` and `Editor::ReleaseInput`; `SetImGuiInput(editorOpen || overlay &&
  !captured)`) -> game. `playKeys` = not paused, editor closed; `playLook` = captured and not on the frame the capture
  started; `playClicks` = `CursorCapture::Result::clicks` (the click that focuses the window, or the first one right
  after the pointer enters on Wayland, is not a shot). The overlay's Left Alt is read with `KeyDownRaw`. Headless sets
  all true; the simulation is never paused (sessions are networked).
- `Simulation::Step` (a server tick, no engine) is steps 2, the bracket of 4 and 7 once each with `dt = fixedDt`: no
  `BeginFrame`, no particles, no viewer, the window held.

Time
- Headless: `dt == fixedDt` (`Engine::Run`), so one tick per frame and `StepAlpha() == 0`. Headless frame N is tick
  N; the self-test script relies on it.
- Windowed: `dt` clamped to 0.1 s, 0..4 ticks per frame. `OnFixedUpdate` gets `fixedDt` (1/60); `OnUpdate` and
  `Scene::Update` get the frame `dt`. So lifetimes and rubble ages advance by frame time in the engine and by exactly
  1/60 s per tick in `Simulation::Step`: the two are not tick-for-tick equal.

Units, spaces, ranges
- Scene API is world metres. A voxel object's `Transform::current.position` is the model's minimum corner, `scale`
  the voxel size; the body's position is that same corner (`Impl::CreateBody`), not the centre of mass.
- Transforms are float. `LightDesc::position` and `Damage`'s `center` are `dvec3` but are cast to `vec3`, as in
  `Explode`: far from the origin precision is float precision.
- `Explode(center, radius, power)` / `sim::BlastDesc`: metres, metres, material hardness, quantised by `sim::Blast`
  to 1/16 voxel and 1/16 hardness (`BlastResult::center` / `radius` are the quantised values in metres). `power`
  applies to terrain only (`World::CarveSphereQ`); `Damage` ignores hardness, respecting only
  `MaterialFlag_Indestructible`.
- `Damage` does nothing for `radius <= 0` or NaN. `debrisParticleStride` is a divisor, clamped to >= 1 (`Impl`
  constructor). Density is clamped to >= 1 (`Impl::CreateBody`).
- CCD only for Dynamic bodies that are `Layer::Debris` (< `debrisBodyVoxels`) or whose smallest solid extent is <
  `thinBodyVoxels` (`Impl::CreateBody`). `SolidBounds` is `[min, max)`, so the extent is the solid size in voxels.

Voxel size and thresholds [vd-2, vd-3]
- The engine's voxel size is `voxel::kDefaultVoxelSize` = 0.125 m: the default of `WorldConfig`, `TerrainSettings`,
  `SceneConfig` and the editor's glTF `voxelsPerUnit` (8). The game sets no size; `--voxel-size <m>` (game,
  0.1-0.15, offline only: a session takes the server's size) overrides `EngineConfig::world.voxelSize`;
  `sim::Simulation` passes it to the terrain generator and the scene (`Resolve`), physics and the renderer read the
  world's.
- Debris thresholds are metre sizes (`scene::kMinPieceVolume`, `kDebrisBodyVolume`, `kThinBodyLength`,
  `kLargeRubbleVolume`, `sim::kServerReplicatedPieceVolume`), turned into voxels by `scene::VoxelsForVolume` /
  `VoxelsForLength` (`MinPieceVoxels` for the piece minimum): 8 / 25 / 5 / 512 / 262 voxels at 0.125 m. The scene
  thresholds default to `scene::kFromMetres`, resolved in voxels of the scene's `voxelSize` when the scene is built
  (`scene::ResolveThresholds`; `Resolve` does it with the world's size, so `Config()` reports counts); an explicit
  voxel count is kept. `sim::ServerConfig` converts the replicated volume at the config's own world size. The blast's
  crumb rule is `scene::MinPieceVoxels(world.VoxelSize())` (the session's size, shared bit for bit; `kMinPieceVoxels`
  is its value at the default size). Nobody converts by hand (the game does not).
- Not rescaled (cost limits in voxels, not sizes): `sim::kIslandSearchMargin` 24, `kMaxIslandVoxels` 60 000,
  `kMaxBursts` 400. `fx` presets scale particle sizes from a 0.1 m reference voxel.

Rubble budget [shred-1e]
- Every Dynamic object created with `VoxelObjectDesc::debris` and a body (terrain `Detach` pieces, object fragments)
  is rubble when `markDebris` is on: `Debris{age, lifetime}` with the lifetime by size (`RubbleLifetime`) - under
  `debrisBodyVoxels` `debrisLifetime` 15 s, under `rubbleLargeVoxels` (`kLargeRubbleVolume` 1 m^3)
  `rubbleMediumLifetime` 20 s, larger `rubbleLargeLifetime` 30 s (the owner's answer to shred.md question 7). A
  `Debris` added by hand has `lifetime = -1`, meaning `debrisLifetime`; 0 means no time limit. Rubble's lifetime is
  `Debris`, not the `Lifetime` component (that one is for timed lights).
- Caps: `maxDebris` 192 for small rubble, `maxRubbleBodies` 48 for rubble of `debrisBodyVoxels` or more,
  `maxRubbleSlots` 600 chunk-store slots for all rubble models (`Parts().size()` of each). `CreateVoxelObject` makes
  room first (`MakeRoom`): the body cap before the bake, the slot cap after it (the slot count is only known from the
  baked parts; the bake is undone if no room is found). Room = the oldest **resting** rubble
  (`!(active || wantsToMove)`; a frozen piece waiting for colliders is not resting) that helps the full cap, removed
  with `debrisDespawnBursts`. If nothing resting is left, the new piece becomes particles (`CrumblePiece`: one burst
  per `debrisParticleStride` voxels, at most 400) and `RubbleStats::crumbledByCap` counts it. `Update` enforces all
  three caps too (oldest first): only `Damage` splitting rubble can exceed them there.
- A rubble model that cannot be baked (store full) crumbles the same way (`crumbledByBake`, a warning), so a piece
  whose voxels a `Detach` already removed never vanishes. An ordinary object still fails with `kNoEntity` and no
  particles (its caller keeps the model).
- `Scene::Rubble()` counts on the call (O(rubble), no physics reads) and returns the counters.
- The budget is client presentation: a server scene has `markDebris` off, so none of it applies there, and nothing
  in it is hashed. Cost: `CreateVoxelObject` of rubble lists the rubble once (and once more with physics reads only
  when a cap is full); `Update` lists and sorts it once per call - O(n log n) for n <= 240, not measured.

Regions and the cast budget [net-3]
- `src/sim/Region.cppm` holds `RegionDesc`, `RegionWorldConfig` (the one world config of a session), the templates
  and their appliers (`notes/net.md`). The game's plaza is `sim::PlaygroundTemplate`: `BuildPlayground` calls it with
  `loadSite = true`, then `ApplyTemplateOps`, `CreateTemplateLights`, the probe / castle scripts, the player,
  `CreateTemplateObjects` - the order the plaza was always built in, so the headless runs log the same numbers.
  `m_Crate` is the template's crate model (`sim::CrateModel`); E drops more of it.
- `sim::CastWorkOf(BlastResult)` is the work a session server charges a cast: only counters that follow from the
  voxels and the operations. Adding a counter to `IslandSearchStats` that a budget should see means adding it there,
  never `graphsBuilt` / `chunksDecoded` or a time.

The engine in a session [net-5]
- `EngineConfig::simulation` replaces the whole `SimulationConfig` (a session's, from
  `net::Client::MakeSimulationConfig`); the engine's own fields it covers (chunkCapacity, maxParticles,
  fixedTimeStep, colliderRadius, fallOutDepth, world, scene) are then ignored. The renderer still sizes its GPU chunk
  buffer from `Chunks().Capacity()`, so the session store must be capped (`storeLimit` 12 288 in `src/Session.cpp`):
  never hand the engine a region's raw cell count.
- `Engine::SetTickSource` must be set before `Run`; offline games never set it, and the offline loop (accumulator, 4
  ticks, dropped time, `StepAlpha`) is unchanged by it.
- The game's session application is `../src/Session.cpp` (`SessionGame`): per tick `Client::Poll`, presentations
  (`Engine::PresentBlast`), `Client::StepPlayer`; a frame without a tick polls in `OnUpdate`. It owns no editor and
  makes no world change of its own.

Replicated bodies in the scene [net-6]
- `scene::NetId{value, piece, bornTick, despawn}` marks a session server's replicated entity (and nothing offline);
  `DespawnReason` values are wire format. The server observes `on_destroy<NetId>`: set `despawn` before destroying
  (`sim::Despawn`), never remove the component by hand (the observer would report a despawn of a live body).
- `DamageResult::keptOffset` / `keptMotion`: where the survivor's new model origin lies in the old model (voxels) and
  its motion type after the cut - what `ObjectModel` sends. A replicated object `Damage` destroys (below the
  replicated size) is despawned `Broken`, not `ToRubble`: its remains are already the fragments (new shape), so a
  client keeping the old copy as rubble would show voxels that are gone.
- `SceneConfig::rubbleLayer` (a session client's scene): rubble (`VoxelObjectDesc::debris`, and pieces `Damage` splits
  off rubble) of `debrisBodyVoxels` or more is created in `physics::Layer::Rubble` instead of Dynamic. Offline it is
  off and every body keeps its layer. The client's copies of server bodies are kinematic `CreateVoxelObject`s (no
  `Debris`, so no lifetime or cap), moved by `net::Client` between Rubble and Dynamic.
- A client never calls `Damage`: object damage arrives as `ObjectModel` (the copy is re-created at the shifted origin;
  the voxels the new model lost are shown as particles, at most 64 bursts) and as `netId`-0 `EntitySpawn` fragments
  (local rubble).
- The game's session draws the copies like any voxel object (they are baked into the chunk store: piece models take
  slots within the client's 2048-slot headroom); the E key sends `PlayerButton_Crate` and the server makes the crate.

## 3. Traps

Scene
- During `Damage` the entity briefly has no `RigidBody` and a null `VoxelBody::model`. `GatherInstances` (Gather.cpp)
  and `DespawnDebris` check for null; new readers must too.
- Old model released before pieces are baked: do not reorder (`SceneDamage.WorksInAFullChunkStore`, one-slot store).
- Old body removed before piece bodies are created, so the kept piece always finds a free body
  (`SceneDamage.PieceWithoutABodyIsDropped`, `maxBodies = 1`). Velocities are sampled before that; sampling later
  reads a dead body.
- The result of `pieceModel->Bake` is ignored on purpose: shared old model + full store gives a piece that is
  simulated but not drawn (untested). `DamageResult::destroyed` can be true with non-empty `pieces`.
- `SceneConfig::bakeModels = false` (server): no model the scene sees is ever baked, so `Parts()` is empty and
  `GatherInstances` returns nothing for it. Anything new that reads `Parts()` or chunk slots of a scene model must
  cope with an unbaked one (it already had to: the full-store case above). A model baked by its creator stays baked.
- `SceneConfig::markDebris = false` (server) switches off every place that adds `Debris` (`CreateVoxelObject`, the
  `Damage` pieces and remnant) and with it the whole rubble budget. Layers do not change: a small body is still
  `Layer::Debris` with CCD. Pieces below `minPieceVoxels` still never become entities.
- `SceneConfig::replicatedPieceVoxels` (0 = off, the default; the `kServerReplicatedPieceVolume` size from
  `sim::ServerConfig`): in `Damage` a piece below it that would be Dynamic gets no entity, body or slot and is
  returned in `DamageResult::fragments` (model unbaked, pose, velocities sampled from the old body). If that is the
  kept piece the entity is destroyed (`destroyed`, `toRubble`) before later pieces are created. A Static / Kinematic
  object only dented keeps its entity whatever its size (`pieceMotion` must be Dynamic). The test sits before the
  bake, so fragments never take chunk slots. Damage's own particles are still spawned. On a server the game's crate
  is client rubble at its first hit.
- A Static / Kinematic object cut apart becomes Dynamic as a whole; the kept entity's `RigidBody::motion` changes.
- Rubble marking is asymmetric by design: new small pieces always get `Debris`; every piece split off an entity that
  is rubble gets it too (age 0, a lifetime by the piece's size: with the parent's age a piece of old large rubble
  expired on the next `Update`, `PiecesCutOffOldRubbleAreNotRemovedAtOnce`; `PiecesCutOffRubbleStayRubble`); the
  damaged entity only if small, Dynamic and `debrisRemnants`; an existing `Debris` keeps its age and lifetime
  (`SceneDebris.RemnantOfADamagedObjectBecomesRubbleOnceItIsSmall`). Pieces of a non-rubble object of
  `debrisBodyVoxels` or more are ordinary objects (offline crate halves stay).
- `VoxelObjectDesc::debris` only acts with `withBody` and `motion == Dynamic`. Below `minPieceVoxels` it returns
  `kNoEntity` (after crumbling the voxels into particles if visible), which is not an error: do not log or assert on
  it.
- A model with no solid voxels and `withBody = true` gives an entity without `RigidBody`.
- A body frozen by the terrain guard reports zero velocities (`PhysicsWorld::GetState`), and `GetPointVelocity` asks
  the Jolt body, which is static while frozen. So pieces of a frozen object start at rest and are not frozen
  themselves; the flight velocity remembered for the original is lost (no test covers it).
- `Update` handles `Lifetime` before rubble: an entity with both leaves no particle puff; invisible rubble leaves none
  either.
- The caps count only pieces that did not expire this frame; ties break by entity id (`ListRubble`) so headless runs
  stay reproducible. A `Debris` entity without a body counts as resting for the far rule and for the budget. The
  `Update` cap pass (rubble split by `Damage`) removes resting pieces first, then moving ones, oldest first each, like
  `MakeRoom` (`CapAfterASplitRemovesRestingRubbleFirst`); it reads physics state only when a cap is exceeded.
- The far rule applies to small rubble (< `debrisBodyVoxels`) only; larger rubble lives its 20 / 30 s wherever the
  viewer is (`LargeRubbleFarFromTheViewerLivesItsLifetime`). For it "resting" is `!active`, so a frozen small piece
  counts (it has no collider 25 m away and would hang there); `MakeRoom` uses `!(active || wantsToMove)` on purpose.
- `OnRigidBodyDestroyed` wakes the body's bounds + one voxel + 0.05 m after removing it (`ActivateInBox`, frozen
  bodies there get `wake`): Jolt wakes nothing on removal, and a crate on expired rubble stayed asleep in the air
  (`RemovedRubbleWakesWhatRestsOnIt`). It applies to every removal (expiry, caps, `Destroy`, `Damage`'s old body,
  `DestroyAbove` / `DestroyBelow`); `Scene::Clear` sets `clearing` and wakes nothing.
- `DestroyBelow` (world metres, whole AABB below) takes only Dynamic bodies that are awake, or frozen with
  `wantsToMove`; an impulse or `ActivateInBox` on a parked body makes it eligible. NaN position or bounds always go.
- `EntityCount()` uses `storage<entt::entity>()->free_list()`: EnTT internals (version pinned in
  `cmake/Dependencies.cmake`); re-check on an upgrade.
- 12 version bits: EnTT skips version 0xFFF and wraps (`entt/entity/entity.hpp`, `next`), so after 4095 reuses of an
  index a stale id is valid again. Nothing in the scene guards against it; rubble churn is how it would happen.

Engine and blasts
- Blast order. `sim::Blast` (Blast.cpp): carve (`ApplyOp(WorldOp::Carve)`) -> island search: by default
  `voxel::SearchIslandsOnGraph` on `Simulation::TerrainGraphs()` [shred-4], with
  `SimulationConfig::islandSearch = Voxel` the stage-1 search (`ExtractFloatingIslands` with
  `removeFromWorld = false`, then the face flood `voxel::ProveFaceComponents` of the components that touch the island
  box face); the same islands in the same order unless a budget runs out; with the graph search and
  `SimulationConfig::neckRule` (default on) the load check of shred.md Part 4b runs inside it [shred-4b] -> per island
  of at least `scene::MinPieceVoxels(world voxel size)` `WorldOp::Detach` applied with `ApplyOp` (no entity), in-box
  islands in scan order first, then proven ones in candidate order, then the load check's released parts in piece
  order -> one `WorldOp::Clear` for all smaller ones of the three kinds (the union mask over their tight box) ->
  snapshot (`QueryBodies`) -> `Damage` each (fragments get `BlastPush` added) -> impulses -> `ActivateInBox`.
  `sim::PresentBlast` (Blast.cpp): per carve the debris particles, per Detach with netId 0 a rubble object from the
  removed voxels with `op.velocity`, per ClearMask particles only (a stride of at least `debrisParticleStride`, at
  most `kMaxBursts` bursts, pushed with `BlastPush` at the voxel) -> explosion particles -> fragment rubble -> flash.
  Rubble made by `PresentBlast` is created after the snapshot, so a fresh island is **not** damaged by the blast that
  freed it; it still gets the same push, as initial velocity (impulse = mass x `BlastPush`, so
  `Detach::velocity = BlastPush` at the island's corner, the body position).
- `ExtractFloatingIslands` removes nothing itself: the Detach clears the voxels (`World::ClearMask`) and
  `BuildDetachedPiece` makes the piece from what was removed. If `CreateVoxelObject` then fails for an ordinary object
  that terrain is simply gone; only the scene's error line shows it (rubble crumbles instead, above).
- Impulse distance is measured to `BodyState::position` = the model corner, so big objects are pushed off-centre.
  Impulse = mass x `BlastPush` (6 m/s x linear falloff over 3 x radius, straight up at the centre). The loop visits
  Static and Kinematic bodies too: `GetMass` returns their shape mass, but Jolt's `AddImpulse` ignores bodies that are
  not dynamic, so nothing happens. A frozen body keeps the push as velocity for when it resumes
  (`PhysicsWorld::AddImpulse`).
- The impulse loop runs inside a registry view (Blast.cpp): only physics calls belong there.
- Budgets: 400 to 799 terrain debris bursts per blast (stride = removed / 400, rounded down, in `PresentBlast`), at
  most 400 crumb bursts, islands up to `kMaxIslandVoxels` (60 000), search box radius + `kIslandSearchMargin` (24)
  voxels, face flood inside `voxel::kFloodHalfExtent` (96) voxels of the centre with `kGraphNodeVisitBudget` (32 768)
  node visits per blast (graph search) or `kFloodVisitBudget` (240 000) voxel visits (voxel search); the load check
  within `kNeckFlowWorkBudget` (65 536 since [net-3]) and the other `kNeck*` limits. Flash lifetime
  `ln(40*radius / 0.05) / 9` s (0.7 s for 0.7 m). Call `Explode` / `Blast` from `OnUpdate` / `OnFixedUpdate`, never
  inside a registry view.
- Before `Initialize()` succeeds `World()` and `Explode` dereference a null `world`; the other accessors return
  subsystems that exist but are not initialised. Only `TerrainReady` / `TerrainStats` check. `Run` returns 1 if not
  initialised, 2 on validation errors, else 0; `OnInit` is called at the start of `Run`, not by `Initialize`.
- `ApplyCommandLine` also takes `--no-checkerboard`, missing from the doc comment of `EngineConfig::ApplyCommandLine`
  in `Engine.cppm`. `--camera` and `--editor` are game flags (`main`).
- A `Simulation` without an engine blasts with `sim::Blast` and gets no particles, no flash and no island rubble:
  islands are `Detach` operations, crumbs one `ClearMask`; whoever draws calls `sim::PresentBlast` (the engine
  offline, a session client). `BlastResult::Report()` lists chunks an operation could not apply to; offline, a carve
  that skipped chunks is replayed before the late island search [shred-1b].
- Deferred island searches [shred-1b, shred-1c]: `Simulation::DeferredIslandSearches()` holds at most
  `kMaxDeferredIslandSearches` (16). A search waits for its island box (asked with `World::RequestLoad`), or for the
  one chunk a flood met (`waitsForChunk`); `Engine::Run` calls `sim::RetryIslandSearches` after the streaming phase
  every frame (a cheap check) and presents each result (`sim::PresentBlast`; a `deferred` result has no explosion and
  no flash). A search is given up only once its flood box no longer touches the window (`deferredGivenUp`, a
  `Log::Warn` with its attempts). A full list drops the search farthest from the window centre (`deferredDropped` on
  the new blast's result, a `Log::Warn`). A replayed carve is `ops[0]` of the late result (`carveReplayed`), so
  `PresentBlast` shows its debris. A third kind waits since the stage-4b review: the load check took an unloaded
  chunk as support (`IslandSearchStats::neckDeferred`, internal `SearchOutcome::NeckWaits`): the blast's islands are
  out (`islandsSearched` true) and the late search re-runs search and check once that chunk has loaded - for a chunk
  outside the window only after the window moved over it, so a blast whose flood box reaches past the window keeps one
  waiting entry (tests with small windows see it).
- `SimulationConfig::checkIslandSearch` (tests) runs the other search on the same voxels before any operation is
  applied; it doubles the search cost and must stay off in the game.
- `SimulationConfig::sessionServer` (set by `ServerConfig` only) turns the flood's "chunk not loaded" from a deferral
  into an error: counted in `IslandSearchStats::attachedByUnloaded`, `Log::Error`, the piece stays.
- `Engine::PresentBlast` logs one `Blast:` line per blast (`Blast (late search):` for a retry result): ms per phase
  (`BlastTimes` from `sim::Blast` plus its own present time), flood and graph counters, deferral, skipped chunks,
  edited chunks + rubble slots and the bytes they upload (36 936 B each), and the rubble counts (`rubble S small,
  B/48 bodies, N/600 slots`, + crumbled pieces of that blast). Times are wall clock and logged only;
  `IslandSearchStats` are integer work counts. With `checkIslandSearch` a mismatch logs `the graph and the voxel
  island search differed`.
- `EngineConfig::world.saveDirectory` reaches the simulation's world unchanged: with it, edits are loaded from there,
  saved on shutdown and by the editor's "Save world" button (which reads `World::Config().saveDirectory`). The game
  sets none. A session client keeps it empty (its world comes from `RegionWorldConfig`). `EngineConfig` has no
  physics or job-thread settings: the engine's simulation uses the defaults.
- While `Run` is active the engine is the only caller of `Simulation::StepPhysics` / `UpdateStreaming` /
  `UpdateLifetimes`. A game that calls `engine.Simulation().Step()` from a callback simulates a second tick inside the
  frame and moves the tick counter; nothing stops it.
- `Simulation::Step` after `World().LoadBlocking(p)` keeps the window at `p`. A world that was never placed gets its
  window with the minimum corner at chunk (0, 0, 0) on the first `Step`, not centred on the origin.
- The `ParticleSystem` lives in the simulation only because the scene is constructed from it. `Step` never calls
  `Particles().Update`: a simulation stepped without an engine keeps every particle it spawned until the pool is full
  (use capacity 0, as `ServerConfig` does, or update the pool yourself).

Game (`main.cpp`)
- The player is a physics character (`m_Character`, `sim::PlayerCharacterDesc`) moved only by `sim::StepPlayer` in
  `OnFixedUpdate` [net-4]; `sim::StepPlayer` holds it until `HasCollider` says the ground under the capsule has its
  collider (`notes/physics.md` "Players"). `m_PrevFeet = m_Feet` precedes the step so interpolation stands still
  while waiting. The figure `m_Body` has `withBody = false`, so blasts, `Damage` and `DestroyBelow` never affect it.
  It is placed every frame with `teleport = true` and so has no motion vectors; that is the use `SetTransform`'s
  comment names for `teleport`.
- Intent is written in `OnUpdate` as a `sim::PlayerInput` (`m_Input`: move, yaw, pitch, button levels) and consumed
  in `OnFixedUpdate`; a jump press between two ticks is kept in `m_ButtonLatch` and cleared only when a tick ran. The
  camera follows with `StepAlpha()`.
- Fly mode skips `OnFixedUpdate`; leaving it puts the character where the camera is (`sim::RestorePlayer`).
- Headless forces a forward walk and third person each frame. With `--camera` the game starts in fly mode: the player
  never walks and the last line says `in the air`. Only the run without `--camera` proves the controller. The
  frame-90 blast and beam cut run in both.
- `ExerciseEditor` runs only while the editor is open; its frames are 1-based and go up to 160 (10, 100, 110, 120,
  130 import, 150 delete, 160 pick + summary line). The documented `--frames 120 --editor` run (G4) ends at the brush
  case and never imports, deletes or picks.
- The game runs with `fullHeight` and `packHidden` [vd-1]; its start loads with `LoadScope::Surface` (every surface
  and everything within 16 m; caves stream in afterwards), and `BuildPlayground` loads around the plaza the same way
  before it edits it.
- The playground is authored in voxels of 0.1 m and scaled by `S()` (`0.1 / voxelSize`: 0.8 at the default 0.125 m,
  so every box keeps its metre size to the nearest voxel); the figure, the crate, the beam and the editor smoke test's
  glTF import (6 voxels of 0.1 m per unit) too [vd-2]. Above 0.2 m the standard run's beam is 1 voxel thick and its
  cut leaves two particle crumbs (no `1 new object(s)`): the accepted `--voxel-size` range stops at what was run.
- The furniture probe (`BuildProbe`) is built only with `--probe` (implied by `--camera probe`, `--camera probe-far`,
  `--probe-blast`): the default run builds the old world and logs no `Probe:` line. It is authored in metres on the
  1/8 m grid and voxelised by voxel centres, **both ends included**: on an exact tie (a centre on a boundary) both
  boxes that meet there get that voxel. With half-open boxes the top board of the 25 cm shelf at 0.15 m (ending on a
  tie, 12.5 voxels) lost the row its sides got and floated one row above them. The log line counts middle-voxel
  fallbacks (a box with no centre on an axis) and floating islands over the pad (must be 0). The cost of the rule: a
  box whose upper end is on a tie is one voxel longer (at 0.1 m ties are the x.25 / x.75 m boundaries).
- The headless scripts [shred-1, shred-1d, shred-1e, shred-4b]: `--tower-blast` shoots the watch tower's base ring
  (7 shots at 0.5 m, 1.2 m, power 6, from frame 20, every 10 frames; `UpdateTowerBlast`) and logs
  `Tower: freed by shot K of 7`, the piece's speed at +1 / +5 frames (a frozen piece: the velocity it keeps) and, at
  +5, `Tower: store a/b after the cuts; rubble ...`. `--castle-blast` builds a FillBox castle 16-36 m south of the
  plaza and blasts a curtain wall, the keep and a tower at frames 40 / 50 / 60 (the flood's worst case; it adds many
  dense chunks). `--probe-blast` blasts the chair's legs at frame 30 and logs islands, crumbs and each new piece
  (voxels, layer, rubble, mass, position from the probe corner, velocity - the kept one while frozen -, awake / asleep
  / frozen) at +0, 1, 5, 15, 30, 60, 119 frames, and at +5 `Probe piece N keeps its push: horizontal X m/s at +5
  frames of Y m/s given` (keeps = at least half; `LOST` otherwise). The push survives because a piece over a stale
  collider waits frozen with its velocity (`notes/physics.md` "Stale parts of a collider"). `--neck-blast [W]`
  (`UpdateNeckBlast`) is shred.md Part 4b's script. `--after-fill` counts the scripts' frames from the first frame the
  window is filled.
- The character is never destroyed explicitly; it lives inside the simulation that `PhysicsWorld::Shutdown` drops.

## 4. Recipes

Add a component with a scene system
1. Plain struct in `Components.cppm` with documented units. System in `Scene.cpp`: collect ids inside the view loop,
   create / destroy after it (as in `Update`, `UpdateDebris`).
2. Call it from `Simulation` (`Stream` / `SimulateBodies` / `Lifetimes`), not from `Engine::Run`, so that a server
   runs it too: per tick in `SimulateBodies` next to `SyncFromPhysics`, with lifetimes in `Lifetimes` next to
   `scene.Update`. Update the frame protocol comment in `Scene.cppm`, the `Simulation` class comment and ARCHITECTURE
   7.1 / 7.4.
3. Decide what `Damage` does with it: components stay on the kept piece; new pieces get only Transform, VoxelBody,
   RigidBody, Name and maybe Debris.
4. Decide what it does on a server (`sim::ServerConfig`): anything that only serves the picture belongs behind a
   `SceneConfig` switch like `bakeModels`, or outside the scene. If it is replicated, it needs an event and a
   protocol bump (`notes/net.md` recipes).
5. Test with `Fixture`; extend `StaleAndNullEntitiesAreSafeEverywhere` for a new public function. A system that runs
   on the tick gets a `Simulation` test in `tests/sim` as well.

Change a piece / rubble threshold or rule
1. `SceneConfig` default and comment (`Scene.cppm`), or the metre constant it resolves from; the table in
   ARCHITECTURE 7.3.
2. Logic: `Impl::CreateBody`, `IsSmall`, `RubbleLifetime`, `MakeRoom`, `CreateVoxelObject`, `Damage`.
3. All of `SceneDebris` (`DefaultThresholdsAtTheEngineVoxelSize` runs the true defaults, `ThresholdsKeepTheirMetreSize`
   pins the metre sizes), `Rubble.*` and `ServerScene.*` in `tests/sim` (a
   rule that marks rubble must honour `markDebris`); headless: compare `R rubble`. Tests written in voxels of 0.1 m
   (`SceneTests.cpp`, `ServerSceneTests.cpp`, `BlastTests.cpp` arenas) set 0.1 m explicitly and leave the thresholds at
   `kFromMetres`; the crumb tests use `scene::MinPieceVoxels(0.1)`.

Change what `Damage` does to pieces
1. The piece loop in `Scene::Damage`. Keep "release old model, then bake" and "sample velocities, then remove the
   body", and the switches (`bakeModels`, `markDebris`, `replicatedPieceVoxels`). Update the `Damage` comment
   (`Scene.cppm`) and `DamageResult`.
2. Tests: `CutBeamBecomesTwoEntitiesThatInheritMotion`, `CutRotatedBeamKeepsEveryVoxelInPlace`,
   `PiecesOfASpinningObjectKeepSpinningAndMotionVectors`, `DentedStaticObjectStaysStatic`,
   `StaticObjectCutApartBecomesDynamic`, `SharedModelIsNeverModified`. The `CollectVoxels` helper (SceneTests.cpp)
   proves no voxel moved or was duplicated.
3. Headless run: `Beam cut: ... 1 new object(s)` must not change (G3).

Add an effect to a blast
1. Decide the half. Changes to the simulation (world, bodies, entities that exist on a server) go into `sim::Blast`
   and must be returned in `BlastResult` if a client needs them; terrain changes only as `WorldOp`s through `ApplyOp`.
   Pictures (particles, lights, local rubble) go into `sim::PresentBlast`, made from the result and the client's own
   world. Spawned entities need a `Lifetime` or `debris = true`.
2. Both halves are unit tested (`tests/sim/BlastTests.cpp`); `Engine::PresentBlast` only forwards.
3. Headless run (`Blast at the crates: A -> B objects (T terrain voxels, I island(s), C crumb(s) of K voxels, V object
   voxels in N object(s))`) and a `--camera` screenshot of the plaza (`--camera 14.3,7.0,-4.5,3.1416,-0.2` frames the
   crates).

Add an `Application` callback or move a loop step
1. Virtual with an empty default (`Application` in `Engine.cppm`), call site in `Run`, keeping the order rules of
   section 2. A step that belongs to the simulation moves inside `Simulation` (one of the three phases), so `Step` has
   it too.
2. Update the frame protocol comment in `Scene.cppm` and ARCHITECTURE 7.1 / 7.4; both headless runs; `Simulation.*`
   in `tests/sim`.

Add something to the simulation (a new subsystem, a new phase)
1. Member of `Simulation::Impl` in dependency order (what references comes after what is referenced), release in
   `Shutdown` in reverse. Accessor in `Simulation.cppm`; a shortcut on `Engine` only if games need it.
2. It must not import `vortex.graphics` or `vortex.engine`: the configure step fails otherwise
   (`vortex_assert_gpu_free`). A setting goes into `SimulationConfig`; if a server needs another value, set it in
   `ServerConfig` and extend `Simulation.ServerConfigChangesOnlyWhatAServerMustNotHave`.
3. Decide per phase: in `Step` only, or in the engine's per-frame calls too. Keep `Step` equal to the three public
   phases in order (`Simulation.TickCountsFixedStepsAndNothingElse`, `LifetimesRunOnTheTick...`).

Change the player controller or the self-test script
1. Movement rules go into `sim::StepPlayer` / `PlayerTuning`, never into the game (`notes/net.md` recipe "New player
   rule"). The game's side: input in `OnUpdate`, the step in `OnFixedUpdate`, camera in `OnUpdate`.
2. Constants: `kEyeHeight` 1.62, `kPlayerHeight` 1.8, `kPlayerRadius` 0.3 m (`main.cpp`, copied into `m_Tuning` by
   `SpawnPlayer`); walk 3.8, run 7.0, jump 4.6 m/s are the `sim::PlayerTuning` defaults; figure 6 x 18 x 4 voxels of
   0.1 m, scaled by `S()`, centred on the feet.
3. Script steps hang on `m_HeadlessFrame` (blast and beam at 90) and the `ExerciseEditor` cases; the measurement
   scripts on `m_ScriptFrame`. A step past frame 200 (120 with `--editor`) never runs with the gate commands: change
   the gate in `CLAUDE.md` in the same commit. Keep the log lines the gates look for: `Blast at the crates`, `Beam
   cut`, `Player feet`, `Probe:`, `Tower:`, `Neck:`.

## 5. Verifying a change here

- Gate G2 (all tests). The suites of this area: `Scene*` in `VortexEngineTests` (then the whole binary, because
  `EditorTests.cpp` uses the scene); `Simulation.*`, `ServerScene.*`, `WorldOps.*`, `Blast.*`, `Rubble.*`,
  `Structure.GraphFloatingEqualsFaceFlood` in `VortexSimTests`.
- Simulation: `Steps600TicksWithoutARenderer` and `ServerConfigurationSteps600Ticks...` are the stage-1 acceptance
  (crates at rest on the terrain collider, flash gone, tick = 600); `StepHoldsTheWorldWindowWhereverItWasPlaced`,
  `WorldIsMemoryOnlyWhateverTheConfigurationAsks`, `TickCountsFixedStepsAndNothingElse` pin the three rules of
  section 2. The fixtures wait for collider cooking (`Jobs().WaitIdle()` while `Terrain().Stats().pending != 0`), so a
  failure that comes and goes is an ordering bug, not timing: run with `--gtest_repeat=20`.
- That `VortexSimTests` has no renderer in it is checked when CMake configures
  (`vortex_assert_gpu_free(VortexSimTests)`); by hand: `ldd build/engine/tests/VortexSimTests` lists only system
  libraries.
- Release: `Scene.CreateAndDestroyReleaseBodyAndChunkSlots`, `RegistryOperationsReleaseBodiesToo`,
  `DestructorReleasesEverything`, `CreationFailuresReturnNoEntityAndLeakNothing`. They assert `physics.BodyCount()`
  and `store.UsedCount()` return to 0; put the same two asserts in every new test that creates objects.
- Ids: `BodiesMapBackToEntities`, `SceneDamage.SnapshotThenDamageHitsEveryObjectOnce`. Frame protocol:
  `PhysicsDrivesTransformsAndPreviousIsLastFrame`, `LifetimeExpiresAndLightsFade`,
  `DestroyBelowRemovesWhatFellOutOfTheWorld`. `SceneDamage.*` and `SceneDebris.*`: see the recipes.
- Gate G3 (headless game run); in the log: `Blast at the crates: A -> B objects (...)` (compare with the run before
  your change); `Beam cut: N voxels removed, 1 new object(s)`; `Player feet at (x, y, z), on the ground` (must not say
  `in the air` in the run without `--camera`, and x / z must differ from the spawn point); `Physics: B bodies (A
  awake, F frozen), T terrain colliders; scene: V voxel objects, R rubble` with R at or below `maxDebris` +
  `maxRubbleBodies` and no large F near the plaza; `World: ... store a/b` with a below b; one `Blast:` line per blast;
  `World: surface filled in N frames`, `World: window filled in N frames` and at exit `World: generation N chunks (D
  dense) in T s ...` (from just before `OnInit` until the window was filled: wall rate, capped by
  `maxIntegrationsPerUpdate` Dense results per frame, and the generator's rate per worker-second,
  `WorldStats::generateMs`). None of: `Scene: could not create the body ...`, `... could not be baked (chunk store
  full)`, `Scene: removed N object(s) that fell out of the world`. Exit code 0.
- Gate G4 (`--editor`): exit 0. `Editor: N entities, last action '...'` appears only if the run reaches frame 160, and
  must not end in `(failed)`.
- Destruction or rubble changes: gates G6 (`--voxel-size 0.1` / `0.15`), G7 (`--probe-blast`), G8 (`--tower-blast`),
  G9 (`--castle-blast`), G10 (`--neck-blast`); measure blast costs after `World: window filled` (`--after-fill`,
  CLAUDE.md measuring rules). Streaming changes: G5 (`--climb`; `--fly` logs its own unloads and store-full counts).
  Anything a session server runs (the simulation, `sim::Blast`, the scene with server switches): G11 and G13.
- Screenshot without `--camera`: third-person view from behind the figure on the plaza; figure visible and upright,
  crates or their wreckage ahead, nothing hanging in the air, no flash left by frame 200. Windowed behaviour (mouse
  look, jump feel, editor focus) cannot be checked headless: say so.
