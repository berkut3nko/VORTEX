# [nat-R0] NAT probe and pair test, finished: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/rendezvous|rendezvous]]

| | |
|---|---|
| Stage | `[nat-R0]`: review and finish the WIP (STUN, hashes, punch datagrams, `Traversal`, ENet raw hooks), the two command-line tools, the documents |
| Design | [rendezvous.md](../design/rendezvous.md): "Decision in brief", "Flow", "The engine side", "Punch datagram", "Command lines", "Tests", "Stages" (R0 and "R0's pair test, with no Worker") |
| Code read at | engine `bafca16` (the WIP: `wip/nat-r0`, where `llm-tasks` starts), game `0e27ea7` |
| Depends on | nothing beyond the WIP (it is on `engine-rewrite` + WP1 + WP2) |
| Cards | 3: review and fix (1 d), the tools and the loopback pair run (0.5 d), close (0.5 d) |
| Wire / rules / hashes | none: no byte of the session protocol changes (punch datagrams sit outside ENet's format), no `kProtocolVersion` bump, `StepPlayer` unchanged, `buildHash` unchanged, no gate added |

## Before you start

- `git -C engine log --oneline -1` shows `bafca16 WIP [nat-R0]: ...` (or a later card of this pack); the game is at
  `0e27ea7` or later. Both repositories on `llm-tasks`, clean.
- `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` exits 0, then `tools/ctest.sh -R 'Stun\.|Sha256\.|Rendezvous\.|Traversal\.|Enet\.' --output-on-failure`
  passes (19 tests: the 15 of the WIP plus 4 older `Enet.*` ones).
- What the WIP built (all in `engine/src/net/`, all GPU-free and socket-free except `EnetTransport`):
  - `Address.cppm` (`:address`): `Endpoint` (IPv4 + port, host byte order), `Ipv4`, `Datagram`, `FormatEndpoint`,
    `ParseIpv4Endpoint`, `IsPrivateIpv4`, `LocalInterface`, `IsLanInterface`, `InSubnetOf`, `LocalInterfaces()`.
  - `Crypto.cppm` (`:crypto`): `Sha256` (`Update`, `Finish`, static `Of`), `HmacSha256(key, parts)` and
    `HmacSha256(key, message)`.
  - `Stun.cppm` (`:stun`): `MakeStunBindingRequest`, `LooksLikeStun`, `ParseStunBindingRequest`,
    `MakeStunBindingResponse`, `ParseStunBindingResponse` -> `StunResult`, `NatClass` + `ClassifyNat`,
    `StunProbe` (`Settings{retrySeconds 2, refreshSeconds 15, triesPerRound 3}`, `Update`, `OnDatagram`, `Settled`,
    `Mapped`, `Outside`, `Class`, `TakeChange`).
  - `Traversal.cppm` (`:traversal`): `CandidateKind`, `Candidate`, `CandidateKinds`, `ParseCandidateKinds`,
    `FormatCandidates` / `ParseCandidates`, `RoomKey` / `HostKey` / `RoomTag` / `CandHash`, `Punch`, `EncodePunch` /
    `DecodePunch` / `PunchProof` / `HasPunchMagic`, `TraversalLimits` / `PairTestLimits()`, `ClientJoinSetup`,
    `ClientTraversal`, `ServerJoin`, `ServerTraversal` (`AddJoin` -> `AddJoinResult`, `SentFor`, `SentToStunFor`,
    `NonceFor`), `PunchTestSettings`, `PunchTest` (`Phase`, `Paste`, `Line`, `TakeLog`), `kPunchLinePrefix = "VXPT1"`.
  - `Punch.cppm` (`:punch`): `kDefaultStunServers`, `WantsNatTool`, `RunNatTool(argc, argv, PunchRole)`; its
    `internal/Punch.cpp` holds `NatProbe` and `PairTest` (stdin read on a detached thread, 5 ms loop).
  - `Enet.cppm` / `internal/Enet.cpp`: `EnetSettings::interceptRaw`, `EnetStats::rawTaken / rawDropped /
    strangersDropped`, `EnetTransport::Open / ConnectTo / SendRaw / TakeRaw`, `Connect` = `Open` + `ConnectTo`,
    `kMaxRawWaiting = 256`, `ResolveEndpoint`; in `Enet.cpp` the always-installed `Impl::Intercept`, `CreateHost`
    (`IgnoreConnectionResets`: `SIO_UDP_CONNRESET` off under `_WIN32`), `g_Hosts` under `g_HostsMutex`.
  - Tests: `engine/tests/net/TraversalTests.cpp` (`Stun.*` 2, `Sha256.*` 2, `Rendezvous.KeysOfAKnownToken`,
    `Traversal.*` 8, over `engine/tests/net/NatSim.h`), `engine/tests/net/EnetTests.cpp`
    (`Enet.APunchOrStunDatagramNeverReachesENet`, `Enet.AClientHostTakesNoStrangerWhilePunching`).
  - Callers: `engine/tools/server/main.cpp` and the game's `src/main.cpp` call `net::RunNatTool` when
    `net::WantsNatTool(argc, argv)`, before anything else (server role / client role).

## Decisions (us, 2026-11-22; the owner may veto)

1. **The unverified cap counts probes only.** rendezvous.md "What each side may send" says at most 10 datagrams to
   an unverified join's `stun` candidates. Probes are the unsolicited ones that could aim the server at a stranger;
   an Ack only answers a datagram that proved the token from that address. Counting Acks would stop an EIM client
   whose probes come from its own `stun` address from ever being answered. So: at most 10 **probes** to an
   unverified join's `stun` candidates; Acks count only toward the 100. Card 1 fixes the counter and amends the
   design's sentence.
2. **A finished join keeps its slot** until 15 s after it came (it still answers late probes); `maxJoins` (4) counts
   every join held, finished or not. The Worker admits 4 joins a minute per room, so slots never run out in practice.
3. **At most 4 `lan` candidates**, in interface order, so that `lan` never crowds `given` and `stun` out of the 8.
4. **More adapter names are excluded** from `lan` candidates: prefixes `vnet`, `utun`; parts `tap-windows`, `vpn`,
   `npcap`. Loopback, link-local and 100.64.0.0/10 stay excluded by "private (RFC 1918) only".
5. **`--candidates` with an empty list or an empty word** (`""`, `lan,,stun`, `lan,`) is a bad command line (exit 2).
6. **Exact log lines** (docs/verify.md gets them in card 3):
   - `NAT: outside <A.B.C.D:P | ->, class <eim | symmetric | unknown>` - the same line in `--nat-probe` and in
     `--punch-test` (the probe's `(UDP port P)` suffix moves to its own line `NAT: UDP port P`).
   - `NAT: STUN server <A:P> answered <A:P | nothing>`, `NAT: interface <name> <ip>/<mask>[ (lan candidate)]`,
     `NAT: the outside address changed to <A:P | ->: ...`.
   - `Punch: line pasted at HH:MM:SS UTC (N s after start)`, `Punch: join 1 reached <A:P> (<kind>) in <N> ms` (N from
     this side's own paste), `Punch: join 1 failed after <N> s: <reason>`, `Punch: ENet connected to <A:P> in <N> ms`
     (client), `Punch: ENet peer <A:P> connected over the punched path` (server), `Punch: the pair test is done`.
   - "The gap between the two pastes" (rendezvous.md "R0's pair test") is read from the two sides'
     `Punch: line pasted at` times; neither side can know the other's.
7. **doc-check's four citation findings** are fixed by a row in the anchor table of engine/docs/README.md (the code
   comments stay as they are: they name the bold bullets "Keeping mappings", "Choosing", "Timeouts" of "Flow" and
   the paragraph "R0's pair test, with no Worker" of "Stages").
8. **`SIO_UDP_CONNRESET`** cannot be compiled here (Linux): it is checked by reading, and the owner's Windows build
   (owner check 16, card 3) compiles it.
9. **No new gate, no new test infrastructure.** The loopback pair run of card 2 is a recorded manual check (the
   recipe and its result go into docs/verify.md), not a test. Real STUN servers are never asked by you: every run
   passes `--stun 127.0.0.1:27067`, where nothing answers.

## Skeleton

No new interface. Changed: the doc comments of `TraversalLimits::maxUnverifiedStun` and `ServerTraversal::SentToStunFor`
(probes only), `IsLanInterface`'s lists, `PunchTest::MakeLine` (lan cap), `ParseCandidateKinds` (empty refused),
`NatProbe`'s last line. New tests (all in `engine/tests/net/TraversalTests.cpp`, suite `Traversal`):
`Traversal.LanCandidatesAreOnlyPrivateNonVirtualAddresses`, `Traversal.AnUnverifiedJoinStillAcksAfterItsTenProbes`
(card 1), `Traversal.WithNoStunAnswerTheLineIsPrintedAndTheClassIsUnknown` (card 2).

## Card 1: Review the WIP against the design and fix what differs

| | |
|---|---|
| Goal | The punching code does exactly what rendezvous.md promises (layout, proof, caps, filters, candidates), so the owner's pair test measures the real thing; doc-check passes again. |
| Depends on | the WIP (engine `bafca16`) |
| Effort | 1 d (e) |
| Touches | `vortex.net` (`:traversal`, `:address`); wire: no; movement rule: no; buildHash: no |

**Read first** (about 70 KiB)
- `engine/docs/design/rendezvous.md` "Punch datagram" (whole section), "The engine side" (the `EnetTransport`
  bullets), "Flow" ("Timeouts", "Choosing", "Keeping mappings").
- `engine/src/net/Traversal.cppm` (whole, 15 KiB) and `engine/src/net/internal/Traversal.cpp` (whole, 30 KiB).
- `engine/src/net/internal/Address.cpp`: `IsLanInterface`, `InSubnetOf`.
- `engine/src/net/internal/Enet.cpp`: `grep -n 'Intercept\|CreateHost\|IgnoreConnectionResets\|clientHost'`, read
  those functions only.
- `engine/tests/net/TraversalTests.cpp` lines 1-186 (the helpers `TokenOf`, `PunchFrom`, `CountKind`, `RunPair`) and
  `TEST(Traversal, CapsWhatAJoinMakesEachSideSend)`.

**Files**
- change `engine/src/net/internal/Traversal.cpp`: `ServerTraversal::Send`, `ParseCandidateKinds`, `PunchTest::MakeLine`.
- change `engine/src/net/Traversal.cppm`: doc comments of `TraversalLimits::maxUnverifiedStun`,
  `ServerTraversal` (class comment: slots), `ServerTraversal::SentToStunFor`.
- change `engine/src/net/internal/Address.cpp`: `IsLanInterface`.
- change `engine/tests/net/TraversalTests.cpp`: two new tests at the end.
- change `engine/docs/README.md` ("Anchors cited from code" table), `engine/docs/ROADMAP.md` (the `Tests:` line),
  `engine/docs/design/rendezvous.md` ("What each side may send").

**Steps** (each finding: check, then fix where the card says so; write the two tests first, see them fail, then fix)

1. *Layout and proof (no change expected).* Confirm in `EncodePunch`: bytes 0-3 `"VXPN"`, 4 version 1, 5 kind
   (1 Probe, 2 Ack), 6 sender (`'S'` 0x53 / `'C'` 0x43), 7-14 room tag, 15-22 client nonce big-endian, 23-30 server
   nonce big-endian, 31-38 proof; `kPunchBytes == 39`; `DecodePunch` refuses any other size. Confirm `PunchProof`
   hashes `"vxpn1"`, then the 27 bytes 4..30 of the encoded datagram, then the 8-byte `candHash`, under the token's
   16 bytes, and keeps the first 8 bytes. Then run this independent check; it must print the three values that
   `Rendezvous.KeysOfAKnownToken` expects (`94c0f469fd9e5d1ce662cc248d17ff7a f7282e40f4707117ccb8083adbd5a27a`, then
   `fd9260c2ec17aef4`):
   ```
   python3 -I -c "
   import hashlib,hmac
   t=bytes(range(16)); room=hashlib.sha256(b'vortex-room-v1'+t).digest()
   print(room[:16].hex(), hmac.new(t,b'vortex-host-v1',hashlib.sha256).digest()[:16].hex())
   b=bytes([1,1,0x43])+room[:8]+(0x0123456789abcdef).to_bytes(8,'big')+bytes(8)
   ch=hashlib.sha256(b'10.0.0.2:5000/lan').digest()[:8]
   print(hmac.new(t,b'vxpn1'+b+ch,hashlib.sha256).digest()[:8].hex())"
   ```
2. *Per-join cap (no change expected).* `ClientTraversal::Send` returns without sending once `m_Sent >=
   m_Limits.maxDatagrams`; `ServerTraversal::Send` once `j.sent >= m_Limits.maxDatagrams`; both count Probes and Acks
   (default 100; `PairTestLimits()` 4000 for the owner's own two machines).
3. *Unverified cap (fix, decision 1).* Today `ServerTraversal::Send` refuses a Probe when `j.sentToStun >= 10` but
   increments `sentToStun` for Acks too (`if (toStun) ++j.sentToStun;`), so Acks eat the probe budget. Change to
   `if (toStun && kind == PunchKind::Probe) ++j.sentToStun;`. Acks are never refused by this cap. Doc comments:
   `maxUnverifiedStun` "server: probes to an unverified join's stun candidates (Acks answer proven datagrams and count
   only toward maxDatagrams)"; `SentToStunFor` "... probes of it to its stun candidates".
4. *Join slots (no code change, decision 2).* `AddJoin` returns `Busy` when `m_Joins.size() >= maxJoins`; a join is
   erased in `Update` only when `done && now - start > acceptSeconds`. Add to the `ServerTraversal` class comment:
   "A join holds its slot until acceptSeconds after it came, finished or not."
5. *15 s freshness (no change expected).* `OnDatagram` drops a datagram when `now - j.start > acceptSeconds` (15 s
   default; 305 s in `PairTestLimits()`); `ClientTraversal` has no such limit (its nonce is fresh per join).
6. *Client host filter (no change expected).* In `Impl::Intercept`, after the raw take: a host made by `Open`
   (`clientHost`) drops every ENet datagram while `server` is empty and, after `ConnectTo`, every one whose
   `receivedAddress` (host and port) is not `*server`, counting `strangersDropped`. A `Listen` host never filters.
   The order is: test receive loss, then raw take (punch magic, or STUN with a transaction in `stunSent`), then the
   filter.
7. *LAN exclusions (fix, decision 4).* In `IsLanInterface` add `"vnet"`, `"utun"` to the prefix list and
   `"tap-windows"`, `"vpn"`, `"npcap"` to the part list (names are lower-cased first; Windows names are
   `"<FriendlyName> / <Description>"`). Keep `IsPrivateIpv4` as the first test (it is what excludes 127/8,
   169.254/16 and 100.64/10).
8. *LAN cap (fix, decision 3).* In `PunchTest::MakeLine` count the `lan` candidates added and stop adding `lan` after
   4 (the loop over `m_Settings.interfaces` keeps its order). `given` and `stun` are unchanged.
9. *Subnets (no change expected).* `ServerTraversal::AddJoin` keeps a client `lan` candidate only when
   `IsPrivateIpv4(ip) && InSubnetOf(ip, m_Subnets)`, keeps at most `kMaxJoinStunCandidates` (2) `stun` ones and drops
   every other kind; `PunchTest::Paste` (server role) passes the interfaces that pass `IsLanInterface` as subnets.
10. *`SIO_UDP_CONNRESET` (read only, decision 8).* In `Enet.cpp`: `<mswsock.h>` and the `#define` fallback sit inside
    `#if defined(_WIN32)` after `<enet/enet.h>` in the global module fragment; `IgnoreConnectionResets` calls
    `WSAIoctl(socket, SIO_UDP_CONNRESET, &report(FALSE), ...)` under `_WIN32` and does nothing elsewhere;
    `CreateHost` calls it for every host (`Listen` and `Open`). Change nothing unless one of these is false.
11. *Empty kinds (fix, decision 5).* `ParseCandidateKinds`: return false when `text` is empty or when any word is
    empty (split on `,`; `"lan,"` and `",lan"` and `"lan,,stun"` are false). `RunNatTool` already turns false into
    exit 2 with `--candidates expects a list of lan, stun, given`.
12. *Design text.* In rendezvous.md "Punch datagram", bullet "What each side may send", change "and at most 10 to an
    unverified join's `stun` candidates" to "and at most 10 probes to an unverified join's `stun` candidates (its
    Acks answer proven datagrams and count only toward the 100)".
13. *doc-check.* Append this row to the table under "Anchors cited from code" in engine/docs/README.md (after the
    `docs/two-pc-test-windows.md 4.9` row; re-read the file right before editing, other writers change it too):
    ```
    | rendezvous.md "Keeping mappings", "Choosing", "Timeouts", "R0's pair test" | [design/rendezvous.md](design/rendezvous.md) "Flow" (its bold bullets) and "Stages" ("R0's pair test, with no Worker") |
    ```
    Then update ROADMAP's `Tests: **N** in ctest: M GoogleTest cases` line (section "State"): M = the number doc-check
    reports as "the sources hold M TEST macros" after your two tests; N = the total of
    `ctest -N --test-dir build | tail -1` after the build. Change only those two numbers.

**Tests** (written first)
- `Traversal.LanCandidatesAreOnlyPrivateNonVirtualAddresses` (TraversalTests.cpp). Part 1, `net::IsLanInterface`
  with mask `Ipv4(255,255,255,0)`; true for `{"eth0", 192.168.1.10}`, `{"wlan0", 10.0.0.5}`, `{"enp3s0", 172.16.4.2}`,
  `{"Wi-Fi / Intel(R) Wi-Fi 6 AX201", 192.168.0.12}`, `{"Ethernet / Realtek PCIe GbE", 192.168.0.13}`; false for
  `{"lo", 127.0.0.1}`, `{"eth1", 169.254.3.3}`, `{"eth2", 100.64.0.1}`, `{"tailscale0", 100.101.1.1}`,
  `{"docker0", 172.17.0.1}`, `{"br-1a2b3c", 172.18.0.1}`, `{"virbr0", 192.168.122.1}`, `{"vboxnet0", 192.168.56.1}`,
  `{"vnet3", 192.168.122.7}`, `{"Ethernet 3 / TAP-Windows Adapter V9", 10.8.0.2}`,
  `{"vEthernet (WSL) / Hyper-V Virtual Ethernet Adapter", 172.20.0.1}`, `{"ProtonVPN / ProtonVPN Tunnel", 10.2.0.2}`,
  `{"eth3", 8.8.8.8}`. `net::InSubnetOf(Ipv4(192,168,1,77), {eth0 above})` true, `Ipv4(192,168,2,1)` false, an
  interface with mask 0 matches nothing.
  Part 2, the cap: a `PunchTest` (client role, `kToken`, two STUN servers `kStun1`, `kStun2`, `port` 6000, interfaces
  `"eth0"`..`"eth5"` at 192.168.1.10..15 /24, seed 3). Loop `t` from 0 to 1.0 in 0.01 steps: `Update(t, out)`; answer
  every datagram in `out` sent to `kStun1` with `MakeStunBindingResponse(t, {Ipv4(203,0,113,20), 6000})` and to
  `kStun2` with `{Ipv4(203,0,113,20), 6001}` (get the transaction with `ParseStunBindingRequest`), handed back with
  `OnDatagram({server, bytes}, t)`. Then `Candidates()` has 6 entries: 4 `Lan` at 192.168.1.10..13 port 6000, then
  2 `Stun` (6000, 6001); `Line()` is not empty. Fails before step 8 (6 lan, 2 stun: 8 entries).
- `Traversal.AnUnverifiedJoinStillAcksAfterItsTenProbes` (TraversalTests.cpp). `net::ServerTraversal s(kToken, {}, {}, 9)`;
  `cands = "203.0.113.30:6000/stun"`; `AddJoin({1, 0x31, cands, false}, 0.0)` == `Added`. Loop `t` from 0 to 1.95
  in 0.05 steps: `Update(t, out)`. Probes to `{Ipv4(203,0,113,30), 6000}` == 10 (`CountKind`). At `t = 2.0`:
  `OnDatagram(PunchFrom(that address, kToken, kToken, Probe, Client, 0x31, 0, cands), 2.0)` is true; `Update(2.0)`
  gives exactly 1 Ack and 0 Probes; the same again at 2.5 gives 1 Ack. `SentToStunFor(1) == 10` at the end (it is
  12 before step 3, which is how the test fails first).

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning
2. `tools/ctest.sh -R 'Traversal\.|Enet\.|Stun\.|Sha256\.|Rendezvous\.' --output-on-failure` - all pass (21)
3. `tools/ctest.sh` - all pass
4. `tools/doc-check.sh --build build` - PASS (the four `cite` findings and the `count` finding are gone)
5. no `verify.sh` run: nothing a gate runs changed (the NAT code runs only under `--nat-probe` / `--punch-test`)

**Docs**: engine/docs/README.md (anchor row), ROADMAP (test count), rendezvous.md (step 12).

**Must not break**
- `Rendezvous.KeysOfAKnownToken` values (they are the Worker's room and host keys in `[nat-R1]`).
- `Enet.APunchOrStunDatagramNeverReachesENet`, `Enet.AClientHostTakesNoStrangerWhilePunching`, every `Traversal.*`
  expected value (rule 14 of the guide): `CapsWhatAJoinMakesEachSideSend` must still count 100 and 10.
- CLAUDE.md section 4: only `Enet.cpp` includes `<enet/enet.h>`; Net stays GPU-free.

**Done when**
- [ ] the python check printed the three expected values
- [ ] `sentToStun` counts probes only; the two new tests pass; all tests pass
- [ ] `IsLanInterface` has the five new names; `MakeLine` adds at most 4 lan candidates
- [ ] `ParseCandidateKinds("")` and `("lan,,stun")` are false
- [ ] doc-check PASS; rendezvous.md's sentence amended; one commit per repository

**Traps**
- Do not "fix" the cap by refusing Acks: an EIM client whose probes come from its `stun` address would never be
  answered (decision 1).
- `PunchFrom`'s first argument is where the datagram comes *from*; `CountKind` counts by kind only, so filter `out`
  by `d.address` yourself for the per-address counts.
- The STUN answers in the cap test must come from the server's own address, or `StunProbe::OnDatagram` ignores them.

## Card 2: Finish `--nat-probe` and `--punch-test`, and run the pair test on loopback

| | |
|---|---|
| Goal | The owner can run the NAT probe and the pair test on real machines: no STUN answer never stops the run, the log lines are the documented ones, and one recorded loopback run shows the whole path (paste, punch, ENet over the punched path). |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | `vortex.net` (`:punch`); wire: no; movement rule: no; buildHash: no |

**Read first** (about 40 KiB)
- `engine/src/net/internal/Punch.cpp` (whole, 13 KiB), `engine/src/net/Punch.cppm`.
- `engine/src/net/internal/Traversal.cpp`: `PunchTest::Update`, `PunchTest::Paste`, `PairTestLimits`.
- `engine/src/net/internal/Stun.cpp`: `StunProbe::Update`, `StunProbe::Settled`.
- rendezvous.md "Stages" ("R0's pair test, with no Worker") and "Command lines".

**Files**
- change `engine/src/net/internal/Punch.cpp`: `NatProbe` (its last log line).
- change `engine/tests/net/TraversalTests.cpp`: one new test.
- change `engine/docs/ROADMAP.md`: the test count.

**Behaviour to confirm** (read the code; each holds at `bafca16`, change nothing that holds)
1. No reachable STUN: names that do not resolve are dropped by `ResolveStun` with a warning; a `StunProbe` with no
   servers is `Settled()` at once; with servers that never answer, each is settled after 3 tries 2 s apart (about
   6 s). Then `NatProbe` logs and exits 0, and `PunchTest` leaves `Gathering` (settled, or `gatherSeconds` 7 s),
   prints its line with class `unknown` and waits for the paste. The run goes on.
2. While waiting for the paste, `PunchTest::Update` calls `m_Stun.Update` every pass: a new round every
   `refreshSeconds` (15 s), a retry 2 s after an unanswered try.
3. After a paste: `PairTestLimits()` = probe rounds every 0.1 s for 10 s, then every 1 s, until 300 s
   (`windowSeconds`); the server takes the client's datagrams for 305 s (`acceptSeconds`).
4. `RunNatTool` refuses port 27015 (`kDefaultPort`) for `--listen` / `--port` with exit 2.

**Steps**
1. In `NatProbe`, replace the last log call by two:
   `Log::Info("NAT: outside {}, class {}", outside ? FormatEndpoint(*outside) : std::string("-"), ToString(stun.Class()));`
   and `Log::Info("NAT: UDP port {}", transport->Port());`. No other log line changes.
2. Write the test below, build, run it.
3. Run the two loopback runs below (the probe, then the pair). Use only ports 27065-27067 and never real STUN
   servers. Save both logs under `build/nat-r0/` (not committed).
4. Put the commands you ran and the lines they printed into your report (card 3 copies them into docs/verify.md).

**Test**
- `Traversal.WithNoStunAnswerTheLineIsPrintedAndTheClassIsUnknown` (TraversalTests.cpp): a `PunchTest` (server role,
  `kToken`, `stunServers = {kStun1, kStun2}`, interfaces `{"eth0", 192.168.1.10 /24}`, `port` 5000, seed 4). Loop `t`
  from 0 to 8.0 in 0.01 steps, calling `Update(t, out)` and never answering. Asserts: `Line()` is empty at `t = 5.9`
  and not empty from some `t <= 7.0`; `CurrentPhase()` is `WaitingForPaste` at the end; `Class() == NatClass::Unknown`;
  `TakeLog()` holds `"NAT: outside -, class unknown"`; `Candidates()` is the one lan candidate `192.168.1.10:5000`;
  the line contains `"VXPT1 S "` and ends with `"192.168.1.10:5000/lan"`. Count the STUN requests sent to `kStun1`:
  3 by `t = 8.0` (tries at 0, 2, 4; the next round starts at 15 s).

**The two loopback runs** (from the game worktree root; the pair run takes about 15 s)
```
mkdir -p build/nat-r0 && cd build/nat-r0
../engine/tools/server/vortex_server --nat-probe --stun 127.0.0.1:27067 --bind 127.0.0.1 --listen 27065 > probe.log 2>&1; echo "exit $?"
# expect: exit 0 within about 7 s; probe.log has "NAT: STUN server 127.0.0.1:27067 answered nothing",
# "NAT: outside -, class unknown", "NAT: UDP port 27065".

T=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
rm -f s.in c.in && mkfifo s.in c.in
exec 7<>s.in 8<>c.in                      # keep both FIFOs open: the programs never see end of input
../engine/tools/server/vortex_server --punch-test --token "$T" --bind 127.0.0.1 --listen 27065 \
    --candidates given --public 127.0.0.1:27065 --stun 127.0.0.1:27067 < s.in > s.log 2>&1 & SP=$!
../Shredborne --punch-test --token "$T" --bind 127.0.0.1 --port 27066 \
    --candidates given --stun 127.0.0.1:27067 < c.in > c.log 2>&1 & CP=$!
for i in $(seq 1 30); do grep -q '^VXPT1' s.log && grep -q '^VXPT1' c.log && break; sleep 1; done
grep -m1 '^VXPT1' c.log >&7               # the client's line into the server
sleep 2
grep -m1 '^VXPT1' s.log >&8               # the server's line into the client
wait $CP; echo "client exit $?"; wait $SP; echo "server exit $?"
exec 7>&- 8>&-
```
Expected (both exit 0):
- both logs: `NAT: outside -, class unknown`, `Punch: line pasted at HH:MM:SS UTC (...)`;
- `c.log`: `Punch: join 1 reached 127.0.0.1:27065 (given) in N ms`, `Punch: ENet connected to 127.0.0.1:27065 in N ms`,
  `Punch: the pair test is done`;
- `s.log`: `Punch: join 1 reached 127.0.0.1:27066 (peer-reflexive) in N ms`,
  `Punch: ENet peer 127.0.0.1:27066 connected over the punched path`, `Punch: the pair test is done`.
Why these kinds: the client offers no candidate (`given` is a server kind), so its line ends in `-`; it probes the
server's `given` 127.0.0.1:27065 and is acked from there (kind `given`); the server learns 127.0.0.1:27066 from the
client's proven probe (peer-reflexive), probes it, and is acked. If a run differs, look first at which side pasted
what (`Punch: that line was not taken: ...`), then stop and report (do not change limits to make it pass).

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning
2. `tools/ctest.sh -R 'Traversal\.' --output-on-failure` - all pass
3. `tools/ctest.sh` - all pass
4. `tools/doc-check.sh --build build` - PASS (update the test count first)
5. the two loopback runs above, with the expected lines

**Docs**: ROADMAP's test count (the docs of the tools come in card 3).

**Must not break**: port 27015 is never used (decision 9 and CLAUDE.md "Ports"); `vortex_server` without
`--nat-probe` / `--punch-test` behaves as before (the NAT tool returns before `--session` is read).

**Done when**
- [ ] `NAT: outside ..., class ...` is the same line in both tools, `NAT: UDP port P` separate
- [ ] the new test passes; all tests pass; doc-check PASS
- [ ] both loopback runs printed the expected lines and exit codes; the report quotes them

**Traps**
- Without `exec 7<>s.in 8<>c.in` the programs' stdin opens block, or see end of input after the first `echo`.
- The `VXPT1` line is printed on stdout by `std::printf`, the log lines through `Log`; both go into the `.log` file
  here. Paste the whole line: `Paste` finds `VXPT1` anywhere in the text.
- Do not run `--nat-probe` or `--punch-test` without `--stun 127.0.0.1:27067`: the defaults ask Cloudflare's and
  Google's servers, which only the owner does.

## Card 3: Close [nat-R0]

| | |
|---|---|
| Goal | The owner can run the pair test from the documents alone and report results that decide rendezvous.md "Open questions" 1; the stage is recorded as built. |
| Depends on | cards 1-2 |
| Effort | 0.5 d (e) |
| Touches | documents only (both repositories); wire: no |

**Read first** (about 45 KiB): docs/verify.md "vortex_server" and "Sessions with the game" (headings and first
lines only, for the form); docs/owner-checks.md (whole, 12 KiB); engine/docs/notes/net.md section 1 (the `Enet.cppm`
bullet) and section 3 (headings); engine/docs/ROADMAP.md "Now" (first two bullets) and "Next" item 1;
rendezvous.md header table and "Stages".

**Steps**
1. **docs/verify.md**: a new section `## NAT tools (--nat-probe, --punch-test)` placed before `## Sessions with the
   game`, with: the two commands of rendezvous.md "Command lines" for R0 (both programs; flags `--token`, `--stun`
   repeatable, `--candidates`, `--bind`, server `--listen` and `--public`, client `--port`), exit codes (0 done /
   reached and ENet connected; 1 failed; 2 bad command line, port 27015 included), the exact lines of decision 6,
   and the loopback recipe of card 2 with the lines it printed and the date (a recorded manual check, not a gate).
2. **docs/owner-checks.md**: change the header sentence "The numbers 1-15 are fixed" to "The numbers 1-16 are
   fixed"; add a section `## Internet sessions (check 16)` before `## On Windows`:
   - 16. `([nat-R0].)` The pair test between two machines that must play together, one of them the Windows laptop,
     at the commits of this stage: first `--nat-probe` on each (`./build/engine/tools/server/vortex_server --nat-probe`,
     `./build/Shredborne.exe --nat-probe`), then `T=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')`, on the server
     machine `./build/engine/tools/server/vortex_server --punch-test --token $T --listen 27020 2>&1 | tee pair-server.log`,
     on the other `./build/Shredborne.exe --punch-test --token $T 2>&1 | tee pair-client.log`; copy each program's
     `VXPT1 ...` line into the other's terminal and press Enter. Pairs: the home line against the laptop on a phone
     hotspot, and against a tester's home line. Report per pair: both `NAT: outside ..., class ...` lines, both
     `Punch: line pasted at` times, the `Punch: join 1 reached ... (kind) in N ms` or `failed after N s` lines, and
     `Punch: ENet connected`. These results decide rendezvous.md "Open questions" 1 (a relay, `[nat-R2]`).
   - In "On Windows" add a row: `| 16 | Build at the stage's commits in MSYS2 CLANG64 (it compiles SIO_UDP_CONNRESET); run as above with Shredborne.exe. |`
3. **engine/docs/notes/net.md**: section 1 (Map): extend the `Net.cppm` bullet's partition list with `:address`,
   `:crypto`, `:stun`, `:traversal`, `:punch`; add bullets for `Address.cppm`, `Crypto.cppm`, `Stun.cppm`,
   `Traversal.cppm`, `Punch.cppm` (one line each: what they hold, naming the symbols of "Before you start"); extend the
   `Enet.cppm` bullet with `Open` / `ConnectTo` / `SendRaw` / `TakeRaw` and "the intercept is always installed".
   Section 3 (Traps): add three bullets:
   - "The intercept runs for every datagram of every host: receive loss first, then the raw take (punch magic
     `VXPN`, or a STUN answer to a transaction this host sent: the last 16), then a client host's filter (nothing
     before `ConnectTo`, afterwards only its server). Keep that order; never parse ENet's header in it."
   - "Why plain ENet would drop a punch or STUN datagram anyway: read as an ENet header, `VX` is peer id 1624 and a
     STUN answer's `0x0101` is peer id 257, both past any host's peer count (`Enet.APunchOrStunDatagramNeverReachesENet`).
     A session never lets ENet see them: do not raise `maxPeers` to 257 or more."
   - "`Traversal` is a pure state machine (no socket, no clock): tests drive it over `tests/net/NatSim.h`; the
     programs drive it over `EnetTransport::SendRaw` / `TakeRaw` on the thread that owns the transport, before any
     keep-alive thread starts."
4. **rendezvous.md**: header "Status" -> "R0 built (not reviewed): STUN, hashes, `Traversal`, ENet raw hooks, the NAT
   tools; R1 designed, nothing built. Reviewed, rechecked adversarially and amended twice." Stage row `[nat-R0]`
   "Done when": add at the end "Built: engine `<card 1 sha>`..`<this card's>`, game `<sha>` (2026-11-2x); the
   owner's pair results pending (owner check 16)." Use the real short hashes of your card commits; for this card's
   own commit write "this commit".
5. **ROADMAP.md**: "Now": replace the `[nat-R0]` bullet with "**[nat-R0] built** (2026-11-2x): the NAT probe and
   the pair test run on the owner's machines (docs/verify.md "NAT tools"); waiting for the owner's pair results
   (owner check 16), which decide `[nat-R2]`." and move `[nat-R1]` from "Next" item 1 to the top of "Now" (renumber
   "Next" from 1). Keep the owner's M1 acceptance bullet. Test count as in card 1.
6. **Verify**: `VERIFY_SLOT=1 tools/verify.sh full --ci` must PASS; then start `VERIFY_SLOT=1 tools/verify.sh soak`
   in the background and report its result when it ends (about 35 min). Look at the screenshots it lists.
7. `grep -rn 'DISABLED_' engine/tests tests | grep -i 'traversal\|stun\|rendezvous'` prints nothing.

**Checks**
1. `tools/doc-check.sh --build build` - PASS (it checks the test names you cite in verify.md, owner-checks.md and
   notes: cite only tests that exist)
2. `VERIFY_SLOT=1 tools/verify.sh full --ci` - PASS
3. `VERIFY_SLOT=1 tools/verify.sh soak` - PASS (background)

**Must not break**: the history folders (never edit a file under `history/`); the anchor table rows other writers
added; the check numbers 1-15 of owner-checks.md (the two-PC guide cites 9-15).

**Done when**
- [ ] docs/verify.md, owner-checks.md (check 16), notes/net.md, rendezvous.md, ROADMAP updated as above
- [ ] verify full --ci PASS, soak PASS, doc-check PASS; commits in both repositories (engine first)
- [ ] the report lists what the owner must run (check 16) and says the stage is **Built**, not reviewed

**Traps**: notes/net.md section 2 (the wire registry) does not change: punch datagrams are not session messages.

## Values this stage lands

None in the plan of the [LLM guide](README.md) "Values planned across the packs": no protocol, button, event,
message, entity kind, material or gate. Test count: +18 over ROADMAP's 748 (15 from the WIP, 3 here), to be read
from `ctest -N`.
