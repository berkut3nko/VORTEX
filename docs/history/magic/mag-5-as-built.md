# [mag-5] as built (2026-11-27)

What [mag-5] built, and where it differs from [design/magic.md](../../design/magic.md) as it stood before the stage.
It follows [mag-8]'s casting rules (mana, gestures, effects on arrival). The owner re-scoped the stage on 2026-11-27:
no fire spread ([spread-S1] not built), so no `SpreadStep`, no climbing fire and no timber ignition.

## What it builds

- Core: palette ids `knit_iron_hot` 53 (`Conjured | Hot`, hardness 3.5, knit's bonds) and `iron_hot` 54 (`Hot`,
  hardness 4.0, iron's bonds), and pitch's four burn stages `pitch_b1..b4` 55-58 (`Conjured | Film`, hardness 0, a
  dark film that glows). `MaterialFlag_Film` (bit 7) and `voxel::kNonStructuralFlags` (`Liquid | Film`): a film is no
  collider (`TerrainColliders`), is not in the cell graph or the load check, and is weightless.
- The Turning maps: Pitch (6) air -> `pitch_b1`; Heat (7) knit and rimed knit -> hot knit, iron and rimed iron -> hot
  iron, ice -> air, floe -> water, the other rimed ids -> their base; Cool (8) hot -> plain; Rime gains the quench (hot
  -> plain); Freeze turns every pitch stage into ice; Knit and Slag take a film's cell; Unknit takes hot knit and pitch.
  Bore costs: hot knit 56, hot iron 64, pitch 0.
- Sim: `sim::IsOpenId` is air or a film (the operators move through pitch); the server's spell ray steps through films
  (`SpellRaycast` in `Cast.cpp`, at most 8); `PierceParams::preMap` (Meltdown Beam reads metal through Heat); the
  scheduled `Scorch` (damage to enemies whose feet lie in a box, cylinder or capsule, with Burning); `KnitOwnerInfo::spell`.
- Spells, marked built in the table: Pitchfall (1), Wildfire (5), Meltdown Beam (9), Firestorm Sweep (13).
  - Pitchfall: a lob as Molten Pour; pitch over the Fall and Run skin cells and the pool (at most 1 600 at 0.125 m), Heat
    over the support voxels they touch that Heat changes, 20 splash within 1 m. 30 mana.
  - Wildfire: the full Plume (a Rise gesture: from S up to E's height); pitch over its Rise and Run skin cells and the
    Front cells that are skin or join it, Heat on what they touch, 20 to enemies in the plume's box on the arrival. 30
    mana.
  - Meltdown Beam: r 2 voxels, power 80, budget 14 400 (scaled), at most 64 slices; a Heat capsule of r 4 voxels round
    the bore lands before the bore on the arrival, so the bore cuts metal; Cool at 4 s by the same capsule; a direct hit
    on an enemy in front of the terrain: 45 and Burning 3 s. 20 mana.
  - Firestorm Sweep: a Burst disc of 4 m whose leading half-rim rises 0.75 m into pitch, Heat under the sweep, 30 and
    Burning 2 s to enemies on the disc at T_z; a Line draws the curtain (one voxel thick, 0.75 m high) along the
    polyline, its scorch a capsule of 1.5 m from its first to its last point. 30 mana for a tap, 10 + 5 per metre drawn.
- Lifetimes: pitch 3.5 s (210 ticks), hot metal 4 s (240 ticks), both through the `KnitRegistry` (pitch in the conjure
  layer with Unknit, Heat in the turn layer with Cool). A pitch voxel a Freeze or a fill took is not its owner's any
  more, as for every conjuring.
- The hazard scan: every 6 ticks while slag, pitch or hot metal may lie, the strongest of pitch (15 HP/s) and a `Hot`
  id (10 HP/s) within two voxels of a capsule, credited to the owner of that voxel (Heat's owner for hot metal); then
  Burning (8 HP/s, `CastContext::burning`), at most 15 HP/s in all. Hit points per second are spread over the scans in
  integers. Burning ends at its tick or when the player is no longer a target (dead).
- A rune breaks when pitch lies on its floor (Heat breaks it already when its ice floor melts).
- Wire: no change. Maps 6-8 were reserved by [mag-2]; `kProtocolVersion` stays 15; `buildHash` changes through the
  material, turning and spell tables.
- Game: `--spell-demo pitchfall,wildfire,meltdown-beam,firestorm-sweep` in [mag-7]'s yard with their lines (`Pitch: N
  voxels burning, 0 left after expiry`, `Wildfire: N voxels burning up to X m`, `Meltdown: bore X m through knit and
  stone, H hot voxels`, `Heat: knit blastable, N hot voxels carved`, `Firestorm: curtain N voxels over X m`);
  `--siege-matrix` runs 16 rows. Thermal's projectiles take its orange; offline hand casting offers the four spells.

## Where it differs from the design

1. **No fire spread** (the owner's re-scope). Pitch burns only where it lands and goes with its owner's Unknit at 3.5 s;
   the Heat map has no Ignite rows, so timber never catches; no `TooMuchFire` cap. Spread S1 takes pitch over without
   rework: its four burn stages are the ids of one fire class (K 0, B 4) and the Pitch map lays the first; S1 adds the
   class to `SpreadTable` and replaces the registry's expiry of pitch fills with the burn steps.
2. **One pitch stage is used.** Without spread only `pitch_b1` is laid; b2-b4 exist for S1.
3. **Burning has no status bit.** It is server-only damage; the HUD's `BURNING` waits for a free owner-block bit.
4. **Meltdown Beam's pre-map is metal only.** The design reads "metal through Heat"; rimed stone keeps its rimed cost
   in the walk (the Shatter combo's double depth) and is thawed by the beam's own Heat before the carve, which still
   carves it (stone 48 <= 80).
5. **Wildfire's damage** is a scorch over the plume's box grown 0.5 m sideways and 2 m down, as Permafrost Climb's
   zones, not a test of the plume's cells.
6. **The quench** is the Rime map's rows (Shatter Spike's capsule, Permafrost Climb's box). Flash Freeze's Freeze
   covers open cells only, so it quenches pitch, not hot metal.
7. **Not built**: the ten-fires case (it needs spread). The design's combo test of heat on knit is
   `Magic.HeatMakesKnitBlastable`.

## Tests

`Materials.ThermalIdsAreAppendedAndPitchIsAFilm` (core), `TurningTable.NeverNameBedrockAndIdentityElsewhere` (the new
rows), `Magic.PitchBurnsThenExpires`, `Magic.HeatMakesKnitBlastable`, `Magic.FrostQuenchesHotMetal`,
`Magic.FrostFreezesPitchIntoIce`, `Magic.MeltdownBeamBoresAndBurns`, `Magic.FirestormSweepAlongALine`,
`Magic.WildfireClimbsAFaceAndMeltsIce`, `Magic.HotMetalBurnsOnContactUntilItCools`,
`Magic.ThermalSpellsGiveTheSameVoxelsTwice`, `Magic.HeatUnderAFloeBridgeReleasesWhatItCarried` (sim) and
`Magic.ThermalSpellsReplicateWithEqualHashes` (net: a duel, the caster attuned to Thermal; both clients hold the
server's voxels after the four spells and after their expiries).

## Measured

On the Windows PC (not the UHD 620), `--siege-matrix` at 0.125 m: Pitchfall 809 pitch voxels, work 6 673, 1.1 ms,
176 B; Wildfire 175 voxels up to 3.25 m, work 1 292, 0.1 ms, 62 B; Meltdown Beam 3.12 m through 0.5 m of knit and
stone, 160 hot voxels, work 4 427, 0.1 ms, 92 B; Firestorm Sweep a curtain of 258 voxels over 4.59 m, work 1 380,
0.1 ms, 200 B. At 0.1 / 0.15 m: pitch 849 / 582, the beam 2.60 / 2.70 m, the curtain 364 / 175.
