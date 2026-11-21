# Mechanisms, the use key and ropes

[[../../../00 Docs Map|Docs map]] · [[../README|docs index]] · [[../ROADMAP|roadmap]] · [[../ARCHITECTURE|architecture]] · [[networking|networking]] · [[shred|shred]] · [[game-ui|game UI]] · [[worldgen|worldgen]]

| | |
|---|---|
| Status | Designed, nothing built or measured for it. |
| Decided | - Owner's request (2026-11-17): "in game should be implemented rope with physics, in game action which moves objects such as door opening by clicking E on them or activating lever (for castle gates)".<br>- Us (this design, 2026-11-17), amended the same day after two critics' review: 27 findings, two pairs of them the same, so 25 distinct: 24 applied, two of those with a change, and 1 rejected in part ("Decision in brief").<br>- Us, placement in ROADMAP under the owner's standing rules (the owner may veto): stage 1 comes after the fight measurement (review WP8), so its yard is measured inside a known fight budget, and takes the protocol number after M2a's, because it changes the E key and adds a `PlayerButton`. |
| Next | [mech-1] door and gate (M-a offline, M-b session), in ROADMAP "Next" after WP8. [mech-2]..[mech-4] in ROADMAP "Later"; [mech-5] after M2. |
| Open | Questions 1-15 in "Open questions" (each with the recommended answer; 1 and the fixed-or-adaptive part of 2 are settled). |
| History | [design-review-2026-11-17.md](../history/mechanisms/design-review-2026-11-17.md): "Review of the design (2026-11-17)", the findings and where each was applied (cited in the text as "Review")<br>[before-rewrite-2026-11-18.md](../history/mechanisms/before-rewrite-2026-11-18.md): the original header and the sections this rewrite changed |
| Marks | "Measured" = a number from `docs/ROADMAP.md` or a log; "est." or (e) = an estimate, replaced by a measurement in the stage named under "Stages". "Code" = read from the working tree of 2026-11-17 on engine `4855f85` / game `ad0fe69`, with networking stage 8 uncommitted in it (`src/net` line numbers may differ a little from `e21a84d`, which committed this design, and from `430fde0`); `file:line` references are to that tree and are not updated (`PW.cpp` = `src/physics/internal/PhysicsWorld.cpp`, `Server.cpp` / `Client.cpp` = `src/net/internal/`). |

Related: [networking.md](networking.md) (stage 6 entity replication, 7a baseline, 7b checks: this design adds to all
three), [shred.md](shred.md) (destruction; the structural integrity plan SI-1..3), [worldgen.md](worldgen.md) (forts,
gatehouses, plans), [game-ui.md](game-ui.md) (HUD, keys), the game's `docs/owner-checks.md` (the owner's checklist)
and `docs/world/setting.md` (forts, sieges, "Pass wall", "Hold the crossing", warded keeps).

## Decision in brief

One rule, the same as networking's: **the server decides; a client either repeats an integer computation from the
server's data, or receives the result.** Mechanisms and ropes add no new kind of authority, only new data.

| Thing | What it is | Who decides | What travels | Players meet it |
|---|---|---|---|---|
| Door, portcullis, drawbridge, lever, button, winch | A **mechanism**: a kinematic replicated voxel object whose travel position is an integer function of (motion segment, tick) | Server, on a Use edge (or a link, a timer, a cut) | `MechanismMove` (16 B) per change; nothing in snapshots | At rest: solid. Moving: a ghost (doors, drops, drawbridges), except a gate rising out of its passage, which stays solid |
| The use key | E = new `PlayerButton_Use = 16` (wire format, protocol bump). The crate moves to **Q** | Server, at the input's tick, with the same function the client uses for its prompt | One bit per input; the result in the snapshot owner block | - |
| Load rope (cage, chandelier, counterweight) | **One** Jolt `DistanceConstraint` (max length only) between a world anchor and a replicated load; no link bodies | Server physics; the load is an ordinary stage-6 body | Rope spawn (32 B, est.), `RopeCut` (12 B) | The load: as any stage-6 body |
| Mechanism chain (portcullis, drawbridge) | A rope whose ends follow mechanism poses: no physics at all | Server (cuts only) | Rope spawn, `RopeCut` | Never (drawing only) |
| Decorative rope, sway, wind, cut halves | Client-side Verlet strand (`vortex.fx`), drawn as instances of one shared link model | Each client, for itself | Nothing (the rope's spawn and cut are the server's) | Never |
| Rope cut | A cast's ray passing a rope, the cast sphere meeting it, an anchor voxel turned air, or its load or mechanism despawned | Server | `RopeCut` | - |

- **Fun first, few bugs:** mechanisms are scripted, not simulated: a door cannot get stuck, cannot be pushed into a
  wall, behaves the same offline and online, and costs no snapshot bytes. Physics is used where it is visible and
  cheap: loads swing on one constraint and fall when cut, ropes sway and dangle on every client.
- **Lead:** every mechanism motion starts a few ticks after the use is resolved. The server picks the lead per use
  from what it measures of its clients (4-20 ticks; about 8 on the owner's 24 ms LAN, est.); offline it is 2 ticks.
  No correction happens while a client's round trip + clock target + 3 ticks fits in the lead. The press shows at
  once on the HUD, in the lever's handle and as a short "unlatching" rattle; the motion itself eases out (fast start).
- **Visible from the first stage:** the yard (a hut with a door, a wall with a portcullis and a lever) is part of the
  default offline game from stage 1 M-a and of `playground.session` from M-b, sited clear of every scripted area.
- **Stage 1 (est. 9-10 days):** E opens a hinged door and a lever raises and drops a portcullis, offline and in a
  session, with the damage rule (doors wear down hit by hit). Then ropes and the chain cut that jams a gate (stage 2),
  then winches, drawbridges, buttons (stage 3), then placement in the watch tower and the fort generator (stage 4).
  A player rope tool waits for M2 (stage 5, decided later).

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
| Drawing thin things | Instances with a full `mat4` (`voxel/Instances.cppm:20-27`); non-uniform scale traces correctly (`trace.glsl:366-369`); instances are in the shadow / AO BVH, rebuilt on the CPU every frame over all instances (`Renderer.cpp:1185-1209`) | No rope primitive; particles are billboards without shadows (`docs/notes/graphics.md` "Traps") |
| HUD | `HudCanvas` Text / Rect / Crosshair (`core/Hud.cppm:115-143`), the HUD costs 0.01 ms (measured) | No prompt |
| Sound | No audio module (`engine/src`: core, editor, engine, fx, graphics, math, net, physics, scene, sim, voxel) | Feedback is pictures only |

## Interaction: the use key

### E becomes "use", the crate moves to Q

- **Decision:** `PlayerButton_Use = 16`, `kPlayerButtonMask` 31. E is Use in play and in a session; the crate keeps
  `PlayerButton_Crate = 8` (its wire value does not change) and moves to **Q** (free: the game reads W A S D, Space,
  Shift, Ctrl, E, F, V, F1, F3, F5, F12, Esc, Left Alt today). The HUD's tool line becomes `Blast [LMB]   Use [E]
  Crate [Q]`.
- **Every place that names E for the crate changes in stage 1 M-a** (one list, so the owner's checklists match the
  build): offline key `src/main.cpp:415`, offline tool line `src/main.cpp:453`, session latch `src/Session.cpp:240-241`
  and the scripted latch `:412`, session tool line `src/Session.cpp:471`, the game's `docs/two-pc-test-windows.md`
  (checklist 6.3: "E drops a crate 3 m ahead") and `docs/owner-checks.md` (check 14), `game-ui.md` (keys) and
  CLAUDE.md's key list (the last four are written by whoever lands stage 1, not by this design).
- **Why not a context rule** ("use if something is in reach, else a crate"): the client's prompt and the server's
  resolution can disagree for one frame at the edge of reach (different eye by a few mm, a door that just moved), and
  the result would be a surprise crate in a doorway. Two buttons can never be misread. The crate is a test and toy
  tool; Q next to E keeps it one finger away.
- The input stays 7 bytes (`buttons` is one byte with 3 bits left). A new button value is wire format:
  `kProtocolVersion` goes up by one: the number after M2a's (see "Coordination").

### The look ray and the target

One GPU-free function in `vortex.sim`, used by the client every frame (prompt) and by the server on a Use edge:

```
UseTarget FindUseTarget(const World&, const MechanismView&, glm::vec3 eye, glm::vec3 dir, uint32_t tick)
// UseTarget: { netId, kind: None | Usable | Info, distance }
```

1. **Reach:** `kUseReach` = 2.5 m from the eye (est.: a door handle from a standing player at arm's length plus a
   little forgiveness; tuned in stage 1).
2. **Terrain occludes:** `World::Raycast(eye, dir, kUseReach)`; every target must be nearer than the terrain hit.
3. **Every mechanism occludes, usable or not:** for each mechanism whose swept bounds (its box over its whole travel,
   precomputed) meet the reach segment, the ray is tested against its **block box** (the authored solid box of the
   leaf, bars or deck, model space) placed at its pose **at `tick`**. A closed portcullis or gate leaf blocks a use
   through it, bars included (a cast may pass between bars; a use never does: the bars are one box). A raised gate's
   box is out of the passage and blocks nothing there.
4. **Usable candidates:** usable mechanisms' **use boxes** (the door leaf, the lever handle, the button face; model
   space) at the pose at `tick`, inflated by 0.1 m (est.: thin handles stay easy to hit); ray against oriented box.
   A candidate counts only if nearer than the terrain hit and nearer than every **other** mechanism's block box (a
   door's own block box does not hide its own use box). The nearest wins; ties by the lower NetId.
5. **Info targets:** when no usable candidate wins and the nearest block box hit within reach belongs to a
   non-usable mechanism (a portcullis, a lever-driven gate leaf, a drawbridge), it is returned as an `Info` target:
   the prompt shows greyed `Gate - opened by a lever` / `Drawbridge - winch` and E does nothing (`NotUsable`).
6. **No physics raycast:** use and block boxes are authored mechanism data; the body's size, layer and dents do not
   matter for a use. Bodies other than mechanisms (a crate in front of a lever) do not block a use: forgiving, and one
   less source of disagreement.
7. **Hysteresis (client prompt only):** the last target stays while the ray still hits its box inflated by 0.2 m,
   so the prompt does not flicker between two levers.

Model-space boxes are read through the mechanism's `modelOffset` (see "The pose after a dent").
Cost: one terrain DDA of 20 voxels and a handful of box tests: under 5 us per call (est.).

**Authoring rule (checked when a template or plan is loaded, a load error otherwise):** the use box of a lever or
winch is at least `kUseReach` + 0.5 m from the outer face plane of every target (the face away from the lever), or
the straight line from every point of that outer face to the use box meets terrain. Together with step 3 this keeps
defenders' levers out of attackers' reach through a closed gate, a murder hole or around the gate's edge (est. 3 m).

### What the server does with a Use edge (resolved at the input's tick)

`Server::PlayerUse(slot, input, at)` beside `PlayerCrate`, called from the same two places (the step's edge,
Server.cpp:929, and the late-input path, :851):

1. **Eye and look at the input's tick.** On the step's edge `at` is the state after the step. On the late-input path
   it is `pl.StateAt(in.tick)` as `LateCast` does (not `pl.state`, which is what the crate takes today): a late Use
   must not pair today's eye with mechanism poses from up to 30 ticks ago. `at == nullptr` or `now - in.tick >
   kUseStaleTicks` (30) is `Stale`; `static_assert(kUseStaleTicks < kStateHistory)` keeps the rewind inside the ring.
2. **Mechanism poses at the input's tick:** every mechanism keeps a ring of its last `kMechanismSegments` = 8 motion
   segments, so its position at any tick in the stale window is exact (see "The model"). This is better than casts
   get for terrain: a moving door is aimed at where the player saw it. Terrain is taken as it is now, as for casts.
3. `FindUseTarget`. Then the refusals, in order (`UseResult`, wire format, in the owner block):

| `UseResult` | When |
|---|---|
| `None` = 0 | Accepted (or no use yet) |
| `Stale` = 1 | The input's tick is more than 30 ticks behind the server, or the state at it is no longer kept |
| `NoTarget` = 2 | Nothing usable in reach at that tick (the HUD says `Out of reach`) |
| `Locked` = 3 | The mechanism (or the lever's target) carries `Locked` or `Jammed` |
| `Busy` = 4 | The spam guard or the "closing wins" rule below |
| `Broken` = 5 | Freed (fell off its hinges) or its lever was destroyed |
| `NotUsable` = 6 | The target is an `Info` target (a gate driven by a lever); the HUD shows the info line again |

4. **Busy, and closing wins.** Each mechanism records the tick of its last decision and the direction of its current
   segment (opening or closing; for a lever, its targets' direction; for a door, towards 0 is closing).
   - Any use is refused while the mechanism, or any target of the lever, was decided less than
     `kMechanismBusyTicks` = 12 ticks ago.
   - A **close** may interrupt an opening segment once those 12 ticks have passed.
   - An **open** is refused while a closing segment runs and until `kMechanismReopenTicks` = 20 ticks (est.) after it
     arrives.
   - Why: with "moving is walk-through" a spammer or a tug-of-war could otherwise keep a door or gate moving, and so
     passable, almost all the time, and shake it on every screen at 16 B a reversal. Now a contested door or gate
     always rests solid for at least about 0.3 s between swings, and a defender's emergency drop waits at most 0.2 s.
5. **Accept:** the new motion segment(s) start at `startTick = serverTick + lead` (below), from the position the
   current segment gives at that tick (a reversal mid-swing is continuous). A lever makes its own segment and one per
   target, all with the same `startTick`. Each segment goes out as a `MechanismMove` event through the same emit
   path as every other event (in the opSeq stream).
6. The owner block of the user's snapshots gets `lastUseTick` (the input's tick) and `useResult`, as casts do
   (`kSnapshotOwnerBytes` 32 -> 37): repeated in every snapshot, so a lost one is never a lost refusal.

Offline the game calls the same `sim::ApplyUse(sim, user eye, look, tick, lead)` from its fixed update on the E edge
(where `src/main.cpp:415` drops the crate today); offline has no refusal path other than the HUD text.

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
- Estimates: the owner's two-PC LAN (24 ms ping, the two-PC guide's results of 2026-11-17) gives about 2 + 3 + 2 = 7-8 ticks
  (125 ms); a 100 ms internet link with jitter about 12-14 ticks; the clamp at 20 (333 ms) covers about 230 ms RTT
  with a steady clock. Beyond that the players touching the mechanism are corrected once, as for any late event.
- **Why not a fixed 15 ticks (the first version):** 250 ms is about twice what the owner's LAN needs and covers only
  about 150 ms RTT on a steady link (not 180 ms as first claimed: the clock target was left out), and offline has no
  latency to hide. **Why not per client:** the timetable is shared; one lead per use, the worst joined player's, is
  the simple rule. One slow player makes doors 0.1-0.2 s slower for everyone, never wrong.
- **Determinism:** `sim::ApplyUse` takes the lead as an argument; the server's RTT and margins on the simulated-clock
  loopback are themselves reproducible, so `Entities.OneScriptGivesTheSameBodiesTwice`-style tests stay bit for bit.

### Press and hold

- **Press (an edge)** for doors, levers and buttons: responsive, and the edge rule already exists
  (`PlayerState::lastButtons`).
- **Hold (the button level)** for a winch (stage 3): the server evaluates the crew of a winch at most once every
  `kMechanismBusyTicks`: the players holding E whose look ray hits the winch's use box inflated by 0.2 m (the prompt's
  hysteresis, so a wobbling look does not drop out), at most 3. A release counts only after E has been up (or the ray
  off the box) for 12 ticks. When the crew count changes, the server emits a new segment whose speed is
  `1 + crew / 2` times the solo speed (est.: 9 s solo for a 3 m portcullis, 4.5 s with three); crew 0 emits a stop
  (`durationTicks` 0 at the current position). So a winch makes at most one `MechanismMove` per 12 ticks, whatever a
  client sends (`Mechanism.WinchToggleSpamMakesAtMostOneSegmentPerBusyPeriod`). The client draws a progress ring from
  the segments it gets, not from its own key (no prediction of shared state).
- A "hold as toggle" accessibility option is a later setting (`game-ui.md`), not a wire change.

### What the predicting client shows at once

- It never predicts a mechanism's state (it is shared; two players can use one gate in the same tick).
- On the Use edge with a usable target in the prompt: the crosshair pulses, the prompt changes to the verb in
  progress (`Opening...`), the **lead cue** starts (below), and **a lever's handle is drawn thrown at once**. That is a
  picture only: the lever's body, and so its collision, stays at the timetable pose; a handle under 0.3 m drawn ahead
  for a few ticks is accepted. When the lever's `MechanismMove` arrives the drawn handle follows the real segment
  (whichever is further along); when the owner block reports a refusal for that input tick, the handle and the cue
  snap back and the prompt shows the reason for 1 s (`Locked`, `Busy`, `Out of reach`, `Broken`).
- **The lead cue (pictures only, every client):** from a `MechanismMove`'s arrival until its `startTick`, the
  mechanism's leaf rattles by 1-2 deg (a door), its chains draw taut and a dust puff shows at the hinge or the slot (a
  gate). The user's own client starts the cue at the press. It covers the lead (there is no sound) and warns
  drawbridge riders before the deck goes.
- **Ease:** doors, levers, buttons, drops and lowering use an **ease-out** table (fast start, soft stop: about 25 % of
  the swing in the first 15 % of its time, est.), so motion is visible within a tick or two of `startTick`. Slow
  raises (a portcullis or drawbridge going up, a winch) use smoothstep. Which table is a function of the kind and the
  segment's direction: no wire byte.

### The prompt on the HUD

`HudCanvas` only (never ImGui). Under the crosshair, one line: `[E] Open`, `[E] Close`, `[E] Raise gate`, `[E] Lower
gate`, `[E] Press`, `Hold [E] Raise 40%`, greyed `Locked` / `Jammed` / `Broken`, greyed info lines (`Gate - opened by
a lever`). The verb comes from the mechanism's kind and the direction its next segment would take. When a lever's
target is out of view, the HUD shows `Gate raising` / `Gate dropping` for 2 s after the press. A few quads: the HUD's
0.01 ms (measured) does not move measurably (est.).

## Mechanisms

### The model: an integer travel position on a timetable

A mechanism is a voxel object with a **travel position** `p` (int16) between its `minPos` and `maxPos`, and a ring of
`kMechanismSegments` = 8 **motion segments**:

```
struct MechanismSegment {          // wire format (MechanismMove): 4 + 2 + 2 + 2 + 1 = 11 B
    uint32_t startTick;            // server tick the motion starts
    int16_t  startPos;             // p at startTick (the previous segment's p there)
    int16_t  targetPos;            // equal to startPos when durationTicks == 0
    uint16_t durationTicks;        // D: ticks from startPos to targetPos; 0 = at startPos, stopped
    uint8_t  flags;                // Locked, Jammed, Freed, Broken (sticky ones stay set)
};
```

- **The duration** is computed once by the server, in integers, when it makes the segment:
  `D = ceil(|target - start| * kind.fullTravelTicks / (maxPos - minPos))`, where `fullTravelTicks` is the desc's
  open or close time for the whole range (0.6 s door = 36 ticks per 1024 units, a lever 12 ticks per 512, a winch
  segment its crew-scaled time). The first version's `ticksPerUnit16` could not express a door (0.5625 ticks per 16
  units) or a lever (0.375) as an integer and left its fixed-point scale undefined.
- **`PositionAt(ring, tick)`:** the newest segment with `startTick <= tick` (the spawn always carries one, so there is
  always one); `k = tick - startTick`. If `k >= D`: `targetPos` exactly (this covers `D == 0`). Otherwise
  `e = Ease(table, k, D)` and `p = start + (target - start) * e / 65536` (int64 intermediate, truncation toward zero).
  `Ease`: a 65-entry **`uint32`** table with `T[0] = 0`, `T[64] = 65536`; `f = k * 64`, `i = f / D`, `r = f % D`,
  `e = T[i] + (T[i+1] - T[i]) * r / D` (int64). Since `k < D` there, `i < 64`. Integer only: the same `p` on Linux and
  Windows, and the end is reached exactly at `startTick + D` (the first version's `uint16` table topped out at 65535
  and never reached `targetPos`, so nothing ever came to rest).
- **Pose** (see "The pose after a dent" for `modelOffset`):
  `pose(p) = spawnPose x kindTransform(p, pivot) x translate(modelOffset x voxelSize)`. A hinge turns about an
  authored pivot (model space, 1/16 voxel) around one of the six model axes by `p / 4096` turns (90 deg = 1024); a
  slide moves along one model axis by `p` sixteenths of a voxel. The float pose may differ by an ulp between libms;
  nothing compares it, only `p`, the flags and `modelOffset` are checked (EntityCheck below).
- **Arrival** = `startTick + D` of the newest started segment; **at rest** from its arrival tick until a newer segment
  starts; **moving** in `[startTick, startTick + D)`.
- `autoReturnTicks` (desc): after arriving at `maxPos`, a mechanism with it returns to `minPos` on its own at
  `arrival + autoReturnTicks`. It is part of the timetable, so it costs no event: a button springs back, a sally door
  shuts after 10 s.
- **The ring holds enough:** `kMechanismSegments` >= ceil(maxLead / busy) + ceil(stale / busy) + 1 = 2 + 3 + 1 = 6
  (`static_assert`), so every segment that may still start (up to the 20-tick lead) and every one a use in the
  30-tick stale window may be resolved against are kept. The ring evicts the oldest segment that ended before
  `serverTick - kUseStaleTicks`.

Kinds (wire format, `MechanismKind`):

| Kind | Motion | Range | Speed (est.) | Notes |
|---|---|---|---|---|
| `Door` = 1 | hinge, vertical axis | -1024 .. +1024 (both ways) or 0 .. 1024 (`OneWay`) | 0.6 s per 90 deg | Opens **away from the user** (the side of the door plane the user's eye is on, decided by the server at the input's tick): it never swings into the person opening it. Fort doors are authored `OneWay` (they open inward, against a rebate) |
| `Portcullis` = 2 | slide up | 0 .. height | raise 3 s by lever (stage 1), 9 s solo by winch (stage 3); drop 0.75 s | Linked from a lever or a winch, not usable directly (an `Info` target) |
| `Lever` = 3 | hinge | 0 .. 512 (45 deg throw) | 0.2 s | Toggles; `targets` (up to 4 NetIds) get the matching direction |
| `Button` = 4 | slide in | 0 .. 2 voxels | 0.1 s | `autoReturnTicks` 30; drives `targets` to `maxPos` (or toggles them, `Toggle` flag) |
| `Drawbridge` = 5 | hinge, horizontal axis | 0 (down) .. 1024 (up) | lower 4 s, raise 6 s | Stage 3; driven by a winch or lever (an `Info` target) |
| `Winch` = 6 | hinge (the drum turns with its target) | - | - | Stage 3; a hold device; its drum is drawn from its target's `p` |

`MechanismDesc` (wire, in the spawn; est. 40-70 B): kind, flags (`OneWay`, `Usable`, `Toggle`, `Locked`, `Warded`),
pivot (3 x int16, 1/16 voxel, model space), axis (0-5), minPos, maxPos, full-travel ticks (open, close),
`autoReturnTicks`, use box and block box (6 x uint8 voxels each, model space), push axis (0-5), up to 2 world anchor
voxels (3 x int32 each: the terrain voxels the hinge rests in), hinge box (model-space voxels that hold the hinge, see
"Destruction"), targets (count + NetIds).

### The pose after a dent

`Scene::Damage` re-places the kept piece at `source.TransformPoint(keptOffset)` (Scene.cpp:586-602) and clients shift
their copy by `rotation * offset * voxelSize` (Client.cpp:832-850). Every model-space field of the desc (pivot, use
box, block box, hinge box, push box, rope attach points) was authored against the original model.

- **Decision:** `scene::Mechanism` stores `glm::ivec3 modelOffset` (voxels, the sum of every `keptOffset`), on the
  server updated from `DamageResult::keptOffset`, on clients from `ObjectModel::offset`; the spawn and the baseline
  carry it (3 x int16). Every model-space desc field is read as `field - modelOffset`, and the pose formula above
  carries `translate(modelOffset x voxelSize)`, so a door cropped at its minimum corner still turns about the same
  world hinge line. `modelOffset` is part of `MechanismTerm`, so a disagreement shows in EntityCheck.
- Test: `Mechanism.DentThatCropsTheModelKeepsThePivot` (a dent removes the minimum-corner column; the world pose at
  every `p` is unchanged within 1 mm on the server and on a client copy).

### Linked mechanisms

A lever or button is an input with its own small travel; using it schedules its targets in the same tick (one
`MechanismMove` per target, same `startTick`). A lever's state is its position (up / down), and each target goes to
the end that matches: lever up -> gate raised, down -> dropped. If a target is freed or broken the lever still throws
and nothing else happens (the prompt says `Jammed` once the server refuses). Links are by NetId, set at spawn from the
template's indices; a link to a despawned NetId is dropped. Chains of links (a lever driving a button driving a gate)
are not allowed: one hop keeps "what moves when I pull this" obvious and the work bounded.

### Collision with players: at rest solid; moving a ghost unless it rises out of the passage

- **At rest:** `Layer::Dynamic` (session characters collide with Static | Dynamic, `sim::kSessionCharacterLayers`).
  A closed door blocks, an open one is a wall you walk along, a lowered drawbridge carries you.
- **Moving, the ghost rule:** doors, levers, buttons, drawbridges (both ways) and anything moving **into** a passage
  (a dropping portcullis) are in `Layer::Rubble` (never meets a character) while they move. A swinging door cannot
  push a player into a wall, cannot be held half-open by a player (the "door stuck" class of bugs), and cannot carry
  a player into a bad state.
- **Moving, the rising rule:** a slide kind moving **away** from its passage (a portcullis rising, by lever or by a
  winch crew) stays in `Layer::Dynamic` while it moves. A body that only moves up out of the passage can neither push
  a player sideways nor drop onto one, so the ghost rule's reasons do not apply; and a gate is then passable only
  where its real gap is taller than the capsule (about 60 % into a 3 m raise, est.). Without this, attackers would
  walk under the bars the tick a 3 s raise or a 9 s winch hold starts. Authoring keeps the top of a portcullis inside
  its slot, so nobody rides it up (if one does, `UpdateGroundVelocity` carries them on both sides from the same
  timetable velocity).
- **Why the stage-6 ghost rule could be relaxed here:** stage 6 needed ghosts because client copies are drawn in the
  past and their poses at the predicted tick are unknown. A mechanism's pose and layer are a function of the
  timetable at any tick on every side, so the layer is chosen by kind and direction without a wire change.
- **The carry exception of stage 6 is not taken:** a replicated body stays Dynamic while it carries a player, but for
  a mechanism that would need every client to know who stands on it. So a drawbridge that rises drops whoever stands
  on it into the ditch (the lead cue warns them first) - what real drawbridges were for, and a good siege moment
  (stage 3).
- **The layer is a function of the timetable** (`sim::MechanismLayer(desc, ring, tick)`), so it flips at the same
  tick on the server, on the predicting client (including replays) and offline. It always overrides `CreateBody`'s
  size rule: a mechanism is never in `Layer::Debris`, whatever its voxel count, so a small or dented lever stays
  hittable by casts (the session cast mask is Dynamic | Rubble, Server.cpp:776-778).
- **Landing on a player (push-out):** before every player step (the same place as
  `TerrainColliders::FlushBox(PlayerReach)`), both sides run `sim::PushOutOfMechanisms(state, tick)`, but only for
  mechanisms whose **arrival** tick (from the timetable, the same everywhere and in replays) is `tick` or `tick - 1`.
  If the player's capsule overlaps the mechanism's **posed solid voxels** (not its box: a portcullis grid is mostly
  air) deeper than the character padding plus one quantum (1/16 voxel), the feet move **along the push axis only** to
  the side of the mechanism's mid-plane they are on, to 2 cm outside. If that spot is solid terrain
  (`World::GetVoxel` over the capsule), the other side; if both are, nothing (Jolt's penetration recovery). Integer
  side test on quantised positions. A player standing on a lowered drawbridge or leaning on a closed door is never
  pushed: those are at rest, outside the arrival window (`Mechanism.StandingOnALoweredBridgeIsNotPushed`,
  `Mechanism.LeaningOnAClosedDoorIsNotPushed`). In M2 a landing portcullis or bridge also deals crush damage to
  players as a server sweep (networking.md decides crush is a sweep).
- **Bodies in the way (the crush rule):** a kinematic body has infinite mass; a crate wedged between a closing door
  and its frame, or under a landing portcullis, would be shot out or pushed through the terrain by Jolt's penetration
  recovery, and Q-dropped crates in doorways would make that a cheap griefing trigger. **Decision:**
  `sim::StepMechanisms` (server and offline) crushes every non-player body that overlaps a mechanism's block box at
  the pose for the tick during the last 6 ticks of a closing segment and at its arrival: a replicated body by
  `sim::Despawn(..., DespawnReason::Broken)` (clients make rubble and a puff), a local or offline body by
  `Scene::Destroy` with fragments presented as for a blast. A crate cannot prop a gate open: fewer bugs, and the rule
  is readable. Bodies merely in the swing path are pushed by the kinematic leaf as today (`Rubble` meets Dynamic and
  Rubble). Test: `Mechanism.ClosingGateCrushesACrateInsteadOfLaunchingIt` (the crate's speed never exceeds the
  gate's; it is gone at arrival).

### The one movement rule grows by one call

Before every `sim::StepPlayer` (server, predicting client including every replayed tick, offline), the owner calls
`sim::PoseMechanisms(sim, tick, PlayerReach(feet))`: every mechanism whose swept bounds meet the box is set to its
exact pose at `tick` (transform, velocity = pose(tick + 1) - pose(tick) over the tick, layer by the timetable), then
`PushOutOfMechanisms`. After the players, `sim::StepMechanisms(sim, tick)` gives every moving mechanism
`MoveKinematic(pose(tick + 1))` so physics bodies meet it during `StepPhysics`, applies the crush rule, and sets the
layer of mechanisms that just arrived. Setting the exact pose each tick (not integrating velocities) means no drift,
and the Jolt v5.6 trap of a sleeping kinematic body under a constraint (Jolt discussion #2041) cannot happen: every
move goes through the body interface, which activates it.

Where it runs:

| Owner | `PoseMechanisms` | `StepMechanisms` |
|---|---|---|
| Server | in `StepPlayers`, per player before its step | between `RunCasts` and `StepPhysics` (Server.cpp:2116-2119) |
| Predicting client | before each predicted and replayed step, at that step's tick (`Client::nextTick`, not `Simulation::Tick()`) | after the predicted step |
| Offline | in the game's `OnFixedUpdate` before `StepPlayer` (`src/main.cpp:202-214`) | before `sim.StepPhysics` (`Engine.cpp:284`) |

The client draws mechanisms at its predicted tick with the frame's alpha (not in the past like other bodies): what the
player sees is what the player collides with. A cast on time is resolved at its input's tick, when the server's
mechanism body is at the same pose (see "Review": the cast-rewind finding).

### Replication

New wire values (all bump `kProtocolVersion`; `docs/notes/net.md` "Recipes", new event kind, is the recipe):

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

The event numbers above are provisional: each value takes the next free number from the registry in
[notes/net.md](../notes/net.md) section 2 when [mech-1] lands (magic.md amendment 4). On the ROADMAP order of
2026-11-21 ([mag-2] first) `MechanismMove` and `RopeCut` become 40 and 41; `PlayerButton_Use` keeps 16 (magic's Spell
edge is reserved at 32). `Mechanisms.WireSizes` pins the sizes and whatever values land.

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

### How they are placed

- **Templates:** `RegionTemplate` gains `std::vector<TemplateMechanism> mechanisms` (model, position, rotation in
  quarter turns about y, `MechanismDesc` with links as indices into the vector, initial position). The server spawns
  them after the objects at start (Server.cpp:2083) and turns indices into NetIds; offline the game creates the same
  entities. `TemplateObject` itself does not change (crates stay crates). The authoring rule (levers behind their
  targets) is checked here.
- **The yard, from stage 1:** `sim::MechanismYardTemplate`, sited with the playground's level-site search north of
  the plaza, clear of the probe (east), the watch tower, the castle-case scripts and the drill's sites. A few
  `FillBox` ops: a 4 x 3 m hut with a 0.9 x 2.1 m doorway and a door; a 6 m wall, 4 m high, with a 3 x 3 m passage,
  a portcullis and a lever on a post inside (behind the wall, by the authoring rule). It is part of the **default
  offline game from stage 1 M-a** and of **`playground.session` from M-b** (the owner checks features windowed in the
  default game and has just run `playground.session` on two PCs; a yard behind a flag would stay unseen until
  stage 4). `--no-mechanisms` builds the world without it.
  - Every script that pins numbers (`--tower-blast`, `--neck-blast`, `--castle-blast`, `--probe`, `--climb`, `--fly`,
    the `vortex_server` drill and arena runs, `tools/session-check.sh`) either confirms its logged numbers unchanged
    with the yard or takes `--no-mechanisms`; any number that moves is re-recorded in CLAUDE.md in the same commit
    (expected to move: `World: generation` and join bytes on the playground, a few hundred bytes of baseline).
- **Stage 4:** the watch tower's doorway (`Region.cpp:300`, 0.6 x 2.0 m) gets a door in the default playground, after
  re-measuring the tower and neck scripts with it.
- **Later (worldgen W3):** the fort recipe's gatehouse kit gains sockets (gate leaf pair, portcullis slot with a winch
  room above, drawbridge hinge line over the ditch, a lever niche inside), and keeps get sally doors and hanging
  loads. Plan records with stable plan indices give the NetIds. setting.md's "doors stay open" in the Quiet means town
  doorways stay open holes: mechanisms are concentrated in forts and keeps, which keeps the counts in the budget.

## Ropes with physics

### Which ropes are physics, which are pictures

| Rope | Physics | Why |
|---|---|---|
| **Load rope**: a hanging cage, chandelier, counterweight, a log trap | Server only: **one Jolt `DistanceConstraint`** (min 0, max = length: a rope, not a rod) between a world-fixed anchor (`Body::sFixedToWorld` at the anchor voxel's point) and the load's attach point. No link bodies. Offline the same | The load really swings when a blast pushes it and really falls when the rope is cut: the owner's "rope with physics". One constraint is stable at any mass ratio (no chain of light links under a heavy load), costs about 1 us per step (est.) and adds no bodies. The load is an ordinary stage-6 body: samples while it swings (31 B, one of the 64 awake slots, a ghost to players), `EntityRest` when still |
| **Mechanism chain**: portcullis chain, drawbridge chains, a winch rope | None. Its ends follow the anchor and the mechanism's pose (timetable); drawn straight (taut) | The mechanism carries the load by script; a physical chain would only disagree with it |
| **Decorative rope**: lines, hanging ropes, well ropes, rope rails | Client-only Verlet (`fx::RopeStrand`) | Sway and wind are decoration; nothing in play depends on them |
| **Cut halves** of any rope | Client-only Verlet, one end pinned (anchor, or the load's attach point following its copy) or none (falls, 10 s lifetime) | Pictures |

Rejected: a chain of rigid links (`HingeConstraint` / `PointConstraint` between capsules) in sessions - every link is
an awake replicated body (a 12-link rope = 12 of the 64 slots and 372 B per snapshot while it moves, est.), heavy loads
on light links stretch and explode (the classic mass-ratio failure), and Jolt is not built cross-platform
deterministic, so it cannot run on clients. Jolt soft-body rods cannot carry rigid loads (constraints do not act on
soft bodies) and bring a pipeline the engine does not use.

### The physics wrapper (stage 2)

`PhysicsWorld` gains `ConstraintId` (generation + slot, the `CharacterId` pattern, PW.cpp:342-350, 1135-1141),
`CreateDistanceConstraint(BodyId|World, pointA, BodyId, pointB, minLen, maxLen, spring)`, `SetConstraintLength`
(winched loads, stage 3), `DestroyConstraint`. Rules the wrapper enforces:
- `DestroyBody` removes the body's constraints first; the destructor removes all constraints before the
  `PhysicsSystem` goes (PW.cpp:370-400).
- Never to a terrain chunk body (they are destroyed on every edit, Terrain.cpp:522, 540): terrain anchors are
  world-fixed; the anchor voxel is checked by the server instead (cuts below).
- A frozen load (`SetFrozen` makes it Static, PW.cpp:841) is fine: a constraint between the world and a static body
  does nothing.
- `Scene::Damage` makes a new body even for the kept piece (Scene.cpp:32-43, 589-592): the server re-creates the
  constraint on it if the kept piece still holds the attach voxel (attach point shifted by `keptOffset`), else it cuts
  the rope at the load end (`RopeCut` at fraction 1.0) and the load falls. Integer decision (a voxel in a volume).
- The constraint's position steps: Jolt's defaults (10 velocity, 2 position); a damping of the load's linear and
  angular velocity (0.2, est.) so a chandelier rests within about 5 s (measured in stage 2).

### Cosmetic ropes on clients

`fx::RopeStrand` (in `vortex.fx`: Core, GPU-free, testable): up to 16 points, pinned ends (a world point or an entity's
local point read each frame), gravity, damping 0.98, 4 length-projection iterations per tick (Jakobsen relaxation),
wind as a per-client force from a time noise with a phase by position (nothing synced). A strand **sleeps** when its
ends are still, wind is off and no point moved more than 1 mm for 60 ticks. Collision: none in stage 2 except a clamp
of points above solid voxels for strands within 20 m (one `World::GetVoxel` per point), kept only if the measured cost
allows (est. doubles the strand cost). A cast within 3 m kicks nearby strands' points (`Engine::PresentBlast`, a
picture). Beyond 40 m strands are not simulated: they are drawn as their rest catenary (a fixed table by sag ratio).
At most **24 strands are simulated** (the drawn cap / 15 links: 384 / 15 = 25, less the load ropes and chains drawn
first; the first version's 64 simulated strands could never all be drawn).

### Drawing a rope: instances of one shared link model

- **Decision:** each segment of a strand is one instance of a shared baked link model (rope: 1 x 1 x 8 voxels;
  chain: a 3 x 1 x 5 link, alternate links turned 90 deg) with a non-uniform scale: a 3-4 cm thick rope, the link
  length = the segment. An engine-side `GatherRopes` next to `GatherInstances` (`src/engine/internal/Gather.cpp`) emits
  `InstanceDesc`s; scene render data stays in `vortex.engine`. One chunk slot (36 936 B) per link model, not per rope.
- **Why:** no new pipeline, shader or binding; ropes get shadows and AO through the BVH like every object; the voxel
  look matches. Rejected for now: a strand pipeline (tube expansion in a vertex shader, est. 0.02-0.05 ms) - a new
  pass, no shadows, and it would need its own TAA care; it is plan B if instances cost too much.
- **Thinness at 768 x 432:** a 4 cm rope covers about 2 px at 10 m and under 1 px past about 25 m (est.), where it
  would shimmer under TAA. The gather widens the drawn thickness to at least 1.2 px at its distance, and decorative
  strands beyond 40 m are not drawn.
- **Budget:** at most 384 rope instances drawn (nearest first; load ropes and mechanism chains before decoration).
  Est. cost: 0.03-0.08 ms in the G-buffer, 0.05-0.1 ms of CPU BVH rebuild (the instance BVH is rebuilt on the CPU
  every frame over all instances, `Renderer.cpp:1185-1209`), a few % on shadow and AO rays (long thin overlapping
  boxes are the BVH's worst case). **Measured at the cap in stage 2**, not at the yard: `--rope-stress N` places N
  16-point strands within 20 m of a fixed `--camera`; N = 26 (about 384 links) after `World: window filled`, the
  deltas of `GPU gbuffer` / `GPU lighting` and the BVH build time against `--no-ropes`. If over 0.3 ms GPU in total,
  the strand pass is built.

### Cutting a rope (server, integer)

- **The ray (a direct shot):** in `ResolveRay` (and the offline cast), before the hit, the server tests the ray
  against every rope polyline whose integer AABB meets the ray's box: segment-to-segment distance in 1/16 voxel,
  int64 (the ray is at most `castRange` 60 m = 7680 units and a rope segment at most 2048, so every product fits),
  threshold `ropeRadius + 0.1 m` of aim forgiveness. Every rope the ray passes before its hit point (or within
  `castRange` when it hits nothing) is cut, at most 4 per cast. Ropes do not stop the ray: the cast's blast still
  goes where the ray hits, so a rope never eats a shot at the enemy behind it. A ray that hits nothing but cut a rope
  counts as run (no `Missed` / "Cast not run"), with no carve. Tests: `Rope.DirectShotCutsAHangingRope`,
  `Rope.SkyBehindStillCuts`, `Rope.RopeDoesNotStopTheCast`.
- **The sphere:** after a cast's carve, the server tests every rope whose integer AABB meets the cast sphere's: the
  rope's server polyline (taut ropes and chains: the straight chord between their end points, quantised to 1/16 voxel;
  decorative ropes: a 4-segment polyline from their rest sag table) against the sphere in the cast's own units
  (centre and radius in 1/16 voxel, as `CarveSphereQ`). Per segment: `t = ac . ab`; clamp to the ends; compare
  `d^2 * |ab|^2` with `(r + ropeRadius)^2 * |ab|^2` in int64. The load's attach point is the server body's pose at the
  cast tick (a float the server alone reads; quantised before the test, so a logged replay decides the same).
- **Hits to cut:** one, ropes and chains alike. A chain that takes three hits with a counter nobody can see reads as
  "my shots do nothing"; one hit is readable. Chains that matter (drawbridge, portcullis) are placed where defenders
  cover them (stage 3 tunes their placement, not their toughness).
- **Anchor lost:** after every terrain operation (`EditReport::changed` already lists the chunks), ropes and
  mechanisms with an anchor voxel in a changed chunk read it with `World::GetVoxel`; air cuts the rope at that end
  (or frees the mechanism). Also when an anchor lands in a `Detach` mask (its island fell).
- **Despawn:** a rope end on a NetId that is despawned is cut (see "Replication").
- **Result:** `RopeCut{ropeId, tick, fraction}`. The server removes the constraint; the load, already a dynamic
  replicated body, falls (it may wake: `EntityWake`, samples). Each client splits its strand at the fraction into two
  cosmetic halves. **A cut portcullis chain jams the portcullis shut** (`MechanismMove` to `minPos` at drop speed with
  `Jammed`; stage 2, so the first rope already changes a fight in the yard); cutting both drawbridge chains drops the
  bridge (to `minPos`, 1 s, `Jammed`, stage 3).

### A player rope tool (tie, pull, climb): later, not in stages 1-4

- **Decision:** not now; reconsidered after M2 (health, teams). Recorded reasons:
  - Climbing needs a new movement mode in `StepPlayer` (a `PlayerState` field, so the 32-byte owner block and every
    `Movement.*` test change, plus a protocol bump) - the most sensitive contract in the engine.
  - Tying and pulling bodies needs constraints between replicated bodies driven by player input: more awake bodies,
    snapshot bytes, and a griefing tool (tie the enemy's gate) before teams exist.
  - Its fun is unproven next to M2's combat loop; doors, gates and cuttable ropes are proven siege toys.
- **The cheap first step when it comes (stage 5):** climbable fixed ropes and ladders as **climb volumes** (an authored
  box; inside it `StepPlayer` maps forward input to vertical speed; one `PlayerFlag` bit), the rope itself cosmetic.
  No rope physics on players at all.

## Destruction interplay

### The damage rule (stage 1)

Today a 1.2 m, power-6 player cast damages every body whose bounds meet the sphere's box and removes every
non-indestructible voxel in it: one cast would clear a 0.9 x 2.1 m door, about 3 casts a 3.5 x 4 m leaf (under 0.5 s
at 10 casts/s), and a cast at the gatehouse wall within 1.2 m of a lever would break the lever and jam the gate by
accident. Doors as cover and holding a gate need a rule, decided in stage 1:

- **A mechanism is damaged only when aimed at:** in the `Blast` object loop (Blast.cpp:633-690), an entity with
  `scene::Mechanism` is damaged only if the cast's centre lies within `kMechanismHitRadius` = 0.3 m (est.) of its
  posed bounds (an oriented box test); then it loses at most a sphere of that radius around the centre, and only
  voxels with `hardness * 16 <= power` (the terrain's own rule, `World.cppm:124`). A new
  `Scene::Damage(entity, centre, radius, DamageOptions{power, hingeBox, layer})` overload carries it; ordinary objects
  keep today's rule (their logged numbers do not move).
- **A cast that hits a mechanism carves no terrain.** When `ResolveRay` (or the offline cast) resolves on a
  mechanism's body, the cast is a mechanism hit: the mechanism's `Damage`, the blast push and rope cuts, but no
  terrain `Carve`, no island search. Otherwise a cast at a door's middle would carve the stone frame within 1.2 m,
  air both hinge anchors and free the door on the first hit (found while applying the review; see "Review"). A cast
  at the wall beside a door still carves the wall, and may air the anchors: blowing the frame out is a legitimate way
  through.
- **Materials:** doors `planks` (hardness 1.2) or `wood` (1.5), gate leaves wood with `iron` straps (8 > power 6: iron
  voxels survive player casts, so straps stay as a skeleton), portcullis bars wood or iron. An all-iron portcullis is
  breached through its anchors (the wall around it), which is the durable-lane rule. Est. hits to free: door 3-4,
  gate leaf 15-30; stage 1 measures them (`--mechanism-blast` logs the casts needed to free each one and the
  `ObjectModel` bytes per hit).
- **Warded:** `MechanismDesc::Warded` makes `Damage` skip the mechanism entirely (setting.md: a warded keep's gate can
  be breached only after the ward drops; warding is an engine rule not designed yet, and releasing it is a future
  `MechanismMove` flag change). Every closable gate can be broken unless warded.
- **Split doors keep their hinged part.** In `Damage`, for an entity with `scene::Mechanism`, the piece holding the
  most hinge-box voxels (ties by the lower piece index) keeps the entity, stays Kinematic with the mechanism's layer,
  and is exempt from the `replicatedPieceVoxels` fragment path (262 voxels, not the 512 the first version wrote: a
  0.9 x 2.1 m door 1-2 voxels thick has only 119-238). The other pieces follow the normal rules (server bodies by
  `TakePieceSlot`, or client rubble). No piece holds a hinge-box voxel -> `Freed` (below). Doors are authored at least
  3 voxels thick (est. 360+ voxels). Test: `Mechanism.SplitDoorKeepsItsHingedPart`.
- **Tests:** `Mechanism.CastAtTheGateLeavesTheLeverBesideIt`, `Mechanism.DoorTakesNHits`,
  `Mechanism.MechanismHitCarvesNoTerrain`, `Mechanism.WardedTakesNoDamage`.

### What happens to a damaged mechanism

- **Dented, hinge box held:** it keeps working (kinematic, same desc, `modelOffset` updated, the timetable layer
  re-applied).
- **Hinge box emptied, or both terrain anchors air:** `MechanismMove{Freed}` then it is an ordinary stage-6 body: it
  falls off its hinges, its pieces ask `sim::TakePieceSlot` (2 per tick, 64 awake, 256 live) or become client rubble.
  Its links are dropped, its chains cut (`RopeCut`); a lever that drove it shows `Broken`.
- **A lever** (est. 32-64 voxels) is damaged only by a cast aimed at it; a hit that leaves no hinge-box voxel breaks
  it (`Broken`, despawned) and its targets stay where they are. Defenders can jam their own gate shut by shooting
  the lever; the gate itself stays breakable unless warded.
- **Big gates are carved, not staged:** an `ObjectModel` per hit is est. 0.3-1 KiB for a 3.5 x 4 m leaf; ten casts a
  second on one gate is 3-10 kB/s against the **12 kB/s** entity budget: tight. Stage 3 measures it under `--drill` on
  a gate; above 6 kB/s the gate gets a coarser model (fewer voxels), not a new damage system (open question 6).
- **Structural integrity (shred.md SI-1..3):** SI-1 judges Static objects and resting Dynamic ones; mechanisms are
  Kinematic, so they are not judged while on their hinges (a hinge is scripted support), and a test pins that.
  Once freed a mechanism is Dynamic and every SI rule applies. SI-2 impacts (when built) never come from scripted
  motion: kinematic bodies are excluded; a falling cut load or freed gate leaf is a dynamic body and does deal its
  estimated-weight impact when it lands.
- **Ropes are weak points:** a cast through or near a rope cuts it (above). A cut chandelier or cage falls on what is
  below; in M2 it deals crush damage by the server's sweep.

## Gameplay it enables

Sessions (setting.md's situations):
- **Hold the gate** ("Pass wall", "Siege"): defenders inside the gatehouse drop the portcullis with a lever (0.75 s
  drop); raising it again is a 9 s winch hold, faster with a crew, the holders exposed, and the gate stays solid until
  its gap is taller than a player. Attackers blast the portcullis's wood (or its anchors), or shoot the lever through
  a murder hole so it stays as it is; they cannot pull the defenders' lever through the bars.
- **Cut the drawbridge** ("Hold the crossing", stage 3): attackers cut both chains and the bridge falls open for good;
  defenders raise it with attackers on it and drop them into the ditch.
- **Last resort** (stage 2): defenders cut their own portcullis chain: it drops and jams; attackers must break through.
- **Doors as cover:** close a door behind you to block sight and casts; it wears down hit by hit (3-4, est.), then
  falls off. A crate cannot prop it open (it is crushed when the door shuts).
- **Hanging loads:** cut the chandelier over the keep's hall onto the attackers (M2 crush damage); a cage drop on a
  crate pile.
- **Timed gates:** a sally door that shuts 10 s after its lever, an attackers' gate that opens at a session time
  (a segment the server emits at start with a future `startTick`: 16 B once).

Offline (the default game from stage 1): the yard as a toy - open the door, run the gate, blast the lever, shoot a
door off its hinges; from stage 2 cut the cage onto crates and the portcullis chain.

## Budgets and costs

All est. until the stage named measures them. Server tick 16.7 ms; client frame about 21 ms on the UHD 620.

| Item | Budget | CPU (client / server) | GPU (UHD 620) | Network |
|---|---|---|---|---|
| Mechanisms per region | 128 (the server refuses a template beyond) | - | Voxel objects like crates | Baseline 200 B - 1.5 KiB each (25-60 KiB for a fort's worth, est.) |
| Moving at once | no cap needed; play gives about 10 | `StepMechanisms` 1-2 us per moving one per tick (crush test included) | - | 0 in snapshots |
| `PoseMechanisms` | mechanisms within one player's reach box | about 1 us each per step; a 20-tick replay with 3 near: 60 us | - | - |
| `FindUseTarget` | one per frame (client), one per Use edge (server) | under 5 us | - | 1 bit per input |
| Use results | - | - | - | +5 B per owner snapshot |
| `MechanismMove` | 1 per accepted use per target; a winch at most 1 per 12 ticks | - | - | 16 B; a siege 8-16 B/s per client, charged to the 12 kB/s entity budget, never refused |
| Load ropes | 32 per region; their loads use the generic 64-awake cap while they swing | server: 1 us per constraint per step | - | rope spawn 32 B; samples of a swinging load 31 B |
| Cosmetic strands | 24 simulated (within 40 m), 12-16 points | 10-25 us per tick (client) | - | 0 |
| Loose cut halves | 16 (oldest go), 10 s each | in the 24 above | - | 0 |
| Rope instances drawn | 384 | BVH rebuild +0.05-0.1 ms | +0.03-0.08 ms G-buffer, a few % on shadow / AO rays (measured at the cap, `--rope-stress 26`) | 0 |
| Rope cut tests | per cast: ropes along the ray and in the sphere's box | under 1 us per rope | - | `RopeCut` 12 B |
| Gate under fire | carved, 0.3 m per aimed hit | as any object | as any object | 0.3-1 KiB per hit; 3-10 kB/s at 10 casts/s against the 12 kB/s entity budget (stage 3 measures) |

How measured:
- The game's exit log gains `Mechanisms: N, moving max M, uses U (refused R), lead min A max B, crushed K, step max S
  us, pose max P us, use query max Q us` and `Ropes: N (load L, cut C), strands simulated max S, instances drawn max
  I, sim max T us`.
- GPU: `GPU gbuffer` / `GPU lighting` lines at `--camera yard` and with `--rope-stress 26` after
  `World: window filled`, with and without `--no-ropes` (a stage-2 switch that draws no rope instances).
- Server: `Ticks: server:` with `--bot-use` on `playground.session`; the `Mechanisms:` and `Objects:` lines
  (`mechanismModelsOverBudget`, `mechanismHitsSkipped`, `modelsToRubble`).
- Network: the event bytes in the existing `Sync:` / `Objects:` lines, the baseline bytes in `Join:` lines.

## Tests

GPU-free first (VortexSimTests unless noted), each written to fail against a stub:

- `Mechanism.PositionIsAFunctionOfTick`: `PositionAt` at every tick of a segment equals a tick-by-tick reference;
  `p == targetPos` exactly at `startTick + D` and after; `D == 0` gives `startPos`; pinned golden positions for both
  ease tables (the same on every platform).
- `Mechanism.DurationIsIntegerExact`: `D` for a door (1024 units, 36 ticks) and a lever (512 units, 12 ticks) and a
  partial reversal.
- `Mechanism.ReversalKeepsPositionContinuous`: a reversal at any tick of a swing starts from the exact position.
- `Mechanism.AutoReturnFollowsTheTimetable`: a button returns at `arrival + autoReturnTicks` with no event.
- `Mechanism.LayerFollowsMotion`: Dynamic at rest; Rubble from `startTick` to arrival for a door, a drop and a
  drawbridge; Dynamic throughout a portcullis rise; Dynamic at the arrival tick; never Debris for a 20-voxel lever.
- `Mechanism.NotCountedAsAwake`: a swinging door takes no `TakePieceSlot` slot and no `CountPieces` count.
- `Mechanism.RingHoldsTheLeadAndTheStaleWindow`: with the largest lead and a use every 12 ticks, every tick in the
  stale window resolves.
- `Use.TargetAtTheInputTick`: a moving door is hit at the input's tick and missed at tick + 10 (and the reverse),
  also on the late-input path with the eye from `StateAt`.
- `Use.TerrainOccludes`, `Use.ReachLimit`, `Use.NearestOfTwo`, `Use.SmallLeverIsUsable`,
  `Use.ClosedGateBlocksTheLeverBehindIt` (raised gate: usable; closed: `NoTarget`), `Use.GateIsAnInfoTarget`.
- `Use.DoorOpensAwayFromTheUser` (both sides of the plane; `OneWay` ignores the side).
- `Use.LeverDrivesItsTargets` (same `startTick`, matching direction; a freed target is skipped).
- `Use.RefusalsInOrder` (Stale, NoTarget, Locked, Busy, Broken, NotUsable), `Use.OpenCannotInterruptAClose`,
  `Use.CloseInterruptsAnOpen`.
- `Use.AuthoringRuleRejectsAReachableLever` (template load).
- `Mechanism.LandingPushesThePlayerOut` (the feet's side; the other side when the first is terrain; no push when
  both are), `Mechanism.StandingOnALoweredBridgeIsNotPushed`, `Mechanism.LeaningOnAClosedDoorIsNotPushed`.
- `Mechanism.ClosingGateCrushesACrateInsteadOfLaunchingIt`.
- `Mechanism.PlayerStepIsTheSameReplayed`: steps with `PoseMechanisms` before each, restored and replayed, give equal
  states (bit for bit on one machine).
- `Mechanism.TwoSimulationsFedTheSameInputsAgree`: the same player inputs, leads and casts into two simulations give
  equal segment rings, flags, entity hashes and world hashes (as
  `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes`).
- Damage: `Mechanism.DentedKeepsWorking`, `Mechanism.DentThatCropsTheModelKeepsThePivot`,
  `Mechanism.SplitDoorKeepsItsHingedPart`, `Mechanism.HingeGoneFrees`, `Mechanism.AnchorAirFrees`,
  `Mechanism.CastAtTheGateLeavesTheLeverBesideIt`, `Mechanism.DoorTakesNHits`, `Mechanism.MechanismHitCarvesNoTerrain`,
  `Mechanism.WardedTakesNoDamage`, `Mechanism.NotJudgedByTheObjectLoadCheck` (SI-1 skips kinematic; written when SI-1
  lands).
- Stage 2: `Physics.DistanceConstraintHoldsLength` (within 1 cm over 600 ticks), `Physics.DestroyBodyRemovesItsConstraints`,
  `Physics.ShutdownWithConstraints`, `Rope.CutTestIsIntegerExact` (ends, tangent, just outside, long segments at the
  int64 limit), `Rope.DirectShotCutsAHangingRope`, `Rope.SkyBehindStillCuts`, `Rope.RopeDoesNotStopTheCast`,
  `Rope.AnchorAirCuts`, `Rope.LoadDespawnCutsItsRope`, `Rope.LoadFallsAfterTheCut`, `Rope.LoadSwingsAndRests` (rests
  within 5 s), `Rope.DamagedLoadKeepsItsRopeIfTheAttachVoxelSurvives`, `Rope.CutChainJamsTheGate`;
  `RopeStrand.LengthWithin5Percent`, `PinnedEndsStay`, `SleepsWithoutWind` (Core tests).
- VortexNetTests: `Mechanisms.WireSizes` (the values taken from the registry when [mech-1] lands; spawn with 1 and 8
  segments, `MechanismMove` 16, `RopeCut` 12, owner block 37), `Mechanisms.LateJoinerDerivesTheSamePosition` (two pending segments), `Mechanisms.DentedDoorKeepsItsLayerOnEverySide`,
  `Mechanisms.NoCorrectionWalkingThroughAnOpeningGate` (RTT 100 ms at the computed lead: 0 corrections),
  `Mechanisms.NoCorrectionUnderARisingGate`, `Mechanisms.CorrectionOnlyWhenTheEventIsLate` (RTT 400 ms, beyond the
  clamp: the player who touched the door is corrected, one who did not is not), `Mechanisms.EntityCheckCoversTheSegment`
  (repaired through `ModelRequest`), `Mechanisms.GateNeverGoesToRubbleOverBudget` (credit spent, a cast on the gate
  still sends its `ObjectModel`), `Mechanisms.WinchToggleSpamMakesAtMostOneSegmentPerBusyPeriod` (stage 3),
  `Use.RefusalReachesTheUser`; `Movement.NoCorrectionAbove2cmOnFlatGround` and `Entities.OneScriptGivesTheSameBodiesTwice`
  stay as they are.

Headless (the owner sees these as screenshots and log lines):
- `./build/Shredborne --headless --validation --frames 300 --mechanism-script --camera yard --screenshot yard.png`
  (the yard is in the default world): at frame 40 the scripted player uses the door, at frame 60 pulls the lever;
  must exit 0 and log `Mechanisms: door 0 open at tick N`, `gate 1 raised at tick M`, `0 refused`; the screenshot
  shows the door open and the portcullis up. `--camera yard-gate` frames the gate. The default headless game run
  (`--frames 200`) still logs its three required lines.
- `--mechanism-blast`: shoots the door until it is freed and the lever; logs `door 0 freed after N casts`,
  `lever 2 broken`, `ObjectModel bytes per hit`, and the `Blast:` lines with piece counts.
- Stage 2: `--rope-script` cuts the cage rope at frame 80 by a direct shot with sky behind (logs `Rope 0 cut by cast`,
  `load landed at ...`) and the portcullis chain at frame 120 (`gate 1 jammed`); `--camera yard` with ropes swaying in
  the screenshot; `--rope-stress 26` for the cost.
- `vortex_server --session=engine/tools/server/sessions/playground.session --clients 2 --bot-use 90 --drill 20
  --delay-ms 40` (lossless, no jitter) must exit 0 with `hashes equal`, both `Entities: ... agree`,
  `Mechanisms: bot N: U uses, R refused, positions agree`, and the gate-walking bot's `Client N movement:` at 0
  corrections. The same with `--loss 0.05 --jitter-ms 30` must exit 0 with `hashes equal` and `agree`, and log
  `Mechanisms: bot N: late events E, corrections at mechanisms C` with C <= E (a client counts a `MechanismMove`
  applied after its predicted tick passed `startTick` as late; a lost reliable event comes back a retransmission
  later, so zero corrections under loss is not a fair gate).

## Stages

Each ends with green tests, the headless runs, a fresh-eyes review, ROADMAP.md updated and a commit in both repos.
Stage 1 starts after the fight measurement (ROADMAP order), so after M2a's protocol bump and `PlayerButton` bits
(see "Coordination").

| Stage | Content | Acceptance | Est. |
|---|---|---|---|
| **1. Door and gate** (the smallest fun slice). M-a offline, M-b session | M-a: `scene::Mechanism` (with `modelOffset`), `sim` segment (`durationTicks`, two `uint32` ease tables) / pose / layer rule (ghost and rising) / `FindUseTarget` (occluders, info targets, authoring rule) / `ApplyUse` (closing wins, lead argument) / `PoseMechanisms` / `StepMechanisms` (crush) / arrival push-out; kinds Door, Portcullis, Lever; the damage rule (aimed hits, 0.3 m, hardness, no terrain carve, hinged part kept, Warded) and freed / broken; `PlayerButton_Use`, crate on Q (every file in the list above); HUD prompt, info lines, lever throw and lead cue; the yard in the default offline game (`--no-mechanisms`; scripts' numbers confirmed or re-recorded). M-b: `EntityKind::Mechanism`, `MechanismMove`, owner block, `PlayerUse` on both paths, the per-use lead, the client `mechanisms` map, server resting tracks, the entity-budget rule, baseline with segments, EntityCheck / EntityList term, the yard in `playground.session`, `--bot-use` | All stage-1 tests; the yard headless run and `--mechanism-blast` exit 0 with their lines (hits to free each door recorded) and a screenshot with the door open and the gate up; the default game run's three lines; the lossless and lossy drills as above; `tools/session-check.sh` still PASS; the owner's checklist in `game-ui.md` gains "E opens the yard door, the lever runs the gate, Q drops a crate" (windowed: the owner's) | 9-10 days (M-a 5, M-b 4-5) |
| **2. Ropes** | Constraint wrapper; load ropes (a cage and a chandelier in the yard); `EntityKind::Rope`, `RopeCut` by ray, by sphere, by anchor and by despawn; the portcullis chain drawn, and **its cut drops and jams the gate**; `GatherRopes` instances and the link models (a `rope` material: `MaterialTableHash` and `buildHash` change); cut halves; `--rope-stress`. **Last, cut first if the stage overruns:** decorative strands with wind and sleep (`fx::RopeStrand`, the 24-strand cap) | Rope tests; `--rope-script` exits 0; rope GPU and BVH cost measured at `--rope-stress 26` with and without `--no-ropes` and within budget; the drill with `--bot-casts` cutting ropes ends `hashes equal` and `agree` | 6-7 days |
| **3. Siege kit** | Winch (hold, crew once per 12 ticks, release hysteresis, HUD ring), drawbridge (riders fall when it rises after the lead cue), both drawbridge chains cut drop it, Button, auto-return sally door, `Locked` from the session file; gate-under-fire bytes measured | Tests; a yard gatehouse drill with bots holding the winch, cutting chains and shooting the gate: `modelsToRubble` counts no mechanism, event bytes per hit recorded | 4-5 days |
| **4. Placement** | The watch tower door in the default playground (tower / neck scripts re-measured and their CLAUDE.md numbers updated); worldgen W3 gatehouse kit sockets and keep loads from plan indices | Default runs unchanged except the recorded numbers; a generated fort with working gates in a session | 3 days + W3 |
| **5. Later (after M2)** | Climb volumes for ropes and ladders; team locks (`ownerTeam`); crush damage to players; warding release; a player rope tool if still wanted | Decided then | - |

Total for stages 1-4 without W3: est. 22-25 days (the first version said 19-21; the review's rules cost about 3-4
days, mostly in stage 1).

## Coordination with other work

- **Protocol order.** Networking stage 8 is committed and changed no wire value (`kProtocolVersion`, `PlayerButton`
  and the owner block as stage 7b left them). M2a "first fight" (ROADMAP "Next", review WP7) takes the next
  protocol number and adds generic action-slot `PlayerButton` bits (designed in WP6); this design takes the number
  after M2a's. `PlayerButton` values, new event kinds (37, 38), `EntityKind` 3-4, `UseResult` and the owner block size
  are wire format: whoever lands second renumbers nothing it does not have to, and bumps. If M2a grows the owner
  block, the +5 bytes go after its fields; if its action-slot bits take the free bits of `buttons`, Use takes the next
  free value at merge (wire values are taken at merge time, never assumed).
- **shred.md SI-1..3**: they promise no wire change; this design does change the wire, so they are separate commits
  (in ROADMAP order mechanisms stage 1 comes before SI-2; SI-1 and SI-3 come after W1). SI-1 must keep skipping
  Kinematic objects (`Mechanism.NotJudgedByTheObjectLoadCheck`); SI-2 must exclude kinematic bodies from impacts.
  The new `Scene::Damage` overload must not change ordinary objects' damage (shred.md's logged numbers).
- **worldgen.md W3**: the gatehouse kit's sockets (stage 4) follow the authoring rule (levers behind their targets);
  "an open gate" as a per-session template op stays valid for gates that are only holes.
- **Keys and checklists** (E use, Q crate, the prompt, the owner's checklist items; all changed in stage 1 M-a):
  game-ui.md, CLAUDE.md, the game's `docs/owner-checks.md` (check 14) and `docs/two-pc-test-windows.md` (checklist
  6.3), `src/main.cpp:415` / `:453`, `src/Session.cpp:240-241` / `:412` / `:471`.
- **Default world:** the yard is in the default game and `playground.session` from stage 1; every scripted number in
  CLAUDE.md is confirmed or re-recorded in the same commit.

## Rejected alternatives

| Rejected | Reason |
|---|---|
| Doors on Jolt hinge motors, gates on slider motors | Jolt is not cross-platform deterministic here (the owner's run was Linux server + Windows client): only the server could simulate them, so they would be sampled like crates. Offline characters push dynamic doors and online they do not: two behaviours |
| Doors as generic stage-6 bodies driven by `MoveKinematic` and sampled | 31 B per sample per client for the whole swing (about 1 KiB per 1 s swing, est.), one of the 64 awake slots each (starving blast pieces), walk-through for 0.5 s more until Jolt sleeps them |
| Gates as terrain `FillBox` steps | No smooth motion, every step re-cooks colliders on every side, and a fill would put back voxels a blast removed. Kept only for gates that are holes ("an open gate") |
| Solid while moving for every kind (gates push and carry players) | Needs the carry decision on every client and a push-into-wall rule while moving. Taken only where neither arises: a slide rising out of its passage. Lifts may reconsider it with a corrections test |
| A fixed 15-tick lead everywhere | Twice the owner's LAN need, about 350 ms before visible motion with smoothstep, and offline has no latency; the per-use lead needs no wire change |
| Reversals allowed both ways after 12 ticks | A spammer or a tug-of-war keeps a door moving, and so walk-through; closing wins instead |
| Damage to mechanisms from any cast sphere that touches them | One cast clears a door; casts at a wall break a lever beside it by accident |
| Repairing a mechanism from the `EntityList` entry | A wire change to every entry and against 7b's "never re-made from the list"; the `ModelRequest` path already repairs |
| E overloaded by context (use, else crate) | Client and server can disagree at the edge of reach: surprise crates |
| Predicting a mechanism's state on the client | Shared state: two users in one tick, a locked gate; a wrong guess snaps back. Only the lever's picture and the lead cue are predicted |
| Physical rope chains (rigid links) in sessions | 12 awake bodies and 372 B per snapshot per swinging rope (est.), unstable under heavy loads, not deterministic across machines |
| Jolt soft-body ropes | Cannot carry a rigid load; an unused pipeline |
| Ropes as particles | Billboards without shadows; there is no line primitive. (Beads in the particle renderer stay a free fallback for far chains) |
| A strand render pass now | A new pipeline without shadows and with TAA risk, for a cost the instance path may already meet; plan B |
| Chains that take 3 hits | An invisible counter reads as "my shots do nothing" |
| Ropes that stop a cast | A rope would eat shots at enemies behind it |
| Staged damage models for big gates | New data and code before the carve path is measured; carving is what players expect from this game |
| Pristine mechanisms derived from a plan on clients (0 join bytes) | Needs the generator's plans on clients; the baseline is cheap enough for template counts. Revisit with W3 |
| A player rope tool in the first stages | See "A player rope tool": movement contract, griefing before teams, unproven fun |

## Open questions (each with the recommended answer)

1. **Where does the crate go?** -> Q: no misreads (settled; listed so play tests can overturn it).
2. **The lead clamp: 4-20 ticks?** -> Yes; widen the top only if internet play shows corrections at doors, and log
   the lead per use so the owner's runs show it. (Fixed or adaptive is settled: adaptive per use.)
3. **Which way does a door open?** -> Away from the user by default; fort doors and gates are authored one-way.
4. **Do drawbridge riders fall when it rises?** -> Yes (consistent with stage 6, warned by the lead cue, a good siege
   moment); revisit if play tests hate it - then a raise is refused while a player stands on the deck (`Blocked`).
5. **Can levers be destroyed?** -> Yes, by aimed casts only: the gate stays as it is and remains breakable unless
   warded.
6. **Big gates: carved or staged?** -> Carved; stage 3 measures bytes per hit, a coarser model if a gate under fire
   is over 6 kB/s (half the entity budget).
7. **Do loads swing as server physics or hang still until cut?** -> Swing (one constraint, generic awake cap): it is
   the "rope with physics" the owner asked for and costs one awake slot per swinging load. If a fight shows loads
   crowding out blast pieces, loads beyond 8 awake are slept by the server.
8. **Pristine mechanisms from plans (0 join bytes)?** -> With W3, once plans exist on clients; until then baseline.
9. **Team locks?** -> M2 (teams). Stage 3 has `Locked` from the session file only.
10. **The tower door in the default playground?** -> Stage 4, after re-measuring the tower and neck scripts.
11. **Rope drawing: instances or a strand pass?** -> Instances; the strand pass if 384 links cost over 0.3 ms GPU at
    `--rope-stress 26`.
12. **Hold E: a toggle option for accessibility?** -> Yes, as a later setting; no wire change.
13. **`kMechanismHitRadius` 0.3 m and `kMechanismReopenTicks` 20?** -> Start there; stage 1 tunes the radius to 3-4
    hits per door and records the measured hits to free.
14. **Passing under a rising gate: is about 60 % of a 3 m raise the right gap?** -> It follows from the capsule
    height; if attackers still slip under too early, a portcullis raise uses smoothstep with a slow start (no wire
    change).
15. **Rewind casts at moving mechanisms for queued casts?** -> No (see "Review", rejected in part); reconsider if
    play shows shots through a closing door that the shooter saw closed.
