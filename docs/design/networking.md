# Networking skeleton (M1)

Status: design decision, revised after two reviews. Stages 0-6 are built (see "Known risks of terrain
agreement after stage 0", "Stage 1 as built", "Stage 2 as built", "Stage 3 as built", "Stage 4 as built", "Review of
stage 4", "Stage 5 as built", "Review of stage 5", "Stage 6 as built" and "Review of stage 6"): the protocol runs
over ENet (UDP) between `vortex_server` and the game (`Shredborne --connect`), and in one process over loopback transports
for the tests; since stage 6 crates, terrain pieces of the replicated size and object pieces are server bodies that every
client copies. Stage 5's acceptance (two people on two machines) is the owner's to run; on one machine it is shown with a
server and two headless game clients on localhost. "Code" = read from the source at
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
  that falls below the threshold is despawned `toRubble` (with the start value a crate at its first hit: the crate is
  152 voxels at 0.125 m, see "Review of stage 6"; as built it is despawned `Broken`).
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
  `LoadBlocking` at start. Store capacity = dense count after loading x 2 (est.; as built: the window's cells capped
  at 2 GiB of chunks, "Stage 3 as built"); a failed slot on the server is an error that ends the session, asserted
  zero in tests. Memory (dense chunks follow the surface, not the height):
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
| 3 | **Built** ("Stage 3 as built"). `VortexNet`: loopback + `ImpairedTransport`, messages, `Server` / `Client`, `RegionDesc`, fixed server window with margin, probe hashes, event stream with byte budget, session file, `vortex_server`; split template set-up; join = full event log; the cast work budget (R5) | In-process server + 2 clients, 200 server-side blasts that free islands, 5 % loss: hashes equal | 5-6 |
| 4 | **Built** ("Stage 4 as built", "Review of stage 4"). `StepPlayer` with `HasCollider`, `SetCharacterState`, inputs, snapshots, time sync, prediction, reconciliation (timeouts counted; the visual-only timeout branch is not built, see "Stage 4 as built"), remote players | No correction above 2 cm on flat ground; border-blast, ping-step, lost-cast and hitch tests; correction rate logged | 7-9 |
| 5 | **Built** ("Stage 5 as built", "Review of stage 5"; the two-machine run is the owner's). `EnetTransport` with `ReliableBytesInFlight`; game `--connect` / `--token`; cast capture, cooldown, cast queue | **Two people on two machines walk and see each other and each other's holes** (est. day 26-30) | 3 |
| 6 | **Built** ("Stage 6 as built", "Review of stage 6"). `NetId`, despawn observer, replicated `Detach`, `ObjectModel`, `MoveKinematic`, `Layer::Rubble`, character mask, velocity snapshots with priority, `EntityRest`, both caps, `toRubble`, client rubble | Fight test with islands and crates; the collapse, cap, split, rest and falling-piece tests | 9-11 |
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

### Stage 3 as built

Built (2026-11-13): library `VortexNet` (`Vortex::Net`, module `vortex.net`, links `VortexSim` only; checked by
`vortex_assert_gpu_free` together with `VortexNetTests` and `vortex_server`) with the partitions `:transport`
(`ITransport`, `LoopbackTransport`, `ImpairedTransport`), `:wire` (`ByteWriter` / `ByteReader`, mask runs, events,
`RegionDesc`), `:messages` (`Hello`, `Welcome`, `Refuse`, `EventBatch`, `ServerStatus`), `:session` (session file,
`BuildHash`, probe chunks, `RegionHash`), `:server` (`net::Server`), `:client` (`net::Client`) and `:drill`
(`net::BlastDrill`, the scripted blasts of the acceptance); in `vortex.sim` the partition `:region` (`RegionDesc`,
`IsValid`, `RegionWorldConfig`, `RegionWindowFocus`, `MakeRegionGenerator`, `InPlayBounds`, `RegionTemplate`,
`PlaygroundTemplate`, `ApplyTemplateOps` / `CreateTemplateLights` / `CreateTemplateObjects`) and the cast work
budget (`sim::CastWork`, `CastWorkOf`, `OpWork`, `kCastWorkPerTick`); in `vortex.voxel` `MaterialTableHash`,
`HashMix64`, `kGeneratorGoldenHash` and `VoxelMask::SetRange`; the tool `vortex_server`
(`engine/tools/server`, example sessions in `tools/server/sessions/`). `VortexEngine` links `VortexNet` (nothing in
the engine uses it yet; the game's `--connect` is stage 5). Where the design was open or disagreed with the code as
it is after shred.md stages 0-4b, this was chosen:

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| "5 % loss" on a loopback, without ENet (stage 5) and without an own reliability layer (rejected above). | `ImpairedTransport` works on the sending side and keeps ENet's semantics: on the state channel a lost packet is gone, duplicates and reordering happen; on the reliable channels every lost attempt costs one resend delay (default 2 x delay + jitter + 20 ms) and a packet never overtakes an earlier one of its peer and channel (head-of-line blocking); an outage drops state packets and holds reliable ones. So loss on events shows as latency, which is what ENet gives. Seeded, driven by a clock function (a manual clock in tests). | The acceptance therefore proves that ordered, at-least-once delivery with arbitrary delays gives equal worlds (and that the state channel's loss, duplicates and reordering harm nothing); it does not test ENet. Stage 5's `EnetTransport` implements the same `ITransport`. `ITransport` gained `Disconnect` and Connected / Disconnected events; `LoopbackTransport::Connect(a, b)` links two endpoints. |
| What is `RegionDesc` exactly? | `{templateId, seed, playMinChunk, playMaxChunk (x / z, whole chunks, [min, max)), heightRange (metres), generator, flatGroundVoxels, terrain}` in `vortex.sim`; on the wire as integers and raw float bits (every terrain tunable, the voxel size included, and the 20 material ids). Two fields are new: `generator` (`Terrain` = 0, `Flat` = 1; wire values) and `flatGroundVoxels`, so tests and drills run sessions on flat ground. `sim::IsValid(region)` = `voxel::IsValid(terrain)` + known template / generator + non-empty play bounds of at most 512 chunks per axis inside +-2^24 + a finite height range of 1-128 rows + a flat ground inside it. | The receiver validates before it builds anything (`Client` on `Welcome`, `ParseSession` on the file). The client also refuses a `Welcome` whose material ids differ from its own registration. The height range is rounded to `sim::kRegionHeightSnap` (16 m) on both sides by the same `RegionWorldConfig`. |
| Which window does a stage-3 client have? | The region's window, like the server's: play bounds + 3 chunks, the full-height rows (`Client::MakeSimulationConfig` -> `sim::RegionWorldConfig`). `Client::Attach` loads it whole (`LoadBlocking`) before the first event is applied, so no operation is ever skipped on a client (counted: `ClientStats::skippedChunks` / `unsyncedChunks`, asserted 0). `MakeSimulationConfig` raises the store to `sim::RegionChunkCapacity` (the server's rule). An operation a client cannot apply in full fails the client (it leaves; there is no resync before stage 7). | The design's camera-centred client window needs stage 7's bases, ring and chunk requests; until then a client holds the whole region (fine for 96 m, heavy for 250 m). `Attach` refuses a simulation with a save directory, another voxel size (bit compare), another window or height range, or another material table (`BuildHash`); after loading, one whose window did not land on the region's corner (`sim::WindowIsRegions`), whose store filled (`storeFullLoads` / `storeFullDecodes`), or whose probe chunks differ from what the client's own generator makes (another generator, or a world edited before `Attach`; an edit away from the 64 probes is not seen until the hashes are compared). Stage 5: the game's GPU store mirrors `ChunkStore::Capacity`, so the engine cannot take the region's capacity blindly for a 250 m region (stage 7's smaller client window is the answer). |
| Join = full event log: how? | `Welcome` (region, spawn point, `logEnd`, the probe hashes) and then the whole log as `EventBatch`es on the events channel, ahead of the live stream on the same ordered channel. The server keeps every event encoded once (`WriteEvent` bytes) with its tick, and a per-client cursor (`sentUpTo`); a joiner's cursor starts at `logEnd`. A client buffers batches until it is attached, applies events in `opSeq` order, ignores duplicates and fails on a gap. **Paced** (review): every peer's cursor advances only while `ITransport::ReliableBytesInFlight(peer)` is below `ServerSettings::maxEventBytesInFlight` (32 kB, the design's bulk figure), so a joiner's catch-up waits in the server's log, not in the transport ahead of the live stream; `ServerStats::sendsHeld` / `maxBacklogEvents` count it. | The log grows for the whole session (about 200 KB per 100 drill blasts). Stage 7 replaces it by the ring + chunk data. Measured (`Session.ALateJoinIsPacedAndLiveEventsKeepFlowing`, a 48 kB/s shared uplink, a 61 KiB log at the join): the live client's worst event latency during the join 28 ticks paced against 71 unpaced; the joiner catches up in 100 ticks instead of 84. Over ENet the same limit keeps a joiner's catch-up out of its own per-peer reliable window. |
| Batches and event bytes. | One `EventBatch` per tick per client with the tick's events, split at 16 KiB (`ServerSettings::maxBatchBytes`; an event larger than that goes alone). Header 11 bytes: the design's 10 plus the message type byte. Event sizes: carve and swap 17, fill 16, box 26, `Detach` 29 + mask runs, `ClearMask` 19 + mask runs, `EntitySpawn` (light) 50. A `Detach` velocity travels as 3 x int16 in 1/256 m/s (+-128 m/s): presentation only, `ApplyOp` never reads it, the server keeps its float. Mask runs: alternating clear / set run lengths in LEB128, starting with a (possibly empty) clear run, self-delimiting; a decoder refuses masks over 1024 voxels per axis or 2^27 in all before allocating, runs that do not add up, and empty runs after the first; one batch decodes at most 2^27 mask voxels in all (16 MiB of bits, checked before each mask is allocated), and the server splits batches there too. | Event kinds 1-6 are `WorldOpKind`; `EntitySpawn` = 32 with `EntityKind::Light` = 1 (stage 6 adds objects and the other entity events). `kProtocolVersion` = 1 must change with any byte. Every decoder refuses trailing bytes; random and corrupted messages never crash one (`Wire.ForgedBytesNeverCrashADecoder`). |
| Event byte budget. | Credit of 400 bytes per tick (24 kB/s) capped at the 16 kB burst; **every** event charges it (template, script and admin operations from `Server::ApplyOp` never wait), a queued cast waits while the credit is spent; debt is bounded at -16 kB. An event larger than the burst is sent anyway and counted (`ServerStats::oversizeEvents`, logged). | Measured: the drill's events are 2-4 KB per blast (a tower `Detach` about 1.5 KB), so at one blast per 6 ticks the byte budget never delays a cast; in bursts of four it does (tested). |
| The cast work budget (R5): units and weights. | `sim::CastWorkOf(blast)` = search (`nodeVisits` x 2 + `chunkGraphs` x 200; the voxel search: `boxVoxels` / 8 + `floodVisits`, est.) + neck (2 x (`neckExteriorVisits` + `neckArcs` + `neckFlowWork` / 8 + 64 x `neckSplitRelabels` + `neckVoxels` / 64), shred.md's formula) + operations (100 each plus their box: carve / fill / swap / box voxels / 16, mask voxels / 128). A unit is about 0.1 us; `kCastWorkPerTick` = 60 000 (about 6 ms). Never charged: `graphsBuilt`, `chunksDecoded` (cache and streaming history) and the object half of a blast (`Scene::Damage` and the push depend on Jolt's float poses, so a replayed server could put a cast on another tick). | Calibrated once (`CastBudget.DISABLED_CalibrateWorkAgainstTime`, quiet CPU, -O2, this laptop): 300 casts - 240 on the built-in terrain at 0.125 m (0.6-2 m, power 6, every other one with an emptied graph cache) and the drill's three structures - took 1.09 ms per 10 000 units overall; terrain casts 22 000 units / 2.4 ms median, 37 000 / 4.0 ms the largest; per cast 0.40-1.66 ms per 10 000 units, the neck tower 1.35 (its 8558-voxel re-label and `Detach` are charged low). A least-squares fit gave 20 us per chunk graph and 0.18 us per unit of shred.md's neck formula; node visits were too few to fit (est.). A cold graph in a game that also streams and renders costs 70-120 us, but a server is a process of its own: the quiet price is the one charged. |
| How the queue spends it. | Per tick: `castCredit = clamp(castCredit + 60 000, -6 x 60 000, 60 000)` (never banked beyond one tick; a cast larger than a tick leaves a debt), then casts run in FIFO order while the work credit and the event credit are both positive; the work and bytes of a cast are charged after it ran. A cast that has waited `maxCastWaitTicks` (6) runs whatever the credits say (`castsOverdue`, logged when it really ran past them). | The tick a cast runs on is a function of the queue and the counts only: two servers with 1 and 3 workers, one with its graph cache emptied before every tick, run 24 casts queued in bursts on the same ticks with the same work, operations and hash (`CastBudget.ScheduleFollowsCountsNotTimeOrCache`). Stage 5 queues casts captured from inputs instead of `Server::QueueCast`. |
| shred.md open question 7: the flow budget. | `sim::kNeckFlowWorkBudget` lowered from 262 144 to **65 536** arc scans, as recommended, now that the cast budget charges `neckFlowWork`. | Every measured cast needs 2 600-13 000 (the game's neck release 7 055); spent means "released by a later cast" (R7). All tests and the `--neck-blast` / `--tower-blast` runs are unchanged. |
| `buildHash`. | `HashMix64` over `kProtocolVersion`, `voxel::kGeneratorGoldenHash` and `voxel::MaterialTableHash(materials)`. `kGeneratorGoldenHash` is a constant computed from the checked-in golden tables (chunk hashes with their probe coordinates, noise bits, height bits); `Terrain.GeneratorGoldenHashNamesTheGoldenTables` recomputes it, so regenerating the goldens forces a new value. `MaterialTableHash` covers, per world-palette id, the raw bits of `hardness` and `density`, `flags` and `strength[3]`: everything the carve, the island search and the load check read (shred.md R6). | R6's integer `MaterialPhysicsQ` table is not built: decisions still read the float `hardness` (exact compare, stage 2) and derive `densityQ` from the float density, so the hash covers those inputs instead. A build that rounds differently has the same `buildHash`; the probe chunks catch it. |
| Probe chunks. | 64 chunks of the region's window chosen from `RegionDesc::seed` (PCG32): a uniform column, and the column's lowest surface row or the one above when the generator classifies columns (else a uniform row); their pristine hashes (the generator's output, never the edited world) travel in `Welcome` with their coordinates. A client refuses a `Welcome` with more than 64 probes or one outside the region's window (network input: the generator never runs on such coordinates), then recomputes them with its own generator. A mismatch with the server is logged and counted (`ClientStats::probeMismatches`); the session goes on. `Attach` checks the attached world against the client's own probe hashes (above). | Server terrain mode (stage 7) is what a mismatch should start. On the default terrain fewer than a quarter of the probes are air; another terrain seed fails more than half of them (`SessionFile.ProbeChunksFindAGeneratorThatDiffers`). |
| Server window, store and memory. | `Server::Start` builds the world from `RegionWorldConfig` (fixed window at play bounds + 3 chunks, `sim::ServerConfig` on top), places it with `LoadBlocking(RegionWindowFocus)` and checks the corner. Store capacity: `session.chunk_capacity`, or `sim::RegionChunkCapacity` = the window's cell count capped at 2 GiB of chunks (`sim::kMaxRegionStoreSlots`, 58 140; the store allocates a chunk only when a slot is used, so a large capacity costs only its pointer table) instead of the design's "dense count x 2": a runaway world fills the store, the designed failure, before the process runs out of memory. A full store (`storeFullLoads` / `storeFullDecodes`), an operation the server's own world cannot apply in full, or an island search that meets an unloaded chunk (`islandBoxUnloaded`, `attachedByUnloaded`, `neckUnloaded`) marks the server `Failed` (logged); tests assert none. **`Failed` ends the session** (review): every peer is disconnected at once, the failing operation and everything after it are never sent, queued casts are dropped, `ApplyOp` / `QueueCast` refuse, new peers are disconnected on connect, and `vortex_server` stops and exits 1. Packing (`pack_hidden`, default on) uses `World`'s own rule: hidden and farther than 16 m from the window's centre. | **Measured** (`vortex_server --session arena250.session`, -O2, 7 generation workers): 63 x 63 chunks + margin, 44 rows at 0.125 m: preload **9.6-10.4 s** (three runs; view-distance.md's gate is 10 s: at the gate, not under it), 9888 dense resident + 31 522 Packed chunks, **48.5 MiB RLE** (gate 150 MiB), RSS 463-488 MiB (the estimate was 1.1-1.3 GiB without packing). The server's own resident rule (near players, recent edits) is not built: the 9888 dense chunks are the visible surface, which a server does not need. |
| Template split. | The game's plaza moved into the engine as `sim::PlaygroundTemplate` (template 1; template 0 = pristine terrain), returning `RegionTemplate{ops, lights, objects, spawn, anchor}` from a loaded world without changing it. A server applies `ops` as operations 1..k, sends each light as `EntitySpawn{netId, Light}` (net ids count up from 1, never reused), creates `objects` (the six crates) as server bodies and sends `spawn` in `Welcome`. The offline game builds the same template (`loadSite` = true loads the site as before) and applies it with `ApplyTemplateOps` / `CreateTemplateLights` / `CreateTemplateObjects`, in the order the plaza was always built. On a server the site search keeps only candidates whose plaza (+-8 m) lies inside the play bounds (`sim::PlaygroundArea`); `sim::IsValid` requires a playground's play bounds to hold the plaza at the origin; without a site (`RegionTemplate::sited` false: no level ground above the sea) the server does not start. | Crates are server bodies only until stage 6 (a client has none: `Session.TemplateRunsOnTheServerAndReachesClientsAsEvents`). The game's headless runs are unchanged (beam cut 13 voxels, HUD 149 quads, tower freed by shot 6 at 11.27x, neck release 8558 voxels at 5.61x). The probe and castle scripts stay game code. |
| Casts in stage 3. | Server-side only: `Server::QueueCast(sphere, power)` or `(BlastDesc)` (quantised on the server). Rejected, counted, unless `sim::CastFitsRegion`: `floor(centre / 16)` inside the play bounds and the window's rows (`sim::InPlayBounds`), a radius of at most `sim::kMaxCastRadiusQ` (72 voxels, 9 m at 0.125 m: the island box, radius / 16 + 24, then stays inside the 3-chunk margin; static_assert) and a carve box (`SphereVoxelBounds`) inside the window's rows (a carve through the floor or the ceiling meets cells that do not exist and would fail the server). | Inputs, cooldown and capture at the input tick are stages 4-5; they feed the same queue. |
| Handshake. | The client sends `Hello` on the transport's Connected event. The server answers `Refuse{Protocol / Build / Token / Full / Malformed}` or `Welcome`; a refused peer is disconnected once its refusal has left the transport (ENet's `disconnect_later`) or after `closeGraceTicks` (2 s), a peer that sends no `Hello` within `helloTimeoutTicks` (10 s) too (`ServerStats::peersTimedOut`). A client that fails (malformed message, gap, invalid `Welcome`, an operation it cannot apply) or is refused disconnects, which frees its slot. `Client::State`: Connecting, Joining, Welcomed (build a `Simulation` from `MakeSimulationConfig` / `MakeGenerator`, then `Attach`), Playing, Refused, Closed, Failed. | The client owns no simulation, so the game's `Engine` can keep owning one (stage 5 has to create the engine's world after `Welcome`). |
| Cast wall time. | `CastRecord::seconds` (`sim::Blast`), `ServerStats::castSeconds` / `maxCastSeconds` / `maxCastMsPer10k`, printed by `vortex_server`: logged only, never read by the schedule (R5). `CastRecord`s keep the last `castRecordLimit` (4096) casts; the per-kind counts are running totals in `ServerStats`. | Stage 8's histogram compares this with the work units (calibrated at 0.40-1.66 ms per 10 000). |
| Ticks and status. | `Server::Tick` = receive, streaming (no focus points yet), casts, `StepPhysics`, `UpdateLifetimes(fixedDt)`, the tick's batches, an unreliable `ServerStatus{tick, lastOpSeq}` on even ticks (stage 4's snapshot header replaces it). `Server::Advance(seconds)` runs due ticks, at most 2 per call, and keeps the rest (`Session.AdvanceCatchesUpTwoTicksAPassAndDropsNone`). | |
| Presentation on a client. | `ClientSettings::presentation`: applied **live** operations (`opSeq` after the `Welcome`'s `logEnd`; the join log, template included, is history) are grouped per cast (a carve and the `Detach` / `ClearMask` after it) and handed out by `TakePresentations` for `sim::PresentBlast`: particles, rubble from the removed voxels, flash. | Fragments of objects are not sent (stage 6), so a client shows no object damage yet. |
| `vortex_server`. | Loads a session file, runs headless; until stage 5 its clients are in-process (`--clients N`, `--join-step K`, `--loss`, `--delay-ms`, `--jitter-ms`, `--outage-ms`), each with its own `Simulation`, and `--drill N` runs `net::BlastDrill`. Exit 0 when every client's hash equals the server's and nothing failed. | Stage 8's `vortex_bot` and real UDP replace the in-process clients. |

Verified: 26 new tests (548 in all, five of them disabled measurements; 13 more with the review below, 561): `VortexNetTests` (links `Vortex::Net` only) -
`Wire.*` 7 (integers, floats and LEB128; every operation at its byte size; the light spawn; mask runs and their
refusals; `RegionDesc` bit for bit; messages with missing or trailing bytes; forged bytes), `Transport.*` 3 (loopback
order and disconnect; the impaired transport keeps 600 reliable packets in order through 5 % loss, jitter and a 200 ms
outage while the state channel loses, duplicates and reorders; the rate limit), `SessionFile.*` 4 (file round trip
and named errors, region validation, `BuildHash` sees a strength, probes), `Session.*` 5 (refusals, the client's world
from `Welcome` at 0.1 m and the worlds it refuses, the server window and rejected casts, the template split, tick
catch-up), `CastBudget.*` 2 + 1 disabled (the schedule above; equal work with a warm and an emptied graph cache:
`chunkGraphs` equal, `graphsBuilt` not), `SessionAcceptance.*` 2; in `VortexCoreTests`
`Terrain.GeneratorGoldenHashNamesTheGoldenTables` and `Materials.TableHashSeesWhatDecisionsRead`.

**Acceptance** (`SessionAcceptance.TwoClientsAgreeAfter200BlastsAt5PercentLoss`): an in-process server and two
clients, every link with 5 % loss, 40 ms + 0-30 ms delay, 1 % duplicates and 2 % reordering on the state channel and a
200 ms outage at 10 s; 200 server-side blasts of the drill, one every 6 ticks, on four sites of a flat 28 m region at
0.125 m, each rebuilding its site and shooting it: 67 caps cut off their stems (an island inside the island box, plus
a crumb in the `ClearMask` each), 67 bars cut off their posts (face-flood islands: the bar reaches past the island
box), 66 watch towers on 2 x 2 stone necks released by the load check. Every blast freed what it was built to free;
2259 events, 400.8 KiB, none over the burst, no cast delayed (one blast per 6 ticks stays inside both budgets); on the
server's link 23 reliable resends and 54 lost state packets (25 and 53 after the review: pacing changes the send pattern). Client A (joined at once, showing particles and rubble)
and client B (joined after blast 100 from the full log) end at the server's last `opSeq` with **equal `WorldHash`es**
over the whole region, no skipped or unsynced chunk and no probe mismatch. The same run as a binary (`vortex_server
--session drill.session --drill 200 --clients 2 --loss 0.05 --delay-ms 40 --jitter-ms 30 --outage-ms 10000,10200`)
exits 0 with the same counts (client B joined from a log of 1131 events / 205 KB); `playground.session` (the plaza
template on the built-in terrain, 30 drill blasts, two lossy clients) and `arena250.session` (40 blasts on terrain,
server only) exit 0 too. `SessionAcceptance.AStreamWithOneEventChangedGivesAnotherHash` shows the comparison can fail.

**Not done (later stages, or open):** ENet and the same `ReliableBytesInFlight` pacing over UDP (stage 5), inputs, snapshots
and prediction (stage 4), entity replication beyond lights (stage 6: crates and pieces are server-only, client
rubble only), chunk sync / bases / ring / `HashCheck` / server terrain mode (stage 7; a probe mismatch is only
logged), `vortex_bot` (stage 8), a client window smaller than the region, the game's session mode and its
`--voxel-size` refusal (stage 5), the server's own resident rule for packed chunks, R6's integer material table, the
event log's growth over a long session, and the preload gate (10 s) is met only at its edge on this laptop.

### Review of stage 3 (2026-11-13)

Three reviewers read the change; 17 findings (two were the same), each checked against the code. Fixed, every bug
first reproduced by a failing GPU-free test (13 new tests: `Client.*` 6 against a hand-written server endpoint,
`Session.*` 6, `Wire.ABatchDecodesABoundedMaskVolume`):

| Finding | Fix |
|---|---|
| A client with a store too small for the region (the default 8192 slots; the 250 m arena has 9888 dense chunks) kept playing while its world diverged: unsynced / skipped chunks were only counted. | `MakeSimulationConfig` raises the store to `sim::RegionChunkCapacity`; `Attach` refuses a world whose store filled while loading; an operation a client cannot apply in full fails it (and it leaves). `Client.AttachRefusesAStoreTooSmallForTheRegion`. |
| The probe check used the client's own generator, not the attached world: another generator or an edited world was taken. | `Attach` hashes the probe chunks of the loaded world (`net::CheckProbes(probes, world)`, one-chunk `WorldHash`es) against the client's own generator's (`RehashProbes`) and refuses a difference. `Client.AttachRefusesAWorldItsGeneratorDidNotMake`. |
| `QueueCast` took any radius: a cast over 72 voxels near the edge, or one whose carve crossed the window's floor or ceiling, failed the server. | `sim::CastFitsRegion` / `kMaxCastRadiusQ` (above). Reproduced: such casts gave `incompleteOps` and `islandBoxUnloaded`. `Session.CastsWhoseBoxesLeaveTheWindowAreRejected`. |
| A failed server kept ticking and sending, including the operation it had applied only in part. | `Failed` ends the session (above). `Session.AFailedServerEndsTheSession`. |
| A late joiner with presentation replayed the whole log (and the template's carve) as blasts. | Live events only. `Client.ALateJoinerPresentsOnlyLiveEvents`. |
| `Welcome` probe coordinates went to the generator unchecked. | At most 64, inside the window. `Client.AWelcomeWithProbesOutsideTheWindowIsRefused`. |
| The join log and the live flush were not paced per peer. | Per-peer cursor paced by `ReliableBytesInFlight` (above). `Session.ALateJoinIsPacedAndLiveEventsKeepFlowing`. |
| The default store capacity (2^20) bounded no memory; the `SessionConfig` comment described another rule. | 2 GiB cap, comment corrected (above). |
| The playground's site search ignored the play bounds. | `PlaygroundArea`, `IsValid` rule, `sited` (above). `Session.APlaygroundWithoutASiteDoesNotStart`, `SessionFile.RegionValidationRefusesWhatCannotBeBuilt`. |
| `Attach` did not check the window's corner. | `sim::WindowIsRegions`, shared with `Server::Start`. Not reproducible without breaking `RegionWindowFocus`; no test of its own. |
| Cast wall time was not logged (R5). | `CastRecord::seconds` and the `ServerStats` maxima (above). Asserted non-zero in the acceptance. |
| shred.md still gave the old flow budget in the algorithm section. | Annotated there (262 144 as built in 4b, 65 536 since stage 3). |
| The acceptance's duplicate check could not fail, and no test reached the client's duplicate / gap branches. | Split into real assertions (the transport never duplicates reliable packets, so `duplicateEvents` is 0); `Client.OverlappingBatchesApplyEachEventOnce` (5..9 after 1..7: 3 duplicates, each event applied once, batches before `Attach` kept) and `Client.AGapInTheStreamFailsTheClientAndItLeaves`. |
| A failed client kept its slot. | It disconnects (a refused one too). `Session.AFailedClientLeavesAndFreesItsSlot` (a corrupted batch; the next client gets the slot). |
| One 16 kB batch could decode gigabytes of masks. | At most 2^27 mask voxels per batch, checked before allocating; `reserve` bounded by the bytes left. |
| No test ran delayed or overdue casts with a client attached. | `RunBursts` (the schedule test) now has a client whose hash must equal the server's: it follows the interleaved build / queued-cast stream. |
| Peers that never joined were never removed. | Hello timeout and refusal grace (above). `Session.PeersThatNeverJoinAreDisconnected`. |
| Every `CastRecord` was kept for the session. | The last `castRecordLimit` (4096); per-kind totals in `ServerStats`. |

None was rejected. Not reproduced in a test of its own: the window-corner check (it cannot fail while
`RegionWindowFocus` is right) and the client's failure on an operation it cannot apply mid-session (it needs a store
that fills after `Attach`; the refusal at `Attach` is tested). `vortex_server` after the review: the drill session
exits 0 with equal hashes and the same counts (client B joined from a log of 1131 events / 205 KB, paced 57 times,
largest backlog 958 events); cast wall time 0.81 ms per 10 000 units overall, up to 2.6 ms for one cast (three
simulations in one process, not a quiet CPU); `playground.session` and `arena250.session` exit 0 (arena preload
10.6 s this run, store 58 140 slots).

### Stage 4 as built

Built (2026-11-13): in `vortex.physics` `PhysicsWorld::SetCharacterState`, `TerrainColliders::HasCollider`,
`PhysicsConfig::characterLayers` with `LayerBit` / `kDefaultCharacterLayers`; in `vortex.sim` the partition `:player`
(`PlayerInput`, `PlayerButton`, `PlayerState`, `PlayerFlag`, `PlayerTuning` + `IsValid`, `StepPlayer`,
`RestorePlayer`, `PressButtons`, `TakeCast`, `MoveDirection`, `LookDirection`, `PlayerBounds`,
`PlayerCharacterDesc`, the yaw / pitch / axis quantisers); in `vortex.net` the messages `InputBatch` (type 6) and
`Snapshot` (type 7, replaces `ServerStatus`, whose value 5 is retired), `Welcome::player`, `kProtocolVersion` = 2, the
partition `:prediction` (`TickClock`, `ClockSettings`, `RemotePlayers`), one player per joined client in
`net::Server`, prediction and reconciliation in `net::Client` (`TicksDue`, `StepPlayer`, `RunTick`,
`PredictionStats`), and `net::BotPilot` in `:drill`; `vortex_server`'s clients are players; the offline game moves
its player with `sim::StepPlayer`. No new library or target. Where the design was open or disagreed with the code as
it is after shred.md stages 0-4b and networking stage 3, this was chosen:

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| What a snapshot's state and `opSeqAtTick` mean. | A snapshot is made at the end of server tick `T` (after `StepPlayer`, the casts and physics of `T`) and carries `tick = T`, the owner's state after `T`, and `lastOpSeq` = the last event made by the end of `T` (the stage-3 status header folded in). The client compares it with its own state after tick `T` once it has applied events up to `lastOpSeq`; the replay of `T + 1 ..` then runs on terrain that holds every event of `T` and earlier (and possibly later ones that already arrived: accepted inexactness). | One number per snapshot. Header 21 bytes (17 before the review added `marginTick`), owner 27, other player 16: 48 B with one player, 64 B with two, 80 B with three. Every decoder refuses non-finite positions. |
| Inputs on the wire. | `InputBatch{firstTick, inputs}`: consecutive ticks, 7 bytes each (moveX, moveZ i8, yaw u16, pitch i16, buttons u8) after a 6-byte header. A client sends one packet every `inputSendInterval` = 2 ticks (30 Hz) with the inputs after the server's `ackInputTick`, at most the newest 8; the decoder takes 1-32. So every input is in about 4 packets. | 62 B per packet, 1.9 kB/s per player (design est. 2.5). The 8-input cap is what bounds the loss a cast survives: 3 lost packets (6 ticks) are covered, 5 are not. |
| What the server does with an input. | `t <= lastRealTick`: duplicate. `t <` the tick about to run: **late** - it refreshes the last input (the movement repeated from now on) and its button edges are derived then against the levels the repeats kept, so a cast in it is captured at once, exactly once. `t - now >= 16`: dropped (`inputsTooEarly`). Else queued (a second copy is a duplicate). `ackInputTick` = the newest input held; `inputMargin` = the newest input tick of a packet minus the tick about to run (negative: late), sent with that tick as `marginTick` (review: dropped too-early inputs included, and only from the packet with the newest last tick so far, so a reordered or duplicated older packet changes nothing; margins beyond +-1024 ticks are forged and ignored). A missing input repeats the last one with its levels (never an edge) for 6 ticks, then the player stands. A jump edge in a late input is carried to the next step the server runs (`jumpsFromLateInputs`; review). | The design's "an input for a tick already simulated only refreshes the last input" alone loses a short press that arrives late (the next on-time input no longer holds it); deriving the edge on the refresh is what makes `Movement.LostInputPacketsWithACastStillCastOnce` see one cast, and carrying the jump makes `LostInputPacketsWithAJumpStillJumpOnce` jump once (a few ticks after the client did: a correction of the jump's timing instead of a missing jump). |
| Time sync: what the margin targets, and the asymmetric control. | Target = 2 + (`packetTicks` - 1) + ceil(2 x jitter), jitter = running mean of the change between consecutive margins. The newest input of a packet is the one reported; the oldest new one in it is `packetTicks - 1` ticks less early, hence the term. Three late reports in a row: jump forward by `target - margin` at once; **three reports in a row 2 or more ticks short of the target: jump by what the least short of them misses (review: not in the design)**. Judged on the lowest margin of the last 16 reports: above the target, the clock runs at 0.98 (bleed); **below the target but not late, at 1.02 (catch-up: not in the design)**. Above both 8 and the target for 60 server ticks: hard resync. Reports about inputs made before the last jump or hold are ignored, and so are repeats about an input already reported (a snapshot repeats the last margin while no newer input arrived: an outage on the way up would otherwise look like three late reports); the input a report is about is `marginTick`, never the acknowledged input (a client too early has nothing acknowledged). | Without the catch-up a clock placed a little short stays short for good: measured on the flat-ground acceptance (5 % loss, 40 + 0-30 ms), the start estimate (RTT 0.11 s = 6.6 ticks) was 2-3 ticks short of the effective round trip (each message also waits for the receiver's next tick), margins sat at 0-2, **449 inputs were late and 20 corrections happened in 30 s**; with the catch-up and the packet term: 3 late inputs, 0 corrections. The start lead is now RTT + 2 ticks + the target. |
| Hard resync. | A **hold** of the surplus, `margin - target` ticks: the clock runs that many ticks fewer (no rewind). Only a margin above both `resyncAbove` and the target counts (review: with jitter the target can exceed 8, and `margin - target` wrapped to about 4e9 ticks). | Inputs already sent stay valid on the server, so a hold costs no misprediction; a rewind would re-predict ticks whose inputs the server already queued (and ignores as duplicates). Never triggered in the acceptance (the 150 ms hitch peaks at 12 for well under a second). |
| The first snapshot. | Prediction starts with the first snapshot whose events are applied: the character is restored to its state, the client runs ahead by the lead, and the ticks in between are predicted standing (what the server does without inputs) and never sent. Before that `StepPlayer` does nothing. `Client::Attach` creates the character at the `Welcome` spawn and cooks the colliders around it (`Update` + `Flush`); the server creates its character at the join and does the same, so the gate holds neither side at the start. | A joiner whose join log is long starts predicting late (it waits for the log); its clock then jumps. |
| Reconciliation, blend, snap. | Error = distance of the positions (velocity is not compared: the next snapshot sees what it changed). Under `ignoreError` (2 cm): left alone. Else restore (`RestorePlayer` = `SetCharacterState`), replay the kept inputs of `T + 1 ..` with `StepPlayer` (casts in a replay are ignored), and add the visual jump to an offset blended out linearly over 6 ticks (100 ms); at or above `snapError` (1.5 m): snap, no offset. 128 ticks of history; an older snapshot is `tooOld`, one at or past the predicted tick is taken as it is (`behind`). Measured replays: at most 16 ticks, 0.11 ms (three clients in one process). | `PredictionStats` keeps the counts, an error histogram (< 1 mm, < 2 cm, < 5 cm, < 20 cm, < 1.5 m, more), replay ticks and time, clock jumps / holds / bleed / catch-up ticks and margins; `vortex_server` logs them per client with the corrections per minute. |
| The timeout branch. | Snapshots wait in arrival order (at most 32); the newest whose events are applied is compared and the older ones dropped; a snapshot that has waited `eventWaitTicks` (15, 250 ms) since it arrived is given up: counted (`eventTimeouts`), no replay (review: the first version kept one snapshot and restarted its wait with every newer one, so a stall never timed out and nothing was compared). **The design's "applies the position error through the visual blend only" is not built**: an offset that is blended back to zero shows the error for 100 ms and then nothing, so it would not move the drawn player anywhere useful; the next snapshot whose events have arrived corrects for real. | Zero timeouts in every acceptance run (in the border test 21 snapshots had to wait for their events first, `waitedForEvents`). Stage 7's bases change when events can be missing. |
| Casts from inputs in stage 4 (the lost-cast test needs them; capture, cooldown and the cast queue are listed under stage 5). | The minimum: a cast edge that `TakeCast` accepts (`PlayerState::cooldownTicks`, `PlayerTuning::castCooldownTicks` = 15, predicted by the client like the movement) is captured by the server from its player's state after this tick's step: a ray from the eye along `LookDirection(yaw, pitch)` into its world, at most `castRange` (60 m); the hit is queued (`sim::CastFitsRegion` as for every cast) as a blast of `castRadius` (1.2 m) and `castPower` (6), recorded with `CastRecord::player` = slot + 1. A ray that meets nothing casts nothing (`castsMissed`). The client returns the edge as `PlayerStepResult::cast` to show the effect and changes nothing. | Deviations from "Time and ticks", left to stage 5: the ray is cast when the cast is captured, not when it is dequeued; a late input is captured on the tick it arrives, not "at its input tick"; one fixed loadout from the tuning. The cooldown is in the state because `PlayerState` carries it in the design. |
| The tuning. | `sim::PlayerTuning` (speeds 3.8 / 7 / 4.6 m/s, capsule 1.8 x 0.3 m, eye 1.62 m, slope 50 degrees, step 0.35 m, the cast) travels in `Welcome` as raw bits (45 bytes); a client checks `sim::IsValid` and predicts with it, never with its own. So do the tick length and gravity (16 bytes, review): `MakeSimulationConfig` takes them and `Attach` refuses a simulation whose tick, gravity (bit compare) or character mask differ. `kProtocolVersion` = 3. | Not in the design; a tuning that differed would make every step a correction. |
| What the predicted character collides with. | `PhysicsConfig::characterLayers` (new; default Static, Dynamic, Debris); in a session **Static only on both sides**: `sim::kSessionCharacterLayers`, set by `sim::ServerConfig` and `Client::MakeSimulationConfig`, checked by `Attach` (review: the first version set it on the client only). | The design gives the character mask to stage 6 ("Static and Dynamic only; a replicated body Dynamic only while at rest"). Without it now a presenting client's own rubble - which the server's character never meets - blocked its prediction: in the drill, client 0 (presentation on) had **100 corrections in 20 s**, client 1 (off) one; with the mask client 0 has 6. Stage 6 adds Dynamic for replicated bodies at rest, to the shared constant. The server's players walk through the template's crates (server bodies, unknown to clients until stage 6) as their clients do. |
| The gate. | `TerrainColliders::HasCollider(box)` = every chunk of the box grown by one voxel loaded and built at least once (`ChunkReady` without `installed >= exposed`). `IsReady` (bodies, `Engine::TerrainReady`) is unchanged. **Colliders at a step (review):** before every player step the server, and before every predicted or replayed step and every restore the client, call `TerrainColliders::FlushBox(sim::PlayerReach(feet))` (the capsule + 1 m): the chunks there whose newest content is not installed are cooked on the calling thread at once, so what a character collides with follows the edits applied, not worker timing. | Neither side ever held the player (`heldTicks` 0). Measured: the border-blast test gives the same numbers whether cooks are waited for after every tick or left to the workers (0 corrections, largest error 10.4 mm); on `playground.session` (slopes) client 0 went from errors of 1 mm - 2 cm in most comparisons to bit-equal. The cost is one chunk cook per chunk edited under a player, on the thread that steps it. |
| Remote players. | Every snapshot lists every other player (slot, feet, yaw of its last input, flags); a slot missing from a newer snapshot has left. `RemotePlayers` keeps 8 samples per slot and draws linearly at server time minus max(4 ticks, 2 x the slot's mean sample interval), at most 12 ticks; past the newest sample it extrapolates for one interval, then holds. Server time is `net::ServerTime` (review: "newest snapshot tick + client ticks since" stepped back on every late snapshot): real time from `TicksDue` plus a smoothed offset to the snapshots' ticks; it runs at 0.9-1.1 x real time towards that estimate and only moves forward. | Measured: a remote player is drawn at most 0.99 m from where the server has it now (the snapshot's way down plus 4-6 ticks of interpolation delay, at up to 7 m/s, jumps included). No `Hermite` (players turn at once); no extrapolation for bodies (stage 6). |
| Driving a client. | `TicksDue(seconds)` is the client's clock; per tick the owner runs the streaming phase, `Poll`, `StepPlayer(controls)`, the physics and lifetime phases - `RunTick(controls)` is exactly that on the attached simulation (colliders around the predicted player). Messages are only accepted on their channel: on the state channel only snapshots, anything else is counted (`malformedSnapshots`) and ignored; a forged event batch there no longer fails the client (it did, found by `Movement.ForgedPacketsAreHarmless`). | **The engine is not driven by it yet**: `Engine::Run` keeps its own accumulator; stage 5 (`--connect`) must take the tick count from `TicksDue` (the design's "tick-rate scale"). |
| The offline game. | `src/main.cpp` moves its player with `sim::StepPlayer` (the gate is `HasCollider`; jump is an edge of the Space level, with a latch so a tap shorter than a tick counts; the fly-mode landing uses `RestorePlayer`). | The headless run's player ends 0.69 m further along the tower wall its scripted walk ends against (`Player feet at (16.94, 4.36, -3.54)`, before `-2.85`): the two gates never differed in that run (checked tick by tick with a temporary log); what remains is the quantised yaw (4e-5 rad) amplified by the slide along the wall. `--climb` is unchanged. |

Verified: 23 new tests (584 in all, six of them disabled measurements). `VortexPhysicsTests`:
`PhysicsWorld.CharacterStateIsRestoredAndReplaysClose`, `PhysicsWorld.CharacterLayersChooseWhatBlocksACharacter`,
`TerrainGuard.HasColliderIgnoresUncoveredFacesWaitingForARecook`. `VortexSimTests`: `Player.*` 4 (quantisers and
tuning validation; edges and cooldown; walk / run / jump / held; two simulations with 1 and 3 workers stay bit-equal
over 600 ticks of a scripted walk across chunk borders with jumps, and 0.0 m apart with a restore every 30 ticks).
`VortexNetTests`: `Clock.*` 7 and `RemotePlayers.*` 1 (margin sequences: jump after three late reports and ignore
older and repeated reports, bleed and catch-up, a hold after a second above 8 but none for a hitch-shaped sequence, jitter in the
target, the packet term; interpolation, extrapolation, leaving), `Wire` extended (inputs and snapshots bit for bit,
sizes, refusals, forged bytes), and the acceptance `Movement.*` 7 + 1 disabled, all over the seeded
`ImpairedTransport` with one manual clock (server ticked by `Advance`, every client by its own `TicksDue`):

| Test | Link | Result |
|---|---|---|
| `NoCorrectionAbove2cmOnFlatGround`: two bots walk, run, strafe, turn and jump for 30 s | 5 % loss, 40 + 0-30 ms, 1 % duplicates, 2 % reordering | **0 corrections**, max error 0.0000 m in 1628 comparisons (the prediction is bit-equal), 0 held ticks, 2 late inputs on the server; remote player within 0.96 m (0.99 before `ServerTime`). The test now asserts that state packets were lost, duplicated and reordered on the links (review: its "the network interfered" check passed on a perfect link). Disabled `FlatGroundCorrectionRateOverSeeds`, 10 seeds x 2 bots x 60 s: **1 correction in 32 326 comparisons (0.05 per player-minute)**, 36 late inputs - a correction needs a late input at a tick where the controls changed. |
| `BlastAtAChunkBorderBesideAWalkingPlayer`: walking along z 1.9 m inside chunk column x = 0, a 1.2 m blast at the border x = 0 every 20 ticks a little ahead (40 blasts), then 40 at x = 0.6 (the crater rim under the capsule) | 40 + 0-30 ms, no loss (loss is the flat test's) | 0 corrections, 0 snaps, 0 held ticks; bound set here (review: was one per blast): at most 4 rim corrections in 40 blasts and a largest error under 2 cm; `BlastAtAChunkBorderWithoutWaitingForCooks` runs it with cooks left to the workers, same result (0 corrections, 10.4 mm). **Errors of 1-16 mm** (below the threshold) in most comparisons once blasts start, also before the first blast reaches the player: a temporary log showed the server's player hopping by about 5 mm where the client's did not while walking along x = 2.03 m; cause not isolated (likely Jolt's contact order over colliders created at different times on the two sides). After the review (colliders flushed at every step) the comparisons before the first blast are bit-equal and the largest error is 7 mm beside and 10.4 mm at the rim, the same with and without waiting for cooks: what remains follows the blasts, most likely the client predicting on the uncarved ground for a round trip (terrain is not predicted). |
| `PingStepMarginRecoversWithinHalfASecond`: +50 ms on both links after 10 s, -50 ms after 20 s | 40 ms | The step took the lowest margin to 0 (the target, 4, absorbed 3 ticks: no input late). Review: with the 2 % catch-up alone the margin was back at target - 1 only **177 passes (2.95 s)** after the step, and the test did not check it. With the short-margin jump: first short report 13 passes after the step (the step's own transit), one jump of 4 ticks, back at target - 1 **31 passes (0.52 s)** after the step = 18 passes after it was first seen; asserted: at most 30 passes after it is seen, 36 after the step. The step down was bled off. 0 hard resyncs, 1 correction, 0 snaps. |
| `LostInputPacketsWithACastStillCastOnce`: a cast pressed for 2 ticks while an outage drops exactly 3 input packets (a client with target 1, so the copies arrive late) | 30 ms | 3 packets lost; the cast reached the server in a late input: **one cast** (`castsFromLateInputs` 1), queued and run once, the client applied its carve (equal hash) and showed one cast. |
| `ServerHitchNeedsNoHardResync`: the server does not run for 9 ticks, then catches up 2 per pass | 40 + 0-10 ms | Margins up to 12 (under the 8-for-a-second rule); 0 hard resyncs, 0 inputs dropped, 0 corrections, 0 snaps. |
| `ForcedMispredictionsConverge`: a wall built 0.5 m ahead of the server's walking player (the client is past it), then a 5 m teleport (`Server::TeleportPlayer`, new admin call) | 50 ms | Wall: 1 correction (6.3 cm), both sides end against the wall within 2 cm, no further corrections. Teleport: 1 snap (5.0 m), nothing left to blend, converged. |
| `ForgedPacketsAreHarmless`: 600 passes of random bytes and absurd input batches from the client's endpoint, random bytes to the client | 30 ms | 599 malformed packets and 1157 too-early inputs counted; the session goes on; the prediction converges once the forgeries stop. |

`vortex_server`: the CLAUDE.md drill command exits 0 with **hashes equal** and the stage-3 counts (67/67, 67/67,
66/66); its two walking, jumping clients: client 0 (presentation) 6 corrections in 20 s (18 per minute: the drill
rebuilds structures and fills boxes around them, one round trip late on the client), client 1 one; no snaps, no
timeouts, no resyncs. With `--clients 3 --join-step 50 --bot-casts 90` the bots cast 32 times through the drill
(26 run, 6 outside the play bounds rejected), 3-7 corrections per client in 20 s, hashes equal.
`playground.session --drill 30 --clients 2 --bot-casts 120` on the built-in terrain: hashes equal, 2 corrections per
client in 3 s, most comparisons 1 mm - 2 cm apart on slopes. The game's standard, editor, `--climb`,
`--tower-blast --after-fill` (freed by shot 6, 11.27x) and `--neck-blast` (8558 voxels, 5.61x) runs exit 0 under
validation.

**Not done (later stages, or open):** ENet and the engine driven by `TicksDue` with the game's `--connect` (stage 5);
capture at the input tick for late inputs, loadouts, the ray at dequeue (stage 5); the character mask's Dynamic layer
for replicated bodies at rest, "player under a falling replicated piece" and object fragments (stage 6); the
"chunks must be based" gate (stage 7); the visual-only timeout branch (see above; stage 7, where bases change when
events can be missing, must decide it); the sub-2 cm errors after blasts near a player (not isolated, see the border
test); the M1 scripted fight's "own position within 5 cm after drain" (it needs stage 6's
replication); a correction-rate measurement between a Windows client and a Linux server (M6).

### Review of stage 4 (2026-11-13)

Three reviewers read the change; 15 findings, three of them reported twice (the clock that goes blind, the event-wait
timeout, the character layers), each checked against the code. Every bug was first reproduced by a failing GPU-free
test (9 new tests, 593 in all, six of them disabled measurements). Fixed:

| Finding | Fix |
|---|---|
| A client 16 or more ticks early never recovered: the server dropped its inputs, `ackInputTick` stood still, and the clock took every report about it as a repeat, so neither the bleed nor the hard resync ran (a lead sized from ENet's 500 ms default RTT, or a ping drop of 270 ms, would do it). Also the margin was the last-arrived packet's, paired with the wrong tick under reordering. | `Snapshot::marginTick` (header 21 bytes, `kProtocolVersion` 3): the newest input of the packet with the newest last tick so far, too-early ones included; the client reports that tick, never the ack. Ticks a client did not make (at or past its next tick, or margins beyond +-1024) are ignored on both sides (`ForgedPacketsAreHarmless` pinned the marginTick at a forged 0xFFFFFFFF before that). `Movement.AClientFarTooEarlyResyncsAndItsInputsAreTakenAgain`: joined at 1 s RTT, dropped to 40 ms; before: margin 32 for good, 1184 inputs dropped, 0 resyncs; after: one hold, no input dropped in the last 5 s, margin back at the target, both sides agree. |
| The hard-resync hold `margin - target` wrapped to about 4e9 ticks when jitter raised the target above 8 and the margin was 9-10: the clock stopped for good. | The timer runs only while the margin is above both 8 and the target; the hold is the surplus. `Clock.AHoldNeverExceedsTheSurplusWhenTheTargetIsAboveTheResyncBound`: margins 9 / 13 alternating; on the old rule the 31st report has target 10 and margin 9, a hold of 2^32 - 1 (worked through; the test's run against the old code fed non-increasing input ticks by mistake and showed a 27-tick hold instead, so the wrap itself was not run). |
| Replays and predictions stepped against colliders installed when a worker finished, on both sides; the acceptance hid it by waiting for every cook after every tick. | `TerrainColliders::FlushBox` before every player step (server) and every predicted, replayed and restored step (client), on `sim::PlayerReach` (capsule + 1 m). `Movement.BlastAtAChunkBorderWithoutWaitingForCooks` (the same numbers as with waiting: 0 corrections, 10.4 mm), `TerrainGuard.FlushBoxInstallsTheNewestCollidersOfABoxAtOnce`. It did not reproduce as corrections in the border test (0 before and after); the measurable effect is on slopes: `playground.session` client 0 from 1 mm - 2 cm errors to bit-equal. |
| Tick length and gravity were not part of the session. | In `Welcome`; `MakeSimulationConfig` takes them; `Attach` refuses others (bit compare). `Session.ClientTakesTheServersTickGravityAndCharacterLayers` (1/30 s and -12.5 m/s^2 on the server). |
| Remote players were sampled at "newest snapshot + ticks since", which stepped back on every late snapshot. | `net::ServerTime` (above). `ServerTime.MovesOnlyForwardWithJitteredSnapshots`, `Movement.RemotePlayersAreDrawnMovingForwardOnly` (before: 2 steps back in 150 passes, up to 6.3 cm). |
| The 250 ms event-wait timeout could not fire while snapshots kept arriving (each restarted the wait), so reconciliation stopped silently while events lagged. | A queue of waiting snapshots, aged from arrival (above). `Movement.SnapshotsWhoseEventsLagAreGivenUpAndCounted` (one batch in flight at a time: before 0 timeouts, after 33, and comparisons resume once the events are in). |
| Server and client characters collided with different layers (the server's with crates and pieces its clients do not have). | `sim::kSessionCharacterLayers` on both sides (above), asserted in the session test. |
| The ping-step acceptance ("margin recovers within 0.5 s") was not tested and was not met: the 2 % catch-up took 177 passes (2.95 s). | The short-margin jump (above): 31 passes after the step, 18 after it was first seen. **Owner question:** the test asserts "within 0.5 s of the first short report" (at most 30 passes) and 0.6 s from the step, because the step's own transit (13 passes at 40 + 50 ms) passes before any clock can see it. If "0.5 s from the step" is meant literally, the 3-report confirmation would have to shrink to 2 (more jumps from jitter). |
| The stages table said "with the timeout branch". | Row 4 corrected; the visual-only branch is left to stage 7. |
| The border-blast bound (one correction per blast) was 40x looser than measured. | At most 4 rim corrections in 40 blasts and a largest error under 2 cm. |
| The flat-ground "the network did interfere" check passed on a perfect link (every packet repeats acknowledged inputs). | Asserts losses, duplicates and reordering on the links. |
| A late input's jump edge was spent without a jump. | Carried to the next step (above). `Movement.LostInputPacketsWithAJumpStillJumpOnce` (before: the server's player never left the ground). |

None was rejected. After the review: flat ground still 0 corrections (1 in 32 318 comparisons over 10 seeds, 38 late
inputs); the drill (`--drill 200 --clients 2`, 5 % loss) exits 0 with equal hashes and 67/67, 67/67, 66/66, clients 4
and 4 corrections in 20 s (6 and 1 before; the late joiner now counts 7 given-up snapshots that were invisible before),
no snaps or resyncs; with `--clients 3 --join-step 50 --bot-casts 90` 5, 12 and 10 corrections, hashes equal;
`playground.session --drill 30 --clients 2 --bot-casts 120` 0 and 2 corrections, hashes equal. The game's standard,
editor, `--climb`, `--tower-blast --after-fill --frames 700` (shot 6, 11.27x) and `--neck-blast` (8558 voxels, 5.61x)
runs exit 0 under validation, with the numbers of before.

### Stage 5 as built

Built (2026-11-13): ENet 1.3.18 (pinned by tag in `cmake/Dependencies.cmake`, built from source as the static
library `vortex_enet`, linked **PRIVATE into `VortexNet` only**; a new configure check `vortex_assert_no_network` stops
the build if Core, Physics, Sim or `VortexSimTests` ever link it, and `vortex_assert_gpu_free` covers `vortex_enet`
too); in `vortex.net` the partition `:enet` (`EnetTransport`, `EnetSettings`, `EnetStats`, `kDefaultPort` = 27015,
`ParseEndpoint`), `ITransport::Flush`, cast capture at the input tick with the ray at dequeue, `CastRejection` and the
snapshot owner block's `lastCastTick` / `castRejected` (`kProtocolVersion` = 4; 5 since the review), `Client::LastCast`,
`ClientSettings::minFreeStoreSlots`, `MakeSimulationConfig(base, storeLimit)` and `Attach(simulation, whileLoading)`;
in `vortex.engine` `EngineConfig::simulation`, `EngineConfig::windowFollowsCamera` and `Engine::SetTickSource` (the
design's "tick-rate scale"); the game's session mode (`src/Session.cpp`: `Shredborne --connect HOST[:PORT] --token
HEX`); `vortex_server --listen [PORT] --bind ADDR --seconds S --exit-when-empty` and `--enet` (its bots over UDP). No
new library or executable target. Where the design was open or disagreed with the code as it is after shred.md
stages 0-4b and networking stages 3-4, this was chosen:

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| How ENet is built and where it may go. | `FetchContent` of the v1.3.18 tag with its own CMake skipped (it sets global include directories); the nine C files compiled into `vortex_enet` with the feature macros its configure step would find on Linux (`-w`, like ImGui). Only `src/net/internal/Enet.cpp` includes `<enet/enet.h>`, in its global module fragment; the interface `Enet.cppm` knows no ENet type. | The game links it through `VortexEngine -> VortexNet` (static libraries), so "net library, server, game" holds by construction; `vortex_assert_no_network` pins the other half. A Windows build adds `ws2_32` / `winmm` (in the CMake, not built yet: M6). IPv4 only (ENet 1.3). |
| Channels and flags. | One ENet channel per `net::Channel` (3). State: unreliable **sequenced** (ENet drops an older datagram that arrives after a newer one) with `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT`; events and bulk: reliable. ENet's bandwidth limits are off (0). | Sequenced is a subset of what `ITransport` allows on the state channel (loss, reordering, duplicates), so the client's stale-snapshot and the server's input rules are untouched. A snapshot larger than one datagram (stage 6's bodies) is fragmented unreliably instead of becoming reliable. |
| `ReliableBytesInFlight` (the pacing of stage 3). | **Exact**: every reliable packet carries a free callback; the bytes count from `enet_peer_send` until ENet releases the packet (all its fragments acknowledged, or the connection reset). So it covers both what waits in ENet's queues and what is sent but not acknowledged. | The server's per-peer pacing (32 kB) and the refusal close (`ReliableBytesInFlight == 0`) work over UDP as on the loopback; measured on localhost with 5 % receive loss: peak 19 kB in flight for 400 x 40-3000-byte messages, back to 0 once acknowledged. Each peer's record lives until ENet has reset the peer (its Disconnect event, or the host's destruction), because queued packets point at it. |
| When datagrams leave. | `Send` queues inside ENet; `Poll` services the host (sends, receives, one event at a time); new `ITransport::Flush` (default no-op; `ImpairedTransport` releases what is due and flushes the inner one) sends at once. The server's owners flush after every pass, the game after every frame and while it waits. | Without a flush a reply would wait for the next `Poll`, one tick later. |
| Disconnect semantics. | `Disconnect` is graceful (`enet_peer_disconnect_later`: the refusal or the last batch still goes out) and reports `Disconnected` to the caller at once, like the loopback; later traffic from that peer is ignored. The remote side sees ENet's disconnect. A connect that never succeeds ends in `Disconnected` for that peer after ENet's timeout; `Client` now goes from Connecting to Closed on it (it ignored it before: no server peer yet). The destructor disconnects every peer and waits up to 0.5 s for the acknowledgements. | Peer timeout: 10 s minimum / 30 s maximum (`EnetSettings`; ENet's 5 / 30 s), so a client that loads its region does not look dead; a crashed client keeps its slot up to 30 s. |
| Loss "where possible" over a real socket. | `EnetSettings::receiveLoss`: ENet's intercept hook drops that fraction of the **incoming datagrams** (seeded) before ENet parses them, so ENet's own resends, acknowledgements and RTT react to real loss. Delay, jitter, duplicates and reordering stay `ImpairedTransport`'s, layered above (`vortex_server --enet` sets the loss below ENet and the rest above it). | Tests, drills only; 0 in a session. No root, no `tc`. |
| A blocking load while connected. | `Client::Attach(simulation, whileLoading)`: with a callback the window is loaded in `World::Update` steps (2 ms apart) and the callback runs between them (the game and the bots poll their transport there); `LoadBlocking` then only finishes. Without it, as before. | The playground region loads in 1.3-2 s on this laptop (dev build), the drill region in 0.2 s; a 250 m region about 10 s (stage 3's preload): within the 10 s ENet minimum even without the callback, but the callback keeps the join log flowing meanwhile. |
| Open issue of stage 3: the game's GPU store mirrors `ChunkStore::Capacity()`, the region's capacity is its cell count. | **The smallest safe answer:** the client store is capped at the game's GPU store (`MakeSimulationConfig(base, storeLimit)`: min(region cells, 12 288 slots = 433 MiB, as offline)), and `Attach` refuses a region whose loaded window fills that store or leaves fewer than `ClientSettings::minFreeStoreSlots` free (the game asks for 2048, est.: 600 rubble slots plus chunks that casts materialise or decode) with a clear error ("the region is too large for this client: a smaller session region, or networking stage 7's client window"). The session region for stage 5 is `playground.session` (96 m of play, 120 m window, 44 rows): **2084 of 12 288 slots** after loading (buried chunks packed on the client too). No client window smaller than the region (stage 7). | The 250 m arena was not tried with the game: the server holds 9888 dense chunks there, which with the 2048 headroom is 11 936 of 12 288 (at the edge, est.); stage 7's camera-centred client window is the real answer. An operation that later finds no slot still fails the client (stage 3 rule; no resync before stage 7). |
| Cast capture (design: "captured at its input tick as {origin, direction, tick, loadout} from the server's player state, with cooldown and range checked then; raycast and Blast run when dequeued"). | A cast edge the cooldown accepts is queued as {eye, look direction, input tick, loadout 0 (the tuning's radius and power)}. On time: the eye of the state after that tick's step. **Late input** (stage 4 took it on arrival): judged at its own tick - the cooldown the player had after that tick (a ring of the last 32 states per player) and the current one must both be 0; the cast is captured from the state of that tick, and the cooldown is set to what it would be had the cast been taken then (`castCooldownTicks - ticks since`), exactly what the owner predicted. The **ray** is cast when the cast is dequeued to run (not at capture), on the world the casts before it left; a ray that meets nothing within `castRange` (`Missed`) or a hit outside the play bounds / rows (`Outside`) costs nothing and the queue goes on. "Range" is therefore checked at dequeue (it needs the world). The owner block of every snapshot carries `lastCastTick` (input tick of the newest cast edge taken from that player) and `castRejected` (`None` / `Cooldown` / `Missed` / `Outside`), 5 bytes: owner block 32 B, `kProtocolVersion` 4. | Reproduced first (tests run against the stage-4 code): `Movement.ACastOneCooldownAfterALateCastStillRuns` (a late cast and one exactly a cooldown later: before, the server started the cooldown on arrival and refused the second - 1 run, 1 on cooldown, the client showed 2; now 2 and 2) and `Movement.ACastsRayIsCastWhenTheCastRuns` (a server cast queued just before at the same spot: before, the player's ray met the uncarved surface at y = 1536 q; now the crater wall at 1413 q). `Movement.TheOwnerHearsWhyItsCastDidNotRun` (a cast at the sky: `Missed` for that tick, nothing carved). The work budget and the six-tick limit are stage 3's queue unchanged: a cast that waited 6 ticks runs. Lag compensation of the eye beyond the own state is not built (M2). |
| The engine driven by a client clock (stage 4's open point). | `Engine::SetTickSource(f)`: per frame the engine asks `f(wall seconds, 8)` (the client's `TicksDue(seconds, maxTicks)`) how many fixed ticks to run, at most 8, and runs every tick it gets (review of stage 5: the first version asked for up to 30 and dropped all above 8; the clock now keeps the rest for the next frames); its accumulator then only gives the view an interpolation fraction. A headless run passes the wall clock too (its own frames keep the fixed step). Without a source, the offline loop is unchanged. `EngineConfig::simulation` takes the session's whole `SimulationConfig` (`MakeSimulationConfig`), `windowFollowsCamera = false` keeps the region's window where it is. | Per tick the game (`OnFixedUpdate`) does `Client::Poll` + presentations + `Client::StepPlayer`, between the engine's streaming (once per frame, not per tick: colliders are flushed under the player before each step anyway) and its physics; a frame that runs no tick (hold, bleed) still polls. Lifetimes still run per frame (stage 1's note: nothing replicated may hang on them). |
| The game in a session. | `Shredborne --connect HOST[:PORT] --token HEX`: connect, wait up to 15 s for `Welcome` (a refusal or no answer is a clear error and exit 1), build the engine from the session, `Attach` (polling while it loads), play. Controls as offline: mouse look, WASD, Space, Shift, LMB = the cast button (a level plus a latched edge, like jump), V, Esc menu ("Leave the session"). The client shows a puff where its crosshair meets the terrain when its prediction accepts a cast; the hole and everything else come with the server's operations (`PresentBlast`). Other players are figures with a gold torso at `Client::RemotePlayers()`. HUD: server, slot, ping, players, "Cast not run: ..." from `castRejected`, "Disconnected from the server". **Refused in a session** (exit 2): `--voxel-size` (the server's voxel size is the session's), `--editor`, `--camera` and every offline script; the crate key, free flight and the editor are not there (they would change this side's world only). | Crates are server bodies until stage 6, so a client does not see the plaza's crates (they block nobody: the character mask is terrain only). A headless session run walks (slot 0) or stands (others), casts three times, then faces another player in first person, and logs `Session: slot N applied opSeq A of the server's B; world hash H` at exit; `vortex_server` logs `Session: ... client(s) left; server at opSeq A, ... world hash H` when a client leaves. |
| `vortex_server` over UDP. | `--listen [PORT]` (default 27015; `--bind ADDR`, default every interface): wall clock, `Advance` per pass; ends after `--seconds S`, when every client that joined has left (`--exit-when-empty`), or on Ctrl-C / SIGTERM; a status line every 10 s; a port that cannot be opened is exit 2. `--enet` (implied by `--listen`) gives the in-process bots their own UDP sockets to 127.0.0.1 with `--loss` as receive loss below ENet. The drill and the bots work in both modes; a pass that runs no tick no longer queues a drill step twice (found when the first wall-clock run freed 25/67 mushrooms: steps were queued on every pass of the same tick). | The bots of one process share its thread: a late joiner's load stalls the server and the other bots, which shows as clock jumps / resyncs that a separate process does not have (below). |

Verified (all on this laptop, the dev build: -O2 -g): 7 new tests, **600 in all (six disabled measurements)**:
`Enet.ParsesEndpoints`, `Enet.ReliableMessagesArriveOnceInOrderThroughRealLoss` (400 reliable messages each way, every
7th 3000 bytes, and 400 state messages, through 5 % receive loss on both sockets: all reliable ones once, intact and in
order; state ones in increasing order only; 16 of 264 / 7 of 247 datagrams dropped; 0.6 s),
`Enet.DisconnectsAndFailedConnectsAreReported` (graceful disconnect seen on both sides, the message queued before it
delivered; a connect to a closed port given up after 1.5 s with 0.2 / 1 s timeouts; an unresolvable name refused),
`EnetSession.TwoPlayersWalkCastAndAgreeOverUdpWithLoss` (the GPU-free acceptance: server and two clients on three
UDP sockets of 127.0.0.1, 5 % receive loss on each, 9 s of wall clock: pilots walk, run, jump and cast every 50 ticks,
9 drill structures shot; 29 casts run (9 drill, 20 players), 9 `Detach`; both clients at the server's last `opSeq`
with **equal `WorldHash`**, each saw the other; corrections 0 and 4 of about 240 comparisons, no snaps), and the three
`Movement.*` cast tests above; `Wire.MessagesRoundTripAndRefuseTrailingOrMissingBytes` extended (the owner block at
32 bytes, an unknown rejection refused). The whole suite takes 20 s with `ctest -j4`.

Runs:
- **Localhost session (the stage-5 acceptance on one machine):** `vortex_server --session playground.session --listen
  27015 --exit-when-empty` and two headless game clients (`Shredborne --connect 127.0.0.1:27015 --token ... --headless
  --validation --frames 750 / 600`, the second one second later) on the same UHD 620: both exit 0 under validation,
  both log **world hash c41e49f35666b97f at opSeq 68 of the server's 68**, and the server logs the same hash at
  opSeq 68 when each leaves (earlier runs while the headless script was tuned: equal too, at opSeq 61 with one client
  and 67, 68, 68, 69 with two). Each client: 3 casts shown, 6 blasts shown (its own 3 and the other's 3), 1 remote
  player seen; 0-4 corrections in 25 s over the runs (max error 0.18 m: walking into its own fresh crater, which the
  prediction meets a round trip late), no snaps, no clock jumps or resyncs, margin 3-10 ticks, RTT 31-44 ms
  (localhost plus the clients' frame time: ENet acknowledges when it is serviced), 2089 / 12 288 store slots. The
  second client's screenshot shows the first one's figure across the plaza and its crater. Server: 1676 snapshots of
  68 B, 6 casts from inputs, none late or rejected. A wrong token is refused before the engine starts (exit 1), a port
  nobody answers on gives "no answer ... within 15 s" (exit 1), `--voxel-size` with `--connect` exits 2, and stopping
  the server with Ctrl-C ends a playing client's session at once (its HUD line, exit 1 for a headless run).
- **A game client joining late over UDP:** `vortex_server --session drill.session --listen 27016 --drill 300
  --clients 1 --bot-casts 90` and a headless game client 9 s later: it joined from a log of 1034 events / 187 KB, applied
  2392 events, 0 corrections, 0 clock jumps or resyncs; the server's bot ended with equal hashes, the drill freed
  100/100/100.
- `vortex_server` drill command of CLAUDE.md (loopback, simulated clock): **hashes equal**, 67/67, 67/67, 66/66, the
  same events and bytes as before; clients 4 and 0 corrections.
- The same drill over UDP (`--enet --clients 3 --join-step 50 --bot-casts 90 --loss 0.05 --delay-ms 40 --jitter-ms
  30`, 20 s of wall clock), three runs: **hashes equal** every time, 67/67, 67/67, 66/66 (the first wall-clock run
  freed 25/67: the drill-step bug above), 31-33 player casts, no budget overruns; about 100 of 2070 datagrams dropped
  at the server, 44-58 per client. Corrections 12, 1, 12 in the last run (the drill rebuilds structures around the
  bots, terrain changes one round trip late on a client); in another 7, 16, 2 with 2 resyncs and 2 snaps for the late
  joiner, whose load and catch-up stalled the one thread of the process (the separate-process late join above had
  none). In the GPU-free `EnetSession` test client 0 makes one hard resync in every run (margins up to 20): most likely
  the lead it took from the RTT right after its join while the same thread was busy with the other client (not
  isolated; the game clients in their own processes never resynced).
- The game's standard, editor, `--climb`, `--tower-blast --after-fill --frames 700` (freed by shot 6, 11.27x) and
  `--neck-blast` (8558 voxels, 5.61x) runs exit 0 under validation with the numbers of before (beam cut 13 voxels, HUD
  149 quads, the player's feet at (16.94, 4.36, -3.54) or (16.93-16.95, 4.36, -3.49): that walk ends sliding along a
  wall and varies between runs of one binary): offline play is unchanged.

**Not done (later stages, or open):** the two-machine acceptance itself (the owner's: CLAUDE.md and
`design/game-ui.md` have the steps); windowed play of a session (mouse lock, the menu's "Leave the session", the HUD
with a real window: owner checks); a session on the 250 m arena with the game (est. at the edge of the GPU store, see
above; stage 7); replicated crates, pieces and object damage (stage 6: a client sees no crates); chunk sync, server
terrain mode, `HashCheck` (stage 7: a probe mismatch is still only logged); `vortex_bot` as its own executable and the
10-minute soak (stage 8); a Windows client (M6); encryption and real tickets (owner decision 2: trusted testers);
lag compensation of the cast origin beyond the own state (M2); loadouts other than the tuning's one cast (`loadout` is
always 0 and not on the wire); the event log still grows for the whole session (stage 7).

### Review of stage 5 (2026-11-13)

Three reviewers read the change; 13 findings, one of them reported three times (the engine's tick cap), each checked
against the code. Every bug was first reproduced by a failing GPU-free test where one could be written (7 new tests,
**607 in all, six of them disabled measurements**; `EnetSession.*` now runs alone: `RUN_SERIAL`, label `wall-clock`).
`kProtocolVersion` = 5. Fixed:

| Finding | Fix |
|---|---|
| A session that ended while `Attach` loaded the region (its callback polls: a disconnect, a refusal, a malformed batch that failed the client) was overwritten to Playing, and `Attach` returned true: the game showed no "Disconnected" and a headless run exited 0 with a world that no longer received operations. | The load loop stops once the state is not Welcomed, and `Attach` then returns false with a log line. `Client.ASessionThatEndsWhileTheRegionLoadsIsNotAttached` (disconnect and malformed batch; before: true and Playing). |
| The engine took up to 30 ticks from the clock (`TicksDue`'s default cap) and ran at most 8: the rest of a long frame or of a jump was lost while the clock counted it as run (more late inputs, more jumps). Reported three times. | `Client::TicksDue(seconds, maxTicks)` forwards the cap to `TickClock`, which keeps the rest; `Engine::SetTickSource` passes `kMaxSourceTicksPerFrame` (8) and runs every tick it gets (an over-cap answer is logged once). `Client.TicksAboveTheCallersCapAreCarriedToTheNextFrames` (0.3 s = 8 + 8 + 2; the API did not exist), `Clock.TicksAboveTheCapAreCarriedToTheNextCalls` (a 0.4 s frame gives the ticks of 24 short ones; a 20-tick jump runs as 8 + 8 + 4). The prediction start also drops ticks owed from before it (`TickClock::DropBacklog`): the lead is set from the round trip. **Not taken:** the reviewers also asked to stop capping the frame time at 0.25 s for the tick source. A backlog drained at 8 ticks a frame and the clock's own late-input jump (three late reports, by the measured margin) would both move the clock forward for the same stall; with the cap the backlog is at most 15 ticks (two frames), gone before the late reports return, and the jump restores the rest exactly. |
| ENet's default 32 MiB message and 32 MiB waiting-data limits let any UDP sender make the server allocate before a token is checked. | A listening transport takes messages of at most `EnetSettings::maxIncomingMessageBytes` (default `kListenMaxMessageBytes`, 64 KiB; a client's largest message is under 1 KiB) and `kListenMaxWaitingBytes` (1 MiB) per peer; ENet drops a larger one before allocating it, unacknowledged, so its sender times out. ENet checks the same host limit in `enet_peer_send`, so `Send` lifts it for this side's own messages (a large event goes alone). A connecting transport keeps ENet's defaults (it talks to the server it chose). `Enet.AListeningTransportRefusesLargeMessagesButSendsThem` (before: the 1 MiB message was delivered; now refused, the sender dropped after 0.9 s with 0.2 / 1 s timeouts, the next client served; the server still sends 1 MiB intact). |
| The prediction lead and the HUD ping used ENet's smoothed round trip, which a stall of this side inflates. | Measured first: it was worse than a stall. ENet starts the estimate at 500 ms and moves it 1/8 per acknowledged reliable message; a client sends few (Hello, then pings every 500 ms), so the GPU-free acceptance's client 0 started with a 12-tick lead (about 110 ms of RTT on localhost) and made **a hard resync in every run**. Now: the connect handshake's own round trip seeds ENet's estimate on the connecting side (as ENet means its first sample to), `ITransport::RoundTripSeconds` is the lowest of ENet's smoothed value and its lowest of this and the last throttle interval (a stall raises the first, never the others), and `EnetTransport::SmoothedRoundTripSeconds` is ENet's value for the HUD. The game services the connection from a helper thread while `Engine::Initialize` runs (logged: 0.04 s headless here), the in-process bots of `vortex_server --enet` and the `EnetSession` test run whole passes of everyone else inside a bot's region load (`Attach`'s callback) and once after it. `EnetSession` now asserts **0 hard resyncs** (3 runs: 0; leads 7 ticks for both clients). |
| A late cast accepted at its input tick left the kept states of the ticks since then at cooldown 0, so a second late edge a few ticks later was taken (the client had refused it). | `LateCast` rewrites the kept states of the ticks since the cast with the cooldown they would have had (`full - j`, as `StepPlayer` counts). `Movement.TwoLateCastEdgesAreJudgedAgainstTheCooldownTheFirstStarted` (two edges 5 ticks apart, 19 ticks late; before: 2 casts, 0 refused; now 1 and 1 `Cooldown`). |
| The 32-tick state ring was value-initialised: a late input for tick 0 was judged against "tick 0, at the origin, cooldown 0" and cast from there. | Unused entries have the tick `0xFFFFFFFF`, and inputs for ticks before the player joined are ignored and counted (`ServerStats::inputsBeforeJoin`; no client makes them: it predicts from its first snapshot on). `Movement.ALateInputFromBeforeTheJoinCastsNothing` (before: a cast ran). |
| `vortex_server --listen` binds every interface while the example session files, tokens included, are in the repository. | `--new-token` (a random token, printed at start) and `--token HEX` override the file's; listening beyond the loopback with the file's token logs a loud warning. The owner's two-machine steps in `CLAUDE.md` and `design/game-ui.md` now require `--new-token`. Not refused outright: an owner's own session file with a fresh token is legitimate, and the server cannot tell it from a checked-in one. |
| The one-machine acceptance could not fail on broken replication: a headless client exited 0 whatever its hash, and someone had to compare hex strings. | A headless session client drains at exit (polls until it has applied the server's last operation and nothing new came for 1 s, at most 5 s) and exits 1 when the drain does not settle or its hash is incomplete; `vortex_server` logs `Session: server ends at opSeq A, tick T, world hash H`; the game repository's `tools/session-check.sh` runs the server (fresh token, 127.0.0.1) and two headless clients and exits 1 unless all three exit 0 and both clients' hashes equal the server's at the same opSeq. Not a unit test (it needs the GPU client); run below. |
| No ENet servicing during `Engine::Initialize` while the server pushes the join log (plausible). | Measured 0.04 s headless on this laptop (a window adds its own creation; not measured). The game now services the connection from a helper thread meanwhile (above), so neither a timeout nor an RTT sample can come of it; the server's peer timeout is unchanged. |
| `EnetSession` asserted prediction quality on the wall clock in one shared thread, and its reproducible hard resync went unasserted. | Registered on its own with `RUN_SERIAL` (`VORTEX_SERIAL_TESTS_<target>` in `tests/CMakeLists.txt`); no participant stalls the others (above); the resync's cause was the lead (above) and is fixed; `EXPECT_EQ(hardResyncs, 0)` added. |
| `vortex_server` ignored Ctrl-C during the drain (up to 600 ticks). | The drain stops on a signal and the run fails ("stopped by a signal ... no drain"); the handler restores the default, so a second Ctrl-C kills the process. |
| An open-ended server keeps the whole event log and sends it to every joiner, unreported. | The 10 s status line gives the log's size; `ServerSettings::joinLogWarnBytes` (32 MiB) logs a warning once, and above `maxJoinLogBytes` (128 MiB) a joiner is refused with the new `RefuseReason::LogTooLarge` ("the session's event log is too large to join (networking stage 7)"). `Session.AJoinIsRefusedOnceTheEventLogIsTooLarge`. Stage 7 replaces the log. |

None was rejected as wrong; one requested change was not made (the uncapped frame time, above). After the review:
`tools/session-check.sh` **PASS**: both headless game clients and the server exit 0 under validation, both clients
drained in 1.0 s and log world hash `79627dcc892d9730` at opSeq 68 of the server's 68, the server logs the same hash
at opSeq 68 when each leaves and when it ends (the hash differs from run to run: where the walking player's casts land
depends on wall-clock timing); 0 clock jumps or resyncs, margins 2-10 / 4-7, corrections 6 (max 0.60 m, walking into
its own fresh crater) and 0, region loaded in 3.0 s while polling; the second client's screenshot shows the first
one's figure at the tower and a crater. The CLAUDE.md drill (loopback) exits 0 with equal hashes, 67/67, 67/67, 66/66,
corrections 4 and 0. The UDP drill (`--enet --clients 3 --join-step 50 --bot-casts 90 --loss 0.05 --delay-ms 40
--jitter-ms 30`, three runs) exits 0 with equal hashes and the full drill every time; corrections 4-22 per client,
0-1 hard resyncs and 0-1 snaps per run (before the review 2 resyncs and 2 snaps in one of three): a bot's region load
no longer stops the others, but one thread still runs the server, the drill and three simulations, so the clock is
judged with game clients. The game's standard (beam cut 13 voxels, HUD 149 quads, feet (16.95, 4.36, -3.49)),
editor, `--climb` (0 unloads inside the view radius, window y fixed), `--tower-blast --after-fill --frames 700` (shot 6,
11.27x) and `--neck-blast --frames 200` (8558 voxels, 5.61x) runs exit 0 under validation with the numbers of before.
**Owner question:** should `vortex_server --listen` refuse (rather than warn) to listen beyond the loopback without
`--token` / `--new-token`? It would break a session file with its own fresh token unless that file is marked somehow.

### Stage 6 as built

Built (2026-11-16; started by an earlier implementer whose unreviewed partial work the owner committed as engine
`c1e19c8` "M1 - Stage 6", then judged, fixed and finished on top of it): in `vortex.scene` `NetId{value, piece,
bornTick, despawn}`, `DespawnReason` (Destroyed 1, ToRubble 2, Broken 3; wire format), `SceneConfig::rubbleLayer`,
`DamageResult::keptOffset` / `keptMotion`; in `vortex.physics` `Layer::Rubble`, `SetLayer` / `GetLayer`, `MoveKinematic`,
`PhysicsConfig::characterMovesBodies`; in `vortex.sim` the partition `:replication` (`AllocateNetId`, `Replicate`,
`IsAwake`, `CountPieces`, `TakePieceSlot`, `Despawn`), `ReplicationConfig` / `ReplicationState` / `ReplicationStats`,
`SimulationConfig::replication` (on in `ServerConfig`), `ObjectEvent` and `BlastResult::objectEvents` / `lostNetIds`,
`PlayerButton_Crate` = 8, `kSessionCharacterLayers` = Static | Dynamic; in `vortex.net` the events `EntityDespawn` (33),
`EntityRest` (34), `ObjectModel` (35) and `EntitySpawn` of kind `Object` (2) (and since the review `EntityWake` (36),
protocol 7), `BodySample` in `Snapshot`, `RemoteBodies`,
the partition `:entities` (`EntityInfo`, `CompareEntities`, `EntityListHash`), `Server::SpawnCrate` / `SpawnObject` /
`ClearBodies` / `Entities` / `AwakeBodies`, `Client::Entities` / `Bodies`, `RunDrillStep`, `BlastDrill::KeepSites`,
`PilotSettings::crateEveryTicks`; `kProtocolVersion` = 6. The game's session mode sends E as `PlayerButton_Crate` and logs
its copies; `vortex_server` gained `--bot-crates`, `--drill-after`, `--drill-delay`, `--drill-sites`, `--drill-every`
and the entity check; `tools/session-check.sh` runs a drill beside the session and compares entity lists. No new
library or executable target (the layering and both configure checks are unchanged).

**What `c1e19c8` had wrong or left out** (each fixed here; the rest of it - the scene, sim, wire, server and client
halves above - was read against this design and kept):

| Found | Fix |
|---|---|
| `CharacterLayerFilter` followed the mask for `Layer::Rubble`: a mask with that bit let rubble block a character. Its own test (`PhysicsWorld.RubbleLayerMeetsTheWorldBodiesAndItselfButNoCharacter`) failed. | The filter never accepts Rubble, like Character. |
| `PhysicsWorld.CharactersThatMoveNoBodiesNeitherPushNorWeigh` failed: it expected a character to push a 51 kg crate (beyond Jolt's 100 N `mMaxStrength`) and measured from the creation x, not the rest x (the crate settles by 1e-6 m). | The test pushes a 5 kg box and measures from where it rested; the behaviour was right. |
| No network test of stage 6 (the sim test's header named a missing `EntityTests.cpp`): none of the acceptance existed. | `tests/net/EntityTests.cpp` (5 tests), `Client.ALateSnapshotAfterAnEntityRestKeepsTheRestPose`, `RemoteBodies.HermiteInThePastRestRefusesLateSamples`. |
| Nothing on the game side: E did nothing in a session, nothing logged the copies, `tools/session-check.sh` saw no body. | E key, headless crate, entity logs, the drill beside the session (below). |
| A copy its own scene removed (`DestroyBelow`: drawn in the past, it can fall out first) stayed in the client's map with a dead entity. | Dropped and counted (`ClientStats::copiesLost`); the entity check sees it missing. |
| A client showed no object damage (it never runs `Scene::Damage`; stage 2 left object particles to stage 6). | The voxels an `ObjectModel` removed become particles (at most 64 bursts). |
| No way to test a split above the threshold (only crates, which break at once). | `Server::SpawnObject(model, origin)` (admin / tests). |

Where the design was open or disagreed with the code as it is after shred.md stages 0-4b and networking stages 3-5,
this was chosen:

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| A moving piece and a player ("Player movement": on the server the character collides with it, the client gets a correction). | **Symmetric**: a replicated body is in `Layer::Rubble` while it moves and `Layer::Dynamic` at rest **on the server too** (`TrackBodies` after physics, `SpawnObjectAt` at creation), as on every client (a copy is Dynamic from its `EntityRest` until its `EntityWake`; as built here "until a newer sample", changed by the review). No character meets Rubble - except, since the review, the body a player stands on, which stays Dynamic while it carries that player (server: any player; client: its own). | A `CharacterVirtual` is not a body: a falling body passes through a player on the server whatever the layer, so the design's "push" was only ever the character's contact with a moving body, which the client sees 175-300 ms late (rejected alternatives: guaranteed corrections). Left: the latency of one `EntityRest` (a player walking into a piece that has just come to rest). `PlayerUnderFallingPiecesAt150ms`: 4 corrections over 6 caps falling on a walking player, max 0.13 m, no snap. A piece may come to rest overlapping a player; the player walks out (Jolt's penetration recovery), the same on both sides. Crush damage is a server sweep (M2, shred.md question 8), not collision. |
| Pushing and standing on bodies. | `PhysicsConfig::characterMovesBodies = false` on both sides of a session (`ServerConfig`, `MakeSimulationConfig`; `Attach` checks it): no impulse on any contact, mass 0. Offline unchanged. | A client's copies are kinematic and cannot be pushed; a server player that pushed a crate would be corrected at every touch. `RestingCratesBlockPlayersOnBothSides`: walking into a resting crate stops both sides at x + 1.76 m, 0 corrections. |
| Which pieces become server bodies, and the caps. | Terrain islands of at least `SceneConfig::replicatedPieceVoxels` (the metre volume, 0.512 m^3: 262 voxels at 0.125 m) and object pieces `Damage` keeps as entities (it keeps only those on a server) ask `sim::TakePieceSlot`: at most `bodiesPerTick` (2, shred.md Part 4 `serverBodiesPerTick`, both kinds together), `maxAwakePieces` (64; awake = Jolt-active or frozen while it wanted to move; slots taken earlier in the same blast count; since the review every awake replicated body counts, placed objects too) and `maxLivePieces` (256; crates, template objects and players' crates are placed objects, not pieces). Since the review a cast's terrain pieces ask largest first, and none is made while the entity byte budget is spent. Over a budget: `netId` 0 (`Detach`) or a fragment (`EntitySpawn` with `netId` 0). The live cap first evicts the oldest resting piece (birth tick, then id), else the smallest awake one (voxels, then id), as `EntityDespawn{ToRubble}`. | The NetId of a piece reads Jolt's sleep state, so a replayed server may number pieces differently - never voxels (the `Detach` mask is the same). The 2 per tick bound what a client must build per frame (shred.md: about 4 at 30 fps); the client builds every copy when its event is applied (shred.md's "dust until the body exists" is not built). Collapse test: 64 awake reached, 16 more pieces of the wave went to rubble, 8 evicted resting pieces became rubble on the presenting client. |
| The server body of a terrain piece. | `sim::Blast` step 5, after objects and impulses: `BuildDetachedPiece` of the voxels the `Detach` removed, at the `Detach` origin, with its push as the velocity; the `Detach` carries the NetId (made in step 2, before the piece's voxels are cleared). A body that cannot be made (out of bodies) is despawned `Destroyed` at once (`lostNetIds`). | The client builds the same model from its own world (worlds are equal at that `opSeq`): no ids on the wire, equal model hashes (`Replication.ALargePieceIsAServerBodyAndASmallOneClientRubble`, every entity check). |
| A damaged survivor below the threshold ("despawned toRubble"). | Despawned **`Broken`**; its remains are the fragments (`netId`-0 spawns) a client makes into rubble. `ToRubble` is the caps' eviction only (a client keeps that copy as rubble). | Its old copy has voxels the blast removed; the fragments have the new shape. A survivor at or over the threshold is an `ObjectModel` (offset in old-model voxels, motion, the model's ids); the client re-creates its copy at the shifted origin and shifts its samples (`RemoteBodies::Shift`) - the design's `Scene::ReplaceModel` is not built. |
| Entity events and the event byte budget ("island and piece events count against the event budget"). | Terrain operations (a `Detach` with a NetId included) charge the cast queue's credit as before; entity events (spawns, models, rests, despawns) do **not**, and are counted (`ServerStats::entityEventBytes`). | They follow from Jolt (which crate a blast catches, when a body sleeps): charging them would put a replayed cast on another tick (shred.md R5; `CastBudget.ScheduleFollowsCountsNotTimeOrCache` now compares casts without their `opSeq`). As built here "bounded by the body budgets instead" - wrong (an `ObjectModel` resends a whole model at every hit); since the review they have a budget of their own (see there). Measured: 70 KiB of the drill's 471 KiB (200 blasts, 204 crates). |
| Wire sizes (protocol 6). | `EntitySpawn` object 60 B + model (6 B size + LEB128 (run, id) pairs; raw float pose and velocities, motion, a resting flag), `ObjectModel` 12 B + model, `EntityRest` 37 B (tick, raw float pose), `EntityDespawn` 6 B. Models share the batch decoder's voxel budget with masks (`kMaxModelAxis` 1024). Snapshot header 22 B (a body count), a body 31 B (netId, raw position, rotation 4 x i16, velocity 3 x i16 in 1/256 m/s, flags). | A hollow crate's model is under 120 B. Raw floats where a client places something exactly (rest poses compare bit for bit), quantised where it only interpolates. |
| Snapshot priority. | Every awake replicated body competes for the room after the players (1200 B, `ServerSettings::snapshotBytes`): priority = (snapshots it was left out of + 1) / (1 + distance to that client's player / 16 m), ties by NetId. | Collapse test (64 awake, two clients, 5 % loss): snapshots up to 1186 B with 36 bodies (35.6 kB/s, under the design's 37 kB/s cap), 4046 bodies left out of a snapshot, largest gap between two samples of one body 8 ticks during the wave (12 over the run); without loss the design's "at most 2 snapshots apart" holds for near bodies, far ones wait a third. |
| Drawing copies. | Kinematic `CreateVoxelObject`s; each client tick (`Client::StepPlayer` / `RunTick`) `MoveKinematic` to `RemoteBodies::Sample` at a body time that follows `ServerTime` (pulled 10 % a tick): Hermite between samples (sampled velocities as tangents, rotation slerped), delay max(4 ticks, 2 x the body's mean sample interval) up to 12, extrapolation along the last velocity for one interval, then hold; no sample: still. | The rest rule is exact: an `EntityRest` puts the copy at the server's pose bit for bit (every test compares rest poses at under 1 mm, measured 0.0000 m) and refuses samples of its tick or older; a newer one starts it moving (`Client.ALateSnapshotAfterAnEntityRestKeepsTheRestPose`; since the review the `EntityWake` does, and a snapshot's samples wait for its events). |
| The client's own rubble. | `SceneConfig::rubbleLayer` (set by `MakeSimulationConfig`, checked by `Attach`): rubble of `debrisBodyVoxels` or more in `Layer::Rubble`, smaller in Debris - instead of the design's `VoxelObjectDesc` layer + `clientRubble` flag. Made from `netId`-0 `Detach`es (`PresentBlast`), fragments and `ToRubble` copies, all `VoxelObjectDesc::debris`: shred.md stage 1e's lifetimes and caps. | Only a presenting client makes rubble; a non-presenting one (bots) only applies voxels. |
| The entity check ("EntityCheck every 2 s"). | Not on the wire (stage 7's list with the entity baseline). `EntityInfo` / `CompareEntities` (NetIds, `VolumeHash` of models, rest poses; since the review also rest states and rest rotations) and `EntityListHash` (ids and models) serve the tests, `vortex_server` (exit 1 unless every bot agrees, rest poses within 5 cm) and `tools/session-check.sh` (the game clients' list hash at the server's `opSeq`). | Stage 7 sends it and re-sends the list on a mismatch. |
| Crates (E). | `PlayerButton_Crate` edge (wire); the server drops the playground crate centred 3 m along the look from the eye of the player's state (offline E puts the crate's corner at the camera + 3 m), at most one per 30 ticks, 8 alive per player (the oldest `Destroyed`), refused outside the play bounds / rows. Not predicted: the copy appears a round trip later. | An addition to the design (it had template crates only). A crate is a placed object (no piece cap; since the review it counts against the awake cap, is refused at it, and goes when its player leaves). At 0.125 m it is a 6^3 shell with a 4^3 hollow, **152 voxels** (0.297 m^3; as built this row said 296), under the 262 of the threshold from the start: the first blast that catches it breaks it (`Broken` + fragments) - the 0.512 m^3 threshold is not retuned (owner question, "Review of stage 6"). |
| The drill. | Each step clears its site's replicated bodies (`ClearBodies`, `Destroyed`), builds, drops a crate beside the structure (its blast catches it) and on a site's first step one in the margin that stays, then casts (`RunDrillStep`). The drain waits until no body moves. | 200 drill blasts: 200 terrain pieces became server bodies (mushroom caps, bars, neck towers of 8558 voxels), 204 crates, 200 broken into client rubble. |
| shred.md Part 4 budgets "where they apply". | Only `serverBodiesPerTick` (2). `releasesPerTick`, `nodeVisitsPerTick` and the solves belong to the `StructuralSolver` (shred.md stage 5, not built); a cast's load check still releases at most 64 parts / 60 000 voxels. | When stage 5 of shred.md queues releases, each one asks `TakePieceSlot` like a blast's island. |
| Join. | The full event log (stage 3) carries every entity event: a joiner makes its copies from the log's spawns and `Detach`es and places them by the log's `EntityRest`s; awake ones by the next snapshots. | Stage 7's entity baseline replaces it. Tombstones (despawned NetIds) grow for the session, 4 B each. |

Verified (all on this laptop, the dev build: -O2 -g): 16 new tests, **623 in all (six disabled measurements)**,
green, the new ones repeated 3-5 times. `VortexPhysicsTests` 3 (`RubbleLayerMeets...`, `KinematicBodiesFollowMoveKinematic`,
`CharactersThatMoveNoBodies...`), `VortexSimTests` `Replication.*` 5, `VortexNetTests`: `Wire.EntityEventsRoundTripAtTheirSizes`
(and the snapshot and forged-bytes tests extended), `RemoteBodies.*` 1, `Client.ALateSnapshotAfterAnEntityRestKeepsTheRestPose`, and
the acceptance `Entities.*` 5, over the seeded `ImpairedTransport` with one manual clock and player bots on their own clocks:

| Test | Link | Result |
|---|---|---|
| `FightWithIslandsAndCrates` (the M1 scripted fight): two bots walk, run, jump and drop a crate every 2 s; a drill blast every 6 ticks (10 casts/s) for 600 ticks | 5 % loss, 80 + 0-30 ms, duplicates, reordering, a 200 ms outage | 100 terrain pieces as server bodies, 114 crates (10 from the players' E), 100 broken into client rubble; after the drain equal world hashes, the same 8 NetIds and models on all sides, every copy resting exactly at the server's pose (0.0000 m), each player's prediction within 5 cm of the server's; 1 and 4 corrections in 10 s, no snap. |
| `CollapseWith64AwakePiecesAt5PercentLoss`: 80 stems cut, two a tick; then 16 more over a live cap of 72 | 5 % loss, 40 + 0-30 ms | Awake cap reached (64) with nothing asleep, 16 pieces to rubble; snapshots under 1200 B; then 8 resting pieces evicted `ToRubble` (rubble on the presenting client only); 72 objects agree on both clients, rest poses exact. |
| `ASplitBelowTheThresholdBreaksTheObjectIntoClientRubble`: a dumbbell of two 512-voxel cubes cut at its neck, then one cube blasted below 262 | 5 % loss, 40 + 0-30 ms | One `ObjectModel`, one new server body; then `Broken` + a fragment, rubble on the presenting client; ids and model hashes equal. |
| `PlayerUnderFallingPiecesAt150ms`: a bot walks up and down under six caps whose stems are cut one after another | 75 ms each way | 4 corrections over 6 pieces, max 0.13 m, no snap; agreement under 2 cm once it stands. |
| `RestingCratesBlockPlayersOnBothSides`: walking into a resting crate; then E held for 10 ticks | 40 ms each way | Both sides stop at the same x, 0 corrections; one crate, the same on both sides. |

Runs: the CLAUDE.md drill (loopback, 5 % loss) exits 0 with **hashes equal**, 67/67, 67/67, 66/66 and every client's
entity check agreeing (8 objects, rest poses 0.0000 m apart; 3 and 3 corrections); with `--clients 3 --join-step 50
--bot-casts 90 --bot-crates 120 --outage-ms 10000,10200` too (9 objects, one `ObjectModel`); over UDP (`--enet`, three
bots) hashes equal and entities agree (corrections 16, 6, 6, one snap and one resync for the late joiner: the one-thread
bots of stage 5's note). `tools/session-check.sh` **PASS** with the drill beside the session (30 steps on two corner
sites, 10/10/10 freed, 30 terrain pieces as server bodies, 34 crates): both headless game clients' world hashes and
entity list hashes equal the server's at the same opSeq; their screenshots show the plaza's crates (copies) and a
dropped crate; 0 corrections; 0-2 clock jumps / resyncs per client over the runs (without the drill 0: the clients apply
a 12.5 m site rebuild ten times a second; the server's own passes stayed under 3 ticks except its first). The game's
standard (beam cut 13 voxels, HUD 149 quads, feet (16.94, 4.36, -3.54)), editor, `--climb`, `--tower-blast
--after-fill --frames 700` (shot 6, 11.27x) and `--neck-blast` (8558 voxels, 5.61x) runs exit 0 under validation
with the numbers of before: offline play is unchanged (replication is off offline; `characterMovesBodies` and
`rubbleLayer` keep their offline defaults).

**Not done (later stages, or open):** windowed play of a session with crates and falling pieces and the two-machine
run (owner); the wire `EntityCheck`, entity baseline and model requests (stage 7); tuning the replicated threshold (a
crate breaks at its first hit); crush damage from falling pieces (M2) - a piece can come to rest overlapping a player;
impacts (shred.md Part 5); the client's per-frame body build budget; `Scene::ReplaceModel`; a 250 m arena with the
game. Review of this stage is pending.

### Review of stage 6 (2026-11-16)

Three reviewers read the stage (engine `c1e19c8` plus the work on top of it, and the game's session mode): 17 findings,
two of them duplicates of others, and one more problem found while verifying. Every bug was first reproduced in a
GPU-free test that failed (checked by putting the old code back where the test could not show it by itself); 11 new
tests, **634 in all (six disabled)**.

| Finding | Verdict | Fix (test) |
|---|---|---|
| Body samples were taken when a snapshot arrived, before the events it was built after: a stale `EntityRest` (a resend, head-of-line blocking, a late joiner's log) parked a moving copy at an old pose in `Layer::Dynamic`; an `ObjectModel` shifted a new-frame sample a second time (the copy drawn the offset away); a new piece's first samples were dropped. | Real (medium). | Samples wait like the owner block: a snapshot's samples are taken after `ApplyPending`, before reconciliation, once its `lastOpSeq` is applied (at most 32 snapshots wait). `RemoteBodies::Rest` drops only samples of its tick or older and says whether the body rests; a stale rest leaves the copy moving (`ClientStats::staleRests`). (`Client.ASnapshotsSamplesWaitForItsEvents`, `Client.AnObjectModelAndItsSnapshotInOnePollKeepTheCopyOnTheSample`: both fail with samples taken on arrival.) |
| Same as the previous, from the second reviewer (`ObjectModel` vs a reordered snapshot). | Duplicate. | As above. |
| Samples of a body whose spawn or `Detach` is not applied yet were dropped. | Real (low); a consequence of the first. | Fixed by it: a sample waits for its snapshot's `lastOpSeq`, which covers the spawn. Seeding a track with the spawn pose is not needed. |
| Waking was never an event: a copy left `Layer::Dynamic` only when a sample won a snapshot slot, and an `ObjectModel` re-made the copy resting while the server's survivor is a new, awake body. | Real (medium). | `EntityWake{netId, tick}` (event kind 36, 9 bytes, `kProtocolVersion` 7), emitted by `TrackBodies` when a body that rested moves. A client's copy leaves Dynamic at the same point of the stream as the server's body; it holds still until its samples come (the rest's tick stays refused). Samples no longer change a copy's rest state. An `ObjectModel` re-makes the copy moving unless the model is static. (`Client.ALateSnapshotAfterAnEntityRestKeepsTheRestPose` now wakes by the event; `Wire.EntityEventsRoundTripAtTheirSizes`.) |
| A player standing on a replicated body fell into it when the body woke (any blast within two radii, any touch): Rubble meets no character. | Real (medium). | The body under a player stays `Layer::Dynamic` while it moves: on the server any player's ground body (`CharacterState::groundBody`, Jolt's ground body id), on a client the copy under its own predicted player (`MoveCopies`, `EntityWake`); it carries the player (the client's copy moves late: corrections, as the design accepted for contact with moving bodies). (`Entities.APlayerOnACrateThatWakesStaysOnIt`: woken under the player, 0 corrections and the feet 2 cm under the top; without the server's half the player drops 0.77 m into the crate, 11 corrections.) |
| `replicatedPieceVoxels` came from the default voxel size: `Server::Start` applied `ServerConfig` before the region's world (262 voxels at 0.1 m instead of 512; 262 instead of 152 at 0.15 m). | Real (low). | `ServerConfig` after `RegionWorldConfig`. (`Entities.TheReplicatedSizeFollowsTheSessionVoxelSize`: 512 / 262 / 152.) |
| The awake cap counted only pieces; awake crates and players' crates fill the same snapshots. | Real (medium). | `TakePieceSlot` counts every awake replicated body (`PieceCounts::awakeBodies`); a player's crate is refused at the cap. Players' crates are an addition to the design: one per 30 ticks, 8 alive per player, counted in the awake cap, gone when the player leaves. (`Replication.TheAwakeCapCountsPlacedObjects`.) A network collapse variant with awake crates was not added: the sim test pins the count. |
| Entity events had no byte bound; an `ObjectModel` resends the whole model at every hit (the claim "bounded by the body budgets" was wrong). | Real (medium). | An entity byte budget of its own (`ServerSettings::entityEventBytesPerTick` 200 = 12 kB/s, burst 16 KiB), charged by every entity event and never read by the cast schedule (R5 holds: it decides only which bodies are replicated, as the awake cap does). While it is spent a cast makes no new server body (`PieceSlot::OverByteBudget`: a terrain piece is a `netId`-0 `Detach`, no model on the wire; an object piece a fragment, whose model is sent - once per object, as the object is let go) and a damaged object is let go as client rubble (`EntityDespawn{ToRubble}`, 6 B) instead of an `ObjectModel` (`ServerStats::modelsToRubble`); a client keeps its copy as it was before the hit (the crater and the split-off pieces are not cut out of that rubble: a degradation under load, accepted). Rests, wakes, despawns and placed objects are always sent. A cap on replicated model size or a mask-encoded `ObjectModel` was not needed. (`Entities.EntityEventsKeepTheirByteBudget`: 120 hits on six 3.4 KB-model cubes at 6 kB/s: 9.7 KB sent, 3 models, 6 let go; without the budget every hit sends a model.) |
| A player's cast ray met terrain only: a crate, a falling piece or a felled tower in front of the terrain could not be shot. | Real (medium). | `ResolveRay` also casts into the server's bodies (`PhysicsWorld::Raycast` with a layer mask, Dynamic \| Rubble: on a server only replicated bodies are there) and takes the nearer hit (`ServerStats::castsAtBodies`); the game's predicted puff does the same against its copies (`Client::IsCopy`). R5 is untouched (player rays already depended on Jolt through positions). (`Entities.ACastAimedAtACrateBreaksIt`: aimed at a crate 10 m away, the ground behind it out of reach; it breaks.) |
| Body slots went to pieces in emission order: a tower released by the load check (emitted last) lost its slot to smaller in-box pieces. | Real (low) for terrain pieces; not for object pieces. | A cast's terrain pieces of the replicated size ask largest first (then lowest, then emission order); NetIds go in that order, the `Detach` order (wire order) is unchanged. Object pieces are already asked largest first (`DamageResult::pieces` is sorted so). (`Replication.TheTicksBodiesGoToTheLargestPiecesFirst`: one body left, the smaller piece emitted first; the larger gets it.) |
| Object-damage presentation bypassed `PresentBlast`; a `Broken` object showed no burst. | Real (low) for `Broken`; the rest rejected. | A live `EntityDespawn{Broken}` bursts the old copy's voxels inside the newest shown cast's sphere (at most 64 bursts), as `Scene::Damage` does offline. Moving the fragments into the presentation group was rejected: `PresentBlast` makes them with the same `CreateVoxelObject{.debris}` call the client makes, so nothing would change on screen. |
| Docs: the crate is 152 voxels at 0.125 m, not 296; owner decision 6's tuning was left undone on that premise; ARCHITECTURE.md and the game's CLAUDE.md said a blast makes "no entity for an island". | Real. | Corrected here, in "Stage 6 as built", ARCHITECTURE.md 7.2 and the game's CLAUDE.md (on a session server a replicated island gets a server body and a NetId). The tuning is an owner question (below). |
| The entity check ignored whether a copy rests and its rotation: a client that never applied `EntityRest`, or applied the wrong rotation, passed every test and `vortex_server`. | Real (high). | `EntityComparison::restStateMismatches` and `maxRestAngle`; `Agree(position, angle = 1 mrad, midFlight = false)` fails on either. The rest test checks the rotation bit for bit. |
| Motion replication had no end-to-end assertion that could fail (every comparison ran after the settle, `MaxGap` saw no tail). | Real (medium). | The collapse test compares the copies with the server's bodies every few passes during the wave (moving copies within 2.5 m; measured 0.93 m over 1216 comparisons), asserts samples sent and taken, no copy lost, and that every body left was sampled (`RemoteBodies::Received`); `MaxGap` now counts the gap from a body's last sample to its rest. |
| `Entities.*` depended on worker timing (collider bodies got Jolt ids in job completion order). | Real (medium). | The order was one cause; the other was the job budget, which re-read the live in-flight counter, and `GuardBodies` started a second batch inside the tick: how many cooks started depended on which had finished. Results are installed in chunk order, `Flush` waits for each round, and the budget is per round (an `Update` and the tick's `GuardBodies`, or a pass of `Flush`). (`Entities.OneScriptGivesTheSameBodiesTwice`: the same script twice gives the same bodies bit for bit; it failed 5 of 8 runs under load before the per-round budget, 0 of 20 after.) In the game a frame may start fewer cooks than before when jobs finish quickly (the cap is per frame, as for world jobs; the frame's physics ticks no longer refill it); not measured windowed. Offline the headless walk now ends at feet (17.15, 4.36, -2.99) instead of (16.94, 4.36, -3.54) (the old code varied by 5 cm run to run, the new one by 1 cm): colliders appear on other frames, the path differs a little. |
| A player's crates outlived the player; the crate limits were untested. | Real (low). | `DropPlayer` despawns its crates (`Destroyed`). (`Entities.PlayersCratesAreLimitedAndLeaveWithThePlayer`: E every other tick for 600 ticks: 20 crates, 280 edges refused, at most 8 alive; the player leaves, its crates go; a late joiner agrees.) |
| The headless game's drain did not wait for its copies to rest, so `tools/session-check.sh` could fail on timing. | Real (low). | `Drain` also waits until every copy rests (as `vortex_server`'s drain waits for its bodies). Since a player's crates now go when it leaves, the server's "client left" line is logged before they are despawned (`Server::SetOnClientLeft`), at the opSeq the client drained to; the session check failed on that before. |
| Found while verifying: the drill's late joiner (log of 1579 events, paced at 32 KiB in flight over 5 % loss) started predicting from the newest snapshot whose events it had applied, 66 ticks behind the newest; its inputs reached the server 53 ticks late, the clock jumped, overshot by 50 and held (27 corrections, a 5 m snap). The review's `EntityWake` events made the catch-up a little longer (more batches, more lost packets to resend in order) and showed it; the cause is stage 5's start rule. | Real (medium). | A joiner starts only from a snapshot at most `ClientSettings::startFreshTicks` (8) behind the newest one received, else it waits for a fresher one, at most `startWaitTicks` (120; `PredictionStats::staleStarts`). (`Client.AJoinerStartsPredictingFromAFreshSnapshot`; the drill's joiner: 0 corrections, no resync.) |

Verified after the review (this laptop, dev build): warning-free; 634 tests (six disabled) green; the session tests
were run several times, the reproducibility test 20 times under parallel load. `Entities.*`: the fight as before (8
objects agree, rest poses and rotations exact, 0 rest-state mismatches; 1 and 4 corrections, no snap), the collapse
(64 awake, 16 to rubble, 8 evicted; moving copies within 0.93 m mid-flight, largest sample gap 8 during the wave). The
loopback drill (2 clients, 5 % loss): hashes equal, 67/67, 67/67, 66/66, entities agree; 3 and 0 corrections (the late
joiner had 27 and a snap before its start rule), the same with the 200 ms outage (4 and 1); with three clients,
casting and dropping crates: 6 / 7 / 4 corrections, 8 casts hit a body, 47 wakes, entities agree. Over UDP (three
one-thread bots): hashes equal, entities agree, 17 / 6 / 11 corrections and one snap (stage 5's one-thread note).
`tools/session-check.sh` PASS (the server's lines at 560, 561 and 562: each client's drained opSeq, then its crate's
despawn). The game's standard run (beam cut 13 voxels, HUD 149 quads, feet on the ground), editor, `--climb` (0
unloads, window y fixed), `--tower-blast --after-fill --frames 700` (shot 6, 11.27x) and `--neck-blast` (6 duds 0
released; 8558 voxels, 5.61x) exit 0 under validation with the numbers of before, except the headless walk's last
feet (above: the collider cook budget); replication stays off offline.

**Owner question (decision 6, the replicated threshold).** The crate is 152 voxels at 0.125 m (0.30 m^3), the threshold
262 (0.512 m^3): every hit breaks a crate into client rubble, as offline lumps under the threshold are not shared.
(a) Keep 0.512 m^3: crates are one-hit, only large pieces are shared (no extra cost). (b) Lower it to about 0.15 m^3
(77 voxels at 0.125 m): a crate survives a hit as a shared object; est. 2-3x the replicated pieces of a collapse, so
the awake cap (64) is reached sooner and more of a collapse becomes client rubble. Recommendation: (a) until the
two-machine run shows whether one-hit crates feel wrong.

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
