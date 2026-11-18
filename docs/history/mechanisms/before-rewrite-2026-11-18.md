# Mechanisms history: text replaced or changed by the WP3 rewrite

Moved verbatim from `docs/design/mechanisms.md` lines 1-13, 43-62, 65-83, 166-187, 383-466 and 780-812 at engine
`430fde0` on 2026-11-18 (WP3 document rewrite). The text below is unchanged; corrections go only under "Errata" at the
end.

The original text of every section of the current document that the rewrite replaced or changed, in source order. The
current document holds the rewritten version.

---

# Mechanisms, the use key and ropes

Status: design decision (2026-11-17), amended the same day after two critics' review (see "Review of the design"
at the end: 27 findings, two pairs of them the same, so 25 distinct: 24 applied, two of those with a change, and 1 rejected in part); nothing built or measured for it yet. Requested
by the owner on 2026-11-17: "in game should be implemented rope with physics, in game action which moves objects such
as door opening by clicking E on them or activating lever (for castle gates)". "Code" = read from the working tree on
2026-11-17 (networking stage 7b committed, stage 8 in progress and uncommitted: line numbers in `src/net` may move a
little); file:line references are to that tree (`PW.cpp` = `src/physics/internal/PhysicsWorld.cpp`, `Server.cpp` /
`Client.cpp` = `src/net/internal/`). "Measured" = a number from `docs/ROADMAP.md` or a log; "est." or (e) = an
estimate, replaced by a measurement in the stage named under "Stages". Related: `networking.md` (stage 6 entity
replication, 7a baseline, 7b checks: this design adds to all three), `shred.md` (destruction; the structural integrity
plan SI-1..3), `worldgen.md` (forts, gatehouses, plans), `game-ui.md` (HUD, keys, the owner's checklist), the game's
`docs/world/setting.md` (forts, sieges, "Pass wall", "Hold the crossing", warded keeps).

<!-- source lines 43-62 -->

## What the code gives us today

| Need | Today (code) | Gap |
|---|---|---|
| Moving solid objects | `PhysicsWorld::MoveKinematic` (PW.cpp:949-962, tested `KinematicBodiesFollowMoveKinematic`); the terrain guard never freezes kinematic bodies; a character standing on a moving body takes its velocity (`UpdateGroundVelocity`, PW.cpp:1153) | None for kinematic doors |
| Joints | Jolt v5.6.0 (pinned, `cmake/Dependencies.cmake:122-125`) ships Hinge, Slider, Distance, Pulley ... constraints with motors | `PhysicsWorld` has no constraint API: no handle, `DestroyBody` (PW.cpp:703-710) does not remove constraints (undefined behaviour in Jolt), shutdown order (PW.cpp:370-400) |
| Replicated bodies | Stage 6: `NetId`, `EntitySpawn` (32) .. `EntityWake` (36). Server: `DescribeObject` (Server.cpp:384-399) marks a spawn resting from `sim::IsAwake(physics, body)`; `TrackBodies` (:486-517) wakes / rests every NetId body by `IsAwake` and sets moving ones to `Layer::Rubble`; `AwakeSamples` (:591-606) samples every non-resting track. Client: `MakeCopy` (Client.cpp:675-691) sets the layer from a `resting` flag; `MoveCopies` (:931-965) sets every non-resting copy to Rubble each tick and zeroes its velocity without samples; an `ObjectModel` re-makes a non-static survivor moving and waits for the server's wake / rest (:851-855) | All of it is driven by "awake", which a kinematic body under `MoveKinematic` always is: mechanisms need their own path on both sides (see "Replication") |
| The piece budget | `sim::IsAwake(physics, body)` and `CountPieces` (`Replication.cpp:37-56`) take a body, not an entity | A mechanism must be skipped by its `scene::Mechanism` component, or doors starve blast pieces under the 64-awake cap |
| The entity byte budget | `entityEventBytesPerTick` 200 = **12 kB/s** at 60 Hz, burst 16 KiB (`Server.cppm:46-47`); every entity event charges `entityCredit` (Server.cpp:639-641), which refills to at most the burst and may fall to -8 x burst (:1939). Over budget a damaged object's `ObjectModel` is not sent: the survivor is despawned `ToRubble` (:427-432, `modelsToRubble`) | A gate must never be deleted by the budget |
| Object damage | `Blast` damages every body whose bounds meet the cast sphere's box (Blast.cpp:633-690, `QueryBodies`, no layer filter); `Scene::Damage` removes every non-`Indestructible` voxel in the sphere, hardness ignored (Scene.cpp:493-507); a cut that splits the model makes every piece Dynamic, the kept one is the largest (:556-563), pieces under `replicatedPieceVoxels` (262 voxels = 0.512 m^3, `Simulation.cppm:127-130`) are fragments and a kept one that small destroys the entity (:568-576); the survivor is a new body with `CreateBody`'s size layer (Scene.cpp:66-69: Debris under 25 voxels, else Dynamic) and reports `keptOffset` | A player cast is 1.2 m radius, power 6 (`sim/Player.cppm:79-80`): one cast clears a 2.4 m sphere of any door, and the kept piece's motion, layer and model origin all change |
| A "use" input | `PlayerButton` Jump 1, Run 2, Cast 4, Crate 8, mask 15 (`sim/Player.cppm:18-26`); `PlayerCrate` (Server.cpp:573-587) runs on the step's edge (:929) with the current state, and on the late-input path (:851) **also with the current state** (`pl.state`); only casts rewind the eye (`LateCast`, :806, `pl.StateAt(in.tick)`, ring `kStateHistory` 32, :51) | No use button, no target function, no result to the client |
| Refusal feedback | Casts: `lastCastTick` / `CastRejection` in the snapshot owner block (`net/Messages.cppm:162-207`, `kSnapshotOwnerBytes` 32 at :177), shown as "Cast not run" (`src/Session.cpp:481`) | The same pattern for use |
| The client's lead | `StartPrediction` predicts `ceil(rtt / dt) + 2 + clock.Target()` ticks past the snapshot's tick (Client.cpp:274-279); `Target()` = `baseMargin` 2 + `packetTicks - 1` + a jitter term (`Prediction.cpp:37-40`). The server knows each player's input margin (Server.cpp:874-879) | An event made at server tick T reaches a client whose predicted tick is about T + RTT + Target + 2..3 |
| Templates | `TemplateObject{model, position}` (`sim/Region.cppm:152-155`), made Dynamic (`Region.cpp:381-386`) | No rotation, motion, kind or links |
| Joiners, checks | `BaselineEntity` (`Messages.cppm:275-281`; also the answer to a `ModelRequest`, Client.cpp:1026), `EntityCheck{count, idXor, modelXor}` (`:346-355`), `EntityList` of `{netId, modelHash}` 12 B entries (`:365-378`); a wrong or missing body is asked for by `ModelRequest`, never re-made from the list (Client.cpp:1373-1415) | No mechanism state in any of them |
| Casts at thin things | `ResolveRay` (Server.cpp:770-800) hits terrain or a Dynamic / Rubble body; a ray that hits neither is `Missed` and runs nothing | A rope in front of the sky cannot be shot |
| Drawing thin things | Instances with a full `mat4` (`voxel/Instances.cppm:20-27`); non-uniform scale traces correctly (`trace.glsl:366-369`); instances are in the shadow / AO BVH, rebuilt on the CPU every frame over all instances (`Renderer.cpp:1185-1209`) | No rope primitive; particles are billboards without shadows (`docs/notes/graphics.md:142`) |
| HUD | `HudCanvas` Text / Rect / Crosshair (`core/Hud.cppm:115-143`), the HUD costs 0.01 ms (measured) | No prompt |
| Sound | No audio module (`engine/src`: core, editor, engine, fx, graphics, math, net, physics, scene, sim, voxel) | Feedback is pictures only |

<!-- source lines 65-83 -->

### E becomes "use", the crate moves to Q

- **Decision:** `PlayerButton_Use = 16`, `kPlayerButtonMask` 31. E is Use in play and in a session; the crate keeps
  `PlayerButton_Crate = 8` (its wire value does not change) and moves to **Q** (free: the game reads W A S D, Space,
  Shift, Ctrl, E, F, V, F1, F3, F5, F12, Esc, Left Alt today). The HUD's tool line becomes `Blast [LMB]   Use [E]
  Crate [Q]`.
- **Every place that names E for the crate changes in stage 1 M-a** (one list, so the owner's checklists match the
  build): offline key `src/main.cpp:415`, offline tool line `src/main.cpp:453`, session latch `src/Session.cpp:240-241`
  and the scripted latch `:412`, session tool line `src/Session.cpp:471`, `docs/two-pc-test-windows.md:450` ("E drops a
  crate 3 m ahead"), `game-ui.md` (keys, checklist) and CLAUDE.md's key list (the last two are written by whoever
  lands stage 1, not by this design).
- **Why not a context rule** ("use if something is in reach, else a crate"): the client's prompt and the server's
  resolution can disagree for one frame at the edge of reach (different eye by a few mm, a door that just moved), and
  the result would be a surprise crate in a doorway. Two buttons can never be misread. The crate is a test and toy
  tool; Q next to E keeps it one finger away.
- The input stays 7 bytes (`buttons` is one byte with 3 bits left). A new button value is wire format:
  `kProtocolVersion` goes up by one (11 if networking stage 8 has not bumped it first; the two stages must agree on
  the order, see "Coordination").

<!-- source lines 166-187 -->

### The lead: per use, from what the server measures

- **Condition for no correction.** A `MechanismMove` made at server tick T reaches a client while its predicted tick
  is about T + RTT + `Target()` + 2..3 (Client.cpp:274-279: the client runs `ceil(rtt/dt) + 2 + Target()` past a
  snapshot that is already RTT/2 old). A player who walks into the leaf is not corrected while
  `RTT + clock target + 3 ticks <= lead`.
- **Decision:** the server computes the lead once per accepted use as
  `clamp(max over joined players of (ceil(RTT_p / dt) + max(margin_p, 0)) + 2, 4, 20)` ticks, where `margin_p` is the
  player's input margin the server already tracks for the clock (Server.cpp:874-879: how many ticks early its inputs
  arrive, i.e. its clock target plus jitter) and `RTT_p` the transport's smoothed round trip. Offline the lead is 2
  ticks. It is not wire format: `startTick` travels in the segment. The value is logged per use in the `Mechanisms:`
  line (min / max lead).
- Estimates: the owner's two-PC LAN (24 ms ping, `docs/two-pc-test-windows.md:42`) gives about 2 + 3 + 2 = 7-8 ticks
  (125 ms); a 100 ms internet link with jitter about 12-14 ticks; the clamp at 20 (333 ms) covers about 230 ms RTT
  with a steady clock. Beyond that the players touching the mechanism are corrected once, as for any late event.
- **Why not a fixed 15 ticks (the first version):** 250 ms is about twice what the owner's LAN needs and covers only
  about 150 ms RTT on a steady link (not 180 ms as first claimed: the clock target was left out), and offline has no
  latency to hide. **Why not per client:** the timetable is shared; one lead per use, the worst joined player's, is
  the simple rule. One slow player makes doors 0.1-0.2 s slower for everyone, never wrong.
- **Determinism:** `sim::ApplyUse` takes the lead as an argument; the server's RTT and margins on the simulated-clock
  loopback are themselves reproducible, so `Entities.OneScriptGivesTheSameBodiesTwice`-style tests stay bit for bit.

<!-- source lines 383-466 -->

### Replication

New wire values (all bump `kProtocolVersion`; `docs/notes/net.md:466-469` is the recipe):

| Value | Bytes (est.) | Content |
|---|---|---|
| `EntityKind::Mechanism` = 3 | `ObjectSpawn` (model as masks) + `MechanismDesc` 40-70 + state 1 + 6 + n x 11 | State = segment count n (1..8) + `modelOffset` (3 x int16) + every segment that has not ended before the spawn's (or baseline's) tick, oldest first (the sticky flags ride in the segments). A door is 200-400 B, a portcullis 0.5-1.5 KiB (est., by model size) |
| `EntityKind::Rope` = 4 | 32 | Rope desc: anchors (world voxel, or NetId + model point), length (mm), material, flags (`Load`, `Chain`, `Cut`, cut fraction) |
| Event `MechanismMove` = 37 | 16 | event kind + `netId` + `MechanismSegment` (11) |
| Event `RopeCut` = 38 | 12 | event kind + `ropeId`, tick, cut fraction (uint16), flags |
| Owner block `lastUseTick`, `useResult` | +5 per snapshot to the owner | As `lastCastTick` / `castRejected` |
| `UseResult` 0-6, `MechanismKind` 1-6 | - | Values are wire format |
| `PlayerButton_Use` = 16 | 0 | A bit in the input byte |
| `EntityList` | unchanged | A mechanism's `modelHash` entry is `VolumeHash ^ MechanismTerm(...)` (below): no layout change |

**Clients keep mechanisms apart from copies.** The stage-6 copy code is driven by rest / wake events and samples,
which a mechanism never gets, and it sets layers itself (`MakeCopy` from `resting`, `MoveCopies` to Rubble every tick
with zero velocity, an `ObjectModel` re-making a survivor moving until a wake / rest that never comes). Left as is, a
door would be Rubble on every client after its first dent while it is solid on the server: different collisions at
the same opSeq.
- `Client` gets a map `mechanisms` (NetId -> entity, desc, ring, `modelOffset`), counted with `copies` for
  EntityCheck. `MoveCopies`, `bodies.Rest` / `bodies.Add`, the snapshot samples and the `EntityRest` / `EntityWake`
  handlers ignore it.
- A `Mechanism` spawn, a `BaselineEntity` of kind `Mechanism` and an `ObjectModel` for a mechanism NetId re-make the
  kinematic body and then call `sim::ApplyMechanismLayer(entity, tick)` (the timetable layer at the client's
  predicted tick). Events for a NetId the client lacks go through the existing `Hold` path.
- `MechanismMove{Freed}` moves the entity from `mechanisms` to `copies` as a moving copy; from then the server's
  `EntityWake` / samples / `EntityRest` drive it as any stage-6 body.

**The server tracks a mechanism as resting.**
- `DescribeObject` sets `kObjectResting` from `sim::IsAwake(physics, body)`, and `CreateBody` adds a kinematic body
  with `EActivation::Activate` (PW.cpp:697): a spawned door would get `tracks[netId].resting = false` and be sampled
  in every snapshot for good. Decision: a mechanism spawn writes `tracks[netId] = {resting = true, rest pose = spawn
  pose}`.
- `TrackBodies`, `AwakeSamples`, `CountPieces` and the 64-awake count test the `scene::Mechanism` component (not
  `IsAwake`, which has no entity) and skip a mechanism that is not `Freed`. So mechanisms never take an awake slot or
  a snapshot byte.
- On `Freed` the existing resting track makes `TrackBodies` emit `EntityWake` on its first motion, as for any body.
- After every `Scene::Damage` of a mechanism the blast re-applies Kinematic and the timetable layer on the server
  (Damage makes the survivor with `CreateBody`'s size layer, Scene.cpp:66-69, even mid-swing).
- Test: `Mechanism.DentedDoorKeepsItsLayerOnEverySide` (dents mid-swing and at rest; the server's and a client's
  layer agree at every tick).

**The entity byte budget.** The budget is `entityEventBytesPerTick` 200 = **12 kB/s** with a 16 KiB burst (the first
version compared against the 24 kB/s terrain event estimate, which is the other budget). Today, over budget, a
damaged object's survivor is despawned `ToRubble` instead of getting its `ObjectModel` (Server.cpp:427-432): an
attacker could spend the credit with casts elsewhere and delete a gate with one hit.
- **Decision:** `EmitObjectEvents` never sends an entity with a non-freed `scene::Mechanism` down the `ToRubble` path.
  Over budget it still gets its `ObjectModel`, charged to `entityCredit` (which may fall to -8 x burst,
  Server.cpp:1939), counted in `stats.mechanismModelsOverBudget`. Only while the credit is at that floor does
  `sim::Blast` skip `Damage` on mechanisms (the cast still runs; the gate takes no dent that tick), counted in
  `mechanismHitsSkipped`. Why both: never deleting a gate is the fun rule; the floor bounds the bytes a sustained
  gate bombardment can push past the budget (est. never reached: a gate under fire is 3-10 kB/s, below).
- `MechanismMove` and `RopeCut` are entity events: charged to `entityCredit`, **never refused or delayed** (they are
  state, not pictures). At est. 8-16 B/s per client in a siege they are under 0.2 % of the budget.
- Stage 3's gate drill fails if `modelsToRubble` ever counts a mechanism.

**Despawns cut ropes.** A load or mechanism NetId also goes by `ToRubble` (budget, eviction `Replication.cpp:122-127`),
`Broken` (Blast.cpp:641-646), falling out of the world, or a lever cut apart. The server's `on_destroy<NetId>`
observer (the one that makes `EntityDespawn`) also emits `RopeCut{ropeId, tick, fraction = 1.0 at that end}` for every
rope with an end on that NetId, in the same tick and before the `EntityDespawn`; a `Freed` mechanism does the same for
its chains. Clients treat a rope end on a missing or tombstoned NetId as cut. Test: `Rope.LoadDespawnCutsItsRope`
(`ToRubble` and eviction).

**Joiners:** a mechanism's `BaselineEntity` carries its spawn with its state: `modelOffset`, flags and every segment
that has not ended before the baseline's tick (not "before `asOfOpSeq`": an opSeq is not a tick), 1..8 of them. A
joiner whose clock is before a pending segment's `startTick` derives the same `p` for every tick
(`Mechanisms.LateJoinerDerivesTheSamePosition`, with two pending segments). Ropes carry their cut flag. A pristine
region's mechanisms cost their spawn bytes in the baseline (est. 64 mechanisms x 400 B = 25 KiB; a fort more, see
budgets). Deriving pristine mechanisms from a plan with 0 join bytes is left for the fort generator (open question 8).

**Checks (7b):** `MechanismTerm(ring head, flags, modelOffset)` is an integer hash of the newest segment the client
has applied at the check's opSeq, the sticky flags and `modelOffset`. A mechanism's `EntityCheck` model term is
`EntityModelTerm(netId, modelHash ^ MechanismTerm)`, and its `EntityList` entry's `modelHash` is `VolumeHash ^
MechanismTerm`. A client whose segment state differs (a lost or misapplied `MechanismMove`) therefore sees a wrong
model in the list and sends `ModelRequest`, as 7b does for any wrong body; the answer is a `BaselineEntity` of kind
`Mechanism` carrying the segments, which re-makes the body (7b's path, unchanged), counted as `mechanismsRepaired`.
`EntityListEntry` does not change. A rope's term is `EntityModelTerm(id, RopeTerm(desc, cut))`. Test:
`Mechanisms.EntityCheckCoversTheSegment` (a dropped `MechanismMove` is a mismatch and is repaired through
`ModelRequest`).

**Bytes in play (est.):** a door use 16 B to each client; a lever with one gate 32 B. A siege of 5 v 5 with a door or
gate change every 2 s: 8-16 B/s per client, about 0.1 % of the 12 kB/s entity budget.

<!-- source lines 780-812 -->

## Stages

Each ends with green tests, the headless runs, a fresh-eyes review, ROADMAP.md updated and a commit in both repos.
Start after networking stage 8 is committed (both bump the protocol and touch `PlayerButton`).

| Stage | Content | Acceptance | Est. |
|---|---|---|---|
| **1. Door and gate** (the smallest fun slice). M-a offline, M-b session | M-a: `scene::Mechanism` (with `modelOffset`), `sim` segment (`durationTicks`, two `uint32` ease tables) / pose / layer rule (ghost and rising) / `FindUseTarget` (occluders, info targets, authoring rule) / `ApplyUse` (closing wins, lead argument) / `PoseMechanisms` / `StepMechanisms` (crush) / arrival push-out; kinds Door, Portcullis, Lever; the damage rule (aimed hits, 0.3 m, hardness, no terrain carve, hinged part kept, Warded) and freed / broken; `PlayerButton_Use`, crate on Q (every file in the list above); HUD prompt, info lines, lever throw and lead cue; the yard in the default offline game (`--no-mechanisms`; scripts' numbers confirmed or re-recorded). M-b: `EntityKind::Mechanism`, `MechanismMove`, owner block, `PlayerUse` on both paths, the per-use lead, the client `mechanisms` map, server resting tracks, the entity-budget rule, baseline with segments, EntityCheck / EntityList term, the yard in `playground.session`, `--bot-use` | All stage-1 tests; the yard headless run and `--mechanism-blast` exit 0 with their lines (hits to free each door recorded) and a screenshot with the door open and the gate up; the default game run's three lines; the lossless and lossy drills as above; `tools/session-check.sh` still PASS; the owner's checklist in `game-ui.md` gains "E opens the yard door, the lever runs the gate, Q drops a crate" (windowed: the owner's) | 9-10 days (M-a 5, M-b 4-5) |
| **2. Ropes** | Constraint wrapper; load ropes (a cage and a chandelier in the yard); `EntityKind::Rope`, `RopeCut` by ray, by sphere, by anchor and by despawn; the portcullis chain drawn, and **its cut drops and jams the gate**; `GatherRopes` instances and the link models (a `rope` material: `MaterialTableHash` and `buildHash` change); cut halves; `--rope-stress`. **Last, cut first if the stage overruns:** decorative strands with wind and sleep (`fx::RopeStrand`, the 24-strand cap) | Rope tests; `--rope-script` exits 0; rope GPU and BVH cost measured at `--rope-stress 26` with and without `--no-ropes` and within budget; the drill with `--bot-casts` cutting ropes ends `hashes equal` and `agree` | 6-7 days |
| **3. Siege kit** | Winch (hold, crew once per 12 ticks, release hysteresis, HUD ring), drawbridge (riders fall when it rises after the lead cue), both drawbridge chains cut drop it, Button, auto-return sally door, `Locked` from the session file; gate-under-fire bytes measured | Tests; a yard gatehouse drill with bots holding the winch, cutting chains and shooting the gate: `modelsToRubble` counts no mechanism, event bytes per hit recorded | 4-5 days |
| **4. Placement** | The watch tower door in the default playground (tower / neck scripts re-measured and their CLAUDE.md numbers updated); worldgen W3 gatehouse kit sockets and keep loads from plan indices | Default runs unchanged except the recorded numbers; a generated fort with working gates in a session | 3 days + W3 |
| **5. Later (after M2)** | Climb volumes for ropes and ladders; team locks (`ownerTeam`); crush damage to players; warding release; a player rope tool if still wanted | Decided then | - |

Total for stages 1-4 without W3: est. 22-25 days (the first version said 19-21; the review's rules cost about 3-4
days, mostly in stage 1).

## Coordination with work in progress

- **Networking stage 8** (in progress): owns the next protocol bump; this design takes the one after. `PlayerButton`
  values, new event kinds (37, 38), `EntityKind` 3-4, `UseResult` and the owner block size are wire format: whoever
  lands second renumbers nothing it does not have to, and bumps. If stage 8 grows the owner block, the +5 bytes go
  after its fields.
- **shred.md SI-1..3**: they promise no wire change; this design does change the wire, so it lands after them or in
  a separate commit. SI-1 must keep skipping Kinematic objects (`Mechanism.NotJudgedByTheObjectLoadCheck`); SI-2 must
  exclude kinematic bodies from impacts. The new `Scene::Damage` overload must not change ordinary objects' damage
  (shred.md's logged numbers).
- **worldgen.md W3**: the gatehouse kit's sockets (stage 4) follow the authoring rule (levers behind their targets);
  "an open gate" as a per-session template op stays valid for gates that are only holes.
- **game-ui.md, CLAUDE.md, docs/two-pc-test-windows.md:450, src/main.cpp:415 / :453, src/Session.cpp:240-241 / :412 /
  :471**: keys (E use, Q crate), the prompt, the owner's checklist items; all changed in stage 1 M-a.
- **Default world:** the yard is in the default game and `playground.session` from stage 1; every scripted number in
  CLAUDE.md is confirmed or re-recorded in the same commit.
