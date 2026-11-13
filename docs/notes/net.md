# Notes: sessions (`src/net`, `src/sim/Region.cppm`, `tests/net`, `tools/server`)

Read `CLAUDE.md`, `docs/ARCHITECTURE.md` 7.5 and `docs/design/networking.md` ("Stage 3 as built", "Stage 4 as
built") first; nothing from them is repeated here. Paths are relative to `engine/`. Written at networking stage 3,
extended at stage 4 (2026-11-13).

## 1. Map

- `src/net/Net.cppm` - primary unit of `vortex.net`; re-exports the partitions below.
- `src/net/Transport.cppm`, `internal/Transport.cpp` - `ITransport`, `TransportEvent`, `Channel`,
  `LoopbackTransport` (shared inboxes, `Connect(a, b)`), `Impairment` / `ImpairedTransport` (send-side queue sorted by
  due time; reliable packets keep per peer-and-channel order through `m_LastDue`).
- `src/net/Wire.cppm`, `internal/Wire.cpp` - `ByteWriter` / `ByteReader`, mask runs, `Event` = `variant<WorldOp,
  EntitySpawn>`, `WriteEvent` / `ReadEvent`, `WriteRegion` / `ReadRegion`, `QuantizeVelocity`.
- `src/net/Messages.cppm`, `internal/Messages.cpp` - message structs, `Encode` / `Decode`, `EncodeBatch` (events
  already encoded), `kProtocolVersion`; stage 4: `InputBatch`, `Snapshot`, `RemotePlayer`, `Welcome::player`.
- `src/net/Prediction.cppm`, `internal/Prediction.cpp` - `TickClock` (the client clock: jumps, bleed, catch-up,
  holds), `ServerTime` (the server tick other players are drawn at; only moves forward) and `RemotePlayers` (other
  players drawn in the past).
- `src/net/Session.cppm`, `internal/Session.cpp` - `SessionConfig`, `ParseSession` / `LoadSessionFile` /
  `FormatSession`, tokens, `BuildHash`, `ProbeChunks` / `CheckProbes`, `RegionHash`.
- `src/net/Server.cppm`, `internal/Server.cpp` - `ServerSettings`, `CastRecord`, `ServerStats`, `net::Server`
  (event log, peers with send cursors, cast queue and the two credits).
- `src/net/Client.cppm`, `internal/Client.cpp` - `ClientSettings`, `ClientState`, `ClientStats`, `PredictionStats`,
  `net::Client` (events; the own player's prediction, input sends and reconciliation).
- `src/net/Drill.cppm`, `internal/Drill.cpp` - `BlastDrill`: mushroom / bar / neck tower per step, sites of 100 x 100
  voxels in the play bounds; `BotPilot` / `PilotSettings`: seeded controls of a bot player.
- `src/sim/Player.cppm`, `internal/Player.cpp` - `PlayerInput` / `PlayerState` / `PlayerTuning`, `StepPlayer`, the
  quantisers (the movement rule of server, client and offline game).
- `src/sim/Region.cppm`, `internal/Region.cpp` - `RegionDesc` and its helpers, templates (`PlaygroundTemplate` is the
  game's plaza, moved here from `src/main.cpp`).
- `src/sim/Blast.cppm` (end of the constants) - `CastWork`, `CastWorkOf`, `OpWork`, the weights.
- `tools/server/main.cpp` - `vortex_server`; `tools/server/sessions/*.session` - examples that `SessionFile.*` parses.
- `tests/net/` - `WireTests.cpp`, `TransportTests.cpp`, `ClientTests.cpp` (`FakeServer`: a client against a
  hand-written server endpoint - overlaps, gaps, forged `Welcome`s, late-join presentation, refused worlds),
  `SessionTests.cpp` (the `Session` fixture: a server and bots on impaired loopbacks driven by one manual clock; the
  acceptance; pacing; the disabled calibration), `ClockTests.cpp` (`TickClock` and `RemotePlayers` from margin and
  sample sequences), `MovementTests.cpp` (the `Match` fixture: a server driven by `Advance` and player bots each on
  its own `TicksDue`; the stage-4 acceptance; the disabled correction rate over seeds).
- `tests/sim/PlayerTests.cpp` - `StepPlayer` alone and two simulations stepped with the same inputs.

## 2. Invariants

- **Integers on the wire decide voxels.** Everything a client applies is a `sim::WorldOp`'s integers; floats travel
  as raw bits (`RegionDesc`) or are presentation (`Detach` velocity, light colour). Never add a float that a client
  turns into a voxel decision.
- **One order.** The server numbers events as it makes them (`opSeq` = index in its log + 1); batches carry
  consecutive runs; a client applies `opSeq == applied + 1` only. A cast's operations are consecutive (casts run one
  after another inside a tick).
- **Event kinds** 1-6 = `sim::WorldOpKind`, 32 = `EntitySpawn`; `EntityKind::Light` = 1; `RegionGenerator`,
  `RegionTemplateId`, `MessageType`, `RefuseReason` values are wire format. Change any byte -> bump
  `kProtocolVersion`.
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
  (slots are allocated on use), not for the engine's GPU store, which mirrors `Capacity()` (stage 5).
- Growth over a session: the event log (about 2 KB per cast) and nothing else; `CastRecord`s are a ring of
  `castRecordLimit`.
- Players do not join in `PeerId` order: a bot's slot depends on whose `Hello` arrived first. Tests take the slot
  from `Client::Slot()`.
- `PhysicsConfig::characterLayers` is `sim::kSessionCharacterLayers` (Static only) on a server (`sim::ServerConfig`)
  and a client (`MakeSimulationConfig`); `Attach` refuses another mask, tick length or gravity. A client simulation
  built by hand without `MakeSimulationConfig` is refused (with the default mask the client's own rubble blocked its
  prediction: 100 corrections in 20 s in the drill).
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

- New player rule (crouch, sprint, a second jump): `sim::StepPlayer` and `PlayerTuning` (+ `WritePlayerTuning` /
  `ReadPlayerTuning`, `sim::IsValid`), a button in `PlayerButton` if it needs one (wire format), `kProtocolVersion` + 1,
  a `Player.*` test, then `Movement.NoCorrectionAbove2cmOnFlatGround` must still see 0 corrections.

- New event kind: value in `EventKind` (or a new `WorldOpKind` 7+), `WriteEvent` / `ReadEvent`, a size line in
  `Wire.EveryWorldOpRoundTripsAtItsSize`, `kProtocolVersion` + 1, the client's `Apply`.
- New region template: a value in `RegionTemplateId`, a case in `BuildRegionTemplate` and `sim::IsValid`, a keyword
  in `ParseSession` / `FormatSession`; it may read the loaded world but must not change it.
- Re-calibrating the cast work: `VortexNetTests --gtest_also_run_disabled_tests
  --gtest_filter=CastBudget.DISABLED_CalibrateWorkAgainstTime` on a quiet machine; the CSV columns are in the test's
  comment; aim at about 1 ms per 10 000 units and record the numbers in networking.md.

## 5. Verifying a change here

- `ctest --test-dir build -R "Wire|Transport|Client|Session|CastBudget|Clock|RemotePlayers|Movement"` (all of
  `VortexNetTests`, about 13 s) and `-R Player` (`VortexSimTests`).
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
