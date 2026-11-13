# Notes: sessions (`src/net`, `src/sim/Region.cppm`, `tests/net`, `tools/server`)

Read `CLAUDE.md`, `docs/ARCHITECTURE.md` 7.5 and `docs/design/networking.md` ("Stage 3 as built") first; nothing
from them is repeated here. Paths are relative to `engine/`. Written at networking stage 3 (2026-11-13).

## 1. Map

- `src/net/Net.cppm` - primary unit of `vortex.net`; re-exports the partitions below.
- `src/net/Transport.cppm`, `internal/Transport.cpp` - `ITransport`, `TransportEvent`, `Channel`,
  `LoopbackTransport` (shared inboxes, `Connect(a, b)`), `Impairment` / `ImpairedTransport` (send-side queue sorted by
  due time; reliable packets keep per peer-and-channel order through `m_LastDue`).
- `src/net/Wire.cppm`, `internal/Wire.cpp` - `ByteWriter` / `ByteReader`, mask runs, `Event` = `variant<WorldOp,
  EntitySpawn>`, `WriteEvent` / `ReadEvent`, `WriteRegion` / `ReadRegion`, `QuantizeVelocity`.
- `src/net/Messages.cppm`, `internal/Messages.cpp` - message structs, `Encode` / `Decode`, `EncodeBatch` (events
  already encoded), `kProtocolVersion`.
- `src/net/Session.cppm`, `internal/Session.cpp` - `SessionConfig`, `ParseSession` / `LoadSessionFile` /
  `FormatSession`, tokens, `BuildHash`, `ProbeChunks` / `CheckProbes`, `RegionHash`.
- `src/net/Server.cppm`, `internal/Server.cpp` - `ServerSettings`, `CastRecord`, `ServerStats`, `net::Server`
  (event log, peers with send cursors, cast queue and the two credits).
- `src/net/Client.cppm`, `internal/Client.cpp` - `ClientSettings`, `ClientState`, `ClientStats`, `net::Client`.
- `src/net/Drill.cppm`, `internal/Drill.cpp` - `BlastDrill`: mushroom / bar / neck tower per step, sites of 100 x 100
  voxels in the play bounds.
- `src/sim/Region.cppm`, `internal/Region.cpp` - `RegionDesc` and its helpers, templates (`PlaygroundTemplate` is the
  game's plaza, moved here from `src/main.cpp`).
- `src/sim/Blast.cppm` (end of the constants) - `CastWork`, `CastWorkOf`, `OpWork`, the weights.
- `tools/server/main.cpp` - `vortex_server`; `tools/server/sessions/*.session` - examples that `SessionFile.*` parses.
- `tests/net/` - `WireTests.cpp`, `TransportTests.cpp`, `ClientTests.cpp` (`FakeServer`: a client against a
  hand-written server endpoint - overlaps, gaps, forged `Welcome`s, late-join presentation, refused worlds),
  `SessionTests.cpp` (the `Session` fixture: a server and bots on impaired loopbacks driven by one manual clock; the
  acceptance; pacing; the disabled calibration).

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

## 4. Recipes

- New event kind: value in `EventKind` (or a new `WorldOpKind` 7+), `WriteEvent` / `ReadEvent`, a size line in
  `Wire.EveryWorldOpRoundTripsAtItsSize`, `kProtocolVersion` + 1, the client's `Apply`.
- New region template: a value in `RegionTemplateId`, a case in `BuildRegionTemplate` and `sim::IsValid`, a keyword
  in `ParseSession` / `FormatSession`; it may read the loaded world but must not change it.
- Re-calibrating the cast work: `VortexNetTests --gtest_also_run_disabled_tests
  --gtest_filter=CastBudget.DISABLED_CalibrateWorkAgainstTime` on a quiet machine; the CSV columns are in the test's
  comment; aim at about 1 ms per 10 000 units and record the numbers in networking.md.

## 5. Verifying a change here

- `ctest --test-dir build -R "Wire|Transport|Client|Session|CastBudget"` (all of `VortexNetTests`, about 9 s).
- `build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/drill.session --drill=200
  --clients=2 --loss=0.05 --delay-ms=40 --jitter-ms=30 --outage-ms=10000,10200` must exit 0 with "hashes equal" and
  "freed terrain in 67/67 mushroom, 67/67 bar, 66/66 neck-tower steps".
- `--session=.../arena250.session --ticks=60` logs the server's preload time and memory (the 10 s / 150 MiB gates).
- The game's standard headless run must still log `Beam cut: 13 voxels removed, 1 new object(s)` (the plaza comes
  from the template now).
