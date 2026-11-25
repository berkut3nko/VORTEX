# Land: an island world from a generated map

[[../../../00 Docs Map|Docs map]] · [[../README|engine docs index]] · [[../ROADMAP|roadmap]] · [[worldgen|worldgen]] · [[map|map]] · [[../../../docs/world/map-and-castle-research|research notes]] · [[../../../docs/world/setting|setting]] · [[../../../docs/product/castle-layout-draft|castle layout draft]]

| | |
|---|---|
| Status | Designed, nothing built or measured. |
| Decided | The owner, 2026-11-25 (answers to the research notes' questions, [map-and-castle-research.md](../../../docs/world/map-and-castle-research.md) section 6): see "The owner's answers". Us (this design, 2026-11-25): "Decision" items 1-9, the starter site in "The starter region". |
| Next | The owner's review, then `[land-1]` (the world map as 2D fields and as an image, offline tool). |
| Open | "Open questions (with the answer we recommend)". |
| Marks | (e) is an estimate; every stage names the measurement that confirms or kills it. Code claims were read at engine `6fdd9e5` (`voxel/Generator.cppm` `TerrainSettings`, `TerrainHeight`; the build ceiling and the full-height window). |

This design is about the **land**: the shape of the whole world, its relief, water, regions, biomes, points of interest
and roads. [worldgen.md](worldgen.md) stays the design of **structures** (kits, the planner of buildings, foundations,
necks, history); its `RegionPlan` gains the land fields defined here. [map.md](map.md) stays the in-game map and
minimap; it draws the image this design makes.

## The owner's answers (2026-11-25)

1. **Scale:** generate bold terrain for the whole map, an island or an archipelago, from a generated image of a mythical
   RPG map; fill only one region (the starter region) with detailed nature.
2. **Castle site:** our choice; choose the best for the starter region. Other regions and biomes come later.
3. **Method:** option C, terrain eroded once on the server.
4. **Water:** no water simulation; fixed water levels; no improvement planned at this stage.
5. **Look:** realistic shapes with readable materials.
6. **Biomes:** no border inside the starter region; the other regions are bold biomes, the starter region is detailed.
7. **Roads:** static, baked by procedural generation at each server restart, from the points of interest and the
   terrain.

The two reference pictures the owner gave (a strategy game's region map with relief and pastel region colours, and a
hand-drawn fantasy map) set the **kind** of output: a whole island with coasts, mountain chains, lakes, regions and
their borders, readable as a map image. We take no coastline, region shape, name or style from them (worldgen.md,
"Originality is a rule").

## Decision

1. **One world map, baked at server start.** At start the server turns `(seed, world recipe)` into a **`WorldMap`**:
   coarse 2D fields over the whole island (height, water level, region id, biome, rock hardness, flow) plus the
   points of interest and the road network. Nothing in it is simulated again during the session. A restart bakes it
   again; with the same seed and the same build the result is the same.
2. **The map comes first, the voxels follow.** Every chunk is generated from the `WorldMap` by a pure function of the
   chunk coordinate, as today (worldgen.md "Decision" 1): base height from the map, detail noise, strata materials,
   then worldgen's structures. A global process (erosion, flow, lakes, roads) runs only in the bake.
3. **The server bakes; clients receive the map.** Erosion runs once, on the server, and the baked fields travel to
   every client in the plan (worldgen.md "Decision" 2). Clients never run the bake, so its code can change between
   builds without a desync; only the sampling and the stamper must be bit-identical (pinned by golden tests, as
   `TerrainHeight` is today).
4. **Two levels of detail by region.** The **starter region** gets the full pipeline: a fine bake (1 m cells), strata
   cliffs, scree, trees, ground cover, the castle site, the village, the cave, the roads. **Every other region is bold**:
   the coarse bake (4 m cells), its biome's ground materials and a sparse set of large features (forest masses, rock
   outcrops, lakes); no structures yet. Bold regions are walkable land, not painted scenery.
5. **Water has fixed levels.** The sea has one level (`seaLevel`, 0 m). Each lake has its own fixed level, found by
   filling the depressions of the baked height (a priority-flood fill). Rivers are chains of flat reaches with a fixed
   level each; between two reaches a waterfall or a rapid is a look (an effect and a stepped bed), never a flow.
   Nothing moves water.
6. **Regions and biomes come from the map.** The island is split into named regions (the setting's six: section
   4.2 of setting.md) along watersheds, ridges and rivers where they exist, else along Voronoi edges; each region has one
   biome chosen by its climate (temperature from latitude and height, moisture from the prevailing wind and the
   distance to the sea), with the setting's assignment as the target. Borders between regions are blended over a
   band (e) of 30-60 m.
7. **Points of interest are scored, not dropped.** Each kind of point (castle site, village, cave, crossing,
   landmark) has a score over the baked fields; the bake takes the best candidates per region with spacing rules
   (research notes 4.5). The starter region's castle site is designed (see "The starter region"), not searched.
8. **Roads are the cheapest paths between points of interest.** A\* over the coarse grid with costs from slope,
   water, cliffs and existing roads; switchbacks appear where a slope is too steep; a crossing is a bridge or a ford
   at the narrowest place of a river reach. Roads are baked at each restart with the map.
9. **The map is also an image.** The bake renders the `WorldMap` as a map image (relief shading, water, region
   colours and borders, roads, points of interest): the owner reviews seeds as images before any voxel exists, and
   [map.md](map.md)'s full map uses the same image. A painted image can also be an **input** (a land mask, a mountain
   mask, region colours) that guides the generator (open question 1).

## The world map

### Sizes (e)

| Thing | Size | Why |
|---|---|---|
| The island | about 3 x 2 km (e), 6 regions | 6 regions of about 0.8-1.2 km each; walking across takes several minutes, so regions feel like places |
| The starter region | about 400 x 400 m (roadmap-and-launch.md 2.2), inside a larger region | The demo's village, forest, cave and castle |
| Coarse grid | 4 m cells (one per chunk column): 750 x 500 cells = 375 000 cells (e) | Bold relief; the chunk generator interpolates between cells |
| Fine grid | 1 m cells over the starter region and a 100 m margin: 600 x 600 = 360 000 cells (e) | Cliffs, terraces and gullies at the castle's scale |
| Height range | sea at 0 m; land up to about 100 m; lake and river beds down to -20 m | The world is -48 m to 128 m and nothing is built within 2 m of the top (worldgen.md "Height") |

### Fields of the map

Per coarse cell: height (16-bit, 1/256 m below 128 m), water level (16-bit; "none"), region id (8-bit), biome
(8-bit), rock hardness (4-bit), flow accumulation (16-bit, log scale), road and point-of-interest flags. About 9 bytes
per cell, about 3.4 MB raw for the island (e); delta coding and compression should bring it under 1 MB (e). The fine
grid adds height and hardness for the starter region (about 1 MB raw, e).

## The bake, step by step

| Step | What it does | Input | Output |
|---|---|---|---|
| L0 | Land mask: an island or an archipelago from low-frequency noise with a radial falloff, or from an input image | seed, recipe (or image) | land / sea |
| L1 | Large relief: separate fields for continentalness (height above the sea), ruggedness and ridges (research notes 4.1), with mountain chains as painted or noise ridge lines | land mask | raw height |
| L2 | Climate: temperature from latitude and height, moisture from a prevailing wind across the island (rain on the windward side of mountains, a rain shadow behind them) | raw height | temperature, moisture |
| L3 | Erosion (option C): stream power on the coarse grid (valleys and ranges), then droplet erosion (gullies and fans), with rock hardness from L1's strata so hard layers stay as cliffs | raw height, hardness | eroded height, flow |
| L4 | Water: fill depressions (priority-flood) to get lakes with fixed levels; rivers where flow passes a threshold, cut as flat reaches with steps; the sea at 0 m | eroded height, flow | water levels, river lines |
| L5 | Regions: six regions along watersheds, ridges and rivers; each gets its biome from its mean climate, close to the setting's assignment | height, rivers, climate | region ids, biomes |
| L6 | Starter region detail: the fine grid (1 m) from the coarse one, strata, a fine erosion pass, and the starter site template blended in (see below) | coarse fields, template | fine height, hardness |
| L7 | Points of interest: scored candidates per region (castle site, village, cave, crossing, landmark), with spacing | all fields | points |
| L8 | Roads: A\* between points, slope and water costs, switchbacks, bridges and fords | points, height, water | road lines |
| L9 | Image: the map image for review and for map.md | all | image |

Every step is deterministic for a seed and a build. L3 and L6's erosion are the costly ones: the bake budget is under
30 s on the server at start (e), measured in `[land-2]`.

## Water at fixed levels

- **Sea:** level 0 m everywhere.
- **Lakes:** each depression of the eroded height is filled to its spill height. The lake's level is that height; its
  outflow is a river from the spill point.
- **Rivers:** a river is a polyline along the flow, split into **reaches**. Each reach has one level, its bed is cut
  0.5-2 m (e) below it, and its banks are shaped by the skirt formula (worldgen.md "Skirts"). Where the bed drops more
  than a step (e: 1 m) between reaches, the step is a rapid (several small steps) or a waterfall (one tall step) with a
  spray effect: a look, not flow.
- **Engine rule kept:** water beside a crater stands as a wall (ROADMAP), so fights stay 10 m or more from water
  (worldgen.md "Water"). Shores where fights happen get a dry shelf (gravel, shingle, rock) at least 10 m wide.

## Regions and biomes

- The six regions of setting.md 4.2 (Tamberlough, Kelmeri Terraces, Tolvenn Basin, Ushmara, Calvessa Coast, Brodvel
  Fens) and their climates set the targets: the bake places each where its climate fits (the wet highland on the
  windward mountains, the badlands in a rain shadow, the fens at a river delta, the coast on a dry shore).
- The setting's "broken ring around an inland sea" is a recipe option: a large central lake or inner sea with the
  regions around it (open question 2).
- A **bold region** is generated from the coarse grid with its biome's ground materials (grass, dirt, sand, red earth,
  snow), its rock colour, and large features only: forest masses as tree clusters, outcrops, lakes, rivers. It has no
  structures until worldgen's stages reach it.
- The **starter region** is Tamberlough (setting.md 5.1): wet fir highland with gorges.

## The starter region

The starter region holds the demo: the village, the forest, the cave and Hollowkeep (castle-layout-draft.md).

**Hollowkeep's site (our choice):** a rock spur inside a loop of a **drowned river gorge** (a ria: an incised river
valley whose lower course lies at sea level). Why this site:
- It is the strongest real castle site with water: water on 2-3 sides, cliffs, one neck to the approach field (research
  notes 2.1, 4.3 "Incised meanders").
- A ria's water is the sea's, so its level is the fixed sea level: realistic with no flow at all.
- It keeps the castle draft as it is: the west cliff (10-12 m) and the south cliff (12 m) stand above the gorge, the
  north neck is the approach field with the ditch, the east side is the forest slope. The draft's dry stream bed
  becomes a **gravel shelf** at the cliff foot, 10-15 m wide, then the water: routes R6 (drain culvert) and R7
  (postern path) stay on dry ground and the 10 m rule above holds.
- Later regions add other site types (ridge, island, river crossing, hillside) with their own templates.

**The site template.** A hand-designed landform at the draft's sizes: the spur with its rock step, the neck and the
approach field, the two cliffs, the gravel shelf, the ria loop, the forest slope. L6 blends it into the fine grid
(the skirt formula), and the erosion pass runs after the blend with the template's rock marked hard, so the
surroundings are eroded naturally and the castle's ground stays as designed.

**The rest of the starter region.** The village stands at the head of the ria where the gorge opens into a valley
(a crossing: research notes 2.2), on the road that climbs to the approach field. The forest covers the east slopes,
the cave opens in the forest (roadmap-and-launch.md 4.2). Roads come from L8, not by hand.

## Points of interest and roads

- **Scores** (graded, research notes 4.5): castle site (spur or ridge end, height above the valley, neck width, cliffs,
  water near, room for wards, a road or a crossing near); village (gentle slope, water near, fields, on a road,
  near a castle); cave (a steep slope in a hard layer, forest); crossing (a narrow reach, low banks); landmark (a high
  point seen from far).
- **Spacing:** at least one candidate per kind per region, at most N (recipe), with a minimum distance.
- **Roads:** A\* on the coarse grid between the points of each region and to the neighbours' crossings. The cost of a
  step grows with slope (a road climbs at most about 1:8; steeper ground turns into switchbacks), is high across water
  except at a crossing, and is infinite at cliffs. The road bed is stamped as terrain (worldgen.md's spine), a bridge is
  a structure record at the crossing, a ford a shallow reach.
- **Per restart:** the points and the roads are baked with the map; the same seed gives the same roads.

## From the map to voxels

- The chunk generator (worldgen.md's `RegionGenerator::Structured`, or a new `Land` generator below it) samples the
  baked fields: bicubic height from the fine grid in the starter region and the coarse grid elsewhere, plus small
  detail noise; strata bands set the rock material by height and hardness (research notes 4.4); the biome sets the
  ground materials; water fills to its level.
- The sampling uses only operations whose results are pinned by golden tests on every platform, as `TerrainHeight`
  is today; its golden hash joins `buildHash`.
- The plan's fields are in the `RegionPlan`; the client gets them before chunk sync (worldgen.md "Decision" 2).

## Budgets (e)

| Item | Target | Measured in |
|---|---|---|
| Bake on the server at start | under 30 s for the island and the starter region | `[land-2]` |
| Map transfer to a joiner | under 1 MB compressed | `[land-4]` |
| Chunk generation from the map | no slower than today's generator per chunk | `[land-3]` |
| Map image | 2048 x 1365 pixels, under 1 s to render | `[land-1]` |

## Stages

| ID | Stage | Acceptance |
|---|---|---|
| `[land-1]` | The world map as 2D fields and as an image, offline (a tool): L0-L2, L5 without rivers, L9 | 10 seeds rendered as images for the owner's review; the same seed gives the same image bit for bit |
| `[land-2]` | Erosion and water: L3, L4 on the coarse grid; lakes and rivers on the image | the bake time; lakes have one level each; every river reaches the sea or a lake; images reviewed |
| `[land-3]` | Voxels from the map, offline: the coarse grid everywhere, biome materials, water at its levels; walking the island in the offline game | chunk generation cost; golden hash of the sampling; the owner walks it |
| `[land-4]` | The starter region: the fine grid, strata cliffs, Hollowkeep's site template, points of interest, roads (L6-L8) | the castle draft's heights hold on the site; roads reach every point; the owner walks it |
| `[land-5]` | In a session: the map baked at server start and sent in the plan; clients generate from it | transfer size; hashes equal across clients; a restart with the same seed gives the same world |

The order relative to the ROADMAP (`[wg-W1]`, `[mag-1]`, the demo work) is the owner's call; `[land-1]` is a tool and
touches no runtime code.

## Rejected alternatives

- **Erosion on every client:** float simulations of this size are hard to keep bit-identical across builds and
  platforms; the server bakes and sends (Decision 3).
- **Simulated water:** out of scope by the owner's answer 4.
- **Searching the castle site in the starter region:** the hand-made castle would not fit a found site; the starter
  site is a template (Decision 7). Later regions use search.
- **One fractal noise for the whole island:** self-similar, poor cliffs, no rivers (research notes 4.1, 4.4).

## Open questions (with the answer we recommend)

1. **"A generated image of a mythical RPG map":** is the image our generator's output (Decision 9: we render the map
   and the terrain follows the same fields), or an input the owner makes elsewhere (a painted or AI-made picture whose
   colours mean land, mountains, water and regions)? We recommend: our own generator first (`[land-1]`), and an
   input path for a painted mask (land, mountains, regions by colour) in the same stage, so both work.
2. **Island shape:** one island, an archipelago, or the setting's ring of regions around an inland sea? We recommend
   one main island with a large inland sea or lake and a few small islands, which fits the setting.
3. **A new world per restart?** The same seed gives the same world; does a restart keep the seed, or does a reset
   (setting.md section 2) pick a new one? We recommend: the seed is in the server config; a reset changes it.
4. **Island size:** about 3 x 2 km (e). Larger means longer walks between regions and a larger map transfer. We
   recommend starting at this size and measuring.
5. **Bold regions' content:** walkable land with biome materials, forests and rock only (Decision 4). Enough until
   worldgen reaches them? We recommend yes.

## Sources

The findings and links are in [map-and-castle-research.md](../../../docs/world/map-and-castle-research.md): castle siting
(sections 2-3), climate parameters, Voronoi regions, hydrology-first rivers, stream power, droplet erosion, strata
and site scoring (section 4). Priority-flood depression filling is the standard method of filling sinks in a grid
DEM (Barnes, Lehman and Mulla, 2014).
