# Magic: sixteen siege spells from four forms and four elements

[[../../../00 Docs Map|Docs map]] · [[../README|docs index]] · [[../ROADMAP|roadmap]] · [[../ARCHITECTURE|architecture]] · [[spread|spread]] · [[shred|shred]] · [[networking|networking]] · [[mechanisms|mechanisms]] · [[fx-budget|fx budget]] · [[fog|fog]] · [[game-ui|game UI]] · [[../../../docs/world/setting|setting]]

| | |
|---|---|
| Status | Designed and reviewed, nothing built or measured for it. Merged from three proposals by a judge's synthesis brief; reviewed by two critics on 2026-11-21 (29 findings on 27 issues, all taken, three only in part: [design-review-2026-11-21.md](../history/magic/design-review-2026-11-21.md)). |
| Decided | - Owner's concept (2026-11-21), verbatim in [owner-concept-2026-11-21.md](../history/magic/owner-concept-2026-11-21.md), not reopened here: every spell is a **Cast Type** (Liquid: down and pooling; Flame: up and rising; Solid: linear penetration that spends structural health per voxel; Torrent: lateral or radial burst) x an **Element** (Metal: create and reinforce voxels; Thermal: damage over time, ignition, heats metal; Pressure: impulse, knockback, launch, bonus damage to brittle voxels, no residue; Frost, the owner's recommended 4th: slow, freeze liquid into walkable ice, brittleness 2x against Pressure and Solid). All 16 named siege spells and their tactical roles.<br>- Owner, standing rules: fun gameplay with few bugs first; realistic synced simulation is unnecessary; we settle open design questions ourselves with a recommended answer; every feature has a cheap path and a measured cost; replicated world edits are integer-exact and no float decides which voxel changes; no randomness in replicated rules; everything stays original.<br>- Owner (2026-11-17, [spread.md](spread.md)): fire is lockstep, a simple spread operation, no randomness; Claude implements it. Owner (2026-11-10, ROADMAP "Direction"): building only in fights, created matter is temporary; magic means create matter, move matter, change materials, spreading fire and summoning.<br>- Us (this design, 2026-11-21): items 1-16 of "Decision in brief" and every answer in "Open questions", as amended by the design review. Base: proposal 3 (8.2 of 10), with proposal 2's player design (7.2) and proposal 1's exact operators (7.0) ([proposal-scores-2026-11-21.md](../history/magic/proposal-scores-2026-11-21.md)).<br>- Us, ROADMAP placement under the owner's standing rules (the owner may veto): [mag-1] and [mag-2] right after [wp-8] and before [mech-1] ("ROADMAP placement"). |
| Next | [mag-1] the siege core in sim, headless only, in ROADMAP "Next" after [wp-8]. |
| Open | "Open questions" 1-29, each with the recommended answer. For the owner: the in-world name for "Napalm" (question 19), the keys, colours and feel of slick ground and launches (windowed, question 23), the reorder of ROADMAP "Next" (question 24), and the damage split between Thermal and Pressure (question 27). |
| History | [owner-concept-2026-11-21.md](../history/magic/owner-concept-2026-11-21.md): the owner's concept, verbatim<br>[proposal-scores-2026-11-21.md](../history/magic/proposal-scores-2026-11-21.md): the scores of the three proposals, what the merged design takes from each, and the conflicts settled<br>[design-review-2026-11-21.md](../history/magic/design-review-2026-11-21.md): the two critics' findings and what was done with each |
| Marks | **(m)** measured (quoted from ROADMAP), **(c)** computed from sizes, **(e)** estimate; every (e) names the measurement that replaces it. Voxel counts are at 0.125 m. Work units are `sim::CastWorkOf` units (about 0.11 us each on the UHD 620 laptop: 10 000 units = 1.09 ms). Code read at engine `58f2650`. |

Related:
- [spread.md](spread.md): `SpreadStep`, the fire classes and credits, the Ignite and Douse maps (folded into this
  design's `MapShape`), `ChunkChange::shapeChanged`, and two amendments found here ("Amendments to other designs").
- [shred.md](shred.md): Part 2 (R1-R7), Part 4b (the load check, which gains an optional `extraLoad`), the SI plan
  (`BlastOptions::noCarve`, the structure credit, site checks).
- [networking.md](networking.md): the op stream and opSeq, the cast queue and its credits, `sim::CastWorkOf`,
  "Player movement" (one movement rule, `sim::StepPlayer`, `kPlayerRuleRevision`), "Entity replication" (slots and
  caps); [notes/net.md](../notes/net.md) section 2 is the registry of wire values.
- [mechanisms.md](mechanisms.md): its wire values (event kinds, `PlayerButton_Use`, the owner block 32 -> 37 B), the
  E and Q keys, and its blocked-gate rule, which conjured matter meets like terrain.
- [fx-budget.md](fx-budget.md) (the `FxDirector`, the 4 ms effects caps), [fog.md](fog.md) (dust and smoke puffs),
  [game-ui.md](game-ui.md) (the HUD rules: `HudCanvas`, the 5 x 9 font, no ImGui in the game).
- [worldgen.md](worldgen.md) (W4's appended ids) and the game's [setting.md](../../../docs/world/setting.md) section 9:
  Knitting (fill, conjured matter that does not last), Kindling (fire), Drawing (move matter), Turning (swap
  material).

## Decision in brief

1. **A spell is a form operator applied with an element's verbs. There is no per-spell code.** The owner's four cast
   types are the **forms**; their engine and HUD names are the owner's own alternates: **Flow** (Liquid), **Plume**
   (Flame), **Pierce** (Solid), **Burst** (Torrent). A form is one integer operator that decides **where** a spell
   reaches. The four **elements** (Metal, Thermal, Pressure, Frost) decide **what** happens there: id maps, a carve,
   a knit fill, a player zone. The 16 spells are one 4 x 4 table of integer parameters, `sim::SpellTable`.
2. **The reach is solved once, on the server, and sent as ops** (shred.md R1). Each operator is a bounded integer
   search with a total visit order (heap keys ending in (z, y, x)), a visit cap and work counted from visits and
   distinct chunks read (R4, R5). Clients never run an operator: they apply ops. Spread's fire stays the only lockstep
   rule, and Thermal spells feed it.
3. **Three new op kinds, numbered when they land:** `MapShape` (an id map over a sphere, capsule, box or vertical
   cylinder), `MapMask` (an id map over an RLE voxel mask) and `CarveCapsule` (the Pierce bore). Each decides every
   voxel from (voxel, old id, op integers, an integer table), so ChunkClip, ring replay and HashCheck hold unchanged.
   Masks hold only the voxels that change. spread.md's unbuilt `IgniteSphere` and `DouseSphere` become `MapShape`
   maps 1 and 2.
4. **Conjured matter is terrain, not bodies.** Knit iron, slag, ice, floe and pitch are world-palette ids: static
   colliders that are climbable, cover, carvable, hashed, synced to joiners and under no body cap. A piece released
   later that holds a conjured id never takes a replicated slot: it is client rubble with its lifetime.
5. **Lifetimes come from a server-only `KnitRegistry` that emits exact expiry ops.** Each conjured or turned voxel has
   an owning cast and a due tick, on the server only. At the due tick one `MapMask` (Unknit, Thaw or Cool) covers the
   voxels that cast still owns. A scheduling mistake can only make matter last too long or too short for everyone; it
   can never split peers.
6. **Everything that moves or roots a player is a zone announced ahead and evaluated inside `sim::StepPlayer`.** A
   zone reaches every peer as an opSeq event at least **24 ticks (0.4 s) before it starts** (the lead, a session
   setting). The server and the predicting client test membership from the player's own state in the same
   `StepPlayer`, so a client that holds the event in time makes no correction. The same 0.4 s is the readable
   telegraph: a ring that contracts, then fires. Late zones are inserted, replayed and counted, never dropped. A root
   or pin is followed by 1.5 s in which no root or pin applies, so control never chains into a lock.
   `kPlayerRuleRevision` goes up in each stage that adds a zone kind or the slick rule (2 in [mag-1]).
7. **Casts stay hitscan** with the server's eye-rewound capture (`StateAt`), as blasts are today. Flow is lobbed: an
   instant integer arc trace at capture, not a flying bolt. Projectile flight is presentation only. Shockwave's
   "deflect" becomes a dispel: it cuts short the live enemy zones near it (lifts, pins, chills) from its own zone
   tick, and frees rooted or pinned allies.
8. **Fresh knit iron holds against the basic blast, but wears**: hardness 7.0 against power 6.0, and a blast whose ray
   hits it is absorbed and chips a 0.25 m patch off the plate instead of carving. Explicit counters: Siege Cannon bores
   it, Thermal heats it soft, Frost rimes it brittle, blasts wear it down, and it crumbles on schedule. Its bonds equal
   stone's, so plating replaces a cut neck but never makes it stronger.
9. **Brittleness is hardness only.** Frost turns stone, dark stone, brick, iron and knit iron into rimed ids at half
   hardness with unchanged bonds. The owner's "2x from Pressure and Solid" holds exactly: the carve threshold and the
   bore cost both read hardness. The load check is unchanged, and rime needs no site check.
10. **A knit fill never runs the load check** (the spirit of "never after `FillBox`": no surprise collapse under your
    own wall). Any op that turns a collider into a non-collider (air, a liquid, a film) runs a site check, whatever
    its map: expiries, shatters, ablation, Heat on ice and floe. Bores run the island search and load check like a
    blast; Crushing Weight runs one on purpose with an integer `extraLoad`, on the structure credit.
11. **Players are attuned to 2 elements per life** (8 spells), chosen on the respawn screen. LMB stays the M2a blast,
    RMB casts the selected spell, keys 1-4 pick the form, R swaps the element. Each spell costs one M2a charge, and
    each form has its own cooldown. No single spell kills from full health.
12. **Instant reach, presented as a sweep**, except where growth is the gameplay: Rising Spire's bands and Barricade
    Wave's bands (at most 6 per spell).
13. **Caps that refuse, never silent loss** (R7): 6 000 live conjured voxels per player (the oldest conjuring crumbles
    early), 60 000 per region, 32 zones, 8 bodies woken per Pressure cast, spread's fire caps; work counted in
    `sim::CastWorkOf`.
14. **Smallest fun slice first:** [mag-1] builds Siege Cannon, Barricade Wave, Shockwave and Updraft (breach, cover,
    knock off, climb) in sim, headless only, with only the operators, maps and zone kinds those four use; [mag-2] puts
    them on the wire with the one protocol bump that reserves every value the later stages need, and makes them
    playable; [mag-3] Frost (Shatter Spike, Flash Freeze, Permafrost Climb) makes the top-ranked Shatter combo playable
    next.
15. **Spawns are safe.** No spell reaches an enemy team's spawn area, no conjured matter forms in any spawn area, and a
    respawned player is immune to roots, pins and knockback for 2 s.
16. **Each wire value takes the next free number when it lands** ([notes/net.md](../notes/net.md) section 2 is the
    registry); the numbers below are the ones today's ROADMAP order gives.

## The concept, line by line

| Owner's concept | Where it lands |
|---|---|
| Spell = Cast Type x Element | Form operator x element verbs; one pipeline (`sim::CastSpell`) and one table (`sim::SpellTable`) |
| Liquid: downward and pooling; over edges, down walls, fills trenches and moats | **Flow**: a lobbed arc, a fall phase, then a lowest-first flood that never rises; its `Front` is the pool |
| Flame: upward and rising; climbs faces; fills ceilings and archways | **Plume**: the exact mirror; rises, clings to faces, spreads highest-first under ceilings |
| Solid: linear penetration, losing structural health per voxel | **Pierce**: a bore that pays every voxel it adds from an integer budget, sent as one `CarveCapsule` |
| Torrent: lateral or radial, sweeping floors or wall faces | **Burst**: a sweep in the hit face's plane that follows the surface by up to a step height, as a disc or a strip across the aim |
| Metal: create and reinforce; levelling raises HP | `knit_iron` (hardness 7.0, absorbs blasts), `slag`; levels later as table rows and reserved ids (question 15) |
| Thermal: direct damage, DoT, ignition, heats metal | Server damage and Burning; `pitch` (a spread fire class); the Heat map (ignites timber, heats metal into `*_hot`, melts ice); the hazard scan |
| Pressure: knockback, launch, objects, brittle bonus, no residue | Impulse and Lift zones; body impulses; `CarveCapsule` with no residue; the Shatter map on rimed and ice; `extraLoad` |
| Frost: slow, freeze liquid into walkable ice, brittle 2x | Chill and Root zones; `ice`, `floe` (frozen water, an anchor); rimed ids at half hardness; `Slick` ground |
| 16 named spells and their roles | "The sixteen spells", one entry each, with sizes, ops, bytes, work and counters |

Deviations, each settled below: the reach is instant with a presented sweep (question 1); casts are hitscan, so
"deflects incoming projectiles" becomes a dispel that cuts enemy zones short and frees allies (question 7); voxels
have no team, so "enemy metal" means any metal (question 10); the spell names are working names and "Napalm" needs an
in-world one (question 19).

## Play first

### The two axes, as a player sees them

| Form (owner's name) | HUD word | How it travels (presentation) | Where it lands | Reads as |
|---|---|---|---|---|
| Liquid | **FLOW** | A lobbed blob on a visible arc | Runs down from the impact, coats wall faces as it falls, pools at the lowest point | "It runs downhill" |
| Flame | **PLUME** | A rising wisp | Climbs from the impact, coats faces, pools under ceilings and arches | "It climbs" |
| Solid | **PIERCE** | A thin straight streak | Bores along the line and spends its budget per voxel | "It goes through" |
| Torrent | **BURST** | A spinning disc | Spreads across the surface it hits, as a disc, or a strip across your aim | "It spreads flat" |

| Element | Setting craft | Colour + particle shape | Signature world state | Signature player effect |
|---|---|---|---|---|
| Metal | Knitting | Violet-grey with violet seams; angular sparks | **knit iron**: the blast cannot cut through it, only chip it; lasts 15-25 s, cracks for its last 2 s | none (cover, climbing) |
| Thermal | Kindling | Orange; embers | **pitch** (a burning film that lights timber and burns players), **hot** metal (soft, burns on contact) | burning damage |
| Pressure | Drawing | White; expanding rings | none (no residue); shatters rimed matter and ice | knockback, launch, pin |
| Frost | Turning | Cyan; hexagonal shards | **rimed** (pale, frost-veined, half hardness), **ice** (walkable, slick), **floe** (a frozen moat) | chill (slow), root |

The four colours differ in hue and in lightness, and each element has its own particle shape, for colour-blind
players. The HUD words are the engine names, so one name set serves code, documents and players; "PLUME + FROST"
carries no contradiction. The owner confirms the colours windowed (question 23).

### What a player carries

- **LMB, the blast.** The M2a cast (60 damage at the centre, 0 at 1.2 m; carves with power 6), the same for everyone.
- **RMB, the selected spell.** Keys **1-4** pick the form (FLOW, PLUME, PIERCE, BURST); the mouse wheel cycles forms.
  **R** swaps between the two attuned elements. The selection is the client's, sent in every input
  (`PlayerInput::spell`).
- **Attunement: 2 of the 4 elements per life**, chosen on the respawn screen (M2a's 5 s). Keys 1-4 toggle elements
  there; exactly 2 must be on; Ready or the timer confirms; the default is the last choice (Metal + Pressure on a
  first life). Q (crate) and E (use) stay mechanisms'.

Why 2 per life and not all 16 or a loadout of 4: 8 spells is the most a fast first-person fight holds in the hands;
the enemy stays readable (two element colours glow at the hands); the HUD stays one bar of 4; combos become team
play (a Frost mage primes, a Pressure mage shatters); and the owner's two-axis grammar stays in the player's hands.
Every combination is one respawn away.

### Resources

| Resource | Value | Shown as | Why |
|---|---|---|---|
| Charges (M2a) | 3 at most, 1 back every 1.2 s; the 15-tick cooldown is the burst limit. The blast and every spell cost 1 | 3 pips under the crosshair | One pool keeps the sustained rate at about 0.83 casts/s per player (c), so 10 players make about 8.3 casts/s, inside the 10 casts/s design case. Shooting and weaving compete for the same pips |
| Form cooldown | Pierce 300 ticks (5 s), Burst 420 (7 s), Flow 540 (9 s), Plume 540 (9 s); shared by both elements of a form | A shrinking fill on each slot of the spell bar, with the seconds left | A big spell is a decision with a readable window ("his BURST is down"); it stops chaining Barricade Wave into Shockwave |
| Conjured matter | 6 000 live conjured voxels per player (about four walls) | `KNIT 1840/6000` above the element panel while above 0 | Over it, the player's oldest conjuring crumbles at once (an info line, not a refusal) |

Rejected: a mana bar (a second meter beside the charges), per-spell cooldowns (8 timers), costs that differ per spell
(players misjudge them). The first knob if the heavy breachers feel cheap: Siege Cannon, Meltdown Beam and Crushing
Weight cost 2 charges (question 11).

### Readability rules (they bind every spell)

1. **The form is the silhouette and the motion; the element is the colour and the material.** A cyan streak is a
   Shatter Spike; an orange arc is a Pitchfall.
2. **Everything that moves or roots a player arms 0.4 s after it appears**, with a contracting ring on the ground or a
   ghost of the zone. Only direct damage acts at once, and the streak or arc was the warning.
3. **Matter that rises out of the ground in front of players shows a 0.3 s ghost first** (Barricade Wave, Rising
   Spire, Permafrost Climb's ledges). Matter that follows a visible projectile (a Flow's arc, a Pierce's streak)
   appears where it lands.
4. **Each world state has one look**: knit iron violet-grey with seams (cracking in its last 2 s); slag bright
   orange; hot metal orange glow; rimed a pale, frost-veined base colour; ice pale glassy cyan; floe a shade bluer;
   pitch a black film with flames. They are material ids, so they cost the renderer nothing new.
5. **Nothing conjured lasts**: knit 15-25 s, ice 10-12 s, floe 15 s, rime 8 s, hot 4 s, pitch 3-4 s. All of it is gone
   at the Settle by construction.
6. **No single spell kills from full health.** Thermal carries the direct damage, as the owner's concept says ("high
   direct damage on contact"): the largest direct hit is 45 (Meltdown Beam), 69 with its burn. Pressure is
   structure-first: Siege Cannon hits for 30. A kill takes at least two casts, so the victim sees why.
7. **Hazards do not stack within an element.** At most one Thermal hazard tick and one status of each kind apply at a
   time; the strongest wins.
8. **Friendly fire stays off (M2a).** Damage, roots, pins, chill and knockback hit enemies only. Updraft and Rising
   Spire's lift move everyone (allies ride them on purpose), and terrain (walls, ice, rime) affects everyone.
9. **Control has a guard.** When a root or a pin ends, the player is `FREE` for 1.5 s: no root or pin applies (chill
   and knockback still do). Two or three Frost and Pressure players cannot chain one target into a lock.
10. **Spawns are safe.** A spell whose ops or zones would reach an enemy team's spawn area (6 m around each spawn
    point) is refused (`NEAR SPAWN`); fills never take a cell in any spawn area; a respawned player is immune to roots,
    pins and knockback for 2 s, or until they cast.

### Combos and counters

The combos are not scripted. Each state is an id, and each element's maps say what happens to each id.

| State (ids) | Made by | Blast (power 6) | Pierce cost per voxel | Pressure | Thermal | Frost |
|---|---|---|---|---|---|---|
| knit iron (7.0), slag (7.0) | Metal | **absorbed**: no carve; chips a 0.25 m patch of the plate (Ablate) | 112 | - | becomes **hot** (3.5) | becomes **rimed** (3.5) |
| hot (knit 3.5, iron 4.0) | Thermal on metal | carves | 56 / 64 | - | stays hot | **quench**: back to plain (knit, iron) |
| rimed (half hardness, same bonds) | Frost on stone, dark stone, brick, iron, knit | carves, even rimed iron | half: **2x depth** | **shatters to air** (Shockwave, Crushing Weight), up to 0.5 m deep from an exposed face | thaws to its base | stays rimed (refreshed) |
| ice (1.0) | Frost on air or pitch | carves | 16 | shatters (exposed, 0.5 m deep) | melts | - |
| floe (frozen water, an anchor) | Frost on water | indestructible | stops a bore | shatters back to water | melts back to water | - |
| pitch (a burning film) | Thermal | not solid | - | - | - | **frozen into ice** |
| burning timber (spread.md) | Thermal's Heat map | as spread.md | - | - | - | quenched (spread's Douse rows) |

Ranked by how much fun they are:
1. **Shatter.** Shatter Spike, then Siege Cannon through about 3.3 m instead of 2, or Shockwave peeling the rimed face
   0.5 m deep. The owner's "shatter combos with teammates". Plating over the rime protects it: Shatter reaches only
   rime it can see.
2. **Bridge and trap.** Flash Freeze over the moat makes a bridge; a defender's Wildfire or Meltdown melts it under
   the attackers; a Shockwave shatters it.
3. **Heat beats metal.** A Barricade or a Molten Pour plating that the blast can only chip becomes blast-able for 4 s
   after a Thermal hit. That is the counter-build loop.
4. **Quench.** Frost on hot metal makes it plain again; Frost on pitch makes walkable ice; Frost on fire puts it out.
5. **Vertical play.** Updraft onto a Rising Spire's top; a Rebar peg ladder up to a ledge (three lances, six pegs:
   about 6.4 m in 10 s); Permafrost Climb against Wildfire on the same wall face.
6. **Displacement.** Shockwave into your own Barricade (the enemy stops against it and eats a blast); Crushing Weight
   on a plank bridge drops it; Glacier Slide plus Shockwave off a battlement (a fall of 4 m or more hurts, M2a).
7. **Felling.** Siege Cannon through a tower's outer wall cuts its neck from range; Crushing Weight on a weak upper
   floor; defenders plate the neck with Molten Pour (knit bonds equal stone's, so a plate carries what a cut took
   away), attackers chip the plate with blasts, and the plating's crack 2 s before it crumbles is their cue.

### Time to kill, against M2a

M2a: 100 HP; a blast does 60 at its centre and 0 at 1.2 m (a near miss at 0.4 m is 40); 3 charges plus one per
1.2 s; burst limit 0.25 s; 5 s respawn.

| Case | Casts | Time (c, every hit lands) |
|---|---|---|
| Blasts only (M2a today) | 3 x about 40 | 0.5 s burst; 1.5-2.5 s with real misses (e) |
| Shatter Spike, then 2 blasts | 25 + 40 + 40 | about 0.6 s; the chill (speed x 0.625) makes the blasts land |
| Flash Freeze root, then Meltdown Beam and a blast (two players) | 10 + 45 + 60 (a rooted target takes the blast at its centre) | the 0.4 s ring plus 0.25 s; a rooted target cannot dodge: **the team payoff** |
| Meltdown Beam, then a blast | 45 + 40 + 24 (burn) | about 2 s with the burn; 0.5 s with a second blast |
| Siege Cannon, then 2 blasts | 30 + 40 + 40 | about 0.5 s; the Cannon is a structure tool that also hurts |
| Pitchfall carpet alone | 20 + 15/s | about 5.3 s standing in it: denial, not a kill |
| Updraft juggle | 0 + 2-3 blasts at an airborne target on a known path | about 1 s |

Targets for the owner's taste check: one attacker kills a full-health target in 1.5-3 s, a coordinated pair in about
1 s, and no death comes from one spell. If fights feel too lethal, the first knobs are Meltdown Beam's 45 and the
blast's 60 (M2a).

### Team play in a 5v5 siege

There are no fixed classes; attunement pairs make the roles. The team panel shows each ally's two element letters and
colours ("ANNA [M][F]").

| Pair | Role | Typical play |
|---|---|---|
| Metal + Frost | Warden | Barricades on the bridge, Flash Freeze roots, plating the neck |
| Pressure + Frost | Breacher | Shatter Spike, then Siege Cannon on the neck; Shockwave on rimed walls |
| Metal + Pressure | Scaler | Rising Spire, Updraft, Rebar ladders: puts the team on the wall |
| Thermal + Metal | Burner | Pitchfall on stairs, Meltdown Beam through knit iron, Molten patches |
| Thermal + Pressure | Skirmisher | Firestorm and Shockwave clear battlements; Updraft juggles |
| Thermal + Frost | Controller | Ice bridges and melt traps; Wildfire against climbers |

**A Felling duel on the playground** (M2a's mode): attacking Breachers rime and cannon the neck from behind
Barricades while Scalers put two players on the enemy wall with Spire and Updraft. Defenders plate the neck with
Molten Pour, burn the climbers with Wildfire (it melts their ice), and knock the scalers off with Shockwave. The
plating's 25 s life and its 2 s crack set the rhythm: a breach window opens every time a plating fades.

### Input and HUD (game-ui.md: `HudCanvas` quads and the 5 x 9 font, no ImGui)

- **Keys:** LMB blast; RMB spell; 1-4 form (wheel cycles); R swaps the element; E use and Q crate (mechanisms.md); V
  view. The editor keeps its keys while F1 is open.
- **Spell bar** (bottom centre, 4 slots of 84 x 48 px at 720p, scaled by `TextScale`): each slot shows its key digit
  and the form word (6 glyphs at most, which fits 70 px at scale 2); the slot's border takes the active element's
  colour; a cooldown fill shrinks from the top with the seconds left; the selected slot is raised 6 px with a 2 px
  border. Under the bar the full spell name ("SIEGE CANNON") shows in the element's colour for 1.5 s after any
  change, then at 50 %.
- **Element panel** (right of the bar): `[R] FROST / pressure`, the active element bright and the other dim; the
  conjured-matter meter above it while above 0.
- **Charges:** 3 pips of 6 x 6 px under the crosshair (M2a).
- **Crosshair per form:** Pierce a dot; Burst wide brackets; Plume a caret; Flow a ring, plus a predicted landing
  marker on its arc (presentation only: a few particles on the client's own arc).
- **Statuses** beside M2a's health bar: `ROOTED 0.8`, `CHILLED`, `PINNED`, `BURNING`, `LIFTED`, `FREE 1.2` (the
  control guard), in element colours.
- **Refusals** reuse M2a's "Cast not run" line with new words: `PIERCE COOLING`, `NO CHARGE`, `NOT ATTUNED`,
  `TOO MUCH MATTER`, `TOO MANY ZONES`, `TOO MUCH FIRE`, `NEAR SPAWN`; `KNIT CAP: OLDEST CRUMBLED` is an info line.
- **What the HUD reads:** the form cooldowns, the attunement pair and the statuses come from the snapshot's owner
  block ("Wire values"); the client never guesses them from `CastFx` or rejections.
- **Attune menu** on the respawn screen (the existing `Menu` class): four element items with one-line descriptions.
- **Cost:** about 110 more quads (e), beside the measured 149-177 quads at 0.01 ms (m). Measured by the default run's
  `HUD: N quads` line with a spell selected.
- **Owner checks (windowed; an assistant cannot confirm them):** the keys feel right and RMB does not conflict, the
  bar reads at a glance, the colours are distinguishable, slick ground and launches feel good. They go into the
  game's [owner-checks.md](../../../docs/owner-checks.md) when [mag-2] lands.

## Where every effect lives (the replication map)

Every effect of every spell falls into exactly one row.

| Class | How it reaches peers | Who decides | Checked by | Effects |
|---|---|---|---|---|
| **World op** (terrain) | opSeq stream, `ApplyOp` clipped per chunk, ring replay, ChunkData for joiners | Server (offline: the same server code in-process, M2a) | `HashCheck` / `ChunkHash`, `WorldHash` goldens | Knit fills and their bands, slag and its set, pitch, ice, floe, rime, heat, cool, thaw, unknit, shatter, the Pierce bore, and what the island search and load check release (`Detach`, `ClearMask`) |
| **Lockstep world rule** | `SpreadStep` (spread.md) | Every peer computes; the server lists chunks and sends border overrides | `HashCheck` | Fire climbing from pitch and from ignited timber |
| **Entity event** | opSeq stream (`EntitySpawn`, `EntityRest`, `EntityWake`, `EntityDespawn`, `ObjectModel`) | Server | `EntityCheck`, `EntityList` | Pieces the load check releases (existing path; conjured pieces never get a slot); a body woken by an impulse |
| **Snapshot motion** | Unreliable 30 Hz snapshots | Server's Jolt | none (presentation of the past) | Bodies thrown by Pressure; remote players' motion and status bits |
| **Player-rule zone** | opSeq event `ZoneStart` (and `ZoneCancel`, a truncation from a tick at least the lead ahead), lead 24 ticks; live zones in the join baseline | Server announces; **every peer evaluates inside `StepPlayer`** | Reconciliation (corrections counted by cause) | Knockback, launch, lift, pin, root, chill, release |
| **Player-rule ground** | The voxel id under the feet (a world op put it there) | World state | Reconciliation | Slick ice (a material flag) |
| **Server-only rule** | Results only (health, form cooldowns, attunement in the owner block; kill feed) | Server | none | Damage, burning, the hazard scan, fall-damage immunity after a lift, lag-compensated hit tests, body impulses, form cooldowns, charges, attunement checks, the spawn guard |
| **Presentation** | `CastFx` event plus what each peer derives from applied ops and zones | Each client, inside the `FxDirector` | none | Streaks, arcs, bursts, rings, ghosts, crumble warnings, steam, frost mist, flames, glow by id |

Not replicated, by rule: operator solves (only their outputs travel), the `KnitRegistry` (only its expiry ops), hit
tests, body impulses (their results arrive in snapshots), damage.

## The two axes as data

### `sim::SpellTable` (integers, in `vortex.sim`, behind WP6's rules seam)

- `Form`: Flow 0, Plume 1, Pierce 2, Burst 3. `Element`: Metal 0, Thermal 1, Pressure 2, Frost 3.
  `SpellId = form * 4 + element`, 0..15. **These are wire values.** 16 and 17 are spread.md's Kindle and Pour, kept
  for drills and tests only (a session file enables them; play never offers them).
- Per form (`FormDef`): the operator's parameters (below), the cooldown in ticks.
- Per spell (`SpellDef`), integers only, metres converted to voxels and q (1/16 voxel) at session start by one
  rounding from the session's voxel size:
  - the operator's parameters for this spell (footprint, reaches, caps, Pierce radius, power and budget);
  - the element verbs: which roles a fill takes, the map of each op, the fill cap, the rim height;
  - follow-ups: bands (count, ticks apart, start offset), a scheduled map (Slag's Set) and its delay;
  - the registry layer and lifetime of what it conjures or turns;
  - zones: kind, shape, size, arm (the session lead), duration, strengths in cm/s, team mask;
  - server-only: direct damage, splash radius, status and its ticks, charges, body impulse.
- `SpellTableHash` joins `buildHash`. Tuning a number refuses mismatched builds; it never desyncs them. Damage is in
  the table but changes nothing a client computes; it is hashed anyway, so the HUD never shows another build's
  numbers.

### The cast pipeline

```
input tick T_in: Spell edge + PlayerInput::spell          (StepPlayer derives the edge, as Cast today)
capture (server): attuned? charge? form cooldown? caps?   -> CastRejection, or queued with eye(T_in), dir(T_in)
dequeue T_run (cast queue, cast credit):
  ray: the server's cast ray from the rewound eye (Flow: the integer arc trace)    -> hit voxel h, face n, aim D
  operator: sim::Propagate(form, seeds, world, params)                            -> roles, a segment, a column
  verbs (server): which cells fill, which map, bands, zones, damage, impulses
  spawn guard: any op box, fill cell or zone shape in an enemy spawn area          -> CastRejection::NearSpawn
  ops now: MapMask / MapShape / CarveCapsule, then the island search and load check where the spell carves
  zones: ZoneStart{start = T_run + lead}; ZoneCancel{endTick = T_run + lead}; follow-ups and expiries into the
         schedule; registry entries
  damage and statuses (server), CastFx (presentation)
every tick: RunScheduled emits due bands, Set, Shatter-at-arm, Cool / Thaw / Unknit expiries; site checks (and
            Crushing Weight's extraLoad check) wait in the structure credit's queue
```

`sim::CastSpell` is the whole pipeline in Sim (GPU-free), used by the server and by M2a's in-process offline server;
[mag-1]'s headless scripts call it on the offline `Simulation` directly.

### Inputs every operator shares (server only, quantised once, R3)

- **The hit.** The existing cast ray from the eye of the input's tick (`StateAt`, the 32-tick state ring), to terrain
  or a replicated body in front of it, within `castRange`. It gives `h` (the solid voxel), the face normal `n` (the
  DDA's last step: an axis and a sign) and `hitQ` (the point, in q). **`Film` ids are skipped on the CPU only:** on a
  `Film` hit, `World::Raycast`'s spell variant traces again from just past that voxel (at most 8 restarts, then the
  film counts as the hit). `shaders/shared/trace.glsl` is unchanged, so the GPU still draws pitch as voxels and
  `Trace.SingleChunkMatchesReferenceDDA` holds (`Raycast.ASpellRayPassesPitchAndHitsTheStoneBehind`).
- **The aim.** `D = round(direction * 16384)` as i16 x 3, from the yaw and pitch integers of `PlayerInput`.
- **Flow's arc.** A Flow spell does not cast the straight ray. It traces an integer parabola in fine fixed point,
  because gravity in q per tick^2 rounds to 0 (9.81 m/s^2 is 0.35 q/tick^2 at 0.125 m): position `P` and velocity `V`
  in **1/4096 voxel** (q << 8) as int64 x 3, `P` from the eye, `V = round(D * speed / 16384)` per tick (Flow's speed
  22 m/s, converted once at session start), `G = round(9.81 / voxelSize * 4096 / 3600)` per tick^2 (89 at 0.125 m,
  112 at 0.1 m, 74 at 0.15 m; under 0.6 % error). The first step takes half gravity (`V.y -= G / 2` once before the
  loop), so the sum matches the parabola's `G t^2 / 2`; then each tick `P += V; V.y -= G`. The segment from `P >> 8`
  to the next `P >> 8` (in q) is walked by the same voxel DDA as the ray, at most 150 ticks (2.5 s). Integer only, so
  a server and an offline run give the same hit; the client draws the same arc for its landing marker.
  `Flow.ArcTraceIsIntegerAndReproducible`, `Flow.ArcMatchesTheParabola` (a 45 degree throw's apex and range within 1
  voxel of the analytic parabola at 0.1, 0.125 and 0.15 m).
- **A body hit.** Pierce damages the body (`Scene::Damage`, as a blast's object half) and stops there; other forms take
  the air cell in front of the body's hit point as their seed and skip cells inside server bodies' boxes when they
  fill.
- **The play area.** A hit outside the play bounds or the window's rows is refused as today (`CastFitsRegion`,
  `CastRejection::Outside`).
- Every operator is clipped to the cast's **flood box** (`floor(centreQ / 16) +- kFloodHalfExtent` (96) voxels,
  `sim::FloodBox`; the centre is `hitQ`, or a bore's midpoint for Pierce), which keeps every island search after it
  inside the server window, and stays below the build ceiling: nothing is ever filled above it, so no spell op is
  refused with `outOfBounds`.
- **A bore's boxes** (Pierce). Its island box is the capsule's AABB plus `kIslandSearchMargin` (24) per side, never a
  sphere's. The bore is bounded in voxels, not metres: `kMaxBoreVoxels` = 64 from the hit (8 m at 0.125 m, 6.4 m at
  0.1 m, 9.6 m at 0.15 m), radius at most 4 voxels, and `B` never leaves the play bounds. Its flood box is centred on
  `floor((hitQ + B) / 32)`, the middle of the hit-to-end segment, which lies in the play bounds, so the flood box is
  inside the server window (3 chunks = 96 voxels of margin). Pinned by
  `static_assert(kMaxBoreVoxels / 2 + 3 * kMaxPierceRadiusVoxels + kIslandSearchMargin + kBoreProofMargin <=
  kFloodHalfExtent)` with `kBoreProofMargin` 24 (32 + 12 + 24 + 24 = 92 <= 96): structure past the bore's end is
  seen for at least 24 voxels before the flood box's face counts as anchored, so a bore cuts a neck from range at
  every voxel size (`Pierce.BoreStaysInsideItsFloodBox`; `--spell-demo siege-cannon` at G6's 0.1 and 0.15 m).

## The four operators (exact)

All four are pure functions in `vortex.sim` (`sim::Propagate`, GPU-free, `Vortex::Sim`). They read voxel ids only,
through a per-cast chunk cache (a packed chunk is decoded once), and their result does not depend on the cache, the
thread count or the load order. Parameters are at 0.125 m; they count voxels and are converted from metres at session
start, so sizes hold within one voxel at `--voxel-size` 0.1-0.15.

### Rules shared by every operator

**Cell classes** (from the id only, through tables covered by `MaterialTableHash`):
- `open(id)`: air, or an id with the new `Film` flag (pitch). Operators move through open cells.
- `support(id)`: not open. That covers every collider and every liquid (`water`, `lava`, spread's `knitwater_*`).
  Liquids are support on purpose, so a Flow slides **over** a moat and a Burst sweeps its surface.
- `skin(c)`: open, with at least one support face neighbour.
- **Walls.** A cell outside the server window, above the build ceiling, below the window's bottom row, outside the
  play bounds grown by the 3-chunk margin in x / z, or outside the flood box reads as support and is never in a role.
  Offline, an unloaded chunk reads as a wall: a spell never waits.

**Seeds.** `s0 = h + n`, the open cell in front of the hit face. A disc footprint of radius `rf` is the open cells
`s0 + a*u + b*v` with `a*a + b*b <= rf*rf`, where `(u, v)` are the two axes of the hit face's plane in fixed order
(x before y before z). A floor hit gives a disc on the floor, a wall hit a disc one voxel thick in front of the wall,
a ceiling hit a disc under it.

**Order.** Every queue is a min-heap on an explicit integer key that ends in (z, y, x), so the order is total (R4).
`visited` is a dense bitset over the operator's box and is never iterated.

**Work** (R5). Each operator reports `visits` (cells popped or tested) and `chunksRead` (distinct chunks it read),
both functions of the voxels and the cast only, never of the cache. `sim::CastWorkOf` charges
`visits * kPropagateVisitWork (1) + chunksRead * kColdChunkGraphWork (200)`.

**Roles.** An operator returns disjoint cell sets:
- `Path`: cells the spell moves through. Flow and Plume split it into `Fall` / `Rise` (the vertical phase) and `Run`
  (the spread phase).
- `Front`: where it ends: the pool, the tops and the under-ceiling cells, the leading rim, the Pierce stub.
- `Contact`: support voxels face-adjacent to `Path` or `Front` (the matter it touches but does not enter).

**Never-rules**, each pinned by a test: Flow never moves to a higher y; Plume never moves to a lower y; every Burst
cell rests on its surface and moves at most `stepMax` voxels along the normal per step; Pierce never passes an
indestructible voxel or one harder than its power.

### Flow (Liquid): down, then lowest-first

1. **Seeds**: the disc of radius `rf` at the arc's hit.
2. **Fall phase.** For each seed in index order, step down while the cell below is open, at most `fallMax` cells. The
   cells passed are `Fall`; a cell already visited is passed through and not counted twice. The cell where it stops
   (support below) is a **landing**. A column that runs out of `fallMax` lands nowhere.
3. **Spread phase.** Push every landing into a min-heap keyed by `(y, dx*dx + dz*dz from the hit column, z, x)`. Pop up
   to `runPops` cells:
   - if the cell below is open and not visited, push only the cell below (it falls; a falling column never spreads);
   - otherwise push the side cells `+x, -x, +z, -z`, in that order, that are open, not visited and within `lateral`
     of the hit column. A side cell over an edge has open space below and falls when popped: the flow pours over
     edges and cascades down stairs and walls;
   - never up.
4. **Roles.** `Front` = the `frontCount` popped spread-phase cells with the smallest key: the bottom of the deepest
   basin reached, filled to that volume (the pool), or the end of a downhill run. `Run` = the other spread-phase
   cells. `Contact` as above.
5. **Why it pools.** The key is lowest-y first, so once any cell finds a way down, everything lower is expanded before
   the upper level continues. That is Priority-Flood basin filling, bounded and run once, on the server. spread.md
   rejected it as a lockstep rule; R1 allows it here, because only its result travels.

Shapes (c): a wall hit at 3 m gives a curtain about 9 wide down the face (about 9 x 24 = 216 `Fall` cells), then a
pool at the foot; a floor hit runs on the floor until it finds an edge, then follows it down; flat open ground stops as
a disc of about 1 200 cells (2.5 m radius).

### Plume (Flame): up, then highest-first under ceilings

The exact mirror of Flow:
- each seed rises while the cell above is open and `y <= seed.y + riseMax`; those cells are `Rise`;
- columns that stop under a ceiling seed a min-heap keyed by `(-y, dx*dx + dz*dz, z, x)`; a popped cell rises if it
  can, otherwise it spreads to its four sides within `lateral` (at most `runPops` pops);
- never down; a side cell past a ceiling's edge rises again, so the plume flows around lintels, up the next face and
  fills archways and overhangs from the top;
- `Front` = the top cell of every column plus every under-ceiling cell; `Run` = the other spread-phase cells.
- **A fill takes a `Front` cell only if it is skin or joins support through the cells already filled** (walked in
  heap order, after the `Rise` and `Run` cells of the fill). On a floor hit in open air the column tops are 6 m up with
  nothing under them; without this rule a Frost or Thermal Plume would leave single voxels hanging there until their
  expiry (fills run no island search). Rising Spire fills its whole column, so its tops always join.

Shapes (c): a floor hit with `rf` 3 is a column 0.75 m wide and up to `riseMax` tall (29 x 48 = 1 392 cells at 6 m); a
wall hit is a strip one voxel thick and about 7 wide climbing the face; under a timber floor it rises, then spreads
under the planks.

### Pierce (Solid): a bore that pays for every voxel it adds

- **Step.** `S = round(16 * D / |D|)` (i32 x 3, about one voxel long), from `D` by an integer square root, once.
- **Start.** `A = hitQ - (2 * r * S) / 16` (two radii in front of the face, so the start cap is in front of the
  surface; integer division). `B_0 = A`, `B_k = A + k * S`.
- **The capsule.** Voxel `v` (centre `c = 16v + 8`, in q) is in `Capsule(A, B, r)` when, with `L2 = |B-A|^2` and
  `t = dot(c-A, B-A)` clamped to `[0, L2]`: `|c-A|^2 * L2 - t^2 <= r^2 * L2` for `0 < t < L2`, and the end-sphere
  tests at the clamps (all int64; coordinates relative to `A`; a capsule over 64 m is refused, which keeps every
  product under 2^53 (c)).
- **The walk.** For `k = 1, 2, ...`: the **new voxels** `N_k` are those in `Capsule(A, B_k, r)` and not in
  `Capsule(A, B_{k-1}, r)`, read in (z, y, x) order over the growing cap's box. Stop **before** `k` when:
  - `N_k` holds an indestructible voxel that is not a liquid (bedrock, floe), or a solid voxel whose cost after the
    element's pre-map is above the spell's `power` (a Rebar Lance at iron);
  - `cost(N_k)` = the sum of `boreCost[id]` over its solid voxels (air, liquids and films cost 0) exceeds the
    remaining budget;
  - `k > maxSlices`, `|B_k - hit|` passes `kMaxBoreVoxels`, `N_k` meets a wall (window, ceiling, play bounds, flood
    box), or the visit cap would pass.
  Otherwise the budget drops by `cost(N_k)` and the walk goes on. Air is free, so a bore crosses a room and enters the
  next wall.
- **Visits** are `sum |N_k|`, the new member voxels, a count that does not depend on how `N_k` is found; the visit cap
  (3 000) and the work count it. Finding `N_k` scans at most the slice's box (`(2r + 3)^2 x 3` voxels per slice, about
  25 000 tests for a 64-slice Siege Cannon), bounded by `maxSlices` and charged as `slices x kPierceSliceWork` (100).
  A 64-slice Siege Cannon has about 2 100 members, so the cap never cuts the depths below.
- **The result** is `B = B_last` and one op, `CarveCapsule(A, B, r, power)`, which carves every voxel of the capsule
  that is carvable with `hardness * 16 <= power`. By construction the op carves exactly the voxels the walk paid for
  (`Pierce.WalkAndCapsuleAgree`): nothing past `B`, nothing harder than `power`, nothing unpaid. A voxel's fate
  depends only on the op's integers, never on the voxels before it on the line, so ChunkClip holds.
- `boreCost[id]` is an integer table in `SpellTable`, authored in code: `round(hardness * 16)` for every id
  (stone 48, stone_dark 64, brick 40, iron 128, wood 24, planks 19, knit 112, rimed stone 24, ice 16, ...);
  `Materials.BoreCostMatchesHardness` pins it against the material table.
- **The element's pre-map.** Thermal reads metal ids through the Heat map (knit 56, iron 64), because it heats before
  it carves.
- **Roles.** `Front` = the stub: slices `0..2r/16` (air in front of the entry); `Path` = the paid solid voxels;
  `Contact` = the rim, solid voxels within `r + rimDepth` of the axis over the bored slice range.
- **Players** are hit along the segment from the eye to `B`: the server's capsule test against enemy capsules at the
  input tick (M2b's lag compensation; M2a's present positions until it exists).

Depths per spell are in "The sixteen spells" (c, from slice counts at `pi * r^2` voxels per slice).

### Burst (Torrent): a sweep in the hit face's plane

- **A Burst cell** is `open(c) && support(c - n)`. `s0` must be one, or there is no footprint.
- **In-plane offsets** of `c` from `s0`: `(du, dv)` along `(u, v)`; `dn` along `n`. The aim's in-plane part is
  `(Du, Dv)`; `|Dp| = isqrt(Du*Du + Dv*Dv)`; if it is 0 (aim along the normal), `(Du, Dv) = (1, 0)` in units of `u`.
- **Footprints:** a **disc**, `du*du + dv*dv <= R*R`; or a **strip** across the aim,
  `|du*Dv - dv*Du| <= W * |Dp|` and `|du*Du + dv*Dv| <= T * |Dp|` (half-width `W`, half-thickness `T`).
- **The sweep.** A min-heap keyed by `(du*du + dv*dv, du, dv, dn, z, y, x)`, at most `pops`. For each popped cell `c`
  and in-plane step `e` in the order `+u, -u, +v, -v`, with `p = c + e`, take the first `k` in the order
  `0, +1, -1, +2, -2, ... +stepMax, -stepMax` for which `q = p + k*n` is a Burst cell and the way is clear (for
  `k > 0` the cells `c + j*n`, `1 <= j <= k`, are open; for `k < 0` the cells `p + j*n`, `k < j <= 0`, are open). Push
  `q` if it is not visited and inside the footprint.
- `stepMax` = 3 voxels (0.375 m, just over the 0.35 m step height): the sweep goes wherever a player can walk. It
  follows rubble, stairs, slopes and wall faces, and stops at drop-offs larger than a step (pouring over edges is
  Flow's job).
- **Roles.** Disc: `Front` = the cells with `du*du + dv*dv > (R-1)^2` and `du*Du + dv*Dv >= 0` (the **leading half**
  of the rim; the whole ring if the aim is along the normal); `Path` = the rest. Strip: every cell is `Front`.
  `Contact` = the surface under the sweep (`c - n`) and the support face neighbours.
- **The rim** is extruded along `+n` by the element's `rimHeight - 1` more cells while they are open.

### Operator parameters (0.125 m)

| | Flow | Plume | Pierce | Burst |
|---|---|---|---|---|
| Footprint | disc `rf` 4 (0.5 m, up to 49 seeds) | disc `rf` 3 (29 seeds); `rf` 8 for Updraft's column | radius `r` 1.5-3.2 voxels per spell | disc `R` 24-32, or strip `W` 18 x `T` 1 |
| Reach | `fallMax` 64 (8 m); `runPops` 1 200-1 600; `lateral` 24-32 | `riseMax` 48-80 (6-10 m); `runPops` 400; `lateral` 16 | `maxSlices` 24-64, at most `kMaxBoreVoxels` 64 from the hit; budget per spell | `pops` 4 000; `stepMax` 3 |
| Front | the 240 lowest cells (the pool) | column tops + under-ceiling cells (filled only if skin or joined) | the stub | leading half-rim, or the whole strip |
| Visit cap | 12 000 | 4 000 | 3 000 new member voxels (`sum \|N_k\|`) | 6 000 |
| Box (voxels) | x / z +-36, y -64..+4 | x / z +-20, y -4..+82 | the capsule's box | in-plane +-34, normal -4..+14 |
| Scratch (c) | a 73 x 68 x 73 bitset = 45 KB | 41 x 86 x 41 = 18 KB | none | 69 x 69 x 18 = 11 KB |

## The element verbs

### What each element does with each role (server-only rules, `sim::SpellTable`)

| | Metal (Knitting) | Thermal (Kindling) | Pressure (Drawing) | Frost (Turning) |
|---|---|---|---|---|
| Flow | `Fall` skin cells + `Front` -> slag, cap 900 voxels, lowest key first | `Fall` and `Run` skin cells + `Front` -> pitch; Contact -> Heat | no matter: the landing is a zone's centre | `Fall` and `Run` skin cells + `Front` -> ice (Freeze), cap 2 048; Contact water -> floe, fire quenched |
| Plume | `Rise` + `Front` -> knit, in bands, ledges every metre | `Rise` and `Run` skin cells + `Front` -> pitch; Contact -> Heat | no matter: the column is a Lift zone | `Rise` cells within 2 voxels of support + `Front` -> ice, ledges; a Rime box over the face |
| Pierce | two bores 1 m apart, then knit fills both sockets and two pegs | Heat before the bore, then the bore | the bore | the bore, then a Rime capsule (r 6) around it |
| Burst | strip extruded 12 high -> knit, in bands | rim extruded 6 high -> pitch; Contact -> Heat | no matter: a radial Impulse zone, an ally Release zone; the Shatter flood within 2 m | Path -> ice; water -> floe slab; Root and Chill zones |
| A fill cell inside a player's capsule (grown by 1 voxel, at the emission tick) | skipped: a gap | n/a (pitch is a film) | n/a | skipped; the player is rooted, and presentation draws the encasing |
| Bodies (server) | fills skip cells inside a server body's box | none | at most 8 woken, nearest first by (distance in q, NetId), velocity kick capped at 12 m/s | none |

Players are never entombed: a fill never takes a cell a player's capsule holds at the tick it is emitted, and a band
emitted later re-excludes the capsules of that tick. A fill never takes a cell inside any team's spawn area (the
spawn guard, "Caps").

**The Shatter flood** (Pressure on rime and ice; Shockwave and Crushing Weight, at their zone tick). Shatter is not a
plain sphere, which would reach rime behind a knit plate and blow a breach over 2 m wide and deep through rimed iron
in two casts. It is a
bounded server search, like the operators: the seeds are the shatterable voxels (any `_rimed` id, `ice`, `floe`)
inside the spell's sphere (r 2 m) that have an open face neighbour (exposed); a breadth-first flood keyed by
`(depth, z, y, x)` goes through face-connected shatterable voxels inside the sphere, at most 4 voxels (0.5 m) deep,
at most 4 000 visits. The result goes out as one `MapMask(Shatter)`. Plating over rime blocks it; each Shockwave peels
one layer; Siege Cannon through rime stays the deep breach. Its work is charged at `T_run` at its cap (4 000 visits
plus the chunks of its box), a bound from the table, so the cast's work is known when it is dequeued.

### The Turning table (`voxel::TurningTable`, Core, integers authored in code)

A 256-entry `std::array<uint8_t>` per map; a voxel's new id is `map[old]`; every map is the identity on the ids it does
not name, and no map names bedrock (`TurningTable.NeverNameBedrockAndIdentityElsewhere`). `TurningTableHash` joins
`buildHash`. **Map numbers are wire values**; their contents change only with `buildHash`.

| Map | Changes (everything else maps to itself) | First used by |
|---|---|---|
| 1 Ignite | spread.md: planks / wood / leaves -> `k1` of its class; grass, grass_dry -> `scorched` | spread's Kindle (drills) |
| 2 Douse | spread.md: kindle -> base, burn -> charred, burning foliage -> air | spread's Pour (drills) |
| 3 Knit | air -> `knit_iron` | Barricade Wave [mag-1] |
| 4 Slag | air -> `slag` | Molten Pour [mag-4] |
| 5 Set | `slag` -> `knit_iron` | Molten Pour [mag-4] |
| 6 Pitch | air -> `pitch_b1` | Thermal [mag-5] |
| 7 Heat | Ignite's rows; `knit_iron`, `knit_iron_rimed` -> `knit_iron_hot`; iron, `iron_rimed` -> `iron_hot`; `ice` -> air; `floe` -> water; other rimed -> base | Thermal [mag-5] |
| 8 Cool | `knit_iron_hot` -> `knit_iron`; `iron_hot` -> iron | Thermal expiry |
| 9 Rime | stone, stone_dark, brick, iron -> their `_rimed`; `knit_iron` -> `knit_iron_rimed`; `knit_iron_hot` -> `knit_iron`; `iron_hot` -> iron (the quench); Douse's rows | Frost [mag-3] |
| 10 Thaw | every `_rimed` -> its base (`knit_iron_rimed` -> `knit_iron`) | Frost expiry |
| 11 Freeze | air, `pitch_*` -> `ice`; water -> `floe`; Douse's rows | Frost [mag-3] |
| 12 FreezeWater | water -> `floe` (air untouched) | Flash Freeze [mag-3] |
| 13 Shatter | every `_rimed` (base and knit), `ice` -> air; `floe` -> water | Shockwave, Crushing Weight, always through the Shatter flood's mask (rows active from [mag-3]) |
| 14 Unknit | `knit_iron`, `knit_iron_hot`, `knit_iron_rimed`, `slag`, `ice` -> air; `floe` -> water | every conjuring's expiry [mag-1] |
| 15 Ablate | `knit_iron`, `slag` -> air | an absorbed blast [mag-1] |

Rows that name ids not yet built are the identity until their stage adds the ids; the hash changes then.

Two rules of the map set:
- **`shapeChanged` is decided per voxel, never per map.** `MapShape` and `MapMask` write through spread S1's
  `EditVoxelsInChunk`-style bookkeeping: a chunk's `ChunkChange::shapeChanged` is true when any voxel's collidability
  changed (`collidable[old] != collidable[new]`), whatever the map. An op whose changed voxels all keep their
  collidability (Rime, Thaw, Cool, Heat on metal, Ignite and Set always; Heat, Douse and Freeze where they touch no
  ice, floe, pitch or burning foliage) re-cooks no collider and uploads only the changed 64 B blocks (spread S1's
  partial uploads).
- The maps that change a voxel's collider shape: Knit, Slag, Freeze and FreezeWater (air, pitch or water into a
  collider), Unknit, Shatter and Ablate (a collider into air or water), Heat (ice -> air, floe -> water), Douse and the
  Douse rows of Rime and Freeze (burning foliage -> air). The same list decides site checks ("Expiry and structure").

### New world-palette ids (15, plus 3 reserved)

Appended after spread's 21-63 and W4's 12, in this order; whichever design lands first takes the next numbers, and
`MaterialTableHash` covers ids, values and flags. Every hardness is a multiple of 1/16, so the exact compare
`hardness * 16 > power` stays exact.

| Id (working name) | Count | Hardness | Density | `strength` C / S / T (kg/m^2) | Flags | Look | Stage |
|---|---|---|---|---|---|---|---|
| `knit_iron` | 1 | 7.0 | 2 000 (light: knit walls do not overload floors) | 100 000 / 40 000 / 15 000 (stone's: a plate replaces a cut neck, never makes it stronger) | Conjured | dark violet-grey, banded; cracks shown in its last 2 s | [mag-1] |
| `slag` | 1 | 7.0 | 2 000 | as knit | Conjured, Hot | bright orange, emissive 5 | [mag-4] |
| `ice` | 1 | 1.0 | 900 | 40 000 / 8 000 / 3 000 | Conjured, Slick | pale cyan, roughness 0.1 (opaque) | [mag-3] |
| `floe` | 1 | 1.0 | 900 | as ice | Conjured, Slick, **Indestructible** (an anchor) | as ice, a shade bluer | [mag-3] |
| `knit_iron_rimed` | 1 | 3.5 | 2 000 | as knit | Conjured | frosted knit | [mag-3] |
| `stone_rimed`, `stone_dark_rimed`, `brick_rimed`, `iron_rimed` | 4 | half the base: 1.5 / 2.0 / 1.25 / 4.0 | base | **base, unchanged** | base flags | base colour with pale frost veins | [mag-3] |
| `knit_iron_hot` | 1 | 3.5 | 2 000 | as knit | Conjured, Hot | orange, emissive 3 | [mag-5] |
| `iron_hot` | 1 | 4.0 | 7 800 | iron's | Hot | orange, emissive 3 | [mag-5] |
| `pitch_b1..b4` | 4 | 0 | 900 | none (not in the graph) | Film; a spread fire class | black film, flames | [mag-5] |
| **Total** | **15** | | | | | | |
| reserved | 3 | | | | | knit tiers (question 15) | later |

- **`floe` is `Indestructible`**: a graph anchor with no new graph rule (water is already indestructible). A bridge of
  floe over a moat is never an island, and a blast never holes it. Its counters are maps, not carves: Heat and
  Shatter turn it back to water.
- **New `MaterialFlags`** (the next free bits when they land; today bits 4-7): `Film` (not a collider:
  `CollisionFilter` excludes it as it does `Liquid`; ignored by the cell graph and the island search; weightless;
  skipped by the server's cast ray; open for operators), `Slick` (read by `StepPlayer`), `Hot` (read by the server's
  hazard scan), `Conjured` (read by the absorb rule, the caps, the piece rule and the editor).
- `GraphNode` stays 72 B: the new ids are only `GraphMaterials` entries.
- **Pitch** is one more fire class in spread's `SpreadTable` (`SpreadTableHash` covers it): K 0 (it is laid burning),
  B 4, pace 10, residue air, quench to `ice` by Freeze and to air by Douse; it never ignites from a neighbour, and as a
  source it lights timber beside and above it by the target's delays, never below (spread's rule). It burns out in
  3-4 s (c) with no registry entry.

### The absorb rule, ablative (server, R1)

A blast whose ray's hit voxel is `Conjured` with `hardness * 16 > power` (fresh `knit_iron`, `slag`) runs **no carve**:
its damage to players and objects happens, its `CarveSphere` is not emitted, and instead it emits
`MapShape(Ablate, sphere hitQ, r 0.25 m)` (map 15: knit and slag -> air; 17 B), and presentation shows violet sparks.
The plate loses a 0.5 m patch per blast and the stone behind it is spared: a 1-voxel Molten Pour coat costs an
attacker one blast per spot (the next blast there carves the stone), a 3-voxel Barricade two blasts per hole.
Without the absorb a thin plate would protect nothing, because a blast's sphere reaches the stone behind it; without
the ablation a one-voxel film would make a whole face immune, which reads wrong and stalls Fellings. The radius is
0.25 m, not more, so one blast cannot hole a Barricade. Hot and rimed knit (3.5) do not absorb, which is the Thermal
and Frost counter. Ablate turns a collider into air, so it runs a site check like an expiry.
`Magic.ABlastOnFreshKnitIsAbsorbed`, `Magic.AnAbsorbedBlastAblatesThePlate` pin it; it is a server decision about
which op to emit, so no op rule changes.

## Conjured matter

### `sim::KnitRegistry` (server and offline server only, never replicated)

- **Per chunk:** a sorted vector of `(voxel index u16, owner u32)` in two layers: `conjure` (knit, slag, ice, floe)
  and `turn` (rime, heat).
- **Per owner** (one per cast and layer): `{dueTick, map (Unknit, Thaw or Cool), caster, voxel list as chunk + RLE}`.
- **Recording.** When a spell's op fills or turns voxels, the server takes the exact changed voxels from the apply
  (`EditReport::changed` and the edit list) and records them; a newer owner overwrites an older entry for the same
  voxel and layer. A knit fill only takes air, so a newer knit can take a voxel only after the older one was carved
  away; the older owner then no longer owns it, and its expiry cannot touch the newer matter.
- **Expiry.** At `dueTick`, in `RunScheduled`, the server builds the mask of the voxels the owner still owns **and that
  still hold one of the ids its map changes**, emits one `MapMask(map)` and erases the entries. A voxel carved or
  turned meanwhile is left out; and since a map is the identity elsewhere, even a stale entry could not change another
  voxel.
- **The cheap expiry.** An owner whose cast put its matter down with one `MapShape` (a Rime capsule or box, a floe
  cylinder, a Heat capsule), or whose voxels lie in a box no other live owner of the same layer shares, expires with
  one `MapShape(map)` over that shape or box (17-29 B) instead of a mask. The registry keeps an `overlapped` bit per
  owner, set on both owners whenever a new owner's box meets a live owner's box of the same layer; every conjured or
  turned voxel has an owner, so with the bit clear the shape changes exactly the owner's own voxels. Masks of 0.3-3 KB
  become 30 B in the common case (`KnitRegistry.ShapeExpiryOnlyWithoutOverlap`).
- **Order.** Due owners run in `(dueTick, owner)` order (R4). Same-tick expiries are never merged across owners, so
  the result does not depend on container order. `KnitRegistry.ReplayGivesTheSameOps` pins it.
- **Per-player cap.** Each player's conjure owners form a FIFO with their live voxel counts. A cast that would pass
  6 000 first expires the player's oldest owners at once (ordinary Unknit ops, with their site checks).
- **Memory (c):** 6 B per recorded voxel, so 100 000 (the region caps) is 0.6 MB, plus about 200 B per live owner.
- **Editor:** the registry is paused in the editor and conjured ids are hidden from its palette (spread.md's precedent):
  no conjured matter in saved scenes, no save-format change.

### Lifetimes (e, tuned in [mag-6])

| Matter | Lifetime | Why |
|---|---|---|
| Barricade Wave wall | 20 s | Holds a push, never a position for good |
| Rising Spire | 15 s | Long enough to climb off onto a wall |
| Molten Pour shell | 25 s | Repair is the defenders' slow tool |
| Rebar pegs | 20 s | A climbing route, briefly: three lances (six pegs) live at once |
| Ice (Glacier Slide, Permafrost Climb, Flash Freeze film) | 12 s / 10 s / 12 s | Control, not terrain |
| Floe (Flash Freeze bridge) | 15 s | Crossing a moat under fire |
| Rime | 8 s | A combo window for a teammate |
| Hot metal | 4 s, then Cool | A short punish |
| Slag | sets into knit after 1.0 s (60 ticks), keeps the cast's lifetime | "Pour, then harden" |
| Pitch | burns out by spread's rule, 3-4 s | Bounded by the fire rule itself |

- At the Settle everything goes, by construction: the region resets from `template + seed`.
- **The crumble warning is presentation only.** No op carries a lifetime. Each spell's `CastFx` names the spell, the
  client derives the lifetime from the (hashed) spell table and keeps a small map of op box -> crumble tick, and shows
  cracks and chaff for the last 2 s. A joiner gets no warning for matter conjured before it joined; accepted.

### Follow-ups (`RunScheduled`)

Bands, Slag's Set, a Shockwave's or Crushing Weight's Shatter flood at the zone tick, and the registry's expiries go
out on their tick, unconditionally: a lift's hover is timed against its bands. Bands, Set and expiries are computed
at `T_run` (or are bounded by their owner's voxel count) and charged whole to the cast's work there; the Shatter flood
is charged at its cap. Work that cannot be known at `T_run`, a site check or Crushing Weight's `extraLoad` check, is
never a follow-up: it goes to the structure credit's queue when its tick comes and is charged by its own counters
when it runs. Follow-up bytes go to the event credit, which may go negative and then delays the next queued cast,
never a follow-up. A band re-excludes the player capsules of its own tick. At most 6 bands per spell.

### Expiry and structure

- **Site checks follow what changed, not the map's name.** Any `MapShape` or `MapMask` that turns a graph-solid voxel
  into a non-collider (air, a liquid, a film), or removes an anchor (floe -> water), queues a **site check** over the
  chunks it changed: the island search plus the load check seeded there (`sim::Blast` with `BlastOptions::noCarve`),
  on the SI plan's **structure credit**, never the cast credit. That covers Unknit and Shatter, Ablate, Heat on ice and
  floe (a spire on a floe bridge falls when Pitchfall melts the floe), and the Douse rows inside Rime and Freeze
  (burning foliage -> air). It is spread.md amendment 2's rule ("into any non-collider") for every op. Releases go out
  as `Detach` / `ClearMask`; leftovers wait in SI-3's re-check queue (R7). Clients never decide this (R1). [mag-1]
  builds `noCarve` and the minimal structure credit if [si-2] / [si-3] have not (shred.md SI-2 "Data": whichever
  comes first introduces it). `Magic.HeatUnderAFloeBridgeReleasesWhatItCarried`.
- **A knit fill never runs the load check.** Knit weight counts in every later check, and knit's density (2 000) keeps
  such falls rare. Ops whose changed voxels all stay colliders (Rime, Thaw, Cool, Set, Knit, Slag, Freeze, Heat on
  metal) change no support and run none.
- **Pieces of conjured matter.** A piece that holds any `Conjured` id never asks `sim::TakePieceSlot`: it is a `Detach`
  with NetId 0, client rubble with its size lifetime (15-30 s), so no replicated body outlives its conjuring and no new
  `DespawnReason` is needed (`Magic.ConjuredPiecesNeverTakeASlot`). Floe never detaches (an anchor).

### Caps (server decisions, R1; offline the same code)

| Cap | Value (e) | Over it |
|---|---|---|
| Live conjured voxels per player | 6 000 (about four walls) | The player's oldest conjurings expire now (an ordinary Unknit); info line `KNIT CAP` |
| Live conjured voxels per region | 60 000 | `CastRejection::TooMuchConjured` |
| Turned (rime, heat) voxels per region | 40 000 | `TooMuchConjured` |
| Zones, live or pending | 4 per player, 32 per region | `TooManyZones` |
| Bodies woken by one Pressure cast | 8, nearest first by (distance in q, NetId) | the others are untouched |
| Fire | spread.md's local and global caps, for every Thermal spell | `TooMuchFire` |
| Operator visits | Flow 12 000, Plume 4 000, Pierce 3 000 member voxels, Burst 6 000, the Shatter flood 4 000 | the operator stops there (part of the rule, deterministic) |
| Spawn areas (the spawn guard) | A box 6 m around each team spawn point of M2a (the setting's Seams), full height | A spell whose op boxes, fill cells or zone shapes meet an **enemy** spawn area: `CastRejection::NearSpawn`. Fill cells inside **any** spawn area, own team included, are dropped. Fire spreading in from outside is spread.md's rule (spawn areas are built of stone) |
| Store slots | knit growing into air chunks materialises chunks: at most about 300 at the region cap (c) | inside the 12 288 store; the game stays under 7 000 (`store a/b` line) |

## The sixteen spells

Common to all: 1 charge plus the form's cooldown; `T_run` is the dequeue tick, `T_z = T_run + lead` (24 ticks by
default) the zone tick; every spell sends one `CastFx` (32 B); damage is server-only and hits enemies. Sizes, depths
and bytes are (c) from the parameters at 0.125 m unless marked (e); work is `CastWorkOf` units (e), 10 000 = 1.09 ms.
"Counter" names the answers.

**How the bytes are counted (c).** A `MapMask` is 21 B plus its runs: varints that alternate over the mask's box in
the order its `axis` byte names, so a mask costs about two varints (2-3 B) per row of its run axis that it crosses.
The server encodes all three orders and sends the shortest (ties: x), so a sheet runs along its own plane and a
column along y. Each figure gives the aligned case (walls along x or z) and the worst aim (a 45 degree wall or face,
where no axis follows the sheet). An expiry is the cheap `MapShape` form (17-29 B, "The cheap expiry") unless another
owner overlaps; the upper figure assumes it does and sends the mask.

### Metal (Knitting): build, patch, climb

**1. Molten Pour (Flow x Metal): plating a crumbling gate, patching a breach, a ramp in a ditch.**
- Lobbed. Flow `rf` 4, `fallMax` 64, `runPops` 1 200, `lateral` 24, `frontCount` 240.
- Fill: `Fall` skin cells (the shell down the face) and `Front` (the pool, filling a breach or trench from the bottom),
  lowest key first, at most 900 voxels, player capsules and spawn areas excluded.
- Ops: `MapMask(Slag)` at `T_run`; `MapShape(Set, the mask's box)` at `T_run + 60` (slag hardens; a voxel carved
  meanwhile is air and stays air; another cast's slag in the box sets early, a lifetime difference only). Registry:
  conjure, Unknit at 25 s.
- Players: 20 splash within 1 m of the landing; slag burns 10 HP/s on contact (hazard scan) for its first second.
- About 450-900 voxels; bytes: slag 0.15 KB aligned to 0.6 KB on a diagonal face, Set 27, Unknit 27 B (cheap) to
  0.6 KB, `CastFx` 32: **0.25-1.3 KB**; work about 4-8 k (**0.4-0.9 ms**), no island search (a fill).
- Counter: Siege Cannon (112 per voxel), Meltdown Beam (heats, then bores), Frost then a blast, blasts chipping it
  (each takes a 0.5 m patch, "The absorb rule, ablative"), or wait 25 s.

**2. Rising Spire (Plume x Metal): an instant scaling tower; sealing an arrow slit.**
- Plume `rf` 3 (0.75 m), `riseMax` 48 (6 m) or the ceiling. A wall hit grows a strip up the face from the sill and
  seals slits; under an arch it fills from the top.
- Ledges: every 8 voxels of height, 6 wide x 4 out x 2 thick (0.75 x 0.5 x 0.25 m) on the side facing the caster's
  yaw (rounded to an axis); a jump (1.08 m) plus a step (0.35 m) reaches the next one (c).
- Zone: **Lift** over the footprint grown by 0.3 m, from `T_z`, everyone (not dispellable: it is part of a build);
  rise 8.6 m/s (0.143 m per tick), eased over the last 0.5 m to a hover floor 0.5 m above the spire's top (or a
  capsule's height under a ceiling). It ends at `lastBandTick + lead` (`T_z + 70` by default): the last band reaches
  every client that holds the zone in time before its predicted lift ends, whatever the session's lead.
- Ops: 6 bands of `MapMask(Knit)`, band k covering [k, k + 1) m. **Timing rule:** band k is emitted at the first tick
  at which the lift's nominal floor is at least band k's top plus 2 voxels, so a rider's capsule grown by 1 voxel is
  always above the band and the band fills the rider's column. The server tick runs `StepPlayers` before
  `RunScheduled`, so after tick `T_z + j` the floor has risen `j + 1` ticks: band k at `T_z + 7(k + 1) + 2` for
  k = 0..4 (floor 1.43 m over band 0's top of 1 m, 5.45 m over band 4's 5 m), band 5 at `T_z + 46` (the ease reaches
  6.25 m at about `T_z + 44`). A 0.3 s ghost shows the whole spire from `T_run`. A player who leaves the lift early,
  or meets a ceiling, gets a gap round the capsule, as any fill leaves. Unknit at 15 s.
- About 1 400 + 6 x 48 = **1 700 voxels**; bytes: 6 bands of 0.12-0.15 KB, `ZoneStart` 32, Unknit 27 B to 0.45 KB,
  `CastFx` 32: **0.8-1.4 KB**; work **1.5-3 k**.
- Counter: Siege Cannon through the core (0.86 m of knit per shot), heat or rime then blasts, Shockwave on the rider.

**3. Rebar Lance (Pierce x Metal): footholds; impaling.**
- Pierce with the aim made horizontal (yaw only, so pegs are steps): `r` 1.5 (0.375 m), `power` 64 (4.0: stone,
  dark stone, brick; not iron or knit), budget 3 360 per socket (c: 1.25 m of stone, 10 slices of 7 voxels at 48), at
  most 3 m.
- **Two pegs per lance**: the walk runs at the hit and again 8 voxels (1 m) above it along the face, with the same
  aim; the upper socket is made only if the voxel 8 above `h` is solid and within the power (else one peg).
- Ops: one `CarveCapsule` per socket, then one `MapMask(Knit)` over both sockets and two pegs of the same
  cross-section 10 voxels (1.25 m) out of the face. Unknit at 20 s.
- Zone: **Root** r 0.5 m at the hit, `T_z`, 0.75 s, enemies. Direct hit: 30.
- About 280 voxels; three lances are six pegs 1 m apart, a ladder to a ledge of about 6.4 m within 10 s (top peg at
  5 m, plus a jump and a step) for one player (the `--spell-demo rebar-lance` line records the height); bytes 2 x 29 +
  knit 0.08 KB aligned to 0.3 KB at 45 degrees + `ZoneStart` 30 + Unknit 27 B to 0.3 KB + `CastFx` 32 + small island
  ops: **0.25-0.8 KB**; work 4-10 k, up to 20 k at a neck (**0.4-2 ms**).
- Counter: a blast at a peg's root (the stone around it carves, the peg falls as rubble), Shockwave knocks climbers
  off.

**4. Barricade Wave (Burst x Metal): instant chest-high cover across a street or a bridge.**
- Burst strip across the aim: half-width `W` 18 (4.5 m wide), half-thickness `T` 1 (3 voxels, 0.375 m), `stepMax` 3,
  extruded 12 high (1.5 m: over the 1.43 m a jump plus a step reaches (c), under the 1.62 m eye). A ceiling hit hangs
  a wall that closes an archway.
- Ops: 4 bands of `MapMask(Knit)` from the centre outward at `T_run + 18 + 4k` (after the 0.3 s ghost). Unknit at 20 s.
- Players standing on the line are left a gap; nobody is trapped. Players running into the line during the RTT meet
  the knit in a replay: a terrain correction, counted by cause
  (`Movement.BarricadeAcrossAWalkerCountsCorrectionsByCause`; "What can still cause a correction").
- About **1 300 voxels**; bytes: 4 bands of 0.04-0.1 KB (x- or z-runs aligned, y-runs at 45 degrees), Unknit 27 B to
  0.35 KB, `CastFx` 32: **0.2-0.8 KB**; work about **2 k** (0.2 ms).
- Counter: Siege Cannon (through 0.375 m of knit, then on), two blasts on one spot hole it, Thermal heat then blasts,
  Frost then blasts, its foot undermined by blasts, or 20 s. You can shoot over it.

### Thermal (Kindling): direct damage, area denial, anti-metal (needs spread S1; sessions S2 and S3)

**5. Pitchfall (working name for the owner's "Napalm"; Flow x Thermal): a burning carpet down stairs and battlements.**
- Lobbed. Flow `rf` 4, `fallMax` 64, `runPops` 1 200, `lateral` 24, `frontCount` 240.
- Ops: `MapMask(Pitch)` over the `Fall` and `Run` skin cells and the `Front`, at most 1 600 voxels; `MapMask(Heat)`
  over the `Contact` voxels Heat changes (timber ignites, metal heats, ice melts, with a site check if it melted
  any). Heated metal: Cool at 4 s.
- From there spread's fire climbs into timber beside or above the pitch, never down; pitch burns out by itself.
- Players: 20 splash; pitch 15 HP/s within one voxel of the capsule (hazard scan).
- Bytes: pitch 0.2-0.5 KB (a carpet runs along x or z), Heat 0.05-0.3 KB, Cool 27 B, `CastFx` 32: **0.3-0.9 KB**,
  then spread parts on the spread credit; work **0.3-0.5 ms**, plus spread.
- Counter: Frost turns the carpet into walkable ice; jump it; wait 3-4 s.

**6. Wildfire (Plume x Thermal): climbs siege towers and timber storeys; roasts those above.**
- Plume `rf` 3, `riseMax` 48, `runPops` 400 under ceilings; `Front` cells only where skin or joined (a floor hit in
  the open leaves no burning voxels hanging in the air).
- Ops: `MapMask(Pitch)` over the `Rise` and `Run` skin cells and the `Front` (a burning strip up a stone face, flames
  under a ceiling); `MapMask(Heat)` over `Contact` (beams and the floor above catch, ice melts).
- Players: 20 to enemies in the plume's cells; pitch 15 HP/s.
- Bytes **0.25-0.75 KB**, plus spread; work about **0.2 ms**, plus spread.
- Counter: Permafrost Climb freezes the face back; Frost quenches.

**7. Meltdown Beam (Pierce x Thermal): bores through armour; lights the room behind; the hardest single hit.**
- Pierce `r` 2 (0.25 m), `power` 80 (5.0), budget 14 400, at most 64 voxels; metals read through Heat (knit 56, iron
  64). Depths (c, about 12.6 voxels per slice): stone 3.0 m, knit 2.5 m, iron 2.2 m.
- Ops, in order: `MapShape(Heat, capsule A..B, r 4)` **before** the carve (knit and iron become hot, so the beam cuts
  them; timber within 0.5 m catches), then `CarveCapsule(A, B, 2, 80)`; then the island search and load check.
  Registry: Cool at 4 s for what Heat turned (the cheap form: the same capsule).
- Players: **45 direct** + Burning 3 s at 8 HP/s = 69.
- Bytes 29 + 29 + carve results (0.5-1.5 KB, like a blast) + Cool 29 + `CastFx` 32 = **0.6-1.6 KB**; work 0.3-4.5 ms
  at a neck.
- Counter: distance (the shortest bore of the anti-metal tools), Frost quenches the hot walls.

**8. Firestorm Sweep (Burst x Thermal): clears a bridge or a courtyard.**
- Burst disc `R` 32 (4 m), `stepMax` 3; the leading half-rim extruded 6 high (0.75 m).
- Ops: `MapMask(Pitch)` over the rim (a fire curtain that blocks sight and passage for 3-4 s); `MapMask(Heat)` over
  `Contact` (a plank deck catches, and later falls by spread S3's burnout site checks).
- Players: 30 at `T_z` to enemies inside the disc (the contracting ring is the warning) + Burning 2 s.
- About 600 pitch voxels; bytes: rim 0.25 KB (y-runs: one run per column of the curved curtain), Heat 0.05-0.3 KB,
  `ZoneStart` 32, `CastFx` 32: **0.35-0.65 KB**, plus spread; work about **0.6 ms**.
- Counter: leave the ring; Frost.

### Pressure (Drawing): mobility, crowd control, demolition; no residue

**9. Crushing Weight (Flow x Pressure): pins players; collapses weak floors under them.**
- Lobbed. Flow's fall phase from the hit's single seed (`fallMax` 64) gives the landing.
- Zone: **Pin**, y-cylinder r 2.5 m, 3 m above the landing, `T_z`, 2.0 s, enemies; 15 damage at `T_z`.
- At `T_z`: the Shatter flood (sphere r 2 m round the landing floor; rimed matter and ice break where exposed), then
  **the load check with an `extraLoad`**: 600 kg/m^2 of the disc's floor area added, as integer weight, to the top
  floor cell of each column in the disc, the check seeded there (shred.md Part 4b gains this optional input; same
  max-flow, same uniqueness). The check is queued at `T_z` on the **structure credit** as a site check and charged by
  Part 4b's counters when it runs; its work cannot be known at `T_run`, so it is never a follow-up of the cast credit.
  Under a busy credit it waits (counted), while the Pin and the damage happen at `T_z` regardless. A weakened plank
  floor or a thin bridge goes. Natural rock never does: 600 kg/m^2 is about 0.23 m of stone, inside rock's 3x margin
  (`Neck.ExtraLoadNeverFailsNaturalTerrain`).
- Bytes `ZoneStart` 32 + Shatter 0-0.3 KB + `CastFx` 32 + any `Detach` (0-2 KB); work: the Shatter flood at its cap
  at `T_run`, the load check **0.2-4 ms** on the structure credit.
- Needs the [si-2] `NeckCheck` split. Counter: step out of the ring; do not stand on planks; a Shockwave frees pinned
  allies and cuts the Pin short.

**10. Updraft (Plume x Pressure): launches allies onto walls; juggles enemies.**
- Plume single-seed column from the hit to the ceiling or 80 voxels (10 m); the zone's radius 1 m, its strength
  ramped to 0 over the outer 0.25 m.
- Zone: **Lift**, y-cylinder, `T_z`, 1.5 s, everyone; vertical target 9 m/s, eased to a hover at the top. A lifted
  player is marked `Lifted`: no fall damage until they land. Bodies in the column: one upward impulse at `T_z`.
- Bytes `ZoneStart` 32 + `CastFx` 32 = **about 65 B**; work about 0.01 ms. The cheapest spell, and the most
  movement-sensitive: the reference case of `Movement.ZonesPredictedWithoutCorrection`.
- Counter: avoid the column; Shockwave cuts the lift short and knocks the lifted away; shoot the known path.

**11. Siege Cannon (Pierce x Pressure): deep holes through stone and metal, nothing left behind.**
- Pierce `r` 3.2 (0.4 m), `power` 128 (8.0: iron and knit cut), budget 24 576, at most 64 voxels (8 m at 0.125 m).
  Depths (c, about 32 voxels per slice): stone 2.0 m, brick 2.4 m, iron 0.75 m, knit 0.86 m, wholly rimed stone
  4.0 m; through a Shatter Spike's 1 m bore and its 0.75 m of rime, about 3.3 m. Stops at bedrock and floe.
- Ops: `CarveCapsule`; the island search and load check (as a blast's, on the cast credit, in the bore's own flood
  box, "A bore's boxes"): it cuts a tower's neck from range through its outer wall.
- Zone: **Impulse**, a capsule from the bore's exit 3 m onward along the aim, `T_z`, 9 m/s along the axis + 2 m/s up,
  enemies: the pressure follows through. Bodies along the bore: impulses at `T_z` (at most 8). Direct hit: **30**
  (structure first; Thermal carries the damage).
- Bytes 29 + `ZoneStart` 42 + `CastFx` 32 + carve results (1-2 KB); work 0.5-4.5 ms at a neck.
- Counter: a Barricade in its path costs it 0.375 m of knit; Molten plating (0.86 m of knit per shot); stand out of
  the follow-through's line (an Impulse acts at its start tick only, so no dispel stops it).

**12. Shockwave (Burst x Pressure): knocks defenders off walls and attackers off pegs; dispels.**
- Burst's seed and plane only.
- Zone: **Impulse**, radial (a y-cylinder r 4 m, 3 m high for a floor or ceiling hit; a sphere r 4 m for a wall hit),
  9 m/s outward + 3 m/s up at the centre, falling to 0 at the edge and to a vertical push inside the dead radius,
  `T_z`, enemies; 15 damage at `T_z`. Bodies in it: impulses (at most 8).
- **Dispel** (the owner's "deflects incoming projectiles"), a **truncation**: at the cast, for every enemy zone of a
  kind that lasts (Pin, Chill, and Updraft's Lift; never Rising Spire's) whose centre lies within 5 m of the hit and
  that is still live at `T_z`, one `ZoneCancel{zoneId, endTick = T_z}`: the zone acts as before up to `T_z` and not
  from `T_z` on. A pending Crushing Weight Pin or Updraft ends at `T_z`; a live one stops there. The cancel carries the
  full lead by construction, so a client that holds zones in time holds the cancel in time too. Instant zones (an
  Impulse, the start of a Root) at or before `T_z` cannot be dispelled; Siege Cannon's follow-through is one.
- **Release** (from [mag-3], when roots and pins exist), an ally zone of the same shape at `T_z`: it clears
  `rootLeft`, `pinLeft` and `slowLeft` of allies in it (their control guard then starts: `ccGuardLeft`). A Shockwave
  frees a rooted or pinned teammate.
- At `T_z`: the Shatter flood in a 2 m sphere (rimed matter and ice burst where exposed, at most 0.5 m deep; from
  [mag-3]), then a site check if it changed anything.
- Bytes `ZoneStart` 30-32 + Release 30-32 + 0-4 cancels x 7 + Shatter 0-0.5 KB + `CastFx` 32 = **0.1-0.65 KB**, plus
  island results; work 0.2-2.3 ms.
- Counter: be outside 4 m; stand against a wall (the push is radial, a wall behind you stops it).

### Frost (Turning): control, bridges, combo setup

**13. Glacier Slide (Flow x Frost): a slick lane down slopes and stairs.**
- Lobbed. Flow `rf` 4, `fallMax` 64, `runPops` 1 600, `lateral` 32, `frontCount` 240.
- Ops: one `MapMask(Freeze)` over the `Fall` and `Run` skin cells and the `Front` (air -> ice, at most 2 048) and the
  `Contact` water and burning timber (water -> floe, fire quenched; a site check where burning foliage became air).
  Unknit at 12 s.
- The film is 0.125 m, under the step height. On it the slick rule applies: momentum carries, 1.3x top speed,
  downhill acceleration on slopes. Attackers slide down fast; defenders on it cannot stop at once.
- Bytes: ice 0.2-0.5 KB, Unknit 27 B to 0.5 KB, `CastFx` 32: **0.25-1.0 KB**; work **0.4-0.7 ms**.
- Counter: Thermal melts it; blasts carve it (1.0).

**14. Permafrost Climb (Plume x Frost): encases ladders, archers and murder holes.**
- Plume `rf` 3, `riseMax` 48. Ice over the `Rise` cells within 2 voxels of support and the `Front` cells that are
  skin or join the ice (a floor hit in the open makes a ring of ice at the base, never voxels hanging 6 m up), so
  openings up to 0.25 m deep (slits, murder holes) are plugged; ice ledges every metre (as Rising Spire's) with a
  0.3 s ghost.
- Ops: `MapMask(Freeze)`; `MapShape(Rime, box over the face, 2 voxels deep)` (primes it brittle). Registry: Unknit
  (ice) at 10 s, Thaw (rime) at 8 s, both usually the cheap form.
- Zones: **Root** 1.2 s, then **Chill** 3 s, over the plume's box, `T_z`, enemies; 15 damage. Their capsules are
  excluded from the ice, and presentation draws an encasing shell for the root.
- Bytes: ice 0.25-0.6 KB (a face sheet runs along its plane), Rime 27, Thaw 27, Unknit 27 B to 0.6 KB, 2 zones 80,
  `CastFx` 32: **0.45-1.4 KB**; work about **0.3 ms**.
- Counter: Wildfire melts it (and whoever stands on it); blasts; Shockwave shatters the ledges.

**15. Shatter Spike (Pierce x Frost): pierces and primes a wall for a teammate.**
- Pierce `r` 1.5, `power` 64, budget 2 688 (c: 1 m of stone), at most 3 m.
- Ops: `CarveCapsule`; `MapShape(Rime, capsule entry..B, r 6)` (0.75 m around the bore and past its end); only
  rimeable ids change. Registry: Thaw at 8 s (the cheap form: the same capsule).
- Zone: **Chill** r 1 m at the hit, `T_z`, 3 s, enemies. Direct hit: 25.
- The combo: rimed stone costs half, so Siege Cannon goes about 3.3 m instead of 2; a blast (power 6) cuts rimed iron
  and rimed knit; Shockwave's Shatter flood peels the exposed rime 0.5 m deep.
- Bytes 29 + 29 + Thaw 29 B (1.2 KB as a mask if another rime overlaps) + `ZoneStart` 30 + `CastFx` 32 + small island
  ops: **0.15-1.3 KB**; work 0.3-2 ms.
- Counter: Thermal thaws it; Metal plating over it (Shatter reaches only exposed rime).

**16. Flash Freeze (Burst x Frost): a walkable bridge over a moat; roots a choke point.**
- Burst disc `R` 24 (3 m) on the hit plane; generated water stops the cast ray, so a shot at a moat lands on its
  surface.
- Ops: on water, `MapShape(FreezeWater, y-cylinder r 3 m, 2 voxels below the surface)`: a floe slab 0.25 m thick,
  about 3 600 voxels where the disc is water (c); on dry cells, `MapMask(Freeze)` over `Path` (a 1-voxel ice film,
  slick) and burning `Contact` (quenched). Registry: Unknit at 15 s (floe -> water, the cheap form: the same
  cylinder, with a site check) and 12 s (ice).
- Zones: **Root** 1.0 s, then **Chill** 3 s, the disc, `T_z`, enemies; 10 damage.
- Bytes 19 + ice 0.1-0.3 KB + Unknit 19-27 B to 0.3 KB + 2 zones 64 + `CastFx` 32 = **0.25-0.75 KB**; work about
  **0.15 ms**; the floe re-cooks the 2-4 water chunks it covers.
- Counter: Thermal melts the bridge, with whoever stands on it; Shockwave shatters it.

### The matrix at a glance

| | Metal | Thermal | Pressure | Frost |
|---|---|---|---|---|
| **Flow** | Molten Pour: slag shell and pool, sets in 1 s, 25 s | Pitchfall: burning carpet, fire climbs | Crushing Weight: Pin zone, Shatter, load check with `extraLoad` | Glacier Slide: slick ice lane, 12 s |
| **Plume** | Rising Spire: pillar, ledges, ride-up Lift, 15 s | Wildfire: pitch up the face, Heat on contact | Updraft: Lift zone 1.5 s | Permafrost Climb: ice plugs and ledges, rime, Root |
| **Pierce** | Rebar Lance: two sockets + horizontal pegs, Root | Meltdown Beam: Heat, bore, Burning | Siege Cannon: power 8 bore, follow-through Impulse | Shatter Spike: bore, Rime capsule, Chill |
| **Burst** | Barricade Wave: 4.5 x 1.5 m wall, 20 s | Firestorm Sweep: fire curtain, Heat, ring damage | Shockwave: radial Impulse, Shatter flood, dispel and release | Flash Freeze: floe bridge, ice, Root + Chill |

Interactions fall out of the tables, with no extra code: Thermal melts ice and thaws rime (Heat); Frost quenches hot
metal and freezes pitch; Pressure and Solid exploit rime (hardness); Metal plates against Solid (knit 7.0) and absorbs
blasts, which chip it; pitch and fire never go down (spread's rule); floe is anchored by water; any carve removes
conjured matter its power allows.

## Players: zones, statuses and the movement rule (revisions 2-5)

### Why zones, and why the lead

- Today the only effects on a player's step are its own inputs and the world, and prediction makes no correction on
  flat ground (`Movement.NoCorrectionAbove2cmOnFlatGround`). A knockback from another player's cast reaches a client
  one way late, after the client has already predicted past the tick it applies to: a correction every time.
- **The fix: announce the effect before it applies.** The server emits `ZoneStart` at `T_run` with
  `startTick = T_run + lead`. The event reaches the client at about `T_run + RTT/2`, while the client is predicting
  about `T_run + RTT + Target() + 2..3` ticks (mechanisms.md, from `Client.cpp`). So the client holds the zone before
  it predicts `startTick` whenever the lead covers that client lead.
- **The lead is 24 ticks (0.4 s) by default**, a session setting (`zone_lead_ticks` in the session file, 12-60, sent in
  `Welcome`). The measured client lead on the soak link is about 10-16 ticks (c), so 24 covers it with margin; a
  12-tick lead would not. Under the soak's loss tails (event p99 0.58-0.79 s (m)) some zones arrive late: they are
  counted, never dropped. Anything timed against the lead (Rising Spire's hover) is written in terms of the lead, so
  a session that raises it stays consistent.
- The same 0.4 s is the **telegraph**: the zone's ring is drawn when its event arrives and fires at its start. At
  150 ms ping a player still sees it about 0.3 s before it fires: fair and dodgeable.
- **Membership, not hits.** Every peer runs `StepPlayer` for a tick with the zones live at that tick and tests them
  against its own `PlayerState`, from feet quantised to q. The two sides' feet can differ by millimetres (up to 2 cm
  on flat ground), so an effect must not jump there. **Impulse** falls to 0 at its rim and its horizontal push shrinks
  to 0 inside a dead radius `R / 8` round its centre, so a millimetre never reverses a 9 m/s push (a Shockwave aimed
  at the floor under an enemy's feet is the usual case). **Lift** ramps its strength to 0 over its outer 0.25 m.
  **Pin, Chill and Root** are all-or-nothing: the two sides can disagree only for feet within their difference of the
  rim; that correction is counted under the cause `zone edge` and accepted. `Player.ZoneEdgeAndCentreAreContinuous`.

### `sim::ZoneSet` (in the `Simulation`, on every peer)

- A zone: `{zoneId u16, kind u8, teamMask u8, shape, startTick u32, duration u16, strength i16 x 2}`. Kinds: Impulse
  1, Lift 2, Pin 3, Chill 4, Root 5, Release 6, and Block 7 (reserved, "What can still cause a correction"). Shapes as
  `MapShape`'s: sphere, capsule, box, y-cylinder, in q.
- Filled by `ZoneStart` events at their opSeq. A zone acts on the ticks `[startTick, startTick + duration)`.
  `ZoneCancel{zoneId, endTick}` is a **truncation**: `duration = min(duration, endTick - startTick)`; the ticks before
  `endTick` are unchanged. The server issues it only at a Shockwave's `T_run` with `endTick = T_run + lead`, so it has
  the full lead by construction; under loss it is late like any event, and counted as `zone late`.
- **Retention for replays.** A zone's record is kept while any tick that may still be stepped can see it. The server
  prunes a zone once its own tick passes `startTick + duration`. A client keeps it until its oldest replayable tick,
  `max(nextTick - kHistoryTicks, last compared snapshot tick)`, passes `startTick + duration`: a correction replays
  from the snapshot's tick, about RTT + Target + 2 ticks behind the predicted one (`Client.cpp` `Reconcile`, 128 ticks
  of history), and a zone cleared when the predicted tick passed its end would be missing from every replay that
  crosses its span, which would then differ from the server and correct again. **Every lookup passes the tick being
  stepped**: a replayed `StepPlayer` passes its history entry's own tick (`HistoryEntry::tick`), never `nextTick`.
  `Movement.ZoneSurvivesAReplayAcrossItsEnd` (a correction forced right after a Lift ends gives no second correction).
  A client holds at most 64 records (32 live or pending, by the server's cap, plus recently ended ones); past that the
  oldest ended record goes, and a replay that needed it corrects and is counted.
- A lookup tests the player's reach against each zone's box (negligible (c)).
- **Joiners** get the live zones in the entity baseline (at most 32 x 42 B, about 1.3 KB).
- **Late zone events.** A zone whose `startTick` the client already predicted past is still inserted; the client
  restores its last acknowledged state and replays, as any correction does, and logs
  `Client: zone late by N ticks`. The server's result already contains the zone, so dropping it would only make the
  correction larger.
- Offline (M2a's in-process server) runs the same events through loopback; [mag-1]'s headless scripts fill the
  `Simulation`'s `ZoneSet` directly.

### `PlayerState`, `StepPlayer` and the physics call

**New state** (integer, part of the owner block, restored before every replay; 12 B):

| Field | Type | Meaning | Per tick |
|---|---|---|---|
| `push` | i16 x 3, cm/s | External velocity (knockback, the carry-over of a launch) | Added to the wish. On the ground each axis moves toward 0 by 45 cm/s (27 m/s^2); in the air by 1 cm/s; on slick ground by 6 cm/s. Clamped to +-1 500 |
| `status` | u8 bits | Rooted, Pinned, Chilled, Lifted, Free (the control guard), Fresh (just respawned) | from the counters; Lifted cleared on landing; Fresh cleared when the guard ends or the player casts |
| `slowQ` | u8, sixteenths | Speed cut while `slowLeft > 0` | - |
| `slowLeft`, `rootLeft`, `pinLeft` | u8 ticks | Remaining duration (at most 255 = 4.25 s) | count down by 1 |
| `ccGuardLeft` | u8 ticks | **The control guard.** Set to 90 (1.5 s) when `rootLeft` or `pinLeft` reaches 0 and neither is left, and to 120 (2 s) with `Fresh` at a respawn. While above 0, Root and Pin zones do not apply (Chill and Impulse still do); while `Fresh`, Impulse does not apply either | count down by 1 |

**Order inside `StepPlayer(physics, terrain, world, zones, character, state, input, tuning, area, tick, dt)`** (new
arguments: `const voxel::World&` for the ground id, `const sim::ZoneSet&` and the tick):
1. Button edges, the cooldown and the cast and spell edges, as today. **Chill** stretches the cooldown ("slows cast
   speed") through one function, `sim::CastCooldownFor(state, tuning)` = `castCooldownTicks * (16 + slowQ / 2) / 16`
   with the `slowQ` of the state at that tick. `TakeCast` restarts the cooldown with it; `Server::LateCast` sets it
   from the state at the input's tick (`StateAt(in.tick)`), never from the plain `settings.player.castCooldownTicks`
   as today, and its rewrite of the kept states counts down from that value. Otherwise a chilled player's late cast
   would get an unchilled cooldown on the server while its client predicted the chilled one.
   `Movement.LateCastWhileChilledIsJudgedAsPredicted`.
2. The gate (`HasCollider`), as today.
3. **Zones** live at `tick`, in `(startTick, zoneId)` order, whose `teamMask` includes the player's team. The feet are
   quantised to q (round to nearest).
   - **Impulse** (`startTick` only; skipped while `Fresh`): `d = feetQ - centreQ` (for a capsule zone, from the
     nearest point of its axis), `len = isqrt64(|d|^2)`, `f = strength * max(0, R - len) / R`;
     `push.xz += d.xz * f / max(len, R / 8)` (integer, cm/s; continuous, 0 at the centre), plus
     `strength2 * (R - len) / R` up.
   - **Lift** (while live): the vertical target is `max(vy, strength * ramp)` cm/s, `ramp` rising from 0 at the rim
     to 1 at 0.25 m inside it, eased to 0 over the last 0.5 m below the zone's top (a hover); sets `Lifted`.
   - **Pin** (while live and the player is inside; not while `ccGuardLeft > 0`): `pinLeft = max(pinLeft, strength2)`,
     a fixed tail (30 ticks): a pinned player stays pinned 0.5 s after leaving the ring or after the zone ends. The
     tail never reads the zone's remaining duration, so a truncation changes only ticks from its `endTick` on.
   - **Chill**: `slowLeft = max(slowLeft, strength2)`, `slowQ = max(slowQ, strength)` (Chill's `slowQ` 6: speed x 0.625).
   - **Root** (`startTick` only; not while `ccGuardLeft > 0`): `rootLeft = max(rootLeft, duration)`.
   - **Release** (`startTick` only; allies): `rootLeft = pinLeft = slowLeft = 0`, `slowQ = 0`.
4. **Wish**: `wish * (16 - slowQ) / 16`; 0 while rooted; `x 6 / 16` while pinned; no jump while rooted or pinned.
5. **Slick ground.** The ground voxel is the voxel holding `feetQ - 8` q, half a voxel under the quantised feet. It is
   never `floor(y / voxelSize) - 1` from the float feet: Jolt rests the feet at `k * voxelSize` plus or minus about
   1e-5, so that test flips between the surface voxel and the one under it, and the server and the predicting client
   could pick differently. If the ground voxel has `Slick`, the horizontal velocity is
   `v + clamp(wish * 1.3 - v, +-a_ice * dt)` with `a_ice` 3 m/s^2, from the previous state's velocity, plus 3 m/s^2
   downhill where the ground is steeper than 10 degrees, capped at 1.4 x run speed. Momentum carries. Otherwise the
   wish applies at once, as today. `Player.SlickGroundReadsTheSameVoxelAtASurface` (feet at `k * voxelSize` +- 1e-4
   on a 1-voxel film).
6. **Physics call**: `UpdateCharacter(character, dt, desired = wish' + push / 100, jumpSpeed, vertical)`. `vertical`
   is a new `std::optional<float>` parameter: Lift sets `velocity.y = max(velocity.y, v)` in both the ground and the
   air branch (it leaves the ground); a pin adds `-2 g dt` while airborne. Other callers pass `nullopt`. A Physics API
   addition.
7. The play area as today: **the outward `push` is cut the same way as the outward wish**, so a knockback never
   carries a player past the play bounds (WP1's contract, `Player.KnockbackStopsAtThePlayArea`).
8. Counters count down; when the last of `rootLeft` and `pinLeft` reaches 0, `ccGuardLeft = 90`; `push` decays.

- **`PlayerTuning` gains** (travels in `Welcome`): the decay rates, `a_ice`, the slope acceleration and cap, the slick
  top speed, the guard lengths; the lead travels beside it.
- **`kPlayerRuleRevision` goes up in every stage that changes what `StepPlayer` returns:** 2 in [mag-1] (`push`,
  Impulse, Lift, the `vertical` parameter), 3 in [mag-3] (Chill, Root, Release, the control guard and `Fresh`), 4 in
  [mag-4] (slick ground), 5 in [mag-7] (Pin). Each stage changes `buildHash` through its tables anyway, so a bump per
  stage costs nothing and keeps every rule change visible (`SessionFile.BuildHashSeesThePlayerRuleRevision`). The
  owner block's bytes are all reserved in [mag-2]'s protocol bump.
- The movement rule stays float, the same on both sides of one build, as today.

### What can still cause a correction

| Cause | When | Bound |
|---|---|---|
| A zone event later than its lead | RTT + client margin over 24 ticks (about 300 ms RTT (c)), or a lost reliable packet under loss | Counted (`zone late`); G16's bound (under 0.5 m, provisional) stays; the soak logs the counts |
| Terrain changing under the feet | Any world op (knit, ice, carve) applies on the client about RTT/2 after the server | Existing behaviour ("a step at a crater rim"). Slick changes only acceleration, so a player who does not change input on new ice shows no error |
| A fill where a predicted capsule stands | Bands (Barricade Wave, Rising Spire) are not announced ahead. The server skips every capsule (grown by 1 voxel) at the band's tick, but an enemy whose predicted path enters the band's cells in the RTT + Target + 2 ticks before the op arrives meets the knit in a replay | **Expected** for players charging through a Barricade line, not rare. Jolt depenetration, a correction counted under `terrain`; `Movement.BarricadeAcrossAWalkerCountsCorrectionsByCause` measures it. If [mag-2]'s session-check shows it often, the next step is the reserved **Block** zone: announced with the lead, it cuts the wish into the band's box from its start tick, and the band op goes out at that tick |
| A zone's rim | Pin, Chill or Root with feet within the two sides' difference of the rim | Counted under `zone edge`; rare (c) |
| A zone whose world target moved | A Siege Cannon's follow-through zone was announced, then a wall rose in its path | The zone stays where announced (v1); counted |

**Acceptance:** `Movement.ZonesPredictedWithoutCorrection` (net, in-process, 40 ms + 0-30 ms each way, no loss, 10
zones of every kind across two clients, a truncation among them) gives **0 corrections**;
`Movement.SpireRiderNoCorrection` on G16's link (40 ms + 0-30 ms, 5 % loss) gives none for the rider.

### Damage and hazards (server only, M2a's health)

- Direct and splash damage come from the `SpellDef`, at the cast (direct) or at `T_z` (zones and rings). Friendly fire
  is off.
- **Hazard scan**, every 6 ticks: the voxel ids within one voxel of each player's capsule. Pitch: 15 HP/s; a `Hot` id
  (slag, hot metal) face-touching the capsule: 10 HP/s. At most one Thermal hazard per tick, the highest; Burning
  (8 HP/s) does not stack with it above 15 HP/s.
- **Fall damage** after a Lift waits until landing: a launched player who lands takes none for that fall (`Lifted`), so
  a juggle sets up shots and never kills by itself.
- None of it is predicted: health is server state in the snapshot (M2a).

## Structure

- **Bores** (`CarveCapsule`) run the island search and the load check like a blast's carve, on the cast credit.
- **Any op that turns a collider into a non-collider** (expiries, the Shatter flood, Ablate, Heat on ice and floe,
  the Douse rows) runs a site check over its changed chunks on the structure credit ("Expiry and structure").
- **Fills never run one.** Ops whose changed voxels all stay colliders (Rime, Thaw, Cool, Set) change no support and
  run none.
- **`extraLoad`** (Crushing Weight only): an optional, server-only input to Part 4b's check: integer kg added to the
  load of the top floor node of each column in a box, for this one check, run as a site check on the structure
  credit and charged by Part 4b's counters when it runs. The max-flow and its uniqueness are
  unchanged. `Neck.ExtraLoadNeverFailsNaturalTerrain`, `Neck.ExtraLoadReleasesAWeakFloor`, calibrated by
  `--crush-demo` (a 4 m plank walkway falls, a 3 m stone arch stands).
- **Rime is hardness only.** Shear and tension x 0.5 on rimed ids is the documented first knob if play wants
  chill-fellings (question 12); then Rime would need site checks.
- `GraphNode` stays 72 B; the stage-1 test arenas still release nothing; `--neck-blast`, `--tower-blast` and the castle
  case are unchanged.

## Determinism and replication

### Wire values (one protocol bump, in [mag-2])

Every value below takes the **next free number when it lands**; [notes/net.md](../notes/net.md) section 2 is the
registry (its rule was written there with this design's review), and the wire tests pin it. On today's ROADMAP order
([mag-2] before spread S2, [mech-1] and shred's optional `ShredSphere`) the numbers in brackets apply; mechanisms'
`MechanismMove` and `RopeCut` then take 40 and 41. Sizes include the event kind byte and follow from the layouts.

| Value | Layout | Size |
|---|---|---|
| `WorldOpKind::MapShape` (7) | kind u8; map u8; shape u8 (0 sphere, 1 capsule, 2 box, 3 y-cylinder); sphere: centre i32 x 3 q, radius u16 q; capsule: A, B i32 x 3 q, radius u16 q; box: min, max i32 x 3 voxels; cylinder: centre i32 x 3 q, radius u16 q, half height u16 q | 17 / 29 / 27 / 19 B (spread's 17-byte `IgniteSphere` becomes a 17-byte sphere `MapShape`) |
| `WorldOpKind::MapMask` (8) | kind u8; map u8; axis u8 (0 x, 1 y, 2 z: the run axis of the mask's bit order; the server sends the shortest of the three); origin i32 x 3 voxels; dims u16 x 3; RLE runs over the box in that order (the `ClearMask` run encoding) | 21 B + runs |
| `WorldOpKind::CarveCapsule` (9) | kind u8; A, B i32 x 3 q; radius u16 q; power u16 | 29 B |
| `EventKind::ZoneStart` (37) | kind u8; zoneId u16, zone kind u8, teamMask u8, startTick u32, duration u16, strength i16 x 2; shape u8 + its parameters as `MapShape`'s | 30 / 42 / 40 / 32 B (sphere, capsule, box, cylinder) |
| `EventKind::ZoneCancel` (38) | kind u8; zoneId u16, endTick u32 (a truncation) | 7 B |
| `EventKind::CastFx` (39) | kind u8; slot u8, spell u8, originQ i32 x 3, hitQ i32 x 3, face u8, tick u32 | 32 B |
| `MessageType::Attune` | client -> server: craftA u8, craftB u8; accepted while dead or before the first spawn | 3 B |
| `PlayerInput::spell` | u8: `SpellId` (0-15; 16, 17 drill-only); read only on the Spell edge | +1 B per input |
| The Spell edge | M2a's generic action-slot bit for slot 1 (the blast is slot 0) if M2a built one; otherwise `PlayerButton_Spell = 32`, the one exception to the registry rule, written into notes/net.md so mechanisms' `Use = 16` keeps its value whatever lands first (`kPlayerButtonMask` 47, then 63 with [mech-1]) | 0 B |
| Owner block | `PlayerState` additions: `push` 6, `status` 1, `slowQ` 1, three counters 3, `ccGuardLeft` 1 = 12 B; for the HUD: the four form cooldowns as u8 each in 4-tick units (ticks left until ready; 540 ticks is 135), the attunement pair and the active element 1 B = 5 B. **17 B**: 32 today, 37 with mechanisms, **about 54 B** with both (c), plus whatever M2a adds for health and charges | |
| Remote player record | + 1 B: 4 status bits, the active element (2 bits) for the hands' colour | |
| Roster (M2a's player list) | + 1 B per player: the attunement pair (the team panel's letters) | |
| `Welcome` | the lead (u8 ticks) and the `PlayerTuning` additions (decay rates, slick values, guard lengths) | about 18 B |
| `CastRejection` | `FormCooling`, `NotAttuned`, `TooMuchConjured`, `TooManyZones`, `TooMuchFire` (shared with spread: whichever lands first defines it), `NearSpawn`; after M2a's `NoCharge` if it adds one | |

Also reserved in this bump: zone kinds 1-7 and shapes 0-3, map numbers 1-15, `SpellId` 0-17, the form and element
order, the whole owner block. So [mag-3], [mag-4] and [mag-7] change only tables, ids and the player rule revision,
which `buildHash` covers, and need no protocol bump; [mag-5]'s sessions ride spread S2's bump (`SpreadStep`).
spread's `SpreadStep` then takes the kind after `CarveCapsule` (10 on today's order).

**Decoder limits**, as for the sphere ops: radius at most `kMaxCastRadiusQ` for spheres and cylinders; a capsule at
most 64 m (8 192 q) long with radius at most 1 024 q; a box inside +-`kMaxSyncChunkCoordinate`; `map` below the table
count and not 0; `axis` at most 2; mask voxels charged against `maskVoxelBudget`; a zone's shape as an op's, its
duration at most 600 ticks, at most 32 live; a `ZoneCancel`'s `endTick` not before its zone's start.

**Box-based.** All three new op kinds' chunk ranges are their boxes, so `OpRing`, `ChunkSync`, `CoversChunk` and the
deferred-check drop work as for kinds 1-6 (only `SpreadStep` needs list membership).

**`buildHash`** gains `TurningTableHash` (the maps) and `SpellTableHash` (operator parameters, element verbs, bore
costs, lifetimes, zone parameters, cooldowns, damage); the ids and flags go through `MaterialTableHash`, pitch through
`SpreadTableHash`; `kPlayerRuleRevision` is 2 from [mag-1] and rises to 5 by [mag-7].

**`kProtocolVersion`**: the next number after M2a's 11, so 12 in [mag-2] on today's order, and [mech-1] takes 13.

### Apply (`sim::ApplyOp`, every peer, with or without a `ChunkClip`)

- `MapShape`: for each voxel inside the shape (int64 tests, `EditRegion`'s (z, y, x) order), `id = map[old]`.
- `MapMask`: for each masked voxel, `id = map[old]`. The `axis` byte changes only the order the runs are read in, never
  which voxels are masked (`MapMask.AxisOrderGivesTheSameVoxels`).
- `CarveCapsule`: for each voxel inside the capsule, carvable and with `hardness * 16 <= power`, air.
- Every write goes through `World::EditRegion` (or spread S1's shared `EditVoxelsInChunk`), so revisions, graphs,
  hashes and `EditReport::changed` all follow. A voxel decides from (voxel, old id, op integers, table) only: applying
  every chunk with its own clipped call equals the whole call. `EditReport::outOfBounds` refuses an op whole (the
  server never builds one above the ceiling).

### Determinism traps (each has a test)

- **Ops** decide per voxel: `Magic.ClipEqualsWhole` (every chunk clipped, in any order, equals the whole, for all three
  kinds); the extended `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes`.
- **Tables** are authored in integers, never derived from floats: `TurningTable.HashCoversEveryMap`,
  `SpellTable.HashCoversEveryField`; a configure-time grep refuses `rand`, `mt19937` and `Hash3` in
  `internal/Propagate*.cpp`, `internal/Spell*.cpp`, `internal/Magic*.cpp` and the pitch rows (spread's grep,
  extended).
- **The capsule test** is int64 with coordinates relative to `A`: `CarveCapsule.MatchesAChainOfSpheres` (a dense chain
  of spheres, voxel for voxel), `CarveCapsule.FarCoordinatesStayExact`.
- **Operators**: every heap key ends in (z, y, x); the visited set is never iterated; results independent of threads,
  cache and packing (`Propagate.ResultIndependentOfLoadOrderPackingAndThreads`); mirrored setups give mirrored roles.
- **The server's written world equals its ops applied through `ApplyOp`** (`--check-spells`, debug builds, as
  `checkIslandSearch` for blasts).
- **The arc trace** is integer only, in 1/4096 voxel so gravity does not round to 0:
  `Flow.ArcTraceIsIntegerAndReproducible`, `Flow.ArcMatchesTheParabola`.
- **Bookkeeping per voxel:** `shapeChanged` and the site-check trigger come from each voxel's old and new id, never
  from the map's name (`Magic.ShapeChangedFollowsTheVoxelsNotTheMap`).

## Presentation (client only; never in sim, wire or hashes)

- `Engine::PresentSpell`, beside `PresentBlast`, turns `CastFx`, the applied ops and `ZoneStart` events into effects,
  all `EffectDesc` data under the `FxDirector`'s caps (fx-budget.md: 1 500 spawns per frame, 8 192 alive, GPU +4 ms at
  most, CPU destruction + effects 4 ms):

| Effect | Cap per instance (e) | Where |
|---|---|---|
| Cast streak, arc or wisp (`CastFx`) | 32 particles | every spell |
| Impact burst, one shape per element | 120 (angular sparks, embers, rings, hexagonal shards) | every spell |
| Ghost of rising matter | a translucent box from the spell table at the hit, 0.3 s | Barricade, Spire, Permafrost ledges |
| Band dust (knit or ice rising) | 40 per band | Metal, ice spells |
| Zone ring (contracting) | 48 particles or a ground ring | zones |
| Lift streaks | 20 per second per zone | Updraft, Rising Spire |
| Crumble warning and chaff at expiry | 1 per 8 surface voxels, at most 160 | Unknit |
| Steam (Heat on ice, Rime on hot) | 1 `Smoke()` puff per op | |
| Flames, embers | spread S5 | Thermal |
| Effect light | 1 per Thermal impact, merged into at most 8 | Thermal |
| Glow | emissive by id (slag 5, hot 3, pitch stages) | no particles |

- At 10 casts/s this is about 2-3 k spawns a second and 2-4 k alive (e), inside the 8 192 pool; the `FxDirector`'s LOD
  and load factor scale it. Until fx-budget stage 2 exists, the per-instance caps above bound it.
- States are material ids, so knit, slag, hot, rimed, ice, floe and pitch cost nothing beyond the existing emissive
  voxel light (1.5 ms at 0.1 m (m)); more emissive voxels in view is measured, not assumed.
- Pitch is drawn as full glowing voxels; a lower fill height comes only if the renderer gets per-id fill heights.
- Fog ([fog-1] puffs; [fog-3] when built): Thermal impacts add smoke puffs, Thermal meeting Frost adds white steam,
  each client from the ops it applied.
- No audio exists yet. When the audio pass lands, each form gets a travel sound and each element an impact sound:
  half of readability is sound.

## Budgets and costs (all (e) unless marked; each names its measurement)

### Server tick (60 Hz, the UHD 620 laptop as server)

New order: `Receive` -> streaming -> `StepPlayers` (with the `ZoneSet`) -> `RunCasts` (blasts and spells: ray,
operator, immediate ops, zone events, schedule, registry) -> **`RunScheduled`** (bands, Set, Shatter at arm,
expiries due) -> `RunSpread` -> `RunSiteChecks` (structure credit) -> physics -> lifetimes -> despawns, rests, wakes
-> per peer: events, checks, bulk -> snapshots.

**Work (R5: counted, never timed or cache-dependent).** `sim::CastWorkOf` gains a `SpellResult` overload:
`search += visits * 1 + chunksRead * 200`, where Pierce's visits are its new member voxels (`sum |N_k|`) plus
`slices x kPierceSliceWork` (100) for the slice scans; the Shatter flood is charged at its cap (4 000 visits plus the
chunks of its box) at `T_run`, a bound from the table; the island search and load check of a bore as for a blast;
`ops` = `OpWork` of every op the spell emits **including its follow-ups**, charged whole at `T_run` (`MapShape`:
`kOpWork` + box voxels / 16; `MapMask`: `kOpWork` + mask voxels / 128, as `ClearMask`; `CarveCapsule`: `kOpWork` + box
voxels / 16). Site checks (expiries, shatters, ablation, Heat on ice) and Crushing Weight's `extraLoad` check are not
cast work: they run on the structure credit when their tick comes and are charged by Part 4b's counters there. Never
`graphsBuilt`, `chunksDecoded`, registry sizes or time. `CastBudget.SpellWorkIsEqualColdAndWarm` and
`CastBudget.ScheduleFollowsCountsNotTimeOrCache` (extended) pin it. Spells share the cast queue, `kCastWorkPerTick`
(60 000) and the event byte credit with blasts.

Bytes are (c) from "How the bytes are counted" (aligned / worst aim with an overlapping owner); work and ms (e).

| Spell | Ops at the cast (+ later) | Bytes (c) | Cast work units (e) | ms (e) |
|---|---|---|---|---|
| Molten Pour | 1 (+ Set, Unknit) | 250-1 300 | 4-8 k | 0.4-0.9 |
| Rising Spire | 0 (+ 6 bands, Unknit), 1 zone | 800-1 400 | 1.5-3 k | 0.2-0.3 |
| Rebar Lance | 3 (+ Unknit), 1 zone | 250-800 | 4-20 k | 0.4-2 |
| Barricade Wave | 0 (+ 4 bands, Unknit) | 200-800 | 2 k | 0.2 |
| Pitchfall | 2 (+ Cool), spread after | 300-900 | 3-5 k | 0.3-0.5 |
| Wildfire | 2 (+ Cool), spread after | 250-750 | 2 k | 0.2 |
| Meltdown Beam | 2 + island ops (+ Cool) | 600-1 600 | 3-40 k | 0.3-4.5 |
| Firestorm Sweep | 2 (+ Cool), 1 zone, spread after | 350-650 | 5 k | 0.6 |
| Crushing Weight | 0 (+ Shatter; the load check on the structure credit), 1 zone | 60-2 400 | 5 k (+ 2-40 k structure) | 0.5 (+ 0.2-4) |
| Updraft | 0, 1 zone | 65 | 0.1 k | 0.01 |
| Siege Cannon | 1 + island ops, 1 zone | 1 100-2 100 | 5-45 k | 0.5-4.5 |
| Shockwave | 0-4 cancels (+ Shatter), 2 zones | 100-650 | 5 k (+ site check on the structure credit) | 0.5 |
| Glacier Slide | 1 (+ Unknit) | 250-1 000 | 4-7 k | 0.4-0.7 |
| Permafrost Climb | 2 (+ Thaw, Unknit), 2 zones | 450-1 400 | 3 k | 0.3 |
| Shatter Spike | 2 + island ops (+ Thaw), 1 zone | 150-1 300 | 3-20 k | 0.3-2 |
| Flash Freeze | 2 (+ Unknit), 2 zones | 250-750 | 1.5 k | 0.15 |

Measured by the `Magic:` line (per spell: ops, bytes, work, ms per stage, avg / max), `--siege-matrix`, and
`vortex_server --bot-spells`. The spread is 0.01 ms (Updraft) to about 4.5 ms (a bore at a neck), the envelope of
today's blast (0.3-2.9 ms + builds up to 2.8 ms in game (m)).

**Fight case (e):** 10 casts/s, uniform over the 16 spells, is about 0.17 casts per tick at about 8 k units: about
1 400 units per tick, 2-3 % of `kCastWorkPerTick`. Two heavy casts on one tick (up to about 90 k) are absorbed by the
queue's 6-tick debt. **Acceptance:** `vortex_server --bot-spells` with 10 bots at 10 casts/s on `playground.session`
keeps `Ticks: server:` p99 under 16 ms ([wp-8]'s gate), with `Magic:` and `Spread:` lines.

### Bytes per peer at 10 casts/s with 10 players (c, from the per-spell figures)

The form cooldowns cap one player's spells at about 0.56 per second (1/5 + 1/7 + 1/9 + 1/9), so of the design case's
10 casts/s at most about 5.6 are spells and the rest, about 4.4, are blasts. Today's blast-only soak measured
22.7-24.5 kB/s at 10 casts/s (m), about 2.4 KB per blast, against the 24 kB/s event credit: blasts alone fill it.

| Stream | Per cast | Per second | Budget |
|---|---|---|---|
| Spells (ops, zones, `CastFx`, follow-ups, expiries, carve results) | 0.34-1.1 KB (the average over the 16, aligned / worst) | 1.9-6.2 kB/s at 5.6 spells/s | event credit 24 kB/s |
| Blasts (M2a) | about 2.4 KB (m) | about 10.5 kB/s at 4.4 blasts/s | event credit |
| **Events in all** | | **about 12-17 kB/s (c)** | **24 kB/s**: 50-70 % of the credit. Spells are cheaper than blasts because most do not carve, so a fight with spells sends fewer bytes than the blast-only soak. Over the credit the shared queue delays the next cast, never a follow-up |
| Fire (Thermal spells, 2-3 fires alive) | | 3-8 kB/s | spread credit 20 kB/s |
| Snapshot | owner block +17 B, remote +1 B per player | +0.4-0.7 kB/s | 1 200 B per snapshot |
| Uplink | `spell` byte | +0.1-0.24 kB/s | |

**Ring (1 024 ops / 1 MiB):** about 3 ops per cast plus follow-ups is about 35 entries/s, about 1 KB/s of ring bytes:
it wraps in about 25-30 s (c); spread adds about 10 parts/s. L12 holds: no fallback assumes "everything".

### Client CPU, GPU and memory (UHD 620)

- **Apply:** a `MapMask` of 1-2 k voxels 10-40 us; a `MapShape` over a box up to 65 k voxels (one table lookup each)
  50-200 us; `CarveCapsule` as a sphere carve. **Acceptance:** game `Magic:` apply max <= 1 ms per op.
- **Collider re-cooks** only for chunks where a voxel's collidability changed (knit, slag, ice, floe, carve, shatter,
  ablation, unknit, melt), 2-6 chunks per spell, 0.1-0.4 ms each on workers; id-only changes never re-cook
  (`shapeChanged = false`, decided per voxel).
- **GPU uploads (c):** occupancy changes upload whole chunks (36 936 B): a Barricade 2-4 chunks, about 150 KB; id-only
  maps upload 64 B blocks (spread S1). A spell makes about 3 shape-changing ops (fill or bands, carve, ablation,
  expiry) of 2-4 chunks each, about 9 chunk uploads; 5.6 spells/s and 4.4 blasts/s (3-4 chunks) are about 65 uploads
  a second, **about 2.4 MB/s**, and **about 3.7 MB/s** if every cast were a spell: 40-62 KB a frame at 60 fps (bands
  land on separate ticks, so they do not coalesce). The copy is small against the UHD 620's shared memory bandwidth;
  under 0.1 ms a frame (e). Measured with `--stress-spells` (G23) and the `GPU` upload line at a fixed `--camera`.
- **Effects:** `--stress-spells 10` keeps destruction + effects CPU <= 4 ms and GPU effects <= 4 ms.
- **Memory:** registry up to about 0.6 MB (server), zones about 2 KB (64 records), operator scratch at most 45 KB per
  call; conjured chunks under about 300 more store slots per client (`--memory`, the `store a/b` line).
- **HUD:** about 110 more quads, under 0.01 ms (`HUD: N quads`).

## Tests

**VortexSimTests (GPU-free):**
- Operators: `Propagate.FlowNeverRises`, `Propagate.PlumeNeverFalls`, `Propagate.BurstCellsRestOnTheirSurface`,
  `Propagate.BurstNeverStepsMoreThanStepMax`, `Propagate.FlowFillsABasinBottomUp` (the `Front` is the lowest cells),
  `Propagate.FlowPoursOverAnEdge`, `Propagate.PlumeFillsAnArchFromTheTop`,
  `Propagate.PierceStopsAtBedrockAndWhenTheBudgetIsSpent` (golden depths per material and spell),
  `Propagate.MirroredSetupGivesMirroredRoles`, `Propagate.ResultIndependentOfLoadOrderPackingAndThreads`,
  `Propagate.WorkIsEqualColdAndWarm`, `Propagate.StaysInTheFloodBoxAndUnderTheCeiling`, `Pierce.WalkAndCapsuleAgree`,
  `Pierce.BoreStaysInsideItsFloodBox` (a 64-voxel bore at the play bound, at 0.1, 0.125 and 0.15 m),
  `Propagate.PlumeLeavesNoFrontCellHanging`, `Propagate.ShatterReachesOnlyExposedRime` (a knit plate over rime
  protects it; at most 4 voxels deep), `Flow.ArcTraceIsIntegerAndReproducible`, `Flow.ArcMatchesTheParabola`,
  `Raycast.ASpellRayPassesPitchAndHitsTheStoneBehind`.
- Ops and tables: `Magic.ClipEqualsWhole`, `MapMask.DecidesFromTheOldIdOnly`, `CarveCapsule.MatchesAChainOfSpheres`,
  `CarveCapsule.FarCoordinatesStayExact`, `MapMask.AxisOrderGivesTheSameVoxels`,
  `Magic.ShapeChangedFollowsTheVoxelsNotTheMap`, `TurningTable.NeverNameBedrockAndIdentityElsewhere`,
  `TurningTable.HashCoversEveryMap`, `SpellTable.HashCoversEveryField`, `Materials.BoreCostMatchesHardness`.
- Spells: `Magic.SixteenSpellsGolden` (each spell on a fixed yard: pinned `WorldHash`, op list and effect list, Linux
  and Windows), `Magic.OfflineAndServerEmitTheSameOps`, `Magic.FillNeverEntombsAPlayer`,
  `Magic.RisingSpireLiftsItsCaster` (no air under the rider's column when the lift ends, at every voxel size),
  `Magic.ABlastOnFreshKnitIsAbsorbed`, `Magic.AnAbsorbedBlastAblatesThePlate`, `Magic.ConjuredPiecesNeverTakeASlot`,
  `Magic.ConjuredMatterIsGoneWithinItsLifetime`, `Magic.FloeThawsBackToWater`, `Magic.HeatMeltsTheIceBridge`,
  `Magic.NoSpellReachesAnEnemySpawn` (refused with `NearSpawn`; no fill cell in any spawn area),
  `Magic.RebarLanceSetsTwoPegs`.
- Registry: `KnitRegistry.ExpiryRemovesOnlyItsOwnMatter` (carve and re-knit by another cast),
  `KnitRegistry.ReplayGivesTheSameOps`, `KnitRegistry.PlayerCapExpiresTheOldestFirst`,
  `KnitRegistry.FillRunsNoLoadCheckAndExpiryRunsASiteCheck`, `KnitRegistry.ShapeExpiryOnlyWithoutOverlap`.
- Combos: `Combo.ShatterSpikeDeepensSiegeCannon` (at least 1.5x), `Combo.RimedIronIsCarvedByABlast`,
  `Combo.HeatMakesKnitBlastable`, `Combo.FrostTurnsPitchToIce`, `Combo.ShockwaveShattersTheFloeBridge`.
- Structure: `Neck.ExtraLoadNeverFailsNaturalTerrain`, `Neck.ExtraLoadReleasesAWeakFloor`,
  `Magic.ExpiryUnderABridgeReleasesIt`, `Magic.HeatUnderAFloeBridgeReleasesWhatItCarried`,
  `Neck.ExtraLoadWaitsOnTheStructureCredit`; the stage-1 arenas still release nothing.
- Players: `Player.ZonesAreEvaluatedInZoneOrderFromQuantisedFeet`, `Player.PushDecaysToZeroInBoundedTicks`,
  `Player.SlickGroundChangesOnlyAcceleration`, `Player.KnockbackStopsAtThePlayArea`,
  `Player.LiftedPlayerTakesNoFallDamageOnLanding`, `Player.ZoneEdgeAndCentreAreContinuous`,
  `Player.SlickGroundReadsTheSameVoxelAtASurface`, `Player.RootAndPinCannotChainPastTheGuard`,
  `Player.ARespawnedPlayerIgnoresRootPinAndImpulse`, `Player.ReleaseFreesARootedAlly`,
  `SessionFile.BuildHashSeesThePlayerRuleRevision` (revisions 2-5),
  `SessionFile.BuildHashSeesTheSpellAndTurningTables`.
- Spread: `Spread.PhaseGatedContentAdvancesWithNothingElseAwake` (the amendment below); `Spread.*` unchanged with
  Ignite and Douse as `MapShape` maps.
- Budgets: `CastBudget.SpellWorkIsEqualColdAndWarm`, `CastBudget.ScheduleFollowsCountsNotTimeOrCache` with spells.

**VortexNetTests:**
- `Wire.*` round trips and decoder refusals for the three op kinds, `ZoneStart`, `ZoneCancel`, `CastFx`, `Attune`, the
  new `PlayerInput`, owner block and rejections; trailing and missing bytes refused.
- `Movement.ZonesPredictedWithoutCorrection` (0), `Movement.LateZoneCorrectsOnceAndIsCounted`,
  `Movement.NoCorrectionAbove2cmOnFlatGround` still 0, `Movement.IceSlideNoCorrectionAbove2cm`,
  `Movement.ZoneSurvivesAReplayAcrossItsEnd`, `Movement.LateCastWhileChilledIsJudgedAsPredicted`,
  `Movement.SpireRiderNoCorrection` (G16's link), `Movement.BarricadeAcrossAWalkerCountsCorrectionsByCause`.
- `Magic.ClientsAgreeAfterEverySpell` (a drill with `--bot-spells`: `hashes equal`, 0 `Checks:` mismatches, entities
  agree), `Magic.JoinerGetsLiveZonesAndConjuredMatter`, `Magic.WindowedClientAgreesOverExpiries` (a client window of
  48 on arena250; matter expires outside and at the window edge; 0 `HashCheck` mismatches),
  `Magic.ShockwaveTruncatesLiveEnemyZones` (a live Pin and a live Lift end at the Shockwave's `T_z` on the server and
  on both clients, with 0 corrections), `Magic.CastRejectedOverTheCaps`, `Replication.PressureWakesAtMostEightBodies`.

## Headless flags and gates

- **`--spell-demo NAME`** (game, offline, its own yard east of the plaza, like `--probe`): one spell at a fixed target
  at frame 30; logs `Magic: <spell>: path P, front F, contact C, ops N, bytes B, work W, ms M, players hit H,
  released R` and the spell's check line, for example `Spire: rider on top at y = ...`,
  `Cannon: bore 2.0 m stone, 4.0 m rimed`, `Freeze: floe anchored, 0 islands`, `Rebar: ledge reached at 6.4 m`.
  `--spell-demo siege-cannon` also runs at G6's `--voxel-size 0.1` and `0.15` and must fell the yard's tower.
- **`--siege-matrix`**: all built spells in a stamped yard (a stone curtain wall with slits and a timber walkway, a gate,
  a 4 m plank walkway, a stone arch, a moat, a stair, a timber tower), at fixed frames from fixed positions. It
  requires every check line, the combo lines (`Combo: cannon depth X m (rimed Y m)`, `Combo: knit blasted after
  heat`, `Combo: bridge melted`), `Magic: N spells, 0 failed`, and after `--frames 2400`
  `Magic: all conjured matter gone`. `--camera siege-matrix` frames it; its screenshot is looked at. Refused in a
  session.
- **`--crush-demo`**: the `extraLoad` calibration yard.
- **`--stress-spells N`** (with fx-budget's `--stress-fx`): N bots' worth of spells in view; frame cost lines.
- **`vortex_server --bot-spells N`**: bots cast a fixed, scripted rotation (no randomness) through their attunement every
  N ticks; with the drill and the arena walk it must log `hashes equal`, 0 `Checks:` mismatches, `Entities: ... agree`,
  `Zones: emitted Z, late L, cancelled C` and a `Magic:` line. **`--check-spells`** (debug builds) compares the
  server's written world with its ops applied through `ApplyOp`.
- **`tools/session-check.sh`** gains a spell pair between its clients (an Updraft on the walker, a Barricade between
  them); its verdict counts `Client: correction` lines by cause (zone late, zone edge, terrain, other).
- **Gates** (proposed; the stage that builds them adds them to CLAUDE.md's table and to the game's `docs/verify.md`,
  numbered after rendezvous' G20): G21 `--siege-matrix` (exit 0, the lines above, the screenshot); G22
  `vortex_server --session=engine/tools/server/sessions/drill.session --drill=200 --clients=2 --bot-spells 30 --loss=0.05
  --delay-ms=40 --jitter-ms=30` (as G11, plus the `Zones:` and `Magic:` lines); G23 `--stress-spells 10 --after-fill`
  (CPU and GPU effects <= 4 ms). G16 and G17 run with spells from [mag-2] on.

## Stages

| ID | Stage | Depends on | Acceptance | Effort (e) | State |
|---|---|---|---|---|---|
| [mag-1] | **Siege core in sim, headless only** (no wire). Only what its four spells use: `SpellTable` + hash, `sim::CastSpell`, the Pierce operator (bore boxes, `kMaxBoreVoxels`), the Burst strip and seed plane, Plume's single-seed column, `CarveCapsule`, `MapMask` (with its axis order) and `MapShape` as sim ops, `TurningTable` (Knit, Unknit, Ablate), `knit_iron` and the `Conjured` flag, the ablative absorb rule, the `KnitRegistry` with mask and cheap expiries and site checks by what changed (`noCarve` and the minimal structure credit if SI has not built them), the conjured-piece rule, the spawn guard, the `ZoneSet` with Impulse and Lift, truncation and replay retention, `PlayerState` (`push`, status) and `StepPlayer` revision 2, the `UpdateCharacter` `vertical` parameter, server damage hooks. Spells: **Siege Cannon, Barricade Wave, Shockwave, Updraft** (breach, cover, knock off, climb). `--spell-demo`, `--siege-matrix` (4 rows) | [wp-6] (rules seam), [wp-7] (health, teams, charges, spawns), [wp-8] (the fight baseline) | Operator, op, registry, player and determinism tests above for what it builds; G1-G10 unchanged; `--spell-demo siege-cannon` at G6's voxel sizes; demo and matrix lines and screenshot; `Magic:` cost lines after fill | 6-9 d | Designed |
| [mag-2] | **First playable.** The one protocol bump (every value in "Wire values" reserved, the whole owner block included), `CastFx`, zone events with the lead and `ZoneCancel`, the join baseline, `PlayerInput::spell` and the Spell edge, the owner block, `Attune`, rejections, `buildHash` additions, `RunScheduled` on the server, caps. Offline through M2a's in-process server and in sessions. HUD: the spell bar, element panel, form crosshairs, statuses, refusals, the attune menu (Metal and Pressure only). `PresentSpell` basics. `--bot-spells`, `--check-spells`, session-check with spells and corrections by cause | [mag-1] | `Movement.ZonesPredictedWithoutCorrection` at 0; `Movement.BarricadeAcrossAWalkerCountsCorrectionsByCause` measured (decides on the Block zone); G11-G17 and G22 with `--bot-spells` (`hashes equal`, 0 check mismatches, tick p99 < 16 ms); G16's bound held; the owner's Felling-duel taste check with 4 spells and the two-PC run (owner) | 6-9 d | Designed |
| [mag-3] | **Frost, and the Shatter combo.** `ice`, `floe`, the rimed ids, `knit_iron_rimed`; maps Rime, Thaw, Freeze, FreezeWater, Shatter and the Shatter flood (Shockwave's Shatter becomes live); Chill, Root and Release zones, the control guard and `Fresh` (`kPlayerRuleRevision` 3); the full Plume operator with its `Front` rule (Permafrost Climb) and the Burst disc (Flash Freeze); `shapeChanged` per voxel if spread S1 has not built it; the attune menu gains Frost. Spells: **Shatter Spike, Flash Freeze, Permafrost Climb**. No protocol bump | [mag-2] | Frost, combo and guard tests and lines; `Combo.ShatterSpikeDeepensSiegeCannon`; drills agree | 5-7 d | Designed |
| [mag-4] | **Flow, climbing and the rest of Metal.** The Flow operator and its fine fixed-point arc trace; `slag`, the Slag and Set maps, the hazard scan (slag); Plume's bands and ledges with the band timing rule and the lead-tied hover; Rebar Lance's two sockets; the slick rule (`kPlayerRuleRevision` 4). Spells: **Molten Pour, Rising Spire, Rebar Lance, Glacier Slide**. No protocol bump | [mag-3] (ice for Glacier Slide, Root for Rebar Lance) | Their tests and demo lines (`Spire: rider on top`, `Rebar: ledge reached`); `Movement.SpireRiderNoCorrection` and `Movement.IceSlideNoCorrectionAbove2cm`; drills agree | 6-8 d | Designed |
| [mag-5] | **Thermal.** The pitch class in `SpreadTable`, `knit_iron_hot`, `iron_hot`, the Pitch, Heat and Cool maps, the hazard scan for pitch and hot metal, Burning. Spells: **Pitchfall, Wildfire, Meltdown Beam, Firestorm Sweep** | [spread-S1] (headless and scripts); [spread-S2] / [spread-S3] for play (offline practice and sessions both run the server path); rides S2's bump if they land together | spread.md's gates plus the Thermal lines; `Magic.HeatUnderAFloeBridgeReleasesWhatItCarried`; the ten-fires case with Thermal spells inside the spread credit | 4-6 d | Designed |
| [mag-6] | **Presentation and tuning.** `FxDirector` effects per element, rings, ghosts, crumble warnings, steam and frost mist (fog when built), the landing marker, team panel letters, audio hooks once audio exists; a 10-bot spell fight measured; lifetimes, strengths, charges and cooldowns after the owner's taste check | [fx-1], [fx-2] ([wp-8]) | G23 (`--stress-spells` inside 4 ms CPU / 4 ms GPU); fight frame within [wp-8]'s margin on the UHD 620; server p99 < 16.7 ms; the owner's windowed checks | 4-6 d + owner | Designed |
| [mag-7] | **Crushing Weight.** `extraLoad` in Part 4b's check, run as a site check on the structure credit; the Pin zone (`kPlayerRuleRevision` 5). Spell: **Crushing Weight**. No protocol bump. It can land before [mag-5] once [si-2] is done | [si-2] (the `NeckCheck` split), [mag-4] (Flow) | `--crush-demo`; `Neck.ExtraLoad*`; `--neck-blast`, `--tower-blast` and the castle case unchanged; drills agree | 2-3 d | Designed |
| later | Knit tiers from progression (the 3 reserved ids), M7 | M7 | - | - | Not started |

Commits (engine / game) and history files are added to each row as its stage lands.

## ROADMAP placement

Us, under the owner's standing rules; **the owner may veto**: [mag-1] and [mag-2] go into "Next" right after [wp-8]
and before [mech-1]. Spells are M2b's main verb and the game's fun first; they need M2a's health, teams, spawns,
charges and action slot, WP6's rules seam, and [wp-8]'s fight measurement as their baseline. This reorders "Next" from
item 8 on: [mech-1] takes the protocol number after [mag-2]'s, and its wire numbers follow the registry rule. The rest
follow [mag-2], not W1: [mag-3] (Frost) right after [mag-2], so the top-ranked Shatter combo is the next thing
playable; [mag-4] after [mag-3]; [mag-5] after [spread-S1] (headless) and [spread-S2] / [spread-S3] (play); [mag-7]
after [si-2] and [mag-4]; [mag-6] closes. Spread S4 (lockstep liquid) stays in "Later" and is needed by none of the 16
spells. M2b's "spell framework; conjured matter" is [mag-1], [mag-2] and [mag-4]; M5's "material changes and spreading
fire" is [mag-3], [mag-5] and spread.

## Contract changes (recorded in CLAUDE.md section 5, shred.md, spread.md and networking.md by the stage that builds them)

New rows for CLAUDE.md section 5 (with their pinning tests):
- **Spell ops.** `MapShape`, `MapMask` and `CarveCapsule` decide every voxel from (voxel, old id, op integers, an
  integer table); masks hold only voxels that change; a Pierce walk and its capsule agree. Wire format; values take the
  next free number when they land (notes/net.md section 2 is the registry). `Magic.ClipEqualsWhole`,
  `Pierce.WalkAndCapsuleAgree`.
- **Spell reach is the server's** (R1): operators run only on the server (and M2a's in-process offline server); work
  is counted from visits and distinct chunks read. `Propagate.WorkIsEqualColdAndWarm`.
- **Zones only with a lead; nothing else changes a player's step.** Every effect of another player's action on a
  player's movement is a zone announced at least the session lead ahead and evaluated inside `sim::StepPlayer` from
  quantised feet; damage never enters it; a knockback is cut at the play area. A cancel only truncates a zone from a
  tick at least the lead ahead; a client keeps a zone until its oldest replayable tick passes the zone's end, and
  every lookup uses the stepped tick. A root or pin is followed by the control guard. `kPlayerRuleRevision` 2-5.
  `Movement.ZonesPredictedWithoutCorrection`, `Movement.ZoneSurvivesAReplayAcrossItsEnd`,
  `Player.KnockbackStopsAtThePlayArea`, `Player.RootAndPinCannotChainPastTheGuard`.
- **One cast cooldown function.** `sim::CastCooldownFor(state, tuning)` gives the cooldown in `TakeCast`, in
  `Server::LateCast` and in its rewrite of kept states. `Movement.LateCastWhileChilledIsJudgedAsPredicted`.
- **The registry principle.** Conjured state that decides an op lives only on the server and is emitted as
  self-contained ops; a scheduling error can only change lifetimes for everyone, never split peers.
  `KnitRegistry.ReplayGivesTheSameOps`, `KnitRegistry.ExpiryRemovesOnlyItsOwnMatter`.
- **No load check after a knit fill**; any op that turns a collider into a non-collider (or removes an anchor) runs a
  site check over its changed chunks, decided per voxel; bores run the island search and load check in their own
  flood box; `extraLoad` is a server-only input used only by Crushing Weight, on the structure credit.
  `KnitRegistry.FillRunsNoLoadCheckAndExpiryRunsASiteCheck`, `Magic.HeatUnderAFloeBridgeReleasesWhatItCarried`,
  `Pierce.BoreStaysInsideItsFloodBox`, `Neck.ExtraLoadNeverFailsNaturalTerrain`.
- **Conjured pieces take no slot.** A piece holding a `Conjured` id never asks `TakePieceSlot`.
  `Magic.ConjuredPiecesNeverTakeASlot`.
- **The absorb rule, ablative.** A blast whose hit voxel is fresh conjured matter harder than its power emits no
  carve and one `MapShape(Ablate)` of 0.25 m at its hit. `Magic.ABlastOnFreshKnitIsAbsorbed`,
  `Magic.AnAbsorbedBlastAblatesThePlate`.
- **The spawn guard.** No spell's op or zone meets an enemy spawn area; no fill cell lies in any spawn area.
  `Magic.NoSpellReachesAnEnemySpawn`.
- **The shared trace stays shared.** The spell ray's `Film` skip lives in `World::Raycast` on the CPU, never in
  `trace.glsl`. `Trace.SingleChunkMatchesReferenceDDA`, `Raycast.ASpellRayPassesPitchAndHitsTheStoneBehind`.
- **`buildHash`** covers `TurningTableHash` and `SpellTableHash`. `SessionFile.BuildHashSeesTheSpellAndTurningTables`.

Elsewhere: shred.md R1 and R5 gain the spell rows (operators, `CastWorkOf` for spells); Part 4b gains `extraLoad`;
networking.md "Player movement" gains zones and the lead, "World operations" the three kinds; spread.md's kinds 7 and 9
become `MapShape` maps.

## Amendments to other designs (found while fitting this one; applied by the stage named)

1. **spread.md, a stall in the step clock** (before [spread-S1]). "Planning one logical step": a slot emits a step
   only if something is awake; otherwise "nothing is emitted and `s` does not move". Settled pools are awake only when
   `(s + kChunkPhase) % 10 == 0`. With only settled pools left, no chunk is awake at the next `s`, `s` never moves, and
   evaporation never comes. **Fix:** a slot with both credits >= 0 and an empty awake set still advances `s` and sends
   nothing; the next emitted part carries its absolute `s`. Safe: a silent advance happens only while nothing burns
   and no liquid is active, and those are the only rules that need consecutive phases. A credit wait still holds `s`.
   Test: `Spread.PhaseGatedContentAdvancesWithNothingElseAwake`.
2. **spread.md, sites** (before [spread-S3]): "turned a collider-solid voxel into air" becomes "into any non-collider"
   (air, a liquid, a film).
3. **spread.md, ops** (before [spread-S1]): `IgniteSphere` and `DouseSphere` become `MapShape` with maps 1 and 2 (two
   fewer op kinds, one code path); whichever of [mag-1] and [spread-S1] lands first builds `MapShape`. Its Kindle and
   Pour spells stay as drill and test casts (`SpellId` 16, 17); its Q key stays the crate's; its `PlayerInput::spell`
   is this design's byte.
4. **Op and wire numbering** (applied 2026-11-21 with the design review): spread.md designs kinds 7-9, shred.md Part 3
   plans `ShredSphere = 7` and `DetachFractured = 8`, mechanisms.md event kinds 37-38 and `PlayerButton_Use = 16`.
   They collide on paper. Rule: notes/net.md section 2 is the registry, and every kind, event, message, rejection and
   button takes the next free value when it **lands**. The one exception, kept to spare mechanisms a renumbering:
   `PlayerButton_Spell` is 32 (or an action-slot bit). Written into notes/net.md section 2 (the rule and the
   exception); mechanisms.md, spread.md and shred.md Part 3 now call their numbers provisional, taken from the
   registry when they land.
5. **setting.md section 9** (by [mag-2]): Knitting = Metal (conjured matter with a lifetime), Kindling = Thermal,
   Drawing = Pressure, Turning = Frost; the forms keep the owner's names.
6. **ROADMAP**: a "Direction" row for the owner's concept; M2b's "projectile, beam, area" reads "the four forms (Flow,
   Plume, Pierce, Burst)".

## Risks

- **Byte peaks** of masks in a 10-player brawl. Fill sizes are capped and bands spread a few masks over ticks.
  Measured in [mag-2] and [mag-6]; the fallback is smaller fill caps.
- **The feel of slick ground and launches**: windowed only; only the owner can confirm it.
- **Knit near necks could stall Fellings.** Ablation (each blast chips a plate) and stone-equal bonds bound it; the
  lifetimes, the counters and the crack warning are the further levers; the owner tunes them in the taste check.
- **Byte figures are computed, not measured.** Mask sizes depend on aim and overlap ("How the bytes are counted");
  the `Magic:` byte line in [mag-1] and `--bot-spells` in [mag-2] replace them. A fight with spells is expected to send
  fewer event bytes than today's blast-only soak, which already fills the 24 kB/s credit.
- **Zone-late corrections over the internet** (rendezvous' testers): the lead is a session setting; raise it to 30-36
  for an internet session if G16-style counts say so.
- **Ordering with spread S1** for `MapShape`, `shapeChanged` and the palette ids: whichever lands first builds or takes
  them; called out in both stage rows.

## Rejected alternatives

| Alternative | Why not |
|---|---|
| Per-spell code (16 handlers) | 16 rule sets to test and keep in sync; the owner's composition is two axes, so the code is two axes |
| Server-simulated bolts with catch-up and no rewind (proposal 2) | A second hit system with new events and present-position hits, against M2b's lag-compensated hits and today's rewound capture; more bugs. Revisit only if play asks for dodgeable spells |
| Pushes at the cast tick, and a carry teleport onto new matter (proposal 1) | One correction per push for every target; the teleport moves a player outside `StepPlayer` |
| A 12-tick zone lead (proposal 3) | Under the measured client lead on the soak link (10-16 ticks) and its loss tails: zone-late corrections would be common in G16 and G17 |
| Lifetimes as aging ids stepped by `SpreadStep` (proposal 1) | Ties Metal and Frost in a session to spread S2 / S3 and to the spread credit; a busy fire slows every wall's crumble |
| Reverse-table lapses over the cast's shape (proposal 2) | Overlapping casts revert early, and a voxel re-knit by another cast inside the old shape unknits with it |
| Generation ids for lifetimes (8 generations x 5 kinds) | 40-48 ids against 15, and fixed expiry beats give seconds of jitter |
| Conjured walls and spires as replicated bodies | 64 awake / 256 live caps, snapshot bytes, Jolt-only motion, a correction whenever a player stands on one; not supports for the load check |
| Liquid spells as lockstep spread liquid (spread S4) | Awake chunks every step on every peer and border overrides as the main cost, for matter that sets or burns within 1-4 s; one solve and one mask is far cheaper and exact |
| Clients recompute the flood from a small op | Reads many chunks: ring replay into one chunk, ChunkData ahead of the stream and window edges break (spread.md "Why per-chunk closure") |
| Reach applied in waves of ops over ticks | 10 casts/s x 6 waves x 3 roles wraps the 1 024-op ring in about 6 s (L12), and later waves meet a changed world; presentation shows the travel instead (bands only where growth is the gameplay) |
| Pierce as a per-voxel budget inside the op | A voxel's fate would depend on the voxels before it on the line: ChunkClip and ring replay into one chunk break. The server walks; the op is a plain capsule |
| Pierce bores as `ClearMask` | 1-2 KB for a long diagonal bore against 29 B for a capsule |
| A load check after every fill (proposal 1) | Surprise collapses under your own wall; against the spirit of "never after `FillBox`" |
| Rime halving bonds too (proposal 1) | Frost alone would fell towers, and every rime would need a site check. Hardness only; shear and tension x 0.5 is the first knob |
| A `Floats` graph flag for frozen water (proposal 3) | `Indestructible` already makes a graph anchor; no new graph rule |
| An `embers` coating with its own lapse (proposal 2) | A second burning mechanism beside spread's fire; pitch is one more fire class |
| A per-voxel HP side table for Metal | Spread's "state only in ids"; hardness, bore cost and bonds carry structural health |
| Acid as the 4th element | Slow dissolving needs per-step voxel stepping (fire-like ongoing cost) or damage-stage ids per material, for a weaker combo than Frost's |
| All 16 spells on a cycle key, or a loadout of 4 of 16 | Unreadable in a fast fight, or it hides the owner's two-axis grammar; attunement to 2 elements keeps it in the hands |
| A mana bar; per-spell cooldowns; per-spell charge costs | A second meter; 8 timers; costs players misjudge |
| Teams on voxels ("enemy metal") | A byte per voxel or ids per team; terrain has no owner. Statuses and damage respect teams, matter does not |
| Shatter as a plain `MapShape` sphere | Reaches rime behind a knit plate and deletes a 2 m ball of rimed iron in two casts; the flood reaches only exposed rime, 0.5 m deep (design review) |
| An absorb with no ablation | A one-voxel film made a whole face blast-proof (design review); a patch per blast keeps cover worth casting and Fellings possible |
| `ZoneCancel` of pending zones only | A zone pending at a Shockwave's `T_run` always starts at or before its `T_z`, so such a dispel never cancelled anything (design review); a truncation from `T_z` works and keeps the full lead |
| Siege Cannon costing 2 charges from [mag-2] | Per-spell costs were rejected for readability; cutting its direct hit to 30 removes its dominance as a damage spell; 2 charges stays the first knob after the taste check |
| A Block zone for every band from the start | One more zone kind and lead for a correction cause not yet measured; reserved in [mag-2], built only if the counted terrain corrections ask for it |

## Open questions (with the recommended answer; settled by us under the owner's rules, the owner may veto)

1. **Instant or travelling reach?** Instant, with the travel presented from `CastFx`; bands only for Rising Spire and
   Barricade Wave. Reason: ring safety (L12), no world change mid-spell, fewer bugs.
2. **Who computes the reach?** The server (R1), sent as ops. Clients run no operator; spread stays the only lockstep
   rule.
3. **The 4th element: Frost or Acid?** Frost (the owner's recommendation). Its effects are one-shot maps, its
   brittleness is a material, and it gives the combo loop and the ice bridges.
4. **Names of the forms in code and on the HUD?** Flow, Plume, Pierce, Burst: the owner's own alternates. One name set
   for code, documents and players, and it avoids "Flame + Frost" and "Liquid" against liquid materials.
5. **All 16 at hand, a loadout, or attunement?** Attunement to 2 elements per life (8 spells), chosen at respawn.
6. **How do forced movements reach players?** Zones with a 24-tick lead (a session setting), evaluated in
   `StepPlayer`; late zones replayed and counted.
7. **Projectiles or hitscan?** Hitscan with the rewound capture; Flow's instant arc trace; streaks are presentation.
   Shockwave's "deflect" becomes a dispel: it truncates live enemy Pin, Chill and Updraft zones within 5 m from its own
   zone tick, and its Release frees rooted or pinned allies. Instant effects cannot be dispelled.
8. **How are lifetimes kept?** The server's `KnitRegistry`, as exact `MapMask` expiries, one id per conjured kind.
9. **Does a knit fill run the load check?** No. Expiries, shatters and bores do; Crushing Weight on purpose.
10. **"Enemy" metal and stone?** Any metal and stone: voxels have no team.
11. **Charges and cooldowns?** One M2a charge per spell plus a per-form cooldown (Pierce 5 s, Burst 7 s, Flow and Plume
    9 s), shown from the owner block. First knob: Siege Cannon, Meltdown Beam and Crushing Weight cost 2.
12. **How brittle is rimed matter?** Hardness x 0.5, bonds unchanged: the owner's 2x exactly. If play wants
    chill-fellings, shear and tension x 0.5 next (with site checks after Rime).
13. **Crushing Weight's force?** An integer `extraLoad` of 600 kg/m^2 (e) in Part 4b's check at the zone tick, run on
    the structure credit, plus the Shatter flood on rimed matter and ice; calibrated by `--crush-demo`; [mag-7], after
    [si-2].
14. **Is fresh knit iron blast-proof?** Against a carve, yes: hardness 7.0 against power 6.0, and a blast that hits it
    is absorbed. But it wears: each absorbed blast chips a 0.25 m patch (Ablate), so a 1-voxel plate costs one blast
    per spot and a Barricade two per hole. Its bonds equal stone's. Counters: Siege Cannon, Thermal, Frost, blasts,
    the lifetime. Cover must hold to be fun, and must not be a wall that never yields.
15. **Progression ("levelling raises HP")?** Not in v1, for PvP fairness. Later (M7) thickness and lifetime first, then
    hardness tiers as table rows with the 3 reserved ids.
16. **Filling over players?** Never entomb: fills skip capsules (grown by 1 voxel) at their emission tick; Rising Spire
    lifts its rider with a zone; Frost roots instead of encasing in voxels.
17. **The Liquid form and spread S4?** A one-shot server Flow; no spell leaves standing liquid, so S4 stays in "Later".
18. **spread.md's Kindle, Pour and Q?** Kindle and Pour stay drill and test casts; Thermal covers ignition for players,
    Frost covers dousing; Q stays the crate key.
19. **Names?** The owner's 16 names are working names and get setting.md's exact-phrase originality check before
    release. "Napalm" is a real-world weapon's name: **Pitchfall** (recommended, it matches the `pitch` id) or "Cinder
    Pour", for the owner to pick. Ids (`knit_iron`, `slag`, `pitch`, `floe`, `*_rimed`) are working names: names are not
    wire format, ids are.
20. **Can generated water freeze?** Yes, into `floe` (indestructible, an anchor), back to water at its expiry, by Heat
    or by Shatter: moat bridges with counters.
21. **Does Molten Pour flow as a liquid first?** No molten liquid ids: the reach is the flow, it lands as slag (hot, a
    collider) and sets in 1 s. A liquid-then-cooling model needs level x heat ids and reads worse.
22. **The editor?** Conjured ids hidden from its palette, the registry paused (spread.md's precedent).
23. **Keys, colours, slick and launch feel?** As above (LMB blast, RMB spell, 1-4 form, R element; violet-grey, orange,
    white, cyan with one particle shape each; ice at 3 m/s^2). The owner confirms them windowed after [mag-2] and
    [mag-4].
24. **ROADMAP placement?** [mag-1] and [mag-2] after [wp-8], before [mech-1] ("ROADMAP placement"). It reorders "Next";
    the owner may veto. The later stages follow [mag-2] (Frost first), not W1.
25. **Summoning?** Not one of the 16. Calling stays M5's PvE track.
26. **Mechanisms?** Conjured matter blocks gates as terrain does: a closing gate stops at it (mechanisms' blocked rule);
    no new rule.
27. **Which element carries the direct damage?** Thermal, as the owner's concept says: Meltdown Beam 45 (+24 burn) is
    the hardest hit; Siege Cannon 30 and Rebar Lance 30 are structure and utility first. For the owner's taste check.
28. **How long is the control guard?** 1.5 s after a root or pin ends, in which neither applies again; chill and
    knockback still do. Long enough to step out of the next ring at run speed (about 6 m (c)); the first knob if
    Frost feels weak is 1.0 s.
29. **How big is a spawn area, and how long is respawn immunity?** 6 m round each team spawn point, full height,
    and 2 s (or until the player casts) of immunity to roots, pins and knockback. M2a's spawn points set the centres;
    W-stage maps keep spawn areas of stone so spread's fire cannot burn into them.
