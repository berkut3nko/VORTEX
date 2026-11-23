module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

export module vortex.engine;

export import vortex.core;
export import vortex.math;
export import vortex.voxel;
export import vortex.fx;
export import vortex.physics;
export import vortex.graphics;
export import vortex.scene;
export import vortex.sim;

export namespace vortex {

    struct EngineConfig {
        std::string title = "VORTEX";
        uint32_t width = 1280;
        uint32_t height = 720;
        bool headless = false;              ///< No window: render offscreen (automation, screenshots).
        bool validation = false;            ///< Vulkan validation layer.
        uint32_t exitAfterFrames = 0;       ///< 0 = run until closed.
        std::string screenshotPath;         ///< Written on the last frame when exitAfterFrames > 0.
        uint32_t chunkCapacity = 8192;
        uint32_t maxParticles = 32768;
        /// Worker threads of the world's job system and of Jolt (0 = the engine's default: hardware - 1 each).
        /// perf.md T1b measures them; a session takes them only when non-zero.
        unsigned jobThreads = 0;
        unsigned physicsThreads = 0;
        float fixedTimeStep = 1.0f / 60.0f;
        /// Metres of terrain collision kept around the camera. Moving bodies get the colliders
        /// they need wherever they are (physics::TerrainColliders), so this is only what the
        /// player and things at rest near the camera stand on.
        float colliderRadius = 12.0f;
        /// A dynamic body that was falling and is this many metres below the bottom of the world
        /// window has fallen out of the world and is destroyed (scene::Scene::DestroyBelow).
        /// Bodies are frozen at the window's edge, so this catches what was stopped there in
        /// flight and then left behind by the window. Whatever rests below - static objects,
        /// sleeping ones - is never touched: flying up does not delete the village below.
        float fallOutDepth = 64.0f;
        /// The terrain (sim::SimulationConfig::world). With a `saveDirectory` edits are kept:
        /// loaded from there, saved on shutdown and by the editor's "Save world". Leave it empty
        /// for a networked session, whose world must be memory only.
        voxel::WorldConfig world;
        /// Scene tuning: piece and rubble thresholds (see scene::SceneConfig). `voxelSize` is
        /// overwritten with the world's.
        scene::SceneConfig scene;
        graphics::RenderSettings render;
        /// A network session (docs/design/networking.md stage 5): the whole simulation configuration
        /// comes from the session (net::Client::MakeSimulationConfig: the region's world, tick,
        /// gravity, character layers, a store capped to what the GPU buffer may hold) and the
        /// fields above that it covers (chunkCapacity, maxParticles, fixedTimeStep, colliderRadius,
        /// fallOutDepth, world, scene) are ignored. Unset = an offline game built from them.
        std::optional<sim::SimulationConfig> simulation;
        /// The world window follows the camera (offline). A session's window is the region's and
        /// never moves: false.
        bool windowFollowsCamera = true;

        /// @brief Applies --headless --validation --frames N --screenshot F --width W --height H --scale S
        /// --ao-distance M --no-shadows --no-ao --no-emissive --no-taa --no-checkerboard --debug-view NAME
        /// (none albedo normal depth lighting steps motion material; anything else is a warning and ignored)
        /// --no-vsync (= --present immediate) --present fifo|relaxed|immediate (wins over --no-vsync)
        /// --job-threads N --physics-threads N (0 = default; a negative value is a warning and ignored).
        void ApplyCommandLine(const CommandLine& args);
    };

    // --- render data of a scene --------------------------------------------------------------------
    //  The scene module knows nothing of the renderer (it is part of the GPU-free Vortex::Sim);
    //  these two functions read its registry and produce what graphics::FrameInput takes.

    /// @brief Appends one InstanceDesc per baked chunk (ModelPart) of every visible voxel
    /// object: localToWorld / prevLocalToWorld = current / previous transform x part offset,
    /// plus the part's slot, tight bounds and the model's palette. A model that is not baked
    /// (SceneConfig::bakeModels off, or a full chunk store) has no parts and gives nothing.
    void GatherInstances(const scene::Scene& scene, std::vector<graphics::InstanceDesc>& out);

    /// @brief Appends the point lights of all entities with Transform + PointLight and a
    /// positive intensity, in world space.
    void GatherLights(const scene::Scene& scene, std::vector<graphics::PointLight>& out);

    class Engine;

    /// @brief What a game implements.
    class Application {
    public:
        virtual ~Application() = default;
        virtual void OnInit(Engine&) {}
        virtual void OnFixedUpdate(Engine&, float /*dt*/) {}
        virtual void OnUpdate(Engine&, float /*dt*/) {}
        virtual void OnGui(Engine&) {}
        virtual void OnShutdown(Engine&) {}
    };

    /**
     * @brief Owns every subsystem and runs the frame: input, fixed-step simulation, world
     * streaming, scene, particles, rendering.
     * @details Everything that is simulated - terrain, physics, terrain colliders, the scene,
     * the tick counter - lives in one sim::Simulation (Simulation()); the accessors below
     * (World(), Physics(), Scene() ...) are shortcuts into it. The engine adds input, the
     * window, the renderer and the loop that decides how many ticks a frame simulates.
     * Everything that is drawn or simulated besides the terrain is an entity of Scene(): voxel
     * objects, lights, explosion flashes. The engine drives the scene's frame protocol (see
     * scene::Scene) and renders what it gathers.
     */
    class Engine {
    public:
        explicit Engine(const EngineConfig& config);
        ~Engine();
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        /// @param generator World generator; nullptr = built-in terrain with the default materials.
        bool Initialize(std::shared_ptr<voxel::IWorldGenerator> generator = nullptr);
        /// @brief Initialize failed because --validation was asked for and the Khronos validation
        /// layer is not installed (graphics::DecideValidation): the game exits 2, as for validation errors.
        bool ValidationLayerMissing() const;
        int Run(Application& app);
        void RequestExit();

        const EngineConfig& Config() const;
        /// @brief The simulated half of the game (world, physics, scene, tick). The engine steps
        /// it from Run(): do not call its Step / StepPhysics yourself while the engine runs.
        sim::Simulation& Simulation();
        /// @brief Number of fixed steps simulated so far (sim::Simulation::Tick). Inside
        /// Application::OnFixedUpdate it is the number of the tick being simulated.
        uint32_t Tick() const;
        JobSystem& Jobs();
        Input& GetInput();
        voxel::MaterialLibrary& Materials();
        const voxel::WorldMaterials& WorldMaterialIds() const;
        voxel::ChunkStore& Chunks();
        voxel::World& World();
        physics::PhysicsWorld& Physics();
        fx::ParticleSystem& Particles();
        graphics::Renderer& Renderer();
        graphics::Window* Window();          ///< nullptr when headless.

        // --- game interface (docs/design/game-ui.md) ----------------------------------------------
        /// @brief The HUD of this frame: cleared and sized to the output by the engine at the
        /// start of every frame, filled by the game (OnUpdate), drawn after tone mapping and
        /// before ImGui. Works headless (it is part of the screenshot).
        HudCanvas& Hud();
        /// @brief The cursor in HUD (framebuffer) pixels. Meaningless while the cursor is captured.
        glm::vec2 HudCursor() const;
        /// @brief The window has the keyboard focus (false when headless).
        bool WindowFocused() const;
        /// @brief Whether ImGui receives the mouse and may hide input from the game
        /// (graphics::UiLayer::SetInputEnabled). Default true; play mode turns it off so that a
        /// locked cursor never hovers an ImGui window. Takes effect from the next frame.
        void SetImGuiInput(bool enabled);
        /// @brief The play bounds the renderer draws (the curtain of [wp-5]); std::nullopt or an unbounded
        /// area: none. A session sets sim::PlayBoundsOf(region) once it has joined.
        void SetPlayBounds(const std::optional<sim::PlayArea>& bounds);
        /// @brief The entities of the game: voxel objects, lights, lifetimes (see scene::Scene).
        scene::Scene& Scene();

        /// @brief Applies a change of the player's settings: only the fields that differ from `previous` (render
        /// scale, present preference from vsync, camera field of view), so a command-line flag (`--scale`,
        /// `--no-vsync`, `--present`) stays in effect until the player changes that same setting. Sensitivity is the
        /// game's (it scales its own mouse look).
        void ApplySettings(const GameSettings& settings, const GameSettings& previous);

        /// @brief What the session HUD shows: CPU frame (profiler average over its history, ms) and the GPU
        /// passes' last measured sum (ms, upload excluded).
        struct LivePerf {
            float cpuFrameMs = 0.0f;
            float gpuMs = 0.0f;
        };
        LivePerf LivePerformance() const;

        graphics::Camera camera;
        graphics::SunLight sun;
        graphics::SkySettings sky;
        graphics::FogSettings fog;

        /// @brief True once terrain collision exists around `position` (wait for it before simulating a character there).
        bool TerrainReady(const glm::dvec3& position) const;
        /// @brief The same for a box: every chunk the box touches (grown by one voxel) is ready. Use
        /// this with the bounds of a character - the ground it stands on is often in the chunk
        /// below the one that contains its feet (physics::TerrainColliders::IsReady).
        bool TerrainReady(const math::AABB& bounds) const;
        /// @brief Terrain collider streaming and the number of bodies frozen for lack of terrain.
        physics::TerrainColliderStats TerrainStats() const;
        /// @brief Fraction (0..1) of a fixed step elapsed since the last simulation step, for interpolation.
        float StepAlpha() const;
        /**
         * @brief Hands the decision how many fixed ticks a frame runs to `source` (a network client's
         * clock, net::Client::TicksDue: it runs ahead of the server, jumps, bleeds and holds;
         * networking.md "the tick-rate scale"). It gets the frame's wall-clock seconds (also in a
         * headless run, whose own frames use a fixed time step; at most 0.25 s a frame) and
         * `maxTicks` (kMaxSourceTicksPerFrame), and returns the ticks to run, at most `maxTicks`:
         * every tick it returns is run, so the source keeps what is over for the next frames (the
         * client clock does). Without a source the engine keeps its own accumulator (at most 4 ticks
         * per frame, the rest dropped). Set it before Run.
         */
        void SetTickSource(std::function<uint32_t(double seconds, uint32_t maxTicks)> source);
        /// @brief The most ticks one frame runs for a tick source.
        static constexpr uint32_t kMaxSourceTicksPerFrame = 8;

        /**
         * @brief An offline blast: `PresentBlast(sim::Blast(Simulation(), {center, radius, power}))`.
         * @details sim::Blast carves the terrain (quantised: centre and radius to 1/16 voxel, power
         * to 1/16 hardness), detaches floating terrain as Detach operations (crumbs under
         * scene::MinPieceVoxels(world voxel size) as one ClearMask), damages every object in range
         * (scene::Scene::Damage) and pushes bodies away; PresentBlast then shows it. Metres,
         * metres, material hardness.
         */
        void Explode(const glm::dvec3& center, float radius, float power);

        /// @brief The presentation half of a blast in this engine's simulation: sim::PresentBlast
        /// (debris particles, rubble for Detach operations and object fragments, particles for
        /// the crumbs of a ClearMask, the explosion and a flash). Simulates nothing else.
        void PresentBlast(const sim::BlastResult& blast);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
