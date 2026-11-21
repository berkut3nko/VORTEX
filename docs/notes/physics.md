# Physics module notes (`vortex.physics`)

[[../../../00 Docs Map|Docs map]] · [[../ARCHITECTURE|architecture]] · [[../design/collapse|collapse]] · [[../design/shred|shred]]

Companion to `CLAUDE.md` and `docs/ARCHITECTURE.md` section 6 / 6.1 (design, not repeated here).
Line numbers are as of engine commit `ec073a8` (the `engine/` repository; outer repository `ac17bcf`);
search for the symbol if they have drifted.
`PW.cpp` = `src/physics/internal/PhysicsWorld.cpp`, `T.cpp` = `src/physics/internal/Terrain.cpp`.

## 1. Map

- `src/physics/Physics.cppm` - primary module unit; only re-exports the partitions `:world` and `:terrain`.
- `src/physics/PhysicsWorld.cppm` (`:world`) - the whole public surface over Jolt: `BodyId`, `BodyDesc`,
  `BodyState`, `DynamicBodyInfo`, `RayHit`, `ContactEvent`, `Character*`, `PhysicsConfig`, `PhysicsWorld`.
  No Jolt type or header appears here; keep it that way.
- `src/physics/Terrain.cppm` (`:terrain`) - `TerrainColliders` (collider streaming + body guard), `TerrainColliderStats`.
- `PW.cpp` - the only file that includes Jolt. Process-wide runtime ref-count (`AcquireRuntime`, `RuntimeRef`,
  PW.cpp:70-106), glm<->Jolt conversions, layer filters (:146-201), `ContactCollector` (:209), `Shape`
  wrapper (:318), `PhysicsWorld::Impl` with `Simulation` and the `movables` map (:334-409), all methods.
- `T.cpp` - `Snapshot`/`CookJob`/`Shared` (worker side, :63-124), `TerrainColliders::Impl` (`Entry` per
  tracked chunk, `Track`/`Touch`/`NeighbourChanged`/`Scan`/`ChunkReady`/`BoxReady`/`Require`/`GuardBodies`/
  `Apply`/`InstallResults`/`ProcessDirty`), public `Update`/`GuardBodies`/`IsReady`/`Flush`/`Stats`.
- `tests/physics/PhysicsTests.cpp` - suite `PhysicsWorld` (bodies, shapes, rays, contacts, characters), no voxel world.
- `tests/physics/TerrainColliderTests.cpp` - suite `TerrainColliders` (streaming, edits, focus, unload, character on terrain).
- `tests/physics/TerrainGuardTests.cpp` - suites `TerrainGuard` (freeze/park/resume, re-cook rules) and
  `FrozenBodies` (`SetFrozen` alone, fixture `Bare`).
- The guard as `sim::Simulation` calls it is pinned in `tests/sim/SimulationTests.cpp`:
  `StepFreezesABodyWhoseGroundHasNoColliderYet` (a tick freezes a crate with no collider under it, then it resumes
  and lands) and `StepPhysicsGuardsWhatGameCodeCreatedAfterTheStreamingPhase` (fails if `SimulateBodies` stops
  calling `GuardBodies`: the guard inside `TerrainColliders::Update` does not see a body made after it).
- All three build into one executable, `VortexPhysicsTests` (`tests/CMakeLists.txt:31`, sources globbed).
- Callers outside the module (since M1 stage 1 the physics world and the terrain colliders belong to `sim::Simulation`):
  `src/sim/internal/Simulation.cpp:50-60` (streaming, guard + step + sync), `:108-115` (teardown);
  `src/engine/internal/Engine.cpp:203-230` (frame order: which phase is called when), `:279` (log line); the game's character in `src/main.cpp:86-99`. `scene` owns entity bodies.

## 2. Invariants

### Ownership and lifetime
- Jolt globals (allocator, `Factory`, registered types) are ref-counted by `g_RuntimeUsers` under
  `g_RuntimeMutex`. Users are every `Simulation` and every `Shape`; each `Create*Shape` also holds a
  temporary `RuntimeRef`. A `ShapeRef` may outlive its `PhysicsWorld` (test `WorldsAndShapesShareTheRuntime`).
- `RuntimeRef` must be the first member of `Shape` (PW.cpp:324) and of `Simulation` (:346): members die in
  reverse order, so Jolt stays usable until the Jolt objects are gone. Do not reorder those members.
  `Simulation` order also matters: `characters` is declared after `system` so characters die first.
- Jolt runs on its own thread pool (`JPH::JobSystemThreadPool` inside `Simulation`, `PhysicsConfig::threads`,
  0 = hardware - 1 clamped to 1..8), not on the engine `JobSystem`. Only collider cooking uses the latter.
- `Shutdown()` destroys every body and character and clears `movables`/`frozenCount`/events (PW.cpp:448).
  `sim::Simulation::Shutdown` resets `terrain` before `physics.Shutdown()` (Simulation.cpp:108-115); the scene must be cleared first
  (ARCHITECTURE "Destruction order"). Every `BodyId`/`CharacterId` is dead after `Shutdown()`; a new
  `Initialize()` builds a fresh `Simulation` whose ids start over, so an id kept across it can match a
  different body or character. Drop all ids on shutdown.
- `TerrainColliders` holds references to `PhysicsWorld`, `voxel::World`, `JobSystem`, `MaterialLibrary`;
  all must outlive it. Its destructor takes `Shared::lifetime` exclusively, sets `cancelled`, then destroys
  its bodies (T.cpp:538). Jobs still queued afterwards only hold the `shared_ptr<Shared>` and return early.
- A terrain collider body belongs to its `Entry` (`RemoveBody`, T.cpp:376). `colliderCount` is adjusted only
  there and in `Apply`.

### Threading
- Everything is main-thread only except `CreateCompoundShape/MeshShape/BoxShape/SphereShape`. Those read
  only `Impl::initialized` (atomic, PW.cpp:371) and never touch `sim`.
- `ContactCollector::OnContactAdded` runs on Jolt worker threads during `Step` with the two bodies locked:
  it may read only those two bodies and append under `m_Mutex` (PW.cpp:213-239).
- `CookJob` (worker) touches only its `Snapshot`, the immutable `CollisionFilter`, `physics->CreateMeshShape`
  and `Shared::results` under `Shared::mutex`. It never reads `voxel::World` (main-thread only): everything
  it needs is copied in `TakeSnapshot` on the main thread (T.cpp:450).
- `Shared::inFlight` is incremented on the main thread before `Submit` (T.cpp:523) and decremented by a
  `ScopeExit` declared first in `CookJob` (:106), i.e. after the result was published. `Flush` depends on
  exactly that order ("read the job counter first", T.cpp:625).
- `UpdateCharacter`, `CreateCharacter`, `SetCharacterPosition` use `sim->tempAllocator`, the same allocator
  `Step` uses: never call them concurrently with `Step`.

### Order within a frame (Engine.cpp:203-230, Simulation.cpp:50-60)
`Engine::Run` reaches these calls through `sim::Simulation`: `UpdateStreaming(camera, {camera})` is the first two,
`StepPhysics()` is the bracket without `OnFixedUpdate`. `Simulation::Step` (a server tick) runs the same calls once
each, with the window held in place and no `OnFixedUpdate`; there `terrain.Update` runs once per tick, so everything
counted in `Update()` calls (`kBodyKeepUpdates`, the scan) is counted in ticks on a server and in frames in the engine.
`world.Update` -> `terrain.Update` (consume changed chunks, scan, drop, `InstallResults`, `GuardBodies`,
`ProcessDirty`) -> up to 4 x [`OnFixedUpdate`, `terrain.GuardBodies`, `physics.Step(fixedDt)`,
`scene.SyncFromPhysics`]. Inside `Update` the order install -> guard -> process is deliberate (T.cpp:600-604):
bodies resume on what was just installed, and their requests are cooked in the same call.
`GuardBodies` must run after any code that created/moved bodies and before `Step`.

### Units and spaces
- World metres, single precision. `BodyDesc::position` / `BodyState::position` are the **shape origin**, not
  the centre of mass (PW.cpp:683; test `BodyPositionIsTheShapeOrigin`).
- Character positions are **feet** positions; the capsule is offset up by `height/2` (PW.cpp:1014).
- Impulses kg*m/s, density kg/m^3 (min 1, `kMinDensity`), half extents >= 1e-4 m (`kMinHalfExtent`).
- Terrain chunk coordinates in `T.cpp` are absolute world chunk coordinates (not window-relative). A collider
  body sits at `ChunkBounds(coord).min` with mesh vertices in chunk-local metres (voxel units * `voxelSize`,
  T.cpp:118). Focus points are `dvec3`; everything handed to Jolt is `float`.
- Terrain colliders have `userData == 0`, `Layer::Static`, `MotionType::Static` (T.cpp:389-394).

### Ids
- `BodyId::value` is Jolt's index+sequence number verbatim; `0xFFFFFFFF` is invalid; ids with bit 31 set are
  rejected (`Find`, PW.cpp:377-384). Every public method goes through `Find` and is a no-op / default result
  for stale ids - keep that for any new method. How many sequence values Jolt cycles through before an index
  can alias an old id is unverified (Jolt internal): do not keep ids of destroyed bodies around.
- `CharacterId` = 12-bit generation << 20 | 20-bit slot index (PW.cpp:306-308). Generations run 0..0xFFE so
  the id is never `0xFFFFFFFF`; a slot's generation wraps after 4095 destroy cycles, after which a very old
  id can match again. Slots are reused LIFO from `freeCharacters`.
- `movables` (PW.cpp:369) holds exactly the bodies whose engine-level motion type is Dynamic, frozen ones
  included. `frozenCount` == number of entries with `frozen`. Every path that creates, destroys or changes
  motion type must keep both right: `CreateBody` (:661), `DestroyBody` -> `Forget`, `SetMotionType` (:751-780),
  `SetFrozen` (:783). `GetDynamicBodies` and the guard see nothing else.

### Value handling
- All inputs are validated for finiteness; bad values are logged and ignored, never asserted
  (`CreateBody` :625, `SetTransform`, `SetVelocity`, `AddImpulse`, `Raycast`, `Step`, `Update` T.cpp:554).
  Quaternions are normalised on the way into Jolt; only non-finite or near-zero ones become identity
  (`ToJolt(quat)`, PW.cpp:127). `BodyDesc` material values are clamped, not rejected (:637-641).
- `Step` ignores `dt <= 0`; it runs one collision step. Jolt update errors are logged once per kind (:465);
  the once-only flags (also "out of bodies") are reset only by `Initialize`.

### Terrain entry state machine (`Entry`, T.cpp:128)
- `generation` = newest request (bumped by every `MarkDirty`, from the global `nextGeneration`);
  `installed` = generation of what is installed; `exposed` = generation at which a neighbour uncovered faces.
  Ready means `built && installed >= exposed` and the chunk is loaded (`ChunkReady`, :303).
- A result is installed iff `result.generation > entry.installed` (:470). `Track` sets
  `installed = nextGeneration` so results of an earlier, dropped entry for the same chunk are rejected (:226).
- `border`/`hasBorder` mirror the neighbour bits the newest job was given; updated in `ProcessDirty` (:521)
  and `NeighbourChanged` (:261).
- `dirty` may contain duplicates and coordinates of dropped entries; `queued` is the truth (:505).
- At most `maxJobs = max(2, 2 * jobs.ThreadCount())` cook jobs per **round** (review of networking stage 6): a round is
  an `Update` together with the tick's `GuardBodies` (both call `ProcessDirty`), or one pass of `Flush`; its budget is
  what was in flight when it began (`StartRound`) plus what it started - never the live counter re-read while it
  submits, which let a timing-dependent number start (the tick's second batch from `GuardBodies` saw jobs of the first
  finish or not). The rest stay queued for a later `ProcessDirty`. Order: chunks a body asked for lately first, then nearest to a focus
  point (:485-496). A chunk that needs no collider is settled in `ProcessDirty` itself, without a job (:508).
- An untracked chunk is ready only if `NeedsNoCollider` (:306). So a loaded chunk with a surface but outside
  every focus radius is "not ready", which is exactly what parks the bodies resting there.
- A change of material collidability (`MaterialLibrary::Revision`, `RefreshFilter`, :200) re-queues every
  tracked chunk. Liquids never collide (`MaterialFlag_Liquid` is the exclusion passed to the filter).

## 3. Traps

- **Freezing = Jolt static, object layer kept** (PW.cpp:800-802). That is why other bodies rest on a frozen
  body (test `OtherBodiesRestOnAFrozenBody`). Do not "tidy" this by moving frozen bodies to `Layer::Static`:
  Static-vs-Static never collides, and release (:808-812) does not touch the layer, so it would stay Static.
  Jolt cannot activate a static body, so `ActivateInBox` reaches frozen bodies through its own loop over
  `movables` (:857-866), which only sets `wake`; `Activate`, `SetTransform(activate)`, `AddImpulse`,
  non-zero `SetVelocity` and `SetMotionType(Dynamic)` do the same.
- **The guard owns the frozen flag.** `GuardBodies` (T.cpp:372) sets it from terrain readiness for every
  dynamic body on every call, so a `SetFrozen` made by game or scene code is undone at the next guard
  (released if the terrain is ready, and vice versa). Nothing outside `Terrain.cpp` calls `SetFrozen` today;
  a "pause this body" feature needs its own mechanism (or a flag the guard respects), not `SetFrozen`.
- **`GetState` of a frozen body reports zero velocity** (read from the static Jolt body); the kept velocity
  is in `Movable`, `DynamicBodyInfo::linearVelocity` (the guard reads that) and, since shred.md stage 1d,
  `BodyState::keptLinearVelocity` (logs and tests: "frozen, velocity kept").
- **Release restores velocities only if `wake`** (PW.cpp:811-812). A body frozen asleep resumes asleep with
  zero velocity; that is the "parked rubble costs nothing" behaviour, not a bug.
- **`AddImpulse` on a frozen body drops the spin** of an off-centre impulse and converts to linear velocity
  via `GetMass` (PW.cpp:732-738). `SetVelocity` on a frozen body replaces the kept velocities;
  `SetVelocity(0,0)` does not mark it as wanting to move. `SetShape` on a frozen body does not set `wake`.
  `SetMotionType(Static/Kinematic)` on a frozen body forgets it (kept velocities are lost).
- **`SetShape` on a frozen dynamic body with a mesh shape passes the "must be static" check** (it tests the
  Jolt body, which is static while frozen, PW.cpp:827-834). Nothing calls it that way today; releasing such a
  body is unverified and probably invalid. Check `movables` there if this path ever becomes reachable.
- **`SetMotionType(Dynamic)` moves a `Layer::Static` body to `Layer::Dynamic`** (PW.cpp:777) and it is never
  moved back when made static again. Deliberate: a static-layer body would fall through terrain. Kinematic
  targets keep their layer. Mesh-shaped bodies are created without motion properties (:633) and can never
  leave Static (:767).
- **Contact events**: at most one per body pair per `Step` (the hardest), sorted by ids; slower than
  `contactEventMinSpeed` dropped; `!(speed >= min)` also drops NaN (PW.cpp:226). Only `OnContactAdded` is
  implemented: contacts that persist or end produce nothing. The speed is signed (body 1 approaching body 2
  along the 1->2 normal is positive), so separating contacts are dropped too. The queue is capped at 16384
  and then warns once (the per-step buffer has the same cap, silently, :238). **Nothing in the engine, scene or game calls `TakeContactEvents` today** (grep), so in
  a long session the queue fills and stays full; whoever adds impact effects must drain it every frame.
  Events can name bodies destroyed since: look ids up, do not assume validity. Which of the two bodies Jolt
  reports as A is unverified.
- **Characters are not bodies**: not stepped by `Step`, not in `GetDynamicBodies`, not guarded, not hit by
  `Raycast`, no contact events. The game must call `UpdateCharacter` once per fixed step and hold the
  character while its terrain has no collider: since networking stage 4 both go through `sim::StepPlayer`, whose
  gate is `TerrainColliders::HasCollider(capsule bounds)` (section "Players in a session").
- **Character capsule is shrunk by Jolt's `mCharacterPadding`** (PW.cpp:1013) so shape + padding equals the
  requested capsule and the feet sit on the ground. Changing radius maths without this lifts or sinks the player.
- **`UpdateCharacter` has no horizontal inertia**: velocity is rebuilt each call from ground velocity or the
  current vertical speed, plus gravity, plus the wanted x/z (PW.cpp:1069-1079); `desiredVelocity.y` is ignored.
  A jump starts only when `OnGround` and moving towards the ground (:1067-1073).
- **Characters ignore `Layer::Character` and `Layer::Rubble` bodies** (`CharacterLayerFilter`), whatever
  `characterLayers` says, so a kinematic proxy body following the controller, a session client's own large rubble
  and anything replicated that moves never block it (networking stage 6; the first stage-6 commit left Rubble to the
  mask and `RubbleLayerMeetsTheWorldBodiesAndItselfButNoCharacter` failed). The virtual character pushes light dynamic
  bodies (Jolt's `mMaxStrength`, 100 N: a 5 kg box moves, a 51 kg crate on the ground does not) and weighs on the one
  it stands on; `PhysicsConfig::characterMovesBodies = false` (a session) does neither: a listener sets
  `mCanReceiveImpulses = false` on every contact and the character's mass is 0
  (`CharactersThatMoveNoBodiesNeitherPushNorWeigh`).
- **`Raycast` normal**: falls back to `-dir` and is always flipped to face the ray origin (PW.cpp:923-935).
  It hits every layer, frozen bodies included; only sensors are skipped.
- **`TerrainColliders` must be the only consumer of `World::TakeChunkChanges()`** (`ConsumeChanges`; the
  coordinate-only `TakeChangedChunks` empties the same list). A second consumer silently starves one of them. The range scan runs only when a focus point moved a quarter chunk
  or the radius/point count changed (`NeedsScan`, :266), so a chunk that becomes loaded later gets its
  collider *only* because loading reports it as changed (`Touch(coord, true)`).
- **Radii**: create within `radius + chunk/4`, drop beyond `max(1.5*radius, radius + chunk/2)` (T.cpp:556-561).
  `radius <= 0` or no focus point means only body-requested colliders survive.
- **`GuardBodies` step 1 vs step 2** (T.cpp:349-374): only awake / wants-to-move bodies *request* colliders
  (0.75 s ahead, clamped to 2 chunks; kept `kBodyKeepUpdates` = 120 `Update()` calls, i.e. frames, not steps), but *every* dynamic body, asleep too, is frozen when its chunks
  (bounds + one voxel + 0.05 s of travel, clamped to 1 chunk) are not ready. Making sleeping bodies request
  colliders would bring back the cost the parking design removed.
- **Old collider stays until the new one is installed**: `Apply` removes and creates in one call (:384-401).
  Never remove a body in `MarkDirty`/`Touch`. `Apply` wakes bodies in the chunk box only on a *re*build
  where a body existed before or after (:404).
- **`NeedsNoCollider`** (:417): empty dense chunk, non-collidable uniform, or collidable uniform enclosed by
  six collidable uniform neighbours. It also returns true for a chunk that is not loaded, which is why
  `ChunkReady` tests `IsChunkLoaded` first (:304). Keep that order.
- `InstallResults` installs a round's results in chunk order (x, y, z, generation), not in the order the workers
  finished, and `Flush` waits for every job of a pass before it installs: collider bodies get their Jolt ids (which
  Jolt's contact order follows) the same way in every run that installs the same results together. With both, a
  harness that waits for idle between ticks is reproducible bit for bit (`Entities.OneScriptGivesTheSameBodiesTwice`).
- **`Flush` spins with 1 ms sleeps** until no job, result or dirty entry remains, re-guarding in between
  (:619-640). It does not call `Update`, so it does not consume changed chunks or scan: call `Update` first.
  It blocks for as long as the `JobSystem` workers are busy (a `JobSystem` always has at least one thread).
  Tests and loading screens only; never in the frame loop.
- Cost to keep in mind: `GuardBodies` is O(dynamic bodies) with a sort, once per `Update` plus once per fixed
  step (the public one also runs `ProcessDirty`, T.cpp:607-610); `ActivateInBox` walks all `movables` while anything is frozen (PW.cpp:857-866). No measurement of
  either exists in this module (estimate: fine for hundreds of bodies, unverified beyond the 60-body test).
- `SetShape`, `SetMotionType`, `GetActiveBodies` and `TakeContactEvents` have no caller in `src/scene`,
  `src/engine` or the game today (grep): they are covered by tests only, so a regression there shows up in
  `VortexPhysicsTests`, never in a headless game run.
- No `TODO`/`FIXME` markers exist in `src/physics`.

## 4. Recipes

**A. Add a `PhysicsWorld` method on a body.** Declare in `PhysicsWorld.cppm` with a doc comment; implement
in `PW.cpp` starting with `const auto id = m_Impl->Find(body); if (!id) return ...;`, validate finiteness,
then decide explicitly what it does for a frozen body (`m_Impl->FindFrozen(body)`): anything that would
wake or push must set `held->wake` / adjust `held->linear` instead of calling Jolt. Extend
`StaleAndInvalidIdsAreHarmless` (PhysicsTests.cpp:255, loops over bad ids) and add a `FrozenBodies` case.

**B. Add a collision layer.** Add the enumerator at the end of `Layer`; add `kLayer...`, bump `kLayerCount`
(PW.cpp:146-150); extend `LayerPairFilter`, and `BroadPhaseLayers`/`ObjectVsBroadPhaseFilter` only if it
is non-moving; decide `CharacterLayerFilter`. `ToJolt(Layer)` maps unknown values to Dynamic. Test next to
`DebrisIgnoresDebrisButRestsOnStatic` and `CharacterIgnoresCharacterLayerBodies`.

**C. Add a shape type.** New `Create*Shape` in the interface (const, thread safe): check
`m_Impl->initialized`, validate, take a local `RuntimeRef`, build, `WrapShape`. Touch nothing in `sim`.
Set a density so mass is positive. Tests: mass like `CompoundMassIsDensityTimesVolume`, origin like
`BodyPositionIsTheShapeOrigin`; if it must be static, extend `MeshShapesAreStaticOnly`.

**D. Change character behaviour (crouch, air control, pushing).** `CharacterDesc`/`CharacterSlot`
(PW.cpp:310) for per-character data, `CreateCharacter` for shape and settings, `UpdateCharacter` for
velocity rules. Keep feet-position semantics and the padding shrink. Tests: `CharacterStandsJumpsAndLands`,
`CharacterIsBlockedByBodies` (PhysicsTests.cpp:693-782) and `CharacterWalksAndClimbsSmallStep`
(TerrainColliderTests.cpp:363). The game side is `src/main.cpp:86-99`; the headless log line
`Player feet at ..., on the ground` must survive.

**E. Change guard tuning or readiness rules.** Constants at T.cpp:36-44; logic in `GuardBodies`, `ChunkReady`,
`BoxReady`, `ChunkRange`. If the numbers change, update ARCHITECTURE 6 / 6.1 (0.05 s, 0.75 s, 2 chunks, 120
updates, 1.5x) in the same commit. Run the whole `TerrainGuard` suite; the cases most sensitive to tuning are
`BodyFrozenInFlightKeepsItsVelocity`, `WokenParkedBodyBringsItsOwnCollidersBack`,
`ManyBodiesFarFromTheFocusAllLand`, `BodyStopsAtTheEdgeOfTheWorldWindow`.

**F. Change what a collider mesh contains or when it is re-cooked.** Mesh content is
`voxel::BuildChunkCollisionMesh` (voxel module) fed by `Snapshot`/`BorderBits`; anything new the job needs
must be copied into `Snapshot` on the main thread. Re-cook decisions are `Touch`/`NeighbourChanged`; keep the
`generation`/`installed`/`exposed` rules of section 2. Tests: `SurfaceOnChunkBoundaryCollides`,
`CarvingUnderRestingBoxMakesItFall`, `EditedChunkKeepsItsColliderUntilTheNewOneIsInstalled`,
`ChunkEditedEveryFrameStillGetsNewColliders`, `ChunkUncoveredByCarvingItsNeighbourIsNotReadyUntilRecooked`,
`EditAwayFromTheBorderRecooksOnlyTheEditedChunk`.

## 5. Verifying a change

- Build as in `CLAUDE.md`, then `./build/engine/tests/VortexPhysicsTests` (path from the project root) or
  `ctest --test-dir build -R "PhysicsWorld|TerrainColliders|TerrainGuard|FrozenBodies"`.
  Use `--gtest_filter=TerrainGuard.*` while iterating, then the full `ctest` run: `VortexEngineTests`
  exercises the scene on a real `PhysicsWorld` and breaks on `movables`/id mistakes.
- Terrain tests are timing-tolerant by construction (the fixtures wait with `jobs.WaitIdle()` or `Flush()`); a test
  that fails only sometimes means a real ordering bug (`inFlight`, generations), not flakiness to retry.
  Run a suspect test with `--gtest_repeat=50`.
- Headless game run (commands in `CLAUDE.md`): besides `Beam cut: ... 1 new object(s)` and
  `Player feet at ..., on the ground`, read the line
  `Physics: N bodies (A awake, F frozen), C terrain colliders; ...` (Engine.cpp:290). Expect `C > 0`, and `F`
  small or 0 once loading settles; a large or growing `F` means colliders are not being cooked or the
  readiness test regressed. Any `Jolt: ...` warning (from `JoltTrace`), `PhysicsWorld: ... is full` or
  `out of bodies` line is a failure to investigate, as is `more than 16384 contact events`.
- Screenshot: crates and debris must rest on the ground surface, not hang in the air (frozen forever) and
  not be missing (fell through; they are destroyed below the window by `scene.DestroyBelow`).
- Not checkable headless: feel of character movement, stairs and slopes in real play. Say so.

## View distance stage 1 (full-height window, Packed chunks)

- A `Packed` chunk (no slot, RLE bytes only) wants a collider like a Dense one (`WantsCollider`, `Require`);
  `TakeSnapshot` and `BorderBits` read its voxels through `World::ReadChunk` (a decoded copy; `BorderBits` keeps one
  scratch chunk, main thread). `NeedsNoCollider` is false for it. `GetChunk` returns nullptr and uniform id 0 for it,
  so `IsUniformCollidable` treats it as open (conservative: neighbours build their faces).
- `ChunkReady` treats a chunk above the top of a full-height window (x / z inside it) as ready: it is the sky over
  the build ceiling. Without that, a body thrown upwards froze in mid-air under the ceiling for good.

## Physics after destruction (shred.md stage 1d)

- **Stale part of a collider.** `ConsumeChanges` adds each tracked chunk's edit box (`World::ChunkChange`,
  world voxels) to `Entry::pendingEdit`. `ProcessDirty` moves it into `cookingEdits` tagged with the generation
  of the snapshot it starts (`StartCook`, also on the `NeedsNoCollider` path); `Apply` of a result of generation
  g drops every tag <= g (`Installed`). `BodyReady` (the guard's test) = `BoxReady` **and** no touched entry is
  stale where the body is (bounds + one voxel + 0.05 s of travel, in voxels). Keep the per-generation tags: a
  single union box cleared only when the newest result lands never clears on a chunk edited every frame, and
  then freezes every body near a held brush (`BodyBesideAChunkEditedEveryFrameIsNotHeld`).
- `IsReady` (`Engine::TerrainReady`; players use `HasCollider` since networking stage 4) is chunk-level only, unchanged: a player standing in the crater
  they shot is not held.
- The public `GuardBodies()` calls `ConsumeChanges` first: a blast in `OnFixedUpdate` is followed by the guard
  of the same tick before any streaming, and must already freeze the piece
  (`Rubble.DetachedPieceWaitsFrozenWithItsPushOverAStaleCollider` steps once without `UpdateStreaming`).
- **Wake on first build.** `modifiedUnseen` holds chunks edited while no up-to-date collider of them existed:
  edited while untracked (and loaded), or dropped in step 3 while `Entry::Stale()`. `Apply` on a first build
  (`!rebuilt`) erases the chunk and `ActivateInBox`es its bounds. Every guard pass keeps only the chunks a body
  frozen now lies on (`ChunkRange` of its bounds + one voxel; a body simulated since saw the new ground, one
  parked later parks on what it saw), so the set never outgrows the chunks under parked bodies (it used to be
  cleared only when no body at all was frozen: one parked crate let every later edit pile up).
  `TerrainColliderStats::unseenEdits` is its size (`UnseenEditsAreRememberedOnlyUnderFrozenBodies`). Do not
  prune by the world window: a chunk that unloaded under a parked body must stay remembered until it reloads. Tests: `ParkedBodyFallsWhenItsGroundWasCarvedJustBeforeTheColliderWasDropped`,
  `ParkedBodyOverAChunkCarvedAndUnloadedFallsAfterTheReload` (both resumed asleep in the air before).
- **What was and was not reproduced.** Before 1d a piece created over its stale collider was stepped at once
  (the pinned rule; `PieceOverAStaleColliderWaitsFrozenWithItsVelocity` and the sim test failed on
  `IsFrozen`). The lost push itself did **not** reproduce in isolation: a box or a blast-cut pillar stepped
  inside the stale collider kept its velocity (Jolt ignores the back faces it sits behind), and in today's
  game runs the piece was already frozen in its first step by a neighbour's `exposed` rule. The old
  measurement (view-distance.md "Stage 2", about 0 m/s at +1) came from an earlier engine. The rule stays:
  a body must not be simulated against geometry known to be gone.
- Cost: one `std::vector` per tracked entry (empty unless edits are in flight) and, per guarded body, the
  overlap test on the chunks `BoxReady` already visits. Not measured; it is a few comparisons per chunk.

## Players in a session (networking stage 4)

- `SetCharacterState(id, feet, velocity)` = `SetPosition` + `SetLinearVelocity` + `RefreshContacts` (the ground
  state comes from the refresh, not from the history). Replaying the same 60 steps from a restored state on a slab
  gives the same positions (`CharacterStateIsRestoredAndReplaysClose`, under 1 mm asserted, 0 measured); over voxel
  terrain two simulations with 1 and 3 workers stayed bit-equal for 600 ticks (`Player.*` in `VortexSimTests`).
- `TerrainColliders::HasCollider(box)` is the gate of `sim::StepPlayer`: `ChunkReady` without
  `installed >= exposed` (`ChunkBuilt` / `BoxBuilt`). Do not "fix" it to `IsReady`: re-cook timing differs between a
  server and its clients, so that gate would hold players for different numbers of ticks after a blast at a chunk
  border (`HasColliderIgnoresUncoveredFacesWaitingForARecook`). The game holds its player through `StepPlayer` now,
  not `Engine::TerrainReady`.
- `PhysicsConfig::characterLayers` (`LayerBit` mask, default Static | Dynamic | Debris; Character and Rubble never)
  is the filter of `CreateCharacter`, `UpdateCharacter`, `SetCharacterPosition` and `SetCharacterState`, kept in the
  `Simulation` (one per physics world). A session uses `sim::kSessionCharacterLayers` (Static | Dynamic since
  networking stage 6; Static only before) and `characterMovesBodies = false` on the server and every client.

## Replicated bodies (networking stage 6)

- `Layer::Rubble` (enumerator 4, `kLayerCount` 5): collides with Static, Dynamic and itself; never with Debris or a
  character. In it: a session client's rubble of `debrisBodyVoxels` or more (`SceneConfig::rubbleLayer`), its copies
  of server bodies while they move, and a server's replicated bodies while they move (`net::Server` moves them between
  Rubble and Dynamic when they wake or rest). At rest a replicated body is in `Layer::Dynamic` on both sides, the only
  layer besides Static a session character meets.
- Since the review of stage 6 a moving replicated body that carries a player stays in `Layer::Dynamic` (server: any
  player's `CharacterState::groundBody`; client: its own player's), and waking is an event (`EntityWake`).
  `CharacterState::groundBody` is Jolt's `GetGroundBodyID` whenever the character is not in the air.
  `Raycast(origin, dir, max, layers)` sees only the bodies of a `LayerBit` mask (a session cast: Dynamic | Rubble).
- `SetLayer(body, layer)` / `GetLayer(body)`: Jolt's `SetObjectLayer`. Refused (logged) when it would put a movable
  body into `Layer::Static` or take a static one out of it (Static-layer objects never meet each other); a frozen
  body is movable here (its motion type is Static only while it waits).
- `MoveKinematic(body, position, rotation, dt)`: Jolt's `MoveKinematic` - the velocities that reach the target at the
  end of the next step of `dt`, so the body pushes what it meets and reports a velocity; refused for other motion types
  and non-finite input (`KinematicBodiesFollowMoveKinematic`). A client's copy is kinematic: `GuardBodies` never
  freezes it (only `movables`, dynamic bodies, are guarded), and it keeps the velocity of the last `MoveKinematic`
  until set again (the client zeroes it at rest and when it has no sample).
- `TerrainColliders::FlushBox(box)` (review of networking stage 4) takes the world's changes and cooks, on the calling
  thread, every chunk of the box (grown by one voxel) whose newest content is not installed or that is not tracked
  yet; a worker's later result for it is dropped (`result.generation <= installed`). Up-to-date chunks cost a lookup.
  A session calls it on `sim::PlayerReach` before every player step, so that what a character meets is a function of
  the edits applied, not of when a cook finished (`FlushBoxInstallsTheNewestCollidersOfABoxAtOnce`). It leaves a
  coordinate in `dirty` with `queued` false, which `ProcessDirty` skips. Do not call it on large boxes: each stale chunk
  is a full cook on that thread.
- Open, smaller since the review: on the border-blast test errors of up to 10 mm (below the 2 cm threshold) remain
  after blasts begin, the same with and without waiting for cooks; before the first blast the prediction is now
  bit-equal. Most likely the client predicting on the uncarved ground for a round trip; not isolated.

