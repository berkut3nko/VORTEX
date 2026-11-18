# Collapse history: the header before the 2026-11-18 rewrite

Moved verbatim from `docs/design/collapse.md` lines 1-15 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

The body of `docs/design/collapse.md` (from "## Decision") is unchanged; only this header was replaced.

---

# Light structural collapse ("SHRED light")

Status: design decision, nothing built. All run-time costs for new code are estimates from
operation counts (about 5 ns per voxel flood-filled, 1-2 ns per mask operation); stage 2 contains
the benchmark that replaces them. Claims about today's code were checked against the sources.
**Partly superseded by [shred.md](shred.md)** (2026-11-12): terrain is the first host there, the
replication rules of networking.md apply, and its stages 0-1c are built (see "What is wrong today").
The measured face flood costs about 50 ns per voxel visit (optimised build), not 5.
**shred.md stage 4 built the graph on terrain** (2026-11-12, its "Stage 4 as built"): not with the 32-byte
`SupportCell` below but with a node list per chunk - every component of a cell is a node (no merge into node 7, so the
graph equals voxel connectivity, over-full cells included), each node with its voxel count, an integer mass and face
masks, each link with its bond (voxel faces shared) - in `vortex.voxel:structure` (`ChunkGraph`, `TerrainGraphCache`,
`SearchIslandsOnGraph`). Measured build: 25-33 us per Dense chunk at 0.125 m quiet (70-120 us in game, cold); Uniform chunks share one graph per id. The
"Data" section below is collapse.md's original design; stage 5b (objects) should reuse the terrain format.
