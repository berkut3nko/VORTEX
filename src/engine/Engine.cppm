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
        float colliderRadius = 12.0f;       ///< Metres of terrain collision kept around the camera.
        voxel::WorldConfig world;
        graphics::RenderSettings render;

        /// @brief Applies --headless --validation --frames N --screenshot F --width W --height H --scale S
        /// --ao-distance M --no-shadows --no-ao --no-emissive --no-taa.
        void ApplyCommandLine(const CommandLine& args);
    };

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
     * @details Everything that is drawn or simulated besides the terrain is an entity of
     * Scene(): voxel objects, lights, explosion flashes. The engine drives the scene's frame
     * protocol (see scene::Scene) and renders what it gathers.
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
        /// @brief The entities of the game: voxel objects, lights, lifetimes (see scene::Scene).
        scene::Scene& Scene();

        graphics::Camera camera;
        graphics::SunLight sun;
        graphics::SkySettings sky;
        graphics::FogSettings fog;

        /// @brief True once terrain collision exists around `position` (wait for it before simulating a character there).
        bool TerrainReady(const glm::dvec3& position) const;
        /// @brief Fraction (0..1) of a fixed step elapsed since the last simulation step, for interpolation.
        float StepAlpha() const;

        /// @brief Blast: removes terrain and object voxels in a sphere (scene::Scene::Damage for
        /// every object in range), throws debris particles, detaches floating terrain as falling
        /// objects, pushes nearby bodies away and leaves a short flash of light (an entity with
        /// PointLight + Lifetime).
        void Explode(const glm::dvec3& center, float radius, float power);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
