# World-generation history: how the design was chosen (2026-11-17)

Moved verbatim from `docs/design/worldgen.md` lines 89-130 at engine `430fde0` on 2026-11-18 (WP3 document rewrite).
The text below is unchanged; corrections go only under "Errata" at the end.

---

### How it was chosen

Three proposals were judged: **H**, hierarchical (top-down planner run on both sides, primitives plus a small kit);
**K**, kit + WFC (server planner, shipped plan, kit-cell modules, assemblers by need); **S**, history (faction
simulation drives placement, shipped plan, parametric buildings rasterised on both sides). Scores run 1-5; higher is
better.

| Criterion | H | K | S | Notes |
|---|---|---|---|---|
| Fit with the engine and its contracts | 4 | 5 | 4 | K keeps the client side to records and a stamper. S's client-local lights bypass today's entity light path. |
| Determinism across server and clients | 3 | 5 | 4 | H runs A\*, plot growth and the grammar on Windows clients, plus a hash-and-fallback path. S rasterises the grammar on clients. K copies module bytes. |
| Cost on the target hardware | 3 | 4 | 4 | H plans on every client (0.1-0.3 s per arena, est.). K's plan-time load check would cost the server 0.2-0.6 s of preload; this design moves it to CI. |
| Fun and readable fights and destruction | 5 | 4 | 4 | H: spine routes, worn-plan fairness, neck table. K: paired chokepoints. S: missions from history. |
| Look at 0.125 m | 3 | 4 | 4 | H's primitives read plain at 5 m. K's modules carry near detail. S's age palettes and ruins add variety. |
| Authoring effort | 4 | 3 | 4 | K needs 25-45 modules per kit (code first, art later). H and S lean on parametric code. |
| Risk | 3 | 4 | 3 | H: client planner drift. S: history simulation and statistical-only load confidence. |
| How early a playable slice exists | 4 | 4 | 4 | All about 2 weeks. This design splits offline (W1) from session (W2). |
| **Total** | **29** | **33** | **31** | |

**K wins.** Grafted onto it:
- from **H**:
  - the spine route and objective rules;
  - worn-plan fairness;
  - the construction rules C1-C8;
  - the role-slot palette in `.vox` modules;
  - style files as climate-driven cross-kit mixes;
  - the neck table;
  - salted RNG streams;
  - the `--plan-png` debug map.
- from **S**:
  - columnar-decay ruins;
  - reverse-causality history and missions from the "now" state;
  - the empty-plan bit-identity test;
  - the `ISurfaceModifier` hook into the terrain generator;
  - the generated-name blocklist test;
  - the test-only load-margin hook.

K's plan-time load check on the server is dropped: the preload budget has no room for it. CI sweeps and a tool replace
it.

Rejected alternatives are listed at the end, with reasons.
