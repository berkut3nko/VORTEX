# [wp-5] Visible play boundary and the moving window on small regions: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/networking|networking]]

| | |
|---|---|
| Stage | `[wp-5]`: a session client on a small region (the playground) gets the game's moving 48-chunk window and sees 88 m instead of 56 m; the play bounds become visible (a fading grid curtain drawn in the composite pass, a HUD hint near the edge) |
| Design | the project review (game repository, `docs/history/reviews/project-review-2026-11-17.md` section 4, "WP5"); `engine/docs/design/networking.md` "Chunk sync (late join, moving window, repair)" (the "Moving window" bullet) and "Player movement" ("The play area"); `engine/docs/design/view-distance.md` (the paragraph "A session client's window is the game's"); `engine/docs/notes/graphics.md` section 4 recipe A (add a field to `FrameUniforms`) |
| Code read at | engine `bafca16`, game `0e27ea7` (both `llm-tasks`). `[wp-4]` lands before this stage and may have moved lines: always find code with `grep -n`, never by line number |
| Depends on | `[wp-4]` committed (its HUD settings page gets the curtain toggle in card 3) |
| Cards | 4: the moving window on small regions (0.5 d); play bounds to the renderer (0.5 d); the curtain and the HUD hint (1 d); close (0.5 d) |
| Wire / rules / hashes | none: no `kProtocolVersion` bump, `sim::StepPlayer` and `sim::PlayAreaOf` unchanged (no `kPlayerRuleRevision` bump), `buildHash` unchanged, no gate added. One graphics contract changes: `FrameUniforms` gains `playBounds` (card 2) |

## Before you start

- `[wp-4]` is committed in both repositories: `git log --oneline | grep '\[wp-4\]'` and
  `git -C engine log --oneline | grep '\[wp-4\]'` each show its close card.
- `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` exits 0 and `tools/ctest.sh` passes.
- What the code looks like at the start:
  - `engine/src/sim/Region.cppm` / `engine/src/sim/internal/Region.cpp`: `sim::RegionDesc` (`playMinChunk`,
    `playMaxChunk`: play bounds in whole chunks, x and z, [min, max)), `sim::RegionWindowSize` (play bounds + 2 x
    `sim::kServerWindowMarginChunks` (3) per axis), `sim::PlayAreaOf` (the feet area: bounds less
    `PlayerTuning::radius`, 0.3 m), and the client-window family: `sim::ClientWindowMoves(region, windowChunks)`,
    `sim::kClientWindowSlackChunks` (1), `sim::ClientWorldConfig`, `sim::ClientWindowMinChunk`,
    `sim::ClientWindowSize`, `sim::ClientChunkCapacity`. Every one of them branches on `ClientWindowMoves` only.
  - Today `ClientWindowMoves` is true only when the region's window is wider than `windowChunks`:
    ```cpp
    bool ClientWindowMoves(const RegionDesc& region, int32_t windowChunks) {
        if (windowChunks <= 0) return false;
        const glm::ivec3 size = RegionWindowSize(region);
        return size.x > windowChunks || size.z > windowChunks;
    }
    ```
  - `engine/src/voxel/internal/World.cpp` `World::ViewRadius()`: `(min(windowSize.x, windowSize.z) / 2 - 1 -
    recenterSlackChunks) * 32 * voxelSize`. The playground (`engine/tools/server/sessions/playground.session`:
    `play_min = -12 -12`, `play_max = 12 12`, `voxel_size = 0.125`, so 4 m chunks) has a 30 x 30 chunk window:
    (15 - 1 - 0) x 4 = **56 m**. A moving 48-chunk window with slack 1: (24 - 1 - 1) x 4 = **88 m**.
  - The game's session client: `src/Session.cpp`, `kSessionWindowChunks = 48`, `cs.windowChunks =
    kSessionWindowChunks`, `config.windowFollowsCamera = sim::ClientWindowMoves(client.Region(),
    kSessionWindowChunks)`; `SessionGame::OnInit` (Attach, the `Session: joined ...` line), `SessionGame::DrawHud`.
  - The server already handles a client window that reaches beyond its own (`net::Server` `OnWindowUpdate`:
    `Intersect(box, server)`, "every chunk of it is pristine for good"; `OnTerrainWindow`: `TerrainCode::NotInAck`),
    and so does the client (`net::ChunkSync` `neverEdited` for chunks outside the server's window;
    `net::SessionGenerator` generates there even in server terrain mode). Nothing on the net side changes here.
  - Graphics: `engine/src/graphics/Frame.cppm` `graphics::FrameUniforms` (24 members, last `settings`,
    `static_assert(sizeof(FrameUniforms) == 3 * 64 + 21 * 16)` before `[wp-4]`), `graphics::FrameInput`;
    `engine/shaders/include/common.glsl` (the `FrameUniforms` block); `engine/src/graphics/internal/Renderer.cpp`
    `BuildFrameData` (fills `FrameUniforms u`; `origin` = the world's `WindowOrigin()`, render space is relative to
    it); `engine/shaders/composite.frag` (`main`: a sky path that returns early, a surface path, the horizontal edge
    fog last); `engine/tests/graphics/ContractTests.cpp` `LayoutContract.FrameUniformsMatchTheGlslBlock` and the
    `cube::ProxyCube` pattern (a shared `.glsl` file compiled as C++ inside a struct).
  - The engine: `engine/src/engine/Engine.cppm` / `internal/Engine.cpp` (`Engine::Impl`, `Run` fills
    `graphics::FrameInput in` just before `s.renderer.RenderFrame(in)`).

## Decisions (us, 2026-11-22; the owner may veto)

1. **The rule, not a flag.** A client window of `windowChunks > 0` moves whenever it is not exactly the region's
   window: `size.x != windowChunks || size.z != windowChunks`. So the playground (30 chunks) and the drill region
   get the game's 48-chunk window that follows the player; the columns beyond the server's window are generated by
   the client and are pristine for good (already the design of the moving window: "A moving window is not clamped
   to the region"). Why not a `ClientSettings` flag: it would thread a new parameter through five functions and six
   call sites for one caller, and the soak bots (`vortex_bot`, default `--client-window 48`) then cover exactly what
   the game runs. Consequences, accepted: the soak's playground bots move their windows too (`tools/soak.sh` header
   and `docs/verify.md` say so after card 1); `vortex_server`'s in-process bots keep `--client-window 0` (the
   region's fixed window) unless told otherwise, so G11, G12 and the first half of G14 do not change; the unit
   tests with `windowChunks` 0 (every `Match`-based movement test) do not change.
2. **One existing test changes its expected values** (card 1 gives them): `ClientWindow.ARegionThatFitsKeepsTheFixedWindow`
   asserted that a 48-chunk client keeps the fixed window on the 13-chunk test region. Under the new rule "fits"
   means "is exactly the client's size", so the test uses 13 chunks for the fixed case and asserts that 48 moves.
3. **What is drawn: the play bounds themselves**, not the feet area. A new GPU-free `sim::PlayBoundsOf(region)`
   gives them in metres (whole chunks x chunk metres); `sim::PlayAreaOf` stays as it is (it is part of the
   movement rule). The walls of `sim::SetPlayAreaWalls` stand at the same place.
4. **The look**: a translucent cyan sheet with a grid of lines every 8 voxels (1 m at 0.125 m, 0.8 m at 0.1 m,
   1.2 m at 0.15 m; the window origin is a whole chunk, so the lines stay put in the world when the window moves),
   full strength up to 6 m from the camera, gone at 24 m, and only in a band 3-9 m above and below the eye. Drawn
   in `composite.frag` where the view ray crosses a wall in front of the scene (no extra pass, no blended quad:
   L6). It is drawn before TAA, so TAA smooths its lines. The owner judges the look (owner check 15).
5. **The HUD hint**: `EDGE OF THE FIELD`, centred 40 px x text scale above the screen centre, while the feet are
   within 3 m of the feet area's edge (`sim::DistanceToPlayAreaEdge`), amber, fading in as the edge comes nearer.
6. **The toggle**: `graphics::RenderSettings::playBounds` (default true). `[wp-4]`'s HUD settings page gets a row
   `Play bounds: on / off` (card 3). Offline the curtain is drawn only with the test flag `--play-bounds`.
7. **The cost is measured offline at a fixed camera** (`--camera`, `--play-bounds`): sessions refuse `--camera`.
   Budget: the `composite` pass median grows by at most 0.15 ms on the UHD 620 (what a full-screen blended quad
   costs, L6); est. 0.02-0.08 ms.
8. **Skeleton**: card 1 lands the GPU-free declarations of the stage (`sim::PlayBoundsOf`,
   `sim::DistanceToPlayAreaEdge`) with stub bodies and their tests as `DISABLED_`; the graphics pieces land in their
   own cards (a stub `FrameUniforms` member would change the layout contract twice).

## Skeleton

GPU-free, `engine/src/sim/` (declared in card 1, filled in cards 2 and 3):

```cpp
// engine/src/sim/Region.cppm, after PlayAreaOf (export namespace vortex::sim):
/// @brief The play bounds of `region` in world metres, x and z: playMinChunk / playMaxChunk x chunk
/// metres (32 x the voxel size), computed in doubles then rounded to floats as PlayAreaOf does. What
/// the client draws ([wp-5]); PlayAreaOf (the feet area) is these less PlayerTuning::radius.
PlayArea PlayBoundsOf(const RegionDesc& region);

// engine/src/sim/Player.cppm, after struct PlayArea (export namespace vortex::sim):
/// @brief Metres from `feet` (x, z) to the nearest edge of `area`: the smallest of the four
/// distances, negative outside the area, +infinity for an unbounded area (offline). Presentation
/// only (the HUD hint): never an input of StepPlayer.
float DistanceToPlayAreaEdge(const PlayArea& area, const glm::vec2& feet);
```

Graphics and engine (card 2):

```cpp
// engine/src/graphics/Frame.cppm, FrameUniforms, appended after the last member:
glm::vec4 playBounds{0.0f};   ///< min x, min z, max x, max z of the session's play bounds (render space, m); max <= min: none

// engine/src/graphics/Frame.cppm, FrameInput, after hudDim:
/// The session's play bounds in world metres: (min x, min z, max x, max z). max <= min: none (offline).
glm::dvec4 playBounds{0.0};

// engine/src/graphics/Renderer.cppm, RenderSettings, after edgeFog:
bool playBounds = true;              ///< Draw the session's play bounds (the curtain, composite pass).

// engine/src/engine/Engine.cppm, class Engine, after SetImGuiInput:
/// @brief The play bounds the renderer draws (the curtain of [wp-5]); std::nullopt or an unbounded
/// area: none. A session sets sim::PlayBoundsOf(region) once it has joined.
void SetPlayBounds(const std::optional<sim::PlayArea>& bounds);
```

Shader (card 3): new `engine/shaders/shared/bounds.glsl` (GLSL / C++ common subset):

```glsl
float VxGridDistance(float x, float spacing);
float VxCurtainAt(float t, float along, float y, float eyeY, float spacing, float pixelAngle);
float VxCurtainWall(float wall, float roA, float rdA, float roB, float rdB, float lo, float hi,
                    float roY, float rdY, float tScene, float spacing, float pixelAngle);
float VxPlayBoundsCurtain(vec3 ro, vec3 rd, float tScene, vec4 bounds, float spacing, float pixelAngle);
```

New files are picked up by the globs (`engine/CMakeLists.txt` `GLOB_RECURSE` over `src/<dir>/*.cppm|*.cpp`,
`engine/tests/CMakeLists.txt` per test directory, `engine/cmake/VortexShaders.cmake` over `shared/*.glsl`): no CMake
edit in this stage.

Tests of the stage (suite, file, card that enables it):

| Test | File | Card |
|---|---|---|
| `ClientWindow.ASmallRegionsClientWindowMovesAndSees88m` | `engine/tests/net/ClientWindowTests.cpp` | 1 (written and passing in card 1) |
| `ClientWindow.ARegionThatFitsKeepsTheFixedWindow` (changed) | same | 1 |
| `Player.PlayBoundsOfARegionAreItsChunkBounds` | `engine/tests/sim/PlayerTests.cpp` | 2 |
| `Player.DistanceToThePlayAreaEdge` | same | 3 |
| `LayoutContract.FrameUniformsMatchTheGlslBlock` (changed) | `engine/tests/graphics/ContractTests.cpp` | 2 |
| `PlayBounds.TheCurtainShowsWhereTheRayCrossesABoundInFrontOfTheScene` | same | 3 |
| `PlayBounds.NoBoundsBehindTheSceneOrFarAwayDrawNothing` | same | 3 |

## Card 1: The moving 48-chunk client window on small regions

| | |
|---|---|
| Goal | A player joining the playground sees 88 m of terrain with no world end in view, instead of 56 m |
| Depends on | `[wp-4]` committed |
| Effort | 0.5 d (e) |
| Touches | Sim (`vortex.sim:region`, `:player` declarations); tests; docs; wire: no; movement rule: no; buildHash: no |

**Read first** (about 45 KiB)
- `engine/src/sim/Region.cppm`: the block "a session client's window" (`ClientWindowMoves` .. `ClientChunkCapacity`)
- `engine/src/sim/internal/Region.cpp`: `ClientWindowMoves`, `ClientWindowSize`, `ClientWorldConfig`,
  `ClientWindowMinChunk`, `ClientChunkCapacity`
- `engine/src/voxel/internal/World.cpp`: `World::ViewRadius`
- `engine/tests/net/FieldHarness.h`: `kWindow` (12), `WideRegion`, `SmallRegion` (7 x 7 chunks of play: a 13-chunk
  window), `Field::AddBot` (a `windowChunks` of 0 becomes `kWindow`), `Field::Crater`, `Field::Passes`,
  `Field::Drain`, `Field::ExpectSameWorld`, `Walk`
- `engine/tests/net/ClientWindowTests.cpp`: `ClientWindow.ARegionThatFitsKeepsTheFixedWindow` (whole test)
- `engine/docs/design/networking.md` "Chunk sync (late join, moving window, repair)", the "Moving window" bullet
- `engine/docs/notes/net.md`: `grep -n 'A client.s window is the region.s\|moving window follows' engine/docs/notes/net.md`
  and the bullets there

**Files**
- change `engine/src/sim/internal/Region.cpp`: `ClientWindowMoves`
- change `engine/src/sim/Region.cppm`: the doc comments of `ClientWindowMoves` and `ClientWorldConfig`; add the
  declaration of `PlayBoundsOf` (Skeleton)
- change `engine/src/sim/Player.cppm`: add the declaration of `DistanceToPlayAreaEdge` (Skeleton)
- change `engine/src/sim/internal/Region.cpp`: stub `PlayBoundsOf`; `engine/src/sim/internal/Player.cpp`: stub
  `DistanceToPlayAreaEdge`
- change `engine/tests/net/ClientWindowTests.cpp`: the changed test and the new one
- change `engine/tests/sim/PlayerTests.cpp`: the two disabled tests of cards 2 and 3
- change `tools/soak.sh` (header comment only), `docs/verify.md`, `engine/docs/design/networking.md`,
  `engine/docs/design/view-distance.md`, `engine/docs/notes/net.md`

**Interfaces** (exactly)

```cpp
// Region.cppm (comment and declaration unchanged in name and parameters):
/// @brief True when a client whose window is `windowChunks` wide (x and z) is not exactly the
/// region's window: its window then follows its player, and on a region smaller than it reaches
/// beyond the server's window (those columns are pristine for good). False for `windowChunks` 0
/// (the region's window, fixed) and for a region whose window is exactly windowChunks x windowChunks.
bool ClientWindowMoves(const RegionDesc& region, int32_t windowChunks);

// Stubs of the skeleton (Region.cpp and Player.cpp, inside namespace vortex::sim):
PlayArea PlayBoundsOf(const RegionDesc& region) {
    (void)region;
    return PlayArea{};   // [wp-5] card 2 fills this
}
float DistanceToPlayAreaEdge(const PlayArea& area, const glm::vec2& feet) {
    (void)area;
    (void)feet;
    return std::numeric_limits<float>::infinity();   // [wp-5] card 3 fills this
}
```

**Steps**
1. Write the test changes first (they fail before step 2).
2. `ClientWindowMoves`:
   ```cpp
   bool ClientWindowMoves(const RegionDesc& region, int32_t windowChunks) {
       if (windowChunks <= 0) return false;
       const glm::ivec3 size = RegionWindowSize(region);
       return size.x != windowChunks || size.z != windowChunks;
   }
   ```
   Nothing else in `Region.cpp` changes: `ClientWindowSize`, `ClientWorldConfig` (window `windowChunks` x 1 x
   `windowChunks`, `recenterSlackChunks = kClientWindowSlackChunks`), `ClientWindowMinChunk` (centred on the focus,
   never clamped) and `ClientChunkCapacity` (the moving window's cells, clamped to 1024 .. `kMaxRegionStoreSlots`)
   already do the right thing for a window larger than the region's.
3. Add the two skeleton declarations and their stubs (include `<limits>` in `Player.cpp`'s global fragment if it
   is not there).
4. Build; run the focused tests, then all tests.
5. Run a session check and read the client's view radius (Checks 5).
6. Update the documents (Docs).

**Tests**
- `ClientWindow.ARegionThatFitsKeepsTheFixedWindow` (changed, `engine/tests/net/ClientWindowTests.cpp`). Change
  exactly these lines and nothing else:
  - `EXPECT_FALSE(sim::ClientWindowMoves(small, 48));` becomes `EXPECT_TRUE(sim::ClientWindowMoves(small, 48));`,
    and add `EXPECT_FALSE(sim::ClientWindowMoves(small, 13));` (13 = 7 + 2 x 3, the small region's window).
  - `EXPECT_EQ(sim::ClientWorldConfig(small, 48).windowSize, sim::RegionWorldConfig(small).windowSize);` and
    `EXPECT_EQ(sim::ClientChunkCapacity(small, 48), sim::RegionChunkCapacity(small));`: replace 48 by 13.
  - In the session half: `cs.windowChunks = 48;` becomes `cs.windowChunks = 13;`. Every other assertion stays
    (`WindowMoves()` false, the window is `ClientWindowSize(small, 0)` and `RegionSyncBox(small)`, one window update).
  - The other lines (`wide` with 0, 46, 45) stay: the wide region's window is 46, so 46 is fixed and 45 moves.
  - Update the test's first comment: "a region whose window is exactly the client's keeps its window".
- `ClientWindow.ASmallRegionsClientWindowMovesAndSees88m` (new, same file, after the changed test):
  - Pure part. `sim::RegionDesc pg = WideRegion(); pg.playMinChunk = {-12, -12}; pg.playMaxChunk = {12, 12};`
    (the playground's shape at 0.125 m; its window is 30 chunks).
    Assert `sim::ClientWindowMoves(pg, 48)` true, `sim::ClientWindowMoves(pg, 30)` false;
    `sim::ClientWorldConfig(pg, 48).windowSize == glm::ivec3(48, 1, 48)`;
    `sim::ClientWorldConfig(pg, 48).recenterSlackChunks == sim::kClientWindowSlackChunks`.
    Build two worlds as the first test does (`voxel::ChunkStore store(2048); JobSystem jobs(1);
    voxel::World world(config, store, jobs, sim::MakeRegionGenerator(pg, DefaultIds()));`), no loading needed:
    `EXPECT_DOUBLE_EQ(moving.ViewRadius(), 88.0)` for `sim::ClientWorldConfig(pg, 48)` ((24 - 1 - 1) x 4 m) and
    `EXPECT_DOUBLE_EQ(fixed.ViewRadius(), 56.0)` for `sim::RegionWorldConfig(pg)` ((15 - 1 - 0) x 4 m).
  - Session part. `Field f(Settings(small), Lossy()); ASSERT_TRUE(f.started); net::ClientSettings cs;
    cs.windowChunks = 20;` (larger than the 13-chunk region window); `Field::Bot& bot =
    f.AddBot(Walk(small, {{8.0f, 8.0f}, {-8.0f, 6.0f}}), cs); ASSERT_TRUE(f.Join());`
    Assert: `bot.client->WindowMoves()`; `bot.simulation->World().WindowSize() == sim::ClientWindowSize(small, 20)`;
    `bot.client->SyncWindow() == net::RegionSyncBox(small)` (the 20-chunk window holds the server's 13-chunk one
    wherever the player stands inside the play bounds). Then `f.Crater(2.0, 2.0); f.Passes(400);
    ASSERT_TRUE(f.Drain());` and assert `bot.client->Stats().windowUpdates >= 2u` (the walk to x = 8 m is chunk 2,
    more than the slack of 1 from the start chunk 0: at least one step after the Welcome's update),
    `bot.simulation->World().WindowMinChunk().x < net::RegionSyncBox(small).min.x` (the window reaches past the
    server's), and `f.ExpectSameWorld(bot)`.
- `Player.PlayBoundsOfARegionAreItsChunkBounds` (`engine/tests/sim/PlayerTests.cpp`, written now as
  `DISABLED_PlayBoundsOfARegionAreItsChunkBounds`, enabled in card 2): the region of
  `Player.PlayAreaOfARegionKeepsTheReachInsideTheWindow` (copy its setup: `playMinChunk = {-4, -5}`, `playMaxChunk =
  {3, 2}`, flat, the three voxel sizes 0.1 / 0.125 / 0.15). For each: `const sim::PlayArea b =
  sim::PlayBoundsOf(region); const float chunk = 32.0f * voxelSize;` assert `b.Bounded()`,
  `EXPECT_FLOAT_EQ(b.min.x, -4.0f * chunk)`, `b.min.y == -5 chunk`, `b.max.x == 3 chunk`, `b.max.y == 2 chunk`
  (all `EXPECT_FLOAT_EQ`), and `EXPECT_NEAR(sim::PlayAreaOf(region, sim::PlayerTuning{}).min.x, b.min.x + 0.3f,
  1.0e-5f)` (and the same for `max.y` with `- 0.3f`).
- `Player.DistanceToThePlayAreaEdge` (same file, written now as `DISABLED_DistanceToThePlayAreaEdge`, enabled in
  card 3): `sim::PlayArea a; a.min = {-10.0f, -20.0f}; a.max = {10.0f, 20.0f};` then
  `EXPECT_FLOAT_EQ(DistanceToPlayAreaEdge(a, {0, 0}), 10.0f)`, `({9, 0})` 1.0f, `({0, 19.5f})` 0.5f,
  `({-9.75f, 19.0f})` 0.25f, `({11, 0})` -1.0f, `({0, -25})` -5.0f; and
  `EXPECT_TRUE(std::isinf(DistanceToPlayAreaEdge(sim::PlayArea{}, {0, 0})))`.

**Log lines and flags**: none new. Read: the game's exit line `World: generation ... voxel 0.125 m, view radius
88.0 m` and `World: window 48 x 44 x 48 chunks (rows ..., follows the camera) ...` (`Engine::Run`'s exit summary of
a headless run), and `Session: joined ... window 48 x 44 x 48 chunks (follows the player; surface) ...`.

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` - exit 0, no warning
2. `tools/ctest.sh -R 'ClientWindow\.|Movement\.|Player\.' --output-on-failure` - all pass;
   `Movement.NoCorrectionAbove2cmOnFlatGround` still reports 0 corrections (it uses `windowChunks` 0)
3. `tools/ctest.sh` - all pass
4. `tools/doc-check.sh --build build` - PASS (update ROADMAP's test count: one new `TEST(`, two disabled ones count
   as macros too; `ctest -N --test-dir build | tail -1` gives N)
5. `PORT=27065 tools/session-check.sh` - prints `PASS`; then
   `grep -h 'view radius\|World: window\|Session: joined' build/session-check/a.log` shows `view radius 88.0 m`,
   `48 x 44 x 48` and `follows the player`. Look at `build/session-check/a.png` and `b.png` if you can read images
   (terrain to the horizon fog, no cut-off edge).
6. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS

**Docs**
- `engine/docs/design/networking.md` "Chunk sync (late join, moving window, repair)", the "Moving window" bullet:
  "on a region whose window is not exactly the client's (`sim::ClientWindowMoves`; the game's is 48 chunks) the
  window follows the player ...; on a smaller region it reaches beyond the server's window, whose columns the client
  generates (pristine for good) ([wp-5]: the playground sees 88 m). `windowChunks` 0 keeps the region's fixed window."
- `engine/docs/design/view-distance.md` paragraph "A session client's window is the game's": replace "when the
  region is wider than it" and "a region that fits (playground, drill) keeps the region's fixed window" with the new
  rule (every region whose window is not 48 x 48, the playground and the drill region included).
- `engine/docs/notes/net.md`: the bullet "A client's window is the region's when it fits ..." says "when it is exactly
  `ClientSettings::windowChunks` wide".
- `docs/verify.md`: the session bullet "On a region wider than 48 chunks (`arena250.session`) the client's window
  follows the camera" becomes "On every region whose window is not exactly 48 chunks (the playground, the arena)";
  the soak paragraph's "`playground` (fixed windows, ..." becomes "(moving 48-chunk windows, ...".
- `tools/soak.sh` header: `playground  playground.session (fixed windows)` becomes `(moving 48-chunk windows since
  [wp-5])`. No code change in the script.

**Must not break**
- CLAUDE.md section 5 "Sessions": "A session's world comes from `Welcome`'s `RegionDesc` through
  `sim::RegionWorldConfig`" - the client's world config still starts from it (`ClientWorldConfig` calls
  `RegionWorldConfig` first); `Session.ClientBuildsItsWorldFromWelcomeAndRefusesAnyOther`,
  `Session.ServerWindowIsThePlayBoundsPlusThreeChunks` stay green.
- "Chunk sync": hashes compare over the sync box (window inside the server's) - `ClientWindow.HashesCompareOverTheSameBoxAtTheSameOpSeq`.
- "Movement": `Movement.NoCorrectionAbove2cmOnFlatGround` at 0 corrections; `kPlayerRuleRevision` unchanged.
- Lesson L12 (the op ring): nothing here touches the ring; a larger window only means more `neverEdited` chunks.
- The store: the game keeps 2048 slots free (`kSessionStoreHeadroom`); a 48-chunk window on the playground takes
  about what it takes on the arena (G13 measured at most about 5600 slots used).

**Done when**
- [ ] `ClientWindowMoves` has the new rule; nothing else in `Region.cpp` changed besides the stubs
- [ ] both ClientWindow tests pass; the two Player tests exist as `DISABLED_`
- [ ] session-check PASS with `view radius 88.0 m` in the client logs
- [ ] verify quick PASS; doc-check PASS; the five documents updated

**Traps**
- Do not touch `ClientWindowMinChunk` to "clamp the window to the region": the design keeps it unclamped, and the
  server already answers cells outside its window as pristine.
- `Field::AddBot` turns `windowChunks` 0 into 12: to get the fixed window on `SmallRegion` you must pass 13.
- `ViewRadius` uses `recenterSlackChunks`: a world built from `RegionWorldConfig` has slack 0, one from
  `ClientWorldConfig` of a moving window slack 1. The test values 56 and 88 depend on that.
- If session-check fails on its correction bound (G16), do not raise `MAX_CORRECTIONS`: report `Status: blocked`
  with both clients' `Session:` correction lines.

## Card 2: Play bounds to the renderer

| | |
|---|---|
| Goal | The renderer knows the session's play bounds every frame (nothing is drawn yet) |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | Sim (`PlayBoundsOf`), Graphics (`FrameUniforms`, `FrameInput`, `RenderSettings`, `BuildFrameData`), Engine (`SetPlayBounds`), shaders (`common.glsl` block), the game (`src/Session.cpp`); wire: no; movement rule: no |

**Read first** (about 40 KiB)
- `engine/src/graphics/Frame.cppm`: `FrameUniforms`, its `static_assert`, `FrameInput`
- `engine/shaders/include/common.glsl`: the `FrameUniforms` block (top of the file)
- `engine/src/graphics/internal/Renderer.cpp`: `BuildFrameData` from `const glm::dvec3 origin =` to
  `u.gridMinChunk = ...` (`grep -n 'BuildFrameData\|u.gridMinChunk' engine/src/graphics/internal/Renderer.cpp`)
- `engine/tests/graphics/ContractTests.cpp`: `LayoutContract.FrameUniformsMatchTheGlslBlock`
- `engine/docs/notes/graphics.md` section 4 recipe A
- `engine/src/engine/Engine.cppm` (class `Engine`), `engine/src/engine/internal/Engine.cpp`: `struct Engine::Impl`
  and the block in `Run` that fills `graphics::FrameInput in`
- `engine/src/sim/internal/Region.cpp`: `PlayAreaOf` (the arithmetic to copy)
- `src/Session.cpp`: `SessionGame::OnInit` up to the `Session: joined` log line

**Files**
- change `engine/src/sim/internal/Region.cpp`: fill `PlayBoundsOf`
- change `engine/src/graphics/Frame.cppm`, `engine/shaders/include/common.glsl`,
  `engine/src/graphics/Renderer.cppm`, `engine/src/graphics/internal/Renderer.cpp`
- change `engine/src/engine/Engine.cppm`, `engine/src/engine/internal/Engine.cpp`
- change `engine/tests/graphics/ContractTests.cpp`, `engine/tests/sim/PlayerTests.cpp` (enable the test)
- change `src/Session.cpp`

**Interfaces**: as in "Skeleton" (graphics and engine block) plus:

```cpp
// Region.cpp
PlayArea PlayBoundsOf(const RegionDesc& region) {
    const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(region.terrain.voxelSize);
    PlayArea b;
    b.min = glm::vec2(static_cast<float>(region.playMinChunk.x * chunkMetres), static_cast<float>(region.playMinChunk.y * chunkMetres));
    b.max = glm::vec2(static_cast<float>(region.playMaxChunk.x * chunkMetres), static_cast<float>(region.playMaxChunk.y * chunkMetres));
    return b;
}
```

**Steps**
1. Fill `PlayBoundsOf` as above; rename `DISABLED_PlayBoundsOfARegionAreItsChunkBounds` to
   `PlayBoundsOfARegionAreItsChunkBounds`.
2. `FrameUniforms`: append `glm::vec4 playBounds{0.0f};` as the new last member (after `settings`, or after the last
   member `[wp-4]` appended, if any). Raise the `static_assert` by 16 bytes (from `3 * 64 + 21 * 16` it becomes
   `3 * 64 + 22 * 16`; if `[wp-4]` changed it, add one more `16`).
3. `common.glsl`: append in the same position `vec4 playBounds;           // xy = min x / z, zw = max x / z of the
   session's play bounds (render space, m); zw <= xy: none (offline)`.
4. `FrameInput`: add `glm::dvec4 playBounds{0.0};` after `hudDim` (Skeleton comment).
5. `RenderSettings`: add `bool playBounds = true;` after `edgeFog` (Skeleton comment).
6. `BuildFrameData`, right after `u.gridMinChunk = ...`:
   ```cpp
   // [wp-5]: the session's play bounds in render space (x / z); zero = none.
   u.playBounds = glm::vec4(0.0f);
   const glm::dvec4 pb = input.playBounds;
   if (settings.playBounds && std::isfinite(pb.x) && std::isfinite(pb.y) && std::isfinite(pb.z) && std::isfinite(pb.w) &&
       pb.z > pb.x && pb.w > pb.y) {
       u.playBounds = glm::vec4(static_cast<float>(pb.x - origin.x), static_cast<float>(pb.y - origin.z),
                                static_cast<float>(pb.z - origin.x), static_cast<float>(pb.w - origin.z));
   }
   ```
   (`pb.y` / `pb.w` are world z; `origin` is the render origin already computed in this function.)
7. `Engine::Impl`: add `glm::dvec4 playBounds{0.0};`. `Engine::SetPlayBounds`:
   ```cpp
   void Engine::SetPlayBounds(const std::optional<sim::PlayArea>& bounds) {
       if (!bounds || !bounds->Bounded()) {
           m_Impl->playBounds = glm::dvec4(0.0);
           return;
       }
       m_Impl->playBounds = glm::dvec4(bounds->min.x, bounds->min.y, bounds->max.x, bounds->max.y);
   }
   ```
   In `Run`, where `in` is filled (after `in.hudDim = ...`): `in.playBounds = s.playBounds;`. Add `#include
   <optional>` to `Engine.cppm`'s global fragment if it is missing.
8. `ContractTests.cpp` `LayoutContract.FrameUniformsMatchTheGlslBlock`: `EXPECT_EQ(block.members.size(), 24u)`
   becomes 25 (or the count you find + 1); add `VX_EXPECT_FRAME_MEMBER(playBounds);` after the last
   `VX_EXPECT_FRAME_MEMBER`; the last-member check becomes `EXPECT_EQ(block.members.back().name, "playBounds");` and
   `... + sizeof(glm::vec4)`.
9. `src/Session.cpp` `SessionGame::OnInit`, right after `m_Attached = true;`:
   `engine.SetPlayBounds(sim::PlayBoundsOf(m_Client.Region()));`
10. Build, run the checks.

**Tests**
- `Player.PlayBoundsOfARegionAreItsChunkBounds` enabled (card 1 wrote it).
- `LayoutContract.FrameUniformsMatchTheGlslBlock` (changed as in step 8): every shader that declares `u_Frame`
  matches the C++ offsets, 25 members, last `playBounds`.

**Log lines and flags**: none.

**Checks**
1. build - exit 0, no warning
2. `tools/ctest.sh -R 'LayoutContract\.|ShaderContract\.|Player\.' --output-on-failure` - all pass
3. `tools/ctest.sh` - all pass
4. `tools/doc-check.sh --build build` - PASS
5. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS (nothing looks different yet: no shader reads `playBounds`)

**Docs**
- `engine/docs/notes/graphics.md` section 4 recipe A, step 3: "member count (25)"; and the trap that names the last
  member, if it does.
- `engine/docs/ARCHITECTURE.md` section 5 (`grep -n 'FrameUniforms' engine/docs/ARCHITECTURE.md`): if the uniform
  block's members are listed there, add `playBounds` with its meaning.

**Must not break**
- CLAUDE.md "Rendering and data": `FrameUniforms` mirrored on both sides (`LayoutContract.FrameUniformsMatchTheGlslBlock`);
  only mat4 / vec4 / ivec4 members (`engine/docs/notes/graphics.md`); rendering relative to `World::WindowOrigin()`.
- "Movement": `PlayAreaOf` is untouched (it is the movement rule's input).

**Done when**
- [ ] `FrameUniforms::playBounds` on both sides at the same offset; the contract test passes
- [ ] `Engine::SetPlayBounds` exists and the session client calls it once after `Attach`
- [ ] verify quick PASS

**Traps**
- The uniform must be in render space and recomputed every frame: a moving window moves the render origin by whole
  chunks.
- `PlayArea` uses `glm::vec2` with `.y` meaning z. Keep that mapping in `SetPlayBounds` and in step 6.
- Do not use `PlayAreaOf` for the drawing: it is 0.3 m inside the walls.

## Card 3: The visible boundary: curtain, HUD hint, toggle, cost

| | |
|---|---|
| Goal | Players see where the play area ends: a faint glowing grid where they look at the edge, and an "EDGE OF THE FIELD" hint when they are about to reach it |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | shaders (`shared/bounds.glsl` new, `composite.frag`), graphics tests, Sim (`DistanceToPlayAreaEdge`), the game (`src/Session.cpp`, `src/main.cpp`, the `[wp-4]` settings page); wire: no; movement rule: no |

**Read first** (about 55 KiB)
- `engine/shaders/composite.frag` (whole file, 306 lines)
- `engine/shaders/shared/cube.glsl`, `engine/shaders/shared/vx_cpp.h`, `engine/shaders/shared/vx_glsl.glsl` (the
  GLSL / C++ subset: `f` suffixes, no swizzles, `VX_CONST_FLOAT`)
- `engine/tests/graphics/ContractTests.cpp`: `namespace cube` / `ProxyCube` at the top, and `TEST(ProxyCube, ...)`
- `src/Session.cpp`: `SessionGame::OnInit`, `SessionGame::DrawHud`, the offline-flag list in `RunSession`
  (`for (const char* offline : {"--voxel-size", ...`)
- `src/main.cpp`: `ParseCameraPose`, the command-line block at the end of `main` (`args.Has("--no-hud")` ...)
- `[wp-4]`'s HUD settings page: `grep -rn 'Settings' src/*.cpp | grep -i 'page\|menu'` to find it, then read how its
  vsync row toggles `RenderSettings::vsync`
- `engine/docs/design/game-ui.md` "API the game uses"

**Files**
- new `engine/shaders/shared/bounds.glsl`
- change `engine/shaders/composite.frag`
- change `engine/tests/graphics/ContractTests.cpp` (the curtain compiled as C++, two tests)
- change `engine/src/sim/internal/Player.cpp` (fill `DistanceToPlayAreaEdge`), `engine/tests/sim/PlayerTests.cpp`
  (enable the test)
- change `src/Session.cpp` (hint, `--play-bounds` refused), `src/main.cpp` (`--play-bounds`), the `[wp-4]` settings
  page file

**Interfaces**

`engine/shaders/shared/bounds.glsl`, the whole file:

```glsl
// =============================================================================
//  Play-bounds curtain ([wp-5]) - shared by composite.frag and C++ (see shared/vx_cpp.h).
// =============================================================================
//  A session's play bounds are four vertical walls (x = min x, x = max x, z = min z, z = max z).
//  Where the view ray crosses one in front of the scene it shows a faint sheet with a grid of
//  lines, strongest near the camera and near eye height. Render space, metres.
#ifndef VX_SHARED_BOUNDS_GLSL
#define VX_SHARED_BOUNDS_GLSL

VX_CONST_FLOAT(VX_CURTAIN_NEAR, 6.0f)          // full strength up to this distance (m)
VX_CONST_FLOAT(VX_CURTAIN_FAR, 24.0f)          // gone from this distance on (m)
VX_CONST_FLOAT(VX_CURTAIN_BAND_NEAR, 3.0f)     // full strength up to this height above / below the eye (m)
VX_CONST_FLOAT(VX_CURTAIN_BAND_FAR, 9.0f)      // gone from this height difference on (m)
VX_CONST_FLOAT(VX_CURTAIN_BASE, 0.10f)         // opacity of the sheet between the lines
VX_CONST_FLOAT(VX_CURTAIN_LINE, 0.45f)         // opacity added on a line
VX_CONST_FLOAT(VX_CURTAIN_MIN_LINE, 0.02f)     // a line is at least this wide on each side (m)

// Distance (m) from x to the nearest multiple of spacing.
float VxGridDistance(float x, float spacing) {
    return abs(fract(x / spacing + 0.5f) - 0.5f) * spacing;
}

// Opacity of the curtain where the ray crosses a wall at distance t: `along` is the horizontal
// coordinate on the wall, y the height of the crossing, eyeY the camera's height.
float VxCurtainAt(float t, float along, float y, float eyeY, float spacing, float pixelAngle) {
    float fade = (1.0f - smoothstep(VX_CURTAIN_NEAR, VX_CURTAIN_FAR, t)) *
                 (1.0f - smoothstep(VX_CURTAIN_BAND_NEAR, VX_CURTAIN_BAND_FAR, abs(y - eyeY)));
    if (fade <= 0.0f) return 0.0f;
    float width = max(t * pixelAngle, VX_CURTAIN_MIN_LINE);
    float d = min(VxGridDistance(along, spacing), VxGridDistance(y, spacing));
    float line = 1.0f - smoothstep(width, 2.0f * width, d);
    return fade * (VX_CURTAIN_BASE + VX_CURTAIN_LINE * line);
}

// One wall, the plane where axis A equals `wall`; B is the other horizontal axis. Counts when the
// crossing lies in front of the camera and of the scene (0 < t < tScene) and B there is in [lo, hi].
float VxCurtainWall(float wall, float roA, float rdA, float roB, float rdB, float lo, float hi,
                    float roY, float rdY, float tScene, float spacing, float pixelAngle) {
    if (abs(rdA) < 1.0e-6f) return 0.0f;
    float t = (wall - roA) / rdA;
    if (!(t > 0.0f) || !(t < tScene)) return 0.0f;
    float along = roB + rdB * t;
    if (along < lo || along > hi) return 0.0f;
    return VxCurtainAt(t, along, roY + rdY * t, roY, spacing, pixelAngle);
}

// Opacity (0..1) of the curtain along the ray ro + rd * t (rd a unit vector) up to the scene at tScene.
// bounds = (min x, min z, max x, max z); none (0) when max <= min. Overlapping walls combine as
// layers: 1 - (1 - a0)(1 - a1)...
float VxPlayBoundsCurtain(vec3 ro, vec3 rd, float tScene, vec4 bounds, float spacing, float pixelAngle) {
    if (!(bounds.z > bounds.x) || !(bounds.w > bounds.y)) return 0.0f;
    float keep = 1.0f;
    keep *= 1.0f - VxCurtainWall(bounds.x, ro.x, rd.x, ro.z, rd.z, bounds.y, bounds.w, ro.y, rd.y, tScene, spacing, pixelAngle);
    keep *= 1.0f - VxCurtainWall(bounds.z, ro.x, rd.x, ro.z, rd.z, bounds.y, bounds.w, ro.y, rd.y, tScene, spacing, pixelAngle);
    keep *= 1.0f - VxCurtainWall(bounds.y, ro.z, rd.z, ro.x, rd.x, bounds.x, bounds.z, ro.y, rd.y, tScene, spacing, pixelAngle);
    keep *= 1.0f - VxCurtainWall(bounds.w, ro.z, rd.z, ro.x, rd.x, bounds.x, bounds.z, ro.y, rd.y, tScene, spacing, pixelAngle);
    return 1.0f - keep;
}

#endif // VX_SHARED_BOUNDS_GLSL
```

`composite.frag` additions:

```glsl
#include "shared/bounds.glsl"                       // after #include "shared/shading.glsl"

const vec3 VX_CURTAIN_COLOR = vec3(0.45, 0.85, 1.25);   // linear, before exposure: a cool glow

// The play bounds' curtain over `color` for the view ray rd up to distance t ([wp-5]).
vec3 ApplyPlayBounds(vec3 color, vec3 rd, float t) {
    float spacing = 8.0 * u_Frame.cameraPosVoxel.w;                     // 8 voxels: whole chunks divide by it
    float pixelAngle = 2.0 * length(u_Frame.cameraUp.xyz) * u_Frame.renderSize.w;
    float a = VxPlayBoundsCurtain(u_Frame.cameraPos.xyz, rd, t, u_Frame.playBounds, spacing, pixelAngle);
    return mix(color, VX_CURTAIN_COLOR, a);
}
```

```cpp
// Player.cpp
float DistanceToPlayAreaEdge(const PlayArea& area, const glm::vec2& feet) {
    if (!area.Bounded()) return std::numeric_limits<float>::infinity();
    return std::min(std::min(feet.x - area.min.x, area.max.x - feet.x), std::min(feet.y - area.min.y, area.max.y - feet.y));
}
```

**Steps**
1. Write `bounds.glsl` as given. In `ContractTests.cpp`, next to `namespace cube`, add
   ```cpp
   namespace curtain {
       using namespace glm;
       using uint = unsigned int;
       /// shaders/shared/bounds.glsl compiled as C++: the very text composite.frag runs ([wp-5]).
       struct Curtain {
   #include "shared/bounds.glsl"
       };
   }
   ```
   and the two `PlayBounds` tests (below). Build and run them before touching `composite.frag`. If `abs`, `min` or
   `max` is ambiguous in the C++ build, do what `shared/trace.glsl` and `shared/shading.glsl` do (they compile in
   the same way); do not change the GLSL meaning.
2. `composite.frag`: add the include and `ApplyPlayBounds` (above `void main()`). In `main`:
   - sky path: after `if (debugView == VX_DEBUG_NONE) sky = SkyColor(rd);` add
     `if (debugView == VX_DEBUG_NONE) sky = ApplyPlayBounds(sky, rd, VX_SKY_DISTANCE);`
   - surface path: after the edge-fog block (`if (u_Frame.jitter.w > 0.0) { ... }`) and before `o_Color = vec4(color,
     1.0);` add `color = ApplyPlayBounds(color, rd, t);`
   - nothing in the debug views. `VxPlayBoundsCurtain` returns at once when `playBounds` is zero (offline): every
     pixel of the screen takes the same branch.
3. `DistanceToPlayAreaEdge` as given; enable `Player.DistanceToThePlayAreaEdge`.
4. The HUD hint, `src/Session.cpp`:
   - members: `sim::PlayArea m_PlayArea;` and the constant `constexpr float kEdgeHintMetres = 3.0f;` (in the file's
     anonymous namespace, beside `kSessionWindowChunks`).
   - `OnInit`, after `engine.SetPlayBounds(...)` (card 2): `m_PlayArea = sim::PlayAreaOf(m_Client.Region(),
     m_Client.Tuning());`
   - `DrawHud`, inside `if (!paused) {`, after the `Cast not run` text:
     ```cpp
     // [wp-5]: the edge of the play area is near (the walls stop the player there).
     const float edge = sim::DistanceToPlayAreaEdge(m_PlayArea, glm::vec2(m_Feet.x, m_Feet.z));
     if (m_Client.Predicting() && edge < kEdgeHintMetres) {
         const float nearness = 1.0f - std::clamp(edge / kEdgeHintMetres, 0.0f, 1.0f);
         const auto alpha = static_cast<uint8_t>(120.0f + 135.0f * nearness);
         hud.Text(size * 0.5f - glm::vec2(0.0f, 40.0f * s), "EDGE OF THE FIELD", Rgba(255, 214, 120, alpha), scale, Align::Center, shadow);
     }
     ```
     (`m_Feet` is the predicted feet position the view uses; check its name with `grep -n 'm_Feet' src/Session.cpp`.)
5. `--play-bounds MINX,MINZ,MAXX,MAXZ` (offline, metres; for screenshots and the cost measurement):
   - `src/main.cpp`: a parser like `ParseCameraPose` (`ParsePlayBounds(const std::string&) ->
     std::optional<glm::dvec4>`: exactly four comma-separated numbers, max > min on both axes, else nullopt). In
     `main`, after `if (args.Has("--no-hud")) ...`: if `--play-bounds` is given and parses,
     `engine.SetPlayBounds(sim::PlayArea{glm::vec2(b.x, b.y), glm::vec2(b.z, b.w)})` (build the `PlayArea` by
     setting `min` and `max`); if it does not parse, `Log::Error("--play-bounds expects minX,minZ,maxX,maxZ (metres,
     max > min): '{}'", text)` and `return 2`.
   - `src/Session.cpp` `RunSession`: add `"--play-bounds"` to the list of offline-only flags (a session's bounds are
     the server's).
   - the comment block at the top of `src/main.cpp` ("Automation:") names the flag in one sentence.
6. The toggle: in `[wp-4]`'s HUD settings page add a row `Play bounds` with values `on` / `off`, bound to
   `engine.Renderer().Settings().playBounds`, exactly as that page's vsync row is done (same widget, same
   persistence if the page saves its settings). If no such page exists in the code, skip this step and say so under
   "Deviations".
7. Measure (Checks 5) and look at the screenshots.

**Tests**
- `PlayBounds.TheCurtainShowsWhereTheRayCrossesABoundInFrontOfTheScene` (`ContractTests.cpp`): `curtain::Curtain c;
  const glm::vec4 b(-10.0f, -10.0f, 10.0f, 10.0f);` ray `ro = (0, 1.6, 0)`, `rd = (1, 0, 0)`, `tScene = 1000`,
  `spacing = 1.0f`, `pixelAngle = 0.001f`:
  - `EXPECT_NEAR(c.VxPlayBoundsCurtain(ro, rd, 1000.0f, b, 1.0f, 0.001f), 0.4806f, 1.0e-3f)`. Computed: the wall
    x = 10 at t = 10; `along` = z = 0 lies on a line (distance 0), so `line` = 1; fade = 1 - smoothstep(6, 24, 10) =
    1 - (0.2222^2 x (3 - 0.4444)) = 1 - 0.1262 = 0.8738; height difference 0; 0.8738 x (0.10 + 0.45) = 0.4806.
  - Between lines: `ro = (0, 1.6, 0.5)`: `along` = 0.5 (distance 0.5), y = 1.6 (distance 0.4): `line` = 0, so
    `EXPECT_NEAR(..., 0.0874f, 1.0e-3f)` (0.8738 x 0.10).
  - Looking along -z from (3, 1.6, 0): the wall z = -10 at t = 10, `along` = x = 3 on a line: 0.4806 again.
- `PlayBounds.NoBoundsBehindTheSceneOrFarAwayDrawNothing` (same file): with the first ray,
  `EXPECT_EQ(..., 0.0f)` for: `tScene = 5` (the scene is in front of the wall); `bounds = vec4(0)` (offline);
  `bounds = (-30, -10, 30, 10)` (the wall at t = 30, past `VX_CURTAIN_FAR`); `ro = (0, 20, 0)`, `rd =
  normalize(1, -1, 0)`... no: keep it simple, `ro = (0, 1.6, 0)`, `rd = (1, 0, 0)`, `bounds = (-10, 5, 10, 10)` (the
  ray's z = 0 is outside [5, 10], and the z walls are parallel to the ray).
- `Player.DistanceToThePlayAreaEdge` enabled (values in card 1).

**Log lines and flags**
- `--play-bounds MINX,MINZ,MAXX,MAXZ` (offline only; a session refuses it with exit 2). Add it to `docs/verify.md`'s
  flag table of the game.

**Checks**
1. build - exit 0, no warning (engine, game, shaders)
2. `tools/ctest.sh -R 'PlayBounds\.|LayoutContract\.|ShaderContract\.|Player\.' --output-on-failure` - all pass
3. `tools/ctest.sh` - all pass; `tools/doc-check.sh --build build` - PASS (test count)
4. Screenshot: `./build/Shredborne --headless --validation --frames 700 --camera 0,14,0,0,-0.15 --play-bounds
   -48,-10,48,48 --screenshot build/wp5-curtain.png` - exit 0; the wall z = -10 is 10 m ahead: a faint grid in the
   middle of the picture. If the camera is inside a hill (a grey picture), move it up: `0,24,0,0,-0.3`, and say so.
5. Cost (UHD 620, two runs, nothing else running; `--frames 700` so that "World: window filled" comes first and the
   median covers the last 120 frames):
   - A: `./build/Shredborne --headless --frames 700 --camera 0,14,0,0,-0.15 2>&1 | grep 'GPU composite'`
   - B: the same with `--play-bounds -48,-10,48,48`
   - Record both `GPU composite ... (median X ms ...)` values; B - A must be at most 0.15 ms. Run each twice if the
     difference is near the limit and report all four numbers.
6. Session: `PORT=27065 tools/session-check.sh` - PASS; look at `a.png` and `b.png` (the clients stand on the
   plaza, about 40 m from the bounds: no curtain expected there; it must not appear in the sky either).
7. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS; look at G3's screenshot (offline: no curtain).

**Docs**
- `engine/docs/design/game-ui.md` "API the game uses" (or the section listing the session HUD): the hint, its
  threshold (3 m) and colour.
- `engine/docs/notes/graphics.md`: `playBounds` in the `FrameUniforms` notes; `shared/bounds.glsl` in the file map
  (section 1) as shared GLSL / C++ code.
- `docs/verify.md`: the `--play-bounds` flag row.

**Must not break**
- L6: no full-screen blended quad; the curtain is a few ALU operations inside the composite pass.
- CLAUDE.md "Rendering and data": `shared/*.glsl` is compiled as GLSL and as C++: only constructs valid in both.
  Descriptor bindings unchanged (`ShaderContract.*`): `composite.frag` uses no new resource.
- "HUD": the hint is HUD text (display colours), not ImGui.
- The movement rule: `DistanceToPlayAreaEdge` is presentation only; `StepPlayer` never calls it.

**Done when**
- [ ] the curtain shows in the `--play-bounds` screenshot and never offline without the flag
- [ ] the composite cost difference is measured and at most 0.15 ms (both numbers in the report)
- [ ] the hint code is in `DrawHud`; the settings row exists (or the deviation is reported)
- [ ] all tests pass; verify quick PASS

**Traps**
- `t` in `composite.frag` is the distance along the unit ray `rd` (`VxCameraRay` normalises): the fade distances
  are metres. The sky path has no `t`: use `VX_SKY_DISTANCE`.
- Do not draw the curtain in the debug views (their `return` comes earlier; leave it so).
- `fract` of a negative number is positive in GLSL and glm alike: `VxGridDistance` works for negative coordinates.
- The HUD hint threshold uses the feet area (`PlayAreaOf`), where the player actually stops, not the drawn bounds.

## Card 4: Close [wp-5]

| | |
|---|---|
| Goal | The stage is verified, documented and handed to the owner for the windowed look |
| Depends on | cards 1-3 |
| Effort | 0.5 d (e) (plus the background soak) |
| Touches | docs only (and fixes of anything the checks find, each as its own `card 4 fix` commit) |

**Read first** (about 30 KiB)
- `engine/docs/ROADMAP.md` "Now", "Next" item 3, "Not a game yet", "State" (the Sessions row: "an invisible wall at
  the play bounds ([wp-5] shows it); 56 m on the playground ([wp-5])"), "Performance and measured values"
- `docs/owner-checks.md` item 15
- `engine/docs/design/networking.md` "Player movement", the bullet "The play area (WP1)"

**Files**: `engine/docs/ROADMAP.md`, `engine/docs/design/networking.md`, `docs/owner-checks.md`,
`engine/docs/README.md` (only if a stage row needs it), `engine/docs/design/game-ui.md` (if card 3 missed it).

**Steps**
1. `grep -rn 'DISABLED_' engine/tests tests | grep -i 'PlayBounds\|DistanceToThePlayAreaEdge'` prints nothing.
2. Run the checks below in order; fix what fails in `[wp-5] card 4 fix: ...` commits.
3. Documents:
   - `networking.md` "The play area" bullet: add one sentence: the client draws the play bounds (`sim::PlayBoundsOf`,
     `Engine::SetPlayBounds`, the composite pass's curtain, `shared/bounds.glsl`) and shows `EDGE OF THE FIELD`
     within 3 m of the feet area's edge ([wp-5]).
   - `docs/owner-checks.md` item 15: "(WP1, [wp-5].) The play bounds show as a faint cyan grid where you look at them
     from closer than about 20 m, and `EDGE OF THE FIELD` appears in amber within 3 m of the edge: run (Shift)
     straight at the edge ..." - keep the rest of the check, drop "(nothing marks them until ROADMAP [wp-5])", and
     add: "Look along the edge and at it from 1, 5 and 20 m: the grid is steady (no shimmer, no ghost trail when you
     turn), it fades out by about 24 m and above and below you, and it is never drawn over the sky beyond a hill.
     Settings page: Play bounds off removes it. The playground shows terrain to the horizon (88 m) with no edge of
     the world."
   - ROADMAP: `[wp-5]` moves to "Now" as built (state **Built**, not "reviewed"); "Next" item 3 removed and the list
     renumbered; "Not a game yet" drops "no visible play boundary ..." and "a playground session sees 56 m"; the
     Sessions row's gaps drop "an invisible wall ..." and "56 m on the playground"; "Performance and measured values"
     gets the composite cost (A and B medians, UHD 620, date) and "playground session view radius 88 m"; the test
     count line.
4. Write the stage report (the guide's report format, for the stage review): all checks with results, the measured
   numbers, the owner check to run.

**Checks**
1. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` and `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh ci` - both clean
2. `tools/ctest.sh` - all pass; `tools/doc-check.sh --build build` - PASS
3. `PORT=27065 tools/session-check.sh` - PASS, `view radius 88.0 m` in both client logs; corrections within G16's
   bound
4. G13 and G14 lines unchanged against the last run before this stage (`VERIFY_SLOT=1 tools/verify.sh full --ci`
   runs them; compare `hashes equal`, `Entities: ... agree`, the bots' `store ... at most N used` and the terrain
   mode lines with the previous run's logs if you have them, else just their Must)
5. `VERIFY_SLOT=1 tools/verify.sh full --ci` - PASS
6. `VERIFY_SLOT=1 tools/verify.sh soak` in the background - PASS (about 35 min); the playground scenario now runs
   moving windows: read its bots' `Memory: bot N: ... store a / 12288 slots (at most N)` lines: N + 2048 <= 12288
7. Look at every screenshot verify lists, and `build/session-check/a.png` / `b.png`

**Done when**
- [ ] all checks PASS; no `DISABLED_` test of this stage
- [ ] ROADMAP, networking.md, owner check 15 updated; measured values recorded
- [ ] the stage report written; owner check 15 named as the owner's windowed check

**Traps**
- Do not write "reviewed" anywhere: the owner's review makes that.
- Do not edit `docs/history/reviews/project-review-2026-11-17.md` (history is append-only; doc-check fails it).

## Values this stage lands

- `net::kProtocolVersion`, `sim::kPlayerRuleRevision`, `buildHash` inputs, wire values: unchanged.
- `graphics::FrameUniforms`: + `playBounds` (vec4, last member); `LayoutContract.FrameUniformsMatchTheGlslBlock`
  counts one member more.
- Client windows: `sim::ClientWindowMoves` is true for every `windowChunks > 0` that differs from the region's
  window; the game on the playground: 48 x 44 x 48 chunks, view radius 88 m.
- Gates: none added. Owner check 15 extended.
