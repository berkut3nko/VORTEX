# Mechanisms, the use key and ropes

[[../../../00 Docs Map|Docs map]] · [[../README|docs index]] · [[../ROADMAP|roadmap]] · [[../ARCHITECTURE|architecture]] · [[networking|networking]] · [[shred|shred]] · [[game-ui|game UI]] · [[worldgen|worldgen]]

| | |
|---|---|
| Status | [mech-1] offline part built and tested (cards 1-7: doors, the portcullis and its lever, holding with E, the damage rule, the yard; the owner checked doors and the lever in a window, 2026-11-24); its session part: cards 8-11 (wire, protocol 12, server, client, session play) built and tested headless; the owner's windowed check 25 and the stage's full verify and soak not run yet. [mech-2] offline part built (2026-11-24, "Ropes: the offline part as built" below); its session part not built. |
| Decided | - Owner's request (2026-11-17): "in game should be implemented rope with physics, in game action which moves objects such as door opening by clicking E on them or activating lever (for castle gates)".<br>- Us (this design, 2026-11-17), amended the same day after two critics' review: 27 findings, two pairs of them the same, so 25 distinct: 24 applied, two of those with a change, and 1 rejected in part ("Decision in brief").<br>- Us, placement in ROADMAP under the owner's standing rules (the owner may veto): stage 1 comes after the fight measurement (review WP8), so its yard is measured inside a known fight budget, and takes the protocol number after M2a's, because it changes the E key and adds a `PlayerButton`.<br>- Owner's request (2026-11-24): "design all those actions on the button E: toggle a lever, open / close a door, hold a small object (it gets moved with the player's movement), rope actions; add it to card 4". Us, the same day (the owner may veto): 12 decisions, written as "One use key: E"; refined there with their reasons ("Decisions refined"), after a review of the holding design (32 findings) the same day. |
| Next | [mech-1] door and gate (M-a offline, M-b session), in ROADMAP "Next" after WP8. [mech-2]..[mech-4] in ROADMAP "Later"; [mech-5] after M2. |
| Open | Questions 1-20 in "Open questions" (each with the recommended answer; 1 and the fixed-or-adaptive part of 2 are settled). |
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
| The use key | E = new `PlayerButton_Use = 16` (wire format, protocol bump): opens and closes doors, throws levers, picks up and drops small objects, and (from [mech-2]) releases a load rope. The crate moves to **Q** | Server, at the input's tick, with the same function the client uses for its prompt | One bit per input; the result in the snapshot owner block | - |
| Held object (crate, barrel, small rubble) | A dynamic body made kinematic and **posed** in front of its holder every tick; a ghost while held | Server (pick up, drop, throw, auto-drop); every side poses it from the holder | `EntityHold` (12 B), `EntityRelease` (44 B); no samples while held | Never while held (a ghost); after the drop, as any stage-6 body |
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
- **Stage 1 (est. 12-13 days):** E opens a hinged door and a lever raises and drops a portcullis, offline and in a
  session, with the damage rule (doors wear down hit by hit); E also picks up, carries and drops a small object, and
  LMB throws it (added 2026-11-24). Then ropes and the chain cut that jams a gate (stage 2),
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

The steps below are the mechanism part. Since 2026-11-24 the same function also finds holdable bodies and (from
[mech-2]) rope targets: "One use key: E" gives the kinds and the order rule over all of them.

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
   matter for a use. Bodies that are not use targets (a body too large to hold, local rubble in a session) do not
   block a use: forgiving, and one less source of disagreement. A holdable body (a crate in front of a lever) is a
   candidate itself, and the nearer of the two wins ("One use key: E").
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

- It never predicts a mechanism's state (it is shared; two players can use one gate in the same tick). It does
  predict its own pick-up and drop of a small object ("One use key: E", "Prediction and replication").
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
0.01 ms (measured) does not move measurably (est.). Holding adds `[E] Pick up`, `[E] Drop   [LMB] Throw` and the
greyed `Held` and `Occupied` ("One use key: E", "The prompts").

## One use key: E

Added on 2026-11-24 (the owner's request in the header), revised the same day after a review of the design (32
findings; "Decisions refined" gives the changes of numbers and rules). E is one key with one resolution: it opens and
closes a door, throws a lever, picks up and drops a small object, and from [mech-2] releases a load rope. Q keeps
spawning a crate (unchanged). In a session, characters move no bodies (`characterMovesBodies` false, CLAUDE.md
"Replicated bodies") and that contract stays: holding is a server-decided action that poses the object, never
character physics. `sim::StepPlayer` does not change (no weight slowdown); `kPlayerRuleRevision` stays 1.

### What E does

1. **While the player holds an object, E drops it**, whatever the look ray meets. No target search runs.
2. Else `FindUseTarget` returns the nearest use target along the look ray (the order rule below), and E acts on it:
   a mechanism (`ApplyUse`, unchanged), a holdable body (pick up), a rope target ([mech-2]).
3. The server resolves E at the input's tick, as a mechanism use (eye and look from `StateAt(in.tick)`); it decides
   at its tick `now`. The client's prompt and its predicted pick-up are predictions.
4. **LMB while holding** runs no cast: it throws the object (below).
5. **One input with both edges while holding** is one release, a throw: the Use edge and the Cast edge are both
   consumed (no use, no cast).

### The use targets and the order rule

`UseTarget` gains a kind (`sim::UseTargetKind`, not wire format):

| Kind | Value | What | E does |
|---|---|---|---|
| `None` | 0 | nothing in reach | refused `NoTarget` |
| `Mechanism` | 1 | a usable mechanism's use box | `ApplyUse` |
| `Info` | 2 | a non-usable mechanism's block box (a portcullis) | refused `NotUsable` (or `Locked`) |
| `Holdable` | 3 | a body that passes the holdable rule | pick up |
| `Rope` | 4 | reserved now; a load rope's cleat from [mech-2] | release the rope |
| `HeldByOther` | 5 | a body another player holds | refused `Busy` |

The order rule, the same for every kind (one pass; floats decide only which target, and the server sends the result
as integers):

1. `limit` = the terrain hit distance (`World::Raycast(eye, dir, kUseReach)`), else `kUseReach` (2.5 m).
2. **Candidates** come from sources in a fixed order: usable mechanisms (use box, inflated by 0.1 m), holdable
   bodies (the posed model box, inflated by 0.1 m, and only when the pick-up path is clear: "What can be held",
   "The pick-up path"), rope targets ([mech-2]: the cleat box). Each gives a distance `t` along the ray.
3. **Occluders:** the terrain (`limit`), every mechanism's block box at its pose at `tick` (a mechanism's own block
   box does not hide its own use box), and every body held by another player (its posed model box, not inflated).
   Bodies that are not candidates never occlude.
4. A candidate counts when `t < limit` and `t <` every occluder's distance. The smallest `t` wins; ties go to the
   lower kind value, then the lower key. **The key is the caller's:** a hook of `FindUseTarget` (`UseTargetHooks`)
   gives it. The server keys by `scene::NetId::value`; a client keys its copies by the NetId it keeps for them in
   `copies` (a client entity has no `scene::NetId` component), so both sides break a tie the same way; offline the
   key is `uint32(entity)`.
5. No candidate: the nearest occluder within `limit` that is a non-usable mechanism gives `Info`, one that is a body
   held by another gives `HeldByOther`; else `None`.

Why a held body occludes: without it, a client that predicts a pick-up of a crate another player took in the same
tick would, on the server, open the door behind the crate. With it the server answers `Busy` and nothing else moves.
[mech-2] adds its rope source to step 2 and changes no other step.

Positions: mechanisms at their timetable pose at `tick` (exact, as before). Bodies have no history: the server takes
them at their pose now (a late E pairs the eye at `in.tick` with a body pose up to 30 ticks newer, as casts do with
terrain); a client takes its copies as it draws them.

### What can be held

Two tests, so that a client, whose copies are all kinematic and carry no `scene::NetId`, gives the same answer as the
server:

- **The shape test**, `sim::IsHoldableShape(simulation, entity)`, the same function on every side:
  1. The entity has `VoxelBody` and `RigidBody`, no `scene::Mechanism` and no `scene::Held`.
  2. Every axis of its model's size `<= kHoldMaxExtent` (1.25 m) in metres: `size[i] * vs <= 1.25`.
  3. Its solid voxel count is at least `SceneConfig::debrisBodyVoxels` (the integer `CreateBody`'s size rule uses:
     25 at 0.125 m, 48 at 0.1 m, 14 at 0.15 m; below it a piece is Debris) and at most
     `HoldMaxVoxels(vs) = llround(kHoldMaxVolume / vs^3)` (`kHoldMaxVolume` 0.6 m^3, est.; rounded to the nearest,
     because `floor` gives 599 at 0.1 m by float rounding). The counts are integers: the test is the same on every
     side.
- **The owner test**, per side, through the hook `UseTargetHooks::holdable`:
  - Server and offline, `sim::IsHoldable(simulation, entity, replicatedOnly)`: the shape test, and `RigidBody::motion
    == Dynamic` (a frozen body counts: its component stays Dynamic); with `replicatedOnly` (a session server) also a
    `scene::NetId` with `value != 0`.
  - A session client: the shape test, and the entity is a copy in `copies` whose server motion is Dynamic
    (`Copy::motion`, kept from `ObjectSpawn::motion` and from every `ObjectModel`'s `motion`). The copy's own
    `RigidBody::motion` is Kinematic and is not read. A client's own rubble is not in `copies` and cannot be held.
- **The pick-up path** (a candidate test of `FindUseTarget`, so the prompt and the server agree): the sphere of
  radius `r` swept from the hold anchor (below) to the object's centre meets no solid voxel ("The clamp" gives `r`
  and the sweep). An object seen through an arrow slit, a murder hole or a gap under a door is not a candidate: it
  could not come out through the gap.
- **Not a player's floor:** a pick-up is refused `Busy` when the body was the ground body of any player (the holder
  included) at any tick of the last `kHoldGroundTicks` (30). The server keeps, per player, the last replicated body
  it stood on and the tick (`groundBody`, as `TrackBodies`' "carries a player" rule reads it). Why: the floor would
  vanish under a player at once, which corrects that player's client and is a griefing tool (a defender pulls the
  bottom crate of an attacker's stack). Why 30 ticks: a predicting client replays ticks before the pick-up with the
  copy already a ghost; with this window no replay of the last 30 ticks (0.5 s, more than the round trip plus the
  clock target of every link the lead clamp covers) stood on it. A client's prompt shows greyed `Occupied` for the
  body under its own feet and does not predict that pick-up; for other players the server decides.

Examples (from `CrateModel(m, PlaygroundScale(vs))` and the material table): the crate is holdable at every voxel
size (0.8 m at 0.1 m voxels, 0.75 m at 0.125 m and 0.15 m; 296 solid voxels at 0.1 m = 0.30 m^3, 0.30 m^3 at
0.125 m, 0.33 m^3 at 0.15 m: a frame model, about half solid); a solid 0.9 m stone cube (0.73 m^3) does not pass;
beams, planks and ladders (over 1.25 m) do not.

**The castle's movable objects** (the game's `docs/product/castle-layout-draft.md` section 8.1, 36 objects) against
this stage. Holding covers 14 of the 36 objects, and only when their models are frame or hollow models under
0.6 m^3 of solid voxels (a solid 1 m crate is 1 m^3 and fails). The product plan's item C2 ("grab, carry, push and
drop", P1) is so not done after [mech-1]:

| Object (count) | Size (e) | In [mech-1] | Later |
|---|---|---|---|
| Crate (8) | 1 m | held, if a frame model under 0.6 m^3 | - |
| Barrel (6) | 0.8 m | held, if hollow or a frame model under 0.6 m^3 | rolling: a push action |
| Beam (6) | 7 x 0.4 x 0.4 m | no (over 1.25 m) | drag, stage 5 |
| Plank (10) | 4 x 0.5 x 0.125 m | no | drag, stage 5 |
| Ladder (3) | 6 m | no | drag, and climb volumes, stage 5 |
| Cart (2), hay wagon (1) | 2.5-4 m | no | push, stage 5 |

### The hold pose

Inputs: the holder's `PlayerState` (feet), its input's `yaw` (u16) and `pitch` (i16), `tuning.eyeHeight`, the held
`scene::Held{holder, relYaw, sinceTick, pinnedTicks}`, the model size `s` (voxels), `vs`, and the play area
(`sim::PlayAreaOf(region, tuning)` in a session, `PlayArea{}` offline).

- **Hold pitch** (an integer, so every side uses the same value): `HoldPitch(pitch) = int8(clamp(pitch, -16384,
  16384) / 256)` (C++ division, toward zero), so -64..64; the pose uses `pitchH = HoldPitch * 256` (at most 45 deg).
  Other clients get it in the remote record (`holdPitch`, below).
- **The hold anchor** is at chest height, not at the eye: `anchor = feet + (0, eyeHeight - kHoldDrop, 0)`
  (`kHoldDrop` 0.5 m, est.); `dir = LookDirection(yaw, pitchH)`. Why: an object of 0.8-1.25 m held on the crosshair
  at eye height covers 30-45 deg of the view, so the holder cannot see where it walks or aims a throw, and enemies
  see a shield that casts pass through. Lower, the crosshair and the holder's head stay clear; the pitch still
  lifts and lowers the object.
- `h = 0.5 * length(vec3(s)) * vs` (half the model box's diagonal); `dMin = h + kHoldClearance` (0.1 m: the object
  never contains the anchor).
- `d` from the clamp (next subsection); `centre = anchor + dir * d`.
- **The play area:** `centre.x` and `centre.z` are clamped into the play area (the same rectangle the feet stay in),
  after the clamp. So a held object never goes past the play bounds, and a drop never leaves it where no player can
  come back for it. A throw can still carry it out, as a blast's push can.
- **Rotation:** `rot = angleAxis(YawRadians(uint16(yaw + relYaw)), +y)`: yaw only, no pitch or roll. The object is
  levelled at pick-up and turns with its holder.
- **Position** (the body's shape origin, the model's corner): `position = centre - rot * (vec3(s) * vs * 0.5)`.
- **relYaw at pick-up** (`sim::RelYawAtPickup(R, yaw)`): `f = R * (0, 0, -1)` with R the body's rotation; if `f.x^2
  + f.z^2 < 0.25` use `r = R * (1, 0, 0)` instead. `objYaw = atan2(-f.x, -f.z)` (or `atan2(-r.z, r.x)`); `relYaw =
  uint16(QuantizeYaw(objYaw) - yaw)`. The server decides it once and sends it as an integer.

Floats decide only the pose. The pose is never compared across machines: a held body meets nothing, and the release
sends the server's pose.

### The clamp

A thick ray, not a thin one: the object is a sphere of radius `r` swept from the anchor along `dir`.

- `r = 0.5 * (min(s) - 1) * vs`, at least 0 (half the smallest extent less half a voxel: an object resting on a floor
  passes the pick-up path test; 0.35 m for the crate at 0.1 m).
- `t` = the largest distance, up to `kHoldDistance` (1.5 m), at which the sphere centred on `anchor + dir * t` holds
  no solid voxel centre, and the segment from the anchor to that centre meets no mechanism's block box (at its pose
  at the tick) grown by `r`. The voxel part: for every solid voxel of the swept capsule's world AABB, the entry
  distance of its centre into the moving sphere (a quadratic in `t`); `t` is the smallest. The visit order (z, y, x
  ascending) does not change the minimum.
- `d = clamp(t, dMin, kHoldDistance)`. **Pinned** when `t < dMin`: the object cannot stay out of the wall.
  `static_assert(kHoldDistance >= 0.8660254 * kHoldMaxExtent + kHoldClearance)` (1.18 m) keeps a free hold possible
  for every holdable size.
- Why thick: a thin ray passes an arrow slit, a murder hole or a blasted hole narrower than the object, and the object
  would be posed and dropped on the far side of the wall. A 0.35 m sphere does not pass a 0.2 m slit. Why block boxes
  too: a closed door is not terrain.
- The sphere is inside the object's box, so the box can still overlap a wall at its corners while held (it is a
  ghost, by at most `h - r`). The release fixes that (below).
- Cost: the capsule's AABB is about 2-4k voxels at 0.1 m (est.), one `GetVoxel` each; 20-60 us per held object per
  tick (est.; "Budgets").

### Layers while held and after

- **While held:** kinematic, in a new `physics::Layer::Held` (value 6; physics only, not wire format) that meets no
  body and no character. **Rays:** a ray sees it only when the ray's layer mask is exactly `LayerBit(Held)`
  (`LayerMaskFilter`); the default `PhysicsWorld::Raycast` mask (0xFF) and every mask of today's callers do not see
  it. The callers of the default mask today are the offline cast (`AimAction` with `OfflineAimRules`, `bodyLayers`
  0), the client's cast picture ray (`src/Session.cpp`) and the editor's pick ray (`editor/internal/Tools.cpp`); a
  session cast's mask is Dynamic | Rubble. So a holder cannot stand on its own crate and fly, cannot ram crates or
  players with it, and every cast passes through a held object on every side. `MoveKinematic` to the pose every tick
  gives it the holder's velocity for the drawn interpolation. The terrain guard never freezes it (L8: kinematic
  bodies are exempt).
- **After the release** (server and offline): Dynamic. In a session the existing rule: `Layer::Rubble` while it
  moves, `Layer::Dynamic` once the server rests it (`EntityRest`). Offline: `Layer::Dynamic` (as every offline body).
  A client's copy stays kinematic ("Prediction and replication").
- `scene::QueryBodies` has no layer filter, so a blast sphere still finds a held body (auto-drop below).

### Drop, throw and auto-drop

Every end of a hold on the server and offline goes through one function, `sim::EndHold(simulation, entity, reason,
pose, velocity, anchor, env, tick, replicated, out)`, with a reason (`sim::HoldEnd`, wire format):

| `HoldEnd` | Value | When | Release velocity |
|---|---|---|---|
| `Drop` | 1 | E while holding | the holder's `PlayerState::velocity` |
| `Throw` | 2 | LMB (the Cast edge) while holding, with or without a Use edge in the same input | the holder's velocity + `kThrowSpeed` (6 m/s) x `LookDirection(input.yaw, input.pitch)` (the real pitch: aim matters) |
| `HolderDead` | 3 | the holder died ([wp-7] death path) | the holder's velocity |
| `HolderGone` | 4 | the holder left, was removed, or lost its character | the body's current velocity |
| `Damaged` | 5 | a cast's or blast's sphere meets the held body's posed model box: it is released first, then damaged | the body's current velocity |
| `Pinned` | 6 | the clamp was pinned for `kHoldPinnedTicks` (30) ticks in a row | the holder's velocity |

- **The release pose** is the hold pose of the decision tick (from the holder's state; for `HolderGone` and
  `Damaged` the body's current pose). **The fix-up** (authority only; not for `HolderGone` and `Damaged`, which have
  no anchor): the pose is clear when the posed model box holds no solid terrain voxel, meets no mechanism's block
  box, meets no other body in `Layer::Dynamic` (a resting body; moving Rubble moves away by itself) and meets no
  player's capsule (`env.playerFeet`; conservative: the capsule's world AABB against the box's world AABB). Not
  clear: `d` steps down by one voxel at a time to 0.3 m and the first clear pose is taken; none clear: the pose
  stays and Jolt's penetration recovery moves it. A drop against a wall or a stack so puts the object down between
  the holder and it, and a drop onto a player does not start inside the player.
- **The body:** `velocity = sim::QuantizeVelocity(v)` (the formula of `net::QuantizeVelocity`, the wire's 1/256 m/s,
  so the server's body and the event agree), motion Dynamic, the layer above, activated. The `scene::Held` component
  goes.
- **Throw, the wire shape:** a release reason, not a new `CastRejection`. The throw is not a refused cast: the edge
  never reaches the cast path, the owner block's `lastCastTick` / `castRejected` do not change (no "Cast not run"
  line), no [wp-7] charge is spent and no `NoCharge` / `Dead` applies. `StepPlayer` still takes the edge as today
  (`TakeCast`: the cooldown restarts when it was free), so a throw is followed by the normal 15-tick cast cooldown:
  no change to `StepPlayer`. Also, `CastRejection` 6-10 are planned for [mag-2] (llm/README "Values planned").
- **Pinned:** fires only when the object would have to enter a wall (see "Decisions refined").
- **The echo of an auto-drop:** after a `Damaged` or `Pinned` release, the holder's client still shows the object
  held for about one round trip, and an E or LMB pressed then is meant as a drop or a throw. The server so consumes
  every Use and Cast edge of that player whose input tick is at most `autoReleaseTick + echo`, with `echo` the
  player's own term of the lead formula (`ceil(RTT_p / dt) + max(margin_p, 0) + 2`, clamped 4..20 ticks): no use, no
  cast, no charge (`lastUseTick` is set, `useResult` None). The client resolves its pending drop with the release it
  gets. Cost: a deliberate E at a door in that window (at most 0.33 s) is lost once, after the HUD said `Dropped`.
- **A drop and a throw are never refused**, at any input age: the drop runs before the stale test.
- **Despawn of a held body** (budget, kill plane above the build ceiling, a rubble `Lifetime`) ends the hold with no
  `EntityRelease`: the despawn says it on every side.
- **The crush rule skips held bodies.** `StepMechanisms`' crush loop skips an entity with `scene::Held`: a ghost meets
  nothing, and a crate in its holder's hands is not destroyed because the holder stands in a doorway. A crate
  dropped in a closing door's path is an ordinary body again and is crushed as before: a crate cannot prop a door or
  gate open. Blocking a door is the [mech-3] lock's job, not a crate's.
- **Busy:** a pick-up is refused `Busy` less than `kHoldBusyTicks` (12) ticks after the same player's last hold
  change (pick-up or release). A spammer so makes at most 5 hold / release pairs a second (280 B/s, below).
- One held object per player; a held object is not a target for anyone else (`HeldByOther`, refused `Busy`).
- Not in this stage: damage from a thrown or dropped object (the impacts of [si-2] may add it); a weight slowdown.

### Where it runs

| Owner | Pick-up, drop, throw | Pose every tick | Auto-drop |
|---|---|---|---|
| Server | `StepPlayers`, on the step's Use / Cast edge and on the late-input path (`PlayerUse`, `LateCast`) | `StepHolds` after `RunCasts`, before `StepMechanisms` and `StepPhysics`: per player in slot order, `StepHeld` from its state after the step and its input | `HolderDead` in the death path, `HolderGone` where a player is removed, `Damaged` in the blast's object loop, `Pinned` in `StepHolds` |
| Predicting client | predicted on its own edges (below) | its own held body after its predicted step, before `StepPhysics` (`MoveKinematic`); other holders' bodies from the remote view it draws (`SetTransform`, teleport) | never: it applies `EntityRelease` |
| Offline | the game's `OnFixedUpdate` on the E / Cast edge | after `StepPlayer`, before `StepMechanisms` | as the server, without `HolderGone` |

Replays: a held body meets no character, and no player stood on it in the 30 ticks before the pick-up ("Not a
player's floor"), so a replay of the last 30 ticks gives the same states whether the copy is a ghost or not. Only
the newest predicted tick poses it. A replay longer than 30 ticks (a round trip over about 0.45 s) can correct once,
as any late event does. `StepHeld` with `authority` false counts no pinned ticks.

### Prediction and replication

**The rest state of a held body.** A held body is neither resting nor awake. On the server its `BodyTrack` has
`held` true and `resting` false from the pick-up: `EntityHold` works as the wake (no `EntityWake` is sent), and a
client sets its copy `resting = false` on it. `Server::Entities()` so reports it not resting on both sides.
Baselines, `ModelRequest` answers and `DescribeObject` never set `kObjectResting` for a body with `scene::Held` and
always use its current pose. `Server::AwakeBodies` and `AwakeSamples` skip held tracks. At the release the track
gets `held` false and stays not resting, so the next `TrackBodies` samples it and later emits its `EntityRest`.

**Client copies stay kinematic.** A client's copy of a server body is kinematic and moves only from samples
(`MoveCopies`, `RemoteBodies`). A hold or a release changes only its layer, its pose and which samples it takes:
- On `EntityHold` (or a spawn with `kObjectHeld`): `scene::Held` emplaced, `Layer::Held`, `Copy::held` true,
  `Copy::resting` false, and `RemoteBodies::Restart(netId, hold.tick)`: drop every buffered sample and refuse every
  sample of `hold.tick` or older (a variant of `Rest` that does not mark the copy resting). Without the drop, a copy
  that moved before the pick-up keeps old samples that `MoveCopies` would interpolate after the release.
- On `EntityRelease`: `scene::Held` removed, the copy teleported to the event's pose, `Copy::held` false,
  `Copy::resting` false, the moving layer (`MovingLayer`, as `EntityWake` sets it), velocity zero, and
  `RemoteBodies::Restart(netId, release.tick)`. From then the server's samples drive it, then its `EntityRest`.
- `Dynamic`, `SetVelocity` and `Activate` are for the server and offline only (`EndHold`).

**The holder's own client:**
- On its E edge with a `Holdable` prompt target (not greyed `Occupied`), it predicts the pick-up at once: `Held` on
  the copy, `Layer::Held`, posed every tick from the predicted state (`pending = {netId, input tick}`).
- On E or LMB while it holds (pending or confirmed), it predicts the release: it stops posing, keeps the copy where it
  was in `Layer::Held`, and waits for `EntityRelease`. **The hang is chosen:** the copy hangs for about one round
  trip plus the body sample delay (est. 0.15-0.3 s on the owner's LAN) before it falls or flies. A local ballistic
  picture was rejected: a copy is kinematic and sample-driven, so when the samples take over (drawn up to 200 ms in
  the past) the picture would jump back by that much flight. The owner's check names the hang (open question 19).
- **Confirmed** by `EntityHold` for this NetId with its own slot. **Refused** when a snapshot's owner block reports
  `lastUseTick >= pending tick` and no such `EntityHold` was applied by its `lastOpSeq`: `Held` and `Layer::Held` go,
  the copy returns to the server's state (its rest pose, or a moving copy that waits for samples) and the prompt
  shows the `useResult`. A pending drop resolves on the `EntityRelease` of that NetId (any reason).
- **While a pick-up is pending, the client holds back Use and Cast.** It latches each new edge (a press seen while
  pending, even if the key is up again before the answer). On a confirmed pick-up the next input carries the latched
  edges once (a drop or a throw, as the player meant). **On a refused pick-up the latched edges are discarded:** a
  failed grab never fires a blast and never uses the door behind the crate; the player presses again on what the
  prompt now shows.

**Other clients** apply `EntityHold` at its opSeq (the copy changes as above) and pose the copy from the holder's
remote record as the client draws that player (position, yaw, `holdPitch`, the latter from the nearer sample, not
blended). A holder missing from the snapshot leaves the copy where it was. **An `EntityRelease` waits for the drawn
holder:** the client keeps posing the copy from the drawn holder until the drawn time of that player reaches the
release tick, at most `RemoteBodies::kMaxDelayTicks` (12) ticks, then applies the release. Without the wait the
object would jump ahead of its holder by the interpolation delay (about 0.5 m at 5 m/s and 100 ms, est.), and a throw
would leave before the drawn throw.

**Wire** (inside [mech-1]'s bump 11 -> 12, no second bump; values in llm/README "Values planned across the packs":
event kinds 38-40 are planned for [mag-2], so these take 41 and 42):

| Value | Bytes | Layout (little endian) |
|---|---|---|
| Event `EntityHold` = 41 | 12 | kind u8, netId u32 (the held body), holder u8 (slot), relYaw u16, tick u32 (the server tick the hold starts) |
| Event `EntityRelease` = 42 | 44 | kind u8, netId u32, tick u32, reason u8 (`HoldEnd` 1-6), position 3 x f32 (raw bits), rotation 4 x f32 (raw bits, as `EntityRest`), velocity 3 x i16 (1/256 m/s) |
| `ObjectSpawn` flag `kObjectHeld` = 2 | +7 | after the object bytes when set: holder u8, relYaw u16, sinceTick u32 (baselines and `ModelRequest` answers only; a new piece is never held); never with `kObjectResting` |
| `RemotePlayer::holdPitch` | +1 | i8, `HoldPitch` of the player's last input, always sent; `kSnapshotPlayerBytes` 17 -> 18 |
| `HoldEnd` 1-6 | - | wire format; a decoder refuses 0 and values over 6, and `holdPitch` outside -64..64 |

- The release's velocity travels for the server's statistics, the HUD and later use; a client copy does not take it
  (it waits for samples).
- The owner block gains nothing for holding: the pick-up's answer is card 8's `lastUseTick` / `useResult` (no new
  `UseResult` value: `None` accepted, `NoTarget`, `Busy`, `Stale`).
- **No body samples while held** (both sides pose it from the holder).
- **Stale events:** `EntityHold` and `EntityRelease` (and `MechanismMove`) are in the client's per-NetId stale-event
  filter: dropped when `seq <= copy.base` (a copy re-made by a `BaselineEntity` with `kObjectHeld`, or by a
  `ModelRequest` answer, ignores holds and releases at or below its base), held while the copy may still come.
- `EntityHold` and `EntityRelease` are entity events: charged to `entityCredit`, never refused or delayed (state,
  not pictures), as `MechanismMove`.
- **Checks:** `Copy::modelHash` and `EntityInfo::modelHash` (the drill's entity comparison) stay `VolumeHash` on both
  sides. `HeldTerm(holder, relYaw, sinceTick)` is applied only where the `EntityCheck` model term and the
  `EntityList` entries are built (the server's check builder and the client's matching code), from the confirmed
  `scene::Held`: `VolumeHash ^ HeldTerm`. A predicted, unconfirmed hold is not in the term. A client that lost a hold
  or a release sees a wrong model and sends `ModelRequest`; the `BaselineEntity` answer carries `kObjectHeld` and
  re-makes the copy (7b's path, unchanged).
- **The awake cap (argued):** a held body is **not** counted in `CountPieces` / `TakePieceSlot`'s 64 awake
  replicated bodies, nor taken by `EvictionCandidate`. The cap exists for snapshot bytes and contacts; a held body
  sends no sample and meets nothing, and there is at most one per player. **At the release** it counts as any awake
  body, with no cap test: the awake count can exceed 64 by at most the player count (8 players: 8 more samples of
  31 B in a snapshot, 248 B, est.), until those bodies rest. Accepted: evicting a resting body for a drop would
  despawn something a player sees for an object the player chose to put down.
- **Joiners:** the baseline's spawn of a held body carries `kObjectHeld`; the joiner poses it from the holder's
  remote record.
- **Despawn:** the server's `on_destroy<NetId>` observer ends the hold (the player's held entity is a cache checked
  every tick against the `scene::Held` component, which is the truth); clients drop it with the copy.

### The prompts

`HudCanvas` under the crosshair, play mode only:
- While holding, always: `[E] Drop   [LMB] Throw` (white). The LMB part tells the player that LMB no longer blasts.
  While the holder's own pose is pinned (the client's `HeldPose::pinned`, its own computation), the line is amber: a
  warning before the 30-tick drop.
- `Holdable` target: `[E] Pick up`. `HeldByOther`: greyed `Held`. The body under the player's own feet: greyed
  `Occupied`. Mechanisms: the existing prompts.
- A refused pick-up shows its reason for 1 s (`Busy`, `Out of reach`), as a refused use.
- An auto-drop of the player's own object shows a line for 1 s: `Dropped - blocked` (`Pinned`), `Dropped - hit`
  (`Damaged`). `EntityRelease` carries the reason.

### Budgets

| Item | Budget | CPU (est.) | Network |
|---|---|---|---|
| Held objects | one per player | `StepHeld` 20-60 us per held body per tick (the thick clamp: 2-4k voxels at 0.1 m; the swept-box tests of near mechanisms); a client also poses remote holds per tick | 0 in snapshots; +1 B per remote record (`holdPitch`) |
| Pick-up, release | at most one pick-up per 12 ticks per player | `FindUseTarget` with bodies: one `QueryBodies` over the ray's box (a scan over every body today, est. 10-40 us with 300 bodies) and one pick-up path sweep per holdable candidate (est. 20-40 us) | 12 B + 44 B per pair; a spammer 280 B/s, a normal player under 50 B/s, charged to the 12 kB/s entity budget |
| Release fix-up | per release | one oriented-box voxel test (about 1.4k voxels for a crate at 0.1 m, est. 30 us), one `QueryBodies` and the player capsules; at most 10 when pressed against a wall (0.3 ms, est.) | - |
| Release over the awake cap | at most one body per player | - | up to 8 x 31 B per snapshot until they rest (est.) |

### Rope actions (designed now, built in [mech-2])

- **E on a load rope's cleat releases the rope.** A load rope with the `Cleat` flag has a use box around its world
  anchor (the anchor voxel grown by 2 voxels on every side, no desc bytes). It is a `Rope` candidate in the order
  rule. The server answers it through the cut path: `RopeCut{ropeId, tick, fraction 0 (the anchor end), flags
  Released}`; the constraint goes, the load falls, each client drops the strand whole with the load (a picture). A
  cut or released rope is no target. The busy rule of mechanisms applies per rope (12 ticks); there is no reopen.
  Prompt `[E] Release rope`; a rope with no cleat is not a target (its rope can only be cut).
- **E on a climbable rope is "grab to climb".** It needs climb volumes ([mech-5]): a `StepPlayer` change with a
  `kPlayerRuleRevision` bump and a `PlayerFlag` bit. Until then a climbable rope is not a use target and shows no
  prompt.
- [mech-1] reserves `UseTargetKind::Rope` = 4 and builds `FindUseTarget` as a list of candidate sources (step 2 of
  the order rule), so [mech-2] adds its source and changes no order rule.

### Failure cases

| Case | What happens |
|---|---|
| Two players press E on one crate in one tick | The first in slot order gets it; the other's pick-up is refused `Busy` (`HeldByOther`), its prediction undone |
| A client predicts a pick-up the server refuses, then presses E or LMB again before the answer | The latched edges are discarded with the refusal: no door use, no blast; the player presses again |
| A player tries to pick up the crate it or another player stands on (or stood on in the last 30 ticks) | Refused `Busy`; the own client shows greyed `Occupied` and does not predict it |
| A crate seen through an arrow slit, a murder hole or a gap under a door | Not a candidate (the pick-up path sweep); a held crate is clamped at the wall face and cannot be dropped through |
| The holder walks into a wall | The clamp pulls the object in to `dMin`; the prompt turns amber; pinned for 30 ticks drops it between the holder and the wall, `Dropped - blocked` |
| Holding a crate across a closed door | The door's block box (grown by `r`) clamps it on the near side; it cannot be dropped through |
| A late E (up to 30 ticks) at a moving crate | The eye is the input tick's, the crate is where it is now: a miss is `NoTarget`, as for a late cast |
| E or LMB pressed just after a server auto-drop the client has not seen | Consumed by the echo window: no use, no cast, no charge |
| Use and Cast edges in one input while holding | One throw; both edges consumed |
| The held body's rubble `Lifetime` ends, or it rises over the build ceiling | Despawned: the hold ends on every side with the despawn |
| A closing door's leaf passes a held crate | Not crushed (held bodies are skipped); dropped in the path, it is crushed as any body |
| A blast's sphere meets the held body's model box | Released (`Damaged`) first, then damaged and pushed as any dynamic body |
| A blast whose sphere box meets the held body's bounds but whose sphere misses its model box, or a push without damage | It stays held |
| The holder dies or leaves | `HolderDead` / `HolderGone`: it falls where it was |
| A client loses `EntityHold` or `EntityRelease` | Impossible on the reliable stream; a mismatch from a bug is found by the entity list (`HeldTerm`) and repaired by `ModelRequest` |
| A drop onto a resting stack or onto a player | The fix-up steps the pose back until it meets no resting body and no capsule; none clear: Jolt's penetration recovery, as before |
| The object is released overlapping the holder (no clear pose) | In a session it is Rubble while it falls (no character meets it); at rest it is Dynamic, as any body that lands on a player today |
| A held object near the play bounds | Its centre is clamped into the play area: it cannot be dropped past the bounds (a throw still can) |
| Many holders drop at once at the awake cap | The awake count exceeds 64 by at most the player count until they rest (argued above) |

### Tests of the use key

GPU-free (VortexSimTests, suite `Hold`; [mech-1] card 4):
- `Hold.CrateIsHoldableBigThingsAreNot` (the crate at 0.1, 0.125 and 0.15 m; a 1.3 m box, a 0.9 m solid stone cube,
  a 20-voxel piece at 0.1 m, a mechanism, a kinematic body, a held body: not; `replicatedOnly` refuses a body with no
  NetId; the shape test passes a kinematic copy that the owner test refuses).
- `Hold.NearestTargetWins` (a crate in front of a lever, then behind it; an exact tie goes to the mechanism).
- `Hold.ClosedDoorHidesACrateBehindIt` (closed: the door; open: the crate).
- `Hold.HeldByAnotherIsBusy` (kind `HeldByOther`; the door behind it is not the target).
- `Hold.CannotPassASlit` (a wall with a 0.2 m slit, a crate behind it: not a candidate; a held crate looking through
  the slit is clamped at the wall face).
- `Hold.CannotPickUpWhatAPlayerStandsOn` (`BeginHold` with the body under a player: `Busy`).
- `Hold.PoseFollowsTheHolder` (exact centre and yaw; the anchor 0.5 m below the eye; the pitch clamp at 45 deg).
- `Hold.WallClampsTheDistance` (terrain and a closed door's block box; pinned below `dMin`).
- `Hold.PlayAreaClampsTheHold` (a bounded play area: the centre stays inside).
- `Hold.HeldBodyMeetsNothing` (a held crate posed through a resting crate and a walking player moves neither; a
  default-mask raycast through it misses it).
- `Hold.DropKeepsPoseAndHolderVelocity`, `Hold.ThrowAddsSixAlongTheLook`, `Hold.ReleaseStepsOutOfAWall`,
  `Hold.ReleaseAvoidsBodiesAndPlayers`.
- `Hold.PinnedForThirtyTicksDrops`, `Hold.BusyAfterAChange`, `Hold.DamageReleasesFirst`,
  `Hold.PushDoesNotReleaseOrMove`, `Hold.CrushSkipsAHeldBody`.
- `Hold.TwoSimulationsAgree` (the same inputs give the same poses and releases bit for bit).

VortexNetTests ([mech-1] cards 8-10): `Mechanisms.WireSizes` (hold 12 B, release 44 B, held spawn +7 B, remote
record +1 B, refusals), `Hold.ServerPicksUpAndDrops` (no samples while held; the track not resting; release,
samples, rest), `Hold.ThrowRunsNoCast`, `Hold.HolderDeathDrops`, `Hold.HolderLeavingDrops`, `Hold.NotCountedAsAwake`,
`Hold.NoOneLosesTheirFloor`, `Hold.AutoDropEchoIsConsumed`, `Hold.PredictedPickupIsConfirmed`,
`Hold.RefusedPickupIsUndone`, `Hold.RemoteHolderPosesTheObject`, `Hold.LateJoinerSeesTheHold`,
`Hold.EntityCheckCoversTheHold`. `Movement.NoCorrectionAbove2cmOnFlatGround` stays at 0.

Headless ([mech-1] cards 6 and 11): `--hold-script` (offline: pick up a crate, walk 5 m, drop it, pick it up, throw
it; logs `Hold: picked up at tick N`, `dropped at (x, y, z)` (the model box's centre), `thrown at v m/s`), and
`vortex_server ... --bot-hold P` (bots pick up, carry, drop and throw crates: `Hold: bot N: P pickups, D drops, T
throws, R refused`, `hashes equal`, `agree`). Windowed (the owner's): E picks up a crate, it follows the view below
the crosshair, E drops it, LMB throws it, and the crate hangs about one ping before it falls.

### Decisions refined

1. **Holdable by volume, not by mass (80 kg).** The crate's physics mass is about 750 kg at 0.1 m voxels, 965 kg at
   0.125 m and 1270 kg at 0.15 m (its iron edges: 80 of 296 voxels at 7800 kg/m^3), so an 80 kg rule would exclude
   the main holdable. A solid-volume rule (`<= 0.6 m^3`, est.) is integer, the same for every voxel size within 10 %,
   and still excludes a solid stone cube. A material-weighted rule is left for play tests (open question 16).
2. **Auto-drop when pinned, not when "0.5 m short".** "0.5 m short of the hold point" fires for a crate at any wall
   near the end of its reach (a corridor end) while the object is still clear of it, and for a small object it fires
   at another distance. "Pinned" fires only when the object would have to enter the wall. Same 30 ticks.
3. **A held body occludes and answers `Busy`** (the order rule): it removes the surprise door use after a refused
   prediction.
4. **Not counted in the awake cap** (argued in "Prediction and replication"); a release may exceed it by one body
   per player.
5. **Hold pitch clamped to 45 deg and sent as one byte per remote record**, so other clients pose a held object at
   the holder's height without the full pitch; the hold pose uses the same byte on every side.
6. **A thick clamp, not the decision's one ray** ("terrain clamping keeps it out of walls" needs it: a thin ray
   passes slits and holes), and a pick-up path test.
7. **The hold point at chest height** (`kHoldDrop` 0.5 m below the eye), not on the crosshair.
8. **Not a player's floor:** a pick-up of a body a player stood on in the last 30 ticks is refused.
9. **The hold centre stays in the play area**, so a drop cannot remove an object from the round.
10. **"Damaged" means the blast's sphere meets the posed model box** (an oriented box test), not only the sphere's
    AABB: a corner overlap of the boxes alone does not release.
11. **Edges in the echo of an auto-drop are consumed**, and a Use and a Cast edge in one input while holding are one
    throw.
12. **The minimum size is `SceneConfig::debrisBodyVoxels`**, not the constant 25 (that integer is 25 only at
    0.125 m).

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
  gate's; it is gone at arrival). A body a player holds is skipped (a ghost; "One use key: E", "Drop, throw and
  auto-drop"); dropped in the path it is crushed as any body.

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
[notes/net.md](../notes/net.md) section 2 when [mech-1] lands (magic.md amendment 4). The owner put [mech-1] before
magic's [mag-2] (2026-11-21), so these values land first and take the next free numbers then; `PlayerButton_Use`
keeps 16 (magic's Spell edge is reserved at 32). `Mechanisms.WireSizes` pins the sizes and whatever values land.

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

### Ropes: the offline part as built (2026-11-24)

The owner asked for the offline part of ropes first. Built, in `vortex.physics` and `vortex.sim:rope`:
- **Physics:** `PhysicsWorld::CreateDistanceConstraint` (a world-fixed anchor point to a dynamic body's point, min 0,
  max the length), `DestroyConstraint`, `ConstraintBody`; `DestroyBody` and `Shutdown` remove a body's constraints
  first. `ConstraintId` follows the `CharacterId` pattern (slot and generation).
- **Data:** `scene::Rope` on an entity of its own: the anchor voxel (its bottom face centre is the anchor point), the
  load or the mechanism, the attach point (a load's current model voxels; a chain's mechanism's original model
  voxels), the length, the flags (`RopeFlag_Cleat`, `RopeFlag_Chain`), the constraint and the cut state.
- **Load ropes and chains:** `sim::CreateLoadRope` (as long as the ends are apart now), `sim::CreateChainRope` (no
  physics: drawn between the anchor and the mechanism's pose).
- **Cutting:** `CutRopesAlongRay` (positions quantised to 1/16 voxel; within `kRopeRadius` + 0.1 m of the ray, nearest
  first, at most 4 per cast; ropes never stop the ray) and `CutRopesBySphere` run in the rules seam's cast path
  (`ActionOutcome::ropeCuts`), so the server's cast path (the session part) gets them unchanged. A cast into the sky
  still cuts the ropes on its ray (the offline game calls `CutRopesAlongRay` on `Missed`). `StepRopes` every tick:
  an anchor voxel turned to air cuts at 0, a lost load cuts at 1, and a load whose body `Scene::Damage` re-made keeps
  its rope while the attach point still lies in a solid voxel of its model (the constraint is re-made on the new
  body), else it is cut at 1. Decided by floats on quantised points on one machine; the session part must make the
  ray and sphere tests integer as "Cutting a rope (server, integer)" says before the server uses them.
- **A cut chain jams its portcullis:** a segment to `minPos` at its drop speed with `MechanismFlag_Jammed`; the lever
  then answers `Locked`.
- **E on a cleat:** a `Rope` candidate in `FindUseTarget` (the anchor voxel grown by 2 voxels), prompt `[E] Release
  rope`, `ReleaseRope` (busy 12 ticks; a cut rope or one without a cleat is no target). A body that hangs on an uncut
  rope is not holdable.
- **Drawing:** `vortex::GatherRopes` draws each rope as one stretched instance of a baked 1 x 1 x 8 link model (planks
  for ropes, iron for chains), 4 cm thick (6 cm for chains); a cut rope leaves its anchor part hanging straight down.
  No `rope` material yet (no `MaterialTableHash` change), no cosmetic strands, no cut halves.
- **The yard:** a gallows behind the gate wall with a cage on a plain rope and a chandelier (torches on an iron ring) on
  a cleat rope, and the portcullis chain. `--rope-script` checks all of it.

Not built: `EntityKind::Rope`, `RopeCut` on the wire, the integer server tests, cosmetic strands with wind,
`--rope-stress` and the GPU measurement at the cap, ropes that slide or winch.

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

Measured on the Windows PC (Core Ultra 7 356H iGPU, not the UHD 620), [mech-1] card 7, `--mechanism-blast` at 0.125 m: the
yard door is freed after 7 casts (power 6: two on the leaf, then the hinge column, which is the door's hinge box over its
whole height, cast along in 0.45 m steps), the lever breaks after 1, and a survivor's `ObjectModel` volume is 170-260
bytes per hit. `Mechanism.DoorTakesNHits` (an all-planks door whose hinge box is its middle metre): 5 casts. The yard
door's hinge column is wood, not iron: iron (hardness 8) never breaks to the cast power 6, so an iron hinge could never
be freed. `--mechanism-script`: `StepMechanisms` at most 11-29 us per tick with 3 moving (the summary line's `step
max`). The UHD 620 numbers are the owner's (owner check 24 runs on that laptop).

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
- Holding (suite `Hold`, sim and net) and its headless runs: "One use key: E", "Tests of the use key".

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
| **1. Door and gate** (the smallest fun slice). M-a offline, M-b session | M-a: `scene::Mechanism` (with `modelOffset`), `sim` segment (`durationTicks`, two `uint32` ease tables) / pose / layer rule (ghost and rising) / `FindUseTarget` (occluders, info targets, authoring rule) / `ApplyUse` (closing wins, lead argument) / `PoseMechanisms` / `StepMechanisms` (crush) / arrival push-out; kinds Door, Portcullis, Lever; the damage rule (aimed hits, 0.3 m, hardness, no terrain carve, hinged part kept, Warded) and freed / broken; `PlayerButton_Use`, crate on Q (every file in the list above); HUD prompt, info lines, lever throw and lead cue; the yard in the default offline game (`--no-mechanisms`; scripts' numbers confirmed or re-recorded); **holding** ("One use key: E", added 2026-11-24): the order rule over kinds, `IsHoldable`, the hold pose and clamp, `Layer::Held`, drop / throw / auto-drop, the prompts, `--hold-script`. M-b: `EntityKind::Mechanism`, `MechanismMove`, owner block, `PlayerUse` on both paths, the per-use lead, the client `mechanisms` map, server resting tracks, the entity-budget rule, baseline with segments, EntityCheck / EntityList term, the yard in `playground.session`, `--bot-use`; for holding `EntityHold`, `EntityRelease`, `kObjectHeld`, `holdPitch`, the client's predicted pick-up, `HeldTerm`, `--bot-hold` | All stage-1 tests; the yard headless run and `--mechanism-blast` exit 0 with their lines (hits to free each door recorded) and a screenshot with the door open and the gate up; the default game run's three lines; the lossless and lossy drills as above; `tools/session-check.sh` still PASS; the owner's checklist in `game-ui.md` gains "E opens the yard door, the lever runs the gate, Q drops a crate; E picks up a crate, E drops it, LMB throws it" (windowed: the owner's); `--hold-script` and the `--bot-hold` drill with their lines | 12-13 days (M-a 7, M-b 5-6; holding adds about 3 days, est.) |
| **2. Ropes** | Constraint wrapper; load ropes (a cage and a chandelier in the yard); `EntityKind::Rope`, `RopeCut` by ray, by sphere, by anchor and by despawn; the portcullis chain drawn, and **its cut drops and jams the gate**; `GatherRopes` instances and the link models (a `rope` material: `MaterialTableHash` and `buildHash` change); cut halves; `--rope-stress`; **E on a load rope's cleat releases it** (the `Rope` use-target source, `RopeCut` with `Released`, prompt `[E] Release rope`; "One use key: E", "Rope actions"). **Last, cut first if the stage overruns:** decorative strands with wind and sleep (`fx::RopeStrand`, the 24-strand cap) | Rope tests; `--rope-script` exits 0; rope GPU and BVH cost measured at `--rope-stress 26` with and without `--no-ropes` and within budget; the drill with `--bot-casts` cutting ropes ends `hashes equal` and `agree` | 6-7 days |
| **3. Siege kit** | Winch (hold, crew once per 12 ticks, release hysteresis, HUD ring), drawbridge (riders fall when it rises after the lead cue), both drawbridge chains cut drop it, Button, auto-return sally door, `Locked` from the session file; gate-under-fire bytes measured | Tests; a yard gatehouse drill with bots holding the winch, cutting chains and shooting the gate: `modelsToRubble` counts no mechanism, event bytes per hit recorded | 4-5 days |
| **4. Placement** | The watch tower door in the default playground (tower / neck scripts re-measured and their CLAUDE.md numbers updated); worldgen W3 gatehouse kit sockets and keep loads from plan indices | Default runs unchanged except the recorded numbers; a generated fort with working gates in a session | 3 days + W3 |
| **5. Later (after M2)** | Climb volumes for ropes and ladders, with **E on a climbable rope = grab to climb** (a `StepPlayer` change: `kPlayerRuleRevision` bump); a drag action for beams, planks and ladders; team locks (`ownerTeam`); crush damage to players; warding release; a player rope tool if still wanted | Decided then | - |

Total for stages 1-4 without W3: est. 25-28 days (the first version said 19-21; the review's rules cost about 3-4
days, mostly in stage 1; holding, added 2026-11-24, about 3 more).

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
| Holding by physics (a spring or a constraint that pulls the body to the hold point) | The character would push the body and the body the character: in a session that breaks "characters move no bodies", and Jolt is not deterministic across machines, so the holder would be corrected. Posing is exact on every side and meets nothing |
| A held object that stays solid (a shield, a step) | Every client would need the holder's exact pose at the predicted tick; a solid held crate carries its holder upwards (carry-flying) and rams players. A ghost has none of these |
| A weight slowdown while holding | A change to `StepPlayer` and a `kPlayerRuleRevision` bump for a small gain |
| Holdable by mass (80 kg) | Voxel physics masses: the crate is 750-1270 kg by its iron edges ("Decisions refined" 1) |
| A throw as a refused cast (`CastRejection`) | It is not a refusal; it would show "Cast not run", and [mag-2] plans `CastRejection` 6-10 |
| E as "use, else drop the crate" (context) | Still rejected: Q keeps the crate. Holding is a target kind, not a fallback |
| A one-ray hold clamp (the decision as first written) | It passes arrow slits, murder holes and blasted holes narrower than the object: crates go through walls. The thick clamp and the pick-up path test cost about 20-60 us more per held object (est.) |
| A client copy made Dynamic on release | Client copies are kinematic and sample-driven; a simulated copy would differ from the server's body and fight `MoveCopies` |
| Evict a resting body when a release exceeds the awake cap | It despawns something a player sees for an object a player chose to put down; the overshoot is at most one body per player |

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
16. **Holdable by solid volume only, or weighted by material?** -> Volume (0.6 m^3, est.) in [mech-1]: the crate's
    voxel mass is not a game weight. If play tests want iron things too heavy to lift, a rule on `sum(density)` with
    a cap tuned to the crate comes later (no wire change: the rule is the same function on every side).
17. **Hold distance 1.5 m, hold drop 0.5 m below the eye, throw speed 6 m/s, pitch clamp 45 deg?** -> Start there;
    the owner's windowed check tunes them (none is wire format; `kThrowSpeed` and the clamp change only the server's numbers and the client's
    prediction, which follows the same constants in one build).
18. **Should a thrown or dropped object deal damage?** -> Not in [mech-1]. With [si-2]'s impacts, as for any falling
    body; never a special rule for held objects.
19. **The hang of a predicted drop or throw (about one ping plus the body delay): keep it?** -> Keep it in [mech-1]
    and name it in the owner's check. If the owner finds throws laggy, the next step is a holder-side picture that
    keeps the copy in `Layer::Held` on a ballistic path and blends into the samples over 100 ms (no wire change).
20. **Should a session cast stop on a held object?** -> No in [mech-1]: every cast passes through it on every side,
    and the chest-height hold point keeps the holder's head visible. If play shows held crates read as cover, the
    session cast mask gains `Layer::Held`, so a shot at a held crate explodes on it and releases it (`Damaged`): no
    wire change, but the server's pose of the crate lags the shooter's view as for any moving body.
