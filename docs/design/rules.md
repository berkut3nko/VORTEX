# Rules seam

[[../../../00 Docs Map|Docs map]] · [[networking|networking]] · [[magic|magic]] · [[../notes/net|net notes]]

| | |
|---|---|
| Status | Built ([wp-6], 2026-11-23; the owner's review pending) |
| Decided | Us, 2026-11-22 (the owner may veto): decisions 1-12 of the [wp-6] task pack |
| Next | [mag-1] (spells); [wp-7] fills the damage ([fight.md](fight.md) "Damage") |
| Open | none |
| History | - |
| Marks | code names from engine `ed409ee` plus [wp-6] |

What an action does - "player P used action slot S, aimed like this" - is decided in one GPU-free, network-free
place, the partition `vortex.sim:rules` (`engine/src/sim/Rules.cppm`, implementation
`engine/src/sim/internal/Rules.cpp`). The session server, the offline game and a client's presentation share it.

## Decision

- **The seam is a partition of `vortex.sim`**, `vortex.sim:rules`. It is GPU-free and network-free: Sim may not
  see Net (the configure check `vortex_assert_no_network`), so the seam knows players by slot number and nothing
  of peers, messages or snapshots.
- **Two calls, aim then resolve.** `sim::AimAction` turns an `ActionRequest` (who, which slot, eye, look
  direction) into an `ActionAim` (what the ray hit, the quantised sphere and power) or a refusal;
  `sim::ResolveAction` turns a request and its aim into an `ActionOutcome` (blasts with their operations, damage
  events, player impulses, effect cues) and applies the world part. They are separate because the server aims when
  its cast queue reaches the cast (networking.md "Time and ticks": the ray is cast on the world as the casts before
  it left it), and the queue's budgets sit between capture and run.
- **Cooldown and capture stay where they are.** The cooldown is part of the movement rule (`sim::TakeCast` inside
  `sim::StepPlayer`, predicted by the client); the seam does not judge it. `ActionRefusal::Cooldown` exists only so
  that net maps every refusal to `net::CastRejection` in one place (`net::RejectionOf`).
- **The action slot is an internal value, not a button.** `ActionSlot::Blast = 0` is what `PlayerButton_Cast`
  triggers. [wp-7] adds no button; [mag-2] adds `ActionSlot::Spell = 1` with `PlayerButton_Spell = 32`.
- **One generic server, rules compiled in.** No scripting and no rules data file: `net::Server` calls the seam;
  game modes ([wp-7]) are a session-file key and C++ in `sim` and in `ServerCasts.cpp` (a later
  `ServerFight.cpp`). Rules data (spell tables) arrives with [mag-1] as C++ tables hashed into `buildHash`
  ([mag-2]).

## The seam

The types (`vortex.sim:rules`):

| Name | What it is |
|---|---|
| `sim::TeamId`, `kNoTeam` | A team. 0 = none (the sandbox, offline). Teams are numbered from 1 ([wp-7]). |
| `sim::ActionSlot` | What a player uses. Internal, not a wire value: `PlayerButton_Cast` triggers `Blast` (0). |
| `sim::ActionRefusal` | Why an action did nothing: `None`, `Cooldown` (judged by `sim::TakeCast`, never by the seam), `Missed`, `Outside`. Values equal `net::CastRejection`'s. |
| `sim::ActionRequest` | One use of an action slot, as captured: player (slot + 1, 0 = none), slot, input tick, eye, look direction, spell ([mag-2]; 0 now). |
| `sim::AimRules` | How a ray becomes a blast: range, radii, power, which bodies take a cast, the region check ("Aim rules"). |
| `sim::ActionAim` | Where an action lands: hit, at a body, distance, point, voxel, normal, the quantised sphere and power. |
| `sim::DamageEvent` | Health taken from a player ([wp-7] fills). |
| `sim::PlayerImpulse` | A velocity change of a player, cm/s per axis ([mag-1] fills). |
| `sim::EffectKind`, `sim::EffectCue` | Something to show, in the integers of the world operation it belongs to (`Blast`: centre and radius of its sphere, 1/16 voxel). |
| `sim::ActionOutcome` | Everything an action did: refusal, blasts (the applied operations in order), damage, impulses, effects. |
| `sim::PlayerView` | A player as the rules see it: slot + 1, team, feet, alive ([wp-7] fills the list). |
| `sim::BlastDamageRule` | A blast's damage: `centre` at distance 0, falling linearly to 0 at `zeroMm` ([wp-7]). |
| `sim::RulesContext` | What `ResolveAction` may read besides the world: tuning, players, tick, blast damage. |

The calls: `sim::AimAction(simulation, request, rules, aim)` casts the request's ray on the simulation as it is
now and returns `None`, `Missed` or `Outside` (on `Outside` the aim still says `hit`, `atBody` and `sphere`);
`sim::ResolveAction(simulation, context, request, aim)` applies the action (a `Blast` slot runs
`sim::Blast(aim.sphere, aim.power)` and adds its effect cue); `sim::BlastCueOf(carve)` makes the cue of a
`CarveSphere` operation; `sim::CueCentreMetres` / `sim::CueRadiusMetres` turn a cue into metres.

The call sites:

- **Server**: `CaptureCast` captures an `ActionRequest` into the cast queue (`QueuedCast::request`); `RunCasts`
  dequeues it, `ResolveRay` calls `AimAction` with the session's rules, then `RunCast` calls `ResolveAction` and
  emits the applied operations. A server cast (`Server::QueueCast`, the drill) keeps the request's defaults.
- **Offline**: the left click -> `AimAction` with `OfflineAimRules()` -> `ResolveAction` ->
  `Engine::PresentOutcome`.
- **Client**: never resolves. `Client::Impl::Present` makes the cue of each `CarveSphere` it applied with
  `BlastCueOf` and presents it.

## Aim rules

| Field | Session (`SessionAimRules(tuning, region)`) | Offline (`OfflineAimRules()`) |
|---|---|---|
| `range` | `tuning.castRange` (60 m) | 80 m |
| `terrainRadius` | `tuning.castRadius` (1.2 m) | 1.2 m |
| `bodyRadius` | `tuning.castRadius` (1.2 m) | 0.6 m |
| `power` | `tuning.castPower` (6) | 6 |
| `bodyLayers` | Dynamic \| Rubble | 0 (every layer) |
| `replicatedBodiesOnly` | true (a body must carry `scene::NetId`) | false |
| `strictBodyFirst` | false: the body ray stops at the terrain hit, any hit on it counts | true: the body ray runs the whole range, a body takes the cast only when strictly nearer than the terrain |
| `region` | the session's region (`sim::CastFitsRegion` decides `Outside`) | none |

Both reproduce what the code did before the seam: the server's ray and the offline click's. The sphere is
quantised from the hit point (`voxel::QuantizeSphere`, fractional voxels), the power with `voxel::QuantizePower`.

## Outcome

- `blasts`: one `sim::BlastResult` per blast, its applied operations in order; the server emits them (`EmitOp`),
  offline they are presented.
- `damage`: from `RulesContext::players` and `RulesContext::blastDamage` ([wp-7], fight.md "Damage"): every living enemy of the actor in reach, span order; empty when `blastDamage.centre` is 0 (the offline click, a sandbox server, a round not `Live`).
- `impulses`: empty until [mag-1] (the push, `kPlayerRuleRevision` 2).
- `effects`: one `EffectKind::Blast` cue per blast. A client never resolves; its presentation takes the same cue
  from the operation it applied (`sim::BlastCueOf`).
- Presentation: `Engine::PresentOutcome` calls `Engine::PresentBlast` for each blast in order (cues of other kinds
  get their presentation with the stage that adds them). `Engine::Explode` stays in the engine API.

## Server shape

- One generic `net::Server`, rules compiled in (see "Decision").
- `Server::Impl` lives in an internal partition `vortex.net:server_impl` (`engine/src/net/internal/ServerImpl.cppm`:
  `module vortex.net:server_impl;`, which Clang and CMake build), its member functions declared there and defined in
  six implementation units by concern; `Client::Impl` the same in `vortex.net:client_impl` (five units). Nothing
  outside `vortex.net` sees `Impl`. The types the units share (`Peer`, `QueuedCast`, `HistoryEntry`, ...) are in
  `vortex::net::detail` in the partition (constants `inline constexpr`, functions `inline`); one-line accessors
  (`Tick`, `LastOpSeq`, `Dt`, `SinceWelcome`, ...) stay defined inside the struct.

## File map

Server (`engine/src/net/internal/`):

| File | Member functions of `Server::Impl` |
|---|---|
| `Server.cpp` | constructor, destructor, `Emit`, `EmitOp`, `CheckOp`, `Fail`, `Send`, `SendBatch`, `Pump`, `Receive`, `ReceiveFailed`, `DropPeer`, `Flush`, `TrimBacklog`, `CheckStore`, `RefreshHashes`, `CachedHash`, `ColliderFocus`; and every `Server::` public method (`Server::Server` to the end of the file) |
| `ServerPeers.cpp` | `OnHello`, `Refuse`, `DropPlayer`, `PlayersBySlot`, `PlayerFocus`, `PeerOfSlot`, `JoinedCount`, `OnInputs`, `StepPlayers`, `SendSnapshots`, `QueueBaseline`, `MakeBaselineEntity` |
| `ServerCasts.cpp` | `CaptureCast`, `RejectCast`, `Queue`, `ResolveRay`, `LateCast`, `RunCast`, `RunCasts` |
| `ServerSync.cpp` | `QueueChunk`, `ChunkDistance`, `SortChunks`, `PopChunk`, `PruneQueue`, `TerrainQueued`, `TerrainChunkGone`, `FinishTerrainStep`, `OnWindowUpdate`, `OnTerrainWindow`, `OnChunkRequest`, `NextBulk`, `PumpBulk` |
| `ServerChecks.cpp` | `OnEntityListRequest`, `EntityValues`, `SendHashCheck`, `SendChecks`, `OnModelRequest` |
| `ServerEntities.cpp` | `OnNetIdDestroyed`, `DrainDespawns`, `DescribeObject`, `EmitObjectSpawn`, `EmitObjectEvents`, `TrackBodies`, `EntityOf`, `SpawnCrateAt`, `SpawnObjectAt`, `PlayerCrate`, `AwakeSamples` |

Client (`engine/src/net/internal/`):

| File | Member functions of `Client::Impl` |
|---|---|
| `Client.cpp` | constructor, destructor, `Generator`, `Fail`, `Leave`, `OnWelcome`, `Receive`, `ApplyPending`; every `Client::` public method |
| `ClientPrediction.cpp` | `Restore`, `ReachCurrent`, `NotCurrentNear`, `PredictTick`, `SendInputs`, `StartPrediction`, `Reconcile`, `LogCorrection`, `OnSnapshot`, `TryReconcile` |
| `ClientSync.cpp` | `SendWindow`, `StartTerrainMode`, `CheckWindow`, `Present`, `Apply`, `Hold`, `ProcessBulk`, `SyncHousekeeping`, `NoteSynced`, `Synced` |
| `ClientEntities.cpp` | `BodyOf`, `MovingLayer`, `Tombstone`, `Untombstone`, `MakeCopy`, `MakeRubble`, `ApplyEntity`, `ShowRemovedVoxels`, `ShowVoxelsInSphere`, `TakeBodySamples`, `TakeReadySamples`, `MoveCopies`, `OnBaselineEntity` |
| `ClientChecks.cpp` | `AddCheckTime`, `OnHashCheck`, `CheckDeferred`, `OnMismatches`, `OwnEntityCheck`, `OnEntityCheck`, `AskModel`, `RemoveCopy`, `OnEntityList` |

## Stages

| ID | Stage | State |
|---|---|---|
| [wp-6] | rules seam and net split | built (review pending) |

## Open questions

None open. Revisit "One generic server, rules compiled in" ("Decision": rules as data) when [mag-1] lands its tables.
