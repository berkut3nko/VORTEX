module;

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>
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

    static bool IntersectRayAABB(const Ray& ray, const glm::vec3& min, const glm::vec3& max, float& t) {
        glm::vec3 invDir = 1.0f / ray.direction;
        glm::vec3 tbot = invDir * (min - ray.origin);
        glm::vec3 ttop = invDir * (max - ray.origin);
        glm::vec3 tmin = glm::min(ttop, tbot);
        glm::vec3 tmax = glm::max(ttop, tbot);
        float t0 = glm::max(glm::max(tmin.x, tmin.y), tmin.z);
        float t1 = glm::min(glm::min(tmax.x, tmax.y), tmax.z);
        
        // Check if intersection is valid and not behind the ray origin (unless we are inside)
        if (t1 >= t0) {
            // If t0 is negative (inside box), return t1? Or 0? 
            // Usually we return t0 if >= 0, else 0 if inside.
            // But here we just need to know IF we hit and where to start.
            // If t0 < 0, we are inside, so entry is 0.0f.
            float entry = (t0 < 0.0f) ? 0.0f : t0;
            if (t1 >= 0.0f) {
                t = entry;
                return true;
            }
        }
        return false;
    }

    // --- DDA Voxel Raycaster ---
    static bool IntersectRayVoxel(const Ray& rayWorld, const std::shared_ptr<voxel::VoxelEntity>& entity, float& outDist) {
        bool hasHit = false;
        float minHitDist = std::numeric_limits<float>::max();

        glm::mat4 invEntityTransform = glm::inverse(entity->transform);
        
        // Transform ray to Entity Local Space
        glm::vec3 originE = glm::vec3(invEntityTransform * glm::vec4(rayWorld.origin, 1.0f));
        glm::vec3 dirE = glm::vec3(invEntityTransform * glm::vec4(rayWorld.direction, 0.0f)); 

        for(const auto& part : entity->parts) {
            if(!part->chunk) continue;

            // Transform ray to Chunk Local Space (0..32 coordinates)
            glm::mat4 invPartMatrix = glm::inverse(part->GetTransformMatrix());
            glm::vec3 ro = glm::vec3(invPartMatrix * glm::vec4(originE, 1.0f));
            glm::vec3 rd = glm::vec3(invPartMatrix * glm::vec4(dirE, 0.0f));
            
            glm::vec3 rdNorm = glm::normalize(rd);
            
            // 1. Intersect Chunk AABB to find entry point
            float tEntry = 0.0f;
            Ray localRay = { ro, rdNorm };
            
            // Chunk is always 32x32x32 in local voxel space
            if(!IntersectRayAABB(localRay, glm::vec3(0.0f), glm::vec3(32.0f), tEntry)) {
                continue;
            }

            // Nudge entry point slightly inside to avoid precision issues at boundaries
            glm::vec3 startPos = ro + rdNorm * (tEntry + 0.001f);

            // 2. Initialize DDA
            // Clamp start position to be within grid indices
            int x = std::clamp((int)std::floor(startPos.x), 0, 31);
            int y = std::clamp((int)std::floor(startPos.y), 0, 31);
            int z = std::clamp((int)std::floor(startPos.z), 0, 31);

            int stepX = (rdNorm.x > 0) ? 1 : -1;
            int stepY = (rdNorm.y > 0) ? 1 : -1;
            int stepZ = (rdNorm.z > 0) ? 1 : -1;

            glm::vec3 tDelta = glm::abs(1.0f / rdNorm);
            glm::vec3 tMax;

            // Calculate distance to next voxel boundary
            if (rdNorm.x > 0) tMax.x = (std::floor(startPos.x) + 1.0f - startPos.x) * tDelta.x;
            else              tMax.x = (startPos.x - std::floor(startPos.x)) * tDelta.x;

            if (rdNorm.y > 0) tMax.y = (std::floor(startPos.y) + 1.0f - startPos.y) * tDelta.y;
            else              tMax.y = (startPos.y - std::floor(startPos.y)) * tDelta.y;

            if (rdNorm.z > 0) tMax.z = (std::floor(startPos.z) + 1.0f - startPos.z) * tDelta.z;
            else              tMax.z = (startPos.z - std::floor(startPos.z)) * tDelta.z;

            // Prevent infinite loops if ray is parallel to axis (tMax becomes Inf)
            // Logic below handles Inf correctly (condition < Inf is true)

            // 3. Traverse Grid
            // Max steps = 32 * 3 (diagonal traversal)
            for(int i=0; i<100; ++i) {
                // Check Voxel
                if (x >= 0 && x < 32 && y >= 0 && y < 32 && z >= 0 && z < 32) {
                    if (part->chunk->GetVoxel(x, y, z) != 0) {
                        // HIT!
                        // Calculate precise world distance to this voxel
                        // We use the AABB of the single voxel to get exact t
                        float tVoxel = 0.0f;
                        if(IntersectRayAABB(localRay, glm::vec3(x,y,z), glm::vec3(x+1,y+1,z+1), tVoxel)) {
                            glm::vec3 hitPointLocal = ro + rdNorm * tVoxel;
                            glm::vec4 hitPointWorld = entity->transform * part->GetTransformMatrix() * glm::vec4(hitPointLocal, 1.0f);
                            float dist = glm::distance(rayWorld.origin, glm::vec3(hitPointWorld));
                            
                            if (dist < minHitDist) {
                                minHitDist = dist;
                                hasHit = true;
                            }
                        }
                        break; // Found the first hit in this chunk, move to next part (if any overlap)
                    }
                } else {
                    break; // Left the chunk
                }

                // Step to next voxel
                if (tMax.x < tMax.y) {
                    if (tMax.x < tMax.z) {
                        x += stepX;
                        tMax.x += tDelta.x;
                    } else {
                        z += stepZ;
                        tMax.z += tDelta.z;
                    }
                } else {
                    if (tMax.y < tMax.z) {
                        y += stepY;
                        tMax.y += tDelta.y;
                    } else {
                        z += stepZ;
                        tMax.z += tDelta.z;
                    }
                }
            }
        }

        if (hasHit) {
            outDist = minHitDist;
            return true;
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
                    // 1. Re-generate Voxels from Mesh
                    dynMesh->Remesh();
                    
                    // 2. Mark Scene Dirty (Update Graphics Buffer)
                    m_SceneDirty = true; 
                    
                    // 3. Mark Physics Dirty (Engine will regenerate Jolt Body)
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
                
                // --- Broadphase Check (AABB) ---
                // We transform ray to local space to check against localBounds
                glm::mat4 model = entity->transform;
                glm::mat4 invModel = glm::inverse(model);

                glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(ray.origin, 1.0f));
                glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(ray.direction, 0.0f)));
                Ray localRay{ localOrigin, localDir };

                float tAABB = 0.0f;
                if (!IntersectRayAABB(localRay, entity->localBoundsMin, entity->localBoundsMax, tAABB)) {
                    continue; // Skipped by broadphase
                }

                // --- Narrowphase Check (DDA Voxel) ---
                // If AABB hit, check actual voxels for precision
                float dist = 0.0f;
                if (IntersectRayVoxel(ray, entity, dist)) {
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