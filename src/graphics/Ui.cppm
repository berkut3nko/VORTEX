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
        /// With input disabled both capture flags are false.
        void UpdateInputCapture(Input& input) const;
        /**
         * @brief Whether ImGui gets the mouse at all (default true). Disabled: ImGui ignores
         * the mouse and leaves the cursor shape alone (ImGuiConfigFlags_NoMouse |
         * NoMouseCursorChange) and UpdateInputCapture never hides input from the game. Play mode
         * disables it so a hidden or locked cursor can neither hover a window nor have its
         * keys swallowed. While disabled the flags are re-applied at every BeginFrame; enabling
         * clears only the bits this function added, so the editor's own NoMouse (right-button
         * look) is neither removed by an enable nor kept by a disable.
         */
        void SetInputEnabled(bool enabled);
        bool InputEnabled() const { return m_InputEnabled; }
        bool WantsMouse() const;
        bool WantsKeyboard() const;

    private:
        Device* m_Device = nullptr;
        bool m_Initialized = false;
        bool m_HasBackends = false;
        bool m_FrameOpen = false;
        bool m_HasDrawData = false;
        void ApplyInputFlags();

        bool m_InputEnabled = true;
        int m_AddedFlags = 0;      ///< ImGuiConfigFlags bits SetInputEnabled(false) set and must clear
    };
}
