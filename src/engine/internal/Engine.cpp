module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
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
    }

    struct Engine::Impl {
        explicit Impl(const EngineConfig& c) : config(c), store(c.chunkCapacity), particles(c.maxParticles) {}

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

        graphics::Window window;
        graphics::Device device;
        graphics::Renderer renderer;
        graphics::UiLayer ui;

        std::vector<VoxelObject> objects;
        std::vector<graphics::InstanceDesc> instances;
        std::vector<fx::GpuParticle> gpuParticles;
        bool exitRequested = false;
        bool initialized = false;
    };

    Engine::Engine(const EngineConfig& config) : m_Impl(std::make_unique<Impl>(config)) {}

    Engine::~Engine() {
        Impl& s = *m_Impl;
        if (s.device.IsInitialized()) s.device.WaitIdle();
        for (VoxelObject& o : s.objects) s.physics.DestroyBody(o.body);
        s.objects.clear();          // releases model chunk slots before the store goes away
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

    size_t Engine::SpawnObject(std::shared_ptr<voxel::VoxelModel> model, const glm::vec3& position,
                               const glm::quat& rotation, physics::MotionType motion) {
        Impl& s = *m_Impl;
        const float vs = s.world->VoxelSize();
        if (!model->IsBaked()) model->Bake(s.store);

        std::vector<physics::BoxDesc> boxes;
        for (const voxel::ColliderBox& b : voxel::BuildColliderBoxes(model->Volume(), false)) {
            physics::BoxDesc d;
            d.halfExtent = glm::vec3(b.size) * (0.5f * vs);
            d.center = glm::vec3(b.min) * vs + d.halfExtent;
            d.density = std::max(1.0f, s.materials.Get(model->Palette(), b.material).density);
            boxes.push_back(d);
        }

        VoxelObject object;
        object.model = std::move(model);
        object.transform.position = position;
        object.transform.rotation = rotation;
        object.transform.scale = vs;
        object.previous = object.transform;

        physics::BodyDesc body;
        body.shape = s.physics.CreateCompoundShape(boxes);
        body.position = position;
        body.rotation = rotation;
        body.motion = motion;
        body.layer = motion == physics::MotionType::Static ? physics::Layer::Static : physics::Layer::Dynamic;
        body.userData = s.objects.size();
        if (body.shape) object.body = s.physics.CreateBody(body);

        s.objects.push_back(std::move(object));
        return s.objects.size() - 1;
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
            auto model = std::make_shared<voxel::VoxelModel>(std::move(island.volume), voxel::kWorldPalette, "debris");
            SpawnObject(std::move(model), glm::vec3(world.VoxelToWorld(island.origin)));
        }

        // Push nearby bodies outwards.
        for (VoxelObject& o : s.objects) {
            if (!o.alive || !o.body.IsValid()) continue;
            const physics::BodyState state = s.physics.GetState(o.body);
            const glm::vec3 d = state.position - glm::vec3(center);
            const float dist = glm::length(d);
            if (dist > radius * 3.0f) continue;
            const float falloff = 1.0f - dist / (radius * 3.0f);
            s.physics.AddImpulse(o.body, (dist > 1e-3f ? d / dist : glm::vec3(0, 1, 0)) * (s.physics.GetMass(o.body) * 6.0f * falloff));
        }
        math::AABB wake(glm::vec3(center) - glm::vec3(radius * 2.0f), glm::vec3(center) + glm::vec3(radius * 2.0f));
        s.physics.ActivateInBox(wake);

        graphics::PointLight flash;
        flash.position = center;
        flash.color = glm::vec3(1.0f, 0.7f, 0.35f);
        flash.intensity = 40.0f * radius;
        flash.radius = radius * 8.0f;
        lights.push_back(flash);
    }

    int Engine::Run(Application& app) {
        Impl& s = *m_Impl;
        if (!s.initialized) return 1;
        app.OnInit(*this);

        const float fixedDt = s.config.fixedTimeStep;
        double last = NowSeconds();
        double accumulator = 0.0;
        uint32_t frame = 0;
        const size_t persistentLights = lights.size();

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
                std::vector<glm::dvec3> focus{camera.position};
                std::vector<physics::BodyId> active;
                s.physics.GetActiveBodies(active);
                for (size_t i = 0; i < active.size() && i < 32; ++i) focus.push_back(glm::dvec3(s.physics.GetState(active[i]).position));
                s.terrain->Update(focus, s.config.colliderRadius);
            }

            // --- fixed-step simulation -------------------------------------------------------
            {
                ProfileScope scope("CPU: simulation");
                accumulator += dt;
                int steps = 0;
                while (accumulator >= fixedDt && steps < 4) {
                    for (VoxelObject& o : s.objects) o.previous = o.transform;
                    app.OnFixedUpdate(*this, fixedDt);
                    s.physics.Step(fixedDt);
                    for (VoxelObject& o : s.objects) {
                        if (!o.alive || !o.body.IsValid()) continue;
                        const physics::BodyState state = s.physics.GetState(o.body);
                        o.transform.position = state.position;
                        o.transform.rotation = state.rotation;
                    }
                    accumulator -= fixedDt;
                    ++steps;
                }
                if (steps == 4) accumulator = 0.0;   // do not spiral when the frame is too slow
            }

            app.OnUpdate(*this, dt);
            s.particles.Update(dt, [&world = *s.world](const glm::dvec3& p) { return world.IsSolidAt(p); });
            app.OnGui(*this);
            s.ui.EndFrame();

            // --- render ----------------------------------------------------------------------
            s.instances.clear();
            for (const VoxelObject& o : s.objects) {
                if (!o.alive || !o.model) continue;
                const glm::mat4 current = o.transform.ToMatrix();
                const glm::mat4 previous = o.previous.ToMatrix();
                for (const voxel::ModelPart& part : o.model->Parts()) {
                    const glm::mat4 offset = glm::translate(glm::mat4(1.0f), glm::vec3(part.offset));
                    graphics::InstanceDesc inst;
                    inst.localToWorld = current * offset;
                    inst.prevLocalToWorld = previous * offset;
                    inst.slot = part.slot;
                    inst.boundsMin = part.boundsMin;
                    inst.boundsMax = part.boundsMax;
                    inst.palette = o.model->Palette();
                    s.instances.push_back(inst);
                }
            }
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
            in.lights = lights;
            in.particles = s.gpuParticles;
            in.time = now;
            in.deltaTime = dt;

            ++frame;
            const bool lastFrame = s.config.exitAfterFrames != 0 && frame >= s.config.exitAfterFrames;
            if (lastFrame && !s.config.screenshotPath.empty()) s.renderer.RequestScreenshot(s.config.screenshotPath);
            s.renderer.RenderFrame(in);

            // Transient lights (explosion flashes) fade out quickly.
            for (size_t i = persistentLights; i < lights.size();) {
                lights[i].intensity *= std::exp(-dt * 9.0f);
                if (lights[i].intensity < 0.05f) lights.erase(lights.begin() + static_cast<std::ptrdiff_t>(i));
                else ++i;
            }

            Profiler::EndFrame();
            if (lastFrame) break;
        }

        app.OnShutdown(*this);
        if (s.config.exitAfterFrames != 0) {
            for (const graphics::PassTiming& pass : s.renderer.Stats().passes) Log::Info("GPU {:<10} {:.2f} ms", pass.name, pass.milliseconds);
        }
        const uint32_t errors = s.device.ValidationErrorCount();
        if (errors > 0) Log::Error("{} Vulkan validation error(s) were reported during the run", errors);
        return errors > 0 ? 2 : 0;
    }

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
    std::vector<VoxelObject>& Engine::Objects() { return m_Impl->objects; }
}
