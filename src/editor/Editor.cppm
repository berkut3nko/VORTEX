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
     * @brief Editor module for object manipulation and selection.
     */
    export class Editor {
    public:
        Editor() = default;
        
        void Update(GLFWwindow* window, graphics::Camera& camera, graphics::SceneManager& sceneManager, uint32_t width, uint32_t height);
        void RenderGizmo(graphics::Camera& camera, uint32_t width, uint32_t height);

        // Access to entities for Engine sync
        std::vector<std::shared_ptr<voxel::VoxelEntity>>& GetEntities() { return m_Entities; }

        // Dirty flag management
        bool IsSceneDirty() const { return m_SceneDirty; }
        void ResetSceneDirty() { m_SceneDirty = false; }
        void MarkDirty() { m_SceneDirty = true; }

    private:
        bool m_LeftMouseWasPressed = false;
        int m_SelectedObjectID = -1;
        ImGuizmo::OPERATION m_CurrentOperation = ImGuizmo::TRANSLATE;
        
        graphics::SceneManager* m_SceneManager = nullptr;
        
        // Entity List
        std::vector<std::shared_ptr<voxel::VoxelEntity>> m_Entities;
        
        // UI State
        char m_ImportPathBuffer[256] = "assets/models/example.glb";
        float m_ImportScale = 10.0f;
        
        bool m_SceneDirty = false;

        // Internal methods
        void HandleSelection(GLFWwindow* window, graphics::Camera& camera, uint32_t width, uint32_t height);
        void RenderImporterWindow();
        void RenderEntityNode(int index, std::shared_ptr<voxel::VoxelEntity> entity);
    };
}