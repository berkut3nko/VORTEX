module;

#include <entt/entt.hpp>
#include <chrono>
#include <memory> 
#include <functional>
#include <vector> 
#include <string> 
#include <algorithm> 
#include <GLFW/glfw3.h>
#include <imgui.h> 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

module vortex.core;

import vortex.log;
import vortex.graphics;
import vortex.voxel;
import vortex.editor; 
import vortex.physics; 
import :camera;
import :profiller;

namespace vortex {
    struct SimulationObject {
        std::shared_ptr<voxel::VoxelEntity> entity;
        physics::BodyHandle bodyHandle;
        bool lastStaticState;
        bool lastTriggerState;

        SimulationObject(std::shared_ptr<voxel::VoxelEntity> e, physics::BodyHandle h, bool isStatic, bool isTrigger)
            : entity(e), bodyHandle(h), lastStaticState(isStatic), lastTriggerState(isTrigger) {}
    };

    struct Engine::InternalState {
        std::unique_ptr<graphics::GraphicsContext> graphicsContext;
        core::CameraController cameraController;
        editor::Editor editor; 
        
        physics::PhysicsSystem physicsSystem;
        std::vector<SimulationObject> simObjects;

        std::vector<graphics::SceneObject> persistentObjects;
        std::vector<voxel::Chunk> persistentChunks;
        std::vector<voxel::PhysicalMaterial> persistentMaterials;
        
        // --- Lighting Settings (Restored) ---
        float sunPitch = 45.0f;
        float sunYaw = 45.0f;
        float sunIntensity = 1.0f;
        float ambientIntensity = 0.3f;
        float sunColor[3] = {1.0f, 0.95f, 0.8f};

        bool isRunning = false;
    };

    Engine::Engine() : m_State(std::make_unique<InternalState>()) {
        Log::Init();
        m_State->graphicsContext = std::make_unique<graphics::GraphicsContext>();
        m_State->cameraController.movementSpeed = 5.0f;
    }

    Engine::~Engine() = default;

    bool Engine::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        if (!m_State->graphicsContext->Initialize(title, width, height)) return false;
        m_State->physicsSystem.Initialize();
        return true;
    }

    void Engine::AddEntity(std::shared_ptr<voxel::VoxelEntity> entity, bool isStatic) {
        if (!entity) return;

        // --- SHRED Analysis Pass (Initial) ---
        if (entity->isDestructible) {
            auto islands = voxel::SHREDSystem::AnalyzeConnectivity(entity);
            if (islands.size() > 1) {
                Log::Info("SHRED: Entity '" + entity->name + "' split into " + std::to_string(islands.size()) + " fragments upon init.");
                auto fragments = voxel::SHREDSystem::SplitEntity(entity, islands);
                
                std::vector<voxel::PhysicalMaterial> inheritedMats;
                if (auto mesh = std::dynamic_pointer_cast<voxel::DynamicMeshObject>(entity)) {
                    inheritedMats = mesh->materials;
                }

                for (auto& frag : fragments) {
                    frag->isDestructible = true; 
                    
                    if (!inheritedMats.empty()) {
                        auto meshFrag = std::make_shared<voxel::DynamicMeshObject>();
                        meshFrag->name = frag->name;
                        meshFrag->transform = frag->transform;
                        meshFrag->parts = frag->parts;
                        meshFrag->localBoundsMin = frag->localBoundsMin;
                        meshFrag->localBoundsMax = frag->localBoundsMax;
                        meshFrag->logicalCenter = frag->logicalCenter;
                        meshFrag->totalVoxelCount = frag->totalVoxelCount;
                        meshFrag->isDestructible = frag->isDestructible;
                        meshFrag->isStatic = frag->isStatic;
                        meshFrag->isTrigger = frag->isTrigger;
                        meshFrag->materials = inheritedMats;
                        
                        AddEntity(meshFrag, meshFrag->isStatic);
                    } else {
                        AddEntity(frag, frag->isStatic);
                    }
                }
                return; 
            }
        }

        entity->isStatic = isStatic;
        m_State->editor.GetEntities().push_back(entity);
        m_State->editor.MarkDirty();
        
        auto handle = m_State->physicsSystem.AddBody(entity, isStatic);
        
        if (!isStatic) {
            if (glm::length(entity->cachedLinearVelocity) > 0.0f || glm::length(entity->cachedAngularVelocity) > 0.0f) {
                 m_State->physicsSystem.SetLinearVelocity(handle, entity->cachedLinearVelocity);
                 m_State->physicsSystem.SetAngularVelocity(handle, entity->cachedAngularVelocity);
                 
                 entity->cachedLinearVelocity = glm::vec3(0.0f);
                 entity->cachedAngularVelocity = glm::vec3(0.0f);
            }
        }

        m_State->simObjects.emplace_back(entity, handle, isStatic, entity->isTrigger);
    }

    void Engine::Run(std::function<void()> onGuiRender) {
        m_State->isRunning = true;
        Log::Info("Starting Main Loop...");

        auto lastTime = std::chrono::high_resolution_clock::now();

        while (m_State->isRunning) {
            core::ProfileScope frameTimer("CPU: Total Frame");

            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
            lastTime = currentTime;

            if (!m_State->graphicsContext->BeginFrame()) {
                m_State->isRunning = false;
                break;
            }
            
            int width, height;
            glfwGetFramebufferSize(m_State->graphicsContext->GetWindow(), &width, &height);
            
            {
                core::ProfileScope p("CPU: Editor Update");
                m_State->editor.Update(m_State->graphicsContext->GetWindow(), m_State->graphicsContext->GetCamera(), m_State->graphicsContext->GetSceneManager(), width, height);

                auto newEntities = m_State->editor.ConsumeCreatedEntities();
                for(auto& e : newEntities) {
                    AddEntity(e, false); 
                }
            }

            {
                core::ProfileScope p("CPU: Systems Update");
                m_State->cameraController.Update(m_State->graphicsContext->GetWindow(), m_State->graphicsContext->GetCamera(), deltaTime);
                m_State->graphicsContext->UploadCamera();

                // --- RESTORED: Light Update & Upload ---
                graphics::DirectionalLight sun;
                sun.SetDirection(m_State->sunPitch, m_State->sunYaw);
                sun.direction.w = m_State->sunIntensity;
                sun.color = glm::vec4(m_State->sunColor[0], m_State->sunColor[1], m_State->sunColor[2], m_State->ambientIntensity);
                m_State->graphicsContext->UploadLight(sun);
                
                UpdateSystems(deltaTime);
            }

            if (m_State->editor.IsSceneDirty()) {
                core::ProfileScope p("CPU: Scene Upload");
                
                std::vector<graphics::SceneObject> objects;
                std::vector<voxel::Chunk> chunks; 
                
                // 1. Починаємо з матеріалів, завантажених у main.cpp (Глобальна палітра)
                std::vector<voxel::PhysicalMaterial> finalMaterials = m_State->persistentMaterials; 
                
                // 2. Якщо палітра порожня, додаємо дефолтний, щоб уникнути крашу
                if (finalMaterials.empty()) {
                    voxel::PhysicalMaterial defMat{};
                    defMat.color = glm::vec4(1.0f);
                    finalMaterials.push_back(defMat);
                }

                for (const auto& entity : m_State->editor.GetEntities()) {
                    glm::mat4 root = entity->transform;
                    uint32_t currentPaletteOffset = 0; 
                    
                    auto dynMesh = std::dynamic_pointer_cast<voxel::DynamicMeshObject>(entity);
                    if (dynMesh && !dynMesh->materials.empty()) {
                        currentPaletteOffset = (uint32_t)finalMaterials.size() - 1;
                        finalMaterials.insert(finalMaterials.end(), dynMesh->materials.begin(), dynMesh->materials.end());
                    } else {
                        currentPaletteOffset = 0; 
                    }

                    for (const auto& part : entity->parts) {
                        if (!part->chunk) continue;
                        
                        graphics::SceneObject obj;
                        obj.model = root * part->GetTransformMatrix();
                        obj.logicalCenter = part->logicalCenter;
                        obj.voxelCount = part->voxelCount;
                        obj.chunkIndex = (uint32_t)chunks.size(); 
                        chunks.push_back(*part->chunk);
                        obj.paletteOffset = currentPaletteOffset;
                        objects.push_back(obj);
                    }
                }

                m_State->graphicsContext->UploadScene(objects, finalMaterials, chunks);
                m_State->editor.ResetSceneDirty();
            }

            m_State->graphicsContext->BeginRecording();
            m_State->graphicsContext->RecordScene();
            m_State->graphicsContext->RecordAA();

            {
                core::ProfileScope p("CPU: GUI Render");
                ImGui::Begin("Voxel Stats");
                if (onGuiRender) onGuiRender();
                ImGui::Separator();
                
                // --- RESTORED: Lighting Controls ---
                if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::DragFloat("Sun Pitch", &m_State->sunPitch, 1.0f, -89.0f, 89.0f);
                    ImGui::DragFloat("Sun Yaw", &m_State->sunYaw, 1.0f, 0.0f, 360.0f);
                    ImGui::SliderFloat("Sun Intensity", &m_State->sunIntensity, 0.0f, 5.0f);
                    ImGui::SliderFloat("Ambient", &m_State->ambientIntensity, 0.0f, 1.0f);
                    ImGui::ColorEdit3("Sun Color", m_State->sunColor);
                }

                static int currentAA = 1;
                const char* aaModes[] = { "None", "FXAA", "TAA" };
                if (ImGui::Combo("Anti-Aliasing", &currentAA, aaModes, IM_ARRAYSIZE(aaModes))) {
                    m_State->graphicsContext->SetAAMode((vortex::graphics::AntiAliasingMode)currentAA);
                }
                ImGui::End();

                core::Profiler::Render();
                m_State->editor.RenderGizmo(m_State->graphicsContext->GetCamera(), width, height);
            }

            m_State->graphicsContext->EndFrame();

            float gpuScene = m_State->graphicsContext->GetSceneGPUTime();
            float gpuAA = m_State->graphicsContext->GetAAGPUTime();
            core::Profiler::AddSample("GPU: Geometry (Raymarch)", gpuScene);
            core::Profiler::AddSample("GPU: AA/Post", gpuAA);
        }
    }

    void Engine::UpdateSystems(float deltaTime) { 
        auto selectedEntity = m_State->editor.GetSelectedEntity();

        voxel::MaterialPalette tempPalette;
        for(const auto& m : m_State->persistentMaterials) tempPalette.AddMaterial(m);

        std::vector<std::shared_ptr<voxel::VoxelEntity>> entitiesToAdd;
        std::vector<physics::BodyHandle> bodiesToRemove;
        std::vector<size_t> indicesToRemove;

        for (size_t i = 0; i < m_State->simObjects.size(); ++i) {
            auto& simObj = m_State->simObjects[i];
            auto& entity = simObj.entity;
            
            if (!entity) continue;

            if (entity->isDestructible && entity != selectedEntity) {
                if (voxel::SHREDSystem::ValidateStructuralIntegrity(entity, tempPalette)) {
                    auto islands = voxel::SHREDSystem::AnalyzeConnectivity(entity);
                    
                    if (islands.empty()) {
                        Log::Info("SHRED: Entity '" + entity->name + "' was completely pulverized.");
                        indicesToRemove.push_back(i);
                        bodiesToRemove.push_back(simObj.bodyHandle);
                    }
                    else if (islands.size() > 1) {
                        Log::Info("SHRED: Structural Failure! Entity '" + entity->name + "' split into " + std::to_string(islands.size()) + " fragments.");
                        
                        glm::vec3 parentLinVel = m_State->physicsSystem.GetLinearVelocity(simObj.bodyHandle);
                        glm::vec3 parentAngVel = m_State->physicsSystem.GetAngularVelocity(simObj.bodyHandle);

                        std::vector<voxel::PhysicalMaterial> parentMaterials;
                        auto parentMesh = std::dynamic_pointer_cast<voxel::DynamicMeshObject>(entity);
                        if (parentMesh) {
                            parentMaterials = parentMesh->materials;
                        }

                        auto fragments = voxel::SHREDSystem::SplitEntity(entity, islands);
                        
                        for(auto& frag : fragments) {
                            frag->cachedLinearVelocity = parentLinVel;
                            frag->cachedAngularVelocity = parentAngVel;

                            if (!parentMaterials.empty()) {
                                auto meshFrag = std::make_shared<voxel::DynamicMeshObject>();
                                meshFrag->name = frag->name;
                                meshFrag->transform = frag->transform;
                                meshFrag->parts = frag->parts;
                                meshFrag->localBoundsMin = frag->localBoundsMin;
                                meshFrag->localBoundsMax = frag->localBoundsMax;
                                meshFrag->logicalCenter = frag->logicalCenter;
                                meshFrag->totalVoxelCount = frag->totalVoxelCount;
                                meshFrag->isDestructible = frag->isDestructible;
                                meshFrag->isStatic = frag->isStatic;
                                meshFrag->isTrigger = frag->isTrigger;
                                meshFrag->shouldRebuildPhysics = frag->shouldRebuildPhysics;
                                meshFrag->cachedLinearVelocity = frag->cachedLinearVelocity;
                                meshFrag->cachedAngularVelocity = frag->cachedAngularVelocity;
                                
                                meshFrag->materials = parentMaterials;
                                
                                frag = meshFrag; 
                            }
                        }

                        entitiesToAdd.insert(entitiesToAdd.end(), fragments.begin(), fragments.end());
                        
                        indicesToRemove.push_back(i);
                        bodiesToRemove.push_back(simObj.bodyHandle);
                    } else {
                        entity->shouldRebuildPhysics = true;
                        m_State->editor.MarkDirty();
                    }
                }
            }

            if (entity->shouldRebuildPhysics) {
                m_State->physicsSystem.RemoveBody(simObj.bodyHandle);
                simObj.bodyHandle = m_State->physicsSystem.AddBody(entity, entity->isStatic);
                if (entity->isTrigger) m_State->physicsSystem.SetBodySensor(simObj.bodyHandle, true);
                
                entity->shouldRebuildPhysics = false;
                simObj.lastStaticState = entity->isStatic;
                simObj.lastTriggerState = entity->isTrigger;
            }

            if (entity->isStatic != simObj.lastStaticState) {
                m_State->physicsSystem.SetBodyType(simObj.bodyHandle, entity->isStatic);
                simObj.lastStaticState = entity->isStatic;
            }

            if (entity->isTrigger != simObj.lastTriggerState) {
                m_State->physicsSystem.SetBodySensor(simObj.bodyHandle, entity->isTrigger);
                simObj.lastTriggerState = entity->isTrigger;
            }

            if (!entity->isStatic) {
                bool isSelected = (entity == selectedEntity);
                m_State->physicsSystem.SetBodyKinematic(simObj.bodyHandle, isSelected);

                if (isSelected) {
                    m_State->physicsSystem.SetBodyTransform(simObj.bodyHandle, entity->transform);
                }
            }
        }

        for (auto it = indicesToRemove.rbegin(); it != indicesToRemove.rend(); ++it) {
            size_t idx = *it;
            auto entityPtr = m_State->simObjects[idx].entity;
            m_State->simObjects.erase(m_State->simObjects.begin() + idx);
            auto& editorEntities = m_State->editor.GetEntities();
            auto newEnd = std::remove(editorEntities.begin(), editorEntities.end(), entityPtr);
            if (newEnd != editorEntities.end()) {
                editorEntities.erase(newEnd, editorEntities.end());
                m_State->editor.MarkDirty();
            }
        }
        
        for(auto b : bodiesToRemove) m_State->physicsSystem.RemoveBody(b);

        for (auto& newE : entitiesToAdd) {
            AddEntity(newE, newE->isStatic);
        }

        m_State->physicsSystem.Update(deltaTime);

        auto& sceneManager = m_State->graphicsContext->GetSceneManager();
        int renderIndex = 0;
        
        for (const auto& simObj : m_State->simObjects) {
            auto& entity = simObj.entity;
            if (!entity) { renderIndex++; continue; }

            if (!entity->isStatic && entity != selectedEntity) {
                m_State->physicsSystem.SyncBodyTransform(entity, simObj.bodyHandle);
            }
            
            glm::mat4 rootTransform = entity->transform;
            for (const auto& part : entity->parts) {
                if (part->chunk) {
                    glm::mat4 finalModel = rootTransform * part->GetTransformMatrix();
                    sceneManager.SetObjectTransform(renderIndex, finalModel);
                    renderIndex++;
                }
            }
        }
    }

    void Engine::UploadScene(
        const std::vector<graphics::SceneObject>& objects, 
        const std::vector<voxel::PhysicalMaterial>& materials,
        const std::vector<voxel::Chunk>& chunks
    ) {
        m_State->persistentObjects = objects;
        m_State->persistentChunks = chunks;
        m_State->persistentMaterials = materials;
        m_State->graphicsContext->UploadScene(objects, materials, chunks);
    }

    void Engine::Shutdown() {
        if (m_State) {
            m_State->physicsSystem.Shutdown();
            if (m_State->graphicsContext) m_State->graphicsContext->Shutdown();
        }
    }

    graphics::GraphicsContext& Engine::GetGraphics() { return *m_State->graphicsContext; }
}