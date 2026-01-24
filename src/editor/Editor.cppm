module;

#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

struct GLFWwindow;

export module vortex.editor;

import vortex.graphics;

namespace vortex::editor {

    /**
     * @brief Editor module for object manipulation and selection.
     */
    export class Editor {
    public:
        Editor() = default;
        
        /**
         * @brief Updates editor state (input handling, shortcuts).
         * @param window Raw GLFW window.
         * @param camera Main camera.
         * @param sceneManager Scene manager for object access.
         * @param width Screen width.
         * @param height Screen height.
         */
        void Update(GLFWwindow* window, graphics::Camera& camera, graphics::SceneManager& sceneManager, uint32_t width, uint32_t height);

        /**
         * @brief Renders Gizmos. Must be called inside ImGui frame.
         * @param camera Main camera.
         * @param width Screen width.
         * @param height Screen height.
         */
        void RenderGizmo(graphics::Camera& camera, uint32_t width, uint32_t height);

    private:
        int m_SelectedObjectID = -1;
        ImGuizmo::OPERATION m_CurrentOperation = ImGuizmo::TRANSLATE;
        
        graphics::SceneManager* m_SceneManager = nullptr;

        // Helper for raycasting
        void HandleSelection(GLFWwindow* window, graphics::Camera& camera, uint32_t width, uint32_t height);
    };
}