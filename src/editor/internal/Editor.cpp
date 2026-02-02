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
#include <algorithm>
#include <memory>
#include <cmath>

module vortex.editor;

import vortex.log;
import vortex.graphics;
import vortex.voxel;

namespace vortex::editor {

    struct Ray { glm::vec3 origin; glm::vec3 direction; };

    // --- Physics Helpers ---

    // Improved AABB intersection returning entry and exit points
    static bool IntersectRayAABB(const Ray& ray, const glm::vec3& min, const glm::vec3& max, float& tNear, float& tFar) {
        // Safety check: if AABB is invalid (min > max), which happens for empty objects, return no hit.
        if (min.x > max.x || min.y > max.y || min.z > max.z) return false;

        glm::vec3 invDir = 1.0f / ray.direction;
        glm::vec3 tbot = invDir * (min - ray.origin);
        glm::vec3 ttop = invDir * (max - ray.origin);
        glm::vec3 tmin = glm::min(ttop, tbot);
        glm::vec3 tmax = glm::max(ttop, tbot);
        tNear = glm::max(glm::max(tmin.x, tmin.y), tmin.z);
        tFar = glm::min(glm::min(tmax.x, tmax.y), tmax.z);
        return tFar >= tNear && tFar > 0.0f;
    }

    struct RaycastResult {
        bool hit;
        float t;
        glm::ivec3 voxelPos; // The solid voxel that was hit
        glm::ivec3 normal;   // The face normal (for placing blocks adjacent)
    };

    /**
     * @brief Performs DDA raycasting through a chunk.
     * @return Result containing hit position and face normal.
     */
    static RaycastResult RaycastDDA(const Ray& ray, const vortex::voxel::Chunk& chunk) {
        float tNear, tFar;
        if (!IntersectRayAABB(ray, glm::vec3(0.0f), glm::vec3(32.0f), tNear, tFar)) return {false, 0.0f};

        float tStart = std::max(0.0f, tNear);
        
        // Offset slightly inside to handle boundary conditions
        glm::vec3 rayPos = ray.origin + ray.direction * (tStart + 0.001f);
        glm::ivec3 mapPos = glm::ivec3(glm::floor(rayPos));
        mapPos = glm::clamp(mapPos, glm::ivec3(0), glm::ivec3(31));

        glm::vec3 safeDir = ray.direction;
        if (std::abs(safeDir.x) < 1e-6) safeDir.x = 1e-6f;
        if (std::abs(safeDir.y) < 1e-6) safeDir.y = 1e-6f;
        if (std::abs(safeDir.z) < 1e-6) safeDir.z = 1e-6f;

        glm::vec3 deltaDist = glm::abs(1.0f / safeDir);
        glm::ivec3 stepDir = glm::ivec3(glm::sign(safeDir));
        glm::vec3 sideDist;
        glm::ivec3 mask(0); // Tracks which axis we stepped on last

        glm::vec3 origin = ray.origin + ray.direction * tStart;
        for (int i = 0; i < 3; i++) {
            if (safeDir[i] < 0) {
                stepDir[i] = -1;
                sideDist[i] = (origin[i] - mapPos[i]) * deltaDist[i];
            } else {
                stepDir[i] = 1;
                sideDist[i] = (mapPos[i] + 1.0f - origin[i]) * deltaDist[i];
            }
        }

        for (int i = 0; i < 128; i++) {
            if (chunk.GetVoxel(mapPos.x, mapPos.y, mapPos.z) != 0) {
                glm::ivec3 normal(0);
                float dist = 0.0f;

                if (mask.x != 0 || mask.y != 0 || mask.z != 0) {
                    if (mask.x) { normal.x = -stepDir.x; dist = sideDist.x - deltaDist.x; }
                    else if (mask.y) { normal.y = -stepDir.y; dist = sideDist.y - deltaDist.y; }
                    else { normal.z = -stepDir.z; dist = sideDist.z - deltaDist.z; }
                } else {
                     // We hit the very first voxel (on the boundary or inside)
                     // Approximate normal based on ray direction if we didn't step yet
                     glm::vec3 absDir = glm::abs(ray.direction);
                     if (absDir.x > absDir.y && absDir.x > absDir.z) normal.x = (ray.direction.x > 0) ? -1 : 1;
                     else if (absDir.y > absDir.z) normal.y = (ray.direction.y > 0) ? -1 : 1;
                     else normal.z = (ray.direction.z > 0) ? -1 : 1;
                }
                
                return {true, tStart + dist, mapPos, normal};
            }

            mask = glm::ivec3(0);
            if (sideDist.x < sideDist.y) {
                if (sideDist.x < sideDist.z) {
                    sideDist.x += deltaDist.x;
                    mapPos.x += stepDir.x;
                    mask.x = 1;
                } else {
                    sideDist.z += deltaDist.z;
                    mapPos.z += stepDir.z;
                    mask.z = 1;
                }
            } else {
                if (sideDist.y < sideDist.z) {
                    sideDist.y += deltaDist.y;
                    mapPos.y += stepDir.y;
                    mask.y = 1;
                } else {
                    sideDist.z += deltaDist.z;
                    mapPos.z += stepDir.z;
                    mask.z = 1;
                }
            }
            
            if (mapPos.x < 0 || mapPos.x >= 32 || mapPos.y < 0 || mapPos.y >= 32 || mapPos.z < 0 || mapPos.z >= 32) break;
        }
        return {false, 0.0f};
    }

    // --- Editor Implementation ---

    void Editor::Update(GLFWwindow* window, graphics::Camera& camera, graphics::SceneManager& sceneManager, uint32_t width, uint32_t height) {
        m_SceneManager = &sceneManager;
        
        // Gizmo shortcuts only in Select mode
        if (m_CurrentTool == ToolMode::Select) {
            if (glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS) m_CurrentOperation = ImGuizmo::TRANSLATE;
            if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) m_CurrentOperation = ImGuizmo::ROTATE;
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) m_CurrentOperation = ImGuizmo::SCALE;
        }
        
        HandleInput(window, camera, width, height);
        RenderUI();
    }

    void Editor::RenderUI() {
        if (ImGui::Begin("Tools")) {
            
            // Tool Selection
            if (ImGui::RadioButton("Select", m_CurrentTool == ToolMode::Select)) {
                m_CurrentTool = ToolMode::Select;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Brush", m_CurrentTool == ToolMode::Brush)) {
                m_CurrentTool = ToolMode::Brush;
            }

            ImGui::Separator();

            if (m_CurrentTool == ToolMode::Brush) {
                ImGui::TextDisabled("Brush Settings");
                
                // Material Selector: 0 is Eraser
                if (ImGui::SliderInt("Material (0=Eraser)", &m_BrushMaterialID, 0, 255)) {
                    // Logic to preview material could go here
                }
                
                ImGui::SliderInt("Size", &m_BrushSize, 0, 5);
                ImGui::Checkbox("Spherical Shape", &m_BrushIsSphere);

                if (m_BrushMaterialID == 0) {
                     ImGui::TextColored(ImVec4(1, 0.5, 0.5, 1), "Mode: Erase");
                } else {
                     ImGui::TextColored(ImVec4(0.5, 1, 0.5, 1), "Mode: Add");
                }
            }

            ImGui::Separator();

            if (ImGui::CollapsingHeader("Importer")) {
                ImGui::InputText("Path", m_ImportPathBuffer, sizeof(m_ImportPathBuffer));
                ImGui::DragFloat("Scale", &m_ImportScale, 0.1f, 0.1f, 100.0f);
                
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
            ImGui::Text("Scene Objects (%zu):", m_Entities.size());
            
            for (size_t i = 0; i < m_Entities.size(); ++i) {
                RenderEntityNode((int)i, m_Entities[i]);
            }
        }
        ImGui::End();
    }

    std::shared_ptr<voxel::VoxelEntity> Editor::GetSelectedEntity() const {
        if (m_SelectedObjectID >= 0 && m_SelectedObjectID < (int)m_Entities.size()) {
            return m_Entities[m_SelectedObjectID];
        }
        return nullptr;
    }

    void Editor::RenderEntityNode(int index, std::shared_ptr<voxel::VoxelEntity> entity) {
        ImGui::PushID(index);
        bool isSelected = (m_SelectedObjectID == index);
        
        if (ImGui::Selectable(entity->name.c_str(), isSelected)) {
            m_SelectedObjectID = (m_SelectedObjectID != index) ? index : -1;
        }

        if (isSelected) {
            ImGui::Indent();
            ImGui::TextDisabled("Properties");
            
            ImGui::Checkbox("Static", &entity->isStatic);
            ImGui::Checkbox("Trigger (Sensor)", &entity->isTrigger);
            
            auto dynMesh = std::dynamic_pointer_cast<voxel::DynamicMeshObject>(entity);
            if (dynMesh) {
                if (ImGui::Button("Re-Mesh")) {
                    dynMesh->Remesh();
                    m_SceneDirty = true; 
                    entity->shouldRebuildPhysics = true; 
                }
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    void Editor::HandleInput(GLFWwindow* window, graphics::Camera& camera, uint32_t width, uint32_t height) {
        auto& io = ImGui::GetIO();
        if (io.WantCaptureMouse || ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return;

        bool isPressed = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        // Perform action on click (Brush allows holding, Select is click-once)
        bool shouldAct = false;
        if (m_CurrentTool == ToolMode::Select) {
             shouldAct = isPressed && !m_LeftMouseWasPressed;
        } else {
             // For brush, we might want continuous painting, but for now lets stick to single click to prevent mess
             shouldAct = isPressed && !m_LeftMouseWasPressed; 
        }

        if (shouldAct) {
            double mouseX, mouseY;
            glfwGetCursorPos(window, &mouseX, &mouseY);
            
            glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, 0.1f, 400.0f);
            proj[1][1] *= -1; 
            
            float x = (2.0f * (float)mouseX) / width - 1.0f;
            float y = (2.0f * (float)mouseY) / height - 1.0f;

            glm::vec4 rayClip = glm::vec4(x, y, 1.0, 1.0);
            glm::vec4 rayEye = glm::inverse(proj) * rayClip;
            rayEye = glm::vec4(rayEye.x, rayEye.y, rayEye.z, 0.0);
            glm::vec3 rayWor = glm::normalize(glm::vec3(glm::inverse(view) * rayEye));

            if (m_CurrentTool == ToolMode::Select) {
                HandleSelection(camera.position, rayWor);
            } else {
                HandleBrushAction(camera.position, rayWor);
            }
        }
        m_LeftMouseWasPressed = isPressed;
    }

    void Editor::HandleSelection(const glm::vec3& rayOrigin, const glm::vec3& rayDir) {
        float minT = std::numeric_limits<float>::max();
        int hitIndex = -1;
        Ray ray{rayOrigin, rayDir};

        for (size_t i = 0; i < m_Entities.size(); ++i) {
            auto& entity = m_Entities[i];
            glm::mat4 model = entity->transform;
            glm::mat4 invModel = glm::inverse(model);

            // Ray to Entity Local Space
            glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(ray.origin, 1.0f));
            glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(ray.direction, 0.0f)));
            Ray localRay{ localOrigin, localDir };

            float tBroadNear, tBroadFar;
            if (IntersectRayAABB(localRay, entity->localBoundsMin, entity->localBoundsMax, tBroadNear, tBroadFar)) {
                for (const auto& part : entity->parts) {
                    if (!part->chunk) continue;
                    glm::mat4 partModel = part->GetTransformMatrix();
                    glm::mat4 partInv = glm::inverse(partModel);
                    
                    glm::vec3 partOrigin = glm::vec3(partInv * glm::vec4(localOrigin, 1.0f));
                    glm::vec3 partDir = glm::normalize(glm::vec3(partInv * glm::vec4(localDir, 0.0f)));
                    Ray partRay{ partOrigin, partDir };

                    auto result = RaycastDDA(partRay, *part->chunk);
                    if (result.hit) {
                        // Calc world distance for sorting
                        glm::vec3 hitPointChunk = partOrigin + partDir * result.t;
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
            m_SelectedObjectID = (m_SelectedObjectID != hitIndex) ? hitIndex : -1;
            if (m_SelectedObjectID != -1) Log::Info("Selected Entity ID: " + std::to_string(m_SelectedObjectID));
        }
    }

    void Editor::HandleBrushAction(const glm::vec3& rayOrigin, const glm::vec3& rayDir) {
        // If nothing selected, try to find what we are looking at to paint on it.
        // For simplicity, we just raycast against ALL entities and paint on the closest one.
        
        float minT = std::numeric_limits<float>::max();
        std::shared_ptr<voxel::VoxelObject> targetPart = nullptr;
        glm::ivec3 targetVoxelPos;
        glm::ivec3 targetNormal;
        std::shared_ptr<voxel::VoxelEntity> targetEntity = nullptr;

        Ray ray{rayOrigin, rayDir};

        for (auto& entity : m_Entities) {
            // Optimization: If painting, we probably want to edit selected object or ANY object.
            // Let's stick to Raycasting logic similar to selection
            glm::mat4 model = entity->transform;
            glm::mat4 invModel = glm::inverse(model);

            glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(ray.origin, 1.0f));
            glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(ray.direction, 0.0f)));
            Ray localRay{ localOrigin, localDir };

            float tBroadNear, tBroadFar;
            if (IntersectRayAABB(localRay, entity->localBoundsMin, entity->localBoundsMax, tBroadNear, tBroadFar)) {
                for (auto& part : entity->parts) {
                    if (!part->chunk) continue;
                    glm::mat4 partModel = part->GetTransformMatrix();
                    glm::mat4 partInv = glm::inverse(partModel);
                    
                    glm::vec3 partOrigin = glm::vec3(partInv * glm::vec4(localOrigin, 1.0f));
                    glm::vec3 partDir = glm::normalize(glm::vec3(partInv * glm::vec4(localDir, 0.0f)));
                    Ray partRay{ partOrigin, partDir };

                    auto result = RaycastDDA(partRay, *part->chunk);
                    if (result.hit) {
                        glm::vec3 hitPointChunk = partOrigin + partDir * result.t;
                        glm::vec3 hitPointEntity = glm::vec3(partModel * glm::vec4(hitPointChunk, 1.0f));
                        glm::vec3 hitPointWorld = glm::vec3(model * glm::vec4(hitPointEntity, 1.0f));
                        float dist = glm::distance(ray.origin, hitPointWorld);

                        if (dist < minT) {
                            minT = dist;
                            targetPart = part;
                            targetVoxelPos = result.voxelPos;
                            targetNormal = result.normal;
                            targetEntity = entity;
                        }
                    }
                }
            }
        }

        if (targetPart && targetPart->chunk) {
            glm::ivec3 centerPos;
            
            if (m_BrushMaterialID == 0) {
                // Eraser: Target the voxel we hit
                centerPos = targetVoxelPos;
            } else {
                // Builder: Target the neighbor voxel
                centerPos = targetVoxelPos + targetNormal;
            }

            // --- Apply ShapeBuilder ---
            // Calculate bounds based on brush size
            
            if (m_BrushIsSphere) {
                // Sphere
                vortex::voxel::ShapeBuilder::CreateSphere(
                    *targetPart->chunk, 
                    targetPart->logicalCenter, 
                    targetPart->voxelCount, 
                    glm::vec3(centerPos), 
                    (float)m_BrushSize + 0.5f, 
                    (uint8_t)m_BrushMaterialID
                );
            } else {
                // Box
                glm::ivec3 minB = centerPos - glm::ivec3(m_BrushSize);
                glm::ivec3 maxB = centerPos + glm::ivec3(m_BrushSize + 1); // +1 because max is exclusive
                
                vortex::voxel::ShapeBuilder::CreateBox(
                    *targetPart->chunk,
                    targetPart->logicalCenter,
                    targetPart->voxelCount,
                    minB,
                    maxB,
                    (uint8_t)m_BrushMaterialID
                );
            }

            // Mark for update (Scene Refresh)
            m_SceneDirty = true;
            
            if (targetEntity) {
                targetEntity->RecalculateStats(); // Update entity wide bounds
                
                if (m_BrushMaterialID == 0) {
                    // Eraser Mode:
                    // Enable connectivity check to handle potential splits or destruction.
                    // IMPORTANT: Do NOT set shouldRebuildPhysics = true here!
                    // If the object is fully pulverized, the SHRED system will destroy it.
                    // If we set rebuild=true here, the Engine will try to rebuild physics for a dying object,
                    // causing a Double-Free/Segfault when Jolt tries to remove the same body twice.
                    // If the object survives, SHRED will automatically flag it for physics rebuild.
                    targetEntity->shouldCheckConnectivity = true; 
                    targetEntity->shouldRebuildPhysics = false;
                } else {
                    // Builder Mode:
                    // Generally safe to just rebuild physics as adding mass doesn't disconnect parts.
                    targetEntity->shouldRebuildPhysics = true; 
                }

                // Safety: If entity became empty, deselect it immediately to prevent Gizmo/UI crashes
                if (targetEntity->totalVoxelCount == 0) {
                     if (m_SelectedObjectID != -1 && m_SelectedObjectID < (int)m_Entities.size()) {
                         if (m_Entities[m_SelectedObjectID] == targetEntity) {
                             m_SelectedObjectID = -1;
                         }
                     }
                }
            }
        }
    }

    void Editor::RenderGizmo(graphics::Camera& camera, uint32_t width, uint32_t height) {
        if (m_CurrentTool != ToolMode::Select) return; // Only show gizmo in Select Mode
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
             // We don't rebuild physics continuously while dragging to save perf, usually done on mouse release
             // But for now, user might have to toggle static to refresh if colliders desync visually.
             // Ideally: entity->shouldRebuildPhysics = true; (but check ImGuizmo::IsOver or similar to do it only on end)
        }
    }
}