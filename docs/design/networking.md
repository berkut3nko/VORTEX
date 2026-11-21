# Networking skeleton (M1)

[[../../../00 Docs Map|Docs map]] · [[../../ROADMAP|roadmap]] · [[../../ARCHITECTURE|architecture]] · [[shred|shred]] · [[game-ui|game UI]] · [[../../../docs/two-pc-test-windows|two-PC test (Windows)]] · [[../notes/net|net notes]]

## How others solve this optimally (web survey, 2026-11-21)

- Authoritative server + client prediction keyed by input sequence numbers + reconciliation by replaying unacked inputs + remote-entity interpolation + rewind hit tests is the canonical recipe ([Gambetta, Fast-Paced Multiplayer II](https://www.gabrielgambetta.com/client-side-prediction-server-reconciliation.html), [IV, lag compensation](https://www.gabrielgambetta.com/lag-compensation.html)). This design already matches it (integer inputs per tick, 30 Hz snapshots, own player predicted through `sim::StepPlayer`, others interpolated); rewind hit registration is the open piece (M2 "lag-compensated hits").
- Session-per-region servers at MMO scale are a shipped pattern: per-zone instances with a decoupled simulation layer on Photon ([Unity's Albion Online case study](https://unity.com/blog/albion-online-cross-platform-pvp-mmo-architecture), [architecture talk, Quo Vadis 2016](https://www.slideshare.net/slideshow/albion-online-software-architecture-of-an-mmo-talk-at-quo-vadis-2016-berlin/62724504)) — the same shape as M1: one authoritative server per region instance, clients stream their own window.
- Joining by chunk sync (stage 7a) is the standard answer to "bit-identical generation across toolchains is fragile": shipped voxel MMOs send authoritative terrain to joiners rather than trusting both sides' generators. Server terrain mode (7b) is that fallback, and keeping the terrain golden tests as a release gate (two-pc-test-windows.md gate 2) matches treating identical generation as an optimization, not an assumption.

Status: design decision, revised after two reviews. Stages 0-6 are built, stage 7a too (both milestones, integrated and
accepted on 2026-11-17; "Stage 7a as built", integrated, accepted and reviewed ("Review of stage 7a", "Recheck of
stage 7a"), committed as `62f28b4`); stage 7b ("Stage 7b plan", amended after a critic, superseded where "Stage 7b as
built" differs) is built, integrated and accepted on one machine (2026-11-17): milestone M-a (`HashCheck` repair, the
wire `EntityCheck` and the entity list) and milestone M-b (server terrain mode, the whole test table on a 250 m region,
the Windows-build fixes), then reviewed with fresh eyes ("Review of stage 7b": 12 findings, 11 fixed - protocol 10 -
one in part; the acceptance re-run on the fixed code) and rechecked ("Recheck of stage 7b": 3 findings, all fixed, no
wire change); not committed ("Stage 7b as built", its "Integration and
acceptance") (see "Known risks of terrain
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
  channel (est. 8-25 MB for a 154 m window; measured in stage 7b: 40 MiB for the game's window on the arena, 16 MiB
  for the playground).

### Known risks of terrain agreement after stage 0

Stage 0 was built and run on one Linux machine only. What is and is not established:

| Risk | State |
|---|---|
| The Windows build has not been made or run. | **Open.** Terrain agreement across platforms is unproven until the three golden tests are green there. Server terrain mode (built in stage 7b M-b: a client whose probes differ, or whose checks find 3 pristine chunks it made differently, receives its whole window from the server) is the safety net: such a client plays on the server's terrain with equal hashes, at the price of a much larger join (16 MiB on the playground, "Stage 7b as built"). |
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
  within 12 m of that client's player, compared at the same stream position. Mismatch -> `ChunkRequest`. (As built in
  stage 7b: 4 recent + 8 modified + 12 pristine chunks - two around the feet, ten on the way ahead - 510 B per 2 s.)
- Est. after a 5-minute 5v5: 0.5-8 MB per join. Until stage 7 a joiner receives the whole event log instead.
- As built (stage 7a and its review): a moving client window follows with one chunk of slack and names every step
  (`World::TakeWindowSteps`); bulk is paced by a delay-based window over all reliable bytes, and a message that does
  not fit waits only for the peer's bulk in flight (`ITransport::ChannelBytesInFlight`).

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
  hashes}` every 2 s; mismatch -> the entity list is re-sent. The player spawns once chunks within 32 m are based
  (as built: current) and the baseline's entities within 32 m of the spawn are in (`BaselineNear`, review of 7a).
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
| 7a | **Built** ("Stage 7a plan" (superseded), "Stage 7a as built", "Review of stage 7a"; integrated, accepted, reviewed and rechecked 2026-11-17). Chunk sync: modified map, encode at dequeue, bases, ring, `TakeLoadedChunks` / `WindowRevision`, paced bulk, entity baseline; joins by chunk sync (M-a); a client window smaller than the region that follows the player (M-b) | The 7a rows of "Testing" (the join latency at 128 kB/s and 1 % loss: at 5 % loss unreachable, "Review of stage 7a" 7); the drill (with `--max-join-p99-ticks 15`) and `tools/session-check.sh`; the arena walk on `arena250.session` (hashes over each bot's sync box, store under the game's cap) | 7-8 (with 7b) |
| 7b | **Built** (M-a and M-b; "Stage 7b plan" with "Amendments after the critic" (superseded where the code differs), "Stage 7b as built"; integrated and accepted 2026-11-17; reviewed ("Review of stage 7b") and rechecked ("Recheck of stage 7b"), not committed). `HashCheck` repair every 2 s, server terrain mode, the wire `EntityCheck` with the list re-sent and bounded tombstones; the Windows-build fixes | The whole test table on a 250 m region (`Region250.*`); the drill, the UDP drill, `tools/session-check.sh` and the arena walk with 0 check mismatches; terrain mode end to end (`--bot-broken-generator`, the game's `--other-terrain`) with equal hashes and its bytes measured on the playground and the arena | (above) |
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
**Decided (owner, 2026-11-16): (a).** The threshold stays at 0.512 m^3. The owner put fun gameplay with few bugs
ahead of realistic synced rubble: crates break at their first hit, and only large pieces are shared.

### Stage 7a plan (2026-11-16)

Status: **superseded by "Stage 7a as built"** (both milestones built, integrated and accepted on 2026-11-17). Kept as
the record of what was planned and why: where this plan, its amendments (at its end, which win over the sections
above them) and "Stage 7a as built" disagree, "as built" describes the code. Stage 7 is built in two runs. **7a** (this plan): chunk sync for joins and for a client
window smaller than the region, as "Chunk sync" and "Entity replication" define it. **7b** (next run, not blocked by
anything here): `HashCheck` repair every 2 s, server terrain mode, the wire `EntityCheck` with the list re-sent, the
whole test table re-run on a 250 m region. Code facts were read at engine `40e70b7` / game `092cf91`. "est." = estimate,
to be replaced by the measurement named.

#### What the code gives 7a

| Code fact (40e70b7) | Consequence for 7a |
|---|---|
| `World` has no `SetChunkEncoded`, `TakeLoadedChunks`, `WindowRevision` or chunk encoder; `ReadChunk` decodes any loaded cell, `PackedData` returns a Packed cell's RLE; `EncodeChunk` is canonical (kind byte, then `[id u8][len u16]` runs in block order: at most 1 + 3 x 32 768 = 98 305 bytes). | Four small `World` additions (below). The wire form of a chunk is the existing codec, no new format. |
| Cells become loaded in two places: `IntegrateResults` (job results) and `PlaceCell` (sky and rock rows installed by the column pass, no job). A Loading cell is guarded by `Cell::generation`; a surface row counts in `Column::surfaceLeft` until it loads, and `LoadBlocking` / `IsFilled(Surface)` wait for 0. | A load report must cover both paths. Installing a chunk into a Loading cell must bump its generation and settle the column's counters, or a stale job overwrites it or `LoadBlocking` never ends. |
| `UnloadCell` persists a modified cell into the `RegionStore`, which keeps it **in memory** when there is no save directory; it reloads `fromSave` when the window comes back. | A session client that walks away and back would get its own old copy with an unknown base. 7a turns that off for session clients (below). |
| `EditRegion` refuses a whole edit that would put solid voxels above the build ceiling, then visits chunks (y, z, x). `CarveSphereQInChunk` clips the box **before** that test. | A per-chunk clip must clip after the whole-op ceiling decision; `CarveSphereQInChunk` becomes the clip with that order (it is used only by the offline retry, where the whole carve was already applied, so nothing changes there). |
| `Server::Impl::log` holds every event of the session; a joiner's cursor starts at 0; `Pump` holds a peer's next batch while `ReliableBytesInFlight(peer)` (every channel of that peer) is over 32 kB. A joined peer's events-channel messages are ignored. `BodyTrack` knows only `resting`; template lights are emitted once and not kept. | The log becomes a backlog; the server takes client messages on the events channel; rest ticks / poses and lights are kept for the baseline. |
| `Client` fails on an operation it cannot apply in full, requires `opSeq == applied + 1` from 1, hashes the whole region (`RegionHash`), builds its world with the region's window (`RegionWorldConfig`), loads it whole in `Attach` and checks `WindowIsRegions` and the 64 probes. | The Fail rule is replaced by chunk states; the stream starts at the join point; the hash box becomes explicit. |
| `Channel::Bulk` (2) exists in `ITransport`, `ImpairedTransport` and `EnetTransport` (3 ENet channels) and carries nothing. ENet's reliable window is per peer, so bulk delays events of the same peer. `ImpairedTransport::bytesPerSecond` queues every channel of a link behind each other, as a real link does. | Bulk needs no transport change; its pacing is the server's. |
| The game's offline window is 48 x 44 x 48 (`windowSize {48, 8, 48}`, full height); in a session `windowFollowsCamera = false`, store 12 288 slots with 2048 kept free (`kGpuStoreSlots`, `kSessionStoreHeadroom`). `arena250.session`'s window is 69 x 44 x 69, `playground.session`'s 30 x 44 x 30, `drill.session`'s 13 x .. x 13. | Only regions wider than 48 chunks need the moving window; playground and drill keep the fixed one, so `tools/session-check.sh` and every stage 3-6 session test keep their windows. |

#### Messages (protocol 8)

`kProtocolVersion` = 8. New `MessageType` values 8-14; `RefuseReason::LogTooLarge` (6) is retired, never sent and never
reused (like type 5). `Welcome::logEnd` keeps its bytes and is renamed `joinOpSeq` (J): the stream position the joiner
starts after; nothing at or before J is ever sent to it as an event. Client-to-server messages go on the events channel
(reliable, ordered; all under ENet's 64 KiB listening limit). Every decoder refuses trailing or missing bytes.

| Type | Message | Channel | Layout (little endian; runs as in `WriteMaskRuns`) | Bytes |
|---|---|---|---|---|
| 8 | `WindowUpdate` | client -> server, events | type u8, `windowSeq` u32 (1, 2, ... per client), `flags` u8 (bit 0 `reset`: every chunk of this window counts as entering; bit 1 reserved for 7b's server terrain mode, refused in 7a; others refused), `minChunk` 3 x i32, `size` 3 x u16 | 24 |
| 9 | `WindowAck` | server -> client, events | type u8, `windowSeq` u32, `asOfOpSeq` u32 (A), `size` 3 x u16 (must equal the update's), then mask runs over the window box (bit `chunk - minChunk`, x fastest, then y, then z, as `VoxelMask::Index`; not toroidal): bit set = an entering chunk the server's modified map holds at A; it comes as `ChunkData`. Clear bits of entering chunks: pristine at A. | 15 + runs (est. 2-40 for a step, 0.1-4 KB at a join) |
| 10 | `ChunkData` | server -> client, bulk | type u8, `chunk` 3 x i32, `asOfOpSeq` u32 (B), then the `EncodeChunk` bytes to the end of the message (kind 0: 1 byte; kind 1: 2 bytes, id != 0; kind 2: `(n - 1) % 3 == 0`, runs > 0, summing to 32 768) | 17 + 1..98 305 (est. 0.3-8 KB for an edited surface chunk) |
| 11 | `ChunkRequest` | client -> server, events | type u8, `count` u16 (1-256), `count` x (3 x i32) | 3 + 12 n |
| 12 | `ModelRequest` | client -> server, events | type u8, `count` u16 (1-64), `count` x netId u32 (non-zero) | 3 + 4 n |
| 13 | `BaselineEntity` | server -> client, bulk | type u8, `asOfOpSeq` u32, `tick` u32 (the server tick it was encoded on), `restTick` u32 (`0xFFFFFFFF` = awake, or a light), then one `EntitySpawn` event in `WriteEvent` form (netId != 0; kind `Light` 50 B or `Object` 60 B + model; a resting object carries its `EntityRest` pose and zero velocity, flag `kObjectResting`) | 13 + 50, or 13 + 60 + model |
| 14 | `BaselineEnd` | server -> client, bulk | type u8, `asOfOpSeq` u32, `count` u32 (the `BaselineEntity` messages of this join's baseline actually sent) | 9 |

Refused by the decoders: a window size of 0 or over `kMaxClientWindowChunks` (128) per axis, a coordinate beyond
+-2^24 chunks, an ack whose runs do not add up to its box, a chunk body that is not a valid codec stream, request counts
outside their range, a baseline event that is not an `EntitySpawn` with a NetId, models over the batch decoder's voxel
budget. The server additionally refuses (and disconnects, as a broken reliable stream, counted `malformedRequests`) a
`WindowUpdate` whose `windowSeq` is not the previous + 1, whose rows are not the region's (`size.y`, `minChunk.y` =
`sim::RegionRows`), or that comes before the peer joined. Requests for chunks outside the peer's window or the server's
window, and for unknown NetIds, are ignored and counted.

#### Server: state and behaviour

Everything below lives in `net::Server::Impl` (`src/net/internal/Server.cpp`), the per-peer part in `Peer`; the pieces
that can be tested alone (the pacer, the chunk key) go into a new partition `vortex.net:sync` (`src/net/Sync.cppm`,
`internal/Sync.cpp`) together with the client's table.

- **Modified map** `modified: std::unordered_map<uint64_t ChunkKey, uint32_t opSeq>`, fed in `Emit` for every
  `WorldOp` event (template operations at `Start` included): the chunk range of `op.Bounds()` (`sim::OpChunkRange`, the
  same function the client uses), clipped to the server window, each set to the event's opSeq. Never cleared. Sparse
  (it grows with edits, not with area), so stage 9's on-demand 1 km world needs nothing else. Over-approximate (a sphere
  box can be a voxel larger than its voxels; an out-of-bounds refusal still marks): such a chunk is sent although
  pristine, which is correct.
- **Event backlog** replaces the log: a deque of encoded events from the lowest `sentUpTo` of the joined peers to
  `LastOpSeq`; trimmed after every `Flush`. A joiner's cursor starts at J. A peer whose unsent events exceed
  `ServerSettings::maxPeerBacklogBytes` (8 MiB) is disconnected and counted (`peersTooSlow`): there is no catch-up
  path for a client that falls 8 MiB behind (7b's repair works per chunk, not per stream). `LogBytes()` becomes
  `BacklogBytes()`; `joinLogWarnBytes` / `maxJoinLogBytes` go.
- **Peer sync state**: the last acknowledged window (`windowSeq`, `min`, `size`; none before the first update), the bulk
  queue, the pacer and a join record (join tick, bulk bytes and messages sent, chunks sent, baseline entities, the tick
  the queue first ran empty).
- **`WindowUpdate`** (in `Receive`, at the start of the tick): entering = the new window minus the previous one (all of
  it with `reset` or for the first update), clipped to the server window. Chunks of entering whose key is in `modified`
  get their bit and join the bulk queue (deduplicated); chunks queued for the old window stay queued (they are dropped
  at dequeue if they left). The `WindowAck` is sent at once with A = `LastOpSeq()`, on the events channel. Because it
  travels on the events channel and A is the newest event made, **a client receives it with `applied <= A`**: every
  event it has applied was sent before the ack and is at most A. Cost: one map lookup per entering cell (2112 per
  chunk of a window step of a 48 x 44 x 48 window, 101 376 or 209 484 at a join; or a pass over `modified` when that is smaller).
- **Bulk queue** per peer, served in this order (choice 11): (1) `ChunkRequest` / `ModelRequest` answers, FIFO;
  (2) queued chunks within 32 m of that peer's player (horizontal distance of the chunk centre, server-side player
  state); (3) the entity baseline; (4) the remaining chunks, nearest first. Chunks are kept as coordinates sorted by
  horizontal distance from the player at enqueue time and re-sorted when the player crosses a chunk border (at most
  about two sorts a second per peer). **Encoding happens at dequeue**: a chunk no longer in the peer's window (or a
  NetId that no longer exists) is dropped and counted; else `World::EncodeChunkAt` and `asOfOpSeq = LastOpSeq()`, a
  baseline entity from its current state (below). Dequeue runs in `Flush`, after physics, rests and despawns of the
  tick, so every asOf names a complete tick.
- **Pacing** (choice 12; the design rejects a fixed rate). Per peer, in `Flush`: (a) events first - bulk is handed over
  only while the peer's event backlog is empty (`sentUpTo == LastOpSeq`), so in the server live events never wait
  behind bulk; (b) a bulk message is handed over only while `ReliableBytesInFlight(peer) < bulkWindow`; one message may
  overshoot. `bulkWindow = clamp(drainRate x (RoundTripSeconds(peer) + bulkTargetDelay), 4 KiB, 32 KiB)`, with
  `drainRate` an average (1/8 per tick) of the bytes the peer's in-flight count fell by per second while it stayed above
  zero (when it reached zero the link was faster: the sample is at least the drained bytes over the tick), starting at
  8 KiB per (RTT + delay). `bulkTargetDelay` = 0.1 s, so events queue in the transport behind at most about 100 ms of
  bulk; the 32 KiB ceiling is the design's "at most 32 kB of bulk inside the transport". (c) An optional hard cap
  `maxBulkBytesPerSecond` (default 0 = none; a credit per tick) for tests and admins. Events keep their own hold rule,
  now against `maxEventBytesInFlight + bulkWindow`, so a full bulk window never holds the live stream.
- **Requests**: `ChunkRequest` puts each chunk (inside the peer's window and the server's) at the end of the request
  FIFO unless it is queued there already (a chunk queued further back moves to the FIFO); `ModelRequest` puts each live
  NetId there. A request for a NetId that is gone sends nothing: its `EntityDespawn` is in the event stream.
- **Entity baseline** at the join (choice 13): the NetIds alive at J (every `scene::NetId` entity, and the template
  lights, now kept as `lights: netId -> TemplateLight`) are queued in NetId order. At dequeue each becomes a
  `BaselineEntity`: a light as its `EntitySpawn`; an object from `DescribeObject` (pose, velocities, motion, model) -
  if it rests, the pose of its `EntityRest` (bit for bit) and zero velocity, with `restTick`. `BodyTrack` therefore gains
  `restTick`, `restPosition`, `restRotation`, set where `TrackBodies` emits `EntityRest` (and at spawn for objects
  created resting: rest tick = spawn tick, pose = spawn pose). After the last one a `BaselineEnd` with the count sent.
  No tombstone list is sent: a NetId despawned before J never reaches the joiner (it is not in the set), one despawned
  between J and its dequeue is dropped at dequeue, and every despawn after J reaches the joiner on the events channel
  (which tombstones it client-side). Entity events after J are sent live as before.
- **Stats** (`ServerStats` and a per-peer `JoinRecord`): window updates, acks and their modified-chunk count, chunk data
  sent / dropped at dequeue / bytes, requests by kind, baseline entities, bulk bytes, bulk holds, the largest and
  smallest bulk window, the peak bulk rate, peers too slow, the modified map's size.

#### Client: state and behaviour

The client's chunk state lives in `net::ChunkSync` (`vortex.net:sync`), owned by `Client::Impl`. GPU-free and testable
against a `World` without a transport.

- **Table**: one record per cell of the client's window, indexed like the world window (column-major
  `(x + z * sx) * sy + y`, toroidal in x / z, rows fixed - the published contract), holding `coord`, `state`, `base`,
  `enteredSeq` and `ready`. States: **NoBase** (entered with update k, no ack yet: accepts no events), **Awaiting**
  (the ack said "modified": waits for `ChunkData`), **Based** (`base` = the ack's A for a pristine chunk, a
  `ChunkData`'s B for an installed one), **Requested** (a `ChunkRequest` is out), **Unsynced** (no store slot: content
  stale, waits for a free slot). A record whose `coord` is not the cell's any more (the world re-targeted the cell) is a
  new NoBase record. **Current** = Based, `ready` (loaded and caught up) - the only state events are applied to, the
  player may stand on and the hash counts.
- **Op ring** (choice 1-3): the terrain operations (kinds 1-6) of the stream position `(floor, applied]`, at most
  `ringOps` = 1024 or `ringBytes` = 1 MiB (memory: 40 bytes + the raw mask), whichever is less; a `Detach` keeps only its
  box and mask (`netId` 0, velocity 0). `floor` starts at J and rises with every eviction. A base is **covered** when
  `base >= floor`.
- **The rule**, `ChunkSync::Apply(world, seq, op, edits)` for every terrain operation of the stream (after it is pushed
  into the ring): the chunk range of the op (`sim::OpChunkRange`), each chunk inside the client window with
  `current && seq > base` passes. All pass and the range is inside the window: one `sim::ApplyOp` (the fast path, every
  op of a fixed-window client in steady play). Else `sim::ApplyOp(..., clip)` once per passing chunk in (y, z, x)
  order - exact, because every edit decides per voxel from `(voxel, old id)` and the ceiling is decided on the whole op
  (World APIs below). `report.unsynced` chunks become Unsynced; a passing chunk reported `skipped` (the world dropped
  it unseen) loses `ready`. The result says whether the op was **complete here** (every chunk of its range passed).
- **Load reports**: at the start of every `Poll` and at the end of `Attach`, `World::TakeLoadedChunks()`; a reported
  chunk still in the window and loaded becomes `ready` when Based and covered, after replaying the ring's ops in
  `(base, applied]` whose range holds it, each clipped to it; Based but not covered: Requested, never a partial replay;
  NoBase / Awaiting / Requested / Unsynced: nothing yet.
- **`WindowAck` k**: records with `state == NoBase && enteredSeq == k` become Awaiting (bit set) or Based(A) (bit
  clear; `ready` at once if loaded, after the same replay - empty in practice, since `applied <= A`). An ack for a seq
  this client did not send fails it (malformed). Records in any other state are not touched (a `ChunkData` may have
  arrived first on the other channel).
- **`ChunkData`** (coord, B): dropped when the chunk is not in the window (`staleChunkData`) or its record is current
  with `base >= B`; else `World::SetChunkEncoded`: Installed -> Based(B), `ready` after replaying `(B, applied]` (empty
  when the bulk is ahead of the events; when the ring no longer covers B: Requested); StoreFull -> Unsynced.
- **Requests**: Awaiting / Requested chunks are not re-requested (the answer is coming; each coordinate is requested at
  most once per entry into the window); Unsynced chunks are requested, nearest the player first, only while the store
  has more than `requestReserveSlots` (64) free and only as many as are free beyond it; a request moves the record to
  Requested. Requests go out in one `ChunkRequest` per `Poll`, at most 256 coordinates.
- **Window updates**: after the world's first `Update` and whenever `World::WindowRevision()` changed since the last
  update sent: a `WindowUpdate{seq + 1, min, size}`; cells that entered since get `enteredSeq` = that seq. If the
  revision moved by more than one since the last check (two window moves the client did not see apart), the update
  carries `reset` and every record of the window becomes NoBase: a chunk that left and came back between two checks was
  regenerated pristine, and only a reset makes the server send it again.
- **Ops before `Attach`, bulk before `Attach`**: kept as now (`pending`), bulk messages in a `pendingBulk` deque;
  processed in arrival order once Playing. The stream starts at J: a batch at or below J is a duplicate.

How the base rule holds on every path:

| Path | What happens | Why the chunk equals the server's at max(base, applied) |
|---|---|---|
| Live event, chunk current | applied (fast or clipped path) | `base < seq = applied + 1`: the chunk was current at `applied`, the op is the next. |
| Live event, chunk NoBase / Awaiting / Requested / Unsynced | skipped, kept in the ring | Its content is not trusted; it becomes current only through `ChunkData` (whose B covers this event if `seq <= B`, or replays it if `seq > B`). |
| Live event, data ahead (`base >= seq`) | skipped | `ChunkData` at B already holds every event up to B. |
| Pristine chunk, loaded when its ack arrives | Based(A), replay `(A, applied]` (empty) | The server's chunk is unmodified at A, so equal to the generator's; events after A apply live. |
| Pristine chunk that loads late (generation still running, or entering a moving window) | Based(A) on the ack, `ready` on its load report after replaying `(A, applied]` clipped to it; not covered: one `ChunkRequest` | While it loads, live events report it `skipped` and change nothing; the replay gives it those events in stream order. |
| `ChunkData` behind the stream (B < applied) | install, replay `(B, applied]` | Content at B plus the ops after it in order. Covered because B was encoded at dequeue (the ring covers transit, not the queue). |
| `ChunkData` ahead of the stream (B > applied) | install, events `<= B` skipped as they arrive | The carve + swap test pins it. |
| Chunk leaves the window and comes back | the cell is re-targeted: dropped (session worlds keep no unloaded edits, choice 9), record NoBase; on re-entry the ack marks it modified -> `ChunkData`, or pristine -> Based(A) | The server's modified map knows every edit while it was away. |
| Two window moves between checks | `reset`: the whole window NoBase, the server resends every modified chunk of it | A chunk that left and came back unseen was regenerated. |
| Store full on an edit or a replay | `report.unsynced` -> Unsynced (the op changed nothing in it: all-or-nothing per chunk) | Not trusted, not hashed, not walkable; requested again once slots are free, then installed whole. |
| Store full on `ChunkData` | `SetChunkEncoded` changes nothing -> Unsynced, not re-requested until slots are free | As above; "no repeated request for the same chunk" is a test. |
| Ring no longer covers a base | Requested | Never a partial replay. |

**Entity baseline against live entity events** (all in `Client::Impl`; per copy a `copyBase` opSeq):

| Case | Rule |
|---|---|
| `BaselineEntity` (n, B, tick, restTick) for a tombstoned n, or a copy of n that exists | discarded (counted) |
| ... otherwise | copy made from the spawn, `copyBase = B`; resting: `RemoteBodies::Rest(n, restTick)`, `Layer::Dynamic`; awake: the pose is its first sample at `tick`; then the held events of n with opSeq > B are applied in order, those <= B dropped |
| Live entity event (spawn excluded) for n with a copy and `seq <= copyBase` | dropped: the baseline (or answer) already holds it |
| Live entity event for n without a copy, while the join baseline is open (no `BaselineEnd` yet) or n was asked for (`ModelRequest`) | held per NetId in opSeq order (at most 64 per id, 4096 in all; overflow drops them and asks for the model again) |
| ... otherwise | ignored and counted (`ignoredEntityEvents`, as today) |
| `EntityDespawn` for any n | tombstone n, drop its held events, remove its copy |
| `ObjectModel` for an unknown n | held; applied after the spawn if `seq > B` ("replaces an older baseline model"), dropped if not |
| `EntityRest` / `EntityWake` for an unknown n | held; the rest sets the rest tick and pose exactly as a live one does |
| `BaselineEnd` | held events of NetIds still without a copy and not asked for are dropped (counted); the start gate may open |
| Live `Detach` with `netId` n, complete here | voxels cleared, copy built from the removed ids as today, `copyBase = seq` |
| ... not complete here (any chunk of its range not current, ahead, or outside the window) | the passing chunks are cleared; no piece is built from a partial volume; netId 0: no rubble; netId n: one `ModelRequest` (n is "asked for"), the answer is a `BaselineEntity` |
| Snapshot samples of a NetId without a copy | ignored, as today; they wait for their events first, as today |

**The player starts** (`StartPrediction`) only once every chunk within `startRadius` (32 m) of the snapshot's player
position is current **and** `BaselineEnd` has arrived (choice 14); the fresh-snapshot rule (`startFreshTicks`) stays.
**Movement gate on the client** (choice 15): a predicted or replayed step whose `PlayerReach` box holds a chunk that is
not current does not move the player (counted `syncHeldTicks`); the server is not held, so the next snapshot corrects
it (the server's own gate, `HasCollider`, is unchanged on both sides).

**`Fail` on an incomplete op is removed**: an op a client cannot apply in full is the normal state of a chunk that
waits for data. A client still fails on a gap in the stream, a malformed message or an ack for an unsent seq.

#### World and sim APIs

| API | Module | Semantics | Tested by |
|---|---|---|---|
| `struct ChunkClip { glm::ivec3 min, max; }` (chunk box, inclusive) and a trailing `const ChunkClip* clip = nullptr` on `FillBox`, `FillSphereQ`, `CarveSphereQ`, `SwapSphereQ`, `ClearMask` | `vortex.voxel` `:world` (new) | `EditRegion` decides the build ceiling on the whole op, then visits only chunks inside the clip, in the same (y, z, x) / (z, y, x) order; skipped / unsynced / changed chunks only inside it. `CarveSphereQInChunk` = `CarveSphereQ` with a one-chunk clip (ceiling on the whole op: the offline retry is unchanged, its whole carve was never refused). | `World.EditClipEqualsTheWholeEditChunkByChunk` |
| `bool EncodeChunkAt(const glm::ivec3& chunk, std::vector<uint8_t>& out) const` | `vortex.voxel` `:world` (new) | The `EncodeChunk` form of any loaded chunk: a Packed cell's bytes copied, a Dense one encoded (Empty / Uniform when its content is, as `PersistCell` does), Uniform / Empty in 1-2 bytes. False when not loaded. | `World.EncodeChunkAtRoundTripsThroughSetChunkEncoded` |
| `SetChunkResult SetChunkEncoded(const glm::ivec3& chunk, std::span<const uint8_t> rle)` -> `Installed`, `OutsideWindow`, `Malformed`, `StoreFull` | `vortex.voxel` `:world` (new) | Replaces the content of a window cell in any state (Unloaded, Loading, Empty, Uniform, Dense, Packed). A content change: `modified = true`, `QueueChange` (colliders, renderer, `ChunkRevision`), `SetGrid`, the column's top, kept bytes dropped, face neighbours promoted when packing. A Loading / Unloaded cell: generation bumped (its job result is discarded), its row bits cleared and `surfaceLeft` settled, so `LoadBlocking` / `IsFilled` still finish. Dense needs a slot: `StoreFull` leaves the cell exactly as it was. Not reported by `TakeLoadedChunks`. | `World.SetChunkEncodedInstallsAnyChunkAndIsAContentChange` |
| `std::vector<glm::ivec3> TakeLoadedChunks()` and `WorldConfig::reportLoads` (default false) | `vortex.voxel` `:world` (new) | Every cell that became loaded by streaming since the last call (job results and the column pass's sky / rock cells), once per load, in load order; not edits, packing, unpacking or `SetChunkEncoded`. Opt-in, so offline worlds hold no list. A coordinate re-targeted before the take is still listed: the taker checks `IsChunkLoaded`. | `World.TakeLoadedChunksReportsEveryLoadOnce` |
| `uint64_t WindowRevision() const` | `vortex.voxel` `:world` (new) | +1 per `Recenter` (the window's minimum changed), nothing else. | same test |
| `WorldConfig::keepUnloadedEdits` (default true) | `vortex.voxel` `:world` (new) | false: `UnloadCell` does not persist a modified cell; a chunk that comes back is generated again. | `World.UnloadedEditsAreDroppedWithoutKeepUnloadedEdits` |
| `ApplyOp(world, op, edits, const voxel::ChunkClip* clip = nullptr)`; `OpChunkRange(op, min, max)` | `vortex.sim` `:worldop` | The clip forwarded; the chunk range of `Bounds()`. | `WorldOps.ApplyOpClippedToEachChunkEqualsTheWholeOp` |
| `ClientWorldConfig(region, windowChunks, base)`, `ClientWindowMoves(region, windowChunks)` | `vortex.sim` `:region` | M-b: the region's window when it fits in `windowChunks` (x and z), else a `windowChunks`-wide window with the region's rows; `reportLoads` true, `keepUnloadedEdits` false. | `ClientWindow.*` |
| `WorldHash`, `ReadChunk`, `PackedData`, `IsChunkLoaded`, `StateOf`, `ChunkRevision`, `Simulation::UpdateStreaming(windowFocus, focus)` | existing, read for this plan | unchanged | - |

`MakeSimulationConfig` sets `reportLoads` (and in M-b uses `ClientWorldConfig`); `Attach` refuses a world without it,
as it refuses the other session flags.

#### The moving client window (M-b)

- `ClientSettings::windowChunks` (0 = the region's window, every existing caller; the game and the arena bots pass
  48, the game's offline width). When the region's window fits in it (x and z), nothing changes: the region's window,
  fixed, loaded whole in `Attach` (playground, drill: the session check and every stage 3-6 test). Otherwise the
  client world is `windowChunks` x the region's rows x `windowChunks`, follows the player in x / z and is never clamped
  to the region (choice 18): columns beyond the server's window are pristine for good (no operation reaches them, or
  the server would fail), so they are Based on every ack.
- Streaming focus: `Client::RunTick` calls `UpdateStreaming(player feet, focus)` (the spawn before prediction starts);
  the game sets `windowFollowsCamera = true` (the camera is at the player's eye, or behind it in V view: the window
  is 192 m wide, the difference does not matter). `packHidden` as offline.
- `Attach` places the window at the spawn and loads `LoadScope::Surface` (polling), then returns; the rest streams. It
  checks the rows and size instead of `WindowIsRegions`, and the probe chunks that lie inside the loaded window only
  (the generator-side probe check of `OnWelcome` is unchanged). The store is the game's 12 288 slots with
  `minFreeStoreSlots` kept free (est. 5300-5700 dense at a 48 x 44 x 48 window, view-distance.md stage 3).
- Per window step (a chunk border crossed: about 2 per second at run speed): one `WindowUpdate` (24 B), one ack
  (est. 20-60 B), `ChunkData` for the modified chunks of the entering slab. The server's queued chunks that left are
  dropped at dequeue.
- Entities are not windowed (interest management is out of scope for M1): every replicated body is copied wherever it
  is; a `Detach` outside the window asks for its model. Kinematic copies are not guarded by `GuardBodies`; client rubble
  outside the window freezes.

#### Comparing a window-sized client's hash

The **sync box** of a client = its window (from its last `WindowUpdate`) intersected with the server's window
(`net::SyncBox`); for a fixed-window client it is the region's window, so today's `RegionHash` comparisons are the
special case. `Client::Hash()` = `WorldHash` over the sync box, with every chunk of it that is not current counted in
`notLoaded` (incomplete). `Server::Hash(minChunk, maxChunk)` hashes any box. A comparison is valid only **at the same
opSeq**: after a drain, client `AppliedOpSeq() == Server::LastOpSeq()`, and `Client::Synced()` (every chunk of the
window current, no request out, the baseline ended). The game's headless drain and `vortex_server`'s drain wait for
`Synced()` too. Logs: the client's line stays `world hash H (N chunks)` with ` over [min..max)` appended; the server's
"client left" line adds `window hash H over [min..max)` for that client's sync box (equal to the region hash for a
fixed window, so `tools/session-check.sh`'s regexes keep matching).

#### Tests

All GPU-free and seeded; network tests over `ImpairedTransport` on the loopback with the manual clock of the
`Session` / `Arena` / `Match` fixtures, which wait for idle between ticks (stage 6 reproducibility rule).

| Test | M | Asserts | Fails if sync were broken because |
|---|---|---|---|
| `World.EditClipEqualsTheWholeEditChunkByChunk` | a | each Q kind and `ClearMask` / `FillBox`: one clipped call per chunk of the box in (y, z, x) order = the whole call (voxels, edit list in order, report); an op that reaches above the ceiling is refused whole even when clipped to a chunk below it | a clip that re-orders visits or decides the ceiling per chunk gives other voxels |
| `World.SetChunkEncodedInstallsAnyChunkAndIsAContentChange` | a | install into each cell state; `ReadChunk` / `ChunkHash` equal the source; `ChunkRevision` changed; listed by `TakeChunkChanges`; a held job result for that cell arrives later and is discarded; `LoadBlocking` still returns; `StoreFull` leaves the cell bit for bit | a stale job overwrites the data, or `surfaceLeft` never reaches 0 (the test would hang: run under a timeout) |
| `World.EncodeChunkAtRoundTripsThroughSetChunkEncoded` | a | Dense, Packed, Uniform, Empty: encode -> install in another world -> equal `ChunkHash`; a Packed cell's bytes are its own | wrong bytes on the wire |
| `World.TakeLoadedChunksReportsEveryLoadOnce` | a | after `LoadBlocking` every window cell once (column-pass cells included); after a one-chunk step only the entering slab; nothing for edits, packing, unpacking, `SetChunkEncoded`; `WindowRevision` +1 per step; nothing with `reportLoads` false | a pristine chunk that loads late never gets its replay |
| `World.UnloadedEditsAreDroppedWithoutKeepUnloadedEdits` | b | an edited chunk that leaves and comes back is pristine (default: the edit comes back) | a returning chunk would carry an old copy with no base |
| `WorldOps.ApplyOpClippedToEachChunkEqualsTheWholeOp` | a | every `WorldOpKind`, `BuildDetachedPiece` from the concatenated edits equals the whole op's piece | as the World test, through `sim` |
| `Wire.ChunkSyncMessagesRoundTripAtTheirSizes` (+ the forged-bytes test extended) | a | each new message at its size, the refusals listed above, random bytes never crash | a decoder that takes a bad codec stream installs garbage |
| `ChunkSync.AnEventIsAppliedOnlyAboveTheBase` | a | a `World` + `ChunkSync`, no transport: chunks based at A, at B > applied, without a base and unsynced; one op stream; each chunk's hash equals a reference world's at max(base, applied) for current chunks; the others are untouched | `>=` instead of `>`, or events applied to unbased chunks, changes those hashes |
| `ChunkSync.AChunkStillLoadingGetsItsEventsWhenItLoads` | a | a generator gated per chunk holds C Loading while three ops cross it (`skipped`); released, the load report replays them: hash of C equals the reference; second run with a ring of 2 ops: C becomes Requested, one request, no partial replay, the hash is incomplete | a loading chunk keeps the pristine content, or a partial replay gives other voxels |
| `ChunkSync.RingKeeps1024OpsOr1MiB` | a | eviction by count and by bytes (one large mask), `floor` rises, a `Detach` in the ring has netId 0 and no velocity | coverage claimed for evicted ops |
| `Client.BulkAheadOfEventsWithACarveAndSwapOnTheSameVoxels` (FakeServer) | a | a carve at opSeq 1 and a swap air -> stone at 2 over the same voxels. Order 1: `ChunkData`(asOf 2) before the batch 1..2; order 2: batch 1..2 before `ChunkData`(asOf 1), the swap replayed. Both: equal `WorldHash` with a reference, two events skipped for the chunk in order 1 | replaying the carve after the swap (or applying either twice) leaves a hole instead of stone |
| `Client.ASmallStoreMarksChunksUnsyncedAndRequestsEachOnceWhenSlotsFree` (FakeServer) | a | the test fills the client's store (`ChunkStore::Alloc`), the server edits N chunks: N Unsynced, 0 requests, no `Fail`; slots freed: exactly one request per coordinate (counted by the fake server), answered; hash complete and equal | repeated requests flood bulk; a client that trusts a stale chunk differs |
| `Client.ALateJoinerPresentsOnlyEventsAfterItsJoinPoint` (rewrite of `ALateJoinerPresentsOnlyLiveEvents`) | a | Welcome J = 3, ack marking the chunks of ops 1..3, their `ChunkData`, then op 4: one presentation, hash = reference after 4 | a joiner replaying history presents it |
| `Client.EntityEventsForAnUnknownIdWaitForItsSpawn` (FakeServer) | a | `ObjectModel` (5) and `EntityRest` (6) before `BaselineEntity`(asOf 4): applied after it (model hash and rest pose exact); with asOf 7 they are dropped; a tombstoned id's baseline is discarded; after `BaselineEnd` unknown ids are ignored | an older baseline model kept, or a rest applied twice |
| `Client.ADetachOverChunksWithoutABaseAsksForTheModel` (FakeServer) | a | netId 0: no rubble, no request; netId n: one `ModelRequest`, the copy from the answer with the server's model hash; a later `Detach` replay does not make a second copy | a piece built from a partial volume has another model hash |
| `ChunkSync.LateJoinDuring10CastsPerSecondConverges` | a | the drill at 10 casts/s with object splits (two-cube dumbbells, `Server::SpawnObject`) and crates beside client A; client B joins mid-run; every link 5 % loss, 40 + 0-30 ms; B's downlink 64 kB/s and `maxBulkBytesPerSecond` = 32 kB/s. Converges: after the drain equal hash over B's sync box, the same NetIds, models and rest states / poses on all sides (`CompareEntities`); B's average bulk rate over its join <= 32 kB/s; **event latency p99 < 15 ticks (250 ms)** for A and B from B's Welcome until B is synced (ticks from an event's emit to its apply); B started only with its 32 m current; join bytes and ticks printed. A second run with an unpaced bulk (window unlimited) must show a worse p99 (the pacing is what helps) | wrong bases -> other hash; bulk ahead of events in the server or an unbounded window -> p99 over 250 ms |
| `Session.ATemplateReachesAJoinerAsChunkDataAndBaseline` (rewrite of `TemplateRunsOnTheServerAndReachesClientsAsEvents`) | a | the joiner receives no event at or before J; the plaza's chunks come as `ChunkData`, lights and crates as `BaselineEntity` (crates resting, `Layer::Dynamic`); equal hash and entities | the template replayed or missing |
| `Entities.ALateJoinerDuringACollapseGetsTheSameBodies` | a | joins while 64 bodies are awake; after the settle `CompareEntities` agrees (NetIds, models, rest states, rest poses and rotations exact), no copy lost | a rest tick or pose lost in the baseline |
| `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing` (adapted `ALateJoinIsPaced...`) | a | on a shared 48 kB/s uplink A's worst live latency during B's join is under half of the unpaced run's; `bulkHeld` > 0 | bulk crowding the live stream |
| `Session.ASlowPeerIsDisconnectedWhenItsBacklogOutgrowsTheLimit` (replaces `AJoinIsRefusedOnceTheEventLogIsTooLarge`) | a | a peer that stops polling is disconnected past `maxPeerBacklogBytes`; the backlog is trimmed to the slowest joined peer | unbounded server memory |
| Stage 3-6 joins kept: `SessionAcceptance.TwoClientsAgreeAfter200BlastsAt5PercentLoss` (B after blast 100), `Entities.PlayersCratesAreLimitedAndLeaveWithThePlayer` (late joiner), `Client.AJoinerStartsPredictingFromAFreshSnapshot`, `Entities.OneScriptGivesTheSameBodiesTwice` | a | unchanged assertions with joins by chunk sync | - |
| `ClientWindow.ARegionThatFitsKeepsTheFixedWindow` | b | `windowChunks` 48 on playground / drill regions: the stage 6 config (window, capacity), one `WindowUpdate` only | M-b leaking into the fixed-window path |
| `ClientWindow.APlayerWalksInFromOutsideIntoChunksModifiedBefore` | b | flat region 40 chunks wide, client window 12 (test-sized); 30 casts on the far side before; a bot walks across (a waypoint pilot): window updates sent, the far chunks arrive as `ChunkData`, no `syncHeldTicks` while walking at 7 m/s, store under its cap, no coordinate requested twice; at the drain equal hash over the sync box and equal entities | an entering modified chunk taken as pristine |
| `ClientWindow.AChunkThatLeavesAndComesBackAfterEditsIsSentAgain` | b | chunk C edited, the bot walks until C leaves, C edited again, the bot walks back: C arrives with an asOf after the second edit; a pristine chunk that leaves and returns gets no `ChunkData`; equal hash | the client's old copy or the pristine one kept |
| `ClientWindow.QueuedChunksThatLeaveTheWindowAreDropped` | b | a teleport while 200 chunks are queued: the server drops those outside (counted), a late one that still arrives is ignored (`staleChunkData`), equal hash | data installed outside the window, or a stale base |
| `ClientWindow.TwoWindowMovesUnseenResetTheWindow` | b | two `Recenter`s between client checks -> `reset`, the server resends the window's modified chunks, equal hash | a chunk regenerated pristine behind the server's back |
| `ClientWindow.HashesCompareOverTheSameBoxAtTheSameOpSeq` | b | `Client::Hash()` incomplete while any chunk of the sync box awaits data; after the drain equal to `Server::Hash(box)`; one changed op in the stream gives another hash | a comparison that cannot fail |
| `ClientWindow.ThePlayerIsHeldOverChunksWithoutABase` | b | a bot steps onto a chunk whose data is held back: held ticks counted, one correction after it arrives, no snap | walking on pristine content of a modified chunk |

About 26 new tests (634 -> about 660), three rewritten, one removed (`AJoinIsRefusedOnceTheEventLogIsTooLarge`). The
FakeServer fixture gains `AckWindow(asOf, modified)`, `SendChunk(world, coord, asOf)` and request counters; its
`Attach()` answers the client's first `WindowUpdate` with an all-pristine ack at A = the last op sent.

#### `vortex_server`, `tools/session-check.sh`, the arena walk

- **Logs**: per joiner `Join: client N (slot S): window W chunks, M modified; X ChunkData / K KiB, B baseline entities /
  K KiB, R requests; queue empty after T s; bulk window min..max KiB, peak K KiB/s`, and from the bot's client `Join:
  bot N started after T s (32 m current), synced after T s, K KiB bulk received`. The drill command of `CLAUDE.md` must
  still exit 0 with `hashes equal`, 67/67, 67/67, 66/66 and every `Entities: ... agree`; its late joiner now joins by
  chunk sync (est. 0.2-1 MB of bulk instead of the 205 KB log: chunk bytes are not op bytes; measured and written down).
- **New options**: `--client-window N` (the bots' `windowChunks`, default 0), `--walk-across` (bot 0's pilot walks from
  the play bounds' -x edge to the +x edge along the middle z and back; a waypoint mode of `BotPilot`), `--precasts N`
  (N server casts spread along that line before the walker joins, so it walks into chunks modified before),
  `--memory` (logs `VmRSS` from `/proc/self/status` and the server world's dense / packed counts and RLE MiB).
- **Arena walk** (M-b acceptance run): `vortex_server --session=engine/tools/server/sessions/arena250.session
  --clients 2 --client-window 48 --walk-across --precasts 40 --join-step 20 --drill 40 --loss 0.05 --delay-ms 40
  --jitter-ms 30 --memory` must exit 0 with both bots' sync-box hashes equal to the server's and entities agreeing, and
  logs join bytes and time (est. 1-3 MB, seconds on the loopback), each bot's largest store use (gate: under 12 288 -
  2048; est. 5300-5700), the process RSS (server plus two bot simulations; the server alone was 463-488 MiB in stage 3)
  and the server world's RLE MiB (gate 150). A game client on the arena (`--connect` to that session) is the owner's
  visual check, not an acceptance.
- **`tools/session-check.sh`**: unchanged commands; must PASS (playground fits the 48-chunk window, so the hash box is
  the region). The game's `Session.cpp`: `windowChunks = 48` and `windowFollowsCamera = sim::ClientWindowMoves(...)`,
  the drain waits for `Synced()`, the `Join:` line, and the HUD's bottom panel says `JOINING: N chunks` until the start
  gate opens (HUD, not ImGui).

#### Milestones

**M-a: join by chunk sync and the entity baseline, the client window unchanged.** World APIs (clip, `EncodeChunkAt`,
`SetChunkEncoded`, `TakeLoadedChunks`, `WindowRevision`, `reportLoads`); `sim::ApplyOp` clip and `OpChunkRange`;
protocol 8 messages; `vortex.net:sync` (`ChunkSync`, the ring, the pacer, `SyncBox`); server: modified map, backlog
instead of the log, window updates and acks, the bulk queue encoded at dequeue, pacing, requests, the baseline with rest
ticks and lights; client: the table, the rule on every path, unsynced instead of `Fail`, model requests, held entity
events, the start and movement gates, `Hash()` over the sync box (= the region), `Synced()`; the fixed region window
still loaded whole in `Attach`, which now sends the one `WindowUpdate`. `vortex_server` join logs; the game's drain and
`Join:` line. Done when: warning-free build, all tests green (the M-a rows above and every stage 3-6 test), both
headless game runs exit 0 with their usual lines, the CLAUDE.md drill command exits 0 with equal hashes and agreeing
entities, `tools/session-check.sh` PASS, join bytes / time measured; docs: "Stage 7a as built" (first half),
`notes/net.md` (the base rule, the ring, held entity events, join = J), `notes/voxel.md` (the new World calls, the
column-bookkeeping trap), ARCHITECTURE.md 7.5, ROADMAP, the game's CLAUDE.md (join, the drill's join lines).
Est. 4-5 days.

**M-b: a moving client window smaller than the region.** `ClientSettings::windowChunks`, `sim::ClientWorldConfig` /
`ClientWindowMoves`, `keepUnloadedEdits`, streaming around the player (`RunTick`, the engine's `windowFollowsCamera`),
`Attach` with `LoadScope::Surface` around the spawn, window updates on `WindowRevision` with `reset`, the server's
entering sets and dequeue drops, the probe check inside the window, the hash box logs, the game's `JOINING` HUD line,
`vortex_server --client-window / --walk-across / --precasts / --memory` and the arena walk run. Done when: warning-free,
all tests green (the M-b rows), the arena walk exits 0 within its gates with the numbers recorded, the drill command and
`tools/session-check.sh` as in M-a, both headless game runs exit 0; docs completed ("Stage 7a as built", the stages
table row 7 split into 7a / 7b, view-distance.md's networking paragraph: a session client window is now the game's).
Est. 3-4 days.

#### Choices where the design is open

| # | Question | Choice | Why |
|---|---|---|---|
| 1 | Where is the ring? | On the client (replay happens where chunks are installed); the server keeps only the unsent backlog per peer, bounded by `maxPeerBacklogBytes`. | The server never replays; a joiner starts at J, so no history is sent. |
| 2 | What counts in "1024 events"? | Terrain operations only. | Entity events never touch chunks; counting them would shorten the ring by the snapshot-driven event rate. |
| 3 | What counts in "1 MiB"? | Memory of the kept op (40 B + raw mask bits). | It bounds what the client holds; wire bytes are already gone. |
| 4 | How does a joiner know which entering chunks are modified? | `WindowAck` carries a run-length bitmap over the window box (the design's ack had only `asOfOpSeq`). | Without it a client would take a modified chunk as pristine until its data came: wrong voxels, a wrong piece, a wrong hash. Runs cost a few bytes per modified chunk. |
| 5 | Why is the ack on the events channel? | As designed, and because it then arrives with `applied <= A`. | Pristine chunks are based without a replay; bulk would delay the ack behind chunk data. |
| 6 | How are updates and acks paired? | `windowSeq` in both, plus a `reset` flag. | Two moves the client did not see apart would otherwise hide a chunk that was regenerated. |
| 7 | Where is the per-chunk base? | A table in `vortex.net`, indexed like the world window. | No network notion in `vortex.voxel`; the window's indexing is already a published contract. |
| 8 | How is an op applied to some chunks only? | A chunk clip on the World's edit calls, ceiling decided on the whole op. | Exactness follows from per-voxel decisions and the fixed visit order; a predicate callback would allow other clip shapes nobody needs. |
| 9 | What happens to a client chunk that leaves the window? | Dropped (`keepUnloadedEdits = false`): it comes back pristine and, if modified, as `ChunkData`. | One source of truth, bounded client memory over an open-ended walk; costs re-sent chunks when walking back and forth (a "have" list is a later optimisation). |
| 10 | Load reports offline? | Opt-in (`reportLoads`). | An offline world would grow the list forever. |
| 11 | Bulk order | Request answers, chunks within 32 m, the baseline, the rest nearest first. | The start gate needs the near chunks; the baseline is small and copies must exist before their events stop being held. |
| 12 | Pacing | Delay-based window from the measured drain and the RTT (target 100 ms of queue), events always first in the server, an optional hard cap. | The design's "pace by what is in flight" made concrete with what every transport gives (`ReliableBytesInFlight`, `RoundTripSeconds`); no wire change; works over ENet and the loopback alike. |
| 13 | Baseline form and tombstones | `BaselineEntity` wrapping an `EntitySpawn` with asOf, tick and rest tick; `BaselineEnd`; no tombstone list on the wire. | The rest rule needs the rest tick; the end marker bounds held events; dead NetIds never reach the joiner (see the server section). |
| 14 | When does the player start? | 32 m current **and** `BaselineEnd`. | Resting copies block players (`Layer::Dynamic`); a missing crate would be a correction at the first step. |
| 15 | Client movement over unbased chunks | Held, counted, corrected by the server. | The shared gate (`HasCollider`) stays the one rule on both sides; a pristine stand-in of a modified chunk must not be walked on. |
| 16 | Full store | Unsynced, no `Fail`; requested when more than 64 slots are free, as many as are free. | The design's rule; the reserve stops a request / StoreFull loop. |
| 17 | Model requests | Answered with a `BaselineEntity` at the front of the bulk queue, events of that NetId held meanwhile. | One path for the baseline and the answer; the same held-event rules. |
| 18 | Moving window at the region's edge | Not clamped; only when the region does not fit. | Chunks beyond the server's window are pristine for good; a fitting region keeps exactly today's behaviour. |
| 19 | Hash of a window-sized client | Over the sync box, incomplete while any chunk is not current, at the same opSeq after a drain that waits for `Synced()`. | Comparable on both sides with one box rule; the fixed window is the special case. |
| 20 | Modified map shape | Sparse hash map. | Stage 9's 1 km world; a dense array would be millions of cells. |
| 21 | Probe check with a moving window | Only probes inside the loaded window are checked against the world. | The others are not loaded; the generator-side check of every probe is unchanged. |

#### Risks and what 7a leaves

- **Join bytes may exceed the old log** on small sessions: a chunk's RLE (est. 0.3-8 KB) is larger than the ops that
  made it. Measured in M-a; a later encoding against the pristine chunk (both sides can generate it) would cut it.
- **Large `ChunkData`** (up to 98 KB for a noisy chunk) overshoots the bulk window by one message: on a 32 kB/s link
  that is seconds of head-of-line blocking for the events behind it. 7a counts messages over 16 KiB; splitting is not
  built.
- **`SetChunkEncoded` and the column counters**: a wrong `surfaceLeft` hangs `LoadBlocking`; the World test runs it.
- **Load reports at a join**: one vector of 101 376 (fixed window) or about 100 000 coordinates once, then about 2000
  per window step: cheap, but every reported chunk costs a ring scan (bounded by the ring).
- **The pacer's constants** (100 ms, 4-32 KiB, the drain average) are estimates; the p99 test sets them. ENet's
  in-flight count includes acknowledgement latency, which the RTT term covers.
- **Visual stand-ins**: a modified chunk shows its pristine content until its data comes (no base: not walkable, not
  hashed). Copies may rest on such chunks for that time.
- **Model requests** after a far collapse (M-b) all go to the front of the bulk queue and delay chunk data; bounded by
  the 64-per-message limit and the piece caps.
- **Start time on a heavily edited arena** at 32 kB/s can be many seconds (est. hundreds of KB within 32 m); the HUD
  says so.
- **Reproducibility**: bulk order and encoding depend only on server state; client installs depend on the seeded
  transport. `Entities.OneScriptGivesTheSameBodiesTwice` must stay green; a test that compares two runs must wait for
  `Synced()` first.
- Not in 7a (7b): `HashCheck` every 2 s (it will use `current` chunks, the sync box and the opSeq rule here), server
  terrain mode (the reserved `WindowUpdate` flag bit and `ChunkRequest`s of the whole window), the wire `EntityCheck` and
  the list re-send (a second baseline through the same queue), the test table on a 250 m region. Stage 8's `vortex_bot`
  is a `Client` + `Simulation` and needs nothing new; stage 9 encodes at dequeue from chunks it may have to load
  (`EnsureLoaded`) and keeps the sparse modified map.

#### Amendments after the critic (2026-11-16)

A critic read this plan against the code at `40e70b7` (12 findings). Where it was right the plan is changed as below;
**where this table and the sections above disagree, this table wins.**

| # | Finding | Verdict | Change to the plan |
|---|---|---|---|
| 1 | The pacer used `RoundTripSeconds`, which grows with the bulk queue itself (ENet's lowest / last values are minima of the smoothed RTT of the last 5-10 s): `W = C x (RTT + d)` grows by `d x C` per update up to the 32 KiB clamp. `ImpairedTransport::RoundTripSeconds` is a constant, so no planned test could see it. | Right. | `bulkWindow = clamp(drainRate x (baseRtt + bulkTargetDelay), bulkWindowFloor, 32 KiB)` with **`baseRtt` = the lowest `RoundTripSeconds` seen since the peer joined**; whenever `RoundTripSeconds - baseRtt > bulkTargetDelay` the window is halved (LEDBAT-like) instead. `ImpairedTransport::RoundTripSeconds` includes its own link queue for that peer (`linkFree - now`). The pacer is its own GPU-free class (`net::BulkPacer`) with a unit test in which the RTT rises with the queued bytes: the window must not grow (the planned formula fails it). |
| 2 | p99 < 15 ticks is not reachable on that link with a 100 ms target queue and a 4 KiB floor (a lost reliable packet alone costs 10-12 ticks); and "events hold on `maxEventBytesInFlight + bulkWindow`" holds live events behind an oversize `ChunkData`. | Right. | **Live events are never held by bytes in flight** (with joins at J there is no join log left to pace); `maxPeerBacklogBytes` (8 MiB) bounds memory instead: a peer whose reliable bytes in flight exceed it is disconnected (`peersTooSlow`). `bulkTargetDelay` = 2 ticks (33 ms), floor 1200 B (one datagram). The test measures p99 on the same seed **without** the join first and asserts the join adds at most 3 ticks; the design's absolute 250 ms is printed, asserted only where the baseline allows it. Splitting `ChunkData` is **not** built in 7a (counted: `bulkOversize`, messages over 16 KiB; one message may overshoot the window). |
| 3 | "B's downlink 64 kB/s" cannot be expressed: one `m_LinkFree` is shared by every peer of an `ImpairedTransport`, and the server has one link for all bots. With the 32 kB/s cap on, the paced / unpaced comparison tests the cap, not the window. | Right. | `Impairment::peerBytesPerSecond` (a link queue per peer); the paced / unpaced comparison runs with the hard cap off; the cap (`maxBulkBytesPerSecond`) is checked in its own assertion (average bulk rate <= cap). |
| 4 | Every `WorldOp` is idempotent per voxel (`f(f(x)) = f(x)`), so the `>=` / `>` test and the carve + swap test could not fail on the bugs they name. | Right. | The pair is `SwapSphere` dirt -> stone at 1, then `SwapSphere` air -> dirt at 2 on the same sphere (re-applying 1..2 on data at 2 turns the former air into stone). `ChunkSync` keeps per-chunk counters (`applied`, `skippedAtOrBelowBase`), asserted; a replicated `Detach` at `seq == base` must send a `ModelRequest` and build no copy. |
| 5 | The ring's 1 MiB counted raw mask bits (a `ClearMask` box can be 37-864 KiB raw, 2-4 KB on the wire): it would hold seconds of a fight, not 1024 ops. A chunk beyond the server's window (M-b's unclamped window) could never be served and would stay Requested, so `Synced()` never holds. | Right. | The ring keeps each op **as its wire bytes** (masks as runs; `netId` 0, no velocity), counted by those bytes, decoded only when replayed; the 1024-op bound is the one that binds in a fight. Chunks outside the server window (from `RegionDesc`: play bounds + `kServerWindowMarginChunks`) are **never edited**: current as soon as they load, never replayed, never requested (7b's terrain mode serves only the server window; beyond it a client keeps generating, the server never edits there). The long range-only log is not built (load reports in a fixed window never come late; M-b measures it). |
| 6 | The arena walk's store gate measured nothing: bots have no packing and a region-sized store. | Right (M-b). | M-b: `ClientWorldConfig` takes `packHidden` from the session, the bots get `storeLimit` 12 288 and `minFreeStoreSlots` 2048 like the game; `storeFullLoads` / `storeFullDecodes` gated at 0; the `ClientWindow.*` tests use the same config. |
| 7 | Holding client movement over chunks that are not current turns every snapshot into a correction. | Right. | While the own `PlayerReach` box holds a chunk that is not current, a step does not move the player (`syncHeldTicks`), inputs are still sent, and every compared snapshot's owner state is **taken as it is** (no correction counted, no replay; `syncTaken`); when the box is current again, the inputs after the last taken snapshot are replayed once (`syncResumes`) and prediction goes on. |
| 8 | "32 m" was undefined; the gate waited for every body; a gate closed longer than `startWaitTicks` let a stale snapshot start the player. | Right in part. | 32 m is 3D: a chunk box within 32 m of the snapshot's player position (loaded rows only). The gate is evaluated **before** the fresh-snapshot rule; while it is closed every waiting snapshot is dropped, so the age bound restarts when it opens (a `Client.AJoinerStartsPredictingFromAFreshSnapshot` case holds the gate for more than 120 ticks). The baseline is queued nearest the player first. **Kept:** the gate waits for the whole `BaselineEnd` (bounded by the piece caps: est. under 100 KB; measured by the join tests) - a near-set end marker is M-b's if the arena's start time needs it. |
| 9 | Counters, exit rules and fixtures kept "unchanged" no longer mean what they assert. | Right. | `ClientStats::skippedChunks` keeps its meaning (the World skipped a chunk the client holds as current: an error); new `syncSkips` (ops skipped for chunks without a current base: normal), `unsyncedChunks` = chunks that entered Unsynced (store full: still asserted 0 where the store is large). `vortex_server` exits 1 unless every bot is `Synced()`, its sync-box hash equals the server's and its entities agree. The FakeServer sends a `WindowAck` and a `BaselineEnd` in its join. `Synced()` = every chunk of the sync box current, no `ChunkRequest` or `ModelRequest` out, `BaselineEnd` received, no held entity events. |
| 10 | Feeding the modified map with the chunk range of `Bounds()` over-sends (sphere corners, air, hardness), pins client slots, and makes needless model requests. | Right. | `EditReport::changed` (new): the chunks an edit changed at least one voxel of, in visit order. The server's modified map is fed from it (a chunk the op did not change still equals the generator's output). "Complete here" is decided over the chunks the op **touches** (`sim::OpTouchesChunk`, exact: the sphere / box / mask tests `EditRegion` uses; `voxel::SphereTouchesBox` exported). |
| 11 | `SetChunkEncoded` must mark the store slot dirty or the GPU keeps the old chunk. | Right. | `MarkDirty(slot)` on every Dense install; the World test asserts the slot is in `TakeDirty`. |
| 12 | M-a sent its `WindowUpdate` after `Attach`: every chunk touched during the 1-3 s load would be sent as `ChunkData`, every replicated `Detach` would become a `ModelRequest`. | Right. | The fixed window is known from `RegionDesc`: the client sends `WindowUpdate` 1 **on `Welcome`**; the ack is kept in the event stream (`pending` holds batches and acks in arrival order) and processed in stream order after `Attach` has loaded the window, so only chunks changed in `(J, A]` need data. |

None was rejected outright; two suggestions are not built in 7a (oversize `ChunkData` split, a near-set baseline end) and
one was judged unnecessary (the long range-only op log). Test changes that follow: `ChunkSync.AnEventIsAppliedOnlyAboveTheBase`
asserts the per-chunk counters; `Client.BulkAheadOfEvents...` uses the two swaps; the late-join p99 test is relative
to its own no-join baseline and uses `peerBytesPerSecond`; new `BulkPacer.TheWindowDoesNotGrowWithItsOwnQueue`.

### Stage 7a as built

Status (2026-11-17): **milestones M-a and M-b built, integrated, accepted, reviewed and rechecked ("Review of stage 7a"
and "Recheck of stage 7a" right after this section: their fixes and numbers are the current ones); not committed** (the runs'
instructions; M-b was finished and every check re-run on 2026-11-17, see "M-b as built"; the integration run's fix,
measurements and acceptance are "Integration and acceptance" at the end of this section, whose numbers are the
current ones). Built on engine `40e70b7` /
game `092cf91`. M-a (join by chunk sync, the entity baseline, the window
unchanged) is the first part of this section; M-b (a client window smaller than the region that follows its player)
is "M-b as built" below. A successor has the review of 7a to run, then 7b. The plan and its amendments above hold;
where the code differs from them, this section says so.

| Part | Built | Where / tested by |
|---|---|---|
| World | `ChunkClip` on the five integer edits (ceiling decided on the whole op, then only the clipped chunks; `CarveSphereQInChunk` is now a one-chunk clip), `EditReport::changed`, exported `SphereTouchesBox`, `EncodeChunkAt`, `SetChunkEncoded` (any cell state; `SettleLoading` takes a Loading / Unloaded cell out of its column's row bits and `surfaceLeft` and bumps its generation; `MarkDirty`; `modified = true`), `TakeLoadedChunks` with `WorldConfig::reportLoads` (job results and the column pass's `PlaceCell`), `WindowRevision`, `WorldConfig::keepUnloadedEdits` | `World.*` 5 (`tests/core/ChunkSyncWorldTests.cpp`) |
| sim | `ApplyOp(..., clip)`, `OpChunkRange`, `OpTouchesChunk` (exact, the tests `EditRegion` uses) | `WorldOps.ApplyOpClippedToEachChunkEqualsTheWholeOp` |
| Protocol 8 | the seven messages of the plan's table at its byte sizes; `Welcome::joinOpSeq`; `RefuseReason` 6 retired; `IsValidChunkStream` | `Wire.ChunkSyncMessagesRoundTripAtTheirSizes`, `Wire.ForgedBytesNeverCrashADecoder` (extended) |
| `vortex.net:sync` | `SyncBox`, `Intersect`, `RegionSyncBox`, `ChunkCoordHash`, `OpRing` (wire bytes, 1024 ops / 1 MiB), `ChunkSync` (the table and the rule), `BulkPacer` (amendment 1) | `ChunkSync.*` 4, `BulkPacer.*` 1 (`tests/net/SyncTests.cpp`) |
| Server | modified map fed from `EditReport::changed` (`EmitOp`); backlog instead of the log (trimmed to the slowest joined peer); live events never held (`maxEventBytesInFlight` default 0, kept for one test); `maxPeerBacklogBytes` (8 MiB) disconnects a peer that stops reading; `WindowUpdate` (seq, rows, flags checked; a broken one disconnects) answered at once after the peer's pending events; per-peer bulk queue (request answers, chunks within 32 m, the baseline nearest the spawn first, then chunks nearest first), encoded at dequeue; `BulkPacer` per peer, optional `maxBulkBytesPerSecond`; `ChunkRequest` / `ModelRequest` answers to the front; baseline with `BodyTrack` rest tick and pose and the template's lights; `Join:` log line; `ServerStats` sync counters; `Hash(SyncBox)`, `ModifiedChunks`, `BacklogBytes` | `Session.ATemplateReachesAJoinerAsChunkDataAndBaseline`, `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing`, `Session.ASlowPeerIsDisconnectedWhenItsBacklogOutgrowsTheLimit` |
| Client | `WindowUpdate` 1 on `Welcome` (amendment 12; the ack waits in `pending` with the batches and is taken in stream order after `Attach`); `ChunkSync` applies every op (fast path or clipped); `Fail` on an incomplete op removed; bulk kept in `pendingBulk` (accepted from `Joining` on: the bulk channel is not ordered against the `Welcome`); held entity events, `copyBase`, tombstones, `ModelRequest` for a replicated `Detach` that is not complete here; start gate before the fresh rule (amendment 8, 3D 32 m); movement hold (amendment 7); `Hash()` over the sync box with non-current chunks as `notLoaded`; `Synced()`, `JoinOpSeq()`, `SyncWindow()`, `Sync()`; `reportLoads` set by `MakeSimulationConfig` and required by `Attach` | `Client.*`: 5 new, 1 rewritten (`ALateJoinerPresentsOnlyEventsAfterItsJoinPoint`) |
| Transport | `Impairment::peerBytesPerSecond`, `rngPerPeer` (opt-in: another peer's traffic does not change a peer's losses); `ImpairedTransport::RoundTripSeconds` includes its link queue | used by the pacing and late-join tests |
| `vortex_server`, game | drains wait for `Synced()`; `Join:` lines (server side per joiner, bot side, the game's at its drain); the exit rule compares each bot's hash over its sync box and needs `Synced()`; the status line shows the modified map, backlog and bulk | the drill command, `tools/session-check.sh` |

Where the code chose something the plan left open or got wrong:

| Question | Choice | Why |
|---|---|---|
| A bulk message larger than what is left of the window. | It is encoded at dequeue, then **held in the server** (`Peer::heldBulk`) until it fits the window or nothing is in flight (then it goes alone). **Changed** (review of 7a, 3; recheck 1): it waits for the bulk in flight to drain and goes alone while the bytes in flight are under the window. | The first run handed it over at once: A's live latency in the pacing test was 24 ticks against 9 without the join; with the hold 16 (one 10.5 KB message alone = 13 ticks of that 48 kB/s link). Its asOf stays the encoding's: a held chunk is a few ticks older, which the ring covers. |
| What the start gate waits for. | `BaselineEnd` and every chunk of the window within 32 m (3D) current; while it is closed the waiting snapshots are dropped (`startGateDrops`). | Amendment 8. `Client.AJoinerHeldByTheStartGateStartsFromAFreshSnapshot` holds the gate for 300 ticks and starts from the newest snapshot. **Wrong** (review of 7a, 5): the gate opens on `BaselineNear` (the entities within 32 m of the spawn), not `BaselineEnd`. |
| The baseline's order. | NetIds by distance from the spawn at the join (the joiner's player is there). | The near-set end marker is not built (amendment 8). |
| A model answer for a NetId that is gone. | Nothing is sent; its `EntityDespawn` is in the stream and tombstones it. | As planned. |
| Held entity events. | 64 per NetId, 4096 in all; overflow drops them and asks for the model again. `BaselineEnd` drops held events of NetIds nobody asked for. | As planned; never reached in the tests (the collapse joiner held none: its baseline came within a few ticks). |
| `ClientStats::skippedChunks` / `unsyncedChunks`. | `skippedChunks` counts chunks the World skipped although chunk sync held them current (0 in a fixed window); `unsyncedChunks` chunks an op found without a slot (they become Unsynced). Ops skipped for chunks without a base are `ChunkSync::Stats().chunkSkips`. | Amendment 9. `vortex_server` no longer fails on `unsyncedChunks` (it fails on not `Synced()`). |
| Tests that compare two runs' latencies. | The late-join test runs the server and clients with `waitIdle` (collider cooks inside the tick) and `rngPerPeer` (A's packets meet the same losses with and without B). | Without them the same seed gave A's p99 12 in one run and 20 in the other: Jolt's timing and B's traffic changed A's loss draws. |

Verified (this laptop, dev build -O2 -g): **warning-free; 652 tests (six disabled), all green** (634 before: 18 new; one
replaced - `Session.AJoinIsRefusedOnceTheEventLogIsTooLarge` by the slow-peer test; three rewritten). Both
headless game runs exit 0 under validation (`Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, feet
(17.15, 4.36, -2.99) on the ground; offline is untouched by chunk sync).

| Run | Result |
|---|---|
| The CLAUDE.md drill (loopback, 5 % loss) | exit 0, **hashes equal**, 67/67, 67/67, 66/66, both bots' entities agree (8 objects, rest poses exact); corrections 4 and 1. The late joiner (opSeq 1579) **joined by chunk sync: 64.1 KiB of bulk** (103 `ChunkData` 31.8 KiB, 11 baseline entities 22 KiB; the 1579-event log it replaced was about 236 KiB at the drill's 151 B per event), started predicting 0.58 s and was synced 0.55 s after its `Welcome` (server ticks); 339 ops skipped for its chunks without a base, 107 replayed, 4 model requests. |
| The UDP drill (`--enet --clients 3 --join-step 50 --bot-casts 90 --bot-crates 120`) | exit 0, hashes equal, entities agree (9 objects); joiners 36.7-48.5 KiB of bulk, started 0.6-1.0 s, synced 0.45-1.4 s; corrections 1 / 17 / 6 (one-thread bots, stage 5's note). |
| `tools/session-check.sh` | **PASS** (both game clients' hashes and entity lists equal the server's at the same opSeq). Join on the playground: **76.4 KiB of bulk** (50 `ChunkData` = 75 KiB of plaza chunks, 12 baseline entities 1.4 KiB) where the old join sent a 64-event log: the plan's first risk (a chunk's RLE is larger than the ops that made it) measured; encoding against the pristine chunk is the later fix. The screenshots show the plaza, the crates (baseline copies) and a crater. |
| `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing` (shared 48 kB/s uplink) | A's worst live latency in the 300 ticks after B connected: 9 ticks without a join, **16 paced**, 46 unpaced; window 1.2-3.9 KiB, held 178 times; B synced after 161 ticks (54 unpaced). |
| `ChunkSync.LateJoinDuring10CastsPerSecondConverges` (5 % loss, 40 + 0-30 ms, 64 kB/s per peer, bulk capped at 32 kB/s) | converges (equal hash, same NetIds, models and rest poses); **A's p99 21 ticks with and without the join**; the joiner's p99 19; B synced after 3.4 s with 57-59 KiB of bulk (17 KiB/s). **The design's 250 ms (15 ticks) is not met on this link even without a join**: the 64 kB/s downlink also carries the snapshots (up to 36 kB/s with bodies) and a lost reliable packet costs 130 ms plus head-of-line; asserted relative (A: +3 ticks; B: +3 + one bulk message). |
| `BulkPacer.TheWindowDoesNotGrowWithItsOwnQueue` (64 kB/s, 100 ms base RTT) | window at most 9.2 KB, link queue at most 61 ms, 62 KiB/s delivered; the first plan's formula: 32 KiB and 415 ms (the critic's feedback, reproduced). |

#### M-b as built

Checked first: the M-a diff was read against the plan and its amendments before M-b was built on it (no review of 7a
has run yet). It held; two small faults were fixed on the way: a `WindowUpdate` with `reset` kept the owed chunk
requests of the old records (`ChunkSync::BeginWindow` now drops them), and an owed request was sent for a chunk whose
record had left and come back as a new one (`TakeRequests` now sends only records still `Requested`).

| Part | Built | Where / tested by |
|---|---|---|
| sim | `ClientWindowMoves(region, windowChunks)`, `ClientWorldConfig` (the region's window when it fits, else `windowChunks` x the rows x `windowChunks`; `reportLoads` on, `keepUnloadedEdits` off for every session client), `ClientWindowMinChunk` (World's own arithmetic: `WorldToVoxel`, `WindowMinFor`), `ClientWindowSize`, `ClientChunkCapacity` | `ClientWindow.ARegionThatFitsKeepsTheFixedWindow` (a `World` placed at three foci lands where `ClientWindowMinChunk` says) |
| Client | `ClientSettings::windowChunks` (0 = the region's window; at most 128; a region whose window is wider than 128 without it fails at the `Welcome`); the first `WindowUpdate` at the `Welcome` names the window centred on the spawn; `CheckWindow` at the start of every `Poll` (before anything is applied: the world moves only in streaming) sends update k + 1 when `World::WindowRevision` changed, with `reset` when it changed by more than one; `RunTick` streams around the player (the spawn before it predicts); `Attach` places a moving window at the spawn and loads `LoadScope::Surface` (the rest streams), checks the window's corner and size, and the probe chunks it has loaded (a fixed window: all of them); `Window()`, `WindowMoves()`, `ChunksToStart()`; stats `windowUpdates` / `windowResets`, `maxStoreUsed`, wall-clock join times; a full store is logged as it grows | `ClientWindow.*` (9) |
| Server | nothing new for moves (M-a's entering sets and dequeue drops already diff against the last window); `ClientSyncBox(slot)` for the "client left" line; `SetMaxBulkBytesPerSecond` (an admin knob; the tests hold data back with it); the entering pass walks the modified map instead of the window box when the map is smaller; the bulk credit is charged only under a cap | `ClientWindow.QueuedChunksThatLeaveTheWindowAreDropped`, `ThePlayerIsHeldOverChunksWithoutABase` |
| sync | `ChunkSync::Counts()` (per state), `FromData(chunk)`, an early-out `Synced()`; the ring's replay starts at the first op after the base (binary search: a chunk that loads just after its ack scans a few ops, not 1024) | the above |
| `vortex_server` | `--client-window N` (the bots' window; with it the game's store, 12 288 slots, 2048 kept free, and the session's packing; the run fails if a bot's store fills, an op finds no slot or it keeps fewer than 2048 free), `--walk-across` (bot 0's `BotPilot` runs waypoints: the play bounds' -x edge to the +x edge along the middle z, back to the middle; a waypoint without a metre of progress in 300 ticks is given up and counted), `--precasts N` (N server casts along that line, one every 6 ticks, before any bot joins), `--memory` (RSS, server world and bot stores every 10 s of ticks and at the end); per bot a window / store line and, when not synced, its chunk states; the "client left" line adds `window hash H over [min..max)`; the drain of a moving window goes on while a bot's world still loads (at most 2 min wall) | the arena walk below |
| Game | `windowChunks = 48`; `windowFollowsCamera = sim::ClientWindowMoves(region, 48)` (playground and drill keep the fixed window); the drain streams while it waits (30 s for a moving window); the HUD's bottom panel says `JOINING <server>  slot N   K chunks` until the start gate opens; the hash line names its box | `tools/session-check.sh`; a headless client on `arena250.session` (below) |

Choices M-b made where the plan was open:

| Question | Choice | Why |
|---|---|---|
| Where the client notices a move | At the start of `Poll`, from `World::WindowRevision`, never inside the apply path. | The world moves only in streaming (`RunTick`'s, or the engine's per frame), which is outside `Poll`; the game polls at least once a frame, so two moves between checks (a reset) need two `Update`s without a `Poll` in between (it happens in `ClientWindow.TwoWindowMovesUnseenResetTheWindow` on purpose, not in the game). **Wrong** (review of 7a, 10): the game streams every frame and polls on ticks; every step is now listed (`World::TakeWindowSteps`) and sent, no reset. |
| The window of the first update | Computed at the `Welcome` from the spawn (`ClientWindowMinChunk`); `Attach` sends a plain update if the world landed elsewhere. | Amendment 12 for the moving window: nothing has been applied before `Attach`, so no reset is needed. |
| Store gate | `maxStoreUsed` (sampled every `Poll`) plus 2048 at most the capacity; no store-full load or decode; no unsynced chunk. | Amendment 6. |
| Join times | Server ticks (as M-a) and the wall clock from the `Welcome` (logged only). | A server catching up after its preload runs ticks faster than real time: a game client on the arena "started after 12 s" of server ticks, 2.4 s of wall clock. |
| `Synced()` cost | Early out; `SyncHousekeeping` looks at it every 16th `Poll` until the first time. | It walks 101 376 records in the game's window. The join's sync time is therefore known to about 0.27 s. |

Verified (this laptop, dev build -O2 -g): **warning-free; 661 tests (six disabled), all green** (652 before M-b: 9
new `ClientWindow.*`, two of them added when M-b was finished, see below). Each new test was run three times with
equal output, and checked against two deliberate faults: with the server's acks of moved windows marking nothing
modified, the walk-in, leave-and-return and held-player tests fail on the world hash; with the reset rule off, the
two-moves test fails on the hash.

**Finished by the next run (2026-11-17).** The run that built M-b was cut off before it reported; the working trees held
M-a and all of M-b above, built. The next run read the whole 7a diff against the plan, the amendments and this
section, re-ran every check (numbers below are that run's) and closed two gaps of the "Not done" list: a full store
with a moving window and a replicated piece that breaks off outside the window had no test.

| Test (added 2026-11-17) | Asserts |
|---|---|
| `ClientWindow.AFullStoreWhileWalkingMarksChunksUnsyncedAndRequestsEachOnceWhenSlotsFree` | 18 gold boxes in the air (one per chunk, Empty when pristine) on the walker's way; the test takes the client's store but for 4 slots after the join; the walker walks in: 14 chunks Unsynced, 4 installed, **no request while the store is full**, `Synced()` false and the hash incomplete; slots freed: **14 requests, each chunk once** (server and client counts equal), all 18 installed, equal hash over the sync box. Checked against a fault: with the reserve rule off (requests while full) it fails. |
| `ClientWindow.APieceThatBreaksOffOutsideTheWindowIsAskedFor` | a drill mushroom 37.5 m east of a client standing at the middle (every chunk of it outside the 48 m window): the cap is a server body (`detachBodies` > 0); its `Detach` touches no chunk the client holds, so the client sends one `ModelRequest`, gets one `BaselineEntity` answer and copies the server's body (entities agree, models and rest poses); no `ChunkData` (nothing of the site entered), equal hash |

| Run | Result |
|---|---|
| `ClientWindow.APlayerWalksInFromOutsideIntoChunksModifiedBefore` (flat region 40 chunks wide, window 12, 30 craters on the far side before the join, more on the way; 5 % loss, 40 + 0-30 ms) | 46 window updates, no reset; 57 `ChunkData` (147 KiB), 0 chunk requests; 0 ticks held at 7 m/s; store at most 144 slots; equal hash over the sync box at the same opSeq, entities agree |
| `ClientWindow.AChunkThatLeavesAndComesBackAfterEditsIsSentAgain` | the chunk comes back as `ChunkData` as of its second edit or later; a pristine neighbour comes back with no data; equal hash |
| `ClientWindow.QueuedChunksThatLeaveTheWindowAreDropped` (bulk capped at 16 KiB/s, an 80 m jump of the window) | 96 queued chunks dropped at dequeue, 1 late one stale on the client; equal hash |
| `ClientWindow.TwoWindowMovesUnseenResetTheWindow` | one reset; the server marks the window's modified chunks again; the chunk that left and came back unseen is data again; equal hash |
| `ClientWindow.HashesCompareOverTheSameBoxAtTheSameOpSeq` | incomplete while data is held back; equal after the drain; another box or one changed voxel gives another hash |
| `ClientWindow.ThePlayerIsHeldOverChunksWithoutABase` (data held back while the player runs onto it) | 44 ticks held, 21 snapshots taken as they were, 8 resumes, 0 corrections, 0 snaps; equal hash |
| **Arena walk** (`vortex_server --session=engine/tools/server/sessions/arena250.session --clients 2 --client-window 48 --walk-across --precasts 40 --join-step 20 --drill 40 --loss 0.05 --delay-ms 40 --jitter-ms 30 --memory`) | **exit 0, both bots' hashes equal the server's over their sync boxes** (48 x 44 x 48 and 42 x 44 x 48), entities agree (74 objects), drill 40/40. Walker: x -118 to +122 m, 119 window updates, 0 resets, 0 ticks held, 0 corrections; join 219 `ChunkData` / 737 KiB + 38 baseline entities / 128 KiB, queue empty after 3.6 s (server ticks; peak 306 KiB/s on the unlimited loopback); 539 `ChunkData` (1.9 MiB) received over the whole walk. Second bot (wanders near the spawn, joins at step 20): 2270 `ChunkData` / 4.4 MiB - the drill's sites lie at its window's edge and enter and leave as it steps back and forth (choice 9's cost, measured). Store at most **5563 / 5447** of 12 288 slots (gate 10 240); RSS **1101 MiB** for the server and both bot worlds in one process; server world 10 518 dense / 31 084 packed, **48.4 MiB RLE** (gate 150). Wall 37 s; the bots' far cave chunks need most of the drain (the walker was synced 26 s of wall clock after its `Welcome`, after the walk). One earlier run with a 3600-tick drain failed on that (9308 chunks still generating): the drain now waits for the loading. |
| A headless game client on `arena250.session` (`vortex_server ... --listen`, `Shredborne --connect ... --headless --validation --frames 900`) | exit 0 both; window 48 x 44 x 48 follows the player, surface loaded in 1.37 s, whole window filled 5.9 s later, started predicting 2.4 s (wall) after its `Welcome`; its hash equals the server's `window hash` over the same box at the same opSeq; screenshot looked at. Re-run 2026-11-17: exit 0 both; surface loaded in 1.34 s, started 2.22 s and synced 7.46 s (wall) after its `Welcome`, 4 window updates, store at most 5235 slots, 3 casts; client hash `353e44ba92f2c56b` = the server's `window hash` over [-24,-12,-25..24,32,23) at opSeq 8 |
| The CLAUDE.md drill (re-run 2026-11-17) | exit 0, **hashes equal**, 67/67, 67/67, 66/66, both bots' entities agree (8 objects, rest poses exact); fixed 13 x 8 x 13 windows (one `WindowUpdate` each); the late joiner (opSeq 1579): 64.1 KiB of bulk (103 `ChunkData`, 11 baseline entities), started 0.47 s and synced 0.54 s (wall) after its `Welcome`; corrections 4 and 1 |
| `tools/session-check.sh` (re-run 2026-11-17) | **PASS**: both game clients' world and entity list hashes equal the server's at the same opSeq (playground fits 48 chunks: fixed window, hash box = the region's window) |
| Both headless game runs (re-run 2026-11-17) | exit 0 under validation: `Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, feet (17.15, 4.36, -2.99) on the ground; the editor run exits 0 |
| Arena walk, re-run 2026-11-17 (same command) | exit 0, both bots' sync-box hashes equal the server's, entities agree (74 objects), drill 40/40; walker x -118.1 to +122.0 m (2 of 3 waypoints reached: the last, back to the middle, was given up - no metre of progress in 300 ticks; the walk across was done), 119 window updates, 0 resets, 0 ticks held, 0 corrections; it received 539 `ChunkData` / 1.9 MiB, started 6.0 s (wall) after its `Welcome`, synced 24.1 s (after the walk: its far cave chunks); second bot 2290 `ChunkData` / 4.5 MiB, 82 stale; store at most **5570 / 5499** of 12 288 (gate 10 240), store full 0 / 0; RSS 1107 MiB (server and both bots), server world 48.4 MiB RLE; server totals 2829 `ChunkData` / 6.1 MiB (48 dropped at dequeue), 59 model requests, bulk window 1.2-32 KiB, none over 16 KiB; 34 s wall |

**Not done (7b, or open):** `HashCheck`, server terrain mode, the wire `EntityCheck` (7b); splitting large
`ChunkData` / `BaselineEntity` messages (one alone costs about 10 ticks of a 64 kB/s link); the join-bytes risk on the
playground (above: 76 KiB against the old 64-event log); re-sent chunks when a player steps back and forth at a
modified area's edge (a "have" list or hysteresis in the window's follow would cut them; 4.4 MiB for the wandering
bot above); no chunk was replayed from the ring in the arena walk or the walk-in test (entering chunks load within a
few ticks of their ack; the replay path is covered by `ChunkSync.AChunkStillLoadingGetsItsEventsWhenItLoads` and the
drill's late joiner, 107 replays); the game's `JOINING` line was not seen on a
screenshot (headless runs end predicting; seen since, see below) and windowed play on the arena is the owner's to try.
Review of this stage is pending.

#### Integration and acceptance (2026-11-17)

A further run read the whole 7a diff (World, sim, `vortex.net:sync`, wire, server, client, `vortex_server`, the game's
`Session.cpp`) against the plan, its amendments and the two "as built" parts above, fixed one defect, added the
measurements the acceptance asks for, and re-ran everything. The base rule held on every path the plan's table lists
(live, replayed, ahead, pristine-late, leave-and-return, reset, store full, ring not covering); the server's entering
sets, dequeue drops and request answers, the held entity events and the start / movement gates were read against it
and found as described.

| Found | Fix |
|---|---|
| `World::SetChunkEncoded` over a loaded chunk queued its change with an empty edit box, which physics reads as "only loaded": a tracked chunk's old collider was not stale for `BodyReady`, so a client's own rubble resting there was simulated against the old voxels until the new collider was cooked (shred.md 1d's rule: no body is simulated against voxels its chunk's collider still shows). The predicted player was not affected (`FlushBox` before every step). | The install reports the whole chunk as its edit box when the cell was loaded (an install into an Unloaded / Loading cell stays a load). `World.SetChunkEncodedInstallsAnyChunkAndIsAContentChange` asserts it (it failed on the old code). |
| The acceptance asks for event latency during joins; nothing outside the unit tests measured it. | `vortex_server` keeps the server tick each event went out with and measures, per bot, the ticks until the bot applied it: a `Latency: bot N:` line with p99 and max for the events made while some bot was joining (connected until its join data was in) and for all events. |
| A walker's "synced after" is the first moment its whole window is current, which for a window that keeps moving is only once it stops (the arena walker: 25 s, after the walk), so it said nothing about the join. | `ClientStats::joinDataTicks` / `joinDataSeconds`: the join's data is in (baseline ended, no chunk of the window without a base, awaiting data, requested or unsynced; chunks may still be generating locally). Logged on the bots' and the game's `Join:` lines; the latency's join window ends there. |
| The `hashes equal` summary printed each moving-window bot's hash beside the server's region hash (different boxes). | Each client's hash is followed by the server's hash over that client's sync box. |

**How a window-sized client's hash is compared** (the plan's rule, as built): the client's sync box is its window (its
last `WindowUpdate`) intersected with the server's window (`Client::SyncWindow`, server side `Server::ClientSyncBox`).
`Client::Hash()` is `WorldHash` over it, with every chunk of the window that is not current counted in `notLoaded`
(incomplete, not comparable). It is compared with `Server::Hash(box)` over the same box only at the same opSeq
(`AppliedOpSeq == LastOpSeq` after a drain) and only once the client is `Synced()`. A fixed-window client's box is the
region's window, so its hash is the region hash (`tools/session-check.sh` keeps comparing it with the server's world
hash); a moving one's is compared with the `window hash H over [min..max)` of the server's "client left" line, or by
`vortex_server`'s exit rule.

Verified (this laptop, dev build -O2 -g, all on the final binaries): warning-free build; **661 tests (six disabled), all
green**; the standard headless game run (`Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, feet at
(17.15, 4.36, -2.99) on the ground; screenshot looked at), the editor run, `--climb` (0 unloads inside the view radius,
window y fixed, 0 unfilled columns), `--tower-blast --after-fill --frames 700` (freed by shot 6 of 7 by the load check)
and `--neck-blast --frames 200` (0 pieces released by the duds, the tower released) all exit 0 under validation.

| Run | Result |
|---|---|
| The CLAUDE.md drill (loopback, 5 % loss, 40 + 0-30 ms) | exit 0, **hashes equal**, 67/67, 67/67, 66/66, both bots' entities agree (8 objects, rest poses exact); corrections 4 and 1. Late joiner (join point opSeq 1579): 103 `ChunkData` (31.8 KiB) + 11 baseline entities (22.0 KiB) = **64.1 KiB of bulk**, started 0.58 s, data in and synced 0.78 s after its `Welcome` (server ticks; 0.55 / 0.65 s wall), 339 ops skipped for chunks without a base, 107 replayed. **Event latency** (server ticks from made to applied): events made during the joins p99 11 ticks (183 ms) for the bot already playing, 4 ticks for the joiner; all events p99 13 / 10 ticks |
| The drill with `--clients 3 --join-step 50` | exit 0, hashes equal, 67/67, 67/67, 66/66, all three bots' entities agree; two joiners at the same step: 46.7 and 56.8 KiB of bulk, data in 0.52 and 0.77 s; latency during joins p99 11 / 11 / 3 ticks, all events p99 11 ticks for every bot; corrections 3 / 5 / 5 |
| The UDP drill (`--clients 3 --join-step 50 --enet --bot-casts 90 --bot-crates 120`, 28 s wall) | exit 0, hashes equal, entities agree (8 objects); joiners 53.2 KiB of bulk each, data in 0.75 and 1.45 s; corrections 2 / 1 / 1. Latency p99 6 ticks for the first bot (12 during the joins), 17 and 61 ticks for the two joiners: in this one-thread tool a joiner's events made while its own world loads (`Attach`, 0.5-0.7 s of wall clock) wait for the load, the "late joiner's load" note of stage 5; a separate client process waits for its load the same way, so this is join time, not stream lag |
| `tools/session-check.sh` | **PASS**; joins 76.4 KiB of bulk (50 `ChunkData` 75.0 KiB, 12 baseline entities 1.4 KiB), started 3.07 / 3.17 s after the `Welcome`: the game client's whole-window load (3.0 s, before which no bulk is processed) is the join time, not the bulk. Screenshots looked at: the plaza, a crater, crates and broken crates (baseline copies); b's HUD says `players 2` |
| **Arena walk** (`vortex_server --session=engine/tools/server/sessions/arena250.session --clients 2 --client-window 48 --walk-across --precasts 40 --join-step 20 --drill 40 --loss 0.05 --delay-ms 40 --jitter-ms 30 --memory`, 35 s wall) | exit 0, **both bots' hashes equal the server's over their sync boxes** (48 x 44 x 48 and 42 x 44 x 48), entities agree (74 objects), drill 40/40. **Join bytes**: the walker's join 219 `ChunkData` / 737 KiB + 37 baseline entities / 118 KiB (the server's queue empty after 3.5 s of ticks, peak 294 KiB/s on the loopback), 1.9 MiB of bulk over the whole walk (539 `ChunkData`); the second bot 4.6 MiB (2313 `ChunkData`, 58 stale: choice 9's re-sends). **Join time**: the walker started 3.8 s (6.3 s wall) after its `Welcome`, its data was in after 3.7 s (6.3 s wall), whole window current only after the walk (183 s of ticks, 25 s wall); the second bot started 3.4 s, data in 4.3 s. **Event latency during the joins**: p99 11 ticks (183 ms), max 11, for both bots (681 and 394 events) - on this link that is the stream's floor, not a join cost. **Store**: at most **5576 / 5446** of 12 288 slots (gate 10 240), store full 0. **Memory**: RSS 1107 MiB for the server and both bot worlds in one process; server world 10 518 dense / 31 084 packed chunks, **48.4 MiB RLE** (gate 150). Walker: x -118.1 to +122.0 m, 119 window updates, 0 resets, 0 ticks held, 0 corrections; it gave up its last waypoint (back to the middle) at x 9.5 m, 7.5 m short: a precast crater on its line, not sync |
| A headless game client on `arena250.session` (`vortex_server ... arena250.session --listen 27016 --bind 127.0.0.1 --token T --exit-when-empty --precasts 40 --drill 20 --drill-after 1 --drill-delay 2 --drill-sites 2 --memory`; `Shredborne --connect 127.0.0.1:27016 --token T --headless --validation --frames 900 --screenshot ...`) | exit 0 both; window 48 x 44 x 48 follows the player, surface loaded in 1.27 s; started 2.15 s, data in 1.58 s, synced 7.42 s (wall) after its `Welcome`; 829 KiB of bulk (228 `ChunkData`, 20 model answers), store at most 5360 slots; client hash `a814311601c2817e` over [-24,-12,-25..24,32,23) = the server's `window hash` over the same box at opSeq 378; entity list hashes equal. **The server alone: RSS 473-512 MiB** (`--memory`). Screenshot looked at (terrain at the spawn, no gaps) |
| The same with `--frames 15` (the screenshot taken while joining) | **the `JOINING 127.0.0.1:27016  slot 0   479 chunks` line seen on the HUD**, a precast crater installed from `ChunkData` in view; exit 0 both, hash equal to the server's window hash at opSeq 67 |

Not done (unchanged, see the list above): 7b (`HashCheck`, server terrain mode, the wire `EntityCheck`, the test table
on a 250 m region); splitting large bulk messages; encoding chunks against their pristine version (join bytes on the
playground); re-sends at a modified area's edge; no ring replay on a moving window yet (0 in the arena walk); the
design's absolute 250 ms p99 under a 64 kB/s, 5 %-loss link (the unit test's relative bound stands); windowed play on
the arena and the two-machine run are the owner's; ~~the review of 7a (CLAUDE.md step 4) has not run~~ (superseded:
"Review of stage 7a" and "Recheck of stage 7a" below).

### Review of stage 7a (2026-11-17)

Three reviewers read the uncommitted 7a diff (engine on `40e70b7`, game on `092cf91`): 16 findings, two of them
duplicates (11 = 3, 16 = 2), and one more problem found while verifying finding 12, one while fixing finding 3. Every
bug was first reproduced in a GPU-free test that failed on the old code (checked by putting the old code back in one
build: each new test below failed there for its own reason, and passes now); 10 new tests, one renamed and rewritten,
**671 in all (six disabled)**. Protocol 8 has never been committed, so the one new message (15, `BaselineNear`) is part
of it; `kProtocolVersion` stays 8.

| # | Finding | Verdict | Fix (test) |
|---|---|---|---|
| 1 | An awake body's `BaselineEntity` was labelled one tick late: `Flush` (which encodes bulk) runs after `StepPhysics`, which has counted the next tick, while snapshots, `EntityRest` and the rest tick label the state with the tick that made it. A joiner's first sample of every awake copy was one tick of motion off (17 cm at 10 m/s), and the same-flush snapshot sample was refused as older. | Real (low). | The server keeps the tick it stepped (`steppedTick`) and labels the baseline with it. (`Entities.ALateJoinerDuringACollapseGetsTheSameBodies` now compares each awake baseline copy's first sample with the server's pose of the tick it names: 48 copies, 0 m; the old code 0.118 m.) |
| 2 | `ChunkSync::TakeRequests` walked all 101 376 records every `Poll` (normal play: more than 64 slots free), and every window step walked the whole box twice (`BeginWindow`, the ack). | Real (medium). | A count of Unsynced records (every state change goes through `ChunkSync::SetState`): the scan runs only when there are any (`Synced()` also says no at once). `BeginWindow` and `OnAck` visit only the cells that entered (whole columns outside the previous box: 2112 per chunk of a step of the game's window, so 4224 for a session client, whose window steps 2 chunks at a time with the slack of 6 (recheck of 7a, 5); `SentWindow` keeps the previous box). `ChunkSyncStats::recordsVisited` counts it. Measured in the game on the arena: chunk sync's own work 0.017 ms per `Poll` on average, 0.72 ms at most (the join's first ack). (`ChunkSync.AWindowStepVisitsOnlyItsEnteringSlab`: 2 x 2112 per step, 2 x 4180 for a diagonal one, none for a `Poll` with nothing Unsynced; the old walk fails it.) |
| 3 | A bulk message larger than what is left of the pacer's window was held until nothing at all was in flight to the peer: live events every tick (a fight, a 100 ms round trip) kept it - and the whole bulk queue behind it - waiting for as long as they flowed. | Real (medium). | Transports report bytes in flight per channel (`ITransport::ChannelBytesInFlight`, exact on the loopback, the impaired link and ENet: each reliable packet's release callback knows its channel). A message that does not fit waits only for the peer's **bulk** in flight to drain and then goes alone, whatever the events; and a bulk message may always go when no bulk is in flight (`ServerStats::bulkAlone`) - **too loose** (recheck of 7a, 1): only while the bytes in flight are under the window. The reviewers' alternatives (half the window, a hold age of about 2 RTT) were not taken: when the bulk itself fills the link (the shared-uplink pacing test) a hold age would push a 10 KB message past the window every few ticks; counting the bulk channel separates the two cases exactly. (`Session.AnOversizeBulkMessageGoesWhileEventsKeepFlowing`: a 98 KB chunk, an event every tick at 100 ms RTT: the joiner syncs after 24 ticks; with the old rule never.) |
| - | Found while fixing 3: the pacer's drain rate counted only bulk as sent, so while only events flowed the in-flight count never fell between two updates and the rate decayed to its 1 KiB/s minimum: the window stood at its 1200 B floor whenever bulk started again (a window step during a fight). | Real (medium). | Every reliable byte handed to the peer since the last update (the events too) is added to what was in flight (`BulkPacer::Sent`), so the drain is what left. `BulkPacer.TheWindowDoesNotGrowWithItsOwnQueue` is unchanged (9.2 KB, 61 ms). |
| 4 | The pacer's base RTT was the lowest since the join and never aged, and the window halved every tick while the RTT was 33 ms above it: a lasting rise of the path's RTT (a route change, roaming) pinned the window to its floor for the rest of the session. | Real (medium). | The base is the lowest RTT of the last 12 periods of 10 s (`BulkPacerSettings::baseRttPeriod`, `baseRttWindows`, LEDBAT's ageing), and the window halves at most once per round trip. (`BulkPacer.ALastingRiseOfTheRttBecomesTheNewBase`: RTT 100 -> 160 ms for good: 6 halvings in the first second (old: 60), at the floor for 110 s, then back to 12.7 KB, the new path's bandwidth-delay product, base 160 ms; the old pacer stays at 1200 B.) |
| 5 | The start gate waited for the whole entity baseline, every far body included: 37 entities / 118 KiB on the arena, est. 0.5-0.8 MiB at the piece caps, 8-25 s on a 32-64 kB/s link (amendment 8's "under 100 KB" was wrong). | Real (medium). | The near-set marker the amendment deferred: the server orders the baseline by the distance from the spawn to each model's bounding sphere and sends `BaselineNear` (message 15, `BaselineEnd`'s 9-byte layout) after the entities within `ServerSettings::baselineNearMetres` (32 m); the client's start gate opens on it (or on `BaselineEnd`). Far copies keep coming; their events are held as before; `Synced()` still waits for `BaselineEnd`. A body that moves into the 32 m after the join is not waited for (it moves: its contact is a correction anyway). (`ClientWindow.AJoinerStartsBeforeAFarBaselineEnds`: 229 crates, 225 of them 40-68 m east, 64 kB/s: the near set is in after 0.02 s, the player starts after 0.15 s, the baseline ends after 1.08 s (62 KiB); with the old gate it starts with the end. `Wire.ChunkSyncMessagesRoundTripAtTheirSizes`, the forged-bytes test.) |
| 6 | The window recentred whenever the focus entered another chunk; with unloaded edits dropped, a player stepping back and forth across a chunk border re-fetched the modified chunks of a slab each time (the arena's second bot: 2313 `ChunkData` / 4.6 MiB). | Real (medium). | Follow hysteresis: `WorldConfig::recenterSlackChunks`, 1 for a session client's moving window (`sim::kClientWindowSlackChunks`; offline 0, unchanged): the window moves once the focus chunk is more than one chunk from its centre chunk, then centres on it. ~~The view still reaches 23-25 chunks ahead.~~ **Corrected** (recheck of 7a, 2): 22-24 whole chunks lie ahead of the focus chunk (offline 23), and `World::ViewRadius` now leaves the slack out (88 m instead of 92 m for the game's window), so the edge fog ends before the window's edge on the leading side. Measured on the arena walk: the second bot 1483 `ChunkData` / 3.1 MiB, the walker 60 window updates instead of 119, the server 2022 `ChunkData` / 4.8 MiB instead of 2829 / 6.1 MiB. A client cache of the last slab left (replayed instead of re-requested) is not built. (`World.TheWindowFollowsWithSlackAndListsEveryStep`.) |
| 7 | The join latency criterion (p99 under 250 ms during a join) was asserted nowhere; the unit test's bound for the joiner grew with the largest bulk message; the stage was marked accepted. | Real (low-medium); one part rejected. | `ChunkSync.LateJoinDuring10CastsPerSecondConverges` adds a run at 128 kB/s per peer with the bulk capped at 32 kB/s and asserts p99 < 15 ticks for both clients (A 13, B 12); B's relative bound on the 64 kB/s link is now a constant (A's quiet p99 + 10 ticks; B 16 against 21). `vortex_server --max-join-p99-ticks N` fails the run above N; the CLAUDE.md drill command uses 15 (measured 11 for the bot playing, 4 for the joiner). **Rejected:** the target on the test's 5 %-loss link at any rate - the quiet run's p99 is 21 ticks at 128 kB/s too: a lost reliable packet is resent after about 130 ms and holds the 8 ticks of events behind it, so about 40 % of the events wait; the 128 kB/s run therefore uses 1 % loss. |
| 8 | Bulk installs had no per-frame budget (after the game's 3 s `Attach` everything that arrived was installed in one `Poll`), a ring replay decoded each op once per chunk (a large `ClearMask` dozens of times), and none of it, nor the server's encodes, was timed. | Real (low-medium). | `ClientSettings::maxBulkPerPoll` (64 `ChunkData` / `BaselineEntity` per `Poll`, in arrival order: the server sends nearest first, and the markers keep their place; a count, not a time, so runs stay reproducible); the op ring keeps its last 16 decoded ops (`OpRing`) - **replaced** (recheck of 7a, 3): a FIFO of 16 never hit once a chunk's replay touched more than 16 ops; chunks that catch up together now walk the ring once, each op decoded once (`OpRing::Walk`, `ChunkSyncStats::opsDecoded`); `ClientStats::syncSeconds` / `bulkSeconds` (sum, largest `Poll`, the most installs in one) and `ServerStats::encodeSeconds` / `maxEncodeSecondsPerTick`, logged on the `Join:` lines (bots, the game) and `vortex_server`'s `Sync:` line. Measured: the game on the arena 2.0 ms for its largest `Poll` of installs (64), 3.6 ms over the join; a bot 1.6 ms at most; the server's bulk work 14-16 ms per arena join, 1.8 ms in its busiest tick. Nearest-first reordering of pending bulk was not built (the server's order already is). (`Client.BulkInstallsAreBoundedPerPoll`.) |
| 9 | The game's CLAUDE.md said the server's status line shows the event log's size. | Real. | It names the modified map, the event backlog and the bulk bytes. |
| 10 | The game streams every frame (the window follows the camera) and polls on ticks: two steps between `Poll`s (a third-person camera swinging, a diagonal walk, frames without a tick) reset the whole window - every record NoBase, every modified chunk sent again, the player held until it came. No test streamed between `Poll`s. | Real (medium). | `World::TakeWindowSteps` (with `reportLoads`) lists every window corner since the last take; `Client::CheckWindow` sends one update per step, in order, so a chunk that left at one step and came back at the next enters again with the second update (the server sends it again). A reset remains only for steps the client did not see listed (never in play). Plus the hysteresis of 6. (`ClientWindow.TwoWindowStepsBetweenPollsAreTwoUpdates`, the rewrite of `TwoWindowMovesUnseenResetTheWindow`: two updates, no reset, the chunk sent again; `ClientWindow.AWindowThatStepsBetweenPollsWhileWalkingNeverResets`: a walker whose window is pulled 10 m off its feet between its ticks, 169 extra steps: 0 resets, 0 ticks held, equal hash; the old rule: 192 resets.) |
| 11 | = 3. | Duplicate. | - |
| 12 | `ModelRequest`s were neither deduplicated nor bounded on the server; the client re-asked an id already asked when its held events overflowed. | Real (low) on the server; rejected on the client. | Server: an id whose answer is still queued is not queued again (`Peer::modelsQueued`, `ServerStats::modelRequestRepeats`: the queued answer is encoded at its dequeue, after the request came); more than `ServerSettings::maxQueuedAnswers` (131 072) queued answers disconnect the peer as a broken sync stream. Client: **the re-ask is needed** - an answer already on its way, encoded before the dropped events, would miss them - so it stays. (`Session.RepeatedModelRequestsAreAnsweredOnceAndAnswersAreBounded`: 100 requests, one answer, 99 repeats; asked again after the answer left, answered again; 65 chunk requests over a bound of 64 end the peer.) |
| - | Found while verifying 12: after such an overflow the client took the **first** answer that came, even one encoded before the events it had dropped (the copy missed them, and the newer answer was then discarded as a copy that exists). | Real (low). | The client keeps the newest dropped seq per NetId; an answer older than it is discarded (`ClientStats::staleAnswers`), the re-asked one makes the copy. (`Client.AnAnswerOlderThanDroppedHeldEventsIsDiscarded`: 65 held events overflow, the last an `ObjectModel`: the answer as of 1 is discarded, the one as of 66 makes the copy with the new model; the old client keeps the old model.) |
| 13 | The server's handling of hostile or out-of-order sync messages had no test. | Real (low). | `Session.HostileOrOutOfOrderSyncMessagesAreIgnoredOrEndThePeer` (a peer speaking the protocol by hand): a request outside the windows and one for an unknown NetId are ignored and counted, nothing sent; a window that jumps away and back by more than its size gets every modified chunk of its new box marked; an update with a skipped seq, and one with the wrong rows, end the peer (`malformedRequests`). |
| 14 | The full-store walking test bounded only the total of requests: one chunk asked twice and another never would pass. | Real (low). | `Server::ChunkRequestCounts` (per coordinate, as received; since recheck 4 only with `ServerSettings::countChunkRequests`); the test asserts every count is 1 and every chunk Unsynced in the window when the slots free was requested. |
| 15 | The gated-generator tests hung instead of failing (an `ASSERT` left the generator held; the `std::async` guard's future blocked in its destructor), and ctest waited its 1500 s default. | Real (low). | A scope guard releases the generator; the load is a bounded loop on the test thread; every test has a 180 s ctest `TIMEOUT` (the slowest takes about 30 s alone, more under `-j8`). |
| 16 | = 2. | Duplicate. | - |

Verified after the review (this laptop, dev build -O2 -g): warning-free; **671 tests (six disabled), all green**; the new
and changed network tests run three times with equal output. The standard headless game run (`Beam cut: 13 voxels
removed, 1 new object(s)`, `HUD: 149 quads`, feet (17.15, 4.36, -2.99) on the ground; screenshot looked at), the editor
run, `--climb` (0 unloads inside the view radius, window y fixed, 0 unfilled columns), `--tower-blast --after-fill
--frames 700` (freed by shot 6 of 7 by the load check, 11.27x) and `--neck-blast --frames 200` (0 pieces released by
the duds; the tower released, 5.61x) exit 0 under validation: offline is untouched (slack 0, no steps listed).

| Run | Result |
|---|---|
| The CLAUDE.md drill, now with `--max-join-p99-ticks 15` | exit 0, hashes equal, 67/67, 67/67, 66/66, both bots' entities agree (8 objects, rest poses exact); corrections 4 and 2. The late joiner: 62.3 KiB of bulk (103 `ChunkData`, 10 baseline entities), started 0.52 s, synced 0.78 s (server ticks) after its `Welcome`. Latency during the joins p99 11 / 4 ticks, all events 13 / 11. CPU: chunk sync 0.001 ms per `Poll`, bulk installs at most 1.6 ms in one `Poll` (45). |
| The drill with `--clients 3 --join-step 50 --max-join-p99-ticks 15` | exit 0, hashes equal, entities agree; joiners 57.2 and 46.8 KiB, data in after 0.52 and 0.80 s; latency during the joins p99 11 / 11 / 11. |
| The UDP drill (`--enet --clients 3 --join-step 50 --bot-casts 90 --bot-crates 120`), three runs | exit 0 every time, hashes equal, entities agree; corrections 3-6. Event latency p99 (all events) 15-47 ticks for the first bot, 23-75 for the joiners. Three runs with the review's behaviour put back (bulk rule, pacer, near gate, slack, bulk budget) gave the same spread (13-16 and 12-64): the one-thread tool's stalls (a joiner's world load blocks every bot and the server, which then catches up two ticks a pass), not the stream; the "6 ticks" of "Integration and acceptance" was a quiet run. |
| `tools/session-check.sh` | **PASS** (world and entity list hashes equal the server's at the same opSeq); joins 76.4 KiB, started 3.35 / 3.41 s after the `Welcome` (the game's whole-window `Attach`); CPU: chunk sync 0.002-0.003 ms per `Poll` (0.5 ms at most), bulk installs at most 1.8 ms (62 in one `Poll`). Screenshots looked at: the plaza, a crater, crates and broken crates, b's HUD `players 2`. |
| **Arena walk** (the CLAUDE.md command, 33 s wall) | exit 0, both bots' hashes equal the server's over their sync boxes, entities agree (74 objects), drill 40/40. Walker: x -118.1 to +122.0 m, **60 window updates** (119 before), 0 resets, 0 ticks held, 539 `ChunkData` / 1.9 MiB. Second bot: **1483 `ChunkData` / 3.1 MiB** (2313 / 4.6 MiB before), 24 updates. Server: 2022 `ChunkData` / 4.8 MiB (2829 / 6.1 MiB before), none over 16 KiB. Store at most 5500 / 5447 of 12 288 (gate 10 240); RSS 1100 MiB; server world 48.4 MiB RLE. Latency during the joins p99 11 / 10 ticks. CPU: chunk sync 0.003-0.004 ms per `Poll` (1.2 ms at most), bulk installs at most 1.6 ms; the server's bulk work 14-16 ms per join, 1.8 ms in its busiest tick. |
| A headless game client on `arena250.session` (the command of "Integration and acceptance") | exit 0 both; started 2.24 s, data in 1.69 s, synced 7.54 s (wall) after its `Welcome`; 331 KiB of bulk (93 `ChunkData`), 20 model requests; client hash `1dee179d7a9e50a1` = the server's `window hash` over [-24,-12,-24..24,32,24) at opSeq 377; CPU: chunk sync 0.017 ms per `Poll` (0.72 ms at most), bulk installs 3.6 ms over the join, 2.0 ms at most (64 in one `Poll`). Screenshot looked at (a precast crater at the spawn, terrain without gaps). |

**Not done (unchanged by the review):** 7b (`HashCheck`, server terrain mode, the wire `EntityCheck`); splitting large
bulk messages (one 98 KB `ChunkData` now goes alone instead of waiting, but it still blocks a slow link for its length);
encoding chunks against their pristine version (join bytes); a client cache of the slab last left (re-sends at a
modified area's edge are cut by a third by the hysteresis, not removed); the design's absolute 250 ms on a 5 %-loss link
(unreachable: finding 7); windowed play on the arena and the two-machine run are the owner's.

### Recheck of stage 7a (2026-11-17)

A rechecker read the 7a change after the review's fixes: 6 findings, all real (4 partly: the fix taken differs from
the one proposed, as said). Each bug was first reproduced in a GPU-free test that failed on the old code (the old
code with the new counters: each test below failed there for its own reason); 3 new tests, one extended, **674 in all
(six disabled)**. No wire byte changed (`kProtocolVersion` stays 8).

| # | Finding | Verdict | Fix (test) |
|---|---|---|---|
| 1 | Review fix 3 also dropped the window check for small bulk messages: with no bulk in flight one bulk message of any size went whatever the live events in flight, so on a link the events alone saturate (the pacer backed off to its 1200 B floor) bulk still ran at one message per bulk round trip on top of them. | Real (medium-low). | The hold test is `!MayHandOver(inFlight)` again (bytes in flight, events included, at or over the window: nothing goes); a message that does not fit what is left waits for the bulk in flight to drain and then goes alone - which now happens only while the bytes in flight are under the window. The oversize case of fix 3 still moves (a few events in flight are under the window). `ServerStats::bulkOverWindow` counts bulk handed over at or past the window (0 by construction now). (`Session.BulkWaitsWhileLiveEventsAloneFillTheWindow`: a 16 kB/s downlink per peer, about 22 kB/s of events for 4 s, a joiner with a 98 KB `ChunkData` queued: the old rule handed 3 messages over past the window (98.5 KB), the new one none - bulk held 227 ticks, the window at its floor - and the joiner syncs once the events ease, both hashes equal. `Session.AnOversizeBulkMessageGoesWhileEventsKeepFlowing` unchanged: synced after 24 ticks, 1 message alone.) |
| 2 | Follow hysteresis puts the window's edge up to one chunk inside the edge fog on the leading side: with the focus chunk one off the centre the +x edge is 88-92 m away while `World::ViewRadius` stayed 92 m (fog 78-92 m); "the view reaches 23-25 chunks ahead" was wrong (22-24). | Real (low-medium). | `ViewRadius` = (half the window - 1 - `recenterSlackChunks`) chunks: 88 m for a session client's 48-chunk window (fog from 74.8 m), offline unchanged (92 m, slack 0). The edge fog, the fill buckets (0.85 R) and the "unloads inside view radius" count follow it. Docs say 22-24 chunks ahead. (`World.TheViewRadiusNeverReachesPastTheWindowWithSlack`: the focus on the far side of the chunk one off the centre, on each axis and the diagonal: the edge is 16.01 m away in the test's 0.5 m world, the old radius 32 m, the new 16 m.) |
| 3 | The op ring's 16-entry FIFO of decoded ops never hit once the chunks of a batch shared more than 16 ops (a cyclic scan over a FIFO), so review fix 8's "a large edit is decoded once" did not hold, decoded masks of up to 864 KiB stayed alive in it, and nothing counted or tested decodes. | Real (low). | The cache is gone. Chunks that catch up together - one `WindowAck`, one load report, one `Poll`'s consecutive `ChunkData` (`ChunkSync::OnChunkData(span)`, used by `ProcessBulk`, flushed before any other bulk message keeps its place) - walk the ring once (`OpRing::Walk`): each op that some chunk of the batch needs is decoded once and applied clipped to each such chunk in (y, z, x) order; per chunk the ops keep their stream order, and a chunk that fails (not loaded, no slot) stops as before. A batch is sorted and deduplicated (a chunk installed twice in one `Poll` catches up once, from its last base; one whose later install found no slot is left Unsynced). `ChunkSyncStats::opsDecoded`. (`ChunkSync.ABatchCatchingUpDecodesEachRingOpOnce`: 20 overlapping boxes and clear masks, 8 chunks based by one ack: 160 replays, 20 decodes, every chunk equal to the reference; the old ring decoded once per replay.) |
| 4 | `Server::requestCounts` (added for review finding 14's test) was a production map that grew with every coordinate any peer ever sent, counted before validation, never cleared. | Real (low); fix partly as proposed. | Kept behind `ServerSettings::countChunkRequests` (off by default; the full-store walking test turns it on). It still counts as received, not only accepted coordinates: behind a test-only flag the growth does not matter, and a client that asks for a chunk outside its window twice is a client bug the test should see. (`Session.HostileOrOutOfOrderSyncMessagesAreIgnoredOrEndThePeer`: after a request outside the windows a default server's counts are empty; the old server held one.) |
| 5 | Docs and log lines contradicted the code: (a) the status said 7a not reviewed; (b) the start gate row said `BaselineEnd`; (c) CLAUDE.md said message types 8-14; (d) "a step touches 2112 records" (a session step is 2 chunks: 4224); (e) the `Join:` log lines said "baseline in". | Real (low). | (a) status line and the "Integration and acceptance" bullet marked superseded; (b) "**Wrong** (review of 7a, 5): `BaselineNear`" on the row; (c) CLAUDE.md says 8-15; (d) review finding 2's row and the `BeginWindow` comment say 2112 per chunk of a step, 4224 for a session client; (e) both log texts (`src/Session.cpp`, `vortex_server`) say "entities within 32 m of the spawn in (BaselineNear)". |
| 6 | `--max-join-p99-ticks 15` accepted exactly 15 ticks (250 ms) while the design row and the unit test require under 250 ms. | Real (low). | `vortex_server` passes only `p99 < N` (as `EXPECT_LT(wide.p99A, 15u)`) and logs `NOT UNDER --max-join-p99-ticks N`; the docs say "under N ticks". |

Verified after the recheck (this laptop, dev build -O2 -g): warning-free; **674 tests (six disabled), all green**. The
standard headless game run (`Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, feet (17.15, 4.36,
-2.99) on the ground; screenshot looked at), the editor run, `--climb` (0 unloads inside the view radius, window y
fixed, 0 unfilled columns), `--tower-blast --after-fill --frames 700` (freed by shot 6 of 7 by the load check, 11.27x)
and `--neck-blast --frames 200` (0 pieces released by the duds; the tower released, 5.61x) exit 0 under validation.

| Run | Result |
|---|---|
| The CLAUDE.md drill (`--max-join-p99-ticks 15`) | exit 0, hashes equal, 67/67, 67/67, 66/66, both bots' entities agree (8 objects); corrections 4 and 2. The late joiner 62.3 KiB (103 `ChunkData`, 10 baseline entities), started 0.52 s, synced 0.78 s. Latency during the joins p99 11 / 4 ticks (under 15). |
| The drill with `--clients 3 --join-step 50 --max-join-p99-ticks 15` | exit 0, hashes equal, entities agree; latency during the joins p99 11 / 11 / 11. |
| The UDP drill (`--enet --clients 3 --join-step 50 --bot-casts 90 --bot-crates 120`) | exit 0, hashes equal, entities agree (9 objects); corrections 1 / 4 / 10. Event latency p99 during the joins 17 / 70 / 25 ticks, all events 26 / 68 / 23 (inside the one-thread tool's earlier spread of 15-75). |
| `tools/session-check.sh` | **PASS**; joins 75 KiB each, started 2.88 / 3.09 s after the `Welcome`. Screenshots looked at: a's crate on the plaza, b's broken crates and fragments, b's HUD `players 2`. |
| **Arena walk** (the CLAUDE.md command) | exit 0, both bots' hashes equal the server's over their sync boxes, entities agree (74 objects). Walker x -118.1 to +122.0 m, 60 window updates, 0 resets, 0 ticks held, 539 `ChunkData`; second bot 1477 `ChunkData` / 3.1 MiB, 23 updates; server 2016 `ChunkData` / 4.8 MiB, none over 16 KiB, 11 messages alone past the window. Store at most 5500 / 5431 of 12 288; RSS 1109 MiB. Latency during the joins p99 11 / 4 ticks. |

**Not done (unchanged by the recheck):** as after the review (7b, splitting large bulk messages, pristine-delta
encoding, a cache of the slab last left, the absolute 250 ms on a 5 %-loss link, windowed play on the arena and the
two-machine run). New open point: while live events alone fill a peer's link, its join waits (finding 1: the pacer's
job); a fight that saturates a joiner's downlink for long delays its sync, and no minimum bulk share is reserved.

### Stage 7b plan (2026-11-17)

Status: **plan, superseded by "Stage 7b as built"** (M-a and M-b built, integrated and accepted 2026-11-17). Kept for
its reasoning; the amendments at the end of this plan win over the sections above them, and "Stage 7b as built" wins
over both where the code differs (12 pristine slots instead of 6, `Content` installs that keep the modified bit,
the incoming-slots rule, the bounds of the latency tests).** The rest of row 7: `HashCheck` repair, server terrain mode, the wire `EntityCheck` with the
list re-sent and bounded tombstones, the whole "Testing" table on a 250 m region, and two Windows-build fixes. Code facts
were read at engine `62f28b4` / game `92e3902` (7a committed). Everything below builds on "Stage 7a as built" and its
review and recheck; nothing there changes except where a row says so. "est." = estimate, replaced by the measurement
named.

#### What the code gives 7b

| Code fact (62f28b4) | Consequence for 7b |
|---|---|
| The client applies the events channel in arrival order from `pending` (`EventBatch` and `WindowAck` items, `ApplyPending`): an item that follows the events up to A is processed with `applied == A`. The server sends every event before anything else of a tick (`Pump` before the ack in `OnWindowUpdate`; live events are never held unless a test sets `maxEventBytesInFlight`). | A server message on the events channel sent right after `Pump` is seen by the client **exactly at the stream position it was made at**. That is how a check compares at the same opSeq without either side keeping history. |
| `ChunkSync::Record` has `state`, `ready`, `base`, `fromData`; `Requested` chunks take no events and are not current; `TakeRequests` sends owed coordinates of records still `Requested`; `OnChunkData` installs whole and replays `(B, applied]` from the ring or re-requests when the ring no longer covers B. | A repair is only "this current chunk becomes `Requested`": the 7a request, install and replay path does the rest; the base rule is never bypassed. |
| `voxel::ChunkHash` is canonical (Empty, Uniform, Dense and Packed content of the same voxels hash alike); `WorldHash` reads any loaded cell. No call hashes one chunk of a world. | One small voxel helper, `ChunkHashAt(world, chunk)`, shared with `WorldHash`'s per-cell code. |
| `OnWelcome` already computes `RehashProbes` with the client's own generator and counts `probeMismatches`, then plays on. `Attach` compares the loaded world's probe chunks with that generator. Both the game (`Session.cpp`) and `vortex_server`'s bots build their world from `Client::MakeGenerator()`. | Terrain mode can start at the `Welcome` without a new round trip, and a generator wrapper made in `MakeGenerator` reaches every client. |
| `WindowUpdate::kTerrainMode` (2) is reserved and refused by the decoder; the server's entering pass marks only chunks in its modified map. | 7b accepts the bit and adds a second ack form. |
| `World::SetChunkEncoded` installs a Dense chunk with `modified = true`: never packed, never repacked (`Repack` skips modified cells). The arena's server window holds 10 518 dense and 31 084 packed chunks (packed = buried). | A client that receives a whole window would keep every buried chunk resident: est. 15 000 extra slots for the game's window, over its 12 288. Terrain mode needs an install that packs like a load. |
| The client keeps `tombstones` (an unbounded set) and `Copy{entity, resting, modelHash, base}`; `ObjectModel` already replaces a copy (destroy, `MakeCopy`). `BaselineEntity` for an existing copy is discarded. | The list re-send can reuse the replace pattern; tombstones can be bounded once a check catches what an evicted tombstone would have stopped. |
| `ServerStats` / `ClientStats` have no check counters; `CompareEntities` / `EntityListHash` exist only for tests and logs. | New counters, logged on the exit lines. |
| `src/net/internal/Enet.cpp` includes `<enet/enet.h>` first in its global fragment with no Windows guard. `std::optional` users in `src/net`, `src/sim` and `tools/server` all include `<optional>` (checked: `Server.cpp` got it in 7a). | Fix 1 is one guarded block; fix 2 is already in the tree and only needs the Windows guide corrected. |

#### Messages (protocol 9)

`kProtocolVersion` = 9 (it is part of `buildHash`, so a protocol-8 client is refused as `Protocol`). **Every wire change of
7b lands in M-a**; M-b changes no byte (if M-a is committed on its own and M-b then needs a byte, M-b bumps to 10). New
`MessageType` values 16-20. Every decoder refuses trailing or missing bytes; the forged-bytes test covers each.

| Type | Message | Channel | Layout (little endian) | Bytes |
|---|---|---|---|---|
| 8 | `WindowUpdate` (changed) | client -> server, events | unchanged; flag bit 1 `kTerrainMode` is now accepted (other unknown bits still refused) | 24 |
| 11 | `ChunkRequest` (changed) | client -> server, events | type u8, **`reason` u8** (0 `Sync`: ring not covering / unsynced; 1 `Repair`: a `HashCheck` mismatch; others refused), `count` u16 (1-256), `count` x (3 x i32) | 4 + 12 n |
| 13 | `BaselineEntity` (changed) | server -> client, bulk | type u8, `asOfOpSeq` u32, `tick` u32, `restTick` u32, **`flags` u8** (bit 0 `kRelist`: part of an entity list re-send; others refused), then the `EntitySpawn` event | 14 + 50, or 14 + 60 + model |
| 16 | `TerrainAck` (**protocol 10**, review of 7b: after `size` a LEB128 count of the cells coded, and the runs code the update's *entering* cells only, in `net::ForEachEnteringChunk` order; the layout in this row is protocol 9's) | server -> client, events | type u8, `windowSeq` u32, `asOfOpSeq` u32 (A), `size` 3 x u16 (= the update's), then code runs over the window box in `VoxelMask` order (x fastest, then y, then z): repeated `{run LEB128 >= 1, code u8, id u8 only when code = 2}` until the runs add up to the box. Codes: 0 = not part of this ack (not entering, or outside the server's window: the client's generator makes it, never edited); 1 = data follows (`ChunkData` on bulk); 2 = uniform chunk `id` (0 = air) as of A | 15 + runs (est. 2-30 KB at a join of the game's window, 0.1-3 KB per window step) |
| 17 | `HashCheck` | server -> client, events | type u8, `asOfOpSeq` u32 (C), `count` u8 (1-32), `count` x {`chunk` 3 x i32, `hash` u64 (`ChunkHash` of the server's chunk at C), `flags` u8 (bit 0 `pristine`: not in the server's modified map at C; others refused)} | 6 + 21 n (342 at the default 16) |
| 18 | `EntityCheck` | server -> client, events | type u8, `asOfOpSeq` u32 (C), `count` u32, `idXor` u32, `modelXor` u64 | 21 |
| 19 | `EntityListRequest` | client -> server, events | type u8, `asOfOpSeq` u32 (the check that failed) | 5 |
| 20 | `EntityListEnd` | server -> client, bulk | type u8, `asOfOpSeq` u32 (when it was encoded), `listedAt` u32 (Q: the opSeq the list's NetIds were taken at), `count` u32 (`kRelist` entities sent) | 13 |

`EntityCheck` over replicated **objects** (every `scene::NetId` entity with a body; template lights are not counted,
choice 9): `count`, `idXor` = xor of NetIds, `modelXor` = xor of `HashMix64(netId, VolumeHash(model))` (choice 8: a plain
xor of model hashes cancels for any two crates). Server hashes cost one `VolumeHash` per object per check (est. under
0.3 ms for 300 objects), the client's come from `Copy::modelHash`.

Rates per client: `HashCheck` 0.17 kB/s, `EntityCheck` 0.01 kB/s. Refused by the server as a broken sync stream (the
7a rule: disconnect, `malformedRequests`): a `ChunkRequest` with an unknown reason, a `WindowUpdate` without
`kTerrainMode` from a peer that is in terrain mode (choice 12), an `EntityListRequest` before the peer joined.

#### `HashCheck`: server

- **When.** Per joined peer every `ServerSettings::hashCheckTicks` (120 = 2 s), on ticks with `tick % 120 == (17 x
  slot) % 120` (staggered), only after the peer's join data went out once (`JoinRecord::logged`: its bulk queue ran
  empty) and only when its events are all handed over (`sentUpTo == LastOpSeq`; never false outside the
  `maxEventBytesInFlight` test hook). Built in `Flush`, right after that peer's `Pump`, so C = `LastOpSeq()` and every
  event up to C precedes it on the ordered events channel. Despawns of the tick are drained before `Flush` (asserted).
- **Which chunks** (a GPU-free `net::HashCheckPlanner` in `vortex.net:sync`, unit-tested without a server). Candidates
  are chunks of the peer's sync box (its last window inside the server's) only. Per check, in this order, no chunk
  twice, at most `count` 32:
  1. **Recent** (up to `hashCheckRecent` = 4): the chunks changed by the server's last operations, from a ring of the
     last 64 `(seq, chunk)` pairs fed by `EmitOp` from `EditReport::changed`, newest first.
  2. **Modified** (up to `hashCheckModified` = 8): chunks of the modified map, least recently checked for this peer
     first, ties nearest the player (3D distance from the player's feet to the chunk box), then (y, z, x).
  3. **Pristine** (up to `hashCheckPristine` = 4): chunks not in the modified map within `hashCheckPristineMetres`
     (12 m) of the player's feet **or** of its look-ahead point (feet + velocity x `hashCheckLookAheadSeconds`, 1 s),
     the server's Dense / Packed cells before its Uniform / Empty ones, least recently checked first, then nearest the
     look-ahead point (choice 4).
  "Least recently checked" is a per-peer map chunk -> tick of its last check; entries outside the peer's window are
  dropped at each check, so it is bounded by the window (in practice by what was checked: a few hundred).
- **Hash.** `voxel::ChunkHashAt(world, chunk, out)` (new, `vortex.voxel`, next to `WorldHash`: Empty / Uniform from the
  cell, Dense from the slot, Packed decoded into a scratch chunk; false when not loaded, which never happens in the
  server's window). Cost est. 5-60 us per chunk, at most 16 chunks per peer per 2 s; logged
  (`ServerStats::checkSeconds`).
- **Counting.** `hashChecksSent`, `hashCheckEntries` (by kind); `repairsRequested` = `ChunkRequest` coordinates with
  reason `Repair` (the server's view of mismatches); `repairsSent` = `ChunkData` answers of such requests (`BulkItem`
  carries a `repair` flag). Repair answers go to the front of the bulk queue (7a's request FIFO, unchanged).

#### `HashCheck`: client

- The message joins `pending` (the stream item variant gains `HashCheck`, `EntityCheck`, `TerrainAck`) and is processed
  in stream order by `ApplyPending`. When it is processed `applied == C` (the stream contract above); if not, the check
  is ignored and counted (`checksOutOfOrder`, expected 0; never a failure).
- `ChunkSync::OnHashCheck(world, check, applied) -> HashCheckOutcome` (GPU-free, in `:sync`). Per entry:

| Client record of the entry's chunk | Outcome |
|---|---|
| not in the window, or outside the server's window | skipped (`checkOutside`) |
| not current (NoBase, Awaiting, Requested, Unsynced, Based but not ready) | skipped (`checkNotCurrent`): its content is not trusted anyway |
| current, `base > C` (data ahead of the stream, the carve + swap case) | skipped (`checkAhead`): it holds events the check does not |
| current, `base <= C` | `ChunkHashAt` compared with the entry: equal -> `checkEqual`; different -> **mismatch** |

  Why that is the same stream position: a current chunk with `base <= C` holds its base content plus every operation in
  `(base, applied]` that touches it (the base rule), and `applied == C`; the server's hash is its chunk after every
  operation up to C. So both are "the chunk after operation C".
- **Mismatch** -> the record becomes `Requested` (`SetState`, so `Synced()` and the hash box see it at once), its
  coordinate goes to a `repairs` list sent in one `ChunkRequest{reason Repair}` per `Poll`; counted by kind
  (`mismatchesModified`, `mismatchesPristine`). From here it is 7a's path: events touching it are skipped (`chunkSkips`),
  its `ChunkData` at B installs and replays `(B, applied]` from the ring (or is requested again with reason `Sync` when
  the ring no longer covers B), the player is held over it (7a's movement gate), it is not hashed. `repairsInstalled`
  counts installs of chunks whose last request was a repair. A chunk that mismatches again within 10 s of its repair is
  counted (`repeatRepairs`, logged: a bug, not a loop - each check sends at most 32).
- **Pristine failures** (the trigger of terrain mode): an entry flagged `pristine` that mismatches on a record that is
  **not** `fromData` (this client's generator made it). The client keeps the set of such coordinates; when it holds
  `ClientSettings::pristineFailuresForTerrainMode` (3) distinct chunks, terrain mode starts (choice 5).
- Cost: est. under 1 ms per check on the game client (16 hashes, Packed ones decoded; measured 2.0-3.7 ms at most in
  three-process runs, about 0.3 ms of it hashing - review finding 4), logged
  (`ClientStats::checkSeconds`, largest single check).

#### Server terrain mode

**Start.** Two triggers, both on the client; the server only follows the flag.

| Trigger | When | What the client does |
|---|---|---|
| Probes | `OnWelcome`: any of the 64 probe hashes differs from the client's own (the existing `RehashProbes`, now through the generator wrapper) | terrain mode from the start: the first `WindowUpdate` (sent at the `Welcome`, amendment 12) carries `kTerrainMode`; `Attach` skips its loaded-probe check (the world is the wrapper's); logged `Client: N of 64 probe chunks differ from the server's: server terrain mode` |
| 3 pristine chunks | `OnHashCheck` reached 3 distinct pristine failures | the wrapper switches; the next `WindowUpdate` is `{seq + 1, kReset | kTerrainMode, the current window}`: `BeginWindow` makes every record NoBase, so nothing generated here is trusted any more; logged with the tick |

Terrain mode is **sticky** for the session (no way back; choice 12). `ClientSettings::terrainModeOnProbeMismatch`
(default true) exists for the test that shows the pristine trigger alone.

**The generator wrapper** (`net::SessionGenerator`, `vortex.net`, made by `Client::MakeGenerator`): holds the region's
generator (optionally wrapped by `ClientSettings::wrapGenerator`, the tests' and `vortex_server`'s broken generators), the
server's window and an atomic `terrainMode`. In terrain mode `Generate` for a chunk inside the server's window returns
Empty at once (no dense generation, the "stops generating terrain" of the design); `ClassifyColumn` and
`VerticalBounds` still delegate (cheap; they make the column pass place sky and rock rows without jobs, which spares
installs below: the ack is authoritative anyway). Outside the server's window it generates as before (never edited,
never sent, not in the sync box). Switching mid-session breaks "a pure function of the coordinate" for jobs already in
flight; harmless: the reset makes every cell NoBase, and an install bumps the cell's generation so a late job result is
discarded (7a's `SettleLoading`).

**The server.** A `WindowUpdate` with `kTerrainMode` marks the peer (`Peer::terrainMode`, sticky). Its entering set
(7a's rule: the new box minus the old one, all of it with `reset` or for the first update) inside the server's window
is answered with a `TerrainAck` instead of a `WindowAck`, after `Pump`, at A = `LastOpSeq()`: per entering cell the
server's `World::StateOf` - Empty: code 2, id 0; Uniform: code 2, its id; Dense / Packed: code 1 and the chunk is queued
(`QueueChunk`, deduplicated as in 7a). Modified or not makes no difference: the server's content at A is what is sent.
Cost: one `StateOf` per entering cell (101 376 at a join of the game's window, 4224 per two-chunk step; est. 1-3 ms at a
join). For a terrain-mode peer the bulk queue is sorted by **3D** distance from the player's eye (7a: horizontal), so the
surface comes before the buried rock below it (choice 6); `bulkNearMetres` is then 3D too. Everything else is 7a's:
encoded at dequeue, paced, request answers first, dropped when the chunk left the window.

**The client.** `ChunkSync::OnTerrainAck(world, ack, applied)` in stream order (`applied == A`), for records NoBase that
entered with that seq: code 2 -> if the cell already holds exactly that uniform content (`StateOf` Empty / Uniform with
that id) it is only based, else `World::SetChunkEncoded` with the 1-2-byte uniform encoding; either way `Based(A)`,
`fromData`, ready (no replay: `applied == A`); code 1 -> `Awaiting`; code 0 inside the server's window for an entering
record -> malformed (fail). Data installs use the new **content install** (below). Everything else - replay, requests,
unsynced, the start and movement gates, `Synced()`, `Hash()` - is 7a's unchanged.

**World change: content installs** (`vortex.voxel`). `SetChunkEncoded(chunk, rle, InstallMode mode = InstallMode::Edit)`.
`Edit` is 7a's behaviour (`modified = true`, never packed). `Content` (terrain mode only, choice 7) installs the chunk as
a load would: `modified` stays false; a Dense chunk that `Visible` says is hidden and that lies beyond `nearRadius` from
the focus becomes Packed with the message's own RLE (canonical, so packing is not a content change) and its
`MostFrequentSolidId`; a hidden one nearer is Dense with its bytes kept and listed in `m_Resident`, so `Repack` packs it
once the focus moves on. Without this a terrain-mode client would hold every buried chunk resident (est. +15 000 slots
on the game's window). Tested by `World.ContentInstallsPackHiddenChunksLikeLoads`.

**With the rest of 7a.**

| Piece | Terrain mode |
|---|---|
| Moving window | every `WindowUpdate` carries the flag; each step's entering slab gets a `TerrainAck`; queued chunks that left are dropped at dequeue (7a) |
| Ring | unchanged: `ChunkData` at B replays `(B, applied]`; ack-based uniform chunks need no replay |
| `ModelRequest` | unchanged: a replicated `Detach` over chunks that are not current asks for its model (more of them during a terrain-mode join) |
| Store | dense installs need slots as generated chunks did; buried ones pack (content install); a full store is 7a's Unsynced path, requested once more than 64 slots are free |
| `HashCheck` | runs as before; a terrain-mode client's chunks are all `fromData`, so no pristine failure can count |
| Probe check in `Attach` | skipped in terrain mode (its world is the wrapper's air); the generator-side count is still logged |
| Start gate | 7a's (32 m current, `BaselineNear`); in terrain mode it waits for the 3D-nearest data, est. 0.5-3 MB on the arena |

**Cost** (to be measured in M-b, written into "as built"): bytes per join (the `TerrainAck`s plus the `ChunkData`) and
per window step, on `playground.session` (fixed 30 x 44 x 30 window, est. 3-10 MB) and on `arena250.session` with the
game's 48 x 44 x 48 window (the design's 8-25 MB; est. 20 % of the window's cells are non-uniform at 1.2 KB average:
about 24 MB, and 0.5-1 MB per two-chunk step). The bulk pacer's 32 KiB ceiling bounds the rate to 32 KiB per round trip:
about 0.3 MB/s at 100 ms (seconds to a minute for a whole window), 5-10 MB/s on a LAN. Logged per terrain-mode peer
(server) and per client: `Terrain: ... since the Welcome (probes 64/64 differ) | at tick T (3 pristine chunks); join:
ack K KiB, M ChunkData / X MiB, U uniform based without data; steps n, ack + data per step avg A / max B KiB; held
ticks H`.

#### Wire `EntityCheck`, the list re-send, tombstones

- **Server.** Per joined peer every `entityCheckTicks` (120), offset by 60 ticks from its `HashCheck`, in `Flush` after
  `Pump` (C = `LastOpSeq()`, despawns drained), only while the peer's join baseline has ended and no list re-send of
  it is open. The three values are computed once per tick when any peer needs them.
- **Client.** In stream order (`applied == C`). Compared only when the entity state is settled: the join baseline
  ended, no re-send open, nothing held (`held` empty), no `ModelRequest` out or owed; otherwise skipped and counted
  (`entityChecksSkipped`; choice 10). The client's values: its `copies` (count, xor of NetIds, xor of
  `HashMix64(netId, Copy::modelHash)`). Mismatch -> `EntityListRequest{C}` once (`relistRequested`; no second request
  until the list ends; after 3 consecutive mismatching lists the next request waits 10 s, then 30 s: a client that
  cannot make a copy - bodies full - does not ask every 2 s).
- **Server re-send.** On `EntityListRequest`: Q = `LastOpSeq()`; every live object's NetId, nearest the peer's player
  first, into a per-peer `relist` deque; bulk order becomes: request answers, chunks within `bulkNearMetres`, the join
  baseline, **the re-send**, the remaining chunks. Each item is encoded at dequeue as a `BaselineEntity` with `kRelist`
  (`MakeBaselineEntity`, so a resting object at its exact rest pose); a NetId gone by then is dropped. After the last:
  `EntityListEnd{asOf, Q, count}`. One re-send per peer at a time: a request while one is open is merged (counted
  `relistMerged`).
- **Client re-send.** While a re-send is open, live entity events for an unknown, not tombstoned NetId are held (7a's
  hold condition gains `|| relistOpen`). A `kRelist` entity n at asOf B:

| Client state of n | Rule |
|---|---|
| tombstoned | discarded |
| copy with `base >= B` | kept (it is as new as the list), n listed |
| copy with `base < B` | **replaced**: the copy is destroyed (no tombstone) and made again from the spawn, `base = B`, the rest rule as for a baseline; held events of n above B applied; `copiesReplaced` |
| no copy | made as a baseline copy, held events above B applied; `copiesRestored` |

  `EntityListEnd{asOf, Q}`: every copy **not listed** whose `base <= Q` is removed and tombstoned (`copiesDropped`): it
  was dead at Q on the server, and a copy made after Q (`base > Q`) is never touched. Held events of NetIds still
  without a copy and not asked for are dropped (as at `BaselineEnd`). The next check compares again. Processing the end
  marker does not wait for the stream to reach Q: a copy whose live spawn (`<= Q`) arrives later is refused by
  `MakeCopy` (it exists); a despawn `<= Q` that arrives later finds no copy and tombstones the id.
- **Tombstones bounded.** A FIFO of the last `ClientSettings::maxTombstones` (4096, 16 KB) despawned NetIds plus the
  set; the oldest is evicted (`tombstonesEvicted`). What a tombstone stops is a `BaselineEntity` or model answer encoded
  before the despawn; with an evicted tombstone such an answer re-creates a dead copy, and the next `EntityCheck`
  (count and id xor differ) makes the re-send drop it within about 4 s (choice 11). Server memory per peer: the
  `relist` deque, bounded by the live objects (256 pieces + crates).

#### Server and client state (summary)

| Where | New state |
|---|---|
| `ServerSettings` | `hashCheckTicks` 120, `hashCheckRecent` 4, `hashCheckModified` 8, `hashCheckPristine` 4, `hashCheckPristineMetres` 12, `hashCheckLookAheadSeconds` 1, `entityCheckTicks` 120 (0 turns a check off: tests) |
| `Server::Impl` | `recentChanged` ring (64 `(seq, chunk)`), the per-tick entity-check values |
| `Peer` | `terrainMode`, `checked` (chunk -> last check tick), `relist` deque, `relistOpen`, `relistAt` (Q), `relistSent`; `BulkItem::repair`, `BulkItem::Kind::Relist` |
| `ServerStats` | `hashChecksSent`, `hashCheckEntries` by kind, `repairsRequested`, `repairsSent`, `terrainModePeers`, `terrainAcks` / bytes, `terrainChunkData` / bytes, `uniformCoded`, `entityChecksSent`, `entityListRequests`, `relistsSent`, `relistMerged`, `relistEntities` / bytes, `checkSeconds` |
| `ClientSettings` | `pristineFailuresForTerrainMode` 3, `terrainModeOnProbeMismatch` true, `maxTombstones` 4096, `wrapGenerator` (tests, `vortex_server`) |
| `Client::Impl` | `terrainMode`, `terrainModeReason`, `terrainModeTick`, `generator` (the `SessionGenerator`), the pristine-failure set, `repairs`, the repair set, `relistRequested` / `relistOpen` / `listed`, the tombstone FIFO, the relist back-off |
| `ClientStats` | `hashChecks`, `checkEqual`, `checkAhead`, `checkNotCurrent`, `checkOutside`, `checksOutOfOrder`, `mismatchesModified`, `mismatchesPristine`, `repairsRequested`, `repairsInstalled`, `repeatRepairs`, `terrainAcks` / bytes, `uniformBased`, `uniformInstalled`, `entityChecks`, `entityChecksSkipped`, `entityMismatches`, `relists`, `copiesRestored`, `copiesReplaced`, `copiesDropped`, `tombstonesEvicted`, `checkSeconds`, `maxCheckSeconds` |
| `Client` API | `TerrainMode()`, `TerrainModeReason()`; test hooks: `ClientSettings::testDropEntityEvent` (a predicate on live entity events: drop a despawn or a model to make a mismatch), `Client::TestDestroyCopy(netId)` |
| `ChunkSync` | `OnHashCheck`, `OnTerrainAck`, `TakeRepairs`, `IsFromData` |
| `vortex.voxel` | `ChunkHashAt`, `InstallMode` on `SetChunkEncoded` |

#### Tests

All GPU-free and seeded, over `ImpairedTransport` with the manual clock, waiting for idle between ticks (stage 6
reproducibility). **The 250 m region**: `Plain250`, a flat region of 64 x 64 chunks of play at 0.125 m (256 m; server
window 70 x 8 x 70 with rows 0-32 m), the bots with the game's 48-chunk window (a moving window) and the game's store
(12 288 slots, 2048 kept free), unless a row says otherwise. Flat ground keeps preload under a second (choice 13); real
terrain at 250 m is measured by `vortex_server` on `arena250.session` (runs below). The `Field` fixture of
`ClientWindowTests.cpp` moves to `tests/net/FieldHarness.h` (a non-module header included after the imports; if that
fails with modules, the new tests go into `ClientWindowTests.cpp`), parameterised by region, with `Arena`'s presenting
bots and `Settle` (all bodies resting) added; `Region250Tests.cpp` uses it. Every `Region250.*` test asserts at its end:
each bot `Synced()` at the server's last opSeq, its hash over its sync box equal to `Server::Hash(box)`,
`CompareEntities` agreeing (ids, models, rest states, rest poses and rotations exact), and - except where a fault is
injected - **0 hash mismatches, 0 entity mismatches and 0 `checksOutOfOrder`** with at least one check compared per
2 s: the checks' own false positives are the main risk of 7b.

| Test | M | Asserts | Fails if the mechanism were broken because |
|---|---|---|---|
| `Wire.CheckAndTerrainMessagesRoundTripAtTheirSizes` (+ `Wire.ForgedBytesNeverCrashADecoder` extended) | a | messages 16-20 and the changed 11 / 13 at the sizes above; refusals: a check count of 0 or 33, an unknown entry or reason or `BaselineEntity` flag bit, `TerrainAck` runs that do not add up, a run of 0, code 3, code 2 without its id | a decoder that takes a short run installs chunks outside the box |
| `World.ChunkHashAtEqualsChunkHashInEveryCellState` | a | Empty, Uniform, Dense, Packed and a resident buried chunk of the same voxels hash alike and equal `ChunkHash`; false when not loaded | a Packed server chunk would never match a Dense client chunk |
| `HashCheckPlanner.PicksRecentModifiedAndPristineChunksLeastRecentlyCheckedFirst` | a | only chunks of the sync box; recent newest first; every modified chunk of the box checked within ceil(n / 8) checks; pristine within 12 m of the feet or the look-ahead point, server-dense first; no chunk twice in one check | a chunk never checked hides a divergence for the session |
| `ChunkSync.AHashCheckComparesCurrentChunksAtOrBelowItsOpSeqOnly` | a | a `World` + `ChunkSync`, no transport: chunks current with base < C (one equal, one corrupted), base = C, base > C (different from the server's C hash), NoBase / Awaiting / Unsynced, outside the window; only the corrupted one mismatches and becomes Requested with one `Repair` coordinate; `applied != C` skips the whole check | comparing a chunk whose data is ahead gives a false mismatch; ignoring the base rule misses the corrupted one |
| `ChunkSync.ARepairedChunkTakesNoEventAtOrBelowItsNewBase` | a | mismatch -> Requested; an op touching it is skipped; its `ChunkData` at B arrives before (and, second run, after) the next op; the chunk equals a reference at the end; with a ring of 2 the answer behind the stream is requested again with reason `Sync`, never replayed in part | a repair path that replays from the old base, or applies events to a Requested chunk, gives other voxels |
| `Client.AHashCheckSkipsAChunkWhoseDataIsAheadOfIt` (FakeServer; the two-swaps case of 7a) | a | `ChunkData` as of 2 before the batch 1..2, a `HashCheck` at 1 carrying the server's hash at 1: skipped as ahead, no request; a check at 2: equal | a check that compares regardless of the base requests a chunk that is right |
| `Client.APristineMismatchIsRepairedAndCountedOncePerChunk` (FakeServer) | a (the switch: b) | one pristine chunk this client's generator made differently: one `Repair` request, installed, counted once although checked three times before the answer came; three distinct chunks start terrain mode (`kReset | kTerrainMode` update), two do not | a repeated check counted three times would put a client with one bad chunk into terrain mode |
| `Client.EntityChecksWaitWhileTheBaselineIsOpenOrEventsAreHeld` (FakeServer) | a | checks during an open baseline, with held events, with a model request out: skipped, no `EntityListRequest`; once settled a wrong count sends exactly one | a check during a join re-sends the list at every join |
| `Client.AnEntityListResendReplacesRestoresAndDrops` (FakeServer) | a | a copy with an old model replaced, a missing one restored with its held events above B applied, an extra one with base <= Q dropped and tombstoned, one made after Q kept; the next check equal | a re-send that only adds (7a's discard rule) leaves the wrong model and the extra copy |
| `Client.TombstonesAreBoundedAndTheCheckRemovesAResurrectedCopy` (FakeServer) | a | `maxTombstones` 4, 10 despawns: 6 evicted; a stale model answer for an evicted id makes a copy; the next check mismatches; the re-send drops it | unbounded growth, or a resurrected copy that stays |
| `Session.ChecksAreSentEvery2sAfterTheJoinAndAtTheStreamPosition` | a | per peer one `HashCheck` and one `EntityCheck` per 120 ticks, staggered; none before the join's queue ran empty or during a baseline / re-send; with `maxEventBytesInFlight` holding events no check is sent; each check's C equals the opSeq the client had applied when it processed it (`checksOutOfOrder` 0 over 3000 ticks of the drill) | a check sent before its events reached the transport is compared one position early |
| `Session.HostileOrOutOfOrderSyncMessagesAreIgnoredOrEndThePeer` (extended) | a/b | a `ChunkRequest` with reason 7 ends the peer; 50 `EntityListRequest`s give one re-send (`relistMerged` 49); (b) a terrain-mode peer's update without the flag ends it | a forged request grows the server's queues |
| `Region250.FightWithIslandsAndCratesChecksFindNothing` (the M1 fight on 250 m) | a | two bots (one presenting), drill sites in the four quadrants about 80 m apart (some outside each window), 10 casts/s with islands, splits and crates for 600 ticks, 5 % loss, 80 + 0-30 ms, a 200 ms outage; the common end assertions, own positions within 5 cm after the drain, at least 4 checks of each kind compared per bot | checks compared at different stream positions report mismatches here first |
| `Region250.AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt` | a | the walker's generator changes 64 voxels of one surface chunk C 30 m east on its line (C not among the probes: asserted); it walks there at 3.8 m/s; C mismatches as pristine, is repaired, and C is current from data **before the walker's reach box first touches C** (test records both ticks); 0 held ticks; no terrain mode (1 < 3) | without `HashCheck` C is hashed equal nowhere and the end hash differs; a planner without the look-ahead finds C after the walker stood on it |
| `Region250.ACorruptedChunkAndALostCopyAreRepaired` | a | after a drill blast near bot B, the test writes stone into one chunk the blast changed on B's world (`World::FillBox`, a local edit the server never made) and destroys one of B's resting copies (`TestDestroyCopy`); within 2 checks (4 s) the chunk is repaired (a `Repair` request for that coordinate) and the copy restored (`copiesRestored` 1) | the corruption stays: end hash and entity comparison fail |
| `Region250.AnEntityCheckMismatchResendsTheListAndItAgrees` | a | bot B drops one `EntityDespawn` (an extra copy), one `ObjectModel` (a wrong model) and loses one copy (`testDropEntityEvent`, `TestDestroyCopy`): mismatches counted, re-sends 1-3, `copiesDropped`, `copiesReplaced`, `copiesRestored` each >= 1, then agreement; with `entityCheckTicks` 0 the same faults leave `CompareEntities` disagreeing (asserted, so the test can fail) | as the last column says |
| `Region250.AGeneratorBrokenEverywhereJoinsInServerTerrainMode` | b | bot B's generator is another seed: probes differ at the `Welcome`, terrain mode from the first update, the wrapper never generates a dense chunk inside the server's window (counted), B walks 200 m east; the common end assertions; bytes per join and per step printed; store under 10 240 slots; no `Unsynced` | a client that keeps its own terrain diverges at the first carve (end hash) |
| `Region250.ThreePristineChunksSwitchToServerTerrainMode` | b | B's generator differs in every surface chunk east of x = 40 m except the probes, `terrainModeOnProbeMismatch` irrelevant (probes pass); B walks east: 3 distinct pristine failures, one reset + terrain-mode update, the player held while its near data comes (held ticks printed), the end assertions | without the trigger B walks on its own terrain: held never, end hash differs |
| `Region250.LateJoinDuring10CastsPerSecondConverges` (7a's join test on 250 m, plus a terrain-mode joiner) | b | 7a's assertions (p99 < 15 ticks at 128 kB/s and 1 % loss for the playing bot and the joiner; bulk under the cap); a third bot with a broken generator joins in terrain mode during the casts and converges too; the playing bot's p99 still under 15 | terrain-mode bulk crowding the stream shows in the first bot's p99 |
| `Region250.WalkInFromOutsideIntoChunksModifiedBefore` | b | 30 craters near the east edge before the join; the walker crosses 250 m: window updates, entering modified chunks as data, 0 resets, 0 held ticks at 7 m/s, store under its cap, the end assertions | an entering modified chunk taken as pristine |
| `Region250.ChunksThatLoadLateCatchUpWhileTheWindowMoves` | b | a generator gated per column holds the walker's entering slab Loading while three casts hit it; released: ring replays > 0 (the replay path the arena walk never ran), no partial replay, the end assertions | a loading chunk keeps its pristine content |
| `Region250.ASmallStoreInTerrainModeRequestsEachChunkOnce` | b | a terrain-mode bot with all but 200 slots taken: chunks Unsynced, no request while full, each coordinate requested once when slots free (`countChunkRequests`), the end assertions | a request / StoreFull loop floods bulk |
| `Region250.BlastAtAChunkBorderBesideAWalkingPlayer` | b | stage 4's border test while the window steps under the player: at most 4 rim corrections in 40 blasts, largest error under 2 cm, no snap, checks find nothing | a window step during a blast leaves a chunk the player stands on unbased |
| `Region250.CollapseWith64AwakePiecesOutsideOneWindow` | b | stage 6's collapse with the site outside bot B's window: the awake cap reached, 16 pieces to rubble, B's copies by model requests, snapshots under 1200 B, the entity checks find nothing during the wave (skipped while events are held) | a check during the wave would re-send the list every 2 s |
| `Region250.ABlastAtTheRegionEdgeAndCeilingReachesAStraddlingWindow` | b | a bot at the east play edge (its window reaches past the server's window); blasts at the edge and at the top rows; the end assertions; the chunks beyond the server window never requested or checked | a never-edited chunk requested for good keeps `Synced()` false |
| `World.ContentInstallsPackHiddenChunksLikeLoads` | b | a buried dense chunk installed with `Content` beyond `nearRadius` is Packed (no slot), nearer it is resident and `Repack` packs it after the focus moves; `Edit` installs stay as in 7a; the hash is the same either way | a terrain-mode client fills its store with buried chunks |
| `Client.TerrainAckInstallsUniformChunksAndAwaitsTheOthers` (FakeServer) | b | codes 0 / 1 / 2 over a box: uniform air already Empty only based, uniform stone installed, data chunks Awaiting until their `ChunkData`; an ack with code 0 for an entering chunk inside the server window fails the client | a uniform chunk left as the wrapper's air is walked through |

Unchanged and still run (they do not depend on the region's size): the golden generator and integer sphere tests, event
round trips, two worlds with different load timing, the bulk-ahead carve + swap (7a; its check variant is above), the
movement tests (ping step, lost cast packets, hitch, forced misprediction, forged packets). About 26 new tests (674 ->
about 700); `Region250.*` est. 10-40 s each (ctest timeout 180 s, `RUN_SERIAL` where they measure latency).

#### `vortex_server`, `tools/session-check.sh`, the game, the docs

- **`vortex_server`.** New switches: `--bot-broken-generator N` (bot N's generator is the region's with the terrain seed
  + 1: probes differ, terrain mode at the join; the run fails unless that bot entered terrain mode and still ends with
  equal hashes and agreeing entities) and `--bot-broken-chunk N:X,Y,Z` (bot N's generator flips 64 voxels of chunk
  X,Y,Z, refused when it is a probe chunk; the run fails unless that chunk was repaired by a `HashCheck` or never
  entered bot N's window). Exit lines per bot: `Checks: bot N: H hash checks (E equal, A ahead, S skipped, O out of
  order), M mismatches (P pristine), R repairs requested / I installed; terrain mode: no | since the Welcome (P/64 probes)
  | at tick T; E entity checks (S skipped), M mismatches, L lists, restored / replaced / dropped; tombstones T (evicted
  V)` and per terrain-mode bot the `Terrain:` line above; the server's `Checks:` line with its counters. Exit 1 when a
  bot without an injected fault reports any mismatch or out-of-order check (the drill and the arena walk thereby test
  for false positives).
- **Runs that must pass (M-b):** the CLAUDE.md drill (`--max-join-p99-ticks 15`), the UDP drill, the arena walk, and
  `tools/session-check.sh`, all with 0 mismatches; **terrain-mode runs, numbers recorded**: `playground.session --drill
  30 --clients 2 --bot-broken-generator 1` with `--delay-ms 0` and `--delay-ms 40` (join bytes and time, fixed window);
  the arena walk with `--bot-broken-generator 0` (the walker: join bytes, bytes per step, held ticks, store maximum, RSS);
  `--bot-broken-chunk` on the playground (one repair end to end). A headless game client on `arena250.session` as in 7a.
- **`tools/session-check.sh`:** unchanged commands; it also fails when a client's exit line reports a hash or entity
  mismatch or an out-of-order check.
- **Game (`src/Session.cpp`).** The exit line gains the check counters and the terrain mode; a log line when terrain mode
  starts; the HUD's bottom panel shows `TERRAIN FROM SERVER` after `ONLINE ...` in terrain mode (HUD, not ImGui).
  Nothing else: the wrapper comes from `Client::MakeGenerator`.
- **Windows fixes (M-b).** `src/net/internal/Enet.cpp`: before `#include <enet/enet.h>` in the global fragment,
  `#if defined(_WIN32)` / `#ifndef NOMINMAX` `#define NOMINMAX` / `#ifndef WIN32_LEAN_AND_MEAN` `#define
  WIN32_LEAN_AND_MEAN` / `#endif` (nothing on Linux). `<optional>`: already included by every user (`Server.cpp` since
  7a); the guide's row is corrected, a grep in the review confirms it.
- **Docs.** `docs/two-pc-test-windows.md`: protocol 9 and the commits; section 1 and gate 2: a client whose golden tests
  fail (or whose probes differ) now joins in server terrain mode (logged), plays on the server's terrain with equal
  hashes, at the cost of est. 3-10 MB for the playground join (the numbers measured in M-b); gate 2 stays the release
  gate for the build itself, gate 3 passes with `server terrain mode` in the log instead of `0 probe mismatches`; the
  checklist reports the new exit-line counters; the troubleshooting row "probe chunks differ ... session will diverge"
  becomes "terrain mode: slower join, not a divergence"; 4.9's two rows marked fixed. `notes/net.md` (invariants: a
  check is processed in stream order and compares only current chunks at or below its opSeq; terrain mode is sticky and
  the server follows the flag; the re-send replaces, restores and drops; tombstones are bounded and the check is their
  backstop; traps: the 32 KiB ceiling and a terrain-mode join's length), `notes/voxel.md` (`ChunkHashAt`,
  `InstallMode::Content` and why `Edit` never packs), ARCHITECTURE.md 7.5, ROADMAP (state, test count), the game's
  CLAUDE.md (protocol 9, the switches, the exit lines, the test count), and "Stage 7b as built" here.

#### Milestones

**M-a: `HashCheck` repair and the wire `EntityCheck`** (est. 3-4 days). Protocol 9 with every message of the table
(the `TerrainAck` codec and the accepted flag bit included; the server answers a `kTerrainMode` update as malformed
until M-b); `ChunkHashAt`; `HashCheckPlanner`; server checks, recent ring, per-peer check map, repair answers and
counters; client `OnHashCheck`, repairs, the pristine-failure set (counted, the switch itself is M-b), check stats;
`EntityCheck`, `EntityListRequest`, the re-send with `kRelist` and `EntityListEnd`, the replace / restore / drop rules,
the back-off, bounded tombstones; `FieldHarness.h` with `Plain250`; the M-a rows of the table; `vortex_server`'s
`Checks:` lines and `--bot-broken-chunk`, the exit rule against false positives; the game's exit line;
`tools/session-check.sh`'s mismatch check. **Done when:** warning-free; every test green (the M-a rows and all 674
before); both headless game runs exit 0 with their usual lines; the CLAUDE.md drill, the UDP drill, the arena walk and
`tools/session-check.sh` pass with 0 mismatches; one `--bot-broken-chunk` run repairs its chunk; check costs measured;
docs for M-a ("Stage 7b as built" first half, `notes/net.md`, `notes/voxel.md`).

**M-b: server terrain mode, the 250 m test table, the Windows-build fixes** (est. 3-4 days). `SessionGenerator`,
`ClientSettings::wrapGenerator`; terrain mode at the `Welcome` and after 3 pristine chunks; the server's `TerrainAck`
and 3D bulk order for terrain-mode peers; `ChunkSync::OnTerrainAck`; `InstallMode::Content`; the terrain-mode counters
and `Terrain:` lines; `--bot-broken-generator`; the HUD word; the M-b rows of the table; the Windows fixes. **Done
when:** warning-free; every test green; both headless game runs and the four session runs as in M-a; the terrain-mode
runs above exit 0 with equal hashes and agreeing entities and their bytes per join and per window step recorded
(playground and arena), the arena walker's store under 10 240 slots; a Linux build with the `Enet.cpp` guard unchanged
in behaviour; docs complete (the Windows guide, ARCHITECTURE.md, ROADMAP, the game's CLAUDE.md, "Stage 7b as built", the
stages table row 7b).

#### Choices where the design is open

| # | Question | Choice | Why |
|---|---|---|---|
| 1 | Who picks the chunks and who compares? | The server picks and sends its hashes; the client compares. | The server knows the modified map, the recent ops and the player; the client holds the bases. A client that sent hashes up would need the server to keep hashes of past opSeqs. |
| 2 | "Compared at the same stream position" with chunks whose bases differ. | The check rides the events channel right after the events up to C and is processed in stream order (`applied == C`); only current chunks with `base <= C` are compared, chunks ahead are skipped. | No history on either side; the base rule says exactly which chunks equal the server's at C. |
| 3 | How does the server learn of mismatches? | From the repair requests (`ChunkRequest` reason `Repair`); no report message. | The request is needed anyway; a report would add a message and change nothing. |
| 4 | Which pristine chunks, when a walker meets a new slab every second? | Within 12 m of the feet or of the point 1 s ahead, server-dense first, least recently checked first, nearest the look-ahead point. | The chunk the player is about to reach is checked about 2 s before; uniform air and rock rarely differ. The movement gate holds the player if a repair is late. |
| 5 | "Fails 3 pristine hash checks": checks or chunks? | 3 distinct pristine chunks this client generated. | A chunk checked again before its repair arrives, or regenerated when it re-enters the window, is one defect, not three. |
| 6 | Bulk order for a terrain-mode peer. | 3D distance from the player (7a: horizontal). | Buried rock would otherwise come before the visible surface of the next column; the start gate is 3D already. |
| 7 | How is a received window stored without filling the store? | `InstallMode::Content` in terrain mode only: packed and repacked like a load. | Packing is never a content change; 7a's installs keep their tested behaviour. |
| 8 | `modelXor`. | Xor of `HashMix64(netId, modelHash)`. | Every crate has the same model: a plain xor cancels in pairs. |
| 9 | What does `EntityCheck` count? | Replicated objects; lights are not. | Lights come once with the baseline and never change; their re-send would need a light replace path for no gain. |
| 10 | When is an entity check skipped? | Baseline or re-send open, events held, a model request out or owed. | Those are the normal moments when the client's set is not yet the stream's; comparing then would re-send the list at every join. |
| 11 | Bounding tombstones. | A FIFO of 4096; the entity check removes what an evicted tombstone would have stopped. | The only harm of an evicted tombstone is a stale answer re-creating a dead copy, which the check sees within 2-4 s. |
| 12 | Leaving terrain mode; a client that clears the flag. | Never left; a cleared flag ends the peer. | A generator that differed once is not trusted again; the strict rule keeps server and client from disagreeing about what the ack means. |
| 13 | "The whole test table on a 250 m region". | A flat 256 m test region with the game's window and store for the unit tests; `arena250.session` (real terrain) for the terrain-mode bytes, the walk and the game client. | Flat ground loads in under a second per test; the size, the moving window and the sync paths are what the table exercises, the bytes need real terrain. |
| 14 | Uniform chunks in terrain mode. | Coded in the ack (`TerrainAck`), not sent as `ChunkData`. | About 80 % of a window's cells are uniform: as messages they would be 80 000 bulk messages (21 s at 64 installs per `Poll`). |
| 15 | Where does "stop generating" happen? | A wrapper generator that returns Empty inside the server's window in terrain mode; classification still delegated. | No World change; the ack overrides whatever the column pass placed, and equal uniform cells need no install. |

#### Risks and what 7b leaves

- **False mismatches** are the main risk: every session run and `Region250` test asserts 0, and `vortex_server` fails on
  one. A real divergence found by a check is repaired, logged and also counted, so it is never hidden.
- **Terrain-mode bytes and time**: est. 24 MB for the game's window on the arena, about 0.3 MB/s at 100 ms round trip
  under the 32 KiB bulk ceiling (est. 1-1.5 min for the whole window, a few seconds for the 32 m start set); on the
  owner's LAN est. 5-20 s. Encoding against the pristine chunk does not help here (the generator is what differs).
  Raising the ceiling for a quiet link is a later change, not 7b's.
- **The store in terrain mode**: `Visible` may call a buried chunk visible while its neighbours still wait for data, so
  it stays resident until `Repack`; the arena walker's store maximum is the gate (10 240).
- **A mid-session switch holds the player** until its near data comes (est. 1-5 s on a LAN), once.
- **`TerrainAck` size at a join** (est. 2-30 KB on the events channel) delays live events once by that much.
- **Check cost on the game client**: est. under 1 ms per 2 s, measured and logged (measured 2.0-3.7 ms at most in
  three-process runs, about 0.3 ms of it hashing - review finding 4).
- **Stage 8** (`vortex_bot`) needs nothing new: a bot is a `Client` + `Simulation` with `MakeGenerator`. **Stage 9**
  (on-demand server world): the planner picks pristine chunks among loaded cells only (players' 24 m covers 12 m), and a
  `TerrainAck` for a cell the server has not loaded loads it (`EnsureLoaded`) or codes it as data; noted for stage 9
  (**amended**, below: never generated at ack time).

#### Amendments after the critic (2026-11-17)

A critic read this plan against the 7a code: 12 findings. Where an amendment and a section above disagree, **the
amendment wins**. Every finding was judged; none was rejected whole, three were taken in part (said where).

| # | Finding | Verdict | Amendment |
|---|---|---|---|
| 1 | `InstallMode::Content` decides packing with the client's own `Visible()`, whose column classes come from the broken generator, and a pre-join crater received as data keeps `modified` false: surface chunks or craters are packed and drawn as solid blocks; buried ones may stay resident; `SetChunkEncoded` allocates before it could pack. | Real (high). | The server's geometry decides, never the client's. `TerrainAck` code 1 = data the server holds **Dense** (visible or modified): installed as 7a's `Edit` (never packed). New **code 3** = data the server holds **Packed** (hidden, unmodified): installed with `Content`, which packs it at once whatever the client's classes say, and marks the cell `serverHidden`. In terrain mode `Repack` and `Reveal` read `serverHidden` and the face-neighbour edit rule (an edit next to it reveals it, as on the server), never `Visible()` / `ClassifyColumn`; the pack decision is made before `Alloc`. Repair and `Sync` answers reuse the code the record was acked with. The wire part (code 3) lands in M-a with the codec; `World.ContentInstallsPackHiddenChunksLikeLoads` (M-b) gets a client classification that disagrees with the bit (it must not matter) and a crater chunk received as data (never packed). |
| 2 | The list re-send's "replace if base < B" rebuilds nearly every copy (rests and wakes never move `Copy::base`) from a `kRelist` entry encoded at B, while an `EntityRest` in (B, applied] was already applied to the old copy and is not held: the replacement stays awake for good. | Real (high). | **The list carries ids and models, not bodies.** `EntityListRequest` is answered by one **`EntityList{asOfOpSeq Q, count, (netId u32, modelHash u64) x n}`** (message 20, events channel, 9 + 12 n bytes, at most 16 384 entries) sent right after `Pump`, so the client processes it at `applied == Q` (the checks' stream trick). Client rules at Q: a copy whose id is not listed and whose base <= Q is dropped and tombstoned (`copiesDropped`); a listed id without a copy (tombstoned or not), or a copy whose model hash differs and whose base <= Q, is destroyed (no tombstone), put in `asked` and sent as a `ModelRequest` (7a's path: events are held from then on, the answer is encoded after the request and carries the exact rest state) (`copiesRestored` / `copiesReplaced`); copies with base > Q and ids already asked for are left alone. `kRelist`, `EntityListEnd`, the relist hold condition and the replace rule are gone: **`BaselineEntity` (13) is unchanged** in protocol 9. |
| 3 | An `EntityCheck` can fail falsely after bulk entity data ahead of the stream (a `BaselineEntity`, `BaselineEnd` or model answer encoded at B > C) was processed: a crate despawned in (C, B] has no copy. | Real (medium). | The client keeps `entityAsOf` = the largest asOf of every `BaselineEntity`, `BaselineNear` and `BaselineEnd` it processed; a check with C < `entityAsOf` is skipped and counted (`entityChecksAhead`). `Client.EntityChecksWaitWhileTheBaselineIsOpenOrEventsAreHeld` gets the case. |
| 4 | Checks start only when a peer's bulk queue first runs empty, which a terrain-mode walker on a slow link may never reach. | Real (medium). | `HashCheck`s start once the peer's first window ack went out (`Peer::hasWindow`); the planner leaves out chunks queued for the peer (`p.queued`: their data is coming). `EntityCheck`s start once its baseline ended (`!baselineOpen`, server side). |
| 5 | The pristine look-ahead (12 m around feet + 1 s of velocity) is too short for a 2 s cadence: a broken chunk on the path is checked between about 1 s before and after the player reaches it. | Real (medium); fix taken in a simpler form. | Pristine candidates lie within `hashCheckPristineMetres` (12 m) of the **segment** from the feet to feet + velocity x `hashCheckLookAheadSeconds` (**4 s**, horizontal). `hashCheckPristine` is **6**: one slot for the never-checked server-dense chunk nearest the feet, five for the segment, ranked server-dense first, never checked before least recently checked, then by distance to the segment in whole chunks, then by arrival (projection on the segment, nearest first), then (y, z, x). At walk speed (3.8 m/s) about two new on-path surface chunks enter per 2 s against five slots; at run speed the path is still covered nearest-arrival first. `Region250.AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt` runs at walk and run speed and at start offsets 0, 40 and 80 ticks. Entries per check: 4 + 8 + 6 = 18 (384 B per 2 s). |
| 6 | The flat `Plain250` region cannot run the "other seed" terrain-mode test (the flat generator has no seed) and makes packing, store and bytes trivial. | Real (medium). | M-b breaks the generator through `ClientSettings::wrapGenerator` (built in M-a for the broken-chunk tests): a flat generator at another ground height for the 250 m rows, and one GPU-free terrain-mode test on a small noise-terrain region (64 m, `packHidden` on) asserting the store maximum, that no server-Dense chunk is packed on the client and equal hashes after a short walk. `arena250.session` stays the source of the bytes. |
| 7 | `--bot-broken-chunk` conflicts with the exit hash rule: a broken pristine chunk in the window but never within 12 m of the bot is never checked. | Real (medium-low). | `--bot-broken-chunk N` breaks the pristine server-Dense chunk nearest the spawn that is not a probe (`N:X,Y,Z` names one; it is refused unless it is pristine, server-Dense, not a probe and within 12 m of the spawn), and that bot stands (no pilot moves) so the feet slot reaches it. The run fails unless that bot repaired the chunk (`repairsInstalled` >= 1 and a pristine mismatch counted); then the hash rule holds as for every bot. |
| 8 | Entity checks are skipped while any model request is out or an event is held, which in a fight with moving windows may be most of the time. | Real (low-medium). | While `asked` is not empty (baseline ended, no list open, nothing ahead) the check compares `count` and `idXor` with every asked NetId counted as present (it was alive at its `Detach` or request, and a despawn at or below C would have erased it); `modelXor` only when nothing is asked or held. Counted apart (`entityChecksPartial`, `entityChecks`); the fight test asserts both. |
| 9 | The terrain-mode rate estimate ignores the one-hand-over-per-tick limit: at most about 1.9 MiB/s (32 KiB x 60) even on a LAN. | Real (low). | Estimate corrected: at most about 1.9 MiB/s, a whole arena window (24-31 MB) in 13-17 s on a LAN, about 0.32 MB/s at a 100 ms round trip; M-b adds an arena terrain-mode walk with `--delay-ms 50` (queue length, held ticks recorded). A larger ceiling for terrain-mode peers or a smaller window is a later knob, not 7b's. |
| 10 | `OnTerrainAck` did not say what happens to entering records outside the server's window (code 0): left NoBase, the client is never `Synced()`. | Real (low). | Every NoBase record that entered with that seq and lies outside the server's window becomes `Based(A)` as in 7a's `OnAck` (never edited). Added to `Client.TerrainAckInstallsUniformChunksAndAwaitsTheOthers`. |
| 11 | The late-join test's terrain-mode joiner cannot crowd the first bot's stream (per-peer links). | Real (low). | M-b adds a shared-uplink variant (48 kB/s shared, as `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing`) with a terrain-mode joiner, asserting the first bot's p99 there; the per-peer row keeps only convergence. |
| 12 | A stage-9 server would generate a terrain-mode client's whole window at ack time. | Real (low). | Recorded for stage 9: an on-demand server codes unloaded cells from its own `ClassifyColumn` (rows below `solidBelow` and at or above `emptyFrom` as uniform, no generation) and the surface band as data, generated at dequeue under the pacer; a `TerrainAck` never generates synchronously. |

Protocol 9 after the amendments (every byte change in M-a): `WindowUpdate` 8 accepts `kTerrainMode` (the M-a server
still ends a peer that sets it); `ChunkRequest` 11 gains `reason` u8 (0 `Sync`, 1 `Repair`); **13 unchanged**; 16
`TerrainAck` with codes 0-3 (code 3 = server-Packed data); 17 `HashCheck` (count 1-32); 18 `EntityCheck`; 19
`EntityListRequest`; **20 `EntityList`** (replaces `EntityListEnd`). The tables above that name `kRelist`,
`EntityListEnd`, `relist` deques or a 4-slot pristine budget are superseded by this table.

### Stage 7b as built

Status (2026-11-17): **milestones M-a and M-b built and verified, then integrated and accepted ("Integration and
acceptance" at the end of this section), then reviewed with fresh eyes ("Review of stage 7b" below: where it differs
from the text above, the review's fixes and numbers are the current ones; protocol 10); not committed.** M-a was built by one run, M-b by the next, which first
read M-a's diff against the plan and fixed what it found ("M-a read again" under "Stage 7b M-b as built"). Built on engine
`62f28b4` (HEAD `c970f16`: a design-only commit of `worldgen.md`) / game `92e3902`. The plan and its amendments hold;
where the code differs, this section says so. A successor continues with the review of 7b (M-a and M-b together),
then stage 8.

#### M-a as built

| Part | Built | Where / tested by |
|---|---|---|
| Protocol 9 | `kProtocolVersion` 9; `MessageType` 16 `TerrainAck` (codec only: runs of `TerrainCode` 0-3, code 3 = server-Packed data, amendment 1), 17 `HashCheck`, 18 `EntityCheck`, 19 `EntityListRequest`, 20 `EntityList` (amendment 2); `ChunkRequest::reason` (`ChunkRequestReason` Sync 0 / Repair 1); `WindowUpdate::kTerrainMode` accepted by the decoder; `EntityModelTerm(netId, modelHash)`; `BaselineEntity` unchanged | `Wire.CheckAndTerrainMessagesRoundTripAtTheirSizes`, `Wire.ForgedBytesNeverCrashADecoder` (extended: the new messages; a TerrainAck that decodes always covers its box), `Wire.ChunkSyncMessagesRoundTripAtTheirSizes` (the reason byte) |
| voxel | `ChunkHashAt(world, chunk, out)`: the per-cell part of `WorldHash` (Empty, Uniform, Dense, Packed from its runs), which now calls it | `World.ChunkHashAtEqualsChunkHashInEveryCellState` |
| `vortex.net:sync` | `HashCheckSettings`, `HashCheckPlanner` (per peer: when each chunk was last checked), `HashCheckInput`, `ModifiedMap`; `ChunkSync::OnHashCheck` / `TakeRepairs`, `HashCheckOutcome`, the record's `repair` / `repaired` bits, `ChunkSyncStats` check counters | `HashCheckPlanner.*`, `ChunkSync.AHashCheckComparesCurrentChunksAtOrBelowItsOpSeqOnly`, `ChunkSync.ARepairedChunkTakesNoEventAtOrBelowItsNewBase` |
| Server | `ServerSettings::hashCheckTicks` / `entityCheckTicks` (120) / `hashCheck`; the recent-changes ring (64, fed by `EmitOp`, `Server::RecentChanged`); `SendChecks` right after each peer's `Pump` (nothing while its events are held): the list owed, then the HashCheck (once its first window ack went out, staggered 17 ticks per slot), then the EntityCheck (once its baseline ended, 60 ticks later); the entity values once per opSeq; `EntityListRequest` merged per peer; repair answers counted (`BulkItem::repair`); a `kTerrainMode` update ends the peer (M-b serves it); `Server::IsModified`; `ServerStats` check counters | `Session.ChecksAreSentEvery2sAfterTheJoinAndAtTheStreamPosition`, `Session.HostileOrOutOfOrderSyncMessagesAreIgnoredOrEndThePeer` (extended) |
| Client | the three messages and the list in the stream (`StreamItem`), processed at their position; repairs sent as `ChunkRequest{Repair}`; the pristine-failure set (counted; the switch is M-b); `entityAsOf` (amendment 3), partial checks while models are asked for (amendment 8), the list's drop / replace / restore (amendment 2), the back-off; tombstones a FIFO (`maxTombstones` 4096); `Synced()` also waits for an open list; `ClientSettings::wrapGenerator` (MakeGenerator and the Welcome's probe hashes), `testDropEntityEvent`; `Client::TestDestroyCopy`, `Tombstones()`; `net::DescribeChecks`; `ClientStats` check counters | `Client.AHashCheckSkipsAChunkWhoseDataIsAheadOfIt`, `Client.APristineMismatchIsRepairedAndCountedOncePerChunk`, `Client.EntityChecksWaitWhileTheBaselineIsOpenOrEventsAreHeld`, `Client.AnEntityListResendReplacesRestoresAndDrops`, `Client.TombstonesAreBoundedAndTheCheckRemovesAResurrectedCopy` |
| Session helper | `net::BreakChunks(generator, chunks, stone)`: 64 voxels of each listed chunk flipped (the tests' and `--bot-broken-chunk`'s broken generator) | the broken-chunk tests and run |
| Tests | `tests/net/FieldHarness.h` (the `Field` harness moved out of `ClientWindowTests.cpp`, included after the imports, includes nothing; `Plain250`, `GameClient`, presenting bots, `Settle`, per-tick `afterTick`); `tests/net/Region250Tests.cpp` (the four M-a rows) | below |
| `vortex_server` | `Checks: bot N:` and `Checks: server:` lines; the exit rule (a bot with a hash or entity mismatch or a check out of order fails the run); `--bot-broken-chunk N[:X,Y,Z]` (amendment 7: that bot stands; the run fails unless its chunk was found and repaired) | the runs below |
| Game | `Checks: slot N: ...` at exit (`src/Session.cpp`) | `tools/session-check.sh` now fails on a mismatch, a check out of order, or no HashCheck compared |

Where the code chose something the plan left open or got wrong:

| Question | Choice | Why |
|---|---|---|
| Pristine slots (amendment 5's 6) | **12**: two for the never-checked server-dense chunks nearest the feet (a standing player fills the rest from there), ten for the way ahead; the way-ahead ranking puts chunks whose box lies within `pathMetres` (1.5 m, the reach box's half-width) of the way first, by where along the way their box starts, and leaves out chunks wholly behind the feet. 24 entries at most per check: 510 B per 2 s. | `Region250.AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt` failed with 6 slots and the plan's distance buckets: a walker on a chunk-row border has two rows on its path, chunks behind the feet ranked as "arriving first", and at run speed (7 m/s) the broken chunk was checked after the walker reached it (twice) or never (once, the end hash differed). With 12 and the path rank: repaired 0.40-3.82 s before the reach box touched it in all six runs. |
| When the client counts "out of order" | `ClientStats::checksOutOfOrder` counts HashChecks, EntityChecks and EntityLists not processed at their opSeq (the HashCheck ones also in `ChunkSyncStats`). | One number for the exit rules. Never seen: 0 in every test and run. |
| A repeated repair | Counted (`ChunkSyncStats::repeatRepairs`) when a chunk repaired while in the window mismatches again, without the plan's 10 s bound. **Superseded by the review (finding 8):** a repeat within 5 checks of the repair, or after 3 repairs, is not asked for again. | Simpler; any repeat is a bug to look at. |
| The list's model requests | The client's 7a path (`asked`, `ModelRequest`, held events, answers encoded after the request). A tombstoned NetId the list names is un-tombstoned (the server says it is alive at Q). | Amendment 2. |
| The check on a client that never finished its join | HashChecks compare from the first ack on (records not current are skipped); EntityChecks are skipped until `BaselineEnd` (client) and not sent before the peer's baseline ended (server). | Amendment 4. |

Verified (this laptop, dev build -O2 -g, 2026-11-17): **warning-free; 689 tests (six disabled), all green** (674
before: 15 new; `Movement.NoCorrectionAbove2cmOnFlatGround`'s sanity bound on compared snapshots lowered from 800 to 750:
its link draws loss from one random stream that the checks' messages now share, so B compared 789 instead of 821 - 0
corrections either way, A 809 both times). Three of the new tests were checked against faults (one build with the
"data ahead" rule of `OnHashCheck` and the `entityAsOf` rule off): `ChunkSync.AHashCheckComparesCurrentChunksAtOrBelowItsOpSeqOnly`,
`Client.AHashCheckSkipsAChunkWhoseDataIsAheadOfIt` and `Client.EntityChecksWaitWhileTheBaselineIsOpenOrEventsAreHeld`
fail there; the planner test and the broken-chunk test failed on the first planner (above). Both headless game runs
exit 0 under validation (`Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, feet (17.15, 4.36, -2.99)
on the ground; screenshot looked at); the editor run exits 0.

| Run | Result |
|---|---|
| The CLAUDE.md drill (`--max-join-p99-ticks 15`) | exit 0, hashes equal, 67/67, 67/67, 66/66, entities agree (8 objects); bot 0 12 HashChecks (288 entries equal) + 13 EntityChecks, bot 1 (joined at step 100) 8 + 8, **0 mismatches, 0 out of order**; server 20 HashChecks, checks at most 1.5 ms per peer and tick; join latency p99 11 / 9 ticks |
| The UDP drill (`--enet --clients 3 --join-step 50 --bot-casts 90 --bot-crates 120`) | exit 0, hashes equal, entities agree (9 objects); 13 / 10 / 10 HashChecks, 13 / 10 / 11 EntityChecks (one skipped while a joiner's baseline was open, one partial), 0 mismatches |
| **Arena walk** (the CLAUDE.md command) | exit 0, both bots' hashes equal the server's over their sync boxes, entities agree (74 objects); walker 147 HashChecks (2893 entries equal, 47 skipped as not current: chunks still loading on its way), the other bot 146 (3001 equal, 3 outside the server's window); 145 + 2 and 145 + 1 EntityChecks; **0 mismatches**; server 293 HashChecks (84 recent, 2344 modified, 3516 pristine entries) and 293 EntityChecks, at most **4.15 ms** for one peer's checks in one tick (24 chunk hashes, the planner over the modified map, the values of 74 objects), 407 ms over the run; bots at most 1.8 / 2.4 ms per check. Store at most 5557 / 5455 of 12 288; RSS 1111 MiB |
| `tools/session-check.sh` | **PASS** (hashes and entity lists equal at the same opSeq); the clients' `Checks:` lines: 17 and 15 HashChecks, 0 mismatches, 0 out of order (20 and 24 entries skipped as not current: the join's chunks still on their way during the game's 3 s `Attach`) |
| `--bot-broken-chunk 1` on the playground (`--drill 30 --clients 2 --loss 0.05 --delay-ms 40 --jitter-ms 30`) | exit 0, hashes equal: bot 1's generator broke chunk (2, 1, 2) 4 m from the spawn; its first HashChecks found it (`pristine, generated here`), one Repair request, one answer, installed; bot 0 0 mismatches |

**Not done in M-a (open):** the review of M-a (CLAUDE.md step 4); a server-side check costs up to 4 ms in one tick on
the arena for one peer (the planner walks the whole modified map and the per-opSeq entity values hash every model;
the stagger (17 ticks per slot, entity checks 60 later) gives every peer of up to 60 its own ticks, so this is per
tick, not per player count; a cached model hash per object and a planner that keeps its modified candidates would cut
it - not measured, a later optimisation); a broken chunk off every player's path is
never checked (by design: the checks follow the players); M-b.

| Test | Result |
|---|---|
| `Region250.FightWithIslandsAndCratesChecksFindNothing` (two bots on Plain250, one wandering with crates and presenting, one walking 90 m east; 100 drill blasts on four sites 87 m apart, 10 casts/s; 5 % loss, 80 + 0-30 ms, a 200 ms outage) | equal hashes over both sync boxes, entities agree, own positions within 5 cm; bot A 8 HashChecks (144 entries equal) + 8 EntityChecks, bot B 7 HashChecks (126 equal) + 7 full and 1 partial EntityChecks, **0 mismatches, 0 out of order**; server 16 HashChecks (64 recent, 128 modified, 96 pristine entries), 100 terrain pieces as server bodies, 111 crates, B made copies by model requests; checks at most 1.3 ms per peer and tick (server), 0.58 ms (client) |
| `Region250.AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt` (walk 3.8 m/s and run 7 m/s, start offsets 0 / 40 / 80 ticks, 5 % loss, 40 + 0-30 ms) | the chunk repaired 3.13 / 3.82 / 2.42 s (walk) and 1.90 / 0.40 / 1.22 s (run) before the walker's reach box touched it; one pristine mismatch, one pristine failure, 0 held ticks, equal hash |
| `Region250.ACorruptedChunkAndALostCopyAreRepaired` | the gold written into the last changed chunk and the destroyed copy were both repaired after 100 passes (one modified mismatch, one list, one copy restored); equal hashes, entities agree |
| `Region250.AnEntityCheckMismatchResendsTheListAndItAgrees` | 2 mismatches, 2 lists: 1 restored, 1 replaced, 1 dropped, then agreement; with `entityCheckTicks` 0 the faults stay (the comparison disagrees) |
| `Session.ChecksAreSentEvery2sAfterTheJoinAndAtTheStreamPosition` | 3000 drill ticks, a joiner at 1000: 24 + 16 HashChecks, 25 + 17 EntityChecks, all at their position; with events held by the hook (595 batches held) 15 HashChecks and 14 EntityChecks, all at their position |

#### What M-b starts from

- `TerrainAck` / `TerrainCode` (0-3) exist as a codec; the client fails on one (it never asks), the server ends a peer
  that sets `kTerrainMode`. M-b: `SessionGenerator` (wrapping `ClientSettings::wrapGenerator`, which exists), the
  server's ack, `ChunkSync::OnTerrainAck` (amendment 10), `InstallMode::Content` with `serverHidden` (amendment 1).
- The pristine-failure set is `Client::Impl::pristineFailures` (`ClientStats::pristineFailures`): the switch at 3.
- `FieldHarness.h` holds `Plain250` and `GameClient`; the M-b rows go into `Region250Tests.cpp`.
- No wire byte is left for M-b (if M-a is committed alone and M-b needs one, protocol 10).

#### Stage 7b M-b as built

**M-a read again first.** The M-b run read M-a's whole diff (wire, `ChunkSync::OnHashCheck`, the planner, the server's
`SendChecks`, the client's checks, list and tombstones, `vortex_server`, the game, the tests) against the plan and its
amendments before building on it. It held: checks ride the events channel after `Pump` and are compared only at their
opSeq; a mismatch is 7a's `Requested` state; the list asks for bodies instead of re-making them. One fault was fixed:

| Found | Fix |
|---|---|
| A NetId the client had asked for (`asked`) that the `EntityList` does not hold stayed asked for ever: the server answers nothing for a gone NetId, so `Synced()` never came back (reachable when the despawn was lost - the tests' `testDropEntityEvent` - or any future bug that loses one). Every ask comes from an event at or below the list's Q, and a despawn processed since would have cleared it, so such an id died at or before Q. | `OnEntityList` forgets it (asked, owed, held events dropped) and tombstones it, counted in `copiesDropped` (`Client.AnAskedNetIdTheListDoesNotHoldIsForgotten`, which fails on the old code). |

| Part | Built | Where / tested by |
|---|---|---|
| voxel | `InstallMode` on `World::SetChunkEncoded` (`Edit` = 7a's; `Content` = server-Dense data: shown, never packed, not an edit - the cell keeps its modified bit, so buried neighbours stay packed; `HiddenContent` = server-Packed data: installed Packed with the message's bytes, no slot, decided before any allocation; a `Content` install over a world that packs nothing or over an edited cell). The cell's `serverVisibility` (1 shown, 2 hidden) answers `Visible()` before the column rule: a hidden cell is visible only next to an edited face neighbour (as on the server), the client's own column classes are never asked (amendment 1). `World::ServerVisibility` (tests) | `World.ContentInstallsPackHiddenChunksLikeLoads` (a client world whose flat classes call nearly every row visible: every server-Packed chunk Packed with no slot, re-packed after being made resident, a crater chunk received as data and its neighbours stay resident, an edit next to a hidden chunk reveals it, 7a's `Edit` unchanged, equal hashes). Checked against a fault: with the hidden branch of `Visible()` off it fails (nothing re-packed). |
| `vortex.net:session` | `SessionGenerator`: the region's generator (with `ClientSettings::wrapGenerator`) until terrain mode, then Empty at once for every chunk inside the server's window (counted: `Skipped`, `DenseInsideInTerrainMode`); column classes and vertical bounds delegated | every terrain-mode test asserts no dense chunk of the server's window was generated |
| `vortex.net:sync` | `ChunkSync::OnTerrainAck` (codes per entering record: 2 = based at A, installed as `Content` first unless the cell already holds exactly that uniform content; 1 / 3 = `Awaiting`, 3 remembered as hidden with the ack's opSeq; 0 inside the server's window = malformed; outside it = based as in 7a, amendment 10; every chunk it bases counts as from data); `SetTerrainMode`; installs in terrain mode use `HiddenContent` for a hidden record only when no ring operation in (ack, data] touched the chunk or a neighbour (`OpRing::TouchesNear`, ranges only; unknown = touched), else `Content`; repairs and re-requests are `Content`. `TakeRequests` counts the records whose content is still to come (NoBase, Awaiting, Requested: `m_Incoming`) against the free slots | `Client.TerrainAckInstallsUniformChunksAndAwaitsTheOthers`, `Region250.ASmallStoreInTerrainModeRequestsEachChunkOnce` |
| Client | `ClientSettings::terrainModeOnProbeMismatch` (true), `pristineFailuresForTerrainMode` (3); `TerrainModeReason`; terrain mode at the `Welcome` (the first `WindowUpdate` carries `kTerrainMode`; `Attach` skips its probe check) or after the third distinct pristine failure (a `{kReset | kTerrainMode}` update over the current window, sent from the HashCheck's place in the stream); every later update carries the flag; `TerrainAck` in the stream (`StreamItem`), a malformed one fails the client, one without terrain mode too; `TerrainMode()`, `TerrainModeWhy()`, `ClientGenerator()`, `MakeGenerator()` returns the session's `SessionGenerator`; stats `terrainAcks` / bytes, `terrainChunkData` / bytes, `terrainModeTick`; `net::DescribeTerrainMode` | `Client.APristineMismatchIsRepairedAndCountedOncePerChunk` (two failures: no switch; the third: the reset update, the ack and data, the server's world), the Region250 rows |
| Server | `Peer::terrainMode` (sticky: an update without the flag after one with it ends the peer); `OnTerrainWindow`: the entering cells inside the server's window coded by `StateOf` (Empty / Uniform: code 2; Dense: 1; Packed: 3, both queued), code 0 elsewhere, runs in `VoxelMask` order, sent after `Pump` like a `WindowAck`; a terrain-mode peer's bulk queue in 3D distance from its eyes; per peer the bytes of its first terrain update and of each later step (`TerrainCost(slot)`, `TerrainPeerCost`); `ServerStats` terrain counters and the acks' build time | `Session.HostileOrOutOfOrderSyncMessagesAreIgnoredOrEndThePeer` (extended: a terrain update gets a `TerrainAck` describing every chunk of the server's window, a later update without the flag ends the peer) |
| Windows | `Enet.cpp`: `NOMINMAX` and `WIN32_LEAN_AND_MEAN` defined under `#if defined(_WIN32)` before `<enet/enet.h>` (nothing on Linux). `<optional>`: every user already includes it (grep: `Messages.cppm`, `Server.cppm`, `Client.cppm`, `Messages.cpp`, `Server.cpp`, `Client.cpp`, `tools/server/main.cpp`) | the Linux build (unchanged); not compiled on Windows |
| `vortex_server` | `--bot-broken-generator N` (bot N's generator: the region's with terrain seed + 1, a flat region's ground 12 voxels higher; the run fails unless it is in terrain mode and ends with equal hashes and agreeing objects); `Terrain: bot N:` per terrain-mode bot and `Terrain: server:`; a terrain-mode bot's probe mismatches do not fail the run; the drain waits for a terrain-mode bot that is not synced (up to 3 min of wall clock) | the runs below |
| Game | `Session: server terrain mode (...)` logged when it starts; `Terrain: slot N: ...` at exit; the HUD's bottom panel adds `TERRAIN FROM SERVER` to the `ONLINE` / `JOINING` line (HUD, not ImGui); the test switch `--other-terrain` (with `--connect`: this client's generator is the region's with terrain seed + 1 / ground 12 voxels higher, as `--bot-broken-generator`) | headless game clients on the playground and the arena (runs below; the HUD word seen on both screenshots) |

Choices M-b made where the plan was open or wrong:

| Question | Choice | Why |
|---|---|---|
| What a `Content` install does to `modified` | Keeps the cell's bit (false for a fresh install). | The plan's amendment 1 said code 1 is installed as 7a's `Edit`. An `Edit` marks the cell modified, which reveals its face neighbours: every buried chunk under a surface chunk would be resident on a terrain-mode client and on no other (est. thousands of slots on the arena). |
| A hidden chunk whose data comes after an operation near it | Shown (`Content`), decided from the ring's ranges (the chunk or a face neighbour in an op's chunk range between the ack and the data; data ahead of the stream, or a ring that does not cover the ack: shown). **Superseded by the review (finding 1):** packed only if no operation over it or a face neighbour came between its ack and the stream's position (`max(asOf, applied)`), no face neighbour is an edit here (`World::SetChunkEncoded`), and no operation this world skips next to it later (`World::ShowServerHiddenNeighbours`). | The server may have revealed it (an edit of it or of a neighbour); a packed chunk is drawn as solid rock. Conservative: at worst a few chunks resident that the server packs. |
| Free slots for requests of Unsynced chunks | Free slots minus the records whose content is still to come (NoBase, Awaiting, Requested; since the review, finding 6, not hidden data a packing world installs packed). | Found by `Region250.ASmallStoreInTerrainModeRequestsEachChunkOnce`: a window step frees the leaving slab's slots before the entering slab's data comes; requests took them, the data then found none - a request / StoreFull loop (a chunk asked 5 times; 128 requests "while full"). A 7a client meets it too (its entering chunks are generated into slots); the 7a full-store test is unchanged. |
| The per-step cost | Bytes per window update (its ack and the data it queued, counted as sent), on the server. | The client cannot tell which update a ChunkData belongs to; the server knows what each ack queued. |
| The latency of the terrain-mode joiner | Asserted under 15 ticks on 128 kB/s per-peer links; on a shared server uplink the playing bot's p99 is compared with the same run's join by a client with its own terrain (bound: + 6 ticks). | A 48 kB/s shared uplink with a drill, two clients' snapshots and a terrain-mode join was measured first against a run without any joiner: 13 -> 75 ticks (the second client's snapshots alone fill much of that link); the comparison with an ordinary joiner isolates what terrain mode adds. |

Tests added in M-b (13; **702 in all, six disabled**): `World.ContentInstallsPackHiddenChunksLikeLoads`, `Client.AnAskedNetIdTheListDoesNotHoldIsForgotten` (the M-a fault above; fails without the fix),
`Client.TerrainAckInstallsUniformChunksAndAwaitsTheOthers`, `TerrainMode.ANoiseRegionPacksAsTheServerDoes` (amendment 6) and
nine `Region250.*` rows; `Client.APristineMismatchIsRepairedAndCountedOncePerChunk` and the hostile-messages test
extended. The whole table on the 250 m region (`Plain250`: 64 x 64 chunks of flat play at 0.125 m, bots with the game's
48-chunk moving window and store), every row ending with each bot synced at the server's last opSeq, its hash over its
sync box equal to the server's, its objects agreeing, and - where no fault was injected - no hash or entity mismatch
and no check out of order:

| Test (`Region250.*` unless named) | Result (this laptop, dev build) |
|---|---|
| `FightWithIslandsAndCratesChecksFindNothing` (M-a) | 8 / 7 HashChecks, 8 / 7 + 1 partial EntityChecks, 0 mismatches; 100 pieces as server bodies, 111 crates |
| `AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt` (M-a) | repaired 3.13 / 3.82 / 2.42 s (walk) and 1.90 / 0.40 / 1.22 s (run) before the walker's reach box touched it |
| `ACorruptedChunkAndALostCopyAreRepaired`, `AnEntityCheckMismatchResendsTheListAndItAgrees` (M-a) | repaired within 100 passes; 2 lists: 1 restored, 1 replaced, 1 dropped |
| `AGeneratorBrokenEverywhereJoinsInServerTerrainMode` (ground 1.5 m higher everywhere: 64 of 64 probes differ) | terrain mode from the `Welcome`; 2729 chunks answered Empty, 0 dense generated inside the server's window; B runs 118 m east past 14 craters: 15 acks / 11.6 KiB, join 2304 ChunkData / 2.3 MiB (flat ground: one dense row), 14 steps, 58 KiB per step on average, 149 KiB at most; 0 held ticks, store at most 2304 slots, equal hashes, 0 mismatches |
| `ThreePristineChunksSwitchToServerTerrainMode` (every surface chunk east of 40 m near the line broken, no probe) | the switch at tick 480 after 9 distinct pristine failures (one check held several), 1 reset, 20 held ticks, equal hash |
| `LateJoinDuring10CastsPerSecondConverges` (drill at 10 casts/s; B (own terrain) and C (terrain mode) join at step 40) | 128 kB/s per peer, 1 % loss, bulk capped at 32 kB/s: p99 A 8 / B 8 / C 8 ticks, all converge (C's join 3.4 MiB, not complete within the 600 measured ticks at 32 kB/s); one shared 96 kB/s uplink: A's p99 9 ticks while B joins, 14 while C joins in terrain mode (bound + 6) |
| `WalkInFromOutsideIntoChunksModifiedBefore` (30 craters near the east edge after the walker reached the west edge; 230 m east at run speed) | 43 window updates, 0 resets, 0 held ticks, store at most 2304 |
| `ChunksThatLoadLateCatchUpWhileTheWindowMoves` (generation of the entering slab held while three craters hit it) | 4 chunk skips, 4 replays after the release, 0 not covered (the ring replay on a moving window, which the arena walk never ran) |
| `ASmallStoreInTerrainModeRequestsEachChunkOnce` (terrain mode, store taken but for 4 slots, 18 gold boxes on the way) | 14 chunks Unsynced, 0 requests while full, each requested once after the slots freed. Failed before the incoming-slots rule (above). |
| `BlastAtAChunkBorderBesideAWalkingPlayer` (stage 4's test while the window steps: 10 updates during) | 0 corrections beside, 0 in the 40 rim blasts, largest error 1.3 cm, no snap, never held |
| `CollapseWith64AwakePiecesOutsideOneWindow` (80 stems cut 60 m east; B 60 m west) | 64 awake at most, 16 over the cap, largest snapshot 1186 B, B's 64 copies by model requests; 0 entity mismatches (one partial check during the wave) |
| `ABlastAtTheRegionEdgeAndCeilingReachesAStraddlingWindow` (two bots at the east play edge, one in terrain mode; blasts at the edge and in a tower to the ceiling) | no request or check beyond the server's window; chunks there based (code 0) and not from data; equal hashes |
| `TerrainMode.ANoiseRegionPacksAsTheServerDoes` (64 m noise terrain, the server packs; a client with another terrain seed, a 16-chunk window that packs) | 2227 ChunkData installed packed, 716 shown; in the sync box the server holds 400 dense / 1424 packed, the client 687 / 1137, no chunk the server shows packed here; store at most 811 slots. Checked against a fault: with every terrain-mode ChunkData installed as `HiddenContent` (code 1 as code 3) it fails (chunks the server shows packed here). |

Fixed after the table above was written: `net::CompareEntities` measured the angle between two rest rotations as
`2 acos(float dot)`, which reads rotations equal to float precision as 0.7-1.4 mrad (acos is ill-conditioned near 1):
the first terrain-mode arena walk failed the tool's 1 mrad tolerance on 74 bodies resting "within 0.0000 m / 0.00138
rad" on both bots. It is now `2 atan2(|v|, |w|)` of the relative rotation in double; the same run then reads 0.00000 rad.

Verified (this laptop, dev build -O2 -g, on the final binaries): **warning-free; 702 tests (six disabled), all green**.
Both headless game runs exit 0 under validation (`Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, feet
(17.15, 4.36, -2.98) on the ground; screenshot looked at); the editor run exits 0.

| Run | Result |
|---|---|
| The CLAUDE.md drill (`--max-join-p99-ticks 15`) | exit 0, hashes equal, 67/67, 67/67, 66/66, entities agree (8 objects); 12 + 8 HashChecks, 0 mismatches; latency during the joins p99 11 / 9 ticks |
| The arena walk (the CLAUDE.md command, 38 s wall) | exit 0, both sync-box hashes equal, entities agree (74 objects); 145 / 144 HashChecks, 0 mismatches; store at most 5501 / 5449; RSS 1109 MiB; server checks at most 5.0 ms for one peer in one tick |
| `tools/session-check.sh` | **PASS** (both clients' hashes and entity lists equal the server's at the same opSeq; 17 / 16 HashChecks, 0 mismatches, 0 out of order). Screenshot looked at |
| `--bot-broken-chunk 1` on the playground | exit 0, hashes equal; chunk (2, 1, 2) found (1 pristine mismatch), repaired |
| **Terrain mode, playground** (`--drill 30 --clients 2 --bot-broken-generator 1 --loss 0.05 --jitter-ms 30`, `--delay-ms 0` and `40`) | exit 0 both, hashes equal, entities agree (61 objects). Bot 1: 60 of 64 probes differ, terrain mode from the `Welcome`; one ack of 2.9 KiB (31 603 uniform chunks in it, 2286 data, 5711 packed data); **join 7997 ChunkData / 16.2 MiB** (the design said 3-10 MB: the playground's buried chunks have caves, about 2 KiB of RLE each); started 6.3 / 15.8 s of server ticks after its `Welcome` (4.1 / 4.5 s wall), every chunk in after 26 s of server ticks at 0 ms (4.7 s wall; at 40 ms only during the drain, about 60 s of server ticks: the bulk pacer's 32 KiB window per round trip); 0 mismatches (its chunks are all the server's) |
| **Terrain mode, arena walk** (the arena walk with `--bot-broken-generator 0`, 40 s wall) | exit 0, both sync-box hashes equal, entities agree (74 objects). The walker: 61 of 64 probes differ; 60 acks / 409 KiB; join ack 6.6 KiB + 20 153 ChunkData queued (9.0 MiB of them sent before the window moved on), **59 window steps, ack + data 804 KiB per step on average, 1.9 MiB at most**, 27 182 ChunkData / 55 MiB over the 240 m walk (19 801 installed packed, 7381 shown); 0 dense chunks generated inside the server's window; **0 ticks held**; started 15.4 s of server ticks (8.5 s wall) after its `Welcome`, every chunk current 221 s of server ticks (20.7 s wall) after it - after the walk; **store at most 5513 slots** (gate 10 240); RSS 1102 MiB; event latency p99 10 ticks |
| **Terrain mode, game client on the playground** (`Shredborne --connect ... --other-terrain --headless --validation --frames 1500`, the server with `--drill 20`) | exit 0 both; `TERRAIN FROM SERVER` on the HUD (screenshot looked at: the plaza, its crates and walls, the server's terrain); 7931 ChunkData / 16.3 MiB, 5813 installed packed; started 7.4 s, every chunk in 15.2 s (wall) after its `Welcome`; store at most 2145 slots; its world hash equals the server's at opSeq 374, entity lists equal |
| **Terrain mode, game client on the arena** (`arena250.session --precasts 40 --drill 20`, `--other-terrain --frames 1500`) | exit 0 both; **the game's whole 48 x 44 x 48 window: 20 153 ChunkData / 40.0 MiB** (14 910 installed packed, 5243 shown; the design's estimate was 8-25 MB), one ack of 6.6 KiB; started 6.0 s, every chunk in 27.7 s (wall, localhost) after its `Welcome`; store at most 5248 slots; window hash equal to the server's over [-24,-12,-24..24,32,24) at opSeq 370; screenshot looked at (a precast crater, trees, no packed-looking block) |

What the numbers say: a terrain-mode client plays on the server's world with equal hashes and the store of a normal
client; it pays in bytes - 16 MiB to join the playground, 40 MiB for the game's window on the arena and 0.8 MiB per
two-chunk window step while walking there - and in time: through the bulk pacer's 32 KiB window, about 1.9 MiB/s on
a LAN at best (half a minute to fill the arena window on localhost) and about 0.3 MB/s at a 100 ms round trip. The start
gate waits only for the 3D-nearest data (6-16 s in these runs). A larger bulk window for a terrain-mode peer on a quiet
link, and encoding chunks against what the client already holds, are the obvious next steps; neither is 7b's.

**Not done (7b), as M-b left it:** the review of 7b with fresh eyes (M-a and M-b together; CLAUDE.md step 4); the
Windows build itself (the `Enet.cpp` guard is compiled on Linux only) and the two-machine run (the owner's); windowed
play in terrain mode; splitting large bulk messages; a larger bulk ceiling for terrain-mode peers; ~~the `Join:` line's
"synced after" of a bot that syncs in the drain's last 16 Polls reads 0.00 s~~ and ~~a server-side check costs up to
5-6 ms for one peer in one tick on the arena~~ (both fixed by the integration below); a pristine chunk off every
player's path is never checked (by design).

#### Integration and acceptance (2026-11-17)

A further run read the whole 7b diff (World's install modes, `ChunkHashAt`, the wire, `ChunkSync::OnHashCheck` /
`OnTerrainAck` / the incoming-slots rule, the planner, the server's checks, list and terrain acks, the client's
checks, list, tombstones and terrain-mode switch, `SessionGenerator`, `vortex_server`, the game's `Session.cpp`, the
tests) against the plan, its amendments and the two "as built" parts above. The base rule held on every path the
checks add: a mismatch only makes a current chunk `Requested` (7a's request, install and replay do the rest); a
`TerrainAck` bases a record only at its own opSeq and only for records that entered with its update; a terrain-mode
install never bypasses `Install`'s stale test. Packing stayed no content change (`HiddenContent` keeps the message's
canonical bytes, hashed from its runs). Nothing was found broken; three things were half done:

| Found | Fix |
|---|---|
| A server check cost up to 4.15-5.0 ms for one peer in one tick on the arena walk (M-a's open point). Measured with timers in place for one run: the planner 0.2-0.6 ms, the 20-24 chunk hashes 0.7-2.8 ms (`ChunkHash` read a Dense chunk voxel by voxel), the entity values 1.1 ms (`VolumeHash` of every model at every opSeq). | `voxel::ChunkHash` skips eight voxels at a time while a run goes on (the same runs, so the same hash: the goldens, the pinned hash and the new `ChunkHash.EqualsTheVoxelByVoxelRunHash` - random runs of every length against a voxel-by-voxel reference - are unchanged and green); the server keeps each object's model hash with a weak pointer to the model it was taken of (`modelHashes`: a model is never changed in place, `ObjectModel` puts a new one; a new model at a freed address cannot pass as the old). Arena walk: **1.35 ms at most per peer and tick, 213 ms over the run** (4.15 ms / 407 ms before; one run - across runs 0.41-3.87 ms at most per peer and tick, see "Review of stage 7b" finding 4); the bots' checks 68-70 ms over the run (175 before; their largest single check, 0.9-3.0 ms, is wall clock in a one-process tool whose worker threads generate terrain). |
| The `Join:` lines' "synced after" read 0.00 s for a bot that synced in the drain's last Polls (sampled every 16th `Poll`). | `Client::Synced()` notes the first time it answers true (`NoteSynced`, a log value only). |
| `BreakChunks`' doc comment stood above `SessionGenerator`. | Moved. |

**Measured (this laptop, dev build -O2 -g, all on the final binaries).** Warning-free build; **703 tests (six
disabled), all green** (one new: `ChunkHash.EqualsTheVoxelByVoxelRunHash`). The standard headless game run (`Beam cut:
13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, feet (17.15, 4.36, -2.99) on the ground; screenshot looked at),
the editor run, `--climb` (0 unloads inside the view radius, window y fixed, 0 unfilled columns), `--tower-blast
--after-fill --frames 700` (freed by shot 6 of 7 by the load check, 11.27x) and `--neck-blast --frames 200` (0 pieces
released by the duds; the tower released, 5.61x) exit 0 under validation.

| Run | Result |
|---|---|
| The CLAUDE.md drill (`--max-join-p99-ticks 15`) | exit 0, hashes equal, 67/67, 67/67, 66/66, entities agree (8 objects); bot 0 12 HashChecks (288 entries equal) + 13 EntityChecks, bot 1 (joined at step 100) 8 + 8, **0 mismatches, 0 out of order**; server 20 HashChecks, 0.39 ms at most per peer and tick; the joiner 65.7 KiB of bulk, synced 0.72 s (0.56 s wall) after its `Welcome`; latency during the joins p99 11 / 9 ticks |
| The drill with `--clients 3 --join-step 50 --max-join-p99-ticks 15` | exit 0, hashes equal, entities agree (8 objects); 13 / 10 / 10 HashChecks, 0 mismatches; latency during the joins p99 11 / 11 / 4 ticks |
| The UDP drill (`--enet --clients 3 --join-step 50 --bot-casts 90 --bot-crates 120`) | exit 0, hashes equal, entities agree (8 objects); 13 / 10 / 10 HashChecks, 0 mismatches; corrections 3 / 3 / 6; latency during the joins p99 12 / 17 / 49 ticks (the one-thread tool's stalls, as before) |
| `tools/session-check.sh` | **PASS** (world and entity list hashes equal the server's at the same opSeq); 17 / 16 HashChecks, 0 mismatches, 0 out of order (20 / 24 entries not current: the join's chunks during the game's 3 s `Attach`). Screenshots looked at: the plaza with crates and broken crates, a crater, b's HUD `players 2` |
| **Arena walk** (the CLAUDE.md command, 28 s wall) | exit 0, both bots' hashes equal the server's over their sync boxes, entities agree (74 objects), drill 40/40; walker x -118.1 to +122.0 m, 60 window updates, 0 resets, 0 ticks held; 175 / 174 HashChecks (3455 / 3552 entries equal), **0 mismatches**; store at most 5500 / 5450 of 12 288; RSS 1118 MiB; latency during the joins p99 11 / 10 ticks |
| `--bot-broken-chunk 1` on the playground (`--drill 30 --clients 2 --loss 0.05 --delay-ms 40 --jitter-ms 30`) | exit 0, hashes equal; chunk (2, 1, 2) 4.0 m from the spawn found by a HashCheck at opSeq 429 (`pristine, generated here`), one Repair request, installed |
| **Terrain mode, playground** (`--drill 30 --clients 2 --bot-broken-generator 1 --loss 0.05 --jitter-ms 30`, `--delay-ms 0` / `40`, 12 s wall each) | exit 0 both, hashes equal, entities agree (61 objects), 0 mismatches. Bot 1: 60 of 64 probes differ, terrain mode from the `Welcome`; one ack of 2.9 KiB (31 604 uniform chunks); **join 7996 / 7997 ChunkData = 16.3 / 16.2 MiB**; started 6.5 / 15.8 s of server ticks (4.0 / 4.2 s wall), every chunk in after 26 / 66 s of server ticks (4.6 / 5.0 s wall); 0 ticks held; store at most 8030 slots (a bot's fixed window packs nothing, so its hidden chunks are resident; the game's packs, below) |
| **Terrain mode, arena walk** (the arena walk with `--bot-broken-generator 0`, 27 s wall) | exit 0, both sync-box hashes equal, entities agree (74 objects), 0 mismatches. The walker: 61 of 64 probes differ; 60 acks / 409 KiB; join ack 6.6 KiB + 20 153 ChunkData queued (9.0 MiB of them sent before the window moved on); **59 window steps, ack + data 804 KiB per step on average, 1.9 MiB at most**; 27 192 ChunkData / 54.9 MiB over the walk (19 840 installed packed, 7352 shown); 0 dense chunks generated inside the server's window; **0 ticks held**; started 16.3 s of server ticks (6.4 s wall) after its `Welcome`; **store at most 5513 slots** (gate 10 240); RSS 1119 MiB; event latency p99 10 ticks |
| The same at `--delay-ms 50` (amendment 9: a 100 ms round trip) | exit 0, hashes equal; 26 254 ChunkData / 53.3 MiB, 789 KiB per step on average; started 17.6 s of server ticks (6.6 s wall); **0 ticks held, 0 corrections**; the other bot's latency p99 4 ticks, the walker's 13 |
| A headless game client on `arena250.session` (own terrain; the command of 7a's "Integration and acceptance") | exit 0 both; started 2.11 s, data in 1.63 s, synced 7.23 s (wall) after its `Welcome`; 331 KiB of bulk (93 `ChunkData`); 16 HashChecks, 0 mismatches; client hash `1dee179d7a9e50a1` = the server's `window hash` over [-24,-12,-24..24,32,24) at opSeq 377; the server alone RSS 518 MiB; screenshot looked at (a precast crater at the spawn, terrain without gaps) |
| **Terrain mode, game client on the playground** (`Shredborne --connect ... --other-terrain --headless --validation --frames 1500`, server `--drill 20 --drill-after 1 --drill-delay 2 --drill-sites 2`) | exit 0 both; `TERRAIN FROM SERVER` on the HUD (screenshot looked at: the plaza, its walls, a stack of crates); 60 of 64 probes differ; **7931 ChunkData / 16.3 MiB** (5813 installed packed, 2118 shown); started 7.3 s, every chunk in 15.2 s (wall) after its `Welcome`; store at most 2145 slots; 0 ticks held; world hash `fbb0a0180436bff6` and entity id hash `5434b9be54125ca1` equal the server's at opSeq 374 |
| **Terrain mode, game client on the arena** (`arena250.session --precasts 40 --drill 20 ...`, `--other-terrain --frames 1500`) | exit 0 both; **the game's whole 48 x 44 x 48 window: 20 153 ChunkData / 40.0 MiB** (14 910 installed packed, 5243 shown), one ack of 6.6 KiB; started 6.1 s, every chunk in 27.7 s (wall, localhost) after its `Welcome`; store at most 5248 slots; window hash `b1834fc7b7614332` = the server's over [-24,-12,-24..24,32,24) at opSeq 370; screenshot looked at (`TERRAIN FROM SERVER`, a precast crater, trees, no packed-looking block) |

**What the acceptance showed.** Every check of the stage holds on one machine: no check reported a difference where
none was made (0 mismatches and 0 checks out of order over every run above and every `Region250` test), an injected
broken chunk is found and repaired, and a client whose generator differs everywhere plays on the server's terrain with
equal world and entity hashes, from the playground to a 240 m walk across the arena at a 100 ms round trip, never held.
**Terrain-mode cost** (the design's estimate was 8-25 MB for a 154 m window): a join of the playground 16.3 MiB, the
game's whole window on the arena 40 MiB, then 0.8 MiB per two-chunk window step (1.9 MiB at most) while walking; on
localhost the game starts 6-7 s and has every chunk 15-28 s after its `Welcome`. The bulk pacer's 32 KiB window caps it
at about 1.9 MiB/s (one hand-over per tick), about 0.3 MB/s at a 100 ms round trip: on the owner's LAN a Windows client
in terrain mode is expected to start in about 5-10 s on the playground (*estimate*). The start gate waits only for the
3D-nearest data.

**Not done (7b):** ~~the review of 7b with fresh eyes~~ (done: "Review of stage 7b" below) and the commits; the Windows build itself (the
`Enet.cpp` guard is compiled on Linux only) and the two-machine run (the owner's); windowed play in terrain mode (the
owner's); splitting large bulk messages and a larger bulk ceiling for a terrain-mode peer on a quiet link (a later knob,
not 7b's); encoding chunks against what the client holds; a pristine chunk off every player's path is never checked (by
design). Stage 8 needs nothing new from 7b (a bot is a `Client` + `Simulation` with `MakeGenerator`); stage 9's note on
`TerrainAck` stands (amendment 12).

### Review of stage 7b (2026-11-17)

Three reviewers read the uncommitted 7b diff (M-a, M-b and the integration) with fresh eyes: 12 findings. Each was
checked against the code; 11 were real and are fixed, one in part (said where). Every fix of a bug has a GPU-free test
that was run against the old behaviour (one build with each fault put back) and failed there. **Protocol 10**: the
`TerrainAck` layout changed (finding 3); protocol 9 was never committed, but the rule is "any wire byte change bumps".

| # | Finding (severity as reported) | Verdict | Fix | Test (fails on the old code) |
|---|---|---|---|---|
| 1 | A server-hidden chunk is installed packed although a face neighbour was edited after its data was encoded; nothing reveals it (low: drawing only) | **Real.** `Install` asked the ring about (ack, data] only, and `SetChunkEncoded` never asked `FaceNeighbourEdited`. A third case the report did not name: a neighbour the operation *skipped* here (not current, or data ahead) is never an edit on this world, and its later `Content` install is not one either, so an already packed chunk next to it stayed packed while the server showed it. | `World::SetChunkEncoded` turns `HiddenContent` into `Content` when a face neighbour is an edit here; `Install` asks the ring about (ack, max(data, applied)]; in terrain mode an operation that skips a chunk calls the new `World::ShowServerHiddenNeighbours` (its server-hidden face neighbours become shown for good, a packed one is queued for its decode). All conservative: at worst a few resident chunks the server packs (263 of the walker's window on the arena walk, below). | `World.AHiddenInstallNextToAnEditIsShown`, `ChunkSync.HiddenDataNextToAServerEditIsShown` (both orders: data before and after the operation) |
| 2 | A terrain-mode window step costs the server up to 13 ms (est. 1-3 ms), undocumented (medium-high) | **Real.** `OnTerrainWindow` walked the whole update box (101 376 cells) at every step, stable-sorted the whole queue (about 20 000 chunks) with a 3D `length` per entry, and re-sorted it at every chunk crossing outside any timer. | The ack visits the entering cells only (`net::ForEachEnteringChunk`, shared with `ChunkSync`; protocol 10, finding 3); the queue is pruned at every window update (finding 9); `SortChunks` keeps only the nearest 1024 entries sorted at the queue's back (`nth_element` + sort on squared distances, selected again when that tail runs out), so a chunk crossing costs O(n), not O(n log n) with square roots. Measured on the terrain-mode arena walk: **a step's ack 1.2-1.3 ms on average, 4.1-5.1 ms at most** (wall clock in a tool whose bots share the cores; one profiled run: prune 0.55, walk 0.75, select 0.4-0.7 ms with 17 000 queued); the join's ack (the whole box, once) 8.7-13 ms; queue selections **0.66-2.66 ms at most** across runs (wall clock; the full sort measured 4.1 ms); the server's whole tick with that walker 0.49 ms on average, **p99 under 3.5 ms**. Not gated: the tool's wall clock is shared with its bots (stage 3's rule: wall clock is only logged); `vortex_server` logs `step acks ... ms at most, ... on average` and a `Ticks: server:` line. | the arena walk's `Terrain: server:` and `Ticks:` lines |
| 3 | Each step's `TerrainAck` is about 6.8 KiB on the unpaced events channel (est. 0.1-3 KB), not compared (medium) | **Real.** Runs had to cover the whole box in `VoxelMask` order: a step had about 2112 rows of code-0 runs. | **Protocol 10**: a LEB128 count of the cells coded, then runs over the entering cells only, in `ForEachEnteringChunk` order (both sides walk the same cells; the client fails an ack whose count is not its entering count). Arena walk: **0.67 KiB per x step on average, 1.03 KiB at most** (59 steps; the walk is along x), all 60 acks 46 KiB (409 KiB before); join ack 6.5 KiB (arena), 2.7 KiB (playground). Against the events budget (est. 24 kB/s): a runner's acks add under 1 kB/s. | `Wire.CheckAndTerrainMessagesRoundTripAtTheirSizes` (sizes, count / runs disagreeing), `Client.TerrainAckInstallsUniformChunksAndAwaitsTheOthers` (one cell short fails the client) |
| 4 | Check costs: the game client measures 1.5-2 ms per check against an estimate under 1 ms; the server figure was the lowest run (medium-low) | **Real (the record).** Measured again: the game clients of `tools/session-check.sh` **2.47-3.67 ms at most** over two runs (17 / 15 HashChecks, 0.7-1.3 ms per check on average), the terrain-mode game client on the arena 2.00 ms at most (0.5 ms on average); the server **0.41-3.87 ms at most per peer and tick** depending on the run (the arena walk 3.87, session-check 2.83, the terrain-mode arena walk 1.36-1.48, the drill 0.41). The compute is small: `ChunkHash` of a dense terrain chunk measured 10.9 us (400 chunks), so 24 entries are about 0.3 ms on an idle core; the maxima are wall clock in runs where three processes (or a server and its bots' worker threads) share the laptop's cores. | Numbers written here, in ROADMAP and the Windows guide; no code change. If windowed play shows a hitch every 2 s, fewer entries per check for a standing player is the knob (the comparison must stay at `applied == C`). | - |
| 5 | The Windows guide says differing terrain is "not a divergence", but chunks no check reaches still make gate 3's hashes differ; its LAN estimate disagrees with this document (low-medium) | **Real.** | `docs/two-pc-test-windows.md`: the terrain-mode row says a slow join when the probes or 3 checked chunks differ, and that a few differing chunks away from every player are not caught (the exit hashes then differ); the "Hashes differ" row points at gate 2 and the `Checks:` line's pristine failures; one estimate for both texts (start of play vs. the whole join, said which). | - |
| 6 | The incoming-slots rule counts chunks that will install packed, stalling re-requests in terrain mode (low) | **Real.** `m_Incoming` counted every Awaiting record, about 15 000 hidden ones on the arena join. | A record is counted while its content may take a slot: NoBase, Requested, Awaiting unless it is hidden data in a world that packs (`World::PacksHidden`, read at each `TerrainAck`); kept per record (`Record::incoming`, `Recount`). | `ChunkSync.HiddenDataOnItsWayLeavesTheFreeSlotsToUnsyncedChunks` |
| 7 | The planner marks a chunk checked even when the client skipped it; a pristine chunk first picked while still loading is not checked again before the player gets there (medium) | **Real.** The arena walk had 47 such skips; old code with the new test: at walk speed repaired 3.8 s *after* the walker stood on the chunk, at run speed never (the end hash differed). | On the client, not the planner: an entry skipped because its chunk is still loading here (NoBase, or Based and not loaded) is kept with the server's hash (at most 256) and compared once the chunk is current with a base at or below `applied`; every stream operation whose chunk range holds it drops it (`ChunkSync::Apply`), so the server's hash at the check's opSeq is still its hash; a mismatch is `OnHashCheck`'s (`ChunkSync::CheckDeferred`, `Client::CheckDeferred` every Poll while any are kept). Exact, whatever made the client skip, with no wire change and no planner heuristic. Arena walk: 43 entries kept until loaded, all 43 equal then. | `ChunkSync.AnEntrySkippedWhileItsChunkLoadsIsComparedOnceItIsCurrent` (different, equal, touched since), `Region250.AChunkPickedWhileItWasStillLoadingIsRepairedBeforeThePlayerReachesIt` (C held loading until the client processed the check that first picked it; now repaired 4.1 s (walk) / 3.2 s (run) before the reach box touches it) |
| 8 | A chunk that keeps mismatching is re-requested every 2 s without limit, holding the player each time (medium) | **Real** (the plan's choice 17 was dropped in M-a). | A mismatch of a chunk repaired since it entered the window is not asked for again when it comes within `kRepeatRepairChecks` (5) HashChecks of the repair, or after `kMaxRepairsPerChunk` (3) repairs: the chunk stays current (the player is not held), it is logged once as an error and counted (`repairsRefused`; later differences only in `stuckMismatches`). A repeat later than that is repaired again. Still a mismatch: every exit rule fails on it. | `ChunkSync.AChunkThatKeepsDifferingIsNotRepairedInALoop` |
| 9 | A terrain-mode peer's bulk queue keeps chunks that left its window until they are dequeued; on a slow link it grows without bound (medium) | **Real.** Old code on the new test: 762 queued chunks outside the window, 2358 queued against 2304 non-uniform cells in it. | Every `WindowUpdate` (both modes) prunes the queued chunks that left the new window (`PruneQueue`: one pass, keeps the order; request answers are still dropped at dequeue); `terrainBytes` (a map per update seq, never pruned) became per-step pending counters: an update leaves the map once nothing it queued is left, its bytes go into running sums. `TerrainPeerCost` reports the largest queue and the chunks queued outside the window. Arena walk: 26 000-27 000 chunks pruned, 0 queued outside. | `Region250.ATerrainModeWalkerOnASlowLinkQueuesOnlyItsWindow` (64 kB/s: 0 outside, at most 2304); `ClientWindow.QueuedChunksThatLeaveTheWindowAreDropped` now counts them pruned |
| 10 | The 250 m table never exercises terrain-mode packing; its store gates cannot fail (low) | **Real in part.** On `Plain250` the store bound (about 2304 against 10 240) is vacuous; but a Plain250 row with packing on would not test packing either: flat ground has no Dense buried chunk (the rows under the grass are uniform), so there is nothing to pack. Rejected: that row. | The noise-terrain test (the packing test) gains a bound a regression breaks: no chunk the server packs may be resident on the client farther than nearRadius + 16 m from the player (a build whose server-hidden chunks are never packed again fails it: 83). `vortex_server` logs `Terrain: bot N packing:` per terrain-mode bot and **fails the run if a chunk the server shows is packed there** (0 on the arena walk; 263 resident that the server packs, the conservative side). | `TerrainMode.ANoiseRegionPacksAsTheServerDoes` |
| 11 | The server sends a full `EntityList` for every request after the last one went out, with no rate limit (low) | **Real.** | At most one list per peer per `entityCheckTicks / 2` (60 ticks): requests in between are merged into the next (`ServerStats::entityListsDelayed`). A well-behaved client asks again only after a list ended, at a check 2 s later. | `Session.HostileOrOutOfOrderSyncMessagesAreIgnoredOrEndThePeer` (50 requests over 50 ticks: no list more during them, one after) |
| 12 | The terrain-mode joiner's latency bound leaves 1 tick over one seeded measurement (low) | **Real.** Three seeds of the links' draws: the playing bot's p99 9 / 11 / 13 ticks while a joiner with its own terrain joins, 14 / 12 / 13 while a terrain-mode joiner joins: seed 0's +5 was mostly the draws (the medians differ by 2). | Medians over three seeds (`Field` takes a seed), bound + 6 ticks as before, and every run under 30 ticks (an unpaced terrain-mode join measured 75). The test takes about 37 s. | `Region250.LateJoinDuring10CastsPerSecondConverges` |

**Re-run on the fixed code** (this laptop, dev build -O2 -g, 2026-11-17): warning-free; **710 tests (six disabled), all
green** (7 new). The standard and editor headless runs, `--climb`, `--tower-blast --after-fill --frames 700` (freed by
shot 6 of 7 by the load check, 11.27x) and `--neck-blast --frames 200` (0 pieces released by the duds; the tower
released, 5.61x) exit 0 under validation; screenshots looked at.

| Run | Result |
|---|---|
| The CLAUDE.md drill (`--max-join-p99-ticks 15`) | exit 0, hashes equal, entities agree (8 objects); 12 + 8 HashChecks, 0 mismatches; server checks 0.41 ms at most; join latency p99 11 / 9 ticks |
| The drill with `--clients 3 --join-step 50` | exit 0, hashes equal; 13 / 10 / 10 HashChecks, 0 mismatches; join latency p99 11 / 11 / 4 ticks |
| The UDP drill (`--enet --clients 3 --join-step 50 --bot-casts 90 --bot-crates 120`) | exit 0, hashes equal, entities agree (9 objects); 13 / 10 / 11 HashChecks, 0 mismatches |
| The arena walk (the CLAUDE.md command, 30 s wall) | exit 0, both sync-box hashes equal, entities agree (74 objects); walker x -118.1 to +122.0 m, 60 window updates, 0 resets, 0 ticks held; 173 / 173 HashChecks, **0 mismatches**, the walker's 43 entries skipped while loading all compared equal later; store at most 5500 / 5446; RSS 1100 MiB; server checks 3.87 ms at most per peer and tick; server ticks 0.36 ms on average, p99 under 3 ms |
| Terrain mode, the arena walk (`--bot-broken-generator 0`, `--delay-ms 40` / `50`) | exit 0, hashes equal, entities agree, 0 ticks held; 60 acks / 46 KiB; 59 steps, ack + data 796 / 786 KiB per step on average, 1.88 MiB at most; step acks 0.67 KiB on average, 1.03 at most; store at most 5513; 0 chunks the server shows packed on the walker, 263 resident that the server packs |
| `tools/session-check.sh` | **PASS**; 17 / 15 HashChecks, 0 mismatches, 0 out of order; game checks 2.5-3.7 ms at most over two runs; screenshots looked at (the plaza, crates, a crater with broken crates, `players 2`) |
| `--bot-broken-chunk 1` and terrain mode on the playground (`--bot-broken-generator 1`, `--delay-ms 0` / `40`) | exit 0, hashes equal; chunk (2, 1, 2) found and repaired once; the terrain-mode join 7996 / 7997 ChunkData, 16.3 / 16.2 MiB, one ack of 2.7 KiB |
| A terrain-mode game client on the arena (`--other-terrain --frames 1500`) | exit 0 both; 20 153 ChunkData / 40.0 MiB (14 910 installed packed); window hash equal to the server's at opSeq 374; `TERRAIN FROM SERVER` on the HUD. Its screenshot shows dark green leaves filling the view at close range (the player stands under the crown of the tree beside the spawn; the integration's run had the crown in front of the camera). Checked with a temporary log that it is not a packed block: every chunk within 8 m of the player was resident Dense, current and from data; the equal hash says the voxels are the server's. Not confirmed in a window (the owner's) |

**Open after the review** (each with a recommendation): (a) a pristine chunk off every player's path is never checked,
so a build that differs there in a few chunks plays on and its exit hash differs - by design; gate 2 (the golden tests
on the Windows build) stays the release gate. (b) Data ahead of the stream for a server-hidden chunk is installed shown
(the ring cannot say what happened between the stream and the data): conservative, measured 263 extra resident chunks
on the arena walk against 5513 used; leave it. (c) The terrain-mode join is bulk-bound (32 KiB window per round trip):
a larger ceiling for a terrain-mode peer on a quiet link is the next knob, a stage 8 measurement first. (d) Windowed
play in terrain mode and the Windows build are the owner's.

### Recheck of stage 7b (2026-11-17)

A rechecker read the 7b change after the review's fixes: 3 findings, all real (two in the docs only, one a log and test
statistic). The bug was first reproduced: the hostile-messages test now expects exactly one delayed list and failed on
the old code with **57** (ticks of waiting). No new test (one extended): **710 in all (six disabled)**. No wire byte
changed (`kProtocolVersion` stays 10).

| # | Finding | Verdict | Fix (test) |
|---|---|---|---|
| 1 | Comments and notes still describe the pre-review rule for a server-hidden chunk ("between the ack and the data"), and `TerrainCode::Data` says "installed as an edit" (low, docs) | **Real.** The code (`ChunkSync::Install` asking the ring about (ack, max(asOf, applied)], `World::SetChunkEncoded`'s face-neighbour downgrade, `World::ShowServerHiddenNeighbours` on every chunk an operation skips in terrain mode) is right; four texts were not. | `Sync.cppm` (`OnTerrainAck`), the comment in `Sync.cpp`'s `Install`, `notes/net.md` ("Packing follows the server's state") and the M-b choices row above (marked **Superseded by the review (finding 1)**) now give the rule: a hidden chunk is packed only if no operation over it or a face neighbour came between its ack and the stream's position, no face neighbour is an edit here, and no skipped operation later shows it. `Messages.cppm`: code 1 is "installed as content (`InstallMode::Content`: shown, not an edit)". No code change. |
| 2 | `ServerStats::entityListsDelayed` counted ticks of waiting, not delayed lists (low: a log and test statistic) | **Real.** `SendChecks` runs for every peer after every `Pump`; an owed list waiting up to 60 ticks added up to 60. | `Peer::listDelayed`: set and counted once when an owed list is first held, cleared when the list goes; the `ServerStats` comment says so. (`Session.HostileOrOutOfOrderSyncMessagesAreIgnoredOrEndThePeer` now expects exactly 1 delayed list for the 50 spread requests; the old code gave 57.) |
| 3 | Finding 4's "replace the single best run" was done only in the review section: "Integration and acceptance" and ROADMAP still gave 1.35 ms as the server check cost; the plan's "under 1 ms" game-client estimates were not annotated (low, docs) | **Real.** | "Integration and acceptance" and ROADMAP's integration bullet: "one run; across runs 0.41-3.87 ms per peer and tick" with a pointer to finding 4; the plan's two estimates ("Server terrain mode" cost and "Risks and what 7b leaves") carry "measured 2.0-3.7 ms at most in three-process runs, about 0.3 ms of it hashing". |

**Re-run on the rechecked code** (this laptop, dev build -O2 -g, 2026-11-17): warning-free; **710 tests (six
disabled), all green** run serially (`ctest --test-dir build`). Run with `ctest -j8`,
`Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing` failed once (and once in six copies run side by side): under
load the joiner's world had loaded fewer chunks when the operations came, so it asked for more bulk (70.9 KiB instead
of 44.3) and was not synced inside the test's 216 ticks after the join (`joinTicks` 0); it passed serially and alone
every time. A wall-clock dependence of that 7a test (the bots' worlds generate on worker threads), not of this change:
open point (e) below. The standard and editor headless runs, `--climb` (0 unloads inside view radius, window y fixed),
`--tower-blast --after-fill --frames 700` (freed by shot 6 of 7 by the load check, 11.27x) and `--neck-blast --frames
200` (0 pieces released by the duds; the tower released, 5.61x) exit 0 under validation; screenshots looked at.

| Run | Result |
|---|---|
| The CLAUDE.md drill (`--max-join-p99-ticks 15`) | exit 0, hashes equal, entities agree (8 objects); 12 + 8 HashChecks, 0 mismatches; server checks 0.47 ms at most; join latency p99 11 / 9 ticks |
| The drill with `--clients 3 --join-step 50` | exit 0, hashes equal, entities agree; 12 / 10 / 10 HashChecks, 0 mismatches |
| The UDP drill (`--enet --clients 3 --join-step 50 --bot-casts 90 --bot-crates 120`) | exit 0, hashes equal, entities agree (9 objects); 13 / 10 / 10 HashChecks, 0 mismatches; server checks 0.85 ms at most |
| The arena walk (the CLAUDE.md command) | exit 0, both sync-box hashes equal, entities agree (74 objects); walker x -118.1 to +122.0 m, 60 window updates, 0 resets, 0 ticks held; 134 / 134 HashChecks, 0 mismatches (4 entries kept while loading, all equal later); store at most 5521 / 5459; RSS 1103 MiB; server checks 2.33 ms at most per peer and tick, 203 ms over the run |
| `tools/session-check.sh` | **PASS**; 0 hash mismatches, 0 out of order; game checks 2.25 / 1.27 ms at most; server checks 3.45 ms at most per peer and tick; 0 lists delayed (no list was asked for); screenshots looked at (the plaza, the crates, broken crates by the drill, `players 2`, ping 21-25 ms) |

**Open after the recheck** (with a recommendation): (e) `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing` depends
on how far B's world loaded on its worker threads before the operations came, so under a parallel `ctest` it can fail.
Recommendation: in `RunLateJoin` wait (ticks, not wall time) until B's window has loaded before the drill steps go on
(as the gated-generator tests do), or register it `RUN_SERIAL` as `EnetSession` is; a test change only, for the stage
that next touches the pacing tests. (a)-(d) above stand.

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
   2026-11-16: kept at 0.512 m^3 (262 voxels at 0.125 m). Fun and few bugs come before synced rubble.

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
