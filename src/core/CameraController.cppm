module;

#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

export module vortex.core:camera;

import vortex.graphics; // For accessing struct Camera

namespace vortex::core {

    /**
     * @brief Handles First-Person camera movement and rotation based on input.
     * @details Supports WASD movement, Space/Ctrl for elevation, and mouse look.
     */
    export class CameraController {
    public:
        CameraController() = default;

        /**
         * @brief Updates camera position and rotation based on current input state.
         * @param window Raw GLFW window handle for input polling.
         * @param camera Reference to the camera object to modify.
         * @param deltaTime Time elapsed since last frame (for frame-rate independent movement).
         */
        void Update(GLFWwindow* window, graphics::Camera& camera, float deltaTime);

        // --- Settings ---
        float movementSpeed = 5.0f;     ///< Base movement speed in units per second.
        float mouseSensitivity = 0.1f;  ///< Mouse look sensitivity.
        float sprintMultiplier = 4.0f;  ///< Speed multiplier when Shift is held.

    private:
        // Input state
        bool m_IsDragging = false;
        bool m_FirstMouse = true;
        float m_LastX = 0.0f;
        float m_LastY = 0.0f;
    };
}