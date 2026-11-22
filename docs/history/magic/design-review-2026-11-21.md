# Review of the magic design (two critics, 2026-11-21)

Two critics reviewed [design/magic.md](../../design/magic.md) read-only, against CLAUDE.md, the designs it must fit
(spread, shred, networking, mechanisms, fog, fx-budget) and the engine's code at `58f2650`. Critic A sent 19 findings
(mostly determinism, wire and budgets), critic B 10 (mostly play: griefing, balance, stage order). Two topics were
found by both (Shockwave's dispel, Heat without a site check), so there are 29 findings on 27 issues. Each was checked
against the document and, where a claim depends on it, the code; all 27 issues were real, and every one was taken,
three only in part. Below: each finding in substance, what was checked, and what was done. The amended design is
[../../design/magic.md](../../design/magic.md).

## Critic A

1. **Flow's integer arc had zero gravity (high).** With `P` in q and `V` in q per tick, 9.81 m/s^2 is 0.35 q/tick^2
   at 0.125 m (128 q per metre / 3600) and 0.44 at 0.1 m: `G` rounds to 0, every lobbed spell would fly straight, and
   the reproducibility test would still pass. **Checked:** the arithmetic. **Done:** "Inputs every operator shares":
   the arc runs in 1/4096 voxel (q << 8) as int64, `G = round(9.81 / voxelSize * 4096 / 3600)` (89 / 112 / 74 at
   0.125 / 0.1 / 0.15 m), the first step takes half gravity so the sum matches `G t^2 / 2`, and the DDA walks `P >> 8`.
   New test `Flow.ArcMatchesTheParabola` (apex and range within 1 voxel at three voxel sizes).
2. **Shockwave's dispel could never cancel anything (medium; critic B: high).** Every zone starts at its own
   `T_run + lead` with one session lead, so a zone already pending at the Shockwave's `T_run` starts at or before the
   Shockwave's `T_z`; "cancel zones that start after `T_z`" matches none. Cancelling zones that start in
   `(T_run, T_z]` instead would give the cancel less than the lead. **Done:** `ZoneCancel{zoneId, endTick}` is a
   truncation (`endTick = T_run + lead`): the zone acts as before up to `T_z` and not after, so the cancel has the full
   lead by construction. Only lasting kinds are dispelled (Pin, Chill, Updraft's Lift; never Rising Spire's lift);
   instant ones (Impulse, the start of a Root) are not, and Siege Cannon's follow-through counter was removed. Pin's
   latch became a fixed tail (`strength2`) instead of "ticks left", so a truncation changes only ticks from its
   `endTick`. Wire: 7 B. Test renamed `Magic.ShockwaveTruncatesLiveEnemyZones`. With critic B's fix: an ally
   **Release** zone (kind 6) at `T_z` clears allies' root, pin and chill.
3. **Rising Spire's bands always hit the rider (medium).** The server steps players before `RunScheduled`; at
   `T_z + 7k` the rider's feet are about `k + 0.14` m while band k covers `[k, k + 1)`, so the capsule (grown by a
   voxel) skips the whole column in every band and the rider is left over a 6 m shaft. **Checked:** the tick order in
   the design's own server order. **Done:** a timing rule in the spell: band k goes out at the first tick the lift's
   nominal floor is at least band k's top plus 2 voxels (`T_z + 7(k + 1) + 2` for k = 0..4, `T_z + 46` for the top
   band, after the ease), the hover floor is 0.5 m over the top, and the lift ends at `lastBandTick + lead`
   (`T_z + 70`). `Magic.RisingSpireLiftsItsCaster` now asserts no air under the rider's column.
4. **Client zone retention for replays was unspecified (medium).** A correction replays from the snapshot's tick
   (`Client.cpp` `Reconcile`, `kHistoryTicks` 128), RTT + Target + 2 ticks behind the predicted tick, and the replayed
   `StepPlayer` needs the history tick's number. **Checked:** `Reconcile` replays `t + 1 .. nextTick - 1` from the
   history ring; `HistoryEntry` carries `tick`. **Done:** `ZoneSet`: the server prunes by its own tick; a client keeps
   a zone until `max(nextTick - kHistoryTicks, last compared snapshot tick)` passes its end, at most 64 records; every
   lookup passes the stepped tick (`HistoryEntry::tick` in a replay). New test `Movement.ZoneSurvivesAReplayAcrossItsEnd`.
5. **Heat removed support and anchors without a site check (medium; critic B: low).** Map 7 turns ice into air and
   floe into water; site checks were queued only for Unknit and Shatter. **Checked:** `Structure.cpp` makes an anchor
   of a non-ignored `Indestructible` voxel and ignores liquids, so floe -> water removes an anchor. **Done:** "Expiry
   and structure": a site check follows what changed, not the map's name. Any `MapShape` or `MapMask` that turns a
   graph-solid voxel into a non-collider or removes an anchor queues one over its changed chunks on the structure
   credit (spread amendment 2's rule for every op): Heat on ice and floe, the Douse rows of Rime and Freeze, Ablate.
   New test `Magic.HeatUnderAFloeBridgeReleasesWhatItCarried`.
6. **A Pierce bore's island box could leave the flood box and the server window (medium).** **Checked:**
   `Blast.cppm` pins `kMaxCastRadiusQ / 16 + kIslandSearchMargin (24) <= kServerWindowMarginChunks * 32`, and
   `voxel::kFloodHalfExtent` is 96 voxels at every voxel size; "at most 8 m" is 80 voxels at 0.1 m, so the box reached
   108 voxels. **Done:** "A bore's boxes": a capsule's island box is its AABB plus 24; the bore is bounded in voxels
   (`kMaxBoreVoxels` 64, radius at most 4) and never leaves the play bounds; its flood box is centred on the middle of
   the hit-to-end segment; a `static_assert` keeps 24 voxels of proof margin past the island box (92 <= 96). New test
   `Pierce.BoreStaysInsideItsFloodBox`; `--spell-demo siege-cannon` also runs at G6's 0.1 and 0.15 m.
7. **Mask byte estimates ignored the x-row RLE (medium).** **Checked:** `WriteMaskRuns` (`Wire.cpp`) writes
   alternating varint runs in `VoxelMask::Index` order, x innermost (`Volume.cppm`); a sheet facing x, a diagonal strip
   or a capsule costs about two varints per row. **Done:** (a) "The cheap expiry": an owner laid down by one
   `MapShape`, or whose box no other live owner of its layer shares (an `overlapped` bit), expires with one `MapShape`
   (17-29 B); (b) `MapMask` gains an `axis` byte, and the server sends the shortest of the three orders. Every byte
   figure in "The sixteen spells" and the budget table was recomputed (aligned / worst aim with an overlapping owner;
   "How the bytes are counted"), and the fight case is now stated against the 24 kB/s credit with the blasts' share:
   the form cooldowns cap spells at about 5.6 of the 10 casts/s, so events are about 12-17 kB/s (c), below the
   blast-only soak's 22.7-24.5 kB/s (m).
8. **The slick test's "voxel below the feet" flipped at a surface (medium).** A resting character sits at
   `k * voxelSize` plus or minus about 1e-5, so `floor(y / vs) - 1` picks either voxel, possibly differently on the
   two sides. **Done:** the ground voxel is the one holding `feetQ - 8` q (quantised feet, half a voxel down). New test
   `Player.SlickGroundReadsTheSameVoxelAtASurface`.
9. **Zone membership was not robust where the design said it was (low).** Only Impulse fell to 0 at its rim, and
   Impulse was singular at its centre (`d.xz * f / len`). **Done:** the claim was replaced by rules: Impulse's
   horizontal push is `d.xz * f / max(len, R / 8)` (continuous, 0 at the centre); Lift ramps over its outer 0.25 m;
   Pin, Chill and Root stay all-or-nothing, and a disagreement within the two sides' difference of the rim is counted
   as cause `zone edge`. New test `Player.ZoneEdgeAndCentreAreContinuous`.
10. **Crushing Weight's `extraLoad` check at `T_z` had no budget (low).** Its counters cannot be known at `T_run`, and
    follow-ups ran outside `RunCasts`' credit loop. **Done:** the check (like every site check) waits in the structure
    credit's queue from `T_z` and is charged by Part 4b's counters when it runs; the Pin and damage happen at `T_z`
    regardless. "Follow-ups" now says which work is a follow-up and which is not. The Shatter flood (finding B4) is
    charged at its cap at `T_run`. New test `Neck.ExtraLoadWaitsOnTheStructureCredit`.
11. **Chill's stretched cooldown disagreed with the server's late-cast path (low).** **Checked:** `Server::LateCast`
    sets `full = settings.player.castCooldownTicks` and rewrites kept states from it; `TakeCast` uses
    `tuning.castCooldownTicks`. **Done:** one function, `sim::CastCooldownFor(state, tuning)`, used by `TakeCast`,
    `LateCast` (state at the input's tick) and the rewrite; a contract row and
    `Movement.LateCastWhileChilledIsJudgedAsPredicted`.
12. **`shapeChanged` was keyed on the map, and the map-set rule was broken by Freeze (low).** Heat turns ice into air,
    the Douse rows turn burning foliage into air, and Freeze turns pitch (a film) into ice. **Done:** `shapeChanged` is
    decided per voxel (`collidable[old] != collidable[new]`, spread S1's bookkeeping) whatever the map; the map-set rule
    now lists every map that changes collider shape, and the same list decides site checks. New test
    `Magic.ShapeChangedFollowsTheVoxelsNotTheMap`.
13. **Wire sizes did not match the layouts (low).** **Done:** recomputed from the layouts: `MapShape` 17 / 29 / 27 /
    19 B, `MapMask` 21 B + runs (with the axis byte), `ZoneStart` 30 / 42 / 40 / 32 B, `ZoneCancel` 7 B, `CastFx` 32 B,
    the joiner baseline about 1.3 KB. The owner block now carries the four form cooldowns (u8 in 4-tick units) and the
    attunement byte for the HUD, plus `ccGuardLeft` (finding B3): 17 B instead of 11; "Input and HUD" says the HUD reads
    them there.
14. **The cast ray's `Film` skip would change the shared CPU/GPU trace (low).** **Checked:** `World::Raycast` calls
    `TraceWorld` from `trace.glsl`, the shared code under `Trace.SingleChunkMatchesReferenceDDA`. **Done:** the skip
    lives in `World::Raycast`'s spell variant on the CPU (re-trace past a film voxel, at most 8 restarts);
    `trace.glsl` is unchanged. Contract row and `Raycast.ASpellRayPassesPitchAndHitsTheStoneBehind`.
15. **Permafrost Climb on a floor hit left ice hanging in the air (low).** Plume's `Front` includes column tops 6 m
    up with nothing under them. **Done:** a fill takes a `Front` cell only if it is skin or joins support through the
    cells already filled (Plume, every filling element). Test `Propagate.PlumeLeavesNoFrontCellHanging`, and the case
    is in `Magic.SixteenSpellsGolden`.
16. **Pierce's visit cap counted an undefined quantity (low).** A box scan tests far more voxels than join the bore,
    so "3 000 voxels tested" would stop Siege Cannon after about 0.9 m. **Done:** visits are `sum |N_k|` (new member
    voxels); the slice scans are bounded by `maxSlices` and charged as `slices x kPierceSliceWork`; the golden depths
    include a 64-voxel bore.
17. **The GPU upload estimate was about 10x low (low).** **Done:** recomputed from the design's own counts: about
    9 chunk uploads per spell and 3-4 per blast, about 65 uploads a second, 2.4-3.7 MB/s (c), 40-62 KB a frame; still
    measured by G23 at a fixed camera.
18. **Fills on charging players' predicted paths are common, not rare (low). Taken in part.** **Done:** the
    correction table calls them expected for players charging through a Barricade line; new net test
    `Movement.BarricadeAcrossAWalkerCountsCorrectionsByCause` and corrections counted by cause in session-check.
    **Not done now:** announcing every band with a Block zone. It is one more zone kind for a correction cause not yet
    measured; zone kind 7 (Block) is reserved in [mag-2]'s bump and built only if [mag-2]'s counts ask for it.
19. **Amendment 4 (the registry rule, marked "now") was not applied (low).** **Checked:** notes/net.md section 2 still
    said "a new `WorldOpKind` takes 7, a new entity event 37", mechanisms.md pinned 37 / 38. **Done:** notes/net.md
    section 2 now states the registry rule, the numbers on today's ROADMAP order (magic 7-9 and 37-39, mechanisms 40
    and 41, `SpreadStep` 10) and the one exception (`PlayerButton_Spell = 32`); mechanisms.md, spread.md and shred.md
    Part 3 call their numbers provisional; `Mechanisms.WireSizes` pins whatever values land.

## Critic B

1. **Shockwave's dispel (high).** The same as A2, found independently, with the observation that the 3-byte layout
   had no tick. **Done:** as A2, plus the ally Release.
2. **Spawn points were unprotected (high).** Walls, pitch, pins and roots at a Seam (the setting's spawn points; M2a
   respawns at team spawns after 5 s) would lock respawning players; fills skip only capsules that exist at the
   emission tick. **Done:** the **spawn guard** (decision 15, readability rule 10, "Caps"): a spell whose op boxes,
   fill cells or zone shapes meet an enemy team's spawn area (6 m round each spawn point) is refused with
   `CastRejection::NearSpawn`; fill cells in any spawn area are dropped; a respawned player is `Fresh` for 2 s (no
   root, pin or knockback; cleared when they cast). Test `Magic.NoSpellReachesAnEnemySpawn`; open question 29.
3. **Roots and pins chained into a lock (medium).** `max()` refreshes and no immunity let two or three players keep a
   target rooted or pinned. **Done:** `ccGuardLeft` (u8, owner block): 1.5 s after the last root or pin ends, neither
   applies; chill and knockback still do; HUD status `FREE`. Test `Player.RootAndPinCannotChainPastTheGuard`; open
   question 28.
4. **Shatter Spike then Shockwave deleted a 2 m ball of any wall, iron included (medium).** A `MapShape` sphere
   reaches rime behind a plate, and every stone and iron id rimes. **Done:** Shatter is a bounded server flood
   ("The Shatter flood"): seeds are exposed shatterable voxels within 2 m, at most 4 voxels deep through face-connected
   shatterable voxels, at most 4 000 visits, sent as one `MapMask(Shatter)`. Plating over rime protects it; each
   Shockwave peels one layer. Shatter Spike's rime capsule went from r 10 to r 6 (0.75 m); Siege Cannon after a Spike
   now bores about 3.3 m instead of 2 (the combo test is `Combo.ShatterSpikeDeepensSiegeCannon`, at least 1.5x).
5. **A one-voxel knit film made a wall blast-proof and held 3x better than stone (medium). Taken in part.**
   **Checked:** `Material.cpp` gives stone 100 000 / 40 000 / 15 000; the design gave knit 300 000 / 120 000 / 60 000.
   **Done:** the absorb rule is ablative: an absorbed blast emits `MapShape(Ablate, r 0.25 m)` (map 15, knit and slag
   to air, reserved in [mag-2]), so a plate loses a patch per blast and the stone behind is spared; knit's bonds equal
   stone's. The map is named Ablate, not "Spall", because spall is the setting's word for the power mages carry out.
   **Changed from the proposal:** radius 0.25 m instead of 0.375 m, so one blast cannot hole a 3-voxel Barricade (two
   blasts per hole, one per spot on a 1-voxel plate). Test `Magic.AnAbsorbedBlastAblatesThePlate`.
6. **Siege Cannon took Thermal's damage role (medium). Taken in part.** **Checked:** the owner's concept gives
   Thermal "high direct damage on contact". **Done:** Siege Cannon's direct hit 50 -> 30, Meltdown Beam 35 -> 45
   (+24 burn = 69, under 100); Rebar Lance 40 -> 30 so Thermal's lead is clear; readability rule 6, the TTK table and
   the knobs updated; open question 27 for the owner's taste. **Not done:** Siege Cannon costing 2 charges from [mag-2].
   Per-spell costs were rejected for readability, and the damage cut removes its dominance as a damage spell; 2
   charges stays the first knob (question 11).
7. **Rising Spire's 12-tick hover was shorter than the lead the design itself requires (medium).** **Done:** the lift
   ends at `lastBandTick + lead`, for any session lead; with A3's timing, `T_z + 70` by default. New test
   `Movement.SpireRiderNoCorrection` on G16's link.
8. **The stage order was not the smallest fun slice (medium).** [mag-1] built every zone kind, the slick rule and the
   full Plume operator for four spells that use none of them, on the claim that later stages then need no revision
   bump, which saves nothing because each stage changes `buildHash` through its tables. Shatter, ranked fun #1, waited
   for [si-2]. ROADMAP "Later" listed the magic rows after a bullet that says "after W1". **Done:** re-cut:
   [mag-1] only what Siege Cannon, Barricade Wave, Shockwave and Updraft use (6-9 d instead of 8-11 d); [mag-2] as
   before; [mag-3] Frost and the Shatter combo (Shatter Spike, Flash Freeze, Permafrost Climb); [mag-4] Flow, climbing
   and the rest of Metal (Molten Pour, Rising Spire, Rebar Lance, Glacier Slide, slick); [mag-5] Thermal; [mag-6]
   presentation and tuning; [mag-7] Crushing Weight after [si-2]. `kPlayerRuleRevision` goes 2 -> 5 over [mag-1],
   [mag-3], [mag-4] and [mag-7]; every wire value is still reserved in [mag-2]. ROADMAP "Later" now says the magic
   rows follow [mag-2] and [si-2], not W1, and lists them first; M2b and M5 point to the new IDs.
9. **Heat melted ice and floe without a site check (low).** The same as A5. **Done:** as A5.
10. **One player could not make a Rebar ladder tall enough for a castle wall (low).** Three live pegs reached about
    4.4 m. **Done:** one lance sets two pegs, at the hit and 1 m above it (the upper only if the voxel there is solid
    and within the power), and pegs last 20 s: three lances are six pegs, a ledge of about 6.4 m within 10 s. The
    `--spell-demo` Rebar line records the height; test `Magic.RebarLanceSetsTwoPegs`.

## Not taken, with the reason

- A18's Block zone now: reserved, built only if [mag-2]'s measured terrain corrections ask for it.
- B5's 0.375 m ablation radius: 0.25 m keeps a Barricade standing through one blast.
- B6's 2-charge Siege Cannon from [mag-2]: per-spell costs stay rejected; it remains the first knob.

## Also changed while applying the findings

- Header (status, decided items 1-16, open questions 1-29, history row), decisions 6-8, 10 and 14-16, the concept
  table's deviations, readability rules 6, 9 and 10, the combos table and fun ranking, the HUD statuses and refusals,
  the replication map, the cast pipeline, the matrix, the contract rows, the rejected alternatives (five rows), the
  risks (a byte-figures risk) and open questions 7, 11, 13, 14 and 24.
- Index and roadmap: engine docs/README.md (the design's row, the stage-ID row `[mag-1]`..`[mag-7]`, the history list
  and count), ROADMAP.md ("Next" item 8, "Later", M2b, M5).
