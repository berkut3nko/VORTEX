module;

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <entt/entt.hpp>
#include <chrono>

module vortex.core;

namespace vortex {

    // --- Logger Implementation ---

    std::shared_ptr<spdlog::logger> s_CoreLogger;

    void Log::Init() {
        spdlog::set_pattern("%^[%T] %n: %v%$");
        s_CoreLogger = spdlog::stdout_color_mt("VORTEX");
        s_CoreLogger->set_level(spdlog::level::trace);
    }

    void Log::Info(const std::string& msg) {
        if (s_CoreLogger) s_CoreLogger->info(msg);
    }

    void Log::Warn(const std::string& msg) {
        if (s_CoreLogger) s_CoreLogger->warn(msg);
    }

    void Log::Error(const std::string& msg) {
        if (s_CoreLogger) s_CoreLogger->error(msg);
    }

    std::shared_ptr<spdlog::logger>& Log::GetCoreLogger() {
        return s_CoreLogger;
    }

    // --- Engine Implementation ---

    Engine::Engine() {
        // Automatically initialize logger on creation if not done
        if (!s_CoreLogger) {
            Log::Init();
        }
        m_GraphicsContext = std::make_unique<graphics::GraphicsContext>();
    }

    Engine::~Engine() {
        Shutdown();
    }

    bool Engine::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        Log::Info("Initializing Vortex Engine...");

        if (!m_GraphicsContext->Initialize(title, width, height)) {
            Log::Error("Failed to initialize Graphics Context!");
            return false;
        }

        Log::Info("Engine initialized successfully.");
        return true;
    }

    void Engine::Run() {
        m_IsRunning = true;
        Log::Info("Starting Main Loop...");

        auto lastTime = std::chrono::high_resolution_clock::now();

        while (m_IsRunning) {
            // Calculate Delta Time
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
            lastTime = currentTime;

            // 1. Begin Frame (Poll events, acquire swapchain)
            if (!m_GraphicsContext->BeginFrame()) {
                m_IsRunning = false;
                break;
            }

            // 2. Update Gameplay Systems (ECS)
            UpdateSystems(deltaTime);

            // 3. Render Frame
            m_GraphicsContext->EndFrame();
        }

        Log::Info("Main Loop finished.");
    }

    void Engine::Shutdown() {
        Log::Info("Shutting down engine...");
        m_GraphicsContext->Shutdown();
    }

    void Engine::UpdateSystems(float deltaTime) {
        // Placeholder: In a ECS, you would invoke systems here.
        // Example:
        // auto view = m_Registry.view<TransformComponent, VelocityComponent>();
        // for(auto entity : view) { ... }
        
        // For now, we just pass purely to keep the loop running.
        (void)deltaTime; 
    }

}