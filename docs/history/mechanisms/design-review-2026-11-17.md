# Mechanisms history: the two critics' review of the design (2026-11-17)

Moved verbatim from `docs/design/mechanisms.md` lines 866-897 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

## Review of the design (2026-11-17)

Two critics read the first version. Every finding was checked against the code named in it (working tree of
2026-11-17) and applied unless noted.

| # | Finding | Verdict | Where |
|---|---|---|---|
| 1 | A damaged gate goes `ToRubble` when the entity budget is spent; no budget rule for `MechanismMove` / `RopeCut`; the 24 kB/s figure is the wrong budget | Confirmed (Server.cpp:427-432, :639-641, :1939, Server.cppm:46: 12 kB/s). Applied: always send, charge, skip dents only at the credit floor | Replication, "The entity byte budget" |
| 2 | Stage-6 copy code overrides the timetable layer on both sides | Confirmed (Client.cpp:690, :851-855, :955-964, :1051-1056; Server.cpp:396, :408, :494, PW.cpp:697; Scene.cpp:66-69). Applied: client `mechanisms` map, server resting tracks, layer re-applied after Damage | Replication |
| 3 | `ticksPerUnit16` cannot express door or lever speeds; the ease never reaches `targetPos` | Confirmed (arithmetic). Applied: `durationTicks`, `uint32` table to 65536, `k >= D` gives the target | The model |
| 4 | After a dent the model-space pivot and boxes are in the old model space | Confirmed (Scene.cpp:586-602, Client.cpp:832-850). Applied: `modelOffset` | The pose after a dent |
| 5 | The 15-tick lead covers less RTT than claimed; 0 corrections under loss cannot pass | Confirmed (Client.cpp:274-279, Prediction.cpp:37-40). Applied: the condition corrected, lossless drill for 0 corrections, late events counted under loss | The lead; Tests |
| 6 | Ring of 4 too small; baseline by opSeq; winch spam | Confirmed. Applied: 8 segments with a static_assert, baseline by tick with 1..8 segments, winch once per 12 ticks with hysteresis | The model; Press and hold; Replication |
| 7 | Repair from the `EntityList` is an unstated wire change against 7b | Confirmed (Messages.cppm:365-378, Client.cpp:1373-1415). Applied: the term in `modelHash`, repair by `ModelRequest` | Replication, Checks |
| 8 / 26 | The lever-layer reasoning is wrong (25 voxels Debris threshold, 32-64-voxel lever is Dynamic; a Debris lever could not be shot) | Confirmed (Scene.cppm:41, Scene.cpp:66-69, Server.cpp:776-778). The two findings are one; applied: the mechanism rule overrides the size rule, the throw is justified as a picture | Collision; What the client shows |
| 9 | `replicatedPieceVoxels` is 262, a door dies in one split | Confirmed (Simulation.cppm:127-130, Scene.cpp:556-576). Applied: the hinged piece keeps the entity, Kinematic, exempt; 3-voxel doors | The damage rule |
| 10 | A late Use would take the current eye | Confirmed (Server.cpp:851 passes `pl.state`; casts use `StateAt`, :806-808). Applied | What the server does |
| 11 / 17 | Non-usable mechanisms do not block the use ray: a lever reachable through a closed gate | Confirmed (design text). The two findings are one; applied: every mechanism's block box occludes, plus the authoring rule | The look ray |
| 12 | Push-out on every tick of any overlap pushes riders and leaners | Confirmed (design text). Applied: arrival window, solid voxels deeper than padding, push axis only | Collision |
| 13 | A rope survives its load's or mechanism's despawn | Confirmed (Replication.cpp:122-127, Blast.cpp:641-646). Applied: `on_destroy<NetId>` emits `RopeCut` first | Replication, "Despawns cut ropes" |
| 14 | One cast destroys a door, a lever or most of a leaf | Confirmed (Blast.cpp:633-690, Scene.cpp:493-507, Player.cppm:79-80). Applied in stage 1: aimed hits only, 0.3 m, hardness, Warded; plus a rule the critics did not raise: a hit on a mechanism carves no terrain (otherwise its anchors are aired by the same cast) | The damage rule |
| 15 | A rising portcullis is walk-through from its first tick | Confirmed. Applied: slides rising out of the passage stay Dynamic | Collision |
| 16 | Busy 12 lets a spammer keep a door moving | Confirmed. Applied: closing wins, reopen after arrival + 20 | What the server does |
| 18 | 350 ms to visible motion, no sound, 250 ms is 6x the LAN need | Confirmed (no audio module; smoothstep e(6/36) about 0.07). Applied with one correction: the per-use lead uses each player's **full** RTT plus its measured input margin, not RTT/2 + jitter as proposed (finding 5 shows the client's predicted tick on arrival is about T + RTT + target); ease-out; the lead cue | The lead; What the client shows |
| 19 | Closing doors launch or tunnel crates | Confirmed (design gap). Applied: the crush rule | Collision |
| 20 | A direct shot at a rope does not cut it; a chain's 3-hit counter is invisible | Confirmed (Server.cpp:770-800). Applied with one change: the ray cuts every rope it passes but ropes do not take the cast (the proposal placed the blast at the rope, so a rope would eat shots at enemies behind it); chains 1 hit | Cutting a rope |
| 21 | The owner sees nothing until stage 4 | Confirmed. Applied: the yard in the default game from M-a and `playground.session` from M-b | How they are placed |
| 22 | Stage 2's ropes change no play; decoration costliest | Applied: the chain jam moves to stage 2, decorative strands last | Stages |
| 23 | Rope draw cost not measured at its cap; simulated cap above drawn cap | Confirmed (Renderer.cpp:1185-1209). Applied: `--rope-stress 26`, 24 simulated strands | Drawing a rope |
| 24 | Gates give no prompt; `Missed` reads as a shooting term | Applied: `Info` targets, `NotUsable`, `Out of reach`, the 2 s line for out-of-view targets | The look ray; The prompt |
| 25 | A cast at a moving door is tested against the server's current pose, not the shooter's | **Rejected in part.** A cast on time is queued and resolved in its input's tick (Server.cpp:2116-2117: `StepPlayers` then `RunCasts`), when the server's kinematic body is already at `pose(inputTick)` from the previous `StepMechanisms`, and the client draws the mechanism at its predicted tick, i.e. the same pose (within a frame's alpha). They differ only for a cast the queue holds (`castCredit` / `eventCredit`, Server.cpp:1940-1951) or a late input, and those are resolved against the world at dequeue for terrain and every other body alike (networking's rule). A second trace path through `trace.glsl` objects at an older pose is not worth it for a 0.6 s door; open question 15 keeps it under watch | Open questions |
| 27 | Moving the crate to Q misses the two-PC checklist and the game's tool strings | Confirmed (`docs/two-pc-test-windows.md:450`, `src/main.cpp:415`, `:453`, `src/Session.cpp:240-241`, `:412`, `:471`). Applied | E becomes "use"; Coordination |
