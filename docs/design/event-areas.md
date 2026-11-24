# Event areas: castle fights in a local world

[[../../../00 Docs Map|Docs map]] · [[fight|fight]] · [[rules|rules seam]] · [[networking|networking]] · [[rendezvous|rendezvous]] · [[worldgen|worldgen]] · [[../notes/net|net notes]]

| | |
|---|---|
| Status | Designed, nothing built or measured for it |
| Decided | Owner, 2026-11-23 (castle event areas: local open world, synced castle-fight areas at fixed places, a join prompt at the border, re-entry delay, a lobby that is a place, free for all with parties and friendly fire on, a pluggable objective, reset after each event, a server-held profile, bots test-only). Us, 2026-11-24 (the owner may veto): decisions 1-24 below, the stage split and the wire values |
| Next | [area-1] border and handoff, right after [wp-7] and before [wp-8] |
| Open | "Open questions" 1-12, each with the recommended answer |
| History | - |
| Marks | code read at engine `9474e9c`, game `9729e09` ([wp-7] committed). "est." = an estimate, replaced by the number the stage measures |

The open world is local: one player (later a small co-op group) plays it on their own machine, offline, with no
server. Castle fight areas are places on that map. At an area's border the game asks "Join the siege?"; on yes it
loads for a few seconds and the player is inside the area's server session, with everyone else who came. Walking
out (with a second prompt) or the event's end brings the player back to the local world at the border. Each area
runs events: players gather (the lobby is the area itself), a countdown, a free-for-all fight for an objective,
the result, and the area resets to its template for the next event. What a player earns lives in a profile held by
the home server.

The tasks packs are [llm/area-1.md](../llm/area-1.md), [llm/area-2.md](../llm/area-2.md) and
[llm/area-3.md](../llm/area-3.md).

## Decision

1. **Two worlds, one engine.** The local world is today's offline `sim::Simulation` (the window follows the camera,
   built-in terrain). An area session is today's session client (`net::Client`, its own `sim::Simulation` built
   from `Welcome`). Inside an area only the session's simulation runs and is drawn; the local one is **parked**:
   kept in memory untouched (not stepped, no streaming), and taken back when the player leaves. One `Engine`, one
   window, one device: `Engine::ParkSimulation`, `Engine::StartSimulation`, `Engine::AdoptSimulation`,
   `Engine::StopSimulation` swap what the engine steps and draws ([area-1]).
2. **Same coordinates.** An area's `sim::RegionDesc` is a box of the local world: its play bounds are absolute chunk
   coordinates, its terrain is the local terrain (`TerrainSettings` defaults, seed 1337, 0.125 m voxels, height
   -48..128 m). Session and local world use the same voxel coordinates: no offset, no transform. The client checks
   the `Welcome`'s region against its own copy of the area (`RegionDesc::operator==`) and joins nothing else.
3. **The local copy of an area is pristine.** Offline, the area is fenced: `PhysicsWorld::SetCharacterWalls` at the
   area's play bounds (the same closed fence a session puts round its play area keeps the local character out), and
   a local cast whose sphere reaches into the area (`sim::AreaGuard`) is refused `Outside`. So the local copy of an
   area always equals the area's reset state (terrain and template), and what the player sees across the border
   before joining is what the event starts from.
4. **Inside, the server's world replaces the local copy** by construction: the session's world is the server's,
   and its moving window (the game's 48 chunks) shows the local terrain's pristine columns beyond the server's
   window (`sim::ClientWindowMoves`): the surroundings stay visible, without the local player's edits there.
5. **The handoff is a prompt, then a short load.** In the approach band (outside the play bounds, within 4 m of
   them) the HUD asks `JOIN THE SIEGE AT <NAME>?  ENTER join`; Enter connects while the local game keeps running;
   the `Welcome` parks the local world and starts the session's. Target: joined in under 4 s on the UHD 620 (est.;
   [area-1] measures `Area: joined ... in X s`). The join spawn is the entry point: the feet's x / z, clamped 2 m
   inside the play area, on the ground (`Hello::entry`).
6. **Leaving is a prompt too.** At the play bounds (within 1.5 m of the edge, where [wp-5]'s `EDGE OF THE FIELD`
   hint shows) the HUD asks `LEAVE THE SIEGE?  ENTER leave`; the pause menu has `Leave siege`. Leaving disconnects
   (the server removes the player: the others see it vanish, as any peer that leaves), stops the session's
   simulation, takes the parked local world back, and puts the local feet 3 m outside the nearest edge (outside the
   fence), on the local ground. The event's end (`Refuse{EventOver}`) and a lost connection take the same path.
7. **Re-entry delay** (anti death-dodging). A player who leaves during `Countdown` or `Live` may not rejoin that
   area's event for 60 s (`reentry_seconds`). Held by the server ([area-2]: a ledger keyed by the player id,
   cleared at the reset), refused `RefuseReason::Reentry` with the seconds left; [area-1] has the client-side timer
   only. A leave within 5 s (`creditTicks`, 300) of damage from another side counts as a death credited to that
   attacker (cause `Leave`): dodging out of a lost fight costs what dying costs.
8. **The area session lifecycle**: `Waiting -> Countdown -> Live -> Ended -> Reset`, the [wp-7] round machine with
   these names (`Waiting` is `sim::RoundPhase::Warmup` on the wire; `Reset` is server-only, no wire value):
   - `Waiting` (the lobby: the area is a place on the map): players walk, look round, cast freely (no damage, no
     charges) until `min_players` (4) are in and at least 2 sides;
   - `Countdown` (`countdown_ticks` 600, 10 s): everyone respawns on the spawn ring, health and charges full;
   - `Live` (`round_seconds` 600): damage counts; the objective runs; joiners spawn at their entry at once;
   - `Ended` (`end_ticks` 600): the result on every screen, nobody takes damage;
   - `Reset`: the server sends `Refuse{EventOver}` to every peer (they go back to their local worlds), drops the
     session and builds a new `net::Server` from the same session file on the same transport (`vortex_server
     --area`): the region is pristine again (generator + template). Measured target: reset in under 3 s (est.).
9. **Free for all with parties; friendly fire on.** `sim::GameMode::Siege` (3) replaces [wp-7]'s two teams for areas.
   Every player is on a **side**: alone, its own side; in a party, the party's side (at most `max_party` 4). A party
   is formed before joining by a shared party word (`--party WORD` / the settings key `party`; `Hello::partyKey` =
   its FNV-1a 64 hash, 0 = solo); co-op parties of the local world replace the word later. Damage hits everyone but
   the caster (friendly fire on, self-damage off). A kill of another side is credited; a kill inside one's party is
   not (the victim's deaths + 1, `credit none`). [wp-7]'s `deathmatch` and `felling` stay as test and dev modes
   (G22, `--practice`).
10. **Side ids** are 1..64, the smallest id no present player holds, given at the join (a party's later members
    take its id); `sim::TeamId` keeps its type (u8). A siege session has `max_clients` at most 64.
11. **Objectives are a pluggable rule**: `sim::ObjectiveKind` (`CaptureKeep` 1, `FellTower` 2, `HoldTimer` 3,
    `MostKills` 4), one `sim::ObjectiveState`, one `sim::StepObjective` that switches on the kind; the round machine
    reads only "completed by side S" and the time-out winner. **Capture the keep** is built first ([area-2]);
    `MostKills` is the time-out fallback of every kind. `FellTower` (from [wp-7]'s Felling) and `HoldTimer`
    (defenders hold until the time runs out: needs sides chosen at the join, after co-op parties) are later cards.
12. **Capture the keep**, integer, server-only, once per tick in `Live`: count each side's living players with
    feet in the keep's zone (a voxel box: the keep's hall, 4 m high). No side: progress - 1 (not below 0). Two or
    more sides: contested, no change. One side S with n players: if S holds the capture (or nobody does), progress
    + min(n, 3); else progress - 2 min(n, 3), and when it reaches 0, S holds it. Progress reaching `capture_ticks`
    (1800: 30 s for one player alone) wins the round for the holder. At time out: the side with the most credited
    kills; a tie goes to the holder with progress above 0; else a draw.
13. **The keep** is `sim::RegionTemplateId::Keep` (2): a curtain wall (40 m square, 6 m high, 1 m thick, a 4 m gate
    gap facing -x) and a keep (12 m square, 10 m high, brick, a door on -x) on a stone foundation, centred on the
    play bounds at `RegionDesc::siteVoxelY` (session key `site_y`, voxels). It is **stamped by the generator**
    (`voxel::StampedGenerator`: integer boxes in a fixed order, applied per chunk after the terrain), so the server,
    every client and the local world make the same voxels with no stream operations, and a reset is "generate
    again". W2 replaces the keep with a planned castle.
14. **Spawns**: the join spawn is the entry point (decision 5). Respawns in `Countdown` and `Live` take the point of
    the spawn ring (8 points, `spawn_radius` 36 m round the play bounds' centre, the [wp-7] integer ring table)
    farthest from every living player of another side (largest smallest squared distance, integer centimetres;
    ties: the lowest index).
15. **Charges, health, damage, falls, lava, void, respawn time, protection** are [wp-7]'s ([fight.md](fight.md)
    "Rules", "Damage"); only who counts as an enemy changes (decision 9).
16. **The server-held profile** ([area-3]). Identity: 32 random bytes made once per installation
    (`profile.key`, mode 0600, in the game's settings folder); `playerId` = the first 16 bytes of
    SHA-256("vortex-id-v1" || key); `authKey` = SHA-256("vortex-auth-v1" || key). The server stores per id the
    `authKey` (registered on first use), the progress and the plausibility counters, one text file per id in a
    profile folder on the home server (`vortex_server --profiles DIR`), written by write-then-rename.
17. **Authentication**: after a siege `Welcome` the server sends `ProfileChallenge{nonce}`; the client answers
    `ProfileProof{HMAC-SHA256(authKey, "vortex-profile-v1" || nonce || playerId)}` (with `authKey` itself the first
    time: trust on first use). No proof within 600 ticks, or a wrong one: the player is a **guest** (plays, earns
    nothing, `GUEST` on the roster); never kicked for it.
18. **Progress**: `xp` (level = 1 + floor(sqrt(xp / 50))), siege wins, kills, deaths, and an `unlocks` bit set
    (reserved: [mag-2] attunements read it later). Fights earn on the server: 10 xp per credited kill, 100 per won
    event (each member of the winning side), 20 for 120 s or more in `Live`. Local play reports what it earned as a
    claim (`ProgressReport{seq, localSeconds, xp, mac}`), checked for plausibility (decision 19). Progression is
    horizontal: it never changes health, damage or charges in a fight.
19. **Plausibility of a claim**: `seq` above the last accepted (else ignored); `localSeconds` at most the wall time
    since the last accepted report + 60 s, and at most 4 h; `xp` at most `localSeconds * 600 / 3600` (600 xp an
    hour of local play): over it is clamped to the cap and counted; a claim over twice the cap 3 times marks the
    profile `suspect`, after which local claims are ignored (logged; the owner looks). Until PvE (M4) local play
    earns 1 xp per active minute (the feet moved at least 1 m in it), so the cap is far above honest play.
20. **What the fight server reads** from the profile: `verified`, `level` (shown on the roster) and `unlocks` (none
    used before [mag-2]). Nothing else; never a stat.
21. **Test-only**: bots (`net::Bot`, `vortex_server --clients`), `Shredborne --practice` (kept as a dev tool, no menu
    entry), `--area-test`, `vortex_server --events N`. Mobs and NPCs come with M4 / M5 (PvE), not here.
22. **Wire**: three bumps, one per stage, the next free numbers after [llm/README.md](../llm/README.md)'s plan
    (11 [wp-7], 12 [mech-1], 13 [mag-2]): **14** [area-1], **15** [area-2], **16** [area-3]. If an area stage lands
    before [mech-1] or [mag-2], the stage that lands takes the next free number then and the later packs renumber
    (README "Values planned across the packs"). Values in "Wire".
23. **Gates**: **G25** [area-1] (a headless client enters and leaves an area: its world hash equals the server's and
    the local world comes back unchanged; tier `full`, UDP P25 = 27045, slot B+5) and **G26** [area-2] (two events
    of 4 fighting bots, one party, on the keep: each ends with hashes equal, the reset between them; tier `full`, no
    UDP port). [area-3] adds no gate (unit tests, and G26 logs the profile lines).
24. **Placement**: [area-1], [area-2], [area-3] after [wp-7], before [wp-8]. [wp-8]'s 10-client fight measurement then
    measures a siege (FFA, the keep) as well as its own `fight10` session.

## The handoff, step by step

Local side (the game, `src/Areas.cpp`, on the main thread; the engine's run loop returns to a small shell in
`src/main.cpp` that does the swap):

1. Each frame: the nearest area (`sim::NearestArea`) within 96 m: fence on (`SetCharacterWalls(bounds)`), off beyond
   128 m (hysteresis). Its curtain is drawn (`Engine::SetPlayBounds`).
2. In the approach band (`DistanceToPlayAreaEdge(bounds, feet)` in [-4, 0)): the prompt. Enter: `Connecting`.
3. `Connecting`: ENet to the area's server (`--area-server` / settings `area_server`, token `area_token`), `Hello`
   with `entry` (and the player id and party key from [area-2]). The local game keeps running; the prompt shows
   `JOINING <NAME>...`. 10 s without a `Welcome`: `NO ANSWER`, back to the prompt. A refusal: its text
   (`REENTRY IN 42 S`, `FULL`, `EVENT ENDING`).
4. `Welcome`: its region must equal the area's (else disconnect, `Area: the server's region differs`). The local
   game stops (its run returns), the shell records the local feet, the local world's hash over the area's window
   (`voxel::WorldHash`), parks the simulation, starts the session's (`client.MakeSimulationConfig` with the
   engine's store capacity, `client.MakeGenerator`), and runs the session game on the same engine (the [wp-7]
   `PlaySession` body without its `Engine` construction). The connection is serviced from a helper thread while the
   session world starts, as today.
5. Leave (prompt, pause menu, `EventOver`, a lost link): the session game's run returns; the shell disconnects,
   stops the session simulation, adopts the parked one (every used chunk re-uploaded:
   `ChunkStore::MarkAllDirty`), hashes the area's window again (must be equal: nothing touched the parked world),
   puts the feet outside (decision 6), and runs the local game again.

The renderer's chunk pool is sized once (the local game's `chunkCapacity`, 8192); the session's store is capped to
it (`MakeSimulationConfig(base, capacity)`), so nothing on the GPU is re-allocated by a swap. A session in an area
needs at most the local game's window (48 chunks) of chunks plus `kSessionStoreHeadroom` (est.: the game stays
under 7000 offline).

## Memory and cost

| What | Cost |
|---|---|
| The parked local world | its used chunks stay in RAM (est. 150-250 MiB at the default window); measured by `--area-test` (`Area: parked world N chunks, RSS X MiB`) |
| Join | est. 2-4 s on the UHD 620 dev build: the session world's first window (30 x 44 x 30 cells of the server's window, the rest pristine local columns), the baseline, the start radius |
| Return | est. under 1 s: no generation, one upload of the used chunks |
| A reset | est. under 3 s (a 30 x 30-chunk window; arena250's 68 x 68 takes 10 s) |
| The keep's stamps | a few boxes clipped per chunk: under 1 % of generation time (est.) |

If the parked world's RSS is over 400 MiB on the owner's laptop, the fallback (open question 3) writes its modified
chunks to a scratch save folder (`WorldConfig::saveDirectory`) and rebuilds it on return.

## Wire

| Protocol | What | Bytes |
|---|---|---|
| 14 [area-1] | `Hello` after `token`: `entryFlags u8` (bit 0: has an entry; others refused), `entryXcm i32`, `entryZcm i32` (feet, centimetres) | +9 B |
| 15 [area-2] | `Hello` after the entry: `playerId[16]`, `partyKey u64` | +24 B |
| 15 | `Refuse` after `reason`: `seconds u16` (Reentry: seconds left; else 0) | +2 B |
| 15 | `RefuseReason` 7 `Reentry`, 8 `EventOver`, 9 `NoIdentity` (a siege Hello with a zero player id) | - |
| 15 | `sim::GameMode` 3 `Siege`; `sim::RoundEnd` 5 `Objective`; `sim::DamageCause` 6 `Leave` | - |
| 15 | `sim::RegionTemplateId` 2 `Keep`; `RegionDesc::siteVoxelY` (i32) in `Welcome`'s region | +4 B |
| 15 | `Roster`'s `team` and `RoundUpdate`'s `winner` up to 64 when the mode is `Siege` | - |
| 15 | `MessageType` 25 `ObjectiveUpdate`: `type, kind u8 (1..4), holder u8 (0..64), progress u16, target u16, flags u8 (contested 1)` | 8 B |
| 16 [area-3] | `MessageType` 26 `ProfileChallenge` (`type, nonce[16]`: 17 B), 27 `ProfileProof` (`type, flags u8 (register 1), mac[32]`, then `authKey[32]` with register: 34 or 66 B), 28 `ProgressReport` (`type, seq u32, localSeconds u32, xp u32, mac[32]`: 45 B), 29 `ProfileState` (`type, flags u8 (verified 1, guest 2, suspect 4), level u8, xp u32, wins u16, unlocks u64, reportSeq u32`: 21 B) | - |
| 16 | `Roster` entry gains `level u8` (0 = guest) | +1 B each |

`sim::ObjectiveKind` 0-4 is a wire value (`ObjectiveUpdate`). `buildHash` gets no new input: the keep's stamps
are code covered by the protocol version, and the profile is server data.

## Tests (by stage; the packs give each in full)

- [area-1]: `Area.*` (sim: approach band, entry and exit feet, the guard, the built-in list),
  `AreaWire.HelloCarriesTheEntry`, `AreaHandoff.*` (net, loopback: a join spawns at the entry, a leave removes the
  player for the others, the client refuses another region), `AreaSession.TheBuiltInAreaMatchesItsSessionFile`.
- [area-2]: `Keep.*` (stamps: chunk order free, equal to a box fill), `Objective.*` (capture rules, contested,
  decay, time-out winner), `Siege.*` (sides and parties, friendly fire, party kills not credited, the leave death,
  the re-entry refusal, the spawn ring), `SiegeWire.*`, `AreaReset.*` (a new event starts pristine).
- [area-3]: `Profile.*` (ids, proof, registration, guest), `ProfileStore.*` (atomic file, reload),
  `Plausibility.*` (each cap), `ProfileWire.*`.

## Stages

| Stage | What | State |
|---|---|---|
| [area-1] | one area (Hollowkeep, pristine terrain, sandbox) at chunks x 100..124, z -12..12; the fence and the guard offline; the prompt; `Hello::entry`; the engine's simulation swap; leave by prompt and pause menu; the client-side re-entry timer; `--area-test`; gate G25 (protocol 14) | Designed |
| [area-2] | the keep (stamped generator); `GameMode::Siege`: sides, parties, friendly fire, the spawn ring; the lifecycle with `EventOver` and `vortex_server --area` resets; the objective rule with capture the keep; the server's re-entry ledger and the leave death; the siege HUD; bots that seek the keep; gate G26 (protocol 15) | Designed |
| [area-3] | the identity key, the profile store on the home server, challenge and proof, guests, server-earned xp, local claims and their plausibility, level on the roster (protocol 16) | Designed |

## Rejected alternatives

| Rejected | Reason |
|---|---|
| One shared open world on a server | The owner's decision: the open world is local; only castle areas are synced |
| Teleporting the player to a separate arena map | The area is a place on the map: walking up to the castle is the point |
| Translating the area's coordinates into a server-local frame | Same voxels on both sides already; a transform would put a float in what decides voxels |
| Keeping the local world running while inside | Two simulations stepping on one frame budget (UHD 620); nothing local needs to move meanwhile |
| A second `Engine` (a new window) for the session | Closes and reopens the window: seconds and a flash; the device and pipelines are the slow part (est. 1-2 s) |
| Template operations (stream ops) for the keep | The local world would have to apply them when the area's chunks first load (a window centred there, seconds) and every joiner gets them through chunk sync; stamping in the generator is free on both |
| Restarting the server process for a reset | The live wrapper restarts are for crashes; an in-process rebuild keeps the port and the rendezvous entry |
| Friendly fire off for parties | The owner's decision: on |
| Stat progression (more health, more damage) | Unfair against new players; horizontal unlocks only |
| Ed25519 signatures for the identity | A new dependency (needs the owner); HMAC with a registered key is enough for trusted testers |

## Open questions (each with the recommended answer)

1. **More areas?** -> One in these stages; the list (`sim::BuiltInAreas`) grows with W2's castles, at least 200 m
   apart (one fence at a time).
2. **How does the game find an area's server?** -> The settings keys `area_server` / `area_token` now; the
   rendezvous ([nat-R1]) with one entry per area later.
3. **Park in RAM or save to disk?** -> RAM; the disk fallback if [area-1] measures over 400 MiB RSS for the parked
   world.
4. **Local edits on the area's margin** (inside the server's window, outside the play bounds) do not show inside.
   -> Accepted: inside, the world is the server's.
5. **The re-entry delay**: 60 s, per event? -> Yes; cleared at the reset.
6. **A leave under fire counts as a death?** -> Yes (within 5 s of damage from another side, credited).
7. **Party word or invite?** -> The word now; local co-op groups later.
8. **A full area** -> `Refuse{Full}` shows `<NAME> IS FULL`; no queue.
9. **Objective rotation per event?** -> One objective per area (its session file); rotation after `FellTower` and
   `HoldTimer` exist.
10. **Profile security**: a stolen profile folder lets someone play as its players. -> Accepted for trusted
    testers; signatures later with the owner's go for a dependency.
11. **The plausibility caps** (600 xp an hour, 4 h a report, 60 s slack). -> Start there; re-set with PvE.
12. **Backups of the profile folder** -> The owner's (one line in the live install's notes); no tooling here.
