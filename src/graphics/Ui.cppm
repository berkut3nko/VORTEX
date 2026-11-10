module;

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>

export module vortex.graphics:ui;

import vortex.core;
import :device;
import :window;

export namespace vortex::graphics {

    /**
     * @brief Dear ImGui integration.
     * @details Windowed: GLFW platform backend + Vulkan renderer backend using dynamic
     * rendering (no legacy render pass). Headless (`window == nullptr`): only the ImGui context
     * exists, so game / editor code can issue ImGui calls unconditionally and nothing is drawn.
     *
     * Frame protocol:  BeginFrame() -> (ImGui:: calls) -> EndFrame() -> Draw(cmd) inside the
     * renderer's final colour pass.
     */
    class UiLayer {
    public:
        UiLayer() = default;
        ~UiLayer();
        UiLayer(const UiLayer&) = delete;
        UiLayer& operator=(const UiLayer&) = delete;

        /// @param colorFormat Format of the attachment Draw() renders into (the backbuffer).
        bool Initialize(Device& device, Window* window, VkFormat colorFormat, uint32_t imageCount);
        void Shutdown();
        bool IsInitialized() const { return m_Initialized; }

        /// @brief Starts an ImGui frame. `displaySize` is only used in headless mode.
        void BeginFrame(glm::uvec2 displaySize, float deltaTime);
        /// @brief Finalises the ImGui frame (ImGui::Render). Must be called once per BeginFrame.
        void EndFrame();
        /// @brief Records the UI draw calls. Call inside a dynamic rendering scope that targets the backbuffer.
        void Draw(VkCommandBuffer cmd);

        /// @brief Copies ImGui's capture state into the input object (call after BeginFrame).
        void UpdateInputCapture(Input& input) const;
        bool WantsMouse() const;
        bool WantsKeyboard() const;

    private:
        Device* m_Device = nullptr;
        bool m_Initialized = false;
        bool m_HasBackends = false;
        bool m_FrameOpen = false;
        bool m_HasDrawData = false;
    };
}
