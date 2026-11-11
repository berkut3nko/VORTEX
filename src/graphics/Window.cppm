module;

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

export module vortex.graphics:window;

import vortex.core;

export namespace vortex::graphics {

    struct WindowConfig {
        std::string title = "VORTEX";
        uint32_t width = 1280;
        uint32_t height = 720;
        bool resizable = true;
    };

    /**
     * @brief Operating system window (GLFW) and the source of input events.
     * @details Keyboard / mouse callbacks are installed here and forwarded into the Input object
     * passed to Initialize(); the UI layer chains its own callbacks after ours. Nothing else in
     * the engine talks to GLFW.
     */
    class Window {
    public:
        Window() = default;
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        /// @param input Receives key / button / cursor / scroll events. Must outlive the window.
        bool Initialize(const WindowConfig& config, Input* input);
        void Shutdown();

        GLFWwindow* Native() const { return m_Window; }
        /// @brief Pumps the event queue (calls Input::NewFrame() first).
        void PollEvents();
        bool ShouldClose() const;
        void RequestClose();

        /// @brief Framebuffer size in pixels (0,0 while minimised).
        glm::ivec2 FramebufferSize() const;
        /// @brief True once after the framebuffer size changed.
        bool ConsumeResized();
        bool IsMinimized() const;

        /// @brief Window size in screen coordinates (the unit of Input::MouseX/Y); differs from
        /// FramebufferSize() on scaled (HiDPI) outputs.
        glm::ivec2 WindowSize() const;
        /// @brief The window has the keyboard focus.
        bool IsFocused() const;
        /// @brief The pointer is over the content area (GLFW_HOVERED). On Wayland a pointer
        /// lock only starts once this is true.
        bool IsHovered() const;

        /**
         * @brief Hides the cursor and delivers unbounded relative motion (mouse look):
         * GLFW_CURSOR_DISABLED plus raw (unaccelerated) motion where supported. On Wayland GLFW
         * locks the pointer (pointer-constraints + relative-pointer); the lock starts when the
         * pointer is over the window. On X11 the pointer is grabbed and warped to the centre.
         * Motion arrives through the cursor callback as a virtual position: use
         * Input::MouseDelta*, never absolute positions. The delta is reset on every change.
         */
        void SetCursorCaptured(bool captured);
        bool IsCursorCaptured() const { return m_CursorCaptured; }
        void SetTitle(const std::string& title);

        /// @brief Instance extensions GLFW needs to create a surface on this platform.
        static std::vector<const char*> RequiredInstanceExtensions();
        /// @brief Creates the one and only surface of this window. The caller owns it.
        VkSurfaceKHR CreateSurface(VkInstance instance) const;

    private:
        GLFWwindow* m_Window = nullptr;
        Input* m_Input = nullptr;
        bool m_Resized = false;
        bool m_CursorCaptured = false;
        bool m_CaptureLogged = false;
        bool m_OwnsGlfw = false;
    };
}
