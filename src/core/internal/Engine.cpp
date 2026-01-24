module;

#include <entt/entt.hpp>
#include <chrono>
#include <memory> 
#include <functional>
#include <GLFW/glfw3.h> // needed for window handle

module vortex.core;

import vortex.log;
import vortex.graphics;
import vortex.voxel;
import vortex.editor; // Import Editor
import :camera;

namespace vortex {

    /**
     * @brief Internal Pimpl state for the Engine.
     */
    struct Engine::InternalState {
        std::unique_ptr<graphics::GraphicsContext> graphicsContext;
        core::CameraController cameraController;
        editor::Editor editor; // Editor Instance
        entt::registry registry;
        bool isRunning = false;
    };

    Engine::Engine() : m_State(std::make_unique<InternalState>()) {
        Log::Init();
        m_State->graphicsContext = std::make_unique<graphics::GraphicsContext>();
        m_State->cameraController.movementSpeed = 5.0f;
    }

    Engine::~Engine() = default;

    bool Engine::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        if (!m_State->graphicsContext->Initialize(title, width, height)) {
            Log::Error("Failed to initialize Graphics Context!");
            return false;
        }
        Log::Info("Engine initialized successfully.");
        return true;
    }

    void Engine::Run(std::function<void()> onGuiRender) {
        m_State->isRunning = true;
        Log::Info("Starting Main Loop...");

        auto lastTime = std::chrono::high_resolution_clock::now();

        while (m_State->isRunning) {
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
            lastTime = currentTime;

            // 1. Start Frame (Polls Inputs)
            if (!m_State->graphicsContext->BeginFrame()) {
                m_State->isRunning = false;
                break;
            }

            // Get Window dimensions for Raycast/Gizmo
            int width, height;
            glfwGetFramebufferSize(m_State->graphicsContext->GetWindow(), &width, &height);

            // 2. Editor Update (Raycast & Shortcuts)
            // BEFORE Camera Update to block input if needed
            m_State->editor.Update(
                m_State->graphicsContext->GetWindow(), 
                m_State->graphicsContext->GetCamera(),
                m_State->graphicsContext->GetSceneManager(),
                width, height
            );

            // 3. Update Camera
            m_State->cameraController.Update(
                m_State->graphicsContext->GetWindow(), 
                m_State->graphicsContext->GetCamera(), 
                deltaTime
            );
            m_State->graphicsContext->UploadCamera();

            // 4. Update Game Logic
            UpdateSystems(deltaTime);

            // 5. Custom GUI + Editor Render
            if (onGuiRender) {
                onGuiRender();
            }
            
            // Render Gizmo (Must be called inside ImGui frame)
            m_State->editor.RenderGizmo(
                m_State->graphicsContext->GetCamera(),
                width, height
            );

            // 6. Render
            m_State->graphicsContext->EndFrame();
        }

        Log::Info("Main Loop finished.");
    }

    void Engine::Shutdown() {
        Log::Info("Shutting down engine...");
        if (m_State && m_State->graphicsContext) {
            m_State->graphicsContext->Shutdown();
        }
    }

    void Engine::UpdateSystems(float deltaTime) {
        (void)deltaTime;
        // Placeholder for ECS system updates
    }

    void Engine::UploadScene(
        const std::vector<graphics::SceneObject>& objects, 
        const std::vector<graphics::SceneMaterial>& materials,
        const std::vector<voxel::Chunk>& chunks
    ) {
        m_State->graphicsContext->UploadScene(objects, materials, chunks);
    }

    graphics::GraphicsContext& Engine::GetGraphics() {
        return *m_State->graphicsContext;
    }
}