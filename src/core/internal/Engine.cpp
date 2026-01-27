module;

#include <entt/entt.hpp>
#include <chrono>
#include <memory> 
#include <functional>
<<<<<<< Updated upstream
#include <GLFW/glfw3.h>
#include <imgui.h> 
=======
#include <vector> 
#include <string> 
#include <GLFW/glfw3.h>
#include <imgui.h> 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
>>>>>>> Stashed changes

module vortex.core;

import vortex.log;
import vortex.graphics;
import vortex.voxel;
<<<<<<< Updated upstream
import vortex.editor; // Import Editor
import :camera;

// Use profiler partition implicitly available via vortex.core
// or explicitly if needed, but 'import vortex.core' exports it.

namespace vortex {

    /**
     * @brief Internal Pimpl state for the Engine.
     */
    struct Engine::InternalState {
        std::unique_ptr<graphics::GraphicsContext> graphicsContext;
        core::CameraController cameraController;
        editor::Editor editor; // Editor Instance
=======
import vortex.editor; 
import :camera;
import :profiller; // Make sure Profiler is imported

namespace vortex {

    struct Engine::InternalState {
        std::unique_ptr<graphics::GraphicsContext> graphicsContext;
        core::CameraController cameraController;
        editor::Editor editor; 
>>>>>>> Stashed changes
        entt::registry registry;
        
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
<<<<<<< Updated upstream
        if (!m_State->graphicsContext->Initialize(title, width, height)) {
            Log::Error("Failed to initialize Graphics Context!");
            return false;
        }
        Log::Info("Engine initialized successfully.");
=======
        if (!m_State->graphicsContext->Initialize(title, width, height)) return false;
>>>>>>> Stashed changes
        return true;
    }

    void Engine::AddEntity(std::shared_ptr<voxel::VoxelEntity> entity) {
        if (entity) {
            m_State->editor.GetEntities().push_back(entity);
            m_State->editor.MarkDirty();
        }
    }

    void Engine::Run(std::function<void()> onGuiRender) {
        m_State->isRunning = true;
        Log::Info("Starting Main Loop...");

        auto lastTime = std::chrono::high_resolution_clock::now();

        while (m_State->isRunning) {
<<<<<<< Updated upstream
            // Scope for the entire frame time
            core::ProfileScope frameTimer("Total Frame Time");
=======
            core::ProfileScope frameTimer("CPU: Total Frame");
>>>>>>> Stashed changes

            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
            lastTime = currentTime;

<<<<<<< Updated upstream
            // 1. Start Frame (Polls Inputs & Acquires Image)
            {
                core::ProfileScope p("Wait & Acquire");
                if (!m_State->graphicsContext->BeginFrame()) {
                    m_State->isRunning = false;
                    break;
                }
            }
            
            // Push GPU Timings to Profiler (from previous frame)
            core::Profiler::AddSample("GPU: Scene Geometry", m_State->graphicsContext->GetSceneGPUTime());
            core::Profiler::AddSample("GPU: Anti-Aliasing", m_State->graphicsContext->GetAAGPUTime());

            // Get Window dimensions for Raycast/Gizmo
            int width, height;
            glfwGetFramebufferSize(m_State->graphicsContext->GetWindow(), &width, &height);

            // 2. Editor Update (Raycast & Shortcuts)
            {
                core::ProfileScope p("Editor Update");
                m_State->editor.Update(
                    m_State->graphicsContext->GetWindow(), 
                    m_State->graphicsContext->GetCamera(),
                    m_State->graphicsContext->GetSceneManager(),
                    width, height
                );
            }

            // 3. Update Camera
            {
                core::ProfileScope p("Camera Update");
                m_State->cameraController.Update(
                    m_State->graphicsContext->GetWindow(), 
                    m_State->graphicsContext->GetCamera(), 
                    deltaTime
                );
                m_State->graphicsContext->UploadCamera();
            }

            // 4. Update Game Logic
            {
                core::ProfileScope p("Game Logic");
                UpdateSystems(deltaTime);
            }

            // --- RENDER RECORDING ---
            
            // 5. Begin Command Buffer
            m_State->graphicsContext->BeginRecording();

            // 6. Record Scene Geometry
            {
                core::ProfileScope p("Render: Geometry (CPU Rec)");
                // This measures how long CPU takes to generate draw calls
                m_State->graphicsContext->RecordScene();
            }

            // 7. Record Anti-Aliasing (Compute Dispatch)
            {
                core::ProfileScope p("Render: Anti-Aliasing (CPU Rec)");
                // This measures how long CPU takes to dispatch compute shader
                m_State->graphicsContext->RecordAA();
            }

            // GUI Rendering
            ImGui::Begin("Voxel Stats");
            
            if (onGuiRender) {
                onGuiRender();
=======
            if (!m_State->graphicsContext->BeginFrame()) {
                m_State->isRunning = false;
                break;
            }
            
            int width, height;
            glfwGetFramebufferSize(m_State->graphicsContext->GetWindow(), &width, &height);
            
            {
                core::ProfileScope p("CPU: Editor Update");
                m_State->editor.Update(m_State->graphicsContext->GetWindow(), m_State->graphicsContext->GetCamera(), m_State->graphicsContext->GetSceneManager(), width, height);
>>>>>>> Stashed changes
            }
            ImGui::Separator();        
            // AA Selector
            static int currentAA = 1; // Default FXAA (Enum index)
            const char* aaModes[] = { "None", "FXAA", "TAA" };
            
            if (ImGui::Combo("Anti-Aliasing", &currentAA, aaModes, IM_ARRAYSIZE(aaModes))) {
                m_State->graphicsContext->SetAAMode((vortex::graphics::AntiAliasingMode)currentAA);
            }
            
            ImGui::End();
            
            // Render Gizmo (Must be called inside ImGui frame)
            {
                core::ProfileScope p("Gizmo Render");
                m_State->editor.RenderGizmo(
                    m_State->graphicsContext->GetCamera(),
                    width, height
                );
            }

<<<<<<< Updated upstream
            // Render Profiler Window
            core::Profiler::Render();

            // 8. Submit & Present (UI, Blit, End)
            {
                core::ProfileScope p("GPU Submit & Present");
                m_State->graphicsContext->EndFrame();
            }
        }
=======
            // --- SYNC SCENE ---
            if (m_State->editor.IsSceneDirty()) {
                core::ProfileScope p("CPU: Scene Upload");
                
                std::vector<graphics::SceneObject> objects = m_State->persistentObjects;
                std::vector<voxel::Chunk> chunks = m_State->persistentChunks;
                std::vector<voxel::PhysicalMaterial> finalMaterials = m_State->persistentMaterials;
                
                if (finalMaterials.empty()) {
                    voxel::PhysicalMaterial defMat{};
                    defMat.color = glm::vec4(1.0f);
                    defMat.density = 1.0f;
                    finalMaterials.push_back(defMat);
                }
>>>>>>> Stashed changes

                for (const auto& entity : m_State->editor.GetEntities()) {
                    glm::mat4 root = entity->transform;
                    uint32_t currentPaletteOffset = 0; 
                    
                    auto dynMesh = std::dynamic_pointer_cast<voxel::DynamicMeshObject>(entity);
                    if (dynMesh && !dynMesh->materials.empty()) {
                        if (finalMaterials.empty()) currentPaletteOffset = 0;
                        else currentPaletteOffset = (uint32_t)finalMaterials.size() - 1;

                        finalMaterials.insert(finalMaterials.end(), dynMesh->materials.begin(), dynMesh->materials.end());
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

            m_State->cameraController.Update(m_State->graphicsContext->GetWindow(), m_State->graphicsContext->GetCamera(), deltaTime);
            m_State->graphicsContext->UploadCamera();
            UpdateSystems(deltaTime);

            m_State->graphicsContext->BeginRecording();
            m_State->graphicsContext->RecordScene();
            m_State->graphicsContext->RecordAA();

            // GUI Rendering
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

                // Render Profiler Window (Graphs)
                core::Profiler::Render();
                
                m_State->editor.RenderGizmo(m_State->graphicsContext->GetCamera(), width, height);
            }

            m_State->graphicsContext->EndFrame();

            // --- Collect GPU Timings ---
            // These values come from the PREVIOUS frame's queries (available now)
            float gpuScene = m_State->graphicsContext->GetSceneGPUTime();
            float gpuAA = m_State->graphicsContext->GetAAGPUTime();
            
            core::Profiler::AddSample("GPU: Geometry (Raymarch)", gpuScene);
            core::Profiler::AddSample("GPU: AA/Post", gpuAA);
        }
    }

    void Engine::Shutdown() {
        if (m_State && m_State->graphicsContext) m_State->graphicsContext->Shutdown();
    }

<<<<<<< Updated upstream
    void Engine::UpdateSystems(float deltaTime) {
        (void)deltaTime;
        // Placeholder for ECS system updates
    }
=======
    void Engine::UpdateSystems(float deltaTime) { (void)deltaTime; }
>>>>>>> Stashed changes

    void Engine::UploadScene(
        const std::vector<graphics::SceneObject>& objects, 
        const std::vector<voxel::PhysicalMaterial>& materials,
        const std::vector<voxel::Chunk>& chunks
    ) {
<<<<<<< Updated upstream
=======
        m_State->persistentObjects = objects;
        m_State->persistentChunks = chunks;
        m_State->persistentMaterials = materials;
        
        for(auto& obj : m_State->persistentObjects) {
            obj.paletteOffset = 0;
        }

>>>>>>> Stashed changes
        m_State->graphicsContext->UploadScene(objects, materials, chunks);
    }

    graphics::GraphicsContext& Engine::GetGraphics() { return *m_State->graphicsContext; }
}