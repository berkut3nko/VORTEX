# VORTEX documents

The index of the engine's documents: what each one is, what to read for a task, where a stage ID or a name quoted in
a code comment lives, and the history folder. Sizes (KiB) were measured on 2026-11-18, after the WP3 rewrite.

Rules every document follows:
- **Current documents** (everything outside `history/`) describe the system as it is. A stage's state lives in one
  place: the "Stages" row of its design document. Milestone state, the order of work, the test count and measured
  numbers live in [ROADMAP.md](ROADMAP.md).
- **History** (`history/<topic>/`) is append-only: plans, amendments, as-built sections, reviews, rechecks, owner runs
  and stage logs, moved verbatim with a provenance line. Corrections go only under "Errata" at the end of a file.
- Current text cites code by file and symbol, never by line number. Unbuilt designs keep their `file:line` references
  and name the commit they were read at.
- Never rename a heading or a numbered item that code cites (see "Anchors cited from code").
- Current documents carry one Obsidian navigation line (`[[...]]` wiki links, relative to the file) under the title;
  the vault's map is the game repository's `00 Docs Map.md`. A link there names a current file, never a history one.

## Documents

| File | Kind | Status | Checked at engine | KiB |
|---|---|---|---|---|
| [README.md](README.md) | index | current | `430fde0` | 28 |
| [../README.md](../README.md) | entry point: libraries, tools, use from a game, build | current | `430fde0` | 4 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | current spec: design and contracts, sections 1-11 | current | `430fde0` | 109 |
| [ROADMAP.md](ROADMAP.md) | plan: Now / Next / Later, state, measured values, direction, milestones, owner decisions | current | `430fde0` | 30 |
| [design/networking.md](design/networking.md) | decision + current spec (M1 sessions) | built | `430fde0` | 53 |
| [design/shred.md](design/shred.md) | decision + current spec + plan (destruction; structural integrity plan SI-1..3) | partly built | `430fde0` | 145 |
| [design/collapse.md](design/collapse.md) | decision (object collapse) | partly superseded by shred.md, nothing built here | `430fde0` | 16 |
| [design/view-distance.md](design/view-distance.md) | decision + current spec (full-height window, voxel size, LOD path) | partly built | `430fde0` | 34 |
| [design/game-ui.md](design/game-ui.md) | current spec (HUD, pause menu, cursor capture) | built | `430fde0` | 17 |
| [design/perf.md](design/perf.md) | plan (frame rate on the UHD 620) | designed | code read at `0388a78` | 28 |
| [design/lod.md](design/lod.md) | plan (distance budgets, terrain LOD levels) | designed | code read at `ec073a8` | 17 |
| [design/fx-budget.md](design/fx-budget.md) | plan (effects in fights) | designed | code read at `ec073a8` | 17 |
| [design/fog.md](design/fog.md) | decision + plan (volumetric fog) | designed | code read at `71dc23e` | 52 |
| [design/mechanisms.md](design/mechanisms.md) | decision + plan (use key, doors, gates, ropes) | designed | code read on 2026-11-17 (committed as `e21a84d`) | 82 |
| [design/spread.md](design/spread.md) | decision + plan (lockstep fire and liquid, no randomness) | designed | code read at `e21a84d` | 73 |
| [design/worldgen.md](design/worldgen.md) | decision + plan (regions built from structures) | designed | code read on 2026-11-17 (committed as `c970f16`) | 74 |
| [design/rendezvous.md](design/rendezvous.md) | decision + plan (internet sessions without a public IP: STUN, UDP hole punching, Cloudflare D1) | designed | code read at `a23af7a` (committed as `84ac7e7`) | 36 |
| [notes/core-fx-editor.md](notes/core-fx-editor.md) | notes: map, invariants, traps, recipes, verifying | current | `430fde0` | 33 |
| [notes/graphics.md](notes/graphics.md) | notes | current | `430fde0` | 25 |
| [notes/net.md](notes/net.md) | notes (keeps the wire-value lists) | current | `430fde0` | 59 |
| [notes/physics.md](notes/physics.md) | notes | current | `430fde0` | 31 |
| [notes/scene-engine.md](notes/scene-engine.md) | notes | current | `430fde0` | 50 |
| [notes/voxel.md](notes/voxel.md) | notes | current | `430fde0` | 58 |
| [history/](history/) | history (see "History") | append-only | - | 1286 (52 files) |
| [../CloseIssue.md](../CloseIssue.md) | other: a git checklist for closing an issue branch | kept until the owner decides (ROADMAP "Owner decisions pending") | - | 0.3 |

API reference: run `doxygen` in `engine/`; it writes HTML to `engine/build/doxygen/html/` (untracked; nothing generated is tracked).

## Reading paths

Read the game's `CLAUDE.md` first in every case (verify gates, contracts, lessons; 25 KiB). Then ("KiB" is the sum of
the files and sections named, measured on 2026-11-18, CLAUDE.md not included):

| Task | Read | KiB |
|---|---|---|
| Network change (wire, server, client, sync, bots) | [design/networking.md](design/networking.md); [notes/net.md](notes/net.md); [ARCHITECTURE.md](ARCHITECTURE.md) 7.5; for a measured number [ROADMAP.md](ROADMAP.md) "Performance and measured values"; for why a rule exists the stage file in `history/networking/` | 128 |
| Destruction change (blast, islands, load check, rubble, pieces) | [design/shred.md](design/shred.md) Part 2 (R1-R7), Part 4, Part 4b, "Stages", the structural integrity plan; [ARCHITECTURE.md](ARCHITECTURE.md) 7.2-7.4; [notes/voxel.md](notes/voxel.md), [notes/scene-engine.md](notes/scene-engine.md); networking.md "Interaction with shred.md" | 244 |
| Renderer change (passes, shaders, frame cost) | [ARCHITECTURE.md](ARCHITECTURE.md) 4.5 and 5; [notes/graphics.md](notes/graphics.md); [design/perf.md](design/perf.md); [design/view-distance.md](design/view-distance.md); [design/lod.md](design/lod.md) or [design/fog.md](design/fog.md) when the work is theirs | 106 (without lod.md / fog.md) |
| Physics change (bodies, colliders, characters) | [ARCHITECTURE.md](ARCHITECTURE.md) 6 and 7.3; [notes/physics.md](notes/physics.md); networking.md "Player movement" and "Entity replication"; shred.md SI-2 for impacts | 68 |
| World generation (terrain, regions, templates) | [design/worldgen.md](design/worldgen.md); [ARCHITECTURE.md](ARCHITECTURE.md) 4.3; [notes/voxel.md](notes/voxel.md); view-distance.md "Vertical design" and "Voxel size choice"; shred.md Part 4b (what the load check releases); the game's `docs/world/setting.md` for names and lore | 228 |
| Game interface (HUD, menu, input, cursor) | [design/game-ui.md](design/game-ui.md); [ARCHITECTURE.md](ARCHITECTURE.md) 11; [notes/core-fx-editor.md](notes/core-fx-editor.md) | 53 |
| A new design | [ROADMAP.md](ROADMAP.md) "Now" / "Next" (where it fits); the design documents it touches (their "Related" lines); one built design as a model of the form, e.g. [design/networking.md](design/networking.md) | 83 plus the documents it touches |

A new design document starts with the standard header (a table of Status / Decided / Next / Open / History / Marks,
then one "Related" line), and its body ends with "Stages" (ID | Stage | State | Commits (engine / game) | History),
"Rejected alternatives" and "Open questions". Stage IDs go in square brackets, `[net-7b]`. State words: **Built and
reviewed**, **Built**, **Designed**, **Not started**, **Superseded**. Add the new IDs to the table below.

## Stage IDs

Every ID points to the stage row of its design document, which holds the state and the commits. networking.md,
shred.md and view-distance.md carry the bracketed IDs in their "Stages" rows; in the other documents an ID maps to
the numbered stage of its "Stages" section or table (`[fog-2]` = fog.md "Stages" item 2, `[collapse-3]` =
collapse.md item 3, `[spread-S2]` = spread.md row S2, `[mech-3]` = mechanisms.md row 3, `[wg-W2]` = worldgen.md
"Staged delivery" W2, `[si-2]` = shred.md "SI-2"); superseded IDs point to the history file named in the table. "-" = no history
file (nothing built or planned in detail yet).

### Networking ([design/networking.md](design/networking.md#stages), M1)

| ID | Stage | History |
|---|---|---|
| `[net-0]` | Determinism floor, hashes, golden generator tests | [stage-0-starting-point.md](history/networking/stage-0-starting-point.md) |
| `[net-1]` | `VortexSim`, `sim::Simulation` with a numbered tick | [stage-1.md](history/networking/stage-1.md) |
| `[net-2]` | Quantised world operations, `sim::Blast` + `PresentBlast`, crumbs | [stage-2.md](history/networking/stage-2.md) |
| `[net-3]` | `VortexNet`, server and client in one process, `vortex_server` | [stage-3.md](history/networking/stage-3.md) |
| `[net-4]` | Players: `StepPlayer`, inputs, snapshots, prediction | [stage-4.md](history/networking/stage-4.md) |
| `[net-5]` | ENet, the game's `--connect`, cast queue | [stage-5.md](history/networking/stage-5.md), [owner-runs.md](history/networking/owner-runs.md) |
| `[net-6]` | Replicated bodies (`NetId`, rest / wake, caps) | [stage-6.md](history/networking/stage-6.md) |
| `[net-7a]` | Chunk sync: join by chunk sync (M-a), moving client window (M-b) | [stage-7a.md](history/networking/stage-7a.md) |
| `[net-7b]` | Checks and repair (M-a), server terrain mode (M-b) | [stage-7b.md](history/networking/stage-7b.md) |
| `[net-8]` | `vortex_bot`, soak, timeline, stalls removed, measurements | [stage-8.md](history/networking/stage-8.md) |
| `[net-9]` | On-demand server world, 1 km zones | - |

### Internet sessions ([design/rendezvous.md](design/rendezvous.md#stages))

| ID | Stage | History |
|---|---|---|
| `[nat-R0]` | NAT probe and pair test (STUN, hole punching, no Worker) | [design-review-2026-11-19.md](history/rendezvous/design-review-2026-11-19.md) |
| `[nat-R1]` | Rendezvous: Cloudflare Worker + D1, `--rendezvous` / `--join`, gate G20 | same |
| `[nat-R2]` | A relay, only for pairs R0 finds cannot meet | same |

Also: [owner-questions-2026-11-11.md](history/networking/owner-questions-2026-11-11.md) (the M1 questions as put to
the owner).

### Destruction ([design/shred.md](design/shred.md#stages); [design/collapse.md](design/collapse.md#stages))

| ID | Stage | History |
|---|---|---|
| `[shred-0]` | Measure + gather by chunk | [stage-0-1e.md](history/shred/stage-0-1e.md) |
| `[shred-1]` | Face flood (the "second shot" fix) | [stage-0-1e.md](history/shred/stage-0-1e.md) |
| `[shred-1b]` | Carve replay before a late search | [stage-0-1e.md](history/shred/stage-0-1e.md) |
| `[shred-1c]` | Nothing silent: counters, retry | [stage-0-1e.md](history/shred/stage-0-1e.md) |
| `[shred-1d]` | Physics after destruction | [stage-0-1e.md](history/shred/stage-0-1e.md) |
| `[shred-1e]` | Rubble budget | [stage-0-1e.md](history/shred/stage-0-1e.md) |
| `[shred-2]` | Crumble profiles, dust, carve lumps (client) | - |
| `[shred-3]` | `MaterialPhysicsQ`, fracture | - |
| `[shred-4]` | Cell graph search | [stage-4.md](history/shred/stage-4.md) |
| `[shred-4b]` | Weak necks: the load check (Part 4b) | [stage-4b.md](history/shred/stage-4b.md), [owner-questions.md](history/shred/owner-questions.md) |
| `[shred-4c]` | Castle bricks (owner question 2 of Part 4b) | [owner-questions.md](history/shred/owner-questions.md) |
| `[shred-5]`, `[shred-5b]`, `[shred-6]` | Registry and solver, collapse on objects, impacts: replaced by the structural integrity plan (`[si-1]`..`[si-3]`) | [part-5-impacts-2026-11-12.md](history/shred/part-5-impacts-2026-11-12.md) (stage 6's old Part 5) |
| `[shred-7]` | Optional: `ShredSphere`, `DetachFractured`, bending | [stage-4.md](history/shred/stage-4.md) ("Optional later: load flow", resolved by 4b) |
| `[si-1]` | Objects break under their own weight | [si-plan-review-2026-11-17.md](history/shred/si-plan-review-2026-11-17.md) |
| `[si-2]` | Impacts (inertial contact), crush damage | [si-plan-review-2026-11-17.md](history/shred/si-plan-review-2026-11-17.md) |
| `[si-3]` | Re-checks away from casts | [si-plan-review-2026-11-17.md](history/shred/si-plan-review-2026-11-17.md) |
| `[collapse-1]`..`[collapse-5]` | Anchored remainder; graph core; graph in `Scene::Damage`; no whole-object work per hit; terrain structures (stage 1 is ROADMAP M3 / SI-1; the data format was replaced by the stage-4 node list) | - |

### View distance ([design/view-distance.md](design/view-distance.md#stages))

| ID | Stage | History |
|---|---|---|
| `[vd-1]` | Full-height window | [stage-1.md](history/view-distance/stage-1.md) |
| `[vd-2]` | Voxel preview (`--voxel-size`, furniture probe) | [stage-2.md](history/view-distance/stage-2.md) |
| `[vd-3]` | Switch to 0.125 m | [stage-3.md](history/view-distance/stage-3.md) |
| `[vd-4]` | Distance budgets (= `[lod-1]` = `[perf-L1]`) | - |
| `[vd-5]` | Exact visibility flood | - |
| `[vd-6]` | LOD level 1 at 0.25 m (= `[lod-2]`, `[lod-3]`) | - |
| `[vd-7]` | Presets and level 2 | - |

Also: [owner-questions-2026-11-11.md](history/view-distance/owner-questions-2026-11-11.md).

### Rendering and effects

| ID | Stage | Design | History |
|---|---|---|---|
| `[perf-A]` | Block A: M0, P1, G1, G2 (perf stages 1-3) plus a HUD settings page; ROADMAP `[wp-4]` | [perf.md](design/perf.md#stages) | - |
| `[perf-B]` | Block B: L1, H1 (perf stages 5-6) | [perf.md](design/perf.md#stages) | - |
| `[perf-<item>]` | One item of the ranked list: M0, P1, G1, S1, G2, T1a, H1, L1, G3, T1b, B1, U1, B2, C4, C3, C5, B5, A5, B4, X1, X2, F1, I1, J1, C6 | [perf.md](design/perf.md#ranked-list) | - |
| `[lod-1]`..`[lod-5]` | Distance budgets; one terrain level; edits and holes; levels 2-3; multiplayer | [lod.md](design/lod.md#stages) | - |
| `[fx-1]`..`[fx-6]` | Measure; cheap wins; `FxDirector`; collider churn; authoring; only if measured | [fx-budget.md](design/fx-budget.md#stages) | - |
| `[fog-1]`..`[fog-4]` | Analytic extras; terrain map and fog pass; the simulation; only if the owner asks | [fog.md](design/fog.md#stages) | [design-review-2026-11-17.md](history/fog/design-review-2026-11-17.md) |

### Gameplay and world

| ID | Stage | Design | History |
|---|---|---|---|
| `[mech-1]`..`[mech-5]` | Door and gate; ropes; siege kit; placement; later (after M2) | [mechanisms.md](design/mechanisms.md#stages) | [design-review-2026-11-17.md](history/mechanisms/design-review-2026-11-17.md) |
| `[spread-S1]`..`[spread-S6]` | Fire offline; fire in a session; collapse from fire; liquid; presentation; tuning | [spread.md](design/spread.md), "Stages" | [proposal-scores-2026-11-17.md](history/spread/proposal-scores-2026-11-17.md), [design-review-2026-11-17.md](history/spread/design-review-2026-11-17.md) |
| `[wg-W1]`..`[wg-W6]` | Stamped village offline; in a session; forts and fights; materials, history, second kit; zones; reach | [worldgen.md](design/worldgen.md#staged-delivery) | [how-it-was-chosen-2026-11-17.md](history/worldgen/how-it-was-chosen-2026-11-17.md) |

### Work packages ([ROADMAP.md](ROADMAP.md#now) "Now" / "Next")

The project review of 2026-11-17 (game repository, `docs/history/reviews/project-review-2026-11-17.md`, section 4)
defines them: `[wp-1]` safety and the stuck-at-the-edge fix; `[wp-2]` `tools/verify.sh`; `[wp-3]` the document
rewrite; `[wp-4]` frame-rate block A + settings (`[perf-A]`); `[wp-5]` visible play boundary; `[wp-6]` rules seam +
net split; `[wp-7]` M2a "first fight"; `[wp-8]` fight measurement (`[fx-1]`, `[fx-2]`); `[wp-9]` = `[si-2]`;
`[wp-10]` = `[fog-1]`; `[wp-11]` = `[wg-W1]`. History: [m1-stage-log.md](history/roadmap/m1-stage-log.md) (M1's stage
log), [drafts-2026-11-12.md](history/roadmap/drafts-2026-11-12.md).

## Anchors cited from code

Code comments quote document names, headings and numbered items. Each must resolve. Regenerate the list with

```
grep -rnoE '[A-Za-z/-]+\.md[^)]{0,60}' engine/src engine/tests engine/tools engine/shaders src tools
grep -rnoiE '(review|recheck) of stage [0-9a-e]+|stage [0-9][a-e]?( M-[ab])?' engine/src engine/tests engine/tools engine/shaders src tools
```

from the game root (the first counts each citation by file name with `sed -E 's/^[^:]+:[0-9]+:([A-Za-z/-]+\.md).*/\1/'
| sed 's#.*/##' | sort | uniq -c`). On the code of engine `430fde0` / game `10197d2` the first finds 325 citations (networking.md 173,
shred.md 116, view-distance.md 21, ARCHITECTURE.md 5, collapse.md 4, game-ui.md 4, notes/voxel.md 1,
two-pc-test-windows.md 1) and the second 761 stage tokens.

| Cited as | Lives in |
|---|---|
| networking.md "Time and ticks", "Transport", "World operations" (and its numbered items), "Chunk sync", "Player movement" (and its bold bullet "The play area"), "Entity replication", "Region start-up" / "Region start-up and terrain agreement", "Known risks of terrain agreement after stage 0", "Server world without a camera", "Testing" | [design/networking.md](design/networking.md) (headings kept) |
| networking.md R5 (the cast work budget) | networking.md "Time and ticks", and shred.md Part 2 R5 |
| networking.md "owner decision 6" (the replicated threshold) | networking.md "Owner decisions" 6 (and 7, its 2026-11-16 answer) |
| networking.md "stage N" with no section name; "networking stage 7" (before the split into 7a / 7b) | networking.md "Stages" row `[net-N]`; stage 7 = `[net-7a]` + `[net-7b]` |
| networking.md "the tick-rate scale" | networking.md "Architecture" (`Engine::SetTickSource`); history [stage-1.md](history/networking/stage-1.md), [stage-5.md](history/networking/stage-5.md) |
| networking.md "stage 0" | [stage-0-starting-point.md](history/networking/stage-0-starting-point.md); networking.md "Known risks of terrain agreement after stage 0" |
| networking.md "Stage 1 as built" | [history/networking/stage-1.md](history/networking/stage-1.md) |
| networking.md stage 2 (world operations, crumbs, "Changed by shred.md ...") | [history/networking/stage-2.md](history/networking/stage-2.md) |
| networking.md "Stage 3 as built", "Review of stage 3" | [history/networking/stage-3.md](history/networking/stage-3.md) |
| networking.md "Stage 4 as built", "Review of stage 4" | [history/networking/stage-4.md](history/networking/stage-4.md) |
| networking.md "Stage 5 as built", "Review of stage 5" | [history/networking/stage-5.md](history/networking/stage-5.md) |
| networking.md "Stage 6 as built", "Review of stage 6" | [history/networking/stage-6.md](history/networking/stage-6.md) |
| networking.md "Stage 7a plan" (its amendments 1-12, "The moving client window", milestone M-b), "stage 7a M-b" ("M-b as built"), "Review of stage 7a", "Recheck of stage 7a" | [history/networking/stage-7a.md](history/networking/stage-7a.md) |
| networking.md "Stage 7b plan" (its choices, amendments, Tests, "`HashCheck`: server", "`HashCheck`: client", "Wire `EntityCheck`"), "stage 7b M-b" ("Stage 7b M-b as built"), "Review of stage 7b", "Recheck of stage 7b" | [history/networking/stage-7b.md](history/networking/stage-7b.md) |
| networking.md "Stage 8", "Stage 8 as built" (items 9 and 13; O1-O11, e.g. O2 / O3 / O4 / O5), "Stage 8 measurements", plan amendments (A1-A16), "Review of stage 8" (R1-R20), "Recheck of stage 8" | [history/networking/stage-8.md](history/networking/stage-8.md) |
| networking.md stage 9 | networking.md "Stages" row `[net-9]` |
| shred.md Part 2 R1-R7 | [design/shred.md](design/shred.md) "Part 2 - Rules every later stage follows" |
| shred.md stage 1 rules 1-9 ("rule 6", "stage 1, rule 7") | shred.md "Fix: prove face-touching components by an outward flood (stage 1)" |
| shred.md Part 4 ("largest and lowest first", `serverBodiesPerTick`, at most 2 new server bodies per tick) | shred.md "Part 4 - Structural updates after destruction" |
| shred.md Part 4b, "The rule, exactly", "The narrowest plane", steps and phases ("step 2", "step 4", "phase 3b"), "Work bound per cast", failure modes ("failure mode 3") | shred.md "Part 4b - Weak necks (load check)" (headings and numbering kept) |
| shred.md "Other causes checked", "Tests for stage 1", stage 0 / 1 / 1b / 1c / 1d / 1e, "Stages 1d-1e as built" | [history/shred/stage-0-1e.md](history/shred/stage-0-1e.md) |
| shred.md "open question 7" (`Blast.cppm`, `kNeckFlowWorkBudget`) | Part 4b's question 7 (the flow budget): [history/shred/owner-questions.md](history/shred/owner-questions.md) item 7; decided in shred.md header "Decided" (Us, 2026-11-13, in [net-3]). Not shred.md's current "Open questions for the owner" item 7 |
| shred.md stage 4, "Stage 4 as built" | [history/shred/stage-4.md](history/shred/stage-4.md); row `[shred-4]` |
| shred.md "Stage 4b review" (= "Review of stage 4b"), stage 4b | [history/shred/stage-4b.md](history/shred/stage-4b.md); row `[shred-4b]` |
| shred.md "stage 7, owner" | shred.md "Stages" row `[shred-7]`; [history/shred/stage-4.md](history/shred/stage-4.md) "Optional later: load flow" |
| view-distance.md "Stage 1 as built", "as built", "stage 1 gates", stage 1 | [history/view-distance/stage-1.md](history/view-distance/stage-1.md); row `[vd-1]` |
| view-distance.md stage 2 (voxel preview, `--probe`) | [history/view-distance/stage-2.md](history/view-distance/stage-2.md); row `[vd-2]` |
| view-distance.md stage 3 | [history/view-distance/stage-3.md](history/view-distance/stage-3.md); row `[vd-3]` |
| collapse.md (node merging, `SupportCell`, connectivity claim) | [design/collapse.md](design/collapse.md) "How it works" (body kept) |
| ARCHITECTURE.md "Rendering", 5.1, 5.2, 5.3, 5.6; section 8 (from `engine/README.md`); section 11 | [ARCHITECTURE.md](ARCHITECTURE.md) (section numbers kept) |
| game-ui.md | [design/game-ui.md](design/game-ui.md) |
| notes/voxel.md | [notes/voxel.md](notes/voxel.md) |
| docs/two-pc-test-windows.md 4.9 | the game repository's two-PC guide (name and section 4.9 kept) |

Bare stage tokens with no document name (the second grep) are read by their context: a token in `src/net`,
`tests/net` or `tools/` is networking; 1b-1e and 4b are shred.md; "stage 4" next to a graph or a search is shred.md
`[shred-4]`; a "stage 1 / 2 / 3" next to the window, `--voxel-size` or the probe is view-distance.md. Counts on the
code above: 7b 103, 6 85, 8 81, 7a 81, review of 7a 48, 4 40, review of 7b 40, review of 6 35, 5 28, review of 8 28,
3 25, 7b M-b 23, 1 16, review of 5 16, review of 4 16, recheck of 8 15, recheck of 7a 13, 7a M-b 12, 1e 11, 7 10,
1d 9, 4b 7, 0 6, 2 4, 1c 3, recheck of 7b 3, 9 2, 1b 1.

## History

`history/<topic>/<file>`: each file starts with the line it was moved from (`Moved verbatim from ... at engine
430fde0 on 2026-11-18`). Source lines are of engine `430fde0`. A `before-rewrite-2026-11-18.md` holds the replaced
header and the original of every section that was kept but changed in the WP3 rewrite.

| File | Source | Lines |
|---|---|---|
| [architecture/before-rewrite-2026-11-18.md](history/architecture/before-rewrite-2026-11-18.md) | ARCHITECTURE.md | 1-112, 141-214, 683-729, 780-964, 1037-1165 |
| [collapse/before-rewrite-2026-11-18.md](history/collapse/before-rewrite-2026-11-18.md) | design/collapse.md | 1-15 |
| [fog/before-rewrite-2026-11-18.md](history/fog/before-rewrite-2026-11-18.md) | design/fog.md | 1-15 |
| [fog/design-review-2026-11-17.md](history/fog/design-review-2026-11-17.md) | design/fog.md "Review of the design (critic, 2026-11-17)" | 493-513 |
| [fx-budget/before-rewrite-2026-11-18.md](history/fx-budget/before-rewrite-2026-11-18.md) | design/fx-budget.md | 1-6, 8-31 |
| [game-ui/before-rewrite-2026-11-18.md](history/game-ui/before-rewrite-2026-11-18.md) | design/game-ui.md (header; "Owner checks", now the game's `docs/owner-checks.md`) | 1-9, 245-300 |
| [lod/before-rewrite-2026-11-18.md](history/lod/before-rewrite-2026-11-18.md) | design/lod.md | 1-9 |
| [mechanisms/before-rewrite-2026-11-18.md](history/mechanisms/before-rewrite-2026-11-18.md) | design/mechanisms.md | 1-13, 43-62, 65-83, 166-187, 383-466, 780-812 |
| [mechanisms/design-review-2026-11-17.md](history/mechanisms/design-review-2026-11-17.md) | design/mechanisms.md "Review of the design" | 866-897 |
| [networking/before-rewrite-2026-11-18.md](history/networking/before-rewrite-2026-11-18.md) | design/networking.md | 2-21, 22-44, 63-87, 88-103, 104-125, 126-151, 152-165, 166-194, 195-219, 220-243, 244-274, 275-300, 301-321, 322-337, 3169-3180 |
| [networking/stage-0-starting-point.md](history/networking/stage-0-starting-point.md) | design/networking.md "What the code gives us today and what blocks us" (code at `ac17bcf`) | 45-62 |
| [networking/stage-1.md](history/networking/stage-1.md) | design/networking.md "Stage 1 as built" | 338-360 |
| [networking/stage-2.md](history/networking/stage-2.md) | design/networking.md "Stage 2 as built", crumbs, "Changed by shred.md ..." | 361-489 |
| [networking/stage-3.md](history/networking/stage-3.md) | design/networking.md "Stage 3 as built", "Review of stage 3" | 490-595 |
| [networking/stage-4.md](history/networking/stage-4.md) | design/networking.md "Stage 4 as built", "Review of stage 4" | 596-695 |
| [networking/stage-5.md](history/networking/stage-5.md) | design/networking.md "Stage 5 as built", "Review of stage 5" | 696-820 |
| [networking/stage-6.md](history/networking/stage-6.md) | design/networking.md "Stage 6 as built", "Review of stage 6" | 821-958 |
| [networking/stage-7a.md](history/networking/stage-7a.md) | design/networking.md stage 7a: plan, amendments, as built, M-b, integration, review, recheck | 959-1626 |
| [networking/stage-7b.md](history/networking/stage-7b.md) | design/networking.md stage 7b: plan, amendments, as built, M-a / M-b, integration, review, recheck | 1627-2340 |
| [networking/owner-runs.md](history/networking/owner-runs.md) | design/networking.md "Owner's two-machine run (2026-11-17)" | 2341-2383 |
| [networking/stage-8.md](history/networking/stage-8.md) | design/networking.md stage 8: plan, amendments, as built, measurements, review, recheck | 2384-3147 |
| [networking/owner-questions-2026-11-11.md](history/networking/owner-questions-2026-11-11.md) | design/networking.md "Questions as they were put to the owner" | 3181-3195 |
| [notes/core-fx-editor-2026-11-18.md](history/notes/core-fx-editor-2026-11-18.md) | notes/core-fx-editor.md (whole file) | 1-268 |
| [notes/graphics-2026-11-18.md](history/notes/graphics-2026-11-18.md) | notes/graphics.md (whole file) | 1-283 |
| [notes/net-2026-11-18.md](history/notes/net-2026-11-18.md) | notes/net.md (whole file) | 1-604 |
| [notes/physics-2026-11-18.md](history/notes/physics-2026-11-18.md) | notes/physics.md (whole file) | 1-364 |
| [notes/scene-engine-2026-11-18.md](history/notes/scene-engine-2026-11-18.md) | notes/scene-engine.md (whole file) | 1-569 |
| [notes/voxel-2026-11-18.md](history/notes/voxel-2026-11-18.md) | notes/voxel.md (whole file) | 1-552 |
| [perf/before-rewrite-2026-11-18.md](history/perf/before-rewrite-2026-11-18.md) | design/perf.md | 1-14, 28-46, 64-78, 80-114, 116-233, 261-272, 274-287 |
| [readme/before-rewrite-2026-11-18.md](history/readme/before-rewrite-2026-11-18.md) | `engine/README.md` (whole file) | 1-45 |
| [roadmap/before-rewrite-2026-11-18.md](history/roadmap/before-rewrite-2026-11-18.md) | ROADMAP.md (intro, "Working now", performance, direction, milestones, "Done so far, with gaps", "Planned, not built", later lists) | 1-92, 446-543, 632-668 |
| [roadmap/m1-stage-log.md](history/roadmap/m1-stage-log.md) | ROADMAP.md M1 "State" stage log | 93-445 |
| [roadmap/drafts-2026-11-12.md](history/roadmap/drafts-2026-11-12.md) | ROADMAP.md "Drafts awaiting the owner (2026-11-12)" and the view-distance bullet | 544-631 |
| [shred/before-rewrite-2026-11-18.md](history/shred/before-rewrite-2026-11-18.md) | design/shred.md | 2-32, 33-63, 64-76, 126-203, 218-233, 458-493, 494-543, 544-574, 694-733, 745-759, 811-835, 858-889, 890-918, 919-956, 957-1031, 1032-1044, 1055-1081, 1150-1169, 1170-1209, 1441-1486, 1487-1508, 1509-1522, 1562-1574, 2057-2076, 2077-2104 |
| [shred/stage-0-1e.md](history/shred/stage-0-1e.md) | design/shred.md Part 1, server loading, cost, stages 1b-1e plan, tests, as built, review | 77-125, 204-217, 234-457 |
| [shred/stage-4.md](history/shred/stage-4.md) | design/shred.md "Stage 4 as built", "Review of stage 4", "Optional later: load flow" | 575-693, 734-744 |
| [shred/stage-4b.md](history/shred/stage-4b.md) | design/shred.md judgement of the three designs, code changes, tests, "Stage 4b as built", "Review of stage 4b" | 788-810, 1082-1149, 1258-1397 |
| [shred/owner-questions.md](history/shred/owner-questions.md) | design/shred.md "Open questions for the owner (Part 4b)" | 1225-1257 |
| [shred/part-5-impacts-2026-11-12.md](history/shred/part-5-impacts-2026-11-12.md) | design/shred.md "Part 5 - Impact destruction by estimated weight (stage 6)" | 1398-1440 |
| [shred/si-plan-review-2026-11-17.md](history/shred/si-plan-review-2026-11-17.md) | design/shred.md "Review of the plan (2026-11-17)" | 2000-2026 |
| [spread/before-rewrite-2026-11-18.md](history/spread/before-rewrite-2026-11-18.md) | design/spread.md | 1-21, 580-603, 637-656 |
| [spread/proposal-scores-2026-11-17.md](history/spread/proposal-scores-2026-11-17.md) | design/spread.md "Scores of the two proposals" | 83-132 |
| [spread/design-review-2026-11-17.md](history/spread/design-review-2026-11-17.md) | design/spread.md "Review of the design" | 932-960 |
| [view-distance/before-rewrite-2026-11-18.md](history/view-distance/before-rewrite-2026-11-18.md) | design/view-distance.md | 1-11, 12-29, 31-40, 42-123, 125-141, 143-154, 182-233, 235-246 |
| [view-distance/owner-questions-2026-11-11.md](history/view-distance/owner-questions-2026-11-11.md) | design/view-distance.md "Open questions for the owner" | 263-277 |
| [view-distance/stage-1.md](history/view-distance/stage-1.md) | design/view-distance.md "Stage 1 as built", acceptance, review fixes, still open | 278-383 |
| [view-distance/stage-2.md](history/view-distance/stage-2.md) | design/view-distance.md "Stage 2 as built" | 384-486 |
| [view-distance/stage-3.md](history/view-distance/stage-3.md) | design/view-distance.md "Stage 3 as built" | 487-592 |
| [worldgen/before-rewrite-2026-11-18.md](history/worldgen/before-rewrite-2026-11-18.md) | design/worldgen.md | 1-12, 162-175 |
| [worldgen/how-it-was-chosen-2026-11-17.md](history/worldgen/how-it-was-chosen-2026-11-17.md) | design/worldgen.md "How it was chosen" | 89-130 |
| [rendezvous/design-review-2026-11-19.md](history/rendezvous/design-review-2026-11-19.md) | written with design/rendezvous.md (engine `84ac7e7`), not moved: the review's and the recheck's findings | - |
| [surveys/web-survey-2026-11-21.md](history/surveys/web-survey-2026-11-21.md) | the "(web survey, 2026-11-21)" sections the owner added in engine `4f7c3fe` (merged 2026-11-21) to ARCHITECTURE.md, ROADMAP.md and design/networking, shred, collapse, view-distance, lod, fx-budget, perf, game-ui, worldgen.md; with a note on the claims an audit found false or unverifiable | lines at `4f7c3fe`: ARCHITECTURE 10-34, ROADMAP 8-26, each design document 5-7 to 5-9 |

The game repository keeps its own history in `docs/history/` (game commit `6151db6`): the WP3 rewrite plan
(`REWRITE-PLAN.md`), the CLAUDE.md snapshot and its rule mapping (`claude-md/CLAUDE-2026-11-18.md`,
`claude-md/rule-mapping.md`), the old README (`claude-md/README-2026-11-18.md`), the two-PC guide's replaced sections
(`two-pc/before-rewrite-2026-11-18.md`), the setting's header if it changed (`world/before-rewrite-2026-11-18.md`) and
the project review of 2026-11-17 (`reviews/project-review-2026-11-17.md`).
