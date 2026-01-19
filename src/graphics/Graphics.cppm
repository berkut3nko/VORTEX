module;

// Global Module Fragment
// Include headers necessary for the interface types, if any.
#include <cstdint>
#include <memory>
#include <string>

export module vortex.graphics;

namespace vortex::graphics {

    /**
     * @brief Manages the window, Vulkan instance, and ImGui context.
     * @details Handles the lifecycle of the graphics subsystem.
     */
    export class GraphicsContext {
    public:
        GraphicsContext();
        ~GraphicsContext();

        /**
         * @brief Initializes GLFW, Vulkan, and ImGui.
         * @param title Window title.
         * @param width Window width.
         * @param height Window height.
         * @return True if initialization was successful.
         */
        bool Initialize(const std::string& title, uint32_t width, uint32_t height);

        /**
         * @brief Cleans up all resources.
         */
        void Shutdown();

        /**
         * @brief Polls events and starts a new frame (Vulkan & ImGui).
         * @return False if the window should close.
         */
        bool BeginFrame();

        /**
         * @brief Renders the frame and presents the swapchain.
         */
        void EndFrame();

    private:
        // Using PIMPL idiom (Pointer to Implementation) or void* // to hide internal Vulkan types from the module interface.
        struct InternalState;
        std::unique_ptr<InternalState> m_State;
    };

}