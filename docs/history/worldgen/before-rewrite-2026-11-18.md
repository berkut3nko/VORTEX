# World-generation history: text replaced or changed by the WP3 rewrite

Moved verbatim from `docs/design/worldgen.md` lines 1-12 and 162-175 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

The original text of every section of the current document that the rewrite replaced or changed, in source order. The
current document holds the rewritten version.

---

# World generation: regions built from structures

Status: design decision, 2026-11-17. Nothing is built or measured for it. Figures marked *(est.)* are estimates from
operation counts or proportions and must be measured. Every stage names the measurement that confirms or kills them.
Claims about today's code were checked against the sources (`voxel/Generator.cppm`, `sim/Region.cppm`,
`sim/Blast.cppm`, `sim/internal/Blast.cpp`, `sim/internal/Region.cpp`, `sim/Player.cppm`, `voxel/Connectivity.cppm`,
`voxel/Structure.cppm`, `voxel/internal/Material.cpp`, `net/Messages.cppm`) on 2026-11-17, and again after the review
of the same day (flood box centred on the cast, release cap per cast, island-box locality, 6-connected nodes, the
margin and the client window).
Other agents are changing `engine/src/net` and networking.md while this is written: every wire detail below
(enum values, the protocol number, message ids) is taken at merge time, never assumed.

<!-- source lines 162-175 -->

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
