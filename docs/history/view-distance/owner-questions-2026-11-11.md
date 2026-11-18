# View-distance history: the open questions as they were put to the owner (2026-11-11)

Moved verbatim from `docs/design/view-distance.md` lines 263-277 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

## Open questions for the owner

1. **Voxel size** after the stage-2 screenshots: 0.1 m, **0.125 m (recommended)**, or 0.15 m.
2. **Furniture finer than a voxel?** Recommended: accept blocky furniture now, keep it in world voxels so it collapses
   with buildings, revisit finer-scale props after M1. *Answered (stage 3): blocky world voxels.*
3. **Other players beyond 96 m**: dynamic objects are drawn inside level 0 only. Recommended: draw replicated players
   at any distance (a few instances, est. small cost).
4. **UHD 620 default preset**: Low (192 m) if stage 6 meets its gate, otherwise Minimal with Low opt-in.
5. **Step height**: keep 0.35 m (25 cm risers) or raise to 0.4 m. Recommended: keep. *Answered (stage 3): kept.*
6. **Stage 1 cave mouths**: beyond 16 m a cave mouth looks plugged one chunk in until stage 5. Acceptable for the
   stages in between? Recommended: yes; stage 5 follows the distance budgets.
7. **Build ceiling**: terrain bounds + 32 m (about 43 m above the highest tree-topped peak) as the hard limit for
   building and thrown debris, refused above it. Enough, or does the arena need a higher ceiling (for example 64 m;
   costs only sky cells)?
