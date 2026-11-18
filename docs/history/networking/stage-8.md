# Networking history: stage 8 plan, amendments, as built, measurements, review and recheck

Moved verbatim from `docs/design/networking.md` lines 2384-3147 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

### Stage 8 plan (2026-11-17)

Status: **the plan as written before building; amended after a critic ("Amendments after the critic", at the end of
this section) and built ("Stage 8 as built", which wins where it differs).** Row 8 of "Stages": the soak, one manual session with the real game client, every "est."
replaced by a number, the cast-queue delay histogram; it closes M1 (the owner's two-machine run of 2026-11-17 is
stage 5's acceptance and passed; M1's acceptance after stage 8 is the owner's second LAN run on stage 8's commits,
"Manual session" below). Code facts were read at engine `71dc23e` / game `ad0fe69` (stages 0-7b committed, protocol
10). Everything below builds on "Stage 7b as built", its review and recheck; nothing there changes unless a row says
so. Stage 9 is out of scope and nothing here makes it harder (the bot and the soak are clients; the server change is
one collider focus). "est." = estimate, replaced by the measurement named.

#### What the code gives stage 8

| Code fact (71dc23e) | Consequence for stage 8 |
|---|---|
| `tools/server/main.cpp` (1057 lines) holds the in-process bots: `BotClient` (transport chain, `Client`, `Simulation`, `BotPilot`, latency samples, join ticks), building the bot's `Simulation` from the `Welcome` (the game's store and packing with `--client-window`), `Attach` with a polling callback, the per-pass tick loop (`TicksDue`, `RunTick`, the pilot, `PresentBlast`), the broken-chunk choice (`usable()`), `ResidentMiB`, `P99`, and every per-bot report line (`Client N movement / objects / window / UDP`, `Join: bot N`, `Latency: bot N`, `Checks: bot N`, `Terrain: bot N`, `Client 0 walk`) with the exit rules. | A second tool must not copy this: the bot half moves into `vortex.net` (below); `vortex_server` keeps the server, its transport, the drill and the server-side comparisons. |
| The game's `src/Session.cpp` has its own drain (`Drain`: applied == `ServerOpSeq`, every copy resting, `Synced()`, stable for 1 s) and prints `Session: slot S applied opSeq A of the server's B; world hash H (N chunks) over [box]` and `Session: slot S entities at opSeq A: ...`, which `tools/session-check.sh` parses. | The bot prints the same two lines through one shared function, and drains by the same rule (moved, not copied). |
| `ImpairedTransport` works on the **sending** side, above the transport; `EnetSettings::receiveLoss` drops incoming datagrams below ENet. `vortex_server --enet` puts delay and jitter above ENet. `Impairment::peerBytesPerSecond` limits a sender's per-peer link. | A bot in its own process can only impair what it sends this way; a slow **downlink** emulated above ENet on the receiving side would queue data that ENet has already acknowledged, so the server's pacer (in-flight bytes, RTT) would never see the bottleneck. A real downlink limit has to sit below ENet: see "Impairment on a real UDP socket". |
| `vortex_server` measures event latency as server ticks between `madeAt[opSeq]` (`TickNumber()` after the pass that made it) and the pass in which an in-process bot applied it. `EventBatch` carries the server `tick`, but a separate process does not know the server's current tick. | Latency across processes needs a shared clock: one host (the soak runs on 127.0.0.1) and the server's timeline file (below). No wire change. |
| `ServerStats` already has the cast-delay histogram in ticks (`castDelay[0..7]`), cast wall time (`castSeconds`, `maxCastMsPer10k`), the tick histogram (0.25 ms buckets, `TickPercentile`), `maxSnapshotBytes` / `snapshotBytes`, entity event bytes, bulk window min / max, check costs; `ClientStats` has `maxStoreUsed`, join ticks / seconds, sync and bulk CPU; `PredictionStats` corrections per minute, the error buckets, replays. `EnetStats` counts messages and bytes in all, not per channel. | Most measurements exist as counters; stage 8 adds a few histograms and per-channel bytes and runs them for 10 minutes over real UDP. |
| `Server::Impl::OnHello` creates the player, then `simulation->Terrain().Update(PlayerFocus(), colliderRadius)` and `Terrain().Flush()` (blocking) in the tick that receives the `Hello`. `PlayerFocus()` holds the players only: a server with nobody connected has no collider near the spawn. | The suspect of the owner's "a pass took 126.4 ms at tick 3877" at the first join (the second join found the colliders built): every collider within 12 m of the spawn cooked synchronously. To be confirmed by the measurement first ("The join stall"). |
| `vortex_server`'s wall clock starts before `Server::Start` (`wallStart`), and `lastWall` starts at 0: the first pass after a 10 s arena preload owes 600 ticks and catches up two a pass. | Tick-based numbers of a listening server are skewed for its first seconds (7a: "started after 12 s of server ticks, 2.4 s of wall clock"). Fixed in stage 8 (the clock starts after `Start`). |
| `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing` (`RunLateJoin`) runs without `waitIdle` and without `rngPerPeer`; the server's collider cooks run on worker timing, so which drill pieces become server bodies (`TakePieceSlot` reads Jolt's sleep state) and therefore the joiner's model requests and bulk depend on load. Under `ctest -j8` B asked for 70.9 KiB instead of 44.3 and was not synced in the 216 measured ticks. | The flaky test (recheck of 7b, (e)). `ChunkSync.LateJoinDuring10CastsPerSecondConverges` already runs with `waitIdle` and `rngPerPeer` and is stable. |
| `tools/CMakeLists.txt` adds every `tools/*/` with a `CMakeLists.txt`; `vortex_assert_gpu_free` is called per target. | `engine/tools/bot/` is picked up by itself; it calls `vortex_assert_gpu_free(vortex_bot)`. |

#### Open points handed to stage 8

| # | From | Open point | How stage 8 settles it |
|---|---|---|---|
| O1 | 7a risks, review of 7a 7, recheck of 7a | Splitting large bulk messages (`ChunkData` up to 98 KB, a large model's `BaselineEntity`): one message alone holds a slow link's events for its length. | Measured: the largest bulk messages and `bulkOversize` over the soak, and the 100 kB/s joiner's own event latency during its join against its quiet p99. Rule in "Decisions from the numbers". |
| O2 | 7b review, open (c); 7b M-b | A terrain-mode bulk ceiling: the pacer's 32 KiB window caps a terrain-mode join at about 1.9 MiB/s on a LAN (40 MiB for the arena window). | Measured: the terrain-mode bot's join time and bytes over real UDP at 40 + 30 ms and 5 % loss; one run with a larger ceiling. Rule below. |
| O3 | 7a M-b, review of 7a 6 | Re-sending edge chunks (a client walking back and forth at a modified area's edge re-fetches it; the hysteresis cut it by a third). | Measured: per bot, `ChunkData` received for coordinates it had already been sent (a re-send count, new) and their bytes per second. Rule below. |
| O4 | review of 7a 7 | The absolute latency target: p99 under 250 ms (15 ticks) at 5 % loss was unreachable on the in-process link (a lost reliable packet costs about 130 ms plus head-of-line). | Measured over real ENet: p50 / p99 / max of every bot's event latency, quiet and during joins, at 5 % loss, and at 1 % in one short run. Rule below: the target is restated from the numbers. |
| O5 | recheck of 7a, new open point | While live events alone fill a joiner's link, its bulk waits (no minimum bulk share). | Measured: the throttled joiner's "join data in" time and `bulkHeld` during the drill at 10 casts/s. Rule below. |
| O6 | 7b review 4 | Check cost on the game client (2.0-3.7 ms at most in three-process runs) and on the server (0.41-3.87 ms per peer and tick). | Measured in the soak (bots: `Checks:` lines; the game client in the manual run). Recorded; the knob (fewer entries for a standing player) only if a game client logs over 4 ms. |
| O7 | owner's two-machine run | One 126 ms server pass at the first join. | "The join stall" below. |
| O8 | recheck of 7b (e) | `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing` is flaky under `ctest -j8`. | "The flaky test" below. |
| O9 | stage 3 "Cast wall time" | "Stage 8's histogram compares this with the work units (calibrated at 0.40-1.66 ms per 10 000)." | The cast-cost histogram (ms per 10 000 work units) beside the cast-delay histogram. |
| O10 | owner's two-machine run | Client and server logs (corrections per minute, check counters) were not collected. | The manual session's report list asks for them; the soak gives the one-host numbers. |
| O11 | 7a risks, 7b | Encoding chunks against their pristine version (join bytes); a pristine chunk off every player's path is never checked; the `--listen` refusal question of stage 5. | Not stage 8's: left open as they are (recorded again in "Stage 8 as built"). |

#### What moves out of `vortex_server` (shared, no copy)

A new partition **`vortex.net:bot`** (`src/net/Bot.cppm`, `src/net/internal/Bot.cpp`; imports `:client`, `:drill`,
`:server` only for the server-side helpers, `:entities`, `:sync`; GPU-free like the rest of `vortex.net`). Both tools
and the game use it; `tests/net` tests it. Choice: a partition of `vortex.net`, not a tools library: one module per
directory, `BotPilot` and `BlastDrill` already live there, and a `tools/common` would need its own module and target.

| Piece | Today in | Becomes |
|---|---|---|
| A bot client: transport given by the owner; `Client` + `Simulation` built at the `Welcome` (the game's store 12 288 / headroom 2048 and session packing when `windowChunks > 0`; job threads; region capacity otherwise); `Attach` with the owner's polling callback; per pass the ticks due with the pilot's controls (none while standing or draining); `PresentBlast` when presenting; casts shown; x range; join ticks | `BotClient` + `stepClients` lambda | `net::BotSettings` and `net::Bot` (`Pass(seconds, acting, whileLoading)`, `Client()`, `Simulation()`, `Pilot()`, `Counters()`). A broken-chunk bot stands until that chunk was installed from data (or 30 s), then follows its pilot (7b's bot stood for the whole run). |
| Latency samples: per opSeq the time it was applied (a callback of the owner: server ticks in `vortex_server`, steady-clock nanoseconds in `vortex_bot`); `P99` | `BotClient::latencies`, `P99` | `Bot::RecordApplied(now)`; `net::Percentiles` (p50 / p99 / max, nearest rank) shared with the tests' copies of `P99` |
| The per-bot report lines `Client N movement`, `Client N objects`, `Join: bot N`, `Checks: bot N` (client half: `DescribeChecks` + verdict), `Terrain: bot N` (client half), `Client N window`, `Client 0 walk`, `Client N UDP` | `main.cpp` report | `net::DescribeBot(const Bot&, BotReportContext) -> std::vector<std::string>`, byte for byte the current texts (the docs' and scripts' regexes keep matching); `vortex_server` appends its server-side halves (server hash over the sync box, `Terrain: ... server:` costs, the packing comparison against the server's world) as now |
| The bot-side verdict: synced, hash complete, `skippedChunks` 0, probe mismatches only in terrain mode, no mismatch or check out of order unless injected, the broken chunk repaired, terrain mode when the generator is broken, the store gate (`maxStoreUsed + 2048 <= capacity`, no store-full load / decode, no unsynced chunk) | `main.cpp` exit rules | `net::JudgeBot(const Bot&, BotGates) -> BotVerdict{ok, reasons}`; `vortex_server` adds its comparisons with the server (hash, entities, packing) |
| The drain rule (applied == `ServerOpSeq`, every copy resting, `Synced()`, held for 1 s) | game `Session.cpp` `Drain` | `net::DrainWatch` (feed it each poll, it says when the rule has held for the time given); the game and `vortex_bot` use it; `vortex_server` keeps its in-process `settled()` (it reads the server directly) |
| `Session: slot S applied opSeq ...; world hash ... over [box]` and `Session: slot S entities at opSeq ...` | game `Session.cpp` | `net::SessionHashLine(const Client&)`, `net::SessionEntitiesLine(const Client&)`; the game prints them through these (same text) |
| The broken-chunk choice (pristine, server-Dense, not a probe, within 12 m of the spawn, nearest) | `main.cpp` `usable()` | `net::BrokenChunkCandidate(const Server&, std::string* why)`; `vortex_server --broken-chunk-candidate` logs it for a script |
| `ResidentMiB` | `main.cpp` | `net::ResidentMiB()` (Linux `/proc/self/status`, 0 elsewhere) |

`vortex_server`'s behaviour and log text do not change with the move (checked by running the CLAUDE.md drill, the UDP
drill and the arena walk before and after and comparing their summary lines).

#### `vortex_bot`

`engine/tools/bot/main.cpp`, `add_executable(vortex_bot)`, links `VortexNet` and `vortex_warnings` only,
`vortex_assert_gpu_free(vortex_bot)`. One process = one player: a `net::Bot` on the wall clock.

```
vortex_bot --connect HOST[:PORT] --token HEX [--id N] [--seed S]
           [--seconds S | --end-at UNIX_S] [--leave-at UNIX_S] [--drain-seconds S]
           [--client-window N] [--threads T] [--present]
           [--stand] [--no-jumps] [--casts N] [--crates N] [--walk-across [--loop]]
           [--broken-chunk X,Y,Z] [--broken-generator]
           [--loss P] [--delay-ms D] [--jitter-ms J] [--outage-ms A,B]
           [--down-bytes-per-second R] [--up-bytes-per-second R] [--relay-seed S]
           [--timeline FILE] [--memory]
```

- **Loop.** As `vortex_server`'s UDP pass: sleep 1 ms, `passSeconds` from the steady clock, `Bot::Pass`,
  `Flush`. The connect waits up to 15 s for the `Welcome` (refused, no answer: exit 1, as the game). The region load
  runs inside `Attach`'s callback (polling, so ENet stays serviced). `--client-window` defaults to **48** (the game's:
  a fixed window on the playground, a moving one on the arena); `--threads` to 2 job threads; physics 1 thread.
- **Phases.** Acting until `--seconds` after the `Welcome` or the wall time `--end-at` (UNIX seconds, so a script can
  end every bot at once); then standing and draining (`DrainWatch`, at most `--drain-seconds`, default 60; 180 in
  terrain mode); then connected and draining again until `--leave-at` (default: at once), so the script's bots leave
  one by one in a known order (each leaver's crates are despawned after it: the next one drains them before it
  leaves). It leaves only when the drain rule holds at that moment (at most 10 s past `--leave-at`), logs its exit
  lines, disconnects (ENet's graceful disconnect, 0.5 s wait) and exits.
- **Pilot.** `--casts N` (a cast every N ticks), `--crates N`, `--stand`, `--no-jumps`; `--walk-across` = 7a's
  waypoints (the play bounds' -x edge to the +x edge along the middle z and back), `--loop` repeats them until the
  acting phase ends (new `PilotSettings::loopWaypoints`); the seed is `--seed`, default `0xb07 + id`.
- **Faults.** `--broken-chunk X,Y,Z`: the bot's generator makes that chunk differently (`BreakChunks`); it stands until
  the chunk was repaired. The coordinate comes from the server (`--broken-chunk-candidate`): only the server knows
  what is modified. `--broken-generator`: the region's generator with terrain seed + 1 (flat: ground 12 voxels higher),
  as `--bot-broken-generator`. Each is an injected fault: the verdict expects its mismatches and nothing else.
- **Exit lines** (the formats above, so one script parses bots, `vortex_server` bots and game clients alike):
  `Session: slot S applied opSeq A of the server's B; world hash H (N chunks) over [box]` and `Session: slot S entities
  at opSeq A: ...` (the game's); `Client N movement: ...` (plus `final error E m`: the predicted feet against the last
  snapshot's after the drain, the M1 fight's "own position within 5 cm"), `Client N objects: ...`, `Join: bot N ...`,
  `Latency: bot N: ...` (below), `Checks: bot N: ...`, `Terrain: bot N: ...`, `Client N window: ...`, `Client N UDP:
  ...` (`vortex_server`'s, N = `--id`); new: `Traffic: bot N: up U B/s average (peak P in 1 s), down D B/s average
  (peak Q); by channel state / events / bulk up ... down ...; join phase down J B/s` (payload bytes counted in
  `EnetTransport`), `Relay: bot N: up / down datagrams, bytes on the wire (ENet headers, acknowledgements and resends
  included), dropped by loss / outage / queue, largest queue delay` and, with `--memory`, `Memory: bot N: RSS now / at
  most ... MiB, store at most ... slots`. Last line: `Bot: N ok` or `Bot: N FAILED: <reasons>` (`JudgeBot`).
- **Exit code** 0 when `JudgeBot` passes (synced at the drain, hash complete, the gates above, injected faults found
  and repaired, not disconnected by the server); 1 otherwise; 2 on a bad command line or an unresolvable host. The
  comparison with the server is the script's (the bot cannot see the server's world).

#### Impairment on a real UDP socket

**Choice: a UDP relay below ENet, in the bot process** (`net::UdpRelay` in `vortex.net:enet`, implemented in
`src/net/internal/Enet.cpp` with ENet's portable socket calls `enet_socket_*` / `enet_socketset_select`, so no new
platform code and `<enet/enet.h>` stays in that one file). With any impairment option the bot starts a relay on
127.0.0.1 (an ephemeral port), and its `EnetTransport` connects to the relay; the relay forwards to the server. Why not
`ImpairedTransport` on top: above ENet it can only delay what the bot **sends**; a receive-side delay above ENet would
hold data ENet has already acknowledged, so the server's ENet RTT, its reliable bytes in flight and the bulk pacer
would never see a 100 kB/s downlink, and the soak would measure an unbounded queue in the bot instead of the pacer.
Below ENet every effect is real: ENet resends lost datagrams, its RTT includes the queue, its throttle reacts, the
server's pacer sees the bottleneck. No root, no `tc`, nothing outside the bot's own sockets.

| Part | Rule |
|---|---|
| Thread | One thread per relay: `enet_socketset_select` on its two sockets with a timeout up to the next due datagram (at most 1 ms), so the bot's own stalls (a region load, a long tick) never stall the network. Stats are atomics; `Stop` in the destructor. |
| Sockets | Down side: bound to 127.0.0.1:0; the first datagram's sender is the bot's ENet (later ones from elsewhere are dropped and counted). Up side: an unbound socket that sends to the server's address; the server sees the relay's port as the peer. |
| Per datagram, each direction | Outage (relay clock in `[a, b)`, `--outage-ms A,B` from the bot's start): dropped, both directions. Loss (seeded PCG per direction): dropped. Rate (`--down-` / `--up-bytes-per-second`): a FIFO bottleneck, departure = max(now, last departure) + size / rate; when the queue would hold more than `queueBytes` = max(16 KiB, 250 ms of the rate) the datagram is dropped (drop-tail, like a home router). Then delay + uniform jitter (`--delay-ms`, `--jitter-ms`), released in due order: jitter may reorder datagrams, as netem does; ENet orders them. |
| Defaults | No option: no relay, the bot connects straight to the server. `--loss` applies to both directions (the soak's 5 % is per direction, as `--loss` is on `vortex_server`'s links). |
| Stats | Per direction: datagrams and bytes in / out, dropped by loss / outage / queue, the largest queue delay; logged on the `Relay:` line. |

#### Event latency across processes: the server's timeline

`vortex_server --timeline FILE` writes, flushed once a second: `ops <lastOpSeq> <steady ns>` after every pass that
made events (the steady-clock time at the start of that pass), `join <slot> <steady ns>` when a client joins and
`joindata <slot> <steady ns>` when its join data has all gone out (new `Server::SetOnJoinDataSent(slot)`, called where
the server's `Join: client N` line is logged). `vortex_bot --timeline FILE` records the steady-clock time of every
opSeq it applies (after every `Poll`, as `vortex_server` does per pass) and, at exit, reads the file and computes each
event's latency = applied - made: **p50, p99, max in ms and in ticks (ms / 16.67)**, for all events and for the events
made while some client was joining (between a `join` and its `joindata`), on the existing `Latency: bot N:` line with
`(wall clock, one host)` appended. Valid only on one host: `std::chrono::steady_clock` is the system-wide monotonic
clock on Linux and Windows, the same for every process of the machine. Choice over an estimate from `ServerTime`
(snapshot ticks plus half an RTT): exact on the one machine the soak uses, no wire change, and it includes the
server's own lag (an event made late by a server hitch is late for the player too). A file of about 36 000 lines for
10 minutes (1 MiB).

#### The soak script

`tools/soak.sh` **in the game repository** (next to `tools/session-check.sh`: the same `build/` layout, and it can add
a headless game client). It needs only `vortex_server` and `vortex_bot` unless `GAME_CLIENT=1`.

```
tools/soak.sh [--short] [OUT_DIR]          (default build/soak/; --short: 60 s per scenario, no terrain-mode bot)
BUILD=build PORT=27040 SOAK_SECONDS=600 SCENARIOS="playground arena" TERRAIN=1 GAME_CLIENT=0 tools/soak.sh
```

- **Safety.** `set -u`; every variable gets a value before it is used in a path (`OUT=${1:-$BUILD/soak}` and so on);
  `PORT` defaults to 27040 and the script refuses 27015 (the owner's live server); `--bind 127.0.0.1` and a fresh
  random token per scenario; every process runs under `nice -n 10` (the owner's server on this laptop keeps priority);
  only the PIDs the script started are waited for or killed (a `trap` on EXIT kills its own children, never by name).
  A 10-minute run is started in the background by whoever runs it (two scenarios: about 25 minutes with preload and
  drains).
- **Scenario `playground`** (`playground.session`, fixed 30 x 44 x 30 windows, 5 % loss and 40 + 0-30 ms on every
  bot's relay, both directions): the server with `--listen $PORT --bind 127.0.0.1 --token T --exit-when-empty --seconds
  $((S + 300)) --drill N --drill-every 6 --drill-after 2 --drill-delay 2 --drill-sites 2 --timeline F --memory
  --broken-chunk-candidate` (N = (S - 20) x 10: **10 drill casts per second** that end 20 s before the bots stop);
  the script waits for its `listening` and `Broken chunk candidate` lines (at most 60 s). Bots, all ending at `END =
  now + S + 10` and leaving 20 s later one every 3 s:
  - A: at once, `--present --casts 90 --crates 240` (walks, runs, jumps, casts, drops crates, makes rubble like a game);
  - B: at once + 1 s, `--broken-chunk X,Y,Z` (stands until repaired, then plays), `--casts 120 --crates 300`,
    `--outage-ms` of 2 s at 30 % of the run (ENet's 10 s timeout survives it; 10x the in-process test's 200 ms);
  - C: **late join at 50 %**, `--down-bytes-per-second 100000` (the 100 kB/s downlink for its whole life),
    `--casts 90 --crates 240`;
  - D (`TERRAIN=1`): at 20 %, `--broken-generator` (server terrain mode, 16 MiB on the playground), `--casts 150`;
  - with `GAME_CLIENT=1`: one headless game client (`Shredborne --connect ... --headless --validation --frames F`)
    at 10 %, its frames sized to end before `END`, compared like a bot.
- **Scenario `arena`** (`arena250.session`, the bots' 48-chunk moving windows; the server adds `--precasts 40`): A
  `--walk-across --loop` (walks -118 to +122 m and back for the whole run), B wanders near the spawn with `--casts 90
  --crates 240` and the outage, C joins late at 50 % with the 100 kB/s downlink; no terrain-mode bot (7b measured the
  arena's terrain mode: 40 MiB, under the 100 kB/s link a join of minutes; the playground's D measures terrain mode
  under real loss).
- **Comparison** (as `session-check.sh` does, extended to window boxes): for each bot and game-client log, its last
  `Session: slot S applied opSeq A of the server's B; world hash H (...) over [box]` line: A == B (drained), and the
  server's `Session: the client of slot S left; server at opSeq A, ... window hash W over [box]` line with the same
  slot, opSeq and box has W == H; its `Session: slot S entities at opSeq A: C object(s), ..., id hash X` equals the
  server's `Session: entities at opSeq A: ...` line logged right after that leave line (count and id hash). A missing
  server line at that opSeq is a failure (someone made an event after the bot drained: the end phase was not quiet).
- **Exit 0 only when** the server and every bot (and game client) exit 0 (each bot's own verdict: synced, gates,
  checks without a mismatch that was not injected, injected faults repaired, terrain mode where broken), every
  hash and entity list equals the server's at the same opSeq, no bot was disconnected before its `--leave-at`, and
  the server's `Checks: server:` line and `Server: ... clients joined, N left` agree with the bot count. Otherwise exit
  1 with the reasons. A `summary.txt` per scenario collects the measurement lines (grep of `Ticks:`, `Players:`,
  `Server: cast`, `Sync:`, `Join:`, `Latency:`, `Traffic:`, `Relay:`, `Checks:`, `Terrain:`, `Memory:`, `Client N
  movement`).

#### Measurements and the estimates they replace

Taken in the 10-minute soak (both scenarios; a short run first to check the lines) on this laptop, every process
`nice`d, the owner's live server running beside it (said so in the results: it shares the cores). Each row's result
goes into a **"Stage 8 measurements"** table in "Stage 8 as built" with the estimate it replaces; an estimate no run
can measure is marked so, with why.

| Measurement | How it is taken | Replaces |
|---|---|---|
| Cast-queue delay histogram | `ServerStats::castDelay` (ticks 0..6, 7+; exists) with p50 / p99 / max, plus **new** `castCostHistogram`: wall ms per 10 000 work units per cast in 0.1 ms buckets (p50 / p99 / max), and the overdue casts; `Server: cast delay ...` / `Server: cast cost ...` lines | "Time and ticks": the work budget's "about 6 ms" per tick (`kCastWorkPerTick`) and "a unit is about 0.1 us" (stage 3: calibrated 0.40-1.66 ms per 10 000 on a quiet CPU; the soak gives the loaded figure) |
| Event latency p50 / p99 / max | `Latency: bot N:` (timeline, wall ms and ticks), all events and during joins, per bot; at 5 % loss in both scenarios and at 1 % in one short run | "Testing": "event latency p99 under 250 ms during the join"; the absolute p99 at 5 % loss that 7a could not meet (O4) |
| Bytes per second per client, up and down | `Traffic: bot N:` (payload per channel, average and peak 1 s, join phase apart) and `Relay: bot N:` (bytes on the wire) | "Decision in brief": events est. 24 kB/s, snapshots 1200 B at 30 Hz; "Time and ticks": inputs est. 2.5 kB/s (stage 4: 1.9); "Entity replication": per player est. down 7 kB/s quiet, 24 kB/s with 20 awake bodies, cap 37 kB/s + events 24 kB/s, up 2.5 (with 3-4 players, not 5v5: said so) |
| Event bytes by kind | **New** `ServerStats::eventBytesByKind` / `maxEventBytesByKind` (kinds 1-6, 32-36): average and largest per kind; `ticksWaitedForBytes`, `oversizeEvents` (exist) | "World operations": event sizes est. (`Detach` under 200 typical, under 2 KiB at the cap; `ClearMask` 2.4 B per crumb voxel; `ObjectModel` under 100 for a crate); the events budget's 16 kB burst |
| Snapshot sizes | **New** `ServerStats::snapshotHistogram` (64 B buckets): p50 / p99 / max, bodies per snapshot at most, bodies left out, the largest sample gap (exists) on the `Players:` / `Objects:` lines | "Entity replication": snapshot est. (13 + 27 + 17 + 31 per body, 1200 B hold about 36 bodies), "64 awake (est., derived from the snapshot budget)" |
| Bulk inside the transport | **New** `ServerStats::maxBulkInFlight` (`ChannelBytesInFlight(Bulk)` sampled per peer per tick), bulk window min / max, `bulkHeld`, `bulkOversize`, the largest bulk message (exist) | "Transport": "at most est. 32 kB of bulk inside the transport"; 7a's pacer constants (100 ms, 4-32 KiB: "estimates; the p99 test sets them") |
| Bulk per join | Server `Join: client N` and bot `Join: bot N` lines: playground (A, B at start; C after 5 min of a fight at 100 kB/s), arena (C after 5 min), terrain mode (D on the playground under 5 % loss) | "Chunk sync": "Est. after a 5-minute 5v5: 0.5-8 MB per join"; "Region start-up": terrain mode est. 8-25 MB (7b measured 16 / 40 MiB on localhost: re-measured over the relay); "Risks and what 7a leaves": start time at 32 kB/s "many seconds" |
| Re-sent chunks | **New** `ClientStats::chunkResends` / `resendBytes` (a `ChunkData` for a coordinate already received while it was in the window before, kept as a set of coordinates per session) | 7a's re-send cost (O3; 7a measured 3.1 MiB for a wandering bot in 35 s) |
| Server tick time p50 / p99 / max | `Ticks: server:` gains p50 and **the join ticks** (the wall time of each tick that processed a `Hello`, and the largest); the `a pass took` lines | O7 (126 ms); "Stage 9 ... p99 tick under 16.6 ms" (stage 9's gate: recorded for the 250 m arena only) |
| Server RSS over the run | `--memory` every 10 s (exists) plus the largest at the end; the server is now **its own process** (the in-process walks gave server + bots) | "Server world without a camera": 154 m est. 0.5-0.6 GiB, 250 m est. 1.1-1.3 GiB (stage 3: 463-518 MiB for the arena server alone); "Store capacity = dense count after loading x 2 (est.)" (as built: the 2 GiB cap; the server's `store a / b`) |
| Client store use | `maxStoreUsed` per bot (`Client N window:`), `Memory: bot N:` RSS | "Chunk sync": client capacity, est. 5300-5700 dense in a 48-chunk window, 2048 headroom (est.: 600 rubble slots plus casts) |
| Corrections per minute | `Client N movement:` (exists), plus `final error` after the drain; the error buckets | "Player movement": error under 2 cm ignored (est.), replay "13-15 `UpdateCharacter` calls at 150 ms RTT, est. under 1.5 ms" (`maxReplaySeconds`, exists); the M1 fight's "own position within 5 cm after drain" |
| Repairs and checks | `Checks: bot N:` / `Checks: server:` (exist): repairs requested / installed, mismatches by kind (only B's and D's injected ones), lists, check costs (client largest, server per peer and tick) | "Chunk sync": repair est. 4 pristine chunks (as built 12; 510 B per 2 s: re-measured as bytes); the 7b plan's "under 1 ms per check" (O6) |
| Preload and memory gates | the server's `region ... loaded in` line and `Server: world ... MiB RLE` on the arena | stage 3's preload gate 10 s (at its edge) and 150 MiB RLE |

Estimates no stage-8 run measures, marked in the table as such: the stage days (planning history), fixed-point noise
"5-8 days" (not built), `replicatedPieceVoxels` (an owner decision since 2026-11-16, not an estimate), stage 9's
"0.3-1.5 GiB for 20-40 players" (stage 9's).

#### The join stall (126 ms at the first join)

1. **Measure first.** `ServerStats` gains `joinSeconds` (the work inside `OnHello`), split into `joinColliderSeconds`
   and `joinCollidersBuilt` (the `TerrainColliderStats::built` delta of its `Update` + `Flush`) and the rest
   (baseline sort, character); the first `WindowUpdate` of a peer gets `windowAckSeconds` (the entering pass, or the
   `TerrainAck` with its queueing and sort); the server's `client N joined` line prints them, and `Ticks:` the
   largest join tick. Reproduced with `vortex_server --session playground.session --listen 27041 --bind 127.0.0.1
   --token T --seconds 40` and one `vortex_bot` joining after 20 s (as the owner's first join came about 65 s in):
   the expected reading is about 100 ms of colliders for about 100 chunks; a second bot, about nothing.
2. **Fix (if the measurement confirms the colliders).** The template's spawn is a permanent collider focus of the
   server: `ColliderFocus()` = the players' feet + `tmpl.spawn` (the streaming focus is unchanged), and
   `Server::Start` cooks it once (`Terrain().Update` + `Flush`, with the preload). The colliders near the spawn are then
   always built and kept current by the background cooks after every edit; the join's own `Update` + `Flush` stays
   (the gate rule of stage 4: a server player must not be held for another number of ticks than its client) and only
   waits for cooks already in flight. Memory: about 100 more colliders, kept for the session. Not a wire change; the
   template's crates now have ground from the start (they rest instead of being frozen until a player comes), which
   may shift printed counts in the stage 3-7 tests, never their assertions (to be checked).
3. **Other join work, measured by the same counters.** A normal join's entering pass (39 600 cells on the playground,
   the modified map on the arena) and baseline sort are expected under 1 ms. A terrain-mode join's `TerrainAck` on the
   arena measured 8.7-13 ms in 7b: if the split shows the queueing of about 20 000 chunks (dedupe set inserts) or the
   sort dominates, cut that part (reserve, a flat dedupe bitmap over the server window instead of a hash set, the 7b
   nearest-1024 selection instead of a full sort); a code pass over 101 376 cells that dominates is left (it must code
   every cell at one opSeq), recorded. Target: **no join tick over 4 ms** on the playground and the arena in the soak
   (wall clock, logged, not gated: stage 3's rule; the unit test below gates the count).
4. **Also:** `vortex_server` starts its wall clock after `Server::Start` (`lastWall = wallNow()` before the first
   pass), so a listening server no longer races through its preload time after start.

#### The flaky test and the flake hunt

- **Reproduce** before changing it: eight copies of `VortexNetTests --gtest_filter=Session.ALateJoinsBulkIsPaced*`
  side by side, each printing its `[ pacing ]` line (bulk bytes, join ticks, latencies), until two runs differ.
- **Make it deterministic** (`RunLateJoin`): `s.waitIdle = true` (collider cooks inside the tick: which pieces become
  server bodies no longer depends on worker timing, the stage 6 rule) and `im.rngPerPeer = true` (A's losses do not
  move with B's traffic), as `ChunkSync.LateJoinDuring10CastsPerSecondConverges` does; after the 48 drill steps the
  loop goes on in ticks without drill steps until B is synced (at most 900 ticks), so `joinTicks` no longer depends on
  a fixed window. The test then runs the paced join **twice and asserts equal** bulk bytes, join ticks, `bulkHeld` and
  A's latency (it fails today under load), and its bounds are re-set from the deterministic numbers (the relative
  rule `paced <= none + oneMessage + 2` and `unpaced > paced` stay).
- **Hunt**: `ctest --test-dir build -j16 --schedule-random --repeat until-fail:3` three times (oversubscribed on this
  laptop's 8 threads; never while a build runs), and one run while the 60 s soak loads the machine. Every test that
  fails once is reproduced alone under load, then made deterministic the same way (ticks instead of wall time,
  `waitIdle`, `rngPerPeer`); `RUN_SERIAL` (the `VORTEX_SERIAL_TESTS_` filter) only for tests that measure the wall
  clock by design (the ENet and relay tests), with a comment saying why. The findings go into "Stage 8 as built".

#### Decisions from the numbers

Each open point gets a rule now, so the numbers decide; a rule that says "build" builds only if the change is small
(a day or less, no new subsystem), otherwise the decision and the numbers are recorded for the milestone that needs
it.

| # | Measure | Build when | What is built | Else recorded |
|---|---|---|---|---|
| O1 | Largest bulk messages, `bulkOversize`; C's own event p99 during its join vs. its quiet p99 at 100 kB/s | a message over 16 KiB occurs in the soak **and** C's join p99 exceeds its quiet p99 by more than 15 ticks | `ChunkDataPart` (a new message type; **protocol 11**): a chunk encoded at dequeue as now, its bytes sent as parts of at most 8 KiB under the pacer, installed when complete (asOf of the whole); the same for `BaselineEntity`. Tests: round trip and refusals, a 98 KB chunk on a 64 kB/s link holds the live stream at most one part | "no message over 16 KiB in N runs (largest X KiB)": not built |
| O2 | D's terrain-mode join (bytes, start, all chunks in) at 5 % loss, 40 + 30 ms, and the other bots' p99 meanwhile | a short run with `BulkPacerSettings::ceilingBytes` 128 KiB for terrain-mode peers halves D's join time with the others' p99 within + 2 ticks | `ServerSettings::terrainBulkCeilingBytes` (128 KiB) for a peer in terrain mode; the delay-based window still bounds it. No wire change | the measured times and why the ceiling stays |
| O3 | `chunkResends` / `resendBytes` per bot, per second | re-sends over 30 % of a bot's bulk **and** over 8 KiB/s on average | nothing small exists: a "have" list needs a wire change and a client cache of left slabs; recorded with the numbers for the memory milestone | the numbers |
| O4 | p99 of every bot at 5 % and at 1 % loss, quiet and during joins | - | - | **The target is restated** in "Testing" from the numbers: p99 under 250 ms at 1 % loss; at 5 % loss the join adds at most 3 ticks to the quiet p99 (the 7a unit test's rule), and the absolute 5 % figure is recorded as ENet's resend floor (ENet's retransmit timeout is RTT + 4 x variance, not tunable without patching it; not done) |
| O5 | C's "join data in" time and `bulkHeld` while the drill runs at 10/s | C's join data takes over 4x its quiet-link time (bytes / 100 kB/s) | a minimum bulk share: the pacer hands over at least one bulk message per round trip while bulk waits (`BulkPacerSettings::minShare`), with a unit test on the saturated link of recheck 7a 1 | the numbers |
| O6 | the largest check on the game client (manual session, owner's logs) and the bots | a game client logs over 4 ms | fewer pristine entries for a standing player (`hashCheckPristine` by speed) | the numbers |

A protocol bump (O1) makes the owner's Windows build refuse to join (`Protocol`): the owner rebuilds it for the manual
session anyway (same commit on both machines); the guide says so.

#### Manual session with the real game client

- **What an assistant runs:** `tools/session-check.sh` with `PORT=27042` (PASS), and the short soak with
  `GAME_CLIENT=1` (one headless game client beside the bots, compared like them; its screenshot looked at).
- **The owner's live server** (UDP 27015, playground, a Windows player connected; the owner left it running for
  tests): optionally, and only while the protocol is unchanged (10, the same `buildHash`), **one passive** client -
  `vortex_bot --connect 127.0.0.1:27015 --token <the token in the owner's terminal> --stand --id 9 --seconds 30` -
  joins, stands, drains and leaves; its hash must equal the `window hash` of the server's `client of slot S left`
  line in the owner's terminal (read with the terminal tool, never typed into). It casts nothing, drops nothing and is
  never more than one; the server is never stopped or signalled. This checks a Linux client against the Windows
  player's session. (The computed task text said never to use 27015; the owner's own message leaves the server "for
  your tests", which wins; binding 27015 stays forbidden.)
- **The owner's run (an assistant cannot do it):** the LAN session of "Owner's two-machine run" again on stage 8's
  commits: `CLAUDE.md` and `docs/two-pc-test-windows.md` updated to the new commits and protocol (10, or 11 if O1 is
  built: then the Windows build must be rebuilt), plus: start the server with `--memory`, keep the client logs, and
  report the exit lines (`Client ... movement` / the game's `Session:` lines: corrections per minute, `Checks: slot N`,
  `Join: slot N`, the HUD ping), which the first run did not collect (O10). Windowed play stays the owner's to judge.

#### Tests

All GPU-free; wall-clock ones are marked and run serially.

| Test | Asserts |
|---|---|
| `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing` (made deterministic) | two paced runs equal; paced <= none + one message + 2; unpaced worse; B synced (fails today under parallel load) |
| `Session.TheFirstJoinCooksNoColliderInItsTick` | a server started on the playground region, 600 ticks without clients (with and without a cast at the spawn 2 ticks before the join): the first join's `joinCollidersBuilt` is 0 without the cast and at most the chunks that cast changed with it; a second join 0 (fails on the old server: about 100) |
| `Relay.ShapesRealUdpTraffic` (wall clock, serial) | two `EnetTransport`s through a `UdpRelay`: 256 kB/s down, 50 ms delay: 200 KB of reliable messages take at least 0.78 s and arrive intact, ENet's RTT is at least 100 ms, `ChannelBytesInFlight` on the sender grows while the relay queues; 5 % loss drops about 5 % (seeded, both directions); a 300 ms outage passes no datagram; a datagram from a third address is dropped |
| `Bot.TwoSharedBotsJoinPlayAndDrain` | over the loopback with the manual clock: two `net::Bot`s with pilots (casts, crates) and a drill, one joining late; `DrainWatch` settles; hashes over the sync boxes and entities equal the server's; `JudgeBot` passes; with a broken chunk the bot stands until it is repaired and then moves |
| `Bot.LatencyFromTheServersTimeline` | a written timeline (ops, join, joindata) and applied times: the made time of each opSeq, p50 / p99 / max, the during-join selection; a malformed line is refused with its line number |
| `Bot.ReportLinesKeepTheirFormat` | `DescribeBot`, `SessionHashLine` and `SessionEntitiesLine` match the regexes `tools/session-check.sh`, `tools/soak.sh` and the docs use |
| `Wire` / `Enet` extended | `EnetStats` per-channel bytes add up to the totals |
| (if O1 is built) `Wire.ChunkDataPartsRoundTrip...`, `Session.ALargeChunkInPartsKeepsTheLiveStreamMoving` | as in the O1 row |

About 6 new tests (710 -> about 716, six disabled), more if a rule says build.

#### Order of work

1. The flaky test (it makes `ctest -j8` trustworthy for everything after), then the flake hunt's first pass.
2. The join stall: counters and the `client N joined` line; reproduce with `vortex_server` + a bot (until
   `vortex_bot` exists: an in-process `--enet` bot joining late, `--join-step`); the unit test; the spawn focus; the
   wall clock after `Start`.
3. `vortex.net:bot` (the move): `vortex_server` on it with unchanged output (drill, UDP drill, arena walk compared
   before / after); the game's `Session.cpp` on `DrainWatch` and the two line functions; `session-check.sh` PASS.
4. `UdpRelay` and its test; `EnetStats` per channel.
5. `vortex_bot`; the timeline (`--timeline` on both sides); the new `ServerStats` histograms and counters; the
   `Traffic:` / `Relay:` / `Memory:` lines.
6. `tools/soak.sh`; the short soak on both scenarios until it passes; `GAME_CLIENT=1` once.
7. The 10-minute soak of both scenarios, in the background (about 25 minutes); the 1 % loss short run.
8. "Decisions from the numbers": build what a rule says (each with its test, then the short soak and the session
   tools again), record the rest.
9. The optional passive check against the owner's live server (before any wire change).
10. Docs: "Stage 8 as built" with the measurements table, the Stages table row 8, "Testing" (the restated latency
    target), `notes/net.md` (the bot partition, the relay, the timeline, the soak in "Verifying"), ARCHITECTURE.md (the
    `vortex_bot` target in the layering list and 7.5), ROADMAP (M1: stages 0-8 built; what M1 lacks before acceptance:
    the owner's LAN run on stage 8's commits; test count), the game's `CLAUDE.md` (`vortex_bot`, the soak commands,
    the test count, the timeline) and `docs/two-pc-test-windows.md`.

Done when: warning-free build; every test green, and green three times under `ctest -j16`; both headless game runs
and the editor run exit 0; the CLAUDE.md drill, the UDP drill, the arena walk and `tools/session-check.sh` pass with
their usual lines; the short soak passes on both scenarios; the 10-minute soak passes on both; the measurements table
is filled; every open point O1-O6 has a decision; the docs above are updated. Not committed (the run's instruction).

#### Choices where the design is open

| # | Question | Choice | Why |
|---|---|---|---|
| 1 | Where the shared bot code lives | `vortex.net:bot` | One module per directory; `BotPilot` / `BlastDrill` are already there; GPU-free and tested with the rest. |
| 2 | Impairment over real UDP | A relay below ENet in the bot process, on its own thread | Only below ENet does a slow downlink reach the server's ENet and pacer; no root; the bot's stalls do not stall the network. `ImpairedTransport` stays for the in-process tests and `vortex_server`'s bots. |
| 3 | Latency across processes | The server's timeline file and the shared steady clock (one host) | Exact on the soak's machine, no wire change, the server's own lag included. Across machines it is not measured (the owner's run reports the HUD ping and corrections instead). |
| 4 | How bots end so hashes compare | One wall-time end for all (`--end-at`), quiet drains, then staggered leaves (`--leave-at`, 3 s apart) | The server logs each leaver's hash before its crates are despawned; the next leaver drains those despawns first, so every leave line is at its bot's drained opSeq. |
| 5 | The 100 kB/s downlink | On the late joiner's relay for its whole life | Simplest; the joiner is the peer whose bulk the pacer must fit to a slow link. |
| 6 | Outage length | 2 s, both directions, on bot B | Long enough for ENet's resend back-off to show, far under its 10 s timeout; the in-process 200 ms is covered by the unit tests. |
| 7 | The injected faults | B's broken chunk (from the server's `--broken-chunk-candidate`), D's broken generator | "No mismatch that was not injected" needs an injected one that must be found; only the server knows which chunks are pristine. |
| 8 | Where `soak.sh` lives | The game repository's `tools/` | Next to `session-check.sh`, the same `build/` paths, and it can add a game client. |
| 9 | Gate the soak on wall-clock numbers? | No: correctness only (hashes, entities, checks, verdicts); the numbers are logged and recorded | Stage 3's rule (wall clock is only logged), and the soak shares the laptop with the owner's live server. |
| 10 | The join stall fix | A permanent collider focus at the spawn, cooked at `Start` | Keeps stage 4's gate rule (the join still flushes) and costs about 100 colliders; deferring the player until its colliders cook would hold the server's player for other ticks than its client's. |
| 11 | The bots' window | The game's 48 chunks by default | A bot then joins, streams and stores like the game. |

#### Risks

- **CPU on one laptop:** the server, three or four bots (two job threads each) and the owner's live server share the
  cores; wall-clock numbers (ticks, checks, latency) include that. Mitigated by `nice` and by saying so beside every
  number; correctness does not depend on it.
- **ENet over a relay:** ENet's own throttle drops unreliable packets (snapshots) when the RTT rises at the 100 kB/s
  bottleneck; that is real behaviour and shows as corrections and sample gaps, not as a failure.
- **Ten minutes of fight on the playground:** the drill rebuilds two corner sites 6000 times; the modified map stays
  small, but entity events and crates run for long: tombstone eviction (4096) may happen - the entity check is its
  backstop and must report it as repaired, not as a failure (counted, logged).
- **Timeline file read while written:** the server flushes once a second and the bots read it at least 20 s after
  the last event; a truncated last line is ignored.
- **A protocol bump (O1)** breaks the owner's Windows build until rebuilt (said in the guide); the passive live check
  runs before any such bump.
- **Late joins at 100 kB/s on the arena** take tens of seconds for the near set (est.): the bot's start gate waits,
  measured; `--drain-seconds` covers it.

#### Amendments after the critic (2026-11-17)

A critic read the plan against the code and raised 16 findings. Where it was right the plan is amended here (this list
wins over the sections above where they differ); where a finding was rejected or only partly taken, the reason is
given.

| # | Finding | Decision |
|---|---|---|
| A1 | The join fix kept `OnHello`'s global `Terrain().Flush()`: a barrier that cooks every pending re-cook of the window (the drill's sites) in the join tick, and the gate does not need it (`StepPlayers` already flushes `PlayerReach`) | **Taken.** `OnHello` no longer calls `Update` or `Flush`; the spawn is a permanent collider focus (`Server::Impl::ColliderFocus`, cooked once in `Start`); the first step's `FlushBox` cooks only what of the reach is stale. Ticks that took a `Hello`, a peer's first `WindowUpdate` or a leave are timed apart (`ServerStats::maxHelloTickSeconds` / `maxFirstWindowTickSeconds` / `maxLeaveTickSeconds`, a `Ticks: tick N (a join ...)` line each), and `maxHelloTickCooks` counts the colliders a join tick cooked on its thread. The test runs a drill step one tick before the join. |
| A2 | The 207 ms leave stall of the owner's run was missing, and the soak's own leave lines trigger it | **Taken.** `Server::Hash` caches each chunk's hash by `World::ChunkRevision` over the fixed server window (a quiet window costs lookups; `ChunkHashesComputed` counts the misses); `vortex_server` warms it before anyone joins, hashes once when the leaver's sync box is the region window, and the "N client(s) left" line no longer hashes at all. Not taken: sharing the cache with `HashCheck` (its entries are a few dozen chunks a tick; recorded). |
| A3 | `JudgeBot` could pass broken replication: any number of pristine mismatches for the broken bot, terrain mode without a fault, no minimum of checks, no final-error gate, any box accepted | **Taken** as `net::JudgeBot`: the distinct chunks a HashCheck found different (`Client::MismatchedChunks`) must be exactly the injected chunk (whatever class: see A6), repaired from data; terrain mode iff `--broken-generator`; at least 0.5 x (connected seconds / 2) HashChecks and EntityChecks compared (0.5, not the critic's 0.8: checks of chunks still loading are deferred, and a bot that walks loads often; the margin is recorded); the final position error under 5 cm after the drain (`PredictionStats::lastError`); the store gate. The box: `soak.sh` requires the bot's box string to equal the server's `ClientSyncBox` for that slot (an empty server box cannot equal a synced bot's); an independently computed "expected" window is not added (two independent sources must already agree). One negative case per rule in `Bot.TwoSharedBotsJoinPlayAndDrain`. |
| A4 | A tombstone-eviction exemption would excuse every entity mismatch in 10 minutes | **Taken:** no exemption; any entity mismatch fails the bot. |
| A5 | `GAME_CLIENT=1` cannot drain before the end (the drill and the bots never give it a quiet second) | **Taken:** the game client stays in `tools/session-check.sh` (PORT=27042) and is not part of the soak; a wall-clock `--exit-at` for the headless game is not built (recorded for when the soak needs it). |
| A6 | B's broken chunk lies where A casts first | **Taken:** `net::BrokenChunkCandidate` now asks for a chunk whose top row is at least 3 m under the ground of its column (on the playground still (2, 1, 2)), and the verdict judges the fault by its coordinate. |
| A7 | The latency windows include the joiner's own load and cut off the join's tail | **Taken:** a join ends when its queue is empty **and** no bulk byte is in flight (`Server::JoiningClients`, the timeline's fourth field); each bot reports the events made before it played apart (its own load); O1's input is the events a bot applied after it played while a join was open. |
| A8 | Fixed wall-clock leave slots can overlap | **Taken:** each bot logs `Bot: N ready to leave` after its drain and keeps draining until `soak.sh` creates its leave file; the script releases the next bot only after the last one exited and the server logged its leave line; the bot leaves when its drain rule holds again (at most 15 s). |
| A9 | Rest states and poses are no longer compared across processes | **Taken:** `net::EntityRestHash` (resting NetIds with origin to 1 mm and rotation to 1e-4, q and -q alike) is appended to both `Session: ... entities at opSeq` lines (game, `vortex_bot`, `vortex_server`) and compared by `soak.sh` and `session-check.sh`. The hash relies on stage 6's bit-identical rest poses (a copy at rest is at the `EntityRest` pose bit for bit); quantising only makes it less sensitive - two values an ulp apart on either side of a rounding boundary (or a `q.w` near 0 whose sign differs) would still hash differently, so it is not a tolerance (review of stage 8, R13; the stage 6 contract is still tested in-process). Since the review the objects that are not resting are compared with a 5 cm tolerance by the scripts (`Session: ... awake at opSeq`, R5). |
| A10 | Nothing shows that `soak.sh` can fail | **Taken:** `SOAK_SELFTEST=1` runs the playground with a bot whose generator breaks a chunk it does not declare (`vortex_bot --test-undeclared-broken-chunk`), a bot killed before its drain, and the comparison fed a server log with one leave line removed; it passes only when all three are named. The script also fails when fewer bots were compared than started. |
| A11 | The flake hunt at -j16, not niced, makes timeouts that are not flakes and starves the owner's server | **Taken:** `nice -n 19 ctest -j8 --schedule-random`; timeouts are reported apart. |
| A12 | `soak.sh` waits for the wrong server line; knobs missing | **Taken:** it waits for `Server: waiting for clients` (logged after the candidate and the precasts); `LOSS`, `DELAY`, `JITTER`, `DOWN_RATE` and `TERRAIN` (also in `--short`) are knobs. The O2 server knob is added only if O2's rule calls for it. |
| A13 | The planned re-send counter would count legitimate re-sends | **Taken in principle; not built in this pass:** O3 stays unmeasured and is recorded so (a waste counter needs the dropped copy's hash per coordinate). |
| A14 | The soak cannot reach the snapshot budget or the awake cap | **Taken:** those rows say "not reached by the soak" with the soak's own maxima, and cite `Region250.CollapseWith64AwakePiecesOutsideOneWindow` (64 awake, largest snapshot 1186 B). |
| A15 | The passive check on the live server comes after a possible wire change and stalls the Windows player twice | **Taken, and the check is not run:** the owner's server runs `9cbfdaa`, which has both stalls; a passive bot would put a 120 ms and a 200 ms stall into the Windows player's session for little evidence the owner's own run has not given. No wire change was made in this pass (protocol stays 10). |
| A16 | "`vortex_server` output unchanged by the move" cannot be checked (loopback runs are not bit-reproducible) | **Taken:** the claim is limited to the line formats (`Bot.TwoSharedBotsJoinPlayAndDrain` checks them) and to pass / fail of the CLAUDE.md drill, the UDP drill and the arena walk. The move also changed behaviour on purpose: the broken-chunk bot plays after its repair, the verdict is `JudgeBot`'s (stricter: A3), and the `Join:` / movement lines gained fields. |

### Stage 8 as built

Status (2026-11-18): **built, verified on one machine, reviewed ("Review of stage 8", below: 20 findings, the
fixes verified) and rechecked ("Recheck of stage 8": 8 findings), not committed** (the run's instruction). Everything of "Stage 8 plan" as amended above, except where
this section says otherwise. Items 1-9 are the first pass; items 10-15 the finishing pass (2026-11-17/18), which ran
the 10-minute soaks with a terrain-mode bot and a game client on both regions and fixed what they found. Where the
review corrected a statement or a number below, the text says so ("review of stage 8", R1-R20). Protocol **10**
unchanged (no wire byte changed): a stage 8 build and a 7b build join each other. 722 tests (six disabled; 720 after
the review, two more from its recheck). A successor continues at "What is left (after the recheck)" at the end of
"Recheck of stage 8".

#### What was built

1. **The flaky test** (O8): `RunLateJoin` runs with `waitIdle` and `rngPerPeer`, measures B's join in ticks to its
   end (at most 900 more), and `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing` asserts that a second paced run
   gives the same bulk bytes, join ticks, latency, `bulkHeld` and events. Eight copies side by side under `nice -n 19`
   printed the same `[ pacing ]` line eight times.
2. **The join stall** (O7, A1): `Session.TheFirstJoinCooksNoColliderInItsTick` measured it first - the spawn's 84
   colliders (the drill region) cook in 79-107 ms on this laptop, the owner's 120-126 ms passes. The spawn is a
   permanent collider focus cooked in `Server::Start` (84-209 colliders, 58-141 ms, before anyone joins); `OnHello`
   no longer calls `Terrain().Update` / `Flush()`. The join tick now cooks 0 colliders on its thread (asserted, with a
   drill step one tick before the join).
3. **The leave stall** (A2): `Server::Hash` caches each chunk's hash by `World::ChunkRevision` over the fixed window;
   chunks an operation changed are re-hashed 32 per tick (`RefreshHashes`); `vortex_server` warms the cache before
   anyone joins (39 600 cells in 60 ms on the playground), hashes once when the leaver's box is the region window, and
   takes no region hash for a moving-window leaver (`world hash not taken (its window moves)`; the window hash is the
   comparison). `Session.ServerHashesAreCachedByChunkRevision`: a quiet window recomputes nothing, values equal
   `voxel::WorldHash`.
4. **`vortex.net:bot`**: `net::Bot`, `DrainWatch`, `JudgeBot`, the exit lines (`SessionHashLine`,
   `SessionEntitiesLine`, `ServerEntitiesLine` with `EntityRestHash`, `DescribeBotMovement / Objects / Join /
   Window`), `BrokenChunkCandidate` (3 m buried; on the playground (2, 1, 2), the 7b chunk), `PercentilesOf`,
   `ResidentMiB`, the timeline (`ParseTimeline`, `LatencyFromTimeline`); `Client::MismatchedChunks`,
   `PilotSettings::loopWaypoints`. `vortex_server`'s in-process bots run on it (the move changed behaviour on purpose:
   A16), and the game's `Session.cpp` drains by `DrainWatch` and prints the shared lines.
5. **`net::UdpRelay`** in `vortex.net:enet` (ENet's socket calls; a thread; per direction loss, outages, a drop-tail
   bottleneck of max(16 KiB, 250 ms), delay + jitter that may reorder) and `EnetStats` bytes per channel.
6. **`vortex_bot`** (`engine/tools/bot`, Net only, `vortex_assert_gpu_free(vortex_bot)`): as planned, with the
   amendments (A3, A7, A8): `--leave-file` (`Bot: N ready to leave`, then a fresh 1 s drain hold after the word),
   `--test-undeclared-broken-chunk` (the self-test's fault).
7. **`vortex_server`**: `--timeline FILE` (`ops <last opSeq> <steady ns> <clients joining>`; a join ends when its data
   is acknowledged: `Server::JoiningClients`), `--broken-chunk-candidate`, `--terrain-bulk-ceiling KIB` (O2's knob, off by
   default), the wall clock starting after `Start`; new lines `Ticks: tick N (a join / a first window / a leave) took X
   ms`, `Ticks: server:` with p50 and the join / first-window / leave maxima, `Server: cast cost ...`, `Server: events by
   kind`; `ServerStats` histograms (snapshot sizes, cast cost per 10 000 units), event bytes by kind, bulk in flight.
8. **`tools/soak.sh`** (game repository): as planned and amended (A5, A8, A10, A12); bots start in time order;
   `SERVER_EXTRA` passes server flags. `tools/session-check.sh` compares the rest hash too, defaults to PORT 27042 and
   refuses 27015.
9. **A bug the soak found: a chunk-request storm on the arena** (not in the plan). Under the drill's 10 casts a second
   (about 110 terrain operations a second with its `FillBox` rebuilds) the client's op ring (1024 operations) wrapped in
   about 9 s, and `ChunkSync::CatchUp` asked for **every** chunk still loading whose base the ring no longer covered, whether
   or not a dropped operation had touched it: the first arena short soak logged 21 028 chunk requests and 19 MiB of
   bulk in a minute, a bot's store filled (12 288 slots, `OVER THE GATE`) and the 100 kB/s joiner never synced. Fix:
   `OpRing::CoversChunk` keeps, per chunk, the newest dropped operation whose chunk range held it (ranges over 4096 chunks
   count as "unknown": every older base uncovered); a loading chunk is requested only when a dropped operation after its
   base touched it, else it replays from the ring. Test `ChunkSync.ADroppedOperationUncoversOnlyTheChunksItTouched`.
   After it: 0 chunk requests in every soak run. No wire change.
10. **The bulk pacer under loss (O2 / O5; the finishing pass, 2026-11-17).** The first pass put the slow terrain-mode
   join down to 30 ms of jitter. An experiment over real ENet (playground, one standing terrain-mode `vortex_bot`, 40 ms
   each way, 75 s, four servers side by side) says otherwise - bulk received:

   | Link (each way) | Paced (7a pacer) | Fixed 32 KiB window | Fixed 128 KiB | Paced (stage 8 pacer) |
   |---|---|---|---|---|
   | no loss, no jitter | 223 kB/s (16.3 MiB in 55.7 s) | | | |
   | no loss, 0-30 ms jitter | 218 kB/s | | | |
   | 5 % loss, no jitter | 138 kB/s | 218 kB/s | | |
   | 5 % loss, 0-30 ms jitter | **73 kB/s** | 161 kB/s | 222 kB/s (join done in 41 s) | **125 kB/s** |
   | 5 % loss, 0-30 ms jitter, 100 kB/s down | | | | 25 kB/s of bulk (31 kB/s on the wire) |

   Jitter alone costs nothing; **loss** does, through the pacer: a lost datagram keeps its bytes in flight for ENet's
   resend timeout (RTT + 4 x variance, so jitter makes it longer), the drain rate sampled per tick falls, and the
   window (rate x (base RTT + target)) follows it down. Reproduced first in `BulkPacer.LossDoesNotCollapseTheWindow`
   (a model link, 10 % of datagrams held 200 ms, 80 + 0-60 ms: paced 95 KiB/s against 237 through a fixed window; it
   failed). Fix (`BulkPacerSettings::deliveryInterval` 0.5 s, `deliveryWindows` 8): the window's rate is the highest
   delivery rate of the last eight half-second intervals (or the smoothed drain rate when higher; an interval that
   ended with nothing in flight is not kept): a delivery rate over half a second hardly exceeds the link's, so its
   maximum is about the link's capacity, and the RTT rule still bounds the queue. The model now gives 158 KiB/s
   (67 % of the fixed window; asserted 60 %) and behind 100 kB/s 70 kB/s with a queue of 91 ms; real ENet 125 kB/s
   (1.7x). A quarter-second interval gave 188 KiB/s in the model but raised
   `Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing`'s worst live latency from 22 to 25 ticks (over its bound
   of 24: a client that stalls and then drains its inbox makes one short interval look fast); at half a second it
   stays at 22. Left open: a slow link with loss and jitter is still under-used (the last row: the pacer's RTT signal
   is ENet's RTT minimum over its 5 s throttle epochs, so a delay-based additive increase would fill the bottleneck's
   queue before it saw it; M6, Wi-Fi). `vortex_server --bulk-window-floor KIB` (a measurement knob: a fixed window
   when it equals the ceiling) took the table's fixed-window rows. No wire change.
11. **Re-sent chunks counted (O3; the finishing pass).** `ClientStats::chunkDataRepeats` / `chunkDataRepeatBytes`
   (a `ChunkData` for a coordinate this client had received before, for any reason) and `chunkDataUnchanged` /
   `chunkDataUnchangedBytes` (its RLE bytes equal the last ones received for it: sent again unchanged, the waste a
   "have" list would save; FNV-1a of the RLE per coordinate, about 1 ns a byte); `Client N window:` ends with them.
   Amendment A13's objection (legitimate re-sends) is met by the split: a changed chunk is a repeat, not unchanged.
12. **The soak, finished:** on the arena `TERRAIN=1` (the 10-minute default) adds bot 3 with a broken generator,
   joining at 5 s and standing (server terrain mode over the whole 48-chunk window, about 40 MiB, under 5 % loss; it
   may drain for 400 s); `GAME_CLIENT=1` starts a headless game client (`Shredborne --connect ... --headless
   --validation --frames GAME_FRAMES`, 400 by default) once every bot is ready to leave - it joins the world the fight
   left, renders, drains, logs the bots' `Session:` lines and leaves, and is compared like a bot (its screenshot is
   `game.png`). This answers A5 without a wall-clock `--exit-at`: the game client comes when the end is already
   quiet. `compare` takes `name=log` pairs.
13. **A second ring-wrap bug the soak found: terrain-mode installs all shown** (the finishing pass's 10-minute arena run
   with a terrain-mode bot). The bot's store filled (12 288 of 12 288 slots after 5 minutes, then `chunk store is full,
   model is not baked` for every crate and piece copy); on the playground the same bot had shown 7888 of its 7942
   `ChunkData` where the server shows about 2500 (it passed there: the store held them). Cause: a chunk the
   `TerrainAck` called hidden is installed packed only if no operation came near it since the ack
   (`OpRing::TouchesNear`), and `TouchesNear` answered "touched" whenever the ring no longer covered the ack - which
   under the drill (about 110 operations a second) is every install after the first 9 s of a join that takes minutes.
   Fix: as item 9, the dropped operations are known by the chunks their ranges held (`m_DroppedTouch`, an operation over
   4096 chunks makes everything older unknown); only a chunk with one of them in its 3 x 3 x 3 neighbourhood after the
   ack is shown. Test `ChunkSync.TouchesNearAfterTheRingWrapsLooksAtWhatWasDropped` (failed before). No wire change.
14. **The drain beside a body that never rests** (the same arena run). Every bot logged `the drain did not settle in
   60 s` and the headless game client exited 1 (`replication not shown: the drain did not settle in time`): one server
   body stayed awake to the end (the server's and every client's list: 30 objects, 29 resting), so no `EntityRest`
   came, though it did not move (the bots still compared equal: they leave 15 s after the word whether or not the rule
   held). `net::DrainWatch` now takes a copy that is not resting as settled when it stays within 1 mm of where it was
   when the hold began (a moving copy restarts the hold); the game's headless exit and `vortex_bot` share it. Test
   `Region250.ADrainSettlesBesideAStillAwakeServerBody` (a crate the server keeps awake on flat ground: the old rule
   never settled; and a thrown crate keeps the hold from settling). `vortex_server` logs the replicated bodies that are
   not resting after each leave (`Session: N replicated object(s) not resting: id at (x, y, z) frozen / active`) and
   `vortex_bot` its copies that are not resting at exit. Run 2 named them: two pieces at y = -46.5 m, at the bottom row
   of the arena's window (a deep hole), **frozen** - the chunk under them is outside the window, so no collider can
   ever be built there and they stay "awake" for good (O11). The game client then drained in 1.0 s beside them; the
   three playing bots still did not settle within their 60 s (they compared equal after the leave word) and the
   terrain-mode bot took 331 s, for a reason not identified: `vortex_bot` now logs every 10 s of a drain that does not
   settle what holds it (`Bot N: draining: applied opSeq A of B, synced / NOT SYNCED, N copies not resting (M moved or
   new), held for S s`, and the not-synced counts); run 3 (no frozen body) drained every bot in 1.6-3.1 s.
15. **A black frame for a player who joins where another stands** (the game, `src/Session.cpp`; both 10-minute arena
   runs' `game.png` were black but for the HUD). Every player joins at the template's spawn and players do not collide
   (M2): the game client's first-person camera was inside the figure of the terrain-mode bot standing there, and a
   camera inside voxels sees nothing. A remote figure that holds the camera (0.6 m across, 2 m up) is not drawn now
   until one of the two moves. Reproduced with a standing `vortex_bot` and a headless game client on the playground
   (black before, the plaza after); no GPU-free test (it is the game's drawing). Spawn points per slot would be the
   fuller answer (M2, with player collision).

#### Runs (this laptop, Intel UHD 620 / 8 threads, the owner's live server and its Windows player beside it)

Every soak process ran under `nice -n 10`, with 3-4 bots (two job threads each), the server and, at the end, one
headless game client on the same cores, so the wall-clock numbers below include that contention (stage 3's rule:
logged, never gated). Every bot: 5 % loss each way, 40 + 0-30 ms each way through its relay; the drill: 10 casts a
second on two corner sites, ending 20 s before the bots stop; bot 1 has a 2 s outage at 30 % of the run; bot 2 joins at
50 % behind a 100 kB/s downlink; bot 3 has a broken generator (server terrain mode: on the playground it joins at 20 %
and plays, on the arena at 5 s and stands); on the playground bot 1 also has the injected broken chunk; with
`GAME_CLIENT=1` a headless game client joins once every bot is ready to leave.

| Run | Result |
|---|---|
| First pass (before the finishing pass): short soaks, both 10-minute soaks (arena without a terrain-mode bot), the O2 run, the self-test, a 1 % short run | All PASS in the end (the arena's chunk-request storm, item 9, and a `vortex_bot` drain bug were fixed on the way); their numbers are superseded by the rows below except where a row says "first pass" |
| Pacer experiment (item 10; four servers and one terrain-mode bot each, 75 s) | the table in item 10 |
| Short soak with `GAME_CLIENT=1`, both regions (pacer fix built) | PASS: three bots and the game client equal the server's on each region |
| 10-minute soak, run 1 (pacer fix and O3 counters built) | playground **PASS** (four bots and the game client); arena **FAIL**: the terrain-mode bot's store filled (item 13), and every drain waited for a body that never rested (item 14; the bots still compared equal 15 s after their leave word, the game client exited 1) |
| 10-minute soak, run 2 (items 13 and 14 fixed): the measurement run | **PASS on both** (on the arena the bots' drains waited the whole 60 s: item 14): playground 94 761-94 797 operations, arena 91 202-91 223; every bot's and the game client's hash over its sync box and entity line (count, resting, id hash, rest hash) equal the server's at the same opSeq; the terrain-mode bots in server terrain mode, the broken chunk (2, 1, 2) found by the first check near it and repaired once, no other mismatch |
| 10-minute arena soak, run 3 (drain diagnostics) | **PASS**: four bots (the terrain-mode bot synced) and the game client equal the server's at 90 935-90 953 operations; every drain settled in 1.6-3.1 s; no body left awake at the end. The walker's event p99 was 2.4 s (run 2: 615 ms; the A/B below) |
| Short soak at 1 % loss (playground, 60 s, final build) | PASS; latency in O4 |
| Correction experiment (two bots with the same seed beside the drill, one presenting; 90 s) | 217.7 against 22.7 corrections a minute: the presenting bot's process stalls (below) |
| A game client joining where a player stands (a standing `vortex_bot` and a headless game client on the playground) | a black frame before item 15, the plaza after |
| `SOAK_SELFTEST=1 tools/soak.sh --short` (final pass) and the suite three times under its load | PASS: the three faults named; no test failed or timed out |
| Pacer A/B (`vortex_server --pacer-delivery-interval 0` = the 7a pacer against item 10's, two arena servers side by side, a walker and a wanderer each, the drill, 3 minutes) | event p99 over all events: the walker 843 ms (7a pacer) / 664 ms (stage 8 pacer), the wanderer 715 / 661 ms; their bulk 32.1 / 37.6 kB/s and 21.0 / 22.3 kB/s. No sign in 3 minutes that the faster pacer holds live events back. Run 3's 2.4 s walker p99 is **not explained** (review of stage 8, R10: the first text called it a stall of that process, but its log shows no resync and a margin of 0..7, which a stall of seconds would not leave); a 10-minute A/B of the walker is open ("Review of stage 8") |

#### Stage 8 measurements

**Restated after the review of stage 8** from run 4, the review's 10-minute rerun of both regions (2026-11-18: R15's
clock fix in `vortex_bot`, R8's rates per phase, R9's quiet latency; "pg" playground, "ar" arena; the same scenarios
as runs 1-3, a terrain-mode bot and a game client on each). The first text took its rates over each bot's whole life
(the drain and the wait for the leave word included: about 35 % low on the arena of run 2) and ran with the bot clock
bug (R15); the rows below replace it, and give run 2's figure only where it still says something. Each row names the
estimate it replaces.

| Measurement | Measured (run 4) | Replaces (estimate) |
|---|---|---|
| Cast-queue delay | 6837 (pg) and 6434 (ar) casts, every one run in the tick it was queued: p50 / p99 / max **0 / 0 / 0 ticks**; no tick waited for work or bytes (run 2 the same) | "Time and ticks": the work budget's "about 6 ms" per tick never bound at 10 casts/s |
| Cast cost | per 10 000 work units p50 < 1.1 ms, p99 < 8.1 ms, max 22.8 ms (pg); p50 < 1.4, p99 < 7.8, max 14.2 (ar); overall 1.64 / 1.72 ms per 10 000; the longest cast 38.9 / 24.1 ms | "a unit is about 0.1 us" (= 1 ms per 10 000); stage 3's 0.40-1.66 ms per 10 000 on a quiet CPU - under load the median is about 1.1-1.4 ms, the tail 5-7x |
| Event latency (made to applied, wall clock, one host; events made after the bot played; "quiet" = made while no client joined, R9) | **Quiet p99 (restated by the recheck of stage 8, RC1)**: on the plain 5 % links of the bots that stand or wander **0.58-0.79 s** (pg 655 ms bot 1, 713 bot 0, 725 the terrain-mode bot once it played; ar 580 ms the wanderer, 793 the standing terrain-mode bot), p50 94-113 ms; behind 100 kB/s p50 155-169 ms, quiet p99 864 (pg) / 904 ms (ar). **The arena walker (plain link): quiet p99 1494 ms, max 2190 ms** - 2.6x the wanderer's 580 ms over the same 11 504 events (the arena's quiet window is the run's last 97 s: the terrain-mode join covered the rest), while its p99 during joins is 590 ms: not explained, R10 open. All events p99 (every bot): pg 722 (bot 0), 868 (terrain mode), 1045 (100 kB/s), 1365 ms (bot 1: its outage); ar 629 (walker), 1046 (100 kB/s), 1241 (bot 1: outage), 1506 ms (terrain mode: its own 507 s join). The O5 A/B's 5-minute runs: plain bot 0 quiet p99 550-666 ms, the 100 kB/s joiner 845-1289 ms, bot 1 1389-2568 ms (its outage fell in the quiet window there): the range moves from run to run | "Testing": "p99 under 250 ms during the join"; O4 |
| Event latency during joins | pg bot 0 (plain link, joining nothing itself): **731 ms against its quiet 713 (+1.1 ticks)**; bot 1 2141 ms (its 2 s outage fell inside the terrain-mode join), the 100 kB/s joiner 1287 ms and the terrain-mode bot 1480 ms (both inside their own joins); ar: the walker 590 ms (against its quiet 1494: the other way), the wanderer 1397 ms (outage), the 100 kB/s joiner 1082 ms, the terrain-mode bot 1759 ms (own join). Events made before a bot played (its own load) counted apart: max 0.9-3.7 s | 7a's "p99 under 15 ticks while a client joins": over real ENet at 5 % loss the floor is the loss. "A join adds about a tick to a plain-link bot that is not itself joining" rests on **one bot on one region** (pg bot 0; recheck, RC1): no other bot of run 4 is a plain link outside its own join and outage in both windows, and the arena's quiet window (97 s) is too short to say |
| Bytes per second per client, **play phase** (from its join data in to the end of play) | pg: up 1.85 kB/s payload (2.44 on the wire), down **30.2-31.7 kB/s** payload (34.6-36.1 on the wire; peak 92-120 in 1 s): state 7.0-8.1, events **22.7-24.5**, bulk 0; ar: up 1.9, down 37.4-75.4 kB/s: state 6.3-6.8, events **23.4-24.3** (the terrain-mode bot 19.1: its 97 s of play end with the 20 s after the drill), bulk 7.2 (the late joiner) / 22.0 (the wanderer) / 44.8 (the walker); join phase: a plain join 26-32 kB/s for 4-5 s (pg) and 91-94 kB/s for 12-13 s (ar), the 100 kB/s joiner 53 (pg) / 56 (ar) kB/s, a terrain-mode join 128-132 kB/s for 192 s (pg) / 507 s (ar) | "Decision in brief": events est. 24 kB/s (measured 22.7-24.5 at 10 casts/s, the same on both regions: run 2's "11-15 on the arena where the window holds fewer of the casts' chunks" was the dilution of R8, withdrawn); snapshots est. 1200 B at 30 Hz = 36 kB/s (measured 6-8 kB/s: few bodies awake); inputs est. 2.5 kB/s (1.85); per player est. down 7 kB/s quiet / 24 with 20 awake / cap 37 + events 24, up 2.5 - measured with 4 players and a drill, not a 5v5 |
| Event bytes by kind | CarveSphere 17 B, FillBox 26 B, Detach 1815 B on average (5304 B max), ClearMask 29 B (467 B max), EntitySpawn 168 B (2093 B max), EntityDespawn 6 B, EntityRest 37 B, ObjectModel 4.0 KB (pg) on average (9.9 KB max), EntityWake 9 B; no event over the 16 KiB burst (pg; run 2 alike on both) | "World operations": Detach "under 200 typical, under 2 KiB at the cap" - the drill's pieces average 1.8 KB (their masks); ClearMask 2.4 B per crumb voxel; ObjectModel "under 100 for a crate" - a damaged crate's model is 4-10 KB |
| Snapshot sizes | p50 < 320 B, p99 < 448 B, max 567 B with 15 bodies (pg); p50 < 256, p99 < 448, max 505 B with 13 (ar); 0 bodies left out | **Not reached by the soak** (A14): the 1200 B budget and "64 awake" stand on `Region250.CollapseWith64AwakePiecesOutsideOneWindow` (64 awake, largest snapshot 1186 B, stage 7b) |
| Bulk inside the transport | at most 32.0 / 31.9 KiB of bulk in flight to one peer; window 1.2-32 KiB; largest bulk message 12.9 / 13.4 KiB, 0 over 16 KiB | "Transport": "at most est. 32 kB of bulk inside the transport" (holds); 7a's pacer constants (item 10 changed how its rate is taken, not the constants) |
| Bulk per join | pg at the start: 75-80 KiB (50-51 ChunkData + 12 entities), join data in after 3.8-4.8 s; pg after 5 min of the fight behind 100 kB/s: 1.53 MiB of chunks + 98 KiB of baseline, join data in after 80 s; pg terrain mode under 5 % loss: 16.3 MiB of chunks + 2.1 MiB of baseline and model answers in 191 s; ar at the start: 0.70-0.74 MiB + 127-138 KiB, 11.5-12.2 s; ar after 5 min behind 100 kB/s: 1.27 MiB + 102 KiB, join data in after 57 s, synced after 84 s; ar terrain mode (standing): 40.0 MiB of chunks + 6.9 MiB of baseline and model answers in 507 s (97 kB/s); the game client after the fight (no loss): 2.41 MiB + 121 KiB in 2.1 s (pg), 2.42 MiB + 133 KiB in 5.3 s (ar), 1.3 MiB/s at its peak | "Chunk sync": "0.5-8 MB per join after a 5-minute 5v5" (1.3-2.4 MiB after 5-10 minutes of a drill at 10 casts/s); terrain mode est. 8-25 MB (16.3 MiB on the playground, 40 MiB on the arena: over the estimate) |
| Re-sent chunks (O3, item 11) | ar: the walker (crossing the region and back for 10 minutes) received 4775 `ChunkData` for chunks it had received before, 14.8 MiB of its 26.3 MiB of bulk; **4189 of them unchanged, 12.9 MiB = 22.1 KiB/s over its 597 s of play** (run 2's "14.6 KiB/s" was over its whole life, R8); the wanderer 574 unchanged (1.9 MiB, 3.3 KiB/s), the late joiner 244 (0.8 MiB, 3.2 KiB/s); fixed windows (pg) 0 | 7a's re-send cost (7a measured 3.1 MiB for a wandering bot in 35 s) |
| Server tick (`Server::Tick`: receive, players, **the casts**, physics, events, snapshots, the hash refresh; **not** the drill's rebuilds - `net::RunDrillStep`'s `FillBox` and crates between ticks - nor the transport flush, R12) | pg: p50 < 1.0 ms, p99 < 14.0 ms, max 55.2 ms, 235 of 37 937 ticks over 16.7 ms; ar: p50 < 1.25, p99 < 13.0, max 45.0, 169 of 38 292 over; **join ticks at most 8.1 ms (pg) and 1.8 ms (ar), 0 colliders cooked in any**; first-window ticks at most 8.0 / 10.0 ms (the terrain-mode join's whole-window `TerrainAck`); leave ticks at most 18.2 / 9.9 ms (run 2: 4.8 / 7.8; this laptop also ran a headless game client then); the spawn's 203 / 209 colliders built at `Start` in 126 / 165 ms. **Passes** (the ticks plus the drill's rebuilds, new line): p50 0.96 / 1.15 ms, p99 14.1 / 13.1 ms, max 55.2 / 67.3 ms | O7 and the owner's 126 / 207 ms; stage 9's "p99 under 16.6 ms" (recorded only: both p99 under 14 ms with four bots, the drill and a game client on one laptop; the tick's tail is the drill's casts, which run inside it) |
| Server memory (its own process) | pg 210-213 MiB; ar 583 MiB at the end; arena preload 10.2 s under the soak's load, 48.5 MiB RLE, store 10 379 of 58 140 slots | "Server world without a camera": 250 m est. 1.1-1.3 GiB (583 MiB measured, the server alone; stage 3: 463-518); the 150 MiB RLE gate holds; the 10 s preload gate is at its edge on a loaded laptop |
| Client store and memory | fixed window (pg) at most 2517-2601 slots, the terrain-mode bot 3149; moving 48-chunk window (ar) at most 5693-5813 of 12 288 (gate 10 240), the standing terrain-mode bot 7498; bot RSS 160-178 MiB (pg), 319-354 MiB (ar) | "Chunk sync": est. 5300-5700 dense in a 48-chunk window (at most 5813 used); the 2048 headroom (never touched) |
| Corrections per minute **of play** (the `Play:` line, R8; before R15 the bot's clock ran slow) | pg: **34.7 (bot 0, presenting; 4 clock jumps in 604 s)**, 28.2 (broken chunk and outage), 37.6 (100 kB/s), 17.1 (terrain mode); ar: 2.1 (the walker), 22.8, 33.1, 0 (standing); 0 hard resyncs, clock jumps 0 except bot 0's 4; snaps 0-8; the final position error after every drain 0.000 m; replay at most 12-27 ticks, 0.2-34.9 ms. The game clients 0 (joined after the fight: no casts, R14). Run 2 had 277.9 for bot 0 with 2388 clock jumps - the clock bug (R15), not presentation | "Player movement": error under 2 cm ignored; "replay under 1.5 ms" (est.; measured up to 35 ms on a loaded laptop); the M1 fight's "within 5 cm" after the drain (0 cm) |
| Repairs and checks | the injected chunk found by its first check near it and repaired once; 0 other mismatches in the 1052 (pg) and 1113 (ar) HashChecks and 1016 / 1103 EntityChecks the server sent; tombstones evicted about 7700-7800 per bot that played the whole run, with 0 entity mismatches; client checks at most 2.0-5.6 ms (bots on the loaded laptop); the game clients 1.8 / 3.9 ms, but they joined after the fight (R14: `tools/session-check.sh`'s 1.8-2.4 ms under a drill is the game's figure under play); the server at most 8.4 / 8.3 ms per peer and tick, 1.4 s in all over 10 minutes | "Chunk sync": repair est. 4 pristine chunks; the 7b plan's "under 1 ms per check" (O6) |
| Timeline | 13 666 lines / 359 KiB for 10 minutes (pg, run 2) | "about 36 000 lines (1 MiB)" (est.) |
| Game client after the fight (`GAME_CLIENT=1`, headless UHD 620) | gbuffer 8.8 ms, lighting 8.0 ms in the ruined plaza (pg); 7.9 / 11.5 ms on the arena; HUD 177 quads (run 2) | (no estimate: the frame budget of ROADMAP's performance table) |

Not measured by any stage 8 run (as the plan said): the stage days, fixed-point noise, `replicatedPieceVoxels` (an
owner decision), stage 9's memory figure.

**Bot 0's correction rate** (pg: 266-310 a minute in runs 1-3; every other bot 12-46). The first text found
"presentation": two `vortex_bot`s with the same seed beside the drill, one with `--present`, gave 217.7 against 22.7
corrections a minute, the presenting one's clock swinging -140..148 ticks with 3 hard resyncs. **Corrected by the
review (R15):** `vortex_bot` measured each pass's seconds from the end of the previous pass, so the time spent inside
`Pass` (ticks, installs, presentation) never reached the client's clock; it ran slow and jumped forward 1-4 times a
second (bot 0 of run 2: 2388 jumps), and the bot that worked most inside its passes - the presenting one - lost the
most time, sent its inputs late and was corrected most. With `Bot::PassOnClock` bot 0 logs 34.7 corrections a minute of
play and 4 clock jumps in 10 minutes (run 4), in line with the others. Presentation still costs a client CPU beside a
10 casts/s demolition (the fx budget, `fx-budget.md`), but the soak shows no movement problem from it.

#### Decisions from the numbers

| # | Decision |
|---|---|
| O1 | **Not built.** No bulk message over 16 KiB in any run (largest 13.4 KiB; the largest event a 10.4 KB `ObjectModel`). |
| O2 | **Built differently** (item 10). The ceiling was not the limit: with a fixed 128 KiB window the terrain-mode join ran 1.4x faster than with a fixed 32 KiB one, and the paced join (73 kB/s) far below both - the pacer's window collapsed under loss. Its rate is now the highest recent delivery rate: 125 kB/s in the experiment; the soak's playground terrain-mode join 188-200 s (first pass 320 s; run 4: 191 s), the arena's 40 MiB in 485 s at 102 kB/s (run 4: 507 s at 97 kB/s). `--terrain-bulk-ceiling` stays (off). |
| O3 | **Measured; the rule's thresholds are met, not built.** A walker crossing the arena and back gets about half its bulk as unchanged re-sends (run 4: 12.9 of 26.3 MiB, **22.1 KiB/s over its play**; run 2 said 14.6 KiB/s, averaged over its whole life: R8; rule: over 30 % and over 8 KiB/s); a wanderer or a late joiner about 3 KiB/s, a standing player none. The remedy - a "have" list with the client keeping the slab it left (or its hash) - is a wire change and a client cache, not small: recorded for the memory milestone together with the model answers (29.8 MiB of `BaselineEntity` on the arena in 10 minutes, the same sweeps). |
| O4 | **Target restated** ("Testing"; corrected by the review of stage 8, R9: the first text compared "during joins" with "all events", which contain the joins, and its targets did not follow from its numbers). Quiet against during joins (`EventLatency::quiet`, new). At 5 % loss each way and 40 + 0-30 ms each way (run 4; restated by the recheck, RC1): p50 0.09-0.11 s; **quiet p99 0.58-0.79 s for the plain-link bots that stand or wander**, **1.49 s (max 2.2 s) for the arena walker** (the client with the most bulk, 44.8 kB/s; R10, open), 0.86-0.90 s behind a 100 kB/s link in run 4 and 0.85-1.29 s in the O5 A/B (runs 1-3 on all events 0.62-1.0 s, run 3's walker 2.4 s); up to 2-4 s across a 2 s outage. On the playground's bot 0 (a plain link not itself joining) the joins added about a tick (731 against 713 ms: within the 3-tick rule) - **one bot on one region**, not a general result; a bot's own join or an outage inside a join adds seconds. This is ENet's resend floor (retransmit timeout RTT + 4 x its variance, doubled per retry; not tunable without patching ENet - not done). At **1 % loss** (60 s playground runs, the same delays and drill): run 4 quiet p99 256-281 ms, during the late join 274-312 ms for the plain-link bots (16-19 ticks), 466 ms for the 100 kB/s joiner during its own join; the first pass gave 288-339 ms during joins. **The target "p99 under 250 ms during joins at 1 % loss" is not met over real ENet on this path** (10-35 % over: one resend, about 0.2 s with 0-60 ms of jitter, on an 80-140 ms round trip); it is met in-process (`Region250.LateJoinDuring10CastsPerSecondConverges` and the 7a unit test) and **was not measured on a LAN** (the owner's run reports the HUD ping and corrections, not event latency). Kept as the in-process gate; over ENet recorded, not gated. |
| O5 | **Not built; the rule amended (review of stage 8, R11).** The ratio was miscomputed: run 2's joins took 4.3-4.6x their link time (1.26 MB in 58 s, 1.75 MB in 75 s; the first text said 4.7-5.0x), run 4's 4.7x (pg: 1.70 MB in 80 s) and 3.7x (ar: 1.53 MB in 57 s) - over the rule's 4x on the playground. The rule's remedy, a minimum bulk share, acts only while live events alone fill the window; an A/B says the window is not what holds the joiner back: two 5-minute playground soaks side by side (the joiner at 150 s behind 100 kB/s), the paced window against one fixed at the 32 KiB ceiling (`--bulk-window-floor 32`): bulk during the join 26.5 against 25.7 kB/s, join data in after 37.3 s (0.98 MB, 3.7x) against 43.5 s (1.14 MB, 3.8x) - and the fixed window overfilled the bottleneck's 250 ms queue: **the joiner's own event p99 during its join 0.55 s paced, 10.5 s fixed**. So more bulk in flight does not make the join faster on that link (loss at the bottleneck and ENet's own resend back-off and window do), and a minimum share would only add to the queue. The rule is amended: build `minShare` only if a measurement shows bulk held while none of it is in flight and the live events alone fill the window (`ServerStats::bulkHeld` split by that, not built). Left for M6 (Wi-Fi): what limits a lossy 100 kB/s link (ENet's reliable window and back-off, measured per peer). |
| O6 | **Not built.** The evidence is `tools/session-check.sh`'s game clients under a 30-step drill: 1.7-3.3 ms at most per check in stage 8's runs, 1.8-2.4 ms in the review's run - under the 4 ms rule. (The soak's game clients, 1.8-3.9 ms in run 4 and 1.0-1.2 ms in run 2, joined after the fight with no casts and are not evidence for play, R14; bots on the loaded laptop 2.0-5.6 ms.) The owner's run adds the windowed numbers. |
| O7 | Done: item 2; join ticks at most 2.3 / 5.3 ms, 0 colliders; the largest join-related tick is a terrain-mode join's whole-window ack, 8.3 ms on the arena (within the tick; such a join is rare: only a client whose terrain differs). |
| O8 | Done: item 1, and the flake hunt (below). |
| O9 | Done: the cast-cost row. |
| O10 | The owner's run: `docs/two-pc-test-windows.md` 6.5 lists what to collect. |
| O11 | Open as before. Also open: an awake replicated body frozen where no collider can be (below the window's bottom row on the arena) stays awake for good and counts against the 64-awake cap (item 14; 2 in a 10-minute arena run). |

#### What is left

Verified on the final build of the finishing pass (2026-11-18, before the review; the review's own verification is in "The review's rerun"): the build is warning-free; all 719 tests (six disabled) pass, and pass three times in a row under `nice -n 19 ctest -j8 --schedule-random --repeat until-fail:3` while the soak's self-test loaded the machine (no flake, no timeout; the plan's pass under load, which the first pass skipped);
the standard headless game run (`Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, `on the ground`),
the editor run, `--climb` (0 unloads inside the view radius), `--tower-blast --after-fill --frames 700` (freed by shot
6 of 7, the load check) and `--neck-blast --frames 200` (0 pieces released by the duds, the tower released at 5.61x)
exit 0 under validation; the CLAUDE.md drill, the UDP drill (`--enet`), the arena walk and `tools/session-check.sh`
(PORT 27042; screenshots looked at: the plaza, craters, crates, HUD ONLINE) pass; the short soak with a game client
passes on both regions, and `SOAK_SELFTEST=1` names its three faults; both 10-minute soaks pass (run 2, and the arena
again in run 3), with a terrain-mode bot and a game client on each. After run 3 only three things changed, none in
what the soak compares: the game's figure rule (item 15), the `Client N walk:` line's text for a looping walker, and
the `--pacer-delivery-interval` knob (off unless given); the game runs, `tools/session-check.sh` and the short soak
with a game client were run again on that final build.

Not done, and for whom:
- **M1's acceptance: the owner's LAN run on stage 8's commits** with the real game clients, windowed (Linux host,
  Windows laptop): `docs/two-pc-test-windows.md` 6.5, with the short soak as its pre-check. An assistant ran only
  headless game clients and bots on one machine.
- **The passive check against the owner's live server: deliberately not run** (A15: that server runs `9cbfdaa`, with
  both stalls, and a Windows player is on it).
- The commits (engine first, then the game), before or with the owner's run. The review of stage 8 with fresh eyes
  (CLAUDE.md "How to work on a feature", step 4) is done: "Review of stage 8" below, with its own list of what is left.
- Open points for later milestones: the pacer on a slow, lossy, jittery link (O5, M6); re-sent chunks and model
  answers (O3, the memory milestone); bodies frozen awake for good (O11); sharing the hash cache with `HashCheck`
  (A2); a wall-clock `--exit-at` for a game client that plays in the soak (A5: it now joins after the fight instead).

### Review of stage 8 (2026-11-18)

Three reviewers read the uncommitted stage 8 (`vortex_bot`, the soak, the measurements) against the code and the logs
and raised 20 findings. Each was checked against the code; the bugs among them were reproduced first in a GPU-free
test that failed on the old code. **19 taken (fixed or the text corrected), 1 taken in part**; no finding rejected
outright. Protocol **10** unchanged (no wire byte changed). 720 tests (six disabled; one new, three extended).
"Stage 8 as built" and "Stage 8 measurements" above are corrected where the findings showed them wrong; this list
says what changed and why.

| # | Finding (severity) | Verdict | Fix |
|---|---|---|---|
| R1 | The soak never judged the game client's own checks or terrain mode, and the game exited 0 on a check mismatch (high) | **Real.** `Session.cpp` failed a headless run only on the drain and an incomplete hash; `soak.sh` read only its `Session:` lines | `net::ClientCheckFailures(client, terrainModeExpected)` (shared with `JudgeBot`): a chunk a HashCheck found different, an entity mismatch, a check out of order, server terrain mode unless `--other-terrain`; a headless `--connect` run fails on any of them (`Session: replication not shown: ...`, exit 1). `soak.sh` also applies `session-check.sh`'s gate to the game client's `Checks: slot` line (0 hash / entity mismatches, 0 out of order, HashChecks compared), requires its `Terrain: slot` line not to say server terrain mode, and requires a fresh `game.png` (R20) |
| R2 | With no broken-chunk candidate the playground soak dropped the repair fault and could still pass (high) | **Real** | No candidate on the playground is a scenario failure (and in the self-test); the server must request exactly the injected repairs (`Checks: server: ... N repairs requested`: 1 on the playground, 0 on the arena, 2 in the self-test); bot 1's `JudgeBot` already fails when its chunk was not found and repaired |
| R3 | Nothing gated that the fight happened (drill steps, freed terrain, casts, crates, the walk, joins and leaves) (medium) | **Real**, and it hid a bug: the drill report itself was wrong on long runs - the cast record kept `drillSteps` records, players' casts and the precasts share it, so a 10-minute run dropped the oldest drill casts and the report counted the wrong steps (playground 4861 of 5800 steps counted, arena "0/1736 freed": every step misattributed) | `vortex_server` keeps `drillSteps + precasts + 65 536` records with a drill (about 100 B each) and warns if the record is full. `BotGates` gained `minActedShare` (the pilot drove at least 80 % of the ticks it was asked to act on once it had predicted: `Bot::WantedTicks` / `ActedTicks`), `minPilotShare` (casts and crates at least half of acted ticks / N), `maxRejectedCastShare` (at most 20 % + 2 of its casts not run), `minWalkShare` (a walker covers 80 % of the waypoints' x span, reaches at least 2 waypoints, gives up fewer than it reaches, its window moved); `vortex_bot` sets them. `soak.sh` requires the `Drill: freed terrain` line to total the asked steps with every kind freeing terrain, no `cast outside the play bounds`, and `Server: N clients joined, N left, ..., 0 peers timed out` with N the clients it started. A negative case in `Bot.TwoSharedBotsJoinPlayAndDrain` |
| R4 | A broken-chunk bot whose chunk differed again after its repair still passed (medium) | **Real** (reproduced: the test below failed on the old `JudgeBot` - after a second repair it would have passed) | `JudgeBot` fails a broken-chunk bot with any repeat, refused or stuck mismatch, or more mismatches than first-time repairs (on a fixed window: more than one mismatch or repair) - `its broken chunk ... differed again after its repair`. `Bot.TwoSharedBotsJoinPlayAndDrain` now changes the repaired chunk on the bot (as a wrongly applied operation would), steps until a HashCheck finds it again, and expects that verdict |
| R5 | Positions of copies that are not resting were never compared across processes (medium) | **Real** (the drain accepts a still, awake copy since item 14) | `net::AwakeEntitiesText` / `SessionAwakeLine` / `ServerAwakeLine`: `Session: [slot S] awake at opSeq A: N awake: id (x, y, z); ...` (sorted by NetId, metres to 1 mm), logged by the game, `vortex_bot` and `vortex_server` (after its entities line); `soak.sh` and `session-check.sh` require the same NetIds awake, each within 5 cm of the server's (a tolerance in the script: no quantisation cell a still body could straddle) |
| R6 | "Not every bot was ready to leave" was printed but not counted; `wait` without a limit (low) | **Real** | Counted as a failure; every wait for a bot or the server is bounded (`wait_limit`: 60 s after a bot's leave word, then killed and counted) |
| R7 | Self-test fault 2 could not fail; the self-test did not show that the normal verdict path counts the faults; no bot that exits 0 without draining (low) | **Real** | The self-test reads the normal verdict's own messages: `bot 3 exited 1 ... differed with no fault injected`, `bot 4 exited 137` **and** `FAIL: bot 4 logged no hash`, `FAIL: bot 0 (slot S): the server logged no leave line`, and a fourth fault: bot 5 (`vortex_bot --test-skip-drain`) leaves at a third of the run during the fight without draining and exits 0 - the comparison must name it. It passes only with all four named and at least four failures counted |
| R8 | Per-client byte and correction rates were averaged over the whole process life (drain and the wait for the leave word included): the arena run-2 rates about 35 % low, and the explanation of the lower arena event rate wrong (medium-high) | **Real** (run 3's arena logs: events 21.4-23.0 kB/s down, as on the playground) | `vortex_bot` snapshots its transport, relay and prediction counters when its join data is in and when play ends: `Traffic: bot N: play S s: up ... down ... (state / events / bulk); join ...; drain and wait ...`, the `Relay:` line per phase, and a new `Play: bot N:` line (corrections per minute of play, clock jumps and resyncs in play). The Bytes, Re-sent, Corrections rows, O3 and ROADMAP are restated from the rerun below; "the window holds fewer of the casts' chunks" is withdrawn |
| R9 | O4 compared "during joins" with "all events" (which contain the joins), and its restated targets were not supported by the numbers (medium) | **Real** | `EventLatency::quiet` (events made while no client joined) on the `Latency:` line; O4 and the Testing row restated from quiet against during-joins in the rerun; the 1 %-loss target over real ENet is said to be **missed** (274-339 ms during joins in the first pass and run 4, 10-35 % over 250 ms); "holds on a LAN" removed (not measured) |
| R10 | Run 3's 2.4 s walker p99 was put down to a stall without evidence (medium) | **Real** (the walker's log shows no resync and a margin of 0..7) | **Taken in part.** The stall claim is withdrawn ("Runs" says "not explained"). The rerun's walker (run 4, the same arena scenario, the clock fix in) has 0 clock jumps, bulk 44.8 kB/s in play, and a **quiet p99 of 1494 ms** (max 2190) against the wanderer's 580 over the same events (its all-events p99 of 629 ms is the comparison R9 rejected; the first text of this row said "the 2.4 s did not recur" from it: withdrawn by the recheck, RC1). The client with the most bulk has the worst latency, the pattern this finding asked about: **open**. The 10-minute walker A/B with the largest bulk in flight and ENet's `packetThrottle` per peer that the finding asks for was **not run** (it needs per-peer server counters and an ENet accessor; open below); item 10's "no worse event latency" stands on the 3-minute A/B only, and says so |
| R11 | O5's own rule says build the minimum bulk share; the ratio was miscomputed (low-medium) | **Real** (4.3-4.6x, not 4.7-5.0x; over the rule's 4x either way) | Ratio corrected; see O5 below for what was run and decided |
| R12 | The Server tick row's explanation did not match what the tick timer covers (low) | **Real**: `Server::Tick` times receive, players, the casts, physics, events, snapshots and the hash refresh; the drill's rebuilds (`net::RunDrillStep`: `FillBox` and crates, between ticks) and the transport flush are outside it | The row says so and attributes the playground p99 to the drill's casts (inside the tick); `vortex_server` logs a pass-time p50 / p99 (`Server: longest pass ...; passes that ran a tick: p50 ..., p99 ...`) beside `Ticks:` for stage 9 |
| R13 | A9's "an ulp never fails a run" was false (low) | **Real** | A9 and the notes reworded: the rest hash relies on stage 6's bit-identical rest poses; quantisation only makes it less sensitive |
| R14 | Game-client numbers taken in a quiet world after the fight were reported as comparable (low) | **Real** | Marked "joined after the fight, no casts"; O6 rests on `session-check.sh`'s 1.7-3.3 ms under a drill |
| R15 | `vortex_bot` left the time spent inside each pass out of its clock: the clock ran slow and jumped forward 1-4 times a second (high) | **Real** (reproduced: `Bot.PassOnClockCountsTheWorkInsidePasses` failed on the old timing with 343 of 1200 ticks and 43 clock jumps in 20 s) | `net::Bot::PassOnClock(clock, ...)`: the seconds since the last pass **began**; the pass that loaded the region counts the next one from its end. `vortex_bot` uses it. The test: passes that spend 6 ms inside (a transport whose `Flush` advances the manual clock) and sleep 1 ms: 1199 of 1200 ticks, 0 jumps, 0 resyncs. The movement rows and bot 0's "presentation" attribution are restated from the rerun |
| R16 | `Relay.ShapesRealUdpTraffic` judged the outage by when the receiver read a datagram, with 20 ms of margin (medium) | **Real** (a flake risk under load) | Each ping carries its send time (State channel: never re-sent); pings sent well inside the outage ([0.65, 0.85) of [0.6, 0.9)) must never arrive, and at least half of those sent outside it must; `received > 100` replaced by that. **Recheck (RC4):** that still judged by the sending thread's stamp, not at the relay; now by the relay's own read times (`RelaySettings::onAdmit`) |
| R17 | A signal after the play phase, or a drain that never held, still let `vortex_bot` print `ok` and exit 0 (low) | **Real** | A signal in any phase fails it (`stopped by a signal`); a drain that did not settle (no `--leave-file`) or did not hold again within 15 s of the leave word fails it |
| R18 | `vortex_bot` could run forever and defaulted to port 27015 (low) | **Real** | `--connect HOST:PORT` (the port is required) and `--seconds` or `--end-at` are required (exit 2 otherwise). Not taken: refusing 27015 outright - the owner may point a bot at a server of his own there |
| R19 | A SIGKILLed `soak.sh` left the server and bots running for up to 15-25 minutes, holding the port (low) | **Real** | `vortex_server` and `vortex_bot --parent-pid PID` (Linux: `prctl(PR_SET_PDEATHSIG, SIGTERM)`, refused if PID is not the parent already); `soak.sh` passes `$$`; `--leave-wait` is the scenario's length + 600 s instead of a fixed 900 |
| R20 | `soak.sh` left an old `game.png` in place (low) | **Real** | Removed with the logs; a missing screenshot fails the scenario with `GAME_CLIENT=1` |

#### The review's rerun (2026-11-18, the fixes above built)

| Run | Result |
|---|---|
| Build, tests | Warning-free; 720 tests (six disabled) pass twice under `nice -n 10 ctest -j8` after the last change (once more before it; the relay test's handshake through 5 % loss once missed the fixed outage window it was given: the outage now starts from the connection, `UdpRelay::AddOutage`, and three solo runs and both suite runs since passed) |
| Game | The standard headless run (`Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, `on the ground`; screenshot looked at), the editor run, `--climb` (0 unloads inside the view radius), `--tower-blast --after-fill --frames 700` (freed by shot 6 of 7, the load check) and `--neck-blast --frames 200` (0 pieces released by the duds, the tower released): all exit 0 under validation |
| `vortex_server` | The CLAUDE.md drill (`hashes equal`, 67/67, 67/67, 66/66, join p99 11 / 9 ticks), the UDP drill (`--enet`, the same), the arena walk (`hashes equal`, stores at most 5500 / 5442, join p99 11 ticks) |
| `tools/session-check.sh` (PORT 27042) | PASS; both clients' hash, entities, rest hash and awake lines equal the server's; checks 1.8 / 2.4 ms at most; screenshots looked at (the plaza, crates, craters, HUD ONLINE) |
| Short soak with `GAME_CLIENT=1` (PORT 27043), both regions | PASS; the presenting bot 1 clock jump and 21 corrections a minute of play (it had about 2400 jumps and 278 a minute in run 2: R15) |
| `SOAK_SELFTEST=1 tools/soak.sh --short` (PORT 27044) | PASS: the four faults named by the normal verdict's messages, 6 failures counted |
| **10-minute soak, run 4** (PORT 27040, `GAME_CLIENT=1`, `TERRAIN=1`), both regions | **PASS on both**: playground 93 514-93 538 operations, arena 91 422-91 442; every bot's and the game client's hash over its sync box, entity line and awake line equal the server's at the same opSeq (one body awake at the end on each region - pg at 0.06 m, ar frozen at -46.75 m below the window, as in item 14 - the same NetId and position on every client); the drill 5800 of 5800 steps on both regions, every kind freeing terrain (run 2's report counted 4861 and "0/1736": R3); 5 clients joined and left, none timed out; 1 repair requested (pg) and 0 (ar), as injected. The numbers are in "Stage 8 measurements" (restated) |
| O5 A/B (two 5-minute playground soaks side by side, the paced window against a fixed 32 KiB one) | Both PASS; see O5 in "Decisions from the numbers" (the presenting bot of the paced arm stalled - 14 hard resyncs, a margin up to 207 ticks, with two soaks on the laptop - so only the joiner's numbers compare) |
| Short soak at 1 % loss (playground) | PASS; latency in O4 |

#### What is left (after the review)

Not done, each with a recommendation:
- **M1's acceptance: the owner's LAN run on stage 8's commits** (`docs/two-pc-test-windows.md` 6.5; the short soak as
  its pre-check). Windowed play, the real network and a Windows client are the owner's; an assistant ran headless
  clients and bots on one machine only.
- **The commits** (engine first, then the game). Recommendation: commit stage 8 with this review now; the owner's run
  needs both machines on one commit.
- **The walker A/B of R10** (10 minutes, `--pacer-delivery-interval 0` against the default, the same relay seeds,
  with per-peer largest bulk in flight and ENet's `packetThrottle` on the server's leave line). Superseded by the
  recheck's list below (RC1: run 4's walker has a quiet p99 of 1.49 s, so the question is open, not answered).
- **What limits a lossy 100 kB/s link** (O5: the join runs at 22-27 kB/s of bulk whatever the window, and a larger
  window only fills the bottleneck's queue). Recommendation: M6, with ENet's reliable window and back-off measured per
  peer; not a stage 8 change.
- **Re-sent chunks for a player crossing a large region** (O3: 22 KiB/s, half a walker's bulk). Recommendation: the
  memory milestone ("have" list plus a client cache of left slabs), as recorded.
- **Bodies frozen awake for good below the arena's window** (O11; one in run 4, compared within 5 cm on every client
  now). Recommendation: M3 (collapse) or the next arena work: let the server put such a body to rest (or despawn it)
  when no collider can ever be built under it, so it stops counting against the awake cap.
- **The 1 %-loss latency target over real ENet** (O4: 274-312 ms during joins). Recommendation: keep 250 ms as the
  in-process gate and ask the owner's LAN run for the HUD ping and the corrections; an absolute event-latency figure
  on a LAN needs a shared clock across machines (not built).
- Sharing the hash cache with `HashCheck` (A2) and a wall-clock `--exit-at` for a game client that plays in the soak
  (A5): still open, still small, not needed for M1.

### Recheck of stage 8 (2026-11-18)

A rechecker read the stage 8 change after the review's fixes and raised 8 findings (RC1-RC8). Each was checked
against the code and run 4's logs; the bugs among them were shown first in a GPU-free test or a run that failed on the
old code. **7 taken (fixed or the text corrected), 1 taken with a part rejected**. Protocol **10** unchanged (no wire
byte changed: the new join point is in log lines, the relay hook is a test hook). **722 tests** (six disabled; two
new, two extended). No fix touched what is replicated (log lines, a test hook, a tool's histogram, the bot's own
verdict, the scripts), so the 10-minute soak was not rerun; the short soak, its self-test and every other check
were ("The recheck's verification", below).

| # | Finding (severity) | Verdict | Fix |
|---|---|---|---|
| RC1 | The latency claims (O4, the Testing row, R10, ROADMAP: "quiet p99 0.58-0.73 s on plain links ... a join adds about a tick", "the 2.4 s did not recur") do not match run 4's logs: the walker's quiet p99 is 1.49 s, the arena's standing terrain-mode bot 793 ms is left out, and the A/B logs show 1.29-2.57 s (medium) | **Real**, one part **rejected**: the O5 A/B's 1.29-2.57 s are not plain links - they are bot 1, whose 2 s outage fell in that run's quiet window (90 s, the joiner at 150 s), and the 100 kB/s joiner; the plain bot 0 there has 550-666 ms. Everything else as stated: the walker's quiet p99 is 1494 ms (max 2190) against the wanderer's 580 over the same 11 504 events while its p99 during joins is 590; the 629 ms the text quoted is the all-events p99 R9 rejected; "a join adds about a tick" rests on playground bot 0 alone; the all-events range left out bot 1 (1365 ms, outage) and the arena event range the terrain-mode bot (19.1 kB/s) | O4, the Testing row, the latency, during-joins and bytes rows of "Stage 8 measurements", R10 and ROADMAP restated: quiet p99 **0.58-0.79 s** for the plain-link bots that stand or wander (580-793 ms), the walker apart (1.49 s, max 2.2 s), behind 100 kB/s 0.85-1.29 s across runs; all-events p99 for every bot with what made each high; the terrain-mode bot's 19.1 kB/s explained (97 s of play ending with the 20 s after the drill); "did not recur" and "no event-latency cost of the stage 8 pacer for the walker" withdrawn; "a join adds about a tick" said to come from one bot on one region. **R10 stays open**, with the walker's quiet p99 as the evidence for the 10-minute pacer A/B (below) |
| RC2 | The soak's terrain-mode check on the game client can never fire: `DescribeTerrainMode` says `no`, or `since the Welcome (...)` / `at tick N (pristine chunks made differently)`, never the words the script looked for (low-medium) | **Real** (a game client in terrain mode was still caught, by `Session.cpp`'s exit code only) | `soak.sh` requires the `Terrain: slot S:` line's value to be exactly `no` (empty line: a failure as before); checked against the three forms the code prints |
| RC3 | `compare` matched leave lines by slot only; a reused slot compared one client with another's leave line - the self-test's fault 4 was named that way (bot 5 against bot 2's line) (low) | **Real** | The join point names a client: `Client::JoinTick`, `Server::ClientJoinPoint`; `SessionHashLine` and the server's leave line (now `net::ServerLeaveLine`, shared, so its format is tested) end `joined at opSeq J, tick T`; `compare` takes the leave line with the same slot **and** join point; the self-test's fault 3 removes bot 0's own line only, fault 4 must be named by a drain message against its own line. `Bot.TwoSharedBotsJoinPlayAndDrain` checks both lines carry the client's join point and the server's accessor agrees. The join **tick**, not only the opSeq: two clients in one slot can share a join opSeq (no operation between a leave and the next join), never a join tick |
| RC4 | `Relay.ShapesRealUdpTraffic` still depends on thread timing at the outage's end; R16's text says the outage is judged at the relay, which the code did not do (low) | **Real** (shown: a 70 ms sleep injected between a ping's stamp at 0.84-0.85 s and its flush made the old judgement count one arrival "in the outage", three runs out of three; the new one 0) | `RelaySettings::onAdmit` (a test hook: the relay's read time of each datagram and whether it dropped it there). The test numbers its pings (a marker the hook finds in the datagram: ENet sends unreliable payloads as they are) and judges both directions on the relay's times: no ping the relay read inside the outage arrives, none arrives unread, the relay read and dropped pings inside the outage, most it forwarded arrived. R16's wording now matches |
| RC5 | R3's gates had one negative test; the walk, acted-share and rejected-cast gates were never shown to fail, nor `WantedTicks` to leave out the wait for a repair (low) | **Real**, and it hid a bug: the walk gate's span was the waypoints' span, but the pilot counts a waypoint reached 2 m short of it, so a walker that reached every waypoint covers the span less 4 m - on a 7-chunk region exactly 80 %, at the gate's edge (`Bot.ThePlayedGatesFailWithTheirOwnReasons` with a share of 0.95 failed on the old span) | `JudgeBot`'s walk span is the waypoints' less 2 x `reachMetres`. New test `Bot.ThePlayedGatesFailWithTheirOwnReasons`: a walker judged before its second waypoint (`it reached ...`), against 1.5 of its span (`it walked x from ...`), and passing 0.95 after reaching all; a bot casting at the sky (every cast `Missed`: 50 of 50 refused) fails `maxRejectedCastShare` 0.2 with that reason only and passes 1.0; `minActedShare` 1.01 fails (`its pilot drove ...`). `TwoSharedBotsJoinPlayAndDrain` asserts `WantedTicks` stays 0 while the broken-chunk bot waits for its repair. Not built: a client that stops predicting - `Client` never clears `predicting` once set, so for an honest bot acted == wanted (asserted) and the gate guards a regression only; a test hook in the client for it was judged not worth its code |
| RC6 | `vortex_server` kept every pass time (`passBusy`) of a listening server and sorted a copy at exit: about 0.9 MB an hour (low) | **Real** | `net::DurationHistogram` (0.05 ms buckets to 100 ms, an overflow bucket answering with the largest value; 16 KB whatever the run's length); the `Server: longest pass ...` line reads its p50 / p99. Test `Bot.DurationHistogramKeepsPercentilesInFixedMemory` (within one bucket of `PercentilesOf` over 200 000 values; the overflow case) |
| RC7 | `soak.sh` ran the game client in the foreground with no time limit and no `--parent-pid` (low) | **Real** | `timeout -k 10 600` around it, in the background, in the started list and collected with `wait_limit` (exit 124: a failure named `did not exit within 600 s`); `timeout` also ends it when the script is SIGKILLed (the game takes no `--parent-pid`) |
| RC8 | `awake_differ` named the wrong side when the client's awake list was empty (awk's `NR == FNR` on an empty first file) (low) | **Real** (checked: an empty client list against one server object said "awake on the client only") | `FILENAME == ARGV[1]` in `soak.sh` and `tools/session-check.sh`; checked with an empty list on either side |

#### The recheck's verification (2026-11-18)

| Run | Result |
|---|---|
| Build, tests | Warning-free (the changed modules rebuilt with a full log); **722 tests** (six disabled) pass twice under `nice -n 10 ctest -j8` (117 s and 115 s) |
| Test-first | `Bot.ThePlayedGatesFailWithTheirOwnReasons` failed on the old walk span (a walker at x -10..6 m with all 3 waypoints reached failed a 0.95 share of the 20 m span); RC4's old judgement counted 1 arrival "in the outage" in 3 of 3 runs with a 70 ms stall injected between a ping's stamp and its flush (a temporary change, removed), the new one 0 |
| Game | The standard headless run (`Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, `on the ground`; screenshot looked at), the editor run, `--climb` (0 unloads inside the view radius), `--tower-blast --after-fill --frames 700` (freed by shot 6 of 7, the load check, 11.27x) and `--neck-blast --frames 200` (0 pieces released by the duds, the tower released at 5.61x): all exit 0 under validation |
| `vortex_server` | The CLAUDE.md drill (`hashes equal`, 67/67, 67/67, 66/66, join p99 11 / 9 ticks), the UDP drill (`--enet`, 3 clients, `hashes equal`; pass p50 0.35 ms / p99 12.65 ms from the new histogram), the arena walk (`hashes equal`, stores at most 5501 / 5452) |
| `tools/session-check.sh` (PORT 27046) | PASS; both leave lines end `joined at opSeq 64, tick 1` (both clients joined in one tick, other slots); checks 2.7 / 3.1 ms at most; screenshots looked at (the plaza, crates, craters, HUD ONLINE) |
| Short soak with `GAME_CLIENT=1` (PORT 27047), both regions | PASS: three bots and the game client equal the server's on each region (its `Terrain: slot 3: no`); screenshots looked at (the ruined playground with another player, the arena's craters and crates) |
| `SOAK_SELFTEST=1 tools/soak.sh --short` (PORT 27048) | PASS: four faults named, 6 failures counted; bot 5 (slot 4, joined at tick 311) is now compared with its own leave line (opSeq 2702), not the late joiner's in the same slot (joined at tick 1873, left at 6787); fault 3 names bot 0 by its join point |
| 10-minute soak | Not rerun: no fix changed what is replicated or how (log text, a test hook, a tool histogram, the bot's own verdict, the scripts) |

#### What is left (after the recheck)

Not done, each with a recommendation (the review's list above, as amended):
- **M1's acceptance: the owner's LAN run on stage 8's commits** (`docs/two-pc-test-windows.md` 6.5; the short soak as
  its pre-check). Windowed play, the real network and a Windows client are the owner's.
- **The commits** (engine first, then the game). Recommendation: commit stage 8 with its review and this recheck
  now; the owner's run needs both machines on one commit.
- **R10, the walker's latency** (run 4: quiet p99 1.49 s against 0.58-0.79 s for the other plain-link bots; the
  client with the most bulk, 44.8 kB/s, has the worst event latency). Recommendation: the 10-minute arena A/B before
  M6's pacer work and before any claim that the stage 8 pacer costs live events nothing - `--pacer-delivery-interval 0`
  against the default, the same relay seeds, two walkers and two wanderers, with per-peer bulk in flight and ENet's
  `packetThrottle` on the server's leave line (a small accessor in `Enet.cpp`, not built). Not an M1 blocker: a
  player crossing the arena gets holes about a second late at 5 % loss, and the owner's LAN run reports how play
  feels.
- **What limits a lossy 100 kB/s link** (O5), **re-sent chunks** for a player crossing a large region (O3), **bodies
  frozen awake** below the arena's window (O11), **the 1 %-loss latency target over real ENet** (O4), sharing the hash
  cache with `HashCheck` (A2) and a wall-clock `--exit-at` (A5): as in the review's list, with its recommendations.
