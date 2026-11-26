# Fortress: Hollowkeep's architecture for siege and route-making

[[../../../00 Docs Map|Docs map]] · [[../README|engine docs index]] · [[../ROADMAP|roadmap]] · [[land|land]] · [[worldgen|worldgen]] · [[mechanisms|mechanisms]] · [[spread|spread]] · [[../../../docs/product/castle-layout-draft|castle layout draft]]

| | |
|---|---|
| Status | `[fort-1]`-`[fort-3]` built: the land generator stamps Hollowkeep's skeleton, openings, interiors, the low route, roofs and crowns from `voxel::PlanFort` ("The skeleton as built", "Openings and interiors as built", "Roofs and crowns as built"). `[fort-4]` and `[fort-5]` designed. |
| Decided | The owner, 2026-11-25: the brief in "The brief" (a detailed medieval fortress as a destructible siege and route-making playground); the answers in the castle draft's section 10 (no trapdoor mechanism, fire spread is in the demo, furniture, medieval construction scaffolding and interactable elements). Us (this design, 2026-11-25): "Decision" items 1-10. |
| Next | The owner walks the stamped castle (owner check 29) and answers "Open questions". Then `[fort-4]`. |
| Open | "Open questions (with the answer we recommend)". |
| History | - |
| Marks | (e) is an estimate. Every size is a design target until a stage measures it. 1 voxel = 0.125 m; "v" after a number is voxels. |

Related: [worldgen.md](worldgen.md) (kit cell, role slots, construction rules C1-C10, designed necks, tiers),
[land.md](land.md) (the site, the ditch, the castle road), [mechanisms.md](mechanisms.md) (doors, portcullis, lever,
held objects, ropes, the drawbridge of `[mech-3]`), [spread.md](spread.md) (fire), [shred.md](shred.md) Part 4b (the
load check), [magic.md](magic.md) (blast and spell numbers), the game's
[castle-layout-draft.md](../../../docs/product/castle-layout-draft.md) (routes R1-R8, necks N1-N6, objects).

## The brief

The owner's brief, 2026-11-25, in short:
1. **No primitive blockouts.** Walls and towers get battered plinths (15-25 degrees, the bottom 2-4 m), buttresses,
   quoins, string courses at each floor, crenellations, machicolations and timber hoardings. Roofs are pitched:
   conical on towers, gabled or hipped on the keep and the gatehouse, on visible trusses, with slate, tile or thatch,
   dormers, chimneys and a bell turret.
2. **Hollow, playable interiors.** Joists and plank floors, vaulted undercrofts, spiral stairs in corner turrets,
   splayed arrow slits.
3. **A crowded bailey.** Half-timbered and wattle-and-daub lean-tos against the curtain: stables, a forge, barracks,
   a kitchen, a granary; repair scaffolding on damaged walls.
4. **3 destruction tiers that read at a glance:** Tier 1 heavy frame (dark granite), Tier 2 breachable masonry
   (limestone, brick, iron-bound oak), Tier 3 fast breach (timber, plaster, roofs, scaffolding).
5. **One brutal chokepoint and 3 bypass loops:** the barbican (drawbridge, portcullis, murder holes, flanking slits);
   a low route (grate, culvert, cellar), a mid route (lean-to rafters and roof to the wall walk), a high route
   (scaffold and hoardings).
6. **Collapse-to-ramp set pieces and loose props:** cut a strut and a walkway, a crane or a gallery falls into a ramp;
   planks, tables, carts, barrels and ladders to bridge, block and climb.
7. **Vignettes, not noise:** moss and damp on plinths, soot over chimneys, a guardroom, a smithy and siege yard, a
   great hall, a cellar.

The owner's answers the same day: no trapdoor mechanism (a magic trap spell later instead); fire spread is in the
demo; add furniture, medieval construction scaffolding and interactable elements. The brief replaces the draft's
2026-11-24 answer "weak infill materials: no": timber infill, plaster and roof materials are Tier 3.

## Decision

1. **The castle becomes stamped generator output** (worldgen.md Decision 1). A **fort plan** of integer records
   (shapes, primitives, roofs, module placements, markers) is made from the baked land map's `LandSite`. A **stamper**
   writes the plan's voxels inside the land generator's `Generate(coord)`, last, after the terrain and nature (so the
   ditch and the walls replace the plants and rocks they cross). So a
   pristine castle costs 0 bytes at the join, packs and evicts like terrain, and its detail does not multiply
   `WorldOp`s (today 2 812 boxes in 1.7 s for the greybox; the detailed castle is est. 50-150 k boxes).
   `sim::HollowkeepTemplate` keeps only what is not voxels: mechanisms, movable objects, lights, the spawn.
2. **Each side makes the plan from its own bake**, as the land map itself (land.md Decision 3). The planner uses
   only +, -, *, /, sqrt, floor and integer hashes. Its golden hash joins `voxel::kLandGoldenHash`, so `buildHash`
   and the join's map hash cover it. Nothing new travels.
3. **The plan lives in `vortex.voxel`** (Core, GPU-free) as a partition `:fort` beside `:land`, because the land
   generator calls the stamper. When `[wg-W1]` makes `Vortex::WorldGen`, the planner and stamper move there and the
   kits are shared; the record format is worldgen.md's `RegionPlan` subset.
4. **One grid: worldgen.md's kit cell** (16 x 24 x 16 v = 2 x 3 x 2 m). Storeys are 3 m; the great hall is 2 storeys.
   Wall runs follow the site's contour polygon (land.md), so curved runs are made of straight segments between the
   24 site directions, each squared locally to x or z in 2 m bays where the segment allows.
5. **Every voxel has a role slot** (worldgen.md "Kits": frame, infill, plinth, roof, trim, opening, metal, emissive,
   air, deco), and a castle style maps slots to materials. The role decides the tier, so the look and the strength
   always agree.
6. **New materials, castle only** (appended ids; `voxel::MaterialTableHash` changes; "Material and structural
   integrity matrix"). Castle masonry is no longer the terrain's `stone`, which also settles the draft's "magic is bad
   at breaking terrain" item: terrain keeps its hardness, castle masonry gets its own.
7. **Tier 1 is very hard, not indestructible.** The basic blast (power 6.0) does not carve granite (hardness 7.5);
   Siege Cannon bores it slowly. Bedrock stays only where worldgen.md allows it: the sanctum's keel floor and spine
   cores (C10).
8. **Every pristine part stands with a 2x margin** (worldgen.md C1-C6), and every set piece is a designed neck with
   worldgen.md's limits (at most 45 000 voxels per part, 60 000 per cast, within 84 v of its cut volume). The load
   check never runs after generation.
9. **Routes are planned, then checked.** The planner places the chokepoint, the 3 bypass loops and the set pieces by
   rules relative to the gate direction, then a reachability check walks each route on the intact plan and on a worn
   plan (Tier 3 removed, every designed release removed): at least 2 routes reach the sanctum on each.
10. **Offline first, session second.** Stages `[fort-1]`-`[fort-5]` are offline; each keeps the plan inside the map
    hash, so the land session can take it in one step after `[fort-2]` (open question 3).

## Architectural grammar and module rules

All sizes in voxels (v) at 0.125 m, then metres. A **run** is one wall segment between two site directions.

### Walls (curtain and inner wall)

| Part | Rule | Size | Slot |
|---|---|---|---|
| Section | Outer face, core, inner face | curtain 20 v (2.5 m): faces 6 v, core 8 v; inner wall 16 v (2 m): faces 5 v, core 6 v | face: frame; core: infill |
| Batter (talus) | The bottom of the outer face leans out 1 v per 3 v of height (18.4 degrees). Height: 16 v (2 m) over the run's highest ground, plus the ground's drop under the wall at each column | 2-4 m on level ground, more where the slope falls | plinth |
| Foundation | Every column reaches 12 v (1.5 m) under its own ground (land.md rule 3); on a slope the batter grows down to it | - | plinth |
| Buttress | On a straight run longer than 10 m: one every 6-8 m, never closer than 3 m to a tower or a gate. 12 v wide; it projects 8 v at the batter's top and steps back 2 v at each string course; its top meets the wall 2 m under the wall walk | 1.5 m wide, 1 m deep | frame |
| Quoins | At every corner of a wall, tower or building: alternate blocks of 6 x 3 v and 3 x 6 v in plan, 3 v high, in the darker tone of the frame material | 0.75 x 0.375 m blocks | deco (frame material, darker id) |
| String course | At each floor level and at the wall walk: a band 2 v high that projects 1 v | 0.25 m band | trim (on a 2 v member: C6) |
| Wall walk | 12 v wide on the curtain (1.5 m), 10 v on the inner wall; the run's highest ground + 70 v (8.8 m), stepping down the slope run by run (today's rule) | - | frame |
| Parapet | Outer side: 10 v high, 4 v thick; merlons 16 v wide and 6 v higher, embrasures 6 v wide; every second merlon has an arrow slit | 1.2 m, merlon 2 m, gap 0.75 m | frame |
| Machicolation | On the gatehouse, the keep and the 2 towers next to the gate: corbels of 3 stepped stones (each 2 v out, 3 v high) every 12 v; the parapet sits 6 v out on them; the 6 v gaps between corbels are drop holes | 0.75 m overhang | frame (corbels), trim |
| Hoarding | A timber gallery on the curtain near the gate and on the towers of the high route: putlog beams 2 x 2 v through putlog holes every 16 v, 12 v out; a plank floor with a drop slot every 2 m; plank walls 14 v; a pent roof of shingles | 1.5 m out, 1.75 m high | frame (wood), infill (planks), roof |
| Repair patch | Where history marks damage (one run per flank): brick infill inside a ragged outline, with the scaffold outside | 4-6 m wide | infill (brick) |

Rule: a run never ends as a flat cut. It ends in a tower, a gatehouse, a buttress or a corner with quoins.

### Towers

| Part | Rule | Size |
|---|---|---|
| Plan | Flank towers on the curtain: D-shaped (a half-octagon outward, square inward) at every third site direction, none within 1 direction of the gate. The keep: square with 4 corner turrets. Gatehouse towers: 2 round-fronted towers | D-towers 7 x 7 m; keep 16 x 16 m; gate towers 6 m across |
| Batter | As the wall, 1 v per 3 v, 24 v (3 m) over the highest ground; an octagon flares as an octagon | - |
| Shell | Walls 12 v (1.5 m) for flank towers, 24 v (3 m) for the keep's lower half and 16 v above | - |
| Floors | Every 24 v (3 m): joists 2 x 3 v every 8 v (1 m) across the short span, planks 1 v on top (2 v total floor) | - |
| Back | Flank towers are open at the back above the wall walk (the draft: open-backed), closed below with a door to the bailey | - |
| Crown | Parapet and merlons, machicolations on the 2 towers next to the gate, a conical or pyramidal roof on flank towers (see Roofs) | - |
| String courses | At every floor | - |
| Spiral stair | In one corner turret (keep) or in the shell's thickness (flank towers): a 16 v (2 m) shaft, a 2 x 2 v newel, steps 2 v rise, each 30 degrees on (12 per turn, 3 m per turn), each step a wedge 7 v long | 2 m shaft |

### Openings

| Opening | Rule | Size |
|---|---|---|
| Arrow slit | Outside 1 v wide, 12 v high; it splays through the wall to 8 v wide inside, with a 6 v sill recess (an air wedge, cut after the walls) | 0.125 x 1.5 m outside, 1 m inside |
| Cross slit | On gate towers: an arrow slit plus a horizontal slit 6 v across at its middle | - |
| Door | 8 x 18 v under a 2 v lintel of frame material; a door mechanism (`[mech-1]`) where the draft names one | 1 x 2.25 m |
| Window | Upper keep floors only: 6 x 10 v, round-headed (1 v steps), with shutters (planks, Tier 3) | - |
| Murder hole | In the gate passage's vault: 4 x 4 v holes every 16 v, open to the upper room | - |
| Putlog hole | 2 x 2 v in the curtain's outer face every 16 v at hoarding and scaffold levels | - |
| Drain mouth | The culvert's outer end: 8 x 8 v, an iron grate of 1 v bars every 2 v in a 2 v granite frame | 1 x 1 m |

Openings occupy whole support cells with a lintel cell above (worldgen.md C2), so an opening never cuts a bearing
member below 2 v.

### Vaults and the undercroft

- **Barrel vault:** a half circle over the span, built of rings 1 v deep; the ring is 4 v thick; each ring row is the
  voxel approximation of the circle (integer test x^2 + y^2 against r^2 on voxel centres). Tier 1 granite ribs every
  16 v, Tier 2 ashlar between them.
- **Undercrofts:** under the keep (the sanctum's level stays the ground floor with its keel; the undercroft is beside
  it, under the forebuilding), under the kitchen and under the stables. 2 storeys of rock are not dug: an undercroft is
  built only where the ground falls, so its floor is the natural ground and its vault carries the floor above (land.md
  rule 3: foundations go down).
- **Gate passage:** a barrel vault with murder holes.

### Roofs

| Building | Roof | Pitch | Shell (C5) | Cover |
|---|---|---|---|---|
| Flank tower | Conical over a round crown, pyramidal over a square or D crown | 2:1 | 3 v | slate |
| Keep | Hipped, behind the parapet; 4 corner turret cones | 1:1 | 2 v | slate |
| Gatehouse | Gabled across the passage; a bell turret on the ridge | 1:1 | 2 v | tile |
| Lean-tos | Mono-pitch from the curtain's inner face down to the front | 1:2 | 2 v | thatch (stables, granary), tile (forge, kitchen), shingles (workshop, barracks) |
| Hoarding | Pent | 1:1 | 2 v | shingles (planks) |

Trusses: a principal rafter pair (2 x 3 v wood) every 16 v with a tie beam and a king post on the keep and the
gatehouse; common rafters 2 x 2 v every 8 v under the cover. Rafters are frame, the cover is roof. A dormer (a small
gable 12 v wide) every 4 m on the keep roof. Chimneys: 8 x 8 v stone stacks, 12 v over the ridge, on the keep (great
hall fireplace), the kitchen and the forge.

### Lean-to buildings (the bailey)

| Part | Rule |
|---|---|
| Placement | Against the curtain's inner face, in the sectors gate +-3 to gate +-9, never in the 8 m lane from the gate to the inner gate, never in front of a tower door. Depth 4-6 m (2-3 bays), length 2-5 bays of 2 m |
| Frame | Posts 2 x 2 v at every bay, a sill beam 2 v on a 4 v stone plinth, a wall plate at each storey, a brace in the end bays |
| Infill | Wattle-and-daub panels 2 v thick between the frame (Tier 3) |
| Storeys | 1 (stables, forge, kitchen, granary) or 2 (barracks, workshop); the upper floor a plank floor on joists |
| Roof | Mono-pitch 1:2 from the curtain (its top 8-12 v under the wall walk) to the front eave |
| The mid-route workshop | 2 storeys; its rafters are climbable (1 v gaps between rafter pairs are not walkable, the tie beams are); its roof ridge sits 8 v (1 m) under the wall walk, 1 bay from a stair-less stretch of curtain |
| Granary | On 4 staddle stones (stone mushrooms 6 v high), a plank floor, thatch |

Kinds and their room kits: "Vignettes and room kits".

### Construction scaffolding, walkways and cranes (the owner's "medieval construction scaffolding")

The castle is still being built and repaired, so construction scaffolding is a recurring kit, not a single prop. Each
scaffold is a climbable route and a designed neck (N2).

| Part | Rule |
|---|---|
| Scaffold kit | Medieval putlog scaffolding: standards (poles 2 x 2 v) every 16 v along the face and 12 v out, lashed ledgers every 24 v up, putlogs from the ledgers into putlog holes in the wall, plank decks 8 v wide at each lift, ladders between lifts on alternate sides, diagonal braces on every third bay. Tier 3 wood and planks |
| Where (3-4 per castle) | 1. The outer face of the curtain at the repair patch (the high route H1, up to the hoarding). 2. An unfinished flank tower: its top storey stands open to the sky with the scaffold around it and a hoist (a material lift). 3. The keep's upper stage under repair, on the bailey side (a climb to the keep's roof walk). 4. Optional: inside the bailey along a damaged stretch of the inner wall |
| Materials at the foot | Stacks of planks and beams, a lime-mortar tub, a pile of cut stone blocks (ashlar), a hand cart: the movable props and stamped clutter of the work site |
| Walkway | Timber bridges 12 v wide between a flank tower's upper door and the inner wall top, and between 2 lean-to upper floors: a plank deck on 2 beams, a rail, carried on 2 struts from the ground (the set piece) |
| Crane | In the siege yard: a treadwheel crane, mast 2 x 2 bays at its foot, jib 8 m toward the inner wall at the wall walk's height; its foot is the neck |

### Proportion checks the planner enforces

- No wall face longer than 8 m without a buttress, a tower, a string course and a crenellated top.
- No building without a plinth, a pitched roof (or a parapet over a roof) and quoins at its stone corners.
- No tower without a batter, floors and a crown.
- Every bearing member is at least 2 v thick; 1 v detail (quoins, string courses, slits' edges) is trim or deco on a
  member (worldgen.md C6).
- Stone free columns at most 19 m, cantilevers within C4; the keep's shaft over 19 m tapers (worldgen.md C4).

## Material and structural integrity matrix

New ids are appended to the world palette. Hardness is the carve threshold (shred.md: a voxel carves when the cast's
score reaches its `hardnessQ`); the basic blast is power 6.0 (magic.md item 8). Strengths are compression / shear /
tension in kg/m^2 for the load check. Values marked (e) are set in `[fort-1]` and `[fort-3]` by measurement.

| Material (id) | Tier | Look | Hardness | Density kg/m^3 | C / S / T | Burns |
|---|---|---|---|---|---|---|
| granite (new) | 1 | dark blue-grey, coarse | 7.5 | 2700 | 200 000 / 80 000 / 25 000 (e) | no |
| granite_quoin (new) | 1 | granite, darker | 7.5 | 2700 | as granite | no |
| ashlar (new, limestone) | 2 | warm light grey, regular | 3.0 | 2400 | 100 000 / 40 000 / 15 000 | no |
| rubble_core (new) | 2 | brown-grey, only seen in a breach | 2.0 | 2000 | 40 000 / 6 000 / 1 500 (slumps, draft N6) | no |
| brick (exists) | 2 | red | 2.5 | 1900 | 80 000 / 30 000 / 8 000 | no |
| oak_iron (new) | 2 | dark oak with iron band voxels | 6.5 | 900 | 100 000 / 40 000 / 40 000 (e) | slow |
| iron (exists) | 2 | grates, portcullis, bands | 8.0 | 7800 | - | no |
| wood (exists, timber) | 3 | brown | 1.5 | 700 | 80 000 / 30 000 / 30 000 | yes |
| planks (exists) | 3 | light brown | 1.2 | 600 | 50 000 / 25 000 / 25 000 | yes |
| wattle_daub (new) | 3 | cream plaster | 0.6 | 900 | 10 000 / 4 000 / 3 000 (e) | wattle: yes |
| thatch (new) | 3 | straw | 0.3 | 250 | 2 000 / 1 000 / 1 000 (e) | fast |
| slate (new) | 3 | blue-grey | 0.8 | 600 (effective) | 30 000 / 10 000 / 8 000 (e) | no |
| roof_tile (new) | 3 | terracotta | 0.8 | 600 (effective) | 25 000 / 8 000 / 6 000 (e) | no |
| soot (new, deco) | - | black | as its base | - | as its base | - |
| bedrock (exists, keel) | never | dark | indestructible | - | anchor | no |

Roof materials use an effective density (a 0.125 m layer of solid slate would weigh 10 times a real roof; worldgen.md
C5). Soot and the darker quoin are deco ids with the same strength as their base (worldgen.md C2's rule).

| Part | Tier | Material | Collapse behaviour |
|---|---|---|---|
| Batter, foundation, tower corner piers, vault ribs, the keep's lower shell, buttresses | 1 | granite | stands under blasts; Siege Cannon bores it slowly; carries everything above |
| Curtain faces above the batter, inner wall faces, partition walls, the keep's upper shell, gate tower shells | 2 | ashlar | 2 cannon shots open a face; the rubble core behind slumps outward into a ramp (N6) |
| Wall cores | 2 | rubble_core | slumps when a face is cut |
| Repair patches, chimneys, the forge hearth | 2 | brick | opens in 2-3 cannon shots (the draft's R2) |
| Gate leaves, the inner gate, the postern, cellar bulkheads | 2 | oak_iron | resists the basic blast; cannon or fire |
| Portcullis, grates, anvil, chains | 2 | iron | bore the frame, not the bars |
| Joists, posts, rafters, putlogs, struts, crane, standards | 3 | wood | 1-2 blasts; a cut strut releases what it carries |
| Floors, decks, hoardings, shutters, stairs in wood, doors | 3 | planks | 1 blast; drop between storeys |
| Lean-to infill | 3 | wattle_daub | 1 blast opens a bay |
| Roof covers | 3 | slate, roof_tile, thatch, planks | 1 blast opens a roof; thatch burns fast |
| The sanctum's floor, spine cores | never | bedrock | never falls (worldgen.md C10 keeps destructible parts apart) |

## Siege and route-building blueprint

Directions are relative to the gate: **front** is the gate's sector, **flanks** are gate +-6 directions (90 degrees),
**rear** is gate + 12. On the first map the gate faces west to the ridge.

```
                         rear (spur tip, steepest fall)
               T --------- curtain ---------- T
               |   [undercroft]   KEEP        |      L1 culvert mouth (rear or downhill flank,
   flank       |  kitchen/stables [hall]      |         2-3 m over the outside ground)
   (downhill)  |       \  cellar  /  BT       |
       L1 ===> grate --> culvert              |      BT bell tower (N1): falls onto the inner wall
               |      inner wall + inner gate |
               T   lean-tos     |   lean-tos  T  <== H1 scaffold on the outer face (repair patch),
               |  workshop(M1)  |  forge yard |       up to the hoarding of the nearest tower
   flank       |  stables  barracks  crane    |
               T --- hoarding ---[GATEHOUSE]--T      M1 workshop roof -> wall walk (inside)
                            ditch | bridge
                                  | castle road         front (neck, the approach)
```

### The chokepoint: the barbican

| Element | Rule |
|---|---|
| Ditch | land.md's trench (4-10 m out, 5 m deep, 1:1 sides) across the front |
| Drawbridge | `[mech-3]`; until then the fixed bridge of land.md |
| Gatehouse | 2 round-fronted gate towers (Tier 1 batter and piers, Tier 2 shells), a vaulted passage 4 m wide with murder holes from the upper room, cross slits flanking the bridge, the portcullis (`[mech-1]`) and its winch room above, oak_iron leaves behind it |
| Guardroom | The upper room: the winch (the lever today), weapon racks, braziers, arrow barrels ("Vignettes") |
| Defender's view | The gate towers' slits and the gatehouse machicolations cover the bridge and the 10 m before it |

### The 3 bypass loops (each with a way to steal it and a way to cut it)

| Loop | Route | Maker | Thief | Cutter |
|---|---|---|---|---|
| Low (L1) | A culvert from the undercroft through the curtain at the lowest outside ground (a downhill flank or the rear), its mouth 2-3 m over the ground, closed by an iron grate in a granite frame; inside a 1 m crawl up to the undercroft; a weakened oak_iron bulkhead or a bricked-up alcove into the cellar; a stair up into the bailey | Barricade Wave or a ladder to the mouth; 2 cannon shots into the frame or a burned bulkhead | follows the crawl | drops the cellar's plank floor into the crawl; blocks the bulkhead with a cask |
| Mid (M1) | Breach the workshop's wattle infill from the bailey (or climb in by its door), climb its tie beams, break its roof (1 blast), step 1 m up onto the wall walk | 1-2 blasts and a jump | follows | burns the thatch or blasts the tie beams: the floor below stays |
| High (H1) | Outside: climb the scaffold at the repair patch to its top lift, enter the hoarding gallery through its open end, walk around the tower above the defenders' line of sight, drop onto the wall walk | climbing only | climbs behind | cuts a scaffold leg (N2) or a hoarding putlog (the gallery drops) |

Every loop ends in the outer bailey or on the curtain's wall walk, so crews meet at the draft's 2 choke areas (the
inner gate and its bell-tower bypass, then the keep). The draft's R1-R8 stay: L1 is R6, H1 extends R3, M1 is new (R9).

### Collapse-to-ramp set pieces

A released part falls as one rigid body (shred.md); the engine has no hinge. A ramp comes from the shape: one end of
the part stays on a ledge or a wall top, the other end loses its support, so the part tips and lies across the gap.
Each set piece is a designed neck checked by a headless script in the style of `--neck-blast`.

| Set piece | Neck (cut volume) | Falls into |
|---|---|---|
| Bell tower (N1) | its south-west or north-east pier at 4 m | a bridge onto the inner wall top, or into the bailey |
| Tower-to-inner-wall walkway | its 2 struts (wood) | a ramp from the inner wall top down to the bailey |
| Crane jib | the crane's mast foot (wood) | a bridge across the inner wall's top from the siege yard |
| Hoarding gallery | its putlogs (wood) | a pile at the wall's foot, and the gallery's open end becomes a gap in the high route |
| Scaffold (N2) | any of its outer standards | a slope of planks against the wall |
| Gatehouse upper floor (N3) | its 2 tie beams | blocks the gate passage |

Each released part obeys worldgen.md's limits (45 000 voxels per part, 60 000 per cast, within 84 v of its cut
volume), and its centre of mass is placed outside its remaining support so it tips the planned way.

### Movable props

Server-side bodies under the 40-object cap of the draft (64 awake, 256 live): planks 10, beams 6, ladders 4, crates 8,
barrels 6, a cart 2, a table 2, a hay wagon 1 (39 objects). Heavy tables and carts block doors (a closing door crushes
a crate: mechanisms.md); planks and beams bridge gaps; ladders and crates make climbs.

## Vignettes and room kits

A vignette is a placed cluster with a function, never noise. Each kit is stamped voxels (destructible furniture) plus
a few props from the list above and lights (torch voxels in their own cell, worldgen.md C8).

| Kit | Where | Stamped | Props, lights |
|---|---|---|---|
| Guardroom | gatehouse upper room | winch drum, weapon racks, 2 braziers, arrow barrels, a bench | a lever (the portcullis), 2 torches |
| Smithy and siege yard | a forge lean-to and the yard in front | brick hearth and chimney (soot above), anvil (iron), quench tub, timber stacks, a pulley, the crane | planks, beams, a cart |
| Stables | a thatched lean-to | stalls (plank partitions), hay (thatch voxels), troughs | the hay wagon |
| Barracks | a 2-storey lean-to | bunks, a table, chests | crates |
| Kitchen | a lean-to over an undercroft | brick oven and chimney, a long table, casks | barrels |
| Great hall | the keep's upper storeys (2 storeys) | long oak tables, benches, a stone fireplace, hanging cloths (planks in colour), hammer-beam trusses, a gallery balcony | 2 tables |
| Undercroft | under the kitchen and the forebuilding | casks, grain sacks, stone pillars, a bricked-up alcove (the low route) | barrels |

Weathering by rule (deco voxels, no decals): moss and damp ids on the batter's top 3 v on the sides facing away from
the sun's direction and within 2 m of the ground; soot ids above chimneys and braziers; brick repair patches in ragged
outlines behind the scaffold.

## Ordered generation pipeline

The planner runs once per bake on each side; the stamper runs per chunk. Every pass is integer records in a fixed
order, so the stamp order is the record order (worldgen.md P13).

| Pass | Planner | Stamper |
|---|---|---|
| 0 Site | the `LandSite` (land.md): keep point, contour radii, gate, pads, the ditch, the bridge | - |
| 1 Skeleton and foundations | wall runs on the curtain and inner polygons; towers at their directions; the keep and the gatehouse on their pads; each footprint's foundation depth from the ground under it; batters | foundations and batters (plinth), Tier 1 piers and corners |
| 2 Masonry and openings | faces and cores per run; buttresses, quoins, string courses, parapets and merlons; openings (slits, doors, windows, putlog holes, murder holes, drain mouth); vaults and undercrofts; spiral stairs | faces, cores, trim, then openings as air (cut after the walls), then vaults |
| 3 Timber floors, roofs and hoardings | joists and floors per storey; roofs (records per footprint, C5 shells), trusses, dormers, chimneys, the bell turret; machicolations and hoardings | floors, roofs, crowns |
| 4 Bailey | lean-to placement (the rules above), their frames, infill and roofs; the construction scaffolds (3-4 sites); walkways; the crane | the bailey's buildings |
| 5 Props and vignettes | room kits by building kind; weathering; the movable props, lights and mechanisms as template entities | stamped furniture, deco |
| 6 Checks (planner only) | rating sums per bearing column (2x margin); route reachability on the intact and the worn plan; the designed necks' sizes; the dense-chunk estimate | - |

## The skeleton as built

`[fort-1]`, 2026-11-25. `voxel::PlanFort(map, land settings)` (`engine/src/voxel/internal/Fort.cpp`) turns the bake's
`LandSite` into `FortRecord`s in stamp order; `voxel::StampFort` writes them into a chunk from the record and its
column's ground only. The land generator plans once (`LandGeneratorSettings::fort`, on by default; the game's
`--no-hollowkeep` clears it), raises each tile's column classes over the records that cross it (`voxel::FortRows`)
and stamps last. `sim::HollowkeepTemplate` keeps the portcullis, its lever and the spawn.

| Record | What it stamps |
|---|---|
| Trench | the ditch: air from its level floor (1:1 sides) to 14 m over the ground, 4-10 m outside the curtain in the 4 sectors at the gate |
| Box | the bridge, its cutting and trestles, the gatehouse (a stepped granite batter, a granite base, a string course, ashlar above, a parapet with merlons, the passage, the upper room, the portcullis slot), the lever's post |
| Wall | one per run of the curtain (2.5 m) and the inner wall (2 m): granite foundation and batter (1 in 3, to 2.5 m over the column's ground), ashlar faces 0.75 m, a rubble core, ashlar paving under the walk, granite string courses at the batter's top and the walk, buttresses every 6-8 m on runs over 10 m (1 m deep, stepping back 0.25 m every 2 m), a parapet with 2 m merlons. The walk (`voxel::FortWalk`) is continuous: each run is at least its own highest ground plus the walk height; where 2 runs meet, both take one junction height, raised until no ramp climbs more than 1 in 1 (walls only grow); a run is level for 4.5 m beside a tower and 1.5 m at its other ends (where a neighbour's end overlaps), and ramps between |
| Block | the D-shaped flank towers (7 m, every third direction), the 2 round-fronted gate towers (6 m) and the keep (16 m, 22 m high): granite batter to 3 m, an ashlar shell (the keep: granite to 8 m, 3 m thick, then 2 m), granite quoins at square corners, string courses at every floor, plank floors (towers every 3 m, the keep at 5, 11 and 17 m), the keep's bedrock keel, a door, a flat roof with a parapet and merlons; the keep's 4 corner turrets rise 2.5 m |

Measured on the first map (`vortex_land --chunk-cost`, dev build, one thread): 117 records; the plan with the
generator in 2 ms; chunks within 64 m of the keep cost 70 us against 67 us without the castle, and the castle adds
780 chunks the voxel loop runs for (9 062 against 8 282). Tests: `Fort.*`, `Hollowkeep.*`.

## Openings and interiors as built

`[fort-2]`, 2026-11-26, in the same records (`engine/src/voxel/internal/Fort.cpp`):
- **Slits:** every storey of a flank tower has 3 arrow slits (front and the 2 diagonal faces), 1.5 m high from 0.75 m
  over the floor, 0.14 m wide at the outer face and splaying to 1 m at the inner face; the gate towers' slits are
  cross slits; the keep has slits on 3 faces below 8 m (and the front above the ground storey) and 2 round-headed
  windows (0.75 x 1.25 m) on each face above; every second merlon of the walls has a one-voxel slit.
- **Doors:** a flank tower opens onto both wall walks beside it, each door on its walk's centre line where it meets
  the tower's side (`FortRecord::walkLeftU`, `walkRightU`) and at the walk's height there (`walkLeft`, `walkRight`),
  cut through the shell and its string course; inside, a plank landing on that half of the tower at the walk's level,
  and no storey floor or joist within 2.5 m over a walk; each tower stands at least 6 m over its walks. The bailey
  doors of the towers and the keep's door cut through the batter, which is a skirt 3 m over the lower of the column's
  ground and the block's floor (so high ground beside a tower does not raise it to a walk).
- **Floors:** planks 0.25 m thick on wood joists (0.25 m, every metre).
- **Spiral stairs:** in the middle of every tower and in the keep's back corner, from the ground floor through the
  roof: a 1 m shaft, a 0.2 m ashlar newel, steps of 0.25 m rise per 30 degrees (3 m a turn), each 0.5 m thick so the
  steps overlap and carry each other; floors and the roof are open over the shaft (the sector test uses a literal
  table of 12 directions, no libm). The gate towers' shell is 1 m, so their floors leave 1 m around the shaft.
- **Putlog holes:** 0.25 m square every 2 m, just under the curtain's walk (moved there by `[fort-3]`, where the
  hoardings' beams pass).
- **The gate passage:** walls to 2.5 m and a barrel vault (`FortShape::Vault`) to 4.5 m, a straight groove where
  the portcullis runs (4.5 m wide, 4.5 m high), 3 murder holes from the upper room through the crown.
- **The low route L1** (`FortShape::Tunnel`, lined in ashlar, with end walls and a footing down to lower ground):
  on a curtain run with no tower at either end, at least 5 directions from the gate, preferring 9 m or more to the
  inner wall and the most ground over the room. An iron grate (bars every 0.25 m) in a granite frame, 2 m over the
  outside ground and 3.25 m out from the wall's middle (beyond the batter; no buttress within 3 m); a 1 x 1 m crawl
  through the wall to an oak_iron bulkhead and a doorway into a 4 x 3 m room 2.5 m high, whose floor leaves 1 m of
  ground over its roof where the bailey stands high enough (within 0.5 m under and 2 m over the grate); a spiral
  stair (`FortShape::Spiral`) in the room's middle up to the bailey's ground, or on a low site to the room's roof,
  a drop of at most 2.75 m (`FortPlan::culvert`, `culvertMouth`, `culvertExit`, `culvertExitHeight`). The basic blast
  leaves the grate; a power-9 shot opens it.

Measured on the first map: 125 records, planned in 3 ms; chunks within 64 m of the keep cost 100 us against 90 us
without the castle (+11 %; the machine ran slow that day: the built-in terrain measured 273 us against 210 us
before). What depends on a column only (the stair's sector, the openings' geometry) is worked out once per column.
The review of this stage found the walk's steps and the doors' heights; `Fort.EveryFlankTowerOpensOntoBothWallWalks`
and the walk test's continuity and ramp checks pin them. Not yet: roofs, hoardings,
machicolations, the bailey (the later stages); undercrofts under the keep's forebuilding and the kitchen wait for
their buildings (`[fort-4]`).

## Roofs and crowns as built

`[fort-3]`, 2026-11-26 (`FortShape::Roof`, `FortRoof`, `FortRecord::trim`):
- **Roofs:** a roof rises `height` per metre in from its eaves at `half`, over the footprint of a block (the D shape
  of a flank tower, a square for the keep) or as a gable along a ridge. The cover is a shell of (rise per voxel of
  run + 1) voxels (worldgen.md C5: 3 at 2 in 1, 2 at 1 in 1); under it rafters 0.25 m deep (12 radial ones in a cone,
  one every metre on a gable, and the ridge beam), tie beams at the eaves (2 crossing ones in a cone, one every 2 m on
  a gable), king posts. Flank towers: a slate cone of 2 in 1 over the battlements, its eaves 0.25 m out. Gate
  towers: a slate cone over a machicolated crown. The keep: a slate pyramid of 1 in 1 (5.25 m from the middle, a
  2.75 m walk inside the parapet), a slate cone on each corner turret, the great hall's brick chimney (1 x 1 m,
  1.25 m over the apex). The gatehouse: a tiled gable across the passage (1 in 1; a 1 m walk all round), stone gable
  ends, a bell turret on the ridge (4 posts, a plank floor, an iron bell, a tiled pyramid).
- **Machicolations** (`FortTrim_Machicolations`) on the gatehouse, the gate towers and the keep: corbels of 3
  stepped stones (0.25, 0.5, 0.75 m out, 1.125 m in all) 0.75 m wide every 1.5 m, the parapet 0.25-0.75 m out on
  them; between the corbels the gallery behind the parapet has drop holes down the face.
- **Hoardings** (`FortTrim_Hoarding`) on the 4 curtain runs by the gate: putlog beams through the holes every 2 m,
  a plank floor 1.5 m out level with the walk (a drop slot by the face every 2 m), an outer plank wall 1.75 m high
  with slits, posts on the parapet and a pent roof of planks falling 0.6 per metre outward; a doorway through the
  parapet every 4 m.
- **Materials:** slate and roof_tile (hardness 0.8, effective density 600), thatch (0.3, 250, flammable; for
  `[fort-4]`'s lean-tos).

The basic blast opens a roof (more than 100 voxels) and releases nothing
(`Hollowkeep.TheBasicBlastBreachesTheCastleWithoutReleasingIt`). Not yet: dormers (they wait for the keep's upper
storeys, `[fort-5]`), the GPU cost at fixed cameras on the UHD 620 (the owner's check).

## Budgets

| Item | Budget | How it is measured |
|---|---|---|
| Plan time | under 200 ms on the UHD 620 laptop (e) | a `Fort:` log line |
| Stamp cost | under 2x the land chunk's 60 us in chunks the castle crosses (e) | `--chunk-cost` of `vortex_land` |
| Dense chunks | the castle adds 1 000-1 500 dense chunks (the draft's estimate); the game stays under 7 000 slots | `store a/b` inside the keep |
| Graph nodes | under worldgen.md's node budget per 24 m box | the cast counters |
| Released voxels | worldgen.md's limits per designed neck | the set piece scripts |
| GPU | the frame inside the keep and from the approach, at a fixed camera | G21-style baseline |

## Tests and checks per stage

- The plan's golden hash (the same site gives the same records), in `kLandGoldenHash`.
- `ClassifyColumn` agreement: column classes still bound the stamped castle.
- 0 releases under dud casts at 2x load over the whole castle (worldgen.md "Three layers of proof").
- Each designed neck releases its planned part within the limits, and the part tips the planned way.
- Reachability: at least 2 routes to the sanctum on the intact and on the worn plan.
- Screenshots from fixed cameras (the approach, the bailey, the great hall, the scaffold) looked at; windowed play is
  the owner's.

## Stages

| ID | Stage | State | Commits (engine / game) | History |
|---|---|---|---|---|
| `[fort-1]` | Stamped skeleton: the fort plan and stamper in `vortex.voxel:fort`; the greybox moves out of `WorldOp`s; batters, faces and rubble cores, buttresses, quoins, string courses, parapets with merlons; D-shaped flank towers; materials granite, granite_quoin, ashlar, rubble_core. Acceptance: the plan's golden hash (done: `Fort.GoldenHash`); column classes agree (done: `Fort.ColumnClassesBoundTheStampedCastle`, `Fort.EveryRecordsBoundsHoldAllItWrites`); 0 releases under the basic blast (done: `Hollowkeep.TheBasicBlastBreachesTheCastleWithoutReleasingIt`; the 2x-load dud sweep waits for worldgen.md's test hook); stamp cost measured (done: "The skeleton as built"); shots | Built | - | - |
| `[fort-2]` | Openings and interiors: arrow slits, doors, windows, murder holes, putlog holes; joists and floors; spiral stairs; vaults and undercrofts; the culvert and grate (L1); oak_iron. Acceptance: the stairs climb every storey with headroom (done: `Fort.TowersHaveSplayedSlitsAndASpiralStairToTheRoof`; a bot walk waits for the bot's path code); L1 leads from its grate to the bailey and a siege shot opens it (done: `Fort.TheCulvertLeadsFromItsGrateToTheBailey`, `Hollowkeep.ASiegeShotOpensTheCulvertGrate`); the vaulted passage (done: `Fort.TheGatePassageIsVaultedWithMurderHoles`); every tower opens onto both walks (done: `Fort.EveryFlankTowerOpensOntoBothWallWalks`); 0 releases (done: the blast tests) | Built | - | - |
| `[fort-3]` | Roofs and crowns: conical, pyramidal, hipped and gabled roofs on trusses; dormers, chimneys, the bell turret; machicolations and hoardings; slate, roof_tile, thatch. Acceptance: C5 shells hold (done: `Fort.RoofShellsAreThickEnoughForTheirPitch`); roofs on trusses, machicolations, hoardings, the gatehouse's roof and bell (done: `Fort.TowersCarryRoofsOnTrussesOverAnOpenTopFloor`, `Fort.MachicolationsHaveDropHolesBetweenTheirCorbels`, `Fort.HoardingsHangOnTheCurtainByTheGate`, `Fort.TheGatehouseHasAGableRoofAndABell`); 0 releases (done); the GPU cost at fixed cameras (the owner, check 29); dormers wait for `[fort-5]` | Built | - | - |
| `[fort-4]` | The bailey: lean-tos (stables, forge, barracks, kitchen, granary, workshop), wattle_daub; the construction scaffolding kit at its 3-4 sites, walkways, crane; the set pieces as designed necks; M1 and H1. Acceptance: each set piece releases within limits and tips the planned way (scripts); reachability on the worn plan | Designed | - | - |
| `[fort-5]` | Props and vignettes: room kits, movable props, lights, weathering, soot; fire-ready materials with spread.md. Acceptance: the object count under the cap; the guardroom's lever drives the portcullis; owner check of the look | Designed | - | - |

The order relative to the ROADMAP (`[wg-W1]`, `[mech-3]`, `[spread-S2]`, the demo work) is the owner's call.
`[fort-1]` needs nothing new from other designs.

## Rejected alternatives

- **More `WorldOp` boxes** (today's greybox method): est. 50-150 k boxes, seconds of apply time on every start, and
  each pristine voxel would be terrain the chunk sync sends to a joiner whose generator did not make it.
- **A hand-made MagicaVoxel castle:** it does not fit a site found on the map (the site's contour decides the walls),
  and the owner asked for a castle built on the natural slope.
- **An indestructible Tier 1:** players cannot read where "never" starts and the frame would hold the castle up
  against any plan; a very hard but carvable granite keeps the castle readable and still lets a long siege through.
- **A hinge joint for ramps:** the engine releases rigid parts, it does not simulate joints; a tipping shape gives
  the ramp without a new physics feature.

## Open questions (with the answer we recommend)

1. **Tower shape:** D-shaped flank towers and round-fronted gate towers, or keep them square? Recommended: D and
   round, as above; they break the box silhouette most.
2. **Fire materials:** thatch, wattle and shingles need burn stages in spread.md (it has wood and planks). Recommended:
   thatch burns as foliage does (fast), wattle and shingles as planks, until `[spread-S5]` tunes them.
3. **When the castle goes into the land session:** recommended after `[fort-2]` (the walls, interiors and the low
   route are what a match needs; roofs and props follow in place).
4. **The keep's height:** the draft's 22 m keep over 19 m of stone needs a taper or timber top stages (worldgen.md
   C4). Recommended: a taper of 1 v per storey on the outer face, and the roof in timber.
5. **The drawbridge:** `[mech-3]` before `[fort-5]`, so the barbican is whole for the demo? Recommended: yes.
