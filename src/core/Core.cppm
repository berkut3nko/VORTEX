module;

// Global Module Fragment
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>
#include <memory>
#include <string>
#include <string_view>

export module vortex.core;

// Import our internal modules
import vortex.graphics;

namespace vortex {

    /**
     * @brief Static logger wrapper to provide clean API in modules.
     */
    export class Log {
    public:
        static void Init();
        
        static void Info(const std::string& msg);
        static void Warn(const std::string& msg);
        static void Error(const std::string& msg);
        
        // Expose underlying spdlog logger if advanced usage is needed
        static std::shared_ptr<spdlog::logger>& GetCoreLogger();
    };

    /**
     * @brief The main Engine class orchestrating the ECS and Systems.
     */
    export class Engine {
    public:
        Engine();
        ~Engine();

        /**
         * @brief Initializes the engine, window, and graphics context.
         * @param title Window title.
         * @param width Window width.
         * @param height Window height.
         */
        bool Initialize(const std::string& title, uint32_t width, uint32_t height);

        /**
         * @brief Starts the main loop.
         */
        void Run();

        /**
         * @brief Stops the engine loop.
         */
        void Shutdown();

        /**
         * @brief Access the ECS registry directly.
         * @return Reference to the EnTT registry.
         */
        entt::registry& GetRegistry() { return m_Registry; }

    private:
        // Core systems
        std::unique_ptr<graphics::GraphicsContext> m_GraphicsContext;
        entt::registry m_Registry;
        
        bool m_IsRunning = false;

        // Private helper to update ECS systems
        void UpdateSystems(float deltaTime);
    };

}