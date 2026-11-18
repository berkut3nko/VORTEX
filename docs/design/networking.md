# Networking skeleton (M1)

| | |
|---|---|
| Status | Built and reviewed: stages 0-8 ([net-0] .. [net-8]), the M1 session protocol, server, client, chunk sync, checks, bots and soak. Stage 9 ([net-9]) not started. The detail, M1's acceptance included, is in "Stages". |
| Decided | - Owner, 2026-11-11: M1 is accepted on a 250 m arena, stage 9 behind M2; trusted testers only before M7 (ENet, plain token); Windows with Clang for the Core golden test; x86-64 with AVX2 only, no fixed-point noise; a hole one round trip after the click; lumps under the replicated threshold are client rubble ("Owner decisions" 1-6). <br> - Owner, 2026-11-12: the server window is the play bounds plus 3 chunks on every side (shred.md stage 1). <br> - Owner, 2026-11-12/13: shred.md stages 4 and 4b (cell-graph search, load check) before networking stage 3. <br> - Owner, 2026-11-16: the replicated threshold stays 0.512 m^3; crates break at their first hit ("Owner decisions" 7). <br> - Us, stage 8 (history `stage-8.md`, "Decisions from the numbers" O1-O11): no splitting of bulk messages; the bulk pacer's rate is the highest recent delivery rate; no minimum bulk share; no "have" list in M1; the 250 ms join-latency target stays an in-process gate (over real ENet recorded, not gated). |
| Next | [net-9] (on-demand server world, 1 km zones): ROADMAP "Later", behind M2. The next protocol change is M2a's ([wp-7], ROADMAP "Next"). |
| Open | "Open points" 1-6 (R10 walker latency, the pacer on a slow lossy link, re-sent chunks, bodies frozen awake, the 1 %-loss target over ENet, Windows goldens). |
| History | [stage-0-starting-point.md](../history/networking/stage-0-starting-point.md): the code before stage 1 (`ac17bcf`) and what blocked a network. <br> [stage-1.md](../history/networking/stage-1.md) .. [stage-6.md](../history/networking/stage-6.md): each stage as built, with its review (stages 3-6). <br> [stage-7a.md](../history/networking/stage-7a.md), [stage-7b.md](../history/networking/stage-7b.md), [stage-8.md](../history/networking/stage-8.md): plan, amendments, as built, integration, review, recheck; stage 8's measurements. <br> [owner-runs.md](../history/networking/owner-runs.md): the owner's two-machine run of 2026-11-17. <br> [owner-questions-2026-11-11.md](../history/networking/owner-questions-2026-11-11.md): the questions as put to the owner. <br> [before-rewrite-2026-11-18.md](../history/networking/before-rewrite-2026-11-18.md): this document's sections before the WP3 rewrite. |
| Marks | (m) measured, (e) estimate. Numbers measured in a stage are in its history file; current reference values are in [ROADMAP](../ROADMAP.md) "Performance and measured values". The protocol version is `net::kProtocolVersion` in `src/net/Messages.cppm`. |

Related: [fx-budget.md](fx-budget.md) (its simulation / presentation split is reused), [shred.md](shred.md) (Part 2 rules R5 / R6, Part 4b load check; see "Interaction with shred.md"), [collapse.md](collapse.md), [view-distance.md](view-distance.md) (full-height window, voxel size, view radius).

## Decision in brief

One rule decides every case: **the server computes; a client either repeats an integer-exact computation in the
server's order, or receives the result.** The single exception is the own player, which is predicted through Jolt and
corrected by the server; its error is measured, not assumed to be zero.

| Thing | How a client gets it | Check | Fallback |
|---|---|---|---|
| Pristine terrain | Generates it locally, FP contraction pinned off | Golden hashes per platform; probe hashes at join; sampled hashes near the player (`HashCheck`) | Server terrain mode: the server sends the chunks (RLE) |
| Terrain edits, island clears | Quantised integer operations in one server order (`opSeq`) | Sampled chunk hashes during play (`HashCheck`) | Server sends the chunk (`ChunkRequest{Repair}`) |
| Falling terrain pieces, damaged objects | Result: occupancy mask or model + entity event, in the same `opSeq` stream | `EntityCheck` | Entity list re-sent; the client asks for what it lacks (`ModelRequest`) |
| Bodies, other players | Snapshots at 30 Hz with velocity, drawn in the past | - | - |
| Own player | Prediction with the shared `sim::StepPlayer` | Server state in every snapshot | Restore + replay, error smoothing |
| Small debris, particles, flashes, lumps under the piece threshold | Local only: never simulated by the server, never hashed | - | - |

- Two GPU-free libraries, `VortexSim` (scene + sim) and `VortexNet`, plus `vortex_server` and `vortex_bot`. ENet
  behind `net::ITransport`. 60 Hz simulation on both sides (`sim::SimulationConfig::fixedTimeStep`), 30 Hz packets.
- One chunk-sync mechanism (`ChunkData` + a base `opSeq` per chunk + replay from a client event ring) serves late
  join, a moving client window, chunks still loading, repair and the generator-mismatch fallback.
- Everything that is sent has a byte budget: snapshots (1200 B at 30 Hz), terrain events (24 kB/s), entity events
  (12 kB/s), bulk (paced by the application). Work over budget is delayed or turned into client rubble, never dropped
  silently.

## Architecture

```
VortexSim      scene sim                GPU-free: Core + Physics + EnTT
VortexNet      net                      GPU-free: Sim, PRIVATE vortex_enet
VortexEngine   engine editor            links Sim, Net, Graphics; owns one sim::Simulation
vortex_server  tools/server/main.cpp    links VortexNet only
vortex_bot     tools/bot/main.cpp       links VortexNet only: one net::Bot over its own UDP socket on the wall clock
```

- Core has `ChunkHash` / `ChunkHashAt` / `WorldHash`, the quantised edits, chunk encode / install
  (`World::EncodeChunkAt`, `SetChunkEncoded` with `voxel::InstallMode`), `World::TakeLoadedChunks`,
  `WindowRevision` and `TakeWindowSteps`. Physics has `SetCharacterState`, `MoveKinematic`, `SetLayer`,
  `Layer::Rubble`, the character layer mask (`PhysicsConfig::characterLayers`), `characterMovesBodies` and
  `TerrainColliders::HasCollider` / `FlushBox`.
- `sim::Simulation` = the simulation members the engine used to own, plus a numbered tick (`Tick()`) and the phases
  `UpdateStreaming`, `StepPhysics`, `UpdateLifetimes`. `vortex.sim` also holds the region (`RegionDesc`,
  `RegionWorldConfig`, `ClientWorldConfig`, templates), `WorldOp` / `ApplyOp`, `Blast`, the cast work budget,
  `StepPlayer` and replication (`TakePieceSlot`, `Despawn`). A session world is memory-only: `sim::ServerConfig`
  clears the save directory and `net::Client::Attach` refuses a world that has one.
- `vortex.net` partitions: `:transport` (`ITransport`, loopback, impaired), `:enet` (`EnetTransport`, `UdpRelay`),
  `:wire` (byte reader / writer, events, mask runs, `RegionDesc`), `:messages`, `:prediction` (`TickClock`,
  `ServerTime`, `RemotePlayers`, `RemoteBodies`), `:session` (session file, `BuildHash`, probe chunks,
  `SessionGenerator`), `:entities` (`EntityInfo`, `CompareEntities`), `:sync` (`ChunkSync`, `OpRing`, `BulkPacer`,
  `HashCheckPlanner`), `:server`, `:client`, `:drill` (`BlastDrill`, `RunDrillStep`, `BotPilot`) and `:bot`
  (`net::Bot`, `DrainWatch`, `JudgeBot`, the shared exit lines, the timeline).
- Simulation vs presentation: `sim::Blast` (server or offline) = carve, island extraction, object damage, impulses;
  it returns the operations and events it produced. `Engine::PresentBlast` / `sim::PresentBlast` (client) =
  particles, local rubble and flash made from those and from the client's own data (ids of the voxels it removes,
  old and new object model). No particle data is sent.
- Server scene (`sim::ServerConfig`): `bakeModels = false`, zero-capacity `ParticleSystem`, `debrisRemnants = false`,
  `markDebris = false`, replication on, `replicatedPieceVoxels` from the session's voxel size.
- The game owns `net::Client` (`src/Session.cpp`) and drives it from `OnFixedUpdate`, after the engine's streaming
  step. `Engine` gained only a tick number (`Engine::Tick()`) and the tick-rate scale (`Engine::SetTickSource`: the
  client's `TicksDue` decides how many ticks a frame runs, at most 8).

## Transport

| Channel | Mode | Carries |
|---|---|---|
| 0 state | unreliable (ENet: sequenced, fragmented unreliably) | inputs up, snapshots down |
| 1 events | reliable, ordered, byte budget | handshake, the `opSeq` stream (operations, detach, entity events), window acks, terrain acks, chunk and model requests, hash and entity checks, entity lists |
| 2 bulk | reliable, ordered, paced by the application | chunk data, entity baseline and models |

- `net::ITransport` = `Send`, `Poll`, `Flush`, `Disconnect`, `RoundTripSeconds(peer)`,
  `ReliableBytesInFlight(peer)` and `ChannelBytesInFlight(peer, channel)` (exact: each reliable packet's release
  callback knows its channel). Implemented by `EnetTransport` (ENet, IPv4, peer timeout 10-30 s, default port
  `net::kDefaultPort`), `LoopbackTransport` and the decorator `ImpairedTransport` (seeded loss, delay, jitter,
  duplication, reordering, rate limit, outage; on the reliable channels a loss costs a resend delay, as ENet's does).
  `net::UdpRelay` impairs real UDP below ENet (per direction loss, outages, a drop-tail bottleneck, delay + jitter);
  `vortex_bot` and the soak use it. Only `src/net/internal/Enet.cpp` includes `<enet/enet.h>`.
- ENet channels separate only ordering: the reliable window and queue are per peer. Hence the invariant **at most
  32 KiB of bulk is inside the transport**, kept by `net::BulkPacer` per peer: window = clamp(rate x (base RTT +
  `targetDelay`), 1200 B, 32 KiB); rate = the highest delivery rate of the last eight half-second intervals, or the
  smoothed drain rate when higher; base RTT = the lowest of the last 12 periods of 10 s; the window halves at most
  once per round trip. Bulk is handed over only while the peer's event backlog is empty and the bytes in flight
  (events included) are under the window; a message larger than what is left waits for the peer's bulk in flight to
  drain and then goes alone. Live events are never held behind bulk in the server.
- Events: a credit of 400 B per tick (24 kB/s), burst 16 KiB, charged by every terrain event; a queued cast waits while
  it is spent; an event larger than the burst is sent anyway and counted. Entity events have a budget of their own
  (200 B per tick, 12 kB/s, burst 16 KiB) that decides only what is replicated (see "Entity replication").
- `Hello{protocolVersion, buildHash, token[16]}`. `buildHash` = `HashMix64` of `net::kProtocolVersion`,
  `voxel::kGeneratorGoldenHash` and `voxel::MaterialTableHash` (everything the carve, the island search and the load
  check read: shred.md R6). The token is compared in plain text with the session's (`--new-token` makes a random
  one). No encryption (owner decision 2). Refusals: `Protocol`, `Build`, `Token`, `Full`, `Malformed`.

## Time and ticks

- Server tick `T`, fixed 1/60 s, never dropped; after a hitch it catches up at most 2 ticks per loop pass
  (`Server::Advance`). Order (`Server::Tick`): receive -> streaming (collider focus: players and the spawn) ->
  `StepPlayer` per player in slot order -> queued casts within a **work budget** (graph node visits + chunk graphs
  used (`chunkGraphs`) at a fixed cold price per chunk, plus the load check's counted work
  (`neckExteriorVisits + neckArcs + neckFlowWork / 8 + 64 x neckSplitRelabels + neckVoxels / 64`, weights calibrated
  once) - island-box voxels scanned + face-flood visits with the reference voxel search - + operations + their boxes;
  `sim::CastWorkOf`, `sim::kCastWorkPerTick` = 60 000 units, about 6 ms on the reference machine; never graphs
  *built* or chunks decoded, which depend on the server's cache and streaming history, never a time and never the
  object half of a blast (it depends on Jolt); wall clock is only logged, so a replayed server puts every cast on the
  same tick; shred.md R5) and the event byte budget -> physics (`GuardBodies`, `Step`, `SyncFromPhysics`) ->
  lifetimes -> despawns, rests and wakes (`TrackBodies`) -> per peer: events, checks, bulk (`Flush`) -> snapshots on
  even ticks -> the hash cache refresh (32 chunks per tick).
- Cast queue: per tick the work credit grows by `kCastWorkPerTick` (never banked beyond one tick, debt at most six
  ticks); casts run in FIFO order while the work credit and the event credit are both positive; a cast that has waited
  6 ticks (`maxCastWaitTicks`) runs whatever the credits say.
- The client runs ahead by RTT/2 plus a margin. Target margin = 2 + (packet ticks - 1) + ceil(2 x jitter). Each
  snapshot reports the margin of the newest input (`marginTick`). Control is asymmetric: three late reports in a row,
  or three reports 2 or more ticks short, jump the clock forward at once; below the target it catches up at 1.02,
  above it bleeds at 0.98; above both 8 and the target for 60 server ticks a hard resync holds the surplus (no rewind).
  A joiner starts predicting only from a snapshot at most 8 ticks behind the newest it received (`startFreshTicks`).
- Inputs: every packet (every 2 ticks) carries the inputs the server has not acknowledged (at most 8). The server
  queue holds 16 ticks ahead per player; inputs further ahead are dropped (also the speed-hack bound). A late input
  refreshes the last input and its button edges are derived then, so a late cast or jump still happens once. A
  missing input repeats the last movement for 6 ticks, then zero. Buttons are levels; the server derives edges
  against `PlayerState::lastButtons`, and a repeated input never makes an edge.
- A cast is captured at its input tick as `{eye, direction, tick, loadout 0}` from the server's player state of that
  tick, with the cooldown checked then (a late input is judged at its own tick); the ray is cast and `Blast` runs
  when it is dequeued. The snapshot's owner block carries `lastCastTick` and `castRejected` (`None`, `Cooldown`,
  `Missed`, `Outside`). The ray also meets replicated bodies in front of the terrain.

## Region start-up and terrain agreement

- `RegionDesc{templateId, seed, play bounds (whole chunks), heightRange, generator, flatGroundVoxels, TerrainSettings
  as raw float bits}` travels in `Welcome`; `sim::IsValid(region)` is checked before anything is built (network input:
  out-of-range settings make the generator's float-to-int conversions undefined). Both sides build their world from it
  through `sim::RegionWorldConfig` (server) / `sim::ClientWorldConfig` (client) and run memory-only. The **voxel size
  is a session parameter** (raw bits): the integer operations mean nothing between two sizes and `buildHash` does not
  cover it. The game refuses `--voxel-size` (and the editor, `--camera` and the offline scripts) in a session.
- Template set-up runs **on the server only** (`sim::PlaygroundTemplate`, template 1): its edits are operations 1..k,
  its lights `EntitySpawn` kind `Light`, its crates server bodies; the spawn point travels in `Welcome`. The offline
  game applies the same template in its old order.
- Build: `-ffp-contract=off` in `vortex_options` (Jolt keeps its flags); the same Clang major on both platforms.
  Under clang-cl the flag is spelled `/clang:-ffp-contract=off` (the bare form is ignored with a warning only); the
  build makes an unknown argument an error.
- Tests, in `VortexCoreTests`, all checked in: (1) hashes of 64 fixed chunks for 3 seeds
  (`Terrain.GoldenChunkHashes`); (2) the raw float bit patterns of the noise functions and of the terrain height at
  4096 points per seed out to 1 km (`Terrain.GoldenNoiseBits`, `Terrain.GoldenHeightBits`). (2) is the one with
  power: (1) sees floats only after `floor` and threshold compares. `Terrain.GeneratorGoldenHashNamesTheGoldenTables`
  ties `voxel::kGeneratorGoldenHash` to the tables. If (2) fails on Windows, fixed-point noise is the answer (est. 5-8
  days, changes all terrain); it is not built otherwise.
- Run time: the server sends hashes of 64 probe chunks chosen from the seed (`net::kProbeChunks`). A client that
  disagrees, or that later fails 3 pristine hash checks, enters **server terrain mode** ("Server terrain mode").

### Known risks of terrain agreement after stage 0

| Risk | State |
|---|---|
| The Windows golden tests have not been run. | **Open.** The owner's Windows client (2026-11-17, history `owner-runs.md`) joined a Linux server with all 64 probe chunks equal and a pixel-identical world: evidence, not proof. The three golden tests on the Windows build stay the release gate. Server terrain mode is the safety net: such a client plays on the server's terrain with equal hashes, at the price of a much larger join. |
| The chunk-hash golden has little power against last-bit differences (built without `-ffp-contract=off`, every noise and height golden fails but only 1 of 192 chunk hashes differs, a 1 km probe; the first probe set, all within 131 m and 64 of them air, did not differ at all). | Closed by the float-bit goldens. The run-time probe check has the same weakness: it can pass while a few voxels per square kilometre differ. Such a client is caught only by a `HashCheck` on a chunk near a player; a chunk away from every player is never checked, so its exit hash differs. The float-bit goldens are therefore a release gate for every client build, not only a unit test. |
| clang-cl drops a bare `-ffp-contract=off`; MSVC and other compilers get no flag. | Handled in `CMakeLists.txt` for clang-cl (checked with the driver only). Other compilers: configure warning; the goldens decide. |
| libm. The path from seed to voxel ids calls only `std::floor`, `std::ceil`, `std::abs`, `std::lround`, `std::min/max` and + - * /. | No libm risk today. Any future generator (worldgen.md) that calls a transcendental function re-opens it; the goldens only cover the default settings. |
| Compiler freedom other than contraction: x87 excess precision, reassociation, `-ffast-math`, a rounding mode set by a host application or driver. | Not possible on x86-64 with SSE and without fast-math flags, by the language rules; not measured on a second compiler version. The goldens would show it. |
| The goldens pin the **default** `TerrainSettings` and three seeds. | Accepted: other settings run the same float operations on other inputs; agreement there is an inference, not a test. |
| Float-to-int conversion out of range (settings near zero or NaN, chunk coordinates beyond +-2^25). | `voxel::IsValid` / `sim::IsValid` are called on every `RegionDesc` received or parsed. |

## World operations

Unit `q` = 1/16 voxel. Sphere test on every side: `d = 16*v + 8 - c`, inside when `dot(d, d) <= r*r` in int64. Power
is `u16` in 1/16 hardness units (a voxel survives when `hardness * 16 > power`, an exact float compare).
`World::CarveSphereQ / FillSphereQ / SwapSphereQ / ClearMask / FillBox`; the float functions quantise and forward.
Edit visit order is part of the contract. `WorldOpKind` values (`CarveSphere` 1, `FillSphere` 2, `SwapSphere` 3,
`FillBox` 4, `Detach` 5, `ClearMask` 6) are wire format; `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes`
pins the whole contract.

| Event (all carry `opSeq` order) | Payload |
|---|---|
| `CarveSphere` / `FillSphere` / `SwapSphere` | kind, centre 3 x i32 q, radius u16 q, power u16 or from / to u8 |
| `FillBox` | kind, min, max voxels, id u8 |
| `Detach` | netId u32 (0 = client rubble), origin, dims, RLE **occupancy mask**, velocity (presentation only) |
| `ClearMask` | origin, dims, RLE occupancy mask (all crumbs of one blast; no netId, no velocity) |
| `ObjectModel` | netId, voxel offset, motion type, dims, RLE ids |
| `EntitySpawn` / `EntityDespawn{reason}` / `EntityRest{tick, pose}` / `EntityWake{tick}` | see "Entity replication" |

Event sizes are pinned by `Wire.*` tests; the measured sizes per kind are in history `stage-8.md` ("Stage 8
measurements").

1. A client never sends an operation. Cast is a button; the client plays the cast effect and changes nothing.
2. `sim::Blast` on the server: carve; the island search (cell graph, face flood, load check; see "Interaction with
   shred.md") -> one `Detach` per island of at least `scene::MinPieceVoxels(voxelSize)` (0.016 m^3 in voxels of the
   session's size: 8 at 0.125 m), one `ClearMask` for all smaller islands together; `Scene::Damage` per caught
   object -> `ObjectModel` for the survivor, `EntitySpawn` per piece; impulses. Casts that do not fit the region
   (`sim::CastFitsRegion`: centre in the play bounds, radius at most `sim::kMaxCastRadiusQ`, carve box inside the
   rows) are rejected.
3. A client applies events in `opSeq` order with the same `World::*Q` call, clipped to the chunks it holds current
   ("Chunk sync"). For `Detach` it clears the masked voxels and builds the piece's volume **from the ids it just
   removed** (worlds are equal at that `opSeq`). If a touched chunk is not current here and `netId != 0`, it sends
   `ModelRequest` (answered on bulk). Terrain is never predicted; the price is a hole one round trip after the click.
4. The crumbs of a blast share one `ClearMask`, which a client shows as particles only (no piece, no body).

## Chunk sync (late join, moving window, repair)

- Server: map `modified: chunk -> last opSeq`, fed from the chunks each operation changed (`EditReport::changed`,
  `Server::Impl::EmitOp`; every server path that applies a terrain operation goes through it), never cleared.
- The client sends `WindowUpdate{windowSeq, flags, minChunk, size}` at the `Welcome` and for every step its window
  takes (`World::TakeWindowSteps`). The server answers `WindowAck{asOfOpSeq, mask of entering modified chunks}` on
  the events channel (so the client receives it with every applied event at or below it) and queues `ChunkData{coord,
  asOfOpSeq, RLE}` on bulk for the entering modified chunks. **A chunk is encoded when it is dequeued**, so
  `asOfOpSeq` is current and the ring covers transit, not the queue. Bulk order: request answers, chunks within 32 m of
  the player, the entity baseline, then the rest nearest first; queued chunks that left the window are pruned at every
  update.
- Client state per chunk (`net::ChunkSync`): **NoBase** (entered, nothing received: accepts no events, the player is
  held over it), **current** with a base (`ChunkData.asOfOpSeq`, or the ack's `asOfOpSeq` for a pristine one),
  **Requested**, **Awaiting** (terrain mode), **Unsynced** (no store slot).
- One rule for both directions: **an event is applied to a chunk only if the chunk is current and `opSeq > base`**,
  live or replayed. A chunk installed (`World::SetChunkEncoded`) or reported loaded (`TakeLoadedChunks`) gets the ring
  events above its base, clipped to it; chunks that catch up together walk the ring once (`OpRing::Walk`).
- Ring: the last 1024 events or 1 MiB, whichever is less. When the ring has dropped an operation after a chunk's base
  that touched that chunk (`OpRing::CoversChunk`; an operation over 4096 chunks counts as touching everything older),
  the chunk becomes a `ChunkRequest`; it is never replayed partially.
- `World::SetChunkEncoded` into a loading cell settles its column bookkeeping (`SettleLoading`); over a loaded chunk it
  reports the whole chunk as edited (physics: no body against the old collider).
- Full store: a failed install or an operation that finds no slot marks the chunk Unsynced: logged, counted, left out
  of checks, not requested again until a slot is free (each once; the free-slot count leaves room for content still
  to come). Client capacity is the game's GPU store (12 288 slots; `--client-window` runs keep 2048 free).
- Moving window: on a region wider than the client's window (`sim::ClientWindowMoves`; the game's is 48 chunks) the
  window follows the player with one chunk of slack (`sim::kClientWindowSlackChunks`), and `World::ViewRadius` leaves
  the slack out. On smaller regions the window is the region's (fixed).
- Join: the modified chunks of the window and the entity baseline arrive on paced bulk; the player starts once the
  chunks within 32 m are current and the baseline's entities within 32 m of the spawn are in (`BaselineNear`);
  `Client::Synced()` needs every chunk current and `BaselineEnd`. Bulk installs are bounded per `Poll`
  (`maxBulkPerPoll` 64). A peer whose event backlog exceeds 8 MiB is disconnected.
- Hashes are compared only once a client is `Synced()`, over its sync box (its window inside the server's), at the
  same opSeq (`Client::Hash`, `Server::Hash`; the server caches chunk hashes by `World::ChunkRevision`).

## Checks and repair

Every 2 s (`hashCheckTicks` / `entityCheckTicks` 120, staggered per slot) the server sends a peer, on the events
channel right after its events (`SendChecks`; nothing while its events are held):

- `HashCheck{asOfOpSeq, entries}`: the `voxel::ChunkHash` of up to 24 chunks (`HashCheckPlanner`): 4 of the last
  operations, 8 modified chunks least recently checked, 12 pristine chunks (two around the feet, ten on the way
  ahead within 12 m). The client processes it at exactly its opSeq and compares only chunks current with a base at or
  below it. A mismatch only makes the chunk Requested (`ChunkRequest{Repair}`, chunk sync's path). An entry skipped
  while its chunk was still loading is kept and compared once the chunk is current (`ChunkSync::CheckDeferred`); any
  operation over the chunk drops it, or it would compare against an old hash. A chunk that differs again within 5
  checks of its repair, or after 3 repairs (`kRepeatRepairChecks`, `kMaxRepairsPerChunk`), is logged once and left
  current, never looped on (`repairsRefused`, `stuckMismatches`). `ChunkHash` keeps its maximal-run definition
  (`ChunkHash.EqualsTheVoxelByVoxelRunHash`).
- `EntityCheck{asOfOpSeq, count, xor of NetIds, xor of HashMix64(netId, model hash)}` once the peer's baseline ended.
  A mismatch sends `EntityListRequest`; the server answers with `EntityList` (NetIds and model hashes at its opSeq),
  at most one per peer per half check period (a held list counts once in `entityListsDelayed`). The client drops
  copies the list does not hold, asks for missing or wrong bodies (`ModelRequest`) and never re-makes a body from the
  list; after 3 lists in a row without a matching check it waits 10 s before the next request, then 30 s. The server caches each object's model hash by its model pointer, so a
  replicated object's `VoxelModel` is never changed in place (`ObjectModel` makes a new one).
- Exit rules: every `vortex_server` run logs `Checks: bot N:` / `Checks: server:` and fails on a mismatch where no
  fault was injected (`--bot-broken-chunk N[:X,Y,Z]` injects one: that bot stands and must repair it); the game logs
  `Checks: slot N:` at exit and `tools/session-check.sh` fails on a mismatch or a check out of order.
- A pristine chunk away from every player is never checked (the checks follow the players): see "Known risks".

## Server terrain mode

A client whose probes differ at the `Welcome`, or whose checks find 3 pristine chunks it generated differently,
stops generating the server's terrain:

- `net::SessionGenerator` answers Empty for every chunk inside the server's window. Every later `WindowUpdate`
  carries `kTerrainMode` (sticky: the server ends a peer that clears it); a switch mid-session sends a reset update
  over the current window.
- The server answers with a `TerrainAck` that codes the entering cells, in `net::ForEachEnteringChunk` order: uniform
  chunks inline, the rest as `ChunkData` (`TerrainCode` `Data` for chunks the server shows, `HiddenData` for chunks
  it packs). Data installs by the server's packing (`voxel::InstallMode`: `Content` shown and not an edit,
  `HiddenContent` packed without a slot); the client's own column classes never decide.
- A server-hidden chunk is shown when the server may show it: an edited face neighbour, an operation over it or a
  neighbour after its ack, or an operation this client skipped next to it (`World::ShowServerHiddenNeighbours`).
  Conservative: at worst a few chunks resident that the server packs. `vortex_server` fails a run when a chunk the
  server shows is packed on a terrain-mode bot (`Terrain: bot N packing:`).
- Requests for Unsynced chunks leave the slots the content still to come will take (NoBase, Requested, Awaiting
  unless it installs packed).
- Every window update prunes the server's bulk queue to the window; only its nearest 1024 entries are kept sorted
  (`SortChunks`).
- A terrain-mode join is large (the game's whole window as bulk; sizes in ROADMAP). It is shown end to end by
  `vortex_server --bot-broken-generator N` and the game's test switch `--other-terrain` (the HUD then says
  `TERRAIN FROM SERVER`); both log `Terrain:` lines at exit.

## Player movement

- `sim::PlayerInput{tick, moveX, moveZ (i8), yaw (u16), pitch (i16), buttons (u8)}`; `PlayerButton` values (jump,
  run, cast, crate) are wire format. `PlayerState{position, velocity, flags, cooldownTicks, lastButtons}`,
  unquantised to its owner.
- **One movement rule:** a player moves only through `sim::StepPlayer` (server, predicting client, offline game).
  Its gate is `TerrainColliders::HasCollider(capsule)` = chunks loaded and a collider built at least once (never
  `IsReady`: re-cook timing differs per side); on a client the chunks must also be current. Before every player step
  both sides call `TerrainColliders::FlushBox(sim::PlayerReach(feet))` (the client also before replays and restores),
  so what a character collides with follows the edits applied, not worker timing.
- `sim::PlayerTuning`, the tick length and gravity travel in `Welcome` (raw bits); a client predicts with them, never
  with its own, and `Attach` refuses a simulation that differs. The character mask is `sim::kSessionCharacterLayers`
  (Static | Dynamic) on both sides; characters move no bodies in a session (`characterMovesBodies` false).
- `SetCharacterState(id, position, velocity)` = set both, then refresh contacts. Jolt's `SaveState` is not usable
  across the wire; what does not replay exactly is absorbed by the threshold.
- The client keeps 128 ticks of `{input, predicted state}`. A snapshot carries `ackInputTick`, the owner's state and
  `lastOpSeq`; the client reconciles once it has applied events up to that sequence. A snapshot that waits 250 ms
  (`eventWaitTicks` 15) for its events is given up and counted (no visual-only branch: the next snapshot whose events
  arrived corrects for real).
- Error under 2 cm: ignored. Above: restore, replay the unacked inputs (casts in a replay are ignored), blend the
  visual error out over 100 ms. At 1.5 m or more: snap. `Movement.NoCorrectionAbove2cmOnFlatGround` must stay at 0
  corrections.
- Remote players: every snapshot lists every other player; `RemotePlayers` draws them linearly in the past
  (`net::ServerTime`, only forward). Players do not collide with each other in M1.
- Accepted inexactness, each with a logged metric: stale colliders and missing faces of an exposed neighbour after an
  edit; a moving piece is seen `interp delay + RTT/2 + buffer` late (175 ms at 150 ms ping = 1.75 m at 10 m/s), so
  contact with it is corrected, not predicted. Replay cost (est.): 13-15 `UpdateCharacter` calls at 150 ms RTT, under
  1.5 ms; `PlayerState` is 27 B.

## Entity replication

- `scene::NetId{value, piece, bornTick, despawn}`: server-allocated, counting up, never reused; `net` keeps
  `NetId -> Entity`. Entity events are part of the `opSeq` stream. `EntityDespawn{reason}` (`scene::DespawnReason`:
  Destroyed, ToRubble, Broken; wire format) is emitted from a registry `on_destroy<NetId>` observer, so every
  destruction path is covered (set `NetId::despawn` first: `sim::Despawn`).
- Server-owned: players, template objects, lights, players' crates (E: one per 30 ticks, 8 alive per player, gone
  when the player leaves), terrain islands and object pieces of at least `replicatedPieceVoxels` (0.512 m^3: 262
  voxels at 0.125 m; owner decision 7). A crate (152 voxels at 0.125 m) breaks at its first hit: despawned `Broken`,
  its remains are client-rubble fragments.
- New pieces ask `sim::TakePieceSlot`: 2 per tick, at most 64 awake replicated bodies of any kind, 256 live pieces,
  none while the entity byte budget is spent; a cast's largest pieces ask first. Over a budget: `netId` 0 (`Detach`)
  or a fragment (`EntitySpawn` with `netId` 0), which each client makes into local rubble. The live cap evicts the
  oldest resting piece, else the smallest awake one, as `EntityDespawn{ToRubble}` (a client keeps its copy as rubble).
- A survivor at or over the threshold is an `ObjectModel` (a new model, offset and motion); below it it is despawned
  `Broken`. While the entity byte budget is spent a damaged object is let go as client rubble instead.
- A replicated body is in `Layer::Rubble` while it moves and `Layer::Dynamic` at rest or while it carries a player,
  on the server and every client; rest and wake are events (`EntityRest{tick, pose}`, `EntityWake{tick}`), never
  inferred from samples. No character meets Rubble.
- Entity events are not charged to the cast queue's credit (they follow from Jolt); their own budget decides only what
  is replicated.
- Client copy of a server body: kinematic, moved with `MoveKinematic` to `RemoteBodies::Sample` (Hermite in the past,
  delay max(4 ticks, 2 x its sample interval) up to 12, then one interval of extrapolation, then hold). A snapshot's
  samples are taken only after the events it was built after; a copy at rest is at the `EntityRest` pose bit for bit;
  samples of that tick or older are refused.
- Snapshot: header, the owner's block, other players, then awake bodies (netId, raw position, quantised rotation and
  velocity, flags) in 1200 B (`kSnapshotBudgetBytes`), by a priority that grows with the snapshots a body was left
  out of and shrinks with distance (16 m).
- Joiners: the baseline (`BaselineEntity` per object or light, each encoded at dequeue with its own `asOfOpSeq` and
  rest state, ordered by distance from the spawn; `BaselineNear` after the near set, `BaselineEnd` at the end).
  Client rules: a bounded tombstone FIFO of despawned ids (4096); a spawn for a tombstoned id is discarded; events for
  a NetId that is not here yet are held and the model asked for.
- The session tests are reproducible bit for bit because colliders install in chunk order with a per-round cook budget
  (`Entities.OneScriptGivesTheSameBodiesTwice`). Offline nothing is replicated.
- Estimates the design was sized by (est., not measurements; ROADMAP's soak rows give measured bandwidth): snapshot
  header 13 B, other player 17 B, awake body 31 B, so 1200 B hold about 36 bodies and 64 awake are at most 2 snapshots
  apart without loss; per player in 5v5 down 7 kB/s quiet, 24 kB/s with 20 awake bodies, cap 37 kB/s + events
  24 kB/s, up 2.5 kB/s; server memory 154 m 0.5-0.6 GiB, 250 m 1.1-1.3 GiB (source:
  `history/networking/before-rewrite-2026-11-18.md`).

## Server world without a camera

- The server window never moves. It is `playBounds` (whole chunks) plus **3 chunks** on the four horizontal sides
  (`sim::kServerWindowMarginChunks`; owner decision 2026-11-12), so the **flood box** of every cast centred inside
  `playBounds` (`floor(centre / 16) +- 96` voxels, `sim::FloodBox`) stays loaded, and with it the island box (radius +
  24 voxels). The rows are the full height of the region's `heightRange` (snapped to 16 m); rows below its bottom are
  the world's ground and rows above its top are air for the search, never "not loaded". A face flood or a load check
  that meets an unloaded chunk (`attachedByUnloaded`, `neckUnloaded`) and an island box that is not loaded
  (`islandBoxUnloaded`) are errors (`Blast.ServerFloodBoxAlwaysLoaded`, `Blast.ServerNeverDefersAnIslandSearch`).
- `LoadBlocking` at start. Store capacity: the session's `chunk_capacity`, or `sim::RegionChunkCapacity` (the window's
  cells capped at 2 GiB of chunks). A full store, an operation the server cannot apply in full, or an unloaded search
  marks the server `Failed`, which ends the session (every peer disconnected, `vortex_server` exits 1). Packing
  (`pack_hidden`) is memory saving only: it never changes `WorldHash` or an operation's result.
- Colliders: `TerrainColliders::Update` around every player and the spawn (cooked once at `Start`, so a join cooks
  none); `GuardBodies` requests ground for every awake body; `scene.Update` and the lifetimes run on the tick.
- Stage 9 ([net-9], not built): an on-demand world for 1 km zones. A cell loads within 24 m of a player, for an event,
  for the flood box of every cast (blocking) or for an awake body; on eviction a pristine chunk is dropped, a modified
  one rests as RLE. Est. 0.3-1.5 GiB for 20-40 players; gate: p99 tick under 16.6 ms with 20 bots.

## Interaction with shred.md

Destruction decides on the server; clients only apply its operations. No shred.md stage added a `WorldOpKind` or
changed `Detach` or `ClearMask`.

- **Face flood** (shred.md stage 1): a piece that reaches past the island box is proven by a bounded flood inside the
  flood box, so it is one more `Detach` (or crumbs in the `ClearMask`) of the same blast. Integer-only, fixed
  candidate and visit order, independent of what is loaded outside the flood box. Offline a flood that meets an
  unloaded chunk defers the search (`sim::RetryIslandSearches`); a server never defers (its window has the 3-chunk
  margin).
- **Cell graph** (shred.md stage 4): the server searches on the cell graph cached per terrain chunk in the
  `Simulation` (`voxel::SearchIslandsOnGraph`), rebuilt when `World::ChunkRevision` changes; every chunk edit goes
  through `World::EditRegion` or bumps the revision. Results equal the voxel search except where a budget runs out
  (`sim::kGraphNodeVisitBudget` node visits per blast).
- **Load check** (shred.md Part 4b): after the island floods, the largest strictly overloaded set of an integer
  max-flow is released as more `Detach` operations (netId 0 offline; on a server a piece of the replicated size that
  gets a slot is a body). At most 64 parts and 60 000 voxels per cast; the flow budget is `sim::kNeckFlowWorkBudget`.
  Inputs: the flood box voxels, the integer strength table (`Material::strength`, in `buildHash` through
  `MaterialTableHash`) and the voxel size. It never runs after `FillBox`, editor edits or generation.
- **Work budget** (shred.md R5): the cast work budget charges `chunkGraphs` and the load check's counters, never
  `graphsBuilt` / `chunksDecoded` (cache history) or time ("Time and ticks").
- **Replicated bodies** (shred.md Part 4 `serverBodiesPerTick`): 2 per tick through `sim::TakePieceSlot`. The
  structural integrity plan (shred.md SI-1..SI-3) releases through the same slots when it is built.

## Bots, soak and measurement

- `net::Bot` (`vortex.net:bot`) is one bot player: a `net::Client` with its own `Simulation`, piloted by `BotPilot`
  (walks, runs, jumps, casts, drops crates, or walks across the region and back). `vortex_server`'s in-process bots,
  `vortex_bot` and the game's headless drain share it, with `DrainWatch` (applied the server's last opSeq, synced,
  every copy resting or still for 1 s) and the shared exit lines (`SessionHashLine`, `SessionEntitiesLine`,
  `ServerLeaveLine`, the awake list). `Bot::PassOnClock` counts the work inside a pass into the bot's clock.
- `vortex_bot` (`engine/tools/bot`, Net only): one bot per process over its own UDP socket; impairment by a
  `UdpRelay` (`--loss`, `--delay-ms`, `--jitter-ms`, `--outage-ms`, `--down-bytes-per-second`); `--connect HOST:PORT`
  and an end (`--seconds` or `--end-at`) are required; `--leave-file` lets a script release the bots one by one;
  `--parent-pid` ends it with its script. Its verdict is `net::JudgeBot` (synced, drained, checks clean, injected
  faults repaired once, played as told, store gate); exit 0 only when it passes.
- `vortex_server` logs per run: `Ticks: server:` (tick p50 / p99 / max and the join, first-window and leave ticks),
  `Server: longest pass ...` (`net::DurationHistogram`), cast cost per 10 000 work units, event bytes by kind, and
  with `--timeline FILE` the server's timeline from which each bot computes its `Latency:` line (event made to event
  applied; `EventLatency::quiet` for events made while no client joined).
- The soak is the game's `tools/soak.sh`: `vortex_server` and 3-4 `vortex_bot` processes over real UDP on the
  playground and the 250 m arena, each bot behind a relay (5 % loss, 40 + 0-30 ms, a 2 s outage, a 100 kB/s late
  joiner, a broken chunk, a terrain-mode bot), 10 casts/s, optionally a headless game client after the fight
  (`GAME_CLIENT=1`). It compares every client's hash over its sync box, entity line, rest hash and awake list with the
  server's leave line of the same slot **and** join point. `SOAK_SELFTEST=1` must name four injected faults.
- The measured numbers (cast cost, event latency, bytes per client, bulk per join, re-sent chunks, server tick and
  memory, corrections) are in history `stage-8.md` ("Stage 8 measurements") and ROADMAP.

## Testing

`tests/net` -> `VortexNetTests`, linking `VortexNet` only. Each side is one `Simulation` with a small window and
physics `threads = 1`; `LoadBlocking` and `TerrainColliders::Flush` remove async timing where a test wants that; a
manual clock advances all sides. `ImpairedTransport` wraps the loopback; `Region250.*` runs the table on a 250 m flat
region with the game's 48-chunk moving window. `voxel::ChunkHash` hashes canonical content; `WorldHash(box)` sums
`mix(coord, ChunkHash)` over non-air chunks. No body poses.

| Test | Asserts (as built) |
|---|---|
| Golden generator hashes; integer sphere vs brute force; event encode / decode | `Terrain.Golden*`, `QuantizedEdits.*`, `Wire.*` (sizes, refusals, forged bytes never crash a decoder) |
| Same event list on two worlds with different load timing; blast at the region edge and at maximum height | `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` (workers 1 and 3), `Region250.ABlastAtTheRegionEdgeAndCeilingReachesAStraddlingWindow`: equal `WorldHash`; same islands as in the middle |
| **Scripted fight** (M1 acceptance): 2 clients, 600 ticks, 10 casts/s with islands and crates, 5 % loss, 80 +- 30 ms, one 200 ms outage | `Entities.FightWithIslandsAndCrates`, `Region250.FightWithIslandsAndCratesChecksFindNothing`: `WorldHash`, `NetId` sets and model hashes equal on all sides, rest poses exact; own position within 5 cm after drain; 0 check mismatches |
| Late join during 10 casts/s and object splits, bulk limited to 32 kB/s; walk in from outside; chunk still loading | `ChunkSync.LateJoinDuring10CastsPerSecondConverges`, `Region250.LateJoinDuring10CastsPerSecondConverges` (also a terrain-mode joiner), `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing`, `ClientWindow.APlayerWalksInFromOutsideIntoChunksModifiedBefore`, `Region250.WalkInFromOutsideIntoChunksModifiedBefore`, `ChunkSync.AChunkStillLoadingGetsItsEventsWhenItLoads`, `Region250.ChunksThatLoadLateCatchUpWhileTheWindowMoves`: converges (equal hash, ids, models); event latency p99 under 250 ms (15 ticks) during the join **at 1 % loss in-process** (`vortex_server --max-join-p99-ticks 15` fails the drill above it; loopback only). Over real ENet at 5 % loss the floor is ENet's resend timeout: recorded, not gated ("Open points" 1 and 5) |
| **Soak**: the game's `tools/soak.sh` (see "Bots, soak and measurement"), 10 minutes per region | Every hash over each bot's sync box and every entity line (ids, models, rest poses) equal the server's at the same opSeq; every bot's own verdict (`JudgeBot`); objects not resting within 5 cm; the drill, joins and repairs as asked; the game client's own checks; `SOAK_SELFTEST=1` names four injected faults |
| Bulk delivered ahead of events with a carve + swap pair on the same voxels | `Client.BulkAheadOfEventsWithASwapPairOnTheSameVoxels`: equal `WorldHash` |
| Generator broken on a client: everywhere; in one non-probe chunk | `Region250.AGeneratorBrokenEverywhereJoinsInServerTerrainMode`, `ThreePristineChunksSwitchToServerTerrainMode`, `AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt`, `AChunkPickedWhileItWasStillLoadingIsRepairedBeforeThePlayerReachesIt`: server terrain mode; repaired before the player reaches it |
| Client with a deliberately small store receives fills until exhaustion | `Client.ASmallStoreMarksChunksUnsyncedAndRequestsEachOnceWhenSlotsFree`, `ClientWindow.AFullStoreWhileWalkingMarksChunksUnsyncedAndRequestsEachOnceWhenSlotsFree`, `Region250.ASmallStoreInTerrainModeRequestsEachChunkOnce`: no request while full, each chunk requested once; equal hash once slots are free |
| Blast at a chunk border beside a walking player; player under a falling replicated piece at 150 ms | `Movement.BlastAtAChunkBorderBesideAWalkingPlayer` (at most 4 rim corrections in 40 blasts, largest error under 2 cm), `Region250.BlastAtAChunkBorderBesideAWalkingPlayer`, `Entities.PlayerUnderFallingPiecesAt150ms`: no snap |
| Collapse with 64 awake pieces, 5 % loss; cap reached with nothing asleep; split below threshold; late snapshot after `EntityRest` | `Entities.CollapseWith64AwakePiecesAt5PercentLoss`, `Region250.CollapseWith64AwakePiecesOutsideOneWindow`, `Entities.ASplitBelowTheThresholdBreaksTheObjectIntoClientRubble`, `Client.ALateSnapshotAfterAnEntityRestKeepsTheRestPose`: snapshots under 1200 B; ids equal; rest pose kept |
| +50 ms ping step; 3 lost input packets containing a cast; 150 ms server hitch; forced misprediction; forged packets | `Movement.PingStepMarginRecoversWithinHalfASecond` (within 0.5 s of the first short report), `LostInputPacketsWithACastStillCastOnce`, `ServerHitchNeedsNoHardResync`, `ForcedMispredictionsConverge`, `ForgedPacketsAreHarmless`: one cast; no hard resync; converges; no crash |
| Over real UDP | `EnetSession.TwoPlayersWalkCastAndAgreeOverUdpWithLoss`, `Relay.ShapesRealUdpTraffic`, `Bot.*` |

## Stages

| ID | Stage | State | Commits (engine / game) | History |
|---|---|---|---|---|
| [net-0] | Determinism floor: `-ffp-contract=off`; `ChunkHash` / `WorldHash`; golden generator tests (chunk hashes and float bits) | **Built.** Golden tests green on Linux and red without the flag; not yet run on Windows ("Known risks") | `240c650` / `82af28d` | [stage-0-starting-point.md](../history/networking/stage-0-starting-point.md) |
| [net-1] | `VortexSim`; gather functions moved to the engine; `sim::Simulation` with a numbered tick; server scene config | **Built** | `04726d1` / `5bf7793` | [stage-1.md](../history/networking/stage-1.md) |
| [net-2] | Quantised edits, swap, `ClearMask`, `WorldOp` / `ApplyOp`, skipped / unsynced chunk report, `sim::Blast` (islands as `Detach`) + `PresentBlast`, `Scene::Damage` threshold; crumbs as one `ClearMask` | **Built** | `83b5981`, `5cda1c2` (crumbs) / `2b8e88a`, `7b853bb` | [stage-2.md](../history/networking/stage-2.md) |
| [net-3] | `VortexNet`: loopback + `ImpairedTransport`, messages, `Server` / `Client`, `RegionDesc`, fixed server window with margin, probe hashes, event stream with byte budget, session file, `vortex_server`; split template set-up; the cast work budget (R5) | **Built and reviewed** | `44d4efa` / `0663ec3` | [stage-3.md](../history/networking/stage-3.md) |
| [net-4] | `StepPlayer` with `HasCollider`, `SetCharacterState`, inputs, snapshots, time sync, prediction, reconciliation, remote players; no correction above 2 cm on flat ground, correction rate logged | **Built and reviewed** | `bbfae73` / `f10e3c3` | [stage-4.md](../history/networking/stage-4.md) |
| [net-5] | `EnetTransport`; game `--connect` / `--token`; cast capture at the input tick, cooldown, cast queue; `Engine::SetTickSource` | **Built and reviewed.** Accepted: the owner's two-machine run (Linux host, Windows client) passed on 2026-11-17 at engine `9cbfdaa` / game `7082a1c` | `8daae32` / `50cfcc2`; run recorded in `448bc85`, `71dc23e` | [stage-5.md](../history/networking/stage-5.md), [owner-runs.md](../history/networking/owner-runs.md) |
| [net-6] | `NetId`, despawn observer, replicated `Detach`, `ObjectModel`, `MoveKinematic`, `Layer::Rubble`, character mask, velocity snapshots with priority, `EntityRest` / `EntityWake`, both caps, client rubble, players' crates | **Built and reviewed** | `c1e19c8` (owner's partial), `40e70b7` / `092cf91` | [stage-6.md](../history/networking/stage-6.md) |
| [net-7a] | Chunk sync: modified map, encode at dequeue, bases, ring, paced bulk, entity baseline, join by chunk sync (M-a); a client window that follows its player on regions wider than 48 chunks (M-b) | **Built and reviewed** (review and recheck) | `62f28b4` / `92e3902` | [stage-7a.md](../history/networking/stage-7a.md) |
| [net-7b] | `HashCheck` repair, the wire `EntityCheck` with the entity list, bounded tombstones (M-a); server terrain mode, the test table on a 250 m region, the Windows-build fixes (M-b) | **Built and reviewed** (review and recheck) | `9cbfdaa` / `7082a1c` | [stage-7b.md](../history/networking/stage-7b.md) |
| [net-8] | `vortex_bot`, `vortex.net:bot`, `UdpRelay`, the game's `tools/soak.sh` (short mode, self-test, game client), the server timeline, the join and leave stalls removed, two ring-wrap bugs fixed, the bulk pacer under loss, re-sent chunks counted, the measurements | **Built and reviewed** (review and recheck). M1's acceptance is the owner's windowed LAN run on these commits (the game's `docs/two-pc-test-windows.md` 6.5): not yet reported | `430fde0` / `10197d2` | [stage-8.md](../history/networking/stage-8.md) |
| [net-9] | On-demand server world, eviction; 1 km region, 20 bots: RSS and tick-time histogram, p99 tick under 16.6 ms | **Not started.** Behind M2 (owner decision 1) | - | - |

## Open points

1. **R10, the walker's event latency.** Over real ENet at 5 % loss the arena walker (the client with the most bulk) had
   a quiet p99 about twice that of the other plain-link bots. Next: a 10-minute arena A/B (`vortex_server
   --pacer-delivery-interval 0` against the default, the same relay seeds, two walkers and two wanderers, per-peer
   bulk in flight and ENet's `packetThrottle` on the server's leave line) before M6's pacer work. Not an M1 blocker.
2. **The pacer on a slow, lossy link** (stage 8 O5): a 100 kB/s link with loss and jitter is under-used, and a larger
   window only fills the bottleneck's queue. M6 (Wi-Fi): measure ENet's reliable window and back-off per peer.
3. **Re-sent chunks** (stage 8 O3): a player crossing a large region gets about half its bulk as unchanged re-sends.
   The memory milestone: a "have" list with a client cache of the slab left (a wire change).
4. **Bodies frozen awake** (stage 8 O11): a replicated body below the arena window's bottom row can never get a
   collider, stays awake for good and counts against the 64-awake cap. M3 or the next arena work: let the server put
   such a body to rest or despawn it.
5. **The 1 %-loss target over real ENet** (stage 8 O4): the in-process gate holds; over ENet it is missed by one
   resend. Kept as the in-process gate; a LAN figure needs a shared clock across machines (not built).
6. **Windows goldens** ("Known risks"): run the three golden tests on the Windows build (M6, or the owner's Windows
   report). Small items also open: sharing the hash cache with `HashCheck`, a wall-clock `--exit-at` for a game
   client that plays in the soak, splitting large bulk messages, encoding chunks against their pristine version.

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

## Owner decisions

The owner accepted the recommended answer to every question of 2026-11-11 (the questions as they were put:
[owner-questions-2026-11-11.md](../history/networking/owner-questions-2026-11-11.md)):
1. M1 is accepted on a 250 m arena; the on-demand server world for 1 km zones moves behind M2.
2. Trusted testers only before M7: ENet with a plain token, no encrypted transport.
3. A Windows machine with Clang is to be used in stage 0 for the Core golden terrain test only
   (if none turns out to be available, server terrain mode is the safety net until M6).
4. Servers are x86-64 with AVX2 only; no fixed-point noise.
5. A hole appearing one round trip after the click is acceptable; predicted carving is judged in M2.
6. Pieces under the replicated threshold (start 512 voxels) are client-only rubble; tuned in stage 6.

Later decisions:

7. 2026-11-16 (the threshold, asked in "Review of stage 6", history `stage-6.md`): kept at 0.512 m^3 (262 voxels at
   0.125 m). Fun and few bugs come before synced rubble: crates break at their first hit, and only large pieces are
   shared.
