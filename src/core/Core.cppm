module;

// Global Module Fragment
#include <entt/entt.hpp>
#include <memory>
#include <string_view>

export module vortex.core;

// Import our internal modules
import vortex.graphics;
// Import core sub-modules
export import :logger;
export import :profiller;

namespace vortex {
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