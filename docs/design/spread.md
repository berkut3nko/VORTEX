# Spread: fire that climbs and liquid that runs down, in lockstep, with no randomness

Status: **design, reviewed by two critics (2026-11-17) and amended; nothing built or measured for it.** Owner's request
(2026-11-17), for the magic system (ROADMAP M5, setting.md's "Kindling"): a deterministic spreading system. Fire
spreads up and sideways, never down; a liquid spell spreads the other way, down and sideways, never up. Written
read-only against engine `e21a84d` / game `ad0fe69` (networking stages 0-7b committed, stage 8 being built in the same
tree, so `Server.cpp` / `Sync.cpp` line numbers may move). Numbers are marked **(m)** measured (quoted), **(c)**
computed from sizes, or **(e)** estimate. Every (e) names the measurement that replaces it. The critics' findings and
what was done with each are in "Review of the design" at the end.

Relation to other designs:
- [shred.md](shred.md) Part 2 (R1-R7; this design adds one exception to R1 and amends R2's ChunkClip wording, see
  "Contract changes"), Part 4b (the load check), the SI plan (site checks and the structure credit).
- [networking.md](networking.md) stages 3-8 (op stream, cast credit, chunk sync, HashCheck, terrain mode, stage 8's
  `OpRing::CoversChunk`).
- [fx-budget.md](fx-budget.md) (FxDirector, the 4 ms effects cap).
- [fog.md](fog.md) (the dust grid that smoke feeds).
- [worldgen.md](worldgen.md) (W4's appended ids; C7's 1-voxel party gaps, which fire now jumps).
- The game's `docs/world/setting.md`: conjured matter does not last (Knitting); burned timber quarters are the
  setting's ruins.

## Decision in brief

**The owner decided three things; they are not reopened here:**
- **Lockstep.** Every peer (server, offline game, every client) runs the same spread operation, driven by replicated
  events: ignite, pour (with its douse) and the spread step.
- **A simple spread operation.** One small integer rule for fire and one for liquid.
- **No randomness.** No RNG, no `Hash3`, no hash-based chances anywhere in the rules. Variety comes from materials,
  geometry and presentation only.

What follows settles the rest:

1. **All spread state lives in the voxel ids. There is no side table.** Burn stages and liquid levels are world-palette
   ids: 43 new ids (21-63), appended. ChunkData, `ChunkHash` / HashCheck, WorldHash, packing, the server's modified
   map, joiners and the client's ring therefore carry spread state with **no new state contract**. The set of active
   voxels is derived from the ids whenever it is needed.
2. **The spread advances in numbered logical steps.** `SpreadStep` is a new `WorldOpKind` 8. A step slot comes every
   6 server ticks (10 Hz). When something is awake and the spread credits allow, the server emits one **logical
   step**: one or more `SpreadStep` ops (parts) with the same step index `s`, emitted back to back in one tick, whose
   sorted chunk lists are disjoint. `s` counts **emitted logical steps only**, and a logical step is all-or-nothing:
   every awake chunk steps in it. So every chunk sees `s, s+1, s+2, ...` in order, and the rules' phases and paces
   always rotate. Clients apply each part at its opSeq like any op, so the base rule (`seq > base`), ring replay,
   HashCheck at an exact opSeq and the deferred-check drop all hold.
3. **Every chunk steps in isolation, plus the server's border overrides.** A peer computes a listed chunk from **its
   own old ids only**. Everything outside the chunk reads as an inert wall. The op also carries **overrides**: the
   absolute new id of every voxel where the real result (with the true neighbours) differs from the isolated one.
   Those are the voxels where fire or liquid crosses a chunk face, in a band 4 voxels deep at the bottom face, 3 at the
   top and 2 at the sides. The server computes overrides with **the same function**, so they are the rule's own output,
   not a second authority. It stays lockstep: est. 95-99 % of changed voxels are computed by every peer from events,
   and the data that travels are border values a peer holding part of the world cannot compute.
   - Why: a step that reads other chunks breaks ring replay into one chunk, ChunkData installed ahead of or behind the
     stream, and chunks at the client window edge. All three are checked against the code in "Why per-chunk
     closure".
4. **The rules are synchronous, local and integer.** Every new id is computed from old ids, the step index and the
   voxel's world coordinates, so chunk order, voxel order and thread count cannot change a result.
   - **Fire:** an unburnt flammable voxel ignites when a burning voxel **below or beside** it (or one voxel of air
     away, sideways) has burnt for at least that material's delay for that direction. Burning voxels advance through
     fixed stages and end as air (planks, leaves) or charred wood (wood). Burn stages advance on a pace whose phase is
     a fixed 4-voxel ordered pattern of world coordinates (not a chance: every voxel burns for a fixed time), so a
     map's burnouts do not all land on one step.
   - **Liquid:** each liquid voxel holds an integer level 1-8. Each step the liquid falls twice (down only), then
     levels out once between fixed pairs of side neighbours. The pair axis and offset cycle with `s % 4`. Mass is
     conserved, so a pour fills a crater from the bottom up and settles as a low mound on flat ground. Nothing ever
     moves up.
5. **Two spells, two new sphere ops.**
   - **Kindle** (fire): `IgniteSphere`, new `WorldOpKind` 7, CarveSphere's 17-byte layout.
   - **Pour** (liquid, a Knitting): `DouseSphere` (new `WorldOpKind` 9, the same layout) at the hit point, which
     quenches what burns where you aim, then the existing `SwapSphere` (kind 3) from air (0) to a full liquid voxel in
     front of the surface. It is a finite volume; there are no source cells.
6. **Bounded by construction.** Only planks, wood and leaves spread fire. Ground cover (grass) and every mineral never
   do. Every burning voxel ends within 88 logical steps (9 s at full rate). A pour's volume is finite, and liquid
   evaporates and soaks away. The server refuses a Kindle or a Pour while too much burns or flows near the cast point,
   or while the spread budgets run near full (admission caps). Over the work or byte budget the whole step waits; it is
   never wrong (R7).
7. **Collapse stays the server's** (R1). A step that burns a support away or weakens it to its last stage marks a
   **site**. The server's load check runs there and emits `Detach` / `ClearMask` through the stream. Clients never
   decide a collapse. Burning ids carry lower `strength`, so a burning tower falls when its beams give out. A released
   piece that is still burning lights the floor under it, decided when it is released (not by its body).
8. **Scheduling is server-only and cannot desync.** Which chunks are listed, sleep, budgets, skipped slots and how a
   step is split into parts are all server decisions carried in the ops. A mistake there can only stall spread for
   everyone. It can never make two peers disagree, because a peer's result depends only on its chunk and the op.

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

## Why per-chunk closure (checked against the code)

Today no op reads another chunk, and stages 7a and 7b rest on that:
1. **Ring replay into one chunk.** `ChunkSync::CatchUp` (`net/internal/Sync.cpp`) replays ops in (base, applied]
   **clipped to that chunk only**. A step that read its neighbours at that opSeq cannot be replayed, because their
   past states are gone.
2. **ChunkData ahead of the stream.** `Install` installs at once with base = `asOfOpSeq`, often above `applied`. A
   step reading such a neighbour reads it at the wrong time.
3. **Window-edge chunks** (arena250: a 48-chunk window with slack). Their outside neighbours are not held. Under
   continuous fire they would be requested on every step. A joiner or repair whose bulk always arrives behind the
   stream would loop.

**The fix: a chunk's new ids are a function of its own old ids, its coordinates and the op's bytes.**
- The whole classic ChunkClip property survives: applying every chunk of an op with its own clipped call equals the
  whole call.
- Therefore the fast and clipped apply paths, ring replay, data ahead or behind, chunks outside the window, repairs
  and terrain mode all work, and HashCheck compares exactly. What `ChunkSync` and `OpRing` still need is to treat a
  kind-8 op by its chunk **list**, not its box (see "ChunkSync on clients").
- The one wording change is under "Contract changes".

Rejected instead (see "Rejected alternatives"):
- pure neighbour-reading lockstep, which needs ChunkData held to its exact opSeq, window edges that cannot step, and
  est. 200 KiB of in-order chunk data in front of a joiner's live events;
- server-side state replication, which costs est. 10-50 times the bytes.

## Cell model and data

### New world-palette ids (appended after `bedrock` = 20)

| Group | Ids | Count | Notes |
|---|---|---|---|
| Planks fire | `planks_k1..k6` (kindle), `planks_b1..b6` (burn) | 12 | |
| Wood fire | `wood_k1..k8`, `wood_b1..b8` | 16 | |
| Foliage fire | `foliage_k1..k3`, `foliage_b1..b2` | 5 | For `leaves` and `leaves_autumn` |
| Residues | `charred_wood`, `scorched` | 2 | `scorched` = burnt ground cover |
| Spell liquid | `knitwater_1..knitwater_8` | 8 | Level 1-8; 8 = full voxel. "Knit water" is a working name (open question 12). |
| **Total** | **ids 21-63** | **43** | 192 ids stay free; W4's planned 12 ids follow |

- Ids are added in the table's order through `MaterialLibrary::Add`. The order is part of `MaterialTableHash`.
- **Generated `water` (8) and `lava` (17) stay inert.**
  - Water is never stepped and never consumed. It counts as **wet** for fire: it quenches and blocks ignition beside
    it.
  - Lava does nothing in v1: the plaza's pit would make its chunks permanently active.
  - Loading or generation therefore never wakes anything.
- The liquid is named as a Knitting (setting.md section 9: conjured matter with a lifetime; its evaporation is that
  lifetime), not "welling": setting.md's spall wells and pooled spall are another thing. Its colour must stay distinct
  from whatever colour pooled spall gets. The stage that builds it adds "Pour = a Knitting of liquid" to setting.md's
  section 9 row (setting.md is not edited now).

### Material table values (Material struct, so `MaterialTableHash` covers them)

| Ids | Colour / emissive (presentation) | density, hardness | `strength[3]` (load check, kg/m^2) | flags |
|---|---|---|---|---|
| `*_k1..kK` (kindle) | Base colour; emissive ramps from 1 at k1 to 4 at kK | Base values | Base strength | `Flammable` off (already burning); `Burning` presentation flag |
| `*_b1..bB` (burn) | Darkening base; emissive 5 at b1 falling to 1.5 at bB | Base density; hardness 0.6 × base | Base × (B + 1 − j) / B, rounded down (planks b6 = 1/6) | `Burning` presentation flag |
| `charred_wood` | Near black, emissive 0 | Density 280, hardness 0.6 | 25 % of wood's | none |
| `scorched` | Dark dirt | Dirt's | Dirt's | none |
| `knitwater_1..8` | Conjured teal (distinct from pooled spall), roughness 0.1, emissive 0.3; GPU fill height L/8 (S4) | Density 1000, **hardness 0** | none | `Liquid` (**not** `Indestructible`) |

- Foliage stages stay weightless (`Foliage` flag kept on `foliage_*`).
- `knitwater_*` behaves like water elsewhere: it is not a collider (`CollisionFilter` excludes `Liquid`), it is ignored
  by the island search and the cell graph, and a blast carves it (hardness 0, so the existing `CarvableIds` takes it).
- `GraphNode` stays 72 B. The new ids are only extra `GraphMaterials` entries.
- The `Burning` flag and the fill height are presentation only (GPU material table); no rule reads them.

### `voxel::SpreadTable` (integers, authored in code)

A set of 256-entry `std::array`s in `vortex.voxel` (Core, GPU-free), built next to `WorldMaterials` from the palette
ids. It is never derived from float fields.

| Field | Per | Meaning |
|---|---|---|
| `kind` | id | none, unburnt flammable (class), burning (class, stage), liquid (level), ground cover, wet source (`water`) |
| `stage` | burning id | k_i → i; b_j → K + j |
| `next` | burning id | id after advancing (residue after the last burn stage) |
| `K, B, pace` | class | kindle stages, burn stages, burn pace |
| `dBelow, dReach, dSlope, dSide, dDiag, dJump` | class | ignition delays (0 = never) |
| `residue, quenchEarly, quenchLate` | class | residue id; base id on an early quench; id on a late quench |
| `soaks` | id | liquid on top of it soaks away on evaporation steps |
| `kPhase[4][4][4]` | voxel (x&3, y&3, z&3) | the burn phase pattern, integers 0..15 (below) |
| `kChunkPhase[2][2][2]` | chunk (cx&1, cy&1, cz&1) | the evaporation phase of a chunk: 8 distinct integers in 0..9 (below) |

- **`SpreadTableHash`** (every field, every id, the phase tables, fixed order) joins `buildHash` next to
  `MaterialTableHash` (`net/internal/Session.cpp:208`). A peer with another table is refused (`another build`).
- **Memory:** no per-voxel cost beyond the id byte.
  - Scratch: one padded box of 40^3 = 64 000 B (c) per worker that steps (pad 4 on every side; see "Reach").
  - Server: a `SpreadIndex` (below), est. 100 B per active chunk (counts plus a 64 B live-block mask).
  - Clients keep no spread state (presentation keeps its own, see "Presentation").

## The rules (exact, integer, synchronous)

Notation:
- `old(p)` is the id before the step. Nothing is written until every new id of the chunk is computed.
- `s` is the op's step index (u32): the number of the logical step, counting emitted logical steps only.
- **Face neighbours** = 6. **Sides** = ±x, ±z. **Side-diagonals** = (±1, 0, ±1). **Slope cells** = (±1, −1, 0) and
  (0, −1, ±1). **Jump cells** = (±2, 0, 0) and (0, 0, ±2).
- **The wall:** in the isolated pass every cell outside the evaluated chunk reads as `bedrock`. Bedrock is inert in
  every rule: not flammable, not burning, not liquid, not wet, solid. A test pins that the rule never names bedrock.
  (The server's real pass reads true ids around the chunk; see "Planning one logical step".)
- `burning(id)`, `stage(id)`, `liq(id)` and `lvl(id)` come from the table. `lvl(air) = 0`; `lvl` of anything not
  liquid is undefined and never read.
- `wet(p)` = a face neighbour of p is `water`, or `knitwater_L` with L >= 2.
- `D(p)` = `kPhase[x & 3][y & 3][z & 3]` in **absolute world voxel coordinates** (two's complement `& 3`).

### Fire (up and sideways, never down)

**An unburnt flammable voxel T of class c with `!wet(T)` becomes `c_k1`** when any of these holds (sources are
burning voxels in `old`; delays belong to the target's class):

| Source S relative to T | Condition | planks | wood | foliage |
|---|---|---|---|---|
| Below, (0, −1, 0) | stage(S) >= dBelow | 2 | 3 | 1 |
| Flame reach, (0, −2, 0), with old(0, −1, 0) = air | stage(S) >= dReach | 4 | 5 | 2 |
| Slope cells (4) | stage(S) >= dSlope | 3 | 5 | 2 |
| Sides (4) | stage(S) >= dSide | 3 | 6 | never |
| Side-diagonals (4) | stage(S) >= dDiag | 4 | 8 | never |
| Jump cells (4), with the cell between them air | stage(S) >= dJump | 10 | 12 | never |
| Anything with dy > 0 | never | – | – | – |

- **Never down.** No cell above T's layer is ever a source.
- The source's stage carries the delay, so an unburnt voxel needs no stored heat.
- "Any" is commutative, so nothing is tie-broken.
- A source's stage keeps rising until burnout, so a delay once passed stays passed.
- **The gap jump.** Fire crosses **one** voxel of air sideways, late in the source's burn (planks b4, wood b4), never
  two. worldgen.md's C7 party gaps (1 voxel) between buildings therefore carry a fire from house to house after about
  3 s of full flame, while lanes and streets (3 voxels or more) are the firebreaks. A player cuts a firebreak by
  blasting a gap of 2 voxels or more.
- **Foliage only climbs.** A leaf voxel catches from below, by flame reach or on a slope, never from beside. A crown
  burns as a cone upward from where it caught, and crown-to-crown spread happens only where the next crown's leaves are
  higher. A forest fire climbs and stops at the crown tops: bounded by geometry.

**Material variety falls out of the table.** Foliage tops out at stage 5 (K 3 + B 2). It lights wood above it
(dBelow 3) and on a slope (dSlope 5), but never wood beside it (dSide 6). Planks reach stage 12 and light everything.

**A burning voxel B of class c at stage t:**
1. **Quench:** if `wet(B)`:
   - kindle stage (t <= K): becomes `quenchEarly(c)`, the base id. Caught early, the beam is saved.
   - burn stage: becomes `quenchLate(c)`, which is `charred_wood` for planks and wood.
   - foliage: always becomes air.
2. **Burn:** otherwise:
   - a kindle stage advances every step (k_i → k_i+1, kK → b1);
   - a burn stage advances only when `(s + D(B)) % pace(c) == 0`;
   - the last burn stage becomes `residue(c)`: planks → air, wood → `charred_wood`, foliage → air.

**The burn phase pattern `kPhase`.** A fixed table of integers 0..15, authored in code, depending only on
(min(x&3, 3 − (x&3)), y&3, min(z&3, 3 − (z&3))): 2 × 4 × 2 = 16 cells, each a distinct value, laid out as an ordered
(Bayer-like) pattern. It is a position, not a chance: every voxel burns for the same 1..pace steps per burn stage as
before, but neighbouring voxels advance on different steps. Without it, `s % pace` is global, so every burn stage on
the map advances on one step in `pace` and every burnout lands together: a mechanical heartbeat in the picture, and a
6-10x peak in re-cooks, site checks and uploads on that step. With it, burnouts spread over all steps and read as a
crumbling dissolve. It is mirror-symmetric about every plane x = 4n − ½ and z = 4n − ½ (x&3 ↔ 3 − (x&3)), so a
setup mirrored about such a plane burns mirrored (`SymmetricSetupStaysSymmetric` uses those planes). If the owner
wants the plain global pace back, it is `kPhase = 0`: one table change (open question 16).

**Ground cover.** `grass` and `grass_dry` never catch from a neighbour; otherwise one fire would sweep the region's
ground layer. Only a Kindle sphere turns them into `scorched` (a flame burst in presentation). All other materials
never burn.

**Speeds and times at 0.125 m and 10 Hz** (c, from the table; they scale with `--voxel-size`). A front advances one
voxel per `delay` steps (a diagonal one per `dDiag` steps over √2 voxels).

| Class | Up | Side | Diagonal | Burn time (steps = K + (B−1)·pace + 1..pace) | Ends as |
|---|---|---|---|---|---|
| planks (K6 B6 pace 10) | 0.62 m/s | 0.42 m/s | 0.44 m/s | 57-66 steps, about 6 s | air |
| wood (K8 B8 pace 10) | 0.42 m/s | 0.21 m/s | 0.22 m/s | 79-88 steps, about 8.5 s | `charred_wood` |
| foliage (K3 B2 pace 3) | 1.25 m/s | never | never (slope 0.88 m/s up) | 7-9 steps, under 1 s | air |

- The pace's phase is a fixed position pattern plus the step index from the op, not a chance.
- Planks' side and diagonal speeds are close (0.42 / 0.44 m/s), so a floor front is nearly round, not an octagon.
- A 3 m plank wall lit at its foot burns to the top in about 5 s; its foot burns out about 6 s after it caught (c).
- A 5 × 5 m plank floor lit at one corner: the front reaches the far corner (7.1 m away) in about 16 s (c), the
  burning band behind the front is about 0.42 m/s × 6.2 s ≈ 2.6 m wide (c), and the floor is gone at about 22 s (c).
  The walls light from the floor below at once (dBelow 2). The share of the house's timber burning at the peak is
  est. 35-50 % (e); `--fire-demo` measures it (`Fire: peak burning fraction`) against a target of 35 % or more.

### Liquid (down and sideways, never up)

Cell classes in the step:
- **Air** (id 0);
- **Liq** (`knitwater_L`, level L in 1..8);
- **Solid** (everything else: `water`, `lava`, burning ids and the wall included).

Steps 1-4 change only Air and Liq cells, from these levels.

**1. Losses** (from `old`, once per step). With `e = ((s + kChunkPhase(chunk of c)) % 10 == 0)`, for each Liq cell c:
```
loss(c) = 2 if any face neighbour of c is burning                          // steam
        + 1 if e and above(c) is Air                                       // evaporation, 1 s
        + 1 if e and below(c) is Solid with soaks                          // dirt, grass, grass_dry, sand, snow, scorched
L0(c)   = L(c) − min(loss(c), L(c))
```
- **`kChunkPhase`** is a fixed table over the chunk's coordinate parities (8 distinct values in 0..9). A whole chunk
  evaporates on the same step, once per 10 steps, and the map's chunks evaporate on 8 different steps, so liquid
  chunks wake 8 ways apart instead of all at once. It is per chunk, not per voxel, so a settled pool's chunk sleeps 9
  steps in 10 (a per-voxel phase would keep every liquid chunk awake every step). The seam between two chunks is a
  one-unit (1/8 voxel) difference for at most 1 s.

**2. Fall, twice** (V1 on L0 gives L1, then V2 on L1 gives L2). For each Liq c, with `below` read from the sub-step's
input:
```
g(c)  = below(c) is Air or Liq ? min(L(c), 8 − L(below(c))) : 0
L'(v) = L(v) − g(v) + g(above(v))                       for every Air / Liq v
```
- Each cell has one cell above it, so there is no conflict.
- Capacity is read from the input level, so L' <= 8 always.
- Liquid only moves down here: 2 voxels per step, 2.5 m/s, a viscous, readable spell liquid.

**3. Level out, once, on fixed pairs** (from L2):
- The phase is `p = s % 4`: the axis is x when p is even and z when p is odd, and the offset is `o = p >> 1`.
- Pairs are cells with axis coordinate `a` and `a + 1` where `(a − o) & 1 == 0`, in **absolute world voxel
  coordinates**. Two's complement makes this right for negative coordinates. Chunks are 32 wide, so local parity
  equals global parity; never use window-relative coordinates.
- For each pair (a, b) with both cells Air or Liq: `hi` = the higher level, `lo` = the other. Skip it when:
  - the levels are equal;
  - `hi` is not supported (below(hi) is not Solid and L2(below(hi)) != 8): a falling column does not spread;
  - `lo` is Air and L2(hi) < 2: thin edges stop.
- Otherwise move `t = (L2(hi) − L2(lo)) / 2` (integer, floor) from hi to lo.

**4. Result.** L > 0 gives `knitwater_L`; otherwise Air.

**Reach (what a cell's result reads).** Down 4, up 3, sideways 2 (c):
- the levelling's support test reads `L2(y − 1)`; `L2(y − 1)` needs `L1(y − 2)`, which needs `L0(y − 3)`, which needs
  the id at y − 4 (steam: a face neighbour burning; soak: `below` Solid with soaks);
- `L2(y)` needs `L1(y + 1)`, which needs `L0(y + 2)`, which needs the id at y + 3 (evaporation reads `above`);
- a partner at x ± 1 needs its own `L0`, which reads x ± 2 (steam); fire reads 2 down (flame reach) and 2 sideways (the
  jump).

So the padded box is 40^3 (pad 4 on every side), StepBox computes L0 / L1 / L2 wherever the box holds their inputs,
which covers the chunk and the support cells below it, and the isolated-vs-real differences (the overrides) lie in
local y 0..3 at the bottom face, y 29..31 at the top and 0..1 / 30..31 at the sides. Debug builds and
`Spread.IsolatedPlusOverridesEqualsReference` assert that band per face. Because 4 voxels is one 4^3 block, the
live-block mask dilated by one block covers every cell a live voxel can change.

**Properties (each pinned by a test):**
- **Never up.** No step moves level to a higher y. The fall moves down only; levelling out stays in one layer.
- **No conflicts, no bias.**
  - Every cell is in at most one pair per step, and each cell falls into exactly one cell below.
  - Floor of half the difference is symmetric, so a setup mirrored about a plane between voxels stays mirrored.
  - The phase decides when a pair moves, never who wins.
- **Mass is exact.** Units change only by loss, which is counted: `poured = present + steamed + evaporated + soaked`,
  logged by `--flood-demo`.
- **It settles.**
  - With no losses, every change strictly lowers (Σ L·y, Σ L²) lexicographically: falls lower the first, transfers
    keep it and lower the second. So the state reaches a fixed point; there are no cycles.
  - The fixed point is pooled: in a basin, sideways flow happens only over solid ground or full voxels, and a partly
    filled cell below takes the inflow first. A crater fills layer by layer, bottom first.
  - On flat ground, a pair whose levels differ by 1 never moves, so a pour stops as a **mound** whose surface falls
    about one level (1/8 voxel) per cell, about 7 degrees, with edges at level 1.
- **It ends.** Evaporation and soaking remove at least one unit per surface cell per second, so all conjured liquid is
  gone within est. 10-60 s of the pour, by depth. setting.md: "Conjured matter does not last".

**Pour volume (c):**
- A Pour of r = 0.75 m (6 voxels) is about 905 full voxels, 7 240 units.
- A 1 m wide, 0.5 m deep trench needs 2 048 units per metre, so one pour fills about 3.5 m of it.
- On flat stone it stops as a mound: units ≈ π R³ / 3 with R in cells, so R ≈ 19 cells (2.4 m), about 2.4 voxels
  (30 cm) high in the middle, about 18 m² (c). `Spread.PourOnFlatStoneTerminalProfile` pins the radial profile.
  Drawn as full cubes this is a 2-3 terrace "jelly ziggurat"; S4 draws the top voxel of a column at L/8 height (see
  "Presentation").

### Fire and liquid together

Both rules read `old` only. Fire writes only flammable and burning voxels; liquid writes only Air and Liq voxels. The
sets are disjoint, so the step is one synchronous update.
- **Liquid beside fire:** the burning voxel is quenched (rule 1 of fire) and the liquid loses 2 units (steam), in the
  same step. A column falling onto a burning floor lands on step t; on step t+1 the floor is quenched and the liquid
  steams.
- **Liquid beside unburnt timber:** `wet` blocks ignition while level >= 2 lies beside it. A pour keeps a floor and the
  foot of a wall from catching while it lies there. On a wall the liquid falls past the face within a step or two, so
  a wall is protected by the Pour's douse (below), not by the liquid.
- **Generated water:** quenches and blocks ignition, and is never consumed.
- **A voxel that burns out to air this step** is Solid for the liquid until the next step.
- **Termination of fire:**
  - Stages move forward; residues are not flammable.
  - A quench resets a kindle voxel to its base id, but it stays wet, so it cannot re-ignite, until the liquid beside it
    is gone. That liquid is finite and steams 2 units per step beside fire.
  - So every fire ends: a voxel burns at most 88 logical steps after ignition, and the number of quench-and-relight
    cycles is bounded by the liquid that was poured. Because `s` counts emitted logical steps and every awake chunk
    steps in every logical step, a chunk sees every phase of `s % pace` and `s % 4`; no scheduling can lock a chunk
    onto a subset of phases.

### Destruction and the load check

**Blasts** carve burning voxels and `knitwater` like any voxel (hardness 0.6 × base and 0). The next step sees them
gone. A blast in a pool removes liquid at once, and the neighbours flow into the crater.

**Sites (server and offline only, R1).** A step marks a chunk as a site when it:
- (a) turned a collider-solid voxel into air (planks or foliage burnt out), or
- (b) moved a voxel into the last burn stage or into `charred_wood`.

With the burn phase pattern these marks are spread over all steps, not bunched on one step in `pace`. Each site is
checked like a cast's site:
- the island search on the cached chunk graphs, with the load check (shred.md Part 4b), seeded at the site chunk;
- on the server, under the **structure credit** of the SI plan (its own work and bytes, never the cast credit);
  offline, under a fixed cap of 8 site chunks per step (e);
- leftovers wait in SI-3's re-check queue (R7).

Released sets go out as `Detach` / `ClearMask` through `EmitOp`, after the step, as their own ops, charged to the
structure credit. Clients never decide a collapse.

The rule "never after `FillBox`, editor edits or generation" is kept: spread never runs at generation, and a site
check is the server's (or the offline Simulation's) decision after an op it made. This needs a minimal SI-3 queue if
SI-3 is not built first (stage S3).

**Burning pieces.**
- A `Detach` of burning voxels keeps their ids in the piece's model. Bodies are never stepped. The piece glows, and
  presentation fades its flames by the body's age.
- **The landing ignition is decided at release, not by the body.** When a site check releases a piece with 8 or more
  burning voxels, the server (offline: the Simulation, the same code) takes the piece's lowest burning voxel (lowest
  y, then (z, x) order) and casts an integer column ray straight down from it through the world (the piece is already
  cleared) to the first collider-solid voxel, at most 512 voxels. It schedules one `IgniteSphere` (r = 0.375 m) on that
  voxel's top, `fallTicks` later, with `fallTicks` from an integer square root of the drop in voxels and the session's
  constants. At emission it re-casts the column from the same voxel (the world may have changed) and applies the
  Kindle caps (over them: dropped, counted).
- This happens whether or not the piece gets a server slot (`sim::TakePieceSlot`), so whether a burning beam lights
  the floor never depends on hidden slot caps or on rubble versus body, and offline and session behave alike. If a body
  rolls and rests elsewhere, the fire still lights under where it fell from. A burning beam falls and lights the floor:
  the only way fire goes down.

**Players.**
- Burning ids stay colliders until burnout. A burnout under a player is a terrain change like a carve (`FlushBox`
  before every player step; the movement contract is unchanged).
- `knitwater` is not a collider: players wade.
- Burn damage and any slowdown in liquid are combat (M2) and **server-only**. Neither enters `sim::StepPlayer`, so
  `Movement.NoCorrectionAbove2cmOnFlatGround` is untouched.

## Spell entry points

| Spell | Op | Per-voxel decision (from the voxel, its old id and the op only) | Work (R5) |
|---|---|---|---|
| **Kindle** | `IgniteSphere`, WorldOpKind **7**: CarveSphere's layout, 17 B; `power` reserved, must be 0 (decoder rejects others). r = 0.5 m at the hit point. | unburnt planks / wood / leaves / leaves_autumn → `k1` of its class; grass / grass_dry → `scorched`; all else unchanged | `kOpWork` + sphere voxels / 16, as the other spheres |
| **Pour** (1) | `DouseSphere`, WorldOpKind **9**: the same 17-byte layout, `power` 0. r = 0.75 m at the hit point. | kindle stage → `quenchEarly`; burn stage → `quenchLate`; burning foliage → air; all else unchanged | as above |
| **Pour** (2) | existing `SwapSphere` (3), `from = 0`, `to = knitwater_8`, r = 0.75 m. The centre is the hit point + normal × r, quantised (the server decides), so the sphere sits in the air in front of the surface. | air → `knitwater_8`; all else unchanged | as SwapSphere today |

- A Pour is one cast that emits the douse first, then the swap (their voxel sets are disjoint, the order is fixed
  anyway). The douse gives an immediate, readable disc on a wall or a roof: late-quenched timber is charred and never
  burns again, early-quenched timber is saved and can relight only from the disc's edge (planks: about 2 s to reach
  its centre again).
- `IgniteSphere` does not test `wet`: that would read neighbours. A wet voxel lit by it is quenched on the next step.
- **Choosing a spell:** `PlayerInput` gains a `spell u8` (0 Blast, 1 Kindle, 2 Pour; wire format, S2), read with the
  cast button's edge. Offline (S1) the game keeps the chosen spell locally.
- **Keys** (to confirm, see open question 9): `Q` cycles the spell; the HUD's mode panel shows it.
- **Cast ray:** the server's cast ray skips `knitwater` (an id filter on the server's ray only, wire-neutral), so a
  puddle does not eat shots. Generated water still stops casts, as today.
- **Refusals:** `CastRejection` gains `TooMuchFire = 4` and `TooMuchLiquid = 5` (wire format). The HUD's existing
  "Cast not run" text shows them.
- **Offline:** the same keys, the same ops, applied by the Simulation.

## The step schedule and budgets

### Server tick (and the offline Simulation, the same code)

Tick order: `Receive`, `UpdateStreaming`, `StepPlayers`, `RunCasts`, **`RunSpread`**, `RunSiteChecks`, `StepPhysics`,
and so on. `RunSpread` acts on a **step slot**, when `serverTick % 6 == 0`. At a slot it refills the spread credits
and, if something is awake and both credits are >= 0, emits one logical step numbered `s = last s + 1`. Otherwise
nothing is emitted and `s` does not move. The offline game runs the same `sim::SpreadDriver` inside `Simulation`, so
offline and session take one path (from S2; S1's offline driver writes the real pass directly, see "Stages").

### `SpreadIndex` (server and offline only, never replicated)

- A `std::map` keyed by chunk in (y, z, x) order, holding: counts of burning and liquid voxels, the 512-bit mask of
  4^3 blocks holding a live (burning or liquid) id, and `quiet` (logical steps in a row in which the chunk was stepped
  and neither it nor its 26 neighbours changed).
- After every applied op, each chunk in its `EditReport::changed` is **recounted by a scan** (est. 10 µs per chunk),
  and its and its neighbours' `quiet` is reset. No edit list is trusted, and nothing iterates a hash container.

### Planning one logical step (server only)

1. **Awake set A.** In map order, the chunks with:
   - burning > 0 (stages advance), or
   - liquid > 0 and `quiet < 4` (4 consecutive logical steps cover the 4 levelling phases, so 4 quiet steps prove a
     fixed point until something changes), or
   - liquid > 0 and `(s + kChunkPhase(chunk)) % 10 == 0` (its evaporation step).
2. **Stepped set S** = A plus every neighbour (26) inside the server window whose **real mask** is not empty: some
   block of it lies within one block of a live block (its own, or a neighbour's across the face). A neighbour with an
   empty real mask cannot change and is left out exactly. Neighbours that hold no live id are **receivers**: fire and
   liquid can enter them.
3. **Work.** Counted from the `SpreadIndex` masks only (R5: never `graphsBuilt`, `chunksDecoded`, time or cache
   state), for both server passes:
   - `work(C, pass) = 1 000 + 64 × (evaluated blocks of C in that pass)`, where the real pass's evaluated blocks are
     the real mask and the isolated pass's are C's own live blocks dilated by one block within C;
   - `work(step) = Σ over C in S of work(C, real) + work(C, isolated)`.
   The step's work is charged whole, receivers included. The `Spread:` line logs the evaluated voxels per pass, and
   `Spread.PlannerChargesEverySteppedChunk` pins that the charge equals that count plus the per-chunk constants.
4. **Compute** every chunk of S with one function, `StepBox(40^3 ids, chunk coordinates, s, table) -> 32^3 new ids`,
   twice, **in parallel on the Simulation's `JobSystem`** (`ParallelFor` over S, one 64 000 B box per worker). Results
   are independent per chunk by construction; they are collected and written on the main thread in list order.
   - **real**: the pad holds the true old ids of every chunk inside the server window (offline: the world window;
     read with `ReadChunk`, so packed is fine). Chunks inside the window but **outside S are frozen**: their cells keep their true old ids for
     every read (support, `wet`, steam, fire sources, `above` for evaporation), but no fall or pair transfer moves
     units into or out of them (g = 0 into a frozen cell, pairs that include one are skipped, their own liquid does
     not move). A liquid cell above true air in a frozen chunk is then unsupported and waits a step (R7). Only cells
     outside the server window read as the wall.
   - **isolated**: the pad is all wall, the same code path the client's `ApplyOp` runs.
   - Overrides = the voxels where real != isolated, in the per-face band of "Reach" (asserted in debug builds and
     tests).
5. **List** = the chunks of S whose real result differs from their old ids, in (y, z, x) order.
   - A chunk whose real result is identity is left out; a client not stepping it is then exact.
   - Leaving it out is safe because a chunk's result writes only that chunk. Liquid that crosses a face changes both
     chunks, so both are listed.
6. **Parts.** The list is cut, in order, into parts of at most 8 KiB each (encoded). All parts carry the same `s` and
   were computed from one snapshot. That is exact: each chunk's result depends only on its own old ids and its
   overrides, and no part changes a chunk of another part.
7. **Credits (debt model).** Two credits, refilled at every slot and separate from the cast `eventCredit` and the
   `entityCredit`:
   - **work:** `kSpreadWorkPerStep` = 2 000 000 evaluated voxels per slot (e: at 2-4 ns per evaluated voxel, mostly
     inert ones, 4-8 ms on one thread, about 1.5-3 ms on the 3 workers of a 4-core laptop; replaced by
     `SpreadMeasure.NsPerEvaluatedVoxel`), clamped to [−8, +1] slots' worth;
   - **bytes:** 2 KiB per slot (20 KB/s) on average, burst 32 KiB, clamped at −8 slots' worth.
   A step runs when both are >= 0, is charged whole (it may drive them negative), and the next slots wait while either
   is negative. Over the budget the **whole world** steps more slowly, uniformly (R7: never wrong), and the admission
   caps below refuse new fires before that happens. Waiting slots are counted in the `Spread:` line.
8. **Hard cap.** A step whose work exceeds twice `kSpreadWorkPerStep` admits no receiver (a neighbour holding no live
   id) into S: fire and liquid wait at that chunk face (R7, the same for everyone because it is the server's list),
   and the awake set cannot grow. The caps keep this out of play; the `Spread:` line counts it.
9. **Emit.** `EmitOp(SpreadStep part, Credit::Spread)` for each part, back to back in this tick; each numbers the part,
   keeps it in the backlog and feeds `modified` from `report.changed`. Listed chunks feed `recentSpread`, not
   `recentChanged` (see "Boundaries, joiners, repair").
10. **Write.** The server writes each part's chunks, as it emits it, from the computed real result (= isolated +
    overrides) through `World::EditVoxelsInChunk`, the same write the client's apply ends in. It does not run a third
    pass. Debug builds, `--check-spread` and `Spread.ServerWorldEqualsDecodedApply` decode every part and apply it
    through `ApplyOp` to a copy, and require byte-equal chunks: the encoding round trip is pinned there.
11. **Sites** from the step go to `RunSiteChecks` (see "Destruction and the load check").

### Admission caps (server decisions, R1; e, tuned by measurement)

| Cap | Value | What happens |
|---|---|---|
| Kindle, local | refused while 16 or more chunks whose centres lie within 32 m of the cast point hold burning voxels | `TooMuchFire` |
| Pour, local | refused while 16 or more chunks within 32 m hold liquid, or the server window holds 200 000 liquid units or more (about 27 pours) | `TooMuchLiquid` |
| Global safety (both spells) | refused while the last 10 steps averaged more than 75 % of either spread credit, or 128 chunks hold burning voxels | `TooMuchFire` / `TooMuchLiquid` |
| Landing ignitions | follow the Kindle caps | dropped, counted |
| Hard cap | see planning step 8 | fire and liquid wait at a chunk face; counted; not expected in play |

The local caps keep one team's fire from refusing the other team's spells elsewhere on the map; the global safety
only binds when the budgets are nearly spent.

**Why a fire cannot sweep a region:**
- **Fuel by material.** Only planks, wood and leaves spread; a fire's reach is one timber or foliage cluster whose
  pieces are at most 1 voxel of air apart. worldgen's C7 party gaps join a row of houses into one cluster (a row burns
  as a block, from house to house); a lane or street (3 voxels or more) stops it. Foliage only climbs, so a forest fire
  stops at the crown tops.
- **Finite per voxel.** At most 88 logical steps burning.
- **Caps and credits** as above, and the Settle resets everything.
- A team can still burn a wooden quarter on purpose, with casts over time. That is gameplay.

## Determinism and replication

### Wire format (`kProtocolVersion` takes the next number after stage 8's; 11 if stage 8 leaves it at 10)

| Kind | Layout | Size |
|---|---|---|
| 7 `IgniteSphere` | as CarveSphere: kind u8, centre i32 × 3 (1/16 voxel), radius u16, power u16 = 0 | 17 B |
| 9 `DouseSphere` | the same | 17 B |
| 8 `SpreadStep` (one part) | kind u8; step varint; box origin chunk zigzag-varint i32 × 3 (x, y, z); box extent varint × 3 (each >= 1); chunkCount varint; per chunk, in (y, z, x) order: index delta varint, overrideCount varint, then per override: index delta varint, id u8 | see below |

- **Chunk index** = the chunk's linear index in (y, z, x) order inside the part's box; the delta is from the previous
  chunk's index + 1 (the first from 0), so chunks are strictly increasing by construction. Coordinates are i32, as
  every other chunk coordinate on the wire (within ±`kMaxSyncChunkCoordinate`).
- **Override index** = `x | y << 5 | z << 10` (local); the delta is from the previous override's index + 1. Ascending
  index is EditRegion's (z, y, x) voxel order.
- Each part is **self-contained** (no delta against an earlier op), because ring replay into one chunk needs each op to
  stand alone.
- **Size (c from e counts):** one house, about 12 chunks and 60 overrides: header about 14 B, chunks about 2-3 B each,
  overrides 2-3 B each, about 190 B per step (the i16 layout of the first draft was 283 B).
- **Decoder limits:**
  - at most 512 chunks per part; box extent product at most 2^22; box inside ±`kMaxSyncChunkCoordinate`;
  - chunk indices inside the box; override indices < 32 768;
  - ids < the palette's count;
  - at most 16 384 overrides per part, charged against a budget like `maskVoxelBudget`.
- **Other wire values:** `PlayerInput::spell`, `CastRejection` 4-5, `SpreadTableHash` in `buildHash`.
- Event kinds 1-6 and 32-36 are unchanged.

### Apply (`sim::ApplyOp`, kind 8, every peer, with or without a `ChunkClip`)

For every listed chunk inside the clip:
1. Copy its old ids into the padded 40^3 box, with the wall in the pad. A packed chunk is read as `EditRegion` reads it.
2. Build the live-block mask: the 512 4^3 blocks holding a burning or liquid id, dilated by one block.
3. Evaluate the rule on those blocks only. The other voxels are unchanged by rule, so skipping them is exact.
4. Write the overrides over the result.
5. Write the changed voxels through **`World::EditVoxelsInChunk(chunk, sorted (index, id) list)`**.

Clients evaluate a part's chunks in parallel on their `JobSystem` (`ParallelFor`) and write them on the main thread in
list order; the result does not depend on order (`Spread.ResultIndependentOfChunkOrderAndThreads`).

`EditVoxelsInChunk` shares `EditRegion`'s bookkeeping:
- `changed`, `MarkDirty`, `QueueChange` / `BumpRevision`, `PromoteFaceNeighbours`, `Materialize` / `unsynced`;
- the ceiling (never hit, see below).

**Shape-aware re-cooks go through `voxel::ChunkChange`, not `EditReport`.** The terrain colliders never see an
`EditReport`: they consume `World::TakeChunkChanges()` (`Terrain.cpp`, `ConsumeChanges`), which today marks every
queued chunk dirty. So:
- `ChunkChange` gains **`shapeChanged`**, ORed across edits until the change is taken.
- `EditVoxelsInChunk` sets it when some voxel's collidability changed (`CollisionFilter::collidable[old] !=
  collidable[new]`); `EditRegion` sets it on every edit, as today; loads, unloads and `SetChunkEncoded` keep it true.
- In `ConsumeChanges`, an edited change with `shapeChanged == false` skips `Touch` / `MarkDirty`, `pendingEdit` and the
  face-neighbour notices. It still bumps the revision (graphs, hashes) and the GPU slot's dirty state.
- Stage changes, ignitions, quenches and liquid moves therefore never re-cook; only burnout to air does. Checked so
  far: `CollisionFilter` (`Collider.cppm`) emits faces between collidable and non-collidable voxels only; S1 confirms
  the cooked shape reads nothing else per material before relying on it.

**Build ceiling.** A step never puts a non-zero id where there was air, except liquid moving down or sideways in its own
layer. All liquid entered through a Pour that was checked whole against the ceiling, so a step is never refused for the
ceiling (tested).

### ChunkSync and the ring on clients (membership, never the box)

`ChunkSync` and `OpRing` handle ops by their chunk box (`OpChunkRange`) today. A `SpreadStep` listing two fires at
opposite ends of arena250 has a box of about 69 × rows × 69 chunks, so every box-based path must test **list
membership** for kind 8:
- `OpRing::Entry` keeps the part's sorted chunk list (or a view into its decoded form) next to `lo / hi`.
- `OpRing::Push`'s drop tracking (stage 8: `m_DroppedTouch`, `m_DroppedUnknown`) records each listed chunk in
  `m_DroppedTouch`, and **never** sets `m_DroppedUnknown` for kind 8. Otherwise a box over 4096 chunks would make
  `CoversChunk` fail for every chunk with an older base: stage 8's "every pristine chunk still loading was asked for
  whole" failure. (`m_DroppedTouch` grows by distinct chunks only, bounded by the region; stage 8 owns its pruning.)
- `Walk` / `needs` (CatchUp) and `ForEachAfter` test membership; `TouchesNear` tests membership of the chunk or one of
  its 26 neighbours, so a map-wide box never makes a terrain-mode hidden chunk install as Content.
- `ChunkSync::Apply` iterates the list, not `ForEachChunk(lo, hi)`, for `passes`, `blocked`, the clipped path and the
  deferred-check drop.
- `OpTouchesChunk` = binary search in the list.
- A `SpreadStep` always takes the per-chunk path. A listed chunk that is current with base < seq steps; the others are
  skipped and counted as today.

This lands in S2 on top of whatever stage 8 commits (its names may move).

### Boundaries, joiners, repair

| Case | What happens |
|---|---|
| Listed chunk outside the client's window, or not held | No record, so it is skipped. It comes back as ChunkData when it enters the window (7a). |
| Window edge (arena250) | Steps normally: it never reads a neighbour. **No special case.** |
| Chunk entering the window mid-fire, or a late joiner | ChunkData at asOf holds every step up to its base: the fire is in the ids. If the data is behind, the ring replays the parts clipped to the chunk; each part is self-contained for it. If ahead, older parts are skipped by the base rule. If the ring cannot cover it, `CoversChunk` (by membership) decides, and the chunk is requested whole (7a) only if a dropped part listed it. **No new baseline, no new message.** |
| Loading, Unsynced, no store slot, packed, terrain mode | As today: replayed, requested, decoded or installed by the server's packing. Liquid falling into air chunks takes store slots, bounded by the liquid caps. |
| HashCheck while cells change | Compared at exactly the check's opSeq; both sides applied the same parts (all parts of a step go out in one tick, before the tick's checks). Listed chunks get a new revision every step (the server re-hashes when picked). They go into their own `recentSpread` list (64 entries), and the `HashCheckPlanner` takes at most 1 of its 4 recent picks from it, so chunks changed by casts, Detach and ClearMask keep their recent-check guarantee. A deferred entry over a listed chunk is dropped by membership. A mismatch makes the chunk Requested (Repair): the safety net, which should never fire. |
| Ring (1024 ops / 1 MiB) | Stage 8's soak measured a wrap in about 17 s at 10 casts/s on the arena (m, the `CoversChunk` comment in `Sync.cppm`); with +10 spread parts/s it wraps in about 14 s (c). By bytes, 1 MiB holds 50 s or more at the 2 KiB/step credit (c). Membership-based `CoversChunk`, not the floor, is what keeps joiners from re-requesting. |
| Server window edge | Outside is wall: fire and liquid stop there, in the 3-chunk margin outside the play bounds. |
| Linux / Windows | Integers only, a table authored in integers, sorted containers, no floats in any decision, `& 1` / `& 3` parity on two's complement. `Spread.GoldenScriptHash` runs in both builds. |

### Determinism traps (each has a test)

- The table and the phase patterns must not be derived from float fields.
- `ApplyOp` for kinds 7, 8 and 9 must never read a neighbour chunk. A test poisons the neighbours and requires an
  equal result.
- The pad value (`bedrock`) must be inert in every rule.
- Phase parity and `kPhase` are in world coordinates, never window coordinates.
- `s` counts emitted logical steps; a step is all-or-nothing over the awake set.
- The server's written result must equal the decoded op applied through `ApplyOp` (`--check-spread`, debug, a test).
- No `Hash3`, `rand`, `random` or `mt19937` in `internal/Spread*.cpp`: a configure-time grep fails the build.

## Presentation (client only; never in sim, wire or hashes)

- **Glow.** Stage emissive, char and liquid colours come from the GPU material table. Torch and lava already light their
  surroundings through emissive, so fire lights its surroundings at the same cost (measure).
- **Flicker (S1).** Without randomness a front in uniform planks is an exact contour, and one emissive value per stage
  draws it as crisp concentric bands. In the lighting / emissive shading pass (not `trace.glsl`, which CPU decisions
  share), ids with the `Burning` flag get `emissive *= 0.7 + 0.3 × noise(worldVoxel, time)` from a cheap integer hash.
  Randomness in presentation is allowed: it never feeds back. Cost: a few ALU per lit burning pixel, measured in the
  `--fire-demo` fixed-camera GPU line; the S1 screenshot at 5.75 m must show no equal-brightness bands.
- **`PresentSpread`**, like `PresentBlast`, turns each applied part's changes into effects inside the FxDirector budget
  (fx-budget.md). Random particles are allowed here; they never feed back.
  - ignition: a small `Flame()` burst;
  - burnout to air: embers and ash;
  - quench and steam: a `Smoke()` puff in white;
  - liquid that newly fills a cell under air: a splash, at most N per step.
- **Steady flames (S5).** A presentation-only `BurningSet` per chunk is rebuilt from the step's changes and by a scan
  on install. Each frame the FxDirector picks emitters from exposed burning voxels within about 40 m, nearest first.
  Height and rate come from the stage: kindle stages are tall flames, late burn stages are embers.
- **Smoke into fog** (fog.md stage 3, once built). Each burning chunk injects dust-grid density proportional to
  min(burning voxels, 512) at its highest burning column, per step. It is fair because every client steps the same
  voxels; it is presentation and never decides anything.
- **Liquid surface (S4).** The top liquid voxel of a column is drawn at L/8 of its height: a per-id fill height in the
  GPU material table, applied where the primary ray's hit inside a `Liquid` voxel is shaded (the ray continues when it
  passes above the fill). This stays out of every CPU-decided path (the server ray already skips `knitwater`). If it
  has to live in `trace.glsl`, that file is a tested contract: its DDA reference test is updated with it. Cost
  measured on the UHD 620 in S4. 16 levels instead of 8 (8 more ids) would halve the mound's slope; S4's screenshot
  decides.
- **Partial id uploads (S1).** A fire changes ids every step without changing occupancy (until a burnout). A changed
  chunk whose occupancy pyramid did not change uploads only its changed 64 B block ranges instead of 36 936 B.
- **Bodies.** A body with burning ids gets a few flame emitters, fading over 5 s of its age.
- Windowed look and feel are the owner's to confirm (game-ui.md checklist). Headless runs check logs and screenshots
  only.

## Costs (all estimates unless marked) and how they are measured

| Item | Estimate | Measured by |
|---|---|---|
| Rule evaluation | 2-4 ns per evaluated voxel on average (most are inert: one table lookup), 15-30 ns for an active one, -O2, scalar. A burning chunk evaluates 10-20 k voxels per pass (its dilated live blocks). Box copy and mask 20-50 µs per chunk per pass. | `SpreadMeasure.NsPerEvaluatedVoxel` (disabled measurement test) |
| Server, one burning house (about 12 awake chunks + about 8 receivers) | Real + isolated about 600 k evaluated voxels (c from e counts), about 1.5-2.5 ms on one thread, 0.5-1 ms on 3 workers per step. At the work credit about 1.5-3 ms on 3 workers. | `Spread:` line (ms per step, avg / max); `vortex_server --spread-drill`, `Ticks: server:` p99 |
| Server, 10 fires + 2 floods (acceptance case) | About 1.5-2 M evaluated voxels per step, inside the credit (e); `Ticks: server:` p99 <= 16 ms | `--bot-ignite` with ten sites and `--bot-pour` (S2, S4) |
| Site checks | As a cast's check per site chunk, under the structure credit; spread over steps by the burn phase pattern | `Blast:`-style counters in the `Spread:` line |
| Client CPU (4-core laptop, UHD 620) | The isolated pass of the listed chunks it holds, on its workers: typical fight 0.2-0.6 ms per step, at the credit about 1-1.5 ms per step (one frame in six). Acceptance: apply max <= 4 ms per step in the 10 fires + 2 floods case. | game `Spread:` line at exit (apply ms avg / max); `--fire-demo --after-fill` |
| GPU uploads | With partial id uploads: est. 3-10 KB per changed chunk, 30-60 chunks per step frame ≈ 0.1-0.6 MB, est. under 0.2 ms on the UHD 620. Without them it would be up to 60 × 36 936 B ≈ 2.2 MB in one frame (the per-frame upload cap of 128 chunks lets it all through). | `GPU` upload line, `--fire-demo` with a fixed `--camera` |
| Collider re-cooks | Only `shapeChanged`: est. 2-10 chunks per step at the peak (flattened by the burn phase pattern), 0.1-0.4 ms each on workers. Without the change: est. 30 × 7 per step, which is why it lands in S1. | re-cook counter in the `Spread:` line |
| Wire per client | One house about 190 B per step ≈ 2 KB/s (c from e counts). A pour flowing over faces: 60-130 overrides per crossing per step ≈ 0.15-0.4 KB per step. Ten fires + two floods ≈ 2-3 KB per step (e), at or just over the 2 KiB/step credit (20 KB/s); per-player downlink today is est. 7-37 kB/s plus 24 kB/s of events (networking.md). Server-side state replication of the same changes would be 30-150 KB/s (e). | `Spread:` bytes / overrides; `Sync:` line; the net test below |
| Memory | No per-voxel cost; 64 KB scratch per stepping worker; about 100 B per active chunk on the server | `--memory` |
| Ring | +10 entries/s, +0.2-1 KB/s; wraps in about 14 s at 10 casts/s (c) | `Join:` / ring counters |

**The `Spread:` line** (game at exit, `vortex_server` at exit and every 10 s) gives:
- logical steps emitted, parts, slots waited (work / bytes), hard-cap steps;
- awake, receivers and listed chunks avg / max;
- evaluated voxels per pass (real, isolated; client: isolated);
- ignitions, burnouts, quenches, liquid cells changed;
- overrides;
- bytes;
- ms per step (server: real + isolated on workers, write; client: apply);
- re-cooks;
- sites, releases and landing ignitions (emitted / dropped).

## Tests

**VortexSimTests (GPU-free):**
- `Spread.FireNeverIgnitesBelow`: a burning column in a timber block for 300 steps; no voxel below the ignition layer
  ever burns.
- `Spread.FrontSpeedsMatchTheTable`: golden front positions after 10, 20 and 40 steps, per class, up / side /
  diagonal / slope / reach.
- `Spread.JumpsOneGapNotTwo`: planks walls 1 voxel apart catch, 2 apart never do; foliage never jumps.
- `Spread.FoliageOnlyClimbs`: a lit crown never burns a leaf below its ignition layer except by slope cells; two
  touching crowns at one height: the second catches only above the contact.
- `Spread.SymmetricSetupStaysSymmetric`: fire and liquid, mirrored about planes x = 4n − ½ (inside one chunk for the
  liquid). Pins that there is no randomness and no scan bias, and that `kPhase` is mirror-symmetric.
- `Spread.BurnoutsSpreadEvenly`: over 60 steps of a steady planks fire, the burnout count per step stays within 2x of
  its mean.
- `Spread.EveryFireBurnsOutWithinBound`, `Spread.QuenchEarlySavesQuenchLateChars`, `Spread.WetBlocksIgnition`.
- `Spread.StepClockCountsLogicalSteps`: a planner forced to split a step into parts and a credit forced to skip slots
  still give every listed chunk phases 0-3 of `s % 4` in rotation and burn wood out within 88 logical steps.
- `Spread.LiquidNeverRises`: random setups; the test may use randomness, the rule never does.
- `Spread.LiquidMassIsExact`: poured = present + losses, every step.
- `Spread.LiquidSettles`: no change for 4 consecutive steps within a bound.
- `Spread.CraterFillsBottomUp`: a pinned level grid in a basin.
- `Spread.PourOnFlatStoneTerminalProfile`: a golden radial level profile of one pour on flat stone.
- `Spread.LiquidEndsByItself`.
- `Spread.FallingLiquidDousesBurningFloor`, `Spread.DouseQuenchesTheDisc`.
- `Spread.FrozenNeighboursKeepTheirFloor`: liquid at the bottom of a stepped chunk above air in a frozen chunk does not
  spread sideways (no phantom floor).
- `Spread.IsolatedPlusOverridesEqualsReference`: seeded random 3 × 3 × 3-chunk worlds and random S subsets. Isolated +
  overrides must equal a slow whole-world reference stepper that freezes the chunks outside S by the same rule, bit for
  bit, and overrides must lie in the per-face band (bottom 0..3, top 29..31, sides 0..1 / 30..31).
- `Spread.ApplyNeverReadsNeighbours` (poisoned neighbours) and `Spread.PadIsInert`.
- `Spread.ResultIndependentOfChunkOrderAndThreads` (shuffled, 1-4 threads).
- `Spread.ReplayIntoOneChunkEqualsLive`.
- `Spread.ClipEqualsWhole`: kinds 7, 8 and 9, every chunk clipped in any order = whole.
- `Spread.PlannerIgnoresCacheState` (R5: cold or warm graph and hash caches give byte-equal ops).
- `Spread.PlannerChargesEverySteppedChunk`: the charged work equals the evaluated voxels logged plus the per-chunk
  constants.
- `Spread.ServerWorldEqualsDecodedApply`: the server's written chunks equal the decoded parts applied through
  `ApplyOp`.
- `Spread.StepsNeverRefusedByCeiling`.
- `Spread.BurnoutReleasesAStructure`: planks posts under a stone slab; a site check releases it. The stage-1 arenas
  still release nothing.
- `Spread.FallingBurningPieceLightsFloorOverSlotCaps`: piece slots forced to 0; the floor under the released burning
  piece still ignites after its fall time.
- `Spread.ShapeUnchangedSkipsRecook`: stage changes and liquid moves queue a `ChunkChange` with `shapeChanged` false and
  no re-cook; a burnout re-cooks.
- `Spread.GoldenScriptHash`: a pinned WorldHash after Kindle + Pour + 300 steps on `PlaygroundTemplate`, printed like
  `kGeneratorGoldenHash`. It runs in the Linux and the Windows build.
- The extended `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes` (kinds 7, 8 and 9).
- `SpreadTable.HashCoversEveryField`.

**VortexNetTests:**
- Wire round trips of kinds 7, 8 and 9 (chunk coordinates far beyond ±32 768 included); the decoder rejects bad
  boxes, indices, ids and counts.
- `Spread.SplitStepAppliesExact`: a step cut into parts gives the same world as one part.
- `Spread.WindowedClientAgreesInsideWindow`: an arena-like region with a client window of 48. A fire crosses the window
  edge; a chunk enters the window mid-fire; a joiner arrives mid-fire; ring overflow leads to a request. 0 HashCheck
  mismatches.
- `Sync.SpreadStepMembershipInRing` (SyncTests): a part listing two chunks 60 chunks apart wraps the ring, and a loading
  chunk between them is still covered (`CoversChunk`), never requested whole.
- `Spread.FireDoesNotDelayCasts`: a fire at the byte credit leaves the cast delay histogram unchanged.
- `Spread.RecentChecksKeepCastChunks`: with a burning house, cast chunks still get recent HashCheck picks.
- `Spread.TenFiresTwoPoursWithinCredit`: ten kindled timber blocks and two pours: logical steps emitted >= 90 % of
  slots and bytes <= the credit (or the caps refuse the casts that would exceed it).
- `Spread.CastRejectedOverCaps` (local and global).
- `Spread.SpellInputRoundTrip`.

**Configure:** the no-randomness grep over `internal/Spread*.cpp`.

## Headless flags

All of them build their own scene east of the plaza, like `--probe`, so the default run's world is unchanged. They
imply the offline game and are refused in a session.

- **`--fire-demo`** builds a timber house: plank walls and floor, wood corner posts and beams, a stone upper room on
  the beams, a second plank house across a 1-voxel party gap, and a tree beside the first house whose crown reaches
  above its wall. At frame 30 it kindles the south-west corner at its foot. It logs `Spread:` every second and at
  exit, and requires:
  - `Fire: 0 voxels ignited below the ignition layer`;
  - `Fire: tree caught` (through the crown above the burning wall, by slope);
  - `Fire: neighbour house caught` (across the party gap);
  - `Fire: peak burning fraction P %` with P >= 35 (target, e);
  - `Fire: out after N steps` (with `--frames 1200`);
  - `Fire: collapse: N piece(s) released` (the stone room falls when the posts give out; offline from S1).

  `--camera fire-demo` frames it. A screenshot at frame 200 must show the glow without equal-brightness bands.
- **`--flood-demo`** cuts a trench (FillBox: 8 m × 1 m × 0.5 m) with a 1 m step down at one end, and pours twice at the
  upper end at frame 30. It logs `Flood: poured U, present P, steamed S, evaporated E, soaked K` (it must add up),
  `Flood: settled at step N`, `Flood: 0 voxels above the pour's top` and the level profile along the trench. With
  `--frames 1500` it must log `Flood: gone`.
- **`--douse-demo`** runs `--fire-demo` plus a Pour on the burning house's wall at frame 120. It logs
  `Douse: N burning quenched (early E, late L)` and must log E > 0, and no voxel of the doused disc that was quenched
  late burns again.
- With `--after-fill`: the same scripts after `World: window filled`, for timings on a quiet CPU.
- **`vortex_server`:**
  - `--bot-ignite N` / `--bot-pour N`: bots cast Kindle / Pour every N ticks at timber or open ground near them;
  - `--spread-drill`: the drill's sites get a kindled timber block and a pour;
  - `--check-spread`: the server decodes and re-applies every part and compares.

  The drill and the arena walk with these must exit 0 with `hashes equal`, `Checks:` 0 mismatches and a `Spread:`
  line. `tools/session-check.sh` gains a fire on a timber block between the clients.

## Stages (each verified, reviewed and committed; the smallest fun slice first; efforts are estimates)

| Stage | Contents | Acceptance | Effort (e) |
|---|---|---|---|
| **S1 Fire offline** (the first playable slice: a burning house that falls) | Fire ids and table values, `SpreadTable` + hash (local), `kPhase`, `StepBox` (40^3 box; fire rule, gap jump, foliage climb), `IgniteSphere` as a sim op (kind 7, not on the wire), `SpreadIndex` + an offline `SpreadDriver` (all-or-nothing logical steps, the work credit, the real pass with frozen neighbours, written directly), `EditVoxelsInChunk`, `ChunkChange::shapeChanged`, partial id uploads, the emissive flicker, burnout site checks offline (8 site chunks per step) and the release-time landing ignition, an offline Kindle key, simple `PresentSpread` bursts, `--fire-demo` | All fire tests, all existing tests, both headless runs unchanged, `--fire-demo` lines (collapse included) and screenshot, `Spread:` ms after fill, frame and upload cost on the UHD 620 with a fixed camera | large |
| **S2 Fire in a session** | Kind 8 with overrides and the compact encoding, parts, the isolated pass and `ApplyOp` for kinds 7 and 8, offline switched to the op path, wire kinds 7 and 8, `PlayerInput::spell`, protocol bump, `buildHash` + `SpreadTableHash`, `RunSpread` on the server on workers, work and byte credits (debt), `Emit` credit argument, local and global caps and `CastRejection`, kind-8 membership in `OpRing` / `ChunkSync`, `recentSpread`, bots and drills, session-check with fire | Drill and arena walk with `--bot-ignite` (`hashes equal`, 0 check mismatches), the ten-fires case (client apply max <= 4 ms, server `Ticks:` p99 <= 16 ms, steps >= 90 %), session-check `PASS`, net and sync tests; the owner's two-PC run with a fire (owner) | large |
| **S3 Collapse from fire in a session** | `RunSiteChecks` under the server's structure credit (a minimal SI-3 queue if SI-3 is not built), Detach / ClearMask charged to it, the landing ignition on the server | `--fire-demo` unchanged; `--neck-blast`, `--tower-blast` and the castle case unchanged; drill with fire releases and agrees | small-medium |
| **S4 Liquid** | `knitwater` ids, the liquid rule and interactions, `DouseSphere` (kind 9), the Pour spell and server-ray filter, the L/8 surface height (GPU), `--flood-demo`, `--douse-demo`, `--bot-pour` | Liquid tests, flood and douse lines, the surface screenshot and its GPU cost, drills agree with pours | medium-large |
| **S5 Presentation** | Steady flames, steam, splashes, smoke into fog (when fog stage 3 exists), burning bodies | fx inside its 4 ms cap; owner's windowed look | medium |
| **S6 Tuning** | Delays, paces, caps, credits and evaporation after the owner plays | the owner | small |

## Contract changes (the stage that builds them records them in CLAUDE.md, shred.md and networking.md)

- **ChunkClip (R2 wording).** "Each voxel decides from (voxel, old id)" becomes "each voxel decides from (voxel, old
  id), or for `SpreadStep` from its own chunk's old ids, plus the op's bytes". Applying every chunk with its own
  clipped call still equals the whole call.
- **R1 exception.** Clients re-run the spread rule: lockstep, by the owner's decision. Everything a peer could not
  compute (border values) arrives in the op, and every scheduling choice is the server's.
- **R2.** No `Hash3` in spread (owner). The burn phase pattern is a fixed position table, not a hash.
- **The step clock.** `s` counts emitted logical steps; a logical step is all-or-nothing over the awake set; its parts
  share `s`, have disjoint ascending chunk lists and are emitted back to back in one tick.
- `WorldOpKind` 7, 8 and 9, the kind-8 encoding, `CastRejection` 4-5 and `PlayerInput::spell` are wire format.
- `buildHash` covers `SpreadTableHash` (phase tables included).
- "Spread state lives only in voxel ids: no side table may decide a step."
- `ChunkSync` and `OpRing` treat kind 8 by list membership everywhere they use a chunk box.
- `Emit` / `EmitOp` take an explicit credit (`Cast`, `Entity`, `Spread`, `Structure`); spread parts never charge the
  cast `eventCredit`. (Today every `sim::WorldOp` charges `eventCredit`, and `RunCasts` holds casts while it is <= 0.)
- `voxel::ChunkChange::shapeChanged` decides collider re-cooks.

## Rejected alternatives

| Alternative | Score / reason |
|---|---|
| **A, state in voxels, as proposed** | 49/60. Kept as the base. Its liquid (a monotone sheet fed by 4 source ids, 32 levels, 1-voxel deep, never pooling) and its 89 ids are replaced. |
| **B, side layer, as proposed** | 43/60. Elapsed and hop counters beside the chunk mean four new contracts (layer iff live id, ChunkData layer, hash meaning, saves), each a desync class. Its liquid, its single function for the isolated and the real pass, and its scheduling-cannot-desync principle are kept. |
| Pure neighbour-reading lockstep | Ring replay into one chunk is impossible; ChunkData would have to be held to its exact opSeq; window-edge chunks cannot step; a joiner into a burning town would need est. 200 KiB in order before live events. |
| Server computes, clients receive changed voxels (masks) | Not lockstep (owner); est. 10-50 times the bytes. |
| Hash-noise ignition chances (R2 allows `Hash3`) | Owner: no randomness. Material delays and geometry give the variety; a symmetric setup burns symmetrically, which players can learn. |
| Moving liquid with pressure (rises in U-tubes) | The owner's "never up". |
| Moving liquid cell by cell with tie-breaks | Jitter and scan bias, the classic falling-sand bugs. Pairs on a fixed phase have neither. |
| Non-local pooling (Priority-Flood) as the rule | A peer holding part of the world cannot run it. It is kept as a test reference only. |
| A global "first N voxels" budget inside the rule | A client holding part of the world cannot recompute it. Budgets choose the list on the server instead. |
| Grass fires that spread | One fire sweeps the region's ground layer. B bounded it with a hop counter, which needs state outside the ids. |
| Steps on each peer's own tick count | Nothing to compare in HashCheck, nothing for replay. Steps are stream events. |
| Partial steps (a round-robin of awake chunks under a credit, `s` running on every slot) | The first draft. Chunks stepped on alternate slots see only some phases of `s % pace` and `s % 4`: fires that never end, liquid that levels along one axis, sleep at a non-fixed point. All-or-nothing logical steps instead. |
| Dropping the fires farthest from the players when over the byte credit | A dropped fire misses logical steps of the global clock (the phase lock above), and which fire freezes would depend on where players stand. The whole world slows uniformly instead, and the caps refuse new fires before that. |
| A per-voxel evaporation phase | It keeps every liquid chunk awake every step (no sleep). The per-chunk `kChunkPhase` spreads the wakes 8 ways and keeps sleep. |

## Open questions (with the recommended answer)

1. **Does fire ever go down?** No. Burning pieces that fall carry it (a release-time server op). Reason: the owner's
   rule; readable.
2. **Do planks burn to air or char?** Air; wood leaves `charred_wood`. Reason: plank walls vanish (fun, opens
   buildings); beams keep a weak, readable skeleton and fewer surprise collapses.
3. **Grass and fields?** Kindle scorches them; they never spread. Reason: bounded by construction, no state needed.
4. **Wet timber that stays fireproof after the liquid is gone?** Not in v1. Wetness lasts while liquid lies beside it;
   the Pour's douse protects walls. Reason: no extra ids. Revisit after play.
5. **Lava ignites?** Not in v1 (the plaza pit would never sleep). Later, lava could ignite only through an explicit op
   (an eruption spell).
6. **Visible liquid level?** Yes, in S4: the top voxel drawn at L/8 height, GPU only, measured on the UHD 620.
   16 levels only if the S4 screenshot still reads as terraces.
7. **Step rate.** 10 Hz (every 6 ticks). Reason: readable speeds, cheap, the ring keeps about 14 s. 15 Hz costs +50 %
   ops and work for little.
8. **Do casts pass through spell liquid?** Yes, on the server's ray only. Generated water stops casts as today.
9. **Spell keys.** `Q` cycles Blast / Kindle / Pour, and the HUD shows the spell. The owner confirms windowed. The
   number keys stay the editor's.
10. **Wind?** None. A deterministic per-session wind (one integer direction in `Welcome` that shortens one side delay)
    is possible later; it is not randomness, but it adds a rule. Not before play shows the need.
11. **Palette order with W4.** Whichever lands first takes 21 onwards; the other appends. `MaterialTableHash` and the
    goldens catch drift. Terrain goldens are unchanged by appending.
12. **Names.** "Kindle" (setting.md's Kindling), "Pour" and "knit water" (a Knitting) are working names; the setting
    owner names them. Ids are renamed freely before S2 (names are not wire format, ids are). The liquid's colour stays
    distinct from pooled spall.
13. **Editor.** Hide the new ids from the editor palette in v1, and pause the offline driver in the editor. Reason: no
    hand-painted fire in saved scenes, and no new save format.
14. **Voxel size.** The rules count voxels, so speeds in m/s scale with `--voxel-size` (±20 % at 0.1 / 0.15).
    Accepted; noted in the `Spread:` line.
15. **Burning bodies.** They keep their ids and glow; the floor under the release point lights after the fall time.
    Reason: bodies are server-authored; stepping them would need a second spread world. If resting pieces glowing for
    their lifetime reads wrong, map burning ids to `charred_wood` in the piece model builder (sim, both sides).
16. **The burn phase pattern (owner to confirm).** Recommended: keep `kPhase`. It is a fixed position pattern, not a
    chance (every voxel burns a fixed time), and without it every burnout on the map lands on the same step. If the
    owner reads it as randomness, `kPhase = 0` restores the global pace in one table change, at the cost of the
    6-10x peaks.
17. **Should a row of party-gap houses burn as one block?** Yes: setting.md's "Timber towns burn", and streets are
    the readable firebreaks. If play shows whole quarters going from one cast, raise planks' `dJump` (later in the
    burn) before touching worldgen's C7.
18. **Spread byte credit.** 2 KiB per step (20 KB/s per client) with the compact encoding; the ten-fires test decides
    whether it moves. Reason: per-player downlink today is est. 7-37 kB/s plus 24 kB/s of events; more than about
    20 KB/s for fire alone is out of proportion.

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
