# World generation: regions built from structures

[[../../../00 Docs Map|Docs map]] · [[../../ROADMAP|roadmap (M4)]] · [[../../../docs/world/setting|setting and story]] · [[../notes/voxel|voxel notes]]

## How others solve this optimally (web survey, 2026-11-21)

- Rule-driven assembly of hand-made parts is a shipped pattern, and WFC is its most general solver: extract adjacency rules from a small hand-authored sample, collapse the lowest-entropy cell first, and on a contradiction restart with a new seed — fast restart beats backtracking at game scale; shipped in Bad North, Caves of Qud, Townscaper ([mxgmn/WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse), [practical builder's guide](https://www.mysimulator.uk/content/articles/wfc-procedural.html)). Nested WFC cuts large-scene generation from exponential to polynomial time ([arXiv:2509.09919](https://arxiv.org/pdf/2509.09919)) — the same hierarchy as RegionPlan -> plot -> structure here.
- The integer `RegionPlan` stamped identically on every side is stronger than typical WFC usage: determinism by construction instead of by seed. Region types (arena / zone) with player caps, spawn points and bedrock as data, not code, matches what shipped zone-based MMOs do (Albion Online's ~430 zones, [zone map DB](https://www.albiondatabase.com/interactive-map)).

Status: design decision, 2026-11-17. Nothing is built or measured for it. Figures marked *(est.)* are estimates from
operation counts or proportions and must be measured. Every stage names the measurement that confirms or kills them.
Claims about today's code were checked against the sources (`voxel/Generator.cppm`, `sim/Region.cppm`,
`sim/Blast.cppm`, `sim/internal/Blast.cpp`, `sim/internal/Region.cpp`, `sim/Player.cppm`, `voxel/Connectivity.cppm`,
`voxel/Structure.cppm`, `voxel/internal/Material.cpp`, `net/Messages.cppm`) on 2026-11-17, and again after the review
of the same day (flood box centred on the cast, release cap per cast, island-box locality, 6-connected nodes, the
margin and the client window).
Other agents are changing `engine/src/net` and networking.md while this is written: every wire detail below
(enum values, the protocol number, message ids) is taken at merge time, never assumed.

This design answers the owner's request of 2026-11-16: a map generator that uses structures to build a solid world
with towns, forts and villages, plus a story layer that later gameplay situations can build on. It serves ROADMAP M4
("procedural terrain assembled with hand-made prefabs by rules") and the M3 regrowth item, and it follows the owner's
priority: **fun gameplay with few bugs over realism**.

**Originality is a rule, not a hope.** Real architectural traditions (public history) give structure, proportion and
dimension only. No real building, town plan, place name or sacred symbol is reproduced. No other game's buildings,
region layouts, names, characters, lore, story beats, palettes or signature theme pairings are used (see "Building kits",
originality rules). The owner named Genshin Impact as a look they like. We take from it only what the whole genre
shares and what that game itself borrowed from public sources: readable silhouettes, landmarks that pull the eye, dense
settlements, traversal. The kit names, region styles and anything with a name below are working names. Lore names are
the owner's call.

## Decision

1. **Structures are generator output, not template operations.** A new `RegionGenerator::Structured` wraps today's
   terrain generator. Its `Generate(coord)` stays a pure function of (inputs, chunk coordinate). It produces base
   terrain, then the plan's terrain shaping, then the plan's stamped structures for that chunk. Pristine towns
   therefore:
   - cost 0 chunk-sync bytes on join;
   - pack and evict like terrain;
   - are drawn in the 3-chunk margin outside the play bounds;
   - regrow by regeneration later (M3);
   - downsample for LOD level 1 later (M6).
   Template `WorldOp`s stay for small per-session variation (an open gate, a barricade), lights and replicated objects.
2. **The server plans; clients only stamp.** A **planner** runs once per session on the server (and in the offline
   game). It turns `(seed, recipe, style, kit library)` into a **`RegionPlan`**: compact integer records for terrain
   shaping, module placements, roofs, primitives, buildings and gameplay markers. It is about 30-60 KB for an arena
   *(est.)*. The server sends the plan once to each joiner before chunk sync. Both sides then run the same small
   integer **stamper** inside chunk generation. Only the stamper and the kit's module bytes must be bit-identical
   across platforms. The planner (A\*, plot growth, module choice, history) never runs on a client, so it can evolve
   and cannot desync anyone.
3. **One grid for everything: the kit cell, 16 x 24 x 16 voxels (2 m x 3 m x 2 m).**
   - It is exactly 2 x 3 x 2 of the load check's 8^3-voxel support cells, so a module's structural roles line up with
     the cell graph.
   - Two kit cells are one chunk width. One storey is 3 m. Every kit and every tradition snaps to it, with stylised
     proportions where history differs (a 2 m "ken", a 4 m bay of 2 cells).
4. **Buildings are kit modules on a massing grid, with parametric roofs and a few primitives.**
   - A building is a massing grid of storey cells. A neighbour-mask lookup picks each slot's module class (edge,
     corner, inner corner, interior). A seeded, constrained choice picks the variant: door, window, brace, shopfront.
     Socket-constrained WFC replaces the seeded choice for facades from stage W3.
   - Roofs are parametric integer-pitch records over footprint rectangles, with trim modules snapped on.
   - Towers, wall cores and domes are primitive records: box, shell, cylinder, octagon, dome, stair.
   - Every voxel of a module is a **role slot** (frame, infill, plinth, roof, trim, opening, metal, emissive, air,
     keep). The region's style maps slots to world material ids. One module serves many palettes, and **the role
     decides the material, so it decides both the look and the bond strength.**
5. **Pristine buildings stand with a 2x margin, and each landmark stage has one designed neck.** Three layers make
   this true:
   - construction rules in the planner and kits;
   - a closed-form rating per module, computed at kit load;
   - CI sweeps that fire duds at generated regions with loads doubled.
   A designed release is at most 45 000 voxels per part and 60 000 per cast, and every released voxel lies within
   84 voxels (10.5 m) Chebyshev of every voxel of its neck, so it fits under `kMaxIslandVoxels` (60 000) and inside the
   flood box of any cast that cuts the neck (see "Designed necks"). The load check still never runs after generation
   (shred.md Part 4b).
6. **Fights stay fair and navigable after destruction.**
   - Destruction has three readable tiers: never, strong and weak.
   - Every objective is reachable from every spawn by at least 2 routes, and one of them is a **spine**: terrain, road
     beds and stairs on terrain, with a bedrock core under its surface.
   - Fairness is checked on the intact plan and again on a "worn" plan with the weak tier and every designed release
     removed.
7. **History is a flag pass, not a simulation.**
   - 3-6 seeded local events, each placed in one of the setting's world epochs (docs/world/setting.md, section 7), set
     flags that later planner steps read: an older wall ring, a burned quarter, rebuilt upper
     storeys, a ruin.
   - Ruins use **columnar decay**: roofs, floors and cantilevers go, and walls are cut to a hashed height per 1 m
     column. They stand in compression only.
   - The region card's 3-5 history lines and the session's mission come from the "now" state, using reverse causality
     (decide the present, then rationalise the past).
8. **First playable slice: stage W1, about 1.5 weeks *(est.)*.** One village on an arena-sized region, offline, with:
   - one kit (Frame) of about 14 code-built modules;
   - existing materials only;
   - a segmented watch-tower landmark;
   - tests for determinism, `ClassifyColumn` agreement, 0 releases under duds, and the designed neck.
   W2 puts it in a session (about 1 week *(est.)*).

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

## Architecture

### Libraries and layering

One new library, **`Vortex::WorldGen`**: module `vortex.worldgen` in `engine/src/worldgen/` (`WorldGen.cppm` plus
partitions and `internal/*.cpp`). It is GPU-free and network-free, and it links `Vortex::Core` only:

```
Vortex::Core <- Vortex::WorldGen <- Vortex::Sim <- Vortex::Net <- Vortex::Graphics <- Vortex::Engine
                (Physics sits between Core and Sim as today; WorldGen does not need it)
```

| Partition | Contents | Runs on |
|---|---|---|
| `:plan` | `RegionPlan` records, encode / decode / validate, `PlanHash` | both |
| `:kit` | `KitLibrary` (code-built kits in W1, `.vox` modules from W4), role slots, styles, per-module ratings, `KitLibraryHash` | both |
| `:stamp` | `PlanIndex` (records per chunk column), the stamper, `StructuredGenerator : voxel::IWorldGenerator` | both |
| `:planner` | sampling, sites, roads, plots, foundations, massing, module solve, roofs, forts, history flags, markers, fast checks, `PlanRegion(...)` | server and offline game |
| `:names` | name syllable tables, history-line templates, blocklist | server and offline game |

Rules:
- Add `Vortex::WorldGen` to `vortex_assert_gpu_free` and `vortex_assert_no_network`. It links `vortex_options`
  (BMI flags). A new test target `VortexWorldGenTests` is GPU-free and network-free.
- Sim links WorldGen. `sim::BuildRegionTemplate` calls the planner for structured regions, and
  `sim::RegionWorldConfig` builds a `StructuredGenerator` when a plan is given.
- Net encodes and decodes the plan through Sim's re-exports. Only `Net` sends it.
- Third-party code stays out of module fragments. `--plan-png` uses stb, which is already compiled in
  `ThirdPartyImpl.cpp`.
- The load-check validation (dud sweeps, neck cuts) needs `sim::Blast`. It lives in tests and in
  `tools/region_check`, which links Sim, never in WorldGen.

### Changes to existing code

| Where | Change | Wire? |
|---|---|---|
| `voxel::TerrainGenerator` | An optional per-column **`ISurfaceModifier`** hook: height override (integer voxels), cover-material override by depth, cave mask, tree mask. Also export the integer column-height tile that `Generate` already uses. **With no hook, output is bit-identical** (test), so `kGeneratorGoldenHash` and every terrain golden stay unchanged. | no |
| `sim::RegionGenerator` | `Structured = 2` | yes |
| `sim::RegionTemplateId` | `Generated = 2` | yes |
| `sim::RegionDesc` | `recipeId` (u32), `styleId` (u16), `planHash` (u64). `IsValid` refuses: voxel size other than exactly 0.125 m (raw bits); an unknown recipe or style; a structured generator without `Generated`. | yes |
| `sim::RegionTemplate` | spawns per team (list), objectives, plan pointer. The single `spawn` stays for old templates. | no (server side) |
| `net` | New paced reliable message `RegionPlanPart { u32 offset; bytes }`, after `Welcome` and before any `ChunkData`. The client builds its `World` only once the plan is complete and `PlanHash(bytes) == planHash`; otherwise it refuses the session (`plan mismatch`). `buildHash` folds in `KitLibraryHash` and `kWorldGenGoldenHash`. 16 of the 64 probe chunks come from chunks the plan touches. One protocol bump: the next free number at merge (the code says 8 today). | yes |
| `sim::SimulationConfig` | Test-only `loadCheckMarginNum / Den` (default 1/1): multiplies node loads in the load check. Never set by a server, never on the wire. | no |
| `tools/server` | Session keys `generator = structured`, `template = generated`, `recipe = <name>`, `style = <name>`. Log line `Region: N buildings, M modules, plan K bytes, planned in X ms, structure chunks C`. | no |
| Game | `--region <recipe> [--style S] --seed N` (offline). `--camera region-landmark` / `region-street`. `--region-blast` cuts the landmark's neck and logs the release. The default run stays the playground, unchanged. | no |

### Where each piece runs

| Step | Server | Client | Offline game |
|---|---|---|---|
| Planner (P0-P13 below) | once at `Start`, before preload | never | once at start |
| Plan bytes | sent to each joiner | received, hash-checked | in memory |
| Base terrain | every chunk | every chunk | every chunk |
| Stamper | every chunk the plan touches | every chunk the plan touches | same |
| Variation ops, lights, objects | as today (`Start`) | as today | applied in order (`ApplyTemplateOps`) |
| Load-check proof | CI and `tools/region_check` only | never | never |

## The planner, step by step

The planner runs on the server and in the offline game only. It is still **integer-only** (int32 / int64, sorted
containers, explicit tie-breaks), so a seed reproduces the same plan and plan goldens can pin it. Its one float input is
`voxel::TerrainHeight`, converted to voxels with the generator's own floor.

Random numbers come from salted streams: `PCG32(HashMix64(seed, stepTag, entityId, attempt))`. Changing one step or
entity never shifts another. Units are voxels (int32) for geometry; planning grids are 1 m (one support cell), 2 m (one
kit cell) and 4 m (site lattice). All costs are for one 250 m arena on the owner's 4-core / 8-thread laptop, -O2
*(est.)*.

| Step | What | Output | Cost (est.) |
|---|---|---|---|
| P0 Sample | `TerrainHeight` on a 2 m grid over play bounds + margin (about 137^2 = 19k samples), and on a 4 m grid over the **scenery ring** out to play bounds + 24 chunks (W3; see "Margin"). Derive integer slope class, water, cliff (> 50°, `maxSlopeDegrees`), soil class (dirt to 2 m, sand 1.5 m on beaches) and domination (height minus the mean of a 24 m ring). | fields | 10-30 ms |
| P1 Frame | Arena: team axis, 2 spawn zones (the setting's **seams**, near opposite edges), 3 lane polylines, 3-5 contest points, the centrepiece site (a village, a fort or a `ledger_yard`). **Every seam is 80 m or less from some landmark** (the 92 m view radius; nothing beyond the window renders until LOD level 1); on a 250 m arena that is an outpost tower near each seam, not the central landmark. Zone: anchor and hierarchy (W5). | frame | < 5 ms |
| P2 History | 3-6 epochs from a small state machine per recipe. Sets flags only (see "History and story hooks"). Off in W1. | flags, events | < 1 ms |
| P3 Sites | Score 4 m lattice cells per settlement kind with integer interest terms in [-1024, 1024] (Emilien et al. 2012 technique; -1024 forbids). Greedy pick, exclusion disc, ties to the lower cell index. | sites | < 10 ms |
| P4 Roads | Integer A\* on the 2 m grid. Cost = length + slope + curvature + water (infinite until bridges) + cliff, with existing road cells at 1/4 cost (tree-like networks). Built in tiers: anchors and lanes, then sites, then outposts, then footpaths. Grades: streets at most 1:3, lanes 1:2; steeper parts become stair modules. | `Road` records | 20-100 ms |
| P5 Plots | Anisotropic land conquest on the 1 m grid (Emilien): seeds claim road frontage, corners go to the lower id, plots grow perpendicular to the road, slope raises cost, and roads, water, walls and cliffs are infinite. Simplify to at most 3 rectangles on the 2 m kit grid. Footprint at least 2 x 3 kit cells. Arena terms: no plot closes a lane; a gap of 3 m or more every 30 m of lane frontage *(est.)*. | plots | 10-50 ms |
| P6 Foundations | Per footprint by slope (table under "Placement rules"): pad, terrace or stilts. Every pad and terrace has a stone core to competent ground. Cave and tree keep-outs. | `Shape` records | < 5 ms |
| P7 Massing | The archetype picks storeys (1-4), setbacks, jetties (cantilever modules only), roof form and integer pitch. Result: a 3-D storey-cell grid of at most 12 x 6 x 12 per building. | massing | < 1 ms per building |
| P8 Module solve | Neighbour mask: class and rotation. Variant: seeded weighted choice under constraints (W1), socket WFC from W3 (fixed linear scan from the ground storey, street facade first, then clockwise; 64 backtracks at most, then the class fallback). Hard constraints: a door on the street side; 2 exits per room; doors only where terrain or a stair meets the sill (Emilien's fail and retry); lane-facing windows capped per facade. | `Module` records | 0.1-1 ms per building |
| P9 Roofs, trims | Roof records over footprint rectangles; trim modules on roof sockets (ridge ends, eave corners, gable fronts, finials). | `Roof`, `Module` records | < 5 ms |
| P10 Forts | Wall line, wall runs (1-D variant choice), gap-backed towers, gatehouse, keep, ditch (W3). Compounds by jigsaw sockets with overlap and terrain-fit tests, depth at most 6, fallback pools. | records | < 20 ms |
| P11 Markers | Spawns in spawn shelters, objectives, lights (lamp sockets, capped), crates, landmark and neck markers. | `Marker` records | < 5 ms |
| P12 Fast checks | Rating sums per building, landmark shafts included (C4); ground + building height 126 m or less; designed-release reach and per-cast totals; dense-chunk budget per 192 m window; graph-node budget; designed-release sizes; reachability with a spine; fairness (W3); landmark cadence (W5). A failure re-rolls the failing unit with `attempt + 1` (at most 4), then falls back: fewer storeys, a plain facade, a landmark whose upper stages switch to wood (never one that drops below the 20 m roof clearance: that is a failed unit). An arena that fails 8 attempts refuses to start (like today's `sited == false`). | report | 20-80 ms |
| P13 Emit | Sort records by kind, then building id, then slot order (the stamp order). Encode, hash, fill `RegionTemplate`. | plan, hash | < 5 ms |

**Total: 0.1-0.4 s per arena, single thread *(est.)*.**

## Data formats

### Plan (`RegionPlan`, version 1)

Fields are little-endian. Coordinates are int16 voxels relative to the plan origin: +-4 km, enough for a 1 km zone and
the full -48..128 m height. Records are sorted, and the global record index is the stamp order.

| Record | Bytes | Fields |
|---|---|---|
| Header | 48 | `'VXPL'`, u16 planVersion, u16 flags, u32 recipeId, u16 styleId, u8 attempt, u8 pad, u32 seed, u64 kitLibraryHash, i32 origin[3], u16 counts[4] |
| `Shape` | 20 | u8 kind (Pad, Terrace, Foundation, Cut, Rampart, Batter, Mound, SpineCore, CaveKeepOut, TreeKeepOut; later Channel), u8 fillSlot, u8 surfaceSlot, u8 skirt (voxels), i16 x0, z0, x1, z1, i16 y, i16 param (depth, facing thickness, batter profile), u16 owner, u16 reserved |
| `Road` | 16 | i16 a[3], b[3], u8 width (voxels / 2), u8 tier, u8 surfaceSlot, u8 flags (stairs) |
| `Roof` | 20 | u8 form (Gable, Hip, HalfHip, Pent, Flat, Tiered, Concave, Tent, Dome), u8 rise, u8 run, u8 overhang, i16 x0, z0, x1, z1, i16 baseY, u8 shellSlot, u8 trimSlot, u8 thickness, u8 axis, u16 building |
| `Module` | 12 | u16 moduleId, i16 x, y, z, u8 rot (2 bits) and mirror (1 bit), u8 theme, u16 building |
| `Prim` | 20 | u8 shape (Box, Shell, Cylinder, Octagon, Dome, Stair, Taper), u8 slot, u8 thickness, u8 rot, i16 box[6], u16 building, u16 param |
| `Decay` | 16 | i16 box[6], u16 building, u8 minCells, u8 maxCells. Columnar cap: clear the building's voxels above a hashed height per 1 m column, with the opening rule of C9. Decay records are applied after all other records, in record order (the stamper's last pass). |
| `Building` | 20 | u16 id, u8 archetype, u8 state (intact, damaged, ruin, overgrown), u16 flags (landmark, objective, rebuilt, burned), u16 settlement, i16 box[6] |
| `Marker` | 16 | u8 kind (spawn, objective, light, object, landmark, neck, contest, seam, yard; `seam` = a spawn and exit with its shelter, `yard` = a `ledger_yard`'s slab floor), u8 team, u16 id, i16 x, y, z, u16 facing, u16 param, u16 building |
| `Site` / `Event` | 16 / 8 | site kind, centre, radius, holder, state, name seed / epoch, kind, sites, factions, outcome. Used by the region card and the mission; never stamped. |

**Plan sizes (est.):**
- Arena (a village or fort, 60-120 buildings): 3 000-6 000 modules, 150-300 roofs, 400-800 shapes and roads, 200-500
  prims. That is 60-110 KB raw and **30-60 KB** after delta-coding the sorted coordinates.
- Zone: 5-10x that, cut into 64 m plan tiles (W5).

### Kits

A **kit** is a set of modules on the kit cell (16 x 24 x 16 voxels), with roof rules and archetype rules. In W1-W3,
kits are **built in code** by a small box DSL inside `vortex.worldgen` (`box`, `frame`, `slope`, `mirror`, `repeat`,
slots). They are deterministic, need no assets and are cheap to review. From W4, hand-authored MagicaVoxel modules load
from `engine/assets/kits/<kit>/` with a manifest, and both kinds produce the same in-memory form.

**Module voxels are role slots, not colours** (`.vox` palette index = slot):

| Index | Slot | Index | Slot |
|---|---|---|---|
| 0 | keep (transparent: leave what is there) | 6 | opening (glass from W4; open in W1) |
| 1 | frame (bearing) | 7 | metal |
| 2 | infill | 8 | emissive (torch) |
| 3 | plinth (bearing) | 9-14 | reserved |
| 4 | roof | 15 | deco (never bearing) |
| 5 | trim | 255 | air (cuts terrain, trees and earlier stamps) |

Manifest per module (W4 form; code-built modules fill the same struct):

```
[module wall_g_door]
file    = wall_g_door.vox
size    = 1 1 1              # kit cells (w h d)
class   = edge               # interior | edge | outer_corner | inner_corner | roof_trim | prop | landmark
storey  = ground             # ground | upper | top | any
sockets = left:fr_wall right:fr_wall top:fr_floor bottom:plinth out:street
weight  = 8
tags    = door exit
require = sill_ground        # Emilien-style: placed only where a sill meets ground or a stair
```

At kit load, each module also gets these derived integer fields, all hashed into `KitLibraryHash`:
- the role per 8^3 cell;
- the voxel count;
- the graph node count (one per connected component per cell);
- the mass per bottom cell column (kg, from material densities);
- the **rated top load per top cell column**. This is the closed-form compression capacity of the weakest bearing cell
  in that column: shared faces x the weakest material's C, then halved. The kit loader refuses a module whose bearing
  cells hold glass, torch, sand or snow slots (weakest-material rule, shred.md failure mode 2), or whose bearing
  members are thinner than 2 voxels. A **deco** slot in a bearing cell is accepted only if, in every theme of the
  style, deco maps to a material whose C, S and T are each at least those of that cell's bearing material (checked per
  theme at load; for example stone_dark deco on a stone member, iron on anything). Otherwise deco stays in non-bearing
  cells.
- for each roof trim and each `Roof` record: the vertical shell thickness against the pitch (C5).

### Styles (`engine/assets/worldgen/styles/*.style`, key = value like `.session`)

A style is a region's identity: a climate, a terrain preset of existing `TerrainSettings` fields, kits with weights, a
theme per kit (slot to world material id), the roof pitch set, the settlement grammar and the fortification type.

```
# highland_timber.style
terrain    = preset:highland        # existing TerrainSettings fields only, validated by voxel::IsValid
kits       = frame:3 logcrib:1
pitches    = 3:2 2:1 1:1
theme frame.oak   frame=wood plinth=stone infill=planks roof=planks trim=wood metal=iron emissive=torch
theme frame.dark  frame=wood plinth=stone_dark infill=planks roof=stone_dark trim=planks   # (W1 palette)
settlement = street_village hill_fort
fort       = curtain_wall
landmarks  = tower_house stage_tower
```

Recipes (`*.recipe`) list the region kind, lanes, the settlement mix and size ranges, objective kinds, and budgets
(lights, objects, variation ops, extra dense chunks). Styles, recipes and kits all fold into `KitLibraryHash`, so a
client from another build is refused (`another build`).

## Building kits (original designs from real traditions)

Every kit borrows **structure and proportion** from public architectural history. Its **silhouettes, ornament and
colours are designed fresh**. Before a kit's modules are authored, the owner reviews a one-page style sheet: 3
silhouettes, 2 themes and 6 ornament motifs drawn by us.

**Originality rules (checked in review at every new kit; names are also checked by a test):**
- Real buildings (named in the research) are dimension references only. Never replicate one.
- Never use real sacred symbols, scripture or recognisable sacred forms (crosses, the two-post double-lintel gate,
  calligraphy) on anything, and least of all on destructible targets. The game's sacred buildings are our own towers,
  halls and gates.
- Regions are **climate-driven cross-kit mixes**: every style draws its kits from at least two distant traditions, so
  no region reads as mostly one real country, and the set of regions never lines up one to one with another game's
  nations. Never pair one culture with the theme combinations another game is known for. In particular, never:
  - a windmill-and-cathedral town on windy green plains;
  - a cliff harbour among golden stone pillars;
  - storm islands with blossom trees.
- Palettes are our own. They are chosen per climate and saturated enough to read at 92 m, and they are never sampled
  from another game. Ornament vocabulary is invented per kit.
- Generated names come from our own syllable tables. A test checks them against a blocklist of real place names and
  known game names.

**Two identities per kit** (from the camera: 60° FOV at 432 px is about 2.4 mrad per pixel):
- **Far:** at 92 m a voxel is about 0.6 px, so only features of 2 m or more read: roof form and pitch, towers, tiers,
  and roof and wall colour.
- **Near:** at 5 m a voxel is about 10 px, so 1-4 voxel detail reads: frame patterns, brackets, lattices, log ends.

Near detail lives in deco voxels (slot 15) attached to members of 2 voxels or more (rule C6), never as free 1-voxel
lattices. In a bearing cell, deco must map to a material at least as strong as the member (C2); elsewhere it may be
any material.

**Pitches** are integer step ratios only: 2:1, 3:2, 1:1, 2:3, 1:2, 1:3. A concave roof is a polyline of these ratios.

| Kit | Structure drawn from | Grid use | Far identity | Near identity (our own) | Archetypes | Designed necks |
|---|---|---|---|---|---|---|
| **Frame** | Central European timber frames on stone plinths, jettied town houses, arcaded market halls | 1 kit cell = 2 frame bays of 1 m; storey = 1 cell (3 m); jetty 2-4 voxels per storey | Steep gables (3:2, 2:1) to the street, stepped overhangs, dark roofs over light walls | 2-voxel frame members in brace figures we draw, carved sill bands, small dormers | town house (2-4 storeys), market hall (open arcade below), barn, gatehouse, stage tower | plinth ring or ground posts (upper storeys + roof fall); arcade piers (hall storey falls); tower stage ring |
| **Bayhall** | Column-and-bracket halls on platforms, courtyard compounds, tiered towers | 4 m bay = 2 cells, odd bay counts (3/5/7); columns 4 x 4 voxels, chamfered | A broad concave roof floating over a pale platform; lifted corners (2-4 voxels); stacked roofs on major halls | Stepped bracket stacks with our own profiles, lattice door panels (deco cells) | hall, gate hall, courtyard compound (jigsaw), tiered tower | about 2/3 of a segment's columns (that roof segment falls). Roofs are cut into segments by cross and ridge joints: a segment's columns are at most 8 m apart in both directions (2 x 2 bays) with eaves of 1.5 m or less, so the reach rule of "Designed necks" holds; a 5-7 bay hall is 3-4 segments. Tower ground ring |
| **Tierkeep** | Narrow-front merchant rows and tiered keeps on battered stone bases | stylised "ken" = 1 cell (2 m); row frontage 3 cells; low pitch 1:2, ridge parallel to the street, pent roofs between storeys | Shrinking light tiers with dark roofs on a flaring stone base; continuous low eave lines along streets | Vertical slat screens, shaped gable fronts of our own outline | row house, keep (2-4 tiers), gate tower, wall run | keep ground-storey posts; each tier is a separate brick joined by a weak timber ring (one tier of up to 45k per cast; a keep falls tier by tier) |
| **Earthcourt** | Arid courtyard houses, fortified hill villages, wind shafts, domes | 1 cell = 2 m; walls 4-8 voxels; towers taper 1 voxel per 2 m | Cubic masses, flat roofs as fighting terraces, tapering corner towers, wind shafts, domes | Stepped parapets, relief bands and screens of our own geometry, bent entries | courtyard house, corner-tower compound, granary block, domed hall | tower base rings, dome drums, wind-shaft bases |
| **Logcrib** | Nordic and Slavic log cribs, longhouses, palisade forts, tiered timber towers | 2-voxel log courses with the corner voxel trimmed (round look); crib = 3 cells (6 m); log ends project 2-3 voxels | Long low roof hulls, steep 1:1 roofs, stacked roof tiers | Notched corners, carved ends of our own design, shingle rows | log house, longhouse, store on posts, tiered timber tower, palisade, gallery tower | tower masts; gallery towers at their base cribs; palisades open gaps only (wood barely fails) |
| **Towerstone** | Rubble-stone hill houses, arcaded streets, tower houses, terraces | arcade bay = 2 cells; piers 4-6 voxels; tower houses 3-4 cells square, walls 8 voxels | Clusters of slender towers, low 1:3 roofs and flat terraces stacked up a slope | Arch kits, shutters, loggias | stone house, arcade row, tower house (stages of 10 m or less), terrace wall | arcade piers (the frontage above falls); tower stage rings |

**Shared fort pieces** (every style themes them):
- **Wall run:** a `Rampart` core shape, a 3-voxel facing `Prim`, a wall-walk, and crenel modules (crenel and merlon 5-8
  voxels).
- **Gap-backed tower:** U-shaped, open to the inside; each stage up to 45k voxels.
- **Gatehouse:** twin towers and a passage 3-4 m wide; the upper storey falls into the passage and blocks it.
- **Keep:** on a `Batter` terrain base.
- **Ditch:** a `Cut`, 4 m deep, with 1:1 sides so it can be walked out of.

**Arches** are cantilever pairs under today's check and simply stand. Keystone-drop arches wait for a voussoir
material.

**Materials.** W1-W3 use the existing palette only: stone, stone_dark, brick, wood, planks, dirt, iron and torch, with
bedrock for cores.
- **Roofs** use planks, or a thin stone_dark shell at pitches of 1:1 or lower, never solid brick or stone (rule C5).
  A shell is never 1 voxel thick: stair-stepped voxels touch only at edges and fall apart (C5).
- **W4 adds a material batch**, appended after the existing ids (all values *(est.)*, to tune):

| Material | Density (kg/m³) | C / S / T (kg/m²) | Hardness | Notes |
|---|---|---|---|---|
| plaster (white, cream, ochre, earth-red ids) | 1600 | 30k / 6k / 2k | 0.8 | Weak infill: panels blow out and frames stay |
| tile_roof (terracotta, grey, dark ids) | 500 effective | 20k / 8k / 4k | 1.0 | Real tile is 47-80 kg/m²; a solid layer would be 5x too heavy |
| slate / shingle | 400 effective | as tile_roof | 1.0 | |
| thatch | 150 | low | 0.5 | |
| rammed_earth | 1950 | 50k / 10k / 3k | 2.0 | |
| adobe | 1600 | 30k / 6k / 1.5k | 1.2 | Holed domes slump |
| rubble_core | 2000 | 60k / 5k / 1k | 2.0 | Breaches slump instead of leaving bridges |
| wood_dark, wood_charred | 700 | wood strengths | 1.5 | Colour ids |

  - The batch changes `MaterialTableHash`, so `buildHash` changes and other builds are refused as today.
  - The plan carries world material ids validated against the table. **`WorldMaterials` (20 ids in `RegionDesc`) does
    not grow**, so no wire layout changes for materials.
  - Appending must leave the terrain goldens unchanged. W4 verifies that; if the order of `RegisterDefaultWorldMaterials`
    turns out to enter the goldens differently, the goldens are regenerated on purpose and `kGeneratorGoldenHash` is
    updated.

## Region styles

Each style is a cross-kit mix chosen by climate:

| Style (working name) | Climate and terrain preset | Kits | Signature (our own) |
|---|---|---|---|
| `highland_timber` | Wet uplands: high hills, steep valleys, low snow line | Frame, Logcrib | Dark frames on pale infill; steep dark roofs; stone stage towers as landmarks; street villages and hill forts. No windmill town. |
| `loess_terraces` | Dry rolling plateau, no sea | Bayhall, Earthcourt | Ochre earth walls under grey concave roofs; terraced lanes; a round earthen house-fort as anchor |
| `white_coast` | Warm coast with sea inside the bounds, low hills | Towerstone, Earthcourt | Whitewashed cubes stacked up a slope; arcaded streets; slender towers; flat roofs as fighting terraces |
| `ridge_keep` | Cold forested volcanic ridges, snow on the tops | Tierkeep, Logcrib | Grey battered bases under dark timber tiers with turf roofs; palisade rings on spurs |
| `red_badlands` | Hot red badlands and salt pans, wells, no river until W5 | Earthcourt (3), Tierkeep (1: battered stone bases and tiered granary keeps) | Red earth cubes on grey battered bases; tapering towers; tiered granaries |
| `fen_causeway` | Wet delta, reed beds, causeways (needs `Channel`, W5) | Logcrib (2), Bayhall (1: platform halls on posts) | Long low log hulls along causeways; raised halls above the reeds |

W1 ships `highland_timber` with the Frame kit only. It is closest to the existing plaza's materials.

**Regions of the setting** (docs/world/setting.md, 4.2) map onto styles as follows. "Earliest" is the first stage in
which a recognisable version can be generated; everything in "missing" waits for its stage or is not designed yet.

| Region | Style | Kits | Earliest | Missing until then (or not designed) |
|---|---|---|---|---|
| Tamberlough | `highland_timber` | Frame, Logcrib | W1 (Frame only, existing materials; valleys without rivers) | plaster / slate palette (W4); Logcrib (W6); rivers, water mills, covered bridge (W5); motte (`Mound`, W3) |
| Ushmara | `red_badlands` | Earthcourt, Tierkeep | W4 (Earthcourt only) | Tierkeep bases (W6); river oasis (W5); wind shafts and domes are Earthcourt archetypes |
| Kelmeri Terraces | `loess_terraces` | Bayhall, Earthcourt | W6 (the second of Bayhall / Earthcourt) | bank dwellings (Cut shapes plus front modules: not designed); round earthen house-fort and arch bridge (W5) |
| Calvessa Coast | `white_coast` | Towerstone, Earthcourt | W6 (Towerstone) | tower mills (archetype not designed); cove harbour piers |
| Tolvenn Basin | `ridge_keep` | Tierkeep, Logcrib | W6 (both kits); a full keep needs W3 tiers and shred.md stage 4c to grow | steam vents (FX only, not designed) |
| Brodvel Fens | `fen_causeway` | Logcrib, Bayhall | W5 (`Channel`) + W6 (Logcrib) | causeways, fords, fen islands, stilt stores (stilts W3), ring fort |

Each kit appears in two or three styles, and every style mixes kits from distant traditions.

## Settlement types

All sizes are compressed from real ones *(est., tune by play)*. "Dense chunks" means the extra dense chunks a client
window holds when the settlement is in view: a built column gains about 2 dense rows, and walls about 1 row per 4 m of
height *(est.)*.

| Type | Extent | Buildings | Content | Where | Extra dense chunks |
|---|---|---|---|---|---|
| Outpost | 10-30 m | 1-3 | Watch tower, wayside hall of our own form, bridge post, mill | Passes, hilltops, crossings, lane markers | 10-40 |
| Hamlet | 30-60 m | 3-8 | Houses, barn, stores on posts, a well | Slope under 15°, near a road | 40-120 |
| Village | 60-120 m | 15-40 | A green or square of 20-30 m, 1 landmark, a lane loop | Gentle ground, near water, terraces on 10-30° | 200-500 |
| Fort | 60-140 m | Keep, curtain, 4-8 towers, gatehouse, 4-10 inner buildings | Spur, hilltop or cliff edge, one approach, a ditch | Domination plus cliff adjacency | 300-700 |
| Walled town | 150-220 m | 60-120 | Wall ring with 2-4 gates and towers every 30-50 m, market square 30-50 m, 2-3 landmarks, main street and lanes | Zone anchor only (W5) | 700-1 200 (the window cap is the limit) |
| Ruin | As its source | Decayed | Foundations, wall stubs, rubble mounds, one damaged landmark | From the history flags | 60 % of the intact figure |

**Per region:**
- **Arena (250 m):** one centrepiece (village, fort or `ledger_yard`), 2-3 outposts marking the lanes, 0-2 hamlets,
  1-2 landmarks, 2 seams (spawn shelters, W3), each within 80 m of a landmark.
- **`ledger_yard`** (the setting's centrepiece, W3): a walled yard about 24-32 m square whose floor is a bedrock slab
  (the "never" tier, flush with the ground, no spire), 2-4 chime towers of the style's kit standing on their own
  foundations at the yard's corners, and its walls. The towers and walls are ordinary destructible buildings; C10
  keeps them off any bedrock face beside them. Marker `yard` on the slab.
- **Zone (1 km, W5):**
  - one anchor;
  - 2-4 villages in a loose K = 3 ring around it, bent by terrain and roads;
  - 6-12 outposts;
  - ruins from history;
  - 40-60 minor features (wall stubs, wells, carts as voxels, cairns), so that something new enters view every 60-100 m.
- **Landmarks:** 30-55 m tall, clearing roofs by 20 m or more, 1-2 per 250 m. At least one is within 80 m of each
  spawn. Ground height + landmark height is 126 m or less (the build ceiling is 128 m). A stone shaft above the C4
  limit tapers or continues in wood (C4); stage heights follow "Designed necks".

## Placement rules

**Foundations by footprint slope** (max-min over the footprint):

| Slope | Foundation | Records | Destruction role |
|---|---|---|---|
| < 10° | Pad: level the footprint plus 1 m, skirt at most 45° | `Pad` + `Foundation` | none |
| 10-30° | Terrace: stepped pads, each held by a stone retaining wall 4 voxels thick | `Terrace` + facing `Prim` | strong; terraces become high ground and layered cover |
| 30-50° | Stilts: timber posts on stone footings, ground storey open (from W3) | `Foundation` footings + post modules | **designed neck**: a few casts send the house down the slope |
| > 50°, or water | not built (cliff-edge wall runs only) | | |

**Bearing.**
- A `Foundation` is a stone prism from the building's base down to 0.5 m below competent ground: under 2 m of
  grass and dirt, and 1.5 m deeper on beach sand.
- Contact is the whole footprint, never a downhill edge.
- `Foundation` and `Pad` records are aligned to 8-voxel cells and extend at least one full cell beyond every bearing
  member above them. No grass, dirt, sand or snow voxel shares a cell with a bearing member or with its foundation,
  from the member down to competent ground: a node is rated by its weakest material (shred.md failure mode 2), and a
  stone foundation cell that touches turf is rated as dirt (C 50 000), which holds only 9.6 m of stone wall at
  margin 2. Paving inside those cells is stone; soil and grass start in the next cell out.

This is the playground's 3.75 m stone foundation done as terrain. Without its foundation, the watch tower filled onto a
slope was 2.29x over and fell at the first dud; on its foundation it held with a 5.2x margin (shred.md).

**Skirts** blend pad height into the natural height with integer arithmetic only:
`h = h_nat + (y - h_nat) * (k - d) / k`, with d the Chebyshev distance in voxels and integer division.

**Water.**
- Plots sit at sea level + 1 m or higher, and 3 m or more from water.
- Important fights stay 10 m or more from water, because water beside a crater stands as a wall (ROADMAP).
- The generator has no rivers. Settlements use coasts, lakes and dry ravines until a `Channel` shape exists (W5, with
  the owner's look check).

**Cliffs.**
- Cliff cells are edges and are never built on.
- Fort wall lines snap to cliff edges within 20 m: the wall sits on rock, and that side gets no towers.
- Buildings keep 4 m from cliff tops.
- Stairs climb cliffs in switchbacks of stair modules on terrain.

**Caves and trees.**
- Caves reach 30 m below the surface. They are suppressed under every footprint plus 4 m and under every road and
  spine (`CaveKeepOut`), so craters cannot open voids under foundations.
- Trees are suppressed within footprint + 3 m (canopy radius 2.6 m) and on road corridors (`TreeKeepOut`).
- Caves elsewhere stay, as flank routes.

**Height.** No record reaches within 2 m of the 128 m build ceiling: an edit above it is refused whole. For every
building, ground height + building height is 126 m or less; P12 checks it. Default `TerrainSettings` reach about
3 + 10 + 9 + 55 = 77 m, so a 55 m landmark needs ground under 71 m; recipes pick landmark sites accordingly.

**Separation.** Independently rated buildings keep at least **1 voxel of air** between them, hidden behind trims that do
not cross the gap. Their graphs then stay separate, a release takes one house, and a terraced row never merges into one
anchor over 60 000 voxels. The gap is made by **party-edge module variants** whose outer voxel layer is air (slot
255), so module origins stay on the 8-voxel grid (C2).

**Margin.** Objectives and landmarks lie at least 12 m inside the play bounds, so their flood boxes do too.
`CastFitsRegion` checks only the chunk of the cast's centre, so a cast centred on the edge carves up to its radius into
the 3-chunk margin and its island box reaches 3 m further:
- Margin structures within (the largest cast radius the session allows + 3 m) of the play bounds are bedrock-faced or
  absent: 4.5 m at today's 1.2 m player cast. Destructible scenery starts at least that far outside the bounds, so
  no island box meets it and nothing past the bounds falls. No building straddles the play bounds. A cast aimed from
  inside may still scar the first metres of terrain past the bounds.
- A larger spell radius (up to `kMaxCastRadiusQ`, 9 m) widens that band; the recipe states the largest radius it allows.
- **Scenery ring.** A client window on a 250 m arena follows the player and is never clamped to the region
  (`ClientWorldConfig`), so a player at the edge sees up to about 92 m past the bounds. From W3, P0 / P13 plan a
  scenery-only ring out to play bounds + 24 chunks: terrain shaping and a few silhouettes (outposts, hamlets, wall
  runs), with no rating, neck or fairness checks, since nothing there is reachable. The server never generates it
  (its window is play bounds + 3 chunks). In W1-W2 clients show bare terrain there; that is stated, not hidden.

## Load-check guarantee

**Facts this rests on** (shred.md Part 4b; `sim/Blast.cppm`):
- The check runs only after a cast, duds included. It never runs after generation, `FillBox`, editor edits or loading,
  and that rule stays.
- A pristine building that is already overloaded falls at the **first nearby dud** (failure mode 6), so standing must
  be designed in.
- A node is a connected component of an 8^3 cell, rated by the **weakest material in it**.
- Anchors:
  - parts over 60 000 voxels (`kMaxIslandVoxels`);
  - parts reaching the flood box face (+-96 voxels);
  - Indestructible voxels;
  - chunks solid through all 32^3 voxels.
- Network budgets: 8 192 nodes, 65 535 arc pairs, flow work 65 536. Above them nothing is judged.

### Construction rules (planner and kits)

| Rule | Content |
|---|---|
| C1 Bearing | Every bearing cell rests on a bearing cell of the module below (sockets enforce it), or on a `Foundation` / `Terrace` / footing that reaches competent ground. A floating bearing cell is a planner error. |
| C2 Cell alignment | Module origins are multiples of 8 voxels (gaps between buildings come from party-edge variants, never from shifted origins). Glass, torch, sand and snow slots never share a cell with a bearing member: openings occupy whole cells, with a lintel cell above. Deco shares a bearing cell only where every theme maps it to a material at least as strong (C, S and T) as the member; the kit loader checks it per theme. `Foundation` and `Pad` records are cell-aligned and keep soil out of bearing cells (see "Bearing"). |
| C3 Mixed cells | A cell holding frame and infill is rated as the infill, and the rating sums use the weakest material of each bearing cell. Once infill is blown out, the wood-only cells are stronger, so the skeleton stands. |
| C4 Margin 2x | Building load on every bearing column is at most the module's rated top load, which is already halved. Stone free columns 19 m or less; stone cantilevers 7.7 m or less; hanging stone 2.9 m or less; hanging brick 2.1 m or less; stone deck spans 12 m or less between piers. Overhangs only through cantilever modules (jetty, eave, balcony) of 1 kit cell (2 m) or less. **Landmark shafts:** a straight shell's base stress is density x height whatever its wall thickness, so a shaft taller than 19 m of stone (27 m of stone_dark, 21 m of brick at today's densities) must either taper, with base bond area (m^2) >= 2 x mass above (kg) / C (kg/m^2) at every level, or continue in wood or planks above that height (57 m and 41 m limits). P12 checks every landmark against this. |
| C5 Roof weight and shell | Roof slots use light materials (planks; a stone_dark shell only at pitches of 1:1 or lower; tile_roof at effective density from W4), never solid brick or stone layers. Nodes and floods are 6-connected, so a stair-stepped shell must overlap from step to step: **vertical shell thickness >= the largest rise per 1-voxel run + 1** (1:2 and 1:1: 2 voxels; 3:2 and 2:1: 3 voxels). Checked when a `Roof` record is emitted and by the kit loader for roof trims. |
| C6 Graph-friendly | Bearing members are 2 voxels thick or more. 1-voxel detail is deco and shares a face with a member of 2 voxels or more (in a bearing cell under C2's material rule). No free 1-voxel lattices: they multiply nodes, exhaust the budgets (so nothing falls) and erode at LOD level 1. No solid chunk-aligned 32^3 cube in a part meant to fall. |
| C7 Separation | 1 voxel of air between buildings, made by party-edge module variants. Walls and towers join through a designed weak joint (a thinner course or a timber walk). |
| C8 Lights | Torch voxels sit in their own cell, in a lantern module, never in a bearing cell. |
| C9 Ruins | Columnar decay only. Ruins emit no roofs, floors or cantilever modules; walls are capped per 1 m column. **Openings:** a column crossing a door or window is capped at its sill unless both jamb columns next to the opening keep their caps above the lintel top (the stamper decides the jambs first, then the opening's columns). So no lintel or wall stretch is left floating or cantilevered. A ruin then stands in compression, far below the 38 m stone column limit. |
| C10 Bedrock apart | Bedrock is an anchor (it never fails and always supports), so a destructible structure touching it at the side or from below could never fall. Destructible parts may rest on bedrock from above (as on ground), but keep at least 1 voxel of air from any other bedrock face above the ground: spawn shelters, keel stairs, the `ledger_yard` slab's edges, bedrock platforms. A keel route up to a height stands as its own bedrock stair, apart from any destructible tower. |

### Designed necks

Every archetype declares its neck in its rule: the **neck's cut volume**, the voxels a cast must remove to release it.
The flood box is centred on the cast, not on the neck (`FloodBox`: floor(centre) +- 96 voxels), and a part that
reaches its face is an anchor (shred.md failure mode 5). A cast that cuts the neck has its centre within the cast
radius of some voxel of the cut volume, which may be the farthest one. So a designed release:
- is at most **45 000 voxels per part**, and the parts one cast releases total **60 000 voxels or less**
  (`kNeckMaxReleasedVoxels`; the rest stays overloaded until a later cast nearby, shred.md failure mode 7);
- has every voxel within **96 - ceil(castRadius / voxel) - 2 voxels Chebyshev of every voxel of the neck's cut
  volume**: 84 voxels (10.5 m) at today's 1.2 m cast. A larger spell radius shrinks it; the recipe's largest radius
  sets it;
- releases 1-4 large parts, never a cascade of small ones. That fits the piece budget: 2 new per tick, 64 awake, 256
  live, and pieces of 262 voxels or more replicate.

Consequences: a wide neck shortens the reach (a tower ring 8 m across allows about 6.5 m of stage to each side of its
centre, and a stage top at most 10.5 m above the neck's lowest course), hall roofs are split into segments (Bayhall), and a tiered keep releases one
tier per cast.

| Archetype | Neck | Release (est.) | Casts to cut (est.) |
|---|---|---|---|
| Town house | ground posts or plinth ring | upper storeys + roof, 20-40k | 3-6 |
| Market hall | arcade piers | hall storey + roof, 25-45k | 4-8 |
| Bay hall | about 2/3 of one roof segment's columns (segments of 2 x 2 bays or less) | one roof segment, 15-45k | 4-8 per segment |
| Stilt house (W3) | posts | the whole house, 15-35k | 2-4 |
| Stage tower / tower house | ring of the next stage (stages 10 m or less) | one stage, 30-45k; the stage below stands | 4-7 per stage |
| Tiered keep (W3) | ground-storey posts, then each weak ring | one tier of 30-45k per cast, 2-4 tiers in all | team effort |
| Gatehouse | piers | upper storey falls into the passage | 4-8 |
| Wall tower, gap-backed | base ring | tower top, 30-45k | 4-7 |
| Bridge span (W5) | pier footing | span, 20-45k | 4-8 |

**Limitation, stated plainly:** a whole 20-40 m stone tower is about 200 000 voxels *(est.)*. It is an anchor and
**cannot fall as one piece** until shred.md stage 4c (castle bricks). Until then, landmarks and keeps are stacks of
designed stages. "Whole castles collapse" means stage by stage. Stage 4c comes before keeps are made bigger.

### Three layers of proof

1. **Kit load** (every start, microseconds): modules violating C2, C3 or C6 are refused, and ratings are computed. The
   planner's P12 sums every building's loads against the ratings (C4). This is cheap enough to run on every plan.
2. **CI** (`VortexWorldGenTests` plus Sim-linked tests):
   - **Dud sweep** (used by every test below): the load check judges only the island box, the cast's bounding box +
     24 voxels (about +-4.2 m for a 1.2 m cast; shred.md failure mode 4), so a sparse grid misses necks. A sweep is a
     3-D grid with spacing of at most 2 x (castRadius + 3 m) - 1 m (a 6 m grid at 1.2 m), over every grid point within
     4 m of a structure voxel, plus one dud centred on every bearing cell layer that a module or prim marks as a neck
     or a storey base.
   - `WorldGen.ArchetypesStand`: every archetype x 8 seeds x 3 slope classes, built on generated terrain in a test
     world, with a sweep over the archetype's whole box at `loadCheckMarginNum/Den = 2/1`: **0 voxels released**. The
     set includes a 40 m and a 55 m landmark on dirt-covered slopes (C4 shafts and the cell-aligned foundations).
   - `WorldGen.DesignedNecksRelease`: cut each neck with `FillBox` (the `--neck-blast` pattern), then fire casts at the
     neck's extreme corners (top, bottom, each side), each in a fresh copy of the world. Every one releases the declared
     parts, each at most 45 000 voxels, at most 60 000 per cast, and within the reach rule.
   - `WorldGen.RegionNeverReleases`: 3 seeds per recipe (fast). Generate the play bounds and sweep every building at
     margin 2: 0 releases. A 200-seed run is a disabled measurement, run by hand or nightly. Cost *(est.)*: about
     1 000 duds per village at 3-10 ms each, 3-10 s per seed.
   - `WorldGen.RuinsDoNotFloat` (W4): run `voxel::ExtractFloatingIslands` over every stamped ruin box in a test world,
     with no blast: 0 islands.
   - Every test also logs graph nodes per 24 m box against the budget below.
3. **Tool:** `tools/region_check --recipe R --seed N` plans and generates a region headless and runs the same sweeps,
   budgets and fairness checks. It prints a report and writes `--plan-png`. It is used for every new kit, archetype or
   recipe, and for any seed a player reports.

There is no runtime check: the server preload has no room for it. The confidence is statistical over seeds and
parameters, not a proof for every plan. The failure is benign: an unexpected release happens only after a player's cast,
never on spawn. Its seed reproduces it.

## Destruction gameplay

**Three readable tiers, the same in every kit:**

| Tier | What | Look |
|---|---|---|
| Never | Bedrock cores under spawn shelters, objective platforms and spine roads; terrain itself below foundations | Bedrock shows a distinct dark tone where a crater reaches it |
| Strong | Stone, stone_dark and brick bearing members: plinths, piers, tower walls | Consistent per style: the plinth always reads as the plinth |
| Weak | Wood, planks, glass; from W4 plaster, thatch, adobe and tile | Infill, roofs, frames |

Roles decide slots and slots decide tiers, so players learn one rule everywhere: "cut the plinth and the house folds;
shoot the infill and it opens". Honest note: a 1.2 m cast removes a whole 2 m frame bay, frame included. The skeleton
stands because the load check keeps the rest up, not because frames resist casts.

**Cover and chokepoints.**
- Lane edges are terrain: cliffs, ramparts, water and bedrock-cored walls. The 3-lane structure survives any amount of
  destruction.
- Inside lanes, buildings and walls are erodible cover.
- Each lane's chokepoint comes in a pair: one **breakable** (a gate, an arcade, a bridge span) and one **durable** (a
  terrain cut, a stair on terrain). Destroying the breakable one changes the route but never closes it.
- Cover mix per lane, target: 40 % weak, 40 % strong, 20 % never *(est.)*.

**Navigation survives.**
- **Spine routes:** terrain, road beds and stairs on terrain, from every spawn to every objective, never through a
  building that can fall. They have a `SpineCore` of bedrock 0.75 m below the surface *(est.)*, so a pit on a spine is
  at most 0.75 m deep and a jump (1.08 m: take-off at 4.6 m/s) gets out.
- **Objectives** sit on ground or on bedrock-cored platforms. Tower tops are positions, never objectives: a "high
  ground" objective is a zone on the highest standing terrain or a bedrock platform, and towers are approaches to it.
  A keel stair is its own bedrock structure, kept apart from destructible towers by C10.
- **Spawn shelters:** bedrock core under 0.5 m of stone, 2 or more exits, facing the landmark.
- **Interiors:** 2 or more exits per room. Stairs sit in the bearing core, so the upper floor is not cut off first.
- **Pad edges** over 1 m get a ramp or stair. Ditches have 1:1 sides.
- **Rubble:** big pieces at rest are `Layer::Dynamic` and carry players, so collapses make ramps while they last.

**Fairness (arenas, W3).** On the walkable 1 m graph of the plan (step 0.35 m, jump 1.08 m, slope 50°), per team:
- arrival time from spawn to each contest point within 10 %;
- high-ground cells within 40 m of each contest point within 15 %;
- cover share by tier.

The check runs on the intact plan and on a worn plan (weak tier and designed releases removed). A failed check re-rolls.
Terrain is not mirrored (see rejected alternatives); fairness comes from site choice, spawn placement and the re-roll.

**Later, not designed here:** telegraphed collapses (the server delays a release a few ticks with dust and creak
events; deterministic because it is a server decision), regrowth (M3) and defenders' conjured reinforcements (M5).

## History and story hooks

**The flag pass (P2, W4).**
- 3-6 local events per region come from a small state machine per recipe. Each event sits in one of the world epochs
  of docs/world/setting.md, section 7 (one epoch list for both documents):
  - the **Building Age**: the old inner wall ring, stone town cores and ground floors;
  - the **Border Centuries**: the outer ring, forts at passes and crossings, gate towers, watch towers between forts;
  - the **Unmaking**: ruins, burned quarters, tower stumps, fallen landmarks;
  - the **Laying** fixes all of it; nothing newer exists except
  - the **Slacks**' inscriptions: the "now" state (a repaired gate, a walled breach, a held or fallen fort).
- Epochs only set flags:
  - an older inner wall ring (partly ruined);
  - a burned quarter (buildings in state `ruin` or `damaged`, the `wood_charred` theme, rubble mounds);
  - "rebuilt" buildings (a stone ground storey under timber upper storeys, which is also readable weak material);
  - a hall turned into a watch post.
- **Reverse causality** (the Caves of Qud technique): the recipe and the arena decide today's state first. For example,
  "the centre fort is held by side A, side B holds the burned village". The pass then picks epochs that explain it.
  The map is always usable, and the story is always coherent.

**What it drives.**
- **The mission:** a held fort facing a burned village gives Siege; a contested ruin gives Hold; a landmark that
  changed hands twice gives Demolish. The mission comes from the "now" state.
- **Spawns:** each side spawns at a site it holds.
- **The region card:** 3-5 lines from templates over the event log, with names from our own syllables, shown in the
  HUD (bitmap font, never ImGui).

**Later.**
- The campaign layer (M7) adds one input epoch, "the last campaign": the same region generates held, contested or
  fallen.
- Sites with unfinished business (a burned granary, an abandoned mine) are PvE hooks.

The **premise, factions and lore** are drafted in docs/world/setting.md for the owner (the Held Lands return to the
shape recorded in each region's Ledger; one slack is one session). The generator only fills slots: site names from
that file's syllable tables, event wording and faction labels.

## Determinism and replication

| Part | Runs on | Rule |
|---|---|---|
| Base terrain | both | Existing, golden-pinned float path. With an empty plan or no hook, the output is bit-identical to today (test `WorldGen.EmptyPlanIsTerrain`). |
| Planner | server, offline | Integer-only so a seed reproduces (`WorldGen.PlanGolden`: plan hash per recipe x 3 seeds, printed like the terrain goldens). **Not a client contract.** |
| Plan bytes | wire | `planHash` in `RegionDesc` (`Welcome`). The client refuses a mismatch or an unknown `planVersion`. Decoded records are validated (inside the window, under the ceiling, known module ids and material ids) before anything is built. |
| Stamper | both | **Integer-exact.** No float decides a voxel. Rotations and mirrors are index permutations; roofs use integer ratios; domes and cylinders use int64 squared tests like `CarveSphereQ`; skirts use the integer blend above; decay heights come from `Hash3` per 1 m column. Fixed order: shapes in record order per column, then prims, modules and roofs by global record index (later wins), then all `Decay` records in record order, last. `ChunkGoldens` pins that order. The result does not depend on chunk order or thread. |
| `ClassifyColumn` / `VerticalBounds` | both | From `PlanIndex`: per chunk column, the [lowest, highest] row any record touches; each record's index box includes everything it writes (a `Pad`'s skirt of k voxels beyond x0..z1, roof overhangs, party-edge air). Those rows are generated; outside them the base classification stands. A column top may be too high, never too low. `surfaceFrom` (the lowest row a sky ray can reach) is lowered to min(base, the lowest row of any `Cut` shape, air slot (255) or opened `Shape` in that column), since those open rows below the base surface. `ViewDistance.ClassifyColumnAgreesWithGenerate` and the `surfaceFrom` agreement test get structured twins. |
| Kits, styles, recipes | shipped with the build | `KitLibraryHash` in `buildHash` |
| Variation ops, lights, objects | server | Existing `RegionTemplate` path. Variation ops make modified chunks, so keep them few. |

**New tests:**
- `WorldGen.ChunkGoldens`: 64 chunks over 3 seeds, chosen on built columns. Folded into a new `kWorldGenGoldenHash`
  in `buildHash`.
- `WorldGen.ChunkOrderIndependent`: generate the same chunks in 3 orders on 1 and 7 threads; hashes equal.
- `WorldGen.NamesNotBlocked`.
- `tools/session-check.sh` on a structured session: hashes equal.

**Contract lines for CLAUDE.md** (added in W1):
- No float may decide a plan record or a stamped voxel, except the golden `TerrainHeight` sample in the planner.
- Plan records, `Shape` / `Roof` / `Prim` / `Marker` kinds, role slots and `planVersion` are wire format: any change
  bumps `planVersion`, and the protocol number when the message changes.
- The stamper is pure and visits records in global index order. Every new record kind gets a golden.
- Pristine structures are never load-checked at runtime; CI proves them. New archetypes need the stand and neck tests.

Windows determinism of the base terrain is still unproven (networking.md). The stamper adds only integer code on top of
it, and the planner adds nothing because it is shipped.

## Budgets

All figures are *(est.)* until W1 and W2 measure them.

| Item | Arena 250 m | Zone 1 km (W5) |
|---|---|---|
| Planner | 0.1-0.4 s, single thread, server only | 2-6 s, once, on the stage-9 on-demand server |
| Plan bytes per join | 30-60 KB; about 0.3-0.5 s at the 128 kB/s stage 7a pace | 300-600 KB in 64 m tiles, streamed per window |
| Chunk-sync bytes for pristine structures | 0 (the template-op alternative: 1.3-3 MiB per village, 8-19 MiB per town) | 0 |
| Variation ops | 20 or fewer small ops, under 50 modified chunks | same per site |
| Lights | 24 or fewer from plan markers, as template lights (50 B each) | per site, near players |
| Replicated objects | 16 or fewer (about 3 KiB each in the baseline) | same |
| Stamping | +3-8 % generation time on built chunks (10-50 µs of module copies against about 1 ms of terrain per chunk) | same |
| Extra dense chunks, client window | **1 200 or fewer per 192 m window, enforced by P12.** 5 300-5 600 today, so 6 500-6 800 under the 7 000 game cap | same |
| Extra dense chunks, server window | +1 500-3 000 over 9 888 | only where loaded |
| Server preload | today 9.6-10.4 s against a 10 s gate; **+0.5-1.5 s**, so over the gate (see open questions) | not under the arena gate |
| Graph nodes | 6 000 or fewer in any 24 m box (limit 8 192) | same |
| Designed release | 45 000 voxels or less per part, 60 000 or less per cast; within 84 voxels (10.5 m) of every neck voxel at a 1.2 m cast; 1-4 parts | same |
| Kit memory | under 2 MiB per code-built kit; 5-15 MB for 5 kits with landmarks | same |
| GPU | unknown. More surface near the ground may shorten gbuffer rays; lights cost per light. Measure at a fixed `--camera` after `World: window filled`, with and without the region. | |
| CI | fast set under 60 s (the 6 m dud sweep is 3-10 s per seed: 3 seeds per recipe fit for 1-2 recipes; beyond that the fast set takes 1 seed per recipe and the rest moves to the nightly run); 200-seed sweep 10-60 s per seed, by hand or nightly | |

## Staged delivery

Each stage ends with the usual checks: all tests, both headless runs exit 0 under validation, a screenshot looked at, a
fresh-eyes review, a ROADMAP update, and commits (engine, then game). Windowed play, the look and the feel are confirmed
by the owner only; an assistant reports headless results and never claims more.

### W1: a stamped village, offline (about 1.5 weeks)

Build:
- `Vortex::WorldGen` with `:plan`, `:kit`, `:stamp`, `:planner`, plus layering asserts.
- The `ISurfaceModifier` hook in the terrain generator.
- Shapes: Pad, Foundation, Cut, CaveKeepOut, TreeKeepOut, Road.
- Prims: Box, Shell, Cylinder, Stair. Roofs: Gable and Hip.
- The **Frame** kit: about 14 code-built modules (ground, upper and top walls; outer and inner corners; door; window;
  braced; jetty; stair; lantern; 2 roof trims), the `highland_timber` style, and existing materials only.
- Planner v0, arena-sized:
  - site (flattest disc near the centre);
  - main street plus 2-4 lanes by A\*;
  - strip plots;
  - 1-3 storeys;
  - mask lookup plus seeded variants;
  - pads and foundations;
  - rating sums and the dense-chunk cap;
  - one segmented stage-tower landmark;
  - spawns at both ends;
  - lights and 4 crates as template data.
- Game flags `--region village --seed N`, `--camera region-landmark` and `region-street`, `--region-blast`.
- `tools/region_check` with `--plan-png`.
- The test-only load-margin hook.

Acceptance:
- These tests pass: `EmptyPlanIsTerrain`, `ChunkGoldens`, `ChunkOrderIndependent`, the structured
  `ClassifyColumn` agreement, `PlanGolden`, `ArchetypesStand` (0 releases at margin 2) and `DesignedNecksRelease`
  (the tower's top stage, within limits).
- `--region village` exits 0 under validation and logs `Region: ...`.
- `--region-blast` logs a release of 45 000 voxels or less.
- The stage tower obeys C4 (stone stages to 19 m at most, wood above, or a taper) and its foundation obeys "Bearing".
- These are measured and written down: planner ms, chunks/s with and without the region, client `store a/b` inside
  the village, GPU at a fixed camera, graph nodes per 24 m box.
- Screenshots from both cameras are looked at.

### W2: in a session (about 1 week)

Build:
- `RegionGenerator::Structured`, `RegionTemplateId::Generated`, the `RegionDesc` fields with `IsValid`.
- `RegionPlanPart`, `planHash`, `KitLibraryHash` and `kWorldGenGoldenHash` in `buildHash`, structure-aware probes, one
  protocol bump.
- Spawns per team.
- Session keys and a `village.session`.
- `tools/session-check.sh` on the structured session.

Acceptance:
- Session-check prints `PASS`: world and entity hashes equal at the same opSeq, and the clients' join logs show plan
  bytes and 0 structure `ChunkData`.
- The server's preload time and dense count are measured and reported against the 10 s gate, with no claim that they
  are fine until measured.
- The drill still frees terrain in the region.

### W3: forts and fights (2-3 weeks)

Build:
- Socket WFC facades and 1-D wall runs.
- The fort recipe: curtain wall, gap-backed towers, gatehouse, a keep in tiers on a `Batter` base, ditch, jigsaw
  compounds.
- Stilt foundations.
- Spine cores, spawn shelters (Marker `seam`), contest points and the Hold objective; the `ledger_yard` archetype
  (Marker `yard`) and C10 checks.
- The scenery ring beyond the play bounds (see "Margin").
- The fairness and reachability checks (intact and worn).
- The node budget enforced.

Acceptance:
- 0 releases over 3 seeds per recipe at margin 2.
- Every keep tier and tower stage releases within limits.
- `region_check` shows both teams' fairness within targets on 3 seeds.
- Every objective has a spine route on the worn plan.
- A session-check variant on the fort recipe passes.

### W4: materials, history, a second kit (about 3 weeks)

Build:
- The material batch (plaster, tile_roof, slate, thatch, rammed_earth, adobe, rubble_core, wood_dark, wood_charred).
- Rubble-core walls that slump.
- The history flag pass, columnar-decay ruins, burned themes, the region card in the HUD, missions from the "now"
  state, name tables and the blocklist test.
- The **Earthcourt** kit with the `red_badlands` style (Earthcourt only until Tierkeep exists): it serves three
  styles and its materials (rammed_earth, adobe) arrive in this batch.
- The `.vox` module pipeline with manifests.

Acceptance:
- The terrain goldens are unchanged by the material batch, or regenerated on purpose with `kGeneratorGoldenHash`
  updated.
- A breach in a rubble-core wall slumps (a test).
- Ruins release 0 under the dud sweep, and `WorldGen.RuinsDoNotFloat` finds 0 floating islands in stamped ruins.
- `WorldGen.NamesNotBlocked` passes.
- The owner has reviewed the style sheet before its modules were built.

### W5: zones (with networking stage 9)

Build:
- Plan tiles; the settlement hierarchy and road tiers over 1 km; the walled-town anchor; the landmark-cadence check.
- `Channel` rivers and bridges, after the owner's water look check.

Acceptance:
- The zone planner runs under 6 s.
- A client walking 1 km receives tiles and never shows a stamped chunk without its tile.
- Cadence: a new landmark or point of interest every 100 m or less on sampled routes.

### W6: reach

- Regrowth by regeneration (a `RestoreBox` `WorldOpKind`, a wire change) for M3.
- LOD level 1 from 2x-downsampled modules (M6) and far landmark checks.
- The campaign `metaState` epoch (M7).
- An editor kit preview and neck report (`vortex.editor`).
- Kits Bayhall, Tierkeep, Logcrib and Towerstone, and with them the styles `loess_terraces`, `white_coast`,
  `ridge_keep` and `fen_causeway` (the last also needs W5's `Channel`).

## Rejected alternatives

- **Structures as template operations** (the playground's way). Every touched chunk becomes modified, is never packed,
  and is sent to each joiner: 1.3-3 MiB per village and 8-19 MiB per town per join *(est.)*. Buried foundations keep
  chunks resident. Nothing shows outside the server window, and regrowth must re-send chunks. Kept only for small
  per-session variation.
- **Planner on both sides from the seed, with a hash and a fallback (H).** It saves 30-60 KB per join, but A\*, plot
  growth, module choice and history would then all be bit-exact contracts on unproven Windows clients, and every
  client would pay the planning time. Shipping the plan is simpler, and the fallback path disappears.
- **Shipping expanded per-member primitives** instead of module placements. About 0.6-1.6 MiB per arena (H's own
  estimate) against 30-60 KB.
- **Grammar rasterised on clients (S).** Smaller plans (5-7 KB), but the whole split grammar becomes client contract.
  Modules plus a few primitives keep the client side to copies and integer shapes.
- **A faction history simulation that drives placement (S).** It makes maps asymmetric and hard to keep fair, and its
  value is unproven until playtests show players read the history. A flag pass with reverse causality gives the same
  visible traces at a fraction of the cost.
- **A load check at plan time on the server (K)** or a start-up neck report (H). It costs 0.1-0.6 s of preload, which
  is already over its gate. CI and `region_check` give the same confidence before shipping.
- **WFC for the top-level layout.** It gives only local consistency: hierarchy, roads and plots need global structure.
  WFC is used only for facades and wall runs.
- **Per-cell roof modules.** They depend on the span and are hard to author. Roofs are parametric, with trim modules.
- **Mirrored or rotated terrain for arenas.** It is a new `TerrainSettings` option (wire and goldens), risks a visible
  fold line, and looks artificial. Revisit only if playtests show organic arenas cannot be balanced by site choice,
  spawns and the re-roll.
- **Client-local lights from the plan (S).** 0 bytes, but they bypass today's entity light path. Revisit when lights
  become the budget problem.
- **New materials in the first slice.** They change `MaterialTableHash` and need tuning; the existing palette is enough
  to prove the pipeline.
- **Other voxel sizes for structured regions.** `lround` resampling of modules breaks load paths and cell alignment.
  Structured regions are 0.125 m only.
- **A 15-voxel "ken" or 20-voxel storeys** (the real proportions). They break 8^3 cell alignment and the
  weakest-material rule for openings. The kit stylises to 2 m and 3 m.

## Open questions (with the answer we recommend)

1. **Is the server preload gate (10 s) hard for structured arenas?** Recommended: measure in W2. If structures push it
   over, allow 12 s for structured arenas until the server resident rule (pack everything not near players or edits)
   is built. Do not cut town density to fit the gate.
2. **Client dense cap above 7 000 inside towns?** Recommended: no. Keep P12's 1 200-per-window cap and keep towns to
   zones until palette-compressed chunks exist. Re-measure the GPU on the UHD 620 before ever raising it.
3. **First style and kit?** Recommended: `highland_timber` with Frame. It uses the existing materials and is closest to
   the plaza. Earthcourt with `red_badlands` comes second (W4), so the second style looks clearly different and the
   kit then serves three styles; Bayhall follows in W6.
4. **Mirrored arena terrain?** Recommended: no, as above. Organic terrain plus the fairness check and re-roll.
5. **Bedrock above ground** (spawn shelters, lane keystones) with its own look? Recommended: yes, as the "never" tier.
   It is one distinct dark facing in every style, used sparingly. Spine cores stay buried.
6. **Who authors modules?** Recommended: code-built kits through W3, then hand-authored `.vox` modules per kit when an
   artist or the owner has time. Both bake to the same form, so nothing waits on art.
7. **Order against shred.md stage 4c?** Recommended: W1-W2 do not need it. Build 4c before keeps and landmarks grow past
   stacked stages (W3's keep is built as stages either way).
8. **The story premise, factions and names?** A draft exists: docs/world/setting.md (the Held Lands, the Ledger, the
   Settle; three factions; six regions mapped to styles above). Recommended: the generator uses its syllable tables
   and epochs from W4, but the HUD shows neutral side labels (side A / B) until the owner approves the factions.
9. **Glass in windows?** Recommended: open openings through W3. They give sightlines for fights and avoid the
   weakest-material trap. Glass panes come in their own cells from W4.
10. **A scenery ring beyond the play bounds?** Clients on a 250 m arena see about 92 m past the bounds. Recommended:
    yes from W3 (scenery-only records, no checks); bare terrain there in W1-W2.
11. **The largest cast radius per recipe?** It sets the designed-release reach and the margin band. Recommended:
    1.2 m (the player's cast) for W1-W3; any larger spell (M5) states its radius in the recipe, and the neck tests run
    at that radius.

## Sources (technique and dimensions only)

- Emilien, Bernhardt, Peytavie, Cani, Galin: *Procedural Generation of Villages on Arbitrary Terrains* (2012):
  interest functions, land conquest, fail-and-retry facades.
- Müller et al. *Procedural Modeling of Buildings* (2006); Wonka et al. *Instant Architecture* (2003): split grammars.
- Gumin, *WaveFunctionCollapse*; Boris the Brave's WFC notes; Stålberg's talks on corner-based modules: local WFC with
  fallbacks.
- Burgess and Purkeypile, modular level design (GDC 2013); jigsaw structure pools (Microsoft Learn): kits, sockets,
  fallback pools.
- Lynch, *The Image of the City*; Nintendo CEDEC 2017 field level design; the Sucker Punch landmark taxonomy (GDC
  2021): landmarks, edges, cadence.
- Gustafsson's Teardown design notes; THE FINALS and Rainbow Six Siege destruction interviews: consistent material
  tiers, supports over damage totals, navigation after collapse.
- Grinblat and Bucklew, *Subverting Historical Cause & Effect* (2017): reverse causality.
- Public architectural history (half-timbering, Chinese bay halls and pagodas, Japanese castle bases, arid courtyard
  houses and kasbahs, Nordic and Slavic log building, Mediterranean tower towns and arcades, fortification types):
  proportions and dimensions, as gathered in the research of 2026-11-17.
- Engine: shred.md Part 4b (load check, closed forms, failure modes), networking.md (join by chunk sync, preload and
  memory figures), view-distance.md and lod.md (window, readability), collapse.md (structures in terrain).
