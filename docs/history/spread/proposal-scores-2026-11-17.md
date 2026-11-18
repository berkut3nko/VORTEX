# Spread history: scores of the two proposals (2026-11-17)

Moved verbatim from `docs/design/spread.md` lines 83-132 at engine `430fde0` on 2026-11-18 (WP3 document rewrite). The
text below is unchanged; corrections go only under "Errata" at the end.

---

## Scores of the two proposals

Two lockstep, randomness-free proposals were judged:
- **A, "state in voxels":** about 89 ids, a monotone liquid sheet with source ids, shell corrections as absolute ids.
- **B, "side layer":** 6 ids, plus per-chunk elapsed and hop counters and liquid levels in a `SpreadLayer` next to the
  chunk, carried in ChunkData and HashCheck, with a conserved pair-exchange liquid and coded overrides.

Scores are 1-5 (5 best).

| Criterion | A | B | Chosen | Reason |
|---|---|---|---|---|
| Exact agreement: windowed clients, Linux / Windows | 5 | 4 | 5 | Both close the step per chunk. B adds the invariant "a layer cell iff a live id" on every write path (editor, templates, saves, terrain mode), and a hole there desyncs. Ids cannot drift from themselves. |
| Late joiners and chunk sync | 5 | 3 | 5 | A and the chosen design need no new state. B needs a ChunkData layer section, a TerrainAck inline exception and a save format bump. (All three need the ring's kind-8 membership, below.) |
| HashCheck while cells change | 5 | 4 | 5 | `ChunkHash` covers ids, so it covers spread. B changes the hash's meaning and adds a `LayerRevision`. |
| Bandwidth | 4 | 4 | 4 | Both send border values only, est. 2-3 KB/s for one burning house with the compact encoding. |
| Server CPU, 4-core laptop | 4 | 4 | 4 | Both step only listed chunks with block skipping, on the job workers. B has an explicit active list; the chosen design scans for it, est. 10 µs per chunk. |
| Client cost, UHD 620 | 3 | 3 | 3 | A fire front changes ids every step in both designs: partial id uploads and shape-aware re-cooks are needed in both. |
| Bounded and readable in PvP | 4 | 4 | 5 | A's liquid is a 12.5 cm sheet that never pools, and its sources need ids. B lets grass burn, which is hop-limited but sweeps fields. The chosen design: fuel by material, A's ground-cover rule, B's conserved liquid that fills craters. |
| Fit with op stream and base rule | 5 | 5 | 5 | All are ops in the opSeq stream. |
| Material table, buildHash, load check | 3 | 4 | 4 | A uses 89 of the 235 free ids, which collides with W4's plans. B uses 6 but keeps one strength per burning id. The chosen design uses 43, with strength falling per stage. |
| Implementation risk and effort | 4 | 2 | 4 | B adds four contracts, each a bug class. |
| How early a playable slice exists | 4 | 3 | 4 | Fire alone, offline, needs no net change. |
| Simplicity of the operation (owner) | 3 | 3 | 4 | A has 5 terms per liquid cell, wet ids and sources. B has hop budgets, elapsed counters and override codes. |
| **Total (of 60)** | **49** | **43** | **52** | |

**The chosen design takes from A:**
- state in ids only;
- the 17-byte `IgniteSphere`;
- overrides as absolute ids;
- a separate integer `SpreadTable` with its own hash;
- chunks with an identity result left out of the list;
- membership-exact `OpTouchesChunk`;
- admission caps;
- ground cover that does not spread;
- planks burning to air and wood to charred wood;
- the build-ceiling argument;
- most of the tests.

**It takes from B:**
- the conserved pair-exchange liquid on a fixed 4-phase order, so a pour is a finite volume that pools, with no sources;
- one padded-box function for the isolated and the real pass;
- one bookkeeping path for per-voxel chunk writes;
- shape-aware re-cooks;
- server-only sleep, where scheduling cannot desync;
- casts passing through spell liquid;
- the configure-time no-randomness check;
- the whole-world reference stepper test;
- quick dousing saving the timber;
- burning ids kept on falling pieces.
