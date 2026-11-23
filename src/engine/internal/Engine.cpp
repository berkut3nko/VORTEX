module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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
        // perf.md P1: --no-vsync is immediate (else mailbox, else FIFO), for measuring; --present wins over it.
        if (args.Has("--no-vsync")) render.present = graphics::PresentPreference::Immediate;
        if (const std::optional<std::string> name = args.Get("--present")) {
            if (!graphics::ParsePresentPreference(*name, render.present))
                Log::Warn("--present expects fifo, relaxed or immediate: '{}' ignored", *name);
        }
        // perf.md T1b: thread counts for measuring (0 = the default, hardware - 1 each).
        for (auto [flag, field] : {std::pair{"--job-threads", &jobThreads}, std::pair{"--physics-threads", &physicsThreads}}) {
            if (!args.Has(flag)) continue;
            const int n = args.GetInt(flag, 0);
            if (n < 0) Log::Warn("{} expects a count (0 = default): {} ignored", flag, n);
            else *field = static_cast<unsigned>(n);
        }
        if (const std::optional<std::string> name = args.Get("--debug-view")) {
            // In graphics::DebugView order.
            constexpr std::string_view kNames[] = {"none", "albedo", "normal", "depth", "lighting", "steps", "motion", "material"};
            const auto* found = std::find(std::begin(kNames), std::end(kNames), std::string_view(*name));
            if (found != std::end(kNames)) render.debugView = static_cast<graphics::DebugView>(found - std::begin(kNames));
            else Log::Warn("--debug-view expects none, albedo, normal, depth, lighting, steps, motion or material: '{}' ignored", *name);
        }
    }

    namespace {
        /// The GPU's actual clock file (Intel: gt_act_freq_mhz) of the first /sys/class/drm/card<N>, sorted by path;
        /// empty if there is none. Connectors (card0-eDP-1) are skipped.
        std::filesystem::path FindGpuClockFile() {
            std::error_code ec;
            std::vector<std::filesystem::path> cards;
            for (std::filesystem::directory_iterator it("/sys/class/drm", ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (name.size() <= 4 || name.compare(0, 4, "card") != 0) continue;
                if (!std::all_of(name.begin() + 4, name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
                cards.push_back(it->path());
            }
            std::sort(cards.begin(), cards.end());
            for (const std::filesystem::path& card : cards) {
                const std::filesystem::path file = card / "gt_act_freq_mhz";
                if (std::filesystem::exists(file, ec) && !ec) return file;
            }
            return {};
        }

        /// One integer from a sysfs file; nullopt if it cannot be read.
        std::optional<long> ReadInteger(const std::filesystem::path& file) {
            std::ifstream in(file);
            long value = 0;
            if (!(in >> value)) return std::nullopt;
            return value;
        }
    }

    struct Engine::Impl {
        explicit Impl(const EngineConfig& c) : config(c), sim(SimulationConfigFor(c)) {}

        /// The simulation of an offline game: the engine's own settings, everything else default.
        static sim::SimulationConfig SimulationConfigFor(const EngineConfig& c) {
            if (c.simulation) {
                // A session's: the world and steps are the server's; thread counts only when asked for.
                sim::SimulationConfig config = *c.simulation;
                if (c.jobThreads != 0) config.jobThreads = c.jobThreads;
                if (c.physicsThreads != 0) config.physics.threads = c.physicsThreads;
                return config;
            }
            sim::SimulationConfig config;
            config.jobThreads = c.jobThreads;
            config.physics.threads = c.physicsThreads;
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
        glm::dvec4 playBounds{0.0};   // [wp-5]: (min x, min z, max x, max z) in world metres; zero = none

        std::vector<graphics::InstanceDesc> instances;
        std::vector<graphics::PointLight> lights;
        std::vector<fx::GpuParticle> gpuParticles;
        bool exitRequested = false;
        float stepAlpha = 0.0f;
        std::function<uint32_t(double, uint32_t)> tickSource;
        bool tickSourceOverReported = false;
        bool initialized = false;
        // Run-wide statistics for the `Perf:` exit line (perf.md M0).
        PerfSummary perf;
        uint64_t lastTimingSerial = 0;
        // The GPU clock, read every 60 frames (perf.md M0: an integrated GPU's clock explains its timings).
        std::filesystem::path gpuClockFile;
        bool gpuClockChecked = false;
        RunStats gpuClock;
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
        Log::Info("Jobs: {} worker thread(s)", s.sim.Jobs().ThreadCount());   // beside Jolt's "Physics initialized" line

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
        PresentBlast(sim::Blast(m_Impl->sim, sim::BlastDesc{center, radius, power}));
    }

    void Engine::PresentOutcome(const sim::ActionOutcome& outcome) {
        for (const sim::BlastResult& blast : outcome.blasts) PresentBlast(blast);
    }

    void Engine::PresentBlast(const sim::BlastResult& blast) {
        sim::Simulation& sim = m_Impl->sim;
        const uint32_t slotsBefore = sim.Chunks().UsedCount();
        const scene::RubbleStats rubbleBefore = sim.Scene().Rubble();
        const uint64_t crumbledBefore = rubbleBefore.crumbledByCap + rubbleBefore.crumbledByBake;
        const auto start = std::chrono::steady_clock::now();
        sim::PresentBlast(sim, blast);
        // One line per blast (docs/design/shred.md stage 0): where the main-thread time went and
        // how much the renderer has to upload (an edited chunk goes up whole; a rubble model
        // takes new slots of the same size). Its own cost counts as presentation. Edits come
        // chunk by chunk (World::EditRegion), so runs are collapsed before the short list is sorted.
        std::vector<glm::ivec3> chunks;
        for (const sim::AppliedOp& applied : blast.ops)
            for (const voxel::VoxelEdit& edit : applied.edits) {
                const glm::ivec3 chunk = voxel::VoxelToChunk(edit.voxel);
                if (chunks.empty() || chunks.back() != chunk) chunks.push_back(chunk);
            }
        std::sort(chunks.begin(), chunks.end(), [](const glm::ivec3& a, const glm::ivec3& b) {
            return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
        });
        chunks.erase(std::unique(chunks.begin(), chunks.end()), chunks.end());
        const uint32_t slotsAfter = sim.Chunks().UsedCount();
        const scene::RubbleStats rubble = sim.Scene().Rubble();
        const size_t skipped = blast.Report().skipped.size();
        const double presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const uint32_t rubbleSlots = slotsAfter > slotsBefore ? slotsAfter - slotsBefore : 0u;
        const double uploadMb = static_cast<double>((chunks.size() + rubbleSlots) * sizeof(voxel::Chunk)) / 1.0e6;
        const sim::BlastTimes& t = blast.times;
        const sim::IslandSearchStats& q = blast.search;
        const uint32_t carved = (!blast.ops.empty() && blast.ops[0].op.kind == sim::WorldOpKind::CarveSphere) ? blast.ops[0].result.changed : 0u;
        if (blast.deferred && blast.ops.empty() && blast.deferredGivenUp == 0) return;
        // The island search: the cell graph (shred.md stage 4, the default) or the stage-1 voxel search.
        const std::string search =
            q.graph ? std::format("islands {:.2f} (graph: {} seeds, {} box component(s), {} flood(s) of {} node visits in {:.2f} ms; {} graph(s) "
                                  "built in {:.2f} ms, {} used, {} over-full cell(s); {} island node(s) re-labelled in {:.2f} ms; cache {} "
                                  "chunk(s), {:.1f} MB)",
                                  t.searchMs, q.seedNodes, q.boxComponents, q.graphFloods, q.nodeVisits, t.graphFloodMs, q.graphsBuilt,
                                  t.graphBuildMs, q.chunkGraphs, q.overfullCells, q.relabelledNodes, t.relabelMs, sim.TerrainGraphs().Size(),
                                  static_cast<double>(sim.TerrainGraphs().Stats().bytes) / 1.0e6)
                    : std::format("islands {:.2f} ({} box voxels) + flood {:.2f} ({} visits)", t.searchMs, q.boxVoxels, t.floodMs, q.floodVisits);
        const std::string attached =
            q.graph ? std::format("box face: {} merged, attached {} limit / {} face / {} known / {} budget / {} unloaded, {} over the limit", q.merged,
                                  q.attachedByLimit, q.attachedByFace, q.attachedByKnown, q.attachedByBudget, q.attachedByUnloaded, q.faceOversize)
                    : std::format("face {} candidate(s): {} proven, {} merged, attached {} limit / {} known / {} budget / {} unloaded, {} over the limit",
                                  q.faceCandidates, q.facePieces + q.faceCrumbs, q.merged, q.attachedByLimit, q.attachedByKnown, q.attachedByBudget,
                                  q.attachedByUnloaded, q.faceOversize);
        // The load check (shred.md Part 4b): its network, work and what it released.
        std::string neck;
        if (q.neckRan) {
            std::string pieces;
            for (const voxel::NeckCut& c : q.neckCuts)
                pieces += std::format(" [{} voxels: load {} > cut {} MU, {} faces{}]", c.voxels, c.load, c.cut, c.faces, c.insideCell ? ", inside a cell" : "");
            std::string waits;
            if (q.neckLoading + q.neckBeyondWindow > 0)
                waits = std::format(", {} loading / {} beyond-window chunk(s) taken as support{}", q.neckLoading, q.neckBeyondWindow,
                                    q.neckDeferred ? ", re-check queued" : "");
            neck = std::format("; neck {} nodes ({} split), {} loads, {} anchors ({} solid chunk(s)), {} arcs, {} ext visits, {} flow work, {} piece(s) + "
                               "{} crumb(s){}{}{}{}{}{}{} in {:.2f} ms + {:.2f} ms of graph builds",
                               q.neckNodes, q.neckSplit, q.neckLoads, q.neckAnchors, q.neckSolidChunks, q.neckArcs, q.neckExteriorVisits, q.neckFlowWork,
                               q.neckPieces, q.neckCrumbs, pieces, q.neckOversize ? std::format(", {} oversize", q.neckOversize) : std::string(),
                               q.neckDeferredParts ? std::format(", {} part(s) left for a later cast (cap)", q.neckDeferredParts) : std::string(),
                               q.neckSkipped ? ", SKIPPED (network limit)" : "", q.neckFlowBudget ? ", FLOW BUDGET SPENT" : "",
                               q.neckExteriorBudget ? std::format(", {} exterior flood(s) cut by the budget", q.neckExteriorBudget) : std::string(),
                               waits, t.neckMs, t.neckBuildMs);
        }
        Log::Info("Blast{}: {:.2f} ms = carve {:.2f} ({} voxels{}) + {}{} + ops {:.2f} + objects {:.2f} + present {:.2f}; {} island(s) ({} across the "
                  "island box face), {} crumb(s) of {} voxels; {}; {} oversize island(s); {} decode(s); {}{}{}{}{} edited chunk(s) + {} rubble "
                  "slot(s) = {:.2f} MB to upload; rubble {} small, {}/{} bodies, {}/{} slots{}",
                  blast.deferred ? " (late search)" : "", t.carveMs + t.searchMs + t.floodMs + t.opsMs + t.objectsMs + presentMs, t.carveMs,
                  carved, blast.carveReplayed ? ", replayed" : "", search, neck, t.opsMs, t.objectsMs, presentMs, blast.IslandCount(), q.facePieces,
                  blast.crumbs, blast.CrumbVoxels(), attached, q.oversizeIslands, q.chunksDecoded,
                  blast.islandsSearched ? "" : (q.floodDeferred ? "flood deferred (chunk not loaded); " : "search deferred (box not loaded); "),
                  skipped == 0 ? "" : std::format("{} skipped chunk(s); ", skipped),
                  blast.deferredDropped ? "an older deferred search dropped; " : "", blast.deferredGivenUp ? "given up; " : "",
                  chunks.size(), rubbleSlots, uploadMb, rubble.small, rubble.bodies, sim.Config().scene.maxRubbleBodies, rubble.slots,
                  sim.Config().scene.maxRubbleSlots,
                  rubble.crumbledByCap + rubble.crumbledByBake > crumbledBefore
                      ? std::format(", {} piece(s) crumbled by the budget", rubble.crumbledByCap + rubble.crumbledByBake - crumbledBefore)
                      : std::string());
        if (q.referenceMismatch != 0) Log::Error("Blast: the graph and the voxel island search differed (checkIslandSearch)");
    }

    int Engine::Run(Application& app) {
        Impl& s = *m_Impl;
        if (!s.initialized) return 1;
        // Generation throughput is measured from here: OnInit usually loads the start blocking.
        const double generationStart = NowSeconds();
        voxel::WorldStats generationAtFill;
        double generationSeconds = 0.0;
        app.OnInit(*this);

        sim::Simulation& sim = s.sim;
        const float fixedDt = sim.FixedTimeStep();
        double last = NowSeconds();
        double accumulator = 0.0;
        uint32_t frame = 0;
        // Streaming progress, logged once each (view-distance.md stage 1 gates).
        bool surfaceFilled = false, windowFilled = false;
        const double streamStart = NowSeconds();
        if (!s.gpuClockChecked) {
            s.gpuClockChecked = true;
            s.gpuClockFile = FindGpuClockFile();
            if (s.gpuClockFile.empty()) Log::Info("GPU clock: not available");
        }

        while (!s.exitRequested) {
            const double frameStart = NowSeconds();
            ProfileScope frameScope("CPU: frame");
            if (!s.config.headless) {
                s.window.PollEvents();
                if (s.window.ShouldClose()) break;
            } else {
                s.input.NewFrame();
            }

            const double now = NowSeconds();
            // Headless runs use a fixed frame time so they are reproducible (a tick source still gets
            // the wall clock: a network client's clock follows the server's, which is real time).
            const double wallDt = std::min(now - last, 0.25);
            float dt = s.config.headless ? fixedDt : static_cast<float>(std::min(now - last, 0.1));
            last = now;

            const glm::uvec2 size = s.renderer.OutputSize();
            s.hud.Begin(glm::vec2(size));
            s.ui.BeginFrame(size, dt);
            s.ui.UpdateInputCapture(s.input);

            // --- streaming -------------------------------------------------------------------
            {
                const double worldStart = NowSeconds();
                ProfileScope scope("CPU: world");
                // The world window follows the camera, and the camera keeps the ground around
                // the player. Bodies ask for their own colliders and are frozen while they have
                // none (TerrainColliders::GuardBodies).
                const glm::dvec3 focus[] = {camera.position};
                if (s.config.windowFollowsCamera) sim.UpdateStreaming(camera.position, focus);
                else sim.UpdateStreaming(focus);   // a session: the window is the region's
                // Island searches of earlier blasts whose box has loaded since (sim::Blast had
                // asked the world for it): what broke off falls now.
                for (const sim::BlastResult& late : sim::RetryIslandSearches(sim)) PresentBlast(late);
                const voxel::World& world = sim.World();
                if (!surfaceFilled && world.IsFilled(voxel::LoadScope::Surface)) {
                    surfaceFilled = true;
                    Log::Info("World: surface filled in {} frames ({:.1f} s)", frame, NowSeconds() - streamStart);
                }
                if (!windowFilled && world.IsFilled(voxel::LoadScope::Everything)) {
                    windowFilled = true;
                    Log::Info("World: window filled in {} frames ({:.1f} s)", frame, NowSeconds() - streamStart);
                    generationAtFill = world.Stats();
                    generationSeconds = NowSeconds() - generationStart;
                }
                s.perf.cpuWorld.Add((NowSeconds() - worldStart) * 1000.0);
            }

            // --- fixed-step simulation -------------------------------------------------------
            {
                const double simulationStart = NowSeconds();
                ProfileScope scope("CPU: simulation");
                sim.Scene().BeginFrame();   // what was drawn last frame becomes "previous"
                if (s.tickSource) {
                    // The source's clock decides (a network client); the accumulator only gives the
                    // view an interpolation fraction. The source is asked for at most what a frame
                    // runs and keeps the rest (a tick it hands out and is not run is lost to it).
                    const uint32_t due = s.tickSource(wallDt, kMaxSourceTicksPerFrame);
                    if (due > kMaxSourceTicksPerFrame && !s.tickSourceOverReported) {
                        s.tickSourceOverReported = true;
                        Log::Error("Engine: the tick source returned {} ticks, more than the {} asked for (the rest is lost)", due,
                                   kMaxSourceTicksPerFrame);
                    }
                    const uint32_t ticks = std::min(due, kMaxSourceTicksPerFrame);
                    for (uint32_t i = 0; i < ticks; ++i) {
                        app.OnFixedUpdate(*this, fixedDt);
                        sim.StepPhysics();
                    }
                    accumulator = std::clamp(accumulator + wallDt - static_cast<double>(ticks) * fixedDt, 0.0, static_cast<double>(fixedDt));
                } else {
                    accumulator += dt;
                    int steps = 0;
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
                }
                s.stepAlpha = static_cast<float>(std::min(accumulator / fixedDt, 1.0));
                s.perf.cpuSimulation.Add((NowSeconds() - simulationStart) * 1000.0);
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
            in.playBounds = s.playBounds;
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
            {
                const graphics::RenderStats& rs = s.renderer.Stats();
                s.perf.fenceWait.Add(rs.fenceWaitMs);
                // The pass times of a frame slot arrive once (two frames later): count each reading once.
                if (rs.timingSerial != s.lastTimingSerial) {
                    s.lastTimingSerial = rs.timingSerial;
                    for (const graphics::PassTiming& p : rs.passes) s.perf.GpuPass(p.name).Add(p.milliseconds);
                }
            }

            // Lifetimes and light fades advance after the frame was drawn, so a flash is seen
            // at full intensity on its first frame.
            // Rubble clean-up runs there too; then whatever lies far below the loaded world goes.
            sim.Scene().SetViewer(camera.position);
            sim.UpdateLifetimes(dt);

            if (frame % 60 == 0 && !s.gpuClockFile.empty()) {
                if (const std::optional<long> mhz = ReadInteger(s.gpuClockFile)) {
                    Log::Info("GPU clock: {} MHz (frame {})", *mhz, frame);
                    s.gpuClock.Add(static_cast<double>(*mhz));
                }
            }

            Profiler::EndFrame();
            s.perf.cpuFrame.Add((NowSeconds() - frameStart) * 1000.0);
            if (lastFrame) break;
        }

        app.OnShutdown(*this);
        if (s.config.exitAfterFrames != 0) {
            const voxel::WorldStats ws = sim.World().Stats();
            // Store-full loads / decodes: chunks drawn as a rock stand-in for want of a slot (must be 0).
            // Refused regions: save files of another voxel size or unreadable (their edits are not shown, never overwritten).
            Log::Info("World: {} dense, {} uniform, {} empty chunks; store {}/{}, {} store-full loads, {} store-full decodes; "
                      "{} refused region file(s) ({} with unwritten edits)",
                      ws.denseChunks, ws.uniformChunks, ws.emptyChunks, sim.Chunks().UsedCount(), sim.Chunks().Capacity(), ws.storeFullLoads,
                      ws.storeFullDecodes, ws.refusedRegions, ws.unwrittenRegions);
            const voxel::World& world = sim.World();
            Log::Info("World: window {} x {} x {} chunks (rows {}..{}{}), {} packed chunks in {:.1f} MiB, {} pending; "
                      "{} unloads inside the view radius",
                      world.WindowSize().x, world.WindowSize().y, world.WindowSize().z, world.WindowMinChunk().y,
                      world.WindowMinChunk().y + world.WindowSize().y, world.FullHeight() ? ", fixed" : ", follows the camera",
                      ws.packedChunks, static_cast<double>(ws.packedBytes) / (1024.0 * 1024.0), ws.pendingChunks, ws.unloadsInsideRadius);
            Log::Info("World: {} window moves, recentre {:.3f} ms last, {:.3f} ms max; {} packed decodes in {:.2f} ms ({} revealed), "
                      "{} packed again", ws.recenters, ws.lastRecenterMs, ws.maxRecenterMs, ws.decodes, ws.decodeMs, ws.reveals, ws.repacks);
            if (!windowFilled) Log::Info("World: window not filled after {} frames ({} unfilled columns)", frame, ws.unfilledColumns);
            // Chunks generated from the start of the run until the window was filled (or until now).
            const voxel::WorldStats& gen = windowFilled ? generationAtFill : ws;
            const double genSeconds = std::max(windowFilled ? generationSeconds : NowSeconds() - generationStart, 1.0e-6);
            // The wall rate is capped by the integration budget per frame (maxIntegrationsPerUpdate
            // Dense results); the rate per worker-second is the generator's own speed.
            Log::Info("World: generation {} chunks ({} dense) in {:.2f} s {}: {:.0f} chunks/s, {:.0f} dense chunks/s; "
                      "{:.0f} chunks per worker-second in the generator; voxel {:.3f} m, view radius {:.1f} m",
                      gen.generatedChunks, gen.generatedDense, genSeconds, windowFilled ? "until the window was filled" : "(window not filled)",
                      static_cast<double>(gen.generatedChunks) / genSeconds, static_cast<double>(gen.generatedDense) / genSeconds,
                      gen.generateMs > 0.0 ? static_cast<double>(gen.generatedChunks) * 1000.0 / gen.generateMs : 0.0,
                      static_cast<double>(world.VoxelSize()), world.ViewRadius());
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
        // Every run, windowed included, leaves its numbers (docs/verify.md "Game (offline, headless)", the `Perf:` line).
        Log::Info("{}", FormatPerfLine(s.perf, s.renderer.PresentModeLabel()));
        if (s.gpuClock.Count() > 0) {
            const std::optional<long> maxMhz = ReadInteger(s.gpuClockFile.parent_path() / "gt_max_freq_mhz");
            Log::Info("GPU clock: {} samples, p50 {:.0f} MHz, min {:.0f}, max {:.0f}{}", s.gpuClock.Count(), s.gpuClock.Percentile(50),
                      s.gpuClock.Percentile(0), s.gpuClock.Max(), maxMhz ? std::format(", gt_max {} MHz", *maxMhz) : std::string());
        }
        const uint32_t errors = s.device.ValidationErrorCount();
        if (errors > 0) Log::Error("{} Vulkan validation error(s) were reported during the run", errors);
        return errors > 0 ? 2 : 0;
    }

    bool Engine::ValidationLayerMissing() const { return m_Impl->device.ValidationMissing(); }

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
    void Engine::SetTickSource(std::function<uint32_t(double seconds, uint32_t maxTicks)> source) { m_Impl->tickSource = std::move(source); }
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

    void Engine::SetPlayBounds(const std::optional<sim::PlayArea>& bounds) {
        if (!bounds || !bounds->Bounded()) {
            m_Impl->playBounds = glm::dvec4(0.0);
            return;
        }
        m_Impl->playBounds = glm::dvec4(bounds->min.x, bounds->min.y, bounds->max.x, bounds->max.y);
    }
    scene::Scene& Engine::Scene() { return m_Impl->sim.Scene(); }

    void Engine::ApplySettings(const GameSettings& settings, const GameSettings& previous) {
        graphics::RenderSettings& rs = m_Impl->renderer.Settings();
        if (settings.renderScale != previous.renderScale) rs.renderScale = std::clamp(settings.renderScale, 0.25f, 1.0f);
        if (settings.vsync != previous.vsync)
            rs.present = settings.vsync ? graphics::PresentPreference::FifoRelaxed : graphics::PresentPreference::Immediate;
        if (settings.playBounds != previous.playBounds) rs.playBounds = settings.playBounds;
        if (settings.fovDegrees != previous.fovDegrees)
            camera.fovY = glm::radians(static_cast<float>(std::clamp(settings.fovDegrees, 50, 110)));
    }

    Engine::LivePerf Engine::LivePerformance() const {
        LivePerf perf;
        for (const Profiler::Timer& timer : Profiler::SnapshotTimers()) {
            if (timer.name == "CPU: frame") {
                perf.cpuFrameMs = timer.avgMs;
                break;
            }
        }
        for (const graphics::PassTiming& pass : m_Impl->renderer.Stats().passes)
            if (pass.name != "upload") perf.gpuMs += pass.milliseconds;
        return perf;
    }
}
