module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

module vortex.physics;

import vortex.core;
import vortex.math;

namespace vortex::physics {

    namespace {

        // --- process-wide Jolt runtime --------------------------------------------------------
        //
        // Jolt keeps its allocator hooks, type factory and default material in globals. They are
        // set up by the first user and torn down with the last one. Users are PhysicsWorld
        // instances and every Shape (a shape may outlive the world that created it).

        std::mutex g_RuntimeMutex;
        uint32_t g_RuntimeUsers = 0;

        void JoltTrace(const char* format, ...) {
            char buffer[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            Log::Warn("Jolt: {}", static_cast<const char*>(buffer));
        }

        void AcquireRuntime() {
            std::scoped_lock lock(g_RuntimeMutex);
            if (g_RuntimeUsers++ != 0) return;
            JPH::RegisterDefaultAllocator();
            JPH::Trace = JoltTrace;
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }

        void ReleaseRuntime() {
            std::scoped_lock lock(g_RuntimeMutex);
            if (g_RuntimeUsers == 0 || --g_RuntimeUsers != 0) return;
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }

        /// Keeps the Jolt runtime alive for as long as it exists.
        class RuntimeRef {
        public:
            RuntimeRef() { AcquireRuntime(); }
            ~RuntimeRef() { ReleaseRuntime(); }
            RuntimeRef(const RuntimeRef&) = delete;
            RuntimeRef& operator=(const RuntimeRef&) = delete;
        };

        // --- conversions ------------------------------------------------------------------------

        JPH::Vec3 ToJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
        JPH::RVec3 ToJoltR(const glm::vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
        glm::vec3 ToGlm(JPH::Vec3Arg v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }
#ifdef JPH_DOUBLE_PRECISION
        glm::vec3 ToGlm(JPH::RVec3Arg v) {
            return glm::vec3(static_cast<float>(v.GetX()), static_cast<float>(v.GetY()), static_cast<float>(v.GetZ()));
        }
#endif
        glm::quat ToGlm(JPH::QuatArg q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

        bool IsFinite(float v) { return std::isfinite(v); }
        bool IsFinite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        bool IsFinite(const glm::quat& q) {
            return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
        }

        /// Jolt requires unit quaternions; anything unusable becomes the identity.
        JPH::Quat ToJolt(const glm::quat& q) {
            if (!IsFinite(q)) return JPH::Quat::sIdentity();
            const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (length < 1e-6f) return JPH::Quat::sIdentity();
            const float inv = 1.0f / length;
            return JPH::Quat(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
        }

        JPH::EMotionType ToJolt(MotionType motion) {
            switch (motion) {
                case MotionType::Static:    return JPH::EMotionType::Static;
                case MotionType::Kinematic: return JPH::EMotionType::Kinematic;
                case MotionType::Dynamic:   break;
            }
            return JPH::EMotionType::Dynamic;
        }

        // --- collision layers -------------------------------------------------------------------

        constexpr JPH::ObjectLayer kLayerStatic = static_cast<JPH::ObjectLayer>(Layer::Static);
        constexpr JPH::ObjectLayer kLayerDynamic = static_cast<JPH::ObjectLayer>(Layer::Dynamic);
        constexpr JPH::ObjectLayer kLayerDebris = static_cast<JPH::ObjectLayer>(Layer::Debris);
        constexpr JPH::ObjectLayer kLayerCharacter = static_cast<JPH::ObjectLayer>(Layer::Character);
        constexpr JPH::ObjectLayer kLayerCount = 4;

        constexpr JPH::BroadPhaseLayer kBroadPhaseNonMoving(0);
        constexpr JPH::BroadPhaseLayer kBroadPhaseMoving(1);
        constexpr JPH::uint kBroadPhaseLayerCount = 2;

        JPH::ObjectLayer ToJolt(Layer layer) {
            const auto value = static_cast<JPH::ObjectLayer>(layer);
            return value < kLayerCount ? value : kLayerDynamic;
        }

        class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
        public:
            JPH::uint GetNumBroadPhaseLayers() const override { return kBroadPhaseLayerCount; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
                return layer == kLayerStatic ? kBroadPhaseNonMoving : kBroadPhaseMoving;
            }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
                return layer == kBroadPhaseNonMoving ? "NonMoving" : "Moving";
            }
#endif
        };

        class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
        public:
            bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
                // Static objects only ever need to be tested against things that move.
                return layer != kLayerStatic || broadPhase == kBroadPhaseMoving;
            }
        };

        class LayerPairFilter final : public JPH::ObjectLayerPairFilter {
        public:
            bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
                if (a == kLayerStatic && b == kLayerStatic) return false;
                if (a == kLayerDebris && b == kLayerDebris) return false;
                return true;
            }
        };

        /// What a character controller collides with: Static, Dynamic and Debris bodies. Bodies in
        /// the Character layer (e.g. a kinematic proxy that follows the controller) never block it.
        class CharacterLayerFilter final : public JPH::ObjectLayerFilter {
        public:
            bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer != kLayerCharacter; }
        };

        class IgnoreSensorsFilter final : public JPH::BodyFilter {
        public:
            bool ShouldCollideLocked(const JPH::Body& body) const override { return !body.IsSensor(); }
        };

        // --- contact events ---------------------------------------------------------------------

        constexpr size_t kMaxQueuedContactEvents = 16384;

        /// Collects new contacts. Called from Jolt's worker threads while the bodies are locked,
        /// so it only reads the two bodies it is given.
        class ContactCollector final : public JPH::ContactListener {
        public:
            void SetMinSpeed(float speed) { m_MinSpeed = speed; }

            void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override {
                const JPH::uint pointCount = static_cast<JPH::uint>(manifold.mRelativeContactPointsOn1.size());
                JPH::RVec3 point = manifold.mBaseOffset;
                if (pointCount > 0) {
                    JPH::Vec3 sum = JPH::Vec3::sZero();
                    for (JPH::uint i = 0; i < pointCount; ++i) sum += manifold.mRelativeContactPointsOn1[i];
                    point += sum / static_cast<float>(pointCount);
                }

                // The manifold normal points from body 1 to body 2; body 1 approaching along it is positive.
                const JPH::Vec3 normal = manifold.mWorldSpaceNormal;
                const float speed = (body1.GetPointVelocity(point) - body2.GetPointVelocity(point)).Dot(normal);
                if (!(speed >= m_MinSpeed)) return;

                ContactEvent event;
                event.bodyA = BodyId{body1.GetID().GetIndexAndSequenceNumber()};
                event.bodyB = BodyId{body2.GetID().GetIndexAndSequenceNumber()};
                event.userDataA = body1.GetUserData();
                event.userDataB = body2.GetUserData();
                event.position = ToGlm(point);
                event.normal = ToGlm(normal);
                event.approachSpeed = speed;

                std::scoped_lock lock(m_Mutex);
                if (m_Pending.size() < kMaxQueuedContactEvents) m_Pending.push_back(event);
            }

            /// Moves what the last step produced into `out`: one event per body pair (the hardest
            /// impact), in a deterministic order.
            void Publish(std::vector<ContactEvent>& out) {
                std::vector<ContactEvent> pending;
                {
                    std::scoped_lock lock(m_Mutex);
                    pending.swap(m_Pending);
                }
                if (pending.empty()) return;

                std::sort(pending.begin(), pending.end(), [](const ContactEvent& a, const ContactEvent& b) {
                    if (a.bodyA.value != b.bodyA.value) return a.bodyA.value < b.bodyA.value;
                    if (a.bodyB.value != b.bodyB.value) return a.bodyB.value < b.bodyB.value;
                    return a.approachSpeed > b.approachSpeed;
                });
                const ContactEvent* previous = nullptr;
                for (const ContactEvent& event : pending) {
                    if (previous && previous->bodyA == event.bodyA && previous->bodyB == event.bodyB) continue;
                    previous = &event;
                    if (out.size() >= kMaxQueuedContactEvents) {
                        if (!m_WarnedOverflow) {
                            Log::Warn("PhysicsWorld: more than {} contact events are waiting for TakeContactEvents(); "
                                      "dropping new ones", kMaxQueuedContactEvents);
                            m_WarnedOverflow = true;
                        }
                        break;
                    }
                    out.push_back(event);
                }
            }

        private:
            std::mutex m_Mutex;
            std::vector<ContactEvent> m_Pending;
            float m_MinSpeed = 0.0f;
            bool m_WarnedOverflow = false;
        };

        // --- shapes -----------------------------------------------------------------------------

        constexpr float kMinDensity = 1.0f;        // kg / m^3: keeps the mass of every shape positive
        constexpr float kMinHalfExtent = 1e-4f;    // metres

        /// Rounding of box edges used by Jolt's convex collision. Small against a voxel, and never
        /// larger than the box allows.
        float ConvexRadiusFor(const glm::vec3& halfExtent) {
            const float smallest = std::min(halfExtent.x, std::min(halfExtent.y, halfExtent.z));
            return std::min(JPH::cDefaultConvexRadius, smallest * 0.25f);
        }

        bool IsUsableBox(const glm::vec3& center, const glm::vec3& halfExtent) {
            return IsFinite(center) && IsFinite(halfExtent) && halfExtent.x >= kMinHalfExtent && halfExtent.y >= kMinHalfExtent &&
                   halfExtent.z >= kMinHalfExtent;
        }

        float UsableDensity(float density) { return IsFinite(density) ? std::max(density, kMinDensity) : kMinDensity; }

        JPH::Ref<JPH::BoxShape> MakeBox(const glm::vec3& halfExtent, float density) {
            JPH::Ref<JPH::BoxShape> box = new JPH::BoxShape(ToJolt(halfExtent), ConvexRadiusFor(halfExtent));
            box->SetDensity(UsableDensity(density));
            return box;
        }

        // --- characters -------------------------------------------------------------------------

        constexpr uint32_t kCharacterIndexBits = 20;
        constexpr uint32_t kCharacterIndexMask = (1u << kCharacterIndexBits) - 1u;
        constexpr uint32_t kCharacterGenerationCount = 0xFFFu;   // generations 0..0xFFE: an id is never 0xFFFFFFFF

        struct CharacterSlot {
            JPH::Ref<JPH::CharacterVirtual> character;
            uint32_t generation = 0;
            float stepHeight = 0.0f;
        };
    }

    // The engine-facing shape: a counted reference to an immutable Jolt shape.
    class Shape {
    public:
        explicit Shape(JPH::RefConst<JPH::Shape> shape) : m_Shape(std::move(shape)) {}
        const JPH::Shape* Get() const { return m_Shape.GetPtr(); }

    private:
        RuntimeRef m_Runtime;                  // declared first: released after the shape itself
        JPH::RefConst<JPH::Shape> m_Shape;
    };

    namespace {
        ShapeRef WrapShape(JPH::RefConst<JPH::Shape> shape) { return std::make_shared<const Shape>(std::move(shape)); }
    }

    // Everything that lives between Initialize() and Shutdown(). Members are destroyed in reverse
    // order: characters, then the physics system (and with it every body), then its helpers.
    struct PhysicsWorld::Impl {
        struct Simulation {
            Simulation(const PhysicsConfig& config, int threads)
                : tempAllocator(32u * 1024u * 1024u),
                  jobSystem(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads) {
                system.Init(config.maxBodies, 0, config.maxBodyPairs, config.maxContactConstraints, broadPhaseLayers,
                            objectVsBroadPhase, layerPairs);
                system.SetGravity(ToJolt(config.gravity));
                contacts.SetMinSpeed(config.contactEventMinSpeed);
                system.SetContactListener(&contacts);
            }

            RuntimeRef runtime;                    // first member: Jolt is usable before anything below allocates
            BroadPhaseLayers broadPhaseLayers;
            ObjectVsBroadPhaseFilter objectVsBroadPhase;
            LayerPairFilter layerPairs;
            JPH::TempAllocatorImplWithMallocFallback tempAllocator;
            JPH::JobSystemThreadPool jobSystem;
            ContactCollector contacts;
            JPH::PhysicsSystem system;
            std::vector<CharacterSlot> characters;
            std::vector<uint32_t> freeCharacters;
        };

        PhysicsConfig config;
        std::unique_ptr<Simulation> sim;
        std::atomic<bool> initialized{false};     // read by the thread safe shape functions
        std::vector<ContactEvent> events;
        uint32_t reportedUpdateErrors = 0;
        bool warnedBodyLimit = false;

        /// The Jolt id behind an engine id, if that body still exists.
        std::optional<JPH::BodyID> Find(BodyId body) const {
            if (!sim || !body.IsValid()) return std::nullopt;
            // Bit 31 is reserved by Jolt's broad phase: no body id ever has it set.
            if ((body.value & JPH::BodyID::cBroadPhaseBit) != 0) return std::nullopt;
            const JPH::BodyID id(body.value);
            if (sim->system.GetBodyLockInterfaceNoLock().TryGetBody(id) == nullptr) return std::nullopt;
            return id;
        }

        CharacterSlot* FindCharacter(CharacterId id) const {
            if (!sim || !id.IsValid()) return nullptr;
            const uint32_t index = id.value & kCharacterIndexMask;
            if (index >= sim->characters.size()) return nullptr;
            CharacterSlot& slot = sim->characters[index];
            if (!slot.character || slot.generation != (id.value >> kCharacterIndexBits)) return nullptr;
            return &slot;
        }
    };

    PhysicsWorld::PhysicsWorld() : m_Impl(std::make_unique<Impl>()) {}

    PhysicsWorld::~PhysicsWorld() { Shutdown(); }

    bool PhysicsWorld::Initialize(const PhysicsConfig& config) {
        if (m_Impl->sim) {
            Log::Warn("PhysicsWorld::Initialize: already initialized");
            return true;
        }
        if (!IsFinite(config.gravity) || !IsFinite(config.contactEventMinSpeed)) {
            Log::Error("PhysicsWorld::Initialize: gravity and contactEventMinSpeed must be finite");
            return false;
        }

        PhysicsConfig cfg = config;
        cfg.maxBodies = std::clamp<uint32_t>(cfg.maxBodies, 1u, JPH::BodyID::cMaxBodyIndex);
        cfg.maxBodyPairs = std::max<uint32_t>(cfg.maxBodyPairs, 1u);
        cfg.maxContactConstraints = std::max<uint32_t>(cfg.maxContactConstraints, 1u);

        unsigned threads = cfg.threads;
        if (threads == 0) {
            const unsigned hardware = std::thread::hardware_concurrency();
            threads = std::clamp<unsigned>(hardware > 1 ? hardware - 1 : 1, 1u, 8u);
        }
        threads = std::min<unsigned>(threads, 64u);

        m_Impl->sim = std::make_unique<Impl::Simulation>(cfg, static_cast<int>(threads));
        m_Impl->config = cfg;
        m_Impl->events.clear();
        m_Impl->reportedUpdateErrors = 0;
        m_Impl->warnedBodyLimit = false;
        m_Impl->initialized.store(true);

        Log::Info("Physics initialized (Jolt, {} worker thread(s), up to {} bodies)", threads, cfg.maxBodies);
        return true;
    }

    void PhysicsWorld::Shutdown() {
        if (!m_Impl->sim) return;
        m_Impl->initialized.store(false);
        m_Impl->sim.reset();
        m_Impl->events.clear();
    }

    bool PhysicsWorld::IsInitialized() const { return m_Impl->sim != nullptr; }

    void PhysicsWorld::Step(float dt) {
        Impl::Simulation* sim = m_Impl->sim.get();
        if (!sim || !(dt > 0.0f) || !IsFinite(dt)) return;

        const JPH::EPhysicsUpdateError error = sim->system.Update(dt, 1, &sim->tempAllocator, &sim->jobSystem);
        const uint32_t bits = static_cast<uint32_t>(error);
        if (const uint32_t fresh = bits & ~m_Impl->reportedUpdateErrors; fresh != 0) {
            m_Impl->reportedUpdateErrors |= fresh;
            if (fresh & static_cast<uint32_t>(JPH::EPhysicsUpdateError::ManifoldCacheFull)) {
                Log::Warn("PhysicsWorld: contact manifold cache is full, some contacts are ignored "
                          "(raise PhysicsConfig::maxContactConstraints, now {})", m_Impl->config.maxContactConstraints);
            }
            if (fresh & static_cast<uint32_t>(JPH::EPhysicsUpdateError::BodyPairCacheFull)) {
                Log::Warn("PhysicsWorld: body pair cache is full, some contacts are ignored "
                          "(raise PhysicsConfig::maxBodyPairs, now {})", m_Impl->config.maxBodyPairs);
            }
            if (fresh & static_cast<uint32_t>(JPH::EPhysicsUpdateError::ContactConstraintsFull)) {
                Log::Warn("PhysicsWorld: contact constraint buffer is full, some contacts are ignored "
                          "(raise PhysicsConfig::maxContactConstraints, now {})", m_Impl->config.maxContactConstraints);
            }
        }

        sim->contacts.Publish(m_Impl->events);
    }

    glm::vec3 PhysicsWorld::Gravity() const {
        return m_Impl->sim ? ToGlm(m_Impl->sim->system.GetGravity()) : m_Impl->config.gravity;
    }

    // --- shapes -------------------------------------------------------------------------------

    ShapeRef PhysicsWorld::CreateCompoundShape(std::span<const BoxDesc> boxes) const {
        if (boxes.empty()) return nullptr;
        if (!m_Impl->initialized.load()) {
            Log::Error("PhysicsWorld::CreateCompoundShape: physics is not initialized");
            return nullptr;
        }
        RuntimeRef runtime;

        size_t usable = 0;
        const BoxDesc* only = nullptr;
        for (const BoxDesc& box : boxes) {
            if (!IsUsableBox(box.center, box.halfExtent)) continue;
            only = &box;
            ++usable;
        }
        if (usable != boxes.size()) {
            Log::Warn("PhysicsWorld::CreateCompoundShape: ignoring {} box(es) with a non-positive or non-finite size",
                      boxes.size() - usable);
        }
        if (usable == 0) return nullptr;

        // Jolt compounds need at least two parts; a single box is a (translated) box.
        if (usable == 1) {
            JPH::Ref<JPH::BoxShape> box = MakeBox(only->halfExtent, only->density);
            if (only->center == glm::vec3(0.0f)) return WrapShape(box.GetPtr());
            JPH::Ref<JPH::Shape> moved = new JPH::RotatedTranslatedShape(ToJolt(only->center), JPH::Quat::sIdentity(), box.GetPtr());
            return WrapShape(moved.GetPtr());
        }

        JPH::StaticCompoundShapeSettings settings;
        settings.SetEmbedded();
        for (const BoxDesc& box : boxes) {
            if (!IsUsableBox(box.center, box.halfExtent)) continue;
            settings.AddShape(ToJolt(box.center), JPH::Quat::sIdentity(), MakeBox(box.halfExtent, box.density).GetPtr());
        }
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.IsValid()) {
            Log::Error("PhysicsWorld::CreateCompoundShape: {}", result.HasError() ? result.GetError().c_str() : "unknown error");
            return nullptr;
        }
        return WrapShape(result.Get().GetPtr());
    }

    ShapeRef PhysicsWorld::CreateMeshShape(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices) const {
        if (vertices.empty() || indices.size() < 3) return nullptr;
        if (!m_Impl->initialized.load()) {
            Log::Error("PhysicsWorld::CreateMeshShape: physics is not initialized");
            return nullptr;
        }
        if (indices.size() % 3 != 0) {
            Log::Error("PhysicsWorld::CreateMeshShape: index count {} is not a multiple of 3", indices.size());
            return nullptr;
        }
        for (const uint32_t index : indices) {
            if (index >= vertices.size()) {
                Log::Error("PhysicsWorld::CreateMeshShape: index {} is out of range ({} vertices)", index, vertices.size());
                return nullptr;
            }
        }
        for (const glm::vec3& v : vertices) {
            if (!IsFinite(v)) {
                Log::Error("PhysicsWorld::CreateMeshShape: the mesh contains a non-finite vertex");
                return nullptr;
            }
        }
        RuntimeRef runtime;

        JPH::VertexList vertexList;
        vertexList.reserve(vertices.size());
        for (const glm::vec3& v : vertices) vertexList.push_back(JPH::Float3(v.x, v.y, v.z));
        JPH::IndexedTriangleList triangles;
        triangles.reserve(indices.size() / 3);
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2], 0));
        }

        // The constructor removes degenerate and duplicate triangles.
        JPH::MeshShapeSettings settings(std::move(vertexList), std::move(triangles));
        settings.SetEmbedded();
        if (settings.mIndexedTriangles.empty()) return nullptr;

        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.IsValid()) {
            Log::Error("PhysicsWorld::CreateMeshShape: {}", result.HasError() ? result.GetError().c_str() : "unknown error");
            return nullptr;
        }
        return WrapShape(result.Get().GetPtr());
    }

    ShapeRef PhysicsWorld::CreateBoxShape(const glm::vec3& halfExtent, float density) const {
        if (!m_Impl->initialized.load()) {
            Log::Error("PhysicsWorld::CreateBoxShape: physics is not initialized");
            return nullptr;
        }
        if (!IsUsableBox(glm::vec3(0.0f), halfExtent)) {
            Log::Error("PhysicsWorld::CreateBoxShape: invalid half extent ({}, {}, {})", halfExtent.x, halfExtent.y, halfExtent.z);
            return nullptr;
        }
        RuntimeRef runtime;
        return WrapShape(MakeBox(halfExtent, density).GetPtr());
    }

    ShapeRef PhysicsWorld::CreateSphereShape(float radius, float density) const {
        if (!m_Impl->initialized.load()) {
            Log::Error("PhysicsWorld::CreateSphereShape: physics is not initialized");
            return nullptr;
        }
        if (!IsFinite(radius) || radius < kMinHalfExtent) {
            Log::Error("PhysicsWorld::CreateSphereShape: invalid radius {}", radius);
            return nullptr;
        }
        RuntimeRef runtime;
        JPH::Ref<JPH::SphereShape> sphere = new JPH::SphereShape(radius);
        sphere->SetDensity(UsableDensity(density));
        return WrapShape(sphere.GetPtr());
    }

    // --- bodies -------------------------------------------------------------------------------

    BodyId PhysicsWorld::CreateBody(const BodyDesc& desc) {
        Impl::Simulation* sim = m_Impl->sim.get();
        if (!sim) {
            Log::Error("PhysicsWorld::CreateBody: physics is not initialized");
            return {};
        }
        if (!desc.shape || desc.shape->Get() == nullptr) {
            Log::Error("PhysicsWorld::CreateBody: the body has no shape");
            return {};
        }
        const JPH::Shape* shape = desc.shape->Get();
        const bool meshLike = shape->MustBeStatic();
        if (meshLike && desc.motion != MotionType::Static) {
            Log::Error("PhysicsWorld::CreateBody: mesh shapes can only be used by static bodies");
            return {};
        }
        if (!IsFinite(desc.position) || !IsFinite(desc.rotation) || !IsFinite(desc.linearVelocity) || !IsFinite(desc.angularVelocity)) {
            Log::Error("PhysicsWorld::CreateBody: position, rotation and velocities must be finite");
            return {};
        }

        JPH::BodyCreationSettings settings(shape, ToJoltR(desc.position), ToJolt(desc.rotation), ToJolt(desc.motion), ToJolt(desc.layer));
        // Bodies made of solid shapes keep their mass properties even while static, so that
        // SetMotionType can make them move later.
        settings.mAllowDynamicOrKinematic = !meshLike;
        settings.mIsSensor = desc.sensor;
        settings.mMotionQuality = desc.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
        settings.mAllowSleeping = desc.allowSleeping;
        settings.mFriction = IsFinite(desc.friction) ? std::max(desc.friction, 0.0f) : 0.0f;
        settings.mRestitution = IsFinite(desc.restitution) ? std::clamp(desc.restitution, 0.0f, 1.0f) : 0.0f;
        settings.mLinearDamping = IsFinite(desc.linearDamping) ? std::max(desc.linearDamping, 0.0f) : 0.0f;
        settings.mAngularDamping = IsFinite(desc.angularDamping) ? std::max(desc.angularDamping, 0.0f) : 0.0f;
        settings.mGravityFactor = IsFinite(desc.gravityFactor) ? desc.gravityFactor : 1.0f;
        settings.mUserData = desc.userData;
        if (desc.motion != MotionType::Static) {
            settings.mLinearVelocity = ToJolt(desc.linearVelocity);
            settings.mAngularVelocity = ToJolt(desc.angularVelocity);
        }

        JPH::BodyInterface& bodies = sim->system.GetBodyInterface();
        JPH::Body* body = bodies.CreateBody(settings);
        if (!body) {
            if (!m_Impl->warnedBodyLimit) {
                Log::Warn("PhysicsWorld::CreateBody: out of bodies ({} in use); raise PhysicsConfig::maxBodies",
                          sim->system.GetNumBodies());
                m_Impl->warnedBodyLimit = true;
            }
            return {};
        }
        const JPH::BodyID id = body->GetID();
        bodies.AddBody(id, desc.motion == MotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        return BodyId{id.GetIndexAndSequenceNumber()};
    }

    void PhysicsWorld::DestroyBody(BodyId body) {
        const auto id = m_Impl->Find(body);
        if (!id) return;
        JPH::BodyInterface& bodies = m_Impl->sim->system.GetBodyInterface();
        bodies.RemoveBody(*id);
        bodies.DestroyBody(*id);
    }

    bool PhysicsWorld::IsValid(BodyId body) const { return m_Impl->Find(body).has_value(); }

    BodyState PhysicsWorld::GetState(BodyId body) const {
        BodyState state;
        const auto id = m_Impl->Find(body);
        if (!id) return state;
        const JPH::BodyLockRead lock(m_Impl->sim->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return state;
        const JPH::Body& b = lock.GetBody();
        state.position = ToGlm(b.GetPosition());       // the shape origin, not the centre of mass
        state.rotation = ToGlm(b.GetRotation());
        state.linearVelocity = ToGlm(b.GetLinearVelocity());
        state.angularVelocity = ToGlm(b.GetAngularVelocity());
        state.active = b.IsActive();
        return state;
    }

    void PhysicsWorld::SetTransform(BodyId body, const glm::vec3& position, const glm::quat& rotation, bool activate) {
        const auto id = m_Impl->Find(body);
        if (!id) return;
        if (!IsFinite(position) || !IsFinite(rotation)) {
            Log::Warn("PhysicsWorld::SetTransform: ignoring a non-finite transform");
            return;
        }
        m_Impl->sim->system.GetBodyInterface().SetPositionAndRotation(
            *id, ToJoltR(position), ToJolt(rotation), activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    }

    void PhysicsWorld::SetVelocity(BodyId body, const glm::vec3& linear, const glm::vec3& angular) {
        const auto id = m_Impl->Find(body);
        if (!id) return;
        if (!IsFinite(linear) || !IsFinite(angular)) {
            Log::Warn("PhysicsWorld::SetVelocity: ignoring a non-finite velocity");
            return;
        }
        m_Impl->sim->system.GetBodyInterface().SetLinearAndAngularVelocity(*id, ToJolt(linear), ToJolt(angular));
    }

    void PhysicsWorld::AddImpulse(BodyId body, const glm::vec3& impulse, std::optional<glm::vec3> worldPoint) {
        const auto id = m_Impl->Find(body);
        if (!id) return;
        if (!IsFinite(impulse) || (worldPoint && !IsFinite(*worldPoint))) {
            Log::Warn("PhysicsWorld::AddImpulse: ignoring a non-finite impulse");
            return;
        }
        JPH::BodyInterface& bodies = m_Impl->sim->system.GetBodyInterface();
        if (worldPoint) bodies.AddImpulse(*id, ToJolt(impulse), ToJoltR(*worldPoint));
        else bodies.AddImpulse(*id, ToJolt(impulse));
    }

    void PhysicsWorld::SetMotionType(BodyId body, MotionType motion) {
        const auto id = m_Impl->Find(body);
        if (!id) return;
        JPH::PhysicsSystem& system = m_Impl->sim->system;
        const JPH::EMotionType target = ToJolt(motion);

        JPH::ObjectLayer layer = kLayerDynamic;
        {
            const JPH::BodyLockRead lock(system.GetBodyLockInterface(), *id);
            if (!lock.Succeeded()) return;
            const JPH::Body& b = lock.GetBody();
            if (b.GetMotionType() == target) return;
            if (target != JPH::EMotionType::Static && (b.GetShape()->MustBeStatic() || b.GetMotionPropertiesUnchecked() == nullptr)) {
                Log::Error("PhysicsWorld::SetMotionType: a body with a mesh shape must stay static");
                return;
            }
            layer = b.GetObjectLayer();
        }

        JPH::BodyInterface& bodies = system.GetBodyInterface();
        // Static-layer objects never collide with each other: a body that starts to fall must
        // leave that layer or it would drop through the terrain.
        if (target == JPH::EMotionType::Dynamic && layer == kLayerStatic) bodies.SetObjectLayer(*id, kLayerDynamic);
        bodies.SetMotionType(*id, target, JPH::EActivation::Activate);
    }

    void PhysicsWorld::SetShape(BodyId body, const ShapeRef& shape) {
        const auto id = m_Impl->Find(body);
        if (!id) return;
        if (!shape || shape->Get() == nullptr) {
            Log::Error("PhysicsWorld::SetShape: the new shape is null");
            return;
        }
        JPH::PhysicsSystem& system = m_Impl->sim->system;
        const JPH::Shape* joltShape = shape->Get();
        const bool meshLike = joltShape->MustBeStatic();
        if (meshLike) {
            const JPH::BodyLockRead lock(system.GetBodyLockInterface(), *id);
            if (!lock.Succeeded()) return;
            if (!lock.GetBody().IsStatic()) {
                Log::Error("PhysicsWorld::SetShape: mesh shapes can only be used by static bodies");
                return;
            }
        }
        // Jolt keeps the body position (shape origin), rotation and velocities; only the centre
        // of mass, the mass properties and the bounds follow the new shape.
        system.GetBodyInterface().SetShape(*id, joltShape, !meshLike, JPH::EActivation::Activate);
    }

    void PhysicsWorld::Activate(BodyId body) {
        const auto id = m_Impl->Find(body);
        if (!id) return;
        JPH::BodyInterface& bodies = m_Impl->sim->system.GetBodyInterface();
        if (bodies.GetMotionType(*id) != JPH::EMotionType::Static) bodies.ActivateBody(*id);
    }

    void PhysicsWorld::ActivateInBox(const math::AABB& box) {
        if (!m_Impl->sim || box.IsEmpty() || !IsFinite(box.min) || !IsFinite(box.max)) return;
        // Everything that can move lives in the moving broad-phase layer.
        m_Impl->sim->system.GetBodyInterface().ActivateBodiesInAABox(JPH::AABox(ToJolt(box.min), ToJolt(box.max)),
                                                                     JPH::SpecifiedBroadPhaseLayerFilter(kBroadPhaseMoving),
                                                                     JPH::ObjectLayerFilter());
    }

    float PhysicsWorld::GetMass(BodyId body) const {
        const auto id = m_Impl->Find(body);
        if (!id) return 0.0f;
        const JPH::BodyLockRead lock(m_Impl->sim->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return 0.0f;
        const JPH::Body& b = lock.GetBody();
        if (const JPH::MotionProperties* motion = b.GetMotionPropertiesUnchecked(); motion && b.IsDynamic()) {
            const float inverse = motion->GetInverseMassUnchecked();
            if (inverse > 0.0f) return 1.0f / inverse;
        }
        // Static and kinematic bodies: what the body would weigh if it moved (0 for meshes).
        return b.GetShape()->MustBeStatic() ? 0.0f : b.GetShape()->GetMassProperties().mMass;
    }

    uint64_t PhysicsWorld::GetUserData(BodyId body) const {
        const auto id = m_Impl->Find(body);
        return id ? m_Impl->sim->system.GetBodyInterface().GetUserData(*id) : 0;
    }

    glm::vec3 PhysicsWorld::GetPointVelocity(BodyId body, const glm::vec3& worldPoint) const {
        const auto id = m_Impl->Find(body);
        if (!id || !IsFinite(worldPoint)) return glm::vec3(0.0f);
        return ToGlm(m_Impl->sim->system.GetBodyInterface().GetPointVelocity(*id, ToJoltR(worldPoint)));
    }

    math::AABB PhysicsWorld::GetBounds(BodyId body) const {
        const auto id = m_Impl->Find(body);
        if (!id) return {};
        const JPH::BodyLockRead lock(m_Impl->sim->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return {};
        const JPH::AABox& bounds = lock.GetBody().GetWorldSpaceBounds();
        return math::AABB(ToGlm(bounds.mMin), ToGlm(bounds.mMax));
    }

    // --- queries ------------------------------------------------------------------------------

    RayHit PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
        RayHit out;
        if (!m_Impl->sim || !IsFinite(origin) || !IsFinite(direction) || !IsFinite(maxDistance) || !(maxDistance > 0.0f)) return out;
        const float length = glm::length(direction);
        if (!(length > 1e-12f)) return out;
        const glm::vec3 dir = direction / length;

        const JPH::PhysicsSystem& system = m_Impl->sim->system;
        const JPH::RRayCast ray(ToJoltR(origin), ToJolt(dir * maxDistance));
        JPH::RayCastResult result;
        const IgnoreSensorsFilter noSensors;
        if (!system.GetNarrowPhaseQuery().CastRay(ray, result, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter(), noSensors)) return out;

        const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
        out.hit = true;
        out.body = BodyId{result.mBodyID.GetIndexAndSequenceNumber()};
        out.distance = result.mFraction * maxDistance;
        out.position = ToGlm(point);
        out.normal = -dir;

        const JPH::BodyLockRead lock(system.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& b = lock.GetBody();
            out.userData = b.GetUserData();
            const glm::vec3 normal = ToGlm(b.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
            const float normalLength = glm::length(normal);
            if (IsFinite(normal) && normalLength > 1e-6f) {
                out.normal = normal / normalLength;
                // Always report the side of the surface the ray came from.
                if (glm::dot(out.normal, dir) > 0.0f) out.normal = -out.normal;
            }
        }
        return out;
    }

    void PhysicsWorld::GetActiveBodies(std::vector<BodyId>& out) const {
        out.clear();
        if (!m_Impl->sim) return;
        const JPH::PhysicsSystem& system = m_Impl->sim->system;
        JPH::BodyIDVector active;
        system.GetActiveBodies(JPH::EBodyType::RigidBody, active);
        out.reserve(active.size());
        const JPH::BodyLockInterfaceNoLock& lookup = system.GetBodyLockInterfaceNoLock();
        for (const JPH::BodyID& id : active) {
            const JPH::Body* b = lookup.TryGetBody(id);
            if (b && b->IsDynamic()) out.push_back(BodyId{id.GetIndexAndSequenceNumber()});
        }
    }

    std::vector<ContactEvent> PhysicsWorld::TakeContactEvents() {
        std::vector<ContactEvent> out = std::move(m_Impl->events);
        m_Impl->events.clear();
        return out;
    }

    uint32_t PhysicsWorld::BodyCount() const { return m_Impl->sim ? m_Impl->sim->system.GetNumBodies() : 0u; }

    uint32_t PhysicsWorld::ActiveBodyCount() const {
        return m_Impl->sim ? m_Impl->sim->system.GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0u;
    }

    // --- characters ---------------------------------------------------------------------------

    CharacterId PhysicsWorld::CreateCharacter(const CharacterDesc& desc) {
        Impl::Simulation* sim = m_Impl->sim.get();
        if (!sim) {
            Log::Error("PhysicsWorld::CreateCharacter: physics is not initialized");
            return {};
        }
        if (!IsFinite(desc.position) || !IsFinite(desc.height) || !IsFinite(desc.radius) || !IsFinite(desc.maxSlopeDegrees) ||
            !IsFinite(desc.stepHeight) || !IsFinite(desc.mass)) {
            Log::Error("PhysicsWorld::CreateCharacter: the description contains non-finite values");
            return {};
        }

        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        const float padding = settings->mCharacterPadding;
        const float radius = std::max(desc.radius, 0.05f);
        const float height = std::max(desc.height, 2.0f * radius + 0.02f);
        const float halfCylinder = 0.5f * height - radius;

        // A capsule standing on the feet position. Jolt keeps the shape `padding` away from
        // everything it touches, so the shape is shrunk by that amount: shape + padding is the
        // capsule that was asked for, and the feet rest exactly on the ground.
        JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(halfCylinder, radius - padding);
        JPH::Ref<JPH::Shape> standing = new JPH::RotatedTranslatedShape(JPH::Vec3(0.0f, 0.5f * height, 0.0f), JPH::Quat::sIdentity(), capsule.GetPtr());

        settings->mShape = standing.GetPtr();
        settings->mMass = std::max(desc.mass, 1.0f);
        settings->mMaxSlopeAngle = JPH::DegreesToRadians(std::clamp(desc.maxSlopeDegrees, 0.0f, 89.0f));
        // Only contacts on the lower hemisphere of the capsule can carry the character.
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
        // Voxel terrain is full of coplanar triangles; this keeps walking over their seams smooth.
        settings->mEnhancedInternalEdgeRemoval = true;

        uint32_t index;
        if (!sim->freeCharacters.empty()) {
            index = sim->freeCharacters.back();
            sim->freeCharacters.pop_back();
        } else {
            if (sim->characters.size() > kCharacterIndexMask) {
                Log::Error("PhysicsWorld::CreateCharacter: too many characters");
                return {};
            }
            index = static_cast<uint32_t>(sim->characters.size());
            sim->characters.emplace_back();
        }

        CharacterSlot& slot = sim->characters[index];
        slot.character = new JPH::CharacterVirtual(settings.GetPtr(), ToJoltR(desc.position), JPH::Quat::sIdentity(), 0, &sim->system);
        slot.stepHeight = std::max(desc.stepHeight, 0.0f);
        slot.character->RefreshContacts(sim->system.GetDefaultBroadPhaseLayerFilter(kLayerCharacter),
                                        CharacterLayerFilter(), JPH::BodyFilter(), JPH::ShapeFilter(),
                                        sim->tempAllocator);
        return CharacterId{(slot.generation << kCharacterIndexBits) | index};
    }

    void PhysicsWorld::DestroyCharacter(CharacterId character) {
        CharacterSlot* slot = m_Impl->FindCharacter(character);
        if (!slot) return;
        slot->character = nullptr;
        slot->generation = (slot->generation + 1u) % kCharacterGenerationCount;   // stale ids stop matching
        m_Impl->sim->freeCharacters.push_back(character.value & kCharacterIndexMask);
    }

    void PhysicsWorld::UpdateCharacter(CharacterId character, float dt, const glm::vec3& desiredVelocity, float jumpSpeed) {
        CharacterSlot* slot = m_Impl->FindCharacter(character);
        if (!slot || !(dt > 0.0f) || !IsFinite(dt)) return;
        Impl::Simulation& sim = *m_Impl->sim;
        JPH::CharacterVirtual& ch = *slot->character;

        const JPH::Vec3 gravity = sim.system.GetGravity();
        const glm::vec3 wanted = IsFinite(desiredVelocity) ? desiredVelocity : glm::vec3(0.0f);

        // Pick up the motion of whatever the character stands on (moving platforms, sliding debris).
        ch.UpdateGroundVelocity();
        const JPH::Vec3 groundVelocity = ch.GetGroundVelocity();
        const float verticalSpeed = ch.GetLinearVelocity().GetY();
        const bool movingTowardsGround = (verticalSpeed - groundVelocity.GetY()) < 0.1f;

        JPH::Vec3 velocity;
        if (ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround && movingTowardsGround) {
            // Standing: the fall speed is forgotten, and this is the only moment a jump can start.
            velocity = groundVelocity;
            if (IsFinite(jumpSpeed) && jumpSpeed > 0.0f) velocity += JPH::Vec3(0.0f, jumpSpeed, 0.0f);
        } else {
            velocity = JPH::Vec3(0.0f, verticalSpeed, 0.0f);
        }
        velocity += gravity * dt;
        velocity += JPH::Vec3(wanted.x, 0.0f, wanted.z);
        ch.SetLinearVelocity(velocity);

        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, slot->stepHeight, 0.0f);
        // Follow the ground down ledges the character could also climb, instead of hopping off them.
        update.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(slot->stepHeight, 0.1f), 0.0f);

        ch.ExtendedUpdate(dt, gravity, update, sim.system.GetDefaultBroadPhaseLayerFilter(kLayerCharacter),
                          CharacterLayerFilter(), JPH::BodyFilter(), JPH::ShapeFilter(), sim.tempAllocator);
    }

    CharacterState PhysicsWorld::GetCharacterState(CharacterId character) const {
        CharacterState state;
        const CharacterSlot* slot = m_Impl->FindCharacter(character);
        if (!slot) return state;
        state.position = ToGlm(slot->character->GetPosition());
        state.velocity = ToGlm(slot->character->GetLinearVelocity());
        state.onGround = slot->character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        return state;
    }

    void PhysicsWorld::SetCharacterPosition(CharacterId character, const glm::vec3& feetPosition) {
        CharacterSlot* slot = m_Impl->FindCharacter(character);
        if (!slot) return;
        if (!IsFinite(feetPosition)) {
            Log::Warn("PhysicsWorld::SetCharacterPosition: ignoring a non-finite position");
            return;
        }
        Impl::Simulation& sim = *m_Impl->sim;
        slot->character->SetPosition(ToJoltR(feetPosition));
        slot->character->RefreshContacts(sim.system.GetDefaultBroadPhaseLayerFilter(kLayerCharacter),
                                         CharacterLayerFilter(), JPH::BodyFilter(), JPH::ShapeFilter(),
                                         sim.tempAllocator);
    }
}
