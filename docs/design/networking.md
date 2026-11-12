# Networking skeleton (M1)

Status: design decision, revised after two reviews. Stages 0, 1 and 2 are built (see "Known risks of terrain
agreement after stage 0", "Stage 1 as built" and "Stage 2 as built"); nothing of the network itself exists yet. "Code" = read from the source at
commit `ac17bcf` and re-checked for this document, so the table "What the code gives us today" describes the code
before stage 1; "measured" = figure from `docs/ROADMAP.md`; "est." = estimate, replaced by a measurement in the stage
named under "Stages". Related: `fx-budget.md` (its simulation / presentation split is reused), `collapse.md`.

## Decision in brief

One rule decides every case: **the server computes; a client either repeats an integer-exact computation in the
server's order, or receives the result.** The single exception is the own player, which is predicted through Jolt and
corrected by the server; its error is measured, not assumed to be zero.

| Thing | How a client gets it | Check | Fallback |
|---|---|---|---|
| Pristine terrain | Generates it locally, FP contraction pinned off | Golden hashes per platform; probe hashes at join; sampled hashes near the player | Server sends chunks (RLE) |
| Terrain edits, island clears | Quantised integer operations in one server order (`opSeq`) | Sampled chunk hashes during play | Server sends the chunk |
| Falling terrain pieces, damaged objects | Result: occupancy mask or model + entity event, in the same `opSeq` stream | `EntityCheck` | Entity list re-sent |
| Bodies, other players | Snapshots at 30 Hz with velocity, drawn in the past | - | - |
| Own player | Prediction with the shared `sim::StepPlayer` | Server state in every snapshot | Restore + replay, error smoothing |
| Small debris, particles, flashes, lumps under the piece threshold | Local only: never simulated by the server, never hashed | - | - |

- Two new GPU-free libraries, `VortexSim` (scene + sim) and `VortexNet`, plus `vortex_server` and `vortex_bot`. ENet
  behind `net::ITransport`. 60 Hz simulation on both sides (code: `Engine.cppm:34`), 30 Hz packets.
- One chunk-sync mechanism (`ChunkData` + a base `opSeq` per chunk + replay from a client event ring) serves late
  join, a moving client window, chunks still loading, repair and the generator-mismatch fallback.
- Everything that is sent has a byte budget: snapshots (1200 B at 30 Hz), events (est. 24 kB/s), bulk (paced by the
  application). Work over budget is delayed or turned into client rubble, never dropped silently.
- Small stages. Est. 46-53 days without the 1 km stage (was 36-42 before the reviews: stages 2, 3, 4, 6, 7, 8 grew).

## What the code gives us today and what blocks us

| Code fact | Consequence |
|---|---|
| Core and Physics are GPU-free targets; `scene` is compiled into `VortexEngine` (`CMakeLists.txt:111-115`) and imports `vortex.graphics` only for `InstanceDesc` / `PointLight` (`Scene.cppm:22`); `GatherInstances` / `GatherLights` read only the public registry (`Scene.cpp:495-519`). | The two functions move to the engine as free functions; the scene then links without Vulkan. |
| `Engine::Impl` owns simulation and renderer together; `Engine::Explode` does carve, islands, object damage and impulses next to particles and a flash (`Engine.cpp:125-183`). Island extraction clears the island's voxels from the world (`Connectivity.cpp:222-234`). | The simulation half must be extracted. An island is a world edit and needs an event from the first networked stage. |
| Loop: at most 4 steps per frame, then the accumulator is dropped; no tick counter; a headless run advances one fixed step per rendered frame and still creates a Vulkan device (`Engine.cpp:114-119, 205-206, 226-240`). | Nothing is tick-indexed, and `Shredborne --headless` cannot be a wall-clock network client. |
| The generator is a pure function of settings and chunk coordinate using only + - * /, floor, abs, lround and compares (`Generator.cpp:124-134`, `Noise.cppm:132-158`). Engine code is built with `-mfma` and no `-ffp-contract` flag (`CMakeLists.txt:45-50`). The only test is same-process (`WorldTests.cpp:302`). | Bit-identical terrain across builds holds today only by accident. One flag removes the cause; a golden test proves it. |
| Edits visit chunks and voxels in a fixed order and decide per voxel from `(voxel, oldId)` (`World.cpp:450-500`); sphere tests are float. Cells outside the window or not loaded are skipped silently (`:92-95, 461-462`); on a full `ChunkStore` an edit abandons the chunk half-way (`:437-441, 482-485`) and a generated dense chunk is installed as Empty (`:322-331`). No swap edit. | Operations are replayable and can be clipped to one chunk, but need integer parameters, a rule for unready chunks and a visible failure state. |
| `ExtractFloatingIslands` is integer but returns nothing if any chunk of its box (radius + 24 voxels) is not loaded (`Connectivity.cpp:166-173`). Jolt 5.6.0 is built without `CROSS_PLATFORM_DETERMINISTIC` and steps on a thread pool (`Dependencies.cmake:109-128`). | Islands and object splits are server results. The server window needs a margin. Body poses are never hashed. |
| `Scene::Damage` uses the body's float transform, spawns particles itself (`Scene.cpp:343-360`), makes an entity and body for every piece of at least `minPieceVoxels` = 16, gives `Debris` to pieces and remnants under 48 voxels (`:419-426`), re-bases the survivor and turns a cut static object dynamic (`:393-405`). Entities die in `UpdateDebris`, `Update` (lifetime), `DestroyBelow` and `Damage`, with no notification. Layer and clean-up follow the voxel count only (`:55-56, 205`). | The piece threshold, the rubble policy and "which entities exist" are not reachable from outside. Scene changes are listed under "Entity replication". |
| The chunk codec is a canonical RLE (`Region.cpp:30-74`); cells carry a `generation` (`World.cppm:142`). No chunk hash. `TakeChangedChunks` belongs to `TerrainColliders` (`Terrain.cppm:33-34`); integration and recentring have no notification (`World.cpp:130-165, 291-341`); the client window follows the camera inside `Engine::Run` (`Engine.cpp:216`). | A wire format exists. Net code needs its own `World` notifications; the server needs its own "modified" map. |
| `World` is one window around one focus; the game uses 48 x 8 x 48 chunks = 154 x 26 x 154 m with `chunkCapacity` = 12,288, which is also the GPU chunk buffer size (`src/main.cpp:472-474`, `Engine.cpp:118`). `sizeof(Chunk)` = 36,936 B; 8100 of 18,432 cells are dense (measured). Default terrain rises 55 m and more (`Generator.cppm:52-56`). | A 250 m arena does not fit a client window. A fixed server window must be taller than 8 chunks. The client store can fill where the server's cannot. |
| `UpdateCharacter` runs outside `PhysicsWorld::Step` but reads the ground body's velocity, keeps Jolt's contact list between calls and pushes dynamic bodies (`PhysicsWorld.cpp:1054-1088`). Jolt's `SaveState` contains process-local `BodyID`s. Only `SetCharacterPosition` and `SetTransform` (teleport, zero velocity, `:694-701`) exist. | Replay is legal but not exact. A server state can be applied only as position + velocity. Moving kinematic bodies need `MoveKinematic`. |
| `CharacterLayerFilter` is hard-coded: everything but the Character layer (`PhysicsWorld.cpp:191-195`). Debris does not collide with Debris (`:182-189`). | A predicted character would collide with bodies the server does not have. The filter must be configurable. |
| The player is gated by `TerrainReady(capsule)` before `UpdateCharacter` (`src/main.cpp:92-95`); `ChunkReady` is also false while a carved neighbour waits for a re-cook: `built && installed >= exposed` (`Terrain.cpp:303-306`). | As a shared movement rule this would stop players for a different number of ticks on each side after every blast at a chunk border. |
| Movement rules are split between `src/main.cpp:86-100, 116-185` and `UpdateCharacter`. Entities are raw, recycled EnTT handles. `BuildPlayground` makes edits, crates, six lights and the player (`src/main.cpp:304-400`). | A shared step function, an input struct, a `NetId` and a split of template set-up are needed. |

## Architecture

```
VortexSim      scene sim                NEW, GPU-free: Core + Physics + EnTT
VortexNet      net                      NEW, GPU-free: Sim, PRIVATE enet
VortexEngine   engine editor            links Sim, Net, Graphics; owns one sim::Simulation
vortex_server  tools/server/main.cpp    links VortexNet only
vortex_bot     tools/bot/main.cpp       links VortexNet only: net::Client + Simulation on a wall-clock tick loop
```

- Core gains `ChunkHash` / `WorldHash`, quantised edits, chunk encode / replace, `World::TakeLoadedChunks()` and
  `WindowRevision()` (a second notification, independent of `TakeChangedChunks`). Physics gains
  `SetCharacterState`, `MoveKinematic`, `SetLayer`, `Layer::Rubble`, a character layer mask in `PhysicsConfig` and
  `TerrainColliders::HasCollider(bounds)`.
- `sim::Simulation` = today's simulation members of `Engine::Impl` plus `uint32_t tick` and `Step(focusPoints)`. Its
  world never has a `saveDirectory` in a session (`ServerConfig` clears it; a client sets none). `sim` also holds `RegionDesc`, `WorldOp`, `Blast` and `StepPlayer`; `net` holds
  transport, byte reader / writer, messages, `Server`, `Client` and the session file.
- Simulation vs presentation: `sim::Blast` (server or offline) = carve, island extraction, object damage, impulses;
  it returns the events it produced. `Engine::PresentBlast` (client) = particles and flash made from those events and
  from the client's own data (ids of the voxels it removes, old and new object model). No particle data is sent.
- Server scene: `bakeModels = false`, zero-capacity `ParticleSystem`, `debrisRemnants = false`, no `Debris` component
  (`markDebris = false`); all four are what `sim::ServerConfig` sets.
  The game owns `net::Client` and drives it from `OnFixedUpdate`, after the engine's streaming step, so it reads the
  window from `World`. `Engine` gains only a tick number and a tick-rate scale.

## Transport

| Channel | Mode | Carries |
|---|---|---|
| 0 state | unreliable | inputs up, snapshots down |
| 1 events | reliable, ordered, byte budget | handshake, the `opSeq` stream (operations, detach, entity events), window acks, hash and entity checks |
| 2 bulk | reliable, ordered, paced by the application | chunk data, entity baseline and models |

`net::ITransport` = `Send`, `Poll`, `RoundTripSeconds(peer)`, `ReliableBytesInFlight(peer)`; implemented by
`EnetTransport`, `LoopbackTransport` and the decorator `ImpairedTransport` (seeded loss, delay, jitter, duplication,
reordering, rate limit, outage; needs no root). ENet channels separate only ordering: the reliable window and queue are
per peer. Hence the invariant **at most est. 32 kB of bulk is inside the transport**: `net::Server` hands over the next
bulk message only below that, and halves the rate when RTT or event latency grows. Events have their own budget (est.
24 kB/s, burst 16 kB). `Hello{protocolVersion, buildHash, token[16]}`: `buildHash` covers the material table and the
generator golden hash; the token is compared in plain text with the session file. No encryption: a stub.

## Time and ticks

- Server tick `T`, fixed 1/60 s, never dropped; after a hitch it catches up at most 2 ticks per loop pass. Order:
  receive -> streaming -> `StepPlayer` per player in slot order -> queued casts within a **work budget** (graph node
  visits + chunk graphs used (`chunkGraphs`) at a fixed cold price per chunk since shred.md stage 4, plus the load
  check's counted work since stage 4b (`neckExteriorVisits + neckArcs + neckFlowWork / 8 + 64 x neckSplitRelabels +
  neckVoxels / 64`, weights calibrated once) - island-box voxels
  scanned + face-flood visits with the reference voxel search - + operations + event bytes per tick; never graphs
  *built* or chunks decoded, which depend on the server's cache and streaming history, calibrated once to about 6 ms on the
  reference machine from shred.md stage 0's measurement; wall clock is only logged, so a replayed server puts every
  cast on the same tick; shred.md R5) and the event byte budget -> `GuardBodies`, `physics.Step`, `SyncFromPhysics` -> lifetimes -> snapshots on even ticks.
- The client runs ahead by RTT/2 plus a margin of 2 ticks + measured jitter. Each snapshot reports the margin of the
  last input. Control is asymmetric: margin below 0 in 3 consecutive snapshots -> the clock jumps forward at once;
  a surplus is bled off at 2 %; above 8 ticks for 1 s -> hard resync (a server hitch recovers before that).
- Inputs: every packet carries all inputs the server has not acknowledged (at most 8, est. 2.5 kB/s). The server
  queue holds 16 per player; inputs further ahead are dropped (also the speed-hack bound). An input for a tick
  already simulated only refreshes "last input". A missing input repeats the last movement for 6 ticks, then zero.
  Buttons are levels; the server derives edges, and a repeated input never makes an edge.
- A cast is captured at its input tick as `{origin, direction, tick, loadout}` from the server's player state, with
  cooldown and range checked then; raycast and `Blast` run when dequeued. Above 6 ticks in the queue the budget is
  exceeded instead. The owner block of the snapshot carries `lastCastTick` and `castRejected`.

## Region start-up and terrain agreement

- `RegionDesc{templateId, seed, playBounds, heightRange, TerrainSettings as raw float bits}` travels in `Welcome`.
  Both sides build the same generator and run memory-only. The **voxel size is a session parameter**: the server
  sets it (`TerrainSettings::voxelSize`, raw bits), and the client builds its `World` (`WorldConfig::voxelSize`, and
  the window size / full-height range derived for it) from `Welcome`, never from its own settings or command line.
  The integer operations (q = 1/16 voxel, island boxes in voxels) mean nothing between two different sizes, and
  `buildHash` does not cover it. The game's `--voxel-size` (view-distance stage 2) is an offline preview only: a
  session mode must refuse it or override it with the server's value. Template set-up is split (stage 3): edits and objects run
  **on the server only** (operations 1..k, replicated entities); lights are `EntitySpawn` kind `Light`; the player
  spawn point comes from the server.
- Build: `-ffp-contract=off` in `vortex_options` (Jolt keeps its flags); the same Clang major on both platforms.
  Under clang-cl the flag must be spelled `/clang:-ffp-contract=off`: the bare form is ignored with a warning only
  (checked with Clang 23 and `--driver-mode=cl`; the build now makes an unknown argument an error).
- Test, in `VortexCoreTests`, on both platforms, all checked in: (1) hashes of 64 fixed chunks for 3 seeds
  (`Terrain.GoldenChunkHashes`); (2) the raw float bit patterns of the noise functions and of the terrain height at
  4096 points per seed out to 1 km (`Terrain.GoldenNoiseBits`, `Terrain.GoldenHeightBits`). (2) is the one with
  power: (1) sees floats only after `floor` and threshold compares. If this fails on Windows, fixed-point noise is
  the answer (est. 5-8 days, changes all terrain); it is not built otherwise.
- `RegionDesc` is input from the network: the receiver checks `voxel::IsValid(TerrainSettings)` before building a
  generator (stage 3). Out-of-range settings make float-to-int conversions in the generator undefined.
- Run time: the server sends hashes of 64 probe chunks chosen from the seed. A client that disagrees, or that later
  fails 3 pristine hash checks, enters **server terrain mode**: every chunk of its window is requested on the bulk
  channel (est. 8-25 MB for a 154 m window).

### Known risks of terrain agreement after stage 0

Stage 0 was built and run on one Linux machine only. What is and is not established:

| Risk | State |
|---|---|
| The Windows build has not been made or run. | **Open.** Terrain agreement across platforms is unproven until the three golden tests are green there. Server terrain mode (stage 7) is the safety net. |
| The chunk-hash golden has little power against last-bit differences. Measured: built without `-ffp-contract=off`, every noise and height golden fails, but only 1 of 192 chunk hashes differs (a 1 km probe), and the first probe set (all within 131 m, 64 of them air) did not differ at all. | Closed by the float-bit goldens. The run-time check (64 probe chunk hashes from the seed) has the same weakness: it can pass while a few voxels per square kilometre differ. Such a client is caught only by `HashCheck` on a chunk it touches (stage 7). The float-bit goldens are therefore a release gate for every client build, not only a unit test. |
| clang-cl drops a bare `-ffp-contract=off`; MSVC and other compilers get no flag. | Handled in `CMakeLists.txt` for clang-cl (verified with the driver only, not with a full Windows build). Other compilers: configure warning; the goldens decide. |
| libm. The path from seed to voxel ids calls only `std::floor`, `std::ceil`, `std::abs`, `std::lround`, `std::min/max` (exact in IEEE 754, no rounding choice) and + - * /. No `sin`, `pow`, `exp`, `sqrt`. | No libm risk today. Any future generator (M4) that calls a transcendental function re-opens it; the goldens only cover the default settings. |
| Compiler freedom other than contraction: x87 excess precision, reassociation, `-ffast-math`, a different rounding mode set by a host application or driver. | Not possible on x86-64 with SSE and without fast-math flags, by the language rules; not measured on a second compiler version. The goldens would show it. |
| The goldens pin the **default** `TerrainSettings` and three seeds. A region with other settings (other scales, `voxelSize`) runs the same code on other inputs. | Accepted: the float operations are the same, so a build that passes is expected to agree for other settings too, but that is an inference, not a test. |
| Float-to-int conversion out of range (settings near zero or NaN, chunk coordinates beyond +-2^25). | `voxel::IsValid` exists and is tested; nothing calls it yet (stage 3 must, see above). |

## World operations

Unit `q` = 1/16 voxel. Sphere test on every side: `d = 16*v + 8 - c`, inside when `dot(d, d) <= r*r` in int64. Power
is `u16` in 1/16 hardness units. New `World::CarveSphereQ / FillSphereQ / SwapSphereQ / ClearMask`; the float
functions quantise and forward. Batch header per tick 10 B.

| Event (all carry `opSeq` order) | Payload | Bytes (est.) |
|---|---|---|
| `CarveSphere` / `SwapSphere` | kind, centre 3 x i32 q, radius u16 q, power u16 or from / to u8 | 17 |
| `FillBox` | kind, min, max voxels, id u8 | 26 |
| `Detach` | netId u32 (0 = client rubble), origin, dims, RLE **occupancy mask**, velocity | 30 + mask; est. under 200 typical, under 2 KiB at the 60,000-voxel cap |
| `ClearMask` | origin, dims, RLE occupancy mask (all crumbs of one blast; no netId, no velocity) | 19 + mask; est. 2.4 per crumb voxel, 908 for the worst measured blast (370 crumb voxels) |
| `ObjectModel` | netId, voxel offset 3 x i16, motion type u8, dims, RLE ids | 15 + RLE; est. under 100 for a crate |
| `EntitySpawn` / `EntityDespawn{reason}` / `EntityRest{tick, pose}` | see "Entity replication" | 10-60 + model |

1. A client never sends an operation. Cast is a button; the client plays the cast effect and changes nothing.
2. `sim::Blast` on the server: carve; `ExtractFloatingIslands` -> one `Detach` per island of at least
   `scene::MinPieceVoxels(voxelSize)` (the piece volume 0.016 m^3 in voxels of the session's size: 8 at 0.125 m), one
   `ClearMask` for all smaller islands together; `Scene::Damage` per caught
   object -> `ObjectModel` for the survivor, `EntitySpawn` per piece; impulses. Casts centred outside `playBounds`
   are rejected. Island and piece events count against the event budget: over budget, the cast waits.
3. A client applies events on receipt in `opSeq` order with the same `World::*Q` call. For `Detach` it clears the
   masked voxels and builds the piece's volume **from the ids it just removed** (worlds are equal at that `opSeq`).
   If a touched chunk has no base yet, `netId = 0` makes no rubble and `netId != 0` sends `ModelRequest` (answered
   on bulk). Terrain is never predicted; the price is a hole one round trip after the click.
4. From stage 2 every island of the piece volume or more has an event; the crumbs of a blast share one `ClearMask`, which a
   client shows as particles only (no piece, no body). Until stage 6 all islands are `netId = 0`: the server clears
   the voxels and keeps no body. Stage 6 adds replicated pieces.

## Chunk sync (late join, moving window, repair)

- Server: map `modified: chunk -> last opSeq`, fed with the chunk range of every event's box, never cleared.
- The client sends `WindowUpdate{minChunk, size}` at join and when `WindowRevision()` changes. The server answers
  `WindowAck{asOfOpSeq}` (events) and queues `ChunkData{coord, asOfOpSeq, RLE}` (bulk) for modified chunks, nearest
  first. **A chunk is encoded when it is dequeued**, so `asOfOpSeq` is current and the ring covers transit, not the
  queue. `ChunkRequest` answers go to the front.
- Client state per chunk: **no base** (entered the window, nothing received: accepts no events, not walkable),
  **based** (`ChunkData.asOfOpSeq`, or `WindowAck.asOfOpSeq` for a pristine one), **unsynced** (no store slot).
- One rule for both directions: **an event is applied to a chunk only if `opSeq > base`**, live or replayed. A chunk
  whose base is ahead of the stream ignores events until the stream passes it; a chunk installed
  (`World::SetChunkEncoded`) or reported by `TakeLoadedChunks` gets the ring events above its base, clipped to it.
- Ring: last 1024 events or 1 MiB, whichever is less (`Detach` keeps only box + mask). If the ring no longer covers
  a chunk's base, that chunk becomes a `ChunkRequest`; it is never replayed partially.
- Full store: a failed `Materialize`, `Adopt` or `SetChunkEncoded` marks the chunk unsynced: logged, counted, left
  out of `HashCheck`, not requested again until a slot is free. Client capacity = window dense chunks + carved
  uniform chunks + piece models; today 12,288 slots (433 MiB, bounded by GPU memory) against 8100 dense (measured).
- Repair: every 2 s `HashCheck` with 8 modified chunks, the chunks of the last operations and est. 4 pristine chunks
  within 12 m of that client's player, compared at the same stream position. Mismatch -> `ChunkRequest`.
- Est. after a 5-minute 5v5: 0.5-8 MB per join. Until stage 7 a joiner receives the whole event log instead.

## Player movement

- `PlayerInput{tick, moveX, moveZ (i8), yaw (u16), pitch (i16), buttons (u8)}`. `PlayerState{position, velocity,
  flags, cooldownTicks}`: 27 B, unquantised to its owner.
- `sim::StepPlayer(physics, terrain, character, state, input, tuning, dt)` holds every rule now split between
  `src/main.cpp` and `UpdateCharacter`. Its gate is `HasCollider(capsule)` = chunks loaded and a collider built at
  least once (no `installed >= exposed` term); on a client the chunks must also be based. The server's window is
  always loaded, so in practice only the client gates; it still sends the input.
- `SetCharacterState(id, position, velocity)` = set both, then `RefreshContacts`. Jolt's `SaveState` is not usable
  across the wire; what does not replay exactly (ground state after a refresh) is absorbed by the threshold and
  measured in stage 4.
- The client keeps 128 ticks of `{input, predicted state}`. A snapshot carries `ackInputTick`, the owner's state and
  `opSeqAtTick`; the client reconciles after it has applied events up to that sequence. If they are not there after
  250 ms it applies the position error through the visual blend only, skips replay until the events arrive, and
  counts it.
- Error under 2 cm (est.): ignored. Above: restore, replay the unacked inputs (13-15 `UpdateCharacter` calls at
  150 ms RTT, est. under 1.5 ms), blend the visual error out over 100 ms. Above 1.5 m: snap.
- Character collision in a session (mask in `PhysicsConfig`): Static and Dynamic only. On a client a replicated
  body is in the Dynamic layer **only while at rest**; moving pieces and all local rubble never block the predicted
  character. A push or a blocked step from a falling piece is a server result and arrives as a correction.
- Accepted inexactness, each with a logged metric: stale colliders and missing faces of an exposed neighbour after
  an edit; a moving piece is seen `interp delay + RTT/2 + buffer` late (175 ms at 150 ms ping = 1.75 m at 10 m/s),
  so contact with it is corrected, not predicted. Players do not collide in M1.

## Entity replication

- `scene::NetId{uint32_t}`: server-allocated, counting up, never reused; `net` keeps `NetId -> Entity`. Entity
  events are part of the `opSeq` stream. `EntityDespawn` is emitted from a registry `on_destroy<NetId>` observer, so
  every destruction path is covered.
- Server-owned: players, template objects, lights, pieces of at least `replicatedPieceVoxels` (512, est.). Scene
  changes: `Damage` takes that threshold and returns smaller pieces as volumes without entities;
  `VoxelObjectDesc` gains a layer and a `clientRubble` flag; `Scene::ReplaceModel(entity, model, offset, motion)`.
- Below the threshold: `EntitySpawn` with `netId = 0` (object fragments) or `Detach` with `netId = 0`. Each client
  makes local rubble: always with the `Debris` component and the existing `maxDebris` cap; under 48 voxels in
  `Layer::Debris` as today, larger in the new `Layer::Rubble` (collides with Static, Dynamic and itself). A survivor
  that falls below the threshold is despawned `toRubble` (with the start value a crate, 296 voxels, at its first hit).
- Client copy of a server body: kinematic, moved with `MoveKinematic` (so it has a velocity); in `Layer::Rubble`
  while moving, `Layer::Dynamic` at rest. `EntityRest{netId, tick, pose}`: samples with `tick <= restTick` are
  dropped; rest ends only on a newer sample with the awake flag.
- Caps: 256 live pieces, **64 awake** (est., derived from the snapshot budget). Eviction order: oldest sleeping, then
  smallest awake, sent as `EntityDespawn{toRubble}`: server and clients lose its collision at the same `opSeq`, each
  client keeps it as local rubble. A new island over the awake cap is emitted as `netId = 0`.
- Snapshot (est.; no bit packing, no deltas): header 13 B; own state 27 B; other player 17 B; awake body 31 B (netId,
  position, quaternion 4 x i16, velocity 3 x i16, flags). 1200 B hold about 36 bodies, by a priority that grows with
  time since last sent and shrinks with distance; 64 awake = at most 2 snapshots apart without loss.
- Interpolation: 8 `(tick, pose, velocity)` samples, Hermite; delay per entity = max(67 ms, 2 x its recent update
  interval), at most 200 ms; extrapolation up to one own update interval, then hold.
- Joiners: baseline `EntitySpawn`s on bulk, each with its own `asOfOpSeq` (encoded at dequeue). Client rules: a
  tombstone set of despawned ids for the session; a spawn for a tombstoned id is discarded; an `ObjectModel` for an
  unknown id waits for its spawn and replaces an older baseline model. `EntityCheck{count, xor of ids, xor of model
  hashes}` every 2 s; mismatch -> the entity list is re-sent. The player spawns once chunks within 32 m are based.
- Per player in 5v5 (est.): down 7 kB/s quiet, 24 kB/s with 20 awake bodies, cap 37 kB/s + events 24 kB/s; up 2.5.

## Server world without a camera

- Stages 0-8: the server window never moves. It is `playBounds` (snapped to whole chunks, asserted) plus **3 chunks**
  on all six sides (`sim::kServerWindowMarginChunks`; shred.md stage 1, owner decision 2026-11-12): the **flood box** of
  every cast centred inside `playBounds` (`floor(centre / 16) +- 96` voxels, `sim::FloodBox`) stays loaded, and with
  it the island box (radius + 24 voxels, at most 96 per side). Vertical extent from the template's `heightRange`, not 8
  chunks. A face flood that still meets a chunk that is not loaded is an error (`SimulationConfig::sessionServer`,
  `IslandSearchStats::attachedByUnloaded`, asserted zero in tests: `Blast.ServerFloodBoxAlwaysLoaded`;
  `Blast.FaceFloodOnAServerCountsAnUnloadedChunk` pins the error path), and so is an island box that is not loaded
  (`IslandSearchStats::islandBoxUnloaded`, logged, nothing searched, nothing deferred:
  `Blast.ServerNeverDefersAnIslandSearch`). With a fixed row range, rows below its bottom are the world's ground and
  rows above its top are air for the search: neither is ever "not loaded".
  (Since view-distance stage 1 the hook exists: `WorldConfig::fullHeight` + `heightRange` + `heightSnap` give a fixed
  row range on server and client alike, and the same range is the build ceiling; `ServerConfig` does not set them
  yet. Packing on the server is plain memory saving and does not change `WorldHash` or any operation's result.)
  `LoadBlocking` at start. Store capacity = dense count after loading x 2 (est.); a failed slot on the server is an
  error that ends the session, asserted zero in tests. Memory (dense chunks follow the surface, not the height):
  154 m est. 0.5-0.6 GiB with the 3-chunk margin (170 -> 178 m across, about +10 % dense chunks), 250 m est.
  1.1-1.3 GiB.
- `TerrainColliders::Update(playerPositions, 12 m)` every tick; `GuardBodies` already requests ground for every awake
  body. `scene.Update(fixedDt)` and `DestroyBelow` run on the tick.
- Stage 9 (1 km): `WorldConfig::loadPolicy = OnDemand`. A cell loads within 24 m of a player, for an event or for
  the **flood box** of every cast (`World::EnsureLoaded`, blocking; not only its island box) or for an awake body; on eviction a pristine chunk is dropped, a modified one
  rests as RLE. Est. 0.3-1.5 GiB for 20-40 players.

## Testing

New suite `tests/net` -> `VortexNetTests`, linking `VortexNet` only. Each side is one `Simulation` with a small window
and physics `threads = 1`; `LoadBlocking` and `TerrainColliders::Flush` remove async timing where a test wants that; a
manual clock advances all sides. `ImpairedTransport` wraps the loopback. `voxel::ChunkHash` hashes canonical content;
`WorldHash(box)` sums `mix(coord, ChunkHash)` over non-air chunks. No body poses.

| Test | Asserts |
|---|---|
| Golden generator hashes; integer sphere vs brute force; event encode / decode | Same constants on both platforms; boundary voxels; round trip |
| Same event list on two worlds with different load timing; blast at the region edge and at maximum height | Equal `WorldHash`; same islands as in the middle |
| **Scripted fight** (M1 acceptance): 2 clients, 600 ticks, 10 casts/s with islands and crates, 5 % loss, 80 +- 30 ms, one 200 ms outage | `WorldHash`, `NetId` sets and model hashes equal on all sides; own position within 5 cm after drain |
| Late join during 10 casts/s and object splits, bulk limited to 32 kB/s; walk in from outside; chunk still loading | Converges: equal hash, ids and models; event latency p99 under 250 ms during the join |
| Bulk delivered ahead of events with a carve + swap pair on the same voxels | Equal `WorldHash` |
| Generator broken on a client: everywhere; in one non-probe chunk | Server terrain mode; repaired before the player reaches it |
| Client with a deliberately small store receives fills until exhaustion | No repeated request for the same chunk; equal hash once slots are free |
| Blast at a chunk border beside a walking player; player under a falling replicated piece at 150 ms | Correction rate under a bound set in that stage; no snap |
| Collapse with 64 awake pieces, 5 % loss; cap reached with nothing asleep; split below threshold; late snapshot after `EntityRest` | Largest sample gap per body; bytes under the caps; ids equal; rest pose kept |
| +50 ms ping step; 3 lost input packets containing a cast; 150 ms server hitch; forced misprediction; forged packets | Margin recovers within 0.5 s; one cast; no hard resync; converges; no crash |

## Stages

| # | Built | Verified by | Days (est.) |
|---|---|---|---|
| 0 | `-ffp-contract=off`; `ChunkHash` / `WorldHash`; golden generator tests (chunk hashes and float bits); `VortexCore` + its tests built once on Windows | Golden tests green on Linux (and on Windows if a machine exists) and red in a build without the flag; the existing 299 tests pass; generator time before / after recorded | 2 |
| 1 | `VortexSim`; gather functions moved to the engine; `sim::Simulation` with a numbered tick; server scene config | Existing tests pass; game unchanged; a test steps a `Simulation` 600 ticks in a binary that links no Vulkan or glfw | 4 |
| 2 | Quantised edits, swap, `ClearMask`, `WorldOp` / `ApplyOp`, skipped / unsynced chunk report, `sim::Blast` (islands as `Detach` with `netId = 0`) + `PresentBlast`, `Scene::Damage` threshold; offline game on this path | Unit tests; two `Simulation`s fed the same events, islands included, have equal hashes; offline blasts look the same | 5-6 |
| 3 | `VortexNet`: loopback + `ImpairedTransport`, messages, `Server` / `Client`, `RegionDesc`, fixed server window with margin, probe hashes, event stream with byte budget, session file, `vortex_server`; split template set-up; join = full event log | In-process server + 2 clients, 200 server-side blasts that free islands, 5 % loss: hashes equal | 5-6 |
| 4 | `StepPlayer` with `HasCollider`, `SetCharacterState`, inputs, snapshots, time sync, prediction, reconciliation with the timeout branch, remote players | No correction above 2 cm on flat ground; border-blast, ping-step, lost-cast and hitch tests; correction rate logged | 7-9 |
| 5 | `EnetTransport` with `ReliableBytesInFlight`; game `--connect` / `--token`; cast capture, cooldown, cast queue | **Two people on two machines walk and see each other and each other's holes** (est. day 26-30) | 3 |
| 6 | `NetId`, despawn observer, replicated `Detach`, `ObjectModel`, `MoveKinematic`, `Layer::Rubble`, character mask, velocity snapshots with priority, `EntityRest`, both caps, `toRubble`, client rubble | Fight test with islands and crates; the collapse, cap, split, rest and falling-piece tests | 9-11 |
| 7 | Chunk sync: modified map, encode at dequeue, bases, ring, `TakeLoadedChunks` / `WindowRevision`, paced bulk, `HashCheck`, entity baseline + `EntityCheck`, server terrain mode; region larger than the client window | The whole test table on a 250 m region | 7-8 |
| 8 | Soak: `vortex_server` + two `vortex_bot` over real UDP through `ImpairedTransport` (incl. a 100 kB/s downlink during a join), hashes written at exit; one manual session with the real game client; every est. replaced; cast queue delay histogram | Hashes equal after a 10-minute run; numbers filled in | 4 |
| 9 | On-demand server world, `EnsureLoaded`, eviction; may move behind M2 | 1 km region, 20 bots: RSS and tick-time histogram; p99 tick under 16.6 ms | 5-8 |

### Stage 1 as built

Built: library `VortexSim` (`Vortex::Sim`: modules `vortex.scene` and the new `vortex.sim`; links Core, Physics, EnTT),
`sim::Simulation`, `sim::SimulationConfig`, `sim::ServerConfig`, `SceneConfig::bakeModels` / `markDebris`, the free
functions `vortex::GatherInstances(scene, out)` / `GatherLights(scene, out)` in `vortex.engine`, `Engine::Simulation()`
and `Engine::Tick()`, the test binary `VortexSimTests` (links `VortexSim` only) and a configure-time check
(`vortex_assert_gpu_free`) that stops the build when Core, Physics, Sim or `VortexSimTests` link a renderer, window or
UI library. Where the design was open or disagreed with the code, this was chosen:

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| What are the `focusPoints` of `Step(focusPoints)`? `World` has one window around one focus; the server window never moves, a client window follows the camera. | `focusPoints` are **collider** focus points only (the players; may be empty). `Step` and `UpdateStreaming(focusPoints)` advance loading **without moving the window**; `UpdateStreaming(windowFocus, focusPoints)` centres the window first. Nothing is remembered between calls. | The server places its window once with `World().LoadBlocking(centre)` and then only calls `Step`. A world whose window was never placed gets it with the minimum corner at chunk (0, 0, 0). |
| `Engine::Run` streams once per frame, runs 0-4 ticks per frame and advances lifetimes per frame with the frame time; the server order does all of it per tick. | `Step` = `UpdateStreaming` + the physics of `StepPhysics` + `UpdateLifetimes(fixedTimeStep)`, in the server's order. The three phases are public; the engine calls them where it called the parts before, so the game is unchanged. Only a physics phase counts a tick: `Tick()` = ticks simulated = the number of the next one. | **Server and offline / client still differ in when lifetimes run** (per tick vs per rendered frame, with the frame time). Nothing that both sides must agree on may hang on `scene::Lifetime` or on rubble clean-up; such removals are server results (`EntityDespawn`, stage 6). Game code of a tick (stage 4 `StepPlayer`, stage 2 `Blast`) runs between the streaming phase and the physics phase, where `OnFixedUpdate` is today. |
| "Its world never has a `saveDirectory`", but `EngineConfig::world` is a `WorldConfig` and the editor has a "Save world" button. | The rule is a property of a session, not of every `Simulation`: `ServerConfig` clears the directory; any other configuration keeps it. (A first version cleared it for every simulation, which silently disabled the map editor's "Save world"; review rejected that.) | An offline game or the map editor keeps its edits when `EngineConfig::world.saveDirectory` is set; the game sets none. **Stage 3:** a network client must run memory-only too - `net::Client` should refuse (or clear, with a warning) a world that has a `saveDirectory`. |
| "No `Debris` component" on a server scene. | New `SceneConfig::markDebris` (default true); false = the scene never adds the component (pieces split off, remnants, `VoxelObjectDesc::debris` objects). `sim::ServerConfig(config)` sets it together with `bakeModels = false`, `debrisRemnants = false`, `maxParticles = 0`. | Until `Scene::Damage` takes the replicated-piece threshold (stage 2 / 6), a server scene keeps every piece of 16 voxels and more as a body for good (in `Layer::Debris` below 48 voxels). A zero-capacity particle pool no longer logs "pool is full". |
| `ParticleSystem` is presentation, but `Scene` is constructed from one. | `Simulation` owns it (capacity 0 on a server). `Step` does not advance particles and does not call `Scene::BeginFrame()`; the owner that draws does both. | `Transform::previous` is meaningless in a simulation nobody draws. `sim::Blast` / `PresentBlast` (stage 2) is where particle spawning leaves the simulation path. |
| "`Engine` gains only a tick number and a tick-rate scale." | The tick number is there (`Engine::Tick()`, `sim::Simulation::Tick()`, `uint32_t`: 2.2 years at 60 Hz). The tick-rate scale is not built. | It belongs to time sync (stage 4), where its consumer is. |
| Where do the scene tests live now that the scene is in `VortexSim`? | `tests/engine/SceneTests.cpp` stays in `VortexEngineTests` (it also tests the gather functions, which need `vortex.engine`). `tests/sim` holds the `Simulation` tests and the server-scene tests. | `tests/net` (stage 3) links `VortexNet` only, as planned. |

Not part of stage 1 and untouched: `Engine::Explode` is still in the engine (stage 2 turns it into `sim::Blast` +
`Engine::PresentBlast`); the loop still drops time after 4 ticks per frame and a headless run still creates a Vulkan
device; `Simulation` has no notion of players, operations or a session.

### Stage 2 as built

Built: `voxel::SphereQ`, `QuantizeSphere`, `QuantizePower`, `InSphereQ`, `SphereVoxelBounds`, `voxel::EditReport`,
`voxel::VoxelMask`; `World::CarveSphereQ / FillSphereQ / SwapSphereQ / ClearMask` and a report argument on them and
on `FillBox` (the float `CarveSphere` / `FillSphere` quantise and forward); `ExtractFloatingIslands(..., bool*
searched)`; in `vortex.sim` the partitions `:worldop` (`WorldOpKind`, `WorldOp`, `OpResult`, `ApplyOp`,
`BuildDetachedPiece`) and `:blast` (`BlastDesc`, `AppliedOp`, `BlastResult`, `BlastPush`, `Blast`);
`SceneConfig::replicatedPieceVoxels`, `DamageFragment`, `DamageResult::fragments / toRubble`;
`sim::kServerReplicatedPieceVoxels` = 512 set by `ServerConfig`; `Engine::PresentBlast`. `Engine::Explode` is now
`PresentBlast(sim::Blast(...))`, and the game's scripted crate blast calls the two halves itself. Where the design was
open or disagreed with the code, this was chosen:

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| Rounding of the float -> q conversion. | `std::llround(x * 16)` in double (halves away from zero), centre clamped to the int32 range (+-2^27 voxels), radius to [0, 65535] q (4095 voxels), non-finite input = radius 0. Metres are divided by the voxel size in double first (`sim::Blast(sim, BlastDesc)`). | Quantisation runs only where an operation is made (server, offline); clients receive the integers. Every later decision uses only the integers: the island box is `floor(centre / 16) +- (radius / 16 + 24)` from the q values, plus the face flood inside the flood box `floor(centre / 16) +- 96` (shred.md stage 1); since stage 4 both are searched on the cell graph (same boxes). |
| Sphere bounds. | `SphereVoxelBounds` is the per-axis box `abs(16 v + 8 - c) <= r`, which can be one voxel larger than the tight bounds per side; the voxel test decides. Edit boxes are clipped to +-2^30 voxels (`kMaxEditVoxel`), `FillBox` too, so no `ChunkToVoxel(chunk) + 32` overflows. | `InSphereQ` returns early when one axis exceeds the radius, so squares stay below 2^34 for any `int` voxel. |
| How is `power` compared with a float hardness without rounding? | A voxel survives when `hardness * 16 > power` in float: multiplying by 16 is exact and so is converting a 16-bit integer, so the compare has no rounding. A per-carve table of 256 flags is built from it. | The material table must be equal on both sides (`buildHash` in `Hello`, stage 3). A hardness that is not a multiple of 1/16 (0.6 = 9.6 units) compares exactly: power 9 leaves it, 10 removes it (tested). |
| "Skipped / unsynced chunk report": which chunks? | Only chunks the operation would really touch (exact for sphere, box and mask), in visit order (y, z, x). `skipped` = not loaded: outside the window, `Unloaded` or `Loading`; `unsynced` = loaded but Empty / Uniform and the store had no slot. A chunk is all-or-nothing: a non-dense chunk is materialised at its first change, so a failed slot leaves it untouched. | Stage 7 turns a skipped chunk into replay from the ring or a `ChunkRequest`, an unsynced one into the "unsynced" client state. Today the offline game ignores the report, as it ignored the silent skip before. |
| `WorldOp` shape. | One struct with a kind (`CarveSphere = 1, FillSphere = 2, SwapSphere = 3, FillBox = 4, Detach = 5`; values are wire format) and the union of the fields; unused fields keep defaults so `==` compares exactly. `Detach` holds `min` = origin, a `VoxelMask` (bits packed 8 per byte in `VoxelVolume::Index` order, the bytes are the wire form before RLE), `velocity`, `netId`. `FillSphere` was added (templates, the editor brush); the swap writes `id` in place of `from`; `from == to` is a no-op, `from = 0` fills air. | Stage 3 encodes it as in the "World operations" table. No `opSeq` yet: `BlastResult::ops` is an ordered list and the server assigns sequence numbers when it emits them. |
| How does the blasting side clear an island? | `ExtractFloatingIslands` runs with `removeFromWorld = false`; each island becomes a `Detach` that is applied with `ApplyOp` (= `World::ClearMask`), the call a client makes. The piece is built from the ids that call removed (`BuildDetachedPiece`), on the server as on a client. | The old path removed island voxels with `SetVoxel` one by one; the new one is one edit per island, with the report. Islands are found by the single-threaded scan of `LabelComponents` in `Index` order and emitted in the order of their first voxel, so equal voxels in the box give equal operations in equal order (tested: two islands, scan order). |
| An island box that is not fully loaded. | The search is refused as before, but now visible: `BlastResult::islandsSearched = false` (new `searched` out-parameter). No `Detach` is emitted, so nothing diverges. Offline, `sim::Blast` asks the world to load the box (`World::RequestLoad`) and `sim::RetryIslandSearches` runs the search once it is loaded (view distance stage 1 review): its `Detach` / `ClearMask` are later operations of their own, not part of the blast's. | A server window always has the flood box loaded (3-chunk margin; shred.md stage 1), so a server never defers: an island box that is still not loaded there is an error (`islandBoxUnloaded`, logged), not a deferral. A client must not defer either: it applies the server's operations. |
| Who creates rubble for a `Detach` with `netId = 0`? | Nobody in the simulation: `sim::Blast` makes no entity for an island. `Engine::PresentBlast` creates the local rubble (`VoxelObjectDesc::debris`) from the voxels the Detach removed and starts it with `WorldOp::velocity` = `BlastPush` at the island's corner, the velocity change the old impulse gave (impulse = mass x push, so mass cancels). The corner is where the old code sampled it too: the rubble body sat at `VoxelToWorld(island.origin)` and `BodyState::position` is the shape origin, not the centre of mass (test `BodyPositionIsTheShapeOrigin`). Sampling every push at the centre instead (bodies, fragments, islands together) would be a gameplay change, not part of stage 2. | **Behaviour change offline:** a fresh island is no longer hit by the same blast's `Damage` (the old snapshot was taken after islands were created). Islands are pushed exactly as before. |
| What does `PresentBlast` get, and what stays in `Scene::Damage`? | `BlastResult` keeps per operation the `VoxelEdit`s it made (the carved voxels for particles, the island voxels for the piece), the quantised centre and radius, and the object fragments. Terrain debris particles, island rubble, fragment rubble, the explosion burst and the flash are in `PresentBlast`. `Scene::Damage` still spawns its own carve / loose-voxel particles into the scene's pool (empty on a server). | Moving object particles out of `Damage` belongs to stage 6, where a client makes them from the old and new `ObjectModel`. |
| `Scene::Damage` threshold: default, survivor, motion type. | `SceneConfig::replicatedPieceVoxels`, default 0 = off (offline: every piece of `minPieceVoxels` or more is an entity, unchanged). `ServerConfig` sets 512 (view-distance stage 3: 0.512 m^3 in voxels of the world, 262 at 0.125 m). A piece below it gets no entity, no body and no slot; it is returned as `DamageFragment{model, pose, linear, angular velocity}`. The survivor goes too (`toRubble`, `destroyed`) when it is below the threshold **and Dynamic** after the cut; a Static / Kinematic object that is only dented keeps its entity whatever its size. `sim::Blast` adds `BlastPush` to fragment velocities. | Stage 6 sends these as `EntitySpawn` with `netId = 0` / `EntityDespawn{toRubble}`. On a server today a crate (512 solid voxels in the sim test, 296 in the game) disappears at its first hit; offline nothing changes. |
| Where `Blast` runs in the offline game. | Where `Explode` ran: `OnUpdate` (mouse click; the headless script at frame 90), not in `OnFixedUpdate`. | Unchanged timing keeps the before / after comparison exact. The server runs `Blast` inside the tick, after streaming and before physics (stage 3 / 5 cast queue). |

Verified: 22 new tests (12 `QuantizedEdits.*` in `VortexCoreTests`; `WorldOps.*` 3, `Blast.*` 5 and `ServerScene.*` 2 new
in `VortexSimTests`), 385 in all, green; the sim tests repeated 10 times. The acceptance test
`Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` builds two simulations with different worker counts (1
and 3), applies the same set-up operations (boxes and a swap), runs two `sim::Blast`s on one (two islands, then a
plain crater) and only `ApplyOp`s the resulting four operations on the other: `WorldHash` over the whole window is
equal and complete, and each client piece has the server mask's voxel count. The same stream with the first carve
1/16 voxel larger, or with one `Detach` left out, gives a different hash. Headless game, editor and pause-menu runs
exit 0 under validation; the crate blast logs `7 -> 7 objects (0 terrain voxels, 0 island(s), 558 object voxels in 5
object(s))` - before the change the same 5 objects lost 118 + 100 + 44 + 252 + 44 = 558 voxels and no terrain - and
`Beam cut: 37 voxels removed, 1 new object(s)`, as before. Screenshots of the crate area (`--camera
14.3,7.0,-4.5,3.1416,-0.2`, frames 91, 95, 120, 200) before and after look the same; the resting places of the
pieces differ between runs of one build as much as between the builds. A temporary run (not kept) blasting the base
of a crystal column showed the island path offline: 1 island of 144 voxels detached, became rubble and fell.
**Not done:** no opSeq, encoding or transport (stage 3); `BlastResult::Report()` is not acted on anywhere; object
particles are still spawned inside `Damage`; the 512 threshold is untested in play (stage 6 tunes it).

**Crumbs (after stage 2, owner report).** A blast into rough terrain made one `Detach` per floating island, however
small: one blast produced ~180 records of 1-5 voxels each, and offline every one of them became a `VoxelModel` that
`CreateVoxelObject` then crumbled. The owner's rule: 1-5 voxel objects must not live in memory; under 16 voxels (0.1 m then; 0.016 m^3 since view-distance stage 3) is
particles.

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| Which islands are crumbs? | `voxelCount < scene::kMinPieceVoxels` (16), a constant exported by `vortex.scene` and the default of `SceneConfig::minPieceVoxels`. Since view-distance stage 3: `voxelCount < scene::MinPieceVoxels(world.VoxelSize())`, the piece volume 0.016 m^3 in voxels of the session's size (16 at 0.1 m, 8 at 0.125 m). | What the server sends must not depend on a scene's tuning, so `sim::Blast` uses the world's voxel size (a session parameter both sides hold bit for bit), not `Scene().Config()`. A scene with a larger `minPieceVoxels` still crumbles a larger `Detach` piece when it presents it. |
| How are they sent? | One new `WorldOpKind::ClearMask = 6` (`WorldOp::Clear(origin, mask)`: `min` and `mask` only, other fields default) per blast, after the carve and the `Detach`es, only if there are crumbs. `ApplyOp` = `World::ClearMask`, the call a `Detach` makes. | The mask's bits are set from each crumb's own voxels, so it does not depend on the order islands come in; the islands themselves come in scan order anyway. The world after it is the same as after one `Detach` per crumb (crumbs are disjoint). `BlastResult::crumbs` counts them, `IslandCount()` now counts `Detach` operations only, `CrumbVoxels()` the cleared voxels. |
| How is the union mask bounded? | **One mask over the smallest box holding every crumb**, no split per chunk. Since shred.md stage 1 a crumb proven by the face flood touches the island box face and reaches past it by less than `MinPieceVoxels`; since stage 4b a crumb the load check releases touches a judged node (whose voxels lie at most 7 voxels outside the island box) or is a load alone hanging on one, so it reaches past the box by less than `MinPieceVoxels + 7` (corrected in the stage-4b review). The raw mask is therefore at most `(2 * (radius / 16 + 24) + 2 * (MinPieceVoxels + 7))^3` bits (before: `(2 * (radius / 16 + 24))^3`, 45.6 KiB for the game's 1.2 m blast at 0.1 m); 864 KiB plus that margin at the hard limit (ExtractFloatingIslands refuses boxes over 192 voxels per axis). It lives only while the blast is applied and sent; the wire form is RLE. | One mask per chunk would turn the same crumbs into up to 27-64 records (a 72-voxel box spans 3-4 chunks per axis) with the same run-length bytes plus a header each, which is what this change removes. Applying a sparse mask is cheap: `World::ClearMask` skips every row of the box whose mask bits are all zero (`VoxelMask::AnyInRange`) before it reads a chunk id. Re-applying the largest measured mask (45 x 60 x 70 voxels) took 1-2 ms in the dev build, 18.7 ms with the old per-voxel test. |
| Bytes per blast. | Measured on the default terrain (44 blasts of 1.2 m, power 6, shot straight down on a 7 x 7 grid of points +-22 m around the origin, trees included). Wire bytes are estimates: header 17 B per carve, 30 B per `Detach`, 19 B per `ClearMask`, the mask as alternating zero / one run lengths in LEB128. | **Before:** 2051 operations (44 carves + 2007 `Detach`, every one of them under 16 voxels), worst blast 299 operations (370 voxels in 298 islands), est. 65 KB in all, 9.6 KB for the worst. **After:** 75 operations (44 carves + 31 `ClearMask`; no island reached 16 voxels), at most 2 per blast, est. 7.2 KB in all, 908 B for the worst (19 B + 889 B of runs; isolated crumb voxels cost two runs each, about 2.4 B per voxel). Largest raw mask held: 23.6 KiB. |
| Who shows them? | `sim::PresentBlast` (new, GPU-free; `Engine::PresentBlast` forwards to it): for a `ClearMask` it spawns `fx::SpawnVoxelBreak` for every `debrisParticleStride`-th (3) removed voxel, at most 400 bursts, in the voxel's material colour, each pushed with `BlastPush` at that voxel. No model, entity or body. | Before, a crumb got at least one burst and the push of its corner; now a 1-2 voxel crumb can get none when its voxels are not on the stride. The presentation moved out of `Engine` so the "no entity, no body" rule is unit tested; a stage-3 client calls the same function. |

Verified: 7 new tests (`QuantizedEdits.MaskRangeTestSeesExactlyTheBitsInTheRange`,
`QuantizedEdits.SparseClearMaskReportsOnlyChunksWithMaskedVoxels`; `Blast.CrumbsBecomeOneClearMaskAndLargerIslandsKeepTheirDetach`
(22 crumbs + one cap -> carve, one `Detach`, one `ClearMask` of exactly the 83 crumb voxels over their tight box),
`Blast.IslandOfSixteenVoxelsIsAPieceAndOfFifteenACrumb`, `Blast.OfflineCrumbsBecomeParticlesWithoutAnyEntityOrBody`,
`Blast.OfflineLargeIslandStillBecomesRubble`, `Blast.TwoSimulationsFedOperationsWithAClearMaskHaveEqualHashes` (equal
hashes; a second server makes the same operations; the stream without its `ClearMask` gives a different hash)) and
`WorldOps.ApplyOpIsTheWorldCallOfEachKind` extended; 392 in all, green. The measurement above was a temporary test,
not kept. Headless game, editor and pause-menu runs exit 0; the crate blast logs `0 island(s), 0 crumb(s) of 0
voxels` (unchanged: it carves no terrain). **Found, not addressed:** the island search itself took 70-200 ms per
1.2 m blast in that measurement, about all of `sim::Blast`'s time - and the `dev` build compiles without any `-O`
flag (`CMAKE_CXX_FLAGS_RELWITHDEBINFO` is empty in the cache, CMake 4.4), so every CPU time in this paragraph is
unoptimised.

**Changed by shred.md stages 0-1c (2026-11-12).** The island search proves face components by a bounded flood
inside the flood box (`voxel::ProveFaceComponents`), so the **result** of a search changes for the same voxels: a
piece that reaches past the island box is now one more `Detach` (or crumbs in the `ClearMask`) of the same blast. No
new `WorldOpKind`, no change of `Detach` or `ClearMask`: clients still only apply. The decision is integer-only, in a
fixed candidate and visit order, with a work budget counted in visits (240 000 per blast), and does not depend on what
is loaded outside the flood box (`Blast.FaceFloodIndependentOfLoadingOutsideTheFloodBox`,
`Blast.FaceFloodTwoSimulationsEqualHashes`, `Blast.FaceFloodBudgetIsCountedInVisits`, which reaches the budget with
1 and 3 workers at the same visit, 240 001). A proven piece can span up to
192 voxels per axis (raw mask up to 864 KiB, RLE on the wire); the watch tower is 8516 voxels at 0.125 m. A `Detach`
whose RLE mask exceeds the 16 kB event burst is still sent (correctness first) and must be logged by stage 3.
Offline only: a flood that meets a chunk that is not loaded defers the search; a carve that skipped loading chunks is
replayed on exactly those chunks as each one loads, before the late search (`World::CarveSphereQInChunk`; such a
replay is never sent; a client replays skipped ops in stage 7 instead).

**Changed by shred.md stage 4 (2026-11-12, before stage 3 by the owner's decision).** The server searches islands on
the cell graph cached per terrain chunk (`voxel::SearchIslandsOnGraph`, `sim::IslandSearchKind::Graph`, the default)
instead of the voxel search, in the same island and flood boxes, with the same emission order. The graph is exact
(every component of an 8^3 cell is a node; collapse.md's "more than 8 components merge" was not built), so the
**results** equal the stage-1 search except where a work budget runs out (the graph counts node visits,
`sim::kGraphNodeVisitBudget` = 32 768 per blast, the voxel search voxel visits, 240 000) and, when a chunk of the flood
box is not loaded, in the offline deferral and the reason a flood counts as attached (they flood in another order; on a
server both attach, so the operations are equal). That is a server decision;
clients still only apply, and no `WorldOpKind`, `Detach`, `ClearMask` or wire format changed. Tested: every stage-1
case gives equal operations and hashes with either search (`Structure.GraphFloatingEqualsFaceFlood`), every stage-1
test compares the two on its own voxels (`SimulationConfig::checkIslandSearch`; face islands only where no budget ran
out and every flood box chunk was loaded), and the graph never detaches what the
voxel search keeps on random terrain with liquids (`Structure.GraphNeverDetachesWhatTheVoxelSearchKeeps`). A cast now
costs 0.06-1.0 ms of island search on a quiet CPU, cold graph builds included (game terrain and flat ground; the voxel
search 3.3-19 ms). In game a cold chunk graph costs 70-120 us (15-27 us quiet; shred.md "Stage 4 as built"), and the
node budget does not bound builds: the first cast at an untouched site builds up to the island box's chunks (up to 64
for a 1.2 m blast) plus chunks its floods reach, so the cold worst case is about 64 x 120 us = 7.7 ms. **Stage 3's cast
budget must charge `IslandSearchStats::chunkGraphs` (graphs used, a function of the flood box voxels) at that cold
price**, not `graphsBuilt`: which graphs are cached depends on earlier casts, eviction and streaming, so a budget
charged by builds would put a replayed cast on another tick. Prebuilding graphs on a worker (shred.md owner question)
would lower the price. The graph cache is per `sim::Simulation` and invalidated by
`World::ChunkRevision`, so a server needs nothing new at stage 9 beyond the flood box it already keeps loaded.

**Changed by shred.md stage 4b (2026-11-13, before stage 3 by the owner's decision).** After the island floods the
server runs a load check on the same graph (shred.md Part 4b): the nodes of the island box that are not islands are
judged, what hangs on them inside the flood box is a load, what reaches the flood box face is support, and the largest
set whose weight no distribution fits the strength of what holds it (integer max-flow, unique whatever the visit order)
is released. The **meaning of the operation stream changes**: a blast may now emit more `Detach` operations (netId 0)
after its islands - a tower on a thin neck falls on the cast that judges it - and their crumbs join the one `ClearMask`.
**No new `WorldOpKind`, no change of `Detach`, `ClearMask` or of the wire format; clients still only apply.** Inputs: the
flood box voxels, the integer strength table (`Material::strength`, compiled-in until stage 3 hashes it into
`buildHash`, R6) and `VoxelSizeMicro(World::VoxelSize())`; a replayed server releases the same parts
(`Blast.NeckReplayGivesIdenticalOps`, `Blast.NeckReleaseTwoSimulationsEqualHashes`,
`Blast.NeckNothingOutsideTheFloodBoxIsRead`; `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` unchanged). A
released piece can be large: the watch tower is one `Detach` of 8558 voxels at 0.125 m (1-2 KB RLE, inside the 16 kB
event burst). On a server an exterior flood that meets a chunk that is not loaded is support and an error
(`neckUnloaded`); offline it is support for that cast too (counted, `neckLoading` / `neckBeyondWindow`) and a late
search runs the check again once the chunk has loaded - it never holds back the islands (stage-4b review). One check
releases at most 64 parts and 60 000 voxels (`kNeckMaxReleasedParts` / `kNeckMaxReleasedVoxels`; the rest stays for a
later cast), so one cast adds at most 64 `Detach` operations and one tower-sized mask per cast. The
check costs 0.13-0.73 ms quiet at a cached site, plus up to 1.8 ms of graph builds at a cold one on game terrain
(shred.md "Review of stage 4b"); its work is counted (above), so stage 3's budget charges it. **At its own budgets it
is far over the cast phase:** the preflow alone takes 5-6 ms at about 207 000 arc scans of its 262 144, est. 8-12 ms
with every budget full; stage 3 either charges it so that such a cast moves to a later tick, or the flow budget comes
down (shred.md open question 7). Strength-table drift between a server and its
replays changes decisions: the table must reach `buildHash` in stage 3.

## Out of scope for M1

Hit detection, damage, lag compensation, player proxy bodies and player-player collision (M2); the spell framework
and conjured matter; predicted carving; interest management; delta-compressed or bit-packed snapshots; encryption,
real tickets, NAT traversal, matchmaking; reconnect that resumes state; support-graph collapse (M3); fixed-point
noise; deterministic Jolt; ARM or non-AVX2 hosts; a wall-clock headless mode of `Engine`; a full Windows client build,
CI and the Windows-client / Linux-server correction-rate measurement (M6; in M1 only the Core golden test runs there).

## Rejected alternatives and why

| Rejected | Reason |
|---|---|
| Fixed-point noise now | Contraction is the only hazard and one flag removes it. A rewrite changes all terrain, and M4 replaces the generator anyway. Kept as the answer to a failed Windows golden test. |
| Server always sends terrain | Est. 8-25 MB per join and a continuous stream while walking. Kept as the fallback. |
| Per-sector operation logs; own reliability layer; GameNetworkingSockets; yojimbo | One global order with a base per chunk gives the same voxels. Reliability, fragmentation and congestion are est. 2-3 weeks; the others bring protobuf + OpenSSL or a second message framework. Reconsidered if question 2 is answered "public test". |
| Clients re-run island extraction or object splits | Needs identical, fully loaded voxels in the whole box; one missing chunk yields no island, silently. |
| Sending piece voxel ids in `Detach`; a fixed bulk rate | The client already holds the ids, a mask is several times smaller. A fixed rate queues megabytes in ENet's per-peer window ahead of live events. |
| Client character collides with moving replicated pieces | The piece is shown 175-300 ms late: guaranteed corrections and snaps inside a collapse. |
| Local rubble of 48-511 voxels in `Layer::Debris`; client store sized to the window cell count; evicting far chunks on a full store | Half-metre lumps would pass through each other. 649 MiB of CPU and GPU memory. Eviction is the memory milestone's work; "unsynced" is enough to stop the repair loop. |
| Predicted carving; lockstep or predicted bodies; 30 Hz simulation | Needs rollback of terrain, colliders and islands. Jolt is not cross-platform deterministic here. Movement is tuned at 1/60. |

## Owner decisions (2026-11-11)

The owner accepted the recommended answer to every question below:
1. M1 is accepted on a 250 m arena; the on-demand server world for 1 km zones moves behind M2.
2. Trusted testers only before M7: ENet with a plain token, no encrypted transport.
3. A Windows machine with Clang is to be used in stage 0 for the Core golden terrain test only
   (if none turns out to be available, server terrain mode is the safety net until M6).
4. Servers are x86-64 with AVX2 only; no fixed-point noise.
5. A hole appearing one round trip after the click is acceptable; predicted carving is judged in M2.
6. Pieces under the replicated threshold (start 512 voxels) are client-only rubble; tuned in stage 6.

## Questions as they were put to the owner

1. **Is a 1 km zone part of the M1 acceptance?** (a) M1 is accepted on a 250 m arena and stage 9 moves behind M2;
   (b) stage 9 is required. Recommendation: (a); the protocol is the same, only server memory and loading change.
2. **Will strangers connect before M7?** (a) Trusted testers only: ENet and a plain token; (b) a public test:
   encrypted transport with connect tokens (est. +8-12 days, replaces the transport of stage 5). Recommendation: (a).
3. **Is a Windows machine with Clang available now?** (a) Yes: the golden test runs there in stage 0; (b) no: terrain
   agreement stays unproven until M6, with server terrain mode as the safety net. Recommendation: (a), Core tests only.
4. **Will servers always be x86-64 with AVX2?** If ARM or older hosts are wanted, fixed-point noise is needed and
   belongs in stage 0, before terrain content exists. Recommendation: x86-64 with AVX2 only.
5. **Is a hole one round trip after the click acceptable in the first playable?** (a) Yes, predicted carving is
   judged in M2 playtests; (b) no: predicted carving with rollback joins M1 (est. +6-10 days). Recommendation: (a).
6. **Is it acceptable that lumps under about 0.8 m (512 voxels) are not shared?** Each client sees its own rubble; it
   blocks nobody and is not cover. (a) Yes, the threshold is tuned in stage 6; (b) no: a lower threshold costs server
   bodies and snapshot bytes (64 awake pieces is the budget). Recommendation: (a).
