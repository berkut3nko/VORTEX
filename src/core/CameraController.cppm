module;

#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

export module vortex.core:camera;

import vortex.graphics; // Для доступу до struct Camera

namespace vortex::core {

    /**
     * @brief Handles camera movement and rotation based on input.
     */
    export class CameraController {
    public:
        CameraController() = default;

        /**
         * @brief Updates camera position and rotation.
         * @param window Raw GLFW window handle for input polling.
         * @param camera Reference to the camera object to modify.
         * @param deltaTime Time elapsed since last frame.
         */
        void Update(GLFWwindow* window, graphics::Camera& camera, float deltaTime);

        // Settings
        float movementSpeed = 5.0f;
        float mouseSensitivity = 0.1f;
        float sprintMultiplier = 4.0f;

    private:
        // Mouse state
        bool m_IsDragging = false;
        bool m_FirstMouse = true;
        float m_LastX = 0.0f;
        float m_LastY = 0.0f;
    };
}