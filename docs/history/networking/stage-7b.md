# Networking history: stage 7b plan, amendments, as built, integration, review and recheck

Moved verbatim from `docs/design/networking.md` lines 1627-2340 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

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
