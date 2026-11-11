# Notes: scene, simulation, engine loop, game (`src/scene`, `src/sim`, `src/engine`, `tests/sim`, `tests/engine`, game `src/main.cpp`)

Read `CLAUDE.md` and `docs/ARCHITECTURE.md` section 7 (7.4 for the simulation) first; nothing from them is repeated
here. Line numbers in `src/scene`, `src/sim` and `src/engine` are as of M1 stage 1 (the commit after engine `240c650`);
`main.cpp` and test line numbers are as of game commit `ac17bcf` / engine `ec073a8`. Paths are relative to `engine/`;
`main.cpp` always means the game's `src/main.cpp`, one directory up.

## 1. Map

- `src/scene/Components.cppm` - `Entity`, `kNoEntity`, user-data packing (`ToUserData`, `EntityFromUserData`,
  `kEntityUserDataTag`), the seven plain components.
- `src/scene/Scene.cppm` - `SceneConfig` (all piece / rubble thresholds), `VoxelObjectDesc`, `LightDesc`,
  `DamageResult`, the `Scene` interface. Its doc comments are the contract.
- `src/scene/internal/Scene.cpp` - `Impl::CreateBody` (layer, CCD, density), `Crumble`, `DespawnDebris`,
  `UpdateDebris`, `Damage`, `DestroyBelow`. No render data: the module imports nothing of `vortex.graphics`.
- `src/sim/Sim.cppm` - primary unit of `vortex.sim`; re-exports `:simulation`, `:worldop`, `:blast`. Later stages add
  partitions here (`RegionDesc`, `StepPlayer`; `docs/design/networking.md`).
- `src/sim/WorldOp.cppm`, `internal/WorldOp.cpp` - `WorldOpKind` (wire values), `WorldOp` + factories + `Bounds`,
  `OpResult`, `ApplyOp`, `BuildDetachedPiece`.
- `src/sim/Blast.cppm`, `internal/Blast.cpp` - `BlastDesc`, `AppliedOp`, `BlastResult`, `BlastPush`, `Blast` (the
  simulation half of a blast), `kMaxIslandVoxels`, `kIslandSearchMargin`.
- `src/sim/Simulation.cppm` - `SimulationConfig`, `ServerConfig`, the `Simulation` interface. Its doc comment is the
  contract (tick, phases, what is not stepped).
- `src/sim/internal/Simulation.cpp` - `Resolve` (the two config overrides), `Impl` (member order), `HeldWindowFocus`,
  `Stream` / `SimulateBodies` / `Lifetimes` (the three phases), `Initialize`, `Shutdown`.
- `src/engine/Engine.cppm` - `EngineConfig`, `GatherInstances` / `GatherLights` (free functions), `Application`
  (5 callbacks), `Engine` accessors (shortcuts into its `Simulation()`), `Tick`, `TerrainReady`, `StepAlpha`, `Explode`,
  `PresentBlast`.
- `src/engine/internal/Engine.cpp` - `ApplyCommandLine`, `Impl` (config, input, the simulation, then the graphics
  objects), `Initialize`, `Explode` (= `PresentBlast(sim::Blast(...))`), `PresentBlast`, `Run` (the only frame loop),
  exit statistics, exit code.
- `src/engine/internal/Gather.cpp` - the two gather functions; they read `Scene::Registry()` only.
- Libraries: `scene` + `sim` are `VortexSim` (Core + Physics + EnTT, no GPU); `engine` + `editor` are `VortexEngine`.
- `tests/sim/BlastTests.cpp` - suites `WorldOps` and `Blast` (stage 2, incl. the two-simulation hash acceptance);
  helpers `Arena` (simulation on flat ground, window loaded), `Mushrooms` (set-up operations with two islands to be),
  `StemCut`.
- `tests/sim/SimulationTests.cpp` - suite `Simulation`; helpers `FlatWorld` (a simulation on flat ground, window
  loaded) and `Playground` (crates, a static block, a lamp, a flash). `tests/sim/ServerSceneTests.cpp` - suite
  `ServerScene`; helper `Bench` (store + materials + physics + particles + scene, no world). Both are
  `VortexSimTests`, which links `VortexSim` only.
- `tests/engine/SceneTests.cpp` - suites `Scene`, `SceneDamage`, `SceneDebris`; `Fixture` (:47) builds store +
  materials + physics + particles + scene without a GPU. It stayed in `VortexEngineTests` because it also tests the
  gather functions (`import vortex.engine`).
- `tests/engine/EditorTests.cpp` - editor logic (not covered here); it uses the scene.
- `main.cpp` - `ShredborneGame`: playground, player controller, headless self-test, `--camera` / `--editor` /
  `--pause-menu` / `--no-hud` parsing, play-mode cursor (`CursorCapture`), pause menu, HUD (`DrawHud`), the F3 ImGui
  debug overlay (`DebugOverlay`).
- `Engine` has no unit test (needs a device): `Run`, `PresentBlast` and the game are checked only by the headless
  runs. `sim::Blast`, the half that changes the simulation, is unit tested in `tests/sim/BlastTests.cpp`. What `Run` does to the simulation is three `Simulation` calls, and those are unit tested.

## 2. Invariants

Ownership and release
- A body belongs to the `RigidBody` component. The only release path is the `on_destroy<RigidBody>` hook
  (Scene.cpp:27-32). It fires for `destroy`, `remove<RigidBody>`, `clear`; it does not fire for `emplace_or_replace`,
  `replace`, `patch` or assigning `.body` - those leak the old body.
- Chunk slots belong to `voxel::VoxelModel`, shared through `VoxelBody::model`. Any other holder of the pointer (the
  game's `m_Crate`, main.cpp:448; a test local) keeps the slots in use.
- A baked model points at its store (`Model.cppm:62`) and must die before it. The game relies on `ShredborneGame game`
  being declared after `Engine engine` (main.cpp:478-480).
- `entt::registry`'s destructor fires no signals, so `~Scene` calls `Clear()` (Scene.cpp:151).
  `Simulation::Shutdown` (Simulation.cpp:108-115; also the destructor): `scene.Clear()`, terrain, world,
  `physics.Shutdown()`. `Engine::~Engine` (Engine.cpp:75-83): device idle, `sim.Shutdown()`, then ui, renderer, device,
  window. `Simulation::Impl` declares `scene` after `store`, `physics`, `particles` (Simulation.cpp:67-77) because it
  is constructed from references to them, and `jobs` first (queued jobs run in its destructor, after everything else).
- `Engine::Impl` declares `sim` before the graphics objects (Engine.cpp:54-63): the renderer is handed the chunk
  store, materials and world of the simulation every frame.
- Failure leaks nothing: `CreateVoxelObject` returns `kNoEntity` before creating the entity if the bake fails and
  destroys the entity if the body fails (Scene.cpp:184-197).

Entity ids
- 32 bits: 20 index + 12 version (Components.cppm:20). Body user data = tag "VXSE" in the high half, entity in the low
  half. Entity 0 is valid; terrain colliders use user data 0.
- Every `Scene` function accepts `kNoEntity` and stale ids (`Scene.StaleAndNullEntitiesAreSafeEverywhere`); a new
  public function starts with `registry.valid(entity)`.
- Every voxel object has `Transform::scale == SceneConfig::voxelSize`, and `Simulation` overwrites that with
  `world.voxelSize` (`Resolve`, Simulation.cpp:23-26). `Damage` and `CreateBody` read the scale as the voxel size; do
  not write it.
- Never keep component pointers across a call that creates or destroys entities or components; `Damage` copies
  everything out first (Scene.cpp:300-317).

Threading: scene, simulation, engine and all `Application` callbacks are main-thread only (Scene.cppm:136), nothing
here locks; jobs run inside `World::Update` / `TerrainColliders::Update` - never touch the scene from a job.

The simulation (`sim::Simulation`)
- It owns, in this order: job system, material library, chunk store, world, physics, terrain colliders, particle
  pool, scene, tick counter. `Engine::World()`, `Physics()`, `Scene()` ... return its members.
- `Tick()` = fixed steps simulated so far = number of the next tick. Only `Step` and `StepPhysics` add one. Inside
  `OnFixedUpdate`, `Engine::Tick()` is the tick being simulated; in a headless run it is the 0-based frame number.
- `Step(focusPoints)` = `Stream(HeldWindowFocus(), focusPoints)` -> `SimulateBodies()` -> `Lifetimes(fixedDt)` ->
  `++tick` (Simulation.cpp:136-143). The public phases are the same three helpers; `StepPhysics` adds the `++tick`.
- The window: `Step` and `UpdateStreaming(focusPoints)` call `World::Update` with the centre of the chunk the window
  is already centred on (`HeldWindowFocus`, Simulation.cpp:45-48), so nothing recentres. Only
  `UpdateStreaming(windowFocus, focusPoints)` moves it. `focusPoints` go to `TerrainColliders::Update` only.
- `Resolve` forces one thing whatever the caller configured: `scene.voxelSize = world.voxelSize`. The world's
  `saveDirectory` is passed through (offline game, map editor); `ServerConfig` clears it (a session world is memory
  only). Tests: `OfflineWorldKeepsItsEditsInTheSaveDirectory`, `ServerWorldIsMemoryOnlyWhateverTheConfigurationAsks`.
- All stepping functions return at once before `Initialize` and after `Shutdown`; `World()` and `Terrain()`
  dereference null there, exactly as `Engine::World()` always did.

Order within a frame (`Engine::Run`, Engine.cpp:185-271)
1. `PollEvents` (windowed) or `input.NewFrame()` (headless); `ui.BeginFrame`, `UpdateInputCapture`.
2. `sim.UpdateStreaming(camera, {camera})` = `world->Update(camera.position)`; `terrain->Update({camera},
   colliderRadius)`.
3. `scene.BeginFrame()` once (previous = current).
4. Up to 4 ticks: `OnFixedUpdate(fixedDt)` -> `sim.StepPhysics()` = `terrain->GuardBodies()` -> `physics.Step` ->
   `scene.SyncFromPhysics()` -> `++tick`. After 4 ticks the accumulator is zeroed (time is dropped).
5. `OnUpdate(dt)`, `particles.Update`, `OnGui`, `ui.EndFrame`.
6. Gather instances / lights / particles, `RenderFrame`.
7. `scene.SetViewer(camera)`, `sim.UpdateLifetimes(dt)` = `scene.Update(dt)`, `scene.DestroyBelow(windowOrigin.y -
   fallOutDepth)`.
- `GuardBodies` stays between game code and `Step`: bodies made in `OnFixedUpdate`, or in the previous frame's
  `OnUpdate` / `Explode`, are checked before they are simulated.
- `scene.Update` runs after rendering so a flash is drawn once at full intensity. ImGui calls are legal only in
  `OnGui` (between `ui.BeginFrame` and `ui.EndFrame`).
- `Engine::Hud()` is cleared (`HudCanvas::Begin(renderer.OutputSize())`) right before `ui.BeginFrame` every frame and
  handed to `RenderFrame` as `FrameInput::hud` / `hudDim`; draw into it in `OnUpdate` / `OnGui`, never keep quads
  across frames. `SetImGuiInput` changes ImGui flags at once and the capture flags from the next `UpdateInputCapture`.
- Game input routing (`main.cpp` `OnUpdate`): pause menu -> editor -> `CursorCapture::Update` (applies edges to the
  window; on `justPaused` `Menu::Open` and `Editor::ReleaseInput`; `SetImGuiInput(editorOpen || overlay && !captured)`)
  -> game. `playKeys` = not paused, editor closed; `playLook` = captured and not on the frame the capture started;
  `playClicks` = `CursorCapture::Result::clicks` (the click that focuses the window, or the first one right after the
  pointer enters on Wayland, is not a shot). The overlay's Left Alt is read with `KeyDownRaw`. Headless sets all true;
  the simulation is never paused (sessions are networked).
- `Simulation::Step` (a server tick, no engine) is steps 2, the bracket of 4 and 7 once each with `dt = fixedDt`: no
  `BeginFrame`, no particles, no viewer, the window held.

Time
- Headless: `dt == fixedDt` (Engine.cpp:196), so one tick per frame and `StepAlpha() == 0`. Headless frame N is tick
  N; the self-test script relies on it.
- Windowed: `dt` clamped to 0.1 s, 0..4 ticks per frame. `OnFixedUpdate` gets `fixedDt` (1/60); `OnUpdate` and
  `Scene::Update` get the frame `dt`. So lifetimes and rubble ages advance by frame time in the engine and by exactly
  1/60 s per tick in `Simulation::Step`: the two are not tick-for-tick equal.

Units, spaces, ranges
- Scene API is world metres. A voxel object's `Transform::current.position` is the model's minimum corner, `scale` the
  voxel size; the body's position is that same corner (Scene.cpp:51), not the centre of mass.
- Transforms are float. `LightDesc::position` and `Damage`'s `center` are `dvec3` but are cast to `vec3`
  (Scene.cpp:211, 323), as in `Explode`: far from the origin precision is float precision.
- `Explode(center, radius, power)` / `sim::BlastDesc`: metres, metres, material hardness, quantised by `sim::Blast` to
  1/16 voxel and 1/16 hardness (`BlastResult::center` / `radius` are the quantised values in metres). `power` applies
  to terrain only (`World::CarveSphereQ`); `Damage` ignores hardness, respecting only `MaterialFlag_Indestructible`.
- `Damage` does nothing for `radius <= 0` or NaN (Scene.cpp:298). `debrisParticleStride` is a divisor, clamped to >= 1
  (Scene.cpp:24). Density is clamped to >= 1 (Scene.cpp:44).
- CCD only for Dynamic bodies that are `Layer::Debris` (< `debrisBodyVoxels`) or whose smallest solid extent is <
  `thinBodyVoxels` (Scene.cpp:54-65). `SolidBounds` is `[min, max)`, so the extent is the solid size in voxels.

## 3. Traps

Scene
- During `Damage` the entity briefly has no `RigidBody` and a null `VoxelBody::model` (Scene.cpp:366-416).
  `GatherInstances` (Gather.cpp) and `DespawnDebris` check for null; new readers must too.
- Old model released before pieces are baked (Scene.cpp:376-380): do not reorder
  (`SceneDamage.WorksInAFullChunkStore`, one-slot store).
- Old body removed before piece bodies are created, so the kept piece always finds a free body
  (`SceneDamage.PieceWithoutABodyIsDropped`, `maxBodies = 1`). Velocities are sampled before that (Scene.cpp:350-364);
  sampling later reads a dead body.
- The result of `pieceModel->Bake` is ignored on purpose (Scene.cpp:398-400): shared old model + full store gives a
  piece that is simulated but not drawn (untested). `DamageResult::destroyed` can be true with non-empty `pieces`.
- `SceneConfig::bakeModels = false` (server): no model the scene sees is ever baked, so `Parts()` is empty and
  `GatherInstances` returns nothing for it. Anything new that reads `Parts()` or chunk slots of a scene model must
  cope with an unbaked one (it already had to: the full-store case above). A model baked by its creator stays baked.
- `SceneConfig::markDebris = false` (server) switches off all three places that add `Debris` (Scene.cpp:204, 418, 426).
  Layers do not change: a small body is still `Layer::Debris` with CCD. Pieces below `minPieceVoxels` still never
  become entities.
- `SceneConfig::replicatedPieceVoxels` (0 = off, the default; 512 from `sim::ServerConfig`): in `Damage` a piece below
  it that would be Dynamic gets no entity, body or slot and is returned in `DamageResult::fragments` (model unbaked,
  pose, velocities sampled from the old body). If that is the kept piece the entity is destroyed (`destroyed`,
  `toRubble`) before later pieces are created. A Static / Kinematic object only dented keeps its entity whatever its
  size (`pieceMotion` must be Dynamic). The test sits before the bake, so fragments never take chunk slots. Damage's
  own particles are still spawned. On a server the game's crate (296 voxels) is client rubble at its first hit.
- A Static / Kinematic object cut apart becomes Dynamic as a whole; the kept entity's `RigidBody::motion` changes.
- Rubble marking is asymmetric by design: new small pieces always get `Debris`; the damaged entity only if small,
  Dynamic and `debrisRemnants`; an existing `Debris` keeps its age
  (`SceneDebris.RemnantOfADamagedObjectBecomesRubbleOnceItIsSmall`).
- `VoxelObjectDesc::debris` only acts with `withBody` and `motion == Dynamic` (Scene.cpp:175). Below 16 voxels it
  returns `kNoEntity`, which is not an error: do not log or assert on it.
- A model with no solid voxels and `withBody = true` gives an entity without `RigidBody` (Scene.cpp:190).
- A body frozen by the terrain guard reports zero velocities (PhysicsWorld.cppm:73), and `GetPointVelocity`
  (PhysicsWorld.cpp:888) asks the Jolt body, which is static while frozen. So pieces of a frozen object start at
  rest and are not frozen themselves; the flight velocity remembered for the original is lost (no test covers it).
- `Update` handles `Lifetime` before rubble: an entity with both leaves no particle puff; invisible rubble leaves none
  either (Scene.cpp:97).
- The cap counts only pieces that did not expire this frame; ties break by entity id (Scene.cpp:128-130) so headless
  runs stay reproducible. A `Debris` entity without a body counts as resting for the far rule (Scene.cpp:119).
- `DestroyBelow` (world metres, whole AABB below) takes only Dynamic bodies that are awake, or frozen with
  `wantsToMove`; an impulse or `ActivateInBox` on a parked body makes it eligible. NaN position or bounds always go.
- `EntityCount()` uses `storage<entt::entity>()->free_list()` (Scene.cpp:235-240): EnTT 3.15 internals
  (`cmake/Dependencies.cmake`); re-check on an upgrade.
- 12 version bits: EnTT skips version 0xFFF and wraps (`entt/entity/entity.hpp`, `next`), so after 4095 reuses of an
  index a stale id is valid again. Nothing in the scene guards against it; rubble churn is how it would happen.

Engine
- Blast order. `sim::Blast` (Blast.cpp): carve (`ApplyOp(WorldOp::Carve)`) -> island search with
  `removeFromWorld = false` -> per island `WorldOp::Detach` applied with `ApplyOp` (no entity) -> snapshot
  (`QueryBodies`) -> `Damage` each (fragments get `BlastPush` added) -> impulses -> `ActivateInBox`.
  `Engine::PresentBlast` (Engine.cpp): per carve the debris particles, per Detach with netId 0 a rubble object from
  the removed voxels with `op.velocity` -> explosion particles -> fragment rubble -> flash. Rubble made by
  `PresentBlast` is created after the snapshot, so a fresh island is **not** damaged by the blast that freed it
  (before stage 2 it was, without a hardness check); it still gets the same push, as initial velocity
  (impulse = mass x `BlastPush`, so `Detach::velocity = BlastPush` at the island's corner - the old impulse sampled the
  body position, which is that same corner, not the centre of mass).
- `ExtractFloatingIslands` no longer removes anything itself: the Detach clears the voxels (`World::ClearMask`) and
  `BuildDetachedPiece` makes the piece from what was removed. If `CreateVoxelObject` then fails (store full, no body)
  that terrain is simply gone; only the scene's error line shows it.
- Impulse distance is measured to `BodyState::position` = the model corner, so big objects are pushed off-centre.
  Impulse = mass x `BlastPush` (6 m/s x linear falloff over 3 x radius, straight up at the centre). The loop visits
  Static and Kinematic bodies too: `GetMass` returns their shape mass, but Jolt's `AddImpulse` ignores bodies that are
  not dynamic, so nothing happens. A frozen body keeps the push as velocity for when it resumes
  (PhysicsWorld.cpp:725-743).
- The impulse loop runs inside a registry view (Blast.cpp): only physics calls belong there.
- Budgets: 400 to 799 terrain debris bursts per blast (stride = removed / 400, rounded down, in `PresentBlast`),
  islands up to `kMaxIslandVoxels` (60 000), search box radius + `kIslandSearchMargin` (24) voxels. Flash lifetime
  `ln(40*radius / 0.05) / 9` s (0.7 s for 0.7 m). Call `Explode` / `Blast` from `OnUpdate` / `OnFixedUpdate`, never
  inside a registry view.
- Before `Initialize()` succeeds `World()` and `Explode` dereference a null `world`; the other accessors return
  subsystems that exist but are not initialised. Only `TerrainReady` / `TerrainStats` check. `Run` returns 1 if not
  initialised, 2 on validation errors, else 0; `OnInit` is called at the start of `Run`, not by `Initialize`.
- `ApplyCommandLine` also takes `--no-checkerboard` (Engine.cpp:35), missing from the comment at `Engine.cppm:54-55`.
  `--camera` and `--editor` are game flags (main.cpp:481-489).
- A `Simulation` without an engine blasts with `sim::Blast` and gets no particles, no flash and no island rubble:
  islands are `Detach` operations, whoever draws makes the rubble (`Engine::PresentBlast` offline, a client later).
  `BlastResult::Report()` lists chunks an operation could not apply to; nothing reads it yet (stage 7).
- `EngineConfig::world.saveDirectory` reaches the simulation's world unchanged: with it, edits are loaded from there,
  saved on shutdown and by the editor's "Save world" button (which reads `World::Config().saveDirectory`). The game
  sets none. A networked client must keep it empty (stage 3). `EngineConfig` has no physics or job-thread settings: the engine's simulation uses the defaults.
- While `Run` is active the engine is the only caller of `Simulation::StepPhysics` / `UpdateStreaming` /
  `UpdateLifetimes`. A game that calls `engine.Simulation().Step()` from a callback simulates a second tick inside the
  frame and moves the tick counter; nothing stops it.
- `Simulation::Step` after `World().LoadBlocking(p)` keeps the window at `p`. A world that was never placed gets its
  window with the minimum corner at chunk (0, 0, 0) on the first `Step`, not centred on the origin.
- The `ParticleSystem` lives in the simulation only because the scene is constructed from it. `Step` never calls
  `Particles().Update`: a simulation stepped without an engine keeps every particle it spawned until the pool is
  full (use capacity 0, as `ServerConfig` does, or update the pool yourself).

Game (`main.cpp`)
- The player is a physics character (`m_Character`); the figure `m_Body` has `withBody = false` (:422), so blasts,
  `Damage` and `DestroyBelow` never affect it. It is placed every frame with `teleport = true` (:205) and so has no
  motion vectors; that is the use `SetTransform`'s comment names for `teleport` (Scene.cppm:224-225).
- `OnFixedUpdate` does not simulate until `TerrainReady(capsule AABB)` (:92-94). Keep the box test (the ground is
  usually in the chunk below the feet). `m_PrevFeet = m_Feet` precedes the early return so interpolation stands still
  while waiting.
- Intent (`m_WishVelocity`, `m_WishJump`) is written in `OnUpdate`, consumed in `OnFixedUpdate`; the jump flag is
  cleared only when a tick ran. The camera follows with `StepAlpha()`.
- Fly mode skips `OnFixedUpdate`; leaving it teleports the character to the camera (:121-124).
- Headless forces `wish = forward` and third person each frame (:147-149). With `--camera` the game starts in fly
  mode: the player never walks and the last line says `in the air`. Only the run without `--camera` proves the
  controller. The frame-90 blast runs in both.
- `ExerciseEditor` (:250-299) runs only while the editor is open; its frames are 1-based and go up to 160. The
  documented `--frames 120 --editor` run ends at the brush case and never imports, deletes or picks.
- No `TODO` / `FIXME` markers exist in these files; the weak spots above are the list. The character is never
  destroyed explicitly; it lives inside the simulation that `PhysicsWorld::Shutdown` drops (`sim.reset()`).
- `CLAUDE.md` says rubble "has a `Lifetime`": in the code rubble is the `Debris` component (age, `UpdateDebris`);
  `Lifetime` is only used by timed lights.

## 4. Recipes

Add a component with a scene system
1. Plain struct in `Components.cppm` with documented units. System in `Scene.cpp`: collect ids inside the view loop,
   create / destroy after it (as in `Update`, `UpdateDebris`).
2. Call it from `Simulation` (Simulation.cpp:50-65), not from `Engine::Run`, so that a server runs it too: per tick
   in `SimulateBodies` next to `SyncFromPhysics`, with lifetimes in `Lifetimes` next to `scene.Update`. Update the
   protocol comment (`Scene.cppm:138-150`), the `Simulation` class comment and ARCHITECTURE 7.1 / 7.4.
3. Decide what `Damage` does with it: components stay on the kept piece; new pieces get only Transform, VoxelBody,
   RigidBody, Name and maybe Debris (Scene.cpp:422-427).
4. Decide what it does on a server (`sim::ServerConfig`): anything that only serves the picture belongs behind a
   `SceneConfig` switch like `bakeModels`, or outside the scene.
5. Test with `Fixture`; extend `StaleAndNullEntitiesAreSafeEverywhere` for a new public function. A system that
   runs on the tick gets a `Simulation` test in `tests/sim` as well.

Change a piece / rubble threshold or rule
1. `SceneConfig` default and comment (`Scene.cppm:26-70`); the table in ARCHITECTURE 7.3.
2. Logic: `Impl::CreateBody`, `IsSmall`, `CreateVoxelObject` (Scene.cpp:175-204), `Damage` (:397, 418, 426).
3. All of `SceneDebris` (`DefaultsKeepTinyFragmentsOutOfTheScene` pins defaults) and `ServerScene.*` in `tests/sim`
   (a rule that marks rubble must honour `markDebris`); headless: compare `M rubble`.

Change what `Damage` does to pieces
1. Loop at Scene.cpp:389-429. Keep "release old model, then bake" and "sample velocities, then remove the body",
   and the two switches (`bakeModels` at :400, `markDebris` at :418 and :426).
   Update the `Damage` comment (`Scene.cppm:249-279`) and `DamageResult`.
2. Tests: `CutBeamBecomesTwoEntitiesThatInheritMotion`, `CutRotatedBeamKeepsEveryVoxelInPlace`,
   `PiecesOfASpinningObjectKeepSpinningAndMotionVectors`, `DentedStaticObjectStaysStatic`,
   `StaticObjectCutApartBecomesDynamic`, `SharedModelIsNeverModified`. The `CollectVoxels` helper (SceneTests.cpp:85)
   proves no voxel moved or was duplicated.
3. Headless run: `Beam cut: ... 1 new object(s)` must not change.

Add an effect to a blast
1. Decide the half. Changes to the simulation (world, bodies, entities that exist on a server) go into `sim::Blast`
   and must be returned in `BlastResult` if a client needs them; terrain changes only as `WorldOp`s through `ApplyOp`.
   Pictures (particles, lights, local rubble) go into `Engine::PresentBlast`, made from the result and the client's
   own world. Spawned entities need a `Lifetime` or `debris = true`.
2. `sim::Blast` is unit tested (`tests/sim/BlastTests.cpp`); `PresentBlast` has no test: keep it glue.
3. Headless run (`Blast at the crates: A -> B objects (T terrain voxels, I island(s), V object voxels in N
   object(s))`) and a `--camera` screenshot of the plaza (`--camera 14.3,7.0,-4.5,3.1416,-0.2` frames the crates).

Add an `Application` callback or move a loop step
1. Virtual with an empty default (`Engine.cppm:76-84`), call site in `Run`, keeping the order rules of section 2.
   A step that belongs to the simulation moves inside `Simulation` (one of the three phases), so `Step` has it too.
2. Update `Scene.cppm:138-150` and ARCHITECTURE 7.1 / 7.4; both headless runs; `Simulation.*` in `tests/sim`.

Add something to the simulation (a new subsystem, a new phase)
1. Member of `Simulation::Impl` in dependency order (Simulation.cpp:67-81: what references comes after what is
   referenced), release in `Shutdown` in reverse. Accessor in `Simulation.cppm`; a shortcut on `Engine` only if games
   need it.
2. It must not import `vortex.graphics` or `vortex.engine`: the configure step fails otherwise
   (`vortex_assert_gpu_free`). A setting goes into `SimulationConfig`; if a server needs another value, set it in
   `ServerConfig` and extend `Simulation.ServerConfigChangesOnlyWhatAServerMustNotHave`.
3. Decide per phase: in `Step` only, or in the engine's per-frame calls too. Keep `Step` equal to the three public
   phases in order (`Simulation.TickCountsFixedStepsAndNothingElse`, `LifetimesRunOnTheTick...`).

Change the player controller or the self-test script
1. Simulation in `OnFixedUpdate` (uses `fixedDt`), intent and camera in `OnUpdate`.
2. Constants: eye 1.62, height 1.8, radius 0.3 m (main.cpp:444-446); walk 3.8, run 7.0, jump 4.6 m/s; figure 6x18x4
   voxels, centred by the (3, 0, 2) voxel offset (:204).
3. Script steps hang on `m_HeadlessFrame` (blast and beam at 90, :151) and the `ExerciseEditor` cases. A step past
   frame 200 (120 with `--editor`) never runs with the documented commands: change `CLAUDE.md` in the same commit.
   Keep the log lines the checks look for: `Blast at the crates`, `Beam cut`, `Player feet`.

## 5. Verifying a change here

Unit tests, no GPU: `./build/engine/tests/VortexEngineTests --gtest_filter='Scene*'` (40 tests in `SceneTests.cpp`),
then the whole binary because `EditorTests.cpp` uses the scene; and `./build/engine/tests/VortexSimTests` (32 tests:
`Simulation.*` 15, `ServerScene.*` 9, `WorldOps.*` 3, `Blast.*` 5), about 5 s.
- Simulation: `Steps600TicksWithoutARenderer` and `ServerConfigurationSteps600Ticks...` are the stage-1 acceptance
  (crates at rest on the terrain collider, flash gone, tick = 600); `StepHoldsTheWorldWindowWhereverItWasPlaced`,
  `WorldIsMemoryOnlyWhateverTheConfigurationAsks`, `TickCountsFixedStepsAndNothingElse` pin the three rules of
  section 2. The fixtures wait for collider cooking (`Jobs().WaitIdle()` while `Terrain().Stats().pending != 0`), so a
  failure that comes and goes is an ordering bug, not timing: run with `--gtest_repeat=20`.
- That the binary has no renderer in it is checked when CMake configures (`vortex_assert_gpu_free(VortexSimTests)`);
  by hand: `ldd build/engine/tests/VortexSimTests` lists only libstdc++, libm, libgcc_s, libc.
- Release: `Scene.CreateAndDestroyReleaseBodyAndChunkSlots`, `RegistryOperationsReleaseBodiesToo`,
  `DestructorReleasesEverything`, `CreationFailuresReturnNoEntityAndLeakNothing`. They assert `physics.BodyCount()`
  and `store.UsedCount()` return to 0; put the same two asserts in every new test that creates objects.
- Ids: `BodiesMapBackToEntities`, `SceneDamage.SnapshotThenDamageHitsEveryObjectOnce`. Frame protocol:
  `PhysicsDrivesTransformsAndPreviousIsLastFrame`, `LifetimeExpiresAndLightsFade`,
  `DestroyBelowRemovesWhatFellOutOfTheWorld`. `SceneDamage.*` (14) and `SceneDebris.*` (9): see the recipes.

Headless game run (commands in `CLAUDE.md`); in the log:
- `Blast at the crates: A -> B objects (T terrain voxels, I island(s), V object voxels in N object(s))`; since stage 2
  `7 -> 7 objects (0 terrain voxels, 0 island(s), 558 object voxels in 5 object(s))`. `Beam cut: N voxels removed, 1
  new object(s)`.
- `Player feet at (x, y, z), on the ground`, logged by `main` after `Run`. It must not say `in the air` (run without
  `--camera`), and x / z must differ from the spawn point.
- `Physics: B bodies (A awake, F frozen), T terrain colliders; scene: V voxel objects, R rubble`: R at or below
  `maxDebris` (192); a large F near the plaza means missing terrain colliders.
- `World: ... store used/capacity`: used below capacity.
- None of: `Scene: could not create the body ...`, `... could not be baked (chunk store full)`,
  `Scene: removed N object(s) that fell out of the world` (exhausted budget, or bodies falling through terrain).
- Exit code 0. With `--editor`, `Editor: N entities, last action '...'` appears only if the run reaches frame 160, and
  must not end in `(failed)`.

Screenshot without `--camera`: third-person view from behind the figure on the plaza; figure visible and upright,
crates or their wreckage ahead, nothing hanging in the air, no flash left by frame 200. Windowed behaviour (mouse
look, jump feel, editor focus) cannot be checked headless: say so.
