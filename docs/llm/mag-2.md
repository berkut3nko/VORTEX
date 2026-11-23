# [mag-2] First playable spells (protocol 13): task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/magic|magic]]

| | |
|---|---|
| Stage | `[mag-2]`: the four `[mag-1]` spells (Siege Cannon, Barricade Wave, Shockwave, Updraft) in offline practice and in sessions: the one magic protocol bump, zone events with a lead, `CastFx`, the Spell edge, the owner block, `Attune`, the spell HUD |
| Design | [magic.md](../design/magic.md) "Stages" row `[mag-2]`; "Wire values (one protocol bump, in [mag-2])"; "`sim::ZoneSet`" (retention, late zones, joiners); "What can still cause a correction"; "Input and HUD"; "Where every effect lives"; "Follow-ups (`RunScheduled`)"; "Caps"; "Presentation"; "Tests" (VortexNetTests); "Headless flags and gates". ROADMAP "Next" item 7 |
| Code read at | engine `ea41341`, game `58d7b87` (before `[wp-4]`..`[mag-1]`: where a card builds on them it names the symbol and says "read the built interface") |
| Depends on | `[mag-1]` (all of `vortex.sim:cast`, `:zones`, `:spell`, `:knit`; ops 7-9 in `ApplyOp`), `[mech-1]` (protocol 12, `PlayerButton_Use = 16`), `[wp-7]` (protocol 11: health, teams, charges, roster, in-process offline server, `CastRejection` 4-5), `[wp-6]` (`vortex.sim:rules`) |
| Cards | 10 |
| Effort | 6-9 d (e) |
| Changes | wire: **yes**, `net::kProtocolVersion` 12 -> **13** in card 3 only. Movement rule: **no** (`kPlayerRuleRevision` stays 2: the Spell bit was refused by the decoder before, so no old input reaches the new path). `buildHash`: `SpellTableHash`, `TurningTableHash` (card 3). Gate: **G24** (the spell drill, card 7) |

## Before you start

- Committed in both repositories: `[wp-6]`, `[wp-7]`, `[wp-8]`, `[mech-1]`, `[mag-1]`. Check with
  `git log --oneline | grep -E '\[(wp-7|mech-1|mag-1)\]'` in the game root and in `engine/`. Missing: stop, `Status: blocked`.
- `grep -n 'kProtocolVersion =' engine/src/net/Messages.cppm` must print 12. Another value: stop and ask.
- What this stage builds on (verify each with `grep -n`; use the built name if it differs and list it under Deviations):
  - `engine/src/sim/Cast.cppm` (`[mag-1]`): `SpellCast`, `CastContext`, `CheckCapture`, `CastSpell`, `RunScheduled`,
    `CastSpellResult` (`groups`, `zonesStarted`, `zonesCancelled`, `damage`, `hitQ`, `face`), `SpellRefusal`,
    `SpawnAreasOf`, `CastWorkOf(const CastSpellResult&)`.
  - `engine/src/sim/Zones.cppm`: `ZoneKind`, `Zone`, `ZoneSet` (`Insert`, `Cancel`, `ForEachLive`, `PruneEndedBefore`,
    `All`), `kMaxZoneRecords` (64). `Simulation::Zones()`.
  - `engine/src/sim/Player.cppm`: `PlayerZones`, `StepPlayer(..., const PlayerZones&, dt)`, `PlayerState::push`,
    `status`, `slowQ`, `slowLeft`, `rootLeft`, `pinLeft`, `ccGuardLeft`, `PlayerButton_Use = 16`, `kPlayerButtonMask`
    (31), `TakeCast`, `PlayerStepResult`.
  - `engine/src/sim/Spell.cppm`: `SpellId`, `Form`, `Element`, `SpellTable`, `BuildSpellTable`, `SpellTableHash`,
    `kUpdraft` 6, `kSiegeCannon` 10, `kBarricadeWave` 12, `kShockwave` 14; `voxel::TurningTableHash`.
  - `engine/src/sim/WorldOp.cppm`: `WorldOpKind::MapShape` 7, `MapMask` 8, `CarveCapsule` 9, `WorldOp::map`, `shape`,
    `axis`, `OpWireBytes`; `voxel::ShapeQ`, `MaskRuns`, `MaskFromRuns`, `ShortestRunAxis` (`Volume.cppm`).
  - `engine/src/net/Wire.cppm`: `EventKind` (32-37 with `[mech-1]`'s `MechanismMove`), `Event` (a `std::variant`),
    `WriteEvent`, `ReadEvent(in, event, maskVoxelBudget)`, `EventBytes`, `WriteMaskRuns` / `ReadMaskRuns`.
  - `engine/src/net/Messages.cppm`: `MessageType` (1-20 plus `[wp-7]`'s), `CastRejection` (0-5 after `[wp-7]`),
    `kMaxCastRejection`, `Snapshot` (`owner`, `others`, `lastCastTick`, `castRejected`), `kSnapshotOwnerBytes`,
    `kSnapshotPlayerBytes`, `Welcome`, `BaselineEnd`.
  - `engine/src/net/Session.cppm`: `BuildHash`, `BuildHashOf(protocol, generatorGolden, materialTable, playerRule)`.
  - `engine/src/net/internal/Server.cpp`: `Server::Tick` order (Receive, streaming, `StepPlayers`, `RunCasts`,
    `StepPhysics`, lifetimes, `DrainDespawns`, `TrackBodies`, `Flush`, snapshots), `RunCasts`, `EmitOp`,
    `EmitObjectEvents`, `LateCast`, `StateAt`, the cast queue and cast credit.
  - `engine/src/net/internal/Client.cpp`: `Reconcile`, `HistoryEntry::tick`, `kHistoryTicks` (128), the event apply
    loop by opSeq.
  - Game: `src/Session.cpp` (the session client), `src/main.cpp` (never whole: grep `HudCanvas`, `Menu`, `m_Checks`),
    `[wp-7]`'s in-process offline server (grep `LocalServer\|InProcess` in `src/`).

## Decisions

Settled here (us; the owner may veto). Binding for every card.

1. **Values** (README "Values planned across the packs"; the registry in notes/net.md section 2 wins once written):
   `kProtocolVersion` 13. `WorldOpKind` 7 `MapShape`, 8 `MapMask`, 9 `CarveCapsule` go on the wire. `EventKind` 38
   `ZoneStart`, 39 `ZoneCancel`, 40 `CastFx` (magic.md's table says 37-39: it predates `[mech-1]`'s 37; README wins).
   `PlayerButton_Spell = 32`, `kPlayerButtonMask` 63. `MessageType::Attune` = the first free number after `[wp-7]`'s
   (read the enum; write it into the registry). `CastRejection`, after `[wp-7]`'s `NoCharge` 4 and `Dead` 5:
   `FormCooling` 6, `NotAttuned` 7, `TooMuchConjured` 8, `TooManyZones` 9, `TooMuchFire` 10 (reserved for spread; nothing
   emits it here), `NearSpawn` 11; `kMaxCastRejection` 11. Reserved in this bump (decoders accept, nothing emits):
   zone kinds 1-7, shapes 0-3, maps 1-15, `SpellId` 0-17. Gate **G24** (magic.md's "G22" proposal predates the plan).
2. **The lead** is 24 ticks: `Welcome::zoneLeadTicks` (u8). A `ZoneStart` made at `T_run` has `startTick = T_run + lead`;
   a `ZoneCancel` has `endTick = T_run + lead`.
3. **The Spell edge** is derived in `StepPlayer` exactly like the Cast edge (against `PlayerState::lastButtons`) and
   goes through the same `TakeCast` (one shared 15-tick cooldown; magic.md "Resources"). `PlayerStepResult` gains
   `bool spell`. A step with both edges takes Cast and drops Spell (fixed rule, both sides). No rule revision.
4. **`PlayerInput::spell`** (u8) is always on the wire (+1 B per input) and read only on the Spell edge. A value above
   17, or 16 / 17 outside a drill session, is refused by the server (`NotAttuned`), never by the decoder.
5. **Capture** (server, at the input's tick, `Server::Impl` beside the blast capture): (a) `[wp-7]` dead -> `Dead`;
   (b) `sim::CheckCapture(ctx, table, cast, inputTick)` -> its refusal mapped by `RejectionOf(SpellRefusal)`; (c)
   `[wp-7]` charges: none -> `NoCharge`; else spend one and the form cooldown (`casters[slot].formReadyTick[form] =
   inputTick + cooldown`) and queue the cast into **the same** cast queue as blasts (one queue, one cast credit,
   order by input tick then slot). No refunds (mag-1 decision 19).
6. **Spells not built** (the 12 others): the HUD greys them and the client never sets the Spell bit for them; a server
   that gets one answers `NotAttuned`. The attune menu offers Metal and Pressure only (the other two drawn as `LATER`).
7. **Run** (`T_run`, cast queue): `sim::CastSpell(simulation, table, cast, ctx, targets)`; then in this order: one
   `CastFx` event; every op of every `groups[i]` through `EmitOp` (as a blast's); each `zonesStarted` as `ZoneStart`
   (the server inserts it into its own `Simulation::Zones()` at the same time); each `zonesCancelled` as `ZoneCancel`
   (server applies `Cancel` too); object events through `EmitObjectEvents`; damage through `[wp-7]`'s damage entry
   with the cause it uses for casts. Cast work = `CastWorkOf(result)`, charged to the cast credit as a blast's.
8. **Every tick** after `RunCasts`: `RunScheduled(simulation, table, ctx, tick, targets)` (bands, expiries; their ops
   through `EmitOp`, bytes to the event credit, may go negative), then `ctx.structure.Run(...)` (site checks), its
   ops through `EmitOp`. Then `Zones().PruneEndedBefore(tick)` on the server.
9. **Joiners** get the live zones in `BaselineEnd`: it gains `u8 count` and `count` zone bodies (the `ZoneStart` body
   without the kind byte), the zones live or pending at the baseline's `asOfOpSeq`. No new message type.
10. **Owner block** (+17 B, `kSnapshotOwnerBytes` += 17): `push` i16 x 3, `status`, `slowQ`, `slowLeft`, `rootLeft`,
    `pinLeft`, `ccGuardLeft` (u8 each), the four form cooldowns as u8 in 4-tick units, rounded **up**, 255 at most
    (`min(255, (readyTick - tick + 3) / 4)`, 0 = ready), and one byte `attune`: bits 0-1 element A, 2-3 element B,
    bit 4 the active element (0 = A, 1 = B; the last Spell edge's, for the hands' colour). The client restores the
    seven player fields before every replay; it never reads the cooldown bytes for prediction (HUD only).
11. **Remote player record** +1 B (`kSnapshotPlayerBytes` += 1): bits 0-3 `status` & 15 (Rooted, Pinned, Chilled,
    Lifted), bits 4-5 the active element. **Roster** (`[wp-7]`'s player list message) +1 B per player: the `attune`
    byte's bits 0-3. If `[wp-7]` built no roster message, stop and ask (`Status: blocked`).
12. **`Attune`** (client -> server, events channel): `craftA u8, craftB u8`, 3 B. Accepted while dead or before the first
    spawn; `craftA != craftB`, both < 4, both built (Metal 0 or Pressure 2 in this stage); otherwise ignored and
    logged `Server: attune refused from slot N`. Applied at the next spawn into `ctx.casters[slot]`.
13. **Client zones**: a `ZoneStart` / `ZoneCancel` is applied to the client's `Simulation::Zones()` at its opSeq (with
    the other events). Retention: prune with `PruneEndedBefore(oldest)`, `oldest = max(nextTick - kHistoryTicks,
    lastComparedSnapshotTick)`. Every `StepPlayer` (predicted or replayed) passes `PlayerZones{&zones, &world,
    entry.tick, teamBit}`. A zone whose `startTick <= lastPredictedTick` when it arrives is inserted and forces a
    restore-and-replay from the last acknowledged state; log `Client: zone late by N ticks` (N = lastPredicted -
    startTick + 1).
14. **Corrections by cause** (client counters, printed once at exit; the existing `Client: correction` line is not
    changed, rule 22): cause of each correction, first match wins: `zone late` (a late zone was inserted since the
    last compared snapshot); `zone edge` (a live zone at the compared tick has its box within 2 voxels of the player
    reach); `terrain` (an op applied since the last compare touched a chunk of `PlayerReach(feet)`); `other`. Exit line:
    `Corrections: terrain T, zone late L, zone edge E, other O`.
15. **Offline** runs through `[wp-7]`'s in-process server: the same server code, so the same capture and run.
    `[mag-1]`'s headless scripts (`--spell-demo`, `--siege-matrix`) keep calling `CastSpell` on the `Simulation`.
16. **Bots** (`--bot-spells N`, `vortex_server` and `vortex_bot`): every N ticks a bot sets the Spell bit for one input
    with the next spell of the fixed rotation `[Barricade Wave, Updraft, Siege Cannon, Shockwave]`, aimed at the drill
    target it already uses; attunement Metal + Pressure. No randomness.
17. **`--check-spells`** (server; refused by release builds with exit 2): before each spell's ops are applied, the
    server copies (ReadChunk) every chunk of the ops' `OpChunkRange`s into a scratch `voxel::World` of the same config;
    after, it applies the same ops there through `ApplyOp` and compares `voxel::ChunkHash` per chunk. A difference:
    `Checks: server: spell N differs in chunk (x, y, z)` and exit 1.

## Skeleton

Card 1 lands every declaration below with stub bodies (`// [mag-2] card N fills this`) and every new test, written in
full and named `DISABLED_<Name>`. No new partition: everything extends existing files.

`engine/src/sim/Player.cppm`:

```cpp
PlayerButton_Spell = 32,                  ///< edge: RMB, the selected spell (magic.md "What a player carries")
inline constexpr uint8_t kPlayerButtonMask = 63;
// PlayerInput gains (part of operator== and SameControls):
uint8_t spell = 0;                        ///< SpellId, read on the Spell edge only
// PlayerStepResult gains:
bool spell = false;                       ///< a Spell edge was taken (TakeCast passed)
```

`engine/src/net/Wire.cppm`:

```cpp
enum class EventKind : uint8_t { /* 32-37 as built */ ZoneStart = 38, ZoneCancel = 39, CastFx = 40 };
/// ZoneStart: kind u8; zoneId u16, zone kind u8, teamMask u8, startTick u32, duration u16, strength i16 x 2; shape.
struct ZoneStartEvent { sim::Zone zone; friend bool operator==(const ZoneStartEvent&, const ZoneStartEvent&) = default; };
/// ZoneCancel: kind u8; zoneId u16, endTick u32. 7 B.
struct ZoneCancelEvent { uint16_t zoneId = 0; uint32_t endTick = 0; friend bool operator==(const ZoneCancelEvent&, const ZoneCancelEvent&) = default; };
/// CastFx: kind u8; slot u8, spell u8, originQ i32 x 3, hitQ i32 x 3, face u8, tick u32. 32 B. Presentation only.
struct CastFx {
    uint8_t slot = 0; uint8_t spell = 0; glm::ivec3 originQ{0}, hitQ{0}; uint8_t face = 0; uint32_t tick = 0;
    friend bool operator==(const CastFx&, const CastFx&) = default;
};
using Event = std::variant<sim::WorldOp, EntitySpawn, EntityDespawn, EntityRest, ObjectModel, EntityWake,
                           /* [mech-1]'s MechanismMove as built, */ ZoneStartEvent, ZoneCancelEvent, CastFx>;
/// A shape as MapShape writes it: kind u8, then sphere 12+2, capsule 24+2, box 24, cylinder 12+2+2 bytes.
void WriteShape(ByteWriter& out, const voxel::ShapeQ& shape);
bool ReadShape(ByteReader& in, voxel::ShapeQ& shape);   ///< false: kind > 3 or a limit of decision "Decoder limits"
inline constexpr uint16_t kMaxZoneDuration = 600;
inline constexpr int32_t kMaxCapsuleLengthQ = 8192;       ///< magic.md: 64 m at 0.125 m (512 voxels x 16 q)
inline constexpr uint16_t kMaxCapsuleRadiusQ = 1024;
```

`engine/src/net/Messages.cppm`:

```cpp
// MessageType: Attune = <first free after [wp-7]>,   ///< client -> server, events channel: the next life's two elements
inline constexpr uint16_t kProtocolVersion = 13;
enum class CastRejection : uint8_t { /* 0-5 as built */ FormCooling = 6, NotAttuned = 7, TooMuchConjured = 8,
                                     TooManyZones = 9, TooMuchFire = 10, NearSpawn = 11 };
inline constexpr uint8_t kMaxCastRejection = 11;
CastRejection RejectionOf(sim::SpellRefusal refusal);     ///< None..NearSpawn; NotBuilt -> NotAttuned; Missed, Outside as blasts
struct Attune { uint8_t craftA = 0, craftB = 2; };        ///< Element values
// Welcome gains: uint8_t zoneLeadTicks = 24;
// BaselineEnd gains: std::vector<sim::Zone> zones;      ///< live or pending at asOfOpSeq, at most 32
// Snapshot gains (owner block): std::array<uint8_t, 4> formCooldown4{}; uint8_t attune = 0;
// RemotePlayer gains: uint8_t statusBits = 0;           ///< decision 11
inline constexpr size_t kSnapshotOwnerBytes = /* as built */ + 17;
inline constexpr size_t kSnapshotPlayerBytes = /* as built */ + 1;
```

`engine/src/net/Session.cppm`:

```cpp
/// protocol, generator goldens, material table, player rule, spell table, turning table.
uint64_t BuildHashOf(uint32_t protocol, uint64_t generatorGolden, uint64_t materialTable, uint32_t playerRule,
                     uint64_t spellTable, uint64_t turningTable);
```

`engine/src/net/Client.cppm` (public, for tests and the exit line):

```cpp
struct CorrectionCauses { uint32_t terrain = 0, zoneLate = 0, zoneEdge = 0, other = 0; };
const CorrectionCauses& Corrections() const;
void SendAttune(uint8_t craftA, uint8_t craftB);
```

`engine/src/net/Server.cppm`: `ServerConfig` gains `bool spells = true; uint32_t botSpellsEvery = 0; bool checkSpells
= false;`; `ServerStats` gains `uint32_t spellCasts, spellsRefused, zonesEmitted, zonesCancelled, spellOps; uint64_t
spellBytes;`.

`engine/src/engine/Engine.cppm`: `void PresentSpell(const net::CastFx& fx, std::span<const sim::BlastResult> groups,
std::span<const sim::Zone> zones);` (card 8).

Tests (new files): `engine/tests/net/MagicWireTests.cpp` (`Wire.*` for the new values), `engine/tests/net/MagicNetTests.cpp`
(`Movement.*` with zones, `Magic.*` net). Helpers: anonymous namespace, never an engine type's name (`TestZone`, `SpellYard`).

## Card 1: Skeleton: the new values declared, stubs, tests DISABLED_

| | |
|---|---|
| Goal | Every interface of this stage compiles; every new test exists and is disabled |
| Depends on | `[mag-1]`, `[mech-1]` committed |
| Effort | 0.5 d (e) |
| Touches | sim, net, engine. Wire: declarations only (`kProtocolVersion` stays 12 until card 3) |

**Steps**
1. Add the Skeleton's declarations. `WriteEvent` / `ReadEvent` for the three new variant alternatives: stubs that write
   nothing and return false (`// [mag-2] card 3 fills this`); `std::visit` sites that must be exhaustive get a no-op
   branch (grep `std::visit` in `engine/src/net`).
2. `BuildHashOf` with six arguments; the four-argument version stays and forwards with 0, 0 until card 3.
3. Write every test of cards 2-7 in full, `DISABLED_`.

**Checks** 1. build, no warning. 2. `tools/ctest.sh` all pass. 3. `tools/doc-check.sh --build build` PASS.

**Done when** - [ ] builds; - [ ] `grep -c 'DISABLED_' engine/tests/net/Magic*.cpp` counts every test named in cards 2-7.

## Card 2: The three op kinds on the wire, with decoder limits

| | |
|---|---|
| Goal | `MapShape`, `MapMask`, `CarveCapsule` round-trip exactly; malformed ones are refused |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | net (Wire). Wire: yes (lands with card 3's bump; no session runs between) |

**Read first**: magic.md "Wire values" (the three op rows, "Decoder limits"); `engine/src/net/internal/Wire.cpp`
`WriteEvent` for `ClearMask` (the mask runs).

**Steps**
1. `WriteShape`: kind; sphere `a` i32 x 3, `radiusQ` u16; capsule `a`, `b`, `radiusQ`; box `a`, `b` (voxels); cylinder
   `a`, `radiusQ`, `halfHeightQ`. Little-endian as every field today.
2. `MapShape`: kind 7, map u8, shape. `CarveCapsule`: kind 9, `a`, `b`, `radiusQ`, power u16 (no shape kind byte: always
   a capsule). `MapMask`: kind 8, map u8, axis u8, origin i32 x 3, dims u16 x 3, then `MaskRuns(mask, axis)` as LEB128
   numbers (the `ClearMask` encoding with the run order of `axis`).
3. Decoder limits (each a `return false`): sphere / cylinder `radiusQ > kMaxCastRadiusQ`; capsule `radiusQ >
   kMaxCapsuleRadiusQ` or `|b - a|^2 > kMaxCapsuleLengthQ^2` (int64); box coordinates outside
   `+-kMaxSyncChunkCoordinate * 32`; `map == 0 || map > 15`; `axis > 2`; mask voxels charged against
   `maskVoxelBudget` before allocating (as `ReadMaskRuns`); `MaskFromRuns` false.
4. `EventBytes` agrees with `sim::OpWireBytes` for every op (`static_assert`-style test below).

**Tests** (`MagicWireTests.cpp`)
- `Wire.MagicOpsRoundTrip`: each kind, each shape, a 13 x 7 x 5 mask in each axis: decode(encode(op)) == op; sizes 17 / 29
  / 27 / 19 B for `MapShape` sphere / capsule / box / cylinder, 29 B `CarveCapsule`, 21 B + runs `MapMask`.
- `Wire.MagicOpsRefuseLimitsAndTrailingBytes`: map 0, map 16, axis 3, radius over, capsule 65 m, a missing byte: false.
- `Wire.OpWireBytesEqualsEventBytes`: for the ops above.
- `MapMask.AxisOrderGivesTheSameVoxels` (net side): one mask sent in axes 0, 1, 2 applies the same voxels.

**Checks** build; `tools/ctest.sh -R 'Wire\.|MapMask\.' --output-on-failure`; `tools/ctest.sh`; doc-check.

**Must not break** `Wire.MessagesRoundTripAndRefuseTrailingOrMissingBytes`; kinds 1-6 bytes unchanged.

## Card 3: The protocol bump: events, input, owner block, Attune, rejections, Welcome, baseline, buildHash

| | |
|---|---|
| Goal | Protocol 13 carries every value magic needs through `[mag-7]` |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | net (Wire, Messages, Session), sim (Player). **Wire: `kProtocolVersion` 12 -> 13 here.** buildHash: + spell, turning |

**Read first**: magic.md "Wire values" (whole table); notes/net.md section 2 (the registry format); `Messages.cpp`
`WriteSnapshot` / `ReadSnapshot`, `WriteInputs`, `WriteWelcome`, `WriteBaselineEnd`.

**Steps**
1. Events: `ZoneStart` (30 / 42 / 40 / 32 B for sphere / capsule / box / cylinder), `ZoneCancel` (7 B), `CastFx` (32 B).
   Decoder: zone kind 1-7, duration 1..`kMaxZoneDuration`, shape by `ReadShape`; `ZoneCancel` any values (the client
   checks `endTick >= startTick` when applying and ignores a bad one, logged).
2. `PlayerInput::spell` after `buttons` in every input; the decoder refuses `buttons & ~kPlayerButtonMask`.
3. `PlayerButton_Spell` and the Spell edge in `StepPlayer` (decision 3): after the Cast edge,
   `spellEdge = (buttons & Spell) && !(lastButtons & Spell) && !castTaken && TakeCast(state, tuning)`.
4. Owner block (decision 10) and remote record (decision 11) in `WriteSnapshot` / `ReadSnapshot`; the server fills them
   in card 4 (zeros until then). Roster byte (decision 11).
5. `Attune` message (decision 12) both ways; `Welcome::zoneLeadTicks`; `BaselineEnd::zones` (u8 count, at most 32).
6. `CastRejection` values, `ToString` words (`FORM COOLING`... the HUD words are card 9's), `RejectionOf(SpellRefusal)`
   with a `static_assert` per value it maps.
7. `kProtocolVersion = 13`; `BuildHash` passes `sim::SpellTableHash(BuildSpellTable(kDefaultVoxelSize, ids))` and
   `voxel::TurningTableHash(DefaultTurningTable())` (read the built names). The game's and the server's build hash
   are the same function: grep `BuildHash(` callers, change none of their arguments.
8. notes/net.md section 2: every value of decision 1, the event layouts with sizes, the owner block bytes.

**Tests**
- `Wire.ZoneEventsAndCastFxRoundTrip` (sizes above), `Wire.ZoneStartRefusesKind0And8AndDuration601`.
- `Wire.InputCarriesSpellAndRefusesUnknownButtons` (bit 64 refused).
- `Wire.SnapshotOwnerBlockCarriesMagicState` (every field at its max: push +-1500, cooldown 255, attune 0x1E).
- `Wire.AttuneRoundTrip`, `Wire.BaselineEndCarriesZones` (0 and 32 zones; 33 refused).
- `SessionFile.BuildHashSeesTheSpellAndTurningTables` (one changed field of each table changes the hash).
- `Player.SpellEdgeSharesTheCastCooldown` (sim): Spell at tick 10 taken, Spell at tick 12 refused by the cooldown;
  Cast and Spell on one tick: Cast taken, Spell not.

**Checks** build; `tools/ctest.sh -R 'Wire\.|SessionFile\.|Player\.'`; `tools/ctest.sh`; doc-check;
`VERIFY_SLOT=1 tools/verify.sh quick` (G11, G15: old binaries are not mixed: all built at 13).

**Must not break** "Sessions" (any byte change bumps once: this card only); `Movement.NoCorrectionAbove2cmOnFlatGround` 0.

## Card 4: The server casts spells: capture, run, schedule, zones, damage, Attune

| | |
|---|---|
| Goal | A session player's RMB casts its spell; peers receive ops, zones and `CastFx` in the opSeq stream |
| Depends on | card 3; `[wp-7]` charges and damage |
| Effort | 1 d (e) |
| Touches | net (Server). Wire: no new bytes |

**Read first**: `Server.cpp` `StepPlayers` (where a Cast edge is captured), `RunCasts`, `EmitOp`, `LateCast`;
magic.md "The cast pipeline", "Follow-ups", "Caps".

**Steps**
1. `Server::Impl` owns `sim::SpellTable table` (built at start from the region's voxel size) and `sim::CastContext ctx`
   (`spawns = SpawnAreasOf([wp-7] spawn points, world)`, walls from the region). Per slot `CasterState` from `Attune`.
2. Capture (decision 5) where a `PlayerStepResult::spell` comes back, also inside `LateCast` (the state at
   `in.tick`). The queued item is the existing cast queue's item with `ActionSlot::Spell` (`[wp-6]` reserved it) and
   `spell = input.spell`.
3. Run (decision 7) when the queue reaches it. `targets` = living players at this tick (slot, team, feet).
   `SpellRefusal` at run time -> `castRejected` in the owner block (as a missed blast).
4. Every tick after `RunCasts` (decision 8). Zone pruning on the server: `PruneEndedBefore(tick)`.
5. Caps (`maxZones` 32, `maxZonesPerPlayer` 4, conjured caps): `CastSpell` enforces them; the server only maps refusals.
6. Owner block: fill decision 10's bytes from `PlayerState` and `ctx.casters[slot]` at the snapshot's tick.
7. Log at exit: `Magic: server: casts N, refused R, ops O, bytes B, scheduled S, sites C` and `Zones: emitted Z, late L,
   cancelled C` (L is 0 on a server; the bot adds its own).

**Tests** (`MagicNetTests.cpp`, in-process server + clients as `Session.*` tests do)
- `Magic.ServerRunsASpellAndEmitsItsEvents`: one client attuned Metal + Pressure casts Barricade Wave: `CastFx`, then
  `MapMask` ops, in that order in the stream; client hashes equal.
- `Magic.CastRejectedOverTheCaps`: a fifth zone of one player -> `TooManyZones` in the owner block; a cast in an enemy
  spawn box -> `NearSpawn`; a form on cooldown -> `FormCooling`; charge not refunded.
- `Magic.AttuneOnlyWhileDeadOrBeforeSpawn`: an alive `Attune` is ignored; after death it applies at the respawn.

**Checks** build; `tools/ctest.sh -R 'Magic\.|Session\.|Replication\.'`; `tools/ctest.sh`; doc-check.

**Must not break** "Chunk sync" (every op through `EmitOp`); "Replicated bodies" (`TakePieceSlot`, conjured pieces take
no slot: `[mag-1]`'s rule); R5 cast credit (`CastBudget.*`).

## Card 5: The client: zones by opSeq, replay retention, late zones, corrections by cause, joiner zones

| | |
|---|---|
| Goal | A client predicts zones with no correction when they arrive inside their lead |
| Depends on | card 4 |
| Effort | 1 d (e) |
| Touches | net (Client). Movement rule: no (the rule is `[mag-1]`'s; the client feeds it) |

**Read first**: `Client.cpp` `Reconcile`, `HistoryEntry`, the event apply loop; magic.md "`sim::ZoneSet`" (retention,
late zones, joiners), "What can still cause a correction".

**Steps**
1. Apply `ZoneStartEvent` -> `Zones().Insert(zone)`; `ZoneCancelEvent` -> `Zones().Cancel(id, endTick)` (unknown id:
   ignored, counted). Over `kMaxZoneRecords`: drop the oldest ended record first, then refuse (counted).
2. Late zone (decision 13): mark `restoreAndReplay` for this frame's reconcile, `++causes.zoneLate` at the next
   correction, log the line.
3. Every `StepPlayer` call passes `PlayerZones` with the stepped tick (predicted: the new tick; replay: `entry.tick`).
   The team bit is the client's own team from `[wp-7]`'s roster.
4. Prune after each reconcile (decision 13's `oldest`).
5. Corrections by cause (decision 14); exit line `Corrections: terrain T, zone late L, zone edge E, other O`.
6. Joiner: `BaselineEnd::zones` inserted before the first predicted step.
7. `CastFx` is queued for presentation (card 8); a client without graphics drops it.

**Tests** (net, in-process, the link of `Movement.NoCorrectionAbove2cmOnFlatGround`)
- `Movement.ZonesPredictedWithoutCorrection`: 40 ms + 0-30 ms each way, no loss; 10 zones of every built kind
  (Impulse, Lift) across two clients, a truncation among them: **0 corrections**.
- `Movement.LateZoneCorrectsOnceAndIsCounted`: a zone with lead 0: exactly 1 correction, `zoneLate == 1`.
- `Movement.ZoneSurvivesAReplayAcrossItsEnd`: a correction forced right after a Lift ends: no second correction.
- `Magic.JoinerGetsLiveZones`: a client joins while a Lift is live: it is in its `ZoneSet` with equal fields.
- `Movement.NoCorrectionAbove2cmOnFlatGround` unchanged at 0.

**Checks** build; `tools/ctest.sh -R 'Movement\.|Magic\.|Client\.'`; `tools/ctest.sh`; doc-check;
`VERIFY_SLOT=1 tools/verify.sh quick`.

**Traps** Never look a zone up with `nextTick` inside a replay. Never drop a late zone (the server's state has it).

## Card 6: The Barricade measurement and Shockwave's truncation across clients

| | |
|---|---|
| Goal | The design's open measurement is made: how often a band corrects a walker |
| Depends on | card 5 |
| Effort | 0.5 d (e) |
| Touches | tests only |

**Tests**
- `Movement.BarricadeAcrossAWalkerCountsCorrectionsByCause`: client B walks +x at run speed; client A casts Barricade
  Wave across B's path 1.5 m ahead, 10 times. Asserts: hashes equal; every correction has a cause; prints
  `Barricade: corrections terrain T of 10 casts`. No bound asserted (it decides on the Block zone: report T).
- `Magic.ShockwaveTruncatesLiveEnemyZones`: three clients, A and B on team 1, C on team 2. A casts Updraft on B (a
  Lift, dispellable by enemies); C casts Shockwave over it: the Lift's duration ends at `T_run + lead` on the server
  and on every client, 0 corrections.
- `Magic.ClientsAgreeAfterEverySpell`: each of the four spells cast once near both clients: hashes equal, 0 `Checks:`
  mismatches, `Entities: ... agree`.

**Checks** `tools/ctest.sh -R 'Movement\.|Magic\.'`; `tools/ctest.sh`. Report the number T in "Numbers".

## Card 7: Bots, --check-spells, the spell drill as gate G24

| | |
|---|---|
| Goal | The drill casts every spell under loss and proves agreement |
| Depends on | card 6 |
| Effort | 1 d (e) |
| Touches | `engine/tools/server`, `engine/tools/bot`, `tools/verify.sh`, `tools/lib/verify-gates.sh`, fixtures |

**Steps**
1. `--bot-spells N` (decision 16) in `vortex_server`'s in-process bots and `vortex_bot`; `--check-spells` (decision 17).
2. Bot exit line: `Zones: bot N: applied A, late L, cancelled C` and `Corrections: ...` (decision 14).
3. Gate G24: `build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/drill.session --drill=200
   --clients=2 --bot-spells 30 --loss=0.05 --delay-ms=40 --jitter-ms=30`. Must: exit 0; `hashes equal`; every
   `Entities: client N: ... - agree`; no `Checks:` mismatch; a `Magic: server: casts N` line with N >= 20; a `Zones:
   emitted Z` line with Z >= 5. Tier `quick` (about 40 s). Judge `gate_G24` in `tools/lib/verify-gates.sh`, fixtures
   `tools/tests/fixtures/verify/G24.log` (pass) and a planted fault in `tools/selftest-verify.sh` (a `Checks:` line).
4. CLAUDE.md section 2 table row G24; docs/verify.md the flags and the lines (rule 22).

**Checks** build; `tools/ctest.sh -R 'Tools\.'`; the G24 command by hand (exit 0, lines present); `tools/doc-check.sh
--build build`; `VERIFY_SLOT=1 tools/verify.sh quick` (G24 included, PASS).

**Must not break** G11's lines (the drill without `--bot-spells` is unchanged); G13 `store` bound.

## Card 8: The game: keys, offline through the in-process server, PresentSpell basics

| | |
|---|---|
| Goal | RMB casts the selected spell offline and in a session, and the player sees it land |
| Depends on | card 5 |
| Effort | 1 d (e) |
| Touches | game (`src/main.cpp`, `src/Session.cpp`), engine (`Engine::PresentSpell`). Wire: no |

**Steps**
1. Input (game): RMB -> `PlayerButton_Spell`; keys 1-4 form, wheel cycles, R swaps the element; `input.spell =
   MakeSpell(form, active ? elementB : elementA)`. An unbuilt spell (decision 6): the bit is not set. The editor
   keeps its keys while F1 is open. E and Q stay `[mech-1]`'s.
2. Offline: the same input goes to `[wp-7]`'s in-process server (no separate path).
3. `Engine::PresentSpell` (presentation only, never in sim or hashes; magic.md "Presentation"): `CastFx` -> a streak
   of at most 32 particles from `originQ` to `hitQ`; per element an impact burst of at most 120 (Metal angular
   sparks, Pressure rings); ops through `PresentBlast` as today (a bore's carve shows its debris); a zone ring of at
   most 48 particles at `ZoneStart`'s shape; Lift streaks 20 per second per zone. Under the `FxDirector` caps if
   `[wp-8]` built it.
4. Headless: `--spell-session` script (game, `--connect` with the drill server): frame 200 attune, frames 300-600 one
   cast of each spell; log `Magic: client cast S, refused R` and exit 3 if any built spell was never applied.

**Checks** build; `VERIFY_SLOT=1 tools/verify.sh quick` (look at the screenshots); a headless `--spell-demo
siege-cannon` (`[mag-1]`'s lines unchanged).

**Owner** windowed: RMB feel, keys, the effects read (docs/owner-checks.md, card 10).

## Card 9: The spell HUD: bar, element panel, crosshairs, statuses, refusals, attune menu

| | |
|---|---|
| Goal | A player sees which spell is ready, which element is active and why a cast was refused |
| Depends on | card 8 |
| Effort | 1 d (e) |
| Touches | game HUD (`HudCanvas`, the 5 x 9 font; **no ImGui**, CLAUDE.md "HUD"). Wire: no |

**Read first**: magic.md "Input and HUD"; the game's `HudCanvas` use (grep `HudCanvas` in `src/`), `Menu`.

**Steps**
1. Spell bar: 4 slots of 84 x 48 px at 720p scaled by `TextScale`, bottom centre; key digit and form word (`FLOW`,
   `PLUME`, `PIERCE`, `BURST`); border in the active element's colour; cooldown fill from the owner block's
   `formCooldown4` (seconds = value * 4 / 60, one decimal); selected slot raised 6 px with a 2 px border; unbuilt spell:
   greyed. Under it the spell name in the element colour for 1.5 s after a change, then 50 %.
2. Element panel: `[R] METAL / pressure` (active bright); `KNIT n/6000` above it while above 0 (from `[mag-1]`'s
   registry offline; in a session: omitted, not in the owner block).
3. Crosshair per form: Pierce dot, Burst wide brackets, Plume caret, Flow ring.
4. Statuses beside `[wp-7]`'s health bar from `status`: `LIFTED` (others reserved).
5. Refusal words on `[wp-7]`'s "Cast not run" line: `PIERCE COOLING` (form word + COOLING), `NO CHARGE`, `NOT
   ATTUNED`, `TOO MUCH MATTER`, `TOO MANY ZONES`, `NEAR SPAWN`.
6. Attune menu on the respawn screen: four items, Metal and Pressure selectable, Frost and Thermal `LATER`; sends
   `Attune` on Ready.
7. Measure: the default run's `HUD: N quads` with a spell selected; record N (magic.md estimates about 110 more).

**Checks** build; `VERIFY_SLOT=1 tools/verify.sh quick` (G3's `HUD: N quads` > 0; look at the screenshot: bar, panel).

**Must not break** L6 (no full-screen blended quad); "HUD" contract (`HudQuad` layout untouched).

## Card 10: session-check with spells, owner checks, close [mag-2]

| | |
|---|---|
| Goal | The stage is verified, documented and ready for the owner's taste check |
| Depends on | cards 1-9 |
| Effort | 0.5-1 d (e) |

**Steps**
1. `tools/session-check.sh`: client A casts Updraft on the walker and a Barricade between them (`--spell-session`
   steps); its verdict prints the clients' `Corrections:` lines; the bound (at most 6 corrections, max error under
   0.5 m) is unchanged. Change the verdict, its fixture and `Tools.SessionCheckVerdict` together (rule 22).
2. docs/owner-checks.md: keys and RMB, bar legibility, colours, the Felling duel with 4 spells, the two-PC run.
3. magic.md stage row `[mag-2]` -> Built (commits); ROADMAP Now / Next and the test count; CLAUDE.md section 5
   "Sessions" (the new values) and "Movement" (zones via `PlayerZones` with the stepped tick); notes/net.md checked.
4. `grep -rn 'DISABLED_' engine/tests tests` shows no `[mag-2]` test.
5. `VERIFY_SLOT=1 tools/verify.sh full --ci`; `VERIFY_SLOT=1 tools/verify.sh soak` in the background.

**Done when** - [ ] full --ci PASS; - [ ] G16 within its bound with spells; - [ ] `Movement.ZonesPredictedWithoutCorrection`
0; - [ ] the Barricade number in the report; - [ ] owner checks listed.

## Values this stage lands

notes/net.md section 2: `kProtocolVersion` 13; `WorldOpKind` 7 `MapShape`, 8 `MapMask`, 9 `CarveCapsule` (on the wire);
`EventKind` 38 `ZoneStart`, 39 `ZoneCancel`, 40 `CastFx`; `MessageType::Attune` (next free after `[wp-7]`);
`PlayerButton_Spell` 32, mask 63; `CastRejection` 6-11; reserved: zone kinds 1-7, shapes 0-3, maps 1-15, `SpellId`
0-17; owner block +17 B; remote record +1 B; roster +1 B per player; `Welcome::zoneLeadTicks`; `BaselineEnd` zones.
`buildHash`: + `SpellTableHash`, `TurningTableHash`. Gate G24. `[mag-3]`, `[mag-4]`, `[mag-7]` need no bump; spread's
`SpreadStep` takes `WorldOpKind` 10.
