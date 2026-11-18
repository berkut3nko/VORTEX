# Spread history: the two critics' review of the design (2026-11-17)

Moved verbatim from `docs/design/spread.md` lines 932-960 at engine `430fde0` on 2026-11-18 (WP3 document rewrite).
The text below is unchanged; corrections go only under "Errata" at the end.

---

## Review of the design (two critics, 2026-11-17)

Each finding was checked against the code (engine working tree with stage 8 in progress) and the design. All were real
in substance; where a fix was changed or partly rejected, the reason is given.

| # | Finding | Verdict | What changed |
|---|---|---|---|
| 1 | The step clock ran on every slot while chunks stepped round-robin: phase lock (fires that never end, one-axis levelling, sleep at a non-fixed point) | Real | `s` counts emitted logical steps; a step is all-or-nothing; over-size steps split into parts with one `s`; the "recompute with chunks moved out" path is gone; debt-model credits; `Spread.StepClockCountsLogicalSteps`. |
| 2 | The work credit charged only taken chunks; the 26 receivers (live ones included) were stepped free, three times | Real | S = awake + neighbours with a non-empty real mask; the whole step is charged per pass from the live-block masks (R5-clean); `Spread.PlannerChargesEverySteppedChunk`. |
| 3 | The real pass read chunks outside S as bedrock: a phantom floor for liquid | Real | Chunks inside the window but outside S keep their true ids for reads and are frozen for mass; only outside the window is wall; the reference test freezes alike; `Spread.FrozenNeighboursKeepTheirFloor`. |
| 4 | A 3-voxel pad is too small: the liquid's downward reach is 4 | Real (derivation checked: down 4, up 3, sides 2) | 40^3 box; the per-face override band stated and asserted. |
| 5 | `OpRing` and `ChunkSync` use the op's box: map-wide boxes cause stage 8's whole-chunk re-requests and terrain-mode installs; ring time overstated | Real (`Sync.cpp` Push / Walk / TouchesNear / Apply checked) | Kind 8 by membership everywhere; `Sync.SpreadStepMembershipInRing`; ring about 14 s (c from stage 8's 17 s). |
| 6 | `Emit` charges every WorldOp to `eventCredit`, so fire would delay casts | Real (`Server.cpp` Emit checked) | `Emit` / `EmitOp` take an explicit credit; `Spread.FireDoesNotDelayCasts`. |
| 7 | `shapeChanged` on `EditReport` never reaches the colliders | Real (`Terrain.cpp` `ConsumeChanges` checked) | The flag is on `voxel::ChunkChange`, with the skip in `ConsumeChanges`. |
| 8 | Spread chunks crowd cast chunks out of HashCheck's recent picks | Real (`kRecentChanged` 64, 4 recent picks newest first) | Own `recentSpread` list, at most 1 of the 4 recent picks. |
| 9 | i16 chunk coordinates on the wire | Real | i32 (zigzag varint) box origin, as other chunk coordinates. |
| 10 | Work credit does not bound the work; adjacency decides which fire stalls | Real; its proposed S (live neighbours not taken read as wall) **rejected**: that is finding 3's phantom floor, and with all-or-nothing no live chunk is left out | Merged with 2. |
| 11 | 1 KiB/step and 8 B chunk headers slow every fire 3-5x in a big fight | Real; the "drop the farthest fires" part **rejected**: dropped fires miss logical steps (finding 1's phase lock) and players would see fires freeze by their distance to others | Compact self-contained varint encoding (about 190 B for one house, not the critic's 120); credit 2 KiB/step, burst 32 KiB; global admission at 75 % of either credit; `Spread.TenFiresTwoPoursWithinCredit`. |
| 12 | CPU estimates contradict the per-chunk numbers; single-threaded spikes; uploads 4.1 MB | Real | Parallel evaluation on the `JobSystem` on server and client; no third server pass (the round trip is checked in debug, `--check-spread` and a test); partial id uploads moved into S1; costs re-estimated in evaluated voxels; the ten-fires acceptance row in S2. |
| 13 | Global `s % pace` and `s % 10` synchronise all burnouts and evaporation | Real; accepted for burns (`kPhase`, mirror-symmetric); for evaporation a per-voxel phase is **rejected** (it keeps every liquid chunk awake) in favour of a per-chunk phase | `kPhase`, `kChunkPhase`; `Spread.BurnoutsSpreadEvenly`; open question 16 asks the owner to confirm the pattern is not "randomness". |
| 14 | Fire never crosses a 1-voxel gap, and worldgen's C7 gap is exactly 1 | Real (worldgen.md C7 checked) | Jump cells (±2, 0, 0) / (0, 0, ±2) with air between, planks 10, wood 12, foliage never; the old "keep 3 voxels" text corrected; `Spread.JumpsOneGapNotTwo`. |
| 15 | Touching crowns make forest fires that hold the global Kindle cap and stall at chunk faces | Real; replacing the entry cap by the farthest-fire drop **rejected** (as in 11) | Foliage only climbs; local Kindle / Pour caps (16 chunks within 32 m) plus a global safety; the entry cap is now a last-resort hard cap at twice the work credit. |
| 16 | The landing ignition depended on hidden piece-slot caps | Real | Decided at release by an integer column ray, scheduled by fall time, offline the same; `Spread.FallingBurningPieceLightsFloorOverSlotCaps`. |
| 17 | Pour does little to a burning wall; the "fireproof your gate" tactic fails | Real | `DouseSphere` (kind 9) at the hit point; the gate sentence reworded; `--douse-demo` acceptance. |
| 18 | Liquid settles as a stepped mound, drawn as a ziggurat | Real (mound computed: R ≈ 19 cells, about 18 m²) | Volume text corrected; `Spread.PourOnFlatStoneTerminalProfile`; the L/8 surface moved into S4; 16 levels left to S4's screenshot. |
| 19 | House burn-time claims contradict the speed table; a thin band | Real (24 s to the far corner, a 1.25 m band) | Planks pace 10, dSide 3, dDiag 4 (nearly round fronts): far corner about 16 s, band about 2.6 m (c); peak burning fraction measured by `--fire-demo` against 35 %. |
| 20 | Per-stage uniform emissive draws crisp bands | Real | Presentation flicker in S1 (shading pass, not `trace.glsl`). |
| 21 | S1 is large, touches the wire, and the payoff waits for S3 | Real | Re-cut: S1 is offline fire with the offline collapse; S2 the wire and session; S3 only the server-side site credit. S1 is still the largest stage. |
| 22 | "Welling" collides with the setting's spall wells and pooled spall | Real (setting.md checked) | Renamed to knit water (a Knitting), colour kept distinct from pooled spall. |
