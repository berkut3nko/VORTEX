# Notes: sessions (`src/net`, `src/sim/Region.cppm`, `tests/net`, `tools/server`, `tools/bot`)

[[../../../00 Docs Map|Docs map]] · [[../design/networking|networking design]] · [[../ARCHITECTURE|architecture]]

Checked against engine `430fde0`.

Read `CLAUDE.md`, `docs/ARCHITECTURE.md` 7.5 and `docs/design/networking.md` first; nothing from them is repeated
here. The plans, as-built sections, reviews and rechecks of every stage are in `docs/history/networking/`
(`stage-3.md` ... `stage-8.md`); the design's "Stages" table maps the IDs used below ([net-6] = networking stage 6).
Paths are relative to `engine/`. Code is cited by file and symbol; search for the symbol. The game's session mode is
`../src/Session.cpp`. The text before this rewrite, with its line numbers and measurements, is
`docs/history/notes/net-2026-11-18.md`.

## 1. Map

- `src/net/Net.cppm` - primary unit of `vortex.net`; re-exports the partitions below (`:transport`, `:enet`, `:wire`,
  `:messages`, `:prediction`, `:session`, `:entities`, `:sync`, `:server`, `:client`, `:drill`, `:bot`).
- `src/net/Transport.cppm`, `internal/Transport.cpp` - `ITransport`, `TransportEvent`, `Channel`,
  `LoopbackTransport` (shared inboxes, `Connect(a, b)`), `Impairment` / `ImpairedTransport` (send-side queue sorted by
  due time; reliable packets keep per peer-and-channel order through `m_LastDue`).
- `src/net/Enet.cppm`, `internal/Enet.cpp` - `EnetTransport` [net-5] (UDP through ENet; the only file that includes
  `<enet/enet.h>`), `EnetSettings` (timeouts, test receive loss), `EnetStats` [net-8] (bytes per channel),
  `ParseEndpoint`, `kDefaultPort`; `UdpRelay` [net-8] (a UDP relay on 127.0.0.1 with loss, delay + jitter, outages and
  a drop-tail bottleneck per direction, on a thread of its own, ENet's socket calls only: `vortex_bot`'s impairment
  below ENet; `Seconds()`, `AddOutage`, the `RelaySettings::onAdmit` test hook).
- `src/net/Bot.cppm`, `internal/Bot.cpp` [net-8] - `Bot` (a client, its simulation built at the Welcome with the
  game's store and packing when it has a window, a `BotPilot`, the per-pass ticks, applied times; an injected broken
  chunk makes it stand until repaired; `PassOnClock` for a wall clock; `WantedTicks` / `ActedTicks`), `DrainWatch`
  (the game's and `vortex_bot`'s drain rule), `JudgeBot` / `BotGates` / `BotVerdict` (a bot's own verdict, with the
  gates that it played as told), `ClientCheckFailures` (what a client's own checks say against it: the game's headless
  exit and `JudgeBot`), the exit lines (`SessionHashLine` and `vortex_server`'s `ServerLeaveLine`, both with the
  client's join point, `SessionEntitiesLine`, `ServerEntitiesLine` with `EntityRestHash`, `SessionAwakeLine` /
  `ServerAwakeLine` / `AwakeEntitiesText`, `DescribeBotMovement / Objects / Join / Window`), `BrokenChunkCandidate`
  (buried 3 m), `PercentilesOf`, `DurationHistogram` (fixed buckets: `vortex_server`'s pass times), `ResidentMiB`, the
  server timeline (`ParseTimeline`, `LatencyFromTimeline`). `vortex_server`'s in-process bots, `vortex_bot` and the
  game's `Session.cpp` share it.
- `src/net/Wire.cppm`, `internal/Wire.cpp` - `ByteWriter` / `ByteReader`, mask runs, `EventKind`, `EntityKind`,
  `Event` = `variant<WorldOp, EntitySpawn, EntityDespawn, EntityRest, ObjectModel, EntityWake>` ([net-6]:
  `ObjectSpawn`, model runs `WriteVolumeRuns` / `ReadVolumeRuns`, `VolumeHash`), `WriteEvent` / `ReadEvent`,
  `WriteRegion` / `ReadRegion`, `QuantizeVelocity`.
- `src/net/Entities.cppm`, `internal/Entities.cpp` - `EntityInfo`, `CompareEntities`, `EntityListHash`: the entity
  comparison of the drill, the tests and the scripts [net-6] (not the wire `EntityCheck` of [net-7b]).
- `src/sim/Replication.cppm`, `internal/Replication.cpp` - `AllocateNetId`, `Replicate`, `IsAwake`, `CountPieces`,
  `TakePieceSlot` (the piece budgets), `Despawn`; `ReplicationConfig` / `ReplicationState` are in `Simulation.cppm`,
  `scene::NetId` / `DespawnReason` in `scene/Components.cppm`; `sim::Blast` makes the server bodies.
- `src/net/Messages.cppm`, `internal/Messages.cpp` - `MessageType`, message structs, `Encode` / `Decode`,
  `EncodeBatch` (events already encoded), `kProtocolVersion` (its comment lists what each version changed);
  `InputBatch`, `Snapshot`, `RemotePlayer`, `Welcome::player` [net-4]; `ChunkRequestReason`, `TerrainCode`,
  `TerrainRun`, `TerrainAck` [net-7b].
- `src/net/Prediction.cppm`, `internal/Prediction.cpp` - `TickClock` (the client clock: jumps, bleed, catch-up,
  holds), `ServerTime` (the server tick other players are drawn at; only moves forward), `RemotePlayers` (other
  players drawn in the past) and `RemoteBodies` [net-6] (replicated bodies drawn in the past, the rest rule).
- `src/net/Sync.cppm`, `internal/Sync.cpp` - chunk sync [net-7a]: `SyncBox`, `RegionSyncBox` (the server window),
  `ChunkCoordHash`, `OpRing` (the client's ring of applied terrain ops as wire bytes), `ChunkSync` (the client's
  per-chunk states and bases, the base rule, replay, requests; `OnHashCheck`, `TakeRepairs`, `CheckDeferred`
  [net-7b]), `WindowEntersWhole`, `ForEachEnteringChunk` (the cells a window update enters with: client and server,
  wire order), `BulkPacer` (the server's per-peer bulk window), `HashCheckPlanner` [net-7b] (which chunks a peer's
  HashCheck holds).
- `src/net/Session.cppm`, `internal/Session.cpp` - `SessionConfig`, `ParseSession` / `LoadSessionFile` /
  `FormatSession`, tokens, `BuildHash` / `BuildHashOf` (protocol, generator golden, material table, player rule
  revision), `ProbeChunks` / `CheckProbes`, `RegionHash`, `BreakChunks` [net-7b] (a
  generator that makes some chunks differently, for tests and `--bot-broken-chunk`), `SessionGenerator` [net-7b] (the
  client's generator, Empty inside the server's window once server terrain mode is on).
- `src/net/Server.cppm`, `internal/Server.cpp` - `ServerSettings`, `CastRecord`, `ServerStats`, `net::Server` (event
  backlog, peers with send cursors, the modified map, per-peer bulk queues and pacers, the entity baseline, cast queue
  and the credits; `Impl::EmitOp`, `Impl::ColliderFocus`, `SendChecks`, `RefreshHashes`, `PruneQueue`, `SortChunks`).
- `src/net/Client.cppm`, `internal/Client.cpp` - `ClientSettings`, `ClientState`, `ClientStats`, `PredictionStats`,
  `net::Client` (events; the own player's prediction, input sends and reconciliation; `MakeSimulationConfig`,
  `Attach`).
- `src/net/Drill.cppm`, `internal/Drill.cpp` - `BlastDrill`: mushroom / bar / neck tower per step, sites of 100 x 100
  voxels in the play bounds, a crate in each blast and one per site in the margin, `KeepSites`; `RunDrillStep` (clear
  the site's bodies, build, crates, cast); `BotPilot` / `PilotSettings`: seeded controls of a bot player
  (`crateEveryTicks`: the E key).
- `src/sim/Player.cppm`, `internal/Player.cpp` - `PlayerInput` / `PlayerState` / `PlayerTuning`, `PlayerButton`,
  `StepPlayer`, `PlayerReach`, `kSessionCharacterLayers`, the quantisers (the movement rule of server, client and
  offline game).
- `src/sim/Region.cppm`, `internal/Region.cpp` - `RegionDesc` and its helpers, `RegionWorldConfig`,
  `ClientWorldConfig`, `ClientChunkCapacity`, `CastFitsRegion`, templates (`PlaygroundTemplate` is the game's plaza).
- `src/sim/Blast.cppm` (end of the constants) - `CastWork`, `CastWorkOf`, `OpWork`, the weights, `kMaxCastRadiusQ`,
  `kServerWindowMarginChunks`.
- `tools/server/main.cpp` - `vortex_server`; `tools/server/sessions/*.session` - examples that `SessionFile.*` parses.
  `tools/bot/main.cpp` - `vortex_bot` [net-8] (one bot player per process). Both link Net only and are GPU-free
  (`vortex_assert_gpu_free`).
- `tests/net/` (`VortexNetTests`) - `EnetTests.cpp` (real UDP sockets on 127.0.0.1, wall clock; the GPU-free acceptance
  `EnetSession.*`), `WireTests.cpp`, `TransportTests.cpp`, `ClientTests.cpp` (`FakeServer`: a client against a
  hand-written server endpoint - overlaps, gaps, forged `Welcome`s, late-join presentation, refused worlds; the join
  helpers `Join`, `Ack`, `SendChunk`, `ReadRequests`), `SessionTests.cpp` (the `Session` fixture: a server and bots on
  impaired loopbacks driven by one manual clock; the acceptance; pacing; late joins such as
  `ChunkSync.LateJoinDuring10CastsPerSecondConverges`; the disabled calibration), `ClockTests.cpp` (`TickClock` and
  `RemotePlayers` from margin and sample sequences; `RemoteBodies`), `MovementTests.cpp` (the `Match` fixture: a server
  driven by `Advance` and player bots each on its own `TicksDue`; the disabled correction rate over seeds),
  `EntityTests.cpp` [net-6] (the `Arena` fixture, `Match` with presenting bots and a settle that waits for every body
  to rest; the fight, collapse, split, falling-piece and crate tests), `SyncTests.cpp` [net-7a] (`ChunkSync` and
  `OpRing` against a World without a transport, `BulkPacer` against a simulated link), `ClientWindowTests.cpp`
  [net-7a] (the `Field` fixture, a flat region 40 chunks wide and a 12-chunk client window; walk-in,
  leave-and-return, teleport, reset, hash box, held player, a full store while walking, a piece that breaks off outside
  the window; its harness is `FieldHarness.h`, a header included after the imports that includes nothing itself: a
  standard header after an import can clash with a module's copy), `Region250Tests.cpp` [net-7b] (the test table on
  `Plain250`, a flat 256 m region with the game's window and store), `BotTests.cpp` [net-8] (`Bot.*`, `Relay.*`: the
  relay on the wall clock, RUN_SERIAL).
- `tests/sim/ReplicationTests.cpp` - the server half of [net-6] on one `Simulation` (piece slots, caps, damage).
- `tests/sim/PlayerTests.cpp` - `StepPlayer` alone and two simulations stepped with the same inputs.

## 2. Invariants

Wire values (CLAUDE.md lists the names; the numbers are here, `WireTests.cpp` and `kProtocolVersion`'s comment pin
them). Change any byte of a message or an event -> bump `kProtocolVersion`. Retired values are never reused.
- **Event kinds** (`EventKind`, the first byte of an event): 1-6 = `sim::WorldOpKind` (1 `CarveSphere`, 2
  `FillSphere`, 3 `SwapSphere`, 4 `FillBox`, 5 `Detach`, 6 `ClearMask`), 32 `EntitySpawn`, 33 `EntityDespawn`, 34
  `EntityRest`, 35 `ObjectModel`, 36 `EntityWake` [net-6]. A new `WorldOpKind` takes 7, a new entity event 37.
- **Message types** (`MessageType`): 1 `Hello`, 2 `Welcome`, 3 `Refuse`, 4 `EventBatch`, (5 retired: `ServerStatus`),
  6 `Inputs`, 7 `Snapshot`; [net-7a] chunk sync 8 `WindowUpdate`, 9 `WindowAck`, 10 `ChunkData`, 11 `ChunkRequest`,
  12 `ModelRequest`, 13 `BaselineEntity`, 14 `BaselineEnd`, 15 `BaselineNear`; [net-7b] checks and terrain mode 16
  `TerrainAck`, 17 `HashCheck`, 18 `EntityCheck`, 19 `EntityListRequest`, 20 `EntityList`.
- **`ChunkRequestReason`** [net-7b]: 0 `Sync` (the ring no longer covered the chunk's base, or a slot freed for an
  Unsynced one), 1 `Repair` (a HashCheck found the chunk different).
- **`TerrainCode`** [net-7b], one per entering cell of a `TerrainAck`, sent as `TerrainRun`s: 0 `NotInAck`, 1 `Data`
  (Dense on the server: `ChunkData` follows, installed `Content`), 2 `Uniform` (the run's id, 0 = air), 3 `HiddenData`
  (Packed on the server: `ChunkData` follows, installed packed). Since the review of stage 7b ([net-7b]) the runs
  follow their total and code the update's entering cells only.
- **`ForEachEnteringChunk` order** (`Sync.cppm`), wire format since the review of stage 7b ([net-7b]): every cell of
  the box when the window enters whole (`WindowEntersWhole`: the first update, a reset, a new size) or when the rows differ, in
  (y, z, x) order; else the whole columns outside the previous box (rows fixed), in (z, x, y) order. Server
  (`TerrainAck`) and client (`ChunkSync`) walk the same cells in the same order.
- Also wire format: `WindowUpdate::kTerrainMode` (2) and `kReset`, `EntityKind` (1 `Light`, 2 `Object`),
  `scene::DespawnReason` (1 `Destroyed`, 2 `ToRubble`, 3 `Broken`), `PlayerButton` (1 `Jump`, 2 `Run`, 4 `Cast`, 8
  `Crate`), `RegionGenerator` (0 `Terrain`, 1 `Flat`), `RegionTemplateId` (0 `Empty`, 1 `Playground`),
  `RefuseReason` (1 `Protocol`, 2 `Build`, 3 `Token`, 4 `Full`, 5 `Malformed`; 6 retired), `CastRejection` (0-3).

Ordering, the base rule and sync
- **Integers on the wire decide voxels.** Everything a client applies is a `sim::WorldOp`'s integers; floats travel
  as raw bits (`RegionDesc`) or are presentation (`Detach` velocity, light colour). Never add a float that a client
  turns into a voxel decision.
- **One order.** The server numbers events as it makes them (`opSeq`, counting up from 1); batches carry consecutive
  runs; a client's stream starts after its join point (`Welcome::joinOpSeq` [net-7a]) and it applies
  `opSeq == applied + 1` only. A cast's operations are consecutive (casts run one after another inside a tick).
- **The base rule** [net-7a]. A terrain operation reaches a client chunk only if the chunk is current (`Based` and
  caught up) and the opSeq is above its base, live or replayed from the ring (`ChunkSync::Apply`, `CatchUp`). A chunk
  without a current base keeps whatever it has and is never hashed (`Client::Hash` counts it `notLoaded`) or walked
  on. An op that skips a chunk it touches is "not complete here": no presentation, and a replicated `Detach` asks for
  its model (`ModelRequest`) instead of building a piece from a partial volume.
- **The modified map is fed from the chunks an op changed** (`EditReport::changed`, `Server::Impl::EmitOp`), never
  from its box: a chunk an op did not change still equals the generator's output wherever it did before. Every server
  path that applies a terrain op must go through `EmitOp` (or a joiner will take a changed chunk as pristine).
- **Bulk is encoded at dequeue** (`NextBulk`): its asOf is the last event made then, so the client's ring covers the
  transit, never the queue. A message that does not fit the window is held encoded (`heldBulk`) - still covered - but
  only until the peer's **bulk** in flight has drained (`ITransport::ChannelBytesInFlight(peer, Bulk)`), then it goes
  alone while the bytes in flight (the live events) are under the window. Two tests, both needed: the total in flight
  under the window (always), and the bulk in flight drained (for a message larger than what is left). (A held message
  waiting for `ReliableBytesInFlight == 0` never left while events flowed; going alone whatever the events let bulk
  run on a link the events alone saturate: review and recheck of [net-7a].) `ServerStats::bulkOverWindow` stays 0.
- **The ring covers by count first.** `OpRing` keeps ops as wire bytes (masks as runs), 1024 ops or 1 MiB of those
  bytes; a base below its floor is requested whole, never replayed in part.
- **A window move is noticed at the start of `Poll`** (`CheckWindow`) and nowhere else: the world moves only in
  streaming, outside `Poll`, so every op is applied against `ChunkSync`'s current window. Every step is listed by the
  world (`World::TakeWindowSteps`, with `reportLoads`) and sent as its own `WindowUpdate`, in order: a chunk that left
  at one step and came back (regenerated) at the next enters again with the second, and the server sends it again. The
  game streams every frame and polls on ticks, so two steps between `Poll`s are normal. A reset is left only for steps
  that were not listed (never in play).
- **A client's window is the region's when it fits `ClientSettings::windowChunks`** (0 = always; the game and the
  arena bots pass 48), loaded whole before anything is applied; else it is `windowChunks` x the rows x
  `windowChunks`, follows the player (`RunTick`; the game's engine follows the camera), loads its surface at `Attach`
  and streams the rest [net-7a] (`sim::ClientWorldConfig`). Its store is `sim::ClientChunkCapacity` (every cell of that
  window), capped by the owner's `storeLimit`. An operation it cannot apply in full does not fail it: the chunks it
  could not change become Unsynced and are requested whole once more than `requestReserveSlots` are free.
- **A moving window follows with slack** (`WorldConfig::recenterSlackChunks`, `sim::kClientWindowSlackChunks` = 1): it
  moves once the focus chunk is more than one chunk from its centre chunk, then centres on it;
  `sim::ClientWindowMinChunk` is where it is placed (or moved to), not where every focus puts it. Offline worlds keep 0.
  The focus may sit one chunk off the centre, so `World::ViewRadius` leaves the slack out (88 m for the game's
  48-chunk window instead of 92: the edge fog ends before the window's edge).
- **Chunks beyond the server's window are never edited** (`ChunkSync`'s `neverEdited`): current once loaded, never
  requested, never replayed. A moving window is not clamped to the region.
- **Session worlds are memory only, built from `Welcome` and pristine at `Attach`** (`Client::Attach` checks save
  directory, voxel size bits, window, height range and `BuildHash` of the simulation's materials; after loading the
  window's corner, a store that never filled, and the probe chunks of the loaded world against its own generator).
- **Live events are never held; bulk is paced.** Every Flush hands each joined peer all its events (one batch = one
  tick's events, <= `maxBatchBytes`, <= `kMaxMaskVoxels` of masks), then its bulk while its reliable bytes in flight
  are under its `BulkPacer` window (delay-based: rate x (base RTT + 2 ticks), the rate the highest delivery rate of the
  last eight half-second intervals [net-8] - a lost datagram keeps its bytes in flight for a resend timeout, so a rate
  sampled per tick fell under loss and the window with it; halved at most once per round trip while the RTT is more
  than that above its base; the base is the lowest RTT of the last 12 x 10 s, so a lasting rise of the path becomes the
  new base) - a message larger than what is left going alone once none of its bulk is in flight, still only while the
  total is under the window. The drain rate counts every reliable byte handed over (`BulkPacer::Sent` with the events
  since the last tick: counting only bulk let the rate decay to its minimum while only events flowed).
  `maxEventBytesInFlight` (default 0) holds events only for a test; `maxPeerBacklogBytes` disconnects a peer that
  stopped reading. A joiner starts at its join point. While a peer's events alone fill its link its bulk waits (no
  minimum share is reserved: a long fight on a saturated downlink delays that peer's sync).
  `ServerSettings::countChunkRequests` (tests only) keeps per-coordinate request counts; off, the server keeps none.
- **A bulk queue holds only its window** (review of [net-7b]). Every `WindowUpdate` prunes the queued chunks that left
  the new window (`PruneQueue`); only the nearest 1024 entries are kept sorted at the queue's back and selected again
  when they run out or the player crosses a chunk (`SortChunks`, O(n)): a terrain-mode queue holds about 20 000
  chunks. Anything that pops or erases from `Peer::chunks` must keep `sortedTail` (`PopChunk`, the request path).
- **Only live events are presented** (`opSeq > joinOpSeq`).
- **A comparison needs `Synced()`.** Drains (`vortex_server`, `vortex_bot`, the game, the test fixtures) wait until the
  client is `Synced()` (every chunk current, no request out, baseline ended, nothing held) at the server's last opSeq;
  the hash is over `SyncWindow()` (the client's sync box: its window inside the server's).

Entities [net-6, net-7a]
- **Entity events wait for their copy.** While the join's baseline is open, or for a NetId asked for, rest / wake /
  model events of an unknown NetId are held and applied after the copy if newer than its base (`Copy::base`); an event
  at or below a copy's base is dropped (the baseline holds it). `BaselineEnd` drops what nobody claimed. When a NetId's
  held events overflow (64) they are dropped and the model is asked for **again**, also with a request out (an answer
  already on its way may predate them); an answer older than the newest dropped event is discarded
  (`ClientStats::staleAnswers`). The server answers a `ModelRequest` whose answer is still queued with that one answer
  (`modelsQueued`), and disconnects a peer with more than `maxQueuedAnswers` queued.
- **The start gate opens on `BaselineNear`** (message 15): the baseline is sent nearest the spawn first (distance to
  each model's bounding sphere) and the marker follows the entities within `baselineNearMetres` (32 m); `BaselineEnd`
  opens it too. `Synced()` still waits for `BaselineEnd`.
- **Tombstones are bounded** (`ClientSettings::maxTombstones`, a FIFO): an evicted one can only let a stale answer
  re-create a dead copy, which the next EntityCheck and the list remove. The check is the tombstones' backstop: do not
  turn entity checks off in a session. A joiner starts with none (dead NetIds never reach it).
- **Every NetId destruction is an event.** The server connects `on_destroy<scene::NetId>` at `Start`; whatever
  destroys a replicated entity (Damage, `DestroyBelow`, eviction, `ClearBodies`, a player's crate limit) queues an
  `EntityDespawn` with `NetId::despawn` as its reason (set it before destroying: `sim::Despawn`), emitted by
  `DrainDespawns` after the cast or the tick. Never remove a NetId entity without letting the observer see it.
- **Only terrain operations charge the cast queue's event credit.** Entity events follow from Jolt (where a piece
  rests, which crate a blast catches, sleep): charging them would put a replayed cast on another tick (networking.md
  R5). They are counted (`ServerStats::entityEventBytes`). A `Detach` with a NetId is a terrain operation and is
  charged as before.
- **Entity events have a budget of their own** (`entityEventBytesPerTick` / `entityEventBurstBytes`), which decides
  only which bodies are replicated, never when a cast runs: while it is spent `TakePieceSlot` answers
  `OverByteBudget` (`ReplicationState::refuseNewPieces`, set around each `RunCast`) and a damaged object is let go
  `ToRubble` instead of an `ObjectModel`. Rests, wakes, despawns and placed objects are always sent.
- **The NetId of a piece is a server decision, the voxels are not.** `TakePieceSlot` reads Jolt's sleep state (the
  awake cap) and the tick's budget: a replayed server may give another piece the id, never other voxels.
- **Layers follow rest on both sides, and rest and wake are events.** A replicated body is in `Layer::Rubble` while it
  moves and `Layer::Dynamic` at rest, on the server (`TrackBodies` after physics, which emits `EntityRest` and
  `EntityWake`; `SpawnObjectAt` at creation) and on every client (a copy is Dynamic from its `EntityRest` to its
  `EntityWake`). A moving body that carries a player stays Dynamic: on the server any player's ground body, on a
  client the copy under its own player (`MoveCopies`). The session character mask is Static | Dynamic and characters
  never move bodies (`characterMovesBodies` false), so what blocks a player is a function of the opSeq on both sides
  (a player standing on another player's ride is the exception).
- **The rest rule.** A copy at rest is at the `EntityRest` pose bit for bit; samples of the rest's tick or older are
  refused (`RemoteBodies::Add`); a stale rest (older than a kept sample) leaves the copy moving. Samples never change a
  copy's rest state. Rest poses and rotations compare exactly in every test, and so do rest states (`Agree`).
- **A snapshot's samples wait for its events**, as its owner block does: they are taken after `ApplyPending`, before
  reconciliation, once `Snapshot::lastOpSeq` is applied. So a spawn, model, rest or wake before a sample is always
  applied first, and an `ObjectModel` shifts only old-frame samples.
- **The awake cap counts every awake replicated body** (pieces, crates, players' crates): they share the snapshots. A
  cast's terrain pieces ask for slots largest first; NetIds follow that order, the `Detach` order does not.

Checks and terrain mode [net-7b]
- **A check is compared at its stream position.** HashCheck, EntityCheck and EntityList ride the events channel and
  are built in `Flush` right after the peer's `Pump` (`SendChecks`; nothing while a test hook holds its events), so the
  client processes them with `applied` equal to their opSeq (else counted `checksOutOfOrder`, never compared). A
  HashCheck compares only chunks that are current with a base at or below its opSeq: a chunk whose data is ahead
  (base > C) holds events the check does not, a chunk not current is not trusted anyway. A mismatch is [net-7a]'s
  Requested state plus a `ChunkRequest{Repair}`: never a second path around the base rule or the ring.
- **The entity check compares only a settled set.** Skipped while the baseline is open, while a list is out, or when
  entity data ahead of it came from bulk (`entityAsOf`: the newest asOf of a `BaselineEntity`, `BaselineNear` or
  `BaselineEnd` processed); while models are asked for only the count and the ids (asked NetIds count as present). The
  list is ids and models (`EntityList` at Q): a copy not listed with base <= Q is dropped and tombstoned, a listed id
  without a copy or with another model is asked for (`ModelRequest`, which brings the exact rest state), a copy newer
  than Q is left alone. Never re-make a copy from a list entry: its rest / wake events already applied would be lost.
- **One `EntityList` per peer per `entityCheckTicks / 2`.** Requests in between are merged into the owed list
  (`entityListsMerged`); a list held by the gap counts once in `entityListsDelayed` (`Peer::listDelayed`) -
  `SendChecks` runs for every peer after every `Pump`, so a counter bumped there counts ticks, not lists, unless it is
  latched per event.
- **A check entry skipped while its chunk loads is compared later.** The server counts a pick as checked whether or
  not the client could compare it; an entry whose chunk is NoBase or Based and not loaded keeps the server's hash
  (`ChunkSync::CheckDeferred`, at most 256) and is compared once the chunk is current with a base at or below
  `applied`. It is exact only because **every stream operation whose chunk range holds the chunk drops the entry**
  (`ChunkSync::Apply`, before anything else): never compare a kept entry after an operation that may have changed the
  chunk, and never keep one for a chunk whose data is on its way (Awaiting, Requested, Unsynced: the server's anyway).
- **A repair is never looped on** (the plan's choice 17). A chunk that differs again within `kRepeatRepairChecks` (5)
  HashChecks of its repair, or after `kMaxRepairsPerChunk` (3) repairs, stays current (the player is not held over it
  every 2 s), is logged once as an error and counted (`repairsRefused`, then `stuckMismatches`). It is still a mismatch
  for every exit rule.
- **Server terrain mode is the client's decision and sticky.** The client switches (probes differ at the `Welcome`, or
  `pristineFailuresForTerrainMode` distinct pristine chunks it generated differently) and from then on every
  `WindowUpdate` carries `kTerrainMode`; the server only follows the flag and ends a peer that clears it. A terrain-mode
  update is answered by a `TerrainAck` in the ack's place (after `Pump`, processed at its opSeq): every entering chunk
  of the server's window coded by the server's cell state (`TerrainCode`); the data is [net-7a]'s ChunkData path,
  modified or not. The `SessionGenerator` answers Empty inside the server's window, so nothing this client generates
  there is ever trusted (every chunk it bases counts as from data: no pristine check can fail it again). Outside the
  server's window it generates as before (never edited, never sent).
- **Packing follows the server's state in terrain mode.** A ChunkData is installed by `InstallMode`: `Content` for data
  the server holds Dense (shown, never packed, not an edit - an `Edit` would reveal every buried neighbour),
  `HiddenContent` for data it holds Packed (packed at once, no slot, made resident near the focus and packed again by
  the server's bit, never by this world's column classes, which come from the generator that differs) - only if no
  operation over the chunk or a face neighbour came between its ack and the stream's position, no face neighbour is an
  edit here, and no skipped operation later shows it (next rule), otherwise `Content`. Repairs and re-requests are
  `Content`; [net-7a] clients keep `Edit`. Packing is never a content change: modes differ in slots and drawing, never
  in hashes.
- **A server-hidden chunk is shown whenever the server may show it.** Installed `Content` instead of packed when a face
  neighbour is an edit here (`World::SetChunkEncoded`) or an operation near it came between its ack and the stream's
  position (`OpRing::TouchesNear` up to `max(asOf, applied)`); and in terrain mode an operation that skips a chunk (not
  current, or its data ahead) shows that chunk's server-hidden face neighbours for good
  (`World::ShowServerHiddenNeighbours`): the neighbour's later data is a `Content` install, never an edit, so nothing
  else would. `vortex_server` fails a run where a terrain-mode bot holds packed a chunk the server shows.
- **Slots for Unsynced requests are what is left after the content still to come** (NoBase, Awaiting, Requested
  records: `ChunkSync`'s incoming count). A window step frees the leaving slab's slots before the entering slab's data
  arrives; asking for Unsynced chunks with them made a request / StoreFull loop in terrain mode. Hidden data a packing
  world installs packed takes no slot and is not counted (`Record::incoming`, `ChunkSync::Recount`: every change of a
  record's state or `hidden` bit goes through `SetState` / `Recount`).

The server's tick and the cast schedule
- **The cast schedule is a function of counts.** `CastWorkOf` may read only counters that follow from the flood box's
  voxels and the operations' integers. `graphsBuilt`, `chunksDecoded`, any time and the object half of a blast are
  never inputs (`CastBudget.ScheduleFollowsCountsNotTimeOrCache`).
- **The server's world never meets an unloaded chunk.** Window = play bounds + `kServerWindowMarginChunks` (3)
  chunks, loaded whole at `Start`; a cast is queued only if `sim::CastFitsRegion` (centre in the play bounds, radius
  <= `kMaxCastRadiusQ`, carve inside the rows). An island search error, an incomplete operation or a full store marks
  the server `Failed`, and **`Failed` is terminal**: peers are disconnected, nothing more is made or sent (not even
  the rest of the failing cast), new peers are disconnected on connect.
- **Nothing in the schedule reads a clock.** `CastRecord::seconds` and the `ServerStats` cast times are logged only.
- **A cast is judged at its input's tick, its ray at its dequeue** [net-5]. On time: the state after the tick's step;
  late: the state of that tick from the player's 32-tick ring, the cooldown then and now both 0, the cooldown set as if
  taken then. The sphere is unknown until `RunCasts` pops the cast and `ResolveRay` casts it into the world as it is
  then; a ray that casts nothing costs nothing. `lastCastTick` / `castRejected` in the owner block tell the owner.
- **A late cast rewrites the cooldown history.** Accepted at tick T, the kept states of T.. get `full - j`, so a later
  late edge is judged as its client predicted it. Ring entries never written have the tick `0xFFFFFFFF`, and inputs
  before the player's join tick are ignored (`inputsBeforeJoin`).

Players and the client clock [net-4, net-5]
- **One movement rule.** Server, predicting client and offline game move a player only with `sim::StepPlayer` from
  integer inputs; anything that changes how a player moves goes there (and into `PlayerTuning`, which travels in
  `Welcome`), never into one side, and bumps `sim::kPlayerRuleRevision` (in `BuildHash`) when the same inputs give
  another result. Server and client pass `sim::PlayAreaOf(region, tuning)` (the server from its session, the client
  from `Welcome`, set before the first predicted tick and used by every replay) and put its walls into their physics
  (`sim::SetPlayAreaWalls`: the server after `Initialize`, the client in `Attach`); the offline game passes
  `sim::PlayArea{}` and has no walls. The rest of a step is the session's too: the tick length and gravity travel in
  `Welcome`, the character mask is `sim::kSessionCharacterLayers` on both sides (`Attach` refuses anything else), and
  both sides call `TerrainColliders::FlushBox(sim::PlayerReach(feet))` before every step (the client also before every
  replayed step and restore), so the colliders a step meets follow the edits applied, never worker timing.
- **Button levels, derived edges.** Inputs carry levels; `PressButtons` derives edges against
  `PlayerState::lastButtons`. A repeated input carries the levels the state already has, so it never makes an edge; a
  late input refreshes them and its edges count once (the cast in it is captured then).
- **The clock hears about the inputs it made.** A snapshot's margin is about `marginTick` (the newest input of the
  packet with the newest inputs, dropped ones included), never `ackInputTick`: a client too early has nothing
  acknowledged. Ticks at or past the client's next tick, and margins beyond +-1024, are forged and ignored.
- **A snapshot is compared only after its events.** `Snapshot::lastOpSeq` <= `AppliedOpSeq` before the comparison.
  Snapshots wait in arrival order; the newest whose events are applied is compared (older ones are dropped), and one
  that has waited `eventWaitTicks` since it arrived is given up and counted - its age never restarts when a newer one
  comes.
- **A joiner starts predicting from a fresh snapshot**: at most `startFreshTicks` behind the newest one received (else
  it waits, at most `startWaitTicks`). A prediction started far in the past sends late inputs: a clock jump, then a
  hard resync.
- **Inputs sent are never taken back.** A hard resync holds the clock; nothing rewinds `PredictTick`, except a
  snapshot at or past it (`behind`), which moves it forward.
- **Every tick the clock hands out is run.** `Client::TicksDue(seconds, maxTicks)` keeps what is over `maxTicks` for
  the next calls; an owner that can run only so many per frame asks for that many (the engine:
  `Engine::kMaxSourceTicksPerFrame`, 8). A tick handed out and dropped is lost to the clock.
- **`Attach` attaches only a live session.** Its callback polls; if the session ended meanwhile (Closed, Failed,
  Refused) the load stops and `Attach` returns false - never Playing on a dead connection.
- **Only `Enet.cpp` sees ENet.** `vortex_enet` is PRIVATE to `VortexNet`; Core, Physics, Sim and `VortexSimTests` must
  not link it (`vortex_assert_no_network`).

Exit rules of a session client
- **A headless game client** (`--connect --headless`) drains at exit (applied == the server's opSeq, held 1 s, at most
  5 s) and exits 1 when it cannot: the hash it logs is then comparable with the server's last one. It also exits 1
  when its own checks found a mismatch, a check came out of its stream position, or it played in server terrain mode
  without `--other-terrain` (`net::ClientCheckFailures`, called from `../src/Session.cpp`). `vortex_bot` applies the
  same failures through `net::JudgeBot`, plus the `BotGates` (it predicted, cast, dropped crates and walked as told).

## 3. Traps

Clocks and passes
- A wall-clock owner of a `Bot` (or a `Client`) must give `TicksDue` the time from one pass's **start** to the next
  (`Bot::PassOnClock`) [net-8]: taking the time after the pass leaves its work out, the client's clock runs slow and
  jumps forward every second (`vortex_bot` did, with inflated corrections in the soak;
  `Bot.PassOnClockCountsTheWorkInsidePasses`). Only the pass that loaded the region counts from its end.
- A wall-clock loop may run no tick in a pass: anything scheduled "on tick T" must remember that it ran (the drill
  queued a step on every pass of the same tick once).
- `TicksDue` may return 0 (bleed, hold) or several ticks (catch-up, jump): a harness that polls only inside client
  ticks must still be fine without a poll for a pass.
- Other players are drawn at `ServerTime`, which advances only in `TicksDue` (real time): a harness that ticks a client
  without calling `TicksDue` sees them stand at the first snapshot's tick.
- `TickClock::Report` takes ticks the client made; feeding it non-increasing input ticks makes every report a repeat
  (ignored) - a test bug that once hid what a clock test was meant to show.
- Comparing prediction and server position is meaningful only while the player stands: a walking client is ahead by
  its lead (about 0.5 m at walk speed). The tests stop the controls first (`Settle`).
- `vortex_server`'s in-process bots share one thread. Over UDP (`--enet`) a bot's region load runs whole passes of the
  server and the other bots from `Attach`'s callback (and one after it), so it no longer stops them; but one thread
  still runs the server, the drill and every bot's simulation, and its margins swing (-30..+28 ticks in the UDP
  drill, measured before stage 8). Judge the clock with game
  clients or `vortex_bot` (separate processes), not with the bots of `--enet`. For the same reason latency over
  `--enet` is stalls, not stream lag: a joiner's events made while its own world loads wait for the load, and the
  server then catches up two ticks a pass. Read latency on the loopback runs (`--max-join-p99-ticks` only on the
  loopback; the p99 must be under N, as in the unit test).
- The p99 < 250 ms join target cannot be met on a 5 %-loss link at any rate: a lost reliable packet is resent after
  about 130 ms and holds the ticks of events behind it. The unit test asserts it at 1 % loss.
- `BulkPacerSettings::deliveryInterval` is half a second on purpose: a client that stalls and then drains its inbox (a
  loopback test's joiner loading) makes one short interval look faster than the link; a quarter second broke
  `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing`'s live-latency bound. The pacer's RTT is
  `EnetTransport::RoundTripSeconds`, the least of ENet's smoothed RTT and its 5 s epoch minima: a queue shows only once
  it has lasted an epoch, so do not size the window by the RTT signal alone (an additive increase would fill a slow
  link's queue before seeing it).

Scripts, gates and log lines [net-8]
- A gate that only makes a run *easier* when it fails is still a gate. `JudgeBot`'s `BotGates` (acted share, casts,
  crates, the walk) and the soak's drill / joins / repairs checks exist because a bot that stopped predicting or a
  drill report that counted the wrong casts (the cast record held only `drillSteps` records: players' casts and
  precasts pushed the oldest drill casts out) passed silently.
- **A slot does not name a client**: a slot is reused once its client left. The exit line (`SessionHashLine`) and the
  server's leave line (`ServerLeaveLine`) both end `joined at opSeq J, tick T` (`Client::JoinOpSeq` / `JoinTick`,
  `Server::ClientJoinPoint`); a script matches a client to its leave line by slot **and** join point
  (`tools/soak.sh` matched by slot alone and compared a bot that left early with the late joiner's line).
- A script that checks a log line's value checks the value the code prints (`DescribeTerrainMode` says exactly `no`;
  the soak looked for words that line never holds), and awk's `NR == FNR` is true for the second file when the first
  is empty (use `FILENAME == ARGV[1]`).
- A test of the relay's outage judges by the relay's own clock (`RelaySettings::onAdmit`: the time the relay read each
  datagram and whether it dropped it), never by when another thread stamped, sent or read it.
- A long-running server keeps no per-pass or per-tick vector (`DurationHistogram`, fixed buckets).
- `EntityRestHash` is not a tolerance: it relies on [net-6]'s bit-identical rest poses (an ulp across a rounding
  boundary changes it). Objects that are not resting are compared by the scripts from the awake lines with 5 cm of
  tolerance.
- `net::DrainWatch` accepts a copy that is not resting if it stands within 1 mm for the hold time: a server body
  frozen while it moved (over terrain that cannot get a collider: below the window's bottom row) stays awake for good
  and never sends an `EntityRest`. `vortex_server` lists such bodies after each leave (`not resting: ... frozen`).
- A player's crates go when it leaves (`DropPlayer`); a crate is refused at the awake cap. So a departure makes
  events: what a departing client drained to is the opSeq before them, which `Server::SetOnClientLeft` reports
  (`vortex_server` logs its "client left" line there; `tools/session-check.sh` compares at it).
- `vortex_server --drill-every 6` (the default) on terrain rebuilds a 12.5 m site ten times a second; game clients
  beside it (`tools/session-check.sh`) apply those edits and may show 0-2 clock jumps / resyncs from their own frame
  time, no corrections (the server's longest pass is logged: `Server: longest pass`).
- "Synced after" is not a join time for a walker: its window keeps entering chunks to generate, so the first moment
  every chunk is current comes only once it stops. The join's own end is `ClientStats::joinDataTicks` (baseline ended,
  no chunk waiting for the server: none without a base, awaiting, requested or unsynced); `vortex_server`'s `Latency:`
  join windows end there. The `Join:` lines' "synced after" is noted every 16th Poll and whenever `Client::Synced()` is
  asked (a log value only).
- `ClientStats::chunkDataRepeats` counts every `ChunkData` for a coordinate received before (changed chunks too);
  `chunkDataUnchanged` only those whose RLE bytes equal the last ones received (the waste of re-sending; O3).
  `vortex_server --bulk-window-floor KIB` fixes the window (floor = ceiling) and `--pacer-delivery-interval 0` restores
  the [net-7a] pacer, for measurements, never for play.

Server costs and caches
- Never call `Terrain().Flush()` (a global barrier: every pending re-cook of the window) in a server tick for one peer
  [net-8]. A join relies on the spawn being a permanent collider focus (`Server::Impl::ColliderFocus`, cooked in
  `Start`) and on `StepPlayers`' `FlushBox(PlayerReach)`; the slow join pass of the owner's run was that flush.
- `Server::Hash` caches each chunk's hash by `World::ChunkRevision` [net-8]; a path that changes a chunk without
  bumping its revision (the world's rule since [shred-4]) would also leave a stale hash in the leave lines. Changed
  chunks are re-hashed 32 per tick (`RefreshHashes`), so a leave costs lookups.
- The server keeps each object's model hash with a weak pointer to its model (`modelHashes`): models are never changed
  in place - if one ever is, that cache returns a stale hash and every client mismatches. `voxel::ChunkHash` skips runs
  eight voxels at a time; both keep a check's cost low. Checks are staggered (17 ticks per slot, entity checks half a
  period later), so the cost is per tick, not per player count up to 60 peers. The `Checks:` and `Ticks:` lines give
  the costs; reference values are in ROADMAP "Performance".
- `OpRing::CoversChunk` trusts `sim::OpChunkRange`: an operation whose range missed a chunk it changes would let a
  loading chunk replay without it. Ranges over 4096 chunks are not listed (every older base is uncovered).
- Never let an `OpRing` that no longer covers a base answer "everything": under 10 casts/s (with the drill's rebuilds)
  the 1024-operation ring wraps in about 9 s, and both fallbacks that did so broke the arena soak (every loading chunk
  requested whole; every terrain-mode install shown, a full store). `CoversChunk` and `TouchesNear` consult the record
  of what the dropped operations touched (`m_DroppedTouch`, one entry per chunk ever touched by a dropped operation:
  bounded by the region's chunks; an operation over 4096 chunks makes everything older unknown).
- Growth over a session: the modified map (one entry per chunk ever changed, sparse) and the tombstones; the event
  backlog is trimmed every tick to the slowest joined peer; `CastRecord`s are a ring of `castRecordLimit`.
- Stepping back and forth at the edge of a modified area re-sends its chunks each time they enter (chunks that leave
  are dropped, `keepUnloadedEdits` false). The follow slack cut it by about a third; a cache of the slab last left
  would cut the rest (networking.md "Open points").

Chunk sync
- The HashCheck planner [net-7b] puts pristine chunks on the player's path first: a chunk whose box lies within
  `pathMetres` (1.5 m) of the way ahead. A walker on a chunk-row border has two rows on its path, so the way-ahead
  slots cover half the distance: 12 pristine slots (10 for the way) keep a 7 m/s runner checked ahead
  (`Region250.AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt`). Fewer slots or a ranking that lets chunks
  behind the feet "arrive first" let a broken chunk pass unchecked.
- A terrain-mode join is large and paced (the whole window as `ChunkData` through the pacer's window, at most one
  hand-over per tick): the start gate waits only for the 3D-nearest data; `Synced()` and every drain wait for all of
  it (`vortex_server`'s drain waits up to 3 min for a terrain-mode bot). Sizes and times are in ROADMAP "Performance".
- A terrain-mode client's own column classes still place the sky and rock rows of the server's window (they are cheap
  and the ack overrides them): a `TerrainAck` code 2 over a cell that holds another uniform content installs it
  (`uniformInstalled`); a column job finishing after an install is discarded (the install bumps the cell's
  generation).
- A pristine chunk that is never checked stays as this client generated it: a standing player's checks reach 12 m
  around it, a walker's its way ahead. `vortex_server --bot-broken-chunk` therefore breaks a chunk within 12 m of the
  spawn and makes that bot stand.
- The bulk channel is not ordered against the events channel: a `BaselineEnd` can arrive before the `Welcome`. The
  client keeps bulk from `Joining` on (a Match-fixture join hung on a dropped `BaselineEnd`).
- A `WindowAck` is processed in stream order with the batches (it waits in `pending`); processing it early would base
  pristine chunks before the events before it are applied (harmless for content, but the replay then has work).
- A moving window is `Synced()` only once every chunk of it has loaded, caves included: on the arena that is many
  seconds of a bot's workers after it stops. Drains of a moving window wait for the world's loading on the wall clock
  (`vortex_server`), or stream while they wait (the game's drain; it does not run the engine's streaming otherwise).
- `Synced()` walks the window's records (101 376 for the game's) unless something is Unsynced or owed:
  `SyncHousekeeping` looks at it every 16th `Poll` until the join has synced once. Do not call it per tick elsewhere.
  `TakeRequests` walks them only while some record is Unsynced (`ChunkSync::SetState` keeps that count: every state
  change goes through it), and a window step visits only its entering columns (`ChunkSyncStats::recordsVisited`).
- Bulk installs are bounded per `Poll` (`ClientSettings::maxBulkPerPoll`, 64, a count so runs stay reproducible); the
  rest wait in `pendingBulk` in arrival order. Chunks that catch up together (one `WindowAck`, one load report, one
  `Poll`'s consecutive `ChunkData` via `ChunkSync::OnChunkData(span)`) walk the op ring once (`OpRing::Walk`): each op
  is decoded once per batch, not per chunk (`ChunkSyncStats::opsDecoded`; an earlier 16-entry FIFO cache never hit
  once a batch shared more than 16 ops). Keep catch-ups batched; per chunk the ops keep stream order. Costs are logged:
  `ClientStats::syncSeconds` / `bulkSeconds` on the `Join:` lines, the server's `encodeSeconds` on its `Join:` /
  `Sync:` lines.
- `Client::MakeSimulationConfig` raises `chunkCapacity` to the region's cell count: fine for a CPU `Simulation` (slots
  are allocated on use), not for the engine's GPU store, which mirrors `Capacity()`: the game passes `storeLimit`
  (12 288) and `ClientSettings::minFreeStoreSlots` (2048), and `Attach` refuses a region that does not fit [net-5].
- Trap for store tests on the flat region (`Field`, `Plain250`): its surface row is Dense when pristine (grass over
  dirt), so the window holds a constant number of slots as it moves and crater chunks overwrite slots they already
  have; a chunk that needs a slot of its own is one that is Empty when pristine (a box in the air).

Entities
- `PhysicsConfig::characterLayers` is `sim::kSessionCharacterLayers` (Static | Dynamic) on a server
  (`sim::ServerConfig`) and a client (`MakeSimulationConfig`), with `characterMovesBodies` false and, on a client,
  `SceneConfig::rubbleLayer`; `Attach` refuses another mask, tick length, gravity or those flags. A client simulation
  built by hand without `MakeSimulationConfig` is refused (with the default mask the client's own rubble blocked its
  prediction: many corrections in the drill).
- Copies move only in `Client::StepPlayer` / `RunTick` (`MoveCopies`, one tick each): a harness that polls and steps
  the simulation without them sees copies stand at their spawn until their `EntityRest` (the `Session` fixture of
  `SessionTests.cpp` does that; `Arena` and `Match` do not).
- A client's copy removed by its own scene (`DestroyBelow`: drawn in the past it can fall out first) is dropped and
  counted (`ClientStats::copiesLost`); the entity check shows it missing until the server's despawn comes.
- The `Arena` / `Match` harnesses are reproducible only because colliders install in chunk order with a per-round job
  budget (`notes/physics.md`) and the harness waits for idle between ticks: a harness that does not wait sees worker
  timing again (`Entities.OneScriptGivesTheSameBodiesTwice`).
- A player's cast at dequeue takes the nearer of the terrain and a replicated body (`PhysicsWorld::Raycast` with
  Dynamic | Rubble; every server body there is replicated); the game's puff takes the nearer of the terrain and a
  copy.
- A falling replicated piece passes through players (it is in Rubble) and may come to rest overlapping one; the
  character is then inside a Dynamic body on both sides and walks out (Jolt's penetration recovery), corrected once at
  most (`PlayerUnderFallingPiecesAt150ms`). Crush damage is planned with shred.md SI-2.
- `Client`'s character belongs to the simulation; the client never destroys it (the simulation may die first).

Transports
- `ImpairedTransport::Disconnect` drops queued packets: refuse first, disconnect once `ReliableBytesInFlight` is 0
  (the server does).
- `LoopbackTransport::ReliableBytesInFlight` counts everything still in the other side's inbox: a peer that never polls
  keeps it above 0 (its sends stay held by the pacing; a refused one is closed after `closeGraceTicks`).
- `ImpairedTransport` draws every peer's losses from one random stream unless `Impairment::rngPerPeer`: two runs that
  differ in one peer then differ in every peer's losses (a latency comparison across runs is noise without it).
- ENet services the socket only inside `Poll` (and sends in `Flush`): an owner that stops polling for longer than
  `EnetSettings::timeoutMinimumMs` (10 s) loses its peers. A long load goes through `Attach(simulation, whileLoading)`
  with a callback that polls.
- `ENetHost` has no user pointer: the receive-loss hook finds its transport in a process-wide map (main thread only).
  A transport's per-peer records must outlive the host (packets point at them): the destructor destroys the host first.
- `EnetTransport::Disconnect` reports `Disconnected` at once and forgets the peer; ENet's own disconnect event for it
  later only frees the record. Do not expect a second event.
- The ENet RTT includes the remote's frame time (it acknowledges when it is serviced): a 40 fps headless game client
  shows 33-44 ms on localhost. ENet starts its smoothed RTT at 500 ms and moves it 1/8 per acknowledged reliable
  message; a client sends few, so it stayed near 440 ms for seconds and the lead taken from it held the clock in a hard resync. `EnetTransport` seeds it with the connect handshake's round trip
  (connecting side), and `RoundTripSeconds` is the lowest of the smoothed value and ENet's lowest of this and the last
  5 s interval (a stall of this side raises only the first); `SmoothedRoundTripSeconds` is ENet's own (the HUD's ping).
- ENet's `host->maximumPacketSize` limits what a peer may send here **and** what `enet_peer_send` takes: a listening
  transport sets it to 64 KiB (`kListenMaxMessageBytes`; strangers' messages are reassembled before any token) and
  `Send` lifts it for the call. A refused large message is dropped unacknowledged, so its sender times out.
- The game services its connection from a helper thread while `Engine::Initialize` runs; nothing else may touch the
  client or the transport until that thread is joined (`EnetTransport` is one thread at a time, not thread-safe).
- Players do not join in `PeerId` order: a bot's slot depends on whose `Hello` arrived first. Tests take the slot from
  `Client::Slot()`.

Regions, templates and tests
- `voxel::kGeneratorGoldenHash` must be updated whenever `Terrain.GoldenChunkHashes` (or the noise / height goldens)
  are regenerated; `Terrain.GeneratorGoldenHashNamesTheGoldenTables` prints the new value.
- `PlaygroundTemplate(world, ids, loadSite)`: `loadSite = true` moves a camera window to the site (the offline game);
  on a server it would move the fixed window off the region - pass false (`BuildRegionTemplate` does).
- Comparing the fixed step: `SimulationConfig::fixedTimeStep` is a float (0.016666668); `1.0 / 60.0` in double is
  smaller, so `Advance(1.0 / 60.0)` may run no tick. Tests use `FixedTimeStep()`.
- A dud next to an overloaded structure releases it (the load check runs after every blast, power 0 included): a test
  that warms the graph cache with a dud must do it where nothing is overloaded (`CastWorkIsEqualColdAndWarm` uses the
  mushroom).
- A World edit that reaches a row above or below a full-height window reports those chunks as `skipped` (they are not
  cells), even for a carve that would change nothing there: on a server that is a failure, hence the carve-box rule in
  `CastFitsRegion`.
- Test hooks: `Server::SetMaxBulkBytesPerSecond` holds data back (1 B/s lets one message through, then nothing for
  minutes); a test moves a client's window itself with `Simulation::UpdateStreaming(focus, ...)` and `Poll` instead of
  `RunTick` (`ClientWindowTests.cpp`'s `manual` bots).

## 4. Recipes

- New cast kind / loadout: the loadout id is in `QueuedCast` only (0 = the tuning's cast); a second one needs a wire
  field in `PlayerInput` (and `kInputBytes`), its radius / power in the tuning, `kProtocolVersion` + 1, and a
  `Movement.*` test that a late cast of it is judged at its tick.
- New player rule (crouch, sprint, a second jump): `sim::StepPlayer` and `PlayerTuning` (+ `WritePlayerTuning` /
  `ReadPlayerTuning`, `sim::IsValid`), a button in `PlayerButton` if it needs one (wire format, next free bit 16),
  `kProtocolVersion` + 1 (or, with no byte changed, only `sim::kPlayerRuleRevision` + 1), a `Player.*` test, then `Movement.NoCorrectionAbove2cmOnFlatGround` must still see 0
  corrections.
- New event kind: value in `EventKind` (or a new `WorldOpKind` 7+), `WriteEvent` / `ReadEvent`, a size line in
  `Wire.EveryWorldOpRoundTripsAtItsSize` (`Wire.EntityEventsRoundTripAtTheirSizes` for entity events),
  `kProtocolVersion` + 1 (and its comment), the client's `Apply` / `ApplyEntity`, and `MaskVoxels` in `Server.cpp` if
  it carries a volume (the batch decoder's voxel budget). A terrain op also goes through `Server::Impl::EmitOp`, gets a
  correct `sim::OpChunkRange` and a `CastWorkOf` weight, and is covered by the base rule and the ring like the others.
- New message: a value after 20 in `MessageType`, `Encode` / `Decode` with a size check in `WireTests.cpp`,
  `kProtocolVersion` + 1. A check-like message rides the events channel and is processed at its opSeq.
- New kind of server body (a thrown object, a spell's matter): create it, `sim::Replicate(simulation, entity, piece)`,
  then the server's `EmitObjectSpawn`; it is then in snapshots, rests, despawns and the entity check like a crate. A
  piece counts against the piece caps (`piece = true`), a placed object does not.
- New region template: a value in `RegionTemplateId`, a case in `BuildRegionTemplate` and `sim::IsValid`, a keyword in
  `ParseSession` / `FormatSession`; it may read the loaded world but must not change it.
- Re-calibrating the cast work: `VortexNetTests --gtest_also_run_disabled_tests
  --gtest_filter=CastBudget.DISABLED_CalibrateWorkAgainstTime` on a quiet machine; the CSV columns are in the test's
  comment; aim at about 1 ms per 10 000 units and record the numbers in ROADMAP "Performance".

## 5. Verifying a change here

- Gates G1 and G2. The suites of this area: all of `VortexNetTests` (`Wire`, `Transport`, `Client`, `ClientWindow`,
  `Session`, `SessionAcceptance`, `SessionFile`, `CastBudget`, `Clock`, `RemotePlayers`, `RemoteBodies`, `ServerTime`,
  `Movement`, `Entities`, `ChunkSync`, `BulkPacer`, `HashCheckPlanner`, `TerrainMode`, `Region250`, `Enet`,
  `EnetSession`, `Bot`, `Relay`) and in `VortexSimTests` `Player.*` and `Replication.*`. The ENet and relay tests use
  UDP on 127.0.0.1 and the wall clock; the relay tests run serially.
- G11 (the lossy drill): `hashes equal`, every drill step frees terrain, every `Entities: client N: ... - agree`, the
  join p99 under 15 ticks, no `Checks:` mismatch. Its `Client N movement:` lines give each bot player's corrections,
  snaps and clock jumps / resyncs; `Objects:` the bodies, rests, wakes, despawns by reason, caps and snapshot bytes;
  `Join:` / `Latency:` / `Sync:` the joins. Add `--bot-casts 90` for bots that cast, `--outage-ms A,B` for an outage,
  `--enet` for bots on their own UDP sockets (the clock is not judged there, traps).
- G12 (the arena preload), G13 (the arena walk: hashes over each bot's sync box, entities, the store bound, the walk),
  G14 (server terrain mode: bot 1's `Checks:` line ends `its broken generator put it in server terrain mode`; the
  `Terrain: bot N:` lines give the join's bytes; a `Terrain: bot N packing:` line fails the run when a chunk the
  server shows is packed there), G15 (a headless game client on the arena: its `world hash ... over [box]` equals the
  server's `window hash ... over [box]` at the same opSeq; `--frames 15` shows the `JOINING` line). Every
  `vortex_server` run logs `Checks: bot N: ...` (`net::DescribeChecks`) and `Checks: server: ...` and fails on a
  mismatch where no fault was injected; `--bot-broken-chunk 1` (on the playground: `--drill 30 --clients 2`) shows one
  repair end to end. Every run logs `Ticks: server: ...` (wall clock; `ServerStats::tickHistogram`).
- G16 (`tools/session-check.sh` in the game repository): `vortex_server` on 127.0.0.1 with a fresh token and two
  headless game clients; `PASS` only when all three exit 0, both clients' drained hashes and entity lines equal the
  server's at the same opSeq, and both clients' `Checks:` lines report HashChecks compared, 0 hash and entity
  mismatches and 0 checks out of order (logs and screenshots in `build/session-check/`: look at them).
- G17 / G18 / G19 (the soak, `tools/soak.sh` in the game repository): each bot is a `vortex_bot` process behind its own
  `UdpRelay`; its `Latency:` line needs the server's `--timeline` file (one host). `GAME_CLIENT=1` adds a headless game
  client that joins once the bots are ready to leave (compared like a bot; look at `game.png`); `TERRAIN=1` (default of
  the long run) adds a standing terrain-mode bot. `SOAK_SELFTEST=1` checks that the normal verdict names an undeclared
  fault, a killed bot, a bot that leaves early without draining and exits 0 (`vortex_bot --test-skip-drain`) and a
  missing server line. Never PORT=27015 (the scripts refuse it; `vortex_bot` needs an explicit port and an end).
- `VortexNetTests --gtest_also_run_disabled_tests --gtest_filter=Movement.DISABLED_FlatGroundCorrectionRateOverSeeds`
  prints the flat-ground correction rate over 10 seeds (a measurement; earlier values are in
  `docs/history/notes/net-2026-11-18.md` and `docs/history/networking/stage-4.md`).
- The game's standard headless run (G3) must still log `Beam cut: ... 1 new object(s)` (the plaza comes from the
  template).
- Windowed sessions (two machines, the HUD's ping, how corrections feel) are the owner's to judge: say so.
