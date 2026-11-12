module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module vortex.scene;

namespace vortex::scene {

    struct Scene::Impl {
        Impl(voxel::ChunkStore& chunkStore, const voxel::MaterialLibrary& materialLibrary, physics::PhysicsWorld& physicsWorld,
             fx::ParticleSystem& particleSystem, const SceneConfig& sceneConfig)
            : store(chunkStore), materials(materialLibrary), physics(physicsWorld), particles(particleSystem),
              config(ResolveThresholds(sceneConfig)) {
            config.debrisParticleStride = std::max<uint32_t>(1, config.debrisParticleStride);
            // The body belongs to the component: whatever removes the component (Destroy,
            // registry.destroy / remove / clear) releases the body with it.
            registry.on_destroy<RigidBody>().connect<&Impl::OnRigidBodyDestroyed>(*this);
        }

        void OnRigidBodyDestroyed(entt::registry& source, entt::entity entity) {
            const physics::BodyId body = source.get<RigidBody>(entity).body;
            // Jolt wakes nothing when a body is removed: what rested on it (rubble, a crate on a
            // piece that expired) would stay asleep in the air. Wake its bounds, one voxel wider;
            // frozen bodies there resume awake. Not while the whole scene is cleared.
            math::AABB bounds;
            if (!clearing) bounds = physics.GetBounds(body);
            physics.DestroyBody(body);
            if (!bounds.IsEmpty()) {
                bounds.Expand(config.voxelSize + 0.05f);
                physics.ActivateInBox(bounds);
            }
        }

        /// Creates the body of a voxel object: a compound of its collider boxes at `transform`.
        /// Returns an invalid id if the model has no solid voxels or physics refused the body.
        physics::BodyId CreateBody(Entity entity, const voxel::VoxelModel& model, const math::Transform& transform,
                                   physics::MotionType motion, const glm::vec3& linear, const glm::vec3& angular) {
            const float vs = transform.scale;
            std::vector<physics::BoxDesc> boxes;
            for (const voxel::ColliderBox& b : voxel::BuildColliderBoxes(model.Volume(), false)) {
                physics::BoxDesc d;
                d.halfExtent = glm::vec3(b.size) * (0.5f * vs);
                d.center = glm::vec3(b.min) * vs + d.halfExtent;
                d.density = std::max(1.0f, materials.Get(model.Palette(), b.material).density);
                boxes.push_back(d);
            }
            physics::BodyDesc body;
            body.shape = physics.CreateCompoundShape(boxes);
            if (!body.shape) return {};
            body.position = transform.position;
            body.rotation = transform.rotation;
            body.motion = motion;
            // Small fragments do not collide with each other: piles of rubble stay cheap.
            body.layer = motion == physics::MotionType::Static ? physics::Layer::Static
                       : (model.SolidCount() < config.debrisBodyVoxels ? physics::Layer::Debris : physics::Layer::Dynamic);
            // Small or thin bodies pass through the terrain mesh within one step once they are
            // fast: a one-voxel slab falling flat is beyond the triangles after a 3 m drop, whatever
            // its voxel count. Continuous detection costs nothing while a body is slow.
            bool thin = false;
            glm::ivec3 solidMin, solidMax;
            if (model.Volume().SolidBounds(solidMin, solidMax)) {
                const glm::ivec3 extent = solidMax - solidMin;
                thin = static_cast<uint32_t>(std::min({extent.x, extent.y, extent.z})) < config.thinBodyVoxels;
            }
            body.continuous = motion == physics::MotionType::Dynamic && (body.layer == physics::Layer::Debris || thin);
            body.linearVelocity = linear;
            body.angularVelocity = angular;
            body.userData = ToUserData(entity);
            return physics.CreateBody(body);
        }

        /// True if a dynamic piece with this many voxels is "small" (Debris layer, maxDebris).
        bool IsSmall(uint32_t solidVoxels) const { return solidVoxels < config.debrisBodyVoxels; }

        /// Seconds a piece of rubble of this size lives (Debris::lifetime; 0 = no limit).
        float RubbleLifetime(uint32_t solidVoxels) const {
            const float seconds = IsSmall(solidVoxels) ? config.debrisLifetime
                                : solidVoxels < config.rubbleLargeVoxels ? config.rubbleMediumLifetime
                                                                         : config.rubbleLargeLifetime;
            return seconds > 0.0f ? seconds : 0.0f;
        }

        /// Most particle bursts a piece that crumbles because of the rubble budget leaves.
        static constexpr uint32_t kMaxCrumbleBursts = 400;

        /// A piece of rubble that does not fit the budget: particles where its voxels are, one
        /// burst per `debrisParticleStride` voxels (at most kMaxCrumbleBursts), with its velocity.
        void CrumblePiece(const voxel::VoxelModel& model, const math::Transform& transform, const glm::vec3& velocity) {
            const uint32_t bursts = std::min(kMaxCrumbleBursts, std::max<uint32_t>(1, model.SolidCount() / config.debrisParticleStride));
            Crumble(model.Volume(), model.Palette(), transform, model.SolidCount(), bursts, velocity);
        }

        /// Chunk-store slots a model holds in this scene's store.
        static uint32_t SlotsOf(const voxel::VoxelModel& model) {
            return model.IsBaked() ? static_cast<uint32_t>(model.Parts().size()) : 0u;
        }

        /// One piece of rubble as the budget sees it.
        struct RubblePiece {
            Entity entity;
            float age;
            bool small;
            uint32_t slots;
            bool resting;      // asleep, parked or without a body: removing it changes nothing in motion
        };

        /// Every piece of rubble, oldest first. `resting` is read from physics only when asked.
        void ListRubble(std::vector<RubblePiece>& out, bool withResting) const {
            out.clear();
            for (const auto [entity, debris] : registry.view<const Debris>().each()) {
                const VoxelBody* voxels = registry.try_get<VoxelBody>(entity);
                const RigidBody* body = registry.try_get<RigidBody>(entity);
                const uint32_t solid = voxels && voxels->model ? voxels->model->SolidCount() : 0u;
                bool resting = true;
                if (withResting && body && physics.IsValid(body->body)) {
                    const physics::BodyState state = physics.GetState(body->body);
                    resting = !(state.active || state.wantsToMove);
                }
                out.push_back({entity, debris.age, IsSmall(solid), voxels && voxels->model ? SlotsOf(*voxels->model) : 0u, resting});
            }
            // Oldest first; pieces of the same age in a fixed order.
            std::sort(out.begin(), out.end(), [](const RubblePiece& a, const RubblePiece& b) {
                return a.age != b.age ? a.age > b.age : static_cast<uint32_t>(a.entity) < static_cast<uint32_t>(b.entity);
            });
        }

        RubbleStats Count() const {
            RubbleStats stats = counters;
            std::vector<RubblePiece> pieces;
            ListRubble(pieces, false);
            for (const RubblePiece& p : pieces) {
                (p.small ? stats.small : stats.bodies) += 1;
                stats.slots += p.slots;
            }
            return stats;
        }

        /// Makes room for a new piece of rubble: one more body of debrisBodyVoxels or more
        /// (`body`) and `slots` more chunk-store slots. Removes the oldest resting rubble that
        /// helps, as long as that is needed. False if a cap is still exceeded (nothing that rests
        /// is left to remove): the new piece must not become an entity.
        bool MakeRoom(bool body, uint32_t slots) {
            std::vector<RubblePiece> pieces;
            ListRubble(pieces, false);
            uint32_t bodies = 0, used = 0;
            for (const RubblePiece& p : pieces) {
                if (!p.small) ++bodies;
                used += p.slots;
            }
            const bool fits = !(body && bodies + 1 > config.maxRubbleBodies) && used + slots <= config.maxRubbleSlots;
            if (fits) return true;
            ListRubble(pieces, true);              // only now: which of them rest
            for (const RubblePiece& p : pieces) {
                const bool needBody = body && bodies + 1 > config.maxRubbleBodies;
                const bool needSlots = used + slots > config.maxRubbleSlots;
                if (!needBody && !needSlots) break;
                if (!p.resting || !((needBody && !p.small) || (needSlots && p.slots > 0))) continue;
                if (!p.small) --bodies;
                used -= p.slots;
                DespawnDebris(p.entity);
                ++counters.removedByCap;
            }
            return !(body && bodies + 1 > config.maxRubbleBodies) && used + slots <= config.maxRubbleSlots;
        }

        /// Particle bursts at up to `bursts` evenly chosen voxels of a volume placed at `transform`.
        void Crumble(const voxel::VoxelVolume& volume, voxel::PaletteId palette, const math::Transform& transform, uint32_t solid,
                     uint32_t bursts, const glm::vec3& velocity) {
            if (bursts == 0 || solid == 0) return;
            const uint32_t stride = std::max<uint32_t>(1, solid / bursts);
            const glm::ivec3 size = volume.Size();
            uint32_t seen = 0, spawned = 0;
            for (int z = 0; z < size.z && spawned < bursts; ++z)
                for (int y = 0; y < size.y && spawned < bursts; ++y)
                    for (int x = 0; x < size.x && spawned < bursts; ++x) {
                        const uint8_t id = volume.Get(x, y, z);
                        if (id == 0 || (seen++ % stride) != stride / 2) continue;
                        const glm::vec3 world = transform.TransformPoint(glm::vec3(x, y, z) + glm::vec3(0.5f));
                        fx::SpawnVoxelBreak(particles, glm::dvec3(world), materials.Get(palette, id).color, transform.scale, velocity);
                        ++spawned;
                    }
        }

        /// Removes a piece of rubble, leaving a puff of particles where it was.
        void DespawnDebris(Entity entity) {
            const VoxelBody* voxels = registry.try_get<VoxelBody>(entity);
            const Transform* transform = registry.try_get<Transform>(entity);
            if (voxels && voxels->model && voxels->visible && transform) {
                Crumble(voxels->model->Volume(), voxels->model->Palette(), transform->current, voxels->model->SolidCount(),
                        config.debrisDespawnBursts, glm::vec3(0.0f));
            }
            registry.destroy(entity);
        }

        void UpdateDebris(float dt) {
            std::vector<Entity> expired;
            const bool farRule = hasViewer && config.debrisFarDistance > 0.0f;
            for (const auto [entity, debris] : registry.view<Debris>().each()) {
                debris.age += dt;
                const float lifetime = debris.lifetime < 0.0f ? config.debrisLifetime : debris.lifetime;
                bool expire = lifetime > 0.0f && debris.age >= lifetime;
                // Small rubble only: a felled tower or a wall slab lives its lifetime wherever the
                // viewer is (it would visibly vanish 25 m away).
                const VoxelBody* voxels = registry.try_get<VoxelBody>(entity);
                const bool small = !voxels || !voxels->model || IsSmall(voxels->model->SolidCount());
                if (!expire && farRule && small && debris.age >= config.debrisFarLifetime) {
                    const Transform* transform = registry.try_get<Transform>(entity);
                    const RigidBody* body = registry.try_get<RigidBody>(entity);
                    // Frozen bodies are not active either: small rubble parked without terrain (it
                    // would hang there until the viewer comes back) goes too.
                    const bool resting = !body || !physics.GetState(body->body).active;
                    expire = resting && transform &&
                             glm::distance(glm::dvec3(transform->current.position), viewer) > static_cast<double>(config.debrisFarDistance);
                }
                if (expire) expired.push_back(entity);
            }
            for (const Entity entity : expired) DespawnDebris(entity);

            // Caps (only Damage splitting rubble can push a count over its cap here; new pieces
            // make room when they are created): resting rubble first, oldest first, as MakeRoom;
            // then moving rubble, oldest first.
            std::vector<RubblePiece> pieces;
            ListRubble(pieces, false);
            uint32_t small = 0, bodies = 0, slots = 0;
            for (const RubblePiece& p : pieces) {
                (p.small ? small : bodies) += 1;
                slots += p.slots;
            }
            if (small <= config.maxDebris && bodies <= config.maxRubbleBodies && slots <= config.maxRubbleSlots) return;
            ListRubble(pieces, true);              // only now: which of them rest
            for (const bool restingPass : {true, false}) {
                for (const RubblePiece& p : pieces) {
                    if (p.resting != restingPass) continue;
                    const bool over = p.small ? small > config.maxDebris : bodies > config.maxRubbleBodies;
                    if (!over && !(slots > config.maxRubbleSlots && p.slots > 0)) continue;
                    (p.small ? small : bodies) -= 1;
                    slots -= p.slots;
                    DespawnDebris(p.entity);
                    ++counters.removedByCap;
                }
            }
        }

        voxel::ChunkStore& store;
        const voxel::MaterialLibrary& materials;
        physics::PhysicsWorld& physics;
        fx::ParticleSystem& particles;
        SceneConfig config;
        RubbleStats counters;                     // removedByCap, crumbledByCap, crumbledByBake
        double buildCeiling = std::numeric_limits<double>::infinity();   // World::BuildCeiling
        entt::registry registry;
        glm::dvec3 viewer{0.0};
        bool hasViewer = false;
        bool clearing = false;                    // Scene::Clear: destroyed bodies wake nothing
    };

    Scene::Scene(voxel::ChunkStore& store, const voxel::MaterialLibrary& materials, physics::PhysicsWorld& physics,
                 fx::ParticleSystem& particles, const SceneConfig& config)
        : m_Impl(std::make_unique<Impl>(store, materials, physics, particles, config)) {}

    // The registry's own destructor fires no signals, so bodies are released here.
    Scene::~Scene() { Clear(); }

    const SceneConfig& Scene::Config() const { return m_Impl->config; }

    // --- entities --------------------------------------------------------------------------------

    Entity Scene::CreateVoxelObject(const VoxelObjectDesc& desc) {
        Impl& s = *m_Impl;
        if (!desc.model) {
            Log::Error("Scene: cannot create a voxel object without a model");
            return kNoEntity;
        }
        if (desc.withBody && !s.physics.IsInitialized()) {
            Log::Error("Scene: voxel object '{}' wants a body but the physics world is not initialised", desc.model->Name());
            return kNoEntity;
        }

        Transform transform;
        transform.current.position = desc.position;
        transform.current.rotation = desc.rotation;
        transform.current.scale = s.config.voxelSize;
        transform.previous = transform.current;

        // Rubble too small to be an object crumbles on the spot (before it costs chunk slots).
        const bool rubble = desc.debris && desc.withBody && desc.motion == physics::MotionType::Dynamic;
        if (rubble && desc.model->SolidCount() < s.config.minPieceVoxels) {
            if (desc.visible) {
                s.Crumble(desc.model->Volume(), desc.model->Palette(), transform.current, desc.model->SolidCount(),
                          std::max<uint32_t>(1, desc.model->SolidCount() / s.config.debrisParticleStride), desc.linearVelocity);
            }
            return kNoEntity;
        }

        // Above the build ceiling is outside the world: refused whole, never cut.
        if (std::isfinite(s.buildCeiling)) {
            const glm::vec3 size = glm::vec3(desc.model->Volume().Size()) * s.config.voxelSize;
            float top = -std::numeric_limits<float>::infinity();
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 p((corner & 1) ? size.x : 0.0f, (corner & 2) ? size.y : 0.0f, (corner & 4) ? size.z : 0.0f);
                top = std::max(top, (desc.rotation * p).y);
            }
            if (static_cast<double>(desc.position.y) + static_cast<double>(top) > s.buildCeiling) {
                Log::Warn("Scene: voxel object '{}' would reach {:.1f} m, above the build ceiling at {:.1f} m: refused", desc.model->Name(),
                          static_cast<double>(desc.position.y) + static_cast<double>(top), s.buildCeiling);
                return kNoEntity;
            }
        }

        // Rubble the scene cleans up (docs/design/shred.md stage 1e): a body cap first, before
        // anything is baked.
        const bool budgeted = rubble && s.config.markDebris;
        const bool smallRubble = s.IsSmall(desc.model->SolidCount());
        if (budgeted && !smallRubble && !s.MakeRoom(true, 0)) {
            if (desc.visible) s.CrumblePiece(*desc.model, transform.current, desc.linearVelocity);
            ++s.counters.crumbledByCap;
            return kNoEntity;
        }

        const bool bakedHere = s.config.bakeModels && !desc.model->IsBaked();
        if (bakedHere && !desc.model->Bake(s.store)) {
            if (rubble) {
                // The voxels already left the world (a Detach): never let the piece vanish.
                Log::Warn("Scene: rubble '{}' could not be baked (chunk store full): it crumbles", desc.model->Name());
                if (desc.visible) s.CrumblePiece(*desc.model, transform.current, desc.linearVelocity);
                ++s.counters.crumbledByBake;
                return kNoEntity;
            }
            Log::Error("Scene: voxel object '{}' could not be baked (chunk store full)", desc.model->Name());
            return kNoEntity;
        }
        if (budgeted && !s.MakeRoom(false, Impl::SlotsOf(*desc.model))) {
            if (bakedHere) desc.model->Unbake();
            if (desc.visible) s.CrumblePiece(*desc.model, transform.current, desc.linearVelocity);
            ++s.counters.crumbledByCap;
            return kNoEntity;
        }

        const Entity entity = s.registry.create();
        if (desc.withBody && desc.model->SolidCount() > 0) {
            const physics::BodyId body =
                s.CreateBody(entity, *desc.model, transform.current, desc.motion, desc.linearVelocity, desc.angularVelocity);
            if (!body.IsValid()) {
                Log::Error("Scene: could not create the body of voxel object '{}'", desc.model->Name());
                s.registry.destroy(entity);
                return kNoEntity;
            }
            s.registry.emplace<RigidBody>(entity, body, desc.motion);
        }
        s.registry.emplace<Transform>(entity, transform);
        s.registry.emplace<VoxelBody>(entity, desc.model, desc.visible);
        const std::string& name = desc.name.empty() ? desc.model->Name() : desc.name;
        if (!name.empty()) s.registry.emplace<Name>(entity, name);
        if (budgeted) s.registry.emplace<Debris>(entity, 0.0f, s.RubbleLifetime(desc.model->SolidCount()));
        return entity;
    }

    Entity Scene::CreateLight(const LightDesc& desc) {
        Impl& s = *m_Impl;
        Transform transform;
        transform.current.position = glm::vec3(desc.position);
        transform.previous = transform.current;

        PointLight light;
        light.color = desc.color;
        light.intensity = desc.intensity;
        light.radius = desc.radius;
        light.sourceRadius = desc.sourceRadius;

        const Entity entity = s.registry.create();
        s.registry.emplace<Transform>(entity, transform);
        s.registry.emplace<PointLight>(entity, light);
        if (desc.lifetime > 0.0f) s.registry.emplace<Lifetime>(entity, desc.lifetime, desc.decay);
        return entity;
    }

    void Scene::Destroy(Entity entity) {
        if (m_Impl->registry.valid(entity)) m_Impl->registry.destroy(entity);
    }

    void Scene::Clear() {
        m_Impl->clearing = true;
        m_Impl->registry.clear();
        m_Impl->clearing = false;
    }

    bool Scene::IsValid(Entity entity) const { return m_Impl->registry.valid(entity); }

    size_t Scene::EntityCount() const {
        // In the entity storage the living ids are exactly the first free_list() elements.
        const entt::registry& registry = m_Impl->registry;
        const auto* entities = registry.storage<entt::entity>();
        return entities ? static_cast<size_t>(entities->free_list()) : 0;
    }

    size_t Scene::VoxelObjectCount() const {
        const entt::registry& registry = m_Impl->registry;
        const auto* bodies = registry.storage<VoxelBody>();
        return bodies ? static_cast<size_t>(bodies->size()) : 0;
    }

    RubbleStats Scene::Rubble() const { return m_Impl->Count(); }

    size_t Scene::DebrisCount() const {
        const entt::registry& registry = m_Impl->registry;
        const auto* debris = registry.storage<Debris>();
        return debris ? static_cast<size_t>(debris->size()) : 0;
    }

    entt::registry& Scene::Registry() { return m_Impl->registry; }
    const entt::registry& Scene::Registry() const { return m_Impl->registry; }

    void Scene::SetTransform(Entity entity, const glm::vec3& position, const glm::quat& rotation, bool teleport) {
        Impl& s = *m_Impl;
        if (!s.registry.valid(entity)) return;
        Transform* transform = s.registry.try_get<Transform>(entity);
        if (!transform) return;
        transform->current.position = position;
        transform->current.rotation = rotation;
        if (teleport) transform->previous = transform->current;
        if (const RigidBody* body = s.registry.try_get<RigidBody>(entity)) s.physics.SetTransform(body->body, position, rotation);
    }

    // --- physics <-> entities --------------------------------------------------------------------

    Entity Scene::EntityFromUserData(uint64_t userData) const {
        const Entity entity = scene::EntityFromUserData(userData);
        return m_Impl->registry.valid(entity) ? entity : kNoEntity;
    }

    Entity Scene::EntityFromBody(physics::BodyId body) const {
        const Impl& s = *m_Impl;
        if (!body.IsValid() || !s.physics.IsValid(body)) return kNoEntity;
        const Entity entity = EntityFromUserData(s.physics.GetUserData(body));
        if (entity == kNoEntity) return kNoEntity;
        // A body somebody else made could carry a tagged value by accident: ask the entity too.
        const RigidBody* owned = s.registry.try_get<RigidBody>(entity);
        return owned && owned->body == body ? entity : kNoEntity;
    }

    void Scene::QueryBodies(const math::AABB& box, std::vector<Entity>& out) const {
        const Impl& s = *m_Impl;
        for (const auto [entity, body] : s.registry.view<const RigidBody>().each()) {
            const math::AABB bounds = s.physics.GetBounds(body.body);
            if (!bounds.IsEmpty() && bounds.Overlaps(box)) out.push_back(entity);
        }
    }

    // --- destruction -----------------------------------------------------------------------------

    DamageResult Scene::Damage(Entity entity, const glm::dvec3& center, float radius) {
        Impl& s = *m_Impl;
        DamageResult result;
        if (!s.registry.valid(entity) || !(radius > 0.0f)) return result;

        // Everything is copied out of the registry first: creating the piece entities below
        // invalidates component references.
        std::shared_ptr<voxel::VoxelModel> model;
        Transform source;
        physics::BodyId oldBody;
        physics::MotionType motion = physics::MotionType::Dynamic;
        bool visible = true;
        {
            const VoxelBody* voxels = s.registry.try_get<VoxelBody>(entity);
            const RigidBody* body = s.registry.try_get<RigidBody>(entity);
            const Transform* transform = s.registry.try_get<Transform>(entity);
            if (!voxels || !body || !transform || !voxels->model || !body->body.IsValid()) return result;
            model = voxels->model;
            visible = voxels->visible;
            source = *transform;
            oldBody = body->body;
            motion = body->motion;
        }
        const math::Transform& transform = source.current;
        const voxel::PaletteId palette = model->Palette();
        const float vs = transform.scale;

        // Sphere in the object's voxel space.
        const glm::vec3 local = transform.InverseTransformPoint(glm::vec3(center));
        const float r = radius / vs;
        const glm::ivec3 size = model->Size();
        const glm::ivec3 lo = glm::max(glm::ivec3(glm::floor(local - glm::vec3(r))), glm::ivec3(0));
        const glm::ivec3 hi = glm::min(glm::ivec3(glm::ceil(local + glm::vec3(r))) + glm::ivec3(1), size);
        if (lo.x >= hi.x || lo.y >= hi.y || lo.z >= hi.z) return result;

        // Models are shared between entities: work on a private copy of the volume.
        voxel::VoxelVolume volume = model->Volume();
        for (int z = lo.z; z < hi.z; ++z)
            for (int y = lo.y; y < hi.y; ++y)
                for (int x = lo.x; x < hi.x; ++x) {
                    const uint8_t id = volume.Get(x, y, z);
                    if (id == 0) continue;
                    const glm::vec3 d = glm::vec3(x, y, z) + glm::vec3(0.5f) - local;
                    if (glm::dot(d, d) > r * r) continue;
                    const voxel::Material& material = s.materials.Get(palette, id);
                    if ((material.flags & voxel::MaterialFlag_Indestructible) != 0) continue;
                    volume.Set(x, y, z, 0);
                    if ((result.removed++ % s.config.debrisParticleStride) == 0) {     // a sample is plenty of debris
                        const glm::vec3 world = transform.TransformPoint(glm::vec3(x, y, z) + glm::vec3(0.5f));
                        fx::SpawnVoxelBreak(s.particles, glm::dvec3(world), material.color, vs,
                                            glm::normalize(world - glm::vec3(center) + glm::vec3(0.0f, 1.0e-3f, 0.0f)) * 2.5f);
                    }
                }
        if (result.removed == 0) return result;

        // Motion of the original body, sampled before it goes away.
        const glm::vec3 angular = s.physics.GetState(oldBody).angularVelocity;
        auto pointVelocity = [&](const glm::vec3& worldPoint) { return s.physics.GetPointVelocity(oldBody, worldPoint); };

        voxel::SplitResult split = voxel::SplitComponents(volume, s.config.minPieceVoxels);
        result.loose = static_cast<uint32_t>(split.loose.size());
        for (const voxel::LooseVoxel& v : split.loose) {
            const glm::vec3 world = transform.TransformPoint(glm::vec3(v.position) + glm::vec3(0.5f));
            fx::SpawnVoxelBreak(s.particles, glm::dvec3(world), s.materials.Get(palette, v.material).color, vs, pointVelocity(world));
        }
        std::vector<glm::vec3> linear;
        linear.reserve(split.pieces.size());
        for (const voxel::VolumePiece& piece : split.pieces) {
            linear.push_back(pointVelocity(transform.TransformPoint(glm::vec3(piece.offset) + glm::vec3(piece.volume.Size()) * 0.5f)));
        }

        s.registry.remove<RigidBody>(entity);           // destroys the old body (on_destroy hook)
        if (split.pieces.empty()) {
            s.registry.destroy(entity);
            result.destroyed = true;
            return result;
        }

        std::string name;
        if (const Name* label = s.registry.try_get<Name>(entity)) name = label->value;
        // Pieces of rubble are rubble: new pieces, with a lifetime by their own size from now (a
        // piece of old rubble that took its age would vanish at once, in flight).
        const bool rubbleParent = s.config.markDebris && s.registry.all_of<Debris>(entity);

        // Let go of the old model before the pieces are baked: if this entity was its last user,
        // its chunk slots are free for the pieces (which never need more slots than it had).
        const std::string modelName = model->Name();
        s.registry.get<VoxelBody>(entity).model.reset();
        model.reset();

        // An object that is only dented keeps its motion type. One that is cut apart becomes
        // dynamic as a whole: the scene does not know which piece was the anchored one, and a
        // severed static part must not hang in the air.
        const bool splitApart = split.pieces.size() > 1;

        // Every piece stays where its voxels were; the largest one keeps the entity so
        // references to it stay meaningful.
        for (size_t i = 0; i < split.pieces.size(); ++i) {
            voxel::VolumePiece& piece = split.pieces[i];
            const glm::vec3 offset(piece.offset);
            Transform placed = source;
            placed.current.position = source.current.TransformPoint(offset);
            placed.previous.position = source.previous.TransformPoint(offset);

            auto pieceModel = std::make_shared<voxel::VoxelModel>(std::move(piece.volume), palette, modelName);
            const bool small = s.IsSmall(pieceModel->SolidCount());
            const bool kept = i == 0;
            const physics::MotionType pieceMotion = kept && !splitApart ? motion : physics::MotionType::Dynamic;

            // Below the replicated-piece threshold: no entity, the caller makes local rubble.
            if (s.config.replicatedPieceVoxels > 0 && piece.voxelCount < s.config.replicatedPieceVoxels &&
                pieceMotion == physics::MotionType::Dynamic) {
                result.fragments.push_back({std::move(pieceModel), placed.current, linear[i], angular});
                if (kept) {
                    s.registry.destroy(entity);
                    result.destroyed = true;
                    result.toRubble = true;
                }
                continue;
            }

            // Only possible when the old model is shared and the store is full: the piece is
            // simulated but not drawn (Bake logs it and leaves the model unbaked).
            if (s.config.bakeModels) pieceModel->Bake(s.store);

            const Entity target = kept ? entity : s.registry.create();
            const physics::BodyId body = s.CreateBody(target, *pieceModel, placed.current, pieceMotion, linear[i], angular);
            if (!body.IsValid()) {
                // Out of bodies: a piece that cannot be simulated would hang in the air, drop it.
                Log::Error("Scene: could not create the body of a piece of '{}', the piece is dropped", modelName);
                s.registry.destroy(target);
                if (kept) result.destroyed = true;
                continue;
            }
            s.registry.emplace<RigidBody>(target, body, pieceMotion);
            if (kept) {
                s.registry.get<Transform>(target) = placed;
                s.registry.get<VoxelBody>(target).model = std::move(pieceModel);
                // What is left of the object is itself no more than a lump: rubble like the rest.
                if (small && s.config.markDebris && s.config.debrisRemnants && pieceMotion == physics::MotionType::Dynamic &&
                    !s.registry.all_of<Debris>(target)) {
                    s.registry.emplace<Debris>(target);
                }
            } else {
                s.registry.emplace<Transform>(target, placed);
                s.registry.emplace<VoxelBody>(target, std::move(pieceModel), visible);
                if (!name.empty()) s.registry.emplace<Name>(target, name);
                if (rubbleParent) s.registry.emplace<Debris>(target, 0.0f, s.RubbleLifetime(piece.voxelCount));
                else if (small && s.config.markDebris) s.registry.emplace<Debris>(target);       // rubble: cleaned up by Update()
                result.pieces.push_back(target);
            }
        }
        return result;
    }

    // --- per frame -------------------------------------------------------------------------------

    void Scene::BeginFrame() {
        for (const auto [entity, transform] : m_Impl->registry.view<Transform>().each()) transform.previous = transform.current;
    }

    void Scene::SyncFromPhysics() {
        Impl& s = *m_Impl;
        for (const auto [entity, body, transform] : s.registry.view<const RigidBody, Transform>().each()) {
            if (!s.physics.IsValid(body.body)) continue;
            const physics::BodyState state = s.physics.GetState(body.body);
            transform.current.position = state.position;
            transform.current.rotation = state.rotation;
        }
    }

    void Scene::Update(float dt) {
        Impl& s = *m_Impl;
        std::vector<Entity> expired;
        for (const auto [entity, lifetime] : s.registry.view<Lifetime>().each()) {
            lifetime.remaining -= dt;
            if (lifetime.remaining <= 0.0f) {
                expired.push_back(entity);
            } else if (lifetime.lightDecay > 0.0f) {
                if (PointLight* light = s.registry.try_get<PointLight>(entity)) light->intensity *= std::exp(-lifetime.lightDecay * dt);
            }
        }
        for (const Entity entity : expired) s.registry.destroy(entity);

        s.UpdateDebris(dt);
    }

    void Scene::SetViewer(const glm::dvec3& position) {
        m_Impl->viewer = position;
        m_Impl->hasViewer = true;
    }

    void Scene::SetBuildCeiling(double height) { m_Impl->buildCeiling = height; }
    double Scene::BuildCeiling() const { return m_Impl->buildCeiling; }

    uint32_t Scene::DestroyAbove(double height) {
        Impl& s = *m_Impl;
        if (!std::isfinite(height)) return 0;
        std::vector<Entity> lost;
        for (const auto [entity, body] : s.registry.view<const RigidBody>().each()) {
            if (body.motion != physics::MotionType::Dynamic || !s.physics.IsValid(body.body)) continue;
            const physics::BodyState state = s.physics.GetState(body.body);
            if (!(state.active || state.wantsToMove)) continue;
            if (static_cast<double>(s.physics.GetBounds(body.body).min.y) > height) lost.push_back(entity);
        }
        for (const Entity entity : lost) s.registry.destroy(entity);
        if (!lost.empty()) Log::Info("Scene: removed {} object(s) thrown above the build ceiling", lost.size());
        return static_cast<uint32_t>(lost.size());
    }

    uint32_t Scene::DestroyBelow(float height) {
        Impl& s = *m_Impl;
        std::vector<Entity> lost;
        for (const auto [entity, body] : s.registry.view<const RigidBody>().each()) {
            if (!s.physics.IsValid(body.body)) continue;
            const physics::BodyState state = s.physics.GetState(body.body);
            const math::AABB bounds = s.physics.GetBounds(body.body);
            const bool finite = std::isfinite(state.position.x) && std::isfinite(state.position.y) && std::isfinite(state.position.z);
            // Written so that NaN bounds count as lost, too.
            if (!finite || !std::isfinite(bounds.max.y)) {
                lost.push_back(entity);
                continue;
            }
            // Only what is on its way down: a body that rests there - placed by the game or the
            // editor, or left asleep when the world window moved on - was not lost by anybody.
            const bool falling = body.motion == physics::MotionType::Dynamic && (state.active || state.wantsToMove);
            if (falling && bounds.max.y < height) lost.push_back(entity);
        }
        for (const Entity entity : lost) s.registry.destroy(entity);
        if (!lost.empty()) Log::Info("Scene: removed {} object(s) that fell out of the world", lost.size());
        return static_cast<uint32_t>(lost.size());
    }
}
