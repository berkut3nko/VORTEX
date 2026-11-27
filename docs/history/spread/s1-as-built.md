# [spread-S1] as built (2026-11-27)

What [spread-S1] built, and where it differs from [design/spread.md](../../design/spread.md) and the task pack
[llm/spread-s1.md](../../llm/spread-s1.md) as they stood before the stage. The pack was written at engine `ea41341`;
[mag-1]..[mag-8] landed before this stage, [si-2] and [fog-1] did not (the owner moved fire ahead of them).

## What it builds

- **Core: the fire ids.** 35 ids appended after `pitch_b4` (58): `planks_k1..k6` 59-64, `planks_b1..b6` 65-70,
  `wood_k1..k8` 71-78, `wood_b1..b8` 79-86, `foliage_k1..k3` 87-89, `foliage_b1..b2` 90-91, `charred_wood` 92,
  `scorched` 93, with the values of spread.md "Material table values" (a kindle stage keeps its base's hardness and
  bonds and glows from 1 to 4; burn stage j of B has 0.6 of the base's hardness and (B + 1 - j) / B of its bonds and
  glows from 5 down to 1.5). `MaterialFlag_Burning` (bit 8) marks every burning id, pitch's four stages included: the
  GPU material table gives them `kGpuMaterialBurning` (2), and the composite pass flickers their emissive per voxel
  eight times a second. `voxel::kNonColliderFlags` (`Liquid | Foliage | Film`) is the terrain colliders' filter.
- **Core: the maps.** Ignite (1): planks, wood and both leaves into their first kindle stage, grass and dry grass into
  `scorched`. Douse (2): a kindle stage into its base, a burn stage into `charred_wood`, burning leaves and pitch into
  air. Rime (9) and Freeze (11) carry Douse's timber and leaf rows (Frost puts fire out; Freeze still turns pitch into
  ice).
- **Core: `voxel::SpreadTable`** (`vortex.voxel:spread`): kind, class, stage and next per id, five classes (planks,
  wood, foliage as spread.md gives them; pitch K 0, B 4, pace 10, no delays, residue air), the burn phase pattern
  `kPhase` (16 distinct values, mirror-symmetric), `SpreadTableHash` (logged in the `Spread:` line, not in `buildHash`).
- **Core: `voxel::StepBox`**, the fire rule of spread.md "Fire (up and sideways, never down)" on a padded 40^3 box, with
  `LiveBlocks` and `DilateBlocks`. A configure step refuses `Hash3`, `rand(`, `random` and `mt19937` in
  `src/voxel/internal/Spread.cpp` and `src/sim/internal/Spread.cpp`.
- **Core: the world.** `World::EditVoxelsInChunk` writes one chunk through `EditRegion`. `ChunkChange::shapeChanged`
  is true where a voxel changed its collider class (air, collider, solid but no collider); `TerrainColliders` skips the
  other changes and counts them (`recooksSkipped`). An edit that keeps every voxel's occupancy marks only its 64 B id
  blocks (`ChunkStore::MarkDirtyBlocks`, `TakeDirtyBlocks`); the renderer copies runs of them into the GPU copy
  (`RendererStats::blockBytesUploaded`). `WorldConfig::reportEdits` keeps the list of edited chunks
  (`World::TakeEditedChunks`) for the fire driver.
- **Sim: `sim::SpreadDriver`** (`vortex.sim:spread`). An index of the chunks that hold burning ids (block masks), a
  logical step every 6 ticks under a work credit of 2 000 000 a step (clamped to -8 steps; over twice the credit the
  neighbours wait), the awake chunks and their neighbours stepped on the job system from ids read once, written in
  (y, z, x) order. Up to 8 site checks a step (`Blast(ChunkSite, {.noCarve})`: the island search and the load check),
  a FIFO of 256. `Kindle` (`MapShape(Map_Ignite)`, 0.5 m) refused at 16 burning chunks within 32 m or 128 in all.
  `FormatSpreadLine` gives the `Spread:` line.
- **Sim: pitch.** With `SimulationConfig::spread` the pitch fills of Pitchfall, Wildfire and Firestorm Sweep get a
  registry owner with no expiry op (`KnitOwnerInfo::expiryMap` 0, lifetime `kPitchBurnoutTicks` = 246 ticks, the
  slowest burnout) and the hazard scan's window follows it. `ServerConfig` clears `spread`: a session keeps Unknit at
  3.5 s. The bore cost table has the fire ids.
- **Engine:** `EngineConfig::spread`, `Engine::Spread()` (the driver), the fixed step runs it between
  `OnFixedUpdate` and the physics, `Engine::PresentSpread` (at most 32 flame bursts and 32 ember bursts a call, and
  `PresentBlast` for each site check).
- **Game:** the offline game sets `spread`; G kindles what the crosshair aims at (`Kindle not run: too much fire`
  for 2 s on a refusal; `Kindle [G]` in the tool line); `--fire-demo` (src/FireDemo.cpp) and `--camera fire-demo`; the
  `Spread:` line at exit of any offline run whose fire stepped. The siege matrix's pitch check waits for the burnout.

## Where it differs from the pack and the design

1. **Ids 59-93, not 22-56.** The magic stages took 21-58 first. `MaterialFlag_Burning` is bit 8: bits 5-7 are Slick,
   Hot and Film.
2. **Pitch is a fifth fire class**, as magic.md "New world-palette ids" asks. Its burnout is no site (a film holds
   nothing). In a session it keeps the registry's expiry; offline the fire rule replaces it.
3. **Heat has no Ignite rows yet.** magic.md gives the Heat map Ignite's rows; in a session (no fire) they would leave
   timber kindling for good. Offline, pitch lights timber through the fire rule instead. The rows come with S2.
4. **`shapeChanged` is the collider class, not the occupancy.** FreezeWater (water into floe) and Freeze over pitch
   change a collider without changing the occupancy; an occupancy test would have skipped their re-cook. The upload
   still uses the occupancy (the GPU pyramid).
5. **The driver lives in the engine, not in `sim::Simulation`.** `Simulation` would have to import the partition that
   uses it; the engine owns the driver (`Engine::Spread()`) and tests own their own. `Simulation` keeps only the flag.
6. **New fire is found through the world's list of edited chunks** (`WorldConfig::reportEdits`), so a Kindle, a
   spell's pitch, a landing and a FillBox in a test are indexed the same way at the next slot; the step's own writes
   are counted at once and taken off the list.
7. **The landing ignition, without [si-2].** As spread.md "Burning pieces" decides it: at release, from the column
   under the piece's lowest burning voxel (y, then z, then x), at most 512 voxels down to the first voxel that is no
   film or liquid, a Kindle of 0.375 m on its top `ISqrt64(drop x 93 942 / 1024) + 1` ticks later at 0.125 m (the
   column walked again then), under the Kindle caps. It reads no body: offline the piece is presentation rubble, which
   caps and lifetimes may remove. The request named a rest or contact hook; there is none that sim reads, and the design
   decides at release on purpose (offline and session alike). [si-2] may replace it.
8. **`--fire-demo`'s scene.** The stone room's slab covers the whole 5 x 5 m house (its walls stand 0.5 m in from the
   edges, a 4 x 4 m room 1.75 m high), resting on wood beams round the wall tops; the posts are the walls' wood corners.
   A planks base 6 voxels deep lies under house A (not under the kindled corner, which the kindle's sphere reaches), so
   a falling burning piece has a floor to light (house A's own floor burns out first). Its checks: the ignition layer is
   the lowest row the kindle or a landing lit; the trunk under the crown keeps its wood; a landing lit the base's top
   row and nothing of the base under the lowest row a landing lit burnt; the fire goes out within 3 000 script frames (the pack's 1 200 frames are 200 logical
   steps; the whole demo burns for about 360-430).
9. **No gate.** The pack adds none. `--fire-demo` is documented in docs/verify.md; G23's `Pitch:` line keeps its
   wording and judge, the check now waits for the burnout.

## Tests

Core: `Materials.FireIdsAreAppendedInOrder`, `Materials.BurnStagesWeaken`, `TurningTable.IgniteMap` (and
`TurningTable.NeverNameBedrockAndIdentityElsewhere` with the new rows), `SpreadTable.BedrockIsInert`,
`SpreadTable.PhaseIsSixteenDistinctAndMirrored`, `SpreadTable.HashCoversEveryField`, `SpreadRule.FireNeverIgnitesBelow`,
`SpreadRule.FrontSpeedsMatchTheTable`, `SpreadRule.JumpsOneGapNotTwo`, `SpreadRule.FoliageOnlyClimbs`,
`SpreadRule.SymmetricSetupStaysSymmetric`, `SpreadRule.EveryFireBurnsOutWithinBound`,
`SpreadRule.QuenchEarlySavesQuenchLateChars`, `SpreadRule.WetBlocksIgnition`, `SpreadRule.BurnoutsSpreadEvenly`,
`SpreadRule.PadIsInert`, `ChunkStore.BlockDirtyIsSupersededByWhole`, `World.EditVoxelsInChunkEqualsSetVoxel`,
`World.ShapeChangedOnlyWhenCollidabilityChanges` (it also checks the block-only upload), `World.ReportsEditedChunksWhenAsked`.
Sim: `Spread.ShapeUnchangedSkipsRecook`, `Spread.StepClockCountsLogicalSteps`,
`Spread.PhaseGatedContentAdvancesWithNothingElseAwake`, `Spread.ResultIndependentOfChunkOrderAndThreads`,
`Spread.FireCrossesChunkFaces`, `Spread.KindleCapsRefuse`, `Spread.BurnoutReleasesAStructure`,
`Spread.FallingBurningPieceLightsFloor`, `Spread.PitchClimbsAndLightsTimber`, `Spread.SessionsKeepPitchExpiryAndNeverStep`.
Graphics: `LayoutContract` checks `VX_MATERIAL_BURNING`.

## Measured

On the Windows PC (not the UHD 620), `Shredborne --headless --fire-demo --frames 3200 --camera fire-demo --after-fill`,
0.125 m, two runs (m): a logical step (gather, compute on the workers, write; the site checks not counted) took
1.22 ms on average and 1.95-2.00 ms at most, with at most 9-10 chunks awake and stepped; 366 steps with fire, the
fire out after 378 logical steps (about 38 s); 23 394 ignitions, 18 758 burnouts, 1 201 written chunks re-cooked and
1 023 not (stage changes only), 1 386 site checks, 3 pieces released, 1 landing ignition (2 dropped: they fell on
stone). Peak burning fraction of house A 41-42 %. CPU frame p50 3.02 ms, p99 4.91 ms for the whole run. Not measured:
the UHD 620, GPU pass times with and without fire at the fixed camera, the upload bytes per step frame.
