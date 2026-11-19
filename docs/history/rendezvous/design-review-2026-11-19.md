# Review of the rendezvous design (one reviewer, 2026-11-19)

The design was reviewed read-only against ENet 1.3.18's source, the engine's `Enet.cpp` / `UdpRelay`, CLAUDE.md and
Cloudflare's documentation. Findings, verbatim in substance, and what was done with each. The amended design is
[../../design/rendezvous.md](../../design/rendezvous.md).

## Severity 1

1. **Wrong pairing rule.** The design said every pair where at least one side has an endpoint-independent mapping
   (EIM) meets. But EIM with port-dependent filtering against a symmetric NAT fails. The symmetric side's mapping
   toward the server has a new port `b'`, while the server punched only the port STUN reported, `b`. The server's
   NAT therefore drops every probe from `b'`, and peer-reflexive learning never starts. This is common: CGNATs often
   filter by port, and phone hotspots are often symmetric. R0 cannot measure filtering with Cloudflare's or Google's
   STUN servers (no RFC 5780 CHANGE-REQUEST).
   **Done:**
   - "Who can meet" now states the real rule.
   - `NatSim` models filtering and IP pooling.
   - R0 gains a pair test between two real machines (`--punch-test`), and R2 is decided from pair results.
2. **Token theft by an impostor.** Several parties can hold the room key: Cloudflare, anyone who saw the room tag on
   the wire, and former testers. Any of them can register the room while the server is down, or forge an Ack, and
   then receive the client's `Hello{token}`. Also, "the client answers only Probes from the server's nonce" could
   not work: the server's nonce never reaches the client before its first probe.
   **Done:**
   - Every punch datagram carries both nonces and a proof, `HMAC-SHA256(token, ...)` truncated to 8 bytes.
   - The client sends `Hello` only to an address that proved it holds the token, and the server answers only proven
     probes.
3. **G20 could not run as written.** The STUN servers were hard-coded (real hosts in a gate). `UdpRelay` forwards to
   one fixed server and drops traffic from any address but its first client, so it is not a NAT.
   **Done:**
   - Added `--stun HOST:PORT` and an in-process fake STUN server.
   - Added a real NAT emulator, `net::NatBox`: an outside socket per mapping, with mapping, filtering and pooling
     settings.
   - G20 runs three pairs, including the failing one.

## Severity 2

4. **Forged joins reflect traffic.** The "3 KB" figure left out the 28 bytes of IP/UDP header on each datagram. The
   cap's scope (per join or per candidate) was unclear, and 4 joins could be re-armed every 15 s.
   **Done:**
   - The Worker refuses a join whose STUN IP differs from `CF-Connecting-IP`. HTTPS is forced to IPv4, so both see
     the same address.
   - The cap is 100 datagrams per join across all of its candidates (39 bytes each, 6.7 KB on the wire).
   - At most 4 joins a minute per room.
5. **The LAN preference contradicted "first Ack wins".** **Done:** after the first proven Ack the client waits
   150 ms for a better kind of candidate. Both players behind one ISP's CGNAT connect only if that CGNAT supports
   hairpinning; this is now stated.
6. **ENet details.**
   - `intercept` was installed only for test loss. **Done:** it is now always installed.
   - Resolving the STUN host names blocks. **Done:** it runs on the HTTPS thread.
   - A client's 1-slot host accepts incoming CONNECTs. **Done:** a client host drops every ENet datagram not from
     its chosen server.
   - On Windows, `WSAECONNRESET` cuts the receive loop short. **Done:** `SIO_UDP_CONNRESET` is turned off.
7. **A restarted server was locked out for 120 s** (a new host key meets 409). **Done:** the host key is kept in a
   file, `rendezvous-host.key`, beside the session.
8. **No backoff.** **Done:** capped exponential backoff, and a once-only log line when the daily quota is spent.

## Severity 3 (all done)

- Keepalive: STUN every 15 s, with a retry after 2 s; the NAT class compares the IP as well as the port.
- The 60 s `/v1/host` refresh was redundant with the long poll: dropped. The budget table was recomputed.
- A join insert writes 2 rows (the index row counts).
- Layering: HTTPS lives in its own library, `vortex_https`, linked only by `vortex_server` and the game.
  `Vortex::Net` and its tests never see curl. System curl is a recorded exception to "pinned by tag".
  `curl_global_init` runs on the main thread.
- The room key's input is pinned: the 16 raw token bytes.
- LAN candidates come from `getifaddrs` / `GetAdaptersAddresses`, leaving out loopback, link-local, 100.64/10
  (Tailscale, CGNAT) and docker / virtual adapters.
- workers.dev is blocked on some networks: a custom domain (free) is the fix, noted for the owner.
- The join estimate now includes backoff after an error.

## Confirmed by the reviewer (no change)

- **ENet:** `intercept` runs after `enet_socket_receive` has set `receivedAddress` / `receivedData`, and before the
  command parser; returning 1 consumes the datagram.
  - `"VX"` (0x5658) is dropped at `peerID >= host->peerCount`, before the compression or checksum code. A STUN
    reply's first word, 0x0101 (peer id 257), is dropped the same way.
  - A raw `enet_socket_send` on the host's socket from the main thread is safe.
  - `ConnectTo` on the Ack address works through the same mapping.
  - ENet's CONNECT retries back off (500 ms, then doubling), so they make poor punches; a separate punch phase is
    right.
- **Cloudflare:** the limits, and that a 20 s long poll with `setTimeout` is sound on the free plan. The arithmetic
  is right.
- **Protocol:** no bump is needed, and checking `buildHash` at the Worker is a good choice.

# Adversarial recheck of the amended design (workflow `rendezvous-recheck`, 2026-11-19)

Two critics (NAT and security; engine and operations) read the amended design, and a skeptic tried to refute each
finding. 11 findings were confirmed and 3 rejected. Where the skeptic offered a better fix, that fix was applied.

## Confirmed and done

1. **The token travels in plain text in `Hello`** (S1). "The token never leaves the machines that hold it" was
   false: after a punch, `Hello` crosses the internet in plain text.
   **Done:** the wording was corrected (Decision 4, Security). The proof stops off-path impostors and fake rooms. An
   on-path observer reads the token, as on a LAN today (owner decision 2). `--new-token` is named as the way to
   revoke an invite. Open question 7 records the HMAC'd `Hello` for the next protocol bump; the owner decides.
2. **Proofs were not tied to the join** (S2). Replayed probes could drive peer-reflexive learning past the 403 rule.
   A copied nonce let a second join get valid `'S'` probes.
   **Done:**
   - `candHash` (over the client's posted candidates) is part of the proof.
   - The Worker has `UNIQUE(room, nonce)` (409), and the server drops a second join with a known nonce.
   - The client keeps the first server nonce it accepts.
   - Acks are never answered, the client has the same 100-datagram cap, and a join learns at most 2 addresses.
   - Test added: `Traversal.AJoinThatCopiesAnotherJoinsNonceProvesNothingToIt`.
   - ICE transaction ids were rejected, because an on-path observer already reads `Hello`.
3. **The 403 rule against IPv6 and Windows** (S2). WinHTTP cannot force IPv4.
   **Done:**
   - An IPv6 caller is stored unverified, and an unverified join gets at most 10 datagrams.
   - A join may list at most 2 `stun` candidates, and only the kinds `lan` and `stun`.
   - The server punches a client's `lan` candidates only inside its own subnets.
   - `CURLOPT_NOPROXY "*"` is set, and the 403 body names the IP the Worker saw.
4. **The host key file** (S2) sat next to checked-in session files, and the key was sent in a URL.
   **Done:** the host key is derived from the token, `HMAC-SHA256(token, "vortex-host-v1")`, and sent in an
   `Authorization: Bearer` header. No file.
5. **R0's pair test could not produce valid results** (S2, reported by both critics): the peer was passed on the
   command line, the roles were undefined, and the 10 s windows need not overlap.
   **Done:**
   - The roles are fixed by the program, and the peer's line is read from stdin.
   - A pasted line stands in for the Worker's join.
   - Each side probes until it reaches the peer or 5 minutes pass.
   - Test added: `Traversal.PastesMinutesApartStillMeet`.
6. **`NatBox` could not sit in the programs' outbound path** (S1).
   **Done:** `vortex_natbox` is a two-sided proxy, with inside alias sockets and an outside IP pool on 127.0.0.0/8.
7. **G20's "must fail" pair would pass over a real LAN candidate** (S2).
   **Done:** `--candidates stun,given`, `--bind 127.0.0.1`, and G20 asserts the chosen kind.
8. **WinHTTP cannot force IPv4** (S2).
   **Done:** libcurl on both platforms (MSYS2 CLANG64 curl with Schannel), with `CURL_IPRESOLVE_V4`.
9. **One blocking HTTPS thread queued re-registration and DELETE behind the 20 s long poll** (S2).
   **Done:** two threads, a cancellable wait, a DELETE capped at 2 s on exit.
   Test added: `Rendezvous.ReRegisterAndDeleteDoNotWaitForTheLongPoll`.
10. **The client's 10 s limit misreported a server that was backing off** (S3).
    **Done:**
    - The join reply carries the age of `last_seen`, and the client reports three cases.
    - The backoff is capped at 20 s, below a join's 60 s life.
    - Test added: `Rendezvous.AStaleServerIsReportedNotBlamedOnTheNat`.
11. **The Windows pieces had no gate, and R1's estimate was short** (S3).
    **Done:** Windows owner checks at R0 and R1; R1 is now 6-8 d plus 0.5-1 d; the Worker is tested through
    `local.mjs` (the real `worker.js` on `node:http` + `node:sqlite`) instead of a C++ fake.

## Rejected by the skeptics

- **"Anyone with the room key can lock the real server out of its room."** The host key is needed to hold a room,
  and the room key is not visible on the path (only its first 8 bytes, as the room tag). The parties who hold the
  room key are the trusted testers.
- **"The 100-datagram cap runs out before the 10 s window."** 10 s is an upper bound. The cap ends punching early
  only for joins that are failing anyway.
- **"Everything does not run on the main thread for the game client."** The keep-alive thread starts only after
  `Welcome`, after punching. The sentence was narrowed to "the thread that owns the transport".
