# Networking history: the owner's two-machine run (2026-11-17)

Moved verbatim from `docs/design/networking.md` lines 2341-2383 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

### Owner's two-machine run (2026-11-17)

The owner ran the stage 5 acceptance over the home LAN at engine `9cbfdaa` / game `7082a1c` (protocol 10).
- **Setup:** `vortex_server` with `playground.session --listen 27015 --new-token` on the Arch laptop (UHD 620), which
  also played with `--connect 127.0.0.1`. The second device was the **Windows laptop** (Intel Core Ultra 7 356H),
  which joined over Wi-Fi. This was the first Windows build of the game and the first Windows client against a Linux
  server. The owner has not yet reported the toolchain, any source changes, or whether the golden tests were run.
- **Terrain agreement across platforms:** the server log shows both joins as ordinary chunk-sync joins (50
  `ChunkData` / 75 KiB, 12 baseline entities, 0 requests). A client whose probe chunks differ would have started
  server terrain mode, which is about 16 MiB on the playground. So the Windows build's 64 probe chunks matched the
  Linux server's. That is the first evidence from a run that the generator agrees across the two platforms. The golden
  float-bit tests on Windows remain the release gate ("Known risks of terrain agreement after stage 0").
- **Server log:** 18 casts from players, 50 -> 65 modified chunks, 0 backlog, bulk work at most 1.1 ms per tick.
- **Stalls in the owner's server logs (for stage 8).** A second run of the owner's server the same evening logged three
  long passes:
  - 124.9 ms when client 2 joined into slot 0;
  - **207.4 ms when that client left**;
  - 120.9 ms when client 5 (a headless Linux client) joined into slot 0, while the Windows player was elsewhere.

  Client 3's join into slot 1, while client 2 stood at the spawn, logged no stall. Causes, read from the code:
  1. **Join.** `Server.cpp` cooks the spawn's terrain colliders synchronously in the join tick
     (`Terrain().Update(PlayerFocus())` then `Terrain().Flush()`; the comment explains why the player is not gated).
     When no player stands near the spawn, the colliders there were dropped, and cooking them costs about 120 ms.
     Suggested fix: keep every spawn point in the server's collider focus permanently (cooked once at start). The join's
     `Flush` then has nothing to cook, and the no-gate rule stays.
  2. **Leave.** `vortex_server`'s `SetOnClientLeft` logger hashes the whole region (13 790 chunks) and then the
     client's sync box, which for a playground client is the same box, inside the tick (`tools/server/main.cpp`).
     Suggested fix: hash once when the boxes are equal, and cache `ChunkHash` per chunk by `ChunkRevision` (HashCheck
     would use the same cache), or take the hash off the tick.

  A 120-200 ms stall delays every connected player's snapshots and inputs (about 7-12 ticks). Stage 8's soak should
  show both in its tick p99 / max and assert they are gone.

The same session on 2026-11-17: the owner reports the world renders **pixel-identical on Linux and on Windows**. A
headless Linux client joined the owner's live server while the Windows player was connected. It saw that player, had
3 casts run, and at opSeq 92 its world hash (`eea99b31...`) and entity id hash equal the server's 'client left'
lines; it ran 10 hash checks with 0 mismatches, 3 corrections and 27 ms RTT.
- **Owner's verdict:** "every things works fine, seamless experience".
- **Screenshot (host client, windowed):** `ONLINE ... slot 0`, `ping 24 ms`, `players 2`, 43 fps. The other player's
  gold-torso figure is in view, and the craters, a crate and the half-felled watch tower are shared.
- **Still open:** the client and server logs (corrections per minute, check counters) were not collected. Stage 8's
  soak measures those numbers.
