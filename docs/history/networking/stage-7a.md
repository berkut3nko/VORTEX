# Networking history: stage 7a plan, amendments, as built, integration, review and recheck

Moved verbatim from `docs/design/networking.md` lines 959-1626 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

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
