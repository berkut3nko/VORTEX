# Roadmap history: the M1 stage log ("State" under M1, stages 0-9)

Moved verbatim from `docs/ROADMAP.md` lines 93-445 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

State (stages are those of [design/networking.md](design/networking.md)):
- **Stage 0, determinism floor - done on Linux, Windows run still open.** Engine code is compiled
  with `-ffp-contract=off` (`vortex_options`; spelled `/clang:-ffp-contract=off` for clang-cl, which
  ignores the bare flag; Jolt keeps its own flags). `voxel::ChunkHash` / `voxel::WorldHash`
  (`src/voxel/Hash.cppm`) hash canonical chunk content. Three golden tests pin the built-in
  generator for 3 seeds: `Terrain.GoldenChunkHashes` (64 chunks per seed, incl. snow columns and
  chunks 1 km out), `Terrain.GoldenNoiseBits` and `Terrain.GoldenHeightBits` (raw float bits). Checked
  by building once without the flag: both float-bit tests fail and 1 of 192 chunk hashes differs, so
  the float-bit tests are the ones that detect a differently rounding build.
  `voxel::TerrainHeight` and `voxel::IsValid(TerrainSettings)` are new; nothing calls `IsValid` yet.
  Measured on the owner's laptop (5184 chunks, best of 7, three runs each): 1.57 ms per chunk before the
  flag, 1.59-1.65 ms after, and 1.60-1.64 ms with the flag removed again in the same session, so the
  cost is inside the run-to-run noise (about 5 %).
  **Not done:** `VortexCore` and `VortexCoreTests` have not been built or run on Windows (the
  clang-cl flag was checked with the compiler driver only); until the golden tests are green there,
  terrain agreement across platforms is unproven. Known risks: `design/networking.md`.
- **Stage 1, simulation without a renderer - done.** New library `Vortex::Sim` = `vortex.scene` + `vortex.sim`
  (Core + Physics + EnTT; a configure-time check, `vortex_assert_gpu_free`, fails the build if Vulkan, GLFW or ImGui
  get into it). `sim::Simulation` holds what used to be the simulation members of `Engine::Impl` - job system,
  materials, chunk store, world, physics, terrain colliders, particle pool, scene - plus a `uint32_t` tick.
  `Step(focusPoints)` is one server tick (streaming without moving the window, body guard, physics, pose sync,
  lifetimes, fall-out); the engine calls its three phases (`UpdateStreaming`, `StepPhysics`, `UpdateLifetimes`) where
  it called the parts before, so the frame loop and the game are unchanged. The scene no longer imports
  `vortex.graphics`: `GatherInstances` / `GatherLights` are free functions of `vortex.engine`.
  `sim::ServerConfig` gives the server scene: no particles, `SceneConfig::bakeModels = false` (models hold no chunk
  slots), `markDebris = false` and `debrisRemnants = false` (nothing is rubble). `ServerConfig` also makes the
  world memory only (clears `saveDirectory`); any other simulation, the engine's included, keeps it, so the
  editor's "Save world" works whenever the app sets a directory.
  Verified: the 310 earlier tests pass; 22 new tests in `VortexSimTests`, which links `VortexSim`, Jolt, glm, spdlog
  and GoogleTest only, step a `Simulation` 600 ticks with the offline and with the server configuration (crates
  land and fall asleep on the terrain collider, a timed light expires on its tick) and 240 ticks on the built-in
  terrain; both headless game runs exit 0 with the expected log lines. "Game unchanged" was checked against a build of
  the previous commit: same blast (7 -> 7 objects), same beam cut (37 voxels, 1 new object), player on the ground, the
  same picture; chunk and body counts vary between runs of either build by a few percent (streaming timing).
  **Not done / open:** the tick-rate scale of `Engine` (stage 4, with time sync); nothing yet stops a network client
  from running with a `saveDirectory` (stage 3: `net::Client` must refuse or clear it); on a server scene pieces of
  16 voxels (0.1 m) and more stay for good until `Scene::Damage` gets its threshold (stage 2 / 6); lifetimes run per tick
  in `Step` but per rendered frame in the engine, so nothing replicated may depend on them. The choices made where
  the design was open are in `design/networking.md`, "Stage 1 as built".
- **Stage 2, world operations and the blast split - done.** Replicated terrain edits take integer parameters only:
  `voxel::SphereQ` (centre and radius in 1/16 voxel, int64 sphere test), power in 1/16 hardness units, and
  `World::CarveSphereQ / FillSphereQ / SwapSphereQ / ClearMask` (the float `CarveSphere` / `FillSphere` quantise and
  forward). Every edit can report the chunks it could not apply to (`voxel::EditReport`: `skipped` = not loaded,
  `unsynced` = no store slot; all-or-nothing per chunk). `sim::WorldOp` (carve, fill, swap, box, detach) and
  `sim::ApplyOp` are what server and clients run. `sim::Blast` = carve, island search, one `Detach` per island (an
  occupancy mask, netId 0, the blast's push as velocity; no entity), `Scene::Damage` per object, impulses; it spawns
  no particles. `Engine::PresentBlast` makes the particles, the island rubble (from the voxels the Detach removed)
  and the flash; `Engine::Explode` = both, and the game runs on it. `SceneConfig::replicatedPieceVoxels` (0 offline,
  512 from `ServerConfig`): smaller object pieces - and a dynamic survivor below it - come back as fragments
  without an entity (client rubble). Verified: 22 new tests (385 in all); two simulations with different worker
  counts fed the same operations, two islands included, end with equal `WorldHash`, and a stream with one carve
  1/16 voxel larger or one Detach missing does not; headless game, editor and pause-menu runs exit 0 with the same
  crate blast (5 objects lose 558 voxels, no terrain) and the same beam cut (37 voxels, 1 new object) as before,
  and the crate area looks the same before and after. One offline change: a fresh island is no longer damaged by
  the blast that freed it (it is still pushed the same way). **Not done / open:** nothing acts on the skipped /
  unsynced report yet (stage 7); object particles are still spawned inside `Damage`; no `opSeq` or encoding
  (stage 3). Choices where the design was open: `design/networking.md`, "Stage 2 as built".
  **Crumbs (follow-up):** islands under the piece volume (0.016 m^3, `scene::MinPieceVoxels`; 16 voxels of 0.1 m then) are no longer a `Detach` each but one
  `ClearMask` per blast over their tight box, shown as particles only (`sim::PresentBlast`, now GPU-free and tested;
  no model, entity or body). Measured on generated terrain: 2051 operations for 44 blasts (worst 299) became 75 (at
  most 2), est. 65 KB -> 7.2 KB of wire. Found, open: the island search takes 70-200 ms per 1.2 m blast in the `dev`
  build, and that build has no `-O` flag at all (empty `CMAKE_CXX_FLAGS_RELWITHDEBINFO` with CMake 4.4).
- **Stage 3, the protocol on a loopback - done.** Library `Vortex::Net` (`vortex.net`, links `Vortex::Sim` only;
  `vortex_assert_gpu_free` covers it, `VortexNetTests` and `vortex_server`): `ITransport` with `LoopbackTransport`
  and the `ImpairedTransport` decorator (ENet semantics: reliable channels resend and keep order, the state channel
  loses, duplicates and reorders); byte reader / writer, events (world operations at 16-29 bytes plus RLE masks,
  `EntitySpawn` for lights), `Hello` / `Welcome` / `Refuse` / `EventBatch` / `ServerStatus`, every decoder refusing
  bad input; session files (`tools/server/sessions/*.session`); `BuildHash` = protocol version + the generator's
  golden hash + the material table (strengths included, shred.md R6); 64 probe chunks from the seed in `Welcome`.
  `net::Server`: window = play bounds + 3 chunks at the region's height range, loaded whole at start; the template's
  operations as 1..k (the game's plaza is now `sim::PlaygroundTemplate`, applied the same way offline), lights as
  `EntitySpawn`, crates as server bodies; per tick receive -> streaming -> casts within the cast work budget
  (`sim::CastWorkOf`, 60 000 units of about 0.1 us, calibrated) and the event byte budget (24 kB/s, 16 kB burst) ->
  physics -> lifetimes -> batches; joiners get the whole log. `net::Client`: refuses a world with a save directory or
  another voxel size / window / material table, loads the region window, applies events in order, groups them for
  `sim::PresentBlast`. `sim::kNeckFlowWorkBudget` lowered to 65 536 (shred.md question 7). Verified: 26 new tests
  (548); the acceptance - an in-process server and two clients, 5 % loss with delay, jitter and an outage, 200
  server-side blasts that free terrain (67 in-box islands with crumbs, 67 face-flood islands, 66 weak-neck releases),
  one client joining after 100 blasts from the full log - ends with equal world hashes; a server replayed with other
  worker counts and an emptied graph cache runs every cast on the same tick. `vortex_server --drill 200 --clients 2
  --loss 0.05` exits 0 with equal hashes; on a 250 m terrain region the server preloads in 9.6-10.4 s (gate 10 s)
  with 48.5 MiB of RLE and 463-488 MiB RSS. The game's headless runs are unchanged. **Not done / open:** ENet and the
  game's `--connect` (stage 5), inputs and snapshots (4), objects and pieces replicated (6), chunk sync and server
  terrain mode (7; a probe mismatch is only logged), `vortex_bot` (8); a stage-3 client holds the whole region; the
  join log grows for the session; R6's integer `MaterialPhysicsQ`. Choices: `design/networking.md`, "Stage 3 as built".
  Review of stage 3 (17 findings, all fixed, 13 new tests, 561 in all): a client refuses a store too small for the
  region or a world its generator did not make and fails (and leaves) on an operation it cannot apply; casts must
  fit the window (`sim::CastFitsRegion`, radius <= 9 m at 0.125 m); a failed server ends the session; only live
  events are presented; `Welcome` probes are range-checked; sends are paced per peer by `ReliableBytesInFlight`
  (32 kB); batches decode at most 16 MiB of masks; silent and refused peers time out; the default store is capped at
  2 GiB of chunks; the playground must find its site inside the play bounds; cast wall time is logged.
- **Stage 4, players - done.** `sim::StepPlayer` (`vortex.sim:player`): integer inputs (move axes, yaw, pitch,
  button levels with derived edges), a cast cooldown, the gate `TerrainColliders::HasCollider` (chunks with a
  collider built once: no wait for re-cooks, whose timing differs per side); `PhysicsWorld::SetCharacterState`;
  `PhysicsConfig::characterLayers` (a client's character collides with terrain only until stage 6). Messages
  `InputBatch` (the unacknowledged inputs, at most 8, 30 Hz) and `Snapshot` (tick, event position, input ack and
  margin, owner state, other players; replaces `ServerStatus`), the tuning in `Welcome`, protocol 2. Server: inputs
  queued up to 16 ticks ahead, a missing one repeats the last movement for 6 ticks, a late one refreshes it and its
  cast counts once; a cast edge is captured as a ray into the server's world and queued. Client: `TickClock` (jump
  after three late reports, 2 % bleed and - not in the design - 2 % catch-up, a hold as hard resync), prediction
  with 128 ticks of history, reconciliation after the snapshot's events (2 cm ignore, replay, 100 ms blend, 1.5 m
  snap, 250 ms timeout counted), `RemotePlayers`. `net::BotPilot`; `vortex_server`'s clients walk, jump and (with
  `--bot-casts N`) cast, and the report gives each one's correction rate. The offline game moves its player with
  `StepPlayer`. Verified: 23 new tests (584 then); over the seeded impaired loopback no correction above 2 cm on flat
  ground in 30 s at 5 % loss (0.05 per player-minute over 10 seeds), none beside 80 blasts at a chunk border, the
  +50 ms ping step never late and no resync, three lost packets with a cast give one cast, a 150 ms hitch no resync,
  a wall and a teleport converge, forged packets harmless; the drill exits with equal hashes, 6 and 1 corrections in
  20 s. **Not done / open:** the engine driven by the client clock and `--connect` (stage 5, since built), capture at the input
  tick and loadouts (5, built: one loadout), replicated bodies in the character mask (6), the visual-only timeout branch (7), errors up to
  10 mm after blasts near a player (not isolated). Choices: `design/networking.md`, "Stage 4 as built".
  **Review of stage 4** (15 findings, none rejected, 9 new tests, 593): the clock now hears margins of dropped
  too-early inputs (`Snapshot::marginTick`, protocol 3; a client placed far too early never recovered), the hard
  resync can no longer wrap to 4e9 ticks, three margins 2+ short of the target jump the clock (a +50 ms ping step:
  back in 0.52 s instead of 2.95 s; the 0.5 s acceptance is met from the first short report, from the step itself
  only with 0.6 s - an owner question), colliders under a player are flushed before every step on both sides
  (`TerrainColliders::FlushBox`; slopes went from 1 mm - 2 cm errors to bit-equal), tick length and gravity travel in
  `Welcome`, both sides use one character mask, remote players never step back (`ServerTime`), the 250 ms event wait
  times out while snapshots keep coming, a late jump is jumped, and the border, ping-step and flat-ground tests assert
  what they claim. See "Review of stage 4".
- **Stage 5, ENet and the game as a client - built; the acceptance (two people on two machines) is the owner's.**
  ENet 1.3.18 (pinned, built from source, PRIVATE to `Vortex::Net`; `vortex_assert_no_network` keeps it out of Core,
  Physics and Sim) behind `net::EnetTransport` (exact `ReliableBytesInFlight` from ENet's packet releases, graceful
  disconnects, a test-only receive loss below ENet); `ITransport::Flush`. Casts captured as {eye, look, input tick,
  loadout}: a late input's cast judged at its own tick (state ring, cooldown as the owner predicted it), the ray cast
  when the cast runs; the snapshot's owner block says which cast was taken last and why it did not run (protocol 4).
  `Engine::SetTickSource` (the client clock drives the engine), `EngineConfig::simulation` / `windowFollowsCamera`;
  the game's `--connect` / `--token` (`src/Session.cpp`): the store is capped at the GPU buffer (12 288 slots) and a
  region that does not fit with 2048 slots to spare is refused with a clear error - the playground session takes 2084
  slots; `--voxel-size`, the editor and the offline scripts are refused in a session. `vortex_server --listen [PORT]
  --bind --seconds --exit-when-empty`, `--enet` for its bots. Verified: 7 new tests, 600 in all (six disabled):
  ordered exactly-once delivery through 5 % real datagram loss, disconnects, and two walking, casting clients on
  three UDP sockets ending with the server's world hash; a server and two headless game clients on localhost end at
  the same opSeq with equal hashes, each sees the other and the other's holes; a game client joins a running UDP drill
  from a 187 KB log without corrections; the drill over UDP keeps equal hashes and 67/67/66 freed; offline runs
  unchanged. **Not done / open:** the two-machine run and windowed play of a session (owner; steps in CLAUDE.md and
  `design/game-ui.md`); the 250 m arena in the game (est. at the edge of the GPU store; stage 7's client window);
  crates and pieces on clients (6); chunk sync and server terrain mode (7); `vortex_bot` and the soak (8); a Windows
  client (M6). Choices: `design/networking.md`, "Stage 5 as built".
  **Review of stage 5** (13 findings, 11 distinct, every bug first a failing GPU-free test; 7 new tests, **607 in all**,
  six disabled; protocol 5): `Attach` no longer turns a session that ended during the load into Playing; the engine asks
  the client clock for at most 8 ticks a frame and the clock keeps the rest (it dropped up to 22); a listening ENet
  transport takes messages of at most 64 KiB (strangers could make it allocate 32 MiB each); the client's RTT is
  seeded from the connect handshake (ENet's started at 500 ms: the lead it gave made a hard resync in every UDP test
  run) and is stall-insensitive, the game services the connection while the engine initialises, a bot's load no
  longer stops the in-process server; late casts update the cooldown history and inputs from before the join are
  ignored; `vortex_server --new-token` / `--token` and a warning for a public token; a headless session client drains
  and fails on an unshown hash, `tools/session-check.sh` compares the hashes (PASS); Ctrl-C no longer waits out the
  drain; a too-large join log refuses joiners (`LogTooLarge`) and is reported. See "Review of stage 5".
- **Stage 6, replicated bodies - built and reviewed.** An unreviewed partial implementation was committed by the
  owner (engine `c1e19c8`); it was judged against the design, two failing physics tests fixed (a character filter
  that let `Layer::Rubble` block characters; a push test with the wrong mass), and the stage finished on top of it.
  `scene::NetId` with an `on_destroy` observer on the server (every destruction is an `EntityDespawn`), `Detach` with a
  NetId (the server body made from the removed voxels, as every client makes its copy), `EntitySpawn` objects,
  `ObjectModel`, `EntityRest`, `Layer::Rubble`, `MoveKinematic`, `SetLayer`; the piece budgets (`sim::TakePieceSlot`:
  2 new server bodies per tick, 64 awake, 256 live with eviction `ToRubble`; over a budget client rubble), awake bodies
  in the snapshots by priority (31 B each, 1200 B per snapshot), `net::RemoteBodies` (Hermite in the past, the rest
  rule), kinematic copies on clients, client rubble with stage 1e's lifetimes and caps, the session character mask
  Static | Dynamic with bodies in Rubble while they move on both sides and characters that move no bodies; crates
  from E (`PlayerButton_Crate`), protocol 6. `vortex_server`: the drill's pieces and crates are server bodies, bots drop
  crates (`--bot-crates`), the drain waits for bodies to rest and an entity check compares every bot's copies (exit 1
  on a difference); `--drill-after/-delay/-sites/-every` run a drill beside a session; `tools/session-check.sh` does and
  compares the game clients' entity lists. Verified: 16 new tests, **623 in all** (six disabled): the scripted fight
  with islands and crates (5 % loss, 80 + 0-30 ms, an outage; equal hashes, ids, models and exact rest poses, players
  within 5 cm), a collapse with 64 awake pieces at 5 % loss (both caps), a split below the threshold, a player under
  falling pieces at 150 ms (4 corrections over 6 pieces, no snap), resting crates blocking players on both sides
  (0 corrections), the rest rule against a hand-written server; the drill (loopback and UDP) with equal hashes and
  agreeing entities; session-check PASS; offline runs unchanged.
  **Review of stage 6** (17 findings, 2 duplicates, none rejected outright; parts of two rejected with reasons; one more
  problem found while verifying; 11 new tests, **634 in all**): a snapshot's body samples wait for its events; waking
  is an event (`EntityWake`, protocol 7) and a damaged survivor's copy is re-made moving; a body that carries a player
  stays solid on both sides; the replicated size follows the session's voxel size; the awake cap counts every awake
  body; entity events have a byte budget of their own (over it: client rubble, damaged objects let go); casts hit
  replicated bodies; a cast's largest pieces get the tick's bodies; `Broken` objects burst; the entity check judges
  rest states and rotations; the collapse test follows the copies mid-flight; collider installs and the cook budget no
  longer follow worker timing (the session tests are reproducible bit for bit); players' crates go with the player;
  the game's drain waits for rest; a late joiner starts predicting from a fresh snapshot (the drill's joiner had a
  hard resync). **Not done / open:** windowed play and the two-machine run with bodies (owner); the wire
  `EntityCheck` and entity baseline (7); the replicated threshold was decided on 2026-11-16: it stays at 0.512 m^3, so a crate
  (152 voxels) breaks at its first hit; fun and few bugs come before synced rubble; crush damage (M2). Choices:
  `design/networking.md`, "Stage 6 as built", "Review of stage 6".
- **Stage 7a (chunk sync: joins and a moving client window) - milestones M-a and M-b built, integrated, accepted,
  reviewed and rechecked (2026-11-17), committed (engine `62f28b4`, game `92e3902`).** A joiner no longer receives the event log: it starts at its join point; the server's modified map (fed
  from the chunks each operation changed) says which chunks of its window come as `ChunkData` (encoded at dequeue,
  paced bulk: a delay-based window per peer), the rest it generates; the entity baseline brings every live replicated
  body (resting ones at their exact rest pose). The client applies an operation to a chunk only above its base (live or
  replayed from a 1024-op ring), holds entity events until their copy exists, asks for a model when a replicated
  `Detach` meets chunks without a base, requests chunks it could not keep (full store) or replay, starts its player
  once the chunks within 32 m are current and the baseline has ended. Protocol 8. **M-b:** on a region wider than 48
  chunks (`arena250.session`) a session client's window is the game's (48 x the rows x 48, full height) and follows
  its player: every move is a `WindowUpdate`, entering modified chunks come as `ChunkData`, chunks that leave are
  dropped and come back from the server; playground and drill keep the fixed region window. Measured: the drill's late
  joiner 64 KiB of bulk instead of a ~236 KiB log, synced about 0.5 s after its `Welcome`; the playground's join 76 KiB
  (larger than its old 64-event log: the plan's first risk); the join adds nothing to another client's event latency on
  its own link; the arena walk (`vortex_server ... arena250.session --client-window 48 --walk-across ...`): a bot walks
  -118 to +122 m, both bots' hashes over their windows equal the server's, store at most 5576 of 12 288 slots, the
  walker's join 855 KiB (737 KiB of chunks) and 1.9 MiB of `ChunkData` over the walk, event latency during the joins p99
  183 ms on a 5 %-loss, 40-70 ms link (the stream's floor there), RSS 1.1 GiB for the server and two bot worlds (the
  server alone 0.5 GiB), server world 48 MiB RLE; a headless game client on the arena keeps its hash equal to the
  server's over its window and shows `JOINING ... N chunks` while it joins. One fix in the integration: an installed
  chunk is a whole-chunk edit for physics. The design's p99 250 ms is not met on a 64 kB/s, 5 %-loss link even without
  a join (21 ticks; the unit test asserts the join's cost relative to that).
  **Review of 7a** (16 findings, 14 distinct, two more found while verifying; every bug first a failing GPU-free test;
  10 new tests, **671 in all (six disabled)**): an oversize bulk message waits only for the peer's bulk in flight
  (transports count bytes in flight per channel; it waited for an idle event stream before), the pacer's drain counts
  the events and its base RTT ages (12 x 10 s; halving once per round trip), the start gate opens on `BaselineNear`
  (message 15: the baseline's entities within 32 m of the spawn; 229 crates: start 0.15 s, baseline end 1.08 s), a
  moving client window follows with one chunk of slack and sends every step (`World::TakeWindowSteps`: two steps between
  `Poll`s, normal in the game, reset the whole window before; the arena's wandering bot 4.6 -> 3.1 MiB of re-sends),
  the baseline names the tick it was stepped on, chunk sync walks only entering slabs and only Unsynced records, bulk
  installs are bounded per `Poll` (64) and timed (the game on the arena: 0.017 ms of chunk sync per `Poll`, at most
  2.0 ms of installs), repeated `ModelRequest`s are answered once and stale answers discarded, the join latency target
  is asserted at 128 kB/s and 1 % loss (13 / 12 ticks; at 5 % loss it is unreachable on any link) and by
  `vortex_server --max-join-p99-ticks 15` in the drill, hostile sync messages and per-coordinate requests are tested,
  every test has a 180 s timeout.
  **Recheck of 7a** (6 findings, all real; 3 new tests, one extended, **674 in all (six disabled)**): bulk goes only
  while the bytes in flight are under the window again (the review's oversize fix let one message go whatever the
  events in flight; now a larger one goes alone only once its bulk drained and the total is under the window), the
  view radius leaves the follow slack out (88 m for a session client: the window's edge never inside the edge fog), a
  batch of chunks catching up walks the op ring once and decodes each op once (the 16-op FIFO cache never hit), the
  server's per-coordinate request counts are a test-only setting, `--max-join-p99-ticks N` requires under N, docs and
  log lines match the code. While a peer's events alone fill its link its join waits (no reserved bulk share).
  **Not done:** 7b (`HashCheck`, server terrain mode, the wire `EntityCheck`), splitting large bulk messages, encoding
  chunks against their pristine version (join bytes), a client cache of the slab last left (the rest of the re-sends);
  windowed play on the arena is the owner's. Choices: `design/networking.md`, "Stage 7a plan" (with its amendments),
  "Stage 7a as built", "Review of stage 7a", "Recheck of stage 7a".
- **Stage 7b, milestone M-a (`HashCheck` repair, the wire `EntityCheck` and the entity list) - built and verified
  (2026-11-17), not reviewed, not committed.** The plan was amended after a critic (12 findings, all taken, three in
  part): the entity list carries ids and models and the client asks for missing or wrong bodies instead of re-making
  copies; entity checks are skipped when bulk entity data is ahead and compare count and ids while models are asked for;
  checks start at the first window ack; the pristine look-ahead follows the player's path. Protocol 9: `HashCheck`
  (every 2 s per client: the last operations' chunks, 8 modified chunks least recently checked, 12 pristine chunks
  near the feet and on the path ahead), `EntityCheck` (count, xor of NetIds, xor of id-mixed model hashes), both sent
  right after the events and compared at exactly their opSeq; a mismatching chunk is requested again (`ChunkRequest`
  reason Repair) through 7a's path; `EntityListRequest` / `EntityList`; tombstones a FIFO of 4096; the `TerrainAck`
  codec for M-b. 15 new tests (**689 in all, six disabled**), among them the first rows of the test table on a 250 m
  flat region with the game's window and store: the fight (0 false mismatches over 15 checks per bot), a broken chunk
  on a walker's way repaired 0.4-3.8 s before the walker reaches it at walk and run speed, a corrupted chunk and a lost
  copy repaired, entity faults re-sent by the list. `vortex_server` logs `Checks:` lines, fails on a false mismatch and
  shows a repair end to end with `--bot-broken-chunk`; the game logs its checks and `tools/session-check.sh` fails on a
  mismatch. Measured: checks cost the server at most 4.2 ms in one tick for one peer on the arena walk (24 chunk hashes
  and the entity values), a bot at most 2.4 ms per check; 510 B per HashCheck. **Not done:** M-b (server terrain mode,
  its rows of the 250 m table, the Windows-build fixes), the review of M-a. Choices: `design/networking.md`, "Stage 7b
  plan" with "Amendments after the critic", "Stage 7b as built".
- **Stage 7b, milestone M-b (server terrain mode, the whole test table on 250 m, the Windows-build fixes) - built and
  verified (2026-11-17), not reviewed, not committed.** M-a's diff was read again first (one fault fixed: a NetId asked
  for that the entity list does not hold was waited for for ever). A client whose probes differ at the `Welcome`, or
  whose checks find three pristine chunks it generated differently, switches to server terrain mode for the session:
  its generator stops making the server's terrain, every `WindowUpdate` carries the flag, the server answers with a
  `TerrainAck` (uniform chunks inline, the others as `ChunkData` nearest first in 3D) and the client installs them by
  the server's packing (`voxel::InstallMode`: buried chunks stay packed without a store slot, whatever the client's own
  classes say). Requests for Unsynced chunks leave the slots of the content still to come (a terrain-mode window step
  otherwise looped requests against a full store). No wire byte changed (protocol 9). 13 new tests (**702 in all, six
  disabled**): the rest of the networking test table on the 250 m flat region (a generator broken everywhere joins in
  terrain mode with equal hashes; three broken pristine chunks switch to it; a late join during 10 casts/s with a
  terrain-mode joiner, p99 8 ticks for all at 128 kB/s per peer; walk in from outside; chunks that load late replay
  from the ring while the window moves; a small store in terrain mode; the border blast while the window steps; the
  collapse outside one window; a blast at the region edge and ceiling beside a straddling window), a noise-terrain
  terrain-mode test with packing, the World install modes and the client's `TerrainAck`. `vortex_server
  --bot-broken-generator N` shows terrain mode end to end (`Terrain:` lines); the game logs it and its HUD says
  `TERRAIN FROM SERVER`; `Enet.cpp` defines `NOMINMAX` / `WIN32_LEAN_AND_MEAN` on Windows. Measured: a terrain-mode
  join of the playground is **16.2 MiB** (7997 ChunkData; the design said 3-10 MB), the game's whole window on the arena
  **40 MiB** (est. 8-25 MB) and 0.8 MiB per two-chunk window step while walking there; the terrain-mode walker was
  never held and its store peaked at 5513 slots; a headless game client in terrain mode (`--other-terrain`, a test
  switch) ends with the server's hash on the playground and the arena. **Not done:** the review of 7b (M-a and M-b), splitting large bulk messages, a larger bulk ceiling for a
  terrain-mode peer on a quiet link, windowed play, the Windows build itself and the two-machine run (the owner's).
- **Stage 7b integrated and accepted (2026-11-17), not reviewed, not committed.** The whole 7b diff was read against
  the plan; nothing broken, three things half done and fixed: a server check cost up to 5 ms for one peer in one tick
  on the arena (`ChunkHash` now skips runs eight voxels at a time - same hashes - and the server caches each object's
  model hash; now at most 1.35 ms, 213 ms over the arena walk instead of 407 - one run; across runs 0.41-3.87 ms per
  peer and tick, see networking.md "Review of stage 7b" finding 4), the `Join:` lines' "synced after" could
  read 0 s, a misplaced doc comment. One new test (**703 in all, six disabled**). Accepted on one machine with 0 check
  mismatches everywhere: the drill (also with 3 clients and over UDP), `tools/session-check.sh`, the arena walk, a
  broken chunk repaired, terrain mode with `vortex_server --bot-broken-generator` on the playground (16.3 MiB join) and
  on the arena walk (0 ticks held, also at a 100 ms round trip; store at most 5513 slots), and headless game clients
  with `--other-terrain` on the playground (16.3 MiB, every chunk in 15 s on localhost) and the arena (the game's whole
  window 40 MiB, 28 s), equal hashes and entity lists, `TERRAIN FROM SERVER` on the HUD. Numbers: `design/networking.md`
  "Stage 7b as built", "Integration and acceptance". **Not done:** ~~the review of 7b~~ (below) and the commits, the
  Windows build and the two-machine run, windowed play in terrain mode (the owner's), splitting large bulk messages, a
  larger bulk ceiling for a terrain-mode peer.
- **Stage 7b reviewed (2026-11-17), not committed.** Three reviewers, 12 findings: 11 real and fixed (one in part), one
  part rejected (a flat-ground packing row tests no packing); each bug fix has a test that fails on the old behaviour
  (`design/networking.md` "Review of stage 7b"). **Protocol 10**: a `TerrainAck` codes the window step's entering cells
  only (`net::ForEachEnteringChunk` order), 0.67 KiB per step instead of about 6.8 (all acks of the arena walk 46 KiB
  instead of 409). Server: a terrain-mode step costs 1.2-1.3 ms on average (was up to 13 ms: the whole box walked, the
  whole queue stable-sorted), window updates prune queued chunks that left the window (the queue grew without bound on
  a slow link), only the nearest 1024 queued chunks are kept sorted, at most one `EntityList` per client per second, a
  tick-time histogram in the logs (the terrain-mode arena walk: 0.5 ms average, p99 under 3.5 ms). Client: a check
  entry skipped while its chunk loaded is compared once the chunk is current (a broken chunk first picked while loading
  was never checked again before the player stood on it), a chunk that keeps differing after its repair is logged, not
  re-asked every 2 s, hidden data that installs packed no longer keeps free slots from Unsynced chunks, and a
  server-hidden chunk is shown when an edit next to it or an operation this client skipped would show it on the server.
  Check costs recorded: game client 2.0-3.7 ms at most per check in three-process runs (about 0.3 ms of hashing),
  server 0.4-3.9 ms per peer and tick. 7 new tests (**710 in all, six disabled**); the whole acceptance re-run green.
  **Not done:** the commits; the Windows build and the two-machine run, windowed play in terrain mode (the owner's);
  splitting large bulk messages, a larger bulk ceiling for a terrain-mode peer (stage 8 measures first).
- **Stage 7b rechecked (2026-11-17), not committed.** 3 findings, all real: `ServerStats::entityListsDelayed` counted
  ticks of waiting instead of delayed lists (now once per list; the hostile-messages test expects exactly 1 and the old
  code gave 57), and three doc fixes (the server-hidden install rule as the review left it, `TerrainCode::Data` is a
  `Content` install, the 1.35 ms server check figure marked as one run). No wire change (protocol 10); **710 tests,
  six disabled**; the whole acceptance re-run green. New open point: `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing`
  can fail under `ctest -j8` (its joiner's world loads on worker threads): make it wait in ticks for B's window or
  run it serially (`design/networking.md` "Recheck of stage 7b").
- **Stage 8 built and verified on one machine (2026-11-17/18), not committed.** The soak: `vortex_bot` (one bot player
  per process over real UDP; its impairment a `net::UdpRelay` below ENet: loss, delay and jitter, an outage, a slow
  downlink) and `tools/soak.sh` (the server and 4 bots on the playground and the 250 m arena, 10 casts a second, a late
  joiner behind 100 kB/s, an injected broken chunk, a broken generator (server terrain mode) on both regions, and with
  `GAME_CLIENT=1` a headless game client that joins the world the fight left; 60 s or 10 minutes; a self-test that
  must catch four faults). The bot code is shared (`vortex.net:bot`: `vortex_server`'s bots and the game's drain and
  exit lines use it). The plan was amended after a critic (16 findings; design/networking.md "Stage 8 plan"). Fixed on
  the way: the owner's 126 ms join stall (the spawn's colliders are built at start; the join no longer flushes every
  collider) and 207 ms leave stall (cached chunk hashes), the flaky late-join test (deterministic now), two
  ring-wrap bugs the soak found under 10 casts/s (a client asked for every chunk still loading - 21 000 requests a
  minute on the arena - and a terrain-mode client showed every chunk the server keeps packed - a full store on the
  arena; now only chunks near a dropped operation count), the bulk pacer under loss (its window collapsed: terrain-mode
  joins 1.6-1.7x faster now), and a drain that waited for a body that never rests. **Reviewed (2026-11-18, "Review of
  stage 8": 20 findings, all taken, one in part):** `vortex_bot`'s clock left the time inside each pass out (it ran
  slow and jumped forward every second: the "presenting bot's 278 corrections a minute" was this, now 35), the soak
  did not judge the game client's own checks, a missing broken-chunk candidate, the drill (whose report counted the
  wrong casts on long runs), joins, repairs, a bot that stopped playing, a repaired chunk that differed again, or the
  positions of objects not resting; the rates were averaged over each bot's whole life; the 1 %-loss latency target
  was claimed where it is missed. All fixed or restated, and the 10-minute soak rerun on both regions passes.
  **Rechecked (2026-11-18, "Recheck of stage 8": 8 findings, all taken, one in part):** the latency text did not
  match run 4's logs (the arena walker's quiet p99 is 1.49 s, not "did not recur"), the soak's terrain-mode check on
  the game client could never fire, leave lines were matched by slot alone (a reused slot compared the wrong client),
  the relay test still judged its outage by thread timing, the walk gate's span was 20 % too long on small regions
  (a walker that reached every waypoint failed a share over 0.8), the server kept every pass time, and the soak's game
  client ran unbounded. All fixed; the short soak, its self-test and every other check pass again.
  Measured ("Stage 8 measurements", restated from that rerun): at 5 % loss and 40 + 0-30 ms each way event latency
  p50 0.09-0.11 s and quiet p99 0.58-0.79 s for the plain-link players that stand or wander, but **1.49 s for the
  player crossing the arena** (the one with the most bulk; not explained, the open walker A/B), 0.85-1.29 s behind a
  100 kB/s link (on one playground bot the joins added about a tick - one bot, not a general result; at 1 % loss
  0.26-0.31 s, over the 250 ms target on real ENet), **30-32 kB/s down per player in play** at 10 casts/s on both regions (events 23-24.5;
  plus up to 45 kB/s of bulk for a player crossing the arena), no cast ever delayed, server tick p50 under 1.25 ms and
  p99 under 14 ms on this laptop with everything on it (join ticks at most 8 ms, 0 colliders), the arena server 583
  MiB, client stores at most 5813 slots (7498 in terrain mode), corrections 0-38 a minute of play, every hash, entity
  and awake list equal after both 10-minute runs. 12 new tests (**722 in all, six disabled**). No wire change
  (protocol 10). **What M1 still
  lacks before acceptance:** the owner's LAN run on stage 8's commits with the real game clients
  (docs/two-pc-test-windows.md 6.5, the short soak as its pre-check). Open points left for later milestones: the pacer
  on a slow, lossy, jittery link (a 100 kB/s joiner's chunks come at 21-27 kB/s whatever the bulk window: M6),
  re-sent chunks and model answers for a player crossing a large region (half a walker's bulk, 22 KiB/s; the memory
  milestone), bodies frozen awake where no collider can be (they count against the awake cap), and the 10-minute walker
  A/B (R10: the walker's 1.49 s quiet p99; recommended before M6's pacer work, not an M1 blocker). Also fixed: a
  player joining where another stands saw a black frame (the camera inside the other's figure; spawn points per slot
  belong to M2).
- Stage 9: not started (on-demand server world; behind M2 by the owner's decision). Before stage 3: shred.md stage 1 changed the search result (face flood), the server
  window margin (3 chunks, flood box) and the cast budget (a work budget); shred.md stage 4 (built, the owner's answer
  to its question 4) moved the island search onto the cell graph - same results except where a work budget runs out,
  node visits and chunk graphs used (at a cold price; never graphs built, which depend on the cache) to count in stage
  3's cast budget; shred.md stage 4b (built) adds the load check: more `Detach` operations per blast (a tower on a
  thin neck), no new op kind, its counted work joins the cast budget and the strength table must reach `buildHash`;
  see networking.md "Changed by shred.md".


---

## Errata

- Relative links above were written for `engine/docs/ROADMAP.md`: from this file, prefix them with `../../`
  (for example `../../design/networking.md`).
- State words above are those of the day each bullet was written. Stages 7b and 8 were committed later: 7b as engine
  `9cbfdaa` / game `7082a1c`, stage 8 as engine `430fde0` / game `10197d2`. The owner's two-machine run (stage 5's
  acceptance) passed on 2026-11-17 at 7b's commits. Current state: `ROADMAP.md` "State" and the "Stages" table of
  `design/networking.md`.
