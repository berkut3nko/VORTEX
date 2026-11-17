# Notes: sessions (`src/net`, `src/sim/Region.cppm`, `tests/net`, `tools/server`)

Read `CLAUDE.md`, `docs/ARCHITECTURE.md` 7.5 and `docs/design/networking.md` ("Stage 3 as built", "Stage 4 as
built", "Stage 5 as built", "Stage 6 as built", "Review of stage 6", "Stage 7a plan" with its amendments, "Stage 7a as
built", "Stage 7b plan" with its amendments, "Stage 7b as built") first; nothing from them is repeated here. Paths
are relative to `engine/`. Written at networking stage 3, extended at stages 4, 5, 6, 7a and 7b (M-a, M-b) and the reviews of
stage 7b (protocol 10) and of
stages 5, 6 and 7a. The game's session
mode is `../src/Session.cpp`.

## 1. Map

- `src/net/Net.cppm` - primary unit of `vortex.net`; re-exports the partitions below.
- `src/net/Transport.cppm`, `internal/Transport.cpp` - `ITransport`, `TransportEvent`, `Channel`,
  `LoopbackTransport` (shared inboxes, `Connect(a, b)`), `Impairment` / `ImpairedTransport` (send-side queue sorted by
  due time; reliable packets keep per peer-and-channel order through `m_LastDue`).
- `src/net/Enet.cppm`, `internal/Enet.cpp` - `EnetTransport` (stage 5: UDP through ENet; the only file that
  includes `<enet/enet.h>`), `EnetSettings` (timeouts, test receive loss), `EnetStats`, `ParseEndpoint`,
  `kDefaultPort`.
- `src/net/Wire.cppm`, `internal/Wire.cpp` - `ByteWriter` / `ByteReader`, mask runs, `Event` = `variant<WorldOp,
  EntitySpawn, EntityDespawn, EntityRest, ObjectModel, EntityWake>` (stage 6: `ObjectSpawn`, model runs `WriteVolumeRuns` /
  `ReadVolumeRuns`, `VolumeHash`), `WriteEvent` / `ReadEvent`, `WriteRegion` / `ReadRegion`, `QuantizeVelocity`.
- `src/net/Entities.cppm`, `internal/Entities.cpp` - `EntityInfo`, `CompareEntities`, `EntityListHash`: the entity
  check of the drill, the tests and `tools/session-check.sh` (stage 6; not the wire `EntityCheck` of stage 7).
- `src/sim/Replication.cppm`, `internal/Replication.cpp` - `AllocateNetId`, `Replicate`, `IsAwake`, `CountPieces`,
  `TakePieceSlot` (the piece budgets), `Despawn`; `ReplicationConfig` / `ReplicationState` are in `Simulation.cppm`,
  `scene::NetId` / `DespawnReason` in `scene/Components.cppm`; `sim::Blast` makes the server bodies (step 5).
- `src/net/Messages.cppm`, `internal/Messages.cpp` - message structs, `Encode` / `Decode`, `EncodeBatch` (events
  already encoded), `kProtocolVersion`; stage 4: `InputBatch`, `Snapshot`, `RemotePlayer`, `Welcome::player`.
- `src/net/Prediction.cppm`, `internal/Prediction.cpp` - `TickClock` (the client clock: jumps, bleed, catch-up,
  holds), `ServerTime` (the server tick other players are drawn at; only moves forward), `RemotePlayers` (other
  players drawn in the past) and `RemoteBodies` (stage 6: replicated bodies drawn in the past, the rest rule).
- `src/net/Sync.cppm`, `internal/Sync.cpp` - stage 7a's chunk sync pieces: `SyncBox`, `RegionSyncBox` (the server
  window), `ChunkCoordHash`, `OpRing` (the client's ring of applied terrain ops as wire bytes), `ChunkSync` (the
  client's per-chunk states and bases, the base rule, replay, requests; stage 7b: `OnHashCheck`, `TakeRepairs`; review
  of 7b: `CheckDeferred`), `ForEachEnteringChunk` (the cells a window update enters with: client and server, wire order),
  `BulkPacer` (the server's per-peer bulk window), `HashCheckPlanner` (stage 7b: which chunks a peer's HashCheck holds).
- `src/net/Session.cppm`, `internal/Session.cpp` - `SessionConfig`, `ParseSession` / `LoadSessionFile` /
  `FormatSession`, tokens, `BuildHash`, `ProbeChunks` / `CheckProbes`, `RegionHash`, `BreakChunks` (stage 7b: a
  generator that makes some chunks differently, for tests and `--bot-broken-chunk`), `SessionGenerator` (stage 7b M-b:
  the client's generator, Empty inside the server's window once server terrain mode is on).
- `src/net/Server.cppm`, `internal/Server.cpp` - `ServerSettings`, `CastRecord`, `ServerStats`, `net::Server`
  (event backlog, peers with send cursors, the modified map, per-peer bulk queues and pacers, the entity baseline, cast
  queue and the credits).
- `src/net/Client.cppm`, `internal/Client.cpp` - `ClientSettings`, `ClientState`, `ClientStats`, `PredictionStats`,
  `net::Client` (events; the own player's prediction, input sends and reconciliation).
- `src/net/Drill.cppm`, `internal/Drill.cpp` - `BlastDrill`: mushroom / bar / neck tower per step, sites of 100 x 100
  voxels in the play bounds, a crate in each blast and one per site in the margin (stage 6), `KeepSites`;
  `RunDrillStep` (clear the site's bodies, build, crates, cast); `BotPilot` / `PilotSettings`: seeded controls of a
  bot player (`crateEveryTicks`: the E key).
- `src/sim/Player.cppm`, `internal/Player.cpp` - `PlayerInput` / `PlayerState` / `PlayerTuning`, `StepPlayer`, the
  quantisers (the movement rule of server, client and offline game).
- `src/sim/Region.cppm`, `internal/Region.cpp` - `RegionDesc` and its helpers, templates (`PlaygroundTemplate` is the
  game's plaza, moved here from `src/main.cpp`).
- `src/sim/Blast.cppm` (end of the constants) - `CastWork`, `CastWorkOf`, `OpWork`, the weights.
- `tools/server/main.cpp` - `vortex_server`; `tools/server/sessions/*.session` - examples that `SessionFile.*` parses.
- `tests/net/` - `EnetTests.cpp` (stage 5: real UDP sockets on 127.0.0.1, wall clock; the GPU-free acceptance
  `EnetSession.*`), `WireTests.cpp`, `TransportTests.cpp`, `ClientTests.cpp` (`FakeServer`: a client against a
  hand-written server endpoint - overlaps, gaps, forged `Welcome`s, late-join presentation, refused worlds),
  `SessionTests.cpp` (the `Session` fixture: a server and bots on impaired loopbacks driven by one manual clock; the
  acceptance; pacing; the disabled calibration), `ClockTests.cpp` (`TickClock` and `RemotePlayers` from margin and
  sample sequences; `RemoteBodies`), `MovementTests.cpp` (the `Match` fixture: a server driven by `Advance` and player
  bots each on its own `TicksDue`; the stage-4 acceptance; the disabled correction rate over seeds),
  `EntityTests.cpp` (stage 6: the `Arena` fixture, `Match` with presenting bots and a settle that waits for every
  body to rest; the fight, collapse, split, falling-piece and crate tests), `SyncTests.cpp` (stage 7a: `ChunkSync` and
  `OpRing` against a World without a transport, `BulkPacer` against a simulated link). Late-join tests are in
  `SessionTests.cpp` (`ChunkSync.LateJoinDuring10CastsPerSecondConverges`), the FakeServer join helpers (`Join`, `Ack`,
  `SendChunk`, `ReadRequests`) in `ClientTests.cpp`. `ClientWindowTests.cpp` (stage 7a M-b: the `Field` fixture, a
  flat region 40 chunks wide and a 12-chunk client window; walk-in, leave-and-return, teleport, reset, hash box, held
  player, a full store while walking, a piece that breaks off outside the window); its `Field` harness lives in
  `FieldHarness.h` since stage 7b (a header included after the imports that includes nothing itself: a standard header
  after an import can clash with a module's copy). `Region250Tests.cpp` (stage 7b: the test table on `Plain250`, a flat
  256 m region with the game's window and store). Trap for store tests on the flat
  region: its surface row is Dense when pristine (grass over dirt), so the window holds a constant number of slots
  as it moves and crater chunks overwrite slots they already have; a chunk that needs a slot of its own is one that is
  Empty when pristine (a box in the air).
- `tests/sim/ReplicationTests.cpp` - the server half of stage 6 on one `Simulation` (piece slots, caps, damage).
- `tests/sim/PlayerTests.cpp` - `StepPlayer` alone and two simulations stepped with the same inputs.

## 2. Invariants

- **Integers on the wire decide voxels.** Everything a client applies is a `sim::WorldOp`'s integers; floats travel
  as raw bits (`RegionDesc`) or are presentation (`Detach` velocity, light colour). Never add a float that a client
  turns into a voxel decision.
- **One order.** The server numbers events as it makes them (`opSeq`, counting up from 1); batches carry consecutive
  runs; a client's stream starts after its join point (`Welcome::joinOpSeq`, stage 7a) and it applies `opSeq ==
  applied + 1` only. A cast's operations are consecutive (casts run one after another inside a tick).
- **The base rule (stage 7a).** A terrain operation reaches a client chunk only if the chunk is current (`Based` and
  caught up) and the opSeq is above its base, live or replayed from the ring (`ChunkSync::Apply`, `CatchUp`). A chunk
  without a current base keeps whatever it has and is never hashed (`Client::Hash` counts it `notLoaded`) or walked on.
  An op that skips a chunk it touches is "not complete here": no presentation, and a replicated `Detach` asks for its
  model (`ModelRequest`) instead of building a piece from a partial volume.
- **The modified map is fed from the chunks an op changed** (`EditReport::changed`, `Server::Impl::EmitOp`), never
  from its box: a chunk an op did not change still equals the generator's output wherever it did before. Every server
  path that applies a terrain op must go through `EmitOp` (or a joiner will take a changed chunk as pristine).
- **Bulk is encoded at dequeue** (`NextBulk`): its asOf is the last event made then, so the client's ring covers the
  transit, never the queue. A message that does not fit the window is held encoded (`heldBulk`) - still covered - but
  only until the peer's **bulk** in flight has drained (`ITransport::ChannelBytesInFlight(peer, Bulk)`), then it goes
  alone while the bytes in flight (the live events) are under the window (review of 7a: a held message waiting for
  `ReliableBytesInFlight == 0` never left while events flowed; recheck of 7a: going alone whatever the events let bulk
  run on a link the events alone saturate). Two tests, both needed: the total in flight under the window (always), and
  the bulk in flight drained (for a message larger than what is left). `ServerStats::bulkOverWindow` stays 0.
- **The ring covers by count first.** `OpRing` keeps ops as wire bytes (masks as runs), 1024 ops or 1 MiB of those
  bytes; a base below its floor is requested whole, never replayed in part.
- **Entity events wait for their copy.** While the join's baseline is open, or for a NetId asked for, rest / wake /
  model events of an unknown NetId are held and applied after the copy if newer than its base (`Copy::base`); an event at
  or below a copy's base is dropped (the baseline holds it). `BaselineEnd` drops what nobody claimed. When a NetId's
  held events overflow (64) they are dropped and the model is asked for **again**, also with a request out (an answer
  already on its way may predate them); an answer older than the newest dropped event is discarded
  (`ClientStats::staleAnswers`). The server answers a `ModelRequest` whose answer is still queued with that one answer
  (`modelsQueued`), and disconnects a peer with more than `maxQueuedAnswers` queued.
- **The start gate opens on `BaselineNear`** (message 15, review of 7a): the baseline is sent nearest the spawn first
  (distance to each model's bounding sphere) and the marker follows the entities within `baselineNearMetres` (32 m);
  `BaselineEnd` opens it too. `Synced()` still waits for `BaselineEnd`.
- **Event kinds** 1-6 = `sim::WorldOpKind`, 32 = `EntitySpawn`, 33 `EntityDespawn`, 34 `EntityRest`, 35
  `ObjectModel`, 36 `EntityWake`; `EntityKind::Light` = 1, `Object` = 2; `scene::DespawnReason` (1 Destroyed, 2
  ToRubble, 3 Broken), `PlayerButton_Crate` = 8, `RegionGenerator`, `RegionTemplateId`, `MessageType`, `RefuseReason`
  values are wire format. Change any byte -> bump `kProtocolVersion` (8 since stage 7a: `MessageType` 8-15,
  `Welcome::joinOpSeq`, `RefuseReason` 6 retired; 15 `BaselineNear` came with the review, before 8 was committed; 9
  since stage 7b M-a: `MessageType` 16 `TerrainAck`, 17 `HashCheck`, 18 `EntityCheck`, 19 `EntityListRequest`, 20
  `EntityList`, `ChunkRequestReason`, `TerrainCode` and the accepted `WindowUpdate::kTerrainMode` are wire format; 10
  since the review of 7b: a `TerrainAck` carries the count of cells it codes and codes the update's entering cells only,
  in `net::ForEachEnteringChunk` order - that walk's order is wire format, shared by the server and `ChunkSync`).
- **A check is compared at its stream position (stage 7b).** HashCheck, EntityCheck and EntityList ride the events
  channel and are built in `Flush` right after the peer's `Pump` (`SendChecks`; nothing while a test hook holds its
  events), so the client processes them with `applied` equal to their opSeq (else counted `checksOutOfOrder`, never
  compared). A HashCheck compares only chunks that are current with a base at or below its opSeq: a chunk whose data is
  ahead (base > C) holds events the check does not, a chunk not current is not trusted anyway. A mismatch is 7a's
  Requested state plus a `ChunkRequest{Repair}`: never a second path around the base rule or the ring.
- **The entity check compares only a settled set.** Skipped while the baseline is open, while a list is out, or when
  entity data ahead of it came from bulk (`entityAsOf`: the newest asOf of a `BaselineEntity`, `BaselineNear` or
  `BaselineEnd` processed); while models are asked for only the count and the ids (asked NetIds count as present). The
  list is ids and models (`EntityList` at Q): a copy not listed with base <= Q is dropped and tombstoned, a listed id
  without a copy or with another model is asked for (7a's `ModelRequest`, which brings the exact rest state), a copy
  newer than Q is left alone. Never re-make a copy from a list entry: its rest / wake events already applied would be
  lost (the critic's finding 2).
- **Server terrain mode is the client's decision and sticky (stage 7b M-b).** The client switches (probes differ at
  the `Welcome`, or `pristineFailuresForTerrainMode` distinct pristine chunks it generated differently) and from then
  on every `WindowUpdate` carries `kTerrainMode`; the server only follows the flag and ends a peer that clears it. A
  terrain-mode update is answered by a `TerrainAck` in the ack's place (after `Pump`, processed at its opSeq): every
  entering chunk of the server's window coded by the server's cell state (uniform inline, Dense data, Packed data); the
  data is 7a's ChunkData path, modified or not. The `SessionGenerator` answers Empty inside the server's window, so
  nothing this client generates there is ever trusted (every chunk it bases counts as from data: no pristine check can
  fail it again). Outside the server's window it generates as before (never edited, never sent).
- **Packing follows the server's state in terrain mode.** A ChunkData is installed by `InstallMode`: `Content` for
  data the server holds Dense (shown, never packed, not an edit - an `Edit` would reveal every buried neighbour),
  `HiddenContent` for data it holds Packed (packed at once, no slot, made resident near the focus and packed again by
  the server's bit, never by this world's column classes, which come from the generator that differs) - only if no
  operation over the chunk or a face neighbour came between its ack and the stream's position, no face neighbour is an
  edit here, and no skipped operation later shows it (below: "A server-hidden chunk is shown whenever the server may
  show it"), otherwise `Content`. Repairs and re-requests are `Content`; 7a clients keep `Edit`. Packing is never a content change: modes differ in slots and
  drawing, never in hashes.
- **Slots for Unsynced requests are what is left after the content still to come** (NoBase, Awaiting, Requested
  records: `ChunkSync`'s incoming count). A window step frees the leaving slab's slots before the entering slab's data
  arrives; asking for Unsynced chunks with them made a request / StoreFull loop in terrain mode. Hidden data a packing
  world installs packed takes no slot and is not counted (review of 7b; `Record::incoming`, `ChunkSync::Recount`: every
  change of a record's state or `hidden` bit goes through `SetState` / `Recount`).
- **A check entry skipped while its chunk loads is compared later (review of 7b).** The server counts a pick as checked
  whether or not the client could compare it; an entry whose chunk is NoBase or Based and not loaded keeps the server's
  hash (`ChunkSync::CheckDeferred`, at most 256) and is compared once the chunk is current with a base at or below
  `applied`. It is exact only because **every stream operation whose chunk range holds the chunk drops the entry**
  (`ChunkSync::Apply`, before anything else): never compare a kept entry after an operation that may have changed the
  chunk, and never keep one for a chunk whose data is on its way (Awaiting, Requested, Unsynced: the server's anyway).
- **A repair is never looped on (review of 7b, the plan's choice 17).** A chunk that differs again within
  `kRepeatRepairChecks` (5) HashChecks of its repair, or after `kMaxRepairsPerChunk` (3) repairs, stays current (the
  player is not held over it every 2 s), is logged once as an error and counted (`repairsRefused`, then
  `stuckMismatches`). It is still a mismatch for every exit rule.
- **A server-hidden chunk is shown whenever the server may show it (review of 7b).** Installed `Content` instead of
  packed when a face neighbour is an edit here (`World::SetChunkEncoded`) or an operation near it came between its ack
  and the stream's position (`OpRing::TouchesNear` up to `max(asOf, applied)`); and in terrain mode an operation that
  skips a chunk (not current, or its data ahead) shows that chunk's server-hidden face neighbours for good
  (`World::ShowServerHiddenNeighbours`): the neighbour's later data is a `Content` install, never an edit, so nothing
  else would. `vortex_server` fails a run where a terrain-mode bot holds packed a chunk the server shows.
- **One `EntityList` per peer per `entityCheckTicks / 2` (review of 7b).** Requests in between are merged into the owed
  list (`entityListsMerged`); a list held by the gap counts once in `entityListsDelayed` (`Peer::listDelayed`, recheck
  of 7b) - `SendChecks` runs for every peer after every `Pump`, so a counter bumped there counts ticks, not lists,
  unless it is latched per event.
- **A bulk queue holds only its window (review of 7b).** Every `WindowUpdate` prunes the queued chunks that left the
  new window (`PruneQueue`); only the nearest 1024 entries are kept sorted at the queue's back and selected again when
  they run out or the player crosses a chunk (`SortChunks`, O(n)): a terrain-mode queue holds about 20 000 chunks.
  Anything that pops or erases from `Peer::chunks` must keep `sortedTail` (`PopChunk`, the request path).
- **One `EntityList` per peer per half check period (review of 7b).** Requests in between merge into the next.
- **Tombstones are bounded** (`ClientSettings::maxTombstones`, a FIFO): an evicted one can only let a stale answer
  re-create a dead copy, which the next EntityCheck and the list remove. The check is the tombstones' backstop: do not
  turn entity checks off in a session.
- **Every NetId destruction is an event.** The server connects `on_destroy<scene::NetId>` at `Start`; whatever
  destroys a replicated entity (Damage, `DestroyBelow`, eviction, `ClearBodies`, a player's crate limit) queues an
  `EntityDespawn` with `NetId::despawn` as its reason (set it before destroying: `sim::Despawn`), emitted by
  `DrainDespawns` after the cast or the tick. Never remove a NetId entity without letting the observer see it.
- **Only terrain operations charge the cast queue's event credit.** Entity events follow from Jolt (where a piece
  rests, which crate a blast catches, sleep): charging them would put a replayed cast on another tick (R5). They are
  counted (`ServerStats::entityEventBytes`). A `Detach` with a NetId is a terrain operation and is charged as before.
- **Entity events have a budget of their own** (review of stage 6; `entityEventBytesPerTick` / `entityEventBurstBytes`),
  which decides only which bodies are replicated, never when a cast runs: while it is spent `TakePieceSlot` answers
  `OverByteBudget` (`ReplicationState::refuseNewPieces`, set around each `RunCast`) and a damaged object is let go
  `ToRubble` instead of an `ObjectModel`. Rests, wakes, despawns and placed objects are always sent.
- **The NetId of a piece is a server decision, the voxels are not.** `TakePieceSlot` reads Jolt's sleep state (the
  awake cap) and the tick's budget: a replayed server may give another piece the id, never other voxels.
- **Layers follow rest on both sides, and rest and wake are events.** A replicated body is in `Layer::Rubble` while it
  moves and `Layer::Dynamic` at rest, on the server (`TrackBodies` after physics, which emits `EntityRest` and
  `EntityWake`; `SpawnObjectAt` at creation) and on every client (a copy is Dynamic from its `EntityRest` to its
  `EntityWake`). A moving body that carries a player stays Dynamic: on the server any player's ground body, on a client
  the copy under its own player (`MoveCopies`). The session character mask is Static | Dynamic and characters never
  move bodies (`characterMovesBodies` false), so what blocks a player is a function of the opSeq on both sides (a
  player standing on another player's ride is the exception).
- **The rest rule.** A copy at rest is at the `EntityRest` pose bit for bit; samples of the rest's tick or older are
  refused (`RemoteBodies::Add`); a stale rest (older than a kept sample) leaves the copy moving. Samples never change a
  copy's rest state. Rest poses and rotations compare exactly in every test, and so do rest states (`Agree`).
- **A joiner starts predicting from a fresh snapshot**: at most `startFreshTicks` behind the newest one received
  (else it waits, at most `startWaitTicks`). One that became ready only after a long catch-up of the join log is far in
  the past, and a prediction started there sends late inputs: a clock jump, then a hard resync (review of stage 6).
- **A snapshot's samples wait for its events**, as its owner block does: they are taken after `ApplyPending`, before
  reconciliation, once `Snapshot::lastOpSeq` is applied. So a spawn, model, rest or wake before a sample is always
  applied first, and an `ObjectModel` shifts only old-frame samples (review of stage 6).
- **The awake cap counts every awake replicated body** (pieces, crates, players' crates): they share the snapshots.
  A cast's terrain pieces ask for slots largest first; NetIds follow that order, the `Detach` order does not.
- **The cast schedule is a function of counts.** `CastWorkOf` may read only counters that follow from the flood
  box's voxels and the operations' integers. `graphsBuilt`, `chunksDecoded`, any time and the object half of a blast
  are never inputs (`CastBudget.ScheduleFollowsCountsNotTimeOrCache`).
- **The server's world never meets an unloaded chunk.** Window = play bounds + 3 chunks, loaded whole at `Start`;
  a cast is queued only if `sim::CastFitsRegion` (centre in the play bounds, radius <= `kMaxCastRadiusQ`, carve inside
  the rows). An island search error, an incomplete operation or a full store marks the server `Failed`, and
  **`Failed` is terminal**: peers are disconnected, nothing more is made or sent (not even the rest of the failing
  cast), new peers are disconnected on connect.
- **A client's window is the region's when it fits `ClientSettings::windowChunks`** (0 = always; the game and the
  arena bots pass 48), loaded whole before anything is applied; else it is `windowChunks` x the rows x `windowChunks`,
  follows the player (`RunTick`; the game's engine follows the camera), loads its surface at `Attach` and streams the
  rest (stage 7a M-b, `sim::ClientWorldConfig`). Its store is `sim::ClientChunkCapacity` (every cell of that window),
  capped by the owner's `storeLimit`. An operation it cannot apply in full no longer fails it (stage 7a): the chunks it
  could not change become Unsynced and are requested whole once more than `requestReserveSlots` are free.
- **A window move is noticed at the start of `Poll`** (`CheckWindow`) and nowhere else: the world moves only in
  streaming, outside `Poll`, so every op is applied against `ChunkSync`'s current window. Every step is listed by the
  world (`World::TakeWindowSteps`, with `reportLoads`) and sent as its own `WindowUpdate`, in order: a chunk that left at
  one step and came back (regenerated) at the next enters again with the second, and the server sends it again. The game
  streams every frame and polls on ticks, so two steps between `Poll`s are normal (review of 7a: they used to reset the
  whole window). A reset is left only for steps that were not listed (never in play).
- **A moving window follows with slack** (`WorldConfig::recenterSlackChunks`, `sim::kClientWindowSlackChunks` = 1): it
  moves once the focus chunk is more than one chunk from its centre chunk, then centres on it; `sim::ClientWindowMinChunk`
  is where it is placed (or moved to), not where every focus puts it. Offline worlds keep 0. The focus may sit one
  chunk off the centre, so `World::ViewRadius` leaves the slack out (88 m for the game's 48-chunk window instead of 92:
  the edge fog ends before the window's edge; recheck of 7a).
- **Chunks beyond the server's window are never edited** (`ChunkSync`'s `neverEdited`): current once loaded, never
  requested, never replayed. A moving window is not clamped to the region.
- **Session worlds are memory only, built from `Welcome` and pristine at `Attach`** (`Client::Attach` checks save
  directory, voxel size bits, window, height range and `BuildHash` of the simulation's materials; after loading the
  window's corner, a store that never filled, and the probe chunks of the loaded world against its own generator).
- **Live events are never held; bulk is paced.** Every Flush hands each joined peer all its events (one batch = one
  tick's events, <= `maxBatchBytes`, <= `kMaxMaskVoxels` of masks), then its bulk while its reliable bytes in flight are
  under its `BulkPacer` window (delay-based: rate x (base RTT + 2 ticks), halved at most once per round trip while the
  RTT is more than that above its base; the base is the lowest RTT of the last 12 x 10 s, so a lasting rise of the path
  becomes the new base) - a message larger than what is left going alone once none of its bulk is in flight, still only
  while the total is under the window. The drain rate counts every
  reliable byte handed over (`BulkPacer::Sent` with the events since the last tick: counting only bulk let the rate
  decay to its minimum while only events flowed). `maxEventBytesInFlight` (default 0) holds events only for a test;
  `maxPeerBacklogBytes` disconnects a peer that stopped reading. A joiner starts at its join point. While a peer's
  events alone fill its link its bulk waits (no minimum share is reserved: a long fight on a saturated downlink delays
  that peer's sync). `ServerSettings::countChunkRequests` (tests only) keeps per-coordinate request counts; off, the
  server keeps none (they would grow with every coordinate asked for).
- **Only live events are presented** (`opSeq > joinOpSeq`).
- **A comparison needs `Synced()`.** Drains (`vortex_server`, the game, the test fixtures) wait until the client is
  `Synced()` (every chunk current, no request out, baseline ended, nothing held) at the server's last opSeq; the hash is
  over `SyncWindow()` (the region's window while windows are fixed).
- **Nothing in the schedule reads a clock.** `CastRecord::seconds` and the `ServerStats` cast times are logged only.
- **One movement rule.** Server, predicting client and offline game move a player only with `sim::StepPlayer` from
  integer inputs; anything that changes how a player moves goes there (and into `PlayerTuning`, which travels in
  `Welcome`), never into one side. The rest of a step is the session's too: the tick length and gravity travel in
  `Welcome`, the character mask is `sim::kSessionCharacterLayers` on both sides (`Attach` refuses anything else), and
  both sides call `TerrainColliders::FlushBox(sim::PlayerReach(feet))` before every step (the client also before every
  replayed step and restore), so the colliders a step meets follow the edits applied, never worker timing.
- **The clock hears about the inputs it made.** A snapshot's margin is about `marginTick` (the newest input of the
  packet with the newest inputs, dropped ones included), never `ackInputTick`: a client too early has nothing
  acknowledged. Ticks at or past the client's next tick, and margins beyond +-1024, are forged and ignored.
- **Button levels, derived edges.** Inputs carry levels; `PressButtons` derives edges against
  `PlayerState::lastButtons`. A repeated input carries the levels the state already has, so it never makes an edge; a
  late input refreshes them and its edges count once (the cast in it is captured then).
- **A snapshot is compared only after its events.** `Snapshot::lastOpSeq` <= `AppliedOpSeq` before the comparison.
  Snapshots wait in arrival order; the newest whose events are applied is compared (older ones are dropped), and one
  that has waited `eventWaitTicks` since it arrived is given up and counted - its age never restarts when a newer one
  comes.
- **A cast is judged at its input's tick, its ray at its dequeue** (stage 5). On time: the state after the tick's
  step; late: the state of that tick from the player's 32-tick ring, the cooldown then and now both 0, the cooldown
  set as if taken then. The sphere is unknown until `RunCasts` pops the cast and `ResolveRay` casts it into the
  world as it is then; a ray that casts nothing costs nothing. `lastCastTick` / `castRejected` in the owner block
  tell the owner.
- **Only `Enet.cpp` sees ENet.** `vortex_enet` is PRIVATE to `VortexNet`; Core, Physics, Sim and `VortexSimTests`
  must not link it (`vortex_assert_no_network`).
- **Every tick the clock hands out is run.** `Client::TicksDue(seconds, maxTicks)` keeps what is over `maxTicks` for
  the next calls; an owner that can run only so many per frame asks for that many (the engine: `Engine::
  kMaxSourceTicksPerFrame`, 8). A tick handed out and dropped is lost to the clock (review of stage 5).
- **`Attach` attaches only a live session.** Its callback polls; if the session ended meanwhile (Closed, Failed,
  Refused) the load stops and `Attach` returns false - never Playing on a dead connection.
- **A late cast rewrites the cooldown history.** Accepted at tick T, the kept states of T.. get `full - j`, so a
  later late edge is judged as its client predicted it. Ring entries never written have the tick `0xFFFFFFFF`, and
  inputs before the player's join tick are ignored (`inputsBeforeJoin`).
- **Inputs sent are never taken back.** A hard resync holds the clock; nothing rewinds `PredictTick`, except a
  snapshot at or past it (`behind`), which moves it forward.

## 3. Traps

- `voxel::kGeneratorGoldenHash` must be updated whenever `Terrain.GoldenChunkHashes` (or the noise / height goldens)
  are regenerated; `Terrain.GeneratorGoldenHashNamesTheGoldenTables` prints the new value.
- `PlaygroundTemplate(world, ids, loadSite)`: `loadSite = true` moves a camera window to the site (the offline game);
  on a server it would move the fixed window off the region - pass false (`BuildRegionTemplate` does).
- Comparing the fixed step: `SimulationConfig::fixedTimeStep` is a float (0.016666668); `1.0 / 60.0` in double is
  smaller, so `Advance(1.0 / 60.0)` may run no tick. Tests use `FixedTimeStep()`.
- A dud next to an overloaded structure releases it (the load check runs after every blast, power 0 included): a
  test that warms the graph cache with a dud must do it where nothing is overloaded (`CastWorkIsEqualColdAndWarm`
  uses the mushroom).
- `ImpairedTransport::Disconnect` drops queued packets: refuse first, disconnect once `ReliableBytesInFlight` is 0
  (the server does).
- `LoopbackTransport::ReliableBytesInFlight` counts everything still in the other side's inbox: a peer that never
  polls keeps it above 0 (its sends stay held by the pacing; a refused one is closed after `closeGraceTicks`).
- A World edit that reaches a row above or below a full-height window reports those chunks as `skipped` (they are
  not cells), even for a carve that would change nothing there: on a server that is a failure, hence the carve-box
  rule in `CastFitsRegion`.
- `Client::MakeSimulationConfig` raises `chunkCapacity` to the region's cell count: fine for a CPU `Simulation`
  (slots are allocated on use), not for the engine's GPU store, which mirrors `Capacity()`: the game passes
  `storeLimit` (12 288) and `ClientSettings::minFreeStoreSlots` (2048), and `Attach` refuses a region that does not
  fit (stage 5).
- ENet services the socket only inside `Poll` (and sends in `Flush`): an owner that stops polling for longer than
  `EnetSettings::timeoutMinimumMs` (10 s) loses its peers. A long load goes through `Attach(simulation,
  whileLoading)` with a callback that polls.
- `ENetHost` has no user pointer: the receive-loss hook finds its transport in a process-wide map (main thread only).
  A transport's per-peer records must outlive the host (packets point at them): the destructor destroys the host
  first.
- `EnetTransport::Disconnect` reports `Disconnected` at once and forgets the peer; ENet's own disconnect event for it
  later only frees the record. Do not expect a second event.
- The ENet RTT includes the remote's frame time (it acknowledges when it is serviced): a 40 fps headless game client
  shows 33-44 ms on localhost. ENet starts its smoothed RTT at 500 ms and moves it 1/8 per acknowledged reliable
  message; a client sends few, so it stayed near 440 ms for seconds and the lead taken from it held the clock in a hard
  resync. `EnetTransport` seeds it with the connect handshake's round trip (connecting side), and
  `RoundTripSeconds` is the lowest of the smoothed value and ENet's lowest of this and the last 5 s interval (a stall
  of this side raises only the first); `SmoothedRoundTripSeconds` is ENet's own (the HUD's ping).
- ENet's `host->maximumPacketSize` limits what a peer may send here **and** what `enet_peer_send` takes: a listening
  transport sets it to 64 KiB (`kListenMaxMessageBytes`; strangers' messages are reassembled before any token) and
  `Send` lifts it for the call. A refused large message is dropped unacknowledged, so its sender times out.
- `vortex_server`'s in-process bots share one thread. Over UDP a bot's region load runs whole passes of the server and
  the other bots from `Attach`'s callback (and one after it), so it no longer stops them; but one thread still runs
  the server, the drill and every bot's simulation, and its margins swing (-30..+28 ticks in the UDP drill). Judge
  the clock with game clients (separate processes), not with the bots of `--enet`.
- The game services its connection from a helper thread while `Engine::Initialize` runs; nothing else may touch the
  client or the transport until that thread is joined (`EnetTransport` is one thread at a time, not thread-safe).
- A headless game session drains at exit (applied == the server's opSeq, held 1 s, at most 5 s) and exits 1 when it
  cannot: the hash it logs is then comparable with the server's last one.
- A wall-clock loop may run no tick in a pass: anything scheduled "on tick T" must remember that it ran (the drill
  queued a step on every pass of the same tick once).
- Growth over a session: the modified map (one entry per chunk ever changed, sparse) and the tombstones; the event
  backlog is trimmed every tick to the slowest joined peer; `CastRecord`s are a ring of `castRecordLimit`.
- Players do not join in `PeerId` order: a bot's slot depends on whose `Hello` arrived first. Tests take the slot
  from `Client::Slot()`.
- `PhysicsConfig::characterLayers` is `sim::kSessionCharacterLayers` (Static | Dynamic since stage 6) on a server
  (`sim::ServerConfig`) and a client (`MakeSimulationConfig`), with `characterMovesBodies` false and, on a client,
  `SceneConfig::rubbleLayer`; `Attach` refuses another mask, tick length, gravity or those flags. A client simulation
  built by hand without `MakeSimulationConfig` is refused (with the default mask the client's own rubble blocked its
  prediction: 100 corrections in 20 s in the drill).
- Copies move only in `Client::StepPlayer` / `RunTick` (`MoveCopies`, one tick each): a harness that polls and steps
  the simulation without them sees copies stand at their spawn until their `EntityRest` (the `Session` fixture of
  `SessionTests.cpp` does that; `Arena` and `Match` do not).
- A client's copy removed by its own scene (`DestroyBelow`: drawn in the past it can fall out first) is dropped and
  counted (`ClientStats::copiesLost`); the entity check shows it missing until the server's despawn comes.
- Tombstones of despawned NetIds are a FIFO of `maxTombstones` (4096) since stage 7b; a joiner starts with none (dead
  NetIds never reach it).
- The HashCheck planner (stage 7b) puts pristine chunks on the player's path first: a chunk whose box lies within
  `pathMetres` (1.5 m) of the way ahead. A walker on a chunk-row border has two rows on its path, so the way-ahead
  slots cover half the distance: 12 pristine slots (10 for the way) keep a 7 m/s runner checked ahead
  (`Region250.AChunkBrokenOnTheWayIsRepairedBeforeThePlayerReachesIt`, 0.40 s of margin in its worst phase). Fewer slots
  or a ranking that lets chunks behind the feet "arrive first" let a broken chunk pass unchecked (the test failed so).
- A terrain-mode join is large and paced: 16.3 MiB for the playground's fixed 30 x 44 x 30 window (about 7950
  ChunkData; the design's estimate was 3-10 MB), 40 MiB for the game's 48 x 44 x 48 window on the arena and 0.8 MiB per
  two-chunk window step there (1.9 MiB at most), through the pacer's 32 KiB window (at most one hand-over per tick:
  about 1.9 MiB/s on a LAN, 0.3 MB/s at a 100 ms round trip; the game on localhost starts 6-7 s and has every chunk
  15-28 s after its `Welcome`). The start gate waits only for the 3D-nearest data; `Synced()` and every drain wait for
  all of it (`vortex_server`'s drain waits up to 3 min for a terrain-mode bot). The `Join:` lines' "synced after" is
  noted every 16th Poll and whenever `Client::Synced()` is asked (a log value only). Since the review of 7b a window
  step's `TerrainAck` is 0.67 KiB on average (1.03 at most) on the events channel and costs the server 1.2-1.3 ms on
  average (4-5 ms at most, wall clock with the bots in the process): prune the queue, code the entering slab, select the
  nearest 1024; the join's ack (the whole box) 9-13 ms once.
- What a server check costs (stage 7b integration, the arena walk): the planner 0.2-0.6 ms, 20-24 chunk hashes, the
  entity values; 1.35-3.9 ms at most per peer and tick depending on the run (wall clock: in `vortex_server` the bots'
  worker threads share the cores; the review of 7b measured `ChunkHash` of a dense terrain chunk at 10.9 us, so 24
  entries are about 0.3 ms of compute). The game client: 2.0-3.7 ms at most per check, 0.5-1.3 ms on average, in runs
  of three processes on the laptop; windowed play is the owner's to judge. Two things keep it there: `voxel::ChunkHash` skips runs eight
  voxels at a time, and the server keeps each object's model hash with a weak pointer to its model (`modelHashes`;
  models are never changed in place - if one ever is, that cache returns a stale hash and every client mismatches).
  Checks are staggered (17 ticks per slot, entity checks half a period later), so the cost is per tick, not per
  player count up to 60 peers.
- A terrain-mode client's own column classes still place the sky and rock rows of the server's window (they are cheap
  and the ack overrides them): a `TerrainAck` code 2 over a cell that holds another uniform content installs it
  (`uniformInstalled`); a column job finishing after an install is discarded (the install bumps the cell's generation).
- A pristine chunk that is never checked stays as this client generated it: a standing player's checks reach 12 m
  around it, a walker's its way ahead. `vortex_server --bot-broken-chunk` therefore breaks a chunk within 12 m of the
  spawn and makes that bot stand.
- The bulk channel is not ordered against the events channel: a `BaselineEnd` can arrive before the `Welcome`. The
  client keeps bulk from `Joining` on (stage 7a: a Match-fixture join hung on a dropped `BaselineEnd`).
- `ImpairedTransport` draws every peer's losses from one random stream unless `Impairment::rngPerPeer`: two runs that
  differ in one peer then differ in every peer's losses (a latency comparison across runs is noise without it).
- A `WindowAck` is processed in stream order with the batches (it waits in `pending`); processing it early would base
  pristine chunks before the events before it are applied (harmless for content, but the replay then has work).
- A moving window is `Synced()` only once every chunk of it has loaded, caves included: on the arena that is many
  seconds of a bot's workers after it stops (a 3600-tick drain was too short once on the loopback, whose ticks race).
  Drains of a moving window wait for the world's loading on the wall clock (`vortex_server`), or stream while they
  wait (the game's drain; it does not run the engine's streaming otherwise).
- `Synced()` walks the window's records (101 376 for the game's) unless something is Unsynced or owed:
  `SyncHousekeeping` looks at it every 16th `Poll` until the join has synced once. Do not call it per tick elsewhere.
  `TakeRequests` walks them only while some record is Unsynced (`ChunkSync::SetState` keeps that count: every state
  change goes through it), and a window step visits only its entering columns (`ChunkSyncStats::recordsVisited`).
- Bulk installs are bounded per `Poll` (`ClientSettings::maxBulkPerPoll`, 64, a count so runs stay reproducible); the
  rest wait in `pendingBulk` in arrival order. Chunks that catch up together (one `WindowAck`, one load report, one
  `Poll`'s consecutive `ChunkData` via `ChunkSync::OnChunkData(span)`) walk the op ring once (`OpRing::Walk`): each op
  is decoded once per batch, not per chunk (`ChunkSyncStats::opsDecoded`; recheck of 7a: the old 16-entry FIFO cache
  never hit once a batch shared more than 16 ops). Keep catch-ups batched; per chunk the ops keep stream order. Costs are logged: `ClientStats::syncSeconds` / `bulkSeconds` on the
  `Join:` lines (the game on the arena: 0.017 ms per `Poll`, at most 2.0 ms for 64 installs), the server's
  `encodeSeconds` on its `Join:` / `Sync:` lines.
- "Synced after" is not a join time for a walker: its window keeps entering chunks to generate, so the first moment
  every chunk is current comes only once it stops (the arena walker: 25 s, after the walk). The join's own end is
  `ClientStats::joinDataTicks` (baseline ended, no chunk waiting for the server: none without a base, awaiting,
  requested or unsynced); `vortex_server`'s `Latency:` join windows end there.
- `vortex_server --enet` runs every bot in one thread: a joiner's events made while its own world loads wait for the
  load (0.5-0.7 s), and the server then catches up two ticks a pass, so latency p99 over UDP is stalls (15-47 ticks
  for the bot playing, 23-75 for joiners in the review's runs; the same with the review's changes put back), not stream
  lag. The loopback runs keep the load outside the simulated clock; read latency there (`--max-join-p99-ticks` only
  on the loopback; the p99 must be under N, as in the unit test).
- Stepping back and forth at the edge of a modified area re-sends its chunks each time they enter (chunks that leave
  are dropped, `keepUnloadedEdits` false). The follow slack cut it by a third (a bot wandering beside the arena's drill
  sites: 4.6 -> 3.1 MiB); a cache of the slab last left would cut the rest.
- The p99 < 250 ms join target cannot be met on a 5 %-loss link at any rate: a lost reliable packet is resent after
  about 130 ms and holds the 8 ticks of events behind it (the quiet p99 is 21 ticks at 64 and at 128 kB/s). The unit
  test asserts it at 128 kB/s and 1 % loss (13 / 12 ticks).
- Test hooks: `Server::SetMaxBulkBytesPerSecond` holds data back (1 B/s lets one message through, then nothing for
  minutes); a test moves a client's window itself with `Simulation::UpdateStreaming(focus, ...)` and `Poll` instead of
  `RunTick` (`ClientWindowTests.cpp`'s `manual` bots).
- A player's crates go when it leaves (`DropPlayer`); a crate is refused at the awake cap. So a departure makes events:
  what a departing client drained to is the opSeq before them, which `Server::SetOnClientLeft` reports (vortex_server
  logs its "client left" line there; `tools/session-check.sh` compares at it).
- The `Arena` / `Match` harnesses are reproducible only because colliders install in chunk order with a per-round job
  budget (physics notes) and the harness waits for idle between ticks: a harness that does not wait sees worker
  timing again (`Entities.OneScriptGivesTheSameBodiesTwice`).
- A player's cast at dequeue takes the nearer of the terrain and a replicated body (`PhysicsWorld::Raycast` with
  Dynamic | Rubble; every server body there is replicated); the game's puff takes the nearer of the terrain and a copy.
- A falling replicated piece passes through players (it is in Rubble) and may come to rest overlapping one; the
  character is then inside a Dynamic body on both sides and walks out (Jolt's penetration recovery), corrected once
  at most (`PlayerUnderFallingPiecesAt150ms`: 4 corrections over 6 pieces). Crush damage is M2 (shred.md question 8).
- `vortex_server --drill-every 6` (the default) on terrain rebuilds a 12.5 m site ten times a second; game clients
  beside it (`tools/session-check.sh`) apply those edits and show 0-2 clock jumps / resyncs from their own frame time,
  no corrections (the server's longest pass is logged: `Server: longest pass`).
- Other players are drawn at `ServerTime`, which advances only in `TicksDue` (real time): a harness that ticks a
  client without calling `TicksDue` sees them stand at the first snapshot's tick.
- `TickClock::Report` takes ticks the client made; feeding it non-increasing input ticks makes every report a
  repeat (ignored) - a test bug that once hid what a clock test was meant to show.
- `Client`'s character belongs to the simulation; the client never destroys it (the simulation may die first).
- Comparing prediction and server position is meaningful only while the player stands: a walking client is ahead by
  its lead (about 0.5 m at walk speed). The tests stop the controls first (`Settle`).
- `TicksDue` may return 0 (bleed, hold) or several ticks (catch-up, jump): a harness that polls only inside client
  ticks must still be fine without a poll for a pass.

## 4. Recipes

- New cast kind / loadout: the loadout id is in `QueuedCast` only (0 = the tuning's cast); a second one needs a wire
  field in `PlayerInput` (and `kInputBytes`), its radius / power in the tuning, `kProtocolVersion` + 1, and a
  `Movement.*` test that a late cast of it is judged at its tick.

- New player rule (crouch, sprint, a second jump): `sim::StepPlayer` and `PlayerTuning` (+ `WritePlayerTuning` /
  `ReadPlayerTuning`, `sim::IsValid`), a button in `PlayerButton` if it needs one (wire format), `kProtocolVersion` + 1,
  a `Player.*` test, then `Movement.NoCorrectionAbove2cmOnFlatGround` must still see 0 corrections.

- New event kind: value in `EventKind` (or a new `WorldOpKind` 7+), `WriteEvent` / `ReadEvent`, a size line in
  `Wire.EveryWorldOpRoundTripsAtItsSize` (`Wire.EntityEventsRoundTripAtTheirSizes` for entity events),
  `kProtocolVersion` + 1, the client's `Apply` / `ApplyEntity`, and `MaskVoxels` in `Server.cpp` if it carries a
  volume (the batch decoder's voxel budget).
- New kind of server body (a thrown object, a spell's matter): create it, `sim::Replicate(simulation, entity, piece)`,
  then the server's `EmitObjectSpawn`; it is then in snapshots, rests, despawns and the entity check like a crate.
  A piece counts against the piece caps (`piece = true`), a placed object does not.
- New region template: a value in `RegionTemplateId`, a case in `BuildRegionTemplate` and `sim::IsValid`, a keyword
  in `ParseSession` / `FormatSession`; it may read the loaded world but must not change it.
- Re-calibrating the cast work: `VortexNetTests --gtest_also_run_disabled_tests
  --gtest_filter=CastBudget.DISABLED_CalibrateWorkAgainstTime` on a quiet machine; the CSV columns are in the test's
  comment; aim at about 1 ms per 10 000 units and record the numbers in networking.md.

## 5. Verifying a change here

- `ctest --test-dir build -R "Wire|Transport|Client|Session|CastBudget|Clock|RemotePlayers|Movement|Enet"` (all of
  `VortexNetTests`, about 26 s alone, 11 s of it the wall-clock ENet tests) and `-R Player` (`VortexSimTests`).
- Over UDP: `build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/drill.session --drill=200
  --clients=3 --join-step 50 --bot-casts 90 --enet --loss=0.05 --delay-ms=40 --jitter-ms=30` (20 s of wall clock) must
  say "hashes equal" and 67/67, 67/67, 66/66.
- With the game: `tools/session-check.sh` in the game repository (about 30 s): vortex_server on 127.0.0.1 with a
  fresh token and two headless game clients; it exits 0 only when all three exit 0 and both clients' drained hashes
  equal the server's at the same opSeq, and (stage 7b) both clients' `Checks:` lines report HashChecks compared, 0 hash
  and entity mismatches and 0 checks out of order (logs and screenshots in `build/session-check/`).
- Stage 7b: every `vortex_server` run logs `Checks: bot N: ...` (`net::DescribeChecks`) and `Checks: server: ...` and
  fails on a mismatch where no fault was injected; `--bot-broken-chunk 1` (on the playground: `--drill 30 --clients 2`)
  shows one repair end to end. `-R "Region250|TerrainMode|HashCheck|ChunkSync|Client|Session"` runs the check and
  terrain-mode tests (`Region250.*` about 70 s in all). Server terrain mode end to end: `vortex_server
  --session=engine/tools/server/sessions/playground.session --drill 30 --clients 2 --bot-broken-generator 1 --loss 0.05
  --delay-ms 40 --jitter-ms 30` must exit 0 with `hashes equal` and bot 1's `Checks:` line ending `its broken generator
  put it in server terrain mode`; its `Terrain: bot 1:` line gives the join's bytes. The arena walk with
  `--bot-broken-generator 0` measures a terrain-mode walker (bytes per step, held ticks, store; since the review of 7b
  also the step acks by direction, the queue's largest size and chunks outside the window, and a `Terrain: bot N
  packing:` line that fails the run when a chunk the server shows is packed there). Every run logs `Ticks: server: N
  ticks, ... ms on average, p99 under ... ms, ... at most` (wall clock; `ServerStats::tickHistogram`).
- `VortexNetTests --gtest_also_run_disabled_tests --gtest_filter=Movement.DISABLED_FlatGroundCorrectionRateOverSeeds`
  (20 s) prints the flat-ground correction rate over 10 seeds (stage 4: 1 in 32 326 comparisons; after the review 1 in
  32 318).
- `build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/drill.session --drill=200
  --clients=2 --loss=0.05 --delay-ms=40 --jitter-ms=30 --outage-ms=10000,10200` must exit 0 with "hashes equal" and
  "freed terrain in 67/67 mushroom, 67/67 bar, 66/66 neck-tower steps"; its `Client N movement:` lines give each bot
  player's corrections (stage 4: 6 and 1 in 20 s, no snaps; after the review 4 and 4, the late joiner's 7 given-up
  snapshots now counted). Add `--bot-casts 90` for bots that cast too.
- `--session=.../arena250.session --ticks=60` logs the server's preload time and memory (the 10 s / 150 MiB gates).
- Stage 7a: every `vortex_server` run logs per joiner `Join: client N ...` (server: window, modified chunks, bytes,
  bulk window, queue empty after) and `Join: bot N ...` (started / join data in / synced, bulk received, requests), and
  per bot `Latency: bot N: ...` (server ticks from an event being made to the bot applying it: p99 and max for the
  events made while some bot joined, and for all; `--max-join-p99-ticks N` fails the run unless the p99 is under N ticks, the drill uses 15 = 250 ms).
  The arena walk (M-b acceptance, 35 s):
  `vortex_server --session=engine/tools/server/sessions/arena250.session --clients 2 --client-window 48 --walk-across
  --precasts 40 --join-step 20 --drill 40 --loss 0.05 --delay-ms 40 --jitter-ms 30 --memory` must exit 0 (hashes over
  each bot's sync box, entities, store at most 12 288 - 2048 slots; after the review of 7a: 5500 / 5447, latency during
  joins p99 11 / 10 ticks, RSS 1100 MiB, server world 48.4 MiB RLE, the walker 60 window updates). A game client on the arena: `vortex_server ... arena250.session
  --listen 27016 --bind 127.0.0.1 --token T --exit-when-empty --precasts 40` and `Shredborne --connect 127.0.0.1:27016
  --token T --headless --validation --frames 900 --screenshot a.png` (`--frames 15` shows the `JOINING` line); its
  `world hash ... over [box]` must equal the server's `window hash ... over [box]` at the same opSeq.
- The game's standard headless run must still log `Beam cut: 13 voxels removed, 1 new object(s)` (the plaza comes
  from the template now).
- Stage 6: `-R "Entities|Replication|RemoteBodies|Client"` (11 `Entities.*` since the review); the drill's
  `Entities: client N: ... - agree` lines and
  its `Objects:` line (bodies, spawns, rests, despawns by reason, caps, snapshot bytes); `tools/session-check.sh`
  compares the game clients' `entities at opSeq` lines with the server's.
