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

- **[audio-1] Audio first pass: built, not reviewed** (2026-11-26). The new module `vortex.audio` (`Vortex::Audio`,
  miniaudio 0.11.25 with stb_vorbis; linked by the engine only, checked by `vortex_assert_no_audio`): the manifest,
  a mixer with five buses (master, sfx, ambience, ui, music), 32 voices with priority and steal rules, at most 4 per
  sound, a start budget per sound (6, refilled at 12 per second), the burst and merge rules, distance classes, and
  per-play variant, pitch and volume variation. `tools/audio/build_sounds.py` made the shipped set from the 1272
  generated WAV takes (1694.5 MB): 256 sounds, 636 Ogg Vorbis files, **16.53 MB** in `assets/audio`, with the licence
  log `docs/product/sound-licence-log.csv`. The engine plays blasts, breaks by material, island cracks, impacts by
  material and speed, mechanisms, rope cuts and pick-ups (`vortex::SoundEffects`, from the scene: also in a session);
  the game plays steps by ground material, jumps, landings, casts, use refusals, throws, the menus and ambience beds
  by biome, water, cover and height. Five volume settings on the settings page. Headless runs mix offline and log
  `Audio:` lines. [design/audio.md](design/audio.md). The owner listens: owner check 30. First listen (2026-11-26): steps like hooves, no gate heard, water slurping. Fixed in code and the tool: steps at a person's cadence (1.5 m a step walking), one hit per footstep file, -4 dBTP (mechanism files clipped), river water plays the stream bed, the portcullis plays its tension first, mechanisms 3 dB louder. The takes themselves need making again: 21 prompts rewritten (the game's `docs/product/sound-list.md` section 3.1), and the generator must set `seconds_total`.
- **[wp-7] M2a "first fight": built and reviewed** (2026-11-23; review fixes 2026-11-24). The sandbox is a PvP game: a session file with
  `mode = deathmatch` or `felling` runs health 100, blast damage (60 at the capsule, 0 at 1.2 m, integer mm), falls,
  lava and the void, death (not stepped, casts refused `Dead`) and a 3 s respawn at the team spawn with 2 s of spawn
  protection (a teleport: a new `lifeSeq`, never a correction), two teams, three cast charges (one back every 1.2 s;
  `NoCharge`), and one round (warmup, countdown, live, ended; score limit, time with overtime, forfeit, Felling's
  crown), all decided by the server only (`vortex.sim:combat`, `Server::Impl::DealDamage`). Protocol 11 (owner block
  +7 B, remote record +1 B, `Welcome` +7 B, `Roster` 21, `RoundUpdate` 22, `KillNotice` 23). Bots fight
  (`vortex_server --fight`, `vortex_bot --fight`); new gate G22 (a 2v2 bot match on `duel.session` ends with a winner,
  tier `full`). The game's fight HUD (health, charge pips, hit marker, damage flash, kill feed, round banner, Tab
  scoreboard, death screen, team-coloured figures) and offline practice (`Shredborne --practice [--bots N] [--mode
  deathmatch|felling]`, an in-process server). Every old session file stays a sandbox: G11-G21 unchanged.
  [design/fight.md](design/fight.md). The batch review's 14 confirmed findings are fixed, the engine's each with a test (casts
  meet enemy capsules, a respawn taken once its events are applied, a short respawn and a death while held replay
  once, late dead-time casts refused `Dead`, a left slot's kill credit and queued casts cleared, one roster per tick,
  Felling towers rebuilt when the round goes Live, `max_clients` at most 64 in a fight, practice's server catch-up
  bounded, `--practice --bots 0` refused, a session closed after its round exits 0); 3 were rejected (the curtain's
  cost (measured under its budget), the crown count's cost (a fraction of a millisecond every 30 ticks), a new
  practice round's full reload (the design: pristine terrain)). Not run: `verify.sh full --ci`
  and the soak (the batch runs them at its end).
- **Owner: [wp-7]'s fight** (owner checks 21 and 22, [owner checks](../../docs/owner-checks.md)): a windowed practice
  round with 3 bots to its end (the HUD, Tab, team colours) and the taste check (too fast, too slow, too lethal?).
  The Windows client must be rebuilt (protocol 11).
- **[wp-6] Rules seam + net split: built, not reviewed** (2026-11-23). Nothing changes for a player; spells, modes and
  the use key now land in small files. What an action does is decided in one GPU-free place, `vortex.sim:rules`
  ([design/rules.md](design/rules.md)): `sim::AimAction` (a captured `sim::ActionRequest` -> where it lands, the
  quantised sphere and power, or a refusal) and `sim::ResolveAction` (-> `sim::ActionOutcome`: blasts with their
  operations, damage and impulses (empty until [wp-7] / [mag-1]), effect cues). The session server aims and resolves
  every cast through it (`SessionAimRules`), the offline click too (`OfflineAimRules`, `Engine::PresentOutcome`), and a
  client presents a cast from its cue (`sim::BlastCueOf`). `Server.cpp` (130 KiB) and `Client.cpp` (117 KiB) are split
  by concern into internal partitions `vortex.net:server_impl` / `:client_impl` and eleven units (rules.md "File
  map"). No wire (`kProtocolVersion` 10), movement-rule (`kPlayerRuleRevision` 1) or `buildHash` change. Checks: the
  net suite and the new `Rules.*`; the drill (G11's command) and the arena walk (G13's) give the same world and entity
  hashes and summary lines as before; session-check `PASS`. Not run: `verify.sh full --ci` and the soak (the batch
  runs them at its end).
- **[wp-5] Visible play boundary + moving window on small regions: built, not reviewed** (2026-11-23). A session
  client gets the game's moving 48-chunk window on every region whose window is not exactly 48 chunks
  (`sim::ClientWindowMoves`), so the playground shows terrain to 88 m instead of 56 m (the columns beyond the server's
  window are the client's own, pristine for good). The play bounds are visible: a faint cyan grid curtain drawn in the
  composite pass where the view ray crosses a bound in front of the scene (`shaders/shared/bounds.glsl`, compiled as
  GLSL and C++, `FrameUniforms::playBounds`, `Engine::SetPlayBounds`, `sim::PlayBoundsOf`), full up to 6 m, gone at
  24 m and 9 m above / below the eye; `EDGE OF THE FIELD` in amber within 3 m of the feet area's edge
  (`sim::DistanceToPlayAreaEdge`); Esc, Settings, Play bounds on / off (`play_bounds` in the settings file);
  offline `--play-bounds minX,minZ,maxX,maxZ` for screenshots. No wire, movement-rule or `buildHash` change.
  [design/networking.md](design/networking.md) "Chunk sync" and "Player movement"; [design/game-ui.md](design/game-ui.md)
  "Settings". Not run: `verify.sh full --ci` and the soak (the batch runs them at its end).
- **Owner: [wp-6]'s offline click** (owner check 20, [owner checks](../../docs/owner-checks.md)): blasts land at the
  crosshair, a crate takes 0.6 m, terrain 1.2 m; nothing feels different.
- **Owner: [wp-5]'s look** (owner check 15, [owner checks](../../docs/owner-checks.md)): the curtain from 1, 5 and
  20 m (steady, no shimmer or ghost trail), the hint at the edge, the settings toggle, the playground's 88 m.
- **[wp-4] Frame-rate block A + settings: built, not committed or reviewed** (2026-11-23). The far view's GPU frame
  is 2.6 ms shorter with the same picture (gbuffer 12.18 -> 10.62 ms, lighting 6.63 -> 5.72: G1 ends primary rays at
  the edge-fog radius, G2 takes the grid's modulo once per ray), the windowed game presents FIFO_RELAXED with 3 images
  and submits a frame as two batches so only present / HUD / ImGui wait for the swapchain image (P1; `--present`,
  `--no-vsync`), every run ends with a
  `Perf:` line (CPU p50 / p99 / max, fence wait, frames over 25 / 33 ms, GPU per pass, `GPU upload`, `GPU clock:`)
  and a windowed run leaves `build/logs/shredborne-*.log`; Esc, Settings sets render scale, vsync, mouse speed and
  field of view, kept in `~/.config/shredborne/settings.txt`; a session's corner shows `fps / cpu / gpu`. New gate G21
  (the per-machine GPU baseline, tier `full`, alone). [design/perf.md](design/perf.md) stages 1-3;
  [design/game-ui.md](design/game-ui.md) "Settings". Not run: `verify.sh full --ci` and the soak (the owner's rule
  for this pack: proportionate checks).
- **Owner: [wp-4]'s windowed checks** (owner checks 18 and 19, [owner checks](../../docs/owner-checks.md)): the three
  present modes with their `Swapchain:` and `Perf:` lines, the settings page with a real mouse, and the look of the
  0.5 / 0.6 render-scale pair (perf.md "Open questions" 2).
- **[nat-R1] built, not reviewed** (2026-11-23): a tester types `Shredborne --join <token>` (with `rendezvous.url`
  beside the game, or `--rendezvous URL`) and plays on the owner's home server, with no address and no port: the
  server (`vortex_server --listen PORT --token HEX --rendezvous`) registers its room on the rendezvous Worker
  (`engine/tools/rendezvous/worker.mjs` over D1), both sides learn their outside address by STUN and punch through
  their NATs, and ENet connects over the proven address. Tested locally: the Worker under Node over `node:sqlite`
  (`Tools.RendezvousWorker`), the Worker's client over a fake HTTPS client (`Rendezvous.*`), and gate G20 (three NAT
  pairs emulated by `vortex_natbox`). [design/rendezvous.md](design/rendezvous.md), "Stages" R1. [nat-R2] (a relay)
  waits for R0's pair results.
- **Owner: deploy the Worker and play one internet session** (owner check 17, [owner checks](../../docs/owner-checks.md)):
  Cloudflare account, `wrangler d1 create` / `deploy` (rendezvous.md "Owner steps"), the URL into `rendezvous.url`,
  then a tester on another network joins with the Windows client.
- **[nat-R0] built** (2026-11-23): the NAT probe and the pair test run on the owner's machines (docs/verify.md "NAT
  tools"); waiting for the owner's pair results (owner check 16), which decide `[nat-R2]`.
- **Owner: M1 acceptance: LAN run on stage 8 commits** (engine `430fde0` / game `10197d2`), windowed, Linux host and
  Windows laptop, the short soak as its pre-check: two-PC guide
  ([section 6.5](../../docs/two-pc-test-windows.md)).

**[wp-2] `tools/verify.sh` + `tools/doc-check.sh`: done (2026-11-22).** Fewer regressions reach playtests: CLAUDE.md's
gates are assertions with exit codes. `tools/verify.sh quick|full|soak` (game `docs/verify.md` "tools/verify.sh") runs
G1-G19 one after another from a copy of the build (one `Build:` line for every process), judges each gate's "Must"
from its logs (`tools/lib/verify-gates.sh`: exit codes, required and forbidden lines, numbers with their bounds such
as G10's load > cut, G12's 11 s and 150 MiB, G13's store slots and walk), ends with a PASS / FAIL table naming the
failing gate, its log and line, writes a history CSV outside `build/` and exits 0 only when every gate passed; `--ci`
adds the -Werror build. Headless game runs exit 3 on a failed expectation of their own script (`Expectation failed:`;
a session client's `Session: replication not shown:`), and 1 / 2 win over 3. `tools/session-check.sh` judges G16's
bound (at most 6 corrections, under 0.5 m, 0 snaps, provisional) and the one-build rule itself; G12 and G16 run alone
(a machine "alone" lock that soaks, builds and verify runs hold shared). `tools/doc-check.sh` checks links, wiki
links, nav lines, stage IDs (documents and code comments), the anchor table, citations in code, append-only history,
test names, this page's test count and the gate table. Each judge has a self-test with planted faults
(`Tools.VerifyAssertions`, `Tools.SessionCheckVerdict`, `Tools.DocCheckSelftest`); the seeded fault of the review
(`--test-skip-beam-cut`: no "Beam cut" line) fails quick's G3 with exit 3. Not taken from the review's scope (us, the
owner may veto): `vortex_server` folding `freed[]`, the walk and preload limits into its exit code (verify asserts
the lines instead: the engine changes only in documents), JUnit output (no CI yet), a `slow` ctest label, doc-check
of the protocol number, flags and `*.md:<line>` references. The review's sixteen findings are fixed (game
`docs/history/wp2/plan-2026-11-21.md` "Review dispositions"): session-check fails a log without a `Build:` line, G10
checks its voxel sizes, gates are stopped with their whole session, an alone-lock timeout fails its gate instead of
the run, worktree builds reuse the main checkout's sources, `tools/ctest.sh` runs G2 by hand under the locks, and
doc-check's citations, wiki links (Obsidian's table pipe, nested headings; `newLinkFormat` relative), reference
definitions and code spans are exact, with reviewed history exceptions. After the fixes `verify.sh full --ci` passed
every gate but G12, whose preload took 11.74 s straight after G2's 5 minutes of ctest on every core (9.72 s and 8.22
s alone minutes later): G12 in `full` stays known-marginal while the CPU is hot; a cool-down before it is open.
Before the fixes every gate passed on the final tree (2026-11-22: `verify.sh full --ci`,
then `verify.sh soak` with G19 `PASS` for both regions).
Plan and review: game [docs/history/wp2/plan-2026-11-21.md](../../docs/history/wp2/plan-2026-11-21.md). Commits: engine `...`, game `...`.

**[wp-3] Document rewrite: done (2026-11-18), ahead of [wp-1] / [wp-2]** (it changes no code, so it collides with
neither; the order is otherwise the review's). Current documents describe the system as it is, history is moved
verbatim to `history/`; the plan and checks are in [../../docs/history/REWRITE-PLAN.md](../../docs/history/REWRITE-PLAN.md).
Acceptance (with [wp-2], 2026-11-22): `tools/doc-check.sh` passes (ctest `Tools.DocCheck`; its first run found 18
faults in current documents, fixed: aliases quoted as headings in the anchor table, a stage-ID placeholder, a misplaced nav line,
a test name placeholder); the fresh reader's check of
[../../docs/history/claude-md/rule-mapping.md](../../docs/history/claude-md/rule-mapping.md) is part of WP2's review
and recorded with it. Commits: engine `a23af7a`, game `1c45cd2`.

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
close the output-folder lock (fd 6), `tools/build.sh` checks `DEPS_FROM` pins under the locks. Since [wp-2]:
`tools/session-check.sh`'s same-`Build:`-line verdict and G16's correction bound are automatic and tested
(`Tools.SessionCheckVerdict`). Commits: engine `be1a2ca`, game `0a99568`.

## Next

In this order:

1. **[wp-8] Fight measurement + fx-budget 1-2 + 10 clients.** Fights stay playable on the UHD 620: `--stress-fx`, a
   10-bot session at 10 casts/s with measured clients, then [design/fx-budget.md](design/fx-budget.md) stages 1-2.
   5-8 d (e). [Review](../../docs/history/reviews/project-review-2026-11-17.md) "WP8". State: built
   ([llm/wp-8.md](llm/wp-8.md) cards 1-7: the skeleton; `--stress-fx`, the `Fx:` and `FxStress:` lines,
   `--blast-log`; the `fight10.session`; effect lights in composite; the particle wins; edited chunks uploaded first
   and the collider starvation test), not yet measured or tested.
2. **[mech-1] Door and gate.** E opens doors, a lever runs a gate, crates move to Q; E picks up, carries and drops
   a small object and LMB throws it (designed 2026-11-24).
   [design/mechanisms.md](design/mechanisms.md), "Stages" row 1. Its own protocol bump, after M2a's; after WP8 so its
   yard is measured inside a known fight budget. 12-13 d (e). State: the offline part is built and tested, [llm/mech-1.md](llm/mech-1.md)
   cards 1-7: the timetable, posing and the crush rule; use targets, levers and holding; the damage rule; E, Q and the
   use prompt in the offline game; the yard in the default game (`--no-mechanisms`, `--mechanism-script`,
   `--mechanism-blast`, `--hold-script`). Tests and `tools/verify.sh quick` pass on the Windows PC; the owner
   checked doors and the lever in a window (2026-11-24). Card 8 (protocol 12: the wire values) and card 9 (the server: the yard with
   `mechanisms = 1`, E use, the lead, the throw and the auto-drops, held bodies in the checks) are built and tested headless;
   card 10 (the client: mechanism copies, prediction at mechanisms, predicted pick-up and drop, other holders' objects)
   is built and tested headless; card 11 (the yard in `playground.session`, the bot errands `--bot-use` / `--bot-hold`, the
   use prompt in the session game) is built. Measured on the drills (2 bots, 40 ms, 60 s): lead 10-11 ticks, 0 late events,
   10-11 uses, 14-15 pick-ups, entities and positions agree, hashes equal; open: one correction of about 2 cm beside the
   door it had just opened for one bot (lossless and lossy), 4-6 corrections of up to 7 cm for one carrying bot, 1-2
   auto-drops (pinned) by carrying bots, and `tools/session-check.sh` fails with or without the yard (two bodies still
   moving at its end, 1-5 m apart: the same at card 9's commit, so not caused by this stage). Not run: `verify.sh full
   --ci`, the soak, the owner's windowed check 25.
3. **[mag-1] Siege core in sim, headless only.** The spell system's first four spells, re-scoped by the owner
   (2026-11-26, Pressure is movement): Updraft, Dash, Punch and Barricade Wave (jump, dash, punch, cover), with only
   what they use: the Burst strip and seed plane, Plume's single column, `MapMask` / `MapShape` as sim ops, knit iron
   that blasts chip, with server-scheduled expiries, the spawn guard, Impulse, Lift and Shove zones and the caster's own
   dash in `sim::StepPlayer` (`kPlayerRuleRevision` 2), Punch's light carve and dispel; no wire change. The owner
   reviews the 4 new spells first (magic.md "The sixteen spells" 9, 11, 12, 13).
   [design/magic.md](design/magic.md), "Stages". 6-9 d (e).
4. **[mag-2] First playable spells.** The same four spells in offline practice and in sessions: the one magic
    protocol bump (it reserves every value the later magic stages need), zone events with a 0.4 s lead, the spell bar
    and the attune menu on the HUD; the owner's Felling-duel taste check. [design/magic.md](design/magic.md), "Stages".
    6-9 d (e).
5. **[si-2] Impacts with crush damage** (with the `NeckCheck` split and a chunk-source interface first). A falling
    tower cracks walls and kills. [design/shred.md](design/shred.md), "SI-2 - impacts (inertial contact)". 7-11 d (e).
6. **[fog-1] Fog stage 1.** Haze toward the sun, low mist and blast dust puffs, without losing frame rate.
    [design/fog.md](design/fog.md), "Stages" 1. About 4 d (e).
7. **[spread-S1] Fire offline.** A burning house that falls, offline only, no wire change (its smoke feeds fog later).
    [design/spread.md](design/spread.md), "Stages" S1. Large (e).
8. **[wg-W1] Stamped village.** The first real map, with towers worth felling. Soak tier (nightly) first.
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
  Frost, the bore and the Shatter combo (Shatter Spike, Flash Freeze, Permafrost Climb) right after [mag-2], [mag-4]
  Flow, climbing, the rune and the rest of Metal (Molten Pour, Rising Spire, Rebar Lance, Rune Trap) after [mag-3],
  [mag-7] Meteor Leap after [si-2] and [mag-4], [mag-5] Thermal (after [spread-S1] headless, [spread-S2] / [spread-S3]
  for play), [mag-6] presentation and tuning. None needs spread S4.
- [si-1] objects break under their own weight and [si-3] re-checks away from casts, after W1 (shred.md SI plan).
- [mech-2]..[mech-4] ropes, siege kit, placement (mechanisms.md; the offline part of [mech-2] is built: load ropes,
  chains, cutting, release with E, drawing, the yard's gallows; its session part is not); [spread-S2]..[spread-S6] fire in a session (a
  protocol bump), collapse from fire, liquid, presentation, tuning (spread.md).
- Frame-rate block B: [perf-L1] (= lod.md stage 1 = [vd-4] distance budgets) and [perf-H1] half-resolution lighting
  (perf.md).
- [audio-2]..[audio-4] (audio.md): the fight's sounds in a session, occlusion, spells and the castle; rigid-part
  characters with procedural animation and team colours (3-5 d (e), may slot between packages).
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
- **Fights** ([wp-7]): `duel.session` (2v2 deathmatch, first to 5) and `felling.session` (a tower per team; fell the
  enemy's crown); health, charges, kills, 3 s respawns, one round; teams in blue and red. Offline practice:
  `Shredborne --practice [--bots N] [--mode deathmatch|felling]` against fighting bots. Played headless only (the
  owner's windowed round: owner check 21).

## Not a game yet

One round per session and no matchmaking (M2a); sound is a first pass (no fight sounds in a session, no occlusion,
no music); settings are only render scale, vsync, mouse speed, FOV, the play-bounds curtain and five volumes (no key
rebinding); no character animation (one static figure model). A 5v5 has never run (the most so far: 4 bots and a game client in the soak); the GTX 1650 has
never been measured.

## State

Tests: **1006** in ctest: 998 GoogleTest cases (6 disabled measurements), `CpuCheck.RefusesAMissingFeature` and the
game's `CpuCheck.GameRefusesAMissingFeature` (CMake scripts), `Tools.LocksAndInstallLive` (`tools/selftest-locks.sh`),
`Tools.DocCheck`, `Tools.DocCheckSelftest`, `Tools.SessionCheckVerdict` and `Tools.VerifyAssertions` (WP2's checks
and their self-tests), `Tools.RendezvousWorker` (the rendezvous Worker under `node --test`, registered when `node` is
found), none needing a GPU (the ENet and relay ones use UDP on 127.0.0.1). The owner's windowed checks and their results: [owner checks](../../docs/owner-checks.md).

| Area | State | Gaps |
|---|---|---|
| Build | Pinned dependencies, Vulkan headers fetched, shaders compiled to SPIR-V at build time; presets dev / debug / release / ci (-Werror on engine and game); `tools/build.sh` under build locks; `VORTEX_GRAPHICS=OFF` builds the GPU-free part without a Vulkan loader; BuildInfo (`Build:` line, `--version`); start-up CPU check (exit 4); live server from `tools/install-live.sh`'s copy. | Only Clang tested; no CI; the Windows CPU-check path unverified. |
| Core | Logging, job system, profiler, input state, RNG, command line. | |
| Game UI | Play mode locks the mouse (pointer lock on Wayland, grab on X11) and looks without a button; Esc pause menu. HUD without ImGui (`vortex.core:hud`, one instanced draw after tone mapping, 5 x 9 bitmap font): crosshair, mode, tool, frame rate, key hints, menu, session panel (`fps / cpu / gpu`). Settings page from the pause menu (`[wp-4]`, `vortex.core:settings`): render scale, vsync, mouse speed, field of view, play bounds (`[wp-5]`), kept in `~/.config/shredborne/settings.txt`. The fight HUD (`[wp-7]`, `src/FightHud.cpp`: health, charges, hit marker, damage flash, kill feed, round banner, Tab scoreboard, death screen). ImGui only for F3 and F1. | Windowed behaviour (the settings page with a real mouse included) confirmed only by the owner; no key rebinding, gamepad, TTF text, chat. |
| Voxel world | Streaming window of sparse 32^3 chunks; generation on workers; edits with material hardness; region files for the offline game and editor (record the voxel size, refuse another). Full-height window -48..128 m that follows x / z; column pass for sky and rock; buried chunks Packed (RLE, no slot); build ceiling at 128 m. | Chunks dense (37 KiB); no background saving. |
| Terrain | Built-in generator (hills, mountains, beaches, sea, caves, trees), `IWorldGenerator` for custom ones; voxel size a world setting, 0.125 m default (`--voxel-size` previews 0.1-0.15 m offline); content and debris thresholds keep their metre size. | Biomes, structures, ore veins (worldgen.md). |
| Ray queries | One traversal for GPU and CPU (`shaders/shared/trace.glsl`), world + objects through a BVH. | |
| Objects | Voxel models shared between instances, collider boxes, mass from material density. | |
| Scene | `vortex.scene`: EnTT entities (transform, voxel body, rigid body, point light, lifetime); destroying one frees its body and chunk slots; GPU-free (`Vortex::Sim`). | Emitter component, transform hierarchy, contact events per entity, spatial index, entity save / load. |
| Destruction | `sim::Blast`: integer carve; islands searched on a cell graph cached per chunk (face flood inside a +-96-voxel flood box), one `Detach` per island of 8+ voxels, one `ClearMask` for smaller ones; a max-flow load check releases what its bonds cannot carry (integer strengths per material); object damage (`Scene::Damage`), impulses. `Engine::PresentBlast`: particles, local rubble, flash. Rubble lifetime by size (15 / 20 / 30 s), caps 192 small, 48 bodies, 600 store slots. | No impact damage (contact events never drained: [si-2]); objects not load-checked ([si-1]); no re-checks away from casts ([si-3]); windowed falls not confirmed by the owner. |
| Import | MagicaVoxel `.vox` read / write, glTF voxelisation. | |
| Physics | Jolt behind an engine interface, fixed step, character moved by `sim::StepPlayer` (restorable for replays); terrain colliders streamed and requested by moving bodies; bodies without a ready (or not yet re-cooked) collider frozen with their velocity; a parked body woken when its edited ground gets a collider. | Guard gaps under Later. |
| Particles | CPU pool with emitters, voxel collision, presets. | Pool of 8192, blended particles sorted every frame, additive ones not; fight cost unmeasured ([wp-8]). |
| Renderer | Ray-marched G-buffer (primary rays end at the edge-fog radius), ray-traced sun shadows, AO, emissive voxel light, point lights, temporal accumulation, fog, particles, TAA, tone mapping; headless screenshots; validation-clean on the UHD 620. Present mode FIFO_RELAXED / FIFO / IMMEDIATE by preference, 3 images; per-pass GPU timers with `upload`, the `Perf:` line. | perf.md stages 1-3 built: M0, P1, G1, G2; block B next (L1, H1); P1's windowed result is the owner's; reflections, bloom, shadowed particles. |
| Editor | `vortex.editor` (F1): fly camera, selection with translate / rotate gizmo, inspector, terrain brush, primitives, `.vox` / glTF import, environment / render / profiler / world panels; tool logic unit tested. | Editor list under Later. |
| Simulation | `sim::Simulation`: world, physics, terrain colliders and scene on a numbered fixed tick; `sim::WorldOp` / `ApplyOp`; `sim::Blast`; the rules seam `vortex.sim:rules` (`sim::AimAction`, `sim::ResolveAction`: every cast, server and offline; [wp-6]); no renderer or window. The fight's rules `vortex.sim:combat` ([wp-7]: damage, charges, lives, teams, the round, Felling's tower). | Crush damage waits for [si-2]; spells for [mag-1]. |
| Sessions (M1) | Networking stages 0-8 built, reviewed and committed ([design/networking.md](design/networking.md), "Stages"). `vortex.net` over ENet: server-authoritative terrain operations, predicted movement, replicated bodies with NetIds (2 new per tick, 64 awake, 256 live), joins by chunk sync, a moving 48-chunk client window on every region whose window is not exactly 48 chunks (the playground sees 88 m, [wp-5]), the play bounds drawn as a curtain with an edge hint ([wp-5]), hash and entity checks with repair, server terrain mode; `vortex_server`, `vortex_bot`, `tools/soak.sh`, `tools/session-check.sh`. Owner LAN run 2026-11-17 (stage 7b's commits): "seamless". | M1 acceptance (owner's LAN run on stage 8 or a later pair); the play-bounds curtain's look confirmed only by the owner (owner check 15); 5v5 never run; the arena walker's 1.49 s quiet p99 unexplained; the arena walker's 150 s still stretch in G19 of 2026-11-22 unexplained (trees on its line were only a contributing weakness; the walk line now names the next one's surroundings, and the walker's verdict fails one over 20 s); pacer on a slow lossy link (M6); re-sent chunks for a walker (memory); bodies frozen awake count against the cap; a server body that never sleeps (creeps about 1 mm/s) holds a soak bot's drain open: G17 fails intermittently with `drain did not hold again` (known flake since 2026-11-17, 2 of 5 runs on 2026-11-24; measured values 2026-11-24); Windows build not reproducible. |

## Performance and measured values

UHD 620 laptop (8 threads), 1280x720 output, 768x432 internal, optimised `dev` build (-O2 -g; CPU timings before
2026-11-12 are -O0). GPU rows from 2026-11-12 to 2026-11-22 describe the renderer
before `[wp-4]` (2026-11-23: G1 + G2 took 2.6 ms off the far view, 0.4 ms off the near one). (m) measured, (e) estimate. Where to save frame time: [design/perf.md](design/perf.md).

| Date | Measure | Value | Source |
|---|---|---|---|
| 2026-11-26 | [audio-1] the sound set and its cost (the Windows PC, the 13 headless game runs of `verify.sh quick`, offline output) | assets 16.97 MB (256 sounds, 636 files, 1578 s; 16.53 MB and 1528 s after the owner's first listen: one step per footstep file, -4 dBTP; from 1694.5 MB of WAV takes); mixing 0.14-0.54 % of one core (up to 23 voices at once, 24-58 plays a run); main-thread `AudioSystem::Update` 0.008-0.028 ms (once 1.1 ms); the first play of a variant decodes it on the main thread offline: 2.5-4.6 ms (once 29.5 ms); decoded 0.6-4.0 MB a run. A device run decodes the sounds of every fight on miniaudio's job thread at the start instead, 18 MB of 16-bit PCM (e, from the files' lengths); the device thread's cost is the offline mixing cost (e) (m) | `build/verify/quick-20261126-105653` |
| 2026-11-24 | G17 (`tools/soak.sh --short`, both regions) on the batch [wp-5] / [wp-6] / [wp-7] | 5 runs: 2 FAIL, 3 PASS. Both failures were `Bot: N FAILED: its drain did not hold again within 15 s of the leave word`: one server body stayed awake through the whole drain, creeping about 1 cm in 16 s (playground object 842 at (10.8, 8.1, 14.9), arena object 814 at (19.5, 4.5, -8.9)), so the bots' copies never got an `EntityRest`; a copy still moving differed from the server's by 0.05-0.11 m (sample timing, not a desync); terrain hashes and resting objects agreed. The first failure ran last in a loaded `full` run, the second alone on the pre-review tree; on the reviewed HEAD (engine `5b2c193`, game `ba5c851`) `verify.sh full --only G17` passed twice (202 / 192 s). The same symptom predates the batch: G19 of 2026-11-22 (WP2 tree) logged `the drain did not settle in 60 s: ... 1 copies not resting` before its drain held, and the soak of 2026-11-17 (`build/soak-o2`) failed the same way. Recorded as a known intermittent flake (a body that never sleeps), not chased (m) | `build/verify/full-20261124-000048`, `-003255`, `-003917`, `-074054`, `-074416` |
| 2026-11-23 | The cast path through the rules seam and the net split, before / after (drill: G11's command; walk: G13's; session-check: G16) | drill: equal world hash `7cf265595fef59bf` at opSeq 3163, `Drill: freed` line and both `Entities: client N ... agree` lines in 2 runs before and 5 after; arena walk: equal `Drill: freed` line and world / sync-box hashes (`2f47611cfe48a298`), entity id hash `3f44ac910423721e`, in 2 before and 4 after (its end opSeq 770-775, end tick and awake count vary between runs of one build: timing); session-check `PASS` 2 / 2 before, 4 / 4 after, the walking client's corrections 1, 3 before and 2, 2, 6, 2 after, max error 0.063, 0.321 m before and 0.183, 0.467, 0.379, 0.100 m after (wall-clock ENet: its worlds differ run to run) | [wp-6] |
| 2026-11-23 | [wp-7] the fight: G22's command (`duel.session`, 4 fighting bots, 5 % loss, 40 + 0-30 ms) and `Shredborne --headless --practice` | G22: team 1 won 5-4 by score at tick 6197 (Live from tick 126), 9 kills, 64 damage events / 1078 hp, 23 casts refused for no charge, 4 for dead, hashes equal, 33 s wall (Countdown took 22 s of it: the bots' joins); `Fight.ATwoVersusTwoBotMatchEndsWithAWinner` (loopback, flat field, first to 2) about 1.5 s. Practice (UHD 620, headless, 1500 frames): server ready 8.2 s with 1 bot / 18.8 s with 3; CPU frame p50 16.76 / p99 28.94 ms with 1 bot, 17.24 / 31.45 ms with 3; RSS at exit 748 MiB with 1 bot, 1385 MiB with 3 (every bot is a whole client world) | [wp-7] |
| 2026-11-23 | GPU composite, the play-bounds curtain 10 m ahead (`--camera 0,14,0,0,-0.15`, without / with `--play-bounds -48,-10,48,48`) | first version (four wall tests per pixel): 1.06 / 1.23 and 1.08 / 1.25 ms (+0.17, over the 0.15 ms budget); with the inside-camera path (the ray leaves through one wall): 1.05 / 1.17 and 1.07 / 1.16 ms, **+0.09 to +0.12 ms** (m, headless, 700 frames, medians of the last 120, two runs each, on AC). Off (offline, or far camera without bounds): far-view composite 0.94 ms against G21's baseline 0.96 | [wp-5] card 3 |
| 2026-11-23 | Playground session view radius | 88.0 m (48 x 44 x 48 moving window; 56 m with the region's 30-chunk window before [wp-5]); store 5140 / 12 288 slots at the join (m, G16, both clients); G16 corrections 1 and 0, max error 0.102 m | [wp-5] card 1 |
| 2026-11-23 | GPU, far (`--camera=-1,61,65,-0.241,-0.55`), before / after `[wp-4]` G1 + G2 | gbuffer 12.18 -> 10.62, lighting 6.63 -> 5.72, composite 1.00 -> 0.87, TAA 0.52-0.53, present 0.42, upload 0.00 ms: GPU sum 20.8 -> 18.2 ms (m, headless, 1200 frames, medians of the last 120 after the window filled at frame 272-283, two runs each, GPU at 1150 MHz) | [wp-4]: [design/perf.md](design/perf.md) "Stages" |
| 2026-11-23 | GPU, near (`--camera probe`), before / after G1 + G2 | gbuffer 6.22 -> 5.96, lighting 8.86 -> 8.64, composite 1.58-1.60 -> 1.53-1.59 ms (m, same runs) | same |
| 2026-11-23 | CPU frame (`Perf:` line, whole run, headless) | far p50 20.83 -> 18.19-18.25 ms, p99 22.91-23.12 -> 20.4-26.2; near p50 17.71 -> 17.24, p99 19.0-20.0; 1-17 frames over 25 ms and 1 over 33 ms per 1200-frame run (start-up and the window filling) (m). Headless the frame is GPU bound (fence wait p50 about 18 ms in 20) | same |
| 2026-11-23 | Thread split, A defaults (7 job workers + 7 Jolt) / B `--job-threads 6 --physics-threads 2` | GPU medians and CPU p50 equal; window filled far at frame 274-283 / 321-323 (5.0-5.4 / 5.9-6.0 s), near 267-272 / 310-311; `--fly` and `--climb` 0 unfilled columns and 0 unloads both; simulation p99 fly 2.89 / 1.65, climb 2.88 / 0.36 ms (m). Defaults unchanged (perf.md T1b) | same, "Why the windowed game can show 27 fps" item 3 |
| 2026-11-23 | G21 GPU medians, run to run (4 runs) | gbuffer and lighting within 0.05 ms; TAA 0.51-0.66 and composite 0.85-0.96 ms vary by up to 0.15 ms between runs (m): G21 judges them by +5 % **and** +0.10 ms, so a baseline recorded on a low run can fail a later one | [wp-4] card 7 |
| 2026-11-22 | `tools/verify.sh full --ci`, WP2 tree | every gate passed: G12 preload 10.10 s, 48.5 MiB RLE; G11 join p99 9 ticks; G13 store at most 5457 slots, walk -118.1 to 122.0 m; G16 0 and 1 corrections, max error 0.000 / 0.070 m; G2 ran 740 tests (746 less the 6 disabled); `verify.sh soak`: G19 `PASS` (23 min) (m). On battery power the same day, G12's preload took 12.9-13.6 s in 3 runs: G12 is measured on AC | [wp-2]: game [history/wp2/plan-2026-11-21.md](../../docs/history/wp2/plan-2026-11-21.md) |
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
- **M2a "first fight"** ([wp-7], built 2026-11-23: [design/fight.md](design/fight.md)): server-side health and damage (blast falloff, falls, lava, leaving the play area),
  death and respawn at team spawns, two teams, cast charges, round timer and score, HUD (health, team, score, timer,
  kill feed), bots that fight, rules only on the server, offline practice as an in-process server. A Felling duel on
  the playground (one watch tower per side; releasing the enemy's top wins), team deathmatch as the fallback; one
  round per session (the server ends it, clients rejoin). Decided by us (review 6.2; the owner may veto): 100 HP, a
  blast does 60 at its centre and 0 at 1.2 m, friendly fire off, 3 s respawn (fight.md), 8 min rounds, 3 cast charges with one
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
