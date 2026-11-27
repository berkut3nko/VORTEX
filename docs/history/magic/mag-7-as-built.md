# [mag-7] as built (2026-11-27)

What [mag-7] built, and where it differs from [design/magic.md](../../design/magic.md) as it stood before the stage.
It follows [mag-8]'s casting rules (mana, gestures, effects on arrival). The owner re-scoped the stage on 2026-11-27:
no `extraLoad` in Part 4b's load check and no [si-2], so the slam fells no floor.

## What it builds

- Players (`sim::kPlayerRuleRevision` 6): the Pin zone (kind 3: while live and inside, `pinLeft = max(pinLeft, tail)`
  with a tail of 30 ticks; not during the control guard) and Meteor Leap's own move. `sim::LeapPushOf` turns the look
  into a launch of 12 m/s with its pitch clamped to 15-80 degrees up; `sim::StartLeap` puts it into `push` (the
  vertical part is taken by the next step as a launch) and sets `PlayerStatus_Leaping` (bit 6) and
  `PlayerStatus_Lifted`. While leaping the wish is ignored, the slick rule is off and the push does not decay; the
  first step after the launch that ends on the ground ends the leap and zeroes the horizontal push.
  `sim::StartSpellMove` starts it at the Spell edge, as a Dash.
- `sim::LeapAimToward`: the look whose leap lands at a target (the lower arc when it is at least 15 degrees up, else the
  higher one). Only a client calls it, to choose the Spell edge's look; the server never solves it.
- Sim: Meteor Leap (spell 2) in `sim::CastSpell`. The server traces the launch of the input's look with `sim::TraceArc`
  from the feet after the edge's step (or now, for a script's `casterState`), the trace's half step of gravity
  corrected so that the trace and the character's steps agree tick for tick. The landing is the floor under the cell
  in front of the face the arc meets. At max(the landing tick, T_run + the lead): an Impulse (5 m/s away, 2 m/s up)
  and a Pin y-cylinder of 2.5 m round the landing for enemies, 15 damage with the Pin, the Shatter flood (r 1.5 m,
  0.5 m deep) and up to 8 bodies kicked. The Pin is dispellable (a Punch ends it). 45 mana, no projectile.
- Server: no fall damage on a landing that began `Lifted` (a leap, an Updraft or a spire's Lift).
- Wire: no change. `PlayerStatus_Leaping` travels in the owner block's status byte, which carried every bit already;
  remote records keep bits 0-3. `kProtocolVersion` stays 15; `buildHash` changes through the spell table and the
  rule revision.
- Game: `--spell-demo meteor-leap` (a yard south of [mag-4]'s: a stone block 1.5 m tall with a cube of ice beside the
  landing; the offline player leaps 9.5 m onto it) logs `Leap: X m in N ticks, landed D m from the target, slam pinned
  P, shattered S`; `--siege-matrix` runs it as its 12th row. The offline game and the session client give a Meteor
  Leap's Spell edge the look `sim::LeapAimToward` solves for the release point. `SpellFx::LeapTrail` draws the caster's
  trail in Pressure's colour and a ring of dust where it lands.

## Where it differs from the design

1. **No `extraLoad` and no `--crush-demo`** (the owner's re-scope). The slam carves nothing and runs no load check.
2. **The launch lands at the release point.** [mag-8] says the leap lands at E; the input carries only a look, so the
   client chooses the look whose arc lands at E, and every peer launches along it. The pitch clamp is 15-80 degrees,
   not 15-60: with a fixed speed of 12 m/s a short leap needs a steep arc (80 degrees reaches 5.0 m on flat ground;
   nearer targets overshoot to there). Out of reach, the leap goes at 45 degrees toward E.
3. **The leap is its own status.** The design gives the flight to `push` with its air decay of 1 cm/s per tick. As
   built the push does not decay while `Leaping` and the wish is ignored, so the server's integer trace and the
   character agree; the horizontal push ends at the landing instead of decaying on the ground.
4. **The slam has a knockback.** The owner's scope asked for one: an Impulse cylinder at the slam's tick, before the
   Pin in zone order. The pinned player's extra fall (`-2 g dt` in the air) is not built.
5. **The landing tick is the trace's.** The slam starts at max(the traced landing, T_run + the lead); the caster lands
   within a tick or two of it (`Magic.MeteorLeapLandsAtTheTarget`). A caster knocked off its arc does not move the
   slam.

## Tests

`Magic.MeteorLeapLandsAtTheTarget` (flat ground 9.5 m away and a block 1.5 m high 8 m away: the caster within 1 m of
the target, the slam within 0.75 m and 3 ticks of the landing), `Magic.MeteorLeapSlamPinsAndShatters` (an enemy
knocked back, pinned and dealt 15, an ally untouched, exposed ice and rime shattered, the tail and the guard after),
`Player.AMeteorLeapLaunchesAlongTheLookAndLandsAtItsTarget`, `Player.AMeteorLeapReplayedGivesTheSameStates` (with a
Pin on the way) and `Movement.MeteorLeapNoCorrection` (G16's link: no correction for the caster, both sides within
5 cm).

## Measured

On the Windows PC (not the UHD 620), `--spell-demo meteor-leap`: 9.95 m in 58 ticks, landed 0.45 m from the target at
0.125 / 0.1 / 0.15 m; the slam shattered 64 / 150 / 36 ice voxels; work 6 601, 0.65 ms, 98 B.
