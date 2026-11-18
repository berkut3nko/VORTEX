# Fog history: text replaced or changed by the WP3 rewrite

Moved verbatim from `docs/design/fog.md` lines 1-15 at engine `430fde0` on 2026-11-18 (WP3 document rewrite). The text
below is unchanged; corrections go only under "Errata" at the end.

The original text of every section of the current document that the rewrite replaced or changed, in source order. The
current document holds the rewritten version.

---

# Fog: sunlit haze, fog that lies in valleys and craters, blast dust that drifts and settles

Status: **design, nothing built or measured for it; reviewed once (see "Review of the design", end of file).**
Owner's request (2026-11-17): "add some easy volumetric simulation of fog (maybe dependent on dense maps)". Written
read-only against engine `71dc23e` / game `ad0fe69` (networking stages 0-7b committed, stage 8 being built). Numbers
are **(m)** measured (quoted from perf.md / view-distance.md / lod.md / notes), **(c)** computed from sizes, **(e)**
estimate; every (e) names the measurement that replaces it. "Dense maps" in the request is read as **density maps**:
2D maps of fog density built from the terrain.

Relation to other designs: [perf.md](perf.md) (frame budget and the measuring method used here),
[fx-budget.md](fx-budget.md) (blast presentation, dust particles, the 4 ms effects cap, its planned effect lights in
composite), [view-distance.md](view-distance.md) (window, edge fog, `--climb` / `--fly` gates), [lod.md](lod.md)
(fog density and the level-0 edge), [networking.md](networking.md) "Stage 6 as built" onwards (what a client already
shares with the server), [worldgen.md](worldgen.md) (domination, sea level) and the game's `docs/world/setting.md`
(the Brodvel Fens: "Fog limits sight").
