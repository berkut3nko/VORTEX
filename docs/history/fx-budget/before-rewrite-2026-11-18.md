# FX-budget history: text replaced or changed by the WP3 rewrite

Moved verbatim from `docs/design/fx-budget.md` lines 1-6 and 8-31 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

The original text of every section of the current document that the rewrite replaced or changed, in source order. The
current document holds the rewritten version.

---

# Effects that stay fast in fights

Status: design decision, nothing built or measured for it yet. "Measured" = `docs/ROADMAP.md`
(UHD 620, 768x432 internal, 21 ms GPU per frame). "Est." = from reading the code; stage 1
replaces every estimate with a number. Related: `collapse.md` (object damage, debris lifetimes),
`lod.md` (distance budgets).

<!-- source lines 8-31 -->

## Decision

A cast is split into two halves with opposite overload rules:

- **Simulation** (carve, object damage, island search, colliders, bodies): queued, coalesced,
  bounded per frame, **never dropped**.
- **Presentation** (particles, light): same frame as the cast, **scaled or dropped, never delayed**.

One engine-side class, `FxDirector`, owns the queues and fixed per-frame caps. Effect lights
leave the ray-traced lighting pass and are evaluated unshadowed in `composite.frag` (at most 8
after merging). Particles stay on the existing CPU pool, shrunk to 8192, with effect LOD, a load
factor and a pixel overdraw budget. Effects become data (`EffectDesc`) with caps the director
enforces.

Rejected, and why:

| Rejected | Reason |
|---|---|
| Stateless GPU particles (closed-form emitters, proposal B) | Removes CPU cost that is est. 1 ms at 8192 particles; the limit on this GPU is fill, which it does not touch. New binding, shared evaluator, per-emitter sort. Revisit only if stage 1 shows particle CPU > 1.5 ms after stage 2. |
| Tiled effect-light grid (B) | With 8 merged lights a plain loop is est. < 0.5 ms; a tile buffer adds two bindings for no gain at that count. |
| Learned-cost budget + GPU governor (B) | Static caps plus the pool load factor bound the frame; a feedback loop is hard to test headless. Caps are `EngineConfig` fields, so a governor can drive them later. |
| `EffectEvent` network record, server-side library (B) | No multiplayer on the roadmap. The simulation/presentation split keeps the door open. |
| Half-resolution particle target, async island search | Kept as conditional stage 6: only if stage 1 numbers demand them. |
| Deferring nothing but shrinking counts (A, stage 1 only) | Does not bound the worst frame: ten casts still run ten island searches and N whole-volume splits in one frame. |
