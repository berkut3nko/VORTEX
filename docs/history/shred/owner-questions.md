# Shred history: Part 4b's open questions as put to the owner

Moved verbatim from `docs/design/shred.md` lines 1225-1257 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

Their state now: `docs/design/shred.md` header ("Decided", "Open") and "Open questions for the owner", item 11.

---

### Open questions for the owner (each with a recommendation)

1. **Where does the tower fall?** With stone C = 100 000 kg/m^2 the watch tower (35 t above its neck) falls on any
   stone neck under 24 faces (0.375 m^2, a sixth of its base) and stands on 24 or more; natural rock keeps a 3x
   margin or more. **Recommendation: keep it and tune by playing** (one integer per material; at 150 000 the tower
   holds from 16 faces, at 70 000 only from 34).
2. **Whole castles may collapse - when?** In 4b anything over 60 000 voxels is an anchor, so a castle never falls by
   this check (a corner tower, about 45 000 voxels, could once the walls beside it are cut away).
   **Recommendation: a stage 4c right after 4b is measured**: raise the exterior load limit to 4 x 60 000, split any
   release over 60 000 voxels (and oversize islands) into chunk-aligned 32^3 bricks, each its own `Detach`, at most
   240 000 released voxels per cast (the rest a later cast), wire bursts measured; stage 5 then spreads big collapses
   over ticks.
3. **A shot at the roof does not re-check a neck far below** (failure mode 4). **Recommendation: accept** until stage
   5's dirty boxes; players cut necks with shots near them.
4. **Foliage weightless?** **Recommendation: yes** - generated trees have 1-voxel trunks under canopies of up to
   26 800 leaves and would otherwise all fail.
5. **Soft ground slumps**: sand overhangs over 1.9 m and dirt over 5.3 m fall when a cast lands near them.
   **Recommendation: yes**, it reads as "shredding" soft ground; rock is unaffected.
6. **Weak player-built structures** stand until a shot lands near them, then fall. **Recommendation: yes, plus an
   editor "neck report"** later (run the check on a box and list overloaded parts with their ratio; collapse.md's
   spawn guard as a report, never a release). The review measured the case: the watch tower FillBoxed onto a game
   slope **without its foundation** rests on about 25 faces of turf (10 000 000 MU against 22 932 000) and falls on the
   first dud near it; the game's playground, built on its foundation, releases nothing under the six `--neck-blast`
   duds.
7. **(Review) The flow budget allows a cast far over the cast phase.** The preflow takes 5-6 ms at about 207 000 of
   its 262 144 arc scans; every typical cast needs 2 600-13 000 (the tower's release 7 000-10 700). **Recommendation:**
   lower `kNeckFlowWorkBudget` to 65 536 (about 1.5 ms, 5x the largest measured need; spent means "released next
   cast", R7) when stage 3's cast budget is built, and charge `neckFlowWork` there; not changed now because it changes
   which casts release (only in networks far larger than any measured).
   *Done with networking stage 3 (2026-11-13): `kNeckFlowWorkBudget` = 65 536, and the cast work budget charges
   `neckFlowWork` (2 x `neckFlowWork` / 8 units of about 0.1 us, networking.md "Stage 3 as built"). Every test and the
   `--neck-blast` / `--tower-blast` runs are unchanged (the game's neck release needs 7 055).*
