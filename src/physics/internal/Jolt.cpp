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
#include <Jolt/Physics/Body/BodyLock.h> 

#include <iostream>
#include <cstdarg>
#include <thread>
#include <vector>
#include <cmath> // Added for ceil
#include <algorithm> // Added for max
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp> 

module vortex.physics;

import vortex.log;
import vortex.voxel;

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

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        switch (inObject1) {
            case Layers::NON_MOVING: return inObject2 == Layers::MOVING; 
            case Layers::MOVING:     return true; 
            default:                 return false;
        }
    }
};

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

class MyBodyActivationListener : public JPH::BodyActivationListener {
public:
    virtual void OnBodyActivated(const JPH::BodyID& inBodyID, uint64_t inBodyUserData) override { }
    virtual void OnBodyDeactivated(const JPH::BodyID& inBodyID, uint64_t inBodyUserData) override { }
};

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
        if (m_Internal->jobSystem) {
            Shutdown();
        }

        JPH::RegisterDefaultAllocator();
        
        JPH::Trace = TraceImpl;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        m_Internal->tempAllocator = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
        m_Internal->jobSystem = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);

        const uint32_t cMaxBodies = 4096; // Increased body limit for voxel debris
        const uint32_t cNumBodyMutexes = 0;
        const uint32_t cMaxBodyPairs = 4096;
        const uint32_t cMaxContactConstraints = 4096;

        m_Internal->physicsSystem.Init(
            cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
            m_Internal->bpLayerInterface, 
            m_Internal->objectVsBpLayerFilter, 
            m_Internal->objectLayerPairFilter
        );

        m_Internal->physicsSystem.SetBodyActivationListener(&m_Internal->bodyActivationListener);
        
        // --- Critical for Voxel Stability ---
        // Increase tolerance for penetration slightly to prevent jitter
        JPH::PhysicsSettings settings;
        settings.mPenetrationSlop = 0.02f; // Default is 0.02, kept for reference
        settings.mBaumgarte = 0.2f;        // Stabilization factor
        m_Internal->physicsSystem.SetPhysicsSettings(settings);

        vortex::Log::Info("Jolt Physics Initialized.");
    }

    void PhysicsSystem::Shutdown() {
        if (m_Internal && m_Internal->jobSystem) {
            delete m_Internal->jobSystem;
            delete m_Internal->tempAllocator;
            
            if (JPH::Factory::sInstance) {
                delete JPH::Factory::sInstance;
                JPH::Factory::sInstance = nullptr;
            }
            
            m_Internal->jobSystem = nullptr;
            m_Internal->tempAllocator = nullptr;
        }
    }

    void PhysicsSystem::Update(float deltaTime) {
        if(m_Internal->jobSystem) {
            // FIX: Prevent tunneling by ensuring enough collision steps
            // If deltaTime is large (lag), we take more steps.
            // Target frequency: 60Hz.
            const float stepSize = 1.0f / 60.0f;
            int cCollisionSteps = std::max(1, (int)std::ceil(deltaTime / stepSize));
            
            // Hard cap to prevent death spiral on extreme lag
            if (cCollisionSteps > 10) cCollisionSteps = 10;

            m_Internal->physicsSystem.Update(deltaTime, cCollisionSteps, m_Internal->tempAllocator, m_Internal->jobSystem);
        }
    }

    BodyHandle PhysicsSystem::AddBody(std::shared_ptr<vortex::voxel::VoxelEntity> entity, bool isStatic) {
        if (!entity || entity->parts.empty() || !m_Internal->jobSystem) return {JPH::BodyID::cInvalidBodyID};
        
        // Check if we have any valid colliders
        auto boxes = VoxelColliderBuilder::Build(*entity->parts[0]->chunk); 
        bool hasAny = !boxes.empty();
        
        if (!hasAny) {
             for(auto& p : entity->parts) {
                if(p->chunk && !VoxelColliderBuilder::Build(*p->chunk).empty()) {
                    hasAny = true; break;
                }
            }
        }
        
        if(!hasAny) return {JPH::BodyID::cInvalidBodyID};

        JPH::StaticCompoundShapeSettings compoundSettings;
        
        for (const auto& part : entity->parts) {
            if (!part || !part->chunk) continue;
            
            auto partBoxes = VoxelColliderBuilder::Build(*part->chunk);
            for (const auto& box : partBoxes) {
                JPH::Vec3 halfExtent = ToJolt(box.size) * 0.5f;
                // Jolt boxes are centered.
                glm::vec3 boxCenterLocal = part->position + box.min + (box.size * 0.5f);
                JPH::Vec3 center = ToJolt(boxCenterLocal);
                compoundSettings.AddShape(center, JPH::Quat::sIdentity(), new JPH::BoxShape(halfExtent));
            }
        }

        auto shapeResult = compoundSettings.Create();
        if (shapeResult.HasError()) {
            vortex::Log::Error("Jolt Shape Error: " + std::string(shapeResult.GetError()));
            return {JPH::BodyID::cInvalidBodyID};
        }

        JPH::ShapeRefC shape = shapeResult.Get();

        JPH::BodyCreationSettings bodySettings(
            shape.GetPtr(),                          
            ToJolt(glm::vec3(entity->transform[3])), 
            ToJolt(glm::quat(entity->transform)),    
            isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
            isStatic ? Layers::NON_MOVING : Layers::MOVING
        );

        bodySettings.mAllowSleeping = true;
        bodySettings.mIsSensor = entity->isTrigger;
        
        if (!isStatic) {
            bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bodySettings.mMassPropertiesOverride.mMass = 10.0f; // TODO: Calculate based on voxel count/density
            
            // --- FIX: Enable Continuous Collision Detection (CCD) ---
            // This prevents fast moving voxel debris from tunneling through the floor.
            bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
        }

        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        
        if (!body) return {JPH::BodyID::cInvalidBodyID};

        bodyInterface.AddBody(body->GetID(), isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

        return { body->GetID().GetIndexAndSequenceNumber() };
    }

    void PhysicsSystem::RemoveBody(BodyHandle handle) {
        if (handle.id == JPH::BodyID::cInvalidBodyID || !m_Internal->jobSystem) return;
        
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        
        bodyInterface.RemoveBody(bodyID);
        bodyInterface.DestroyBody(bodyID);
    }

    void PhysicsSystem::SyncBodyTransform(std::shared_ptr<vortex::voxel::VoxelEntity> entity, BodyHandle handle) {
        if(!m_Internal->jobSystem) return;
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        
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
        entity->transform = transform;
    }

    void PhysicsSystem::SetBodyKinematic(BodyHandle handle, bool isKinematic) {
        if(!m_Internal->jobSystem) return;
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();

        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) return;

        JPH::EMotionType targetType = isKinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;

        if (bodyInterface.GetMotionType(bodyID) != targetType) {
            bodyInterface.SetMotionType(bodyID, targetType, JPH::EActivation::Activate);
            if (isKinematic) bodyInterface.SetLinearAndAngularVelocity(bodyID, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            else bodyInterface.ActivateBody(bodyID);
        }
    }

    void PhysicsSystem::SetBodyType(BodyHandle handle, bool isStatic) {
        if(!m_Internal->jobSystem) return;
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
        if(!m_Internal->jobSystem) return;
        JPH::BodyID bodyID(handle.id);
        const JPH::BodyLockInterface& lockInterface = m_Internal->physicsSystem.GetBodyLockInterface();
        JPH::BodyLockWrite lock(lockInterface, bodyID);
        if (lock.Succeeded()) {
            lock.GetBody().SetIsSensor(isTrigger);
        }
    }

    void PhysicsSystem::SetBodyTransform(BodyHandle handle, const glm::mat4& transform) {
        if(!m_Internal->jobSystem) return;
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) return;

        glm::vec3 pos = glm::vec3(transform[3]);
        glm::quat rot = glm::quat_cast(transform);

        bodyInterface.SetPositionAndRotation(bodyID, ToJolt(pos), ToJolt(rot), JPH::EActivation::DontActivate);
    }

    // --- Velocity Implementation ---

    glm::vec3 PhysicsSystem::GetLinearVelocity(BodyHandle handle) {
        if(!m_Internal->jobSystem) return glm::vec3(0.0f);
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        return ToGlm(bodyInterface.GetLinearVelocity(bodyID));
    }

    glm::vec3 PhysicsSystem::GetAngularVelocity(BodyHandle handle) {
        if(!m_Internal->jobSystem) return glm::vec3(0.0f);
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        return ToGlm(bodyInterface.GetAngularVelocity(bodyID));
    }

    void PhysicsSystem::SetLinearVelocity(BodyHandle handle, const glm::vec3& velocity) {
        if(!m_Internal->jobSystem) return;
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        bodyInterface.SetLinearVelocity(bodyID, ToJolt(velocity));
    }

    void PhysicsSystem::SetAngularVelocity(BodyHandle handle, const glm::vec3& velocity) {
        if(!m_Internal->jobSystem) return;
        JPH::BodyID bodyID(handle.id);
        JPH::BodyInterface& bodyInterface = m_Internal->physicsSystem.GetBodyInterface();
        bodyInterface.SetAngularVelocity(bodyID, ToJolt(velocity));
    }
}