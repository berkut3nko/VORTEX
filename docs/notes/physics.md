# Physics module notes (`vortex.physics`)

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
- All three build into one executable, `VortexPhysicsTests` (`tests/CMakeLists.txt:31`, sources globbed).
- Callers outside the module: `src/engine/internal/Engine.cpp:213-240` (frame order), `:81-87` (teardown),
  `:290` (log line); the game's character in `src/main.cpp:86-99`. `scene` owns entity bodies.

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
  `Engine` resets `terrain` before `physics.Shutdown()` (Engine.cpp:81, :87); the scene must be cleared first
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

### Order within a frame (Engine.cpp:213-240)
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
- At most `maxJobs = max(2, 2 * jobs.ThreadCount())` cook jobs are in flight (T.cpp:153, :514); the rest stay
  queued for a later `ProcessDirty`. Order: chunks a body asked for lately first, then nearest to a focus
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
  is only in `Movable` and in `DynamicBodyInfo::linearVelocity` (PW.cpp:969). The guard needs the latter.
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
  character while `Engine::TerrainReady(capsuleBounds)` is false (main.cpp:91-95). Use the box overload.
- **Character capsule is shrunk by Jolt's `mCharacterPadding`** (PW.cpp:1013) so shape + padding equals the
  requested capsule and the feet sit on the ground. Changing radius maths without this lifts or sinks the player.
- **`UpdateCharacter` has no horizontal inertia**: velocity is rebuilt each call from ground velocity or the
  current vertical speed, plus gravity, plus the wanted x/z (PW.cpp:1069-1079); `desiredVelocity.y` is ignored.
  A jump starts only when `OnGround` and moving towards the ground (:1067-1073).
- **Characters ignore `Layer::Character` bodies** (`CharacterLayerFilter`, :193), so a kinematic proxy body
  following the controller does not block it. Whether the virtual character pushes dynamic bodies is unverified.
- **`Raycast` normal**: falls back to `-dir` and is always flipped to face the ray origin (PW.cpp:923-935).
  It hits every layer, frozen bodies included; only sensors are skipped.
- **`TerrainColliders` must be the only consumer of `World::TakeChangedChunks()`** (T.cpp:566). A second
  consumer silently starves one of them. The range scan runs only when a focus point moved a quarter chunk
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
