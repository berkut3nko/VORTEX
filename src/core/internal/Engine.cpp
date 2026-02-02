module;

#include <entt/entt.hpp>
#include <chrono>
#include <memory> 
#include <functional>
#include <vector> 
#include <string> 
#include <algorithm> // Added for std::remove
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
                for (auto& frag : fragments) {
                    frag->isDestructible = true; // Allow recursive destruction
                    AddEntity(frag, frag->isStatic);
                }
                return; 
            }
        }

        entity->isStatic = isStatic;
        m_State->editor.GetEntities().push_back(entity);
        m_State->editor.MarkDirty();
        
        auto handle = m_State->physicsSystem.AddBody(entity, isStatic);
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
            
            // Editor Update
            {
                core::ProfileScope p("CPU: Editor Update");
                m_State->editor.Update(m_State->graphicsContext->GetWindow(), m_State->graphicsContext->GetCamera(), m_State->graphicsContext->GetSceneManager(), width, height);

                auto newEntities = m_State->editor.ConsumeCreatedEntities();
                for(auto& e : newEntities) {
                    AddEntity(e, false); 
                }
            }

            // Scene Upload (if dirty)
            if (m_State->editor.IsSceneDirty()) {
                core::ProfileScope p("CPU: Scene Upload");
                
                std::vector<graphics::SceneObject> objects;
                std::vector<voxel::Chunk> chunks = m_State->persistentChunks;
                std::vector<voxel::PhysicalMaterial> finalMaterials = m_State->persistentMaterials; 
                
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
                        currentPaletteOffset = (uint32_t)finalMaterials.size();
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

            // Systems Update
            {
                core::ProfileScope p("CPU: Systems Update");
                m_State->cameraController.Update(m_State->graphicsContext->GetWindow(), m_State->graphicsContext->GetCamera(), deltaTime);
                m_State->graphicsContext->UploadCamera();
                
                UpdateSystems(deltaTime);
            }

            // Rendering
            m_State->graphicsContext->BeginRecording();
            m_State->graphicsContext->RecordScene();
            m_State->graphicsContext->RecordAA();

            {
                core::ProfileScope p("CPU: GUI Render");
                ImGui::Begin("Voxel Stats");
                if (onGuiRender) onGuiRender();
                ImGui::Separator();
                
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

        // 1. Prepare Palette for SHRED checks
        voxel::MaterialPalette tempPalette;
        for(const auto& m : m_State->persistentMaterials) tempPalette.AddMaterial(m);
        // Note: Dynamic meshes might have their own local materials not in persistent list at index.
        // For simplicity, we assume global palette for integrity checks or basic default.

        // Lists to manage entity lifecycle changes
        std::vector<std::shared_ptr<voxel::VoxelEntity>> entitiesToAdd;
        std::vector<physics::BodyHandle> bodiesToRemove;
        
        // We use an index loop because we might need to remove elements (carefully)
        std::vector<size_t> indicesToRemove;

        for (size_t i = 0; i < m_State->simObjects.size(); ++i) {
            auto& simObj = m_State->simObjects[i];
            auto& entity = simObj.entity;
            
            if (!entity) continue;

            // --- SHRED: Structural Integrity Check ---
            // Only check if destructible and NOT currently being grabbed by editor
            if (entity->isDestructible && entity != selectedEntity) {
                // Perform check. Warning: This is expensive to do every frame for all objects.
                // In a real game, schedule this less frequently or only on collision events.
                if (voxel::SHREDSystem::ValidateStructuralIntegrity(entity, tempPalette)) {
                    // Integrity failed -> Voxels destroyed.
                    // Now check if it split into islands.
                    auto islands = voxel::SHREDSystem::AnalyzeConnectivity(entity);
                    
                    if (islands.empty()) {
                        // Case 0: Total destruction (no voxels left)
                        Log::Info("SHRED: Entity '" + entity->name + "' was completely pulverized.");
                        indicesToRemove.push_back(i);
                        bodiesToRemove.push_back(simObj.bodyHandle);
                    }
                    else if (islands.size() > 1) {
                        // Case 1: Split into multiple fragments
                        Log::Info("SHRED: Structural Failure! Entity '" + entity->name + "' split into " + std::to_string(islands.size()) + " fragments.");
                        auto fragments = voxel::SHREDSystem::SplitEntity(entity, islands);
                        entitiesToAdd.insert(entitiesToAdd.end(), fragments.begin(), fragments.end());
                        
                        // Mark current parent for removal
                        indicesToRemove.push_back(i);
                        bodiesToRemove.push_back(simObj.bodyHandle);
                    } else {
                        // Case 2: Just damage (1 island remaining), keep original entity but update physics
                        entity->shouldRebuildPhysics = true;
                        m_State->editor.MarkDirty();
                    }
                }
            }

            // --- Physics Rebuild ---
            if (entity->shouldRebuildPhysics) {
                m_State->physicsSystem.RemoveBody(simObj.bodyHandle);
                simObj.bodyHandle = m_State->physicsSystem.AddBody(entity, entity->isStatic);
                if (entity->isTrigger) m_State->physicsSystem.SetBodySensor(simObj.bodyHandle, true);
                
                entity->shouldRebuildPhysics = false;
                simObj.lastStaticState = entity->isStatic;
                simObj.lastTriggerState = entity->isTrigger;
            }

            // --- State Sync ---
            if (entity->isStatic != simObj.lastStaticState) {
                m_State->physicsSystem.SetBodyType(simObj.bodyHandle, entity->isStatic);
                simObj.lastStaticState = entity->isStatic;
            }

            if (entity->isTrigger != simObj.lastTriggerState) {
                m_State->physicsSystem.SetBodySensor(simObj.bodyHandle, entity->isTrigger);
                simObj.lastTriggerState = entity->isTrigger;
            }

            // Gizmo/Physics Sync
            if (!entity->isStatic) {
                bool isSelected = (entity == selectedEntity);
                m_State->physicsSystem.SetBodyKinematic(simObj.bodyHandle, isSelected);

                if (isSelected) {
                    m_State->physicsSystem.SetBodyTransform(simObj.bodyHandle, entity->transform);
                }
            }
        }

        // Process Removals (Reverse order to keep indices valid)
        for (auto it = indicesToRemove.rbegin(); it != indicesToRemove.rend(); ++it) {
            size_t idx = *it;
            
            // Capture the entity pointer before removing the wrapper from simObjects
            auto entityPtr = m_State->simObjects[idx].entity;

            // 1. Remove from Physics Simulation list
            m_State->simObjects.erase(m_State->simObjects.begin() + idx);
            
            // 2. Remove from Editor Entity List (Crucial for correct selection and rendering)
            auto& editorEntities = m_State->editor.GetEntities();
            
            // Note: std::remove shifts valid elements to the front and returns the new logical end.
            auto newEnd = std::remove(editorEntities.begin(), editorEntities.end(), entityPtr);
            if (newEnd != editorEntities.end()) {
                editorEntities.erase(newEnd, editorEntities.end());
                // Force scene update since entity list changed
                m_State->editor.MarkDirty();
            }
        }
        
        // Clean up Jolt physics bodies
        for(auto b : bodiesToRemove) m_State->physicsSystem.RemoveBody(b);

        // Process Additions (New fragments)
        for (auto& newE : entitiesToAdd) {
            AddEntity(newE, newE->isStatic);
        }

        // --- Physics Step ---
        m_State->physicsSystem.Update(deltaTime);

        // --- Sync Transforms ---
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