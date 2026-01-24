module;

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <limits>

module vortex.editor;

import vortex.log;
import vortex.graphics;

namespace vortex::editor {

    // --- Raycasting Helpers ---
    struct Ray {
        glm::vec3 origin;
        glm::vec3 direction;
    };

    bool IntersectRayAABB(const Ray& ray, const glm::vec3& min, const glm::vec3& max, float& t) {
        glm::vec3 invDir = 1.0f / ray.direction;
        glm::vec3 tbot = invDir * (min - ray.origin);
        glm::vec3 ttop = invDir * (max - ray.origin);

        glm::vec3 tmin = glm::min(ttop, tbot);
        glm::vec3 tmax = glm::max(ttop, tbot);

        float t0 = glm::max(glm::max(tmin.x, tmin.y), tmin.z);
        float t1 = glm::min(glm::min(tmax.x, tmax.y), tmax.z);

        if (t1 >= t0 && t1 >= 0.0f) {
            t = t0;
            return true;
        }
        return false;
    }

    void Editor::Update(GLFWwindow* window, graphics::Camera& camera, graphics::SceneManager& sceneManager, uint32_t width, uint32_t height) {
        m_SceneManager = &sceneManager;

        // Toggle Gizmo Mode
        if (glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS) m_CurrentOperation = ImGuizmo::TRANSLATE;
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) m_CurrentOperation = ImGuizmo::ROTATE;

        // Handle Object Selection
        HandleSelection(window, camera, width, height);
    }

    void Editor::HandleSelection(GLFWwindow* window, graphics::Camera& camera, uint32_t width, uint32_t height) {
        auto& io = ImGui::GetIO();
        
        // 1. Skip if interacting with ImGui (GUI or Gizmo)
        if (io.WantCaptureMouse || ImGuizmo::IsOver() || ImGuizmo::IsUsing()) {
            return;
        }

        // 2. Check Left Mouse Click
        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
            
            // Get Mouse Position
            double mouseX, mouseY;
            glfwGetCursorPos(window, &mouseX, &mouseY);

            // Construct Ray
            glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, 0.1f, 100.0f);
            
            // NDC
            float x = (2.0f * (float)mouseX) / width - 1.0f;
            float y = 1.0f - (2.0f * (float)mouseY) / height;
            float z = 1.0f;
            
            glm::vec3 rayNds = glm::vec3(x, y, z);
            glm::vec4 rayClip = glm::vec4(rayNds.x, rayNds.y, -1.0, 1.0);
            glm::vec4 rayEye = glm::inverse(proj) * rayClip;
            rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0, 0.0);
            glm::vec3 rayWor = glm::vec3(glm::inverse(view) * rayEye);
            rayWor = glm::normalize(rayWor);

            Ray ray{ camera.position, rayWor };

            // Find closest intersection
            float minT = std::numeric_limits<float>::max();
            int hitIndex = -1;

            auto& objects = m_SceneManager->GetObjects();
            
            for (size_t i = 0; i < objects.size(); ++i) {
                // Transform Ray to Local Space (Inverse Model Matrix)
                glm::mat4 invModel = glm::inverse(objects[i].model);
                
                glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(ray.origin, 1.0f));
                glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(ray.direction, 0.0f)));
                Ray localRay{ localOrigin, localDir };

                // Assume Chunk AABB is (0,0,0) to (32,32,32)
                float t = 0.0f;
                if (IntersectRayAABB(localRay, glm::vec3(0.0f), glm::vec3(32.0f), t)) {
                    // t is distance in local space, approximate global distance check
                    glm::vec3 hitPointLocal = localOrigin + localDir * t;
                    glm::vec3 hitPointWorld = glm::vec3(objects[i].model * glm::vec4(hitPointLocal, 1.0f));
                    float dist = glm::distance(camera.position, hitPointWorld);

                    if (dist < minT) {
                        minT = dist;
                        hitIndex = (int)i;
                    }
                }
            }

            if (hitIndex != -1) {
                m_SelectedObjectID = hitIndex;
                Log::Info("Selected Object ID: " + std::to_string(m_SelectedObjectID));
            }
        }
    }

    void Editor::RenderGizmo(graphics::Camera& camera, uint32_t width, uint32_t height) {
        if (m_SelectedObjectID == -1 || !m_SceneManager) return;

        auto& objects = m_SceneManager->GetObjects();
        
        if (m_SelectedObjectID < 0 || static_cast<size_t>(m_SelectedObjectID) >= objects.size()) {
            m_SelectedObjectID = -1;
            return;
        }

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(0, 0, (float)width, (float)height);

        // Matrices
        glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
        glm::mat4 proj = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, 0.1f, 100.0f);
        
        // IMPORTANT: ImGuizmo expects standard OpenGL projection (Y-Up).
        // Since we removed the manual flip, ensure your view/proj logic matches what ImGuizmo expects.
        // If your engine uses Vulkan Y-Down natively in 'proj', you might need to flip specifically for ImGuizmo
        // BUT typically leaving it standard GLM (Y-Up) works best with ImGuizmo.

        auto& obj = objects[m_SelectedObjectID];
        glm::mat4 model = obj.model;

        // --- Logic Center Offset ---
        // 1. Move the pivot to the logical center
        glm::vec3 center = obj.logicalCenter;
        
        // Transform the model matrix so that the Gizmo appears at the logical center
        // T_visual = T_model * T_center
        glm::mat4 visualMatrix = glm::translate(model, center);

        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), 
                             m_CurrentOperation, ImGuizmo::WORLD, 
                             glm::value_ptr(visualMatrix));

        // Update if changed
        if (ImGuizmo::IsUsing()) {
             // 2. Apply the inverse offset to get back the Chunk Origin transform
             // T_new_model = T_new_visual * T_inverse_center
             glm::mat4 newModel = glm::translate(visualMatrix, -center);
             
             m_SceneManager->SetObjectTransform(m_SelectedObjectID, newModel);
        }
    }
}