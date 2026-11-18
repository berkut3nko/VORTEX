# LOD history: text replaced or changed by the WP3 rewrite

Moved verbatim from `docs/design/lod.md` lines 1-9 at engine `430fde0` on 2026-11-18 (WP3 document rewrite). The text
below is unchanged; corrections go only under "Errata" at the end.

The original text of every section of the current document that the rewrite replaced or changed, in source order. The
current document holds the rewritten version.

---

# Level of detail for distant terrain and objects

Rebased to 0.125 m base voxels (levels 0.125 * 2^L) and a full-height window by [view-distance.md](view-distance.md);
mechanisms below unchanged. The numbers below were rewritten for 0.125 m when the engine switched to it (view-distance
stage 3, 2026-11-12); figures marked *(0.1 m)* are kept from the original 0.1 m text where nothing was re-measured.

Status: design decision, nothing built or measured for it. Millisecond figures not quoted from
`ROADMAP.md` are estimates; every stage names the measurement that confirms or kills them.
Claims about today's code were checked against the sources (uncommitted physics work included).
