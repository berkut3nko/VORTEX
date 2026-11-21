# Physics module notes (`vortex.physics`)

Checked against engine `430fde0`.

Companion to `CLAUDE.md` and `docs/ARCHITECTURE.md` section 6 / 6.1 (design, not repeated here). Code is cited by file
and symbol; search for the symbol. `PW.cpp` = `src/physics/internal/PhysicsWorld.cpp`, `T.cpp` =
`src/physics/internal/Terrain.cpp`. The text before this rewrite, with its line numbers, is
`docs/history/notes/physics-2026-11-18.md`.

## 1. Map

- `src/physics/Physics.cppm` - primary module unit; only re-exports the partitions `:world` and `:terrain`.
- `src/physics/PhysicsWorld.cppm` (`:world`) - the whole public surface over Jolt: `BodyId`, `BodyDesc`,
  `BodyState`, `DynamicBodyInfo`, `RayHit`, `ContactEvent`, `Character*`, `Layer` / `LayerBit`, `PhysicsConfig`,
  `PhysicsWorld`. No Jolt type or header appears here; keep it that way.
- `src/physics/Terrain.cppm` (`:terrain`) - `TerrainColliders` (collider streaming + body guard),
  `TerrainColliderStats`.
- `PW.cpp` - the only file that includes Jolt. Process-wide runtime ref-count (`AcquireRuntime` / `ReleaseRuntime`,
  `RuntimeRef`), glm <-> Jolt conversions (`ToJolt`, `ToGlm`), layer filters (`LayerPairFilter`, `BroadPhaseLayers`,
  `ObjectVsBroadPhaseFilter`, `CharacterLayerFilter`), `ContactCollector`, the `Shape` wrapper, `PhysicsWorld::Impl`
  with `Simulation` and the `movables` map, all methods.
- `T.cpp` - `Snapshot` / `CookJob` / `Shared` (worker side), `TerrainColliders::Impl` (`Entry` per tracked chunk,
  `Track` / `Touch` / `NeighbourChanged` / `Scan` / `ChunkReady` / `ChunkBuilt` / `BoxReady` / `BoxBuilt` /
  `BodyReady` / `Require` / `GuardBodies` / `Apply` / `InstallResults` / `ProcessDirty`), public `Update` /
  `GuardBodies` / `IsReady` / `HasCollider` / `FlushBox` / `Flush` / `Stats`.
- `tests/physics/PhysicsTests.cpp` - suite `PhysicsWorld` (bodies, shapes, rays, contacts, characters, layers,
  kinematic bodies), no voxel world.
- `tests/physics/TerrainColliderTests.cpp` - suite `TerrainColliders` (streaming, edits, focus, unload, character on
  terrain, `HasCollider`, `FlushBox`).
- `tests/physics/TerrainGuardTests.cpp` - suites `TerrainGuard` (freeze / park / resume, re-cook rules, stale edits)
  and `FrozenBodies` (`SetFrozen` alone, fixture `Bare`).
- The guard as `sim::Simulation` calls it is pinned in `tests/sim/SimulationTests.cpp`:
  `StepFreezesABodyWhoseGroundHasNoColliderYet` (a tick freezes a crate with no collider under it, then it resumes
  and lands) and `StepPhysicsGuardsWhatGameCodeCreatedAfterTheStreamingPhase` (fails if `SimulateBodies` stops
  calling `GuardBodies`: the guard inside `TerrainColliders::Update` does not see a body made after it).
- The three physics test files build into one executable, `VortexPhysicsTests` (`tests/CMakeLists.txt`, sources
  globbed).
- Callers outside the module (the physics world and the terrain colliders belong to `sim::Simulation`):
  `src/sim/internal/Simulation.cpp` `Stream` (streaming), `SimulateBodies` (guard + step + sync), `Shutdown`
  (teardown); `Engine::Run` (frame order: which phase is called when; the `Physics:` exit line); players through
  `sim::StepPlayer` (`src/sim/internal/Player.cpp`), which the game (`OnFixedUpdate` in `../src/main.cpp`), the
  session server and the predicting client all call. `scene` owns entity bodies.

## 2. Invariants

### Ownership and lifetime
- Jolt globals (allocator, `Factory`, registered types) are ref-counted by `g_RuntimeUsers` under
  `g_RuntimeMutex`. Users are every `Simulation` and every `Shape`; each `Create*Shape` also holds a temporary
  `RuntimeRef`. A `ShapeRef` may outlive its `PhysicsWorld` (test `WorldsAndShapesShareTheRuntime`).
- `RuntimeRef` must be the first member of `Shape` and of `Simulation` (both in `PW.cpp`): members die in reverse
  order, so Jolt stays usable until the Jolt objects are gone. Do not reorder those members. `Simulation` order also
  matters: `characters` is declared after `system` so characters die first.
- Jolt runs on its own thread pool (`JPH::JobSystemThreadPool` inside `Simulation`, `PhysicsConfig::threads`,
  0 = hardware - 1 clamped to 1..8), not on the engine `JobSystem`. Only collider cooking uses the latter.
- `PhysicsWorld::Shutdown()` destroys every body and character and clears `movables` / `frozenCount` / events.
  `sim::Simulation::Shutdown` resets `terrain` before `physics.Shutdown()`; the scene must be cleared first
  (ARCHITECTURE "Destruction order"). Every `BodyId` / `CharacterId` is dead after `Shutdown()`; a new `Initialize()`
  builds a fresh `Simulation` whose ids start over, so an id kept across it can match a different body or character.
  Drop all ids on shutdown.
- `TerrainColliders` holds references to `PhysicsWorld`, `voxel::World`, `JobSystem`, `MaterialLibrary`; all must
  outlive it. Its destructor takes `Shared`'s `lifetime` mutex exclusively, sets `cancelled`, then destroys its
  bodies. Jobs still queued afterwards only hold the `shared_ptr<Shared>` and return early.
- A terrain collider body belongs to its `Entry` (`RemoveBody`). `colliderCount` is adjusted only there and in
  `Apply`.

### Threading
- Everything is main-thread only except `CreateCompoundShape` / `CreateMeshShape` / `CreateBoxShape` /
  `CreateSphereShape`. Those read only `Impl::initialized` (atomic) and never touch `sim`.
- `ContactCollector::OnContactAdded` runs on Jolt worker threads during `Step` with the two bodies locked: it may read
  only those two bodies and append under `m_Mutex`.
- `CookJob` (worker) touches only its `Snapshot`, the immutable `CollisionFilter`, `physics->CreateMeshShape` and
  `Shared::results` under `Shared::mutex`. It never reads `voxel::World` (main-thread only): everything it needs is
  copied in `TakeSnapshot` on the main thread.
- `Shared::inFlight` is incremented on the main thread before `Submit` and decremented by a `ScopeExit` declared first
  in `CookJob`, i.e. after the result was published. `Flush` depends on exactly that order ("read the job counter
  first").
- `UpdateCharacter`, `CreateCharacter`, `SetCharacterPosition`, `SetCharacterState` use `sim->tempAllocator`, the
  same allocator `Step` uses: never call them concurrently with `Step`.

### Order within a frame or tick (`Engine::Run`, `Simulation.cpp`)
`Engine::Run` reaches these calls through `sim::Simulation`: `UpdateStreaming(camera, {camera})` is the first two,
`StepPhysics()` is the bracket without `OnFixedUpdate`. `Simulation::Step` (a server tick) runs the same calls once
each, with the window held in place and no `OnFixedUpdate`; there `terrain.Update` runs once per tick, so everything
counted in `Update()` calls (`kBodyKeepUpdates`, the scan) is counted in ticks on a server and in frames in the engine.
`world.Update` -> `terrain.Update` (consume changed chunks, scan, drop, `InstallResults`, `GuardBodies`,
`ProcessDirty`) -> up to 4 x [`OnFixedUpdate`, `terrain.GuardBodies`, `physics.Step(fixedDt)`,
`scene.SyncFromPhysics`]. Inside `Update` the order install -> guard -> process is deliberate: bodies resume on what
was just installed, and their requests are cooked in the same call. `GuardBodies` must run after any code that
created / moved bodies and before `Step`. A player step calls `FlushBox(sim::PlayerReach(feet))` first (below).

### Units and spaces
- World metres, single precision. `BodyDesc::position` / `BodyState::position` are the **shape origin**, not the
  centre of mass (test `BodyPositionIsTheShapeOrigin`).
- Character positions are **feet** positions; the capsule is offset up by `height/2` (`CreateCharacter`).
- Impulses kg*m/s, density kg/m^3 (min 1, `kMinDensity`), half extents >= 1e-4 m (`kMinHalfExtent`).
- Terrain chunk coordinates in `T.cpp` are absolute world chunk coordinates (not window-relative). A collider body
  sits at `ChunkBounds(coord).min` with mesh vertices in chunk-local metres (voxel units * `voxelSize`, `CookJob`).
  Focus points are `dvec3`; everything handed to Jolt is `float`.
- Terrain colliders have `userData == 0`, `Layer::Static`, `MotionType::Static` (`Apply`).

### Ids
- `BodyId::value` is Jolt's index + sequence number verbatim; `0xFFFFFFFF` is invalid; ids with bit 31 set are
  rejected (`Impl::Find`). Every public method goes through `Find` and is a no-op / default result for stale ids -
  keep that for any new method. How many sequence values Jolt cycles through before an index can alias an old id is
  unverified (Jolt internal): do not keep ids of destroyed bodies around.
- `CharacterId` = 12-bit generation << 20 | 20-bit slot index (`CharacterSlot`, `FindCharacter`). Generations run
  0..0xFFE so the id is never `0xFFFFFFFF`; a slot's generation wraps after 4095 destroy cycles, after which a very
  old id can match again. Slots are reused LIFO from `freeCharacters`.
- `movables` holds exactly the bodies whose engine-level motion type is Dynamic, frozen ones included.
  `frozenCount` == number of entries with `frozen`. Every path that creates, destroys or changes motion type must
  keep both right: `CreateBody`, `DestroyBody` -> `Forget`, `SetMotionType`, `SetFrozen`. `GetDynamicBodies` and the
  guard see nothing else.

### Value handling
- All inputs are validated for finiteness; bad values are logged and ignored, never asserted (`CreateBody`,
  `SetTransform`, `SetVelocity`, `AddImpulse`, `Raycast`, `Step`, `TerrainColliders::Update`). Quaternions are
  normalised on the way into Jolt; only non-finite or near-zero ones become identity (`ToJolt(quat)`). `BodyDesc`
  material values are clamped, not rejected.
- `Step` ignores `dt <= 0`; it runs one collision step. Jolt update errors are logged once per kind; the once-only
  flags (also "out of bodies") are reset only by `Initialize`.

### Terrain entry state machine (`Entry` in `T.cpp`)
- `generation` = newest request (bumped by every `MarkDirty`, from the global `nextGeneration`); `installed` =
  generation of what is installed; `exposed` = generation at which a neighbour uncovered faces. Ready means
  `built && installed >= exposed` and the chunk is loaded (`ChunkReady`). Built means a collider of the newest
  content without the `exposed` rule (`ChunkBuilt`, what `HasCollider` asks).
- A result is installed iff `result.generation > entry.installed` (`InstallResults`). `Track` sets
  `installed = nextGeneration` so results of an earlier, dropped entry for the same chunk are rejected.
- `border` / `hasBorder` mirror the neighbour bits the newest job was given; updated in `ProcessDirty` and
  `NeighbourChanged`.
- `dirty` may contain duplicates and coordinates of dropped entries; `queued` is the truth.
- At most `maxJobs = max(2, 2 * jobs.ThreadCount())` cook jobs per **round** [net-6]: a round is an `Update` together
  with the tick's `GuardBodies` (both call `ProcessDirty`), or one pass of `Flush`; its budget is what was in flight
  when it began (`StartRound`) plus what it started - never the live counter re-read while it submits, which let a
  timing-dependent number start. The rest stay queued for a later `ProcessDirty`. Order: chunks a body asked for
  lately first, then nearest to a focus point. A chunk that needs no collider is settled in `ProcessDirty` itself,
  without a job.
- An untracked chunk is ready only if `NeedsNoCollider`. So a loaded chunk with a surface but outside every focus
  radius is "not ready", which is exactly what parks the bodies resting there.
- A change of material collidability (`MaterialLibrary::Revision`, `RefreshFilter`) re-queues every tracked chunk.
  Liquids never collide (`MaterialFlag_Liquid` is the exclusion passed to the filter).
- Packed chunks [vd-1]: a `Packed` chunk (no slot, RLE bytes only) wants a collider like a Dense one
  (`WantsCollider`, `Require`); `TakeSnapshot` and `BorderBits` read its voxels through `World::ReadChunk` (a decoded
  copy; `BorderBits` keeps one scratch chunk, main thread). `NeedsNoCollider` is false for it. `GetChunk` returns
  nullptr and uniform id 0 for it, so `IsUniformCollidable` treats it as open (conservative: neighbours build their
  faces).
- `ChunkReady` treats a chunk above the top of a full-height window (x / z inside it) as ready: it is the sky over the
  build ceiling [vd-1]. Without that, a body thrown upwards froze in mid-air under the ceiling for good.

### Stale parts of a collider [shred-1d]
- `ConsumeChanges` adds each tracked chunk's edit box (`World::ChunkChange`, world voxels) to `Entry::pendingEdit`.
  `ProcessDirty` moves it into `cookingEdits` tagged with the generation of the snapshot it starts (`StartCook`, also
  on the `NeedsNoCollider` path); `Apply` of a result of generation g drops every tag <= g (`Installed`).
  `BodyReady` (the guard's test) = `BoxReady` **and** no touched entry is stale where the body is (bounds + one voxel
  + 0.05 s of travel, in voxels). Keep the per-generation tags: a single union box cleared only when the newest result
  lands never clears on a chunk edited every frame, and then freezes every body near a held brush
  (`BodyBesideAChunkEditedEveryFrameIsNotHeld`). The rule: a body must not be simulated against geometry known to be
  gone; a new piece over its stale collider waits frozen with its velocity
  (`PieceOverAStaleColliderWaitsFrozenWithItsVelocity`).
- `IsReady` (`Engine::TerrainReady`) is chunk-level only: a player standing in the crater they shot is not held.
- The public `GuardBodies()` calls `ConsumeChanges` first: a blast in `OnFixedUpdate` is followed by the guard of the
  same tick before any streaming, and must already freeze the piece
  (`Rubble.DetachedPieceWaitsFrozenWithItsPushOverAStaleCollider` steps once without `UpdateStreaming`).
- **Wake on first build.** `modifiedUnseen` holds chunks edited while no up-to-date collider of them existed: edited
  while untracked (and loaded), or dropped in the scan while `Entry::Stale()`. `Apply` on a first build (`!rebuilt`)
  erases the chunk and `ActivateInBox`es its bounds. Every guard pass keeps only the chunks a body frozen now lies on
  (`ChunkRange` of its bounds + one voxel), so the set never outgrows the chunks under parked bodies.
  `TerrainColliderStats::unseenEdits` is its size (`UnseenEditsAreRememberedOnlyUnderFrozenBodies`). Do not prune by
  the world window: a chunk that unloaded under a parked body must stay remembered until it reloads. Tests:
  `ParkedBodyFallsWhenItsGroundWasCarvedJustBeforeTheColliderWasDropped`,
  `ParkedBodyOverAChunkCarvedAndUnloadedFallsAfterTheReload`.

### Players [net-4]
- A character moves only through `sim::StepPlayer` (offline game, session server, predicting client: CLAUDE.md
  "one movement rule"). Its gate is `TerrainColliders::HasCollider(capsule bounds)`: `ChunkReady` without
  `installed >= exposed` (`ChunkBuilt` / `BoxBuilt`). Do not "fix" it to `IsReady`: re-cook timing differs between a
  server and its clients, so that gate would hold players for different numbers of ticks after a blast at a chunk
  border (`HasColliderIgnoresUncoveredFacesWaitingForARecook`).
- `SetCharacterState(id, feet, velocity)` = `SetPosition` + `SetLinearVelocity` + `RefreshContacts` (the ground state
  comes from the refresh, not from the history). Replaying the same steps from a restored state gives the same
  positions (`CharacterStateIsRestoredAndReplaysClose`); two simulations with different worker counts stay bit-equal
  over voxel terrain (`Player.*` in `VortexSimTests`).
- `PhysicsConfig::characterLayers` (`LayerBit` mask, default Static | Dynamic | Debris; Character and Rubble never) is
  the filter of `CreateCharacter`, `UpdateCharacter`, `SetCharacterPosition` and `SetCharacterState`, kept in the
  `Simulation` (one per physics world). A session uses `sim::kSessionCharacterLayers` (Static | Dynamic) and
  `characterMovesBodies = false` on the server and every client.
- `SetCharacterWalls(min, max)` (WP1): four static boxes in `Layer::Bounds` (enumerator 5, `kLayerCount` 6) at the
  planes x = min.x / max.x, z = min.y / max.y, 2 m thick, y -512..512 m; replaced by the next call, removed by a
  non-finite or empty rectangle. Every character meets them whatever `characterLayers` says (`CharacterLayerFilter`);
  `LayerPairFilter` and `ObjectVsBroadPhaseFilter` let no body meet them and `LayerMaskFilter` hides them from every
  `Raycast`. They count in `BodyCount`. A session's play area (`sim::SetPlayAreaWalls`;
  `PhysicsWorld.CharacterWallsStopOnlyCharacters`).
- `TerrainColliders::FlushBox(box)` [net-4] takes the world's changes and cooks, on the calling thread, every chunk
  of the box (grown by one voxel) whose newest content is not installed or that is not tracked yet; a worker's later
  result for it is dropped (`result.generation <= installed`). Up-to-date chunks cost a lookup. Every player step
  calls it on `sim::PlayerReach` first, so that what a character meets is a function of the edits applied, not of
  when a cook finished (`FlushBoxInstallsTheNewestCollidersOfABoxAtOnce`). It leaves a coordinate in `dirty` with
  `queued` false, which `ProcessDirty` skips.

### Replicated bodies [net-6]
- `Layer::Rubble` (enumerator 4, `kLayerCount` 5): collides with Static, Dynamic and itself; never with Debris or a
  character. In it: a session client's rubble of `debrisBodyVoxels` or more (`SceneConfig::rubbleLayer`), its copies
  of server bodies while they move, and a server's replicated bodies while they move (`net::Server` moves them between
  Rubble and Dynamic when they wake or rest). At rest a replicated body is in `Layer::Dynamic` on both sides, the only
  layer besides Static a session character meets. A moving replicated body that carries a player stays in
  `Layer::Dynamic` (server: any player's `CharacterState::groundBody`; client: its own player's).
  `CharacterState::groundBody` is Jolt's `GetGroundBodyID` whenever the character is not in the air.
- `Raycast(origin, dir, max, layers)` sees only the bodies of a `LayerBit` mask (a session cast: Dynamic | Rubble).
- `SetLayer(body, layer)` / `GetLayer(body)`: Jolt's `SetObjectLayer`. Refused (logged) when it would put a movable
  body into `Layer::Static` or take a static one out of it (Static-layer objects never meet each other); a frozen
  body is movable here (its motion type is Static only while it waits).
- `MoveKinematic(body, position, rotation, dt)`: Jolt's `MoveKinematic` - the velocities that reach the target at
  the end of the next step of `dt`, so the body pushes what it meets and reports a velocity; refused for other motion
  types and non-finite input (`KinematicBodiesFollowMoveKinematic`). A client's copy is kinematic: `GuardBodies`
  never freezes it (only `movables`, dynamic bodies, are guarded), and it keeps the velocity of the last
  `MoveKinematic` until set again (the client zeroes it at rest and when it has no sample).

## 3. Traps

- **Freezing = Jolt static, object layer kept** (`SetFrozen`). That is why other bodies rest on a frozen body (test
  `OtherBodiesRestOnAFrozenBody`). Do not "tidy" this by moving frozen bodies to `Layer::Static`: Static-vs-Static
  never collides, and release does not touch the layer, so it would stay Static. Jolt cannot activate a static body,
  so `ActivateInBox` reaches frozen bodies through its own loop over `movables`, which only sets `wake`; `Activate`,
  `SetTransform(activate)`, `AddImpulse`, non-zero `SetVelocity` and `SetMotionType(Dynamic)` do the same.
- **The guard owns the frozen flag.** `GuardBodies` sets it from terrain readiness for every dynamic body on every
  call, so a `SetFrozen` made by game or scene code is undone at the next guard (released if the terrain is ready, and
  vice versa). Nothing outside `Terrain.cpp` calls `SetFrozen`; a "pause this body" feature needs its own mechanism
  (or a flag the guard respects), not `SetFrozen`.
- **`GetState` of a frozen body reports zero velocity** (read from the static Jolt body); the kept velocity is in
  `Movable`, `DynamicBodyInfo::linearVelocity` (the guard reads that) and `BodyState::keptLinearVelocity` [shred-1d]
  (logs and tests: "frozen, velocity kept").
- **Release restores velocities only if `wake`** (`SetFrozen`). A body frozen asleep resumes asleep with zero
  velocity; that is the "parked rubble costs nothing" behaviour, not a bug.
- **`AddImpulse` on a frozen body drops the spin** of an off-centre impulse and converts to linear velocity via
  `GetMass`. `SetVelocity` on a frozen body replaces the kept velocities; `SetVelocity(0,0)` does not mark it as
  wanting to move. `SetShape` on a frozen body does not set `wake`. `SetMotionType(Static/Kinematic)` on a frozen
  body forgets it (kept velocities are lost).
- **`SetShape` on a frozen dynamic body with a mesh shape passes the "must be static" check** (it tests the Jolt body,
  which is static while frozen). Nothing calls it that way; releasing such a body is unverified and probably invalid.
  Check `movables` there if this path ever becomes reachable.
- **`SetMotionType(Dynamic)` moves a `Layer::Static` body to `Layer::Dynamic`** and it is never moved back when made
  static again. Deliberate: a static-layer body would fall through terrain. Kinematic targets keep their layer.
  Mesh-shaped bodies are created without motion properties and can never leave Static.
- **Contact events**: at most one per body pair per `Step` (the hardest), sorted by ids; slower than
  `contactEventMinSpeed` dropped; `!(speed >= min)` also drops NaN. Only `OnContactAdded` is implemented: contacts
  that persist or end produce nothing. The speed is signed (body 1 approaching body 2 along the 1->2 normal is
  positive), so separating contacts are dropped too. The queue is capped at `kMaxQueuedContactEvents` (16384) and then
  warns once (the per-step buffer has the same cap, silently). **Nothing in the engine, scene, sim, net or game calls
  `TakeContactEvents`** (grep), so in a long session the queue fills and stays full; whoever adds impact effects
  (shred.md SI-2) must drain it every tick. Events can name bodies destroyed since: look ids up, do not assume
  validity. Which of the two bodies Jolt reports as A is unverified.
- **Characters are not bodies**: not stepped by `Step`, not in `GetDynamicBodies`, not guarded, not hit by `Raycast`,
  no contact events. Only `sim::StepPlayer` calls `UpdateCharacter` (once per tick) and holds the character while its
  terrain has no collider (`HasCollider`, section 2 "Players").
- **Character capsule is shrunk by Jolt's `mCharacterPadding`** (`CreateCharacter`) so shape + padding equals the
  requested capsule and the feet sit on the ground. Changing radius maths without this lifts or sinks the player.
- **`UpdateCharacter` has no horizontal inertia**: velocity is rebuilt each call from ground velocity or the current
  vertical speed, plus gravity, plus the wanted x / z; `desiredVelocity.y` is ignored. A jump starts only when
  `OnGround` and moving towards the ground.
- **Characters ignore `Layer::Character` and `Layer::Rubble` bodies** (`CharacterLayerFilter`), whatever
  `characterLayers` says, so a kinematic proxy body following the controller, a session client's own large rubble
  and anything replicated that moves never block it [net-6]. The virtual character pushes light dynamic bodies
  (Jolt's `mMaxStrength`, 100 N: a 5 kg box moves, a 51 kg crate on the ground does not) and weighs on the one it
  stands on; `PhysicsConfig::characterMovesBodies = false` (a session) does neither: a listener sets
  `mCanReceiveImpulses = false` on every contact and the character's mass is 0
  (`CharactersThatMoveNoBodiesNeitherPushNorWeigh`).
- **`Raycast` normal**: falls back to `-dir` and is always flipped to face the ray origin. Without a layer mask it hits
  every layer, frozen bodies included; only sensors are skipped.
- **`TerrainColliders` must be the only consumer of `World::TakeChunkChanges()`** (`ConsumeChanges`; the
  coordinate-only `TakeChangedChunks` empties the same list). A second consumer silently starves one of them. The
  range scan runs only when a focus point moved a quarter chunk or the radius / point count changed (`NeedsScan`), so
  a chunk that becomes loaded later gets its collider *only* because loading reports it as changed
  (`Touch(coord, true)`).
- **Radii**: create within `radius + chunk/4`, drop beyond `max(1.5*radius, radius + chunk/2)`
  (`TerrainColliders::Update`). `radius <= 0` or no focus point means only body-requested colliders survive.
- **`GuardBodies` step 1 vs step 2**: only awake / wants-to-move bodies *request* colliders (`kRequestLookahead`
  0.75 s ahead, clamped to 2 chunks; kept `kBodyKeepUpdates` = 120 `Update()` calls, i.e. frames, not steps), but
  *every* dynamic body, asleep too, is frozen when its chunks (bounds + one voxel + `kFreezeLookahead` 0.05 s of
  travel, clamped to 1 chunk) are not ready. Making sleeping bodies request colliders would bring back the cost the
  parking design removed.
- **Old collider stays until the new one is installed**: `Apply` removes and creates in one call. Never remove a body
  in `MarkDirty` / `Touch`. `Apply` wakes bodies in the chunk box only on a *re*build where a body existed before or
  after.
- **`NeedsNoCollider`**: empty dense chunk, non-collidable uniform, or collidable uniform enclosed by six collidable
  uniform neighbours. It also returns true for a chunk that is not loaded, which is why `ChunkReady` tests
  `IsChunkLoaded` first. Keep that order.
- **Reproducible collider installs.** `InstallResults` installs a round's results in chunk order (x, y, z,
  generation), not in the order the workers finished, and `Flush` waits for every job of a pass before it installs:
  collider bodies get their Jolt ids (which Jolt's contact order follows) the same way in every run that installs the
  same results together. With both, a harness that waits for idle between ticks is reproducible bit for bit
  (`Entities.OneScriptGivesTheSameBodiesTwice`). Keep both.
- **`Flush` spins with 1 ms sleeps** until no job, result or dirty entry remains, re-guarding in between. It does not
  call `Update`, so it does not consume changed chunks or scan: call `Update` first. It blocks for as long as the
  `JobSystem` workers are busy (a `JobSystem` always has at least one thread). Tests and loading screens only; never
  in the frame loop or a server tick (notes/net.md "Traps").
- **`FlushBox` on a large box** cooks every stale chunk on the calling thread: keep it to `sim::PlayerReach`-sized
  boxes.
- Cost to keep in mind: `GuardBodies` is O(dynamic bodies) with a sort, once per `Update` plus once per fixed step
  (the public one also runs `ProcessDirty`); `ActivateInBox` walks all `movables` while anything is frozen. The
  stale-edit tags cost one `std::vector` per tracked entry (empty unless edits are in flight) and a few comparisons
  per chunk `BoxReady` visits. None of these is measured in this module (estimate: fine for hundreds of bodies).
- `SetShape`, `SetMotionType`, `GetActiveBodies` and `TakeContactEvents` have no caller outside `src/physics` (grep):
  they are covered by tests only, so a regression there shows up in `VortexPhysicsTests`, never in a headless game
  run.
- Open: on the session border-blast test, prediction errors of up to 10 mm (below the 2 cm correction threshold)
  remain after blasts begin, with and without waiting for cooks; most likely the client predicting on the uncarved
  ground for a round trip; not isolated [net-6].

## 4. Recipes

**A. Add a `PhysicsWorld` method on a body.** Declare in `PhysicsWorld.cppm` with a doc comment; implement in `PW.cpp`
starting with `const auto id = m_Impl->Find(body); if (!id) return ...;`, validate finiteness, then decide explicitly
what it does for a frozen body (`m_Impl->FindFrozen(body)`): anything that would wake or push must set `held->wake` /
adjust `held->linear` instead of calling Jolt. Extend `StaleAndInvalidIdsAreHarmless` (PhysicsTests.cpp, loops over
bad ids) and add a `FrozenBodies` case.

**B. Add a collision layer.** Add the enumerator at the end of `Layer`; add `kLayer...`, bump `kLayerCount`
(`PW.cpp`); extend `LayerPairFilter`, and `BroadPhaseLayers` / `ObjectVsBroadPhaseFilter` only if it is non-moving;
decide `CharacterLayerFilter` and the `LayerBit` masks that should include it (`kSessionCharacterLayers`, a session
cast's mask). `ToJolt(Layer)` maps unknown values to Dynamic. Test next to `DebrisIgnoresDebrisButRestsOnStatic`,
`CharacterIgnoresCharacterLayerBodies` and `RubbleLayerMeetsTheWorldBodiesAndItselfButNoCharacter`.

**C. Add a shape type.** New `Create*Shape` in the interface (const, thread safe): check `m_Impl->initialized`,
validate, take a local `RuntimeRef`, build, `WrapShape`. Touch nothing in `sim`. Set a density so mass is positive.
Tests: mass like `CompoundMassIsDensityTimesVolume`, origin like `BodyPositionIsTheShapeOrigin`; if it must be
static, extend `MeshShapesAreStaticOnly`.

**D. Change character behaviour (crouch, air control, pushing).** Physics side: `CharacterDesc` / `CharacterSlot`
(`PW.cpp`) for per-character data, `CreateCharacter` for shape and settings, `UpdateCharacter` for velocity rules.
Keep feet-position semantics and the padding shrink. Rule side: `sim::StepPlayer` and `PlayerTuning` (notes/net.md
recipe "New player rule": wire format and the protocol). Tests: `CharacterStandsJumpsAndLands`,
`CharacterIsBlockedByBodies` (PhysicsTests.cpp), `CharacterWalksAndClimbsSmallStep` (TerrainColliderTests.cpp),
`Player.*` (VortexSimTests) and `Movement.NoCorrectionAbove2cmOnFlatGround` (VortexNetTests, 0 corrections). The
headless log line `Player feet at ..., on the ground` must survive.

**E. Change guard tuning or readiness rules.** Constants at the top of `T.cpp` (`kFreezeLookahead`,
`kRequestLookahead`, `kBodyKeepUpdates`); logic in `GuardBodies`, `ChunkReady`, `BoxReady`, `BodyReady`,
`ChunkRange`. If the numbers change, update ARCHITECTURE 6 / 6.1 (0.05 s, 0.75 s, 2 chunks, 120 updates, 1.5x) in the
same commit. Run the whole `TerrainGuard` suite; the cases most sensitive to tuning are
`BodyFrozenInFlightKeepsItsVelocity`, `WokenParkedBodyBringsItsOwnCollidersBack`, `ManyBodiesFarFromTheFocusAllLand`,
`BodyStopsAtTheEdgeOfTheWorldWindow`.

**F. Change what a collider mesh contains or when it is re-cooked.** Mesh content is `voxel::BuildChunkCollisionMesh`
(voxel module) fed by `Snapshot` / `BorderBits`; anything new the job needs must be copied into `Snapshot` on the
main thread. Re-cook decisions are `Touch` / `NeighbourChanged`; keep the `generation` / `installed` / `exposed`
rules of section 2 and the stale-edit tags. Tests: `SurfaceOnChunkBoundaryCollides`,
`CarvingUnderRestingBoxMakesItFall`, `EditedChunkKeepsItsColliderUntilTheNewOneIsInstalled`,
`ChunkEditedEveryFrameStillGetsNewColliders`, `ChunkUncoveredByCarvingItsNeighbourIsNotReadyUntilRecooked`,
`EditAwayFromTheBorderRecooksOnlyTheEditedChunk`.

## 5. Verifying a change here

- Gate G1 (build), then `ctest --test-dir build -R "PhysicsWorld|TerrainColliders|TerrainGuard|FrozenBodies"` or
  `./build/engine/tests/VortexPhysicsTests --gtest_filter=TerrainGuard.*` while iterating, then gate G2 (all tests):
  `VortexEngineTests` exercises the scene on a real `PhysicsWorld` and breaks on `movables` / id mistakes, and the
  session tests (`Movement.*`, `Entities.*`) break on layer, `HasCollider` or install-order changes.
- Terrain tests are timing-tolerant by construction (the fixtures wait with `jobs.WaitIdle()` or `Flush()`); a test
  that fails only sometimes means a real ordering bug (`inFlight`, generations), not flakiness to retry. Run a
  suspect test with `--gtest_repeat=50`.
- Gate G3 (headless game run): besides `Beam cut: ... 1 new object(s)` and `Player feet at ..., on the ground`, read
  the line `Physics: N bodies (A awake, F frozen), C terrain colliders; ...`. Expect `C > 0`, and `F` small or 0 once
  loading settles; a large or growing `F` means colliders are not being cooked or the readiness test regressed. Any
  `Jolt: ...` warning (from `JoltTrace`), `PhysicsWorld: ... is full` or `out of bodies` line is a failure to
  investigate, as is `more than 16384 contact events`. Gate G7 (`--probe-blast`: `Probe piece 0 keeps its push`)
  covers the stale-collider rule.
- A session change: gate G11 (the drill; `Client N movement:` corrections) and G16.
- Screenshot: crates and debris must rest on the ground surface, not hang in the air (frozen forever) and not be
  missing (fell through; they are destroyed below the window by `scene.DestroyBelow`).
- Not checkable headless: feel of character movement, stairs and slopes in real play. Say so.
