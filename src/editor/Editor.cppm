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

    export class Editor {
    public:
        Editor() = default;
        
        void Update(GLFWwindow* window, graphics::Camera& camera, graphics::SceneManager& sceneManager, uint32_t width, uint32_t height);
        void RenderGizmo(graphics::Camera& camera, uint32_t width, uint32_t height);

        std::vector<std::shared_ptr<voxel::VoxelEntity>>& GetEntities() { return m_Entities; }

        bool IsSceneDirty() const { return m_SceneDirty; }
        void ResetSceneDirty() { m_SceneDirty = false; }
        void MarkDirty() { m_SceneDirty = true; }

        std::shared_ptr<voxel::VoxelEntity> GetSelectedEntity() const;

        /**
         * @brief Returns and clears the list of entities created by the Editor (e.g. Importer).
         * @details These entities need to be officially registered with the Engine to have physics.
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
        
        // Queue for new entities created via UI (Importer)
        std::vector<std::shared_ptr<voxel::VoxelEntity>> m_CreatedEntities;
        
        char m_ImportPathBuffer[256] = "assets/models/frank.glb";
        float m_ImportScale = 10.0f;
        
        bool m_SceneDirty = false;

        void HandleSelection(GLFWwindow* window, graphics::Camera& camera, uint32_t width, uint32_t height);
        void RenderImporterWindow();
        void RenderEntityNode(int index, std::shared_ptr<voxel::VoxelEntity> entity);
    };
}