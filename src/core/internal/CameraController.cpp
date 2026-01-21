module;

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <string> 

module vortex.core; 

import :camera;
import vortex.log;

namespace vortex::core {

    // Статичні змінні для збереження стану між викликами
    static bool isCursorLocked = false;
    static bool lastTabState = false;

    void CameraController::Update(GLFWwindow* window, graphics::Camera& camera, float deltaTime) {
        if (!window) return;

        // --- 1. Обробка перемикання режимів (TAB) ---
        bool currentTabState = glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS;
        if (currentTabState && !lastTabState) {
            isCursorLocked = !isCursorLocked;
            
            if (isCursorLocked) {
                // Активація режиму камери
                Log::Info("Camera: LOCKED (Tracking Motion)");
                
                // Блокуємо ImGui, щоб він не змінював курсор
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
                
                // Використовуємо звичайний курсор, щоб VM не сходила з розуму
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

                // Скидаємо початкову позицію, щоб уникнути ривка при вході
                double xpos, ypos;
                glfwGetCursorPos(window, &xpos, &ypos);
                m_LastX = (float)xpos;
                m_LastY = (float)ypos;
                
            } else {
                // Деактивація (режим UI)
                Log::Info("Camera: UNLOCKED");
                
                // Дозволяємо ImGui керувати курсором
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
        lastTabState = currentTabState;

        // --- 2. Рух клавіатурою (WASD) ---
        float currentSpeed = movementSpeed * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) currentSpeed *= sprintMultiplier;

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera.position += currentSpeed * camera.front;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera.position -= currentSpeed * camera.front;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera.position -= currentSpeed * glm::normalize(glm::cross(camera.front, camera.up));
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera.position += currentSpeed * glm::normalize(glm::cross(camera.front, camera.up));
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) camera.position += currentSpeed * glm::vec3(0.0f, 1.0f, 0.0f);
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) camera.position -= currentSpeed * glm::vec3(0.0f, 1.0f, 0.0f);

        // --- 3. Обертання мишею (Simple Delta) ---
        if (isCursorLocked) {
            // Отримуємо поточну позицію
            double xpos, ypos;
            glfwGetCursorPos(window, &xpos, &ypos);

            // Рахуємо різницю від ПОПЕРЕДНЬОГО кадру
            float xoffset = (float)xpos - m_LastX;
            float yoffset = m_LastY - (float)ypos; // Інвертуємо Y (0 зверху)

            // Оновлюємо "останню" позицію для наступного кадру
            m_LastX = (float)xpos;
            m_LastY = (float)ypos;

            // Якщо був рух - застосовуємо
            if (xoffset != 0.0f || yoffset != 0.0f) {
                // Log::Info("Delta: " + std::to_string(xoffset) + " " + std::to_string(yoffset));

                xoffset *= mouseSensitivity;
                yoffset *= mouseSensitivity;

                camera.yaw += xoffset;
                camera.pitch += yoffset;

                // Обмеження вертикального кута
                if (camera.pitch > 89.0f) camera.pitch = 89.0f;
                if (camera.pitch < -89.0f) camera.pitch = -89.0f;

                // Перерахунок векторів
                glm::vec3 front;
                front.x = cos(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
                front.y = sin(glm::radians(camera.pitch));
                front.z = sin(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
                camera.front = glm::normalize(front);
            }
        }
    }
}