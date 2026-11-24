# Fight rules (M2a)

[[../../../00 Docs Map|Docs map]] · [[rules|rules seam]] · [[networking|networking]] · [[magic|magic]] · [[../notes/net|net notes]]

| | |
|---|---|
| Status | Built ([wp-7], 2026-11-23; the owner's taste check pending) |
| Decided | Us, 2026-11-23 (the owner may veto): decisions 1-20 of the [wp-7] task pack |
| Next | [wp-8] (fight feel under authority) |
| Open | the owner's taste check |
| History | - |
| Marks | code read at engine `85bf73b`; built on `b2f4cd5` ([wp-6]) |

The sandbox becomes a PvP game: players have health, blasts, falls, lava and the void hurt them, the dead respawn
at their team's spawn after 3 s, two teams play one round to a score limit (team deathmatch) or until a tower
falls (the Felling duel), and the HUD shows all of it. Bots fight too, and `Shredborne --practice` plays a round
against them offline through an in-process server.

## Decision

1. **Server-only rules.** Health, damage, death, respawn, charges, teams, score and the round are decided by the
   session server only (`net::Server`, `engine/src/net/internal/ServerFight.cpp`). Clients show what the owner
   block, the remote record and three messages (`Roster`, `RoundUpdate`, `KillNotice`) say. Nothing of it is
   predicted except "a dead player does not move". Integers wherever a test pins a value.
2. **`vortex.sim:combat`** (`engine/src/sim/Combat.cppm`, `internal/Combat.cpp`) holds the rules: `FightRules`,
   `FighterState`, the damage functions and the round machine `sim::Round`. GPU-free and network-free.
   `net::Server` owns one `FighterState` per slot and one `Round`; the offline sandbox owns none.
3. **Modes**: the session key `mode = sandbox | deathmatch | felling`, default `sandbox`. A sandbox session is
   today's game (no teams, no damage, unlimited charges, no round): `sim::SandboxRules()`, whatever the other fight
   keys say. Every session file from before (`drill`, `arena250`, `playground`) is a sandbox. `deathmatch`: the
   first team to `score_limit` credited kills wins. `felling`: as deathmatch, and releasing the enemy tower's
   crown wins at once.
4. **One round per session.** After `Ended` the server logs `Round: session over at tick T`; with `end_session = 1`
   (the default of a fight mode) it disconnects every peer and stops ticking; a listening `vortex_server` then
   exits 0 (a live install's wrapper starts the next session). No region reset. The game's client, closed after
   its round `Ended`, keeps the result on screen with `SESSION OVER`, logs `Session: the round is over, the server
   closed the session` and exits 0 (a close in any other phase is still `Disconnected from the server`, exit 1).

## Rules

- **Health 100.** Friendly fire off, self-damage off.
- **Respawn 3 s** (`respawn_ticks`, 180) at the team spawn; then 2 s (120 ticks) of spawn protection (no damage
  taken), ended by the player's first cast.
- **Charges**: 3 at most (`max_charges`), one back every 72 ticks (`charge_ticks`), the 15-tick cooldown kept as
  the burst limit. A cast edge at capture: dead -> `CastRejection::Dead`; no charge -> `NoCharge`; else one charge
  is spent and the cast queued. No refunds (a cast refused later, Missed or Outside, keeps its charge spent).
  Charges are spent in `Countdown`, `Live` and `Ended`; the warmup's casts are free.
- **Death and the movement rule.** A dead player is not stepped (the server skips `sim::StepPlayer` for it; it
  stays where it died, velocity zero); a cast edge of a dead player is refused `Dead`, a crate or jump edge does
  nothing. The client stops predicting while its owner block says dead. A respawn is a teleport by the server:
  `PlayerState{spawn feet, velocity 0, flags 0, cooldown 0, lastButtons = kPlayerButtonMask}` (no edge until a
  button is released: a held click never fires at the spawn). The owner block's `lifeSeq` (+1 per spawn) tells
  the client: a new `lifeSeq` takes the snapshot's owner state and replays the inputs after it without counting a
  correction or a snap. `StepPlayer` is unchanged: `sim::kPlayerRuleRevision` stays 1. As built after the review:
  the client records the new life when the snapshot arrives and takes it in `Reconcile`, once a snapshot of that life
  has its events applied (the replay steps on the terrain the server stepped); until then the player is not stepped,
  as while dead. Over a reach that is not current the state is taken as the held path takes it and the replay waits
  for the resume. A death keeps the inputs predicted after its snapshot (with the frozen state) and drops a pending
  resume, so a respawn sooner than the client's lead replays them once. An input of the dead time (its tick at or
  before the life's start, `FighterState::lifeStartTick`) that reaches the server late is not of the new life: its
  cast edge is refused `Dead` and nothing else of it acts.
- **Warmup.** Until `min_players` players are on both teams the round waits; the players already there walk and
  cast freely (no damage, no charges). They are on their teams (a joiner starts at its team's spawn) but not
  spawned (`FighterState::spawned` false, owner flag `kSpectator`). As built: the pack froze warmup players; they
  move, because a joiner waiting frozen is no fun and the client predicts them like any player.
- **Environment damage** (server, after each player's step, slot order, `Live` only): a **fall** - on the tick a
  player becomes `OnGround` after a step that began in the air, `s = floor(-velocityBefore.y * 100)` cm/s, damage
  `(s - 886) * 11 / 100` when `s > 886` (a 4 m fall); **lava** - the voxel at `feet - 0.05 m` or `feet + 0.2 m` is
  the material `lava`: 20 every 15 ticks (the first at once); **void** - feet below the region's height minimum
  + 2 m: 999. Causes `Fall`, `Lava`, `Void`.
- **Kill credit.** A death by `Blast` credits its source (an enemy: friendly and self damage are never dealt). A
  death by `Fall`, `Lava`, `Void` or `Crush` credits the last enemy who damaged the victim within 300 ticks; else
  nobody. A credited kill: the killer's kills + 1 and its team's score + 1. Every death: the victim's deaths + 1.
  A leaving player is nobody's last attacker any more and its queued casts still carve but hurt and credit nobody
  (the slot goes to the next joiner); a credit never goes to the victim's own team.
- **Teams** 1 and 2. A joiner goes to the team with fewer players, ties to team 1; a peer that leaves frees its
  place; teams are never rebalanced mid-round. Team spawns are the session keys `team_spawn_1 = X Z`,
  `team_spawn_2 = X Z` (metres, at least 4 m inside the play bounds). The feet go to the ground under
  `(X + ox, Z + oz)` (a downward `World::Raycast` from the region's height maximum), `(ox, oz)` 2 m on a ring of 8
  (`k` = the team's spawns so far mod 8, 45 degrees apart, from an integer table), plus 0.05 m; a miss uses
  `(X, Z)`, then the template's spawn. The Welcome's spawn is the joiner's team spawn.
- **The damage entry** is `Server::Impl::DealDamage(const sim::DamageEvent&)`: blasts, the environment and later
  spells and crush go through it, so kill credit, the kill feed and the score have one path.
- **Session keys** (`net::ParseSession`): `mode`, `team_spawn_1`, `team_spawn_2`, `score_limit`, `round_seconds`
  (1..3600), `respawn_ticks`, `charge_ticks`, `max_charges` (0..9), `min_players`, `countdown_ticks`, `end_ticks`,
  `tower_1`, `tower_2`, `end_session`. Refused: a fight mode without both team spawns, a spawn or tower less than
  4 m inside the play bounds, felling without both towers, `charge_ticks` 0 with charges, `max_clients` above 64
  (the `Roster`'s bound). `FormatSession` writes
  them only for a fight mode. The sessions `duel.session` (2v2 deathmatch, G22) and `felling.session`.

## Damage

A blast hurts every living enemy of its caster within reach, computed in `sim::ResolveAction` (rules.md
"Outcome") with `sim::BlastDamageAt`, in integer millimetres:

- centre `c = llround(centreQ * voxelSize * 1000 / 16)` per axis; feet `f = llround(feet * 1000)`;
- the capsule's axis runs from `f.y + r` to `f.y + h - r` (`r` = 300, `h` = 1800 for the default tuning); `dy` is
  the distance of `c.y` outside it (0 inside); `d = ISqrt64(dx^2 + dz^2 + dy^2)` (floor);
- `s = max(0, d - r)` (the distance to the capsule's surface); damage `= 60 * (1200 - s) / 1200` when
  `s < 1200`, else 0.

A cast's ray also meets the capsules of the living enemies of its caster (`AimRules::playerRadius` /
`playerHeight`, the session rules only; a player is a `CharacterVirtual` with no body): an enemy nearer than the
terrain and the bodies takes the blast where the ray meets its capsule, with the terrain radius, so 60 at the
surface. Server only: the aim is never predicted, so nothing on the wire changes.

No occlusion: walls within 1.2 m are carved by the same blast. Worked numbers (`Combat.BlastDamageFallsLinearlyToZeroAt1200mm`,
feet at y 10 m, 0.125 m voxels): centre at the feet - 60; 0.5 m aside at y 10.75 m - surface 200 mm, 50; 0.75 m
aside - 450 mm, 37; 1.5 m aside - 1200 mm, 0; above the head at y 12.5 m - `dy` 1000, surface 700 mm, 25. Falls
(`Combat.FallDamageStartsAtFourMetres`): -8.85 m/s 0, -12.53 m/s 40, -17.72 m/s 97, -30 m/s 232.

Damage counts only in a `Live` round: `Countdown` and `Ended` carve but never hurt. A blast at a player's feet
leaves it in the crater; a second blast at the crater's floor takes less (the rim stands between).

## Round

`sim::Round` (`sim::StepRound`, `sim::ScoreKill`), server-only; `RoundUpdate` carries it.

- `Warmup` until `min_players` players are on teams and both teams have one -> `Countdown` (`countdown_ticks`,
  300): every player respawned at its team spawn, health and charges full, kills, deaths and scores 0 ->
  `Live` (`round_seconds`, 480 s) -> `Ended` (`end_ticks`, 600; nobody takes damage, casts still carve).
- Win: a team reaches `score_limit` (15), Felling's crown (the enemy's crown under 20 % of its voxels, checked
  every 30 ticks; both at once: the higher score, else a draw), or a team has no players for 1800 ticks in `Live`
  (forfeit). At time out the higher score wins; a tie goes to overtime: the next credited kill wins, at most 3600
  more ticks, then a draw (winner 255).
- A joiner in `Countdown` or `Live` spawns at once.
- **Felling** (`sim::BuildFellingTower`): each team's tower at `tower_1` / `tower_2` - a 4 x 1.5 x 4 m stone base
  1 m into the ground, a 2 x 10 x 2 m brick shaft, a 3.5 x 2.5 x 3.5 m brick crown - applied at the server's start
  after the template's operations, as stream operations (joiners get them through chunk sync), and built again
  (the same boxes) when the round goes `Live`, so a crown cut by the free warmup casts stands whole. The load check
  releases the crown when the shaft is cut (`Felling.CuttingTheShaftFellsTheCrown`); a blast 6 m away leaves it
  standing.

Log lines (G22 reads them; docs/verify.md): `Round: phase <Warmup|Countdown|Live|Ended> at tick T (players A + B)`,
`Kill: tick T slot V (team X) by <Blast|Fall|Lava|Void|Crush>, credit slot K` (`credit none`), `Round: team N wins
A-B by <score|time|felled|forfeit> at tick T (<mode>)` or `Round: draw A-B at tick T (<mode>)`, `Round: session over
at tick T`, and `Fight: kills K, deaths D, damage events E, damage H hp, refused dead X, no charge Y, respawns R`.

## Wire

Protocol 11, the only bump in WP4-WP8 (`net::kProtocolVersion`; the values in notes/net.md section 2):

| What | Bytes |
|---|---|
| `Welcome` after the probes | `mode u8` (<= 2), `maxHealth u8` (>= 1), `maxCharges u8`, `chargeTicks u16`, `respawnTicks u16`: +7 B |
| Owner block after `castRejected` | `health, charges, chargeTicks, flags, respawnTicks, lifeSeq, hits`, u8 each: +7 B (`kSnapshotOwnerBytes` 39). Ticks left clamped to 255; flags `kDead` 1, `kProtected` 2, `kSpectator` 4 (more refused) |
| Remote record after `flags` | `fight u8`: bits 0-6 health (at most 127), bit 7 dead (`kSnapshotPlayerBytes` 17) |
| `Roster` (21) | `type, count u8` (<= 64), per player `slot u8, team u8` (<= 2), `kills u16, deaths u16`: 2 + 6 B each; duplicate slots refused; at most one per tick (sent at the end of `StepFight` when a kill, join, leave or the round start changed it) |
| `RoundUpdate` (22) | `type, mode, phase, phaseEndTick u32, score1 u16, score2 u16, scoreLimit u16, winner u8` (0, 1, 2, 255), `end u8` (<= 4), `overtime u8` (<= 1): 16 B |
| `KillNotice` (23) | `type, tick u32, victim u8` (a slot), `killer u8` (slot + 1, 0 none; never the victim), `cause u8` (1..5), `flags u8` (`kCredited` 1): 9 B |

`CastRejection` 4 `NoCharge`, 5 `Dead` (`kMaxCastRejection` 5). `DamageCause` 1 `Blast`, 2 `Fall`, 3 `Lava`,
4 `Void`, 5 `Crush` (reserved for [si-2]; nothing emits it here); [mag-1] / [mag-2] spell damage uses `Blast`.
`GameMode` 0-2, `RoundPhase` 0-3, `RoundEnd` 0-4. No new `PlayerButton`, `EventKind`, `WorldOpKind` or
`EntityKind`; `buildHash` gets no new input. The three messages go on the events channel and are taken on
receipt, not by opSeq (they are not stream events). Sizes pinned by `FightWire.SizesAreAsDesigned`.

## Bots

`PilotSettings::fight` (`vortex_server --fight`, `vortex_bot --fight`): `net::Bot` hands its pilot the living
enemies (`RemotePlayers()` on the other roster team, dead bit clear) and whether it may cast (alive, a charge,
cooldown 0) before each tick; a dead bot sends neutral inputs. The pilot (`BotPilot::SetTargets`, `SetCanCast`)
holds the nearest enemy (x / z, ties the lower slot) until it leaves the list; runs toward it beyond
`keepDistance + 4` (16 m), backs off under `keepDistance - 4`, strafes at right angles between (direction flipped
every `strafeTicks`, the first from the seed); jumps as before; turns back at the play bounds' margin; aims its
eye at the enemy's feet (0.1 m up, `sim::AimAngles`); casts on an edge (one tick down) once it held the target
`reactionTicks` (20) and the target is within `fightRange` (30 m). No line of sight is checked (the blast carves
cover). Deterministic: no randomness besides the pilot's seed, remote players by slot. Without a target it
wanders as before.

Gate **G22** (`tools/verify.sh` tier `full`, no UDP port): `vortex_server --session=duel.session --clients 4
--fight --until-round-end --require-winner --ticks 30000 --loss 0.05 --delay-ms 40 --jitter-ms 30` exits 0 with
`Round: team N wins`, `hashes equal`, four `Entities: ... agree`, no check mismatch and `Fight: kills K` with
K >= 5.

## Practice

`Shredborne --practice [--bots N] [--mode deathmatch|felling]` (N 1-3, default 1; 0 is refused, as a round needs a player
on each team): an in-process `net::Server` on `net::LoopbackTransport`, driven on the game's main thread (transports
are main-thread only) at most 8 ticks a frame with at most 8 ticks of backlog kept (a longer hitch is dropped, never
run fast later: `Server::Advance`'s bounds), with in-process `net::Bot`s, all built by `InProcessServer` (`src/Practice.cpp`). The region is the playground of
`playground.session` written in code, the spawns of `duel.session`, `min_players = 1 + bots`, `score_limit = 10`,
`round_seconds = 480`; felling adds the towers of `felling.session`. The player is slot 0 (team 1); the bots'
seeds are `0xb07 + i`. When the round is over, Enter starts a new one. The offline sandbox (no flag; G3-G10, the
editor) is unchanged.

## HUD

The game's fight HUD (`src/FightHud.cpp`, `HudCanvas`; no ImGui), drawn only in a fight mode:

- health: a 200 x 14 px bar at the bottom centre (green above 60, yellow above 30, red below; white outline while
  protected) with the number beside it;
- charges: `maxCharges` pips of 6 x 6 px under the crosshair, the next one filling from the bottom;
- a hit marker (four short diagonal strokes round the crosshair) for 12 frames whenever the owner block's `hits`
  changes, and a red flash of the screen's edge when the own health drops;
- the kill feed at the top right under the frame rate (`KILLER > VICTIM  CAUSE`, names in team colours, 6 s, at
  most 5);
- the round banner at the top centre: `BLUE 3 : 2 RED   7:42`, `WAITING FOR PLAYERS A/B`, `STARTS IN N`,
  `OVERTIME`; at the end `BLUE WINS` / `RED WINS` / `DRAW` with the reason, large in the middle;
- the scoreboard (Tab held, or `--scoreboard`): two columns by team, `P3   K 4   D 2`, the own row `YOU`;
- the death screen: `KILLED BY P2 (BLAST)` and `RESPAWN IN 2.4`, the crosshair and pips hidden;
- `Cast not run:` keeps its line (`NO CHARGE`, `DEAD`).

Team colours: team 1 blue `Rgba(70, 130, 255)`, team 2 red `Rgba(230, 70, 60)`; figures take their team's torso
colour, a dead remote figure is hidden. Names are `P<slot + 1>`, the own one `YOU`. The exit line of a session or
practice client: `Fight: slot S team T health H kills K deaths D, round <phase> A-B`.

## Stages

| Stage | What | State |
|---|---|---|
| [wp-7] | M2a first fight: health, damage, death, respawn, teams, charges, rounds, Felling, fighting bots (G22), the fight HUD, offline practice | built (engine and game [wp-7] commits, 2026-11-23); the owner's taste check open |

## Open questions

- The owner's taste check after the first playtest: too fast, too slow, too lethal?
