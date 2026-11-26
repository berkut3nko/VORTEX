# [mag-1] as built (2026-11-26)

What [mag-1] built, and where it differs from [design/magic.md](../../design/magic.md) as it stood before the stage.
Engine commits `43d0167` (world ops, spell table, conjured matter), `9553b98` (zones, push and the dash in the player
step), `9ae9254` (a fix found on the way: an open door at rest no longer gets a NaN spin), `9a38534` (the cast
pipeline), then the headless scripts, gate G23 and these documents.

## What it builds

- Sim, no wire change: `sim::SpellTable` and `SpellTableHash`; `voxel::TurningTable` with Knit, Unknit and Ablate;
  `WorldOpKind::MapShape` (7) and `MapMask` (8) as sim values (on the wire from [mag-2]); `voxel::ShapeQ` (sphere,
  capsule, box, y-cylinder); `knit_iron` with `MaterialFlag_Conjured`; site-based blasts (`BlastSite`,
  `BlastOptions::noCarve`), `sim::BlastAt` and the ablative absorb rule; `sim::KnitRegistry` with expiries, the
  conjured-matter caps and site checks on `sim::StructureCredit`; the Burst strip and Barricade Wave's bands, Plume's
  single column; `sim::ZoneSet` (Impulse, Lift, Shove; Rune reserved) inside `sim::StepPlayer` with `push`, statuses
  and the caster's own dash; `sim::CastSpell` / `sim::RunScheduled` (`vortex.sim:cast`) with the capture checks, the
  spawn guard, the zone caps, the dispel and the damage entries for the caller.
- Spells: Updraft (6), Dash (10), Barricade Wave (12), Punch (14).
- Game: the offline player is stepped with the simulation's zones (team 0); `--spell-demo NAME[,NAME...]`,
  `--siege-matrix` and `--camera siege-matrix` (`src/SpellScripts.cpp`); gate G23 in `tools/verify.sh quick`.

## Where it differs from the design

1. **The spells.** The owner's re-scope (2026-11-26): Updraft, Dash, Punch and Barricade Wave instead of Siege
   Cannon and Shockwave. The Pierce operator, `WorldOpKind::CarveCapsule` (9 stays free), the bore cost table and
   their tests move to [mag-3].
2. **`knit_iron` is palette id 44**, the next free id when it landed (the plan had 21; spread and the land's
   materials had taken the ids before it).
3. **`kPlayerRuleRevision` is 3**, not 2: land.md's foliage had taken 2. The later stages shift by one: 4 in [mag-3],
   5 in [mag-7].
4. **The Lift eases over the coasting height.** Below the column's top the Lift's target speed falls off so that a
   lifted player hovers at the top instead of flying past it on the speed it gained (the plan's `max(velocity.y,
   target)` overshot by the coasting height `v^2 / 2g`).
5. **Impulse and Shove.** An Impulse pushes radially from its centre, or from the nearest point of a capsule's axis
   (Dash: enemies beside the path are pushed away from it); only a Shove pushes along its axis (Punch). Decision 15 of
   the plan (a capsule Impulse along its axis) is replaced.
6. **No skeleton of stubs.** Each part was built complete with its tests enabled, in the cards' order.
7. **The left-click blast** goes through `sim::ResolveAction`, which now calls `sim::BlastAt` with the terrain voxel
   the ray hit: the absorb rule holds for the offline blast and, once conjured matter exists in a session, for the
   server's. Without conjured matter it is `sim::Blast`, bit for bit.
8. **The `Magic:` line has no `contact` field**: none of the four operators reads `Contact`. `SpellStats` gained
   `path` and `front` for it. The line is logged 45 frames after the cast, so that `players hit` and `released` cover
   the zone tick and the bands.
9. **The headless yard** is a flat stone pad 20 m east of the plaza with a plank wall, not the stamped siege yard of
   "Headless flags and gates" (that yard waits for the spells that need walls, a moat and a tower). The scripts cast
   without charges and form cooldowns. Updraft, Punch and Barricade Wave are cast by a script of team 1, so the
   offline player is an enemy; the player casts its own Dash. `--spell-demo` takes a list of names.
10. **A fixed camera on a stepped player.** Under `--camera siege-matrix` the colliders follow the camera, not the
    player, so the scripts cook the player's reach before each step (`TerrainColliders::FlushBox`, as a session
    does). Without it the gate held the dashing player for 32 ticks.
11. **Gate G23** is `--siege-matrix` with Dash and Punch at 0.1 and 0.15 m, in tier quick (the design proposed G21;
    G21 and G22 were taken by the GPU baseline and the bot fight). The matrix also writes `<name>-wall.png` at script
    frame 180, so the screenshot shows the barricade and the lifted player (the barricade has crumbled by the last
    frame).

## Measured

On the Windows PC (not the UHD 620), `--siege-matrix --after-fill`, 0.125 m: Barricade Wave work 1654, 0.75 ms, 1332
knit voxels in 4 bands; Updraft work 680, 0.04 ms, lifted +10.18 m; Dash work 664, 0.26 ms, 7.90 m in 18 ticks;
Punch work 208, under 0.01 ms at the cast, 224 planks voxels broken, the player shoved. Dash at 0.1 / 0.15 m: 7.94 m
in 18 ticks; Punch broke 502 / 150 planks voxels. Barricade Wave at 0.1 / 0.15 m: 2025 / 930 knit voxels.
