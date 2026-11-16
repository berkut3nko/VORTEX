# Notes: sessions (`src/net`, `src/sim/Region.cppm`, `tests/net`, `tools/server`)

Read `CLAUDE.md`, `docs/ARCHITECTURE.md` 7.5 and `docs/design/networking.md` ("Stage 3 as built", "Stage 4 as
built", "Stage 5 as built", "Stage 6 as built", "Review of stage 6") first; nothing from them is repeated here. Paths
are relative to `engine/`. Written at networking stage 3, extended at stages 4, 5 and 6 and the reviews of stages 5
and 6. The game's session
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
- `src/net/Session.cppm`, `internal/Session.cpp` - `SessionConfig`, `ParseSession` / `LoadSessionFile` /
  `FormatSession`, tokens, `BuildHash`, `ProbeChunks` / `CheckProbes`, `RegionHash`.
- `src/net/Server.cppm`, `internal/Server.cpp` - `ServerSettings`, `CastRecord`, `ServerStats`, `net::Server`
  (event log, peers with send cursors, cast queue and the two credits).
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
  body to rest; the fight, collapse, split, falling-piece and crate tests).
- `tests/sim/ReplicationTests.cpp` - the server half of stage 6 on one `Simulation` (piece slots, caps, damage).
- `tests/sim/PlayerTests.cpp` - `StepPlayer` alone and two simulations stepped with the same inputs.

## 2. Invariants

- **Integers on the wire decide voxels.** Everything a client applies is a `sim::WorldOp`'s integers; floats travel
  as raw bits (`RegionDesc`) or are presentation (`Detach` velocity, light colour). Never add a float that a client
  turns into a voxel decision.
- **One order.** The server numbers events as it makes them (`opSeq` = index in its log + 1); batches carry
  consecutive runs; a client applies `opSeq == applied + 1` only. A cast's operations are consecutive (casts run one
  after another inside a tick).
- **Event kinds** 1-6 = `sim::WorldOpKind`, 32 = `EntitySpawn`, 33 `EntityDespawn`, 34 `EntityRest`, 35
  `ObjectModel`, 36 `EntityWake`; `EntityKind::Light` = 1, `Object` = 2; `scene::DespawnReason` (1 Destroyed, 2
  ToRubble, 3 Broken), `PlayerButton_Crate` = 8, `RegionGenerator`, `RegionTemplateId`, `MessageType`, `RefuseReason`
  values are wire format. Change any byte -> bump `kProtocolVersion` (7 since the review of stage 6).
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
- **A client holds the whole region window** (until stage 7) and loads it before applying anything; its store is
  `sim::RegionChunkCapacity` (`MakeSimulationConfig`). An operation it cannot apply in full fails it: there is no
  resync before stage 7.
- **Session worlds are memory only, built from `Welcome` and pristine at `Attach`** (`Client::Attach` checks save
  directory, voxel size bits, window, height range and `BuildHash` of the simulation's materials; after loading the
  window's corner, a store that never filled, and the probe chunks of the loaded world against its own generator).
- **Every send is paced per peer.** `Peer::sentUpTo` advances only while `ReliableBytesInFlight(peer)` is below
  `maxEventBytesInFlight`; a joiner starts at 0 after `Welcome`. One batch = one tick's events, <= `maxBatchBytes`,
  <= `kMaxMaskVoxels` of masks (the decoder's per-batch bound).
- **Only live events are presented** (`opSeq > logEnd`).
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
- Growth over a session: the event log (about 2 KB per cast) and nothing else; `CastRecord`s are a ring of
  `castRecordLimit`.
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
- Tombstones of despawned NetIds grow for the session (4 bytes each); stage 7's join changes how they start.
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
  equal the server's at the same opSeq (logs and screenshots in `build/session-check/`).
- `VortexNetTests --gtest_also_run_disabled_tests --gtest_filter=Movement.DISABLED_FlatGroundCorrectionRateOverSeeds`
  (20 s) prints the flat-ground correction rate over 10 seeds (stage 4: 1 in 32 326 comparisons; after the review 1 in
  32 318).
- `build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/drill.session --drill=200
  --clients=2 --loss=0.05 --delay-ms=40 --jitter-ms=30 --outage-ms=10000,10200` must exit 0 with "hashes equal" and
  "freed terrain in 67/67 mushroom, 67/67 bar, 66/66 neck-tower steps"; its `Client N movement:` lines give each bot
  player's corrections (stage 4: 6 and 1 in 20 s, no snaps; after the review 4 and 4, the late joiner's 7 given-up
  snapshots now counted). Add `--bot-casts 90` for bots that cast too.
- `--session=.../arena250.session --ticks=60` logs the server's preload time and memory (the 10 s / 150 MiB gates).
- The game's standard headless run must still log `Beam cut: 13 voxels removed, 1 new object(s)` (the plaza comes
  from the template now).
- Stage 6: `-R "Entities|Replication|RemoteBodies|Client"` (11 `Entities.*` since the review); the drill's
  `Entities: client N: ... - agree` lines and
  its `Objects:` line (bodies, spawns, rests, despawns by reason, caps, snapshot bytes); `tools/session-check.sh`
  compares the game clients' `entities at opSeq` lines with the server's.
