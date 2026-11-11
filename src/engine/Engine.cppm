module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
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

        /// @brief Applies --headless --validation --frames N --screenshot F --width W --height H --scale S
        /// --ao-distance M --no-shadows --no-ao --no-emissive --no-taa.
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
        /// @brief The entities of the game: voxel objects, lights, lifetimes (see scene::Scene).
        scene::Scene& Scene();

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

        /// @brief Blast: removes terrain and object voxels in a sphere (scene::Scene::Damage for
        /// every object in range), throws debris particles, detaches floating terrain as falling
        /// objects (rubble: islands below SceneConfig::minPieceVoxels become particles, small ones
        /// are cleaned up later), pushes nearby bodies away and leaves a short flash of light (an
        /// entity with PointLight + Lifetime).
        void Explode(const glm::dvec3& center, float radius, float power);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
