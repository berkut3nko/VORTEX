# [mech-1] Door and gate: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/mechanisms|mechanisms]]

| | |
|---|---|
| Stage | `[mech-1]` door and gate: E opens a hinged door, a lever raises and drops a portcullis, offline and in a session; doors wear down hit by hit; the crate moves to Q; E picks up and drops a small object, LMB throws it (added 2026-11-24) |
| Design | [mechanisms.md](../design/mechanisms.md): "Interaction: the use key", "One use key: E" (added 2026-11-24; its rope actions are `[mech-2]`), "Mechanisms" (all of it except winch / drawbridge / button rows), "Destruction interplay" "The damage rule (stage 1)" and "What happens to a damaged mechanism", "Budgets and costs", "Tests", "Stages" row 1. Ropes ("Ropes with physics") are `[mech-2]`: not here |
| Code read at | engine `bafca16`, game `0e27ea7` (before `[wp-4]`..`[wp-8]`: names below are checked against this code; where a later stage moved a function, find it with `grep -rn`) |
| Depends on | `[wp-7]` (protocol 11, the owner block's fight fields, the rules seam of `[wp-6]`), `[wp-8]` (the fight baseline the yard is measured in) |
| Cards | 11 (M-a offline: cards 1-7; M-b session: cards 8-11) |
| Effort | 12-13 d (e): M-a 7, M-b 5-6 (holding about 3 d of it) |
| Changes | `net::kProtocolVersion` 11 -> **12** (card 8); `PlayerButton_Use = 16`, `kPlayerButtonMask` 15 -> 31 (card 6); `EntityKind::Mechanism = 3`, `EventKind::MechanismMove = 37`, owner block +5 B (card 8). Holding (card 8, the same bump): `EventKind::EntityHold = 41`, `EventKind::EntityRelease = 42` (38-40 are `[mag-2]`'s in the plan), `ObjectSpawn` flag `kObjectHeld = 2` (+7 B), `RemotePlayer::holdPitch` (+1 B: `kSnapshotPlayerBytes` 17 -> 18), `sim::HoldEnd` 1-6. Not wire: `physics::Layer::Held = 6` (card 4), `sim::UseTargetKind`. `sim::kPlayerRuleRevision`: **unchanged** (mechanisms are posed before `StepPlayer`, held objects after it, never inside it). `buildHash`: only through `kProtocolVersion`. No new gate |

## State and the session part in brief (2026-11-24)

**Built (cards 1-7, the offline part):** engine `af50590` .. `21016b4`, game up to `434799a`. Tests and
`tools/verify.sh quick` pass on the Windows PC; the owner checked doors and the lever in a window. Where the build
differs from this pack:
- `UseTarget::kind` is the last field (after `distance`), so `{entity, info, distance}` initialisers stay valid.
- A cast that hits a mechanism becomes a `MechanismBlast` in the rules seam (`sim::ResolveAction`, `ActionAim::body`),
  not in the game's click handler: the server's cast path (card 9) gets it without new code.
- `DamageMechanism` and `MechanismBlast` live in `Blast.cpp` and share its piece handling (`TakeDamagePieces`).
- The yard door's hinge column is wood, not iron: iron (hardness 8) never breaks to the cast power 6. Its hinge box is
  the whole column, so `--mechanism-blast` casts along the column (door freed after 7 casts, lever after 1).
- `--camera yard` looks down from above the gate wall: the hut stands behind the wall, so eye height cannot see its door.

**Not built (cards 8-11, the session part).** What it does, in order:
1. **Card 8, the wire (protocol 11 -> 12, one bump for everything below).** A mechanism is a new entity kind
   (`EntityKind::Mechanism = 3`) that a client spawns from its whole `MechanismDesc` and segment ring, so a late
   joiner derives every pose itself. A use that the server accepts becomes one `MechanismMove` event (16 B: the
   NetId and the new segment) per moved mechanism. The owner block gains the last use's result and tick (+5 B), so a
   refused E shows "Locked", "Busy" or "Out of reach". Holding adds `EntityHold` (41, 12 B) and `EntityRelease`
   (42, 44 B), the `kObjectHeld` spawn flag for late joiners, and a 1-byte `holdPitch` per remote player (17 -> 18
   B). notes/net.md section 2 lists every value.
2. **Card 9, the server.** The server reads the Use edge at the input's tick (stale after 30 ticks: `Stale`), runs
   `FindUseTarget` with the server's hooks (NetId tie keys, replicated bodies only) and either `ApplyUse` (doors,
   levers) or `BeginHold` / `EndHold` (crates). The lead (4-20 ticks) is chosen per use from what the server measures
   of its clients, so a door starts to move when every client can show it from its start. Mechanisms are never
   deleted by the entity byte budget and never count as awake pieces. Every tick after the players, the server
   poses held bodies (`StepHeld`), drops them when pinned, when the holder dies or leaves, or when a blast hits them,
   and sends the releases. A session file opts in with `mechanisms = 1` (`playground.session` does; the drill and
   arena files do not, so G11-G19 keep their numbers).
3. **Card 10, the client.** Mechanisms are kept apart from body copies: the client poses them from the segment ring
   at its predicted tick, so a predicting player walks through a door that opens and collides with a gate exactly
   where the server has it (no corrections). It predicts its own pick-up and drop (and holds back further E and LMB
   presses until the server answers), poses other players' held objects from their remote records, and applies a
   release when its drawn copy of the holder reaches that tick. Entity checks and lists include mechanisms and the
   held state, so a wrong copy is repaired by the existing path.
4. **Card 11, session play and close.** `vortex_bot --bot-use` and `--bot-hold` exercise doors, the lever and
   crates in drills (hashes and entities must agree), `tools/session-check.sh` keeps passing, and the owner runs the
   yard with a friend over the LAN (owner checks). ROADMAP, CLAUDE.md and the docs get the stage's final state.

## Before you start

- Committed, in both repositories: `[wp-7]` and `[wp-8]` (`git log --oneline | grep '\[wp-7\]'` and `'\[wp-8\]'` in
  `engine/` and in the game root). Read `kProtocolVersion` in `engine/src/net/Messages.cppm`: it must be 11. If it
  is not, stop (`Status: blocked`).
- What exists (names verified at `bafca16`):
  - `scene::Scene` (`engine/src/scene/Scene.cppm`): `CreateVoxelObject(VoxelObjectDesc)`, `Damage(Entity, dvec3,
    float)` returning `DamageResult{removed, pieces, fragments, destroyed, toRubble, keptOffset, keptMotion}`,
    `SetTransform`, `QueryBodies`, `TryGet<T>`, `Registry()`. Components in `engine/src/scene/Components.cppm`:
    `Transform`, `VoxelBody`, `RigidBody{body, motion}`, `NetId{value, piece, bornTick, despawn}`, `DespawnReason`.
  - `physics::PhysicsWorld` (`engine/src/physics/PhysicsWorld.cppm`): `SetTransform`, `SetVelocity`,
    `SetMotionType`, `SetLayer`, `GetLayer`, `MoveKinematic(body, pos, rot, dt)`, `GetBounds`, `SetCharacterState`,
    `Activate`; `Layer::{Static, Dynamic, Debris, Character, Rubble, Bounds}`; `LayerBit`.
  - `sim` (`engine/src/sim/`): `PlayerButton` (Jump 1, Run 2, Cast 4, Crate 8), `kPlayerButtonMask` 15 (masked in
    `PressButtons`, `Player.cpp`), `PlayerReach`, `PlayerBounds`, `LookDirection`, `StepPlayer`; `Blast` (object loop
    "3. Objects caught in the blast" in `internal/Blast.cpp`; push loop "4."); `IsAwake`, `CountPieces`,
    `TakePieceSlot`, `Despawn`, `Replicate` (`Replication.cppm`); `RegionTemplate{ops, lights, objects, spawn,
    anchor}`, `PlaygroundTemplate`, `ApplyTemplateOps`, `CreateTemplateObjects`, `PlaygroundScale` (`Region.cppm`).
    `[mag-1]` is not built yet when this stage starts (it comes after): do not wait for `BlastOptions`.
  - Server (`engine/src/net/internal/Server.cpp`, split by `[wp-6]`: `grep -rn 'void StepPlayers\|PlayerCrate\|
    LateCast\|EmitObjectEvents\|TrackBodies\|AwakeSamples\|DescribeObject' engine/src/net`): `Player::StateAt`,
    `kStateHistory` 32, `margin`, `Server::Tick()` order Receive, UpdateStreaming, StepPlayers, RunCasts,
    StepPhysics, DrainDespawns, TrackBodies, Flush, SendSnapshots; `entityCredit`, `stats.modelsToRubble`.
  - Client (`engine/src/net/internal/Client.cpp`, also split by `[wp-6]`): `copies` (NetId -> Copy), `MakeCopy`,
    `MoveCopies`, `Hold`, `FreshColliders()`, `PredictTick`, `Reconcile` (replay loop), the resume replay in
    `Client::StepPlayer`, `Client::RunTick` (`StepPlayer` then `simulation.StepPhysics()`), the EntityCheck count
    (`EntityModelTerm(netId, copy.modelHash)`).
  - Game: `src/main.cpp` `OnFixedUpdate` (offline `sim::StepPlayer`), `OnUpdate` (`Key::E` -> `SpawnCrate`, left
    click -> `engine.Explode`), `DrawHud` (tool line `"Tool: Blast [LMB]   Crate [E]"`), `BuildPlayground`,
    `--camera` parsing in `main()`; `src/Session.cpp` (E latch `PlayerButton_Crate`, the scripted crate latch, the
    session tool line). `[wp-7]` may have changed these lines (HUD, offline practice): grep for them.
- CMake: engine modules and tests are globbed (`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` in `engine/CMakeLists.txt`
  and `engine/tests/CMakeLists.txt`): a new `.cppm` / `.cpp` / test file needs no CMake line. A new partition still
  needs its `export import :name;` line in its module's primary interface (`engine/src/sim/Sim.cppm`).

## Decisions

Settled by us for this pack (the owner may veto). Where the design or the skeleton handed to this pack differ from
the code, the code wins and the choice is written here.

1. **The data types live in `vortex.scene`, the functions in `vortex.sim`.** `scene` is imported by `sim`, never
   the reverse, so `scene::Mechanism` cannot hold a `sim::` type. `MechanismKind`, the flags, `MechanismSegment`,
   `MechanismTrack`, `MechanismDesc` and `Mechanism` are in `engine/src/scene/Components.cppm`; `sim:mechanism`
   re-exports them with `using` declarations so callers may write `sim::MechanismSegment`.
2. **The desc is stored by value in the component** (no desc table and no `uint16_t desc` index): one entity carries
   everything a pose needs, offline, on the server and on every client copy alike.
3. **One link per lever in `[mech-1]`** (`Mechanism::linked` entity, `linkedNetId` on the wire). The design's "up to 4
   targets" comes with `[mech-3]` (buttons and winches).
4. **No ease byte.** The table is chosen by kind and direction (`EaseTableFor`): a portcullis rising uses
   smoothstep, every other motion ease-out (design "Ease"). Tables are generated by exact integer formulas
   (card 2), not listed.
5. **Function signatures add what the design needs:** `FindUseTarget` takes the tick (poses at the input's tick),
   `ApplyUse` takes the user's eye (the door opens away from it) and returns the segments it made
   (`std::vector<MechanismChange>`), `StepMechanisms` takes `authority` (crush, anchors and frees run only offline
   and on the server; a client only moves its bodies) and returns what it changed.
6. **Constants (design estimates, start values):** `kUseReach` 2.5 m, `kUseStaleTicks` 30, `kMechanismBusyTicks` 12,
   `kMechanismReopenTicks` 20, `kMechanismHitRadius` 0.3 m, lead clamp 4..20 ticks, offline lead 2,
   `kMechanismCrushTicks` 6, use-box inflation 0.1 m (prompt hysteresis 0.2 m).
7. **Door full-travel ticks are for the whole range.** A two-way door (-1024..1024) has `openTicks = closeTicks =
   72`, so 90 deg takes 36 ticks (0.6 s); a lever 0..512 has 12; the portcullis raise 180 (3 s), drop 45 (0.75 s).
8. **The spawn pose is an integer world voxel plus quarter turns about +y** (`spawnVoxel`, `quarterTurns`): both
   sides derive the same float pose from integers; the wire carries 12 + 1 bytes for it.
9. **`autoReturnTicks` must be 0 in `[mech-1]`** (no kind of this stage uses it): `ValidateMechanismDesc` refuses
   another value. `PositionAt` ignores it until `[mech-3]`.
10. **Authoring rule, distance form only:** a lever's use box is at least `kUseReach + 0.5` m from its target's outer
    face plane. The design's alternative ("or the line meets terrain") is left for `[mech-4]`'s generated forts.
11. **A cast whose ray resolves on a mechanism body runs `sim::MechanismBlast`:** no terrain operation, the
    mechanism's damage, the push to other bodies. Other clients see the dent's particles
    (`Client::ShowRemovedVoxels`) and fragments, no explosion burst (there is no carve event to present). Accepted:
    no new event kind for a picture.
12. **Broken = the lever's link is gone:** a use on a lever whose target is missing or freed still throws the lever
    and reports `Broken` (the HUD shows it). A lever whose own hinge is shot away is despawned (`Broken` flag in its
    last segment, then `DespawnReason::Broken`).
13. **Info targets skip the busy rule:** a use aimed at a portcullis answers `Locked` if it is locked or jammed,
    otherwise `NotUsable`.
14. **The sim tests are written in card 1, the net tests in the card that builds their path** (8-11): the net tests
    drive server and client paths that the skeleton cannot stub without inventing them. Every test of this stage
    exists, enabled, by card 11.
15. **The yard is in the default offline game and, in sessions, only where the session file says `mechanisms = 1`**
    (a new server-only key; `playground.session` sets it, `drill.session` and `arena250.session` do not, so G11-G19
    keep their numbers).

Added on 2026-11-24 (the owner: "hold a small object ... add it to card 4"; design "One use key: E"):

16. **One key, one resolution.** While a player holds an object, E drops it (no target search). Else
    `FindUseTarget` returns the nearest target of any kind (`UseTargetKind`: Mechanism 1, Info 2, Holdable 3, Rope 4
    reserved for `[mech-2]`, HeldByOther 5); ties go to the lower kind, then the lower key. A body held by another
    player occludes and answers `Busy`. `UseTarget::info` stays (the card-1 tests read it): `info == (kind == Info)`.
17. **Holdable, in two tests:** the shape test (`IsHoldableShape`, every side: no `Mechanism`, no `Held`, every model
    axis `<= 1.25 m`, solid voxels in `[SceneConfig::debrisBodyVoxels, llround(0.6 / vs^3)]`) and a per-side owner
    test through `UseTargetHooks::holdable` (server and offline: `RigidBody::motion == Dynamic`, a `NetId` in a
    session; a client: a copy whose server motion, `Copy::motion`, is Dynamic). The tie key is a hook too (NetId on
    both session sides). Not the design request's 80 kg: the crate weighs 750-1270 kg by its iron edges (design
    "Decisions refined" 1).
18. **The hold is posed, never simulated:** kinematic, `physics::Layer::Held` (meets nothing; rays only with the mask
    `LayerBit(Held)`), `MoveKinematic` to `HeldPoseAt` every tick after the players step. `StepPlayer` does not
    change. The pose hangs from a chest-height anchor (0.5 m below the eye), its distance comes from a sphere swept
    against terrain and block boxes (a thick clamp), and its centre stays in the play area.
19. **Auto-drop when pinned** (the clamp cannot keep the object out of a wall) for 30 ticks, not "0.5 m short"
    (design "Decisions refined" 2).
20. **The throw is a release reason** (`HoldEnd::Throw`), not a `CastRejection`: the Cast edge never reaches the cast
    path, the owner block's cast fields do not change, no charge is spent. `CastRejection` 6-10 are `[mag-2]`'s.
21. **Event kinds 41 and 42**, not 38 and 39: the plan (llm/README) gives 38-40 to `[mag-2]`; its `EventKind` row
    plans 41 and 42 for this stage since 2026-11-24.
22. **A held body is not counted as awake** (`CountPieces`, `EvictionCandidate`, `TrackBodies`, `AwakeSamples`,
    `AwakeBodies` skip it): no samples, no contacts, one per player. A release may take the awake count over 64 by at
    most the player count (accepted; design "Prediction and replication").
23. **The data types in scene, the functions in sim's `:mechanism`** (decision 1): `scene::Held` in
    `Components.cppm`; `sim::HoldChange` in `Blast.cppm` (as `MechanismChange`: `BlastResult` carries it).
24. **Card 1 is committed:** the holding declarations land in card 4 with their tests written enabled there, not as
    `DISABLED_` stubs.

Added on 2026-11-24 after the review of the holding design (32 findings; design "Decisions refined" 6-12):

25. **Client copies stay kinematic.** A hold or a release on a client changes the copy's `Held` component, layer,
    pose and `Copy::held` / `resting`, and calls `RemoteBodies::Restart(netId, tick)` (drop the buffered samples and
    refuse those of `tick` or older); never `Dynamic`, `SetVelocity` or `Activate`. A predicted drop or throw hangs
    until `EntityRelease` (chosen; design open question 19).
26. **A held body's track is not resting** from the pick-up (`EntityHold` is the wake); baselines and model answers
    never send `kObjectResting` with `kObjectHeld`; `AwakeBodies` and `AwakeSamples` skip held tracks.
27. **Not a player's floor:** a pick-up of a body any player stood on in the last 30 ticks is refused `Busy`.
28. **The echo of an auto-drop:** after a `Damaged` or `Pinned` release the server consumes the player's Use and Cast
    edges of the next `echo` ticks (that player's lead term). A Use and a Cast edge in one input while holding are
    one throw.
29. **Latched edges while a pick-up is pending**, discarded when it is refused.
30. **Other clients apply a release when the drawn holder reaches its tick** (at most 12 ticks later).
31. **The crush rule skips held bodies;** a blast releases a held body only when its sphere meets the posed model
    box.

## Skeleton

Card 1 lands this, with stub bodies (`// [mech-1] card N fills this`) that compile and return a neutral value
(`0`, `false`, `{}`, `UseResult::NoTarget`).

`engine/src/scene/Components.cppm` (inside `export namespace vortex::scene`, after `NetId`; add `#include <array>`
to the global fragment):

```cpp
/// Kinds of mechanism (docs/design/mechanisms.md "Mechanisms"). Wire format (card 8).
enum class MechanismKind : uint8_t { Door = 1, Portcullis = 2, Lever = 3 };   // 4 Button, 5 Drawbridge, 6 Winch: [mech-3]
/// State flags carried in every segment; Locked, Jammed, Freed and Broken stay set once set. Wire format.
enum MechanismFlag : uint8_t { MechanismFlag_Locked = 1, MechanismFlag_Jammed = 2, MechanismFlag_Freed = 4, MechanismFlag_Broken = 8 };
/// Authoring flags of a MechanismDesc. Wire format.
enum MechanismDescFlag : uint8_t {
    MechanismDesc_OneWay = 1,   ///< a door opens to maxPos only
    MechanismDesc_Usable = 2,   ///< E acts on it (doors, levers); a portcullis is an Info target
    MechanismDesc_Locked = 4,   ///< spawns with MechanismFlag_Locked
    MechanismDesc_Warded = 8,   ///< damage skips it entirely
};
/// One motion of a mechanism (wire: 4 + 2 + 2 + 2 + 1 = 11 bytes).
struct MechanismSegment {
    uint32_t startTick = 0;      ///< tick the motion starts
    int16_t startPos = 0;        ///< p at startTick
    int16_t targetPos = 0;       ///< == startPos when durationTicks == 0
    uint16_t durationTicks = 0;  ///< D; 0 = stopped at startPos
    uint8_t flags = 0;           ///< MechanismFlag
    friend bool operator==(const MechanismSegment&, const MechanismSegment&) = default;
};
inline constexpr uint32_t kMechanismSegments = 8;
/// The last kMechanismSegments segments, ring[0] oldest .. ring[count - 1] newest; count >= 1 once spawned.
struct MechanismTrack {
    std::array<MechanismSegment, kMechanismSegments> ring{};
    uint8_t count = 0;
    const MechanismSegment& Newest() const { return ring[count - 1]; }
};
/// A box of the original model, voxels [min, max) (wire: 6 x uint8).
struct MechanismBox {
    glm::ivec3 min{0}, max{0};
    friend bool operator==(const MechanismBox&, const MechanismBox&) = default;
};
/// What a mechanism is (wire: 63 bytes, card 8). Model-space fields are in the ORIGINAL model's voxels.
struct MechanismDesc {
    MechanismKind kind = MechanismKind::Door;
    uint8_t flags = 0;                 ///< MechanismDescFlag
    glm::ivec3 pivotQ{0};              ///< hinge point, 1/16 voxel (wire 3 x int16)
    uint8_t axis = 2;                  ///< motion axis: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z
    int16_t minPos = 0, maxPos = 0;    ///< travel range; hinge: 4096 = one turn; slide: 1/16 voxel
    uint16_t openTicks = 0;            ///< ticks for the whole range when opening
    uint16_t closeTicks = 0;           ///< ... when closing
    uint16_t autoReturnTicks = 0;      ///< 0 in [mech-1]
    MechanismBox useBox, blockBox, hingeBox;
    uint8_t pushAxis = 4;              ///< model axis (0-5) a landing pushes players along
    uint8_t anchorCount = 0;           ///< 0 or 2
    std::array<glm::ivec3, 2> anchors{};   ///< world voxels the hinge rests in (wire 2 x 3 x int32)
    friend bool operator==(const MechanismDesc&, const MechanismDesc&) = default;
};
/// A mechanism: kinematic voxel object on a timetable (docs/design/mechanisms.md "The model").
struct Mechanism {
    MechanismDesc desc;
    glm::ivec3 spawnVoxel{0};          ///< world voxel of the original model origin at p = 0
    uint8_t quarterTurns = 0;          ///< spawn rotation about +y, 0-3
    glm::ivec3 modelOffset{0};         ///< voxels: the sum of every DamageResult::keptOffset
    MechanismTrack track;
    Entity linked = kNoEntity;         ///< a lever's target ([mech-1]: at most one)
    uint32_t linkedNetId = 0;          ///< the same in a session (0 offline)
    uint32_t lastDecisionTick = 0;     ///< server / offline: the busy rule
    bool anyDecision = false;
    math::AABB swept;                  ///< world bounds over the whole travel (card 3)
};
```

`engine/src/scene/Scene.cppm` (card 5 fills it; card 1 declares it, the stub forwards to today's `Damage`):

```cpp
/// @brief Options of the mechanism damage rule (docs/design/mechanisms.md "The damage rule").
struct DamageOptions {
    uint16_t powerQ = 0xFFFF;          ///< a voxel with hardness * 16 > powerQ survives (CarveSphereQ's rule); 0xFFFF: today's rule
    bool keepByBox = false;            ///< the piece with the most voxels in [keepMin, keepMax) keeps the entity
    glm::ivec3 keepMin{0}, keepMax{0}; ///< current model voxels
    bool keepMotion = false;           ///< the kept piece keeps the motion type even when split, and is never a fragment
};
// in DamageResult:
uint32_t keptBoxVoxels = 0;            ///< keepByBox: voxels of the keep box in the kept piece (0: none held)
// in class Scene:
DamageResult Damage(Entity entity, const glm::dvec3& center, float radius, const DamageOptions& options);
```

New `engine/src/sim/Mechanism.cppm`, add `export import :mechanism;` to `Sim.cppm`:

```cpp
module;
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
export module vortex.sim:mechanism;
import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import :simulation;
import :player;
import :blast;
import :region;

export namespace vortex::sim {
    using scene::MechanismKind; using scene::MechanismSegment; using scene::MechanismTrack;
    using scene::MechanismDesc; using scene::Mechanism; using scene::kMechanismSegments;

    inline constexpr float kUseReach = 2.5f;                 ///< metres from the eye
    inline constexpr uint32_t kUseStaleTicks = 30;
    inline constexpr uint32_t kMechanismBusyTicks = 12;
    inline constexpr uint32_t kMechanismReopenTicks = 20;
    inline constexpr float kMechanismHitRadius = 0.3f;       ///< metres
    inline constexpr uint32_t kMechanismLeadMin = 4, kMechanismLeadMax = 20, kOfflineMechanismLead = 2;
    inline constexpr uint32_t kMechanismCrushTicks = 6;
    inline constexpr float kUseBoxInflate = 0.1f, kPromptHysteresis = 0.2f;   ///< metres
    static_assert(kMechanismSegments >= (kMechanismLeadMax + kMechanismBusyTicks - 1) / kMechanismBusyTicks +
                                            (kUseStaleTicks + kMechanismBusyTicks - 1) / kMechanismBusyTicks + 1);

    /// Why a use did nothing (snapshot owner block). Wire format.
    enum class UseResult : uint8_t { None = 0, Stale = 1, NoTarget = 2, Locked = 3, Busy = 4, Broken = 5, NotUsable = 6 };
    inline constexpr uint8_t kMaxUseResult = 6;
    const char* ToString(UseResult result);     ///< "accepted", "stale", "out of reach", "locked", "busy", "broken", "not usable"

    extern const std::array<uint32_t, 65> kEaseOut;    ///< T[i] = 16 * (4096 - (64 - i)^2)
    extern const std::array<uint32_t, 65> kEaseSmooth; ///< T[i] = (96 i^2 - i^3) / 2, integer division
    uint32_t Ease(const std::array<uint32_t, 65>& table, uint32_t k, uint32_t D);   ///< 0..65536, k < D
    const std::array<uint32_t, 65>& EaseTableFor(const MechanismDesc& desc, const MechanismSegment& segment);
    bool IsOpening(const MechanismDesc& desc, int16_t from, int16_t to);
    uint16_t SegmentDuration(int16_t from, int16_t to, const MechanismDesc& desc);
    int16_t PositionAt(const Mechanism& m, uint32_t tick);
    bool IsMoving(const MechanismTrack& track, uint32_t tick);        ///< newest started segment with D > 0 and tick < start + D
    uint32_t ArrivalTick(const MechanismTrack& track);                ///< Newest().startTick + Newest().durationTicks
    void PushSegment(MechanismTrack& track, const MechanismSegment& segment);
    bool ValidateMechanismDesc(const MechanismDesc& desc, const glm::ivec3& modelSize, std::string* why = nullptr);

    struct MechanismPose {
        glm::dvec3 position{0.0};   ///< world metres of the CURRENT model origin (the body's shape origin)
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    MechanismPose PoseAt(const Mechanism& m, int16_t p, float voxelSize);
    physics::Layer MechanismLayer(const Mechanism& m, uint32_t tick);
    math::AABB SweptBounds(const Mechanism& m, const glm::ivec3& modelSize, float voxelSize);

    /// Makes `entity` (a voxel object with a body) a mechanism at rest at `initialPos` from `tick`.
    bool MakeMechanism(Simulation& simulation, scene::Entity entity, const MechanismDesc& desc, const glm::ivec3& spawnVoxel,
                       uint8_t quarterTurns, int16_t initialPos, uint32_t tick);
    /// Before every StepPlayer: mechanisms whose swept bounds meet `reach` take their exact pose, velocity and layer at `tick`.
    void PoseMechanisms(Simulation& simulation, uint32_t tick, const math::AABB& reach);
    /// After PoseMechanisms: a mechanism that arrived at `tick` or `tick - 1` and overlaps the capsule pushes the feet out.
    bool PushOutOfMechanisms(Simulation& simulation, uint32_t tick, PlayerState& state, physics::CharacterId character,
                             const PlayerTuning& tuning);
    struct MechanismStepResult {
        std::vector<MechanismChange> changes;   ///< Freed by the anchors (authority only)
        uint32_t moving = 0;
        uint32_t crushed = 0;
        std::vector<glm::dvec3> crushedAt;      ///< where a crushed body was (offline puffs)
    };
    /// Before StepPhysics: moving mechanisms MoveKinematic to pose(tick + 1); with `authority` the crush rule and anchors.
    MechanismStepResult StepMechanisms(Simulation& simulation, uint32_t tick, bool authority);

    struct UseTarget {
        scene::Entity entity = scene::kNoEntity;
        bool info = false;          ///< a non-usable mechanism (a portcullis): the prompt's info line
        float distance = 0.0f;      ///< metres from the eye
    };
    bool FindUseTarget(const Simulation& simulation, const glm::dvec3& eye, const glm::vec3& dir, uint32_t tick, UseTarget& out);
    /// The prompt's hysteresis: the ray still meets `entity`'s use box inflated by `inflate` metres.
    bool RayMeetsUseBox(const Simulation& simulation, scene::Entity entity, const glm::dvec3& eye, const glm::vec3& dir,
                        uint32_t tick, float inflate);
    UseResult ApplyUse(Simulation& simulation, const UseTarget& target, const glm::dvec3& eye, uint32_t tick, uint32_t leadTicks,
                       std::vector<MechanismChange>& changes);
    /// HUD text of a target: "[E] Open", "[E] Close", "[E] Raise gate", "[E] Lower gate", "Gate - opened by a lever", "Locked".
    std::string UsePrompt(const Simulation& simulation, const UseTarget& target, uint32_t tick, bool& greyed);

    struct MechanismHit {
        uint32_t removed = 0;
        bool freed = false;         ///< door / portcullis off its hinge (MechanismFlag_Freed)
        bool broken = false;        ///< lever destroyed (MechanismFlag_Broken)
    };
    /// The damage rule on one mechanism at `centre` (aimed: the caller checked kMechanismHitRadius). Appends to
    /// `into` (objectsHit, fragments, objectEvents, mechanismChanges).
    MechanismHit DamageMechanism(Simulation& simulation, scene::Entity entity, const glm::dvec3& centre, uint16_t powerQ,
                                 BlastResult& into);
    /// A cast that resolved on a mechanism's body: no terrain operation, DamageMechanism, the push to other bodies.
    BlastResult MechanismBlast(Simulation& simulation, scene::Entity target, const voxel::SphereQ& sphere, uint16_t power);
    /// Metres from `point` to the mechanism's posed model box (0 inside).
    float DistanceToMechanism(const Simulation& simulation, scene::Entity entity, const glm::dvec3& point);
    /// Offline and server: frees a door / portcullis (Dynamic, no Mechanism component) or breaks a lever.
    void ReleaseMechanism(Simulation& simulation, scene::Entity entity, uint32_t tick, uint8_t flag, std::vector<MechanismChange>& changes);
}
```

`engine/src/sim/Blast.cppm` (card 1; here because `BlastResult` carries it and `:mechanism` imports `:blast`, never
the reverse):

```cpp
/// A segment a mechanism took: what the server sends as MechanismMove ([mech-1]).
struct MechanismChange {
    scene::Entity entity = scene::kNoEntity;
    scene::MechanismSegment segment;
};
// in BlastResult:
std::vector<MechanismChange> mechanismChanges;   ///< segments the blast's mechanism damage made (Freed / Broken)
```

`engine/src/sim/Region.cppm` (cards 1 and 7):

```cpp
/// A mechanism of a template (spawned after the objects; links by index).
struct TemplateMechanism {
    std::shared_ptr<voxel::VoxelModel> model;
    glm::ivec3 spawnVoxel{0};
    uint8_t quarterTurns = 0;
    scene::MechanismDesc desc;
    int16_t initialPos = 0;
    int32_t link = -1;               ///< index into RegionTemplate::mechanisms of a lever's target
};
// in RegionTemplate:
std::vector<TemplateMechanism> mechanisms;
/// The yard (a hut with a door; a wall with a portcullis and a lever), north of the playground's plaza.
RegionTemplate MechanismYardTemplate(voxel::World& world, const voxel::WorldMaterials& m, const glm::ivec3& plazaAnchor);
/// Creates a template's mechanisms in order and links them; empty (logged) if the authoring rule fails.
std::vector<scene::Entity> CreateTemplateMechanisms(Simulation& simulation, const RegionTemplate& t);
/// The authoring rule (Decisions 10): every lever's use box is at least kUseReach + 0.5 m from its target's outer face.
bool CheckMechanismAuthoring(const RegionTemplate& t, float voxelSize, std::string* why = nullptr);
```

`engine/src/sim/Player.cppm`: `PlayerButton_Use = 16` (card 1, with the comment "edge: E, a use (mechanisms.md);
the crate moves to Q"); `kPlayerButtonMask` stays 15 until card 6.

**Added on 2026-11-24 (holding; card 4 lands these, card 1 is committed).**

`engine/src/physics/PhysicsWorld.cppm` `Layer`: after `Bounds`, `Held, ///< a held object: meets no body, no character,
and only a ray whose mask is exactly LayerBit(Held)`. `PhysicsWorld.cpp`: `kLayerHeld = 6`, `kLayerCount` 7;
`LayerPairFilter`: false when either is Held; `ObjectVsBroadPhaseFilter`: false for Held; `CharacterLayerFilter`: false
for Held; `LayerMaskFilter`: Held only when the mask is exactly `LayerBit(Held)` (so the default mask 0xFF does not see
it).

`engine/src/scene/Components.cppm`, after `Mechanism`:

```cpp
/// @brief A body a player holds (docs/design/mechanisms.md "One use key: E"). Its presence is the truth.
struct Held {
    uint8_t holder = 0;          ///< session slot; 0 offline
    uint16_t relYaw = 0;         ///< object yaw - holder yaw at pick-up (PlayerInput yaw units)
    uint32_t sinceTick = 0;      ///< the tick the hold started
    uint16_t pinnedTicks = 0;    ///< consecutive pinned ticks (authority only)
};
```

`engine/src/sim/Blast.cppm`, after `MechanismChange`:

```cpp
/// @brief A hold or a release: what the server sends as EntityHold / EntityRelease ([mech-1]).
struct HoldChange {
    scene::Entity entity = scene::kNoEntity;
    bool hold = false;           ///< true: EntityHold; false: EntityRelease
    uint8_t holder = 0;
    uint16_t relYaw = 0;
    uint32_t tick = 0;
    uint8_t reason = 0;          ///< sim::HoldEnd (release)
    glm::vec3 position{0.0f};    ///< release pose (shape origin) and velocity (quantised)
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};
};
// in BlastResult:
std::vector<HoldChange> holdChanges;   ///< releases (Damaged) the blast made, before its object events
```

`engine/src/sim/Mechanism.cppm` (`UseTarget` gains `kind`; the rest is new; add `#include <functional>` and
`#include <span>` to the global fragment):

```cpp
enum class UseTargetKind : uint8_t { None = 0, Mechanism = 1, Info = 2, Holdable = 3, Rope = 4, HeldByOther = 5 };
// UseTarget gains, after `info`:  UseTargetKind kind = UseTargetKind::None;

/// @brief What differs per side in FindUseTarget (design "The order rule" step 4, "What can be held").
struct UseTargetHooks {
    /// The owner test of a holdable body (after IsHoldableShape). Empty: IsHoldable(sim, e, false).
    std::function<bool(scene::Entity)> holdable;
    /// The tie key. Empty: uint32_t(entity). Server: NetId::value; client: the NetId of the copy.
    std::function<uint32_t(scene::Entity)> key;
};
// FindUseTarget gains a last parameter: const UseTargetHooks* hooks = nullptr

inline constexpr float kHoldDistance = 1.5f, kHoldClearance = 0.1f, kHoldMaxExtent = 1.25f;   ///< metres
inline constexpr float kHoldDrop = 0.5f;               ///< metres: the hold anchor below the eye
inline constexpr float kHoldMaxVolume = 0.6f;          ///< m^3 of solid voxels
inline constexpr uint32_t kHoldPinnedTicks = 30, kHoldBusyTicks = 12;
inline constexpr uint32_t kHoldGroundTicks = 30;       ///< a body stood on this recently is not a pick-up
inline constexpr int16_t kHoldPitchLimit = 16384;      ///< 45 deg in PlayerInput pitch units
inline constexpr float kThrowSpeed = 6.0f;             ///< m/s along the look
inline constexpr float kHoldReleaseMin = 0.3f;         ///< metres: the release fix-up's nearest centre
static_assert(kHoldDistance >= 0.8660254f * kHoldMaxExtent + kHoldClearance);

/// Why a hold ended (EntityRelease). Values are wire format.
enum class HoldEnd : uint8_t { Drop = 1, Throw = 2, HolderDead = 3, HolderGone = 4, Damaged = 5, Pinned = 6 };
inline constexpr uint8_t kMaxHoldEnd = 6;

/// @brief Where a hold is posed and released besides the holder: the play area and the other players.
struct HoldEnv {
    PlayerTuning tuning;
    PlayArea area;                         ///< sim::PlayAreaOf(region, tuning) in a session; PlayArea{} offline
    std::span<const glm::vec3> playerFeet; ///< every player's feet (the release fix-up's capsules)
};

int8_t HoldPitch(int16_t pitch);                       ///< clamp(pitch, -16384, 16384) / 256
uint32_t HoldMaxVoxels(float voxelSize);               ///< llround(kHoldMaxVolume / vs^3)
uint16_t RelYawAtPickup(const glm::quat& rotation, uint16_t holderYaw);
glm::dvec3 HoldAnchor(const glm::vec3& feet, const PlayerTuning& tuning);   ///< feet + (0, eyeHeight - kHoldDrop, 0)
/// Every side: VoxelBody and RigidBody, no Mechanism, no Held, extents, solid voxels in [debrisBodyVoxels, HoldMaxVoxels].
bool IsHoldableShape(const Simulation& simulation, scene::Entity entity);
/// Server and offline: the shape test, RigidBody::motion == Dynamic, and with `replicatedOnly` a NetId != 0.
bool IsHoldable(const Simulation& simulation, scene::Entity entity, bool replicatedOnly);
/// The pick-up path: the sphere of the object's radius swept from `anchor` to its centre meets no solid voxel.
bool PickupPathClear(const Simulation& simulation, scene::Entity entity, const glm::dvec3& anchor);
struct HeldPose {
    MechanismPose pose;          ///< shape origin and rotation
    float distance = 0.0f;       ///< d: anchor to the object's centre
    bool pinned = false;
};
/// The pose of `held` for a holder at `feet` looking (yaw, HoldPitch * 256); mechanisms at `tick` clamp it.
HeldPose HeldPoseAt(const Simulation& simulation, scene::Entity held, const glm::vec3& feet, uint16_t yaw, int8_t holdPitch,
                    const HoldEnv& env, uint32_t tick);
/// Authority: picks `entity` up for `holder` (None, Busy or NoTarget); `out` is the EntityHold. `stoodOn`: a player
/// stood on the body within kHoldGroundTicks (the caller knows; offline the own ground body now).
UseResult BeginHold(Simulation& simulation, scene::Entity entity, uint8_t holder, uint16_t yaw, uint32_t tick,
                    uint32_t lastChangeTick, bool anyChange, bool stoodOn, bool replicated, HoldChange& out);
/// Every tick after the players: MoveKinematic to the pose; with `authority` counts pinned ticks and returns true
/// at kHoldPinnedTicks (the caller ends the hold Pinned).
bool StepHeld(Simulation& simulation, scene::Entity entity, const PlayerState& holder, uint16_t yaw, int8_t holdPitch,
              const HoldEnv& env, uint32_t tick, bool authority);
/// Server and offline: ends the hold at `pose` (fix-up out of walls, bodies and players), Dynamic, quantised
/// `velocity`; `out` is the EntityRelease. `anchor` (the holder's; nullptr for Damaged and HolderGone) enables the fix-up.
void EndHold(Simulation& simulation, scene::Entity entity, HoldEnd reason, const MechanismPose& pose, const glm::vec3& velocity,
             const glm::dvec3* anchor, const HoldEnv& env, uint32_t tick, bool replicated, HoldChange& out);
glm::vec3 ReleaseVelocity(HoldEnd reason, const glm::vec3& holderVelocity, uint16_t yaw, int16_t pitch);
/// A client copy (kinematic, never simulated): `hold` emplaces Held and Layer::Held; a release removes Held and
/// teleports to the release pose. The layer after a release and the samples are the caller's (net).
void ApplyHoldChange(Simulation& simulation, scene::Entity entity, const HoldChange& change);
/// "[E] Drop   [LMB] Throw".
std::string HoldPrompt();
```

Test files (new): `engine/tests/sim/MechanismTests.cpp` (suites `Mechanism`, `Use`; `Hold` from card 4),
`engine/tests/net/MechanismNetTests.cpp` (suites `Mechanisms` and the net `Hold.*` tests, card 8 on). Shared test helpers get a `Yard` / `Make` prefix (never `Mechanism` or `Door`: a test
helper never shares a name with an engine type).

## Card 1: Skeleton

| | |
|---|---|
| Goal | Every interface of the stage compiles, stubbed; the sim tests exist, disabled |
| Depends on | `[wp-7]`, `[wp-8]` committed |
| Effort | 0.5 d |
| Touches | scene, sim; wire: no; movement rule: no; buildHash: no |

**Read first** (about 60 KiB)
- This pack's "Decisions" and "Skeleton".
- `engine/src/scene/Components.cppm` (all), `engine/src/scene/Scene.cppm` `DamageResult`, `Damage`.
- `engine/src/sim/Sim.cppm`, `engine/src/sim/Region.cppm` `TemplateObject`, `RegionTemplate`, `CreateTemplateObjects`.
- `engine/docs/design/mechanisms.md` "Tests" (the names).

**Files**
- change `engine/src/scene/Components.cppm`, `engine/src/scene/Scene.cppm`, `engine/src/scene/internal/Scene.cpp`
  (the new `Damage` overload: stub = `return Damage(entity, center, radius);`).
- new `engine/src/sim/Mechanism.cppm`, `engine/src/sim/internal/Mechanism.cpp` (`module; ... module vortex.sim;`);
  change `engine/src/sim/Sim.cppm`, `engine/src/sim/Region.cppm`, `engine/src/sim/internal/Region.cpp` (stubs),
  `engine/src/sim/Player.cppm` (`PlayerButton_Use = 16`).
- new `engine/tests/sim/MechanismTests.cpp`.

**Steps**
1. Add the declarations exactly as in "Skeleton". `kEaseOut` / `kEaseSmooth` are defined in card 2; in card 1 define
   them as zero arrays.
2. Stubs return neutral values; `ToString(UseResult)` returns the strings in the comment already (it is trivial).
3. Write every test of the "Tests" lists of cards 2-5 in `MechanismTests.cpp`, in full, each named `DISABLED_<Name>`.
   They compile against the skeleton.

**Tests**
- All of cards 2-5, disabled. `ctest -N` lists them as disabled (gtest prints them as `DISABLED`).

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning.
2. `tools/ctest.sh` - all pass (the new ones are disabled).
3. `tools/doc-check.sh --build build` - PASS (update ROADMAP's test count line: `ctest -N` total and `TEST(` count).

**Docs**
- ROADMAP.md: the test count line only.

**Must not break**
- `Scene.CreationFailuresReturnNoEntityAndLeakNothing` and every `Scene.*` / `Replication.*` test: the old
  `Damage(entity, center, radius)` keeps its exact behaviour.

**Done when**
- [ ] Build green, all tests green, the new tests present as `DISABLED_`.

**Traps**
- `Components.cppm` imports `vortex.math` already (for `math::AABB` in `Mechanism::swept`); add no `import
  vortex.sim` anywhere in `scene` (layering).

## Card 2: The timetable

| | |
|---|---|
| Goal | A mechanism's travel position is an exact integer function of the tick, the same on every machine |
| Depends on | card 1 |
| Effort | 0.5 d |
| Touches | sim (`Mechanism.cpp`); wire: no; movement rule: no |

**Read first** (about 20 KiB)
- mechanisms.md "The model: an integer travel position on a timetable" and "Kinds".
- This card.

**Steps**
1. Tables (constexpr-generated, `int64` arithmetic): `kEaseOut[i] = 16 * (4096 - (64 - i) * (64 - i))`;
   `kEaseSmooth[i] = (96 * i * i - i * i * i) / 2` (integer division). Both give `T[0] = 0`, `T[64] = 65536`, monotonic.
   Golden: `kEaseOut[1] = 2032`, `[32] = 49152`, `[63] = 65520`; `kEaseSmooth[1] = 47`, `[32] = 32768`, `[63] = 65488`.
2. `Ease(T, k, D)` (requires `0 <= k < D`): `f = k * 64` (uint64); `i = f / D`; `r = f % D`;
   `return T[i] + (int64(T[i + 1]) - T[i]) * r / D` (int64; non-negative since T is monotonic).
3. `IsOpening(desc, from, to)`: Door: `abs(to) > abs(from)`; Portcullis and Lever: `to > from`.
4. `EaseTableFor(desc, seg)`: Portcullis with `seg.targetPos > seg.startPos` -> `kEaseSmooth`; everything else ->
   `kEaseOut`.
5. `SegmentDuration(from, to, desc)`: `range = maxPos - minPos` (> 0 by validation); `full = IsOpening ? openTicks :
   closeTicks`; `delta = abs(to - from)`; `D = (uint32(delta) * full + range - 1) / range` (ceil); clamp to 65535.
   `from == to` -> 0.
6. `PositionAt(m, tick)`: scan `ring[count - 1]` down to `ring[0]` for the first segment with `startTick <= tick`;
   none -> return `ring[0].startPos`. With segment s: `k = tick - s.startTick`; if `k >= s.durationTicks` return
   `s.targetPos` (covers D == 0); else `e = Ease(EaseTableFor(desc, s), k, D)`;
   `return s.startPos + int16((int64(s.targetPos) - s.startPos) * e / 65536)` (C++ `/` truncates toward zero).
7. `IsMoving(track, tick)`: the segment `PositionAt` would use has `D > 0` and `tick < startTick + D`.
   `ArrivalTick(track) = Newest().startTick + Newest().durationTicks`.
8. `PushSegment(track, s)`: if `count == kMechanismSegments`, shift `ring[1..7]` down to `[0..6]` (the oldest goes)
   and write `ring[7]`; else `ring[count++] = s`. Sticky flags: `s.flags |= Newest().flags & (Locked | Jammed |
   Freed | Broken)` when `count > 0` before the push. The caller guarantees `s.startTick >= Newest().startTick + 1`.
9. `ValidateMechanismDesc(desc, modelSize, why)`: kind 1..3; `minPos < maxPos`; axis and pushAxis 0..5;
   `autoReturnTicks == 0`; `openTicks > 0 && closeTicks > 0`; each box `0 <= min < max <= 255` per axis; Door / Lever
   need `Usable`; Portcullis must not have it; `anchorCount` 0 or 2.

**Tests** (rename `DISABLED_` away for these)
- `Mechanism.PositionIsAFunctionOfTick`: door 0 -> 1024, D 36, ease-out: positions at k = 1, 9, 18, 27, 35, 36 are
  56, 448, 768, 960, 1023, 1024; and the same equals a tick-by-tick reference loop computing
  `start + (target - start) * Ease / 65536` for every k; `p == 1024` for every tick >= startTick + 36;
  a `D == 0` segment gives `startPos` at every tick. Closing 1024 -> 0, D 36: k = 1, 18, 35 give 968, 256, 1.
  Portcullis raise 0 -> 320, D 180, smoothstep: k = 1, 45, 90, 135, 179, 180 give 0, 50, 160, 270, 319, 320.
  Lever 0 -> 512, D 12: k = 1, 6, 11 give 81, 384, 508.
- `Mechanism.DurationIsIntegerExact`: two-way door (-1024..1024, 72 / 72): 0 -> 1024 is 36; lever (0..512, 12 / 12):
  0 -> 512 is 12; a door reversal 300 -> 0 is 11 (ceil(300 x 72 / 2048) = ceil(10.55)).
- `Mechanism.ReversalKeepsPositionContinuous`: a door opening from tick 100 (D 36); a closing segment pushed at
  every start tick 101..135 from `PositionAt(start)`; at its start tick the position equals the opening's at that
  tick (no jump), and it reaches 0 exactly at its arrival.
- `Mechanism.RingHoldsTheLeadAndTheStaleWindow`: segments pushed every 12 ticks with start = decision + 20; at every
  push, `PositionAt` for every tick in `[decision - 30, decision + 20]` equals the value an unbounded list of the
  same segments gives (the ring never lost a segment still needed).

**Checks**
1. build; 2. `tools/ctest.sh -R 'Mechanism\.' --output-on-failure`; 3. `tools/ctest.sh`; 4. doc-check.

**Must not break**
- Nothing outside `Mechanism.cpp` changes.

**Done when**
- [ ] The four tests pass with the exact values above.

**Traps**
- `int16_t` arithmetic overflows: compute in `int64`, cast at the end. Never use `float` or `std::round` here.

## Card 3: The layer rule, posing and stepping

| | |
|---|---|
| Goal | A moving door is a ghost, a rising gate stays solid, a landing gate pushes players out, and a crate cannot prop a gate open |
| Depends on | card 2 |
| Effort | 1 d |
| Touches | sim (`Mechanism.cpp`, `Replication.cpp`); wire: no; movement rule: no (`StepPlayer` untouched) |

**Read first** (about 50 KiB)
- mechanisms.md "Collision with players ...", "The one movement rule grows by one call".
- `engine/src/sim/internal/Replication.cpp` `CountPieces`, `EvictionCandidate`; `engine/src/sim/Replication.cppm`.
- `engine/src/physics/PhysicsWorld.cppm` `MoveKinematic`, `SetLayer`, `SetVelocity`, `SetCharacterState`.
- `engine/src/scene/Scene.cppm` `SetTransform`, `QueryBodies`; `engine/src/sim/Player.cppm` `PlayerBounds`.

**Steps**
1. `axisVec(a)`: 0 (1,0,0), 1 (-1,0,0), 2 (0,1,0), 3 (0,-1,0), 4 (0,0,1), 5 (0,0,-1).
2. `PoseAt(m, p, vs)`: `R0 = angleAxis(quarterTurns * pi/2, +y)`; `o = vec3(modelOffset)` (voxels).
   Hinge kinds (Door, Lever): `pivot = vec3(pivotQ) / 16`; `Rp = angleAxis(p * 2 pi / 4096, axisVec(axis))`;
   `K(o) = pivot + Rp * (o - pivot)`; rotation = `R0 * Rp`. Slide (Portcullis): `K(o) = o + axisVec(axis) * (p /
   16.0)`; rotation = `R0`. Position = `dvec3(spawnVoxel) * vs + dvec3(R0 * (K(o) * vs))`. (The current model's
   voxel u is the original's u + modelOffset; desc boxes are read as `box - modelOffset` in current voxels.)
3. `MechanismLayer(m, tick)`: not `IsMoving` -> `Layer::Dynamic`. Moving: Portcullis with the active segment's
   `targetPos > startPos` (rising out of the passage) -> `Dynamic`; else `Rubble`. Never `Debris`.
4. `SweptBounds(m, size, vs)`: for p in `minPos + (maxPos - minPos) * j / 8`, j = 0..8: pose the 8 corners of the
   current model box `[0, size)`, union; grow by one voxel. `MakeMechanism` stores it in `swept`.
5. `MakeMechanism(sim, e, desc, spawnVoxel, turns, initialPos, tick)`: refuse (false) unless the entity has
   `VoxelBody` and `RigidBody` and `ValidateMechanismDesc` holds. Emplace `Mechanism{desc, spawnVoxel, turns}` with
   one segment `{tick, initialPos, initialPos, 0, (desc.flags & Locked) ? MechanismFlag_Locked : 0}`;
   `physics.SetMotionType(body, Kinematic)`, `RigidBody::motion = Kinematic`; place it at `PoseAt(initialPos)`
   (`Scene::SetTransform`, teleport); `SetLayer(MechanismLayer)`; `swept = SweptBounds`.
6. **Visit order** for every loop over mechanisms (pose, step, use target, crush): ascending key, key = `NetId::value`
   when the entity has one, else `uint32(entity)`. Collect `(key, entity)` from `view<scene::Mechanism>()`, sort.
7. `PoseMechanisms(sim, tick, reach)`: for each mechanism (order 6) whose `swept` meets `reach` and which has a
   `RigidBody`: `a = PoseAt(PositionAt(tick))`, `b = PoseAt(PositionAt(tick + 1))`;
   `scene.SetTransform(e, vec3(a.position), a.rotation, true)`; `physics.SetVelocity(body, vec3(b.position -
   a.position) / dt, AngularVelocity(a.rotation, b.rotation, dt))`; `physics.SetLayer(body, MechanismLayer(m,
   tick))`. `AngularVelocity(q0, q1, dt)`: `d = q1 * conjugate(q0)`; if `d.w < 0` negate; `angle = 2 * acos(min(d.w,
   1))`; under 1e-7 -> zero; else `normalize(vec3(d.x, d.y, d.z)) * angle / dt`. `dt` = `Config().fixedTimeStep`.
8. `PushOutOfMechanisms(sim, tick, state, character, tuning)`, per mechanism (order 6) with `A = ArrivalTick`,
   `Newest().durationTicks > 0` and `A == tick || A + 1 == tick`:
   1. `B` = `PlayerBounds(state.position, tuning)` shrunk by 0.02 m on every side.
   2. Overlap: map B's 8 corners into current model voxels (`local = conj(rot) * (corner - pos) / vs`), take
      `[floor(min), ceil(max))` clamped to the model size; visit z, y, x ascending; a solid voxel whose world centre
      (`pos + rot * ((v + 0.5) * vs)`) lies inside B is an overlap. None -> next mechanism.
   3. `n` = `rot * axisVec(pushAxis)` with `n.y = 0`, normalised; length under 0.5 -> skip (log once:
      `Mechanisms: push axis of N is vertical`).
   4. `mid` = world centre of the posed block box (`(blockBox.min + blockBox.max) / 2 - modelOffset` voxels);
      `half` = the largest `|dot(corner - mid, n)|` over the block box's 8 posed corners.
   5. Side: `s = dot(Q(feet) - Q(mid), n) >= 0 ? +1 : -1` where `Q(x) = llround(x * 16 / vs)` per axis (the
      quantised positions decide, not the float difference).
   6. For `side` in `{s, -s}`: `d = dot(feet - mid, n) * side`; `target = feet + n * side * (half - d + tuning.radius +
      0.02)`; if no solid world voxel (`World::GetVoxel`) lies in the voxel box of `PlayerBounds(target)`:
      `state.position = target`, `physics.SetCharacterState(character, target, state.velocity)`, return true.
   7. Both sides blocked: nothing (Jolt's penetration recovery).
9. `StepMechanisms(sim, tick, authority)`, per mechanism (order 6):
   1. If `IsMoving(track, tick)` or `ArrivalTick == tick + 1`: `a = PoseAt(PositionAt(tick))`, `b =
      PoseAt(PositionAt(tick + 1))`; `SetTransform(a, teleport)`; `physics.MoveKinematic(body, b.position, b.rotation,
      dt)`; `moving++`.
   2. Layer: `SetLayer(MechanismLayer(m, tick + 1))` for every mechanism (the layer the coming physics step uses).
   3. `authority` only, the crush rule: if the newest segment is closing (`!IsOpening`), `D > 0`, and `tick + 1` is in
      `[start + D - kMechanismCrushTicks, start + D]`: box = the block box at pose b; `scene.QueryBodies(world AABB of
      the box)`; for each entity found that is not a mechanism, has a `RigidBody` with motion Dynamic, and whose
      `physics.GetBounds` mapped into the mechanism's current model voxels (8 corners, their box) overlaps the block
      box: replicated (`NetId`) -> `sim::Despawn(sim, e, DespawnReason::Broken)`, else `scene.Destroy(e)`;
      `crushed++`, `crushedAt.push_back(body centre)`. Levers never crush.
   4. `authority` only, anchors: `anchorCount == 2`, both anchor chunks loaded (`World::IsChunkLoaded`) and both
      `World::GetVoxel(anchor) == 0` -> `ReleaseMechanism(sim, e, tick, MechanismFlag_Freed, result.changes)`.
10. `ReleaseMechanism(sim, e, tick, flag, changes)`: `p = PositionAt(tick)`; segment `{max(tick, Newest().startTick +
    1), p, p, 0, Newest().flags | flag}` pushed and appended to `changes`; then Lever (`flag == Broken`): with a
    `NetId` -> `Despawn(Broken)`, else `scene.Destroy`. Door / Portcullis (`Freed`): every lever whose `linked == e`
    gets `linked = kNoEntity` (keep `linkedNetId` for the wire, set 0); remove the `Mechanism` component;
    `SetMotionType(Dynamic)`, `RigidBody::motion = Dynamic`, `SetLayer(Dynamic)`, `Activate`. From here it is an
    ordinary body.
11. `Replication.cpp`: `CountPieces` and `EvictionCandidate` skip entities that have `scene::Mechanism` (a kinematic
    body is always awake to Jolt: a door must never take one of the 64 awake slots).

**Tests**
- `Mechanism.LayerFollowsMotion`: Dynamic at rest; Rubble from `startTick` to `start + D - 1` for a door (both ways)
  and a portcullis drop; Dynamic through a portcullis rise; Dynamic at the arrival tick; a 24-voxel lever is never
  Debris (`physics.GetLayer` after `PoseMechanisms` and after `StepMechanisms`).
- `Mechanism.NotCountedAsAwake`: a door swinging (moving), `CountPieces(sim).awakeBodies` is 0 and `TakePieceSlot`
  answers Replicate with `ReplicationConfig::enabled`.
- `Mechanism.LandingPushesThePlayerOut`: a portcullis dropping onto a player standing in the passage; at the
  arrival tick `PushOutOfMechanisms` moves the feet to the side they were on (2 cm clear of the block box); with a
  terrain block on that side, to the other side; with blocks on both sides, not at all.
- `Mechanism.StandingOnALoweredBridgeIsNotPushed`: a slide mechanism at rest at its target (arrival long ago), a
  player standing on it: no push. (A Portcullis-kind slide lying flat stands in for the `[mech-3]` drawbridge.)
- `Mechanism.LeaningOnAClosedDoorIsNotPushed`: a closed door at rest, the capsule touching it: no push.
- `Mechanism.ClosingGateCrushesACrateInsteadOfLaunchingIt`: a crate (`CrateModel`) under a dropping portcullis;
  step 120 ticks with `StepMechanisms(authority)` + `StepPhysics`; the crate's speed never exceeds 5 m/s (a launch is
  more) and it is gone by the arrival tick; `crushed == 1`.
- `Mechanism.PlayerStepIsTheSameReplayed`: a player walking through a door that opens; record states with
  `PoseMechanisms` + `PushOutOfMechanisms` + `StepPlayer` + `StepMechanisms(false)` + `StepPhysics` for 60 ticks;
  restore the state of tick 20 (`RestorePlayer`) and replay 21..59 the same way: every state equal bit for bit.

**Checks**
1. build; 2. `tools/ctest.sh -R 'Mechanism\.|Replication\.' --output-on-failure`; 3. `tools/ctest.sh`; 4. doc-check.

**Must not break**
- `Replication.*`, `Entities.OneScriptGivesTheSameBodiesTwice`: `CountPieces` changes only for mechanism entities.
- CLAUDE.md section 5 "Movement": `StepPlayer` is not changed; `kPlayerRuleRevision` stays.
- L8: kinematic bodies are never frozen by the terrain guard (only dynamic ones are): do not add them to it.

**Done when**
- [ ] The seven tests pass; no other test changed.

**Traps**
- `Scene::SetTransform` wakes the body; that is fine for kinematic bodies. Never call `AddImpulse` on a mechanism.
- Use `PositionAt(tick + 1)` for the velocity, never a difference of floats over several ticks.

## Card 4: Use targets, ApplyUse and links

| | |
|---|---|
| Goal | Looking at a door and pressing E opens it away from you; a lever runs its gate; spamming E cannot keep a gate open; E picks up a crate, it follows the holder as a ghost, and drops, throws and auto-drops work in the sim |
| Depends on | card 3 |
| Effort | 2 d (1 d mechanisms, 1 d holding, steps 8-18) |
| Touches | sim (`Mechanism.cpp`, `Region.cpp`, `Blast.cpp`, `Replication.cpp`), scene (`Held`), physics (`Layer::Held`); wire: no; movement rule: no |

**Read first** (about 70 KiB)
- mechanisms.md "The look ray and the target", "What the server does with a Use edge" (steps 3-5 and the table),
  "Linked mechanisms", "The prompt on the HUD", "One use key: E" (all of it except "Prediction and replication").
- This pack's "Skeleton" part "Added on 2026-11-24".
- `engine/src/voxel/World.cppm` `Raycast`, `WorldRayHit`.
- `engine/src/physics/internal/PhysicsWorld.cpp` the layer filters (`LayerPairFilter`, `ObjectVsBroadPhaseFilter`,
  `CharacterLayerFilter`, `LayerMaskFilter`) and the default `Raycast` mask; `engine/src/scene/internal/Scene.cpp`
  `QueryBodies`, `CreateBody` (the Debris size rule, `SceneConfig::debrisBodyVoxels`); the crush loop of
  `StepMechanisms` (`engine/src/sim/internal/Mechanism.cpp`, card 3).
- `engine/src/sim/internal/Blast.cpp` steps "3." and "4."; `engine/src/sim/internal/Replication.cpp` `CountPieces`,
  `EvictionCandidate`; `engine/src/sim/internal/Player.cpp` `LookDirection`, `YawRadians`, `QuantizeYaw`.

**Steps**
1. Ray against a box in a mechanism's frame (`RayBox(m, pose, box, inflateMetres, eye, dir, vs, tHit)`):
   `lo = vec3(box.min - modelOffset) - inflate / vs`, `hi = vec3(box.max - modelOffset) + inflate / vs`;
   `o = conj(rot) * (eye - pos) / vs`, `d = conj(rot) * dir`; slab test; `tEnter = max(0, tNear)` in voxels; hit when
   `tNear <= tFar` and `tFar >= 0`; metres = `tEnter * vs`.
2. `FindUseTarget(sim, eye, dir, tick, out)`:
   1. `terrain = World().Raycast(eye, dir, kUseReach)`; `limit = terrain.hit ? terrain.distance : kUseReach`.
   2. For each mechanism (order of card 3 step 6) whose `swept` meets the segment eye..eye + dir * kUseReach: pose at
      `PositionAt(tick)`; `block[m]` = block box distance (or none); `use[m]` = use box inflated by
      `kUseBoxInflate` (or none) if `desc.flags & Usable`.
   3. Usable candidates: `use[m] < limit` and `use[m] <` every other mechanism's `block` (its own block box does not
      hide its own use box). Pick the smallest `use[m]`; ties: the lower key. Found -> `out = {e, false, use[m]}`,
      true.
   4. Else the nearest `block[m] < limit`; if that mechanism is not Usable -> `out = {e, true, block[m]}`, true.
   5. Else false.
3. `RayMeetsUseBox(sim, e, eye, dir, tick, inflate)`: step 1 on the use box inflated by `inflate`, within
   `kUseReach + inflate`.
4. `ApplyUse(sim, target, eye, T, lead, changes)` (T = the decision tick; the server's now, offline the tick):
   1. `m` = the target's Mechanism (missing -> NoTarget). Info target: `Newest().flags & (Locked | Jammed)` ->
      Locked, else NotUsable.
   2. `g` = for a Lever, the linked entity if it still has a Mechanism component, else none.
   3. Locked: `m` or `g` has Locked or Jammed in its newest flags -> Locked.
   4. Busy: `m.anyDecision && T - m.lastDecisionTick < kMechanismBusyTicks`, or the same for `g` -> Busy.
   5. Start: `s(x) = max(T + lead, x.Newest().startTick + 1)`; positions `p0 = PositionAt(x, s(x))`.
   6. Wanted target. Door: if `Newest().targetPos != 0` -> 0 (close) else open: `OneWay` -> maxPos; else side test:
      `eyeO = conj(R0) * (eye - spawnVoxel * vs) / vs` (original model voxels at p = 0); `pivot = pivotQ / 16`;
      `v = centre(useBox) - pivot`; `sdir = cross(axisVec(axis), v)`; `to = dot(eyeO - pivot, sdir) > 0 ? minPos :
      maxPos` (positive p swings the leaf along `sdir`; it must swing away from the eye). Lever: `to =
      Newest().targetPos == maxPos ? minPos : maxPos`; `g`'s `gTo = (to == maxPos) ? g.maxPos : g.minPos`.
   7. Closing wins, on the moving part (the door itself, or `g` for a lever): an open request (door `to != 0`; gate
      `gTo == maxPos`) while that part's newest segment is closing (`!IsOpening(start, target)`) and `T <
      ArrivalTick + kMechanismReopenTicks` -> Busy.
   8. Broken: a Lever with no `g` -> push the lever's segment (step 9), return Broken.
   9. Accept: for `m` (and `g`): `seg = {s(x), p0, to, SegmentDuration(p0, to, desc), Newest().flags}`;
      `PushSegment`; `lastDecisionTick = T`, `anyDecision = true`; append `MechanismChange{entity, seg}` (lever first,
      then its target). Return None.
5. `UsePrompt(sim, target, tick, greyed)`: info -> greyed `"Gate - opened by a lever"`, or greyed `"Locked"`;
   Door -> `"[E] Close"` if `Newest().targetPos != 0` else `"[E] Open"`; Lever -> `"[E] Lower gate"` if
   `Newest().targetPos == maxPos` else `"[E] Raise gate"`; Locked / Jammed -> greyed `"Locked"`.
6. `CheckMechanismAuthoring(t, vs, why)`: for each lever with `link >= 0`: G = the target posed at its initial
   position, `n` = horizontal unit of `R0 * axisVec(G.desc.pushAxis)`; `cG` = G's block box centre (world), `hG` =
   its half extent along n; `side = sign(dot(cL - cG, n))` with `cL` the lever's use box centre; outer face plane at
   `cG - n * side * hG`; distance from the lever's use box (8 posed corners, the smallest `|dot(corner - plane, n)|`)
   must be `>= kUseReach + 0.5`. Else false, `why = "lever I reachable through its gate (D m < 3.0 m)"`.
7. `CreateTemplateMechanisms(sim, t)`: `CheckMechanismAuthoring` fails -> `Log::Error("Mechanisms: template refused:
   {}", why)`, return {}. Else for each in order: `CreateVoxelObject({.model, .position = VoxelToWorld(spawnVoxel),
   .rotation = R0, .motion = Kinematic})`, `MakeMechanism(..., sim.Tick())`; then set `linked` from `link` indices.

Holding (steps 8-18; the declarations of "Skeleton", "Added on 2026-11-24"; `vs` = the voxel size, `dt` =
`Config().fixedTimeStep`):

8. **Layer `Held`** in physics as the skeleton says, `LayerMaskFilter` included. Nothing else in physics changes. In
   `StepMechanisms`' crush loop (card 3), skip entities with `scene::Held` (design "Drop, throw and auto-drop").
9. **`IsHoldableShape(sim, e)`**: `VoxelBody` and `RigidBody` present; no `Mechanism`, no `Held`; `s` = the model's
   volume size: `s[i] * vs <= kHoldMaxExtent` for i = 0..2; `n` = solid voxel count (the count `CreateBody`'s size
   rule uses): `scene.Config().debrisBodyVoxels <= n <= HoldMaxVoxels(vs)`. `HoldMaxVoxels(vs) =
   uint32(llround(double(kHoldMaxVolume) / (double(vs) * vs * vs)))` (to the nearest: `floor` gives 599 at 0.1f by
   float rounding). **`IsHoldable(sim, e, replicatedOnly)`**: `IsHoldableShape`, `RigidBody::motion == Dynamic`,
   and with `replicatedOnly` a `NetId` with `value != 0`. A client never calls `IsHoldable` (its copies are
   Kinematic and have no `NetId`): it passes its own hook (card 10).
10. **`PickupPathClear(sim, e, anchor)`** and the thick sweep (one internal helper `SweepSphere(sim, from, dir, r,
    maxT, tick)`, used here and by step 12): `r = max(0, 0.5f * (min(s) - 1) * vs)`; the voxels of the capsule's
    world AABB (from .. from + dir * maxT, grown by r), z, y, x ascending; for each solid voxel centre `c`: `w = c -
    from`, `b = dot(w, dir)`, `q = dot(w, w) - r^2`; inside already (`q <= 0`) -> 0; else if `b > 0` and `b^2 - q >=
    0`: entry `b - sqrt(b^2 - q)`; the minimum over voxels, and over every mechanism block box (posed at
    `PositionAt(tick)`, grown by `r / vs` voxels) the `RayBox` entry of step 1; `maxT` when none. `PickupPathClear`:
    `from = anchor`, `dir` and `maxT` = the unit vector and the distance to the object's centre; clear when the
    result is `maxT`.
11. **`FindUseTarget`** becomes a pass over candidate sources (steps 2.2-2.5 above stay the mechanism source):
    1. Candidates: (a) usable mechanisms as step 2.3, kind Mechanism; (b) bodies: `scene.QueryBodies(AABB of the
       segment eye .. eye + dir * kUseReach)`, each `IsHoldableShape(sim, e)` and the owner test (`hooks &&
       hooks->holdable ? hooks->holdable(e) : IsHoldable(sim, e, false)`) and `PickupPathClear(sim, e, eye -
       (0, kHoldDrop, 0))` -> ray against its posed model box `[0, s)` (its `Transform`), inflated by
       `kUseBoxInflate`, kind Holdable; (c) no rope source yet (a comment names the slot for `[mech-2]`).
    2. Occluders: terrain (`limit`), every mechanism's block box (not for its own use box), and every body with
       `Held` (its model box, not inflated; the caller's own held body never reaches here: E drops first).
    3. Winner: the smallest distance `t < limit` and below every occluder; ties by the lower kind value, then the
       lower key (`hooks && hooks->key ? hooks->key(e) : uint32_t(e)`; the mechanism source of step 2.3 uses the same
       key). None: the nearest occluder within `limit` that is a non-usable mechanism -> kind Info (`info = true`),
       or a held body -> kind HeldByOther; else false.
    4. Set `out.kind` in every case; `out.info = out.kind == Info`.
12. **`HoldPitch(p)`**: `int8_t(std::clamp<int32_t>(p, -kHoldPitchLimit, kHoldPitchLimit) / 256)`.
    **`HoldAnchor(feet, tuning)`**: `dvec3(feet) + (0, tuning.eyeHeight - kHoldDrop, 0)`.
    **`HeldPoseAt(sim, e, feet, yaw, hp, env, tick)`**:
    - `a = HoldAnchor(feet, env.tuning)`; `dir = LookDirection(yaw, int16_t(hp * 256))`.
    - `s` = model size; `h = 0.5f * length(vec3(s)) * vs`; `dMin = h + kHoldClearance`.
    - `t = SweepSphere(sim, a, dir, r, kHoldDistance, tick)` (step 10's `r`).
    - `d = clamp(t, dMin, kHoldDistance)`, `pinned = t < dMin`.
    - `centre = a + dvec3(dir) * d`; then `centre.x = clamp(centre.x, env.area.min.x, env.area.max.x)`, `centre.z =
      clamp(centre.z, env.area.min.y, env.area.max.y)` (unbounded offline: no change).
    - `rot = angleAxis(YawRadians(uint16_t(yaw + held.relYaw)), vec3(0, 1, 0))`; `pose.position = centre - dvec3(rot
      * (vec3(s) * vs * 0.5f))`; `pose.rotation = rot`.
13. **`RelYawAtPickup(R, yaw)`**: `f = R * (0, 0, -1)`; if `f.x^2 + f.z^2 < 0.25f` then `r = R * (1, 0, 0)`, `objYaw
    = atan2(-r.z, r.x)`, else `objYaw = atan2(-f.x, -f.z)`; return `uint16_t(QuantizeYaw(objYaw) - yaw)`.
    **`BeginHold(sim, e, holder, yaw, T, lastChangeTick, anyChange, stoodOn, replicated, out)`**: `anyChange && T -
    lastChangeTick < kHoldBusyTicks` -> Busy; `stoodOn` -> Busy; `!IsHoldable(sim, e, replicated)` -> (`Held` present ?
    Busy : NoTarget). Else `relYaw = RelYawAtPickup(R, yaw)`; emplace `Held{holder, relYaw, T, 0}`;
    `SetMotionType(Kinematic)` (keep `RigidBody::motion` Dynamic: it is the motion after the release);
    `SetLayer(Held)`; `SetVelocity(0, 0)`. `out = {e, true, holder, relYaw, T}`. Return None.
14. **`StepHeld(sim, e, holder, yaw, hp, env, tick, authority)`**: `p = HeldPoseAt(...)`;
    `physics.MoveKinematic(body, p.pose.position, p.pose.rotation, dt)`. `authority`: `pinnedTicks = p.pinned ?
    pinnedTicks + 1 : 0`; return `pinnedTicks >= kHoldPinnedTicks`. Else return false.
15. **`EndHold(sim, e, reason, pose, v, anchor, env, tick, replicated, out)`** (server and offline only; `out.holder` /
    `relYaw` from the `Held` component before it goes):
    1. Fix-up, only when `anchor != nullptr` (Drop, Throw, Pinned, HolderDead; Damaged and HolderGone pass nullptr):
       `Clear(pose)` = (a) no solid world voxel (`World::GetVoxel`) whose centre lies inside the oriented model box
       at `pose` (visit the voxels of its world AABB, z, y, x ascending); (b) the box meets no mechanism's block box
       at `PositionAt(tick)` (oriented boxes: the 15-axis separating-axis test); (c) `scene.QueryBodies(the box's
       world AABB)` has no other entity whose body is in `Layer::Dynamic` (resting bodies; moving Rubble is
       skipped) and whose `GetBounds` meets the box's world AABB; (d) no `env.playerFeet[i]` whose capsule AABB
       (`PlayerBounds(feet, env.tuning)`) meets the box's world AABB. Not clear: `c` = the pose's centre, `u =
       normalize(c - *anchor)`, `d = length(c - *anchor)`; for `d' = d - vs, d - 2 vs, ...` while `d' >=
       kHoldReleaseMin`: the pose moved to centre `*anchor + u * d'`; the first clear one is taken. None clear: the
       original pose.
    2. Body: `RemoveComponent<Held>`; `SetTransform(pose, teleport)`; `SetMotionType(Dynamic)`; `vq =
       QuantizeVelocity(v)` (move `net::QuantizeVelocity`'s formula to `sim` as `sim::QuantizeVelocity`, net keeps a
       forwarding call); `SetVelocity(vq, 0)`; `SetLayer(replicated ? Rubble : Dynamic)`; `Activate`.
    3. `out = {e, false, holder, relYaw, tick, uint8(reason), pose.position, pose.rotation, vq}`.
16. **Blast and replication:** `Blast` step 3: an entity with `Held` whose posed model box (oriented) meets the
    blast's sphere (the closest point of the box to `result.center` within `result.radius`) -> `EndHold(sim, e,
    Damaged, its current pose, its current linear velocity, nullptr, env-less: a default `HoldEnv{}`, sim.Tick(), it
    has a NetId, hc)`, `result.holdChanges.push_back(hc)`, then the damage as today; a held entity whose AABB the
    sphere box meets but whose model box the sphere misses is skipped (no release, no damage). Step 4 (push) and
    `MechanismBlast`'s push: skip entities with `Held`. `Replication.cpp`: `CountPieces` and `EvictionCandidate`
    skip entities with `Held`.
17. **`ReleaseVelocity(reason, holderVelocity, yaw, pitch)`**: Throw -> `holderVelocity + LookDirection(yaw, pitch) *
    kThrowSpeed` (the real pitch, not `HoldPitch`); every other reason -> `holderVelocity`.
    **`ApplyHoldChange(sim, e, c)`** (a client copy, which is and stays Kinematic): `c.hold` -> emplace `Held{c.holder,
    c.relYaw, c.tick, 0}`, `Layer::Held`, velocity 0; else remove `Held`, `SetTransform(c.position, c.rotation,
    teleport)`, velocity 0. No `SetMotionType`, no `Activate`, no fix-up (the server's pose is final). The layer after
    a release (`MovingLayer`), `Copy::resting` / `held` and the samples are the client's (card 10 step 12).
18. `UsePrompt`: kind Holdable -> `"[E] Pick up"`; HeldByOther -> greyed `"Held"`.
    `HoldPrompt()` returns `"[E] Drop   [LMB] Throw"`. The caller shows greyed `"Occupied"` instead of `"[E] Pick up"` when the target's body is the
    player's own ground body (`GetCharacterState(character).groundBody`).

**Tests** (the thirteen `Use.*` above are unchanged)
- `Use.TargetAtTheInputTick`: a door swinging open from tick 100; a ray that meets the leaf's pose at tick 110 and
  not at 120 hits at 110 and misses at 120 (and a second ray the reverse).
- `Use.TerrainOccludes`: a `FillBox` wall between eye and door: no target; removed: the door.
- `Use.ReachLimit`: the use box at 2.4 m: found; at 2.7 m: not.
- `Use.NearestOfTwo`: two doors on one ray: the nearer; equal distances: the lower key.
- `Use.SmallLeverIsUsable`: a 2 x 6 x 2 voxel lever is found.
- `Use.ClosedGateBlocksTheLeverBehindIt`: lever behind a closed portcullis, eye in front: no usable target (the
  gate is the Info target); gate raised (p = maxPos): the lever is usable.
- `Use.GateIsAnInfoTarget`: looking at a portcullis: `info == true`, `ApplyUse` -> NotUsable.
- `Use.DoorOpensAwayFromTheUser`: eye on +z -> segment target +1024 and the leaf's free end moves to -z; eye on -z ->
  -1024; a OneWay door -> +1024 from both sides.
- `Use.LeverDrivesItsTargets`: lever and gate segments share `startTick = T + lead`; lever up -> gate to maxPos,
  down -> minPos; with the gate freed (`ReleaseMechanism`) the lever still throws and the result is Broken.
- `Use.RefusalsInOrder`: Locked (desc Locked), Busy (second use 5 ticks after the first), Broken (lever without
  target), NotUsable (info target) - each in a setup where the earlier refusals do not apply; NoTarget for a ray into
  the sky. (Stale is the server's: `Use.RefusalReachesTheUser`, card 9.)
- `Use.OpenCannotInterruptAClose`: gate closing; an open request 15 ticks later and 10 ticks after its arrival is
  Busy; 21 ticks after arrival it is accepted.
- `Use.CloseInterruptsAnOpen`: door opening; a close 13 ticks later is accepted and starts at the exact position.
- `Use.AuthoringRuleRejectsAReachableLever`: a template with the lever 2 m from the gate's outer face:
  `CreateTemplateMechanisms` returns empty; at 3.5 m it makes both.

Holding (suite `Hold`, written enabled in this card). Fixture `HoldYard` (a helper prefix, not an engine name): a
`Simulation` at voxel size 0.1, stone ground `FillBox` up to y = 10.0 m over x, z in [-8, 8] m, a crate =
`CreateVoxelObject(CrateModel(m, PlaygroundScale(0.1)))` (8^3 voxels, so `h = 0.5 * sqrt(192) * 0.1 = 0.69282`,
`dMin = 0.79282`, sweep radius `r = 0.35`), holder feet (0, 10, 0), default `PlayerTuning` (eye y 11.62, anchor y
11.12), `HoldEnv{tuning, PlayArea{}, {}}`, yaw 0 and pitch 0 (look -z) unless stated. A body placed in the air is not
stepped by physics unless the test says so.
- `Hold.CrateIsHoldableBigThingsAreNot`: `CrateModel(m, PlaygroundScale(vs))` at voxel sizes 0.1, 0.125, 0.15: true.
  False for: a 13 x 4 x 4 planks box at 0.1 (1.3 m), a solid 9^3 stone cube at 0.1 (729 > `HoldMaxVoxels(0.1)` =
  600), a 20-voxel piece at 0.1 (under `debrisBodyVoxels` 48), a door mechanism, a body with `motion` Kinematic, a
  crate with `Held`; `replicatedOnly` true on a crate without `NetId`: false. A crate made Kinematic (as a client
  copy): `IsHoldableShape` true, `IsHoldable` false. `HoldMaxVoxels(0.1) == 600`, `(0.125) == 307`, `(0.15) == 178`.
- `Hold.NearestTargetWins`: a lever (2 x 6 x 2) with its use box 2.0 m ahead and the crate 1.2 m ahead on the same
  ray: kind Holdable, the crate; the crate moved 2.4 m ahead (behind the lever): kind Mechanism; the crate moved so
  that its inflated box and the lever's inflated use box are entered at the same distance (equal within 1e-6): kind
  Mechanism (the lower kind wins a tie).
- `Hold.ClosedDoorHidesACrateBehindIt`: a closed door 1.0 m ahead, the crate 2.0 m ahead: kind Mechanism (the door);
  the door at p = maxPos (open, out of the ray): the crate.
- `Hold.HeldByAnotherIsBusy`: `BeginHold(crate, holder 1)`; holder 0's ray meets the crate, a door behind it: kind
  HeldByOther, not the door.
- `Hold.CannotPassASlit`: a stone wall x [-2, 2], y [10, 13], z [-2.0, -1.6) m with a vertical slit x [-0.1, 0.1)
  carved through it; the crate's centre at (0, 11.62, -2.4): the eye ray passes the slit and meets the crate, but
  `PickupPathClear` is false and `FindUseTarget` gives kind None; the slit filled, then re-carved 1.0 m wide (x [-0.5,
  0.5)): kind Holdable. The crate held, looking through the 0.2 m slit: `distance` 1.33520 within 1e-3 (the slit's
  edge voxels at x = +-0.15 stop the sphere), the centre's z above -1.6 (the near side).
- `Hold.CannotPickUpWhatAPlayerStandsOn`: `BeginHold(..., stoodOn = true, ...)` -> Busy and no `Held`; `stoodOn =
  false` -> None.
- `Hold.PoseFollowsTheHolder`: `relYaw` 0 (crate unrotated, holder yaw 0): centre (0, 11.12, -1.5) and position
  (-0.4, 10.72, -1.9) within 1e-4, `pinned` false; holder yaw 16384: centre (-1.5, 11.12, 0) within 1e-4 and rotation
  = `angleAxis(pi/2, +y)` within 1e-5; pitch 20000: `HoldPitch == 64`, centre (0, 12.18066, -1.06063) within 1e-3;
  pitch -20000: `HoldPitch == -64`. `RelYawAtPickup` of the crate turned by 90 deg about +y, holder yaw 0: 16384.
- `Hold.WallClampsTheDistance`: stone `FillBox` x [-2, 2], y [10, 13], z [-2.6, -1.6) m: `distance` 1.30489 within
  1e-3 (the nearest voxel centre (+-0.05, 11.15, -1.65) enters the 0.35 m sphere), `pinned` false; the wall at z
  [-2.2, -1.0): `distance` 0.79282, `pinned` true; terrain removed and a closed door whose block box face is 1.6 m
  ahead: `distance` 1.25 within 1e-3 (the box grown by `r`).
- `Hold.PlayAreaClampsTheHold`: `env.area = {(-1, -1), (1, 1)}`: centre (0, 11.12, -1.0) within 1e-4.
- `Hold.HeldBodyMeetsNothing`: the crate held and posed through a second, resting crate and through the holder's own
  capsule; 60 ticks of `StepHeld` + `StepPhysics`: the second crate's position is unchanged within 1e-4 and its
  speed stays 0; a second player walking (moveZ 127) through the held crate's place moves more than 3.0 m in the 60
  ticks; `physics.GetLayer` is Held; `physics.Raycast(eye, -z, 5)` (the default mask) does not hit the held crate's
  body, and `Raycast(eye, -z, 5, LayerBit(Held))` does.
- `Hold.DropKeepsPoseAndHolderVelocity`: `EndHold(Drop, pose, (2, 0, 0), &anchor, env, T, false, c)` in open air: no
  `Held`, motion Dynamic, layer Dynamic (with `replicated` true: Rubble), velocity exactly (2, 0, 0), position exactly
  `pose.position`; `c.hold == false`, `c.reason == 1`, `c.tick == T`.
- `Hold.ThrowAddsSixAlongTheLook`: `ReleaseVelocity(Throw, (1, 0, 0), 0, 0)` == (1, 0, -6) within 1e-5; with pitch
  32767 (straight up): (1, 6, 0) within 1e-4; `ReleaseVelocity(Drop, ...)` == the holder's velocity.
- `Hold.ReleaseStepsOutOfAWall`: the wall at z [-2.2, -1.0), the pinned pose (d 0.79282): after `EndHold(Pinned, ...,
  &anchor, ...)` the centre is at z -0.59282 within 1e-3 (two steps of 0.1 m back) and the box holds no solid voxel;
  with `anchor == nullptr` the pose is unchanged.
- `Hold.ReleaseAvoidsBodiesAndPlayers`: a second crate resting (`Layer::Dynamic`) with its centre at (0, 11.12,
  -1.5): the release of the held crate at d 1.5 steps back until the two boxes' AABBs do not meet (centre z >= -0.7
  within 1e-3); the second crate removed and `env.playerFeet = {(0, 10.5, -1.5)}`: the release steps back until the
  box's AABB does not meet that capsule's `PlayerBounds`.
- `Hold.PinnedForThirtyTicksDrops`: the wall at z [-2.2, -1.0): `StepHeld(authority)` returns false for calls 1-29
  and true at call 30; with one unpinned call (holder yaw 32768: looking away) at call 15, still false at call 30,
  false at call 44 and true at call 45.
- `Hold.BusyAfterAChange`: `BeginHold(..., T, lastChangeTick = T - 11, anyChange = true, ...)` -> Busy; `T - 12` ->
  None; `anyChange = false` -> None.
- `Hold.DamageReleasesFirst`: a power-6, 1.2 m `Blast` centred on the held crate: `holdChanges` has one release with
  reason 5 (Damaged); the surviving entity (if any) has no `Held` and motion Dynamic.
- `Hold.PushDoesNotReleaseOrMove`: a `Blast` 2.0 m from the held crate (its sphere box misses the crate's bounds, its
  push box meets them): the crate keeps `Held`; after `StepHeld` + `StepPhysics` its position equals the hold pose
  within 1e-4. A second `Blast` placed off a corner of the crate, so that its sphere's AABB meets the crate's bounds
  but the sphere misses the model box: the crate keeps `Held`, no release in `holdChanges`, no voxel removed.
- `Hold.CrushSkipsAHeldBody`: a door closing over 72 ticks; the crate held with its box inside the door's block box
  for the last 6 ticks: `StepMechanisms(authority)` crushes 0; the crate released there and the door closed again:
  crushed at arrival (`crushed == 1`).
- `Hold.TwoSimulationsAgree`: two fixtures, the same `BeginHold`, then 120 ticks of a scripted holder (walk forward,
  yaw +200 per tick, pitch sweeping -20000..20000) with `StepHeld` + `StepPhysics`, then `EndHold(Throw)`: the held
  body's transform equal bit for bit at every tick, and the two release `HoldChange`s equal.

**Checks**
1. build; 2. `tools/ctest.sh -R 'Use\.|Mechanism\.|Hold\.|Blast\.|Replication\.|Physics' --output-on-failure`;
3. `tools/ctest.sh`; 4. doc-check (the ROADMAP test count).

**Must not break**
- `Blast.*`, `Replication.*` (ordinary objects: the held branch never runs without `Held`); `Physics.*` (the new
  layer changes no other pair); `Mechanism.*` of card 3.

**Done when**
- [ ] The thirteen `Use.*` tests and the twenty `Hold.*` tests pass.

**Traps**
- `RigidBody::motion` stays Dynamic while held (only the physics motion type is Kinematic): `IsHoldable` reads the
  component, and `Held` excludes a held body. A client copy's `RigidBody::motion` is Kinematic from its creation:
  never call `IsHoldable` for a client (card 10 passes its own hook).
- The crush loop reads `RigidBody::motion == Dynamic`: without the `Held` skip it despawns a held crate.
- A blast or the rubble caps can destroy a held entity: every cache of "the held entity" checks
  `registry.valid(e)` before `all_of<scene::Held>(e)`.
- `Blast.cppm` needs `<glm/gtc/quaternion.hpp>` in its global fragment for `HoldChange`.
- `sim` cannot import `net`: put the velocity quantisation in `sim::QuantizeVelocity` (the formula of
  `net::QuantizeVelocity`, 1/256 m/s, int16 per axis) and make the net one forward to it.
- Every float here (eye, ray) decides only which mechanism and which side: server-decided and sent as integers, so
  it is allowed. The segment itself is integers.
- Do not use `PhysicsWorld::Raycast` for targets (design "No physics raycast"): bodies other than mechanisms never
  block a use.

## Card 5: The damage rule

| | |
|---|---|
| Goal | Doors wear down hit by hit and fall off their hinges; a cast at the wall beside a lever leaves the lever |
| Depends on | card 3 |
| Effort | 1 d |
| Touches | scene (`Scene.cpp`), sim (`Blast.cpp`, `Mechanism.cpp`); wire: no |

**Read first** (about 50 KiB)
- mechanisms.md "Destruction interplay" (both subsections).
- `engine/src/scene/internal/Scene.cpp` `Scene::Damage` (all of it); `engine/src/sim/internal/Blast.cpp` steps "3." and
  "4." of `Blast` (quoted: `// 3. Objects caught in the blast.` and `// 4. Push bodies outwards`).
- `engine/src/voxel/World.cppm` `QuantizePower` (the hardness rule `hardness * 16 > power` survives).

**Steps**
1. `Scene::Damage(entity, center, radius, options)`: the old body moves here unchanged, plus:
   1. In the carve loop, after the `Indestructible` test: `if (options.powerQ != 0xFFFF && material.hardness * 16.0f >
      float(options.powerQ)) continue;` (the same float test `World::CarveSphereQ` uses).
   2. After `SplitComponents`: `keptIndex = 0`; with `keepByBox`, for each piece i count its solid voxels `v` with
      `v + piece.offset` inside `[keepMin, keepMax)`; `keptIndex` = the largest count (ties: lower i);
      `result.keptBoxVoxels` = that count.
   3. Replace `const bool kept = i == 0;` with `i == keptIndex`. With `keepMotion` the kept piece's motion is the old
      `motion` even when `splitApart`, and the fragment branch never takes the kept piece.
   4. `Damage(entity, center, radius)` becomes `return Damage(entity, center, radius, DamageOptions{});`.
2. `DistanceToMechanism(sim, e, point)`: point into current model voxels (pose at `PositionAt(sim.Tick())`); clamp to
   the model box `[0, size)`; distance in voxels x vs.
3. `DamageMechanism(sim, e, centre, powerQ, into)`:
   1. `Warded` -> return {}.
   2. `keep = hingeBox - modelOffset`; `r = scene.Damage(e, centre, kMechanismHitRadius, {powerQ, true, keep.min,
      keep.max, true})`. `removed == 0` -> return {}.
   3. `into.objectsHit++`, `into.objectVoxels += removed`, fragments appended (with `BlastPush(centre, 0.3, pos)`),
      new pieces handled exactly as Blast step 3 does (replication: `TakePieceSlot` -> `ObjectEvent::Spawn`, else
      fragment and `Destroy`). Extract that piece-handling block of `Blast` into an internal helper
      `HandleDamagePieces(...)` used by both, unchanged for ordinary objects.
   4. Survivor (`!r.destroyed`): `modelOffset += r.keptOffset`; `physics.SetMotionType(Kinematic)` if it differs;
      pose at `PositionAt(tick)` re-applied; `SetLayer(MechanismLayer)`. Replicated: an `ObjectEvent::Model` with
      `motion = Kinematic`, as Blast makes for objects.
   5. `r.keptBoxVoxels == 0` (or `r.destroyed`): Lever -> `ReleaseMechanism(..., Broken, into.mechanismChanges)`,
      `broken = true`; else `ReleaseMechanism(..., Freed, into.mechanismChanges)`, `freed = true`.
4. `Blast` step 3: an entity with `scene::Mechanism`: if `DistanceToMechanism(sim, e, result.center) >
   kMechanismHitRadius` skip it; else `DamageMechanism(sim, e, result.center, power, result)` instead of
   `Scene::Damage`. Step 4 (push): skip mechanism entities. (`Blast.cpp` is an implementation unit of `vortex.sim`
   and sees `:mechanism`.)
5. `MechanismBlast(sim, target, sphere, power)`: result.center / radius from the sphere (as `Blast` sets
   them), `islandsSearched = true`, no op; `DamageMechanism(sim, target, center, power, result)`; then the
   push of step 4 to every non-mechanism body (same `BlastPush`, same wake box). Return it.

**Tests**
- `Mechanism.DentedKeepsWorking`: a hit at the door's middle: voxels removed, still a mechanism, still opens (a use
  moves it to maxPos).
- `Mechanism.DentThatCropsTheModelKeepsThePivot`: hits that empty the model's minimum-corner column (`keptOffset !=
  0`); for p in {-1024, -512, 0, 512, 1024} the world position of a surviving hinge voxel is the same before and
  after within 1 mm.
- `Mechanism.SplitDoorKeepsItsHingedPart`: a cut across a 3-voxel-thick door: the piece with the hinge-box voxels
  keeps the entity (Kinematic, `Mechanism` component), the other is a new Dynamic piece.
- `Mechanism.HingeGoneFrees`: hits on the hinge box until it is empty: `freed`, no `Mechanism` component, Dynamic.
- `Mechanism.AnchorAirFrees`: `FillBox` both anchors to air: the next `StepMechanisms(authority)` frees it and
  returns one change with `MechanismFlag_Freed`.
- `Mechanism.CastAtTheGateLeavesTheLeverBesideIt`: a 1.2 m / power 6 `Blast` on the wall 0.8 m beside a lever:
  the lever keeps every voxel.
- `Mechanism.DoorTakesNHits`: `MechanismBlast` power 6 at the leaf centre plus 0.25 m steps along its width until
  freed: freed after 3 to 8 hits at 0.125 m (log the number: `Mechanism hits to free a door: N`; card 7 records it).
- `Mechanism.MechanismHitCarvesNoTerrain`: `MechanismBlast` at a door in a stone frame: `WorldHash` unchanged.
- `Mechanism.WardedTakesNoDamage`: a Warded door: `Blast` and `MechanismBlast` remove nothing.
- `Mechanism.TwoSimulationsFedTheSameInputsAgree`: two simulations, the same uses (same ticks, leads, eyes) and the
  same `Blast` / `MechanismBlast` calls: equal segment rings, flags, `modelOffset`, model volumes and `WorldHash`.

**Checks**
1. build; 2. `tools/ctest.sh -R 'Mechanism\.|Use\.|Blast\.|Scene\.|Replication\.' --output-on-failure`;
3. `tools/ctest.sh`; 4. doc-check; 5. `VERIFY_SLOT=1 tools/verify.sh quick` (G3-G10 unchanged: no mechanism exists
   in the world yet).

**Must not break**
- Ordinary objects: `Scene.*`, `Replication.DamagedObjectsGiveModelsSpawnsAndFragments`, G7 (`Probe piece 0 keeps
  its push`), G8-G10 lines identical.
- `Blast.TwoSimulationsFedTheSameOperationsHaveEqualHashes`.

**Done when**
- [ ] Ten tests pass; verify quick PASS.

**Traps**
- The keep box is in the CURRENT model's voxels (`hingeBox - modelOffset`); a dent shifts it.
- Never let `CreateBody`'s size rule put a dented lever in `Debris`: re-apply `MechanismLayer` after every damage.

## Card 6: Offline input and the HUD

| | |
|---|---|
| Goal | In the offline game E uses, picks up and drops; LMB while holding throws; Q drops a crate; the HUD shows what E will do |
| Depends on | cards 4, 5 |
| Effort | 1 d |
| Touches | sim (`kPlayerButtonMask`), game (`src/main.cpp`, `src/Session.cpp`); wire: no (no client sends the bit to a server that reads it yet); movement rule: no |

**Read first** (about 40 KiB)
- `src/main.cpp`: `OnFixedUpdate`, the `Key::E` line in `OnUpdate`, the left-click block, `DrawHud`.
- `src/Session.cpp`: the `Key::E` latch, the scripted crate latch, the session tool line.
- mechanisms.md "E becomes use, the crate moves to Q", "What the predicting client shows at once", "The prompt".

**Steps**
1. `engine/src/sim/Player.cppm`: `kPlayerButtonMask = 31`.
2. `src/main.cpp` `OnUpdate`: `Key::E` pressed (play keys) -> `m_ButtonLatch |= sim::PlayerButton_Use`; `Key::Q`
   pressed -> today's `SpawnCrate(...)` call. Tool line: `"Tool: Blast [LMB]   Use [E]   Crate [Q]"`.
3. Left click on an object hit: if the hit entity (`Scene::EntityFromUserData(object.userData)`) has
   `scene::Mechanism`: `engine.PresentBlast(sim::MechanismBlast(sim, e, voxel::QuantizeSphere(glm::dvec3(object.position)
   / vs, 0.6 / vs), voxel::QuantizePower(6.0f)))` - the object blast's 0.6 m sphere, quantised as `Blast(sim,
   BlastDesc)` does; else today's `engine.Explode`. Its `mechanismChanges` need nothing offline.
4. `OnFixedUpdate` (restructured so the mechanism step also runs while flying):
   ```
   tick = sim.Tick()
   if walking:
       PoseMechanisms(sim, tick, PlayerReach(m_Player.position, m_Tuning))
       PushOutOfMechanisms(sim, tick, m_Player, m_Character, m_Tuning)
       r = StepPlayer(...)                                   // unchanged call
       if (r.pressed & PlayerButton_Use):
           eye = m_Player.position + (0, eyeHeight, 0); dir = LookDirection(input.yaw, input.pitch)
           if FindUseTarget(sim, eye, dir, tick, t): m_UseResult = ApplyUse(sim, t, eye, tick, kOfflineMechanismLead, changes)
           else m_UseResult = NoTarget
           m_UseResultTick = tick; for each change: SpawnLeadCue(change)
   result = StepMechanisms(sim, tick, true)
   for each p in result.crushedAt: a puff (fx::SpawnVoxelBreak x 6, grey)
   ```
   `SpawnLeadCue(change)`: 6 dust particles (`fx::SpawnVoxelBreak`, wood colour, upward 1 m/s) at the mechanism's
   posed pivot (door, lever) or the block box's top centre (portcullis).
5. HUD (in `DrawHud`, under the crosshair, play mode only): each frame `FindUseTarget(sim, cam.position,
   cam.Forward(), sim.Tick(), t)`; hysteresis: if the last target is still met by `RayMeetsUseBox(..., kPromptHysteresis)`
   keep it. Text from `UsePrompt`; greyed = `Rgba(160, 160, 160, 200)`, else `Rgba(255, 255, 255, 230)`. For 60
   frames after a refusal (`m_UseResult != None`) show `ToString(m_UseResult)` capitalised instead ("Out of reach",
   "Locked", "Busy", "Broken", "Not usable").
6. `src/Session.cpp`: the E latch becomes `PlayerButton_Use`, Q latches `PlayerButton_Crate`; tool line `Blast [LMB]
   Use [E]   Crate [Q]`; the scripted crate latch stays `PlayerButton_Crate`. (A session server ignores the Use bit
   until card 9.)
7. **Holding, offline** (`src/main.cpp`; members `m_Held` (entity, `kNoEntity` when none), `m_HoldChangeTick`,
   `m_AnyHoldChange`, `m_ThrowLatch`, `m_HoldNotice` / `m_HoldNoticeTick` for the auto-drop line). In step 4's block,
   replacing the `r.pressed & PlayerButton_Use` branch (`env = HoldEnv{m_Tuning, PlayArea{}, {}}`: offline the play
   area is unbounded and no other player exists):
   ```
   if m_Held != kNoEntity and (!registry.valid(m_Held) or !registry.all_of<scene::Held>(m_Held)):
       m_Held = kNoEntity                       // a blast released or destroyed it, or the rubble caps removed it
   eye, dir as in step 4; anchor = HoldAnchor(m_Player.position, m_Tuning); hp = HoldPitch(input.pitch)
   if (r.pressed & Use) or m_ThrowLatch:
       if m_Held != kNoEntity:
           reason = m_ThrowLatch ? Throw : Drop             // a Use and a throw in one tick: one throw
           pose = HeldPoseAt(sim, m_Held, m_Player.position, input.yaw, hp, env, tick).pose
           EndHold(sim, m_Held, reason, pose, ReleaseVelocity(reason, m_Player.velocity, input.yaw, input.pitch),
                   &anchor, env, tick, false, c); m_Held = kNoEntity; m_HoldChangeTick = tick; m_AnyHoldChange = true
       else if (r.pressed & Use) and FindUseTarget(sim, eye, dir, tick, t):      // no hooks offline
           Holdable: stoodOn = (physics.GetCharacterState(m_Character).groundBody == the target's body)
                     m_UseResult = BeginHold(sim, t.entity, 0, input.yaw, tick, m_HoldChangeTick, m_AnyHoldChange,
                                             stoodOn, false, c)
                     on None: m_Held = t.entity, m_HoldChangeTick = tick, m_AnyHoldChange = true
           Mechanism / Info: ApplyUse as step 4
       else if (r.pressed & Use): m_UseResult = NoTarget
       m_ThrowLatch = false
   if m_Held != kNoEntity and StepHeld(sim, m_Held, m_Player, input.yaw, hp, env, tick, true):
       EndHold(Pinned, the same pose, m_Player.velocity, &anchor, env, ...); m_Held = kNoEntity; m_HoldChangeTick = tick
       m_HoldNotice = "Dropped - blocked"; m_HoldNoticeTick = tick
   ```
   This runs after `StepPlayer` and before `StepMechanisms` (and so before the engine's `StepPhysics`). Offline the
   floor rule reads only the current ground body: no client replays exist. A `Damaged` release (the first line finds
   `m_Held` without `Held` while the entity is valid) sets `m_HoldNotice = "Dropped - hit"`.
8. **LMB while holding:** in `OnUpdate`'s left-click block, when `m_Held != kNoEntity`, set `m_ThrowLatch = true` and
   neither `engine.Explode` nor `MechanismBlast` runs.
9. **HUD:** while `m_Held != kNoEntity` the prompt is `HoldPrompt()` (white; amber `Rgba(255, 190, 60, 230)` while
   `HeldPoseAt(...).pinned`) and no target search runs; else step 5's prompt, where `UsePrompt` now also gives
   `[E] Pick up` and greyed `Held`, and greyed `Occupied` for the body under the player's feet (card 4 step 18). For
   60 frames after an auto-drop, `m_HoldNotice` replaces the prompt.
10. **`--hold-script`** (headless): frame 40, the scripted player 1.2 m south of the playground's first template crate
    (the first `TemplateObject`; log its position), looking at it: a scripted Use edge; frames 41-160 walk north 5 m
    (moveZ 127) holding it; frame 161 Use edge (drop); frame 200 Use edge (pick up again); frame 210 a scripted throw.
    Log `Hold: picked up at tick N`, `Hold: dropped at (x, y, z)` (the centre of the crate's posed model box, not its
    shape origin), `Hold: thrown at V m/s`, and `Expect`: both pick-ups `None`, the dropped centre within 1.6 m of the
    holder's feet (x, z) at the drop, the crate's speed after the throw over 5 m/s. Add the flag and its lines to
    `docs/verify.md`.

**Checks**
1. build; 2. `tools/ctest.sh`; 3. doc-check; 4. `VERIFY_SLOT=1 tools/verify.sh quick`; look at G3's screenshot: the
   tool line reads `Use [E]   Crate [Q]`.
5. `./build/Shredborne --headless --validation --frames 260 --hold-script` - exit 0 and its three lines.

**Docs**
- `docs/owner-checks.md` (game): check 14's crate key E -> Q; `docs/two-pc-test-windows.md` checklist 6.3: "Q drops a
  crate 3 m ahead"; `README.md` "Controls": E use (doors, levers, pick up / drop), LMB while holding throws, Q crate;
  `engine/docs/design/game-ui.md` keys and the prompts; `docs/verify.md` `--hold-script`.

**Must not break**
- G3 log lines (`Beam cut`, `HUD: N quads`, `on the ground`); `Movement.*` (the mask change makes no edge from bits
  that never come).

**Done when**
- [ ] Offline E calls `ApplyUse`, picks up and drops; LMB while holding throws; Q drops a crate; `--hold-script`
  exits 0; verify quick PASS; docs updated.

**Traps**
- Windowed behaviour (the prompt under a real mouse, the held crate in view, the throw) is an owner check: say so in
  the report.
- `m_Held` is a cache: a blast can release the crate inside `engine.Explode` (`Damaged`); check the `Held` component
  every tick before using it.

## Card 7: The yard in the offline game

| | |
|---|---|
| Goal | The default offline game has a hut with a door and a wall with a portcullis and its lever; a headless run shows the door open and the gate up |
| Depends on | card 6 |
| Effort | 1 d |
| Touches | sim (`Region.cpp`), game (`src/main.cpp`); wire: no |

**Read first** (about 45 KiB)
- `engine/src/sim/internal/Region.cpp` `PlaygroundTemplate` (the `box` lambda, `S`, the tower at x 30..58, z -62..-30
  in 0.1 m units, the timber frame, the crates).
- `src/main.cpp` `BuildPlayground`, the `--camera` parsing, `m_ProbeCamera`, the script frame handling of
  `--tower-blast`, `Expect` (exit 3).
- mechanisms.md "How they are placed".

**Steps**
1. `MechanismYardTemplate(world, m, c)` (c = the plaza anchor; 0.1 m units scaled by `S` exactly as
   `PlaygroundTemplate` does; the yard lies north, at -z, clear of the tower):
   - Pad: `box({-70, 0, -150}, {60, 90, -84}, 0)`; `box({-70, -30, -150}, {60, 0, -84}, m.stone)`;
     `box({-70, -1, -150}, {60, 0, -84}, m.stoneDark)`.
   - Hut: `box({-60, 0, -140}, {-20, 30, -110}, m.brick)`; `box({-57, 0, -137}, {-23, 30, -113}, 0)` (3-voxel walls at
     0.1 m); roof `box({-62, 30, -142}, {-18, 32, -108}, m.planks)`; doorway `box({-45, 0, -113}, {-36, 21, -110}, 0)`.
   - Gate wall: `box({-10, 0, -100}, {50, 40, -97}, m.brick)`; passage `box({5, 0, -100}, {35, 30, -97}, 0)`; slot
     above it for the rising gate: `box({5, 30, -99}, {35, 40, -98}, 0)`.
   - Lever post: `box({39, 0, -136}, {41, 10, -134}, m.wood)`.
   - Models, built in WORLD voxels (`V(metres) = max(1, lround(metres / vs))`): door `V(0.9) x V(2.1) x 3` of
     `m.planks` with an `m.iron` column at its hinge edge (x = 0); portcullis `V(3.0) x V(3.0) x 1` of `m.wood`, solid
     where `x % 3 == 0 || y % 4 == 0 || y == 0`; lever `2 x 6 x 2` of `m.wood`.
   - Door: `spawnVoxel = c + S({-45, 0, -113})` adjusted so the leaf's 3 voxels sit inside the 3-voxel wall; hinge on
     its -x edge: `pivotQ = (0, 0, 24)`, axis 2 (+y), range -1024..1024, `openTicks = closeTicks = 72`, flags
     `Usable`, useBox = blockBox = the whole model, hingeBox = `x in [0, 1)`, pushAxis 4 (+z), anchors = the two wall
     voxels at `spawnVoxel + (-1, 2, 1)` and `spawnVoxel + (-1, height - 3, 1)`.
   - Portcullis: `spawnVoxel = c + S({5, 0, -99})`; axis 2, range `0 .. V(2.5) * 16`, `openTicks = 180`, `closeTicks
     = 45`, no Usable flag, boxes = the whole model, hingeBox = the top row, pushAxis 4, no anchors.
   - Lever: `spawnVoxel` on top of the post (`c + S({39, 10, -136})`), hinge at its base: `pivotQ = (16, 0, 16)`,
     axis 0 (+x), range 0..512, 12 / 12 ticks, `Usable`, useBox = blockBox = whole, hingeBox = `y in [0, 1)`,
     pushAxis 4, `link` = the portcullis's index, initial 0.
   - Order in `mechanisms`: door 0, portcullis 1, lever 2 (the log's indices).
   - Check: the authoring rule holds (the lever's use box is about 3.6 m behind the gate's outer face at z -97). If
     any yard box overlaps a playground box (the tower ends at z -62 in 0.1 m units, the yard starts at -84), stop and
     report.
2. `src/main.cpp`: `BuildPlayground` builds the yard after the plaza unless `--no-mechanisms`: `yard =
   MechanismYardTemplate(...)`; `ApplyTemplateOps(sim, yard)`; `m_Yard = CreateTemplateMechanisms(sim, yard)`; log
   `Mechanisms: yard built: 3 mechanisms (door 0, gate 1, lever 2) at (x, y, z) m`.
3. `--camera yard`: from 4 m south of the gate passage, eye height, looking north (-z) at the gate with the hut door
   in view (pick the pose from the template coordinates; log it).
4. `--mechanism-script` (headless): frame 40: a scripted use of the door (eye 1.5 m in front of its use box centre,
   looking at it: `FindUseTarget` + `ApplyUse`); frame 60: the same for the lever; frame 260: log
   `Mechanisms: door 0 open at tick N` (p == maxPos or minPos), `gate 1 raised at tick M` (p == maxPos), `R refused`
   (R must be 0); `Expect` each (exit 3 when not).
5. `--mechanism-blast` (headless): from frame 40, every 15 frames a `MechanismBlast` at the door's leaf (centre, then
   0.25 m steps along its width) until freed, then at the lever until broken; log `Mechanisms: door 0 freed after N
   casts`, `Mechanisms: lever 2 broken after K casts`, `Mechanisms: ObjectModel bytes per hit min A max B` (the
   `net::WriteVolumeRuns` size of the survivor; the game links Net); `Expect` both happened by frame 400.
6. Exit summary line (every run with mechanisms): `Mechanisms: N, moving max M, uses U (refused R), lead min A max B,
   crushed K, step max S us, pose max P us, use query max Q us` (times measured with `std::chrono` around the calls).
7. Re-run G3-G10 by hand (`VERIFY_SLOT=1 tools/verify.sh quick`). If a G7-G10 number moves because of the yard (the
   neck tower's fall, `World: generation`, store counts), record the new value in CLAUDE.md's gate table in this
   card, with one line of why; if a script cannot stand the yard, give that script `--no-mechanisms` implicitly in
   `main()` and say so.

**Log lines and flags**
- `--no-mechanisms`, `--mechanism-script`, `--mechanism-blast`, `--camera yard`: add them to `docs/verify.md` under
  the game's flags with their lines (no gate: they are stage checks).

**Checks**
1. build; 2. `tools/ctest.sh`; 3. doc-check; 4. `VERIFY_SLOT=1 tools/verify.sh quick`;
5. `./build/Shredborne --headless --validation --frames 300 --mechanism-script --camera yard --screenshot
   build/yard.png` - exit 0, the three lines; look at `build/yard.png`: the door open, the gate up (it shows above the
   wall);
6. `./build/Shredborne --headless --validation --frames 420 --mechanism-blast` - exit 0; record N and K in the report
   and in mechanisms.md "Budgets and costs" (measured);
7. G6's sizes: step 5 with `--voxel-size 0.1` and `0.15` - exit 0.

**Docs**
- `docs/verify.md` (flags and lines); CLAUDE.md only if a gate number moved; mechanisms.md: the measured hits.

**Done when**
- [ ] The yard is in the default game; both scripts exit 0 with their lines; the screenshot looked at; G3-G10 PASS.

**Traps**
- Build the yard after `BuildPlayground`'s `LoadBlocking`: its boxes must be loaded (the plaza's 16 m load covers the
  pad within about 15 m; if `ApplyTemplateOps` logs an incomplete op, load around the yard centre first with
  `World::LoadBlocking`).

## Card 8: Wire, protocol 12

| | |
|---|---|
| Goal | Mechanisms, their moves and the use result travel; old builds refuse the new one |
| Depends on | card 7 |
| Effort | 1 d |
| Touches | net (`Wire`, `Messages`); wire: **yes** (`kProtocolVersion` 12) |

**Read first** (about 45 KiB)
- `engine/src/net/Wire.cppm` `EventKind`, `EntityKind`, `ObjectSpawn`, `EntitySpawn`, `Event`; `Wire.cpp` the
  EntitySpawn encoder / decoder.
- `engine/src/net/Messages.cppm` `kProtocolVersion` (its comment), the owner block (`lastCastTick`, `castRejected`,
  `kSnapshotOwnerBytes`, and the fields `[wp-7]` added), `BaselineEntity`, `EntityCheck`, `EntityListEntry`.
- `engine/docs/notes/net.md` section 2 (the registry) and "Recipes" (new event kind).

**Steps**
1. `Wire.cppm`: `EventKind::MechanismMove = 37`; `EntityKind::Mechanism = 3`; `struct MechanismMove { uint32_t netId
   = 0; scene::MechanismSegment segment; }` (16 B with its kind byte); `Event` gains `MechanismMove`.
2. `EntitySpawn` gains `MechanismState mechanism;` used when `kind == Mechanism`: `struct MechanismState {
   scene::MechanismDesc desc; glm::ivec3 spawnVoxel; uint8_t quarterTurns; glm::ivec3 modelOffset; uint32_t
   linkedNetId; std::vector<scene::MechanismSegment> segments; }` (segments oldest first, 1..8).
3. Encoding (little endian, as the rest): after the `ObjectSpawn` bytes of the object: desc = kind u8, flags u8,
   pivotQ 3 x i16, axis u8, minPos i16, maxPos i16, openTicks u16, closeTicks u16, autoReturnTicks u16, useBox /
   blockBox / hingeBox 6 x u8 each, pushAxis u8, anchorCount u8, anchors 2 x 3 x i32 (always both) = **63 B**; state =
   count u8, modelOffset 3 x i16, linkedNetId u32, spawnVoxel 3 x i32, quarterTurns u8 = **24 B**, then count x 11 B
   segments. Decoder refuses: kind not 1..3, count 0 or > 8, a box over 255 or empty, `ValidateMechanismDesc`
   false, quarterTurns > 3, pivotQ / modelOffset outside int16.
4. Owner block: `uint32_t lastUseTick = 0; sim::UseResult useResult = sim::UseResult::None;` after `[wp-7]`'s
   fields; `kSnapshotOwnerBytes += 5`; decoder refuses `useResult > kMaxUseResult`.
5. `kProtocolVersion = 12` with the comment line `12: [mech-1] (EntityKind::Mechanism, MechanismMove 37, the owner
   block's lastUseTick / useResult, PlayerButton_Use 16).`
6. `uint64_t MechanismTerm(const scene::MechanismSegment& newest, uint8_t flags, const glm::ivec3& modelOffset)` in
   `Messages.cppm`: `h = voxel::HashMix64(0x4D454348u, newest.startTick)`; `h = HashMix64(h, uint16(startPos) |
   uint64(uint16(targetPos)) << 16 | uint64(durationTicks) << 32 | uint64(newest.flags) << 48)`; `h = HashMix64(h,
   flags)`; `h = HashMix64(h, uint32(modelOffset.x) | uint64(uint32(modelOffset.y)) << 32)`; `h = HashMix64(h,
   uint32(modelOffset.z))`; return h.
7. Registry, `engine/docs/notes/net.md` section 2: event kind 37 `MechanismMove` [mech-1]; `EntityKind` 3 `Mechanism`;
   `PlayerButton` 16 `Use`; `UseResult` 0-6; `MechanismKind` 1-3 (4-6 reserved for `[mech-3]`); `MechanismFlag` 1, 2,
   4, 8; desc flags 1, 2, 4, 8. Replace the provisional sentence "then mechanisms' `MechanismMove` 40 and `RopeCut`
   41" by the landed values ("`MechanismMove` 37, `EntityHold` 41 and `EntityRelease` 42 landed with [mech-1];
   38-40 stay free for [mag-2]'s planned `ZoneStart`, `ZoneCancel`, `CastFx`; `RopeCut` takes the next free value
   after 42 with [mech-2]"). Also: `ObjectSpawn` flag 2 `kObjectHeld`; `sim::HoldEnd` 1 `Drop`, 2 `Throw`, 3
   `HolderDead`, 4 `HolderGone`, 5 `Damaged`, 6 `Pinned`; `kSnapshotPlayerBytes` 18. `engine/docs/llm/README.md`
   "Values planned across the packs" already plans 41 and 42 (since 2026-11-24): check that the landed values match.
8. **Holding on the wire** (design "One use key: E", "Prediction and replication"):
   - `EventKind::EntityHold = 41`: `struct EntityHold { uint32_t netId; uint8_t holder; uint16_t relYaw; uint32_t
     tick; }` = kind u8 + 4 + 1 + 2 + 4 = **12 B**.
   - `EventKind::EntityRelease = 42`: `struct EntityRelease { uint32_t netId; uint32_t tick; uint8_t reason;
     glm::vec3 position; glm::quat rotation; glm::vec3 velocity; }`: kind u8, netId u32, tick u32, reason u8,
     position 3 x f32 and rotation 4 x f32 as `EntityRest` writes them, velocity 3 x i16 (`lround(v * 256)`) =
     **44 B**. The decoder refuses reason 0 or `> sim::kMaxHoldEnd` and a non-finite pose.
   - `Event` gains both. `HoldChange` <-> these two in `net` (a `ToEvent(const sim::HoldChange&, netId)` helper).
   - `kObjectHeld = 2` in `ObjectSpawn::flags`; `ObjectSpawn` gains `uint8_t holder; uint16_t relYaw; uint32_t
     holdSince;` written after the object's runs only when the flag is set (**+7 B**). A decoder refuses
     `kObjectHeld` together with `kObjectResting`.
   - `RemotePlayer` gains `int8_t holdPitch = 0;` (after `fight`); `kSnapshotPlayerBytes` + 1 (17 -> 18 on top of
     `[wp-7]`); the decoder refuses values outside -64..64.
   - `uint64_t HeldTerm(uint8_t holder, uint16_t relYaw, uint32_t since)` in `Messages.cppm`: `h =
     voxel::HashMix64(0x484F4C44u, uint64(holder) | uint64(relYaw) << 8)`; `return voxel::HashMix64(h, since)`.
   - `kProtocolVersion`'s comment line for 12 gains "EntityHold 41, EntityRelease 42, kObjectHeld, the remote
     record's holdPitch".

**Tests** (new file `engine/tests/net/MechanismNetTests.cpp`)
- `Mechanisms.WireSizes`: `MechanismMove` encodes to 16 B; a mechanism spawn with 1 and with 8 segments encodes to
  the same object's `EntityKind::Object` spawn + 63 + 24 + 11 and + 63 + 24 + 88; a snapshot with an owner block
  is `kSnapshotOwnerBytes` (the `[wp-7]` value + 5) longer in its owner part; every value round-trips equal; a
  decoder refuses a count of 0 and 9, a kind of 4, a useResult of 7, one trailing and one missing byte. Holding:
  `EntityHold` encodes to 12 B and `EntityRelease` to 44 B; a held object's spawn is its unheld spawn + 7;
  `kSnapshotPlayerBytes` is the `[wp-7]` value + 1 and each remote record encodes to it; every value round-trips
  equal (the release's velocity as `QuantizeVelocity`); refused: reason 0 and 7, `holdPitch` 65 and -65, a spawn
  with both `kObjectHeld` and `kObjectResting`.
- Extend `Wire.MessagesRoundTripAndRefuseTrailingOrMissingBytes` (or its successor) with one `MechanismMove`, one
  `EntityHold` and one `EntityRelease`.

**Checks**
1. build; 2. `tools/ctest.sh -R 'Wire\.|Mechanisms\.' --output-on-failure`; 3. `tools/ctest.sh`; 4. doc-check.

**Docs**
- notes/net.md section 2 (above); CLAUDE.md section 5 "Sessions": add `EntityKind::Mechanism`, `MechanismMove`,
  `EntityHold`, `EntityRelease`, `sim::HoldEnd` and `UseResult` to the wire-format list (names only; the numbers stay
  in notes/net.md); llm/README's plan row (step 7).

**Must not break**
- `Session.*` builds refuse other protocols ("another build"); `SessionFile.BuildHashSeesThePlayerRuleRevision`.

**Done when**
- [ ] Protocol 12, the registry updated, `Mechanisms.WireSizes` green (with its holding part).

**Traps**
- Never take 38-40 for holding (the plan gives them to `[mag-2]`); if 41 or 42 is taken when you land, take the next
  free values above 40 and write them in "Deviations".
- Never reuse 37 if it is already taken when you land: take the next free value, write it in "Deviations" and in the
  README plan table (rule 8 of the guide).

## Card 9: Server

| | |
|---|---|
| Goal | In a session E opens the yard's door for everyone, a lever runs the gate, and no gate is ever deleted by the byte budget; E picks up, drops and LMB throws a crate, decided by the server |
| Depends on | card 8 |
| Effort | 1.5 d |
| Touches | net (server, session file); wire: uses card 8's values |

**Read first** (about 70 KiB; read sections, Server.cpp is over 60 KiB)
- The server's `StepPlayers`, `OnInputs` (late inputs), `PlayerCrate`, `LateCast`, `EmitObjectEvents`, `DescribeObject`,
  `EmitObjectSpawn`, `TrackBodies`, `AwakeSamples`, the cast-run path (`grep -n 'ResolveRay\|RunCast'`), the baseline
  builder (`grep -n 'BaselineEntity'`), the entity check / list builders (`grep -n 'EntityModelTerm\|EntityListEntry'`),
  `Server::Tick()`, the template start (`grep -n 'CreateTemplateObjects\|templ.objects'`). For holding also
  `MakeBaselineEntity` (`ServerPeers.cpp`), `Server::Entities()`, `Server::AwakeBodies()` and the "carries a player"
  test in `TrackBodies` (`ServerEntities.cpp`).
- `engine/src/net/Session.cppm` `SessionConfig`, `ParseSession`, `FormatSession`.
- mechanisms.md "What the server does with a Use edge", "The lead", "Replication" (server bullets).

**Steps**
1. Session file key `mechanisms = 0|1` (`SessionConfig::mechanisms`, default false; parse, format, test in the
   existing `SessionFile.*` round-trip test). At start, after the template objects: if set and the template is the
   playground, `yard = MechanismYardTemplate(world, ids, templ.anchor)`; its ops go through `EmitOp` like the
   template's; `CreateTemplateMechanisms`; for each entity in order `Replicate(e, false)`, fill `linkedNetId` from
   `linked`, then `EmitMechanismSpawn(netId, e)`: `tracks[netId] = {resting = true, restTick = Tick(), restPosition,
   restRotation = the pose}`; `Emit(EntitySpawn{kind Mechanism, object = DescribeObject with flags kObjectResting,
   mechanism = its state with every segment})`.
2. `PlayerUse(Peer& p, const sim::PlayerInput& in, const sim::PlayerState* at, uint32_t now)`:
   `pl.lastUseTick = in.tick`; `at == nullptr || now - in.tick > sim::kUseStaleTicks` -> `useResult = Stale`, return;
   `eye = at->position + (0, eyeHeight, 0)`; `dir = LookDirection(in.yaw, in.pitch)`;
   `FindUseTarget(sim, eye, dir, in.tick, t)` false -> NoTarget; else `useResult = ApplyUse(sim, t, eye, now, UseLead(),
   changes)`; each change -> `Emit(MechanismMove{NetIdOf(change.entity), change.segment})` (`NetIdOf(e)`: the
   entity's `scene::NetId::value`); stats.
   `static_assert(sim::kUseStaleTicks < kStateHistory)`.
3. Call it where crates are: `StepPlayers` after `StepPlayer`: `if (r.pressed & PlayerButton_Use) PlayerUse(*peer,
   input, &pl.state, now)`; the late path in `OnInputs`: `if (pressed & PlayerButton_Use) PlayerUse(p, in,
   pl.StateAt(in.tick), now)` (the state AT the input's tick, unlike today's crate).
4. `UseLead()`: over joined players: `rttTicks = ceil(transport.RoundTripSeconds(peer) / dt)`; `margin = pl.margin ==
   kNoInputMargin ? 0 : max(pl.margin, 0)`; `lead = clamp(max(rttTicks + margin) + 2, kMechanismLeadMin,
   kMechanismLeadMax)`; track `stats.useLeadMin / Max`.
5. `StepPlayers`, after `FlushBox` and before `StepPlayer`: `PoseMechanisms(sim, now, reach)` and
   `PushOutOfMechanisms(sim, now, pl.state, pl.character, settings.player)`.
6. `Server::Tick()`: between `RunCasts` and `StepPhysics`: `r = sim::StepMechanisms(sim, tick, true)`; emit each
   change; `DrainDespawns` afterwards (crushed bodies).
7. The cast path: when a cast's ray resolves on a body whose entity has `scene::Mechanism`, run `sim::MechanismBlast`
   instead of `sim::Blast` (charge `CastWorkOf` of the result: ops are empty, so only its object half, which is not
   charged); if `entityCredit <= -8 * entityBurst` skip it and count `stats.mechanismHitsSkipped`. Then emit as for
   any cast: object events first (models, spawns), then the result's `mechanismChanges` (a `Freed` move after its
   model), then despawns.
8. `EmitObjectEvents`: an `ObjectEvent::Model` whose entity has `scene::Mechanism` never goes `ToRubble`: it is sent;
   if `entityCredit <= 0` count `stats.mechanismModelsOverBudget`. Its `motion` byte is Kinematic.
9. `TrackBodies`, `AwakeSamples`: skip entities with `scene::Mechanism` (a freed one has lost the component and is an
   ordinary body: its resting track makes the next `TrackBodies` emit `EntityWake`).
10. Baseline / ModelRequest answer: a mechanism's `BaselineEntity` is its spawn with `segments` = the ring's
    segments with `startTick + durationTicks >= tick` (the baseline's tick), always at least the newest, oldest
    first; `modelOffset`, `linkedNetId` current.
11. Checks: wherever the server computes a replicated object's model hash for `EntityCheck` / `EntityList`, a
    mechanism uses `VolumeHash ^ MechanismTerm(m.track.Newest(), m.track.Newest().flags, m.modelOffset)` (after the
    cached `VolumeHash`, which stays keyed by the model pointer).
12. Snapshot owner block: `lastUseTick`, `useResult` from the player.
13. `vortex_server` exit / stats line: `Mechanisms: server: N mechanisms, uses U (refused R), lead min A max B, moves
    M, crushed K, models over budget O, hits skipped S`.

Holding (steps 14-21; design "One use key: E", "Where it runs", "Prediction and replication"; `NetIdOf(e)` below is
the entity's `scene::NetId::value`, read with `TryGet<scene::NetId>`; `pl.last` is the player's last real input, the
`sim::PlayerInput last` field of `Player`):

14. `Player` gains `scene::Entity held = kNoEntity`, `uint32_t holdChangeTick = 0`, `bool anyHoldChange = false`,
    `uint32_t autoReleaseTick = 0`, `uint32_t autoReleaseEcho = 0` (0: no echo window), `scene::Entity groundEntity =
    kNoEntity`, `uint32_t groundTick = 0`. After each `StepPlayer`: if `GetCharacterState(character).groundBody` is a
    replicated body's, `groundEntity` = its entity and `groundTick = now`. At the start of `StepPlayers` and of
    `StepHolds`: a `held` that is no longer valid or lost its `scene::Held` component -> `kNoEntity` (the component is
    the truth). `BodyTrack` gains `bool held = false`. `EmitHold(const sim::HoldChange& c)`: `Emit(EntityHold{...})`
    or `Emit(EntityRelease{...})` with `NetIdOf(c.entity)`. A hold: `tracks[netId].held = true` and
    `tracks[netId].resting = false` (`EntityHold` is the wake: no `EntityWake` is sent). A release:
    `tracks[netId].held = false`, `resting` stays false (the next `TrackBodies` samples it and later rests it).
    `stats.holds` / `stats.releases[reason]`. `HoldEnvFor()`: `{settings.player, sim::PlayAreaOf(region,
    settings.player), the feet of every joined, living player in slot order}`.
15. `PlayerUse` (step 2) starts:
    - **Echo:** `if (pl.autoReleaseEcho && in.tick <= pl.autoReleaseTick + pl.autoReleaseEcho)`: `lastUseTick =
      in.tick`, `useResult = None`, return (the edge is consumed; design "The echo of an auto-drop").
    - **Drop:** `if (pl.held != kNoEntity)`: `useResult = None`; `EndHold(sim, pl.held, Drop, HeldPoseAt(...,
      pl.state.position, in.yaw, HoldPitch(in.pitch), env, now).pose, ReleaseVelocity(Drop, pl.state.velocity, ...),
      &anchor, env, now, true, c)` with `anchor = HoldAnchor(pl.state.position, settings.player)` (now, not `at`: the
      hold is the server's state now); `EmitHold(c)`; `held = kNoEntity`; `holdChangeTick = now`, `anyHoldChange =
      true`; return. The drop runs before the stale test: a drop is never refused.
    - After the stale check, `FindUseTarget(sim, eye, dir, in.tick, t, &hooks)` with `hooks.holdable = [](e) {
      return IsHoldable(sim, e, true); }` and `hooks.key = NetIdOf`; by `t.kind`: `Holdable` -> `stoodOn` = some
      player (this one included) has `groundEntity == t.entity` and `now - groundTick <= kHoldGroundTicks`;
      `useResult = BeginHold(sim, t.entity, slot, in.yaw, now, holdChangeTick, anyHoldChange, stoodOn, true, c)`, on
      None `EmitHold(c)`, `held = t.entity`, `holdChangeTick = now`; `HeldByOther` -> `Busy`; `Mechanism` / `Info`
      -> `ApplyUse` as step 2; `None` -> `NoTarget`.
16. **The Cast edge while holding, and both edges:** in `StepPlayers` after `StepPlayer`, the holding branch runs
    **before** the Use branch of step 3: `if (r.pressed & PlayerButton_Cast)` and the echo window of step 15 holds
    for `input.tick` -> the Cast edge is consumed (no cast capture), and a Use edge of the same input too. Else `if
    (pl.held != kNoEntity && (r.pressed & PlayerButton_Cast))`: `EndHold(Throw, the hold pose,
    ReleaseVelocity(Throw, pl.state.velocity, input.yaw, input.pitch), &anchor, env, now, true, c)`, `EmitHold(c)`,
    and the cast capture for `r.cast` is skipped (no queue entry, no rules seam, no charge, `lastCastTick` /
    `castRejected` unchanged); a Use edge in the same input is consumed with it (`lastUseTick = input.tick`,
    `useResult = None`; step 3's `PlayerUse` does not run). The late path (`LateCast` and the late `PlayerUse`): the
    same tests on `pl.held` now and on the echo window; a throw replaces the late cast.
17. `StepHolds()` in `Server::Tick()` after `RunCasts`, before `StepMechanisms` / `StepPhysics`: for each player in
    slot order with `held`: `if (StepHeld(sim, held, pl.state, pl.last.yaw, HoldPitch(pl.last.pitch), env, now,
    true))` -> `EndHold(Pinned, ...)`, `EmitHold`, `autoReleaseTick = now`, `autoReleaseEcho = EchoTicks(pl)`
    (`ceil(RTT_p / dt) + max(margin_p, 0) + 2`, clamped to `kMechanismLeadMin..kMechanismLeadMax`: step 4's term for
    this player alone). A dead player (`[wp-7]`, not stepped) has no `held` (step 18).
18. **Auto-drops:** in the `[wp-7]` death path (`grep -n 'Dead\|respawn' engine/src/net/internal/Server*.cpp`), before
    the death is applied: `held` -> `EndHold(HolderDead, ...)`. Where a player is removed (disconnect, kick, timeout):
    `EndHold(HolderGone, its current transform, its current velocity, nullptr, ...)`. After every `sim::Blast` /
    `MechanismBlast` the server emits `result.holdChanges` **before** the result's object events (a release, then
    its `ObjectModel`), and for each sets the holder's `held = kNoEntity`, `autoReleaseTick = now` and
    `autoReleaseEcho = EchoTicks(holder)`.
19. `TrackBodies`, `AwakeSamples`, `Server::AwakeBodies`: skip tracks with `held` (no wake, no rest, no layer, no
    sample, not awake). The `on_destroy<NetId>` observer of a held body clears its holder's `held` (the despawn tells
    every client).
20. Baseline / `ModelRequest` answer / spawn (`MakeBaselineEntity`, `DescribeObject`): a body with `scene::Held` sets
    `kObjectHeld` with holder, relYaw, sinceTick, **never** `kObjectResting`, and its current pose (not the track's
    rest pose). `Server::Entities()` reports a held body not resting (its track says so since step 14). Checks (step
    11's place, the `EntityCheck` model term and the `EntityList` entries only): a body with `scene::Held` uses
    `VolumeHash ^ HeldTerm(holder, relYaw, sinceTick)`; `EntityInfo::modelHash` of `Server::Entities()` stays
    `VolumeHash`. Snapshots: each `RemotePlayer::holdPitch = HoldPitch(pl.last.pitch)`.
21. The exit line gains `Hold: server: P pickups, D drops, T throws, A auto-drops (dead H, gone G, damaged M, pinned
    N), refused R, echo edges E`.

**Tests** (in `MechanismNetTests.cpp`, on `EntityTests.cpp`'s in-process server / bot set-up: copy its helpers with a
`Yard` prefix)
- `Use.RefusalReachesTheUser`: a client's use with no target gets `useResult == NoTarget` for its `lastUseTick` in
  the next snapshots; a late input more than 30 ticks old gets `Stale`.
- `Mechanisms.GateNeverGoesToRubbleOverBudget`: `entityCredit` spent by other casts; a cast at the gate still sends
  its `ObjectModel`; `modelsToRubble` counts no mechanism.
- `Use.TargetAtTheInputTick` already covers the sim half; add `Mechanisms.LateUseUsesTheStateAtItsTick`: a use sent
  late (tick T - 10) by a player who moved since is resolved from `StateAt(T - 10)`'s eye (hits; with today's state
  it would miss).
- `Hold.ServerPicksUpAndDrops`: a bot looks at a resting crate and presses Use: one `EntityHold` (its slot) in the
  stream, `useResult` None, no `EntityWake`; `Server::Entities()` reports the crate not resting while held; no
  `BodySample` for the crate's NetId in any snapshot while held (60 ticks); a `ModelRequest` answer for it carries
  `kObjectHeld` and not `kObjectResting`; a second Use: one `EntityRelease` reason 1, then samples, then within 600
  ticks one `EntityRest`.
- `Hold.ThrowRunsNoCast`: holding, a Cast edge: an `EntityRelease` reason 2 with velocity = `QuantizeVelocity(state
  velocity + look * 6)`; no cast ran (`stats` casts unchanged, `WorldHash` unchanged), `lastCastTick` unchanged; in a
  deathmatch session the player's charges are unchanged. An input with both the Use and the Cast edge while holding:
  one release, reason 2, and no use and no cast.
- `Hold.HolderDeathDrops` (deathmatch session, a scripted lethal blast on the holder): `EntityRelease` reason 3 before
  the death's effects; `Hold.HolderLeavingDrops`: the holder's peer disconnects: reason 4.
- `Hold.NotCountedAsAwake`: 64 awake replicated bodies minus one plus a held crate: `TakePieceSlot` still answers
  Replicate for one new piece; `Server::AwakeBodies()` does not count the held crate.
- `Hold.NoOneLosesTheirFloor`: bot 1 stands on a crate, bot 0 presses Use at it: `useResult` Busy, no `EntityHold`;
  bot 0 stands on it and presses Use: Busy; bot 1 steps off and bot 0 presses Use 20 ticks later: Busy, 31 ticks
  later: None.
- `Hold.AutoDropEchoIsConsumed`: a holder pinned for 30 ticks (`EntityRelease` reason 6); a Cast edge and then a Use
  edge (at a door in reach) with input ticks inside the echo window: no cast (`stats` casts and `WorldHash`
  unchanged), no `MechanismMove`; the same edges after the window: one cast, one `MechanismMove`.

**Checks**
1. build; 2. `tools/ctest.sh -R 'Mechanisms\.|Use\.|Hold\.|Entities\.|Replication\.|SessionFile\.|Fight\.' --output-on-failure`;
3. `tools/ctest.sh`; 4. doc-check; 5. `VERIFY_SLOT=1 tools/verify.sh quick` (G11-G12 unchanged: no yard in
   `drill.session` / `arena250.session`).

**Must not break**
- CLAUDE.md "Replicated bodies": every destruction through `on_destroy<NetId>`; `TakePieceSlot` caps; rest and wake
  are events. "Checks": the model hash cache keyed by the model pointer (a mechanism's model is replaced, never
  changed in place). "Chunk sync": the yard's ops go through `EmitOp`.

**Done when**
- [ ] Ten tests green (three mechanism, seven holding); verify quick PASS.

**Traps**
- The decision tick is `now`; the target is found at `in.tick`. Do not mix them up: the segment starts at `now +
  lead`, never in the past. A hold starts at `now` (no lead), and a drop always uses the state now.
- A release is emitted before the `ObjectModel` of the same damage: a client must not re-make a held copy.
- A held body's track must not stay `resting` from an earlier `EntityRest`: the drill's entity comparison
  (`restStateMismatches`) and every baseline read that flag.

## Card 10: Client

| | |
|---|---|
| Goal | A predicting client walks through a door that opens without a correction, and draws mechanisms where it collides with them; it predicts its own pick-up and drop and poses other players' held objects |
| Depends on | card 9 |
| Effort | 1.5 d |
| Touches | net (client); wire: no |

**Read first** (about 70 KiB, sections)
- The client's entity event handler (`grep -n 'EntitySpawn\|ObjectModel\|EntityWake\|EntityRest' Client.cpp`),
  `MakeCopy`, `MoveCopies`, `Hold`, the baseline handler, the EntityCheck count, `FreshColliders`, `PredictTick`,
  `Reconcile`, `Client::StepPlayer` (resume replay), `Client::RunTick`; `ClientStats`; `ApplyEntity`'s per-NetId
  block and `MovingLayer` in `ClientEntities.cpp`; `RemoteBodies` and `RemotePlayerView` in `net/Prediction.cppm`.
- mechanisms.md "Replication" ("Clients keep mechanisms apart from copies"), "The lead", "What the predicting client
  shows at once".

**Steps**
1. `std::unordered_map<uint32_t, MechanismCopy> mechanisms;` (`MechanismCopy{entity, base opSeq, modelHash}`),
   iterated only by sorted NetId where order matters. An entity is in `copies` or in `mechanisms`, never both.
2. `EntitySpawn` / `BaselineEntity` of kind Mechanism: `CreateVoxelObject({model, position = pose, rotation, motion =
   Kinematic})`, emplace `scene::Mechanism` from the state (desc, spawnVoxel, quarterTurns, modelOffset, segments
   pushed in order, `linkedNetId`; `linked` resolved from `mechanisms` when both exist), `swept`; pose at `nextTick`
   and `SetLayer(MechanismLayer(m, nextTick))`. A baseline entity for a NetId already present replaces it (7b's repair
   path).
3. `MechanismMove`: NetId in `mechanisms` -> `PushSegment`; count `stats.mechanismLateEvents` when
   `predicting && nextTick > segment.startTick`; with `MechanismFlag_Freed`: remove the component, move the entry to
   `copies` as a moving copy (`resting = false`, layer Rubble, velocity zero) - from now the server's wake / samples /
   rest drive it. With `Broken`: nothing (the despawn follows). Unknown NetId -> the existing `Hold` path.
4. `ObjectModel` for a NetId in `mechanisms`: re-make the kinematic body with the new model (as for copies:
   `ShowRemovedVoxels`, destroy, create), keep the component with `modelOffset += offset`, re-pose at `nextTick`,
   `SetLayer(MechanismLayer)`.
5. `MoveCopies`, `EntityRest`, `EntityWake` and snapshot samples never touch `mechanisms` (they look up `copies`
   only; check).
6. EntityCheck: count mechanisms with copies: `EntityModelTerm(netId, VolumeHash(model) ^ MechanismTerm(...))`; the
   EntityList comparison uses the same hash; a mismatch asks `ModelRequest` (unchanged path).
7. `PrepareStep(uint32_t tick)`: `FreshColliders(); sim::PoseMechanisms(sim, tick, PlayerReach(player.position,
   tuning)); sim::PushOutOfMechanisms(sim, tick, player, character, tuning);` - replaces `FreshColliders()` before each
   of the four `StepPlayer` calls (`PredictTick`: tick `nextTick`; `Reconcile` replay: `k`; resume replay: `k`;
   `StartPrediction` goes through `PredictTick`).
8. `RunTick`: after `StepPlayer(controls)`: `sim::StepMechanisms(simulation, nextTick - 1, false)` (no crush, no
   anchors on a client).
9. `stats.mechanismCorrections`: in `Reconcile`, a correction whose `PlayerReach(snap.owner.position)` meets any
   mechanism's `swept` counts one.
10. `vortex_server`'s per-bot lines (the bot uses `net::Client`): `Mechanisms: bot N: U uses, R refused, positions
    agree` (agree = every mechanism's `PositionAt(t)` and newest segment equal to the server's for t = the last
    server tick) and `Mechanisms: bot N: late events E, corrections at mechanisms C`.

Holding (steps 11-18; design "Prediction and replication"; client copies stay kinematic: never `Dynamic`,
`SetVelocity` other than zero, or `Activate` on a copy):

11. `Copy` gains `uint8_t motion = 2` (the server body's `physics::MotionType`: from `ObjectSpawn::motion` in
    `MakeCopy`, updated by every `ObjectModel`'s `motion`) and `bool held = false`. The client keeps `netIdOf`
    (`scene::Entity` -> NetId, filled in `MakeCopy`, erased with the copy). `Client` gains `HoldPending{uint32_t netId,
    inputTick; bool drop}` (optional), `uint32_t heldNetId` (0 none, confirmed or predicted), `uint8_t latchedEdges`
    (Use / Cast edges seen while a pick-up is pending) and `pendingReleases` (NetId -> the `EntityRelease` waiting
    for the drawn holder, step 15). `RemoteBodies` gains `Restart(netId, tick)`: drop every buffered sample of the
    body and refuse every sample of `tick` or older from now on, without marking it rested (`Rest`'s twin).
    `RemotePlayerView` gains `int8_t holdPitch`, taken from the nearer sample in the remote-player interpolation, not
    blended (as `fight`).
12. **Events.** The per-NetId block of `ApplyEntity` (today the chain `holds_alternative<EntityRest> ? ... :
    holds_alternative<EntityWake> ? ... : std::get<ObjectModel>`, which would throw `bad_variant_access` on a new
    kind) becomes one helper, `EntityEventNetId(const Event&)`, that names every entity event of a NetId: `EntityRest`,
    `EntityWake`, `ObjectModel`, `MechanismMove`, `EntityHold`, `EntityRelease`. The `seq <= base` drop and the
    `Hold` deferral path apply to all of them (for `MechanismMove`, the base of the `mechanisms` entry). So a copy
    re-made by a `BaselineEntity` with `kObjectHeld` or by a `ModelRequest` answer ignores holds and releases at or
    below its base.
    - `EntityHold` for a NetId in `copies`: `ApplyHoldChange(sim, e, {hold, holder, relYaw, tick})`, `copy.held =
      true`, `copy.resting = false` (the event is the wake), `bodies.Restart(netId, hold.tick)`. `MoveCopies`,
      samples, `EntityRest` and `EntityWake` skip a held copy. Holder == own slot: `heldNetId = netId`, pending
      cleared, the latched edges go out (step 13).
    - `EntityRelease`: own slot's object -> applied at once (step 14). Another holder's -> kept in
      `pendingReleases` and applied by step 15. Applying it: `ApplyHoldChange` with the event's pose (a teleport, no
      velocity), `copy.held = false`, `copy.resting = false`, `SetLayer(MovingLayer(...))` as `EntityWake` does,
      `bodies.Restart(netId, release.tick)`; own: `heldNetId = 0`, pending cleared; reason `Pinned` or `Damaged` of
      the own object: the HUD line `Dropped - blocked` / `Dropped - hit` for 1 s.
    - Baseline / spawn with `kObjectHeld`: the same as an `EntityHold` after the copy is made. Despawn of a held
      copy: own `heldNetId = 0`, its `pendingReleases` entry dropped.
13. **Predicted pick-up and drop** (in `PredictTick`, after `StepPlayer`, only for the newest tick, never in
    replays). The hooks of the client: `hooks.holdable = [](e) { return IsHoldableShape(sim, e) && the copy of
    netIdOf[e] exists with motion == Dynamic and !held; }`, `hooks.key = netIdOf[e]`.
    - Use edge, `heldNetId == 0`, no pending, `FindUseTarget(..., &hooks)` kind Holdable, and the target's body is
      not the own `groundBody` (that one shows greyed `Occupied` and is sent unpredicted): `pending = {netId, tick,
      false}`; `ApplyHoldChange(sim, e, {hold, own slot, RelYawAtPickup(R, input.yaw), tick})` (the copy stays
      Kinematic); `heldNetId = netId`.
    - Use or Cast edge (or both: one throw) with `heldNetId != 0`: `pending = {heldNetId, tick, true}`; stop posing;
      the copy stays where it is, in `Layer::Held`, until `EntityRelease` (the hang, chosen: design open question 19).
    - **Latching:** while a pick-up is pending (`pending && !pending.drop`), the client clears Use and Cast from the
      input it steps and sends, and sets their bits in `latchedEdges` on every new press (an edge, even if the key is
      up again before the answer). Confirmed (step 12): the next input carries the latched bits for one input (an
      edge each; the server then sees a drop or a throw). Refused (step 14): `latchedEdges = 0` (a failed grab fires
      nothing). No cast picture while `heldNetId != 0` or a pick-up is pending (wherever the client presents its
      own cast edge: `grep -n 'cast' engine/src/net/internal/Client*.cpp src/Session.cpp`).
14. **Resolve:** on each snapshot, after the events up to its `lastOpSeq`: a pending pick-up with `owner.lastUseTick >=
    pending.inputTick` and no confirming `EntityHold` -> undo: remove `Held`, the copy back to the server's state (its
    rest pose and `Layer::Dynamic` if it rests, else `MovingLayer` and waiting for samples), `heldNetId = 0`,
    `latchedEdges = 0`, `stats.holdRefused++`, the prompt shows `useResult`. A pending drop resolves only on the
    `EntityRelease` of that NetId (any reason: the server never refuses a drop, and an edge in the echo of an
    auto-drop is consumed while that auto-drop's release resolves it).
15. **Posing** in `RunTick` after `StepPlayer`, before `StepMechanisms` / `StepPhysics` (`env` = `HoldEnv{tuning,
    sim::PlayAreaOf(region, tuning), {}}`; a client runs no fix-up, so no feet): own held copy (not pending a drop):
    `StepHeld(sim, e, player, input.yaw, HoldPitch(input.pitch), env, nextTick - 1, false)`, and keep its
    `HeldPose::pinned` for the HUD's amber prompt. Other holders: for each held copy whose holder is another slot, its
    `RemotePlayers()` entry as the game draws it: `p = HeldPoseAt(sim, e, remote.position, remote.yaw,
    remote.holdPitch, env, nextTick - 1)`; `scene.SetTransform(e, p.pose, teleport)`. No entry: leave it. **A pending
    release of another holder** is applied (step 12) once the drawn time of that holder (the server tick the
    remote-player interpolation draws) reaches `release.tick`, or 12 ticks (`RemoteBodies::kMaxDelayTicks`) after it
    arrived, whichever comes first; until then the copy stays posed from the drawn holder.
16. **Checks:** `Copy::modelHash` stays `VolumeHash` (it is what `Client::Entities()` reports to the drill's
    comparison). Only where the client builds its `EntityCheck` model term and compares `EntityList` entries, a copy
    with a confirmed hold (an applied `EntityHold`, not a predicted one) uses `VolumeHash ^ HeldTerm(holder, relYaw,
    sinceTick)`.
17. **The session game** (`src/Session.cpp`): the prompt `Occupied` and the auto-drop lines come from the client
    (expose `Client::HoldNotice()` and the own pose's `pinned`); card 11 step 6a draws them.
18. `stats` and the bot line: `Hold: bot N: P pickups, D drops, T throws, R refused, positions agree` (agree: every held
    copy's holder and relYaw equal to the server's at the last tick).

**Tests**
- `Mechanisms.LateJoinerDerivesTheSamePosition`: two pending segments (start in the future); a client joining then
  computes the same `PositionAt` for every tick of the next 60 as the server.
- `Mechanisms.DentedDoorKeepsItsLayerOnEverySide`: dents mid-swing and at rest: server and client layers equal at
  every tick for 120 ticks.
- `Mechanisms.NoCorrectionWalkingThroughAnOpeningGate`: lossless loopback with RTT 100 ms, the lead computed by
  `UseLead`; a player uses the door and walks through: 0 corrections.
- `Mechanisms.NoCorrectionUnderARisingGate`: a player waits at the passage while the lever raises the gate and walks
  under it once it is up: 0 corrections.
- `Mechanisms.CorrectionOnlyWhenTheEventIsLate`: RTT 400 ms (lead clamped at 20): the player walking into the door is
  corrected at least once; a second player who stays away is not.
- `Mechanisms.EntityCheckCoversTheSegment`: drop one `MechanismMove` to one client (test hook of the harness): the
  next EntityCheck mismatches, the list names the door, the client's `ModelRequest` repairs it (its segments equal
  the server's afterwards), counted in `mechanismsRepaired`.
- `Hold.PredictedPickupIsConfirmed`: RTT 100 ms; the client's prompt finds the crate's copy (kind Holdable, through
  the client hooks: the copy is Kinematic and has no `scene::NetId`); the copy is Held from the tick of its Use edge;
  the `EntityHold` confirms it (`heldNetId` unchanged, `holdRefused` 0); 0 corrections over 120 ticks of walking with
  it; the copy's physics motion type stays Kinematic throughout, the drop included, and after the drop its pose
  follows the server's samples (no sample of the release tick or older is taken).
- `Hold.RefusedPickupIsUndone`: two clients press Use on one crate in the same server tick: the lower slot holds it;
  the other's copy is back at the server's pose (rest pose within 1e-4) after the snapshot with its `lastUseTick`,
  `holdRefused == 1`; a Use press and a Cast press made and released while pending (each shorter than the round
  trip) never reach the server (no use, no cast in the server's log).
- `Hold.RemoteHolderPosesTheObject`: client B's copy of client A's held crate is within 0.05 m of `HeldPoseAt` from B's
  drawn view of A at every tick of 60; A walks at full speed and drops it: on B, the copy's jump at the release is
  under 0.1 m (the release waits for the drawn A to reach its tick).
- `Hold.LateJoinerSeesTheHold`: a client joins while a crate is held: its baseline copy is Held with the server's
  holder and relYaw and not resting; after the drop its copy is at the release pose bit for bit, then follows the
  server's samples.
- `Hold.EntityCheckCoversTheHold`: drop one `EntityHold` to one client (the harness hook): its next EntityList
  comparison names the crate, `ModelRequest` re-makes it held.

**Checks**
1. build; 2. `tools/ctest.sh -R 'Mechanisms\.|Hold\.|Movement\.|Entities\.|Client\.' --output-on-failure`;
3. `tools/ctest.sh`; 4. doc-check; 5. `VERIFY_SLOT=1 tools/verify.sh quick`.

**Must not break**
- `Movement.NoCorrectionAbove2cmOnFlatGround` stays at 0 corrections; `Client.ASnapshotsSamplesWaitForItsEvents`;
  CLAUDE.md "Movement": `FlushBox` before every step stays (it is inside `PrepareStep`).

**Done when**
- [ ] Eleven tests green (six mechanism, five holding); `Movement.*` unchanged.

**Traps**
- Pose at the step's tick (`nextTick` or the replayed `k`), never at `Simulation::Tick()`: the client's simulation
  tick is not its predicted tick.
- Never pose a held copy in a replay: it meets no character, and no player stood on it in the 30 ticks before the
  pick-up (card 9 step 15), so the replayed states do not depend on it; a replay that teleports it would make it
  jump back on screen.
- Never make a copy Dynamic (a release included): `MoveCopies` drives copies with `MoveKinematic`, which refuses a
  body that is not kinematic.

## Card 11: Session play and close

| | |
|---|---|
| Goal | The owner can open the yard's door, run its gate and carry crates with a friend; the stage is verified and documented |
| Depends on | card 10 |
| Effort | 1.5 d |
| Touches | net (bot), game (`src/Session.cpp`), tools, docs |

**Read first** (about 50 KiB)
- `engine/src/net/Bot.cppm` `BotSettings`; `engine/tools/server/main.cpp` (`--bot-casts`, `--bot-crates`).
- `src/Session.cpp`: the HUD block and the "Cast not run" refusal line (the owner block's use is shown the same way).
- `docs/verify.md`, `tools/session-check.sh`, mechanisms.md "Tests" "Headless".

**Steps**
1. `engine/tools/server/sessions/playground.session`: `mechanisms = 1`.
2. `BotSettings::useEveryTicks` (0 off) and `--bot-use P`: every P ticks a bot alternates its target, the door then
   the lever: it walks to `spot` = the target's use box centre (world, at its pose) + 1.4 m along the horizontal
   normal of its `pushAxis` towards the plaza (+z side for the yard), turns to look at the use box centre, and
   presses Use for one input. Bot 1 additionally walks through the gate passage (5 m north and back) whenever its
   client sees the portcullis at `maxPos`.
3. `src/Session.cpp`: the use prompt as offline (card 6 step 5, `FindUseTarget` on the client's simulation at
   `Client::PredictTick() - 1`); a refusal for the newest `lastUseTick` shows the reason for 1 s, as "Cast not run"
   does; the lead cue (card 6) when a `MechanismMove` arrives (count new segments per frame).
4. Lossless drill: `build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/playground.session
   --clients 2 --bot-use 90 --drill 20 --delay-ms 40` - exit 0, `hashes equal`, both `Entities: ... agree`,
   `Mechanisms: bot N: U uses, R refused, positions agree` for both, bot 1's `Client 1 movement:` at 0 corrections.
5. Lossy: the same with `--loss 0.05 --jitter-ms 30` - exit 0, `hashes equal`, `agree`, `Mechanisms: bot N: late
   events E, corrections at mechanisms C` with C <= E.
6. `tools/session-check.sh` PASS (playground with the yard).
6a. **Holding:** `BotSettings::holdEveryTicks` and `--bot-hold P`: every P ticks a bot walks to the nearest template
    crate (1.2 m from it, looking at its centre), presses Use, walks 5 m towards the plaza holding it, then presses
    Use again (drop); every third time it throws instead (a Cast edge). `src/Session.cpp`: the hold prompt (amber
    while the own pose is pinned), `Dropped - blocked` / `Dropped - hit` for 1 s after an own auto-drop, greyed
    `Occupied` for the body under the own feet, and
    `[E] Pick up` / greyed `Held` as offline (card 6 step 9, `heldNetId` for "holding"); a refused pick-up shows its
    `useResult` for 1 s. Drill: `vortex_server --session=engine/tools/server/sessions/playground.session --clients 2
    --bot-hold 120 --drill 20 --delay-ms 40` - exit 0, `hashes equal`, both `Entities: ... agree`, `Hold: bot N: P
    pickups, D drops, T throws, R refused, positions agree` with P >= 3 for both bots, `Hold: server:` with auto-drops
    0 and `Movement` 0 corrections for both bots; the same with `--loss 0.05 --jitter-ms 30`: exit 0, `hashes equal`,
    `agree`.
7. Close: `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage;
   `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh ci`; `VERIFY_SLOT=1 tools/verify.sh full --ci`;
   `VERIFY_SLOT=1 tools/verify.sh soak` in the background (report its result when it ends).

**Docs**
- mechanisms.md: header Status "Stage 1 built", "Stages" row 1 state **Built** (with the commits), the measured hits
  to free, lead min / max, the drill numbers in "Budgets and costs" marked measured.
- CLAUDE.md section 5 "Movement": "both sides call `sim::PoseMechanisms` and `sim::PushOutOfMechanisms` at the step's
  tick after `FlushBox` and before every player step (the client also before replays)"; "Replicated bodies": "a
  mechanism is never ToRubble, never takes an awake slot or a snapshot sample"; and for holding: "a held body is
  kinematic in `Layer::Held` (meets nothing), posed from its holder on every side, sends no samples and takes no
  awake slot; hold and release are events (`EntityHold`, `EntityRelease`), each works as a wake, and client copies
  stay kinematic"; test names
  `Mechanism.PlayerStepIsTheSameReplayed`, `Mechanisms.NoCorrectionWalkingThroughAnOpeningGate`,
  `Hold.HeldBodyMeetsNothing`, `Hold.ServerPicksUpAndDrops`.
- `docs/owner-checks.md`: a new check "E opens the yard door, the lever runs the gate, Q drops a crate; E picks up a
  crate, it follows the view below the crosshair, E drops it, LMB throws it (it hangs about one ping first); a crate
  cannot be pulled through an arrow slit; the prompt reads right; a shot door falls off after a few
  hits" (windowed, owner's); game-ui.md the prompts; ROADMAP.md: "Now" / "Next" (`[mech-1]` built), the test count,
  the measured values; `docs/verify.md`: `--bot-use`, `--bot-hold`, the `Mechanisms:` and `Hold:` lines.

**Checks**
1. Steps 4-7 above (6a included); their lines in the report.

**Must not break**
- G11-G19 (drill and arena sessions have no yard); G16's correction bounds on the playground with the yard.

**Done when**
- [ ] Drills, session-check and `verify.sh full --ci` PASS; soak started; docs as listed; the stage state is
  **Built** (the owner reviews before "Built and reviewed").

**Traps**
- The soak takes about 35 minutes: start it in the background, never wait on it with a sleep loop.

## Values this stage lands

For notes/net.md section 2 and the guide's plan table (card 8): `kProtocolVersion` 12; `EventKind` 37
`MechanismMove`; `EntityKind` 3 `Mechanism`; `PlayerButton` 16 `Use`, `kPlayerButtonMask` 31; `UseResult` 0
`None`, 1 `Stale`, 2 `NoTarget`, 3 `Locked`, 4 `Busy`, 5 `Broken`, 6 `NotUsable`; `MechanismKind` 1 `Door`, 2
`Portcullis`, 3 `Lever` (4-6 reserved: `[mech-3]`); `MechanismFlag` 1 `Locked`, 2 `Jammed`, 4 `Freed`, 8 `Broken`;
`MechanismDescFlag` 1 `OneWay`, 2 `Usable`, 4 `Locked`, 8 `Warded`; owner block +5 B (`lastUseTick` u32,
`useResult` u8); spawn desc 63 B, state 24 B + 11 B per segment; `MechanismMove` 16 B. Session file key
`mechanisms` (server only, not wire). `kPlayerRuleRevision`: unchanged.

Holding (added 2026-11-24): `EventKind` 41 `EntityHold` (12 B), 42 `EntityRelease` (44 B) - 38-40 stay planned for
`[mag-2]`, and `[mech-2]`'s `RopeCut` takes the next free value after 42; `ObjectSpawn` flag 2 `kObjectHeld` (+7 B:
holder u8, relYaw u16, sinceTick u32); `RemotePlayer::holdPitch` i8 (-64..64), `kSnapshotPlayerBytes` 17 -> 18;
`sim::HoldEnd` 1 `Drop`, 2 `Throw`, 3 `HolderDead`, 4 `HolderGone`, 5 `Damaged`, 6 `Pinned` (`kObjectHeld` is never
sent with `kObjectResting`). No new `UseResult`,
`CastRejection` or owner-block byte. Not wire format: `physics::Layer::Held` 6, `sim::UseTargetKind` (0 None, 1
Mechanism, 2 Info, 3 Holdable, 4 Rope reserved for `[mech-2]`, 5 HeldByOther).
