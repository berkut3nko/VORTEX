module;

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>

// Critical: Use same depth range 0..1 as Vulkan
#define GLM_FORCE_DEPTH_ZERO_TO_ONE 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <limits>
#include <algorithm>
#include <memory>
#include <cmath>

module vortex.editor;

import vortex.log;
import vortex.graphics;
import vortex.voxel;

namespace vortex::editor {

    struct Ray { glm::vec3 origin; glm::vec3 direction; };

    // Покращена версія AABB перетину, що повертає точки входу та виходу
    static bool IntersectRayAABB(const Ray& ray, const glm::vec3& min, const glm::vec3& max, float& tNear, float& tFar) {
        glm::vec3 invDir = 1.0f / ray.direction;
        glm::vec3 tbot = invDir * (min - ray.origin);
        glm::vec3 ttop = invDir * (max - ray.origin);
        glm::vec3 tmin = glm::min(ttop, tbot);
        glm::vec3 tmax = glm::max(ttop, tbot);
        tNear = glm::max(glm::max(tmin.x, tmin.y), tmin.z);
        tFar = glm::min(glm::min(tmax.x, tmax.y), tmax.z);
        return tFar >= tNear && tFar > 0.0f;
    }

    // CPU реалізація DDA (як у шейдері) для точного виділення вокселів
    static bool RaycastDDA(const Ray& ray, const vortex::voxel::Chunk& chunk, float& tHit) {
        float tNear, tFar;
        // Перевіряємо межі чанка (0..32)
        if (!IntersectRayAABB(ray, glm::vec3(0.0f), glm::vec3(32.0f), tNear, tFar)) return false;

        float tCurrent = std::max(0.0f, tNear);
        
        // Починаємо трохи всередині, щоб уникнути похибок на гранях
        glm::vec3 rayPos = ray.origin + ray.direction * (tCurrent + 0.001f);
        glm::ivec3 mapPos = glm::ivec3(glm::floor(rayPos));
        
        // Clamp координат, щоб не вийти за межі масиву при старті
        mapPos = glm::clamp(mapPos, glm::ivec3(0), glm::ivec3(31));

        // Безпечний напрямок (уникнення ділення на 0)
        glm::vec3 safeDir = ray.direction;
        if (std::abs(safeDir.x) < 1e-6) safeDir.x = 1e-6f;
        if (std::abs(safeDir.y) < 1e-6) safeDir.y = 1e-6f;
        if (std::abs(safeDir.z) < 1e-6) safeDir.z = 1e-6f;

        glm::vec3 deltaDist = glm::abs(1.0f / safeDir);
        glm::ivec3 stepDir = glm::ivec3(glm::sign(safeDir));
        glm::vec3 sideDist;

        // Розрахунок початкових sideDist відносно точки входу
        glm::vec3 origin = ray.origin + ray.direction * tCurrent;
        for (int i = 0; i < 3; i++) {
            if (safeDir[i] < 0) {
                stepDir[i] = -1;
                sideDist[i] = (origin[i] - mapPos[i]) * deltaDist[i];
            } else {
                stepDir[i] = 1;
                sideDist[i] = (mapPos[i] + 1.0f - origin[i]) * deltaDist[i];
            }
        }

        // DDA Цикл
        for (int i = 0; i < 128; i++) {
            // Перевірка вокселя
            if (chunk.GetVoxel(mapPos.x, mapPos.y, mapPos.z) != 0) {
                // Знайдено воксель!
                // tHit вже містить відстань до входу в цей воксель (приблизно)
                // Для точності додамо відстань, пройдену всередині чанка
                tHit = tCurrent; 
                return true;
            }

            // Крок вперед
            if (sideDist.x < sideDist.y) {
                if (sideDist.x < sideDist.z) {
                    tCurrent += (sideDist.x - (sideDist.x - deltaDist.x)); // Update t (не ідеально точно, але достатньо для сортування)
                    sideDist.x += deltaDist.x;
                    mapPos.x += stepDir.x;
                } else {
                    tCurrent += (sideDist.z - (sideDist.z - deltaDist.z));
                    sideDist.z += deltaDist.z;
                    mapPos.z += stepDir.z;
                }
            } else {
                if (sideDist.y < sideDist.z) {
                    tCurrent += (sideDist.y - (sideDist.y - deltaDist.y));
                    sideDist.y += deltaDist.y;
                    mapPos.y += stepDir.y;
                } else {
                    tCurrent += (sideDist.z - (sideDist.z - deltaDist.z));
                    sideDist.z += deltaDist.z;
                    mapPos.z += stepDir.z;
                }
            }
            
            // Вихід за межі чанка
            if (mapPos.x < 0 || mapPos.x >= 32 || mapPos.y < 0 || mapPos.y >= 32 || mapPos.z < 0 || mapPos.z >= 32) return false;
        }
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

    std::shared_ptr<voxel::VoxelEntity> Editor::GetSelectedEntity() const {
        if (m_SelectedObjectID >= 0 && m_SelectedObjectID < (int)m_Entities.size()) {
            return m_Entities[m_SelectedObjectID];
        }
        return nullptr;
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
                    m_CreatedEntities.push_back(dynObj);
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
            if(m_SelectedObjectID != index)
                m_SelectedObjectID = index;
            else
                m_SelectedObjectID = -1; 
        }

        if (isSelected) {
            ImGui::Indent();
            ImGui::TextDisabled("Physics Properties");
            
            // Physics toggles
            ImGui::Checkbox("Is Static (Bedrock)", &entity->isStatic);
            ImGui::Checkbox("Is Trigger (Sensor)", &entity->isTrigger);
            ImGui::Spacing();

            auto dynMesh = std::dynamic_pointer_cast<voxel::DynamicMeshObject>(entity);
            if (dynMesh) {
                ImGui::TextColored(ImVec4(0.5f,1.0f,0.5f,1.0f), "[Imported Mesh]");
                if (ImGui::DragFloat("Re-Import Scale", &dynMesh->importSettings.scale, 0.1f, 0.1f, 100.0f)) {}
                
                // --- RE-MESH LOGIC ---
                if (ImGui::Button("Re-Mesh & Rebuild Physics")) {
                    dynMesh->Remesh();
                    m_SceneDirty = true; 
                    entity->shouldRebuildPhysics = true; 
                }
            } else {
                 ImGui::TextColored(ImVec4(0.5f,0.8f,1.0f,1.0f), "[Procedural/Generic]");
            }

            ImGui::Text("Parts: %zu", entity->parts.size());
            ImGui::Text("Voxels: %u", entity->totalVoxelCount);
            ImGui::Unindent();
            ImGui::Separator();
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
            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, 0.1f, 400.0f);
            proj[1][1] *= -1; // FIX: Flip Y for Vulkan projection
            
            // Convert Mouse Coordinates to Vulkan Clip Space
            // Vulkan Clip: (-1,-1) is Top-Left, (1,1) is Bottom-Right.
            // Mouse (0,0) is Top-Left.
            float x = (2.0f * (float)mouseX) / width - 1.0f;
            float y = (2.0f * (float)mouseY) / height - 1.0f; // Maps 0 to -1 (Top)

            glm::vec4 rayClip = glm::vec4(x, y, 1.0, 1.0); // Point on Far plane
            glm::vec4 rayEye = glm::inverse(proj) * rayClip;
            rayEye = glm::vec4(rayEye.x, rayEye.y, rayEye.z, 0.0);
            glm::vec3 rayWor = glm::normalize(glm::vec3(glm::inverse(view) * rayEye));

            Ray ray{ camera.position, rayWor };
            float minT = std::numeric_limits<float>::max();
            int hitIndex = -1;

            // Iterate all entities
            for (size_t i = 0; i < m_Entities.size(); ++i) {
                auto& entity = m_Entities[i];
                glm::mat4 model = entity->transform;
                glm::mat4 invModel = glm::inverse(model);

                // Ray to Entity Local Space
                glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(ray.origin, 1.0f));
                glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(ray.direction, 0.0f)));
                Ray localRay{ localOrigin, localDir };

                float tBroadNear, tBroadFar;
                // Broad Phase: Entity AABB
                if (IntersectRayAABB(localRay, entity->localBoundsMin, entity->localBoundsMax, tBroadNear, tBroadFar)) {
                    
                    // Narrow Phase: Check individual Parts (Chunks) with DDA
                    for (const auto& part : entity->parts) {
                        if (!part->chunk) continue;

                        // Ray to Part Local Space (Chunk Space 0..32)
                        // Part transform is relative to Entity
                        glm::mat4 partModel = part->GetTransformMatrix();
                        glm::mat4 partInv = glm::inverse(partModel);

                        glm::vec3 partOrigin = glm::vec3(partInv * glm::vec4(localOrigin, 1.0f));
                        glm::vec3 partDir = glm::normalize(glm::vec3(partInv * glm::vec4(localDir, 0.0f)));
                        Ray partRay{ partOrigin, partDir };

                        float hitT = 0.0f;
                        if (RaycastDDA(partRay, *part->chunk, hitT)) {
                            // Calculate actual world distance
                            // hitT is distance in Chunk Space. We need to map it back to World to compare minT properly.
                            glm::vec3 hitPointChunk = partOrigin + partDir * hitT;
                            glm::vec3 hitPointEntity = glm::vec3(partModel * glm::vec4(hitPointChunk, 1.0f));
                            glm::vec3 hitPointWorld = glm::vec3(model * glm::vec4(hitPointEntity, 1.0f));

                            float dist = glm::distance(ray.origin, hitPointWorld);
                            if (dist < minT) {
                                minT = dist;
                                hitIndex = (int)i;
                            }
                        }
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
        glm::mat4 proj = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, 0.1f, 400.0f);

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