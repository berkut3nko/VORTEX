# Shred history: Part 5, impact destruction as designed on 2026-11-12

Moved verbatim from `docs/design/shred.md` lines 1398-1440 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

Never built. Superseded by the structural integrity plan's SI-2 (`docs/design/shred.md`), which keeps its
integer energy, total order, server-only impacts and "no crush from contacts" and drops the rest (see "Dropped
or simplified, with the reason" there).

---

## Part 5 - Impact destruction by estimated weight (stage 6)

- **Contacts become usable**: `ContactEvent` (`physics/PhysicsWorld.cppm:101-107`) gains `invMassA/B` (0 for static
  / terrain, read in the listener while bodies are locked, `PhysicsWorld.cpp:207-208`) and the compound child index
  of each side. **Drain every tick** inside `Simulation::StepPhysics`: today nobody calls `TakeContactEvents`, so the
  16 384-event queue fills and stays full (notes/physics.md:160-161).
- **Child -> material**: a `ColliderMaterials` component filled in `CreateBody` from `BuildColliderBoxes(.., false)`
  (`Scene.cpp:42-47`).
- **Ownership, not existence, decides who may cause an impact.** Offline, client rubble lives in the same
  `Simulation` and `PhysicsWorld` (`sim::PresentBlast` creates it in `simulation.Scene()`, `Blast.cpp:225-234`) and
  every piece is an entity (`replicatedPieceVoxels = 0`, `Scene.cppm:100-109`); a 24-511-voxel piece (up to about
  1 m^3, 2.6 t of stone at 0.125 m) falling 5 m carries about 127 kJ, far over `Emin`. So bodies created as rubble
  carry a `clientRubble` bit in their Jolt user data (`VoxelObjectDesc::clientRubble`, planned in networking.md:227;
  `ContactEvent::userDataA/B`, `PhysicsWorld.cppm:103`). `sim::Impact` ignores every contact whose dynamic side is
  client rubble or below the replicated threshold of the session's voxel size, **offline included**. Offline
  allocates `NetId`s as a server does (the offline simulation is the server), so the sort key below exists there too.
- **`sim::Impact`** (server / offline, in the cast phase of the next tick): per event, energy `E = 0.5 mu v_n^2`
  (mu = reduced mass; `1 / invA` against static), early reject under `Emin` (e: 500 J). Then quantise once (R3):
  `ImpactQ{pointQ, normal int8[3], energyQ (J/4), netA, netB, matA, matB, terrainChunk, tick}`. **Then** deduplicate
  per `(netA, netB, terrainChunk)` (keep the highest `energyQ`, ties by the rest of the key) and sort by the full
  integer key `(netA, netB, pointQ.x, pointQ.y, pointQ.z, energyQ desc, matA, matB)`. `PhysicsWorld` today keeps one
  event per Jolt `BodyID` pair (`PhysicsWorld.cpp:251-258`), and terrain is many per-chunk static colliders with no
  netId, so one body landing across two terrain chunks gives several events with the same `(netA, 0)`; without the
  full key, ties fall back to Jolt `BodyID` order, which depends on when collider cooks finished.
  - Terrain side: crushed volume `V = eta * E / breakEnergy[ground]` (eta 0.3, e); radius by integer cube root;
    power = the struck material's `hardnessQ`; emit a `Carve` (later `ShredSphere`) half a radius into the surface
    and mark it dirty for the structural solver, so a falling tower cracks the wall it lands on.
  - Body side (server bodies only, needs networking stage 6): dent with a small `Damage` below a shatter threshold,
    `Fracture` above it; emits `ObjectModel` for the survivor and `EntitySpawn` / `EntityDespawn` for pieces.
  - Calibration (e): 1 m^3 of stone (2600 kg) dropped 5 m = 127 kJ -> a 0.3 m crater and the block shatters; a
    347 kg crate at 5 m/s = 4.3 kJ -> a dent in dirt, no shatter.
- **Cooldowns and budgets** (tick-counted state, R5): one fracture per entity per 15 ticks, sub-threshold hits add
  decaying fatigue, new fragments are immune for 30 ticks; per tick at most 4 impact ops and 2 shatters, taken in
  the sorted order; new bodies share the collapse budget (`serverBodiesPerTick`, 64 awake / 256 live). Over budget
  an impact may only dent; queued impacts older than 30 ticks are dropped and counted (R7).
- Before networking stage 6 there are no server bodies: impacts act on terrain only, from authoritative offline
  bodies (crates, `Damage` pieces of at least the replicated threshold).
- **Crush damage to players is not a contact.** Players are `JPH::CharacterVirtual` (`PhysicsWorld.cpp:21, 311,
  1038`), which never reaches the body `ContactListener` (`PhysicsWorld.cpp:213`), and on a client moving pieces and
  all local rubble never block the predicted player (networking.md:213-214). So crush is a separate server check
  each tick: a sweep of every awake server body (`netId != 0`) against the player capsules, damage from the
  quantised relative speed and mass, sent as a damage event (M2 game logic). `netId 0` rubble never damages.
