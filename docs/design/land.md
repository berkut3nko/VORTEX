# Land: an island world from a generated map

[[../../../00 Docs Map|Docs map]] · [[../README|engine docs index]] · [[../ROADMAP|roadmap]] · [[worldgen|worldgen]] · [[map|map]] · [[../../../docs/world/map-and-castle-research|research notes]] · [[../../../docs/world/setting|setting]] · [[../../../docs/product/castle-layout-draft|castle layout draft]]

| | |
|---|---|
| Status | `[land-1]` built: the world map comes from the owner's drawn image through `tools/worldmap/worldmap.py` (the game's [world-map.md](../../../docs/world/world-map.md)). `[land-2]` built: `voxel::BakeLand` (`vortex.voxel:land`) erodes the coarse grid and finds its lakes and rivers; `vortex_land` bakes the drawn map and writes the review pictures ("The bake as built"). `[land-3]` built: `voxel::CreateLandGenerator` makes the chunks from the baked map; the offline game walks the island with `--land` ("From the map to voxels"). `[land-4]` built: `voxel::BakeLandStarter` bakes the starter region's fine grid, Hollowkeep's site, the points of interest, the cave and the roads ("The starter region as built"). `[land-5]` built: a session plays on the map (`generator = land`); each side bakes its own copy and the client compares the baked map's hash ("The map in a session"). |
| Decided | The owner, 2026-11-25 (answers to the research notes' questions, [map-and-castle-research.md](../../../docs/world/map-and-castle-research.md) section 6): see "The owner's answers". Us (this design, 2026-11-25): "Decision" items 1-9, the starter site in "The starter region". |
| Next | The owner walks the island and the starter region (owner checks 26 and 27), plays the land session on two machines (owner check 28) and measures the bake on the UHD 620 laptop. Every stage of this design is built; what follows is worldgen's structures on the map (worldgen.md). |
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
8. **The map (later the same day):** any map generator may help, but we draw our own: use only the owner's example
   image (no other tool, no shared licence), make a height map and a biome map from it, split it into regions, and
   start in the region of the simplest biome. The height map, the biome map and the points of interest stay editable,
   and a hash checks that the server and the client have the same map: the map is the seed.
9. **Hollowkeep's site on the drawn map:** a spur of the eastern ridge over the starter grassland. After `[land-4]`:
   keep the site the bake chose for now (points.json can move it later).
10. **No map transfer (2026-11-25):** every player has the same generation code and the same map files, and the bake
    is deterministic, so nothing is sent: each side bakes its own map, and the join compares the baked map's hash.

The two reference pictures the owner gave (a strategy game's region map with relief and pastel region colours, and a
hand-drawn fantasy map) set the **kind** of output: a whole island with coasts, mountain chains, lakes, regions and
their borders, readable as a map image. We take no coastline, region shape, name or style from them (worldgen.md,
"Originality is a rule").

## Decision

1. **One world map, from the drawn map, baked at start on every side.** The island's coarse fields come from the
   owner's drawn map through `tools/worldmap/worldmap.py` (world-map.md): `height.png`, `biome_ids.png`,
   `region_ids.png`, `world.json` and `points.json`, editable files under version control and shipped with the game.
   At start the server, and every client before it joins, turns them into the **`WorldMap`**: erosion, water levels,
   rock hardness, flow, the points of interest and the road network. Nothing in it is simulated again during the
   session. A restart bakes it again; the same map files and the same build give the same result.
2. **The map comes first, the voxels follow.** Every chunk is generated from the `WorldMap` by a pure function of the
   chunk coordinate, as today (worldgen.md "Decision" 1): base height from the map, detail noise, strata materials,
   then worldgen's structures. A global process (erosion, flow, lakes, roads) runs only in the bake.
3. **The map is deterministic everywhere; each side bakes it, and the join compares the result.** The owner's rule
   (2026-11-25):
   the same map files give the same world on every machine. The bake and the sampling use only `+ - * /`, `sqrt`,
   `floor` and comparisons on floats (IEEE 754 rounds them exactly; the build has `-ffp-contract=off`) and integer
   hashes; no libm function (`pow`, `log`, `exp`, `sin` ...) decides a value, and both run on one thread in a fixed
   order. Golden hashes pin both on every platform: `Land.BakeGoldenHash` (the bake) and `LandSampling.GoldenHash`
   (heights, water and voxels), with `LandStarter.GoldenHash` for the starter region. So no map is sent (the owner's
   answer 10): the server and every client bake their own copy of the map files, and the join compares the server's
   **baked map hash** (`voxel::LandHash`, 8 bytes) with the client's. Equal: the client generates its chunks from its
   own bake. Different (an edited file, another points.json, another build of the bake): the join is refused with the
   reason, and the **map id** (world-map.md: the files' pixel hashes) tells which input differs. Comparing the result
   and not only the inputs catches the code too. A client pays the bake once (about 3 s here, `[land-5]` measures the
   UHD 620 laptop) and can bake while it connects; server terrain mode stays the fallback for single chunks, never
   for the whole map.
4. **Two levels of detail by region.** The **starter region** gets the full pipeline: a fine bake (1 m cells), strata
   cliffs, scree, trees, ground cover, the castle site, the village, the cave, the roads. **Every other region is bold**:
   the coarse bake (4 m cells), its biome's ground materials and a sparse set of large features (forest masses, rock
   outcrops, lakes); no structures yet. Bold regions are walkable land, not painted scenery.
5. **Water has fixed levels.** The sea has one level (`seaLevel`, 0 m). Each lake has its own fixed level, found by
   filling the depressions of the baked height (a priority-flood fill). Rivers are chains of flat reaches with a fixed
   level each; between two reaches a waterfall or a rapid is a look (an effect and a stepped bed), never a flow.
   Nothing moves water.
6. **Regions and biomes come from the drawn map.** The biome of every place is the colour the owner drew
   (world-map.md); a region is a connected area of one biome. The setting's six named regions (setting.md 4.2) group
   these biome regions (open question 2). Borders between biomes are blended over a band (e) of 30-60 m in the voxels.
7. **Points of interest are scored, not dropped.** Each kind of point (castle site, village, cave, crossing,
   landmark) has a score over the baked fields; the bake takes the best candidates per region with spacing rules
   (research notes 4.5). The starter region's castle site is designed (see "The starter region"), not searched.
8. **Roads are the cheapest paths between points of interest.** A\* over the coarse grid with costs from slope,
   water, cliffs and existing roads; switchbacks appear where a slope is too steep; a crossing is a bridge or a ford
   at the narrowest place of a river reach. Roads are baked at each restart with the map.
9. **The map is also an image.** The tool writes review pictures (biomes, shaded relief, regions), and later bakes
   add roads and points of interest to them: the owner reviews a map as pictures before any voxel exists, and
   [map.md](map.md)'s full map can use the same picture.

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
| L0-L1 | Land, biomes, regions and the raw height from the drawn map (`[land-1]`, built: world-map.md) | the map files | land / sea, biomes, regions, raw height |
| L3 | Erosion (option C): stream power on the coarse grid (valleys and ranges), slumping of slopes over the talus angle, then droplet erosion (gullies and fans), with rock hardness from strata bands so hard layers stay as cliffs (`[land-2]`, built) | raw height, hardness | eroded height |
| L4 | Water: closed basins get an outlet (breaching), the rest of each basin is a lake at its spill height or a filled puddle; rivers where flow passes a threshold, as flat reaches with steps; the sea at 0 m (`[land-2]`, built) | eroded height | water levels, flow, river lines |
| L6 | Starter region detail: the fine grid (1 m) from the coarse one, strata, a fine erosion pass, and the castle site's two graded pads blended in (see below) | coarse fields, template | fine height, hardness |
| L7 | Points of interest: scored candidates per region (castle site, village, cave, crossing, landmark), with spacing | all fields | points |
| L8 | Roads: A\* between points, slope and water costs, switchbacks, bridges and fords | points, height, water | road lines |
| L9 | Image: the map image for review and for map.md | all | image |

Every step is deterministic for a seed and a build. L3 and L6's erosion are the costly ones: the bake budget is under
30 s on the server at start (e), measured in `[land-2]`.

## The bake as built (`[land-2]`)

`voxel::BakeLand(LandSource, LandSettings)` (`engine/src/voxel/Land.cppm`) runs on the coarse grid of 4 m cells; the
drawn map (5 m pixels) gives 800 x 598 cells. Every knob is a `LandSettings` field with the tuned default.

1. **Resample** the drawn height (bilinear) and the biome and region ids (nearest). The coastline stays where the map
   draws it: a sea cell is at -0.5 m or lower, a land cell at 0.3 m or higher, before and after every step.
2. **Hardness** 0-15 per cell: a base per biome (desert 2, grassland 3, forest 4, mountain 8, rocky isle 10), plus 6
   in hard strata bands (5 m bands tilted by slow noise; one band in three is hard) in the mountains and above 30 m.
3. **Stream power** (40 rounds, implicit, n = 1): each round routes the flow by a priority-flood from the sea and the
   map border, sums the drainage area, and lowers each cell towards its receiver by K A^0.5 / distance, K scaled by
   1 / (1 + hardness / 2). No cell goes more than 12 m below its raw height, and a drawn lake's rim not more than 0.5 m.
4. **Slopes:** 6 rounds of slumping where a slope is steeper than the talus (0.9 rise over run, hard rock 3.0).
5. **Droplets:** one per land cell, 80 steps each (after Beyer 2015): they cut gullies and lay fans; the ground and
   the droplets' sediment keep the same total, and a droplet that reaches water ends there.
6. **Breaching:** every closed basin of the eroded height gets an outlet cut along its drainage path from its lowest
   cell, at most 6 m deep (after Lindsay 2016). A basin with a drawn lake is cut down to the drawn level (its lowest
   drawn bank less 0.5 m), however deep the outlet must be: the owner's lake keeps its drawn size, and its outlet
   is a gorge.
7. **Lakes:** a priority-flood fill of what is left; each basin is a lake at its spill height when the map draws a
   lake in it or it covers at least 2000 m^2 and is 0.75 m deep, else a puddle that is filled.
8. **Rivers:** the flow is routed again; a river starts where the drainage area reaches 0.06 km^2 and runs down its
   receivers to the sea, a lake or another river. Its level, from the mouth upstream, is the bank less 0.3 m rounded
   down to 0.5 m and never below the water it drains into, so the levels form flat reaches with steps. The bed is cut
   0.5-2 m below the level (deeper with more flow), 3 cells wide where the area is 4 times the threshold; a diagonal
   step also cuts one side cell, so the channel is connected through cell edges; a dry bank lower than the water is
   raised to 0.2 m above it.

`voxel::CheckLand` holds the rules: the sea did not move; dry land is above the sea; each lake has one level over a
bed below it; a river's bed is below its level; its level never rises downstream; and every river reaches the sea
or a lake, through the rivers it joins (`Land.*` tests, each check with a planted fault). `voxel::LandHash` covers
every baked field: two bakes of one source in one build are equal.

**The tool:** `build/engine/tools/land/vortex_land --map assets/world --out build/land [--twice]` (run from the game's
root) logs `Land: baked ...`, the lakes, the rivers, `Land: hash H` and `Land: checks pass` (exit 3 when a check
fails or `--twice` finds two hashes), and writes `land_height.png` and `land_water.png` (16-bit, the world map
tool's height encoding; water 0 = none), `land_flow.png`, and the review pictures `land_relief.png` (shaded relief
with the sea, lakes and rivers) and `land_change.png` (red cut, green laid down, full colour at 5 m).

**Determinism:** the stream power's `A^0.5` is a `sqrt`, and a river's depth grows by 0.5 m per doubling of its flow
by halving in a loop (no `log2`). `Land.BakeGoldenHash` pins the bake of the test island.

**The first map (map id `5556ecd6b3b7d86d`, 2026-11-25, m):** baked in 2.6-3.0 s (stream power 2.2-2.5 s), `Land: hash
e94ffdb8eb6e3288`; erosion cut
2.8 million m^3 and laid down 0.9 million m^3 (the rest reached the sea), deepest cut 8.1 m; 1673 outlet cells cut,
26 puddles filled. Three lakes: the drawn northern lake at 3.88 m (0.036 km^2, 6.6 m deep; its outlet is a gorge to
the north coast), the drawn desert lake at 2.97 m, and a mountain tarn at 35.2 m. 14 rivers, 2.8 km, 194 reaches, 7
steps over 1 m (largest 5.0 m); 8 reach the sea, 4 a lake, 2 join another river. The starter grassland is a low
plain of laid-down sediment with a stream down its middle to the south coast.

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

- The biomes are the drawn map's (world-map.md). The six regions of setting.md 4.2 (Tamberlough, Kelmeri Terraces,
  Tolvenn Basin, Ushmara, Calvessa Coast, Brodvel Fens) are named groups of its biome regions (open question 2): for
  example the southern desert fits Ushmara, the forested north-west highland Tamberlough.
- The drawn map has an inland lake in the north and a sea inlet with islands in the east, not the setting's ring
  around an inland sea; the setting's text can follow the map (the owner's call).
- A **bold region** is generated from the coarse grid with its biome's ground materials (grass, dirt, sand, red earth,
  snow), its rock colour, and large features only: forest masses as tree clusters, outcrops, lakes, rivers. It has no
  structures until worldgen's stages reach it.
- The **starter region** is the largest region of the simplest biome on the drawn map: region 8, the grassland in
  the north-east, about 350 x 580 m, between the eastern mountain ridge and the eastern forest, its south end on the
  sea (world-map.md). Which setting region it belongs to is the owner's call (open question 2).

## The starter region

The starter region holds the demo: the village, the forest, the cave and Hollowkeep (castle-layout-draft.md).

**Hollowkeep's site (the owner's answer 9):** a **ridge spur**: a rock tongue that runs east from the eastern
mountain ridge (region 6) and ends above the starter grassland. Why this site:
- It is a real castle type (a spur castle): steep ground on 3 sides, one neck to the higher ridge, cut by a ditch
  (research notes 2.1).
- It keeps the castle draft as it is, turned by 90 degrees: the draft's north neck faces west to the ridge, so the
  approach field and the ditch lie on the ridge side. The two cliffs (10-12 m and 12 m) stand above dry ravines on
  the spur's flanks. The draft's dry stream bed is the floor of one ravine: routes R6 (drain culvert) and R7
  (postern path) stay on dry ground as drawn.
- It needs no water: the site works with the fixed-level water rule and no rivers.
- The ridge gives the spur its height with no cut terrain: the castle sees over the whole grassland to the sea.
- Later regions add other site types (ria, island, river crossing, hillside) with their own templates.

**Built on the slope (the owner's rules, 2026-11-25).** The first site carved the draft's landform into the land as
one flat 96 x 136 m rectangle, which read as a cube on the ridge. Medieval builders had no earthmoving machines and
built around the land, so now:
1. no macro flattening: the relief across the site stays the land's own;
2. the curtain wall is a polygon along a natural contour of the spur or knoll, not a rectangle;
3. foundations go down, terrain never comes up: every wall, tower and building reaches 1.5 m below its own ground,
   and a wall is taller where the ground falls;
4. tiers: the keep on the high point, the inner courtyard inside the inner wall, the outer bailey between the walls,
   joined by the gates and the ground's own slope;
5. micro-grading only: level pads under the keep (18 x 18 m) and the gatehouse (14 x 10 m), blended outward over the
   angle of repose (1.5 m of run per metre of rise) with a noisy edge.

The castle draft's fixed heights (field 0 m, a 5 m rock step, 11-12 m cliffs) become what the chosen hill gives; its
parts, routes and necks stay.

### The starter region as built (`[land-4]`)

`voxel::BakeLandStarter` (`engine/src/voxel/internal/LandStarter.cpp`; `BakeLand` calls it when
`LandSettings::starterDetail` is set) runs after the rivers, on one thread, with the same determinism rules as the
coarse bake (`LandStarter.GoldenHash` pins it).

1. **The fine grid (L6):** 1 m cells over the starter region's box and 160 m around it (on the first map 672 x 900
   m). Its base is the coarse height (`voxel::LandCoarseHeight`); it stores only a correction `delta`, faded to 0 over
   its outer 16 m, so it meets the coarse land with no seam. Per cell it also holds a surface kind (natural, road,
   site) and how much of the biome's detail noise stays (none on roads and the site).
2. **Strata cliffs:** where the coarse cell is hard (hardness 9 or more, dry) and the slope is over 0.4, 4 m bands
   become benches and cliffs (60 % of each band's height rises gently, the last 40 % steeply), fully over a slope of
   0.8.
3. **Hollowkeep's site** (`voxel::LandSite`): the search tries every 8 m of the fine grid for a keep point: on
   mountain ground (any ground outside the starter region if none fits); the ground 50 m out lies 4 m or more below
   it in at least 14 of 24 directions (a spur or a knoll); the ground within 6 m departs at most 3 m (a pad needs
   only micro-grading); 10-60 m above the starter region's mean and at most 85 m; dry within 46 m; the side away from
   the neck within 150 m of the starter region. The most fall, the least grading and the nearest win. The neck (and
   the gate) faces the highest ground 50 m out. In each of 24 directions (15 degrees, a literal cos / sin table:
   `voxel::LandSiteDirection`) the curtain wall's radius is where the ground first lies 7 m below the keep's, less
   2 m (16-42 m), and the inner wall's where it lies 3 m below (11 m to the curtain less 7 m); both smoothed twice
   round the circle. Only two pads are graded (rules above); everything inside the curtain keeps its height and
   loses its plants (surface `Site`).
4. **Points of interest (L7):** village (level dry ground of the starter region, 120-400 m from the approach field,
   near fresh water); landing (dry starter ground at the sea, nearest the village); cave (a slope of 0.5-1.4 outside
   the starter region, 120-450 m from the village, with rock over a 26 m tunnel falling 2 m and a 5 m chamber; harder
   ground first); landmark (the highest dry ground of the fine grid); crossings where a road meets a river.
5. **The cave:** a tunnel of radius 2.2 m uphill into the slope from its mouth, a chamber of radius 5 m at its end,
   both with flat floors; the generator carves it and lowers the column classes it crosses.
6. **Roads and trails (L8):** A\* on the fine grid from the village: roads to the castle bridge's far end (its last point fixed
   at the gatehouse pad's level, \`LandSite::gateGround\`) and the landing
   first, then trails (footpaths) to the cave and the landmark, so that routes look made by people, not by a ruler
   (the owner's feedback, 2026-11-25). A step costs its length times the ground's cost (1-2.1: noise of 45 m and
   15 m, better and worse ground) or 0.35 on a road already made (later roads and trails join and branch off earlier
   ones: a network), plus 30 grade^2 up to the grade limit (`roadMaxGrade` 0.16, about 1:6; `trailMaxGrade` 0.35)
   and 400 per unit of grade above it (switchbacks), plus 12 side-slope^2 (routes follow the contours); sea, lakes
   and the castle's footprint are closed; a river step costs 6 m more. The line is smoothed twice over 7 cells.
   Crossings: a trail fords a river 0.3 m under its water; a road crosses on a timber bridge, whose deck over each run
   of river points is level at the higher bank, 0.9-3 m over the water (`LandRoad::bridgeCells`). Points within 1.5 m
   of a route already laid (each fine cell within 2 cells of one keeps its nearest point; the search then takes that
   route's point nearest to this one) are fixed to its bed, and a bridge's points to its deck, when the grade lets the
   route reach them from the last fixed point (beside a hairpin, two legs of a road lie close at different heights:
   one wins), so junctions and shared stretches have one height (`LandStarter.RoutesMeetAtOneHeight`; on the first
   map the largest difference between routes within 0.5 m fell from 5.7 m to 0.12 m). The bed of the free points is
   the mean of the ground's upper and lower grade envelopes, averaged over up to 9 m, then held to the limit per metre
   by a forward pass inside the band the fixed points allow: every step is at most the route's limit
   (`LandStarter.EveryRoadReachesItsPointWithinTheGrade`). A road is stamped 1.6 m either side as dirt, a trail
   0.9 m (on a steep trail the voxels make rough steps); the ground beside slopes to the bed as an embankment or a cut
   of 1 in 2 (1.5 m plus twice the height between bed and ground, at most 8 m), so a route never stands on a wall.
   Under a bridge the river is not stamped. The generator builds each bridge from the road's points over the water
   and 2 more on each bank: a 3.4 m plank deck 0.3 m thick at the bed's height (at least 0.4 m over the water), a rail
   0.9 m high on posts every 2 m, piles to the river bed every 4 m. On the first map: the castle road 941 m, the
   landing road 496 m (a bridge of 6 cells), the cave trail 568 m and the landmark trail 711 m.

**The owner's points (`points.json`):** `vortex_land` writes the bake's points to `build/land/points.json`
(`{"kind": "village", "x": 2428.5, "z": 784.5}`, map metres from the map's corner). Copied into the map folder
(`assets/world/points.json`) and edited, each kind it names is placed there instead of scored (crossings are
ignored: the roads find them); a castle from the file takes the rotation whose neck faces the highest ground.
`LandStarter.PointsJsonPlacesPointsAndReadsBack`. The map id (world-map.md) and the baked map hash both cover
points.json (Decision 3).

**The first map (2026-11-25, m):** the starter bake takes 0.2-0.5 s. The site stands at (2356.5, 964.5) map m,
neck to the west, field 17.0 m, at the ridge's southern foot above the grassland's south-west corner (fit
125.9 m^2: the ridge face near the grassland is steep, so the site moves much land; the owner can place it with
points.json). Village (2428, 784), landing (2518, 1098), cave mouth (2308, 784) at 57.6 m, landmark (2210, 836).
Roads (after the network rules): to the castle 264 m, to the landing 382 m with a ford of 9 cells, to the cave
388 m; each at most 0.120; the castle and cave roads share their first stretch from the village.
The player starts in the village (`voxel::LandSpawn`). The review picture is `build/land/land_starter.png`
(relief, water, roads in brown, the site's ground lighter, points as squares).

Known limits of this first pass: the site's cliffs are straight planes of stone; near the field's corners the
edge noise leaves thin rock fins; the castle stands on a block 10-15 m above low ground at this site; no fine
erosion pass yet; the village, landing and landmark are points only (their structures come with worldgen.md's
stages).

### Hollowkeep's greybox

`sim::HollowkeepTemplate(map, land settings)` (`engine/src/sim/internal/Hollowkeep.cpp`) builds the castle as boxes
(the draft's section 9: greybox first), every box from 1.5 m below its own ground (`voxel::LandHeight`):
- the curtain (2.5 m thick) and the inner wall (2 m) along the site's polygons, one column every 0.5 m; each run's
  wall walk is its highest ground plus 8.8 m (8 m for the inner wall), so the walks step down the slope; a dark
  battered plinth to 1.2 m over the ground; a 1.2 m parapet with merlons every 2 m on the outer side;
- towers at every third corner of the curtain (none beside the gate): 6 x 6 m, 14 m over their highest ground,
  hollow from the bailey's ground, two timber floors, a door on the bailey side;
- the gatehouse on its pad, squared to the gate's direction: 14 x 8 m, 14 m high, a 4 m passage, the timber upper
  room (N3), the portcullis slot, the portcullis ([mech-1]) and its lever 6 m inside;
- the ditch across the neck: one trench outside the curtain's 4 edges around the gate, 4-10 m out along the ray from
  the keep, its floor level at 5 m under the gate's ground, 3 m wide, with 1:1 sides (nothing is dug where the ground
  already lies that low), cut on a 0.5 m grid of 0.6 m columns so no ground stands between the cuts; a level timber
  bridge on the gate's line from 3 m out of the gate to 1 m past the trench (`voxel::LandSiteBridgeSpan`), on two
  trestles, open to the sky (a cutting where the far bank rises); the castle road ends at its far end at the
  gatehouse pad's level and cuts through the neck where the ground is higher; the player arrives there (the
  drawbridge waits for [mech-3]);
- the keep on its pad: 16 x 16 m, 22 m high, 3 m walls (dark stone to 8 m, brick above), the sanctum's keel floor
  (bedrock), timber floors at 5, 11 and 17 m, a door towards the gate, merlons on top.

The game builds it at the start whenever the map has a site (`Hollowkeep: built N operations, M mechanisms at
(x, y, z) m in T s`; `--no-hollowkeep` leaves it out; `--camera hollowkeep` looks at it from the open side). On the
first map: the keep at map metres (2260.5, 524.5), 44.7 m, on a spur at the ridge's north end, the gate west to the
ridge, the curtain 16-34 m from the keep, 2773 operations in 1.7 s. Tests: `Hollowkeep.*` (every tall structure
reaches below its own ground; the wall walk steps; the portcullis and lever pass the authoring rule) and
`LandStarter.TheSiteKeepsTheNaturalSlope`. Not yet: the bell tower, the stables and their culvert, the scaffold, the
postern, doors as mechanisms, the objects of the draft's section 8, a session. The detailed castle that replaces this
greybox (wall anatomy, roofs, interiors, the bailey, 3 material tiers, bypass loops, set pieces) is designed in
[fortress.md](fortress.md), `[fort-1]`..`[fort-5]`.

## Nature

The owner's rules for filling the land (2026-11-25), as voxels. Built in `[land-6]` (2026-11-25) for the coarse map and
the starter region alike: `PlaceNature` and the ground cover in `engine/src/voxel/internal/LandGenerator.cpp`.

**Getting through plants (our decision, 2026-11-25).** Solid voxel vegetation makes a forest a maze, so plant matter
has three kinds, each with one rule:
- **Wood and rock** (trunks, deadwood, logs, boulders, cairns): solid. Cover, obstacles, destructible like every
  material.
- **Foliage** (leaves, needles, reeds, ferns, flowers, mushrooms: `MaterialFlag_Foliage`): no terrain collision, as
  water. Players walk through a bush and debris falls through a canopy; rays and casts still meet it, so a bush is
  concealment, not protection, and a blast clears it. `TerrainColliders` skip `MaterialFlag_Liquid |
  MaterialFlag_Foliage`; `sim::kPlayerRuleRevision` is 2 (a player near plants moves differently than before).
- **Ground cover** (moss, litter, mud, gravel): the top voxels' material, no height.
- No slowdown in undergrowth (it would change the movement rule again); later, if play wants it.
- Heights keep the view clear: shrubs under 1.1 m and reeds under 1.2 m (below the 1.6 m eye), every canopy from
  2.4 m up, ground-hugging patches 1-3 voxels high.

**The four layers, as built:**
1. **Geology and water:** scree (gravel cover) on slopes of 0.35-0.6 below hard strata, bare stone on steeper hard
   ground and over 1.15 slope; cobbles (gravel) on river and lake beds, mud flats on fresh-water shores; half-buried
   boulders (0.5-1.6 m, 30 % of their radius in the ground, mossy tops in forests).
   **The coast** (the owner's feedback, 2026-11-25): the distance to the nearest sea cell's centre (up to 12 m, smooth
   along the coast) shapes it. The beach reaches up to 0.8-2.4 m above the sea (noise of 18 m), less with the
   distance, so its width wanders; shingle (gravel) in patches of 9 m noise; bare stone on slopes over 0.7 within
   10 m (sea cliffs); rocks in the sea's shallows (6 % of the 3 m cells down to 1.5 m deep); on the coast dune grass
   tufts, a few boulders and driftwood logs (no moss).
2. **Plants in strata:** trees ("Trees" below), nurse logs (deadwood, moss on the back, mushrooms and ferns beside),
   shrubs of 2-4 ragged lumps under 1.1 m, fern patches, flower patches (yellow or violet) on grassland, mushroom
   rings; the forest floor is litter (dark), moss and grass in patches of a few metres (litter on about a fifth).
3. **Life:** not yet (animal trails, burrows, bones; birds and insects need effects and sound).
4. **History:** cairns of four stacked stones 2.8 m beside every road, one every 90 m, alternating sides. Wagon ruts,
   ruined walls and monoliths come with worldgen's structures.

**The rules, as built:**
- **Two grids:** trees on a jittered 6 m grid ("Trees"); the plants and rocks below them on a jittered 3 m grid, at
  most one feature per cell, by the biome's odds (forest: 15 % shrub, 15 % fern, 3 % each mushrooms, boulder, log;
  grassland: 18 % flowers, 3 % shrub; mountain: boulders, shrubs; desert and rocky isles: a few boulders).
- **70 / 30:** a slow density field (value noise of 40 m) gives about 30 % of the ground the full odds and the rest a
  quarter of them: dense pockets and open ground.
- **Masks (combat hygiene):** nothing on the fine grid's roads and Hollowkeep's site; only reeds in and beside fresh
  water (0.5 m deep at most, its bank up to 0.6 m above); no tall feature on slopes over 0.8; nothing under 1.2 m
  above the sea but reeds and rocks; no soft plant (shrub, fern, flowers, mushrooms, log) on bare ground: slopes over
  1.1, hard strata over 0.3, mountain ground above 43 m or over 0.5 (the owner's feedback: bushes on the grey
  mountains read wrong), only boulders there; conifers below 70 m and dead trees below 60 m on mountains. The sandbox overwrites
  what stands on its plaza.
- **Glue:** the mixed ground cover around features stands in for decals (moss on boulders and logs, litter under
  the forest, mud along water).
- Pure integer hashes and arithmetic, as the rest of the sampling: `LandSampling.GoldenHash` pins it.

### Trees

The owner's feedback (2026-11-25): no round "olive" crowns on sticks, no strict geometric forms. The research
(Weber and Penn 1995, Runions et al. 2007, Palubicki et al. 2009, Veloren's and Vintage Story's tree generators)
points to a parametric skeleton: tapered wood segments in two levels and ragged leaf clumps at the branch tips,
deterministic and cheap enough for a per-chunk generator. Space colonisation stays for a later "hero oak".

- **Species** (on the 6 m grid; chance per cell: forest 0.8, dark forest 0.85, mountain 0.2, grassland 0.05):
  - oak: 7-10.5 m, trunk flare, a fork at 40-50 % of the height into 3-5 crooked two-segment limbs, two side
    branches per limb, a clump at every tip and one over the fork;
  - birch: 8-11 m, slim pale trunk (`bark_pale`), sometimes two stems, 7 short rising branches with small clumps;
  - spruce: 9-12 m, a straight leader, whorls every 0.5-0.8 m of 4-5 drooping branches with needle slabs, 15 % of
    whorls missing;
  - pine: 9-12 m, a bare bent trunk, 4-6 branches high up with flat needle clumps;
  - snag: a broken deadwood trunk with splinters and stubs (5 % of trees, 10 % on mountains).
- **In patches:** a 35 m noise picks the species (forest: oak, birch, spruce bands; dark forest: spruce and pine;
  grassland: lone oaks and birches; mountain: spruce low, pine above 45 m, none above 70 m).
- **Age:** 60 m noise scales a tree by 0.75-1.0; in the sparse pockets by 0.85 more; on mountains above 45 m by 0.7.
- **On slopes:** the foot is the lowest of 5 ground samples less 0.3 m; above a slope of 0.15 the lowest 1.2 m leans
  downhill and the trunk goes up again (pistol butt), and three roots run downhill and to the sides into the ground.
  No tree on slopes over 0.9.
- **Clear of play:** the trunk's cell and 8 directions at 1.5, 3 and 4.5 m around it are natural ground without water
  (a trail 0.9 m wide passing diagonally slipped between 4 samples at 4 m), so trunks, roots and canopies never stand
  on or hang over a road, a trail, the castle site or a bank; every clump's foot stays 2.4 m above the tree's ground.
- **Determinism:** branch azimuths turn by the golden angle as fixed (cos, sin) constants, lengths and jitters from
  integer hashes, directions normalised with sqrt: no libm function.

**Materials:** appended to the world palette (`WorldMaterials`: moss, litter, mud, gravel, deadwood, bark_pale;
foliage: needles, reed, fern, flower_yellow, flower_violet, mushroom), so the older ids keep their values; they are not in
a session's Welcome (the material table is in the build hash).

**Measured (2026-11-25, m):** chunk generation 54 us per chunk over 32 x 32 columns around the village (the built-in
terrain 201 us); headless shots in `build/land/t-*.png`. Not yet: a windowed walk
(owner check 26 covers it), life, wagon ruts, puddles, monoliths.

## Points of interest and roads

- **Scores** (graded, research notes 4.5): castle site (spur or ridge end, height above the valley, neck width, cliffs,
  water near, room for wards, a road or a crossing near); village (gentle slope, water near, fields, on a road,
  near a castle); cave (a steep slope in a hard layer, forest); crossing (a narrow reach, low banks); landmark (a high
  point seen from far).
- **Spacing:** at least one candidate per kind per region, at most N (recipe), with a minimum distance.
- **Roads:** A\* on the coarse grid between the points of each region and to the neighbours' crossings. The cost of a
  step grows with slope (a road climbs at most about 1:6, a trail 0.35; steeper ground turns into switchbacks), is high across water
  except at a crossing, and is infinite at cliffs. The road bed is stamped as terrain (worldgen.md's spine), a bridge is
  a structure record at the crossing, a ford a shallow reach.
- **Per restart:** the points and the roads are baked with the map; the same seed gives the same roads.

## From the map to voxels

Built in `[land-3]` for the coarse grid: `voxel::CreateLandGenerator(map, LandGeneratorSettings)`
(`engine/src/voxel/internal/LandGenerator.cpp`), an `IWorldGenerator`. `CenteredLandOrigin` puts the map's centre at
the world origin; outside the map the border cells (the sea) continue.

- **Height** (`voxel::LandHeight`): Catmull-Rom over the 4 x 4 nearest cells, clamped to the 4 nearest cells' range
  (no overshoot at a coast or a bank), plus detail noise (two octaves of integer-hash value noise, 9 m and 3.5 m)
  with an amplitude per biome (grassland 0.35 m, forest 0.5, desert 0.7, rocky isle 1.0, mountain 1.6), blended
  bilinearly; a cell on or next to water has none, so banks and shores keep their baked height.
- **Water** (`voxel::LandWaterLevel`): the highest level of the 4 nearest cells that hold water; water voxels fill
  from the ground up to it. Sand covers the ground under water and within 1.25 m of a column whose water stands
  above its ground.
- **Materials:** the biome of the nearest cell to a point moved by up to 7 m of noise (borders wander). Grass and
  forest: grass, 2 m of dirt, stone; desert: 3 m of sand, stone on slopes over 0.8; mountain: dry grass below 45 m
  on gentle slopes, stone above, snow over 92 m; rocky isle: stone. Any slope over 1.15, and a hard-strata cell's
  slope over 0.6, is stone; dark stone from 12 m down. The materials are the engine's default palette (the material
  table and its hash do not change).
- **Trees** on a jittered 5 m grid with a chance per biome (forest 0.55, dark forest 0.7, grassland and mountain
  0.05, desert 0); none in water, under 1.2 m, on steep ground or on mountains over 50 m.
- **The fine grid** (`[land-4]`): inside it the height is the coarse height plus the bilinear `delta`, the detail
  noise is scaled by the cell's `detail`, and its surface kind replaces the biome's cover: road (dirt), site (grass;
  stone where steep). The cave is carved from its tunnel and chamber, and the columns it crosses classify down to
  its floor. `ClassifyColumn` agrees with the voxel loop (`LandSampling.ClassifiedRowsAgreeWithTheVoxelLoop`,
  `LandStarter.ColumnClassesAgreeAroundTheCaveAndTheSite`).
- **Spawn** (`voxel::LandSpawn`): the dry cell nearest the starter region's middle whose 3 x 3 cells differ by at
  most 0.5 m and with no water within 3 cells.

**The game:** the world map is the offline game's terrain (2026-11-25; `--terrain classic` keeps the old noise
terrain). It bakes `assets/world` at start (about 3 s), logs `Land: DIR baked in T s, hash H: ...; spawn at (x, z) m in
region R`, and generates the world from it with the starter village at the world origin (`voxel::LandWorldOrigin`;
sessions use the same origin, `sim::RegionLandSettings`). The sandbox (plaza, yard, playground castle) is built
beside the village, so every offline gate (G3-G10, G21) runs on the island; `--land [DIR]` leaves the sandbox out and
starts the player in the village (`Land: player at ...`). A headless run of 400 frames fills the window in 0.2 s (14 891 chunks, 7186 dense, in 1.07 s of
generation) and exits 0 (m).

**Cost (m, `vortex_land --chunk-cost`, one thread):** 50 us per chunk over 32 x 32 columns around the spawn (6224
chunks), against 205-226 us for the built-in terrain generator (11 322 chunks): inside the budget "no slower than
today's generator".

### The map in a session (`[land-5]`)

- **The region:** `sim::RegionGenerator::Land` (2, protocol 13). `RegionDesc` carries the server's `landHash`
  (`voxel::LandHash` of its bake) and `landSourceHash` (`voxel::LandSourceHash`: every pixel of the map files, the
  starter region and the owner's points) in `Welcome`, and holds this process's own baked map in `land` (never on
  the wire). `sim::MakeRegionGenerator` makes `voxel::CreateLandGenerator(land)` with the map's centre at the world
  origin (`sim::RegionLandSettings`).
- **The server:** a session file with `generator = land` and `land_map = DIR` (relative to the working directory);
  `net::LoadSessionFile` bakes it (`net::BakeSessionLand`, `voxel::CheckLand` must pass) and attaches it
  (`sim::SetRegionLand`), logging `Land: DIR baked in T s, hash H, files F`. `engine/tools/server/sessions/land.session`:
  256 m around the village, the castle's site and the cave (chunks 160..224 x -110..-46), run from the game's root.
- **A client:** `ClientSettings::landMap` gives its own baked map. At the Welcome of a land region the client
  compares (`sim::CheckRegionLand`): equal baked hashes attach its map and it plays on; otherwise it fails with the
  reason - its map files differ (the source hashes differ), the same files bake to another map (another build of the
  bake), or it has no map. The game bakes `--land DIR` (default `assets/world`) in the background from the start of
  a `--connect` / `--join` run, so the Welcome waits only for what is left of it; it then exits 1 with `Session:
  HOST answered, and this client could not take its session`. `vortex_bot --land DIR` bakes before it connects;
  vortex_server's own bots share the server's map.
- **Builds:** `voxel::kLandGoldenHash` (the three golden hashes) is part of `buildHash`: builds that bake or sample
  differently refuse each other at the Hello (`Build`), before any map is compared.
- **Measured (2026-11-25, m):** `vortex_server --session=engine/tools/server/sessions/land.session --drill=60
  --clients=2`: the bake 2.9 s, the region loaded in 4.0 s (9096 dense / 7904 packed chunks, 9.6 MiB RLE); both
  bots 0 probe mismatches, `hashes equal`, entities agree (the full-window bots took 62-64 s to load, against
  105-114 s on arena250 with the same options). Over UDP (G15's way, port 27016): a headless game client baked its
  own map in 3.0 s while it connected and its world hash equalled the server's over its box; a client with an edited
  points.json (`--land` on a copy) failed with "this client's map files differ from the server's" and exit 1.
  `LandSession.*` (same map joins and hashes equal; other files or none fail; the reason names the files or the
  bake; the session file round-trips).

## Budgets (e)

| Item | Target | Measured in |
|---|---|---|
| Bake at start (server and each client) | under 30 s for the island and the starter region | `[land-2]`: the island's coarse grid 3.0 s (m); `[land-4]`: the starter region 0.2-0.5 s more (m) |
| Bake on the owner's UHD 620 laptop (a client's join) | under 10 s (e) | the owner (check 28); here 2.9-3.2 s (m) |
| Chunk generation from the map | no slower than today's generator per chunk | `[land-3]`: 50 us against 205-226 us (m) |
| Map image | 2048 x 1365 pixels, under 1 s to render | `[land-1]` |

## Stages

| ID | Stage | Acceptance |
|---|---|---|
| `[land-1]` | The world map from the owner's drawn image (a tool): biomes, regions, raw heights, review pictures, the map id. **Built** (world-map.md) | the same source gives the same map id on two Python builds (done: `5556ecd6b3b7d86d`); the owner reviews the pictures |
| `[land-2]` | Erosion and water: L3, L4 on the coarse grid; lakes and rivers on the image. **Built** ("The bake as built") | the bake time (done: 3.0 s); lakes have one level each and every river reaches the sea or a lake (done: `voxel::CheckLand`, `Land.*`); the owner reviews the pictures |
| `[land-3]` | Voxels from the map, offline: the coarse grid everywhere, biome materials, water at its levels; walking the island in the offline game. **Built** ("From the map to voxels") | chunk generation cost (done: 50 us per chunk, the built-in terrain 205-226 us); golden hash of the sampling (done: `LandSampling.GoldenHash`); the owner walks it (owner check 26) |
| `[land-4]` | The starter region: the fine grid, strata cliffs, Hollowkeep's site template, points of interest, roads (L6-L8). **Built** ("The starter region as built") | the castle draft's heights hold on the site (done: `LandStarter.TheSiteKeepsTheCastleDraftsHeights`); roads reach every point (done: `LandStarter.EveryRoadReachesItsPointWithinTheGrade`); the owner walks it (owner check 27) |
| `[land-5]` | In a session: the server and each client bake the map from their own files; the join compares the baked map hash (and names what differs); clients generate from their own bake; the bake's and the sampling's golden hashes join `buildHash`; the map id covers points.json. **Built** ("The map in a session") | a client with an edited map file is refused with the reason (done: UDP run and `LandSession.AClientWithOtherMapFilesOrNoneFails`); world hashes equal across clients (done: the drill, `hashes equal`); the bake time on the UHD 620 laptop (the owner, check 28); a restart with the same files gives the same world (the golden hashes) |
| `[land-6]` | Nature: plants in strata, rocks, ground cover, cairns; foliage without collision. **Built** ("Nature") | the masks keep roads and the site clear (done: the shots); foliage has no collision (done: `TerrainColliders.FoliageHasNoCollision`); chunk cost under the built-in terrain's (done: 60 us against 200 us); the owner walks it (owner check 26) |

The order relative to the ROADMAP (`[wg-W1]`, `[mag-1]`, the demo work) is the owner's call; `[land-1]` is a tool and
touches no runtime code.

## Rejected alternatives

- **Sending the baked map to every client** (the plan before 2026-11-25): needed only while the bake could differ
  between machines. With the bake deterministic and pinned by golden hashes, the files every player has already
  hold the map; a hash at the join is enough (Decision 3, the owner's answer 10).
- **Simulated water:** out of scope by the owner's answer 4.
- **Searching the castle site in the starter region:** the hand-made castle would not fit a found site; the starter
  site is a template (Decision 7). Later regions use search.
- **One fractal noise for the whole island:** self-similar, poor cliffs, no rivers (research notes 4.1, 4.4).

## Open questions (with the answer we recommend)

1. **The image:** answered (owner's answer 8): the owner's drawn map is the input; `[land-1]` turns it into fields.
2. **Setting regions:** the drawn map has 41 biome regions; how do the setting's six named regions (setting.md 4.2) map
   onto it, and is the starter grassland part of Tamberlough? We recommend: group the biome regions into six named
   regions by hand in a small table in world.json (an editable file), and treat the starter grassland as the edge of
   Tamberlough where its highland meets the coast.
3. **A new world per restart?** The same seed gives the same world; does a restart keep the seed, or does a reset
   (setting.md section 2) pick a new one? We recommend: the seed is in the server config; a reset changes it.
4. **Island size:** about 3 x 2 km (e). Larger means longer walks between regions and a longer bake at the join. We
   recommend starting at this size and measuring.
5. **Bold regions' content:** walkable land with biome materials, forests and rock only (Decision 4). Enough until
   worldgen reaches them? We recommend yes.
6. **Hollowkeep's site on the drawn map:** answered (owner's answer 9): a spur of the eastern ridge.

## Sources

The findings and links are in [map-and-castle-research.md](../../../docs/world/map-and-castle-research.md): castle siting
(sections 2-3), climate parameters, Voronoi regions, hydrology-first rivers, stream power, droplet erosion, strata
and site scoring (section 4). Priority-flood depression filling is the standard method of filling sinks in a grid
DEM (Barnes, Lehman and Mulla, 2014).
