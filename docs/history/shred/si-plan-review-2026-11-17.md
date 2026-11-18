# Shred history: the critic's review of the structural integrity plan (2026-11-17)

Moved verbatim from `docs/design/shred.md` lines 2000-2026 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

The plan it reviewed, with every fix applied, is `docs/design/shred.md` "Structural integrity plan (2026-11-17)".

---

### Review of the plan (2026-11-17)

A critic read the first draft against the code; every finding was checked there. All 15 hold; the fixes above take the
critic's fix unless a row says otherwise.

| # | Finding (checked at) | Verdict | Change |
|---|---|---|---|
| 1 | Craters (`Carve`) and power-0 re-checks reach clients as explosions with a flash; parts they release are pushed (`sim::Blast` always emits the carve, Blast.cpp:600-612; `Client::Impl::Present` opens a non-deferred group for any `CarveSphere`; `PresentBlast` bursts and flashes, Blast.cpp:868-891; every `Detach` carries `BlastPush`, Blast.cpp:481) | Accepted | Crater = `ClearMask` of the carve rule's voxels (`carveAsClear`), re-checks `noCarve`, all `deferred`; `push = false` defined as velocity 0 and no impulses; `Presentation.ImpactsAndRechecksAreDeferred`. The protocol-bump alternative is rejected: no wire change is needed |
| 2 | Striker dents resend whole models and a spent entity credit turns landed bodies into walk-through ghosts (`EmitObjectEvents` `ToRubble` at credit <= 0; client rubble is `Layer::Rubble`, Scene.cpp:66-69; `kSessionCharacterLayers`, Player.cppm:93) | Accepted, fix changed | No striker dents; the crater blast damages **no** object (`objects = false`) rather than skipping only the striker - a crate in the crater just drops in, and no model is resent; dents only on a still receiver; credit >= encoded size before any impact `Damage` / shatter; 0 `ToRubble` by impacts required; wire estimate corrected to about 6.5 KiB |
| 3 | The normal's direction is undefined (A -> B, A = higher motion type, PhysicsTests.cpp:491-492) | Accepted | Oriented into the receiver at quantisation; `Impact.NormalPointsIntoTheReceiver` |
| 4 | Energy alone lets heavy bodies crater at 2-5 m/s, every new sub-shape contact | Accepted | `speedQ`; crater needs 6 m/s, dent 4 m/s; `Impact.SpeedFloorStopsTrenches` |
| 5 | The crater bounds test is the crate's float test on the pre-move centre; a flood into unloaded chunks `Fail`s the session (`CastFitsRegion`, Region.cpp:131-145) | Accepted | `CastFitsRegion` on the final quantised sphere; the test extended to edge contacts and the bottom row |
| 6 | The statue head stays perched on its stub (a 272-voxel node, never a member; no push reaches it) | Accepted | Support under the cut faces crumbles, per released part, up to 64 voxels; `GrazedNeckDropsTheHead` runs without a push |
| 7 | "At rest" by Jolt's sleep skips almost every hit of a burst (`Activate` on create, PhysicsWorld.cpp:697; 0.5 s sleep; 15-tick cast cooldown); 8 checks per cast allowed 16-20 ms uncharged | Accepted | Still = the old body's motion before the hit; pending checks moved from SI-3 into SI-1; 2 checks / 16 tiles per tick, 8 tiles per check |
| 8 | Craters cannot open the stone_dark paving (4.0) and at power 3.0 hollow the stone foundation under it (Region.cpp:280-285, Material.cpp:139-140) | Accepted, fix extended | Gate on the struck voxel, centre at the contact point, **and** power capped at 2.5 (brick): a struck stone pattern line (3.0) would still have let a 3.0 crater undercut the paving beside it |
| 9 | A shatter makes up to 6 walk-through client bricks (2 slots per tick); `shattered` leaks to `Damage` pieces; a both-eligible pair makes two impacts | Accepted, fix changed | Slots asked first, at most `granted + 1` blocks, and the cut simplified to 2-3 slabs across the longest axis (one rule offline and online); pieces' `shattered` set explicitly, but their readyTick stays fresh (they are born touching, so copying the parent's would end their immunity); one impact per unordered pair |
| 10 | Charging impacts to the cast credit ties players' casts to Jolt, against networking stage 6; the record ring does not isolate Jolt | Accepted | A structure credit of their own (shared with SI-3); R5 unamended; record ring dropped |
| 11 | `Publish`'s unstable sort on thread-ordered events picks the surviving contact by arrival (PhysicsWorld.cpp:279-306); the energy floor was applied before quantising | Accepted | Content tie-break (not `stable_sort`: it would rest on Jolt's job order, which we do not control); integer thresholds after quantising; `SameImpactsWithOneOrThreeThreads` |
| 12 | The 4x retry costs up to 11 ticks of credit and breaks the 2^17 arc bound (Structure.cpp:696-699; `kNeckMaxArcPairs` 65 535) for a case never measured | Accepted, fix changed | Retry cut. Re-checks run on the structure credit rather than "only when the cast credit is full": a full credit does not stop one re-check from making debt, and a separate credit leaves the casts alone entirely |
| 13 | `VolumeLoad.EqualsTheTerrainCheck...` compares bedrock (a finite wood-bedrock arc) with `BaseRow` (infinite) | Accepted | Terrain copy on the window's bottom row; bedrock pinned as a known difference |
| 14 | A 1 m drop lands in 27 ticks, inside the immunity; several tests pass on a no-op | Accepted, adjusted | Ledges removed 30 ticks after spawn; the 1 m block is asserted eligible and `belowCrater` (no "dent": striker dents are gone, finding 2); positive controls added to every negative test |
| 15 | The Static path acts on no game or session content (`TemplateObject` has no motion, `SpawnObjectAt` Dynamic, main.cpp dynamic only; only the editor's `spawnMotion`) | Accepted | `BaseRow`, the Static remainder and its test dropped; Static objects are not judged |

Rejected: none of the findings. Rejected within the fixes: the protocol bump for a presentation flag (1: not needed),
the striker-skip field (2: `objects = false` is simpler and resends nothing), `std::stable_sort` (11), "re-check only at
a full cast credit" (12), copying the parent's readyTick (9).
