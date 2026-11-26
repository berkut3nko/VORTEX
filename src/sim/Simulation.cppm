module;

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

export module vortex.sim:simulation;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.scene;
import :worldop;

export namespace vortex::sim {

    /// @brief What a Simulation is built from. The defaults are those of an offline game.
    /// @brief Which island search a blast runs (docs/design/shred.md stage 4).
    enum class IslandSearchKind : uint8_t {
        /// The cell graph cached per terrain chunk (voxel::SearchIslandsOnGraph): the default.
        Graph,
        /// The stage-1 voxel search (voxel::ExtractFloatingIslands + voxel::ProveFaceComponents):
        /// the reference the graph is tested against.
        Voxel,
    };

    /**
     * @brief Server bodies that clients copy (docs/design/networking.md stage 6, "Entity
     * replication"). Off offline (every piece is local rubble there); sim::ServerConfig turns it on.
     * @details With `enabled`, sim::Blast makes a server body with a scene::NetId for every terrain
     * piece of at least SceneConfig::replicatedPieceVoxels (its Detach carries the id) and gives one
     * to every object piece Scene::Damage keeps as an entity, within the budgets below; a piece over
     * a budget is client rubble instead (`netId = 0` / a fragment), never lost (shred.md R7).
     */
    struct ReplicationConfig {
        bool enabled = false;
        /// Live pieces at most (placed objects such as crates are not pieces). A new piece over it
        /// evicts the oldest resting piece, else the smallest awake one (EntityDespawn{ToRubble}).
        uint32_t maxLivePieces = 256;
        /// Awake replicated bodies at most (derived from the snapshot budget, which every awake body
        /// shares: pieces, crates and other placed objects alike; review of stage 6): a new piece while
        /// this many are awake is client rubble, and a server that places objects refuses one then.
        uint32_t maxAwakePieces = 64;
        /// New server bodies per tick (pieces of any kind; shred.md Part 4 `serverBodiesPerTick`: what
        /// a client can build, about 120 per second). Further pieces in the same tick are client rubble.
        uint32_t bodiesPerTick = 2;
    };

    struct SimulationConfig {
        uint32_t chunkCapacity = 8192;        ///< Slots of the chunk store (terrain + baked models).
        /// Size of the particle pool. 0 = no particles at all: every effect the scene asks for is
        /// dropped without a word (a session server).
        uint32_t maxParticles = 8192;
        float fixedTimeStep = 1.0f / 60.0f;   ///< Seconds per tick.
        /// Metres of terrain collision kept around every focus point given to Step /
        /// UpdateStreaming. Moving bodies get the colliders they need wherever they are.
        float colliderRadius = 12.0f;
        /// A dynamic body that was falling and is this many metres below the bottom of the world
        /// window is destroyed (scene::Scene::DestroyBelow).
        float fallOutDepth = 64.0f;
        /// A session server (ServerConfig sets it): its window holds every blast's flood box by
        /// construction, so a face flood that meets a chunk that is not loaded is an error (counted
        /// in IslandSearchStats::attachedByUnloaded, logged) and the piece stays, instead of the
        /// offline answer of deferring the search until the chunk has loaded.
        bool sessionServer = false;
        /// The island search of a blast. Both give the same islands in the same order, except
        /// where a work budget runs out (they count work differently) and, when a chunk of the
        /// flood box is not loaded, in whether the face islands wait offline and why a flood is
        /// attached on a server (they flood in another order).
        IslandSearchKind islandSearch = IslandSearchKind::Graph;
        /// Tests: every island search also runs the other kind on the same voxels (before any
        /// operation is applied) and compares the islands; a difference is logged and counted
        /// (Simulation::IslandSearchChecks, IslandSearchStats::referenceMismatch).
        bool checkIslandSearch = false;
        /// The load check after a blast's graph island search (docs/design/shred.md Part 4b): a
        /// part whose weight the bonds holding it cannot carry falls (a tower on a thin neck). Off
        /// with IslandSearchKind::Voxel (the reference has none).
        bool neckRule = true;
        unsigned jobThreads = 0;              ///< Worker threads of the job system (0 = one per core, minus one).
        /// The terrain. With a `saveDirectory` the world loads modified chunks from there and
        /// saves them on shutdown and on World().Save() (an offline game, the map editor). A
        /// session world must be memory only (a region is template + seed and resets every
        /// session; a world that loaded saved chunks would not be the world the other side
        /// generated): ServerConfig clears it, and a network client must leave it empty.
        voxel::WorldConfig world;
        /// `voxelSize` is overwritten with the world's, and the debris thresholds left at
        /// scene::kFromMetres (the default) become their metre size in voxels of it
        /// (scene::ResolveThresholds; Config() reports the counts).
        scene::SceneConfig scene;
        physics::PhysicsConfig physics;
        /// Server bodies with a NetId (a session server: ServerConfig sets `enabled`).
        ReplicationConfig replication;
    };

    /// @brief What the replication of a simulation decided so far (ReplicationConfig).
    struct ReplicationStats {
        uint32_t netIds = 0;              ///< NetIds handed out (lights, objects, pieces).
        uint32_t pieces = 0;              ///< Pieces that became server bodies.
        uint32_t overTickBudget = 0;      ///< Pieces left to client rubble: bodiesPerTick reached.
        uint32_t overAwakeCap = 0;        ///< ... maxAwakePieces replicated bodies awake.
        uint32_t overByteBudget = 0;      ///< ... the server's entity byte budget spent (ReplicationState::refuseNewPieces).
        uint32_t evictedResting = 0;      ///< Pieces evicted by maxLivePieces: the oldest resting one.
        uint32_t evictedAwake = 0;        ///< ... nothing rested: the smallest awake one.
        uint32_t bodyFailures = 0;        ///< A piece's body could not be made (out of bodies): its id despawns at once.
        uint32_t maxLive = 0, maxAwake = 0;   ///< Most pieces seen live / awake when one was asked for.
    };

    /// @brief The replication bookkeeping a Simulation keeps (sim::AllocateNetId, sim::TakePieceSlot).
    struct ReplicationState {
        uint32_t nextNetId = 1;           ///< Counts up, never reused.
        uint32_t budgetTick = 0xFFFFFFFFu;
        uint32_t bodiesThisTick = 0;
        uint32_t pendingPieces = 0;       ///< Slots taken whose body is made later in the same blast.
        /// Set by the session server while its entity byte budget is spent (review of stage 6):
        /// TakePieceSlot answers OverByteBudget, so a new piece is client rubble with no model sent.
        bool refuseNewPieces = false;
        ReplicationStats stats;
    };

    /// SceneConfig::replicatedPieceVoxels of a server, in metres: object pieces smaller than this
    /// are client rubble (start value of docs/design/networking.md, owner decision 6, chosen as
    /// 512 voxels of 0.1 m; tuned in stage 6).
    inline constexpr double kServerReplicatedPieceVolume = 0.512;
    /// kServerReplicatedPieceVolume in voxels of voxel::kDefaultVoxelSize: 262 voxels of 0.125 m.
    inline constexpr uint32_t kServerReplicatedPieceVoxels =
        scene::VoxelsForVolume(kServerReplicatedPieceVolume, voxel::kDefaultVoxelSize);

    /**
     * @brief Turns a configuration into the one a session server runs: nothing is drawn there
     * and nobody looks at it.
     * @details A memory-only world (`world.saveDirectory` cleared), no particles
     * (`maxParticles = 0`), models are not baked into the chunk store
     * (`scene.bakeModels = false`), and the scene never marks anything as rubble
     * (`scene.debrisRemnants = false`, `scene.markDebris = false`): which entities exist on a
     * server must not depend on a viewer, an age or a cap that clients do not share. Object
     * pieces below `kServerReplicatedPieceVolume` get no entity (`scene.replicatedPieceVoxels`,
     * that volume in voxels of `world.voxelSize`): they are client rubble. `sessionServer` is set:
     * a blast's face flood that meets a chunk that is not loaded is an error there, not a reason
     * to wait. Characters collide with `kSessionCharacterLayers` (terrain and replicated bodies),
     * the same set a client's prediction uses, and neither push nor weigh on bodies
     * (`physics.characterMovesBodies` false: a client's copies are kinematic). Server bodies with a NetId (`replication.enabled`,
     * networking stage 6). Everything else is kept (the other debris thresholds are resolved from
     * the world's size by the Simulation).
     */
    SimulationConfig ServerConfig(SimulationConfig config = {});

    /**
     * @brief What one island search covers and what it carves first: a blast's sphere (SphereSite)
     * or a site check over one chunk (ChunkSite, no carve). sim::Blast runs it.
     */
    struct BlastSite {
        /// The carve applied before the search (a CarveSphere); not applied with BlastOptions::noCarve.
        WorldOp carve;
        glm::ivec3 islandMin{0}, islandMax{0};   ///< The island box [min, max), voxels.
        glm::ivec3 floodMin{0}, floodMax{0};     ///< The flood box [min, max), voxels; it holds the island box.
        glm::dvec3 center{0.0};                  ///< Metres: the objects, the push and the log.
        float radius = 0.0f;                     ///< Metres: the objects' and the push's reach (0: none).
    };

    /// @brief The island search of a blast that could not run because a chunk of its island box,
    /// or of its flood box that a flood reached, was not loaded (sim::Blast remembers it,
    /// sim::RetryIslandSearches runs it later).
    struct DeferredIslandSearch {
        BlastSite site;               ///< Its boxes, and the carve to replay on `carvePending`.
        /// Chunks the blast's carve skipped (not loaded) that may still load, in its visit order:
        /// the carve is replayed on each of them, and only on them, as soon as it is loaded.
        std::vector<glm::ivec3> carvePending;
        /// A face flood met `waitChunk` (not loaded), or the load check took it as support: the
        /// search runs again once it is loaded.
        bool waitsForChunk = false;
        glm::ivec3 waitChunk{0};
        uint32_t attempts = 0;        ///< RetryIslandSearches calls so far
    };

    /**
     * @brief The simulated half of a game: terrain, physics, terrain colliders and the scene, on
     * a numbered fixed tick. No renderer, no window, no input.
     * @details This is what a session server runs, what a network client predicts with and
     * what vortex::Engine owns and draws. The module links neither Vulkan nor GLFW.
     *
     * A tick is one fixed step of `SimulationConfig::fixedTimeStep` seconds. `Tick()` counts the
     * ticks simulated so far and is the number of the tick that is simulated next; nothing else
     * advances it. There are two ways to drive it:
     *  - **`Step(focusPoints)`**: one whole tick in the order a server uses.
     *  - **The three phases of `Step`** for an owner that interleaves its own work (the engine:
     *    game callbacks, a variable number of ticks per rendered frame, drawing):
     *    `UpdateStreaming` once per frame, `StepPhysics` once per tick, `UpdateLifetimes` once per
     *    frame with the frame time.
     * Game code that changes the simulation in a tick (moving a character, a blast) runs after
     * the streaming phase and before the physics phase, so that the body guard sees what it made.
     *
     * Not advanced here, because it is presentation: `Particles().Update` and
     * `Scene().BeginFrame()` (the pose drawn last frame); the owner that draws calls them.
     *
     * Order of destruction (also what `Shutdown` does): scene, terrain colliders, world, physics.
     * Main-thread only. Between a successful `Initialize` and `Shutdown` every accessor is valid;
     * outside of that `World()` and `Terrain()` must not be called (the other subsystems exist
     * but are empty), and the stepping functions do nothing.
     */
    class Simulation {
    public:
        explicit Simulation(const SimulationConfig& config = {});
        ~Simulation();
        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        /**
         * @brief Registers the default world materials, creates the world, starts physics and the
         * terrain colliders. The world window is not placed and nothing is loaded yet.
         * @param generator World generator; nullptr = built-in terrain with the default materials.
         * @return false if physics could not be started (the simulation stays uninitialised);
         * true also when it was initialised before (the generator is then ignored).
         */
        bool Initialize(std::shared_ptr<voxel::IWorldGenerator> generator = nullptr);
        /// @brief Destroys every entity, the terrain colliders, the world and all bodies. Safe to
        /// call twice; the destructor does it. `Tick()` keeps its value. A simulation is not meant
        /// to be initialised a second time: make a new one.
        void Shutdown();
        bool IsInitialized() const;

        /// @brief The configuration in use: the one given, with `scene.voxelSize` = the world's.
        const SimulationConfig& Config() const;
        float FixedTimeStep() const;

        JobSystem& Jobs();
        voxel::MaterialLibrary& Materials();
        const voxel::WorldMaterials& WorldMaterialIds() const;
        voxel::ChunkStore& Chunks();
        voxel::World& World();
        const voxel::World& World() const;
        physics::PhysicsWorld& Physics();
        physics::TerrainColliders& Terrain();
        const physics::TerrainColliders& Terrain() const;
        fx::ParticleSystem& Particles();
        scene::Scene& Scene();
        const scene::Scene& Scene() const;

        /// @brief Ticks simulated so far = the number of the tick that runs next. Starts at 0.
        uint32_t Tick() const;

        /// @brief Island searches waiting for their box to load (sim::Blast adds, at most
        /// kMaxDeferredIslandSearches; sim::RetryIslandSearches runs and removes them).
        std::vector<DeferredIslandSearch>& DeferredIslandSearches();
        /// @brief The cell graphs of the world's chunks, kept between blasts (sim::Blast's graph
        /// search builds what it needs; an edit makes the next search rebuild that chunk only).
        voxel::TerrainGraphCache& TerrainGraphs();
        /// @brief Island searches run so far (`run`, whatever the config), and with
        /// SimulationConfig::checkIslandSearch the ones compared with the other search and how
        /// many differed. A test arena checks `searches == run`.
        struct IslandSearchChecks {
            uint32_t run = 0;
            uint32_t searches = 0;
            uint32_t mismatches = 0;
            uint32_t neckReleases = 0;    ///< Parts the load check released (pieces and crumbs), all searches.
        };
        IslandSearchChecks& IslandSearchCheck();
        /// @brief NetIds and the piece budgets (SimulationConfig::replication; sim::AllocateNetId).
        ReplicationState& Replication();
        const ReplicationState& Replication() const;

        /**
         * @brief Simulates one tick: streaming, physics, lifetimes; then `Tick()` is one higher.
         * @details Exactly `UpdateStreaming(focusPoints)`, the body guard + physics step + pose
         * sync of `StepPhysics`, and `UpdateLifetimes(fixedTimeStep)`. The world window stays
         * where it is (place it once with `World().LoadBlocking`, or move it with the other
         * `UpdateStreaming`).
         * @param focusPoints World positions (metres) that need terrain collision around them:
         * the players. May be empty: awake bodies ask for their own ground.
         */
        void Step(std::span<const glm::dvec3> focusPoints);

        /// @brief Streaming phase: advances loading of the world window **without moving it** and
        /// streams terrain colliders around `focusPoints` (which also guards the bodies). A world
        /// whose window was never placed gets it with the minimum corner at chunk (0, 0, 0).
        void UpdateStreaming(std::span<const glm::dvec3> focusPoints);
        /// @brief The same, but the world window is first centred on `windowFocus` (world metres):
        /// a client whose window follows its camera.
        void UpdateStreaming(const glm::dvec3& windowFocus, std::span<const glm::dvec3> focusPoints);

        /// @brief Physics phase of one tick: guards the bodies (whatever was created or moved
        /// since the last guard is checked before it is simulated), steps physics by the fixed
        /// time step and copies body poses into the scene. Then `Tick()` is one higher.
        void StepPhysics();

        /// @brief Lifetime phase: `Scene::Update(dt)` (lifetimes, light fades, rubble) and
        /// `Scene::DestroyBelow` for what fell `fallOutDepth` below the world window.
        /// @param dt Seconds: the fixed time step on a tick, the frame time when called per frame.
        void UpdateLifetimes(float dt);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
