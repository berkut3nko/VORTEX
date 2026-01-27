module;

#include <entt/entt.hpp>
#include <chrono>
#include <memory> 
#include <functional>
#include <vector> 
#include <string> 
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
        if (entity) {
            // Set initial state
            entity->isStatic = isStatic;
            
            // Add to Editor
            m_State->editor.GetEntities().push_back(entity);
            m_State->editor.MarkDirty();
            
            // Add to Physics
            auto handle = m_State->physicsSystem.AddBody(entity, isStatic);
            
            // Add to Simulation Loop using explicit constructor
            m_State->simObjects.emplace_back(entity, handle, isStatic, entity->isTrigger);
        }
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

                // NEW: Register entities created by Editor (Importer)
                auto newEntities = m_State->editor.ConsumeCreatedEntities();
                for(auto& e : newEntities) {
                    AddEntity(e, false); // Default to Dynamic for imported meshes
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

        // Iterate by reference to modify state
        for (auto& simObj : m_State->simObjects) {
            auto& entity = simObj.entity;
            
            if (!entity) continue;

            // --- 1. Handle Physics Rebuild (e.g. after Re-mesh) ---
            // Use local bool to help optimizer
            bool needsRebuild = entity->shouldRebuildPhysics;
            if (needsRebuild) {
                Log::Info("Rebuilding physics for entity: " + entity->name);
                m_State->physicsSystem.RemoveBody(simObj.bodyHandle);
                simObj.bodyHandle = m_State->physicsSystem.AddBody(entity, entity->isStatic);
                
                if (entity->isTrigger) {
                    m_State->physicsSystem.SetBodySensor(simObj.bodyHandle, true);
                }
                
                entity->shouldRebuildPhysics = false;
                
                // Sync internal state
                simObj.lastStaticState = entity->isStatic;
                simObj.lastTriggerState = entity->isTrigger;
            }

            // --- 2. Handle State Changes (Editor Toggles) ---
            bool currentStatic = entity->isStatic;
            if (currentStatic != simObj.lastStaticState) {
                m_State->physicsSystem.SetBodyType(simObj.bodyHandle, currentStatic);
                simObj.lastStaticState = currentStatic;
            }

            bool currentTrigger = entity->isTrigger;
            if (currentTrigger != simObj.lastTriggerState) {
                m_State->physicsSystem.SetBodySensor(simObj.bodyHandle, currentTrigger);
                simObj.lastTriggerState = currentTrigger;
            }

            // --- 3. Handle Gizmo Interaction ---
            if (!currentStatic) {
                bool isSelected = (entity == selectedEntity);
                m_State->physicsSystem.SetBodyKinematic(simObj.bodyHandle, isSelected);

                if (isSelected) {
                    m_State->physicsSystem.SetBodyTransform(simObj.bodyHandle, entity->transform);
                }
            }
        }

        // --- 4. Step Physics ---
        m_State->physicsSystem.Update(deltaTime);

        // --- 5. Sync Physics -> Graphics ---
        auto& sceneManager = m_State->graphicsContext->GetSceneManager();
        int renderIndex = 0;
        
        for (const auto& simObj : m_State->simObjects) {
            auto& entity = simObj.entity;
            if (!entity) {
                renderIndex += 1; // Or handle appropriately if using complex indexing
                continue;
            }

            // Sync Transform: Physics -> Entity
            // Skip if static or selected (controlled by Gizmo)
            if (!entity->isStatic && entity != selectedEntity) {
                m_State->physicsSystem.SyncBodyTransform(entity, simObj.bodyHandle);
            }
            
            // Sync Transform: Entity -> SceneManager (Render Cache)
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