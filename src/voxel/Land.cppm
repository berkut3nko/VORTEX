module;

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

export module vortex.voxel:land;

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

    /// @brief The drawn map's fields as the world map tool writes them: one value per pixel, row by row.
    struct LandSource {
        int width = 0;
        int height = 0;
        float metresPerPixel = 5.0f;
        std::vector<float> heights;     ///< metres; the sea is at 0 m
        std::vector<uint8_t> biomes;    ///< LandBiome values
        std::vector<uint8_t> regions;   ///< 0 = water
    };

    /**
     * @brief The knobs of the bake (engine/docs/design/land.md "The bake, step by step"). Units are metres and
     * square metres; the defaults are tuned on the first drawn map.
     */
    struct LandSettings {
        float cellSize = 4.0f;              ///< the coarse grid: one cell per chunk column

        // L3a: stream power (valleys), implicit, n = 1.
        int streamIterations = 40;
        float streamK = 2.0e-4f;            ///< K * dt for erodibility 1, per iteration
        float streamM = 0.5f;               ///< drainage-area exponent
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
        double resample = 0, streamPower = 0, thermal = 0, droplets = 0, water = 0, rivers = 0, total = 0;
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
        uint32_t breachedCells = 0;
        uint32_t puddlesFilled = 0;
        uint32_t banksRaised = 0;
        LandTimings seconds;                ///< not part of LandHash

        uint32_t Index(int x, int z) const { return uint32_t(z) * uint32_t(width) + uint32_t(x); }
    };

    /// @brief Reads `height.png` (16-bit), `biome_ids.png`, `region_ids.png` and the scale from `world.json`.
    bool LoadLandSource(const std::filesystem::path& dir, LandSource& out, std::string& error);

    /// @brief Bakes the coarse world map (L3 erosion, L4 water) from the drawn map. Deterministic for one build.
    LandMap BakeLand(const LandSource& source, const LandSettings& settings = {});

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
}
