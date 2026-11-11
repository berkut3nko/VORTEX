module;

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>

export module vortex.sim:simulation;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.scene;

export namespace vortex::sim {

    /// @brief What a Simulation is built from. The defaults are those of an offline game.
    struct SimulationConfig {
        uint32_t chunkCapacity = 8192;        ///< Slots of the chunk store (terrain + baked models).
        /// Size of the particle pool. 0 = no particles at all: every effect the scene asks for is
        /// dropped without a word (a session server).
        uint32_t maxParticles = 32768;
        float fixedTimeStep = 1.0f / 60.0f;   ///< Seconds per tick.
        /// Metres of terrain collision kept around every focus point given to Step /
        /// UpdateStreaming. Moving bodies get the colliders they need wherever they are.
        float colliderRadius = 12.0f;
        /// A dynamic body that was falling and is this many metres below the bottom of the world
        /// window is destroyed (scene::Scene::DestroyBelow).
        float fallOutDepth = 64.0f;
        unsigned jobThreads = 0;              ///< Worker threads of the job system (0 = one per core, minus one).
        /// The terrain. With a `saveDirectory` the world loads modified chunks from there and
        /// saves them on shutdown and on World().Save() (an offline game, the map editor). A
        /// session world must be memory only (a region is template + seed and resets every
        /// session; a world that loaded saved chunks would not be the world the other side
        /// generated): ServerConfig clears it, and a network client must leave it empty.
        voxel::WorldConfig world;
        scene::SceneConfig scene;             ///< `voxelSize` is overwritten with the world's.
        physics::PhysicsConfig physics;
    };

    /**
     * @brief Turns a configuration into the one a session server runs: nothing is drawn there
     * and nobody looks at it.
     * @details A memory-only world (`world.saveDirectory` cleared), no particles
     * (`maxParticles = 0`), models are not baked into the chunk store
     * (`scene.bakeModels = false`), and the scene never marks anything as rubble
     * (`scene.debrisRemnants = false`, `scene.markDebris = false`): which entities exist on a
     * server must not depend on a viewer, an age or a cap that clients do not share. Everything
     * else is kept.
     */
    SimulationConfig ServerConfig(SimulationConfig config = {});

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
