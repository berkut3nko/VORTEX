module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

export module vortex.voxel:land;

import :chunk;
import :generator;
import :material;

export namespace vortex::voxel {

    /// @brief Biome ids of the drawn world map (the game's tools/worldmap, `biome_ids.png`).
    enum class LandBiome : uint8_t {
        Sea = 0,
        Lake = 1,
        Grassland = 2,
        Forest = 3,
        DarkForest = 4,
        Desert = 5,
        Mountain = 6,
        RockIsle = 7,
    };

    enum class WaterKind : uint8_t { None = 0, Sea = 1, Lake = 2, River = 3 };

    /// @brief Where a river's last cell drains: the sea (or the map border), a lake, or another river.
    enum class RiverEnd : uint8_t { Sea, Lake, River };

    /// @brief Kinds of points of interest (land.md "Points of interest and roads"). Values are file names' order.
    enum class LandPointKind : uint8_t { Castle = 0, Village = 1, Landing = 2, Cave = 3, Landmark = 4, Crossing = 5 };

    /// @brief A point of interest in map metres (from the map's corner: x east, z south).
    struct LandPoint {
        LandPointKind kind = LandPointKind::Village;
        float x = 0.0f;
        float z = 0.0f;
        bool fromFile = false;          ///< placed by the owner's points.json, not scored
    };

    /// @brief The drawn map's fields as the world map tool writes them: one value per pixel, row by row.
    struct LandSource {
        int width = 0;
        int height = 0;
        float metresPerPixel = 5.0f;
        std::vector<float> heights;     ///< metres; the sea is at 0 m
        std::vector<uint8_t> biomes;    ///< LandBiome values
        std::vector<uint8_t> regions;   ///< 0 = water
        int starterRegion = 0;          ///< world.json "starter_region" (0 = none)
        /// The owner's points.json, when the map folder has one: each kind it names replaces the scored point.
        std::vector<LandPoint> points;
    };

    /**
     * @brief The golden hashes of the land code (land.md "Decision" 3): what Land.BakeGoldenHash,
     * LandStarter.GoldenHash and LandSampling.GoldenHash must produce. A change to the bake or the sampling changes one
     * on purpose (the test prints the new value); kLandGoldenHash mixes them into the session's build hash, so builds
     * that bake or sample differently refuse each other.
     */
    inline constexpr uint64_t kLandBakeGoldenHash = 0x90a1458c7d9c497eull;
    inline constexpr uint64_t kLandStarterGoldenHash = 0x3518dcab484973e2ull;
    inline constexpr uint64_t kLandSamplingGoldenHash = 0xc013c33ac06d6983ull;
    /// @brief Pins Hollowkeep's plan and its stamped voxels on `Fort.GoldenHash`'s test map (vortex.voxel:fort, fortress.md).
    inline constexpr uint64_t kFortGoldenHash = 0xed2f382ac10146d2ull;
    inline constexpr uint64_t kLandGoldenHash =
        kLandBakeGoldenHash ^ (kLandStarterGoldenHash * 3) ^ (kLandSamplingGoldenHash * 5) ^ (kFortGoldenHash * 7);

    /// @brief A 64-bit hash of the map's inputs: the sizes, the scale, every pixel of the height, biome and region
    /// fields, the starter region and the owner's points. Two sides with the same files have the same value.
    uint64_t LandSourceHash(const LandSource& source);

    /**
     * @brief The knobs of the bake (engine/docs/design/land.md "The bake, step by step"). Units are metres and
     * square metres; the defaults are tuned on the first drawn map.
     */
    struct LandSettings {
        float cellSize = 4.0f;              ///< the coarse grid: one cell per chunk column

        // L3a: stream power (valleys), implicit, n = 1, drainage-area exponent m = 0.5 (a sqrt: no libm pow).
        int streamIterations = 40;
        float streamK = 2.0e-4f;            ///< K * dt for erodibility 1, per iteration
        float maxStreamCut = 12.0f;         ///< no cell is cut deeper than this below its raw height
        float lakeRimCut = 0.5f;            ///< ... nor a cell next to a drawn lake (keeps its rim)

        // L3b: slopes steeper than the talus angle slump (the soft ground only keeps gentle slopes).
        int thermalIterations = 6;
        float softTalus = 0.9f;             ///< rise over run
        float hardTalus = 3.0f;

        // L3c: droplets (gullies and fans), per land cell.
        float dropletsPerLandCell = 1.0f;
        int dropletSteps = 80;
        float maxDropletCut = 3.0f;

        // L4: water. Closed basins first get an outlet at most maxBreach deep (a drawn lake's basin: as deep as it
        // needs to bring it to its drawn level); what stays below is a lake or a filled puddle.
        float maxBreach = 6.0f;
        float minLakeArea = 2000.0f;        ///< a smaller basin is filled (a puddle), unless the map draws a lake there
        float minLakeDepth = 0.75f;
        float riverArea = 60000.0f;         ///< drainage area where a river starts
        float riverFreeboard = 0.3f;        ///< water level below the bank
        float reachStep = 0.5f;             ///< river levels are multiples of this

        // L6-L8: the starter region in detail.
        bool starterDetail = true;
        float fineMargin = 160.0f;          ///< metres of fine grid around the starter region's box
        float roadMaxGrade = 0.16f;         ///< a road bed's steepest step (about 1:6); steeper ground costs much more
        float trailMaxGrade = 0.35f;        ///< a trail's (a footpath to the cave and the landmark): rough steps

        // Strata: rock hardness by height bands, tilted by slow noise.
        float strataBand = 5.0f;
        uint64_t seed = 0x4c414e44ull;       ///< "LAND": only the strata and the droplets use it
    };

    struct LandLake {
        float level = 0.0f;
        uint32_t cells = 0;
        float maxDepth = 0.0f;
        bool drawn = false;                 ///< the map draws a lake on at least one of its cells
    };

    /// @brief A run of river cells at one water level. `first` indexes the river's `cells`.
    struct LandReach {
        uint32_t first = 0;
        uint32_t count = 0;
        float level = 0.0f;
    };

    struct LandRiver {
        std::vector<uint32_t> cells;        ///< from the source down; each drains into the next
        std::vector<LandReach> reaches;
        RiverEnd end = RiverEnd::Sea;
        uint32_t endCell = 0;               ///< the sea, lake or river cell the last cell drains into
    };

    struct LandTimings {
        double resample = 0, streamPower = 0, thermal = 0, droplets = 0, water = 0, rivers = 0, starter = 0, total = 0;
    };

    /// @brief Surface kinds of the fine grid (what the generator puts on top instead of the biome's cover).
    enum class LandSurface : uint8_t { Natural = 0, Road = 1, Site = 2, StreamBed = 3 };

    /**
     * @brief The starter region's fine grid (land.md L6): 1 m cells whose height is the coarse height plus `delta`.
     * Cell (i, j) covers map metres [x0 + i, x0 + i + 1) x [z0 + j, z0 + j + 1); `delta` is 0 on the outer ring, so the
     * fine grid joins the coarse land with no seam.
     */
    struct LandFine {
        int x0 = 0, z0 = 0;                 ///< map metres of the grid's corner
        int width = 0, height = 0;          ///< cells (metres)
        std::vector<float> delta;
        std::vector<LandSurface> surface;
        std::vector<uint8_t> detail;        ///< 0-255: how much of the biome's detail noise stays (roads and the site: 0)
        uint32_t Index(int i, int j) const { return uint32_t(j) * uint32_t(width) + uint32_t(i); }
        bool Empty() const { return width == 0; }
    };

    /// Directions around a site: every 15 degrees, k = 0 along +x, turning towards +z (LandSiteDirection).
    inline constexpr int kSiteDirections = 24;

    /**
     * @brief Hollowkeep's site (land.md "The starter region"): a natural spur or knoll, its relief kept. The keep stands
     * on the high point; the curtain wall follows the contour about 7 m below it and the inner wall the contour about 3 m
     * below (radii per direction, smoothed); the gate and the dug ditch face the neck (the highest ground around).
     */
    struct LandSite {
        bool placed = false;
        float x = 0.0f, z = 0.0f;           ///< map metres of the keep's middle (the site's high point)
        float keepGround = 0.0f;            ///< the keep pad's level
        float gateGround = 0.0f;            ///< the gatehouse pad's level (the bridge's deck, the castle road's end)
        int gate = 0;                       ///< the direction (0..23) of the gate, the neck and the approach
        std::array<float, kSiteDirections> curtain{};   ///< the curtain wall's radius (metres) per direction
        std::array<float, kSiteDirections> inner{};     ///< the inner wall's radius (metres) per direction
        float searchCost = 0.0f;            ///< the search's score (lower is better)
    };

    /// @brief The unit vector (x, z) of site direction k (0..23), from a fixed table: no libm function.
    glm::vec2 LandSiteDirection(int k);

    /// @brief The site direction (0..23) nearest to (dx, dz).
    int LandSiteNearestDirection(float dx, float dz);

    /// @brief The gate's axis squared to x or z (the gatehouse, its passage and the bridge are axis-aligned).
    glm::vec2 LandSiteGateAxis(const LandSite& site);

    /// @brief How far (map metres) the point lies outside the curtain along the ray from the keep, in the 4 sectors
    /// around the gate (the ditch's sectors); false outside them.
    bool LandSiteOutsideCurtain(const LandSite& site, float mx, float mz, float& off);

    /// @brief The castle's bridge (land.md "Hollowkeep's greybox"): metres along the gate's axis from the gate's curtain
    /// point to 1 m past the ditch (the first point more than 10 m outside the curtain), 14 m if none.
    float LandSiteBridgeSpan(const LandSite& site);

    /// @brief A road (land.md L8): fine-grid points from one point of interest to another, and its bed's heights.
    struct LandRoad {
        uint8_t from = 0, to = 0;           ///< indexes into LandMap::points
        std::vector<float> x, z, bed;       ///< map metres, one per metre of road
        float length = 0.0f;
        float maxGrade = 0.0f;              ///< of the bed
        uint32_t fordCells = 0;             ///< a trail's points in a river (a ford)
        uint32_t bridgeCells = 0;           ///< a road's points over a river (a timber bridge at the bed's height)
        bool trail = false;                 ///< a footpath: narrower, up to LandSettings::trailMaxGrade
    };

    /// @brief A cave (land.md L7): a tunnel from its mouth into the hill, ending in a chamber.
    struct LandCave {
        bool placed = false;
        float x[2] = {}, y[2] = {}, z[2] = {};   ///< the tunnel's axis from the mouth to the chamber, map metres
        float radius = 2.2f;
        float chamberRadius = 5.0f;
    };

    /**
     * @brief The baked coarse fields (land.md "Fields of the map"). Cell (x, z) is `z * width + x`; x grows east
     * and z south, as the drawn map's columns and rows.
     */
    struct LandMap {
        int width = 0;
        int height = 0;
        float cellSize = 4.0f;
        std::vector<float> rawHeights;      ///< the drawn map's height, resampled
        std::vector<float> eroded;          ///< after erosion, before the basins' outlets are cut (L4)
        std::vector<float> heights;         ///< eroded, lakes' puddles filled, river beds cut
        std::vector<float> water;           ///< water level where waterKind != None
        std::vector<WaterKind> waterKind;
        std::vector<uint8_t> biomes;
        std::vector<uint8_t> regions;
        std::vector<uint8_t> hardness;      ///< 0-15
        std::vector<float> flow;            ///< drainage area in square metres
        std::vector<uint16_t> lakeOf;       ///< 0 = none, else index into lakes + 1
        std::vector<LandLake> lakes;
        std::vector<LandRiver> rivers;
        int starterRegion = 0;              ///< the source's, copied
        uint64_t sourceHash = 0;            ///< LandSourceHash of the source it was baked from (not in LandHash)
        LandFine fine;                      ///< the starter region in detail (L6)
        LandSite site;
        std::vector<LandPoint> points;      ///< L7
        std::vector<LandRoad> roads;        ///< L8
        LandCave cave;
        uint32_t breachedCells = 0;
        uint32_t puddlesFilled = 0;
        uint32_t banksRaised = 0;
        LandTimings seconds;                ///< not part of LandHash

        uint32_t Index(int x, int z) const { return uint32_t(z) * uint32_t(width) + uint32_t(x); }
    };

    /// @brief Reads `height.png` (16-bit), `biome_ids.png`, `region_ids.png`, the scale from `world.json` and, when the
    /// folder has one, the owner's `points.json` (see WriteLandPoints).
    bool LoadLandSource(const std::filesystem::path& dir, LandSource& out, std::string& error);

    /// @brief Bakes the coarse world map (L3 erosion, L4 water) from the drawn map. Bit for bit the same on every
    /// machine for the same source and settings (no libm function decides a value; Land.BakeGoldenHash).
    LandMap BakeLand(const LandSource& source, const LandSettings& settings = {});

    /**
     * @brief Bakes the starter region in detail (L6-L8) onto a coarse map: the fine grid with strata cliffs,
     * Hollowkeep's site, the points of interest (the source's points.json first), the cave and the roads. BakeLand calls
     * it when LandSettings::starterDetail is set.
     */
    void BakeLandStarter(const LandSource& source, const LandSettings& settings, LandMap& map);

    /// @brief Hash of everything baked (not the timings).
    uint64_t LandHash(const LandMap& map);

    /**
     * @brief The rules a baked map keeps: the coastline stays where the map draws it, each lake has one level over
     * a bed below it, river levels never rise downstream and stay above their bed, and every river reaches the sea
     * or a lake (through the rivers it joins). Returns "" when they hold, else the first broken rule.
     */
    std::string CheckLand(const LandMap& map);

    /**
     * @brief Writes the baked fields and the review pictures: `land_height.png` and `land_water.png` (16-bit grey,
     * metres = value / 256 - 64; water 0 = none), `land_flow.png` (8-bit, log drainage area), `land_relief.png`
     * (shaded relief with water), `land_change.png` (red cut, green raised; full colour at 5 m).
     */
    bool WriteLandFiles(const LandMap& map, const std::filesystem::path& dir, std::string& error);

    /**
     * @brief Writes `points.json`: the map's points of interest in map metres, one object per point
     * (`{"kind": "village", "x": 2450.0, "z": 760.0}`). Copied into the map folder and edited, it places those kinds
     * (the castle: the middle of Hollowkeep's site); crossings are found by the roads and ignored when read.
     */
    bool WriteLandPoints(const LandMap& map, const std::filesystem::path& path, std::string& error);

    /// @brief The name of a point kind in points.json ("castle", "village", "landing", "cave", "landmark", "crossing").
    const char* LandPointName(LandPointKind kind);

    /// @brief The baked coarse height in metres at map metres (Catmull-Rom over the cells, clamped to the 4 nearest).
    float LandCoarseHeight(const LandMap& map, float mx, float mz);

    /// @brief The fine grid's height correction at map metres (bilinear; 0 outside the fine grid).
    float LandFineDelta(const LandMap& map, float mx, float mz);


    /// @brief How the map becomes voxels (land.md "From the map to voxels", [land-3]).
    struct LandGeneratorSettings {
        float voxelSize = kDefaultVoxelSize;
        WorldMaterials materials;
        /// World metres of the map's corner (cell 0, 0). The default puts the map's centre at the world origin
        /// (CenteredLandOrigin).
        glm::vec2 origin{0.0f};
        uint32_t seed = 1;
        bool trees = true;
        /// Stamp Hollowkeep (fortress.md) on the bake's site: the game's --no-hollowkeep clears it.
        bool fort = true;
        /// As TerrainSettings::skipClassifiedRows: false (tests only) runs the voxel loop for every row.
        bool skipClassifiedRows = true;
    };

    /// @brief The origin that puts the map's centre at the world origin.
    glm::vec2 CenteredLandOrigin(const LandMap& map);

    /// @brief The origin the game and sessions use (land.md "From the map to voxels"): the starter region's village at
    /// the world origin when the bake placed one, else the map's centre (CenteredLandOrigin).
    glm::vec2 LandWorldOrigin(const LandMap& map);

    /**
     * @brief The chunk generator of the baked map: ground height, biome materials, water at its fixed levels and
     * trees. A pure function of (map, settings, chunk coordinate); its height and water are pinned bit for bit by
     * `LandSampling.GoldenHash`.
     */
    std::shared_ptr<IWorldGenerator> CreateLandGenerator(std::shared_ptr<const LandMap> map,
                                                         const LandGeneratorSettings& settings);

    /// @brief Ground height in metres at a world position (no trees): the float the generator turns into voxels.
    float LandHeight(const LandMap& map, const LandGeneratorSettings& settings, float x, float z);

    /// @brief Water level in metres at a world position, or kNoLandWater where the ground is dry.
    float LandWaterLevel(const LandMap& map, const LandGeneratorSettings& settings, float x, float z);
    inline constexpr float kNoLandWater = -1.0e9f;

    /**
     * @brief A dry, fairly level spot (world metres x, z) near the middle of a region, away from water; with `region` 0
     * the starter region's village when the bake placed one, else the starter region. Without level ground there: the region's least steep dry cell.
     */
    glm::vec2 LandSpawn(const LandMap& map, const LandGeneratorSettings& settings, int region = 0);
}
