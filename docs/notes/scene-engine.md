# Notes: scene, engine loop, game (`src/scene`, `src/engine`, `tests/engine`, game `src/main.cpp`)

Read `CLAUDE.md` and `docs/ARCHITECTURE.md` section 7 first; nothing from them is repeated here. Line numbers are as
of engine commit `ec073a8` / game commit `ac17bcf`. Paths are relative to `engine/`; `main.cpp` always means the
game's `src/main.cpp`, one directory up.

## 1. Map

- `src/scene/Components.cppm` - `Entity`, `kNoEntity`, user-data packing (`ToUserData`, `EntityFromUserData`,
  `kEntityUserDataTag`), the seven plain components.
- `src/scene/Scene.cppm` - `SceneConfig` (all piece / rubble thresholds), `VoxelObjectDesc`, `LightDesc`,
  `DamageResult`, the `Scene` interface. Its doc comments are the contract.
- `src/scene/internal/Scene.cpp` - `Impl::CreateBody` (layer, CCD, density), `Crumble`, `DespawnDebris`,
  `UpdateDebris`, `Damage`, `DestroyBelow`, the gather functions.
- `src/engine/Engine.cppm` - `EngineConfig`, `Application` (5 callbacks), `Engine` accessors, `TerrainReady`,
  `StepAlpha`, `Explode`.
- `src/engine/internal/Engine.cpp` - `ApplyCommandLine`, `Impl` (member order), `Initialize`, `Explode`, `Run` (the
  only frame loop), exit statistics, exit code.
- `tests/engine/SceneTests.cpp` - suites `Scene`, `SceneDamage`, `SceneDebris`; `Fixture` (:47) builds store +
  materials + physics + particles + scene without a GPU.
- `tests/engine/EditorTests.cpp` - editor logic (not covered here); it uses the scene.
- `main.cpp` - `ShredborneGame`: playground, player controller, headless self-test, `--camera` / `--editor` parsing.
- `Engine` has no unit test (needs a device): `Run`, `Explode` and the game are checked only by the two headless runs.

## 2. Invariants

Ownership and release
- A body belongs to the `RigidBody` component. The only release path is the `on_destroy<RigidBody>` hook
  (Scene.cpp:28-33). It fires for `destroy`, `remove<RigidBody>`, `clear`; it does not fire for `emplace_or_replace`,
  `replace`, `patch` or assigning `.body` - those leak the old body.
- Chunk slots belong to `voxel::VoxelModel`, shared through `VoxelBody::model`. Any other holder of the pointer (the
  game's `m_Crate`, main.cpp:448; a test local) keeps the slots in use.
- A baked model points at its store (`Model.cppm:62`) and must die before it. The game relies on `ShredborneGame game`
  being declared after `Engine engine` (main.cpp:478-480).
- `entt::registry`'s destructor fires no signals, so `~Scene` calls `Clear()` (Scene.cpp:152). `Engine::~Engine`
  (Engine.cpp:77-88): device idle, `scene.Clear()`, terrain, world, ui, renderer, device, window,
  `physics.Shutdown()`. `Impl` declares `scene` after `store`, `physics`, `particles` (Engine.cpp:54-60) because it is
  constructed from references to them.
- Failure leaks nothing: `CreateVoxelObject` returns `kNoEntity` before creating the entity if the bake fails and
  destroys the entity if the body fails (Scene.cpp:185-198).

Entity ids
- 32 bits: 20 index + 12 version (Components.cppm:20). Body user data = tag "VXSE" in the high half, entity in the low
  half. Entity 0 is valid; terrain colliders use user data 0.
- Every `Scene` function accepts `kNoEntity` and stale ids (`Scene.StaleAndNullEntitiesAreSafeEverywhere`); a new
  public function starts with `registry.valid(entity)`.
- Every voxel object has `Transform::scale == SceneConfig::voxelSize`, and `Engine` overwrites that with
  `world.voxelSize` (Engine.cpp:43-47). `Damage` and `CreateBody` read the scale as the voxel size; do not write it.
- Never keep component pointers across a call that creates or destroys entities or components; `Damage` copies
  everything out first (Scene.cpp:301-318).

Threading: scene, engine and all `Application` callbacks are main-thread only (Scene.cppm:122), nothing here locks;
jobs run inside `World::Update` / `TerrainColliders::Update` - never touch the scene from a job.

Order within a frame (`Engine::Run`, Engine.cpp:195-283)
1. `PollEvents` (windowed) or `input.NewFrame()` (headless); `ui.BeginFrame`, `UpdateInputCapture`.
2. `world->Update(camera.position)`; `terrain->Update({camera}, colliderRadius)`.
3. `scene.BeginFrame()` once (previous = current). ARCHITECTURE 7.1 lists it before step 2; the code has it here,
   which makes no difference since streaming does not touch transforms.
4. Up to 4 ticks: `OnFixedUpdate(fixedDt)` -> `terrain->GuardBodies()` -> `physics.Step` -> `scene.SyncFromPhysics()`.
   After 4 ticks the accumulator is zeroed (time is dropped).
5. `OnUpdate(dt)`, `particles.Update`, `OnGui`, `ui.EndFrame`.
6. Gather instances / lights / particles, `RenderFrame`.
7. `scene.SetViewer(camera)`, `scene.Update(dt)`, `scene.DestroyBelow(windowOrigin.y - fallOutDepth)`.
- `GuardBodies` stays between game code and `Step`: bodies made in `OnFixedUpdate`, or in the previous frame's
  `OnUpdate` / `Explode`, are checked before they are simulated.
- `scene.Update` runs after rendering so a flash is drawn once at full intensity. ImGui calls are legal only in
  `OnGui` (between `ui.BeginFrame` and `ui.EndFrame`).

Time
- Headless: `dt == fixedDt` (Engine.cpp:206), so one tick per frame and `StepAlpha() == 0`. Headless frame N is tick
  N; the self-test script relies on it.
- Windowed: `dt` clamped to 0.1 s, 0..4 ticks per frame. `OnFixedUpdate` gets `fixedDt` (1/60); `OnUpdate` and
  `Scene::Update` get the frame `dt`.

Units, spaces, ranges
- Scene API is world metres. A voxel object's `Transform::current.position` is the model's minimum corner, `scale` the
  voxel size; the body's position is that same corner (Scene.cpp:51), not the centre of mass.
- Transforms are float. `LightDesc::position` and `Damage`'s `center` are `dvec3` but are cast to `vec3`
  (Scene.cpp:212, 324), as in `Explode`: far from the origin precision is float precision.
- `Explode(center, radius, power)`: metres, metres, material hardness. `power` applies to terrain only
  (`World::CarveSphere`); `Damage` ignores hardness, respecting only `MaterialFlag_Indestructible` (Scene.cpp:341).
- `Damage` does nothing for `radius <= 0` or NaN (Scene.cpp:299). `debrisParticleStride` is a divisor, clamped to >= 1
  (Scene.cpp:25). Density is clamped to >= 1 (Scene.cpp:45).
- CCD only for Dynamic bodies that are `Layer::Debris` (< `debrisBodyVoxels`) or whose smallest solid extent is <
  `thinBodyVoxels` (Scene.cpp:55-66). `SolidBounds` is `[min, max)`, so the extent is the solid size in voxels.

## 3. Traps

Scene
- During `Damage` the entity briefly has no `RigidBody` and a null `VoxelBody::model` (Scene.cpp:367-417).
  `GatherInstances` and `DespawnDebris` check for null; new readers must too.
- Old model released before pieces are baked (Scene.cpp:377-381): do not reorder
  (`SceneDamage.WorksInAFullChunkStore`, one-slot store).
- Old body removed before piece bodies are created, so the kept piece always finds a free body
  (`SceneDamage.PieceWithoutABodyIsDropped`, `maxBodies = 1`). Velocities are sampled before that (Scene.cpp:351-365);
  sampling later reads a dead body.
- The result of `pieceModel->Bake` is ignored on purpose (Scene.cpp:399-401): shared old model + full store gives a
  piece that is simulated but not drawn (untested). `DamageResult::destroyed` can be true with non-empty `pieces`.
- A Static / Kinematic object cut apart becomes Dynamic as a whole; the kept entity's `RigidBody::motion` changes.
- Rubble marking is asymmetric by design: new small pieces always get `Debris`; the damaged entity only if small,
  Dynamic and `debrisRemnants`; an existing `Debris` keeps its age
  (`SceneDebris.RemnantOfADamagedObjectBecomesRubbleOnceItIsSmall`).
- `VoxelObjectDesc::debris` only acts with `withBody` and `motion == Dynamic` (Scene.cpp:176). Below 16 voxels it
  returns `kNoEntity`, which is not an error: do not log or assert on it.
- A model with no solid voxels and `withBody = true` gives an entity without `RigidBody` (Scene.cpp:191).
- A body frozen by the terrain guard reports zero velocities (PhysicsWorld.cppm:73), and `GetPointVelocity`
  (PhysicsWorld.cpp:888) asks the Jolt body, which is static while frozen. So pieces of a frozen object start at
  rest and are not frozen themselves; the flight velocity remembered for the original is lost (no test covers it).
- `Update` handles `Lifetime` before rubble: an entity with both leaves no particle puff; invisible rubble leaves none
  either (Scene.cpp:98).
- The cap counts only pieces that did not expire this frame; ties break by entity id (Scene.cpp:129-131) so headless
  runs stay reproducible. A `Debris` entity without a body counts as resting for the far rule (Scene.cpp:120).
- `DestroyBelow` (world metres, whole AABB below) takes only Dynamic bodies that are awake, or frozen with
  `wantsToMove`; an impulse or `ActivateInBox` on a parked body makes it eligible. NaN position or bounds always go.
- `EntityCount()` uses `storage<entt::entity>()->free_list()` (Scene.cpp:236-241): EnTT 3.15 internals
  (`cmake/Dependencies.cmake`); re-check on an upgrade.
- 12 version bits: EnTT skips version 0xFFF and wraps (`entt/entity/entity.hpp`, `next`), so after 4095 reuses of an
  index a stale id is valid again. Nothing in the scene guards against it; rubble churn is how it would happen.

Engine
- `Explode` order (Engine.cpp:125-183): carve terrain -> particles -> islands -> snapshot (`QueryBodies`) -> `Damage`
  each -> impulses -> `ActivateInBox` -> flash. The snapshot is taken after the islands exist, so a fresh island whose
  bounds touch the blast box is damaged as well, without a hardness check. Pieces made by `Damage` are not in the
  snapshot.
- `ExtractFloatingIslands` removes the island from the world (`removeFromWorld = true`). If `CreateVoxelObject` then
  fails (store full, no body) that terrain is simply gone; only the scene's error line shows it.
- Impulse distance is measured to `BodyState::position` = the model corner, so big objects are pushed off-centre.
  Impulse = mass x 6 x linear falloff over 3 x radius. The loop visits Static and Kinematic bodies too: `GetMass`
  returns their shape mass, but Jolt's `AddImpulse` ignores bodies that are not dynamic, so nothing happens. A frozen
  body keeps the push as velocity for when it resumes (PhysicsWorld.cpp:725-743).
- The impulse loop runs inside a registry view (Engine.cpp:161-169): only physics calls belong there.
- Budgets: 400 to 799 terrain debris bursts per blast (stride = removed / 400, rounded down, Engine.cpp:136),
  islands up to 60 000 voxels, search box radius + 24 voxels. Flash lifetime `ln(40*radius / 0.05) / 9` s (0.7 s
  for 0.7 m). Call `Explode` from `OnUpdate` / `OnFixedUpdate`, never inside a registry view.
- Before `Initialize()` succeeds `World()` and `Explode` dereference a null `world`; the other accessors return
  subsystems that exist but are not initialised. Only `TerrainReady` / `TerrainStats` check. `Run` returns 1 if not
  initialised, 2 on validation errors, else 0; `OnInit` is called at the start of `Run`, not by `Initialize`.
- `ApplyCommandLine` also takes `--no-checkerboard` (Engine.cpp:35), missing from the comment at `Engine.cppm:51`.
  `--camera` and `--editor` are game flags (main.cpp:481-489).

Game (`main.cpp`)
- The player is a physics character (`m_Character`); the figure `m_Body` has `withBody = false` (:422), so blasts,
  `Damage` and `DestroyBelow` never affect it. It is placed every frame with `teleport = true` (:205) and so has no
  motion vectors; that is the use `SetTransform`'s comment names for `teleport` (Scene.cppm:206-208).
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
2. Call it from `Engine::Run`: per tick next to `SyncFromPhysics`, per frame next to `scene.Update`. Update the
   protocol comment (`Scene.cppm:124-133`) and ARCHITECTURE 7.1.
3. Decide what `Damage` does with it: components stay on the kept piece; new pieces get only Transform, VoxelBody,
   RigidBody, Name and maybe Debris (Scene.cpp:422-427).
4. Test with `Fixture`; extend `StaleAndNullEntitiesAreSafeEverywhere` for a new public function.

Change a piece / rubble threshold or rule
1. `SceneConfig` default and comment (`Scene.cppm:27-59`); the table in ARCHITECTURE 7.3.
2. Logic: `Impl::CreateBody`, `IsSmall`, `CreateVoxelObject` (Scene.cpp:176-205), `Damage` (:398, 419, 426).
3. All of `SceneDebris` (`DefaultsKeepTinyFragmentsOutOfTheScene` pins defaults); headless: compare `M rubble`.

Change what `Damage` does to pieces
1. Loop at Scene.cpp:390-429. Keep "release old model, then bake" and "sample velocities, then remove the body".
   Update the `Damage` comment (`Scene.cppm:231-259`) and `DamageResult`.
2. Tests: `CutBeamBecomesTwoEntitiesThatInheritMotion`, `CutRotatedBeamKeepsEveryVoxelInPlace`,
   `PiecesOfASpinningObjectKeepSpinningAndMotionVectors`, `DentedStaticObjectStaysStatic`,
   `StaticObjectCutApartBecomesDynamic`, `SharedModelIsNeverModified`. The `CollectVoxels` helper (SceneTests.cpp:85)
   proves no voxel moved or was duplicated.
3. Headless run: `Beam cut: ... 1 new object(s)` must not change.

Add an effect to `Explode`
1. Insert at the right stage (order above). Spawned entities need a `Lifetime` or `debris = true` (snapshot rule).
2. `Explode` has no unit test: put the logic into `Scene` with a test, keep `Explode` as glue.
3. Headless run (`Blast at the crates: A -> B objects`) and a `--camera` screenshot of the plaza.

Add an `Application` callback or move a loop step
1. Virtual with an empty default (`Engine.cppm:59-67`), call site in `Run`, keeping the order rules of section 2.
2. Update `Scene.cppm:124-133` and ARCHITECTURE 7.1; both headless runs.

Change the player controller or the self-test script
1. Simulation in `OnFixedUpdate` (uses `fixedDt`), intent and camera in `OnUpdate`.
2. Constants: eye 1.62, height 1.8, radius 0.3 m (main.cpp:444-446); walk 3.8, run 7.0, jump 4.6 m/s; figure 6x18x4
   voxels, centred by the (3, 0, 2) voxel offset (:204).
3. Script steps hang on `m_HeadlessFrame` (blast and beam at 90, :151) and the `ExerciseEditor` cases. A step past
   frame 200 (120 with `--editor`) never runs with the documented commands: change `CLAUDE.md` in the same commit.
   Keep the log lines the checks look for: `Blast at the crates`, `Beam cut`, `Player feet`.

## 5. Verifying a change here

Unit tests, no GPU: `./build/engine/tests/VortexEngineTests --gtest_filter='Scene*'` (40 tests in `SceneTests.cpp`),
then the whole binary because `EditorTests.cpp` uses the scene.
- Release: `Scene.CreateAndDestroyReleaseBodyAndChunkSlots`, `RegistryOperationsReleaseBodiesToo`,
  `DestructorReleasesEverything`, `CreationFailuresReturnNoEntityAndLeakNothing`. They assert `physics.BodyCount()`
  and `store.UsedCount()` return to 0; put the same two asserts in every new test that creates objects.
- Ids: `BodiesMapBackToEntities`, `SceneDamage.SnapshotThenDamageHitsEveryObjectOnce`. Frame protocol:
  `PhysicsDrivesTransformsAndPreviousIsLastFrame`, `LifetimeExpiresAndLightsFade`,
  `DestroyBelowRemovesWhatFellOutOfTheWorld`. `SceneDamage.*` (14) and `SceneDebris.*` (9): see the recipes.

Headless game run (commands in `CLAUDE.md`); in the log:
- `Blast at the crates: A -> B objects`; `Beam cut: N voxels removed, 1 new object(s)`.
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
