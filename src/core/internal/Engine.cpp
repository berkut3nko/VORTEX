module;

#include <entt/entt.hpp>
#include <chrono>
#include <memory> 
#include <functional>

module vortex.core;

import vortex.log;
import vortex.graphics;
import vortex.voxel;
import :camera;

namespace vortex {

    /**
     * @brief Internal Pimpl state for the Engine.
     * @details Hides implementation details and dependencies (like EnTT) from the public API.
     */
    struct Engine::InternalState {
        std::unique_ptr<graphics::GraphicsContext> graphicsContext;
        core::CameraController cameraController;
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

            // 2. Update Camera
            m_State->cameraController.Update(
                m_State->graphicsContext->GetWindow(), 
                m_State->graphicsContext->GetCamera(), 
                deltaTime
            );
            m_State->graphicsContext->UploadCamera();

            // 3. Update Game Logic
            UpdateSystems(deltaTime);

            // 4. Custom GUI
            if (onGuiRender) {
                onGuiRender();
            }

            // 5. Render
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