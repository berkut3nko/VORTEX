module;

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

export module vortex.sim:region;

import vortex.voxel;
import vortex.scene;
import :simulation;
import :worldop;
import :blast;
import :player;

export namespace vortex::sim {

    /// @brief Which generator a region's terrain comes from. Values are wire format (never renumber).
    enum class RegionGenerator : uint8_t {
        Terrain = 0,   ///< voxel::CreateTerrainGenerator(RegionDesc::terrain)
        Flat = 1,      ///< voxel::CreateFlatGenerator(flatGroundVoxels, grass, stone): tests and drills
        Land = 2,      ///< voxel::CreateLandGenerator(RegionDesc::land): the baked world map (land.md [land-5])
    };

    /// @brief Region templates a server knows. Values are wire format (never renumber).
    enum class RegionTemplateId : uint32_t {
        Empty = 0,        ///< Pristine terrain: no operations, no objects, spawn at the play bounds' centre.
        Playground = 1,   ///< The plaza of the Shredborne sandbox (PlaygroundTemplate).
    };

    /**
     * @brief Everything both sides of a session need to build the same world
     * (docs/design/networking.md, "Region start-up and terrain agreement").
     * @details Travels in `Welcome`: the client builds its World and generator from it, never from
     * its own settings or command line. The voxel size of the session is `terrain.voxelSize`
     * (exact float bits on the wire). Input from the network: check it with IsValid first.
     */
    struct RegionDesc {
        RegionTemplateId templateId = RegionTemplateId::Empty;
        uint32_t seed = 0;                     ///< The region's own seed (probe chunks, template choices).
        /// Play bounds in whole chunks, x and z, [min, max). Casts centred outside are rejected; the
        /// server window is these plus kServerWindowMarginChunks on every side.
        glm::ivec2 playMinChunk{0};
        glm::ivec2 playMaxChunk{0};
        /// World metres [min, max): the full-height window's rows on both sides (rounded outwards
        /// to kRegionHeightSnap) and the build ceiling. Rows below are ground, rows above air.
        glm::vec2 heightRange{-48.0f, 128.0f};
        RegionGenerator generator = RegionGenerator::Terrain;
        int32_t flatGroundVoxels = 0;          ///< Flat: voxels y < this are ground.
        /// Land (land.md "Decision" 3): the server's baked map (voxel::LandHash) and its inputs
        /// (voxel::LandSourceHash). No map travels: each side bakes its own and a client compares
        /// (CheckRegionLand).
        uint64_t landHash = 0;
        uint64_t landSourceHash = 0;
        /// Land: this process's own baked map (never on the wire): the server's from its session file
        /// (SetRegionLand), a client's attached after CheckRegionLand. Not compared by operator==.
        std::shared_ptr<const voxel::LandMap> land;
        /// The terrain (and the session's voxel size). `materials` are the default world
        /// material ids; Flat uses `materials.grass` / `materials.stone`.
        voxel::TerrainSettings terrain;

        bool operator==(const RegionDesc& other) const;
    };

    /// The height range is rounded outwards to this many metres on both sides (WorldConfig::heightSnap).
    inline constexpr float kRegionHeightSnap = 16.0f;
    /// Largest play bounds per axis, in chunks (stage 9's 1 km at 0.1 m is 313).
    inline constexpr int32_t kMaxRegionChunks = 512;

    /// @brief True when `region` can be built safely: voxel::IsValid(terrain), non-empty play bounds
    /// of at most kMaxRegionChunks per axis well inside +-2^25 chunks, a finite height range of
    /// at most 128 rows, a flat ground inside the range, and for a playground play bounds that hold
    /// the plaza around the origin (+-kPlaygroundHalfMetres). `why` gets the first reason.
    bool IsValid(const RegionDesc& region, std::string* why = nullptr);

    /// @brief The window of a region (server and, until networking stage 7, client): the play
    /// bounds plus kServerWindowMarginChunks chunks on all four horizontal sides; y is set by the
    /// full-height range (size 1 and row 0 here).
    glm::ivec3 RegionWindowMinChunk(const RegionDesc& region);
    /**
     * @brief Where a player's feet may go in a session on `region` (sim::StepPlayer's `area`): the
     * play bounds less `tuning.radius` on every side, so the capsule stays inside the play bounds.
     * Server and client compute it from `Welcome` alike (doubles, then floats). Its reach
     * (PlayerReach: the capsule grown by 1 m) stays more than a chunk inside the server's window
     * (kServerWindowMarginChunks) at every session voxel size, so StepPlayer's gate never meets a
     * chunk outside the window.
     */
    PlayArea PlayAreaOf(const RegionDesc& region, const PlayerTuning& tuning);
    /// @brief The play bounds of `region` in world metres, x and z: playMinChunk / playMaxChunk x chunk
    /// metres (32 x the voxel size), computed in doubles then rounded to floats as PlayAreaOf does. What
    /// the client draws ([wp-5]); PlayAreaOf (the feet area) is these less PlayerTuning::radius.
    PlayArea PlayBoundsOf(const RegionDesc& region);
    /// @brief The chunk rows [x, y) of the region's full-height window (World's rounding).
    glm::ivec2 RegionRows(const RegionDesc& region);
    glm::ivec3 RegionWindowSize(const RegionDesc& region);

    /**
     * @brief `base` with everything a session decides: the voxel size, the window size, the
     * full-height range (`fullHeight`, `heightRange`, `heightSnap` = kRegionHeightSnap) and no
     * save directory. Streaming knobs (`packHidden`, `nearRadius`, job limits) are kept: packing
     * never changes a hash or an operation's result.
     */
    voxel::WorldConfig RegionWorldConfig(const RegionDesc& region, voxel::WorldConfig base = {});
    /// @brief A point whose window (World::LoadBlocking / Update) is RegionWindowMinChunk: the
    /// middle of the chunk the window centres on, at the middle of the height range.
    glm::dvec3 RegionWindowFocus(const RegionDesc& region);
    /// @brief The region's generator. `ids` = the default world materials of this process. A Land region
    /// needs its `land` map attached (nullptr without it).
    std::shared_ptr<voxel::IWorldGenerator> MakeRegionGenerator(const RegionDesc& region, const voxel::WorldMaterials& ids);

    /// @brief The land generator's settings for a region: its voxel size, `ids`, the starter village at the
    /// world origin (voxel::LandWorldOrigin).
    voxel::LandGeneratorSettings RegionLandSettings(const RegionDesc& region, const voxel::LandMap& map, const voxel::WorldMaterials& ids);

    /// @brief A server's land region: attaches `map` and sets the hashes the Welcome carries.
    void SetRegionLand(RegionDesc& region, std::shared_ptr<const voxel::LandMap> map);

    /**
     * @brief A client's check of a Welcome's land region against its own baked map: equal baked map hashes
     * attach `map` and return true. Otherwise `why` says what differs: the map files (the source hashes
     * differ) or only the bake (the same files baked by different code).
     */
    bool CheckRegionLand(RegionDesc& region, std::shared_ptr<const voxel::LandMap> map, std::string& why);
    /// @brief The blast centre (floor(centre / 16)) lies inside the play bounds and the world's rows.
    bool InPlayBounds(const RegionDesc& region, const voxel::World& world, const voxel::SphereQ& sphere);
    /**
     * @brief A cast a session server may run: InPlayBounds, a radius of at most kMaxCastRadiusQ (its
     * island box then stays inside the window's margin) and a carve (voxel::SphereVoxelBounds) inside
     * the window's rows (a carve through the floor or the ceiling would meet chunks that are not
     * loaded: the server would fail).
     */
    bool CastFitsRegion(const RegionDesc& region, const voxel::World& world, const voxel::SphereQ& sphere);
    /// @brief True when `world`'s window is the region's (corner and rows): what server and client
    /// check after World::LoadBlocking(RegionWindowFocus).
    bool WindowIsRegions(const voxel::World& world, const RegionDesc& region);

    // --- a session client's window (docs/design/networking.md "Stage 7a plan", "The moving client
    // window (M-b)") ------------------------------------------------------------------------------

    /// @brief True when a client whose window is `windowChunks` wide (x and z) is not exactly the
    /// region's window: its window then follows its player, and on a region smaller than it reaches
    /// beyond the server's window (those columns are pristine for good). False for `windowChunks` 0
    /// (the region's window, fixed) and for a region whose window is exactly windowChunks x windowChunks.
    bool ClientWindowMoves(const RegionDesc& region, int32_t windowChunks);
    /// @brief The follow hysteresis of a moving client window (WorldConfig::recenterSlackChunks).
    inline constexpr int32_t kClientWindowSlackChunks = 1;
    /**
     * @brief A session client's world: RegionWorldConfig when the region's window is exactly
     * `windowChunks` wide or `windowChunks` is 0 (ClientWindowMoves false), else a `windowChunks` x
     * the region's rows x `windowChunks` window
     * that follows the player and is never clamped to the region (columns beyond the server's window
     * are pristine for good), with `recenterSlackChunks` = kClientWindowSlackChunks (it moves once
     * the player is more than that from its centre chunk: review of stage 7a). Always `reportLoads`
     * (chunk sync replays what a chunk missed while it loaded) and no `keepUnloadedEdits` (a chunk
     * that leaves the window comes back from the server).
     */
    voxel::WorldConfig ClientWorldConfig(const RegionDesc& region, int32_t windowChunks, voxel::WorldConfig base = {});
    /// @brief The minimum corner (y = the region's first row) of the window World::Update(focus)
    /// gives a ClientWorldConfig world placed there (or moved there: a step past the slack centres it
    /// on the focus); the region's window when it does not move.
    glm::ivec3 ClientWindowMinChunk(const RegionDesc& region, int32_t windowChunks, const glm::dvec3& focus);
    /// @brief The window size of ClientWorldConfig (y = the region's rows).
    glm::ivec3 ClientWindowSize(const RegionDesc& region, int32_t windowChunks);
    /// @brief Store slots of a ClientWorldConfig world: RegionChunkCapacity, or the cells of the
    /// moving window (at least 1024, at most kMaxRegionStoreSlots).
    uint32_t ClientChunkCapacity(const RegionDesc& region, int32_t windowChunks);

    /// Store slots a session world gets at most by default (2 GiB of dense chunks): a store that
    /// fills is the designed failure, before the process runs out of memory.
    inline constexpr uint32_t kMaxRegionStoreSlots = static_cast<uint32_t>((2ull << 30) / sizeof(voxel::Chunk));
    /**
     * @brief Store slots for a world holding the region's whole window (server, and a client until
     * networking stage 7): every cell of the window (dense or not; the store allocates a chunk only
     * when a slot is used, so a slot costs its pointer until then), at least 1024, at most
     * kMaxRegionStoreSlots.
     */
    uint32_t RegionChunkCapacity(const RegionDesc& region);

    /// @brief A light of a template: an `EntitySpawn` of kind Light in a session.
    struct TemplateLight {
        glm::dvec3 position{0.0};
        glm::vec3 color{1.0f};
        float intensity = 5.0f;
        float radius = 8.0f;
        friend bool operator==(const TemplateLight&, const TemplateLight&) = default;
    };

    /// @brief An object of a template (a dynamic body). Server only until networking stage 6.
    struct TemplateObject {
        std::shared_ptr<voxel::VoxelModel> model;
        glm::dvec3 position{0.0};             ///< World metres of the model origin.
    };

    /// @brief A mechanism of a template ([mech-1]): spawned after the objects, linked by index.
    struct TemplateMechanism {
        std::shared_ptr<voxel::VoxelModel> model;
        glm::ivec3 spawnVoxel{0};             ///< World voxel of the model origin at p = 0.
        uint8_t quarterTurns = 0;             ///< Spawn rotation about +y, 0-3.
        scene::MechanismDesc desc;
        int16_t initialPos = 0;
        int32_t link = -1;                    ///< A lever's target: an index into RegionTemplate::mechanisms.
    };

    /**
     * @brief The set-up of a region, split by who runs what (docs/design/networking.md stage 3):
     * `ops` change the terrain and run on the server only (operations 1..k of the session's
     * stream; offline the game applies them itself); `lights` are sent as `EntitySpawn` kind
     * Light; `objects` are server entities (replicated from stage 6); `spawn` is where players
     * start (sent in `Welcome`).
     */
    struct RegionTemplate {
        std::vector<WorldOp> ops;
        std::vector<TemplateLight> lights;
        std::vector<TemplateObject> objects;
        std::vector<TemplateMechanism> mechanisms;   ///< Offline and server entities, made after the objects ([mech-1]).
        glm::dvec3 spawn{0.0};
        glm::ivec3 anchor{0};                  ///< The template's own reference voxel (the plaza centre).
        /// The template found a place for itself (the playground: a level site above the sea inside
        /// its area). A server does not start without one.
        bool sited = true;
    };

    /// @brief Where the playground may put its plaza (world metres, x / z): the plaza (est. 8 m
    /// around the site) must lie inside [min, max). Unbounded by default (the offline game).
    struct PlaygroundArea {
        glm::dvec2 min{-1.0e30};
        glm::dvec2 max{1.0e30};
    };
    /// Half size of the playground plaza (metres; authored as 80 voxels of 0.1 m).
    inline constexpr double kPlaygroundHalfMetres = 8.0;

    /**
     * @brief The Shredborne plaza (authored in voxels of 0.1 m, kept at its metre size): the
     * most level spot near the origin, a paved plaza on a stone foundation, brick walls, the watch
     * tower, a timber frame, crystals, a lava pit, gold and iron blocks, six lights, six crates.
     * @details Reads the world (ray casts to pick the site) and changes nothing: the operations
     * are returned. `anchor` = plaza centre at floor level (world voxels). With `loadSite` (an
     * offline game whose window follows the camera) the window is first centred on the site and
     * everything near it loaded (World::LoadBlocking, caves included); a session server's window
     * is fixed and loaded, so it passes false. Only sites whose plaza lies inside `area` are
     * candidates (a server passes its play bounds); `sited` is false when there was none (the
     * plaza is then built at the origin, y = 0).
     */
    RegionTemplate PlaygroundTemplate(voxel::World& world, const voxel::WorldMaterials& m, bool loadSite, const PlaygroundArea& area = {});
    /// @brief The template of `region` for a simulation whose window is loaded.
    RegionTemplate BuildRegionTemplate(Simulation& simulation, const RegionDesc& region);
    /// @brief The crate of the playground: 0.8 m of planks, hollow, iron edges (`scale` = 0.1 m / voxel size).
    std::shared_ptr<voxel::VoxelModel> CrateModel(const voxel::WorldMaterials& m, double scale);
    /// @brief 0.1 m / voxelSize, exactly 1 at 0.1 m: the factor the playground's voxels are scaled by.
    double PlaygroundScale(float voxelSize);

    /// @brief Offline: applies a template's operations in order (ApplyOp); returns the voxels changed.
    /// An operation that could not apply completely is logged.
    uint32_t ApplyTemplateOps(Simulation& simulation, const RegionTemplate& t);
    /// @brief Creates the template's lights (scene lights) in order.
    std::vector<scene::Entity> CreateTemplateLights(Simulation& simulation, const RegionTemplate& t);
    /// @brief Creates the template's objects (dynamic bodies) in order.
    std::vector<scene::Entity> CreateTemplateObjects(Simulation& simulation, const RegionTemplate& t);

    /// @brief The yard ([mech-1]): a hut with a door, and a wall with a portcullis and a lever, north of the playground's plaza.
    RegionTemplate MechanismYardTemplate(voxel::World& world, const voxel::WorldMaterials& m, const glm::ivec3& plazaAnchor);
    /// @brief A small castle west of the playground's plaza (the offline game): curtain walls with a brick weak spot, a
    /// gatehouse with a portcullis and its lever, a keep and a sally port with doors, a frame for a log trap, crates and
    /// barrels to carry. Mechanisms: 0 keep door, 1 sally door, 2 portcullis, 3 lever (runs 2).
    RegionTemplate PlaygroundCastleTemplate(voxel::World& world, const voxel::WorldMaterials& m, const glm::ivec3& plazaAnchor);
    /**
     * @brief Hollowkeep's greybox on the baked map's site (land.md "Hollowkeep's greybox"): boxes on the natural slope.
     * The curtain and inner walls follow the site's contour polygons, each run's wall walk set by its highest ground;
     * towers, the gatehouse (squared to the gate's direction, with the portcullis and its lever) and the keep on its pad.
     * Every structure reaches 1.5 m below its own ground: nothing raises the terrain. Not sited without a site.
     */
    RegionTemplate HollowkeepTemplate(const voxel::LandMap& map, const voxel::LandGeneratorSettings& land);

    /// @brief Creates a template's mechanisms in order and links them; empty (logged) if the authoring rule fails.
    std::vector<scene::Entity> CreateTemplateMechanisms(Simulation& simulation, const RegionTemplate& t);
    /// @brief The authoring rule: every lever's use box is at least kUseReach + 0.5 m from its target's outer face.
    bool CheckMechanismAuthoring(const RegionTemplate& t, float voxelSize, std::string* why = nullptr);
}
