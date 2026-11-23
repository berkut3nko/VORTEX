# Rendezvous: internet sessions without a public IP (STUN, UDP hole punching, Cloudflare D1)

[[../../../00 Docs Map|Docs map]] · [[../README|docs index]] · [[../ROADMAP|roadmap]] · [[networking|networking]] · [[../notes/net|net notes]] · [[../../../docs/two-pc-test-windows|two-PC test (Windows)]]

| | |
|---|---|
| Status | R0 built (not reviewed): STUN, hashes, `Traversal`, ENet raw hooks, the NAT tools; R1 designed, nothing built. Reviewed, rechecked adversarially and amended twice. |
| Decided | - Owner's request (2026-11-19): the session server runs on the owner's own AVX2/FMA3 machine at home, with no public ("white") IPv4 address and no paid VPS. It finds players through STUN and UDP hole punching, with a Cloudflare Worker + D1 on the free tier as the meeting point.<br>- Us (this design, 2026-11-19): items 1-10 of "Decision in brief".<br>- Owner (2026-11-19): hide the token in `Hello` with M2a's protocol bump ([wp-7]); "Hello without the token" below. |
| Next | [nat-R1] rendezvous (ROADMAP "Now" since 2026-11-23, see "Stages"); the owner's R0 pair results (owner check 16) decide [nat-R2]. ROADMAP placement: "Placement" below. |
| Open | "Open questions" 1-6, each with the recommended answer. 1 (a relay for pairs that cannot meet) waits for R0's pair results. |
| History | [design-review-2026-11-19.md](../history/rendezvous/design-review-2026-11-19.md): the review's and the recheck's findings, and what was done with each |
| Marks | **(m)** measured, **(c)** computed, **(e)** estimate, **(d)** from Cloudflare's documentation read on 2026-11-19 (re-check at R1: free tiers change). Code read at engine `a23af7a`, with WP1 uncommitted in the tree. |

Related:
- [networking.md](networking.md) "Transport" (`EnetTransport`, `Hello`, the token, `net::UdpRelay`) and "Owner
  decisions" 2 (trusted testers only before M7: plain token, no encryption). This design keeps both.
- ROADMAP M7 (accounts, matchmaking): the rendezvous here is not matchmaking. It finds one known server from an
  invite, and M7 can replace it.

## The problem

The session server (`vortex_server --listen`) is reached today only by its address: on a LAN, or with a forwarded
port on a public IPv4. The owner's line has neither a public address nor a router that can forward a port (an ISP's
carrier-grade NAT, "CGNAT", or a grey address). A friend at home is usually behind a NAT of their own. Neither side
can be reached first, and no machine of ours has a public address.

What both sides do have:
- Outbound UDP. A NAT keeps a mapping (inside `ip:port` -> outside `ip:port`) open while traffic flows.
- Free public STUN servers, which tell a socket its own outside address: `stun.cloudflare.com:3478` (d) and
  `stun.l.google.com:19302`.
- A free HTTPS endpoint we can program: a Cloudflare Worker with a D1 database (SQLite). A Worker cannot send or
  receive UDP (d: outbound `connect()` is TCP only, and inbound UDP is not offered). It is only the meeting point
  where the two sides swap addresses. Game traffic never passes through Cloudflare.

## Decision in brief

1. **Three parts.**
   - STUN: each side learns its own outside address from the same UDP socket that ENet then uses.
   - Rendezvous: a Cloudflare Worker over D1 swaps the two sides' candidate addresses, keyed by the invite.
   - Hole punching: both sides send small "punch" datagrams to each other's candidates at the same time, until one
     that proves the token arrives. The client's ENet then connects to the best proven address.
2. **One socket.** STUN, punches and ENet share the ENet host's socket. NAT mappings belong to a socket, so a second
   socket would be mapped to a different outside port. ENet's `intercept` hook takes STUN responses and punch
   datagrams before ENet parses them; it is now always installed (today only for test loss). The transport also
   gets a raw send.
3. **The invite is the token.** Players already share the session token (32 hex digits).
   - The room key is `SHA-256("vortex-room-v1" || the token's 16 raw bytes)`, first 16 bytes.
   - The host key is `HMAC-SHA256(token bytes, "vortex-host-v1")`, first 16 bytes.
   - The Worker sees neither the token nor anything it can turn back into the token. It cannot link the room key to
     the host key.
4. **Every punch proves the token, for this join.**
   - Each punch datagram carries both sides' nonces and an 8-byte HMAC with the token as key. The HMAC also covers a
     hash of the client's posted candidates.
   - The client sends `Hello` only to an address that proved the token for its own join, and the server answers
     only probes that prove it.
   - This stops anyone who holds only the room key, and an impostor who registers the room while the server is
     down.
   - It does **not** hide the token from someone who can read the traffic between the two players: until M2a,
     `Hello` still carries the token in plain text, as on a LAN and with `--connect` today. From M2a's protocol bump
     on it does not ("Hello without the token").
5. **The server owns its room.** D1 stores only `SHA-256(host key)`, and every update or delete must present the key.
   - A restart with the same token keeps the room, and there is no key file to lose or commit.
   - A server started with `--new-token` gets a new room each time. Its invite changes, and that is also how the
     owner revokes an invite.
   - A room is absent once its server has not been seen for 120 s.
6. **Long poll, not fast polling.** The server waits for joins with one HTTPS request of up to 20 s; the Worker
   checks D1 once a second inside it. That is about 4 400 Worker requests a day for a server up all day (c), under
   the free 100 000 (d). Fast polling every 2 s would need 43 200.
7. **Candidates and peer-reflexive learning.**
   - Each side lists its LAN addresses, its STUN address, and (server only) an address given on the command line,
     such as a forwarded port.
   - A proven punch from an address not in the list is answered at that address. This is ICE's "peer-reflexive"
     rule. A join learns at most 2 such addresses.
   - "Who can meet" says exactly which NAT pairs this reaches.
8. **No protocol bump.** Punch datagrams sit outside ENet's format, and builds without this code drop them (see
   "Punch datagram"). `Hello`, the token check and every message stay as they are.
   - The Worker's API has its own version (`/v1/`).
   - The client reports `buildHash` to the Worker, so a client of another build is refused before punching
     ("another build", the words `Hello`'s refusal uses).
9. **The CPU floor stays as it is.** The server is a normal `vortex_server` built for x86-64 with AVX2 + FMA
   (`vortex_options`); WP1's start-up check exits with code 4 on a CPU without them. Rendezvous adds no
   instruction-set needs. SHA-256 and HMAC are plain C++, and HTTPS uses libcurl with the system's TLS.
10. **No paid fallback by default.** Some NAT pairs cannot meet by punching ("Who can meet"). R0 tests the real
   pairs that must play before anything is built for them; "Open questions" 1 lists the free options. The free
   tier never costs money (d: over-limit requests fail with errors). Nothing here may add a service that can bill.

## Who can meet

NATs differ in two ways:
- **Mapping:** whether one inside socket keeps one outside port for every destination. "EIM" (endpoint-independent
  mapping) does; "symmetric" makes a new port per destination.
- **Filtering:** what may come in through a mapping. Endpoint-independent filtering lets anyone in. Address-dependent
  filtering lets in any port of an IP we have sent to. Port-dependent filtering lets in only the exact `ip:port` we
  have sent to.

| Server side | Client side | Meets? |
|---|---|---|
| EIM, any filtering | EIM, any filtering | yes: each punches the other's STUN address, which is the real one |
| EIM, endpoint-independent or address-dependent filtering | symmetric, keeping one outside IP | yes: the client's probe arrives from a new port `b'`, the server answers it there (peer-reflexive) |
| EIM, port-dependent filtering | symmetric | **no**: the server punched `b`, so its NAT drops everything from `b'` |
| symmetric | EIM, endpoint-independent or address-dependent filtering | yes, mirrored |
| symmetric | EIM, port-dependent filtering | **no**, mirrored |
| symmetric | symmetric | **no** |
| either side with outbound UDP blocked | | **no** |
| both behind one ISP's CGNAT | | only if that CGNAT hairpins (RFC 4787 REQ-9 asks for it; not every CGNAT does) |

Filtering cannot be measured with Cloudflare's or Google's STUN servers: they lack RFC 5780's CHANGE-REQUEST. So R0
does not guess from classes. It runs a **pair test** between the real machines that must play together
(`--punch-test`, "Stages").

## Flow

```
 server (home, behind NAT)          Worker + D1 (Cloudflare)          client (friend, behind NAT)
 bind UDP :P (ENet host)
 STUN x2 from :P -> outside A:a
 POST /v1/host {room, hostKey,  -->  rooms row
   cands, build, nat}
 GET /v1/host/wait (long poll) -->   (holds up to 20 s)
                                                                    ENet host on an ephemeral port :Q
                                                                    STUN x2 from :Q -> outside B:b
                                     <-- POST /v1/join {room, cands, nonceC, build}
                                     B == CF-Connecting-IP? -> verified;
                                     joins row; reply: host cands,  -->  Probe(nonceC, 0, proof) to A:a, LAN...
                                     server's last_seen age              every 100 ms
 wait returns the join          <--
 Probe(nonceC, nonceS, proof)    ------- opens the server's NAT toward B ---------->
 to B:b, LAN... every 100 ms     <------ client's probes now pass; Ack ------------
 Ack (to the source address)     ------->  client: proven address X (waits 150 ms for a better kind)
                                            enet_host_connect(X); Hello{token}; normal session
```

- **Timeouts.** Each side punches for at most 10 s per join. Then the client reports one of three cases, with exit
  code 1:
  - The join reply showed the server's `last_seen` older than 25 s: "the server has not reached the rendezvous for
    N s (restarting or backing off)". The client waits and joins again, within the Worker's join limit, for up to
    60 s before it gives up.
  - No server probe arrived, though the server was polling: "the server did not answer the join; try again".
  - A server probe arrived but the two sides never got through to each other: "the two networks cannot meet (see
    the NAT probe)". The server logs `Punch: join <id> failed`.
- **Choosing.** After the first proven Ack the client waits 150 ms for a better kind (lan > given > stun >
  peer-reflexive). Two players at one home then connect over the LAN, not through the NAT's hairpin.
- **Keeping mappings.** The server repeats its STUN request every 15 s, and retries after 2 s without an answer.
  This keeps its mapping open with no player connected: some CGNATs drop UDP mappings after 30 s. A changed outside
  IP or port re-registers at once. ENet's pings (every 500 ms) keep a connected peer's mapping open.
- **Errors.** Every Worker call backs off on failure: 1, 2, 4 ... capped at 20 s, below a join's 60 s life, so a
  backed-off server still finds a pending join.
  - When the daily quota is spent (Cloudflare "Error 1027") the server logs it once: `Rendezvous: quota spent until
    00:00 UTC`.
  - Sessions already running are not touched. New joins then wait, or use `--connect` where an address is known.
- **Expected join overhead** (e):
  - about 0.3 s of STUN;
  - one HTTPS round trip to the nearest Cloudflare edge;
  - up to 1 s until the long poll sees the join;
  - about one game round trip of punching, plus the 150 ms choice.

  That is 1-3 s on top of today's join, or up to 20 s more while the server is backing off after an error.

## Worker API and D1 schema

Source: `engine/tools/rendezvous/` (`worker.js`, `schema.sql`, `wrangler.toml.example`, `local.mjs`, `test.mjs`).
- The owner deploys it once with Cloudflare's `wrangler` on their account ("Owner steps").
- Claude writes and tests it with Node, which is an Arch package (no downloaded packages):
  - `local.mjs` serves the real `worker.js` over `node:http`, with a D1 stand-in on `node:sqlite`, for tests and
    G20.
  - `test.mjs` runs under `node --test`.

```sql
CREATE TABLE rooms (
  room       TEXT PRIMARY KEY,      -- 32 hex: SHA-256("vortex-room-v1" || token bytes)[0..16)
  host_key   TEXT NOT NULL,         -- 64 hex: SHA-256 of the host key
  cands      TEXT NOT NULL,         -- JSON, at most 8 {"ip","port","kind"} (kind: lan | stun | given)
  nat        TEXT NOT NULL,         -- "eim" | "symmetric" | "unknown"
  build      TEXT NOT NULL,         -- buildHash, 16 hex
  protocol   INTEGER NOT NULL,
  last_seen  INTEGER NOT NULL       -- unix seconds
);
CREATE TABLE joins (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  room       TEXT NOT NULL,
  cands      TEXT NOT NULL,         -- the client's string as posted (lan, at most 2 stun), returned byte for byte
  nonce      TEXT NOT NULL,         -- 16 hex, the client's punch nonce
  verified   INTEGER NOT NULL,      -- 1 when every stun candidate's IP is the caller's IPv4
  created    INTEGER NOT NULL,
  UNIQUE (room, nonce)
);
CREATE INDEX joins_by_room ON joins (room, id);
```

| Call | Who | Does |
|---|---|---|
| `POST /v1/host` `{room, cands, nat, build, protocol}`, header `Authorization: Bearer <hostKey>` | server, at start and when its outside address changes | Creates the room, or updates it if `SHA-256(hostKey)` matches. A live room with another key is refused (409). |
| `GET /v1/host/wait?room&after=<id>`, the same header | server, again and again | Returns joins with `id > after` as soon as there are any, else after 20 s with none. Refreshes `last_seen`. At most 21 D1 queries (under the 50 subrequests a request may make, d). |
| `POST /v1/join` `{room, cands, nonce, build, protocol}` | client | 404 for an unknown or absent room. 409 "another build" on a `build` mismatch, and 409 for a nonce already used in the room. 403 when an IPv4 caller's `stun` candidate is another IP; the body names the IP the Worker saw. 429 after 4 joins in a minute for the room. Otherwise inserts the join and returns the server's `cands`, `nat` and the age of its `last_seen`. |
| `DELETE /v1/host` `{room}`, the same header | server, on a clean exit | Deletes the room and its joins. |

- **Validation.** Bodies are at most 2 KiB. Addresses are IPv4 literals, ports 1-65535, and fields have a fixed
  length. A join may list only `lan` and `stun` candidates, at most 2 `stun`, and its `lan` candidates must be
  private (RFC 1918) addresses.
- **IPv4 for HTTPS.** The HTTPS client resolves IPv4 only (`CURL_IPRESOLVE_V4`) and ignores proxy settings
  (`CURLOPT_NOPROXY "*"`). The Worker then sees the same outside IPv4 that STUN reports. A VPN or a second outside
  address shows up as 403, and the client prints both IPs.
- **IPv6 callers.** A caller seen over IPv6 anyway is stored with `verified = 0`, not refused. The server sends at
  most 10 probes to an unverified join's `stun` candidates: enough to open its own filter, too few to aim at
  anyone. The reply tells the client, which logs `Rendezvous: the Worker saw IPv6; the HTTPS client should force
  IPv4`.
- **Joins.** A room keeps at most 16; the oldest are deleted on insert, and joins older than 60 s lazily.
- **Expired rooms.** Every call treats a room not seen for 120 s as absent. A cron trigger once an hour deletes such
  rooms and their joins (1 of the free 5 cron triggers, d).

### Free-tier budget (one server up 24 h, 50 joins a day; c, against d)

| | per day | free limit (d) |
|---|---|---|
| Worker requests | 4 320 waits + 50 joins + a few registrations ~ 4 400 | 100 000 |
| D1 rows read | 4 320 waits x ~21 indexed lookups ~ 91 000 | 5 000 000 |
| D1 rows written | 4 320 `last_seen` + 50 joins x 3 (two index rows count) + deletes ~ 4 500 | 100 000 |
| D1 storage | under 1 MB | 5 GB |
| Worker CPU per request | one JSON parse and up to 21 tiny queries: well under 10 ms (e; checked with `wrangler tail` at R1) | 10 ms |

That leaves room for about 20 servers up all day, or many more for short sessions.

## The engine side

- **`Vortex::Net`** (no GPU, no window, no HTTPS) gets the logic, testable without a network:
  - **`net::Stun`**, an RFC 5389 Binding request with a 96-bit random transaction id. It parses
    `XOR-MAPPED-ADDRESS`, falls back to `MAPPED-ADDRESS`, and checks the magic cookie `0x2112A442` and the
    transaction id. It is tested with RFC 5769's IPv4 response vector.
  - **The NAT class.** STUN goes to two servers on different IPs from the same socket. The same outside IP and port
    from both gives `eim`. A different IP or port gives `symmetric`. No answer from either gives `unknown` (UDP
    blocked, or the servers are unreachable). This is reported and logged; nothing decides on it, because the pair
    test does.
  - **`net::Sha256` and HMAC-SHA256** (about 150 lines, FIPS 180-4 and RFC 4231 vectors). They serve the room key,
    the host key and the punch proof only, never anything that decides voxels.
  - **`net::Traversal`**, a pure state machine. Input: our candidates, the peer's, the room tag, the token, both
    nonces, the candidate hash and the clock. Output: datagrams to send, the chosen address, or one of the failure
    cases of "Flow". It never touches a socket. A join comes from the Worker or, in R0's pair test, from a pasted
    line.
  - **`net::Rendezvous`**: the room and host keys, registering, the long poll with backoff, re-registering when the
    outside address changes, deleting the room on exit, and handing joins to `Traversal`. It talks to an abstract
    `net::HttpsClient`, and tests give it a fake.
  - **`net::NatSim`** (in tests): NATs in memory with mapping (EIM / symmetric), filtering (endpoint-independent /
    address / port), pooling (one IP / any of several), hairpinning and mapping timeouts.
- **`EnetTransport` additions:**
  - `Open(bindAddress, port)` makes the host without connecting.
  - `SendRaw(address, bytes)` is `enet_socket_send` on the host's socket.
  - `TakeRaw()` returns the STUN and punch datagrams that `intercept` took.
  - `ConnectTo(address)` starts the ENet connection. `Connect` = `Open` + `ConnectTo`; `Listen` is unchanged.
  - `intercept` is always installed and keeps the test loss. It takes a datagram with the STUN cookie at offset 4
    and a transaction id we sent, or one with the punch magic. A client host also drops every ENet datagram not from
    its chosen server, so no stranger can take its one peer slot while it punches.
  - On Windows the socket gets `SIO_UDP_CONNRESET` off. Otherwise one ICMP "port unreachable" from punching a dead
    LAN candidate ends a receive loop early.
  - STUN, punching and `ConnectTo` run on the thread that owns the transport, before the game's keep-alive thread
    starts.
  - LAN candidates come from `getifaddrs` / `GetAdaptersAddresses`. Left out: loopback, link-local, 100.64.0.0/10
    (Tailscale and CGNAT addresses), docker or virtual adapters, and VPN and tunnel adapters by name (`IsLanInterface`:
    tun, tap, wg, utun, vnet, tap-windows, vpn, openvpn, wireguard, npcap, ...). At most 4 per side, in interface
    order, so `lan` never crowds `given` and `stun` out of the 8 (`kMaxCandidates`; [nat-R0] decision 3). The rule
    belongs to the candidate list itself, not to the pair test: R0 applies it in `PunchTest::MakeLine`, and R1's
    `ChooseCandidates` (which takes over that body for the line, the register and the join) keeps it.
  - `--candidates stun,given` limits the kinds offered (G20 uses it). The server punches a client's `lan`
    candidates only when they lie in one of its own subnets.
- **`vortex_https`** is a small library of its own, linked only by `vortex_server` and the game (as `Enet.cpp` is the
  only file that sees ENet). It implements `net::HttpsClient` with libcurl on both platforms.
  - Linux: the system's libcurl (`find_package(CURL)`; the `curl` package is installed on Arch with pacman).
  - Windows: MSYS2 CLANG64's `mingw-w64-clang-x86_64-curl`, with Schannel, Windows' own TLS. The game already uses
    that prefix's runtime.
  - Neither is fetched. System curl is a recorded exception to "dependencies are pinned by tag": it is the system's
    TLS.
  - Every request sets `CURL_IPRESOLVE_V4` and `CURLOPT_NOPROXY "*"`. Certificate checks stay on, and
    `curl_global_init` runs once on the main thread.
  - **Two threads.** One owns only the long poll. Its transfer aborts when `HttpsClient::Cancel(id)` sets a flag
    that its progress callback reads. The other runs the short calls (register, join, delete) and the STUN name
    lookups.
  - A request in flight never holds up another, and the server's tick and the client's frame only poll for results.
    This applies the lesson of stage 8's join and leave stalls.
  - On a clean exit: cancel the wait, send the DELETE with a 2 s limit (log and go on if it times out), then join
    both threads.

### Punch datagram

| Bytes | Field |
|---|---|
| 0-3 | magic `"VXPN"` (0x56 0x58 0x50 0x4E) |
| 4 | version 1 |
| 5 | kind: 1 Probe, 2 Ack |
| 6 | sender: `'S'` server, `'C'` client |
| 7-14 | room tag: the room key's first 8 bytes |
| 15-22 | client nonce (from the join) |
| 23-30 | server nonce (per join; 0 in a client's Probe before it has heard the server) |
| 31-38 | proof: `HMAC-SHA256(token bytes, "vxpn1" \|\| bytes 4-30 \|\| candHash)`, first 8 bytes |

`candHash` is the first 8 bytes of SHA-256 over the client's `cands` string, exactly as posted. The Worker returns it
byte for byte. In R0's pair test it is the pasted line's candidate part.

- **Size.** 39 bytes in both directions (67 on the wire with IP and UDP headers). An Ack is never larger than the
  Probe it answers, and an Ack is never answered.
- **What the client checks.** It takes an `'S'` datagram only if all of these hold:
  - it carries the client's own nonce and a non-zero server nonce;
  - the proof is valid for its own `candHash`;
  - after the first accepted one, the server nonce is the same.

  Its nonce is fresh per join, so an old proof cannot be replayed. A join that copies its nonce is refused by the
  Worker (409), and the proof would not match the copy's candidates anyway.
- **What the server checks.** It takes a `'C'` datagram only if all of these hold:
  - the proof is valid;
  - the join came from the Worker in the last 15 s;
  - the server nonce is 0 or its own.

  It drops a second join with a client nonce it already holds. Anything else is dropped silently.
- **What each side may send.** At most 100 datagrams per join across all of its candidates (6.7 KB on the wire), and
  at most 10 probes to an unverified join's `stun` candidates (its Acks answer proven datagrams and count only toward
  the 100). The server runs at most 4 joins at once, and the Worker
  admits at most 4 joins per minute per room.
  - A verified join's STUN IP is the caller's own (the Worker's 403 rule).
  - LAN candidates are private, and in one of the server's own subnets.

  So a forged join cannot aim the server at a stranger on the internet.
- **Why ENet never takes one.**
  - Read as an ENet header, `"VX"` (0x5658) is peer id 0x658 (1624), with the compressed flag set. ENet drops it
    at `peerID >= host->peerCount`, before any compression or checksum code.
  - A STUN reply's first word, 0x0101 (peer id 257), is dropped the same way.
  - Our `intercept` takes both first anyway. A test pins this: such a datagram, handed to plain ENet, changes no
    peer.

### Command lines

| Program | Flags |
|---|---|
| `vortex_server` | `--rendezvous URL` (with `--listen PORT --token HEX`: register and punch); `--public ADDR:PORT` (an extra `given` candidate); `--stun HOST:PORT` (repeatable; replaces the two default servers); `--candidates KINDS`; `--nat-probe` (R0: print the outside address and the NAT class, exit 0); `--punch-test --token HEX` (R0's pair test, below; the server's side) |
| `Shredborne` | `--join TOKEN --rendezvous URL` (instead of `--connect HOST:PORT --token TOKEN`, which stays); `--stun`, `--candidates`, `--nat-probe`; `--punch-test --token HEX` (the client's side) |

The URL can also come from a one-line file, `rendezvous.url`, next to the executable. Testers then need only the
token. Log lines (`docs/verify.md` gets them at R0 / R1):
- `NAT: outside A:a, class eim|symmetric|unknown`
- `Rendezvous: room <first 8 hex> registered, N candidates`
- `Punch: join <id> reached <addr> (<kind>) in X ms`
- `Punch: join <id> failed after 10 s`

## Security (trusted testers, owner decision 2)

- **The token never reaches the Worker or D1.** The room key and the host key are one-way functions of it. The
  token crosses the wire only inside `Hello`, in plain text, and only to an address that proved it holds the token
  for this join.
- **What a party with only the room key gets:** Cloudflare, anyone who reads D1, and anyone who saw the room tag in
  a punch.
  - They learn the server's addresses.
  - They can stand up a fake room while the server is down.
  - They cannot get the token, a proof, or a place in the game.
- **What an on-path observer gets.** Someone who can read the traffic between the two players (their Wi-Fi, either
  ISP) reads the token from `Hello`. That gives lasting access to the room until the owner restarts with
  `--new-token`. This is today's risk on a LAN and with `--connect`, accepted by owner decision 2 until M7. "Open
  questions" 7 holds the cheap fix for when the protocol is next opened.
- **Addresses are shared by design.** The server's outside and LAN addresses go to everyone who asks with the room
  key. That is how direct play works, and the LAN addresses are private ranges.
- **Hijacking a room** needs the host key, which only token holders can derive. D1 holds only its hash.
- **Spending the free tier.** Anyone who learns the Worker URL can send it requests. That costs no money: over the
  limit the Worker returns errors until 00:00 UTC (d). Unknown rooms cost one indexed read. The URL is shared only
  with testers, and a leaked one is fixed by redeploying under a new name. Rate limits wait for M7.
- **Reflection.** See "What each side may send". STUN goes only to the configured servers, and HTTPS checks
  certificates. The host key travels in a header, never in a URL.

## Tests

| Test (suite VortexNetTests unless noted) | Pins |
|---|---|
| `Stun.ParsesTheRfc5769IPv4Response`, `Stun.RefusesAWrongTransactionOrCookie` | the STUN codec |
| `Sha256.MatchesFips180Vectors`, `Sha256.HmacMatchesRfc4231Vectors`, `Rendezvous.KeysOfAKnownToken` | the hashes, and the exact inputs of the room and host keys |
| `Traversal.TheMeetTableHolds` | every row of "Who can meet" over `NatSim`, "no" rows included (each with the right failure case) |
| `Traversal.SameLanPrefersTheLanCandidate` | the 150 ms choice; hairpinning on and off |
| `Traversal.AnImpostorWithTheRoomKeyGetsNoHello` | the proof: a fake server's Probe and Ack are dropped |
| `Traversal.AJoinThatCopiesAnotherJoinsNonceProvesNothingToIt` | `candHash` in the proof |
| `Traversal.AnswersOnlyProvenLiveJoins`, `Traversal.CapsWhatAJoinMakesEachSideSend`, `Traversal.AcksAreNeverAnswered` | the caps, the unverified cap of 10, the 4-join limit, no Ack ping-pong |
| `Traversal.PastesMinutesApartStillMeet` | R0's pair test with port-dependent filtering on both sides and the pastes 3 minutes apart: reached |
| `Enet.APunchOrStunDatagramNeverReachesENet` | plain ENet drops both |
| `Enet.AClientHostTakesNoStrangerWhilePunching` | the client host's filter |
| `Rendezvous.RegistersWaitsAndDeletes`, `Rendezvous.ReRegistersWhenTheOutsideAddressChanges`, `Rendezvous.BacksOffAndLogsTheSpentQuotaOnce`, `Rendezvous.ARestartWithTheSameTokenKeepsTheRoom`, `Rendezvous.AnotherBuildIsRefusedBeforePunching`, `Rendezvous.ReRegisterAndDeleteDoNotWaitForTheLongPoll`, `Rendezvous.AStaleServerIsReportedNotBlamedOnTheNat` | the client of the Worker, over a fake `HttpsClient` |
| `node --test engine/tools/rendezvous/test.mjs` | the Worker: room ownership, absence after 120 s, the join caps, a reused nonce, the 403 IP rule and its body, an IPv6 caller stored unverified, two `stun` candidates at most, validation; at most 21 D1 queries per wait |

**Gate G20** (added to CLAUDE.md at R1).

`node engine/tools/rendezvous/local.mjs` serves the real Worker. `vortex_natbox`, a test tool linking Net, puts each
program behind an emulated NAT and runs a fake STUN server. The server (`--bind 127.0.0.1`, as in G15) and a game
client with `--join` both run with `--candidates stun,given`, so no real interface address can short-cut the NAT.

**How `vortex_natbox` works.** It is a two-sided proxy:
- **Inside.** For every remote endpoint R the program may address, the box gives the program an alias socket A(R) on
  127.0.0.1. The program only ever sends to aliases.
  - The rendezvous answers each side with its own box's aliases of the peer's candidates.
  - `--stun` names the alias of the fake STUN server.
- **Outbound.** A datagram to A(R) passes the box's mapping rule. That rule picks or makes an outside socket on an
  outside IP from the pool: 127.0.0.2, 127.0.0.3 and so on. All of 127.0.0.0/8 is local on Linux, so pooling needs
  no setup. The datagram leaves from that socket toward R.
- **Inbound.** A datagram arriving at an outside socket from O passes the filtering rule. It is then delivered to the
  program from A(O), made on demand. So peer-reflexive answers, Acks and ENet's replies cross the same mapping.

**Three pairs:**
- EIM with endpoint-independent filtering against symmetric: must log `Punch: ... reached` with kind
  `peer-reflexive`, then G15's hash lines.
- EIM with port-dependent filtering against EIM: must log `Punch: ... reached` with kind `stun`, then the hash lines.
- EIM with port-dependent filtering against symmetric: must log `Punch: ... failed after 10 s` and exit 1.

This proves the real ENet path and the shipped Worker through emulated NATs. Only the owner's runs prove real ones.

**Owner checks** (added to `docs/owner-checks.md` at R0 / R1):
- R0's pair tests;
- the Windows client built in MSYS2 CLANG64 at each stage's commits;
- one internet session with a tester on another network, reporting the join time and the game's F3 round trip.

## Stages

| ID | Builds | Effort (e) | Done when |
|---|---|---|---|
| [nat-R0] | `net::Stun`, the NAT class, `Sha256` / HMAC, `Traversal` + `NatSim` with every test above that needs no Worker; `EnetTransport::Open / SendRaw / TakeRaw / ConnectTo`, the always-on `intercept`, the client host's filter, `SIO_UDP_CONNRESET`; `--stun`, `--candidates`, `--nat-probe`, `--punch-test` | 3-4 d | Tests pass. The owner runs the pair test (below) for each pair that must play, one side on Windows, and reports the `NAT:` and `Punch:` lines. Those results decide "Open questions" 1. Built: on `main` after engine `27052ab` / game `a91f730` (2026-11-23; the commit hashes follow when the owner commits); the owner's pair results pending (owner check 16). |
| [nat-R1] | The Worker + D1 (`local.mjs`, Node tests), `vortex_https` (libcurl on both platforms), `Rendezvous`, `--rendezvous` / `--join`, `vortex_natbox` with the fake STUN server, G20, the owner checks | 6-8 d, plus 0.5-1 d for the Windows build round trip | Every gate plus G20. The owner deploys the Worker and plays one internet session, with the Windows client as the joining side. |
| [nat-R2] | Only for pairs that R0 found cannot meet: the relay chosen in "Open questions" 1 | 3-6 d | Those pairs play. |

**R0's pair test, with no Worker.**
1. The two roles are fixed by the program: `vortex_server --punch-test` is the server side, and `Shredborne
   --punch-test` is the client side. Both take the same `--token`.
2. Each prints one line to copy: its candidates, plus, on the client side, the client nonce it made at start.
3. The owner pastes each line into the other running program (it reads stdin). Nothing restarts, and STUN every 15 s
   keeps each mapping open while it waits.
4. A pasted line stands in for the Worker's join. From its own paste, each side probes 10 times a second for 10 s,
   then once a second, and answers proven probes, until it has reached the peer or 5 minutes have passed.
5. It logs `reached` or `failed`, with the gap between the two pastes.

Example pairs: the home line against the Windows laptop on a phone hotspot, and against a tester's home line.

**Placement** (us, under the owner's rules; the owner may veto). R0 and R1 go into ROADMAP "Next" right after WP2,
before WP4. Reasons:
- With no wire change they fit anywhere.
- R0 tells us, on the real lines, whether punching is enough.
- Internet play turns every later stage, M2a's first fight above all, into something testers outside the house can
  try.

R2 waits for R0's pair results.

## Owner steps

**Once, about 15 minutes, at R1:**
1. Make a free Cloudflare account. The Workers free plan should need no card (e: check this at sign-up; if it asks
   for one, stop and tell us).
2. `npm install -g wrangler` and `wrangler login`. This is the owner's own machine and their choice: Claude never
   runs downloaded binaries.
3. `wrangler d1 create vortex-rendezvous`, then paste the printed id into `wrangler.toml`.
4. `wrangler d1 execute vortex-rendezvous --remote --file schema.sql`.
5. `wrangler deploy`. Write the printed `https://....workers.dev` URL into `rendezvous.url` next to both programs.

**Windows:** `pacman -S mingw-w64-clang-x86_64-curl` in MSYS2 CLANG64.

**For a lasting invite:** the live server keeps one token (`--token`) instead of `--new-token`. Restarting with
`--new-token` revokes the old invite.

**Only if needed:** `*.workers.dev` is blocked on some networks. A tester who cannot reach it needs the Worker on a
custom domain; Cloudflare's free plan allows that, but the domain must be the owner's.

## Rejected alternatives

- **A STUN or relay server on Cloudflare.** Workers have no UDP (d).
- **Fast polling (every 2 s).** It needs 43 200 requests a day for one server, against the long poll's 4 400.
- **Durable Objects with WebSockets.** They are free on the SQLite backend since 2025 (d) and would push joins with
  no wait. The owner asked for D1, and the long poll gives about 1 s of delay well inside the free tier. Kept as the
  upgrade if 1 s ever matters.
- **Cloudflare KV.** Its free tier allows 1 000 writes a day (d), less than one server's `last_seen` updates.
- **A second socket for STUN or punching.** It gets another NAT mapping, so the address it learns is not the one
  ENet uses.
- **Deciding from NAT classes alone.** Public STUN servers cannot measure filtering, and the class misses the cases
  that matter (see "Who can meet"). R0's pair test decides instead.
- **WinHTTP on Windows.** It cannot be told to use IPv4 only, so it would break the 403 rule, and it would be a
  second HTTPS code path that no gate runs.
- **A host key kept in a file.** Deriving it from the token leaves no secret on disk.
- **ICE-style transaction ids and address binding in the proof.** An on-path observer already reads `Hello`, so they
  buy nothing until `Hello` hides the token. Binding the proof to the join (`candHash`) stops the off-path attacks.
- **A relay of our own as the default.** It needs a public address, which is exactly what the owner lacks.
- **UPnP / NAT-PMP / PCP port mapping.** It does not help behind a carrier's NAT, which is the owner's case, and it
  needs another library. Reconsider only if R0 shows the home router has a public address.
- **Port prediction for symmetric NATs.** It is unreliable and sprays many datagrams. Not built.
- **Off-the-shelf VPNs (Tailscale, ZeroTier).** They work today with `--connect` and no code at all, for testers
  willing to install them. They are a good stopgap before R1, but every player would need an account and a client,
  so they are not the game's way in.

## Open questions (with the recommended answer)

1. **Pairs that cannot meet (R2): which relay?** Recommended: decide from R0's pair results, and build nothing if
   every pair that must play meets. Otherwise, in this order:
   - (a) IPv6, where both ends have it. This needs an IPv6-capable ENet (a fork, a dependency change, owner OK).
   - (b) Cloudflare Realtime TURN. Its first 1 000 GB a month are free (d), shared with Cloudflare's SFU; then
     $0.05/GB. Only if the owner confirms that the account cannot be billed without a card, and with a monthly byte
     cap enforced by the Worker that issues the TURN credentials.
   - (c) That tester uses Tailscale or ZeroTier.
2. **The invite format.** Recommended: the 32 hex digits of today's token. A shorter word code would need a token
   format change (owner decision 2 keeps the token).
3. **IPv6 candidates.** Recommended: later, with question 1(a). ENet 1.3.18 is IPv4 only.
4. **More than one server per token.** Recommended: no. One room per token, and `--new-token` makes a new room.
5. **The Worker URL compiled into the game.** Recommended: no. Use `rendezvous.url` or `--rendezvous`, so forks and
   tests point elsewhere.
6. **Showing the invite in game (HUD).** Recommended: after R1, with the session UI. The command line is enough for
   R1's testers.
7. **Hiding the token in `Hello`.** After a punch both sides hold both nonces, so `Hello` could carry
   `HMAC(token, "vxhello1" || nonceC || nonceS)` instead of the token. `--connect` would need a one-round-trip
   server challenge. This is a wire change, so it bumps `kProtocolVersion`, and it touches owner decision 2.
   Recommended: ride along with M2a's protocol bump ([wp-7]) if the owner agrees, else wait for M7. The owner
   decides.
