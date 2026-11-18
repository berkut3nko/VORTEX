# View-distance history: stage 2 as built (the voxel preview) and its measurements

Moved verbatim from `docs/design/view-distance.md` lines 384-486 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

## Stage 2 as built

The voxel preview. At the default 0.1 m it changes nothing: defaults, goldens, `Terrain.GoldenChunkHashes` and the
standard headless runs are as before (the furniture probe is built only with `--probe`; every playground box is
non-empty at scale 1, so `S()` leaves it as it was; the default runs log no `Probe:` line).

- **`--voxel-size <m>`** (game, 0.1-0.15 m, **offline only**; anything else is refused with a warning and 0.1 m is
  used) sets `EngineConfig::world.voxelSize`. The range is what was run: above 0.2 m the standard run's beam is
  1 voxel thick (`S(3)` = 1) and its cut leaves two 4-voxel particle crumbs, no `1 new object(s)`. In a session
  the voxel size is the server's (`networking.md`, "Region start-up"): the integer operations mean nothing between
  two sizes and `buildHash` does not cover it. The engine needed no change: `sim::Simulation` already copies the
  world's size into the terrain generator and the scene, physics cooks colliders and the renderer traces in the
  world's voxels, and the full-height range is in metres (55 / 44 / 37 rows). The only hard-coded size was the
  game's (`main.cpp`): the playground, the third-person figure, the crate and the beam are authored in voxels of
  0.1 m and are now scaled by `0.1 / voxelSize` (exactly 1 at 0.1 m); the editor's glTF import defaults to
  `1 / voxelSize` voxels per metre. Left in voxels for stage 3: the debris thresholds (16 / 48 / 6), the tree-trunk
  rule (every trunk is 1 voxel above 0.1 m, visible in the peak screenshots); fx particle sizes are already relative
  to the voxel.
- **Furniture probe** (`BuildProbe`, `--probe`; `--camera probe`, `--camera probe-far` and `--probe-blast` imply
  it), world voxels on a paved pad 9 m east of the plaza centre (loaded blocking around its middle before it is
  edited; the site is chosen level only within 7 m of the plaza centre, so on a slope the pad cuts into the hill or
  stands on its 3 m foundation): a brick wall 2.5 m high with a door (frame 12.5 cm, clear opening 87.5 x 200 cm,
  the leaf open), two bookshelves 100 x 37.5 cm with 12.5 cm boards at a pitch of 37.5 cm (2 m, books on three
  shelves) and of 25 cm (1.875 m), a table 150 x 87.5 cm, 75 cm high, and a chair beside it (seat 50 x 50 cm at
  50 cm, back 87.5 cm, legs 12.5 cm square). Authored in metres on the 1/8 m grid, voxelised by voxel centres with
  **both ends included**: a box [lo, hi] gets the voxels whose centres it holds, so on an exact tie both boxes that
  meet there get the voxel and touching boxes stay touching. (The first version used half-open boxes plus a
  middle-voxel fallback; at 0.15 m the top board of the 25 cm shelf, ending on a tie at 12.5 voxels, got the row
  above the end of its sides and floated before any blast. That was the voxeliser, not the voxel size.) The cost: a
  box whose upper end is on a tie is one voxel longer; at 0.1 m ties are the x.25 / x.75 m boundaries, so the chair
  stands where none of its edges is one. The `Probe:` log line counts fallbacks (0 at all three sizes) and runs the
  blast's island search over the pad: **0 floating islands** at 0.1, 0.125 and 0.15 m.
- `--camera probe`: 5.75 m in front of the wall, 1.6 m up, relative to the probe's floor corner (a voxel corner: in
  metres it moves by up to a voxel between sizes, the furniture with it). `--camera probe-far`: 20 m from the middle
  of the furniture at eye height above the ground there, the first spot on an arc in front of the probe from which
  the middle is in plain sight (logged; here the straight-ahead spot, 21 m away on a hillside 7 m above the pad).
- `--probe-blast` blasts the chair's legs at frame 30 (radius 0.32 m, 8 cm above the floor, power 2: wood breaks,
  the stone floor does not) and logs the islands, the crumbs and each new piece (voxels, layer, mass, position,
  velocity, awake / frozen) at +0, 1, 5, 15, 30, 60 and 119 frames.
- **Generation chunks/s**: `WorldStats::generatedChunks / generatedDense / generateMs` (worker-side, saved chunks not
  counted, tested in `World.LoadsWindowAndClassifiesChunks` / `PersistsEditsToDisk`); `Engine::Run` logs the wall
  rate from just before `OnInit` until the window is filled, and the generator's own rate per worker-second.

### Measured (UHD 620 laptop, 7 workers, optimised `dev` build, 1280x720 output, 768x432 internal)

Every number and screenshot below except the blast row comes from runs of 1200 headless frames without validation,
after `World: window filled`; GPU times are medians of the last 120 frames, two runs each (a range where they
differ). Near: `--camera probe`. Far: `--camera=-1,61,65,-0.241,-0.55`, 4 m above the highest point within 70 m of the
spawn (the `--climb` peak, 56.8 m), looking towards the valley, without the probe; the pose is in metres, so it is the
same view at every size and only the view radius changes.

| | 0.1 m | 0.125 m | 0.15 m |
|---|---|---|---|
| Window (rows), width | 48 x 55 x 48 (-15..40), 153.6 m | 48 x 44 x 48 (-12..32), 192 m | 48 x 37 x 48 (-10..27), 230.4 m |
| View radius | 73.6 m | 92.0 m (1.25x) | 110.4 m (1.5x) |
| Store a/b: near / far (filled) | 5836 / 6324 of 12 288 | 5311 / 5629 | 5117 / 5260 |
| Packed: near / far | 17 293 in 25.0 MiB / 17 201 in 25.1 MiB | 14 797 in 23.3 / 14 860 in 23.5 | 12 553 in 21.8 / 12 603 in 21.9 |
| Near: gbuffer / lighting / composite (ms) | 6.54-6.55 / 9.33-9.54 / 2.00-2.24 | 6.23 / 8.88-9.33 / 1.83-2.15 | 5.59 / 8.62 / 2.12-2.42 |
| Far: gbuffer / lighting / composite (ms) | 10.58 / 6.61-6.67 / 1.00-1.09 | 12.20-12.21 / 7.28-7.38 / 1.24-1.40 | 11.54 / 7.69-7.70 / 1.36-1.37 |
| Probe-far (21 m): gbuffer / lighting / composite (ms), one run | 7.53 / 10.01 / 1.81 | 7.45 / 9.55 / 1.85 | 7.02 / 8.49 / 1.83 |
| Fill (near): frames, s | 338-339, 6.8-6.9 s | 268-271, 5.3 s | 215-219, 4.1-4.2 s |
| Generated until filled (near): chunks (dense) | 32 949 (24 254) | 27 026 (21 152) | 22 806 (18 448) |
| Generation, wall: chunks/s (dense/s) | 3831-4055 (2820-2985) | 3963-3997 (3102-3128) | 4075-4114 (3296-3328) |
| Generation per worker-second (near; far) | 1029-1097; 1095-1132 | 1023; 979-985 | 1043-1047; 963-1019 |
| Probe chair (voxels): seat, seat height | 5 x 5, 4 (0.40 m) | 4 x 4, 3 (0.375 m) | 4 x 4, 2 (0.30 m) |
| Chair legs blasted (150-frame run, validation) | 8 carved; 1 piece of 53 voxels (Dynamic layer, >= 48: no `Debris` lifetime); 0 crumbs. Push (-2.4, 1.2, -2.4) m/s, gone at +1 frame: dropped in place | 8 carved; 1 piece of 32 voxels (`Debris` layer); 0 crumbs. Push (-2.3, 1.6, -2.3) m/s, gone at +1 frame: dropped in place | 8 carved; 1 piece of 28 voxels (`Debris` layer); 0 crumbs. Frozen in its first step, kept its push (-2.3, 0.5, -2.3) m/s: slid 0.85 m (-x, -z) |
| Standard game run (validation, 200 frames) | exit 0, `Beam cut: 37 voxels removed, 1 new object(s)`, on the ground | exit 0, beam 13 removed, 1 new object, on the ground | exit 0, beam 16 removed, 1 new object, on the ground |

- The wall rate is not the generator's limit: the fill is capped by the integration budget, 64 Dense results per
  frame at about 50 headless frames/s (3200 dense/s); the dense rates above sit at that cap. The generator itself
  does about 1000-1100 chunks per worker-second at every size (a chunk is 32^3 voxels whatever its size), about
  7000-8000 chunks/s on 7 workers. Bigger voxels fill sooner because the same metres need fewer chunks.
- Bigger voxels need fewer slots (fewer rows hold the relief) and fewer Packed bytes, although the window covers
  1.56x (0.125) and 2.25x (0.15) the area.
- Far view: gbuffer rises with the radius (+1.6 ms at 0.125, +1.0 ms at 0.15) because more pixels hit terrain
  instead of sky, and lighting with them (+0.7 / +1.1 ms); near view: everything is cheaper at bigger voxels (fewer
  steps per ray), gbuffer -0.3 / -1.0 ms.
- **The blast compares physics timing, not voxel sizes.** At every size what stands above the cut is one piece
  (never particles), and every piece gets the same push. Whether it keeps the push depends on its first physics
  step: at 0.1 and 0.125 m the push was gone one frame later (the Dynamic-layer and the Debris-layer piece alike, so
  the 48-voxel layer threshold is not the cause); at 0.15 m the guard froze the piece for its first step and it kept
  it. Most likely cause (from the code, not isolated): the piece is made over the terrain collider that still holds
  its own former voxels (the old collider stays until the rebuilt one is installed, and a built, stale collider
  counts as ready), and the overlap eats the push. An earlier run at 0.125 m threw the piece about 2 m. Open, an
  engine issue for every terrain Detach (`notes/scene-engine.md`), not part of this stage. (Later, shred.md stage
  1d: such a piece is held frozen, its push kept, until the stale collider is re-cooked; the loss itself did not
  reproduce in isolation by then.)
- **Shape at 0.15 m** (no floating parts any more): the 25 cm-pitch shelf's boards come out uneven (pitch 1.67
  voxels: some gaps are 1 voxel, some boards merge into 2-voxel slabs), every 12.5 cm book is 1 voxel wide, the seat
  sits at 0.30 m with 2-voxel legs. At 0.1 m the 25 cm shelf is uneven too (boards 1-2 voxels thick, gaps
  1-2); at 0.125 m every 1/8 m feature is exact. At 21 m (`probefar_*`) all three read as two shelves, a table and a
  chair (the 6 m wall is about 170 output pixels wide, a shelf about 30); a 12.5 cm board is about 3 output pixels (2
  internal), so the difference between the sizes shows in the close-up, hardly at 21 m (judged from the screenshots).

Screenshots (in the session scratchpad, `preview/`): `probe_<size>.png` (near, filled), `probefar_<size>.png` (21 m,
filled), `peak_<size>.png` (far, filled), `probeblast_<size>.png` (120 frames after the chair blast; the window is
still streaming there, 150 frames with validation), `game_<size>.png` (standard run). The far view shows the radius
growing: the plaza and a lake come out of the fog at 0.125 m, the lowland beyond them at 0.15 m. Not checked:
windowed play at 0.125 / 0.15 m (the owner), the `--climb` / `--fly` gates at other sizes (only 0.1 m was run:
0 unloads, window y fixed, 0 store-full).

Open question 1 was answered by the owner: **0.125 m** (stage 3).
