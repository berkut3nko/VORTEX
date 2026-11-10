module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module vortex.engine;

namespace vortex {

    void EngineConfig::ApplyCommandLine(const CommandLine& args) {
        if (args.Has("--headless")) headless = true;
        if (args.Has("--validation")) validation = true;
        exitAfterFrames = static_cast<uint32_t>(args.GetInt("--frames", static_cast<int>(exitAfterFrames)));
        screenshotPath = args.GetString("--screenshot", screenshotPath);
        width = static_cast<uint32_t>(args.GetInt("--width", static_cast<int>(width)));
        height = static_cast<uint32_t>(args.GetInt("--height", static_cast<int>(height)));
        render.renderScale = args.GetFloat("--scale", render.renderScale);
        render.ambientDistance = args.GetFloat("--ao-distance", render.ambientDistance);
        if (args.Has("--no-shadows")) render.sunShadows = false;
        if (args.Has("--no-ao")) render.ambientOcclusion = false;
        if (args.Has("--no-emissive")) render.emissiveLighting = false;
        if (args.Has("--no-taa")) render.taa = false;
        if (args.Has("--no-checkerboard")) render.checkerboardLighting = false;
    }

    struct Engine::Impl {
        explicit Impl(const EngineConfig& c)
            : config(c), store(c.chunkCapacity), particles(c.maxParticles),
              scene(store, materials, physics, particles, SceneConfigFor(c)) {}

        static scene::SceneConfig SceneConfigFor(const EngineConfig& c) {
            scene::SceneConfig config = c.scene;
            config.voxelSize = c.world.voxelSize;
            return config;
        }

        EngineConfig config;
        JobSystem jobs;
        Input input;
        voxel::MaterialLibrary materials;
        voxel::WorldMaterials worldMaterials;
        voxel::ChunkStore store;
        std::unique_ptr<voxel::World> world;
        physics::PhysicsWorld physics;
        std::unique_ptr<physics::TerrainColliders> terrain;
        fx::ParticleSystem particles;
        // After everything it references: entities own bodies and model chunk slots.
        scene::Scene scene;

        graphics::Window window;
        graphics::Device device;
        graphics::Renderer renderer;
        graphics::UiLayer ui;

        std::vector<graphics::InstanceDesc> instances;
        std::vector<graphics::PointLight> lights;
        std::vector<fx::GpuParticle> gpuParticles;
        bool exitRequested = false;
        float stepAlpha = 0.0f;
        bool initialized = false;
    };

    Engine::Engine(const EngineConfig& config) : m_Impl(std::make_unique<Impl>(config)) {}

    Engine::~Engine() {
        Impl& s = *m_Impl;
        if (s.device.IsInitialized()) s.device.WaitIdle();
        s.scene.Clear();            // releases bodies and model chunk slots before physics and the store go away
        s.terrain.reset();
        s.world.reset();
        s.ui.Shutdown();
        s.renderer.Shutdown();
        s.device.Shutdown();
        s.window.Shutdown();
        s.physics.Shutdown();
    }

    bool Engine::Initialize(std::shared_ptr<voxel::IWorldGenerator> generator) {
        Impl& s = *m_Impl;
        Log::Init();
        s.worldMaterials = voxel::RegisterDefaultWorldMaterials(s.materials);
        if (!generator) {
            voxel::TerrainSettings terrain;
            terrain.voxelSize = s.config.world.voxelSize;
            terrain.materials = s.worldMaterials;
            generator = voxel::CreateTerrainGenerator(terrain);
        }
        s.world = std::make_unique<voxel::World>(s.config.world, s.store, s.jobs, std::move(generator), &s.materials);

        if (!s.physics.Initialize()) return false;
        s.terrain = std::make_unique<physics::TerrainColliders>(s.physics, *s.world, s.jobs, s.materials);

        graphics::Window* window = nullptr;
        if (!s.config.headless) {
            graphics::WindowConfig wc;
            wc.title = s.config.title;
            wc.width = s.config.width;
            wc.height = s.config.height;
            if (!s.window.Initialize(wc, &s.input)) return false;
            window = &s.window;
        }
        graphics::DeviceConfig dc;
        dc.appName = s.config.title;
        dc.validation = s.config.validation;
        if (!s.device.Initialize(dc, window)) return false;
        if (!s.renderer.Initialize(s.device, window, s.config.width, s.config.height, s.store.Capacity(), s.config.render)) return false;
        if (!s.ui.Initialize(s.device, window, s.renderer.BackbufferFormat(), s.renderer.BackbufferImageCount())) return false;
        s.renderer.SetUiLayer(&s.ui);
        s.initialized = true;
        return true;
    }

    void Engine::Explode(const glm::dvec3& center, float radius, float power) {
        Impl& s = *m_Impl;
        voxel::World& world = *s.world;
        const float vs = world.VoxelSize();
        const glm::vec3 centerVoxel = glm::vec3(center / static_cast<double>(vs));
        const float radiusVoxels = radius / vs;

        std::vector<voxel::VoxelEdit> removed;
        world.CarveSphere(centerVoxel, radiusVoxels, power, &removed);

        // Debris for a bounded sample of the removed voxels, in their material's colour.
        const size_t stride = std::max<size_t>(1, removed.size() / 400);
        for (size_t i = 0; i < removed.size(); i += stride) {
            const glm::dvec3 p = world.VoxelCenter(removed[i].voxel);
            const glm::vec3 away = glm::normalize(glm::vec3(p - center) + glm::vec3(0.0f, 0.2f, 0.0f)) * 3.0f;
            fx::SpawnVoxelBreak(s.particles, p, s.materials.Get(voxel::kWorldPalette, removed[i].oldId).color, vs, away);
        }
        fx::SpawnExplosion(s.particles, center, radius);

        // Terrain that lost its support becomes falling objects.
        const glm::ivec3 margin(static_cast<int>(radiusVoxels) + 24);
        const glm::ivec3 c = glm::ivec3(glm::floor(centerVoxel));
        for (voxel::FloatingIsland& island :
             voxel::ExtractFloatingIslands(world, &s.materials, c - margin, c + margin, 60000)) {
            // Rubble: crumbs become particles right here, small lumps are cleaned up by the scene.
            auto model = std::make_shared<voxel::VoxelModel>(std::move(island.volume), voxel::kWorldPalette, "debris");
            s.scene.CreateVoxelObject({.model = std::move(model), .position = glm::vec3(world.VoxelToWorld(island.origin)), .debris = true});
        }

        // Objects caught in the blast lose voxels and may fall apart. The list is a snapshot:
        // the pieces created here are not damaged a second time.
        std::vector<scene::Entity> caught;
        s.scene.QueryBodies(math::AABB(glm::vec3(center) - glm::vec3(radius), glm::vec3(center) + glm::vec3(radius)), caught);
        for (const scene::Entity entity : caught) s.scene.Damage(entity, center, radius);

        // Push nearby bodies outwards (the new pieces included).
        for (const auto [entity, body] : s.scene.Registry().view<const scene::RigidBody>().each()) {
            if (!s.physics.IsValid(body.body)) continue;
            const physics::BodyState state = s.physics.GetState(body.body);
            const glm::vec3 d = state.position - glm::vec3(center);
            const float dist = glm::length(d);
            if (dist > radius * 3.0f) continue;
            const float falloff = 1.0f - dist / (radius * 3.0f);
            s.physics.AddImpulse(body.body, (dist > 1e-3f ? d / dist : glm::vec3(0, 1, 0)) * (s.physics.GetMass(body.body) * 6.0f * falloff));
        }
        math::AABB wake(glm::vec3(center) - glm::vec3(radius * 2.0f), glm::vec3(center) + glm::vec3(radius * 2.0f));
        s.physics.ActivateInBox(wake);

        // A flash of light that fades out quickly and removes itself once it is too dim to see.
        constexpr float kFlashDecay = 9.0f, kFlashCutoff = 0.05f;
        scene::LightDesc flash;
        flash.position = center;
        flash.color = glm::vec3(1.0f, 0.7f, 0.35f);
        flash.intensity = 40.0f * radius;
        flash.radius = radius * 8.0f;
        flash.decay = kFlashDecay;
        flash.lifetime = std::log(flash.intensity / kFlashCutoff) / kFlashDecay;
        if (flash.lifetime > 0.0f) s.scene.CreateLight(flash);
    }

    int Engine::Run(Application& app) {
        Impl& s = *m_Impl;
        if (!s.initialized) return 1;
        app.OnInit(*this);

        const float fixedDt = s.config.fixedTimeStep;
        double last = NowSeconds();
        double accumulator = 0.0;
        uint32_t frame = 0;

        while (!s.exitRequested) {
            ProfileScope frameScope("CPU: frame");
            if (!s.config.headless) {
                s.window.PollEvents();
                if (s.window.ShouldClose()) break;
            } else {
                s.input.NewFrame();
            }

            const double now = NowSeconds();
            // Headless runs use a fixed frame time so they are reproducible.
            float dt = s.config.headless ? fixedDt : static_cast<float>(std::min(now - last, 0.1));
            last = now;

            const glm::uvec2 size = s.renderer.OutputSize();
            s.ui.BeginFrame(size, dt);
            s.ui.UpdateInputCapture(s.input);

            // --- streaming -------------------------------------------------------------------
            {
                ProfileScope scope("CPU: world");
                s.world->Update(camera.position);
                // The camera keeps the ground around the player. Bodies ask for their own
                // colliders and are frozen while they have none (TerrainColliders::GuardBodies).
                const glm::dvec3 focus[] = {camera.position};
                s.terrain->Update(focus, s.config.colliderRadius);
            }

            // --- fixed-step simulation -------------------------------------------------------
            {
                ProfileScope scope("CPU: simulation");
                accumulator += dt;
                int steps = 0;
                s.scene.BeginFrame();       // what was drawn last frame becomes "previous"
                while (accumulator >= fixedDt && steps < 4) {
                    app.OnFixedUpdate(*this, fixedDt);
                    // Whatever the game or the last frame's update created or moved since the
                    // last guard is checked before it is simulated.
                    s.terrain->GuardBodies();
                    s.physics.Step(fixedDt);
                    s.scene.SyncFromPhysics();
                    accumulator -= fixedDt;
                    ++steps;
                }
                if (steps == 4) accumulator = 0.0;   // do not spiral when the frame is too slow
                s.stepAlpha = static_cast<float>(accumulator / fixedDt);
            }

            app.OnUpdate(*this, dt);
            s.particles.Update(dt, [&world = *s.world](const glm::dvec3& p) { return world.IsSolidAt(p); });
            app.OnGui(*this);
            s.ui.EndFrame();

            // --- render ----------------------------------------------------------------------
            s.instances.clear();
            s.scene.GatherInstances(s.instances);
            s.lights.clear();
            s.scene.GatherLights(s.lights);
            s.particles.BuildRenderData(s.world->WindowOrigin(), camera.position, s.gpuParticles);

            graphics::FrameInput in;
            in.camera = camera;
            in.sun = sun;
            in.sky = sky;
            in.fog = fog;
            in.world = s.world.get();
            in.store = &s.store;
            in.materials = &s.materials;
            in.instances = s.instances;
            in.lights = s.lights;
            in.particles = s.gpuParticles;
            in.time = now;
            in.deltaTime = dt;

            ++frame;
            const bool lastFrame = s.config.exitAfterFrames != 0 && frame >= s.config.exitAfterFrames;
            if (lastFrame && !s.config.screenshotPath.empty()) s.renderer.RequestScreenshot(s.config.screenshotPath);
            s.renderer.RenderFrame(in);

            // Lifetimes and light fades advance after the frame was drawn, so a flash is seen
            // at full intensity on its first frame.
            // Rubble clean-up runs there too; then whatever lies far below the loaded world goes.
            s.scene.SetViewer(camera.position);
            s.scene.Update(dt);
            s.scene.DestroyBelow(static_cast<float>(s.world->WindowOrigin().y) - s.config.fallOutDepth);

            Profiler::EndFrame();
            if (lastFrame) break;
        }

        app.OnShutdown(*this);
        if (s.config.exitAfterFrames != 0) {
            const voxel::WorldStats ws = s.world->Stats();
            Log::Info("World: {} dense, {} uniform, {} empty chunks; store {}/{}", ws.denseChunks, ws.uniformChunks, ws.emptyChunks, s.store.UsedCount(), s.store.Capacity());
            const physics::TerrainColliderStats ts = s.terrain->Stats();
            Log::Info("Physics: {} bodies ({} awake, {} frozen), {} terrain colliders; scene: {} voxel objects, {} rubble",
                      s.physics.BodyCount(), s.physics.ActiveBodyCount(), ts.frozenBodies, ts.colliders, s.scene.VoxelObjectCount(),
                      s.scene.DebrisCount());
            // Last frame, plus the median of the profiler's history: a single frame is a noisy
            // sample (clock scaling of an integrated GPU), the median is what to compare runs by.
            const std::vector<Profiler::Timer> timers = Profiler::SnapshotTimers();
            for (const graphics::PassTiming& pass : s.renderer.Stats().passes) {
                float median = pass.milliseconds;
                int samples = 0;
                const std::string timerName = std::format("GPU: {}", pass.name);
                for (const Profiler::Timer& timer : timers) {
                    if (timer.name != timerName || timer.samples <= 0) continue;
                    samples = std::min(timer.samples, Profiler::kHistory);
                    std::vector<float> sorted(static_cast<size_t>(samples));
                    for (int i = 0; i < samples; ++i) {
                        sorted[static_cast<size_t>(i)] =
                            timer.history[static_cast<size_t>((timer.cursor - 1 - i + 2 * Profiler::kHistory) % Profiler::kHistory)];
                    }
                    std::sort(sorted.begin(), sorted.end());
                    median = sorted[sorted.size() / 2];
                }
                Log::Info("GPU {:<10} {:.2f} ms (median {:.2f} ms over {} frames)", pass.name, pass.milliseconds, median, samples);
            }
        }
        const uint32_t errors = s.device.ValidationErrorCount();
        if (errors > 0) Log::Error("{} Vulkan validation error(s) were reported during the run", errors);
        return errors > 0 ? 2 : 0;
    }

    bool Engine::TerrainReady(const glm::dvec3& position) const { return m_Impl->terrain && m_Impl->terrain->IsReady(position); }
    bool Engine::TerrainReady(const math::AABB& bounds) const { return m_Impl->terrain && m_Impl->terrain->IsReady(bounds); }
    physics::TerrainColliderStats Engine::TerrainStats() const { return m_Impl->terrain ? m_Impl->terrain->Stats() : physics::TerrainColliderStats{}; }
    float Engine::StepAlpha() const { return m_Impl->stepAlpha; }
    void Engine::RequestExit() { m_Impl->exitRequested = true; }
    const EngineConfig& Engine::Config() const { return m_Impl->config; }
    JobSystem& Engine::Jobs() { return m_Impl->jobs; }
    Input& Engine::GetInput() { return m_Impl->input; }
    voxel::MaterialLibrary& Engine::Materials() { return m_Impl->materials; }
    const voxel::WorldMaterials& Engine::WorldMaterialIds() const { return m_Impl->worldMaterials; }
    voxel::ChunkStore& Engine::Chunks() { return m_Impl->store; }
    voxel::World& Engine::World() { return *m_Impl->world; }
    physics::PhysicsWorld& Engine::Physics() { return m_Impl->physics; }
    fx::ParticleSystem& Engine::Particles() { return m_Impl->particles; }
    graphics::Renderer& Engine::Renderer() { return m_Impl->renderer; }
    graphics::Window* Engine::Window() { return m_Impl->config.headless ? nullptr : &m_Impl->window; }
    scene::Scene& Engine::Scene() { return m_Impl->scene; }
}
