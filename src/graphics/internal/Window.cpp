module;

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <string>

module vortex.graphics;

import :window;
import vortex.log;

namespace vortex::graphics {

    Window::Window() {}
    Window::~Window() { Shutdown(); }

    void Window::FramebufferResizeCallback(GLFWwindow* window, int width, int height) {
        (void)width; (void)height;
        auto app = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (app) app->wasResized = true;
    }

    bool Window::Initialize(const std::string& title, uint32_t width, uint32_t height) {
        if (!glfwInit()) {
            Log::Error("Failed to initialize GLFW");
            return false;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        m_Window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);

        if (!m_Window) {
            Log::Error("Failed to create GLFW window");
            return false;
        }

        glfwSetWindowUserPointer(m_Window, this);
        glfwSetFramebufferSizeCallback(m_Window, FramebufferResizeCallback);

        Log::Info("Window initialized successfully.");
        return true;
    }

    VkSurfaceKHR Window::CreateSurface(VkInstance instance) {
        VkSurfaceKHR surface;
        if (glfwCreateWindowSurface(instance, m_Window, nullptr, &surface) != VK_SUCCESS) {
            Log::Error("Failed to create window surface");
            return VK_NULL_HANDLE;
        }
        return surface;
    }

    void Window::Shutdown() {
        if (m_Window) {
            glfwDestroyWindow(m_Window);
            m_Window = nullptr;
            glfwTerminate();
        }
    }

    bool Window::ShouldClose() const {
        return glfwWindowShouldClose(m_Window);
    }

    void Window::PollEvents() {
        glfwPollEvents();
    }

    void Window::GetFramebufferSize(int& width, int& height) const {
        glfwGetFramebufferSize(m_Window, &width, &height);
    }
}