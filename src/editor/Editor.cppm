module;

#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <vector>
#include <memory> 

struct GLFWwindow;

export module vortex.editor;

import vortex.graphics;
import vortex.voxel; 

namespace vortex::editor {

    /**
     * @brief Defines the active tool in the editor.
     */
    export enum class ToolMode {
        Select, ///< Standard Gizmo manipulation.
        Brush   ///< Voxel painting/erasing.
    };

    /**
     * @brief The in-game editor system.
     * @details Handles object selection, Gizmo manipulation, the Importer UI, and Voxel Tools.
     */
    export class Editor {
    public:
        Editor() = default;
        
        /**
         * @brief Updates editor logic, input handling, and UI rendering.
         * @param window Input window.
         * @param camera Active camera.
         * @param sceneManager Reference to the scene manager for raycasting/updates.
         * @param width Viewport width.
         * @param height Viewport height.
         */
        void Update(GLFWwindow* window, graphics::Camera& camera, graphics::SceneManager& sceneManager, uint32_t width, uint32_t height);
        
        /**
         * @brief Renders the 3D Gizmo for the selected entity.
         */
        void RenderGizmo(graphics::Camera& camera, uint32_t width, uint32_t height);

        std::vector<std::shared_ptr<voxel::VoxelEntity>>& GetEntities() { return m_Entities; }

        bool IsSceneDirty() const { return m_SceneDirty; }
        void ResetSceneDirty() { m_SceneDirty = false; }
        void MarkDirty() { m_SceneDirty = true; }

        /**
         * @brief Returns the currently selected entity, or nullptr if none.
         */
        std::shared_ptr<voxel::VoxelEntity> GetSelectedEntity() const;

        /**
         * @brief Returns and clears the list of entities created by the Editor (e.g. Importer).
         */
        std::vector<std::shared_ptr<voxel::VoxelEntity>> ConsumeCreatedEntities() {
            auto result = std::move(m_CreatedEntities);
            m_CreatedEntities.clear();
            return result;
        }

    private:
        bool m_LeftMouseWasPressed = false;
        int m_SelectedObjectID = -1;
        ImGuizmo::OPERATION m_CurrentOperation = ImGuizmo::TRANSLATE;
        
        graphics::SceneManager* m_SceneManager = nullptr;
        std::vector<std::shared_ptr<voxel::VoxelEntity>> m_Entities;
        std::vector<std::shared_ptr<voxel::VoxelEntity>> m_CreatedEntities;
        
        char m_ImportPathBuffer[256] = "assets/models/frank.glb";
        float m_ImportScale = 10.0f;
        bool m_SceneDirty = false;

        // --- Tools State ---
        ToolMode m_CurrentTool = ToolMode::Select;
        int m_BrushMaterialID = 1;     ///< 0 = Eraser (Air).
        int m_BrushSize = 0;           ///< Radius/Extents (0 = 1x1x1).
        bool m_BrushIsSphere = false;  ///< Box vs Sphere shape.

        // --- Internal Helpers ---
        void HandleInput(GLFWwindow* window, graphics::Camera& camera, uint32_t width, uint32_t height);
        void HandleSelection(const glm::vec3& rayOrigin, const glm::vec3& rayDir);
        void HandleBrushAction(const glm::vec3& rayOrigin, const glm::vec3& rayDir);
        
        void RenderUI();
        void RenderEntityNode(int index, std::shared_ptr<voxel::VoxelEntity> entity);
    };
}