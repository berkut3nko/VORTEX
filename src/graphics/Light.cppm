module;

#include <glm/glm.hpp>

export module vortex.graphics:light;

namespace vortex::graphics {

    /**
     * @brief GPU-aligned structure for Global Directional Light (Sun).
     * @note Aligned to std140/std430 (16 bytes).
     */
    export struct DirectionalLight {
        glm::vec4 direction; // xyz = dir, w = intensity
        glm::vec4 color;     // rgb = color, a = ambient intensity
        
        // Helper to set direction from Euler angles (in degrees)
        void SetDirection(float pitch, float yaw) {
            float radPitch = glm::radians(pitch);
            float radYaw = glm::radians(yaw);
            direction.x = cos(radYaw) * cos(radPitch);
            direction.y = sin(radPitch);
            direction.z = sin(radYaw) * cos(radPitch);
            direction = glm::normalize(direction);
        }
    };
}