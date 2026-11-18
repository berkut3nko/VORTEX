# Networking history: the M1 questions as they were put to the owner

Moved verbatim from `docs/design/networking.md` lines 3181-3195 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

## Questions as they were put to the owner

1. **Is a 1 km zone part of the M1 acceptance?** (a) M1 is accepted on a 250 m arena and stage 9 moves behind M2;
   (b) stage 9 is required. Recommendation: (a); the protocol is the same, only server memory and loading change.
2. **Will strangers connect before M7?** (a) Trusted testers only: ENet and a plain token; (b) a public test:
   encrypted transport with connect tokens (est. +8-12 days, replaces the transport of stage 5). Recommendation: (a).
3. **Is a Windows machine with Clang available now?** (a) Yes: the golden test runs there in stage 0; (b) no: terrain
   agreement stays unproven until M6, with server terrain mode as the safety net. Recommendation: (a), Core tests only.
4. **Will servers always be x86-64 with AVX2?** If ARM or older hosts are wanted, fixed-point noise is needed and
   belongs in stage 0, before terrain content exists. Recommendation: x86-64 with AVX2 only.
5. **Is a hole one round trip after the click acceptable in the first playable?** (a) Yes, predicted carving is
   judged in M2 playtests; (b) no: predicted carving with rollback joins M1 (est. +6-10 days). Recommendation: (a).
6. **Is it acceptable that lumps under about 0.8 m (512 voxels) are not shared?** Each client sees its own rubble; it
   blocks nobody and is not cover. (a) Yes, the threshold is tuned in stage 6; (b) no: a lower threshold costs server
   bodies and snapshot bytes (64 awake pieces is the budget). Recommendation: (a).
