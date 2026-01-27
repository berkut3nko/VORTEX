module;

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>

// DO NOT USE GLM_FORCE_DEPTH_ZERO_TO_ONE HERE
// ImGuizmo expects OpenGL-style clip space (-1 to 1 Z).
// Using Vulkan 0..1 range here breaks ImGuizmo raycasting.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <limits>
#include <algorithm>
#include <memory>

module vortex.editor;

import vortex.log;
import vortex.graphics;
import vortex.voxel;

namespace vortex::editor {

    struct Ray { glm::vec3 origin; glm::vec3 direction; };

    static bool IntersectRayAABB(const Ray& ray, const glm::vec3& min, const glm::vec3& max, float& t) {
        glm::vec3 invDir = 1.0f / ray.direction;
        glm::vec3 tbot = invDir * (min - ray.origin);
        glm::vec3 ttop = invDir * (max - ray.origin);
        glm::vec3 tmin = glm::min(ttop, tbot);
        glm::vec3 tmax = glm::max(ttop, tbot);
        float t0 = glm::max(glm::max(tmin.x, tmin.y), tmin.z);
        float t1 = glm::min(glm::min(tmax.x, tmax.y), tmax.z);
        if (t1 >= t0 && t1 >= 0.0f) { t = t0; return true; }
        return false;
    }

    void Editor::Update(GLFWwindow* window, graphics::Camera& camera, graphics::SceneManager& sceneManager, uint32_t width, uint32_t height) {
        m_SceneManager = &sceneManager;
        if (glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS) m_CurrentOperation = ImGuizmo::TRANSLATE;
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) m_CurrentOperation = ImGuizmo::ROTATE;
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) m_CurrentOperation = ImGuizmo::SCALE;
        
        HandleSelection(window, camera, width, height);
        RenderImporterWindow();
    }

    void Editor::RenderImporterWindow() {
        if (ImGui::Begin("Scene Editor")) {
            if (ImGui::CollapsingHeader("Importer")) {
                ImGui::InputText("Path", m_ImportPathBuffer, sizeof(m_ImportPathBuffer));
                ImGui::DragFloat("Import Scale", &m_ImportScale, 0.1f, 0.1f, 100.0f);
                
                if (ImGui::Button("Load Mesh")) {
                    auto dynObj = std::make_shared<voxel::DynamicMeshObject>();
                    dynObj->name = std::string(m_ImportPathBuffer);
                    dynObj->importSettings.filePath = dynObj->name;
                    dynObj->importSettings.scale = m_ImportScale;
                    
                    Log::Info("Importing: " + dynObj->name);
                    dynObj->Remesh();
                    
                    m_Entities.push_back(dynObj);
                    m_SceneDirty = true;
                }
            }

            ImGui::Separator();
            ImGui::Text("Scene Entities (%zu):", m_Entities.size());
            
            for (size_t i = 0; i < m_Entities.size(); ++i) {
                RenderEntityNode((int)i, m_Entities[i]);
            }
        }
        ImGui::End();
    }

    void Editor::RenderEntityNode(int index, std::shared_ptr<voxel::VoxelEntity> entity) {
        ImGui::PushID(index);
        bool isSelected = (m_SelectedObjectID == index);
        
        if (ImGui::Selectable(entity->name.c_str(), isSelected)) {
            m_SelectedObjectID = index;
        }

        if (isSelected && ImGui::TreeNode("Details")) {
            auto dynMesh = std::dynamic_pointer_cast<voxel::DynamicMeshObject>(entity);
            if (dynMesh) {
                ImGui::TextColored(ImVec4(0.5f,1.0f,0.5f,1.0f), "[Imported Mesh]");
                if (ImGui::DragFloat("Re-Import Scale", &dynMesh->importSettings.scale, 0.1f, 0.1f, 100.0f)) {}
                if (ImGui::Button("Re-Mesh")) {
                    dynMesh->Remesh();
                    m_SceneDirty = true;
                }
            } else {
                 ImGui::TextColored(ImVec4(0.5f,0.8f,1.0f,1.0f), "[Procedural/Generic]");
            }

            ImGui::Text("Parts: %zu", entity->parts.size());
            ImGui::Text("Voxels: %u", entity->totalVoxelCount);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void Editor::HandleSelection(GLFWwindow* window, graphics::Camera& camera, uint32_t width, uint32_t height) {
        auto& io = ImGui::GetIO();
        if (io.WantCaptureMouse || ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return;

        bool isPressed = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        if (isPressed && !m_LeftMouseWasPressed) {
            double mouseX, mouseY;
            glfwGetCursorPos(window, &mouseX, &mouseY);
            
            glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
            // Increased Z-Far to 400.0f to match renderer
            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, 0.5f, 400.0f);
            
            float x = (2.0f * (float)mouseX) / width - 1.0f;
            float y = 1.0f - (2.0f * (float)mouseY) / height;
            glm::vec4 rayClip = glm::vec4(x, y, -1.0, 1.0);
            glm::vec4 rayEye = glm::inverse(proj) * rayClip;
            rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0, 0.0);
            glm::vec3 rayWor = glm::normalize(glm::vec3(glm::inverse(view) * rayEye));

            Ray ray{ camera.position, rayWor };
            float minT = std::numeric_limits<float>::max();
            int hitIndex = -1;

            for (size_t i = 0; i < m_Entities.size(); ++i) {
                auto& entity = m_Entities[i];
                glm::mat4 model = entity->transform;
                glm::mat4 invModel = glm::inverse(model);

                glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(ray.origin, 1.0f));
                glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(ray.direction, 0.0f)));
                Ray localRay{ localOrigin, localDir };

                float t = 0.0f;
                if (IntersectRayAABB(localRay, entity->localBoundsMin, entity->localBoundsMax, t)) {
                    glm::vec3 hitWorld = glm::vec3(model * glm::vec4(localOrigin + localDir * t, 1.0f));
                    float dist = glm::distance(camera.position, hitWorld);
                    if (dist < minT) {
                        minT = dist;
                        hitIndex = (int)i;
                    }
                }
            }

            if (hitIndex != -1) {
                if(m_SelectedObjectID != hitIndex){
                    m_SelectedObjectID = hitIndex;
                    Log::Info("Selected Entity ID: " + std::to_string(m_SelectedObjectID));
                }
                else
                    m_SelectedObjectID = -1;
            }
        }
        m_LeftMouseWasPressed = isPressed;
    }

    void Editor::RenderGizmo(graphics::Camera& camera, uint32_t width, uint32_t height) {
        if (m_SelectedObjectID == -1 || m_Entities.empty()) return;
        if (m_SelectedObjectID >= (int)m_Entities.size()) { m_SelectedObjectID = -1; return; }

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(0, 0, (float)width, (float)height);

        glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
        glm::mat4 proj = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, 0.5f, 400.0f);
        
        // Fix for Vulkan Y-flip
        proj[1][1] *= -1; 

        auto& entity = m_Entities[m_SelectedObjectID];
        glm::mat4 model = entity->transform;
        
        glm::vec3 center = entity->GetLocalCenter();
        glm::mat4 visualMatrix = glm::translate(model, center);

        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), 
                             m_CurrentOperation, ImGuizmo::WORLD, 
                             glm::value_ptr(visualMatrix));

        if (ImGuizmo::IsUsing()) {
             entity->transform = glm::translate(visualMatrix, -center);
             m_SceneDirty = true;
        }
    }
}