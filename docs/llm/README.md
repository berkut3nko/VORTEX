# Task packs for a local model

[[../../../00 Docs Map|Docs map]] · [[../README|docs index]] · [[../ROADMAP|roadmap]] · [[../../../CLAUDE|working notes]] · [[../../../docs/verify|verify]]

The guide for building the stages of ROADMAP "Now" / "Next" with a local model, card by card. It is written for the
model that does the work (owner's choice, 2026-11-22: Qwen 3.8 Flash Next, 256K context with auto-compaction, run by
the owner on the laptop) and for whoever writes the task packs it works from. The model reads files and runs shell
commands well; it reasons less reliably than the model that designed the stages. So the packs leave it nothing to
design: every card names its files, its interfaces with full signatures, its algorithm step by step, its tests with
what they assert, its commands and when it is done. When a card does not say, the model stops and asks (see "When
you are stuck"); it never invents a rule.

Owner priorities, in this order: fun gameplay with few bugs; build the features and follow the plan; proportionate
verification (no new test infrastructure unless the design lists it).

## The packs

One file per stage in this folder, named by the stage ID in lower case (`nat-r1.md` for `[nat-R1]`). Each is written
from the stage's design document and holds every card of that stage. A pack that is not written yet is listed by
its file name only.

| Order | Stage | Pack | Design | Depends on |
|---|---|---|---|---|
| 1 | `[nat-R0]` finish: review, the two tools, docs | `nat-r0.md` | [rendezvous.md](../design/rendezvous.md) "Stages" R0 | - |
| 2 | `[nat-R1]` rendezvous: Worker + D1, `vortex_https`, `--rendezvous` / `--join`, `vortex_natbox`, G20 | `nat-r1.md` | [rendezvous.md](../design/rendezvous.md) | `[nat-R0]` |
| 3 | `[wp-4]` frame-rate block A + settings | `wp-4.md` | [perf.md](../design/perf.md) stages 1-3; the project review "WP4" | - |
| 4 | `[wp-5]` visible play boundary + moving window on small regions | `wp-5.md` | the project review "WP5"; [networking.md](../design/networking.md) "Player movement", "Chunk sync" | `[wp-4]` (the settings page shows the boundary toggle) |
| 5 | `[wp-6]` rules seam + net split | `wp-6.md` | the project review "WP6" (the pack's first card writes its design) | - |
| 6 | `[wp-7]` M2a "first fight" (protocol 11) | `wp-7.md` | the project review "WP7" (the pack's first card writes its design) | `[wp-4]`, `[wp-5]`, `[wp-6]` |
| 7 | `[wp-8]` fight measurement, fx-budget 1-2, 10 clients | `wp-8.md` | [fx-budget.md](../design/fx-budget.md) stages 1-2; the project review "WP8" | `[wp-7]` |
| 8 | `[mech-1]` door and gate (protocol 12) | `mech-1.md` | [mechanisms.md](../design/mechanisms.md) "Stages" 1 | `[wp-7]`, `[wp-8]` |
| 9 | `[mag-1]` siege core in sim, headless | `mag-1.md` | [magic.md](../design/magic.md) "Stages" | `[wp-6]`, `[wp-7]`, `[wp-8]` |
| 10 | `[mag-2]` first playable spells (protocol 13) | `mag-2.md` | [magic.md](../design/magic.md) | `[mag-1]`, `[mech-1]` |
| 11 | `[si-2]` impacts with crush damage | `si-2.md` | [shred.md](../design/shred.md) "SI-2 - impacts (inertial contact)" | `[wp-7]`, `[mag-1]` (its structure credit) |
| 12 | `[fog-1]` analytic fog extras | `fog-1.md` | [fog.md](../design/fog.md) "Stages" 1 | `[wp-4]` |
| 13 | `[spread-S1]` fire offline | `spread-s1.md` | [spread.md](../design/spread.md) "Stages" S1; magic.md "Amendments to other designs" | `[mag-1]` (`MapShape`, map 1 Ignite), `[si-2]` (structure credit) |
| 14 | `[wg-W1]` stamped village, offline | `wg-w1.md` | [worldgen.md](../design/worldgen.md) "W1: a stamped village, offline" | the nightly soak tier (its first card) |

The order is ROADMAP "Next" ([ROADMAP.md](../ROADMAP.md#now)). Work on one stage at a time, cards in their order. A
stage starts only when every stage it depends on is committed (check with `git log --oneline | grep '\[<stage>\]'`
in both repositories).

## The work loop, per card

1. **Read the card** (its section of the pack, `## Card N: ...` up to the next card) and the pack's header,
   "Decisions" and "Skeleton". Extract one card with
   `sed -n '/^## Card 3:/,/^## Card 4:/p' engine/docs/llm/<pack>.md`.
2. **Check the start state.** `git status` is clean in both repositories; you are on `llm-tasks` in both; the
   previous card's commit is there; the build is green (`tools/build.sh` exits 0).
3. **Read the "Read first" list.** Read only the named sections and symbols: `grep -n` for the heading or symbol,
   then `sed -n 'A,Bp'`. Never read a whole file over 60 KiB (magic.md, shred.md, Server.cpp, Client.cpp,
   main.cpp): read its sections.
4. **Implement the steps in their order.** Interfaces exactly as the card gives them (names, parameter order, types,
   `const`, defaults). Write the tests the card names first when it says so.
5. **Build** after every step that compiles: `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` from the
   game worktree root. Warnings are errors in spirit: fix every one.
6. **Run the card's checks** (its "Checks" list, in order). Always at least: the focused tests
   (`tools/ctest.sh -R '<Suite>\.' --output-on-failure`), then all tests (`tools/ctest.sh`), then
   `tools/doc-check.sh --build build`. When the card changes behaviour (game, server, client, sim rules, shaders):
   `VERIFY_SLOT=1 tools/verify.sh quick`, and look at the screenshots it lists if you can read images (if you
   cannot, say so in the report).
7. **Update the documents the card names** in the same change (stage row, ROADMAP, notes, docs/verify.md, CLAUDE.md
   section 5, the wire registry in notes/net.md section 2).
8. **Commit** (see "Branches and commits") and **write the report** (see "The report"). Then stop; the next card
   starts only when the owner says so, or at once if the owner told you to run several cards in a row.

The stage's last card is its close: `VERIFY_SLOT=1 tools/verify.sh full --ci`, `VERIFY_SLOT=1 tools/verify.sh soak`
in the background, the ROADMAP and design rows, and a report for the stage review. A stage built by this loop ends
in the state **Built**; "Built and reviewed" is written only after the owner's review (CLAUDE.md section 7, step 4).

## Rules (condensed from CLAUDE.md; CLAUDE.md wins where they differ)

Code layout
1. C++20 named modules, one module per directory (`vortex.core / math / voxel / fx / physics / scene / sim / net /
   graphics / engine / editor`). A new interface is a partition file `X.cppm` (`export module vortex.sim:rules;`),
   added to the module's primary interface (`export import :rules;` in `Sim.cppm`) and to the library's sources in
   `engine/CMakeLists.txt`; its implementation is `internal/X.cpp` starting `module;` ... `module vortex.sim;`.
2. Every `#include` goes in the global module fragment, between `module;` and the `export module` / `module` line.
   Third-party implementations (stb, tinygltf, VMA, curl's callbacks are fine) stay in their plain `.cpp` files.
3. The layering DAG of CLAUDE.md section 4: Core <- Physics <- Sim <- Net <- Engine; Graphics -> Core only. Sim and
   Net never see Vulkan, GLFW or ImGui; Core, Physics and Sim never see Net or ENet; only
   `engine/src/net/internal/Enet.cpp` includes `<enet/enet.h>`. The configure step fails if you break it: do not
   "fix" that check, move the code.
4. Every new executable and test binary links `vortex_cpu_check` and `vortex_options`. Dependencies are pinned in
   `engine/cmake/Dependencies.cmake`; adding one is a card of its own and needs the owner.
5. A test helper never shares a name with an engine type.

Determinism and the wire
6. Whatever decides a voxel on more than one machine is integer: spheres in 1/16 voxel (q) with an int64 test, power
   in 1/16 hardness, fixed visit orders. No float decides a voxel, no `std::unordered_*` iteration decides an order,
   no randomness (`rand`, `std::random_device`, time, addresses) in any replicated rule. A seed, where a design
   names one, is an integer in the data.
7. One movement rule: a player moves only through `sim::StepPlayer`. If the same inputs on the same world give
   another result, bump `sim::kPlayerRuleRevision` (in buildHash) in the same card, and only in the card the pack
   names.
8. Any change to a byte on the wire (a message, an event, a value of `WorldOpKind`, `EventKind`, `MessageType`,
   `EntityKind`, `PlayerButton`, a refusal or rejection) bumps `net::kProtocolVersion` once per stage, in the card the
   pack names, and adds the values to the registry in [notes/net.md](../notes/net.md) section 2 in the same commit.
   New values take the next free number (the plan below gives the expected ones).
9. Tables that both sides must share join `buildHash` (`net::BuildHash`) when the pack says so.
10. Edits that replicate go through `sim::ApplyOp` and, on the server, `Server::Impl::EmitOp`. Every chunk edit goes
    through `World::EditRegion` (or bumps `World::ChunkRevision`).

Tests and builds
11. Tests run without a GPU (ctest has no Vulkan device; graphics tests check layouts and SPIR-V contracts only).
    New logic goes in GPU-free code (Core, Physics, Sim, Net) with tests in `VortexCoreTests`,
    `VortexPhysicsTests`, `VortexSimTests` or `VortexNetTests`; GPU and UI code stays thin on top.
12. Build only through `tools/build.sh` (it takes the locks); run tests through `tools/ctest.sh`. Never run `cmake`
    or `ninja` on `build/` yourself, never delete `build/` or `~/.cache/vortex`.
13. The build stays warning-free (engine and game). `tools/build.sh ci` (-Werror) at every stage close.
14. Never weaken, skip or delete an existing test, and never change an expected value in an existing test or a gate,
    unless the card says so and gives the new value. A test that fails after your change is your bug until shown
    otherwise.
15. Never regenerate goldens (`voxel::kGeneratorGoldenHash`, the terrain tables) unless the card says so.

Ports, processes, owner
16. Never use UDP port 27015 (the owner's live server). In this worktree use `VERIFY_SLOT=1` (ports 27060-27064) and,
    for your own runs, ports 27065-27069. Never start, stop or reinstall the live server (`~/vortex-live/`).
17. Never run downloaded binaries, never `npm install` / `pip install` / `curl | sh`, never `sudo`. If a card needs a
    package, it says so and the owner installs it (one `sudo pacman -S ...` line in your report).
18. Headless is all you can check. Windowed play, mouse lock, menus with a real mouse, the editor UI: say "owner
    check" and add it to `docs/owner-checks.md` when the card says so; never claim they work.

Documents
19. Current documents describe the system as it is, in the same commit as the code. Never edit a file under any
    `history/` folder (doc-check fails it; it is append-only and only a stage review moves text there).
20. Every current document keeps its one nav line under the title. Never rename a heading or a numbered item that
    code cites (engine/docs/README.md "Anchors cited from code"). Cite symbols, never line numbers.
21. A stage ID in brackets must be one of engine/docs/README.md "Stage IDs". `tools/doc-check.sh --build build`
    passes before every commit; ROADMAP's `Tests: **N** in ctest: M GoogleTest cases` line is updated whenever you add
    tests (N from `ctest -N --test-dir build | tail -1`, M the count of `TEST(` macros: doc-check names both).
22. Log lines that a gate or a judge reads ([docs/verify.md](../../../docs/verify.md)) are contract: change the line,
    its judge (`tools/lib/verify-gates.sh`) and its fixture (`tools/tests/fixtures/`) together, or not at all.

Lessons that bite here (CLAUDE.md section 6): L8 (no dynamic body over unloaded terrain), L9 (every body spawned in a
fight has a lifetime and a cap), L10 (`maxJobsInFlight` is per frame), L12 (ask the op ring what it dropped, never
"assume everything"), L13 (size rates by the highest recent delivery), L14 (binaries are copied, never run from
`build/` for the live server).

## When you are stuck

Stop and write the report with `Status: blocked` instead of guessing when:
- the card's interface does not compile against the code as it is (a name, a type or a header differs): quote the
  compiler error and the line of the card;
- a test the card names fails and two honest attempts did not fix it: quote the failure and what you tried;
- an existing test or gate fails and you cannot show your change is not the cause;
- the card is silent on a rule that decides voxels, the wire, a hash or the movement rule;
- the card asks for something these rules forbid (a port, a package, a history edit);
- the work would grow beyond the card's files by more than a small helper.

Never "fix" a failure by loosening a check, deleting a test, adding a sleep, a retry loop or a `GTEST_SKIP` the card
did not ask for, or by catching and ignoring an error.

## Branches and commits

- Branch `llm-tasks` in both repositories (the game worktree and its engine worktree inside it). No other branch, no
  rebase, no `--amend` of a commit already reported, no push, no tag.
- One commit per card, in each repository the card touched: **the engine first, then the game**, whose commit
  includes the moved `engine` submodule pointer (`git add engine`) even when the game changed nothing else.
- Message: `[<stage>] card <N>: <what the player or developer gets, short>`, e.g.
  `[nat-R1] card 3: vortex_https over libcurl with a cancellable long poll`. A body of 2-6 lines: what changed, the
  checks that passed. No co-author line unless the owner asks for one.
- Never commit build output, logs, screenshots, `build*/`, scratch files or secrets (a token, a Worker URL with a
  key).
- Do not use `git stash` (the stash is shared with other worktrees); a work-in-progress commit on `llm-tasks` is fine
  and is squashed only by the owner.

## The report

Every card ends with this report as the last thing you print, short enough to read in two minutes:

```
[<stage>] card <N>: <title>
Status: done | blocked | done with deviations
Commits: engine <sha> / game <sha>
Done: <3-8 bullets: what exists now, by file and symbol>
Checks: <each command run and its result, e.g. "tools/ctest.sh: 812/812 passed", "verify quick: PASS",
        "doc-check: PASS"; screenshots looked at, or "not looked at (no image input)">
Numbers: <measured values the card asked for, with units and the command that gave them>
Deviations: <every place the code differs from the card, and why; "none">
Owner: <owner checks added, packages to install, questions; "none">
```

The owner (or a reviewing model) reads it, reviews the diff with the card beside it, and either says go to the next
card or sends findings, which you fix in a new commit `[<stage>] card <N> fix: ...`.

## Values planned across the packs

The packs are written in parallel, so the values that several of them touch are fixed here. The registry in
[notes/net.md](../notes/net.md) section 2 is the authority once a value lands; this table is the plan it should
match. If a value is already taken when your card lands, take the next free one, say so under "Deviations" and
update the later packs' numbers in the same commit.

| What | Today | Planned |
|---|---|---|
| `net::kProtocolVersion` | 10 | 11 `[wp-7]`; 12 `[mech-1]`; 13 `[mag-2]`. No other stage here bumps it (`[nat-R0]`, `[nat-R1]`, `[wp-4]`, `[wp-5]`, `[wp-6]`, `[wp-8]`, `[mag-1]`, `[si-2]`, `[fog-1]`, `[spread-S1]`, `[wg-W1]` change no byte) |
| `sim::PlayerButton`, `kPlayerButtonMask` | 1 Jump, 2 Run, 4 Cast, 8 Crate; mask 15 | 16 Use `[mech-1]` (mask 31; the crate moves to Q); 32 Spell `[mag-2]` (mask 63). `[wp-7]` adds no bit: the blast stays on Cast, and the seam's action slot is an internal value, not a button (us, 2026-11-22; magic.md "Wire values" then uses `PlayerButton_Spell = 32`) |
| `sim::WorldOpKind` (event kinds 1-6 today) | 1-6 | 7 `MapShape`, 8 `MapMask`, 9 `CarveCapsule` `[mag-1]` (sim ops; on the wire from `[mag-2]`). `[spread-S1]` adds no kind: it ignites with `MapShape` and map 1 Ignite (magic.md "Amendments to other designs" 3) |
| `net::EventKind` (entity events) | 32-36 | 37 `MechanismMove` `[mech-1]`; 38 `ZoneStart`, 39 `ZoneCancel`, 40 `CastFx` `[mag-2]`. `[wp-7]` adds no event kind: fight state travels in the owner block, snapshots and messages |
| `net::MessageType` | 1-20 (5 retired) | 21 onward `[wp-7]` (its pack lists them, e.g. the roster, the round state, the kill feed); `Attune` `[mag-2]` takes the next free after `[wp-7]`'s |
| `net::EntityKind` | 1 Light, 2 Object | 3 Mechanism `[mech-1]` (4 Rope later, `[mech-2]`) |
| `net::CastRejection` | 0-3 | `[wp-7]`: `NoCharge` and `Dead` (4, 5); `[mag-2]`: magic.md's list after them |
| `sim::kPlayerRuleRevision` | 1 | 2 `[mag-1]` (push, Impulse, Lift). `[wp-7]` and `[mech-1]` do not change `StepPlayer`'s result: a dead player is not stepped (the caller skips it), and mechanisms are posed before the step |
| World-palette ids | 0-20 (`bedrock` = 20) | 21 `knit_iron` `[mag-1]`; then spread.md's fire and residue ids in its table's order `[spread-S1]` (from 22). `[wg-W1]` uses existing materials only |
| `MaterialFlags` bits | 0-3 used | bit 4 `Conjured` `[mag-1]`; later bits by magic.md and spread.md as they land |
| Gates (CLAUDE.md section 2, `tools/verify.sh --list`) | G1-G19 | G20 `[nat-R1]` (the three `vortex_natbox` pairs); G21 `[wp-4]` (the per-machine GPU baseline, tier `full`, alone); G22 `[wp-7]` (a 2v2 bot match ends with a winner, tier `full`); then magic.md's proposed gates take the next free numbers: G23 `--siege-matrix` `[mag-1]`, G24 the spell drill `[mag-2]`. Any other pack adds a gate only where its design asks for one, at the next free number, and says so in its header |
| `buildHash` inputs | protocol, generator goldens, `MaterialTableHash`, `kPlayerRuleRevision` | `[mag-2]`: `SpellTableHash`, `TurningTableHash`; `SpreadTableHash` with spread S2 (not here; `[spread-S1]` keeps it local) |

Names shared across packs (the first pack named defines the exact signatures in its "Skeleton"; a later pack's card
says "read the built interface in <file>" instead of restating it):

| Name | Defined by | Used by |
|---|---|---|
| `vortex.sim:rules` (`engine/src/sim/Rules.cppm`): `sim::TeamId`, `sim::ActionSlot` (Blast 0; Spell 1 from `[mag-2]`), `sim::ActionRequest`, `sim::ActionOutcome` (operations, damage, impulses, effect cues), `sim::ResolveAction` | `[wp-6]` | `[wp-7]`, `[mech-1]` (the Use path stays beside it), `[mag-1]`, `[mag-2]`, `[si-2]` (crush damage) |
| `vortex.sim:combat` (`engine/src/sim/Combat.cppm`): `sim::FighterState` (health, team, charges, respawn), `sim::BlastDamageAt`, `sim::ApplyDamage`, `sim::DamageCause`, `sim::TickCharges`, team spawns in the session file | `[wp-7]` | `[mag-1]`, `[mag-2]`, `[si-2]` |
| `sim::BlastOptions` (`noCarve` first) and the minimal structure credit (`sim::StructureCredit`, site checks queued by chunk) | `[mag-1]` (magic.md "Expiry and structure") | `[si-2]` adds `carveAsClear`, `objects`, `push`, `deferred` and the impact queue; `[spread-S1]` queues burnout site checks on it |
| `vortex.sim:mechanism`, `scene::Mechanism`, `sim::PoseMechanisms` / `StepMechanisms` | `[mech-1]` | `[mag-1]` (casts hit mechanisms as objects) |
| `voxel::TurningTable`, `WorldOpKind::MapShape` / `MapMask` | `[mag-1]` | `[spread-S1]` (map 1 Ignite) |
| `voxel::World::EditVoxelsInChunk`, `voxel::ChunkChange::shapeChanged`, partial id uploads | `[spread-S1]` | magic's later stages |
| `core::RunStats` (run-wide percentiles) and the `Perf:` exit line | `[wp-4]` | `[wp-8]`, `[fog-1]` (cost lines) |

## Writing a pack (for the pack writers)

A pack is one file, `engine/docs/llm/<stage-id in lower case>.md`, written by a frontier model from the stage's
design document and the code at the commit it names. It is the only design the local model sees, so it settles
every choice. Keep its total under 90 KiB, and each card with its read-first list under about 150 KiB of text
(about 40K tokens), so a card, its files and the work fit in 256K with room to spare.

Layout:
1. Title (`# [<stage>] <name>: task pack`), the nav line (`[[../README|docs index]] · [[README|LLM guide]] ·
   [[../design/<doc>|<design>]]`), then a header table: stage, design sections, code read at (engine / game
   commits), depends on, cards, effort (e), protocol / rule revision / hashes this stage changes.
2. **Before you start**: what must be committed (the stages and how to check), what the code looks like at the start
   (the files and symbols the stage builds on, by name).
3. **Decisions**: every open point the design leaves to its stage, settled (us, the owner may veto), with the
   recommended answer of the design's "Open questions" where it has one.
4. **Skeleton**: every new or changed interface of the stage, as compilable C++ module code with full signatures and
   one-line doc comments: file paths, partition names, `export` lines, the CMake lines that list the files, the test
   files and their suite names. Card 1 of a code stage lands the skeleton: declarations, stub bodies that compile and
   return a neutral value with a comment `// [<stage>] card <N> fills this`, and the stage's new tests written in
   full, each disabled as `DISABLED_` until the card that makes it pass renames it (the close card checks that
   `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage).
5. **Cards**, one `## Card N: <title>` section each, in the template below.
6. **Values this stage lands**: the registry lines for notes/net.md section 2 and the plan above.

Card rules:
- 0.5-1 day of work for the local model; one concern; it builds and its tests pass at its end.
- Pseudocode, not prose, for every algorithm: numbered steps, integer types and units, rounding rules (which way,
  where), visit orders, caps with their values, what happens at each cap and on each edge case (empty input, a chunk
  not loaded, a value at the limit, a second call).
- Every test by suite and name, its file, its setup in a few lines, what it asserts, and the expected values where
  they are known (a computed value with how it was computed).
- Exact log line formats (`Name: field X, field Y`) for anything a gate, a script or the report reads.
- The contracts of CLAUDE.md section 5 and the lessons of section 6 the card touches, by name, with what must not
  change.
- Cite code by file and symbol; quote a few lines of existing code where the card changes them, so the model finds
  the place with `grep`.

### Card template

```
## Card N: <title>

| | |
|---|---|
| Goal | <one sentence: what the player or developer gets> |
| Depends on | <cards of this pack, other stages> |
| Effort | <0.5 or 1 d (e)> |
| Touches | <libraries / modules; "wire: no", "movement rule: no", "buildHash: no" or what changes> |

**Read first** (about N KiB)
- `engine/src/...cppm`: `Symbol`, `Symbol` (what to learn from it)
- `engine/docs/design/<doc>.md` "<heading>" (what it decides)

**Files**
- new `engine/src/<module>/<File>.cppm` (partition `vortex.<module>:<name>`), `.../internal/<File>.cpp`
- change `engine/src/...`: <what, where (symbol)>
- change `engine/CMakeLists.txt` / `engine/tests/CMakeLists.txt`: <the lines>

**Interfaces** (exactly; full signatures)
<C++ code block>

**Steps**
1. <pseudocode, integer rules, visit order, caps, edge cases>
2. ...

**Tests** (written first when marked)
- `Suite.Name` (`engine/tests/<area>/<File>.cpp`): setup ...; asserts ... ; expected ...

**Log lines and flags**
- `<exact format>` (who reads it)

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning
2. `tools/ctest.sh -R '<Suite>\.' --output-on-failure` - all pass
3. `tools/ctest.sh` - all pass
4. `tools/doc-check.sh --build build` - PASS
5. <`VERIFY_SLOT=1 tools/verify.sh quick` when behaviour changes; a headless run with its expected lines>

**Docs**
- <file: what to add or change>

**Must not break**
- <CLAUDE.md section 5 contracts by name; lessons; gates whose lines must stay equal>

**Done when**
- [ ] <checkable statements>

**Traps**
- <what is easy to get wrong here, and the right way>
```
