# View-distance history: stage 3 as built (the switch to 0.125 m) and its measurements

Moved verbatim from `docs/design/view-distance.md` lines 487-592 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

## Stage 3 as built

The engine runs at 0.125 m. Owner questions 2 and 5 were taken with their recommended answers: furniture stays
blocky world voxels; step height stays 0.35 m (it climbs 25 cm risers, 2 voxels, not 3).

- **Defaults.** `voxel::kDefaultVoxelSize = 0.125f` (`Chunk.cppm`) is the default of `WorldConfig::voxelSize`,
  `TerrainSettings::voxelSize` and `scene::SceneConfig::voxelSize`; the editor's glTF `voxelsPerUnit` is
  `1 / kDefaultVoxelSize` (8) and so is `voxel::MeshImportSettings::voxelsPerUnit` (was 10). The game sets no size
  any more (`main.cpp`); `--voxel-size` (0.1-0.15 m) stays an offline override and its warning names the size used.
- **Debris thresholds in metres** (`Scene.cppm`, `Simulation.cppm`): `kMinPieceVolume` 0.016 m^3,
  `kDebrisBodyVolume` 0.048 m^3, `kThinBodyLength` 0.6 m and `sim::kServerReplicatedPieceVolume` 0.512 m^3 (what
  16 / 48 / 6 / 512 voxels of 0.1 m were), turned into voxels by `scene::VoxelsForVolume` / `VoxelsForLength`
  (nearest, at least 1): **8 / 25 / 5 / 262** at 0.125 m, as computed in the design. The three scene thresholds
  default to `scene::kFromMetres` ("derive"); `scene::ResolveThresholds` turns them into voxels of the config's own
  `voxelSize` when a `Scene` is built, and `sim::Simulation` resolves them with the world's size (so its `Config()`
  reports counts); a threshold given in voxels is kept. `sim::ServerConfig` converts the replicated volume at the
  configuration's world size. Every `SimulationConfig` therefore gets the metre sizes at any voxel size (16 / 48 / 6
  / 512 in a 0.1 m world, 5 / 14 / 4 at 0.15 m); the game sets nothing by hand. The blast's crumb rule is
  `scene::MinPieceVoxels(world.VoxelSize())`: computed from the session's voxel size, which server and clients
  share bit for bit (`scene::kMinPieceVoxels` is its value at the default size, 8). Tests:
  `SceneDebris.ThresholdsKeepTheirMetreSize` (arithmetic), `SceneDebris.DefaultThresholdsAtTheEngineVoxelSize`
  (behaviour of a default `SceneConfig{}`: 7 voxels crumble, 8 are an entity, 24 are Debris rubble, 25 an ordinary
  body), `Simulation.DebrisThresholdsKeepTheirMetreSizeInTheWorld` (0.1 / 0.125 / 0.15 m worlds, offline and
  server), `Blast.IslandOfMinPieceVoxelsIsAPieceAndOfOneLessACrumb` (16 / 8 / 5 at 0.1 / 0.125 / 0.15 m). Not
  rescaled, because they are cost limits in voxels rather than sizes: `kIslandSearchMargin` (24 voxels: 3 m now),
  `kMaxIslandVoxels` (60 000), the 400-burst particle cap; fx particle sizes already scale with the voxel (1.25x
  the 0.1 m reference).
- **Content keeps its metre size.** The playground, the figure, the crate and the beam were already scaled from
  0.1 m voxels in stage 2 (`S()`, 0.8 now: the beam is 19 x 2 x 2 voxels, 2.375 x 0.25 x 0.25 m); the editor smoke
  test's glTF import uses 6 voxels of 0.1 m per unit (4.8).
- **Tree trunks in metres** (`Generator.cpp`): width 0.24 m x (0.8..1.4) = 0.19-0.34 m, drawn as the nearest odd
  number of voxels (at least 1): at 0.125 m 40 % of trunks are 1 voxel (0.125 m) and 60 % 3 voxels (0.375 m) (c);
  at 0.1 m almost all are 3 voxels. The old rule (`lround(0.12 m x (0.8..1.4) / vs) - 1`) gave 1 voxel to every
  trunk above 0.1 m and to 75 % of them at 0.1 m. Visible in the screenshots: thick and thin trunks again. Test:
  `Terrain.TreeTrunksKeepTheirMetreWidth` (trunk widths measured in generated chunks at 0.1 and 0.125 m: square,
  odd, the nearest odd count to a width in 0.19-0.34 m, both 1- and 3-voxel trunks at 0.125 m); the golden probe #32 of seed
  0x9e3779b9 asserts it still holds wood.
- **Region files record the voxel size** (`Region.cpp`): header `"VXR2"`, the size's float bits, the count. `"VXR1"`
  files (no size) count as 0.1 m (`RegionStore::kLegacyVoxelSize`: the only size the engine wrote before). A file
  of another size is refused whole: nothing is loaded from it (the generated terrain shows), `Flush` never
  overwrites it (edits made in that region stay in memory for the session), and an error is logged. A file this
  build cannot read (another magic such as a future "VXR3", a header cut short, an impossible count) is refused the
  same way instead of being replaced; only an empty file is replaced. `Flush` warns with the number of refused
  regions that hold edits; `WorldSaveResult::unwrittenRegions` carries it to the editor's "Save world" status, and
  `WorldStats::refusedRegions` / `unwrittenRegions` are on the exit `World:` log line. Tests:
  `World.SavesOfAnotherVoxelSizeAreRefusedAndKept` (save at 0.5 m, open at 0.25 m: edit absent, own edit not
  flushed, file bytes unchanged, back at 0.5 m the edit is there), `Region.OldFilesWithoutAVoxelSizeCountAsTenthOfAMetre`
  `Region.UnreadableFilesAreRefusedAndNeverOverwritten` (unknown magic, cut header, 2-byte file: refused, bytes
  unchanged; an empty file is replaced).
- **Goldens.** `Terrain.GoldenNoiseBits` and `Terrain.GoldenHeightBits` did **not** change. `GoldenChunkHashes`
  was regenerated: 180 of 192 hashes changed (the other 12 are air or uniform chunks). Its probes were picked on
  3.2 m chunks; each is now the 4 m chunk holding the middle of the listed one (`ProbeChunk`,
  `floor((4c + 2) / 5)`, integer arithmetic; the test asserts the 0.125 m default), and every coverage check still
  passes (grass, dirt, stone, dark stone, sand, snow, water, wood, leaves and a cave per seed; at least 75 % dense,
  at most 3/8 air). The cave check's 1.5 m is now `lround(1.5 / vs)` voxels (was 15). The design asked for the
  golden change in its own commit: nothing is committed yet, so whoever commits should split it out.
- **lod.md** rebased: levels `0.125 * 2^L`, full height at every level, the 1 px / view / fog table, cone and mip
  distances (94 / 96 m), level 1 at 0.25 m, the debris-at-range numbers in metres.

**Tests judged** (the ones whose expected values are in voxels): `SceneTests.cpp` and `ServerSceneTests.cpp` are
written in voxels of 0.1 m (metre positions and radii placed on that grid, a 512-voxel crate): they now set a 0.1 m
voxel size explicitly (`TestConfig`, `OfflineScene`, the server config of a 0.1 m world) and leave the thresholds at
`kFromMetres`, which resolve to 16 / 48 / 6 and 512 there, so each keeps exactly its old meaning.
`DefaultsKeepTinyFragmentsOutOfTheScene` became `TinyFragmentsStayOutOfTheScene` (its 12-voxel beam end against the
0.1 m threshold, 0.012 < 0.016 m^3); the true defaults have their own behavioural test (above). One offset that
hard-coded 3.2 m is now `32 * kVoxel`. The crumb tests in `BlastTests.cpp` (arena at 0.1 m) use the arena's own rule
(`scene::MinPieceVoxels(0.1)` = 16): the largest crumb is 15 voxels (a bar along z) and the 2 x 2 x 2 cube is a
1 x 2 x 2 square with the same mask box; `IslandOfSixteenVoxelsIsAPieceAndOfFifteenACrumb` became
`IslandOfMinPieceVoxelsIsAPieceAndOfOneLessACrumb` and runs at 0.1, 0.125 and 0.15 m. The server threshold tests
check 0.512 m^3 (512 at 0.1 m, 262 at 0.125 m). No test was loosened.

### Measured (UHD 620 laptop, 7 workers, optimised `dev` build, 1280x720 output, 768x432 internal)

Same commands as stage 2: 1200 headless frames without validation, medians of the last 120 frames after `World:
window filled`; "stage 2" is the stage-2 binary with `--voxel-size 0.125` run again today just before the switch
(and the stage-2 table's range).

| | Stage 2 at 0.125 m | Stage 3 (default) | |
|---|---|---|---|
| Near (`--camera probe`): gbuffer / lighting / composite (ms) | 6.23 / 8.90 / 2.36 (table: 6.23 / 8.88-9.33 / 1.83-2.15) | 6.22 / 8.88-9.23 / 1.97-2.33 (3 runs) | within 5 %: lighting +3.7 % at most against today's run, inside the table's range |
| Far (`--camera=-1,61,65,-0.241,-0.55`): gbuffer / lighting / composite (ms) | 12.20 / 7.57 / 1.32 (table: 12.20-12.21 / 7.28-7.38 / 1.24-1.40) | 12.20-12.21 / 7.29-7.62 / 1.28-1.32 (2 runs) | within 5 % |
| Store a/b near / far (filled) | 5311 / 5629 | 5314 / 5632 | dense / uniform / empty / Packed counts identical (5305 / 14 170 / 67 104 / 14 797 near) |
| Fill (near) | 268-271 frames, 5.3 s | 265-268 frames, 5.3 s | |
| Generation per worker-second (near) | 1023-1132 | 998-1078 | |

After the review fixes (thresholds resolved by the scene, crumb rule from the session's size, region refusals;
no shader or `vortex.graphics` change) the runs were repeated in a later, warmer session (an owner's windowed game
had just used the GPU): near gbuffer / lighting / composite 6.22 / 9.38-9.75 / 2.73-2.81 ms (3 runs), far 12.19-12.21
/ 7.42-8.20 / 1.82-2.17 ms (3 runs), store a/b 5314 / 5632 and the chunk counts unchanged, fill 265-266 frames.
gbuffer is the same to 0.02 ms; lighting and composite are up to 0.5-0.9 ms above the first stage-3 runs. Taken as
machine state (thermal, shared power budget), not proven: no stage-2 binary was rerun in that session.

| Check | Result |
|---|---|
| Unit tests | 419 (one disabled measurement), all pass; build warning-free. |
| Standard game run (validation, 200 frames) | exit 0, `Beam cut: 13 voxels removed, 1 new object(s)`, `HUD: 149 quads`, `on the ground`, store 5278 / 12 288. |
| Editor run (validation, 120 frames) | exit 0 (the player's last frame "in the air", the known flip noted in stage 1). At 200 frames (exit 0) the smoke test's glTF import runs: `frank.glb` at 4.8 voxels per unit is 10 x 10 x 11 voxels (1.25-1.4 m, as at 0.1 m). |
| `--climb` (validation) | exit 0, 329 m in 1977 frames, `0 unloads inside view radius, window y fixed` (rows -12..-12), max 0 unfilled columns, recentre max 0.57 ms, 0 store-full, store 4370; blast 30 m ahead, 6 m deep: 24 decodes in 0.56 ms, 22.4 ms for the whole blast. |
| `--fly --frames 1200` (validation) | exit 0, max 0 unfilled columns, 0 unloads, 0 store-full. (`--fly` alone never ends: give it `--frames`.) |
| `--camera probe` (validation, 200 frames) | exit 0; `Probe:` chair 4 x 4 voxels, seat 3 up, 0 fallbacks, **0 floating islands**. |
| `--probe-blast` (validation, 150 frames) | exit 0; 8 voxels carved, 1 piece of 32 voxels, now **Dynamic layer** (25-voxel threshold; 0.0625 m^3, like the 53-voxel piece at 0.1 m), 0 crumbs; its push is gone at +1 frame (the stale-collider issue of stage 2, still open); after the review fixes one run had the piece frozen for its first step, then moving at the push's speed from +5 frames (physics timing, as at 0.15 m in stage 2). |
| `--voxel-size 0.1` / `0.15` (validation, 200 frames) | exit 0, `Beam cut: 37` / `16 voxels removed, 1 new object(s)`, `on the ground`. |
| Screenshots | Near probe: furniture, wall and door as in stage 2, trees with 1- and 3-voxel trunks; peak view over the valley to the fog edge, no holes; standard run: plaza, figure, HUD. |

Not checked: windowed play at 0.125 m (the owner); the GTX 1650. Not done: the separate golden commit (nothing is
committed).

---

## Errata

- "Not done: the separate golden commit (nothing is committed)": the golden regeneration was committed
  separately, as engine `20b5dfc` ("Regenerate GoldenChunkHashes for 0.125 m voxels, test the trunk width"),
  right after `5ffa7e6` (the switch). The game commit is `e2aae4f`.
