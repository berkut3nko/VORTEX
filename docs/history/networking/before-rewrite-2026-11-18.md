# Networking history: the sections of the current document as they were before the WP3 rewrite

Moved verbatim from `docs/design/networking.md` lines 2-21, 22-44, 63-87, 88-103, 104-125, 126-151, 152-165,
166-194, 195-219, 220-243, 244-274, 275-300, 301-321, 322-337 and 3169-3180 at engine `430fde0` on 2026-11-18 (WP3
document rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

<!-- source lines 2-21 -->

Status: design decision, revised after two reviews. Stages 0-6 are built, stage 7a too (both milestones, integrated and
accepted on 2026-11-17; "Stage 7a as built", integrated, accepted and reviewed ("Review of stage 7a", "Recheck of
stage 7a"), committed as `62f28b4`); stage 7b ("Stage 7b plan", amended after a critic, superseded where "Stage 7b as
built" differs) is built, integrated and accepted on one machine (2026-11-17): milestone M-a (`HashCheck` repair, the
wire `EntityCheck` and the entity list) and milestone M-b (server terrain mode, the whole test table on a 250 m region,
the Windows-build fixes), then reviewed with fresh eyes ("Review of stage 7b": 12 findings, 11 fixed - protocol 10 -
one in part; the acceptance re-run on the fixed code) and rechecked ("Recheck of stage 7b": 3 findings, all fixed, no
wire change); committed as engine `9cbfdaa` / game `7082a1c` ("Stage 7b as built", its "Integration and
acceptance"). **The owner's two-machine run passed on 2026-11-17** at those commits (stage 5's acceptance; "Owner's two-machine run" below); stage 8 (the soak, `vortex_bot`, the measurements; "Stage 8 plan", amended after a critic) is built, verified on one machine, reviewed and rechecked, not committed ("Stage 8 as built", "Review of stage 8", "Recheck of stage 8"); M1's acceptance is the owner's LAN run on stage 8's commits (see "Known risks of terrain
agreement after stage 0", "Stage 1 as built", "Stage 2 as built", "Stage 3 as built", "Stage 4 as built", "Review of
stage 4", "Stage 5 as built", "Review of stage 5", "Stage 6 as built" and "Review of stage 6"): the protocol runs
over ENet (UDP) between `vortex_server` and the game (`Shredborne --connect`), and in one process over loopback transports
for the tests; since stage 6 crates, terrain pieces of the replicated size and object pieces are server bodies that every
client copies. Stage 5's acceptance (two people on two machines) is the owner's to run; on one machine it is shown with a
server and two headless game clients on localhost. "Code" = read from the source at
commit `ac17bcf` and re-checked for this document, so the table "What the code gives us today" describes the code
before stage 1; "measured" = figure from `docs/ROADMAP.md`; "est." = estimate, replaced by a measurement in the stage
named under "Stages". Related: `fx-budget.md` (its simulation / presentation split is reused), `collapse.md`.


<!-- source lines 22-44 -->

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


<!-- source lines 63-87 -->

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


<!-- source lines 88-103 -->

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


<!-- source lines 104-125 -->

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


<!-- source lines 126-151 -->

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


<!-- source lines 152-165 -->

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


<!-- source lines 166-194 -->

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


<!-- source lines 195-219 -->

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


<!-- source lines 220-243 -->

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


<!-- source lines 244-274 -->

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


<!-- source lines 275-300 -->

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


<!-- source lines 301-321 -->

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
| Late join during 10 casts/s and object splits, bulk limited to 32 kB/s; walk in from outside; chunk still loading | Converges: equal hash, ids and models; event latency p99 under 250 ms during the join **at 1 % loss** (in-process); at 5 % loss the join adds at most 3 ticks to the quiet p99 (stage 8, O4, as restated by its review and recheck: over real ENet at 5 % loss each way and 40 + 0-30 ms each way the quiet p99 is 0.58-0.79 s for the plain-link bots that stand or wander, but 1.49 s for the arena walker (R10, open) and 0.85-1.29 s behind 100 kB/s - ENet's resend timeout; "a join adds about a tick" rests on one bot on one region; at 1 % loss the p99 is 0.27-0.31 s during a join and 0.26-0.28 s quiet, so the 250 ms is **missed by 10-35 % over real ENet on that path**; a LAN was not measured) |
| **Soak** (stage 8): `tools/soak.sh`, `vortex_server` + 3-4 `vortex_bot` processes over real UDP, each behind a `UdpRelay` (5 % loss, 40 + 0-30 ms, a 2 s outage, a 100 kB/s late joiner, a broken chunk, a broken generator), 10 casts/s, playground and the 250 m arena, 10 minutes | Every hash over each bot's sync box and every entity line (ids, models, rest poses) equal the server's at the same opSeq; every bot's own verdict (`JudgeBot`, with the review's gates that it played as told); the objects not resting within 5 cm; the drill, joins and repairs as asked; the game client's own checks; `SOAK_SELFTEST=1` names four injected faults |
| Bulk delivered ahead of events with a carve + swap pair on the same voxels | Equal `WorldHash` |
| Generator broken on a client: everywhere; in one non-probe chunk | Server terrain mode; repaired before the player reaches it |
| Client with a deliberately small store receives fills until exhaustion | No repeated request for the same chunk; equal hash once slots are free |
| Blast at a chunk border beside a walking player; player under a falling replicated piece at 150 ms | Correction rate under a bound set in that stage; no snap |
| Collapse with 64 awake pieces, 5 % loss; cap reached with nothing asleep; split below threshold; late snapshot after `EntityRest` | Largest sample gap per body; bytes under the caps; ids equal; rest pose kept |
| +50 ms ping step; 3 lost input packets containing a cast; 150 ms server hitch; forced misprediction; forged packets | Margin recovers within 0.5 s; one cast; no hard resync; converges; no crash |


<!-- source lines 322-337 -->

## Stages

| # | Built | Verified by | Days (est.) |
|---|---|---|---|
| 0 | `-ffp-contract=off`; `ChunkHash` / `WorldHash`; golden generator tests (chunk hashes and float bits); `VortexCore` + its tests built once on Windows | Golden tests green on Linux (and on Windows if a machine exists) and red in a build without the flag; the existing 299 tests pass; generator time before / after recorded | 2 |
| 1 | `VortexSim`; gather functions moved to the engine; `sim::Simulation` with a numbered tick; server scene config | Existing tests pass; game unchanged; a test steps a `Simulation` 600 ticks in a binary that links no Vulkan or glfw | 4 |
| 2 | Quantised edits, swap, `ClearMask`, `WorldOp` / `ApplyOp`, skipped / unsynced chunk report, `sim::Blast` (islands as `Detach` with `netId = 0`) + `PresentBlast`, `Scene::Damage` threshold; offline game on this path | Unit tests; two `Simulation`s fed the same events, islands included, have equal hashes; offline blasts look the same | 5-6 |
| 3 | **Built** ("Stage 3 as built"). `VortexNet`: loopback + `ImpairedTransport`, messages, `Server` / `Client`, `RegionDesc`, fixed server window with margin, probe hashes, event stream with byte budget, session file, `vortex_server`; split template set-up; join = full event log; the cast work budget (R5) | In-process server + 2 clients, 200 server-side blasts that free islands, 5 % loss: hashes equal | 5-6 |
| 4 | **Built** ("Stage 4 as built", "Review of stage 4"). `StepPlayer` with `HasCollider`, `SetCharacterState`, inputs, snapshots, time sync, prediction, reconciliation (timeouts counted; the visual-only timeout branch is not built, see "Stage 4 as built"), remote players | No correction above 2 cm on flat ground; border-blast, ping-step, lost-cast and hitch tests; correction rate logged | 7-9 |
| 5 | **Built and accepted** ("Stage 5 as built", "Review of stage 5"; the owner's two-machine run passed 2026-11-17 at stage 7b's commits). `EnetTransport` with `ReliableBytesInFlight`; game `--connect` / `--token`; cast capture, cooldown, cast queue | **Two people on two machines walk and see each other and each other's holes** (est. day 26-30) | 3 |
| 6 | **Built** ("Stage 6 as built", "Review of stage 6"). `NetId`, despawn observer, replicated `Detach`, `ObjectModel`, `MoveKinematic`, `Layer::Rubble`, character mask, velocity snapshots with priority, `EntityRest`, both caps, `toRubble`, client rubble | Fight test with islands and crates; the collapse, cap, split, rest and falling-piece tests | 9-11 |
| 7a | **Built** ("Stage 7a plan" (superseded), "Stage 7a as built", "Review of stage 7a"; integrated, accepted, reviewed and rechecked 2026-11-17). Chunk sync: modified map, encode at dequeue, bases, ring, `TakeLoadedChunks` / `WindowRevision`, paced bulk, entity baseline; joins by chunk sync (M-a); a client window smaller than the region that follows the player (M-b) | The 7a rows of "Testing" (the join latency at 128 kB/s and 1 % loss: at 5 % loss unreachable, "Review of stage 7a" 7); the drill (with `--max-join-p99-ticks 15`) and `tools/session-check.sh`; the arena walk on `arena250.session` (hashes over each bot's sync box, store under the game's cap) | 7-8 (with 7b) |
| 7b | **Built** (M-a and M-b; "Stage 7b plan" with "Amendments after the critic" (superseded where the code differs), "Stage 7b as built"; integrated and accepted 2026-11-17; reviewed ("Review of stage 7b") and rechecked ("Recheck of stage 7b"), not committed). `HashCheck` repair every 2 s, server terrain mode, the wire `EntityCheck` with the list re-sent and bounded tombstones; the Windows-build fixes | The whole test table on a 250 m region (`Region250.*`); the drill, the UDP drill, `tools/session-check.sh` and the arena walk with 0 check mismatches; terrain mode end to end (`--bot-broken-generator`, the game's `--other-terrain`) with equal hashes and its bytes measured on the playground and the arena | (above) |
| 8 | **Built, reviewed and rechecked** ("Stage 8 plan" with "Amendments after the critic", "Stage 8 as built" with its finishing pass, "Review of stage 8": 20 findings, among them the bot's clock, the soak's missing gates and the rates over whole lives; "Recheck of stage 8": 8 findings, among them the latency claims restated (the arena walker's quiet p99 1.49 s, R10 open), the soak's game-client terrain check and leave lines matched by join point; verified on one machine 2026-11-17/18, not committed). `vortex_bot` (one bot per process; impairment by a `UdpRelay` below ENet), `vortex.net:bot` shared with `vortex_server` and the game, `tools/soak.sh` with a short mode, a self-test and a headless game client (`GAME_CLIENT=1`), the server timeline, the join and leave stalls removed, two ring-wrap bugs fixed (the arena's chunk-request storm, terrain-mode installs all shown), the bulk pacer under loss, the drain beside a body that never rests, re-sent chunks counted; the manual session with the real game client is the owner's (M1's acceptance) | Hashes and entity lists equal after 10-minute runs on the playground and the arena, each with a late joiner behind 100 kB/s, an outage, a terrain-mode bot and a game client; "Stage 8 measurements" | 4 |
| 9 | On-demand server world, `EnsureLoaded`, eviction; may move behind M2 | 1 km region, 20 bots: RSS and tick-time histogram; p99 tick under 16.6 ms | 5-8 |


<!-- source lines 3169-3180 -->

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
