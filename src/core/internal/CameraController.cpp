module;

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

module vortex.core; 

import :camera;
import vortex.log; // Залишаємо тільки для критичних повідомлень

namespace vortex::core {

    void CameraController::Update(GLFWwindow* window, graphics::Camera& camera, float deltaTime) {
        if (!window) return;

        // --- 1. Keyboard Movement (WASD) - Time Dependent ---
        float currentSpeed = movementSpeed * deltaTime;
        
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) currentSpeed *= sprintMultiplier;

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera.position += currentSpeed * camera.front;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera.position -= currentSpeed * camera.front;
        
        glm::vec3 right = glm::normalize(glm::cross(camera.front, camera.up));
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera.position -= currentSpeed * right;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera.position += currentSpeed * right;

        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) camera.position += currentSpeed * glm::vec3(0.0f, 1.0f, 0.0f);
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) camera.position -= currentSpeed * glm::vec3(0.0f, 1.0f, 0.0f);

        // --- 2. Mouse Rotation ---
        
        bool isLmbPressed = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool wantCapture = ImGui::GetIO().WantCaptureMouse;

        if (m_IsDragging) wantCapture = false; 

        if (isLmbPressed && !wantCapture) {
            
            if (!m_IsDragging) {
                m_IsDragging = true;
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

                // Use DOUBLE precision for positions
                double xpos, ypos;
                glfwGetCursorPos(window, &xpos, &ypos);
                // Cast to float ONLY for m_Last storage if you kept it float in header, 
                // BUT better to keep it double in logic.
                // Assuming m_LastX/Y are doubles in class (will fix header too) or local statics.
                // For now, let's use local statics to ensure precision if header is float.
                // Actually, let's just cast.
                m_LastX = (float)xpos;
                m_LastY = (float)ypos;
            }

            double xpos, ypos;
            glfwGetCursorPos(window, &xpos, &ypos);

            // Calculate delta in DOUBLE
            double xoffset = xpos - (double)m_LastX;
            double yoffset = (double)m_LastY - ypos; // Y inverted
            
            m_LastX = (float)xpos;
            m_LastY = (float)ypos;

            // Apply sensitivity
            // DO NOT multiply by deltaTime here! Mouse movement is frame-independent distance.
            xoffset *= (double)mouseSensitivity;
            yoffset *= (double)mouseSensitivity;

            camera.yaw += (float)xoffset;
            camera.pitch += (float)yoffset;

            if (camera.pitch > 89.0f) camera.pitch = 89.0f;
            if (camera.pitch < -89.0f) camera.pitch = -89.0f;

            glm::vec3 front;
            front.x = cos(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
            front.y = sin(glm::radians(camera.pitch));
            front.z = sin(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
            camera.front = glm::normalize(front);

        } else {
            if (m_IsDragging) {
                m_IsDragging = false;
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }
}