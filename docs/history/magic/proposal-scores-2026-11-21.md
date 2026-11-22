# Magic proposals: scores and synthesis brief (2026-11-21)

Input: the owner's concept ([owner-concept-2026-11-21.md](owner-concept-2026-11-21.md)) and three independent
proposals for `design/magic.md`: P1 "four form operators and four element tables (minimal operators)", P2 "sixteen
siege spells from four Forms and four Crafts (fun and readability first)", P3 "four cast types x four elements,
determinism, network and cost first". Design only: nothing built, measured or committed.

Facts checked in the code and documents for this scoring (2026-11-21):

- `sim::WorldOpKind` holds 1-6. spread.md claims 7 `IgniteSphere`, 8 `SpreadStep`, 9 `DouseSphere`; shred.md claims
  `ShredSphere = 7`. None of them is built. `kProtocolVersion` is 10. `CastRejection` holds 0-3, and spread claims 4-5.
  `kPlayerRuleRevision` is 1. `kSnapshotOwnerBytes` is 32, and mechanisms takes it to 37. `PlayerButton` holds 1/2/4/8
  (mask 15), and mechanisms takes 16. `PlayerState` holds position, velocity (floats), flags, cooldown and
  lastButtons. `kCastWorkPerTick` is 60 000 and `kOpWork` is 100.
- The client's prediction lead (mechanisms.md, from Client.cpp) is RTT + `Target()` (2 + packetTicks - 1 + jitter) +
  2..3 ticks. On the soak link (40 + 0-30 ms each way) that is about 10-16 ticks (c). Measured soak event latency at
  5 % loss: p50 0.09-0.11 s, quiet p99 0.58-0.79 s (ROADMAP, net-8).
- M2a ([wp-7]) runs offline practice as an in-process server over loopback (review WP7, ROADMAP M2).
- spread.md "The step schedule": "if something is awake ... emits one logical step ... Otherwise nothing is emitted
  and `s` does not move". With only settled pools left, a chunk is awake only when `(s + kChunkPhase) % 10 == 0` for
  the next `s`, so `s` can stall forever. P1's amendment 1 is correct.

## Scores (1-10)

| Criterion | P1 minimal operators | P2 fun and readability | P3 determinism, network, cost |
|---|---|---|---|
| Honours the owner's concept (two axes, 16 spells, roles) | 8 | 9 | 8 |
| Fun, readability, counterplay | 6 | 9 | 7 |
| Fits the engine's contracts | 6 | 6 | 9 |
| Cost and budgets (UHD 620, wire) | 7 | 7 | 9 |
| Buildable stages, small fun first slice | 7 | 5 | 7 |
| Clarity | 8 | 7 | 9 |
| **Mean** | **7.0** | **7.2** | **8.2** |

### P1: why these scores

- **Concept 8.** The two axes are literally the code: 4 operators x 4 tables, no per-spell code. All 16 spells have
  roles. Lost: "deflects incoming projectiles" has no replacement, and with all 16 on R plus the wheel the grammar is
  in the code but not in the player's hands.
- **Fun 6.** The combos are good and fall out of the tables, and fresh knit is blast-proof with counters. But there
  is no telegraph: pushes act at the cast tick, so every push is a correction for its target. There is no loadout, no
  HUD design beyond one line, no time-to-kill model, and 16 spells sit on a cycle key.
- **Contracts 6.** Strong: integer operators with total-order keys ending in (z, y, x), never-rules pinned by tests,
  wall rules, one new op that decides from the old id, minimal masks, a configure grep, and a real bug found in
  spread.md (the stalled step clock). Problems:
  - **Carry** is a server-set position outside `sim::StepPlayer`. That breaks "a player moves only through
    `sim::StepPlayer`", and it is a correction every time.
  - **A load check after every fill** goes against the spirit of "never after `FillBox`" (no surprise collapse under
    your own wall).
  - **Lifetimes as aging ids under `SpreadStep`.** In a session, Metal and Frost then need spread S2/S3, which are in
    Later, and conjured matter's life depends on the spread credit.
  - **Pierce bores as `ClearMask`.** Exact, but heavier on the wire than a capsule.
- **Cost 7.** Per-item estimates, visit caps and a scratch size are there, each with the measurement that replaces
  it. ClearMask bores and per-spell masks cost more bytes than capsules and shapes.
- **Stages 7.** [magic-1] Pressure needs no new ids and no spread: the smallest slice of the three. But Metal and
  Frost in a session wait for [spread-S2] and [spread-S3].
- **Clarity 8.** Dense but exact. Every operator can be implemented from the text.

### P2: why these scores

- **Concept 9.** The most faithful. All 16 spells keep the owner's names, roles and feel, including projectiles,
  "deflect" and a lobbed Pour over battlements. Structural health appears as hardness, bore cost and tiers later.
- **Fun 9.** The best player design:
  - readability rules (the Form is the silhouette, the Craft is the colour; one look per state; no one-spell kill);
  - a 0.4 s arm ring as both the telegraph and the prediction window;
  - a counter for every spell, a time-to-kill table against M2a and team roles from Craft pairs;
  - a HUD within game-ui.md's rules;
  - attunement to 2 Crafts per life, which keeps the two-axis grammar visible.
- **Contracts 6.**
  - Good: every op decides per voxel; knit masks never include a player capsule; knit placement runs no load check;
    zones are predicted without correction.
  - **Server-simulated bolts** with an 8-tick catch-up and no rewind are a new server system with new events. Hits
    on moving players are taken at their present positions. That works against M2b's "lag-compensated hits" and the
    eye rewind casts already use (`LateCast`, `StateAt`).
  - **Lapses** (the reverse table over the same shape) are inexact. Overlapping casts revert early, and a voxel
    re-knit by another cast inside the old mask is unknit with it.
  - Four new op kinds plus `BoltLaunch`, `BoltEnd`, `ZoneSpawn` and an `Attune` message: the largest wire surface.
- **Cost 7.** Bytes and CPU are costed, and macro-cells cut the shape work by 8x. The bolt tick cost is small, but
  per-bolt events and staged parts add up.
- **Stages 5.** The first slice holds the framework, bolts, lapses, zones, a knit FIFO, the HUD bar, the attune menu
  and the protocol bump: 10-14 d. It is not small.
- **Clarity 7.** Well organised and easy to read, but long, with engine details spread between the play and mapping
  sections.

### P3: why these scores

- **Concept 8.** All 16 spells are in one matrix with the right roles. The Pressure combos and the felling play are
  good. "Deflect" becomes a dispel of pending enemy zones, which is a sound replacement. A loadout of 4 of 16 hides
  the two-axis grammar a little.
- **Fun 7.** The zone lead doubles as a telegraph, heavy breachers cost 2 charges, and Rising Spire's ride-up lift
  (a Lift zone plus growing bands) is the best answer to "instant scaling tower". There is less player-facing design
  than P2 (HUD, colours, time to kill).
- **Contracts 9.** The replication map puts every effect in exactly one class. Other strengths:
  - the `KnitRegistry` gives exact, owner-scoped expiries as self-contained ops: a scheduling error changes
    lifetimes for everyone and never splits peers;
  - zones are evaluated inside `StepPlayer` from quantised feet, fall off to 0 at the edge, and their outward push is
    cut at the play area;
  - the int64 capsule test is argued, and the Pierce walk agrees with its op;
  - folding spread's two sphere ops into one `MapShape`.
  Problems:
  - **Fixed op numbers** (7/9/10, `ShredSphere` moved to 11) clash with spread.md and shred.md on paper. The rule
    should be P1's: a kind takes the next free value when it lands.
  - **The 12-tick lead** is shorter than the measured client lead on the soak link (about 10-16 ticks), and far
    under the soak's p99 event latency. Zone-late corrections would be common in G16 and G17.
  - **"Offline, no wire" for [mag-1]** collides with M2a, whose offline practice is an in-process server over
    loopback.
  - Brittle as hardness only is a legitimate choice, and it reads the owner's "2x" exactly.
- **Cost 9.** The best budget work:
  - work units calibrated to ms;
  - follow-ups charged at `T_run`;
  - bytes per stream against each credit;
  - ring wrap time and client apply per op;
  - re-cooks only on shape changes;
  - fx caps per effect and acceptance numbers for each.
- **Stages 7.** A 6-spell offline core, then one session stage with the single protocol bump, then Frost + Liquid
  and Thermal with no further bump. Sensible, but its first playable slice arrives only with [mag-2].
- **Clarity 9.** Tables and the replication map make it the easiest proposal to build from and to review.

## Synthesis brief

### Base

**P3** is the base: its replication map, op set, `KnitRegistry`, zone model in `StepPlayer`, caps, budgets, tests and
stage shape. Its section structure becomes `design/magic.md`. Its player-facing parts are replaced by P2's, and its
footprint definitions by P1's exact operators.

### Take from P1

1. **The exact operator specifications** replace P3's footprint prose:
   - Flow is a fall phase, then a lowest-first priority flood that never rises, with a capped `Front` pool.
   - Plume is its exact mirror.
   - Burst is the surface sweep with +-1 step along the normal, an in-plane radius and the leading half of its rim.
   - Pierce is a slice-by-slice budget walk.
   - Also: seeds and footprint discs; cell classes (`open`, `support`, liquids as support so spells slide over
     moats); wall rules (outside the window, the ceiling or the play bounds reads as support; offline, an unloaded
     chunk is a wall); heap keys ending in (z, y, x); per-operator visit caps; work = visits + distinct chunks read.
   - P3's flood box (+-96 voxels) and ceiling clamp stay on top.
2. **The never-rule tests**: `FlowNeverRises`, `PlumeNeverFalls`, `BurstStaysWithinOneStepOfItsSurface`,
   `PierceStopsAtBedrockAndWhenTheBudgetIsSpent` (golden depths), `MirroredSetupGivesMirroredRoles`,
   `WorkIsEqualColdAndWarm`.
3. **The server emits only voxels that change**: masks are exact and minimal.
4. **Temporary pieces never take a replicated slot.** A piece holding conjured ids is client rubble with its
   lifetime. This replaces P3's `KnitExpiry` component and `DespawnReason::Unknit`, so no new despawn reason is needed.
5. **The op-numbering rule**: notes/net.md section 2 is the registry, and every new kind (and `CastRejection`,
   `EventKind`) takes the next free value when it lands. This replaces P3's fixed 7/9/10 and its move of
   `ShredSphere` to 11.
6. **Amendments to spread.md**: (a) the stalled step clock (a slot with both credits >= 0 and nothing awake still
   advances `s` silently; test `Spread.PhaseGatedContentAdvancesWithNothingElseAwake`); (b) sites fire on
   collider-solid to **any** non-collider (thaw to water, film).
7. **Determinism traps**: the configure grep over `Propagate*`, `Footprint*` and `Magic*`; `--check-spells` in debug
   builds (the server's written world equals its decoded ops applied through `ApplyOp`); `kAgePhase`-style tables in
   world chunk coordinates only.
8. **Fresh conjured metal resists the basic blast, with explicit counters** (Pierce, Thermal, Frost). Use P2/P3's
   knit hardness 7.0 (a multiple of 1/16, over power 6), not aging tiers.
9. **Engine names of the forms**: `Flow`, `Plume`, `Pierce`, `Burst`, the owner's own alternates. The HUD shows the
   same words (FLOW / PLUME / PIERCE / BURST, at most 6 glyphs), so one name set serves code, documents and players.
   "PLUME + FROST" carries no contradiction, which answers P2's objection to "FLAME + FROST".
10. **Smallest-slice thinking**: the first slice needs no element tables beyond knit and no spread (see "Stages").
11. **`Magic.CrushingWeightOnNaturalGroundReleasesNothing`** and the `--crush-demo` calibration yard, kept under P3's
    name `Neck.ExtraLoadNeverFailsNaturalTerrain`.

### Take from P2

1. **The whole "Play first" section**, adapted to the settled answers below:
   - the readability rules (the Form is the silhouette and the motion, the Craft is the colour and the material; one
     look per state; nothing conjured lasts; no single spell kills from full health; hazards do not stack within a
     Craft);
   - Craft colours with a distinct particle shape each, for colour-blind players;
   - the HUD spell bar, the Craft panel, crosshairs per Form, statuses beside health and refusal words, all on
     `HudCanvas` (no ImGui);
   - the time-to-kill table against M2a and its targets (one attacker 1.5-3 s, a coordinated pair about 1 s);
   - team roles from Craft pairs and the Felling-duel flow;
   - the combo ranking and the counters column for each spell.
2. **Attunement: 2 Crafts per life** (8 spells), chosen on the respawn screen. Keys 1-4 pick the Form, R swaps the
   Craft. This replaces P1's "all 16 on a cycle" and P3's "4 of 16": it keeps the owner's grammar in the player's
   hands, keeps the enemy readable (two Craft colours at the hands) and turns combos into team play.
3. **Resource model**: 1 M2a charge per spell, plus a **per-Form cooldown** (Pierce 5 s, Burst 7 s, Flow 9 s,
   Plume 9 s). The first tuning knob is P3's 2-charge cost for the heavy breachers.
4. **The 24-tick arm** as the zone lead default (see conflict 2), with P2's contracting ring and P2's 0.3 s ghost for
   knit matter.
5. **Spell shapes**:
   - Rebar Lance's peg is always horizontal (yaw only), 1.25 m out: a three-peg ladder.
   - Barricade Wave is a strip across the aim that follows the ground, 1.5 m tall (over jump-plus-step reach, under
     the 1.62 m eye) and about 4.5 m wide. Players on the line are left a gap.
   - Rising Spire has ledges every 1.0 m on the caster's side.
   - Permafrost Climb leaves ice ledges.
6. **`ice_water` / floe as `Indestructible`**: a graph anchor with no new graph rule. P1 does the same. It replaces
   P3's `Floats` flag (the cheap path). Its counters are Thermal (Heat maps it to water) and Pressure's Shatter map
   (a map, not a carve, so the bridge can be shattered).
7. **`CarveCapsule` for every bore** (P2 and P3 agree): about 30 B against 1-2 KB for a diagonal ClearMask.
8. **`ChunkChange::shapeChanged`**: id-only maps never re-cook colliders. Whichever of [spread-S1] and the magic
   stage that first needs it lands first builds it.
9. **Lobbing Flow**: Flow's ray is an instant **integer parabolic trace** at capture (server, quantised once, R3),
   not a flying bolt. Molten Pour and Napalm can then be lobbed over battlements as the concept describes, and the
   presentation draws the arc. This is the cheap path of P2's Pour bolt.
10. **Fall-damage immunity after a Launch** until landing, so a juggle sets up shots and never kills by itself.
11. **Risks and owner checks**: mask byte peaks in a brawl, the feel of slick ground and launches (windowed, owner
    only), and knit stalling Fellings (lifetime and counters are the levers).

### Conflicts and the recommended answers (us; the owner may veto)

1. **Projectiles or hitscan?** **Hitscan**, as casts are today: the server's eye-rewound capture (`LateCast`), a
   presentation streak, and Flow's instant arc trace (P2 item 9).
   - Rejected: P2's server-simulated bolts. They are a second hit system with no rewind of the target, new events,
     and more bugs, and M2b asks for lag-compensated hits.
   - "Deflects incoming projectiles" becomes P3's dispel: Shockwave cancels pending enemy zones within 5 m that start
     after it.
   - Revisit bolts only if play asks for dodgeable spells.
2. **How do forced movements reach the player?** **Zones announced ahead and evaluated in `sim::StepPlayer`** (P3's
   model: quantised feet, fixed order, falloff to 0 at the edge, outward push cut at the play area, `ZoneCancel`).
   - **The lead defaults to 24 ticks (0.4 s)**, a session setting, which is P2's arm and telegraph. P3's 12 ticks is
     under the measured client lead on the soak link (about 10-16 ticks) and under its loss tails.
   - Late zones are inserted, replayed and counted (`Client: zone late by N ticks`), never dropped.
   - Rejected: P1's push at the cast tick (one correction per push) and **P1's carry teleport**, which moves a player
     outside `StepPlayer`. Rising Spire uses P3's Lift zone with growing bands. Frost "encases" by skipping capsule
     cells and rooting the player through a zone.
   - `kPlayerRuleRevision` becomes 2 once. `PlayerState` gains `push` and status counters. The `UpdateCharacter`
     vertical parameter is P3's.
3. **How do lifetimes work?** **P3's server-only `KnitRegistry`** with exact, owner-scoped expiry `MapMask`s and one
   id per conjured kind.
   - Rejected: P1's aging ids under `SpreadStep`, which tie Metal and Frost in a session to [spread-S2] / [spread-S3]
     and to the spread credit.
   - Rejected: P2's reverse-table lapses, which revert overlaps early and can unknit another cast's matter.
   - The warning before crumbling is presentation derived from `CastFx` and the spell table (P3), not a `fading` id.
4. **Op set.** P3's three kinds: **`MapShape`** (sphere, capsule, box, y-cylinder + map), **`MapMask`** (RLE mask +
   map) and **`CarveCapsule`**. Values take the next free numbers when they land (P1's rule).
   - spread.md's `IgniteSphere` and `DouseSphere` fold into `MapShape` maps 1 and 2. Amend spread.md before S1. Its
     `SpreadStep` keeps its own kind.
   - The turn table is `voxel::TurningTable` with `TurningTableHash` in `buildHash`.
5. **Does a knit fill run the load check?** **No** (P2, P3; the spirit of "never after `FillBox`"). Knit weight
   counts in later checks, and knit density is low (1 500-2 000).
   - Expiries, shatters and carves run site checks, on the structure credit (SI-3's queue, or its minimal form).
   - Crushing Weight runs one on purpose.
6. **Brittleness (rime)?** **Hardness halved, bonds unchanged** (P3). The owner's "2x from Pressure or Solid" holds
   exactly through the carve threshold and the bore cost, the load check is unchanged, and rime needs no site check.
   - P2's shear and tension x0.5 is the documented first knob if chill-fellings are wanted after play.
   - P1's halved bonds are rejected: Frost alone would fell towers.
7. **Crushing Weight?** **P3's integer `extraLoad`** input to Part 4b's check, at the zone tick, with a Pin zone.
   - Add P2's Shatter of brittle and ice in its disc.
   - It needs the [si-2] `NeckCheck` split, so it lands in the stage after [si-2].
   - Pinned: `Neck.ExtraLoadNeverFailsNaturalTerrain`, `Neck.ExtraLoadReleasesAWeakFloor`.
8. **Thermal residue?** **A spread burning class** (P3's `pitch`, P1's `cinder`). It is a non-collider film that
   ignites timber by the one fire rule, burns out by itself (no registry entry) and never goes down.
   - The Thermal stage therefore follows [spread-S1] offline and [spread-S2]/[spread-S3] in a session.
   - Rejected: P2's `embers` coating with a lapse, a second burning mechanism beside spread's.
   - Hot metal (`knit_iron_hot`, `iron_hot`) and the server hazard scan are common to all three: keep them.
9. **Liquid form and spread S4?** **One-shot server Flow** (all three). No spell leaves standing liquid, so
   [spread-S4] stays in Later and is not needed by magic. spread.md's Kindle and Pour spells stay only as drill and
   test casts, as P1 suggests.
10. **Selection and keys.** LMB stays the M2a blast. RMB casts the selected spell. Keys 1-4 pick the Form, R swaps the
    attuned Craft, and the wheel cycles Forms. Q (crate) and E (use) stay mechanisms'.
    - `PlayerInput::spell` u8 holds the `SpellId` (form x 4 + element).
    - The Spell edge is WP6's generic action slot, or `PlayerButton` 32 after mechanisms' 16.
    - The owner confirms the keys windowed.
11. **Caps.** P3's: 6 000 live conjured voxels per player (the oldest conjuring crumbles at once: an info line, not a
    refusal), 60 000 per region and 40 000 turned voxels per region (`TooMuchConjured`), 4 zones per player and 32
    per region (`TooManyZones`), at most 8 bodies woken per Pressure cast, and spread's `TooMuchFire` for Thermal.
12. **Staged reveal or instant?** **Instant**, presented as a sweep (P1), except where the growth is the gameplay.
    - Exceptions: Rising Spire's bands under its Lift zone and Barricade's 4 bands within 15 ticks (P3).
    - Bands are computed at `T_run`, charged whole there, and re-exclude player capsules at emission.
    - At most 6 bands per spell, which keeps the ring well under L12's wrap.
13. **Damage numbers.** P2's table, with its rule that no spell kills from full health. Damage, damage over time and
    statuses stay server-only (all three).
    - Friendly fire is off.
    - Updraft and the Spire's lift move everyone. Root, Pin, Chill and Impulse hit enemies only. Terrain affects
      everyone.
14. **Names.**
    - Forms: Flow / Plume / Pierce / Burst. Elements in code: Metal / Thermal / Pressure / Frost. Setting crafts
      (setting.md section 9): Knitting / Kindling / Drawing / Turning.
    - The owner's 16 spell names are working names and get setting.md's exact-phrase check before release.
    - "Napalm" is a real-world weapon name. Offer "Pitchfall" (P3) or "Cinder Pour" (P1) for the owner to pick.
15. **Progression ("leveling raises HP").** Not in v1, for PvP fairness. Later tiers are table rows with appended ids
    (P1, P2) or thickness and lifetime first (P3). Three ids stay reserved.
16. **ROADMAP placement.** [mag-1] and [mag-2] go right after [wp-8] and before [mech-1] (P2 and P3 agree: spells are
    M2b's main verb, measured inside [wp-8]'s fight budget). [mech-1] then takes the protocol number after [mag-2]'s.
    - The Frost and Flow stage follows [si-2] (for `extraLoad`).
    - Thermal follows [spread-S1] offline and rides [spread-S2]'s bump in a session.
    - This reorders "Next" items 8 onwards, so the owner is told and may veto.

### Stages for the synthesis (each verified, reviewed and committed)

- **[mag-1] Siege core in sim, headless.** GPU-free: Pierce, Burst and Plume operators (P1's specifications),
  `CarveCapsule`, `MapMask`, `MapShape` as sim ops, `knit_iron`, the `KnitRegistry` with expiries and site checks, the
  `ZoneSet`, and `StepPlayer` revision 2 (Impulse, Lift, Root).
  - Four spells: **Siege Cannon, Barricade Wave, Shockwave, Updraft** (breach, cover, knock off, climb), plus the
    `--spell-demo` and `--siege-matrix` rows and tests.
  - No new tables beyond Knit and Unknit, and no spread.
- **[mag-2] First playable.** The same four spells over the wire, offline through M2a's in-process server and in
  sessions.
  - The one protocol bump reserves every kind, zone kind, map number and rejection the later stages need.
  - Also: `CastFx`, zone events with the 24-tick lead, `PlayerInput::spell`, the HUD bar and the attune menu (for the
    two Crafts the slice has).
  - Gates: `--bot-spells`, session-check with spells, `Movement.ZonesPredictedWithoutCorrection`.
  - The owner's Felling-duel taste check.
- **[mag-3] The rest of Metal and Pressure.** Rebar Lance, Rising Spire (Lift + bands), Molten Pour (Flow operator,
  the arc trace) and Crushing Weight (`extraLoad`, after [si-2]). No protocol bump.
- **[mag-4] Frost.** Rime, ice and floe ids, the Rime, Thaw, Freeze, FreezeWater and Shatter maps, `Slick`, Chill and
  Root, and the combo tests. All four Frost spells. No protocol bump.
- **[mag-5] Thermal.** The film burning class, heat and cool, and the hazard scan, after [spread-S1] (offline) and
  [spread-S2]/[spread-S3] (session). All four Thermal spells.
- **[mag-6] Presentation and tuning.** fx-budget effects per Craft, rings, crumble warnings, steam and frost mist (fog
  when built), within `--stress-spells` 4 ms CPU / 4 ms GPU, then the owner's taste check of lifetimes, charges and
  damage.

### What the merged design must still state explicitly

- **Owner block bytes after mechanisms**: 37 + about 11 = about 48 B per snapshot (c).
- **`buildHash` additions**: `TurningTableHash` and the spell-table hash. The ids go through `MaterialTableHash`.
- **Contract rows for CLAUDE.md section 5**, from P3 section 11, as amended above:
  - zones only with a lead;
  - nothing else changes a player's step;
  - the registry principle;
  - no knit load check;
  - temporary pieces take no slot;
  - the op-numbering rule.
- **Every (e) keeps its named measurement.** The `Spell:` / `Magic:` lines, the per-stage gates and the
  `--siege-matrix` screenshot are added to docs/verify.md by the stage that builds them.
