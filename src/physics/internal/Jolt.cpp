module;

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyLock.h> // Required for BodyLockWrite

#include <iostream>
#include <cstdarg>
#include <thread>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp> 

module vortex.physics;

import vortex.log;
import vortex.voxel;

// --- Jolt Boilerplate Helpers ---

namespace Layers {
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
};

namespace BroadPhaseLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr uint32_t NUM_LAYERS(2);
};

// Class that determines if two object layers can collide
class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        switch (inObject1) {
            case Layers::NON_MOVING: return inObject2 == Layers::MOVING; // Non-moving only collides with moving
            case Layers::MOVING:     return true; // Moving collides with everything
            default:                 return false;
        }
    }
};

// Class that determines if an object layer can collide with a broadphase layer
class BPLayerInterfaceImpl : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }

    virtual uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
        return mObjectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch ((JPH::BroadPhaseLayer::Type)inLayer) {
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING: return "MOVING";
            default: return "INVALID";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};

// Class that determines if an object layer can collide with a broadphase layer
class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
            case Layers::NON_MOVING: return inLayer2 == BroadPhaseLayers::MOVING;
            case Layers::MOVING:     return true;
            default:                 return false;
        }
    }
};

// Simple body activation listener
class MyBodyActivationListener : public JPH::BodyActivationListener {
public:
    virtual void OnBodyActivated(const JPH::BodyID& inBodyID, uint64_t inBodyUserData) override { }
    virtual void OnBodyDeactivated(const JPH::BodyID& inBodyID, uint64_t inBodyUserData) override { }
};

// --- Helpers ---

static void TraceImpl(const char* inFMT, ...) {
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);
    std::cout << buffer << std::endl;
}

static JPH::Vec3 ToJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
static JPH::Quat ToJolt(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
static glm::vec3 ToGlm(const JPH::Vec3& v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }

namespace vortex::physics {

    struct PhysicsSystem::InternalState {
        JPH::TempAllocatorImpl* tempAllocator = nullptr;
        JPH::JobSystemThreadPool* jobSystem = nullptr;
        JPH::PhysicsSystem physicsSystem;
        
        BPLayerInterfaceImpl bpLayerInterface;
        ObjectVsBroadPhaseLayerFilterImpl objectVsBpLayerFilter;
        ObjectLayerPairFilterImpl objectLayerPairFilter;
        MyBodyActivationListener bodyActivationListener;
    };

    PhysicsSystem::PhysicsSystem() : m_Internal(std::make_unique<InternalState>()) {}
    PhysicsSystem::~PhysicsSystem() { Shutdown(); }

    void PhysicsSystem::Initialize() {
        JPH::RegisterDefaultAllocator();
        
        JPH::Trace = TraceImpl;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        m_Internal->tempAllocator = new JPH::TempAllocatorImpl(10 * 1024 * 1024); // 10MB temp buffer
        m_Internal->jobSystem = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);

        const uint32_t cMaxBodies = 1024;
        const uint32_t cNumBodyMutexes = 0;
        const uint32_t cMaxBodyPairs = 1024;
        const uint32_t cMaxContactConstraints = 1024;

        m_Internal->physicsSystem.Init(
            cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
            m_Internal->bpLayerInterface, 
            m_Internal->objectVsBpLayerFilter, 
            m_Internal->objectLayerPairFilter
        );

        m_Internal->physicsSystem.SetBodyActivationListener(&m_Internal->bodyActivationListener);
        
        vortex::Log::Info("Jolt Physics Initialized.");
    }

    void PhysicsSystem::Shutdown() {
        if (m_Internal->jobSystem) {
            delete m_Internal->jobSystem;
            delete m_Internal->tempAllocator;
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
            m_Internal->jobSystem = nullptr;
        }
    }

    void PhysicsSystem::Update(float deltaTime) {
        // If deltaTime is too high, clamp it or step multiple times. 
        // For simplicity, we use 1 step per frame here, but ideally use fixed timestep accumulator.
        const int cCollisionSteps = 1;
        m_Internal->physicsSystem.Update(deltaTime, cCollisionSteps, m_Internal->tempAllocator, m_Internal->jobSystem);
    }

    BodyHandle PhysicsSystem::AddBody(std::shared_ptr<vortex::voxel::VoxelEntity> entity, bool isStatic) {
        if (!entity || entity->parts.empty()) return {JPH::BodyID::cInvalidBodyID};

        auto& chunk = *entity->parts[0]->chunk;
        
        // 1. Build Colliders using VoxelColliderBuilder
        auto boxes = VoxelColliderBuilder::Build(chunk);

        if (boxes.empty()) {
            // Check if ANY part has colliders (logic from previous fix)
            bool hasAny = false;
            for(auto& p : entity->parts) {
                if(p->chunk && !VoxelColliderBuilder::Build(*p->chunk).empty()) {
                    hasAny = true; break;
                }
            }
            if(!hasAny) {
                vortex::Log::Warn("No colliders generated for entity: " + entity->name);
                return {JPH::BodyID::cInvalidBodyID};
            }
        }

        // 2. Create Compound Shape
        JPH::StaticCompoundShapeSettings compoundSettings;
        
        for (const auto& part : entity->parts) {
            if (!part || !part->chunk) continue;
            
            auto partBoxes = VoxelColliderBuilder::Build(*part->chunk);
            for (const auto& box : partBoxes) {
                JPH::Vec3 halfExtent = ToJolt(box.size) * 0.5f;
                // Calculate center in Entity Local Space
                glm::vec3 boxCenterLocal = part->position + box.min + (box.size * 0.5f);
                JPH::Vec3 center = ToJolt(boxCenterLocal);

                compoundSettings.AddShape(center, JPH::Quat::sIdentity(), new JPH::BoxShape(halfExtent));
            }
        }

        auto shapeResult = compoundSettings.Create();
        if (shapeResult.HasError()) {
            vortex::Log::Error("Failed to create Jolt shape: " + std::string(shapeResult.GetError()));
            return {JPH::BodyID::cInvalidBodyID};
        }

        JPH::ShapeRefC shape = shapeResult.Get();

        // 3. Create Body
        JPH::BodyCreationSettings bodySettings(
            shape.GetPtr(),                          // Pass raw pointer to shape
            ToJolt(glm::vec3(entity->transform[3])), // Initial Position
            ToJolt(glm::quat(entity->transform)),    // Initial Rotation
            isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
            isStatic ? Layers::NON_MOVING : Layers::MOVING
        );

        // Allow dynamic objects to sleep
        bodySettings.mAllowSleeping = true;
        bodySettings.mIsSensor = entity->isTrigger;
        
        if (!isStatic) {
            bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bodySettings.mMassPropertiesOverride.mMass = 10.0f; // Default mass
        }

        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        
        if (!body) return {JPH::BodyID::cInvalidBodyID};

        bodyInterface.AddBody(body->GetID(), isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

        return { body->GetID().GetIndexAndSequenceNumber() };
    }

    void PhysicsSystem::RemoveBody(BodyHandle handle) {
        if (handle.id == JPH::BodyID::cInvalidBodyID) return;
        
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        
        bodyInterface.RemoveBody(bodyID);
        bodyInterface.DestroyBody(bodyID);
    }

    void PhysicsSystem::SyncBodyTransform(std::shared_ptr<vortex::voxel::VoxelEntity> entity, BodyHandle handle) {
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        
        // If body is kinematic (being moved by gizmo), don't overwrite transform from physics!
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Kinematic || 
            bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) return;

        JPH::Vec3 pos;
        JPH::Quat rot;
        bodyInterface.GetPositionAndRotation(bodyID, pos, rot);
        
        glm::vec3 glmPos = ToGlm(pos);
        glm::quat glmRot(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
        
        glm::mat4 transform = glm::mat4(1.0f);
        transform = glm::translate(transform, glmPos);
        transform = transform * glm::mat4_cast(glmRot);
        
        // Preserve scale if it exists (assuming uniform scale 1.0 for now as Jolt doesn't scale rigid bodies easily)
        entity->transform = transform;
    }

    void PhysicsSystem::SetBodyKinematic(BodyHandle handle, bool isKinematic) {
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();

        // Check if static to prevent changing bedrock
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) return;

        JPH::EMotionType targetType = isKinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;

        if (bodyInterface.GetMotionType(bodyID) != targetType) {
            bodyInterface.SetMotionType(bodyID, targetType, JPH::EActivation::Activate);
            
            if (isKinematic) {
                // Stop movement when grabbing
                bodyInterface.SetLinearAndAngularVelocity(bodyID, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            } else {
                // Wake up when releasing
                bodyInterface.ActivateBody(bodyID);
            }
        }
    }

    void PhysicsSystem::SetBodyType(BodyHandle handle, bool isStatic) {
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        
        JPH::EMotionType newType = isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic;
        JPH::ObjectLayer newLayer = isStatic ? Layers::NON_MOVING : Layers::MOVING;

        if (bodyInterface.GetMotionType(bodyID) != newType) {
            bodyInterface.SetMotionType(bodyID, newType, isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            bodyInterface.SetObjectLayer(bodyID, newLayer); 
        }
    }

    void PhysicsSystem::SetBodySensor(BodyHandle handle, bool isTrigger) {
        JPH::BodyID bodyID(handle.id);
        // Use BodyLockWrite because BodyInterface does not have SetIsSensor
        const JPH::BodyLockInterface& lockInterface = m_Internal->physicsSystem.GetBodyLockInterface();
        JPH::BodyLockWrite lock(lockInterface, bodyID);
        if (lock.Succeeded()) {
            lock.GetBody().SetIsSensor(isTrigger);
        }
    }

    void PhysicsSystem::SetBodyTransform(BodyHandle handle, const glm::mat4& transform) {
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) return;

        glm::vec3 pos = glm::vec3(transform[3]);
        glm::quat rot = glm::quat_cast(transform);

        bodyInterface.SetPositionAndRotation(bodyID, ToJolt(pos), ToJolt(rot), JPH::EActivation::DontActivate);
    }

}