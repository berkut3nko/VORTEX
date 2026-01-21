module;

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

module vortex.core; 

import :camera;
import vortex.log; // Залишаємо тільки для критичних повідомлень, якщо треба

namespace vortex::core {

    void CameraController::Update(GLFWwindow* window, graphics::Camera& camera, float deltaTime) {
        if (!window) return;

        // --- 1. Keyboard Movement (WASD) ---
        float currentSpeed = movementSpeed * deltaTime;
        
        // Acceleration
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
            currentSpeed *= sprintMultiplier;
        }

        // Forward/Backward
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) 
            camera.position += currentSpeed * camera.front;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) 
            camera.position -= currentSpeed * camera.front;
        
        // Strafe Left/Right
        glm::vec3 right = glm::normalize(glm::cross(camera.front, camera.up));
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) 
            camera.position -= currentSpeed * right;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) 
            camera.position += currentSpeed * right;

        // Up/Down (Global Y)
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) 
            camera.position += currentSpeed * glm::vec3(0.0f, 1.0f, 0.0f);
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) 
            camera.position -= currentSpeed * glm::vec3(0.0f, 1.0f, 0.0f);

        // --- 2. Mouse Rotation (Drag with LMB) ---
        
        // Check inputs
        bool isLmbPressed = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool wantCapture = ImGui::GetIO().WantCaptureMouse;

        // If we are ALREADY dragging, we ignore ImGui's wantCapture.
        // This prevents the camera from stopping if the mouse accidentally hovers over a UI element while turning.
        if (m_IsDragging) {
            wantCapture = false; 
        }

        // Logic: Rotate ONLY if LMB is pressed AND (we are already dragging OR mouse is not over UI)
        if (isLmbPressed && !wantCapture) {
            
            // Start Dragging Event
            if (!m_IsDragging) {
                m_IsDragging = true;
                
                // Lock ImGui cursor handling so it doesn't fight us
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

                // Use NORMAL cursor mode for VM compatibility.
                // We will just read the delta. Cursor remains visible.
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

                // Reset last position to current to avoid a "jump" on the first frame
                double xpos, ypos;
                glfwGetCursorPos(window, &xpos, &ypos);
                m_LastX = (float)xpos;
                m_LastY = (float)ypos;
            }

            // Continuous Rotation Logic
            double xpos, ypos;
            glfwGetCursorPos(window, &xpos, &ypos);

            float xoffset = (float)xpos - m_LastX;
            float yoffset = m_LastY - (float)ypos; // Reversed Y (GLFW 0 is top)
            
            // Update last position for next frame
            m_LastX = (float)xpos;
            m_LastY = (float)ypos;

            // Apply sensitivity
            xoffset *= mouseSensitivity;
            yoffset *= mouseSensitivity;

            camera.yaw += xoffset;
            camera.pitch += yoffset;

            // Constrain Pitch (prevent camera flip)
            if (camera.pitch > 89.0f) camera.pitch = 89.0f;
            if (camera.pitch < -89.0f) camera.pitch = -89.0f;

            // Recalculate Vectors
            glm::vec3 front;
            front.x = cos(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
            front.y = sin(glm::radians(camera.pitch));
            front.z = sin(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
            camera.front = glm::normalize(front);

        } else {
            // Stop Dragging Event
            if (m_IsDragging) {
                m_IsDragging = false;
                
                // Release ImGui lock
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
                
                // Ensure cursor is normal (it should be already, but just in case)
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }
}