# VORTEX roadmap

[[../../00 Docs Map|Docs map]] · [[README|docs index]] · [[ARCHITECTURE|VORTEX architecture]]

What to build next, what exists today, what it costs, and what the owner decided. It is the one home of the
milestone state, the work queue, the test count and the measured reference values. Per-stage state lives in each
design document's "Stages" table; [ARCHITECTURE.md](ARCHITECTURE.md) says how the pieces fit. History (stage logs,
earlier drafts, the text this page replaced) is in [history/roadmap/](history/roadmap/).

Order of work: the project review's default ([review](../../docs/history/reviews/project-review-2026-11-17.md),
section 4), which the owner did not object to. Under the owner's standing rules (auto-continue in this order, settle
design questions ourselves, fun with few bugs first) the next item starts when the one before it is verified and
committed. Effort is in work days, all estimates (e); calendar time has run about 6-7x faster.

## Now

- **[wp-2] `tools/verify.sh` + `tools/doc-check.sh`.** Fewer regressions reach playtests: CLAUDE.md's gates become
  assertions with exit codes (quick / full / soak), game scripts exit 3 on a failed expectation. 2-3 d (e). [Review](../../docs/history/reviews/project-review-2026-11-17.md) "WP2".
- **Owner: M1 acceptance: LAN run on stage 8 commits** (engine `430fde0` / game `10197d2`), windowed, Linux host and
  Windows laptop, the short soak as its pre-check: two-PC guide
  ([section 6.5](../../docs/two-pc-test-windows.md)).

**[wp-3] Document rewrite: done (2026-11-18), ahead of [wp-1] / [wp-2]** (it changes no code, so it collides with
neither; the order is otherwise the review's). Current documents describe the system as it is, history is moved
verbatim to `history/`; the plan and checks are in [../../docs/history/REWRITE-PLAN.md](../../docs/history/REWRITE-PLAN.md).
Its acceptance is still open and waits for [wp-2]: `tools/doc-check.sh` passes, and a fresh reader checks
[../../docs/history/claude-md/rule-mapping.md](../../docs/history/claude-md/rule-mapping.md). Commits: engine `a23af7a`,
game `1c45cd2`.

**[wp-1] Safety, hygiene and the stuck-at-the-edge fix: done (built 2026-11-19, every gate 2026-11-21).** The edge bug was real: a player who walked
about 12 m past the play bounds touched a chunk outside the server's fixed window and `StepPlayer`'s gate held it for
good, walking back included (`Movement.APlayerPastThePlayBoundsWalksBack` failed with the old rule: held at x = 23.6 m
for 888 ticks; with a moving client window 335 corrections). Now `sim::StepPlayer` keeps the feet inside
`sim::PlayAreaOf(RegionDesc, tuning)` in a session (the outward wish is cut before Jolt moves, and walls that only
characters meet stand at the play bounds, `sim::SetPlayAreaWalls`, so a slide down a crater wall across the edge
stops against them; the review's first version undid such a move and the player hung in the air for good,
`Player.AtThePlayAreasEdgeASteepFaceNeitherHoldsNorSinksTheFeet`; offline unbounded); no
`kProtocolVersion` bump (no byte changed), `sim::kPlayerRuleRevision` (1) in `net::BuildHash` instead, so builds with
and without the clamp refuse each other (a live server older than WP1 refuses WP1 clients). Also: `tools/build.sh` and
the build / run locks (`tools/lib/lock.sh`; session-check and soak run copies of the binaries), `tools/install-live.sh`
(`~/vortex-live/<commit>/`), the worktree recipe (game `docs/verify.md`); `.gitmodules` (relative URL, branch
`engine-rewrite`), `.gitignore`, the game built with the engine's warnings, a `ci` preset with -Werror, the golden test
name in `engine/CMakeLists.txt`; `--validation` without the layer exits 2; BuildInfo (`Build:` line, `--version`) in
the game, `vortex_server` and `vortex_bot`; a start-up CPU check (exit 4); `MakeGenerator()` before the session's
keep-alive thread; `VORTEX_GRAPHICS=OFF` (Core, Physics, Sim, Net, the tools and their tests without Vulkan, GLFW or
ImGui, built in a scratch directory); G4 at 170 frames (the editor smoke run reaches the import, delete and verdict).
Not done here: stage branches and tags and pushes (owner decision 1); the Windows paths
of the CPU check and of hard `--validation` are unverified until the owner builds there; owner check 15 (the play
bounds, windowed) not yet run. Gates on the final tree (the walls, the sixth object layer, the new filters and the
recheck's fixes included), 2026-11-21: G1-G19 (G6 at 0.1 and 0.15 m, G10 at W = 2, 11, 12 and W = 2 at 0.1 and
0.15 m) and the `ci` build, every one passed: G12 9.13 s, 48.5 MiB; G16 run alone three times, 2 / 0 / 5 corrections
on the walking client, max error 0.18 / 0.00 / 0.23 m; G19 `PASS` for both regions, the game client's hashes equal.
G16 had corrections before WP1 too; they are the walking client's late inputs (measured values, 2026-11-19 and
2026-11-21, from the new `Client: correction` log line); its bound is provisional. Since the recheck: check children
close the output-folder lock (fd 6), `tools/build.sh` checks `DEPS_FROM` pins under the locks. `tools/session-check.sh`'s
same-`Build:`-line verdict has no automated test, and G16's correction bound is read from the client logs by hand
(WP2). Commits: engine `be1a2ca`, game `0a99568`.

## Next

In this order:

1. **[nat-R0] NAT probe and pair test.** The owner's home server (no public IPv4) learns whether players can reach it:
   STUN, the NAT class, hole punching on ENet's socket, a pair test with no Worker; no wire change.
   [design/rendezvous.md](design/rendezvous.md), "Stages" R0. 3-4 d (e).
2. **[nat-R1] Rendezvous.** Testers outside the house join by an invite: a Cloudflare Worker + D1 (free tier) as the
   meeting point, `--rendezvous` / `--join`, gate G20. [design/rendezvous.md](design/rendezvous.md), "Stages" R1.
   6-8 d (e). [nat-R2] (a relay) waits for R0's pair results.
3. **[wp-4] Frame-rate block A + settings.** A steadier, faster frame; settings in game; every playtest leaves
   numbers. [design/perf.md](design/perf.md) stages 1-3 (M0, P1, G1 + G2), a HUD settings page. 3-4 d (e). [Review](../../docs/history/reviews/project-review-2026-11-17.md) "WP4".
4. **[wp-5] Visible play boundary + moving window on small regions.** You see where the play area ends; the
   playground shows 88 m instead of 56 m. 1-2 d (e). [Review](../../docs/history/reviews/project-review-2026-11-17.md) "WP5".
5. **[wp-6] Rules seam + net split.** No player change yet: spells and modes then come without growing `net::Server`.
   A GPU-free `sim` rules seam, then `Server.cpp` / `Client.cpp` split; no behaviour or wire change. 6-9 d (e). [Review](../../docs/history/reviews/project-review-2026-11-17.md) "WP6".
6. **[wp-7] M2a "first fight".** The sandbox becomes a PvP game: hurt, die, respawn, win a round (M2 below). The only
   protocol bump in WP4-WP8. 8-12 d (e). [Review](../../docs/history/reviews/project-review-2026-11-17.md) "WP7".
7. **[wp-8] Fight measurement + fx-budget 1-2 + 10 clients.** Fights stay playable on the UHD 620: `--stress-fx`, a
   10-bot session at 10 casts/s with measured clients, then [design/fx-budget.md](design/fx-budget.md) stages 1-2.
   5-8 d (e). [Review](../../docs/history/reviews/project-review-2026-11-17.md) "WP8".
8. **[mech-1] Door and gate.** E opens doors, a lever runs a gate, crates move to Q.
   [design/mechanisms.md](design/mechanisms.md), "Stages" row 1. Its own protocol bump, after M2a's; after WP8 so its
   yard is measured inside a known fight budget. 9-10 d (e).
9. **[mag-1] Siege core in sim, headless only.** The spell system's first four spells, Siege Cannon, Barricade Wave,
   Shockwave and Updraft (breach, cover, knock off, climb), with only what they use: the Pierce operator, the Burst
   strip, Plume's single column, `CarveCapsule` / `MapMask` / `MapShape` as sim ops, knit iron that blasts chip, with
   server-scheduled expiries, the spawn guard, Impulse and Lift zones in `sim::StepPlayer` (`kPlayerRuleRevision` 2);
   no wire change. [design/magic.md](design/magic.md), "Stages". 6-9 d (e).
10. **[mag-2] First playable spells.** The same four spells in offline practice and in sessions: the one magic
    protocol bump (it reserves every value the later magic stages need), zone events with a 0.4 s lead, the spell bar
    and the attune menu on the HUD; the owner's Felling-duel taste check. [design/magic.md](design/magic.md), "Stages".
    6-9 d (e).
11. **[si-2] Impacts with crush damage** (with the `NeckCheck` split and a chunk-source interface first). A falling
    tower cracks walls and kills. [design/shred.md](design/shred.md), "SI-2 - impacts (inertial contact)". 7-11 d (e).
12. **[fog-1] Fog stage 1.** Haze toward the sun, low mist and blast dust puffs, without losing frame rate.
    [design/fog.md](design/fog.md), "Stages" 1. About 4 d (e).
13. **[spread-S1] Fire offline.** A burning house that falls, offline only, no wire change (its smoke feeds fog later).
    [design/spread.md](design/spread.md), "Stages" S1. Large (e).
14. **[wg-W1] Stamped village.** The first real map, with towers worth felling. Soak tier (nightly) first.
    [design/worldgen.md](design/worldgen.md), "W1: a stamped village, offline". About 1.5 weeks (e).

Placement of [mech-1] and [spread-S1] was decided by us under the owner's rules; the owner may veto it. Mechanisms
change the E key and add a `PlayerButton`, so they take the protocol number after M2a's; spread S1 has no wire change,
so it fits after fog stage 1; spread S2 bumps the protocol and goes to Later.
[mag-1] and [mag-2] come right after [mech-1] (owner, 2026-11-21: "Doors first", vetoing our proposal to put them
before it): they need M2a's health, teams and charges, WP6's rules seam and [wp-8]'s fight baseline, and [mag-2]
takes the protocol number after [mech-1]'s.
[nat-R0] and [nat-R1] go right after [wp-2] (rendezvous.md "Placement", us, the owner may veto): no wire change, and
internet play lets testers outside the house try every later stage, M2a's first fight above all.

**Side task, any time:** Windows reproducibility. The owner's report (toolchain, shell, `git status` / `git diff`,
golden-test output; about 30 owner-minutes), a `windows-clang64` preset, `.gitattributes`, a one-page build guide.
0.5-1 d (e). [Review](../../docs/history/reviews/project-review-2026-11-17.md) "Side task".

## Later

In rough order:
- The rest of magic ([design/magic.md](design/magic.md), "Stages"; it follows [mag-2] and [si-2], not W1): [mag-3]
  Frost and the Shatter combo (Shatter Spike, Flash Freeze, Permafrost Climb) right after [mag-2], [mag-4] Flow,
  climbing and the rest of Metal (Molten Pour, Rising Spire, Rebar Lance, Glacier Slide) after [mag-3], [mag-7]
  Crushing Weight after [si-2] and [mag-4], [mag-5] Thermal (after [spread-S1] headless, [spread-S2] / [spread-S3]
  for play), [mag-6] presentation and tuning. None needs spread S4.
- [si-1] objects break under their own weight and [si-3] re-checks away from casts, after W1 (shred.md SI plan).
- [mech-2]..[mech-4] ropes, siege kit, placement (mechanisms.md); [spread-S2]..[spread-S6] fire in a session (a
  protocol bump), collapse from fire, liquid, presentation, tuning (spread.md).
- Frame-rate block B: [perf-L1] (= lod.md stage 1 = [vd-4] distance budgets) and [perf-H1] half-resolution lighting
  (perf.md).
- Audio first pass (presentation only); rigid-part characters with procedural animation and team colours (3-5 d (e)
  each, may slot between packages).
- [shred-2] / [shred-3] crumble profiles and fracture (shred.md); M2b's characters and training dummy; [net-9] the on-demand server world and
  1 km zones (behind M2, owner decision of 2026-11-11); [vd-5] exact visibility flood (memory); M6 ([vd-6] / [vd-7]
  LOD levels and presets, Windows and CI); M7.

Engine backlog (no order):
- **Scene:** emitter component, transform hierarchy, contact events per entity, entity save / load, a spatial index
  for `QueryBodies` (it tests every body).
- **Physics guard:** parked objects stay static bodies while the game runs (save and unload them with their chunks
  once entities serialise); a chunk that gains terrain keeps its old collider for the few frames of re-cooking, so a
  body can sink into fresh voxels; characters are held by `sim::StepPlayer`'s `HasCollider` gate, not by the guard,
  and nothing keeps colliders around a character the camera has left; bodies touching a chunk whose faces a brush or
  spell keeps uncovering wait for the stroke to pause.
- **Editor:** undo / redo, a voxel brush for objects, scale gizmo and snapping, file dialog, reusing or freeing
  palettes (every import adds one of 256 GPU materials), entity save / load ("Save world" saves terrain only when
  `EngineConfig::world.saveDirectory` is set; the game sets none), pausing the simulation, picking entities without
  a body, inspector edits of motion type, name and material, a material editor.
- **World:** clip-map LOD cascades ([design/lod.md](design/lod.md)); palette-compressed or brick-sparse chunk payloads
  (chunks are dense, 37 KiB); water beyond the animated sky-reflecting surface (ARCHITECTURE.md 5.7: no refraction,
  flow, terrain reflections, foam or underwater look; water next to a crater stands as a wall; an enclosed water
  surface lit only by local lights still reflects sky colour); transparency; biomes, structures and ore veins, a
  data-driven material and generator description (worldgen.md); background saving and save-format versioning.
- **Rendering:** reflections (metals keep 65 % diffuse instead), volumetric light shafts, bloom, depth of field;
  shadowed, lit particles; multi-bounce diffuse light; upscaling better than bilinear; shader width on Intel
  (`INTEL_DEBUG=fs`: `lighting.frag` about 3400 instructions with 9 spills and `gbuffer_world.frag` compile 8 wide
  only; getting them 16 wide is untried); textured materials.
- **Gameplay foundations:** voxel-part rigs, skeletal animation, IK; impact and break sounds per material (ids
  reserved); input action mapping and rebinding, gamepad navigation, Unicode / TTF text, textured widgets, in-game
  chat; double-precision physics for worlds far from the origin.
- **Tooling:** GCC / MSVC builds (only Clang is tested), CI with tests and a headless validation render,
  image-comparison tests, the CPU reference renderer `tools/cpu_render` (planned, not started).

## Playable today

- **Offline** (`./build/Shredborne`): walk 3.8 m/s, run 7, jump 4.6, first / third person, fly; LMB blasts a 1.2 m
  hole, E drops a crate, Esc opens the pause menu; F1 editor, F3 debug overlay. Terrain, crates and the plaza's
  buildings break; a tower falls on the shot that cuts it free, or when its neck is too thin for its weight. 0.125 m
  voxels, 92 m view radius.
- **Sessions** (`vortex_server` + `Shredborne --connect HOST[:PORT] --token HEX`): players walk together and see each
  other (gold-torso figures); every hole is the server's, one round trip after the click; crates and large pieces
  are shared server bodies, small rubble is each client's own. Regions: `playground.session` (96 m) and
  `arena250.session` (250 m, bare terrain). Played by the owner over a LAN, Linux host and Windows client, on
  2026-11-17.

## Not a game yet

No health, damage, death, respawn, teams, rounds or score (M2a); no audio; no settings menu (mouse sensitivity is a
constant, FOV fixed); no character animation (one static figure model); no visible play boundary (an invisible wall at the play bounds
stands until [wp-5]); a playground session sees 56 m. A 5v5 has never run (the most so far: 4 bots and a game client in the soak); the GTX 1650 has
never been measured.

## State

Tests: **742** in ctest: 739 GoogleTest cases (6 disabled measurements), `CpuCheck.RefusesAMissingFeature` and the
game's `CpuCheck.GameRefusesAMissingFeature` (CMake scripts) and `Tools.LocksAndInstallLive` (`tools/selftest-locks.sh`),
none needing a GPU (the ENet and relay ones use UDP on 127.0.0.1). The owner's windowed checks and their results: [owner checks](../../docs/owner-checks.md).

| Area | State | Gaps |
|---|---|---|
| Build | Pinned dependencies, Vulkan headers fetched, shaders compiled to SPIR-V at build time; presets dev / debug / release / ci (-Werror on engine and game); `tools/build.sh` under build locks; `VORTEX_GRAPHICS=OFF` builds the GPU-free part without a Vulkan loader; BuildInfo (`Build:` line, `--version`); start-up CPU check (exit 4); live server from `tools/install-live.sh`'s copy. | Only Clang tested; no CI; the Windows CPU-check path unverified. |
| Core | Logging, job system, profiler, input state, RNG, command line. | |
| Game UI | Play mode locks the mouse (pointer lock on Wayland, grab on X11) and looks without a button; Esc pause menu. HUD without ImGui (`vortex.core:hud`, one instanced draw after tone mapping, 5 x 9 bitmap font): crosshair, mode, tool, frame rate, key hints, menu, session panel. ImGui only for F3 and F1. | Windowed behaviour confirmed only by the owner; no settings menu, key rebinding, gamepad, TTF text, chat. |
| Voxel world | Streaming window of sparse 32^3 chunks; generation on workers; edits with material hardness; region files for the offline game and editor (record the voxel size, refuse another). Full-height window -48..128 m that follows x / z; column pass for sky and rock; buried chunks Packed (RLE, no slot); build ceiling at 128 m. | Chunks dense (37 KiB); no background saving. |
| Terrain | Built-in generator (hills, mountains, beaches, sea, caves, trees), `IWorldGenerator` for custom ones; voxel size a world setting, 0.125 m default (`--voxel-size` previews 0.1-0.15 m offline); content and debris thresholds keep their metre size. | Biomes, structures, ore veins (worldgen.md). |
| Ray queries | One traversal for GPU and CPU (`shaders/shared/trace.glsl`), world + objects through a BVH. | |
| Objects | Voxel models shared between instances, collider boxes, mass from material density. | |
| Scene | `vortex.scene`: EnTT entities (transform, voxel body, rigid body, point light, lifetime); destroying one frees its body and chunk slots; GPU-free (`Vortex::Sim`). | Emitter component, transform hierarchy, contact events per entity, spatial index, entity save / load. |
| Destruction | `sim::Blast`: integer carve; islands searched on a cell graph cached per chunk (face flood inside a +-96-voxel flood box), one `Detach` per island of 8+ voxels, one `ClearMask` for smaller ones; a max-flow load check releases what its bonds cannot carry (integer strengths per material); object damage (`Scene::Damage`), impulses. `Engine::PresentBlast`: particles, local rubble, flash. Rubble lifetime by size (15 / 20 / 30 s), caps 192 small, 48 bodies, 600 store slots. | No impact damage (contact events never drained: [si-2]); objects not load-checked ([si-1]); no re-checks away from casts ([si-3]); windowed falls not confirmed by the owner. |
| Import | MagicaVoxel `.vox` read / write, glTF voxelisation. | |
| Physics | Jolt behind an engine interface, fixed step, character moved by `sim::StepPlayer` (restorable for replays); terrain colliders streamed and requested by moving bodies; bodies without a ready (or not yet re-cooked) collider frozen with their velocity; a parked body woken when its edited ground gets a collider. | Guard gaps under Later. |
| Particles | CPU pool with emitters, voxel collision, presets. | Pool of 32 768 sorted every frame; fight cost unmeasured ([wp-8]). |
| Renderer | Ray-marched G-buffer, ray-traced sun shadows, AO, emissive voxel light, point lights, temporal accumulation, fog, particles, TAA, tone mapping; headless screenshots; validation-clean on the UHD 620. | perf.md not started ([wp-4]); reflections, bloom, shadowed particles. |
| Editor | `vortex.editor` (F1): fly camera, selection with translate / rotate gizmo, inspector, terrain brush, primitives, `.vox` / glTF import, environment / render / profiler / world panels; tool logic unit tested. | Editor list under Later. |
| Simulation | `sim::Simulation`: world, physics, terrain colliders and scene on a numbered fixed tick; `sim::WorldOp` / `ApplyOp`; `sim::Blast`; no renderer or window. | Gameplay rules live in `net::Server` ([wp-6]). |
| Sessions (M1) | Networking stages 0-8 built, reviewed and committed ([design/networking.md](design/networking.md), "Stages"). `vortex.net` over ENet: server-authoritative terrain operations, predicted movement, replicated bodies with NetIds (2 new per tick, 64 awake, 256 live), joins by chunk sync, a moving client window on regions wider than 48 chunks, hash and entity checks with repair, server terrain mode; `vortex_server`, `vortex_bot`, `tools/soak.sh`, `tools/session-check.sh`. Owner LAN run 2026-11-17 (stage 7b's commits): "seamless". | M1 acceptance (owner's LAN run on stage 8 or a later pair); an invisible wall at the play bounds ([wp-5] shows it); 56 m on the playground ([wp-5]); 5v5 never run; the arena walker's 1.49 s quiet p99 unexplained; pacer on a slow lossy link (M6); re-sent chunks for a walker (memory); bodies frozen awake count against the cap; Windows build not reproducible. |

## Performance and measured values

UHD 620 laptop (8 threads), 1280x720 output, 768x432 internal, optimised `dev` build (-O2 -g; CPU timings before
2026-11-12 are -O0). No shader or `src/graphics` commit since engine `5cda1c2` (2026-11-12), so GPU rows from that date
on describe today's renderer. (m) measured, (e) estimate. Where to save frame time: [design/perf.md](design/perf.md).

| Date | Measure | Value | Source |
|---|---|---|---|
| 2026-11-21 | G16 corrections, WP1 final tree | alone, 3 runs: 2, 0, 5 corrections (walking client only), max error 0.179 / 0.004 / 0.229 m, server 3-6 late inputs a run (m). Beside G19 (the 35-minute soak sharing the 8 cores), 1 run: 2 corrections, max error 0.500 m, 42 late inputs: the walk's first ticks reached the server late (0.50 m, eight ticks' walk, along the walking direction on flat ground). Run G16 alone. | WP1 gate run |
| 2026-11-19 | G16 corrections (`tools/session-check.sh`, 2 clients each run) | stage 8 (`430fde0` / `10197d2`, 5 runs): per run 2, 3, 2, 2, 3 corrections in all, max error 0.073-0.207 m; WP1 (walls, 12 runs): 4, 2, 2, 5, 2, 2, 1, 3, 3, 2, 3, 2, max 0.063-0.353 m; always one client of the two, 0 snaps (m). The gate was never "0 corrections" in practice. Cause (WP1 runs with the `Client: correction` line): only the walking client (slot 0), 27-37 m inside the play area, no wall met; an input that reached the server late (one run's server `Players:` line: 4 late inputs, 13 ticks that repeated a movement) leaves the server a tick behind: 0.063 m (one tick at 3.8 m/s) on flat ground, often at the walk's first ticks while the clock margin settles, up to a step (0.33-0.35 m vertical) where the path crosses a crater rim of its own casts. G16's bound: under 0.5 m, provisional. | WP1 review recheck |
| 2026-11-17 | GPU frame, session plaza (game client) | gbuffer 10.37, lighting 8.96, composite 1.38, TAA 0.64, present 0.42 = 21.8 ms (46 fps ceiling) (m). Replaces CLAUDE.md's "about 21 ms (gbuffer ~8, lighting ~10, rest ~2.5)" and this page's "~9-11 / ~9-10 / ~2.5". | review section 1 (`live.log`) |
| 2026-11-12 | GPU, near (`--camera probe`), 0.125 m | gbuffer / lighting / composite 6.22 / 8.88-9.23 / 1.97-2.33 ms (m) | [vd-3]: [history/view-distance/stage-3.md](history/view-distance/stage-3.md) |
| 2026-11-12 | GPU, far (`--camera=-1,61,65,-0.241,-0.55`, the peak over the valley), 0.125 m | 12.20-12.21 / 7.29-7.62 / 1.28-1.32 ms (m); about 21.5 ms with TAA and present | same |
| 2026-11-12 | Far gbuffer by voxel size (0.1 / 0.125 / 0.15 m) | 10.58 / 12.20 / 11.54 ms at view radius 73.6 / 92 / 110 m (m) | [vd-2]: [history/view-distance/stage-2.md](history/view-distance/stage-2.md) |
| 2026-11-12 | World window, 0.125 m | 48 x 44 x 48 chunks (192 m, -48..128 m), view radius 92 m with edge fog over its last 15 %; 5300-5600 dense and about 14 800 Packed (23-24 MiB RLE) once filled; store a/b 5314 near / 5632 far; fill 265-268 frames (5.3 s); 473 MiB GPU memory (m) | [vd-3]; this page before the rewrite |
| 2026-11-12 | Generation | about 1000-1100 chunks per worker-second at every voxel size; fill capped at 64 dense integrations per frame (m) | [vd-2] |
| 2026-11-10 | Lighting features (at 0.1 m) | sun shadows ~4.7 ms, AO ~5.9, emissive ~1.5 (m); off with `--no-shadows`, `--no-ao`, `--no-emissive` | this page before the rewrite |
| 2026-11-11 | HUD | 0.01 ms, median of 120 frames, headless 1280x720 (play about 150 quads, pause menu about 70) (m) | same |
| 2026-11-18 | Game client after a 10-minute fight (headless) | gbuffer / lighting 8.8 / 8.0 ms in the ruined plaza, 7.9 / 11.5 ms on the arena; HUD 177 quads (m) | [net-8]: [history/networking/stage-8.md](history/networking/stage-8.md) "Stage 8 measurements" |
| 2026-11-17 | Owner, windowed | LAN session: 24 ms ping, 43 fps on the UHD 620 at the playground's 56 m radius. 2026-11-12, offline: 27 fps at a peak (m) | [history/networking/owner-runs.md](history/networking/owner-runs.md); review section 1 |
| 2026-11-17 | Session client (owner's live server) | join 1.42 s, 130.7 KiB; 10.7 corrections a minute, max error 6.3 cm, 0 snaps; RTT 27 ms; checks 0 mismatches, 2.20 ms max; store 2087 / 12 288 (m) | review section 1 (`live.log`) |
| 2026-11-17 | Session view radius | 56 m on the playground (window 30 x 44 x 30), 88 m on the arena, 92 m offline (m) | review section 1 |
| 2026-11-13 | Load check, `--neck-blast` | tower on a 2 x 2 neck released at 5.61x its cut (0.125 m); 2 x 11 at 1.04x; 2 x 12 stands (m) | [shred-4b]: [history/shred/stage-4b.md](history/shred/stage-4b.md); CLAUDE.md before the rewrite |
| 2026-11-12/13 | Blast cost (quiet CPU) | island search 0.06-1.0 ms per cast incl. cold graph builds; load check 0.13-0.73 ms at a cached site plus up to 1.8 ms of builds at a cold one; in game 0.3-2.9 ms + builds up to 2.8 ms (m) | [history/roadmap/drafts-2026-11-12.md](history/roadmap/drafts-2026-11-12.md) |
| 2026-11-17 | Drill join latency (loopback, simulated clock) | p99 11 ticks for the bot playing, 4 for the joiner; over `--enet` (one thread for every bot) 15-75 ticks (m) | [net-7a]: [history/networking/stage-7a.md](history/networking/stage-7a.md); CLAUDE.md before the rewrite |
| 2026-11-17 | Server terrain mode | join 16.2-16.3 MiB on the playground, 40 MiB for the game's window on the arena, 0.8 MiB per window step while walking there; `TerrainAck` 0.67 KiB per step (was about 6.8); 1.2-1.3 ms on the server per terrain-mode step on average (m) | [net-7b]: [history/networking/stage-7b.md](history/networking/stage-7b.md) |
| 2026-11-17 | Check costs | game client 2.0-3.7 ms at most per check in three-process runs (about 0.3 ms of hashing); server 0.4-3.9 ms per peer and tick (m) | [net-7b] "Review of stage 7b" |
| 2026-11-13 | Arena server preload | 9.6-10.4 s (gate 10 s), 48.5 MiB RLE, 463-488 MiB RSS (m) | [net-3]: [history/networking/stage-3.md](history/networking/stage-3.md) |
| 2026-11-18 | Soak, event latency (5 % loss, 40 + 0-30 ms each way) | p50 0.09-0.11 s; quiet p99 0.58-0.79 s on plain links, 1.49 s (max 2.2 s) for the arena walker, 0.85-1.29 s behind 100 kB/s; at 1 % loss quiet p99 256-281 ms (the 250 ms target is met in-process, not over ENet) (m) | [net-8] "Stage 8 measurements" |
| 2026-11-18 | Soak, bandwidth per player in play (10 casts/s) | down 30.2-31.7 kB/s on the playground (events 22.7-24.5, state 6-8), up 1.85 kB/s; plus up to 45 kB/s of bulk for a player crossing the arena; unchanged re-sends 22.1 KiB/s for that walker (m) | same |
| 2026-11-18 | Soak, server and clients | server tick p50 under 1.25 ms, p99 under 14 ms (everything on one laptop); join ticks at most 8.1 ms, 0 colliders; arena server 583 MiB, preload 10.2 s under load; client stores at most 5813 slots (7498 in terrain mode); corrections 0-38 a minute of play (m) | same |
| 2026-11-17 | Build | about 45 CPU-minutes for a full build; 482 MB of fetched dependencies (m) | review section 1 |
| - | GTX 1650; a 5v5 fight | never measured | |

## Direction

Decided with the project owner on 2026-11-10; the last five rows are later owner decisions.

| Topic | Decision | What it means for the engine |
|---|---|---|
| Structure | Session-based matches with automatic matchmaking and seamless joining; progression is global, MMO-like. The world is a set of region maps loaded separately (Albion Online style). | A dedicated, GPU-free session server per region instance. No endless world: a region is finite. |
| Players per session | Mixed by region type: small instanced maps and larger shared zones. | Player cap is a property of the region type; the design case for fights is 5v5 at about 10 casts per second. |
| Persistence | A region resets every session. | A region is `template + seed`; damage is a temporary edit log. Everything expensive (support graphs, coarse LOD, colliders of prefabs) can be baked offline per prefab. |
| World content | Procedural terrain assembled with hand-made prefabs by rules. Small arenas (about 250 m) and big zones (about 1 km) from the same generator. | Prefab format with baked data; rule-driven region generator; the server holds a whole region, clients stream a window. |
| Combat | PvP and PvE, PvP first. | Server authority, prediction and lag compensation from the first networked build. Monsters, AI and navigation come after PvP works. |
| Camera | First person. | Close-up voxel detail matters most; the horizon is visible. |
| Hardware | Playable on Intel UHD 620, tuned for a GTX 1650 at 60 fps. Windows and Linux clients, Linux server. | Every feature has a cheap path. View distance is a setting: 77 m on integrated GPUs, 150 m and more on discrete ones. |
| Destruction | Whole castles and cities can collapse. Large falling pieces are real (server-simulated, can crush); small debris is cosmetic and local to each client. Rubble fades after seconds. | The server decides which part of a structure is released and simulates a few large bodies; clients add particles and small pieces. |
| Network stability | Large-scale destruction is limited by game rules rather than by technology: bedrock (indestructible) materials, regeneration of locations, destruction costs. | Indestructible material flag in generation rules; a per-region destruction budget; a regrowth system that restores prefabs from their template. |
| Building | Only in fights: created matter is temporary. Medium-sized conjured objects to climb on or use as a shield. | Spell-created voxels and objects carry a lifetime; no free-form persistent building, no player housing. |
| Magic | Create matter, move matter, change materials, spreading fire, summoning enemies. | A data-driven spell system built from a few world operations (carve, fill, swap material, impulse, spawn). |
| Characters | Rigid voxel parts on a skeleton. | Reuses the object renderer; limbs can be separated. No skinned-mesh path. |
| Online services | Later; stubbed now. | The session server reads players and loadouts from a file and writes results to one. |
| Voxel size and view (2026-11-12) | 0.125 m voxels; the 92 m view radius is "the sweet spot" on the UHD 620: frame rate comes before more distance there. Furniture stays blocky world voxels; step height stays 0.35 m. | `voxel::kDefaultVoxelSize` = 0.125 m (view-distance.md). |
| Fun over realism (2026-11-16) | Fun gameplay with few bugs comes before realistic, synced rubble. The replicated piece threshold stays 0.512 m^3 (a crate breaks at its first hit). | Small pieces stay each client's own rubble; realism features rank below bug fixes, frame rate and feel. |
| Spread (2026-11-17) | Fire and liquid spread in lockstep, with no randomness; Claude implements it. | Integer rules stepped the same on every peer ([design/spread.md](design/spread.md)). |
| Magic spells (2026-11-21) | Every spell is a cast type (Liquid, Flame, Solid, Torrent) x an element (Metal, Thermal, Pressure, Frost, the owner's recommended 4th): 16 named siege spells with their tactical roles ([owner's concept](history/magic/owner-concept-2026-11-21.md)). | Four integer form operators (Flow, Plume, Pierce, Burst) x four element tables, no per-spell code; reach solved on the server and sent as ops; conjured matter is terrain with server-scheduled expiries; movement effects are zones announced ahead ([design/magic.md](design/magic.md)). |
| Repository contents (2026-11-21) | No LICENSE files and no licence sections; no third-party model in the repository (`models/frank.glb` deleted, no replacement); the Obsidian vault stays (the game's `00 Docs Map.md`, `.obsidian/` settings, one `[[...]]` nav line per document); web surveys live only in history. | Import tests and scripts make their glTF in code (`tests/support/GltfFixture.h`); `vortex_deploy_assets` copies shaders only; nav links name current files ([README.md](README.md) rules); [history/surveys/](history/surveys/web-survey-2026-11-21.md). |

## Milestones

### M1 - Networking skeleton
Goal: one server and two clients walk in the same region and see each other's destruction. Built: networking stages
[net-0]..[net-8] ([design/networking.md](design/networking.md), "Stages"); stage 5's two-machine run passed on
2026-11-17. Acceptance: the owner's windowed LAN run on stage 8's commits (Now). [net-9] (the on-demand server world
for 1 km zones) moved behind M2 by the owner (2026-11-11). Stage log: [history/roadmap/m1-stage-log.md](history/roadmap/m1-stage-log.md).

### M2 - Fight feel under authority
- **M2a "first fight"** ([wp-7]): server-side health and damage (blast falloff, falls, lava, leaving the play area),
  death and respawn at team spawns, two teams, cast charges, round timer and score, HUD (health, team, score, timer,
  kill feed), bots that fight, rules only on the server, offline practice as an in-process server. A Felling duel on
  the playground (one watch tower per side; releasing the enemy's top wins), team deathmatch as the fallback; one
  round per session (the server ends it, clients rejoin). Decided by us (review 6.2; the owner may veto): 100 HP, a
  blast does 60 at its centre and 0 at 1.2 m, friendly fire off, 5 s respawn, 8 min rounds, 3 cast charges with one
  back every 1.2 s, the 15-tick cooldown kept as the burst limit. Owner taste check after the first playtest.
- **Fight performance** ([wp-8]): fx-budget stages 1-2 (measurement with `--stress-fx`, effect lights out of the
  ray-traced pass, queued and coalesced damage) and a 10-client run. Acceptance: 5v5 with 10 casts per second holds
  60 fps on a GTX 1650 and does not fall below the idle frame rate by more than a fixed margin on the UHD 620; server
  tick p99 under 16.7 ms. LOD stage 1 ([design/lod.md](design/lod.md): distance budgets for lighting, shadow casters,
  debris and particles) is frame-rate block B.
- **M2b**: spell framework (data-driven casts - the four forms Flow, Plume, Pierce and Burst - producing world
  operations, zones, damage, impulses and effects; lag-compensated hits); conjured matter (temporary walls and
  medium objects with a lifetime, climbable, usable as cover): [design/magic.md](design/magic.md) [mag-1], [mag-2] and
  [mag-4];
  characters from rigid voxel parts with skeletal animation, a training dummy.

### M3 - Collapse of buildings
Built: the cell graph per terrain chunk ([shred-4]) and the load check ([shred-4b]): terrain that its bonds cannot
carry falls as ordinary `Detach` operations. Next: [si-2] impacts with crush damage, then [si-1] (damaged objects
break under their own weight; collapse.md stage 1, the anchored part of an object stays static) and [si-3]
(re-checks away from casts) - [design/shred.md](design/shred.md), "Structural integrity plan (2026-11-17)".
Structures stamped into terrain ([wg-W1]) let castles and towers larger than 12.8 m (an object tops out at 128^3
voxels, collapse.md) fall by the same check. Stability rules: bedrock
materials, a per-region destruction budget, regrowth of prefabs from their template when no player is near.

### M4 - Region generator
[design/worldgen.md](design/worldgen.md): the server plans an integer `RegionPlan` and every side stamps it into
chunk generation (pristine towns, no join bytes); building kits from real-world traditions, standing proven by
construction rules; stages [wg-W1]..[wg-W6]. Setting and story: the game's
[docs/world/setting.md](../../docs/world/setting.md). Prefab library with baked data, rule-driven placement, region
types (arena about 250 m, zone about 1 km) with player caps, spawn points and bedrock layers; editor work aims at
prefabs and rules, not at painting maps. Mechanisms stages 3-4 (siege kit; gates placed in W3 forts) belong here.

### M5 - Magic that changes the world
The owner's 16 siege spells, four cast types x four elements ([design/magic.md](design/magic.md): integer form
operators and element tables, conjured matter with server-scheduled expiries, zones in the movement rule; the first
slice [mag-1] / [mag-2] comes early, in M2b; Frost [mag-3] and Thermal [mag-5] deliver this milestone's material
changes). Moving matter (telekinesis, thrown pieces), material changes (freeze, melt, petrify), spreading fire and
liquid ([design/spread.md](design/spread.md): lockstep integer rules, no randomness, Claude implements it; S1-S6).
Summoning, which starts the PvE track: enemy AI and navigation on destructible ground.

### M6 - Reach and platforms
Far terrain level 1 (150 m) as a setting for discrete GPUs (view-distance.md stages 6-7, lod.md); later levels only
if wanted. Windows client: the owner built the game on a Windows laptop and played a LAN session against the Linux
server on 2026-11-17 (the probe chunks agreed); missing are a documented, repeatable build, the golden tests run on
Windows, and CI. The bulk pacer on a slow, lossy link (Wi-Fi) is measured here.

### M7 - Online services
Accounts, matchmaking, global progression, seamless transfer between regions; replaces the session file from M1.

## Owner decisions pending

From the review, section 6.3, with the recommended answer:

1. Push and branch policy: a standing OK to push verified stage commits (or to a private backup remote); stage and
   wip branches; tags per accepted stage; merge `engine-rewrite` into `main` at M1's end.
2. CI on the public engine repo (needs pushes): yes, Linux build + tests and Windows goldens, $0 on standard runners.
3. Run the live server from an installed copy, not `build/`: yes; agents prepare `~/vortex-live/<commit>/`. Built in
   [wp-1] (`tools/install-live.sh`); the owner installs and restarts the live server when convenient.
4. The Windows report (about 30 minutes), before M2a's protocol bump: `clang --version`, `cmake --version`, the shell,
   `git status` / `git diff` of both repos, and the output of
   `VortexCoreTests --gtest_filter=Terrain.*:Materials.*:ChunkHash.*:WorldHash.*`.
5. Windowed check results: which of the 14 checks passed on 2026-11-17 (Linux and Windows); later the WP4 windowed
   run before and after. Filled once into [owner checks](../../docs/owner-checks.md).
6. Looks: render scale 0.5 against 0.6, the flash out of ray-traced lighting, later H1; from screenshot pairs, 0.6
   until chosen.
7. Delete `engine/CloseIssue.md` (the owner's git cheat sheet) from the repository: answered by the owner, deleted
   2026-11-21.

## Open questions

Settled by the recommended answer when their stage starts (owner's standing rule), unless the owner answers first:
- Player caps per region type (M1 is accepted on a 250 m arena; owner answers in networking.md "Owner decisions").
- Regrowth rules: what regrows, how fast, visible or only when unobserved.
- How much the client is trusted (anti-cheat depth) for PvP.
- The largest conjured object, and the limit of conjured matter per player: for spells settled in
  [design/magic.md](design/magic.md) (6 000 live conjured voxels per player, 60 000 per region); conjured objects
  (bodies) stay open.
- Per design: [shred.md](design/shred.md) (both "Open questions" sections), [fog.md](design/fog.md),
  [mechanisms.md](design/mechanisms.md), [spread.md](design/spread.md), [magic.md](design/magic.md), [worldgen.md](design/worldgen.md),
  [lod.md](design/lod.md), [fx-budget.md](design/fx-budget.md), [collapse.md](design/collapse.md) ("Open questions
  for the game"), [perf.md](design/perf.md) (settled by us in review 6.2), and the game's
  [setting.md](../../docs/world/setting.md) section 11.
