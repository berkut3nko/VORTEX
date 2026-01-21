module;

#include <string>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

export module vortex.graphics:window;

namespace vortex::graphics {

    /**
     * @brief Manages the GLFW window and Vulkan Surface creation.
     */
    export class Window {
    public:
        Window();
        ~Window();

        /**
         * @brief Initializes GLFW and creates the window.
         * @param title Window title.
         * @param width Window width.
         * @param height Window height.
         * @return True if successful.
         */
        bool Initialize(const std::string& title, uint32_t width, uint32_t height);

        /**
         * @brief Destroys the window and terminates GLFW.
         */
        void Shutdown();

        /**
         * @brief Creates a Vulkan surface for this window.
         * @param instance The Vulkan instance.
         * @return The created surface handle.
         */
        VkSurfaceKHR CreateSurface(VkInstance instance);

        /**
         * @brief Checks if the window should close.
         */
        bool ShouldClose() const;

        /**
         * @brief Polls input events.
         */
        void PollEvents();

        /**
         * @brief Gets the raw GLFW window handle.
         */
        GLFWwindow* GetNativeHandle() const { return m_Window; }

        /**
         * @brief Gets current framebuffer size.
         */
        void GetFramebufferSize(int& width, int& height) const;

        bool wasResized = false;

    private:
        GLFWwindow* m_Window{nullptr};
        
        static void FramebufferResizeCallback(GLFWwindow* window, int width, int height);
    };
}