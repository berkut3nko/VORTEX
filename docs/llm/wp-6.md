# [wp-6] Rules seam and net split: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/networking|networking]]

| | |
|---|---|
| Stage | `[wp-6]` rules seam + net split (ROADMAP "Next") |
| Design | The project review of 2026-11-17 (game `docs/history/reviews/project-review-2026-11-17.md`), section 3.2 row "Gameplay rules inside `net::Server`" and section 4 "WP6. Rules seam and net split". No design document exists yet: card 1 writes `engine/docs/design/rules.md` from this pack's "Decisions". |
| Code read at | engine `bafca16`, game `0e27ea7` (branch `llm-tasks`, from `wip/nat-r0`) |
| Depends on | nothing in "Next" (it may start before `[wp-4]` / `[wp-5]`); `[nat-R0]` and `[nat-R1]` touch other files |
| Cards | 8 |
| Effort | 6-9 d (e): seam design 1 d (card 1), build 3-4 d (cards 2-5), split 2-3 d (cards 6-8) |
| Changes | wire: **no** (`net::kProtocolVersion` stays 10); movement rule: **no** (`sim::kPlayerRuleRevision` stays 1); `buildHash`: no; gates: none added |

What the player gets: nothing visible. What the developer gets: one GPU-free place (`vortex.sim:rules`) that turns
"player P used action slot S, aimed like this" into world operations, damage, impulses and effect cues, used by the
session server, the offline game and the client's presentation; and `Server.cpp` (130 KiB) / `Client.cpp` (117 KiB)
split by concern so that `[wp-7]`'s fight rules, `[mech-1]`'s Use key and `[mag-1]`'s spells land in small files.
Acceptance (review WP6): every net test green, `kProtocolVersion` unchanged, drill, arena walk and session-check
summary lines equal before and after.

## Before you start

- `git log --oneline -1` in both repositories shows the branch `llm-tasks`; `git status` is clean in both.
- No stage of this pack's dependencies is needed. If `[wp-4]` or `[wp-5]` landed first, nothing here conflicts
  (they change graphics, settings and the client window; this stage changes the cast path and moves code).
- The code this stage builds on (check each with `grep -n`):
  - `engine/src/sim/Blast.cppm`: `sim::Blast(Simulation&, const voxel::SphereQ&, uint16_t)`,
    `sim::Blast(Simulation&, const BlastDesc&)`, `sim::BlastResult`, `sim::AppliedOp`, `sim::PresentBlast`,
    `sim::CastWorkOf`.
  - `engine/src/sim/Player.cppm`: `sim::PlayerTuning` (`castRadius` 1.2 m, `castPower` 6, `castRange` 60 m,
    `castCooldownTicks` 15, `eyeHeight` 1.62 m), `sim::LookDirection`, `sim::TakeCast`, `sim::StepPlayer`.
  - `engine/src/sim/Region.cppm`: `sim::RegionDesc`, `sim::CastFitsRegion`.
  - `engine/src/net/internal/Server.cpp`, inside `struct Server::Impl`: `CaptureCast`, `RejectCast`, `Queue`,
    `PeerOfSlot`, `ResolveRay`, `LateCast`, `RunCast`, `RunCasts`; the anonymous-namespace struct `QueuedCast`
    (fields `sphere`, `power`, `queuedTick`, `player`, `ray`, `origin`, `direction`, `inputTick`, `loadout`).
  - `engine/src/net/Messages.cppm`: `net::CastRejection` (0 `None`, 1 `Cooldown`, 2 `Missed`, 3 `Outside`),
    `kMaxCastRejection` = 3.
  - `engine/src/net/internal/Client.cpp`: `Client::Impl::Present` (makes a `BlastResult` per `CarveSphere` it
    applied, sets `castCenter` / `castRadius`), `Client::TakePresentations`.
  - `src/main.cpp` (game), `OfflineGame::OnUpdate`: the left click casts with
    `engine.World().Raycast(cam.position, cam.Forward(), 80.0f)` and `engine.Physics().Raycast(..., 80.0f)`, then
    `engine.Explode(position, 0.6f or 1.2f, 6.0f)`. `Engine::Explode` = `PresentBlast(sim::Blast(sim, BlastDesc))`.
  - `src/Session.cpp` (game), `SessionGame::Receive`: `engine.PresentBlast(blast)` for each
    `m_Client.TakePresentations()`.
  - `engine/CMakeLists.txt`, `vortex_add_module_library`: every `*.cppm` under a module's directory (recursively,
    `CONFIGURE_DEPENDS`) goes into the `FILE_SET CXX_MODULES`, every `*.cpp` into the private sources. **New files
    need no CMake line**; tests are globbed the same way by `engine/tests/CMakeLists.txt`.

## Decisions

Settled by us (the owner may veto; the review leaves the seam's design and the server's shape to us).

1. **The seam is a partition of `vortex.sim`**, `vortex.sim:rules` (`engine/src/sim/Rules.cppm`, implementation
   `engine/src/sim/internal/Rules.cpp`). GPU-free and network-free (Sim may not see Net: `vortex_assert_no_network`).
2. **Two calls, aim then resolve.** `sim::AimAction` turns an `ActionRequest` (who, which slot, eye, look direction)
   into an `ActionAim` (what the ray hit, the quantised sphere and power) or a refusal; `sim::ResolveAction` turns
   a request and its aim into an `ActionOutcome` (blasts with their operations, damage events, player impulses,
   effect cues) and applies the world part. They are separate because the server aims when the cast queue
   reaches the cast (`RunCasts` dequeues, then aims on the world as the casts before it left it: networking.md
   "Time and ticks") and the queue's budgets sit between capture and run. Today's `ResolveRay` becomes
   `AimAction`; today's `sim::Blast` call in `RunCast` becomes `ResolveAction`.
3. **Cooldown and capture stay where they are.** The cooldown is part of the movement rule (`sim::TakeCast` inside
   `sim::StepPlayer`, predicted by the client); the seam does not judge it. `ActionRefusal::Cooldown` exists only
   so that net can map every refusal to `CastRejection` in one place.
4. **The action slot is an internal value, not a button.** `ActionSlot::Blast = 0` is what `PlayerButton_Cast`
   triggers. `[wp-7]` adds no button; `[mag-2]` adds `ActionSlot::Spell = 1` with `PlayerButton_Spell = 32`
   (llm/README "Values planned across the packs").
5. **Aim rules are data.** `sim::AimRules` holds the range, the radii, the power, which bodies take a cast and the
   region check. `SessionAimRules(tuning, region)` reproduces today's server exactly; `OfflineAimRules()` reproduces
   today's offline click exactly (80 m, 1.2 m on terrain, 0.6 m on a body, any body, strictly nearer than the
   terrain, no region). No behaviour changes in this stage, offline included.
6. **Damage and impulses are empty in this stage.** `ActionOutcome::damage` and `::impulses` exist; `[wp-7]` card 3
   fills `damage` (it reads `RulesContext::players` and `RulesContext::blastDamage`, both present now and unused);
   `impulses` stay empty until `[mag-1]` (the push, `kPlayerRuleRevision` 2).
7. **The client never resolves.** The server decides; a client only presents what it applied. Its presentation
   shares the seam's cue type: `sim::BlastCueOf(const WorldOp&)` makes the `EffectCue` of a `CarveSphere`, and
   `Client::Impl::Present` takes the centre and radius from it. `Client::TakePresentations()` keeps its signature.
8. **Presentation of an outcome**: `Engine::PresentOutcome(const sim::ActionOutcome&)` calls
   `Engine::PresentBlast` for each blast in order (effect cues of other kinds get their presentation with the
   stage that adds them). The offline click uses it. `Engine::Explode` stays (no caller left; not removed, to keep
   the engine API stable).
9. **The server's shape: one generic `net::Server`, rules compiled in.** No scripting and no rules data file now:
   the server calls the `sim` seam; game modes (`[wp-7]`) are a session-file key and C++ in `sim` and in
   `ServerCasts.cpp` / a later `ServerFight.cpp`. Rules data (spell tables) arrives with `[mag-1]` as C++ tables
   hashed into `buildHash` (`[mag-2]`).
10. **The split**: `Server::Impl` moves into an internal partition `vortex.net:server_impl`
    (`engine/src/net/internal/ServerImpl.cppm`), with its member functions declared there and defined in six
    implementation units by concern (table in card 6). The same for `Client::Impl` (`vortex.net:client_impl`, five
    units, card 7). If Clang or CMake refuses an internal partition (card 2 checks it), the fallback is an interface
    partition that exports nothing and that `Net.cppm` does not re-export (`export module vortex.net:server_impl;`
    with no `export` on any declaration). Either way nothing outside `vortex.net` sees `Impl`.
11. **"Summary lines equal"** means: card 1 runs each baseline twice on the unchanged tree and keeps the summary
    lines that are equal in both runs ("stable lines"); after a card, every stable line must appear unchanged in a
    new run. Lines that differ between two runs of the same tree (timings, latencies under jitter) are not compared.
    `session-check` must also print `PASS`.
12. **rules.md headings** (card 1 writes them; code comments may cite only these): "Decision", "The seam",
    "Aim rules", "Outcome", "Server shape", "File map", "Stages", "Open questions".

## Skeleton

The whole new interface, as card 2 lands it (stub bodies) and cards 3-5 fill it. Copy it exactly.

`engine/src/sim/Rules.cppm`:

```cpp
module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

export module vortex.sim:rules;

import vortex.voxel;
import :simulation;
import :worldop;
import :blast;
import :region;
import :player;

// docs/design/rules.md "The seam": what an action does, GPU-free; the server, the offline game and a
// client's presentation share it. [wp-6]
export namespace vortex::sim {

    /// @brief A team. 0 = none (the sandbox, offline). Teams are numbered from 1 ([wp-7]).
    using TeamId = uint8_t;
    inline constexpr TeamId kNoTeam = 0;

    /// @brief What a player uses. Internal, not a wire value: PlayerButton_Cast triggers Blast.
    enum class ActionSlot : uint8_t {
        Blast = 0,   ///< the left-click blast (PlayerTuning castRadius / castPower)
    };

    /// @brief Why an action did nothing. Values equal net::CastRejection's (net maps them, RejectionOf).
    enum class ActionRefusal : uint8_t {
        None = 0,
        Cooldown = 1,   ///< judged by sim::TakeCast in StepPlayer, never by the seam
        Missed = 2,     ///< the ray met nothing within AimRules::range
        Outside = 3,    ///< the sphere does not fit the region (sim::CastFitsRegion)
    };

    /// @brief One use of an action slot, as captured (the server's cast queue holds it).
    struct ActionRequest {
        uint8_t player = 0;                  ///< slot + 1 of the player; 0 = no player (server cast, offline)
        ActionSlot slot = ActionSlot::Blast;
        uint32_t inputTick = 0;              ///< the input whose edge made it (0 offline)
        glm::dvec3 eye{0.0};                 ///< ray origin, world metres
        glm::vec3 dir{0.0f, 0.0f, -1.0f};    ///< unit look direction
        uint8_t spell = 0;                   ///< [mag-2]; 0 now
    };

    /// @brief How a ray becomes a blast (decision 5).
    struct AimRules {
        float range = 60.0f;                 ///< metres from the eye
        float terrainRadius = 1.2f;          ///< metres, when terrain takes the cast
        float bodyRadius = 1.2f;             ///< metres, when a body takes the cast
        float power = 6.0f;                  ///< Material::hardness units
        uint8_t bodyLayers = 0;              ///< physics::LayerBit mask of bodies that may take it; 0 = every layer
        bool replicatedBodiesOnly = false;   ///< a body must carry scene::NetId (a session server)
        /// Offline: the body ray runs the whole range and a body takes the cast only when strictly nearer
        /// than the terrain hit. Else the body ray stops at the terrain hit and any hit on it counts.
        bool strictBodyFirst = false;
        const RegionDesc* region = nullptr;  ///< set: CastFitsRegion decides Outside; null: no check
    };
    /// @brief The session server's rules: castRange, castRadius on terrain and bodies, castPower, Dynamic | Rubble
    /// bodies with a NetId, the region check. `region` must outlive the returned rules.
    AimRules SessionAimRules(const PlayerTuning& tuning, const RegionDesc& region);
    /// @brief The offline click's rules: 80 m, 1.2 m on terrain, 0.6 m on a body, power 6, every layer,
    /// strictBodyFirst, no region.
    AimRules OfflineAimRules();

    /// @brief Where an action lands.
    struct ActionAim {
        bool hit = false;
        bool atBody = false;                 ///< a body took it (counted by the server: castsAtBodies)
        float distance = 0.0f;               ///< metres from the eye
        glm::dvec3 point{0.0};               ///< world metres
        glm::ivec3 voxel{0};                 ///< the voxel hit (World::WorldToVoxel of `point` for a body)
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        voxel::SphereQ sphere;               ///< the blast, quantised (1/16 voxel)
        uint16_t power = 0;                  ///< 1/16 hardness
    };
    /// @brief Casts the request's ray on the simulation as it is now. Fills `out` (also on Outside: `hit`,
    /// `atBody`, `sphere`); returns None, Missed or Outside.
    ActionRefusal AimAction(Simulation& simulation, const ActionRequest& request, const AimRules& rules, ActionAim& out);

    /// @brief Health taken from a player ([wp-7] fills; empty in [wp-6]).
    struct DamageEvent {
        uint8_t target = 0;                  ///< slot + 1
        uint8_t source = 0;                  ///< slot + 1 of the actor; 0 = none
        int16_t amount = 0;
        uint8_t cause = 0;                   ///< [wp-7] sim::DamageCause
    };
    /// @brief A velocity change of a player, cm/s per axis ([mag-1] fills; empty until then).
    struct PlayerImpulse {
        uint8_t target = 0;
        std::array<int16_t, 3> dvCm{};
    };
    enum class EffectKind : uint8_t {
        Blast = 1,   ///< a blast's flash and burst; centre and radius of its sphere
    };
    /// @brief Something to show, in the integers of the world operation it belongs to.
    struct EffectCue {
        EffectKind kind = EffectKind::Blast;
        glm::ivec3 centreQ{0};               ///< 1/16 voxel
        uint16_t radiusQ = 0;                ///< 1/16 voxel
        friend bool operator==(const EffectCue&, const EffectCue&) = default;
    };
    /// @brief Everything an action did. `blasts` hold the applied operations in order (the server emits them).
    struct ActionOutcome {
        ActionRefusal refusal = ActionRefusal::None;
        std::vector<BlastResult> blasts;
        std::vector<DamageEvent> damage;
        std::vector<PlayerImpulse> impulses;
        std::vector<EffectCue> effects;
    };

    /// @brief A player as the rules see it ([wp-7] fills the list; empty in [wp-6]).
    struct PlayerView {
        uint8_t player = 0;                  ///< slot + 1
        TeamId team = kNoTeam;
        glm::vec3 feet{0.0f};
        bool alive = true;
    };
    /// @brief A blast's damage: `centre` at distance 0, falling linearly to 0 at `zeroMm` ([wp-7]). centre 0 = none.
    struct BlastDamageRule {
        int16_t centre = 0;
        uint16_t zeroMm = 0;
    };
    /// @brief What ResolveAction may read besides the world.
    struct RulesContext {
        const PlayerTuning& tuning;
        std::span<const PlayerView> players{};
        uint32_t tick = 0;
        BlastDamageRule blastDamage{};
    };
    /// @brief Applies the action: a Blast slot runs sim::Blast(aim.sphere, aim.power) and adds its effect cue.
    /// `aim.hit` false: refusal Missed, nothing applied.
    ActionOutcome ResolveAction(Simulation& simulation, const RulesContext& context, const ActionRequest& request, const ActionAim& aim);

    /// @brief The cue of a CarveSphere operation (a client's view of a cast it applied).
    EffectCue BlastCueOf(const WorldOp& carve);
    /// @brief Cue centre in world metres: dvec3(centreQ) * (1/16 * voxelSize), in that order of operations.
    glm::dvec3 CueCentreMetres(const EffectCue& cue, float voxelSize);
    /// @brief Cue radius in metres: float(double(radiusQ) * (1/16) * voxelSize).
    float CueRadiusMetres(const EffectCue& cue, float voxelSize);
}
```

`engine/src/sim/Sim.cppm`: add the line `export import :rules;` after `export import :player;`.

`engine/src/net/Messages.cppm`, after `const char* ToString(CastRejection rejection);`:

```cpp
    /// @brief The rejection a snapshot reports for a refusal of the rules seam (values are equal; [wp-6]).
    CastRejection RejectionOf(sim::ActionRefusal refusal);
```

`engine/src/engine/Engine.cppm`, next to `PresentBlast`:

```cpp
        /// @brief Shows an outcome of the rules seam: PresentBlast for each of its blasts, in order ([wp-6]).
        void PresentOutcome(const sim::ActionOutcome& outcome);
```

Net internal partitions (cards 2, 6, 7): `engine/src/net/internal/ServerImpl.cppm` (`module vortex.net:server_impl;`)
and `engine/src/net/internal/ClientImpl.cppm` (`module vortex.net:client_impl;`); implementation units
`Server.cpp`, `ServerPeers.cpp`, `ServerCasts.cpp`, `ServerSync.cpp`, `ServerChecks.cpp`, `ServerEntities.cpp`,
`Client.cpp`, `ClientPrediction.cpp`, `ClientSync.cpp`, `ClientEntities.cpp`, `ClientChecks.cpp`, all in
`engine/src/net/internal/`, each starting with the global fragment and `module vortex.net;` followed by the imports
of today's `Server.cpp` / `Client.cpp` and `import :server_impl;` / `import :client_impl;`.

Tests (suite `Rules`, appended to `engine/tests/sim/BlastTests.cpp` to reuse its `Arena`, `Mushrooms`, `StemCut`,
`kStrong`, `ApplyAll` helpers): `Rules.TheBlastSlotGivesTheSameOperationsAsTheOldPath`,
`Rules.AMissedAimRefusesWithMissed`, `Rules.OutsideThePlayBoundsRefuses`.

## Card 1: rules.md and the baselines

| | |
|---|---|
| Goal | The seam has its design document, and the summary lines the later cards must keep are recorded. |
| Depends on | - |
| Effort | 0.5 d (e) |
| Touches | documents only; no code. Wire: no. Movement rule: no. buildHash: no. |

**Read first** (about 40 KiB)
- This pack: "Decisions", "Skeleton".
- `engine/docs/design/fx-budget.md` lines 1-14 (the standard header table: Status / Decided / Next / Open / History / Marks).
- `engine/docs/README.md` "Documents" (the table) and the line for `design/rendezvous.md` as a model row.
- `00 Docs Map.md` (game root): the "Designs" list (lines with `[[engine/docs/design/...]]`).
- `engine/docs/design/networking.md` "Architecture" and "Time and ticks" (the cast queue: capture, then run).
- `CLAUDE.md` section 2 rows G11, G13, G16.

**Files**
- new `engine/docs/design/rules.md`
- change `engine/docs/README.md` "Documents": one row for `design/rules.md`
- change `00 Docs Map.md`: one line under the designs
- scratch (not committed): `build/wp6-baseline/` (`build/` is ignored by git)

**Steps**
1. Write `engine/docs/design/rules.md` with:
   - title `# Rules seam`; nav line `[[../../../00 Docs Map|Docs map]] · [[networking|networking]] · [[magic|magic]] · [[../notes/net|net notes]]`;
   - the header table: Status "Designed; building in [wp-6]"; Decided "Us, 2026-11-22 (the owner may veto): decisions
     1-12 of the [wp-6] task pack"; Next "[wp-6] cards 2-8, then [wp-7] (damage), [mag-1] (spells)"; Open "none";
     History "-"; Marks "code names from engine `bafca16`";
   - `## Decision`: decisions 1, 2, 3, 4, 9 of this pack in 2-4 sentences each;
   - `## The seam`: the Skeleton's types with one line each (copy the doc comments), and the two call sites
     (server: `CaptureCast` captures an `ActionRequest`, `RunCasts` dequeues, `AimAction`, then `RunCast` calls
     `ResolveAction`; offline: click -> `AimAction` -> `ResolveAction` -> `Engine::PresentOutcome`);
   - `## Aim rules`: the two rule sets as a table (field, session value, offline value) from decision 5 and the
     Skeleton;
   - `## Outcome`: what each vector holds and which stage fills it (decisions 6, 7, 8);
   - `## Server shape`: decision 9 and decision 10;
   - `## File map`: the card 6 and card 7 tables (copy them);
   - `## Stages`: a table `| ID | Stage | State |` with one row `| [wp-6] | rules seam and net split | designed |`;
   - `## Open questions`: "None open. Revisit decision 9 (rules as data) when [mag-1] lands its tables."
2. Add the row to `engine/docs/README.md` "Documents", after the `design/magic.md` row:
   `| [design/rules.md](design/rules.md) | decision + plan (the rules seam `vortex.sim:rules`, the net split) | designed | code read at `bafca16` | N |`
   with N = `du -k engine/docs/design/rules.md | cut -f1`.
3. Add to `00 Docs Map.md`, after the magic line:
   `- [[engine/docs/design/rules|Rules seam]] — one GPU-free place where an action becomes operations, damage and effects.`
4. Record the baselines (no build runs meanwhile; the binaries are the current `build/` ones). From the game root:
   ```
   B=build/wp6-baseline; mkdir -p $B
   S=engine/tools/server/sessions; SV=build/engine/tools/server/vortex_server
   for i in 1 2; do
     $SV --session=$S/drill.session --drill=200 --clients=2 --loss=0.05 --delay-ms=40 --jitter-ms=30 --max-join-p99-ticks 15 > $B/drill-$i.log 2>&1
     $SV --session=$S/arena250.session --clients 2 --client-window 48 --walk-across --precasts 40 --join-step 20 --drill 40 --loss 0.05 --delay-ms 40 --jitter-ms 30 --memory > $B/walk-$i.log 2>&1
     tools/session-check.sh > $B/sc-$i.txt 2>&1; cat build/session-check/server.log build/session-check/a.log build/session-check/b.log > $B/sc-$i.log
   done
   ```
   Each `vortex_server` run must exit 0 and `sc-$i.txt` must end with `PASS`; if not, stop (`Status: blocked`).
5. Make the stable sets. Put this function in `$B/summ.sh` (scratch, not committed) and run it:
   ```
   summ() {   # summ LOG: the summary lines without their time stamps
     sed -E 's/^\[[0-9:.]+\] \[[a-z]+\] //' "$1" |
       grep -E '^(Drill: freed|Session: hashes equal|Session: server ends|Session: slot [0-9]+ (applied|entities)|Entities: (server|client)|Client [0-9]+ walk:|Checks: (server|bot [0-9]+|slot [0-9]+):)' | sort
   }
   for k in drill walk sc; do comm -12 <(summ $B/$k-1.log) <(summ $B/$k-2.log) > $B/$k.stable; wc -l $B/$k.stable; done
   ```
   `drill.stable` must hold at least the `Drill: freed` and both `Entities: client N: ... - agree` lines (the drill
   is reproducible bit for bit); if it does not, report it under "Numbers" and go on. `sc.stable` may be empty
   (real ENet on the wall clock); then session-check is judged by `PASS` alone.
6. Write `$B/compare.sh` (scratch) that a later card runs:
   ```
   # compare.sh KIND NEWLOG: every stable baseline line of KIND (drill, walk, sc) must be in NEWLOG's summary
   . "$(dirname "$0")/summ.sh"; B=$(dirname "$0")
   missing=$(comm -23 "$B/$1.stable" <(summ "$2")); [ -z "$missing" ] && echo "equal: $1" || { echo "DIFFERS: $1"; echo "$missing"; exit 1; }
   ```

**Checks**
1. `tools/doc-check.sh --build build` - PASS (the new document's nav line, the index row, the stage ID `[wp-6]`).
2. The six baseline runs: exit 0, `PASS` twice; the counts of the three stable files.

**Docs**
- `engine/docs/design/rules.md` (new), `engine/docs/README.md` (one row), `00 Docs Map.md` (one line).

**Must not break**
- Headings of other documents (nothing renamed). History files: untouched.

**Done when**
- [ ] rules.md exists with the 8 headings of decision 12, the nav line and the header table.
- [ ] `build/wp6-baseline/{drill,walk,sc}.stable`, `summ.sh`, `compare.sh` exist; the report lists their line counts.
- [ ] doc-check PASS; one commit per repository (engine: rules.md and README; game: Docs Map and the submodule).

**Traps**
- Do not put the baselines in a committed folder; `build/` is ignored.
- `tools/session-check.sh` waits for the machine's alone lock; do not start a build while it runs.

## Card 2: skeleton

| | |
|---|---|
| Goal | The seam's types and stub calls compile, the internal partitions are proven to build, and the stage's tests exist disabled. |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | Sim (new partition), Net (one function, one partition probe), Engine (one method); wire: no; movement rule: no |

**Read first** (about 45 KiB)
- This pack: "Skeleton".
- `engine/src/sim/Sim.cppm`, `engine/src/sim/Blast.cppm` lines 1-20 (a partition's preamble).
- `engine/src/sim/internal/Player.cpp` lines 1-20 (an implementation unit's preamble).
- `engine/src/net/Net.cppm`, `engine/src/net/internal/Server.cpp` lines 1-36 and 37-235 (the anonymous namespace
  before `struct Server::Impl`).
- `engine/tests/sim/BlastTests.cpp` lines 1-120 (Arena and helpers) and `TEST(Blast, IslandsBecomeDetachOperationsWithoutEntities)`.

**Files**
- new `engine/src/sim/Rules.cppm` (exactly the Skeleton), `engine/src/sim/internal/Rules.cpp`
- change `engine/src/sim/Sim.cppm`: `export import :rules;`
- change `engine/src/net/Messages.cppm` / `internal/Messages.cpp`: `RejectionOf`
- change `engine/src/engine/Engine.cppm` / `internal/Engine.cpp`: `Engine::PresentOutcome`
- new `engine/src/net/internal/ServerImpl.cppm` (probe only in this card, see step 5)
- change `engine/tests/sim/BlastTests.cpp`: the three `Rules` tests, disabled

**Steps**
1. `Rules.cpp` starts:
   ```cpp
   module;
   #include <glm/glm.hpp>
   #include <span>
   #include <vector>
   module vortex.sim;
   import vortex.core;
   import vortex.math;
   import vortex.voxel;
   import vortex.physics;
   import vortex.scene;
   namespace vortex::sim { ... }
   ```
   Stubs (each with the comment `// [wp-6] card 3 fills this`): `SessionAimRules` and `OfflineAimRules` return
   `AimRules{}`; `AimAction` sets `out = {}` and returns `ActionRefusal::Missed`; `ResolveAction` returns an
   outcome with `refusal = ActionRefusal::Missed`; `BlastCueOf` returns `EffectCue{}`; `CueCentreMetres` returns
   `glm::dvec3(0.0)`; `CueRadiusMetres` returns `0.0f`.
2. `RejectionOf` (Messages.cpp), complete now (it is trivial):
   ```cpp
   CastRejection RejectionOf(sim::ActionRefusal refusal) {
       switch (refusal) {
           case sim::ActionRefusal::None: return CastRejection::None;
           case sim::ActionRefusal::Cooldown: return CastRejection::Cooldown;
           case sim::ActionRefusal::Missed: return CastRejection::Missed;
           case sim::ActionRefusal::Outside: return CastRejection::Outside;
       }
       return CastRejection::Missed;
   }
   ```
   plus, after it, `static_assert(static_cast<uint8_t>(sim::ActionRefusal::Outside) == static_cast<uint8_t>(CastRejection::Outside));`
   (same for Cooldown and Missed).
3. `Engine::PresentOutcome`, complete now: `for (const sim::BlastResult& blast : outcome.blasts) PresentBlast(blast);`
4. Tests (step "Tests" below) written in full, each named `DISABLED_<Name>`.
5. **The partition probe** (decision 10). Create `engine/src/net/internal/ServerImpl.cppm`:
   ```cpp
   module;
   #include <cstdint>
   module vortex.net:server_impl;
   namespace vortex::net::detail {
       // [wp-6] card 6 moves Server::Impl here; this probe proves an internal partition builds.
       inline constexpr uint32_t kServerImplProbe = 1;
   }
   ```
   and in `engine/src/net/internal/Server.cpp`, after the existing imports, add `import :server_impl;` and, inside
   `namespace vortex::net`, `static_assert(detail::kServerImplProbe == 1);`. Build. If it builds warning-free: keep
   both (card 6 replaces the probe). If Clang or CMake refuses it: change the partition line to
   `export module vortex.net:server_impl;` (no `export` on the namespace or the constant; `Net.cppm` unchanged), build
   again, and record which form built under "Deviations" ("internal partition refused: <error line>; interface
   partition used"). Card 6 and card 7 use the form that built.

**Tests** (written now, disabled; card 3 enables them)
- `Rules.TheBlastSlotGivesTheSameOperationsAsTheOldPath` (`BlastTests.cpp`): two `Arena`s `a`, `b`, both
  `ApplyAll(x.World(), Mushrooms(x.Ids()))`. `const sim::BlastResult old = sim::Blast(a.simulation, StemCut(), kStrong);`
  In `b`: `sim::ActionRequest request;` (defaults), `sim::ActionAim aim; aim.hit = true; aim.sphere = StemCut(); aim.power = kStrong;`
  `sim::PlayerTuning tuning; const sim::RulesContext context{tuning};`
  `const sim::ActionOutcome got = sim::ResolveAction(b.simulation, context, request, aim);`
  Assert: `got.refusal == ActionRefusal::None`; `got.blasts.size() == 1`; `got.blasts[0].ops.size() == old.ops.size()`
  and every `got.blasts[0].ops[i].op == old.ops[i].op`; `got.blasts[0].IslandCount() == old.IslandCount()` and
  `== 2u` (the two mushroom caps, as `Blast.IslandsBecomeDetachOperationsWithoutEntities`); `got.blasts[0].crumbs == old.crumbs`;
  `b.Hash().hash == a.Hash().hash`; `got.effects.size() == 1` and
  `got.effects[0] == sim::EffectCue{sim::EffectKind::Blast, StemCut().center, StemCut().radius}`;
  `got.damage.empty()`, `got.impulses.empty()`. Also `sim::BlastCueOf(old.ops[0].op) == got.effects[0]`.
- `Rules.AMissedAimRefusesWithMissed`: one `Arena a`; `before = a.Hash()`; request with `eye = {0.05, 9.0, 0.05}`,
  `dir = {0, 1, 0}` (up into the sky; the ground top is 8 m); `sim::ActionAim aim;`
  `EXPECT_EQ(sim::AimAction(a.simulation, request, sim::OfflineAimRules(), aim), sim::ActionRefusal::Missed)`;
  `EXPECT_FALSE(aim.hit)`; then `ResolveAction(a.simulation, context, request, aim)` gives refusal `Missed`, no
  blasts, no effects, and `a.Hash().hash == before.hash`.
- `Rules.OutsideThePlayBoundsRefuses`: one `Arena a`; request `eye = {0.05, 10.0, 0.05}`, `dir = {0, -1, 0}` (hits
  the ground at y = 8 m, voxel (0, 79, 0), chunk (0, 2, 0)). `sim::RegionDesc far; far.playMinChunk = {1, 1}; far.playMaxChunk = {2, 2};`
  `sim::PlayerTuning tuning;` `sim::AimRules rules = sim::SessionAimRules(tuning, far);`
  Expect `AimAction(...) == Outside`, `aim.hit` true, `aim.atBody` false. Then `sim::RegionDesc near;`
  `near.playMinChunk = {-1, -1}; near.playMaxChunk = {1, 1};` with `SessionAimRules(tuning, near)`: expect `None`,
  `aim.sphere.radius == 192` (1.2 m / 0.1 m x 16), `aim.power == voxel::QuantizePower(6.0f)`.

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning.
2. `tools/ctest.sh -R 'Rules\.' --output-on-failure` - the three show as disabled (not run).
3. `tools/ctest.sh` - all pass.
4. `tools/doc-check.sh --build build` - PASS (update ROADMAP's `Tests: **N** in ctest: M GoogleTest cases` with
   the numbers doc-check names).

**Docs**
- ROADMAP test count only.

**Must not break**
- Layering: `vortex.sim:rules` imports no net module (`vortex_assert_no_network`).
- `[[nodiscard]]`-free stubs must not warn: mark unused parameters `[[maybe_unused]]` or leave them unnamed.

**Done when**
- [ ] The Skeleton compiles as written; `Sim.cppm` exports `:rules`.
- [ ] The partition probe builds (which form, in the report).
- [ ] The three `Rules` tests exist as `DISABLED_`; all other tests pass; doc-check PASS.

**Traps**
- `RulesContext` has a reference member: build it as `sim::RulesContext context{tuning};` or
  `sim::RulesContext{tuning, players, tick}`; it cannot be default-constructed.
- An unused function parameter in a stub is a warning (`-Wunused-parameter`): leave it unnamed in the stub.

## Card 3: the blast behind the seam

| | |
|---|---|
| Goal | `AimAction` and `ResolveAction` do exactly what the server's `ResolveRay` and `sim::Blast` call do today, and the offline click's aim. |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | Sim only; wire: no; movement rule: no; buildHash: no |

**Read first** (about 50 KiB)
- `engine/src/net/internal/Server.cpp`: `ResolveRay` (whole), `CaptureCast`.
- `src/main.cpp` (game): the block `if (playClicks && in.MousePressed(MouseButton::Left))` in `OnUpdate`.
- `engine/src/sim/Blast.cppm` `sim::Blast` comments; `engine/src/voxel/World.cppm` `SphereQ`, `QuantizeSphere`,
  `QuantizePower`, `WorldRayHit`, `World::Raycast`; `engine/src/physics/PhysicsWorld.cppm` `RayHit`, both `Raycast`s.
- `engine/src/sim/Region.cppm` `CastFitsRegion`.

**Files**
- change `engine/src/sim/internal/Rules.cpp`
- change `engine/tests/sim/BlastTests.cpp`: drop `DISABLED_` from the three `Rules` tests

**Steps**
1. `SessionAimRules(tuning, region)`: `range = tuning.castRange; terrainRadius = tuning.castRadius;
   bodyRadius = tuning.castRadius; power = tuning.castPower;
   bodyLayers = physics::LayerBit(physics::Layer::Dynamic) | physics::LayerBit(physics::Layer::Rubble);
   replicatedBodiesOnly = true; strictBodyFirst = false; region = &region;`
2. `OfflineAimRules()`: `range = 80.0f; terrainRadius = 1.2f; bodyRadius = 0.6f; power = 6.0f; bodyLayers = 0;
   replicatedBodiesOnly = false; strictBodyFirst = true; region = nullptr;`
3. `AimAction(simulation, request, rules, out)`:
   1. `out = ActionAim{};` `const double vs = static_cast<double>(simulation.World().VoxelSize());`
   2. `const voxel::WorldRayHit terrain = simulation.World().Raycast(request.eye, request.dir, rules.range);`
   3. Body ray length: `const float bodyMax = rules.strictBodyFirst ? rules.range : (terrain.hit ? terrain.distance : rules.range);`
   4. `physics::RayHit body = rules.bodyLayers != 0
        ? simulation.Physics().Raycast(glm::vec3(request.eye), request.dir, bodyMax, rules.bodyLayers)
        : simulation.Physics().Raycast(glm::vec3(request.eye), request.dir, bodyMax);`
   5. `bool bodyTakes = body.hit;` then, if `rules.replicatedBodiesOnly`:
      `bodyTakes = bodyTakes && simulation.Scene().TryGet<scene::NetId>(simulation.Scene().EntityFromUserData(body.userData)) != nullptr;`
      then, if `rules.strictBodyFirst`: `bodyTakes = bodyTakes && (!terrain.hit || body.distance < terrain.distance);`
   6. If `bodyTakes`: `out.hit = true; out.atBody = true; out.point = glm::dvec3(body.position); out.distance = body.distance;
      out.normal = body.normal; out.voxel = simulation.World().WorldToVoxel(out.point); radius = rules.bodyRadius;`
      else if `terrain.hit`: `out.hit = true; out.point = terrain.position; out.distance = terrain.distance;
      out.normal = terrain.normal; out.voxel = terrain.voxel; radius = rules.terrainRadius;`
      else return `ActionRefusal::Missed` (out stays empty).
   7. `out.sphere = voxel::QuantizeSphere(out.point / vs, static_cast<double>(radius) / vs);`
      `out.power = voxel::QuantizePower(rules.power);` (exactly today's expressions: `ResolveRay` and
      `sim::Blast(Simulation&, const BlastDesc&)`).
   8. If `rules.region != nullptr && !CastFitsRegion(*rules.region, simulation.World(), out.sphere)` return `Outside`.
   9. Return `None`.
4. `ResolveAction(simulation, context, request, aim)`:
   1. `ActionOutcome outcome;` If `!aim.hit`: `outcome.refusal = ActionRefusal::Missed; return outcome;`
   2. `switch (request.slot)`: `case ActionSlot::Blast:`
      `outcome.blasts.push_back(Blast(simulation, aim.sphere, aim.power));`
      `outcome.effects.push_back(EffectCue{EffectKind::Blast, aim.sphere.center, aim.sphere.radius});`
      Comment: `// Damage: [wp-7] card 3 fills this from context.players and context.blastDamage.`
      Reference `context` so it is not unused (e.g. `(void)context;` with that comment).
   3. Return `outcome`.
5. `BlastCueOf(carve)`: `return EffectCue{EffectKind::Blast, carve.sphere.center, carve.sphere.radius};` (callers pass
   a `CarveSphere`; for another kind it still reads `sphere`).
6. `CueCentreMetres(cue, voxelSize)`: `constexpr double kInvQ = 1.0 / static_cast<double>(voxel::kQuantaPerVoxel);
   return glm::dvec3(cue.centreQ) * (kInvQ * static_cast<double>(voxelSize));`
   `CueRadiusMetres(cue, voxelSize)`: `return static_cast<float>(static_cast<double>(cue.radiusQ) * kInvQ * static_cast<double>(voxelSize));`
   These are bit for bit `Client::Impl::Present`'s expressions (card 5 relies on it).
7. Enable the three tests (rename, no other change).

**Tests**
- The three `Rules.*` of card 2, enabled. Expected values are in card 2.

**Checks**
1. Build - exit 0, no warning.
2. `tools/ctest.sh -R 'Rules\.|Blast\.' --output-on-failure` - all pass.
3. `tools/ctest.sh` - all pass.
4. `tools/doc-check.sh --build build` - PASS.

**Docs**
- rules.md "Stages": the row's state "building (card 3 of 8)".

**Must not break**
- `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes`: `ResolveAction` calls `sim::Blast` unchanged.
- CLAUDE.md section 5 "World edits and blasts": integer spheres and powers (`QuantizeSphere`, `QuantizePower`); no
  float decides a voxel beyond what `ResolveRay` already did.

**Done when**
- [ ] `Rules.*` pass enabled; `grep -n "card 3 fills" engine/src/sim/internal/Rules.cpp` finds nothing.

**Traps**
- Do not compute the sphere from `out.voxel`: the centre is the hit point (fractional voxels), as today.
- `strictBodyFirst` changes the body ray length too (80 m offline, terrain distance on the server): both are today's.

## Card 4: the server casts through the seam

| | |
|---|---|
| Goal | The session server aims and resolves every cast through `vortex.sim:rules`; the drill, the arena walk and session-check give their baseline lines. |
| Depends on | card 3 |
| Effort | 1 d (e) |
| Touches | Net (`Server.cpp`); wire: **no** (`kProtocolVersion` stays 10); movement rule: no |

**Read first** (about 45 KiB)
- `engine/src/net/internal/Server.cpp`: `QueuedCast`, `CaptureCast`, `RejectCast`, `Queue`, `PeerOfSlot`,
  `ResolveRay`, `LateCast`, `RunCast`, `RunCasts`, `Server::Start` (where `playArea` is set), `Server::QueueCast` (both).
- `engine/src/net/Server.cppm`: `ServerStats` fields `castsAtBodies`, `castsMissed`, `castsRejected`, `castsQueued`.
- This pack's card 3 (the algorithm the server now calls).

**Files**
- change `engine/src/net/internal/Server.cpp`

**Steps**
1. `QueuedCast`: replace the four fields `origin`, `direction`, `inputTick`, `loadout` with
   `sim::ActionRequest request;   ///< the captured action (ray casts; a server cast keeps the defaults)`.
   Keep `sphere`, `power`, `queuedTick`, `player`, `ray`. Replace every use: `q.origin` -> `q.request.eye`,
   `q.direction` -> `q.request.dir`, `q.inputTick` -> `q.request.inputTick` (grep `\.origin\|\.direction\|inputTick\|loadout`
   inside `Server.cpp` and fix each that belongs to a `QueuedCast`; `CastRecord` keeps its own fields).
2. `Server::Impl`: add the member `sim::AimRules aimRules;   // sim::SessionAimRules(settings.player, region): every player's cast`
   next to `playArea`, and set it in `Server::Start` where `s.playArea` is set:
   `s.aimRules = sim::SessionAimRules(s.settings.player, s.region);` (`region` is a member of `Impl`: the pointer
   stays valid for the server's life).
3. `CaptureCast`: fill `q.request`:
   ```cpp
   q.request.player = static_cast<uint8_t>(p.slot + 1);
   q.request.slot = sim::ActionSlot::Blast;
   q.request.inputTick = input.tick;
   q.request.eye = glm::dvec3(at.position) + glm::dvec3(0.0, static_cast<double>(t.eyeHeight), 0.0);
   q.request.dir = sim::LookDirection(input.yaw, input.pitch);
   ```
   Keep `q.ray = true`, `q.power = voxel::QuantizePower(t.castPower)` (CastRecord reads it), the stats lines and
   `lastCastTick` / `castRejected` exactly.
4. `ResolveRay(QueuedCast& q)` keeps its name and its callers; its body becomes:
   ```cpp
   sim::ActionAim aim;
   const sim::ActionRefusal why = sim::AimAction(*simulation, q.request, aimRules, aim);
   if (aim.atBody) ++stats.castsAtBodies;        // as before: counted when a body took it, even if then Outside
   if (why == sim::ActionRefusal::Missed) ++stats.castsMissed;
   if (why == sim::ActionRefusal::Outside) ++stats.castsRejected;
   if (why == sim::ActionRefusal::None) {
       q.sphere = aim.sphere;
       return true;
   }
   if (Peer* p = PeerOfSlot(q.player); p != nullptr && p->player->lastCastTick == q.request.inputTick)
       p->player->castRejected = RejectionOf(why);
   return false;
   ```
   Keep its doc comment, adding "(sim::AimAction, rules.md \"Aim rules\")".
5. `RunCast(const QueuedCast& q, bool overdue)`: replace the line
   `const sim::BlastResult r = sim::Blast(*simulation, q.sphere, q.power);` by
   ```cpp
   sim::ActionAim aim;
   aim.hit = true;
   aim.sphere = q.sphere;
   aim.power = q.power;
   const sim::RulesContext context{settings.player, {}, tick};
   sim::ActionOutcome outcome = sim::ResolveAction(*simulation, context, q.request, aim);
   const sim::BlastResult& r = outcome.blasts.front();
   ```
   inside the same timed block (`start` before, `seconds` after: the timing covers the same work). Everything after
   it reads `r` as before. A server cast (`Server::QueueCast`, the drill) has `q.request` defaulted: slot Blast,
   player 0 - the same outcome.
6. Build, run the checks, then the three comparisons (step "Checks" 5-7).

**Tests**
- No new test. Every existing net test is the check: `Movement.*`, `Entities.*`, `Replication.*`, `Session.*`,
  `Region250.*`, `Client.*` exercise the cast path.

**Checks**
1. Build - exit 0, no warning.
2. `tools/ctest.sh -R 'Movement\.|Entities\.|Session\.|Client\.' --output-on-failure` - all pass.
3. `tools/ctest.sh` - all pass.
4. `tools/doc-check.sh --build build` - PASS.
5. Drill: `build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/drill.session --drill=200 --clients=2 --loss=0.05 --delay-ms=40 --jitter-ms=30 --max-join-p99-ticks 15 > build/wp6-baseline/drill-new.log 2>&1; echo $?`
   - `0`; then `bash build/wp6-baseline/compare.sh drill build/wp6-baseline/drill-new.log` - `equal: drill`.
6. The arena walk (G13's command, as card 1, into `walk-new.log`) - exit 0; `compare.sh walk` - `equal: walk`.
7. `tools/session-check.sh` - `PASS`; concatenate its three logs into `build/wp6-baseline/sc-new.log` as card 1;
   `compare.sh sc` - `equal: sc`.
8. `grep -n "kProtocolVersion = 10" engine/src/net/Messages.cppm` - one line (unchanged).
9. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS.

**Docs**
- rules.md "Stages": "building (card 4 of 8)". `engine/docs/notes/net.md` section 4 "Recipes", the item "New cast
  kind / loadout": replace "the loadout id is in `QueuedCast` only" with "an action is a `sim::ActionRequest` in
  `QueuedCast::request` (rules.md \"The seam\"); a new slot is a value in `sim::ActionSlot` and a case in
  `sim::ResolveAction`".

**Must not break**
- CLAUDE.md section 5 "Sessions": `CastBudget.CastWorkIsEqualColdAndWarm` (`CastWorkOf(r)` reads the same result).
- "Replicated bodies": "A player's cast hits the replicated body in front of the terrain" (`replicatedBodiesOnly`,
  the body ray to the terrain distance).
- "Chunk sync": every applied op still goes through `EmitOp` in `RunCast`'s loop over `r.ops`.
- The order of work in `RunCasts` (credit, dequeue, aim, `refuseNewPieces`, run) is unchanged.

**Done when**
- [ ] `ResolveRay` contains no raycast; `RunCast` contains no direct `sim::Blast` call.
- [ ] The three comparisons print `equal`, session-check `PASS`, verify quick PASS, protocol 10.

**Traps**
- `PeerOfSlot` compares `lastCastTick` with the request's input tick: keep that comparison, or a rejection lands
  on the wrong cast.
- `outcome` must outlive `r` (a reference into it): declare `outcome` in `RunCast`'s scope, not in a nested block.

## Card 5: the offline click and the client's presentation through the seam

| | |
|---|---|
| Goal | The offline game's left click and a client's cast presentation use the seam; nothing looks or plays differently. |
| Depends on | card 3 |
| Effort | 0.5 d (e) |
| Touches | game `src/main.cpp`; Net (`Client.cpp`); Engine (`PresentOutcome`, already in); wire: no |

**Read first** (about 30 KiB)
- `src/main.cpp` (game): `OfflineGame::OnUpdate` from `if (playClicks && in.MousePressed(MouseButton::Left))` to
  the line with `Key::E`; `m_Tuning` and its set-up (`m_Tuning.height = kPlayerHeight`).
- `engine/src/net/internal/Client.cpp`: `Client::Impl::Present`.
- `engine/src/engine/Engine.cppm`: `Explode`, `PresentBlast`, `PresentOutcome`.

**Files**
- change `src/main.cpp` (game)
- change `engine/src/net/internal/Client.cpp`

**Steps**
1. main.cpp: replace the two `engine.Explode(...)` lines and the two raycasts above them with:
   ```cpp
   sim::ActionRequest request;            // offline: no player slot, the camera's ray (what the crosshair aims at)
   request.eye = cam.position;
   request.dir = cam.Forward();
   sim::ActionAim aim;
   if (sim::AimAction(engine.Simulation(), request, sim::OfflineAimRules(), aim) == sim::ActionRefusal::None) {
       const sim::RulesContext context{m_Tuning, {}, engine.Simulation().Tick()};
       engine.PresentOutcome(sim::ResolveAction(engine.Simulation(), context, request, aim));
   }
   ```
   Keep the comment "Whatever is closest along the view ray: terrain or an object." above it. The offline game
   imports `vortex.engine`, which re-exports `vortex.sim`; if `sim::AimAction` is not visible, add `import vortex.sim;`
   next to the file's other imports.
2. The scripts (`--probe-blast`, `--tower-blast`, `--castle-blast`, `--neck-blast`, `--climb`, the beam) keep their
   direct `sim::Blast` calls: they are tests of the blast itself, not player actions.
3. `Client::Impl::Present`, the `CarveSphere` branch: replace
   ```cpp
   constexpr double kInvQ = 1.0 / static_cast<double>(voxel::kQuantaPerVoxel);
   const double vs = static_cast<double>(world.VoxelSize());
   blast.center = glm::dvec3(op.sphere.center) * (kInvQ * vs);
   blast.radius = static_cast<float>(static_cast<double>(op.sphere.radius) * kInvQ * vs);
   ```
   with
   ```cpp
   // The cast's cue, as the rules seam makes it on the server (rules.md "Outcome"); a client never resolves.
   const sim::EffectCue cue = sim::BlastCueOf(op);
   blast.center = sim::CueCentreMetres(cue, world.VoxelSize());
   blast.radius = sim::CueRadiusMetres(cue, world.VoxelSize());
   ```
   The values are bit for bit the old ones (card 3 step 6).

**Tests**
- No new test (the click is windowed only; the client path is covered by `Client.*`, `Entities.*`, session-check).

**Checks**
1. Build - exit 0, no warning (engine and game).
2. `tools/ctest.sh` - all pass.
3. `tools/doc-check.sh --build build` - PASS.
4. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS (G3-G10's lines as judged; the scripts do not click); look at G3's
   screenshot (crosshair, mode panel, hints).
5. `tools/session-check.sh` - PASS; `compare.sh sc` on its logs - `equal: sc`.

**Docs**
- rules.md "Stages": "building (card 5 of 8)".
- game `docs/owner-checks.md`: add an item "Offline left click after [wp-6]: blasts land where the crosshair is, a
  crate takes a small blast (0.6 m), terrain a 1.2 m one; nothing feels different" (owner check: windowed only).

**Must not break**
- G3-G10 (the scripts are untouched). G15-G16's client hashes (presentation changes no voxel).
- CLAUDE.md section 5 "World edits and blasts": `Engine::PresentBlast` is still the only presentation of a blast.

**Done when**
- [ ] `grep -n "engine.Explode" src/main.cpp` finds nothing; `grep -n "kInvQ" engine/src/net/internal/Client.cpp`
  finds no line in `Present`.
- [ ] verify quick PASS; session-check PASS and equal.

**Traps**
- The offline cast has no cooldown and no charge: do not add `TakeCast` (behaviour change).
- `cam.position` is a `glm::dvec3`, `cam.Forward()` a `glm::vec3`: no conversion needed.

## Card 6: Server.cpp split by concern

| | |
|---|---|
| Goal | `Server.cpp` becomes six files of one concern each, with `Server::Impl` in an internal partition; no behaviour change. |
| Depends on | card 4 (the cast code moves as it is after card 4) |
| Effort | 1 d (e) |
| Touches | Net only; wire: no; behaviour: none (moved code, byte for byte apart from declarations) |

**Read first** (about 60 KiB; do not read Server.cpp whole: it is 130 KiB)
- `engine/src/net/internal/Server.cpp` lines 1-240 (preamble, the anonymous namespace, the start of `Impl` with its
  data members), then each method only when you move it (`grep -n "        <name>(" Server.cpp`).
- `engine/src/net/internal/ServerImpl.cppm` (the probe of card 2).
- This card's table.

**Files**
- change `engine/src/net/internal/ServerImpl.cppm` (the partition: types and `struct Server::Impl`)
- change `engine/src/net/internal/Server.cpp` (keeps the public `Server::` methods and the core loop)
- new `engine/src/net/internal/ServerPeers.cpp`, `ServerCasts.cpp`, `ServerSync.cpp`, `ServerChecks.cpp`,
  `ServerEntities.cpp`

**The map** (every `Server::Impl` member function by file; one-line accessors such as `Tick()` and `LastOpSeq()`
stay defined inside the struct in the partition)

| File | Member functions of `Server::Impl` |
|---|---|
| `Server.cpp` | constructor, destructor, `Emit`, `EmitOp`, `CheckOp`, `Fail`, `Send`, `SendBatch`, `Pump`, `Receive`, `ReceiveFailed`, `DropPeer`, `Flush`, `TrimBacklog`, `CheckStore`, `RefreshHashes`, `CachedHash`, `ColliderFocus`; and every `Server::` public method (`Server::Server` to the end of the file) |
| `ServerPeers.cpp` | `OnHello`, `Refuse`, `DropPlayer`, `PlayersBySlot`, `PlayerFocus`, `PeerOfSlot`, `JoinedCount`, `OnInputs`, `StepPlayers`, `SendSnapshots`, `QueueBaseline`, `MakeBaselineEntity` |
| `ServerCasts.cpp` | `CaptureCast`, `RejectCast`, `Queue`, `ResolveRay`, `LateCast`, `RunCast`, `RunCasts` |
| `ServerSync.cpp` | `QueueChunk`, `ChunkDistance`, `SortChunks`, `PopChunk`, `PruneQueue`, `TerrainQueued`, `TerrainChunkGone`, `FinishTerrainStep`, `OnWindowUpdate`, `OnTerrainWindow`, `OnChunkRequest`, `NextBulk`, `PumpBulk` |
| `ServerChecks.cpp` | `OnEntityListRequest`, `EntityValues`, `SendHashCheck`, `SendChecks`, `OnModelRequest` |
| `ServerEntities.cpp` | `OnNetIdDestroyed`, `DrainDespawns`, `DescribeObject`, `EmitObjectSpawn`, `EmitObjectEvents`, `TrackBodies`, `EntityOf`, `SpawnCrateAt`, `SpawnObjectAt`, `PlayerCrate`, `AwakeSamples` |

The free function `MaskVoxels(const Event&)` and the anonymous-namespace types and constants (`kTickHello`,
`kTickFirstWindow`, `kTickLeave`, `LoggedEvent`, `kStateHistory`, `Player`, `BulkItem`, `JoinRecord`, `Peer`,
`BodyTrack`, `QueuedCast`, `kEyeMetres`, `kMaxReportedMargin`) go to the partition, in
`namespace vortex::net::detail`. If a method is missing from the table (the list was taken at engine `bafca16`),
put it with the concern its name says and list it under "Deviations".

**Steps**
1. Partition preamble (`ServerImpl.cppm`): the global fragment with **every** `#include` of today's `Server.cpp`
   (copy the block), then `module vortex.net:server_impl;` (or the form card 2 recorded), then today's imports
   (`import vortex.core; ... import vortex.sim;`) and the partitions of `vortex.net` that `Server.cpp` uses:
   `import :transport; import :wire; import :messages; import :session; import :sync; import :entities; import :server;`
   (add any other the compiler names).
2. In the partition: `namespace vortex::net { namespace detail { <the moved anonymous-namespace content> } using namespace detail; struct Server::Impl { ... }; }`.
   Constants become `inline constexpr` (a namespace-scope `constexpr` variable has internal linkage and cannot be
   used from another unit of the module). Remove the probe constant of card 2.
3. `struct Server::Impl` in the partition keeps every data member, its nested types (`HashEntry`), the in-class
   one-line accessors, and gets one declaration per moved member function, copied from its definition line with
   the body replaced by `;` (keep `const`, default arguments stay in the declaration only).
4. Each `.cpp` of the table: the global fragment with the `#include`s its code needs (start from all of
   `Server.cpp`'s and remove those that are unused only if the build stays warning-free), `module vortex.net;`, the
   same `import`s as today's `Server.cpp`, `import :server_impl;`, then
   `namespace vortex::net { <ReturnType> Server::Impl::<Name>(<params>) { <body unchanged> } ... }`.
   Move bodies with copy and paste; change nothing inside a body.
5. `OnNetIdDestroyed` is connected as `&Impl::OnNetIdDestroyed` (entt): it stays a member function; nothing changes.
6. Build after each moved file (the build must stay green between files; a half-moved file is fine as long as
   every function is defined exactly once).

**Tests**
- None new. All net tests are the check.

**Checks**
1. Build - exit 0, no warning.
2. `tools/ctest.sh -R 'Wire\.|Session\.|Movement\.|Entities\.|Replication\.|Region250\.|ChunkSync\.|ClientWindow\.|Client\.|Bot\.' --output-on-failure` - all pass.
3. `tools/ctest.sh` - all pass.
4. `tools/doc-check.sh --build build` - PASS.
5. Drill, walk and session-check comparisons as card 4 checks 5-7 - `equal` three times, `PASS`.
6. `wc -c engine/src/net/internal/Server*.cpp engine/src/net/internal/ServerImpl.cppm` - in the report; no file
   over 45 KiB except `Server.cpp` and the partition (if one is, say which).

**Docs**
- `engine/docs/notes/net.md` section 1 "Map": the `Server.cppm` line becomes: "`src/net/Server.cppm`,
  `internal/ServerImpl.cppm` (partition `:server_impl`: `Server::Impl`, its types), `internal/Server.cpp` (the public
  methods, the tick loop, emit, transport), `ServerPeers.cpp` (join, inputs, player steps, snapshots, baseline),
  `ServerCasts.cpp` (the cast queue through `vortex.sim:rules`), `ServerSync.cpp` (chunk sync and bulk),
  `ServerChecks.cpp` (hash, entity checks), `ServerEntities.cpp` (replicated bodies, crates)". Find the current line
  with `grep -n "Server.cppm" engine/docs/notes/net.md`.

**Must not break**
- Every contract of CLAUDE.md section 5 "Sessions", "Chunk sync", "Checks", "Server terrain mode", "Replicated
  bodies": code is moved, not changed. `Server::Impl::EmitOp` keeps its name (CLAUDE.md cites it).
- Lesson L12 / L13: the bulk pacer and the ring code move unchanged.

**Done when**
- [ ] Six `Server*.cpp` files and the partition; `grep -c "Server::Impl::" engine/src/net/internal/Server*.cpp` sums to the number of declarations in the partition.
- [ ] All tests pass; three comparisons equal; session-check PASS.

**Traps**
- An entity in an anonymous namespace is TU-local: used from another unit it fails with "exposes TU-local entity".
  Move it to `detail`, never copy it into several files.
- Default arguments go in the declaration only; repeating one in the definition is an error.
- Keep `static` helper functions of a body local to the file that uses them only if exactly one file uses them;
  otherwise put them in `detail` as `inline`.

## Card 7: Client.cpp split by concern

| | |
|---|---|
| Goal | `Client.cpp` becomes five files of one concern each, with `Client::Impl` in an internal partition; no behaviour change. |
| Depends on | card 6 (the same technique, proven) and card 5 (Present as changed) |
| Effort | 1 d (e) |
| Touches | Net only; wire: no; behaviour: none |

**Read first** (about 50 KiB; Client.cpp is 117 KiB: read by sections)
- `engine/src/net/internal/Client.cpp` lines 1-200 (preamble, anonymous namespace, `Impl`'s data members).
- `engine/src/net/internal/ServerImpl.cppm` (the form that worked in card 6).

**Files**
- new `engine/src/net/internal/ClientImpl.cppm` (`module vortex.net:client_impl;` or the card-2 form)
- change `engine/src/net/internal/Client.cpp`
- new `ClientPrediction.cpp`, `ClientSync.cpp`, `ClientEntities.cpp`, `ClientChecks.cpp` (in `engine/src/net/internal/`)

**The map**

| File | Member functions of `Client::Impl` |
|---|---|
| `Client.cpp` | constructor, destructor, `Generator`, `Fail`, `Leave`, `OnWelcome`, `Receive`, `ApplyPending`; every `Client::` public method |
| `ClientPrediction.cpp` | `Restore`, `ReachCurrent`, `NotCurrentNear`, `PredictTick`, `SendInputs`, `StartPrediction`, `Reconcile`, `LogCorrection`, `OnSnapshot`, `TryReconcile` |
| `ClientSync.cpp` | `SendWindow`, `StartTerrainMode`, `CheckWindow`, `Present`, `Apply`, `Hold`, `ProcessBulk`, `SyncHousekeeping`, `NoteSynced`, `Synced` |
| `ClientEntities.cpp` | `BodyOf`, `MovingLayer`, `Tombstone`, `Untombstone`, `ApplyEntity`, `ShowRemovedVoxels`, `ShowVoxelsInSphere`, `TakeBodySamples`, `TakeReadySamples`, `MoveCopies`, `OnBaselineEntity` |
| `ClientChecks.cpp` | `AddCheckTime`, `OnHashCheck`, `CheckDeferred`, `OnMismatches`, `OwnEntityCheck`, `OnEntityCheck`, `AskModel`, `RemoveCopy`, `OnEntityList` |

The anonymous-namespace content (`SameWorldMaterials`, `kHistoryTicks`, `ClockOf`, `WaitingSnapshot`,
`kMaxWaitingSnapshots`, `WaitingSamples`, `kMaxHeldPerId`, `kMaxHeldEvents`, `HistoryEntry`) goes to the partition
in `namespace vortex::net::detail` (functions `inline`, constants `inline constexpr`).

**Steps**
- As card 6 steps 1-6, with `Client` for `Server` and `:client_impl` for `:server_impl`. The partition imports
  `:client` and the partitions today's `Client.cpp` uses (`:transport`, `:wire`, `:messages`, `:prediction`,
  `:session`, `:entities`, `:sync`).

**Tests**
- None new.

**Checks**
1. Build - exit 0, no warning.
2. `tools/ctest.sh -R 'Client\.|ClientWindow\.|Movement\.|Entities\.|ChunkSync\.|Region250\.|Bot\.' --output-on-failure` - all pass.
3. `tools/ctest.sh` - all pass. 4. doc-check PASS.
5. Drill, walk, session-check comparisons - `equal` three times, `PASS`.
6. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS.

**Docs**
- `engine/docs/notes/net.md` section 1 "Map": the `Client.cppm` line lists the five files and the partition as
  card 6 did for the server.

**Must not break**
- `Movement.NoCorrectionAbove2cmOnFlatGround` (0 corrections); `Client.ASnapshotsSamplesWaitForItsEvents`;
  `ChunkSync.*`; the client's order of work in `Poll` / `StepPlayer` / `RunTick` (moved, never reordered).

**Done when**
- [ ] Five `Client*.cpp` files and the partition; all tests pass; three comparisons equal; verify quick PASS.

**Traps**
- `kHistoryTicks` sizes `std::array` members of `Impl`: it must be visible in the partition before `Impl`.
- `Client::Impl::Present` was changed in card 5: move the changed version.

## Card 8: close [wp-6]

| | |
|---|---|
| Goal | The stage is verified end to end and every document describes the system as built. |
| Depends on | cards 1-7 |
| Effort | 0.5 d (e) |
| Touches | documents; no code unless a check fails |

**Read first** (about 40 KiB)
- `engine/docs/design/rules.md` (whole), `engine/docs/design/networking.md` "Architecture",
  `engine/docs/ARCHITECTURE.md` "7.5 Sessions" (first 40 lines), `engine/docs/ROADMAP.md` "Now" and "Next".

**Files**
- `engine/docs/design/rules.md`, `engine/docs/design/networking.md`, `engine/docs/ARCHITECTURE.md`,
  `engine/docs/ROADMAP.md`, `engine/docs/notes/net.md` (checked, already changed in cards 6-7)

**Steps**
1. `grep -rn "DISABLED_Rules\|card [0-9] fills" engine/src engine/tests` - nothing.
2. Run the checks below; all must pass.
3. rules.md: header Status "Built ([wp-6], 2026-MM-DD; the owner's review pending)"; "Stages" row state "built";
   remove plan wording ("will", "card N") from the sections: they describe the system as it is.
4. networking.md "Architecture": in the bullet that lists the `vortex.net` partitions, add after `:server`,
   `:client`: "(implementation split by concern: internal partitions `:server_impl` / `:client_impl`, rules.md
   \"File map\")"; in the bullet "Simulation vs presentation", add: "A player's cast is aimed and resolved by the
   rules seam (`vortex.sim:rules`: `sim::AimAction`, `sim::ResolveAction`; rules.md)".
5. ARCHITECTURE.md "7.5 Sessions": one paragraph "**Rules seam.** ..." (3-4 sentences: the two calls, who calls
   them, that the client never resolves, link `[rules.md](design/rules.md)`).
6. ROADMAP: move `[wp-6]` from "Next" to "Now" as done (the pattern of the `[wp-2]` paragraph: what the developer
   gets, the checks that passed, "Built; review pending", commits `...`); renumber "Next"; the test count line.
7. Start `VERIFY_SLOT=1 tools/verify.sh soak` in the background (35 min); report its result when it ends.

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh ci` - builds clean (-Werror).
2. `tools/ctest.sh` - all pass; the count matches ROADMAP.
3. `tools/doc-check.sh --build build` - PASS.
4. `VERIFY_SLOT=1 tools/verify.sh full --ci` - PASS (G1-G18; G12 may note a slow preload: rerun G12 alone if it
   fails above 11 s right after ctest, as ROADMAP says).
5. Comparisons drill / walk / sc - `equal`; `kProtocolVersion = 10`; `kPlayerRuleRevision = 1`.
6. `VERIFY_SLOT=1 tools/verify.sh soak` - G19 PASS (background).

**Docs**
- As the steps.

**Must not break**
- Headings cited from code (engine/docs/README.md "Anchors cited from code"): do not rename any.

**Done when**
- [ ] Every check above passes; ROADMAP says built with the commits; the report lists the measured file sizes and
  the comparison results.

**Traps**
- "Built and reviewed" is written only after the owner's review (llm/README "The work loop").

## Values this stage lands

None on the wire. Registry (notes/net.md section 2): no new value. llm/README "Values planned across the packs":
`vortex.sim:rules` with `sim::TeamId`, `sim::ActionSlot` (Blast 0), `sim::ActionRequest`, `sim::ActionOutcome`,
`sim::ResolveAction` as in this pack's "Skeleton" (later packs read the built `engine/src/sim/Rules.cppm`), plus the
names this pack adds: `sim::AimAction`, `sim::AimRules`, `SessionAimRules`, `OfflineAimRules`, `sim::ActionAim`,
`sim::ActionRefusal`, `sim::PlayerView`, `sim::BlastDamageRule`, `sim::RulesContext`, `sim::EffectCue` /
`EffectKind`, `sim::BlastCueOf`, `net::RejectionOf`, `Engine::PresentOutcome`; the partitions `vortex.net:server_impl`
and `vortex.net:client_impl`.
