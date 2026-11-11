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
        explicit Impl(const EngineConfig& c) : config(c), sim(SimulationConfigFor(c)) {}

        /// The simulation of an offline game: the engine's own settings, everything else default.
        static sim::SimulationConfig SimulationConfigFor(const EngineConfig& c) {
            sim::SimulationConfig config;
            config.chunkCapacity = c.chunkCapacity;
            config.maxParticles = c.maxParticles;
            config.fixedTimeStep = c.fixedTimeStep;
            config.colliderRadius = c.colliderRadius;
            config.fallOutDepth = c.fallOutDepth;
            config.world = c.world;
            config.scene = c.scene;
            return config;
        }

        EngineConfig config;
        Input input;
        // World, physics, terrain colliders, scene and the tick counter. Declared before the
        // renderer, which draws from its chunk store.
        sim::Simulation sim;

        graphics::Window window;
        graphics::Device device;
        graphics::Renderer renderer;
        graphics::UiLayer ui;
        HudCanvas hud;
        bool hudOverflowReported = false;

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
        s.sim.Shutdown();           // entities (bodies, model chunk slots), terrain colliders, world, physics
        s.ui.Shutdown();
        s.renderer.Shutdown();
        s.device.Shutdown();
        s.window.Shutdown();
    }

    bool Engine::Initialize(std::shared_ptr<voxel::IWorldGenerator> generator) {
        Impl& s = *m_Impl;
        Log::Init();
        if (!s.sim.Initialize(std::move(generator))) return false;

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
        if (!s.renderer.Initialize(s.device, window, s.config.width, s.config.height, s.sim.Chunks().Capacity(), s.config.render)) return false;
        if (!s.ui.Initialize(s.device, window, s.renderer.BackbufferFormat(), s.renderer.BackbufferImageCount())) return false;
        s.renderer.SetUiLayer(&s.ui);
        s.initialized = true;
        return true;
    }

    void Engine::Explode(const glm::dvec3& center, float radius, float power) {
        Impl& s = *m_Impl;
        voxel::World& world = s.sim.World();
        voxel::MaterialLibrary& materials = s.sim.Materials();
        physics::PhysicsWorld& physics = s.sim.Physics();
        fx::ParticleSystem& particles = s.sim.Particles();
        scene::Scene& scene = s.sim.Scene();
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
            fx::SpawnVoxelBreak(particles, p, materials.Get(voxel::kWorldPalette, removed[i].oldId).color, vs, away);
        }
        fx::SpawnExplosion(particles, center, radius);

        // Terrain that lost its support becomes falling objects.
        const glm::ivec3 margin(static_cast<int>(radiusVoxels) + 24);
        const glm::ivec3 c = glm::ivec3(glm::floor(centerVoxel));
        for (voxel::FloatingIsland& island :
             voxel::ExtractFloatingIslands(world, &materials, c - margin, c + margin, 60000)) {
            // Rubble: crumbs become particles right here, small lumps are cleaned up by the scene.
            auto model = std::make_shared<voxel::VoxelModel>(std::move(island.volume), voxel::kWorldPalette, "debris");
            scene.CreateVoxelObject({.model = std::move(model), .position = glm::vec3(world.VoxelToWorld(island.origin)), .debris = true});
        }

        // Objects caught in the blast lose voxels and may fall apart. The list is a snapshot:
        // the pieces created here are not damaged a second time.
        std::vector<scene::Entity> caught;
        scene.QueryBodies(math::AABB(glm::vec3(center) - glm::vec3(radius), glm::vec3(center) + glm::vec3(radius)), caught);
        for (const scene::Entity entity : caught) scene.Damage(entity, center, radius);

        // Push nearby bodies outwards (the new pieces included).
        for (const auto [entity, body] : scene.Registry().view<const scene::RigidBody>().each()) {
            if (!physics.IsValid(body.body)) continue;
            const physics::BodyState state = physics.GetState(body.body);
            const glm::vec3 d = state.position - glm::vec3(center);
            const float dist = glm::length(d);
            if (dist > radius * 3.0f) continue;
            const float falloff = 1.0f - dist / (radius * 3.0f);
            physics.AddImpulse(body.body, (dist > 1e-3f ? d / dist : glm::vec3(0, 1, 0)) * (physics.GetMass(body.body) * 6.0f * falloff));
        }
        math::AABB wake(glm::vec3(center) - glm::vec3(radius * 2.0f), glm::vec3(center) + glm::vec3(radius * 2.0f));
        physics.ActivateInBox(wake);

        // A flash of light that fades out quickly and removes itself once it is too dim to see.
        constexpr float kFlashDecay = 9.0f, kFlashCutoff = 0.05f;
        scene::LightDesc flash;
        flash.position = center;
        flash.color = glm::vec3(1.0f, 0.7f, 0.35f);
        flash.intensity = 40.0f * radius;
        flash.radius = radius * 8.0f;
        flash.decay = kFlashDecay;
        flash.lifetime = std::log(flash.intensity / kFlashCutoff) / kFlashDecay;
        if (flash.lifetime > 0.0f) scene.CreateLight(flash);
    }

    int Engine::Run(Application& app) {
        Impl& s = *m_Impl;
        if (!s.initialized) return 1;
        app.OnInit(*this);

        sim::Simulation& sim = s.sim;
        const float fixedDt = sim.FixedTimeStep();
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
            s.hud.Begin(glm::vec2(size));
            s.ui.BeginFrame(size, dt);
            s.ui.UpdateInputCapture(s.input);

            // --- streaming -------------------------------------------------------------------
            {
                ProfileScope scope("CPU: world");
                // The world window follows the camera, and the camera keeps the ground around
                // the player. Bodies ask for their own colliders and are frozen while they have
                // none (TerrainColliders::GuardBodies).
                const glm::dvec3 focus[] = {camera.position};
                sim.UpdateStreaming(camera.position, focus);
            }

            // --- fixed-step simulation -------------------------------------------------------
            {
                ProfileScope scope("CPU: simulation");
                accumulator += dt;
                int steps = 0;
                sim.Scene().BeginFrame();   // what was drawn last frame becomes "previous"
                while (accumulator >= fixedDt && steps < 4) {
                    app.OnFixedUpdate(*this, fixedDt);
                    // Guard, physics step, pose sync; the tick is counted. Whatever the game or
                    // the last frame's update created or moved since the last guard is checked
                    // before it is simulated.
                    sim.StepPhysics();
                    accumulator -= fixedDt;
                    ++steps;
                }
                if (steps == 4) accumulator = 0.0;   // do not spiral when the frame is too slow
                s.stepAlpha = static_cast<float>(accumulator / fixedDt);
            }

            app.OnUpdate(*this, dt);
            sim.Particles().Update(dt, [&world = sim.World()](const glm::dvec3& p) { return world.IsSolidAt(p); });
            app.OnGui(*this);
            s.ui.EndFrame();

            // --- render ----------------------------------------------------------------------
            s.instances.clear();
            GatherInstances(sim.Scene(), s.instances);
            s.lights.clear();
            GatherLights(sim.Scene(), s.lights);
            sim.Particles().BuildRenderData(sim.World().WindowOrigin(), camera.position, s.gpuParticles);

            graphics::FrameInput in;
            in.camera = camera;
            in.sun = sun;
            in.sky = sky;
            in.fog = fog;
            in.world = &sim.World();
            in.store = &sim.Chunks();
            in.materials = &sim.Materials();
            in.instances = s.instances;
            in.lights = s.lights;
            in.particles = s.gpuParticles;
            in.hud = s.hud.Quads();
            in.hudDim = s.hud.BackgroundDim();
            if (s.hud.Overflowed() && !s.hudOverflowReported) {
                s.hudOverflowReported = true;
                Log::Warn("HUD: more than {} quads in one frame; the rest were dropped", HudCanvas::kMaxQuads);
            }
            in.time = now;
            in.deltaTime = dt;

            ++frame;
            const bool lastFrame = s.config.exitAfterFrames != 0 && frame >= s.config.exitAfterFrames;
            if (lastFrame && !s.config.screenshotPath.empty()) s.renderer.RequestScreenshot(s.config.screenshotPath);
            s.renderer.RenderFrame(in);

            // Lifetimes and light fades advance after the frame was drawn, so a flash is seen
            // at full intensity on its first frame.
            // Rubble clean-up runs there too; then whatever lies far below the loaded world goes.
            sim.Scene().SetViewer(camera.position);
            sim.UpdateLifetimes(dt);

            Profiler::EndFrame();
            if (lastFrame) break;
        }

        app.OnShutdown(*this);
        if (s.config.exitAfterFrames != 0) {
            const voxel::WorldStats ws = sim.World().Stats();
            Log::Info("World: {} dense, {} uniform, {} empty chunks; store {}/{}", ws.denseChunks, ws.uniformChunks, ws.emptyChunks,
                      sim.Chunks().UsedCount(), sim.Chunks().Capacity());
            const physics::TerrainColliderStats ts = sim.Terrain().Stats();
            Log::Info("Physics: {} bodies ({} awake, {} frozen), {} terrain colliders; scene: {} voxel objects, {} rubble",
                      sim.Physics().BodyCount(), sim.Physics().ActiveBodyCount(), ts.frozenBodies, ts.colliders,
                      sim.Scene().VoxelObjectCount(), sim.Scene().DebrisCount());
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
            Log::Info("HUD: {} quads drawn on the last frame", s.renderer.Stats().hudQuads);
        }
        const uint32_t errors = s.device.ValidationErrorCount();
        if (errors > 0) Log::Error("{} Vulkan validation error(s) were reported during the run", errors);
        return errors > 0 ? 2 : 0;
    }

    bool Engine::TerrainReady(const glm::dvec3& position) const {
        return m_Impl->sim.IsInitialized() && m_Impl->sim.Terrain().IsReady(position);
    }
    bool Engine::TerrainReady(const math::AABB& bounds) const {
        return m_Impl->sim.IsInitialized() && m_Impl->sim.Terrain().IsReady(bounds);
    }
    physics::TerrainColliderStats Engine::TerrainStats() const {
        return m_Impl->sim.IsInitialized() ? m_Impl->sim.Terrain().Stats() : physics::TerrainColliderStats{};
    }
    float Engine::StepAlpha() const { return m_Impl->stepAlpha; }
    void Engine::RequestExit() { m_Impl->exitRequested = true; }
    const EngineConfig& Engine::Config() const { return m_Impl->config; }
    sim::Simulation& Engine::Simulation() { return m_Impl->sim; }
    uint32_t Engine::Tick() const { return m_Impl->sim.Tick(); }
    JobSystem& Engine::Jobs() { return m_Impl->sim.Jobs(); }
    Input& Engine::GetInput() { return m_Impl->input; }
    voxel::MaterialLibrary& Engine::Materials() { return m_Impl->sim.Materials(); }
    const voxel::WorldMaterials& Engine::WorldMaterialIds() const { return m_Impl->sim.WorldMaterialIds(); }
    voxel::ChunkStore& Engine::Chunks() { return m_Impl->sim.Chunks(); }
    voxel::World& Engine::World() { return m_Impl->sim.World(); }
    physics::PhysicsWorld& Engine::Physics() { return m_Impl->sim.Physics(); }
    fx::ParticleSystem& Engine::Particles() { return m_Impl->sim.Particles(); }
    graphics::Renderer& Engine::Renderer() { return m_Impl->renderer; }
    graphics::Window* Engine::Window() { return m_Impl->config.headless ? nullptr : &m_Impl->window; }
    HudCanvas& Engine::Hud() { return m_Impl->hud; }

    glm::vec2 Engine::HudCursor() const {
        const Impl& s = *m_Impl;
        const glm::vec2 cursor(static_cast<float>(s.input.MouseX()), static_cast<float>(s.input.MouseY()));
        if (s.config.headless) return cursor;
        // Input reports screen coordinates; the HUD is laid out in framebuffer pixels.
        const glm::ivec2 window = s.window.WindowSize();
        const glm::ivec2 framebuffer = s.window.FramebufferSize();
        if (window.x <= 0 || window.y <= 0) return cursor;
        return cursor * glm::vec2(framebuffer) / glm::vec2(window);
    }

    bool Engine::WindowFocused() const { return !m_Impl->config.headless && m_Impl->window.IsFocused(); }
    void Engine::SetImGuiInput(bool enabled) { m_Impl->ui.SetInputEnabled(enabled); }
    scene::Scene& Engine::Scene() { return m_Impl->sim.Scene(); }
}
