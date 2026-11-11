module;

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    namespace {
        /// The configuration a simulation really runs with (see SimulationConfig).
        SimulationConfig Resolve(SimulationConfig config) {
            config.scene.voxelSize = config.world.voxelSize;
            return config;
        }
    }

    SimulationConfig ServerConfig(SimulationConfig config) {
        config.maxParticles = 0;
        config.world.saveDirectory.clear();
        config.scene.bakeModels = false;
        config.scene.debrisRemnants = false;
        config.scene.markDebris = false;
        return config;
    }

    struct Simulation::Impl {
        explicit Impl(const SimulationConfig& c)
            : config(Resolve(c)), jobs(c.jobThreads),
              store(c.chunkCapacity), particles(c.maxParticles), scene(store, materials, physics, particles, config.scene) {}

        /// A point whose window is the one the world has now: the middle of the chunk that
        /// World::Update centres the window on.
        glm::dvec3 HeldWindowFocus() const {
            const glm::ivec3 chunk = world->WindowMinChunk() + world->WindowSize() / 2;
            return world->VoxelCenter(voxel::ChunkToVoxel(chunk) + glm::ivec3(voxel::kChunkSize / 2));
        }

        void Stream(const glm::dvec3& windowFocus, std::span<const glm::dvec3> focusPoints) {
            world->Update(windowFocus);
            terrain->Update(focusPoints, config.colliderRadius);
        }

        /// Guard, step, sync: the physics of one tick, without counting it.
        void SimulateBodies() {
            terrain->GuardBodies();
            physics.Step(config.fixedTimeStep);
            scene.SyncFromPhysics();
        }

        void Lifetimes(float dt) {
            scene.Update(dt);
            scene.DestroyBelow(static_cast<float>(world->WindowOrigin().y) - config.fallOutDepth);
        }

        SimulationConfig config;
        JobSystem jobs;
        voxel::MaterialLibrary materials;
        voxel::WorldMaterials worldMaterials;
        voxel::ChunkStore store;
        std::unique_ptr<voxel::World> world;
        physics::PhysicsWorld physics;
        std::unique_ptr<physics::TerrainColliders> terrain;
        fx::ParticleSystem particles;
        // After everything it references: entities own bodies and model chunk slots.
        scene::Scene scene;

        uint32_t tick = 0;
        bool initialized = false;
    };

    Simulation::Simulation(const SimulationConfig& config) : m_Impl(std::make_unique<Impl>(config)) {}

    Simulation::~Simulation() { Shutdown(); }

    bool Simulation::Initialize(std::shared_ptr<voxel::IWorldGenerator> generator) {
        Impl& s = *m_Impl;
        if (s.initialized) return true;
        s.worldMaterials = voxel::RegisterDefaultWorldMaterials(s.materials);
        if (!generator) {
            voxel::TerrainSettings terrain;
            terrain.voxelSize = s.config.world.voxelSize;
            terrain.materials = s.worldMaterials;
            generator = voxel::CreateTerrainGenerator(terrain);
        }
        s.world = std::make_unique<voxel::World>(s.config.world, s.store, s.jobs, std::move(generator), &s.materials);

        if (!s.physics.Initialize(s.config.physics)) {
            s.world.reset();
            return false;
        }
        s.terrain = std::make_unique<physics::TerrainColliders>(s.physics, *s.world, s.jobs, s.materials);
        s.initialized = true;
        return true;
    }

    void Simulation::Shutdown() {
        Impl& s = *m_Impl;
        s.scene.Clear();            // releases bodies and model chunk slots before physics and the store go away
        s.terrain.reset();
        s.world.reset();
        s.physics.Shutdown();
        s.initialized = false;
    }

    bool Simulation::IsInitialized() const { return m_Impl->initialized; }
    const SimulationConfig& Simulation::Config() const { return m_Impl->config; }
    float Simulation::FixedTimeStep() const { return m_Impl->config.fixedTimeStep; }

    JobSystem& Simulation::Jobs() { return m_Impl->jobs; }
    voxel::MaterialLibrary& Simulation::Materials() { return m_Impl->materials; }
    const voxel::WorldMaterials& Simulation::WorldMaterialIds() const { return m_Impl->worldMaterials; }
    voxel::ChunkStore& Simulation::Chunks() { return m_Impl->store; }
    voxel::World& Simulation::World() { return *m_Impl->world; }
    const voxel::World& Simulation::World() const { return *m_Impl->world; }
    physics::PhysicsWorld& Simulation::Physics() { return m_Impl->physics; }
    physics::TerrainColliders& Simulation::Terrain() { return *m_Impl->terrain; }
    const physics::TerrainColliders& Simulation::Terrain() const { return *m_Impl->terrain; }
    fx::ParticleSystem& Simulation::Particles() { return m_Impl->particles; }
    scene::Scene& Simulation::Scene() { return m_Impl->scene; }
    const scene::Scene& Simulation::Scene() const { return m_Impl->scene; }

    uint32_t Simulation::Tick() const { return m_Impl->tick; }

    void Simulation::Step(std::span<const glm::dvec3> focusPoints) {
        Impl& s = *m_Impl;
        if (!s.initialized) return;
        s.Stream(s.HeldWindowFocus(), focusPoints);
        s.SimulateBodies();
        s.Lifetimes(s.config.fixedTimeStep);
        ++s.tick;
    }

    void Simulation::UpdateStreaming(std::span<const glm::dvec3> focusPoints) {
        Impl& s = *m_Impl;
        if (s.initialized) s.Stream(s.HeldWindowFocus(), focusPoints);
    }

    void Simulation::UpdateStreaming(const glm::dvec3& windowFocus, std::span<const glm::dvec3> focusPoints) {
        Impl& s = *m_Impl;
        if (s.initialized) s.Stream(windowFocus, focusPoints);
    }

    void Simulation::StepPhysics() {
        Impl& s = *m_Impl;
        if (!s.initialized) return;
        s.SimulateBodies();
        ++s.tick;
    }

    void Simulation::UpdateLifetimes(float dt) {
        Impl& s = *m_Impl;
        if (s.initialized) s.Lifetimes(dt);
    }
}
