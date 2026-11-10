module;

// vulkan.h must come before glfw3.h so that glfwCreateWindowSurface is declared.
#include <vulkan/vulkan.h>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

module vortex.graphics;

import vortex.core;

namespace vortex::graphics {

    namespace {

        /// Number of live users of the GLFW library (windows, plus temporary users). Main thread only.
        int g_GlfwUsers = 0;

        void OnGlfwError(int code, const char* description) {
            Log::Error("GLFW error {}: {}", code, description != nullptr ? description : "(no description)");
        }

        /// @brief Initialises GLFW on first use. Every successful call must be paired with ReleaseGlfw().
        bool AcquireGlfw() {
            if (g_GlfwUsers == 0) {
                glfwSetErrorCallback(OnGlfwError);
                if (glfwInit() != GLFW_TRUE) {
                    Log::Error("Failed to initialise GLFW");
                    return false;
                }
            }
            ++g_GlfwUsers;
            return true;
        }

        void ReleaseGlfw() {
            if (g_GlfwUsers <= 0) return;
            if (--g_GlfwUsers == 0) glfwTerminate();
        }

        int ToWindowDimension(uint32_t value) {
            // glfwCreateWindow requires strictly positive sizes that fit an int.
            return static_cast<int>(std::clamp<uint32_t>(value, 1u, 16384u));
        }
    }

    Window::~Window() {
        Shutdown();
    }

    bool Window::Initialize(const WindowConfig& config, Input* input) {
        if (m_Window != nullptr) {
            Log::Error("Window::Initialize called on a window that is already initialised");
            return false;
        }
        if (!AcquireGlfw()) return false;
        m_OwnsGlfw = true;

        if (glfwVulkanSupported() != GLFW_TRUE) {
            Log::Error("Cannot create window '{}': no Vulkan loader / driver found", config.title);
            Shutdown();
            return false;
        }

        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);

        m_Window = glfwCreateWindow(ToWindowDimension(config.width), ToWindowDimension(config.height),
                                    config.title.c_str(), nullptr, nullptr);
        if (m_Window == nullptr) {
            Log::Error("Failed to create window '{}' ({}x{})", config.title, config.width, config.height);
            Shutdown();
            return false;
        }

        m_Input = input;
        m_Resized = false;
        m_CursorCaptured = false;
        glfwSetWindowUserPointer(m_Window, this);

        // Lambdas defined in a member function may touch private members, which keeps the
        // callbacks out of the public interface.
        glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
            if (self == nullptr || self->m_Input == nullptr) return;
            if (action == GLFW_PRESS) self->m_Input->OnKey(key, true);
            else if (action == GLFW_RELEASE) self->m_Input->OnKey(key, false);
            // GLFW_REPEAT does not change the key state.
        });
        glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int /*mods*/) {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
            if (self == nullptr || self->m_Input == nullptr) return;
            self->m_Input->OnMouseButton(button, action != GLFW_RELEASE);
        });
        glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double x, double y) {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
            if (self == nullptr || self->m_Input == nullptr) return;
            self->m_Input->OnCursor(x, y);
        });
        glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double /*xOffset*/, double yOffset) {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
            if (self == nullptr || self->m_Input == nullptr) return;
            self->m_Input->OnScroll(yOffset);
        });
        glfwSetFramebufferSizeCallback(m_Window, [](GLFWwindow* window, int /*width*/, int /*height*/) {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
            if (self != nullptr) self->m_Resized = true;
        });
        glfwSetWindowFocusCallback(m_Window, [](GLFWwindow* window, int focused) {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
            if (self == nullptr || self->m_Input == nullptr) return;
            if (focused == GLFW_FALSE) self->m_Input->Clear();
        });

        const glm::ivec2 size = FramebufferSize();
        Log::Info("Window '{}' created, framebuffer {}x{}", config.title, size.x, size.y);
        return true;
    }

    void Window::Shutdown() {
        if (m_Window != nullptr) {
            glfwDestroyWindow(m_Window);
            m_Window = nullptr;
        }
        if (m_OwnsGlfw) {
            ReleaseGlfw();
            m_OwnsGlfw = false;
        }
        m_Input = nullptr;
        m_Resized = false;
        m_CursorCaptured = false;
    }

    void Window::PollEvents() {
        if (m_Input != nullptr) m_Input->NewFrame();
        if (m_Window != nullptr) glfwPollEvents();
    }

    bool Window::ShouldClose() const {
        return m_Window == nullptr || glfwWindowShouldClose(m_Window) != GLFW_FALSE;
    }

    void Window::RequestClose() {
        if (m_Window != nullptr) glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
    }

    glm::ivec2 Window::FramebufferSize() const {
        if (m_Window == nullptr) return glm::ivec2(0, 0);
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(m_Window, &width, &height);
        return glm::ivec2(std::max(width, 0), std::max(height, 0));
    }

    bool Window::ConsumeResized() {
        const bool resized = m_Resized;
        m_Resized = false;
        return resized;
    }

    bool Window::IsMinimized() const {
        if (m_Window == nullptr) return true;
        if (glfwGetWindowAttrib(m_Window, GLFW_ICONIFIED) != GLFW_FALSE) return true;
        const glm::ivec2 size = FramebufferSize();
        return size.x == 0 || size.y == 0;
    }

    void Window::SetCursorCaptured(bool captured) {
        if (m_Window == nullptr || captured == m_CursorCaptured) return;

        glfwSetInputMode(m_Window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
            glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
        }
        m_CursorCaptured = captured;

        // The cursor position jumps when the mode changes; do not report that as motion.
        if (m_Input != nullptr) m_Input->ResetCursorDelta();
    }

    void Window::SetTitle(const std::string& title) {
        if (m_Window != nullptr) glfwSetWindowTitle(m_Window, title.c_str());
    }

    std::vector<const char*> Window::RequiredInstanceExtensions() {
        // GLFW owns the strings it returns and frees them in glfwTerminate(). They are copied
        // once so the returned pointers stay valid for the lifetime of the process, whether or
        // not a window is alive.
        static std::vector<std::string> names;
        static bool queried = false;

        if (!queried) {
            if (!AcquireGlfw()) return {};
            uint32_t count = 0;
            const char** extensions = glfwGetRequiredInstanceExtensions(&count);
            if (extensions != nullptr) {
                names.reserve(count);
                for (uint32_t i = 0; i < count; ++i) names.emplace_back(extensions[i]);
                queried = true;
            } else {
                Log::Error("GLFW cannot create Vulkan surfaces on this system (no required instance extensions)");
            }
            ReleaseGlfw();
        }

        std::vector<const char*> result;
        result.reserve(names.size());
        for (const std::string& name : names) result.push_back(name.c_str());
        return result;
    }

    VkSurfaceKHR Window::CreateSurface(VkInstance instance) const {
        if (m_Window == nullptr || instance == VK_NULL_HANDLE) {
            Log::Error("Window::CreateSurface needs an initialised window and a Vulkan instance");
            return VK_NULL_HANDLE;
        }
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        const VkResult result = glfwCreateWindowSurface(instance, m_Window, nullptr, &surface);
        if (result != VK_SUCCESS) {
            Log::Error("Failed to create the window surface (VkResult {})", static_cast<int>(result));
            return VK_NULL_HANDLE;
        }
        return surface;
    }
}
