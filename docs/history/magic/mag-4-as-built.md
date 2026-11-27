# [mag-4] as built (2026-11-27)

What [mag-4] built, and where it differs from [design/magic.md](../../design/magic.md) as it stood before the stage.
It follows [mag-8]'s casting rules (mana, gestures, effects on arrival).

## What it builds

- Core: palette id `slag` 52 (`Conjured | Hot`, hardness 7, knit's density and bonds, emissive 5) and
  `MaterialFlag_Hot` (bit 6). The Turning maps Slag (4) and Set (5) get their rows; Unknit and Ablate gain slag.
  Bore cost of slag 112.
- Sim: the integer arc (`sim::TraceArc`, `sim::ArcLaunch`, `sim::LobLaunch`, 1/4096 voxel); the Flow operator
  (`sim::FlowOperator`, `sim::FlowKeyOf`); `sim::RiseShapeOf` (stairs, bridge or pillar from a Rise's points);
  `KnitRegistry::OwnerAt`; the hazard scan, the splash, the rune trigger test and the rune's burst inside
  `sim::RunScheduled` (`ScheduledResult` gains the zones it starts and cancels, the bursts and the hazard hits).
- Spells: Molten Pour (0), Rune Trap (3), Rising Spire (4) and its Rise stairs and bridge, Rebar Lance (8). The table
  marks all four built.
- Net: the server emits the zones `RunScheduled` starts and cancels, deals each damage with its cause (Lava for the
  hazard scan) and logs `Rune: burst under bot N (zone Z)`. The zone duration limit of the decoder rises from 600 to
  3600 ticks (a rune lasts 60 s). No byte changes: `kProtocolVersion` stays 15; `buildHash` changes through the spell,
  turning and material tables.
- Game: the Flow projectile flies the parabola of its flight under 9.81 m/s^2; `SpellFx::Runes` draws each rune (a
  bright ring and core for the viewer's team, a faint glow at its middle for an enemy's, dimmer before it arms), in
  offline play and in sessions. `--spell-demo` gains `molten-pour`, `rising-spire`, `rebar-lance`, `rune-trap`,
  `rise-stairs` and `rise-bridge` in a yard south of Frost's; `--siege-matrix` runs them after Metal's and Pressure's
  first rows (11 rows), with `--frames 2800`.

## Where it differs from the design

1. **The lob.** A Flow spell with gesture points is lobbed at the release point: the horizontal speed is Flow's
   20 m/s, raised so that the flight is at most 60 ticks, and the vertical speed is solved so that the arc is at the
   release point's height at that tick. A tap (or a script with no points) launches along the aim at 22 m/s. Either
   way the integer trace decides the hit, and the flight is the arc's ticks (at most 60).
2. **No `Contact` role.** The Flow operator returns `Fall`, `Run`, `Front` and the landings; no built spell reads
   `Contact` yet ([mag-5] adds it with pitch).
3. **The hazard scan reaches two voxels.** A fill leaves a player's capsule box grown by one voxel free, so slag
   poured round a player lies one voxel further out than "within one voxel" would see. The scan runs only while slag
   can lie (until the last Set), every 6 ticks, 1 damage per scan (10 HP/s), credited to the slag's caster and never
   to its own team.
4. **The band timing rule from a model of the Lift.** Each band waits for the tick at which a rider lifted from the
   floor reaches the band's top plus 2 voxels, from the Lift as `StepPlayer` computes it (the target eased over the
   coasting height, gravity), in double on the server. Band 0 lands 8 ticks after the zone tick, not 9. The Lift ends
   a lead after the last band.
5. **A spire's bands keep the cells players hold at the cast.** The rider stands in the column; each band is guarded
   on its own tick instead, so the column under the rider is whole.
6. **Ledges on a floor hit only**, out of the column on the side facing the caster (rounded to x or z).
7. **Stairs and bridge geometry.** Each step is two rises thick, so the steps touch face to face; the stairs run from S
   toward E (or away from the eye when E is over S) for ceil(rise / 0.375 m) steps, cut to 10 m along their slope.
   The deck lies at the lower end's ground. At most 6 bands each, landing along the line at 20 m/s after the 0.3 s
   ghost lead. Neither makes a Lift. The mana counts their length; a bridge whose end has no ground within 0.5 m is the
   pillar and costs the pillar's 35.
8. **Rebar Lance.** The upper socket is made where the voxel a metre above the hit is a collider the power carves.
   One knit band fills both sockets and both pegs on the arrival, after the bores. Rising Spire costs 35 mana, Molten
   Pour and Rune Trap 30, Rebar Lance 20 (the design names no tap cost for them).
9. **The climb.** One lance's two pegs stand 1 m apart at the same place, so the lower one is under the upper one: a
   climber stands on the upper peg of each lance only, and a jump (1.04 m measured) reaches the next upper peg 1 m
   up, a little to the side. The demo's three lances, from a ditch at the wall's foot, reach a 4 m ledge (the design's
   estimate was 6.4 m with six usable pegs).
10. **The rune's burst ice** is the floor's skin within 2 m of the rune (the open cell with support under it within two
    voxels of the rune's row, water as floe), nearest first, at most 400, none in a spawn area, landing as a band a lead
    after the trigger.
11. **Breaking a rune** by a carve under its middle is built; Heat and pitch wait for [mag-5].

## Measured

On the Windows PC (not the UHD 620), `--siege-matrix`, 0.125 m: Molten Pour work 6 384, 1.0 ms, 328 slag voxels;
Rising Spire work 3 854, 0.3 ms, 560 B in 6 bands, the rider on top at 6.10 m; Rebar Lance work 4 908 per lance,
0.1 ms; Rune Trap work 642; the stairs 1 440 knit voxels (8 steps, 3 m), the bridge 768 (6 m). At 0.1 / 0.15 m the
demos pass with the same lines (slag 420 / 278, the spire 6.10 / 6.15 m).
