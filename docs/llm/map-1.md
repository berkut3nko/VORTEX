# [map-1] Map and minimap: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/map|map]]

| | |
|---|---|
| Stage | `[map-1]`: a round minimap (top left, north up, the player's arrow, teammates, landmarks) and a parchment full map on M (contours, landmarks with names, hot zones, detail only where the player has been), both drawn from one CPU-baked map image by the HUD pipeline |
| Design | [map.md](../design/map.md) (all of it; "Decision" 1-7, "The map image", "Explored mask", "Landmarks", "Hot zones", "Players on the map", "Looks", "The HUD contract change", "Costs and their measurement") |
| Code read at | engine `9474e9c`, game `9729e09` (`[wp-7]` committed). `[wp-8]` .. `[wg-W1]` land before this stage: find code with `grep -n`, never by line number |
| Depends on | `[wg-W1]` (`worldgen::RegionPlan`, `MarkerKind::Landmark`, `--region village`), `[wp-7]` (`sim::FightRules`, `net::Client::TeamOf`, `src/FightHud.cpp` `TeamColour`), `[wp-4]` (settings page) |
| Cards | 14, each 0.5 d (e): 7 d (e) |
| Changes | wire: **no** (`net::kProtocolVersion` unchanged; [map-2]'s hot-zone message takes the next free protocol, expected 14 after `[mag-2]`'s 13). Movement rule: **no** (`sim::kPlayerRuleRevision` unchanged). `buildHash`: **no** (the map is local display). HUD contract: **yes** (`HudQuad::reserved` becomes `kind`; new buffer `u_Map`, binding 11). Gate: **G25** (proposed: the next free after llm/README's planned G23 `[mag-1]` and G24 `[mag-2]`; the integrator reconciles the number) |

What the player gets: a minimap that shows where they are, which way they look, where their teammates are and
which way the village is; a map on M that fills in as they explore; hot zones that pulse where an event will start.

## Before you start

- `git log --oneline | grep -E '\[(wg-W1|wp-7|wp-4)\]'` shows all three in the game root and in `engine/`.
  Missing: stop, `Status: blocked`.
- The code this stage builds on (check each with `grep -n`):
  - `engine/src/voxel/World.cppm`: `World::ChunkRevision`, `StateOf` (`CellState` Unloaded, Loading, Empty, Uniform,
    Dense, Packed), `GetChunk(chunk, uint8_t* uniformId)`, `ReadChunk(chunk, Chunk&)`, `GridWords()`,
    `WindowMinChunk()`, `WindowSize()`, `VoxelSize()`, `TakeChunkChanges()` (physics owns it: **do not call it**);
    private `Cell::revision`, `BumpRevision(Cell&)`, `ColumnIndex(cx, cz)`, `FindColumn`, `Column::top`.
  - `engine/src/voxel/Chunk.cppm`: `Chunk::Get(x, y, z)`, `kChunkSize` 32.
  - `engine/src/voxel/Material.cppm`: `Material::color` (linear), `emissive`, `MaterialFlag_Liquid`,
    `MaterialFlag_Foliage`, `MaterialLibrary::Get(kWorldPalette, id)`, `Revision()`, `WorldMaterials`.
  - `engine/src/core/Hud.cppm`: `HudQuad {x, y, w, h, color, bitsLo, bitsHi, reserved}`, `HudCanvas::Push`, `Rect`,
    `Text`, `TextScale`, `DimBackground`, `BitmapFont::kSolid`; `engine/src/core/internal/Hud.cpp`.
  - `engine/shaders/include/common.glsl`: `VxHudQuad`, `VX_HUD_CELLS_X/Y`, the `#ifdef VX_NEED_HUD` binding 10 block;
    `engine/shaders/hud.vert`, `hud.frag`.
  - `engine/src/graphics/internal/Renderer.cpp`: `PipelineId::Hud`, `FindBuffer` (`"u_Hud"`), `PassPresent`, the voxel
    upload (`GridPageRevisions`, `kMinGridBytes`, `frame.staging`, `gridCopies`); `engine/src/graphics/Frame.cppm`
    `FrameInput` (`hud`, `hudDim`, `playBounds`).
  - `engine/tests/graphics/ContractTests.cpp`: `kBuffers` (bindings 0-10), `Pipelines()` (`{"hud", "hud.vert",
    "hud.frag", {"u_Frame", "u_Hud"}, {}}`), `LayoutContract.HudQuadMatchesVxHudQuad`,
    `LayoutContract.HudCellGridMatchesTheFont` (its `constant` parser).
  - `engine/src/engine/Engine.cppm` / `internal/Engine.cpp`: `Engine::Hud()`, `World()`, `Materials()`,
    `WorldMaterialIds()`, `SetPlayBounds`, the frame (`graphics::FrameInput in;` ... `in.hud = s.hud.Quads();`), the
    exit lines (`GPU {:<10} ...`, `Perf:`).
  - `engine/src/core/Settings.cppm`: `GameSettings` (`playBounds`), `SettingsPage::Item` (`kPlayBounds = 6`),
    `DefaultSettingsPath()`.
  - WorldGen (`[wg-W1]`): `engine/src/worldgen/Plan.cppm` (`RegionPlan`, `MarkerRec`, `MarkerKind::Landmark`,
    `BuildingRec::settlement`, `box[6]`, `PlanHeader::origin`).
  - Sim: `engine/src/sim/Combat.cppm` (`FightRules::teamSpawns`, `towers`, `kTeamCount`), `Rules.cppm` (`TeamId`,
    `kNoTeam`), `Region.cppm` (`RegionTemplate::anchor`, `PlayBoundsOf(RegionDesc)`), `Player.cppm`
    (`LookDirection(uint16_t yaw, int16_t pitch)`).
  - Game: `src/main.cpp` `ShredborneGame` (`OnUpdate`, `DrawHud`, `m_Feet`, `m_HudEnabled`, `--no-hud`, the plaza
    `sim::PlaygroundTemplate(...)`, `--region`), `src/Session.cpp` `SessionGame` (`DrawHud`, `m_Client.RemotePlayers()`,
    `m_Client.TeamOf`, `m_Client.Slot()`), `src/Practice.cpp` (`RunPractice`, the in-process `SessionConfig`),
    `src/FightHud.h` (`TeamColour`).
  - `tools/verify.sh` (`QUICK_GATES`, `gate_tiers`, `one_script`, `game_run`), `tools/lib/verify-gates.sh`
    (`judge_G7`, `_g3_base`, `need_line`, `need_num`), `tools/selftest-verify.sh` (`case_ok`, `case_bad`).

## Decisions

Settled by us (2026-11-24; the owner may veto), from map.md and its "Open questions".

1. **One map image, CPU-baked, 1 texel per metre**, texel = `heightDm` (bits 0-15), top material id (16-23),
   `Known` (24), `Explored` (25). Extent per run: offline default 1024 m square centred on the spawn; `--region
   village` the plan area plus 16 m; sessions and practice the play bounds plus 16 m. Origin and size snapped
   outward to multiples of 4 m. Cap 1024 x 1024 texels: larger is refused (`Map: extent too large`, map off).
2. **Source = the World only.** No generator reads. A column is re-baked when `World::ColumnRevision(cx, cz)` differs
   from the revision it was baked at. 24 columns per frame, nearest first (ties `(cz, cx)`).
3. **North up** (screen up = world -z, screen right = world +x). The player's arrow turns, the map does not.
4. **Explored mask** of 4 x 4 m cells, reveal disc 16 cells (64 m); sessions and practice reveal the whole play area
   at join; offline saved to `DefaultSettingsPath().parent_path() / "explored" / "<key>.bin"` (not headless).
5. **`u_Map`** = storage buffer, binding 11, block `MapBuffer { uint words[]; }`: 64 header words, 256 minimap
   colours, 256 parchment inks, then texels (word 576 on). Uploaded by 4 KiB page, at most 64 pages per frame.
   No map: a 577-word zero buffer is bound (width 0).
6. **HUD quad kinds**: `kind & 0xFF` 0 cells (today), 1 minimap look, 2 parchment look; bit 8 circle mask. For map
   kinds `color` = texels-per-pixel x 256 (bits 0-15) | opacity (bits 24-31), `bitsLo` / `bitsHi` = centre texel x / z
   in 1/16 (int32 bit pattern).
7. **Markers are cell quads** (`HudCanvas::Cells`): 5 x 9 bitmaps; arrows use the centre 5 x 5 (rows 2-6).
8. **Hot zones** are drawn by the shader from the header (at most 8); in this stage fed only by `--hot-zone
   X,Z,R,S` (metres, metres, metres, seconds; repeatable up to 8).
9. **Party** = `sim::ShowsOnMap(own, other)`: `own != kNoTeam && own == other`. Enemies never shown.
10. **Landmarks**: plan (one `Village` per settlement, one `Tower` per `Landmark` marker), plaza (`Site` at the
    anchor, `PLAZA`), sessions (`Spawn` `BASE` at `Welcome::spawn`), practice (both bases, felling towers).
11. **Minimap**: top left, 8 px margin, diameter `72 x TextScale` px, 96 m across (`texelsPerPixel = 96 / diameter`).
    **Full map**: M toggles, Escape closes it first, fitted to 80 % of the screen height, wheel zoom 1x / 2x / 4x
    (2x / 4x centred on the player, clamped), `DimBackground(0.5)`, no casts while open. `--map` opens it from
    frame 1; `--no-minimap` hides the minimap; settings row `Minimap on / off` (id 7, `kMinimap`, before Back),
    file key `minimap`.
12. **Gate G25** (tier quick): `G --frames 400 --map --hot-zone 24,24,12,30 --screenshot <V>/g25.png` on the
    offline plaza; judged on the `Map:` lines (card 13).

## Skeleton

Card 1 lands it: declarations, stubs returning neutral values (`// [map-1] card N fills this`), and every test of
this stage written in full as `DISABLED_`.

`engine/src/voxel/Map.cppm` (new partition; `Voxel.cppm` gets `export import :map;` after `export import :world;`):

```cpp
module;
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
export module vortex.voxel:map;
import :material;
import :chunk;
import :world;

// docs/design/map.md: the map image, its bake, the explored mask, landmarks and hot zones. Local display only. [map-1]
export namespace vortex::voxel {
    inline constexpr uint32_t kMapHeaderWords = 64;
    inline constexpr uint32_t kMapPaletteWords = 256;
    inline constexpr uint32_t kMapTexelWord = kMapHeaderWords + 2 * kMapPaletteWords;   ///< 576
    inline constexpr uint32_t kMapPageWords = 1024;                                    ///< 4 KiB pages
    inline constexpr int kMapMaxSide = 1024;                                           ///< texels
    inline constexpr int kMapCellMetres = 4;                                           ///< explored cell
    inline constexpr int kMapRevealCells = 16;                                         ///< 64 m
    inline constexpr uint32_t kMapMaxHotZones = 8;
    inline constexpr int kBakeColumnsPerFrame = 24;
    inline constexpr uint32_t kMapKnown = 1u << 24;
    inline constexpr uint32_t kMapExplored = 1u << 25;
    inline constexpr int kMapContourDm = 40;

    /// @brief World x in [origin.x, origin.x + size.x) metres, z likewise; multiples of 4 m.
    struct MapExtent {
        glm::ivec2 originMetres{0};
        glm::ivec2 sizeMetres{0};
        float heightBaseMetres = -48.0f;   ///< height 0 of heightDm
        bool Valid() const;                ///< sizes > 0, <= kMapMaxSide, multiples of 4; origin multiple of 4
        friend bool operator==(const MapExtent&, const MapExtent&) = default;
    };
    /// @brief A square of `sideMetres` centred on `centre`, origin floored to 4 m, side rounded up to 4 m.
    MapExtent MapExtentAround(glm::dvec2 centre, int sideMetres, float heightBaseMetres);
    /// @brief [min, max) grown by `marginMetres`, min floored and max ceiled to 4 m.
    MapExtent MapExtentOfBounds(glm::vec2 min, glm::vec2 max, int marginMetres, float heightBaseMetres);

    enum class MapClass : uint8_t { Built = 0, Water = 1, Lava = 2, Rock = 3, Soil = 4, Green = 5, Snow = 6 };
    MapClass MapClassOf(const MaterialLibrary& library, const WorldMaterials& ids, uint8_t id);
    uint32_t MinimapColour(const Material& material);   ///< display RGBA8 (Rgba::value layout), alpha 255
    uint32_t ParchmentInk(MapClass cls);                ///< display RGBA8, alpha = mix weight (89 = 35 %, 153 = 60 %)

    enum class MapLandmarkKind : uint8_t { Castle = 1, Village = 2, Dungeon = 3, Tower = 4, Site = 5, Spawn = 6 };
    struct MapLandmark {
        MapLandmarkKind kind = MapLandmarkKind::Site;
        glm::ivec2 metres{0};   ///< world x, z
        std::string name;       ///< ASCII, at most 16 characters, upper case
        friend bool operator==(const MapLandmark&, const MapLandmark&) = default;
    };
    struct MapHotZone {
        glm::ivec2 metres{0};
        uint16_t radiusMetres = 0;
        uint16_t startsInSeconds = 0;
        uint8_t kind = 0;
    };

    struct MapTexel { uint16_t heightDm = 0; uint8_t id = 0; bool known = false, explored = false; };
    uint32_t PackMapTexel(const MapTexel& t);
    MapTexel UnpackMapTexel(uint32_t word);

    /// @brief The words u_Map holds (map.md "The HUD contract change"), with a revision per 4 KiB page.
    class MapImage {
    public:
        bool Reset(const MapExtent& extent, std::string* why = nullptr);   ///< all texels 0; false: invalid extent
        const MapExtent& Extent() const { return m_Extent; }
        int Width() const { return m_Extent.sizeMetres.x; }
        int Height() const { return m_Extent.sizeMetres.y; }
        std::span<const uint32_t> Words() const { return m_Words; }
        std::span<const uint64_t> PageRevisions() const { return m_Pages; }
        uint64_t Revision() const { return m_Revision; }
        uint32_t Texel(int tx, int tz) const;                     ///< 0 outside
        void SetTexel(int tx, int tz, uint32_t word);             ///< ignored outside; bumps the page only on change
        bool TexelOf(glm::dvec2 metres, glm::ivec2& texel) const; ///< floor; false outside
        void SetPalettes(std::span<const uint32_t, 256> minimap, std::span<const uint32_t, 256> inks);
        void SetHotZones(std::span<const MapHotZone> zones);      ///< the first 8; header words 6, 8-39
    private:
        void Touch(uint32_t word);
        MapExtent m_Extent;
        std::vector<uint32_t> m_Words;
        std::vector<uint64_t> m_Pages;
        uint64_t m_Revision = 0;
    };

    /// @brief One bit per 4 x 4 m cell of the extent (map.md "Explored mask").
    class ExploredMask {
    public:
        void Reset(const MapExtent& extent);
        int CellsX() const { return m_CellsX; }
        int CellsZ() const { return m_CellsZ; }
        bool Test(int cx, int cz) const;                ///< false outside
        bool Set(int cx, int cz);                       ///< true when it was clear
        /// @brief Sets the disc of kMapRevealCells around the feet's cell; appends newly set cells (x, z order of the scan).
        uint32_t Reveal(glm::dvec2 feetMetres, std::vector<glm::ivec2>* newlySet = nullptr);
        uint32_t RevealAll(std::vector<glm::ivec2>* newlySet = nullptr);
        uint32_t Count() const;
        std::vector<uint8_t> Encode() const;            ///< 'VXEX', v1, cells, origin, runs
        bool Decode(std::span<const uint8_t> bytes);    ///< false: unchanged (empty) on any malformed byte
    private:
        MapExtent m_Extent;
        int m_CellsX = 0, m_CellsZ = 0;
        std::vector<uint8_t> m_Bits;                    ///< bit (cx + cz * cellsX), LSB first
        glm::ivec2 m_LastCell{INT32_MIN};
    };

    struct MapBakeStats {
        uint64_t columnsBaked = 0;
        uint64_t incomplete = 0;          ///< bakes stopped at an Unloaded / Loading chunk
        uint32_t maxColumnsInFrame = 0;
        double maxFrameMs = 0.0;          ///< Update's own time, the largest of the run
        uint32_t KnownTexels(const MapImage& image) const;
    };

    class MapBaker {
    public:
        bool Start(const MapExtent& extent, const MaterialLibrary& library, const WorldMaterials& ids, std::string* why = nullptr);
        bool Running() const { return m_Running; }
        /// @brief The dirty scan and at most kBakeColumnsPerFrame bakes, nearest the focus first.
        void Update(const World& world, glm::dvec2 focusMetres);
        /// @brief Bakes chunk column (cx, cz). False: outside the extent, not in the window, or incomplete.
        bool BakeColumn(const World& world, int cx, int cz);
        uint32_t Reveal(glm::dvec2 feetMetres);   ///< ExploredMask::Reveal + the texels' Explored bits
        uint32_t RevealAll();
        void SetLandmarks(std::vector<MapLandmark> landmarks) { m_Landmarks = std::move(landmarks); }
        std::span<const MapLandmark> Landmarks() const { return m_Landmarks; }
        MapImage& Image() { return m_Image; }
        const MapImage& Image() const { return m_Image; }
        ExploredMask& Explored() { return m_Explored; }
        const MapBakeStats& Stats() const { return m_Stats; }
    private:
        bool m_Running = false;
        MapImage m_Image;
        ExploredMask m_Explored;
        std::vector<uint64_t> m_Baked;             ///< per chunk column of the extent: the ColumnRevision baked
        std::vector<MapLandmark> m_Landmarks;
        MapBakeStats m_Stats;
        Chunk m_Scratch;                           ///< ReadChunk target for Packed chunks
        uint64_t m_PaletteRevision = 0;
    };
}
```

`engine/src/voxel/World.cppm`, public, beside `ChunkRevision` (card 2):

```cpp
/// @brief The largest ChunkRevision of the window column (cx, cz)'s cells since it was last re-targeted;
/// 0 when the column is not in the window. Changes with every edit, load, unload and chunk data in it. [map-1]
uint64_t ColumnRevision(int cx, int cz) const;
/// @brief The column-top word of (cx, cz): window rows >= it are air; -1 when the column is not in the window.
int ColumnTopRow(int cx, int cz) const;
```

`engine/src/core/Hud.cppm` (card 7):

```cpp
inline constexpr uint32_t kHudKindCells = 0, kHudKindMapMinimap = 1, kHudKindMapParchment = 2;
inline constexpr uint32_t kHudFlagCircle = 0x100;
// HudQuad: `uint32_t reserved = 0;` becomes `uint32_t kind = 0;  ///< kHudKind* | kHudFlag*`
enum class MapLook : uint8_t { Minimap = 1, Parchment = 2 };
struct MapView {
    glm::vec2 centreTexel{0.0f};    ///< map texel coordinates (texel tx covers [tx, tx + 1))
    float texelsPerPixel = 1.0f;    ///< 1/256 .. 255
    MapLook look = MapLook::Minimap;
    bool circle = false;
    uint8_t opacity = 255;
};
/// @brief Texel <-> output pixel for a map drawn in `rect` with `view` (the inverse of hud.frag's mapping).
struct MapProjection {
    HudRect rect;
    MapView view;
    glm::vec2 ToPixel(glm::vec2 texel) const;   ///< rect centre + (texel - centre) / texelsPerPixel
    glm::vec2 ToTexel(glm::vec2 pixel) const;   ///< centre + (pixel - rect centre) * texelsPerPixel
    bool Inside(glm::vec2 pixel, float inset = 0.0f) const;      ///< circle or rect, shrunk by inset
    glm::vec2 ClampToRim(glm::vec2 pixel, float inset) const;    ///< towards the centre onto the rim less inset
};
namespace MapIcons {               // 5 x 9 cell bitmaps (bit = row * 5 + column), card 7 gives the rows
    uint64_t Arrow(int dir8);      ///< 0 = up (-z), clockwise in 45 degree steps
    uint64_t Landmark(uint8_t kind);   ///< voxel::MapLandmarkKind's value; unknown -> Site
    uint64_t Dot();
    uint64_t Cross();
}
// HudCanvas:
void Map(const HudRect& rect, const MapView& view);           ///< one quad of kind 1 / 2
void Cells(const HudRect& rect, uint64_t bits, Rgba color);   ///< one kind-0 quad with these bits
```

`engine/src/worldgen/Plan.cppm` (card 10): `std::vector<voxel::MapLandmark> PlanLandmarks(const RegionPlan& plan,
float voxelSize);` (`vortex.worldgen` imports `vortex.voxel`: check its `import` lines; if it does not, stop).

`engine/src/sim/Combat.cppm` (card 10): `bool ShowsOnMap(TeamId own, TeamId other);`.

`engine/src/graphics/Frame.cppm` (card 8): `FrameInput` gains `const voxel::MapImage* map = nullptr;  ///< u_Map; null: an empty map`.

`engine/src/engine/Engine.cppm` (card 5): `bool SetMapExtent(const voxel::MapExtent& extent);` (starts the baker;
false and `Map: extent too large ...` logged when refused), `voxel::MapBaker* Map();` (null until started).

Shaders (cards 8-9): `common.glsl` gains the `VX_HUD_KIND_*`, `VX_HUD_FLAG_CIRCLE`, `VX_MAP_*` constants and the
`#ifdef VX_NEED_MAP` block with binding 11; `hud.vert` passes `data` whole and the rect size; `hud.frag` defines
`VX_NEED_MAP` and the map path.

Tests (card 1 writes them all `DISABLED_`; the card named after each makes it pass):
- `engine/tests/core/MapTests.cpp` (VortexCoreTests): `MapImage.HeaderAndTexelLayout` (3), `MapImage.PageRevisionsFollowWrites` (3),
  `MapPalette.ClassesOfTheWorldMaterials` (3), `MapBake.FlatGroundGivesOneHeight` (4), `MapBake.TopMaterialAndHeightOfAStep` (4),
  `MapBake.UnloadedColumnStaysUnknown` (4), `MapBake.AnEditRebakesOnlyItsColumn` (5), `MapBake.NearestColumnsFirst` (5),
  `ExploredMask.RevealIsADiscOfCells` (6), `ExploredMask.EncodeDecodeRoundTrip` (6), `ExploredMask.DecodeRefusesMalformed` (6).
- `engine/tests/core/WorldTests.cpp`: `World.ColumnRevisionFollowsEveryCellRevision` (2).
- `engine/tests/core/HudTests.cpp`: `HudCanvas.MapQuadPacksItsView` (7), `HudCanvas.CellsQuadKeepsItsBits` (7),
  `MapProjection.WorldToPixelAndBack` (7), `MapProjection.MarkersOutsideTheMinimapClampToItsRim` (7).
- `engine/tests/worldgen/PlanTests.cpp`: `WorldGen.PlanLandmarksNameTheVillageAndItsTower` (10).
- `engine/tests/sim/CombatTests.cpp`: `Combat.MapShowsTeammatesOnly` (10).
- `engine/tests/graphics/ContractTests.cpp`: `LayoutContract.HudQuadKindsMatchTheGlslConstants` (9),
  `LayoutContract.MapHeaderMatchesTheGlslConstants` (9); `kBuffers` and `Pipelines()` rows changed (8).

No new test file needs a CMake line: `vortex_add_tests` globs each test directory (check `engine/tests/CMakeLists.txt`;
if it lists files, add `MapTests.cpp` there).

## Card 1: skeleton

| | |
|---|---|
| Goal | The stage's interfaces compile as stubs and its tests exist, disabled |
| Depends on | `[wg-W1]`, `[wp-7]` committed |
| Effort | 0.5 d (e) |
| Touches | voxel, core hud, sim, worldgen, engine, graphics (declarations only). Wire: no. Movement rule: no. buildHash: no |

**Read first** (about 40 KiB): map.md (all); this pack's "Skeleton"; `engine/src/voxel/Voxel.cppm`;
`engine/src/core/Hud.cppm` `HudQuad`; `engine/tests/core/HudTests.cpp` (its includes and `import` lines).

**Steps**
1. Create `engine/src/voxel/Map.cppm` as in the skeleton and `engine/src/voxel/internal/Map.cpp` (`module;` ...
   `module vortex.voxel;`) with stub bodies: `Reset` returns false, `Texel` 0, `Reveal` 0, `Update` does nothing,
   `BakeColumn` false, `Encode` empty, `Decode` false. Add `export import :map;` to `Voxel.cppm` after `:world`.
2. Add `ColumnRevision` / `ColumnTopRow` to `World` returning 0 / -1.
3. In `Hud.cppm` rename `reserved` to `kind` (grep `reserved` in `engine/src` and `src`: no user today), add the
   constants, `MapLook`, `MapView`, `MapProjection`, `MapIcons`, `HudCanvas::Map`, `HudCanvas::Cells` (stubs).
4. `sim::ShowsOnMap` returns false; `worldgen::PlanLandmarks` returns `{}`; `Engine::SetMapExtent` returns false,
   `Engine::Map()` nullptr; `FrameInput::map` (unused yet).
5. Write every test of the skeleton list in full (their bodies from the cards below), each named `DISABLED_<Name>`.

**Checks**: build (exit 0, no warning); `tools/ctest.sh` all pass (the disabled ones are not run);
`tools/doc-check.sh --build build` PASS.

**Done when**: [ ] everything compiles; [ ] the 20 tests of the skeleton list exist, each as `DISABLED_<Name>`
(11 in `MapTests.cpp`, 1 World, 4 HUD, 1 WorldGen, 1 Combat, 2 LayoutContract).

## Card 2: the column revision

| | |
|---|---|
| Goal | The map can see that a chunk column changed without taking physics' change list |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | voxel (`World`). Wire: no |

**Read first**: `World.cppm` `Cell`, `Column`, `BumpRevision`; `internal/World.cpp` every `BumpRevision(` call (5),
`ColumnIndex`, `FindColumn`, `UpdateColumnTop`.

**Steps**
1. Add `std::vector<uint64_t> m_ColumnRevisions;` sized to the column count where `m_Columns` is sized.
2. `BumpRevision(Cell& cell)`: after `cell.revision = ++m_ContentRevision;` write
   `m_ColumnRevisions[ColumnIndex(cell.coord.x, cell.coord.z)] = cell.revision;`. The revision only grows, so the
   column's word is the largest of its cells'.
3. `ColumnRevision(cx, cz)`: `FindColumn(cx, cz)` null -> 0; else `m_ColumnRevisions[ColumnIndex(cx, cz)]`.
4. `ColumnTopRow(cx, cz)`: `FindColumn` null -> -1; else `column->top`.
5. Where a column is re-targeted (`RetargetColumn`), leave the word: its cells are re-targeted too and bump it.

**Tests**
- `World.ColumnRevisionFollowsEveryCellRevision` (`WorldTests.cpp`, use the file's existing world helper): load a
  small window blocking; for a column `(cx, cz)` in it: `ColumnRevision` != 0 and equals the max of
  `ChunkRevision` over its rows; `FillBox` one voxel in row 2 -> it grows and equals that chunk's new revision; a
  column outside the window -> 0; `ColumnTopRow` outside -> -1, inside within `[0, WindowSize().y]`.

**Checks**: build; `tools/ctest.sh -R 'World\.' --output-on-failure`; `tools/ctest.sh`; doc-check.

**Must not break**: `World.GridEncodingMatchesShaderContract`, `Structure.ChunkRevisionChangesWithEveryContentChange`
(the revision rule is unchanged; this only mirrors it). Packing keeps the revision (no new bump).

**Traps**: do not bump anything new; do not take `TakeChunkChanges`.

## Card 3: the map image and its colour tables

| | |
|---|---|
| Goal | `MapImage` holds the u_Map words exactly as the shader will read them |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | voxel (`:map`) |

**Steps**
1. `MapExtent::Valid`, `MapExtentAround` (`ox = floor((centre.x - side / 2) / 4) * 4`, side ceiled to 4),
   `MapExtentOfBounds` (min floored, max ceiled to 4 m after the margin).
2. `Reset`: invalid -> false, `why` = `"extent WxH m: at most 1024 x 1024, multiples of 4"`. Else
   `m_Words.assign(kMapTexelWord + W * H, 0)`; header: `[0] = W`, `[1] = H`, `[2] = bit_cast<uint32_t>(origin.x)`,
   `[3] = origin.z` likewise, `[4] = (uint32_t)(int32_t)llround(heightBase * 10)`, `[5] = kMapContourDm`, `[6] = 0`;
   pages `ceil(size / kMapPageWords)`, all set to `++m_Revision`.
3. `PackMapTexel`: `height | id << 16 | (known ? kMapKnown : 0) | (explored ? kMapExplored : 0)`.
4. `SetTexel`: outside -> return; `index = kMapTexelWord + tx + tz * W`; equal -> return; write, `Touch(index)`
   (`m_Pages[index / kMapPageWords] = ++m_Revision`).
5. `TexelOf`: `tx = floor(x - origin.x)`, likewise z; inside `[0, W) x [0, H)`.
6. `SetPalettes`: words 64-319 and 320-575, touching their pages. `SetHotZones`: count `min(n, 8)` in word 6; zone
   `i` at `8 + 4i`: `x16 = (origin-relative metres x) * 16`, `z16`, `radius * 16`, `startsIn | kind << 16`; zones past
   the count zeroed; touch the page.
7. `MinimapColour(m)`: per channel `s = LinearToSrgb(min(1, c * 1.15))` (`c <= 0.0031308 ? 12.92 c : 1.055 c^(1/2.4) -
   0.055`), byte `= lround(s * 255)`; `m.emissive > 0`: `c` = the albedo unscaled at full brightness (`max(c / maxc, ...)`:
   divide by the largest channel). Alpha 255. Red in the low byte.
8. `MapClassOf(lib, ids, id)`: id 0 -> `Built` (never drawn); `ids.water` or liquid flag (not lava) -> Water;
   `ids.lava` -> Lava; stone, stoneDark, bedrock, crystal -> Rock; dirt, sand -> Soil; grass, grassDry, leaves,
   leavesAutumn or the foliage flag -> Green; snow -> Snow; else Built.
9. `ParchmentInk`: Water `(120,160,185,89)`, Lava `(200,90,40,89)`, Rock `(150,140,125,89)`, Soil `(190,165,120,89)`,
   Green `(150,170,110,89)`, Snow `(245,245,240,89)`, Built `(120,80,55,153)`.

**Tests**
- `MapImage.HeaderAndTexelLayout`: `Reset({{-512, -512}, {1024, 1024}, -48})` true; `Words().size()` =
  576 + 1048576; words 0-6 = 1024, 1024, (uint32)-512, (uint32)-512, (uint32)-480, 40, 0; `SetTexel(3, 2, w)` lands
  at word `576 + 3 + 2 * 1024`; `Reset` of 1028 or of 1022 -> false; `TexelOf({-512.0, -511.5})` = (0, 0), `(511.9,
  0)` = (1023, 512), `(512, 0)` false.
- `MapImage.PageRevisionsFollowWrites`: after Reset, remember revisions; `SetTexel` -> only page
  `(576 + idx) / 1024` grows; the same word again -> nothing grows; `SetHotZones` -> page 0 grows; 9 zones -> word 6 = 8.
- `MapPalette.ClassesOfTheWorldMaterials`: `RegisterDefaultWorldMaterials`; water Water, lava Lava, stone Rock, sand
  Soil, leaves Green, snow Snow, planks Built; `MinimapColour` of a material with colour (1, 0, 0) -> byte 0 = 255,
  bytes 1-2 = 0, byte 3 = 255.

**Checks**: build; `tools/ctest.sh -R 'MapImage\.|MapPalette\.'`; `tools/ctest.sh`; doc-check.

## Card 4: baking a chunk column

| | |
|---|---|
| Goal | One chunk column's texels hold the top height and material of their metre columns |
| Depends on | cards 2, 3 |
| Effort | 0.5 d (e) |
| Touches | voxel (`MapBaker::BakeColumn`, `Start`) |

**Read first**: map.md "Baking a chunk column"; `World::StateOf`, `GetChunk`, `ReadChunk`; `Chunk::Get`.

**Steps**
1. `Start`: `m_Image.Reset(extent)` false -> false; build both palettes from `library.Get(kWorldPalette, id)` for
   id 0..255 (id 0 colour 0) and `SetPalettes`; `m_Explored.Reset(extent)`; `m_Running = true`. `m_Baked` needs the
   voxel size, which `Start` does not get: the first `Update` sizes it (`ceil(size / (32 * voxelSize)) + 1` chunk
   columns per axis, indexed from `floor(origin / (32 * voxelSize))`, all 0).
2. `BakeColumn(world, cx, cz)`:
   1. `vs = world.VoxelSize()`; the texel range: `tx` from `max(0, floor(32 cx * vs - origin.x - 0.5) )` while
      `tx < W`; keep `tx` only if `vx = (int)floor((origin.x + tx + 0.5) / vs)` is in `[32 cx, 32 cx + 32)`; stop at
      the first `vx >= 32 cx + 32`. Same for z. No texel -> return false.
   2. `top = world.ColumnTopRow(cx, cz)`; -1 -> false. `minY = world.WindowMinChunk().y`.
   3. Samples: arrays (at most 6 x 6 = 36) of `(lx = vx - 32 cx, lz, open = true, y = -1, id = 0)`.
   4. For `r = top - 1` down to 0: `c = {cx, minY + r, cz}`; switch `world.StateOf(c)`:
      `Unloaded`, `Loading` -> `++m_Stats.incomplete`, return false (nothing written);
      `Empty` -> continue;
      `Uniform` -> `uint8_t u; world.GetChunk(c, &u)`; every open sample: `y = (minY + r) * 32 + 31`, `id = u`, closed;
      `Dense` -> `const Chunk* k = world.GetChunk(c)`; `Packed` -> `world.ReadChunk(c, m_Scratch)`, `k = &m_Scratch`;
      for each open sample, `for ly = 31..0`: `id = k->Get(lx, ly, lz)`; first non-zero closes it with
      `y = (minY + r) * 32 + ly`.
      All closed -> break.
   5. Still open: height 0, id 0.
   6. Each sample's texel: `dm = clamp(llround(((y + 1) * (double)vs - base) * 10), 0, 65535)` (open: 0);
      `explored = m_Explored.Test(floor(tx / 4), floor(tz / 4))`; `SetTexel(tx, tz, PackMapTexel({dm, id, true,
      explored}))`.
   7. `++m_Stats.columnsBaked`; return true. (The caller records the revision.)

**Tests** (`MapTests.cpp`; a small world as `WorldTests.cpp` makes one: flat generator or `FillBox` on an empty one;
reuse its helper by copying it under another name, never a name of an engine type)
- `MapBake.FlatGroundGivesOneHeight`: world with ground top voxel row 79 (y < 80 solid, 0.125 m, so the surface is
  at 10 m), base -48 -> every baked texel `heightDm` = `llround((80 * 0.125 + 48) * 10)` = 580, id = the ground id,
  known.
- `MapBake.TopMaterialAndHeightOfAStep`: `FillBox` a 2 x 2 m planks block 1 m high on the flat ground in the
  column -> the 2 x 2 texels under it read planks and 590, the rest 580.
- `MapBake.UnloadedColumnStaysUnknown`: a column whose surface chunk is not loaded (a window not loaded) ->
  `BakeColumn` false, its texels 0, `Stats().incomplete` 1.

**Checks**: build; `tools/ctest.sh -R 'MapBake\.'`; `tools/ctest.sh`; doc-check.

**Traps**: `GetChunk` returns nullptr for Packed: use `ReadChunk`. `GetVoxel` per voxel works but is 30x slower:
use the chunk. Never write a texel of an incomplete column (a half-loaded column would show holes).

## Card 5: scheduling and the engine

| | |
|---|---|
| Goal | The engine keeps the map current every frame within its budget and logs what it cost |
| Depends on | card 4 |
| Effort | 0.5 d (e) |
| Touches | voxel (`MapBaker::Update`), engine (`SetMapExtent`, `Map()`, the frame, exit lines) |

**Steps**
1. `MapBaker::Update(world, focus)`: not running -> return. Start a `core` timer (the profiler's clock or
   `std::chrono::steady_clock`). For every window column (`m = WindowMinChunk()`, `cx` in `[m.x, m.x + size.x)`,
   `cz` likewise) whose chunk-column index in `m_Baked` exists: `rev = ColumnRevision`; `rev != 0 && rev !=
   m_Baked[i]` -> push `(d2, cz, cx)` with `d2` = squared chunk distance to `floor(focus / (32 vs))`.
2. `std::partial_sort` the first `kBakeColumnsPerFrame` by `(d2, cz, cx)`; for each: `BakeColumn` true ->
   `m_Baked[i] = rev` (the value read in step 1; a column edited during the bake is seen again next frame).
3. Stats: `maxColumnsInFrame`, `maxFrameMs` (the whole Update).
4. Engine: a `std::unique_ptr<voxel::MapBaker>` in the engine state. `SetMapExtent(e)`: `Start(e, Materials(),
   WorldMaterialIds(), &why)`; false -> `Log::Warn("Map: extent too large ({}), the map is off", why)`, return false.
   Log `Map: extent X0,Z0 size WxH texels (1 m), K KiB` (K = `Words().size() * 4 / 1024`).
5. In the frame, after the game's `OnUpdate` and before `graphics::FrameInput in;`: `if (map) map->Update(sim.World(),
   {camera.position.x, camera.position.z})` (`graphics::Camera::position`, a `glm::dvec3` in world metres); `in.map = map ? &map->Image() : nullptr`.
6. Exit line (beside the `GPU` lines): `Map: baked C columns (I incomplete), T texels known, max X.XX ms and N columns
   in one frame (budget 24)`.

**Tests**
- `MapBake.AnEditRebakesOnlyItsColumn`: bake everything (`Update` until no column is dirty); remember all texels;
  `FillBox` one voxel at the top of column (cx, cz); one `Update` -> `Stats().columnsBaked` grew by exactly 1 and
  only that column's texels may differ.
- `MapBake.NearestColumnsFirst`: a fresh baker over a loaded 12 x 12 chunk window, focus at its centre; one `Update`
  -> exactly 24 columns baked, every baked column at most as far as every unbaked one.

**Checks**: build; `tools/ctest.sh -R 'MapBake\.'`; `tools/ctest.sh`; doc-check. The engine part is checked in
card 11 (the game sets the extent).

**Must not break**: L10 (world jobs once per `Update`: the map submits no job); nothing reads `TakeChunkChanges`.

## Card 6: the explored mask

| | |
|---|---|
| Goal | The map knows where the player has been, and remembers it offline |
| Depends on | card 4 |
| Effort | 0.5 d (e) |
| Touches | voxel (`ExploredMask`, `MapBaker::Reveal` / `RevealAll`) |

**Steps**
1. `Reset`: cells `size / 4` per axis, bits cleared, `m_LastCell` = INT32_MIN.
2. `Reveal(feet)`: `c = (floor((feet.x - origin.x) / 4), floor((feet.z - origin.z) / 4))`; equal to `m_LastCell` ->
   0; else `m_LastCell = c`; for `dz = -16..16`, `dx = -16..16` with `dx^2 + dz^2 <= 256` (integers), cell inside:
   `Set` true -> count, append. Returns the count. A feet cell outside the extent still reveals the inside part.
3. `RevealAll`: every cell, z then x.
4. `MapBaker::Reveal` / `RevealAll`: for each newly set cell, the 16 texels `[4cx, 4cx + 4) x [4cz, 4cz + 4)` that are
   Known get the `kMapExplored` bit (`SetTexel`); unknown texels get it when baked (card 4 step 6).
5. `Encode`: `'V','X','E','X'`, u16 1, u16 cellsX, u16 cellsZ, i32 origin x, i32 origin z (little-endian), then the
   bit bytes as runs `(count 1..255, value)`. `Decode`: magic, version 1, cells equal to this mask's, origin equal,
   runs summing to exactly the byte count, no trailing byte; any failure -> false and the mask unchanged.

**Tests**
- `ExploredMask.RevealIsADiscOfCells`: extent 256 x 256 m at (0, 0); `Reveal({128, 128})` sets 797 cells (the
  integer points with `dx^2 + dz^2 <= 256`); the same feet again -> 0; `({130, 129})` (same cell) -> 0;
  `({-100, -100})` (cell (-25, -25), every cell of its disc outside) -> 0; a fresh mask, `Reveal({2, 2})` (cell
  (0, 0)) -> the quarter disc with `dx, dz >= 0` (the test counts it with its own loop; do not hard-code a number).
- `ExploredMask.EncodeDecodeRoundTrip`: reveal two discs, encode, decode into a reset mask of the same extent -> equal
  bits and `Count()`.
- `ExploredMask.DecodeRefusesMalformed`: wrong magic, version 2, other cell counts, a run of 0, one byte short, one
  trailing byte -> false, and `Count()` stays 0.

797 is exact: the integer points `(dx, dz)` with `dx^2 + dz^2 <= 256` (counted by a loop over -16..16). For the
corner case the test computes its own expected count; the card does not claim one.

**Checks**: build; `tools/ctest.sh -R 'ExploredMask\.'`; `tools/ctest.sh`; doc-check.

## Card 7: HUD quad kinds, projection and icons

| | |
|---|---|
| Goal | The game can put a map and marker bitmaps on the HUD |
| Depends on | card 1 |
| Effort | 0.5 d (e) |
| Touches | core (`:hud`). HUD contract: `kind` (shader side in card 9) |

**Steps**
1. `HudCanvas::Map(rect, view)`: empty rect -> nothing. `scale = clamp(lround(view.texelsPerPixel * 256), 1,
   65535)`; quad `{rect, color = scale | opacity << 24, bitsLo = bit_cast<uint32_t>((int32_t)lround(centre.x * 16)),
   bitsHi = same for y, kind = (look == Minimap ? 1 : 2) | (circle ? kHudFlagCircle : 0)}`; `Push`.
2. `HudCanvas::Cells(rect, bits, color)`: `bits & BitmapFont::kSolid`; zero bits or transparent -> nothing; quad kind 0.
3. `MapProjection::ToPixel(t) = (rect.x + rect.w / 2, rect.y + rect.h / 2) + (t - centre) / texelsPerPixel`;
   `ToTexel` the inverse. Use the same `centre` the quad carries after rounding to 1/16 (`round(c * 16) / 16`) and the
   rounded scale (`scale / 256`), so markers sit exactly where the shader draws.
4. `Inside(p, inset)`: circle -> `|p - c| <= min(w, h) / 2 - inset`; else the rect shrunk by inset. `ClampToRim`:
   inside -> p; else `c + normalize(p - c) * (min(w, h) / 2 - inset)` (circle) or clamped to the shrunk rect.
5. Icons (rows top to bottom, 5 characters, `#` set; write a `consteval` parser as `HudFont.inc` does or reuse it):
   - Arrow up, rows 2-6: `..#..`, `.###.`, `#####`, `..#..`, `..#..`; up-right, rows 2-6: `..###`, `...##`, `..#.#`,
     `.#...`, `#....`. Directions 2, 4, 6 rotate the up arrow's 5 x 5 by 90, 180, 270 degrees clockwise; 3, 5, 7 rotate
     the up-right one. Rows 0-1 and 7-8 empty.
   - Village: `.....`, `..#..`, `.###.`, `#####`, `#...#`, `#.#.#`, `#.#.#`, `#####`, `.....`.
   - Tower: `#.#.#`, `#####`, `.###.`, `.###.`, `.#.#.`, `.###.`, `.###.`, `#####`, `.....`.
   - Castle: `#.#.#`, `#####`, `#####`, `##.##`, `#...#`, `#...#`, `#####`, `.....`, `.....`.
   - Dungeon: `.....`, `.###.`, `#...#`, `#...#`, `#.#.#`, `#.#.#`, `#####`, `.....`, `.....`.
   - Site: `.....`, `..#..`, `.#.#.`, `#...#`, `.#.#.`, `..#..`, `.....`, `.....`, `.....`.
   - Spawn (flag): `##...`, `####.`, `#####`, `##...`, `#....`, `#....`, `#....`, `#....`, `.....`.
   - Dot rows 3-5: `.###.` x3; Cross rows 2-6: `#...#`, `.#.#.`, `..#..`, `.#.#.`, `#...#`.

**Tests** (`HudTests.cpp`)
- `HudCanvas.MapQuadPacksItsView`: `Map({10, 20, 144, 144}, {{100.5f, 37.25f}, 0.5f, MapLook::Parchment, true, 200})`
  -> one quad: rect equal, `color` = 128 | 200 << 24, `bitsLo` = 1608, `bitsHi` = 596, `kind` = 2 | 0x100. Negative
  centre `(-3.0, 0)` -> `bitsLo` = `bit_cast<uint32_t>(-48)`.
- `HudCanvas.CellsQuadKeepsItsBits`: `Cells(r, MapIcons::Dot(), c)` -> kind 0, bits equal; `Cells(r, 0, c)` -> none.
- `MapProjection.WorldToPixelAndBack`: rect (0, 0, 200, 100), centre (50, 50), 0.5 texel/px: `ToPixel({50, 50})` =
  (100, 50); `ToPixel({60, 50})` = (120, 50); `ToTexel(ToPixel(t)) == t` for 5 points.
- `MapProjection.MarkersOutsideTheMinimapClampToItsRim`: circle rect (0, 0, 144, 144) (centre (72, 72), radius 72): `ClampToRim({572, 72}, 6)` =
  (138, 72); a point inside -> unchanged.
- Arrows: `MapIcons::Arrow(2)` = the up arrow turned clockwise (rows 2-6 `..#..`, `..##.`, `#####`, `..##.`, `..#..`):
  add it to `HudCanvas.CellsQuadKeepsItsBits`.

**Checks**: build; `tools/ctest.sh -R 'HudCanvas\.|MapProjection\.|BitmapFont\.|HudLayout\.'`; `tools/ctest.sh`; doc-check.

**Must not break**: every existing HUD quad keeps kind 0 (`HudCanvas` zero-initialises it);
`LayoutContract.HudQuadMatchesVxHudQuad` (same size and offsets).

## Card 8: u_Map on the GPU

| | |
|---|---|
| Goal | The renderer keeps a device copy of the map image and binds it to the HUD pipeline |
| Depends on | cards 3, 7 |
| Effort | 0.5 d (e) |
| Touches | graphics (`Frame.cppm`, `Renderer.cpp`, `common.glsl`), engine (`in.map`), contract tests |

**Read first**: `Renderer.cpp` the voxel upload (from `const VkDeviceSize reserved = materialBytes + gridBytes;` to the
`vkCmdCopyBuffer(... v.grid.handle ...)`), `FindBuffer`, `kMinGridBytes`; `ContractTests.cpp` `kBuffers`, `Pipelines()`.

**Steps**
1. `common.glsl`: after the `VX_NEED_HUD` block:
   `#ifdef VX_NEED_MAP` / `layout(set = 0, binding = 11, std430) readonly buffer MapBuffer { uint words[]; } u_Map;` /
   `#endif`. (Declared only by `hud.frag`, card 9. Until then the contract table lists it but no shader declares it:
   land card 8 and card 9 together if `ShaderContract.EveryBufferOfTheTableIsUsedSomewhere` fails; the card 8 commit
   may hold the table change back to card 9.)
2. Renderer: a device-local `Buffer map` beside `voxels.grid`, created with `kMinMapBytes = (576 + 1) * 4` zeroed;
   `std::vector<uint64_t> mapPages` (the revision uploaded per page), `const void* mapSource` (the image pointer it
   mirrors) and its size.
3. In the voxel upload, after the grid: `input.map` null -> if the buffer is not the minimal one, replace it by the
   minimal zero buffer (deletion queue). Else: size differs or `mapSource != input.map` -> recreate at
   `Words().size() * 4`, all pages stale. Copy up to 64 stale pages (`PageRevisions()[p] != mapPages[p]`) in page
   order through `frame.staging` (reserve their bytes in `reserved` before the chunk uploads; the last page may be
   short), one `VkBufferCopy` each, `vkCmdCopyBuffer(cmd, frame.staging.handle, map.handle, n, copies)`; set
   `mapPages[p]`. The barrier the grid copy uses covers it (same pass, same stage); if the grid's barrier names its
   buffer, add one for `map`.
4. `FindBuffer`: `if (name == "u_Map") return &map;`.
5. Engine: `in.map = ...` (card 5 step 5).
6. ContractTests: `kBuffers` += `{"u_Map", 11, BindingType::StorageBuffer, "MapBuffer"}`; `Pipelines()` hud row
   buffers `{"u_Frame", "u_Hud", "u_Map"}`.
7. Memory log: the renderer's buffer total (`voxels.occ.size + ...`) adds `map.size`.

**Checks**: build; `tools/ctest.sh -R 'ShaderContract\.|LayoutContract\.'` (after card 9); `tools/ctest.sh`;
`./build/Shredborne --headless --validation --frames 200` exit 0 (no validation error).

**Must not break**: the grid upload order and its budget (`kStagingBytes`); L6.

## Card 9: the two looks in hud.frag

| | |
|---|---|
| Goal | Map quads draw the minimap and the parchment looks; cell quads draw exactly as before |
| Depends on | card 8 |
| Effort | 0.5 d (e) |
| Touches | shaders (`common.glsl`, `hud.vert`, `hud.frag`), contract tests. HUD contract change |

**Steps**
1. `common.glsl` constants (beside `VX_HUD_CELLS_*`): `const uint VX_HUD_KIND_CELLS = 0u;`,
   `VX_HUD_KIND_MAP_MINIMAP = 1u`, `VX_HUD_KIND_MAP_PARCHMENT = 2u`, `VX_HUD_FLAG_CIRCLE = 256u`,
   `VX_MAP_HEADER_WORDS = 64u`, `VX_MAP_PALETTE_WORDS = 256u`, `VX_MAP_TEXELS = 576u`, `VX_MAP_KNOWN = 0x01000000u`,
   `VX_MAP_EXPLORED = 0x02000000u`; the `VxHudQuad` comment: `data.w = kind`.
2. `hud.vert`: `layout(location = 1) flat out uvec4 v_Data;` = `u_Hud.quads[i].data`; `layout(location = 2) flat out
   vec2 v_Size;` = `rect.zw`.
3. `hud.frag` (`#define VX_NEED_MAP` before the include):

```glsl
layout(location = 1) flat in uvec4 v_Data;
layout(location = 2) flat in vec2 v_Size;

uint MapTexel(ivec2 t, ivec2 size) {
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size))) return 0u;
    return u_Map.words[VX_MAP_TEXELS + uint(t.x + t.y * size.x)];
}
float HeightDm(uint w) { return float(w & 0xFFFFu); }

vec4 Minimap(uint w, uint wx, uint wz) {
    if ((w & VX_MAP_KNOWN) == 0u) return vec4(28.0, 32.0, 38.0, 217.0) / 255.0;
    if ((w & VX_MAP_EXPLORED) == 0u) return vec4(52.0, 56.0, 62.0, 255.0) / 255.0;
    vec3 c = unpackUnorm4x8(u_Map.words[VX_MAP_HEADER_WORDS + ((w >> 16u) & 0xFFu)]).rgb;
    float h = HeightDm(w);
    float hx = (wx & VX_MAP_KNOWN) != 0u ? HeightDm(wx) : h;
    float hz = (wz & VX_MAP_KNOWN) != 0u ? HeightDm(wz) : h;
    float shade = clamp(1.0 + 0.04 * ((h - hx) + (h - hz)), 0.65, 1.3);
    float tint = 1.0 + min(0.08 * h / 320.0, 0.4);
    return vec4(clamp(c * shade * tint, 0.0, 1.0), 1.0);
}

vec4 Parchment(uint w, uint wx, uint wz) {
    vec3 paper = vec3(236.0, 222.0, 186.0) / 255.0;
    if ((w & VX_MAP_KNOWN) == 0u || (w & VX_MAP_EXPLORED) == 0u) return vec4(paper * 0.94, 1.0);
    vec4 ink = unpackUnorm4x8(u_Map.words[VX_MAP_HEADER_WORDS + VX_MAP_PALETTE_WORDS + ((w >> 16u) & 0xFFu)]);
    vec3 c = mix(paper, ink.rgb, ink.a);
    uint contour = max(u_Map.words[5], 1u);
    uint band = (w & 0xFFFFu) / contour;
    bool line = ((wx & VX_MAP_EXPLORED) != 0u && (wx & 0xFFFFu) / contour != band) ||
                ((wz & VX_MAP_EXPLORED) != 0u && (wz & 0xFFFFu) / contour != band);
    if (line) c = mix(c, vec3(110.0, 85.0, 60.0) / 255.0, (band % 5u) == 0u ? 0.85 : 0.55);
    return vec4(c, 1.0);
}

vec4 HotZones(vec4 c, vec2 texel, float texelsPerPixel) {
    uint n = min(u_Map.words[6], 8u);
    float pulse = 0.65 + 0.35 * sin(u_Frame.temporalParams.w * 6.2831853);
    for (uint i = 0u; i < n; ++i) {
        uint b = 8u + 4u * i;
        vec2 centre = vec2(float(int(u_Map.words[b])), float(int(u_Map.words[b + 1u]))) / 16.0;
        float radius = float(u_Map.words[b + 2u]) / 16.0;
        float d = distance(texel, centre);
        vec3 amber = vec3(255.0, 170.0, 60.0) / 255.0;
        if (abs(d - radius) < 1.0 * texelsPerPixel) c.rgb = mix(c.rgb, amber, 0.9 * pulse);
        else if (d < radius) c.rgb = mix(c.rgb, amber, 0.12 * pulse);
    }
    return c;
}
```

   `main`: `uint kind = v_Data.w & 0xFFu;` kind 0 -> today's code with `v_Data.x / .y / .z` (unchanged result),
   return. Else: `vec2 local = (v_Cell / vec2(VX_HUD_CELLS_X, VX_HUD_CELLS_Y) - 0.5) * v_Size;`
   circle flag: `r = 0.5 * min(v_Size.x, v_Size.y)`, `d = length(local)`; `d > r` -> `o_Color = vec4(0)`, return;
   `d > r - 2.0` -> the ring `(230, 225, 210)` opaque, return. `scale = float(v_Data.x & 0xFFFFu) / 256.0`;
   `centre = vec2(float(int(v_Data.y)), float(int(v_Data.z))) / 16.0`; `texel = centre + local * scale`;
   `t = ivec2(floor(texel))`; `size = ivec2(u_Map.words[0], u_Map.words[1])`; `w = MapTexel(t, size)`, `wx =
   MapTexel(t - ivec2(1, 0), size)`, `wz = MapTexel(t - ivec2(0, 1), size)`; look by kind; hot zones (texel in
   origin-relative metres = texel coordinates, since 1 texel = 1 m); `a = c.a * float(v_Data.x >> 24) / 255.0`;
   `o_Color = vec4(c.rgb * a, a)`.
4. Tests (`ContractTests.cpp`), with the parser of `HudCellGridMatchesTheFont` extended to `const uint` (strtol stops
   at the `u`): `LayoutContract.HudQuadKindsMatchTheGlslConstants` (`VX_HUD_KIND_CELLS` = `kHudKindCells`, the two map
   kinds, `VX_HUD_FLAG_CIRCLE` = `kHudFlagCircle`); `LayoutContract.MapHeaderMatchesTheGlslConstants`
   (`VX_MAP_HEADER_WORDS` = `voxel::kMapHeaderWords`, `VX_MAP_PALETTE_WORDS`, `VX_MAP_TEXELS` = `kMapTexelWord`,
   `VX_MAP_KNOWN` = `kMapKnown` (parse hex: use `strtol(..., 0)`), `VX_MAP_EXPLORED`). `VortexGraphicsTests` links
   `VortexGraphics` -> Core: `voxel::` constants are visible (Core holds voxel).

**Checks**: build; `tools/ctest.sh -R 'ShaderContract\.|LayoutContract\.'`; `tools/ctest.sh`; `VERIFY_SLOT=1
tools/verify.sh quick --only G3` - PASS and the screenshot's HUD unchanged (no map is drawn yet); doc-check.

**Must not break**: `LayoutContract.HudQuadMatchesVxHudQuad`, `HudCellGridMatchesTheFont`; no `discard` (L3); colours
are display values; the cell path's output bit for bit.

**Traps**: the centre words are int32 bit patterns in a `uint`: GLSL's `int(uint)` keeps the bit pattern, so
`int(v_Data.y)` gives the signed value (a centre left of the origin is negative). Never `floatBitsToInt` here.

## Card 10: landmarks, party rule, hot zones API

| | |
|---|---|
| Goal | The map knows what to mark and whom to show |
| Depends on | card 3 |
| Effort | 0.5 d (e) |
| Touches | worldgen (`PlanLandmarks`), sim (`ShowsOnMap`). Wire: no |

**Steps**
1. `worldgen::PlanLandmarks(plan, vs)`: settlements = distinct `BuildingRec::settlement` in ascending order; for
   each, `x = (sum over its buildings of (box[0] + box[3]) / 2) / count` (int64, floor division toward -inf; voxels
   relative to `header.origin`), likewise z (`box[2]`, `box[5]`); metres `= floor((origin + x) * vs)` -> `Village`
   `VILLAGE`. Then each `MarkerRec` with `kind == MarkerKind::Landmark`, in record order -> `Tower` `TOWER` at its
   `(x, z)` the same way. (Castles and dungeons: when `[wg-W3]` gives the marker a kind in `param`, map it here.)
2. `sim::ShowsOnMap(own, other)`: `own != kNoTeam && own == other`.
3. `MapImage::SetHotZones` exists (card 3); nothing else.

**Tests**
- `WorldGen.PlanLandmarksNameTheVillageAndItsTower`: `PlanRegion(..., VillageRecipe(), kits, 1, report)` (as
  `WorldGen.PlanGolden` builds it) -> exactly one `Village` and one `Tower`; both inside the plan area; the tower
  within 6 m of the village's centre (the W1 planner puts the stage tower at the site centre).
- `Combat.MapShowsTeammatesOnly`: (1, 1) true, (1, 2) false, (0, 0) false, (2, 2) true, (0, 1) false.

**Checks**: build; `tools/ctest.sh -R 'WorldGen\.PlanLandmarks|Combat\.MapShows'`; `tools/ctest.sh`; doc-check.

## Card 11: the minimap in the game

| | |
|---|---|
| Goal | Offline and in sessions the player sees the minimap with the arrow, teammates and landmarks |
| Depends on | cards 5, 6, 9, 10 |
| Effort | 0.5 d (e) |
| Touches | game (`src/MapHud.h`, `src/MapHud.cpp` new, `src/main.cpp`, `src/Session.cpp`, `src/Practice.cpp`), the game's CMakeLists (the new source) |

**Interfaces** (`src/MapHud.h`, plain game code, the include rule of `src/FightHud.h`)
```cpp
struct MapMarker { glm::vec2 metres{0.0f}; Rgba colour; bool dead = false; };
struct MapHudInput {
    const voxel::MapBaker& map;
    glm::vec2 feet{0.0f};                 ///< world x, z
    glm::vec3 look{0.0f, 0.0f, -1.0f};    ///< sim::LookDirection of the own yaw (pitch 0)
    std::span<const MapMarker> party;
    std::span<const voxel::MapHotZone> zones;   ///< for the full map's timers
    double seconds = 0.0;
};
class MapHud {
public:
    void DrawMinimap(HudCanvas& hud, const MapHudInput& in);
    void DrawFullMap(HudCanvas& hud, const MapHudInput& in, int zoom);   ///< zoom 1, 2, 4 (card 12)
    uint32_t LastLandmarks() const;   ///< drawn on the last frame (the exit line)
    uint32_t LastParty() const;
};
```

**Steps**
1. `DrawMinimap`: `d = 72 * hud.TextScale()`; `rect = PlaceRect(Anchor::TopLeft, {d, d}, hud.Size(), {8, 8})`;
   `view = {texel of feet (feet - origin, floats), 96.0f / d, MapLook::Minimap, true, 235}`; `hud.Map(rect, view)`.
   Projection `p = MapProjection{rect, view}` (rounded as card 7 says).
2. Landmarks: for each `map.Landmarks()`: `px = p.ToPixel(texel)`; inside (inset 6) -> `Cells` icon 5 x 9 cells at
   `scale = TextScale` px per cell, centred, colour `(250, 240, 210)`; outside -> at `ClampToRim(px, 6)`, colour
   alpha 150.
3. Party: each marker inside -> `Dot` in its colour (dead: `Cross`, grey `(150, 150, 150)`); outside: skipped.
4. Self: `dir8 = ((int)lround(atan2(look.x, -look.z) / (pi / 4)) % 8 + 8) % 8`, `Arrow(dir8)` white with a black
   shadow cell quad one pixel down-right first, at the minimap centre.
5. Offline (`src/main.cpp`): at start, after the plaza or `--region` is applied: `engine.SetMapExtent(
   MapExtentAround(spawn xz, 1024, -48))` (`--region`: `MapExtentOfBounds` of the plan area, margin 16); landmarks
   (`PLAZA` at the template anchor in metres = `anchor * vs`, or `PlanLandmarks`); explored file load (not
   headless: card 12's helper); each frame `map->Reveal(m_Feet xz)`; `DrawHud` draws the minimap first (under the
   other HUD) unless `--no-minimap`, the settings row is off, the pause menu or the full map is up.
6. Sessions (`SessionGame`): at Welcome: `SetMapExtent(MapExtentOfBounds(PlayBoundsOf(region).min, .max, 16, region
   height min))`, `RevealAll()`, landmarks `BASE` at `Welcome::spawn` (fight modes only); practice
   (`RunPractice` has the `SessionConfig`): `BLUE BASE` / `RED BASE` at `fight.teamSpawns`, felling `BLUE TOWER` /
   `RED TOWER` at `fight.towers` (pass them to `PlaySession` as a `std::vector<voxel::MapLandmark>`). Party markers:
   `RemotePlayers()` filtered by `sim::ShowsOnMap(TeamOf(Slot()), TeamOf(o.slot))`, colour `TeamColour`, dead from
   `o.Dead()`.
7. Exit line (game): `Map: L landmark(s), H hot zone(s), P party marker(s) drawn on the last frame`.

**Checks**: build; `./build/Shredborne --headless --validation --frames 400 --screenshot build/map-mini.png` exit 0,
the log has `Map: extent -512,-512 size 1024x1024 texels (1 m), 4098 KiB` (the origin depends on the spawn: read
it), `Map: baked ...` with T > 0, `Map: 1 landmark(s), 0 hot zone(s), 0 party marker(s) ...`; look at the screenshot
(minimap top left, the plaza icon); `VERIFY_SLOT=1 tools/verify.sh quick` PASS (G3's HUD line still N > 0; G16 is
full tier: run `VERIFY_SLOT=1 tools/verify.sh full --only G15,G16` and look at `build/session-check/a.png`).

**Docs**: `docs/owner-checks.md`: "Minimap: windowed offline and session; the arrow turns with the view; the plaza /
village icon at the rim points the right way; teammates' dots in practice".

**Traps**: `4098 KiB` = (576 + 1048576) x 4 / 1024 = 4098.25, printed by `{}` of an integer division as 4098: print
the integer. The minimap is drawn before the fight HUD so the banner and the feed stay on top.

## Card 12: the full map, M, zoom, flags, settings row

| | |
|---|---|
| Goal | M opens the parchment map of everything explored, with names and hot zones |
| Depends on | card 11 |
| Effort | 0.5 d (e) |
| Touches | game (`MapHud.cpp`, `main.cpp`, `Session.cpp`), core (`GameSettings::minimap`, `SettingsPage` row) |

**Steps**
1. `DrawFullMap(hud, in, zoom)`: `h = floor(0.8 * hud.Size().y)`; the extent's aspect `W / H`: rect `w = h * W / H`
   (if wider than 90 % of the width, fit to width); centred. Zoom 1: centre = the extent's centre, `texelsPerPixel =
   H / h`; zoom 2 / 4: `texelsPerPixel = H / (h * zoom)`, centre = feet clamped so the view stays inside the extent.
   `hud.DimBackground(0.5f)`; `hud.Map(rect, {centre, tpp, MapLook::Parchment, false, 255})`; a 2 px frame `(110, 85,
   60)` (`HudCanvas::Frame`).
2. Landmarks: icon at `2 x TextScale` per cell, ink `(70, 50, 35)`, the name centred under it (`Text`, scale
   `TextScale`); off the view (zoomed): skipped. Hot zones: `m:ss` under the centre (`startsInSeconds - elapsed since
   it was set`, floor, not below 0). Self arrow and party as on the minimap, at `2 x TextScale`.
3. Title `MAP` top centre of the rect; hint `M CLOSE   WHEEL ZOOM` under it.
4. Input (both games): M pressed (play keys, not paused) toggles; Escape with the map open closes it and does not
   open the pause menu; while open: no cast edge (clear the left-button latch), crosshair hidden, the minimap hidden;
   `Input::ScrollY()` > 0 -> zoom x2 (max 4), < 0 -> /2 (min 1).
5. Flags: `--map` (open from frame 1), `--no-minimap`, `--hot-zone X,Z,R,S` (repeatable, at most 8; a malformed value
   exits 2 `--hot-zone needs X,Z,R,S`); zones go to `map->Image().SetHotZones` once at start (offline and practice;
   `--connect` refuses `--hot-zone`, exit 2).
6. Settings: `GameSettings::minimap = true`, file key `minimap = on|off` after `play_bounds`; `SettingsPage::Item`
   `kMinimap = 7`, row `Minimap        on / off` after `Play bounds`; update `GameSettings.RoundTripsAndRefusesBadValues`
   and `SettingsPage.ActivatingCyclesThePresets` for the new row (the expected values of these tests change: this
   card gives them: the row list gains one entry before Back; `minimap` round-trips; `minimap = maybe` is refused).
7. Explored file (offline, not headless): key `seed<S>-<plaza|village>-<originX>_<originZ>`; load at start
   (`Decode` false -> log `Map: explored file <path> ignored (malformed)` and start empty); save at exit:
   `Map: explored saved E of N cells to <path>`.

**Checks**: build; `tools/ctest.sh -R 'GameSettings\.|SettingsPage\.'`; `./build/Shredborne --headless --validation
--frames 400 --map --hot-zone 24,24,12,30 --screenshot build/map-full.png` exit 0, `Map: 1 landmark(s), 1 hot zone(s)`;
look at the screenshot (parchment, contours around the plaza, the amber ring, `PLAZA`); `--practice --headless
--frames 900 --map --screenshot build/map-practice.png` exit 0 with `BLUE BASE` / `RED BASE` drawn; `tools/ctest.sh`;
doc-check.

**Docs**: README "Controls": M map, wheel zoom; `docs/owner-checks.md`: "Full map: M opens and closes it, Escape
closes it first, the wheel zooms, no cast while it is open; explored areas persist across two offline runs".

## Card 13: measure, and gate G25

| | |
|---|---|
| Goal | The map's cost on the UHD 620 is known and a gate keeps it working |
| Depends on | card 12 |
| Effort | 0.5 d (e) |
| Touches | tools (`verify.sh`, `lib/verify-gates.sh`, `selftest-verify.sh`, `tests/fixtures/verify/G25.log`), docs |

**Steps**
1. Measure (dev build, AC power, after `World: window filled`), each 3 runs, the median of the `GPU hud` and `GPU
   upload` lines and the `Map: baked` line:
   - `./build/Shredborne --headless --frames 1200 --camera=-1,61,65,-0.241,-0.55 --no-minimap` (baseline),
   - the same without `--no-minimap` (minimap),
   - the same with `--map` (full map),
   - `--neck-blast --after-fill --frames 600` (bakes after edits: `max X ms and N columns in one frame`).
   Budgets (map.md "Costs"): minimap <= 0.05 ms, full map <= 0.30 ms, bake <= 0.6 ms in a frame. Over budget: report
   it under "Numbers" and stop for the owner (do not tune past the card).
2. Gate G25 (tier quick, after G12 in `QUICK_GATES`; `gate_tiers` lists it in `quick full`; `--list` prints its
   command): `gate_G25() { G_LOG=$V/g25.log; game_run g25 180 --frames 400 --screenshot "$V/g25.png" --map
   --hot-zone 24,24,12,30; SHOTS+=("$V/g25.png"); judge_G25 "$G_LOG" "$RUN_EXIT" "$V/g25.png"; }`.
3. `judge_G25` (`verify-gates.sh`): `vg_begin G25`; `_g3_base "$@"` if it fits a 400-frame plaza run (it checks the
   exit, the build line and the HUD line: read it first), else `need_exit 0` and `need_build`;
   `need_line "Map: extent .* size [0-9]+x[0-9]+ texels" "Map: extent ..."`;
   `need_num "Map: baked [0-9]+ columns .*, ([0-9]+) texels known" ">" 0 "known texels" known`;
   `need_line "Map: 1 landmark\(s\), 1 hot zone\(s\)" "Map: 1 landmark(s), 1 hot zone(s) ..."`;
   `forbid_line "Map: extent too large" "Map: extent too large"`.
4. Fixture `tools/tests/fixtures/verify/G25.log` (from a real run's log, trimmed to the lines the judge and `_g3_base`
   read); self-test: `case_ok "G25 passes" G25 G25.log 0` and `case_bad` for: no landmark (`s/1 landmark/0 landmark/`),
   0 texels known, `Map: extent too large` added, exit 1.
5. CLAUDE.md section 2 table: a G25 row (command, Must); docs/verify.md: the `Map:` lines and G25.

**Checks**: `tools/selftest-verify.sh` passes (`Tools.VerifyAssertions`); `VERIFY_SLOT=1 tools/verify.sh quick` PASS
including G25; look at `g25.png`; doc-check (`Tools.DocCheck` compares the gate table with `--list`).

**Must not break**: every other judge and fixture; G21 (the `hud` pass stays under its 0.20 ms threshold: if the
minimap pushes it over, record a new baseline only after the owner accepts the change).

## Card 14: close [map-1]

| | |
|---|---|
| Goal | The stage is documented, verified and ready for the owner's review |
| Depends on | cards 1-13 |
| Effort | 0.5 d (e) |
| Touches | docs |

**Steps**
1. `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage.
2. map.md: status `Built ([map-1], date)`, the measured numbers in "Costs" replacing the (e) values, deviations as
   "As built" notes. game-ui.md "Rendering and cost": the map quads (kinds, `u_Map`) and their measured cost.
3. CLAUDE.md section 5 "HUD": add `HudQuad::kind` (0 cells, 1 / 2 map looks, bit 8 circle), `u_Map` binding 11 and
   the tests `LayoutContract.HudQuadKindsMatchTheGlslConstants`, `LayoutContract.MapHeaderMatchesTheGlslConstants`.
4. ROADMAP: `[map-1]` Built, the test count line (`ctest -N --test-dir build | tail -1` and the `TEST(` count),
   "Next" moves on; engine/docs/README stage IDs row `[map-1]`..`[map-3]` if the integrator has not added it.
5. `VERIFY_SLOT=1 tools/verify.sh full --ci` PASS; `VERIFY_SLOT=1 tools/verify.sh soak` in the background (a soak
   failure unrelated to the map is recorded in ROADMAP as a known flake, not chased).
6. Report for the stage review with the numbers of card 13.

## Values this stage lands

notes/net.md section 2 and llm/README "Values planned across the packs":
- No wire value: `net::kProtocolVersion`, `MessageType`, `EventKind`, `PlayerButton` unchanged. [map-2] takes the
  next free `MessageType` and protocol (expected: after `[mag-2]`'s `Attune` = 24 and protocol 13, so 25 and 14).
- `sim::kPlayerRuleRevision` unchanged; `buildHash` unchanged.
- HUD contract: `HudQuad::kind` (`kHudKindCells` 0, `kHudKindMapMinimap` 1, `kHudKindMapParchment` 2,
  `kHudFlagCircle` 0x100); `u_Map` binding 11 (`MapBuffer`); map header 64 words, palettes 2 x 256, texels from
  word 576; texel bits 0-15 height dm, 16-23 id, 24 Known, 25 Explored.
- Settings: file key `minimap`, page item `kMinimap = 7`.
- Gate G25 (tier quick; proposed number, the integrator reconciles with the README's fixed table).
